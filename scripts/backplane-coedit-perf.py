#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The agent edit path's cost in the person's editor (docs/backplane/coedit.md).

Embedded KiCad with BACKPLANE_KICAD_FRAMETRACE: an agent makes --edits commits (a footprint move
and a new track each), --gap seconds apart.  Reports the ipc:* spans, and for the --window seconds
after each edit the gal_redraw spans of the PCB frame (count, total ms) and the longest UI loop:
the flash must be overlay-only and stop with its fade.

    backplane-coedit-perf.py KICAD KICAD_CLI --out DIR [--edits 20] [--gap 2.5] [--window 2.0]
"""

import argparse
import json
import os
from pathlib import Path
import statistics
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_coedit_support import EmbeddedKiCad  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("kicad")
    ap.add_argument("kicad_cli")
    ap.add_argument("--out", default="coedit-perf-out")
    ap.add_argument("--edits", type=int, default=20)
    ap.add_argument("--gap", type=float, default=2.5)
    ap.add_argument("--window", type=float, default=2.0)
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    trace = (out / "frametrace.jsonl").resolve()
    os.environ["BACKPLANE_KICAD_FRAMETRACE"] = str(trace)

    edits = []
    with EmbeddedKiCad(a.kicad, a.kicad_cli, "demos/pic_programmer", out) as k:
        m = k.m
        agent = k.session("agent:tperf")
        req = m["editor"].GetItems()
        req.header.document.CopyFrom(k.doc)
        req.types.append(m["enums"].KOT_PCB_FOOTPRINT)
        fps = []
        for item in agent.request(req, m["editor"].GetItemsResponse).items:
            fp = m["board"].FootprintInstance()
            item.Unpack(fp)
            fps.append(fp)
        time.sleep(3.0)
        for i in range(a.edits):
            fp = fps[i % len(fps)]
            fp.position.x_nm += 100_000
            begin = agent.request(m["editor"].BeginCommit(header=m["types"].ItemHeader(document=k.doc)),
                                  m["editor"].BeginCommitResponse)
            up = m["editor"].UpdateItems()
            up.header.document.CopyFrom(k.doc)
            up.items.add().Pack(fp)
            agent.request_raw(up)
            t = m["board"].Track()
            t.start.x_nm, t.start.y_nm = 100_000_000 + i * 1_000_000, 120_000_000
            t.end.x_nm, t.end.y_nm = 100_000_000 + i * 1_000_000, 125_000_000
            t.width.value_nm, t.layer = 250_000, m["board"].BL_B_Cu
            cr = m["editor"].CreateItems()
            cr.header.document.CopyFrom(k.doc)
            cr.items.add().Pack(t)
            agent.request_raw(cr)
            at = time.time() * 1000
            agent.request(m["editor"].EndCommit(header=m["types"].ItemHeader(document=k.doc), id=begin.id,
                                                action=m["editor"].CMA_COMMIT, message=f"perf edit {i}"),
                          m["editor"].EndCommitResponse)
            edits.append(at)
            time.sleep(a.gap)

    loops, spans = [], []
    for line in trace.read_text().splitlines():
        try:
            rec = json.loads(line)
        except ValueError:
            continue
        (loops if rec.get("kind") == "loop" else spans if rec.get("kind") == "span" else []).append(rec)

    def stats(values):
        return {"count": len(values), "median": round(statistics.median(values), 2) if values else 0,
                "max": round(max(values), 2) if values else 0}

    summary = {"ipc": {}}
    for sp in spans:
        if sp["what"].startswith("ipc:"):
            summary["ipc"].setdefault(sp["what"], []).append(sp["ms"])
    summary["ipc"] = {k: stats(v) for k, v in summary["ipc"].items()}

    per_edit = []
    for at in edits:
        window = [sp for sp in spans if sp["what"].startswith("gal_redraw") and "Pcb" in sp["what"]
                  and at <= sp["t"] <= at + a.window * 1000]
        lw = [lp["ms"] for lp in loops if at <= lp["t"] <= at + a.window * 1000]
        per_edit.append({"redraws": len(window), "redraw_ms": round(sum(sp["ms"] for sp in window), 1),
                         "max_redraw_ms": round(max((sp["ms"] for sp in window), default=0), 1),
                         "max_loop_ms": round(max(lw, default=0), 1)})
    summary["after_each_edit"] = {
        "redraws": stats([e["redraws"] for e in per_edit]),
        "redraw_ms_total": stats([e["redraw_ms"] for e in per_edit]),
        "max_redraw_ms": stats([e["max_redraw_ms"] for e in per_edit]),
        "max_loop_ms": stats([e["max_loop_ms"] for e in per_edit]),
    }
    (out / "summary.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
