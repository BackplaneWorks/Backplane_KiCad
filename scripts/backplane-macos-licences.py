#!/usr/bin/env python3
"""Put the licence of every library the macOS package bundles into the package, and say where each came from.

Every Mach-O file in KiCad.app is traced to where it was built: a Homebrew keg (found by name among the kegs `$prefix/opt` points at,
the versions the build linked), the nng 1.x install, or this source tree (KiCad's own). For each Homebrew formula that is bundled, its
licence files are copied from the keg into licenses/homebrew/<formula>/, keeping their paths inside the keg (so two files of one name,
or names that differ only in case, never land on each other). A keg with no licence text of its own takes it from the formula's source
at the bundled version (the URL in the keg's own .brew/<formula>.rb); a public-domain library with no text anywhere gets a note.

What the build compiled in without a Mach-O to show for it (header-only and static libraries) is read from the build tree: every
header in ninja's deps log and every archive in build.ninja that is in a Homebrew keg gives that formula a row ('header-only' or
'static'), with the same text-or-fail rule. KiCad's vendored libraries (thirdparty/) have their notices copied with their paths into
licenses/vendor/thirdparty/, as the Linux release does, plus the notices that live in a README or a header; a vendored library the build
compiled in with no notice fails the run.

Writes, beside the licences:
  licenses/LIBRARIES.tsv  library, version, licence (Homebrew's SPDX), where the text came from, how it is linked, the files, the Mach-Os
  licenses/BUNDLED.tsv    every Mach-O in KiCad.app, by its path in the bundle, and what it was built from

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


# Vendored libraries whose notice is in a README or at the top of a source file, not in a LICENSE/COPYING/NOTICE/COPYRIGHT file:
# {thirdparty dir: [(file, 'whole' | 'head')]} ('head': the file's leading comment block)
VENDOR_EXTRA = {
    'glad': [('include/glad/gl.h', 'head')],
    'lemon': [('README.md', 'whole')],
    'planegcs': [('README.kicad.md', 'whole')],
    '3dxware_sdk': [('README.txt', 'whole')],
    'vma': [('vk_mem_alloc.h', 'head')],
}
# a notice file: its name says licence/copying/notice/copyright anywhere (turtle's BOOST.LICENSE_1_0.txt), and it is no source file
VENDOR_NAME = re.compile(r'licen[cs]e|copying|notice|copyright', re.I)
NOTICE_SUFFIX = ('', '.txt', '.md', '.rst', '.markdown')
SOURCE_SUFFIX = ('.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx', '.inl', '.py', '.js', '.java', '.cmake', '.in', '.sh')


def leading_comment(path):
    out = []
    for line in path.read_text(errors='replace').splitlines():
        t = line.strip()
        if t.startswith(('//', '/*', '*', '*/')) or (not t and out):
            out.append(line)
        elif out:
            break
    return '\n'.join(out).rstrip() + '\n'


def build_inputs(build, prefix_paths):
    """Files the build compiled or linked from outside its own tree: every header in ninja's deps log and every archive (.a) named in
    build.ninja, as real paths. Only the ones under the given roots are kept."""
    seen = set()
    r = subprocess.Popen(['ninja', '-C', str(build), '-t', 'deps'], text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    for line in r.stdout:
        if line.startswith('    '):
            seen.add(line.strip())
    r.wait()
    for bn in [build / 'build.ninja', *build.glob('CMakeFiles/rules.ninja')]:
        if bn.is_file():
            for tok in re.findall(r'(?<![\w/.-])(/[^\s:$]+\.a)\b', bn.read_text(errors='replace')):
                seen.add(tok)
    out = set()
    for f in seen:
        f = f if os.path.isabs(f) else os.path.normpath(os.path.join(str(build), f))
        if any(f.startswith(str(pp) + '/') for pp in prefix_paths):
            out.add(os.path.realpath(f))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    ap.add_argument('root', type=Path)
    ap.add_argument('--prefix', type=Path, required=True)
    ap.add_argument('--nng', type=Path, required=True)
    ap.add_argument('--build', type=Path, action='append', default=[], help='trees KiCad\'s own objects are built or installed in; '
                    'the first with a build.ninja is also read for the headers and archives the build compiled in')
    ap.add_argument('--source', type=Path, default=None, help='the KiCad source tree (thirdparty/ notices)')
    ap.add_argument('--cache', type=Path, default=None, help='where source archives are kept between runs')
    a = ap.parse_args()
    root = a.root.resolve(strict=True)
    app = root / 'KiCad.app'
    cache = a.cache or root.parent / 'licence-sources'
    kg, linked = kegs(a.prefix)
    # a keg's Mach-Os live under lib/, bin/, libexec/ or Frameworks/ (not its headers and docs: boost's and llvm's are large)
    by_name = index([k / d for k in kg for d in ('lib', 'bin', 'libexec', 'Frameworks') if (k / d).is_dir()])
    nng = index([a.nng]) if a.nng.is_dir() else {}
    builds = [b for b in a.build if b.is_dir()]
    own = index(builds)
    bundled, used, unknown, problems = [], {}, [], []
    # how each formula is in the package: 'dylib' (a Mach-O of its keg), 'static' (an archive of its keg linked), 'headers' (compiled in)
    how = {}

    def keg_of(path):
        hit = [k for k in kg if str(path).startswith(str(k) + '/')]
        return hit[0] if hit else None

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
            how.setdefault(formula, set()).add('dylib')
            bundled.append((rel, 'homebrew:' + formula + ' ' + version))
        elif p.name in nng:
            bundled.append((rel, 'nng'))
        elif p.name in own or not builds:
            # with no build tree given (a package checked after the build), what is from neither Homebrew nor nng is KiCad's own
            bundled.append((rel, 'kicad'))
        else:
            unknown.append(rel)
            bundled.append((rel, 'unknown'))
    # what the build compiled in from Homebrew and from thirdparty/: headers (ninja's deps log) and static archives (build.ninja)
    vendored_used = set()
    ninja_dir = next((b for b in builds if (b / 'build.ninja').is_file()), None)
    if ninja_dir is not None:
        src = (a.source or Path.cwd()).resolve()
        for f in build_inputs(ninja_dir, [a.prefix, Path(os.path.realpath(a.prefix)), src / 'thirdparty']):
            keg = keg_of(f)
            if keg is not None:
                formula, version = kg[keg]
                used.setdefault(formula, (keg, version, []))
                how.setdefault(formula, set()).add('static' if f.endswith('.a') else 'headers')
            elif f.startswith(str(src / 'thirdparty') + '/'):
                vendored_used.add(Path(f).relative_to(src / 'thirdparty').parts[0])
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
    rows = []

    def linked_as(formula):
        h = how.get(formula, set())
        if 'dylib' in h:
            return 'dylib' + (' (and its headers compiled in)' if 'headers' in h else '')
        if 'static' in h:
            return 'static'
        return 'header-only'

    for formula in sorted(used):
        keg, version, names = used[formula]
        spdx = info.get(formula, {}).get('license') or ''
        dest = hb / formula
        dest.mkdir()
        files, came = [], []
        for src_f in licence_files(keg):
            out = dest / src_f.relative_to(keg)
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src_f, out)
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
            problems.append(f'{formula} {version} ({linked_as(formula)}): no licence file in the keg ({keg}) nor at the top of its source')
        rows.append(('homebrew:' + formula, version, spdx or '-', ' '.join(came) or '-', linked_as(formula),
                     ' '.join(f.relative_to(lic_root).as_posix() for f in files) or '-', ' '.join(sorted(set(names))) or '-'))
    # KiCad's vendored libraries: every LICENSE/COPYING/NOTICE/COPYRIGHT file under thirdparty/, paths kept (as the Linux release does),
    # and the notices that live in a README or a source file's head
    if a.source is not None:
        tp = a.source.resolve() / 'thirdparty'
        vend = lic_root / 'vendor' / 'thirdparty'
        if vend.exists():
            shutil.rmtree(vend)
        vend.mkdir(parents=True)
        per = {}
        for f in sorted(tp.rglob('*')):
            if f.is_file() and f.suffix.lower() not in SOURCE_SUFFIX and (VENDOR_NAME.match(f.name) or (VENDOR_NAME.search(f.name) and f.suffix.lower() in NOTICE_SUFFIX)):
                out = vend / f.relative_to(tp)
                out.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(f, out); out.chmod(0o644)
                per.setdefault(f.relative_to(tp).parts[0], []).append(out)
        for d, extra in VENDOR_EXTRA.items():
            for name, mode in extra:
                f = tp / d / name
                if not f.is_file():
                    continue
                out = vend / d / (name if mode == 'whole' else name + '.NOTICE.txt')
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_text(f.read_text(errors='replace') if mode == 'whole' else leading_comment(f)); out.chmod(0o644)
                per.setdefault(d, []).append(out)
        for d in sorted(p.name for p in tp.iterdir() if p.is_dir()):
            compiled = d in vendored_used
            files = per.get(d, [])
            if compiled and not files:
                problems.append(f'thirdparty/{d}: compiled in, with no notice')
            rows.append(('vendored:' + d, '-', '-', 'thirdparty/' + d if files else '-',
                         'compiled in' if compiled else ('not compiled in' if ninja_dir is not None else 'vendored'),
                         ' '.join(f.relative_to(lic_root).as_posix() for f in files) or '-', '-'))
    if any(o == 'nng' for _, o in bundled):
        rows.append(('nng', (a.nng / 'SOURCE_COMMIT').read_text().strip() if (a.nng / 'SOURCE_COMMIT').is_file() else '-', 'MIT',
                     'nng source', 'dylib', 'nng/LICENSE.txt', ' '.join(sorted({Path(p).name for p, o in bundled if o == 'nng'}))))
    kicad_lic = sorted(f.name for f in lic_root.iterdir() if f.is_file() and f.name.upper().startswith(('LICENSE', 'COPYING')))
    rows.append(('kicad', '-', 'GPL-3.0-or-later', 'this source', 'the program', ' '.join(kicad_lic) or '-', '-'))
    head = 'library\tversion\tlicence\ttext from\tlinked as\tfiles\tbundled as\n'
    (lic_root / 'LIBRARIES.tsv').write_text(head + ''.join('\t'.join(r) + '\n' for r in rows))
    (lic_root / 'BUNDLED.tsv').write_text('path\tbuilt from\n' + ''.join(f'{p}\t{o}\n' for p, o in bundled))
    if info:
        (lic_root / 'homebrew-packages.json').write_text(json.dumps({'formulae': [info[f] for f in sorted(info)]}, indent=1) + '\n')
    for r in rows:
        n = len(r[5].split()) if r[5] != '-' else 0
        print(f'{r[0]} {r[1]} ({r[2]}), {r[4]}: {n} file(s) from {r[3]}')
    hb_rows = [r for r in rows if r[0].startswith('homebrew:')]
    print(f'{len(hb_rows)} Homebrew formulas ({sum(1 for r in hb_rows if r[4].startswith("dylib"))} dylib, '
          f'{sum(1 for r in hb_rows if r[4] == "static")} static, {sum(1 for r in hb_rows if r[4] == "header-only")} header-only); '
          f'{len(vendored_used)} vendored libraries compiled in; {sum(1 for _, o in bundled if o == "kicad")} KiCad objects; '
          f'{sum(1 for _, o in bundled if o == "nng")} nng; {len(unknown)} of unknown origin'
          + ('' if ninja_dir is not None else ' (no build tree: headers and archives not read)'))
    for u in unknown:
        problems.append(f'{u}: a Mach-O from no Homebrew keg, nng or this build')
    if problems:
        sys.exit('licences:\n  ' + '\n  '.join(problems))


if __name__ == '__main__':
    main()
