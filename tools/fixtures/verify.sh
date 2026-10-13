#!/bin/sh
# Cross-check generated fixtures with an independent reader (extract-xiso).
#
# Runs inside the nxdk Docker image, which ships extract-xiso and python3:
#   make fixtures-verify                      (from the repo root)
# or directly:
#   sh tools/fixtures/verify.sh <fixtures-dir>
#
# Every <name>.iso in the directory is extracted, and the result is compared
# with <name>.json (sizes and SHA-256 of every file, the directory set, and
# no extra files). Exits non-zero if any fixture fails.
set -eu

dir=${1:?usage: verify.sh <fixtures-dir>}
xiso=${EXTRACT_XISO:-/usr/src/nxdk/tools/extract-xiso/build/extract-xiso}
here=$(dirname "$0")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

status=0
found=0
for iso in "$dir"/*.iso; do
    [ -e "$iso" ] || continue
    found=1
    name=$(basename "$iso" .iso)
    if ! "$xiso" -q -d "$work/$name" -x "$iso" >"$work/$name.log" 2>&1; then
        echo "$name: extract-xiso failed:"
        cat "$work/$name.log"
        status=1
        continue
    fi
    python3 -I "$here/check_extracted.py" "$dir/$name.json" "$work/$name" || status=1
    rm -rf "${work:?}/$name"  # large fixtures extract to GBs; free space as we go
done

if [ "$found" -eq 0 ]; then
    echo "no .iso files in $dir"
    exit 1
fi
exit "$status"
