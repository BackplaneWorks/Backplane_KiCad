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
#include <pcbnew_utils/board_test_utils.h>
#include <board.h>
#include <board_design_settings.h>
#include <drc/drc_engine.h>
#include <pcb_marker.h>
#include <drc/drc_item.h>
#include <settings/settings_manager.h>
#include <thread_pool.h>

#include <algorithm>
#include <array>
#include <tuple>
#include <utility>
#include <vector>


struct DRC_SOLDER_MASK_BRIDGING_TEST_FIXTURE
{
    DRC_SOLDER_MASK_BRIDGING_TEST_FIXTURE()
    { }

    SETTINGS_MANAGER       m_settingsManager;
    std::unique_ptr<BOARD> m_board;
};


BOOST_FIXTURE_TEST_CASE( DRCSolderMaskBridgingTest, DRC_SOLDER_MASK_BRIDGING_TEST_FIXTURE )
{
    wxString brd_name( wxT( "solder_mask_bridge_test" ) );
    KI_TEST::LoadBoard( m_settingsManager, brd_name, m_board );
    KI_TEST::FillZones( m_board.get() );

    std::vector<DRC_ITEM>  violations;
    BOARD_DESIGN_SETTINGS& bds = m_board->GetDesignSettings();

    // Disable some DRC tests not useful in this testcase (and time consuming)
    bds.m_DRCSeverities[ DRCE_LIB_FOOTPRINT_ISSUES ] = SEVERITY::RPT_SEVERITY_IGNORE;
    bds.m_DRCSeverities[ DRCE_LIB_FOOTPRINT_MISMATCH ] = SEVERITY::RPT_SEVERITY_IGNORE;
    bds.m_DRCSeverities[ DRCE_COPPER_SLIVER ] = SEVERITY::RPT_SEVERITY_IGNORE;
    bds.m_DRCSeverities[ DRCE_STARVED_THERMAL ] = SEVERITY::RPT_SEVERITY_IGNORE;
    bds.m_DRCSeverities[ DRCE_SILK_MASK_CLEARANCE ] = SEVERITY::RPT_SEVERITY_IGNORE;

    bds.m_DRCEngine->SetViolationHandler(
            [&]( const std::shared_ptr<DRC_ITEM>& aItem, const VECTOR2I& aPos, int aLayer,
                 const std::function<void( PCB_MARKER* )>& aPathGenerator )
            {
                PCB_MARKER temp( aItem, aPos );

                if( bds.m_DrcExclusions.find( DRC_EXCLUSION::FromMarker( temp ) ) == bds.m_DrcExclusions.end() )
                    violations.push_back( *aItem );
            } );

    bds.m_DRCEngine->RunTests( EDA_UNITS::MM, true, false );

    // Violation count after fix for deterministic cross-net reporting.
    // Previously 5, but that included potential duplicates from race conditions.
    const int expected_err_cnt = 4;

    if( violations.size() == expected_err_cnt )
    {
        BOOST_CHECK_EQUAL( 1, 1 );  // quiet "did not check any assertions" warning
        BOOST_TEST_MESSAGE( "DRC solder mask bridge test passed" );
    }
    else
    {
        BOOST_CHECK_EQUAL( violations.size(), expected_err_cnt );

        UNITS_PROVIDER unitsProvider( pcbIUScale, EDA_UNITS::INCH );

        BOARD_RC_ITEM_RESOLVER resolver( *m_board );

        for( const DRC_ITEM& item : violations )
            BOOST_TEST_MESSAGE( item.ShowReport( &unitsProvider, RPT_SEVERITY_ERROR, resolver ) );

        BOOST_ERROR( wxString::Format( "DRC solder mask bridge test failed board <%s>", brd_name ) );
    }
}


