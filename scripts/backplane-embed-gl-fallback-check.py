#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bp.13 check for `kicad --embedded` without usable OpenGL (a machine with no GPU, a remote session).

OpenGL is made unusable for KiCad (Mesa reports version 2.0, below the 2.1 KiCad's OpenGL canvas
needs; --gl-version picks another).  Then the PCB editor, the schematic editor, the symbol and
footprint editors and GerbView are shown one after the other, and:

- no top-level KiCad window other than the frames appears (no "Could not use OpenGL, falling back
  to software rendering" box: before, it was a modal over the frame that held the editor up until
  someone clicked it), and no ET_DIALOG_OPENED;
- each frame draws (its screenshot is not one flat colour);
- an API edit (BeginCommit, CreateItems, EndCommit) and a RunAction are answered, not AS_BUSY;
- GetCapabilities has embed-notice, and exactly one ET_NOTICE kind "graphics" arrives for the whole
  process: the fallback sticks, so the later frames neither try OpenGL again nor say it again;
- KiCad's stderr names the fallback.

Runs on a private Xvfb + i3 with a python-xlib stand-in host; screenshots to --out.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import (FAILURES, IpcSession, Session, check,  # noqa: E402
                                          viewable, wait_for)


def flat(path, crop):
    """True when a crop of a screenshot is one colour (nothing drawn)."""
    result = subprocess.run(["convert", str(path), "-crop", crop, "+repage", "-format",
                             "%[fx:standard_deviation]", "info:-"], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout
    try:
        return float(result.strip() or 0) < 0.01
    except ValueError:
        return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--out", type=Path, default=Path("embed-gl-fallback-out"))
    parser.add_argument("--gl-version", default="2.0", help="MESA_GL_VERSION_OVERRIDE for KiCad")
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / "demos/pic_programmer"

    os.environ["MESA_GL_VERSION_OVERRIDE"] = args.gl_version
    os.environ.pop("KICAD_SOFTWARE_RENDERING", None)

    with Session(args.kicad, args.out, source) as s, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=120000) as api:
        embed = api.module("backplane.embed")
        editor = api.module("common.commands.editor_commands")
        base = api.module("common.types.base_types")
        board = api.module("board.board_types")
        capabilities = api.module("backplane.capabilities")
        envelope = api.envelope
        disp = s.host.display
        w, h = s.host.size
        got = []
        since = [0]

        def events():
            reply = api.request_raw(embed.NextEvents(since=since[0]))
            if reply.status.status != envelope.AS_OK:
                return
            out = embed.NextEventsResponse()
            reply.message.Unpack(out)
            for e in out.events:
                since[0] = max(since[0], e.seq)
                got.append(e)

        def answered(command, label):
            reply = api.request_raw(command)
            status = envelope.ApiStatusCode.Name(reply.status.status)
            check(reply.status.status == envelope.AS_OK,
                  f"{label} is answered ({status} {reply.status.error_message})")
            return reply

        def popups():
            """Top-level windows the window manager ever managed, other than the host's own."""
            return [(hex(wid), name) for wid, (transient, name) in s.watcher.seen.items()
                    if wid != s.host.top.id]

        caps = api.request(capabilities.GetCapabilities(), capabilities.GetCapabilitiesResponse).capabilities
        check("embed-notice" in caps, "GetCapabilities has embed-notice")

        # Fabrication outputs for GerbView
        mfg = args.out / "mfg"
        mfg.mkdir(parents=True, exist_ok=True)
        pcb = next(Path(s.project_dir).glob("*.kicad_pcb"))
        for what in ("gerbers", "drill"):
            subprocess.run([str(args.kicad_cli), "pcb", "export", what, "--output", str(mfg) + "/", str(pcb)],
                           env=dict(s.env), check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        gerbers = sorted(str(p) for p in mfg.iterdir() if p.suffix.lower() in (".gtl", ".gbl", ".gm1", ".drl"))

        shown = []

        def show(kind, label, item=""):
            name = embed.FrameKind.Name(kind)
            reply = api.request_raw(embed.ShowFrame(kind=kind, x=0, y=0, width=w, height=h, item=item))
            if not check(reply.status.status == envelope.AS_OK,
                         f"{name}: ShowFrame is answered ({envelope.ApiStatusCode.Name(reply.status.status)} "
                         f"{reply.status.error_message})"):
                return None
            r = embed.ShowFrameResponse()
            reply.message.Unpack(r)
            for window in shown:
                if window != r.frame.window:
                    api.request_raw(embed.HideFrame(window=window))
            shown[:] = [r.frame.window]
            check(wait_for(lambda: viewable(disp, r.frame.window), 60), f"{name}: the frame is on screen")
            time.sleep(4)
            s.host.pump(0.2)
            events()
            shot = s.shot(label)
            check(not flat(shot, f"{w - 200}x{h - 200}+100+100"), f"{name}: the frame draws ({shot.name})")
            check(not popups(), f"{name}: no top-level KiCad window besides the frame ({popups()})")
            return r.frame

        # PCB editor: shown, edited through the API, an action run
        show(embed.FK_PCB, "01-pcb")
        docs = api.request(editor.GetOpenDocuments(type=base.DOCTYPE_PCB), editor.GetOpenDocumentsResponse)
        if check(len(docs.documents) == 1, "the board is open"):
            header = base.ItemHeader(document=docs.documents[0])
            reply = answered(editor.BeginCommit(header=header), "BeginCommit")
            commit = editor.BeginCommitResponse()
            reply.message.Unpack(commit)
            create = editor.CreateItems(header=header)
            create.items.add().Pack(board.Track(id=base.KIID(value=str(uuid.uuid4())),
                                                start=base.Vector2(x_nm=60000000, y_nm=60000000),
                                                end=base.Vector2(x_nm=70000000, y_nm=60000000),
                                                width=base.Distance(value_nm=250000), layer=board.BL_F_Cu))
            answered(create, "an API edit (CreateItems of a track)")
            answered(editor.EndCommit(header=header, id=commit.id, action=editor.CMA_COMMIT,
                                      message="gl-fallback check edit"), "EndCommit")
        reply = answered(editor.RunAction(action="common.Control.zoomFitScreen"), "RunAction zoomFitScreen")
        if reply.status.status == envelope.AS_OK:
            ran = editor.RunActionResponse()
            reply.message.Unpack(ran)
            check(ran.status == editor.RAS_OK, f"RunAction answers RAS_OK ({ran.status})")

        show(embed.FK_SCH, "02-schematic")
        show(embed.FK_SYMBOL_EDITOR, "03-symbol-editor", item="pic_programmer:C")
        show(embed.FK_FOOTPRINT_EDITOR, "04-footprint-editor", item="footprints:RV2X4")
        if gerbers:
            answered(embed.OpenGerbers(files=gerbers), "OpenGerbers")
        show(embed.FK_GERBVIEW, "05-gerbview")

        # The PCB editor still takes actions at the end
        show(embed.FK_PCB, "06-pcb-again")
        answered(editor.RunAction(action="common.Control.zoomRedraw"), "a second RunAction")

        time.sleep(2)
        events()
        notices = [json.loads(e.detail or "{}") for e in got if e.type == embed.ET_NOTICE]
        graphics = [n for n in notices if n.get("kind") == "graphics"]
        check(len(graphics) == 1 and "OpenGL" in graphics[0].get("title", ""),
              f"exactly one ET_NOTICE kind graphics for the whole process, about OpenGL ({notices})")
        opened = [e.detail for e in got if e.type == embed.ET_DIALOG_OPENED]
        check(not opened, f"no ET_DIALOG_OPENED ({opened})")
        check(not popups(), f"no top-level KiCad window at any point ({popups()})")

    log = (args.out / "kicad.log").read_text(errors="replace")
    check("notice graphics" in log, "KiCad's stderr names the fallback")

    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
