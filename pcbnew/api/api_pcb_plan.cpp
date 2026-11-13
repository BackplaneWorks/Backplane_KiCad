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

// The routing plan API (docs/backplane/floorplanner.md): kiapi.backplane.plan.

#include <ratsnest/ratsnest_data.h>
#include <connectivity/connectivity_algo.h>
#include <api/api_handler_pcb.h>
#include <backplane/board_plan.h>
#include <backplane/plan_tool.h>
#include <backplane_coedit.h>
#include <board.h>
#include <board_commit.h>
#include <board_design_settings.h>
#include <connectivity/connectivity_data.h>
#include <footprint.h>
#include <netinfo.h>
#include <pad.h>
#include <pcb_edit_frame.h>
#include <pcb_marker.h>
#include <pcb_track.h>
#include <drc/drc_item.h>
#include <router/sketch_route_board.h>
#include <tool/tool_manager.h>
#include <zone.h>
#include <zone_filler.h>

#include <json_common.h>

#include <set>

using namespace kiapi::backplane::plan;
using nlohmann::json;


namespace
{
ApiResponseStatus planError( ApiStatusCode aCode, const std::string& aMessage )
{
    ApiResponseStatus e;
    e.set_status( aCode );
    e.set_error_message( aMessage );
    return e;
}

bool isAgent( const std::string& aClient )
{
    return aClient.rfind( "agent:", 0 ) == 0;
}

/// Each array of the plan JSON that holds items with ids.
const char* const ARRAYS[] = { "corridors", "planes", "groups", "splits", "notes", "constraints" };

std::string arrayFor( const std::string& aKind )
{
    if( aKind == "corridor" )   return "corridors";
    if( aKind == "plane" )      return "planes";
    if( aKind == "group" )      return "groups";
    if( aKind == "split" )      return "splits";
    if( aKind == "note" )       return "notes";
    if( aKind == "constraint" ) return "constraints";
    return "";
}

std::string prefixFor( const std::string& aArray )
{
    if( aArray == "corridors" )   return "c";
    if( aArray == "planes" )      return "p";
    if( aArray == "groups" )      return "g";
    if( aArray == "splits" )      return "s";
    if( aArray == "notes" )       return "n";
    return "k";
}

json* findItem( json& aPlan, const std::string& aId, std::string* aArray = nullptr )
{
    for( const char* name : ARRAYS )
    {
        if( !aPlan.contains( name ) )
            continue;

        for( json& item : aPlan[name] )
        {
            if( item.value( "id", "" ) == aId )
            {
                if( aArray )
                    *aArray = name;

                return &item;
            }
        }
    }

    return nullptr;
}
} // namespace


bool API_HANDLER_PCB::planBusy( std::string& aReason )
{
    if( !frame() )
        return false;

    if( PLAN_TOOL* tool = toolManager()->GetTool<PLAN_TOOL>(); tool && tool->IsDragging() )
    {
        aReason = "the person is dragging plan items; retry in a moment";
        return true;
    }

    return false;
}


void API_HANDLER_PCB::commitPlan( const BOARD_PLAN& aPlan, const std::string& aClient,
                                  const wxString& aDescription, const std::string& aOp,
                                  const std::vector<std::string>& aItems )
{
    if( PCB_EDIT_FRAME* f = frame() )
    {
        f->CommitPlan( aPlan, aDescription, aOp, aItems );
        return;
    }

    // Headless: no undo stack, the plan is saved with the board.
    BOARD_PLAN next = aPlan;
    next.history = board()->GetPlan().history;
    next.revision = board()->GetPlan().revision;

    if( next.ToJson() == board()->GetPlan().ToJson() )
        return;

    next.Note( aClient, aOp, aItems );
    board()->GetPlan() = std::move( next );
    onModified();
}


