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

#ifndef KICAD_API_SCH_SIM_H
#define KICAD_API_SCH_SIM_H

/*
 * Simulation over the API (kiapi.backplane.sim, capability sim-run; BACKPLANE_IPC.md "Simulation").
 * Shared by the handlers (api_handler_sch_sim.cpp) and the run engine (api_sch_sim_run.cpp).
 */

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <reporter.h>
#include <sim/spice_circuit_model.h>

#include <api/backplane/sim.pb.h>

class SCHEMATIC;
class KIWAY;


namespace SIM_API
{

/// A reporter that keeps every line with its severity; safe from ngspice's threads.
class LINE_REPORTER : public REPORTER
{
public:
    struct LINE
    {
        wxString text;
        SEVERITY severity;
    };

    REPORTER& Report( const wxString& aText, SEVERITY aSeverity = RPT_SEVERITY_UNDEFINED ) override;

    bool HasMessage() const override;

    bool HasMessageOfSeverity( int aSeverityMask ) const override;

    std::vector<LINE> Lines() const;

    /// Every line, split at newlines, without trailing white space
    std::vector<std::string> TextLines() const;

    size_t Count() const;

    void Clear();

    /// Also pass every line on to aReporter (the Simulator window's console); null to stop
    void SetForward( REPORTER* aReporter );

    REPORTER* Forward() const;

private:
    mutable std::mutex m_mutex;
    std::vector<LINE>  m_lines;
    REPORTER*          m_forward = nullptr;
};


/// KiCad's circuit model (its SPICE netlist exporter), keeping what the API reports beside the
/// netlist: the net -> node map, and this run's extra directives.
class CIRCUIT : public SPICE_CIRCUIT_MODEL
{
public:
    CIRCUIT( SCHEMATIC* aSchematic, KIWAY* aKiway ) :
            SPICE_CIRCUIT_MODEL( aSchematic, aKiway )
    {
    }

    SCHEMATIC* Sch() const { return m_schematic; }

    /// The analysis written as text on the sheets, read without a netlist (the exporter knows the
    /// sheets only once it has written one)
    wxString SheetCommand();

    /// KiCad's net name -> the ngspice node, of the last netlist
    const std::map<wxString, wxString>& NetNodes() const { return m_netNodes; }

    /// When the last netlist was fully written (the simulator reads it after)
    std::chrono::steady_clock::time_point WrittenAt() const { return m_writtenAt; }

    bool ReadSchematicAndLibraries( unsigned aNetlistOptions, REPORTER& aReporter ) override;

    void WriteTail( OUTPUTFORMATTER& aFormatter, unsigned aNetlistOptions ) override;

protected:
    wxString GenerateItemPinNetName( const wxString& aNetName, int& aNcCounter ) const override;

private:
    mutable std::map<wxString, wxString>  m_netNodes;
    std::chrono::steady_clock::time_point m_writtenAt;
};


unsigned OptionsFromProto( const kiapi::backplane::sim::Options& aOptions );

void OptionsToProto( unsigned aOptions, kiapi::backplane::sim::Options* aOut );

/// "OP", "TRAN" ... "Custom"
std::string AnalysisName( const wxString& aCommand );

/// The exporter's messages as problems (kind PK_NETLIST unless the text names something better)
void ReporterToProblems( const LINE_REPORTER& aReporter,
                         google::protobuf::RepeatedPtrField<kiapi::backplane::sim::Problem>* aOut );

/// Problems of the symbols that would not simulate (a missing model, an unreadable library ...)
void ModelProblems( SCHEMATIC* aSchematic, google::protobuf::RepeatedPtrField<kiapi::backplane::sim::Problem>* aOut );

/// Nodes with no DC path to node 0 (PK_FLOATING_NODE) and a circuit without node 0 (PK_NO_GROUND),
/// from the netlist's items.
void TopologyProblems( const CIRCUIT& aCircuit,
                       google::protobuf::RepeatedPtrField<kiapi::backplane::sim::Problem>* aOut );

/// Apply the request's overrides to the circuit; false and aError when one is malformed
bool SetOverrides( CIRCUIT& aCircuit,
                   const google::protobuf::RepeatedPtrField<kiapi::backplane::sim::Override>& aOverrides,
                   std::string& aError );

/// The saved setup's first analysis (the workbook's first tab), or the analysis written on the
/// schematic; empty when there is neither.  aOptions is set when a tab gives them.
wxString SavedCommand( SCHEMATIC* aSchematic, KIWAY* aKiway, unsigned* aOptions );


/// What a run needs from its request and its netlist once the schematic is out of the picture.
struct RUN_REQUEST
{
    wxString                 command;       ///< the analysis line(s) in the netlist
    std::vector<std::string> postCommands;  ///< ngspice commands after the run (linearize, fft)
    std::vector<std::string> signals;
    std::vector<std::string> measures;
    bool                     includeNetlist = false;
    bool                     show = false;
    unsigned                 timeoutMs = 120000;
    unsigned                 maxPoints = 0;
    unsigned                 options = 0;

    /// RunSimulation.show with a Simulator window: the run is that window's own.  Starts it
    /// (its tab, its circuit, its ngspice run); false when it did not start.
    std::function<bool()> startInWindow;

    /// Called on the main thread when the run has ended and its result is read (the window plots
    /// the signals, or takes its own circuit back)
    std::function<void( const kiapi::backplane::sim::RunSimulationResponse& )> onEnd;
};


/**
 * The one simulation an API client can have running in this process (ngspice is one instance).
 * Everything here runs on the main thread except the two ngspice callbacks, which only set flags.
 */
class RUNNER
{
public:
    static RUNNER& Get();

    /// True while a run started here has not been collected as finished
    bool Busy() const;

    /// True while an API run holds the simulator (the Simulator window keeps its hands off)
    static bool Active();

    /// The Simulator window is going away: its console reporter and state listener are no
    /// longer given anything, nor given back the simulator when a run ends
    void WindowGone( REPORTER* aReporter, SIM_STATE_LISTENER* aListener );

    /// Start: attach the circuit (netlist + ngspice load) and run in ngspice's background thread.
    /// The response is complete at once when the netlist or the load fails.
    /// @return the run's id
    uint64_t Start( std::shared_ptr<CIRCUIT> aCircuit, const RUN_REQUEST& aRequest, const wxString& aProjectPath,
                    std::shared_ptr<SPICE_SETTINGS> aSettings,
                    std::chrono::steady_clock::time_point aRequestStart );

    /// Wait up to aWaitMs for run aId (0 = the latest) and answer it: whole when it has ended,
    /// SIM_RUNNING otherwise.  False when there is no such run.
    bool Collect( uint64_t aId, unsigned aWaitMs, kiapi::backplane::sim::RunSimulationResponse& aOut );

    /// Stop the running simulation (aId 0 = whichever).  False when none runs.
    bool Stop( uint64_t aId );

    /// Forget a circuit's schematic: called before the schematic goes away
    void Detach( SCHEMATIC* aSchematic );

    struct IMPL;

private:
    RUNNER();
    ~RUNNER();

    std::unique_ptr<IMPL> m_impl;
};

} // namespace SIM_API

#endif // KICAD_API_SCH_SIM_H
