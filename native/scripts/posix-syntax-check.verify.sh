#!/usr/bin/env bash
# ponytail: smallest check that posix-syntax-check list stays filesystem-driven.
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

included=()
while IFS= read -r f; do
  is_excluded "$f" && continue
  included+=("$f")
done < <(find src -name '*.cpp' -type f | sort)

has() {
  local want="$1"
  for f in "${included[@]}"; do
    [[ "$f" == "$want" ]] && return 0
  done
  return 1
}

has src/io/vhdx_source.cpp
has src/search/hex_search.cpp
! has src/bridge/bridge_scan.cpp
echo "posix-syntax-check.verify: ok (${#included[@]} files)"
