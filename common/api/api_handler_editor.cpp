/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2024 Jon Evans <jon@craftyjon.com>
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
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

#include <api/api_handler_editor.h>
#include <api/proto_merge.h>
#include <backplane_coedit.h>
#include <commit.h>
#include <nlohmann/json.hpp>
#include <wx/timer.h>
#include <set>
#include <wx/button.h>
#include <widgets/wx_infobar.h>
#include <undo_redo_container.h>

#include <api/api_enums.h>
#include <api/api_utils.h>
#include <eda_base_frame.h>
#include <eda_item.h>
#include <title_block.h>
#include <wx/wx.h>

using namespace kiapi::common::commands;


API_HANDLER_EDITOR::API_HANDLER_EDITOR( EDA_BASE_FRAME* aFrame ) :
        API_HANDLER(),
        m_frame( aFrame )
{
    registerHandler<BeginCommit, BeginCommitResponse>( &API_HANDLER_EDITOR::handleBeginCommit );
    registerHandler<kiapi::backplane::coedit::GetChanges,
                    kiapi::backplane::coedit::GetChangesResponse>(
            &API_HANDLER_EDITOR::handleGetChanges );
    registerHandler<kiapi::backplane::coedit::GetNotices,
                    kiapi::backplane::coedit::GetNoticesResponse>(
            &API_HANDLER_EDITOR::handleGetNotices );
    registerHandler<kiapi::backplane::coedit::SetAgentActivity,
                    kiapi::backplane::coedit::SetAgentActivityResponse>(
            &API_HANDLER_EDITOR::handleSetAgentActivity );
    registerHandler<kiapi::backplane::coedit::ResolveCollision,
                    kiapi::backplane::coedit::ResolveCollisionResponse>(
            &API_HANDLER_EDITOR::handleResolveCollision );
    registerHandler<kiapi::backplane::coedit::UndoAgentChange,
                    kiapi::backplane::coedit::UndoAgentChangeResponse>(
            &API_HANDLER_EDITOR::handleUndoAgentChange );
    registerHandler<kiapi::backplane::coedit::GetHolds,
                    kiapi::backplane::coedit::GetHoldsResponse>(
            &API_HANDLER_EDITOR::handleGetHolds );
    registerHandler<EndCommit, EndCommitResponse>( &API_HANDLER_EDITOR::handleEndCommit );
    registerHandler<CreateItems, CreateItemsResponse>( &API_HANDLER_EDITOR::handleCreateItems );
    registerHandler<UpdateItems, UpdateItemsResponse>( &API_HANDLER_EDITOR::handleUpdateItems );
    registerHandler<DeleteItems, DeleteItemsResponse>( &API_HANDLER_EDITOR::handleDeleteItems );
    registerHandler<HitTest, HitTestResponse>( &API_HANDLER_EDITOR::handleHitTest );
    registerHandler<GetDocumentModifiedState, GetDocumentModifiedStateResponse>(
            &API_HANDLER_EDITOR::handleGetDocumentModifiedState );
    registerHandler<GetTitleBlockInfo, types::TitleBlockInfo>( &API_HANDLER_EDITOR::handleGetTitleBlockInfo );
    registerHandler<SetTitleBlockInfo, google::protobuf::Empty>( &API_HANDLER_EDITOR::handleSetTitleBlockInfo );
}


HANDLER_RESULT<BeginCommitResponse> API_HANDLER_EDITOR::handleBeginCommit(
        const HANDLER_CONTEXT<BeginCommit>& aCtx )
{
    // Before 11.0, commit requests had no header so we assume they are for the PCB editor
    if( aCtx.Request.has_header() )
    {
        HANDLER_RESULT<std::optional<KIID>> valid = validateItemHeaderDocument( aCtx.Request.header() );

        if( !valid )
            return tl::unexpected( valid.error() );
    }

    if( std::optional<ApiResponseStatus> busy = checkForExclusive( currentHolds() ) )
        return tl::unexpected( *busy );

    if( m_commits.count( aCtx.ClientName ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "the client {} already has a commit in progress",
                                          aCtx.ClientName ) );
        return tl::unexpected( e );
    }

    wxASSERT( !m_activeClients.count( aCtx.ClientName ) );

    BeginCommitResponse response;

    KIID id;
    m_commits[aCtx.ClientName] = std::make_pair( id, createCommit() );
    response.mutable_id()->set_value( id.AsStdString() );

    m_activeClients.insert( aCtx.ClientName );

    return response;
}


