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
 * The simulation run of the API (kiapi.backplane.sim.RunSimulation): KiCad's circuit model is
 * attached to KiCad's ngspice instance and run in ngspice's background thread, as the Simulator
 * window does; when it ends the vectors, the measurements and the problems are read here.
 */

#include <api/api_sch_sim.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <regex>
#include <set>
#include <thread>

#include <wx/app.h>
#include <wx/regex.h>
#include <wx/tokenzr.h>

#include <richio.h>
#include <schematic.h>
#include <string_utils.h>

#include <sim/sim_model.h>
#include <sim/simulator.h>
#include <sim/spice_generator.h>
#include <sim/spice_settings.h>
#include <sim/spice_simulator.h>

using namespace kiapi::backplane::sim;
using namespace std::chrono;


namespace
{

std::string utf8( const wxString& aText )
{
    return std::string( aText.ToUTF8() );
}


std::string lower( std::string aText )
{
    std::transform( aText.begin(), aText.end(), aText.begin(),
                    []( unsigned char c )
                    {
                        return std::tolower( c );
                    } );
    return aText;
}


std::string trimmed( const std::string& aText )
{
    size_t first = aText.find_first_not_of( " \t\r\n" );

    if( first == std::string::npos )
        return {};

    size_t last = aText.find_last_not_of( " \t\r\n" );
    return aText.substr( first, last - first + 1 );
}


double msBetween( steady_clock::time_point aFrom, steady_clock::time_point aTo )
{
    return duration_cast<microseconds>( aTo - aFrom ).count() / 1000.0;
}


bool isGround( const std::string& aNode )
{
    std::string node = lower( aNode );
    return node == "0" || node == "gnd";
}

} // namespace


// ---------------------------------------------------------------------------------------------
// LINE_REPORTER
// ---------------------------------------------------------------------------------------------

REPORTER& SIM_API::LINE_REPORTER::Report( const wxString& aText, SEVERITY aSeverity )
{
    std::lock_guard<std::mutex> lock( m_mutex );
    m_lines.push_back( { aText, aSeverity } );

    if( m_forward )
        m_forward->Report( aText, aSeverity );

    return *this;
}


void SIM_API::LINE_REPORTER::SetForward( REPORTER* aReporter )
{
    std::lock_guard<std::mutex> lock( m_mutex );
    m_forward = aReporter;
}


REPORTER* SIM_API::LINE_REPORTER::Forward() const
{
    std::lock_guard<std::mutex> lock( m_mutex );
    return m_forward;
}


bool SIM_API::LINE_REPORTER::HasMessage() const
{
    std::lock_guard<std::mutex> lock( m_mutex );
    return !m_lines.empty();
}


bool SIM_API::LINE_REPORTER::HasMessageOfSeverity( int aSeverityMask ) const
{
    std::lock_guard<std::mutex> lock( m_mutex );

    for( const LINE& line : m_lines )
    {
        if( line.severity & aSeverityMask )
            return true;
    }

    return false;
}


std::vector<SIM_API::LINE_REPORTER::LINE> SIM_API::LINE_REPORTER::Lines() const
{
    std::lock_guard<std::mutex> lock( m_mutex );
    return m_lines;
}


std::vector<std::string> SIM_API::LINE_REPORTER::TextLines() const
{
    std::vector<std::string> out;

    for( const LINE& line : Lines() )
    {
        wxStringTokenizer tokenizer( line.text, wxS( "\r\n" ), wxTOKEN_STRTOK );

        while( tokenizer.HasMoreTokens() )
        {
            wxString text = tokenizer.GetNextToken();
            text.Trim( true );

            if( !text.IsEmpty() )
                out.push_back( utf8( text ) );
        }
    }

    return out;
}


size_t SIM_API::LINE_REPORTER::Count() const
{
    std::lock_guard<std::mutex> lock( m_mutex );
    return m_lines.size();
}


void SIM_API::LINE_REPORTER::Clear()
{
    std::lock_guard<std::mutex> lock( m_mutex );
    m_lines.clear();
}


// ---------------------------------------------------------------------------------------------
// CIRCUIT
// ---------------------------------------------------------------------------------------------

bool SIM_API::CIRCUIT::ReadSchematicAndLibraries( unsigned aNetlistOptions, REPORTER& aReporter )
{
    m_netNodes.clear();
    m_broken.clear();

    auto nameOf = []( const BREAK_REQUEST& aBreak ) -> wxString
    {
        wxString name = aBreak.reference + wxS( "_" ) + aBreak.pin;

        for( size_t ii = 0; ii < name.length(); ++ii )
        {
            if( !wxIsalnum( name[ii] ) && name[ii] != '_' )
                name[ii] = '_';
        }

        return name;
    };

    std::vector<PIN_BREAK> pinBreaks;

    for( const BREAK_REQUEST& request : m_breaks )
    {
        PIN_BREAK pinBreak;
        pinBreak.reference = request.reference;
        pinBreak.pin = request.pin;
        pinBreak.newNode = wxS( "brk_" ) + nameOf( request );
        pinBreaks.push_back( pinBreak );
    }

    SetPinBreaks( pinBreaks );
    SetExtraDirectives( m_runDirectives );

    bool ok = SPICE_CIRCUIT_MODEL::ReadSchematicAndLibraries( aNetlistOptions, aReporter );

    // What joins each broken pin to its net again, written with this run's other lines
    std::vector<wxString> lines = m_runDirectives;

    for( size_t ii = 0; ii < m_breaks.size() && ii < PinBreaks().size(); ++ii )
    {
        const PIN_BREAK& applied = PinBreaks()[ii];

        if( !applied.applied )
        {
            aReporter.Report( wxString::Format( wxS( "Break: the netlist has no pin %s of %s" ), applied.pin,
                                                applied.reference ),
                              RPT_SEVERITY_ERROR );
            ok = false;
            continue;
        }

        wxString insert = m_breaks[ii].insert;
        insert.Trim( true ).Trim( false );

        if( insert.IsEmpty() )
            insert = wxS( "Vbrk_{name} {net} {pin} dc 0 ac 0" );

        insert.Replace( wxS( "{net}" ), applied.node );
        insert.Replace( wxS( "{pin}" ), applied.newNode );
        insert.Replace( wxS( "{name}" ), nameOf( m_breaks[ii] ) );

        lines.push_back( insert );

        kiapi::backplane::sim::BrokenPin broken;
        broken.set_reference( utf8( applied.reference ) );
        broken.set_pin( utf8( applied.pin ) );
        broken.set_net_node( utf8( applied.node ) );
        broken.set_pin_node( utf8( applied.newNode ) );
        broken.set_insert( utf8( insert ) );
        m_broken.push_back( broken );
    }

    SetExtraDirectives( lines );
    return ok;
}


void SIM_API::SetRun( CIRCUIT& aCircuit, const google::protobuf::RepeatedPtrField<std::string>& aDirectives,
                      const google::protobuf::RepeatedPtrField<Break>& aBreaks )
{
    std::vector<wxString>               directives;
    std::vector<CIRCUIT::BREAK_REQUEST> breaks;

    for( const std::string& directive : aDirectives )
        directives.push_back( wxString::FromUTF8( directive ) );

    for( const Break& item : aBreaks )
    {
        breaks.push_back( { wxString::FromUTF8( item.reference() ), wxString::FromUTF8( item.pin() ),
                            wxString::FromUTF8( item.insert() ) } );
    }

    aCircuit.SetRun( std::move( directives ), std::move( breaks ) );
}


