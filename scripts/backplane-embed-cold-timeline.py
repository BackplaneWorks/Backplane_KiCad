#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Cold start of `kicad --embedded` on a real project, with the hub's marks.
    cold-timeline.py <scripts dir> <kicad> <kicad-cli> <project dir> <out dir> <share> <mode> [runs]
mode "hub": as the hub does today (first ShowFrame, sleep 1150 ms, HideFrame, ShowFrame again).
mode "event": first ShowFrame, then ready on ET_FRAME_DRAWN (28), capped at 1500 ms.
Marks are ms after the process started (/proc start time): socket, answering, show_sent,
show_replied (= the hub's "first show"), mapped, drawn, ready.  Spans come from
BACKPLANE_KICAD_FRAMETRACE."""
import glob, json, os, sys, threading, time
from pathlib import Path
sys.path.insert(0, sys.argv[1])
from backplane_embed_test_support import IpcSession, Session, viewable, wait_for
import pynng

kicad, cli, project, out, share = map(Path, sys.argv[2:7])
mode = sys.argv[7]
runs = int(sys.argv[8]) if len(sys.argv) > 8 else 3
for var, sub in (("KICAD10_SYMBOL_DIR", "symbols"), ("KICAD10_FOOTPRINT_DIR", "footprints"),
                 ("KICAD10_3DMODEL_DIR", "3dmodels"), ("KICAD10_TEMPLATE_DIR", "template")):
    os.environ[var] = str(share / sub)
orig_write = Path.write_text
def seeded_write(self, data, *a, **k):
    if self.name in ("sym-lib-table", "fp-lib-table", "design-block-lib-table") and (share / self.name).is_file():
        data = (share / self.name).read_text()
    return orig_write(self, data, *a, **k)

def proc_start(pid):
    ticks = os.sysconf("SC_CLK_TCK")
    btime = next(int(l.split()[1]) for l in open("/proc/stat") if l.startswith("btime"))
    start = int(open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()[19])
    return btime + start / ticks

DRAWN = 28
results = []
for run in range(runs):
    o = out / f"{mode}-{run}"
    o.mkdir(parents=True, exist_ok=True)
    trace = o / "frametrace.jsonl"
    trace.unlink(missing_ok=True)
    os.environ["BACKPLANE_KICAD_FRAMETRACE"] = str(trace)
    marks = {}
    before = set(glob.glob("/tmp/backplane-embed-*"))
    stop = [False]
    def socket_watch():
        while not stop[0] and "socket" not in marks:
            for d in set(glob.glob("/tmp/backplane-embed-*")) - before:
                if Path(d, "run/kicad/api.sock").exists():
                    marks["socket"] = time.time(); return
            time.sleep(0.002)
    threading.Thread(target=socket_watch, daemon=True).start()
    Path.write_text = seeded_write
    with Session(kicad, o, project) as s:
        Path.write_text = orig_write
        t0 = proc_start(s.kicad_process.pid)
        with IpcSession(cli, api_socket=s.socket, timeout=300000) as api:
            marks["answering"] = time.time()   # IpcSession's handshake answered
            e = api.module("backplane.embed")
            gf = api.request(e.GetFrames(), e.GetFramesResponse)
            drawn = []
            def events():
                since = 0
                with pynng.Req0(dial=f"ipc://{s.socket}.events", recv_timeout=70000) as poll:
                    while not stop[0]:
                      try:
                        req = api.envelope.ApiRequest(); req.message.Pack(e.NextEvents(since=since, wait_ms=1000))
                        poll.send(req.SerializeToString())
                        resp = api.envelope.ApiResponse.FromString(poll.recv())
                        r = e.NextEventsResponse(); resp.message.Unpack(r)
                        for ev in r.events:
                            since = max(since, ev.seq)
                            if int(ev.type) == DRAWN:
                                drawn.append((time.time(), ev.detail))
                      except Exception:
                        return
            threading.Thread(target=events, daemon=True).start()
            w, h = s.host.size
            marks["show_sent"] = time.time()
            r = api.request(e.ShowFrame(kind=e.FK_PCB, x=0, y=0, width=w, height=h), e.ShowFrameResponse)
            marks["show_replied"] = time.time()
            win = r.frame.window
            def mapped():
                if viewable(s.host.display, win):
                    marks.setdefault("mapped", time.time()); return True
                return False
            if mode == "hub":
                end = time.time() + 1.15
                while time.time() < end:
                    mapped(); time.sleep(0.005)
                api.request(e.HideFrame(window=win), e.HideFrameResponse)
                api.request(e.ShowFrame(kind=e.FK_PCB, x=0, y=0, width=w, height=h), e.ShowFrameResponse)
                marks["ready"] = time.time()
                wait_for(mapped, 30, 0.005)
            else:
                cap = time.time() + 1.5
                while time.time() < cap and not drawn:
                    mapped(); time.sleep(0.003)
                marks["ready"] = drawn[0][0] if drawn else time.time()
                marks["ready_by"] = "event" if drawn else "cap"
                wait_for(mapped, 30, 0.005)
            time.sleep(1.0)
            if drawn:
                marks["drawn"] = drawn[0][0]; marks["drawn_detail"] = drawn[0][1]
            stop[0] = True
    row = {k: (round((v - t0) * 1000) if isinstance(v, float) else v) for k, v in marks.items()}
    spans = []
    if trace.is_file():
        for line in trace.read_text().splitlines():
            try:
                j = json.loads(line)
            except ValueError:
                continue
            if j.get("kind") == "span" and (j["what"].startswith(("open:", "create:", "ctor:", "embed:open", "embed:show"))
                                            or j["ms"] >= 50):
                spans.append((round(j["t"] - t0 * 1000), round(j["ms"]), j["what"]))
            if j.get("kind") == "loop" and j["ms"] >= 100:
                spans.append((round(j["t"] - t0 * 1000), round(j["ms"]), "LOOP " + ",".join(j["ran"])[:200]))
    row["spans"] = sorted(spans)
    results.append(row)
    print(json.dumps({k: v for k, v in row.items() if k != "spans"}), flush=True)
    for sp in row["spans"]:
        print("   ", sp, flush=True)
(out / f"{mode}.json").write_text(json.dumps(results, indent=1))
