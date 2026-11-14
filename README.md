# DRC equality gate bases

Data only: one directory per recorded base, never part of a release branch.

Each `<name>/` holds the reports of every board in `scripts/drc-equality/boards-all.txt` (447;
`plugins/fakeboard` is not a board and has none) from one runtime: `kicad-cli pcb drc --format json`
through `scripts/drc-equality/drcrun.sh` at 1 thread (`<board_path_with_underscores>-s14.json`) and
8 threads (`-s14-t8.json`), the layout `scripts/drc-equality/eqgate.py` takes as BASEDIR.
`MANIFEST` names the runtime, its SOURCE_COMMIT, when it was recorded and a sha256 per report.

- `bp13-7a7cccdb97/`: bp.13 batch 7 (ci/batch7 7a7cccdb97). The base for batch 8 and later.
  Recorded after `efce1b5f76` (matched-length items ordered by id), so skew_group_matched_drc,
  skew_within_diff_pairs_drc and issue18045/test_proj name the items batch 7 names.

Recorded with lane P's `eqbase.sh SRC RUNTIME OUT`; compared with `BASE_REPORTS=<dir> eqlocal.sh`.
