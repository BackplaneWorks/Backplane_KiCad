#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bp.13.2 (sim-run): kiapi.backplane.sim on the headless server, against closed-form answers.

Small schematics are generated (backplane_sim_test_support), opened in `kicad-cli api-server`,
and every command is asked:

  analyses   .op and .dc on a divider, .tran and .ac on an RC low-pass (the corner, the time
             constant), .noise (kT/C), .pz (the pole), .tf (gain, input and output resistance),
             .sp (a series resistor between two ports), fft (a sine's line)
  run        overrides (netlist only), directives, measures, max_points, the saved set-up as the
             default command, SIM_RUNNING + GetSimulationResult, StopSimulation, the timeout,
             AS_BUSY for a second run
  failures   each named distinctly with its reference or node: a missing model, a library that is
             not found, a model not in its library, an unknown subcircuit, no convergence, a
             floating node, no ground, a singular matrix
  models     GetSimulationModels, AssignSimulationModel (built-in, library .model and .subckt
             with a pin map, raw, clear, exclude_from_sim, all-or-nothing), saved and reloaded
  set-up     SetSimulationSetup / GetSimulationSetup write the workbook KiCad's Simulator reads

    backplane-sim-headless-check.py <kicad-cli> [--out DIR]

Prints one line per check and SIM_HEADLESS_OK, or raises on the first failure.
"""

import argparse
import cmath
import json
import math
from pathlib import Path
import sys
import time

from google.protobuf.empty_pb2 import Empty

from backplane_ipc_test_support import IpcSession
from backplane_sim_test_support import Part, write_project

BOLTZMANN = 1.380649e-23

MODELS_LIB = """* models for the simulation check
.model DCHK D(Is=1e-14 N=1 Rs=0.5)
.subckt DIVCHK in out gnd
R1 in out 1k
R2 out gnd 1k
.ends DIVCHK
"""


def close(actual, expected, rel=1e-3, absolute=0.0):
    return abs(actual - expected) <= max(rel * abs(expected), absolute)


class Checker:
    def __init__(self, api, out):
        self.api = api
        self.out = out
        self.sim = api.module("backplane.sim")
        self.project = api.module("common.commands.project_commands")
        self.types = api.module("common.types.base_types")
        self.document = None
        self.count = 0
        self.report = []

    def ok(self, name, detail=""):
        self.count += 1
        self.report.append({"check": name, "detail": detail})
        print(f"ok {self.count:3d}  {name}{'  ' + detail if detail else ''}", flush=True)

    def open(self, name, parts, texts=(), files=None):
        if self.document is not None:
            self.api.request(self.project.CloseDocument(document=self.document), Empty)
            self.document = None
        folder = self.api.root / name
        path = write_project(folder, name, parts, texts)
        for file_name, text in (files or {}).items():
            target = folder / file_name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text)
        self.path = path
        self.document = self.api.request(
            self.project.OpenDocument(type=self.types.DOCTYPE_SCHEMATIC, path=str(path)),
            self.project.OpenDocumentResponse).document
        return self.document

    def run(self, command="", wait_ms=30000, **fields):
        return self.api.request(
            self.sim.RunSimulation(document=self.document, command=command, wait_ms=wait_ms, **fields),
            self.sim.RunSimulationResponse)

    def vector(self, response, name):
        for vector in response.vectors:
            if vector.name == name or vector.signal == name:
                return vector
        raise AssertionError(f"no vector {name} in {[v.name for v in response.vectors]}")

    def value(self, response, name):
        for value in response.values:
            if value.name == name or value.signal == name:
                return value.value
        raise AssertionError(f"no value {name} in {[v.name for v in response.values]}")

    def problem(self, response, kind):
        for problem in response.problems:
            if problem.kind == kind:
                return problem
        raise AssertionError(f"no problem of kind {kind} in {response.problems}")


def divider(r1="3k", r2="1k", source="dc 10"):
    return [Part("V1", source, ["in", "GND"]), Part("R1", r1, ["in", "out"]),
            Part("R2", r2, ["out", "GND"])]


def low_pass(source="dc 1 ac 1"):
    return [Part("V1", source, ["in", "GND"]), Part("R1", "1k", ["in", "out"]),
            Part("C1", "1u", ["out", "GND"])]


def check_analyses(c):
    sim = c.sim

    # .op: 10 V on 3k + 1k
    c.open("divider", divider())
    r = c.run(".op")
    assert r.status == sim.SIM_OK, r
    assert r.analysis == "OP" and r.points == 1 and r.devices == 3 and r.ngspice_version, r
    assert close(c.value(r, "V(/out)"), 2.5) and close(c.value(r, "/in"), 10.0), r.values
    assert close(c.value(r, "I(R1)"), 2.5e-3) and close(c.value(r, "I(V1)"), -2.5e-3), r.values
    assert r.timings.total_ms > 0 and r.timings.netlist_ms > 0 and not r.netlist, r.timings
    c.ok("op divider", f"V(out)={c.value(r, 'V(/out)'):.6g} ngspice {r.ngspice_version}")

    # .dc: V(out) = V1 / 4 at every point; the sweep axis first
    r = c.run(".dc V1 0 10 1", signals=["out"])
    assert r.status == sim.SIM_OK and r.analysis == "DC" and r.points == 11, r
    assert r.vectors[0].kind == sim.VK_SWEEP and list(r.vectors[0].real) == [float(i) for i in range(11)], r.vectors[0]
    out = c.vector(r, "V(/out)")
    assert out.kind == sim.VK_VOLTAGE and all(close(v, i / 4, absolute=1e-12) for i, v in enumerate(out.real)), out
    assert not c.vector(r, "V(/in)").real and c.vector(r, "V(/in)").length == 11, "unasked vectors carry no data"
    c.ok("dc sweep", "V(out)=V1/4 at 11 points")

    # .tf: gain 1/4, input 4k, output 3k || 1k
    r = c.run(".tf v(/out) V1")
    assert r.status == sim.SIM_OK and r.analysis == "TF", r
    assert close(c.value(r, "transfer_function"), 0.25), r.values
    assert close(c.value(r, "v1#input_impedance"), 4000.0), r.values
    assert close(c.value(r, "output_impedance_at_v(/out)"), 750.0), r.values
    c.ok("tf", "gain 0.25, Rin 4k, Rout 750")

    # .ac on RC 1k / 1u: H = 1 / (1 + j f/fc), fc = 159.155 Hz
    c.open("lowpass", low_pass())
    fc = 1 / (2 * math.pi * 1e3 * 1e-6)
    r = c.run(".ac dec 20 1 100k", signals=["V(out)"],
              measures=["f3db WHEN vdb(/out)=-3.0103", "ac lowgain FIND vm(/out) AT=1"])
    assert r.status == sim.SIM_OK and r.analysis == "AC", r
    frequency = r.vectors[0]
    out = c.vector(r, "V(/out)")
    assert frequency.kind == sim.VK_FREQUENCY and not frequency.complex and out.complex, (frequency, out)
    assert len(out.real) == len(out.imag) == len(frequency.real) == r.points == 101, r.points
    worst = 0.0
    for f, re, im in zip(frequency.real, out.real, out.imag):
        expected = 1 / (1 + 1j * f / fc)
        worst = max(worst, abs(complex(re, im) - expected))
    assert worst < 1e-9, worst
    measures = {m.name: m for m in r.measures}
    assert not measures["f3db"].failed and close(measures["f3db"].value, fc, rel=2e-3), measures["f3db"]
    assert close(measures["lowgain"].value, abs(1 / (1 + 1j / fc)), rel=1e-6), measures["lowgain"]
    assert all(v.name not in ("f3db", "lowgain") for v in r.values), "a measure is not a vector of the run"
    c.ok("ac low-pass", f"|H-H0|max={worst:.2e}, f3db={measures['f3db'].value:.4f} (closed form {fc:.4f})")

    # .tran: a 0 -> 1 V step, V(out) = 1 - exp(-t / RC)
    c.open("step", low_pass("dc 0 pulse(0 1 0 1n 1n 1 2)"))
    r = c.run(".tran 5u 5m", signals=["V(/out)"], directives=[".options reltol=1e-6"],
              measures=["tran t63 WHEN v(/out)=0.632120558"])
    assert r.status == sim.SIM_OK and r.analysis == "TRAN", r
    t = r.vectors[0]
    out = c.vector(r, "V(/out)")
    assert t.kind == sim.VK_TIME and len(t.real) == len(out.real) == r.points > 100, r.points
    worst = max(abs(v - (1 - math.exp(-max(x - 1e-9, 0) / 1e-3))) for x, v in zip(t.real, out.real))
    assert worst < 2e-3, worst
    assert close(r.measures[0].value, 1e-3, rel=5e-3), r.measures
    assert ".options reltol=1e-6" in c.run(".tran 5u 5m", directives=[".options reltol=1e-6"],
                                           include_netlist=True).netlist
    c.ok("tran step", f"max error {worst:.2e}, tau measured {r.measures[0].value:.6g}")

    # max_points: every Nth point, the first and the last kept
    full = len(t.real)
    r = c.run(".tran 5u 5m", signals=["V(/out)"], max_points=50)
    t2 = r.vectors[0]
    assert t2.length == r.points and 2 <= len(t2.real) <= 51, (len(t2.real), t2.length)
    assert t2.real[0] == 0.0 and close(t2.real[-1], 5e-3), (t2.real[0], t2.real[-1])
    assert len(c.vector(r, "V(/out)").real) == len(t2.real), "every vector is thinned alike"
    c.ok("max_points", f"{full} -> {len(t2.real)} points")

    # .noise: the output noise of an RC low-pass integrates to sqrt(kT/C)
    c.open("noise", low_pass())
    r = c.run(".noise v(/out) V1 dec 30 1 100meg", signals=["onoise_spectrum"])
    assert r.status == sim.SIM_OK and r.analysis == "NOISE", r
    expected = math.sqrt(BOLTZMANN * 300.15 / 1e-6)
    total = c.value(r, "onoise_total")
    assert close(total, expected, rel=2e-2), (total, expected)
    spectrum = c.vector(r, "onoise_spectrum")
    assert spectrum.kind == sim.VK_NOISE and r.vectors[0].kind == sim.VK_FREQUENCY, r.vectors[0]
    assert close(spectrum.real[0], math.sqrt(4 * BOLTZMANN * 300.15 * 1e3), rel=1e-2), spectrum.real[0]
    c.ok("noise", f"onoise_total={total:.4e} (sqrt(kT/C)={expected:.4e})")

    # .pz: one pole at -1/RC
    r = c.run(".pz /in 0 /out 0 vol pz", signals=["*"])
    assert r.status == sim.SIM_OK and r.analysis == "PZ", r
    pole = c.vector(r, "pole(1)")
    assert pole.kind == sim.VK_POLE and close(pole.real[0], -1000.0) and abs(pole.imag[0]) < 1e-6, pole
    c.ok("pz", f"pole {pole.real[0]:.6g} rad/s")

    # .sp: 50 ohm in series between two 50 ohm ports: S21 = 2/3, S11 = 1/3
    c.open("sp", [Part("V1", "dc 0 ac 1 portnum 1 z0 50", ["p1", "GND"]), Part("R1", "50", ["p1", "p2"]),
                  Part("V2", "dc 0 ac 0 portnum 2 z0 50", ["p2", "GND"])])
    r = c.run(".sp dec 2 1meg 100meg", signals=["s_2_1", "s_1_1"])
    assert r.status == sim.SIM_OK and r.analysis == "SP", r
    s21 = c.vector(r, "s_2_1")
    s11 = c.vector(r, "s_1_1")
    assert s21.kind == sim.VK_SPARAM and all(close(v, 2 / 3) for v in s21.real), s21
    assert all(close(v, 1 / 3) for v in s11.real) and all(abs(v) < 1e-9 for v in s21.imag), s11
    c.ok("sp", "S21=2/3 S11=1/3")

    # fft: a sine through the low-pass: its line is |H(f)|.  4096 samples hold 41 whole periods, so
    # the line sits on a bin (after 20 ms, when the filter has settled)
    f0 = 41 / (4096 * 10e-6)
    c.open("fft", low_pass(f"dc 0 sin(0 1 {f0:.6f})"))
    r = c.run(".tran 10u 60.95m 20m\nlinearize v(/out)\nfft v(/out)", signals=["V(out)"],
              directives=[".options reltol=1e-5"])
    assert r.status == sim.SIM_OK and r.analysis == "FFT", r
    frequency = r.vectors[0]
    out = c.vector(r, "V(/out)")
    assert frequency.kind == sim.VK_FREQUENCY and out.complex, (frequency.kind, out.complex)
    magnitude = [abs(complex(re, im)) for re, im in zip(out.real, out.imag)]
    peak = max(range(len(magnitude)), key=magnitude.__getitem__)
    expected = abs(1 / (1 + 1j * f0 / fc))
    assert close(frequency.real[peak], f0, rel=0.01), (frequency.real[peak], f0)
    assert close(magnitude[peak], expected, rel=0.03), (magnitude[peak], expected)
    c.ok("fft", f"line at {frequency.real[peak]:.0f} Hz, {magnitude[peak]:.4f} (|H|={expected:.4f})")


def check_run_controls(c):
    sim = c.sim
    c.open("controls", divider())

    # overrides: this run's netlist only
    r = c.run(".op", overrides=[sim.Override(reference="R2", value="3k")], include_netlist=True)
    assert r.status == sim.SIM_OK and close(c.value(r, "V(/out)"), 5.0), r.values
    assert "R2 /out GND 3k" in r.netlist, r.netlist
    r = c.run(".op", overrides=[sim.Override(reference="V1", field="dc", value="4")])
    assert close(c.value(r, "V(/out)"), 1.0), r.values
    r = c.run(".op")
    assert close(c.value(r, "V(/out)"), 2.5), "the schematic keeps its values"
    models = c.api.request(sim.GetSimulationModels(document=c.document, references=["R2"]),
                           sim.GetSimulationModelsResponse)
    assert models.models[0].value == "1k", models
    r = c.run(".op", overrides=[sim.Override(reference="R2", field="nonsense", value="1")])
    assert r.status in (sim.SIM_NETLIST_ERROR, sim.SIM_MODEL_ERROR), r
    assert any("R2" in p.message and "nonsense" in p.message for p in r.problems), r.problems
    c.ok("overrides", "Value, a Sim.Params key, an unknown key refused; schematic untouched")

    # a sweep by overrides: V(out) = 10 * R2 / (3k + R2)
    for ohms in (500, 1000, 3000, 9000):
        r = c.run(".op", overrides=[sim.Override(reference="R2", value=str(ohms))])
        assert close(c.value(r, "V(/out)"), 10 * ohms / (3000 + ohms)), (ohms, r.values)
    c.ok("override sweep", "4 values of R2")

    # signals: net names and V() forms are the same vector; an unknown one is a warning
    r = c.run(".dc V1 0 4 1", signals=["OUT", "v(/in)", "I(R1)", "V(nowhere)"])
    assert r.status == sim.SIM_OK, r
    assert len(c.vector(r, "V(/out)").real) == 5 and len(c.vector(r, "V(/in)").real) == 5, r.vectors
    assert len(c.vector(r, "I(R1)").real) == 5 and c.vector(r, "I(R1)").kind == sim.VK_CURRENT, r.vectors
    assert any("nowhere" in p.message and p.severity == sim.PS_WARNING for p in r.problems), r.problems
    r = c.run(".dc V1 0 4 1", signals=["*"])
    assert all(len(v.real) == 5 for v in r.vectors), "* returns every vector"
    c.ok("signals", "net name, V(), I(), *, unknown")

    # a failed measure is named, the run stays OK
    r = c.run(".dc V1 0 4 1", measures=["dc never WHEN v(/out)=100", "dc vmax MAX v(/out)"])
    assert r.status == sim.SIM_OK, r
    measures = {m.name: m for m in r.measures}
    assert measures["never"].failed and measures["never"].error and close(measures["vmax"].value, 1.0), measures
    assert c.problem(r, sim.PK_MEASURE_FAILED).severity == sim.PS_WARNING, r.problems
    c.ok("measures", "one value, one failure named")

    # bad requests
    for bad in (sim.RunSimulation(document=c.document, command="this is not spice"),
                sim.RunSimulation(document=c.document, command="fft v(/out)"),
                sim.RunSimulation(document=c.document, command=".op",
                                  overrides=[sim.Override(value="1")])):
        response = c.api.request_raw(bad)
        assert response.status.status == c.api.envelope.AS_BAD_REQUEST, response.status
    response = c.api.request_raw(sim.RunSimulation(document=c.document))
    assert response.status.status == c.api.envelope.AS_BAD_REQUEST and "saved" in response.status.error_message, \
        response.status
    c.ok("bad requests", "no analysis, fft alone, override without reference, no saved set-up")

    # a long run: SIM_RUNNING, a second run is AS_BUSY, the result is collected, a stop aborts
    c.open("long", low_pass("dc 0 sin(0 1 1k)"))
    started = time.monotonic()
    r = c.run(".tran 20n 60", wait_ms=50, timeout_ms=60000)
    assert r.status == sim.SIM_RUNNING and r.run_id, r
    assert time.monotonic() - started < 5, "the request returned without waiting for the run"
    run_id = r.run_id
    busy = c.api.request_raw(sim.RunSimulation(document=c.document, command=".op"))
    assert busy.status.status == c.api.envelope.AS_BUSY, busy.status
    version = c.api.module("common.commands.base_commands")
    c.api.request(version.GetVersion(), version.GetVersionResponse)
    again = c.api.request(sim.GetSimulationResult(document=c.document, run_id=run_id, wait_ms=20),
                          sim.RunSimulationResponse)
    assert again.status == sim.SIM_RUNNING and again.run_id == run_id, again
    stop = c.api.request(sim.StopSimulation(document=c.document, run_id=run_id), sim.StopSimulationResponse)
    assert stop.stopping, stop
    done = c.api.request(sim.GetSimulationResult(document=c.document, run_id=run_id, wait_ms=10000),
                         sim.RunSimulationResponse)
    assert done.status == sim.SIM_ABORTED and done.points > 10, (done.status, done.points)
    assert c.problem(done, sim.PK_ABORTED), done.problems
    assert not c.api.request(sim.StopSimulation(document=c.document), sim.StopSimulationResponse).stopping
    wrong = c.api.request_raw(sim.GetSimulationResult(document=c.document, run_id=run_id + 100))
    assert wrong.status.status == c.api.envelope.AS_BAD_REQUEST, wrong.status
    c.ok("running, busy, stop", f"aborted with {done.points} points, other requests served meanwhile")

    # the timeout stops the run by itself, whether or not anyone waits
    r = c.run(".tran 20n 60", wait_ms=50, timeout_ms=500)
    assert r.status == sim.SIM_RUNNING, r
    time.sleep(1.5)
    after = c.run(".op")
    assert after.status == sim.SIM_OK, "the timed-out run released the simulator"
    r = c.run(".tran 20n 60", wait_ms=5000, timeout_ms=400)
    assert r.status == sim.SIM_TIMEOUT and c.problem(r, sim.PK_TIMEOUT) and r.points > 10, r.status
    assert 300 < r.timings.run_ms < 3000, r.timings
    c.ok("timeout", f"stopped after {r.timings.run_ms:.0f} ms with {r.points} points")

    # a finished run that outlasted wait_ms is collected whole
    r = c.run(".tran 1u 200m", wait_ms=0, signals=["V(/out)"])
    result = r
    deadline = time.monotonic() + 60
    while result.status == sim.SIM_RUNNING and time.monotonic() < deadline:
        result = c.api.request(sim.GetSimulationResult(document=c.document, wait_ms=500),
                               sim.RunSimulationResponse)
    assert result.status == sim.SIM_OK and result.points > 1000 and len(c.vector(result, "V(/out)").real) > 1000
    c.ok("collect later", f"{result.points} points by GetSimulationResult")


def check_failures(c):
    sim = c.sim
    rc = low_pass()

    # a symbol with no model at all
    c.open("nomodel", rc + [Part("U1", "LM358", ["in", "out", "GND"])])
    r = c.run(".op")
    assert r.status == sim.SIM_MODEL_ERROR, r
    problem = c.problem(r, sim.PK_MISSING_MODEL)
    assert problem.reference == "U1" and problem.severity == sim.PS_ERROR, problem
    after = c.run(".op", overrides=[])  # still refused, and ngspice is still usable afterwards
    assert after.status == sim.SIM_MODEL_ERROR, after
    c.ok("missing model", problem.message)

    # the library file does not exist
    c.open("nolib", rc + [Part("D1", "D", ["out", "GND"],
                               fields={"Sim.Library": "missing.lib", "Sim.Name": "DCHK", "Sim.Device": "D",
                                       "Sim.Pins": "1=K 2=A"})])
    r = c.run(".op")
    problem = c.problem(r, sim.PK_LIBRARY_NOT_FOUND)
    assert r.status == sim.SIM_MODEL_ERROR and problem.reference == "D1" and "missing.lib" in problem.message, r
    c.ok("library not found", problem.message[:90])

    # the library exists, the model is not in it
    c.open("nomodelinlib", rc + [Part("D1", "D", ["out", "GND"],
                                      fields={"Sim.Library": "models.lib", "Sim.Name": "NOT_THERE",
                                              "Sim.Device": "D", "Sim.Pins": "1=K 2=A"})],
           files={"models.lib": MODELS_LIB})
    r = c.run(".op")
    problem = c.problem(r, sim.PK_MODEL_NOT_IN_LIBRARY)
    assert r.status == sim.SIM_MODEL_ERROR and problem.reference == "D1", r
    c.ok("model not in library", problem.message[:90])

    # raw SPICE naming a subcircuit nobody defines: ngspice says so, the symbol is named
    c.open("nosubckt", rc + [Part("U1", "LM358", ["in", "out", "GND"],
                                  fields={"Sim.Device": "SPICE", "Sim.Params": 'type="X" model="LM358"'})])
    r = c.run(".op")
    problem = c.problem(r, sim.PK_MISSING_MODEL)
    assert r.status == sim.SIM_MODEL_ERROR and problem.reference == "U1" and "lm358" in problem.message.lower(), r
    assert r.netlist, "a refused netlist comes back"
    c.ok("unknown subcircuit", problem.message[:90])

    # no convergence: a source that contradicts itself
    c.open("noconv", [Part("V1", "dc 5", ["in", "GND"]), Part("R3", "1k", ["in", "GND"]),
                      Part("B1", "B", ["x", "GND"],
                           fields={"Sim.Device": "SPICE",
                                   "Sim.Params": 'type="B" model="V=(v(/out)>2.5 ? 0 : 5)"'}),
                      Part("R1", "1k", ["x", "out"]), Part("R2", "1meg", ["out", "GND"])])
    r = c.run(".op")
    assert r.status == sim.SIM_NO_CONVERGENCE, r
    problems = [p for p in r.problems if p.kind == sim.PK_NO_CONVERGENCE]
    assert any(p.node in ("/out", "/x") and p.net == p.node for p in problems), problems  # one of the loop's nodes
    assert not r.vectors and not r.values and "Timestep too small" in r.log, r.log[-300:]
    c.ok("no convergence", [p.message for p in problems if p.node][0][:90])

    # a floating node: between two capacitors
    c.open("floating", [Part("V1", "dc 1", ["in", "GND"]), Part("C1", "1u", ["in", "mid"]),
                        Part("C2", "1u", ["mid", "GND"])])
    r = c.run(".op")
    assert r.status == sim.SIM_FLOATING_NODE, r
    problem = c.problem(r, sim.PK_FLOATING_NODE)
    assert problem.node == "/mid" and problem.net == "/mid" and problem.severity == sim.PS_ERROR, problem
    assert "C1" in problem.message and "C2" in problem.message, problem.message
    netlist = c.api.request(sim.GetSimulationNetlist(document=c.document, command=".op"),
                            sim.GetSimulationNetlistResponse)
    assert any(p.kind == sim.PK_FLOATING_NODE and p.node == "/mid" for p in netlist.problems), \
        "the netlist says so before any run"
    c.ok("floating node", problem.message[:90])

    # no ground at all: not run
    c.open("noground", [Part("V1", "dc 1", ["in", "ret"]), Part("R1", "1k", ["in", "ret"])])
    r = c.run(".op")
    assert r.status == sim.SIM_FLOATING_NODE and c.problem(r, sim.PK_NO_GROUND).severity == sim.PS_ERROR, r
    assert not r.values, r.values
    c.ok("no ground", c.problem(r, sim.PK_NO_GROUND).message[:70])

    # a singular matrix: two voltage sources in parallel
    c.open("singular", [Part("V1", "dc 1", ["in", "GND"]), Part("V2", "dc 2", ["in", "GND"]),
                        Part("R1", "1k", ["in", "GND"])])
    r = c.run(".op")
    assert r.status == sim.SIM_SINGULAR_MATRIX, r
    problem = c.problem(r, sim.PK_SINGULAR_MATRIX)
    assert problem.reference in ("V1", "V2") and problem.severity == sim.PS_ERROR, problem
    assert not any(p.kind == sim.PK_FLOATING_NODE for p in r.problems), r.problems
    c.ok("singular matrix", f"{problem.reference}: {problem.message[:70]}")

    # and a good circuit after all of them
    c.open("after", divider())
    assert close(c.value(c.run(".op"), "V(/out)"), 2.5)
    c.ok("recovers", "a good run after every failure")


def check_models(c):
    sim = c.sim
    parts = [Part("V1", "dc 5", ["in", "GND"]), Part("R1", "1k", ["in", "a"]),
             Part("D1", "D", ["GND", "a"]),                      # pin 1 = K, pin 2 = A
             Part("U1", "DIV", ["a", "half", "GND"]),
             Part("R9", "1k", ["half", "GND"]),
             Part("TP1", "TestPoint", ["half"])]
    c.open("models", parts, files={"sim/models.lib": MODELS_LIB})
    get = lambda **kw: c.api.request(sim.GetSimulationModels(document=c.document, **kw),
                                     sim.GetSimulationModelsResponse)
    assign = lambda **kw: c.api.request(sim.AssignSimulationModel(document=c.document, **kw),
                                        sim.AssignSimulationModelResponse)

    models = get()
    by_ref = {m.reference: m for m in models.models}
    assert set(by_ref) == {"V1", "R1", "D1", "U1", "R9", "TP1"} and models.symbols == 6, by_ref.keys()
    assert by_ref["R1"].source == sim.MS_INFERRED and by_ref["R1"].device == "R", by_ref["R1"]
    assert by_ref["R1"].error_kind == sim.ME_NONE and by_ref["R1"].netlist_line.strip() == "R1 /in /a 1k"
    assert [(p.pin_number, p.net) for p in by_ref["R1"].pins] == [("1", "/in"), ("2", "/a")], by_ref["R1"].pins
    for ref in ("D1", "U1", "TP1"):
        assert by_ref[ref].error_kind == sim.ME_NO_MODEL and by_ref[ref].source == sim.MS_NONE, by_ref[ref]
    assert models.problems == 3 and [m.reference for m in get(only_problems=True).models] == ["D1", "TP1", "U1"]
    assert get(references=["R1", "R77"]).not_found == ["R77"]
    c.ok("GetSimulationModels", "inferred passives, 3 parts without a model")

    # a library .model with an explicit pin map (the symbol's pin 1 is the cathode)
    r = assign(references=["D1"], library=sim.LibraryModel(library="sim/models.lib", name="DCHK"),
               pins=[sim.PinMap(pin_number="1", model_node="K"), sim.PinMap(pin_number="2", model_node="A")])
    result = r.results[0]
    assert r.changed == 1 and r.failed == 0 and result.changed and not result.error, r
    fields = {f.name: f.value for f in result.model.fields}
    assert fields["Sim.Library"] == "sim/models.lib" and fields["Sim.Name"] == "DCHK", fields
    assert fields["Sim.Device"] == "D" and fields["Sim.Pins"] == "1=K 2=A", fields
    assert result.model.source == sim.MS_LIBRARY and result.model.library_path.endswith("sim/models.lib")
    c.ok("assign library .model", str(fields))

    # a .subckt, pins by order
    r = assign(references=["U1"], library=sim.LibraryModel(library="sim/models.lib", name="DIVCHK"))
    fields = {f.name: f.value for f in r.results[0].model.fields}
    assert fields["Sim.Device"] == "SUBCKT" and fields["Sim.Pins"] == "1=in 2=out 3=gnd", fields
    assert [(p.pin_number, p.model_node) for p in r.results[0].model.pins] == \
        [("1", "in"), ("2", "out"), ("3", "gnd")], r.results[0].model.pins
    c.ok("assign .subckt", fields["Sim.Pins"])

    # the test point does not simulate
    r = assign(references=["TP1"], exclude_from_sim=True)
    assert r.changed == 1 and r.results[0].model.exclude_from_sim and r.results[0].model.error_kind == sim.ME_NONE
    assert assign(references=["TP1"], exclude_from_sim=True).changed == 0, "the same again changes nothing"
    assert get().problems == 0
    c.ok("exclude_from_sim", "TP1 excluded, no problems left")

    # it simulates: the diode conducts, the divider halves V(a) (loaded by R9: 1k || 1k below 1k)
    r = c.run(".op")
    assert r.status == sim.SIM_OK, r
    va = c.value(r, "V(/a)")
    assert 0.55 < va < 0.75, va
    assert close(c.value(r, "V(/half)"), va / 3, rel=1e-6), (c.value(r, "V(/half)"), va)
    c.ok("assigned models simulate", f"V(a)={va:.4f} V(half)={c.value(r, 'V(/half)'):.4f}")

    # built-in devices and their parameters
    r = assign(references=["R9"], builtin=sim.BuiltinModel(device="R", params=[sim.Param(name="r", value="2k")]))
    assert r.changed == 1 and r.results[0].model.value == "2k", r.results[0]
    r = assign(references=["V1"], builtin=sim.BuiltinModel(
        device="V", type="SIN", params=[sim.Param(name="dc", value="5"), sim.Param(name="ampl", value="1"),
                                        sim.Param(name="f", value="1k")]))
    model = r.results[0].model
    fields = {f.name: f.value for f in model.fields}
    assert model.device == "V" and model.type == "SIN" and fields["Sim.Type"] == "SIN", model
    assert "ampl=1" in fields["Sim.Params"] and "f=1k" in fields["Sim.Params"], fields
    with_params = get(references=["V1"], include_params=True).models[0]
    params = {p.name: p for p in with_params.params}
    assert params["ampl"].value == "1" and params["f"].unit == "Hz", params.get("f")
    c.ok("assign built-in", f"R9=2k, V1 {fields['Sim.Params']}")

    # raw SPICE
    r = assign(references=["R1"], raw=sim.RawSpiceModel(primitive="R", model="1k tc1=0.001"))
    model = r.results[0].model
    assert model.source == sim.MS_RAW and model.device == "SPICE", model
    line = get(references=["R1"]).models[0].netlist_line
    assert "1k tc1=0.001" in line, line
    c.ok("assign raw SPICE", line.strip())

    # refused, and nothing written when any reference fails
    before = get()
    r = assign(references=["R9", "D1", "R404"], library=sim.LibraryModel(library="sim/models.lib", name="NOPE"))
    kinds = [result.error_kind for result in r.results]
    assert r.changed == 0 and r.failed == 3 and kinds[0] == sim.ME_MODEL_NOT_IN_LIBRARY and kinds[2] == sim.ME_OTHER
    assert "DCHK" in r.results[0].error, "the error lists what the library has"
    r = assign(references=["D1"], library=sim.LibraryModel(library="sim/other.lib", name="DCHK"))
    assert r.results[0].error_kind == sim.ME_LIBRARY_NOT_FOUND, r.results[0]
    r = assign(references=["D1"], library=sim.LibraryModel(library="sim/models.lib", name="DCHK"),
               pins=[sim.PinMap(pin_number="1", model_node="ZZ")])
    assert r.results[0].error_kind == sim.ME_PIN_MISMATCH and "K" in r.results[0].error, r.results[0]
    r = assign(references=["D1"], library=sim.LibraryModel(library="sim/models.lib", name="DIVCHK"))
    assert r.results[0].error_kind == sim.ME_PIN_MISMATCH, "3 nodes on a 2-pin symbol"
    r = assign(references=["R9"], builtin=sim.BuiltinModel(device="R", params=[sim.Param(name="zz", value="1")]))
    assert r.results[0].error_kind == sim.ME_BAD_PARAMS, r.results[0]
    r = assign(references=["R9", "R404"], builtin=sim.BuiltinModel(device="R", params=[sim.Param(name="r", value="5k")]))
    assert r.changed == 0 and r.failed == 1, "all or nothing"
    assert get() == before, "a refused assignment writes nothing"
    r = assign(references=["R9", "R404"], partial=True,
               builtin=sim.BuiltinModel(device="R", params=[sim.Param(name="r", value="5k")]))
    assert r.changed == 1 and r.failed == 1 and get(references=["R9"]).models[0].value == "5k"
    bad = c.api.request_raw(sim.AssignSimulationModel(document=c.document, references=["R9"]))
    assert bad.status.status == c.api.envelope.AS_BAD_REQUEST, bad.status
    c.ok("assign refusals", "unknown model, library, node, pin count, parameter, reference; all-or-nothing; partial")

    # saved, reopened: the same models; cleared: back to no model
    expected = get()
    c.api.request(c.project.SaveDocument(document=c.document), Empty)
    text = c.path.read_text()
    assert '"Sim.Library" "sim/models.lib"' in text and "(exclude_from_sim yes)" in text, "saved in the schematic"
    c.api.request(c.project.CloseDocument(document=c.document), Empty)
    c.document = c.api.request(c.project.OpenDocument(type=c.types.DOCTYPE_SCHEMATIC, path=str(c.path)),
                               c.project.OpenDocumentResponse).document
    reopened = get()
    assert [(m.reference, m.source, m.model_name, m.exclude_from_sim, list(m.fields)) for m in reopened.models] == \
        [(m.reference, m.source, m.model_name, m.exclude_from_sim, list(m.fields)) for m in expected.models]
    r = assign(references=["D1"], clear=True)
    assert r.changed == 1 and not r.results[0].model.fields and r.results[0].model.error_kind == sim.ME_NO_MODEL
    c.ok("save and reload", "Sim.* fields and the exclusion are in the file; clear removes them")


def check_setup(c):
    sim = c.sim
    c.open("setup", low_pass(), texts=[".tran 10u 2m"])
    get = lambda: c.api.request(sim.GetSimulationSetup(document=c.document), sim.SetupResponse).setup
    put = lambda **kw: c.api.request(sim.SetSimulationSetup(document=c.document, **kw), sim.SetupResponse)

    setup = get()
    assert not setup.tabs and not setup.workbook and setup.schematic_command == ".tran 10u 2m", setup
    assert setup.workbook_path.endswith("setup.wbk"), setup.workbook_path
    r = c.run()   # no command: the analysis written on the sheet
    assert r.status == sim.SIM_OK and r.analysis == "TRAN" and r.command == ".tran 10u 2m", r.command
    c.ok("schematic directive", "the sheet's .tran is the default analysis")

    answer = put(command=".ac dec 10 1 100k", signals=["V(/out)"],
                 measurements=[sim.Measurement(expr="MAX V(/out)", format="3~V")])
    assert answer.changed and answer.tab == 0 and len(answer.setup.tabs) == 1, answer
    tab = answer.setup.tabs[0]
    assert tab.analysis == "AC" and tab.command == ".ac dec 10 1 100k" and list(tab.signals) == ["V(/out)"], tab
    assert tab.measurements[0].expr == "MAX V(/out)" and tab.options.save_all_currents, tab
    assert answer.setup.workbook == "setup.wbk", answer.setup.workbook
    workbook = json.loads(Path(answer.setup.workbook_path).read_text())
    assert workbook["version"] == 8 and workbook["tabs"][0]["analysis"] == "AC", workbook
    assert workbook["tabs"][0]["commands"][0] == ".ac dec 10 1 100k", workbook["tabs"][0]
    assert ".save all" in workbook["tabs"][0]["commands"] and ".probe alli" in workbook["tabs"][0]["commands"]
    assert workbook["tabs"][0]["traces"][0]["signal"] == "V(/out)", workbook["tabs"][0]["traces"]
    project_file = json.loads(c.path.with_suffix(".kicad_pro").read_text())
    assert project_file["schematic"]["ngspice"]["workbook_filename"] == "setup.wbk", project_file.get("schematic")
    c.ok("SetSimulationSetup", "workbook v8 written, named in the project")

    assert not put(command=".ac dec 10 1 100k", signals=["V(/out)"],
                   measurements=[sim.Measurement(expr="MAX V(/out)", format="3~V")]).changed, "idempotent"
    answer = put(command=".ac dec 20 10 10k")           # same analysis: the same tab, traces kept
    assert answer.tab == 0 and len(answer.setup.tabs) == 1 and list(answer.setup.tabs[0].signals) == ["V(/out)"]
    answer = put(command=".tran 1u 5m", signals=["V(/out)", "I(R1)"],
                 options=sim.Options(save_all_currents=False))
    assert answer.tab == 1 and len(answer.setup.tabs) == 2 and not answer.setup.tabs[1].options.save_all_currents
    answer = put(command=".op", tab=0)
    assert [t.analysis for t in answer.setup.tabs] == ["OP", "TRAN"], answer.setup.tabs
    c.ok("tabs", "replace by analysis, append, replace by index")

    r = c.run()   # no command: the saved set-up's first tab
    assert r.status == sim.SIM_OK and r.analysis == "OP", r.analysis
    answer = put(remove=True, tab=0)
    assert [t.analysis for t in answer.setup.tabs] == ["TRAN"], answer.setup.tabs
    r = c.run()
    assert r.analysis == "TRAN" and r.command == ".tran 1u 5m", r.command
    netlist = c.api.request(sim.GetSimulationNetlist(document=c.document), sim.GetSimulationNetlistResponse)
    assert netlist.command == ".tran 1u 5m" and ".probe alli" not in netlist.netlist, netlist.netlist
    c.ok("saved set-up runs", "RunSimulation and GetSimulationNetlist default to it, with its options")

    answer = put(compatibility="ngspice")
    assert answer.setup.compatibility == "ngspice" and get().compatibility == "ngspice"
    for bad in (dict(command="hello"), dict(remove=True, tab=7), dict(command=".op", tab=9),
                dict(compatibility="spectre"), dict()):
        response = c.api.request_raw(sim.SetSimulationSetup(document=c.document, **bad))
        assert response.status.status == c.api.envelope.AS_BAD_REQUEST, (bad, response.status)
    c.ok("compatibility and refusals", "ngspice mode saved; bad command, tab, mode refused")

    # the netlist: nodes, pin currents, directives
    netlist = c.api.request(sim.GetSimulationNetlist(
        document=c.document, command=".op", directives=[".param gain=2"],
        overrides=[sim.Override(reference="C1", value="2u")]), sim.GetSimulationNetlistResponse)
    assert {(n.net, n.node) for n in netlist.nodes} >= {("/in", "/in"), ("/out", "/out")}, netlist.nodes
    assert any(n.node == "gnd" for n in netlist.nodes), netlist.nodes
    currents = {(p.reference, p.signal, p.vector) for p in netlist.pin_currents}
    assert ("R1", "I(R1)", "i(r1)") in currents, currents
    assert ".param gain=2" in netlist.netlist and "C1 /out GND 2u" in netlist.netlist, netlist.netlist
    assert netlist.items == 3 and list(netlist.directives) == [".tran 10u 2m"] and not netlist.problems, netlist
    c.ok("GetSimulationNetlist", "nodes, pin currents, directives, overrides")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("cli", type=Path)
    parser.add_argument("--out", type=Path, default=None)
    args = parser.parse_args()
    with IpcSession(args.cli, timeout=120000, client_name="works.backplane.sim-check") as api:
        capabilities = api.module("backplane.capabilities")
        have = api.request(capabilities.GetCapabilities(), capabilities.GetCapabilitiesResponse)
        assert "sim-run" in have.capabilities, list(have.capabilities)
        checker = Checker(api, args.out)
        checker.ok("capability", "sim-run")
        check_analyses(checker)
        check_run_controls(checker)
        check_failures(checker)
        check_models(checker)
        check_setup(checker)
        if args.out:
            args.out.mkdir(parents=True, exist_ok=True)
            (args.out / "sim-headless.json").write_text(json.dumps(checker.report, indent=2) + "\n")
        print(f"SIM_HEADLESS_OK {checker.count} checks", flush=True)


if __name__ == "__main__":
    sys.exit(main())
