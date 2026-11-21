#!/usr/bin/env python3
"""Make a macOS KiCad runtime self-contained.

KiCad's own install (KICAD_APPLE_MAKE_RELOCATEABLE_BUNDLE) gathers what its
binaries link, but a Homebrew build leaves references behind: Python's
extension modules link Homebrew's openssl, xz, sqlite..., and the framework's
site-packages is a symlink into Homebrew. This copies every library a
Mach-O file in the runtime still loads from outside it into
KiCad.app/Contents/Frameworks, points the reference there (@loader_path),
drops outside rpaths, replaces symlinks that leave the runtime, and signs
what it changed (ad hoc). With --check it changes nothing and fails when
anything still points outside.

    backplane-macos-relocate.py RUNTIME_ROOT [--check]
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

SYSTEM = ("/usr/lib/", "/System/")
RELATIVE = ("@rpath/", "@loader_path/", "@executable_path/")
MAGICS = {b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xce\xfa\xed\xfe", b"\xbe\xba\xfe\xca"}


def macho(path: Path) -> bool:
    if path.is_symlink() or not path.is_file():
        return False
    try:
        with path.open("rb") as f:
            return f.read(4) in MAGICS
    except OSError:
        return False


def run(*args: str) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def deps(path: Path) -> list[str]:
    lines = run("otool", "-L", str(path)).splitlines()[1:]
    out = [line.strip().split(" (")[0] for line in lines if line.strip()]
    # a library lists its own install name first
    ident = run("otool", "-D", str(path)).splitlines()[1:]
    return [d for d in out if not (ident and d == ident[0].strip())]


def rpaths(path: Path) -> list[str]:
    found, lines = [], run("otool", "-l", str(path)).splitlines()
    for i, line in enumerate(lines):
        if line.strip() == "cmd LC_RPATH":
            found.append(lines[i + 2].strip().split(" ")[1])
    return found


def outside(ref: str) -> bool:
    return not ref.startswith(SYSTEM) and not ref.startswith(RELATIVE)


def sign(path: Path) -> None:
    subprocess.run(["codesign", "--force", "--sign", "-", str(path)], check=True, capture_output=True)


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    check = "--check" in sys.argv[2:]
    app = root / "KiCad.app"
    frameworks = app / "Contents/Frameworks"
    problems: list[str] = []

    # symlinks that leave the runtime
    for link in [p for p in root.rglob("*") if p.is_symlink()]:
        target = Path(os.path.realpath(link))
        if target.exists() and root in target.parents:
            continue
        if check:
            problems.append(f"symlink out of the runtime: {link} -> {os.readlink(link)}")
            continue
        link.unlink()
        if target.is_dir():
            link.mkdir()  # Homebrew's site-packages: nothing of ours lives there
        elif target.exists():
            shutil.copy2(target, link)

    queue = [p for p in root.rglob("*") if macho(p)]
    seen: set[Path] = set()
    while queue:
        path = queue.pop()
        if path in seen:
            continue
        seen.add(path)
        changed = False
        for ref in deps(path):
            if not outside(ref):
                continue
            if check:
                problems.append(f"{path.relative_to(root)} loads {ref}")
                continue
            # a framework the bundle already holds (Python.framework): its copy
            inner = None
            if ".framework/" in ref:
                name, rest = ref.split(".framework/", 1)
                candidate = frameworks / (Path(name).name + ".framework") / rest
                if candidate.exists():
                    inner = candidate
            source = Path(os.path.realpath(ref))
            if inner is None and not source.exists():
                problems.append(f"{path.relative_to(root)} loads missing {ref}")
                continue
            dest = inner or frameworks / source.name
            if inner is None and not dest.exists():
                shutil.copy2(source, dest)
                dest.chmod(0o755)
                subprocess.run(["install_name_tool", "-id", f"@rpath/{dest.name}", str(dest)], check=True, capture_output=True)
                sign(dest)
                queue.append(dest)
            rel = os.path.relpath(dest, path.parent)
            subprocess.run(["install_name_tool", "-change", ref, f"@loader_path/{rel}", str(path)], check=True, capture_output=True)
            changed = True
        for rp in rpaths(path):
            if outside(rp):
                if check:
                    problems.append(f"{path.relative_to(root)} has rpath {rp}")
                    continue
                subprocess.run(["install_name_tool", "-delete_rpath", rp, str(path)], check=True, capture_output=True)
                changed = True
        if changed:
            sign(path)

    if not check:
        # the bundles' seals cover the files changed above: frameworks, then
        # the apps inside out
        for version in sorted(frameworks.glob("*.framework/Versions/*")):
            if version.is_dir() and not version.is_symlink():
                subprocess.run(["codesign", "--force", "--sign", "-", str(version)], check=True, capture_output=True)
        for bundle in sorted(app.rglob("*.app"), key=lambda p: -len(p.parts)) + [app]:
            subprocess.run(["codesign", "--force", "--sign", "-", str(bundle)], check=True, capture_output=True)

    for p in problems:
        print(p, file=sys.stderr)
    print(f"{len(seen)} Mach-O files, {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
