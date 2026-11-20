#!/bin/bash
# Native CI build only. Never installs into /Applications or the user's runtime.
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)" = Darwin ] || { echo 'macOS is required' >&2; exit 2; }
export HOMEBREW_NO_AUTO_UPDATE=1
prefix=$(brew --prefix)
export PATH="$prefix/opt/protobuf@29/bin:$prefix/bin:$PATH"
# KiCad uses the nng 1.x API; Homebrew's nng is 2.0, so CI builds 1.x separately.
nng=${BACKPLANE_NNG_PREFIX:?set BACKPLANE_NNG_PREFIX to an nng 1.x install}
nng_source=${BACKPLANE_NNG_SOURCE:?set BACKPLANE_NNG_SOURCE to its matching nng checkout}
[ ! -e "$prefix/include/nng/nng.h" ] || { echo "Homebrew nng would shadow nng 1.x; brew uninstall nng" >&2; exit 2; }
export PKG_CONFIG_PATH="$nng/lib/pkgconfig:$prefix/lib/pkgconfig:$prefix/opt/curl/lib/pkgconfig"
version=$(tr -d '\r\n' < BACKPLANE_VERSION)
[[ "$version" =~ ^10\.99\.0-(bp\.[0-9]+(\.[0-9]+)?)$ ]] || { echo 'Invalid Backplane release version' >&2; exit 2; }
version_extra=${BASH_REMATCH[1]}
if [ "${GITHUB_REF_TYPE:-}" = tag ] && [ "$GITHUB_REF_NAME" != "backplane-kicad-v$version" ]; then
  echo 'Release tag does not match BACKPLANE_VERSION' >&2
  exit 2
fi
build="$PWD/build/backplane-macos"
stage="$PWD/stage-macos"
release="$PWD/release/macos"
mkdir -p "$release"
ccache --max-size=2G
cmake -S . -B "$build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$stage" \
  -DCMAKE_PREFIX_PATH="$nng;$prefix;$prefix/opt/protobuf@29;$prefix/opt/curl;$prefix/opt/opencascade" \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DOCC_INCLUDE_DIR="$(brew --prefix opencascade)/include/opencascade" \
  -DOCC_LIBRARY_DIR="$(brew --prefix opencascade)/lib" \
  -DNGSPICE_LIB_NAME=ngspice -DNGSPICE_ROOT_DIR="$(brew --prefix libngspice)" \
  -DSQLite3_ROOT="$(brew --prefix sqlite)" \
  -DKICAD_BUILD_QA_TESTS=OFF -DKICAD_BUILD_I18N=OFF \
  -DKICAD_USE_SENTRY=OFF -DKICAD_USE_PCH=OFF \
  -DKICAD_RUNTIME_PYTHON_EXECUTABLE= \
  -DKICAD_VERSION_EXTRA="$version_extra" -DBACKPLANE_FORK_VERSION="$version" \
  -DBACKPLANE_UPSTREAM_COMMIT="$(cat BACKPLANE_UPSTREAM)"
