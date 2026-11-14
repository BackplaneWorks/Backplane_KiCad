#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""An ERC exclusion stored for a sheet that is not in the schematic is kept and reported.

On a copy of demos/complex_hierarchy, one ERC exclusion is added to the .kicad_pro under a sheet that
does not exist (/<root>/deadbeef-0000-4000-8000-000000000001, item deadbeef-...00aa, comment
"stale: its sheet was removed"), as lane E's repro does on Collar. Required:

- kicad-cli sch erc --severity-all --severity-exclusions: the root sheet ("/") has one excluded row with
  no items, the stored comment and unresolved_sheet_path "/<root>/deadbeef-...0001"; an unpatched
  copy has none;
- kicad --embedded, RunChecks{FK_SCH} (StartErcTask without it), GetMarkers FK_SCH: the same row
  (excluded, severity "exclusion", no items, the comment, unresolved_sheet_path, sheet_path the root);
- the .kicad_pro keeps its bytes through the CLI run and through open, ERC and an unmodified save.

    backplane-stale-exclusion-check.py <kicad> <kicad-cli> [--out DIR]
"""

import argparse
import hashlib
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

DEMO = Path(__file__).resolve().parents[1] / "demos/complex_hierarchy"
DEAD_SHEET = "deadbeef-0000-4000-8000-000000000001"
DEAD_ITEM = "deadbeef-0000-4000-8000-0000000000aa"
COMMENT = "stale: its sheet was removed"


def staged(stale):
    tmp = Path(tempfile.mkdtemp(prefix="stale-exclusion-")) / DEMO.name
    shutil.copytree(DEMO, tmp)
    root = re.search(r'\(uuid "([0-9a-f-]+)"\)', (tmp / "complex_hierarchy.kicad_sch").read_text()).group(1)
    if stale:
        pro = tmp / "complex_hierarchy.kicad_pro"
        d = json.loads(pro.read_text())
        path = {"path": [{"value": root}, {"value": DEAD_SHEET}]}
        d.setdefault("erc", {}).setdefault("erc_exclusions", []).append(
            {"marker": {"error_type": "ERCET_PIN_NOT_DRIVEN", "items": [{"value": DEAD_ITEM}],
                        "main_item_sheet_path": path, "sheet_specific_path": json.loads(json.dumps(path)),
                        "position": {"x_nm": "100000000", "y_nm": "50000000"}},
             "comment": COMMENT})
        pro.write_text(json.dumps(d, indent=2) + "\n")
    return tmp, f"/{root}/{DEAD_SHEET}"


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def cli_rows(cli, folder, env=None):
    out = folder / "erc.json"
    subprocess.run([str(cli), "sch", "erc", "--format", "json", "--severity-all", "--severity-exclusions",
                    "-o", str(out), str(folder / "complex_hierarchy.kicad_sch")], cwd=folder, env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=600)
    rep = json.loads(out.read_text())
    out.unlink()
    rows = []
    for sheet in rep.get("sheets", []):
        for v in sheet.get("violations", []):
            if v.get("unresolved_sheet_path"):
                rows.append((sheet.get("path"), v))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--out", type=Path, default=Path("stale-exclusion-out"))
    args = parser.parse_args()

    plain, _ = staged(False)
    check(not cli_rows(args.kicad_cli, plain), "kicad-cli: without a stale exclusion, no unresolved row")

    folder, stale_path = staged(True)
    pro = folder / "complex_hierarchy.kicad_pro"
    before = sha(pro)
    rows = cli_rows(args.kicad_cli, folder)
    check(len(rows) == 1, f"kicad-cli: one unresolved row ({len(rows)})")
    if rows:
        sheet, v = rows[0]
        check(sheet == "/" and v.get("excluded") and v.get("items") == [] and v.get("comment") == COMMENT
              and v.get("unresolved_sheet_path") == stale_path and v.get("type") and v.get("description"),
              f"kicad-cli: on '/', excluded, no items, the comment and the stored path ({sheet}, {v})")
    check(sha(pro) == before, "kicad-cli: the .kicad_pro keeps its bytes")

    with Session(args.kicad, args.out, folder) as s, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=600000) as api:
        embed = api.module("backplane.embed")
        base = api.module("common.types.base_types")
        ed = api.module("common.commands.editor_commands")
        proj = api.module("common.commands.project_commands")
        tasks = api.module("common.tasks")
        w, h = s.host.size
        since = [0]
        session_pro = s.project_dir / "complex_hierarchy.kicad_pro"
        session_before = sha(session_pro)

        def events():
            r = api.request(embed.NextEvents(since=since[0]), embed.NextEventsResponse)
            for e in r.events:
                since[0] = max(since[0], e.seq)
            return list(r.events)

        fr = api.request(embed.ShowFrame(kind=embed.FK_SCH, x=0, y=0, width=w, height=h), embed.ShowFrameResponse).frame
        wait_for(lambda: viewable(s.host.display, fr.window), 180)
        time.sleep(3)
        events()
        r = api.request_raw(embed.RunChecks(kind=embed.FK_SCH)) if hasattr(embed, "RunChecks") else None
        if r is not None and r.status.status == api.envelope.AS_OK:
            wait_for(lambda: any(e.type == embed.ET_CHECKS_DONE for e in events()), 600, 0.5)
        else:
            st = api.module("schematic.schematic_tasks")
            doc = api.request(ed.GetOpenDocuments(type=base.DOCTYPE_SCHEMATIC), ed.GetOpenDocumentsResponse).documents[0]
            tok = api.request(st.StartErcTask(document=doc), tasks.StartTaskResponse).token
            wait_for(lambda: api.request(tasks.GetTaskStatus(token=tok), tasks.TaskStatusResponse).state
                     != tasks.TS_RUNNING, 600, 1)
        markers = api.request(embed.GetMarkers(kind=embed.FK_SCH), embed.GetMarkersResponse).markers
        stale = [m for m in markers if getattr(m, "unresolved_sheet_path", "")]
        check(len(stale) == 1, f"GetMarkers: one marker for the stale exclusion ({len(stale)})")
        if stale:
            m = stale[0]
            check(m.excluded and m.severity == "exclusion" and not m.items and m.comment == COMMENT
                  and m.unresolved_sheet_path == stale_path and m.type and m.message,
                  f"GetMarkers: excluded, no items, the comment and the stored path ({m.type}, {m.sheet_path}, "
                  f"{m.unresolved_sheet_path})")
            if rows:
                check(m.type == rows[0][1]["type"], f"GetMarkers and kicad-cli name the same type ({m.type})")

        doc = api.request(ed.GetOpenDocuments(type=base.DOCTYPE_SCHEMATIC), ed.GetOpenDocumentsResponse).documents[0]
        r = api.request_raw(proj.SaveDocument(document=doc))
        check(r.status.status == api.envelope.AS_OK, f"SaveDocument answers AS_OK ({r.status.error_message[:60]!r})")
        time.sleep(1)
        check(sha(session_pro) == session_before, "open, ERC and an unmodified save keep the .kicad_pro's bytes")

    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
