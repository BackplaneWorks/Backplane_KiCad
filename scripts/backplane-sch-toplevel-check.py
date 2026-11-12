#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A schematic whose project names its top-level sheet by a UUID other than the root file's own.

KiCad 10.99 roots every symbol and sheet instance path at the project's top-level sheet UUID
(.kicad_pro schematic.top_level_sheets).  Checks:

1. Headless (kicad-cli api-server): load and SaveDocument keep the paths rooted at the project's
   UUID (they were rewritten to the root file's UUID, which the editor then "repaired" on every load).
2. Embedded editor: other writers save sheets with paths rooted at the file's UUID, three times;
   each reloads with no modal "automatically fixed" dialog, the reloaded schematic is not counted
   as the person's edit (no conflict, the next change reloads too), and the editor still works.

    backplane-sch-toplevel-check.py KICAD KICAD_CLI [--out DIR]
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import FAILURES, IpcSession, Session, check, wait_for  # noqa: E402

PROJECT_ROOT = "6a7b3c1e-1111-4222-8333-944455556666"


def make_project(source, dest):
    """complex_hierarchy with a project top-level sheet UUID unlike the root file's own."""
    shutil.copytree(source, dest)
    root = dest / "complex_hierarchy.kicad_sch"
    file_uuid = re.search(r'\(uuid "([^"]+)"\)', root.read_text()).group(1)
    for sch in dest.glob("*.kicad_sch"):
        sch.write_text(sch.read_text().replace(f'(path "/{file_uuid}', f'(path "/{PROJECT_ROOT}'))
    pro = dest / "complex_hierarchy.kicad_pro"
    data = json.loads(pro.read_text())
    data.setdefault("schematic", {})["top_level_sheets"] = [
        {"filename": "complex_hierarchy.kicad_sch", "name": "complex_hierarchy", "uuid": PROJECT_ROOT}]
    pro.write_text(json.dumps(data, indent=2))
    return file_uuid


def roots(dest):
    found = {}
    for sch in dest.glob("*.kicad_sch"):
        for m in re.finditer(r'\(path "/([0-9a-f-]{36})', sch.read_text()):
            found[m.group(1)] = found.get(m.group(1), 0) + 1
    return found


def break_paths(path, file_uuid, n):
    """What a writer that roots paths at the file's UUID saves (atomic rename-over); n newlines
    make each write's content new."""
    text = path.read_text().replace(f'(path "/{PROJECT_ROOT}', f'(path "/{file_uuid}').rstrip()
    temporary = path.with_suffix(".tmp-write")
    temporary.write_text(text + "\n" * n)
    os.replace(temporary, path)


def headless(kicad_cli, out, source):
    work = Path(tempfile.mkdtemp(prefix="bp-sch-toplevel-"))
    dest = work / "complex_hierarchy"
    make_project(source, dest)
    sock = work / "api.sock"
    env = dict(os.environ, HOME=str(work / "home"), XDG_CONFIG_HOME=str(work / "config"))
    proc = subprocess.Popen([str(kicad_cli), "api-server", str(dest / "complex_hierarchy.kicad_pro"),
                             "--socket", str(sock), "--idle-exit", "120"], env=env,
                            stdout=(out / "api-server.log").open("w"), stderr=subprocess.STDOUT)
    try:
        wait_for(sock.exists, 60)
        with IpcSession(kicad_cli, api_socket=sock, timeout=120000, client_name="agent:tsch") as api:
            project = api.module("common.commands.project_commands")
            base = api.module("common.types.base_types")
            doc = api.request(project.OpenDocument(type=base.DOCTYPE_SCHEMATIC,
                                                   path=str(dest / "complex_hierarchy.kicad_sch")),
                              project.OpenDocumentResponse).document
            reply = api.request_raw(project.SaveDocument(document=doc))
            check(reply.status.status == api.envelope.AS_OK, f"headless: SaveDocument ({reply.status})")
        found = roots(dest)
        check(found.get(PROJECT_ROOT, 0) > 0 and set(found) == {PROJECT_ROOT},
              f"headless: a save keeps every instance path rooted at the project's sheet UUID ({found})")
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()


def embedded(kicad, kicad_cli, out, source):
    staged = Path(tempfile.mkdtemp(prefix="bp-sch-toplevel-src-")) / "complex_hierarchy"
    file_uuid = make_project(source, staged)
    with Session(kicad, out, staged) as s, \
            IpcSession(kicad_cli, api_socket=s.socket, timeout=120000, client_name="host") as api:
        embed = api.module("backplane.embed")
        editor = api.module("common.commands.editor_commands")
        base = api.module("common.types.base_types")
        since = [0]

        def events():
            reply = api.request(embed.NextEvents(since=since[0]), embed.NextEventsResponse)
            since[0] = reply.next
            return list(reply.events)

        w, h = s.host.size
        api.request(embed.ShowFrame(kind=embed.FK_SCH, x=0, y=0, width=w, height=h),
                    embed.ShowFrameResponse)
        def open_docs():
            return api.request(editor.GetOpenDocuments(type=base.DOCTYPE_SCHEMATIC),
                               editor.GetOpenDocumentsResponse).documents
        wait_for(lambda: bool(open_docs()), 60)
        doc = open_docs()
        check(bool(doc), "embedded: the schematic is open")
        time.sleep(4)
        s.shot("01-open")
        events()

        def modified():
            r = api.request(editor.GetDocumentModifiedState(document=doc[0]),
                            editor.GetDocumentModifiedStateResponse)
            return r.state == editor.DMS_MODIFIED

        def not_modal():
            # A modal dialog answers every write AS_BUSY.
            r = api.request_raw(editor.BeginCommit(header=base.ItemHeader(document=doc[0])))
            if r.status.status != api.envelope.AS_OK:
                return False
            begin = editor.BeginCommitResponse()
            r.message.Unpack(begin)
            api.request(editor.EndCommit(header=base.ItemHeader(document=doc[0]), id=begin.id,
                                         action=editor.CMA_DROP), editor.EndCommitResponse)
            return True

        check(not modified(), "embedded: loads clean (no repair)")
        files = [s.project_dir / "complex_hierarchy.kicad_sch", s.project_dir / "ampli_ht.kicad_sch",
                 s.project_dir / "complex_hierarchy.kicad_sch"]
        reloads, conflicts, modal_free, clean = 0, 0, 0, 0
        for i, path in enumerate(files):
            break_paths(path, file_uuid, i + 1)
            got = []
            wait_for(lambda: got.extend(events()) or any(e.type in (embed.ET_DOCUMENT_RELOADED,
                                                                    embed.ET_RELOAD_CONFLICT)
                                                         for e in got), 15, 0.3)
            time.sleep(1.5)
            s.shot(f"{i + 2:02d}-after-write-{i}")
            reloads += sum(e.type == embed.ET_DOCUMENT_RELOADED for e in got)
            conflicts += sum(e.type == embed.ET_RELOAD_CONFLICT for e in got)
            modal_free += not_modal()
            clean += not modified()
        check(reloads >= len(files) and conflicts == 0,
              f"embedded: each write reloads, none asks the person ({reloads} reloads, {conflicts} conflicts)")
        check(modal_free == len(files), f"embedded: no modal repair dialog ({modal_free}/{len(files)} free)")
        check(clean == len(files), f"embedded: a load-time repair is not the person's edit ({clean}/{len(files)})")
        check(s.kicad_process.poll() is None, "embedded: KiCad is still running")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("kicad", type=Path)
    ap.add_argument("kicad_cli", type=Path)
    ap.add_argument("--out", type=Path, default=Path("sch-toplevel-out"))
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parents[1] / "demos/complex_hierarchy"
    headless(a.kicad_cli, a.out, source)
    embedded(a.kicad, a.kicad_cli, a.out, source)
    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
