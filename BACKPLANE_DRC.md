# Backplane DRC: parallel, deterministic, lean (bp.13)

What changed in DRC on `perf/drc-parallel`, what "the same result" means, and the boards the
equality gate whitelists.

## Results

The report of a run depends only on the board, its rules and its libraries: it is the same at
every thread count and on every run.

* **Canonical order.** The engine buffers the violations each provider reports and hands them to
  the violation handler when the provider ends, sorted by error code, marker position, layer,
  item ids, message, rule and marker path (`DRC_ENGINE::flushPendingViolations`). This order is
  the new serial order: a 1-thread run gives the report every other thread count gives. bp.12's
  1-thread order is not kept; its set of violations is. Compare reports by content.
* **Error limits.** Each error code still reports at most 199 violations (499 for clearance and
  unconnected items) and the totals per code are unchanged. Where a code reaches its limit the
  report keeps the **first N in canonical order**. bp.12 kept whichever N its threads found first,
  which changed from run to run. Limits are no longer used to stop a provider early, so a board
  with very many violations of one kind is checked completely (bounded memory: the buffer per
  code is pruned to its first N as it fills).
* **Address-independent choices.** Some providers broke ties by heap address. They now use item
  ids or list order: the ratsnest's zone anchors (unconnected items), connection width (which
  items a neck names, and the order a net's copper is merged in).
* **Libraries.** `kicad-cli pcb drc` loads only the footprint libraries the board's footprints
  name, and the library parity check loads any it needs itself, so the CLI, the editor and an
  embedded editor that loads libraries lazily give the same parity result. A footprint naming a
  library missing from the table, or one that fails to load, gives the same violation as before.

### Connection width: same violations, possibly different items named

On the three qa boards where connection width now names other items than bp.12, every violation
is the same: count, type, severity, rule, measured width and layer are identical. Only which of
the items hit at the neck is named first changed (bp.12 took them in pointer-hash order).

| board | connection-width violations | measured values, rules, layers | items named | other violations |
|---|---|---|---|---|
| qa/data/pcbnew/connection_width_rules.kicad_pcb | 3 = 3 | identical | differ | identical |
| qa/data/pcbnew/issue9870.kicad_pcb | 13 = 13 | identical | differ | identical |
| qa/data/pcbnew/issue24772/fw16_MCIO8i.kicad_pcb | 1 = 1 | identical | differ | identical |
| demos/vme-wren/vme-wren.kicad_pcb | 193 = 193 | identical | identical | (length: see below) |

(bp.12 run 12 times per board at 1 and 16 threads; head 4 times.)

## Equality gate

`eq.py` (lane P harness): every board in demos/ and qa/data at 1 and 8 threads, canonical JSON
(date dropped; uuids not present in the board file, i.e. fresh per load, normalised) must be
byte-identical across runs and thread counts, and equal as a set to bp.12. 435 of 447 pass. The
others, by name and reason:

| board | reason |
|---|---|
| qa/data/pcbnew/creepage/creepage.kicad_pcb | creepage: graph ties broken by address (bp.12 also varies at 1 thread) |
| qa/data/pcbnew/creepage/creepage_malformed_edge.kicad_pcb | creepage, as above |
| qa/data/pcbnew/issue20480/issue20480.kicad_pcb | creepage, as above |
| qa/data/pcbnew/issue21482/issue21482.kicad_pcb | creepage, as above (also at error limits) |
| qa/data/pcbnew/issue23389/issue23389.kicad_pcb | creepage, as above |
| qa/data/pcbnew/issue24286/issue24286.kicad_pcb | creepage, as above |
| qa/data/pcbnew/issue24523/issue24523.kicad_pcb | creepage, as above |
| qa/data/pcbnew/issue24875_capstraight/issue24875_capstraight.kicad_pcb | creepage, as above |
| qa/data/cli/artwork_generation_regressions/ZoneFill-4.0.7.kicad_pcb | no uuids in the file: items get fresh ids each load, so id-based choices change per run (bp.12 too) |
| qa/data/pcbnew/connection_width_rules.kicad_pcb | connection width names other items (table above) |
| qa/data/pcbnew/issue9870.kicad_pcb | connection width names other items (table above) |
| qa/data/pcbnew/issue24772/fw16_MCIO8i.kicad_pcb | connection width names other items (table above) |
| qa/data/pcbnew/plugins/fakeboard.kicad_pcb | not a board: fails to load in bp.12 and head |

Known remaining run-to-run variation, all present in bp.12 (step D2 makes them deterministic):

* creepage (above);
* matched length: on vme-wren the measured length of a few `length_out_of_range` nets varies
  (bp.12 gave 3 different values in 3 runs);
* unconnected items on boards with anchors stacked at one position on several layers (VCU118 at
  more than one thread); bp.12 hid it behind its 499 cap.

## Threads

Headless programs (`kicad-cli`, the API server) size the pool to min(8, hardware threads) unless
`MaximumThreads` is set: Hubble_DK DRC is 1.04 s at 8 threads and 1.05 s at 16 with 0.86 vs
0.97 GB peak. The GUI keeps one thread per hardware thread.

## Profiling

`KICAD_DRC_PROFILE=1` prints, per DRC provider and zone fill, wall time, CPU time (so the
effective cores) and resident memory to stderr.