wxString SIM_API::CIRCUIT::SheetCommand()
{
    if( m_exportSheets.empty() )
        m_exportSheets = m_schematic->Hierarchy();

    return GetSchTextSimCommand().Trim().Trim( false );
}


void SIM_API::CIRCUIT::WriteTail( OUTPUTFORMATTER& aFormatter, unsigned aNetlistOptions )
{
    SPICE_CIRCUIT_MODEL::WriteTail( aFormatter, aNetlistOptions );
    m_writtenAt = steady_clock::now();
}


wxString SIM_API::CIRCUIT::GenerateItemPinNetName( const wxString& aNetName, int& aNcCounter ) const
{
    wxString node = SPICE_CIRCUIT_MODEL::GenerateItemPinNetName( aNetName, aNcCounter );

    if( !aNetName.IsEmpty() )
        m_netNodes[UnescapeString( aNetName )] = node;

    return node;
}


// ---------------------------------------------------------------------------------------------
// Topology
// ---------------------------------------------------------------------------------------------

void SIM_API::TopologyProblems( const CIRCUIT& aCircuit, google::protobuf::RepeatedPtrField<Problem>* aOut )
{
    // Union-find over the nodes: two nodes are joined when a device gives a DC path between them
    std::map<std::string, std::string> parent;
    std::map<std::string, std::string> users;   // node -> "C1 pin 1, C2 pin 2"
    bool                               anyItem = false;
    bool                               ground = false;

    std::function<std::string( const std::string& )> find = [&]( const std::string& aNode ) -> std::string
    {
        auto it = parent.find( aNode );

        if( it == parent.end() )
        {
            parent[aNode] = aNode;
            return aNode;
        }

        if( it->second == aNode )
            return aNode;

        std::string root = find( it->second );
        parent[aNode] = root;
        return root;
    };

    auto join = [&]( const std::string& aLhs, const std::string& aRhs )
    {
        std::string lhs = find( aLhs );
        std::string rhs = find( aRhs );

        if( lhs != rhs )
            parent[lhs] = rhs;
    };

    auto nodeKey = []( const std::string& aNode ) -> std::string
    {
        return isGround( aNode ) ? std::string( "0" ) : lower( aNode );
    };

    for( const SPICE_ITEM& item : aCircuit.GetItems() )
    {
        if( !item.model || !item.model->IsEnabled() )
            continue;

        anyItem = true;

        SIM_MODEL::DEVICE_T device = item.model->GetDeviceType();
        bool                isolating = device == SIM_MODEL::DEVICE_T::C || device == SIM_MODEL::DEVICE_T::I;
        bool                mos = device == SIM_MODEL::DEVICE_T::NMOS || device == SIM_MODEL::DEVICE_T::PMOS;
        std::string         first;

        // The element's pins: its model's, or (raw SPICE without a pin map) the symbol's in order
        std::vector<std::pair<std::string, std::string>> pins;   // symbol pin number, model node

        for( const SIM_MODEL_PIN& pin : item.model->GetPins() )
        {
            if( !pin.symbolPinNumber.IsEmpty() )
                pins.emplace_back( utf8( pin.symbolPinNumber ), pin.modelPinName );
        }

        if( item.model->GetPinCount() == 0 )
        {
            for( const std::string& number : item.pinNumbers )
                pins.emplace_back( number, std::string() );
        }

        for( const auto& [pinNumber, modelPin] : pins )
        {
            auto it = std::find( item.pinNumbers.begin(), item.pinNumbers.end(), pinNumber );

            if( it == item.pinNumbers.end() )
                continue;

            size_t index = it - item.pinNumbers.begin();

            if( index >= item.pinNetNames.size() )
                continue;

            std::string node = nodeKey( item.pinNetNames[index] );

            find( node );
            ground |= node == "0";

            std::string& who = users[node];

            if( who.size() < 120 )
                who += ( who.empty() ? "" : ", " ) + item.refName + " pin " + pinNumber;

            // A capacitor and a current source pass no DC; a MOS gate neither
            if( isolating || ( mos && lower( modelPin ) == "g" ) )
                continue;

            if( first.empty() )
                first = node;
            else
                join( first, node );
        }
    }

    // A broken pin is joined to its net again by what was inserted (taken as a DC path: the
    // default is a 0 V source)
    for( const NETLIST_EXPORTER_SPICE::PIN_BREAK& pinBreak : aCircuit.PinBreaks() )
    {
        if( pinBreak.applied )
        {
            ground |= isGround( utf8( pinBreak.node ) );
            join( nodeKey( utf8( pinBreak.node ) ), nodeKey( utf8( pinBreak.newNode ) ) );
        }
    }

    if( !anyItem )
        return;

    if( !ground )
    {
        Problem* problem = aOut->Add();
        problem->set_kind( PK_NO_GROUND );
        problem->set_severity( PS_ERROR );
        problem->set_node( "0" );
        problem->set_message( "the circuit has no ground: no net is node 0 (place a GND power symbol; "
                              "ngspice needs a reference node)" );
        return;
    }

    std::map<std::string, std::string> nets;

    for( const auto& [net, node] : aCircuit.NetNodes() )
        nets.try_emplace( lower( utf8( node ) ), utf8( net ) );

    std::string groundRoot = find( "0" );
    int         reported = 0;

    for( const auto& [node, who] : users )
    {
        if( find( node ) == groundRoot )
            continue;

        if( ++reported > 20 )
            break;

        Problem* problem = aOut->Add();
        problem->set_kind( PK_FLOATING_NODE );
        problem->set_severity( PS_WARNING );
        problem->set_node( node );

        if( auto it = nets.find( node ); it != nets.end() )
            problem->set_net( it->second );

        problem->set_message( "node " + node + " has no DC path to ground (connected to: " + who + ")" );
    }
}


// ---------------------------------------------------------------------------------------------
// RUNNER
// ---------------------------------------------------------------------------------------------

