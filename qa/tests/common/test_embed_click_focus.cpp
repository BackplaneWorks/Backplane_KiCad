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
 * #1198 (bp.13.3 b2): the click that activates an inactive embedded KiCad acts as the click it was.
 *
 * KIPLATFORM::EMBED::KeyWaitsForGesture: the key request a click makes on an inactive KiCad (macOS)
 * waits while the click's button is held or a menu it opened tracks, so the frame becoming key never
 * lands between the click's down and its up.  KIPLATFORM::EMBED::FocusStays: once it is applied,
 * wx's focus stays where it is when it is already on what was clicked or inside it (the GL canvas
 * inside the draw panel), so the draw panel never loses it to its own canvas mid-click.
 */

#define BOOST_TEST_NO_MAIN
#include <boost/test/unit_test.hpp>
#include <qa_utils/wx_utils/unit_test_utils.h>

#include <kiplatform/embed.h>

#include <wx/frame.h>
#include <wx/panel.h>


BOOST_AUTO_TEST_SUITE( EmbedClickFocus )


BOOST_AUTO_TEST_CASE( KeyWaitsWhileTheClickGoesOn )
{
    using KIPLATFORM::EMBED::KeyWaitsForGesture;

    BOOST_CHECK( !KeyWaitsForGesture( 0, false ) );   // the click ended, no menu: the key now
    BOOST_CHECK( KeyWaitsForGesture( 1, false ) );    // left button held (a click, a drag)
    BOOST_CHECK( KeyWaitsForGesture( 2, false ) );    // right button held
    BOOST_CHECK( KeyWaitsForGesture( 1 | 2, false ) );
    BOOST_CHECK( KeyWaitsForGesture( 0, true ) );     // the right click's context menu is open
}


BOOST_AUTO_TEST_CASE( FocusStaysOnWhatWasClickedOrInsideIt )
{
    using KIPLATFORM::EMBED::FocusStays;

    if( !KI_TEST::CanDoDisplayTests() )
    {
        BOOST_TEST_MESSAGE( "Skipping test - no display available" );
        return;
    }

    // a frame, its draw panel, the GL canvas inside the panel, and a toolbar beside the panel
    wxFrame* frame = new wxFrame( nullptr, wxID_ANY, wxS( "embed click focus" ) );
    wxPanel* panel = new wxPanel( frame );
    wxPanel* canvas = new wxPanel( panel );
    wxPanel* toolbar = new wxPanel( frame );

    BOOST_CHECK( FocusStays( panel, panel ) );     // already on what was clicked
    BOOST_CHECK( FocusStays( canvas, panel ) );    // inside the panel clicked: the canvas keeps it
    BOOST_CHECK( FocusStays( panel, canvas ) );    // the canvas clicked inside the focused panel
    BOOST_CHECK( !FocusStays( toolbar, panel ) );  // elsewhere in the frame: to the panel
    BOOST_CHECK( !FocusStays( nullptr, panel ) );  // nothing focused: to the panel
    BOOST_CHECK( FocusStays( toolbar, nullptr ) ); // nothing clicked to give it to

    frame->Destroy();
}


BOOST_AUTO_TEST_SUITE_END()
