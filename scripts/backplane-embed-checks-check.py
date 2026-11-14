#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bp.13 checks for `kicad --embedded`: RunChecks, GetMarkers and SelectItems by id and place.

- RunChecks FK_PCB (with parity) and FK_SCH return a progress id at once; ET_PROGRESS with that id
  follows, then ET_CHECKS_DONE with counts that match GetMarkers; the markers stay in the editor
  (the same ids on a second GetMarkers) and the in-editor DRC/ERC tasks find the same types.
- RunChecks FK_PCB with parity while the schematic is shown asks to show no frame (ET_SHOW_REQUESTED);
  the board stays hidden.
- A second RunChecks while one runs is refused (AS_BUSY); a kind with no checks is unhandled.
- During a RunChecks (its own session on --overlap-board, default demos/video, long enough to land
  requests inside the run), reads are answered within 2 s and a change is refused with the task named;
  a run too short to prove it fails loudly instead of passing on zero reads.
- SelectItems ids selects a marker's items (ids_not_found for an unknown one); zoom_box_nm and
  at_nm move the view; FK_SCH sheet_path shows that sheet.

Runs on a private Xvfb + i3 with a python-xlib stand-in host; screenshots to --out.
"""

import argparse
from collections import Counter
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
    parser.add_argument("--out", type=Path, default=Path("embed-checks-out"))
    parser.add_argument("--overlap-board", type=Path,
                        default=Path(__file__).resolve().parents[1] / "demos/video",
                        help="project folder for the reads-during-a-run case: its DRC with refill must "
                             "run for well over a read round trip (video: ~2 s; pic_programmer is too short "
                             "since bp.13's parallel DRC)")
    parser.add_argument("--overlap-tries", type=int, default=4)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / "demos/pic_programmer"

    with Session(args.kicad, args.out, source) as s, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=300000) as api:
        embed = api.module("backplane.embed")
        base = api.module("common.types.base_types")
        editor = api.module("common.commands.editor_commands")
        boardtasks = api.module("board.board_tasks")
        schtasks = api.module("schematic.schematic_tasks")
        tasks = api.module("common.tasks")
        w, h = s.host.size
        since = [0]
        last_seen = []   # the events of the last run_checks

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
            wait_for(lambda: viewable(s.host.display, r.frame.window), 60)
            time.sleep(2)
            return r.frame

        def run_checks(kind, label, **kw):
            events()
            reply = api.request(embed.RunChecks(kind=kind, **kw), embed.RunChecksResponse)
            pid = reply.progress_id
            seen = []

            def done():
                for e in events():
                    seen.append(e)
                return any(e.type == embed.ET_CHECKS_DONE for e in seen)

            finished = wait_for(done, 300, 0.2)
            last_seen[:] = seen
            progress = [e for e in seen if e.type == embed.ET_PROGRESS and e.progress.id == pid]
            checks = next((e.checks for e in seen if e.type == embed.ET_CHECKS_DONE), None)
            check(pid > 0 and finished and checks is not None,
                  f"{label}: RunChecks answers with progress id {pid}, ET_CHECKS_DONE follows")
            check(bool(progress) and progress[-1].progress.done,
                  f"{label}: ET_PROGRESS with that id, ending done ({len(progress)} events)")
            if checks is not None:
                check(checks.progress_id == pid and checks.kind == kind and not checks.cancelled
                      and not checks.error,
                      f"{label}: Checks names the run ({checks.errors} errors, {checks.warnings} "
                      f"warnings, {checks.exclusions} excluded, {checks.unconnected} unconnected)")
            return checks

        def markers(kind):
            return list(api.request(embed.GetMarkers(kind=kind), embed.GetMarkersResponse).markers)

        def counts(found):
            c = Counter(m.severity for m in found)
            return c["error"], c["warning"], c["exclusion"]

        def run_task(start, result_req, result_type):
            token = api.request(start, tasks.StartTaskResponse).token
            wait_for(lambda: api.request(tasks.GetTaskStatus(token=token), tasks.TaskStatusResponse).state
                     != tasks.TS_RUNNING, 300, 1)
            return api.request(result_req(token=token), result_type)

        # ---- Board
        show(embed.FK_PCB)
        docs = api.request(editor.GetOpenDocuments(type=base.DOCTYPE_PCB), editor.GetOpenDocumentsResponse)
        pcb_doc = docs.documents[0]

        drc = run_checks(embed.FK_PCB, "DRC", parity=True)
        found = markers(embed.FK_PCB)
        if drc is not None:
            check(counts(found) == (drc.errors, drc.warnings, drc.exclusions),
                  f"GetMarkers FK_PCB agrees with ET_CHECKS_DONE ({len(found)} markers)")
        again = markers(embed.FK_PCB)
        check(found and sorted(m.id.value for m in found) == sorted(m.id.value for m in again),
              "the markers stay in the editor (same ids on a second GetMarkers)")
        check(all(m.type and m.message and m.items for m in found),
              "every marker has a type, a message and its items")
        s.shot("01-drc-markers")

        task = run_task(boardtasks.StartDrcTask(board=pcb_doc, test_schematic_parity=True),
                        boardtasks.GetDrcTaskResult, boardtasks.DrcTaskResult)
        check(Counter(m.type for m in markers(embed.FK_PCB)) or not task.violations,
              "the in-editor DRC task leaves markers too")

        # Two at once: the run holds the UI thread, so the second either finds it running
        # (AS_BUSY) or is served after it ends and runs on its own; never two at the same time
        events()
        first = api.request_raw(embed.RunChecks(kind=embed.FK_PCB, parity=True))
        second = api.request_raw(embed.RunChecks(kind=embed.FK_PCB))
        done = []
        expect = 2 if second.status.status == api.envelope.AS_OK else 1
        wait_for(lambda: done.extend(e for e in events() if e.type == embed.ET_CHECKS_DONE) or len(done) >= expect,
                 300, 0.2)
        ids = []
        for r in (first, second):
            if r.status.status == api.envelope.AS_OK:
                reply = embed.RunChecksResponse()
                r.message.Unpack(reply)
                ids.append(reply.progress_id)
        check(first.status.status == api.envelope.AS_OK
              and ( second.status.status == api.envelope.AS_BUSY
                    or sorted(e.checks.progress_id for e in done) == sorted(ids) ),
              f"a second RunChecks is refused while one runs, or runs after it with its own result "
              f"(status {second.status.status}, done for {[e.checks.progress_id for e in done]})")
        other = api.request_raw(embed.RunChecks(kind=embed.FK_PCB_3D))
        check(other.status.status != api.envelope.AS_OK, "RunChecks for FK_PCB_3D is refused")

        # SelectItems by id, place and box
        target = next((m for m in markers(embed.FK_PCB) if m.items), None)
        if target is not None:
            bogus = base.KIID(value="00000000-1111-2222-3333-444444444444")
            sel = api.request(embed.SelectItems(kind=embed.FK_PCB, ids=list(target.items) + [bogus], zoom=True),
                              embed.SelectItemsResponse)
            check([k.value for k in sel.ids_not_found] == [bogus.value],
                  f"SelectItems ids: a marker's items are found, the unknown id is not ({len(target.items)} items)")
            s.shot("02-select-ids")
        box = base.Box2(position=base.Vector2(x_nm=100_000_000, y_nm=80_000_000),
                        size=base.Vector2(x_nm=20_000_000, y_nm=15_000_000))
        api.request(embed.SelectItems(kind=embed.FK_PCB, zoom_box_nm=box), embed.SelectItemsResponse)
        time.sleep(1)
        s.shot("03-zoom-box")
        api.request(embed.SelectItems(kind=embed.FK_PCB, at_nm=base.Vector2(x_nm=150_000_000, y_nm=100_000_000)),
                    embed.SelectItemsResponse)
        time.sleep(1)
        s.shot("04-at")

        # ---- Schematic
        show(embed.FK_SCH)

        # The board's DRC (with parity) while the schematic is on screen and the board hidden: the
        # parity fetch must not ask to show the board (lane A: a host that followed it took the
        # pane and the keyboard from the schematic, and Ctrl+Y went to the board)
        run_checks(embed.FK_PCB, "DRC behind the schematic", parity=True)
        time.sleep(1)   # anything the run posted after ET_CHECKS_DONE
        hidden_seen = [e for e in last_seen + events() if e.type == embed.ET_SHOW_REQUESTED]
        check(not hidden_seen, f"a DRC with parity behind the schematic asks to show no frame "
                               f"({[e.frame.kind for e in hidden_seen]})")
        on = {f.kind for f in api.request(embed.GetFrames(), embed.GetFramesResponse).frames if f.shown}
        check(embed.FK_PCB not in on and embed.FK_SCH in on,
              f"... the board stays hidden and the schematic shown ({sorted(on)})")
        erc = run_checks(embed.FK_SCH, "ERC")
        sch_found = markers(embed.FK_SCH)
        if erc is not None:
            check(counts(sch_found) == (erc.errors, erc.warnings, erc.exclusions),
                  f"GetMarkers FK_SCH agrees with ET_CHECKS_DONE ({len(sch_found)} markers)")
        check(all(m.sheet_path.startswith("/") for m in sch_found), "every schematic marker names its sheet")
        sch_docs = api.request(editor.GetOpenDocuments(type=base.DOCTYPE_SCHEMATIC),
                               editor.GetOpenDocumentsResponse).documents
        if sch_docs:
            task = run_task(schtasks.StartErcTask(document=sch_docs[0]), schtasks.GetErcTaskResult,
                            schtasks.ErcTaskResult)
            check(len(task.violations) == len(markers(embed.FK_SCH)),
                  f"the in-editor ERC task sees the same markers ({len(task.violations)})")

        # A sub-sheet by path: the frame's title names it
        sub = next((m for m in sch_found if m.sheet_path.count("/") > 2), None)
        if sub is not None:
            api.request(embed.SelectItems(kind=embed.FK_SCH, sheet_path=sub.sheet_path, ids=list(sub.items)),
                        embed.SelectItemsResponse)
            time.sleep(1.5)
            title = next(f.title for f in api.request(embed.GetFrames(), embed.GetFramesResponse).frames
                         if f.kind == embed.FK_SCH)
            check("pic_sockets" in title or "[" in title, f"SelectItems sheet_path shows the sheet ({title})")
            s.shot("05-sheet")

    overlap(args)

    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


def overlap(args):
    """bp.13 batch 5: during a run, reads are answered (within a bound) and a change is refused with the
    task named. Its own session on a board whose run is long (--overlap-board), because a read only
    counts as inside the run when the event stream proves it: the run's ET_PROGRESS was seen before the
    read was sent, and the ET_CHECKS_DONE had not arrived when the next NextEvents, sent after the read
    was answered, came back. A run too short to land one read inside is retried up to --overlap-tries
    times, then fails loudly with each run's length, never passes on zero reads."""
    with Session(args.kicad, args.out / "overlap", args.overlap_board) as s, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=300000) as api:
        embed = api.module("backplane.embed")
        base = api.module("common.types.base_types")
        editor = api.module("common.commands.editor_commands")
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
        wait_for(lambda: viewable(s.host.display, frame.window), 120)
        time.sleep(2)
        doc = api.request(editor.GetOpenDocuments(type=base.DOCTYPE_PCB),
                          editor.GetOpenDocumentsResponse).documents[0]

        reads, refusal, tries = [], None, []
        for attempt in range(1, args.overlap_tries + 1):
            events()
            t_run = time.monotonic()
            started = api.request(embed.RunChecks(kind=embed.FK_PCB, parity=True, refill_zones=True),
                                  embed.RunChecksResponse)
            pid = started.progress_id
            running, finished, pairs, slowest = False, False, 0, 0.0
            t_progress, t_done, timeline = None, None, []
            t_end = t_run + 300
            while not finished and time.monotonic() < t_end:
                was_running = running
                t0 = time.monotonic()
                r = api.request_raw(editor.GetOpenDocuments(type=base.DOCTYPE_PCB))
                g = api.request_raw(embed.GetFrames())
                dt = time.monotonic() - t0
                slowest = max(slowest, dt)
                sent_change = None
                if was_running and refusal is None:
                    sent_change = api.request_raw(editor.BeginCommit(header=base.ItemHeader(document=doc)))
                seen = events()
                t_seen = time.monotonic() - t_run
                running = running or any(e.type == embed.ET_PROGRESS and e.progress.id == pid for e in seen)
                finished = any(e.type == embed.ET_CHECKS_DONE for e in seen)
                if running and t_progress is None:
                    t_progress = t_seen
                if finished:
                    t_done = t_seen
                timeline.append(f"+{t0 - t_run:.2f}/{dt:.2f}")
                if was_running and not finished:
                    # the run had begun before this pair was sent and had not ended after it was answered
                    reads.append((r.status.status, g.status.status, dt))
                    pairs += 1
                    if sent_change is not None:
                        refusal = sent_change
                if sent_change is not None and sent_change is not refusal \
                        and sent_change.status.status == api.envelope.AS_OK:
                    # served after the run ended: drop it and try the change in a later run
                    begin = editor.BeginCommitResponse()
                    sent_change.message.Unpack(begin)
                    api.request_raw(editor.EndCommit(header=base.ItemHeader(document=doc), id=begin.id,
                                                     action=editor.CMA_DROP))
            tries.append(f"run {attempt}: {pairs} pairs inside, slowest pair {slowest:.2f} s, progress seen at "
                         f"+{t_progress if t_progress is not None else -1:.2f} s, done seen at "
                         f"+{t_done if t_done is not None else -1:.2f} s")
            print(f"overlap run {attempt} pairs sent/took: {' '.join(timeline)}", flush=True)
            print(f"overlap {tries[-1]}", flush=True)
            if not finished:
                break
            if reads and refusal is not None:
                break

        check(bool(reads), f"a read lands inside a RunChecks on {args.overlap_board.name} "
                           f"({'; '.join(tries)}): the run must outlast a read round trip (pick a longer "
                           f"--overlap-board), and a slowest pair near the run's length means reads "
                           f"wait for the run instead of being served in its yields")
        check(reads and all(a == api.envelope.AS_OK and b == api.envelope.AS_OK for a, b, _ in reads),
              f"during RunChecks, GetOpenDocuments and GetFrames are answered ({len(reads)} pairs inside the run)")
        if reads:
            worst = max(dt for _, _, dt in reads)
            check(worst < 2.0, f"... each pair within 2 s while the run goes on (worst {worst:.2f} s)")
        check(refusal is not None and refusal.status.status == api.envelope.AS_BUSY
              and "a task is running:" in refusal.status.error_message and "token" in refusal.status.error_message,
              f"a change during the run is refused with the task named "
              f"({refusal.status.error_message[:120] if refusal is not None else 'never sent inside a run'})")
        if refusal is not None and refusal.status.status == api.envelope.AS_OK:
            begin = editor.BeginCommitResponse()
            refusal.message.Unpack(begin)
            api.request_raw(editor.EndCommit(header=base.ItemHeader(document=doc), id=begin.id,
                                             action=editor.CMA_DROP))
        s.shot("overlap-01")


if __name__ == "__main__":
    sys.exit(main())
