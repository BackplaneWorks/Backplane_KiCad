#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bp.13 checks for `kicad --embedded`: GerbView in the process, OpenGerbers, editor tabs.

- OpenGerbers loads the board's Gerbers and drill files (exported here with kicad-cli) into an
  embedded GerbView, hidden or shown; a missing and an unreadable file fail with reasons and no
  dialog; FrameInfo.files lists what is loaded; a second call replaces it.
- FrameInfo.lib_id names the footprint and symbol on screen in the library editors.
- "Open Footprint in Footprint Editor" from the board posts ET_EDITOR_REQUESTED for the footprint
  editor with that footprint's lib_id.

Runs on a private Xvfb + i3 with a python-xlib stand-in host; screenshots to --out.
"""

import argparse
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import (FAILURES, IpcSession, Session, check,  # noqa: E402
                                          viewable, wait_for)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--out", type=Path, default=Path("embed-editors-out"))
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / "demos/pic_programmer"

    with Session(args.kicad, args.out, source) as s, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=300000) as api:
        embed = api.module("backplane.embed")
        editor = api.module("common.commands.editor_commands")
        disp = s.host.display
        w, h = s.host.size
        since = [0]

        def events():
            reply = api.request(embed.NextEvents(since=since[0]), embed.NextEventsResponse)
            for e in reply.events:
                since[0] = max(since[0], e.seq)
            return list(reply.events)

        def frames():
            return {f.kind: f for f in api.request(embed.GetFrames(), embed.GetFramesResponse).frames}

        def show(kind, item=""):
            for f in frames().values():
                if f.shown and f.kind != kind:
                    api.request(embed.HideFrame(window=f.window), embed.HideFrameResponse)
            r = api.request(embed.ShowFrame(kind=kind, x=0, y=0, width=w, height=h, item=item),
                            embed.ShowFrameResponse)
            wait_for(lambda: viewable(disp, r.frame.window), 60)
            time.sleep(2)
            return r.frame

        # Fabrication outputs, as Backplane's mfg/ holds them
        mfg = args.out / "mfg"
        mfg.mkdir(parents=True, exist_ok=True)
        board = next(Path(s.project_dir).glob("*.kicad_pcb"))
        env = dict(s.env)
        subprocess.run([str(args.kicad_cli), "pcb", "export", "gerbers", "--output", str(mfg) + "/", str(board)],
                       env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run([str(args.kicad_cli), "pcb", "export", "drill", "--output", str(mfg) + "/", str(board)],
                       env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        gerbers = sorted(str(p) for p in mfg.iterdir()
                         if p.suffix.lower() in (".gtl", ".gbl", ".gto", ".gbo", ".gts", ".gbs", ".gm1", ".drl",
                                                 ".gbr"))
        junk = mfg / "notes.gbr"
        junk.write_text("this is not a Gerber file\n")
        missing = str(mfg / "missing.gtl")
        check(len(gerbers) >= 5, f"kicad-cli exported Gerbers and drill files ({len(gerbers)})")

        show(embed.FK_PCB)

        # Hidden first, as a prewarm
        before = set(s.watcher.current())
        t = time.monotonic()
        reply = api.request(embed.OpenGerbers(files=gerbers + [missing, str(junk)]), embed.OpenGerbersResponse)
        took = time.monotonic() - t
        failed = dict(zip(reply.failed, reply.errors))
        check(sorted(reply.loaded) == sorted(gerbers),
              f"OpenGerbers loads every Gerber and drill file ({len(reply.loaded)}/{len(gerbers)}, {took:.1f} s)")
        check(missing in failed and str(junk) in failed and all(failed.values()),
              f"a missing and an unreadable file fail, each with a reason ({failed})")
        check(reply.frame.kind == embed.FK_GERBVIEW and not reply.frame.shown,
              "GerbView opened hidden in KiCad's own process")
        check(not set(s.watcher.current()) - before, "no dialog appeared")

        frame = show(embed.FK_GERBVIEW)
        listed = frames()[embed.FK_GERBVIEW].files
        check(sorted(listed) == sorted(gerbers), f"FrameInfo.files lists what is loaded ({len(listed)})")
        s.shot("01-gerbview")

        reply = api.request(embed.OpenGerbers(files=gerbers[:2], show=True, x=0, y=0, width=w, height=h),
                            embed.OpenGerbersResponse)
        check(sorted(frames()[embed.FK_GERBVIEW].files) == sorted(gerbers[:2]) and reply.frame.shown,
              "a second OpenGerbers replaces what GerbView shows")
        s.shot("02-gerbview-two")

        # Library editors name what is on screen
        fp = show(embed.FK_FOOTPRINT_EDITOR, item="footprints:RV2X4")
        time.sleep(1)
        check(frames()[embed.FK_FOOTPRINT_EDITOR].lib_id == "footprints:RV2X4",
              f"the footprint editor's lib_id ({frames()[embed.FK_FOOTPRINT_EDITOR].lib_id})")
        sym = show(embed.FK_SYMBOL_EDITOR, item="pic_programmer:C")
        time.sleep(1)
        check(frames()[embed.FK_SYMBOL_EDITOR].lib_id == "pic_programmer:C",
              f"the symbol editor's lib_id ({frames()[embed.FK_SYMBOL_EDITOR].lib_id})")

        # Open Footprint from the board: ET_EDITOR_REQUESTED with the footprint's lib_id
        show(embed.FK_PCB)
        sel = api.request(embed.SelectItems(kind=embed.FK_PCB, references=["RV1"]), embed.SelectItemsResponse)
        events()
        api.request_raw(editor.RunAction(action="pcbnew.EditorControl.EditFpInFpEditor"))
        got = []
        wait_for(lambda: got.extend(e for e in events() if e.type == embed.ET_EDITOR_REQUESTED) or got, 15, 0.3)
        check(bool(got) and got[0].frame.kind == embed.FK_FOOTPRINT_EDITOR and got[0].frame.lib_id,
              f"Open Footprint posts ET_EDITOR_REQUESTED with its lib_id "
              f"({[(e.frame.kind, e.frame.lib_id) for e in got]}, RV1 found: {list(sel.found.references)})")

    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
