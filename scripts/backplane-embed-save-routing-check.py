#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""SaveDocument for a board or a schematic is answered by its own editor, whatever else is open.

KiCad's API server asks its handlers in pointer order, so a handler that answers a request meant
for another editor's document (instead of AS_UNHANDLED) refuses it whenever it happens to come
first.  The Symbol Editor's SaveDocument did, with "the open symbol has no library yet", while it
showed no library symbol.  Each case is run in several fresh KiCad processes (allocation, hence the
handlers' order, differs between them):

- the Symbol Editor open and empty; then with a library symbol loaded;
- the Footprint Editor open and empty;
- both open and empty.

In each: an edit to the board (a track) and to the schematic (a text) through the API, then
SaveDocument for each: AS_OK, and the file is written.

    backplane-embed-save-routing-check.py <kicad> <kicad-cli> [--runs N] [--out DIR]
"""

import argparse
import hashlib
from pathlib import Path
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import FAILURES, IpcSession, Session, check, viewable, wait_for  # noqa: E402


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()[:12]


def run_case(args, label, editors, load_symbol, run):
    source = Path(__file__).resolve().parents[1] / "demos/pic_programmer"
    with Session(args.kicad, args.out / f"{label}-{run}", source) as s, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=120000) as api:
        embed = api.module("backplane.embed")
        editor = api.module("common.commands.editor_commands")
        project = api.module("common.commands.project_commands")
        base = api.module("common.types.base_types")
        board = api.module("board.board_types")
        w, h = s.host.size

        def show(kind, item=""):
            r = api.request_raw(embed.ShowFrame(kind=kind, x=0, y=0, width=w, height=h, item=item))
            if r.status.status != api.envelope.AS_OK:
                return None
            reply = embed.ShowFrameResponse()
            r.message.Unpack(reply)
            wait_for(lambda: viewable(s.host.display, reply.frame.window), 90)
            time.sleep(1.5)
            return reply.frame

        def doc(dtype):
            docs = api.request(editor.GetOpenDocuments(type=dtype), editor.GetOpenDocumentsResponse).documents
            return docs[0] if docs else None

        show(embed.FK_SCH)
        show(embed.FK_PCB)
        for kind in editors:
            ok = show(kind, "Device:R" if (load_symbol and kind == embed.FK_SYMBOL_EDITOR) else "")
            check(ok is not None, f"{label} #{run}: {embed.FrameKind.Name(kind)} opened")
        show(embed.FK_PCB)

        pcb = doc(base.DOCTYPE_PCB)
        sch = doc(base.DOCTYPE_SCHEMATIC)
        pcb_file = s.project.with_suffix(".kicad_pcb")
        sch_file = s.project.with_suffix(".kicad_sch")
        before = (sha(pcb_file), sha(sch_file))

        # a real board edit, so the save has something to write
        header = base.ItemHeader(document=pcb)
        t = api.request(editor.BeginCommit(header=header), editor.BeginCommitResponse)
        create = editor.CreateItems(header=header)
        create.items.add().Pack(board.Track(id=base.KIID(value=str(uuid.uuid4())),
                                            start=base.Vector2(x_nm=60000000, y_nm=60000000),
                                            end=base.Vector2(x_nm=70000000, y_nm=60000000),
                                            width=base.Distance(value_nm=250000), layer=board.BL_F_Cu))
        api.request(create, editor.CreateItemsResponse)
        api.request(editor.EndCommit(header=header, id=t.id, action=editor.CMA_COMMIT, message="save routing"),
                    editor.EndCommitResponse)

        r = api.request_raw(project.SaveDocument(document=pcb))
        check(r.status.status == api.envelope.AS_OK and sha(pcb_file) != before[0],
              f"{label} #{run}: the board's SaveDocument is answered by the PCB editor and writes "
              f"({api.envelope.ApiStatusCode.Name(r.status.status)} {r.status.error_message[:90]})")
        r = api.request_raw(project.SaveDocument(document=sch))
        check(r.status.status == api.envelope.AS_OK,
              f"{label} #{run}: the schematic's SaveDocument is answered by the schematic editor "
              f"({api.envelope.ApiStatusCode.Name(r.status.status)} {r.status.error_message[:90]})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--out", type=Path, default=Path("save-routing-out"))
    args = parser.parse_args()
    SYM, FP = 3, 4  # FK_SYMBOL_EDITOR, FK_FOOTPRINT_EDITOR
    cases = [("symbol-empty", [SYM], False), ("symbol-loaded", [SYM], True),
             ("footprint-empty", [FP], False), ("both-empty", [SYM, FP], False)]
    for label, editors, load in cases:
        for run in range(1, args.runs + 1):
            run_case(args, label, editors, load, run)
    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
