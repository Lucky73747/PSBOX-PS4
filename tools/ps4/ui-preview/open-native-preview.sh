#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../../.." && pwd)"
BIN="${TMPDIR:-/tmp}/psbox-ui-native-preview-live.so"
CXX_BIN="${CXX:-g++}"
PYTHON_BIN="${PSBOX_PREVIEW_PYTHON:-$HOME/.local/share/psbox-pc-manager/venv-py314/bin/python}"

if [[ ! -x "$PYTHON_BIN" ]]; then
  PYTHON_BIN="$(command -v python3)"
fi
"$PYTHON_BIN" -c 'import PyQt6' 2>/dev/null || {
  echo "PyQt6 is required. Set PSBOX_PREVIEW_PYTHON to a Python environment that has PyQt6." >&2
  exit 2
}

"$CXX_BIN" -shared -fPIC -std=c++20 -O2 -Wall -Wextra -Wno-unused-function -Wno-missing-field-initializers \
  -I"$REPO_ROOT/src" -I"$REPO_ROOT/ps4-homebrew" -I"$REPO_ROOT/extern/stb" \
  "$SCRIPT_DIR/native_live.cpp" \
  "$REPO_ROOT/ps4-homebrew/stream_menu_ui.cpp" \
  "$REPO_ROOT/ps4-homebrew/stream_stats_ui.cpp" "$REPO_ROOT/src/util/json.cpp" \
  "$REPO_ROOT/src/ui/canvas.cpp" "$REPO_ROOT/src/ui/font.cpp" \
  -o "$BIN.pending.$$"
mv -f -- "$BIN.pending.$$" "$BIN"

exec "$PYTHON_BIN" "$SCRIPT_DIR/run-native-preview.py" \
  "$REPO_ROOT" "$BIN" "${1:-settings}"
