#!/usr/bin/env bash
#
# The internal RAM gate: what a firmware puts there at link time.
#
# The ESP32-P4 has 640 KB of internal RAM and 32 MB of PSRAM. Everything that
# cannot live in PSRAM comes out of the 640 KB - DMA buffers, task stacks,
# anything an interrupt touches while the flash cache is off - and so does
# every global and every `static`, because the linker puts .data and .bss
# there. What the image takes at link time is what the heap does not get at
# boot. A build that takes too much still links, still passes the host
# suites, and stops at "Could not reserve internal/DMA pool" before its own
# banner, or boots and leaves the radio without the last kilobyte it needs.
#
# Two checks, both against a budget file the project keeps:
#
#   1. The static image in internal RAM - code in IRAM, .data and .bss - is
#      within the file's `static_max`.
#   2. Every data symbol of 2048 bytes or more in internal RAM has a line in
#      the file, and is no larger than its line says. A buffer that size
#      belongs in PSRAM (khal_alloc_psram()); the ones that must be internal
#      are few, and the file is their list.
#
# The budget file: `#` comments, blank lines, one `static_max <bytes>` line,
# and one `<symbol> <source file> <max bytes>` line per large static. The
# source file is the base name of where the symbol is defined, or `-` for a
# symbol with no debug information; a name alone is not enough, since every
# file here has an `s_buf`.
#
#   usage: tools/check_ram.sh <firmware.elf> <budget file>
#   stdout: "<name> internal RAM, static: <n> of <max> bytes, <left> left (<pct>%)"
#           "<name> <k> statics of 2048 bytes or more in internal RAM, all listed"
#   exit:   0 within the budget, 1 past it or if an input cannot be read
#
# Needs riscv32-esp-elf-size and riscv32-esp-elf-nm on the PATH, which
# ESP-IDF's export.sh puts there.
set -uo pipefail

elf="${1:?usage: tools/check_ram.sh <firmware.elf> <budget file>}"
budget="${2:?usage: tools/check_ram.sh <firmware.elf> <budget file>}"

# The P4's internal RAM (HP L2MEM), 0x4FF00000 up to 0x4FFC0000. A section or
# a symbol is internal when its address is inside it.
RAM_LO=$((16#4FF00000))
RAM_HI=$((16#4FFC0000))

# A static this size or larger needs a line in the budget file.
LARGE=2048

for input in "$elf" "$budget"; do
    if [ ! -f "$input" ]; then
        echo "error: missing required input: $input" >&2
        exit 1
    fi
done
for tool in riscv32-esp-elf-size riscv32-esp-elf-nm; do
    if ! command -v "$tool" > /dev/null 2>&1; then
        echo "error: $tool is not on the PATH - activate ESP-IDF first" >&2
        exit 1
    fi
done

static_max="$(awk '$1 == "static_max" { print $2; exit }' "$budget")"
case "$static_max" in
    ''|*[!0-9]*)
        echo "error: $budget has no \`static_max <bytes>\` line" >&2
        exit 1
        ;;
esac

name="$(basename "$elf")"
fail=0

# ---- 1. the static image ----------------------------------------------------
# `size -A` prints one section a line: name, size, address, both in decimal.
used=0
while read -r _ size addr _; do
    # The header and the `Total` line have no address, or no number at all.
    case "$size" in
        ''|*[!0-9]*) continue ;;
    esac
    case "$addr" in
        ''|*[!0-9]*) continue ;;
    esac
    if [ "$addr" -ge "$RAM_LO" ] && [ "$addr" -lt "$RAM_HI" ]; then
        used=$((used + size))
    fi
done < <(riscv32-esp-elf-size -A "$elf")

if [ "$used" -eq 0 ]; then
    echo "error: $elf has nothing in internal RAM - not an ESP32-P4 firmware?" >&2
    exit 1
fi

left=$((static_max - used))
pct_left=$((left * 100 / static_max))
printf '%s internal RAM, static: %d of %d bytes, %d left (%d%%)\n' \
    "$name" "$used" "$static_max" "$left" "$pct_left"

if [ "$used" -gt "$static_max" ]; then
    printf 'error: %s puts %d bytes in internal RAM at link time, past the ceiling of %d (static_max in %s)\n' \
        "$name" "$used" "$static_max" "$budget" >&2
    fail=1
fi

# ---- 2. the large statics ---------------------------------------------------
# `nm -S --size-sort -r -l`: address, size, type, name and, after a tab, the
# file and line of the definition; largest first, so the walk stops at the
# first symbol under the threshold.
large=0
while IFS=$' \t' read -r addr size type sym where; do
    case "$addr$size" in
        ''|*[!0-9A-Fa-f]*) continue ;;
    esac
    bytes=$((16#$size))
    if [ "$bytes" -lt "$LARGE" ]; then
        break
    fi
    at=$((16#$addr))
    if [ "$at" -lt "$RAM_LO" ] || [ "$at" -ge "$RAM_HI" ]; then
        continue
    fi
    # Data only. Code in IRAM is in the total above and is nobody's buffer.
    case "$type" in
        b|B|d|D) ;;
        *) continue ;;
    esac

    src="-"
    if [ -n "${where:-}" ]; then
        src="$(basename "${where%:*}")"
    fi
    large=$((large + 1))

    listed="$(awk -v s="$sym" -v f="$src" '$1 == s && $2 == f { print $3; exit }' "$budget")"
    case "$listed" in
        ''|*[!0-9]*)
            printf 'error: %s (%s) puts %d bytes in internal RAM and has no line in %s\n' \
                "$sym" "$src" "$bytes" "$budget" >&2
            printf '       a buffer this size belongs in PSRAM (khal_alloc_psram()); if it has to be internal, list it\n' >&2
            fail=1
            ;;
        *)
            if [ "$bytes" -gt "$listed" ]; then
                printf 'error: %s (%s) is %d bytes, listed at %d in %s\n' \
                    "$sym" "$src" "$bytes" "$listed" "$budget" >&2
                fail=1
            fi
            ;;
    esac
done < <(riscv32-esp-elf-nm -S --size-sort -r -l "$elf" 2> /dev/null)

if [ "$fail" -eq 0 ]; then
    printf '%s %d statics of %d bytes or more in internal RAM, all listed\n' \
        "$name" "$large" "$LARGE"
fi
exit "$fail"
