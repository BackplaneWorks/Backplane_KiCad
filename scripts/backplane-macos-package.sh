#!/bin/bash
# Turn stage-macos/KiCad.app (scripts/backplane-macos-build.sh) into the
# relocatable macOS runtime, laid out like the Linux one so Backplane and the
# IPC tests use either the same way:
#
#   backplane-kicad-macos-arm64-<version>/
#     KiCad.app            the fork, self-contained (backplane-macos-relocate.py)
#     bin/kicad-cli ...    launchers: the pinned libraries, then the app
#     share/kicad/         symbols, footprints, 3D models, templates, tables
#     manifest.json        what Backplane reads (sourceRepository, version...)
#
#   LIBRARY_STAGE=<dir with share/kicad and licenses> \
#   IPC_PYTHON=<python with protobuf and pynng> \
#     scripts/backplane-macos-package.sh
#
# Without LIBRARY_STAGE the archive has no libraries (a local check); without
# IPC_PYTHON the IPC tests are skipped. Writes release/*.tar.gz and
# release/SHA256SUMS-macos-arm64.
set -euo pipefail
cd "$(dirname "$0")/.."
app_src=stage-macos/KiCad.app
[ -x "$app_src/Contents/MacOS/kicad-cli" ] || { echo "run scripts/backplane-macos-build.sh first" >&2; exit 1; }
version="$("$app_src/Contents/MacOS/kicad-cli" --version | head -n 1 | tr ' /' '__')"
archive_root="backplane-kicad-macos-arm64-${version}"
root="release/${archive_root}"
rm -rf "$root" "release/${archive_root}.tar.gz"
mkdir -p "$root/bin" "$root/share/kicad" "$root/licenses"
cp -a "$app_src" "$root/"

if [ -n "${LIBRARY_STAGE:-}" ]; then
  cp -a "$LIBRARY_STAGE/share/kicad/." "$root/share/kicad/"
  cp -a "$LIBRARY_STAGE/licenses/kicad-libraries" "$root/licenses/"
  cat "$root/licenses/kicad-libraries/"*/BACKPLANE_SOURCE.txt > "$root/KICAD_LIBRARY_SOURCES.txt"
  for f in sym-lib-table fp-lib-table design-block-lib-table; do test -s "$root/share/kicad/$f"; done
  test "$(find "$root/share/kicad/symbols" -type f -name '*.kicad_sym' | wc -l)" -gt 0
  test "$(find "$root/share/kicad/footprints" -type f | wc -l)" -gt 0
  test "$(find "$root/share/kicad/3dmodels" -type f | wc -l)" -gt 0
  if grep -RIlq '^version https://git-lfs.github.com/spec/v1' "$root/share/kicad/3dmodels"; then
    echo 'unhydrated Git LFS pointer found in bundled 3D models' >&2
    exit 1
  fi
fi

# everything it loads comes from inside it
python3 scripts/backplane-macos-relocate.py "$root"
python3 scripts/backplane-macos-relocate.py "$root" --check

# launchers: the pinned libraries (when bundled), then the program in the app
launch() {
  local name="$1" target="$2"
  cat > "$root/bin/$name" <<LAUNCHER
#!/bin/sh
set -eu
root=\$(CDPATH= cd -- "\$(dirname -- "\$0")/.." && pwd)
if [ -d "\$root/share/kicad/symbols" ]; then
  export KICAD10_SYMBOL_DIR="\${KICAD10_SYMBOL_DIR-\$root/share/kicad/symbols}"
  export KICAD10_FOOTPRINT_DIR="\${KICAD10_FOOTPRINT_DIR-\$root/share/kicad/footprints}"
  export KICAD10_3DMODEL_DIR="\${KICAD10_3DMODEL_DIR-\$root/share/kicad/3dmodels}"
  export KICAD10_TEMPLATE_DIR="\${KICAD10_TEMPLATE_DIR-\$root/share/kicad/template}"
  config_root="\${KICAD_CONFIG_HOME-\$HOME/Library/Preferences/kicad}/10.0"
  mkdir -p "\$config_root"
  for table in sym-lib-table fp-lib-table design-block-lib-table; do
    if [ ! -e "\$config_root/\$table" ]; then
      temporary_table=\$(mktemp "\$config_root/.\$table.XXXXXX")
      cp "\$root/share/kicad/\$table" "\$temporary_table"
      ln "\$temporary_table" "\$config_root/\$table" 2>/dev/null || true
      rm -f "\$temporary_table"
    fi
  done
fi
exec "\$root/KiCad.app/$target" "\$@"
LAUNCHER
  chmod +x "$root/bin/$name"
}
launch kicad-cli Contents/MacOS/kicad-cli
launch kicad Contents/MacOS/kicad
for editor in pcbnew eeschema gerbview pl_editor pcb_calculator bitmap2component; do
  if [ -x "$root/KiCad.app/Contents/Applications/$editor.app/Contents/MacOS/$editor" ]; then
    launch "$editor" "Contents/Applications/$editor.app/Contents/MacOS/$editor"
  fi
