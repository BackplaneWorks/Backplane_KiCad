#!/usr/bin/env python3
"""The Linux package's licence list, and the licences of what the build compiled in without a shared object to show for it.

The release workflow already copies, for every shared object it bundles (the ELF closure), its Debian package's copyright and
metadata into licenses/debian/<package>/, and KiCad's thirdparty/ notices into licenses/vendor/thirdparty/. This adds:
  - header-only and static libraries: every header in ninja's deps log and every archive (.a) named in build.ninja (gathered before the
    build tree is removed, --inputs) is mapped to its Debian package (dpkg's file lists); a package reached that way and not already in
    licenses/debian gets its copyright and metadata the same way (glm: libglm-dev), and a row "header-only" or "static";
  - the notices of vendored libraries that live in a README or a header's head (lemon, planegcs, 3dxware_sdk, glad, vma), and which
    vendored libraries the build compiled in (a vendored library compiled in with no notice fails the run);
  - licenses/LIBRARIES.tsv: library (debian:<source package>, vendored:<dir>, kicad), version, licence, where the text came from, how
    it is linked, the files, the shared objects; and licenses/BUNDLED.tsv: every ELF file in the package and what it was built from.

usage: backplane-linux-licences.py ROOT --inputs FILE --stage STAGE_DIR --source SOURCE_DIR
       backplane-linux-licences.py --gather BUILD_DIR > FILE      (the build's inputs from outside its tree, before it is removed)
Then scripts/backplane-licence-package-check.py ROOT checks the result.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import backplane_licences as common  # noqa: E402


def dpkg_index():
    """path -> binary package, from dpkg's own file lists (with usrmerge's /lib == /usr/lib)."""
    idx = {}
    info = Path('/var/lib/dpkg/info')
    for lst in info.glob('*.list'):
        pkg = lst.name[:-5]
        for line in lst.read_text(errors='replace').splitlines():
            idx.setdefault(line, pkg)
            if line.startswith('/lib'):
                idx.setdefault('/usr' + line, pkg)
            elif line.startswith('/usr/lib'):
                idx.setdefault(line[4:], pkg)
    return idx


def pkg_meta(pkg):
    fmt = '${binary:Package}\t${Version}\t${source:Package}\t${source:Version}\n'
    r = subprocess.run(['dpkg-query', '-W', '-f=' + fmt, pkg], text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    parts = r.stdout.strip().split('\t')
    return parts if len(parts) == 4 else None


def record_notice(lic, pkg):
    """licenses/debian/<pkg>/{metadata.txt,copyright}, as the workflow writes them."""
    d = lic / 'debian' / pkg
    if (d / 'copyright').is_file():
        return d
    m = pkg_meta(pkg)
    if not m:
        return None
    d.mkdir(parents=True, exist_ok=True)
    arch = subprocess.run(['dpkg-query', '-W', '-f=${Architecture}', pkg], text=True, stdout=subprocess.PIPE).stdout.strip()
    (d / 'metadata.txt').write_text(f'Package: {m[0]}\nVersion: {m[1]}\nSource: {m[2]}\nSource-Version: {m[3]}\n'
                                    f'Source-URL: https://launchpad.net/ubuntu/+source/{m[2]}/{m[3]}\nArchitecture: {arch}\n')
    c = Path('/usr/share/doc') / pkg.split(':')[0] / 'copyright'
    if not c.is_file():
        lst = subprocess.run(['dpkg', '-L', pkg], text=True, stdout=subprocess.PIPE).stdout.splitlines()
        c = next((Path(x) for x in lst if x.endswith('/copyright')), Path('/nonexistent'))
    if c.is_file():
        shutil.copyfile(c, d / 'copyright')
    return d


def read_meta(d):
    m = {}
    for line in (d / 'metadata.txt').read_text().splitlines() if (d / 'metadata.txt').is_file() else []:
        k, _, v = line.partition(': ')
        m[k] = v
    return m


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    ap.add_argument('root', type=Path, nargs='?')
    ap.add_argument('--gather', type=Path)
    ap.add_argument('--inputs', type=Path)
    ap.add_argument('--stage', type=Path)
    ap.add_argument('--source', type=Path, default=Path.cwd())
    a = ap.parse_args()
    if a.gather:
        print('\n'.join(common.gather(a.gather)))
        return
    root = a.root.resolve(strict=True)
    lic = root / 'licenses'
    src = a.source.resolve()
    idx = dpkg_index()
    problems = []
    # what the build compiled in from outside its tree
    how, vendored_used = {}, set()
    for f in (a.inputs.read_text().splitlines() if a.inputs and a.inputs.is_file() else []):
        if f.startswith(str(src / 'thirdparty') + '/'):
            vendored_used.add(Path(f).relative_to(src / 'thirdparty').parts[0])
            continue
        if not f.startswith('/usr/') and not f.startswith('/lib'):
            continue
        pkg = idx.get(f) or idx.get(os.path.realpath(f))
        if pkg:
            how.setdefault(pkg, set()).add('static' if f.endswith('.a') else 'headers')
        elif f.endswith('.a'):
            problems.append(f'{f}: a static archive the build linked, in no Debian package')
    # every ELF file in the package and where it came from
    stage = a.stage.resolve() if a.stage else None
    staged = {p.name for p in stage.rglob('*')} if stage and stage.is_dir() else set()
    bundled, shared = [], {}
    for p in sorted(root.rglob('*')):
        if p.is_symlink() or not p.is_file():
            continue
        with open(p, 'rb') as fh:
            if fh.read(4) != b'\x7fELF':
                continue
        rel = p.relative_to(root).as_posix()
        name = p.name
        real = p.name[:-5] if p.name.endswith('.real') else p.name
        pkg = None
        if rel.startswith('lib/') and real not in staged:
            for d in ('/usr/lib/x86_64-linux-gnu/', '/lib/x86_64-linux-gnu/', '/usr/lib/', '/lib/', '/usr/lib/x86_64-linux-gnu/ngspice/'):
                pkg = idx.get(d + name)
                if pkg:
                    break
        if pkg:
            shared.setdefault(pkg, []).append(name)
            bundled.append((rel, pkg))
        elif real in staged:
            bundled.append((rel, 'kicad'))
        else:
            bundled.append((rel, 'unknown'))
            problems.append(f'{rel}: an ELF file from no Debian package and not built here')
    for pkg in sorted(set(how) | set(shared)):
        if record_notice(lic, pkg) is None:
            problems.append(f'{pkg}: no dpkg record')
    # one row per Debian source package
    by_source = {}
    recorded = {d.name for d in (lic / 'debian').iterdir() if d.is_dir()} if (lic / 'debian').is_dir() else set()
    for pkg in sorted(set(how) | set(shared) | recorded):
        m = read_meta(lic / 'debian' / pkg)
        s = m.get('Source') or pkg.split(':')[0]
        by_source.setdefault(s, {'version': m.get('Source-Version', '-'), 'url': m.get('Source-URL', '-'), 'pkgs': []})['pkgs'].append(pkg)
    rows = []
    for s, e in sorted(by_source.items()):
        kinds = set().union(*(how.get(p, set()) for p in e['pkgs']))
        sos = sorted({n for p in e['pkgs'] for n in shared.get(p, [])})
        has_so = bool(sos) or any(p.split(':')[0] in ('ngspice', 'libngspice0') for p in e['pkgs'])
        linked = ('shared object' + (' (and its headers compiled in)' if 'headers' in kinds else '')) if has_so else \
                 ('static' if 'static' in kinds else 'header-only')
        files = [f'debian/{p}/copyright' for p in e['pkgs'] if (lic / 'debian' / p / 'copyright').is_file()]
        if not files:
            problems.append(f'debian:{s} ({linked}): no copyright file')
        rows.append(('debian:' + s, e['version'], '-', ' '.join(f'/usr/share/doc/{p.split(":")[0]}/copyright' for p in e['pkgs']) + ' ' + e['url'],
                     linked, ' '.join(files) or '-', ' '.join(sos) or '-'))
        for rel_i, (rel, o) in enumerate(bundled):
            if o in e['pkgs']:
                bundled[rel_i] = (rel, 'debian:' + s)
    # vendored libraries: the README/header notices (every notice file, the README/header ones too), and which the build compiled in
    inputs = a.inputs.read_text().splitlines() if a.inputs and a.inputs.is_file() else []
    rows += common.vendored(lic, src, common.thirdparty_used(inputs, src), bool(a.inputs), problems)
    rows.append(common.kicad_row(lic))
    common.write_lists(lic, rows, bundled)
    deb = [r for r in rows if r[0].startswith('debian:')]
    for r in deb:
        if not r[4].startswith('shared object'):
            print(f'{r[0]} {r[1]}: {r[4]} ({r[5]})')
    print(f'{len(deb)} Debian source packages ({sum(1 for r in deb if r[4].startswith("shared"))} shared object, '
          f'{sum(1 for r in deb if r[4] == "static")} static, {sum(1 for r in deb if r[4] == "header-only")} header-only); '
          f'{len(vendored_used)} vendored libraries compiled in; {sum(1 for _, o in bundled if o == "kicad")} KiCad ELF files')
    if problems:
        sys.exit('licences:\n  ' + '\n  '.join(problems))


if __name__ == '__main__':
    main()
