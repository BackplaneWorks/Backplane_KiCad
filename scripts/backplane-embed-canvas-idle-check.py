#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bp.13 batch 8 checks for `kicad --embedded`: canvas-idle.

- GetFrames reports, per frame, canvas_input_ago_ms (since the person's last button, drag,
  wheel or key on its canvas), tool_busy (an interactive tool mid-operation) and tool_name.
- Hovering is not input: motion with no button down leaves canvas_input_ago_ms growing.
- RunChecks{min_canvas_idle_ms} is refused at once with AS_BUSY "canvas in use: ..." while the
  canvas had input within that time or a tool is busy, and accepted once it is idle; with 0 it
  runs regardless of recent input.

Runs on a private Xvfb + i3 with a python-xlib stand-in host; screenshots to --out.
"""

import argparse
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import (FAILURES, IpcSession, Session, check,  # noqa: E402
                                          viewable, wait_for)

U64_MAX = (1 << 64) - 1


def canvas_centre(disp, root, frame_window):
    """Screen centre of the frame's biggest viewable descendant window (the drawing canvas)."""
    best = None

    def walk(window):
        nonlocal best
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
            if attrs.map_state == 2:
                area = g.width * g.height
                if best is None or area > best[0]:
                    o = root.translate_coords(child, 0, 0)
                    best = (area, o.x + g.width // 2, o.y + g.height // 2)
            walk(child)

    walk(disp.create_resource_object("window", frame_window))
    return (best[1], best[2]) if best else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--out", type=Path, default=Path("embed-canvas-idle-out"))
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / "demos/pic_programmer"

    with Session(args.kicad, args.out, source) as s, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=300000) as api:
        embed = api.module("backplane.embed")
        editor = api.module("common.commands.editor_commands")
        common = api.module("backplane.capabilities")
        disp = s.host.display
        w, h = s.host.size
        since = [0]

        caps = api.request(common.GetCapabilities(), common.GetCapabilitiesResponse)
        check("canvas-idle" in caps.capabilities, "GetCapabilities lists canvas-idle")

        def events():
            reply = api.request(embed.NextEvents(since=since[0]), embed.NextEventsResponse)
            for e in reply.events:
                since[0] = max(since[0], e.seq)
            return list(reply.events)

        def pcb():
            return {f.kind: f for f in api.request(embed.GetFrames(), embed.GetFramesResponse).frames}[embed.FK_PCB]

        def run_checks(min_idle):
            """(accepted, error message); an accepted run is waited for to the end."""
            events()
            reply = api.request_raw(embed.RunChecks(kind=embed.FK_PCB, min_canvas_idle_ms=min_idle))
            if reply.status.status != api.envelope.AS_OK:
                return False, reply.status.error_message
            wait_for(lambda: any(e.type == embed.ET_CHECKS_DONE for e in events()), 300, 0.2)
            return True, ""

        r = api.request(embed.ShowFrame(kind=embed.FK_PCB, x=0, y=0, width=w, height=h), embed.ShowFrameResponse)
        wait_for(lambda: viewable(disp, r.frame.window), 60)
        time.sleep(2)
        cx, cy = canvas_centre(disp, s.host.root, r.frame.window)

        # 1. Idle
        time.sleep(5)
        f = pcb()
        check(f.canvas_input_ago_ms >= 5000 and not f.tool_busy,
              f"no input for 5 s: canvas_input_ago_ms {f.canvas_input_ago_ms}, tool_busy {f.tool_busy}")
        ok, why = run_checks(3000)
        check(ok, f"RunChecks with min_canvas_idle_ms 3000 is accepted when idle ({why})")

        # 2. Wheel zoom over the canvas
        s.xdotool("mousemove", str(cx), str(cy))
        refused = []
        for notch in range(10):
            s.xdotool("click", "4" if notch % 2 else "5")
            time.sleep(0.2)
            if notch == 5:
                ago = pcb().canvas_input_ago_ms
                check(ago < 300, f"during wheeling canvas_input_ago_ms is under 300 ({ago})")
                refused.append(run_checks(3000))
        time.sleep(1.5)
        refused.append(run_checks(3000))
        check(all(not ok and why.startswith("canvas in use: ") for ok, why in refused),
              f"RunChecks 3000 during and 1.5 s after wheeling is AS_BUSY 'canvas in use' ({refused})")
        time.sleep(2.5)
        ok, why = run_checks(3000)
        check(ok, f"accepted 4 s after the last notch ({why})")

        # 3. Middle-button drag pan, then arrow-key pan after a click on the canvas
        s.xdotool("mousemove", str(cx), str(cy), "mousedown", "2")
        for step in range(8):
            s.xdotool("mousemove", str(cx + 10 * step), str(cy + 5 * step))
            time.sleep(0.05)
        s.xdotool("mouseup", "2")
        ok, why = run_checks(3000)
        check(not ok and why.startswith("canvas in use: "), f"refused right after a middle-drag pan ({why})")
        time.sleep(3.5)
        s.xdotool("mousemove", str(cx), str(cy), "click", "1")
        time.sleep(3.5)
        s.xdotool("key", "Left", "Left", "Up")
        ago = pcb().canvas_input_ago_ms
        ok, why = run_checks(3000)
        check(ago < 1000 and not ok and why.startswith("canvas in use: "),
              f"arrow-key pan counts as input ({ago} ms, {why})")
        time.sleep(3.5)

        # 4. A tool mid-operation: a graphic line with its first point placed
        api.request(editor.RunAction(action="pcbnew.InteractiveDrawing.line"), editor.RunActionResponse)
        time.sleep(1)
        f = pcb()
        check(not f.tool_busy, f"a drawing tool merely chosen is not busy ({f.tool_name})")
        s.xdotool("mousemove", str(cx - 50), str(cy - 50), "click", "1")
        s.xdotool("mousemove", str(cx + 50), str(cy + 20))
        time.sleep(5)
        f = pcb()
        check(f.tool_busy and "InteractiveDrawing" in f.tool_name and f.canvas_input_ago_ms >= 4500,
              f"after the first point: tool_busy {f.tool_busy}, tool {f.tool_name}, input {f.canvas_input_ago_ms} ms ago")
        ok, why = run_checks(3000)
        check(not ok and "busy" in why, f"refused while the tool is busy though input was 5 s ago ({why})")
        s.shot("01-line-tool-busy")
        s.xdotool("key", "Escape")
        time.sleep(0.5)
        s.xdotool("key", "Escape")
        time.sleep(0.5)
        f = pcb()
        check(not f.tool_busy, f"Escape twice: tool_busy false ({f.tool_name})")
        time.sleep(3.5)
        ok, why = run_checks(3000)
        check(ok, f"accepted 3.5 s after the tool ended ({why})")

        # 5. min_canvas_idle_ms 0 runs regardless of recent input
        s.xdotool("mousemove", str(cx), str(cy), "click", "4", "click", "5")
        ok, why = run_checks(0)
        check(ok, f"RunChecks with min_canvas_idle_ms 0 runs right after wheeling ({why})")

        # 6. Hovering is not input
        time.sleep(1)
        before = pcb().canvas_input_ago_ms
        t0 = time.monotonic()
        while time.monotonic() - t0 < 4:
            s.xdotool("mousemove", str(cx + int(40 * (time.monotonic() % 1))), str(cy))
            time.sleep(0.1)
        after = pcb().canvas_input_ago_ms
        check(before != U64_MAX and after >= before + 3500,
              f"motion with no button down is not input ({before} -> {after} ms)")

    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
