#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bp.13 (plan-view-api): in plan view a layer change keeps the plan tool taking clicks, and the API is
served in plan view.

With plan view on (its key, Ctrl+Shift+R), the active layer becomes In1.Cu by:
  plus      the + key from F.Cu (Switch to Next Layer)
  plus2     + twice from B.Cu (B.Cu -> F.Cu -> In1.Cu)
  minus     the - key from In2.Cu
  api-layer SetActiveLayer over the API, which must be served in plan view (not AS_BUSY)
then a real left click on corridor c-1 (In1.Cu) where plan view draws it, and W (Wider): GetPlan
must show c-1 wider, i.e. the click was the plan tool's.
  api-toggle  RunAction pcbnew.Plan.togglePlanView in plan view must be served and leave plan
            view: there a click on the overlay and W leave c-1 alone (the selection tool's
            canvas); the same RunAction enters plan view again, where click + W widen it.

Before plan-view-api (bp.13 batch 9/10) every gated request in plan view was AS_BUSY, so the
api-layer and api-toggle cases fail there.  The board is a copy of --project with its plan made
by backplane-plan-demo.py --strip (the fork's demo plan), as Backplane's real-use harness loads it.

    backplane-embed-plan-layer-click-check.py <kicad> <kicad-cli> --project DIR [--out DIR]

Runs on a private Xvfb + i3 with a python-xlib stand-in host; screenshots to --out.
"""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import (FAILURES, IpcSession, Session, check,  # noqa: E402
                                          viewable, wait_for)


def canvas_rect(disp, root, frame_window):
    """Screen rectangle (x, y, w, h) of the frame's biggest viewable descendant (the canvas)."""
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
                    best = (area, o.x, o.y, g.width, g.height)
            walk(child)

    walk(disp.create_resource_object("window", frame_window))
    return best[1:] if best else None


def edge_box(board_file):
    """The Edge.Cuts bounding box in mm from the board file's gr_line/gr_rect/gr_arc points."""
    xs, ys = [], []
    text = board_file.read_text()
    for m in re.finditer(r"\((gr_line|gr_rect|gr_arc)\s(.*?)\(layer \"Edge\.Cuts\"\)", text, re.S):
        for x, y in re.findall(r"\((?:start|end|mid) ([-\d.]+) ([-\d.]+)\)", m.group(2)):
            xs.append(float(x))
            ys.append(float(y))
    return min(xs), min(ys), max(xs), max(ys)


def make_project(source, work):
    """A copy of SOURCE with the fork's demo plan (stripped board) beside its board."""
    project = work / source.name
    shutil.copytree(source, project)
    board = next(project.glob("*.kicad_pcb"))
    demo = Path(__file__).resolve().parent / "backplane-plan-demo.py"
    out = work / "plan-out"
    subprocess.run([sys.executable, "-I", str(demo), str(board), "--strip", "--out", str(out)], check=True,
                   stdout=subprocess.DEVNULL)
    for f in out.iterdir():
        shutil.copy(f, project / f.name)
    return project


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--project", type=Path, required=True,
                        help="Collar (collar_main): a 4-layer board whose demo plan has c-1 on In1.Cu")
    parser.add_argument("--out", type=Path, default=Path("embed-plan-layer-click-out"))
    parser.add_argument("--cases", default="plus,plus2,minus,api-layer,api-toggle")
    args = parser.parse_args()

    work = Path(tempfile.mkdtemp(prefix="backplane-planlayer-"))
    try:
        project = make_project(args.project.resolve(), work)
        run(args, project)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if FAILURES:
        print(f"FAIL: {len(FAILURES)} check(s) failed")
        sys.exit(1)
    print("PASS")


