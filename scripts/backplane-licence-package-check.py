#!/usr/bin/env python3
"""Check a macOS package's licences (what scripts/backplane-macos-licences.py put in it), on the package alone.

  - every Mach-O in KiCad.app is in licenses/BUNDLED.tsv, with what it was built from (a Homebrew formula, nng or KiCad), and none is
    of unknown origin;
  - every library in licenses/LIBRARIES.tsv that is in the package (a dylib, static, header-only, a vendored library compiled in, nng,
    KiCad) has its licence files in the package, not empty; every Homebrew formula BUNDLED.tsv names has a row;
  - licenses/vendor/thirdparty is there.

usage: backplane-licence-package-check.py PACKAGE_ROOT     (exit 1 on any problem, each one printed)
"""
import sys
from pathlib import Path

MACHO_MAGIC = {b'\xfe\xed\xfa\xce', b'\xce\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'}


def is_macho(p):
    if p.is_symlink() or not p.is_file():
        return False
    with open(p, 'rb') as f:
        return f.read(4) in MACHO_MAGIC


def tsv(path):
    lines = path.read_text().splitlines()
    head = lines[0].split('\t')
    return [dict(zip(head, l.split('\t'))) for l in lines[1:] if l.strip()]


def main():
    root = Path(sys.argv[1]).resolve(strict=True)
    lic = root / 'licenses'
    problems = []
    for need in (lic / 'BUNDLED.tsv', lic / 'LIBRARIES.tsv'):
        if not need.is_file():
            sys.exit(f'FAIL licences: {need.relative_to(root)} is missing')
    if not (lic / 'vendor' / 'thirdparty').is_dir():
        problems.append('licenses/vendor/thirdparty is missing (KiCad\'s vendored libraries\' notices)')
    bundled = {r['path']: r['built from'] for r in tsv(lic / 'BUNDLED.tsv')}
    rows = tsv(lic / 'LIBRARIES.tsv')
    libs = {r['library']: r for r in rows}
    machos = sorted(p.relative_to(root).as_posix() for p in (root / 'KiCad.app').rglob('*') if is_macho(p))
    for m in machos:
        origin = bundled.get(m)
        if origin is None:
            problems.append(f'{m}: a Mach-O not in BUNDLED.tsv')
        elif origin == 'unknown':
            problems.append(f'{m}: of unknown origin')
    for m in bundled:
        if not (root / m).exists():
            problems.append(f'{m}: in BUNDLED.tsv but not in the package')
    for f in sorted({o.split()[0].split(':', 1)[1] for o in bundled.values() if o.startswith('homebrew:')}):
        if 'homebrew:' + f not in libs:
            problems.append(f'{f}: bundled, but no row in LIBRARIES.tsv')
    if any(o == 'nng' for o in bundled.values()) and 'nng' not in libs:
        problems.append('nng: bundled, but no row in LIBRARIES.tsv')
    shipped = [r for r in rows if r.get('linked as', '') not in ('not compiled in', 'vendored')]
    for r in shipped:
        files = [x for x in r.get('files', '').split() if x != '-']
        if not files:
            problems.append(f'{r["library"]} {r.get("version", "")} ({r.get("linked as")}): no licence file')
        for x in files:
            p = lic / x
            if not p.is_file() or p.stat().st_size == 0:
                problems.append(f'{r["library"]}: {x} missing or empty')
    count = lambda pre, la=None: sum(1 for r in rows if r['library'].startswith(pre) and (la is None or r.get('linked as', '').startswith(la)))
    print(f'licences: {len(machos)} Mach-O files; Homebrew formulas {count("homebrew:")} ({count("homebrew:", "dylib")} dylib, '
          f'{count("homebrew:", "static")} static, {count("homebrew:", "header-only")} header-only); vendored compiled in '
          f'{count("vendored:", "compiled in")}; {len(shipped)} libraries in the package, each checked for its licence text')
    if problems:
        print('FAIL licences:\n  ' + '\n  '.join(problems))
        sys.exit(1)
    print('PASS licences: every library in the package has its licence text')


if __name__ == '__main__':
    main()
