/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers.
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

#include <common.h>
#include <board_design_settings.h>
#include <pad.h>
#include <pcb_board_outline.h>
#include <footprint.h>
#include <thread_pool.h>
#include <zone.h>
#include <connectivity/connectivity_data.h>
#include <drc/drc_engine.h>
#include <drc/drc_rtree.h>
#include <drc/drc_cache_generator.h>
#include <drc/drc_zone_triangulation.h>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>


namespace
{
/**
 * Validate a zone fill's triangulation against its outline (enabled by KICAD_DRC_CHECK_TRIANGULATION).
 *
 * DRC measures zone clearances against the fill's triangles, so they must cover exactly the
 * fill: their summed area must equal the polygon's, and no outline vertex may lie inside a
 * triangle (an ear that swallowed vertices covers area outside the fill).  Prints one line per
 * zone layer to stderr: polygon and triangle areas (mm^2), their relative difference, and the
 * number and worst depth (nm) of outline vertices found inside triangles.
 */
void checkZoneTriangulation( ZONE* aZone )
{
    aZone->GetLayerSet().RunOnLayers(
            [&]( PCB_LAYER_ID layer )
            {
                if( !IsCopperLayer( layer ) )
                    return;

                std::shared_ptr<SHAPE_POLY_SET> fill = aZone->GetFilledPolysList( layer );

                if( !fill || fill->OutlineCount() == 0 )
                    return;

                auto chainArea =
                        []( const SHAPE_LINE_CHAIN& aChain )
                        {
                            double area = 0.0;
                            int    n = aChain.PointCount();

                            for( int ii = 0; ii < n; ++ii )
                            {
                                const VECTOR2I& a = aChain.CPoint( ii );
                                const VECTOR2I& b = aChain.CPoint( ( ii + 1 ) % n );
                                area += (double) a.x * b.y - (double) b.x * a.y;
                            }

                            return std::abs( area ) / 2.0;
                        };

                double                  polyArea = 0.0;
                std::vector<VECTOR2I>   vertices;

                for( int ii = 0; ii < fill->OutlineCount(); ++ii )
                {
                    polyArea += chainArea( fill->COutline( ii ) );

                    for( int jj = 0; jj < fill->HoleCount( ii ); ++jj )
                        polyArea -= chainArea( fill->CHole( ii, jj ) );

                    for( const VECTOR2I& pt : fill->COutline( ii ).CPoints() )
                        vertices.push_back( pt );

                    for( int jj = 0; jj < fill->HoleCount( ii ); ++jj )
                    {
                        for( const VECTOR2I& pt : fill->CHole( ii, jj ).CPoints() )
                            vertices.push_back( pt );
                    }
                }

                struct TRIANGLE { VECTOR2I a, b, c; BOX2I box; };
                std::vector<TRIANGLE> triangles;
                double                triArea = 0.0;

                for( unsigned ii = 0; ii < fill->TriangulatedPolyCount(); ++ii )
                {
                    const SHAPE_POLY_SET::TRIANGULATED_POLYGON* tp = fill->TriangulatedPolygon( ii );

                    for( size_t jj = 0; jj < tp->GetTriangleCount(); ++jj )
                    {
                        TRIANGLE t;
                        tp->GetTriangle( jj, t.a, t.b, t.c );
                        triArea += std::abs( (double) ( t.b.x - t.a.x ) * ( t.c.y - t.a.y )
                                             - (double) ( t.c.x - t.a.x ) * ( t.b.y - t.a.y ) ) / 2.0;
                        t.box = BOX2I( t.a, VECTOR2I( 0, 0 ) );
                        t.box.Merge( t.b );
                        t.box.Merge( t.c );
                        triangles.push_back( t );
                    }
                }

                // Uniform grid over the triangles' boxes, so each vertex is tested against the
                // few triangles near it
                BOX2I extent = fill->BBox();
                int   cells = std::max( 1, (int) std::sqrt( (double) triangles.size() ) );
                double cw = std::max( 1.0, (double) extent.GetWidth() / cells );
                double ch = std::max( 1.0, (double) extent.GetHeight() / cells );
                std::vector<std::vector<int>> grid( (size_t) cells * cells );

                auto cellOf =
                        [&]( int x, int y, int& cx, int& cy )
                        {
                            cx = std::clamp( (int) ( ( x - extent.GetX() ) / cw ), 0, cells - 1 );
                            cy = std::clamp( (int) ( ( y - extent.GetY() ) / ch ), 0, cells - 1 );
                        };

                for( size_t ii = 0; ii < triangles.size(); ++ii )
                {
                    int x0, y0, x1, y1;
                    cellOf( triangles[ii].box.GetX(), triangles[ii].box.GetY(), x0, y0 );
                    cellOf( triangles[ii].box.GetRight(), triangles[ii].box.GetBottom(), x1, y1 );

                    for( int gx = x0; gx <= x1; ++gx )
                    {
                        for( int gy = y0; gy <= y1; ++gy )
                            grid[(size_t) gy * cells + gx].push_back( (int) ii );
                    }
                }

                // A vertex strictly inside a triangle by more than the triangulator's own
                // simplification distance (it drops points closer than 50 nm)
                const double minDepth = 100.0;
                int          inside = 0;
                double       worst = 0.0;
                VECTOR2I     worstPt;

                for( const VECTOR2I& v : vertices )
                {
                    int gx, gy;
                    cellOf( v.x, v.y, gx, gy );

                    for( int idx : grid[(size_t) gy * cells + gx] )
                    {
                        const TRIANGLE& t = triangles[idx];

                        if( !t.box.Contains( v ) )
                            continue;

                        auto edgeDepth =
                                []( const VECTOR2I& p, const VECTOR2I& a, const VECTOR2I& b )
                                {
                                    double cross = (double) ( b.x - a.x ) * ( p.y - a.y )
                                                   - (double) ( b.y - a.y ) * ( p.x - a.x );
                                    double len = std::hypot( (double) ( b.x - a.x ), (double) ( b.y - a.y ) );
                                    return len > 0 ? cross / len : 0.0;
                                };

                        double d1 = edgeDepth( v, t.a, t.b );
                        double d2 = edgeDepth( v, t.b, t.c );
                        double d3 = edgeDepth( v, t.c, t.a );
                        double depth = 0.0;

                        if( d1 > 0 && d2 > 0 && d3 > 0 )
                            depth = std::min( { d1, d2, d3 } );
                        else if( d1 < 0 && d2 < 0 && d3 < 0 )
                            depth = std::min( { -d1, -d2, -d3 } );

                        if( depth > minDepth )
                        {
                            inside++;

                            if( depth > worst )
                            {
                                worst = depth;
                                worstPt = v;
                            }
                        }
                    }
                }

                double rel = polyArea > 0 ? ( triArea - polyArea ) / polyArea : 0.0;

                // Where they differ: the fill minus the triangles (copper DRC cannot see) and the
                // triangles minus the fill (copper DRC sees that is not there), each eroded by
                // 100 nm so the triangulator's sub-50 nm simplification does not count
                SHAPE_POLY_SET triSet;

                for( const TRIANGLE& t : triangles )
                {
                    SHAPE_LINE_CHAIN chain( { t.a, t.b, t.c } );
                    chain.SetClosed( true );
                    triSet.AddOutline( chain );
                }

                triSet.Simplify();

                // The fill as one union (resolving the zero-width bridges of its fractured form)
                SHAPE_POLY_SET fillCopy = *fill;
                fillCopy.ClearArcs();
                fillCopy.Simplify();

                SHAPE_POLY_SET uncovered;
                uncovered.BooleanSubtract( fillCopy, triSet );
                uncovered.Deflate( 100, CORNER_STRATEGY::CHAMFER_ALL_CORNERS, 10 );

                SHAPE_POLY_SET overcovered;
                overcovered.BooleanSubtract( triSet, fillCopy );
                overcovered.Deflate( 100, CORNER_STRATEGY::CHAMFER_ALL_CORNERS, 10 );

                VECTOR2I underAt = uncovered.OutlineCount() ? uncovered.COutline( 0 ).CPoint( 0 ) : VECTOR2I();

                // A point deep inside the uncovered copper, for probing: erode until nothing is left
                // and keep a vertex of the last non-empty step
                VECTOR2I deepAt = underAt;
                int      depth = 100;

                for( int step = 200; uncovered.OutlineCount() && step < 5000000; step *= 2 )
                {
                    SHAPE_POLY_SET eroded = uncovered;
                    eroded.Deflate( step / 2, CORNER_STRATEGY::CHAMFER_ALL_CORNERS, 10 );

                    if( !eroded.OutlineCount() )
                        break;

                    deepAt = eroded.COutline( 0 ).CPoint( 0 );
                    depth = step / 2 + 100;
                }

                std::fprintf( stderr, "TRICHECK\tzone=%s\tlayer=%d\tpoly_mm2=%.9f\ttri_mm2=%.9f\trel=%.3e\t"
                                      "inside=%d\tworst_nm=%.0f\tat=%d,%d\tunder_mm2=%.9f\tunder_at=%d,%d\t"
                                      "over_mm2=%.9f\tdeep_at=%d,%d\tdepth_nm=%d\n",
                              aZone->m_Uuid.AsStdString().c_str(), (int) layer, polyArea / 1e12,
                              triArea / 1e12, rel, inside, worst, worstPt.x, worstPt.y,
                              uncovered.Area() / 1e12, underAt.x, underAt.y, overcovered.Area() / 1e12,
                              deepAt.x, deepAt.y, depth );
            } );
}
} // namespace

