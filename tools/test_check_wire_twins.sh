#!/usr/bin/env bash
# Exercises tools/check_wire_twins.sh on a scratch tree: twins that differ only
# by the SPDX line pass, a drifted twin fails naming its file, and a tree with
# no C5 checkout is skipped - or refused when the C5 is required.
#   usage: tools/test_check_wire_twins.sh
#   exit:  0 all cases passed, 1 something regressed
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
script="$PWD/tools/check_wire_twins.sh"

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

SPDX='/* SPDX-License-Identifier: GPL-3.0-or-later */'

plant() {   # plant <tree> - one matching pair of each twin
    mkdir -p "$1/c5/main" "$1/components/kdrv/radio/include" "$1/components/kdrv/ir/include"
    printf '#define KODE_WIRE_VERSION 3\n' > "$1/c5/main/kode_wire.h"
    printf '%s\n#define KODE_WIRE_VERSION 3\n' "$SPDX" > "$1/components/kdrv/radio/include/kode_wire.h"
    printf '#define KIR_MAX_SYMBOLS 64\n' > "$1/c5/main/kir_wire.h"
    printf '%s\n#define KIR_MAX_SYMBOLS 64\n' "$SPDX" > "$1/components/kdrv/ir/include/kir_wire.h"
}

fail=0
check() {   # check <name> <tree> <expected_exit> [<fragment>] [<env>]
    local name="$1" tree="$2" expected="$3" fragment="${4:-}" env="${5:-}"
    local out rc
    out="$(env $env "$script" "$tree" 2>&1)"
    rc=$?
    if [ "$rc" -ne "$expected" ]; then
        printf 'FAIL  %s: expected exit %s, got %s: %s\n' "$name" "$expected" "$rc" "$out"
        fail=1
    elif [ -n "$fragment" ] && [[ "$out" != *"$fragment"* ]]; then
        printf 'FAIL  %s: the output does not say %q: %s\n' "$name" "$fragment" "$out"
        fail=1
    else
        printf 'ok    %s\n' "$name"
    fi
}

plant "$scratch/same"
check "twins that differ only by the SPDX line pass" "$scratch/same" 0

plant "$scratch/drift"
printf '#define KODE_WIRE_VERSION 4\n' > "$scratch/drift/c5/main/kode_wire.h"
check "a drifted twin fails, naming it" "$scratch/drift" 1 "kode_wire.h"

plant "$scratch/gone"
rm "$scratch/gone/c5/main/kir_wire.h" "$scratch/gone/components/kdrv/ir/include/kir_wire.h"
check "a twin missing on both sides fails, naming it" "$scratch/gone" 1 "kir_wire.h"

mkdir -p "$scratch/noc5/components"
check "no C5 checkout is skipped" "$scratch/noc5" 0 "skipped"
check "no C5 checkout is refused when the C5 is required" "$scratch/noc5" 1 "" "KODE_REQUIRE_C5=1"

if [ "$fail" -eq 0 ]; then
    echo "all cases passed"
else
    echo "one or more cases failed"
fi
exit "$fail"
