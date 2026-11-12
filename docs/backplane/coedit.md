# Live co-editing: the person and agents edit the same open document

Status: design (stage 1 of the floorplanner/co-edit series, bp.11).

## Today (bp.10)

- The hub routes a project's `kicad_ipc` to the embedded GUI's API socket while the GUI session
  is ready, and to a headless `kicad-cli api-server` otherwise.
- In the GUI, every state-changing request answers `AS_BUSY` while a modal dialog is open
  (`API_SERVER`, `BACKPLANE_EMBED::ModalBlocker`), and while any interactive operation is in
  progress (`PCB_EDIT_FRAME::CanAcceptApiCommands`, `SCH_EDIT_FRAME::CanAcceptApiCommands`).
- API commits land on the one undo stack, so the person's Ctrl+Z undoes the agent's change, and
  an agent commit clears the person's redo list.
- Agents serialize on a named lock per document (`agents/design.rs`).

## Goal

Agents' operations land in the live document while the person keeps working. The person never
waits and never loses an edit; an agent never gets a blanket "busy" for something it can do.

## Actors

An *actor* is who made a change: the person (empty actor) or an API client (its client name,
which the hub sets to `agent:<thread>`). `BACKPLANE_ACTOR` (`common/backplane_actor.{h,cpp}`)
holds the current actor for the duration of one API request (`ACTOR_SCOPE`, set by
`API_SERVER` around the handler call). Everything below keys on it.

- `PICKED_ITEMS_LIST` gains `m_actor` (empty = the person) and `m_seq` (a per-document change
  number). `saveCopyInUndoList` stamps both.
- The change log (`CHANGE_LOG`, per editor frame, 4096 entries) records each pushed commit:
  `{seq, actor, description, created, modified, deleted}` as KIIDs.

## Holds: what the person is in the middle of

A *hold* is a set of items the person is editing now. Holds are computed when an agent
operation arrives, never polled:

| Person is... | Hold | Kind |
|---|---|---|
| idle | none | |
| in an interactive tool (move, drag, route, point edit, draw) | every item staged in a live person `COMMIT` (`COMMIT_REGISTRY`), plus the selection | item hold |
| routing or dragging with the router (PNS) | all copper connected items (tracks, vias, pads' footprints, zones) | layer-wide item hold |
| in a modal properties dialog | the selection (what the dialog edits) | item hold |
| in a *structural* dialog (below) | the whole document | exclusive |

The router hold is wide on purpose: PNS keeps raw pointers to board items for the whole
operation, so nothing it indexed may change under it.

### Structural operations (the only mutual exclusion left)

These rewrite the document wholesale; while one runs, every agent write answers `AS_BUSY`
("structural operation '<title>' in progress"), and while an agent runs one (an API request is
synchronous), the person's UI is blocked for that request only:

- Schematic: Annotate, Update PCB from Schematic / Update Schematic from PCB, Edit Symbol
  Fields table, Symbol library change/update, Global label/net rename, Sheet import.
- Board: Update PCB from Schematic (netlist import), Global Deletion, Cleanup Tracks & Vias,
  Exchange/Update Footprints, Board Setup, Renumber footprints, Specctra import, zone fill
  of all zones (`ZONE_FILL_OP` from the person).

A dialog is structural when its class is in `STRUCTURAL_DIALOGS` (by `wxWindow::GetName()` /
title key); everything else is an ordinary modal and only holds its selection.

## Applying an agent operation

`API_HANDLER_EDITOR` checks each item an operation touches against the holds:

1. **Create**: always applied (new items cannot conflict). Under an exclusive hold: `AS_BUSY`.
2. **Update** of an item outside every hold: applied at once.
3. **Update** of a held item: *deferred*. KiCad snapshots the agent's proto and the base (the
   item as it was before the person's edit: the held commit's staged copy, or the item as it is
   now for dialog holds). When the hold ends (the person's commit is pushed or reverted, the
   dialog closes), the deferred update is rebased: a three-way merge at protobuf field level
   (`PROTO_MERGE::ThreeWay(base, person, agent)`: every field the agent changed relative to the
   base takes the agent's value, every other field keeps the person's) and applied as its own
   agent undo group. The item status is `ISC_DEFERRED` in the reply.
4. **Delete** of a held item: deferred the same way and re-issued when the hold ends, if the item
   still exists. While an interactive tool runs, *every* agent delete is deferred (tools keep
   pointers to neighbours: ratsnest anchors, drag connections).
