/*
 * This program source code file is part of KiCad, a free EDA application.
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

#pragma once

#include <array>

namespace BACKPLANE
{

// Deduced from the list itself (no hand-kept size): a platform-only entry sits inside it under its
// #ifdef, and adding one cannot leave the size behind
inline constexpr auto CAPABILITIES = std::to_array<const char*>( {
    "api-server",
    "headless-pcb",
    "headless-sch",
    "export-v10",
    "parent-pid",
    "capabilities",
    "sketch-route",
    "board-structure",
    "assembly",
    "idle-exit",
    "shutdown",
    "api-descriptor",
    "symbol-from-libid",
    "footprint-partial-update",
    "coedit",          // docs/backplane/coedit.md: live co-editing, per-actor undo, deferral
    "rules-merge",     // project settings and custom rules merge, never overwrite
    "routing-plan",    // docs/backplane/floorplanner.md: kiapi.backplane.plan
    "comment-key-63",  // embedded: the comment action sends KEY_ATOM index 63; text fields keep ';'
    "drc-zone-guard",  // DRC checks a zone against its outline when its fill triangulation is bad
    "rules-insert-position", // EditCustomRules.insert: new rules first, last or before a named rule
    "plan-keys",       // in plan view the plan tool's keys (A, R, W, L...) are its own, never KiCad's defaults
    "runaction-frame", // RunAction.frame names the editor; with none, shared actions go to the PCB editor
    "api-handler-order", // a request several handlers know goes to them in a fixed order: PCB, SCH, footprint, symbol editors, then the rest
    "task-gate-reads", // during an API task only KICAD_API_SERVER::IsReadServedDuringTask's reads are
                       // answered (served while it runs); every other request is AS_BUSY at once, naming the task
    "no-load-migration", // embedded: opening a document changes nothing (no load-time 3D migration;
                         // KiCad's own load fixes are actor "kicad"); SaveDocument of an unmodified
                         // document in an open editor writes nothing
    "api-field-fixes", // a FootprintInstance field sent with only its text is a partial update; CreateItems of
                       // a library symbol places text-only fields as the library does and honours
                       // fields_autoplaced, flattens a derived symbol, and a user field named Footprint,
                       // Reference, Value, Datasheet or Description fills that mandatory field
    "sketch-route-width", // SketchRoute takes each net's class width (or track_width), lists failures[]
                          // with reasons, and answers a message with no board AS_BAD_REQUEST
    "plan-view-api",   // plan view is a passive mode: the API is served in it (RunAction, togglePlanView,
                       // SetActiveLayer...), AS_BUSY only during a plan drag or a corridor/plane being
                       // drawn; over the board view the plan overlay draws a "view only" tag, not plan
                       // view's header; GetPlan's PlanResponse.in_plan_view says whether plan view is on
    "plan-select",     // kiapi.backplane.plan.SelectPlanItems selects plan items as a click does (entering
                       // plan view, the first item's layer active, zoom); embed SelectItems in plan view
                       // zooms to the board items without selecting them and keeps plan view on
    "global-tables-seeded", // a config home with no global library table gets the runtime's default
                            // (<stock data>/<table>) at first start, never over an existing one;
                            // ${BACKPLANE_KICAD_LIBRARY} defaults to <stock data>/backplane-library
    // Runtime traits the host keys an update on before installing (in the manifest on purpose:
    // they describe the runtime, not a session; GetFrames advertises them too):
    "embed-autopanels",  // embedded side panels follow the frame's width (EmbedAutoPanels)
    "embed-gl-fallback", // no modal box for the OpenGL fallback: an ET_NOTICE of kind "graphics"
#ifdef _WIN32
    // embedded: a prewarm never takes the keyboard or the activation (shown with SW_SHOWNA; a focus
    // it takes while built goes back).  Windows only: verified there; the host gates its
    // self-started prewarm on it
    "prewarm-keeps-focus",
#endif
    "sim-run",         // kiapi.backplane.sim (BACKPLANE_IPC.md "Simulation"): GetSimulationModels,
                       // AssignSimulationModel, GetSimulationNetlist, Get/SetSimulationSetup, RunSimulation,
                       // GetSimulationResult, StopSimulation; in the editor and headless (bp.13.2)
    "sim-run-points-cap",  // RunSimulation.max_run_points: a run is stopped when ngspice has produced that
                       // many values (points x saved vectors; left out: 5000000) and answered
                       // SIM_POINT_LIMIT / PK_POINT_LIMIT with the points up to there; the person's
                       // own runs in the Simulator window have no limit (bp.13.2)
    "sim-thin-minmax", // RunSimulation.max_points keeps each stretch's lowest and highest point of
                       // every returned vector (RunSimulation.thinning, answered in
                       // RunSimulationResponse.thinning), and a short `signals` list is all ngspice
                       // saves (RunSimulationResponse.saved_requested_only) (bp.13.2)
    "sim-frame",       // embedded: FK_SIMULATOR = 8, KiCad's Simulator window as an embedded frame on the
                       // open schematic, opened on demand (BACKPLANE_EMBED.md "Simulator"; bp.13.2)
    "headless-no-lock", // kicad-cli api-server takes no project lock on OpenDocument and removes none;
                       // another program's lock is reported in OpenDocumentResponse.foreign_lock
                       // {lock_file, user, host, held} and the open succeeds without a prompt (bp.13.2)
} );

} // namespace BACKPLANE
