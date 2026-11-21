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

#include <backplane_merge.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

using nlohmann::json;


namespace BACKPLANE_MERGE
{

namespace
{
bool namedArray( const json& aValue )
{
    if( !aValue.is_array() || aValue.empty() )
        return false;

    for( const json& item : aValue )
    {
        if( !item.is_object() || !item.contains( "name" ) || !item["name"].is_string() )
            return false;
    }

    return true;
}

bool namedOrEmpty( const json& aValue )
{
    return aValue.is_null() || ( aValue.is_array() && aValue.empty() ) || namedArray( aValue );
}

json byName( const json& aArray, const std::string& aName )
{
    if( aArray.is_array() )
    {
        for( const json& item : aArray )
        {
            if( item.is_object() && item.value( "name", "" ) == aName )
                return item;
        }
    }

    return json();   // null: absent
}

std::vector<std::string> names( const json& aArray )
{
    std::vector<std::string> out;

    if( aArray.is_array() )
    {
        for( const json& item : aArray )
            out.push_back( item.value( "name", "" ) );
    }

    return out;
}
} // namespace


json MergeJson( const json& aBase, const json& aOurs, const json& aTheirs,
                std::vector<CONFLICT>& aConflicts, const std::string& aPath )
{
    if( aOurs == aTheirs )
        return aOurs;

    if( aOurs == aBase )
        return aTheirs;

    if( aTheirs == aBase )
        return aOurs;

    // Both changed.  Objects merge per key.
    if( aOurs.is_object() && aTheirs.is_object() && ( aBase.is_object() || aBase.is_null() ) )
    {
        json out = json::object();
        std::vector<std::string> keys;

        for( auto& [k, v] : aOurs.items() )
            keys.push_back( k );

        for( auto& [k, v] : aTheirs.items() )
            if( !aOurs.contains( k ) )
                keys.push_back( k );

        if( aBase.is_object() )
        {
            for( auto& [k, v] : aBase.items() )
                if( !aOurs.contains( k ) && !aTheirs.contains( k ) )
                    keys.push_back( k );
        }

        for( const std::string& k : keys )
        {
            json b = aBase.is_object() && aBase.contains( k ) ? aBase[k] : json();
            json o = aOurs.contains( k ) ? aOurs[k] : json();
            json t = aTheirs.contains( k ) ? aTheirs[k] : json();
            json m = MergeJson( b, o, t, aConflicts, aPath + "/" + k );

            // Absent on the winning side: deleted.
            bool absent = m.is_null() && ( ( m == o && !aOurs.contains( k ) )
                                           || ( m == t && !aTheirs.contains( k ) ) );

            if( !absent )
                out[k] = m;
        }

        return out;
    }

    // Arrays of named objects (net classes) merge per name.
    if( namedOrEmpty( aOurs ) && namedOrEmpty( aTheirs ) && namedOrEmpty( aBase )
        && ( namedArray( aOurs ) || namedArray( aTheirs ) ) )
    {
        std::vector<std::string> order = names( aOurs );

        for( const std::string& n : names( aTheirs ) )
            if( std::find( order.begin(), order.end(), n ) == order.end() )
                order.push_back( n );

        json out = json::array();

        for( const std::string& n : order )
        {
            json m = MergeJson( byName( aBase, n ), byName( aOurs, n ), byName( aTheirs, n ),
                                aConflicts, aPath + "/[name=" + n + "]" );

            if( !m.is_null() )
                out.push_back( m );
        }

        return out;
    }

    // One value changed differently on both sides: keep ours, report it.
    aConflicts.push_back( { aPath, aBase, aOurs, aTheirs } );
    return aOurs;
}


std::string RuleName( const std::string& aForm )
{
    size_t i = aForm.find( '(' );

    if( i == std::string::npos )
        return "";

    size_t j = i + 1;

    while( j < aForm.size() && std::isspace( (unsigned char) aForm[j] ) )
        ++j;

    if( aForm.compare( j, 4, "rule" ) != 0 || j + 4 >= aForm.size()
        || !std::isspace( (unsigned char) aForm[j + 4] ) )
    {
        return "";
    }

    j += 4;

    while( j < aForm.size() && std::isspace( (unsigned char) aForm[j] ) )
        ++j;

    if( j < aForm.size() && aForm[j] == '"' )
    {
        std::string name;

        for( ++j; j < aForm.size() && aForm[j] != '"'; ++j )
        {
            if( aForm[j] == '\\' && j + 1 < aForm.size() )
                ++j;

            name += aForm[j];
        }

        return name;
    }

    std::string name;

    while( j < aForm.size() && !std::isspace( (unsigned char) aForm[j] ) && aForm[j] != '('
           && aForm[j] != ')' )
    {
        name += aForm[j++];
    }

    return name;
}


std::vector<RULES_PART> SplitRules( const std::string& aText )
{
    std::vector<RULES_PART> parts;
    std::string             pending;   // comments and blank lines before the next form
    size_t                  i = 0, n = aText.size();

    while( i < n )
    {
        char c = aText[i];

        if( c == '#' )
        {
            size_t e = aText.find( '\n', i );
            e = e == std::string::npos ? n : e + 1;
            pending += aText.substr( i, e - i );
            i = e;
            continue;
        }

        if( c != '(' )
        {
            pending += c;
            ++i;
            continue;
        }

        // A top-level form: up to its matching parenthesis (strings and comments skipped).
        size_t start = i;
        int    depth = 0;

        for( ; i < n; ++i )
        {
            char d = aText[i];

            if( d == '"' )
            {
                for( ++i; i < n && aText[i] != '"'; ++i )
                    if( aText[i] == '\\' )
                        ++i;
            }
            else if( d == '#' )
            {
                while( i < n && aText[i] != '\n' )
                    ++i;
            }
            else if( d == '(' )
            {
                ++depth;
            }
            else if( d == ')' && --depth == 0 )
            {
                ++i;
                break;
            }
        }

        // Keep the rest of the line (a trailing newline).
        while( i < n && aText[i] != '\n' && std::isspace( (unsigned char) aText[i] ) )
            ++i;

        if( i < n && aText[i] == '\n' )
            ++i;

        std::string form = aText.substr( start, i - start );
        std::string name = RuleName( form );
        std::string key;

        if( !name.empty() )
            key = "rule " + name;
        else if( form.compare( 0, 8, "(version" ) == 0 )
            key = "version";
        else
            key = "form " + form;

        // Leading blank lines belong to nobody; comments belong to the form.
        size_t keepFrom = 0;

        while( keepFrom < pending.size() && ( pending[keepFrom] == '\n' || pending[keepFrom] == ' '
                                              || pending[keepFrom] == '\t' ) )
        {
            ++keepFrom;
        }

        parts.push_back( { key, pending.substr( keepFrom ) + form } );
        pending.clear();
    }

    if( !pending.empty() )
    {
        std::string trailing = pending;
        trailing.erase( 0, trailing.find_first_not_of( " \t\n" ) == std::string::npos
                                   ? trailing.size()
                                   : trailing.find_first_not_of( " \t\n" ) );

        if( !trailing.empty() )
            parts.push_back( { "trailing", trailing } );
    }

    return parts;
}


namespace
{
std::string join( const std::vector<RULES_PART>& aParts )
{
    std::string out;

    for( size_t ii = 0; ii < aParts.size(); ++ii )
    {
        std::string text = aParts[ii].text;

        if( !text.empty() && text.back() != '\n' )
            text += '\n';

        // A blank line between forms, as KiCad writes them.
        if( ii > 0 )
            out += '\n';

        out += text;
    }

    return out;
}

std::string trimmed( const std::string& aText )
{
    size_t a = aText.find_first_not_of( " \t\n" );
    size_t b = aText.find_last_not_of( " \t\n" );
    return a == std::string::npos ? "" : aText.substr( a, b - a + 1 );
}

json partsJson( const std::vector<RULES_PART>& aParts, std::vector<std::string>& aOrder )
{
    json out = json::object();

    for( const RULES_PART& p : aParts )
    {
        out[p.key] = trimmed( p.text );
        aOrder.push_back( p.key );
    }

    return out;
}
} // namespace


std::string MergeRules( const std::string& aBase, const std::string& aOurs,
                        const std::string& aTheirs, std::vector<CONFLICT>& aConflicts )
{
    if( trimmed( aOurs ) == trimmed( aTheirs ) )
        return aOurs;

    if( trimmed( aOurs ) == trimmed( aBase ) )
        return aTheirs;

    if( trimmed( aTheirs ) == trimmed( aBase ) )
        return aOurs;

    std::vector<std::string> baseOrder, ourOrder, theirOrder;
    json base = partsJson( SplitRules( aBase ), baseOrder );
    json ours = partsJson( SplitRules( aOurs ), ourOrder );
    json theirs = partsJson( SplitRules( aTheirs ), theirOrder );

    std::vector<CONFLICT> local;
    json merged = MergeJson( base, ours, theirs, local, "" );

    for( CONFLICT& c : local )
    {
        c.path = c.path.empty() ? c.path : c.path.substr( 1 );   // "rule DDR skew"
        aConflicts.push_back( c );
    }

    // Our order, with their new forms after the form they followed.
    std::vector<std::string> order = ourOrder;

    for( size_t ii = 0; ii < theirOrder.size(); ++ii )
    {
        const std::string& k = theirOrder[ii];

        if( std::find( order.begin(), order.end(), k ) != order.end() )
            continue;

        auto after = order.end();

        for( size_t jj = ii; jj-- > 0; )
        {
            after = std::find( order.begin(), order.end(), theirOrder[jj] );

            if( after != order.end() )
            {
                ++after;
                break;
            }
        }

        order.insert( after == order.end() && ii == 0 ? order.begin() : after, k );
    }

    std::vector<RULES_PART> parts;

    for( const std::string& k : order )
    {
        if( merged.contains( k ) && merged[k].is_string() )
            parts.push_back( { k, merged[k].get<std::string>() } );
    }

    return join( parts );
}


std::string EditRules( const std::string& aText, const std::vector<std::string>& aUpsertRuleTexts,
                       const std::vector<std::string>& aRemoveNames )
{
    std::vector<RULES_PART> parts = SplitRules( aText );
    std::set<std::string>   remove( aRemoveNames.begin(), aRemoveNames.end() );

    std::erase_if( parts, [&]( const RULES_PART& p )
                   { return p.key.rfind( "rule ", 0 ) == 0 && remove.count( p.key.substr( 5 ) ); } );

    if( parts.empty() || parts.front().key != "version" )
        parts.insert( parts.begin(), { "version", "(version 2)\n" } );

    for( const std::string& text : aUpsertRuleTexts )
    {
        std::string name = RuleName( text );
        std::string key = "rule " + name;
        auto        it = std::find_if( parts.begin(), parts.end(),
                                       [&]( const RULES_PART& p ) { return p.key == key; } );

        if( it != parts.end() )
        {
            // Keep the comments written above the rule.
            size_t      form = it->text.find( '(' );
            std::string comments = form == std::string::npos ? "" : it->text.substr( 0, form );
            it->text = ( text.find( '#' ) == 0 ? "" : comments ) + text;
        }
        else
            parts.push_back( { key, text } );
    }

    return join( parts );
}

} // namespace BACKPLANE_MERGE
