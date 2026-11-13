/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <qa_utils/wx_utils/unit_test_utils.h>

#include <board.h>
#include <board_design_settings.h>
#include <netclass.h>
#include <project/net_settings.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_shape.h>
#include <pcb_track.h>
#include <zone.h>
#include <connectivity/connectivity_data.h>
#include <drc/drc_engine.h>
#include <drc/drc_item.h>
#include <pcbnew_utils/board_file_utils.h>
#include <pcbnew_utils/board_test_utils.h>
#include <pcb_io/kicad_sexpr/pcb_io_kicad_sexpr.h>
#include <settings/settings_manager.h>

#include <router/pns_arc.h>
#include <router/pns_kicad_iface.h>
#include <router/pns_node.h>
#include <router/pns_router.h>
#include <router/pns_routing_settings.h>
#include <router/pns_segment.h>
#include <router/pns_sketch_router.h>
#include <router/sketch_route_board.h>
#include <router/pns_via.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <set>


namespace
{

struct SKETCH_ROUTE_OUTCOME
{
    PNS::SKETCH_ROUTER_STATS  m_stats;
    std::vector<BOARD_ITEM*>  m_added;
    int                       m_connections = 0;
};


/**
 * Run the sketch router headlessly over the ratsnest touching \a aItems and add the result to
 * the board.
 */
SKETCH_ROUTE_OUTCOME runSketchRouter( BOARD* aBoard, const std::vector<BOARD_CONNECTED_ITEM*>& aItems,
                                      DIRECTION_45::CORNER_MODE aCornerMode = DIRECTION_45::MITERED_45,
                                      const std::function<void( PNS::SKETCH_ROUTER_OPTIONS& )>& aConfigure = nullptr )
{
    SKETCH_ROUTE_REQUEST request;
    request.m_items = aItems;
    request.m_cornerMode = aCornerMode;
    request.m_configure =
            [&]( PNS::SKETCH_ROUTER_OPTIONS& aOptions )
            {
                if( aConfigure )
                    aConfigure( aOptions );

                // Debugging aid: limit the number of rip-up passes
                if( const char* passes = std::getenv( "SKETCH_ROUTER_PASSES" ) )
                    aOptions.m_maxPasses = atoi( passes );
            };

    SKETCH_ROUTE_RESULT result = RouteSketchOnBoard( aBoard, request, nullptr );

    SKETCH_ROUTE_OUTCOME outcome;
    outcome.m_stats = result.m_stats;
    outcome.m_added = result.m_added;
    outcome.m_connections = result.m_connections;

    // Show the lanes on a drawing layer when dumping boards for inspection
    if( std::getenv( "SKETCH_ROUTER_DUMP_DIR" ) )
    {
        for( const SHAPE_LINE_CHAIN& lane : outcome.m_stats.m_lanes )
        {
            for( int i = 0; i < lane.SegmentCount(); i++ )
            {
                PCB_SHAPE* seg = new PCB_SHAPE( aBoard, SHAPE_T::SEGMENT );
                seg->SetLayer( Dwgs_User );
                seg->SetStart( lane.CSegment( i ).A );
                seg->SetEnd( lane.CSegment( i ).B );
                seg->SetWidth( pcbIUScale.mmToIU( 0.03 ) );
                aBoard->Add( seg );
            }
        }
    }

    aBoard->BuildConnectivity();

    return outcome;
}


/// Run DRC and return the violations that involve one of \a aItems.
std::vector<DRC_ITEM> routingViolations( BOARD* aBoard, const std::vector<BOARD_ITEM*>& aItems )
{
    std::set<KIID> ids;

    for( BOARD_ITEM* item : aItems )
        ids.insert( item->m_Uuid );

    BOARD_DESIGN_SETTINGS& bds = aBoard->GetDesignSettings();

    if( !bds.m_DRCEngine )
    {
        auto engine = std::make_shared<DRC_ENGINE>( aBoard, &bds );
        engine->InitEngine( wxFileName() );
        bds.m_DRCEngine = engine;
    }

    // Routing can't be blamed for these
    for( int code : { DRCE_UNCONNECTED_ITEMS, DRCE_DANGLING_TRACK, DRCE_DANGLING_VIA,
                      DRCE_ISOLATED_COPPER, DRCE_STARVED_THERMAL, DRCE_COPPER_SLIVER,
                      DRCE_SOLDERMASK_BRIDGE, DRCE_LIB_FOOTPRINT_ISSUES, DRCE_LIB_FOOTPRINT_MISMATCH,
                      DRCE_INVALID_OUTLINE, DRCE_CREEPAGE } )
    {
        bds.m_DRCSeverities[code] = RPT_SEVERITY_IGNORE;
    }

    // Zone fills are stale once tracks are added
    if( !aBoard->Zones().empty() )
        KI_TEST::FillZones( aBoard );

    std::vector<DRC_ITEM> violations;

    bds.m_DRCEngine->SetViolationHandler(
            [&]( const std::shared_ptr<DRC_ITEM>& aItem, const VECTOR2I& aPos, int aLayer,
                 const std::function<void( PCB_MARKER* )>& aPathGenerator )
            {
                if( bds.GetSeverity( aItem->GetErrorCode() ) == RPT_SEVERITY_IGNORE )
                    return;

                if( ids.count( aItem->GetMainItemID() ) || ids.count( aItem->GetAuxItemID() ) )
                    violations.push_back( *aItem );
            } );

    bds.m_DRCEngine->RunTests( EDA_UNITS::MM, true, false );
    bds.m_DRCEngine->ClearViolationHandler();

    return violations;
}


/// Save the board for inspection when SKETCH_ROUTER_DUMP_DIR is set.
void dumpBoard( BOARD* aBoard, const std::string& aName )
{
    if( const char* dir = std::getenv( "SKETCH_ROUTER_DUMP_DIR" ) )
    {
        PCB_IO_KICAD_SEXPR io;
        io.SaveBoard( wxString( dir ) + wxT( "/" ) + wxString( aName ) + wxT( ".kicad_pcb" ), aBoard );
    }
}


void reportViolations( BOARD* aBoard, const std::vector<DRC_ITEM>& aViolations )
{
    UNITS_PROVIDER         unitsProvider( pcbIUScale, EDA_UNITS::MM );
    BOARD_RC_ITEM_RESOLVER resolver( *aBoard );

    for( const DRC_ITEM& item : aViolations )
        BOOST_TEST_MESSAGE( item.ShowReport( &unitsProvider, RPT_SEVERITY_ERROR, resolver ) );
}


/// Check that every new track runs at a multiple of 45 degrees (or 90 in orthogonal mode).
int countBadAngles( const std::vector<BOARD_ITEM*>& aItems, bool aOrthogonal )
{
    int bad = 0;

    for( BOARD_ITEM* item : aItems )
    {
        if( item->Type() != PCB_TRACE_T )
            continue;

        PCB_TRACK* track = static_cast<PCB_TRACK*>( item );
        VECTOR2I   d = track->GetEnd() - track->GetStart();

        bool ok = d.x == 0 || d.y == 0 || ( !aOrthogonal && std::abs( d.x ) == std::abs( d.y ) );

        if( !ok )
        {
            BOOST_TEST_MESSAGE( "Off-angle track on net " << track->GetNetname() << " from "
                                << track->GetStart() << " to " << track->GetEnd() );
            bad++;
        }
    }

    return bad;
}


int countVias( const std::vector<BOARD_ITEM*>& aItems )
{
    return (int) std::count_if( aItems.begin(), aItems.end(),
                                []( BOARD_ITEM* aItem )
                                {
                                    return aItem->Type() == PCB_VIA_T;
                                } );
}


std::vector<BOARD_CONNECTED_ITEM*> allPads( BOARD* aBoard )
{
    std::vector<BOARD_CONNECTED_ITEM*> items;

    for( FOOTPRINT* fp : aBoard->Footprints() )
    {
        for( PAD* pad : fp->Pads() )
            items.push_back( pad );
    }

    return items;
}


/**
 * A small synthetic board: a 40 x 30 mm two layer outline with one SMD pad pair per net, placed
 * on the left and right sides.
 */
/**
 * How far a sketch strays from the tracks that were routed along it: for points every 0.25 mm
 * along the sketch, the distance to the nearest new track.  Returns the mean and the maximum.
 */
std::pair<double, double> sketchDeviation( const SHAPE_LINE_CHAIN& aGuide, const std::vector<BOARD_ITEM*>& aItems )
{
    std::vector<SEG> tracks;

    for( BOARD_ITEM* item : aItems )
    {
        if( item->Type() == PCB_TRACE_T || item->Type() == PCB_ARC_T )
        {
            PCB_TRACK* track = static_cast<PCB_TRACK*>( item );
            tracks.emplace_back( track->GetStart(), track->GetEnd() );
        }
    }

    double sum = 0.0, worst = 0.0;
    int    samples = 0;

    for( int i = 0; i < aGuide.SegmentCount(); i++ )
    {
        const SEG seg = aGuide.CSegment( i );
        const int steps = std::max( 1, int( seg.Length() / pcbIUScale.mmToIU( 0.25 ) ) );

        for( int k = 0; k < steps; k++ )
        {
            const VECTOR2I pt = seg.A + ( seg.B - seg.A ) * k / steps;
            double         nearest = std::numeric_limits<double>::max();

            for( const SEG& track : tracks )
                nearest = std::min( nearest, (double) track.Distance( pt ) );

            sum += nearest;
            worst = std::max( worst, nearest );
            samples++;
        }
    }

    return { pcbIUScale.IUTomm( sum / std::max( 1, samples ) ), pcbIUScale.IUTomm( worst ) };
}


struct SYNTHETIC_BOARD
{
    SYNTHETIC_BOARD( int aLayers = 2 )
    {
        m_board = std::make_unique<BOARD>();
        m_board->SetCopperLayerCount( aLayers );
        m_board->SetEnabledLayers( m_board->GetEnabledLayers() | LSET::AllCuMask( aLayers )
                                   | LSET( { Edge_Cuts } ) );

        BOARD_DESIGN_SETTINGS& bds = m_board->GetDesignSettings();
        bds.m_NetSettings->GetDefaultNetclass()->SetClearance( pcbIUScale.mmToIU( 0.2 ) );
        bds.m_NetSettings->GetDefaultNetclass()->SetTrackWidth( pcbIUScale.mmToIU( 0.25 ) );
        bds.m_NetSettings->GetDefaultNetclass()->SetViaDiameter( pcbIUScale.mmToIU( 0.6 ) );
        bds.m_NetSettings->GetDefaultNetclass()->SetViaDrill( pcbIUScale.mmToIU( 0.3 ) );
        bds.m_MinClearance = pcbIUScale.mmToIU( 0.2 );

        const int w = pcbIUScale.mmToIU( 40 );
        const int h = pcbIUScale.mmToIU( 30 );

        PCB_SHAPE* outline = new PCB_SHAPE( m_board.get(), SHAPE_T::RECTANGLE );
        outline->SetLayer( Edge_Cuts );
        outline->SetStart( VECTOR2I( 0, 0 ) );
        outline->SetEnd( VECTOR2I( w, h ) );
        outline->SetWidth( pcbIUScale.mmToIU( 0.1 ) );
        m_board->Add( outline );
    }