BOOST_FIXTURE_TEST_CASE( DRCSolderMaskBridgeDeterminismTest, DRC_SOLDER_MASK_BRIDGING_TEST_FIXTURE )
{
    // Dense autorouted board from issue 24951 has more solder_mask_bridge violations than the DRC
    // error-limit cap.  The count is stable at the cap, but which violations were kept depended on
    // worker arrival order, so the report entries wobbled run-to-run.  Two things must hold:
    //  - the reported violations (position + participating items), in the order they are
    //    reported, are the same on every run and at every thread count, at the cap too;
    //  - the cap keeps the right ones: the first N of all bridges in the provider's order
    //    (PENDING_BRIDGE::operator<).  The engine reports them in its own canonical order, so
    //    this is checked as a set, against a run with the limit lifted.
    KI_TEST::LoadBoard( m_settingsManager, wxT( "issue24951/issue24951" ), m_board );

    BOARD_DESIGN_SETTINGS& bds = m_board->GetDesignSettings();

    // Mirror of the provider's order: layer, then the UUIDs of the participating items (only
    // the populated ones sorted, so the unused third slot of a two-item violation stays
    // trailing, exactly as the provider leaves it), then the position.  A change to
    // PENDING_BRIDGE::operator< must be made here too.
    using KEY = std::tuple<int, std::array<KIID, 3>, int, int>;

    std::vector<wxString> sig;
    std::vector<KEY>      keys;

    bds.m_DRCEngine->SetViolationHandler(
            [&]( const std::shared_ptr<DRC_ITEM>& aItem, const VECTOR2I& aPos, int aLayer,
                 const std::function<void( PCB_MARKER* )>& )
            {
                if( aItem->GetErrorCode() != DRCE_SOLDERMASK_BRIDGE )
                    return;

                sig.push_back( wxString::Format( "%d,%d,%d,%s,%s,%s", aLayer, aPos.x, aPos.y,
                                                 aItem->GetMainItemID().AsString(),
                                                 aItem->GetAuxItemID().AsString(),
                                                 aItem->GetAuxItem2ID().AsString() ) );

                std::array<KIID, 3> ids = { aItem->GetMainItemID(), aItem->GetAuxItemID(),
                                            aItem->GetAuxItem2ID() };

                std::sort( ids.begin(), ids.end() - ( ids[2] == niluuid ? 1 : 0 ) );

                keys.emplace_back( aLayer, ids, aPos.x, aPos.y );
            } );

    auto runDrc =
            [&]() -> std::vector<wxString>
            {
                KI_TEST::FillZones( m_board.get() );
                sig.clear();
                keys.clear();
                bds.m_DRCEngine->RunTests( EDA_UNITS::MM, true, false );
                return sig;
            };

    thread_pool& pool = GetKiCadThreadPool();

    struct RESTORE_POOL
    {
        thread_pool& pool;
        size_t       count;
        ~RESTORE_POOL() { pool.reset( count ); }
    } restore{ pool, pool.get_thread_count() };

    pool.reset( 1 );
    std::vector<wxString> ref = runDrc();
    std::vector<KEY>      refKeys = keys;

    BOOST_TEST_MESSAGE( wxString::Format( "solder_mask_bridge violations: %zu", ref.size() ) );

    // The board must saturate the cap, or the capped path this test exists for is never taken.
    BOOST_REQUIRE( bds.m_DRCEngine->IsErrorLimitExceeded( DRCE_SOLDERMASK_BRIDGE ) );
    BOOST_REQUIRE_GT( ref.size(), 100u );

    // Same violations in the same order: 4 runs each at 1 and at 8 threads
    for( size_t threads : { 1, 8 } )
    {
        pool.reset( threads );

        for( int run = 0; run < 4; ++run )
        {
            std::vector<wxString> cur = runDrc();

            BOOST_CHECK_MESSAGE( cur == ref,
                                 wxString::Format( "solder_mask_bridge report differs (in content "
                                                   "or order) at %zu threads, run %d (%zu vs %zu "
                                                   "violations)",
                                                   threads, run, cur.size(), ref.size() ) );
        }
    }

    // The cap keeps the first N in the provider's order.  A broken heap would still report a
    // stable set, just not the right one, so compare with all of the board's bridges.
    // (the provider reserves room for its whole limit, so not much more than the board needs)
    bds.m_DRCEngine->SetErrorLimitOverride( DRCE_SOLDERMASK_BRIDGE, 100000 );
    runDrc();
    bool allReported = !bds.m_DRCEngine->IsErrorLimitExceeded( DRCE_SOLDERMASK_BRIDGE );
    bds.m_DRCEngine->SetErrorLimitOverride( DRCE_SOLDERMASK_BRIDGE, -1 );

    std::vector<KEY> all = keys;

    BOOST_TEST_MESSAGE( wxString::Format( "solder_mask_bridge violations without the cap: %zu",
                                          all.size() ) );
    BOOST_REQUIRE( allReported );
    BOOST_REQUIRE_GT( all.size(), ref.size() );

    std::sort( all.begin(), all.end() );
    all.resize( ref.size() );
    std::sort( refKeys.begin(), refKeys.end() );

    BOOST_CHECK_MESSAGE( refKeys == all,
                         "the capped report does not keep the first solder_mask_bridge violations "
                         "in the provider's order" );
}
