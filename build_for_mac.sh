#!/usr/bin/env bash
# Builds Strojanka and produces dist/StrojankaInstaller
# Requirements: Xcode command line tools (xcode-select --install), Git,
#               CMake and pkg-config (brew install cmake ninja pkg-config).
set -euo pipefail
cd "$(dirname "$0")"

BUILD_DIR="build/mac"

for tool in git cmake pkg-config; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "ERROR: '$tool' is required. Install it with: brew install cmake ninja pkg-config"
    exit 1
  fi
done

# vcpkg (dependency manager): use $VCPKG_ROOT if set, otherwise a private copy in .vcpkg
if [ -z "${VCPKG_ROOT:-}" ] || [ ! -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]; then
  if [ ! -x ".vcpkg/vcpkg" ]; then
    echo "vcpkg not found, downloading it into .vcpkg ..."
    git clone https://github.com/microsoft/vcpkg .vcpkg
    ./.vcpkg/bootstrap-vcpkg.sh -disableMetrics
  fi
  export VCPKG_ROOT="$PWD/.vcpkg"
fi
echo "Using vcpkg at $VCPKG_ROOT"

if [ "$(uname -m)" = "arm64" ]; then TRIPLET="arm64-osx"; else TRIPLET="x64-osx"; fi
if command -v ninja >/dev/null 2>&1; then GENERATOR="Ninja"; else GENERATOR="Unix Makefiles"; fi

cmake -S . -B "$BUILD_DIR" -G "$GENERATOR" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET="$TRIPLET"
cmake --build "$BUILD_DIR" --parallel
ctest --test-dir "$BUILD_DIR" --output-on-failure
cmake --build "$BUILD_DIR" --target dist

echo
echo "Done: $PWD/dist/StrojankaInstaller"