    NETINFO_ITEM* AddNet( const wxString& aName )
    {
        NETINFO_ITEM* net = new NETINFO_ITEM( m_board.get(), aName, m_board->GetNetCount() );
        m_board->Add( net );
        return net;
    }

    FOOTPRINT* AddFootprint( const VECTOR2I& aPos )
    {
        FOOTPRINT* fp = new FOOTPRINT( m_board.get() );
        fp->SetReference( wxString::Format( wxT( "U%d" ), (int) m_board->Footprints().size() + 1 ) );
        fp->SetPosition( aPos );
        m_board->Add( fp );
        return fp;
    }

    /// Add a pad, in a footprint of its own unless \a aFootprint is given
    PAD* AddPad( const VECTOR2I& aPos, NETINFO_ITEM* aNet, bool aThroughHole = false,
                 const VECTOR2I& aSize = VECTOR2I( pcbIUScale.mmToIU( 1.0 ), pcbIUScale.mmToIU( 0.6 ) ),
                 FOOTPRINT* aFootprint = nullptr )
    {
        FOOTPRINT* fp = aFootprint ? aFootprint : AddFootprint( aPos );

        PAD* pad = new PAD( fp );
        pad->SetNumber( wxString::Format( wxT( "%d" ), (int) fp->Pads().size() + 1 ) );

        if( aThroughHole )
        {
            pad->SetAttribute( PAD_ATTRIB::PTH );
            pad->SetLayerSet( PAD::PTHMask() );
            pad->SetShape( PADSTACK::ALL_LAYERS, PAD_SHAPE::CIRCLE );
            pad->SetSize( PADSTACK::ALL_LAYERS, VECTOR2I( aSize.x, aSize.x ) );
            pad->SetDrillSize( VECTOR2I( aSize.x / 2, aSize.x / 2 ) );
        }
        else
        {
            pad->SetAttribute( PAD_ATTRIB::SMD );
            pad->SetLayerSet( PAD::SMDMask() );
            pad->SetShape( PADSTACK::ALL_LAYERS, PAD_SHAPE::RECTANGLE );
            pad->SetSize( PADSTACK::ALL_LAYERS, aSize );
        }

        pad->SetPosition( aPos );
        pad->SetNet( aNet );
        fp->Add( pad );
        return pad;
    }

