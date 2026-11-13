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

Number ledger for `kiapi.backplane.embed` (fork-only, additive; take the next free number and add
it here in the same change):

| `EventType` | Name | Since |
|---|---|---|
| 1-24 | frames, dialogs, documents, theme, comments, co-editing, plan, progress | bp.8-bp.12 |
| 25 | `ET_EDITOR_REQUESTED` (reserved, bp.13: editor tabs) | bp.13 |
| 26 | `ET_MARKER_CLICKED` (reserved, bp.13: host markers) | bp.13 |
| 27 | `ET_CHECKS_DONE` (RunChecks) | bp.13 |
| 28 | `ET_FRAME_DRAWN` (reserved, bp.13: cold start) | bp.13 |

Other bp.13 numbers: `FrameKind` 7 `FK_GERBVIEW`; `FrameInfo` 12 `lib_id`, 13 `files`; `Event` 8
`marker`, 9 `checks`, 10 `drc_marker` (all reserved until their branch lands).

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

## Tests

`scripts/backplane-*-regression.py <kicad-cli>` run headless against an isolated server and never
touch user projects; CI runs all of them on every build. `--stock-cli <stock 10.0.x kicad-cli>`
(where supported) also checks that KiCad 10 exports open in stock KiCad.

## Source and licensing

Upstream KiCad master at the commit in `BACKPLANE_UPSTREAM`, plus the patches in `PATCHES.md`.
KiCad's licenses and third-party notices remain in the repository root and ship with every
runtime archive, together with the matching source.
