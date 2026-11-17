/*
 * This program source code file is part of KiCad, a free EDA CAD application.
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

/*
 * Simulation over the API (kiapi.backplane.sim, capability sim-run): the models of the symbols,
 * the netlist, the saved set-up and the run.  KiCad's own code does the work (SIM_LIB_MGR,
 * SIM_MODEL and its serializer, the SPICE netlist exporter, NGSPICE); this file asks it and
 * reports.  Served by the schematic editor and by the headless server alike.
 */

#include <api/api_handler_sch.h>
#include <api/api_sch_sim.h>
#include <api/api_utils.h>
#include <api/sch_context.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>

#include <fmt/format.h>
#include <json_common.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/tokenzr.h>

#include <kiway.h>
#include <pgm_base.h>
#include <settings/settings_manager.h>
#include <project.h>
#include <project/project_file.h>
#include <richio.h>
#include <sch_commit.h>
#include <sch_edit_frame.h>
#include <sch_pin.h>
#include <sch_reference_list.h>
#include <sch_screen.h>
#include <sch_sheet_path.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <schematic_settings.h>
#include <string_utils.h>
#include <wildcards_and_files_ext.h>

#include <sim/sim_lib_mgr.h>
#include <sim/simulator_frame.h>
#include <tools/sch_actions.h>
#include <tool/tool_manager.h>
#include <backplane_embed.h>
#include <sim/sim_library.h>
#include <sim/sim_model.h>
#include <sim/sim_model_serializer.h>
#include <sim/spice_generator.h>
#include <sim/spice_settings.h>

using namespace kiapi::backplane::sim;
using kiapi::common::ApiResponseStatus;
using kiapi::common::ApiStatusCode;