HANDLER_RESULT<PlanResponse> API_HANDLER_PCB::handleGetPlan( const HANDLER_CONTEXT<GetPlan>& aCtx )
{
    HANDLER_RESULT<bool> valid = validateDocument( aCtx.Request.board() );

    if( !valid )
        return tl::unexpected( valid.error() );

    const BOARD_PLAN& plan = board()->GetPlan();
    json              out = plan.ToJson();

    if( !plan.loadError.empty() )
        out["load_error"] = plan.loadError;

    if( aCtx.Request.derived() )
    {
        json stackup = json::object();

        for( PCB_LAYER_ID layer : board()->GetEnabledLayers().CuStack() )
        {
            std::string name = LSET::Name( layer ).ToStdString();
            stackup[name] = { { "chip", PlanLayerChip( *board(), name ) },
                              { "text", DescribePlanLayer( *board(), name ) } };
        }

        PLAN_AUDIT    audit = PLAN_AUDIT::Run( plan, *board() );
        PLAN_PROGRESS progress = PLAN_PROGRESS::Run( plan, *board(), audit );
        out["derived"] = { { "stackup", stackup },
                           { "audit", audit.ToJson() },
                           { "progress", progress.ToJson() } };
    }

    PlanResponse response;
    response.set_json( out.dump() );
    response.set_revision( plan.revision );
    return response;
}


HANDLER_RESULT<ProposePlanResponse> API_HANDLER_PCB::handleProposePlan(
        const HANDLER_CONTEXT<ProposePlan>& aCtx )
{
    HANDLER_RESULT<bool> valid = validateDocument( aCtx.Request.board() );

    if( !valid )
        return tl::unexpected( valid.error() );

    if( std::string why; planBusy( why ) )
        return tl::unexpected( planError( ApiStatusCode::AS_BUSY, why ) );

    BOARD_PLAN proposal;

    try
    {
        proposal = BOARD_PLAN::FromJson( json::parse( aCtx.Request.json() ) );
    }
    catch( const std::exception& e )
    {
        return tl::unexpected( planError( ApiStatusCode::AS_BAD_REQUEST,
                                          std::string( "invalid plan: " ) + e.what() ) );
    }

    const std::string& client = aCtx.ClientName;
    BOARD_PLAN         next = board()->GetPlan();
    json               reasons = json::object();
    std::vector<std::string> applied;

    auto keep = [&]( bool aLocked, bool aEdited, const std::string& aAuthor )
    {
        return aLocked || aEdited || aAuthor == "person";
    };

    if( aCtx.Request.mode() == PM_REPLACE_PROPOSED )
    {
        std::erase_if( next.corridors, [&]( const PLAN_CORRIDOR& c )
                       { return c.status == PLAN_STATUS::PROPOSED && !keep( c.locked, c.edited, c.author ); } );
        std::erase_if( next.planes, [&]( const PLAN_PLANE& p )
                       { return p.status == PLAN_STATUS::PROPOSED && !keep( p.locked, p.edited, p.author ); } );
    }

    auto upsert = [&]( auto& aList, auto aItem, bool aLocked, bool aEdited, auto&& aAssign )
    {
        for( auto& existing : aList )
        {
            if( existing.id != aItem.id )
                continue;

            if( aLocked )
                reasons[aItem.id] = "locked by the person";
            else if( aEdited )
                reasons[aItem.id] = "edited by the person; their version stays";
            else
            {
                aAssign( existing, aItem );
                applied.push_back( aItem.id );
            }

            return;
        }

        aList.push_back( aItem );
        applied.push_back( aItem.id );
    };

    for( PLAN_GROUP g : proposal.groups )
    {
        const PLAN_GROUP* old = next.FindGroup( g.id );
        upsert( next.groups, g, old && old->locked, false,
                []( PLAN_GROUP& a, const PLAN_GROUP& b ) { a = b; } );
    }

    for( PLAN_CORRIDOR c : proposal.corridors )
    {
        if( c.author.empty() )
            c.author = client;

        // An agent proposes; it does not approve.
        if( c.status == PLAN_STATUS::APPROVED || c.status == PLAN_STATUS::VERIFIED )
            c.status = PLAN_STATUS::PROPOSED;

        const PLAN_CORRIDOR* old = next.FindCorridor( c.id );
        upsert( next.corridors, c, old && old->locked, old && old->edited,
                []( PLAN_CORRIDOR& a, const PLAN_CORRIDOR& b ) { a = b; } );
    }

    for( PLAN_PLANE p : proposal.planes )
    {
        if( p.author.empty() )
            p.author = client;

        if( p.status == PLAN_STATUS::APPROVED || p.status == PLAN_STATUS::VERIFIED )
            p.status = PLAN_STATUS::PROPOSED;

        const PLAN_PLANE* old = next.FindPlane( p.id );
        upsert( next.planes, p, old && old->locked, old && old->edited,
                []( PLAN_PLANE& a, const PLAN_PLANE& b ) { a = b; } );
    }

    for( const PLAN_SPLIT& s : proposal.splits )
        upsert( next.splits, s, false, false, []( PLAN_SPLIT& a, const PLAN_SPLIT& b ) { a = b; } );

    for( PLAN_NOTE n : proposal.notes )
    {
        if( n.author.empty() )
            n.author = client;

        upsert( next.notes, n, false, n.author == "person",
                []( PLAN_NOTE& a, const PLAN_NOTE& b ) { a = b; } );
    }

    for( const PLAN_LAYER& l : proposal.layers )
    {
        if( PLAN_LAYER* existing = next.FindLayer( l.layer ) )
        {
            existing->role = l.role;
            existing->note = l.note;
        }
        else
        {
            PLAN_LAYER fresh = l;
            fresh.status = PLAN_STATUS::PROPOSED;
            next.layers.push_back( fresh );
        }
    }

    commitPlan( next, client, _( "Agent proposed a routing plan" ), "propose", applied );

    ProposePlanResponse response;
    response.set_revision( board()->GetPlan().revision );

    for( const std::string& id : applied )
        response.add_applied( id );

    for( auto& [id, why] : reasons.items() )
        response.add_refused( id );

    response.set_reasons( reasons.dump() );
    return response;
}


