#!/bin/sh
# Configure and build the Backplane KiCad runtime on macOS (arm64) against
# Homebrew, with the same options as the Linux release, and install it into
# stage-macos/ (KiCad.app). scripts/backplane-macos-package.sh turns that into
# the relocatable release archive.
#
#   scripts/backplane-macos-build.sh [configure|build|all]
#
# Homebrew: boost cairo cmake ccache curl eigen glew glm harfbuzz libgit2
# libngspice ninja nng opencascade openssl@3 pkgconf poppler protobuf
# python@3.12 swig unixodbc wxwidgets@3.2 yaml-cpp zstd
set -eu
cd "$(dirname "$0")/.."
step=${1:-all}
build=build/backplane-macos
B=$(brew --prefix)
NG=$(brew --prefix libngspice)
OCC=$(brew --prefix opencascade)
# KiCad 10 is built and tested on wxWidgets 3.2 (Linux: libwxgtk3.2); with
# Homebrew's 3.3 the headless api-server crashes in its console event loop
WX=$(brew --prefix wxwidgets@3.2)/bin/wx-config-3.2
PY=$(brew --prefix python@3.12)/bin/python3.12
PYF=$(cd "$(brew --prefix python@3.12)/Frameworks/Python.framework" && pwd -P)/Versions/3.12
# the framework the install copies into KiCad.app: Homebrew's, staged with a
# site-packages of its own (Homebrew's is a symlink out of the framework, which
# the copy cannot lay over the folder KiCad's own module was installed into)
PYFW=$PWD/build/python-framework/Python.framework
if [ ! -d "$PYFW" ]; then
  mkdir -p "$(dirname "$PYFW")"
  cp -RP "$(dirname "$PYF")/.." "$PYFW"
  rm "$PYFW/Versions/3.12/lib/python3.12/site-packages"
  mkdir "$PYFW/Versions/3.12/lib/python3.12/site-packages"
fi
# Homebrew's ngspice formula is only the program; libngspice has the library
export PKG_CONFIG_PATH="$NG/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
jobs=${BACKPLANE_JOBS:-$(sysctl -n hw.ncpu)}

if [ "$step" = configure ] || [ "$step" = all ]; then
  cmake -S . -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PWD/stage-macos" \
    -DCMAKE_PREFIX_PATH="$NG;$OCC;$B" \
    -DKICAD_BUILD_QA_TESTS=OFF \
    -DKICAD_BUILD_I18N=OFF \
    -DKICAD_USE_SENTRY=OFF \
    -DKICAD_SCRIPTING_WXPYTHON=OFF \
    -DKICAD_USE_PCH=OFF \
    -DKICAD_IPC_API=ON \
    -DPYTHON_EXECUTABLE="$PY" \
    -DPYTHON_LIBRARY="$PYF/lib/libpython3.12.dylib" \
    -DPYTHON_INCLUDE_DIR="$PYF/include/python3.12" \
    -DPYTHON_FRAMEWORK="$PYFW" \
    -DKICAD_APPLE_MAKE_RELOCATEABLE_BUNDLE=ON \
    -DNGSPICE_INCLUDE_DIR="$NG/include" \
    -DNGSPICE_LIBRARY="$NG/lib/libngspice.dylib" \
    -DOCC_INCLUDE_DIR="$OCC/include/opencascade" \
    -DOCC_LIBRARY_DIR="$OCC/lib" \
    -DwxWidgets_CONFIG_EXECUTABLE="$WX" \
    -DCMAKE_CXX_FLAGS="-DKICAD_STOCK_WX_MAC" \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
fi
if [ "$step" = build ] || [ "$step" = all ]; then
  nice -n 10 cmake --build "$build" --parallel "$jobs"
  rm -rf stage-macos
  cmake --install "$build"
fi
