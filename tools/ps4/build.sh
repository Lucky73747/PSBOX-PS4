#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${PSBOX_BUILD_DIR:-$ROOT/build-ps4-homebrew}"
BUILD_JOBS="${PSBOX_BUILD_JOBS:-2}"
SDK_BUNDLE="${PS4_ORBIS_SDK_BUNDLE:-$ROOT/dependencies/orbis-sdk-v1/orbis-sdk-v1}"
OO_TOOLCHAIN="${OO_PS4_TOOLCHAIN:-$ROOT/dependencies/OpenOrbis-PS4-Toolchain}"
ORBIS_KIT="${ORBIS_KIT_DIR:-$ROOT/dependencies/orbis-porting-kit}"
SDL_SOURCE="${XC_PS4_SDL3_SOURCE:-$ROOT/dependencies/SDL}"
SDL_BUILD="${XC_PS4_SDL3_BUILD:-$ROOT/dependencies/SDL-build-sdl3-freebsd}"
export ORBIS_COMPAT_DIR="${ORBIS_COMPAT_DIR:-$SDK_BUNDLE/orbis-compat}"
export ORBIS_MESA_SRC="${ORBIS_MESA_SRC:-$SDK_BUNDLE/mesa}"
export ORBIS_MESA_BUILD="${ORBIS_MESA_BUILD:-$ORBIS_MESA_SRC/build-orbis}"
LLVM_BIN="${PSBOX_LLVM_BIN:-/usr/lib/llvm-21/bin}"

case "${1:-}" in
  --help|-h)
    exit 0 ;;
  ''|--check-inputs) ;;
  *) printf 'Unknown argument: %s\n' "$1" >&2; exit 2 ;;
esac
if [[ $# -gt 1 ]]; then
  printf 'Expected at most one argument.\n' >&2
  exit 2
fi

for path in \
  "$SDK_BUNDLE/toolchain/orbis-sdk.cmake" \
  "$OO_TOOLCHAIN/link.x" \
  "$ORBIS_KIT/vkloader/CMakeLists.txt" \
  "$SDL_SOURCE/include/SDL3/SDL.h" \
  "$SDL_BUILD/libSDL3.a" \
  "$SDL_BUILD/include-config-release/build_config/SDL_build_config.h" \
  "$ORBIS_COMPAT_DIR/optional/orbis_thread_atexit_stub.c" \
  "$ORBIS_MESA_BUILD/src/amd/vulkan/libvulkan_radeon.a" \
  "$ROOT/deps/opus/CMakeLists.txt" \
  "$ROOT/THIRD_PARTY_NOTICES.md" \
  if [[ ! -e "$path" ]]; then
    printf 'Missing PS4 build input: %s\n' "$path" >&2
    exit 2
  fi
done

if [[ ! "$BUILD_JOBS" =~ ^[1-9][0-9]*$ ]]; then
  printf 'PSBOX_BUILD_JOBS must be a positive integer (got: %s)\n' "$BUILD_JOBS" >&2
  exit 2
fi

# The Orbis SDK bundle contains target headers/libraries, not a host compiler.
# Use the host LLVM 21 package that was used by the saved build.
export PATH="$LLVM_BIN:$OO_TOOLCHAIN/bin/linux:$PATH"
export OO_PS4_TOOLCHAIN="$OO_TOOLCHAIN"
if ! compgen -G "$ORBIS_MESA_BUILD/subprojects/zlib-*/libz.a" >/dev/null; then
  printf 'Missing zlib archive under ORBIS_MESA_BUILD.\n' >&2
  exit 2
fi
for tool in clang clang++ ld.lld cmake ninja; do
  command -v "$tool" >/dev/null || { printf 'Missing host tool: %s\n' "$tool" >&2; exit 2; }
done
if [[ "${1:-}" == --check-inputs ]]; then
  printf 'PS4 input paths and host tools are present. No build was run.\n'
  exit 0
fi

# Keep this build directory between builds. Do not clean it for routine PKGs:
# Ninja tracks source/header dependencies and recompiles only what changed.
cmake -S "$ROOT/ps4-homebrew" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$SDK_BUNDLE/toolchain/orbis-sdk.cmake" \
  -DOO_PS4_TOOLCHAIN="$OO_TOOLCHAIN" \
  -DORBIS_COMPAT_DIR="$ORBIS_COMPAT_DIR" \
  -DORBIS_MESA_SRC="$ORBIS_MESA_SRC" \
  -DORBIS_MESA_BUILD="$ORBIS_MESA_BUILD" \
  -DORBIS_KIT_DIR="$ORBIS_KIT" \
  -DXC_PS4_SDL3_SOURCE="$SDL_SOURCE" \

ninja -C "$BUILD_DIR" -j"$BUILD_JOBS" xcloud_homebrew
PKG_PATH="$BUILD_DIR/XCHM00001.pkg"
if [[ -e "$PKG_PATH" ]]; then
  ln -sfn "$(basename "$(readlink -f "$PKG_PATH")")" "$BUILD_DIR/PSBOX-PS4.pkg"
fi
printf '\nBuilt: %s/xcloud_homebrew\n' "$BUILD_DIR"
printf 'PKG:   %s/PSBOX-PS4.pkg\n' "$BUILD_DIR"
