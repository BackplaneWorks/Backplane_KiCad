# Routing floorplanner: plan, steer, approve, then route

Status: design (stages 2–5 of the floorplanner/co-edit series, bp.11+). Co-editing
(`coedit.md`) is stage 1 and a prerequisite: the person steers the plan while the agent edits it.

An agent proposes how a board will be routed before any copper exists: corridors for buses and
net groups on signal layers, plane regions on power layers. The person audits and steers that
plan in the PCB editor, approves it per layer or per group, and only then is copper routed, by
the plan. Nothing is routed that the person did not approve.

## The plan

### File

`<board>.kicad_plan` beside the `.kicad_pcb` (JSON, UTF-8, LF). The board file is unchanged; a
board without a plan has no file. Saved with the board (same save, same atomic rename), loaded
with it, kept in local history, copied with the project, and committed on main by the hub like
any design file. JSON because agents, the hub and Backplane's viewer read it directly.

```json
{
  "format": 1,
  "revision": 17,
  "board": "main.kicad_pcb",
  "layers": [
    { "layer": "In6.Cu", "role": "signal", "status": "proposed", "note": "" },
    { "layer": "In4.Cu", "role": "power",  "status": "approved", "note": "" }
  ],
  "groups": [
    { "id": "g-ddr-dq0", "name": "DDR DQ0-7", "color": "#e8a33d",
      "nets": ["/DDR/DQ0", "..."], "diff_pairs": [], "netclass": "plan_DDR_DQ0",
      "status": "proposed", "locked": false }
  ],
  "corridors": [
    { "id": "c-12", "group": "g-ddr-dq0", "layer": "In6.Cu", "order": 3,
      "path": [[x, y], ...], "width": 2.4,
      "from": ["U2"], "to": ["U7"],
      "vias": [[x, y], ...], "escapes": [{"at": [x, y], "ref": "U2"}],
      "status": "proposed", "locked": false, "author": "agent:t123", "notes": [] }
  ],
  "planes": [
    { "id": "p-3", "net": "+1V8", "layer": "In4.Cu", "color": "#c0583a",
      "outline": [[[x, y], ...]], "priority": 2, "reference": "In3.Cu",
      "status": "approved", "locked": true, "author": "agent:t123" }
  ],
  "splits": [ { "id": "s-1", "layer": "In4.Cu", "path": [[x, y], ...], "gap": 0.3 } ],
  "notes": [ { "id": "n-1", "at": [x, y], "layer": "*", "text": "keep away from U2",
               "author": "person", "target": "c-12" } ],
  "constraints": [ { "id": "k-4", "kind": "avoid", "target": "c-12", "ref": "U2",
                     "author": "person", "text": "keep away from U2" } ],
  "history": [ { "revision": 17, "actor": "person", "op": "move", "items": ["c-12"],
                 "at": "2026-11-11T21:04:00Z" } ]
}
```