    PCB_TRACK* AddTrack( const VECTOR2I& aStart, const VECTOR2I& aEnd, PCB_LAYER_ID aLayer,
                         NETINFO_ITEM* aNet, int aWidth = pcbIUScale.mmToIU( 0.5 ) )
    {
        PCB_TRACK* track = new PCB_TRACK( m_board.get() );
        track->SetStart( aStart );
        track->SetEnd( aEnd );
        track->SetLayer( aLayer );
        track->SetWidth( aWidth );
        track->SetNet( aNet );
        m_board->Add( track );
        return track;
    }

    void Finish()
    {
        // The router reads its clearances through the DRC engine
        auto engine = std::make_shared<DRC_ENGINE>( m_board.get(), &m_board->GetDesignSettings() );
        engine->InitEngine( wxFileName() );
        m_board->GetDesignSettings().m_DRCEngine = engine;

        m_board->BuildListOfNets();
        m_board->BuildConnectivity();
    }

    std::unique_ptr<BOARD> m_board;
};


VECTOR2I mm( double aX, double aY )
{
    return VECTOR2I( pcbIUScale.mmToIU( aX ), pcbIUScale.mmToIU( aY ) );
}

} // anonymous namespace


BOOST_AUTO_TEST_SUITE( PNSSketchRouter )


BOOST_AUTO_TEST_CASE( StraightConnection )
{
    SYNTHETIC_BOARD sb;
    NETINFO_ITEM*   a = sb.AddNet( wxT( "A" ) );
    PAD*            p1 = sb.AddPad( mm( 5, 15 ), a );
    PAD*            p2 = sb.AddPad( mm( 35, 15.3 ), a );
    sb.Finish();

    SKETCH_ROUTE_OUTCOME outcome = runSketchRouter( sb.m_board.get(), { p1, p2 } );

    BOOST_CHECK_EQUAL( outcome.m_connections, 1 );
    BOOST_CHECK_EQUAL( outcome.m_stats.m_routed, 1 );
    BOOST_CHECK_EQUAL( countVias( outcome.m_added ), 0 );
    BOOST_CHECK_EQUAL( countBadAngles( outcome.m_added, false ), 0 );
    BOOST_CHECK_EQUAL( sb.m_board->GetConnectivity()->GetUnconnectedCount( false ), 0 );

    dumpBoard( sb.m_board.get(), "straight" );
    std::vector<DRC_ITEM> violations = routingViolations( sb.m_board.get(), outcome.m_added );
    reportViolations( sb.m_board.get(), violations );
    BOOST_CHECK_EQUAL( violations.size(), 0 );
}


/**
 * A wall of foreign copper splits the top layer, so the only way across is a via down to the
 * bottom layer and another one back up to the SMD pad.
 */
BOOST_AUTO_TEST_CASE( WallForcesLayerChange )
{
    SYNTHETIC_BOARD sb;
    NETINFO_ITEM*   a = sb.AddNet( wxT( "A" ) );
    NETINFO_ITEM*   wall = sb.AddNet( wxT( "WALL" ) );
    PAD*            p1 = sb.AddPad( mm( 5, 15 ), a );
    PAD*            p2 = sb.AddPad( mm( 35, 12 ), a );
    sb.AddTrack( mm( 20, 0.5 ), mm( 20, 29.5 ), F_Cu, wall );
    sb.Finish();

    SKETCH_ROUTE_OUTCOME outcome = runSketchRouter( sb.m_board.get(), { p1, p2 } );

    BOOST_CHECK_EQUAL( outcome.m_stats.m_routed, 1 );
    BOOST_CHECK_EQUAL( countVias( outcome.m_added ), 2 );
    BOOST_CHECK_EQUAL( countBadAngles( outcome.m_added, false ), 0 );
    BOOST_CHECK_EQUAL( sb.m_board->GetConnectivity()->GetUnconnectedCount( false ), 0 );

    dumpBoard( sb.m_board.get(), "wall" );
    std::vector<DRC_ITEM> violations = routingViolations( sb.m_board.get(), outcome.m_added );
    reportViolations( sb.m_board.get(), violations );
    BOOST_CHECK_EQUAL( violations.size(), 0 );
}


BOOST_AUTO_TEST_CASE( NoViasWhenDisallowed )
{
    SYNTHETIC_BOARD sb;
    NETINFO_ITEM*   a = sb.AddNet( wxT( "A" ) );
    NETINFO_ITEM*   wall = sb.AddNet( wxT( "WALL" ) );
    PAD*            p1 = sb.AddPad( mm( 5, 15 ), a );
    PAD*            p2 = sb.AddPad( mm( 35, 12 ), a );
    sb.AddTrack( mm( 20, 0.5 ), mm( 20, 29.5 ), F_Cu, wall );
    sb.Finish();

    SKETCH_ROUTE_OUTCOME outcome = runSketchRouter( sb.m_board.get(), { p1, p2 }, DIRECTION_45::MITERED_45,
                                                    []( PNS::SKETCH_ROUTER_OPTIONS& aOpts )
                                                    {
                                                        aOpts.m_allowVias = false;
                                                        aOpts.m_maxPasses = 1;
                                                    } );

    BOOST_CHECK_EQUAL( outcome.m_stats.m_routed, 0 );
    BOOST_CHECK( outcome.m_added.empty() );
    BOOST_CHECK_EQUAL( outcome.m_stats.m_failed.size(), 1 );
}


/**
 * Two rows of pads with a slalom of obstacles.  In orthogonal mode every track must be horizontal
 * or vertical, and in 45 degree mode there may be diagonals but no other angles.
 */
BOOST_AUTO_TEST_CASE( CornerModes )
{
    for( DIRECTION_45::CORNER_MODE mode : { DIRECTION_45::MITERED_45, DIRECTION_45::MITERED_90,
                                            DIRECTION_45::ROUNDED_45, DIRECTION_45::ROUNDED_90 } )
    {
        BOOST_TEST_CONTEXT( "corner mode " << (int) mode )
        {
            SYNTHETIC_BOARD sb;
            std::vector<BOARD_CONNECTED_ITEM*> pads;

            for( int i = 0; i < 6; i++ )
            {
                NETINFO_ITEM* net = sb.AddNet( wxString::Format( wxT( "N%d" ), i ) );
                pads.push_back( sb.AddPad( mm( 4, 5 + 3.5 * i ), net ) );
                pads.push_back( sb.AddPad( mm( 36, 3 + 4.1 * i ), net ) );
            }

            sb.AddTrack( mm( 14, 0.5 ), mm( 14, 20 ), F_Cu, sb.AddNet( wxT( "OBSTACLE1" ) ) );
            sb.AddTrack( mm( 26, 10 ), mm( 26, 29.5 ), F_Cu, sb.AddNet( wxT( "OBSTACLE2" ) ) );
            sb.Finish();

            SKETCH_ROUTE_OUTCOME outcome = runSketchRouter( sb.m_board.get(), pads, mode );

            const bool orthogonal = mode == DIRECTION_45::MITERED_90 || mode == DIRECTION_45::ROUNDED_90;
            const bool rounded = mode == DIRECTION_45::ROUNDED_45 || mode == DIRECTION_45::ROUNDED_90;

            BOOST_CHECK_EQUAL( outcome.m_connections, 6 );
            BOOST_CHECK_EQUAL( outcome.m_stats.m_routed, 6 );
            BOOST_CHECK_EQUAL( countBadAngles( outcome.m_added, orthogonal ), 0 );

            int arcs = (int) std::count_if( outcome.m_added.begin(), outcome.m_added.end(),
                                            []( BOARD_ITEM* aItem )
                                            {
                                                return aItem->Type() == PCB_ARC_T;
                                            } );

            if( rounded )
                BOOST_CHECK_GT( arcs, 0 );
            else
                BOOST_CHECK_EQUAL( arcs, 0 );

            BOOST_CHECK_EQUAL( sb.m_board->GetConnectivity()->GetUnconnectedCount( false ), 0 );

            dumpBoard( sb.m_board.get(), "corner_mode_" + std::to_string( (int) mode ) );
            std::vector<DRC_ITEM> violations = routingViolations( sb.m_board.get(), outcome.m_added );
            reportViolations( sb.m_board.get(), violations );
            BOOST_CHECK_EQUAL( violations.size(), 0 );
        }
    }
}


