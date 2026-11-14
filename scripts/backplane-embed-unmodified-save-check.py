#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Embedded, the editor's own Save (Ctrl+S, File > Save) of a document with nothing changed writes
nothing; after a real edit it writes.

The fixture (qa/data/pcbnew/issue21184, an older file format) has a project top-level sheet UUID
unlike the root file's own, so loading it repairs instance paths in memory and adds implied
junctions: before this change a Save with nothing edited rewrote the files (paths re-keyed, the
format upgraded, junctions with fresh random UUIDs).  One session per editor; Save is the person's Ctrl+S on the
focused frame (bp.12 has no RunAction for the schematic editor).

    backplane-embed-unmodified-save-check.py KICAD KICAD_CLI [--out DIR]
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
import tempfile
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import FAILURES, Session, check, viewable, wait_for  # noqa: E402
from backplane_ipc_test_support import IpcSession  # noqa: E402

PROJECT_ROOT = "6a7b3c1e-3333-4444-8555-a66677778888"


def make_project(dest):
    source = Path(__file__).resolve().parents[1] / "qa/data/pcbnew/issue21184"
    shutil.copytree(source, dest)
    pro = dest / "issue21184.kicad_pro"
    data = json.loads(pro.read_text())
    data.setdefault("schematic", {})["top_level_sheets"] = [
        {"filename": "issue21184.kicad_sch", "name": "issue21184", "uuid": PROJECT_ROOT}]
    pro.write_text(json.dumps(data, indent=2))


def state(paths):
    return {p.name: (p.stat().st_mtime_ns, hashlib.sha256(p.read_bytes()).hexdigest()) for p in paths}


def run(kicad, kicad_cli, out, project, kind_name):
    with Session(kicad, out / kind_name, project) as s, \
            IpcSession(kicad_cli, api_socket=s.socket, timeout=600000) as api:
        embed = api.module("backplane.embed")
        editor = api.module("common.commands.editor_commands")
        base = api.module("common.types.base_types")
        kind = embed.FK_PCB if kind_name == "pcb" else embed.FK_SCH
        doctype = base.DOCTYPE_PCB if kind_name == "pcb" else base.DOCTYPE_SCHEMATIC
        files = sorted(s.project_dir.glob("*.kicad_pcb" if kind_name == "pcb" else "*.kicad_sch"))
        w, h = s.host.size
        r = api.request(embed.ShowFrame(kind=kind, x=0, y=0, width=w, height=h, focus=True),
                        embed.ShowFrameResponse)
        wait_for(lambda: viewable(s.host.display, r.frame.window), 180)
        time.sleep(3)
        reserved = list(api.request(embed.GetFrames(), embed.GetFramesResponse).reserved_keys)
        check(not any(k.lower().replace(" ", "") in ("ctrl+s", "control+s") for k in reserved),
              f"{kind_name}: Ctrl+S is KiCad's, not reserved by the host ({reserved})")

        def save():
            api.request(embed.ShowFrame(kind=kind, x=0, y=0, width=w, height=h, focus=True),
                        embed.ShowFrameResponse)
            s.host.pump(0.5)
            s.xdotool("mousemove", str(w // 2), str(h // 2))
            s.xdotool("key", "Escape")
            s.xdotool("key", "ctrl+s")

        def frame():
            return next((f for f in api.request(embed.GetFrames(), embed.GetFramesResponse).frames
                         if f.kind == kind), None)

        others = [embed.FrameKind.Name(f.kind) for f in api.request(embed.GetFrames(), embed.GetFramesResponse).frames
                  if f.kind in (embed.FK_PCB, embed.FK_SCH) and f.kind != kind]
        check(not others, f"{kind_name}: only its editor is open, so Save reaches it ({others})")
        check(frame() is not None and not frame().modified, f"{kind_name}: loads unmodified")

        before = state(files)
        time.sleep(1.1)   # an mtime change shows even on a coarse clock
        save()
        time.sleep(3)
        check(state(files) == before, f"{kind_name}: Save with nothing changed writes nothing (bytes and mtime)")

        doc = api.request(editor.GetOpenDocuments(type=doctype), editor.GetOpenDocumentsResponse).documents[0]
        header = base.ItemHeader(document=doc)
        commit = api.request(editor.BeginCommit(header=header), editor.BeginCommitResponse)
        create = editor.CreateItems(header=header)
        if kind_name == "pcb":
            board = api.module("board.board_types")
            create.items.add().Pack(board.Track(id=base.KIID(value=str(uuid.uuid4())),
                                                start=base.Vector2(x_nm=60000000, y_nm=60000000),
                                                end=base.Vector2(x_nm=70000000, y_nm=60000000),
                                                width=base.Distance(value_nm=250000), layer=board.BL_F_Cu))
        else:
            sch = api.module("schematic.schematic_types")
            create.items.add().Pack(sch.SchematicLine(id=base.KIID(value=str(uuid.uuid4())),
                                                      start=base.Vector2(x_nm=10160000, y_nm=10160000),
                                                      end=base.Vector2(x_nm=20320000, y_nm=10160000),
                                                      type=sch.SLT_GRAPHIC))
        api.request(create, editor.CreateItemsResponse)
        api.request(editor.EndCommit(header=header, id=commit.id, action=editor.CMA_COMMIT,
                                     message="unmodified-save check edit"), editor.EndCommitResponse)
        check(wait_for(lambda: frame().modified, 10), f"{kind_name}: the edit marks it modified")
        save()
        wait_for(lambda: not frame().modified, 20)
        after = state(files)
        main_file = files[0].name if kind_name == "pcb" else "issue21184.kicad_sch"
        check(after[main_file] != before[main_file], f"{kind_name}: Save after an edit writes")
        check(not frame().modified, f"{kind_name}: and leaves it unmodified")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--out", type=Path, default=Path("unmodified-save-out"))
    args = parser.parse_args()
    work = Path(tempfile.mkdtemp(prefix="unmodified-save-"))
    project = work / "issue21184"
    make_project(project)
    for kind_name in ("pcb", "sch"):
        run(args.kicad, args.kicad_cli, args.out, project, kind_name)
    shutil.rmtree(work, ignore_errors=True)
    print("FAILED: %d" % len(FAILURES) if FAILURES else "ALL PASS")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
