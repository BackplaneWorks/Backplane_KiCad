#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bp.13 batch 8 checks for `kicad --embedded`: ClickMarker (click-marker).

- ClickMarker{FK_PCB, marker} posts exactly one ET_MARKER_CLICKED with that drc_marker (its id,
  type, items and place as GetMarkers gives them); the reply's event_seq is that event's seq.
- A real click on the same marker posts an event equal to ClickMarker's but for its seq.
- The same on FK_SCH for an ERC marker, with drc_marker.sheet_path set.
- A host marker by set and id posts ET_MARKER_CLICKED with marker{set, id}.
- An unknown id is AS_BAD_REQUEST "ClickMarker: no marker ..."; during a RunChecks, AS_BUSY.

Runs on a private Xvfb + i3 with a python-xlib stand-in host; screenshots to --out.
"""

import argparse
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import (FAILURES, IpcSession, Session, check,  # noqa: E402
                                          viewable, wait_for)


def canvas_centre(disp, root, frame_window):
    """Screen centre of the frame's biggest viewable descendant window (the drawing canvas)."""
    best = None

    def walk(window):
        nonlocal best
        try:
            children = window.query_tree().children
        except Exception:
            return
        for child in children:
            try:
                attrs = child.get_attributes()
                g = child.get_geometry()
            except Exception:
                continue
            if attrs.map_state == 2:
                area = g.width * g.height
                if best is None or area > best[0]:
                    o = root.translate_coords(child, 0, 0)
                    best = (area, o.x + g.width // 2, o.y + g.height // 2)
            walk(child)

    walk(disp.create_resource_object("window", frame_window))
    return (best[1], best[2]) if best else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--out", type=Path, default=Path("embed-click-marker-out"))
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / "demos/pic_programmer"

    with Session(args.kicad, args.out, source) as s, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=300000) as api:
        embed = api.module("backplane.embed")
        base = api.module("common.types.base_types")
        capabilities = api.module("backplane.capabilities")
        disp = s.host.display
        w, h = s.host.size
        since = [0]

        caps = api.request(capabilities.GetCapabilities(), capabilities.GetCapabilitiesResponse)
        check("click-marker" in caps.capabilities, "GetCapabilities lists click-marker")

        def events():
            reply = api.request(embed.NextEvents(since=since[0]), embed.NextEventsResponse)
            for e in reply.events:
                since[0] = max(since[0], e.seq)
            return list(reply.events)

        def clicked_events(seconds=3):
            got = []
            wait_for(lambda: got.extend(e for e in events() if e.type == embed.ET_MARKER_CLICKED) or False,
                     seconds, 0.2)
            return got

        def show(kind):
            for f in api.request(embed.GetFrames(), embed.GetFramesResponse).frames:
                if f.shown and f.kind != kind:
                    api.request(embed.HideFrame(window=f.window), embed.HideFrameResponse)
            r = api.request(embed.ShowFrame(kind=kind, x=0, y=0, width=w, height=h), embed.ShowFrameResponse)
            wait_for(lambda: viewable(disp, r.frame.window), 60)
            time.sleep(2)
            return r.frame

        def run_checks(kind):
            events()
            api.request(embed.RunChecks(kind=kind), embed.RunChecksResponse)
            wait_for(lambda: any(e.type == embed.ET_CHECKS_DONE for e in events()), 300, 0.2)
            return list(api.request(embed.GetMarkers(kind=kind), embed.GetMarkersResponse).markers)

        def same_marker(a, b):
            return (a.id.value == b.id.value and a.type == b.type and a.severity == b.severity
                    and a.message == b.message and [i.value for i in a.items] == [i.value for i in b.items]
                    and a.at_nm.x_nm == b.at_nm.x_nm and a.at_nm.y_nm == b.at_nm.y_nm
                    and a.sheet_path == b.sheet_path)

        def click_marker(kind, marker, zoom=False):
            events()
            reply = api.request_raw(embed.ClickMarker(kind=kind, marker=marker.id, zoom=zoom))
            got = clicked_events()
            return reply, got

        # ---- Board: ClickMarker against a real click on the same marker
        frame = show(embed.FK_PCB)
        markers = run_checks(embed.FK_PCB)
        check(bool(markers), f"DRC leaves markers to click ({len(markers)})")
        if markers:
            m = next((x for x in markers if x.type == "clearance"), markers[0])
            reply, got = click_marker(embed.FK_PCB, m)
            ok = reply.status.status == api.envelope.AS_OK
            seq = embed.ClickMarkerResponse.FromString(reply.message.value).event_seq if ok else 0
            check(ok and len(got) == 1 and same_marker(got[0].drc_marker, m) and got[0].seq == seq,
                  f"ClickMarker posts one ET_MARKER_CLICKED with the marker, seq {seq} "
                  f"({reply.status.error_message}; {[(e.seq, e.drc_marker.id.value) for e in got]})")
            via_api = got[0] if got else None

            api.request(embed.SelectItems(kind=embed.FK_PCB, at_nm=m.at_nm), embed.SelectItemsResponse)
            time.sleep(1)
            cx, cy = canvas_centre(disp, s.host.root, frame.window)
            events()
            s.xdotool("mousemove", str(cx), str(cy), "click", "1")
            real = clicked_events(5)
            check(bool(real) and via_api is not None and same_marker(real[0].drc_marker, via_api.drc_marker)
                  and real[0].frame.kind == via_api.frame.kind,
                  f"a real click on it posts the same event but for its seq "
                  f"({[e.drc_marker.id.value for e in real]})")

            # A hidden frame does the same and asks for no show
            sch_frame = show(embed.FK_SCH)
            reply, got = click_marker(embed.FK_PCB, m)
            check(reply.status.status == api.envelope.AS_OK and len(got) == 1
                  and not any(e.type == embed.ET_SHOW_REQUESTED for e in events()),
                  "ClickMarker on the hidden PCB editor posts its event and asks for no show")

        # Unknown id; AS_BUSY during a RunChecks
        bogus = base.KIID(value="00000000-0000-0000-0000-000000000042")
        reply = api.request_raw(embed.ClickMarker(kind=embed.FK_PCB, marker=bogus))
        check(reply.status.status == api.envelope.AS_BAD_REQUEST
              and reply.status.error_message.startswith("ClickMarker: no marker"),
              f"an unknown id is AS_BAD_REQUEST ({reply.status.error_message})")
        if markers:
            events()
            api.request(embed.RunChecks(kind=embed.FK_PCB), embed.RunChecksResponse)
            reply = api.request_raw(embed.ClickMarker(kind=embed.FK_PCB, marker=markers[0].id))
            check(reply.status.status == api.envelope.AS_BUSY,
                  f"ClickMarker during a RunChecks is AS_BUSY ({reply.status.status}, {reply.status.error_message})")
            wait_for(lambda: any(e.type == embed.ET_CHECKS_DONE for e in events()), 300, 0.2)

        # Host marker
        api.request(embed.SetHostMarkers(kind=embed.FK_PCB, set="review", markers=[
            embed.HostMarker(id="r1", title="Look here", severity=embed.MS_WARNING,
                             at_nm=base.Vector2(x_nm=120_000_000, y_nm=90_000_000))]), embed.SetHostMarkersResponse)
        events()
        reply = api.request_raw(embed.ClickMarker(kind=embed.FK_PCB, host=embed.MarkerRef(set="review", id="r1")))
        got = clicked_events()
        check(reply.status.status == api.envelope.AS_OK and len(got) == 1
              and got[0].marker.set == "review" and got[0].marker.id == "r1",
              f"a host marker by set and id posts marker{{review, r1}} ({[(e.marker.set, e.marker.id) for e in got]})")

        # ---- Schematic: an ERC marker
        show(embed.FK_SCH)
        erc = run_checks(embed.FK_SCH)
        check(bool(erc), f"ERC leaves markers to click ({len(erc)})")
        if erc:
            reply, got = click_marker(embed.FK_SCH, erc[0])
            check(reply.status.status == api.envelope.AS_OK and len(got) == 1
                  and same_marker(got[0].drc_marker, erc[0]) and got[0].drc_marker.sheet_path,
                  f"ClickMarker on an ERC marker posts it with its sheet path "
                  f"({reply.status.error_message}; {[e.drc_marker.sheet_path for e in got]})")
        s.shot("01-sch-after-click-marker")

    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
