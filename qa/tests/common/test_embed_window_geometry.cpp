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

/**
 * @file test_embed_window_geometry.cpp
 * Backplane #1185: an embedded KiCad never saves its window geometry or AUI panel sizes into
 * the person's settings (pcbnew.json, eeschema.json, 3d_viewer.json); every other setting is
 * saved as usual.
 */

#include <qa_utils/wx_utils/unit_test_utils.h>

#include <settings/app_settings.h>
#include <settings/parameters.h>

#include <json_common.h>
#include <fstream>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>


namespace
{
struct TEST_APP_SETTINGS : public APP_SETTINGS_BASE
{
    TEST_APP_SETTINGS() : APP_SETTINGS_BASE( "embed_geometry_test", 0 )
    {
        m_params.emplace_back( new PARAM<int>( "aui.right_panel_width", &m_rightPanelWidth, -1 ) );
        m_params.emplace_back( new PARAM<bool>( "aui.show_layer_manager", &m_showLayers, true ) );
        m_params.emplace_back( new PARAM<int>( "other.value", &m_other, 0 ) );
    }

    int  m_rightPanelWidth = -1;
    bool m_showLayers = true;
    int  m_other = 0;
};


struct KEEP_GEOMETRY_RESET
{
    ~KEEP_GEOMETRY_RESET() { JSON_SETTINGS::SetKeepWindowGeometry( false ); }
};


nlohmann::json readJson( const wxString& aPath )
{
    std::ifstream in( aPath.fn_str() );
    return nlohmann::json::parse( in );
}


wxString makeDir()
{
    wxFileName dir( wxStandardPaths::Get().GetTempDir(), wxEmptyString );
    dir.AppendDir( wxString::Format( "kicad_embed_geometry_%lu", (unsigned long) wxGetProcessId() ) );
    dir.Mkdir( wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL );
    return dir.GetPath( wxPATH_GET_SEPARATOR );
}


// The person's file before embedding, then an embedded session's values (#1185's Windows numbers).
nlohmann::json personFile()
{
    return nlohmann::json::parse( R"({
        "meta": { "filename": "embed_geometry_test.json", "version": 0 },
        "window": { "pos_x": 100, "pos_y": 80, "size_x": 1400, "size_y": 900, "maximized": false,
                    "display": 0, "perspective": "", "aui_state": { "panes": [] } },
        "aui": { "right_panel_width": 250, "show_layer_manager": true },
        "other": { "value": 1 }
    })" );
}


void embeddedSession( TEST_APP_SETTINGS& aSettings )
{
    aSettings.m_Window.state.pos_x = -1137246128;
    aSettings.m_Window.state.size_x = 2071176320;
    aSettings.m_Window.state.size_y = 32245;
    aSettings.m_Window.state.maximized = true;
    aSettings.m_Window.aui_state = nlohmann::json::parse( R"({ "panes": [ { "w": 1239146100 } ] })" );
    aSettings.m_rightPanelWidth = 1239146100;
    aSettings.m_showLayers = false;
    aSettings.m_other = 7;
}
} // namespace


BOOST_AUTO_TEST_SUITE( EmbedWindowGeometry )


// #1185: the stock keys keep the person's geometry; the embedded frame's own goes to "embed",
// except what is out of range.
BOOST_AUTO_TEST_CASE( EmbeddedSaveKeepsPersonsGeometry )
{
    KEEP_GEOMETRY_RESET reset;
    wxString            dir = makeDir();
    wxString            file = dir + wxS( "embed_geometry_test.json" );

    {
        std::ofstream out( file.fn_str() );
        out << personFile().dump( 2 );
    }

    JSON_SETTINGS::SetKeepWindowGeometry( true );
    TEST_APP_SETTINGS settings;
    settings.LoadFromFile( dir );
    BOOST_REQUIRE_EQUAL( settings.m_Window.state.pos_x, 100 );

    embeddedSession( settings );
    settings.m_Window.state.pos_y = 12;     // a sane embedded value: kept for the embedded frame
    BOOST_CHECK( settings.SaveToFile( dir ) );

    nlohmann::json saved = readJson( file );

    // Stock geometry: as the person's file had it.
    BOOST_CHECK_EQUAL( saved["window"]["pos_x"].get<int>(), 100 );
    BOOST_CHECK_EQUAL( saved["window"]["pos_y"].get<int>(), 80 );
    BOOST_CHECK_EQUAL( saved["window"]["size_x"].get<int>(), 1400 );
    BOOST_CHECK_EQUAL( saved["window"]["size_y"].get<int>(), 900 );
    BOOST_CHECK_EQUAL( saved["window"]["maximized"].get<bool>(), false );
    BOOST_CHECK( saved["window"]["aui_state"] == personFile()["window"]["aui_state"] );
    BOOST_CHECK_EQUAL( saved["aui"]["right_panel_width"].get<int>(), 250 );

    // The embedded frame's: sane values only.
    BOOST_CHECK_EQUAL( saved["window"]["embed"]["pos_y"].get<int>(), 12 );
    BOOST_CHECK_EQUAL( saved["window"]["embed"]["maximized"].get<bool>(), true );
    BOOST_CHECK( !saved["window"]["embed"].contains( "pos_x" ) );
    BOOST_CHECK( !saved["window"]["embed"].contains( "size_x" ) );
    BOOST_CHECK( !saved["window"]["embed"].contains( "aui_state" ) );
    BOOST_CHECK( !saved["aui"].contains( "embed" ) || !saved["aui"]["embed"].contains( "right_panel_width" ) );

    // Everything else: saved as usual.
    BOOST_CHECK_EQUAL( saved["aui"]["show_layer_manager"].get<bool>(), false );
    BOOST_CHECK_EQUAL( saved["other"]["value"].get<int>(), 7 );
    BOOST_CHECK( saved["window"].contains( "zoom_factors" ) );

    wxRemoveFile( file );
}


