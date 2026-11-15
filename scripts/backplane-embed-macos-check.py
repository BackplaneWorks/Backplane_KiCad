#!/usr/bin/env python3
"""macOS embedded GUI check (BACKPLANE_EMBED.md, "macOS"): `kicad --embedded` over a stand-in host
window on a logged-in macOS desktop (a GitHub macOS runner, or a Mac under the hub lock mac-ui).

The host is a small Cocoa app (built here with clang) whose window id goes to --parent-window; the
check plays the client's part: it pushes the host's container rect with SetHostGeometry and drives
the frames over the API.  Keys are Quartz events (the posting process needs Accessibility, which
GitHub's runners grant); windows are read from CGWindowList (no permission needed).

Checks: capabilities; the PCB frame placed over the container, directly above the host, painted,
ET_FRAME_DRAWN; Cmd+Z, Cmd+S, Cmd+C and Cmd+V reach KiCad with no visible menu bar (an accessory
app); a reserved chord comes back as ET_RESERVED_KEY; a prewarmed frame's first show and its
ET_FRAME_DRAWN time; the first click on the canvas of an inactive KiCad acts; the host's
visible=false hides the frame while FrameInfo.shown stays; the host's process ending hides every
frame; Close.  Writes screenshots and a summary JSON to --out.

usage: backplane-embed-macos-check.py <bin/kicad> <bin/kicad-cli> [--out DIR]
"""

import argparse
import hashlib
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from pathlib import Path

import Quartz
from PIL import Image, ImageStat

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_ipc_test_support import IpcSession  # noqa: E402

FAILURES = []
RESULTS = {}


def check(condition, message):
    print(("PASS " if condition else "FAIL ") + message, flush=True)
    if not condition:
        FAILURES.append(message)
    return condition


def wait_for(predicate, seconds, step=0.05):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(step)
    return predicate()


HOST_SOURCE = r"""
#import <Cocoa/Cocoa.h>
// The stand-in host: a titled window; prints its window id and its content rect in screen points
// (top-left origin of the primary display), then runs until killed.
int main(int argc, char **argv) { @autoreleasepool {
  [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(40, 60, 1000, 640)
      styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
  w.title = @"Embed host"; w.backgroundColor = NSColor.darkGrayColor;
  [w makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
    double H = NSScreen.screens[0].frame.size.height; NSRect c = [w contentRectForFrameRect:w.frame];
    printf("%ld %.0f %.0f %.0f %.0f\n", (long)w.windowNumber, c.origin.x, H - (c.origin.y + c.size.height),
           c.size.width, c.size.height);
    fflush(stdout); });
  [NSApp run];
}}
"""

KEY = {"s": 1, "z": 6, "c": 8, "v": 9, "k": 40, "escape": 53}


def key(name, command=True):
    for down in (True, False):
        event = Quartz.CGEventCreateKeyboardEvent(None, KEY[name], down)
        Quartz.CGEventSetFlags(event, Quartz.kCGEventFlagMaskCommand if command else 0)
        Quartz.CGEventPost(Quartz.kCGHIDEventTap, event)
        time.sleep(0.03)


def click(x, y):
    point = Quartz.CGPointMake(x, y)
    for kind in (Quartz.kCGEventMouseMoved, Quartz.kCGEventLeftMouseDown, Quartz.kCGEventLeftMouseUp):
        event = Quartz.CGEventCreateMouseEvent(None, kind, point, Quartz.kCGMouseButtonLeft)
        Quartz.CGEventPost(Quartz.kCGHIDEventTap, event)
        time.sleep(0.05)


def windows():
    """On-screen windows of layer 0, front to back: [(number, pid, (x, y, w, h))]."""
    out = []
    for w in Quartz.CGWindowListCopyWindowInfo(Quartz.kCGWindowListOptionOnScreenOnly, Quartz.kCGNullWindowID):
        if w.get("kCGWindowLayer", 0) != 0:
            continue
        b = w["kCGWindowBounds"]
        out.append((int(w["kCGWindowNumber"]), int(w["kCGWindowOwnerPID"]),
                    (round(b["X"]), round(b["Y"]), round(b["Width"]), round(b["Height"]))))
    return out


def on_screen(number):
    return next((w for w in windows() if w[0] == number), None)