HANDLER_RESULT<EndCommitResponse> API_HANDLER_EDITOR::handleEndCommit(
        const HANDLER_CONTEXT<EndCommit>& aCtx )
{
    // Before 11.0, commit requests had no header so we assume they are for the PCB editor
    if( aCtx.Request.has_header() )
    {
        HANDLER_RESULT<std::optional<KIID>> valid = validateItemHeaderDocument( aCtx.Request.header() );

        if( !valid )
            return tl::unexpected( valid.error() );
    }

    COEDIT::HOLDS holds = currentHolds();

    if( std::optional<ApiResponseStatus> busy = checkForExclusive( holds ) )
        return tl::unexpected( *busy );

    if( !m_commits.count( aCtx.ClientName ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "the client {} does not has a commit in progress",
                                          aCtx.ClientName ) );
        return tl::unexpected( e );
    }

    wxASSERT( m_activeClients.count( aCtx.ClientName ) );

    const std::pair<KIID, std::unique_ptr<COMMIT>>& pair = m_commits.at( aCtx.ClientName );
    const KIID& id = pair.first;
    const std::unique_ptr<COMMIT>& commit = pair.second;

    EndCommitResponse response;

    // Do not check IDs with drop; it is a safety net in case the id was lost on the client side
    switch( aCtx.Request.action() )
    {
    case kiapi::common::commands::CMA_DROP:
    {
        commit->Revert();
        m_commits.erase( aCtx.ClientName );
        m_activeClients.erase( aCtx.ClientName );
        break;
    }

    case kiapi::common::commands::CMA_COMMIT:
    {
        if( aCtx.Request.id().value().compare( id.AsStdString() ) != 0 )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "the id {} does not match the commit in progress",
                                              aCtx.Request.id().value() ) );
            return tl::unexpected( e );
        }

        // A removal staged before the person picked the item up (or started a tool) waits.
        // An update that replaces the object (a footprint: REMOVE old + ADD new, same KIID) is
        // an update: it waits only if the person holds the item, and then as a rebased update.
        if( holds.Active() )
        {
            std::vector<std::pair<EDA_ITEM*, int>> staged = commit->StagedItems();
            std::map<KIID, std::vector<EDA_ITEM*>>  added;
            std::set<EDA_ITEM*>                     removalsDone;

            for( const auto& [item, type] : staged )
            {
                if( item && ( type & CHT_TYPE ) == CHT_ADD )
                    added[item->m_Uuid].push_back( item );
            }

            for( const auto& [item, type] : staged )
            {
                if( !item || ( type & CHT_TYPE ) != CHT_REMOVE || removalsDone.count( item ) )
                    continue;

                removalsDone.insert( item );
                auto replacement = added.find( item->m_Uuid );

                if( replacement != added.end() && !replacement->second.empty() )
                {
                    if( !isHeld( holds, item ) && !holds.deferDeletes )
                        continue;

                    // The newest replacement is the agent's version; every one is unstaged.
                    DEFERRED_OP op;
                    op.client = aCtx.ClientName;
                    op.header.mutable_document()->CopyFrom( aCtx.Request.header().document() );
                    op.id = item->m_Uuid;
                    replacement->second.back()->Serialize( op.agent );
                    heldBase( item )->Serialize( op.base );
                    commit->UnstageRemoval( item, nullptr );

                    for( EDA_ITEM* added_item : replacement->second )
                        commit->Unstage( added_item, nullptr );   // never committed: freed

                    replacement->second.clear();
                    deferOp( std::move( op ) );
                }
                else if( holds.deferDeletes || isHeld( holds, item ) )
                {
                    DEFERRED_OP op;
                    op.client = aCtx.ClientName;
                    op.remove = true;
                    op.id = item->m_Uuid;
                    commit->UnstageRemoval( item, nullptr );
                    deferOp( std::move( op ) );
                }
            }
        }

        pushCurrentCommit( aCtx.ClientName, wxString( aCtx.Request.message().c_str(), wxConvUTF8 ) );
        break;
    }

    default:
        break;
    }

    return response;
}


COMMIT* API_HANDLER_EDITOR::getCurrentCommit( const std::string& aClientName )
{
    if( !m_commits.count( aClientName ) )
    {
        KIID id;
        m_commits[aClientName] = std::make_pair( id, createCommit() );
    }

    return m_commits.at( aClientName ).second.get();
}


void API_HANDLER_EDITOR::pushCurrentCommit( const std::string& aClientName,
                                            const wxString& aMessage )
{
    auto it = m_commits.find( aClientName );

    if( it == m_commits.end() )
        return;

    it->second.second->Push( aMessage.IsEmpty() ? m_defaultCommitMessage : aMessage );
    m_commits.erase( it );
    m_activeClients.erase( aClientName );
}


HANDLER_RESULT<bool> API_HANDLER_EDITOR::validateDocument( const DocumentSpecifier& aDocument )
{
    if( tl::expected<bool, ApiResponseStatus> validation = validateDocumentInternal( aDocument ); !validation )
        return tl::unexpected( validation.error() );

    return true;
}


HANDLER_RESULT<std::optional<KIID>> API_HANDLER_EDITOR::validateItemHeaderDocument(
        const types::ItemHeader& aHeader )
{
    if( !aHeader.has_document() || aHeader.document().type() != thisDocumentType() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        // No error message, this is a flag that the server should try a different handler
        return tl::unexpected( e );
    }

    HANDLER_RESULT<bool> documentValidation = validateDocument( aHeader.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( tl::expected<bool, ApiResponseStatus> result = validateDocumentInternal( aHeader.document() ); !result )
        return tl::unexpected( result.error() );

    if( aHeader.has_container() && !aHeader.container().value().empty() )
    {
        return KIID( aHeader.container().value() );
    }

    // Valid header, but no container provided
    return std::nullopt;
}


std::optional<ApiResponseStatus> API_HANDLER_EDITOR::checkForBusy()
{
    // Co-editing: reads are always answered, whatever the person is doing.
    if( !m_frame || COEDIT::READ_SCOPE::Active() )
        return std::nullopt;

    if( !m_frame->CanAcceptApiCommands() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BUSY );
        e.set_error_message( "KiCad is busy and cannot respond to API requests right now" );
        return e;
    }

    return std::nullopt;
}


HANDLER_RESULT<CreateItemsResponse> API_HANDLER_EDITOR::handleCreateItems(
        const HANDLER_CONTEXT<CreateItems>& aCtx )
{
    // New items cannot conflict with what the person is editing.
    if( std::optional<ApiResponseStatus> busy = checkForExclusive( currentHolds() ) )
        return tl::unexpected( *busy );

    CreateItemsResponse response;

    // The dedicated CreateItems.container field (when set) overrides any container in the header
    types::ItemHeader header;
    header.CopyFrom( aCtx.Request.header() );

    if( aCtx.Request.container().value().length() )
        *header.mutable_container() = aCtx.Request.container();

    HANDLER_RESULT<ItemRequestStatus> result = handleCreateUpdateItemsInternal( true,
            aCtx.ClientName,
            header, aCtx.Request.items(),
            [&]( const ItemStatus& aStatus, const google::protobuf::Any& aItem )
            {
                ItemCreationResult itemResult;
                itemResult.mutable_status()->CopyFrom( aStatus );
                itemResult.mutable_item()->CopyFrom( aItem );
                response.mutable_created_items()->Add( std::move( itemResult ) );
            } );

    if( !result.has_value() )
        return tl::unexpected( result.error() );

    response.set_status( *result );
    return response;
}


