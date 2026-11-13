# DRC equality gate

Committed as run for the bp.13 DRC zone guard (perf/drc-parallel-bp13 @ 02b097e1db:
447 boards, 0 failing), before it is wired into CI.  Paths in the shell scripts are still
those of the build machine it ran on (`/home/user/drc`, runtimes in `rts/<name>`, the base
build's reports in `out/eq-all17`); CI wiring makes them arguments.

- `boards-all.txt`: the corpus, every `.kicad_pcb` under `demos/` and `qa/data/`, relative
  to the source root.
- `drcrun.sh KICAD_CLI BOARD THREADS OUT.json`: one DRC run with a private config
  (`MaximumThreads=THREADS`); prints wall, user, sys, max RSS and the report's md5 without
  its date line.
- `cmp17.py BOARD A.json B.json`: compares two reports as multisets, with item UUIDs that
  are not stored in the board file set aside (the board item gets a new one at every load).
- `eq18.sh RT TAG`: every board in the corpus, runtime `RT` at 1 and 8 threads against the
  base build (`s14`, bp.13 without the guard) at 1 thread; writes `out/eq-TAG/cmp.txt`.
- `eqgate.py CMP.txt eq-exclude.txt OUTDIR BASEDIR`: the verdict.  Every board must equal
  the base and be the same at 1 and 8 threads; a raw byte difference between 1 and 8 threads
  is allowed only where the base build has it too.  Boards in `eq-exclude.txt` are checked by
  their mode instead (each with its reason there); an excluded board missing from the corpus
  fails.

The DRC regressions (known gaps, determinism, library parity, the zone guard probes, the
corpus triangulation check) are `scripts/backplane-drc-regression.py KICAD_CLI`.
