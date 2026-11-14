#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""While an API task runs, KiCad answers only its allow-listed reads; every change is refused.

A board check run (RunChecks FK_PCB, parity and refill_zones) serves the API from inside the zone
fill and the DRC (KICAD_API_SERVER::ServeDuringTask).  This check sends one request of each
changing kind while the refill runs and again while the DRC's providers run, each one a real
change that would show if it got through:

  DeleteItems (a footprint), BeginCommit, RunAction (pcbnew.ZoneFiller.zoneUnfillAll),
  embed ShowFrame (the schematic), embed SetHostMarkers, SaveDocument (the board), ApprovePlan

and requires every answer to be AS_BUSY naming the task.  Then the run's DRC markers (type,
message, items, position), the board's items and the board file are compared with an undisturbed
run before and after it (A, disturbed B, C: A == B == C).  After the run a commit is accepted
again.

    backplane-embed-task-gate-check.py <kicad> <kicad-cli> [--project DIR] [--out DIR]
"""

import argparse
import hashlib
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import (FAILURES, IpcSession, Session, check,  # noqa: E402
                                          viewable, wait_for)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--project", type=Path, default=Path(__file__).resolve().parents[1] / "demos/video")
    parser.add_argument("--out", type=Path, default=Path("embed-task-gate-out"))
    args = parser.parse_args()

    with Session(args.kicad, args.out, args.project.resolve()) as s, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=600000) as api:
        embed = api.module("backplane.embed")
        base = api.module("common.types.base_types")
        editor = api.module("common.commands.editor_commands")
        project = api.module("common.commands.project_commands")
        plan = api.module("backplane.plan")
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
        check(wait_for(lambda: viewable(s.host.display, frame.window), 180), "the board is shown")
        time.sleep(3)
        doc = api.request(editor.GetOpenDocuments(type=base.DOCTYPE_PCB),
                          editor.GetOpenDocumentsResponse).documents[0]
        header = base.ItemHeader(document=doc)
        board_file = next(s.project_dir.glob("*.kicad_pcb"))

        def items():
            reply = api.request(editor.GetItems(header=header), editor.GetItemsResponse)
            return reply.items

        footprint = next(i for i in items() if i.type_url.endswith("FootprintInstance"))
        fp_id = api.module("board.board_types").FootprintInstance()
        footprint.Unpack(fp_id)

        def board_digest():
            h = hashlib.sha256()
            for blob in sorted(i.SerializeToString(deterministic=True) for i in items()):
                h.update(blob)
            return h.hexdigest()[:16]

        def markers_digest():
            found = api.request(embed.GetMarkers(kind=embed.FK_PCB), embed.GetMarkersResponse).markers
            rows = sorted((m.type, m.message, tuple(sorted(i.value for i in m.items)), m.severity, m.layer,
                           m.rule, m.at_nm.x_nm, m.at_nm.y_nm) for m in found)
            return hashlib.sha256(repr(rows).encode()).hexdigest()[:16], len(rows)

        writes = [
            ("DeleteItems", lambda: editor.DeleteItems(header=header, item_ids=[fp_id.id])),
            ("BeginCommit", lambda: editor.BeginCommit(header=header)),
            ("RunAction", lambda: editor.RunAction(action="pcbnew.ZoneFiller.zoneUnfillAll")),
            ("ShowFrame", lambda: embed.ShowFrame(kind=embed.FK_SCH, x=0, y=0, width=w, height=h)),
            ("SetHostMarkers", lambda: embed.SetHostMarkers(kind=embed.FK_PCB, set="task-gate")),
            ("SaveDocument", lambda: project.SaveDocument(document=doc)),
            ("ApprovePlan", lambda: plan.ApprovePlan(board=doc, approve=True)),
        ]

        def run(label, disturb):
            events()
            mtime = board_file.stat().st_mtime_ns
            pid = api.request(embed.RunChecks(kind=embed.FK_PCB, parity=True, refill_zones=True),
                              embed.RunChecksResponse).progress_id
            sent = {}           # phase class -> [(write, status, message)]
            detail, done, t0 = "", False, time.monotonic()
            while not done and time.monotonic() - t0 < 600:
                for e in events():
                    if e.type == embed.ET_PROGRESS and e.progress.id == pid:
                        detail = e.progress.detail or e.progress.title
                    elif e.type == embed.ET_CHECKS_DONE:
                        done = True
                if done or not disturb:
                    time.sleep(0.02)
                    continue
                phase = "refill" if "fill" in detail.lower() else \
                        "drc" if detail.startswith("Checking") else None
                if phase and phase not in sent:
                    sent[phase] = []
                    for name, make in writes:
                        r = api.request_raw(make())
                        sent[phase].append((name, r.status.status, r.status.error_message, detail))
            check(done, f"{label}: the run ends ({time.monotonic() - t0:.1f} s)")
            time.sleep(1)
            md, n = markers_digest()
            return {"markers": md, "count": n, "board": board_digest(),
                    "saved": board_file.stat().st_mtime_ns != mtime, "sent": sent}

        a = run("A undisturbed", False)
        b = run("B disturbed", True)
        c = run("C undisturbed", False)

        for phase in ("refill", "drc"):
            got = b["sent"].get(phase)
            check(got is not None, f"B: writes were sent during the {phase} "
                                   f"({'at ' + repr(got[0][3]) if got else 'its phase was never seen'})")
            for name, status, message, _ in got or []:
                check(status == api.envelope.AS_BUSY and "a task is running:" in message,
                      f"B {phase}: {name} is answered AS_BUSY naming the task ({status}: {message[:110]})")

        print(f"A {a['markers']} ({a['count']} markers) board {a['board']}; B {b['markers']} ({b['count']}) "
              f"board {b['board']}; C {c['markers']} ({c['count']}) board {c['board']}", flush=True)
        check(a["markers"] == c["markers"] and a["board"] == c["board"],
              "the undisturbed runs agree (A == C), so a difference in B would be the writes'")
        check(b["markers"] == a["markers"], f"the disturbed run's DRC markers equal the undisturbed run's "
                                            f"({b['count']} vs {a['count']})")
        check(b["board"] == a["board"], "the board's items after the disturbed run equal the undisturbed run's")
        check(not b["saved"], "the board file was not saved during the disturbed run")
        shown = {f.kind for f in api.request(embed.GetFrames(), embed.GetFramesResponse).frames if f.shown}
        check(embed.FK_SCH not in shown, f"the schematic was not shown ({sorted(shown)})")

        after = api.request_raw(editor.BeginCommit(header=header))
        check(after.status.status == api.envelope.AS_OK, f"after the run a commit is accepted again ({after.status.status})")
        if after.status.status == api.envelope.AS_OK:
            begin = editor.BeginCommitResponse()
            after.message.Unpack(begin)
            api.request_raw(editor.EndCommit(header=header, id=begin.id, action=editor.CMA_DROP))
        s.shot("task-gate")

    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
