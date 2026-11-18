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
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * #1199: the embedding host's window requests are applied in the order KiCad received them.
 *
 * The cases drive BACKPLANE_EMBED's own entry points, the ones the API handlers and the events
 * socket call (ReserveHostSlot at receipt, HOST_REQUEST around a handler, HostGeometryNow,
 * HostBarrier, SetHostGeometryLater, DrainHost), so they test the production path; the reducer
 * underneath (EMBED_HOST_ORDER) is also tested on its own.  Receipt orders are the ones the two
 * sockets can really produce: a main-socket request takes its slot when received and is handled
 * later; an events-socket geometry takes its slot and is ready at once.
 */

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <stdexcept>
#include <thread>

#include <backplane_embed.h>
#include <embed_host_order.h>

using namespace EMBED_HOST_ORDER;

namespace
{
const char* const GEOMETRY_TYPE = "kiapi.backplane.embed.SetHostGeometry";
const char* const SHOW_TYPE = "kiapi.backplane.embed.ShowFrame";
const char* const HIDE_TYPE = "kiapi.backplane.embed.HideFrame";


GEOMETRY rect( int aX, int aWidth, bool aVisible = true, bool aRaise = false )
{
    return GEOMETRY{ aX, 153, aWidth, 500, aVisible, aRaise };
}


/// A main-socket SetHostGeometry: slot at receipt (done by the caller), handled now.
void handleGeometry( uint64_t aSlot, const GEOMETRY& aGeometry )
{
    BACKPLANE_EMBED::HOST_REQUEST request( aSlot );
    BACKPLANE_EMBED::Get().HostGeometryNow( aGeometry.x, aGeometry.y, aGeometry.width, aGeometry.height,
                                            aGeometry.visible, aGeometry.raise );
}


/// An events-socket SetHostGeometry: received (slot) and ready at once.
void eventsGeometry( const GEOMETRY& aGeometry )
{
    BACKPLANE_EMBED::Get().SetHostGeometryLater( aGeometry.x, aGeometry.y, aGeometry.width,
                                                 aGeometry.height, aGeometry.visible, aGeometry.raise );
}


GEOMETRY applied()
{
    return BACKPLANE_EMBED::Get().HostState().geometry;
}


/// Leave nothing pending for the next case.
struct CLEAN
{
    CLEAN() { BACKPLANE_EMBED::Get().DrainHost(); }
    ~CLEAN() { BACKPLANE_EMBED::Get().DrainHost(); }
};
} // namespace


BOOST_AUTO_TEST_SUITE( EmbedHostOrder )


BOOST_AUTO_TEST_CASE( ReducerShowMakesVisibleGeometryWins )
{
    STATE s;
    s = Reduce( s, OP{ KIND::GEOMETRY, rect( 430, 496, false ), 1 } );
    BOOST_CHECK( !s.geometry.visible );
    BOOST_CHECK( s.known );

    s = Reduce( s, OP{ KIND::SHOW, {}, 2 } );
    BOOST_CHECK( s.geometry.visible );
    BOOST_CHECK_EQUAL( s.geometry.width, 496 );   // a show keeps the rect

    s = Reduce( s, OP{ KIND::BARRIER, {}, 3 } );
    BOOST_CHECK( s.geometry.visible );
    BOOST_CHECK_EQUAL( s.seq, 3u );

    s = Reduce( s, OP{ KIND::GEOMETRY, rect( 47, 879, true, true ), 4 } );
    BOOST_CHECK_EQUAL( s.geometry.width, 879 );
    BOOST_CHECK( !s.geometry.raise );   // a raise is a step, never state
}


