#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Autosave of API commits in an embedded editor: an agent's API commit in the embedded PCB editor while KiCad is
not the active app.  Checks: GetDocumentModifiedState 2, a local-history snapshot within the
debounce, Close(fail_if_modified) refuses (the close prompt's condition), and that the person's
undo and the agent's undo stay separate.

Usage: backplane-embed-api-autosave-check.py KICAD KICAD_CLI [--out DIR]
Needs Xvfb, i3, xdotool, git, python3-xlib, pynng, protobuf.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE / "scripts"))
from backplane_ipc_test_support import IpcSession  # noqa: E402

FAILURES = []


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("kicad")
    ap.add_argument("kicad_cli")
    ap.add_argument("--out", default="embed-api-autosave-out")
    ap.add_argument("--autosave", type=int, default=3, help="autosave interval, s")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    from Xlib import X, display as xdisplay

    work = Path(tempfile.mkdtemp(prefix="apimod-"))
    number = next(n for n in range(150, 400) if not Path(f"/tmp/.X11-unix/X{n}").exists())
    name = f":{number}"
    env = dict(os.environ)
    env.update({"DISPLAY": name, "GDK_BACKEND": "x11", "NO_AT_BRIDGE": "1", "LIBGL_ALWAYS_SOFTWARE": "1",
                "WXTRACE": "KICAD_AUTOSAVE", "KICAD_ENABLE_WXTRACE": "1"})
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
        (c / "kicad_common.json").write_text(
            '{"system": {"local_history_debounce": %d}, "auto_backup": {"enabled": true}}' % args.autosave)

    proj_dir = work / "pic_programmer"
    shutil.copytree(SOURCE / "demos/pic_programmer", proj_dir)
    project = proj_dir / "pic_programmer.kicad_pro"

    procs = []
    kicad = None
    try:
        procs.append(subprocess.Popen(["Xvfb", name, "-screen", "0", "1600x1000x24", "-nolisten", "tcp",
                                       "+extension", "GLX"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        assert wait_for(lambda: Path(f"/tmp/.X11-unix/X{number}").exists(), 10)
        i3conf = work / "i3.conf"
        i3conf.write_text("font pango:monospace 8\ndefault_border none\nfocus_follows_mouse no\n")
        procs.append(subprocess.Popen(["i3", "-c", str(i3conf)], env=env,
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(1.5)
        disp = xdisplay.Display(name)
        scr = disp.screen()
        top = scr.root.create_window(0, 0, 1400, 900, 0, scr.root_depth, X.InputOutput, X.CopyFromParent,
                                     background_pixel=scr.white_pixel)
        top.set_wm_name("EmbedHost")
        cont = top.create_window(0, 0, 1400, 900, 0, scr.root_depth, X.InputOutput, X.CopyFromParent,
                                 background_pixel=0x303030)
        cont.map()
        top.map()
        disp.sync()
        time.sleep(1)

        sock = work / "run/kicad/api.sock"
        sock.parent.mkdir(parents=True, exist_ok=True)
        log = (out / "kicad.log").open("w")
        kicad = subprocess.Popen([args.kicad, "--embedded", "--parent-window", hex(cont.id),
                                  "--toplevel-window", str(top.id), "--api-socket", str(sock),
                                  "--parent-pid", str(os.getpid()), str(project)],
                                 env=env, stdout=subprocess.PIPE, stderr=log, text=True)
        end = time.monotonic() + 120
        while time.monotonic() < end:
            line = kicad.stdout.readline()
            if not line or line.startswith("KiCad API server listening at "):
                break
        import threading
        threading.Thread(target=lambda: [None for _ in kicad.stdout], daemon=True).start()

        def session(client):
            return IpcSession(args.kicad_cli, api_socket=sock, timeout=120000, client_name=client)

        with session("agent:t1") as agent, session("person:pane") as person:
            embed = agent.module("backplane.embed")
            editor = agent.module("common.commands.editor_commands")
            board = agent.module("board.board_types")
            types = agent.module("common.types.base_types")
            base = agent.module("common.commands.base_commands")

            agent.request(embed.ShowFrame(kind=embed.FK_PCB, x=0, y=0, width=1400, height=900),
                          embed.ShowFrameResponse)
            time.sleep(3)
            # Backplane has the keyboard, as when an agent edits while the person types in the hub
            subprocess.run(["xdotool", "windowactivate", "--sync", str(top.id)], env=env, check=False)
            subprocess.run(["xdotool", "windowfocus", "--sync", str(top.id)], env=env, check=False)
            time.sleep(1)

            docs = agent.request(editor.GetOpenDocuments(type=types.DOCTYPE_PCB), editor.GetOpenDocumentsResponse)
            doc = docs.documents[0]
            header = types.ItemHeader(document=doc)

            def state(api):
                return api.request(editor.GetDocumentModifiedState(document=doc),
                                   editor.GetDocumentModifiedStateResponse).state

            def tracks(api):
                r = api.request(editor.GetItems(header=header, types=[agent.module("common.types.enums").KOT_PCB_TRACE]),
                                editor.GetItemsResponse)
                return len(r.items)

            hist = proj_dir / ".history"

            def snapshots():
                if not hist.exists():
                    return 0
                r = subprocess.run(["git", "-C", str(hist), "rev-list", "--all", "--count"], text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
                return int(r.stdout.strip() or 0) if r.returncode == 0 else len(list(hist.rglob("*")))

            check(state(agent) == 1, f"fresh board unmodified (state {state(agent)})")
            n0 = tracks(agent)
            s0 = snapshots()

            tr = agent.request(editor.BeginCommit(header=header), editor.BeginCommitResponse)
            create = editor.CreateItems(header=header)
            create.items.add().Pack(board.Track(
                id=types.KIID(value=str(uuid.uuid4())),
                start=types.Vector2(x_nm=120000000, y_nm=100000000),
                end=types.Vector2(x_nm=140000000, y_nm=100000000),
                width=types.Distance(value_nm=300000), layer=board.BL_F_Cu))
            agent.request(create, editor.CreateItemsResponse)
            agent.request(editor.EndCommit(header=header, id=tr.id, action=editor.CMA_COMMIT,
                                           message="agent add track"), editor.EndCommitResponse)
            check(tracks(agent) == n0 + 1, "the agent's track is on the board")
            st = state(agent)
            check(st == 2, f"agent commit: GetDocumentModifiedState 2 (got {st})")
            frames = {f.kind: f for f in agent.request(embed.GetFrames(), embed.GetFramesResponse).frames}
            print(f"INFO frame modified {frames[embed.FK_PCB].modified}", flush=True)

            got = wait_for(lambda: snapshots() > s0, args.autosave + 12, 0.5)
            check(got, f"agent commit: local-history snapshot within {args.autosave}+12 s ({s0} -> {snapshots()})")

            refused = agent.request_raw(embed.Close(mode=embed.CM_FAIL_IF_MODIFIED))
            check(refused.status.status != agent.envelope.AS_OK and "unsaved" in refused.status.error_message,
                  f"close with fail_if_modified refuses: the close prompt would ask ({refused.status.error_message!r})")

            def add_track(api, y_mm, msg):
                t = api.request(editor.BeginCommit(header=header), editor.BeginCommitResponse)
                c = editor.CreateItems(header=header)
                c.items.add().Pack(board.Track(
                    id=types.KIID(value=str(uuid.uuid4())),
                    start=types.Vector2(x_nm=120000000, y_nm=y_mm * 1000000),
                    end=types.Vector2(x_nm=140000000, y_nm=y_mm * 1000000),
                    width=types.Distance(value_nm=300000), layer=board.BL_F_Cu))
                api.request(c, editor.CreateItemsResponse)
                api.request(editor.EndCommit(header=header, id=t.id, action=editor.CMA_COMMIT, message=msg),
                            editor.EndCommitResponse)

            def undo(api, redo=False):
                api.request(editor.RunAction(action="common.Interactive." + ("redo" if redo else "undo")),
                            editor.RunActionResponse)
                time.sleep(1)

            # Undo separation: a person's edit after the agent's; each undo takes only its own.
            add_track(person, 110, "person add track")
            check(tracks(agent) == n0 + 2, "the person's track is on the board too")
            undo(person)
            check(tracks(agent) == n0 + 1, "the person's undo takes the person's track only")
            undo(person)
            check(tracks(agent) == n0 + 1, "a second person undo leaves the agent's track")
            undo(person, redo=True)
            check(tracks(agent) == n0 + 2, "the person's redo puts the person's track back")
            undo(agent)
            check(tracks(agent) == n0 + 1, "the agent's undo takes its own track, not the person's")
            undo(agent, redo=True)
            check(tracks(agent) == n0 + 2, "the agent's redo puts it back")

            # Hidden frame: the person on another tab while the agent edits the board
            agent.request(embed.HideFrame(window=frames[embed.FK_PCB].window), embed.HideFrameResponse)
            time.sleep(args.autosave + 3)
            s1 = snapshots()
            add_track(agent, 90, "agent add track (hidden)")
            check(tracks(agent) == n0 + 3, "the agent's second track is on the hidden board")
            got = wait_for(lambda: snapshots() > s1, args.autosave + 12, 0.5)
            check(got, f"agent commit on a hidden frame: snapshot within {args.autosave}+12 s ({s1} -> {snapshots()})")
    finally:
        if kicad and kicad.poll() is None:
            kicad.kill()
        for p in reversed(procs):
            p.terminate()
        shutil.rmtree(work, ignore_errors=True)

    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
