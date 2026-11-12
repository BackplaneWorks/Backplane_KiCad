#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stress test of live co-editing (docs/backplane/coedit.md).

The GUI runs embedded (`kicad --embedded`) on a private X server.  A person driver uses the PCB
editor the way a person does (xdotool: select, move with M, drag, open Properties, Ctrl+Z) while
an agent driver edits the same board over IPC (updates and moves footprints, creates and
deletes tracks, groups some of it in commits), both at once, for --seconds.

Checks: KiCad stays up; no item operation of the agent is refused (AS_BUSY) outside a
structural dialog; every deferred operation is applied or reported dropped; the person's Ctrl+Z
never reverts an agent change without a notice; the saved board reloads with the same items
as the live one.  Screenshots go to --out.

Needs: Xvfb, i3, xdotool, ImageMagick `import`, python3-xlib, pynng, protobuf, protoc.
"""

import argparse
import json
import os
from pathlib import Path
import random
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_ipc_test_support import IpcSession  # noqa: E402

import importlib.util  # noqa: E402

_spec = importlib.util.spec_from_file_location(
    "embed_check", Path(__file__).resolve().parent / "backplane-embed-x11-check.py")
embed_check = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(embed_check)

FAILURES = []


def check(condition, message):
    print(("PASS " if condition else "FAIL ") + message, flush=True)
    if not condition:
        FAILURES.append(message)
    return condition


class AgentStats:
    def __init__(self):
        self.lock = threading.Lock()
        self.counts = {}
        self.errors = []

    def add(self, key, n=1):
        with self.lock:
            self.counts[key] = self.counts.get(key, 0) + n

    def error(self, text):
        with self.lock:
            self.errors.append(text)


def agent_loop(args, socket, board_doc, stop, stats, seed):
    """The agent: edits the board over IPC as fast as a model-driven agent plausibly would."""
    rng = random.Random(seed)
    with IpcSession(args.kicad_cli, api_socket=socket, timeout=60000,
                    client_name="agent:stress") as api:
        envelope = api.envelope
        editor = api.module("common.commands.editor_commands")
        board_types = api.module("board.board_types")
        types = api.module("common.types.base_types")
        enums = api.module("common.types.enums")

        created = []

        def doc():
            d = types.DocumentSpecifier()
            d.CopyFrom(board_doc)
            return d

        def footprints():
            req = editor.GetItems()
            req.header.document.CopyFrom(doc())
            req.types.append(enums.KOT_PCB_FOOTPRINT)
            reply = api.request(req, editor.GetItemsResponse)
            out = []
            for item in reply.items:
                fp = board_types.FootprintInstance()
                item.Unpack(fp)
                out.append(fp)
            return out

        def busy(response, what):
            if response.status.status == envelope.AS_BUSY:
                stats.add("busy")
                stats.error(f"{what}: AS_BUSY {response.status.error_message}")
                return True
            if response.status.status != envelope.AS_OK:
                stats.add("error")
                stats.error(f"{what}: {response.status}")
                return True
            return False

        def update_footprint(fp):
            fp.position.x_nm += rng.choice((-1, 1)) * 100000
            fp.position.y_nm += rng.choice((-1, 1)) * 100000
            req = editor.UpdateItems()
            req.header.document.CopyFrom(doc())
            req.items.add().Pack(fp)
            response = api.request_raw(req)
            if busy(response, "UpdateItems"):
                return
            result = editor.UpdateItemsResponse()
            response.message.Unpack(result)
            for r in result.updated_items:
                stats.add("update." + editor.ItemStatusCode.Name(r.status.code))

        def create_track():
            t = board_types.Track()
            x = rng.randint(100, 200) * 1000000
            y = rng.randint(60, 140) * 1000000
            t.start.x_nm, t.start.y_nm = x, y
            t.end.x_nm, t.end.y_nm = x + rng.randint(2, 10) * 1000000, y
            t.width.value_nm = 250000
            t.layer = board_types.BL_B_Cu
            req = editor.CreateItems()
            req.header.document.CopyFrom(doc())
            req.items.add().Pack(t)
            response = api.request_raw(req)
            if busy(response, "CreateItems"):
                return
            result = editor.CreateItemsResponse()
            response.message.Unpack(result)
            for r in result.created_items:
                stats.add("create." + editor.ItemStatusCode.Name(r.status.code))
                track = board_types.Track()
                r.item.Unpack(track)
                if track.id.value:
                    created.append(track.id.value)

        def delete_track():
            if not created:
                return
            kiid = created.pop(rng.randrange(len(created)))
            req = editor.DeleteItems()
            req.header.document.CopyFrom(doc())
            req.item_ids.add().value = kiid
            response = api.request_raw(req)
            if busy(response, "DeleteItems"):
                return
            result = editor.DeleteItemsResponse()
            response.message.Unpack(result)
            for r in result.deleted_items:
                stats.add("delete." + editor.ItemDeletionStatus.Name(r.status))

        fps = footprints()
        while not stop.is_set():
            try:
                roll = rng.random()
                if roll < 0.10:
                    begin = editor.BeginCommit()
                    begin.header.document.CopyFrom(doc())
                    response = api.request_raw(begin)
                    if busy(response, "BeginCommit"):
                        continue
                    opened = editor.BeginCommitResponse()
                    response.message.Unpack(opened)
                    for _ in range(rng.randint(2, 5)):
                        create_track()
                        update_footprint(rng.choice(fps))
                    end = editor.EndCommit()
                    end.header.document.CopyFrom(doc())
                    end.id.CopyFrom(opened.id)
                    end.action = editor.CMA_COMMIT
                    end.message = "Agent: grouped placement nudge"
                    if not busy(api.request_raw(end), "EndCommit"):
                        stats.add("commit")
                elif roll < 0.45:
                    update_footprint(rng.choice(fps))
                elif roll < 0.75:
                    create_track()
                else:
                    delete_track()
                if rng.random() < 0.05:
                    fps = footprints()
            except Exception as error:  # noqa: BLE001
                stats.add("exception")
                stats.error(f"exception: {error}")
                if "timed out" in str(error):
                    return
            time.sleep(rng.uniform(0.02, 0.15))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path, help="the GUI kicad binary")
    parser.add_argument("kicad_cli", type=Path, help="kicad-cli (for the protobuf bindings)")
    parser.add_argument("--out", type=Path, default=Path("coedit-stress-out"))
    parser.add_argument("--seconds", type=int, default=120)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--xvfb", default=shutil.which("Xvfb") or "Xvfb")
    parser.add_argument("--i3", default=shutil.which("i3") or "i3")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    work = Path(tempfile.mkdtemp(prefix="backplane-coedit-"))
    processes = []
    number = embed_check.free_display()
    name = f":{number}"
    env = dict(os.environ)
    env.update({
        "DISPLAY": name, "GDK_BACKEND": "x11", "GDK_SCALE": "1", "NO_AT_BRIDGE": "1",
        "LIBGL_ALWAYS_SOFTWARE": "1", "GALLIUM_DRIVER": "llvmpipe", "LP_NUM_THREADS": "4",
    })
    env.pop("WAYLAND_DISPLAY", None)
    for variable, directory in (("HOME", "home"), ("XDG_CONFIG_HOME", "config"),
                                ("XDG_DATA_HOME", "data"), ("XDG_CACHE_HOME", "cache"),
                                ("XDG_RUNTIME_DIR", "run"), ("TMPDIR", "tmp")):
        path = work / directory
        path.mkdir(parents=True, exist_ok=True, mode=0o700)
        env[variable] = str(path)
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

    def xdotool(*words):
        subprocess.run(["xdotool", *map(str, words)], env=env, check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    kicad = None
    try:
        processes.append(subprocess.Popen([args.xvfb, name, "-screen", "0", "1600x1000x24",
                                           "-nolisten", "tcp", "+extension", "GLX"],
                                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        assert embed_check.wait_for(lambda: Path(f"/tmp/.X11-unix/X{number}").exists(), 10)
        i3conf = work / "i3.conf"
        i3conf.write_text(f"ipc-socket {work}/i3.sock\nfont pango:monospace 8\n"
                          "default_border none\ndefault_floating_border normal\n"
                          "focus_follows_mouse no\n")
        processes.append(subprocess.Popen([args.i3, "-c", str(i3conf)], env=env,
                                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(1.5)
        host = embed_check.Host(name, 1500, 950)

        socket = work / "run/kicad/api.sock"
        socket.parent.mkdir(parents=True, exist_ok=True)
        log = (args.out / "kicad.log").open("w")
        kicad = subprocess.Popen([str(args.kicad), "--embedded", "--parent-window",
                                  hex(host.container.id), "--toplevel-window", str(host.top.id),
                                  "--api-socket", str(socket), "--parent-pid", str(os.getpid()),
                                  str(project)],
                                 env=env, stdout=subprocess.PIPE, stderr=log, text=True)
        deadline = time.monotonic() + 120
        ready = False
        while time.monotonic() < deadline:
            line = kicad.stdout.readline()
            if not line:
                break
            if line.startswith("KiCad API server listening at "):
                ready = True
                break
        threading.Thread(target=lambda: [None for _ in kicad.stdout], daemon=True).start()
        if not check(ready, "KiCad started"):
            return 1

        with IpcSession(args.kicad_cli, api_socket=socket, timeout=120000,
                        client_name="host") as api:
            embed = api.module("backplane.embed")
            coedit = api.module("backplane.coedit")
            base = api.module("common.commands.base_commands")
            editor = api.module("common.commands.editor_commands")
            types = api.module("common.types.base_types")
            enums = api.module("common.types.enums")
            board_types = api.module("board.board_types")

            frames = api.request(embed.GetFrames(), embed.GetFramesResponse)
            host.display.create_resource_object("window", frames.holder).change_save_set(1)
            w, h = host.size
            reply = api.request(embed.ShowFrame(kind=embed.FK_PCB, x=0, y=0, width=w, height=h,
                                                focus=True),
                                embed.ShowFrameResponse)
            check(reply.frame.window != 0, "PCB editor shown")
            host.pump(3.0)

            board_doc = types.DocumentSpecifier()
            board_doc.type = types.DOCTYPE_PCB
            board_doc.board_filename = board_file.name
            open_docs = api.request(editor.GetOpenDocuments(type=types.DOCTYPE_PCB),
                                    editor.GetOpenDocumentsResponse)
            if open_docs.documents:
                board_doc.CopyFrom(open_docs.documents[0])

            req = editor.GetItems()
            req.header.document.CopyFrom(board_doc)
            req.types.append(enums.KOT_PCB_FOOTPRINT)
            refs = []
            for item in api.request(req, editor.GetItemsResponse).items:
                fp = board_types.FootprintInstance()
                item.Unpack(fp)
                refs.append(fp.reference_field.text.text.text)
            check(len(refs) > 10, f"board has footprints ({len(refs)})")
            shot("00-start")

            stop = threading.Event()
            stats = AgentStats()
            agent = threading.Thread(target=agent_loop,
                                     args=(args, socket, board_doc, stop, stats, args.seed))
            agent.start()

            rng = random.Random(args.seed + 1)
            cx, cy = w // 2, h // 2
            person = {"moves": 0, "drags": 0, "dialogs": 0, "undos": 0}
            start = time.monotonic()
            shots = 0
            while time.monotonic() - start < args.seconds:
                if kicad.poll() is not None:
                    break
                ref = rng.choice(refs)
                api.request(embed.SelectItems(kind=embed.FK_PCB, references=[ref], zoom=True),
                            embed.SelectItemsResponse)
                time.sleep(0.3)
                xdotool("mousemove", cx, cy)
                roll = rng.random()
                if roll < 0.40:
                    # Move with M: the tool runs while the agent keeps editing.
                    xdotool("key", "m")
                    for step in range(8):
                        xdotool("mousemove", cx + 6 * step, cy + 4 * step)
                        time.sleep(0.08)
                    xdotool("click", "1")
                    person["moves"] += 1
                elif roll < 0.65:
                    xdotool("mousedown", "1")
                    for step in range(8):
                        xdotool("mousemove", cx - 5 * step, cy + 3 * step)
                        time.sleep(0.06)
                    xdotool("mouseup", "1")
                    person["drags"] += 1
                elif roll < 0.85:
                    xdotool("key", "e")
                    time.sleep(1.2)
                    if shots < 3:
                        shot(f"10-dialog-{shots}")
                        shots += 1
                    xdotool("key", "Escape")
                    person["dialogs"] += 1
                else:
                    xdotool("key", "ctrl+z")
                    person["undos"] += 1
                time.sleep(0.2)
                xdotool("key", "Escape")
                if rng.random() < 0.05:
                    shot(f"20-live-{int(time.monotonic() - start):03d}")

            stop.set()
            agent.join(timeout=90)
            check(not agent.is_alive(), "agent driver finished")
            check(kicad.poll() is None, "KiCad is still running")
            print("person:", json.dumps(person), flush=True)
            print("agent:", json.dumps(stats.counts, sort_keys=True), flush=True)
            for e in stats.errors[:20]:
                print("  agent error:", e, flush=True)

            check(stats.counts.get("busy", 0) == 0, "no agent item operation refused as busy")
            check(stats.counts.get("error", 0) == 0 and stats.counts.get("exception", 0) == 0,
                  "no agent errors")
            applied = sum(v for k, v in stats.counts.items() if k.endswith("ISC_OK") or k.endswith("IDS_OK"))
            deferred = sum(v for k, v in stats.counts.items() if "DEFERRED" in k)
            check(applied > 50, f"agent operations applied live ({applied})")
            print(f"deferred operations: {deferred}", flush=True)

            # Deferred operations drain once the person is idle.
            xdotool("key", "Escape")
            holds = None
            for _ in range(100):
                holds = api.request(coedit.GetHolds(document=board_doc), coedit.GetHoldsResponse)
                if holds.deferred == 0:
                    break
                time.sleep(0.1)
            check(holds is not None and holds.deferred == 0, "every deferred operation was applied")

            notices = api.request(coedit.GetNotices(actor="agent:stress"),
                                  coedit.GetNoticesResponse).notices
            kinds = {}
            for n in notices:
                kinds[n.kind] = kinds.get(n.kind, 0) + 1
            print("notices:", json.dumps(kinds, sort_keys=True), flush=True)
            if deferred:
                check(kinds.get("rebased", 0) + kinds.get("dropped", 0) > 0,
                      "deferred operations were reported rebased or dropped")

            changes = api.request(coedit.GetChanges(since=0, doc="pcb"),
                                  coedit.GetChangesResponse).changes
            took_back = {json.loads(n.json).get("seq") for n in notices if n.kind == "undo_took_back"}
            silent = [c for c in changes if c.undo and c.actor == "agent:stress"
                      and not c.rebased and c.seq not in took_back]
            # An undo record carries the undone change's actor; match it to a notice by
            # description rather than seq (the record has a new seq).
            took_back_desc = {json.loads(n.json).get("description") for n in notices
                              if n.kind == "undo_took_back"}
            silent = [c for c in silent if c.description not in took_back_desc]
            person_changes = sum(1 for c in changes if not c.actor and not c.undo)
            agent_changes = sum(1 for c in changes if c.actor == "agent:stress" and not c.undo)
            print(f"change log: {person_changes} person, {agent_changes} agent, "
                  f"{sum(1 for c in changes if c.undo)} undo/redo", flush=True)
            check(person_changes > 0 and agent_changes > 0, "both actors' changes are in the log")
            check(not silent, f"the person's Ctrl+Z never silently reverted an agent change ({len(silent)})")
            shot("30-end")

            # Save and compare with a fresh load of the file.
            def counts():
                out = {}
                for kind in (enums.KOT_PCB_FOOTPRINT, enums.KOT_PCB_TRACE, enums.KOT_PCB_VIA):
                    req = editor.GetItems()
                    req.header.document.CopyFrom(board_doc)
                    req.types.append(kind)
                    out[kind] = len(api.request(req, editor.GetItemsResponse).items)
                return out

            live = counts()
            project_cmds = api.module("common.commands.project_commands")
            saved_reply = api.request_raw(project_cmds.SaveDocument(document=board_doc))
            check(saved_reply.status.status == api.envelope.AS_OK, "SaveDocument from the API")
            text = board_file.read_text()
            saved = {enums.KOT_PCB_FOOTPRINT: len(re.findall(r"^\t\(footprint ", text, re.M)),
                     enums.KOT_PCB_TRACE: len(re.findall(r"^\t\(segment\b", text, re.M)),
                     enums.KOT_PCB_VIA: len(re.findall(r"^\t\(via\b", text, re.M))}
            print("live:", live, "saved:", saved, flush=True)
            check(live[enums.KOT_PCB_FOOTPRINT] == saved[enums.KOT_PCB_FOOTPRINT]
                  and live[enums.KOT_PCB_VIA] == saved[enums.KOT_PCB_VIA],
                  "the saved board has the live board's footprints and vias")
            drc = subprocess.run([str(args.kicad_cli), "pcb", "drc", "--output",
                                  str(work / "drc.json"), "--format", "json", str(board_file)],
                                 env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            check(drc.returncode in (0, 5), f"the saved board loads in kicad-cli (rc {drc.returncode})")

    finally:
        if kicad and kicad.poll() is None:
            kicad.terminate()
            try:
                kicad.wait(10)
            except subprocess.TimeoutExpired:
                kicad.kill()
        for p in reversed(processes):
            p.terminate()
        shutil.rmtree(work, ignore_errors=True)

    print(f"{len(FAILURES)} failure(s)", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
