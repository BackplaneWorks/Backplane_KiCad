/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2023 Jon Evans <jon@craftyjon.com>
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

#include <wx/filename.h>
#include <wx/stdpaths.h>

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <qa_utils/file_utils.h>

#include <kinng.h>
#include <nng/nng.h>
#include <nng/protocol/reqrep0/req.h>

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

#include <import_export.h>
#include <api/common/envelope.pb.h>

BOOST_AUTO_TEST_SUITE( KiNNG )

BOOST_AUTO_TEST_CASE( CreateIPCResponder )
{
    KI_TEST::SCOPED_TEMP_DIR tempDir( "kicad_qa_kinng" );

    KINNG_REQUEST_SERVER server( tempDir.Path().string() );
}

/**
 * Stop() while the listener waits for the reply to a request it has received (the hub's next poll arriving as KiCad exits, the main
 * thread already in Stop and never answering): Stop must return. Stop is held by its test hook between setting the shutdown flag and
 * waking the listener, so the listener has time to re-check its wait: with the flag set before the wake (the fix) it ends; with the
 * old order (woken first, the flag stored after, outside the lock) it went back to sleep and the join never returned (Windows exit
 * hangs, bp.13.3). Measured both ways.
 */
BOOST_AUTO_TEST_CASE( StopWhileARequestWaitsForItsReply )
{
    KI_TEST::SCOPED_TEMP_DIR tempDir( "kicad_qa_kinng" );
    std::string              url = "ipc://" + ( tempDir.Path() / "kinng-stop.sock" ).string();

    // leaked if Stop hangs, so the failing test ends instead of joining the stuck listener again in the destructor
    auto*              server = new KINNG_REQUEST_SERVER( url );
    auto               received = std::make_shared<std::promise<void>>();
    auto               once = std::make_shared<std::once_flag>();
    server->SetCallback( [received, once]( std::string* ) { std::call_once( *once, [&]() { received->set_value(); } ); } ); // no Reply
    server->SetStopHookForTest( []() { std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) ); } );
    BOOST_REQUIRE( server->Start() );

    nng_socket client;
    BOOST_REQUIRE_EQUAL( nng_req0_open( &client ), 0 );
    nng_socket_set_ms( client, NNG_OPT_RECVTIMEO, 3000 );
    BOOST_REQUIRE_EQUAL( nng_dial( client, url.c_str(), nullptr, 0 ), 0 );
    std::string request = "a poll during the exit";
    BOOST_REQUIRE_EQUAL( nng_send( client, request.data(), request.size(), 0 ), 0 );

    BOOST_REQUIRE( received->get_future().wait_for( std::chrono::seconds( 5 ) ) == std::future_status::ready );
    std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) ); // the listener in its wait for the reply

    // (the stopping thread owns what it touches: on a hang it outlives this test)
    auto stopped = std::make_shared<std::promise<void>>();
    std::future<void> done = stopped->get_future();
    std::thread( [server, stopped]() { server->Stop(); stopped->set_value(); } ).detach();
    bool returned = done.wait_for( std::chrono::seconds( 3 ) ) == std::future_status::ready;
    BOOST_CHECK_MESSAGE( returned, "Stop() returns while a received request waits for its reply" );

    nng_close( client );

    if( returned )
        delete server;
}

/// An empty reply is a reply: Reply("") wakes the listener, which sends it (the client gets an empty message, not a timeout)
BOOST_AUTO_TEST_CASE( AnEmptyReplyIsSent )
{
    KI_TEST::SCOPED_TEMP_DIR tempDir( "kicad_qa_kinng" );
    std::string              url = "ipc://" + ( tempDir.Path() / "kinng-empty.sock" ).string();
    KINNG_REQUEST_SERVER     server( url );
    server.SetCallback( [&]( std::string* ) { server.Reply( "" ); } );
    BOOST_REQUIRE( server.Start() );

    nng_socket client;
    BOOST_REQUIRE_EQUAL( nng_req0_open( &client ), 0 );
    nng_socket_set_ms( client, NNG_OPT_RECVTIMEO, 3000 );
    BOOST_REQUIRE_EQUAL( nng_dial( client, url.c_str(), nullptr, 0 ), 0 );
    std::string request = "x";
    BOOST_REQUIRE_EQUAL( nng_send( client, request.data(), request.size(), 0 ), 0 );
    char*  buf = nullptr;
    size_t sz = 0;
    BOOST_CHECK_EQUAL( nng_recv( client, &buf, &sz, NNG_FLAG_ALLOC ), 0 );
    BOOST_CHECK_EQUAL( sz, 0u );

    if( buf )
        nng_free( buf, sz );

    nng_close( client );
}

BOOST_AUTO_TEST_SUITE_END()
