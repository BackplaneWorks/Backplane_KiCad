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

#include <functional>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

#include <common.h>
#include <board.h>
#include <pcb_board_outline.h>
#include <pcb_track.h>
#include <zone.h>
#include <geometry/shape_segment.h>
#include <geometry/seg.h>
#include <drc/drc_engine.h>
#include <drc/drc_item.h>
#include <drc/drc_rule.h>
#include <drc/drc_test_provider.h>
#include <drc/drc_rtree.h>
#include <board_design_settings.h>

/*
    Silk to silk clearance test. Check all silkscreen features against each other.
    Errors generated:
    - DRCE_SILK_CLEARANCE

*/

class DRC_TEST_PROVIDER_SILK_CLEARANCE : public DRC_TEST_PROVIDER
{
public:
    DRC_TEST_PROVIDER_SILK_CLEARANCE ():
        m_board( nullptr ),
        m_largestClearance( 0 )
    {}

    virtual ~DRC_TEST_PROVIDER_SILK_CLEARANCE() = default;

    virtual bool Run() override;

    virtual const wxString GetName() const override { return wxT( "silk_clearance" ); };

private:

    BOARD* m_board;
    int m_largestClearance;
};


bool DRC_TEST_PROVIDER_SILK_CLEARANCE::Run()
{
    const int progressDelta = 500;

    m_board = m_drcEngine->GetBoard();

    // If the soldermask min width is greater than 0 then we must use a healing algorithm to generate
    // a whole-board soldermask poly, and then test against that.  However, that can't deal well with
    // DRC exclusions (as any change anywhere on the board that affects the soldermask will null the
    // associated exclusions), so we only use that when soldermask min width is > 0.
    bool checkIndividualMaskItems = m_board->GetDesignSettings().m_SolderMaskMinWidth <= 0;

    REPORT_AUX( wxString::Format( wxT( "Silk to mask: %s (solder mask min width %d nm)" ),
                                  checkIndividualMaskItems ? wxT( "individual mask items" )
                                                           : wxT( "whole-board mask" ),
                                  m_board->GetDesignSettings().m_SolderMaskMinWidth ) );

    if( m_drcEngine->IsErrorLimitExceeded( DRCE_SILK_CLEARANCE )
            && m_drcEngine->IsErrorLimitExceeded( DRCE_SILK_MASK_CLEARANCE) )
    {
        return true;    // continue with other tests
    }

    DRC_CONSTRAINT worstClearanceConstraint;
    m_largestClearance = 0;

    if( m_drcEngine->QueryWorstConstraint( SILK_CLEARANCE_CONSTRAINT, worstClearanceConstraint ) )
        m_largestClearance = worstClearanceConstraint.m_Value.Min();

    if( !reportPhase( _( "Checking silkscreen for overlapping items..." ) ) )
        return false;   // DRC cancelled

    DRC_RTREE silkTree;
    DRC_RTREE targetTree;
    int       ii = 0;
    int       items = 0;
    LSET      silkLayers = LSET( { F_SilkS, B_SilkS } );
    LSET      targetLayers = LSET::FrontMask() | LSET::BackMask() | LSET( { Edge_Cuts, Margin } );

    auto countItems =
            [&]( BOARD_ITEM* item ) -> bool
            {
                ++items;
                return true;
            };

    // Rule areas are purely logical (no physical copper, mask, or silk) so they must never
    // participate in silk-clearance collisions.  Their effective shape now follows the
    // outline (so disallow checks can collide against them), which would otherwise cause
    // false silk-to-rule-area violations.
    auto isRuleArea =
            [&]( BOARD_ITEM* item ) -> bool
            {
                return item->Type() == PCB_ZONE_T && static_cast<ZONE*>( item )->GetIsRuleArea();
            };

    auto addToSilkTree =
            [&]( BOARD_ITEM* item ) -> bool
            {
                if( !reportProgress( ii++, items, progressDelta ) )
                    return false;

                if( isRuleArea( item ) )
                    return true;

                for( PCB_LAYER_ID layer : { F_SilkS, B_SilkS } )
                {
                    if( item->IsOnLayer( layer ) )
                        silkTree.Insert( item, layer, SILK_CLEARANCE_CONSTRAINT, 0, ATOMIC_TABLES );
                }

                return true;
            };

    auto addToTargetTree =
            [&]( BOARD_ITEM* item ) -> bool
            {
                if( !reportProgress( ii++, items, progressDelta ) )
                    return false;

                if( isRuleArea( item ) )
                    return true;

                for( PCB_LAYER_ID layer : LSET( item->GetLayerSet() & targetLayers ) )
                    targetTree.Insert( item, layer, SILK_CLEARANCE_CONSTRAINT, 0, ATOMIC_TABLES );

                return true;
            };

    forEachGeometryItem( s_allBasicItems, silkLayers, countItems );
    forEachGeometryItem( s_allBasicItems, targetLayers, countItems );

    forEachGeometryItem( s_allBasicItems, silkLayers, addToSilkTree );
    forEachGeometryItem( s_allBasicItems, targetLayers, addToTargetTree );

    silkTree.Build();
    targetTree.Build();

    REPORT_AUX( wxString::Format( wxT( "Testing %d silkscreen features against %d board items." ),
                                  silkTree.size(),
                                  targetTree.size() ) );

    // Cache the board-outline bounding box and per-subshape collision results so that each
    // subshape is only tested against the outline once during the visitor sweep.  Without
    // caching, QueryCollidingPairs invokes the visitor O(silk * target) times and the outline
    // Collide (which walks the outline's triangulation) was dominating DRC runtime on boards
    // with many silkscreen/mask polygons (see issue 24007).
    PCB_BOARD_OUTLINE* boardOutline = m_board->BoardOutline();
    BOX2I              outlineBBox;

    if( boardOutline && !boardOutline->HasOutline() )
        boardOutline = nullptr;

    if( boardOutline )
        outlineBBox = boardOutline->GetOutline().BBoxFromCaches();

    std::unordered_map<const SHAPE*, bool> outlineCollisionCache;
    std::shared_mutex                      outlineCollisionCacheMutex;

    const std::vector<DRC_RTREE::LAYER_PAIR> layerPairs =
    {
        DRC_RTREE::LAYER_PAIR( F_SilkS, F_SilkS ),
        DRC_RTREE::LAYER_PAIR( F_SilkS, F_Mask ),
        DRC_RTREE::LAYER_PAIR( F_SilkS, F_Adhes ),
        DRC_RTREE::LAYER_PAIR( F_SilkS, F_Paste ),
        DRC_RTREE::LAYER_PAIR( F_SilkS, F_CrtYd ),
        DRC_RTREE::LAYER_PAIR( F_SilkS, F_Fab ),
        DRC_RTREE::LAYER_PAIR( F_SilkS, F_Cu ),
        DRC_RTREE::LAYER_PAIR( F_SilkS, Edge_Cuts ),
        DRC_RTREE::LAYER_PAIR( F_SilkS, Margin ),
        DRC_RTREE::LAYER_PAIR( B_SilkS, B_SilkS ),
        DRC_RTREE::LAYER_PAIR( B_SilkS, B_Mask ),
        DRC_RTREE::LAYER_PAIR( B_SilkS, B_Adhes ),
        DRC_RTREE::LAYER_PAIR( B_SilkS, B_Paste ),
        DRC_RTREE::LAYER_PAIR( B_SilkS, B_CrtYd ),
        DRC_RTREE::LAYER_PAIR( B_SilkS, B_Fab ),
        DRC_RTREE::LAYER_PAIR( B_SilkS, B_Cu ),
        DRC_RTREE::LAYER_PAIR( B_SilkS, Edge_Cuts ),
        DRC_RTREE::LAYER_PAIR( B_SilkS, Margin )
    };


    // The collision tests run in parallel.  SHAPE_POLY_SET::Collide() triangulates lazily, which
    // is not safe to do from several threads at once, so triangulate every polygon the tests can
    // touch first.
    if( boardOutline )
        const_cast<SHAPE_POLY_SET&>( boardOutline->GetOutline() ).CacheTriangulation( false, {}, true );

    auto prepareShapes =
            [&]( DRC_RTREE& aTree, const LSET& aLayers )
            {
                for( PCB_LAYER_ID layer : aLayers )
                {
                    for( DRC_RTREE::ITEM_WITH_SHAPE* itemShape : aTree.OnLayer( layer ) )
                    {
                        if( itemShape->shape->Type() == SH_POLY_SET )
                        {
                            const SHAPE_POLY_SET* poly = static_cast<const SHAPE_POLY_SET*>( itemShape->shape );
                            const_cast<SHAPE_POLY_SET*>( poly )->CacheTriangulation( false, {}, true );
                        }
                    }
                }
            };

    prepareShapes( silkTree, silkLayers );
    prepareShapes( targetTree, targetLayers );

    // A collision found by a worker, reported on the calling thread in serial order.
    struct SILK_HIT
    {
        std::shared_ptr<DRC_ITEM> drcItem;
        VECTOR2I                  pos;
        PCB_LAYER_ID              layer = UNDEFINED_LAYER;
        bool                      hasPoints = false;
        VECTOR2I                  ptA;
        VECTOR2I                  ptB;
    };

    std::function<bool( const DRC_RTREE::LAYER_PAIR&, DRC_RTREE::ITEM_WITH_SHAPE*,
                        DRC_RTREE::ITEM_WITH_SHAPE*, SILK_HIT& )> test =
            [&]( const DRC_RTREE::LAYER_PAIR& aLayers, DRC_RTREE::ITEM_WITH_SHAPE* aRefItemShape,
                 DRC_RTREE::ITEM_WITH_SHAPE* aTestItemShape, SILK_HIT& aHit ) -> bool
            {
                BOARD_ITEM*  refItem = aRefItemShape->parent;
                const SHAPE* refShape = aRefItemShape->shape;
                BOARD_ITEM*  testItem = aTestItemShape->parent;
                const SHAPE* testShape = aTestItemShape->shape;

                std::shared_ptr<SHAPE> hole;

                if( isInvisibleText( refItem ) || isInvisibleText( testItem ) )
                    return false;

                if( testItem->IsTented( aLayers.first ) )
                {
                    if( testItem->HasHole() )
                    {
                        hole = testItem->GetEffectiveHoleShape();
                        testShape = hole.get();
                    }
                    else
                    {
                        return false;
                    }
                }

                if( boardOutline )
                {
                    if( !testItem->GetBoundingBox().Intersects( outlineBBox ) )
                        return false;

                    // Only cache for shapes owned by the R-tree (stable pointers).  Hole
                    // shapes are freshly created per visitor call via shared_ptr, so their
                    // raw addresses cannot be safely used as cache keys.
                    bool collidesOutline;

                    if( testShape == aTestItemShape->shape )
                    {
                        std::optional<bool> cached;

                        {
                            std::shared_lock lock( outlineCollisionCacheMutex );
                            auto             it = outlineCollisionCache.find( testShape );

                            if( it != outlineCollisionCache.end() )
                                cached = it->second;
                        }

                        if( cached )
                        {
                            collidesOutline = *cached;
                        }
                        else
                        {
                            collidesOutline = testShape->Collide( &boardOutline->GetOutline() );

                            std::unique_lock lock( outlineCollisionCacheMutex );
                            outlineCollisionCache.try_emplace( testShape, collidesOutline );
                        }
                    }
                    else
                    {
                        collidesOutline = testShape->Collide( &boardOutline->GetOutline() );
                    }

                    if( !collidesOutline )
                        return false;
                }

                int            errorCode = DRCE_SILK_CLEARANCE;
                DRC_CONSTRAINT constraint = m_drcEngine->EvalRules( SILK_CLEARANCE_CONSTRAINT,
                                                                    refItem, testItem, aLayers.second );
                int            minClearance = -1;

                if( !constraint.IsNull() && constraint.GetSeverity() != RPT_SEVERITY_IGNORE )
                    minClearance = constraint.GetValue().Min();

                if( aLayers.second == F_Mask || aLayers.second == B_Mask )
                {
                    // Silk over a mask opening is tested even without a silk clearance, but never
                    // against a rule that waives it (severity ignore): the person's waiver wins,
                    // as it does in the whole-board mask test (solder mask min width > 0)
                    bool waived = !constraint.IsNull() && constraint.GetSeverity() == RPT_SEVERITY_IGNORE;

                    if( checkIndividualMaskItems && !waived )
                        minClearance = std::max( minClearance, 0 );

                    errorCode = DRCE_SILK_MASK_CLEARANCE;
                }

                // Error limits are only consumed when the provider's violations are flushed,
                // so this reads the same for every pair of this run.
                if( minClearance < 0 || m_drcEngine->IsErrorLimitExceeded( errorCode ) )
                    return false;

                int      actual;
                VECTOR2I pos;

                // Graphics are often compound shapes so ignore collisions between shapes in a
                // single footprint or on the board (both parent footprints will be nullptr).
                if( refItem->Type() == PCB_SHAPE_T && testItem->Type() == PCB_SHAPE_T
                         && refItem->GetParentFootprint() == testItem->GetParentFootprint() )
                {
                    return false;
                }

                // Collide (and generate violations) based on a well-defined order so that
                // exclusion checking against previously-generated violations will work.
                if( aLayers.first == aLayers.second )
                {
                    if( refItem->m_Uuid > testItem->m_Uuid )
                    {
                        std::swap( refItem, testItem );
                        std::swap( refShape, testShape );
                    }
                }

                if( !refShape->Collide( testShape, minClearance, &actual, &pos ) )
                    return false;

                std::shared_ptr<DRC_ITEM> drcItem = DRC_ITEM::Create( errorCode );

                if( minClearance > 0 )
                {
                    drcItem->SetErrorDetail( formatMsg( _( "(%s clearance %s; actual %s)" ),
                                                        constraint.GetName(),
                                                        minClearance,
                                                        actual ) );
                }

                drcItem->SetItems( refItem, testItem );
                drcItem->SetViolatingRule( constraint.GetParentRule() );

                aHit.drcItem = drcItem;
                aHit.pos = pos;
                aHit.layer = aLayers.second;

                // The geometry reportTwoShapeGeometry() would draw, computed while the
                // shapes (and any hole shape) are still alive.
                if( actual == 0 )
                {
                    aHit.hasPoints = true;
                    aHit.ptA = pos;
                    aHit.ptB = pos;
                }
                else if( refShape->NearestPoints( testShape, aHit.ptA, aHit.ptB ) )
                {
                    aHit.hasPoints = true;
                }

                return true;
            };

    std::function<void( SILK_HIT& )> commit =
            [&]( SILK_HIT& aHit )
            {
                if( aHit.hasPoints )
                    reportTwoPointGeometry( aHit.drcItem, aHit.pos, aHit.ptA, aHit.ptB, aHit.layer );
                else
                    reportViolation( aHit.drcItem, aHit.pos, aHit.layer );
            };

    targetTree.QueryCollidingPairsParallel<SILK_HIT>( &silkTree, layerPairs, test, commit,
            m_largestClearance,
            [&]( int aCount, int aSize ) -> bool
            {
                return reportProgress( aCount, aSize, progressDelta );
            },
            [&]() -> bool
            {
                return m_drcEngine->IsCancelled();
            } );

    return !m_drcEngine->IsCancelled();
}


namespace detail
{
    static DRC_REGISTER_TEST_PROVIDER<DRC_TEST_PROVIDER_SILK_CLEARANCE> dummy;
}
