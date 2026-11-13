#!/usr/bin/env python3
# eqgate.py CMP.txt EXCLUDE.txt OUTDIR BASEDIR : the equality gate's verdict from eq18.sh's cmp.txt.
# Every board must be guard:SAME threads:SAME raw:SAME, except those in EXCLUDE.txt, which must
# satisfy their mode instead.  Prints PASS/FAIL lines and exits non-zero on any failure.
import sys, json, collections, os
cmp, excl, out, base = sys.argv[1:5]
rules = {}
for line in open(excl):
    if line.startswith('#') or not line.strip(): continue
    b, mode, why = line.rstrip('\n').split('\t')
    rules[b] = (mode, why)
def counts(p):
    d = json.load(open(p)); c = collections.Counter(v['type'] for v in d.get('violations', []))
    c['unconnected_items'] = len(d.get('unconnected_items', [])); return c
fail = 0; rawdiff = 0; seen = set()
for line in open(cmp):
    w = line.split()
    if w[0] == 'NOREPORT':
        b = ' '.join(w[1:])
        st = [w[0]]
    else:
        st = w[:4]; b = line.split(None, 4)[4]
        # the board path runs to the first ' DIFF' or end
        b = b.split(' DIFF')[0].strip()
    seen.add(b)
    if b in rules:
        mode, why = rules[b]
        n = b.replace('/', '_').replace(' ', '_')[:-len('.kicad_pcb')]
        sn = b.replace('/', '_')[:-len('.kicad_pcb')]
        if mode == 'noload':
            t1 = f'{out}/{n}-t1.json'
            ok = st[0] == 'NOREPORT' and ( not os.path.exists( t1 ) or os.path.getsize( t1 ) == 0 )
        elif mode.startswith('fixed:'):
            ref = mode.split(':', 1)[1]
            try:
                key = lambda v: json.dumps(v, sort_keys=True)
                def norm(p):
                    d = json.load(open(p)); return collections.Counter(key({'description': v['description'], 'items': [i['description'] for i in v['items']]}) for v in d.get('violations', []))
                g, b0 = norm(f'{out}/{n}-t1.json'), norm(f'{base}/{sn}-s14.json')
                removed, added = b0 - g, g - b0
                ok = not added and removed and sum(removed.values()) == 2 and all(any(r in k for r in ref.split('|')) for k in removed) and st[1:3] == ['threads:SAME', 'raw:SAME']
            except Exception:
                ok = False
        elif mode == 'basenondet':
            try:
                ok = st[1:3] == ['threads:SAME', 'raw:SAME'] and counts(f'{out}/{n}-t1.json') == counts(f'{base}/{sn}-s14.json')
            except Exception:
                ok = False
        else:
            try:
                a, t8, s = counts(f'{out}/{n}-t1.json'), counts(f'{out}/{n}-t8.json'), counts(f'{base}/{sn}-s14.json')
                ok = a == t8 == s
            except Exception:
                ok = False
        print(f"{'PASS' if ok else 'FAIL'} excluded ({mode}) {b}: {why[:90]}")
        fail += not ok
    elif st[:2] != ['guard:SAME', 'threads:SAME'] or ( st[2] == 'raw:DIFF' and st[3] != 'base-raw:DIFF' ):
        # Equal once uuids not stored in the board file are set aside (the board item's is new
        # at every load), and byte-identical at 1 and 8 threads unless the base build is not either
        print(f"FAIL {line.strip()[:300]}"); fail += 1
    elif st[2] == 'raw:DIFF':
        rawdiff += 1
for b in rules:
    if b not in seen: print(f"FAIL excluded board not in the corpus: {b}"); fail += 1
print(f"{len(seen)} boards, {fail} failing; {rawdiff} differ in raw bytes at 1 vs 8 threads exactly as the base build does (new board uuid each load)")
sys.exit(1 if fail else 0)
