#!/usr/bin/env python3
"""Check a macOS package's licences (what scripts/backplane-macos-licences.py put in it), on the package alone.

  - every Mach-O in KiCad.app is in licenses/BUNDLED.tsv, with what it was built from (a Homebrew formula, nng or KiCad), and none is
    of unknown origin;
  - every Homebrew formula named there has a row in licenses/homebrew/LIBRARIES.tsv, and every row's files are in the package and not
    empty: no bundled library without its licence;
  - nng has licenses/nng/LICENSE.txt; KiCad has its own licence files in licenses/.

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
    for need in (lic / 'BUNDLED.tsv', lic / 'homebrew' / 'LIBRARIES.tsv'):
        if not need.is_file():
            sys.exit(f'FAIL licences: {need.relative_to(root)} is missing')
    bundled = {r['path']: r['built from'] for r in tsv(lic / 'BUNDLED.tsv')}
    libs = {r['library']: r for r in tsv(lic / 'homebrew' / 'LIBRARIES.tsv')}
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
    formulas = sorted({o.split()[0].split(':', 1)[1] for o in bundled.values() if o.startswith('homebrew:')})
    for f in formulas:
        r = libs.get(f)
        if r is None:
            problems.append(f'{f}: bundled, but no row in LIBRARIES.tsv')
            continue
        files = [x for x in r.get('files', '').split() if x != '-']
        if not files:
            problems.append(f'{f} {r.get("version")}: no licence file')
        for x in files:
            p = lic / x
            if not p.is_file() or p.stat().st_size == 0:
                problems.append(f'{f}: {x} missing or empty')
    if any(o == 'nng' for o in bundled.values()) and not (lic / 'nng' / 'LICENSE.txt').is_file():
        problems.append('nng: licenses/nng/LICENSE.txt missing')
    if not any(p.name.upper().startswith(('LICENSE', 'COPYING')) for p in lic.iterdir() if p.is_file()):
        problems.append('KiCad: no LICENSE*/COPYING* in licenses/')
    print(f'licences: {len(machos)} Mach-O files ({sum(1 for o in bundled.values() if o == "kicad")} KiCad, '
          f'{sum(1 for o in bundled.values() if o == "nng")} nng, {sum(1 for o in bundled.values() if o.startswith("homebrew:"))} from '
          f'{len(formulas)} Homebrew formulas); {len(libs)} formulas with licence text')
    if problems:
        print('FAIL licences:\n  ' + '\n  '.join(problems))
        sys.exit(1)
    print('PASS licences: every bundled library has its licence in the package')


if __name__ == '__main__':
    main()