HANDLER_RESULT<UpdateItemsResponse> API_HANDLER_EDITOR::handleUpdateItems(
        const HANDLER_CONTEXT<UpdateItems>& aCtx )
{
    COEDIT::HOLDS holds = currentHolds();

    if( std::optional<ApiResponseStatus> busy = checkForExclusive( holds ) )
        return tl::unexpected( *busy );

    UpdateItemsResponse response;

    // Co-editing: an update of an item the person is editing right now waits for their edit to
    // end and is then rebased onto it.
    google::protobuf::RepeatedPtrField<google::protobuf::Any> now;
    std::vector<DEFERRED_OP>                                  later;

    if( holds.Active() && validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        for( const google::protobuf::Any& item : aCtx.Request.items() )
        {
            EDA_ITEM* live = nullptr;

            if( std::optional<std::string> id = PROTO_MERGE::ItemId( item ) )
                live = resolveLiveItem( KIID( *id ) );

            if( live && ( isHeld( holds, live )
                          || ( holds.deferDeletes && updateReplacesObject( live ) ) ) )
            {
                DEFERRED_OP op;
                op.client = aCtx.ClientName;
                op.header.CopyFrom( aCtx.Request.header() );
                op.id = live->m_Uuid;
                op.agent.CopyFrom( item );
                heldBase( live )->Serialize( op.base );
                later.push_back( std::move( op ) );
            }
            else
            {
                *now.Add() = item;
            }
        }
    }
    else
    {
        now = aCtx.Request.items();
    }

    // Co-editing: an update made from a read older than the person's last change of the item
    // merges from what the agent saw; a real collision waits for the person.
    if( m_frame && validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        for( google::protobuf::Any& item : now )
            item = rebaseStale( aCtx.ClientName, aCtx.Request.header(), item );
    }

    if( now.empty() && !later.empty() )
    {
        for( DEFERRED_OP& op : later )
        {
            ItemUpdateResult itemResult;
            itemResult.mutable_status()->set_code( ItemStatusCode::ISC_DEFERRED );
            itemResult.mutable_status()->set_error_message(
                    "the person is editing this item; your update is applied when they finish, "
                    "rebased onto their edit" );
            itemResult.mutable_item()->CopyFrom( op.agent );
            response.mutable_updated_items()->Add( std::move( itemResult ) );
            deferOp( std::move( op ) );
        }

        response.set_status( ItemRequestStatus::IRS_OK );
        return response;
    }

    HANDLER_RESULT<ItemRequestStatus> result = handleCreateUpdateItemsInternal( false,
            aCtx.ClientName,
            aCtx.Request.header(), now,
            [&]( const ItemStatus& aStatus, const google::protobuf::Any& aItem )
            {
                ItemUpdateResult itemResult;
                itemResult.mutable_status()->CopyFrom( aStatus );
                itemResult.mutable_item()->CopyFrom( aItem );
                response.mutable_updated_items()->Add( std::move( itemResult ) );
            } );

    if( !result.has_value() )
        return tl::unexpected( result.error() );

    for( DEFERRED_OP& op : later )
    {
        ItemUpdateResult itemResult;
        itemResult.mutable_status()->set_code( ItemStatusCode::ISC_DEFERRED );
        itemResult.mutable_status()->set_error_message(
                "the person is editing this item; your update is applied when they finish, "
                "rebased onto their edit" );
        itemResult.mutable_item()->CopyFrom( op.agent );
        response.mutable_updated_items()->Add( std::move( itemResult ) );
        deferOp( std::move( op ) );
    }

    response.set_status( *result );
    return response;
}


HANDLER_RESULT<DeleteItemsResponse> API_HANDLER_EDITOR::handleDeleteItems(
        const HANDLER_CONTEXT<DeleteItems>& aCtx )
{
    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    COEDIT::HOLDS holds = currentHolds();

    if( std::optional<ApiResponseStatus> busy = checkForExclusive( holds ) )
        return tl::unexpected( *busy );

    std::map<KIID, ItemDeletionStatus> itemsToDelete;

    for( const kiapi::common::types::KIID& kiidBuf : aCtx.Request.item_ids() )
    {
        if( !kiidBuf.value().empty() )
        {
            KIID kiid( kiidBuf.value() );
            itemsToDelete[kiid] = ItemDeletionStatus::IDS_NONEXISTENT;
        }
    }

    if( itemsToDelete.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "no valid items to delete were given" );
        return tl::unexpected( e );
    }

    // Co-editing: deletes of held items, and every delete while an interactive tool runs
    // (tools keep pointers to neighbouring items), wait for the hold to end.
    std::vector<KIID> deferred;

    if( holds.Active() )
    {
        for( auto it = itemsToDelete.begin(); it != itemsToDelete.end(); )
        {
            EDA_ITEM* live = resolveLiveItem( it->first );

            if( live && ( holds.deferDeletes || isHeld( holds, live ) ) )
            {
                DEFERRED_OP op;
                op.client = aCtx.ClientName;
                op.remove = true;
                op.header.CopyFrom( aCtx.Request.header() );
                op.id = it->first;
                deferOp( std::move( op ) );
                deferred.push_back( it->first );
                it = itemsToDelete.erase( it );
            }
            else
            {
                ++it;
            }
        }
    }

    if( !itemsToDelete.empty() )
        deleteItemsInternal( itemsToDelete, aCtx.ClientName );

    DeleteItemsResponse response;

    for( const auto& [id, status] : itemsToDelete )
    {
        ItemDeletionResult* result = response.add_deleted_items();
        result->mutable_id()->set_value( id.AsStdString() );
        result->set_status( status );
    }

    for( const KIID& id : deferred )
    {
        ItemDeletionResult* result = response.add_deleted_items();
        result->mutable_id()->set_value( id.AsStdString() );
        result->set_status( ItemDeletionStatus::IDS_DEFERRED );
    }

    response.set_status( kiapi::common::types::ItemRequestStatus::IRS_OK );
    return response;
}