BOOST_AUTO_TEST_CASE( FoldKeepsRaiseUntilALaterHide )
{
    // A raise then a plain move: the raise survives the fold
    std::vector<OP> f = Fold( { OP{ KIND::GEOMETRY, rect( 1, 10, true, true ), 1 },
                                OP{ KIND::GEOMETRY, rect( 2, 20 ), 2 } } );
    BOOST_REQUIRE_EQUAL( f.size(), 1u );
    BOOST_CHECK_EQUAL( f[0].geometry.x, 2 );
    BOOST_CHECK( f[0].geometry.raise );
    BOOST_CHECK_EQUAL( f[0].seq, 2u );

    // A raise then a hide: the hide wins and takes the raise with it
    f = Fold( { OP{ KIND::GEOMETRY, rect( 1, 10, true, true ), 1 },
                OP{ KIND::GEOMETRY, rect( 1, 10, false ), 2 } } );
    BOOST_REQUIRE_EQUAL( f.size(), 1u );
    BOOST_CHECK( !f[0].geometry.visible );
    BOOST_CHECK( !f[0].geometry.raise );

    // A raise, a hide, a plain show: the stale raise does not come back
    f = Fold( { OP{ KIND::GEOMETRY, rect( 1, 10, true, true ), 1 },
                OP{ KIND::GEOMETRY, rect( 1, 10, false ), 2 },
                OP{ KIND::GEOMETRY, rect( 1, 10, true ), 3 } } );
    BOOST_REQUIRE_EQUAL( f.size(), 1u );
    BOOST_CHECK( f[0].geometry.visible );
    BOOST_CHECK( !f[0].geometry.raise );

    // A hide on its own never carries a raise
    f = Fold( { OP{ KIND::GEOMETRY, rect( 1, 10, false, true ), 1 } } );
    BOOST_CHECK( !f[0].geometry.raise );
}


BOOST_AUTO_TEST_CASE( FoldNeverCrossesAShowOrHide )
{
    std::vector<OP> f = Fold( { OP{ KIND::GEOMETRY, rect( 1, 10 ), 1 }, OP{ KIND::SHOW, {}, 2 },
                                OP{ KIND::GEOMETRY, rect( 2, 20 ), 3 }, OP{ KIND::BARRIER, {}, 4 },
                                OP{ KIND::GEOMETRY, rect( 3, 30 ), 5 }, OP{ KIND::GEOMETRY, rect( 4, 40 ), 6 } } );
    BOOST_REQUIRE_EQUAL( f.size(), 5u );
    BOOST_CHECK( f[0].kind == KIND::GEOMETRY && f[0].geometry.x == 1 );
    BOOST_CHECK( f[1].kind == KIND::SHOW );
    BOOST_CHECK( f[2].kind == KIND::GEOMETRY && f[2].geometry.x == 2 );
    BOOST_CHECK( f[3].kind == KIND::BARRIER );
    BOOST_CHECK( f[4].kind == KIND::GEOMETRY && f[4].geometry.x == 4 );
}


BOOST_AUTO_TEST_CASE( QueueStopsAtASlotStillOpen )
{
    QUEUE q;
    uint64_t main = q.Reserve();                                       // received, not handled
    q.Submit( OP{ KIND::GEOMETRY, rect( 7, 70 ), 0 } );                // received after it

    BOOST_CHECK( q.TakeReady().empty() );                              // held back

    q.Cancel( main );                                                  // refused
    std::vector<OP> ready = q.TakeReady();
    BOOST_REQUIRE_EQUAL( ready.size(), 1u );
    BOOST_CHECK_EQUAL( ready[0].geometry.x, 7 );
    BOOST_CHECK( !q.HasReadyOrOpen() );
}


BOOST_AUTO_TEST_CASE( HideThenALoneShowEndsVisible )
{
    CLEAN clean;
    BACKPLANE_EMBED& embed = BACKPLANE_EMBED::Get();

    // Lane M's return 3, in order: the host hides, then shows the frame with no later geometry
    handleGeometry( embed.ReserveHostSlot( GEOMETRY_TYPE ), rect( 430, 496, false ) );
    BOOST_CHECK( !applied().visible );

    {
        BACKPLANE_EMBED::HOST_REQUEST show( embed.ReserveHostSlot( SHOW_TYPE ) );
        embed.HostBarrier( true );
        BOOST_CHECK( applied().visible );   // before the frame is mapped, so it is ordered in
    }

    BOOST_CHECK( applied().visible );
    BOOST_CHECK_EQUAL( applied().width, 496 );
}