HANDLER_RESULT<UpdatePlanResponse> API_HANDLER_PCB::handleUpdatePlan(
        const HANDLER_CONTEXT<UpdatePlan>& aCtx )
{
    HANDLER_RESULT<bool> valid = validateDocument( aCtx.Request.board() );

    if( !valid )
        return tl::unexpected( valid.error() );

    if( std::string why; planBusy( why ) )
        return tl::unexpected( planError( ApiStatusCode::AS_BUSY, why ) );

    json ops;

    try
    {
        ops = json::parse( aCtx.Request.ops() );
    }
    catch( const std::exception& e )
    {
        return tl::unexpected( planError( ApiStatusCode::AS_BAD_REQUEST,
                                          std::string( "ops are a JSON array: " ) + e.what() ) );
    }

    if( !ops.is_array() )
        return tl::unexpected( planError( ApiStatusCode::AS_BAD_REQUEST, "ops are a JSON array" ) );

    const std::string& client = aCtx.ClientName;
    bool               agent = isAgent( client );
    const BOARD_PLAN&  current = board()->GetPlan();
    uint64_t           base = aCtx.Request.base() ? aCtx.Request.base() : current.revision;
    json               doc = current.ToJson();
    json               reasons = json::object();
    std::vector<std::string> applied;

    for( const json& op : ops )
    {
        std::string kind = op.value( "op", "" );
        std::string id = op.value( "id", "" );

        if( kind == "add" )
        {
            std::string array = arrayFor( op.value( "kind", "" ) );

            if( array.empty() || !op.contains( "item" ) || !op["item"].is_object() )
            {
                reasons[id.empty() ? "add" : id] = "add needs kind and item";
                continue;
            }

            json item = op["item"];
            std::string newId = item.value( "id", "" );

            if( newId.empty() || findItem( doc, newId ) )
            {
                BOARD_PLAN scratch = BOARD_PLAN::FromJson( doc );
                newId = scratch.NewId( prefixFor( array ) );
                item["id"] = newId;
            }

            if( agent )
            {
                item["author"] = client;
                item["edited"] = false;

                if( item.value( "status", "proposed" ) == "approved"
                    || item.value( "status", "proposed" ) == "verified" )
                {
                    item["status"] = "proposed";
                }
            }

            if( !doc.contains( array ) )
                doc[array] = json::array();

            doc[array].push_back( item );
            applied.push_back( newId );
            continue;
        }

        std::string array;
        json*       item = findItem( doc, id, &array );

        if( !item )
        {
            reasons[id] = "no such item";
            continue;
        }

        if( agent && item->value( "locked", false ) )
        {
            reasons[id] = "locked by the person";
            continue;
        }

        if( agent && current.PersonChangedSince( id, base ) )
        {
            reasons[id] = fmt::format( "the person changed it after revision {}; get the plan "
                                       "again and keep their edit", base );
            continue;
        }

        if( kind == "set" )
        {
            json fields = op.value( "fields", json::object() );

            if( agent )
            {
                for( const char* key : { "status", "locked", "edited", "author", "id" } )
                    fields.erase( key );
            }

            std::string before = item->value( "status", "proposed" );
            item->merge_patch( fields );

            // A changed approved item needs approval again.
            if( agent && ( before == "approved" || before == "verified" ) )
                ( *item )["status"] = "proposed";

            applied.push_back( id );
        }
        else if( kind == "delete" )
        {
            if( agent && item->value( "author", "" ) == "person" )
            {
                reasons[id] = "the person made it";
                continue;
            }

            json& list = doc[array];

            for( auto it = list.begin(); it != list.end(); ++it )
            {
                if( it->value( "id", "" ) == id )
                {
                    list.erase( it );
                    break;
                }
            }

            applied.push_back( id );
        }
        else if( kind == "status" )
        {
            std::string status = op.value( "status", "" );

            if( agent && ( status == "approved" || status == "verified" ) )
            {
                reasons[id] = "approval is the person's (ApprovePlan); verification is VerifyPlan's";
                continue;
            }

            ( *item )["status"] = status;
            applied.push_back( id );
        }
        else if( kind == "progress" )
        {
            ( *item )["progress"] = { { "routed", op.value( "routed", 0 ) },
                                      { "total", op.value( "total", 0 ) },
                                      { "failed", op.value( "failed", 0 ) } };
            applied.push_back( id );
        }
        else
        {
            reasons[id] = "unknown op '" + kind + "'";
        }
    }

    BOARD_PLAN next;

    try
    {
        next = BOARD_PLAN::FromJson( doc );
    }
    catch( const std::exception& e )
    {
        return tl::unexpected( planError( ApiStatusCode::AS_BAD_REQUEST,
                                          std::string( "the ops make an invalid plan: " ) + e.what() ) );
    }

    commitPlan( next, client, agent ? _( "Agent updated the routing plan" )
                                     : _( "Updated the routing plan" ),
                "update", applied );

    UpdatePlanResponse response;
    response.set_revision( board()->GetPlan().revision );

    for( const std::string& id : applied )
        response.add_applied( id );

    for( auto& [id, why] : reasons.items() )
        response.add_refused( id );

    response.set_reasons( reasons.dump() );
    return response;
}


