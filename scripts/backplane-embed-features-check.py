#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bp.9 checks for `kicad --embedded`: live theme, cross-probe both ways, reload on disk change,
and robustness (frame state across hide/show, popups and dialogs never escaping, re-attach).

Runs on a private Xvfb + i3 with a python-xlib stand-in host; saves screenshots to --out.
"""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import (FAILURES, IpcSession, Session, check, luminance,  # noqa: E402
                                          mean_difference, parent_of, region_colour, viewable,
                                          wait_for)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--out", type=Path, default=Path("embed-features-out"))
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / "demos/pic_programmer"

    with Session(args.kicad, args.out, source, extra_args=["--theme", "dark"]) as s, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=120000) as api:
        embed = api.module("backplane.embed")
        editor = api.module("common.commands.editor_commands")
        base = api.module("common.types.base_types")
        board = api.module("board.board_types")
        enums = api.module("common.types.enums")
        disp, host = s.host.display, s.host
        top = host.top.id
        frames0 = api.request(embed.GetFrames(), embed.GetFramesResponse)
        holder = frames0.holder
        api.request(embed.SetChrome(), embed.SetChromeResponse)
        events_since = [0]

        def events():
            reply = api.request(embed.NextEvents(since=events_since[0]), embed.NextEventsResponse)
            events_since[0] = reply.next
            return list(reply.events)

        def frames():
            return {f.kind: f for f in api.request(embed.GetFrames(), embed.GetFramesResponse).frames}

        def show(kind, label=None, wait=2.5):
            for other in frames().values():
                if other.shown and other.kind != kind:
                    api.request(embed.HideFrame(window=other.window), embed.HideFrameResponse)
            w, h = host.size
            reply = api.request(embed.ShowFrame(kind=kind, x=0, y=0, width=w, height=h),
                                embed.ShowFrameResponse)
            wait_for(lambda: viewable(disp, reply.frame.window), 30)
            time.sleep(wait)
            host.pump(0.1)
            return reply.frame.window, (s.shot(label) if label else None)

        def document(doctype):
            return api.request(editor.GetOpenDocuments(type=doctype), editor.GetOpenDocumentsResponse).documents[0]

        def selection_count(doctype):
            reply = api.request(editor.GetSelection(header=base.ItemHeader(document=document(doctype))),
                                editor.SelectionResponse)
            return len(reply.items)

        w, h = host.size
        canvas = f"{w // 4}x{h // 4}+{w * 3 // 8}+{h * 3 // 8}"
        menubar = f"{w // 3}x14+{w // 2}+2"

        # ---------------- 1. Theme ----------------
        check(frames0.theme == embed.TM_DARK, "--theme dark is reported by GetFrames")
        _, dark_pcb = show(embed.FK_PCB, "01-pcb-dark", 4)
        _, dark_sch = show(embed.FK_SCH, "02-sch-dark", 3)
        sch_dark = region_colour(dark_sch, canvas)
        bar_dark = region_colour(dark_sch, menubar)
        check(luminance(sch_dark) < 60, f"schematic canvas starts dark {sch_dark}")
        check(luminance(bar_dark) < 110, f"GTK widgets start dark (menubar {bar_dark})")

        reply = api.request(embed.SetTheme(mode=embed.TM_LIGHT), embed.SetThemeResponse)
        check(reply.mode == embed.TM_LIGHT and not reply.gtk_dark and reply.icon_theme == "light",
              f"SetTheme LIGHT answers the applied state ({reply.color_theme}, icons {reply.icon_theme})")
        time.sleep(2)
        light_sch = s.shot("03-sch-light")
        sch_light = region_colour(light_sch, canvas)
        bar_light = region_colour(light_sch, menubar)
        check(luminance(sch_light) > 200, f"schematic canvas turns light live {sch_light}")
        check(luminance(bar_light) > 180, f"GTK widgets turn light live (menubar {bar_light})")
        _, light_pcb = show(embed.FK_PCB, "04-pcb-light", 3)
        check(region_colour(light_pcb, canvas) != region_colour(dark_pcb, canvas),
              "PCB canvas background follows the theme")
        _, light_3d = show(embed.FK_PCB_3D, "05-3d-light", 6)
        reply = api.request(embed.SetTheme(mode=embed.TM_DARK), embed.SetThemeResponse)
        check(reply.mode == embed.TM_DARK and reply.gtk_dark and reply.icon_theme == "dark",
              "SetTheme DARK answers the applied state")
        time.sleep(4)
        dark_3d = s.shot("06-3d-dark")
        corner = f"{w // 6}x{h // 10}+{w // 20}+{h // 8}"
        check(luminance(region_colour(dark_3d, corner)) < luminance(region_colour(light_3d, corner)) - 30,
              "3D viewer background follows the theme")
        check(any(e.type == embed.ET_THEME_CHANGED and e.detail == "dark" for e in events()),
              "ET_THEME_CHANGED reported")
        _, sch_again = show(embed.FK_SCH, "07-sch-dark-again", 3)
        check(luminance(region_colour(sch_again, canvas)) < 60, "and back to dark, live")

        # ---------------- 2. Cross-probe ----------------
        api.request(embed.HideFrame(kind=embed.FK_SCH), embed.HideFrameResponse)
        show(embed.FK_PCB, wait=2)
        events()
        result = api.request(embed.SelectItems(kind=embed.FK_PCB, references=["U1", "NOPE9"],
                                               nets=["GND", "/NO_SUCH_NET"], pins=["U1.1", "U1.99"],
                                               zoom=True), embed.SelectItemsResponse)
        check(list(result.found.references) == ["U1"] and list(result.not_found.references) == ["NOPE9"]
              and list(result.found.nets) == ["GND"] and list(result.not_found.nets) == ["/NO_SUCH_NET"]
              and list(result.found.pins) == ["U1.1"] and list(result.not_found.pins) == ["U1.99"],
              f"SelectItems on the PCB reports found / not found ({result})")
        time.sleep(2)
        pcb_selected = s.shot("08-pcb-select-u1-gnd")
        check(selection_count(base.DOCTYPE_PCB) >= 1, "the PCB selection holds U1 (its pad is part of it)")
        check(selection_count(base.DOCTYPE_SCHEMATIC) >= 1,
              "KiCad's own cross-probe selected U1 in the hidden schematic (KIWAY)")
        check(not any(e.type == embed.ET_SELECTION_CHANGED for e in events()),
              "a host selection is not echoed as ET_SELECTION_CHANGED")

        result = api.request(embed.SelectItems(kind=embed.FK_SCH, references=["U2"], pins=["U2.1"],
                                               nets=["VCC"], zoom=True), embed.SelectItemsResponse)
        check(list(result.found.references) == ["U2"] and list(result.found.pins) == ["U2.1"]
              and list(result.found.nets) == ["VCC"], f"SelectItems on the hidden schematic ({result})")
        check(selection_count(base.DOCTYPE_PCB) >= 1, "schematic → board cross-probe with the schematic hidden")
        _, sch_selected = show(embed.FK_SCH, "09-sch-select-u2", 3)
        missing = api.request_raw(embed.SelectItems(kind=embed.FK_FOOTPRINT_EDITOR, references=["U1"]))
        check(missing.status.status == api.envelope.AS_UNHANDLED, "a frame kind without a handler is AS_UNHANDLED")

        # The person selects in the board editor.
        show(embed.FK_PCB, wait=2)
        api.request(embed.SelectItems(kind=embed.FK_PCB, references=["U1"], zoom=True), embed.SelectItemsResponse)
        time.sleep(1.5)
        events()
        s.xdotool("mousemove", str(w // 2), str(h // 2), "click", "1")
        time.sleep(0.5)
        s.xdotool("key", "Escape")
        time.sleep(0.5)
        s.xdotool("mousemove", str(w // 2), str(h // 2), "click", "1")
        got = []
        wait_for(lambda: got.extend(e for e in events() if e.type == embed.ET_SELECTION_CHANGED) or
                 any(e.selection.references or e.selection.pins or e.selection.nets for e in got), 5)
        person = [e for e in got if e.selection.references or e.selection.pins or e.selection.nets]
        check(bool(person) and person[-1].frame.kind == embed.FK_PCB,
              f"ET_SELECTION_CHANGED for the person's click ({[str(e.selection) for e in person][-1:]})")
        check(any(not (e.selection.references or e.selection.pins or e.selection.nets) for e in got),
              "Escape clearing the selection is reported as an empty selection")

        # ---------------- 3. Reload on disk change ----------------
        board_file = s.project_dir / "pic_programmer.kicad_pcb"
        api.request(embed.SelectItems(kind=embed.FK_PCB, references=["U1"], zoom=True), embed.SelectItemsResponse)
        time.sleep(1.5)
        before = s.shot("10-pcb-before-reload")
        events()

        def add_text(path, label, partial=False):
            text = path.read_text()
            item = (f'\t(gr_text "{label}"\n\t\t(at 50 50 0)\n\t\t(layer "F.SilkS")\n'
                    f'\t\t(uuid "{uuid.uuid4()}")\n\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1.5 1.5)\n'
                    f'\t\t\t\t(thickness 0.3)\n\t\t\t)\n\t\t)\n\t)\n')
            body = text.rstrip()
            assert body.endswith(")")
            new = body[:-1] + item + ")\n"
            if partial:
                # A slow writer: half the file, a pause, then the rest (not atomic).
                with open(path, "w") as handle:
                    handle.write(new[: len(new) // 2])
                    handle.flush()
                    time.sleep(0.25)
                    handle.write(new[len(new) // 2:])
            else:
                temporary = path.with_suffix(".tmp-write")
                temporary.write_text(new)
                os.replace(temporary, path)   # atomic rename-over

        def board_has_text(label):
            items = api.request(editor.GetItems(header=base.ItemHeader(document=document(base.DOCTYPE_PCB)),
                                                types=[enums.KOT_PCB_TEXT]), editor.GetItemsResponse)
            for packed in items.items:
                text = board.BoardText()
                if packed.Unpack(text) and text.text.text == label:
                    return True
            return False

        def wait_event(kind, seconds=8):
            found = []
            wait_for(lambda: found.extend(e for e in events() if e.type == kind) or bool(found), seconds, 0.2)
            return found

        add_text(board_file, "EXTERNAL-ATOMIC")
        reloaded = wait_event(embed.ET_DOCUMENT_RELOADED)
        time.sleep(2)
        after = s.shot("11-pcb-after-reload")
        check(bool(reloaded) and board_has_text("EXTERNAL-ATOMIC"),
              "an atomic rename-over reloads the clean board (ET_DOCUMENT_RELOADED)")
        check(selection_count(base.DOCTYPE_PCB) >= 1, "the selection survives the reload (by UUID)")
        check(mean_difference(before, after, canvas) < 0.08,
              f"the view (zoom/pan) survives the reload (MAE {mean_difference(before, after, canvas):.3f})")

        add_text(board_file, "EXTERNAL-PARTIAL", partial=True)
        reloaded = wait_event(embed.ET_DOCUMENT_RELOADED)
        check(len(reloaded) == 1 and board_has_text("EXTERNAL-PARTIAL"),
              f"a slow partial write reloads once, complete ({len(reloaded)} reloads)")

        project = api.module("common.commands.project_commands")
        saved = api.request_raw(project.SaveDocument(document=document(base.DOCTYPE_PCB)))
        check(saved.status.status == api.envelope.AS_OK, f"SaveDocument over IPC ({saved.status.error_message})")
        check(not wait_event(embed.ET_DOCUMENT_RELOADED, 3), "KiCad's own save is not reloaded")

        # Unsaved edits: never clobbered.
        header = base.ItemHeader(document=document(base.DOCTYPE_PCB))
        transaction = api.request(editor.BeginCommit(header=header), editor.BeginCommitResponse)
        create = editor.CreateItems(header=header)
        create.items.add().Pack(board.Track(id=base.KIID(value=str(uuid.uuid4())),
                                            start=base.Vector2(x_nm=60000000, y_nm=60000000),
                                            end=base.Vector2(x_nm=70000000, y_nm=60000000),
                                            width=base.Distance(value_nm=250000), layer=board.BL_F_Cu))
        api.request(create, editor.CreateItemsResponse)
        api.request(editor.EndCommit(header=header, id=transaction.id, action=editor.CMA_COMMIT,
                                     message="person's edit"), editor.EndCommitResponse)
        wait_for(lambda: frames()[embed.FK_PCB].modified, 5)
        add_text(board_file, "EXTERNAL-CONFLICT")
        conflict = wait_event(embed.ET_RELOAD_CONFLICT)
        time.sleep(1)
        s.shot("12-pcb-conflict-infobar")
        check(bool(conflict) and frames()[embed.FK_PCB].modified and not board_has_text("EXTERNAL-CONFLICT"),
              "an external change over unsaved edits is a conflict, nothing reloaded")
        result = api.request(embed.ReloadDocument(kind=embed.FK_PCB, mode=embed.RM_IF_CLEAN),
                             embed.ReloadDocumentResponse)
        check(result.result == embed.RR_CONFLICT, "ReloadDocument IF_CLEAN refuses over unsaved edits")
        result = api.request(embed.ResolveConflict(kind=embed.FK_PCB, choice=embed.CC_KEEP),
                             embed.ResolveConflictResponse)
        check(result.result == embed.RR_UNCHANGED and frames()[embed.FK_PCB].modified,
              "ResolveConflict KEEP keeps the edits")
        def track_count():
            return len(api.request(editor.GetItems(header=header, types=[enums.KOT_PCB_TRACE]),
                                   editor.GetItemsResponse).items)

        with_edit = track_count()
        api.request(editor.RunAction(action="common.Interactive.undo"), editor.RunActionResponse)
        time.sleep(1)
        undone = track_count()
        api.request(editor.RunAction(action="common.Interactive.redo"), editor.RunActionResponse)
        time.sleep(1)
        check(undone == with_edit - 1 and track_count() == with_edit,
              f"the undo stack was kept: undo/redo of the person's edit ({with_edit}, {undone})")
        wait_for(lambda: frames()[embed.FK_PCB].modified, 5)
        add_text(board_file, "EXTERNAL-RELOAD")
        wait_event(embed.ET_RELOAD_CONFLICT)
        result = api.request(embed.ResolveConflict(kind=embed.FK_PCB, choice=embed.CC_RELOAD),
                             embed.ResolveConflictResponse)
        check(result.result == embed.RR_RELOADED and not frames()[embed.FK_PCB].modified
              and board_has_text("EXTERNAL-RELOAD"), "ResolveConflict RELOAD takes the file from disk")
        result = api.request(embed.ReloadDocument(path=str(board_file), mode=embed.RM_IF_CLEAN),
                             embed.ReloadDocumentResponse)
        check(result.result == embed.RR_UNCHANGED, "ReloadDocument on an unchanged file is RR_UNCHANGED")

        # A sub-sheet of the schematic, while the person is on it.
        sheet_file = s.project_dir / "pic_sockets.kicad_sch"
        w_, h_ = host.size
        api.request(embed.ShowFrame(kind=embed.FK_SCH, document=str(sheet_file), x=0, y=0, width=w_, height=h_),
                    embed.ShowFrameResponse)
        time.sleep(2)
        title_before = frames()[embed.FK_SCH].title
        text = sheet_file.read_text().rstrip()
        sheet_file.write_text(text[:-1] + f'\t(text "EXTERNAL-SHEET"\n\t\t(exclude_from_sim no)\n'
                              f'\t\t(at 50.8 50.8 0)\n\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 2 2)\n'
                              f'\t\t\t)\n\t\t)\n\t\t(uuid "{uuid.uuid4()}")\n\t)\n)\n')
        reloaded = wait_event(embed.ET_DOCUMENT_RELOADED)
        time.sleep(2)
        s.shot("13-sch-subsheet-reloaded")
        check(bool(reloaded) and frames()[embed.FK_SCH].title == title_before and "pic_sockets" in title_before,
              f"an external change to a sub-sheet reloads and stays on that sheet ({title_before})")

        # ---------------- 4. Robustness ----------------
        api.request(embed.SelectItems(kind=embed.FK_PCB, references=["U4"], zoom=True), embed.SelectItemsResponse)
        show(embed.FK_PCB, wait=2)
        s.xdotool("mousemove", str(w // 2 + 40), str(h // 2 + 40))
        s.xdotool("key", "x")                       # the interactive router
        time.sleep(1)
        kept_before = s.shot("14-pcb-before-hide")
        count_before = selection_count(base.DOCTYPE_PCB)
        api.request(embed.HideFrame(kind=embed.FK_PCB), embed.HideFrameResponse)
        time.sleep(1)
        api.request(embed.ShowFrame(kind=embed.FK_PCB, x=0, y=0, width=w, height=h), embed.ShowFrameResponse)
        time.sleep(2)
        kept_after = s.shot("15-pcb-after-show")
        check(mean_difference(kept_before, kept_after, canvas) < 0.05 and selection_count(base.DOCTYPE_PCB) == count_before,
              "HideFrame/ShowFrame keeps zoom, selection and the active tool")
        s.xdotool("key", "Escape")
        s.xdotool("key", "Escape")

        # Popups and tooltips: override-redirect, inside the host; never WM-managed.
        before = set(s.watcher.current())
        s.xdotool("mousemove", str(w // 2), str(h // 2), "click", "3")
        time.sleep(1.5)
        s.shot("16-context-menu")
        s.xdotool("key", "Escape")
        s.xdotool("mousemove", "60", "45")
        time.sleep(3)
        s.shot("17-tooltip")
        check(not set(s.watcher.current()) - before, "context menu and tooltip never become WM top-levels")

        show(embed.FK_PCB_3D, wait=3)
        s.xdotool("mousemove", str(w // 2), str(h // 2), "click", "1")
        s.xdotool("key", "ctrl+comma")
        appeared = wait_for(lambda: set(s.watcher.current()) - before - {top}, 15)
        time.sleep(1.5)
        s.shot("18-3d-preferences")
        dialogs = set(s.watcher.current()) - before - {top}
        transient_ok = bool(dialogs) and all(
            (disp.create_resource_object("window", d).get_wm_transient_for() or type("n", (), {"id": 0})).id == top
            for d in dialogs)
        check(appeared and transient_ok, "the 3D viewer's preferences dialog is transient for the host")
        s.xdotool("key", "Escape")
        time.sleep(1)

        # Host death, then a new host window.
        child = subprocess.Popen([sys.executable, "-c", f"""