BOOST_AUTO_TEST_CASE( BurstEndsAtTheLastRect )
{
    CLEAN clean;
    BACKPLANE_EMBED& embed = BACKPLANE_EMBED::Get();

    // Main socket, as the hub sends: 524, 500, 879, then the show
    handleGeometry( embed.ReserveHostSlot( GEOMETRY_TYPE ), rect( 430, 496, false ) );
    handleGeometry( embed.ReserveHostSlot( GEOMETRY_TYPE ), GEOMETRY{ 430, 153, 496, 524, true, false } );
    handleGeometry( embed.ReserveHostSlot( GEOMETRY_TYPE ), rect( 430, 496 ) );
    handleGeometry( embed.ReserveHostSlot( GEOMETRY_TYPE ), rect( 47, 879 ) );

    {
        BACKPLANE_EMBED::HOST_REQUEST show( embed.ReserveHostSlot( SHOW_TYPE ) );
        embed.HostBarrier( true );
    }

    BOOST_CHECK_EQUAL( applied().x, 47 );
    BOOST_CHECK_EQUAL( applied().width, 879 );
    BOOST_CHECK( applied().visible );

    // Events socket, a drag: only the last rect remains
    for( int ii = 0; ii < 20; ++ii )
        eventsGeometry( rect( 100 + ii, 600 + ii ) );

    embed.DrainHost();
    BOOST_CHECK_EQUAL( applied().x, 119 );
    BOOST_CHECK_EQUAL( applied().width, 619 );
}


BOOST_AUTO_TEST_CASE( AShowThenALaterHideEndsHidden )
{
    CLEAN clean;
    BACKPLANE_EMBED& embed = BACKPLANE_EMBED::Get();

    handleGeometry( embed.ReserveHostSlot( GEOMETRY_TYPE ), rect( 430, 496 ) );

    // The show is received first; the events socket's hide arrives while it is being handled
    {
        BACKPLANE_EMBED::HOST_REQUEST show( embed.ReserveHostSlot( SHOW_TYPE ) );
        embed.HostBarrier( true );
        eventsGeometry( rect( 430, 496, false ) );
        embed.DrainHost();                      // a yield inside the show: held back
        BOOST_CHECK( applied().visible );
    }

    BOOST_CHECK( !applied().visible );          // the later hide wins, and stays

    // A main-socket hide after a main-socket show: the same
    {
        BACKPLANE_EMBED::HOST_REQUEST show( embed.ReserveHostSlot( SHOW_TYPE ) );
        embed.HostBarrier( true );
    }

    handleGeometry( embed.ReserveHostSlot( GEOMETRY_TYPE ), rect( 430, 496, false ) );
    BOOST_CHECK( !applied().visible );
}


BOOST_AUTO_TEST_CASE( NothingReceivedAfterAShowOvertakesIt )
{
    CLEAN clean;
    BACKPLANE_EMBED& embed = BACKPLANE_EMBED::Get();

    handleGeometry( embed.ReserveHostSlot( GEOMETRY_TYPE ), rect( 10, 100 ) );

    // Received in this order: events geometry A, main ShowFrame, events geometry B.  The
    // ShowFrame's handling starts after both events geometries are in.
    eventsGeometry( rect( 20, 200 ) );
    uint64_t show = embed.ReserveHostSlot( SHOW_TYPE );
    eventsGeometry( rect( 30, 300 ) );

    embed.DrainHost();                          // wx ran the events drain first
    BOOST_CHECK_EQUAL( applied().x, 20 );       // A applied, B held behind the show

    {
        BACKPLANE_EMBED::HOST_REQUEST request( show );
        embed.HostBarrier( true );
        BOOST_CHECK_EQUAL( applied().x, 20 );   // still not B during the show
    }

    BOOST_CHECK_EQUAL( applied().x, 30 );       // B after it
}