/**
 * A sketch path that detours through the bottom of the board must pull the routes along with it,
 * even though the direct path is free.
 */
BOOST_AUTO_TEST_CASE( FollowsSketchPath )
{
    SYNTHETIC_BOARD sb;
    std::vector<BOARD_CONNECTED_ITEM*> pads;
    FOOTPRINT*      left = sb.AddFootprint( mm( 5, 7.25 ) );
    FOOTPRINT*      right = sb.AddFootprint( mm( 35, 7.25 ) );

    for( int i = 0; i < 4; i++ )
    {
        NETINFO_ITEM* net = sb.AddNet( wxString::Format( wxT( "B%d" ), i ) );
        pads.push_back( sb.AddPad( mm( 5, 5 + 1.5 * i ), net, false, mm( 1.0, 0.6 ), left ) );
        pads.push_back( sb.AddPad( mm( 35, 5 + 1.5 * i ), net, false, mm( 1.0, 0.6 ), right ) );
    }

    sb.Finish();

    SHAPE_LINE_CHAIN guide;
    guide.Append( mm( 8, 8 ) );
    guide.Append( mm( 12, 24 ) );
    guide.Append( mm( 28, 24 ) );
    guide.Append( mm( 32, 8 ) );

    SKETCH_ROUTE_OUTCOME outcome = runSketchRouter( sb.m_board.get(), pads, DIRECTION_45::MITERED_45,
                                                    [&]( PNS::SKETCH_ROUTER_OPTIONS& aOpts )
                                                    {
                                                        aOpts.m_guide = guide;
                                                    } );

    BOOST_CHECK_EQUAL( outcome.m_stats.m_routed, 4 );
    BOOST_CHECK_EQUAL( countBadAngles( outcome.m_added, false ), 0 );

    // Every route has to pass near the bottom of the sketch
    std::map<int, int> lowest;

    for( BOARD_ITEM* item : outcome.m_added )
    {
        if( item->Type() != PCB_TRACE_T )
            continue;

        PCB_TRACK* track = static_cast<PCB_TRACK*>( item );
        int        y = std::max( track->GetStart().y, track->GetEnd().y );
        lowest[track->GetNetCode()] = std::max( lowest[track->GetNetCode()], y );
    }

    dumpBoard( sb.m_board.get(), "follows_sketch" );

    BOOST_CHECK_EQUAL( lowest.size(), 4 );

    for( const auto& [netCode, y] : lowest )
        BOOST_CHECK_GT( y, pcbIUScale.mmToIU( 18 ) );

    std::vector<DRC_ITEM> violations = routingViolations( sb.m_board.get(), outcome.m_added );
    reportViolations( sb.m_board.get(), violations );
    BOOST_CHECK_EQUAL( violations.size(), 0 );
}


/**
 * A bus of parallel connections squeezed through a gap between two blockages.  All of them have
 * to be routed without overlapping.
 */
BOOST_AUTO_TEST_CASE( BusThroughGap )
{
    SYNTHETIC_BOARD sb;
    std::vector<BOARD_CONNECTED_ITEM*> pads;

    FOOTPRINT* left = sb.AddFootprint( mm( 4, 5.8 ) );
    FOOTPRINT* right = sb.AddFootprint( mm( 36, 22.8 ) );

    for( int i = 0; i < 8; i++ )
    {
        NETINFO_ITEM* net = sb.AddNet( wxString::Format( wxT( "D%d" ), i ) );
        pads.push_back( sb.AddPad( mm( 4, 3 + 0.8 * i ), net, false, mm( 1.2, 0.4 ), left ) );
        pads.push_back( sb.AddPad( mm( 36, 20 + 0.8 * i ), net, false, mm( 1.2, 0.4 ), right ) );
    }

    // Blockages on both layers leave an 8 mm gap in the middle
    for( PCB_LAYER_ID layer : { F_Cu, B_Cu } )
    {
        sb.AddTrack( mm( 20, 0.5 ), mm( 20, 11 ), layer, sb.AddNet( wxT( "OBSTACLE_A" ) + LayerName( layer ) ) );
        sb.AddTrack( mm( 20, 19 ), mm( 20, 29.5 ), layer, sb.AddNet( wxT( "OBSTACLE_B" ) + LayerName( layer ) ) );
    }

    sb.Finish();

    SKETCH_ROUTE_OUTCOME outcome = runSketchRouter( sb.m_board.get(), pads );

    BOOST_CHECK_EQUAL( outcome.m_connections, 8 );
    BOOST_CHECK_EQUAL( outcome.m_stats.m_routed, 8 );
    BOOST_CHECK_EQUAL( countBadAngles( outcome.m_added, false ), 0 );
    BOOST_CHECK_EQUAL( sb.m_board->GetConnectivity()->GetUnconnectedCount( false ), 0 );

    dumpBoard( sb.m_board.get(), "bus_gap" );
    std::vector<DRC_ITEM> violations = routingViolations( sb.m_board.get(), outcome.m_added );
    reportViolations( sb.m_board.get(), violations );
    BOOST_CHECK_EQUAL( violations.size(), 0 );

    BOOST_TEST_MESSAGE( "Bus routed in " << outcome.m_stats.m_elapsedMs << " ms, "
                        << outcome.m_stats.m_expansions << " expansions, "
                        << outcome.m_stats.m_collisionChecks << " collision checks" );
}


/**
 * A loopy freehand sketch (drawn by hand in pcbnew).  The bundle has to follow the loops, cutting
 * only the hairpin that is too tight for the whole bundle to turn in.
 */
