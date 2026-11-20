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
  // Commands on stdin, as the client would do them: "yield <pid>" hands activation to that app
  // (macOS 14+ cooperative activation: a background app's own activate is ignored otherwise),
  // "activate <pid>" activates it from here
  [NSThread detachNewThreadWithBlock:^{
    char line[128];
    while (fgets(line, sizeof line, stdin)) {
      char verb[32]; int pid = 0;
      if (sscanf(line, "%31s %d", verb, &pid) != 2) continue;
      NSString *v = @(verb);
      dispatch_async(dispatch_get_main_queue(), ^{
        NSRunningApplication *app = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
        NSWindow *w = NSApp.windows.firstObject;
        for (NSWindow *x in NSApp.windows) if ([x.title isEqual:@"Embed host"]) w = x;
        if ([v isEqual:@"mini"]) [w miniaturize:nil];
        else if ([v isEqual:@"demini"]) { [w deminiaturize:nil]; [NSApp activateIgnoringOtherApps:YES]; }
        else if ([v isEqual:@"full"]) [w toggleFullScreen:nil];
        else if ([v isEqual:@"front"]) { [NSApp activateIgnoringOtherApps:YES]; [w makeKeyAndOrderFront:nil]; }
        else if ([v isEqual:@"rect"]) {
          double H = NSScreen.screens[0].frame.size.height; NSRect c = [w contentRectForFrameRect:w.frame];
          printf("rect %.0f %.0f %.0f %.0f %d\n", c.origin.x, H - (c.origin.y + c.size.height), c.size.width, c.size.height,
                 (int)((w.styleMask & NSWindowStyleMaskFullScreen) != 0)); fflush(stdout); return; }
        else if ([v isEqual:@"yield"]) { if (@available(macOS 14.0, *)) [NSApp yieldActivationToApplication:app]; }
        else if ([v isEqual:@"activate"]) {
          if (@available(macOS 14.0, *)) [app activateWithOptions:0];
          else [app activateWithOptions:NSApplicationActivateIgnoringOtherApps];
        }
        printf("did %s %d\n", v.UTF8String, pid); fflush(stdout); });
    }
  }];
  [NSApp run];
}}
"""

KEY = {"a": 0, "s": 1, "z": 6, "c": 8, "v": 9, "k": 40, "escape": 53}


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


def press(x, y, hold, button="left"):
    """A click whose button is held `hold` seconds (the activation of an inactive KiCad lands inside it)."""
    point = Quartz.CGPointMake(x, y)
    down, up, which = ((Quartz.kCGEventLeftMouseDown, Quartz.kCGEventLeftMouseUp, Quartz.kCGMouseButtonLeft) if button == "left"
                       else (Quartz.kCGEventRightMouseDown, Quartz.kCGEventRightMouseUp, Quartz.kCGMouseButtonRight))
    Quartz.CGEventPost(Quartz.kCGHIDEventTap, Quartz.CGEventCreateMouseEvent(None, Quartz.kCGEventMouseMoved, point, which))
    time.sleep(0.05)
    Quartz.CGEventPost(Quartz.kCGHIDEventTap, Quartz.CGEventCreateMouseEvent(None, down, point, which))
    time.sleep(hold)
    Quartz.CGEventPost(Quartz.kCGHIDEventTap, Quartz.CGEventCreateMouseEvent(None, up, point, which))
    time.sleep(0.05)


def drag(x1, y1, x2, y2, seconds, steps=30):
    """A left-button drag from (x1, y1) to (x2, y2) over `seconds`."""
    start = Quartz.CGPointMake(x1, y1)
    Quartz.CGEventPost(Quartz.kCGHIDEventTap, Quartz.CGEventCreateMouseEvent(None, Quartz.kCGEventMouseMoved, start, Quartz.kCGMouseButtonLeft))
    time.sleep(0.05)
    Quartz.CGEventPost(Quartz.kCGHIDEventTap, Quartz.CGEventCreateMouseEvent(None, Quartz.kCGEventLeftMouseDown, start, Quartz.kCGMouseButtonLeft))
    for i in range(1, steps + 1):
        time.sleep(seconds / steps)
        point = Quartz.CGPointMake(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps)
        Quartz.CGEventPost(Quartz.kCGHIDEventTap, Quartz.CGEventCreateMouseEvent(None, Quartz.kCGEventLeftMouseDragged, point, Quartz.kCGMouseButtonLeft))
    Quartz.CGEventPost(Quartz.kCGHIDEventTap, Quartz.CGEventCreateMouseEvent(None, Quartz.kCGEventLeftMouseUp, Quartz.CGPointMake(x2, y2), Quartz.kCGMouseButtonLeft))
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
    started = time.time()
    env = dict(os.environ)
    for variable, directory in (("HOME", "home"), ("KICAD_CONFIG_HOME", "config"), ("TMPDIR", "tmp")):
        path = work / directory
        path.mkdir(parents=True, exist_ok=True, mode=0o700)
        env[variable] = str(path)
    # KiCad on macOS keeps its settings in ~/Library/Preferences/kicad (KICAD_CONFIG_HOME aside):
    # empty tables in both, so no first-run library dialog stops the UI thread
    for version in [d / v for d in (Path("config"), Path("home/Library/Preferences/kicad")) for v in ("", "10.0", "10.99")]:
        config = work / version
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

    # The runtime's minimum macOS: the highest minos of KiCad's binary and its bundled libraries
    bundle = (args.kicad.resolve().parent.parent / "KiCad.app").resolve()
    minos = {}
    for binary in [bundle / "Contents/MacOS/kicad", *sorted((bundle / "Contents/Frameworks").glob("*.dylib"))]:
        out = subprocess.run(["vtool", "-show-build", str(binary)], text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout
        for line in out.splitlines():
            if line.strip().startswith("minos"):
                minos[binary.name] = line.split()[1]
    if minos:
        top = max(minos.values(), key=lambda v: tuple(int(p) for p in v.split(".")))
        RESULTS["runtime_min_macos"] = top
        RESULTS["runtime_min_macos_from"] = sorted(n for n, v in minos.items() if v == top)[:8]
        print(f"runtime minimum macOS: {top} (from {RESULTS['runtime_min_macos_from']})", flush=True)

    host_app = work / "host"
    subprocess.run(["clang", "-fobjc-arc", "-O2", "-mmacosx-version-min=14.0", "-framework", "Cocoa", "-x", "objective-c", "-o", str(host_app), "-"],
                   input=HOST_SOURCE, text=True, check=True)

    host = kicad = None
    try:
        host = subprocess.Popen([str(host_app)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
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

            def tell_host(verb):
                host.stdin.write(f"{verb} {kicad.pid}\n")
                host.stdin.flush()
                time.sleep(0.3)

            host_geometry()
            tell_host("yield")   # the client hands activation to KiCad before a focused show
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
            focused = wait_for(lambda: frames()[embed.FK_PCB].focused, 5)
            RESULTS["focus_after_yield"] = bool(focused)
            if not focused:
                # The other way: the client activates KiCad itself, then asks for focus again
                tell_host("activate")
                api.request(embed.ShowFrame(kind=embed.FK_PCB, focus=True, x=0, y=0, width=pw, height=ph), embed.ShowFrameResponse)
                focused = wait_for(lambda: frames()[embed.FK_PCB].focused, 5)
                RESULTS["focus_after_host_activates_kicad"] = bool(focused)
            check(focused, f"ShowFrame{{focus}} gives the PCB frame the keyboard ({RESULTS})")

            # the keyboard is on the canvas, where KiCad's hotkeys are served (not a toolbar field): Cmd+A selects everything, then a
            # plain Escape (no modifier) clears it; each is the person's selection change. No click before: ShowFrame{focus} alone.
            def selection_event(seconds=3):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    if any(e.type == embed.ET_SELECTION_CHANGED for e in pump(200)):
                        return True
                return False

            pump(0)
            key("a")
            all_selected = selection_event()
            key("escape", command=False)
            cleared = selection_event()
            check(all_selected and cleared, f"after ShowFrame{{focus}} the canvas has the keyboard: Cmd+A and a plain Escape act ({all_selected}, {cleared})")

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

            # (Cmd+Z is checked after the paste below: the person's Cmd+Z skips agent changes,
            # docs/backplane/coedit.md "Per-actor undo")

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
            time.sleep(0.3)
            # Cmd+Z: the person's own change (the paste) goes back; the agent's tracks stay
            key("z")
            check(wait_for(lambda: len(track_ids()) == before, 5), "Cmd+Z undoes the person's paste (no visible menu bar)")
            check(kiid in track_ids(), "and leaves the agent's track")

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
            # Prewarming must pay off on macOS too (a transparent window behind everything could be
            # counted occluded and never painted): well under the board's cold first show
            cold = RESULTS["pcb_first_show_drawn_ms"] or 0
            check(drawn is not None and RESULTS["sch_after_prewarm_drawn_ms"] < cold / 2,
                  f"the prewarmed schematic's first show draws in under half a cold show "
                  f"({RESULTS['sch_after_prewarm_drawn_ms']} ms vs {cold} ms)")
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
            # one click on the board (its parts: the middle of the board's area) must select there: ET_SELECTION_CHANGED, the person's
            # selection the hub cross-probes from (an agent's own selection is its own under co-edit, so clearing that proves nothing)
            pump(0)
            click(hx + px + pw * 0.35, hy + py + ph * 0.45)
            got = None
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline and not got:
                got = next((e for e in pump(200) if e.type == embed.ET_SELECTION_CHANGED), None)
            RESULTS["first_click_acted"] = got is not None
            if got is None:
                click(hx + px + pw * 0.35, hy + py + ph * 0.45)
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline and not got:
                    got = next((e for e in pump(200) if e.type == embed.ET_SELECTION_CHANGED), None)
                RESULTS["second_click_acted"] = got is not None
            shot("02b-first-click")
            check(RESULTS["first_click_acted"], f"the first click on an inactive KiCad's board selects there (ET_SELECTION_CHANGED; {RESULTS.get('second_click_acted')})")
            check(wait_for(lambda: frames()[embed.FK_PCB].focused, 3), "and gives KiCad the keyboard")

            # The host's own visibility is an overlay: the frame leaves the screen, FrameInfo.shown stays
            host_geometry(visible=False)
            check(wait_for(lambda: not on_screen(pcb), 2), "SetHostGeometry{visible=false} takes the frame off screen")
            check(frames()[embed.FK_PCB].shown, "FrameInfo.shown stays true while the host hides it")
            check(not any(e.type == embed.ET_FRAME_HIDDEN for e in pump(300)), "no ET_FRAME_HIDDEN for a host hide")
            host_geometry(visible=True)
            check(wait_for(lambda: on_screen(pcb) and directly_above(pcb, hid), 2), "visible=true puts it back above the host")

            # The hide rules on real KiCad, as the client drives them
            def host_say(verb, arg=0):
                host.stdin.write(f"{verb} {arg}\n")
                host.stdin.flush()

            def host_rect():
                host_say("rect")
                while True:
                    line = host.stdout.readline()
                    if not line:
                        return None
                    if line.startswith("rect "):
                        x, y, w, h, full = (int(float(v)) for v in line.split()[1:])
                        return x, y, w, h, full

            def push(visible=True, raise_=False):
                nonlocal hx, hy, hw, hh, pw, ph
                r = host_rect()
                if r:
                    hx, hy, hw, hh, _ = r
                    pw, ph = hw - px, hh - py
                host_geometry(visible=visible, raise_=raise_)

            # minimised and back
            host_say("mini")
            push(visible=False)
            check(wait_for(lambda: not on_screen(pcb), 3), "host minimised: the frame leaves the screen")
            time.sleep(1.0)
            host_say("demini")
            time.sleep(1.0)
            push(visible=True, raise_=True)
            check(wait_for(lambda: on_screen(pcb) and directly_above(pcb, hid), 3), "host back from the Dock: the frame is back above it")

            # full screen: hidden (another app's window cannot join our full-screen Space), and after
            # the transition out the client pushes again and the frame is back in place
            host_say("full")
            push(visible=False)
            time.sleep(2.5)
            check(not on_screen(pcb), "host full screen: the frame is hidden")
            host_say("full")
            time.sleep(2.5)
            push(visible=True, raise_=True)
            r = host_rect()
            expected = (r[0] + px, r[1] + py, r[2] - px, r[3] - py) if r else None
            placed = wait_for(lambda: on_screen(pcb), 3)
            check(placed and expected and all(abs(a - b) <= 2 for a, b in zip(placed[2], expected))
                  and directly_above(pcb, hid), f"out of full screen: the re-push puts the frame back ({placed and placed[2]} vs {expected})")

            # the pane is not the visible tab: the host hides the frame (HideFrame), then shows it again
            api.request(embed.HideFrame(kind=embed.FK_PCB), embed.HideFrameResponse)
            check(wait_for(lambda: not on_screen(pcb), 3) and not frames()[embed.FK_PCB].shown, "another tab: HideFrame takes it off screen")
            api.request(embed.ShowFrame(kind=embed.FK_PCB, x=0, y=0, width=pw, height=ph), embed.ShowFrameResponse)
            check(wait_for(lambda: on_screen(pcb) and directly_above(pcb, hid), 3), "back to the tab: ShowFrame puts it back above the host")

            # a modal dialog (Board Setup) stays above the host, centred on it, and is reported
            frame_numbers = {f.window for f in frames().values()}

            def kicad_windows_above_host():
                # KiCad's windows above the host that are not its frames: the dialog
                order = windows()
                at = next((i for i, w in enumerate(order) if w[0] == hid), len(order))
                return [w for w in order[:at] if w[1] == kicad.pid and w[0] not in frame_numbers]

            host_say("yield", kicad.pid)
            pump(0)
            runner = threading.Thread(target=lambda: api.request_raw(editor.RunAction(action="pcbnew.EditorControl.boardSetup")), daemon=True)
            runner.start()
            dialog = wait_for(lambda: kicad_windows_above_host(), 10, 0.2)
            path, _ = shot("03-board-setup")
            check(bool(dialog), f"a KiCad dialog opens above the host ({dialog and dialog[0][2]}, {path.name})")
            if dialog:
                dx, dy, dw, dh = dialog[0][2]
                cx, cy = dx + dw / 2, dy + dh / 2
                check(r is not None and r[0] <= cx <= r[0] + r[2] and r[1] <= cy <= r[1] + r[3], "the dialog is centred over the host")
                # above the frame too, not between the host and it (there the frame covered it: lane M's client run 18, Find unseen)
                order = [w[0] for w in windows()]
                check(dialog[0][0] in order and pcb in order and order.index(dialog[0][0]) < order.index(pcb), "the dialog is above the frame, not under it")
            check(any(e.type == embed.ET_DIALOG_OPENED for e in pump(500)), "ET_DIALOG_OPENED reported")
            # the person closes it: its Cancel button (bottom right; an agent's RunAction opened it, so KiCad was not the active app)
            if dialog:
                dx, dy, dw, dh = dialog[0][2]
                click(dx + dw - 150, dy + dh - 24)
                time.sleep(0.5)
            runner.join(10)
            check(wait_for(lambda: not kicad_windows_above_host(), 5), "its Cancel button closes the dialog")
            check(any(e.type == embed.ET_DIALOG_CLOSED for e in pump(500)), "ET_DIALOG_CLOSED reported")

            # a popup menu (right click on the canvas) shows above the host
            def popups():
                return [w for w in Quartz.CGWindowListCopyWindowInfo(Quartz.kCGWindowListOptionOnScreenOnly, Quartz.kCGNullWindowID)
                        if w.get("kCGWindowOwnerPID") == kicad.pid and w.get("kCGWindowLayer", 0) > 0]
            point = Quartz.CGPointMake(hx + px + pw * 0.1, hy + py + ph * 0.86)
            for kind in (Quartz.kCGEventRightMouseDown, Quartz.kCGEventRightMouseUp):
                Quartz.CGEventPost(Quartz.kCGHIDEventTap, Quartz.CGEventCreateMouseEvent(None, kind, point, Quartz.kCGMouseButtonRight))
                time.sleep(0.1)
            menu = wait_for(popups, 3, 0.1)
            shot("04-context-menu")
            check(bool(menu), f"a right click opens KiCad's context menu above everything (layer {menu and menu[0].get('kCGWindowLayer')})")
            key("escape", command=False)
            check(wait_for(lambda: not popups(), 3), "Escape closes it")

            # Row 1j (#1198, bp.13.3 b2): the click that activates an inactive KiCad acts as the click it was. Each case first gives the
            # host the keyboard (a click in its own area: KiCad resigns active), then clicks KiCad with the button held long enough
            # for the activation to land between the down and the up (b2: 19 ms after the down; the frame made key then reset the
            # canvas's buttons, so the up had no down)
            def kicad_inactive(case):
                """The host takes the keyboard; checked, as its own line: a case run on a KiCad still active proves nothing."""
                click(hx + 100, hy + 200)
                time.sleep(0.5)
                host_geometry(raise_=True)
                wait_for(lambda: directly_above(pcb, hid), 3)
                import AppKit   # pyobjc-framework-Cocoa: a dependency of the Quartz framework the check already uses
                front = lambda: AppKit.NSWorkspace.sharedWorkspace().frontmostApplication()   # noqa: E731
                resigned = wait_for(lambda: front() is not None and front().processIdentifier() != kicad.pid
                                    and not frames()[embed.FK_PCB].focused, 3)
                return check(bool(resigned), f"1j {case}: before it, KiCad is not the active app and its frame has no focus")

            def selection_event(seconds=3):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    got = next((e for e in pump(200) if e.type == embed.ET_SELECTION_CHANGED), None)
                    if got:
                        return got
                return None

            def selected(got):
                """What a selection event names: its references, nets and pins (a track or a zone has no reference, only its net)."""
                return [*got.selection.references, *got.selection.nets, *got.selection.pins] if got is not None else None

            def clear_selection():
                """Escape in the canvas (KiCad has the keyboard), and its own ET_SELECTION_CHANGED waited for: a late one must
                never be taken for the next case's click."""
                pump(0)
                key("escape", command=False)
                selection_event(1.0)
                pump(300)

            def names_footprint(got, ref):
                return got is not None and (ref in got.selection.references or any(p.startswith(ref + ".") for p in got.selection.pins))

            # Every point a case clicks is aimed and asserted first, with KiCad active: a probe click there must give the selection
            # the case needs (KiCad's own hit test, at whatever size and zoom the pane has here), never a fraction of the pane taken
            # on trust (the mini's pane put 35%/45% on empty board)
            def find_empty():
                for fx, fy in ((0.1, 0.86), (0.08, 0.92), (0.15, 0.8), (0.05, 0.6), (0.5, 0.92)):
                    pt = (hx + px + pw * fx, hy + py + ph * fy)
                    pump(0)
                    key("a")
                    if not selected(selection_event()):
                        continue
                    click(*pt)
                    got = selection_event()
                    if got is not None and not selected(got):
                        return pt
                    clear_selection()
                return None

            def find_on(ref):
                """The board zoomed to footprint `ref` (SelectItems), then a point on it found by probe clicks."""
                api.request(embed.SelectItems(kind=embed.FK_PCB, references=[ref], zoom=True), embed.SelectItemsResponse)
                time.sleep(0.8)
                clear_selection()
                for fx, fy in ((0.4, 0.5), (0.35, 0.5), (0.3, 0.5), (0.45, 0.5), (0.35, 0.45), (0.35, 0.55), (0.25, 0.5)):
                    pt = (hx + px + pw * fx, hy + py + ph * fy)
                    pump(0)
                    click(*pt)
                    got = selection_event()
                    if names_footprint(got, ref):
                        clear_selection()
                        return pt
                    if got is not None:
                        clear_selection()
                return None

            def footprint_places():
                out = {}
                for any_item in api.request(editor.GetItems(header=header, types=[enums.KOT_PCB_FOOTPRINT]), editor.GetItemsResponse).items:
                    fp = board.FootprintInstance()
                    any_item.Unpack(fp)
                    out[fp.id.value] = (fp.position.x_nm, fp.position.y_nm)
                return out

            # the largest footprint (its body is the easiest target), by its bounding box
            footprints = api.request(editor.GetItems(header=header, types=[enums.KOT_PCB_FOOTPRINT]), editor.GetItemsResponse).items
            target, best = None, -1
            for any_item in footprints:
                fp = board.FootprintInstance()
                any_item.Unpack(fp)
                box = api.request(editor.GetBoundingBox(header=header, items=[fp.id], mode=editor.BBM_ITEM_ONLY), editor.GetBoundingBoxResponse)
                if box.boxes:
                    area = box.boxes[0].size.x_nm * box.boxes[0].size.y_nm
                    if area > best and fp.reference_field.text.text.text:
                        target, best = fp.reference_field.text.text.text, area
            RESULTS["1j_target"] = target

            empty = find_empty()
            check(empty is not None, f"1j aim: a point of empty canvas found and proved by a probe click ({empty})")

            # the right click again, held: the menu opens, stays open until dismissed, and once it is closed the frame is key
            if empty:
                kicad_inactive("held right click")
                press(*empty, 0.3, button="right")
                menu = wait_for(popups, 3, 0.1)
                time.sleep(1.0)
                check(bool(menu) and bool(popups()), "1j a held right click on an inactive KiCad opens the context menu, and it stays open")
                key("escape", command=False)
                check(wait_for(lambda: not popups(), 3), "1j Escape closes it")
                pump(0)
                key("a")
                got = selection_event()
                check(bool(selected(got)), f"1j after the menu, the frame has the keyboard: Cmd+A selects ({got and len(selected(got))})")
                clear_selection()

            # a drag begun on an inactive KiCad (a selection box from empty canvas, 1 s) completes as a drag, and moves nothing
            if empty:
                kicad_inactive("drag")
                places = footprint_places()
                pump(0)
                drag(*empty, hx + px + pw * 0.6, hy + py + ph * 0.5, 1.0)
                got = selection_event()
                check(bool(selected(got)), f"1j a 1 s drag begun on an inactive KiCad selects with its box ({got and len(selected(got))} names)")
                check(footprint_places() == places, "1j and moved nothing (every footprint where it was)")
                check(wait_for(lambda: frames()[embed.FK_PCB].focused, 3), "1j and KiCad has the keyboard after the drag")
                clear_selection()

            # a held left click on a footprint selects that footprint (the tool's click is not lost to the activation)
            on = find_on(target) if target else None
            check(on is not None, f"1j aim: a point on footprint {target} found and proved by a probe click ({on})")
            if on:
                kicad_inactive("held left click")
                pump(0)
                press(*on, 0.3)
                got = selection_event()
                check(names_footprint(got, target), f"1j a held left click on an inactive KiCad selects footprint {target} ({got and selected(got)[:4]})")
                check(wait_for(lambda: frames()[embed.FK_PCB].focused, 3), "1j and KiCad has the keyboard")
                clear_selection()

            # The host's process ends: every frame leaves the screen at once
            t0 = time.monotonic()
            host.send_signal(signal.SIGKILL)
            gone = wait_for(lambda: not on_screen(pcb), 3, 0.005)
            RESULTS["frame_gone_after_host_killed_ms"] = round((time.monotonic() - t0) * 1000)
            check(gone, f"the host's death hides the frame ({RESULTS['frame_gone_after_host_killed_ms']} ms)")

            final = api.request(embed.Close(mode=embed.CM_DISCARD), embed.CloseResponse)
            check(final.exiting, "Close discard answers exiting")
            check(wait_for(lambda: kicad.poll() is not None, 30), f"KiCad exited (status {kicad.poll()})")
    except BaseException:
        # Evidence of a hang: what is on screen, KiCad's stacks, every window's owner and title
        shot("zz-failure")
        print(f"KiCad exit status: {kicad.poll() if kicad is not None else None}", flush=True)
        reports = Path.home() / "Library/Logs/DiagnosticReports"
        for report in sorted(reports.glob("*")) if reports.exists() else []:
            if "kicad" in report.name.lower() and report.stat().st_mtime > started:
                shutil.copy(report, args.out / report.name)
                print(f"crash report {report.name}:\n" + report.read_text(errors="replace")[:6000], flush=True)
        if kicad is not None and kicad.poll() is None:
            subprocess.run(["sample", str(kicad.pid), "3", "-file", str(args.out / "kicad-sample.txt")],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        listing = [{k: w.get(k) for k in ("kCGWindowNumber", "kCGWindowOwnerName", "kCGWindowName", "kCGWindowLayer",
                                           "kCGWindowIsOnscreen")} | {"bounds": dict(w["kCGWindowBounds"])}
                   for w in Quartz.CGWindowListCopyWindowInfo(Quartz.kCGWindowListOptionAll, Quartz.kCGNullWindowID)
                   if w.get("kCGWindowOwnerPID") in {p.pid for p in (kicad, host) if p is not None}]
        (args.out / "windows.json").write_text(json.dumps(listing, indent=2, default=str))
        print(json.dumps(listing, indent=1, default=str)[:4000], flush=True)
        raise
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
