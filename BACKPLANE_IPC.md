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
| `runaction-frame` | which editor runs a `RunAction` | `RunAction.frame` (field 100) names it (`FT_PCB_EDITOR`, `FT_SCHEMATIC_EDITOR`); a frame not open, or without the action, leaves it unhandled. With no frame, an `eeschema.*` action goes to the schematic editor and any other (`pcbnew.*`, and shared ones such as `common.Interactive.undo`) to the PCB editor while one is open, else to the schematic editor. Before it, with both editors open, a shared action could reach either |
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

Other bp.13 numbers: `FrameKind` 7 `FK_GERBVIEW`; `FrameInfo` 12 `lib_id`, 13 `files`; `Event` 8
`marker`, 9 `checks`, 10 `drc_marker` (all landed in bp.13).

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

`scripts/backplane-*-regression.py <kicad-cli>` run headless against an isolated server and never
touch user projects; CI runs all of them on every build. `--stock-cli <stock 10.0.x kicad-cli>`
(where supported) also checks that KiCad 10 exports open in stock KiCad.

## Source and licensing

Upstream KiCad master at the commit in `BACKPLANE_UPSTREAM`, plus the patches in `PATCHES.md`.
KiCad's licenses and third-party notices remain in the repository root and ship with every
runtime archive, together with the matching source.