BOOST_AUTO_TEST_CASE( FollowsFreehandSketch )
{
    SYNTHETIC_BOARD sb;
    std::vector<BOARD_CONNECTED_ITEM*> pads;
    FOOTPRINT*      left = sb.AddFootprint( mm( 4, 5.8 ) );
    FOOTPRINT*      right = sb.AddFootprint( mm( 36, 22.8 ) );

    for( int i = 0; i < 8; i++ )
    {
        NETINFO_ITEM* net = sb.AddNet( wxString::Format( wxT( "D%d" ), i ) );
        pads.push_back( sb.AddPad( mm( 4, 3 + 0.8 * i ), net, false, mm( 1.2, 0.4 ), left ) );
        pads.push_back( sb.AddPad( mm( 36, 20 + 0.8 * i ), net, false, mm( 1.2, 0.4 ), right ) );
    }

    for( PCB_LAYER_ID layer : { F_Cu, B_Cu } )
    {
        sb.AddTrack( mm( 20, 0.5 ), mm( 20, 11 ), layer, sb.AddNet( wxT( "OBSTACLE_A" ) + LayerName( layer ) ) );
        sb.AddTrack( mm( 20, 19 ), mm( 20, 29.5 ), layer, sb.AddNet( wxT( "OBSTACLE_B" ) + LayerName( layer ) ) );
    }

    sb.Finish();

    // Traced from a screenshot at 27.1 px/mm, board origin at (60, 49) px
    static const int sketch[][2] = {
        { 230, 213 }, { 290, 225 }, { 340, 250 }, { 355, 280 }, { 350, 310 }, { 330, 345 }, { 300, 380 },
        { 275, 430 }, { 268, 480 }, { 280, 530 }, { 320, 560 }, { 400, 563 }, { 460, 555 }, { 500, 530 },
        { 520, 490 }, { 500, 460 }, { 460, 410 }, { 425, 378 }, { 480, 375 }, { 520, 395 }, { 535, 430 },
        { 545, 470 }, { 570, 475 }, { 600, 455 }, { 620, 430 }, { 650, 422 }, { 670, 435 }, { 685, 460 },
        { 710, 450 }, { 740, 410 }, { 775, 345 }, { 778, 300 }, { 760, 245 }, { 760, 210 }, { 790, 192 },
        { 820, 190 }, { 855, 203 }, { 885, 250 }, { 910, 330 }, { 900, 390 }, { 870, 440 }, { 845, 500 },
        { 840, 550 }, { 860, 590 }, { 900, 613 }, { 940, 622 }
    };

    SHAPE_LINE_CHAIN guide;

    for( const auto& pt : sketch )
        guide.Append( mm( ( pt[0] - 60 ) / 27.1, ( pt[1] - 49 ) / 27.1 ) );

    SKETCH_ROUTE_OUTCOME outcome = runSketchRouter( sb.m_board.get(), pads, DIRECTION_45::MITERED_45,
                                                    [&]( PNS::SKETCH_ROUTER_OPTIONS& aOpts )
                                                    {
                                                        aOpts.m_guide = guide;
                                                    } );

    const auto [meanDev, maxDev] = sketchDeviation( guide, outcome.m_added );

    BOOST_TEST_MESSAGE( "Freehand sketch: " << outcome.m_stats.m_routed << " routed, "
                        << outcome.m_stats.m_vias << " vias in " << outcome.m_stats.m_elapsedMs
                        << " ms; sketch to tracks mean " << meanDev << " mm, max " << maxDev << " mm" );

    BOOST_CHECK_EQUAL( outcome.m_stats.m_routed, 8 );
    BOOST_CHECK_EQUAL( countBadAngles( outcome.m_added, false ), 0 );
    BOOST_CHECK_EQUAL( sb.m_board->GetConnectivity()->GetUnconnectedCount( false ), 0 );
    BOOST_CHECK_EQUAL( countVias( outcome.m_added ), 0 );
    BOOST_CHECK_LT( meanDev, 0.6 );

    std::vector<DRC_ITEM> violations = routingViolations( sb.m_board.get(), outcome.m_added );
    reportViolations( sb.m_board.get(), violations );
    BOOST_CHECK_EQUAL( violations.size(), 0 );

    // The sketch as drawn, for comparison
    for( int i = 0; i < guide.SegmentCount(); i++ )
    {
        PCB_SHAPE* seg = new PCB_SHAPE( sb.m_board.get(), SHAPE_T::SEGMENT );
        seg->SetLayer( Cmts_User );
        seg->SetStart( guide.CSegment( i ).A );
        seg->SetEnd( guide.CSegment( i ).B );
        seg->SetWidth( pcbIUScale.mmToIU( 0.05 ) );
        sb.m_board->Add( seg );
    }

    dumpBoard( sb.m_board.get(), "freehand_sketch" );
}


/**
 * A sketch drawn straight through an obstacle.  The bundle has to keep together and go round the
 * obstacle as close to the sketch as it can, instead of breaking up into individual routes.
 */
BOOST_AUTO_TEST_CASE( SketchThroughObstacle )
{
    SYNTHETIC_BOARD sb;
    std::vector<BOARD_CONNECTED_ITEM*> pads;
    FOOTPRINT*      left = sb.AddFootprint( mm( 4, 5.8 ) );
    FOOTPRINT*      right = sb.AddFootprint( mm( 36, 22.8 ) );

    for( int i = 0; i < 8; i++ )
    {
        NETINFO_ITEM* net = sb.AddNet( wxString::Format( wxT( "D%d" ), i ) );
        pads.push_back( sb.AddPad( mm( 4, 3 + 0.8 * i ), net, false, mm( 1.2, 0.4 ), left ) );
        pads.push_back( sb.AddPad( mm( 36, 20 + 0.8 * i ), net, false, mm( 1.2, 0.4 ), right ) );
    }

    for( PCB_LAYER_ID layer : { F_Cu, B_Cu } )
    {
        sb.AddTrack( mm( 20, 0.5 ), mm( 20, 11 ), layer, sb.AddNet( wxT( "OBSTACLE_A" ) + LayerName( layer ) ) );
        sb.AddTrack( mm( 20, 19 ), mm( 20, 29.5 ), layer, sb.AddNet( wxT( "OBSTACLE_B" ) + LayerName( layer ) ) );
    }

    sb.Finish();

    // Straight through the upper blockage, well above the gap
    SHAPE_LINE_CHAIN guide;
    guide.Append( mm( 8, 6 ) );
    guide.Append( mm( 20, 8 ) );
    guide.Append( mm( 32, 20 ) );

    SKETCH_ROUTE_OUTCOME outcome = runSketchRouter( sb.m_board.get(), pads, DIRECTION_45::MITERED_45,
                                                    [&]( PNS::SKETCH_ROUTER_OPTIONS& aOpts )
                                                    {
                                                        aOpts.m_guide = guide;
                                                    } );

    BOOST_TEST_MESSAGE( "Sketch through obstacle: " << outcome.m_stats.m_routed << " routed, "
                        << outcome.m_stats.m_vias << " vias in " << outcome.m_stats.m_elapsedMs << " ms" );

    BOOST_CHECK_EQUAL( outcome.m_stats.m_routed, 8 );
    BOOST_CHECK_LE( countVias( outcome.m_added ), 2 );
    BOOST_CHECK_EQUAL( countBadAngles( outcome.m_added, false ), 0 );
    BOOST_CHECK_EQUAL( sb.m_board->GetConnectivity()->GetUnconnectedCount( false ), 0 );

    dumpBoard( sb.m_board.get(), "sketch_through_obstacle" );

    std::vector<DRC_ITEM> violations = routingViolations( sb.m_board.get(), outcome.m_added );
    reportViolations( sb.m_board.get(), violations );
    BOOST_CHECK_EQUAL( violations.size(), 0 );
}


