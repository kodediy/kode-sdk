#!/usr/bin/env bash
#
# The P4 and the C5 speak two wire formats, KODE (kode_wire.h) and the
# infrared frames (kir_wire.h), and each header lives twice: once in the SDK
# for the P4, once in the C5's firmware, because the two are built by
# different projects for different chips. They must stay the same file - a
# drift builds on both sides and breaks the link between the chips without a
# word. The copies differ by one line, the SDK's SPDX header, which this
# ignores; anything else is a failure naming the twin.
#
# The C5 is a nested submodule, not checked out by the SDK's own CI. Without
# it the check is skipped, unless KODE_REQUIRE_C5 is set - which KodeOS's CI
# does, having fetched it.
#
#   usage: tools/check_wire_twins.sh [<sdk_root>]   (default: this script's SDK)
#   exit:  0 the twins agree (or no C5 to compare), 1 they do not
set -uo pipefail

root="${1:-$(dirname "$0")/..}"

# "<the C5's copy>:<the SDK's copy>", relative to the SDK root.
TWINS=(
    "c5/main/kode_wire.h:components/kdrv/radio/include/kode_wire.h"
    "c5/main/kir_wire.h:components/kdrv/ir/include/kir_wire.h"
)

if [ ! -d "$root/c5/main" ]; then
    if [ -n "${KODE_REQUIRE_C5:-}" ]; then
        echo "::error::the C5 firmware is not checked out under $root/c5; its wire headers cannot be compared" >&2
        exit 1
    fi
    echo "skipped  the C5 firmware is not checked out; the wire twins were not compared"
    exit 0
fi

fail=0
for pair in "${TWINS[@]}"; do
    c5="$root/${pair%%:*}"
    sdk="$root/${pair##*:}"
    # Two missing files compare equal through grep and diff, so a header that
    # moved on both sides would pass unseen: each copy must exist.
    if [ ! -f "$c5" ] || [ ! -f "$sdk" ]; then
        echo "::error::${pair##*/}: ${pair%%:*} or ${pair##*:} is missing" >&2
        fail=1
        continue
    fi
    if ! diff -q <(grep -v 'SPDX-License-Identifier' "$c5") \
                 <(grep -v 'SPDX-License-Identifier' "$sdk") > /dev/null; then
        echo "::error::${pair##*/} has drifted between ${pair%%:*} and ${pair##*:}" >&2
        diff <(grep -v 'SPDX-License-Identifier' "$c5") \
             <(grep -v 'SPDX-License-Identifier' "$sdk") >&2
        fail=1
    fi
done

if [ "$fail" -eq 0 ]; then
    echo "ok    ${#TWINS[@]} wire headers agree between the SDK and the C5"
fi
exit "$fail"