namespace
{

ApiResponseStatus statusError( ApiStatusCode aCode, const std::string& aMessage )
{
    ApiResponseStatus e;
    e.set_status( aCode );
    e.set_error_message( aMessage );
    return e;
}


std::string utf8( const wxString& aText )
{
    return std::string( aText.ToUTF8() );
}


wxString oneLine( wxString aText )
{
    aText.Trim( true ).Trim( false );
    aText.Replace( wxS( "\r" ), wxS( "" ) );
    aText.Replace( wxS( "\n" ), wxS( "; " ) );
    return aText;
}


/// One symbol of the design: its first unit as the hierarchy gives it, and every unit
struct PART
{
    wxString                                             ref;
    SCH_SYMBOL*                                          symbol = nullptr;
    SCH_SHEET_PATH                                       sheet;
    std::vector<std::pair<SCH_SYMBOL*, SCH_SHEET_PATH>>  units;
};


std::vector<PART> collectParts( SCHEMATIC* aSchematic )
{
    std::vector<PART>          parts;
    std::map<wxString, size_t> byRef;

    for( const SCH_SHEET_PATH& sheet : aSchematic->Hierarchy() )
    {
        std::vector<SCH_SYMBOL*> symbols;

        for( SCH_ITEM* item : sheet.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
            symbols.push_back( static_cast<SCH_SYMBOL*>( item ) );

        std::sort( symbols.begin(), symbols.end(),
                   []( const SCH_SYMBOL* a, const SCH_SYMBOL* b )
                   {
                       if( a->GetUnit() != b->GetUnit() )
                           return a->GetUnit() < b->GetUnit();

                       return a->m_Uuid < b->m_Uuid;
                   } );

        for( SCH_SYMBOL* symbol : symbols )
        {
            wxString ref = symbol->GetRef( &sheet );

            // Power symbols are nets, not parts.  Everything else the exporter writes, a mounting
            // hole without a pin included (as "H1 __H1", which ngspice cannot read): it is listed,
            // so it can be given a model or excluded
            if( symbol->IsPower() || ref.StartsWith( wxS( "#" ) ) )
                continue;

            auto [it, added] = byRef.try_emplace( ref, parts.size() );

            if( added )
            {
                PART part;
                part.ref = ref;
                part.symbol = symbol;
                part.sheet = sheet;
                parts.push_back( std::move( part ) );
            }

            parts[it->second].units.emplace_back( symbol, sheet );
        }
    }

    std::sort( parts.begin(), parts.end(),
               []( const PART& a, const PART& b )
               {
                   return StrNumCmp( a.ref, b.ref, true ) < 0;
               } );

    return parts;
}


std::vector<wxString> sortedPinNumbers( const SCH_SYMBOL& aSymbol )
{
    std::vector<wxString> pins;

    for( const SCH_PIN* pin : aSymbol.GetAllLibPins() )
        pins.push_back( pin->GetNumber() );

    std::sort( pins.begin(), pins.end(),
               []( const wxString& lhs, const wxString& rhs )
               {
                   return StrNumCmp( lhs, rhs, true ) < 0;
               } );

    return pins;
}


wxString shownField( const PART& aPart, const wxString& aName, const wxString& aVariant )
{
    if( const SCH_FIELD* field = aPart.symbol->GetField( aName ) )
        return field->GetShownText( &aPart.sheet, FOR_NETNAME, aVariant );

    return wxEmptyString;
}


/// The structural reason a part has no usable model, beside KiCad's own message
ModelError classifyModelError( const PART& aPart, SIM_LIB_MGR& aMgr, const wxString& aVariant,
                               const wxString& aMessage, bool aNoModel )
{
    wxString library = shownField( aPart, SIM_LIBRARY_FIELD, aVariant );
    wxString name = shownField( aPart, SIM_NAME_FIELD, aVariant );

    if( !library.IsEmpty() )
    {
        NULL_REPORTER devnull;
        wxString      path = aMgr.ResolveLibraryPath( library, devnull );

        if( path.IsEmpty() || !wxFileName::FileExists( path ) )
            return ME_LIBRARY_NOT_FOUND;

        try
        {
            SIM_LIB_MGR        probe( &aPart.symbol->Schematic()->Project() );
            WX_STRING_REPORTER reporter;

            probe.SetLibrary( library, reporter );

            for( const auto& [libPath, lib] : probe.GetLibraries() )
            {
                if( !name.IsEmpty() && !lib.get().FindModel( utf8( name ) ) )
                    return ME_MODEL_NOT_IN_LIBRARY;
            }

            if( probe.GetLibraries().empty() )
                return ME_LIBRARY_NOT_FOUND;
        }
        catch( const IO_ERROR& )
        {
            return ME_LIBRARY_NOT_FOUND;
        }
    }

    // Sim.Pins naming a pin the symbol does not have
    wxString pins = shownField( aPart, SIM_PINS_FIELD, aVariant );

    if( !pins.IsEmpty() )
    {
        std::set<wxString> numbers;

        for( const SCH_PIN* pin : aPart.symbol->GetAllLibPins() )
            numbers.insert( pin->GetNumber() );

        wxStringTokenizer tokenizer( pins, wxS( " \t\r\n" ), wxTOKEN_STRTOK );

        while( tokenizer.HasMoreTokens() )
        {
            wxString token = tokenizer.GetNextToken();
            wxString number = token.BeforeFirst( '=' );

            if( token.Find( '=' ) == wxNOT_FOUND || !numbers.count( number ) )
                return ME_PIN_MISMATCH;
        }
    }

    if( aNoModel )
        return ME_NO_MODEL;

    wxString lower = aMessage.Lower();

    if( lower.Contains( wxS( "pin" ) ) )
        return ME_PIN_MISMATCH;

    if( lower.Contains( wxS( "param" ) ) || lower.Contains( wxS( "value" ) ) )
        return ME_BAD_PARAMS;

    return ME_OTHER;
}


ModelSource modelSource( const PART& aPart, const SIM_MODEL* aModel )
{
    auto stored = [&]( const wxString& aName ) -> wxString
    {
        if( const SCH_FIELD* field = aPart.symbol->GetField( aName ) )
            return field->GetText();

        return wxEmptyString;
    };

    if( !stored( SIM_LIBRARY_FIELD ).IsEmpty() )
        return MS_LIBRARY;

    if( stored( SIM_DEVICE_FIELD ).IsSameAs( wxS( "SPICE" ), false ) )
        return MS_RAW;

    if( !stored( SIM_DEVICE_FIELD ).IsEmpty() )
        return MS_BUILTIN;

    if( !stored( SIM_LEGACY_PRIMITIVE_FIELD ).IsEmpty() || !stored( SIM_LEGACY_MODEL_FIELD ).IsEmpty() )
        return MS_RAW;

    if( aModel && aModel->GetType() != SIM_MODEL::TYPE::NONE )
        return MS_INFERRED;

    return MS_NONE;
}


void packParams( const SIM_MODEL& aModel, google::protobuf::RepeatedPtrField<Param>* aOut )
{
    const SIM_MODEL::PARAM* tuner = aModel.GetTunerParam();

    for( int ii = 0; ii < aModel.GetParamCount(); ++ii )
    {
        const SIM_MODEL::PARAM& param = aModel.GetParam( ii );
        Param*                  out = aOut->Add();

        out->set_name( param.info.name );
        out->set_value( param.value );
        out->set_unit( param.info.unit );
        out->set_default_value( param.info.defaultValue );
        out->set_description( param.info.description );
        out->set_is_instance( param.info.isInstanceParam || param.info.isSpiceInstanceParam );
        out->set_is_principal( tuner ? ( &param == tuner )
                                     : ( ii == 0 && param.info.category == SIM_MODEL::PARAM::CATEGORY::PRINCIPAL ) );
    }
}


/// Describe one part.  aItem is its item in the netlist when it has one (its nets, its line).
void describePart( SCHEMATIC* aSchematic, const PART& aPart, SIM_LIB_MGR& aMgr, const SPICE_ITEM* aItem,
                   bool aIncludeParams, SymbolModel* aOut )
{
    wxString variant = aSchematic->GetCurrentVariant();

    aOut->set_reference( utf8( aPart.ref ) );
    aOut->set_value( utf8( aPart.symbol->GetValue( &aPart.sheet, FOR_NETNAME, variant ) ) );
    aOut->set_lib_id( utf8( aPart.symbol->GetLibId().Format() ) );
    aOut->set_sheet_path( utf8( aPart.sheet.PathHumanReadable() ) );
    aOut->mutable_id()->set_value( aPart.symbol->m_Uuid.AsStdString() );

    bool excluded = false;
    bool dnp = false;

    for( const auto& [unit, sheet] : aPart.units )
    {
        excluded |= unit->ResolveExcludedFromSim( &sheet, variant );
        dnp |= unit->ResolveDNP( &sheet, variant );
    }

    // KiCad drops the whole part only when its primary unit is out
    excluded = aPart.symbol->ResolveExcludedFromSim( &aPart.sheet, variant );

    aOut->set_exclude_from_sim( excluded );
    aOut->set_dnp( dnp );

    for( const SCH_FIELD& field : aPart.symbol->GetFields() )
    {
        if( field.GetName().StartsWith( wxS( "Sim." ) ) || field.GetName().StartsWith( wxS( "Spice_" ) ) )
        {
            Field* out = aOut->add_fields();
            out->set_name( utf8( field.GetName() ) );
            out->set_value( utf8( field.GetText() ) );
        }
    }

    aOut->set_library( utf8( aPart.symbol->GetField( SIM_LIBRARY_FIELD )
                                     ? aPart.symbol->GetField( SIM_LIBRARY_FIELD )->GetText()
                                     : wxString() ) );

    wxString library = shownField( aPart, SIM_LIBRARY_FIELD, variant );

    if( !library.IsEmpty() )
    {
        NULL_REPORTER devnull;
        wxString      path = aMgr.ResolveLibraryPath( library, devnull );

        if( !path.IsEmpty() && wxFileName::FileExists( path ) )
            aOut->set_library_path( utf8( path ) );
    }

    // The model, as the exporter builds it (its own, when the part is in the netlist)
    const SIM_MODEL*   model = aItem ? aItem->model : nullptr;
    std::string        modelName = aItem ? aItem->modelName : std::string();
    WX_STRING_REPORTER reporter;
    wxString           failure;

    if( !model )
    {
        try
        {
            SIM_LIBRARY::MODEL built = aMgr.CreateModel( &aPart.sheet, *aPart.symbol, true, 0, variant, reporter );
            model = &built.model;
            modelName = built.name;
        }
        catch( const IO_ERROR& e )
        {
            failure = e.What();
        }
        catch( const std::exception& e )
        {
            failure = wxString::FromUTF8( e.what() );
        }
    }

    if( failure.IsEmpty() && reporter.HasMessageOfSeverity( RPT_SEVERITY_UNDEFINED | RPT_SEVERITY_ERROR ) )
        failure = reporter.GetMessages();

    bool noModel = !model || model->GetType() == SIM_MODEL::TYPE::NONE;

    aOut->set_source( modelSource( aPart, model ) );

    std::map<wxString, const SCH_PIN*> pinByNumber;

    for( const SCH_PIN* pin : aPart.symbol->GetAllLibPins() )
        pinByNumber.try_emplace( pin->GetNumber(), pin );

    std::map<std::string, std::string> netByNumber;

    if( aItem )
    {
        for( size_t ii = 0; ii < aItem->pinNumbers.size() && ii < aItem->pinNetNames.size(); ++ii )
            netByNumber[aItem->pinNumbers[ii]] = aItem->pinNetNames[ii];
    }

    std::set<wxString> mapped;

    auto addPin = [&]( const wxString& aNumber, const std::string& aNode )
    {
        PinMap* out = aOut->add_pins();
        out->set_pin_number( utf8( aNumber ) );

        if( auto it = pinByNumber.find( aNumber ); it != pinByNumber.end() )
            out->set_pin_name( utf8( it->second->GetName() ) );

        out->set_model_node( aNode );

        if( auto it = netByNumber.find( utf8( aNumber ) ); it != netByNumber.end() )
            out->set_net( it->second );
    };

    if( model && !noModel )
    {
        SIM_MODEL::DEVICE_INFO deviceInfo = model->GetDeviceInfo();

        aOut->set_device( deviceInfo.fieldValue );
        aOut->set_type( model->GetTypeInfo().fieldValue );
        aOut->set_spice_primitive( model->GetSpiceInfo().itemType );

        if( aItem )
        {
            aOut->set_spice_primitive( aItem->model->SpiceGenerator().ItemName( *aItem ).substr( 0, 1 ) );
            aOut->set_netlist_line( aItem->model->SpiceGenerator().ItemLine( *aItem ) );

            if( !aItem->baseModelName.empty() || aItem->model->GetBaseModel() )
                aOut->set_model_name( aItem->modelName );
        }

        if( aOut->model_name().empty() && aOut->source() == MS_LIBRARY )
            aOut->set_model_name( modelName.empty() ? utf8( shownField( aPart, SIM_NAME_FIELD, variant ) )
                                                    : modelName );

        for( int ii = 0; ii < model->GetPinCount(); ++ii )
        {
            const SIM_MODEL_PIN& pin = model->GetPin( ii );

            if( pin.symbolPinNumber.IsEmpty() )
                continue;

            mapped.insert( pin.symbolPinNumber );
            addPin( pin.symbolPinNumber, pin.modelPinName );

            if( !pinByNumber.count( pin.symbolPinNumber ) && failure.IsEmpty() )
            {
                failure = wxString::Format( wxS( "Sim.Pins connects model node '%s' to pin '%s', which the "
                                                 "symbol does not have" ),
                                            wxString::FromUTF8( pin.modelPinName ), pin.symbolPinNumber );
            }
        }

        if( aIncludeParams )
            packParams( *model, aOut->mutable_params() );
    }

    for( const wxString& number : sortedPinNumbers( *aPart.symbol ) )
    {
        if( !mapped.count( number ) )
        {
            mapped.insert( number );
            addPin( number, std::string() );
        }
    }

    if( excluded )
        return;

    if( noModel && failure.IsEmpty() )
    {
        failure = wxString::Format( wxS( "%s (%s) has no simulation model" ), aPart.ref,
                                    wxString::FromUTF8( aOut->lib_id() ) );
    }

    if( !failure.IsEmpty() )
    {
        aOut->set_error( utf8( oneLine( failure ) ) );
        aOut->set_error_kind( classifyModelError( aPart, aMgr, variant, failure, noModel ) );

        if( aOut->error_kind() == ME_NO_MODEL )
            aOut->set_source( MS_NONE );
    }
}


ProblemKind problemKindOf( ModelError aError )
{
    switch( aError )
    {
    case ME_NO_MODEL:             return PK_MISSING_MODEL;
    case ME_LIBRARY_NOT_FOUND:    return PK_LIBRARY_NOT_FOUND;
    case ME_MODEL_NOT_IN_LIBRARY: return PK_MODEL_NOT_IN_LIBRARY;
    case ME_PIN_MISMATCH:         return PK_PIN_MISMATCH;
    case ME_BAD_PARAMS:           return PK_BAD_PARAMS;
    default:                      return PK_NETLIST;
    }
}


/// The SIM_MODEL::TYPE a device + type pair of field values names; NONE when there is none
SIM_MODEL::TYPE typeFromNames( const std::string& aDevice, const std::string& aType )
{
    for( SIM_MODEL::TYPE type : SIM_MODEL::TYPE_ITERATOR() )
    {
        if( type == SIM_MODEL::TYPE::NONE )
            continue;

        SIM_MODEL::INFO info = SIM_MODEL::TypeInfo( type );

        if( wxString::FromUTF8( SIM_MODEL::DeviceInfo( info.deviceType ).fieldValue ).IsSameAs(
                    wxString::FromUTF8( aDevice ), false )
            && wxString::FromUTF8( info.fieldValue ).IsSameAs( wxString::FromUTF8( aType ), false ) )
        {
            return type;
        }
    }

    return SIM_MODEL::TYPE::NONE;
}


/// The request's pin map onto the model; empty = KiCad's own (by pin order).  False with aError.
bool applyPinMap( SIM_MODEL& aModel, const SCH_SYMBOL& aSymbol,
                  const google::protobuf::RepeatedPtrField<PinMap>& aPins, wxString& aError )
{
    if( aPins.empty() )
        return true;

    std::set<wxString> numbers;

    for( const SCH_PIN* pin : aSymbol.GetAllLibPins() )
        numbers.insert( pin->GetNumber() );

    for( int ii = 0; ii < aModel.GetPinCount(); ++ii )
        aModel.AssignSymbolPinNumberToModelPin( ii, wxEmptyString );

    for( const PinMap& pin : aPins )
    {
        wxString number = wxString::FromUTF8( pin.pin_number() );

        if( !numbers.count( number ) )
        {
            aError = wxString::Format( wxS( "the symbol has no pin '%s'" ), number );
            return false;
        }

        if( pin.model_node().empty() )
            continue;

        int      index = -1;
        wxString node = wxString::FromUTF8( pin.model_node() );

        for( int ii = 0; ii < aModel.GetPinCount(); ++ii )
        {
            if( wxString::FromUTF8( aModel.GetPin( ii ).modelPinName ).IsSameAs( node, false ) )
            {
                index = ii;
                break;
            }
        }

        long position = 0;

        if( index < 0 && node.ToLong( &position ) && position >= 1 && position <= aModel.GetPinCount() )
            index = static_cast<int>( position ) - 1;

        if( index < 0 )
        {
            wxString known;

            for( int ii = 0; ii < aModel.GetPinCount(); ++ii )
                known += ( ii ? wxS( ", " ) : wxS( "" ) ) + wxString::FromUTF8( aModel.GetPin( ii ).modelPinName );

            aError = wxString::Format( wxS( "the model has no node '%s' (its nodes: %s)" ), node, known );
            return false;
        }

        aModel.AssignSymbolPinNumberToModelPin( index, number );
    }

    return true;
}


bool fieldsEqual( const std::vector<SCH_FIELD>& aLhs, const std::vector<SCH_FIELD>& aRhs )
{
    std::map<wxString, wxString> lhs;
    std::map<wxString, wxString> rhs;

    for( const SCH_FIELD& field : aLhs )
        lhs[field.GetName()] = field.GetText();

    for( const SCH_FIELD& field : aRhs )
        rhs[field.GetName()] = field.GetText();

    return lhs == rhs;
}


wxString workbookPath( SCHEMATIC* aSchematic, wxString* aStored = nullptr )
{
    PROJECT& project = aSchematic->Project();
    wxString stored;

    if( project.GetProjectFile().m_SchematicSettings && project.GetProjectFile().m_SchematicSettings->m_NgspiceSettings )
        stored = project.GetProjectFile().m_SchematicSettings->m_NgspiceSettings->GetWorkbookFilename();

    if( aStored )
        *aStored = stored;

    if( stored.IsEmpty() )
    {
        if( project.GetProjectName().IsEmpty() )
            return wxEmptyString;

        stored = project.GetProjectName() + wxS( "." ) + FILEEXT::WorkbookFileExtension;
    }

    return project.AbsolutePath( stored );
}


bool readWorkbook( const wxString& aPath, nlohmann::json& aOut )
{
    if( aPath.IsEmpty() || !wxFileName::FileExists( aPath ) )
        return false;

    try
    {
        std::ifstream stream( aPath.fn_str() );
        aOut = nlohmann::json::parse( stream, nullptr, true, true );
        return aOut.is_object();
    }
    catch( const std::exception& )
    {
        return false;
    }
}


/// A workbook tab's command lines: the analysis, and KiCad's option lines
void tabCommand( const nlohmann::json& aTab, wxString& aCommand, unsigned& aOptions )
{
    aOptions = NETLIST_EXPORTER_SPICE::OPTION_ADJUST_PASSIVE_VALS | NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_EVENTS;
    aCommand.clear();

    if( !aTab.contains( "commands" ) || !aTab["commands"].is_array() )
        return;

    for( const nlohmann::json& cmd : aTab["commands"] )
    {
        if( !cmd.is_string() )
            continue;

        if( cmd == ".kicad adjustpaths" )
            aOptions |= NETLIST_EXPORTER_SPICE::OPTION_ADJUST_INCLUDE_PATHS;
        else if( cmd == ".save all" )
            aOptions |= NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_VOLTAGES;
        else if( cmd == ".probe alli" )
            aOptions |= NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_CURRENTS;
        else if( cmd == ".probe allp" )
            aOptions |= NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_DISSIPATIONS;
        else if( cmd == ".kicad esavenone" )
            aOptions &= ~NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_EVENTS;
        else
            aCommand += wxString::FromUTF8( cmd.get<std::string>() ).Trim();
    }
}


nlohmann::json tabCommands( const wxString& aCommand, unsigned aOptions )
{
    nlohmann::json commands = nlohmann::json::array();

    commands.push_back( utf8( aCommand ) );

    if( aOptions & NETLIST_EXPORTER_SPICE::OPTION_ADJUST_INCLUDE_PATHS )
        commands.push_back( ".kicad adjustpaths" );

    if( aOptions & NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_VOLTAGES )
        commands.push_back( ".save all" );

    if( aOptions & NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_CURRENTS )
        commands.push_back( ".probe alli" );

    if( aOptions & NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_DISSIPATIONS )
        commands.push_back( ".probe allp" );

    if( !( aOptions & NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_EVENTS ) )
        commands.push_back( ".kicad esavenone" );

    return commands;
}


const char* compatibilityName( NGSPICE_COMPATIBILITY_MODE aMode )
{
    switch( aMode )
    {
    case NGSPICE_COMPATIBILITY_MODE::NGSPICE:   return "ngspice";
    case NGSPICE_COMPATIBILITY_MODE::PSPICE:    return "pspice";
    case NGSPICE_COMPATIBILITY_MODE::LTSPICE:   return "ltspice";
    case NGSPICE_COMPATIBILITY_MODE::LT_PSPICE: return "ltpspice";
    case NGSPICE_COMPATIBILITY_MODE::HSPICE:    return "hspice";
    default:                                    return "user";
    }
}


bool compatibilityFromName( const std::string& aName, NGSPICE_COMPATIBILITY_MODE& aMode )
{
    for( NGSPICE_COMPATIBILITY_MODE mode :
         { NGSPICE_COMPATIBILITY_MODE::USER_CONFIG, NGSPICE_COMPATIBILITY_MODE::NGSPICE,
           NGSPICE_COMPATIBILITY_MODE::PSPICE, NGSPICE_COMPATIBILITY_MODE::LTSPICE,
           NGSPICE_COMPATIBILITY_MODE::LT_PSPICE, NGSPICE_COMPATIBILITY_MODE::HSPICE } )
    {
        if( aName == compatibilityName( mode ) )
        {
            aMode = mode;
            return true;
        }
    }

    return false;
}


/// The trace type KiCad stores with a signal of a tab: the Y kind | the tab's X axis
int traceTypeOf( const wxString& aSignal, const wxString& aCommand )
{
    int      type = SPT_VOLTAGE;
    wxString upper = aSignal.Upper();

    if( upper.StartsWith( wxS( "I" ) ) && upper.Contains( wxS( "(" ) ) )
        type = SPT_CURRENT;
    else if( upper.StartsWith( wxS( "P(" ) ) )
        type = SPT_POWER;

    switch( SPICE_CIRCUIT_MODEL::CommandToSimType( aCommand ) )
    {
    case ST_TRAN:  type |= SPT_TIME;          break;
    case ST_DC:    type |= SPT_SWEEP;         break;
    case ST_AC:    type = ( type & ~SPT_Y_AXIS_MASK ) | SPT_AC_GAIN | SPT_LIN_FREQUENCY; break;
    case ST_NOISE:
    case ST_SP:
    case ST_FFT:   type |= SPT_LIN_FREQUENCY; break;
    default:                                  break;
    }

    return type;
}


void packSetup( SCHEMATIC* aSchematic, KIWAY* aKiway, Setup* aOut, const wxString& aLiveCopy = wxEmptyString )
{
    wxString stored;
    wxString path = workbookPath( aSchematic, &stored );

    aOut->set_workbook( utf8( stored ) );
    aOut->set_workbook_path( utf8( path ) );

    nlohmann::json js;

    if( readWorkbook( aLiveCopy.IsEmpty() ? path : aLiveCopy, js ) )
    {
        if( js.contains( "tabs" ) && js["tabs"].is_array() )
        {
            for( const nlohmann::json& tab : js["tabs"] )
            {
                wxString command;
                unsigned options = 0;

                tabCommand( tab, command, options );

                Analysis* out = aOut->add_tabs();
                out->set_command( utf8( command ) );
                out->set_analysis( SIM_API::AnalysisName( command ) );
                SIM_API::OptionsToProto( options, out->mutable_options() );

                if( tab.contains( "traces" ) && tab["traces"].is_array() )
                {
                    for( const nlohmann::json& trace : tab["traces"] )
                    {
                        if( trace.contains( "signal" ) && trace["signal"].is_string() )
                            out->add_signals( trace["signal"].get<std::string>() );
                    }
                }

                if( tab.contains( "measurements" ) && tab["measurements"].is_array() )
                {
                    for( const nlohmann::json& measurement : tab["measurements"] )
                    {
                        Measurement* m = out->add_measurements();

                        if( measurement.contains( "expr" ) && measurement["expr"].is_string() )
                            m->set_expr( measurement["expr"].get<std::string>() );

                        if( measurement.contains( "format" ) && measurement["format"].is_string() )
                            m->set_format( measurement["format"].get<std::string>() );
                    }
                }
            }
        }

        if( js.contains( "user_defined_signals" ) && js["user_defined_signals"].is_array() )
        {
            for( const nlohmann::json& signal : js["user_defined_signals"] )
            {
                if( signal.is_string() )
                    aOut->add_user_defined_signals( signal.get<std::string>() );
            }
        }
    }

    SIM_API::CIRCUIT circuit( aSchematic, aKiway );

    aOut->set_schematic_command( utf8( circuit.SheetCommand() ) );

    for( const wxString& directive : circuit.GetDirectives() )
    {
        if( !SPICE_CIRCUIT_MODEL::IsSimCommand( directive ) )
            aOut->add_schematic_directives( utf8( directive ) );
    }

    PROJECT_FILE& projectFile = aSchematic->Project().GetProjectFile();

    if( projectFile.m_SchematicSettings && projectFile.m_SchematicSettings->m_NgspiceSettings )
    {
        aOut->set_compatibility(
                compatibilityName( projectFile.m_SchematicSettings->m_NgspiceSettings->GetCompatibilityMode() ) );
    }
}

} // namespace


// ---------------------------------------------------------------------------------------------
// Shared with the run
// ---------------------------------------------------------------------------------------------

unsigned SIM_API::OptionsFromProto( const Options& aOptions )
{
    unsigned options = NETLIST_EXPORTER_SPICE::OPTION_ADJUST_INCLUDE_PATHS
                       | NETLIST_EXPORTER_SPICE::OPTION_ADJUST_PASSIVE_VALS
                       | NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_VOLTAGES
                       | NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_CURRENTS
                       | NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_EVENTS;

    auto set = [&]( bool aHas, bool aValue, unsigned aFlag )
    {
        if( !aHas )
            return;

        if( aValue )
            options |= aFlag;
        else
            options &= ~aFlag;
    };

    set( aOptions.has_adjust_include_paths(), aOptions.adjust_include_paths(),
         NETLIST_EXPORTER_SPICE::OPTION_ADJUST_INCLUDE_PATHS );
    set( aOptions.has_adjust_passive_vals(), aOptions.adjust_passive_vals(),
         NETLIST_EXPORTER_SPICE::OPTION_ADJUST_PASSIVE_VALS );
    set( aOptions.has_save_all_voltages(), aOptions.save_all_voltages(),
         NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_VOLTAGES );
    set( aOptions.has_save_all_currents(), aOptions.save_all_currents(),
         NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_CURRENTS );
    set( aOptions.has_save_all_dissipations(), aOptions.save_all_dissipations(),
         NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_DISSIPATIONS );
    set( aOptions.has_save_all_events(), aOptions.save_all_events(),
         NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_EVENTS );
    set( aOptions.has_current_sheet_as_root(), aOptions.current_sheet_as_root(),
         NETLIST_EXPORTER_SPICE::OPTION_CUR_SHEET_AS_ROOT );

    return options;
}


void SIM_API::OptionsToProto( unsigned aOptions, Options* aOut )
{
    aOut->set_adjust_include_paths( aOptions & NETLIST_EXPORTER_SPICE::OPTION_ADJUST_INCLUDE_PATHS );
    aOut->set_adjust_passive_vals( aOptions & NETLIST_EXPORTER_SPICE::OPTION_ADJUST_PASSIVE_VALS );
    aOut->set_save_all_voltages( aOptions & NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_VOLTAGES );
    aOut->set_save_all_currents( aOptions & NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_CURRENTS );
    aOut->set_save_all_dissipations( aOptions & NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_DISSIPATIONS );
    aOut->set_save_all_events( aOptions & NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_EVENTS );
    aOut->set_current_sheet_as_root( aOptions & NETLIST_EXPORTER_SPICE::OPTION_CUR_SHEET_AS_ROOT );
}


std::string SIM_API::AnalysisName( const wxString& aCommand )
{
    return utf8( SPICE_SIMULATOR::TypeToName( SPICE_CIRCUIT_MODEL::CommandToSimType( aCommand ), true ) );
}


void SIM_API::ReporterToProblems( const LINE_REPORTER& aReporter,
                                  google::protobuf::RepeatedPtrField<Problem>* aOut )
{
    for( const LINE_REPORTER::LINE& line : aReporter.Lines() )
    {
        wxString text = oneLine( line.text );

        if( text.IsEmpty() )
            continue;

        Problem* problem = aOut->Add();
        problem->set_kind( PK_NETLIST );
        problem->set_severity( ( line.severity == RPT_SEVERITY_WARNING || line.severity == RPT_SEVERITY_INFO
                                 || line.severity == RPT_SEVERITY_ACTION )
                                       ? PS_WARNING
                                       : PS_ERROR );
        problem->set_message( utf8( text ) );
    }
}


void SIM_API::ModelProblems( SCHEMATIC* aSchematic, google::protobuf::RepeatedPtrField<Problem>* aOut )
{
    SIM_LIB_MGR mgr( &aSchematic->Project() );

    for( const PART& part : collectParts( aSchematic ) )
    {
        SymbolModel model;
        describePart( aSchematic, part, mgr, nullptr, false, &model );

        if( model.error_kind() == ME_NONE )
            continue;

        Problem* problem = aOut->Add();
        problem->set_kind( problemKindOf( model.error_kind() ) );
        problem->set_severity( PS_ERROR );
        problem->set_reference( model.reference() );
        problem->set_message( model.error() );
    }
}


bool SIM_API::SetOverrides( CIRCUIT& aCircuit, const google::protobuf::RepeatedPtrField<Override>& aOverrides,
                            std::string& aError )
{
    std::map<wxString, std::vector<NETLIST_EXPORTER_SPICE::FIELD_OVERRIDE>> overrides;

    for( const Override& override : aOverrides )
    {
        if( override.reference().empty() )
        {
            aError = "an override needs a reference";
            return false;
        }

        overrides[wxString::FromUTF8( override.reference() )].push_back(
                { wxString::FromUTF8( override.field() ), wxString::FromUTF8( override.value() ) } );
    }

    aCircuit.SetFieldOverrides( std::move( overrides ) );
    return true;
}


wxString SIM_API::SavedCommand( SCHEMATIC* aSchematic, KIWAY* aKiway, unsigned* aOptions )
{
    nlohmann::json js;

    if( readWorkbook( workbookPath( aSchematic ), js ) && js.contains( "tabs" ) && js["tabs"].is_array() )
    {
        for( const nlohmann::json& tab : js["tabs"] )
        {
            wxString command;
            unsigned options = 0;

            tabCommand( tab, command, options );

            if( !command.IsEmpty() )
            {
                if( aOptions )
                    *aOptions = options;

                return command;
            }
        }
    }

    CIRCUIT circuit( aSchematic, aKiway );
    return circuit.SheetCommand();
}


// ---------------------------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------------------------

void API_HANDLER_SCH::registerSimHandlers()
{
    registerHandler<GetSimulationModels, GetSimulationModelsResponse>( &API_HANDLER_SCH::handleGetSimulationModels );
    registerHandler<AssignSimulationModel, AssignSimulationModelResponse>(
            &API_HANDLER_SCH::handleAssignSimulationModel );
    registerHandler<GetSimulationNetlist, GetSimulationNetlistResponse>(
            &API_HANDLER_SCH::handleGetSimulationNetlist );
    registerHandler<GetSimulationSetup, SetupResponse>( &API_HANDLER_SCH::handleGetSimulationSetup );
    registerHandler<SetSimulationSetup, SetupResponse>( &API_HANDLER_SCH::handleSetSimulationSetup );
    registerHandler<RunSimulation, RunSimulationResponse>( &API_HANDLER_SCH::handleRunSimulation );
    registerHandler<GetSimulationResult, RunSimulationResponse>( &API_HANDLER_SCH::handleGetSimulationResult );
    registerHandler<StopSimulation, StopSimulationResponse>( &API_HANDLER_SCH::handleStopSimulation );
}


void API_HANDLER_SCH::detachSimulation()
{
    if( m_context && m_context->GetSchematic() )
        SIM_API::RUNNER::Get().Detach( m_context->GetSchematic() );
}


std::optional<ApiResponseStatus> API_HANDLER_SCH::simNetlistReady()
{
    // The exporter refreshes connectivity, which an interactive tool must not observe
    if( m_frame && !m_frame->ToolStackIsEmpty() )
        return statusError( ApiStatusCode::AS_BUSY, "Cannot write the simulation netlist during an interactive "
                                                    "operation in the schematic editor" );

    if( !schematic() || !schematic()->IsValid() )
        return statusError( ApiStatusCode::AS_BAD_REQUEST, "no schematic is open" );

    return std::nullopt;
}


HANDLER_RESULT<GetSimulationModelsResponse>
API_HANDLER_SCH::handleGetSimulationModels( const HANDLER_CONTEXT<GetSimulationModels>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = simNetlistReady() )
        return tl::unexpected( *busy );