BOOST_AUTO_TEST_CASE( ARefusedRequestHoldsNothingBack )
{
    CLEAN clean;
    BACKPLANE_EMBED& embed = BACKPLANE_EMBED::Get();

    uint64_t refused = embed.ReserveHostSlot( HIDE_TYPE );
    eventsGeometry( rect( 55, 550 ) );
    embed.DrainHost();
    BOOST_CHECK( applied().x != 55 );           // behind the hide still waiting

    {
        BACKPLANE_EMBED::HOST_REQUEST request( refused );   // answered with an error, no barrier
    }

    BOOST_CHECK_EQUAL( applied().x, 55 );
    BOOST_CHECK_EQUAL( embed.ReserveHostSlot( "kiapi.backplane.embed.GetFrames" ), 0u );
}


BOOST_AUTO_TEST_CASE( RaiseKeptAcrossAQueuedRun )
{
    CLEAN clean;
    BACKPLANE_EMBED& embed = BACKPLANE_EMBED::Get();

    // Held behind an open show, a raise and then a plain move fold into one step that raises
    uint64_t show = embed.ReserveHostSlot( SHOW_TYPE );
    eventsGeometry( rect( 60, 600, true, true ) );
    eventsGeometry( rect( 61, 610 ) );

    std::vector<OP> folded = Fold( { OP{ KIND::GEOMETRY, rect( 60, 600, true, true ), 1 },
                                     OP{ KIND::GEOMETRY, rect( 61, 610 ), 2 } } );
    BOOST_CHECK( folded.size() == 1 && folded[0].geometry.raise );

    {
        BACKPLANE_EMBED::HOST_REQUEST request( show );
        embed.HostBarrier( true );
    }

    BOOST_CHECK_EQUAL( applied().x, 61 );
}


BOOST_AUTO_TEST_CASE( ALaterBarrierHandledFirstWaitsForTheEarlierSlot )
{
    CLEAN clean;
    BACKPLANE_EMBED& embed = BACKPLANE_EMBED::Get();

    handleGeometry( embed.ReserveHostSlot( GEOMETRY_TYPE ), rect( 10, 100 ) );

    // Received: a hide (A), then a show (B), then the events socket's move (C).  B is handled
    // before A (one dispatch never does this; nothing may depend on it).
    uint64_t a = embed.ReserveHostSlot( GEOMETRY_TYPE );
    uint64_t b = embed.ReserveHostSlot( SHOW_TYPE );
    eventsGeometry( rect( 30, 300 ) );

    {
        BACKPLANE_EMBED::HOST_REQUEST show( b );
        embed.HostBarrier( true );
        BOOST_CHECK_EQUAL( applied().x, 10 );   // neither its show nor C ahead of A
        BOOST_CHECK( applied().visible );
    }

    embed.DrainHost();
    BOOST_CHECK_EQUAL( applied().x, 10 );       // still waiting for A
    BOOST_CHECK_EQUAL( embed.HostState().seq < a, true );

    handleGeometry( a, rect( 20, 200, false ) ); // A: the hide, then B's show, then C

    BOOST_CHECK_EQUAL( applied().x, 30 );
    BOOST_CHECK( applied().visible );           // B's show came after A's hide
    BOOST_CHECK( embed.HostState().seq > b );
}


BOOST_AUTO_TEST_CASE( ARequestHandledInsideAnotherWaitsForIt )
{
    CLEAN clean;
    BACKPLANE_EMBED& embed = BACKPLANE_EMBED::Get();

    handleGeometry( embed.ReserveHostSlot( GEOMETRY_TYPE ), rect( 10, 100, false ) );

    // A ShowFrame whose load yields (a task pump, a nested loop) and another request served
    // inside it: a second show and a geometry, both received after the first show
    uint64_t outer = embed.ReserveHostSlot( SHOW_TYPE );
    uint64_t inner = embed.ReserveHostSlot( HIDE_TYPE );
    uint64_t move = embed.ReserveHostSlot( GEOMETRY_TYPE );

    {
        BACKPLANE_EMBED::HOST_REQUEST first( outer );
        embed.HostBarrier( true );
        BOOST_CHECK( applied().visible );

        {
            BACKPLANE_EMBED::HOST_REQUEST second( inner );
            embed.HostBarrier( false );
        }

        handleGeometry( move, rect( 40, 400, false ) );
        BOOST_CHECK( applied().visible );       // nothing after the running show yet
        BOOST_CHECK_EQUAL( applied().x, 10 );
    }

    BOOST_CHECK_EQUAL( applied().x, 40 );       // then both, in order
    BOOST_CHECK( !applied().visible );
}