5. Under an exclusive hold: `AS_BUSY`, nothing applied, no deferral.

Each deferred operation produces a notice to the agent (`ET_AGENT_OP_REBASED`, detail JSON:
`{seq, item, op, reason, fields_kept_from_person}`) and a change-log entry with
`rebased: true`. If the item is gone when the hold ends, the notice says `dropped`.

A `BeginCommit`/`EndCommit` group with some deferred operations pushes the rest at
`EndCommit` and the deferred ones as a second group when their hold ends.

Modal dialogs no longer refuse writes wholesale: `ModalBlocker` becomes an exclusive hold only
for structural dialogs.

## Per-actor undo

- Ctrl+Z (the person) undoes the person's newest entry, skipping agent entries.
- Undoing entry P out of order is safe when no later entry touches P's items (top-level item
  pointers; `BOARD_COMMIT` always stages the parent footprint). When a later agent entry A
  does touch them, the person still wins: P is undone (the item takes P's before state), and
  every such A is dropped from the undo list (freed with `ClearListAndDeleteItems`; its own
  undo is no longer meaningful) with an `ET_AGENT_UNDO_DROPPED` notice.
- `AppendCopyToUndoList` appends to the newest entry *of the current actor*, never across
  actors.
- An agent push no longer clears the person's redo list; it drops only redo entries whose items
  it touches. The person's own push still clears the redo list as before.
- Edit > "Undo agent change: <description>" and the API command `UndoAgentChange{seq}` undo an
  agent entry explicitly (same overlap rule, the newer side wins).

Both editors implement this (`PCB_BASE_EDIT_FRAME`, `SCH_EDIT_FRAME`); the shared logic is in
`UNDO_REDO_CONTAINER` helpers (`FindNewest(actor)`, `Overlapping(list, after)`).

## Attribution

After an agent group is pushed, the touched items' bounding boxes flash (`AGENT_EDIT_FLASH`, a
`VIEW_ITEM` on `LAYER_SELECT_OVERLAY`): a translucent fill and an outline in the agent colour
with a label `<agent> · <description>` that fades over 2 s (timer-driven repaints, only while
one is fading). The status bar keeps the last agent change; the undo history shows the actor.

## The agent sees the person

- Every push (person or agent) appends to `CHANGE_LOG` and posts `ET_DOCUMENT_EDITED`
  (`Event.edit`: seq, actor, description, created/modified/deleted KIIDs).
- `GetChanges{since}` returns change-log entries after `since` (or `lost` if they scrolled out).
- Selection and cross-probe already arrive as `ET_SELECTION_CHANGED`.
- `GetHolds{}` returns the current holds (item KIIDs, kind, title), so an agent can avoid what
  the person is touching.

## Saving

`SaveDocument` from an agent saves the live document, including the person's unsaved edits; the
hub's commit on main therefore contains both. Autosave and local history are unchanged. A save
during an interactive operation is deferred until it ends (the board is mid-edit), and answers
`ISC_DEFERRED`-style status `SS_DEFERRED`; the hub waits for the following `ET_DOCUMENT_SAVED`.

## Hub (Backplane)

- `kicad_ipc` sets the API client name to `agent:<thread>` so actors are distinct per thread.
- While the GUI is ready, agent writes go to the GUI (as today) and no longer need the document
  lock between agents; the lock remains only for structural commands (`ImportNetlist`,
  `UpdatePCBFromSchematic`, `Annotate`, `RunAction` of a structural action) and for the headless
  path, which has no holds.
- `ET_DOCUMENT_EDITED` from the person becomes a quiet note in the agent's next turn context
  (`the person moved U3, R5`); `ET_AGENT_OP_REBASED` is returned to the agent with the tool
  result of its next KiCad call.

## Tests

`qa/backplane/coedit_stress.py`: on Xvfb, a person driver (xdotool: select, drag footprints,
move with `M`, open/close Properties, Ctrl+Z) and an agent driver (IPC: create tracks, update
and delete footprints, commits) run together for N minutes on one board. Checks: no crash; no
`AS_BUSY` except under structural dialogs; every agent op is applied, deferred-then-applied or
reported dropped; the person's Ctrl+Z only ever reverts person entries; the saved board reloads
and matches the live one.

Unit tests (`qa/tests/pcbnew/test_coedit.cpp`): proto three-way merge, overlap detection,
out-of-order undo, append-per-actor, redo dropping.