    wxLogNull                   quiet;
    GetSimulationModelsResponse response;
    SCHEMATIC*                  sch = schematic();
    std::vector<PART>           parts = collectParts( sch );
    std::set<wxString>          wanted;

    for( const std::string& reference : aCtx.Request.references() )
        wanted.insert( wxString::FromUTF8( reference ) );

    // The netlist's own items: the nets of each pin and the line each part gets
    SIM_API::CIRCUIT       circuit( sch, context()->GetKiway() );
    SIM_API::LINE_REPORTER reporter;

    try
    {
        circuit.ReadSchematicAndLibraries( SIM_API::OptionsFromProto( Options() ), reporter );
    }
    catch( const std::exception& )
    {
    }

    SIM_LIB_MGR mgr( &sch->Project() );
    int         excluded = 0;
    int         problems = 0;

    for( const PART& part : parts )
    {
        SymbolModel model;
        describePart( sch, part, mgr, circuit.FindItem( part.ref ), aCtx.Request.include_params(), &model );

        if( model.exclude_from_sim() || model.dnp() )
            excluded++;

        if( model.error_kind() != ME_NONE )
            problems++;

        if( !wanted.empty() && !wanted.count( part.ref ) )
            continue;

        wanted.erase( part.ref );

        if( aCtx.Request.only_problems() && model.error_kind() == ME_NONE )
            continue;

        response.add_models()->Swap( &model );
    }

