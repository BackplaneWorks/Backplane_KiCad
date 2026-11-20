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
 * The user's Documents base (PATHS::GetUserDocumentsBase): KICAD_DOCUMENTS_HOME when it is set,
 * else the platform's Documents folder; and the start-up search (SystemDirsAppend, run at every
 * kiface start) takes it from there, so a KiCad started with the variable set never touches the
 * real Documents (macOS guards it with a prompt: the Mac mini's "KiCad would like to access files
 * in your Documents folder").
 */

#include <boost/test/unit_test.hpp>

#include <paths.h>
#include <search_stack.h>
#include <systemdirsappend.h>
#include <kiplatform/environment.h>

#include <wx/filename.h>
#include <wx/utils.h>


namespace
{
/// KICAD_DOCUMENTS_HOME set (or unset) for one test, as it was after
struct DOCUMENTS_HOME
{
    DOCUMENTS_HOME( const wxString* aValue )
    {
        m_had = wxGetEnv( wxS( "KICAD_DOCUMENTS_HOME" ), &m_was );

        if( aValue )
            wxSetEnv( wxS( "KICAD_DOCUMENTS_HOME" ), *aValue );
        else
            wxUnsetEnv( wxS( "KICAD_DOCUMENTS_HOME" ) );
    }

    ~DOCUMENTS_HOME()
    {
        if( m_had )
            wxSetEnv( wxS( "KICAD_DOCUMENTS_HOME" ), m_was );
        else
            wxUnsetEnv( wxS( "KICAD_DOCUMENTS_HOME" ) );
    }

    bool     m_had = false;
    wxString m_was;
};


bool underPath( const wxString& aPath, const wxString& aBase )
{
    wxFileName path( aPath, wxEmptyString ), base( aBase, wxEmptyString );
    path.Normalize( wxPATH_NORM_ABSOLUTE | wxPATH_NORM_DOTS );
    base.Normalize( wxPATH_NORM_ABSOLUTE | wxPATH_NORM_DOTS );
    return path.GetPath().StartsWith( base.GetPath() );
}
} // namespace


BOOST_AUTO_TEST_SUITE( DocumentsBase )


BOOST_AUTO_TEST_CASE( TheVariableWinsWhenSet )
{
    wxString        mine = wxFileName::GetTempDir() + wxS( "/kicad-documents-base-test" );
    DOCUMENTS_HOME  env( &mine );

    BOOST_CHECK_EQUAL( PATHS::GetUserDocumentsBase(), mine );
    BOOST_CHECK( underPath( PATHS::GetUserPluginsPath(), mine ) );   // the user paths follow it
}


BOOST_AUTO_TEST_CASE( UnsetOrEmptyIsThePlatformsDocuments )
{
    {
        DOCUMENTS_HOME env( nullptr );
        BOOST_CHECK_EQUAL( PATHS::GetUserDocumentsBase(), KIPLATFORM::ENV::GetDocumentsPath() );
    }
    {
        wxString       empty;
        DOCUMENTS_HOME env( &empty );
        BOOST_CHECK_EQUAL( PATHS::GetUserDocumentsBase(), KIPLATFORM::ENV::GetDocumentsPath() );
    }
}


BOOST_AUTO_TEST_CASE( TheStartUpSearchNeverListsTheRealDocumentsWithTheVariableSet )
{
    wxString real = KIPLATFORM::ENV::GetDocumentsPath();
    wxString mine = wxFileName::GetTempDir() + wxS( "/kicad-documents-base-search" );
    wxFileName::Mkdir( mine, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL );

    DOCUMENTS_HOME env( &mine );
    SEARCH_STACK   stack;
    SystemDirsAppend( &stack );

    bool listsMine = false;

    for( unsigned i = 0; i < stack.GetCount(); ++i )
    {
        if( !real.IsEmpty() && !underPath( mine, real ) )
            BOOST_CHECK_MESSAGE( !underPath( stack[i], real ), "searched under the real Documents: " << stack[i] );

        listsMine |= underPath( stack[i], mine );
    }

    BOOST_CHECK( listsMine );   // the variable's folder is searched instead

    wxFileName::Rmdir( mine, wxPATH_RMDIR_RECURSIVE );
}


BOOST_AUTO_TEST_SUITE_END()