import ctypes, sys, time
x = ctypes.CDLL("libX11.so.6"); xf = ctypes.CDLL("libXfixes.so.3")
x.XOpenDisplay.restype = ctypes.c_void_p
x.XDefaultRootWindow.restype = ctypes.c_ulong
x.XCreateSimpleWindow.restype = ctypes.c_ulong
x.XCreateSimpleWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong] + [ctypes.c_int] * 2 + [ctypes.c_uint] * 3 + [ctypes.c_ulong] * 2
x.XMapWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
xf.XFixesChangeSaveSet.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_int]
d = x.XOpenDisplay({s.name.encode()!r})
top = x.XCreateSimpleWindow(d, x.XDefaultRootWindow(d), 0, 0, 800, 600, 0, 0, 0)
c = x.XCreateSimpleWindow(d, top, 0, 0, 800, 600, 0, 0, 0)
x.XMapWindow(d, c); x.XMapWindow(d, top); x.XSync(d, 0)
print(c, flush=True)
xf.XFixesChangeSaveSet(d, int(sys.stdin.readline()), 0, 1, 1)
x.XSync(d, 0); print('ok', flush=True); time.sleep(600)
"""], env=s.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        second = int(child.stdout.readline())
        child.stdin.write(f"{holder}\n")
        child.stdin.flush()
        child.stdout.readline()
        api.request(embed.AttachToParent(window=second), embed.AttachToParentResponse)
        before_death = len(frames())
        events()
        child.kill()
        child.wait()
        detached = wait_event(embed.ET_DETACHED, 10)
        count = len(frames())
        api.request(embed.AttachToParent(window=host.container.id, toplevel=top), embed.AttachToParentResponse)
        pcb, _ = show(embed.FK_PCB, "19-pcb-after-host-death", 2)
        check(bool(detached) and count == before_death and parent_of(disp, pcb) == holder and viewable(disp, pcb),
              f"survives the host's death and re-attaches to a new window (detached={bool(detached)}, "
              f"frames={count}, parent ok={parent_of(disp, pcb) == holder}, viewable={viewable(disp, pcb)})")

        # (The second host's own unnamed top-level is expected.)
        strays = [(hex(wid), n) for wid, (t, n) in s.watcher.seen.items() if wid != top and t != top and n]
        check(not strays, f"no top-level ever escaped ({strays})")

    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