    for( const wxString& missing : wanted )
        response.add_not_found( utf8( missing ) );

    response.set_symbols( static_cast<int>( parts.size() ) );
    response.set_excluded( excluded );
    response.set_problems( problems );
    return response;
}


HANDLER_RESULT<AssignSimulationModelResponse>
API_HANDLER_SCH::handleAssignSimulationModel( const HANDLER_CONTEXT<AssignSimulationModel>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( std::optional<ApiResponseStatus> busy = simNetlistReady() )
        return tl::unexpected( *busy );

    const AssignSimulationModel& request = aCtx.Request;

    if( request.references().empty() )
        return tl::unexpected( statusError( ApiStatusCode::AS_BAD_REQUEST, "no references given" ) );

    bool hasModel = request.model_case() != AssignSimulationModel::MODEL_NOT_SET
                    && !( request.model_case() == AssignSimulationModel::kClear && !request.clear() );

    if( !hasModel && !request.has_exclude_from_sim() )
    {
        return tl::unexpected( statusError( ApiStatusCode::AS_BAD_REQUEST,
                                            "nothing to do: give a model (builtin, library, raw or clear) "
                                            "or exclude_from_sim" ) );
    }

    wxLogNull                     quiet;
    SCHEMATIC*                    sch = schematic();
    wxString                      variant = sch->GetCurrentVariant();
    std::vector<PART>             parts = collectParts( sch );
    AssignSimulationModelResponse response;

    struct PLAN
    {
        const PART*            part = nullptr;
        std::vector<SCH_FIELD> fields;
        bool                   fieldsChanged = false;
        bool                   excludeChanged = false;
    };

    std::vector<PLAN> plans;
    int               failed = 0;

    for( const std::string& reference : request.references() )
    {
        AssignResult* result = response.add_results();
        result->set_reference( reference );

        wxString ref = wxString::FromUTF8( reference );
        auto     it = std::find_if( parts.begin(), parts.end(),
                                    [&]( const PART& aPart )
                                    {
                                        return aPart.ref == ref;
                                    } );

        auto fail = [&]( ModelError aKind, const wxString& aWhy )
        {
            result->set_error_kind( aKind );
            result->set_error( utf8( oneLine( aWhy ) ) );
            failed++;
        };

        if( it == parts.end() )
        {
            fail( ME_OTHER, wxString::Format( wxS( "the schematic has no symbol %s" ), ref ) );
            continue;
        }

        PLAN plan;
        plan.part = &*it;
        plan.fields = it->symbol->GetFields();

        if( request.has_exclude_from_sim() )
        {
            for( const auto& [unit, sheet] : it->units )
                plan.excludeChanged |= unit->GetExcludedFromSim() != request.exclude_from_sim();
        }

        if( hasModel )
        {
            std::vector<wxString> pins = sortedPinNumbers( *it->symbol );
            WX_STRING_REPORTER    reporter;
            SIM_LIB_MGR           mgr( &sch->Project() );
            wxString              error;
            ModelError            errorKind = ME_OTHER;
            std::vector<SCH_FIELD> fields = plan.fields;

            auto dropSimFields = [&]()
            {
                std::erase_if( fields,
                               []( const SCH_FIELD& aField )
                               {
                                   return aField.GetName().StartsWith( wxS( "Sim." ) )
                                          || aField.GetName().StartsWith( wxS( "Spice_" ) );
                               } );
            };

            auto setParams = [&]( SIM_MODEL& aModel, const google::protobuf::RepeatedPtrField<Param>& aParams )
            {
                for( const Param& param : aParams )
                {
                    if( !aModel.FindParam( param.name() ) )
                    {
                        errorKind = ME_BAD_PARAMS;
                        error = wxString::Format( wxS( "the model has no parameter '%s'" ),
                                                  wxString::FromUTF8( param.name() ) );
                        return false;
                    }

                    aModel.SetParamValue( param.name(), param.value() );
                }

                return true;
            };

            try
            {
                switch( request.model_case() )
                {
                case AssignSimulationModel::kClear:
                    dropSimFields();
                    break;

                case AssignSimulationModel::kBuiltin:
                {
                    SIM_MODEL::TYPE type = typeFromNames( request.builtin().device(), request.builtin().type() );

                    if( type == SIM_MODEL::TYPE::NONE || type == SIM_MODEL::TYPE::RAWSPICE )
                    {
                        errorKind = ME_BAD_PARAMS;
                        error = wxString::Format( wxS( "no built-in device '%s' of type '%s'" ),
                                                  wxString::FromUTF8( request.builtin().device() ),
                                                  wxString::FromUTF8( request.builtin().type() ) );
                        break;
                    }

                    SIM_MODEL& model = mgr.CreateModel( type, pins, reporter );

                    if( model.GetPinCount() > static_cast<int>( pins.size() ) )
                    {
                        errorKind = ME_PIN_MISMATCH;
                        error = wxString::Format( wxS( "the device has %d nodes, the symbol %d pins" ),
                                                  model.GetPinCount(), static_cast<int>( pins.size() ) );
                        break;
                    }

                    if( !setParams( model, request.builtin().params() ) )
                        break;

                    if( !applyPinMap( model, *it->symbol, request.pins(), error ) )
                    {
                        errorKind = ME_PIN_MISMATCH;
                        break;
                    }

                    dropSimFields();
                    model.SetIsStoredInValue( model.HasPrimaryValue() );
                    model.WriteFields( fields );
                    break;
                }

                case AssignSimulationModel::kLibrary:
                {
                    wxString library = wxString::FromUTF8( request.library().library() );
                    wxString path = mgr.ResolveLibraryPath( library, reporter );

                    if( library.IsEmpty() || path.IsEmpty() || !wxFileName::FileExists( path ) )
                    {
                        errorKind = ME_LIBRARY_NOT_FOUND;
                        error = wxString::Format( wxS( "library file '%s' not found" ), library );
                        break;
                    }

                    mgr.SetForceFullParse();
                    mgr.SetLibrary( library, reporter );

                    const SIM_MODEL* base = nullptr;

                    for( const auto& [libPath, lib] : mgr.GetLibraries() )
                        base = lib.get().FindModel( request.library().name() );

                    if( !base )
                    {
                        wxString known;
                        int      count = 0;

                        for( const auto& [libPath, lib] : mgr.GetLibraries() )
                        {
                            for( const SIM_LIBRARY::MODEL& libModel : lib.get().GetModels() )
                            {
                                if( count++ < 20 )
                                    known += ( known.IsEmpty() ? wxS( "" ) : wxS( ", " ) )
                                             + wxString::FromUTF8( libModel.name );
                            }
                        }

                        errorKind = ME_MODEL_NOT_IN_LIBRARY;
                        error = wxString::Format( wxS( "no model '%s' in '%s' (it has: %s%s)" ),
                                                  wxString::FromUTF8( request.library().name() ), library, known,
                                                  count > 20 ? wxS( ", ..." ) : wxS( "" ) );

                        if( reporter.HasMessage() )
                            error += wxS( "; " ) + reporter.GetMessages();

                        break;
                    }

                    SIM_MODEL& model = mgr.CreateModel( base, pins, reporter );

                    if( model.GetPinCount() > static_cast<int>( pins.size() ) )
                    {
                        errorKind = ME_PIN_MISMATCH;
                        error = wxString::Format( wxS( "model '%s' has %d nodes, the symbol %d pins" ),
                                                  wxString::FromUTF8( request.library().name() ),
                                                  model.GetPinCount(), static_cast<int>( pins.size() ) );
                        break;
                    }

                    if( !setParams( model, request.library().params() ) )
                        break;

                    if( !applyPinMap( model, *it->symbol, request.pins(), error ) )
                    {
                        errorKind = ME_PIN_MISMATCH;
                        break;
                    }

                    // As the dialog stores it: relative to the project when it is inside it
                    wxFileName fn( library );

                    if( fn.IsAbsolute() && fn.MakeRelativeTo( sch->Project().GetProjectPath() )
                        && !fn.GetFullPath().StartsWith( wxS( ".." ) ) )
                    {
                        library = fn.GetFullPath();
                    }

                    dropSimFields();
                    SetFieldValue( fields, SIM_LIBRARY_FIELD, utf8( library ), false );
                    SetFieldValue( fields, SIM_NAME_FIELD, request.library().name(), false );
                    model.WriteFields( fields );
                    break;
                }

                case AssignSimulationModel::kRaw:
                {
                    if( request.raw().primitive().empty() )
                    {
                        errorKind = ME_BAD_PARAMS;
                        error = wxS( "a raw SPICE model needs its primitive (the item letter)" );
                        break;
                    }

                    SIM_MODEL& model = mgr.CreateModel( SIM_MODEL::TYPE::RAWSPICE, pins, reporter );

                    model.SetParamValue( "type", request.raw().primitive() );
                    model.SetParamValue( "model", request.raw().model() );

                    if( !request.raw().library().empty() )
                        model.SetParamValue( "lib", request.raw().library() );

                    if( !applyPinMap( model, *it->symbol, request.pins(), error ) )
                    {
                        errorKind = ME_PIN_MISMATCH;
                        break;
                    }

                    dropSimFields();
                    model.WriteFields( fields );
                    break;
                }

                default:
                    break;
                }
            }
            catch( const IO_ERROR& e )
            {
                error = e.What();
            }
            catch( const std::exception& e )
            {
                error = wxString::FromUTF8( e.what() );
            }

            if( !error.IsEmpty() )
            {
                fail( errorKind, error );
                continue;
            }

            plan.fieldsChanged = !fieldsEqual( plan.fields, fields );
            plan.fields = std::move( fields );
        }

        plans.push_back( std::move( plan ) );
    }

    response.set_failed( failed );

    bool write = !plans.empty() && ( failed == 0 || request.partial() );
    int  changed = 0;

    if( write )
    {
        std::unique_ptr<COMMIT> commit = createCommit();

        for( const PLAN& plan : plans )
        {
            if( !plan.fieldsChanged && !plan.excludeChanged )
                continue;

            changed++;

            for( const auto& [unit, sheet] : plan.part->units )
            {
                commit->Modify( unit, sheet.LastScreen() );

                if( plan.fieldsChanged )
                {
                    // The model's fields are the part's: every unit carries them.  Each unit keeps
                    // its own Reference text and the fields' positions it already had.
                    std::vector<SCH_FIELD>& unitFields = unit->GetFields();

                    std::erase_if( unitFields,
                                   [&]( const SCH_FIELD& aField )
                                   {
                                       return ( aField.GetName().StartsWith( wxS( "Sim." ) )
                                                || aField.GetName().StartsWith( wxS( "Spice_" ) ) )
                                              && !FindField( plan.fields, aField.GetName() );
                                   } );

                    for( const SCH_FIELD& field : plan.fields )
                    {
                        bool simField = field.GetName().StartsWith( wxS( "Sim." ) );
                        bool value = field.GetId() == FIELD_T::VALUE;

                        if( !simField && !value )
                            continue;

                        if( value )
                        {
                            if( unit->GetField( FIELD_T::VALUE )->GetText() != field.GetText() )
                                unit->GetField( FIELD_T::VALUE )->SetText( field.GetText() );

                            continue;
                        }

                        if( SCH_FIELD* existing = unit->GetField( field.GetName() ) )
                        {
                            existing->SetText( field.GetText() );
                        }
                        else
                        {
                            SCH_FIELD added( unit, FIELD_T::USER, field.GetName() );
                            added.SetText( field.GetText() );
                            added.SetVisible( false );
                            added.SetPosition( unit->GetPosition() );
                            unit->AddField( added );
                        }
                    }
                }

                if( request.has_exclude_from_sim() )
                    unit->SetExcludedFromSim( request.exclude_from_sim() );
            }
        }

        if( changed > 0 )
        {
            commit->Push( _( "Assign Simulation Model" ) );
            onModified();
        }
    }

    response.set_changed( changed );

    // Each part as it now simulates
    SIM_LIB_MGR mgr( &sch->Project() );

    for( const PLAN& plan : plans )
    {
        for( AssignResult& result : *response.mutable_results() )
        {
            if( wxString::FromUTF8( result.reference() ) != plan.part->ref || !result.error().empty() )
                continue;

            result.set_changed( write && ( plan.fieldsChanged || plan.excludeChanged ) );
            describePart( sch, *plan.part, mgr, nullptr, false, result.mutable_model() );
        }
    }

    return response;
}


