#!/usr/bin/env bash
# Exercises the two size checks kbsp gives an app (components/kbsp/
# project_include.cmake): the build's, against the apps region, and the one
# `idf.py flash` runs, against the mailbox. Each case is an empty file of a
# chosen size and an expected exit code.
#
# The numbers kbsp spells out are checked against kapps_fmt.h and
# partitions.csv first, since kbsp cannot include the header that owns them.
# The apps region's case needs ESP-IDF (IDF_PATH, which export.sh sets) for
# its partition table tools, and is skipped without it.
#   usage: tools/test_check_app_size.sh
#   exit:  0 all cases passed, 1 something regressed
set -uo pipefail
cd "$(dirname "$0")/.."

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

kbsp=components/kbsp
fmt=components/kdrv/apps/include/kapps_fmt.h

fail=0
pass() { printf 'ok    %s\n' "$1"; }
fault() { printf 'FAIL  %s  %s\n' "$1" "$2"; fail=1; }

# kbsp_value <name>: the value project_include.cmake sets <name> to.
kbsp_value() {
    sed -n "s/^set($1 \"\\(.*\\)\")/\\1/p" "$kbsp/project_include.cmake"
}
# fmt_value <name>: the value kapps_fmt.h defines <name> as, without its U.
fmt_value() {
    sed -n "s/^#define $1 *\\(0x[0-9A-Fa-f]*\\)U.*/\\1/p" "$fmt"
}
same_number() { [ -n "$1" ] && [ -n "$2" ] && [ $(($1)) -eq $(($2)) ]; }

mailbox_max="$(kbsp_value KBSP_MAILBOX_IMAGE_MAX)"
apps_subtype="$(kbsp_value KBSP_APPS_SUBTYPE)"
if same_number "$mailbox_max" "$(fmt_value KAPPS_PENDING_INTENT_OFF)"; then
    pass "KBSP_MAILBOX_IMAGE_MAX is KAPPS_PENDING_INTENT_OFF"
else
    fault "KBSP_MAILBOX_IMAGE_MAX" "is '$mailbox_max', kapps_fmt.h says '$(fmt_value KAPPS_PENDING_INTENT_OFF)'"
fi
if same_number "$apps_subtype" "$(fmt_value KAPPS_PART_SUBTYPE)"; then
    pass "KBSP_APPS_SUBTYPE is KAPPS_PART_SUBTYPE"
else
    fault "KBSP_APPS_SUBTYPE" "is '$apps_subtype', kapps_fmt.h says '$(fmt_value KAPPS_PART_SUBTYPE)'"
fi
csv_subtype="$(awk -F, '$1 ~ /^apps *$/ { gsub(/ /, "", $3); print $3 }' "$kbsp/partitions.csv")"
if same_number "$apps_subtype" "$csv_subtype"; then
    pass "partitions.csv's apps region has subtype $apps_subtype"
else
    fault "partitions.csv" "the apps region's subtype is '$csv_subtype', not '$apps_subtype'"
fi

# image <name> <bytes>: an empty file of that size.
image() {
    dd if=/dev/zero of="$work/$1" bs=1 count=0 seek="$2" 2> /dev/null
}

check_mailbox() {   # check_mailbox <name> <bytes> <expected_exit> [<text expected>]
    local name="$1" expected_exit="$3" expected_text="${4:-}" out rc
    image "$name.bin" "$2"
    out="$(cmake -DKBSP_BIN="$work/$name.bin" -DKBSP_MAX="$mailbox_max" \
                 -P "$kbsp/check_mailbox_size.cmake" 2>&1)"
    rc=$?
    if [ "$rc" -ne "$expected_exit" ]; then
        fault "$name" "expected exit $expected_exit, got $rc"$'\n'"$out"
    elif [ -n "$expected_text" ] && ! printf '%s' "$out" | grep -qF -- "$expected_text"; then
        fault "$name" "expected the output to say '$expected_text', got:"$'\n'"$out"
    else
        pass "$name"
    fi
}

check_mailbox "mailbox: an image that fills it" $((mailbox_max)) 0
check_mailbox "mailbox: one byte into the intent sector" $((mailbox_max + 1)) 1 "too many"

if [ -z "${IDF_PATH:-}" ]; then
    printf 'skip  apps region: IDF_PATH is not set\n'
else
    tools_dir="$IDF_PATH/components/partition_table"
    if ! python3 "$tools_dir/gen_esp32part.py" -q "$kbsp/partitions.csv" "$work/table.bin"; then
        fault "apps region" "gen_esp32part.py refused partitions.csv"
    else
        apps_size="$(awk -F, '$1 ~ /^apps *$/ { gsub(/ /, "", $5); print $5 }' "$kbsp/partitions.csv")"
        check_apps() {   # check_apps <name> <bytes> <expected_exit>
            local out rc
            image "$1.bin" "$2"
            out="$(python3 "$tools_dir/check_sizes.py" partition --type data \
                       --subtype "$apps_subtype" "$work/table.bin" "$work/$1.bin" 2>&1)"
            rc=$?
            if [ "$rc" -ne "$3" ]; then
                fault "$1" "expected exit $3, got $rc"$'\n'"$out"
            else
                pass "$1"
            fi
        }
        # 5 MB is more than a kodeOS slot holds and well inside the region.
        check_apps "apps region: a 5 MB app" $((5 * 1024 * 1024)) 0
        check_apps "apps region: one byte more than the region" $((apps_size + 1)) 1
    fi
fi

exit "$fail"
