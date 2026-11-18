#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 3 ]]; then
    echo "usage: $0 PINNED_SHENG_UFS HOUJI_LEGACY_UFS PATCHED_UNIXNU" >&2
    exit 2
fi
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp/opencode}/houji-data-test.XXXXXX")
trap 'rm -rf -- "$scratch"' EXIT
timeout 20 python3 "$root/scripts/prepare-houji-ufs.py" --source "$1" --legacy "$2" --output "$scratch/ufs"
# Split optional flags to let callers add sanitizers without shell expansion.
read -ra flags <<< "${CXXFLAGS:-}"
timeout 60 "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Wno-unused-function -fno-exceptions -fno-rtti "${flags[@]}" \
    -DHOUJI_PERSISTENT_DATA=1 -I"$scratch/ufs" -I"$3/drivers/RamBlock" \
    "$root/tests/persistent-data.cpp" -o "$scratch/test"
timeout 30 "$scratch/test"