HANDLER_RESULT<GetSimulationNetlistResponse>
API_HANDLER_SCH::handleGetSimulationNetlist( const HANDLER_CONTEXT<GetSimulationNetlist>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = simNetlistReady() )
        return tl::unexpected( *busy );

    wxLogNull                    quiet;
    GetSimulationNetlistResponse response;
    SCHEMATIC*                   sch = schematic();
    unsigned                     options = SIM_API::OptionsFromProto( aCtx.Request.options() );
    wxString                     command = wxString::FromUTF8( aCtx.Request.command() ).Trim().Trim( false );

    if( command.IsEmpty() )
    {
        unsigned saved = options;
        command = SIM_API::SavedCommand( sch, context()->GetKiway(), &saved );

        if( !aCtx.Request.has_options() )
            options = saved | NETLIST_EXPORTER_SPICE::OPTION_ADJUST_INCLUDE_PATHS;
    }

    SIM_API::CIRCUIT       circuit( sch, context()->GetKiway() );
    SIM_API::LINE_REPORTER reporter;
    std::string            overrideError;

    if( !SIM_API::SetOverrides( circuit, aCtx.Request.overrides(), overrideError ) )
        return tl::unexpected( statusError( ApiStatusCode::AS_BAD_REQUEST, overrideError ) );

    SIM_API::SetRun( circuit, aCtx.Request.directives(), aCtx.Request.breaks() );

    STRING_FORMATTER formatter;

    try
    {
        circuit.GetNetlist( command, options, &formatter, reporter );
    }
    catch( const IO_ERROR& e )
    {
        reporter.Report( e.What(), RPT_SEVERITY_ERROR );
    }

    response.set_netlist( formatter.GetString() );
    response.set_command( utf8( command ) );

    SIM_API::ModelProblems( sch, response.mutable_problems() );

    // The exporter's own lines, less those the model problems already tell with their symbol
    if( response.problems().empty() )
        SIM_API::ReporterToProblems( reporter, response.mutable_problems() );

    for( const BrokenPin& pin : circuit.Broken() )
        *response.add_broken() = pin;

    SIM_API::TopologyProblems( circuit, response.mutable_problems() );

    for( const auto& [net, node] : circuit.NetNodes() )
    {
        NetNode* out = response.add_nodes();
        out->set_net( utf8( net ) );
        out->set_node( utf8( node.Lower() ) );
    }

    int items = 0;

    for( const SPICE_ITEM& item : circuit.GetItems() )
    {
        if( !item.model || !item.model->IsEnabled() )
            continue;

        items++;

        std::vector<std::string> currents = item.model->SpiceGenerator().CurrentNames( item );
        auto                     pins = item.model->GetPins();

        for( size_t ii = 0; ii < currents.size(); ++ii )
        {
            PinCurrent* out = response.add_pin_currents();
            out->set_reference( item.refName );
            out->set_signal( currents[ii] );
            out->set_vector( utf8( wxString::FromUTF8( currents[ii] ).Lower() ) );

            if( ii < pins.size() )
            {
                wxString number = pins[ii].get().symbolPinNumber;
                out->set_pin_number( utf8( number ) );
                out->set_pin_name( pins[ii].get().modelPinName );
            }
        }
    }

    response.set_items( items );

    // Each element's line and where each pin's node stands in it
    {
        std::vector<std::string> netlistLines;
        std::string              netlistText = response.netlist();

        for( size_t at = 0; at <= netlistText.size(); )
        {
            size_t next = netlistText.find( '\n', at );
            netlistLines.push_back( netlistText.substr( at, next == std::string::npos ? std::string::npos
                                                                                     : next - at ) );

            if( next == std::string::npos )
                break;

            at = next + 1;
        }

        auto tokensOf = []( const std::string& aLine )
        {
            std::vector<std::string> tokens;
            wxStringTokenizer        tokenizer( wxString::FromUTF8( aLine ), wxS( " \t\r\n" ), wxTOKEN_STRTOK );

            while( tokenizer.HasMoreTokens() )
                tokens.push_back( utf8( tokenizer.GetNextToken() ) );

            return tokens;
        };

        for( const SPICE_ITEM& item : circuit.GetItems() )
        {
            if( !item.model || !item.model->IsEnabled() )
                continue;

            std::string name = item.model->SpiceGenerator().ItemName( item );
            std::string text = item.model->SpiceGenerator().ItemLine( item );
            ItemLine*   out = response.add_lines();

            out->set_reference( item.refName );
            out->set_item( name );
            out->set_line( text );

            for( size_t ii = 0; ii < netlistLines.size(); ++ii )
            {
                std::vector<std::string> tokens = tokensOf( netlistLines[ii] );

                if( !tokens.empty() && tokens[0] == name )
                {
                    out->set_line_number( static_cast<int>( ii ) + 1 );
                    break;
                }
            }

            std::vector<std::string> tokens = tokensOf( text.substr( 0, text.find( '\n' ) ) );
            size_t                   from = 1;

            // A raw SPICE element without a pin map takes the symbol's pins in their order
            if( item.model->GetPinCount() == 0 )
            {
                for( size_t ii = 0; ii < item.pinNumbers.size() && ii < item.pinNetNames.size(); ++ii )
                {
                    ItemNode* node = out->add_nodes();
                    node->set_pin_number( item.pinNumbers[ii] );
                    node->set_model_node( std::to_string( ii + 1 ) );
                    node->set_node( item.pinNetNames[ii] );
                    node->set_token( -1 );

                    for( size_t jj = from; jj < tokens.size(); ++jj )
                    {
                        if( tokens[jj] == item.pinNetNames[ii] )
                        {
                            node->set_token( static_cast<int>( jj ) );
                            from = jj + 1;
                            break;
                        }
                    }
                }
            }

            for( const SIM_MODEL_PIN& pin : item.model->GetPins() )
            {
                if( pin.symbolPinNumber.IsEmpty() )
                    continue;

                auto it = std::find( item.pinNumbers.begin(), item.pinNumbers.end(), utf8( pin.symbolPinNumber ) );

                if( it == item.pinNumbers.end() )
                    continue;

                size_t index = it - item.pinNumbers.begin();

                if( index >= item.pinNetNames.size() )
                    continue;

                ItemNode* node = out->add_nodes();
                node->set_pin_number( utf8( pin.symbolPinNumber ) );
                node->set_model_node( pin.modelPinName );
                node->set_node( item.pinNetNames[index] );
                node->set_token( -1 );

                for( size_t ii = from; ii < tokens.size(); ++ii )
                {
                    if( tokens[ii] == item.pinNetNames[index] )
                    {
                        node->set_token( static_cast<int>( ii ) );
                        from = ii + 1;
                        break;
                    }
                }
            }
        }
    }

    for( const wxString& directive : circuit.GetDirectives() )
        response.add_directives( utf8( directive ) );

    return response;
}


