#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Memory budget per KiCad process for `kicad --embedded` and the headless api-server.

Samples RSS, PSS and anonymous memory (/proc/<pid>/smaps_rollup) at fixed steps:
  embedded GUI: start, board open (settled), schematic, footprint editor, symbol editor,
                3D viewer, after an in-editor DRC (RunChecks), 20 s after it;
  headless:     `kicad-cli api-server` after it opens the same board.
Each step is compared with scripts/backplane-memory-budget.json (MB per process and step, set from
a clean measurement, not guessed). Over budget is a failure; with --record the measured values
(plus 15 %) are written as the new budget instead.

    backplane-embed-memory-check.py <kicad> <kicad-cli> [--project DIR] [--out DIR] [--record]
"""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import (FAILURES, IpcSession, Session, check,  # noqa: E402
                                          viewable, wait_for)

BUDGET = Path(__file__).resolve().parent / "backplane-memory-budget.json"


def mem(pid):
    out = {}
    with open(f"/proc/{pid}/smaps_rollup") as f:
        for line in f:
            m = re.match(r"(Rss|Pss|Anonymous):\s+(\d+)", line)
            if m:
                out[m.group(1).lower()] = int(m.group(2)) // 1024
    return out


def settle(seconds=3.0):
    time.sleep(seconds)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--project", type=Path)
    parser.add_argument("--out", type=Path, default=Path("embed-memory-out"))
    parser.add_argument("--record", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    project = args.project or root / "demos/video"
    args.out.mkdir(parents=True, exist_ok=True)
    samples = {"gui": {}, "headless": {}}

    with Session(args.kicad, args.out / "gui", project) as s, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=600000) as api:
        embed = api.module("backplane.embed")
        w, h = s.host.size
        pid = s.kicad_process.pid
        since = [0]

        def events():
            reply = api.request(embed.NextEvents(since=since[0]), embed.NextEventsResponse)
            for e in reply.events:
                since[0] = max(since[0], e.seq)
            return list(reply.events)

        def show(kind):
            for f in api.request(embed.GetFrames(), embed.GetFramesResponse).frames:
                if f.shown and f.kind != kind:
                    api.request(embed.HideFrame(window=f.window), embed.HideFrameResponse)
            r = api.request(embed.ShowFrame(kind=kind, x=0, y=0, width=w, height=h), embed.ShowFrameResponse)
            wait_for(lambda: viewable(s.host.display, r.frame.window), 180)
            settle()

        samples["gui"]["start"] = mem(pid)
        show(embed.FK_PCB)
        settle(10)
        samples["gui"]["board"] = mem(pid)
        for kind, step in ((embed.FK_SCH, "schematic"), (embed.FK_FOOTPRINT_EDITOR, "footprint_editor"),
                           (embed.FK_SYMBOL_EDITOR, "symbol_editor"), (embed.FK_PCB_3D, "3d")):
            show(kind)
            samples["gui"][step] = mem(pid)
        show(embed.FK_PCB)
        events()
        api.request(embed.RunChecks(kind=embed.FK_PCB, parity=True), embed.RunChecksResponse)
        wait_for(lambda: any(e.type == embed.ET_CHECKS_DONE for e in events()), 900, 0.5)
        samples["gui"]["after_drc"] = mem(pid)
        settle(20)
        samples["gui"]["after_drc_20s"] = mem(pid)

    # Headless: the api-server with the same board open
    pro = next(project.glob("*.kicad_pro"))
    with tempfile.TemporaryDirectory() as work:
        sock = Path(work) / "api.sock"
        server = subprocess.Popen([str(args.kicad_cli), "api-server", "--socket", str(sock),
                                   str(pro.with_suffix(".kicad_pcb"))],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            wait_for(sock.exists, 60)
            with IpcSession(args.kicad_cli, api_socket=sock, timeout=600000) as head:
                editor = head.module("common.commands.editor_commands")
                base = head.module("common.types.base_types")
                wait_for(lambda: head.request_raw(editor.GetOpenDocuments(type=base.DOCTYPE_PCB)).status.status
                         == head.envelope.AS_OK, 120, 1)
                settle(5)
                samples["headless"]["board"] = mem(server.pid)
        finally:
            server.terminate()
            server.wait(30)

    (args.out / "memory.json").write_text(json.dumps(samples, indent=2))
    print(json.dumps(samples, indent=2))
    key = project.name

    if args.record:
        budget = json.loads(BUDGET.read_text()) if BUDGET.exists() else {}
        budget[key] = {proc: {step: round(v["rss"] * 1.15) for step, v in steps.items()}
                       for proc, steps in samples.items()}
        BUDGET.write_text(json.dumps(budget, indent=2, sort_keys=True) + "\n")
        print(f"recorded the budget for {key} in {BUDGET}")
        return 0

    budget = json.loads(BUDGET.read_text()).get(key) if BUDGET.exists() else None
    check(budget is not None, f"a memory budget exists for {key} ({BUDGET.name}; --record writes one)")
    for proc, steps in (budget or {}).items():
        for step, limit in steps.items():
            got = samples.get(proc, {}).get(step, {}).get("rss")
            check(got is not None and got <= limit,
                  f"{key} {proc} {step}: RSS {got} MB within its budget of {limit} MB")
    print(f"\n{len(FAILURES)} failure(s)")
    if not FAILURES:
        print("ALL PASSED")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
