/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <api/proto_merge.h>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/message.h>

#include <algorithm>
#include <memory>

using google::protobuf::Descriptor;
using google::protobuf::FieldDescriptor;
using google::protobuf::Message;
using google::protobuf::OneofDescriptor;
using google::protobuf::Reflection;


namespace
{

std::unique_ptr<Message> newMessageFor( const std::string& aTypeUrl )
{
    std::string type;

    if( !google::protobuf::Any::ParseAnyTypeUrl( aTypeUrl, &type ) )
        return nullptr;

    const Descriptor* desc =
            google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName( type );

    if( !desc )
        return nullptr;

    const Message* proto =
            google::protobuf::MessageFactory::generated_factory()->GetPrototype( desc );

    return proto ? std::unique_ptr<Message>( proto->New() ) : nullptr;
}


std::string deterministic( const Message& aMessage )
{
    std::string out;
    {
        google::protobuf::io::StringOutputStream stream( &out );
        google::protobuf::io::CodedOutputStream  coded( &stream );
        coded.SetSerializationDeterministic( true );
        aMessage.SerializeToCodedStream( &coded );
    }
    return out;
}


/// A copy of aMessage with only the given fields kept.
std::unique_ptr<Message> only( const Message& aMessage,
                               const std::vector<const FieldDescriptor*>& aKeep )
{
    std::unique_ptr<Message> copy( aMessage.New() );
    copy->CopyFrom( aMessage );

    const Descriptor* desc = copy->GetDescriptor();
    const Reflection* refl = copy->GetReflection();

    for( int ii = 0; ii < desc->field_count(); ++ii )
    {
        const FieldDescriptor* f = desc->field( ii );

        if( std::find( aKeep.begin(), aKeep.end(), f ) == aKeep.end() )
            refl->ClearField( copy.get(), f );
    }

    return copy;
}


bool sameFields( const Message& aA, const Message& aB,
                 const std::vector<const FieldDescriptor*>& aFields )
{
    return deterministic( *only( aA, aFields ) ) == deterministic( *only( aB, aFields ) );
}


/// A message is merged as one value when it has no message fields (Vector2, Distance, KIID...).
bool atomic( const Descriptor* aDesc )
{
    for( int ii = 0; ii < aDesc->field_count(); ++ii )
    {
        if( aDesc->field( ii )->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE )
            return false;
    }

    return true;
}


void takeFields( Message* aOut, const Message& aFrom,
                 const std::vector<const FieldDescriptor*>& aFields )
{
    const Reflection* refl = aOut->GetReflection();

    for( const FieldDescriptor* f : aFields )
        refl->ClearField( aOut, f );

    aOut->MergeFrom( *only( aFrom, aFields ) );
}


void merge( const Message& aBase, const Message& aPerson, const Message& aAgent, Message* aOut,
            const std::string& aPath, PROTO_MERGE::RESULT& aResult, bool aAgentWins )
{
    const Descriptor* desc = aOut->GetDescriptor();
    const Reflection* refl = aOut->GetReflection();

    std::vector<const OneofDescriptor*> oneofsDone;

    for( int ii = 0; ii < desc->field_count(); ++ii )
    {
        const FieldDescriptor* f = desc->field( ii );
        std::vector<const FieldDescriptor*> group{ f };
        std::string path = aPath.empty() ? f->name() : aPath + "." + f->name();

        // A oneof is one value: which member is set, and its contents.
        if( const OneofDescriptor* oneof = f->real_containing_oneof() )
        {
            if( std::find( oneofsDone.begin(), oneofsDone.end(), oneof ) != oneofsDone.end() )
                continue;

            oneofsDone.push_back( oneof );
            group.clear();

            for( int jj = 0; jj < oneof->field_count(); ++jj )
                group.push_back( oneof->field( jj ) );

            path = aPath.empty() ? oneof->name() : aPath + "." + oneof->name();
        }

        bool agentChanged = !sameFields( aBase, aAgent, group );

        if( !agentChanged )
            continue;   // aOut already has the person's value

        // Both landed on the same value: agreement, not a conflict.
        if( sameFields( aPerson, aAgent, group ) )
            continue;

        bool personChanged = !sameFields( aBase, aPerson, group );

        if( !personChanged )
        {
            takeFields( aOut, aAgent, group );
            aResult.fromAgent.push_back( path );
            continue;
        }

        // Both changed this field.  Descend into composite messages, so disjoint edits of
        // the same item (the person moves it, the agent renames it) both survive.
        if( group.size() == 1 && !f->is_repeated()
            && f->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE
            && !atomic( f->message_type() )
            && refl->HasField( aBase, f ) && refl->HasField( aPerson, f )
            && refl->HasField( aAgent, f ) )
        {
            merge( refl->GetMessage( aBase, f ), refl->GetMessage( aPerson, f ),
                   refl->GetMessage( aAgent, f ), refl->MutableMessage( aOut, f ), path,
                   aResult, aAgentWins );
            continue;
        }

        // A real conflict: the person's edit wins, unless they chose the agent's.
        if( aAgentWins )
            takeFields( aOut, aAgent, group );

        aResult.keptFromPerson.push_back( path );
    }
}

} // namespace