HANDLER_RESULT<SetupResponse>
API_HANDLER_SCH::handleGetSimulationSetup( const HANDLER_CONTEXT<GetSimulationSetup>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( !schematic() || !schematic()->IsValid() )
        return tl::unexpected( statusError( ApiStatusCode::AS_BAD_REQUEST, "no schematic is open" ) );

    wxLogNull     quiet;
    SetupResponse response;

    // With a Simulator window open, its tabs as they are now, saved or not
    wxString live = simSyncWorkbook( false );

    packSetup( schematic(), context()->GetKiway(), response.mutable_setup(), live );

    if( !live.IsEmpty() )
        wxRemoveFile( live );

    if( KIWAY* kiway = m_frame ? context()->GetKiway() : nullptr )
    {
        if( KIWAY_PLAYER* window = kiway->Player( FRAME_SIMULATOR, false ) )
        {
            for( const wxString& ref : static_cast<SIMULATOR_FRAME*>( window )->TunerRefs() )
                response.mutable_setup()->add_tuners( utf8( ref ) );
        }
    }

    return response;
}


HANDLER_RESULT<SetupResponse>
API_HANDLER_SCH::handleSetSimulationSetup( const HANDLER_CONTEXT<SetSimulationSetup>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( !schematic() || !schematic()->IsValid() )
        return tl::unexpected( statusError( ApiStatusCode::AS_BAD_REQUEST, "no schematic is open" ) );

    wxLogNull                 quiet;
    const SetSimulationSetup& request = aCtx.Request;
    SCHEMATIC*                sch = schematic();
    wxString                  command = wxString::FromUTF8( request.command() ).Trim().Trim( false );

    if( !request.remove() && command.IsEmpty() && !request.has_compatibility() )
        return tl::unexpected( statusError( ApiStatusCode::AS_BAD_REQUEST, "no command given" ) );

    if( !command.IsEmpty() && !SPICE_CIRCUIT_MODEL::IsSimCommand( command ) )
    {
        return tl::unexpected( statusError( ApiStatusCode::AS_BAD_REQUEST,
                                            fmt::format( "'{}' is not an analysis command (.op, .tran, .ac, .dc, "
                                                         ".noise, .pz, .tf, .sp, fft)", request.command() ) ) );
    }

    wxString stored;
    wxString path = workbookPath( sch, &stored );

    if( path.IsEmpty() )
    {
        return tl::unexpected( statusError( ApiStatusCode::AS_BAD_REQUEST,
                                            "the schematic has no project to keep a workbook with" ) );
    }

    std::shared_ptr<NGSPICE_SETTINGS> settings = sch->Project().GetProjectFile().m_SchematicSettings->m_NgspiceSettings;
    NGSPICE_COMPATIBILITY_MODE        mode = settings->GetCompatibilityMode();

    if( request.has_compatibility() && !compatibilityFromName( request.compatibility(), mode ) )
    {
        return tl::unexpected( statusError( ApiStatusCode::AS_BAD_REQUEST,
                                            fmt::format( "unknown compatibility '{}' (user, ngspice, pspice, "
                                                         "ltspice, ltpspice, hspice)", request.compatibility() ) ) );
    }

    // The Simulator window's live tabs first, so what the person has open is what is edited
    wxString live = simSyncWorkbook( false );

    nlohmann::json js;
    bool           read = readWorkbook( live.IsEmpty() ? path : live, js );

    if( !live.IsEmpty() )
        wxRemoveFile( live );

    if( !read )
        js = nlohmann::json( { { "version", 8 }, { "tabs", nlohmann::json::array() },
                               { "user_defined_signals", nlohmann::json::array() }, { "custom_cursors", 2 } } );

    if( !js.contains( "tabs" ) || !js["tabs"].is_array() )
        js["tabs"] = nlohmann::json::array();

    nlohmann::json before = js;
    nlohmann::json& tabs = js["tabs"];
    int             tabCount = static_cast<int>( tabs.size() );
    int             index = -1;
    SetupResponse   response;

    if( request.remove() )
    {
        if( !request.has_tab() || request.tab() < 0 || request.tab() >= tabCount )
            return tl::unexpected( statusError( ApiStatusCode::AS_BAD_REQUEST, "remove needs an existing tab" ) );

        tabs.erase( tabs.begin() + request.tab() );
        index = request.tab();
    }
    else if( !command.IsEmpty() )
    {
        if( request.has_tab() )
        {
            if( request.tab() < 0 || request.tab() > tabCount )
            {
                return tl::unexpected( statusError( ApiStatusCode::AS_BAD_REQUEST,
                                                    fmt::format( "tab {} is out of range (0..{})", request.tab(),
                                                                 tabCount ) ) );
            }

            index = request.tab();
        }
        else
        {
            SIM_TYPE type = SPICE_CIRCUIT_MODEL::CommandToSimType( command );

            for( int ii = 0; ii < tabCount && index < 0; ++ii )
            {
                wxString existing;
                unsigned existingOptions = 0;

                tabCommand( tabs[ii], existing, existingOptions );

                if( SPICE_CIRCUIT_MODEL::CommandToSimType( existing ) == type )
                    index = ii;
            }

            if( index < 0 )
                index = tabCount;
        }

        if( index == tabCount )
            tabs.push_back( nlohmann::json::object() );

        nlohmann::json& tab = tabs[index];
        unsigned        options = SIM_API::OptionsFromProto( request.options() );

        tab["analysis"] = SIM_API::AnalysisName( command );
        tab["commands"] = tabCommands( command, options );

        // The traces: those asked for, each keeping what the person set on it (colour, cursors)
        nlohmann::json traces = nlohmann::json::array();
        nlohmann::json old = tab.contains( "traces" ) && tab["traces"].is_array() ? tab["traces"]
                                                                                 : nlohmann::json::array();

        for( const std::string& signal : request.signals() )
        {
            nlohmann::json trace;

            for( const nlohmann::json& existing : old )
            {
                if( existing.contains( "signal" ) && existing["signal"] == signal )
                    trace = existing;
            }

            if( trace.is_null() )
            {
                trace = nlohmann::json( { { "trace_type", traceTypeOf( wxString::FromUTF8( signal ), command ) },
                                          { "signal", signal } } );
            }

            traces.push_back( trace );
        }

        if( !request.signals().empty() || !tab.contains( "traces" ) )
            tab["traces"] = traces;

        if( !request.measurements().empty() || !tab.contains( "measurements" ) )
        {
            nlohmann::json measurements = nlohmann::json::array();

            for( const Measurement& measurement : request.measurements() )
                measurements.push_back( nlohmann::json( { { "expr", measurement.expr() },
                                                          { "format", measurement.format() } } ) );

            tab["measurements"] = measurements;
        }
    }

    bool changed = js != before || mode != settings->GetCompatibilityMode() || stored.IsEmpty() || !live.IsEmpty();

    if( changed )
    {
        std::string text = js.dump( 2 ) + "\n";
        wxFFile     file( path, wxS( "wb" ) );

        if( !file.IsOpened() || !file.Write( text.data(), text.size() ) )
        {
            return tl::unexpected( statusError( ApiStatusCode::AS_INTERNAL_ERROR,
                                                fmt::format( "could not write the workbook {}", utf8( path ) ) ) );
        }

        file.Close();

        wxFileName relative( path );
        relative.MakeRelativeTo( sch->Project().GetProjectPath() );
        settings->SetWorkbookFilename( relative.GetFullPath() );
        settings->SetCompatibilityMode( mode );
        settings->SaveToFile();
        Pgm().GetSettingsManager().SaveProject( sch->Project().GetProjectFullName(), &sch->Project() );

        // The open Simulator window takes the workbook as it now is
        simSyncWorkbook( true );
    }

    response.set_tab( index );
    response.set_changed( changed );
    packSetup( sch, context()->GetKiway(), response.mutable_setup() );
    return response;
}


