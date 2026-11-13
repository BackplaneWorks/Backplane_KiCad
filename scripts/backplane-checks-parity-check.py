#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bp.13: the editor's checks find what kicad-cli finds.

For each project, `kicad-cli pcb drc --severity-all --schematic-parity` and `kicad-cli sch erc
--severity-all` (JSON) against `kicad --embedded` RunChecks{FK_PCB, parity} and RunChecks{FK_SCH}
then GetMarkers, on the same saved files.  A finding is keyed by its type and its items' ids; an
excluded one by its type and title (kicad-cli's JSON drops an excluded violation's items).  Every
finding on one side only is a failure, printed with its items as the CLI describes them.

Usage: backplane-checks-parity-check.py KICAD KICAD_CLI [--project DIR ...] [--out DIR]
(default projects: demos/pic_programmer, demos/complex_hierarchy, demos/video).
Runs on a private Xvfb + i3 with a python-xlib stand-in host.
"""

import argparse
from collections import Counter
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import (FAILURES, IpcSession, Session, check,  # noqa: E402
                                          viewable, wait_for)

DEFAULT = ["demos/pic_programmer", "demos/complex_hierarchy", "demos/video"]


def cli_findings(cli, project, work):
    """{'pcb': Counter, 'sch': Counter} of keys, and key -> description for printing."""
    pro = next(project.glob("*.kicad_pro"))
    pcb, sch = pro.with_suffix(".kicad_pcb"), pro.with_suffix(".kicad_sch")
    out = {"pcb": Counter(), "sch": Counter()}
    text = {}

    def add(side, v):
        items = [i.get("uuid", "") for i in v.get("items", []) if i.get("uuid")]
        if v.get("excluded") and not items:
            key = (v["type"], "excluded", v.get("description", ""))
        else:
            key = (v["type"], tuple(sorted(items)))
        out[side][key] += 1
        text[key] = f'{v.get("description", "")}: ' + "; ".join(i.get("description", "") for i in v.get("items", []))

    if pcb.exists():
        j = work / f"{pro.stem}-drc.json"
        subprocess.run([str(cli), "pcb", "drc", "--severity-all", "--schematic-parity", "--format", "json",
                        "-o", str(j), str(pcb)], capture_output=True, timeout=1800)
        d = json.loads(j.read_text())
        for group in ("violations", "unconnected_items", "schematic_parity"):
            for v in d.get(group, []):
                add("pcb", v)
    if sch.exists():
        j = work / f"{pro.stem}-erc.json"
        subprocess.run([str(cli), "sch", "erc", "--severity-all", "--format", "json", "-o", str(j), str(sch)],
                       capture_output=True, timeout=1800)
        d = json.loads(j.read_text())
        for sheet in d.get("sheets", []):
            for v in sheet.get("violations", []):
                add("sch", v)
    return out, text


def editor_findings(kicad, cli, project, out_dir):
    out = {"pcb": Counter(), "sch": Counter()}
    text = {}
    with Session(kicad, out_dir, project) as s, \
            IpcSession(cli, api_socket=s.socket, timeout=600000) as api:
        embed = api.module("backplane.embed")
        w, h = s.host.size
        since = [0]

        def events():
            reply = api.request(embed.NextEvents(since=since[0]), embed.NextEventsResponse)
            for e in reply.events:
                since[0] = max(since[0], e.seq)
            return list(reply.events)

        for kind, side, kw in ((embed.FK_PCB, "pcb", {"parity": True}), (embed.FK_SCH, "sch", {})):
            for f in api.request(embed.GetFrames(), embed.GetFramesResponse).frames:
                if f.shown and f.kind != kind:
                    api.request(embed.HideFrame(window=f.window), embed.HideFrameResponse)
            r = api.request(embed.ShowFrame(kind=kind, x=0, y=0, width=w, height=h), embed.ShowFrameResponse)
            wait_for(lambda: viewable(s.host.display, r.frame.window), 120)
            events()
            api.request(embed.RunChecks(kind=kind, **kw), embed.RunChecksResponse)
            seen = []
            wait_for(lambda: seen.extend(events()) or any(e.type == embed.ET_CHECKS_DONE for e in seen), 1800, 0.5)
            done = next((e.checks for e in seen if e.type == embed.ET_CHECKS_DONE), None)
            check(done is not None and not done.error, f"{project.name} {side}: RunChecks finishes "
                  f"({done.error if done else 'no ET_CHECKS_DONE'})")
            for m in api.request(embed.GetMarkers(kind=kind), embed.GetMarkersResponse).markers:
                items = [i.id.value for i in m.items_detail] or [i.value for i in m.items]
                title = m.message.split("\n")[0]
                if m.excluded:
                    key = (m.type, "excluded", title)
                else:
                    key = (m.type, tuple(sorted(items)))
                out[side][key] += 1
                text[key] = f"{title}: " + "; ".join(i.description for i in m.items_detail)
    return out, text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--project", action="append", type=Path)
    parser.add_argument("--out", type=Path, default=Path("checks-parity-out"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    projects = args.project or [root / p for p in DEFAULT]
    args.out.mkdir(parents=True, exist_ok=True)

    for project in projects:
        work = Path(tempfile.mkdtemp(prefix="checks-parity-"))
        copy = work / project.name
        shutil.copytree(project, copy, symlinks=True,
                        ignore=shutil.ignore_patterns(".venv", ".git", "node_modules", "*-backups"))
        cli, cli_text = cli_findings(args.kicad_cli, copy, work)
        ed, ed_text = editor_findings(args.kicad, args.kicad_cli, copy, args.out / project.name)
        for side in ("pcb", "sch"):
            only_ed = ed[side] - cli[side]
            only_cli = cli[side] - ed[side]
            label = f"{project.name} {'DRC' if side == 'pcb' else 'ERC'}"
            check(not only_ed and not only_cli,
                  f"{label}: the editor and kicad-cli agree ({sum(cli[side].values())} CLI, "
                  f"{sum(ed[side].values())} editor; {sum(only_ed.values())} editor-only, "
                  f"{sum(only_cli.values())} CLI-only)")
            for key, n in list(only_ed.items())[:40]:
                print(f"  editor only x{n}: {key[0]}  {ed_text.get(key, '')[:240]}")
            for key, n in list(only_cli.items())[:40]:
                print(f"  CLI only x{n}: {key[0]}  {cli_text.get(key, '')[:240]}")
        shutil.rmtree(work, ignore_errors=True)

    print(f"\n{len(FAILURES)} failure(s)")
    if not FAILURES:
        print("ALL PASSED")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
