#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Embedded KiCad when the host's container goes away (X11).

Five cases, each on a fresh KiCad under Xvfb + i3 with a separate host process (ctypes Xlib) that
puts the holder in its XFixes save-set (SaveSetRoot, SaveSetUnmap), as Backplane's client does:

  destroy-pcb      the host destroys its container (host alive), board shown
  destroy-sch      the same with the schematic shown
  destroy-switch   destroyed right after a ShowFrame of the schematic (a tab switch)
  destroy-pending  destroyed while a ShowFrame request is in flight
  kill-pcb         the host is killed (-9): the server moves the holder to the root

Every case: an agent's unsaved edit (a track) before the loss.
A destroy case (KiCad's own windows are destroyed with the container): KiCad writes the autosave
and ends with exit code 3 ("windows lost"); the edit is in a .history snapshot written at the loss
(the regular autosave debounce is set far off, so only the loss wrote it); a restarted KiCad offers
the changes back (its Auto-Save Recovery dialog).
The kill case: the holder survives; a new host's AttachToParent + ShowFrame paints and the edit is
still on the board.

Usage: backplane-embed-parent-loss-check.py KICAD KICAD_CLI [--out DIR] [--case NAME]...
Needs Xvfb, i3, ImageMagick (import, convert), git, pynng, protobuf, libX11 and libXfixes.
"""

import argparse
import signal
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import uuid

SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE / "scripts"))
from backplane_ipc_test_support import IpcSession  # noqa: E402

CASES = ["destroy-pcb", "destroy-sch", "destroy-switch", "destroy-pending", "kill-pcb"]
FAILURES = []

HOST = r"""
import sys, os, ctypes
x = ctypes.CDLL("libX11.so.6"); xf = ctypes.CDLL("libXfixes.so.3")
x.XOpenDisplay.restype = ctypes.c_void_p
x.XOpenDisplay.argtypes = [ctypes.c_char_p]
d = ctypes.c_void_p(x.XOpenDisplay(None))
x.XDefaultRootWindow.restype = ctypes.c_ulong
x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
x.XCreateSimpleWindow.restype = ctypes.c_ulong
x.XCreateSimpleWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
                                  ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong, ctypes.c_ulong]
for f in ("XMapWindow", "XDestroyWindow"):
    getattr(x, f).argtypes = [ctypes.c_void_p, ctypes.c_ulong]
x.XStoreName.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_char_p]
x.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
xf.XFixesChangeSaveSet.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_int]
ev, er, maj, mnr = ctypes.c_int(), ctypes.c_int(), ctypes.c_int(5), ctypes.c_int(0)
xf.XFixesQueryExtension(d, ctypes.byref(ev), ctypes.byref(er))
xf.XFixesQueryVersion(d, ctypes.byref(maj), ctypes.byref(mnr))
root = x.XDefaultRootWindow(d)
top = x.XCreateSimpleWindow(d, root, 0, 0, 1400, 900, 0, 0, 0xffffff)
x.XStoreName(d, top, sys.argv[1].encode())
cont = x.XCreateSimpleWindow(d, top, 0, 0, 1400, 900, 0, 0, 0x303030)
x.XMapWindow(d, cont); x.XMapWindow(d, top); x.XSync(d, 0)
print(top, cont, flush=True)
for line in sys.stdin:
    w = line.split()
    if w[0] == "saveset":
        xf.XFixesChangeSaveSet(d, int(w[1]), 0, 1, 1)   # SetModeInsert, SaveSetRoot, SaveSetUnmap
        x.XSync(d, 0)
    elif w[0] == "destroy":
        x.XDestroyWindow(d, cont); x.XSync(d, 0)
    elif w[0] == "kill":
        os.kill(os.getpid(), 9)
    print("ok", flush=True)
