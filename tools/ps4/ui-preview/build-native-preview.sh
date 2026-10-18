#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../../.." && pwd)"
BIN="${TMPDIR:-/tmp}/psbox-ui-native-preview"
OUT="$SCRIPT_DIR/preview-native.png"
CXX_BIN="${CXX:-g++}"
"$CXX_BIN" -std=c++20 -O2 -Wall -Wextra -Wno-unused-function -Wno-missing-field-initializers \
  -I"$REPO_ROOT/src" -I"$REPO_ROOT/ps4-homebrew" -I"$REPO_ROOT/extern/stb" \
  "$SCRIPT_DIR/native_preview.cpp" \
  "$REPO_ROOT/ps4-homebrew/stream_menu_ui.cpp" \
  "$REPO_ROOT/ps4-homebrew/stream_stats_ui.cpp" "$REPO_ROOT/src/util/json.cpp" \
  "$REPO_ROOT/src/ui/canvas.cpp" "$REPO_ROOT/src/ui/font.cpp" \
  -o "$BIN"
"$BIN" "$REPO_ROOT" "$OUT" "${@:-settings}"
printf 'Open: %s\n' "$OUT"
