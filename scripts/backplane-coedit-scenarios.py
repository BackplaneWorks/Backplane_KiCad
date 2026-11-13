#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Co-editing scenarios (docs/backplane/coedit.md), on an embedded KiCad on a private X server.

1. collision: the agent reads U-footprint, the person moves it (M, in the editor), then the
   agent moves it from its old read: exactly one prompt ("Agent ... wants to move ...; you
   changed it ..."), nothing applied for the colliding field until answered; "Take agent's"
   applies it.  A non-colliding agent change to the same item (another field) applies silently.
2. parallel: a long agent workflow (tracks, moves of its parts, commits) while the person moves
   other parts: no prompt, no busy, no lost work on either side.
3. rules and settings: the agent edits one custom rule over IPC while another writer (the
   person's other KiCad, a script) edits another rule in the file: both survive and the live
   DRC engine has both; an agent net class (IPC) and a net class written into the project file
   meanwhile both survive the board's save.

    backplane-coedit-scenarios.py KICAD KICAD_CLI --out DIR [--only collision,parallel,rules]
"""

import argparse
import json
from pathlib import Path
import random
import re
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_coedit_support import EmbeddedKiCad  # noqa: E402

FAILURES = []


def check(condition, message):
    print(("PASS " if condition else "FAIL ") + message, flush=True)
    if not condition:
        FAILURES.append(message)
    return condition


def footprints(api, k):
    m = k.m
    req = m["editor"].GetItems()
    req.header.document.CopyFrom(k.doc)
    req.types.append(m["enums"].KOT_PCB_FOOTPRINT)
    out = {}
    for item in api.request(req, m["editor"].GetItemsResponse).items:
        fp = m["board"].FootprintInstance()
        item.Unpack(fp)
        out[fp.reference_field.text.text.text] = fp
    return out


def holds(k):
    return k.host_api.request(k.m["coedit"].GetHolds(document=k.doc), k.m["coedit"].GetHoldsResponse)


def update(api, k, fp):
    req = k.m["editor"].UpdateItems()
    req.header.document.CopyFrom(k.doc)
    req.items.add().Pack(fp)
    return api.request_raw(req)


def person_moves(k, ref, dx=40, dy=25):
    """The person moves a footprint with M in the editor (a real interactive move)."""
    embed = k.m["embed"]
    k.host_api.request(embed.SelectItems(kind=embed.FK_PCB, references=[ref], zoom=True),
                       embed.SelectItemsResponse)
    time.sleep(0.5)
    w, h = k.host.size
    cx, cy = w // 2, h // 2
    k.xdotool("mousemove", cx, cy)
    k.xdotool("key", "m")
    for step in range(1, 9):
        k.xdotool("mousemove", cx + dx * step // 8, cy + dy * step // 8)
        time.sleep(0.06)
    k.xdotool("click", "1")
    time.sleep(0.4)
    k.xdotool("key", "Escape")
    time.sleep(0.3)


def scenario_collision(k):
    agent = k.session("agent:tcollide")
    fps = footprints(agent, k)
    refs = sorted(r for r in fps if re.match(r"^[A-Z]+\d+$", r))
    ref, other = refs[0], refs[1]
    seen = fps[ref]
    start = (seen.position.x_nm, seen.position.y_nm)

    person_moves(k, ref)
    moved = footprints(k.host_api, k)[ref]
    person_pos = (moved.position.x_nm, moved.position.y_nm)
    check(person_pos != start, f"the person moved {ref}")

    # The agent moves it too, from its read before the person's move.
    stale = type(seen)()
    stale.CopyFrom(seen)
    stale.position.x_nm += 2_000_000
    r = update(agent, k, stale)
    check(r.status.status == agent.envelope.AS_OK, "the agent's update is accepted (not busy)")

    h = holds(k)
    check(h.collisions == 1, f"exactly one prompt for the person ({h.collisions})")
    check(ref in h.collision and "move" in h.collision, f"the prompt names the move of {ref}: {h.collision!r}")
    now = footprints(k.host_api, k)[ref]
    check((now.position.x_nm, now.position.y_nm) == person_pos,
          "until answered, the person's position stays")
    k.shot("collision-prompt")

    coedit = k.m["coedit"]
    k.host_api.request(coedit.ResolveCollision(document=k.doc, choice=coedit.CC_SEE_BOTH),
                       coedit.ResolveCollisionResponse)
    time.sleep(0.5)
    k.shot("collision-see-both")
    left = k.host_api.request(coedit.ResolveCollision(document=k.doc, choice=coedit.CC_TAKE_AGENTS),
                              coedit.ResolveCollisionResponse).remaining
    check(left == 0, "answered: no prompt left")
    now = footprints(k.host_api, k)[ref]
    check(now.position.x_nm == stale.position.x_nm, "Take agent's applied the agent's position")

    # A different field of a part the person moved: merged, no prompt.
    person_moves(k, other, -30, 20)
    fresh_read = footprints(agent, k)[other]   # agent reads after the person's move...
    person_moves(k, other, 20, -15)            # ...and the person moves it again
    renamed = type(fresh_read)()
    renamed.CopyFrom(fresh_read)
    renamed.locked = 2 if renamed.locked != 2 else 1   # an unrelated field
    update(agent, k, renamed)
    h = holds(k)
    after = footprints(k.host_api, k)[other]
    check(h.collisions == 0, "a different field: no prompt")
    check((after.position.x_nm, after.position.y_nm) != (fresh_read.position.x_nm, fresh_read.position.y_nm),
          "and the person's latest move stays")


def scenario_parallel(k, seconds):
    m = k.m
    agent = k.session("agent:tlong")
    fps = footprints(agent, k)
    refs = sorted(r for r in fps if re.match(r"^[A-Z]+\d+$", r))
    random.Random(4).shuffle(refs)
    mine, persons = refs[: len(refs) // 2], refs[len(refs) // 2:]
    created, stop, stats = [], threading.Event(), {"busy": 0, "errors": 0}
    agent_moves = {}
    history = {}

    def run():
        step.rng = random.Random(7)
        while not stop.is_set():
            begin = m["editor"].BeginCommit()
            begin.header.document.CopyFrom(k.doc)
            r = agent.request_raw(begin)
            if r.status.status != agent.envelope.AS_OK:
                stats["busy" if r.status.status == agent.envelope.AS_BUSY else "errors"] += 1
                time.sleep(0.2)
                continue
            opened = m["editor"].BeginCommitResponse()
            r.message.Unpack(opened)
            try:
                step(opened)
            except Exception as e:  # noqa: BLE001
                stats["errors"] += 1
                print("agent step failed:", e, flush=True)
                drop = m["editor"].EndCommit()
                drop.header.document.CopyFrom(k.doc)
                drop.action = m["editor"].CMA_DROP
                agent.request_raw(drop)
                continue
            end = m["editor"].EndCommit()
            end.header.document.CopyFrom(k.doc)
            end.id.CopyFrom(opened.id)
            end.action = m["editor"].CMA_COMMIT
            end.message = "Agent: long workflow step"
            agent.request_raw(end)
            time.sleep(0.1)

    def step(opened):
        rng = step.rng
        if True:
            for _ in range(4):
                t = m["board"].Track()
                x, y = rng.randint(100, 190) * 1_000_000, rng.randint(60, 140) * 1_000_000
                t.start.x_nm, t.start.y_nm, t.end.x_nm, t.end.y_nm = x, y, x + 3_000_000, y
                t.width.value_nm, t.layer = 250_000, m["board"].BL_B_Cu
                req = m["editor"].CreateItems()
                req.header.document.CopyFrom(k.doc)
                req.items.add().Pack(t)
                rr = agent.request_raw(req)
                res = m["editor"].CreateItemsResponse()
                rr.message.Unpack(res)
                for c in res.created_items:
                    tt = m["board"].Track()
                    c.item.Unpack(tt)
                    created.append(tt.id.value)
                ref = rng.choice(mine)
                fp = footprints(agent, k)[ref]
                fp.position.x_nm += 100_000
                rr = update(agent, k, fp)
                if rr.status.status == agent.envelope.AS_BUSY:
                    stats["busy"] += 1
                res = m["editor"].UpdateItemsResponse()
                rr.message.Unpack(res)
                code = m["editor"].ItemStatusCode.Name(res.updated_items[0].status.code) if res.updated_items else "?"
                history.setdefault(ref, []).append((fp.position.x_nm - 100_000, fp.position.x_nm, code))
                agent_moves[ref] = fp.position.x_nm

    before = {r: (fps[r].position.x_nm, fps[r].position.y_nm) for r in persons}
    agent.request_raw(m["coedit"].SetAgentActivity(actor="agent:tlong", task="Reroute the B.Cu bus",
                                                    state="working"))
    worker = threading.Thread(target=run)
    worker.start()
    start, moved = time.monotonic(), set()
    person_pos = {}
    while time.monotonic() - start < seconds:
        ref = persons[len(moved) % len(persons)]
        if len(moved) == 6:
            k.host_api.request(m["embed"].SelectItems(kind=m["embed"].FK_PCB, references=[], zoom=False),
                               m["embed"].SelectItemsResponse)
            k.xdotool("key", "Home")   # zoom to fit: the agent's working area and its chip
            time.sleep(1.0)
            k.shot("agents-working")
        person_moves(k, ref, 30, 20)
        now_fp = footprints(k.host_api, k)[ref]
        pos = (now_fp.position.x_nm, now_fp.position.y_nm)
        if pos != before[ref]:
            moved.add(ref)            # the GUI move took; it must stay
            person_pos[ref] = pos
        else:
            moved.add(ref + "?")       # did not take (GUI timing): not counted
    stop.set()
    worker.join(60)

    k.xdotool("key", "Escape")
    for _ in range(300):
        if holds(k).deferred == 0:
            break
        time.sleep(0.1)
    h = holds(k)
    if h.deferred:
        print(f"still held: reason={h.reason!r} defer_deletes={h.defer_deletes} items={len(h.items)} "
              f"deferred={h.deferred}", flush=True)
    check(h.collisions == 0, f"no prompt during the long workflow ({h.collisions})")
    check(stats["busy"] == 0 and stats["errors"] == 0, f"no busy or errors for the agent ({stats})")

    now = footprints(k.host_api, k)
    req = k.m["editor"].GetItems()
    req.header.document.CopyFrom(k.doc)
    req.types.append(k.m["enums"].KOT_PCB_FOOTPRINT)
    count = len(k.host_api.request(req, k.m["editor"].GetItemsResponse).items)
    check(count == len(fps), f"no footprint duplicated or lost ({count} on the board, {len(fps)} before)")
    lost_person = [r for r, p in person_pos.items() if (now[r].position.x_nm, now[r].position.y_nm) != p]
    check(not lost_person, f"every person move kept ({len(person_pos)} moved, lost: {lost_person})")
    notes = agent.request(m["coedit"].GetNotices(actor="agent:tlong"), m["coedit"].GetNoticesResponse).notices
    failed = [n.json for n in notes if '"failed"' in n.json or n.kind == "dropped"]
    for f in failed[:5]:
        print("  expected x? failed/dropped:", f[:300], flush=True)
    lost_agent = [r for r, x in agent_moves.items() if now[r].position.x_nm != x]
    for r in lost_agent[:2]:
        print(f"  history {r}: " + "; ".join(f"read {a} sent {b} {c}" for a, b, c in history.get(r, [])), flush=True)
    for r in lost_agent[:6]:
        print(f"  {r}: expected x={agent_moves[r]} now x={now[r].position.x_nm} "
              f"(diff {(now[r].position.x_nm - agent_moves[r]) / 1e6:+.3f} mm)", flush=True)
    check(not lost_agent, f"every agent move kept ({len(agent_moves)} parts, lost: {lost_agent})")

    req = m["editor"].GetItems()
    req.header.document.CopyFrom(k.doc)
    req.types.append(m["enums"].KOT_PCB_TRACE)
    ids = set()
    for item in k.host_api.request(req, m["editor"].GetItemsResponse).items:
        t = m["board"].Track()
        item.Unpack(t)
        ids.add(t.id.value)
    missing = [c for c in created if c not in ids]
    check(created and not missing, f"every agent track is there ({len(created)} made, {len(missing)} missing)")
    k.shot("parallel-end")


def scenario_rules(k):
    m = k.m
    agent = k.session("agent:trules")
    rules_file = k.board_file.with_suffix(".kicad_dru")
    rules_file.write_text('(version 2)\n\n# the person\'s HV rule\n(rule "HV"\n  (constraint clearance (min 1mm)))\n\n'
                          '(rule "Agent-gap"\n  (constraint clearance (min 0.2mm)))\n')
    time.sleep(1.0)  # the editor picks up the file

    # The agent reads the rules, then changes its own rule over IPC.
    read = agent.request(m["bcmd"].GetCustomDesignRules(board=k.doc), m["bcmd"].CustomRulesResponse)
    check(len(read.rules) == 2, f"the agent reads both rules ({len(read.rules)})")

    # Meanwhile the person's rule changes in the file (their other editor, a script).
    rules_file.write_text(rules_file.read_text().replace("(min 1mm)", "(min 1.5mm)"))

    rule = m["rules"].CustomRule(name="Agent-gap")
    c = rule.constraints.add()
    c.type = m["rules"].CRCT_CLEARANCE
    c.numeric.min = 300_000
    edit = m["coedit"].EditCustomRules(board=k.doc)
    edit.upsert.add().CopyFrom(rule)
    r = agent.request_raw(edit)
    check(r.status.status == agent.envelope.AS_OK, "EditCustomRules accepted")

    text = rules_file.read_text()
    check("1.5mm" in text and "the person's HV rule" in text, "the person's rule and comment survive")
    check(re.search(r'Agent-gap[\s\S]*0\.3', text) is not None, "the agent's rule change is in the file")

    # A new rule can go first (KiCad lets a later rule win: a fab section the person's rules
    # override), or above a named rule; an unknown name is refused.
    fab = m["rules"].CustomRule(name="Fab-track")
    fc = fab.constraints.add()
    fc.type = m["rules"].CRCT_TRACK_WIDTH
    fc.numeric.min = 127_000
    first = m["coedit"].EditCustomRules(board=k.doc, insert=m["coedit"].RIP_FIRST)
    first.upsert.add().CopyFrom(fab)
    agent.request_raw(first)
    text = rules_file.read_text()
    check(text.find("Fab-track") < text.find("the person's HV rule") and "1.5mm" in text,
          "EditCustomRules insert=RIP_FIRST puts the new rule above the person's")
    bad = m["coedit"].EditCustomRules(board=k.doc, insert=m["coedit"].RIP_BEFORE,
                                      insert_before="No-such-rule")
    bad.upsert.add().CopyFrom(fab)
    r = agent.request(bad, m["bcmd"].CustomRulesResponse)
    check(r.status == m["bcmd"].CRS_INVALID and "No-such-rule" in r.error_text,
          "insert_before an unknown rule is refused")

    # SetCustomDesignRules from the stale read must not undo the person's edit either.
    stale = m["bcmd"].SetCustomDesignRules(board=k.doc)
    for rr in read.rules:
        stale.rules.add().CopyFrom(rr)
    agent.request_raw(stale)
    text = rules_file.read_text()
    check("1.5mm" in text, "a whole-list Set from a stale read keeps the person's edit")

    # Net classes: the agent's (IPC, live) and one written into the project file meanwhile.
    project = m["project"]
    spec = m["types"].ProjectSpecifier()
    spec.CopyFrom(k.doc.project)
    nc = k.host_api.module("common.types.project_settings").NetClass(name="AGENT_HS")
    r = agent.request_raw(project.SetNetClasses(net_classes=[nc], merge_mode=m["types"].MMM_MERGE,
                                                project=spec))
    check(r.status.status == agent.envelope.AS_OK, f"the agent's net class is set live ({r.status.error_message})")
    pro = k.project
    data = json.loads(pro.read_text())
    classes = data.setdefault("net_settings", {}).setdefault("classes", [])
    person_class = dict(classes[0]) if classes else {}
    person_class["name"] = "PERSON_HV"
    classes.append(person_class)
    pro.write_text(json.dumps(data, indent=2))
    time.sleep(1.5)  # the editor merges the file in

    agent.request_raw(project.SaveDocument(document=k.doc))
    names = {c["name"] for c in json.loads(pro.read_text())["net_settings"]["classes"]}
    check({"AGENT_HS", "PERSON_HV"} <= names, f"both net classes survive the save ({sorted(names)})")
    live = {c.name for c in k.host_api.request(project.GetNetClasses(project=spec),
                                               project.NetClassesResponse).net_classes}
    check({"AGENT_HS", "PERSON_HV"} <= live, f"and the open editor has both ({sorted(live)})")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("kicad")
    ap.add_argument("kicad_cli")
    ap.add_argument("--out", default="coedit-scenarios-out")
    ap.add_argument("--only", default="collision,parallel,rules")
    ap.add_argument("--seconds", type=int, default=60)
    a = ap.parse_args()
    only = set(a.only.split(","))
    with EmbeddedKiCad(a.kicad, a.kicad_cli, "demos/pic_programmer", a.out) as k:
        if "collision" in only:
            scenario_collision(k)
        if "parallel" in only:
            scenario_parallel(k, a.seconds)
        if "rules" in only:
            scenario_rules(k)
        check(k.alive(), "KiCad is still running")
    print(f"{len(FAILURES)} failure(s)", flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
