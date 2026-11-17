#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Undo and redo across check runs in an embedded editor (DRC/ERC markers are regenerated data).

The person selects all (markers included), deletes, undoes and redoes, with the host's in-KiCad
checks (RunChecks) after every step and an agent's own commits interleaved.  Three rounds in the
board editor (DRC markers) and three in the schematic editor (ERC markers).  Every step must
restore or remove exactly the real items, no dialog may appear ("Incomplete undo/redo operation:
some items not found"), and the agent's undo takes back only its own item.

Usage: backplane-embed-undo-markers-check.py KICAD KICAD_CLI [--out DIR] [--editor pcb|sch]...
Needs Xvfb, i3, pynng, protobuf, libX11.
"""

import argparse
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

FAILURES = []

HOST = r"""
import sys, ctypes
x = ctypes.CDLL("libX11.so.6")
x.XOpenDisplay.restype = ctypes.c_void_p
x.XOpenDisplay.argtypes = [ctypes.c_char_p]
d = ctypes.c_void_p(x.XOpenDisplay(None))
x.XDefaultRootWindow.restype = ctypes.c_ulong
x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
x.XCreateSimpleWindow.restype = ctypes.c_ulong
x.XCreateSimpleWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
                                  ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong, ctypes.c_ulong]
x.XMapWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
x.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
root = x.XDefaultRootWindow(d)
top = x.XCreateSimpleWindow(d, root, 0, 0, 1400, 900, 0, 0, 0xffffff)
cont = x.XCreateSimpleWindow(d, top, 0, 0, 1400, 900, 0, 0, 0x303030)
x.XMapWindow(d, cont); x.XMapWindow(d, top); x.XSync(d, 0)
print(top, cont, flush=True)
sys.stdin.read()
"""


def check(ok, msg):
    print(("PASS " if ok else "FAIL ") + msg, flush=True)
    if not ok:
        FAILURES.append(msg)
    return ok


def wait_for(pred, seconds, step=0.2):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(step)
    return pred()


def run_editor(editor, args, out):
    print(f"== {editor}", flush=True)
    work = Path(tempfile.mkdtemp(prefix=f"undo-markers-{editor}-"))
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
    proj_dir = work / "pic_programmer"
    shutil.copytree(SOURCE / "demos/pic_programmer", proj_dir)
    project = proj_dir / "pic_programmer.kicad_pro"
    host_py = work / "host.py"
    host_py.write_text(HOST)

    procs, kicad = [], None
    try:
        procs.append(subprocess.Popen(["Xvfb", name, "-screen", "0", "1600x1000x24", "-nolisten", "tcp",
                                       "+extension", "GLX"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        assert wait_for(lambda: Path(f"/tmp/.X11-unix/X{number}").exists(), 10), "Xvfb did not start"
        i3conf = work / "i3.conf"
        i3conf.write_text("font pango:monospace 8\ndefault_border none\nfocus_follows_mouse no\n")
        procs.append(subprocess.Popen(["i3", "-c", str(i3conf)], env=env,
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(1.5)
        host = subprocess.Popen([sys.executable, str(host_py)], env=env, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, text=True)
        procs.append(host)
        top, cont = map(int, host.stdout.readline().split())

        sock = work / "run/kicad/api.sock"
        sock.parent.mkdir(parents=True, exist_ok=True)
        kicad = subprocess.Popen([args.kicad, "--embedded", "--parent-window", hex(cont), "--toplevel-window",
                                  str(top), "--api-socket", str(sock), "--parent-pid", str(os.getpid()),
                                  str(project)], env=env, stdout=subprocess.PIPE,
                                 stderr=(out / f"{editor}-kicad.log").open("w"), text=True)
        ready = threading.Event()

        def read():
            for line in kicad.stdout:
                if line.startswith("KiCad API server listening at "):
                    ready.set()

        threading.Thread(target=read, daemon=True).start()
        assert ready.wait(120), "KiCad not ready"

        with IpcSession(args.kicad_cli, api_socket=sock, timeout=120000, client_name="person:pane") as person, \
                IpcSession(args.kicad_cli, api_socket=sock, timeout=120000, client_name="agent:t1") as agent, \
                IpcSession(args.kicad_cli, api_socket=sock, timeout=120000, client_name="works.backplane.hub") as hub:
            embed = hub.module("backplane.embed")
            editor_cmds = hub.module("common.commands.editor_commands")
            types = hub.module("common.types.base_types")
            enums = hub.module("common.types.enums")
            pcb = editor == "pcb"
            kind = embed.FK_PCB if pcb else embed.FK_SCH
            frame = types.FT_PCB_EDITOR if pcb else types.FT_SCHEMATIC_EDITOR

            hub.request(embed.ShowFrame(kind=kind, x=0, y=0, width=1400, height=900), embed.ShowFrameResponse)
            time.sleep(4)
            doc = hub.request(editor_cmds.GetOpenDocuments(type=types.DOCTYPE_PCB if pcb else types.DOCTYPE_SCHEMATIC),
                              editor_cmds.GetOpenDocumentsResponse).documents[0]
            header = types.ItemHeader(document=doc)
            real_types = ([enums.KOT_PCB_FOOTPRINT, enums.KOT_PCB_TRACE] if pcb
                          else [enums.KOT_SCH_SYMBOL, enums.KOT_SCH_LINE])

            def count():
                return len(hub.request(editor_cmds.GetItems(header=header, types=real_types),
                                       editor_cmds.GetItemsResponse).items)

            def markers():
                return len(hub.request(embed.GetMarkers(kind=kind), embed.GetMarkersResponse).markers)

            seq = [hub.request(embed.NextEvents(since=0, max=256), embed.NextEventsResponse).next]
            dialogs = []

            def drain():
                reply = hub.request(embed.NextEvents(since=seq[0], max=256), embed.NextEventsResponse)
                seq[0] = reply.next
                dialogs.extend(e.detail for e in reply.events if e.type == embed.ET_DIALOG_OPENED)
                return reply.events

            def run_checks():
                pid = hub.request(embed.RunChecks(kind=kind), embed.RunChecksResponse).progress_id
                done = []

                def finished():
                    done.extend(e for e in drain() if e.type == embed.ET_CHECKS_DONE)
                    return bool(done)

                return wait_for(finished, 120, 0.5)

            def action(api, name):
                api.request(editor_cmds.RunAction(action=name, frame=frame), editor_cmds.RunActionResponse)
                time.sleep(1.0)
                drain()

            def agent_add(y_mm):
                t = agent.request(editor_cmds.BeginCommit(header=header), editor_cmds.BeginCommitResponse)
                c = editor_cmds.CreateItems(header=header)
                if pcb:
                    board = agent.module("board.board_types")
                    c.items.add().Pack(board.Track(
                        id=types.KIID(value=str(uuid.uuid4())),
                        start=types.Vector2(x_nm=100000000, y_nm=y_mm * 1000000),
                        end=types.Vector2(x_nm=160000000, y_nm=y_mm * 1000000),
                        width=types.Distance(value_nm=300000), layer=board.BL_F_Cu))
                else:
                    sch = agent.module("schematic.schematic_types")
                    c.items.add().Pack(sch.SchematicLine(
                        start=types.Vector2(x_nm=20000000, y_nm=y_mm * 1000000),
                        end=types.Vector2(x_nm=40000000, y_nm=y_mm * 1000000), type=1))
                agent.request(c, editor_cmds.CreateItemsResponse)
                agent.request(editor_cmds.EndCommit(header=header, id=t.id, action=editor_cmds.CMA_COMMIT,
                                                    message="agent item"), editor_cmds.EndCommitResponse)
                drain()

            base = count()
            for rnd in range(1, 4):
                tag = f"{editor} round {rnd}"
                agent_add(30 + rnd * 5)                  # the agent's own item (its own undo)
                full = count()
                check(full == base + 1, f"{tag}: the agent's item is on the canvas ({base} -> {full})")
                check(run_checks() and markers() > 0, f"{tag}: checks leave markers ({markers()})")

                action(person, "common.Interactive.selectAll")
                action(person, "common.Interactive.delete")
                check(count() == 0, f"{tag}: select all + delete removes every real item ({count()} left)")
                run_checks()
                action(person, "common.Interactive.undo")
                check(count() == full, f"{tag}: undo brings all {full} back ({count()})")
                run_checks()
                action(person, "common.Interactive.redo")
                check(count() == 0, f"{tag}: redo removes them again ({count()} left)")
                run_checks()
                action(person, "common.Interactive.undo")
                check(count() == full, f"{tag}: undo after the checks brings all {full} back ({count()})")
                action(person, "common.Interactive.redo")
                check(count() == 0, f"{tag}: redo after that removes them ({count()} left)")
                action(person, "common.Interactive.undo")
                check(count() == full, f"{tag}: and undo restores them for the next round ({count()})")
                run_checks()

                action(agent, "common.Interactive.undo")
                check(count() == base, f"{tag}: the agent's undo takes back only its item ({count()} of {base})")
                check(not dialogs, f"{tag}: no dialog appeared ({dialogs})")
                check(kicad.poll() is None, f"{tag}: KiCad is running")
    except Exception as error:   # noqa: BLE001
        check(False, f"{editor}: {type(error).__name__}: {error}")
    finally:
        if kicad is not None and kicad.poll() is None:
            kicad.kill()
        for p in reversed(procs):
            if p.poll() is None:
                p.kill()
        shutil.rmtree(work, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kicad")
    ap.add_argument("kicad_cli")
    ap.add_argument("--out", type=Path, default=Path("embed-undo-markers-out"))
    ap.add_argument("--editor", action="append", choices=["pcb", "sch"])
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for editor in args.editor or ["pcb", "sch"]:
        run_editor(editor, args, args.out)
    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
