#!/usr/bin/env python3
"""Put the licence of every library the macOS package bundles into the package, and say where each came from.

Every Mach-O file in KiCad.app is traced to where it was built: a Homebrew keg (found by name among the kegs `$prefix/opt` points at,
the versions the build linked), the nng 1.x install, or this source tree (KiCad's own). For each Homebrew formula that is bundled, its
licence files are copied from the keg into licenses/homebrew/<formula>/, keeping their paths inside the keg (so two files of one name,
or names that differ only in case, never land on each other). A keg with no licence text of its own takes it from the formula's source
at the bundled version (the URL in the keg's own .brew/<formula>.rb); a public-domain library with no text anywhere gets a note.

Writes, beside the licences:
  licenses/homebrew/LIBRARIES.tsv  formula, version, licence (Homebrew's SPDX), where the text came from, the files
  licenses/BUNDLED.tsv             every Mach-O in KiCad.app, by its path in the bundle, and what it was built from

usage: backplane-macos-licences.py ROOT --prefix BREW_PREFIX --nng NNG_PREFIX [--build BUILD_DIR ...] [--cache DIR]
  (--build: the build and install trees; with them, a Mach-O in Frameworks from none of Homebrew, nng or the build fails the run)
Then scripts/backplane-licence-package-check.py ROOT checks the result (and any package later).
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

# licence, licence (British), copying, copyright, notice: in any case
LICENCE_NAME = re.compile(r'licen[cs]e|copying|copyright|notice', re.I)
# Homebrew SPDX values that mean the code carries no licence text to keep (SQLite's "blessing")
PUBLIC_DOMAIN = {'blessing', 'public domain', 'publicdomain', 'cc-pdddc', 'unlicense'}
MACHO_MAGIC = {b'\xfe\xed\xfa\xce', b'\xce\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'}


def is_macho(p):
    if p.is_symlink() or not p.is_file():
        return False
    with open(p, 'rb') as f:
        return f.read(4) in MACHO_MAGIC


def uuids(p):
    out = subprocess.run(['otool', '-l', str(p)], text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout
    return sorted(line.split()[1] for line in out.splitlines() if line.strip().startswith('uuid '))


def kegs(prefix):
    """{keg path: (formula, version)} for every installed keg, and the set of those $prefix/opt points at (the versions the build linked)."""
    out, linked = {}, set()
    cellar = prefix / 'Cellar'
    for f in sorted(cellar.iterdir()) if cellar.is_dir() else []:
        for v in sorted(f.iterdir()) if f.is_dir() else []:
            if v.is_dir():
                out[v.resolve()] = (f.name, v.name)
    opt = prefix / 'opt'
    for link in sorted(opt.iterdir()) if opt.is_dir() else []:
        try:
            linked.add(link.resolve(strict=True))
        except OSError:
            pass
    return out, linked


def index(roots):
    """basename -> [paths] of every file under the given trees (symlinks too: a library is often reached by its versioned name)."""
    idx = {}
    for root in roots:
        for dp, dn, fn in os.walk(root):
            for name in fn:
                idx.setdefault(name, []).append(Path(dp) / name)
    return idx


def licence_files(tree):
    return sorted(p for p in tree.rglob('*') if p.is_file() and not p.is_symlink() and LICENCE_NAME.search(p.name) and p.suffix != '.rb'
                  and '.brew' not in p.relative_to(tree).parts)


def source_url(keg, formula):
    """The stable source URL in the keg's own copy of its formula: the source of exactly the bundled version."""
    rb = keg / '.brew' / (formula + '.rb')
    if not rb.is_file():
        return None
    for line in rb.read_text(errors='replace').splitlines():
        m = re.match(r'\s*url\s+"([^"]+)"', line)
        if m:
            return m.group(1)
    return None