/**
 * A rule area forbidding tracks sits between the pads; the route has to go around it.
 */
BOOST_AUTO_TEST_CASE( KeepoutAvoided )
{
    SYNTHETIC_BOARD sb;
    NETINFO_ITEM*   a = sb.AddNet( wxT( "A" ) );
    PAD*            p1 = sb.AddPad( mm( 5, 15 ), a );
    PAD*            p2 = sb.AddPad( mm( 35, 15 ), a );

    ZONE* keepout = new ZONE( sb.m_board.get() );
    keepout->SetIsRuleArea( true );
    keepout->SetDoNotAllowTracks( true );
    keepout->SetDoNotAllowVias( true );
    keepout->SetLayerSet( LSET::AllCuMask( 2 ) );
    keepout->Outline()->NewOutline();
    keepout->Outline()->Append( mm( 15, 4 ) );
    keepout->Outline()->Append( mm( 25, 4 ) );
    keepout->Outline()->Append( mm( 25, 26 ) );
    keepout->Outline()->Append( mm( 15, 26 ) );
    sb.m_board->Add( keepout );
    sb.Finish();

    SKETCH_ROUTE_OUTCOME outcome = runSketchRouter( sb.m_board.get(), { p1, p2 } );

    BOOST_CHECK_EQUAL( outcome.m_stats.m_routed, 1 );

    const BOX2I box( mm( 15, 4 ), mm( 10, 22 ) );

    for( BOARD_ITEM* item : outcome.m_added )
    {
        if( item->Type() == PCB_TRACE_T )
        {
            PCB_TRACK* track = static_cast<PCB_TRACK*>( item );
            BOOST_CHECK( !box.Intersects( track->GetStart(), track->GetEnd() ) );
        }
    }

    dumpBoard( sb.m_board.get(), "keepout" );

    std::vector<DRC_ITEM> violations = routingViolations( sb.m_board.get(), outcome.m_added );
    reportViolations( sb.m_board.get(), violations );
    BOOST_CHECK_EQUAL( violations.size(), 0 );
}


/**
 * On a four layer board with power planes inside, a layer change has to go all the way to the
 * bottom layer.  Nothing may be routed on the planes.
 */
BOOST_AUTO_TEST_CASE( PlaneLayersSkipped )
{
    SYNTHETIC_BOARD sb( 4 );
    sb.m_board->SetLayerType( In1_Cu, LT_POWER );
    sb.m_board->SetLayerType( In2_Cu, LT_POWER );

    NETINFO_ITEM* a = sb.AddNet( wxT( "A" ) );
    PAD*          p1 = sb.AddPad( mm( 5, 15 ), a );
    PAD*          p2 = sb.AddPad( mm( 35, 12 ), a );
    sb.AddTrack( mm( 20, 0.5 ), mm( 20, 29.5 ), F_Cu, sb.AddNet( wxT( "WALL" ) ) );
    sb.Finish();

    SKETCH_ROUTE_OUTCOME outcome = runSketchRouter( sb.m_board.get(), { p1, p2 } );

    BOOST_CHECK_EQUAL( outcome.m_stats.m_routed, 1 );
    BOOST_CHECK_EQUAL( countVias( outcome.m_added ), 2 );

    for( BOARD_ITEM* item : outcome.m_added )
    {
        if( item->Type() == PCB_TRACE_T )
            BOOST_CHECK( item->GetLayer() == F_Cu || item->GetLayer() == B_Cu );
    }

    std::vector<DRC_ITEM> violations = routingViolations( sb.m_board.get(), outcome.m_added );
    reportViolations( sb.m_board.get(), violations );
    BOOST_CHECK_EQUAL( violations.size(), 0 );
}


/**
 * Through hole pads are on every layer, so a wall on the top layer is passed underneath
 * without any via.
 */
BOOST_AUTO_TEST_CASE( ThroughHolePadsNeedNoVias )
{
    SYNTHETIC_BOARD sb;
    NETINFO_ITEM*   a = sb.AddNet( wxT( "A" ) );
    PAD*            p1 = sb.AddPad( mm( 5, 15 ), a, true, mm( 1.6, 1.6 ) );
    PAD*            p2 = sb.AddPad( mm( 35, 12 ), a, true, mm( 1.6, 1.6 ) );
    sb.AddTrack( mm( 20, 0.5 ), mm( 20, 29.5 ), F_Cu, sb.AddNet( wxT( "WALL" ) ) );
    sb.Finish();

    SKETCH_ROUTE_OUTCOME outcome = runSketchRouter( sb.m_board.get(), { p1, p2 } );

    BOOST_CHECK_EQUAL( outcome.m_stats.m_routed, 1 );
    BOOST_CHECK_EQUAL( countVias( outcome.m_added ), 0 );
    BOOST_CHECK_EQUAL( sb.m_board->GetConnectivity()->GetUnconnectedCount( false ), 0 );

    std::vector<DRC_ITEM> violations = routingViolations( sb.m_board.get(), outcome.m_added );
    reportViolations( sb.m_board.get(), violations );
    BOOST_CHECK_EQUAL( violations.size(), 0 );
}


/**
 * Track width comes from the net's netclass.
 */
