#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check an unpacked runtime package carries the pinned KiCad libraries (scripts/backplane-libraries.sh).

    backplane-library-package-check.py <runtime root> [--with-3d]

The runtime root is the package's top folder (bin/, share/ or KiCad.app/, licenses/).  Checks:
the three default global tables exist and nest ${KICAD10_TEMPLATE_DIR}/<table>; the template
tables they name exist; Device and Capacitor_SMD:C_0805_2012Metric are there; the Backplane
library is there and the footprint table names it; each library's licence folder has its
BACKPLANE_SOURCE.txt and licence files, and KICAD_LIBRARY_SOURCES.txt names the same pins as
scripts/backplane-libraries.sh; no Git LFS pointer; no table has a CR.
"""

import argparse
from pathlib import Path
import re
import sys

SOURCE = Path(__file__).resolve().parents[1]


def pins() -> dict:
    text = (SOURCE / "scripts/backplane-libraries.sh").read_text(encoding="utf-8")
    values = dict(re.findall(r"^([A-Z0-9_]+)=(\S+)$", text, re.M))
    return {
        "kicad-symbols": values["KICAD_SYMBOLS_COMMIT"],
        "kicad-footprints": values["KICAD_FOOTPRINTS_COMMIT"],
        "kicad-templates": values["KICAD_TEMPLATES_COMMIT"],
        "kicad-packages3D": values["KICAD_3DMODELS_COMMIT"],
        "backplane-kicad-library": values["BACKPLANE_LIBRARY_COMMIT"],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("root", type=Path)
    parser.add_argument("--with-3d", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    mac = root / "KiCad.app/Contents/SharedSupport"
    data = mac if mac.is_dir() else root / "share/kicad"
    problems = []

    def need(condition, message):
        if not condition:
            problems.append(message)

    for table, head in (("sym-lib-table", "sym_lib_table"), ("fp-lib-table", "fp_lib_table"),
                        ("design-block-lib-table", "design_block_lib_table")):
        path = data / table
        need(path.is_file() and path.stat().st_size > 0, f"no global {table}")
        if not path.is_file():
            continue
        raw = path.read_bytes()
        need(b"\r" not in raw, f"{table} has CR line endings")
        text = raw.decode("utf-8")
        need(text.lstrip().startswith("(" + head), f"{table} is not a {head}")
        if table != "design-block-lib-table":
            need(f'(type "Table")(uri "${{KICAD10_TEMPLATE_DIR}}/{table}")' in text,
                 f"{table} does not nest ${{KICAD10_TEMPLATE_DIR}}/{table}")
            template = data / "template" / table
            need(template.is_file() and template.stat().st_size > 1000, f"no template/{table}")
            if template.is_file():
                need(b"\r" not in template.read_bytes(), f"template/{table} has CR line endings")
    fp_table = (data / "fp-lib-table").read_text(encoding="utf-8") if (data / "fp-lib-table").is_file() else ""
    need('(name "Backplane_Logo")' in fp_table and "${BACKPLANE_KICAD_LIBRARY}/libraries/" in fp_table,
         "fp-lib-table does not name the Backplane library")
    template_text = (data / "template/sym-lib-table").read_text(encoding="utf-8") \
        if (data / "template/sym-lib-table").is_file() else ""
    need('(name "Device")' in template_text and "${KICAD10_SYMBOL_DIR}/Device.kicad_sym" in template_text,
         "template/sym-lib-table does not name Device")

    device = data / "symbols/Device.kicad_symdir"
    need((device / "C.kicad_sym").is_file() or (data / "symbols/Device.kicad_sym").is_file(),
         "no Device symbol library with C")
    need((data / "footprints/Capacitor_SMD.pretty/C_0805_2012Metric.kicad_mod").is_file(),
         "no Capacitor_SMD.pretty/C_0805_2012Metric.kicad_mod")
    need(any((data / "template").glob("*.kicad_wks")), "no worksheets in template/")
    need((data / "backplane-library/libraries/Backplane_Logo.pretty").is_dir(), "no Backplane_Logo.pretty")
    if args.with_3d:
        need(any((data / "3dmodels").glob("*.3dshapes")), "no 3D models")
    else:
        need(not (data / "3dmodels").exists() or not any((data / "3dmodels").glob("*.3dshapes")),
             "3D models present: this package is not meant to carry them yet")

    expected = pins()
    names = ["kicad-symbols", "kicad-footprints", "kicad-templates", "backplane-kicad-library"]
    if args.with_3d:
        names.append("kicad-packages3D")
    licences = root / "licenses/kicad-libraries"
    for name in names:
        folder = licences / name
        source = folder / "BACKPLANE_SOURCE.txt"
        need(source.is_file() and f"commit={expected[name]}" in source.read_text(encoding="utf-8"),
             f"licenses/kicad-libraries/{name}/BACKPLANE_SOURCE.txt missing or not commit {expected[name]}")
        need(any(p.name.upper().startswith(("LICENSE", "COPYING")) for p in folder.glob("*")) if folder.is_dir() else False,
             f"licenses/kicad-libraries/{name} has no licence file")
    sources = root / "KICAD_LIBRARY_SOURCES.txt"
    need(sources.is_file(), "no KICAD_LIBRARY_SOURCES.txt")
    if sources.is_file():
        text = sources.read_text(encoding="utf-8")
        for name in names:
            need(f"commit={expected[name]}" in text, f"KICAD_LIBRARY_SOURCES.txt does not name {name} {expected[name]}")

    pointers = [p for p in data.rglob("*") if p.is_file() and p.stat().st_size < 200
                and p.read_bytes().startswith(b"version https://git-lfs.github.com/spec/v1")]
    need(not pointers, f"Git LFS pointers: {pointers[:3]}")

    if problems:
        print("FAIL library package check:\n  " + "\n  ".join(problems), file=sys.stderr)
        sys.exit(1)
    symbols = sum(1 for _ in (data / "symbols").glob("*.kicad_symdir"))
    footprints = sum(1 for _ in (data / "footprints").glob("*.pretty"))
    print(f"PASS library package: {data.relative_to(root)} has {symbols} symbol and {footprints} footprint "
          f"libraries, the templates, the three global tables, the Backplane library; licences and "
          f"KICAD_LIBRARY_SOURCES.txt for {', '.join(names)}", flush=True)


if __name__ == "__main__":
    main()
