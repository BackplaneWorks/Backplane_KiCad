#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the SketchRoute IPC command on a headless server.

Routes a 0.8 mm pitch CSP to an FPC connector through IPC: inside a transaction that is then
dropped, from the headless selection, and along a guide path.  Checks the created items, the
error handling, and that the saved board has no unrouted connections and no DRC violations on
the new copper.
"""

import argparse
import json
from pathlib import Path
import subprocess

from google.protobuf.empty_pb2 import Empty

from backplane_ipc_test_support import IpcSession


FIXTURE = "qa/data/pcbnew/sketch_router_csp"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad_cli", type=Path)
    args = parser.parse_args()

    with IpcSession(args.kicad_cli) as api:
        project = api.module("common.commands.project_commands")
        editor = api.module("common.commands.editor_commands")
        sketch_route_commands = api.module("backplane.sketch_route")
        board = api.module("board.board_types")
        types = api.module("common.types.base_types")
        enums = api.module("common.types.enums")

        design = api.copy_project(FIXTURE, "sketch-router")
        board_path = design / "csp_demo.kicad_pcb"
        document = api.request(project.OpenDocument(type=types.DOCTYPE_PCB, path=str(board_path)),
                               project.OpenDocumentResponse).document
        header = types.ItemHeader(document=document)

        def items(kind):
            result = api.request(editor.GetItems(header=header, types=[kind]), editor.GetItemsResponse)
            assert result.status == types.IRS_OK, result
            return result.items

        def copper_ids():
            ids = set()
            for kind, message in ((enums.KOT_PCB_TRACE, board.Track), (enums.KOT_PCB_ARC, board.Arc),
                                  (enums.KOT_PCB_VIA, board.Via)):
                for packed in items(kind):
                    item = message()
                    assert packed.Unpack(item)
                    ids.add(item.id.value)
            return ids

        footprints = {}
        for packed in items(enums.KOT_PCB_FOOTPRINT):
            footprint = board.FootprintInstance()
            assert packed.Unpack(footprint)
            footprints[footprint.reference_field.text.text.text] = footprint
        assert {"U1", "J1"} <= footprints.keys(), footprints.keys()
        parts = [footprints["U1"].id, footprints["J1"].id]
        assert not copper_ids(), "fixture must start unrouted"

        def sketch_route(**fields):
            return api.request(sketch_route_commands.SketchRoute(board=document, **fields),
                               sketch_route_commands.SketchRouteResponse)

        # Inside a transaction the routes join the client's commit, and dropping it discards them
        transaction = api.request(editor.BeginCommit(header=header), editor.BeginCommitResponse)
        routed = sketch_route(items=parts)
        assert routed.connections == 30 and routed.routed == 30 and routed.failed == 0, routed
        assert routed.vias > 0, "the inner CSP balls need vias"
        assert routed.length_nm > 0 and not routed.timed_out, routed
        assert len(routed.created_items) > routed.vias, routed
        api.request(editor.EndCommit(header=header, id=transaction.id, action=editor.CMA_DROP),
                    editor.EndCommitResponse)
        assert not copper_ids(), "dropping the commit must discard the routes"
        print(f"PASS SketchRoute in a dropped commit: {routed.routed}/{routed.connections} "
              f"routed, {routed.vias} vias in {routed.elapsed_ms:.0f} ms", flush=True)

        # min_track_width only narrows a connection that has no way at its width: none here
        transaction = api.request(editor.BeginCommit(header=header), editor.BeginCommitResponse)
        necked = sketch_route(items=parts, min_track_width=types.Distance(value_nm=90000))
        assert necked.routed == 30 and necked.narrowed == 0, necked
        api.request(editor.EndCommit(header=header, id=transaction.id, action=editor.CMA_DROP),
                    editor.EndCommitResponse)
        assert not copper_ids()
        out_of_range = api.request_raw(sketch_route_commands.SketchRoute(
            board=document, items=parts, min_track_width=types.Distance(value_nm=-1)))
        assert out_of_range.status.status == api.envelope.AS_BAD_REQUEST, out_of_range
        print("PASS SketchRoute min_track_width: nothing narrowed where the width fits; "
              "a negative width is refused", flush=True)

        # Bad requests leave the board alone
        missing = api.request_raw(sketch_route_commands.SketchRoute(
            board=document, items=[types.KIID(value="00000000-0000-0000-0000-000000000001")]))
        assert missing.status.status == api.envelope.AS_BAD_REQUEST, missing
        silk = api.request_raw(sketch_route_commands.SketchRoute(board=document, items=parts,
                                                    preferred_layer=board.BL_F_SilkS))
        assert silk.status.status == api.envelope.AS_BAD_REQUEST, silk
        nothing = api.request_raw(sketch_route_commands.SketchRoute(board=document))
        assert nothing.status.status == api.envelope.AS_BAD_REQUEST, nothing
        assert not copper_ids()
        print("PASS SketchRoute rejects unknown items, non-copper layers and no items headless",
              flush=True)

        # Guided: the right-hand balls to the right-hand pins, along a path around the right
        # side of the part, without vias and in 90 degree mode
        pads = {}
        for packed in items(enums.KOT_PCB_PAD):
            pad = board.Pad()
            assert packed.Unpack(pad)
            pads.setdefault(pad.net.name, []).append(pad.id)
        guided_nets = [f"D{n}" for n in range(19, 24)]
        guide = types.PolyLine()
        for x_mm, y_mm in ((23.5, 12.0), (26.0, 14.5), (26.0, 19.0), (24.5, 21.0)):
            guide.nodes.add().point.CopyFrom(types.Vector2(x_nm=int(x_mm * 1e6), y_nm=int(y_mm * 1e6)))
        transaction = api.request(editor.BeginCommit(header=header), editor.BeginCommitResponse)
        guided = sketch_route(items=[pad for net in guided_nets for pad in pads[net]], guide=guide,
                              preferred_layer=board.BL_F_Cu, disallow_vias=True,
                              corner_mode=sketch_route_commands.SRCM_MITERED_90, time_limit_ms=20000)
        api.request(editor.EndCommit(header=header, id=transaction.id, action=editor.CMA_COMMIT,
                                     message="Guided sketch route"), editor.EndCommitResponse)
        assert guided.connections == len(guided_nets), guided
        assert guided.routed == guided.connections and guided.vias == 0, guided
        created = copper_ids()
        assert created == {item.value for item in guided.created_items}
        for packed in items(enums.KOT_PCB_TRACE):
            track = board.Track()
            assert packed.Unpack(track)
            assert track.layer == board.BL_F_Cu, track
            assert track.start.x_nm == track.end.x_nm or track.start.y_nm == track.end.y_nm, track
        print(f"PASS guided SketchRoute in a kept commit: {guided.routed} connections on F.Cu, "
              "90 degree, no vias",
              flush=True)

        # The rest, committed straight away (master has no headless selection, so name the parts)
        rest = sketch_route(items=parts)
        assert rest.connections == 30 - len(guided_nets), rest
        assert rest.routed == rest.connections, rest
        created |= {item.value for item in rest.created_items}
        assert copper_ids() == created
        print(f"PASS SketchRoute outside a commit: {rest.routed}/{rest.connections} routed, "
              f"{rest.vias} vias", flush=True)

        api.request(project.SaveDocument(document=document), Empty)

        # The saved board is fully connected and the new copper passes DRC
        report = api.root / "drc.json"
        subprocess.run([str(args.kicad_cli), "pcb", "drc", "--format", "json", "--severity-error",
                        "--output", str(report), str(board_path)],
                       check=True, env=api.env, stdout=subprocess.DEVNULL)
        drc = json.loads(report.read_text())
        assert not drc.get("unconnected_items"), drc.get("unconnected_items")
        routed_violations = [violation for violation in drc.get("violations", [])
                             if any(item.get("uuid") in created for item in violation.get("items", []))]
        assert not routed_violations, json.dumps(routed_violations, indent=1)[:4000]
        print(f"PASS saved board: fully routed, no DRC errors on {len(created)} new items",
              flush=True)


if __name__ == "__main__":
    main()
