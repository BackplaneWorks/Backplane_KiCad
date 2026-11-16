#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bp.13 batch 12 (plan-select): a row pressed in Backplane's Plan panel selects in plan view.

Cases, on a copy of --project with the fork's demo plan (backplane-plan-demo.py --strip):
  items-zoom   in plan view, c-1 selected by a real click: embed SelectItems {nets of c-1, zoom} and
               then ShowFrame (what the hub sends for a row) keep plan view on (GetPlan
               in_plan_view), keep the plan selection, and W still widens c-1 (the plan tool runs).
               In the board view the same SelectItems still selects the board's items.
  select       in plan view on F.Cu: SelectPlanItems {c-1, zoom} answers selected [c-1]; plan view
               stays on, the active layer is In1.Cu, and W widens c-1 (read back through GetPlan)
               while c-2 keeps its width.
  unknown      SelectPlanItems {c-999} answers AS_BAD_REQUEST; {c-1, c-999} answers missing [c-999].
  from-board   in the board view: SelectPlanItems {c-1, zoom} enters plan view, and W widens c-1.

Before plan-select (bp.13 batch 11) SelectItems in plan view ends it, and SelectPlanItems is not
served, so every case fails there.

    backplane-embed-plan-select-check.py <kicad> <kicad-cli> --project DIR [--out DIR]

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
    parser.add_argument("--out", type=Path, default=Path("embed-plan-select-out"))
    parser.add_argument("--cases", default="items-zoom,select,unknown,from-board")
    args = parser.parse_args()

    work = Path(tempfile.mkdtemp(prefix="backplane-planselect-"))
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

        def show():
            return api.request(embed.ShowFrame(kind=embed.FK_PCB, x=0, y=0, width=w, height=h),
                               embed.ShowFrameResponse).frame

        frame = show()
        check(wait_for(lambda: viewable(disp, frame.window), 180), "the board is shown")
        time.sleep(3)
        doc = api.request(editor.GetOpenDocuments(type=base.DOCTYPE_PCB),
                          editor.GetOpenDocumentsResponse).documents[0]

        def get_plan():
            return json.loads(api.request(plan_pb.GetPlan(board=doc), plan_pb.PlanResponse).json or "{}")

        def in_plan_view():
            return api.request(plan_pb.GetPlan(board=doc), plan_pb.PlanResponse).in_plan_view

        def mode(tag, want):
            got = in_plan_view()
            check(got == want, f"[{tag}] GetPlan in_plan_view is {want} (got {got})")

        def width(cid):
            return next(c for c in get_plan()["corridors"] if c["id"] == cid)["width"]

        def active():
            return api.request(board_cmds.GetActiveLayer(board=doc), board_cmds.BoardLayerResponse).layer

        def raw(request):
            """REQUEST's reply envelope (status and message) without raising."""
            return api.request_raw(request)

        def select_plan(ids, zoom=True):
            """SelectPlanItems: (status code, SelectPlanItemsResponse or None, error text)."""
            if not hasattr(plan_pb, "SelectPlanItems"):
                return None, None, "no SelectPlanItems in the proto"
            reply = raw(plan_pb.SelectPlanItems(board=doc, ids=ids, zoom=zoom))
            if reply.status.status != api.envelope.AS_OK:
                return reply.status.status, None, reply.status.error_message
            out = plan_pb.SelectPlanItemsResponse()
            reply.message.Unpack(out)
            time.sleep(0.8)
            return reply.status.status, out, ""

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
            empty_click()
            key("ctrl+shift+r")
            time.sleep(1.0)
            state["plan"] = not state["plan"]

        def set_active(layer):
            api.request(board_cmds.SetActiveLayer(board=doc, layer=layer), empty_pb2.Empty)
            time.sleep(0.5)

        def finish(tag):
            """Back to the board view, whatever mode the case left."""
            if in_plan_view():
                plan_view_key()
            state["plan"] = False
            key("Escape")
            mode(tag + " back in the board view", False)

        plan = get_plan()
        corridors = plan.get("corridors", [])
        c1 = next((c for c in corridors if c["id"] == "c-1"), None)
        check(c1 is not None and c1["layer"] == "In1.Cu", "the demo plan has c-1 on In1.Cu")
        if c1 is None:
            return
        cid = c1["id"]
        other = next((c["id"] for c in corridors if c["id"] != cid), None)
        nets = next((g.get("nets", []) for g in plan.get("groups", []) if g["id"] == c1.get("group")), [])
        refs = list(c1.get("from", [])) + list(c1.get("to", []))
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

        def click_corridor(tag):
            x, y = to_screen(mid)
            s.xdotool("mousemove", str(x), str(y))
            time.sleep(0.3)
            s.xdotool("mousemove", str(x + 1), str(y))
            time.sleep(0.3)
            s.xdotool("click", "1")
            time.sleep(0.8)
            s.shot(f"{tag}-clicked")

        def widen(tag, cids):
            """W on the canvas (it holds the keyboard): {id: (before, after)}."""
            before = {c: width(c) for c in cids if c}
            key("w")
            time.sleep(0.5)
            s.shot(f"{tag}-w")
            out = {c: (before[c], width(c)) for c in before}
            print(f"[{tag}] W: {out}", flush=True)
            return out

        def selection_count():
            sel = api.request(editor.GetSelection(header=base.ItemHeader(document=doc)),
                              editor.SelectionResponse)
            return len(sel.items)

        for case in args.cases.split(","):
            if case == "items-zoom":
                # board view first: SelectItems still selects the board's items there
                set_active(board_types.BL_In1_Cu)
                mode(case + " board view", False)
                api.request(embed.SelectItems(kind=embed.FK_PCB, references=refs, nets=nets, zoom=True),
                            embed.SelectItemsResponse)
                time.sleep(0.8)
                try:
                    n = selection_count()
                    check(n > 0, f"[{case}] in the board view SelectItems selects the board's items ({n})")
                except Exception as e:  # noqa: BLE001
                    check(False, f"[{case}] GetSelection: {e}")
                key("Escape")
                empty_click()
                # plan view, c-1 selected by the person's click
                plan_view_key()
                mode(case + " plan view on by its key", True)
                click_corridor(case)
                api.request(embed.SelectItems(kind=embed.FK_PCB, references=refs, nets=nets, zoom=True),
                            embed.SelectItemsResponse)
                time.sleep(0.5)
                show()
                time.sleep(1.0)
                s.shot(f"{case}-selected")
                mode(case + " after SelectItems {zoom} and ShowFrame", True)
                got = widen(case, [cid, other])
                check(got[cid][1] > got[cid][0], f"[{case}] after SelectItems the plan selection holds and "
                                                 f"W widens {cid} {got[cid]}")
                finish(case)

            elif case == "select":
                set_active(board_types.BL_F_Cu)
                plan_view_key()
                mode(case + " plan view on by its key", True)
                empty_click()   # the canvas holds the keyboard; nothing selected
                code, reply, why = select_plan([cid])
                check(reply is not None, f"[{case}] SelectPlanItems {{{cid}, zoom}} is served ({why or code})")
                s.shot(f"{case}-selected")
                if reply is not None:
                    check(list(reply.selected) == [cid] and not reply.missing,
                          f"[{case}] selected {list(reply.selected)}, missing {list(reply.missing)}")
                    check(reply.in_plan_view and reply.active_layer == "In1.Cu",
                          f"[{case}] reply in_plan_view {reply.in_plan_view}, active_layer {reply.active_layer}")
                mode(case + " after SelectPlanItems", True)
                layer = active()
                check(layer == board_types.BL_In1_Cu, f"[{case}] the active layer is In1.Cu (got {layer})")
                got = widen(case, [cid, other])
                check(got[cid][1] > got[cid][0], f"[{case}] W widens {cid} {got[cid]}")
                if other:
                    check(got[other][1] == got[other][0], f"[{case}] W leaves {other} alone {got[other]}")
                mode(case + " after W", True)
                finish(case)

            elif case == "unknown":
                code, reply, why = select_plan(["c-999"])
                check(reply is None and code == api.envelope.AS_BAD_REQUEST,
                      f"[{case}] SelectPlanItems {{c-999}} answers AS_BAD_REQUEST (got {code} {why})")
                mode(case + " a refused SelectPlanItems leaves the board view", False)
                code, reply, why = select_plan([cid, "c-999"], zoom=False)
                check(reply is not None and list(reply.missing) == ["c-999"]
                      and list(reply.selected) == [cid],
                      f"[{case}] {{c-1, c-999}}: missing [c-999], selected [c-1] "
                      f"({(list(reply.selected), list(reply.missing)) if reply else (code, why)})")
                finish(case)

            elif case == "from-board":
                set_active(board_types.BL_F_Cu)
                mode(case + " board view", False)
                empty_click()
                code, reply, why = select_plan([cid])
                check(reply is not None, f"[{case}] SelectPlanItems from the board view is served "
                                         f"({why or code})")
                s.shot(f"{case}-selected")
                mode(case + " SelectPlanItems enters plan view", True)
                if reply is not None:
                    state["plan"] = True
                check(active() == board_types.BL_In1_Cu, f"[{case}] the active layer is In1.Cu")
                got = widen(case, [cid])
                check(got[cid][1] > got[cid][0], f"[{case}] W widens {cid} {got[cid]}")
                finish(case)

            else:
                check(False, f"unknown case {case}")


if __name__ == "__main__":
    main()
