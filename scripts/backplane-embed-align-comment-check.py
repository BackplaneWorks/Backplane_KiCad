#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bp.10 checks for `kicad --embedded`: Alt+V / Alt+H alignment and "Comment to Backplane..."
in the PCB and schematic editors.

- Alignment: the actions over IPC (RunAction) and the hotkeys themselves (xdotool Alt+V/Alt+H),
  coordinates read back from the items' boxes; the anchor is the first item selected; the
  schematic stays on its grid.
- Comments (bp.13 comment-key-handoff): embedded, the context-menu item and ';' open no dialog
  and hand the comment key to the host (_BACKPLANE_KEY index 6); ';' typed in a text field (the
  search box) stays there; CommentSelection pushes ET_COMMENT with the references, nets, boxes and
  a non-blank PNG; an empty selection comments on the area in view.

Runs on a private Xvfb + i3 with a python-xlib stand-in host; saves screenshots to --out.
"""

import argparse
from pathlib import Path
import subprocess
import sys
import time

from Xlib import X

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import (FAILURES, IpcSession, Session, check,  # noqa: E402
                                          viewable, wait_for)


def png_stats(path):
    """(width, height, standard deviation 0..1) of a PNG, or None."""
    result = subprocess.run(["identify", "-format", "%w %h %[fx:standard_deviation]", str(path)],
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        w, h, sd = result.stdout.split()
        return int(w), int(h), float(sd)
    except ValueError:
        return None


def centre(box):
    return ((box.x_min + box.x_max) / 2, (box.y_min + box.y_max) / 2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--out", type=Path, default=Path("embed-align-comment-out"))
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / "demos/pic_programmer"

    with Session(args.kicad, args.out, source) as s, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=120000) as api:
        embed = api.module("backplane.embed")
        editor = api.module("common.commands.editor_commands")
        disp, host = s.host.display, s.host
        top = host.top.id
        api.request(embed.SetChrome(), embed.SetChromeResponse)
        events_since = [0]

        w, h = host.size

        def events():
            reply = api.request(embed.NextEvents(since=events_since[0]), embed.NextEventsResponse)
            events_since[0] = reply.next
            return list(reply.events)

        def frames():
            return {f.kind: f for f in api.request(embed.GetFrames(), embed.GetFramesResponse).frames}

        def show(kind, wait=2.5):
            for other in frames().values():
                if other.shown and other.kind != kind:
                    api.request(embed.HideFrame(window=other.window), embed.HideFrameResponse)
            w, h = host.size
            reply = api.request(embed.ShowFrame(kind=kind, x=0, y=0, width=w, height=h, focus=True),
                                embed.ShowFrameResponse)
            wait_for(lambda: viewable(disp, reply.frame.window), 30)
            time.sleep(wait)
            host.pump(0.1)
            return reply.frame.window

        def canvas_centre(kind):
            """The centre of the frame's drawing canvas on screen: where SelectItems' zoom puts the
            selection.  Not the pane's centre: the side panels and toolbars make the two differ, and
            a right-click outside the selection's box selects what is under it instead."""
            frame = disp.create_resource_object("window", frames()[kind].window)
            best = None
            for child in frame.query_tree().children:
                try:
                    if child.get_attributes().map_state != X.IsViewable:
                        continue
                    g = child.get_geometry()
                except Exception:
                    continue
                if best is None or g.width * g.height > best[2] * best[3]:
                    best = (g.x, g.y, g.width, g.height)
            if best is None:
                return w // 2, h // 2
            origin = host.root.translate_coords(frame, best[0], best[1])
            return origin.x + best[2] // 2, origin.y + best[3] // 2

        def wait_event(kind, seconds=8):
            found = []
            wait_for(lambda: found.extend(e for e in events() if e.type == kind) or bool(found), seconds, 0.2)
            return found

        def select(kind, refs, zoom=True):
            api.request(embed.SelectItems(kind=kind, references=refs, zoom=zoom), embed.SelectItemsResponse)
            time.sleep(0.8)

        def boxes(kind, refs):
            """Each reference's box (mm), read back through CommentSelection."""
            select(kind, refs, zoom=False)
            reply = api.request(embed.CommentSelection(kind=kind, note="(probe)"),
                                embed.CommentSelectionResponse)
            return {i.reference: i.bbox for i in reply.comment.items
                    if i.kind in ("FOOTPRINT", "SCH_SYMBOL")}

        hotkeys = {"alignCenterX": "alt+v", "alignCenterY": "alt+h", "undo": "ctrl+z"}

        def run(kind, action):
            """RunAction is served by the board editor only: the schematic gets the keys, with the
            pointer low on the canvas's left (inside the canvas, off the selected items)."""
            if kind == embed.FK_PCB:
                full = "common.Interactive.undo" if action == "undo" else f"pcbnew.AlignAndDistribute.{action}"
                api.request(editor.RunAction(action=full), editor.RunActionResponse)
            else:
                # The host's size now (the WM may have resized it); left of here are KiCad's panels
                host.pump(0.05)
                cw, ch = host.size
                s.xdotool("mousemove", str(int(cw * 0.26)), str(int(ch * 0.86)))
                time.sleep(0.3)
                s.xdotool("key", hotkeys[action])
            time.sleep(1.0)

        refs = ["R2", "R1", "R3"]     # R2 first: the anchor

        # ---------------- 1. Alignment ----------------
        for kind, grid in ((embed.FK_PCB, 0.0), (embed.FK_SCH, 1.27)):
            label = "pcb" if kind == embed.FK_PCB else "sch"
            show(kind)
            # The pointer is kept off the items (on the host's top edge): no item under the cursor
            s.xdotool("mousemove", str(w - 5), "3")
            before = boxes(kind, refs)
            check(set(before) == set(refs), f"{label}: the three resistors are found ({sorted(before)})")
            if set(before) != set(refs):
                continue
            anchor_x, anchor_y = centre(before["R2"])
            tolerance = grid / 2 + 0.01

            select(kind, refs)
            s.shot(f"{label}-01-before-align")
            run(kind, "alignCenterX")
            after = boxes(kind, refs)
            s.shot(f"{label}-02-after-align-x")
            xs = [centre(after[r])[0] for r in refs]
            check(all(abs(x - anchor_x) <= tolerance for x in xs),
                  f"{label}: align centre X puts every X centre on the first selected R2's "
                  f"({anchor_x:.3f} mm: {[round(x, 3) for x in xs]})")
            check(all(abs(centre(after[r])[1] - centre(before[r])[1]) < 0.01 for r in refs),
                  f"{label}: align centre X leaves Y alone")
            run(kind, "undo")

            select(kind, refs)
            run(kind, "alignCenterY")
            after = boxes(kind, refs)
            ys = [centre(after[r])[1] for r in refs]
            check(all(abs(y - anchor_y) <= tolerance for y in ys),
                  f"{label}: align centre Y puts every Y centre on R2's ({anchor_y:.3f} mm: "
                  f"{[round(y, 3) for y in ys]})")
            run(kind, "undo")

            if kind == embed.FK_SCH:
                # Pins stay on the connection grid: every symbol's anchor is a multiple of 1.27 mm
                select(kind, refs)
                run(kind, "alignCenterX")
                reply = api.request(embed.CommentSelection(kind=kind, note="(grid)"),
                                    embed.CommentSelectionResponse)
                on_grid = [abs(i.x_mm / 1.27 - round(i.x_mm / 1.27)) < 1e-3
                           for i in reply.comment.items if i.kind == "SCH_SYMBOL"]
                check(bool(on_grid) and all(on_grid), "sch: aligned symbols stay on the 1.27 mm grid")
                run(kind, "undo")

            # The hotkeys themselves (the pointer on the canvas; any anchor will do here)
            select(kind, refs)
            s.xdotool("mousemove", str(w // 2), str(h // 2))
            time.sleep(0.3)
            s.xdotool("key", "alt+v")
            time.sleep(1.2)  # pointer at the centre: Alt+V there, whatever the anchor
            s.shot(f"{label}-03-after-alt-v")
            after = boxes(kind, refs)
            spread = max(centre(after[r])[0] for r in refs) - min(centre(after[r])[0] for r in refs)
            check(spread <= 2 * tolerance, f"{label}: Alt+V aligns the X centres (spread {spread:.3f} mm)")
            run(kind, "undo")
            select(kind, refs)
            s.xdotool("mousemove", str(w // 2), str(h // 2))
            s.xdotool("key", "alt+h")
            time.sleep(1.2)
            s.shot(f"{label}-04-after-alt-h")
            after = boxes(kind, refs)
            spread = max(centre(after[r])[1] for r in refs) - min(centre(after[r])[1] for r in refs)
            check(spread <= 2 * tolerance, f"{label}: Alt+H aligns the Y centres (spread {spread:.3f} mm)")
            run(kind, "undo")

            # Hotkeys and the context menu show the bindings
            select(kind, refs)
            s.xdotool("mousemove", str(w // 2), str(h // 2), "click", "3")
            time.sleep(1.5)
            s.shot(f"{label}-05-context-menu")
            s.xdotool("key", "Escape")
            time.sleep(0.5)

        # ---------------- 2. Comment to Backplane ----------------
        # bp.13 comment-key-handoff: embedded, the comment action hands the key to the host
        key_atom = disp.intern_atom("_BACKPLANE_KEY")

        def comment_key(seconds=8.0):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                host.pump(0.05)
                for index, (_, atom, data) in enumerate(host.messages):
                    if atom == key_atom:
                        del host.messages[index]
                        return data
            return None

        # The pin is of a part not selected itself: the PCB editor keeps only a footprint when it
        # and its own pad are selected (the schematic keeps both)
        for kind, refs_c, pin, label in ((embed.FK_PCB, ["U1", "C1"], "R1.1", "pcb"),
                                         (embed.FK_SCH, ["U2", "C1"], "C1.1", "sch")):
            show(kind)
            # Footprints/symbols give references; a pin gives its net (as ET_SELECTION_CHANGED)
            api.request(embed.SelectItems(kind=kind, references=refs_c, pins=[pin], zoom=True),
                        embed.SelectItemsResponse)
            time.sleep(0.8)
            events()
            host.pump()
            host.messages.clear()
            before_windows = set(s.watcher.current())
            # Right-click on the canvas keeps the selection; Up from a fresh GTK menu is its last item
            cx, cy = canvas_centre(kind)
            s.xdotool("mousemove", str(cx), str(cy), "click", "3")
            time.sleep(1.5)
            s.shot(f"{label}-10-comment-menu")
            s.xdotool("key", "Up")
            time.sleep(0.5)
            s.shot(f"{label}-11-comment-menu-item")
            s.xdotool("key", "Return")
            data = comment_key()
            time.sleep(1.0)
            frame_window = frames()[kind].window
            check(data is not None and list(data[:5]) == [frame_window, 0x3b, 0, 6, 1],
                  f"{label}: the menu item hands the comment key to the host (_BACKPLANE_KEY index 6: {data})")
            check(not (set(s.watcher.current()) - before_windows - {top})
                  and not any(e.type == embed.ET_COMMENT for e in events()),
                  f"{label}: embedded, it opens no dialog and posts no ET_COMMENT")
            # The host then takes the comment itself (CommentSelection): its content
            note = f"Check the {label} decoupling near {refs_c[0]}"
            api.request(embed.CommentSelection(kind=kind, note=note), embed.CommentSelectionResponse)
            got = wait_event(embed.ET_COMMENT, 10)
            check(bool(got), f"{label}: CommentSelection pushes ET_COMMENT")
            if not got:
                continue
            e = got[-1]
            c = e.comment
            check(e.frame.kind == kind and c.note == note and e.detail == note and c.from_api,
                  f"{label}: the event carries the frame and the note ({e.frame.kind}, {c.note!r})")
            check(sorted(c.names.references) == sorted(refs_c) and list(e.selection.references) == list(c.names.references),
                  f"{label}: references {list(c.names.references)}")
            check(len(c.names.nets) > 0 and pin in c.names.pins,
                  f"{label}: the selected pin {pin} and its net ({list(c.names.pins)}, {list(c.names.nets)[:6]})")
            union = [min(i.bbox.x_min for i in c.items), min(i.bbox.y_min for i in c.items),
                     max(i.bbox.x_max for i in c.items), max(i.bbox.y_max for i in c.items)]
            check(all(abs(a - b) < 1e-3 for a, b in zip(union, [c.bbox.x_min, c.bbox.y_min, c.bbox.x_max, c.bbox.y_max]))
                  and c.bbox.x_max > c.bbox.x_min and c.bbox.y_max > c.bbox.y_min,
                  f"{label}: the overall bbox is the union of the items' ({c.bbox.x_min:.2f},{c.bbox.y_min:.2f})-"
                  f"({c.bbox.x_max:.2f},{c.bbox.y_max:.2f}) mm")
            for item in c.items:
                if item.reference == refs_c[0] and item.kind in ("FOOTPRINT", "SCH_SYMBOL"):
                    check(item.bbox.x_min <= item.x_mm <= item.bbox.x_max or kind == embed.FK_SCH,
                          f"{label}: {item.reference} has kind {item.kind}, position and bbox, "
                          f"layer '{item.layer}' sheet '{item.sheet_path}'")
            stats = png_stats(c.screenshot) if c.screenshot else None
            check(bool(c.screenshot) and Path(c.screenshot).is_file()
                  and Path(c.screenshot).parent.parent == s.socket.parent,
                  f"{label}: the PNG is written under the API socket's directory ({c.screenshot})")
            check(stats is not None and max(stats[0], stats[1]) <= 800 and stats[2] > 0.02
                  and (stats[0], stats[1]) == (c.screenshot_width, c.screenshot_height),
                  f"{label}: the PNG is at most 800 px and not blank ({stats})")
            if c.screenshot:
                subprocess.run(["cp", c.screenshot, str(args.out / f"{label}-13-comment-screenshot.png")])
            check(c.screenshot_region.x_min <= c.bbox.x_min and c.screenshot_region.x_max >= c.bbox.x_max,
                  f"{label}: the PNG's region contains the selection")


            # Over IPC
            reply = api.request(embed.CommentSelection(kind=kind, note="from the API"),
                                embed.CommentSelectionResponse)
            got = wait_event(embed.ET_COMMENT, 5)
            check(bool(got) and got[-1].seq == reply.seq and got[-1].comment.from_api
                  and sorted(reply.comment.names.references) == sorted(refs_c),
                  f"{label}: CommentSelection pushes the same event (seq {reply.seq})")

            # bp.13: capture only (the host takes the note): the comment comes back, nothing pushed
            if "capture_only" in embed.CommentSelection.DESCRIPTOR.fields_by_name:
                events()
                reply = api.request(embed.CommentSelection(kind=kind, note="ignored", capture_only=True),
                                    embed.CommentSelectionResponse)
                time.sleep(1.0)
                check(reply.seq == 0 and sorted(reply.comment.names.references) == sorted(refs_c)
                      and bool(reply.comment.screenshot) and not reply.comment.note
                      and not any(e.type == embed.ET_COMMENT for e in events()),
                      f"{label}: CommentSelection capture_only returns the comment and pushes nothing")

                # The hotkey: ';' hands the comment key to the host, as the menu item does
                host.pump()
                host.messages.clear()
                before_windows = set(s.watcher.current())
                s.xdotool("mousemove", str(cx), str(cy))
                s.xdotool("key", "semicolon")
                data = comment_key()
                time.sleep(1.0)
                check(data is not None and data[1] == 0x3b and data[3] == 6
                      and not (set(s.watcher.current()) - before_windows - {top})
                      and not any(e.type == embed.ET_COMMENT for e in events()),
                      f"{label}: ';' hands the comment key to the host, no dialog ({data})")

                # In a text field ';' is a character: the search box (Ctrl+G) keeps it
                s.xdotool("key", "ctrl+g")
                time.sleep(1.5)
                host.pump()
                host.messages.clear()
                s.xdotool("type", "--delay", "40", "R1;")
                time.sleep(1.0)
                s.shot(f"{label}-15-semicolon-in-search")
                data = comment_key(2.0)
                check(data is None and not any(e.type == embed.ET_COMMENT for e in events()),
                      f"{label}: ';' typed in the search box stays there, nothing to the host ({data})")
                s.xdotool("key", "Escape")
                s.xdotool("mousemove", str(cx), str(cy), "click", "1")
                time.sleep(0.5)

            # Nothing selected: the area in view
            select(kind, [], zoom=False)   # SelectItems with nothing clears the selection
            time.sleep(0.5)
            reply = api.request(embed.CommentSelection(kind=kind, note="this area"), embed.CommentSelectionResponse)
            check(reply.comment.empty_selection and not reply.comment.items
                  and reply.comment.bbox.x_max > reply.comment.bbox.x_min and bool(reply.comment.screenshot),
                  f"{label}: an empty selection comments on the area in view")
            if reply.comment.screenshot:
                subprocess.run(["cp", reply.comment.screenshot, str(args.out / f"{label}-14-area-screenshot.png")])

        missing = api.request_raw(embed.CommentSelection(kind=embed.FK_FOOTPRINT_EDITOR, note="x"))
        check(missing.status.status == api.envelope.AS_UNHANDLED, "CommentSelection for a kind without a handler is AS_UNHANDLED")
        shots = sorted((s.socket.parent / "backplane-comments").glob("comment-*.png"))
        check(0 < len(shots) <= 32, f"the screenshot folder is bounded ({len(shots)} files)")

    print(f"\n{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