// An embedded-only person keeps their layout: a panel resized inside Backplane comes back at the
// next embedded start, while a standalone KiCad still opens with its own.
BOOST_AUTO_TEST_CASE( EmbeddedLayoutComesBack )
{
    KEEP_GEOMETRY_RESET reset;
    wxString            dir = makeDir();
    wxString            file = dir + wxS( "embed_geometry_test.json" );

    {
        std::ofstream out( file.fn_str() );
        out << personFile().dump( 2 );
    }

    JSON_SETTINGS::SetKeepWindowGeometry( true );

    {
        TEST_APP_SETTINGS first;
        first.LoadFromFile( dir );
        first.m_rightPanelWidth = 320;
        first.m_Window.aui_state = nlohmann::json::parse( R"({ "panes": [ { "dock": { "size": 320, "proportion": 100000 } } ] })" );
        BOOST_CHECK( first.SaveToFile( dir ) );
    }

    {
        TEST_APP_SETTINGS next;
        next.LoadFromFile( dir );
        BOOST_CHECK_EQUAL( next.m_rightPanelWidth, 320 );
        BOOST_CHECK_EQUAL( next.m_Window.aui_state["panes"][0]["dock"]["size"].get<int>(), 320 );
        BOOST_CHECK_EQUAL( next.m_Window.state.pos_x, 100 );    // never embedded-saved: the stock one
    }

    // A standalone KiCad: its own layout, and the embed object carried along untouched.
    JSON_SETTINGS::SetKeepWindowGeometry( false );
    TEST_APP_SETTINGS standalone;
    standalone.LoadFromFile( dir );
    BOOST_CHECK_EQUAL( standalone.m_rightPanelWidth, 250 );
    BOOST_CHECK( standalone.m_Window.aui_state == personFile()["window"]["aui_state"] );
    standalone.m_Window.state.pos_x = 300;
    standalone.m_other = 9;
    BOOST_CHECK( standalone.SaveToFile( dir ) );

    nlohmann::json saved = readJson( file );
    BOOST_CHECK_EQUAL( saved["window"]["pos_x"].get<int>(), 300 );
    BOOST_CHECK_EQUAL( saved["aui"]["embed"]["right_panel_width"].get<int>(), 320 );

    wxRemoveFile( file );
}


// Without embedding, KiCad saves its geometry as before.
BOOST_AUTO_TEST_CASE( StandaloneSaveWritesGeometry )
{
    KEEP_GEOMETRY_RESET reset;
    wxString            dir = makeDir();
    wxString            file = dir + wxS( "embed_geometry_test.json" );

    {
        std::ofstream out( file.fn_str() );
        out << personFile().dump( 2 );
    }

    TEST_APP_SETTINGS settings;
    settings.LoadFromFile( dir );
    settings.m_Window.state.pos_x = 300;
    settings.m_rightPanelWidth = 320;
    BOOST_CHECK( settings.SaveToFile( dir ) );

    nlohmann::json saved = readJson( file );
    BOOST_CHECK_EQUAL( saved["window"]["pos_x"].get<int>(), 300 );
    BOOST_CHECK_EQUAL( saved["aui"]["right_panel_width"].get<int>(), 320 );
    BOOST_CHECK( !saved["window"].contains( "embed" ) );

    wxRemoveFile( file );
}


