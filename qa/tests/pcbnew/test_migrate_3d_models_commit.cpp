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

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <pcbnew_utils/board_test_utils.h>

#include <board.h>
#include <board_commit.h>
#include <footprint.h>
#include <zone.h>
#include <lset.h>
#include <settings/settings_manager.h>
#include <tool/tool_manager.h>
#include <dialogs/dialog_migrate_3d_models.h>

#include <map>
#include <string>


namespace
{

/// Every zone by id: whether it is a teardrop, its outline's hash and each copper layer's fill hash
std::map<wxString, std::string> zoneState( BOARD* aBoard )
{
    std::map<wxString, std::string> state;

    for( ZONE* zone : aBoard->Zones() )
    {
        std::string s = zone->IsTeardropArea() ? "teardrop " : "zone ";
        s += zone->Outline()->GetHash().ToString();

        for( PCB_LAYER_ID layer : zone->GetLayerSet().Seq() )
        {
            if( !IsCopperLayer( layer ) )
                continue;

            zone->BuildHashValue( layer );
            s += " " + zone->GetHashValue( layer ).ToString();
        }

        state[zone->m_Uuid.AsString()] = s;
    }

    return state;
}


/// What the 3D model migration does to a board: every footprint's model paths rewritten, in one
/// board-editor commit pushed with @a aFlags
int rewriteModelPaths( BOARD* aBoard, int aFlags )
{
    TOOL_MANAGER mgr;
    mgr.SetEnvironment( aBoard, nullptr, nullptr, nullptr, nullptr );

    BOARD_COMMIT commit( &mgr, true, false );
    int          rewritten = 0;

    for( FOOTPRINT* fp : aBoard->Footprints() )
    {
        if( fp->Models().empty() )
            continue;

        commit.Modify( fp );

        for( FP_3DMODEL& model : fp->Models() )
        {
            model.m_Filename = model.m_Filename.BeforeLast( '.' ) + wxS( ".step" );
            ++rewritten;
        }
    }

    commit.Push( wxS( "Migrate 3D Model References" ), aFlags | SKIP_UNDO | SKIP_SET_DIRTY );
    return rewritten;
}

} // namespace


BOOST_AUTO_TEST_SUITE( Migrate3DModels )


/**
 * demos/complex_hierarchy: 68 footprints with KICAD6 .wrl models and 165 teardrops on their pads.
 * Opening it in KiCad 10's editor migrated the models in a plain commit, which treated the
 * footprints as changed copper: every teardrop was removed and refitted and the GND fill was not
 * poured again around them (45 clearance errors kicad-cli never saw).  The migration's own commit
 * flags must leave every zone, teardrops included, exactly as loaded.
 */
BOOST_AUTO_TEST_CASE( ModelPathMigrationKeepsTeardropsAndFills )
{
    SETTINGS_MANAGER       settingsManager;
    std::unique_ptr<BOARD> board;

    KI_TEST::LoadBoard( settingsManager, "../../../demos/complex_hierarchy/complex_hierarchy", board );
    BOOST_REQUIRE( board );
    board->BuildConnectivity();

    const std::map<wxString, std::string> before = zoneState( board.get() );
    int teardrops = 0;

    for( const auto& [id, s] : before )
        teardrops += s.rfind( "teardrop ", 0 ) == 0;

    BOOST_REQUIRE_GT( teardrops, 100 );

    int rewritten = rewriteModelPaths( board.get(), DIALOG_MIGRATE_3D_MODELS::COMMIT_FLAGS );
    BOOST_REQUIRE_GT( rewritten, 0 );

    const std::map<wxString, std::string> after = zoneState( board.get() );

    BOOST_CHECK_EQUAL( after.size(), before.size() );

    int changed = 0;

    for( const auto& [id, s] : before )
    {
        auto it = after.find( id );

        if( it == after.end() || it->second != s )
            ++changed;
    }

    BOOST_CHECK_MESSAGE( changed == 0, changed << " of " << before.size()
                                               << " zones (teardrops included) changed or gone" );

    // The control: the same rewrite pushed as a plain commit refits teardrops.  A warning only, so
    // a QA environment that never refits them cannot fail this case; it shows the case can see it.
    rewriteModelPaths( board.get(), 0 );
    BOOST_WARN_MESSAGE( zoneState( board.get() ) != after,
                        "control: a plain commit left every zone as it was here" );
}


BOOST_AUTO_TEST_SUITE_END()