BOOST_AUTO_TEST_CASE( NetclassWidth )
{
    SYNTHETIC_BOARD sb;

    std::shared_ptr<NET_SETTINGS> netSettings = sb.m_board->GetDesignSettings().m_NetSettings;
    std::shared_ptr<NETCLASS>     wide = std::make_shared<NETCLASS>( wxT( "Wide" ) );
    wide->SetTrackWidth( pcbIUScale.mmToIU( 0.6 ) );
    wide->SetClearance( pcbIUScale.mmToIU( 0.3 ) );
    netSettings->SetNetclass( wxT( "Wide" ), wide );
    netSettings->SetNetclassLabelAssignment( wxT( "PWR1" ), { wxT( "Wide" ) } );

    NETINFO_ITEM* power = sb.AddNet( wxT( "PWR1" ) );
    NETINFO_ITEM* signal = sb.AddNet( wxT( "SIG1" ) );

    std::vector<BOARD_CONNECTED_ITEM*> pads = { sb.AddPad( mm( 5, 10 ), power ),
                                                sb.AddPad( mm( 35, 10 ), power ),
                                                sb.AddPad( mm( 5, 20 ), signal ),
                                                sb.AddPad( mm( 35, 20 ), signal ) };

    sb.Finish();

    // Without a project the board doesn't resolve netclasses itself
    netSettings->ClearAllCaches();
    power->SetNetClass( netSettings->GetEffectiveNetClass( power->GetNetname() ) );
    signal->SetNetClass( netSettings->GetEffectiveNetClass( signal->GetNetname() ) );

    BOOST_REQUIRE( power->GetNetClass()->GetName() == wxT( "Wide" ) );

    SKETCH_ROUTE_OUTCOME outcome = runSketchRouter( sb.m_board.get(), pads );

    BOOST_CHECK_EQUAL( outcome.m_stats.m_routed, 2 );

    for( BOARD_ITEM* item : outcome.m_added )
    {
        if( item->Type() != PCB_TRACE_T )
            continue;

        PCB_TRACK* track = static_cast<PCB_TRACK*>( item );

        if( track->GetNetname() == wxT( "PWR1" ) )
            BOOST_CHECK_EQUAL( track->GetWidth(), pcbIUScale.mmToIU( 0.6 ) );
        else
            BOOST_CHECK_EQUAL( track->GetWidth(), pcbIUScale.mmToIU( 0.25 ) );
    }

    std::vector<DRC_ITEM> violations = routingViolations( sb.m_board.get(), outcome.m_added );
    reportViolations( sb.m_board.get(), violations );
    BOOST_CHECK_EQUAL( violations.size(), 0 );
}


/**
 * Pads of an IC are left straight out, away from the body, clear of the pad before any bend.
 */
BOOST_AUTO_TEST_CASE( StraightPadExits )
{
    SYNTHETIC_BOARD sb;
    FOOTPRINT*      ic = sb.AddFootprint( mm( 8, 12 ) );
    FOOTPRINT*      conn = sb.AddFootprint( mm( 32, 18 ) );
    std::vector<BOARD_CONNECTED_ITEM*> pads;
    std::vector<PAD*> icPads;

    for( int i = 0; i < 6; i++ )
    {
        NETINFO_ITEM* net = sb.AddNet( wxString::Format( wxT( "S%d" ), i ) );

        // Pins on the right side of the IC, long in x; the IC body is to their left
        icPads.push_back( sb.AddPad( mm( 10, 10 + 0.8 * i ), net, false, mm( 1.5, 0.4 ), ic ) );
        pads.push_back( icPads.back() );
        pads.push_back( sb.AddPad( mm( 32, 16 + 0.8 * i ), net, false, mm( 1.5, 0.4 ), conn ) );
    }

    // Pins on the left side of the IC so it has a body between them
    for( int i = 0; i < 6; i++ )
        sb.AddPad( mm( 6, 10 + 0.8 * i ), nullptr, false, mm( 1.5, 0.4 ), ic );

    sb.Finish();

    SKETCH_ROUTE_OUTCOME outcome = runSketchRouter( sb.m_board.get(), pads );

    BOOST_CHECK_EQUAL( outcome.m_stats.m_routed, 6 );

    // Each IC pin's track starts with a straight stretch heading out (+x), at least as long as
    // the half pad plus the track width
    for( PAD* pad : icPads )
    {
        bool found = false;

        for( BOARD_ITEM* item : outcome.m_added )
        {
            if( item->Type() != PCB_TRACE_T )
                continue;

            PCB_TRACK* track = static_cast<PCB_TRACK*>( item );
            VECTOR2I   start = track->GetStart();
            VECTOR2I   end = track->GetEnd();

            if( end == pad->GetPosition() )
                std::swap( start, end );

            if( start != pad->GetPosition() )
                continue;

            found = true;
            BOOST_CHECK_EQUAL( end.y, start.y );
            BOOST_CHECK_GE( end.x - start.x, pcbIUScale.mmToIU( 0.75 + 0.25 ) - 1 );
        }

        BOOST_CHECK_MESSAGE( found, "no track leaves pad " << pad->GetNumber() );
    }

    dumpBoard( sb.m_board.get(), "pad_exits" );

    std::vector<DRC_ITEM> violations = routingViolations( sb.m_board.get(), outcome.m_added );
    reportViolations( sb.m_board.get(), violations );
    BOOST_CHECK_EQUAL( violations.size(), 0 );
}


/**
 * A sketched bundle in orthogonal corner mode: only horizontal and vertical tracks.
 */
BOOST_AUTO_TEST_CASE( OrthogonalSketch )
{
    SYNTHETIC_BOARD sb;
    std::vector<BOARD_CONNECTED_ITEM*> pads;
    FOOTPRINT*      left = sb.AddFootprint( mm( 5, 7.25 ) );
    FOOTPRINT*      right = sb.AddFootprint( mm( 35, 7.25 ) );

    for( int i = 0; i < 4; i++ )
    {
        NETINFO_ITEM* net = sb.AddNet( wxString::Format( wxT( "O%d" ), i ) );
        pads.push_back( sb.AddPad( mm( 5, 5 + 1.5 * i ), net, false, mm( 1.0, 0.6 ), left ) );
        pads.push_back( sb.AddPad( mm( 35, 5 + 1.5 * i ), net, false, mm( 1.0, 0.6 ), right ) );
    }

    sb.Finish();

    SHAPE_LINE_CHAIN guide;
    guide.Append( mm( 9, 7 ) );
    guide.Append( mm( 9, 24 ) );
    guide.Append( mm( 31, 24 ) );
    guide.Append( mm( 31, 7 ) );

    SKETCH_ROUTE_OUTCOME outcome = runSketchRouter( sb.m_board.get(), pads, DIRECTION_45::MITERED_90,
                                                    [&]( PNS::SKETCH_ROUTER_OPTIONS& aOpts )
                                                    {
                                                        aOpts.m_guide = guide;
                                                    } );

    BOOST_CHECK_EQUAL( outcome.m_stats.m_routed, 4 );
    BOOST_CHECK_EQUAL( countBadAngles( outcome.m_added, true ), 0 );
    BOOST_CHECK_EQUAL( countVias( outcome.m_added ), 0 );

    dumpBoard( sb.m_board.get(), "orthogonal_sketch" );

    std::vector<DRC_ITEM> violations = routingViolations( sb.m_board.get(), outcome.m_added );
    reportViolations( sb.m_board.get(), violations );
    BOOST_CHECK_EQUAL( violations.size(), 0 );
}


/**
 * Running out of time stops routing cleanly and keeps whatever was routed.
 */
