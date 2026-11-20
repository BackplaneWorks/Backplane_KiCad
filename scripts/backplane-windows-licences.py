#!/usr/bin/env python3
"""The Windows package's licence list: what every DLL and EXE in it was built from, and the text of every library it ships.

backplane-windows.ps1 already copies each vcpkg port's `copyright` into licenses/vcpkg/<port>/ and KiCad's thirdparty/ notices into
licenses/vendor/. This traces, from vcpkg's own install lists (vcpkg_installed/vcpkg/info/*.list):
  - every DLL/EXE in the runtime to its port (or to KiCad's build, or to Microsoft's runtime);
  - every header in ninja's deps log and every .lib named in build.ninja (--inputs, from --gather) to its port: a port reached only that
    way is "header-only" or "static";
and writes licenses/LIBRARIES.tsv (library, version, licence, where the text came from, how it is linked, the files, the DLLs) and
licenses/BUNDLED.tsv. A port that ships code with no copyright file fails the run; one that ships nothing (boost-uninstall, the pthread
meta port) is listed as such. Microsoft's C/C++ runtime DLLs get a row pointing at the Visual Studio redistribution terms.

usage: backplane-windows-licences.py STAGE --installed VCPKG_INSTALLED_TRIPLET_DIR --build BUILD_DIR --source SOURCE [--inputs FILE]
       backplane-windows-licences.py --gather BUILD_DIR > FILE
"""
import argparse
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import backplane_licences as common  # noqa: E402

# Microsoft's C/C++ runtime: the Universal CRT and the Visual C++ redistributable (no licence text needed; their terms are Microsoft's)
MS_RUNTIME = re.compile(r'^(api-ms-win-.*|ucrtbase|msvcp140.*|vcruntime140.*|concrt140|vccorlib140.*)\.dll$', re.I)
MS_TERMS = 'https://learn.microsoft.com/visualstudio/releases/2022/redistribution'


