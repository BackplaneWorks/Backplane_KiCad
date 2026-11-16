#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Stage the pinned KiCad libraries and the Backplane KiCad library for a runtime package.
#
#   backplane-libraries.sh <stage> [--with-3d] [--sources <dir>]
#
# Writes, under <stage>:
#   share/kicad/symbols/*.kicad_symdir, share/kicad/footprints/*.pretty,
#   share/kicad/3dmodels/*.3dshapes (only --with-3d: needs git-lfs),
#   share/kicad/template/ (kicad-templates' Projects and worksheets, and the libraries'
#     own sym-lib-table and fp-lib-table),
#   share/kicad/{sym-lib-table,fp-lib-table,design-block-lib-table}: the default global tables,
#     nesting ${KICAD10_TEMPLATE_DIR}/<table>; fp-lib-table also names the Backplane library,
#   share/kicad/backplane-library/ (footprints and STEP models, found by ${BACKPLANE_KICAD_LIBRARY}),
#   licenses/kicad-libraries/<name>/ (BACKPLANE_SOURCE.txt and the library's notices),
#   KICAD_LIBRARY_SOURCES.txt (every BACKPLANE_SOURCE.txt, in name order),
#   BACKPLANE_LIBRARY.json (the runtime manifest's backplaneLibrary key; not packaged itself).
# With --sources, the source snapshots (<name>-<tag>-source.tar.gz and the Backplane library's
# archive) go to <dir>.
#
# One script for Linux, Windows (Git Bash) and macOS (bash 3.2), so every package carries the
# same pinned revisions.  The Linux release calls it with --with-3d and --sources.
set -euo pipefail

# Stable 10.0.6 libraries: master reads them, and so does stock KiCad 10.x.
KICAD_LIBRARY_TAG=10.0.6
KICAD_SYMBOLS_COMMIT=7800d91437ce44e2ed0928f2ad31a287457b8a68
KICAD_FOOTPRINTS_COMMIT=819223b66f96508feaeaa305301b5e6bb5c1038b
KICAD_3DMODELS_COMMIT=e62ed1fc7862da83f789bd562671b5e4b82afcdf
KICAD_TEMPLATES_COMMIT=3ed4538b0f965d821df63a5fffc4441e723cfe7f
# BackplaneWorks/Backplane_KiCad_Library: Backplane's footprints (logos, mounting holes,
# passives, artwork) and the STEP models they use.  Footprint tables name it through
# ${BACKPLANE_KICAD_LIBRARY}: the Linux launchers, and KiCad itself when the variable is
# unset (global-tables-seeded), point it at share/kicad/backplane-library.
BACKPLANE_LIBRARY_REPOSITORY=https://github.com/BackplaneWorks/Backplane_KiCad_Library.git
BACKPLANE_LIBRARY_COMMIT=314532f925fef0becc5a6eaf6be3b000f41cb0d0
# The bundle's label (not a tag in that repository): the commit is the pin
BACKPLANE_LIBRARY_VERSION=2026.11.13

usage() { echo "usage: $0 <stage> [--with-3d] [--sources <dir>]" >&2; exit 2; }
[ $# -ge 1 ] || usage
stage_arg=$1; shift
with_3d=0
sources_arg=
while [ $# -gt 0 ]; do
  case "$1" in
    --with-3d) with_3d=1 ;;
    --sources) [ $# -ge 2 ] || usage; sources_arg=$2; shift ;;
    *) usage ;;
  esac
  shift
done

# Git Bash: GNU tar reads "C:\..." as a remote host, so work in POSIX paths throughout.
posix() { if command -v cygpath >/dev/null 2>&1; then cygpath -u "$1"; else printf '%s\n' "$1"; fi; }
mkdir -p "$stage_arg"
stage=$(cd "$(posix "$stage_arg")" && pwd)
if [ -n "$sources_arg" ]; then mkdir -p "$sources_arg"; sources=$(cd "$(posix "$sources_arg")" && pwd); fi
work=$(posix "${RUNNER_TEMP:-${TMPDIR:-/tmp}}")/backplane-libraries-$$
rm -rf "$work"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

