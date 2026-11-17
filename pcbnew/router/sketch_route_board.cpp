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

#include "sketch_route_board.h"

#include <optional>

#include <board.h>
#include <board_design_settings.h>
#include <commit.h>
#include <drc/drc_engine.h>
#include <netinfo.h>
#include <pcb_track.h>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>

#include <wx/filename.h>
#include <wx/log.h>

#include "pns_arc.h"
#include "pns_kicad_iface.h"
#include "pns_node.h"
#include "pns_router.h"
#include "pns_routing_settings.h"
#include "pns_segment.h"
#include "pns_via.h"


SKETCH_ROUTE_RESULT RouteSketchOnBoard( BOARD* aBoard, const SKETCH_ROUTE_REQUEST& aRequest,
                                        COMMIT* aCommit )
{
    SKETCH_ROUTE_RESULT result;

    if( !aBoard )
        return result;

    // The router takes clearances and widths from the board's DRC engine, which a board loaded
    // without an editor doesn't have yet.  Set one up the way the zone filler does.
    BOARD_DESIGN_SETTINGS& bds = aBoard->GetDesignSettings();

    if( !bds.m_DRCEngine )
    {
        std::shared_ptr<DRC_ENGINE> drcEngine = std::make_shared<DRC_ENGINE>( aBoard, &bds );

        try
        {
            drcEngine->InitEngine( wxFileName( aBoard->GetDesignRulesPath() ) );
        }
        catch( ... )
        {
            // Custom rules that fail to compile only matter to DRC; the implicit constraints
            // from the board setup still apply
        }

        bds.m_DRCEngine = drcEngine;
    }
    else
    {
        // Net classes and rules may have changed since the engine was built (a net class set
        // over the API in the same server): widths and clearances come from it, so rebuild it.
        // A track routed at the Default class's width where the net's class asked for more was
        // the token benchmark's repro 8.
        aBoard->SynchronizeNetsAndNetClasses( false );

        try
        {
            bds.m_DRCEngine->InitEngine( wxFileName( aBoard->GetDesignRulesPath() ) );
        }
        catch( ... )
        {
        }
    }

    // Nor the ratsnest, which is where the unrouted connections come from
    aBoard->BuildConnectivity();

    PNS::ROUTER           router;
    PNS_KICAD_IFACE_BASE  iface;
    PNS::ROUTING_SETTINGS settings( nullptr, "" );

    iface.SetBoard( aBoard );
    router.SetInterface( &iface );
    router.ClearWorld();
    router.SyncWorld();
    router.LoadSettings( &settings );
    settings.SetCornerMode( aRequest.m_cornerMode );

    PNS::SKETCH_ROUTER          sketch( &router );
    PNS::SKETCH_ROUTER_OPTIONS& options = sketch.Options();

    PNS::SKETCH_ROUTER::BoardConventions( aBoard, &iface, options );
    options.m_boundary = aBoard->GetBoardEdgesBoundingBox();
    options.m_guide = aRequest.m_guide;
    options.m_allowVias = aRequest.m_allowVias;
    options.m_trackWidth = aRequest.m_trackWidth;
    options.m_minTrackWidth = aRequest.m_minTrackWidth;

    // The board's rules may hold a via to a size by where it is (a smaller laser via only
    // inside a fine-pitch part's fanout): ask them for each kind where the search would put one
    std::map<const PNS::SKETCH_VIA_KIND*, std::unique_ptr<PCB_VIA>> probes;

    options.m_viaAllowed =
            [&]( const PNS::SKETCH_VIA_KIND& aKind, PNS::NET_HANDLE aNet, const VECTOR2I& aPos ) -> bool
            {
                std::unique_ptr<PCB_VIA>& probe = probes[&aKind];

                if( !probe )
                {
                    if( aKind.m_like && aKind.m_like->Type() == PCB_VIA_T )
                    {
                        probe.reset( static_cast<PCB_VIA*>( aKind.m_like->Clone() ) );
                    }
                    else
                    {
                        probe = std::make_unique<PCB_VIA>( aBoard );
                        probe->SetViaType( (VIATYPE) aKind.m_type );
                        probe->SetLayerPair( iface.GetBoardLayerFromPNSLayer( aKind.m_layerStart ),
                                             iface.GetBoardLayerFromPNSLayer( aKind.m_layerEnd ) );
                        probe->SetWidth( PADSTACK::TEMP_ALL_LAYERS, aKind.m_diameter );
                        probe->SetDrill( aKind.m_drill );
                    }

                    probe->SetParent( aBoard );

                    // It moves: nothing the rules work out about it may be kept
                    probe->SetFlags( ROUTER_TRANSIENT );
                }

                probe->SetPosition( aPos );

                if( NETINFO_ITEM* net = static_cast<NETINFO_ITEM*>( aNet ) )
                    probe->SetNet( net );

                DRC_ENGINE& engine = *bds.m_DRCEngine;

                auto within =
                        [&]( DRC_CONSTRAINT_T aType, PCB_LAYER_ID aLayer, int aValue )
                        {
                            DRC_CONSTRAINT c = engine.EvalRules( aType, probe.get(), nullptr, aLayer );

                            if( c.IsNull() || c.GetSeverity() == RPT_SEVERITY_IGNORE )
                                return true;

                            return ( !c.GetValue().HasMin() || aValue >= c.GetValue().Min() )
                                   && ( !c.GetValue().HasMax() || aValue <= c.GetValue().Max() );
                        };

                // On the layers the design rule checks themselves ask about
                bool ok = within( HOLE_SIZE_CONSTRAINT, UNDEFINED_LAYER, aKind.m_drill );

                probe->Padstack().ForEachUniqueLayer(
                        [&]( PCB_LAYER_ID aLayer )
                        {
                            if( !ok || probe->IsGhostLayer( aLayer ) )
                                return;

                            ok = within( VIA_DIAMETER_CONSTRAINT, aLayer, probe->GetWidth( aLayer ) )
                                 && within( ANNULAR_WIDTH_CONSTRAINT, aLayer,
                                            ( probe->GetWidth( aLayer ) - probe->GetDrillValue() ) / 2 );
                        } );

                if( !ok )
                    return false;

                DRC_CONSTRAINT disallow = engine.EvalRules( DISALLOW_CONSTRAINT, probe.get(), nullptr,
                                                            UNDEFINED_LAYER );

                return !disallow.m_DisallowFlags || disallow.GetSeverity() == RPT_SEVERITY_IGNORE;
            };

    if( aRequest.m_timeLimitMs > 0 )
        options.m_timeLimitMs = aRequest.m_timeLimitMs;

    if( IsCopperLayer( aRequest.m_preferredLayer ) )
        options.m_preferredLayer = iface.GetPNSLayerFromBoardLayer( aRequest.m_preferredLayer );

    if( aRequest.m_progress )
        sketch.SetProgressCallback( aRequest.m_progress );

    // For comparing the router with and without a piece of itself (the routing benchmark)
    if( const char* off = std::getenv( "BACKPLANE_SKETCH_OFF" ) )
    {
        const std::string list( off );

        if( list.find( "field" ) != std::string::npos )
            options.m_obstacleField = false;

        if( list.find( "viaf" ) != std::string::npos )
            options.m_viaField = false;

        if( list.find( "freeexits" ) != std::string::npos )
            options.m_freeExits = false;

        if( list.find( "ripup" ) != std::string::npos )
            options.m_ripUp = false;

        if( list.find( "conventions" ) != std::string::npos )
        {
            options.m_layers = PNS::SKETCH_ROUTER::RoutableLayers( aBoard, &iface );
            options.m_transitLayers.clear();
            options.m_viaKinds.clear();
        }
    }

    if( aRequest.m_configure )
        aRequest.m_configure( options );

    std::vector<PNS::SKETCH_CONNECTION> connections =
            PNS::SKETCH_ROUTER::CollectConnections( aBoard, router.GetWorld(), aRequest.m_items );

    result.m_connections = (int) connections.size();

    if( connections.empty() )
        return result;

    std::unique_ptr<PNS::NODE> branch( sketch.Route( router.GetWorld(), connections ) );
    result.m_stats = sketch.Stats();

    for( int index : result.m_stats.m_failed )
    {
        if( index < 0 || index >= (int) connections.size() )
            continue;

        const PNS::SKETCH_CONNECTION& conn = connections[index];
        SKETCH_ROUTE_RESULT::FAILURE  failure;

        if( NETINFO_ITEM* net = static_cast<NETINFO_ITEM*>( conn.m_net ) )
            failure.m_net = net->GetNetname();

        failure.m_start = conn.m_start ? conn.m_start->Parent() : nullptr;
        failure.m_end = conn.m_end ? conn.m_end->Parent() : nullptr;
        failure.m_startPos = conn.m_startPos;
        failure.m_endPos = conn.m_endPos;

        if( result.m_stats.m_cancelled )
            failure.m_reason = wxS( "cancelled before it was routed" );
        else if( result.m_stats.m_timedOut )
            failure.m_reason = wxS( "the time limit ran out before a route was found" );
        else if( !aRequest.m_allowVias )
            failure.m_reason = wxS( "no path keeps every clearance on one layer (vias not allowed)" );
        else
            failure.m_reason = wxS( "no path keeps every clearance, after rip-up and retry" );

        result.m_failures.push_back( failure );
    }

    if( !branch )
        return result;

    // The sketch router only ever adds items
    PNS::NODE::ITEM_VECTOR removed, added;
    branch->GetUpdatedItems( removed, added );

    for( PNS::ITEM* item : added )
    {
        NETINFO_ITEM* net = static_cast<NETINFO_ITEM*>( item->Net() );

        if( !net )
            net = NETINFO_LIST::OrphanedItem();

        BOARD_ITEM*   boardItem = nullptr;

        switch( item->Kind() )
        {
        case PNS::ITEM::SEGMENT_T:
        {
            PNS::SEGMENT* seg = static_cast<PNS::SEGMENT*>( item );
            PCB_TRACK*    track = new PCB_TRACK( aBoard );
            track->SetStart( seg->Seg().A );
            track->SetEnd( seg->Seg().B );
            track->SetWidth( seg->Width() );
            track->SetLayer( iface.GetBoardLayerFromPNSLayer( seg->Layer() ) );
            track->SetNet( net );
            boardItem = track;
            break;
        }

        case PNS::ITEM::ARC_T:
        {
            PNS::ARC* arc = static_cast<PNS::ARC*>( item );
            PCB_ARC*  pcbArc = new PCB_ARC( aBoard, static_cast<const SHAPE_ARC*>( arc->Shape( -1 ) ) );
            pcbArc->SetWidth( arc->Width() );
            pcbArc->SetLayer( iface.GetBoardLayerFromPNSLayer( arc->Layer() ) );
            pcbArc->SetNet( net );
            boardItem = pcbArc;
            break;
        }

        case PNS::ITEM::VIA_T:
        {
            PNS::VIA* via = static_cast<PNS::VIA*>( item );
            PCB_VIA*  pcbVia = nullptr;

            // Made like the board's own vias of its kind where it has them
            for( const PNS::SKETCH_VIA_KIND& kind : options.m_viaKinds )
            {
                if( kind.m_like && kind.m_like->Type() == PCB_VIA_T && (VIATYPE) kind.m_type == via->ViaType()
                        && kind.m_layerStart == via->Layers().Start() && kind.m_layerEnd == via->Layers().End()
                        && kind.m_diameter == via->Diameter( via->Layers().Start() ) && kind.m_drill == via->Drill() )
                {
                    pcbVia = static_cast<PCB_VIA*>( kind.m_like->Duplicate( false ) );
                    pcbVia->SetLocked( false );
                    pcbVia->ClearFlags();
                    break;
                }
            }

            if( pcbVia )
            {
                pcbVia->SetPosition( via->Pos() );
                pcbVia->SetNet( net );
                pcbVia->SetIsFree( via->IsFree() );
                boardItem = pcbVia;
                break;
            }

            pcbVia = new PCB_VIA( aBoard );
            pcbVia->SetPosition( via->Pos() );
            pcbVia->SetWidth( PADSTACK::TEMP_ALL_LAYERS, via->Diameter( 0 ) );
            pcbVia->SetDrill( via->Drill() );
            pcbVia->SetNet( net );
            pcbVia->SetViaType( via->ViaType() );
            pcbVia->Padstack().SetUnconnectedLayerMode( via->UnconnectedLayerMode() );
            pcbVia->SetIsFree( via->IsFree() );
            pcbVia->SetLayerPair( iface.GetBoardLayerFromPNSLayer( via->Layers().Start() ),
                                  iface.GetBoardLayerFromPNSLayer( via->Layers().End() ) );
            pcbVia->SetFrontPostMachining( via->HolePostMachining() );
            pcbVia->SetSecondaryDrillSize( via->SecondaryDrill() );

            if( std::optional<PNS_LAYER_RANGE> secondaryLayers = via->SecondaryHoleLayers() )
            {
                pcbVia->SetSecondaryDrillStartLayer(
                        iface.GetBoardLayerFromPNSLayer( secondaryLayers->Start() ) );
                pcbVia->SetSecondaryDrillEndLayer(
                        iface.GetBoardLayerFromPNSLayer( secondaryLayers->End() ) );
            }
            else
            {
                pcbVia->SetSecondaryDrillStartLayer( UNDEFINED_LAYER );
                pcbVia->SetSecondaryDrillEndLayer( UNDEFINED_LAYER );
            }

            boardItem = pcbVia;
            break;
        }

        default:
            break;
        }

        if( !boardItem )
            continue;

        if( aCommit )
            aCommit->Add( boardItem );
        else
            aBoard->Add( boardItem );

        result.m_added.push_back( boardItem );
    }

    return result;
}
