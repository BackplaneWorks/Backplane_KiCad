# Backplane IPC guide

This fork is upstream KiCad master (pinned in `BACKPLANE_UPSTREAM`) plus a small Backplane patch
set (`PATCHES.md`). It speaks upstream KiCad's protobuf IPC API unchanged, and adds fork-only
commands in the proto package `kiapi.backplane`. Starting, supervising and describing the
headless server is covered in `BACKPLANE_RUNTIME.md`.

## Connect

- Transport: nng request/reply over `ipc://<socket path>`. Each request is an
  `kiapi.common.ApiRequest` envelope (`api/proto/common/envelope.proto`) whose `message` is a
  packed command; the reply is an `ApiResponse` with a status and a packed response.
- Set `header.client_name` to a stable reverse-DNS name. Copy `header.kicad_token` from the first
  response into later requests.
- Generate bindings from this checkout's `api/proto`, not from a released KiCad:
  `protoc -I api/proto --python_out=<dir> $(find api/proto -name '*.proto')`.
  `scripts/backplane_ipc_test_support.py` is a small, complete Python client to copy from.

```python
import pynng
from common import envelope_pb2
from common.commands import base_commands_pb2

with pynng.Req0(dial="ipc:///run/user/1000/backplane/kicad/<hash>/api.sock") as socket:
    request = envelope_pb2.ApiRequest()
    request.header.client_name = "works.backplane.example"
    request.message.Pack(base_commands_pb2.GetVersion())
    socket.send(request.SerializeToString())
    reply = envelope_pb2.ApiResponse.FromString(socket.recv())
```

## Documents and edits

- One server owns one project, with at most one schematic and one board open. Use one server per
  project.
- `OpenDocument`, `CloseDocument`, `CloseAllDocuments`, `SaveDocument`, `SaveDocumentAs`,
  `SaveCopyOfDocument` (project commands) manage documents; dirty documents must be saved or
  closed with `force`.
- Edit inside a commit: `BeginCommit`, then `CreateItems` / `UpdateItems` / `DeleteItems`, then
  `EndCommit` with `CMA_COMMIT` or `CMA_DROP`. Read with `GetItems`, `GetItemsById`.
  A document counts as modified only once a commit ends with `CMA_COMMIT`, and `CloseDocument`
  does not wait for open commits: end yours first.
  `CloseAllDocuments` without `force` (and closing the project document) refuses while a document has
  unsaved changes; save first or pass `force: true`.
- A PCB footprint update may contain only the footprint ID, position, and layer. If its
  `FootprintInstance` omits `definition`, the existing footprint body is kept; an explicitly
  present empty `definition` replaces the body with an empty definition.
- Schematic: all item types are serialized (wires, junctions, labels, symbols and pins, sheets,
  tables, rule areas, groups ...); `GetSchematicHierarchy`, `GetSchematicNetlist`.
- Board: nets and connectivity, design rules (`board_rules.proto`), stackup (read only),
  layers, plot settings, embedded files, zones, `RunBoardJob*` exports.
- Headless limits: upstream keeps editor state only in the GUI. A headless server returns
  `AS_UNIMPLEMENTED` ("... is not available in headless mode") for selection (`GetSelection`,
  `AddToSelection`, `RemoveFromSelection`, `ClearSelection`, `SaveSelectionToString`,
  `SyncSelection`, `FocusOnItems`), view state (`Get/SetVisibleLayers`, `Get/SetActiveLayer`,
  `Get/SetBoardEditorAppearanceSettings`), `HighlightNets`, `RunAction`, `RevertDocument`,
  `OpenLibraryItem` and `InteractiveMoveItems`. Name items by ID instead of selecting them; to
  undo unsaved work, drop the commit or close the document with `force` and reopen it.
- Schematic symbols can be created from a library ID: send `CreateItems` with a
  `SchematicSymbolInstance` whose `definition.id` identifies the library symbol, even when
  `definition.items` is empty. The server loads the requested library synchronously, expands the
  definition, and keeps explicit instance placement, orientation, unit, and field values. If the
  library symbol cannot be found, that item receives `ISC_INVALID_DATA` and no hollow symbol is
  added.