# A real Python: on Windows "python3" may be the Store's stub
if [ -n "${PYTHON:-}" ]; then :
elif [ -x /usr/bin/python3 ] && [ "$(uname -s)" != Darwin ]; then PYTHON=/usr/bin/python3
elif command -v python >/dev/null 2>&1 && python -c 'import sys; sys.exit(sys.version_info < (3, 8))' 2>/dev/null; then PYTHON=python
else PYTHON=python3
fi
sha256_check() { if command -v sha256sum >/dev/null 2>&1; then sha256sum --quiet -c "$1"; else shasum -a 256 -c "$1" >/dev/null; fi; }

library_root="$work/kicad-libraries"
stage_root="$stage/share/kicad"
mkdir -p "$library_root" "$stage_root/symbols" "$stage_root/footprints" "$stage_root/template"
[ "$with_3d" = 0 ] || mkdir -p "$stage_root/3dmodels"

libraries="kicad-symbols kicad-footprints kicad-templates"
[ "$with_3d" = 0 ] || libraries="kicad-symbols kicad-footprints kicad-packages3D kicad-templates"

clone_library() {
  local name="$1" commit="$2"
  local checkout="$library_root/$name"
  # No line-ending conversion and no LFS smudge: KiCad's files go in byte for byte.  core.eol=lf as
  # well as autocrlf=false: the libraries' .gitattributes say text=auto, which Git for Windows would
  # otherwise check out as CRLF (core.eol=native)
  GIT_LFS_SKIP_SMUDGE=1 git -c core.autocrlf=false -c core.eol=lf -c core.longpaths=true \
    clone --depth 1 --no-tags --branch "$KICAD_LIBRARY_TAG" \
    "https://gitlab.com/kicad/libraries/$name.git" "$checkout"
  test "$(git -C "$checkout" rev-parse HEAD)" = "$commit"
  if [ "$name" = kicad-packages3D ]; then
    command -v git-lfs >/dev/null
    git -C "$checkout" lfs pull
  fi
  printf 'repository=https://gitlab.com/kicad/libraries/%s.git\nref=%s\ncommit=%s\n' \
    "$name" "$KICAD_LIBRARY_TAG" "$commit" > "$checkout/BACKPLANE_SOURCE.txt"
}

clone_library kicad-symbols "$KICAD_SYMBOLS_COMMIT"
clone_library kicad-footprints "$KICAD_FOOTPRINTS_COMMIT"
[ "$with_3d" = 0 ] || clone_library kicad-packages3D "$KICAD_3DMODELS_COMMIT"
clone_library kicad-templates "$KICAD_TEMPLATES_COMMIT"

find "$library_root/kicad-symbols" -maxdepth 1 -type d -name '*.kicad_symdir' \
  -exec cp -a {} "$stage_root/symbols/" \;
find "$library_root/kicad-footprints" -maxdepth 1 -type d -name '*.pretty' \
  -exec cp -a {} "$stage_root/footprints/" \;
test "$(find "$stage_root/symbols" -type f -name '*.kicad_sym' | wc -l)" -gt 0
test "$(find "$stage_root/footprints" -type f | wc -l)" -gt 0
if [ "$with_3d" = 1 ]; then
  find "$library_root/kicad-packages3D" -maxdepth 1 -type d -name '*.3dshapes' \
    -exec cp -a {} "$stage_root/3dmodels/" \;
  test "$(find "$stage_root/3dmodels" -type f | wc -l)" -gt 0
  test "$(find "$stage_root/3dmodels" -type f -size +1k | wc -l)" -gt 0
fi

