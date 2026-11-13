#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Make a routing plan (docs/backplane/floorplanner.md) for a board, the way an agent would
propose one: buses between the part pairs that share the most nets become corridors on the
signal layers, power nets become plane regions on the power layers.

    backplane-plan-demo.py BOARD.kicad_pcb [--strip] [--out DIR]

With --strip the board is copied without its tracks, vias and zones (a board before routing)
next to its plan, so the overlay reads the way it does at planning time.  The plan is written
as <board>.kicad_plan beside the (copied) board.  Pure Python; no KiCad needed.
"""

import argparse
import json
import math
from pathlib import Path
import re
import shutil
import sys


def tokenize(text):
    return re.findall(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()"]+', text)


def parse(tokens):
    stack = [[]]
    for t in tokens:
        if t == "(":
            stack.append([])
        elif t == ")":
            node = stack.pop()
            stack[-1].append(node)
        else:
            stack[-1].append(t[1:-1] if t.startswith('"') else t)
    return stack[0]


def child(node, name):
    for c in node:
        if isinstance(c, list) and c and c[0] == name:
            return c
    return None


def children(node, name):
    return [c for c in node if isinstance(c, list) and c and c[0] == name]


def strip_copper(text):
    """The board text without top-level segment/arc/via/zone forms."""
    out, i, n = [], 0, len(text)
    depth = 0
    start = None
    skip = ("(segment", "(arc", "(via", "(zone")
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            if start is None:
                out.append(text[i:j + 1])
            i = j + 1
            continue
        if c == "(":
            depth += 1
            if depth == 2 and start is None and text.startswith(skip, i):
                start = i
            elif start is None:
                out.append(c)
        elif c == ")":
            if start is not None and depth == 2:
                start = None
                depth -= 1
                i += 1
                # drop the line's newline
                while i < n and text[i] in " \t":
                    i += 1
                if i < n and text[i] == "\n":
                    i += 1
                continue
            depth -= 1
            if start is None:
                out.append(c)
        elif start is None:
            out.append(c)
        i += 1
    return "".join(out)


def rot(x, y, deg):
    a = math.radians(-deg)
    return x * math.cos(a) - y * math.sin(a), x * math.sin(a) + y * math.cos(a)


def placement(fp):
    """A footprint's position and rotation: (at x y [r]) in stock files, (transform (translate x y)
    (rotate r) ...) in boards the Backplane fork saved."""
    at = child(fp, "at")
    if at:
        return float(at[1]), float(at[2]), float(at[3]) if len(at) > 3 else 0.0
    tr = child(fp, "transform")
    if tr:
        t = child(tr, "translate")
        r = child(tr, "rotate")
        return (float(t[1]) if t else 0.0, float(t[2]) if t else 0.0,
                float(r[1]) if r and len(r) > 1 else 0.0)
    return 0.0, 0.0, 0.0


def load(board_path):
    tree = parse(tokenize(board_path.read_text()))[0]
    layers = []
    for l in child(tree, "layers")[1:]:
        if isinstance(l, list) and len(l) >= 3 and l[1].endswith(".Cu"):
            layers.append(l[1])
    footprints = []
    for fp in children(tree, "footprint"):
        fx, fy, frot = placement(fp)
        side = child(fp, "layer")[1]
        ref = "?"
        for prop in children(fp, "property"):
            if prop[1] == "Reference":
                ref = prop[2]
        for fpt in children(fp, "fp_text"):
            if fpt[1] == "reference":
                ref = fpt[2]
        pads = []
        for pad in children(fp, "pad"):
            pat = child(pad, "at")
            px, py = float(pat[1]), float(pat[2])
            rx, ry = rot(px, py, frot)
            net = child(pad, "net")
            name = None
            if net:
                name = net[-1] if len(net) > 1 else None
            pads.append({"number": pad[1], "x": fx + rx, "y": fy + ry, "net": name})
        footprints.append({"ref": ref, "x": fx, "y": fy, "side": side, "pads": pads})
    return layers, footprints


PALETTE = ["#e8a33d", "#d9534f", "#3b7ddd", "#2bb3a3", "#9aa3a3", "#9b6bd3", "#6dbf4b",
           "#e36fa7", "#5a8fd0", "#c9b458"]
PLANE_PALETTE = ["#d4543a", "#3f66b5", "#2fa58f", "#e8b33a", "#d6457a", "#6a9e3a", "#8a5cc2", "#e07b28",
                 "#4fa3d9", "#b0b84a"]


def is_power(net):
    n = net.upper().lstrip("/")
    return (n.startswith("+") or n.startswith("VCC") or n.startswith("VDD")
            or re.match(r"^\+?\d+V\d*", n) is not None or n in ("VBUS", "VIN", "VSYS")
            or re.search(r"(^|/)(\+?\d+V\d*|VBUS|VSYS|VIN|3V3|1V8|5V)$", n) is not None)


def is_ground(net):
    return net.upper().lstrip("/") in ("GND", "GNDA", "GNDD", "AGND", "DGND", "PGND")


def common_prefix(names):
    if not names:
        return ""
    p = names[0]
    for n in names[1:]:
        while not n.startswith(p):
            p = p[:-1]
    return p.rstrip("_/0123456789")


def hull(points):
    pts = sorted(set(points))
    if len(pts) <= 2:
        return pts
    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    lower, upper = [], []
    for p in pts:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], p) <= 0:
            lower.pop()
        lower.append(p)
    for p in reversed(pts):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], p) <= 0:
            upper.pop()
        upper.append(p)
    return lower[:-1] + upper[:-1]


def inflate(poly, d, segments=3):
    """Minkowski-ish growth of a convex polygon (round corners)."""
    if len(poly) < 3:
        x, y = poly[0] if poly else (0, 0)
        if len(poly) == 2:
            (x1, y1), (x2, y2) = poly
            return inflate([(x1, y1), (x2, y2), ((x1 + x2) / 2 + 0.01, (y1 + y2) / 2 + 0.01)], d)
        return [(x + d * math.cos(t), y + d * math.sin(t))
                for t in [i * 2 * math.pi / 12 for i in range(12)]]
    out = []
    n = len(poly)
    for i in range(n):
        p0, p1, p2 = poly[i - 1], poly[i], poly[(i + 1) % n]
        a0 = math.atan2(p1[1] - p0[1], p1[0] - p0[0]) - math.pi / 2
        a1 = math.atan2(p2[1] - p1[1], p2[0] - p1[0]) - math.pi / 2
        while a1 < a0:
            a1 += 2 * math.pi
        for k in range(segments + 1):
            t = a0 + (a1 - a0) * k / segments
            out.append((p1[0] + d * math.cos(t), p1[1] + d * math.sin(t)))
    return out


def plan_for(layers, footprints):
    inner = [l for l in layers if l.startswith("In")]
    power_layers = inner[1::2][:2] if len(inner) >= 2 else []
    signal_layers = [l for l in inner if l not in power_layers] or [layers[0]]
    signal_layers = signal_layers + ["F.Cu", "B.Cu"]

    # nets -> footprints
    net_refs = {}
    for fp in footprints:
        for pad in fp["pads"]:
            if pad["net"] and not pad["net"].startswith("unconnected"):
                net_refs.setdefault(pad["net"], set()).add(fp["ref"])
    by_ref = {fp["ref"]: fp for fp in footprints}

    # part pairs sharing signal nets
    pairs = {}
    for net, refs in net_refs.items():
        if is_power(net) or is_ground(net) or len(refs) < 2 or len(refs) > 4:
            continue
        big = sorted(refs, key=lambda r: -len(by_ref[r]["pads"]))[:2]
        if len(by_ref[big[1]]["pads"]) < 3:
            continue
        key = tuple(sorted(big))
        pairs.setdefault(key, []).append(net)

    ranked = sorted(pairs.items(), key=lambda kv: -len(kv[1]))
    ranked = [kv for kv in ranked if len(kv[1]) >= 3][:9]

    groups, corridors = [], []
    for i, ((a, b), nets) in enumerate(ranked):
        nets = sorted(nets)
        gid = f"g-{i + 1}"
        prefix = common_prefix([n.split("/")[-1] for n in nets]) or f"{a}-{b}"
        groups.append({"id": gid, "name": f"{prefix} ({a}→{b})", "color": PALETTE[i % len(PALETTE)],
                       "nets": nets, "diff_pairs": [], "netclass": f"plan_{prefix or gid}",
                       "status": "proposed", "locked": False})

        def centroid(ref):
            pts = [(p["x"], p["y"]) for p in by_ref[ref]["pads"] if p["net"] in nets]
            if not pts:
                return by_ref[ref]["x"], by_ref[ref]["y"]
            return sum(x for x, _ in pts) / len(pts), sum(y for _, y in pts) / len(pts)

        (ax, ay), (bx, by) = centroid(a), centroid(b)
        dx, dy = bx - ax, by - ay
        # A 45-degree dog-leg, the way a person would sketch it.
        if abs(dx) >= abs(dy):
            mx = bx - math.copysign(abs(dy), dx) if dx else bx
            path = [[ax, ay], [mx, ay], [bx, by]]
        else:
            my = by - math.copysign(abs(dx), dy) if dy else by
            path = [[ax, ay], [ax, my], [bx, by]]
        path = [[round(x, 3), round(y, 3)] for x, y in path]
        layer = signal_layers[i % len(signal_layers)]
        width = math.ceil(min(4.0, max(0.8, 0.26 * len(nets))) * 20) / 20   # n x (track + gap), with room
        vias = []
        if layer.startswith("In"):
            # escape vias in a short row beside the start
            for k in range(min(4, len(nets))):
                vias.append([round(ax + 1.2 + 0.9 * k, 3), round(ay - 1.2, 3)])
        corridors.append({"id": f"c-{i + 1}", "group": gid, "layer": layer, "order": i + 1,
                          "path": path, "width": width, "from": [a], "to": [b], "vias": vias,
                          "escapes": [], "status": "proposed", "locked": False,
                          "author": "agent:planner", "notes": []})

    # Power planes: one region per power net, on the power layers.
    planes = []
    power_nets = sorted([n for n in net_refs if is_power(n)],
                        key=lambda n: -sum(1 for fp in footprints for p in fp["pads"] if p["net"] == n))
    for i, net in enumerate(power_nets[:8]):
        pts = [(p["x"], p["y"]) for fp in footprints for p in fp["pads"] if p["net"] == net]
        if not pts:
            continue
        poly = inflate(hull(pts), 1.6)
        layer = power_layers[i % len(power_layers)] if power_layers else layers[-1]
        planes.append({"id": f"p-{i + 1}", "net": net, "layer": layer,
                       "color": PLANE_PALETTE[i % len(PLANE_PALETTE)],
                       "outline": [[[round(x, 3), round(y, 3)] for x, y in poly]],
                       "priority": len(power_nets) - i, "reference": "",
                       "status": "proposed", "locked": False, "author": "agent:planner"})

    plan_layers = [{"layer": l, "role": "signal", "status": "proposed", "note": ""}
                   for l in signal_layers if any(c["layer"] == l for c in corridors)]
    plan_layers += [{"layer": l, "role": "power", "status": "proposed", "note": ""}
                    for l in power_layers]
    return {"format": 1, "revision": 1, "layers": plan_layers, "groups": groups,
            "corridors": corridors, "planes": planes, "splits": [], "notes": [],
            "constraints": [],
            "history": [{"revision": 1, "actor": "agent:planner", "op": "propose",
                         "items": [c["id"] for c in corridors] + [p["id"] for p in planes],
                         "at": ""}]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board", type=Path)
    parser.add_argument("--strip", action="store_true")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()

    board = args.board
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        for f in board.parent.iterdir():
            if f.is_file():
                shutil.copy2(f, args.out / f.name)
        board = args.out / board.name
    if args.strip:
        board.write_text(strip_copper(board.read_text()))

    layers, footprints = load(board)
    plan = plan_for(layers, footprints)
    plan["board"] = board.name
    out = board.with_suffix(".kicad_plan")
    out.write_text(json.dumps(plan, indent=2) + "\n")
    print(f"{out}: {len(plan['groups'])} groups, {len(plan['corridors'])} corridors, "
          f"{len(plan['planes'])} planes on {len(layers)} copper layers")


if __name__ == "__main__":
    sys.exit(main())
