#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bp.13 check for `kicad --embedded`: a show request says who caused it.

- RunChecks{FK_PCB, parity} with no schematic editor loads the schematic hidden and posts no
  ET_SHOW_REQUESTED; a second run reuses that editor without reloading it.
- ShowFrame{FK_SCH, prewarm} keeps the schematic editor hidden and posts no ET_SHOW_REQUESTED.
- "Switch to Schematic Editor" run over RunAction by an agent (client agent:check) posts
  ET_SHOW_REQUESTED for FK_SCH with detail "agent:check": the host can tell it caused it.
- The same action run for the person (client person:check) posts it with an empty detail, as a
  click on KiCad's own toolbar does.

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
    parser.add_argument("--out", type=Path, default=Path("embed-show-request-actor-out"))
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / "demos/pic_programmer"

    with Session(args.kicad, args.out, source) as s, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=300000) as api:
        embed = api.module("backplane.embed")
        editor = api.module("common.commands.editor_commands")
        w, h = s.host.size
        since = [0]

        def collect(seconds):
            got = []
            deadline = time.monotonic() + seconds
            while True:
                reply = api.request(embed.NextEvents(since=since[0]), embed.NextEventsResponse)
                for e in reply.events:
                    since[0] = max(since[0], e.seq)
                    got.append(e)
                if time.monotonic() >= deadline:
                    return got
                time.sleep(0.25)

        def sch_requests(evts):
            return [e.detail for e in evts if e.type == embed.ET_SHOW_REQUESTED and e.frame.kind == embed.FK_SCH]

        def frames():
            return {f.kind: f for f in api.request(embed.GetFrames(), embed.GetFramesResponse).frames}

        r = api.request(embed.ShowFrame(kind=embed.FK_PCB, x=0, y=0, width=w, height=h), embed.ShowFrameResponse)
        wait_for(lambda: viewable(s.host.display, r.frame.window), 60)
        time.sleep(2)
        collect(1)

        # DRC with schematic parity and no schematic editor yet: it loads the schematic hidden and
        # asks for nothing (lane B's flip: four show requests and the schematic shown)
        check(embed.FK_SCH not in frames(), "no schematic editor before the DRC")
        rc = api.request(embed.RunChecks(kind=embed.FK_PCB, parity=True), embed.RunChecksResponse)
        got = collect(1)
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline and not any(e.type == embed.ET_CHECKS_DONE for e in got):
            got += collect(1)
        done = [e for e in got if e.type == embed.ET_CHECKS_DONE]
        check(bool(done), f"RunChecks FK_PCB parity finishes (ET_CHECKS_DONE, progress {rc.progress_id})")
        check(embed.FK_SCH in frames() and not frames()[embed.FK_SCH].shown,
              f"the parity check loaded the schematic editor hidden ({[(f.kind, f.shown) for f in frames().values()]})")
        check(not sch_requests(got), f"the parity check posts no ET_SHOW_REQUESTED ({sch_requests(got)})")
        check(frames()[embed.FK_PCB].shown, "the board stays on screen")

        # Again, with the hidden schematic editor already loaded: used as it is
        rc = api.request(embed.RunChecks(kind=embed.FK_PCB, parity=True), embed.RunChecksResponse)
        got = collect(1)
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline and not any(e.type == embed.ET_CHECKS_DONE for e in got):
            got += collect(1)
        check(any(e.type == embed.ET_CHECKS_DONE for e in got) and not sch_requests(got)
              and not any(e.type == embed.ET_DOCUMENT_RELOADED and e.frame.kind == embed.FK_SCH for e in got),
              f"a second parity check reuses the hidden schematic (no reload, no request: "
              f"{[embed.EventType.Name(e.type) for e in got if e.frame.kind == embed.FK_SCH]})")

        # Switch to Schematic Editor on the hidden, loaded editor: a request, and no reload
        with IpcSession(args.kicad_cli, api_socket=s.socket, timeout=60000, client_name="person:check") as c:
            c.request_raw(editor.RunAction(action="pcbnew.EditorControl.showEeschema"))
        got = collect(4)
        check(sch_requests(got) == [""] or (sch_requests(got) and all(d == "" for d in sch_requests(got))),
              f"Switch to Schematic Editor on the hidden editor asks the host ({sch_requests(got)})")
        check(not any(e.type in (embed.ET_DOCUMENT_RELOADED, embed.ET_PROGRESS) and e.frame.kind == embed.FK_SCH
                      for e in got),
              f"... without reloading the schematic "
              f"({[embed.EventType.Name(e.type) for e in got if e.frame.kind == embed.FK_SCH]})")

        # The host's prewarm of the (now open) schematic editor: still hidden, no request
        api.request(embed.ShowFrame(kind=embed.FK_SCH, width=804, height=896, prewarm=True), embed.ShowFrameResponse)
        got = collect(8)
        check(not frames()[embed.FK_SCH].shown, "a prewarm keeps the schematic editor hidden")
        check(not sch_requests(got), f"a prewarm posts no ET_SHOW_REQUESTED ({sch_requests(got)})")

        # Switch to Schematic Editor on the prewarmed editor (shown to the toolkit, unmapped), by
        # an agent and for the person
        for client, expect in (("agent:check", "agent:check"), ("person:check", "")):
            with IpcSession(args.kicad_cli, api_socket=s.socket, timeout=60000, client_name=client) as c:
                c.request_raw(editor.RunAction(action="pcbnew.EditorControl.showEeschema"))
            got = sch_requests(collect(4))
            check(bool(got) and all(d == expect for d in got),
                  f"Switch to Schematic Editor run by {client} posts ET_SHOW_REQUESTED with detail "
                  f"'{expect}' ({got})")

    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
