#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bp.13 check for `kicad --embedded`: the person's undo and redo survive an agent's reads and the
host's check runs (DRC/ERC markers are regenerated data).

Carbon, Windows batch 12: in the embedded PCB editor on ecc83-pp, Ctrl+A, Delete, Ctrl+Z, Ctrl+Y,
then a click and Ctrl+Z showed "Incomplete undo/redo operation: some items not found" and
restored nothing, when an agent read the board (the hub's kicad_edit find: SetAgentActivity, then
GetItems as "agent:t1") after each key.

First, the person's keys alone with no click between them (climb-j, bp.13.1), three cycles:
Ctrl+A, Delete, the host's checks, Ctrl+Z, checks, Ctrl+Y, checks, Ctrl+Z; and again after the
person was in another top-level window (Backplane's composer) and clicked the canvas once.  The hidden progress
dialog of a check run used to take the keyboard off the canvas, so Ctrl+Z and Ctrl+Y did nothing
until a click.  (After a failed step the person's own RunAction, client person:pane, puts the
document back so the next steps still mean something.)

Then three rounds in the board editor (DRC markers) and three in the schematic editor (ERC markers),
the person's steps as real keys after a canvas click (xdotool), the host's RunChecks after each
edit, an agent read after EVERY step, and the agent's own commits interleaved:

- the agent adds an item;
- select all (markers included), Delete, checks: exactly the selected real items go;
- Ctrl+Z, checks: all of them come back;
- Ctrl+Y, checks: they go again;
- the agent adds a second item;
- click, Ctrl+Z: they come back; Ctrl+Y: they go; Ctrl+Z: back for the next round, checks;
- the agent's undo, twice: each takes back exactly its own item (per-actor undo unchanged).

No dialog may appear at any step (the window manager's client list and ET_DIALOG_OPENED).

Usage: backplane-embed-undo-agent-check.py KICAD KICAD_CLI [--out DIR] [--editor pcb|sch]...
Needs Xvfb, i3, xdotool, python-xlib, pynng, protobuf.
"""

import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import (FAILURES, IpcSession, Session, check,  # noqa: E402
                                          viewable, wait_for)

SOURCE = Path(__file__).resolve().parents[1]


def project_copy(stem="ecc83-pp"):
    """demos/ecc83 holds two projects; a copy with only `stem`'s files (Session opens the one)."""
    src = SOURCE / "demos/ecc83"
    dst = Path(tempfile.mkdtemp(prefix="undo-agent-src-")) / "ecc83"
    dst.mkdir()
    for p in src.iterdir():
        if p.is_dir():
            shutil.copytree(p, dst / p.name)
        elif p.name.startswith(stem + ".") or not p.name.startswith("ecc83-pp"):
            shutil.copy2(p, dst / p.name)
    return dst


def item_id(any_item):
    """An item's KIID, from its message (every item message has `id`)."""
    from google.protobuf import symbol_database
    try:
        msg = symbol_database.Default().GetSymbol(any_item.type_url.split("/")[-1])()
        any_item.Unpack(msg)
        return msg.id.value
    except Exception:   # noqa: BLE001
        return any_item.value[:64]


def run_editor(editor, args):
    pcb = editor == "pcb"
    out = args.out / editor
    print(f"== {editor}", flush=True)
    source = project_copy()
    try:
        with Session(args.kicad, out, source) as s, \
                IpcSession(args.kicad_cli, api_socket=s.socket, timeout=120000,
                           client_name="works.backplane.hub") as hub, \
                IpcSession(args.kicad_cli, api_socket=s.socket, timeout=120000,
                           client_name="agent:t1") as agent, \
                IpcSession(args.kicad_cli, api_socket=s.socket, timeout=120000,
                           client_name="person:pane") as pane:
            embed = hub.module("backplane.embed")
            coedit = hub.module("backplane.coedit")
            cmds = hub.module("common.commands.editor_commands")
            types = hub.module("common.types.base_types")
            enums = hub.module("common.types.enums")
            hub.module("board.board_types")            # registered, so item_id() can unpack items
            hub.module("schematic.schematic_types")
            kind = embed.FK_PCB if pcb else embed.FK_SCH
            frame_type = types.FT_PCB_EDITOR if pcb else types.FT_SCHEMATIC_EDITOR

            w, h = s.host.size
            shown = hub.request(embed.ShowFrame(kind=kind, x=0, y=0, width=w, height=h),
                                embed.ShowFrameResponse)
            wait_for(lambda: viewable(s.host.display, shown.frame.window), 60)
            time.sleep(3.0)
            doc = hub.request(cmds.GetOpenDocuments(type=types.DOCTYPE_PCB if pcb else types.DOCTYPE_SCHEMATIC),
                              cmds.GetOpenDocumentsResponse).documents[0]
            header = types.ItemHeader(document=doc)

            if pcb:
                real = [enums.KOT_PCB_FOOTPRINT, enums.KOT_PCB_TRACE, enums.KOT_PCB_ARC, enums.KOT_PCB_VIA,
                        enums.KOT_PCB_SHAPE, enums.KOT_PCB_TEXT, enums.KOT_PCB_TEXTBOX, enums.KOT_PCB_ZONE,
                        enums.KOT_PCB_DIMENSION]
                main_type, found_type = enums.KOT_PCB_FOOTPRINT, [enums.KOT_PCB_FOOTPRINT]
            else:
                real = [enums.KOT_SCH_SYMBOL, enums.KOT_SCH_LINE, enums.KOT_SCH_JUNCTION, enums.KOT_SCH_NO_CONNECT,
                        enums.KOT_SCH_LABEL, enums.KOT_SCH_GLOBAL_LABEL, enums.KOT_SCH_HIER_LABEL,
                        enums.KOT_SCH_TEXT, enums.KOT_SCH_SHAPE, enums.KOT_SCH_BUS_WIRE_ENTRY]
                main_type, found_type = enums.KOT_SCH_SYMBOL, [enums.KOT_SCH_SYMBOL]

            def ids(only=None):
                found = set()
                for t in (only or real):
                    try:
                        r = hub.request(cmds.GetItems(header=header, types=[t]), cmds.GetItemsResponse)
                    except Exception:   # noqa: BLE001  (a type this document does not have)
                        continue
                    for item in r.items:
                        found.add((t, item_id(item)))
                return found

            def markers():
                return len(hub.request(embed.GetMarkers(kind=kind), embed.GetMarkersResponse).markers)

            seq = [hub.request(embed.NextEvents(since=0, max=512), embed.NextEventsResponse).next]
            dialogs = []
            managed = set(s.watcher.current())

            def drain():
                reply = hub.request(embed.NextEvents(since=seq[0], max=512), embed.NextEventsResponse)
                seq[0] = reply.next
                dialogs.extend(e.detail for e in reply.events if e.type == embed.ET_DIALOG_OPENED)
                return reply.events

            def new_windows():
                return {wid: s.watcher.seen.get(wid) for wid in s.watcher.current() if wid not in managed}

            def no_dialog(tag):
                drain()
                extra = new_windows()
                ok = check(not dialogs and not extra,
                           f"{tag}: no dialog (events {dialogs}, windows {list(extra.values())})")
                if not ok:
                    s.shot(f"{tag.replace(' ', '-').replace(':', '')}-dialog")
                    # dismiss it so the run goes on (the person's OK)
                    for _ in range(3):
                        if not new_windows():
                            break
                        s.xdotool("key", "Return")
                        time.sleep(1.0)
                    dialogs.clear()
                    managed.update(s.watcher.current())
                return ok

            def run_checks():
                hub.request(embed.RunChecks(kind=kind), embed.RunChecksResponse)
                done = []

                def finished():
                    done.extend(e for e in drain() if e.type == embed.ET_CHECKS_DONE)
                    return bool(done)

                return wait_for(finished, 120, 0.3)

            def agent_read():
                """The hub's kicad_edit find: SetAgentActivity, then GetItems, as the agent."""
                if args.no_agent_reads:
                    return
                agent.request(coedit.SetAgentActivity(actor="agent:t1", task="find footprints", state="working"),
                              coedit.SetAgentActivityResponse)
                agent.request(cmds.GetItems(header=header, types=found_type), cmds.GetItemsResponse)

            cx, cy = w // 2, h // 2

            def key(chord, click=True):
                if click:
                    s.xdotool("mousemove", str(cx), str(cy), "click", "1")
                    time.sleep(0.6)
                s.xdotool("key", "--clearmodifiers", chord)
                time.sleep(2.5)   # the box came about 1.2 s after the key

            def agent_add(n):
                t = agent.request(cmds.BeginCommit(header=header), cmds.BeginCommitResponse)
                c = cmds.CreateItems(header=header)
                if pcb:
                    board = agent.module("board.board_types")
                    c.items.add().Pack(board.Track(
                        id=types.KIID(value=str(uuid.uuid4())),
                        start=types.Vector2(x_nm=60000000, y_nm=(20 + 3 * n) * 1000000),
                        end=types.Vector2(x_nm=90000000, y_nm=(20 + 3 * n) * 1000000),
                        width=types.Distance(value_nm=300000), layer=board.BL_F_Cu))
                else:
                    sch = agent.module("schematic.schematic_types")
                    c.items.add().Pack(sch.SchematicLine(
                        start=types.Vector2(x_nm=20000000, y_nm=(20 + 3 * n) * 1000000),
                        end=types.Vector2(x_nm=40000000, y_nm=(20 + 3 * n) * 1000000), type=1))
                agent.request(c, cmds.CreateItemsResponse)
                agent.request(cmds.EndCommit(header=header, id=t.id, action=cmds.CMA_COMMIT,
                                             message=f"agent item {n}"), cmds.EndCommitResponse)
                time.sleep(0.5)

            def agent_undo():
                agent.request(cmds.RunAction(action="common.Interactive.undo", frame=frame_type),
                              cmds.RunActionResponse)
                time.sleep(1.5)

            def expect(tag, want, shot=None):
                got = ids()
                ok = check(got == want, f"{tag} ({len(got)} real items, want {len(want)}; "
                                        f"{len(want - got)} missing, {len(got - want)} extra)")
                if not ok or shot:
                    s.shot(f"{tag.replace(' ', '-').replace(':', '')}")
                return ok

            # the person starts on the canvas
            s.xdotool("mousemove", str(cx), str(cy), "click", "1")
            time.sleep(0.5)
            s.xdotool("key", "Escape")
            check(run_checks(), f"{editor}: the first checks finish")
            first_markers = markers()
            check(first_markers > 0, f"{editor}: the checks leave markers to select ({first_markers})")
            base = ids()
            check(any(k[0] == main_type for k in base), f"{editor}: the document has real items ({len(base)})")

            def focused():
                r = subprocess.run(["xdotool", "getwindowfocus"], env=s.env, capture_output=True, text=True)
                wid = r.stdout.strip()
                name = subprocess.run(["xdotool", "getwindowname", wid], env=s.env, capture_output=True,
                                      text=True).stdout.strip() if wid else ""
                return f"{wid}:{name!r}"

            def keys_only(variant="keys only"):
                """The person's keys alone, no click between them (climb-j, bp.13.1): each key only
                when KiCad is idle, the host's checks after each edit."""
                if variant != "keys only":
                    # climb-j's redo_after_backplane: the person was in Backplane's composer (another
                    # top-level window), then clicks the canvas once
                    scr = s.host.display.screen()
                    # a widget of the host's own top-level, as the composer is in Backplane's window
                    other = s.host.top.create_window(w - 150, h - 80, 140, 70, 0, scr.root_depth,
                                                     background_pixel=scr.white_pixel)
                    other.set_wm_name("Composer")
                    other.map()
                    s.host.display.sync()
                    time.sleep(1.0)
                    s.xdotool("windowfocus", "--sync", str(other.id))
                    time.sleep(0.5)
                    print(f"INFO {editor} {variant}: focus in the other window {focused()}", flush=True)
                    managed.update(s.watcher.current())
                    s.xdotool("mousemove", str(cx), str(cy), "click", "1")
                    time.sleep(0.6)
                    s.xdotool("key", "Escape")
                    time.sleep(0.4)
                    other.destroy()
                    s.host.display.sync()
                    print(f"INFO {editor} {variant}: focus after the canvas click {focused()}", flush=True)
                for cyc in range(1, 4):
                    tag = f"{editor} {variant} {cyc}"
                    full = ids()
                    print(f"INFO {tag}: focus before Ctrl+A {focused()}", flush=True)
                    key("ctrl+a", click=False)
                    key("Delete", click=False)
                    r = ids()
                    check(not any(k[0] == main_type for k in r),
                          f"{tag}: Ctrl+A, Delete removes the real items ({len(full - r)} gone, {len(r)} left)")
                    run_checks()
                    agent_read()
                    print(f"INFO {tag}: focus before Ctrl+Z {focused()}", flush=True)
                    key("ctrl+z", click=False)
                    expect(f"{tag}: Ctrl+Z after the checks brings them back", full)
                    run_checks()
                    agent_read()
                    print(f"INFO {tag}: focus before Ctrl+Y {focused()}", flush=True)
                    key("ctrl+y", click=False)
                    if not expect(f"{tag}: Ctrl+Y after the checks removes them again", r):
                        # diagnosis: the person's own redo through the API (actor: the person)
                        pane.request(cmds.RunAction(action="common.Interactive.redo", frame=frame_type),
                                     cmds.RunActionResponse)
                        time.sleep(1.5)
                        print(f"INFO {tag}: after the person's RunAction redo, {len(ids())} real items", flush=True)
                    run_checks()
                    agent_read()
                    key("ctrl+z", click=False)
                    if ids() != full:
                        pane.request(cmds.RunAction(action="common.Interactive.undo", frame=frame_type),
                                     cmds.RunActionResponse)
                        time.sleep(1.5)
                    expect(f"{tag}: Ctrl+Z after that brings them back", full)
                    run_checks()
                    agent_read()
                    no_dialog(tag)

            keys_only()
            keys_only("after another window")

            for rnd in range(1, 4):
                tag = f"{editor} round {rnd}"
                before = ids()
                agent_add(2 * rnd)
                agent_read()
                s1 = ids()
                a1 = s1 - before
                check(len(a1) == 1, f"{tag}: the agent's first item is on the canvas ({len(a1)})")
                check(run_checks(), f"{tag}: checks after the agent's commit")
                m = markers()

                key("ctrl+a")
                agent_read()
                key("Delete", click=False)
                agent_read()
                no_dialog(f"{tag}: select all + Delete")
                r = ids()
                gone = s1 - r
                check(not any(k[0] == main_type for k in r) and gone,
                      f"{tag}: select all ({m} markers in it) + Delete removes the real items "
                      f"({len(gone)} gone, {len(r)} left)")
                run_checks()
                agent_read()

                key("ctrl+z")
                agent_read()
                no_dialog(f"{tag}: Ctrl+Z")
                expect(f"{tag}: Ctrl+Z brings every real item back", s1)
                run_checks()
                agent_read()

                key("ctrl+y")
                agent_read()
                no_dialog(f"{tag}: Ctrl+Y")
                expect(f"{tag}: Ctrl+Y removes them again", r)
                run_checks()
                agent_read()

                agent_add(2 * rnd + 1)
                agent_read()
                r2 = ids()
                a2 = r2 - r
                check(len(a2) == 1, f"{tag}: the agent's second item is on the canvas ({len(a2)})")

                key("ctrl+z")
                agent_read()
                no_dialog(f"{tag}: click, Ctrl+Z")
                expect(f"{tag}: click, Ctrl+Z brings them back", s1 | a2)
                agent_read()

                key("ctrl+y")
                agent_read()
                no_dialog(f"{tag}: Ctrl+Y again")
                expect(f"{tag}: Ctrl+Y removes them again", r2)
                agent_read()

                key("ctrl+z")
                agent_read()
                no_dialog(f"{tag}: Ctrl+Z again")
                expect(f"{tag}: Ctrl+Z restores them for what follows", s1 | a2)
                run_checks()
                agent_read()

                agent_undo()
                agent_read()
                expect(f"{tag}: the agent's undo takes back only its second item", s1)
                agent_undo()
                agent_read()
                expect(f"{tag}: and the next only its first", before)
                no_dialog(f"{tag}: the agent's undo")
                check(s.kicad_process.poll() is None, f"{tag}: KiCad is running")

            expect(f"{editor}: after three rounds the document is as it was", base)
    except Exception as error:   # noqa: BLE001
        check(False, f"{editor}: {type(error).__name__}: {error}")
    finally:
        shutil.rmtree(source.parent, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kicad", type=Path)
    ap.add_argument("kicad_cli", type=Path)
    ap.add_argument("--out", type=Path, default=Path("embed-undo-agent-out"))
    ap.add_argument("--editor", action="append", choices=["pcb", "sch"])
    ap.add_argument("--no-agent-reads", action="store_true",
                    help="diagnosis only: leave out the agent's reads between the person's steps")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for editor in args.editor or ["pcb", "sch"]:
        run_editor(editor, args)
    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
