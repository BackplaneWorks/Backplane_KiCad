#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""How long `kicad --embedded` leaves API requests unanswered during a host's check run.

The hub drives a board check as it does in use: the board shown, RunChecks FK_PCB with parity and
refill_zones.  While the run goes on, one client sends Ping and NextEvents back to back and stamps
every answer.  A silent stretch is the time between two answers; during it KiCad answered nothing
(reads, the hub's Ping and the person's own input all wait for the same main-thread yields).

Each stretch is put down to the run's phase, from the ET_PROGRESS events (title/detail): the phase
seen before the stretch, and the one seen at its end when the run moved on inside it ("A -> B").
Per phase: its length (first seen to the next phase first seen) and its longest stretch.

The longest stretch per board is compared with scripts/backplane-responsiveness-budget.json (ms,
set from a clean measurement with --record: the measured maximum plus 15 %, at least 50 ms), and
with --ceiling-ms (default 300: longer reads as a stuck KiCad to a host).

    backplane-embed-responsiveness-check.py <kicad> <kicad-cli> [--project DIR ...] [--runs N]
                                            [--out DIR] [--record] [--ceiling-ms MS]
"""

import argparse
from collections import OrderedDict
import json
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import (FAILURES, IpcSession, Session, check,  # noqa: E402
                                          viewable, wait_for)

BUDGET = Path(__file__).resolve().parent / "backplane-responsiveness-budget.json"


def measure(args, project, out):
    """[(total_s, max_gap_s, phases)] per run; phases: name -> [length_s, max_gap_s, first_seen]."""
    runs = []
    with Session(args.kicad, out, project) as s, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=600000) as api:
        embed = api.module("backplane.embed")
        base_cmds = api.module("common.commands.base_commands")
        w, h = s.host.size
        since = [0]

        def events():
            reply = api.request(embed.NextEvents(since=since[0]), embed.NextEventsResponse)
            for e in reply.events:
                since[0] = max(since[0], e.seq)
            return list(reply.events)

        for f in api.request(embed.GetFrames(), embed.GetFramesResponse).frames:
            if f.shown and f.kind != embed.FK_PCB:
                api.request(embed.HideFrame(window=f.window), embed.HideFrameResponse)
        frame = api.request(embed.ShowFrame(kind=embed.FK_PCB, x=0, y=0, width=w, height=h),
                            embed.ShowFrameResponse).frame
        check(wait_for(lambda: viewable(s.host.display, frame.window), 180), f"{project.name}: the board is shown")
        time.sleep(3)

        for run in range(1, args.runs + 1):
            events()
            t_run = time.monotonic()
            pid = api.request(embed.RunChecks(kind=embed.FK_PCB, parity=True, refill_zones=True),
                              embed.RunChecksResponse).progress_id
            last = time.monotonic()
            phase, done = "(before the first progress)", False
            phases = OrderedDict()
            stretches = []
            ping = True
            while not done and time.monotonic() - t_run < 600:
                if args.probe_interval_ms > 0:
                    time.sleep(args.probe_interval_ms / 1000)
                if ping:
                    api.request_raw(base_cmds.Ping())
                    seen = []
                else:
                    seen = events()
                ping = not ping
                now = time.monotonic()
                gap = now - last
                before = phase
                for e in seen:
                    if e.type == embed.ET_PROGRESS and e.progress.id == pid:
                        name = e.progress.detail or e.progress.title or "(unnamed)"
                        if name != phase:
                            phase = name
                            phases.setdefault(phase, [0.0, 0.0, now - t_run])
                    elif e.type == embed.ET_CHECKS_DONE:
                        done = True
                label = before if phase == before else f"{before} -> {phase}"
                stretches.append((gap, label, last - t_run))
                if before in phases:
                    phases[before][1] = max(phases[before][1], gap)
                if phase != before and phase in phases:
                    phases[phase][1] = max(phases[phase][1], gap)
                last = now
            total = time.monotonic() - t_run
            names = list(phases)
            for i, name in enumerate(names):
                end = phases[names[i + 1]][2] if i + 1 < len(names) else total
                phases[name][0] = end - phases[name][2]
            check(done, f"{project.name} run {run}: ET_CHECKS_DONE within 600 s ({total:.1f} s)")
            worst = max(stretches, default=(0.0, "", 0.0))
            runs.append((total, worst[0], phases))
            print(f"{project.name} run {run}: {total:.2f} s, {len(stretches)} answers, longest silence "
                  f"{worst[0] * 1000:.0f} ms at +{worst[2]:.2f} s in {worst[1]}", flush=True)
            for gap, label, at in sorted(stretches, reverse=True)[:5]:
                print(f"  silence {gap * 1000:6.0f} ms at +{at:6.2f} s  {label}", flush=True)
            for name, (length, mgap, first) in phases.items():
                print(f"  phase +{first:6.2f} s {length * 1000:7.0f} ms long, longest silence "
                      f"{mgap * 1000:6.0f} ms  {name}", flush=True)
            s.shot(f"{project.name}-run{run}")
    return runs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--project", type=Path, action="append",
                        help="project folder (repeatable); default demos/video")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--out", type=Path, default=Path("embed-responsiveness-out"))
    parser.add_argument("--record", action="store_true")
    parser.add_argument("--ceiling-ms", type=float, default=300)
    parser.add_argument("--probe-interval-ms", type=float, default=0,
                        help="pause between probe requests (0: back to back); with a long pause the "
                             "run's own length is measured with next to no probe load")
    args = parser.parse_args()
    projects = args.project or [Path(__file__).resolve().parents[1] / "demos/video"]

    measured = {}
    for project in projects:
        project = project.resolve()
        runs = measure(args, project, args.out / project.name)
        if runs:
            measured[project.name] = max(r[1] for r in runs)

    if args.record:
        budget = json.loads(BUDGET.read_text()) if BUDGET.exists() else {}
        for key, worst in measured.items():
            budget[key] = {"max_silence_ms": max(50, round(worst * 1000 * 1.15)),
                           "measured_ms": round(worst * 1000)}
        BUDGET.write_text(json.dumps(budget, indent=2, sort_keys=True) + "\n")
        print(f"recorded {sorted(measured)} in {BUDGET}")
    else:
        budget = json.loads(BUDGET.read_text()) if BUDGET.exists() else {}
        for key, worst in measured.items():
            limit = budget.get(key, {}).get("max_silence_ms")
            check(limit is not None, f"a responsiveness budget exists for {key} ({BUDGET.name}; --record writes one)")
            if limit is not None:
                check(worst * 1000 <= limit,
                      f"{key}: longest silence during a check run {worst * 1000:.0f} ms within its budget of {limit} ms")
    for key, worst in measured.items():
        check(worst * 1000 <= args.ceiling_ms,
              f"{key}: longest silence during a check run {worst * 1000:.0f} ms under the {args.ceiling_ms:.0f} ms ceiling")

    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
