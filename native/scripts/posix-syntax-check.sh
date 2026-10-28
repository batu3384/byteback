#!/usr/bin/env bash
# CA-056: syntax-check platform-independent native TUs on Linux (g++ -fsyntax-only).
# Source list is derived from the tree — not a hand-maintained manifest — so new
# parsers/io modules cannot bypass the gate by omission.
set -euo pipefail

cd "$(dirname "$0")/.."

is_excluded() {
  local f="$1"
  case "$f" in
    src/bridge/*|src/byteback_engine.cpp) return 0 ;;
    src/io/disk_reader_win.cpp|src/io/volume_mapper_win.cpp) return 0 ;;
    src/smart/smart_monitor.cpp|src/security/data_shredder.cpp) return 0 ;;
    src/recovery/process_util.cpp) return 0 ;;
    tests/*) return 0 ;;
  esac
  return 1
}

mapfile -t FILES < <(
  find src -name '*.cpp' -type f | sort | while read -r f; do
    is_excluded "$f" && continue
    echo "$f"
  done
)

if ((${#FILES[@]} == 0)); then
  echo "posix-syntax-check: no files matched" >&2
  exit 1
fi

echo "posix-syntax-check: ${#FILES[@]} translation units"
for f in "${FILES[@]}"; do
  echo "syntax-check: $f"
  g++ -fsyntax-only -std=c++17 \
    -DSQLITE_ENABLE_FTS5 -DSQLITE_THREADSAFE=1 \
    -I include -I third_party -I third_party/zlib \
    "$f"
done

echo "POSIX syntax proof passed: all platform-independent TUs parsed."
