#!/usr/bin/env python3
"""Unit checks of the licence steps' lookups (no package needed): run by the release workflows before the steps themselves."""
import importlib.util
import sys
import tempfile
from pathlib import Path

here = Path(__file__).resolve().parent


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), here / (name + '.py'))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


fails = []


def check(ok, what):
    print(('ok   ' if ok else 'FAIL ') + what)
    if not ok:
        fails.append(what)


lin = load('backplane-linux-licences')
idx = {
    '/usr/lib/x86_64-linux-gnu/libpulse.so.0': 'libpulse0:amd64',
    # b2's failure: a library in a private subdirectory of the multiarch dir
    '/usr/lib/x86_64-linux-gnu/pulseaudio/libpulsecommon-17.0.so': 'libpulse0:amd64',
    '/usr/lib/x86_64-linux-gnu/ngspice/analog.cm': 'libngspice0:amd64',
    '/lib/x86_64-linux-gnu/libc.so.6': 'libc6:amd64',
    '/usr/lib/x86_64-linux-gnu/libc.so.6': 'libc6:amd64',  # usrmerge: dpkg_index() adds both spellings
}
copied = {
    'libpulse.so.0': '/usr/lib/x86_64-linux-gnu/libpulse.so.0',
    'libpulsecommon-17.0.so': '/usr/lib/x86_64-linux-gnu/pulseaudio/libpulsecommon-17.0.so',
    'analog.cm': '/usr/lib/x86_64-linux-gnu/ngspice/analog.cm',
    'libc.so.6': '/lib/x86_64-linux-gnu/libc.so.6',
    'libnowhere.so.1': '/opt/elsewhere/libnowhere.so.1',
}
check(lin.package_of('libpulsecommon-17.0.so', copied, idx) == ('libpulse0:amd64', None),
      'Linux: a library from a subdirectory (pulseaudio/) maps to its package by the exact path it was copied from')
check(lin.package_of('analog.cm', copied, idx) == ('libngspice0:amd64', None), 'Linux: an ngspice code model maps to libngspice0')
check(lin.package_of('libc.so.6', copied, idx) == ('libc6:amd64', None), 'Linux: /lib and /usr/lib (usrmerge) both map')
check(lin.package_of('libmissing.so.1', copied, idx)[0] is None, 'Linux: a library the workflow did not list is unknown, not guessed')
check(lin.package_of('libnowhere.so.1', copied, idx)[0] is None, 'Linux: a library copied from outside any package is unknown')

com = load('backplane_licences')
with tempfile.TemporaryDirectory() as t:
    t = Path(t)
    for name in ('LICENSE.BSD', 'BOOST.LICENSE_1_0.txt', 'COPYING.LIB', 'license.cpp', 'README.md', 'APPLE_LICENSE'):
        (t / name).write_text('x')
    got = sorted(p.name for p in t.iterdir() if com.is_notice(p))
check(got == ['APPLE_LICENSE', 'BOOST.LICENSE_1_0.txt', 'COPYING.LIB', 'LICENSE.BSD'],
      f'vendored notices: names starting so with any suffix, elsewhere only in text files ({got})')

win = load('backplane-windows-licences')
check(bool(win.MS_RUNTIME.match('VCRUNTIME140_1.dll')) and bool(win.MS_RUNTIME.match('api-ms-win-crt-runtime-l1-1-0.dll'))
      and not win.MS_RUNTIME.match('zlib1.dll'), 'Windows: Microsoft runtime DLL names, and not a port\'s')

print(f'{len(fails)} failed' if fails else 'PASS licence lookups')
sys.exit(1 if fails else 0)