namespace PROTO_MERGE
{

std::optional<std::string> ItemId( const google::protobuf::Any& aItem )
{
    std::unique_ptr<Message> msg = newMessageFor( aItem.type_url() );

    if( !msg || !aItem.UnpackTo( msg.get() ) )
        return std::nullopt;

    const Descriptor*      desc = msg->GetDescriptor();
    const FieldDescriptor* id = desc->FindFieldByName( "id" );

    if( !id || id->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE || id->is_repeated() )
        return std::nullopt;

    const Message&         kiid = msg->GetReflection()->GetMessage( *msg, id );
    const FieldDescriptor* value = kiid.GetDescriptor()->FindFieldByName( "value" );

    if( !value || value->cpp_type() != FieldDescriptor::CPPTYPE_STRING )
        return std::nullopt;

    std::string out = kiid.GetReflection()->GetString( kiid, value );

    if( out.empty() )
        return std::nullopt;

    return out;
}


std::optional<VECTOR2I> Position( const google::protobuf::Any& aItem )
{
    std::unique_ptr<Message> msg = newMessageFor( aItem.type_url() );

    if( !msg || !aItem.UnpackTo( msg.get() ) )
        return std::nullopt;

    const FieldDescriptor* f = msg->GetDescriptor()->FindFieldByName( "position" );

    if( !f || f->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE || f->is_repeated()
        || !msg->GetReflection()->HasField( *msg, f ) )
    {
        return std::nullopt;
    }

    const Message&         v = msg->GetReflection()->GetMessage( *msg, f );
    const FieldDescriptor* x = v.GetDescriptor()->FindFieldByName( "x_nm" );
    const FieldDescriptor* y = v.GetDescriptor()->FindFieldByName( "y_nm" );

    if( !x || !y )
        return std::nullopt;

    return VECTOR2I( (int) v.GetReflection()->GetInt64( v, x ), (int) v.GetReflection()->GetInt64( v, y ) );
}


std::optional<RESULT> ThreeWay( const google::protobuf::Any& aBase,
                                const google::protobuf::Any& aPerson,
                                const google::protobuf::Any& aAgent, bool aAgentWins )
{
    if( aBase.type_url() != aAgent.type_url() || aPerson.type_url() != aAgent.type_url() )
        return std::nullopt;

    std::unique_ptr<Message> base = newMessageFor( aAgent.type_url() );
    std::unique_ptr<Message> person = newMessageFor( aAgent.type_url() );
    std::unique_ptr<Message> agent = newMessageFor( aAgent.type_url() );

    if( !base || !person || !agent || !aBase.UnpackTo( base.get() )
        || !aPerson.UnpackTo( person.get() ) || !aAgent.UnpackTo( agent.get() ) )
    {
        return std::nullopt;
    }

    RESULT result;
    std::unique_ptr<Message> out( person->New() );
    out->CopyFrom( *person );

    merge( *base, *person, *agent, out.get(), "", result, aAgentWins );

    result.merged.PackFrom( *out );
    return result;
}

} // namespace PROTO_MERGE