done

cp LICENSE* AUTHORS.txt README.md "$root/licenses/"
mkdir -p "$root/licenses/vendor"
while IFS= read -r -d '' notice; do
  mkdir -p "$root/licenses/vendor/$(dirname "$notice")"
  cp "$notice" "$root/licenses/vendor/$notice"
done < <(find thirdparty -type f \( -iname 'LICENSE*' -o -iname 'COPYING*' -o -iname 'NOTICE*' -o -iname 'COPYRIGHT*' \) -print0)
# the Homebrew libraries the app carries: each formula's version, license and notices
mkdir -p "$root/licenses/homebrew"
for lib in "$root/KiCad.app/Contents/Frameworks/"*; do
  basename "$lib"
done > "$root/licenses/homebrew/BUNDLED.txt"
for formula in $(brew deps --installed --formula boost cairo curl glew harfbuzz libgit2 libngspice nng opencascade openssl@3 poppler protobuf python@3.12 unixodbc wxwidgets yaml-cpp zstd 2>/dev/null | sort -u) \
    boost cairo curl glew harfbuzz libgit2 libngspice nng opencascade openssl@3 poppler protobuf python@3.12 unixodbc wxwidgets yaml-cpp zstd; do
  dir="$root/licenses/homebrew/$formula"
  [ -d "$dir" ] && continue
  prefix="$(brew --prefix "$formula" 2>/dev/null)" || continue
  mkdir -p "$dir"
  brew info --json=v2 "$formula" | python3 -c 'import json,sys; f=json.load(sys.stdin)["formulae"][0]; print("Formula:", f["name"]); print("Version:", f["versions"]["stable"]); print("License:", f.get("license")); print("Homepage:", f.get("homepage"))' > "$dir/metadata.txt"
  find -L "$prefix/" -maxdepth 1 -type f \( -iname 'LICENSE*' -o -iname 'COPYING*' -o -iname 'NOTICE*' -o -iname 'COPYRIGHT*' \) -exec cp {} "$dir/" \; 2>/dev/null || true
done

git rev-parse HEAD > "$root/SOURCE_COMMIT"
git describe --tags --always > "$root/SOURCE_VERSION"
cat > "$root/manifest.json" <<MANIFEST
{
  "sourceRepository": "https://github.com/i2cjak/Backplane_KiCad",
  "sourceCommit": "$(git rev-parse HEAD)",
  "version": "${version}",
  "platform": "macos-arm64",
  "license": "GPL-3.0-or-later"
}
MANIFEST
{
  echo "Backplane KiCad macOS runtime (arm64)"
  echo "Source commit: $(git rev-parse HEAD)"
  echo "Source version: $(git describe --tags --always)"
  echo "IPC API: enabled"
  echo "Entry point: bin/kicad-cli api-server"
} > "$root/RUNTIME_MANIFEST.txt"

# the runtime must work after relocation, with its build prefix absent
moved="$(mktemp -d)/stage-macos"
mv stage-macos "$moved"
trap 'mv "$moved" stage-macos' EXIT
"$root/bin/kicad-cli" --version
"$root/bin/kicad-cli" api-server --help >/dev/null
if [ -n "${IPC_PYTHON:-}" ]; then
  "$IPC_PYTHON" scripts/backplane-ipc-smoke.py "$root/bin/kicad-cli"
  for regression in document-ipc footprint-ipc schematic-ipc schematic-units pcb-ipc crossprobe-ipc ipc-mutation ipc-variant ipc-embedded ipc-export ipc-rulecheck sketch-router-ipc; do
    "$IPC_PYTHON" "scripts/backplane-${regression}-regression.py" "$root/bin/kicad-cli"
  done
fi
if [ -n "${LIBRARY_STAGE:-}" ]; then
  python3 scripts/backplane-library-smoke.py "$root"
fi

(cd release && tar -czf "${archive_root}.tar.gz" "${archive_root}")
size_bytes="$(stat -f '%z' "release/${archive_root}.tar.gz")"
test "$size_bytes" -lt 2147483648 || { echo "GitHub release asset exceeds the 2 GiB limit ($size_bytes bytes)" >&2; exit 1; }
(cd release && shasum -a 256 "${archive_root}.tar.gz" | sed 's|  |  ./|' > SHA256SUMS-macos-arm64)
cat release/SHA256SUMS-macos-arm64
