#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""#1199 check for `kicad --embedded`: the host's window requests in one order, and during a task.

- GetCapabilities lists `embed-host-order`; GetFrames reports `host` and `host_seq`.
- A main-socket SetHostGeometry is applied when KiCad answers (GetFrames.host is that rect).
- Hide, then a lone ShowFrame with no later geometry: the host is visible (lane M's return 3).
- A burst of geometries ends at the last rect; the events socket's geometry is applied too.
- A show, then a later hide: hidden.
- During a real RunChecks (DRC with zone refill on a large board), SetHostGeometry answers AS_OK
  and GetFrames.host is that rect at once, each one bracketed by a write request the task gate
  still refuses (AS_BUSY), which proves the task was running; the reply time is reported.

Runs on a private Xvfb + i3 with a python-xlib stand-in host.  SetHostRect itself only moves
windows on macOS; here the order and the gate are what is checked, through GetFrames.host.
"""

import argparse
from pathlib import Path
import statistics
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import (FAILURES, IpcSession, Session, check,  # noqa: E402
                                          viewable, wait_for)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--out", type=Path, default=Path("embed-host-order-out"))
    parser.add_argument("--project", type=Path,
                        default=Path(__file__).resolve().parents[1] / "demos/jetson-agx-thor-baseboard")
    args = parser.parse_args()

    with Session(args.kicad, args.out, args.project) as s, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=300000) as api:
        embed = api.module("backplane.embed")
        capabilities = api.module("backplane.capabilities")
        editor = api.module("common.commands.editor_commands")
        disp = s.host.display
        w, h = s.host.size

        caps = api.request(capabilities.GetCapabilities(), capabilities.GetCapabilitiesResponse)
        check("embed-host-order" in caps.capabilities, "GetCapabilities lists embed-host-order")

        def host():
            r = api.request(embed.GetFrames(), embed.GetFramesResponse)
            return r.host, r.host_seq

        def geometry(x, width, visible=True, raise_=False, sock=api):
            return sock.request_raw(embed.SetHostGeometry(x=x, y=153, width=width, height=500,
                                                         visible=visible, **{"raise": raise_}))

        def same(g, x, width, visible):
            return g.x == x and g.width == width and g.height == 500 and g.visible == visible

        r = api.request(embed.ShowFrame(kind=embed.FK_PCB, x=0, y=0, width=w, height=h), embed.ShowFrameResponse)
        wait_for(lambda: viewable(disp, r.frame.window), 120)
        time.sleep(2)

        # Applied when answered
        reply = geometry(430, 496)
        g, seq0 = host()
        check(reply.status.status == 1 and same(g, 430, 496, True),
              f"a main-socket SetHostGeometry is applied when answered ({g.x},{g.width},{g.visible})")

        # Hide, then a lone show (no later geometry): visible
        geometry(430, 496, visible=False)
        check(not host()[0].visible, "the host's hide is applied")
        api.request(embed.ShowFrame(kind=embed.FK_PCB), embed.ShowFrameResponse)
        g, _ = host()
        check(g.visible and same(g, 430, 496, True), "hide then a lone ShowFrame: the host is visible")

        # A burst ends at the last rect
        for x, width in ((430, 524), (430, 496), (47, 879)):
            geometry(x, width)
        api.request(embed.ShowFrame(kind=embed.FK_PCB), embed.ShowFrameResponse)
        g, seq1 = host()
        check(same(g, 47, 879, True) and seq1 > seq0, f"a burst ends at the last rect ({g.x},{g.width}, seq {seq1})")

        # A show, then a later hide: hidden
        api.request(embed.ShowFrame(kind=embed.FK_PCB), embed.ShowFrameResponse)
        geometry(47, 879, visible=False)
        check(not host()[0].visible, "a show then a later hide: hidden")
        geometry(47, 879)

        # The events socket's geometry, in the same order
        frames = api.request(embed.GetFrames(), embed.GetFramesResponse)
        events_sock = frames.events_socket.removeprefix("ipc://")
        with IpcSession(args.kicad_cli, api_socket=events_sock, timeout=60000) as ev:
            for ii in range(10):
                geometry(200 + ii, 600 + ii, sock=ev)
            ok = wait_for(lambda: same(host()[0], 209, 609, True), 5, 0.05)
            check(ok, f"the events socket's burst is applied, ending at its last rect ({host()[0].x})")

        # During a real task
        busy_probe = editor.RunAction(action="common.Control.zoomFitScreen")
        rc = api.request(embed.RunChecks(kind=embed.FK_PCB, refill_zones=True, parity=True),
                         embed.RunChecksResponse)
        during, times, refused = 0, [], []
        deadline = time.monotonic() + 300
        ii = 0
        while time.monotonic() < deadline:
            before = api.request_raw(busy_probe)
            if before.status.status != 7 or "a task is running" not in before.status.error_message:
                break   # the task is over
            ii += 1
            x = 300 + ii
            t0 = time.monotonic()
            reply = geometry(x, 700)
            times.append((time.monotonic() - t0) * 1000)
            g, _ = host()
            after = api.request_raw(busy_probe)
            if reply.status.status != 1:
                refused.append(reply.status.error_message[:80])
            elif same(g, x, 700, True) and "a task is running" in after.status.error_message:
                during += 1
            time.sleep(0.1)

        print(f"RunChecks progress {rc.progress_id}: {during} geometries applied mid-task, "
              f"{len(refused)} refused; reply ms median "
              f"{statistics.median(times) if times else 0:.0f}, max {max(times) if times else 0:.0f}",
              flush=True)
        check(during >= 3 and not refused,
              f"SetHostGeometry during RunChecks is answered AS_OK and applied at once "
              f"({during} bracketed by AS_BUSY probes, refused {refused[:2]})")

    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
