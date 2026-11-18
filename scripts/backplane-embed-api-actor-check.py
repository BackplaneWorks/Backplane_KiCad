#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""#1224 check for `kicad --embedded`: board mutations an API request makes are done in the request.

RefillZones{} (no zone ids) and SetBoardOrigin (grid, drill) on the open board editor used to
run after the reply (CallAfter): outside the request's co-editing actor, and after a SaveDocument
sent right behind them.  Each discriminating case fails before the fix:

(a) an agent's RefillZones{} is recorded as the agent's (ET_DOCUMENT_EDITED actor agent:check)
    and the person's undo leaves the fill;
(c) RefillZones{} then an immediate SaveDocument: the saved board has the fills;
(d) SetBoardOrigin grid and drill from an agent, sent raw as kicad_edit's ipc op does: read back
    right after the reply, kept by an immediate SaveDocument, and the drill origin's undo entry
    the agent's (the person's undo leaves it).  KiCad keeps no undo entry for a grid origin set
    with a point ("we can't undo the other grid dialog settings").
Control (b), expected to pass before and after: the person's RefillZones{} is recorded as the
person's (actor "").

The project is complex_hierarchy (166 zones, so a fill takes real time) with its zone fills
stripped before KiCad opens it, so fills in the saved file can only come from the request.  (c)
also asks the events socket, answered on its own thread, for the fill's ET_DOCUMENT_EDITED at
once after the reply: a fill run after the reply cannot be there yet, whatever order wx then
runs its queues in.  Runs on a private Xvfb + i3.
"""

import argparse
import threading
import json
from pathlib import Path
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import (FAILURES, IpcSession, Session, check,  # noqa: E402
                                          viewable, wait_for)


def strip_fills(text):
    """The board without its zones' (filled_polygon ...) blocks."""
    out, i = [], 0
    for m in re.finditer(r"\(filled_polygon\b", text):
        if m.start() < i:
            continue
        depth, j = 0, m.start()
        while j < len(text):
            depth += {"(": 1, ")": -1}.get(text[j], 0)
            j += 1
            if depth == 0:
                break
        out.append(text[i:m.start()])
        i = j
    out.append(text[i:])
    return "".join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--out", type=Path, default=Path("embed-api-actor-out"))
    parser.add_argument("--project", type=Path,
                        default=Path(__file__).resolve().parents[1] / "demos/complex_hierarchy")
    parser.add_argument("--timing-only", action="store_true",
                        help="only the fill's completion time and the longest event-service gap")
    parser.add_argument("--drill-actor", action="store_true",
                        help="only the drill origin's own change: its actor, and what the person's undo takes")
    args = parser.parse_args()

    source = Path(tempfile.mkdtemp(prefix="api-actor-src-"))
    shutil.copytree(args.project, source, dirs_exist_ok=True)
    board_file = next(source.glob("*.kicad_pcb"))
    board_file.write_text(strip_fills(board_file.read_text()))
    check("filled_polygon" not in board_file.read_text(), "the copy starts with no zone fills")

    with Session(args.kicad, args.out, source) as s, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=300000) as api, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=300000, client_name="agent:check") as agent, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=300000, client_name="person:check") as person:
        embed = api.module("backplane.embed")
        editor = api.module("common.commands.editor_commands")
        project_cmd = api.module("common.commands.project_commands")
        base = api.module("common.types.base_types")
        enums = api.module("common.types.enums")
        board_cmd = api.module("board.board_commands")
        board_types = api.module("board.board_types")
        geometry = api.module("common.types.base_types")
        w, h = s.host.size
        since = [0]

        def edits():
            out = []
            reply = api.request(embed.NextEvents(since=since[0]), embed.NextEventsResponse)
            for e in reply.events:
                since[0] = max(since[0], e.seq)
                if e.type == embed.ET_DOCUMENT_EDITED:
                    try:
                        out.append(json.loads(e.detail))
                    except ValueError:
                        pass
            return out

        def edits_for(seconds):
            got, end = [], time.monotonic() + seconds
            while time.monotonic() < end:
                got += edits()
                time.sleep(0.2)
            return got

        r = api.request(embed.ShowFrame(kind=embed.FK_PCB, x=0, y=0, width=w, height=h), embed.ShowFrameResponse)
        wait_for(lambda: viewable(s.host.display, r.frame.window), 120)
        time.sleep(2)
        doc = api.request(editor.GetOpenDocuments(type=base.DOCTYPE_PCB), editor.GetOpenDocumentsResponse).documents[0]
        edits_for(1)

        def zones_filled():
            req = editor.GetItems(types=[enums.KOT_PCB_ZONE])
            req.header.document.CopyFrom(doc)
            items = api.request(req, editor.GetItemsResponse).items
            zones = []
            for item in items:
                z = board_types.Zone()
                item.Unpack(z)
                if not z.type == board_types.ZT_RULE_AREA:
                    zones.append(sum(len(fp.shapes.polygons) for fp in z.filled_polygons) > 0)
            return zones and all(zones)

        def save():
            req = project_cmd.SaveDocument()
            req.document.CopyFrom(doc)
            return api.request_raw(req)

        def saved_text():
            return board_file_in_run().read_text()

        def board_file_in_run():
            return next(Path(s.project_dir).glob("*.kicad_pcb"))

        def undo_as_person():
            person.request_raw(editor.RunAction(action="common.Interactive.undo"))
            time.sleep(1)

        disp = s.host.display

        def focus_to_host():
            disp.set_input_focus(s.host.top, 1, 0)   # X.RevertToParent, X.CurrentTime
            disp.sync()
            time.sleep(0.3)
            f = disp.get_input_focus().focus
            return getattr(f, "id", f)

        def report_focus(label, before):
            """Reported, not checked (#1224 review): does an agent's fill take the keyboard from the
            host?  FillAllZones binds a one-shot refocus of the canvas at the next idle."""
            time.sleep(1.5)   # past the next idle
            f = disp.get_input_focus().focus
            after = getattr(f, "id", f)
            print(f"FOCUS {label}: host top {s.host.top.id:#x}; before {before:#x}, after {after:#x}: "
                  f"{'MOVED from the host' if after != before else 'stayed with the host'}", flush=True)

        def refill(sess):
            req = board_cmd.RefillZones()
            req.board.CopyFrom(doc)
            return sess.request_raw(req)

        events_sock = api.request(embed.GetFrames(), embed.GetFramesResponse).events_socket.removeprefix("ipc://")

        def timed_fill(label):
            """The agent's RefillZones{} with two observers: the events socket (its own thread)
            polled every 20 ms for the fill's ET_DOCUMENT_EDITED, the fill's completion; and a
            second main-socket client sending GetOpenDocuments (served on the UI thread) every
            50 ms, each one's latency the event-service gap at that moment."""
            probes, stop = [], threading.Event()
            t0 = time.monotonic()

            def prober():
                with IpcSession(args.kicad_cli, api_socket=s.socket, timeout=600000,
                                client_name="works.backplane.probe") as probe:
                    while not stop.is_set():
                        sent = time.monotonic()
                        probe.request_raw(editor.GetOpenDocuments(type=base.DOCTYPE_PCB))
                        probes.append(((sent - t0) * 1000, (time.monotonic() - sent) * 1000))
                        stop.wait(0.05)

            thread = threading.Thread(target=prober, daemon=True)
            with IpcSession(args.kicad_cli, api_socket=events_sock, timeout=60000) as ev:
                mark = since[0]
                thread.start()
                time.sleep(0.2)
                t0 = time.monotonic()
                reply = refill(agent)
                reply_ms = (time.monotonic() - t0) * 1000
                at_reply = ev.request(embed.NextEvents(since=mark), embed.NextEventsResponse).events
                seen = list(at_reply)
                done_ms = None
                if any(e.type == embed.ET_DOCUMENT_EDITED and "Fill" in e.detail for e in at_reply):
                    done_ms = reply_ms
                while done_ms is None and time.monotonic() - t0 < 600:
                    got = ev.request(embed.NextEvents(since=max([mark] + [e.seq for e in seen])),
                                     embed.NextEventsResponse).events
                    seen += got
                    if any(e.type == embed.ET_DOCUMENT_EDITED and "Fill" in e.detail for e in got):
                        done_ms = (time.monotonic() - t0) * 1000
                        break
                    time.sleep(0.02)
                time.sleep(0.5)
            stop.set()
            thread.join(30)
            during = [p for p in probes if p[0] >= 0]
            worst = max(during, key=lambda p: p[1]) if during else (0, 0)
            print(f"TIMING {label}: reply {reply_ms:.0f} ms (status {reply.status.status}); fill done "
                  f"{'%.0f ms' % done_ms if done_ms is not None else 'not seen'} after the request; "
                  f"longest main-thread read latency {worst[1]:.0f} ms (sent at +{worst[0]:.0f} ms), "
                  f"{len(during)} reads, every 50 ms", flush=True)
            return reply, at_reply

        # Raw, as kicad_edit's ipc op sends them: the agent's client
        def set_origin(kind, x_mm, y_mm):
            req = board_cmd.SetBoardOrigin(type=kind)
            req.board.CopyFrom(doc)
            req.origin.x_nm = int(x_mm * 1e6)
            req.origin.y_nm = int(y_mm * 1e6)
            return agent.request_raw(req)

        def get_origin(kind):
            req = board_cmd.GetBoardOrigin(type=kind)
            req.board.CopyFrom(doc)
            return api.request(req, geometry.Vector2)

        if args.drill_actor:
            # The drill origin's own entry, read from the co-editing change log (GetChanges: one
            # Change per undo push or undo, with that entry's actor), matched to the one change
            # recorded after a mark taken right before the request.  The undo stack itself is not
            # exposed by the API; its pushes and undos are, through this log.
            coedit = api.module("backplane.coedit")

            def changes_since(mark):
                req = coedit.GetChanges(since=mark, doc="pcb")
                return api.request(req, coedit.GetChangesResponse)

            def show(c):
                return (f"seq {c.seq} actor '{c.actor}' undo {c.undo} description '{c.description}' "
                        f"modified {list(c.modified)} created {list(c.created)} deleted {list(c.deleted)}")

            def origins():
                g = get_origin(board_cmd.BOT_GRID)
                d = get_origin(board_cmd.BOT_DRILL)
                return (g.x_nm, g.y_nm), (d.x_nm, d.y_nm)

            print(f"ORIGINS at start: grid {origins()[0]}, drill {origins()[1]}", flush=True)
            mark = changes_since(0).last
            set_origin(board_cmd.BOT_GRID, 12.5, 7.5)
            time.sleep(1.0)
            grid_changes = changes_since(mark).changes
            for c in grid_changes:
                print(f"CHANGE after the grid request: {show(c)}", flush=True)
            mark = changes_since(0).last

            reply = set_origin(board_cmd.BOT_DRILL, 21.0, 14.0)
            got = []
            end = time.monotonic() + 5
            while time.monotonic() < end and not got:
                got = [c for c in changes_since(mark).changes if not c.undo]
                time.sleep(0.1)
            for c in got:
                print(f"CHANGE after the drill request: {show(c)}", flush=True)
            check(reply.status.status == 1 and len(got) == 1,
                  f"(d) the drill request produced exactly one change ({len(got)}; status {reply.status.status})")
            drill_change = got[0] if got else None
            check(drill_change is not None and drill_change.actor == "agent:check",
                  f"(d) [DISCRIMINATING] the drill origin's own change is the agent's "
                  f"(actor '{drill_change.actor if drill_change else None}')")

            before = origins()
            print(f"ORIGINS before the person's undo: grid {before[0]}, drill {before[1]}", flush=True)
            mark = changes_since(0).last
            person.request_raw(editor.RunAction(action="common.Interactive.undo"))
            time.sleep(1.0)
            undone = changes_since(mark).changes
            for c in undone:
                print(f"CHANGE after the person's undo: {show(c)}", flush=True)
            if not undone:
                print("CHANGE after the person's undo: none recorded (the undo took nothing)", flush=True)
            after = origins()
            print(f"ORIGINS after the person's undo: grid {after[0]}, drill {after[1]}", flush=True)
            check(after[1] == (int(21.0 * 1e6), int(14.0 * 1e6)),
                  f"(d) the person's undo leaves the agent's drill origin {after[1]} [kept; see EXPLAIN]")
            took = [c for c in undone if c.undo]
            print("EXPLAIN (observed): the drill request's change lists modified "
                  f"{list(drill_change.modified) if drill_change else None}; the person's undo "
                  f"{'took a change with actor ' + repr(took[0].actor) + ' modifying ' + str(list(took[0].modified)) if took else 'took no change'}; "
                  f"grid origin {before[0]} -> {after[0]}, drill origin {before[1]} -> {after[1]}", flush=True)
            print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
            return 1 if FAILURES else 0

        if args.timing_only:
            check(not zones_filled(), "the open board has no zone fills")
            reply, _ = timed_fill(f"{args.project.name}")
            check(reply.status.status == 1 and zones_filled(), "the fill completed")
            print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
            return 1 if FAILURES else 0

        # Refusal: a tool of the person's active.  Each request is refused AS_BUSY and changes
        # nothing; the tool stays active until the person ends it.
        def busy_probe():
            return person.request_raw(editor.RunAction(action="common.Control.zoomFitScreen")).status.status

        def origin_now(kind):
            req = board_cmd.GetBoardOrigin(type=kind)
            req.board.CopyFrom(doc)
            o = api.request(req, geometry.Vector2)
            return (o.x_nm, o.y_nm)

        before = (origin_now(board_cmd.BOT_GRID), origin_now(board_cmd.BOT_DRILL))
        edits_for(0.5)
        person.request_raw(editor.RunAction(action="pcbnew.InteractiveDrawing.line"))
        time.sleep(0.5)
        check(busy_probe() == 7, "refusal: the person's line tool is active (a gated request is AS_BUSY)")
        answers = []
        answers.append(refill(agent).status.status)
        for kind in (board_cmd.BOT_GRID, board_cmd.BOT_DRILL):
            req = board_cmd.SetBoardOrigin(type=kind)
            req.board.CopyFrom(doc)
            req.origin.x_nm, req.origin.y_nm = 33000000, 44000000
            answers.append(agent.request_raw(req).status.status)
        changed = edits_for(1.0)
        after = (origin_now(board_cmd.BOT_GRID), origin_now(board_cmd.BOT_DRILL))
        check(answers == [7, 7, 7], f"refusal: RefillZones{{}}, SetBoardOrigin grid and drill each answer AS_BUSY ({answers})")
        check(not zones_filled() and after == before and not changed,
              f"refusal: the board is unchanged (no fill, origins {after == before}, new edits {len(changed)})")
        check(busy_probe() == 7, "refusal: the person's tool is still active")
        frame_window = r.frame.window
        disp_ = s.host.display
        disp_.set_input_focus(disp_.create_resource_object("window", frame_window), 1, 0)
        disp_.sync()
        s.xdotool("key", "Escape")
        time.sleep(0.5)
        s.xdotool("key", "Escape")
        ended = wait_for(lambda: busy_probe() == 1, 10, 0.3)
        check(ended, "refusal: the person ends the tool (Escape), and requests are served again")

        # (b) control: the person's fill is the person's
        check(not zones_filled(), "the open board has no zone fills")
        refill(person)
        got = [e for e in edits_for(4) if "Fill" in e.get("description", "")]
        check(bool(got) and all(e.get("actor", "") == "" for e in got),
              f"(b, control) the person's RefillZones{{}} is recorded as the person's ({[e.get('actor') for e in got]})")
        undo_as_person()
        check(not zones_filled(), "the person's undo takes their own fill back")
        edits_for(1)

        # (c) fill then an immediate save: the saved board has the fills
        focus_before = focus_to_host()
        reply, at_reply = timed_fill("complex_hierarchy, the agent's RefillZones{}")
        report_focus("(c) the agent's RefillZones{}", focus_before)
        fill_at_reply = any(e.type == embed.ET_DOCUMENT_EDITED and "Fill" in e.detail for e in at_reply)
        check(fill_at_reply, f"(c) the fill is done when RefillZones{{}} answers (its ET_DOCUMENT_EDITED is "
                             f"already there: {fill_at_reply})")
        saved = save()
        text = saved_text()
        check(reply.status.status == 1 and saved.status.status == 1 and "filled_polygon" in text,
              f"(c) RefillZones{{}} then an immediate SaveDocument: the saved board has the fills "
              f"(fill {reply.status.status}, save {saved.status.status}, filled_polygon in file: {'filled_polygon' in text})")

        # (a) the agent's fill is the agent's; the person's undo leaves it (its focus effect is
        # the one (c) reported: the same fill)
        got = [e for e in edits_for(4) if "Fill" in e.get("description", "")]
        check(bool(got) and all(e.get("actor") == "agent:check" for e in got),
              f"(a) the agent's RefillZones{{}} is recorded as the agent's ({[e.get('actor') for e in got]})")
        undo_as_person()
        check(zones_filled(), "(a) the person's undo leaves the agent's fill")

        # (d) origins from an agent, raw: applied before the reply, saved, the drill's undo the agent's
        def set_origin(kind, x_mm, y_mm):
            req = board_cmd.SetBoardOrigin(type=kind)
            req.board.CopyFrom(doc)
            req.origin.x_nm = int(x_mm * 1e6)
            req.origin.y_nm = int(y_mm * 1e6)
            return agent.request_raw(req)

        def get_origin(kind):
            req = board_cmd.GetBoardOrigin(type=kind)
            req.board.CopyFrom(doc)
            return api.request(req, geometry.Vector2)

        for name, kind, x, y in (("grid", board_cmd.BOT_GRID, 12.5, 7.5), ("drill", board_cmd.BOT_DRILL, 21.0, 14.0)):
            edits_for(0.5)
            reply = set_origin(kind, x, y)
            o = get_origin(kind)
            check(reply.status.status == 1 and o.x_nm == int(x * 1e6) and o.y_nm == int(y * 1e6),
                  f"(d) the {name} origin is applied before the reply (read back {o.x_nm}, {o.y_nm})")
            save()
            token = "grid_origin" if name == "grid" else "aux_axis_origin"
            m = re.search(r"\(" + token + r" ([-\d.]+) ([-\d.]+)\)", saved_text())
            check(bool(m) and abs(float(m.group(1)) - x) < 1e-6 and abs(float(m.group(2)) - y) < 1e-6,
                  f"(d) an immediate SaveDocument keeps the {name} origin ({m.groups() if m else None})")

        undo_as_person()
        o = get_origin(board_cmd.BOT_DRILL)
        check(o.x_nm == int(21.0 * 1e6) and o.y_nm == int(14.0 * 1e6),
              f"(d) the person's undo leaves the agent's drill origin ({o.x_nm}, {o.y_nm})")

    shutil.rmtree(source, ignore_errors=True)
    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