HANDLER_RESULT<HitTestResponse> API_HANDLER_EDITOR::handleHitTest(
        const HANDLER_CONTEXT<HitTest>& aCtx )
{
    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HitTestResponse response;

    std::optional<EDA_ITEM*> item = getItemFromDocument( aCtx.Request.header().document(),
                                                         KIID( aCtx.Request.id().value() ) );

    if( !item )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "the requested item ID is not present in the given document" );
        return tl::unexpected( e );
    }

    const EDA_IU_SCALE& scale = getIuScale();
    VECTOR2I posIu = UnpackVector2( aCtx.Request.position(), scale );
    int toleranceIu = scale.NmToIU( aCtx.Request.tolerance() );

    if( ( *item )->HitTest( posIu, toleranceIu ) )
        response.set_result( HitTestResult::HTR_HIT );
    else
        response.set_result( HitTestResult::HTR_NO_HIT );

    return response;
}


HANDLER_RESULT<GetDocumentModifiedStateResponse>
API_HANDLER_EDITOR::handleGetDocumentModifiedState( const HANDLER_CONTEXT<GetDocumentModifiedState>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    GetDocumentModifiedStateResponse response;

    wxCHECK( m_frame, response );
    response.set_state( m_frame->IsContentModified() ? DocumentModifiedState::DMS_MODIFIED
                                                     : DocumentModifiedState::DMS_UNMODIFIED );
    return response;
}


std::vector<KICAD_T> API_HANDLER_EDITOR::parseRequestedItemTypes( const google::protobuf::RepeatedField<int>& aTypes )
{
    std::vector<KICAD_T> types;

    for( int typeRaw : aTypes )
    {
        auto typeMessage = static_cast<types::KiCadObjectType>( typeRaw );

        if( KICAD_T type = FromProtoEnum<KICAD_T>( typeMessage ); type != TYPE_NOT_INIT )
            types.emplace_back( type );
    }

    return types;
}


HANDLER_RESULT<types::TitleBlockInfo>
API_HANDLER_EDITOR::handleGetTitleBlockInfo( const HANDLER_CONTEXT<GetTitleBlockInfo>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::optional<TITLE_BLOCK*> optBlock = getTitleBlock( aCtx.Request.document() );

    if( !optBlock )
    {
        ApiResponseStatus e;
        e.set_status( AS_BAD_REQUEST );
        e.set_error_message( "this editor does not support a title block" );
        return tl::unexpected( e );
    }

    TITLE_BLOCK& block = **optBlock;

    types::TitleBlockInfo response;

    response.set_title( block.GetTitle().ToUTF8() );
    response.set_date( block.GetDate().ToUTF8() );
    response.set_revision( block.GetRevision().ToUTF8() );
    response.set_company( block.GetCompany().ToUTF8() );
    response.set_comment1( block.GetComment( 0 ).ToUTF8() );
    response.set_comment2( block.GetComment( 1 ).ToUTF8() );
    response.set_comment3( block.GetComment( 2 ).ToUTF8() );
    response.set_comment4( block.GetComment( 3 ).ToUTF8() );
    response.set_comment5( block.GetComment( 4 ).ToUTF8() );
    response.set_comment6( block.GetComment( 5 ).ToUTF8() );
    response.set_comment7( block.GetComment( 6 ).ToUTF8() );
    response.set_comment8( block.GetComment( 7 ).ToUTF8() );
    response.set_comment9( block.GetComment( 8 ).ToUTF8() );

    return response;
}


HANDLER_RESULT<google::protobuf::Empty>
API_HANDLER_EDITOR::handleSetTitleBlockInfo( const HANDLER_CONTEXT<SetTitleBlockInfo>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( !aCtx.Request.has_title_block() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "SetTitleBlockInfo requires title_block" );
        return tl::unexpected( e );
    }

    std::optional<TITLE_BLOCK*> optBlock = getTitleBlock( aCtx.Request.document() );

    if( !optBlock )
    {
        ApiResponseStatus e;
        e.set_status( AS_BAD_REQUEST );
        e.set_error_message( "this editor does not support a title block" );
        return tl::unexpected( e );
    }

    TITLE_BLOCK& block = **optBlock;

    const types::TitleBlockInfo& request = aCtx.Request.title_block();

    block.SetTitle( wxString::FromUTF8( request.title() ) );
    block.SetDate( wxString::FromUTF8( request.date() ) );
    block.SetRevision( wxString::FromUTF8( request.revision() ) );
    block.SetCompany( wxString::FromUTF8( request.company() ) );
    block.SetComment( 0, wxString::FromUTF8( request.comment1() ) );
    block.SetComment( 1, wxString::FromUTF8( request.comment2() ) );
    block.SetComment( 2, wxString::FromUTF8( request.comment3() ) );
    block.SetComment( 3, wxString::FromUTF8( request.comment4() ) );
    block.SetComment( 4, wxString::FromUTF8( request.comment5() ) );
    block.SetComment( 5, wxString::FromUTF8( request.comment6() ) );
    block.SetComment( 6, wxString::FromUTF8( request.comment7() ) );
    block.SetComment( 7, wxString::FromUTF8( request.comment8() ) );
    block.SetComment( 8, wxString::FromUTF8( request.comment9() ) );

    onModified();

    return google::protobuf::Empty();
}


HANDLER_RESULT<types::PageSettings> API_HANDLER_EDITOR::handleGetPageSettings(
        const HANDLER_CONTEXT<commands::GetPageSettings>& aCtx)
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::optional<PAGE_INFO> optPageInfo = getPageSettings( aCtx.Request.document() );

    if( !optPageInfo )
    {
        ApiResponseStatus e;
        e.set_status( AS_BAD_REQUEST );
        e.set_error_message( "this editor does not support page settings" );
        return tl::unexpected( e );
    }

    PAGE_INFO& pageInfo = *optPageInfo;

    types::PageSettings response;
    response.set_page_size( ToProtoEnum<PAGE_SIZE_TYPE, types::PageSize>( pageInfo.GetType() ) );

    if( pageInfo.IsCustom() )
        PackVector2( *response.mutable_user_page_size(), pageInfo.GetSizeIU( pcbIUScale.IU_PER_MILS ) );

    response.set_orientation( pageInfo.IsPortrait() ? types::PageOrientation::PO_PORTRAIT
                                                    : types::PageOrientation::PO_LANDSCAPE );
    response.set_drawing_sheet( getDrawingSheetFileName().ToUTF8() );

    return response;
}