// A maximized window on the person's second monitor (left of the first) stays there; the 3D
// viewer's and the simulator's windows (3d_viewer.json, eeschema.json's simulator.window) are
// treated as every other window; a standalone KiCad's change while embedded is kept, and geometry
// the file never had stays out of it.  A frame popped out of the pane is the same process, so it
// saves the same way.
BOOST_AUTO_TEST_CASE( StashFollowsDisk )
{
    nlohmann::json store = nlohmann::json::parse( R"({
        "window": { "pos_x": -30000, "pos_y": -30000, "size_x": 804, "size_y": 952, "maximized": false,
                    "display": 0, "grid": { "last_size": 3 } },
        "simulator": { "window": { "pos_x": 924, "pos_y": 98, "size_x": 804, "size_y": 952 } },
        "lib_view": { "window": { "pos_x": -30000, "pos_y": 1, "size_x": 5, "size_y": 6 } },
        "aui": { "net_nav_panel_docked_size": { "width": 9, "height": 9 }, "right_panel_width": 190,
                 "show_search": true },
        "design_block_chooser": { "height": 32765, "width": 0, "sort_mode": 1 }
    })" );
    nlohmann::json disk = nlohmann::json::parse( R"({
        "window": { "pos_x": -2560, "pos_y": 0, "size_x": 2560, "size_y": 1440, "maximized": true,
                    "display": 1, "grid": { "last_size": 1 } },
        "simulator": { "window": { "pos_x": 40, "pos_y": 40, "size_x": 500, "size_y": 400 } },
        "lib_view": { "window": { "pos_x": 12, "size_x": 500, "size_y": 400 } },
        "aui": { "net_nav_panel_docked_size": { "width": 296, "height": 411 }, "right_panel_width": 274,
                 "show_search": false },
        "design_block_chooser": { "height": 765, "width": 990, "sort_mode": 0 }
    })" );

    JSON_SETTINGS::StashWindowGeometry( store, &disk );

    BOOST_CHECK( store["window"]["pos_x"] == disk["window"]["pos_x"] );
    BOOST_CHECK_EQUAL( store["window"]["maximized"].get<bool>(), true );
    BOOST_CHECK_EQUAL( store["window"]["display"].get<int>(), 1 );
    BOOST_CHECK_EQUAL( store["window"]["grid"]["last_size"].get<int>(), 3 );
    BOOST_CHECK( !store["window"]["embed"].contains( "pos_x" ) );     // the park position
    BOOST_CHECK_EQUAL( store["window"]["embed"]["size_x"].get<int>(), 804 );
    BOOST_CHECK_EQUAL( store["simulator"]["window"]["pos_x"].get<int>(), 40 );
    BOOST_CHECK_EQUAL( store["simulator"]["window"]["embed"]["pos_x"].get<int>(), 924 );
    BOOST_CHECK_EQUAL( store["lib_view"]["window"]["pos_x"].get<int>(), 12 );
    BOOST_CHECK( !store["lib_view"]["window"].contains( "pos_y" ) );
    BOOST_CHECK_EQUAL( store["aui"]["net_nav_panel_docked_size"]["width"].get<int>(), 296 );
    BOOST_CHECK_EQUAL( store["aui"]["right_panel_width"].get<int>(), 274 );
    BOOST_CHECK_EQUAL( store["aui"]["embed"]["right_panel_width"].get<int>(), 190 );
    BOOST_CHECK_EQUAL( store["aui"]["show_search"].get<bool>(), true );
    // bp-win's rc.7 embed run: the design-blocks panel at 32765 px tall, 0 wide
    BOOST_CHECK_EQUAL( store["design_block_chooser"]["height"].get<int>(), 765 );
    BOOST_CHECK_EQUAL( store["design_block_chooser"]["width"].get<int>(), 990 );
    BOOST_CHECK( !store["design_block_chooser"]["embed"].contains( "height" ) );
    BOOST_CHECK_EQUAL( store["design_block_chooser"]["embed"]["width"].get<int>(), 0 );
    BOOST_CHECK_EQUAL( store["design_block_chooser"]["sort_mode"].get<int>(), 1 );

    // Loaded embedded again: the embedded frame's own values, the park position never.
    JSON_SETTINGS::UnstashWindowGeometry( store );
    BOOST_CHECK_EQUAL( store["window"]["size_x"].get<int>(), 804 );
    BOOST_CHECK( store["window"]["pos_x"] == disk["window"]["pos_x"] );
    BOOST_CHECK_EQUAL( store["aui"]["right_panel_width"].get<int>(), 190 );

    // No file yet: the stock geometry is left out, so KiCad's defaults apply when it next starts.
    nlohmann::json fresh = nlohmann::json::parse( R"({ "window": { "pos_x": -30000, "size_x": 5,
            "size_y": 6, "mru_path": "/p", "perspective": "x;bestw=1239146100;prop=100000|" },
            "lib_view": { "window": { "size_x": 5, "size_y": 6,
                    "perspective": "layout2|name=Props;state=2099196;prop=100000;bestw=274|dock_size(2,0,0)=276|" } },
            "aui": { "right_panel_width": 9 } })" );
    JSON_SETTINGS::StashWindowGeometry( fresh, nullptr );
    BOOST_CHECK( !fresh["window"].contains( "pos_x" ) );
    BOOST_CHECK( !fresh["window"].contains( "size_x" ) );
    BOOST_CHECK( !fresh["window"]["embed"].contains( "perspective" ) );
    BOOST_CHECK( fresh["lib_view"]["window"]["embed"].contains( "perspective" ) );   // state bits, weights: sane
    BOOST_CHECK_EQUAL( fresh["window"]["mru_path"].get<std::string>(), "/p" );
    BOOST_CHECK( !fresh["aui"].contains( "right_panel_width" ) );
    BOOST_CHECK_EQUAL( fresh["aui"]["embed"]["right_panel_width"].get<int>(), 9 );
}


BOOST_AUTO_TEST_SUITE_END()
