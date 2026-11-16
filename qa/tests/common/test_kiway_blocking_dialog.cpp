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

#define BOOST_TEST_NO_MAIN
#include <boost/test/unit_test.hpp>
#include <qa_utils/wx_utils/unit_test_utils.h>

#include <kiway.h>

#include <wx/dialog.h>


BOOST_AUTO_TEST_SUITE( KiwayBlockingDialog )


// Every DIALOG_SHIM names itself KIWAY's blocking dialog when built and clears it only when
// destroyed, so a modeless dialog closed by hiding it (ERC, DRC) stayed "blocking", and showing
// another editor (COMMON_CONTROL's showFrame) raised it, which showed it again (lane B, bp.13).
// Only a dialog on screen is blocking.
BOOST_AUTO_TEST_CASE( HiddenDialogIsNotBlocking )
{
    if( !KI_TEST::CanDoDisplayTests() )
    {
        BOOST_TEST_MESSAGE( "Skipping test - no display available" );
        return;
    }

    KIWAY     kiway( KFCTL_STANDALONE );
    wxDialog* dialog = new wxDialog( nullptr, wxID_ANY, wxS( "Blocking dialog test" ) );

    kiway.SetBlockingDialog( dialog );
    BOOST_CHECK_MESSAGE( kiway.GetBlockingDialog() == nullptr, "a dialog never shown is not blocking" );

    dialog->Show( true );
    BOOST_CHECK( kiway.GetBlockingDialog() == dialog );

    // Closed by hiding, as Escape closes the ERC and DRC dialogs: no longer blocking
    dialog->Show( false );
    BOOST_CHECK_MESSAGE( kiway.GetBlockingDialog() == nullptr, "a hidden dialog is not blocking" );

    // Released with Destroy(): deleting a top-level directly crashes without an event loop
    dialog->Destroy();
}


BOOST_AUTO_TEST_SUITE_END()
