#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${PSBOX_BUILD_DIR:-$ROOT/build-ps4-homebrew}"

for path in \
  "$SDK_BUNDLE/toolchain/orbis-sdk.cmake" \
  "$OO_TOOLCHAIN/link.x" \
  "$ORBIS_KIT/vkloader/CMakeLists.txt" \
  "$SDL_SOURCE/include/SDL3/SDL.h" \
  if [[ ! -e "$path" ]]; then
    printf 'Missing PS4 build input: %s\n' "$path" >&2
    printf 'Install the compatible external inputs listed in DEPENDENCIES.md first.\n' >&2
    exit 2
  fi
done

# The Orbis SDK bundle contains target headers/libraries, not a host compiler.
# Use the host LLVM 21 package that was used by the saved build.
export OO_PS4_TOOLCHAIN="$OO_TOOLCHAIN"

cmake -S "$ROOT/ps4-homebrew" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$SDK_BUNDLE/toolchain/orbis-sdk.cmake" \
  -DOO_PS4_TOOLCHAIN="$OO_TOOLCHAIN" \
  -DORBIS_COMPAT_DIR="$ORBIS_COMPAT_DIR" \
  -DORBIS_MESA_SRC="$ORBIS_MESA_SRC" \
  -DORBIS_MESA_BUILD="$ORBIS_MESA_BUILD" \
  -DORBIS_KIT_DIR="$ORBIS_KIT" \
  -DXC_PS4_SDL3_SOURCE="$SDL_SOURCE" \

PKG_PATH="$BUILD_DIR/XCHM00001.pkg"
if [[ -e "$PKG_PATH" ]]; then
  ln -sfn "$(basename "$(readlink -f "$PKG_PATH")")" "$BUILD_DIR/PSBOX-PS4.pkg"
fi
printf '\nBuilt: %s/xcloud_homebrew\n' "$BUILD_DIR"
printf 'PKG:   %s/PSBOX-PS4.pkg\n' "$BUILD_DIR"
