#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bp.13 check for `kicad --embedded`: a show request says who caused it.

- ShowFrame{FK_SCH, prewarm} opens the schematic editor hidden and posts no ET_SHOW_REQUESTED.
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

        # The host's prewarm: hidden, no request
        api.request(embed.ShowFrame(kind=embed.FK_SCH, width=804, height=896, prewarm=True), embed.ShowFrameResponse)
        got = collect(15)
        check(embed.FK_SCH in frames() and not frames()[embed.FK_SCH].shown,
              "a prewarm opens the schematic editor hidden")
        check(not sch_requests(got), f"a prewarm posts no ET_SHOW_REQUESTED ({sch_requests(got)})")

        # Switch to Schematic Editor, by an agent and for the person
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