HANDLER_RESULT<PlanResponse> API_HANDLER_PCB::handleApprovePlan(
        const HANDLER_CONTEXT<ApprovePlan>& aCtx )
{
    HANDLER_RESULT<bool> valid = validateDocument( aCtx.Request.board() );

    if( !valid )
        return tl::unexpected( valid.error() );

    if( isAgent( aCtx.ClientName ) )
    {
        return tl::unexpected( planError( ApiStatusCode::AS_BAD_REQUEST,
                                          "approval is the person's: ask them to approve it in "
                                          "KiCad or in Backplane" ) );
    }

    BOARD_PLAN  next = board()->GetPlan();
    PLAN_STATUS status = aCtx.Request.approve() ? PLAN_STATUS::APPROVED : PLAN_STATUS::REJECTED;
    std::vector<std::string> items;

    if( !aCtx.Request.layer().empty() )
    {
        next.SetLayerStatus( aCtx.Request.layer(), status );
        items.push_back( aCtx.Request.layer() );
    }

    for( const std::string& id : aCtx.Request.ids() )
    {
        if( next.SetItemStatus( id, status ) )
            items.push_back( id );
        else if( PLAN_GROUP* g = next.FindGroup( id ) )
        {
            g->status = status;

            for( PLAN_CORRIDOR& c : next.corridors )
                if( c.group == id && c.status == PLAN_STATUS::PROPOSED )
                    c.status = status;

            items.push_back( id );
        }
    }

    // The host speaks for the person.
    COEDIT::ACTOR_SCOPE person( "" );
    commitPlan( next, "", aCtx.Request.approve() ? _( "Approve routing plan" )
                                                 : _( "Reject routing plan" ),
                aCtx.Request.approve() ? "approve" : "reject", items );

    PlanResponse response;
    response.set_json( board()->GetPlan().ToJson().dump() );
    response.set_revision( board()->GetPlan().revision );
    return response;
}