namespace
{

enum class STOP_REASON
{
    NONE,
    TIMEOUT,
    ABORT
};


/// What ngspice's console says went wrong, as problems.  aLoadLines: how many of the lines came
/// before the run started (ngspice reading the netlist).
void consoleProblems( const std::vector<std::string>& aLines, size_t aLoadLines,
                      const std::map<std::string, std::string>& aItemRefs,
                      const std::map<std::string, std::string>& aNodeNets,
                      google::protobuf::RepeatedPtrField<Problem>* aOut, bool& aFailed )
{
    static const std::regex checkNode( R"(check nodes?\s+(\S+)(?:\s+and\s+(\S+))?)", std::regex::icase );
    static const std::regex troubleNode( R"rx(trouble with node\s+"?([^"\s]+)"?)rx", std::regex::icase );
    static const std::regex troubleInstance( R"(trouble with\s+\S*-instance\s+(\S+))", std::regex::icase );
    static const std::regex modelName( R"rx(model\s+[`'"]?([^\s`'"]+))rx", std::regex::icase );
    static const std::regex subcktLine( R"(unknown subckt:\s*(\S+))", std::regex::icase );

    std::set<std::string> seen;
    std::string           lastItemRef;

    auto refOf = [&]( std::string aItem ) -> std::string
    {
        aItem = lower( aItem );

        // "x1.q3" names a device inside a subcircuit instance: the symbol is the instance
        if( size_t dot = aItem.find( '.' ); dot != std::string::npos )
        {
            std::string head = aItem.substr( 0, dot );
            std::string tail = aItem.substr( dot + 1 );

            for( const std::string& candidate : { tail, head } )
            {
                if( auto it = aItemRefs.find( candidate ); it != aItemRefs.end() )
                    return it->second;
            }
        }

        if( auto it = aItemRefs.find( aItem ); it != aItemRefs.end() )
            return it->second;

        return {};
    };

    auto add = [&]( ProblemKind aKind, ProblemSeverity aSeverity, std::string aRef, const std::string& aRawNode,
                    const std::string& aMessage )
    {
        std::string node = lower( aRawNode );

        // ".probe alli" splits a node per device pin: "probe_int_<node>_<device>" is <node>, at
        // <device>
        if( node.rfind( "probe_int_", 0 ) == 0 )
        {
            std::string rest = node.substr( 10 );

            for( const auto& [known, net] : aNodeNets )
            {
                if( rest.size() > known.size() + 1 && rest.compare( 0, known.size(), known ) == 0
                    && rest[known.size()] == '_' )
                {
                    std::string device = rest.substr( known.size() + 1 );

                    // a subcircuit pin adds "_<pin number>"
                    for( std::string candidate = device; !candidate.empty(); )
                    {
                        if( std::string ref = refOf( candidate ); !ref.empty() )
                        {
                            if( aRef.empty() )
                                aRef = ref;

                            break;
                        }

                        size_t cut = candidate.rfind( '_' );
                        candidate = cut == std::string::npos ? std::string() : candidate.substr( 0, cut );
                    }

                    node = known;
                    break;
                }
            }
        }

        std::string key = std::to_string( aKind ) + "|" + aRef + "|" + node;

        if( !seen.insert( key ).second || aOut->size() >= 60 )
            return;

        Problem* problem = aOut->Add();
        problem->set_kind( aKind );
        problem->set_severity( aSeverity );
        problem->set_reference( aRef );
        problem->set_node( node );

        if( auto it = aNodeNets.find( node ); it != aNodeNets.end() )
            problem->set_net( it->second );

        problem->set_message( aMessage );
    };

    for( size_t ii = 0; ii < aLines.size(); ++ii )
    {
        const std::string  text = trimmed( aLines[ii] );
        const std::string  line = lower( text );
        const bool         loading = ii < aLoadLines;
        std::smatch        match;

        if( line.empty() )
            continue;

        // An echoed netlist line names the item the next message is about
        {
            std::string token = line.substr( 0, line.find_first_of( " \t" ) );

            if( std::string ref = refOf( token ); !ref.empty() )
                lastItemRef = ref;
        }

        bool isError = line.rfind( "error", 0 ) == 0 || line.find( " error:" ) != std::string::npos
                       || line.rfind( "fatal", 0 ) == 0;
        ProblemSeverity severity = isError ? PS_ERROR : PS_WARNING;

        if( line.find( "singular matrix" ) != std::string::npos )
        {
            std::string node;
            std::string ref;

            if( std::regex_search( text, match, checkNode ) )
            {
                node = match[1].str();

                // ngspice names a branch equation as "<device>#branch"
                if( size_t hash = node.find( '#' ); hash != std::string::npos )
                {
                    ref = refOf( node.substr( 0, hash ) );
                    node.clear();
                }
            }

            add( PK_SINGULAR_MATRIX, PS_ERROR, ref, node, text );

            if( match.size() > 2 && match[2].matched )
            {
                std::string other = match[2].str();

                if( size_t hash = other.find( '#' ); hash != std::string::npos )
                    add( PK_SINGULAR_MATRIX, PS_ERROR, refOf( other.substr( 0, hash ) ), "", text );
                else
                    add( PK_SINGULAR_MATRIX, PS_ERROR, "", other, text );
            }
        }
        else if( line.find( "timestep too small" ) != std::string::npos
                 || line.find( "iteration limit reached" ) != std::string::npos
                 || line.find( "no convergence" ) != std::string::npos
                 || line.find( "convergence problems" ) != std::string::npos
                 || line.find( "could not be simulated successfully" ) != std::string::npos
                 || line.find( "without convergence" ) != std::string::npos
                 || line.find( "transient op failed" ) != std::string::npos
                 || line.find( "trouble with" ) != std::string::npos )
        {
            std::string node;
            std::string ref;

            if( std::regex_search( text, match, troubleNode ) )
                node = match[1].str();
            else if( std::regex_search( text, match, troubleInstance ) )
                ref = refOf( match[1].str() );

            add( PK_NO_CONVERGENCE, PS_ERROR, ref, node, text );
        }
        else if( line.find( "mif-error" ) != std::string::npos
                 || ( line.find( "code model" ) != std::string::npos && isError ) )
        {
            add( PK_CODE_MODELS, PS_ERROR, lastItemRef, "", text );
            aFailed = true;
        }
        else if( line.find( "unknown subckt" ) != std::string::npos )
        {
            std::string ref = lastItemRef;

            if( std::regex_search( text, match, subcktLine ) )
            {
                if( std::string named = refOf( match[1].str() ); !named.empty() )
                    ref = named;
            }

            add( PK_MISSING_MODEL, PS_ERROR, ref, "", text );
            aFailed = true;
        }
        else if( line.find( "unable to find definition of model" ) != std::string::npos
                 || line.find( "could not find a valid modelname" ) != std::string::npos
                 || line.find( "can't find model" ) != std::string::npos
                 || line.find( "cannot find model" ) != std::string::npos
                 || line.find( "unknown model type" ) != std::string::npos
                 || line.find( "no such model" ) != std::string::npos
                 || line.find( "model issue on line" ) != std::string::npos )
        {
            add( PK_MISSING_MODEL, PS_ERROR, lastItemRef, "", text );
            aFailed = true;
        }
        else if( line.find( "could not find include file" ) != std::string::npos
                 || line.find( "can't find include" ) != std::string::npos
                 || line.find( "could not open library file" ) != std::string::npos
                 || ( line.find( "include" ) != std::string::npos
                      && line.find( "no such file" ) != std::string::npos ) )
        {
            add( PK_LIBRARY_NOT_FOUND, PS_ERROR, "", "", text );
            aFailed = true;
        }
        else if( line.find( "no circuits loaded" ) != std::string::npos
                 || line.find( "there aren't any circuits loaded" ) != std::string::npos
                 || line.find( "circuit not parsed" ) != std::string::npos
                 || line.find( "cannot recover and awaits to be reset" ) != std::string::npos )
        {
            // The consequence of an error already told above
            aFailed = true;
        }
        else if( line.find( "simulation(s) aborted" ) != std::string::npos
                 || line.find( "simulation interrupted due to error" ) != std::string::npos
                 || line.find( "analysis failed" ) != std::string::npos
                 || line.find( "run simulation(s) aborted" ) != std::string::npos )
        {
            aFailed = true;
        }
        else if( isError )
        {
            // ngspice's own echo of our measurement misses is the measure's failure, not the run's
            if( line.find( "measure" ) != std::string::npos || line.find( "meas " ) != std::string::npos )
                continue;

            add( loading ? PK_NETLIST : PK_OTHER, severity, lastItemRef, "", text );

            if( loading )
                aFailed = true;
        }
    }
}

} // namespace


struct SIM_API::RUNNER::IMPL : public SIM_STATE_LISTENER
{
    std::shared_ptr<SPICE_SIMULATOR> sim;
    std::string                      ngspiceVersion;

    // The current (or last) run
    uint64_t                 id = 0;
    bool                     active = false;      ///< started and not yet finalized
    RUN_REQUEST              request;
    std::shared_ptr<CIRCUIT> circuit;
    SCHEMATIC*               schematic = nullptr;
    LINE_REPORTER            console;
    size_t                   loadLines = 0;
    RunSimulationResponse    result;
    bool                     complete = false;
    std::atomic<STOP_REASON> stopReason{ STOP_REASON::NONE };

    std::map<std::string, std::string> itemRefs;   ///< lower-case SPICE item name -> reference
    std::map<std::string, std::string> nodeNets;   ///< lower-case ngspice node -> KiCad net
    std::set<std::string>              nodes;
    int                                devices = 0;
    google::protobuf::RepeatedPtrField<Problem> topology;
    google::protobuf::RepeatedPtrField<Problem> netlistProblems;

    steady_clock::time_point requestStart;
    steady_clock::time_point attachStart;
    steady_clock::time_point attachEnd;
    steady_clock::time_point runStart;
    steady_clock::time_point runEnd;
    steady_clock::time_point deadline;

    // Set by ngspice's thread
    std::mutex              mutex;
    std::condition_variable cv;
    bool                    finished = false;

    std::unique_lock<std::mutex> simLock;
    REPORTER*                    previousReporter = nullptr;
    SIM_STATE_LISTENER*          previousListener = nullptr;
    std::thread                  watchdog;
    std::set<std::string>        measureNames;
    std::vector<BrokenPin>       broken;
    bool                         inWindow = false;            ///< the run is the Simulator window's own
    SIM_STATE_LISTENER*          forwardListener = nullptr;   ///< the window's, told as well (under mutex)
    std::vector<std::string>     ownedPlots;

    void OnSimStateChange( SIMULATOR* aObject, SIM_STATE aNewState ) override
    {
        {
            std::lock_guard<std::mutex> lock( mutex );

            // The Simulator window follows its own run
            if( forwardListener )
                forwardListener->OnSimStateChange( aObject, aNewState );

            if( aNewState != SIM_IDLE || finished )
                return;

            finished = true;
            runEnd = steady_clock::now();
        }

        cv.notify_all();

        // Whoever is not waiting in Collect still gets the run finished (the simulator given
        // back, the result kept)
        if( wxTheApp )
        {
            uint64_t run = id;

            wxTheApp->CallAfter(
                    [run]()
                    {
                        IMPL& impl = *RUNNER::Get().m_impl;

                        if( impl.active && impl.id == run )
                            impl.Finalize();
                    } );
        }
    }

    bool Finished()
    {
        std::lock_guard<std::mutex> lock( mutex );
        return finished;
    }

    void StopRun( STOP_REASON aReason )
    {
        STOP_REASON none = STOP_REASON::NONE;

        if( !active || Finished() )
            return;

        if( stopReason.compare_exchange_strong( none, aReason ) )
            sim->Stop();
    }

    void EnsureSimulator()
    {
        if( sim )
            return;

        sim = SIMULATOR::CreateInstance( "ngspice" );
    }

    std::string Version()
    {
        if( !ngspiceVersion.empty() || !sim )
            return ngspiceVersion;

        LINE_REPORTER reporter;
        REPORTER*     previous = sim->GetReporter();

        sim->SetReporter( &reporter );
        sim->Command( "version" );
        sim->SetReporter( previous );

        static const std::regex version( R"(ngspice-([0-9][0-9A-Za-z.+\-]*))" );

        for( const std::string& line : reporter.TextLines() )
        {
            std::smatch match;

            if( std::regex_search( line, match, version ) )
            {
                ngspiceVersion = match[1].str();
                break;
            }
        }

        if( ngspiceVersion.empty() )
            ngspiceVersion = "unknown";

        return ngspiceVersion;
    }

    void ReleaseSimulator()
    {
        if( watchdog.joinable() )
        {
            cv.notify_all();
            watchdog.join();
        }

        if( sim )
        {
            SIM_STATE_LISTENER* listener = nullptr;
            REPORTER*           reporter = nullptr;

            {
                std::lock_guard<std::mutex> lock( mutex );
                listener = previousListener;
                reporter = previousReporter;
                forwardListener = nullptr;
            }

            console.SetForward( nullptr );
            sim->SetSimStateListener( listener );
            sim->SetReporter( reporter );

            // The circuit holds the schematic: it is not kept past the run (the window's own
            // circuit is the window's)
            if( !inWindow )
            {
                NULL_REPORTER devnull;
                sim->Attach( nullptr, wxEmptyString, 0, wxEmptyString, devnull );
            }
        }

        {
            std::lock_guard<std::mutex> lock( mutex );
            previousListener = nullptr;
            previousReporter = nullptr;
        }
        circuit.reset();
        schematic = nullptr;

        if( simLock.owns_lock() )
            simLock.unlock();

        active = false;
    }

    void FillCommon( RunSimulationResponse& aOut )
    {
        aOut.set_run_id( id );
        aOut.set_shown( inWindow );

        if( sim )
            aOut.set_code_models( sim->CodeModelsPath() );

        for( const BrokenPin& pin : broken )
            *aOut.add_broken() = pin;

        aOut.set_analysis( AnalysisName( request.command ) );

        std::string command = utf8( request.command );

        for( const std::string& post : request.postCommands )
            command += "\n" + post;

        aOut.set_command( command );
        aOut.set_ngspice_version( ngspiceVersion );
        aOut.set_nodes( static_cast<int>( nodes.size() ) );
        aOut.set_devices( devices );

        Timings* timings = aOut.mutable_timings();
        // The window's own run writes and loads its netlist itself: one figure for both
        steady_clock::time_point written = circuit && circuit->WrittenAt() >= attachStart ? circuit->WrittenAt()
                                                                                        : attachEnd;

        timings->set_netlist_ms( msBetween( attachStart, written ) );
        timings->set_load_ms( msBetween( written, attachEnd ) );
    }

    void Log( RunSimulationResponse& aOut )
    {
        std::vector<std::string> lines = console.TextLines();
        size_t                   first = lines.size() > 200 ? lines.size() - 200 : 0;
        std::string              log;

        for( size_t ii = first; ii < lines.size(); ++ii )
            log += lines[ii] + "\n";

        aOut.set_log( log );
    }

    VectorKind KindOf( const std::string& aName, SIM_TYPE aType, std::string& aSignal )
    {
        std::string name = lower( aName );

        aSignal.clear();

        if( name == "time" )
            return VK_TIME;

        if( name == "frequency" )
            return VK_FREQUENCY;

        if( name.size() >= 6 && name.compare( name.size() - 6, 6, "-sweep" ) == 0 )
            return VK_SWEEP;

        if( name.rfind( "pole(", 0 ) == 0 )
            return VK_POLE;

        if( name.rfind( "zero(", 0 ) == 0 )
            return VK_ZERO;

        if( name.find( "noise" ) != std::string::npos )
            return VK_NOISE;

        if( aType == ST_SP )
        {
            static const std::regex sparam( R"(^[syz]_\d+_\d+$)" );

            if( std::regex_match( name, sparam ) )
                return VK_SPARAM;
        }

        wxString       signal;
        SIM_TRACE_TYPE type = SPT_UNKNOWN;

        if( circuit )
            type = circuit->VectorToSignal( aName, signal );

        if( type == SPT_CURRENT )
        {
            aSignal = utf8( signal );

            // "r1#branch" is the current of R1: named by the symbol, as KiCad shows it
            if( size_t hash = name.find( "#branch" ); hash != std::string::npos )
            {
                if( auto it = itemRefs.find( name.substr( 0, hash ) ); it != itemRefs.end() )
                    aSignal = "I(" + it->second + ")";
            }

            return VK_CURRENT;
        }

        if( type == SPT_POWER )
        {
            aSignal = utf8( signal );
            return VK_POWER;
        }

        if( type == SPT_VOLTAGE && ( nodes.count( name ) || isGround( name ) ) )
        {
            aSignal = utf8( signal );
            return VK_VOLTAGE;
        }

        // A vector a command made of a node's voltage (fft: "v(/out)")
        if( name.size() > 3 && name.rfind( "v(", 0 ) == 0 && name.back() == ')' )
        {
            aSignal = "V(" + name.substr( 2, name.size() - 3 ) + ")";
            return VK_VOLTAGE;
        }

        // A node inside a subcircuit instance ("x1.n3") or one a directive added
        if( type == SPT_VOLTAGE && aType != ST_TF && aType != ST_PZ && aType != ST_SP && aType != ST_NOISE
            && name.find( '(' ) == std::string::npos && name.find( '#' ) == std::string::npos )
        {
            aSignal = utf8( signal );
            return VK_VOLTAGE;
        }

        return VK_OTHER;
    }

    /// The lower-case vector names a requested signal may mean
    std::set<std::string> Wanted( const std::string& aRequest )
    {
        std::set<std::string> names;
        std::string           request = lower( trimmed( aRequest ) );

        request.erase( std::remove( request.begin(), request.end(), ' ' ), request.end() );
        names.insert( request );

        auto node = [&]( const std::string& aNet )
        {
            names.insert( aNet );
            names.insert( "v(" + aNet + ")" );

            for( const auto& [ngNode, net] : nodeNets )
            {
                std::string netLower = lower( net );

                if( netLower == aNet || netLower == "/" + aNet || ngNode == "/" + aNet )
                {
                    names.insert( ngNode );
                    names.insert( "v(" + ngNode + ")" );
                }
            }
        };

        if( request.size() > 3 && request.rfind( "v(", 0 ) == 0 && request.back() == ')' )
            node( request.substr( 2, request.size() - 3 ) );
        else
            node( request );

        return names;
    }

    void Decimate( const std::vector<double>& aIn, google::protobuf::RepeatedField<double>* aOut )
    {
        size_t count = aIn.size();

        if( request.maxPoints < 2 || count <= request.maxPoints )
        {
            aOut->Reserve( static_cast<int>( count ) );

            for( double value : aIn )
                aOut->AddAlreadyReserved( value );

            return;
        }

        size_t stride = ( count - 1 + request.maxPoints - 2 ) / ( request.maxPoints - 1 );

        for( size_t ii = 0; ii < count - 1; ii += stride )
            aOut->Add( aIn[ii] );

        aOut->Add( aIn[count - 1] );
    }

    /// Read the current plot's vectors into the response
    void ReadPlot( RunSimulationResponse& aOut, SIM_TYPE aType, bool aMainPlot, std::set<std::string>& aMatched )
    {
        // ngspice's constants are no result: nothing ran
        if( sim->CurrentPlotName() == wxS( "const" ) )
            return;

        std::vector<std::string> names = sim->AllVectors();
        std::string              plot = utf8( sim->CurrentPlotName() );
        std::string              axis = lower( utf8( sim->GetXAxis( aType ) ) );
        bool                     all = false;
        std::map<std::string, std::set<std::string>> wanted;

        for( const std::string& signal : request.signals )
        {
            if( trimmed( signal ) == "*" )
                all = true;
            else
                wanted[signal] = Wanted( signal );
        }

        bool anyData = all || !wanted.empty();

        // The sweep axis first
        std::stable_sort( names.begin(), names.end(),
                          [&]( const std::string& a, const std::string& b )
                          {
                              return ( lower( a ) == axis ) > ( lower( b ) == axis );
                          } );

        for( const std::string& name : names )
        {
            std::string         nameLower = lower( name );
            std::vector<double> real = sim->GetRealVector( name );
            std::vector<double> imag = sim->GetImaginaryVector( name );
            std::string         signal;
            VectorKind          kind = KindOf( name, aType, signal );
            bool                isAxis = !axis.empty() && nameLower == axis;
            bool                isComplex = !imag.empty() && !isAxis;

            if( real.empty() || measureNames.count( nameLower ) )
                continue;

            if( isAxis && aMainPlot )
                aOut.set_points( static_cast<int>( real.size() ) );

            if( real.size() == 1 && !isAxis )
            {
                Value* value = aOut.add_values();
                value->set_name( nameLower );
                value->set_signal( signal );
                value->set_value( real[0] );
                value->set_kind( kind );
            }

            bool send = anyData && ( all || isAxis );

            for( const auto& [asked, candidates] : wanted )
            {
                if( candidates.count( nameLower ) || candidates.count( lower( signal ) ) )
                {
                    send = true;
                    aMatched.insert( asked );
                }
            }

            // .op and .tf have no curves: their numbers are the values
            if( real.size() == 1 && !isComplex && !send )
                continue;

            Vector* vector = aOut.add_vectors();
            vector->set_name( nameLower );
            vector->set_signal( signal );
            vector->set_kind( kind );
            vector->set_length( static_cast<uint32_t>( real.size() ) );
            vector->set_complex( isComplex );

            if( !aMainPlot )
                vector->set_plot( plot );

            if( send )
            {
                Decimate( real, vector->mutable_real() );

                if( isComplex )
                    Decimate( imag, vector->mutable_imag() );
            }
        }
    }

    void Finalize()
    {
        if( !active || complete )
            return;

        // ngspice says "finished" from its thread a little before the thread is gone (it may still
        // be writing a file a directive asked for): commands given meanwhile are refused.  As the
        // Simulator window does, wait for it, briefly.
        for( int ii = 0; ii < 400 && sim->IsRunning(); ++ii )
            std::this_thread::sleep_for( milliseconds( 5 ) );

        RunSimulationResponse out;
        SIM_TYPE              type = SPICE_CIRCUIT_MODEL::CommandToSimType( request.command );
        STOP_REASON           stopped = stopReason.load();
        bool                  failed = false;

        FillCommon( out );
        out.mutable_timings()->set_run_ms( msBetween( runStart, runEnd ) );

        *out.mutable_problems() = netlistProblems;
        consoleProblems( console.TextLines(), loadLines, itemRefs, nodeNets, out.mutable_problems(), failed );

        std::set<std::string> matched;

        measureNames.clear();

        try
        {
            // Measurements, on the analysis' own plot
            for( const std::string& measure : request.measures )
            {
                std::string statement = trimmed( measure );
                std::string head = lower( statement.substr( 0, statement.find_first_of( " \t" ) ) );

                if( head == ".meas" || head == "meas" || head == ".measure" || head == "measure" )
                {
                    statement = trimmed( statement.substr( head.size() ) );
                    head = lower( statement.substr( 0, statement.find_first_of( " \t" ) ) );
                }

                static const std::set<std::string> analyses = { "tran", "ac", "dc", "sp", "noise", "op" };

                if( !analyses.count( head ) )
                    statement = lower( AnalysisName( request.command ) ) + " " + statement;

                // "<analysis> <name> ..."
                std::string rest = trimmed( statement.substr( statement.find_first_of( " \t" ) ) );
                std::string name = rest.substr( 0, rest.find_first_of( " \t" ) );

                size_t before = console.Count();

                sim->Command( "meas " + statement );

                std::vector<double> value = sim->GetRealVector( lower( name ) );
                Measure*            result = out.add_measures();

                result->set_name( name );
                measureNames.insert( lower( name ) );

                if( value.empty() || failed || std::isnan( value[0] ) )
                {
                    result->set_failed( true );

                    std::vector<LINE_REPORTER::LINE> lines = console.Lines();
                    std::string                      why;

                    for( size_t ii = before; ii < lines.size(); ++ii )
                    {
                        std::string text = trimmed( utf8( lines[ii].text ) );

                        if( !text.empty() )
                            why += ( why.empty() ? "" : "; " ) + text;
                    }

                    result->set_error( why.empty() ? std::string( "no value" ) : why );

                    Problem* problem = out.add_problems();
                    problem->set_kind( PK_MEASURE_FAILED );
                    problem->set_severity( PS_WARNING );
                    problem->set_message( "measure " + name + ": " + result->error() );
                }
                else
                {
                    result->set_value( value[0] );
                }
            }

            // The window's run leaves its plots to the window's tab
            auto own = [&]()
            {
                if( !inWindow && sim->CurrentPlotName() != wxS( "const" ) )
                    ownedPlots.push_back( utf8( sim->CurrentPlotName() ) );
            };

            own();

            if( type == ST_NOISE )
            {
                // ngspice makes two plots: the spectra (odd), then the integrated totals (even).
                // The totals are current when the run ends; the Simulator window, when the run
                // is its own, may have moved to the spectra already.  Either way both are read
                // and the current plot is left as it was found.
                std::string current = utf8( sim->CurrentPlotName() );
                long        number = 0;

                if( current.rfind( "noise", 0 ) == 0 && wxString( current.substr( 5 ) ).ToLong( &number )
                    && number >= 1 )
                {
                    long spectra = ( number % 2 == 0 ) ? number - 1 : number;

                    sim->Command( "setplot noise" + std::to_string( spectra + 1 ) );
                    own();
                    ReadPlot( out, type, false, matched );
                    sim->Command( "setplot noise" + std::to_string( spectra ) );
                    own();
                    ReadPlot( out, type, true, matched );
                    sim->Command( "setplot " + current );
                }
                else
                {
                    ReadPlot( out, type, true, matched );
                }
            }
            else if( !request.postCommands.empty() && !failed )
            {
                // The frequency content of the transient: ngspice commands on its plot
                for( const std::string& command : request.postCommands )
                    sim->Command( command );

                ownedPlots.push_back( utf8( sim->CurrentPlotName() ) );
                ReadPlot( out, ST_FFT, true, matched );
                out.set_analysis( "FFT" );
            }
            else
            {
                ReadPlot( out, type, true, matched );
            }
        }
        catch( const std::exception& e )
        {
            Problem* problem = out.add_problems();
            problem->set_kind( PK_OTHER );
            problem->set_severity( PS_ERROR );
            problem->set_message( std::string( "reading the results failed: " ) + e.what() );
            failed = true;
        }

        for( const std::string& signal : request.signals )
        {
            if( trimmed( signal ) == "*" || matched.count( signal ) || failed )
                continue;

            Problem* problem = out.add_problems();
            problem->set_kind( PK_OTHER );
            problem->set_severity( PS_WARNING );
            problem->set_message( "no vector for signal '" + signal + "' in this run" );
        }

        if( out.points() == 0 && !out.values().empty() )
            out.set_points( 1 );

        // A run that left nothing behind failed, whatever the console said
        if( out.vectors().empty() && out.values().empty() )
            failed = true;

        // The topology's findings: errors when the matrix was singular, else notes
        bool singular = false;
        bool convergence = false;
        bool model = false;
        bool netlist = false;

        for( const Problem& problem : out.problems() )
        {
            if( problem.severity() != PS_ERROR )
                continue;

            switch( problem.kind() )
            {
            case PK_SINGULAR_MATRIX:      singular = true;    break;
            case PK_NO_CONVERGENCE:       convergence = true; break;
            case PK_MISSING_MODEL:
            case PK_CODE_MODELS:
            case PK_LIBRARY_NOT_FOUND:
            case PK_MODEL_NOT_IN_LIBRARY:
            case PK_PIN_MISMATCH:         model = true;       break;
            case PK_NETLIST:
            case PK_BAD_PARAMS:           netlist = true;     break;
            default:                                          break;
            }
        }

        // A node without a DC path that made the matrix singular: an error even when ngspice got an
        // answer by its fallbacks (gmin stepping, a transient operating point), since that answer
        // is set by gmin, not by the circuit
        bool floating = false;
        bool sawSingular = false;

        for( const Problem& problem : out.problems() )
            sawSingular |= problem.kind() == PK_SINGULAR_MATRIX;

        for( Problem problem : topology )
        {
            if( sawSingular || ( failed && convergence ) )
            {
                problem.set_severity( PS_ERROR );
                floating = true;
            }

            *out.add_problems() = problem;
        }

        if( stopped == STOP_REASON::TIMEOUT )
        {
            out.set_status( SIM_TIMEOUT );
            Problem* problem = out.add_problems();
            problem->set_kind( PK_TIMEOUT );
            problem->set_severity( PS_ERROR );
            problem->set_message( "the run was stopped after " + std::to_string( request.timeoutMs ) + " ms" );
        }
        else if( stopped == STOP_REASON::ABORT )
        {
            out.set_status( SIM_ABORTED );
            Problem* problem = out.add_problems();
            problem->set_kind( PK_ABORTED );
            problem->set_severity( PS_ERROR );
            problem->set_message( "the run was stopped" );
        }
        else if( !failed && floating )
        {
            out.set_status( SIM_FLOATING_NODE );
        }
        else if( !failed )
        {
            out.set_status( SIM_OK );

            // It ran: what ngspice got past (a singular matrix cured by gmin stepping) is a note
            for( Problem& problem : *out.mutable_problems() )
            {
                if( problem.kind() != PK_MEASURE_FAILED )
                    problem.set_severity( PS_WARNING );
            }
        }
        else if( model )
        {
            out.set_status( SIM_MODEL_ERROR );
        }
        else if( netlist )
        {
            out.set_status( SIM_NETLIST_ERROR );
        }
        else if( floating )
        {
            out.set_status( SIM_FLOATING_NODE );
        }
        else if( singular )
        {
            out.set_status( SIM_SINGULAR_MATRIX );
        }
        else if( convergence )
        {
            out.set_status( SIM_NO_CONVERGENCE );
        }
        else
        {
            out.set_status( SIM_ERROR );
        }

        if( request.includeNetlist || out.status() == SIM_NETLIST_ERROR || out.status() == SIM_MODEL_ERROR )
            out.set_netlist( sim->GetNetlist() );

        Log( out );
        out.mutable_timings()->set_total_ms( msBetween( requestStart, steady_clock::now() ) );

        result.Swap( &out );
        complete = true;

        std::function<void( const RunSimulationResponse& )> ended = request.onEnd;

        ReleaseSimulator();

        if( ended )
            ended( result );
    }
};


SIM_API::RUNNER::RUNNER() :
        m_impl( std::make_unique<IMPL>() )
{
}


SIM_API::RUNNER::~RUNNER()
{
    if( m_impl->watchdog.joinable() )
    {
        {
            std::lock_guard<std::mutex> lock( m_impl->mutex );
            m_impl->finished = true;
        }

        m_impl->cv.notify_all();
        m_impl->watchdog.join();
    }
}


SIM_API::RUNNER& SIM_API::RUNNER::Get()
{
    static RUNNER runner;
    return runner;
}


bool SIM_API::RUNNER::Active()
{
    return Get().m_impl->active;
}


void SIM_API::RUNNER::WindowGone( REPORTER* aReporter, SIM_STATE_LISTENER* aListener )
{
    IMPL& impl = *m_impl;

    {
        std::lock_guard<std::mutex> lock( impl.mutex );

        if( impl.previousReporter == aReporter )
            impl.previousReporter = nullptr;

        if( impl.previousListener == aListener )
            impl.previousListener = nullptr;

        if( impl.forwardListener == aListener )
            impl.forwardListener = nullptr;
    }

    if( impl.console.Forward() == aReporter )
        impl.console.SetForward( nullptr );

    // Its plots went with it (the window deletes every vector when it closes without a run)
    impl.request.onEnd = nullptr;
}


bool SIM_API::RUNNER::Busy() const
{
    if( m_impl->active )
        return true;

    // The Simulator window's own run
    return m_impl->sim && m_impl->sim->IsRunning();
}


uint64_t SIM_API::RUNNER::Start( std::shared_ptr<CIRCUIT> aCircuit, const RUN_REQUEST& aRequest,
                                 const wxString& aProjectPath, std::shared_ptr<SPICE_SETTINGS> aSettings,
                                 steady_clock::time_point aRequestStart )
{
    IMPL& impl = *m_impl;

    impl.EnsureSimulator();

    impl.id++;
    impl.active = true;
    impl.complete = false;
    impl.request = aRequest;
    impl.circuit = aCircuit;
    impl.schematic = nullptr;
    impl.console.Clear();
    impl.loadLines = 0;
    impl.result.Clear();
    impl.stopReason = STOP_REASON::NONE;
    impl.itemRefs.clear();
    impl.nodeNets.clear();
    impl.nodes.clear();
    impl.devices = 0;
    impl.topology.Clear();
    impl.netlistProblems.Clear();
    impl.inWindow = static_cast<bool>( aRequest.startInWindow );
    impl.requestStart = aRequestStart;
    impl.attachStart = steady_clock::now();
    impl.attachEnd = impl.attachStart;
    impl.runStart = impl.attachStart;
    impl.runEnd = impl.attachStart;

    {
        std::lock_guard<std::mutex> lock( impl.mutex );
        impl.finished = false;
    }

    auto failNow = [&]( RunStatus aStatus, ProblemKind aKind, const std::string& aMessage )
    {
        RunSimulationResponse out;

        impl.FillCommon( out );
        out.set_status( aStatus );
        *out.mutable_problems() = impl.netlistProblems;

        if( !aMessage.empty() )
        {
            Problem* problem = out.add_problems();
            problem->set_kind( aKind );
            problem->set_severity( PS_ERROR );
            problem->set_message( aMessage );
        }

        for( const Problem& problem : impl.topology )
            *out.add_problems() = problem;

        if( impl.sim )
            out.set_netlist( impl.sim->GetNetlist() );

        impl.Log( out );
        out.mutable_timings()->set_total_ms( msBetween( impl.requestStart, steady_clock::now() ) );
        impl.result.Swap( &out );
        impl.complete = true;
        impl.ReleaseSimulator();
        return impl.id;
    };

    if( !impl.sim )
        return failNow( SIM_ERROR, PK_OTHER, "ngspice could not be loaded" );

    // The window's own run takes the simulator's mutex itself
    if( !impl.inWindow )
    {
        impl.simLock = std::unique_lock<std::mutex>( impl.sim->GetMutex(), std::try_to_lock );

        if( !impl.simLock.owns_lock() )
            return failNow( SIM_ERROR, PK_OTHER, "another simulation holds the simulator" );
    }

    {
        std::lock_guard<std::mutex> lock( impl.mutex );
        impl.previousReporter = impl.sim->GetReporter();
        impl.previousListener = impl.sim->GetSimStateListener();
    }

    impl.Version();

    if( impl.inWindow )
    {
        // The window keeps hearing its run: its console and its state listener are told as well
        std::lock_guard<std::mutex> lock( impl.mutex );
        impl.forwardListener = impl.previousListener;
        impl.console.SetForward( impl.previousReporter );
    }

    impl.sim->SetReporter( &impl.console );
    impl.sim->SetSimStateListener( &impl );

    // The plots of this API's earlier run are no longer anyone's
    for( const std::string& plot : impl.ownedPlots )
    {
        if( !plot.empty() && plot != "const" )
            impl.sim->Command( "destroy " + plot );
    }

    impl.ownedPlots.clear();
    impl.console.Clear();

    // As the Simulator window: a project that has no workbook yet simulates in LTspice/PSpice
    // compatibility; here for this run only, the project's setting is not written
    NGSPICE_SETTINGS*          ngSettings = dynamic_cast<NGSPICE_SETTINGS*>( aSettings.get() );
    NGSPICE_COMPATIBILITY_MODE storedMode = NGSPICE_COMPATIBILITY_MODE::USER_CONFIG;

    if( aSettings )
        impl.sim->Settings() = aSettings;

    if( ngSettings )
    {
        storedMode = ngSettings->GetCompatibilityMode();

        if( ngSettings->GetWorkbookFilename().IsEmpty() )
            ngSettings->SetCompatibilityMode( NGSPICE_COMPATIBILITY_MODE::LT_PSPICE );
    }

    LINE_REPORTER exporterReporter;
    bool          attached = false;
    bool          started = false;

    try
    {
        if( impl.inWindow )
        {
            // The window writes the netlist, loads it and runs, as for the person; this circuit
            // is read for the names only
            aCircuit->ReadSchematicAndLibraries( aRequest.options, exporterReporter );
            attached = true;
        }
        else if( !aRequest.netlistText.empty() )
        {
            // The host's own netlist: the schematic is read for its names only, and what its
            // exporter would say about it is not this run's
            LINE_REPORTER ignored;

            aCircuit->ReadSchematicAndLibraries( aRequest.options, ignored );
            aCircuit->MarkWritten();
            impl.sim->AttachNetlist( aCircuit, aRequest.netlistText, aRequest.options, aProjectPath );
            attached = true;
        }
        else
        {
            attached = impl.sim->Attach( aCircuit, aRequest.command, aRequest.options, aProjectPath,
                                         exporterReporter );
        }
    }
    catch( const IO_ERROR& e )
    {
        exporterReporter.Report( e.What(), RPT_SEVERITY_ERROR );
    }
    catch( const std::exception& e )
    {
        exporterReporter.Report( wxString::FromUTF8( e.what() ), RPT_SEVERITY_ERROR );
    }

    if( ngSettings )
        ngSettings->SetCompatibilityMode( storedMode );

    impl.attachEnd = steady_clock::now();
    impl.loadLines = impl.console.TextLines().size();

    ReporterToProblems( exporterReporter, &impl.netlistProblems );

    // A host's netlist is not the schematic's topology
    if( aRequest.netlistText.empty() )
        TopologyProblems( *aCircuit, &impl.topology );

    for( const SPICE_ITEM& item : aCircuit->GetItems() )
    {
        if( !item.model || !item.model->IsEnabled() )
            continue;

        impl.devices++;
        impl.itemRefs[lower( item.model->SpiceGenerator().ItemName( item ) )] = item.refName;
        impl.itemRefs.try_emplace( lower( item.refName ), item.refName );
    }

    for( const auto& [net, node] : aCircuit->NetNodes() )
    {
        impl.nodeNets.try_emplace( lower( utf8( node ) ), utf8( net ) );
        impl.nodes.insert( lower( utf8( node ) ) );
    }

    if( !attached )
    {
        bool modelError = false;

        for( Problem& problem : impl.netlistProblems )
            modelError |= problem.kind() != PK_NETLIST;

        return failNow( modelError ? SIM_MODEL_ERROR : SIM_NETLIST_ERROR, PK_NETLIST,
                        impl.netlistProblems.empty() ? "the netlist could not be written" : "" );
    }

    impl.broken = aCircuit->Broken();

    // An XSPICE element needs ngspice's code models: when this KiCad found none (its *.cm files
    // are not in the runtime) that is said, rather than ngspice's "unknown model" for the part
    if( impl.sim->CodeModelsPath().empty() )
    {
        std::string element;
        std::string text = aRequest.netlistText.empty() ? std::string() : aRequest.netlistText;

        if( text.empty() )
        {
            for( const SPICE_ITEM& item : aCircuit->GetItems() )
            {
                if( item.model && item.model->IsEnabled() )
                    text += item.model->SpiceGenerator().ItemLine( item );
            }
        }

        wxStringTokenizer textLines( wxString::FromUTF8( text ), wxS( "\r\n" ), wxTOKEN_STRTOK );

        while( textLines.HasMoreTokens() && element.empty() )
        {
            wxString line = textLines.GetNextToken().Trim( false );

            if( !line.IsEmpty() && ( line[0] == 'a' || line[0] == 'A' ) )
                element = utf8( line.BeforeFirst( ' ' ) );
        }

        if( !element.empty() )
        {
            std::string ref;

            if( auto it = impl.itemRefs.find( lower( element ) ); it != impl.itemRefs.end() )
                ref = it->second;

            RunSimulationResponse out;

            Problem* problem = impl.netlistProblems.Add();
            problem->set_kind( PK_CODE_MODELS );
            problem->set_severity( PS_ERROR );
            problem->set_reference( ref );
            problem->set_message( "ngspice's code models (*.cm) were not found by this KiCad, so the XSPICE "
                                  "element " + element + " cannot be simulated: the runtime is missing its "
                                  "ngspice code-model folder" );
            return failNow( SIM_MODEL_ERROR, PK_CODE_MODELS, "" );
        }
    }

    if( impl.devices == 0 && aRequest.netlistText.empty() )
        return failNow( SIM_NETLIST_ERROR, PK_NETLIST, "the schematic has nothing to simulate" );

    // Without node 0 every voltage is arbitrary: not run
    for( const Problem& problem : impl.topology )
    {
        if( problem.kind() == PK_NO_GROUND )
            return failNow( SIM_FLOATING_NODE, PK_NO_GROUND, "" );
    }

    if( impl.inWindow )
    {
        started = aRequest.startInWindow();
        impl.attachEnd = steady_clock::now();
    }

    impl.runStart = steady_clock::now();
    impl.deadline = impl.runStart + milliseconds( aRequest.timeoutMs );

    if( impl.inWindow && !started )
    {
        return failNow( SIM_NETLIST_ERROR, PK_NETLIST,
                        "the Simulator window could not start the run (see its message)" );
    }

    if( !impl.inWindow && !impl.sim->Run() )
    {
        impl.runEnd = steady_clock::now();

        {
            std::lock_guard<std::mutex> lock( impl.mutex );
            impl.finished = true;
        }

        impl.Finalize();
        return impl.id;
    }

    // The timeout, whether or not anyone is waiting: asked for on the main thread
    uint64_t                 run = impl.id;
    steady_clock::time_point deadline = impl.deadline;

    impl.watchdog = std::thread(
            [this, run, deadline]()
            {
                IMPL&                        watched = *m_impl;
                std::unique_lock<std::mutex> lock( watched.mutex );

                if( watched.cv.wait_until( lock, deadline,
                                           [&]()
                                           {
                                               return watched.finished;
                                           } ) )
                {
                    return;
                }

                lock.unlock();

                if( wxTheApp )
                {
                    wxTheApp->CallAfter(
                            [run]()
                            {
                                IMPL& timed = *RUNNER::Get().m_impl;

                                if( timed.active && timed.id == run )
                                    timed.StopRun( STOP_REASON::TIMEOUT );
                            } );
                }
            } );

    return impl.id;
}


bool SIM_API::RUNNER::Collect( uint64_t aId, unsigned aWaitMs, RunSimulationResponse& aOut )
{
    IMPL& impl = *m_impl;

    if( impl.id == 0 || ( aId != 0 && aId != impl.id ) )
        return false;

    if( !impl.complete )
    {
        steady_clock::time_point until = steady_clock::now() + milliseconds( aWaitMs );

        {
            std::unique_lock<std::mutex> lock( impl.mutex );

            impl.cv.wait_until( lock, std::min( until, impl.deadline ),
                                [&]()
                                {
                                    return impl.finished;
                                } );
        }

        if( !impl.Finished() && steady_clock::now() >= impl.deadline )
        {
            impl.StopRun( STOP_REASON::TIMEOUT );

            std::unique_lock<std::mutex> lock( impl.mutex );

            impl.cv.wait_for( lock, milliseconds( 3000 ),
                              [&]()
                              {
                                  return impl.finished;
                              } );
        }

        if( impl.Finished() )
            impl.Finalize();
    }

    if( impl.complete )
    {
        aOut = impl.result;
        return true;
    }

    impl.FillCommon( aOut );
    aOut.set_status( kiapi::backplane::sim::SIM_RUNNING );
    aOut.mutable_timings()->set_run_ms( msBetween( impl.runStart, steady_clock::now() ) );
    aOut.mutable_timings()->set_total_ms( msBetween( impl.requestStart, steady_clock::now() ) );
    return true;
}


bool SIM_API::RUNNER::Stop( uint64_t aId )
{
    IMPL& impl = *m_impl;

    if( !impl.active || impl.complete || ( aId != 0 && aId != impl.id ) || impl.Finished() )
        return false;

    impl.StopRun( STOP_REASON::ABORT );
    return true;
}


void SIM_API::RUNNER::Detach( SCHEMATIC* aSchematic )
{
    IMPL& impl = *m_impl;

    if( !impl.active || !impl.circuit || impl.circuit->Sch() != aSchematic )
        return;

    impl.StopRun( STOP_REASON::ABORT );

    {
        std::unique_lock<std::mutex> lock( impl.mutex );

        impl.cv.wait_for( lock, milliseconds( 5000 ),
                          [&]()
                          {
                              return impl.finished;
                          } );
    }

    if( impl.Finished() )
        impl.Finalize();
    else
        impl.ReleaseSimulator();
}
