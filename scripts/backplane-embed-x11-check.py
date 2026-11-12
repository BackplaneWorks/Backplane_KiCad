#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end test of `kicad --embedded` on a private X server with a window manager.

A small python-xlib host stands in for the embedding application: a WM-managed top-level with
a root-visual container child.  The test proves that no KiCad frame is ever managed by the
window manager, that every frame kind shows inside the container, click-to-focus, reserved key
forwarding, dialog transient-for, AS_BUSY under a modal dialog, detach/re-attach (including the
host's connection dying), and Close without KiCad's prompts.  Screenshots go to --out.

Needs: Xvfb, i3, xdotool, ImageMagick `import`, python3-xlib, pynng, protobuf, protoc.
Never touches the user's configuration or projects: everything lives in a temporary tree.
"""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import uuid

from Xlib import X, Xatom, display as xdisplay

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_ipc_test_support import IpcSession  # noqa: E402

FAILURES = []


def check(condition, message):
    print(("PASS " if condition else "FAIL ") + message, flush=True)
    if not condition:
        FAILURES.append(message)
    return condition


def free_display():
    for number in range(140, 400):
        if not Path(f"/tmp/.X11-unix/X{number}").exists() and not Path(f"/tmp/.X{number}-lock").exists():
            return number
    raise RuntimeError("no free X display")


class WmWatcher(threading.Thread):
    """Samples the window manager's client list; remembers every window it ever managed."""

    def __init__(self, name):
        super().__init__(daemon=True)
        self.display = xdisplay.Display(name)
        self.root = self.display.screen().root
        self.atom = self.display.intern_atom("_NET_CLIENT_LIST")
        self.seen = {}
        self.stop = False
        self.lock = threading.Lock()

    def current(self):
        prop = self.root.get_full_property(self.atom, Xatom.WINDOW)
        return list(prop.value) if prop else []

    def run(self):
        while not self.stop:
            try:
                for wid in self.current():
                    with self.lock:
                        if wid not in self.seen:
                            window = self.display.create_resource_object("window", wid)
                            try:
                                transient = window.get_wm_transient_for()
                                transient = transient.id if transient else 0
                                name = window.get_wm_name() or ""
                            except Exception:
                                transient, name = 0, ""
                            self.seen[wid] = (transient, name)
            except Exception:
                pass
            time.sleep(0.02)


class Host:
    """The stand-in embedding application."""

    def __init__(self, name, width=1400, height=900):
        self.display = xdisplay.Display(name)
        screen = self.display.screen()
        self.root = screen.root
        self.top = self.root.create_window(0, 0, width, height, 0, screen.root_depth,
                                           X.InputOutput, X.CopyFromParent,
                                           background_pixel=screen.white_pixel,
                                           event_mask=X.StructureNotifyMask | X.FocusChangeMask)
        self.top.set_wm_name("EmbedHost")
        self.top.set_wm_class("embedhost", "EmbedHost")
        self.container = self.top.create_window(0, 0, width, height, 0, screen.root_depth,
                                                X.InputOutput, X.CopyFromParent,
                                                background_pixel=0x303030,
                                                event_mask=X.StructureNotifyMask)
        self.container.map()
        self.top.map()
        self.display.sync()
        self.messages = []
        self.focus_atom = self.display.intern_atom("_BACKPLANE_FOCUS")
        self.key_atom = self.display.intern_atom("_BACKPLANE_KEY")
        self.size = (width, height)
        self.pump(1.0)

    def pump(self, seconds=0.0):
        deadline = time.monotonic() + seconds
        while True:
            while self.display.pending_events():
                event = self.display.next_event()
                if event.type == X.ClientMessage:
                    self.messages.append((event.window.id, event.client_type, list(event.data[1])))
                elif event.type == X.ConfigureNotify and event.window.id == self.top.id:
                    self.size = (event.width, event.height)
                    self.container.configure(width=event.width, height=event.height)
                    self.display.sync()
            if time.monotonic() >= deadline:
                return
            time.sleep(0.02)

    def wait_message(self, atom, seconds=10.0):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.pump(0.05)
            for index, (window, kind, data) in enumerate(self.messages):
                if kind == atom:
                    del self.messages[index]
                    return window, data
        return None


def parent_of(disp, wid):
    return disp.create_resource_object("window", wid).query_tree().parent.id


def viewable(disp, wid):
    try:
        return disp.create_resource_object("window", wid).get_attributes().map_state == X.IsViewable
    except Exception:
        return False


def wait_for(predicate, seconds, step=0.1):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(step)
    return predicate()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path, help="the GUI kicad binary")
    parser.add_argument("kicad_cli", type=Path, help="kicad-cli (for the protobuf bindings)")
    parser.add_argument("--out", type=Path, default=Path("embed-x11-out"))
    parser.add_argument("--xvfb", default=shutil.which("Xvfb") or "Xvfb")
    parser.add_argument("--i3", default=shutil.which("i3") or "i3")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    work = Path(tempfile.mkdtemp(prefix="backplane-embed-x11-"))
    processes = []
    number = free_display()
    name = f":{number}"
    env = dict(os.environ)
    env.update({
        "DISPLAY": name, "GDK_BACKEND": "x11", "GDK_SCALE": "1", "NO_AT_BRIDGE": "1",
        "LIBGL_ALWAYS_SOFTWARE": "1", "GALLIUM_DRIVER": "llvmpipe", "LP_NUM_THREADS": "4",
        "TMPDIR": str(work / "tmp"),
    })
    env.pop("WAYLAND_DISPLAY", None)
    for variable, directory in (("HOME", "home"), ("XDG_CONFIG_HOME", "config"),
                                ("XDG_DATA_HOME", "data"), ("XDG_CACHE_HOME", "cache"),
                                ("XDG_RUNTIME_DIR", "run"), ("TMPDIR", "tmp")):
        path = work / directory
        path.mkdir(parents=True, exist_ok=True, mode=0o700)
        env[variable] = str(path)

    # Empty global library tables, so nothing asks for them.
    for version in ("10.0", "10.99"):
        config = work / "config/kicad" / version
        config.mkdir(parents=True, exist_ok=True)
        (config / "sym-lib-table").write_text("(sym_lib_table\n  (version 7)\n)\n")
        (config / "fp-lib-table").write_text("(fp_lib_table\n  (version 7)\n)\n")
        (config / "design-block-lib-table").write_text("(design_block_lib_table\n  (version 7)\n)\n")

    source = Path(__file__).resolve().parents[1]
    project_dir = work / "pic_programmer"
    shutil.copytree(source / "demos/pic_programmer", project_dir)
    project = project_dir / "pic_programmer.kicad_pro"
    board_file = project_dir / "pic_programmer.kicad_pcb"

    def shot(label):
        path = args.out / f"{label}.png"
        subprocess.run(["import", "-display", name, "-window", "root", str(path)], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return path

    def painted(path, crop=None):
        command = ["convert", str(path)]
        if crop:
            command += ["-crop", crop, "+repage"]
        command += ["-format", "%[fx:standard_deviation]", "info:"]
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL).stdout.strip()
        try:
            return float(result) > 0.02
        except ValueError:
            return False

    def xdotool(*words):
        subprocess.run(["xdotool", *words], env=env, check=False)

    kicad = None
    try:
        processes.append(subprocess.Popen([args.xvfb, name, "-screen", "0", "1600x1000x24",
                                           "-nolisten", "tcp", "+extension", "GLX"],
                                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        assert wait_for(lambda: Path(f"/tmp/.X11-unix/X{number}").exists(), 10), "Xvfb did not start"
        i3conf = work / "i3.conf"
        i3conf.write_text(f"ipc-socket {work}/i3.sock\nfont pango:monospace 8\n"
                          "default_border none\ndefault_floating_border normal\n"
                          "focus_follows_mouse no\n")
        processes.append(subprocess.Popen([args.i3, "-c", str(i3conf)], env=env,
                                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(1.5)

        second_host = []   # the second stand-in host's container, once it exists

        watcher = WmWatcher(name)
        watcher.start()
        host = Host(name)
        disp = host.display
        top, container = host.top.id, host.container.id

        socket = work / "run/kicad/api.sock"
        socket.parent.mkdir(parents=True, exist_ok=True)
        log = (args.out / "kicad.log").open("w")
        wrapper = os.environ.get("EMBED_WRAPPER", "").split()
        kicad = subprocess.Popen([*wrapper, str(args.kicad), "--embedded", "--parent-window", hex(container),
                                  "--toplevel-window", str(top), "--api-socket", str(socket),
                                  "--parent-pid", str(os.getpid()), str(project)],
                                 env=env, stdout=subprocess.PIPE, stderr=log, text=True)
        ready = None
        events_line = None
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            line = kicad.stdout.readline()
            if not line:
                break
            print("kicad: " + line.rstrip(), flush=True)
            if line.startswith("KiCad embed events at "):
                events_line = line.split(" at ", 1)[1].strip()
            if line.startswith("KiCad API server listening at "):
                ready = line.split(" at ", 1)[1].strip()
                break
        threading.Thread(target=lambda: [print("kicad: " + l.rstrip(), flush=True)
                                         for l in kicad.stdout], daemon=True).start()
        check(ready == f"ipc://{socket}", f"readiness line names the API socket ({ready})")
        check(events_line == f"ipc://{socket}.events", f"events socket announced ({events_line})")

        with IpcSession(args.kicad_cli, api_socket=socket, timeout=120000) as api:
            embed = api.module("backplane.embed")
            caps = api.module("backplane.capabilities")
            editor = api.module("common.commands.editor_commands")
            board = api.module("board.board_types")
            types = api.module("common.types.base_types")
            enums = api.module("common.types.enums")
            base = api.module("common.commands.base_commands")

            capabilities = api.request(caps.GetCapabilities(), caps.GetCapabilitiesResponse)
            check("embed" in capabilities.capabilities
                  and "kiapi.backplane.embed.ShowFrame" in capabilities.commands,
                  "GetCapabilities advertises embed")

            frames = api.request(embed.GetFrames(), embed.GetFramesResponse)
            holder = frames.holder
            check(holder != 0 and parent_of(disp, holder) == container,
                  "holder is a child of the container")
            host.display.create_resource_object("window", holder).change_save_set(X.SetModeInsert)
            disp.sync()
            check(frames.parent == container and frames.toplevel == top,
                  "GetFrames reports parent and toplevel")

            def frame_windows():
                return {f.kind: f for f in api.request(embed.GetFrames(), embed.GetFramesResponse).frames}

            kicad_windows = set()

            def show(kind, label):
                for other in frame_windows().values():
                    if other.shown and other.kind != kind:
                        api.request(embed.HideFrame(window=other.window), embed.HideFrameResponse)
                w, h = host.size
                reply = api.request(embed.ShowFrame(kind=kind, x=0, y=0, width=w, height=h),
                                    embed.ShowFrameResponse)
                window = reply.frame.window
                kicad_windows.add(window)
                ok = wait_for(lambda: viewable(disp, window), 30)
                # The middle of the canvas, where only the editor draws.
                crop = f"{w // 4}x{h // 4}+{w * 3 // 8}+{h * 3 // 8}"
                path = args.out / f"{label}.png"
                # Library editors open empty: their canvas is legitimately blank.
                if kind in (embed.FK_SYMBOL_EDITOR, embed.FK_FOOTPRINT_EDITOR):
                    crop = None
                drawn = wait_for(lambda: host.pump(0) or painted(shot(label), crop), 40, 1.0)
                if not drawn and os.environ.get("EMBED_POKE"):
                    xdotool("mousemove", str(w // 2), str(h // 2), "click", "1")
                    poked = wait_for(lambda: painted(shot(label + "-poked"), crop), 20, 1.0)
                    print(f"DEBUG after a click {label} painted={poked}", flush=True)
                    raw = subprocess.run(["xwininfo", "-tree", "-id", hex(window), "-display", name],
                                         text=True, stdout=subprocess.PIPE).stdout
                    print(raw[:3000], flush=True)
                time.sleep(1.0)
                host.pump(0.1)
                path = shot(label)
                check(drawn, f"{embed.FrameKind.Name(kind)} paints its canvas ({path.name})")
                parent = parent_of(disp, window)
                if os.environ.get("EMBED_DEBUG"):
                    tree = subprocess.run(["xwininfo", "-root", "-tree", "-display", name],
                                          text=True, stdout=subprocess.PIPE).stdout
                    (args.out / f"{label}.tree.txt").write_text(tree)
                    raise SystemExit("debug stop")
                check(ok and parent == holder,
                      f"{embed.FrameKind.Name(kind)} shown inside the holder ({path.name}; "
                      f"window {window:#x} viewable={ok} parent={parent:#x} holder={holder:#x} "
                      f"shown={reply.frame.shown})")
                return window

            pcb = show(embed.FK_PCB, "01-pcb")
            sch = show(embed.FK_SCH, "02-schematic")
            show(embed.FK_PCB_3D, "03-3d-viewer")
            show(embed.FK_SYMBOL_EDITOR, "04-symbol-editor")
            show(embed.FK_FOOTPRINT_EDITOR, "05-footprint-editor")

            all_frames = frame_windows()
            check(all(parent_of(disp, f.window) == holder for f in all_frames.values()),
                  f"all {len(all_frames)} frames are children of the holder")

            # Click to focus
            pcb = show(embed.FK_PCB, "06-pcb-again")
            disp.set_input_focus(host.top, X.RevertToParent, X.CurrentTime)
            disp.sync()
            host.messages.clear()
            w, h = host.size
            xdotool("mousemove", str(w // 2), str(h // 2), "click", "1")
            message = host.wait_message(host.focus_atom)
            check(message is not None and message[0] == container and message[1][0] == pcb,
                  f"click sends _BACKPLANE_FOCUS to the container ({message})")
            focus = disp.get_input_focus().focus
            focus_id = focus.id if hasattr(focus, "id") else focus
            check(focus_id == pcb, f"the clicked frame has the keyboard ({focus_id:#x} vs {pcb:#x})")

            # Reserved keys
            host.messages.clear()
            xdotool("key", "--clearmodifiers", "ctrl+k")
            message = host.wait_message(host.key_atom)
            check(message is not None and message[1][1] == 0x6b and message[1][2] & 4
                  and message[1][0] == pcb, f"Ctrl+K forwarded as _BACKPLANE_KEY ({message})")
            xdotool("key", "--clearmodifiers", "ctrl+shift+Tab")
            message = host.wait_message(host.key_atom)
            check(message is not None and message[1][1] == 0xff09 and message[1][2] & 5 == 5,
                  f"Ctrl+Shift+Tab forwarded ({message})")
            keys = api.request(embed.SetReservedKeys(chords=["Ctrl+K", "F12"]),
                               embed.SetReservedKeysResponse)
            check(list(keys.chords) == ["Ctrl+K", "F12"], "SetReservedKeys normalises")
            bad = api.request_raw(embed.SetReservedKeys(chords=["Hyper+Q"]))
            check(bad.status.status == api.envelope.AS_BAD_REQUEST, "a bad chord is refused")

            # Dialog transient-for, AS_BUSY while it is modal, events over the long-poll socket
            sch = show(embed.FK_SCH, "07-schematic")
            xdotool("mousemove", str(w // 2), str(h // 2), "click", "1")
            time.sleep(0.5)
            before = set(watcher.current())
            xdotool("key", "a")
            appeared = wait_for(lambda: set(watcher.current()) - before - {top}, 20)
            time.sleep(1.5)
            dialogs = set(watcher.current()) - before - {top}
            path = shot("08-dialog")
            transient_ok = bool(dialogs)
            for wid in dialogs:
                win = disp.create_resource_object("window", wid)
                tf = win.get_wm_transient_for()
                transient_ok &= tf is not None and tf.id == top
            check(appeared and transient_ok,
                  f"dialog is WM-managed and transient for the host top-level ({path.name})")

            busy = api.request_raw(editor.RunAction(action="eeschema.InteractiveSelection.ClearSelection"))
            check(busy.status.status == api.envelope.AS_BUSY and "dialog" in busy.status.error_message,
                  f"mutating request under a modal answers AS_BUSY ({busy.status.error_message})")
            read = api.request_raw(editor.GetOpenDocuments(type=types.DOCTYPE_PCB))
            check(read.status.status == api.envelope.AS_OK, "reads still work under the modal")

            import pynng
            with pynng.Req0(dial=f"ipc://{socket}.events", recv_timeout=70000) as poll:
                def next_events(since, wait):
                    request = api.envelope.ApiRequest()
                    request.message.Pack(embed.NextEvents(since=since, wait_ms=wait))
                    poll.send(request.SerializeToString())
                    response = api.envelope.ApiResponse.FromString(poll.recv())
                    result = embed.NextEventsResponse()
                    response.message.Unpack(result)
                    return result

                history = next_events(0, 0)
                types_seen = {e.type for e in history.events}
                check(embed.ET_FRAME_OPENED in types_seen and embed.ET_DIALOG_OPENED in types_seen
                      and embed.ET_FOCUS_IN in types_seen,
                      f"event history has frame opened, focus and dialog ({sorted(types_seen)})")
                started = time.monotonic()
                xdotool("key", "Escape")
                waited = next_events(history.next, 15000)
                check(any(e.type == embed.ET_DIALOG_CLOSED for e in waited.events)
                      and time.monotonic() - started < 14,
                      "long poll wakes when the dialog closes")
                start = time.monotonic()
                empty = next_events(waited.next + 1000000, 500)
                check(not empty.events and time.monotonic() - start >= 0.45,
                      "long poll waits wait_ms when there is nothing")
            xdotool("key", "Escape")
            time.sleep(1)
            check(not set(watcher.current()) - before - {top}, "dialog closed")

            # Detach and re-attach; then the host's connection dies (save-set)
            api.request(embed.AttachToParent(window=0), embed.AttachToParentResponse)
            disp.sync()
            check(not viewable(disp, holder) and parent_of(disp, holder) == host.root.id,
                  "AttachToParent{0} parks the holder unmapped at the root")
            reply = api.request(embed.AttachToParent(window=container, toplevel=top),
                                embed.AttachToParentResponse)
            check(reply.holder == holder and parent_of(disp, holder) == container
                  and wait_for(lambda: viewable(disp, sch), 5), "re-attach maps the frames back")

            child = subprocess.Popen([sys.executable, "-c", f"""
import ctypes, sys, time
x = ctypes.CDLL("libX11.so.6"); xf = ctypes.CDLL("libXfixes.so.3")
x.XOpenDisplay.restype = ctypes.c_void_p
x.XDefaultRootWindow.restype = ctypes.c_ulong
x.XCreateSimpleWindow.restype = ctypes.c_ulong
x.XCreateSimpleWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong] + [ctypes.c_int] * 2 + [ctypes.c_uint] * 3 + [ctypes.c_ulong] * 2
x.XMapWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
x.XStoreName.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_char_p]
xf.XFixesChangeSaveSet.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_int]
d = x.XOpenDisplay({name.encode()!r})
top = x.XCreateSimpleWindow(d, x.XDefaultRootWindow(d), 0, 0, 800, 600, 0, 0, 0)
x.XStoreName(d, top, b'EmbedHost2')
c = x.XCreateSimpleWindow(d, top, 0, 0, 800, 600, 0, 0, 0)
x.XMapWindow(d, c); x.XMapWindow(d, top); x.XSync(d, 0)
print(c, flush=True)
# The host's part of the contract: the holder goes to the root, unmapped, if we die.
xf.XFixesChangeSaveSet(d, int(sys.stdin.readline()), 0, 1, 1)
x.XSync(d, 0); print('ok', flush=True); time.sleep(600)
"""], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
            second = int(child.stdout.readline())
            second_host.append(second)
            child.stdin.write(f"{holder}\n")
            child.stdin.flush()
            child.stdout.readline()
            api.request(embed.AttachToParent(window=second, toplevel=0), embed.AttachToParentResponse)
            disp.sync()
            check(parent_of(disp, holder) == second, "holder moved into a second host's container")
            mark = api.request(embed.NextEvents(since=0), embed.NextEventsResponse).next
            child.kill()
            child.wait()
            detached = wait_for(lambda: any(e.type == embed.ET_DETACHED for e in api.request(
                embed.NextEvents(since=mark), embed.NextEventsResponse).events), 10)
            disp.sync()
            check(detached and not viewable(disp, holder) and parent_of(disp, holder) == host.root.id,
                  "host death: holder survives (save-set), parked unmapped, ET_DETACHED sent")
            frames = api.request(embed.GetFrames(), embed.GetFramesResponse)
            check(len(frames.frames) == len(all_frames), "every frame survived the host's death")
            api.request(embed.AttachToParent(window=container, toplevel=top), embed.AttachToParentResponse)
            check(parent_of(disp, holder) == container, "re-attached to the first host")
            api.request(embed.SetReservedKeys(chords=["Ctrl+K", "Ctrl+J", "Ctrl+Tab",
                                                       "Ctrl+Shift+Tab", "Ctrl+`"]),
                        embed.SetReservedKeysResponse)
            chrome = api.request(embed.SetChrome(menubar=False), embed.SetChromeResponse)
            check(not chrome.menubar and chrome.top_toolbar, "SetChrome hides the menubar only")
            show(embed.FK_PCB, "09-pcb-no-menubar")

            # Close without prompts
            documents = api.request(editor.GetOpenDocuments(type=types.DOCTYPE_PCB),
                                    editor.GetOpenDocumentsResponse)
            header = types.ItemHeader(document=documents.documents[0])

            def modify_board():
                transaction = api.request(editor.BeginCommit(header=header), editor.BeginCommitResponse)
                create = editor.CreateItems(header=header)
                create.items.add().Pack(board.Track(
                    id=types.KIID(value=str(uuid.uuid4())),
                    start=types.Vector2(x_nm=100000000, y_nm=100000000),
                    end=types.Vector2(x_nm=110000000, y_nm=100000000),
                    width=types.Distance(value_nm=250000), layer=board.BL_F_Cu))
                api.request(create, editor.CreateItemsResponse)
                api.request(editor.EndCommit(header=header, id=transaction.id,
                                             action=editor.CMA_COMMIT, message="embed test"),
                            editor.EndCommitResponse)

            original = hashlib.sha256(board_file.read_bytes()).hexdigest()
            modify_board()
            check(wait_for(lambda: frame_windows()[embed.FK_PCB].modified, 5), "board is modified")
            before = set(watcher.current())
            refused = api.request_raw(embed.Close(mode=embed.CM_FAIL_IF_MODIFIED))
            check(refused.status.status == api.envelope.AS_BAD_REQUEST
                  and "unsaved" in refused.status.error_message,
                  f"Close fail_if_modified refuses ({refused.status.error_message})")
            closed = api.request(embed.Close(mode=embed.CM_SAVE, kind=embed.FK_PCB), embed.CloseResponse)
            saved = hashlib.sha256(board_file.read_bytes()).hexdigest()
            print(f"close save: modified={list(closed.modified)} saved_changed={saved != original} "
                  f"pcb_open={embed.FK_PCB in frame_windows()}", flush=True)
            check(len(closed.modified) == 1 and saved != original
                  and embed.FK_PCB not in frame_windows(),
                  "Close save on the PCB saves and closes it without a prompt")
            show(embed.FK_PCB, "10-pcb-reopened")
            modify_board()
            wait_for(lambda: frame_windows()[embed.FK_PCB].modified, 5)
            final = api.request(embed.Close(mode=embed.CM_DISCARD), embed.CloseResponse)
            check(final.exiting, "Close discard on everything answers exiting")
            exited = wait_for(lambda: kicad.poll() is not None, 30)
            check(exited, f"KiCad exited after Close (status {kicad.poll()})")
            check(hashlib.sha256(board_file.read_bytes()).hexdigest() == saved,
                  "discarded changes were not written")
            check(not set(watcher.current()) - before - {top}, "no prompt appeared while closing")

        watcher.stop = True
        time.sleep(0.1)
        frames_managed = [hex(w) for w in watcher.seen if w in kicad_windows or w == holder]
        check(not frames_managed, f"no KiCad frame was ever managed by the WM ({frames_managed})")
        # The second stand-in host's windows are its own (same X client: same resource base);
        # its name can be unreadable when the watcher first sees it or after it was killed
        second_base = (second_host[0] & ~0x1FFFFF) if second_host else None
        strays = [(hex(w), n) for w, (t, n) in watcher.seen.items()
                  if w != top and t != top and n != "EmbedHost2"
                  and (second_base is None or (w & ~0x1FFFFF) != second_base)]
        check(not strays, f"every other managed window was transient for the host ({strays})")
    except BaseException:
        # Leave evidence of a hang: the stacks of a KiCad that stopped answering.
        if kicad is not None and kicad.poll() is None and shutil.which("gdb"):
            stacks = subprocess.run(["gdb", "-p", str(kicad.pid), "-batch", "-ex",
                                     "thread apply all bt 25"], text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout
            (args.out / "kicad-stacks.txt").write_text(stacks)
        elif kicad is not None:
            print(f"KiCad exit status {kicad.poll()}", flush=True)
        raise
    finally:
        if kicad is not None and kicad.poll() is None:
            kicad.terminate()
            try:
                kicad.wait(10)
            except subprocess.TimeoutExpired:
                kicad.kill()
        for process in reversed(processes):
            process.terminate()
            try:
                process.wait(5)
            except subprocess.TimeoutExpired:
                process.kill()
        shutil.rmtree(work, ignore_errors=True)

    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
