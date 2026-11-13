#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Lazy library loading in `kicad --embedded`, against the full preload (BACKPLANE_KICAD_FULL_LIBRARY_PRELOAD).

With a real install's global library tables (--libs: a release's share/kicad), the same
scenario runs twice on a copy of pic_programmer, lazy then full, and the results must match:

- a board footprint naming a library in neither table: the board opens, no hang, the same
  library messages in both modes;
- a project fp-lib-table row overriding a global nickname: the project's library is the one
  used, at start and after the full load;
- saving right after the load: the same bytes in both modes (and so the same stock export);
- the footprint and symbol choosers opened at once: every library loaded afterwards, an unused
  library's footprint placeable;
- right after start, "Update PCB from Schematic" (one symbol now names a footprint in a library
  the board does not use) and "Update Footprints from Library", each applied from its dialog, then
  saved: the same .kicad_pcb in both modes, byte for byte once each (uuid ...) is masked (the
  items they create get fresh random KIIDs, which differ between any two runs, full or lazy);
- ERC and DRC with schematic parity (in-editor tasks): the same findings, as sets.

    backplane-embed-lazylibs-check.py <kicad> <kicad-cli> --libs <share/kicad> --out DIR
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import FAILURES, IpcSession, Session, check, viewable, wait_for  # noqa: E402

DEMO = Path(__file__).resolve().parents[1] / "demos/pic_programmer"
TABLES = ("sym-lib-table", "fp-lib-table", "design-block-lib-table")