def texts_from_source(url, cache, dest):
    """Licence files at the top of the source archive (its root and its docs/), copied under dest. Returns their paths there."""
    cache.mkdir(parents=True, exist_ok=True)
    arc = cache / re.sub(r'[^A-Za-z0-9._-]', '_', url.rsplit('/', 1)[-1])
    if not arc.is_file():
        req = urllib.request.Request(url, headers={'User-Agent': 'backplane-macos-licences'})
        with urllib.request.urlopen(req, timeout=300) as r, open(str(arc) + '.part', 'wb') as f:
            shutil.copyfileobj(r, f)
        os.replace(str(arc) + '.part', arc)
    got = []
    with tarfile.open(arc) as t:
        members = {m.name: m for m in t.getmembers()}
        for m in members.values():
            parts = m.name.split('/')
            rel = parts[1:] if len(parts) > 1 else parts
            # the root and its doc(s)/ by name; a REUSE LICENSES/ folder whole
            top = (len(rel) == 1 or (len(rel) == 2 and rel[0].lower() in ('doc', 'docs'))) and LICENCE_NAME.search(rel[-1])
            reuse = len(rel) == 2 and rel[0] == 'LICENSES'
            if not (top or reuse) or not (m.isfile() or m.issym()):
                continue
            src = m
            if m.issym():  # glib's COPYING is a link into LICENSES/: its text
                src = members.get(os.path.normpath(os.path.join(os.path.dirname(m.name), m.linkname)))
                if src is None or not src.isfile():
                    continue
            out = dest.joinpath(*rel)
            out.parent.mkdir(parents=True, exist_ok=True)
            with t.extractfile(src) as fsrc, open(out, 'wb') as f:
                shutil.copyfileobj(fsrc, f)
            out.chmod(0o644)
            got.append(out)
    return got


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    ap.add_argument('root', type=Path)
    ap.add_argument('--prefix', type=Path, required=True)
    ap.add_argument('--nng', type=Path, required=True)
    ap.add_argument('--build', type=Path, action='append', default=[], help='trees KiCad\'s own objects are built or installed in')
    ap.add_argument('--cache', type=Path, default=None, help='where source archives are kept between runs')
    a = ap.parse_args()
    root = a.root.resolve(strict=True)
    app = root / 'KiCad.app'
    cache = a.cache or root.parent / 'licence-sources'
    kg, linked = kegs(a.prefix)
    # a keg's Mach-Os live under lib/, bin/, libexec/ or Frameworks/ (not its headers and docs: boost's and llvm's are large)
    by_name = index([k / d for k in kg for d in ('lib', 'bin', 'libexec', 'Frameworks') if (k / d).is_dir()])
    nng = index([a.nng]) if a.nng.is_dir() else {}
    own = index([b for b in a.build if b.is_dir()])
    bundled, used, unknown = [], {}, []
    for p in sorted(app.rglob('*')):
        if not is_macho(p):
            continue
        rel = p.relative_to(root).as_posix()
        hits = by_name.get(p.name, [])
        kegs_hit = sorted({k for h in hits for k in kg if k in h.parents})
        if len(kegs_hit) > 1:  # the version $prefix/opt links, then the one whose LC_UUID is this file's
            kegs_hit = [k for k in kegs_hit if k in linked] or kegs_hit
        if len(kegs_hit) > 1:
            want = uuids(p)
            kegs_hit = sorted({k for h in hits for k in kg if k in h.parents and uuids(h.resolve()) == want}) or kegs_hit
        if kegs_hit:
            keg = kegs_hit[0]
            formula, version = kg[keg]
            used.setdefault(formula, (keg, version, []))[2].append(p.name)
            bundled.append((rel, 'homebrew:' + formula + ' ' + version))
        elif p.name in nng:
            bundled.append((rel, 'nng'))
        elif p.name in own or p.parent.name != 'Frameworks' or not a.build:
            # with no build tree given (a package checked after the build), what is from neither Homebrew nor nng is KiCad's own
            bundled.append((rel, 'kicad'))
        else:
            unknown.append(rel)
            bundled.append((rel, 'unknown'))
    info = {}
    if used:
        r = subprocess.run(['brew', 'info', '--json=v2', *sorted(used)], text=True, stdout=subprocess.PIPE)
        if r.returncode == 0:
            info = {f['name']: f for f in json.loads(r.stdout).get('formulae', [])}
    lic_root = root / 'licenses'
    hb = lic_root / 'homebrew'
    if hb.exists():
        shutil.rmtree(hb)
    hb.mkdir(parents=True)
    rows, problems = [], []
    for formula in sorted(used):
        keg, version, names = used[formula]
        spdx = info.get(formula, {}).get('license') or ''
        dest = hb / formula
        dest.mkdir()
        files, came = [], []
        for src in licence_files(keg):
            out = dest / src.relative_to(keg)
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, out)
            out.chmod(0o644)
            files.append(out)
        if files:
            came.append(str(keg))
        else:
            url = source_url(keg, formula)
            if url:
                try:
                    files = texts_from_source(url, cache, dest)
                except Exception as e:
                    problems.append(f'{formula} {version}: no licence in the keg and its source could not be read ({url}: {e})')
                if files:
                    came.append(url)
            if not files and spdx.lower() in PUBLIC_DOMAIN:
                note = dest / 'PUBLIC-DOMAIN.txt'
                home = info.get(formula, {}).get('homepage') or ''
                note.write_text(f'{formula} {version} is in the public domain (Homebrew licence: {spdx}); it carries no licence text.\n'
                                f'{home}\n')
                files = [note]
                came.append('note: public domain (' + spdx + ')')
        if not files:
            problems.append(f'{formula} {version}: no licence file in the keg ({keg}) nor at the top of its source')
        rows.append((formula, version, spdx, ' '.join(came) or '-', ' '.join(f.relative_to(lic_root).as_posix() for f in files) or '-',
                     ' '.join(sorted(set(names)))))
    (hb / 'LIBRARIES.tsv').write_text('library\tversion\tlicence\ttext from\tfiles\tbundled as\n' + ''.join('\t'.join(r) + '\n' for r in rows))
    (lic_root / 'BUNDLED.tsv').write_text('path\tbuilt from\n' + ''.join(f'{p}\t{o}\n' for p, o in bundled))
    if info:
        (lic_root / 'homebrew-packages.json').write_text(json.dumps({'formulae': [info[f] for f in sorted(info)]}, indent=1) + '\n')
    for r in rows:
        print(f'{r[0]} {r[1]} ({r[2] or "?"}): {len(r[4].split()) if r[4] != "-" else 0} file(s) from {r[3]}')
    print(f'{len(rows)} Homebrew formulas bundled; {sum(1 for _, o in bundled if o == "kicad")} KiCad objects; '
          f'{sum(1 for _, o in bundled if o == "nng")} nng; {len(unknown)} of unknown origin')
    for u in unknown:
        problems.append(f'{u}: a Mach-O in Frameworks from no Homebrew keg, nng or this build')
    if problems:
        sys.exit('licences:\n  ' + '\n  '.join(problems))


if __name__ == '__main__':
    main()