grep -Fqx "KICAD_VERSION_EXTRA:STRING=$version_extra" "$build/CMakeCache.txt" || {
  echo 'CMake lost the exact Backplane version suffix; refusing to build' >&2
  exit 2
}
# Verify generated/native protocol bindings before the expensive compile.
python scripts/backplane-ipc-smoke.py --check-bindings
# -k 0: one CI round reports every compile error, not the first
cmake --build "$build" --parallel 3 -- -k 0
cmake --install "$build"
bundle="$stage/KiCad.app"
# Upstream installs the GUI bundle; explicitly retain its matched CLI as well.
cli=$(find "$build" -type f -name kicad-cli -perm -111 | head -n 1)
[ -n "$cli" ]
cp "$cli" "$bundle/Contents/MacOS/kicad-cli"
python scripts/backplane-macos-relocate.py "$bundle" | tee "$release/relocation.log"
root="$release/backplane-kicad-darwin-$(uname -m)-$version"
mkdir -p "$root/bin" "$root/share/backplane" "$root/licenses"
cp -R "$bundle" "$root/KiCad.app"
# The pinned KiCad libraries, their 3D models and the Backplane library (scripts/backplane-libraries.sh)
# go where KiCad.app looks for its stock data, Contents/SharedSupport: symbols/, footprints/,
# 3dmodels/ (KICAD10_3DMODEL_DIR's default), template/ (with the libraries' sym-lib-table and
# fp-lib-table), the three default global tables and backplane-library/.  After relocation (which
# would run `file` on every one of them) and the bundle copy, before the whole-bundle re-sign below,
# which seals them.  Moved, not copied: 3.4 GB of models.
libraries="$PWD/build/backplane-libraries"
rm -rf "$libraries"
bash scripts/backplane-libraries.sh "$libraries" --with-3d
shared="$root/KiCad.app/Contents/SharedSupport"
for item in "$libraries/share/kicad"/*; do
  name=$(basename "$item")
  if [ -e "$shared/$name" ]; then
    cp -R "$item/." "$shared/$name/" && rm -rf "$item"
  else
    mv "$item" "$shared/$name"
  fi
done
# Relocation re-signed each Mach-O it changed, which left the bundles' own signatures stale
# ("nested code is modified or invalid"): sign the whole bundle again, ad hoc, inside out, and
# verify it (Gatekeeper is not involved: the hub's download is never quarantined)
codesign --force --deep --sign - "$root/KiCad.app"
codesign --verify --deep --strict "$root/KiCad.app"
# Backplane's packaged IPC helper loads the runtime's nng from root/lib.
# Alias the whole directory so dependent @loader_path names keep working.
ln -s KiCad.app/Contents/Frameworks "$root/lib"
cp "$build/api/api.desc" "$root/share/backplane/api.desc"
cat > "$root/bin/kicad-cli" <<'LAUNCHER'
#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export KICAD_KIFACE_HOME="$root/KiCad.app/Contents/PlugIns"
export KICAD_STOCK_PLUGIN_HOME="$root/KiCad.app/Contents/PlugIns"
export DYLD_FALLBACK_LIBRARY_PATH="$root/KiCad.app/Contents/Frameworks${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}"
exec "$root/KiCad.app/Contents/MacOS/kicad-cli" "$@"
LAUNCHER
chmod +x "$root/bin/kicad-cli"
# The GUI programs, for the hub's Open in KiCad (program beside the CLI) and its embedded KiCad
for tool in kicad pcbnew eeschema; do
  for rel in "KiCad.app/Contents/MacOS/$tool" "KiCad.app/Contents/Applications/$tool.app/Contents/MacOS/$tool"; do
    [ -x "$root/$rel" ] || continue
    cat > "$root/bin/$tool" <<LAUNCHER
#!/bin/sh
set -eu
root=\$(CDPATH= cd -- "\$(dirname -- "\$0")/.." && pwd)
export KICAD_KIFACE_HOME="\$root/KiCad.app/Contents/PlugIns"
export KICAD_STOCK_PLUGIN_HOME="\$root/KiCad.app/Contents/PlugIns"
export DYLD_FALLBACK_LIBRARY_PATH="\$root/KiCad.app/Contents/Frameworks\${DYLD_FALLBACK_LIBRARY_PATH:+:\$DYLD_FALLBACK_LIBRARY_PATH}"
exec "\$root/$rel" "\$@"
LAUNCHER
    chmod +x "$root/bin/$tool"
    break
  done
  [ -x "$root/bin/$tool" ] || { echo "no $tool in the bundle" >&2; exit 2; }
done
cp LICENSE* COPYING* AUTHORS* "$root/licenses/" 2>/dev/null || true
mv "$libraries/licenses/kicad-libraries" "$root/licenses/"
cp "$libraries/KICAD_LIBRARY_SOURCES.txt" "$root/"
mkdir -p "$root/licenses/nng"
cp "$nng_source/LICENSE.txt" "$root/licenses/nng/"
git -C "$nng_source" rev-parse HEAD > "$root/licenses/nng/SOURCE_COMMIT"
git -C "$nng_source" archive --format=tar.gz --output="$root/licenses/nng/matching-source.tar.gz" HEAD
git archive HEAD | gzip > "$root/licenses/matching-source.tar.gz"
printf '%s\n' "$(git rev-parse HEAD)" > "$root/SOURCE_COMMIT"
cp BACKPLANE_UPSTREAM "$root/UPSTREAM_COMMIT"
# Exercise the relocated package from a different working directory with fresh state.
export HOME="$RUNNER_TEMP/backplane-kicad-macos-home"
export XDG_CONFIG_HOME="$HOME/config" XDG_CACHE_HOME="$HOME/cache" KICAD_CONFIG_HOME="$HOME/kicad"
mkdir -p "$HOME"
"$root/bin/kicad-cli" --version | tee "$release/version.log"
"$root/bin/kicad-cli" api-server --describe | tee "$root/share/backplane/runtime.json" "$release/runtime.json"
python - "$root" "$(uname -m)" "$libraries/BACKPLANE_LIBRARY.json" <<'MANIFEST'
import json
from pathlib import Path
import sys
root = Path(sys.argv[1])
manifest = json.loads((root / 'share/backplane/runtime.json').read_text())
manifest['platform'] = 'darwin-' + sys.argv[2]
manifest['backplaneLibrary'] = json.loads(Path(sys.argv[3]).read_text())
# The oldest macOS the package runs on: the highest minos of every Mach-O in it (the bundled Homebrew
# libraries are built for the runner's macOS). The hub reads it and refuses the install on an older Mac.
import subprocess
def minos(path):
    out = subprocess.run(['vtool', '-show-build', str(path)], text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout
    return [tuple(int(p) for p in line.split()[1].split('.')) for line in out.splitlines() if line.strip().startswith('minos')]
seen = [v for f in (root / 'KiCad.app').rglob('*') if f.is_file() and not f.is_symlink() and (f.suffix in ('.dylib', '.so', '.kiface') or f.stat().st_mode & 0o111) for v in minos(f)]
assert seen, 'no Mach-O minos found in the package'
manifest['minOs'] = '.'.join(str(p) for p in max(seen))
(root / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print('manifest.json: minOs', manifest['minOs'], 'from', len(seen), 'Mach-O build versions')
MANIFEST
python scripts/backplane-ipc-smoke.py "$root/bin/kicad-cli" --expected-version "$version" | tee "$release/ipc.log"
python scripts/backplane-document-ipc-regression.py "$root/bin/kicad-cli" | tee "$release/documents.log"
python scripts/backplane-glb-concurrency-test.py "$root/bin/kicad-cli" | tee "$release/glb-concurrency.log"
# The licence of every library the app bundles, from the Homebrew keg it was linked from (or, where the keg has none, that formula's
# source at the bundled version), each under its own folder; LIBRARIES.tsv says where each text came from, BUNDLED.tsv what every
# Mach-O in the app was built from. The check fails the build if a bundled library ends with no licence file.
python scripts/backplane-macos-licences.py "$root" --prefix "$prefix" --nng "$nng" --build "$build" --build "$stage" \
  --source "$PWD" --cache "$RUNNER_TEMP/licence-sources" | tee "$release/licences.log"
python scripts/backplane-licence-package-check.py "$root" | tee -a "$release/licences.log"
archive="$(basename "$root").tar.gz"
# The manifest the release checks: this version, this exact source, this platform
python - "$root/manifest.json" "$version" "$(git rev-parse HEAD)" "darwin-$(uname -m)" <<'CHECK'
import json, sys
m = json.load(open(sys.argv[1])); version, commit, platform = sys.argv[2:]
problems = [f"{k}: {m.get(k)!r}, want {v!r}" for k, v in (("version", version), ("sourceCommit", commit), ("platform", platform)) if m.get(k) != v]
if "BackplaneWorks" not in str(m.get("sourceRepository", "")): problems.append(f"sourceRepository: {m.get('sourceRepository')!r}")
if not m.get("capabilities"): problems.append("no capabilities")
if problems: sys.exit("manifest.json: " + "; ".join(problems))
print("manifest.json: version, sourceCommit, sourceRepository, platform and %d capabilities" % len(m["capabilities"]))
CHECK
tar -C "$release" -czf "$release/$archive" "$(basename "$root")"
# The libraries gate, on the archive as a person unpacks it: the files, tables, licences and
# sources are inside, the bundle's signature still verifies, and the global tables resolve Device:C,
# Capacitor_SMD and Backplane_Passives_Nominal C_0805_2012Metric (and their models) from an empty
# config home.
# Room for the unpacked copy: the compiled objects are no longer needed (ccache keeps its own)
find "$build" -name '*.o' -delete
unpacked="$RUNNER_TEMP/libraries-gate"
rm -rf "$unpacked" && mkdir -p "$unpacked"
tar -C "$unpacked" -xzf "$release/$archive"
codesign --verify --deep --strict "$unpacked/$(basename "$root")/KiCad.app"
# minOs is the highest LC_BUILD_VERSION minos of the package's Mach-Os, as the manifest step takes it
python - "$unpacked/$(basename "$root")" <<'MINOS'
import json, subprocess, sys
from pathlib import Path
root = Path(sys.argv[1])
def minos(path):
    out = subprocess.run(['vtool', '-show-build', str(path)], text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout
    return [tuple(int(p) for p in line.split()[1].split('.')) for line in out.splitlines() if line.strip().startswith('minos')]
seen = [v for f in (root / 'KiCad.app').rglob('*') if f.is_file() and not f.is_symlink() and (f.suffix in ('.dylib', '.so', '.kiface') or f.stat().st_mode & 0o111) for v in minos(f)]
manifest = json.loads((root / 'manifest.json').read_text())
assert seen, 'no Mach-O minos found in the unpacked package'
want = '.'.join(str(p) for p in max(seen))
assert manifest.get('minOs') == want, f"manifest minOs {manifest.get('minOs')!r}, Mach-Os need {want}"
assert manifest.get('backplaneLibrary', {}).get('commit'), 'manifest has no backplaneLibrary'
print(f"PASS manifest: minOs {want} (highest of {len(seen)} Mach-O build versions), backplaneLibrary {manifest['backplaneLibrary']['commit'][:10]}")
MINOS
python scripts/backplane-library-package-check.py "$unpacked/$(basename "$root")" | tee "$release/libraries.log"
python scripts/backplane-licence-package-check.py "$unpacked/$(basename "$root")" | tee -a "$release/libraries.log"
python scripts/backplane-ngspice-package-check.py "$unpacked/$(basename "$root")" | tee -a "$release/libraries.log"
python scripts/backplane-global-tables-regression.py "$unpacked/$(basename "$root")/bin/kicad-cli" | tee -a "$release/libraries.log"
rm -rf "$unpacked" "$libraries"
# The matching source, as the Linux and Windows releases attach it
git archive --format=tar.gz --prefix="$(basename "$root")-source/" HEAD > "$release/$(basename "$root")-source.tar.gz"
# LF only, "<sha256>  <name>": the integrator merges these lines into the release's SHA256SUMS
(cd "$release" && shasum -a 256 "$archive" "$(basename "$root")-source.tar.gz" > SHA256SUMS)
du -h "$release/$archive" "$release/$(basename "$root")-source.tar.gz"