HANDLER_RESULT<types::PageSettings> API_HANDLER_EDITOR::handleSetPageSettings(
        const HANDLER_CONTEXT<commands::SetPageSettings>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( !aCtx.Request.has_page_settings() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "SetPageSettings requires page_settings" );
        return tl::unexpected( e );
    }

    std::optional<PAGE_INFO> optPageInfo = getPageSettings( aCtx.Request.document() );

    if( !optPageInfo )
    {
        ApiResponseStatus e;
        e.set_status( AS_BAD_REQUEST );
        e.set_error_message( "this editor does not support page settings" );
        return tl::unexpected( e );
    }

    const types::PageSettings& request = aCtx.Request.page_settings();

    if( request.page_size() == types::PageSize::PS_UNKNOWN )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "SetPageSettings requires page_size" );
        return tl::unexpected( e );
    }

    PAGE_INFO pageInfo = *optPageInfo;
    PAGE_SIZE_TYPE pageSizeType = FromProtoEnum<PAGE_SIZE_TYPE>( request.page_size() );

    if( pageSizeType == PAGE_SIZE_TYPE::User )
    {
        if( !request.has_user_page_size() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "custom page size requires user_page_size" );
            return tl::unexpected( e );
        }

        VECTOR2D sizeIu = UnpackVector2( request.user_page_size() );
        PAGE_INFO::SetCustomWidthMils( pcbIUScale.IUToMils( sizeIu.x ) );
        PAGE_INFO::SetCustomHeightMils( pcbIUScale.IUToMils( sizeIu.y ) );

        pageInfo.SetType( PAGE_SIZE_TYPE::User );
    }
    else
    {
        bool portrait = ( request.orientation() == types::PageOrientation::PO_PORTRAIT );
        pageInfo.SetType( pageSizeType, portrait );
    }

    if( !setPageSettings( aCtx.Request.document(), pageInfo ) )
    {
        ApiResponseStatus e;
        e.set_status( AS_BAD_REQUEST );
        e.set_error_message( "this editor does not support page settings" );
        return tl::unexpected( e );
    }

    wxString drawingSheet( wxString::FromUTF8( request.drawing_sheet() ) );
    setDrawingSheetFileName( drawingSheet );

    onModified();

    types::PageSettings response;
    response.set_page_size( ToProtoEnum<PAGE_SIZE_TYPE, types::PageSize>( pageInfo.GetType() ) );

    if( pageInfo.IsCustom() )
        PackVector2( *response.mutable_user_page_size(), pageInfo.GetSizeIU( pcbIUScale.IU_PER_MILS ) );

    response.set_orientation( pageInfo.IsPortrait() ? types::PageOrientation::PO_PORTRAIT
                                                    : types::PageOrientation::PO_LANDSCAPE );
    response.set_drawing_sheet( getDrawingSheetFileName().ToUTF8() );

    return response;
}


namespace
{
class DEFER_TIMER : public wxTimer
{
public:
    explicit DEFER_TIMER( API_HANDLER_EDITOR* aHandler ) : m_handler( aHandler ) {}
    void Notify() override { m_handler->ProcessDeferred(); }

private:
    API_HANDLER_EDITOR* m_handler;
};
} // namespace


API_HANDLER_EDITOR::~API_HANDLER_EDITOR()
{
    if( m_deferTimer )
        m_deferTimer->Stop();
}


std::optional<ApiResponseStatus> API_HANDLER_EDITOR::checkForExclusive( const COEDIT::HOLDS& aHolds )
{
    if( !aHolds.exclusive )
        return std::nullopt;

    ApiResponseStatus e;
    e.set_status( ApiStatusCode::AS_BUSY );

    if( aHolds.reason.IsEmpty() )
        e.set_error_message( "KiCad is busy and cannot respond to API requests right now" );
    else
        e.set_error_message( fmt::format( "the person is in '{}', which rewrites the document; "
                                          "writes wait until it ends",
                                          aHolds.reason.ToStdString( wxConvUTF8 ) ) );

    return e;
}


COEDIT::HOLDS API_HANDLER_EDITOR::currentHolds()
{
    COEDIT::HOLDS holds;

    if( m_frame && !m_frame->CanAcceptApiCommands() )
        holds.exclusive = true;

    return holds;
}


bool API_HANDLER_EDITOR::isHeld( const COEDIT::HOLDS& aHolds, EDA_ITEM* aItem ) const
{
    if( aHolds.exclusive )
        return true;

    for( EDA_ITEM* item = aItem; item; item = item->GetParent() )
    {
        if( aHolds.items.count( item->m_Uuid ) )
            return true;
    }

    return false;
}


const EDA_ITEM* API_HANDLER_EDITOR::heldBase( EDA_ITEM* aItem ) const
{
    for( COMMIT* commit : COEDIT::LiveCommits() )
    {
        if( !commit->GetActor().empty() || commit->Empty() )
            continue;

        for( const auto& [item, copy] : commit->StagedOriginals() )
        {
            if( item == aItem && copy )
                return copy;
        }
    }

    return aItem;
}


void API_HANDLER_EDITOR::deferOp( DEFERRED_OP aOp )
{
    COEDIT::NOTICE notice;
    notice.kind = "deferred";
    notice.actor = aOp.client;
    notice.doc = coeditDocKind();
    notice.json = nlohmann::json( { { "op", aOp.remove ? "delete" : "update" },
                                    { "item", aOp.id.AsStdString() } } ).dump();
    COEDIT::PostNotice( notice );

    m_deferred.push_back( std::move( aOp ) );

    if( !m_deferTimer )
        m_deferTimer = std::make_unique<DEFER_TIMER>( this );

    if( !m_deferTimer->IsRunning() )
        m_deferTimer->Start( 100 );
}