- `PlaceSymbolFromLibrary` also loads its requested symbol library on demand, so it succeeds
  immediately in headless mode without waiting for `LoadAllLibraries`.
- ERC and DRC are not IPC commands: use `kicad-cli sch erc` and `kicad-cli pcb drc`.
- From bp.13, `kicad-cli pcb drc` lists violations in a canonical order (each provider's
  violations sorted by error code, position, layer, item ids, message and rule), the same at every
  thread count. Its byte order therefore differs from bp.12 and earlier, whose order changed from
  run to run; compare reports by content, not by position. Where a code reaches its error limit,
  the report keeps the first N in that order. See `BACKPLANE_DRC.md`.
- From bp.13, headless runs (`kicad-cli`, the API server) use at most 8 pool threads unless the
  `MaximumThreads` advanced setting says otherwise.

Full command list and what master lacks: `bp-coord/kicad-fork/IPC-MAP.md` (coordination repo),
or read `api/proto/` directly.

## Fork commands (`kiapi.backplane`)

| Command | Proto | Notes |
|---|---|---|
| `GetCapabilities` | `backplane/capabilities.proto` | Fork version, upstream commit, API level, capability names, every registered command, native file versions. |
| `SketchRoute` | `backplane/sketch_route.proto` | Routes a bundle of connections (item IDs, optional guide polyline, layer, corner mode, vias, time limit); new items join the open commit. Works headless with explicit item IDs; an empty list means "the selection" and only works in the GUI. |
| `plan.SelectPlanItems` | `backplane/plan.proto` | Capability `plan-select`. Selects plan items (corridors, planes, notes, splits) in the plan tool as a click does, so W/A/R and the context menu act on them; makes the first item's layer active, zooms when `zoom`, enters plan view when it is off. Unknown ids come back in `missing`; none known is `AS_BAD_REQUEST`. `AS_BUSY` only during a plan drag or draw. GUI only. With `plan-select`, embed `SelectItems` in plan view only zooms to the board items (no selection or highlight) and plan view stays on. |

Board-structure (rigid-flex) and assembly commands are added by the phase-2 port (PR #6) in
`backplane/structure.proto` and `backplane/assembly.proto`; check `GetCapabilities` for
`board-structure` and `assembly`.

The GUI started with `kicad --embedded` adds `kiapi.backplane.embed` (`backplane/embed.proto`:
`AttachToParent`, `ShowFrame`, `HideFrame`, `GetFrames`, `NextEvents`, `SetChrome`,
`SetReservedKeys`, `Close`) and the `embed` capability; see `BACKPLANE_EMBED.md`. In every GUI
process, a request that changes state while a modal dialog is open answers `AS_BUSY` with the
dialog's title; reads still work.

Gate features on capability or command names, not on version numbers.

Editor readiness: after a `ShowFrame`, `ET_FRAME_DRAWN` (capability `embed-frame-drawn`) says the
frame's canvas finished its first redraw on screen; wait on it, with a cap (the 3D viewer has no
drawing canvas and never posts it), instead of a fixed delay.

The other bp.13 embed features have one capability each in the embedded GUI's `GetCapabilities`
(never in the headless manifest, which describes `kicad-cli api-server`):

