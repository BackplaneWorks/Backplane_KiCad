#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Opening, checking and saving a project in `kicad --embedded` changes nothing on disk.

The path a person takes with Backplane, nothing more: on a copy of each project, `kicad --embedded`
shows the board, the hub's check runs (RunChecks{FK_PCB, parity}; StartDrcTask{parity} where the
runtime has no RunChecks), the schematic is shown and checked (RunChecks{FK_SCH}; StartErcTask),
then SaveDocument for the board and the schematic, with nothing edited.  Required:

- every .kicad_pcb and .kicad_sch file of the project keeps its bytes and its mtime;
- both documents read unmodified (GetFrames, GetDocumentModifiedState) after opening, after the
  checks and after the saves;
- the co-edit change log has no entry but KiCad's own (actor "kicad"); none for the API client;
- with --drc-parity (default: complex_hierarchy), the editor's DRC equals kicad-cli's
  (pcb drc --severity-all --schematic-parity, in the session's own configuration): same count.

    backplane-embed-open-save-identity-check.py <kicad> <kicad-cli> [--project DIR ...]
                                                [--drc-parity NAME ...] [--out DIR]
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import (FAILURES, IpcSession, Session, check,  # noqa: E402
                                          viewable, wait_for)

DEMOS = Path(__file__).resolve().parents[1] / "demos"


def snapshot(folder):
    out = {}
    for p in sorted(folder.rglob("*")):
        if p.suffix in (".kicad_pcb", ".kicad_sch") and p.is_file():
            out[str(p.relative_to(folder))] = (hashlib.sha256(p.read_bytes()).hexdigest(), p.stat().st_mtime_ns)
    return out


def staged(spec):
    """A project folder, or a .kicad_pro in a folder that holds several: then a copy of the folder
    with only that project file, so the session opens that one"""
    if spec.suffix != ".kicad_pro":
        return spec, spec.name
    tmp = Path(tempfile.mkdtemp(prefix="open-save-src-")) / spec.parent.name
    shutil.copytree(spec.parent, tmp)
    for other in tmp.glob("*.kicad_pro"):
        if other.name != spec.name:
            other.unlink()
    return tmp, spec.stem


def run_project(args, spec, out):
    source, name = staged(spec)
    with Session(args.kicad, out / name, source) as s, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=600000) as api:
        embed = api.module("backplane.embed")
        base = api.module("common.types.base_types")
        ed = api.module("common.commands.editor_commands")
        proj = api.module("common.commands.project_commands")
        tasks = api.module("common.tasks")
        try:
            coedit = api.module("backplane.coedit")
        except Exception:
            coedit = None
        w, h = s.host.size
        since = [0]
        before = snapshot(s.project_dir)
        check(any(k.endswith(".kicad_pcb") for k in before), f"{name}: the copy has a board ({len(before)} files)")

        def events():
            r = api.request_raw(embed.NextEvents(since=since[0]))
            if r.status.status != api.envelope.AS_OK:
                return []
            reply = embed.NextEventsResponse()
            r.message.Unpack(reply)
            for e in reply.events:
                since[0] = max(since[0], e.seq)
            return list(reply.events)

        def show(kind):
            for f in api.request(embed.GetFrames(), embed.GetFramesResponse).frames:
                if f.shown and f.kind != kind:
                    api.request(embed.HideFrame(window=f.window), embed.HideFrameResponse)
            fr = api.request(embed.ShowFrame(kind=kind, x=0, y=0, width=w, height=h), embed.ShowFrameResponse).frame
            wait_for(lambda: viewable(s.host.display, fr.window), 180)
            time.sleep(3)

        def doc(dtype):
            r = api.request_raw(ed.GetOpenDocuments(type=dtype))
            if r.status.status != api.envelope.AS_OK:
                return None
            reply = ed.GetOpenDocumentsResponse()
            r.message.Unpack(reply)
            return reply.documents[0] if reply.documents else None

        def unmodified(label):
            for kind, dtype, what in ((embed.FK_PCB, base.DOCTYPE_PCB, "board"),
                                      (embed.FK_SCH, base.DOCTYPE_SCHEMATIC, "schematic")):
                f = next((f for f in api.request(embed.GetFrames(), embed.GetFramesResponse).frames
                          if f.kind == kind), None)
                d = doc(dtype)
                if f is None or d is None:
                    continue
                st = api.request_raw(ed.GetDocumentModifiedState(document=d))
                state = "?"
                if st.status.status == api.envelope.AS_OK:
                    r = ed.GetDocumentModifiedStateResponse()
                    st.message.Unpack(r)
                    state = ed.DocumentModifiedState.Name(r.state)
                check(not f.modified and state != "DMS_MODIFIED" and not f.title.startswith("*"),
                      f"{name} {label}: the {what} reads unmodified (title {f.title!r}, modified {f.modified}, {state})")

        def check_run(kind, label):
            events()
            r = api.request_raw(embed.RunChecks(kind=kind, parity=(kind == embed.FK_PCB)))
            if r.status.status == api.envelope.AS_OK:
                wait_for(lambda: any(e.type == embed.ET_CHECKS_DONE for e in events()), 900, 0.5)
                return len(api.request(embed.GetMarkers(kind=kind), embed.GetMarkersResponse).markers)
            if kind == embed.FK_PCB:
                bt = api.module("board.board_tasks")
                tok = api.request(bt.StartDrcTask(board=doc(base.DOCTYPE_PCB), test_schematic_parity=True),
                                  tasks.StartTaskResponse).token
                wait_for(lambda: api.request(tasks.GetTaskStatus(token=tok), tasks.TaskStatusResponse).state
                         != tasks.TS_RUNNING, 900, 1)
                return len(api.request(bt.GetDrcTaskResult(token=tok), bt.DrcTaskResult).violations)
            st = api.module("schematic.schematic_tasks")
            tok = api.request(st.StartErcTask(document=doc(base.DOCTYPE_SCHEMATIC)), tasks.StartTaskResponse).token
            wait_for(lambda: api.request(tasks.GetTaskStatus(token=tok), tasks.TaskStatusResponse).state
                     != tasks.TS_RUNNING, 900, 1)
            return len(api.request(st.GetErcTaskResult(token=tok), st.ErcTaskResult).violations)

        show(embed.FK_PCB)
        unmodified("opened")
        drc = check_run(embed.FK_PCB, "DRC")
        has_sch = any(k.endswith(".kicad_sch") for k in before)
        if has_sch:
            show(embed.FK_SCH)
            unmodified("both shown")
            check_run(embed.FK_SCH, "ERC")
        unmodified("checked")

        for dtype, what in ((base.DOCTYPE_PCB, "board"), (base.DOCTYPE_SCHEMATIC, "schematic")):
            d = doc(dtype)
            if d is None:
                continue
            r = api.request_raw(proj.SaveDocument(document=d))
            check(r.status.status == api.envelope.AS_OK,
                  f"{name}: SaveDocument of the {what} answers AS_OK ({r.status.status} {r.status.error_message[:80]!r})")
        time.sleep(1)
        unmodified("saved")

        after = snapshot(s.project_dir)
        for f in sorted(set(before) | set(after)):
            b, a = before.get(f), after.get(f)
            check(b == a, f"{name}: {f} keeps its bytes and mtime"
                          + ("" if b == a else f" (sha {b[0][:12] if b else '-'} -> {a[0][:12] if a else '-'}, "
                                               f"mtime {'same' if b and a and b[1] == a[1] else 'changed'})"))

        if coedit is not None:
            c = api.request_raw(coedit.GetChanges(since=0))
            if c.status.status == api.envelope.AS_OK:
                r = coedit.GetChangesResponse()
                c.message.Unpack(r)
                others = [ch for ch in r.changes if ch.actor != "kicad"]
                check(not others, f"{name}: the change log has nothing but KiCad's own entries "
                                  f"({[(ch.actor, ch.description) for ch in others][:4]})")
                kicad = [ch.description for ch in r.changes if ch.actor == "kicad"]
                if kicad:
                    print(f"  {name}: KiCad's own load-time changes: {kicad}", flush=True)

        if name in args.drc_parity:
            board = s.project.with_suffix(".kicad_pcb")
            j = out / f"{name}-cli.json"
            subprocess.run([str(args.kicad_cli), "pcb", "drc", "--severity-all", "--schematic-parity", "--format",
                            "json", "-o", str(j), str(board)], env=s.env, cwd=s.project_dir,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=900)
            rep = json.loads(j.read_text()) if j.exists() else {}
            cli = sum(len(rep.get(k, [])) for k in ("violations", "unconnected_items", "schematic_parity"))
            check(rep and cli == drc, f"{name}: the editor's DRC equals kicad-cli's ({drc} editor, {cli} kicad-cli)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--project", type=Path, action="append",
                        help="a project folder, or a .kicad_pro in a folder of several (copied, never "
                             "changed); repeatable. Default: demos complex_hierarchy, video, ecc83-pp")
    parser.add_argument("--drc-parity", action="append", default=None)
    parser.add_argument("--out", type=Path, default=Path("embed-open-save-out"))
    args = parser.parse_args()
    projects = args.project or [DEMOS / "complex_hierarchy", DEMOS / "video", DEMOS / "ecc83" / "ecc83-pp.kicad_pro"]
    args.drc_parity = set(args.drc_parity or ["complex_hierarchy"])
    for p in projects:
        run_project(args, p.resolve(), args.out)
    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