HANDLER_RESULT<RunSimulationResponse>
API_HANDLER_SCH::handleRunSimulation( const HANDLER_CONTEXT<RunSimulation>& aCtx )
{
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( std::optional<ApiResponseStatus> busy = simNetlistReady() )
        return tl::unexpected( *busy );

    SIM_API::RUNNER& runner = SIM_API::RUNNER::Get();

    if( runner.Busy() )
    {
        return tl::unexpected( statusError( ApiStatusCode::AS_BUSY,
                                            "a simulation is running: collect it with GetSimulationResult or "
                                            "stop it with StopSimulation" ) );
    }

    wxLogNull            quiet;
    const RunSimulation& request = aCtx.Request;
    SCHEMATIC*           sch = schematic();
    SIM_API::RUN_REQUEST run;

    run.options = SIM_API::OptionsFromProto( request.options() );

    wxString command = wxString::FromUTF8( request.command() );
    command.Trim().Trim( false );

    // A host's own netlist: its analysis is the request's command, else the one the text holds
    const bool raw = !request.netlist().empty();

    if( raw && command.IsEmpty() )
    {
        wxStringTokenizer textLines( wxString::FromUTF8( request.netlist() ), wxS( "\r\n" ), wxTOKEN_STRTOK );

        while( textLines.HasMoreTokens() && command.IsEmpty() )
        {
            wxString line = textLines.GetNextToken().Trim().Trim( false );

            if( line.StartsWith( wxS( "." ) ) && SPICE_CIRCUIT_MODEL::IsSimCommand( line ) )
                command = line;
        }

        if( command.IsEmpty() )
        {
            return tl::unexpected( statusError( ApiStatusCode::AS_BAD_REQUEST,
                                                "the netlist has no analysis line and no command was given" ) );
        }
    }

    if( command.IsEmpty() )
    {
        unsigned saved = run.options;
        command = SIM_API::SavedCommand( sch, context()->GetKiway(), &saved );

        if( !request.has_options() )
            run.options = saved | NETLIST_EXPORTER_SPICE::OPTION_ADJUST_INCLUDE_PATHS;

        if( command.IsEmpty() )
        {
            return tl::unexpected( statusError( ApiStatusCode::AS_BAD_REQUEST,
                                                "no command given and the design has no saved analysis "
                                                "(SetSimulationSetup, or an analysis written on a sheet)" ) );
        }
    }

    // The analysis line(s) go in the netlist; a frequency content analysis is ngspice commands on
    // the transient's result, run after it (as the Simulator's FFT tab does)
    wxStringTokenizer lines( command, wxS( "\r\n" ), wxTOKEN_STRTOK );

    while( lines.HasMoreTokens() )
    {
        wxString line = lines.GetNextToken().Trim().Trim( false );
        wxString lower = line.Lower();

        if( line.IsEmpty() )
            continue;

        if( lower.StartsWith( wxS( ".fft" ) ) )
            line = line.Mid( 1 );

        if( lower.StartsWith( wxS( "fft" ) ) || lower.StartsWith( wxS( ".fft" ) )
            || lower.StartsWith( wxS( "linearize" ) ) )
        {
            run.postCommands.push_back( utf8( line ) );
        }
        else
        {
            run.command += ( run.command.IsEmpty() ? wxS( "" ) : wxS( "\n" ) ) + line;
        }
    }

    if( run.command.IsEmpty() || !SPICE_CIRCUIT_MODEL::IsSimCommand( run.command ) )
    {
        return tl::unexpected( statusError( ApiStatusCode::AS_BAD_REQUEST,
                                            fmt::format( "'{}' has no analysis line (.op, .tran, .ac, .dc, .noise, "
                                                         ".pz, .tf, .sp; fft only after a .tran line)",
                                                         request.command() ) ) );
    }

    for( const std::string& signal : request.signals() )
        run.signals.push_back( signal );

    for( const std::string& measure : request.measures() )
        run.measures.push_back( measure );

    if( raw )
    {
        std::string text = request.netlist();

        if( !text.empty() && text.back() != '\n' )
            text += '\n';

        if( !wxString::FromUTF8( request.command() ).Trim().IsEmpty() )
        {
            // The request's analysis goes before the text's .end
            std::string analysis = utf8( run.command ) + "\n";
            size_t      end = std::string::npos;
            size_t      at = 0;

            while( at < text.size() )
            {
                size_t      next = text.find( '\n', at );
                std::string line = text.substr( at, next == std::string::npos ? std::string::npos : next - at );
                wxString    trimmed = wxString::FromUTF8( line ).Trim().Trim( false ).Lower();

                if( trimmed == wxS( ".end" ) )
                    end = at;

                if( next == std::string::npos )
                    break;

                at = next + 1;
            }

            if( end == std::string::npos )
                text += analysis + ".end\n";
            else
                text.insert( end, analysis );
        }

        run.netlistText = text;
    }

    run.includeNetlist = request.include_netlist();
    run.show = request.show() && m_frame && !raw && request.breaks().empty();
    run.timeoutMs = request.timeout_ms() ? request.timeout_ms() : 120000;
    run.maxPoints = request.max_points();

    // Every symbol needs its reference: the exporter (and the Simulator window, with a dialog)
    // refuses a schematic that is not annotated
    if( !raw )
    {
        SCH_REFERENCE_LIST references;
        sch->Hierarchy().GetSymbols( references, SYMBOL_FILTER_NON_POWER );

        wxString first;
        int      errors = references.GetCount() == 0 ? 0
                                                     : references.CheckAnnotation(
                                                               [&]( ERCE_T, const wxString& aMessage, SCH_REFERENCE*,
                                                                    SCH_REFERENCE* )
                                                               {
                                                                   if( first.IsEmpty() )
                                                                       first = aMessage;
                                                               } );

        if( errors > 0 )
        {
            RunSimulationResponse early;
            Problem*              problem = early.add_problems();

            problem->set_kind( PK_NETLIST );
            problem->set_severity( PS_ERROR );
            problem->set_message( fmt::format( "the schematic is not fully annotated ({} problem(s)): {}", errors,
                                               utf8( oneLine( first ) ) ) );
            early.set_status( SIM_NETLIST_ERROR );
            early.set_analysis( SIM_API::AnalysisName( run.command ) );
            early.set_command( utf8( command ) );
            return early;
        }
    }

    // A symbol without a usable model is told by KiCad's own model code, with its reference;
    // ngspice is not given a netlist that has one (it cannot recover from some of them)
    if( !raw )
    {
        RunSimulationResponse early;

        SIM_API::ModelProblems( sch, early.mutable_problems() );

        if( !early.problems().empty() )
        {
            early.set_status( SIM_MODEL_ERROR );
            early.set_analysis( SIM_API::AnalysisName( run.command ) );
            early.set_command( utf8( command ) );
            early.mutable_timings()->set_netlist_ms(
                    std::chrono::duration_cast<std::chrono::microseconds>( std::chrono::steady_clock::now()
                                                                           - started ).count() / 1000.0 );
            early.mutable_timings()->set_total_ms( early.timings().netlist_ms() );
            return early;
        }
    }

    std::shared_ptr<SIM_API::CIRCUIT> circuit = std::make_shared<SIM_API::CIRCUIT>( sch, context()->GetKiway() );
    std::string                       overrideError;

    if( !SIM_API::SetOverrides( *circuit, request.overrides(), overrideError ) )
        return tl::unexpected( statusError( ApiStatusCode::AS_BAD_REQUEST, overrideError ) );

    std::vector<wxString> directives;

    for( const std::string& directive : request.directives() )
        directives.push_back( wxString::FromUTF8( directive ) );

    SIM_API::SetRun( *circuit, request.directives(), raw ? google::protobuf::RepeatedPtrField<Break>()
                                                         : request.breaks() );

    std::shared_ptr<SPICE_SETTINGS> settings;

    if( sch->Project().GetProjectFile().m_SchematicSettings )
        settings = sch->Project().GetProjectFile().m_SchematicSettings->m_NgspiceSettings;

    // The Simulator window.  With `show` the run is its own (opened if needed; a frequency
    // content analysis is not, its tab needs a transient tab of the window's first).  Without, a
    // window that is open takes its own circuit back when the run ends.
    KIWAY* kiway = context()->GetKiway();

    if( m_frame && kiway )
    {
        if( run.show && run.postCommands.empty() )
        {
            if( !kiway->Player( FRAME_SIMULATOR, false ) || !kiway->Player( FRAME_SIMULATOR, false )->IsShown() )
                frame()->GetToolManager()->RunAction( SCH_ACTIONS::showSimulator );

            if( kiway->Player( FRAME_SIMULATOR, false ) )
            {
                std::map<wxString, std::vector<NETLIST_EXPORTER_SPICE::FIELD_OVERRIDE>> overrides;

                for( const Override& override : request.overrides() )
                {
                    overrides[wxString::FromUTF8( override.reference() )].push_back(
                            { wxString::FromUTF8( override.field() ), wxString::FromUTF8( override.value() ) } );
                }

                wxString windowCommand = run.command;
                unsigned options = run.options;

                run.startInWindow = [kiway, windowCommand, options, overrides, directives]() -> bool
                {
                    SIMULATOR_FRAME* window = static_cast<SIMULATOR_FRAME*>( kiway->Player( FRAME_SIMULATOR, false ) );
                    return window && window->StartApiRun( windowCommand, options, overrides, directives );
                };

                run.onEnd = [kiway]( const RunSimulationResponse& aResult )
                {
                    SIMULATOR_FRAME* window = static_cast<SIMULATOR_FRAME*>( kiway->Player( FRAME_SIMULATOR, false ) );

                    if( !window )
                        return;

                    std::vector<wxString> signals;

                    for( const Vector& vector : aResult.vectors() )
                    {
                        if( !vector.real().empty() && !vector.signal().empty()
                            && ( vector.kind() == VK_VOLTAGE || vector.kind() == VK_CURRENT ) )
                        {
                            signals.push_back( wxString::FromUTF8( vector.signal() ) );
                        }
                    }

                    window->EndApiRun( signals );
                };
            }
        }

        if( !run.startInWindow )
        {
            run.show = false;

            run.onEnd = [kiway]( const RunSimulationResponse& )
            {
                if( KIWAY_PLAYER* window = kiway->Player( FRAME_SIMULATOR, false ) )
                    static_cast<SIMULATOR_FRAME*>( window )->RestoreAfterApiRun();
            };
        }
    }

    uint64_t id = runner.Start( circuit, run, sch->Project().GetProjectPath(), settings, started );

    unsigned waitMs = request.has_wait_ms() ? request.wait_ms() : 250;

    if( m_frame )
        waitMs = std::min( waitMs, 1000u );
    else
        waitMs = std::min( waitMs, run.timeoutMs + 1000 );

    RunSimulationResponse response;

    if( !runner.Collect( id, waitMs, response ) )
        return tl::unexpected( statusError( ApiStatusCode::AS_INTERNAL_ERROR, "the run was lost" ) );

    return response;
}