"""


def check(ok, msg):
    print(("PASS " if ok else "FAIL ") + msg, flush=True)
    if not ok:
        FAILURES.append(msg)
    return ok


def wait_for(pred, seconds, step=0.1):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(step)
    return pred()


class Host:
    def __init__(self, script, env, name):
        self.proc = subprocess.Popen([sys.executable, str(script), name], env=env, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, text=True)
        self.top, self.container = map(int, self.proc.stdout.readline().split())

    def cmd(self, command):
        self.proc.stdin.write(command + "\n")
        self.proc.stdin.flush()
        if command != "kill":
            self.proc.stdout.readline()

    def stop(self):
        if self.proc.poll() is None:
            self.proc.kill()


def run_case(case, args, out):
    print(f"== {case}", flush=True)
    signal.alarm(240)   # a hung case fails rather than stalling the rest
    work = Path(tempfile.mkdtemp(prefix=f"parentloss-{case}-"))
    number = next(n for n in range(150, 400) if not Path(f"/tmp/.X11-unix/X{n}").exists()
                  and not Path(f"/tmp/.X{n}-lock").exists())
    name = f":{number}"
    env = dict(os.environ)
    env.update({"DISPLAY": name, "GDK_BACKEND": "x11", "NO_AT_BRIDGE": "1", "LIBGL_ALWAYS_SOFTWARE": "1"})
    env.pop("WAYLAND_DISPLAY", None)
    for var, d in (("HOME", "home"), ("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                   ("XDG_CACHE_HOME", "cache"), ("XDG_RUNTIME_DIR", "run"), ("TMPDIR", "tmp")):
        p = work / d
        p.mkdir(parents=True, exist_ok=True, mode=0o700)
        env[var] = str(p)
    for version in ("10.0", "10.99"):
        c = work / "config/kicad" / version
        c.mkdir(parents=True, exist_ok=True)
        (c / "sym-lib-table").write_text("(sym_lib_table\n  (version 7)\n)\n")
        (c / "fp-lib-table").write_text("(fp_lib_table\n  (version 7)\n)\n")
        (c / "design-block-lib-table").write_text("(design_block_lib_table\n  (version 7)\n)\n")
        # The regular autosave far off: a snapshot after the loss can only be the loss's own
        (c / "kicad_common.json").write_text(
            '{"system": {"local_history_debounce": 3000}, "auto_backup": {"enabled": true}}')
    proj_dir = work / "pic_programmer"
    shutil.copytree(SOURCE / "demos/pic_programmer", proj_dir)
    project = proj_dir / "pic_programmer.kicad_pro"
    host_script = work / "host.py"
    host_script.write_text(HOST)

    procs, hosts, kicad = [], [], None
    try:
        procs.append(subprocess.Popen(["Xvfb", name, "-screen", "0", "1600x1000x24", "-nolisten", "tcp",
                                       "+extension", "GLX"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        assert wait_for(lambda: Path(f"/tmp/.X11-unix/X{number}").exists(), 10), "Xvfb did not start"
        i3conf = work / "i3.conf"
        i3conf.write_text("font pango:monospace 8\ndefault_border none\nfocus_follows_mouse no\n")
        procs.append(subprocess.Popen(["i3", "-c", str(i3conf)], env=env,
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(1.5)
        host = Host(host_script, env, "EmbedHost")
        hosts.append(host)
        time.sleep(0.5)

        sock = work / "run/kicad/api.sock"
        sock.parent.mkdir(parents=True, exist_ok=True)

        def launch(on_host, label, wait_ready=True):
            if sock.exists():
                sock.unlink()
            proc = subprocess.Popen([*os.environ.get("EMBED_WRAPPER", "").split(), args.kicad, "--embedded",
                                     "--parent-window", hex(on_host.container), "--toplevel-window", str(on_host.top),
                                     "--api-socket", str(sock), "--parent-pid", str(os.getpid()), str(project)],
                                    env=env, stdout=subprocess.PIPE,
                                    stderr=(out / f"{case}-{label}-kicad.log").open("w"), text=True)
            ready = threading.Event()

            def read():
                for line in proc.stdout:
                    if line.startswith("KiCad API server listening at "):
                        ready.set()

            threading.Thread(target=read, daemon=True).start()
            proc.ready = ready
            if wait_ready and not ready.wait(90):
                subprocess.run(["import", "-display", name, "-window", "root", str(out / f"{case}-{label}-stalled.png")],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                raise TimeoutError(f"KiCad ({label}) not ready in 90 s; screen in {case}-{label}-stalled.png")
            return proc

        kicad = launch(host, "first")
        lost_exit = None

        with IpcSession(args.kicad_cli, api_socket=sock, timeout=60000, client_name="agent:t1") as api, \
                IpcSession(args.kicad_cli, api_socket=sock, timeout=60000, client_name="works.backplane.hub") as hub:
            embed = api.module("backplane.embed")
            editor = api.module("common.commands.editor_commands")
            board = api.module("board.board_types")
            types = api.module("common.types.base_types")
            enums = api.module("common.types.enums")

            hub.request(embed.ShowFrame(kind=embed.FK_PCB, x=0, y=0, width=1400, height=900), embed.ShowFrameResponse)
            time.sleep(3)
            frames = hub.request(embed.GetFrames(), embed.GetFramesResponse)
            host.cmd(f"saveset {frames.holder}")

            doc = api.request(editor.GetOpenDocuments(type=types.DOCTYPE_PCB),
                              editor.GetOpenDocumentsResponse).documents[0]
            header = types.ItemHeader(document=doc)

            def tracks():
                return len(api.request(editor.GetItems(header=header, types=[enums.KOT_PCB_TRACE]),
                                       editor.GetItemsResponse).items)

            n0 = tracks()
            track_id = str(uuid.uuid4())
            t = api.request(editor.BeginCommit(header=header), editor.BeginCommitResponse)
            c = editor.CreateItems(header=header)
            c.items.add().Pack(board.Track(
                id=types.KIID(value=track_id),
                start=types.Vector2(x_nm=120000000, y_nm=100000000),
                end=types.Vector2(x_nm=140000000, y_nm=100000000),
                width=types.Distance(value_nm=300000), layer=board.BL_F_Cu))
            api.request(c, editor.CreateItemsResponse)
            api.request(editor.EndCommit(header=header, id=t.id, action=editor.CMA_COMMIT, message="agent track"),
                        editor.EndCommitResponse)
            check(tracks() == n0 + 1, f"{case}: the agent's unsaved track is on the board")

            if case in ("destroy-sch", "destroy-switch"):
                hub.request(embed.ShowFrame(kind=embed.FK_SCH, x=0, y=0, width=1400, height=900),
                            embed.ShowFrameResponse)
                if case == "destroy-sch":
                    time.sleep(4)

            seq = hub.request(embed.NextEvents(since=0, max=256), embed.NextEventsResponse).next

            pending = None
            if case == "destroy-pending":
                def show_late():
                    try:
                        hub.request(embed.ShowFrame(kind=embed.FK_SCH, x=0, y=0, width=1400, height=900),
                                    embed.ShowFrameResponse)
                    except Exception as error:   # noqa: BLE001  (refused or lost: either is fine)
                        print(f"INFO {case}: the pending ShowFrame ended with {error}", flush=True)
                pending = threading.Thread(target=show_late, daemon=True)
                pending.start()
                time.sleep(0.05)

            hist = proj_dir / ".history"

            def snapshots():
                r = subprocess.run(["git", "-C", str(hist), "log", "--all", "--format=%s"], text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
                return r.stdout.splitlines() if r.returncode == 0 else []

            host.cmd("kill" if case.startswith("kill") else "destroy")

            if case.startswith("destroy"):
                # KiCad's windows are gone with the container: it saves and ends with code 3
                wait_for(lambda: kicad.poll() is not None, 20)
                lost_exit = kicad.poll()
                if pending:
                    pending.join(30)
            else:
                alive = not wait_for(lambda: kicad.poll() is not None, 20)
                check(alive, f"{case}: KiCad alive 20 s after the parent went (exit {kicad.poll()})")
                if not alive:
                    return
                events = hub.request(embed.NextEvents(since=seq, max=256), embed.NextEventsResponse).events
                detached = [e.detail for e in events if e.type == embed.ET_DETACHED]
                check("windows lost" not in detached, f"{case}: the windows survived ({detached})")
                second = Host(host_script, env, "EmbedHost2")
                hosts.append(second)
                time.sleep(0.5)
                hub.request(embed.AttachToParent(window=second.container, toplevel=second.top),
                            embed.AttachToParentResponse)
                hub.request(embed.ShowFrame(kind=embed.FK_PCB, x=0, y=0, width=1400, height=900),
                            embed.ShowFrameResponse)
                time.sleep(4)
                shot = out / f"{case}-reattached.png"
                subprocess.run(["import", "-display", name, "-window", "root", str(shot)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                sd = subprocess.run(["convert", str(shot), "-crop", "1400x900+0+0", "+repage", "-format",
                                     "%[fx:standard_deviation]", "info:"], text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout.strip()
                check(float(sd or 0) > 0.02, f"{case}: a re-attach to a new host paints (sd {sd})")
                check(tracks() == n0 + 1, f"{case}: the edit is still on the board")
                return

        if case.startswith("destroy"):
            log = (out / f"{case}-first-kicad.log").read_text(errors="replace")
            check(lost_exit == 3 and "windows lost" in log,
                  f"{case}: KiCad saved and ended with code 3 (exit {lost_exit})")
            logs = snapshots()
            lost = [m for m in logs if "windows were lost" in m]
            body = subprocess.run(["git", "-C", str(hist), "grep", "-l", track_id, "HEAD"], text=True,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout.strip()
            check(bool(lost) and bool(body),
                  f"{case}: the unsaved edit is in a .history snapshot written at the loss ({logs[:3]}, {body!r})")

            # A restart (as the host does on code 3) offers the changes back.  Like the hub
            # (sweep_crashed_locks), first take away the lock files the ended KiCad left.
            for lock in proj_dir.glob("~*.lck"):
                lock.unlink()
            again = Host(host_script, env, "EmbedHost3")
            hosts.append(again)
            time.sleep(0.5)
            kicad = launch(again, "restart", wait_ready=False)

            # KiCad asks at once, while it opens the project (before its API answers)
            def recovery_window():
                r = subprocess.run(["xdotool", "search", "--name", "Recover Unsaved Changes"], env=env, text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
                ids = r.stdout.split()
                return ids[0] if ids else None

            asked = wait_for(lambda: recovery_window() is not None, 60, 0.5)
            check(asked, f"{case}: the restarted KiCad offers the changes back (Recover Unsaved Changes)")
            if not asked:
                return

            # Restore: the dialog's right-hand button
            wid = recovery_window()
            geo = subprocess.run(["xdotool", "getwindowgeometry", "--shell", wid], env=env, text=True,
                                 stdout=subprocess.PIPE).stdout
            g = dict(line.split("=", 1) for line in geo.split())
            x, y, w, h = (int(g[k]) for k in ("X", "Y", "WIDTH", "HEIGHT"))
            subprocess.run(["xdotool", "mousemove", str(x + w * 3 // 4), str(y + h - 36), "click", "1"], env=env)
            opened = kicad.ready.wait(60)
            if not opened:
                subprocess.run(["import", "-display", name, "-window", "root", str(out / f"{case}-after-restore.png")],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            check(opened, f"{case}: KiCad opens after Restore (dialog {x},{y} {w}x{h}; still there: "
                          f"{recovery_window() is not None})")
            if not opened:
                return

            with IpcSession(args.kicad_cli, api_socket=sock, timeout=60000, client_name="works.backplane.hub") as hub:
                embed = hub.module("backplane.embed")
                editor = hub.module("common.commands.editor_commands")
                types = hub.module("common.types.base_types")
                enums = hub.module("common.types.enums")
                hub.request(embed.ShowFrame(kind=embed.FK_PCB, x=0, y=0, width=1400, height=900),
                            embed.ShowFrameResponse)
                time.sleep(3)
                doc = hub.request(editor.GetOpenDocuments(type=types.DOCTYPE_PCB),
                                  editor.GetOpenDocumentsResponse).documents[0]
                items = hub.request(editor.GetItems(header=types.ItemHeader(document=doc),
                                                    types=[enums.KOT_PCB_TRACE]), editor.GetItemsResponse).items
                ids = set()
                for item in items:
                    t = hub.module("board.board_types").Track()
                    item.Unpack(t)
                    ids.add(t.id.value)
                check(track_id in ids, f"{case}: after Restore the agent's track is back on the board")
            kicad.kill()
    except Exception as error:   # noqa: BLE001
        check(False, f"{case}: {type(error).__name__}: {error}")
    finally:
        if kicad is not None and kicad.poll() is None:
            kicad.kill()
        for h in hosts:
            h.stop()
        for p in reversed(procs):
            p.terminate()
        shutil.rmtree(work, ignore_errors=True)


def on_alarm(signum, frame):
    raise TimeoutError("the case took over 240 s")


def main():
    signal.signal(signal.SIGALRM, on_alarm)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kicad")
    ap.add_argument("kicad_cli")
    ap.add_argument("--out", type=Path, default=Path("embed-parent-loss-out"))
    ap.add_argument("--case", action="append", choices=CASES)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for case in args.case or CASES:
        run_case(case, args, args.out)
        signal.alarm(0)
    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
