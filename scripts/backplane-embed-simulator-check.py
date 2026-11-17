#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bp.13.2 (sim-frame, sim-run) in `kicad --embedded`: the Simulator as an embedded frame.

On a generated RC low-pass project:

- GetCapabilities has sim-frame and sim-run; no Simulator frame exists until asked for.
- ShowFrame{FK_SIMULATOR} opens it on demand (the schematic editor is loaded, hidden), mapped in
  the host's container, with ET_FRAME_OPENED and ET_FRAME_SHOWN and no dialog.
- RunSimulation{show} is the window's own run: a tab with the command and the asked traces, the
  numbers back over the API (the RC step against its closed form).
- A run without `show` while the window is open works and leaves the window's tab alone.
- A run the window cannot start (a bad override) is SIM_NETLIST_ERROR with ET_NOTICE kind
  "simulation" and no dialog.
- SetSimulationSetup shows in the open window; GetSimulationSetup reads its live tabs.
- The same commands through the GUI process' handlers with no Simulator window (models, netlist,
  an .ac run, SIM_RUNNING / AS_BUSY / StopSimulation, an assignment and its undo).
- ShowFrame{prewarm} opens it hidden with no show request.
- The tune tool adds a part to the window's tuner panel (Setup.tuners) with no dialog.
- Closing the window while an API run is in flight (plain or shown) leaves the run to be stopped
  and collected.
- HideFrame, ShowFrame again, Ctrl+W on the focused frame closes it (as the editors), Close
  {FK_SIMULATOR} closes it without a prompt; the schematic editor stays.

