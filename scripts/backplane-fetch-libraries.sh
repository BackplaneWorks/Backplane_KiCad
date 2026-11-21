#!/bin/bash
# Fetch the official KiCad libraries the release pins and stage them as
# <work>/kicad-library-stage/{share/kicad,licenses} (and source snapshots in
# <work>/kicad-library-source). The same steps as the Linux release job's
# "Fetch pinned official KiCad libraries"; the macOS job runs this script.
#   scripts/backplane-fetch-libraries.sh WORK_DIR
# Needs git and git-lfs.
KICAD_LIBRARY_TAG="${KICAD_LIBRARY_TAG:-10.0.6}"
KICAD_SYMBOLS_COMMIT="${KICAD_SYMBOLS_COMMIT:-7800d91437ce44e2ed0928f2ad31a287457b8a68}"
KICAD_FOOTPRINTS_COMMIT="${KICAD_FOOTPRINTS_COMMIT:-819223b66f96508feaeaa305301b5e6bb5c1038b}"
KICAD_3DMODELS_COMMIT="${KICAD_3DMODELS_COMMIT:-e62ed1fc7862da83f789bd562671b5e4b82afcdf}"
KICAD_TEMPLATES_COMMIT="${KICAD_TEMPLATES_COMMIT:-3ed4538b0f965d821df63a5fffc4441e723cfe7f}"
RUNNER_TEMP="$(cd "$1" && pwd)"
set -euo pipefail
library_root="$RUNNER_TEMP/kicad-libraries"
stage_root="$RUNNER_TEMP/kicad-library-stage/share/kicad"
mkdir -p "$library_root" "$stage_root/symbols" "$stage_root/footprints" "$stage_root/3dmodels" "$stage_root/template"

clone_library() {
  local name="$1" commit="$2"
  local checkout="$library_root/$name"
  git clone --depth 1 --no-tags --branch "$KICAD_LIBRARY_TAG" \
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
clone_library kicad-packages3D "$KICAD_3DMODELS_COMMIT"
clone_library kicad-templates "$KICAD_TEMPLATES_COMMIT"

find "$library_root/kicad-symbols" -maxdepth 1 -type d -name '*.kicad_symdir' \
  -exec cp -a {} "$stage_root/symbols/" \;
find "$library_root/kicad-footprints" -maxdepth 1 -type d -name '*.pretty' \
  -exec cp -a {} "$stage_root/footprints/" \;
find "$library_root/kicad-packages3D" -maxdepth 1 -type d -name '*.3dshapes' \
  -exec cp -a {} "$stage_root/3dmodels/" \;
test "$(find "$stage_root/symbols" -type f -name '*.kicad_sym' | wc -l)" -gt 0
test "$(find "$stage_root/footprints" -type f | wc -l)" -gt 0
test "$(find "$stage_root/3dmodels" -type f | wc -l)" -gt 0
test "$(find "$stage_root/3dmodels" -type f -size +1k | wc -l)" -gt 0

cp "$library_root/kicad-symbols/sym-lib-table" "$stage_root/template/sym-lib-table"
cp "$library_root/kicad-footprints/fp-lib-table" "$stage_root/template/fp-lib-table"
# Match kicad-templates' install layout so KiCad discovers each template.
cp -a "$library_root/kicad-templates/Projects/." "$stage_root/template/"
find "$library_root/kicad-templates/Worksheets" -type f -name '*.kicad_wks' \
  -exec cp {} "$stage_root/template/" \;
cat > "$stage_root/sym-lib-table" <<'TABLE'
(sym_lib_table
  (version 7)
  (lib (name "KiCad")(type "Table")(uri "${KICAD10_TEMPLATE_DIR}/sym-lib-table")(options "")(descr "Official KiCad symbol libraries"))
)
TABLE
cat > "$stage_root/design-block-lib-table" <<'TABLE'
(design_block_lib_table
  (version 7)
)
TABLE
cat > "$stage_root/fp-lib-table" <<'TABLE'
(fp_lib_table
  (version 7)
  (lib (name "KiCad")(type "Table")(uri "${KICAD10_TEMPLATE_DIR}/fp-lib-table")(options "")(descr "Official KiCad footprint libraries"))
)
TABLE

mkdir -p "$RUNNER_TEMP/kicad-library-stage/licenses/kicad-libraries"
for name in kicad-symbols kicad-footprints kicad-packages3D kicad-templates; do
  destination="$RUNNER_TEMP/kicad-library-stage/licenses/kicad-libraries/$name"
  mkdir -p "$destination"
  cp "$library_root/$name/BACKPLANE_SOURCE.txt" "$destination/"
  find "$library_root/$name" -maxdepth 1 -type f \
    \( -iname 'LICENSE*' -o -iname 'COPYING*' -o -iname 'NOTICE*' -o -iname 'README*' \) \
    -exec cp {} "$destination/" \;
done

# Keep source snapshots alongside the binary runtime so users can
# inspect the exact library revisions used in this release.
mkdir -p "$RUNNER_TEMP/kicad-library-source"
for name in kicad-symbols kicad-footprints kicad-packages3D kicad-templates; do
  tar -C "$library_root/$name" --exclude=.git -czf \
    "$RUNNER_TEMP/kicad-library-source/$name-$KICAD_LIBRARY_TAG-source.tar.gz" .
done
# Keep one staged copy while the native build runs.
rm -rf "$library_root"