| Capability | What | How |
|---|---|---|
| `embed-editor-requested` | KiCad opened a symbol or footprint editor itself | `ET_EDITOR_REQUESTED`; `FrameInfo.lib_id` |
| `embed-gerbview` | GerbView in KiCad's own process | `OpenGerbers{files, show, x, y, width, height}` opens and loads; `ShowFrame{kind: FK_GERBVIEW}` shows it again; `FrameInfo.files` lists what is loaded |
| `embed-checks` | DRC and ERC in the open editor | `RunChecks`, `GetMarkers`, `SelectItems` by id or place, `ET_CHECKS_DONE` |
| `embed-host-markers` | the host's own markers on the canvas | `SetHostMarkers`, `ET_MARKER_CLICKED` |
| `embed-request-actor` | who caused a show request | `Event.detail` of `ET_SHOW_REQUESTED` and `ET_EDITOR_REQUESTED` is the API client whose request made KiCad ask (an agent's or the host's own `RunAction`, ...), empty for the person and KiCad on its own; without the capability it is always empty. DRC parity loads the schematic hidden and asks for nothing |
| `embed-notice` | no message box over an embedded frame | `ET_NOTICE` (once per kind and title), detail `{kind, title, detail, error}`, also on stderr. Kind `graphics`: the drawing fallbacks ("Could not use OpenGL, falling back to software rendering", a driver reset); KiCad switches to the fallback canvas itself and every later frame starts on it. Kind `file-watch-unavailable`: a library file watcher could not start (inotify instances used up); that frame does not reload the library on an outside change |
| `api-field-fixes` | API field handling (bp.13) | a `FootprintInstance` mandatory field sent with only its text changes only the text (layer, place, size, visibility kept); `CreateItems` of a library symbol places a field given without a position as the library places it (text kept) and autoplaces when `fields_autoplaced`; a derived symbol is written flattened; a user field named Footprint, Reference, Value, Datasheet or Description fills that mandatory field (create and update) |
| `sketch-route-width` | SketchRoute widths and failures (bp.13) | routes at each net's class width (net classes set in the same server included) or `track_width`; `SketchRouteResponse.failures` names each unrouted connection (net, ends, reason); a message with no `board` is `AS_BAD_REQUEST` |
| `plan-view-api` | The API in plan view (bp.13) | plan view (the plan tool's edit mode) is passive like the selection tool: `RunAction` (including `pcbnew.Plan.togglePlanView`), `SetActiveLayer` and the other gated requests are served in it; `AS_BUSY` only while plan items are dragged or a corridor or plane is being drawn. Without it, every gated request in plan view is `AS_BUSY`. Over the board view the plan overlay draws a small "view only" tag, never plan view's header. `GetPlan` answers `PlanResponse.in_plan_view`: true while plan view is on, false in the board view (plan overlay shown or not) and headless |
| `embed-macos` | embedding on macOS | frames are borderless top-level windows placed in screen points over the host's pane and ordered just above the host window (`--parent-window` is the host's CGWindowID); KiCad runs as an accessory app. The host sends `SetHostGeometry` and reads `ET_RESERVED_KEY`. Every shown frame fills the `SetHostGeometry` rect: `ShowFrame`'s `x`/`y`/`width`/`height` are ignored for placement there, and no frame is on screen before the first rect |
| `embed-host-geometry` | `SetHostGeometry` | `{x, y, width, height, visible, raise}`: the container's rect in screen points; `visible` false orders every frame out without changing `shown`/`realized`; `raise` orders the shown frames above the host. Also served on the events socket |
| `embed-reserved-key-event` | reserved chords as events | `ET_RESERVED_KEY` with `Event.reserved_key {index, key_code, modifiers, window, chord}`; index 63 is the comment key. On macOS `Ctrl` in `SetReservedKeys` is Cmd |
| `embed-raw-ctrl` | the Control key in reserved chords (macOS) | `SetReservedKeys` accepts `RawCtrl+` for the physical Control key (`Ctrl+`/`Cmd+` is Cmd on macOS), e.g. `RawCtrl+Tab`, `RawCtrl+Shift+Tab`, ``RawCtrl+` ``, which macOS keeps for itself as Cmd chords. Elsewhere `RawCtrl` is the same as `Ctrl` |
| `canvas-idle` | the person's input on each canvas | `FrameInfo.canvas_input_ago_ms` (ms since the last button, drag, wheel or key on that frame's canvas; max uint64 for none; hovering does not count), `tool_busy` (an interactive tool mid-operation: a move, a track started, a shape after its first point), `tool_name`. `RunChecks.min_canvas_idle_ms`: when non-zero, refused at once with `AS_BUSY` "canvas in use: <frame>, input N ms ago / tool T busy" while the checked frame's canvas or any other embedded frame's canvas had input within it (one UI thread: a schematic pan holds the board's DRC back, bp.13 batch 11), or the checked frame's tool is busy (DRC with parity: also a busy schematic tool). KiCad never waits or queues: it refuses or runs |
| `click-marker` | a marker click by id | `ClickMarker{kind, marker \| host{set,id}, zoom}` does what a left click on that marker does, by the same code: a DRC/ERC marker becomes the selection (message panel, the DRC/ERC dialog's row) and one `ET_MARKER_CLICKED` is posted with the fields a real click gives; reply `event_seq`. No view change unless `zoom`; an ERC marker on another sheet shows that sheet. Unknown id: `AS_BAD_REQUEST` "ClickMarker: no marker ..."; frame not open: `AS_UNHANDLED`; during a task: `AS_BUSY` |
| `runaction-frame` | which editor runs a `RunAction` | `RunAction.frame` (field 100) names it (`FT_PCB_EDITOR`, `FT_SCHEMATIC_EDITOR`); a named frame that cannot run it is refused at once with `AS_BAD_REQUEST` and the error message `RunAction: <FrameType> is not open`, `... does not accept actions` (only the PCB and schematic editors take actions) or `... has no action <name>`. With no frame, an `eeschema.*` action goes to the schematic editor and any other (`pcbnew.*`, and shared ones such as `common.Interactive.undo`) to the PCB editor while one is open, else to the schematic editor. Before it, with both editors open, a shared action could reach either |
| `embed-popup-keys` | reserved chords get through KiCad's popups (Linux/X11) | while a KiCad popup (a menu such as Clarify Selection, a combo box's list) holds the keyboard, a chord from `SetReservedKeys` closes it and reaches the host as `_BACKPLANE_KEY`, as it does from a frame; other keys stay with the popup |

Number ledger for `kiapi.backplane.embed` (fork-only, additive; take the next free number and add
it here in the same change):

| `EventType` | Name | Since |
|---|---|---|
| 1-24 | frames, dialogs, documents, theme, comments, co-editing, plan, progress | bp.8-bp.12 |
| 25 | `ET_EDITOR_REQUESTED` (editor tabs) | bp.13 |
| 26 | `ET_MARKER_CLICKED` (host and DRC/ERC marker clicks) | bp.13 |
| 27 | `ET_CHECKS_DONE` (RunChecks) | bp.13 |
| 28 | `ET_FRAME_DRAWN` (cold start) | bp.13 |
| 29 | `ET_NOTICE` (a message box embedded KiCad would show, sent to the host instead) | bp.13 |
| 30 | `ET_RESERVED_KEY` (macOS reserved chords and comment key) | bp.13 |

Other bp.13 numbers: `FrameKind` 7 `FK_GERBVIEW`; `FrameInfo` 12 `lib_id`, 13 `files`; `Event` 8
`marker`, 9 `checks`, 10 `drc_marker`, 11 `reserved_key` (bp.13); messages `ReservedKey`,
`SetHostGeometry` (macOS, bp.13).

## Simulation (`kiapi.backplane.sim`, capability `sim-run`, bp.13.2)

`backplane/sim.proto`. KiCad's own simulator behind eight commands: its model code (`SIM_LIB_MGR`,
`SIM_MODEL` and its serializer), its SPICE netlist exporter and its ngspice instance. Served by the
schematic editor (embedded or not) and by `kicad-cli api-server` with the same answers; `document`
is the open schematic. A net called `GND` (or `0`) is ngspice's node 0.

| Command | What |
|---|---|
| `GetSimulationModels {document, references, include_params, only_problems}` | Per symbol (a multi-unit part once; power symbols never): reference, value, `lib_id`, `exclude_from_sim`, `dnp`, the `Sim.*` fields as stored, `source` (`MS_INFERRED` from the Value of an R, C, L, V or I; `MS_BUILTIN`; `MS_LIBRARY`; `MS_RAW`; `MS_NONE`), `device`, `type`, the SPICE item letter, the model name, the library as stored and resolved, every pin with its model node and its net, the netlist line, and `error_kind` + `error` when it would not simulate (`ME_NO_MODEL`, `ME_LIBRARY_NOT_FOUND`, `ME_MODEL_NOT_IN_LIBRARY`, `ME_PIN_MISMATCH`, `ME_BAD_PARAMS`, `ME_OTHER`). `symbols`, `excluded`, `problems` count the whole design. Changes nothing. |
| `AssignSimulationModel {document, references, builtin \| library \| raw \| clear, pins, exclude_from_sim, partial}` | Writes the `Sim.*` fields as the Simulation Model dialog does (the model is built by KiCad and written by its serializer): a built-in device with parameters (`device` "R", "V", "NPN"..., `type` "", "SIN", "PULSE"...; a device with a principal value keeps it in the Value field, as KiCad does), a `.model` or `.subckt` of a library file (stored relative to the project when inside it), raw SPICE, or `clear`. `pins` maps symbol pins to model nodes (by name, or 1-based position); none = by pin order, which the answer reports. `exclude_from_sim` sets the symbol attribute, alone or with a model. One commit and one undo step; every unit of a part gets the fields. Nothing is written when any reference fails, unless `partial`. Each result carries the symbol as it now simulates. |
| `GetSimulationNetlist {document, command, options, directives, overrides}` | The netlist ngspice would get, the net to node map (`V(<node>)` is the voltage vector), each pin current's vector and KiCad signal name, the sheet's directives, and `problems`: the symbols without a usable model, nodes with no DC path to ground (`PK_FLOATING_NODE`), a circuit without node 0 (`PK_NO_GROUND`). Changes nothing. |
| `GetSimulationSetup {document}` | The Simulator's workbook (`<project>.wbk` unless the project names another): each tab's analysis, command, options, traces and measurements; the user-defined signals; the analysis written as text on a sheet; the other sheet directives; the project's ngspice compatibility mode. Changes nothing. |
| `SetSimulationSetup {document, command, options, signals, measurements, tab, remove, compatibility}` | Writes a tab of that workbook (format 8, what the Simulator loads) and names the workbook in the project file. Without `tab` the tab of the same analysis type is replaced, else one is appended; a trace keeps the colour and cursors the person gave it. Never touches the schematic. |
| `RunSimulation {document, command, directives, overrides, signals, measures, include_netlist, show, timeout_ms, options, wait_ms, max_points}` | Runs. See below. |
| `GetSimulationResult {document, run_id, wait_ms}` | Collects a run answered `SIM_RUNNING`. The last result stays until the next `RunSimulation`. |
| `StopSimulation {document, run_id}` | Stops the run; its result is `SIM_ABORTED` with the points ngspice had. |

**The run.** `command` is the analysis line(s): `.op`, `.tran`, `.ac`, `.dc`, `.noise`, `.pz`,
`.tf`, `.sp`; a frequency content analysis is a `.tran` line followed by `linearize`/`fft` lines,
as the Simulator's FFT tab does. Empty = the saved set-up's first tab, else the analysis written
on a sheet. `directives` are netlist lines for this run. `overrides {reference, field, value}`
change this run's netlist only (`field` empty or `Value`: the principal value; a `Sim.Params` key;
or a whole `Sim.<Name>` field): sweeps and tuning never write the schematic. `signals` choose the
vectors that come back with data: `V(/out)`, `I(R1)`, an ngspice vector, or a net name; `*` = all;
none = names and lengths only. `measures` are ngspice `meas` statements run after the analysis
(the analysis word may be left out).

The API socket serves one request at a time, so the request does not wait for a long run: it
waits `wait_ms` (default 250; at most 1000 in a GUI process, where the UI thread waits with it) and
then answers `status = SIM_RUNNING` with `run_id`; `GetSimulationResult` collects it and
`StopSimulation` ends it. ngspice runs in its own thread, as for the Simulator window: the editor
and every other request stay served. One run at a time in a KiCad process (the Simulator window's
runs included): a second `RunSimulation` is `AS_BUSY`. `timeout_ms` (default 120000) stops the run
by itself: `SIM_TIMEOUT`, with the points so far.

The answer: `status`; `problems[] {kind, severity, reference, node, net, message}`; `log`
(ngspice's console, the last 200 lines); `vectors[] {name, signal, kind, length, complex, real[],
imag[], plot}` with the sweep axis first (`max_points` thins every vector alike, the first and
last point kept); `values[]` for every single number (all of `.op`, the three results of `.tf`,
the noise totals); `measures[] {name, value, failed, error}`; `netlist` when asked or refused;
`timings {netlist_ms, load_ms, run_ms, total_ms}`; `ngspice_version`, `nodes`, `devices`, `points`,
`analysis`, `command`, `run_id`.

| `status` | When | `problems` name |
|---|---|---|
| `SIM_MODEL_ERROR` | a symbol has no usable model: found by KiCad's model code before ngspice sees a netlist, or ngspice knows no such model or subcircuit | the symbol: `PK_MISSING_MODEL`, `PK_LIBRARY_NOT_FOUND`, `PK_MODEL_NOT_IN_LIBRARY`, `PK_PIN_MISMATCH`, `PK_BAD_PARAMS` |
| `SIM_NETLIST_ERROR` | the netlist could not be written or ngspice refused it (an override naming no parameter, a syntax error) | `PK_NETLIST` with the line |
| `SIM_NO_CONVERGENCE` | the operating point or a time step did not converge | `PK_NO_CONVERGENCE` with the node or device ngspice had trouble with |
| `SIM_FLOATING_NODE` | a node has no DC path to ground and the matrix was singular, or there is no node 0 (then nothing is run). When ngspice still got numbers by its fallbacks they come back, but they are gmin's, not the circuit's | `PK_FLOATING_NODE` with the node, its net and what is connected to it; `PK_NO_GROUND` |
| `SIM_SINGULAR_MATRIX` | a singular matrix with every node grounded (a loop of voltage sources or inductors) | `PK_SINGULAR_MATRIX` with the node or the device whose branch it is |
| `SIM_TIMEOUT`, `SIM_ABORTED` | stopped | `PK_TIMEOUT`, `PK_ABORTED` |
| `SIM_ERROR` | anything else ngspice called an error | `PK_OTHER` |

With `SIM_OK`, `problems` holds warnings only: a measure that gave no value
(`PK_MEASURE_FAILED`), an asked signal with no vector, a floating node that did not stop this
analysis.

Sources (VDC, VSIN, VPULSE, IDC...) are ordinary symbols of KiCad's `Simulation_SPICE` library.
A project that has no workbook yet simulates in LTspice/PSpice compatibility, as in the Simulator
window; the project's setting is not written by a run.

## Files

- Native files use upstream master's format (newer than KiCad 10.0.x). Custom properties and
  line endings are native tokens.
- Fork-only data lives beside the design in `<file>.backplane.json` (e.g. the board structure) and
  `*.backplane-assembly.json`. Keep them with the design. Opening a project written by the 10.0.6
  fork migrates its v1 companion: per-item properties and line endings move into native tokens on
  the next save, and the companion keeps only fork-only extensions.
- For stock KiCad 10, export a copy (outside the project folder, into a new or empty folder):
  `kicad-cli project downgrade --target 10.0 -o <dir> <project>.kicad_pro --report-json <file>`.
  Companions are copied and reported; stock KiCad ignores them.

## Libraries

Every runtime (Linux, Windows, macOS) carries the same pinned libraries, staged by
`scripts/backplane-libraries.sh`: KiCad's symbols, footprints, 3D models and templates at tag
10.0.6 and the Backplane KiCad library (footprints and their STEP models), in the stock data folder
(`share/kicad`; on macOS `KiCad.app/Contents/SharedSupport`): `symbols/`, `footprints/`,
`3dmodels/` (`${KICAD10_3DMODEL_DIR}`'s default, no launcher needed), `template/` (with the libraries' `sym-lib-table` and `fp-lib-table`),
`backplane-library/`, and the default global `sym-lib-table`, `fp-lib-table` and
`design-block-lib-table`, which nest `${KICAD10_TEMPLATE_DIR}/<table>`; `fp-lib-table` also names
the Backplane library's six footprint libraries through `${BACKPLANE_KICAD_LIBRARY}/libraries/`.
`licenses/kicad-libraries/<name>/` holds each library's notices and `BACKPLANE_SOURCE.txt`;
`KICAD_LIBRARY_SOURCES.txt` lists them all.

`manifest.json` pins the Backplane library:
`"backplaneLibrary": {"repository", "version", "commit", "sha256"}`. `commit` is the pin (the
library is fetched by it); `version` is the bundle's label (e.g. `2026.11.13`), not a git ref;
`sha256` is over `<stock data>/backplane-library/SHA256SUMS`, which lists every file of the
library with its sha256 (hash that file, then `sha256sum -c SHA256SUMS` in the folder).

`global-tables-seeded` (headless manifest and `GetCapabilities`): at its first start, KiCad copies
`<stock data>/<table>` into the person's settings folder for this version
(`<config>/10.99/<table>`) for each global table that file does not exist for, and logs
`Backplane: seeded global <table> from <default>` on stderr. A table that exists (readable or not,
valid or not) is never touched, nor is any other version's folder; adding entries to an existing
table stays the host's job. `${BACKPLANE_KICAD_LIBRARY}` defaults to
`<stock data>/backplane-library` when neither the environment nor `kicad_common.json` sets it (a
default is never saved). Without the capability only the Linux launcher seeds and sets it.

## Changes from the 10.0.6-based runtime

- `RuleCheckerMarker*` → upstream `DrcMarker` / `ErcMarker`; design-rule types are in
  `board/board_rules.proto`, and DRC severities are keyed by `DrcErrorType`.
- The old `kiapi.schematic.types.GetSchematicHierarchy/Netlist` type URLs are no longer accepted;
  use `kiapi.schematic.commands.*`.
- `SketchRoute` moved from `kiapi.board.commands` to `kiapi.backplane`.
- Headless selection, view layers and `RevertDocument` worked in the 10.0.6-based runtime; upstream
  master refuses them headless (see "Headless limits").
- Native files are no longer readable by stock KiCad 10.0.x; use the export above.
- The SWIG `pcbnew` Python module no longer exists upstream; use IPC or `kicad-cli`.

## Known issues

- The design-block pane's preview (PCB and schematic editors) loads its colour theme once, when it
  is created: after `SetTheme` (or the person changing theme) it keeps the old background and
  item colours until the editor is reopened.  The editors' own canvases follow the change.

## Tests

`scripts/backplane-sim-headless-check.py <kicad-cli>` (sim-run) generates small schematics and
checks every analysis against its closed form (a divider's `.op`, `.dc` and `.tf`; an RC
low-pass's `.ac` corner, `.tran` step, kT/C `.noise` and `.pz` pole; a two-port's `.sp`; an FFT
line), the run controls, each failure kind with its reference or node, the model commands and the
workbook.

`scripts/backplane-*-regression.py <kicad-cli>` run headless against an isolated server and never
touch user projects; CI runs all of them on every build. `--stock-cli <stock 10.0.x kicad-cli>`
(where supported) also checks that KiCad 10 exports open in stock KiCad.

## Source and licensing

Upstream KiCad master at the commit in `BACKPLANE_UPSTREAM`, plus the patches in `PATCHES.md`.
KiCad's licenses and third-party notices remain in the repository root and ship with every
runtime archive, together with the matching source.