BOOST_AUTO_TEST_CASE( TimeLimit )
{
    SYNTHETIC_BOARD sb;
    std::vector<BOARD_CONNECTED_ITEM*> pads;

    for( int i = 0; i < 20; i++ )
    {
        NETINFO_ITEM* net = sb.AddNet( wxString::Format( wxT( "T%d" ), i ) );
        pads.push_back( sb.AddPad( mm( 4, 2 + 1.3 * i ), net ) );
        pads.push_back( sb.AddPad( mm( 36, 28 - 1.3 * i ), net ) );
    }

    sb.Finish();

    SKETCH_ROUTE_OUTCOME outcome = runSketchRouter( sb.m_board.get(), pads, DIRECTION_45::MITERED_45,
                                                    []( PNS::SKETCH_ROUTER_OPTIONS& aOpts )
                                                    {
                                                        aOpts.m_timeLimitMs = 1;
                                                    } );

    BOOST_CHECK_EQUAL( outcome.m_connections, 20 );
    BOOST_CHECK_LE( outcome.m_stats.m_routed, 20 );
    BOOST_CHECK_LT( outcome.m_stats.m_elapsedMs, 1000.0 );

    std::vector<DRC_ITEM> violations = routingViolations( sb.m_board.get(), outcome.m_added );
    BOOST_CHECK_EQUAL( violations.size(), 0 );
}


/**
 * Strip the routing from a real board and route it again.  Everything the router produces must
 * pass DRC and follow the 45 degree rules.
 */
BOOST_AUTO_TEST_CASE( RealBoard )
{
    SETTINGS_MANAGER       settingsManager;
    std::unique_ptr<BOARD> board;

    KI_TEST::LoadBoard( settingsManager, "../../../demos/stickhub/StickHub", board );
    BOOST_REQUIRE( board );

    std::vector<BOARD_ITEM*> toRemove;

    for( PCB_TRACK* track : board->Tracks() )
        toRemove.push_back( track );

    for( BOARD_ITEM* item : toRemove )
    {
        board->Remove( item );
        delete item;
    }

    board->BuildConnectivity();

    const int unconnectedBefore = board->GetConnectivity()->GetUnconnectedCount( false );

    SKETCH_ROUTE_OUTCOME outcome = runSketchRouter( board.get(), allPads( board.get() ) );

    const int unconnectedAfter = board->GetConnectivity()->GetUnconnectedCount( false );

    BOOST_TEST_MESSAGE( "StickHub: routed " << outcome.m_stats.m_routed << " of "
                        << outcome.m_connections << " connections in "
                        << outcome.m_stats.m_elapsedMs << " ms ("
                        << outcome.m_stats.m_passes << " passes, "
                        << outcome.m_stats.m_vias << " vias, "
                        << outcome.m_stats.m_expansions << " expansions, "
                        << outcome.m_stats.m_collisionChecks << " collision checks); unconnected "
                        << unconnectedBefore << " -> " << unconnectedAfter );

    BOOST_CHECK_GT( outcome.m_connections, 0 );
    BOOST_CHECK_GE( outcome.m_stats.m_routed * 10, outcome.m_connections * 8 );
    BOOST_CHECK_LT( unconnectedAfter, unconnectedBefore );
    BOOST_CHECK_EQUAL( countBadAngles( outcome.m_added, false ), 0 );

    dumpBoard( board.get(), "stickhub" );
    std::vector<DRC_ITEM> violations = routingViolations( board.get(), outcome.m_added );
    reportViolations( board.get(), violations );
    BOOST_CHECK_EQUAL( violations.size(), 0 );
}


/**
 * Benchmark hook: set SKETCH_ROUTER_BOARD to a .kicad_pcb path to strip and reroute that board.
 * SKETCH_ROUTER_OUT saves the result.  Does nothing when the variable is not set.
 */
BOOST_AUTO_TEST_CASE( BenchmarkBoard )
{
    const char* path = std::getenv( "SKETCH_ROUTER_BOARD" );

    if( !path )
    {
        BOOST_CHECK( true );
        return;
    }

    std::unique_ptr<BOARD> board = KI_TEST::ReadBoardFromFileOrStream( path );
    BOOST_REQUIRE( board );

    // The board file alone has no netclasses; SKETCH_ROUTER_RULES="track,clearance,via,drill" (mm)
    // sets the default netclass's
    if( const char* rules = std::getenv( "SKETCH_ROUTER_RULES" ) )
    {
        double track = 0, clearance = 0, via = 0, drill = 0;

        if( sscanf( rules, "%lf,%lf,%lf,%lf", &track, &clearance, &via, &drill ) == 4 )
        {
            std::shared_ptr<NETCLASS> nc = board->GetDesignSettings().m_NetSettings->GetDefaultNetclass();
            nc->SetTrackWidth( pcbIUScale.mmToIU( track ) );
            nc->SetClearance( pcbIUScale.mmToIU( clearance ) );
            nc->SetViaDiameter( pcbIUScale.mmToIU( via ) );
            nc->SetViaDrill( pcbIUScale.mmToIU( drill ) );
        }
    }

    auto engine = std::make_shared<DRC_ENGINE>( board.get(), &board->GetDesignSettings() );
    engine->InitEngine( wxFileName() );
    board->GetDesignSettings().m_DRCEngine = engine;
    board->BuildListOfNets();

    std::vector<BOARD_ITEM*> toRemove;

    if( !std::getenv( "SKETCH_ROUTER_KEEP_TRACKS" ) )
    {
        for( PCB_TRACK* track : board->Tracks() )
            toRemove.push_back( track );
    }

    for( BOARD_ITEM* item : toRemove )
    {
        board->Remove( item );
        delete item;
    }

    board->BuildConnectivity();

    const int unconnectedBefore = board->GetConnectivity()->GetUnconnectedCount( false );

    int timeLimit = std::getenv( "SKETCH_ROUTER_TIME_MS" ) ? atoi( std::getenv( "SKETCH_ROUTER_TIME_MS" ) )
                                                         : 120000;

    SKETCH_ROUTE_OUTCOME outcome = runSketchRouter( board.get(), allPads( board.get() ),
                                                    DIRECTION_45::MITERED_45,
                                                    [&]( PNS::SKETCH_ROUTER_OPTIONS& aOpts )
                                                    {
                                                        aOpts.m_timeLimitMs = timeLimit;
                                                    } );

    const int unconnectedAfter = board->GetConnectivity()->GetUnconnectedCount( false );

    BOOST_TEST_MESSAGE( path << ": routed " << outcome.m_stats.m_routed << " of "
                        << outcome.m_connections << " connections in "
                        << outcome.m_stats.m_elapsedMs << " ms ("
                        << outcome.m_stats.m_passes << " passes, "
                        << outcome.m_stats.m_vias << " vias, "
                        << outcome.m_stats.m_expansions << " expansions, "
                        << outcome.m_stats.m_collisionChecks << " collision checks); unconnected "
                        << unconnectedBefore << " -> " << unconnectedAfter );

    BOOST_CHECK_EQUAL( countBadAngles( outcome.m_added, false ), 0 );

    std::vector<DRC_ITEM> violations = routingViolations( board.get(), outcome.m_added );
    reportViolations( board.get(), violations );
    BOOST_CHECK_EQUAL( violations.size(), 0 );

    if( const char* out = std::getenv( "SKETCH_ROUTER_OUT" ) )
    {
        PCB_IO_KICAD_SEXPR io;
        io.SaveBoard( out, board.get() );
    }
}


BOOST_AUTO_TEST_SUITE_END()