HANDLER_RESULT<RoutePlanResponse> API_HANDLER_PCB::handleRoutePlan(
        const HANDLER_CONTEXT<RoutePlan>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> valid = validateDocument( aCtx.Request.board() );

    if( !valid )
        return tl::unexpected( valid.error() );

    RoutePlanResponse response;
    BOARD_PLAN        next = board()->GetPlan();
    std::set<std::string> only( aCtx.Request.ids().begin(), aCtx.Request.ids().end() );
    int               maxNets = aCtx.Request.max_nets() > 0 ? aCtx.Request.max_nets() : 4;

    // Stopped: the corridor this routing worked on went back to planning.
    if( !m_planRouting.empty() )
    {
        PLAN_CORRIDOR* was = next.FindCorridor( m_planRouting );

        if( was && was->status == PLAN_STATUS::PROPOSED )
        {
            m_planRouting.clear();
            m_planTried.clear();
            response.set_stopped( true );
            response.set_done( true );
            return response;
        }
    }

    // The next corridor: one still routing, else the first approved in routing order.
    std::vector<PLAN_CORRIDOR*> order;

    for( PLAN_CORRIDOR& c : next.corridors )
    {
        if( !only.empty() && !only.count( c.id ) )
            continue;

        if( c.status == PLAN_STATUS::ROUTING || next.Approved( c ) )
            order.push_back( &c );
    }

    std::stable_sort( order.begin(), order.end(),
                      []( PLAN_CORRIDOR* a, PLAN_CORRIDOR* b )
                      {
                          if( ( a->status == PLAN_STATUS::ROUTING ) != ( b->status == PLAN_STATUS::ROUTING ) )
                              return a->status == PLAN_STATUS::ROUTING;

                          return a->order < b->order;
                      } );

    COMMIT* commit = getCurrentCommit( aCtx.ClientName + "#plan" );
    m_activeClients.insert( aCtx.ClientName + "#plan" );

    auto finish = [&]( const wxString& aMessage )
    {
        pushCurrentCommit( aCtx.ClientName + "#plan", aMessage );
        m_activeClients.erase( aCtx.ClientName + "#plan" );
    };

    if( order.empty() )
    {
        // Corridors done: the approved plane regions become zones.
        std::vector<ZONE*> zones;

        for( PLAN_PLANE& p : next.planes )
        {
            if( ( !only.empty() && !only.count( p.id ) ) || !next.Approved( p ) )
                continue;

            NETINFO_ITEM* net = board()->FindNet( wxString::FromUTF8( p.net ) );
            PCB_LAYER_ID  layer = board()->GetLayerID( wxString::FromUTF8( p.layer ) );

            if( !net || layer == UNDEFINED_LAYER )
            {
                p.status = PLAN_STATUS::FAILED;
                continue;
            }

            ZONE* zone = new ZONE( board() );
            zone->SetLayer( layer );
            zone->SetNet( net );
            zone->SetZoneName( wxString::FromUTF8( "plan:" + p.id ) );
            zone->SetAssignedPriority( p.priority );

            SHAPE_POLY_SET poly;

            for( size_t r = 0; r < p.outline.size(); ++r )
            {
                SHAPE_LINE_CHAIN chain;

                for( const VECTOR2I& pt : p.outline[r] )
                    chain.Append( pt );

                chain.SetClosed( true );

                if( r == 0 )
                    poly.AddOutline( chain );
                else
                    poly.AddHole( chain );
            }

            zone->SetOutline( new SHAPE_POLY_SET( poly ) );
            commit->Add( zone );
            zones.push_back( zone );
            response.add_created( zone->m_Uuid.AsStdString() );
            p.status = PLAN_STATUS::ROUTED;
        }

        if( !zones.empty() )
        {
            ZONE_FILLER filler( board(), commit );
            filler.Fill( zones );
        }

        finish( _( "Plan: plane regions" ) );
        commitPlan( next, aCtx.ClientName, _( "Routing plan: planes poured" ), "route", {} );
        m_planRouting.clear();
        m_planTried.clear();
        response.set_done( true );
        return response;
    }

    PLAN_CORRIDOR* c = order.front();
    PLAN_GROUP*    g = next.FindGroup( c->group );
    m_planRouting = c->id;

    // The pads of each group net on the corridor's parts.
    std::set<std::string> refs( c->from.begin(), c->from.end() );
    refs.insert( c->to.begin(), c->to.end() );

    std::map<std::string, std::vector<PAD*>> padsByNet;

    if( g )
    {
        std::set<std::string> nets( g->nets.begin(), g->nets.end() );

        for( FOOTPRINT* fp : board()->Footprints() )
        {
            if( !refs.empty() && !refs.count( fp->GetReference().ToStdString( wxConvUTF8 ) ) )
                continue;

            for( PAD* pad : fp->Pads() )
            {
                std::string net = pad->GetNetname().ToStdString( wxConvUTF8 );

                if( nets.count( net ) )
                    padsByNet[net].push_back( pad );
            }
        }
    }

    std::shared_ptr<CONNECTIVITY_DATA> conn = board()->GetConnectivity();

    auto unrouted = [&]( const std::vector<PAD*>& aPads )
    {
        for( PAD* pad : aPads )
            if( !conn->GetRatsnestForPad( pad ).empty() )
                return true;

        return false;
    };

    std::set<std::string>& tried = m_planTried[c->id];
    std::vector<std::string> batch;
    int                      done = 0;

    for( auto& [net, pads] : padsByNet )
    {
        if( pads.size() < 2 || !unrouted( pads ) )
            ++done;
        else if( !tried.count( net ) && (int) batch.size() < maxNets )
            batch.push_back( net );
    }

    c->total = (int) padsByNet.size();
    c->status = PLAN_STATUS::ROUTING;

    if( !batch.empty() )
    {
        SKETCH_ROUTE_REQUEST request;
        std::set<BOARD_CONNECTED_ITEM*> seen;

        for( const std::string& net : batch )
        {
            tried.insert( net );

            for( PAD* pad : padsByNet[net] )
                if( seen.insert( pad ).second )
                    request.m_items.push_back( pad );
        }

        SHAPE_LINE_CHAIN guide;

        for( const VECTOR2I& p : c->path )
            guide.Append( p );

        request.m_guide = guide;
        request.m_cornerMode = DIRECTION_45::MITERED_45;
        request.m_allowVias = true;
        request.m_timeLimitMs = (int) std::min<uint32_t>( aCtx.Request.time_limit_ms(), INT_MAX );

        PCB_LAYER_ID layer = board()->GetLayerID( wxString::FromUTF8( c->layer ) );

        if( IsCopperLayer( layer ) && board()->IsLayerEnabled( layer ) )
            request.m_preferredLayer = layer;

        SKETCH_ROUTE_RESULT result = RouteSketchOnBoard( board(), request, commit );

        for( BOARD_ITEM* item : result.m_added )
            response.add_created( item->m_Uuid.AsStdString() );

        finish( wxString::Format( _( "Plan: route %s" ), wxString::FromUTF8( g ? g->name : c->id ) ) );

        // Count again on the updated connectivity.
        done = 0;

        for( auto& [net, pads] : padsByNet )
            if( pads.size() < 2 || !unrouted( pads ) )
                ++done;
    }
    else
    {
        finish( wxEmptyString );
    }

    c->routed = done;
    c->failed = 0;

    bool remaining = false;

    for( auto& [net, pads] : padsByNet )
    {
        if( pads.size() >= 2 && unrouted( pads ) )
        {
            if( tried.count( net ) )
                c->failed++;
            else
                remaining = true;
        }
    }

    if( !remaining )
    {
        c->status = c->failed ? PLAN_STATUS::FAILED : PLAN_STATUS::ROUTED;
        m_planTried.erase( c->id );
    }

    response.set_corridor( c->id );
    response.set_routed( c->routed );
    response.set_total( c->total );
    response.set_failed( c->failed );

    commitPlan( next, aCtx.ClientName, _( "Routing plan progress" ), "progress", { c->id } );

    PLAN_AUDIT    audit = PLAN_AUDIT::Run( board()->GetPlan(), *board() );
    PLAN_PROGRESS progress = PLAN_PROGRESS::Run( board()->GetPlan(), *board(), audit );
    response.set_report( progress.ToJson()["items"].dump() );
    response.set_done( false );
    return response;
}


