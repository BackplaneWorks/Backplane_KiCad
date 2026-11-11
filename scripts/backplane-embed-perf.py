#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Frame-time measurements for `kicad --embedded` (BACKPLANE_KICAD_FRAMETRACE).

On a private Xvfb + i3 with a stand-in host: first opens of the schematic and 3D viewer (or
their prewarm), repeated editor switches, Ctrl+S and IPC saves on a modified board (time to
the file written and to the modified flag clearing), and a 200 ms Ping series after each save.
Writes <out>/frametrace.jsonl and <out>/summary.json and prints a summary: UI-thread loop
iterations over 16 ms per phase, the worst ones, and the measured latencies.

    backplane-embed-perf.py <kicad> <kicad-cli> --out DIR [--project DIR] [--prewarm]
"""

import argparse
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import IpcSession, Session, viewable, wait_for  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--out", type=Path, default=Path("embed-perf-out"))
    parser.add_argument("--project", type=Path,
                        default=Path(__file__).resolve().parents[1] / "demos/pic_programmer")
    parser.add_argument("--prewarm", action="store_true", help="prewarm SCH and 3D before switching")
    parser.add_argument("--xvfb", default=None)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    trace = (args.out / "frametrace.jsonl").resolve()
    trace.unlink(missing_ok=True)
    os.environ["BACKPLANE_KICAD_FRAMETRACE"] = str(trace)

    phases = []        # (name, t_start_ms, t_end_ms)
    numbers = {}

    def phase(name, start):
        phases.append((name, start, time.time() * 1000))

    with Session(args.kicad, args.out, args.project, xvfb=args.xvfb) as s, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=120000) as api:
        e = api.module("backplane.embed")
        editor = api.module("common.commands.editor_commands")
        project = api.module("common.commands.project_commands")
        base = api.module("common.types.base_types")
        board = api.module("board.board_types")
        common = api.module("common.commands.base_commands")
        w, h = s.host.size

        def timed(command, response):
            t = time.perf_counter()
            r = api.request(command, response)
            return r, (time.perf_counter() - t) * 1000

        def frames():
            return {f.kind: f for f in api.request(e.GetFrames(), e.GetFramesResponse).frames}

        t0 = time.time() * 1000
        r, ms = timed(e.ShowFrame(kind=e.FK_PCB, x=0, y=0, width=w, height=h), e.ShowFrameResponse)
        numbers["first_show_pcb_ms"] = round(ms, 1)
        wait_for(lambda: viewable(s.host.display, r.frame.window), 30)
        time.sleep(3)
        phase("open_pcb", t0)

        if args.prewarm and hasattr(e.ShowFrame(), "prewarm"):
            t0 = time.time() * 1000
            for kind, name in ((e.FK_SCH, "sch"), (e.FK_PCB_3D, "3d")):
                _, ms = timed(e.ShowFrame(kind=kind, prewarm=True), e.ShowFrameResponse)
                numbers[f"prewarm_{name}_ms"] = round(ms, 1)
            time.sleep(4)
            phase("prewarm", t0)

        # First opens (or first shows after prewarm)
        for kind, name in ((e.FK_SCH, "sch"), (e.FK_PCB_3D, "3d")):
            t0 = time.time() * 1000
            api.request(e.HideFrame(kind=e.FK_PCB), e.HideFrameResponse)
            r, ms = timed(e.ShowFrame(kind=kind, x=0, y=0, width=w, height=h), e.ShowFrameResponse)
            numbers[f"first_show_{name}_ms"] = round(ms, 1)
            time.sleep(3)
            api.request(e.HideFrame(kind=kind), e.HideFrameResponse)
            api.request(e.ShowFrame(kind=e.FK_PCB, x=0, y=0, width=w, height=h), e.ShowFrameResponse)
            time.sleep(1.5)
            phase(f"first_{name}", t0)

        # Repeated switches through IPC (hide the shown one, show the next)
        t0 = time.time() * 1000
        switch_ms = []
        order = [e.FK_SCH, e.FK_PCB_3D, e.FK_PCB] * 4
        shown = e.FK_PCB
        for kind in order:
            t = time.perf_counter()
            api.request(e.HideFrame(kind=shown), e.HideFrameResponse)
            api.request(e.ShowFrame(kind=kind, x=0, y=0, width=w, height=h), e.ShowFrameResponse)
            switch_ms.append((time.perf_counter() - t) * 1000)
            shown = kind
            time.sleep(0.6)
        numbers["switch_ipc_ms"] = {"median": round(statistics.median(switch_ms), 1),
                                   "max": round(max(switch_ms), 1)}
        phase("switch_ipc", t0)

        # Repeated switches by the host alone (XUnmapWindow/XMapWindow, no IPC)
        t0 = time.time() * 1000
        f = frames()
        windows = [f[k].window for k in (e.FK_SCH, e.FK_PCB_3D, e.FK_PCB)]
        local_ms = []
        disp = s.host.display
        current = f[e.FK_PCB].window
        for wid in windows * 3:
            t = time.perf_counter()
            disp.create_resource_object("window", current).unmap()
            disp.create_resource_object("window", wid).map()
            disp.sync()
            local_ms.append((time.perf_counter() - t) * 1000)
            current = wid
            time.sleep(0.6)
        f = frames()
        numbers["switch_local_ms"] = {"median": round(statistics.median(local_ms), 1), "max": round(max(local_ms), 1)}
        numbers["switch_local_state_ok"] = bool(f[e.FK_PCB].shown and viewable(disp, f[e.FK_PCB].window)
                                               and not viewable(disp, f[e.FK_SCH].window))
        phase("switch_local", t0)

        # Saves: modify, then Ctrl+S (person) and SaveDocument (IPC)
        board_file = next(s.project_dir.glob("*.kicad_pcb"))
        doc = api.request(editor.GetOpenDocuments(type=base.DOCTYPE_PCB), editor.GetOpenDocumentsResponse).documents[0]
        header = base.ItemHeader(document=doc)

        def modify():
            tx = api.request(editor.BeginCommit(header=header), editor.BeginCommitResponse)
            create = editor.CreateItems(header=header)
            create.items.add().Pack(board.Track(id=base.KIID(value=str(uuid.uuid4())),
                                               start=base.Vector2(x_nm=60000000, y_nm=60000000),
                                               end=base.Vector2(x_nm=70000000, y_nm=60000000),
                                               width=base.Distance(value_nm=250000), layer=board.BL_F_Cu))
            api.request(create, editor.CreateItemsResponse)
            api.request(editor.EndCommit(header=header, id=tx.id, action=editor.CMA_COMMIT, message="perf"),
                        editor.EndCommitResponse)
            wait_for(lambda: frames()[e.FK_PCB].modified, 5, 0.05)

        def after_save(t_start, label, mtime0):
            written = clean = None
            pings = []
            deadline = time.perf_counter() + 6
            while time.perf_counter() < deadline:
                now = (time.perf_counter() - t_start) * 1000
                if written is None and board_file.stat().st_mtime_ns != mtime0:
                    written = now
                if clean is None and not frames()[e.FK_PCB].modified:
                    clean = now
                t = time.perf_counter()
                api.request_raw(common.Ping())
                pings.append((time.perf_counter() - t) * 1000)
                if written is not None and clean is not None and now > 3000:
                    break
                time.sleep(0.2 if clean is not None else 0.02)
            numbers[label] = {"written_ms": round(written, 1) if written else None,
                              "clean_ms": round(clean, 1) if clean else None,
                              "ping_median_ms": round(statistics.median(pings), 1),
                              "ping_max_ms": round(max(pings), 1)}

        t0 = time.time() * 1000
        for i in range(3):
            modify()
            s.host.pump(0.1)
            s.xdotool("mousemove", str(w // 2), str(h // 2), "click", "1")
            time.sleep(0.5)
            mtime0 = board_file.stat().st_mtime_ns
            t = time.perf_counter()
            s.xdotool("key", "ctrl+s")
            after_save(t, f"save_ctrl_s_{i}", mtime0)
        phase("save_ctrl_s", t0)

        t0 = time.time() * 1000
        for i in range(3):
            modify()
            mtime0 = board_file.stat().st_mtime_ns
            t = time.perf_counter()
            api.request_raw(project.SaveDocument(document=doc))
            after_save(t, f"save_ipc_{i}", mtime0)
        phase("save_ipc", t0)

    # Summarise the trace per phase
    loops, spans = [], []
    for line in trace.read_text().splitlines():
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            continue
        (loops if rec.get("kind") == "loop" else spans if rec.get("kind") == "span" else []).append(rec)

    summary = {"numbers": numbers, "phases": {}}
    for name, start, end in phases:
        inside = [l for l in loops if start <= l["t"] <= end]
        over = [l for l in inside if l["ms"] > 16]
        worst = sorted(inside, key=lambda l: -l["ms"])[:5]
        summary["phases"][name] = {"loops_over_8ms": len(inside), "loops_over_16ms": len(over),
                                   "worst": [{"ms": round(l["ms"], 1), "ran": l["ran"][:8]} for l in worst]}
    redraws = [sp["ms"] for sp in spans if sp["what"].startswith("gal_redraw")]
    if redraws:
        summary["gal_redraw_ms"] = {"count": len(redraws), "median": round(statistics.median(redraws), 2),
                                    "max": round(max(redraws), 2)}
    for key in ("save:pcb", "save:pcb:write", "save:pcb:backup", "save:pcb:history", "save:pcb:git", "save:pcb:title", "embed:show", "embed:hide", "embed:baseline",
                "embed:poll", "ipc:ShowFrame", "ipc:HideFrame", "ipc:SaveDocument"):
        values = [sp["ms"] for sp in spans if sp["what"] == key or sp["what"].startswith(key + "@")]
        if values:
            summary.setdefault("spans_ms", {})[key] = {"count": len(values), "median": round(statistics.median(values), 2),
                                                      "max": round(max(values), 2)}
    (args.out / "summary.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
