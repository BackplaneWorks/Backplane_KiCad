"""Shared by the Linux and Windows licence steps (scripts/backplane-{linux,windows}-licences.py): the build's inputs from outside its
tree, KiCad's vendored notices, and the list files the package check reads (scripts/backplane-licence-package-check.py)."""
import os
import re
import shutil
import subprocess
from pathlib import Path

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


def is_notice(f):
    # LICENSE.BSD, COPYING.LIB: a name that starts so is a notice whatever its suffix (but not a source file: license.cpp);
    # elsewhere in the name (BOOST.LICENSE_1_0.txt, APPLE_LICENSE), only a text file
    suffix = f.suffix.lower()
    if suffix in SOURCE_SUFFIX:
        return False
    return VENDOR_NAME.match(f.name) is not None or (VENDOR_NAME.search(f.name) is not None and suffix in NOTICE_SUFFIX)


LIST_HEAD = 'library\tversion\tlicence\ttext from\tlinked as\tfiles\tbundled as\n'


def gather(build):
    """Every header in ninja's deps log and every archive/import library named in build.ninja, as absolute paths (one per line)."""
    seen = set()
    ninja = 'ninja'
    cache = build / 'CMakeCache.txt'
    if cache.is_file():  # the ninja CMake used (on Windows, Visual Studio's, which need not be on PATH)
        m = re.search(r'^CMAKE_MAKE_PROGRAM:[A-Z]+=(.+)$', cache.read_text(errors='replace'), re.M)
        if m and Path(m.group(1).strip()).is_file():
            ninja = m.group(1).strip()
    p = subprocess.Popen([ninja, '-C', str(build), '-t', 'deps'], text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    for line in p.stdout:
        if line.startswith('    '):
            f = line.strip()
            seen.add(f if os.path.isabs(f) else os.path.normpath(os.path.join(str(build), f)))
    p.wait()
    bn = build / 'build.ninja'
    if bn.is_file():
        text = bn.read_text(errors='replace')
        seen.update(re.findall(r'(?<![\w/.-])(/[^\s:$]+\.a)\b', text))
        seen.update(m.replace('$:', ':') for m in re.findall(r'([A-Za-z]\$?:[\\/][^\s"]+\.lib)\b', text))
    return sorted(seen)


def leading_comment(path):
    out = []
    for line in path.read_text(errors='replace').splitlines():
        t = line.strip()
        if t.startswith(('//', '/*', '*', '*/')) or (not t and out):
            out.append(line)
        elif out:
            break
    return '\n'.join(out).rstrip() + '\n'


def vendored(lic, source, used, have_inputs, problems):
    """KiCad's thirdparty/ notices in licenses/vendor/thirdparty/, paths kept (every notice file, and the README/header notices), and
    one row per vendored library: compiled in (its files among the build's inputs) or not."""
    tp = source / 'thirdparty'
    vend = lic / 'vendor' / 'thirdparty'
    vend.mkdir(parents=True, exist_ok=True)
    for f in sorted(tp.rglob('*')):
        if f.is_file() and is_notice(f):
            out = vend / f.relative_to(tp)
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(f, out)
    for d, extra in VENDOR_EXTRA.items():
        for name, mode in extra:
            f = tp / d / name
            if f.is_file():
                out = vend / d / (name if mode == 'whole' else name + '.NOTICE.txt')
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_text(f.read_text(errors='replace') if mode == 'whole' else leading_comment(f))
    rows = []
    for d in sorted(p.name for p in tp.iterdir() if p.is_dir()):
        files = sorted(x.relative_to(lic).as_posix() for x in (vend / d).rglob('*') if x.is_file()) if (vend / d).is_dir() else []
        compiled = d in used
        if compiled and not files:
            problems.append(f'thirdparty/{d}: compiled in, with no notice')
        rows.append(('vendored:' + d, '-', '-', 'thirdparty/' + d if files else '-',
                     'compiled in' if compiled else ('not compiled in' if have_inputs else 'vendored'), ' '.join(files) or '-', '-'))
    return rows


def thirdparty_used(inputs, source):
    tp = os.path.normcase(str(source / 'thirdparty')) + os.sep
    out = set()
    for f in inputs:
        n = os.path.normcase(os.path.normpath(f))
        if n.startswith(tp):
            out.add(n[len(tp):].replace('\\', '/').split('/')[0])
    return out


def kicad_row(lic):
    files = sorted(f.name for f in lic.iterdir() if f.is_file() and f.name.upper().startswith(('LICENSE', 'COPYING', 'AUTHORS')))
    return ('kicad', '-', 'GPL-3.0-or-later', 'this source', 'the program', ' '.join(files) or '-', '-')


def write_lists(lic, rows, bundled):
    (lic / 'LIBRARIES.tsv').write_text(LIST_HEAD + ''.join('\t'.join(r) + '\n' for r in rows))
    (lic / 'BUNDLED.tsv').write_text('path\tbuilt from\n' + ''.join(f'{p}\t{o}\n' for p, o in bundled))
