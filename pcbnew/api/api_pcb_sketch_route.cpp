/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
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

#include <algorithm>
#include <cmath>
#include <climits>
#include <optional>
#include <set>

#include <api/api_enums.h>
#include <api/api_handler_pcb.h>
#include <api/api_utils.h>
#include <board.h>
#include <board_commit.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_edit_frame.h>
#include <router/sketch_route_board.h>
#include <tool/tool_manager.h>
#include <tools/pcb_selection_tool.h>

using namespace kiapi::common::commands;
using namespace kiapi::backplane;


namespace
{

ApiResponseStatus badRequest( const std::string& aMessage )
{
    ApiResponseStatus e;
    e.set_status( ApiStatusCode::AS_BAD_REQUEST );
    e.set_error_message( aMessage );
    return e;
}


/// Pads, tracks and vias stand for themselves; a footprint stands for its pads.
void addRoutable( BOARD_ITEM* aItem, std::vector<BOARD_CONNECTED_ITEM*>& aItems,
                  std::set<BOARD_CONNECTED_ITEM*>& aSeen )
{
    auto add =
            [&]( BOARD_CONNECTED_ITEM* aConnected )
            {
                if( aSeen.insert( aConnected ).second )
                    aItems.push_back( aConnected );
            };

    if( aItem->Type() == PCB_FOOTPRINT_T )
    {
        for( PAD* pad : static_cast<FOOTPRINT*>( aItem )->Pads() )
            add( pad );
    }
    else if( BOARD_CONNECTED_ITEM* connected = dynamic_cast<BOARD_CONNECTED_ITEM*>( aItem ) )
    {
        add( connected );
    }
}


DIRECTION_45::CORNER_MODE cornerMode( SketchRouteCornerMode aMode )
{
    switch( aMode )
    {
    case SketchRouteCornerMode::SRCM_ROUNDED_45: return DIRECTION_45::ROUNDED_45;
    case SketchRouteCornerMode::SRCM_MITERED_90: return DIRECTION_45::MITERED_90;
    case SketchRouteCornerMode::SRCM_ROUNDED_90: return DIRECTION_45::ROUNDED_90;
    default:                                     return DIRECTION_45::MITERED_45;
    }
}

} // namespace


