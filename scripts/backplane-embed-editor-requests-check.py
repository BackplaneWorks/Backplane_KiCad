#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bp.13 check for `kicad --embedded`: ET_EDITOR_REQUESTED says what the person asked for, and
only that.

- A host ShowFrame of the footprint or symbol editor with an item (lib_id) posts no
  ET_EDITOR_REQUESTED: the host asked for it already.  (It used to echo one per ShowFrame, so a
  host that shows on every request looped.)
- KiCad's own Footprint Editor action (common.Control.showFootprintEditor) on an editor that
  already exists tells the host: ET_SHOW_REQUESTED or ET_EDITOR_REQUESTED for the footprint
  editor, both when the host has hidden it and when it is still shown behind the board.
- Open Footprint from the board still posts ET_EDITOR_REQUESTED with the footprint's lib_id.
- RunAction reaches the editor that has the action: eeschema.InspectionTool.runERC opens the ERC
  dialog, pcbnew.InspectionTool.runDRC the DRC one, and an unknown action is not answered OK.

Runs on a private Xvfb + i3 with a python-xlib stand-in host.
"""

import argparse
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import (FAILURES, IpcSession, Session, check,  # noqa: E402
                                          viewable, wait_for)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--out", type=Path, default=Path("embed-editor-requests-out"))
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

        def collect(seconds):
            got = []
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                got.extend(events())
                time.sleep(0.25)
            return got

        def requests(evts, kind):
            return [(e.type, e.frame.lib_id) for e in evts
                    if e.type in (embed.ET_EDITOR_REQUESTED, embed.ET_SHOW_REQUESTED) and e.frame.kind == kind]

        def frames():
            return {f.kind: f for f in api.request(embed.GetFrames(), embed.GetFramesResponse).frames}

        def show(kind, item="", hide_others=True):
            if hide_others:
                for f in frames().values():
                    if f.shown and f.kind != kind:
                        api.request(embed.HideFrame(window=f.window), embed.HideFrameResponse)
            r = api.request(embed.ShowFrame(kind=kind, x=0, y=0, width=w, height=h, item=item),
                            embed.ShowFrameResponse)
            wait_for(lambda: viewable(disp, r.frame.window), 60)
            return r.frame

        show(embed.FK_PCB)
        time.sleep(2)
        collect(1)

        # The host's own ShowFrame with an item: no echo
        show(embed.FK_FOOTPRINT_EDITOR, item="footprints:RV2X4")
        got = requests(collect(3), embed.FK_FOOTPRINT_EDITOR)
        check(not any(t == embed.ET_EDITOR_REQUESTED for t, _ in got),
              f"ShowFrame of the footprint editor with an item posts no ET_EDITOR_REQUESTED ({got})")
        show(embed.FK_FOOTPRINT_EDITOR, item="footprints:RV2X4")
        got = requests(collect(3), embed.FK_FOOTPRINT_EDITOR)
        check(not any(t == embed.ET_EDITOR_REQUESTED for t, _ in got),
              f"a second ShowFrame of the same item posts none either ({got})")
        check(frames()[embed.FK_FOOTPRINT_EDITOR].lib_id == "footprints:RV2X4",
              f"the footprint editor shows the item ({frames()[embed.FK_FOOTPRINT_EDITOR].lib_id})")

        show(embed.FK_SYMBOL_EDITOR, item="pic_programmer:C")
        got = requests(collect(3), embed.FK_SYMBOL_EDITOR)
        check(not any(t == embed.ET_EDITOR_REQUESTED for t, _ in got),
              f"ShowFrame of the symbol editor with an item posts no ET_EDITOR_REQUESTED ({got})")

        # KiCad's Footprint Editor action on the existing editor, hidden by the host
        show(embed.FK_PCB)
        time.sleep(1)
        collect(1)
        api.request_raw(editor.RunAction(action="common.Control.showFootprintEditor"))
        got = requests(collect(4), embed.FK_FOOTPRINT_EDITOR)
        check(bool(got), f"the Footprint Editor action on the hidden editor tells the host ({got})")

        # ... and on the editor still shown behind the board (a host that stacks its frames)
        show(embed.FK_FOOTPRINT_EDITOR)
        show(embed.FK_PCB, hide_others=False)
        time.sleep(1)
        collect(1)
        api.request_raw(editor.RunAction(action="common.Control.showFootprintEditor"))
        got = requests(collect(4), embed.FK_FOOTPRINT_EDITOR)
        check(bool(got), f"the Footprint Editor action on the editor shown behind the board tells the host ({got})")

        # Open Footprint from the board still asks for the editor with its lib_id
        show(embed.FK_PCB)
        time.sleep(1)
        sel = api.request(embed.SelectItems(kind=embed.FK_PCB, references=["RV1"]), embed.SelectItemsResponse)
        collect(1)
        api.request_raw(editor.RunAction(action="pcbnew.EditorControl.EditFpInFpEditor"))
        got = [e for e in collect(5) if e.type == embed.ET_EDITOR_REQUESTED]
        check(len(got) == 1 and got[0].frame.kind == embed.FK_FOOTPRINT_EDITOR and got[0].frame.lib_id,
              f"Open Footprint posts one ET_EDITOR_REQUESTED with its lib_id "
              f"({[(e.frame.kind, e.frame.lib_id) for e in got]}, RV1 found: {list(sel.found.references)})")

        # The schematic editor's own actions over RunAction: ERC opens its dialog (the board's
        # handler used to answer for every action and did not know eeschema.*)
        show(embed.FK_SCH)
        time.sleep(2)
        collect(1)
        ran = api.request(editor.RunAction(action="eeschema.InspectionTool.runERC"), editor.RunActionResponse)
        check(ran.status == editor.RAS_OK, f"RunAction eeschema.InspectionTool.runERC answers RAS_OK ({ran.status})")
        opened = [e.detail for e in collect(8) if e.type == embed.ET_DIALOG_OPENED]
        check(any("ERC" in d or "Electrical" in d for d in opened),
              f"RunAction runERC opens the ERC dialog (ET_DIALOG_OPENED {opened})")
        s.shot("erc-dialog")
        s.xdotool("key", "Escape")
        time.sleep(1)

        # A board action still runs on the board, and an unknown one is refused
        show(embed.FK_PCB)
        time.sleep(1)
        ran = api.request(editor.RunAction(action="pcbnew.InspectionTool.runDRC"), editor.RunActionResponse)
        check(ran.status == editor.RAS_OK, f"RunAction pcbnew.InspectionTool.runDRC answers RAS_OK ({ran.status})")
        opened = [e.detail for e in collect(8) if e.type == embed.ET_DIALOG_OPENED]
        check(any("DRC" in d or "Design Rules" in d for d in opened),
              f"RunAction runDRC opens the DRC dialog (ET_DIALOG_OPENED {opened})")
        s.xdotool("key", "Escape")
        time.sleep(1)
        unknown = api.request_raw(editor.RunAction(action="nosuch.Tool.action"))
        check(unknown.status.status != 1 or "RAS_INVALID" in str(unknown.message),
              f"RunAction of an unknown action is not answered OK ({unknown.status.status})")

    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