def run(args, project):
    from google.protobuf import empty_pb2

    with Session(args.kicad, args.out, project) as s, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=300000) as api:
        embed = api.module("backplane.embed")
        base = api.module("common.types.base_types")
        editor = api.module("common.commands.editor_commands")
        board_cmds = api.module("board.board_commands")
        board_types = api.module("board.board_types")
        plan_pb = api.module("backplane.plan")
        disp = s.host.display
        w, h = s.host.size

        for f in api.request(embed.GetFrames(), embed.GetFramesResponse).frames:
            if f.shown and f.kind != embed.FK_PCB:
                api.request(embed.HideFrame(window=f.window), embed.HideFrameResponse)
        frame = api.request(embed.ShowFrame(kind=embed.FK_PCB, x=0, y=0, width=w, height=h),
                            embed.ShowFrameResponse).frame
        check(wait_for(lambda: viewable(disp, frame.window), 180), "the board is shown")
        time.sleep(3)
        doc = api.request(editor.GetOpenDocuments(type=base.DOCTYPE_PCB),
                          editor.GetOpenDocumentsResponse).documents[0]

        def get_plan():
            return json.loads(api.request(plan_pb.GetPlan(board=doc), plan_pb.PlanResponse).json or "{}")

        def corridor(cid):
            return next(c for c in get_plan()["corridors"] if c["id"] == cid)

        def active():
            return api.request(board_cmds.GetActiveLayer(board=doc), board_cmds.BoardLayerResponse).layer

        def answered(request, response):
            """Send REQUEST: '' when served, else the refusal (AS_BUSY ...)."""
            try:
                api.request(request, response)
                return ""
            except AssertionError as e:
                return str(e).split("\n")[0]

        def set_active(layer):
            refused = answered(board_cmds.SetActiveLayer(board=doc, layer=layer), empty_pb2.Empty)
            time.sleep(0.5)
            return refused

        def run_action(name):
            refused = answered(editor.RunAction(action=name), editor.RunActionResponse)
            time.sleep(1.0)
            return refused

        def empty_click():
            """A click on an empty corner of the canvas: gives it the keyboard, selects nothing."""
            cx, cy, cw, ch = canvas_rect(disp, s.host.root, frame.window)
            s.xdotool("mousemove", str(cx + 20), str(cy + ch - 20), "click", "1")
            time.sleep(0.5)

        def key(*words):
            s.xdotool("key", "--clearmodifiers", *words)
            time.sleep(0.5)

        state = {"plan": False}

        def plan_view_key():
            """Plan view on or off with its key (Ctrl+Shift+R), the canvas holding the keyboard."""
            empty_click()
            key("ctrl+shift+r")
            time.sleep(1.0)
            state["plan"] = not state["plan"]

        plan = get_plan()
        c1 = next((c for c in plan.get("corridors", []) if c["layer"] == "In1.Cu"), None)
        check(c1 is not None, "the demo plan has a corridor on In1.Cu")
        if c1 is None:
            return
        cid = c1["id"]
        (ax, ay), (bx, by) = c1["path"][0], c1["path"][1]
        mid = ((ax + bx) / 2, (ay + by) / 2)
        x0, y0, x1, y1 = edge_box(next(project.glob("*.kicad_pcb")))

        def to_screen(pt):
            """PLAN_TOOL::fitBesideOverview's mapping: board fitted left of the overview column."""
            cx, cy, cw, ch = canvas_rect(disp, s.host.root, frame.window)
            bw, bh = x1 - x0, y1 - y0
            px_per_mm = min((cw - 150.0) / bw, (ch - 116.0) / bh)
            centre_x = (x0 + x1) / 2 + 60.0 / px_per_mm
            centre_y = (y0 + y1) / 2 - 46.0 / px_per_mm
            return (round(cx + cw / 2 + (pt[0] - centre_x) * px_per_mm),
                    round(cy + ch / 2 + (pt[1] - centre_y) * px_per_mm))

        def click_and_widen(tag):
            """A real click on the corridor where plan view draws it, then W: (before, after)."""
            before = corridor(cid)["width"]
            x, y = to_screen(mid)
            s.xdotool("mousemove", str(x), str(y))
            time.sleep(0.3)
            s.xdotool("mousemove", str(x + 1), str(y))
            time.sleep(0.3)
            s.xdotool("click", "1")
            time.sleep(0.8)
            s.shot(f"{tag}-clicked")
            key("w")
            time.sleep(0.5)
            after = corridor(cid)["width"]
            print(f"[{tag}] {cid} clicked at {x},{y}: width {before} -> {after}", flush=True)
            return before, after

        def start(layer):
            """Board view, LAYER active, then plan view on."""
            check(not set_active(layer), f"SetActiveLayer is served in the board view")
            plan_view_key()

        def finish():
            """Back to the board view with no plan selection (an empty click clears it)."""
            if state["plan"]:
                plan_view_key()

        for case in args.cases.split(","):
            if case in ("plus", "plus2", "minus"):
                # a layer change by the keyboard in plan view (KiCad's next/previous layer)
                start({"plus": board_types.BL_F_Cu, "plus2": board_types.BL_B_Cu,
                       "minus": board_types.BL_In2_Cu}[case])
                empty_click()
                for _ in range(2 if case == "plus2" else 1):
                    key("minus" if case == "minus" else "plus")
                layer = active()
                check(layer == board_types.BL_In1_Cu, f"[{case}] the active layer is In1.Cu (got {layer})")
                before, after = click_and_widen(case)
                check(after > before, f"[{case}] after the layer change in plan view, a click on {cid} "
                                      f"and W widen it ({before} -> {after})")
                finish()

            elif case == "api-layer":
                # the layer changed over the API while plan view is on
                start(board_types.BL_F_Cu)
                refused = set_active(board_types.BL_In1_Cu)
                check(not refused, f"[{case}] SetActiveLayer is served in plan view ({refused or 'ok'})")
                layer = active()
                check(layer == board_types.BL_In1_Cu, f"[{case}] the active layer is In1.Cu (got {layer})")
                before, after = click_and_widen(case)
                check(after > before, f"[{case}] after SetActiveLayer in plan view, a click on {cid} "
                                      f"and W widen it ({before} -> {after})")
                finish()

            elif case == "api-toggle":
                # plan view left and entered again over the API (RunAction togglePlanView)
                start(board_types.BL_In1_Cu)
                refused = run_action("pcbnew.Plan.togglePlanView")
                check(not refused, f"[{case}] RunAction togglePlanView is served in plan view "
                                   f"({refused or 'ok'})")
                if not refused:
                    state["plan"] = False
                    # the board view with the plan shown over it: a click there is not the plan's
                    before, after = click_and_widen(case + "-board-view")
                    check(after == before, f"[{case}] in the board view a click on the plan overlay and W "
                                           f"leave {cid} alone ({before} -> {after})")
                    key("Escape")
                    refused = run_action("pcbnew.Plan.togglePlanView")
                    check(not refused, f"[{case}] RunAction togglePlanView enters plan view ({refused or 'ok'})")
                    if not refused:
                        state["plan"] = True
                        before, after = click_and_widen(case)
                        check(after > before, f"[{case}] back in plan view a click on {cid} and W widen it "
                                              f"({before} -> {after})")
                finish()

            else:
                check(False, f"unknown case {case}")


if __name__ == "__main__":
    main()