Units are millimetres, board coordinates, Y down (as KiCad's own files). Layers use KiCad's
canonical names. `status` is one of `proposed`, `approved`, `rejected`, `routing`, `routed`,
`verified`, `failed`. Corridors and planes also carry `edited` (the person changed it since the
agent proposed it) and, once routing starts, `progress: {routed, total}` (connections).

### Progress and status, as the person sees it

| State | Stored as | Drawn as |
|---|---|---|
| Proposed | `status: proposed` | translucent band, dashed outline |
| Edited by the person | `edited: true` | solid outline, small pencil mark |
| Approved | `status: approved` | solid band, ✓ in the label ("14 ✓") |
| Routing | `status: routing`, `progress` | the band fills from source toward destination as connections land; label "9/14" |
| Routed | `status: routed` | the band fades to a thin outline so the copper shows through |
| Verified | `status: verified` | green outline, "14 ✓✓" |
| Problem | derived (audit + DRC) | red outline with a ⚠ count; clicking it zooms to the first issue |
| Locked | `locked: true` | 🔒 in the label; agent updates are refused |

*Verified* is set by `VerifyPlan` (and after `RoutePlan`) when, for the group: DRC has no
violations on its nets, its length range and skew are within the group's `max_skew` /
`length` targets (when given), and none of its nets has an unrouted connection. A later edit of
its copper or the item drops it back to `routed`.

*Problems* are never stored: crossings, congestion, split crossings, missing nets, DRC markers on
the group's nets and unrouted connections after routing, each with a location.

**Parts**: in plan view each part's outline fill shows pin completion: empty when none of its
connected pins is routed, partly filled (proportional, bottom-up) when some are, solid when
all are (from the board's connectivity, live).

**Per layer**: the layer header (and each layer row in the panel) has a progress bar, "SIG1 ·
3/4 groups", counting groups on that layer that are routed or verified.

**Board checklist** (the Routing plan panel): every group with its state, progress and problem
count; a row click selects and zooms to it; "Hide done" hides verified groups.

`GetPlan` returns all of this (`derived.items[id]`: `{state, progress, problems[], verified}`,
`derived.layers[layer]`: `{groups, done}`, `derived.parts[ref]`: `{pins, routed}`), so agents
and Backplane's own viewer show the same states. `history` keeps the newest 500 operations; git keeps the rest.

`format` is the schema version; a newer format than the reader knows opens read-only.
`revision` increases by one per accepted change; every update carries the revision it was based
on (`base`), and an update based on an older revision is rebased per item (the same
person-wins rule as co-editing; see below) or refused if it touches a locked or changed item.

### In memory

`BOARD_PLAN` (`pcbnew/backplane/board_plan.{h,cpp}`), owned by the `BOARD` (like the
design settings): parsed once, edited through `PLAN_COMMIT`s that go on the board's undo stack
as `UNDO_REDO::PLAN` entries (the person's Ctrl+Z undoes plan edits too, per actor as in
`coedit.md`). The board's modified flag covers it; saving the board writes both files.

Derived, never stored: per-layer stack-up text ("Stripline between GND2 (50 µm core) and GND3
(30 µm)", "Faces GND2 across 30 µm"), corridor crossings, congestion, length and skew
estimates.

## Rendering (native GAL overlay)

- New GAL layer `LAYER_PLAN` (drawn above copper, below selection) and a `PLAN_VIEW_ITEM`
  per plan, with the plan painter in `pcbnew/backplane/plan_painter.cpp`. Cached per layer;
  redrawn only on plan change or active-layer change.
- **Signal layer**: each corridor is a band along its path at its width, translucent fill in the
  group colour with rounded ends and a slightly stronger outline; its order number and net
  count in a filled badge at the midpoint; vias as small diamonds; escapes as short ticks at
  the part. Locked corridors draw a solid outline; rejected ones are hatched grey; selected
  ones get handles.
- **Power layer**: plane regions as opaque-ish fills per net, split lines as dashed, net name
  centred in the largest region.
- **Header**: top-left of the canvas, the layer chip `L6 · SIG1` (index · name), the stack-up
  line underneath, and the layer status (proposed / approved).
- **Plan view** (toggle, `Alt+P`): dims copper, zones and silkscreen to ghost level, draws
  footprints as outlined courtyards with their reference centred, and draws only the active
  layer's plan items at full strength (others faint). This is the look of the reference: dark
  board, white outline, grey parts, coloured bands.
- Appearance panel: a "Routing plan" row with visibility and opacity, and per-layer visibility
  follows the copper layer checkboxes.

## Steering (the editing UI)

`PLAN_TOOL` (`pcbnew/backplane/plan_tool.cpp`) plus a dockable **Routing plan** panel.

On the canvas (plan view or not, when the plan layer is visible):
- Click selects a corridor, plane region, split or note; Shift adds.
- Drag a corridor's body moves it; drag a vertex handle reshapes it; double-click on the path
  adds a vertex; Delete on a vertex removes it; drag the edge handles changes the width.
- Plane regions: vertex drag, add/remove vertices, as zone outlines.
- `Draw corridor` (`Ctrl+Shift+B`): click points, double-click to finish; asks for the group.
- `Draw plane region` (`Ctrl+Shift+N`): polygon; asks for the net.
- Context menu: Move to layer ▸, Order ▸ (earlier / later / first / last), Lock / Unlock,
  Accept, Reject, Merge groups (two or more selected), Split group… (net picker), Add note…,
  Delete.
- `A` accepts the selection, `R` rejects it (in plan context only; hotkeys are plan-tool
  scoped).

The panel lists layers (with status and an Approve layer / Reject layer button each), and under
each its groups and items: colour, name, net count, order, status, warnings, length/skew. It
shows the audit: crossings between corridors on the same layer, congestion (corridor width
needed vs available between parts), corridors crossing a plane split, and per-bus length
range and skew estimate (path length + escape distances). Buttons: Approve all, Request
re-plan (sends the notes and edits as constraints to the agent), Route approved, and while
routing runs, **Stop / back to planning**.

Every person edit is a plan operation with author `person`; locked items refuse agent updates;
every operation is added to `history` and posted to the agent (`ET_PLAN_CHANGED` with the
operation), and person operations that constrain the agent (move, reshape, re-layer, reorder,
lock, delete, note, reject) are also added to `constraints` in plain words, so the agent's next
proposal must respect them.

## Audit gate

Routing refuses anything not approved: `RoutePlan` only routes corridors whose own status,
group status and layer status are all `approved`, and planes whose status is `approved`.
Approval is the person's: `ApprovePlan` is refused for agent actors (`AS_BAD_REQUEST`,
"approval is the person's") unless the hub marks the request as the person's own action (the
Backplane UI's Approve button). An approved item that an agent then updates goes back to
`proposed`. The same holds for the approval actions through `RunAction` (`pcbnew.Plan.accept`,
`reject`, `approveLayer`, `rejectLayer`): refused for agent actors. `ApprovePlan` works while
plan view is on; `RunAction` answers `AS_BUSY` then, as for any action while a tool runs.

## Routing from the plan

`RoutePlan{items?}` (an API task, cancellable; the panel's Route button runs the same):
1. For each approved group: create or update its net class `plan_<group>` (width/clearance from
   the group or the default net class), and diff-pair rules for its pairs.
2. For each approved corridor: a rule area named `plan:<corridor-id>` on its layer, outline =
   the corridor band; a custom rule
   `(rule "plan:<id>" (condition "A.NetClass == 'plan_<group>' && A.Layer == '<layer>' && !A.enclosedByArea('plan:<id>')") (constraint disallow track via))`
   so DRC and the interactive router keep the group inside it.
3. Route each corridor's connections with the sketch router (`SketchRoute`: items = the group's
   pads between `from` and `to`, guide = the corridor path, preferred layer = its layer), in
   `order`. Status moves `routing` → `routed` / `failed` per corridor with counts.
4. Each approved plane region becomes a filled zone (net, layer, outline, priority) named
   `plan:<plane-id>`; splits become zone gaps.
5. Stop cancels the task between connections; finished copper stays (its own undo group), the
   rest returns to `approved`, and the panel is back in planning.

Created copper is attributed to the agent (or the person, from the panel's button) per
`coedit.md`.

## API (`kiapi.backplane.plan`, served by the GUI and the headless server)

| Command | Reply | |
|---|---|---|
| `GetPlan{board}` | `{json, revision}` | whole plan with derived data (`stackup`, `audit`) |
| `ProposePlan{board, json, mode}` | `{revision, ids}` | `PM_REPLACE_PROPOSED` replaces every unlocked, unapproved item; `PM_MERGE` upserts by id |
| `UpdatePlan{board, base, ops}` | `{revision, applied, rebased, refused}` | ops: JSON patch-like `{op: set\|move\|delete\|add\|status\|order\|lock\|note, id, ...}` |
| `ApprovePlan{board, layer?, ids?, approve}` | `{revision}` | person only (see above) |
| `RoutePlan{board, ids?}` | task id | cancellable task |
| `GetPlanEvents{since}` | `{events}` | plan operations after a revision |

Hub MCP tools: `plan_get`, `plan_propose`, `plan_update`, `plan_approve` (approve is offered to
agents only to *request* approval: it posts a request to the person), routed to the GUI when
it is open and to the headless server otherwise.

## Backplane's viewer

The hub reads `<board>.kicad_plan` (watched like the board) and the GPUI board viewer draws the
same overlay from it: bands, badges, plane fills, the layer header. Read-only there; steering is
in KiCad.

## Stages

1. Co-editing core (`coedit.md`).
2. `BOARD_PLAN` data, file I/O, `LAYER_PLAN` rendering, plan view; screenshots.
3. `PLAN_TOOL` and the Routing plan panel.
4. `kiapi.backplane.plan` and the hub's `plan_*` MCP tools.
5. `RoutePlan`.