HANDLER_RESULT<SketchRouteResponse> API_HANDLER_PCB::handleSketchRoute(
        const HANDLER_CONTEXT<SketchRoute>& aCtx )
{
    // Without a board no handler would take it and the client got AS_UNHANDLED: say what is missing
    if( !aCtx.Request.has_board() )
        return tl::unexpected( badRequest( "SketchRoute needs `board` (the board's DocumentSpecifier)" ) );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::vector<BOARD_CONNECTED_ITEM*> items;
    std::set<BOARD_CONNECTED_ITEM*>    seen;

    if( aCtx.Request.items_size() > 0 )
    {
        for( const types::KIID& id : aCtx.Request.items() )
        {
            std::optional<BOARD_ITEM*> item = getItemById( KIID( id.value() ) );

            if( !item )
                return tl::unexpected( badRequest( fmt::format( "item {} is not on the board", id.value() ) ) );

            addRoutable( *item, items, seen );
        }
    }
    else if( frame() )
    {
        if( PCB_SELECTION_TOOL* selectionTool = toolManager()->GetTool<PCB_SELECTION_TOOL>() )
        {
            for( EDA_ITEM* item : selectionTool->GetSelection() )
            {
                if( BOARD_ITEM* boardItem = dynamic_cast<BOARD_ITEM*>( item ) )
                    addRoutable( boardItem, items, seen );
            }
        }
    }
    else
    {
        // A headless server keeps the selection on the items themselves
        for( BOARD_ITEM* item : board()->GetItemSet() )
        {
            if( item->IsSelected() )
            {
                addRoutable( item, items, seen );
            }
            else if( item->Type() == PCB_FOOTPRINT_T )
            {
                for( PAD* pad : static_cast<FOOTPRINT*>( item )->Pads() )
                {
                    if( pad->IsSelected() )
                        addRoutable( pad, items, seen );
                }
            }
        }
    }

    if( items.empty() )
        return tl::unexpected( badRequest( "no pads, footprints, tracks or vias to route" ) );

    SKETCH_ROUTE_REQUEST request;
    request.m_items = items;
    request.m_cornerMode = cornerMode( aCtx.Request.corner_mode() );
    request.m_allowVias = !aCtx.Request.disallow_vias();
    request.m_timeLimitMs = (int) std::min<uint32_t>( aCtx.Request.time_limit_ms(), INT_MAX );

    if( aCtx.Request.has_track_width() )
    {
        int64_t width = aCtx.Request.track_width().value_nm();

        if( width < 0 || width > INT_MAX )
            return tl::unexpected( badRequest( "track_width is out of range" ) );

        request.m_trackWidth = (int) width;
    }

    if( aCtx.Request.has_min_track_width() )
    {
        int64_t width = aCtx.Request.min_track_width().value_nm();

        if( width < 0 || width > INT_MAX )
            return tl::unexpected( badRequest( "min_track_width is out of range" ) );

        request.m_minTrackWidth = (int) width;
    }

    if( aCtx.Request.has_guide() && aCtx.Request.guide().nodes_size() > 0 )
    {
        SHAPE_LINE_CHAIN guide = UnpackPolyLine( aCtx.Request.guide() );

        // The router follows straight segments; approximate any arcs
        guide.ClearArcs();
        request.m_guide = guide;
    }

    PCB_LAYER_ID preferred = FromProtoEnum<PCB_LAYER_ID>( aCtx.Request.preferred_layer() );

    if( preferred != UNDEFINED_LAYER )
    {
        if( !IsCopperLayer( preferred ) || !board()->IsLayerEnabled( preferred ) )
            return tl::unexpected( badRequest( "the preferred layer is not an enabled copper layer" ) );

        request.m_preferredLayer = preferred;
    }

    // Join the client's open commit if it has one, so the routes go with the rest of it
    COMMIT*             commit = getCurrentCommit( aCtx.ClientName );
    SKETCH_ROUTE_RESULT result = RouteSketchOnBoard( board(), request, commit );

    SketchRouteResponse response;
    response.set_connections( result.m_connections );
    response.set_routed( result.m_stats.m_routed );
    response.set_failed( std::max( 0, result.m_connections - result.m_stats.m_routed ) );
    response.set_vias( result.m_stats.m_vias );
    response.set_length_nm( result.m_stats.m_length );
    response.set_timed_out( result.m_stats.m_timedOut );
    response.set_elapsed_ms( result.m_stats.m_elapsedMs );
    response.set_narrowed( result.m_stats.m_narrowed );

    for( BOARD_ITEM* item : result.m_added )
        response.add_created_items()->set_value( item->m_Uuid.AsStdString() );

    for( const SKETCH_ROUTE_RESULT::FAILURE& failure : result.m_failures )
    {
        kiapi::backplane::SketchRouteFailure* out = response.add_failures();
        out->set_net( failure.m_net.ToStdString( wxConvUTF8 ) );

        if( failure.m_start )
            out->mutable_start()->set_value( failure.m_start->m_Uuid.AsStdString() );

        if( failure.m_end )
            out->mutable_end()->set_value( failure.m_end->m_Uuid.AsStdString() );

        out->mutable_start_pos()->set_x_nm( failure.m_startPos.x );
        out->mutable_start_pos()->set_y_nm( failure.m_startPos.y );
        out->mutable_end_pos()->set_x_nm( failure.m_endPos.x );
        out->mutable_end_pos()->set_y_nm( failure.m_endPos.y );
        out->set_reason( failure.m_reason.ToStdString( wxConvUTF8 ) );
    }

    if( !m_activeClients.count( aCtx.ClientName ) )
        pushCurrentCommit( aCtx.ClientName, _( "Sketch route via API" ) );

    return response;
}
