#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check an unpacked runtime package carries ngspice and its code models where KiCad looks.

    backplane-ngspice-package-check.py <runtime root>

The runtime root is the package's top folder (bin/ and lib/, or KiCad.app/).  ngspice is loaded by
name at run time and its code models (*.cm: the XSPICE analog, digital and mixed-signal parts) are
files it loads itself, so neither appears in any import table and a package can lose them without
a build error.  Checks, on every platform:

- the ngspice shared library is in the folder the runtime loads its libraries from;
- spice2poly.cm (the file KiCad's loader looks for, eeschema/sim/ngspice.cpp findCmPath) and at
  least the analog and digital code models are in one of the folders that loader searches,
  relative to the executables: bin/ngspice or lib/ngspice (Windows, Linux),
  Contents/PlugIns/sim/ngspice or Contents/Frameworks (macOS).

That the code models actually load is scripts/backplane-sim-headless-check.py's XSPICE cases.
"""

import argparse
from pathlib import Path
import sys


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    failures = []

    def need(condition, message):
        print(("ok   " if condition else "FAIL ") + message)
        if not condition:
            failures.append(message)

    app = root / "KiCad.app"
    if app.is_dir():
        platform = "macos"
        library_folders = [app / "Contents/Frameworks"]
        library_patterns = ["libngspice*.dylib"]
        # NGSPICE::findCmPath: <plugins dir>/sim/ngspice first; the others are relative to the
        # executable's folder (Contents/MacOS)
        model_folders = [app / "Contents/PlugIns/sim/ngspice", app / "Contents/Plugins/sim/ngspice",
                         app / "Contents/Frameworks", app / "Contents/lib/ngspice",
                         app / "Contents/MacOS/ngspice", app / "Contents/MacOS/Contents/Frameworks"]
    elif (root / "bin/kicad-cli.exe").is_file():
        platform = "windows"
        library_folders = [root / "bin"]
        library_patterns = ["ngspice.dll", "libngspice-*.dll"]
        # relative to bin (the executables' folder): "ngspice", "../lib/ngspice"
        model_folders = [root / "bin/ngspice", root / "lib/ngspice", root / "bin/lib/ngspice"]
    else:
        platform = "linux"
        library_folders = [root / "lib"]
        library_patterns = ["libngspice.so*"]
        model_folders = [root / "lib/ngspice", root / "bin/ngspice", root / "bin/lib/ngspice"]

    libraries = [path for folder in library_folders for pattern in library_patterns
                 for path in folder.glob(pattern)]
    need(bool(libraries), f"{platform}: the ngspice library is in {', '.join(str(f.relative_to(root)) for f in library_folders)}"
         f" ({', '.join(p.name for p in libraries) or 'none of ' + ' '.join(library_patterns)})")

    found = next((folder for folder in model_folders if (folder / "spice2poly.cm").is_file()), None)
    need(found is not None,
         "spice2poly.cm is in a folder KiCad's loader searches ("
         + (str(found.relative_to(root)) if found else "none of: "
            + ", ".join(str(f.relative_to(root)) for f in model_folders)) + ")")
    models = sorted(path.name for path in found.glob("*.cm")) if found else []
    need(len(models) >= 1, f"at least one code model is there ({len(models)}: {', '.join(models)})")
    for name in ("analog.cm", "digital.cm"):
        need(name in models, f"{name} is among them")
    stray = sorted(str(path.relative_to(root)) for path in root.rglob("*.cm")
                   if found is None or path.parent != found)
    if stray and found is None:
        print("     code models found elsewhere, where the loader does not look: " + ", ".join(stray[:8]))

    if failures:
        print(f"NGSPICE_PACKAGE_FAILED {len(failures)}")
        sys.exit(1)
    print("NGSPICE_PACKAGE_OK")


if __name__ == "__main__":
    main()
