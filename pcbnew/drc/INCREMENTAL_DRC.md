# Incremental DRC (design, lane P, bp.13+)

Goal: after an edit, re-check only what the edit can have changed and keep the rest of the last
report, with the result **identical to a full run** (same canonical-order report, see
`DRC_ENGINE::flushPendingViolations`). The full run stays the reference and the fallback.

## Inputs

* The previous run's results, kept by the engine: for each violation its provider, error code,
  item ids and marker position (`PENDING_VIOLATION`, before the limit is applied), plus the
  board/rules/settings fingerprint they were computed under.
* The dirty set, built from every `BOARD_COMMIT` since that run (the commit's staged changes
  carry the before-copy of modified and removed items):
  * `items`: ids of added, removed and modified items, and of each footprint's children;
  * `nets`: their nets before and after (a net change dirties both);
  * `layers`: their layer sets before and after;
  * `region`: per layer, the union of the before and after bounding boxes, inflated by the
    largest distance any constraint can reach (`max( m_DRCMaxClearance,
    m_DRCMaxPhysicalClearance )`, plus the largest creepage constraint when creepage runs).

## Invalidation (forces a full run)

* any change to the rules (`.kicad_dru` text, implicit rules), net classes or net class
  assignments, board design settings, stackup, enabled layers, severities or exclusions file;
* a zone refill (fills are not commits; the filler reports the zones it changed, which become
  dirty items with their old and new fill extents as the region);
* the library or schematic netlist used by the parity tests changing (their own caches key it);
* more than a threshold of the board dirty (e.g. region covering > 30 % of the board area or
  > 5 % of the items): a full run is then as fast and simpler.

## Providers

| class | providers | incremental rule |
|---|---|---|
| per item | track width, via diameter, annular width, hole size, text dims, text mirroring, track angle, segment length, misc (text vars, unresolved), footprint checks, via stack, library parity (per footprint) | re-test the dirty items only |
| pairwise local | copper clearance, hole-to-hole, physical clearance, edge clearance, silk clearance, courtyard clearance, solder mask, disallow, zone connections, via stitch | re-test every item whose shape intersects `region` on its layer against the full trees (a pair is re-tested when either item is in the region) |
| per net / net-layer | connectivity (unconnected, dangling, isolated copper), connection width, sliver checker, matched length, diff pair coupling, net chain tuning | recompute the dirty nets only (connection width and slivers per dirty net-layer polygon; the ratsnest already updates per net) |
| whole board | board structure / outline, schematic parity, creepage | always run in full (creepage could later use the region, but its graph is board-wide) |

Merging: drop every cached violation that a re-run could have produced (for per-item and
per-net providers: those naming a dirty item or net; for pairwise providers: those with any
item intersecting `region`, or naming a dirty or deleted item), add the re-run's violations,
then canonical-sort and apply the error limits once over the merged set. Because limits apply
after the merge and the order is canonical, the merged report equals a full run's.

## Proof obligation

Property test (qa/tests/pcbnew/drc/test_drc_incremental.cpp): for every qa/demo board, seeded
random edit sequences (move/rotate a footprint, move/delete/add a track or via, change an item's
net, change a track width, add a keepout, change a net class clearance, refill one zone), and
after every edit assert `canonical(incremental) == canonical(full)` byte for byte. Run at 1 and
N threads.

## API

Engine entry point for RunChecks (the embed lane owns the IPC handler; proposed hunk):

```proto
message RunChecks {
  FrameKind kind         = 1;
  bool      refill_zones = 2;
  bool      parity       = 3;
  bool      incremental  = 4;  // FK_PCB: re-check only what changed since the last run in this
                               // session; the engine falls back to a full run when it must
}
// Checks gains: bool incremental = 9;  // the run was incremental (false: a full run was needed)
```

```cpp
// DRC_ENGINE
void RunTests( EDA_UNITS aUnits, bool aReportAllTrackErrors, bool aTestFootprints,
               BOARD_COMMIT* aCommit = nullptr, const DRC_DIRTY_SET* aDirty = nullptr );
bool LastRunWasIncremental() const;
// The board editor collects commits into a DRC_DIRTY_SET between runs (BOARD_LISTENER).
```