def directly_above(number, host):
    order = [w[0] for w in windows()]
    return number in order and host in order and order.index(number) == order.index(host) - 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--out", type=Path, default=Path("embed-macos-out"))
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    # Short: a Unix socket path is at most 104 bytes on macOS, and $TMPDIR (/var/folders/...) plus
    # "api.sock.events" went past it
    work = Path(tempfile.mkdtemp(prefix="bpem-", dir="/tmp"))
    env = dict(os.environ)
    for variable, directory in (("HOME", "home"), ("KICAD_CONFIG_HOME", "config"), ("TMPDIR", "tmp")):
        path = work / directory
        path.mkdir(parents=True, exist_ok=True, mode=0o700)
        env[variable] = str(path)
    for version in ("", "10.0", "10.99"):
        config = work / "config" / version
        config.mkdir(parents=True, exist_ok=True)
        (config / "sym-lib-table").write_text("(sym_lib_table\n  (version 7)\n)\n")
        (config / "fp-lib-table").write_text("(fp_lib_table\n  (version 7)\n)\n")
        (config / "design-block-lib-table").write_text("(design_block_lib_table\n  (version 7)\n)\n")
    env["BACKPLANE_KICAD_EMBED_TRACE"] = str(args.out / "embed-trace.log")

    source = Path(__file__).resolve().parents[1]
    project_dir = work / "pic_programmer"
    shutil.copytree(source / "demos/pic_programmer", project_dir)
    project = project_dir / "pic_programmer.kicad_pro"
    board_file = project_dir / "pic_programmer.kicad_pcb"

    def shot(label, box=None):
        path = args.out / f"{label}.png"
        subprocess.run(["screencapture", "-x", str(path)], check=False)
        if box and path.exists():
            image = Image.open(path)
            scale = image.size[0] / Quartz.CGDisplayBounds(Quartz.CGMainDisplayID()).size.width
            x, y, w, h = (int(v * scale) for v in box)
            return path, ImageStat.Stat(image.convert("L").crop((x + w // 4, y + h // 4, x + 3 * w // 4, y + 3 * h // 4))).stddev[0]
        return path, None

    host_app = work / "host"
    subprocess.run(["clang", "-fobjc-arc", "-O2", "-framework", "Cocoa", "-x", "objective-c", "-o", str(host_app), "-"],
                   input=HOST_SOURCE, text=True, check=True)

    host = kicad = None
    try:
        host = subprocess.Popen([str(host_app)], stdout=subprocess.PIPE, text=True)
        hid, hx, hy, hw, hh = (int(float(v)) for v in host.stdout.readline().split())
        print(f"host window {hid}, content at {hx},{hy} {hw}x{hh}", flush=True)
        # The pane: the host's content less a 200 pt column on the left (the host's own area, for clicks)
        px, py, pw, ph = 200, 0, hw - 200, hh

        socket = work / "tmp/api.sock"
        log = (args.out / "kicad.log").open("w")
        kicad = subprocess.Popen([str(args.kicad), "--embedded", "--parent-window", str(hid), "--toplevel-window", str(hid),
                                  "--api-socket", str(socket), "--parent-pid", str(os.getpid()), str(project)],
                                 env=env, stdout=subprocess.PIPE, stderr=log, text=True)
        ready = None
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline:
            line = kicad.stdout.readline()
            if not line:
                break
            print("kicad: " + line.rstrip(), flush=True)
            if line.startswith("KiCad API server listening at "):
                ready = line.split(" at ", 1)[1].strip()
                break
        threading.Thread(target=lambda: [print("kicad: " + l.rstrip(), flush=True) for l in kicad.stdout], daemon=True).start()
        if not check(ready == f"ipc://{socket}", f"KiCad --embedded starts on macOS ({ready})"):
            return

        with IpcSession(args.kicad_cli, api_socket=socket, timeout=120000) as api, \
                IpcSession(args.kicad_cli, api_socket=Path(str(socket) + ".events"), timeout=120000) as events_api:
            embed = api.module("backplane.embed")
            caps = api.module("backplane.capabilities")
            editor = api.module("common.commands.editor_commands")
            board = api.module("board.board_types")
            types = api.module("common.types.base_types")
            enums = api.module("common.types.enums")

            capabilities = set(api.request(caps.GetCapabilities(), caps.GetCapabilitiesResponse).capabilities)
            for name in ("embed", "embed-macos", "embed-host-geometry", "embed-reserved-key-event"):
                check(name in capabilities, f"capability {name}")
            check("prewarm-keeps-focus" not in capabilities, "prewarm-keeps-focus is not advertised on macOS")

            seen = []
            since = [0]

            def pump(wait_ms=200):
                reply = events_api.request(embed.NextEvents(since=since[0], wait_ms=wait_ms), embed.NextEventsResponse)
                since[0] = reply.next
                seen.extend(reply.events)
                return reply.events

            def wait_event(kind, seconds, after=0.0):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    for e in pump(200):
                        if e.type == kind:
                            return time.monotonic()
                return None

            pump(0)

            def frames():
                return {f.kind: f for f in api.request(embed.GetFrames(), embed.GetFramesResponse).frames}

            def host_geometry(visible=True, raise_=False):
                # `raise` is a Python keyword: set it by name
                api.request(embed.SetHostGeometry(x=hx + px, y=hy + py, width=pw, height=ph, visible=visible,
                                                  **{"raise": raise_}),
                            embed.SetHostGeometryResponse)

            host_geometry()
            t0 = time.monotonic()
            shown = api.request(embed.ShowFrame(kind=embed.FK_PCB, focus=True, x=0, y=0, width=pw, height=ph), embed.ShowFrameResponse)
            pcb = shown.frame.window
            drawn = wait_event(embed.ET_FRAME_DRAWN, 60)
            RESULTS["pcb_first_show_drawn_ms"] = round((drawn - t0) * 1000) if drawn else None
            check(drawn is not None, f"ET_FRAME_DRAWN after the PCB's first ShowFrame ({RESULTS['pcb_first_show_drawn_ms']} ms)")
            placed = wait_for(lambda: on_screen(pcb), 5)
            expected = (hx + px, hy + py, pw, ph)
            check(placed and all(abs(a - b) <= 2 for a, b in zip(placed[2], expected)),
                  f"the PCB frame sits over the container ({placed and placed[2]} vs {expected})")
            check(wait_for(lambda: directly_above(pcb, hid), 3), "the PCB frame is directly above the host window")
            time.sleep(1.5)
            path, spread = shot("01-pcb", expected)
            check(spread is not None and spread > 4, f"the PCB canvas is painted ({path.name}, spread {spread and round(spread, 1)})")
            check(wait_for(lambda: frames()[embed.FK_PCB].focused, 5), "ShowFrame{focus} gives the PCB frame the keyboard")

            documents = api.request(editor.GetOpenDocuments(type=types.DOCTYPE_PCB), editor.GetOpenDocumentsResponse)
            header = types.ItemHeader(document=documents.documents[0])

            def add_track():
                kiid = str(uuid.uuid4())
                transaction = api.request(editor.BeginCommit(header=header), editor.BeginCommitResponse)
                create = editor.CreateItems(header=header)
                create.items.add().Pack(board.Track(id=types.KIID(value=kiid), start=types.Vector2(x_nm=100000000, y_nm=100000000),
                                                    end=types.Vector2(x_nm=110000000, y_nm=100000000),
                                                    width=types.Distance(value_nm=250000), layer=board.BL_F_Cu))
                api.request(create, editor.CreateItemsResponse)
                api.request(editor.EndCommit(header=header, id=transaction.id, action=editor.CMA_COMMIT, message="macos check"),
                            editor.EndCommitResponse)
                return kiid

            def track_ids():
                reply = api.request(editor.GetItems(header=header, types=[enums.KOT_PCB_TRACE]), editor.GetItemsResponse)
                ids = set()
                for item in reply.items:
                    track = board.Track()
                    if item.Unpack(track):
                        ids.add(track.id.value)
                return ids

            # Cmd+Z: the menu bar's key equivalent in an accessory app with no visible menu bar
            kiid = add_track()
            check(kiid in track_ids(), "an agent's track is on the board")
            key("z")
            check(wait_for(lambda: kiid not in track_ids(), 5), "Cmd+Z undoes it (no visible menu bar)")

            # Cmd+S
            original = hashlib.sha256(board_file.read_bytes()).hexdigest()
            kiid = add_track()
            wait_for(lambda: frames()[embed.FK_PCB].modified, 5)
            key("s")
            saved = wait_for(lambda: hashlib.sha256(board_file.read_bytes()).hexdigest() != original, 10)
            check(saved and wait_for(lambda: not frames()[embed.FK_PCB].modified, 5), "Cmd+S saves the board")

            # Cmd+C, Cmd+V (and a click to place the paste)
            subprocess.run(["pbcopy"], input="", text=True)
            api.request(editor.AddToSelection(header=header, items=[types.KIID(value=kiid)]), editor.SelectionResponse)
            time.sleep(0.3)
            key("c")
            copied = wait_for(lambda: "segment" in subprocess.run(["pbpaste"], text=True, stdout=subprocess.PIPE).stdout, 5)
            check(bool(copied), "Cmd+C copies the selected track to the clipboard")
            before = len(track_ids())
            key("v")
            time.sleep(0.8)
            click(hx + px + pw * 0.3, hy + py + ph * 0.3)
            time.sleep(0.5)
            check(wait_for(lambda: len(track_ids()) == before + 1, 5), "Cmd+V pastes it (placed with a click)")
            key("escape", command=False)

            # A reserved chord comes back as ET_RESERVED_KEY
            api.request(embed.SetReservedKeys(chords=["Ctrl+K"]), embed.SetReservedKeysResponse)
            pump(0)
            key("k")
            got = None
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and not got:
                got = next((e for e in pump(200) if e.type == embed.ET_RESERVED_KEY), None)
            check(got is not None and got.reserved_key.index == 0, f"Cmd+K arrives as ET_RESERVED_KEY ({got and got.reserved_key})")

            # Prewarm the schematic, then show it: its first show must paint quickly (an occluded
            # transparent window may not be drawn by the window server)
            api.request(embed.ShowFrame(kind=embed.FK_SCH, prewarm=True, width=pw, height=ph), embed.ShowFrameResponse)
            time.sleep(4)
            sch_frame = frames().get(embed.FK_SCH)
            check(sch_frame is not None and sch_frame.realized and not sch_frame.shown, "the prewarmed schematic is realized, not shown")
            check(frames()[embed.FK_PCB].focused, "the prewarm left the keyboard with the PCB frame")
            api.request(embed.HideFrame(kind=embed.FK_PCB), embed.HideFrameResponse)
            pump(0)
            t0 = time.monotonic()
            api.request(embed.ShowFrame(kind=embed.FK_SCH, x=0, y=0, width=pw, height=ph), embed.ShowFrameResponse)
            drawn = wait_event(embed.ET_FRAME_DRAWN, 30)
            RESULTS["sch_after_prewarm_drawn_ms"] = round((drawn - t0) * 1000) if drawn else None
            check(drawn is not None and drawn - t0 < 1.0,
                  f"the prewarmed schematic's first show draws within 1 s ({RESULTS['sch_after_prewarm_drawn_ms']} ms)")
            shot("02-sch-after-prewarm")
            api.request(embed.HideFrame(kind=embed.FK_SCH), embed.HideFrameResponse)
            api.request(embed.ShowFrame(kind=embed.FK_PCB, x=0, y=0, width=pw, height=ph), embed.ShowFrameResponse)
            wait_event(embed.ET_FRAME_DRAWN, 10)

            # First click: the host takes the keyboard (a click in its own area), then one click on
            # the board's empty canvas must act: it clears an agent's selection
            click(hx + 100, hy + 200)
            time.sleep(0.5)
            host_geometry(raise_=True)   # the client re-orders after a click in its window
            check(wait_for(lambda: directly_above(pcb, hid), 3), "after a click in the host the frame is above it again")
            api.request(editor.AddToSelection(header=header, items=[types.KIID(value=kiid)]), editor.SelectionResponse)
            selected = lambda: len(api.request(editor.GetSelection(header=header), editor.SelectionResponse).items)
            wait_for(lambda: selected() > 0, 3)
            click(hx + px + pw - 30, hy + py + ph - 60)
            check(wait_for(lambda: selected() == 0, 3), "the first click on an inactive KiCad's canvas acts (selection cleared)")
            check(wait_for(lambda: frames()[embed.FK_PCB].focused, 3), "and gives KiCad the keyboard")

            # The host's own visibility is an overlay: the frame leaves the screen, FrameInfo.shown stays
            host_geometry(visible=False)
            check(wait_for(lambda: not on_screen(pcb), 2), "SetHostGeometry{visible=false} takes the frame off screen")
            check(frames()[embed.FK_PCB].shown, "FrameInfo.shown stays true while the host hides it")
            check(not any(e.type == embed.ET_FRAME_HIDDEN for e in pump(300)), "no ET_FRAME_HIDDEN for a host hide")
            host_geometry(visible=True)
            check(wait_for(lambda: on_screen(pcb) and directly_above(pcb, hid), 2), "visible=true puts it back above the host")

            # The host's process ends: every frame leaves the screen at once
            t0 = time.monotonic()
            host.send_signal(signal.SIGKILL)
            gone = wait_for(lambda: not on_screen(pcb), 3, 0.005)
            RESULTS["frame_gone_after_host_killed_ms"] = round((time.monotonic() - t0) * 1000)
            check(gone, f"the host's death hides the frame ({RESULTS['frame_gone_after_host_killed_ms']} ms)")

            final = api.request(embed.Close(mode=embed.CM_DISCARD), embed.CloseResponse)
            check(final.exiting, "Close discard answers exiting")
            check(wait_for(lambda: kicad.poll() is not None, 30), f"KiCad exited (status {kicad.poll()})")
    finally:
        for process in (kicad, host):
            if process is not None and process.poll() is None:
                process.kill()
        (args.out / "summary.json").write_text(json.dumps({"failures": FAILURES, "results": RESULTS}, indent=2))
        print(json.dumps({"failures": FAILURES, "results": RESULTS}, indent=2), flush=True)
    if FAILURES:
        sys.exit(f"{len(FAILURES)} macOS embed check(s) failed")


if __name__ == "__main__":
    main()