def port_lists(installed):
    """{port: (version, [installed paths relative to vcpkg_installed])} from vcpkg's info/*.list."""
    info = installed.parent / 'vcpkg' / 'info'
    out = {}
    for lst in sorted(info.glob('*.list')) if info.is_dir() else []:
        m = re.match(r'^(.+?)_([^_]+)_([^_]+)\.list$', lst.name)
        if not m:
            continue
        out[m.group(1)] = (m.group(2), [l.strip() for l in lst.read_text(errors='replace').splitlines() if l.strip()])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    ap.add_argument('stage', type=Path, nargs='?')
    ap.add_argument('--gather', type=Path)
    ap.add_argument('--installed', type=Path)
    ap.add_argument('--build', type=Path)
    ap.add_argument('--source', type=Path, default=Path.cwd())
    ap.add_argument('--inputs', type=Path)
    a = ap.parse_args()
    if a.gather:
        print('\n'.join(common.gather(a.gather)))
        return
    stage = a.stage.resolve(strict=True)
    installed = a.installed.resolve(strict=True)
    root_vi = installed.parent  # vcpkg_installed
    lic = stage / 'licenses'
    src = a.source.resolve()
    problems = []
    ports = port_lists(installed)
    by_file = {}
    for port, (ver, files) in ports.items():
        for f in files:
            by_file[os.path.normcase(os.path.normpath(str(root_vi / f)))] = port
    dll_port = {}
    for port, (ver, files) in ports.items():
        for f in files:
            if f.lower().endswith(('.dll', '.exe', '.cm')) and '/debug/' not in f.lower():
                dll_port.setdefault(Path(f).name.lower(), port)
    # what KiCad's build made: its DLLs, EXEs and KIFACEs (outside vcpkg_installed)
    own = set()
    if a.build and a.build.is_dir():
        for dp, dn, fn in os.walk(a.build):
            if 'vcpkg_installed' in dn:
                dn.remove('vcpkg_installed')
            own.update(n.lower() for n in fn if n.lower().endswith(('.dll', '.exe', '.kiface', '.pyd')))
    bundled, how, dlls = [], {}, {}
    for p in sorted(stage.rglob('*')):
        if p.is_symlink() or not p.is_file() or p.suffix.lower() not in ('.dll', '.exe', '.pyd', '.kiface', '.cm'):
            continue
        with open(p, 'rb') as fh:
            if fh.read(2) != b'MZ':
                continue
        rel = p.relative_to(stage).as_posix()
        n = p.name.lower()
        if n in dll_port:  # a vcpkg port's (the build tree also holds app-local copies of these: the port owns them)
            port = dll_port[n]
            how.setdefault(port, set()).add('dll')
            dlls.setdefault(port, []).append(p.name)
            bundled.append((rel, 'vcpkg:' + port))
        elif MS_RUNTIME.match(p.name):
            bundled.append((rel, 'microsoft:vc-runtime'))
        elif n in own:
            bundled.append((rel, 'kicad'))
        else:
            bundled.append((rel, 'unknown'))
            problems.append(f'{rel}: from no vcpkg port, not KiCad\'s build, not Microsoft\'s runtime')
    inputs = a.inputs.read_text(errors='replace').splitlines() if a.inputs and a.inputs.is_file() else []
    for f in inputs:
        port = by_file.get(os.path.normcase(os.path.normpath(f)))
        if not port:
            continue
        if f.lower().endswith('.lib'):
            if port not in dlls:  # an import library of a port whose DLL ships is just the link to it
                how.setdefault(port, set()).add('static')
        else:
            how.setdefault(port, set()).add('headers')
    rows = []
    for port in sorted(ports):
        ver = ports[port][0]
        h = how.get(port, set())
        cr = lic / 'vcpkg' / port / 'copyright'
        if 'dll' in h:
            linked = 'dll' + (' (and its headers compiled in)' if 'headers' in h else '')
        elif 'static' in h:
            linked = 'static'
        elif 'headers' in h:
            linked = 'header-only'
        else:
            linked = 'not compiled in' if inputs else 'installed'
        if not cr.is_file() and linked not in ('not compiled in', 'installed'):
            problems.append(f'vcpkg:{port} {ver} ({linked}): no copyright file')
        rows.append(('vcpkg:' + port, ver, '-', f'vcpkg_installed/{installed.name}/share/{port}/copyright' if cr.is_file() else '-', linked,
                     cr.relative_to(lic).as_posix() if cr.is_file() else '-', ' '.join(sorted(set(dlls.get(port, [])))) or '-'))
    ms = sorted(Path(p).name for p, o in bundled if o == 'microsoft:vc-runtime')
    if ms:
        note = lic / 'microsoft' / 'REDISTRIBUTION.txt'
        note.parent.mkdir(parents=True, exist_ok=True)
        note.write_text('The Microsoft C/C++ runtime DLLs in bin\\ (the Universal CRT and the Visual C++ runtime) are redistributed under\n'
                        f'the Visual Studio redistribution terms: {MS_TERMS}\n\n' + '\n'.join(ms) + '\n')
        rows.append(('microsoft:vc-runtime', '-', 'Microsoft redistributable', MS_TERMS, 'dll (system runtime)',
                     note.relative_to(lic).as_posix(), ' '.join(ms)))
    rows += common.vendored(lic, src, common.thirdparty_used(inputs, src), bool(inputs), problems)
    rows.append(common.kicad_row(lic))
    common.write_lists(lic, rows, bundled)
    shipped = [r for r in rows if r[4] not in ('not compiled in', 'installed', 'vendored')]
    for r in rows:
        if r[0].startswith('vcpkg:') and not r[4].startswith('dll'):
            print(f'{r[0]} {r[1]}: {r[4]}')
    print(f'{sum(1 for r in rows if r[0].startswith("vcpkg:"))} vcpkg ports ({sum(1 for r in rows if r[0].startswith("vcpkg:") and r[4].startswith("dll"))} dll, '
          f'{sum(1 for r in rows if r[4] == "static")} static, {sum(1 for r in rows if r[4] == "header-only")} header-only); '
          f'{len(ms)} Microsoft runtime DLLs; {sum(1 for _, o in bundled if o == "kicad")} KiCad objects; {len(shipped)} libraries shipped')
    if problems:
        sys.exit('licences:\n  ' + '\n  '.join(problems))


if __name__ == '__main__':
    main()