BOOST_AUTO_TEST_CASE( AHandlerThatThrowsHoldsNothingBack )
{
    CLEAN clean;
    BACKPLANE_EMBED& embed = BACKPLANE_EMBED::Get();

    for( bool begun : { false, true } )
    {
        uint64_t slot = embed.ReserveHostSlot( SHOW_TYPE );
        eventsGeometry( rect( begun ? 72 : 71, 700 ) );

        try
        {
            BACKPLANE_EMBED::HOST_REQUEST request( slot );

            if( begun )
                embed.HostBarrier( true );

            throw std::runtime_error( "handler failed" );
        }
        catch( const std::runtime_error& )
        {
        }

        BOOST_CHECK_EQUAL( applied().x, begun ? 72 : 71 );
    }
}


BOOST_AUTO_TEST_CASE( AShowHeldTooLongLetsLaterRequestsPass )
{
    CLEAN clean;
    BACKPLANE_EMBED& embed = BACKPLANE_EMBED::Get();

    handleGeometry( embed.ReserveHostSlot( GEOMETRY_TYPE ), rect( 10, 100, false ) );

    // A ShowFrame running a dialog: its show is applied; later moves wait, then pass
    uint64_t show = embed.ReserveHostSlot( SHOW_TYPE );

    {
        BACKPLANE_EMBED::HOST_REQUEST request( show );
        embed.HostBarrier( true );
        eventsGeometry( rect( 80, 800 ) );
        embed.DrainHost();
        BOOST_CHECK_EQUAL( applied().x, 10 );

        std::this_thread::sleep_for( std::chrono::milliseconds( BACKPLANE_EMBED::HOST_HOLD_MS + 100 ) );
        embed.DrainHost();
        BOOST_CHECK_EQUAL( applied().x, 80 );   // its effect came first; the rest goes on
        BOOST_CHECK( applied().visible );

        eventsGeometry( rect( 81, 810, false ) );
        embed.DrainHost();
        BOOST_CHECK( !applied().visible );
    }

    BOOST_CHECK_EQUAL( applied().x, 81 );       // the show's end re-applies nothing
    BOOST_CHECK( !applied().visible );
}


BOOST_AUTO_TEST_CASE( AnEmptySlotHoldsTheQueueHoweverLong )
{
    CLEAN clean;
    BACKPLANE_EMBED& embed = BACKPLANE_EMBED::Get();

    handleGeometry( embed.ReserveHostSlot( GEOMETRY_TYPE ), rect( 10, 100, false ) );

    // Received, never handled yet (empty), and a show handled but deferred behind it: past the
    // hold limit neither lets the later move through
    uint64_t empty = embed.ReserveHostSlot( GEOMETRY_TYPE );
    uint64_t deferred = embed.ReserveHostSlot( SHOW_TYPE );
    eventsGeometry( rect( 90, 900 ) );

    {
        BACKPLANE_EMBED::HOST_REQUEST request( deferred );
        embed.HostBarrier( true );

        std::this_thread::sleep_for( std::chrono::milliseconds( BACKPLANE_EMBED::HOST_HOLD_MS + 100 ) );
        embed.DrainHost();
        BOOST_CHECK_EQUAL( applied().x, 10 );
        BOOST_CHECK( !applied().visible );
    }

    std::this_thread::sleep_for( std::chrono::milliseconds( BACKPLANE_EMBED::HOST_HOLD_MS + 100 ) );
    embed.DrainHost();
    BOOST_CHECK_EQUAL( applied().x, 10 );       // the empty slot still holds everything
    BOOST_CHECK( !applied().visible );

    handleGeometry( empty, rect( 20, 200, false ) );
    BOOST_CHECK_EQUAL( applied().x, 90 );       // then: its hide, the show, the move
    BOOST_CHECK( applied().visible );
}


BOOST_AUTO_TEST_SUITE_END()
