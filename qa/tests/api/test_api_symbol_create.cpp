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

// CreateItems of a schematic symbol from its library id over the headless api-server: the
// token benchmark's repros 4 (fields sent with only their text), 6 (a derived symbol) and 7 (a
// Footprint user field).

#include "api_e2e_utils.h"

#include <boost/test/unit_test.hpp>
#include <qa_utils/file_utils.h>
#include <wx/ffile.h>
#include <wx/filefn.h>
#include <wx/filename.h>

#include <api/common/commands/editor_commands.pb.h>
#include <api/common/commands/project_commands.pb.h>
#include <api/schematic/schematic_types.pb.h>


namespace
{

namespace sch = kiapi::schematic::types;
namespace cmd = kiapi::common::commands;


/// A project of its own with the QA Device library in its symbol library table
class TEMP_SYMBOL_PROJECT
{
public:
    TEMP_SYMBOL_PROJECT() : m_root( "kicad_api_symbols" ) {}

    bool Create( wxString* aError )
    {
        wxString src = wxString::FromUTF8( KI_TEST::GetTestDataRootDir() ) + wxS( "libraries/" );
        wxString dst = m_root.PathStr() + wxFileName::GetPathSeparator();

        if( !wxCopyFile( src + wxS( "test_project/test_project.kicad_sch" ), dst + wxS( "symbols.kicad_sch" ) )
            || !wxCopyFile( src + wxS( "test_project/test_project.kicad_pro" ), dst + wxS( "symbols.kicad_pro" ) )
            || !wxCopyFile( src + wxS( "Device.kicad_sym" ), dst + wxS( "Device.kicad_sym" ) ) )
        {
            *aError = wxS( "could not copy the fixture" );
            return false;
        }

        wxFFile table( dst + wxS( "sym-lib-table" ), wxS( "w" ) );

        if( !table.IsOpened()
            || !table.Write( wxS( "(sym_lib_table\n  (version 7)\n  (lib (name \"Device\")(type \"KiCad\")"
                                  "(uri \"${KIPRJMOD}/Device.kicad_sym\")(options \"\")(descr \"\"))\n)\n" ) ) )
        {
            *aError = wxS( "could not write sym-lib-table" );
            return false;
        }

        m_schematic = dst + wxS( "symbols.kicad_sch" );
        return true;
    }

    const wxString& Schematic() const { return m_schematic; }

private:
    KI_TEST::SCOPED_TEMP_DIR m_root;
    wxString                 m_schematic;
};


sch::SchematicSymbolInstance librarySymbol( const std::string& aEntry, int64_t aX, int64_t aY )
{
    sch::SchematicSymbolInstance symbol;
    symbol.mutable_position()->set_x_nm( aX );
    symbol.mutable_position()->set_y_nm( aY );
    symbol.mutable_definition()->mutable_id()->set_library_nickname( "Device" );
    symbol.mutable_definition()->mutable_id()->set_entry_name( aEntry );
    symbol.mutable_lib_id()->set_library_nickname( "Device" );
    symbol.mutable_lib_id()->set_entry_name( aEntry );
    symbol.mutable_unit()->set_unit( 1 );
    return symbol;
}


/// CreateItems of one symbol; the created symbol as KiCad answers it
bool create( API_TEST_CLIENT& aClient, const kiapi::common::types::DocumentSpecifier& aDocument,
             const sch::SchematicSymbolInstance& aSymbol, sch::SchematicSymbolInstance* aCreated,
             std::string* aError )
{
    cmd::CreateItems request;
    *request.mutable_header()->mutable_document() = aDocument;
    request.add_items()->PackFrom( aSymbol );

    kiapi::common::ApiResponse response;

    if( !aClient.SendCommand( request, &response ) )
    {
        *aError = aClient.LastError().ToStdString();
        return false;
    }

    cmd::CreateItemsResponse created;

    if( response.status().status() != kiapi::common::AS_OK || !response.message().UnpackTo( &created )
        || created.created_items_size() != 1 )
    {
        *aError = response.status().error_message();
        return false;
    }

    if( created.created_items( 0 ).status().code() != cmd::ISC_OK )
    {
        *aError = created.created_items( 0 ).status().error_message();
        return false;
    }

    return created.created_items( 0 ).item().UnpackTo( aCreated );
}

} // namespace


