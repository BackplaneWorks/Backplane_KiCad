#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Export-to-stock gate: what the fork saves must open cleanly in the current stock KiCad release.

For each project: the fork checks it (sch erc, pcb drc), exports a KiCad 10 copy
(`project downgrade --target 10.0`), and stock kicad-cli then checks and exports that copy.
Fails on any stock load or export that is not clean (a repair, an unknown token, a warning while
loading, a non-zero exit) and on any difference between the fork's ERC/DRC findings on the
original and stock's on the export.

    backplane-stock-export-regression.py <fork kicad-cli> --stock-cli <stock kicad-cli>
        [--project DIR ...] [--keep DIR]

Without --stock-cli it only says it is skipped (the IPC regression step runs every
backplane-*-regression.py with the fork's kicad-cli alone).
"""

import argparse
from collections import Counter
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
DEMOS = HERE.parent / "demos"
DEFAULT_PROJECTS = ["pic_programmer", "ecc83", "complex_hierarchy", "backplane-structure",
                    "backplane-assembly"]

# Lines stock prints on a clean run; anything else on a load or export is a finding
CLEAN = [r"^Loading (board|schematic)", r"^Running (DRC|ERC)", r"^Found \d+ ", r"^Saved (DRC|ERC) Report",
         r"^\*\*\* ", r"^Plotting ", r"^Plotted ", r"^Done\.?$", r"^Exporting ", r"^Exported ",
         r"^Generating ", r"^Writing ", r"^Created ", r"^Gerber files", r"^Successfully ",
         r"^\s*$", r"^Fetching ", r"^Building ", r"^Annotating", r"^Report ", r"^Netlist ",
         r"^Exporting netlist", r"^Creating ", r"^Finished", r"^Generated ", r"^Plot ",
         r"^Drill file", r"^Job file", r"^Loading", r"^Checking ", r"^Updating ", r"^Saving",
         r"^Using ", r"^Output "]
SUSPECT = re.compile(r"(?i)repair|fix(ed|ing)|older version|newer version|unknown|unexpected|"
                     r"invalid|error|warning|failed|could not|cannot|missing|converted")

FAILURES = []


def check(ok, message):
    print(("PASS " if ok else "FAIL ") + message, flush=True)
    if not ok:
        FAILURES.append(message)
    return ok


def run(cli, args, env, timeout=600):
    result = subprocess.run([str(cli), *map(str, args)], env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    return result.returncode, result.stdout


def suspicious(output):
    lines = []
    for line in output.splitlines():
        line = line.strip()
        if any(re.search(pattern, line) for pattern in CLEAN):
            continue
        if SUSPECT.search(line):
            lines.append(line)
    return lines


def violations(report_path):
    """Counts by (type, severity); entries the person excluded are counted on their own
    (stock 10.0 lists stale exclusions with --severity-all, the fork drops them)."""
    report = json.loads(Path(report_path).read_text())
    found = Counter()

    def visit(node):
        if isinstance(node, dict):
            if "type" in node and "severity" in node:
                excluded = bool(node.get("excluded")) or str(node.get("severity")) == "exclusion"
                found[("excluded" if excluded else "active", str(node["type"]), str(node["severity"]))] += 1
            for value in node.values():
                visit(value)
        elif isinstance(node, list):
            for value in node:
                visit(value)

    visit(report)
    return Counter({k: v for k, v in found.items() if k[0] == "active"})


def rulecheck(cli, kind, path, out, env, label):
    command = [kind, "erc" if kind == "sch" else "drc", "--format", "json", "--severity-all",
               "--output", out, path]
    code, output = run(cli, command, env)
    ok = check(code == 0 and Path(out).is_file(), f"{label}: {kind} {'erc' if kind == 'sch' else 'drc'} runs")
    return (violations(out) if ok else Counter()), output


KNOWN = json.loads((HERE / "backplane-stock-engine-known.json").read_text()) \
    if (HERE / "backplane-stock-engine-known.json").is_file() else []


def item_sets(report_path, violation_type):
    """The items of each violation of one type in a JSON report, as frozensets of their
    descriptions (stable across loads, unlike a regenerated teardrop's UUID)."""
    found = Counter()

    def visit(node):
        if isinstance(node, dict):
            if node.get("type") == violation_type and "items" in node:
                found[frozenset(str(i.get("description")) for i in node["items"])] += 1
            for value in node.values():
                visit(value)
        elif isinstance(node, list):
            for value in node:
                visit(value)

    if Path(report_path).is_file():
        visit(json.loads(Path(report_path).read_text()))
    return found


def known_engine_difference(project, kind, key, counts=None, fork_report=None, stock_report=None):
    """A reviewed difference between the fork's (master's) checker and stock's, the same on the
    native file and the export: scripts/backplane-stock-engine-known.json.  An entry with "items"
    covers exactly those violations (each the set of its items' descriptions) the fork reports
    and stock does not, nothing more."""
    for entry in KNOWN:
        if entry.get("kind") != kind or entry.get("type") != key[1] \
                or entry.get("project", project) not in (project, "*"):
            continue
        if "items" not in entry:
            return True
        if counts is None or fork_report is None or stock_report is None:
            continue
        want = Counter(frozenset(pair) for pair in entry["items"])
        extra = item_sets(fork_report, key[1]) - item_sets(stock_report, key[1])
        if extra == want and counts[0] - counts[1] == sum(want.values()):
            return True
    return False


HUBBLE_RULES = """(version 1)
(rule "U1 fanout area"
  (condition "A.intersectsArea('U1 fanout')")
  (constraint track_width (min 0.0762mm))
  (constraint clearance (min 0.0762mm)))
(rule "U1 fanout width check"
  (condition "A.intersectsArea('U1 fanout')")
  (constraint track_width (min 1.0mm)))
"""

HUBBLE_STRAYS = ["backups/layout-2026-10-21/hubble_shape.kicad_pcb",
                 "backups/layout-2026-10-21/hubble_shape.kicad_pro",
                 "backups/layout-2026-10-21/hubble_shape.kicad_sch",
                 "build/layout/try.kicad_pcb"]


def make_hubble_fixture(root):
    """The person's Hubble_DK in miniature (from pic_programmer): a plain rule area named
    'U1 fanout' around U1, custom rules on it with A.intersectsArea(), a build output board
    (build/layout/try.kicad_pcb) and a backup copy of the project (backups/<date>/)."""
    target = root / "hubble_shape"
    shutil.copytree(DEMOS / "pic_programmer", target)
    for suffix in (".kicad_pro", ".kicad_pcb", ".kicad_sch", ".kicad_prl"):
        if (target / ("pic_programmer" + suffix)).is_file():
            (target / ("pic_programmer" + suffix)).rename(target / ("hubble_shape" + suffix))
    board = target / "hubble_shape.kicad_pcb"
    text = board.read_text()
    ref = text.index('(property "Reference" "U1"')
    footprint = text.rindex("(footprint ", 0, ref)
    at = re.search(r"\(at ([-\d.]+) ([-\d.]+)", text[footprint:ref])
    x, y = float(at.group(1)), float(at.group(2))
    x1, y1, x2, y2 = x - 15, y - 15, x + 15, y + 15
    zone = f"""
\t(zone
\t\t(net 0)
\t\t(net_name "")
\t\t(layers "F.Cu" "B.Cu")
\t\t(uuid "6f3a1b2c-0d4e-4f56-8a9b-0c1d2e3f4a5b")
\t\t(name "U1 fanout")
\t\t(hatch edge 0.5)
\t\t(connect_pads
\t\t\t(clearance 0)
\t\t)
\t\t(min_thickness 0.25)
\t\t(filled_areas_thickness no)
\t\t(keepout
\t\t\t(tracks allowed)
\t\t\t(vias allowed)
\t\t\t(pads allowed)
\t\t\t(copperpour allowed)
\t\t\t(footprints allowed)
\t\t)
\t\t(placement
\t\t\t(enabled no)
\t\t\t(sheetname "")
\t\t)
\t\t(fill
\t\t\t(thermal_gap 0.5)
\t\t\t(thermal_bridge_width 0.5)
\t\t)
\t\t(polygon
\t\t\t(pts
\t\t\t\t(xy {x1} {y1}) (xy {x2} {y1}) (xy {x2} {y2}) (xy {x1} {y2})
\t\t\t)
\t\t)
\t)
"""
    end = text.rstrip().rindex(")")
    board.write_text(text[:end] + zone + ")\n")
    (target / "hubble_shape.kicad_dru").write_text(HUBBLE_RULES)
    pro = target / "hubble_shape.kicad_pro"
    pro.write_text(pro.read_text().replace("pic_programmer", "hubble_shape"))
    build = target / "build" / "layout"
    build.mkdir(parents=True)
    shutil.copyfile(board, build / "try.kicad_pcb")
    backup = target / "backups" / "layout-2026-10-21"
    backup.mkdir(parents=True)
    for suffix in (".kicad_pro", ".kicad_sch", ".kicad_pcb"):
        shutil.copyfile(target / ("hubble_shape" + suffix), backup / ("hubble_shape" + suffix))
    return target


def gate_project(fork, stock, source, work, fork_env, stock_env):
    name = source.name
    project_dir = work / name
    shutil.copytree(source, project_dir)
    pro = next(project_dir.glob("*.kicad_pro"), None)
    if pro is None:
        print(f"SKIP {name}: no .kicad_pro")
        return
    board = pro.with_suffix(".kicad_pcb")
    schematic = pro.with_suffix(".kicad_sch")
    reports = work / f"{name}-reports"
    reports.mkdir()

    # Saved natively by the fork first, as a person's project is after any save in it
    for kind, original in (("pcb", board), ("sch", schematic)):
        if original.is_file():
            code, output = run(fork, [kind, "upgrade", "--force", original], fork_env)
            check(code == 0, f"{name}: the fork saves the {kind} natively")

    export = work / f"{name}-v10"
    # Without --force first: a refusal ("export would lose data") is a finding of its own
    code, output = run(fork, ["project", "downgrade", "--target", "10.0", "-o", export,
                              "--report-json", reports / "downgrade-plain.json", pro], fork_env)
    check(code == 0, f"{name}: the fork exports a KiCad 10 copy without --force "
                     f"{[l for l in output.splitlines() if l.strip()][-3:] if code else ''}")
    if name == "hubble_shape" and code == 0:
        report = json.loads((reports / "downgrade-plain.json").read_text())
        skipped = sorted(e["feature"].replace("\\", "/") for e in report["entries"] if e["category"] == "skip")
        check(skipped == sorted(HUBBLE_STRAYS), f"{name}: the stray boards and the backup are left out and listed "
                                                f"{skipped}")
        check(not (export / "build").exists() and not (export / "backups").exists(),
              f"{name}: nothing of build/ or backups/ is in the export")
        rules = (export / "hubble_shape.kicad_dru").read_text()
        check('\n(rule "U1 fanout area"' in rules and '\n(rule "U1 fanout width check"' in rules,
              f"{name}: both rules on the rule area are kept for 10.0")
    if code != 0:
        shutil.rmtree(export, ignore_errors=True)
        code, output = run(fork, ["project", "downgrade", "--target", "10.0", "--force", "-o", export,
                                  "--report-json", reports / "downgrade.json", pro], fork_env)
        if not check(code == 0, f"{name}: the fork exports a KiCad 10 copy with --force"):
            print(output)
            return

    for kind, original in (("sch", schematic), ("pcb", board)):
        if not original.is_file():
            continue
        exported = export / original.name
        fork_native, _ = rulecheck(fork, kind, original, reports / f"fork-{kind}.json", fork_env, f"{name} fork")
        fork_export, _ = rulecheck(fork, kind, exported, reports / f"fork-export-{kind}.json", fork_env,
                                   f"{name} fork on the export")
        stock_found, stock_output = rulecheck(stock, kind, exported, reports / f"stock-{kind}.json", stock_env,
                                              f"{name} stock")
        noise = suspicious(stock_output)
        check(not noise, f"{name}: stock loads the exported {kind} cleanly {noise[:5]}")
        # The export must not change what the fork itself finds; a remaining fork/stock difference
        # is the engines', and must be a known, reviewed one
        if name == "hubble_shape" and kind == "pcb":
            width = ("active", "track_width", "error")
            check(fork_export[width] > 0 and fork_export[width] == stock_found[width],
                  f"{name}: the rule area's track_width rule finds the same in stock 10.0 "
                  f"(fork {fork_export[width]}, stock {stock_found[width]})")
        diff_export = (fork_export - fork_native) + (fork_native - fork_export)
        check(not diff_export, f"{name}: exporting leaves the fork's {kind} findings unchanged {dict(diff_export)}")
        engine = {}
        for key in set(fork_export) | set(stock_found):
            if fork_export[key] != stock_found[key]:
                engine[key] = (fork_export[key], stock_found[key])
        unknown = {k: v for k, v in engine.items()
                   if not known_engine_difference(name, kind, k, v, reports / f"fork-export-{kind}.json",
                                                  reports / f"stock-{kind}.json")}
        for k, v in engine.items():
            if k not in unknown:
                print(f"  known engine difference {name} {kind} {k[1]}: fork {v[0]}, stock {v[1]}")
        check(not unknown, f"{name}: stock's {kind} findings equal the fork's, but for known engine "
                           f"differences {unknown}")

    exports = [("pcb", ["export", "gerbers", "--output", work / f"{name}-gerbers"], board),
               ("pcb", ["export", "drill", "--output", str(work / f"{name}-drill") + os.sep], board),
               ("sch", ["export", "netlist", "--output", work / f"{name}.net"], schematic),
               ("sch", ["export", "pdf", "--output", work / f"{name}-sch.pdf"], schematic)]
    for kind, args, original in exports:
        if not original.is_file():
            continue
        if args[1] == "drill":
            (work / f"{name}-drill").mkdir(exist_ok=True)
        code, output = run(stock, [kind, *args, export / original.name], stock_env)
        noise = suspicious(output)
        check(code == 0 and not noise, f"{name}: stock {kind} {' '.join(map(str, args[:2]))} is clean "
                                       f"(exit {code}) {noise[:5]}")


def template_dir(cli):
    """That KiCad's own template folder (the global library tables a fresh install seeds)."""
    real = Path(cli).resolve()
    for candidate in (real.parent.parent / "share/kicad/template", real.parent / "root/usr/share/kicad/template",
                      real.parent.parent / "root/usr/share/kicad/template"):
        if candidate.is_dir():
            return candidate
    for line in Path(cli).read_text(errors="ignore").splitlines()[:20] if Path(cli).is_file() else []:
        m = re.search(r"root=(\S+)", line)
        if m and (Path(m.group(1).strip('"')) / "usr/share/kicad/template").is_dir():
            return Path(m.group(1).strip('"')) / "usr/share/kicad/template"
    return None


LIBS = None   # --libs: one set of KiCad libraries for both CLIs


def private_env(home, cli):
    """A private HOME/XDG for one CLI, with its own global library tables: a first CLI-only run
    of stock has none and reports every library missing."""
    env = dict(os.environ)
    for variable, directory in (("HOME", "home"), ("XDG_CONFIG_HOME", "config"),
                                ("XDG_DATA_HOME", "data"), ("XDG_CACHE_HOME", "cache")):
        path = home / directory
        path.mkdir(parents=True, exist_ok=True)
        env[variable] = str(path)
    env.pop("DISPLAY", None)
    if LIBS:
        # The same libraries for both, so library checks (footprint links, parity) compare the
        # engines, not two installs
        for var, sub in (("KICAD10_SYMBOL_DIR", "symbols"), ("KICAD10_FOOTPRINT_DIR", "footprints"),
                         ("KICAD10_3DMODEL_DIR", "3dmodels"), ("KICAD10_TEMPLATE_DIR", "template")):
            env[var] = str(LIBS / sub)
    # With --libs, that folder's own global tables (a release's share/kicad has them), so both
    # find the same libraries; else this KiCad's template tables
    template = LIBS if LIBS and (LIBS / "fp-lib-table").is_file() else template_dir(cli)
    if template:
        for version in ("10.0", "10.99"):
            config = home / "config/kicad" / version
            config.mkdir(parents=True, exist_ok=True)
            for table in ("sym-lib-table", "fp-lib-table", "design-block-lib-table"):
                if (template / table).is_file():
                    shutil.copy(template / table, config / table)
    else:
        print(f"note: no template folder found for {cli}; library tables not seeded", flush=True)
    return env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--stock-cli", type=Path)
    parser.add_argument("--libs", type=Path,
                        help="a KiCad 10 share folder with symbols/ and footprints/, used by both CLIs "
                             "(without it each uses whatever its install has, often nothing)")
    parser.add_argument("--project", action="append", type=Path)
    parser.add_argument("--keep", type=Path)
    args = parser.parse_args()
    global LIBS
    LIBS = args.libs.resolve() if args.libs else None
    if not args.stock_cli:
        print("SKIP export-to-stock gate: no --stock-cli")
        return 0
    version = subprocess.check_output([str(args.stock_cli), "--version"], text=True).strip()
    print(f"stock kicad-cli {version}", flush=True)
    with tempfile.TemporaryDirectory(prefix="backplane-stock-export-") as temp:
        work = Path(temp)
        fixtures = work / "fixtures"
        fixtures.mkdir()
        projects = args.project or ([DEMOS / name for name in DEFAULT_PROJECTS] + [make_hubble_fixture(fixtures)])
        fork_env = private_env(work / "fork-home", args.kicad_cli)
        stock_env = private_env(work / "stock-home", args.stock_cli)
        for project in projects:
            gate_project(args.kicad_cli, args.stock_cli, project.resolve(), work, fork_env, stock_env)
        if args.keep:
            shutil.copytree(work, args.keep, dirs_exist_ok=True)
    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