def find_dialog(session, title):
    """The dialog's box (x, y, width, height) on screen, or None: of xdotool's matches (it matches
    _NET_WM_NAME too), the biggest viewable one."""
    disp = session.host.display
    found = subprocess.run(["xdotool", "search", "--onlyvisible", "--name", title], env=session.env,
                           text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout.split()
    best = None
    for wid in found:
        try:
            window = disp.create_resource_object("window", int(wid))
            g = window.get_geometry()
            origin = session.host.root.translate_coords(window, 0, 0)
        except Exception:
            continue
        if best is None or g.width * g.height > best[2] * best[3]:
            best = (origin.x, origin.y, g.width, g.height)
    return best


def click_from_corner(session, box, dx, dy):
    session.xdotool("mousemove", str(box[0] + box[2] - dx), str(box[1] + box[3] - dy), "click", "1")


def close_dialog(session, title, dx=150):
    """Close a dialog by its bottom-right button (Cancel / Close), found from the window manager's
    list: keys may go to the embedded frame, which keeps the keyboard.  Waits until it is gone, so
    later requests are not refused under its modal loop."""
    for _ in range(10):
        box = find_dialog(session, title)
        if box is None:
            return True
        print(f"  closing {title!r} {box[2]}x{box[3]} at {box[0]},{box[1]}", flush=True)
        click_from_corner(session, box, dx, 25)
        time.sleep(1.0)
    return False


def apply_dialog(session, api, editor, action, title, apply_dx, close_dx, shot):
    """Open a modal dialog with RunAction, press its apply button, then Close.  RunAction answers
    only once the dialog closes, so it goes on its own connection."""
    import pynng

    def fire():
        with pynng.Req0(dial=f"ipc://{session.socket}", block_on_dial=True, recv_timeout=600000) as c:
            req = api.envelope.ApiRequest()
            req.header.kicad_token = api.token
            req.message.Pack(editor.RunAction(action=action))
            c.send(req.SerializeToString())
            c.recv()

    runner = threading.Thread(target=fire, daemon=True)
    runner.start()
    if not wait_for(lambda: find_dialog(session, title), 180, 0.5):
        return False
    time.sleep(3)
    box = find_dialog(session, title)   # its dry run (Update PCB) and layout
    print(f"  applying {title!r} {box[2]}x{box[3]} at {box[0]},{box[1]}", flush=True)
    click_from_corner(session, box, apply_dx, 23)
    time.sleep(8)
    session.shot(shot)
    closed = close_dialog(session, title, close_dx)
    runner.join(60)
    return closed and not runner.is_alive()


def prepare(project_root, mode):
    """pic_programmer with a project override of a global nickname and a missing library."""
    target = project_root / f"pic_programmer-{mode}"
    shutil.copytree(DEMO, target)
    # Override: the project's own "Diode_THT" holds only OVERRIDE_ONLY
    override = target / "libs/override.pretty"
    override.mkdir(parents=True)
    source = next((target / "libs/pic_programmer_fp.pretty").glob("*.kicad_mod"))
    text = source.read_text()
    old_name = text.split('"', 2)[1]
    (override / "OVERRIDE_ONLY.kicad_mod").write_text(text.replace(f'"{old_name}"', '"OVERRIDE_ONLY"', 1))
    table = target / "fp-lib-table"
    t = table.read_text()
    t = t.replace('(uri "${KICAD10_FOOTPRINT_DIR}/Diode_THT.pretty")', '(uri "${KIPRJMOD}/libs/override.pretty")')
    table.write_text(t)
    # Missing: one footprint names a library no table has
    board = target / "pic_programmer.kicad_pcb"
    b = board.read_text()
    board.write_text(b.replace('(footprint "MountingHole:MountingHole_4.3mm_M4"',
                               '(footprint "NoSuchLibrary:MountingHole_4.3mm_M4"', 1))
    # Update PCB from Schematic: one symbol now names a footprint from a library the board does not use
    sheet = target / "pic_sockets.kicad_sch"
    old = '\n\t\t(property "Footprint" "Capacitor_THT:C_Disc_D5.1mm_W3.2mm_P5.00mm"'
    sch = sheet.read_text()
    assert old in sch
    sheet.write_text(sch.replace(old, '\n\t\t(property "Footprint" "Resistor_SMD:R_0603_1608Metric"', 1))
    return target


def run_mode(args, mode):
    out = args.out / mode
    out.mkdir(parents=True, exist_ok=True)
    work = out / "project"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir()
    project = prepare(work, mode)

    # A real install's global tables, not the empty ones the session would write
    original = Path.write_text

    def seeded(self, data, *a, **k):
        if self.name in TABLES and (args.libs / self.name).is_file() and "/config/kicad/" in str(self):
            data = (args.libs / self.name).read_text()
        return original(self, data, *a, **k)

    Path.write_text = seeded
    for var, sub in (("KICAD10_SYMBOL_DIR", "symbols"), ("KICAD10_FOOTPRINT_DIR", "footprints"),
                     ("KICAD10_3DMODEL_DIR", "3dmodels"), ("KICAD10_TEMPLATE_DIR", "template")):
        os.environ[var] = str(args.libs / sub)
    if mode == "full":
        os.environ["BACKPLANE_KICAD_FULL_LIBRARY_PRELOAD"] = "1"
    else:
        os.environ.pop("BACKPLANE_KICAD_FULL_LIBRARY_PRELOAD", None)

    result = {}
    try:
        with Session(args.kicad, out, project) as s, \
                IpcSession(args.kicad_cli, api_socket=s.socket, timeout=300000) as api:
            Path.write_text = original
            project = s.project_dir   # the session runs on its own copy: that is what KiCad saves
            embed = api.module("backplane.embed")
            editor = api.module("common.commands.editor_commands")
            base = api.module("common.types.base_types")
            libtypes = api.module("common.types.library_types")
            libcmd = api.module("common.commands.library_commands")
            boardcmd = api.module("board.board_commands")
            boardtasks = api.module("board.board_tasks")
            schtasks = api.module("schematic.schematic_tasks")
            tasks = api.module("common.tasks")
            projectcmd = api.module("common.commands.project_commands")
            w, h = s.host.size

            t = time.monotonic()
            reply = api.request(embed.ShowFrame(kind=embed.FK_PCB, x=0, y=0, width=w, height=h),
                                embed.ShowFrameResponse)
            opened = wait_for(lambda: viewable(s.host.display, reply.frame.window), 120)
            result["board_opened"] = opened
            result["board_open_s"] = round(time.monotonic() - t, 1)
            api.request(embed.ShowFrame(kind=embed.FK_SCH, prewarm=True, width=w, height=h),
                        embed.ShowFrameResponse)
            time.sleep(6)

            def docs(doctype):
                return api.request(editor.GetOpenDocuments(type=doctype), editor.GetOpenDocumentsResponse).documents

            def loaded(kind):
                reply = api.request(libcmd.GetLibraryStatuses(types=[kind], scope=libtypes.LTS_BOTH),
                                    libcmd.LibraryStatusResponse)
                return {e.entry.nickname: e.status for e in reply.libraries}

            fp_start = loaded(libtypes.LT_FOOTPRINT)
            sym_first = loaded(libtypes.LT_SYMBOL)
            result["sym_loaded_after_open"] = sum(1 for v in sym_first.values() if v == libtypes.LLS_LOADED)
            result["sym_libraries"] = len(sym_first)
            result["fp_loaded_at_start"] = sum(1 for v in fp_start.values() if v == libtypes.LLS_LOADED)
            result["fp_libraries"] = len(fp_start)

            pcb_doc = docs(base.DOCTYPE_PCB)[0]
            sch_docs = docs(base.DOCTYPE_SCHEMATIC)

            # Save right away: the bytes must not depend on how the libraries loaded
            api.request_raw(projectcmd.SaveDocument(document=pcb_doc))
            if sch_docs:
                api.request_raw(projectcmd.SaveDocument(document=sch_docs[0]))
            time.sleep(1)
            result["saved"] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                               for p in sorted(project.glob("*.kicad_*")) if p.suffix in (".kicad_pcb", ".kicad_sch")}

            # The project's override wins at start
            header = base.ItemHeader(document=pcb_doc)

            def place(nick, name):
                r = api.request_raw(boardcmd.PlaceFootprintFromLibrary(
                        header=header, lib_id=base.LibraryIdentifier(library_nickname=nick, entry_name=name),
                        position=base.Vector2(x_nm=10_000_000, y_nm=10_000_000)))
                return r.status.status == api.envelope.AS_OK

            result["override_only_at_start"] = place("Diode_THT", "OVERRIDE_ONLY")
            result["global_diode_at_start"] = place("Diode_THT", "D_DO-35_SOD27_P7.62mm_Horizontal")

            # Update PCB from Schematic and Update Footprints from Library, right after start, applied
            # from their dialogs, then saved
            result["update_pcb_applied"] = apply_dialog(s, api, editor, "common.Control.updatePcbFromSchematic",
                                                        "Update PCB from Schematic", 73, 177, f"{mode}-update-pcb")
            result["update_fp_applied"] = apply_dialog(s, api, editor, "pcbnew.GlobalEdit.updateFootprints",
                                                       "Update Footprints from Library", 60, 152,
                                                       f"{mode}-update-footprints")
            api.request_raw(projectcmd.SaveDocument(document=pcb_doc))
            time.sleep(1)
            board_file = project / "pic_programmer.kicad_pcb"
            saved = board_file.read_text()
            result["saved_after_updates_raw"] = hashlib.sha256(saved.encode()).hexdigest()
            result["saved_after_updates"] = hashlib.sha256(
                re.sub(r'\(uuid "[0-9a-fA-F-]+"\)', "(uuid U)", saved).encode()).hexdigest()
            result["uuids_after_updates"] = len(re.findall(r'\(uuid "', saved))
            shutil.copy(board_file, out / "after-updates.kicad_pcb")   # kept for a diff
            result["r0603_on_board"] = board_file.read_text().count('(footprint "Resistor_SMD:R_0603_1608Metric"')

            # The choosers (and with them every full-load path): open, wait, close
            # The person's own way, as for the symbol chooser: click the canvas, then 'A'
            s.xdotool("mousemove", str(w // 2), str(h // 2), "click", "1")
            time.sleep(0.5)
            s.xdotool("key", "a")
            time.sleep(3)
            s.shot(f"{mode}-footprint-chooser")
            close_dialog(s, "Footprint Chooser")
            s.xdotool("key", "Escape")
            def settled():
                states = loaded(libtypes.LT_FOOTPRINT)
                return states and not any(v == libtypes.LLS_LOADING for v in states.values())

            wait_for(settled, 180, 1)
            fp_after = loaded(libtypes.LT_FOOTPRINT)
            result["fp_loaded_after_chooser"] = sum(1 for v in fp_after.values() if v == libtypes.LLS_LOADED)
            result["unused_library_placeable"] = place("Resistor_SMD", "R_0603_1608Metric")
            # The symbol chooser, from the schematic editor
            sym_start = sum(1 for v in loaded(libtypes.LT_SYMBOL).values() if v == libtypes.LLS_LOADED)
            api.request(embed.HideFrame(kind=embed.FK_PCB), embed.HideFrameResponse)
            api.request(embed.ShowFrame(kind=embed.FK_SCH, x=0, y=0, width=w, height=h, focus=True),
                        embed.ShowFrameResponse)
            time.sleep(2)
            # RunAction reaches only the board's handler, so the person's own way: click, then 'A'
            s.xdotool("mousemove", str(w // 2), str(h // 2), "click", "1")
            time.sleep(0.5)
            s.xdotool("key", "a")
            time.sleep(3)
            s.shot(f"{mode}-symbol-chooser")
            close_dialog(s, "Choose Symbol")
            s.xdotool("key", "Escape")

            def sym_settled():
                states = loaded(libtypes.LT_SYMBOL)
                return states and not any(v == libtypes.LLS_LOADING for v in states.values())

            wait_for(sym_settled, 180, 1)
            result["sym_loaded_at_start"] = sym_start
            result["sym_loaded_after_chooser"] = sum(1 for v in loaded(libtypes.LT_SYMBOL).values()
                                                     if v == libtypes.LLS_LOADED)
            api.request(embed.HideFrame(kind=embed.FK_SCH), embed.HideFrameResponse)
            api.request(embed.ShowFrame(kind=embed.FK_PCB, x=0, y=0, width=w, height=h), embed.ShowFrameResponse)
            time.sleep(1)
            result["override_only_after_full"] = place("Diode_THT", "OVERRIDE_ONLY")
            result["global_diode_after_full"] = place("Diode_THT", "D_DO-35_SOD27_P7.62mm_Horizontal")

            # (Both modes placed the same footprints, so the checks below compare like with like)
            def run_task(start, result_req, result_type):
                token = api.request(start, tasks.StartTaskResponse).token
                wait_for(lambda: api.request(tasks.GetTaskStatus(token=token), tasks.TaskStatusResponse).state
                         != tasks.TS_RUNNING, 300, 1)
                return api.request(result_req(token=token), result_type)

            drc = run_task(boardtasks.StartDrcTask(board=pcb_doc, test_schematic_parity=True),
                           boardtasks.GetDrcTaskResult, boardtasks.DrcTaskResult)
            result["drc"] = sorted({(int(v.error_type), v.description) for v in drc.violations})
            if sch_docs:
                erc = run_task(schtasks.StartErcTask(document=sch_docs[0]),
                               schtasks.GetErcTaskResult, schtasks.ErcTaskResult)
                result["erc"] = sorted({(int(v.error_type), v.description) for v in erc.violations})

            log = (out / "kicad.log").read_text(errors="ignore") if (out / "kicad.log").is_file() else ""
            result["library_messages"] = sorted({line.strip() for line in log.splitlines()
                                                 if "NoSuchLibrary" in line or "library" in line.lower()})
    finally:
        Path.write_text = original
    (out / "result.json").write_text(json.dumps(result, indent=1, default=str))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--libs", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=Path("embed-lazylibs-out"))
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    lazy = run_mode(args, "lazy")
    full = run_mode(args, "full")
    print(json.dumps({k: v for k, v in lazy.items() if k not in ("drc", "erc", "saved")}, default=str))
    print(json.dumps({k: v for k, v in full.items() if k not in ("drc", "erc", "saved")}, default=str))

    check(lazy["board_opened"] and full["board_opened"],
          f"the board with a missing library opens in both modes ({lazy['board_open_s']} s lazy, "
          f"{full['board_open_s']} s full)")
    check(lazy["library_messages"] == full["library_messages"],
          "the same library messages lazy and full")
    check(lazy["fp_loaded_at_start"] < full["fp_loaded_at_start"],
          f"lazy loads fewer footprint libraries at start ({lazy['fp_loaded_at_start']} vs "
          f"{full['fp_loaded_at_start']} of {full['fp_libraries']})")
    check(lazy["override_only_at_start"] and not lazy["global_diode_at_start"],
          "the project's override is the library used at start")
    check(lazy["override_only_after_full"] and not lazy["global_diode_after_full"],
          "and after the full load")
    check(lazy["override_only_after_full"] == full["override_only_after_full"]
          and lazy["global_diode_after_full"] == full["global_diode_after_full"],
          "the override resolves the same way as with the full preload")
    check(lazy["fp_loaded_after_chooser"] == full["fp_loaded_after_chooser"],
          f"after the footprint chooser every library is loaded ({lazy['fp_loaded_after_chooser']} vs "
          f"{full['fp_loaded_after_chooser']})")
    check(lazy["sym_loaded_after_chooser"] == full["sym_loaded_after_chooser"],
          f"after the symbol chooser every symbol library is loaded ({lazy['sym_loaded_at_start']} at start lazy, "
          f"{lazy['sym_loaded_after_chooser']} after; full {full['sym_loaded_after_chooser']})")
    check(lazy["unused_library_placeable"] and full["unused_library_placeable"],
          "a footprint from a library the project does not use can be placed")
    check(lazy["saved"] == full["saved"], "saving right after the load writes the same bytes lazy and full")
    check(all(r[k] for r in (lazy, full) for k in ("update_pcb_applied", "update_fp_applied")),
          "Update PCB from Schematic and Update Footprints from Library applied and closed in both modes")
    check(lazy["r0603_on_board"] >= 1 and lazy["r0603_on_board"] == full["r0603_on_board"],
          f"the footprint from the unused library came in from the schematic ({lazy['r0603_on_board']} lazy, "
          f"{full['r0603_on_board']} full)")
    check(lazy["saved_after_updates"] == full["saved_after_updates"]
          and lazy["uuids_after_updates"] == full["uuids_after_updates"],
          f"after both updates the saved .kicad_pcb is identical lazy and full but for fresh KIIDs "
          f"({lazy['saved_after_updates'][:16]}, {lazy['uuids_after_updates']} uuids)")
    check(lazy["drc"] == full["drc"], f"DRC with parity: the same findings ({len(lazy['drc'])})")
    check(lazy.get("erc") == full.get("erc"), f"ERC: the same findings ({len(lazy.get('erc') or [])})")

    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