bool DRC_CACHE_GENERATOR::Run()
{
    m_board = m_drcEngine->GetBoard();

    // Everything below appends to the board's DRC caches rather than replacing them, so they
    // have to start out empty.
    m_board->IncrementTimeStamp();

    int&           largestClearance = m_board->m_DRCMaxClearance;
    int&           largestPhysicalClearance = m_board->m_DRCMaxPhysicalClearance;
    DRC_CONSTRAINT worstConstraint;
    LSET           boardCopperLayers = LSET::AllCuMask( m_board->GetCopperLayerCount() );
    thread_pool&   tp = GetKiCadThreadPool();

    largestClearance = std::max( largestClearance, m_board->GetMaxClearanceValue() );

    // Only consider unconditional constraints for the global maximum.  Conditional constraints
    // (like the barcode physical clearance default) apply only to specific item types and
    // should not inflate the R-tree query radius for all items on the board.
    if( m_drcEngine->QueryWorstConstraint( PHYSICAL_CLEARANCE_CONSTRAINT, worstConstraint, true ) )
        largestPhysicalClearance = worstConstraint.GetValue().Min();

    if( m_drcEngine->QueryWorstConstraint( PHYSICAL_HOLE_CLEARANCE_CONSTRAINT, worstConstraint, true ) )
        largestPhysicalClearance = std::max( largestPhysicalClearance, worstConstraint.GetValue().Min() );

    // If the unconditional max is 0, check for conditional constraints that may still apply.
    // User-defined conditional rules always need the test to run.  The implicit barcode rule
    // only needs the test if barcodes actually exist on the board.
    if( largestPhysicalClearance <= 0 )
    {
        int conditionalMax = 0;

        if( m_drcEngine->QueryWorstConstraint( PHYSICAL_CLEARANCE_CONSTRAINT, worstConstraint ) )
            conditionalMax = worstConstraint.GetValue().Min();

        if( m_drcEngine->QueryWorstConstraint( PHYSICAL_HOLE_CLEARANCE_CONSTRAINT, worstConstraint ) )
            conditionalMax = std::max( conditionalMax, worstConstraint.GetValue().Min() );

        if( conditionalMax > 0 )
        {
            if( m_drcEngine->HasUserDefinedPhysicalConstraint() )
            {
                largestPhysicalClearance = conditionalMax;
            }
            else
            {
                bool hasMatchingItems = false;

                forEachGeometryItem( { PCB_BARCODE_T }, LSET::AllLayersMask(),
                                     [&]( BOARD_ITEM* item ) -> bool
                                     {
                                         hasMatchingItems = true;
                                         return false;
                                     } );

                if( hasMatchingItems )
                    largestPhysicalClearance = conditionalMax;
            }
        }
    }

    // Ensure algorithmic safety
    largestClearance = std::min( largestClearance, INT_MAX / 3 );
    largestPhysicalClearance = std::min( largestPhysicalClearance, INT_MAX / 3 );

    std::set<ZONE*> allZones;

    auto cacheBBoxes =
            []( ZONE* zone, const LSET& copperLayers )
            {
                if( !zone->GetParentFootprint() )
                    zone->Outline()->BuildBBoxCaches();

                for( PCB_LAYER_ID layer : copperLayers )
                {
                    if( SHAPE_POLY_SET* fill = zone->GetFill( layer ) )
                        fill->BuildBBoxCaches();
                }
            };

    for( ZONE* zone : m_board->Zones() )
    {
        allZones.insert( zone );

        if( !zone->GetIsRuleArea() )
        {
            m_board->m_DRCZones.push_back( zone );

            LSET zoneCopperLayers = zone->GetLayerSet() & boardCopperLayers;

            if( zoneCopperLayers.any() )
            {
                cacheBBoxes( zone, zoneCopperLayers );
                m_board->m_DRCCopperZones.push_back( zone );
            }
        }
    }

    for( FOOTPRINT* footprint : m_board->Footprints() )
    {
        for( ZONE* zone : footprint->Zones() )
        {
            allZones.insert( zone );

            if( !zone->GetIsRuleArea() )
            {
                m_board->m_DRCZones.push_back( zone );

                LSET zoneCopperLayers = zone->GetLayerSet() & boardCopperLayers;

                if( zoneCopperLayers.any() )
                {
                    cacheBBoxes( zone, zoneCopperLayers );
                    m_board->m_DRCCopperZones.push_back( zone );
                }
            }
        }
    }

    for( ZONE* zone : m_board->m_DRCCopperZones )
    {
        LSET zoneCopperLayers = zone->GetLayerSet() & boardCopperLayers;

        for( PCB_LAYER_ID layer : zoneCopperLayers )
            m_board->m_DRCCopperZonesByLayer[layer].push_back( zone );
    }

    size_t              count = 0;
    std::atomic<size_t> done( 1 );

    auto countItems =
            [&]( BOARD_ITEM* item ) -> bool
            {
                ++count;
                return true;
            };

    auto addToCopperTree =
            [&]( BOARD_ITEM* item ) -> bool
            {
                if( m_drcEngine->IsCancelled() )
                    return false;

                LSET copperLayers = item->GetLayerSet() & boardCopperLayers;

                // Special-case pad holes which pierce all the copper layers
                if( item->Type() == PCB_PAD_T )
                {
                    PAD* pad = static_cast<PAD*>( item );

                    if( pad->HasHole() )
                        copperLayers = boardCopperLayers;
                }

                copperLayers.RunOnLayers(
                        [&]( PCB_LAYER_ID layer )
                        {
                            m_board->m_CopperItemRTreeCache->Insert( item, layer, CLEARANCE_CONSTRAINT,
                                                                     largestClearance );
                        } );

                done.fetch_add( 1 );
                return true;
            };

    if( !reportPhase( _( "Gathering copper items..." ) ) )
        return false;   // DRC cancelled

    static const std::vector<KICAD_T> itemTypes = {
        PCB_TRACE_T, PCB_ARC_T, PCB_VIA_T,
        PCB_PAD_T,
        PCB_SHAPE_T,
        PCB_FIELD_T, PCB_TEXT_T, PCB_TEXTBOX_T,
        PCB_TABLE_T, PCB_TABLECELL_T,
        PCB_DIMENSION_T,
        PCB_BARCODE_T
    };

    forEachGeometryItem( itemTypes, boardCopperLayers, countItems );

    std::future<void> retn = tp.submit_task(
            [&]()
            {
                std::unique_lock<std::shared_mutex> writeLock( m_board->m_CachesMutex );

                // Always start from an empty tree: the traversal below re-inserts every copper
                // item, and a DRC_RTREE stops accepting inserts once it has been built. 
                m_board->m_CopperItemRTreeCache = std::make_shared<DRC_RTREE>();

                forEachGeometryItem( itemTypes, boardCopperLayers, addToCopperTree );
                m_board->m_CopperItemRTreeCache->Build();
            } );

    std::future_status status = retn.wait_for( std::chrono::milliseconds( 250 ) );

    while( status != std::future_status::ready )
    {
        reportProgress( done, count );
        status = retn.wait_for( std::chrono::milliseconds( 250 ) );
    }

    if( !reportPhase( _( "Tessellating copper zones..." ) ) )
        return false;   // DRC cancelled

    // Cache zone bounding boxes, triangulation, copper zone rtrees, and footprint courtyards
    // before we start.

    for( FOOTPRINT* footprint : m_board->Footprints() )
    {
        footprint->BuildCourtyardCaches();
        footprint->BuildNetTieCache();
    }

    std::vector<std::future<size_t>> returns;

    returns.reserve( allZones.size() );

    auto cache_zones =
            [this, &done]( ZONE* aZone ) -> size_t
            {
                if( m_drcEngine->IsCancelled() )
                    return 0;

                aZone->CacheBoundingBox();
                aZone->CacheTriangulation();

                if( std::getenv( "KICAD_DRC_CHECK_TRIANGULATION" ) )
                    checkZoneTriangulation( aZone );

                if( !aZone->GetIsRuleArea() && aZone->IsOnCopperLayer() )
                {
                    aZone->GetLayerSet().RunOnLayers(
                            [&]( PCB_LAYER_ID layer )
                            {
                                if( IsCopperLayer( layer ) )
                                    DRC_ZONE_TRIANGULATION::Check( aZone, layer );
                            } );
                }

                if( !aZone->GetIsRuleArea() && aZone->IsOnCopperLayer() )
                {
                   std::unique_ptr<DRC_RTREE> rtree = std::make_unique<DRC_RTREE>();

                   aZone->GetLayerSet().RunOnLayers(
                           [&]( PCB_LAYER_ID layer )
                           {
                               if( IsCopperLayer( layer ) )
                                   rtree->Insert( aZone, layer, CLEARANCE_CONSTRAINT );
                           } );

                   rtree->Build();

                   {
                       std::unique_lock<std::shared_mutex> writeLock( m_board->m_CachesMutex );
                       m_board->m_CopperZoneRTreeCache[ aZone ] = std::move( rtree );
                   }

                   done.fetch_add( 1 );
                }

                return 1;
            };

    for( ZONE* zone : allZones )
    {
        returns.emplace_back( tp.submit_task(
                [cache_zones, zone]
                {
                    return cache_zones( zone );
                } ) );
    }

    done.store( 1 );

    for( const std::future<size_t>& ret : returns )
    {
        status = ret.wait_for( std::chrono::milliseconds( 250 ) );

        while( status != std::future_status::ready )
        {
            reportProgress( done, allZones.size() );
            status = ret.wait_for( std::chrono::milliseconds( 250 ) );
        }
    }

    m_board->m_ZoneIsolatedIslandsMap.clear();

    for( ZONE* zone : m_board->Zones() )
    {
        if( !zone->GetIsRuleArea() && !zone->IsTeardropArea() && !zone->IsCopperThieving() )
        {
            zone->GetLayerSet().RunOnLayers(
                    [&]( PCB_LAYER_ID layer )
                    {
                        m_board->m_ZoneIsolatedIslandsMap[ zone ][ layer ] = ISOLATED_ISLANDS();
                    } );
        }
    }

    m_board->UpdateBoardOutline();

    if( m_board->BoardOutline() )
        m_board->BoardOutline()->GetOutline().BuildBBoxCaches();

    std::shared_ptr<CONNECTIVITY_DATA> connectivity = m_board->GetConnectivity();

    connectivity->ClearRatsnest();
    connectivity->Build( m_board, m_drcEngine->GetProgressReporter() );
    connectivity->FillIsolatedIslandsMap( m_board->m_ZoneIsolatedIslandsMap, true );

    return !m_drcEngine->IsCancelled();
}
