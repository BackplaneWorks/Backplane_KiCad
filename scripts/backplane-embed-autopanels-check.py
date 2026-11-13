#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bp.13 check for `kicad --embedded`: side panels give way in a narrow pane.

- PCB editor at 1400 px: its side panels (Properties, Appearance) are shown.
- At 700 px they collapse, least needed first, until the canvas has at least 60% of the width
  (KiCad otherwise left about 80 px of canvas in a 699 px pane).
- Back at 1400 px they return, and the canvas is as wide as it was.
- The person's own choice wins: Properties closed by them stays closed when the pane widens;
  Appearance opened by them in a narrow pane stays open.
- An agent at work opens the Agents pane by itself: that is auto, not the person's choice, so in
  a narrow pane it stays hidden; the agent's change and notice still reach the host
  (ET_DOCUMENT_EDITED, ET_AGENT_NOTICE undo_took_back), and the pane appears once there is room.
- The schematic editor does the same at 700 px.

Runs on a private Xvfb + i3 with a python-xlib stand-in host; screenshots to --out.
"""

import argparse
import json
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import (FAILURES, IpcSession, Session, check,  # noqa: E402
                                          viewable, wait_for)


def canvas_width(disp, frame_window):
    """Width of the frame's biggest viewable descendant window (the drawing canvas)."""
    best = [0, 0]   # area, width

    def walk(window, depth=0):
        try:
            children = window.query_tree().children
        except Exception:
            return
        for child in children:
            try:
                attrs = child.get_attributes()
                g = child.get_geometry()
            except Exception:
                continue
            if attrs.map_state == 2 and g.width * g.height > best[0]:
                best[0], best[1] = g.width * g.height, g.width
            walk(child, depth + 1)

    walk(disp.create_resource_object("window", frame_window))
    return best[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--out", type=Path, default=Path("embed-autopanels-out"))
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / "demos/pic_programmer"

    with Session(args.kicad, args.out, source) as s, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=300000) as api:
        embed = api.module("backplane.embed")
        editor = api.module("common.commands.editor_commands")
        base = api.module("common.types.base_types")
        board_types = api.module("board.board_types")
        since = [0]

        def events():
            reply = api.request(embed.NextEvents(since=since[0]), embed.NextEventsResponse)
            for e in reply.events:
                since[0] = max(since[0], e.seq)
            return list(reply.events)
        disp = s.host.display
        h = s.host.size[1]

        def show(kind, width):
            r = api.request(embed.ShowFrame(kind=kind, x=0, y=0, width=width, height=h), embed.ShowFrameResponse)
            wait_for(lambda: viewable(disp, r.frame.window), 60)
            time.sleep(1.5)   # the embed poll (250 ms) settles the panels
            s.host.pump(0.2)
            return r.frame

        def hide_others(kind):
            for f in api.request(embed.GetFrames(), embed.GetFramesResponse).frames:
                if f.shown and f.kind != kind:
                    api.request(embed.HideFrame(window=f.window), embed.HideFrameResponse)

        # ---- PCB
        frame = show(embed.FK_PCB, 1400)
        wide = canvas_width(disp, frame.window)
        check(wide < 1400 * 0.85, f"at 1400 px the side panels are shown (canvas {wide} px)")
        s.shot("01-pcb-1400")

        show(embed.FK_PCB, 700)
        narrow = canvas_width(disp, frame.window)
        check(narrow >= 700 * 0.58, f"at 700 px the canvas keeps at least ~60% (canvas {narrow} px)")
        s.shot("02-pcb-700")

        show(embed.FK_PCB, 1400)
        again = canvas_width(disp, frame.window)
        check(abs(again - wide) <= 24, f"back at 1400 px the panels return (canvas {again} px, was {wide})")
        s.shot("03-pcb-1400-again")

        # An agent at work in a narrow pane: the Agents pane opens itself (auto) and stays hidden
        show(embed.FK_PCB, 700)
        quiet = canvas_width(disp, frame.window)
        board_doc = api.request(editor.GetOpenDocuments(type=base.DOCTYPE_PCB),
                                editor.GetOpenDocumentsResponse).documents[0]
        events()
        seen = []

        def track_request(cls, track):
            req = cls()
            req.header.document.CopyFrom(board_doc)
            req.items.add().Pack(track)
            return req

        with IpcSession(args.kicad_cli, api_socket=s.socket, timeout=60000, client_name="person:check") as person, \
                IpcSession(args.kicad_cli, api_socket=s.socket, timeout=60000, client_name="agent:check") as agent:
            t = board_types.Track()
            t.start.x_nm, t.start.y_nm = 120_000_000, 90_000_000
            t.end.x_nm, t.end.y_nm = 130_000_000, 90_000_000
            t.width.value_nm = 250_000
            t.layer = board_types.BL_B_Cu
            created = person.request(track_request(editor.CreateItems, t), editor.CreateItemsResponse)
            mine = board_types.Track()
            created.created_items[0].item.Unpack(mine)
            mine.end.x_nm += 5_000_000
            agent.request(track_request(editor.UpdateItems, mine), editor.UpdateItemsResponse)
            time.sleep(1.5)
            s.host.pump(0.2)
            seen += events()
            agent_hidden = canvas_width(disp, frame.window)
            s.shot("04-pcb-700-agent")
            # The person's Ctrl+Z on their track takes the agent's change back: a notice
            person.request_raw(editor.RunAction(action="common.Interactive.undo"))
            wait_for(lambda: seen.extend(events()) or any(e.type == embed.ET_AGENT_NOTICE for e in seen), 10, 0.3)

        edited = [json.loads(e.detail) for e in seen if e.type == embed.ET_DOCUMENT_EDITED]
        notices = [json.loads(e.detail) for e in seen if e.type == embed.ET_AGENT_NOTICE]
        check(agent_hidden >= 700 * 0.58 and abs(agent_hidden - quiet) <= 8,
              f"in a narrow pane the Agents pane opened by an agent stays hidden (canvas {quiet} -> {agent_hidden} px)")
        check(any(d.get("actor") == "agent:check" for d in edited),
              f"the agent's change still reaches the host (ET_DOCUMENT_EDITED, {[d.get('actor') for d in edited]})")
        check(any(d.get("kind") == "undo_took_back" and d.get("actor") == "agent:check" for d in notices),
              f"the agent's notice still reaches the host (ET_AGENT_NOTICE, {[d.get('kind') for d in notices]})")

        show(embed.FK_PCB, 1400)
        with_agents = canvas_width(disp, frame.window)
        check(with_agents < wide - 40, f"with room the Agents pane appears (canvas {with_agents} px, was {wide})")
        s.shot("05-pcb-1400-agents")
        wide = with_agents

        # The person closes Properties: it stays closed through narrow and wide
        api.request_raw(editor.RunAction(action="common.Control.showProperties"))
        time.sleep(1.0)
        closed = canvas_width(disp, frame.window)
        show(embed.FK_PCB, 700)
        show(embed.FK_PCB, 1400)
        after = canvas_width(disp, frame.window)
        check(closed > wide + 40 and abs(after - closed) <= 24,
              f"Properties closed by the person stays closed ({wide} -> {closed} -> {after} px)")

        # The person opens Appearance in a narrow pane: it stays open
        show(embed.FK_PCB, 700)
        before = canvas_width(disp, frame.window)
        api.request_raw(editor.RunAction(action="pcbnew.Control.showLayersManager"))
        time.sleep(1.5)
        s.host.pump(0.2)
        opened = canvas_width(disp, frame.window)
        time.sleep(1.0)
        later = canvas_width(disp, frame.window)
        check(opened < before - 40 and abs(later - opened) <= 8,
              f"Appearance opened by the person in a narrow pane stays open ({before} -> {opened} -> {later} px)")
        s.shot("06-pcb-700-person")

        # ---- Schematic
        hide_others(embed.FK_SCH)
        sch = show(embed.FK_SCH, 1400)
        sch_wide = canvas_width(disp, sch.window)
        show(embed.FK_SCH, 700)
        sch_narrow = canvas_width(disp, sch.window)
        check(sch_narrow >= 700 * 0.58, f"schematic at 700 px: canvas {sch_narrow} px (at 1400: {sch_wide})")
        s.shot("07-sch-700")
        show(embed.FK_SCH, 1400)
        check(abs(canvas_width(disp, sch.window) - sch_wide) <= 24, "schematic panels return at 1400 px")

    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