void API_HANDLER_EDITOR::ProcessDeferred()
{
    if( m_deferred.empty() )
    {
        if( m_deferTimer )
            m_deferTimer->Stop();

        return;
    }

    // Never from inside a nested API request or a modal loop that refuses writes.
    if( COEDIT::AgentActing() )
        return;

    COEDIT::HOLDS holds = currentHolds();

    if( holds.exclusive )
        return;

    std::map<std::string, std::vector<DEFERRED_OP>> ready;
    std::vector<DEFERRED_OP>                        keep;
    std::vector<COEDIT::NOTICE>                     notices;

    for( DEFERRED_OP& op : m_deferred )
    {
        EDA_ITEM* live = resolveLiveItem( op.id );

        if( !live )
        {
            COEDIT::NOTICE notice;
            notice.kind = "dropped";
            notice.actor = op.client;
            notice.doc = coeditDocKind();
            notice.json = nlohmann::json( { { "op", op.remove ? "delete" : "update" },
                                            { "item", op.id.AsStdString() },
                                            { "reason", "the person deleted the item" } } ).dump();
            notices.push_back( notice );
            continue;
        }

        bool still = op.remove ? ( holds.deferDeletes || isHeld( holds, live ) )
                               : isHeld( holds, live );

        // An open API commit (this agent's or another's) has staged this item: applying now
        // would replace it under that commit, whose push would then add a second copy.  Wait
        // for the commit to end.
        for( const auto& [client, entry] : m_commits )
        {
            for( const auto& [item, type] : entry.second->StagedItems() )
            {
                if( item && item->m_Uuid == op.id )
                    still = true;
            }
        }

        if( still )
            keep.push_back( std::move( op ) );
        else
            ready[op.client].push_back( std::move( op ) );
    }

    m_deferred = std::move( keep );

    for( auto& [client, ops] : ready )
    {
        COEDIT::ACTOR_SCOPE  actor( client );
        COEDIT::REBASE_SCOPE rebasing;
        std::string          commitClient = client + "#rebased";

        m_activeClients.insert( commitClient );

        google::protobuf::RepeatedPtrField<google::protobuf::Any> items;
        std::map<KIID, ItemDeletionStatus>                        deletes;
        const types::ItemHeader*                                  header = nullptr;
        nlohmann::json                                            report = nlohmann::json::array();

        for( DEFERRED_OP& op : ops )
        {
            if( op.remove )
            {
                deletes[op.id] = ItemDeletionStatus::IDS_NONEXISTENT;
                report.push_back( { { "op", "delete" }, { "item", op.id.AsStdString() },
                                    { "outcome", "applied" } } );
                continue;
            }

            EDA_ITEM*             live = resolveLiveItem( op.id );
            google::protobuf::Any person;
            live->Serialize( person );

            nlohmann::json entry = { { "op", "update" }, { "item", op.id.AsStdString() } };

            if( std::optional<PROTO_MERGE::RESULT> merged =
                        PROTO_MERGE::ThreeWay( op.base, person, op.agent ) )
            {
                *items.Add() = merged->merged;
                entry["outcome"] = merged->keptFromPerson.empty() ? "rebased" : "collision";
                entry["from_agent"] = merged->fromAgent;
                entry["kept_from_person"] = merged->keptFromPerson;

                if( !merged->keptFromPerson.empty() )
                {
                    COLLISION c;
                    c.client = client;
                    c.header.CopyFrom( op.header );
                    c.id = op.id;
                    c.what = describeItem( live );
                    c.fields = merged->keptFromPerson;
                    c.base.CopyFrom( op.base );
                    c.agent.CopyFrom( op.agent );
                    c.personAt = wxGetUTCTimeMillis().GetValue();
                    raiseCollision( std::move( c ) );
                }
            }
            else
            {
                *items.Add() = op.agent;
                entry["outcome"] = "applied";
            }

            header = &op.header;
            report.push_back( entry );
        }

        if( header && !items.empty() )
        {
            HANDLER_RESULT<ItemRequestStatus> result = handleCreateUpdateItemsInternal(
                    false, commitClient, *header, items,
                    [&]( ItemStatus aStatus, google::protobuf::Any aItem )
                    {
                        if( aStatus.code() != ItemStatusCode::ISC_OK )
                        {
                            report.push_back( { { "op", "update" },
                                                { "item", PROTO_MERGE::ItemId( aItem ).value_or( "" ) },
                                                { "outcome", "failed" },
                                                { "error", aStatus.error_message() } } );
                        }
                    } );

            if( !result )
            {
                report.push_back( { { "op", "update" }, { "outcome", "failed" },
                                    { "error", result.error().error_message() } } );
            }
        }

        if( !deletes.empty() )
            deleteItemsInternal( deletes, commitClient );

        pushCurrentCommit( commitClient, _( "Agent edit, applied after yours" ) );
        m_activeClients.erase( commitClient );

        COEDIT::NOTICE notice;
        notice.kind = "rebased";
        notice.actor = client;
        notice.doc = coeditDocKind();
        notice.json = report.dump();
        notices.push_back( notice );
    }

    for( const COEDIT::NOTICE& notice : notices )
        COEDIT::PostNotice( notice );

    if( m_deferred.empty() && m_deferTimer )
        m_deferTimer->Stop();
}


HANDLER_RESULT<kiapi::backplane::coedit::GetChangesResponse> API_HANDLER_EDITOR::handleGetChanges(
        const HANDLER_CONTEXT<kiapi::backplane::coedit::GetChanges>& aCtx )
{
    kiapi::backplane::coedit::GetChangesResponse response;
    bool                                         lost = false;

    for( const COEDIT::CHANGE& c : COEDIT::ChangesSince( aCtx.Request.since(), aCtx.Request.doc(),
                                                         lost ) )
    {
        kiapi::backplane::coedit::Change* out = response.add_changes();
        out->set_seq( c.seq );
        out->set_doc( c.doc );
        out->set_actor( c.actor );
        out->set_description( c.description );

        for( const std::string& id : c.created )
            out->add_created( id );

        for( const std::string& id : c.modified )
            out->add_modified( id );

        for( const std::string& id : c.deleted )
            out->add_deleted( id );

        out->set_rebased( c.rebased );
        out->set_undo( c.undo );
    }

    response.set_last( COEDIT::LastSeq() );
    response.set_lost( lost );
    return response;
}


