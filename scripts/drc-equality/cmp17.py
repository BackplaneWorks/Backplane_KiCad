#!/usr/bin/env python3
# cmp17.py BOARD A.json B.json : multiset compare of two DRC reports, with item uuids that do not occur
# in the board file replaced by "X" (the board-level item gets a fresh uuid each run).
import json, sys, re
board = open(sys.argv[1], errors='replace').read()
def load(p):
    d = json.load(open(p)); d.pop('date', None)
    def fix(o):
        if isinstance(o, dict): return {k: ('X' if k == 'uuid' and isinstance(v, str) and v not in board else fix(v)) for k, v in o.items()}
        if isinstance(o, list): return [fix(v) for v in o]
        return o
    return fix(d)
a, b = load(sys.argv[2]), load(sys.argv[3])
key = lambda v: json.dumps(v, sort_keys=True)
out = []
for k in sorted(set(a) | set(b)):
    x, y = a.get(k), b.get(k)
    if isinstance(x, list) or isinstance(y, list):
        sx, sy = sorted(map(key, x or [])), sorted(map(key, y or []))
        if sx != sy:
            import collections
            ca, cb = collections.Counter(sx), collections.Counter(sy)
            out.append(f"{k}: {len(sx)} vs {len(sy)}; onlyA {[v[:200] for v in (ca-cb)][:2]} onlyB {[v[:200] for v in (cb-ca)][:2]}")
    elif x != y and k not in ('kicad_version', 'source'):
        out.append(f"{k}: {str(x)[:60]} vs {str(y)[:60]}")
print('SAME' if not out else 'DIFF ' + ' | '.join(out))