Runs on a private Xvfb + i3 with a python-xlib stand-in host (input through XTEST); screenshots
to --out.
"""

import argparse
import json
import math
from pathlib import Path
import sys
import tempfile
import time

from Xlib import X, XK, display as xdisplay
from Xlib.ext import xtest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import (FAILURES, IpcSession, Session, check,  # noqa: E402
                                          viewable, wait_for)
from backplane_sim_test_support import Part, write_project  # noqa: E402

FK_SIMULATOR = 8


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--out", type=Path, default=Path("embed-simulator-out"))
    args = parser.parse_args()

    source = Path(tempfile.mkdtemp(prefix="backplane-sim-project-")) / "simrc"
    write_project(source, "simrc", [Part("V1", "dc 0 pulse(0 1 0 1n 1n 1 2)", ["in", "GND"]),
                                    Part("R1", "1k", ["in", "out"]), Part("C1", "1u", ["out", "GND"])])

    with Session(args.kicad, args.out, source) as s, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=300000) as api:
        embed = api.module("backplane.embed")
        sim = api.module("backplane.sim")
        caps = api.module("backplane.capabilities")
        types = api.module("common.types.base_types")
        editor = api.module("common.commands.editor_commands")
        disp = s.host.display
        w, h = s.host.size
        since = [0]

        def events():
            reply = api.request(embed.NextEvents(since=since[0]), embed.NextEventsResponse)
            for e in reply.events:
                since[0] = max(since[0], e.seq)
            return list(reply.events)

        def frames():
            return {f.kind: f for f in api.request(embed.GetFrames(), embed.GetFramesResponse).frames}

        # The person's mouse and keys, through XTEST (no xdotool needed)
        person = xdisplay.Display(s.name)

        def click(x, y):
            origin = s.host.container.translate_coords(person.screen().root, 0, 0)
            xtest.fake_input(person, X.MotionNotify, x=x - origin.x, y=y - origin.y)
            xtest.fake_input(person, X.ButtonPress, 1)
            xtest.fake_input(person, X.ButtonRelease, 1)
            person.sync()

        def chord(*names):
            codes = [person.keysym_to_keycode(XK.string_to_keysym(name)) for name in names]
            for code in codes:
                xtest.fake_input(person, X.KeyPress, code)
            for code in reversed(codes):
                xtest.fake_input(person, X.KeyRelease, code)
            person.sync()

        def collect_any(response, doc):
            deadline = time.monotonic() + 60
            while response.status == sim.SIM_RUNNING and time.monotonic() < deadline:
                response = api.request(sim.GetSimulationResult(run_id=response.run_id, wait_ms=500, document=doc),
                                       sim.RunSimulationResponse)
            return response

        def collect(response):
            deadline = time.monotonic() + 60
            while response.status == sim.SIM_RUNNING and time.monotonic() < deadline:
                response = api.request(sim.GetSimulationResult(run_id=response.run_id, wait_ms=500,
                                                               document=document), sim.RunSimulationResponse)
            return response

        have = set(api.request(caps.GetCapabilities(), caps.GetCapabilitiesResponse).capabilities)
        check({"sim-frame", "sim-run"} <= have, "GetCapabilities has sim-frame and sim-run")
        check(embed.FK_SIMULATOR == FK_SIMULATOR, "FK_SIMULATOR = 8")
        check(FK_SIMULATOR not in frames(), f"no Simulator frame before it is asked for ({sorted(frames())})")

        before = set(s.watcher.current())

        # The same commands through the GUI process' handlers, with no Simulator window
        r = api.request(embed.ShowFrame(kind=embed.FK_SCH, x=0, y=0, width=w, height=h), embed.ShowFrameResponse)
        wait_for(lambda: viewable(disp, r.frame.window), 60)
        time.sleep(2)
        docs = api.request(editor.GetOpenDocuments(type=types.DOCTYPE_SCHEMATIC), editor.GetOpenDocumentsResponse)
        document = docs.documents[0]
        models = api.request(sim.GetSimulationModels(document=document), sim.GetSimulationModelsResponse)
        check(sorted(m.reference for m in models.models) == ["C1", "R1", "V1"] and models.problems == 0,
              "GUI: GetSimulationModels")
        netlist = api.request(sim.GetSimulationNetlist(document=document, command=".op"),
                              sim.GetSimulationNetlistResponse)
        check("R1 /in /out 1k" in netlist.netlist and not netlist.problems, "GUI: GetSimulationNetlist")
        r = collect_any(api.request(sim.RunSimulation(document=document, command=".ac dec 10 1 100k",
                                                      signals=["V(/out)"], measures=["f3db WHEN vdb(/out)=-3.0103"],
                                                      overrides=[sim.Override(reference="V1", value="dc 0 ac 1")],
                                                      wait_ms=1000), sim.RunSimulationResponse), document)
        f3db = r.measures[0].value if r.measures else 0
        check(r.status == sim.SIM_OK and not r.shown and abs(f3db - 159.155) < 0.5,
              f"GUI: RunSimulation .ac without a Simulator window (f3db {f3db:.3f} Hz; status {r.status}, "
              f"measures {[(m.name, m.failed, m.error) for m in r.measures]})")
        r = api.request(sim.RunSimulation(document=document, command=".tran 20n 60", wait_ms=50),
                        sim.RunSimulationResponse)
        busy = api.request_raw(sim.RunSimulation(document=document, command=".op"))
        frames()   # the UI thread serves other requests while ngspice runs
        stop = api.request(sim.StopSimulation(document=document), sim.StopSimulationResponse)
        done = collect_any(api.request(sim.GetSimulationResult(document=document, wait_ms=1000),
                                       sim.RunSimulationResponse), document)
        check(r.status == sim.SIM_RUNNING and busy.status.status == api.envelope.AS_BUSY and stop.stopping
              and done.status == sim.SIM_ABORTED and done.points > 10,
              f"GUI: SIM_RUNNING, AS_BUSY for a second run, StopSimulation ({done.points} points kept)")
        a = api.request(sim.AssignSimulationModel(
            document=document, references=["R1"],
            builtin=sim.BuiltinModel(device="R", params=[sim.Param(name="r", value="2k")])),
            sim.AssignSimulationModelResponse)
        r = collect_any(api.request(sim.RunSimulation(document=document, command=".tf v(/out) V1", wait_ms=1000),
                                    sim.RunSimulationResponse), document)
        rout = next((v.value for v in r.values if v.name.startswith("output_impedance")), 0)
        check(a.changed == 1 and abs(rout - 2000) < 1e-6, f"GUI: AssignSimulationModel is simulated (Rout {rout})")
        api.request_raw(editor.RunAction(action="common.Interactive.undo"))
        time.sleep(0.5)
        value = api.request(sim.GetSimulationModels(document=document, references=["R1"]),
                            sim.GetSimulationModelsResponse).models[0].value
        check(value == "1k", f"GUI: the assignment is one undo step (R1 = {value})")
        setup = api.request(sim.GetSimulationSetup(document=document), sim.SetupResponse).setup
        check(not setup.tabs and not setup.tuners, "GUI: GetSimulationSetup with no workbook and no window")
        check(FK_SIMULATOR not in frames(), "none of it opened the Simulator window")
        check(not set(s.watcher.current()) - before, "no dialog appeared")
        api.request(embed.HideFrame(window=frames()[embed.FK_SCH].window), embed.HideFrameResponse)

        # A prewarm, though a host should not ask for one: opened hidden, no show request
        events()
        r = api.request(embed.ShowFrame(kind=FK_SIMULATOR, prewarm=True, width=w, height=h), embed.ShowFrameResponse)
        got = []
        opened = wait_for(lambda: got.extend(events()) or any(
            e.type == embed.ET_FRAME_OPENED and e.frame.kind == FK_SIMULATOR for e in got), 30, 0.3)
        time.sleep(1)
        got.extend(events())
        check(opened and FK_SIMULATOR in frames() and not frames()[FK_SIMULATOR].shown,
              "ShowFrame{prewarm} opens the Simulator hidden")
        check(not any(e.type in (embed.ET_SHOW_REQUESTED, embed.ET_FRAME_SHOWN) and e.frame.kind == FK_SIMULATOR
                      for e in got), "a prewarm posts no show request and no ET_FRAME_SHOWN")
        check(not set(s.watcher.current()) - before, "no dialog appeared")
        api.request(embed.Close(mode=embed.CM_DISCARD, kind=FK_SIMULATOR), embed.CloseResponse)
        check(wait_for(lambda: FK_SIMULATOR not in frames(), 10), "the prewarmed frame closes")

        # Opened on demand; the schematic editor comes with it, hidden
        events()
        t = time.monotonic()
        r = api.request(embed.ShowFrame(kind=FK_SIMULATOR, x=0, y=0, width=w, height=h), embed.ShowFrameResponse)
        took = time.monotonic() - t
        check(r.frame.kind == FK_SIMULATOR and r.frame.window != 0,
              f"ShowFrame opens the Simulator ({took:.1f} s, frame_name {r.frame.frame_name!r})")
        check(wait_for(lambda: viewable(disp, r.frame.window), 60), "its window is mapped in the host's container")
        time.sleep(2)
        now = frames()
        check(FK_SIMULATOR in now and now[FK_SIMULATOR].shown, "GetFrames lists it, shown")
        check(embed.FK_SCH in now and not now[embed.FK_SCH].shown, "the schematic editor stays hidden behind it")
        got = events()
        kinds = [(e.type, e.frame.kind) for e in got]
        check((embed.ET_FRAME_OPENED, FK_SIMULATOR) in kinds and (embed.ET_FRAME_SHOWN, FK_SIMULATOR) in kinds,
              "ET_FRAME_OPENED and ET_FRAME_SHOWN for FK_SIMULATOR")
        check(not set(s.watcher.current()) - before, "no dialog appeared")
        s.shot("01-simulator")

        # The window's own run, with the numbers back
        r = collect(api.request(sim.RunSimulation(document=document, command=".tran 5u 5m", show=True,
                                                  signals=["V(/out)"], directives=[".options reltol=1e-6"],
                                                  wait_ms=1000), sim.RunSimulationResponse))
        check(r.status == sim.SIM_OK and r.shown and r.analysis == "TRAN",
              f"RunSimulation{{show}} ran in the window (status {r.status}, shown {r.shown}, {r.points} points)")
        t_axis = r.vectors[0].real if r.vectors else []
        out = next((v.real for v in r.vectors if v.signal == "V(/out)"), [])
        worst = max((abs(v - (1 - math.exp(-max(x - 1e-9, 0) / 1e-3))) for x, v in zip(t_axis, out)), default=1)
        check(len(out) > 100 and worst < 2e-3, f"the step matches 1 - exp(-t/RC) (max error {worst:.2e})")
        time.sleep(2)
        setup = api.request(sim.GetSimulationSetup(document=document), sim.SetupResponse).setup
        tabs = [(t.analysis, t.command, list(t.signals)) for t in setup.tabs]
        check(any(a == "TRAN" and c == ".tran 5u 5m" and "V(/out)" in sig for a, c, sig in tabs),
              f"the window has a TRAN tab with the command and the trace ({tabs})")
        check(not set(s.watcher.current()) - before, "no dialog appeared during the run")
        s.shot("02-run-shown")

        # A run that is not shown, with the window open
        r = collect(api.request(sim.RunSimulation(document=document, command=".op",
                                                  overrides=[sim.Override(reference="V1", field="dc", value="3")],
                                                  wait_ms=1000), sim.RunSimulationResponse))
        value = next((v.value for v in r.values if v.signal == "V(/out)"), None)
        check(r.status == sim.SIM_OK and not r.shown and value is not None and abs(value - 3.0) < 1e-6,
              f"a run without show works beside the open window (V(out)={value})")
        setup = api.request(sim.GetSimulationSetup(document=document), sim.SetupResponse).setup
        check([t.analysis for t in setup.tabs] == ["TRAN"], "the window's tabs are untouched by it")

        # A run the window cannot start: told, not asked
        events()
        r = collect(api.request(sim.RunSimulation(document=document, command=".tran 5u 5m", show=True,
                                                  overrides=[sim.Override(reference="R1", field="bogus", value="1")],
                                                  wait_ms=1000), sim.RunSimulationResponse))
        check(r.status in (sim.SIM_NETLIST_ERROR, sim.SIM_MODEL_ERROR),
              f"a bad override in the window's run is refused (status {r.status})")
        notices = []
        wait_for(lambda: notices.extend(json.loads(e.detail or "{}") for e in events()
                                        if e.type == embed.ET_NOTICE) or notices, 5, 0.3)
        check(any(n.get("kind") == "simulation" for n in notices),
              f"ET_NOTICE kind simulation instead of a message box ({notices})")
        check(not set(s.watcher.current()) - before, "no dialog appeared for the error")
        good = collect(api.request(sim.RunSimulation(document=document, command=".tran 5u 5m", show=True,
                                                     wait_ms=1000), sim.RunSimulationResponse))
        check(good.status == sim.SIM_OK and good.shown, "the next shown run is clean (the override is forgotten)")

        # max_run_points in the GUI process, both ways: a shown run (the window's own) and a
        # background one are stopped at the limit with their points; no dialog, and it runs after
        for shown in (True, False):
            capped = collect(api.request(sim.RunSimulation(document=document, command=".tran 1u 5", show=shown,
                                                           signals=["V(/out)"], max_run_points=30000,
                                                           max_points=200, wait_ms=1000),
                                         sim.RunSimulationResponse))
            limit = [p for p in capped.problems if p.kind == sim.PK_POINT_LIMIT]
            check(capped.status == sim.SIM_POINT_LIMIT and capped.shown == shown and len(limit) == 1
                  and 0 < capped.points < 31000 and 2 <= len(capped.vectors[0].real) <= 200
                  and capped.thinning == sim.TH_MIN_MAX,
                  f"GUI: max_run_points stops a {'shown' if shown else 'background'} run "
                  f"({capped.points} points, status {capped.status})")
        after = collect(api.request(sim.RunSimulation(document=document, command=".tran 5u 5m", show=True,
                                                      signals=["V(/out)"], wait_ms=1000), sim.RunSimulationResponse))
        check(after.status == sim.SIM_OK and after.shown and after.max_run_points == 5000000,
              "the next shown run is whole (limit 5000000)")
        check(not set(s.watcher.current()) - before, "no dialog appeared for the limit")

        # The saved set-up, with the window open
        answer = api.request(sim.SetSimulationSetup(document=document, command=".ac dec 10 1 100k",
                                                    signals=["V(/out)"]), sim.SetupResponse)
        time.sleep(1)
        setup = api.request(sim.GetSimulationSetup(document=document), sim.SetupResponse).setup
        check(answer.changed and [t.analysis for t in setup.tabs] == ["TRAN", "AC"],
              f"SetSimulationSetup adds a tab in the open window ({[t.analysis for t in setup.tabs]})")
        check(not set(s.watcher.current()) - before, "no dialog appeared")
        s.shot("03-setup")

        # The tuner: the person picks R1 in the schematic; it joins the window's tuner panel
        tran = collect(api.request(sim.RunSimulation(document=document, command=".tran 5u 5m", show=True,
                                                     signals=["V(/out)"], wait_ms=1000), sim.RunSimulationResponse))
        sch = api.request(embed.ShowFrame(kind=embed.FK_SCH, x=0, y=0, width=w, height=h), embed.ShowFrameResponse)
        wait_for(lambda: viewable(disp, sch.frame.window), 30)
        time.sleep(1)

        def pick(reference, row):
            # the part's reference text fills the canvas; the tune tool then takes a click on it
            box = types.Box2(position=types.Vector2(x_nm=49_000_000, y_nm=int((25.4 + 12.7 * row - 4.2) * 1e6)),
                             size=types.Vector2(x_nm=5_000_000, y_nm=3_200_000))
            api.request(embed.SelectItems(kind=embed.FK_SCH, references=[reference], zoom_box_nm=box),
                        embed.SelectItemsResponse)
            time.sleep(1)
            api.request_raw(editor.RunAction(action="eeschema.Simulation.tune"))
            time.sleep(1)
            for dx, dy in ((0, 0), (-60, 0), (60, 0), (0, -40), (0, 40), (-120, 0), (120, 0)):
                click(w // 2 + dx, h // 2 + dy)
                time.sleep(0.7)
                tuners = api.request(sim.GetSimulationSetup(document=document), sim.SetupResponse).setup.tuners
                if reference in tuners:
                    return list(tuners)
                api.request_raw(editor.RunAction(action="eeschema.Simulation.tune"))
                time.sleep(0.5)
            return list(api.request(sim.GetSimulationSetup(document=document), sim.SetupResponse).setup.tuners)

        s.shot("03b-tune-pick")
        tuners = pick("R1", 1)
        check(tran.status == sim.SIM_OK and tuners == ["R1"],
              f"the tune tool adds R1 to the Simulator's tuners, embedded ({tuners})")
        check(not set(s.watcher.current()) - before, "no dialog appeared for the tuner")
        chord("Escape")
        api.request(embed.HideFrame(window=sch.frame.window), embed.HideFrameResponse)
        api.request(embed.ShowFrame(kind=FK_SIMULATOR, x=0, y=0, width=w, height=h), embed.ShowFrameResponse)
        time.sleep(1)
        s.shot("03c-tuner")
        r = collect(api.request(sim.RunSimulation(document=document, command=".op", wait_ms=1000),
                                sim.RunSimulationResponse))
        setup = api.request(sim.GetSimulationSetup(document=document), sim.SetupResponse).setup
        check(r.status == sim.SIM_OK and list(setup.tuners) == ["R1"],
              "an API run beside the window keeps its tuner")

        # Closing the window while an API run is in flight: the run goes on and is collected
        for shown in (False, True):
            if FK_SIMULATOR not in frames():
                api.request(embed.ShowFrame(kind=FK_SIMULATOR, x=0, y=0, width=w, height=h),
                            embed.ShowFrameResponse)
                time.sleep(2)
            r = api.request(sim.RunSimulation(document=document, command=".tran 20n 60", show=shown, wait_ms=50,
                                              timeout_ms=60000), sim.RunSimulationResponse)
            api.request(embed.Close(mode=embed.CM_DISCARD, kind=FK_SIMULATOR), embed.CloseResponse)
            gone = wait_for(lambda: FK_SIMULATOR not in frames(), 10)
            still = api.request(sim.GetSimulationResult(document=document, run_id=r.run_id, wait_ms=300),
                                sim.RunSimulationResponse)
            stop = api.request(sim.StopSimulation(document=document), sim.StopSimulationResponse)
            done = collect(api.request(sim.GetSimulationResult(document=document, run_id=r.run_id, wait_ms=1000),
                                       sim.RunSimulationResponse))
            check(r.status == sim.SIM_RUNNING and r.shown == shown and gone and still.status == sim.SIM_RUNNING
                  and stop.stopping and done.status == sim.SIM_ABORTED and done.points > 10,
                  f"the window closed during a {'shown' if shown else 'plain'} API run: the run goes on, is "
                  f"stopped and collected ({done.points} points; statuses {r.status}/{still.status}/{done.status})")
            after = collect(api.request(sim.RunSimulation(document=document, command=".op", wait_ms=1000),
                                        sim.RunSimulationResponse))
            check(after.status == sim.SIM_OK, "and the simulator is usable afterwards")
        check(s.kicad_process.poll() is None, "KiCad is still running")
        check(not set(s.watcher.current()) - before, "no dialog appeared")
        api.request(embed.ShowFrame(kind=FK_SIMULATOR, x=0, y=0, width=w, height=h), embed.ShowFrameResponse)
        time.sleep(2)

        # Hide, show, focus and Ctrl+W
        check(not frames()[FK_SIMULATOR].modified, "a reopened Simulator starts unmodified")
        window = frames()[FK_SIMULATOR].window
        events()
        api.request(embed.HideFrame(window=window), embed.HideFrameResponse)
        check(wait_for(lambda: not frames()[FK_SIMULATOR].shown, 10), "HideFrame hides it")
        check(not any(e.type == embed.ET_FRAME_HIDDEN for e in events()),
              "no ET_FRAME_HIDDEN for a host hide (as for the editors)")
        r = api.request(embed.ShowFrame(kind=FK_SIMULATOR, x=0, y=0, width=w, height=h), embed.ShowFrameResponse)
        check(r.frame.window == window and wait_for(lambda: frames()[FK_SIMULATOR].shown, 10),
              "ShowFrame shows the same frame again")
        time.sleep(1)
        check(not frames()[FK_SIMULATOR].modified, "a freshly opened Simulator is not modified")
        click(w // 5, h * 3 // 4)   # in its console: a click on the signal grid would change a trace
        check(wait_for(lambda: frames()[FK_SIMULATOR].focused, 10), "a click gives it the keyboard (focused)")
        check(not frames()[FK_SIMULATOR].modified, "the click changed nothing")
        events()
        chord("Control_L", "w")
        closed = wait_for(lambda: FK_SIMULATOR not in frames(), 10)
        s.shot("05-after-ctrl-w")
        check(closed, "Ctrl+W closes the Simulator frame")
        check(any(e.type == embed.ET_FRAME_CLOSED and e.frame.kind == FK_SIMULATOR for e in events()),
              "ET_FRAME_CLOSED")
        check(embed.FK_SCH in frames(), "the schematic editor stays")
        check(not set(s.watcher.current()) - before, "no prompt appeared")

        # Open again, run (the workbook is then modified), close without a prompt
        api.request(embed.ShowFrame(kind=FK_SIMULATOR, x=0, y=0, width=w, height=h), embed.ShowFrameResponse)
        time.sleep(2)
        r = collect(api.request(sim.RunSimulation(document=document, command=".tran 5u 5m", show=True,
                                                  signals=["V(/out)"], wait_ms=1000), sim.RunSimulationResponse))
        check(r.status == sim.SIM_OK and r.shown, "it runs again after being reopened")
        time.sleep(1)
        s.shot("04-reopened")
        api.request(embed.Close(mode=embed.CM_DISCARD, kind=FK_SIMULATOR), embed.CloseResponse)
        check(wait_for(lambda: FK_SIMULATOR not in frames(), 10), "Close{FK_SIMULATOR} closes it")
        check(not set(s.watcher.current()) - before, "no save prompt")
        r = collect(api.request(sim.RunSimulation(document=document, command=".op", wait_ms=1000),
                                sim.RunSimulationResponse))
        check(r.status == sim.SIM_OK, "the API still simulates with the window closed")

    print(f"{len(FAILURES)} failed" if FAILURES else "ALL PASSED", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