HANDLER_RESULT<kiapi::backplane::coedit::GetNoticesResponse> API_HANDLER_EDITOR::handleGetNotices(
        const HANDLER_CONTEXT<kiapi::backplane::coedit::GetNotices>& aCtx )
{
    kiapi::backplane::coedit::GetNoticesResponse response;
    std::string actor = aCtx.Request.actor().empty() ? aCtx.ClientName : aCtx.Request.actor();

    for( const COEDIT::NOTICE& n : COEDIT::NoticesFor( actor, aCtx.Request.clear() ) )
    {
        kiapi::backplane::coedit::Notice* out = response.add_notices();
        out->set_kind( n.kind );
        out->set_actor( n.actor );
        out->set_doc( n.doc );
        out->set_json( n.json );
    }

    return response;
}


HANDLER_RESULT<kiapi::backplane::coedit::GetHoldsResponse> API_HANDLER_EDITOR::handleGetHolds(
        const HANDLER_CONTEXT<kiapi::backplane::coedit::GetHolds>& aCtx )
{
    if( aCtx.Request.document().type() != thisDocumentType() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    HANDLER_RESULT<bool> valid = validateDocument( aCtx.Request.document() );

    if( !valid )
        return tl::unexpected( valid.error() );

    COEDIT::HOLDS holds = currentHolds();
    kiapi::backplane::coedit::GetHoldsResponse response;
    response.set_exclusive( holds.exclusive );
    response.set_reason( holds.reason.ToStdString( wxConvUTF8 ) );
    response.set_defer_deletes( holds.deferDeletes );
    response.set_all_copper( holds.allCopper );

    for( const KIID& id : holds.items )
        response.add_items( id.AsStdString() );

    response.set_deferred( (uint32_t) m_deferred.size() );
    response.set_collisions( (uint32_t) m_collisions.size() );

    if( !m_collisions.empty() )
        response.set_collision( collisionText( m_collisions.front() ).ToStdString( wxConvUTF8 ) );

    return response;
}


const EDA_ITEM* API_HANDLER_EDITOR::personBaseSince( EDA_ITEM* aLive, uint64_t aSince ) const
{
    if( !m_frame )
        return nullptr;

    for( const PICKED_ITEMS_LIST* list : m_frame->GetUndoListForCoedit().m_CommandsList )
    {
        if( list->IsAgent() || list->GetSeq() <= aSince )
            continue;

        for( unsigned ii = 0; ii < list->GetCount(); ++ii )
        {
            const EDA_ITEM* picked = list->GetPickedItem( ii );

            // By KIID: an update may have replaced the object since (a footprint is).
            if( picked && picked->m_Uuid == aLive->m_Uuid
                && list->GetPickedItemStatus( ii ) == UNDO_REDO::CHANGED
                && list->GetPickedItemLink( ii ) )
            {
                return list->GetPickedItemLink( ii );
            }
        }
    }

    return nullptr;
}


wxString API_HANDLER_EDITOR::describeItem( EDA_ITEM* aItem ) const
{
    return aItem ? aItem->GetFriendlyName() : wxString();
}


google::protobuf::Any API_HANDLER_EDITOR::rebaseStale( const std::string& aClient,
                                                       const types::ItemHeader& aHeader,
                                                       const google::protobuf::Any& aItem )
{
    std::optional<std::string> id = PROTO_MERGE::ItemId( aItem );

    if( !id )
        return aItem;

    uint64_t                      since = COEDIT::LastRead( aClient );
    std::optional<COEDIT::CHANGE> personal = COEDIT::PersonChangedAfter( *id, since );

    if( !personal )
        return aItem;

    EDA_ITEM* live = resolveLiveItem( KIID( *id ) );
    const EDA_ITEM* baseItem = live ? personBaseSince( live, since ) : nullptr;

    if( !baseItem )
        return aItem;

    google::protobuf::Any base, person;
    baseItem->Serialize( base );
    live->Serialize( person );

    std::optional<PROTO_MERGE::RESULT> merged = PROTO_MERGE::ThreeWay( base, person, aItem );

    if( !merged )
        return aItem;

    COEDIT::NOTICE notice;
    notice.kind = merged->keptFromPerson.empty() ? "rebased" : "collision";
    notice.actor = aClient;
    notice.doc = coeditDocKind();
    notice.json = nlohmann::json( { { "item", *id },
                                    { "reason", "the person changed it after you last read it" },
                                    { "from_agent", merged->fromAgent },
                                    { "kept_from_person", merged->keptFromPerson } } ).dump();
    COEDIT::PostNotice( notice );

    if( !merged->keptFromPerson.empty() )
    {
        COLLISION c;
        c.client = aClient;
        c.header.CopyFrom( aHeader );
        c.id = live->m_Uuid;
        c.what = describeItem( live );
        c.fields = merged->keptFromPerson;
        c.base = base;
        c.agent = aItem;
        c.personAt = personal->at;
        raiseCollision( std::move( c ) );
    }

    return merged->merged;
}


namespace
{
wxString verbFor( const std::vector<std::string>& aFields )
{
    for( const std::string& f : aFields )
    {
        if( f == "position" )
            return _( "move" );

        if( f == "orientation" )
            return _( "rotate" );

        if( f == "layer" )
            return _( "move to another layer" );
    }

    wxString list;

    for( const std::string& f : aFields )
        list << ( list.IsEmpty() ? wxString() : wxString( wxS( ", " ) ) ) << wxString::FromUTF8( f );

    return wxString::Format( _( "change %s of" ), list );
}

wxString agoText( int64_t aAtMs )
{
    int64_t secs = ( wxGetUTCTimeMillis().GetValue() - aAtMs ) / 1000;

    if( secs < 60 )
        return _( "just now" );

    if( secs < 3600 )
        return wxString::Format( _( "%d min ago" ), (int) ( secs / 60 ) );

    return wxString::Format( _( "%d h ago" ), (int) ( secs / 3600 ) );
}
} // namespace


wxString API_HANDLER_EDITOR::collisionText( const COLLISION& c ) const
{
    wxString agent = wxString::FromUTF8( c.client );
    agent.Replace( wxS( "agent:" ), _( "Agent " ) );

    return wxString::Format( _( "%s wants to %s %s; you changed it %s." ), agent,
                             verbFor( c.fields ), c.what, agoText( c.personAt ) );
}


HANDLER_RESULT<kiapi::backplane::coedit::ResolveCollisionResponse>
API_HANDLER_EDITOR::handleResolveCollision(
        const HANDLER_CONTEXT<kiapi::backplane::coedit::ResolveCollision>& aCtx )
{
    if( aCtx.Request.document().type() != thisDocumentType() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    // The person's answer: never an agent's.
    if( aCtx.ClientName.rfind( "agent:", 0 ) == 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "a collision is the person's to resolve" );
        return tl::unexpected( e );
    }

    {
        COEDIT::ACTOR_SCOPE person( "" );
        ResolveCollision( (int) aCtx.Request.choice() );
    }

    kiapi::backplane::coedit::ResolveCollisionResponse response;
    response.set_remaining( (uint32_t) m_collisions.size() );
    return response;
}


void API_HANDLER_EDITOR::raiseCollision( COLLISION aCollision )
{
    // One per item: a newer request from the same agent replaces the waiting one.
    std::erase_if( m_collisions, [&]( const COLLISION& c )
                   { return c.id == aCollision.id && c.client == aCollision.client; } );

    m_collisions.push_back( std::move( aCollision ) );

    if( m_collisions.size() == 1 )
        showCollision();
}


void API_HANDLER_EDITOR::showCollision()
{
    if( !m_frame || !m_frame->GetInfoBar() )
        return;

    WX_INFOBAR* bar = m_frame->GetInfoBar();

    if( m_collisions.empty() )
    {
        bar->Dismiss();
        return;
    }

    wxString msg = collisionText( m_collisions.front() );

    if( m_collisions.size() > 1 )
        msg << wxString::Format( _( "  (1 of %zu)" ), m_collisions.size() );

    bar->RemoveAllButtons();

    auto add = [&]( const wxString& aLabel, int aChoice )
    {
        wxButton* button = new wxButton( bar, wxID_ANY, aLabel );
        button->Bind( wxEVT_BUTTON, [this, aChoice]( wxCommandEvent& ) { ResolveCollision( aChoice ); } );
        bar->AddButton( button );
    };

    add( _( "Keep yours" ), 0 );
    add( _( "Take agent's" ), 1 );
    add( _( "See both" ), 2 );
    bar->ShowMessage( msg, wxICON_QUESTION );
}


void API_HANDLER_EDITOR::ResolveCollision( int aChoice )
{
    if( m_collisions.empty() )
        return;

    COLLISION& c = m_collisions.front();
    wxString   label = wxString::Format( _( "Agent's %s" ), c.what );

    if( aChoice == 2 )
    {
        showAgentGhost( c.agent, label, true );
        return;   // the bar stays for the decision
    }

    showAgentGhost( c.agent, label, false );

    nlohmann::json report = { { "item", c.id.AsStdString() }, { "fields", c.fields } };

    if( aChoice == 1 )
    {
        if( EDA_ITEM* live = resolveLiveItem( c.id ) )
        {
            google::protobuf::Any person;
            live->Serialize( person );

            if( std::optional<PROTO_MERGE::RESULT> merged =
                        PROTO_MERGE::ThreeWay( c.base, person, c.agent, true ) )
            {
                COEDIT::ACTOR_SCOPE actor( c.client );
                std::string         commitClient = c.client + "#resolved";
                google::protobuf::RepeatedPtrField<google::protobuf::Any> items;
                *items.Add() = merged->merged;

                m_activeClients.insert( commitClient );
                handleCreateUpdateItemsInternal( false, commitClient, c.header, items,
                                                 []( ItemStatus, google::protobuf::Any ) {} );
                pushCurrentCommit( commitClient,
                                   wxString::Format( _( "Agent's change to %s (you chose it)" ), c.what ) );
                m_activeClients.erase( commitClient );
            }
        }

        report["outcome"] = "took_agents";
    }
    else
    {
        report["outcome"] = "kept_persons";
    }

    COEDIT::NOTICE notice;
    notice.kind = "collision_resolved";
    notice.actor = c.client;
    notice.doc = coeditDocKind();
    notice.json = report.dump();
    COEDIT::PostNotice( notice );

    m_collisions.pop_front();
    showCollision();
}


HANDLER_RESULT<kiapi::backplane::coedit::SetAgentActivityResponse>
API_HANDLER_EDITOR::handleSetAgentActivity(
        const HANDLER_CONTEXT<kiapi::backplane::coedit::SetAgentActivity>& aCtx )
{
    COEDIT::ACTIVITY a;
    a.actor = aCtx.Request.actor().empty() ? aCtx.ClientName : aCtx.Request.actor();
    a.task = aCtx.Request.task();
    a.state = aCtx.Request.state().empty() ? "working" : aCtx.Request.state();
    COEDIT::SetActivity( a );
    return kiapi::backplane::coedit::SetAgentActivityResponse();
}


HANDLER_RESULT<kiapi::backplane::coedit::UndoAgentChangeResponse>
API_HANDLER_EDITOR::handleUndoAgentChange(
        const HANDLER_CONTEXT<kiapi::backplane::coedit::UndoAgentChange>& aCtx )
{
    if( aCtx.Request.document().type() != thisDocumentType() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    kiapi::backplane::coedit::UndoAgentChangeResponse response;
    std::string                                       error;
    response.set_undone( undoChangeBySeq( aCtx.Request.seq(), error ) );
    response.set_error( error );
    return response;
}