cp "$library_root/kicad-symbols/sym-lib-table" "$stage_root/template/sym-lib-table"
cp "$library_root/kicad-footprints/fp-lib-table" "$stage_root/template/fp-lib-table"
# Match kicad-templates' install layout so KiCad discovers each template.
cp -a "$library_root/kicad-templates/Projects/." "$stage_root/template/"
find "$library_root/kicad-templates/Worksheets" -type f -name '*.kicad_wks' \
  -exec cp {} "$stage_root/template/" \;
printf '%s\n' '(sym_lib_table' '  (version 7)' \
  '  (lib (name "KiCad")(type "Table")(uri "${KICAD10_TEMPLATE_DIR}/sym-lib-table")(options "")(descr "Official KiCad symbol libraries"))' \
  ')' > "$stage_root/sym-lib-table"
printf '%s\n' '(design_block_lib_table' '  (version 7)' ')' > "$stage_root/design-block-lib-table"
printf '%s\n' '(fp_lib_table' '  (version 7)' \
  '  (lib (name "KiCad")(type "Table")(uri "${KICAD10_TEMPLATE_DIR}/fp-lib-table")(options "")(descr "Official KiCad footprint libraries"))' \
  ')' > "$stage_root/fp-lib-table"

mkdir -p "$stage/licenses/kicad-libraries"
for name in $libraries; do
  destination="$stage/licenses/kicad-libraries/$name"
  mkdir -p "$destination"
  cp "$library_root/$name/BACKPLANE_SOURCE.txt" "$destination/"
  find "$library_root/$name" -maxdepth 1 -type f \
    \( -iname 'LICENSE*' -o -iname 'COPYING*' -o -iname 'NOTICE*' -o -iname 'README*' \) \
    -exec cp {} "$destination/" \;
done

# Source snapshots of the exact library revisions, attached to the release beside the runtime.
if [ -n "$sources_arg" ]; then
  for name in $libraries; do
    tar -C "$library_root/$name" --exclude=.git -czf \
      "$sources/$name-$KICAD_LIBRARY_TAG-source.tar.gz" .
  done
fi
rm -rf "$library_root"

# The Backplane KiCad library
checkout="$work/backplane-kicad-library"
bundle="$stage_root/backplane-library"
rm -rf "$checkout" "$bundle"
git init -q "$checkout"
git -C "$checkout" config core.autocrlf false
git -C "$checkout" config core.eol lf
git -C "$checkout" fetch -q --depth 1 "$BACKPLANE_LIBRARY_REPOSITORY" "$BACKPLANE_LIBRARY_COMMIT"
git -C "$checkout" checkout -q --detach FETCH_HEAD
test "$(git -C "$checkout" rev-parse HEAD)" = "$BACKPLANE_LIBRARY_COMMIT"

# The library's own packager checks every footprint against its manifest and writes a
# reproducible archive with the notices the payload needs.  It names files with str(Path), which
# is "\"-separated on Windows: there, run a copy that names them with "/" (the manifest's form).
packager="$checkout/scripts/package_runtime.py"
if "$PYTHON" -I -c 'import os, sys; sys.exit(os.sep != "\\")'; then
  "$PYTHON" -I - "$packager" "$checkout/scripts/package_runtime_posix.py" <<'PATCH'
import sys
source, target = sys.argv[1:]
text = open(source, encoding="utf-8").read()
old = "str(p.relative_to(ROOT))"
assert text.count(old) == 4, "package_runtime.py changed: review the Windows path patch"
open(target, "w", encoding="utf-8", newline="\n").write(text.replace(old, "p.relative_to(ROOT).as_posix()"))
PATCH
  packager="$checkout/scripts/package_runtime_posix.py"
fi
archive_dir="$work/backplane-library-release"
"$PYTHON" -I "$packager" --output "$archive_dir" \
  --version "$BACKPLANE_LIBRARY_VERSION" --source-commit "$BACKPLANE_LIBRARY_COMMIT"