HANDLER_RESULT<PlanResponse> API_HANDLER_PCB::handleStopPlanRouting(
        const HANDLER_CONTEXT<StopPlanRouting>& aCtx )
{
    HANDLER_RESULT<bool> valid = validateDocument( aCtx.Request.board() );

    if( !valid )
        return tl::unexpected( valid.error() );

    BOARD_PLAN next = board()->GetPlan();

    for( PLAN_CORRIDOR& c : next.corridors )
    {
        if( c.status == PLAN_STATUS::ROUTING || c.status == PLAN_STATUS::APPROVED )
            c.status = PLAN_STATUS::PROPOSED;
    }

    commitPlan( next, aCtx.ClientName, _( "Back to planning" ), "replan", {} );

    PlanResponse response;
    response.set_json( board()->GetPlan().ToJson().dump() );
    response.set_revision( board()->GetPlan().revision );
    return response;
}


HANDLER_RESULT<VerifyPlanResponse> API_HANDLER_PCB::handleVerifyPlan(
        const HANDLER_CONTEXT<VerifyPlan>& aCtx )
{
    HANDLER_RESULT<bool> valid = validateDocument( aCtx.Request.board() );

    if( !valid )
        return tl::unexpected( valid.error() );

    BOARD_PLAN next = board()->GetPlan();
    std::shared_ptr<CONNECTIVITY_DATA> conn = board()->GetConnectivity();

    // Copper length per net, DRC markers per net.
    std::map<std::string, double> length;

    for( PCB_TRACK* track : board()->Tracks() )
    {
        if( track->Type() != PCB_VIA_T )
            length[track->GetNetname().ToStdString( wxConvUTF8 )] += track->GetLength();
    }

    std::map<std::string, int> drc;

    for( PCB_MARKER* marker : board()->Markers() )
    {
        std::shared_ptr<RC_ITEM> rc = marker->GetRCItem();

        if( !rc || marker->IsExcluded() )
            continue;

        for( const KIID& id : { rc->GetMainItemID(), rc->GetAuxItemID() } )
        {
            if( BOARD_CONNECTED_ITEM* ci =
                        dynamic_cast<BOARD_CONNECTED_ITEM*>( board()->ResolveItem( id, true ) ) )
            {
                drc[ci->GetNetname().ToStdString( wxConvUTF8 )]++;
            }
        }
    }

    json                     report = json::object();
    std::vector<std::string> verified;

    for( PLAN_GROUP& g : next.groups )
    {
        int    unroutedNets = 0, markers = 0;
        double lo = -1, hi = 0;

        for( const std::string& net : g.nets )
        {
            NETINFO_ITEM* ni = board()->FindNet( wxString::FromUTF8( net ) );

            if( !ni )
                continue;

            if( RN_NET* rn = conn->GetRatsnestForNet( ni->GetNetCode() ) )
                if( !rn->GetUnsortedEdges().empty() )
                    unroutedNets++;

            markers += drc[net];
            double l = length[net];
            lo = lo < 0 ? l : std::min( lo, l );
            hi = std::max( hi, l );
        }

        double skewMm = ( hi - std::max( 0.0, lo ) ) / 1e6;
        bool   lengthOk = g.maxLength <= 0 || hi / 1e6 <= g.maxLength;
        bool   skewOk = g.maxSkew <= 0 || skewMm <= g.maxSkew;
        bool   ok = unroutedNets == 0 && markers == 0 && lengthOk && skewOk;

        report[g.id] = { { "ok", ok }, { "drc", markers }, { "unrouted", unroutedNets },
                         { "min_length", std::max( 0.0, lo ) / 1e6 }, { "max_length", hi / 1e6 },
                         { "skew", skewMm } };

        for( PLAN_CORRIDOR& c : next.corridors )
        {
            if( c.group != g.id )
                continue;

            if( ok && ( c.status == PLAN_STATUS::ROUTED || c.status == PLAN_STATUS::VERIFIED ) )
            {
                c.status = PLAN_STATUS::VERIFIED;
                verified.push_back( c.id );
            }
            else if( !ok && c.status == PLAN_STATUS::VERIFIED )
            {
                c.status = PLAN_STATUS::ROUTED;
            }
        }
    }

    for( PLAN_PLANE& p : next.planes )
    {
        if( p.status == PLAN_STATUS::ROUTED && drc[p.net] == 0 )
        {
            p.status = PLAN_STATUS::VERIFIED;
            verified.push_back( p.id );
        }
    }

    commitPlan( next, aCtx.ClientName, _( "Verify routing plan" ), "verify", verified );

    VerifyPlanResponse response;

    for( const std::string& id : verified )
        response.add_verified( id );

    response.set_report( report.dump() );
    return response;
}
