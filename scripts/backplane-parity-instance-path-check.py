#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""kicad-cli's DRC schematic parity resolves symbol references as the editor does when the
schematic's instance paths are not rooted at the root file's own UUID.

The editor (and `sch export netlist`) roots the hierarchy at the project's top-level sheet UUID
(.kicad_pro schematic.top_level_sheets) and resolves instances stored under another root.  The
parity netlist (KIFACE_NETLIST_SCHEMATIC, also the API's headless DRC) used a loader that did
neither, so references came out empty and unconnected nets were named by symbol UUID
("unconnected-(0a4cc99b-...-PadD6)"): false net_conflict / extra footprint rows.

Variants of qa/data/pcbnew/issue21184 (hierarchical, has unconnected-(U1-NC-Pad8)):
  base     the fixture as is
  old      project top-level UUID differs from the file's; paths still at the file's UUID
  rekeyed  paths rewritten to the project's UUID (what any save in the editor writes)
Each must give no parity row, and the run must not write any schematic file.

    backplane-parity-instance-path-check.py KICAD_CLI [--out DIR]
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

PROJECT_ROOT = "6a7b3c1e-2222-4333-8444-955566667777"
FAILURES = []


def check(condition, message):
    print(("PASS " if condition else "FAIL ") + message)
    if not condition:
        FAILURES.append(message)


def make(source, dest, variant):
    shutil.copytree(source, dest)
    if variant == "base":
        return
    root = dest / "issue21184.kicad_sch"
    file_uuid = re.search(r'\(uuid "([^"]+)"\)', root.read_text()).group(1)
    pro = dest / "issue21184.kicad_pro"
    data = json.loads(pro.read_text())
    data.setdefault("schematic", {})["top_level_sheets"] = [
        {"filename": "issue21184.kicad_sch", "name": "issue21184", "uuid": PROJECT_ROOT}]
    pro.write_text(json.dumps(data, indent=2))
    if variant == "rekeyed":
        for sch in dest.glob("*.kicad_sch"):
            sch.write_text(sch.read_text().replace(f'(path "/{file_uuid}', f'(path "/{PROJECT_ROOT}'))


def snapshot(dest):
    return {p.name: (p.stat().st_mtime_ns, hashlib.sha256(p.read_bytes()).hexdigest())
            for p in sorted(dest.glob("*.kicad_sch"))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / "qa/data/pcbnew/issue21184"
    work = Path(tempfile.mkdtemp(prefix="parity-instance-path-"))
    out = args.out or work

    for variant in ("base", "old", "rekeyed"):
        dest = work / variant
        make(source, dest, variant)
        before = snapshot(dest)
        report = out / f"{variant}.json"
        run = subprocess.run([str(args.kicad_cli), "pcb", "drc", "--severity-all", "--schematic-parity",
                              "--format", "json", "-o", str(report), str(dest / "issue21184.kicad_pcb")],
                             capture_output=True, text=True, timeout=600)
        check(run.returncode == 0 and report.exists(), f"{variant}: kicad-cli pcb drc ran ({run.returncode})")
        if not report.exists():
            continue
        parity = json.loads(report.read_text()).get("schematic_parity", [])
        uuid_named = [v["description"] for v in parity
                      if re.search(r"\((unconnected|Net)-\([0-9a-f]{8}-", v.get("description", ""))]
        check(not parity, f"{variant}: no schematic parity rows ({len(parity)}: "
                          f"{[v['description'][:100] for v in parity[:3]]})")
        check(not uuid_named, f"{variant}: no net named by a symbol UUID ({len(uuid_named)})")
        check(snapshot(dest) == before, f"{variant}: no schematic file written (bytes and mtime)")

    if not args.out:
        shutil.rmtree(work, ignore_errors=True)
    print("FAILED: %d" % len(FAILURES) if FAILURES else "ALL PASS")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