archive="$archive_dir/backplane-kicad-library-$BACKPLANE_LIBRARY_VERSION.tar.gz"
mkdir -p "$bundle"
tar -C "$bundle" --strip-components=1 -xzf "$archive"
(cd "$bundle" && sha256_check SHA256SUMS)
test -d "$bundle/libraries/Backplane_Logo.pretty"
test "$(find "$bundle/libraries" -type f -name '*.kicad_mod' | wc -l)" -gt 0
test "$(find "$bundle/3dmodels" -type f -name '*.step' | wc -l)" -gt 0

# Register every library the release names in the default global table
# a fresh install gets, after KiCad's own nested table.
"$PYTHON" -I - "$bundle/fp-lib-table.example" "$stage_root/fp-lib-table" <<'MERGE'
import re, sys
example, table = sys.argv[1:]
entries = re.findall(r'^\s*(\(lib \(name "([^"]+)"\).*\))\s*$', open(example).read(), re.M)
assert entries, "the library names no footprint libraries"
text = open(table).read()
have = set(re.findall(r'\(lib \(name "([^"]+)"\)', text))
lines = "".join(f"  {line}\n" for line, name in entries if name not in have)
end = text.rstrip().rfind(")")
open(table, "w", newline="\n").write(text[:end].rstrip("\n") + "\n" + lines + ")\n")
MERGE
grep -q '(name "Backplane_Logo")' "$stage_root/fp-lib-table"

destination="$stage/licenses/kicad-libraries/backplane-kicad-library"
mkdir -p "$destination"
# version= is the bundle's label, not a git ref: the pin is the commit
printf 'repository=%s\nversion=%s\ncommit=%s\n' "$BACKPLANE_LIBRARY_REPOSITORY" \
  "$BACKPLANE_LIBRARY_VERSION" "$BACKPLANE_LIBRARY_COMMIT" > "$destination/BACKPLANE_SOURCE.txt"
cp "$bundle/LICENSES.md" "$bundle/README.md" "$destination/"
cp "$bundle/3dmodels/LICENSE.md" "$destination/3dmodels-LICENSE.md"
cp "$bundle/3dmodels/CREDITS.md" "$destination/3dmodels-CREDITS.md"
[ -z "$sources_arg" ] || cp "$archive" "$sources/"

# The runtime manifest's backplaneLibrary key (each platform's packaging merges this file in;
# BACKPLANE_IPC.md, "Runtime manifest"): version is the bundle's label (not a git ref), commit
# the pin, sha256 is over share/kicad/backplane-library/SHA256SUMS, which lists every file's
# sha256, so hashing that file and then `sha256sum -c SHA256SUMS` in the folder checks it all.
"$PYTHON" -I - "$bundle/SHA256SUMS" "$stage/BACKPLANE_LIBRARY.json" "$BACKPLANE_LIBRARY_REPOSITORY" \
  "$BACKPLANE_LIBRARY_VERSION" "$BACKPLANE_LIBRARY_COMMIT" <<'KEY'
import hashlib, json, sys
sums, out, repository, version, commit = sys.argv[1:]
key = {"repository": repository, "version": version, "commit": commit,
       "sha256": hashlib.sha256(open(sums, "rb").read()).hexdigest()}
open(out, "w", newline="\n").write(json.dumps(key, indent=2) + "\n")
KEY

# What the package carries, one BACKPLANE_SOURCE.txt after another
(cd "$stage/licenses/kicad-libraries" && LC_ALL=C ls | while IFS= read -r name; do
   cat "$name/BACKPLANE_SOURCE.txt"; done) > "$stage/KICAD_LIBRARY_SOURCES.txt"
grep -q "commit=$KICAD_SYMBOLS_COMMIT" "$stage/KICAD_LIBRARY_SOURCES.txt"
echo "Backplane libraries staged in $stage (3D models: $([ "$with_3d" = 1 ] && echo yes || echo no))"