BOOST_AUTO_TEST_SUITE( ApiSymbolCreate )


BOOST_FIXTURE_TEST_CASE( LibrarySymbolFieldsAndDerivedSymbols, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    TEMP_SYMBOL_PROJECT project;
    wxString            error;
    BOOST_REQUIRE_MESSAGE( project.Create( &error ), error );

    kiapi::common::types::DocumentSpecifier document;
    BOOST_REQUIRE_MESSAGE( Client().OpenDocument( project.Schematic(), kiapi::common::types::DOCTYPE_SCHEMATIC,
                                                  &document ),
                           "OpenDocument failed: " + Client().LastError() );

    std::string                  createError;
    sch::SchematicSymbolInstance plain;
    const int64_t                x0 = 50800000, y0 = 50800000;

    // No fields: placed from the library, the reference beside the body and shown
    BOOST_REQUIRE_MESSAGE( create( Client(), document, librarySymbol( "R", x0, y0 ), &plain, &createError ),
                           createError );
    const kiapi::common::types::Vector2& refPlain = plain.reference_field().text().position();
    BOOST_CHECK( refPlain.x_nm() != x0 || refPlain.y_nm() != y0 );
    BOOST_CHECK( plain.reference_field().visible() );

    // (4) Fields with only their text: placed as the library places them, the text kept
    const int64_t                x1 = x0 + 25400000;
    sch::SchematicSymbolInstance textOnly = librarySymbol( "R", x1, y0 );
    textOnly.mutable_reference_field()->set_name( "Reference" );
    textOnly.mutable_reference_field()->mutable_text()->set_text( "R92" );
    textOnly.mutable_value_field()->set_name( "Value" );
    textOnly.mutable_value_field()->mutable_text()->set_text( "10k" );

    sch::SchematicSymbolInstance created;
    BOOST_REQUIRE_MESSAGE( create( Client(), document, textOnly, &created, &createError ), createError );
    BOOST_CHECK_EQUAL( created.reference_field().text().text(), "R92" );
    BOOST_CHECK_EQUAL( created.value_field().text().text(), "10k" );
    BOOST_CHECK( created.reference_field().visible() );
    BOOST_CHECK( created.value_field().visible() );
    BOOST_CHECK_EQUAL( created.reference_field().text().position().x_nm() - x1, refPlain.x_nm() - x0 );
    BOOST_CHECK_EQUAL( created.reference_field().text().position().y_nm() - y0, refPlain.y_nm() - y0 );

    // (7) A Footprint user field is the footprint field
    sch::SchematicSymbolInstance withFootprint = librarySymbol( "R", x0, y0 + 25400000 );
    sch::SchematicField*         footprint = withFootprint.add_user_fields();
    footprint->set_name( "Footprint" );
    footprint->mutable_text()->set_text( "Resistor_SMD:R_0603_1608Metric" );

    BOOST_REQUIRE_MESSAGE( create( Client(), document, withFootprint, &created, &createError ), createError );
    BOOST_CHECK_EQUAL( created.footprint_field().text().text(), "Resistor_SMD:R_0603_1608Metric" );

    for( const sch::SchematicField& field : created.user_fields() )
        BOOST_CHECK_NE( field.name(), "Footprint" );

    // (6) A symbol that extends another is written flattened (no "extends" in lib_symbols)
    BOOST_REQUIRE_MESSAGE( create( Client(), document, librarySymbol( "Filter_EMI_C", x1, y0 + 25400000 ),
                                   &created, &createError ),
                           createError );

    cmd::SaveDocument save;
    *save.mutable_document() = document;
    kiapi::common::ApiResponse saved;
    BOOST_REQUIRE( Client().SendCommand( save, &saved ) );
    BOOST_REQUIRE_MESSAGE( saved.status().status() == kiapi::common::AS_OK, saved.status().error_message() );

    wxFFile  file( project.Schematic(), wxS( "r" ) );
    wxString text;
    BOOST_REQUIRE( file.IsOpened() && file.ReadAll( &text ) );
    BOOST_CHECK( text.Contains( wxS( "Device:Filter_EMI_C" ) ) );
    BOOST_CHECK_MESSAGE( !text.Contains( wxS( "(extends" ) ), "lib_symbols holds the derived symbol unflattened" );

    Client().CloseAllDocuments( true );
}


BOOST_AUTO_TEST_SUITE_END()