HANDLER_RESULT<RunSimulationResponse>
API_HANDLER_SCH::handleGetSimulationResult( const HANDLER_CONTEXT<GetSimulationResult>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    unsigned waitMs = aCtx.Request.has_wait_ms() ? aCtx.Request.wait_ms() : 250;

    if( m_frame )
        waitMs = std::min( waitMs, 1000u );
    else
        waitMs = std::min( waitMs, 600000u );

    RunSimulationResponse response;

    if( !SIM_API::RUNNER::Get().Collect( aCtx.Request.run_id(), waitMs, response ) )
    {
        return tl::unexpected( statusError( ApiStatusCode::AS_BAD_REQUEST,
                                            fmt::format( "no simulation run {} (a result is kept until the next "
                                                         "RunSimulation)", aCtx.Request.run_id() ) ) );
    }

    return response;
}


HANDLER_RESULT<StopSimulationResponse>
API_HANDLER_SCH::handleStopSimulation( const HANDLER_CONTEXT<StopSimulation>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    StopSimulationResponse response;
    response.set_stopping( SIM_API::RUNNER::Get().Stop( aCtx.Request.run_id() ) );
    return response;
}


wxString API_HANDLER_SCH::simSyncWorkbook( bool aReload )
{
    KIWAY* kiway = m_frame && context() ? context()->GetKiway() : nullptr;

    if( !kiway )
        return wxEmptyString;

    SIMULATOR_FRAME* window = static_cast<SIMULATOR_FRAME*>( kiway->Player( FRAME_SIMULATOR, false ) );

    if( !window )
        return wxEmptyString;

    if( aReload )
    {
        // The window takes the workbook as the file now has it
        window->LoadWorkbook( workbookPath( schematic() ) );
        return wxEmptyString;
    }

    // What the person has open and has not saved: a copy of it, for reading
    if( !window->IsWorkbookModified() )
        return wxEmptyString;

    wxString copy = wxFileName::CreateTempFileName( wxS( "kicad-sim-workbook" ) );

    if( copy.IsEmpty() || !window->WriteWorkbookCopy( copy ) )
        return wxEmptyString;

    // SaveWorkbook writes <name>.wbk
    wxFileName written( copy );
    written.SetExt( FILEEXT::WorkbookFileExtension );
    wxRemoveFile( copy );
    return written.GetFullPath();
}
