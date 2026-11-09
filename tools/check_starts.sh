#!/usr/bin/env bash
#
# The start check: an owner's start and its queue live in one object.
#
# In an app, kode_init() names every optional Active Object's start weakly
# (KAOS_WEAK, kaos.h), so a start runs exactly when the linker kept the
# object that defines it, and the linker keeps an object when anything uses
# a symbol in it. A door posts through the owner's pointer, AO_<Name>. So a linked door
# links AO_<Name>'s object, and the start comes with it, as long as both are
# defined in that one object. Were they ever apart, a door could be linked
# without its start, and its first post would go to an AO with no queue: a
# boot loop. The same holds for a door module of kode_sys and its set-up,
# kode_sys_<module>_init(), which kode_init() names weakly too.
#
# What is checked, over the archives a build made:
#   1. Every AO_<Name> that kaos defines is defined once, in an object that
#      also defines kaos_<name>_start (the name in lower case).
#   2. Every kaos_<name>_start is defined beside its AO_<Name>.
#   3. Every kode_sys_<module>_init is defined in kode_sys_<module>.c, the
#      object that holds that module's doors.
#   4. The dependencies the startup order and the C5 rely on are real
#      references: AO_AppMgr names AO_Storage and AO_Net, AO_Net names
#      AO_Radio, AO_Radio names AO_Storage, AO_Audio and AO_Ir name AO_Radio
#      (the amplifier's enable and the infrared block are the C5's), AO_Audio
#      names AO_Storage (record-to-file), and AO_Haptic names AO_Imu, each
#      through a symbol its object needs strongly - so linking the first
#      links the second, and kode_init() starts both.
#   5. The doors that only publish or subscribe - they name no AO_<Name>, so
#      checks 1 and 2 cannot see them - sit beside their owner's start:
#      touch_subscribe() beside kaos_touch_start(), radio_amp_request()
#      beside kaos_radio_start(). A door of that kind added anywhere else is
#      a door that links nothing; list it here.
#
# And, over the image itself, one of two promises:
#   --all              every Active Object kaos compiled is in the image: a
#                      build that uses everything starts everything. In the
#                      OS role (config/sdkconfig.h) also: the object that
#                      defines kode_init() needs every kaos_<name>_start and
#                      every kode_sys_<module>_init strongly, so kodeOS links
#                      every module by name and parks none.
#   --absent A,B,...   none of the named AO_<Name> is in the image: an app
#                      that never uses them links none of them.
#
#   usage: tools/check_starts.sh <build dir> [--all | --absent <AO_Name,...>]
#   stdout: "starts: <n> Active Objects and <k> door set-ups, each beside what it starts"
#           and one line for the promise kept
#   exit:   0 every check holds, 1 one does not or an input cannot be read
#
# Needs riscv32-esp-elf-nm on the PATH, which ESP-IDF's export.sh puts there;
# NM names another nm.
set -uo pipefail

usage='usage: tools/check_starts.sh <build dir> [--all | --absent <AO_Name,...>]'
build="${1:?$usage}"
promise="${2:-}"
listed="${3:-}"
if [ -n "$promise" ] && [ "$promise" != "--all" ] && [ "$promise" != "--absent" ]; then
    echo "$usage" >&2
    exit 1
fi
if [ "$promise" = "--absent" ] && [ -z "$listed" ]; then
    echo "$usage" >&2
    exit 1
fi
nm_tool="${NM:-riscv32-esp-elf-nm}"
kaos="$build/esp-idf/kaos/libkaos.a"
sys="$build/esp-idf/kode_sys/libkode_sys.a"

for input in "$kaos" "$sys"; do
    if [ ! -f "$input" ]; then
        echo "error: missing required input: $input" >&2
        exit 1
    fi
done

# "<object> <symbol>" for every global symbol an archive defines. An archive
# lists its members as "<member>:" (GNU) or "<archive>(<member>):" (LLVM); a
# Mach-O name carries a leading underscore. Both are taken off.
defined() {   # defined <archive>
    "$nm_tool" -g "$1" 2>/dev/null | awk '
        /:$/ { obj = $0; sub(/:$/, "", obj); sub(/^.*\(/, "", obj); sub(/\)$/, "", obj); next }
        NF >= 3 && $2 != "U" && $2 != "w" && $2 != "v" {
            sym = $3; sub(/^_/, "", sym); print obj, sym
        }'
}

# "<object> <symbol>" for every symbol an archive's members need strongly; a
# weak reference ("w") is not one, and is the point of the check.
needed() {   # needed <archive>
    "$nm_tool" "$1" 2>/dev/null | awk '
        /:$/ { obj = $0; sub(/:$/, "", obj); sub(/^.*\(/, "", obj); sub(/\)$/, "", obj); next }
        NF >= 2 && $(NF-1) == "U" { sym = $NF; sub(/^_/, "", sym); print obj, sym }'
}

kaos_defs="$(defined "$kaos")" || exit 1
kaos_needs="$(needed "$kaos")" || exit 1
sys_defs="$(defined "$sys")" || exit 1

fail=0
aos=0
inits=0

# 1. Each AO_<Name>, once, beside its start.
while read -r obj sym; do
    name="${sym#AO_}"
    lower="$(printf '%s' "$name" | tr '[:upper:]' '[:lower:]')"
    start="kaos_${lower}_start"
    aos=$((aos + 1))
    where="$(printf '%s\n' "$kaos_defs" | awk -v s="$sym" '$2 == s { print $1 }')"
    if [ "$(printf '%s\n' "$where" | wc -l | tr -d ' ')" -ne 1 ]; then
        printf 'FAIL  %s is defined in more than one object: %s\n' "$sym" "$(printf '%s ' $where)"
        fail=1
    fi
    if ! grep -qx "$obj $start" <<< "$kaos_defs"; then
        printf 'FAIL  %s is in %s and %s is not: a door could link without its start\n' \
            "$sym" "$obj" "$start"
        fail=1
    fi
done < <(printf '%s\n' "$kaos_defs" | awk '$2 ~ /^AO_[A-Za-z0-9]+$/')

# 2. Each start beside its AO_<Name>.
while read -r obj sym; do
    lower="${sym#kaos_}"
    lower="${lower%_start}"
    if ! printf '%s\n' "$kaos_defs" | awk -v o="$obj" -v l="$lower" '
            $1 == o && $2 ~ /^AO_/ { n = substr($2, 4); if (tolower(n) == l) { found = 1 } }
            END { exit found ? 0 : 1 }'; then
        printf 'FAIL  %s is in %s, and the AO it starts is not\n' "$sym" "$obj"
        fail=1
    fi
done < <(printf '%s\n' "$kaos_defs" | awk '$2 ~ /^kaos_[a-z0-9]+_start$/')

# 3. Each door set-up in its module's own object.
while read -r obj sym; do
    module="${sym#kode_sys_}"
    module="${module%_init}"
    inits=$((inits + 1))
    if [ "$obj" != "kode_sys_${module}.c.obj" ] && [ "$obj" != "kode_sys_${module}.o" ]; then
        printf 'FAIL  %s is in %s, not with its doors in kode_sys_%s.c\n' "$sym" "$obj" "$module"
        fail=1
    fi
done < <(printf '%s\n' "$sys_defs" | awk '$2 ~ /^kode_sys_[a-z0-9]+_init$/')

# 4. The dependencies, where both owners were compiled.
for pair in AppMgr:Storage AppMgr:Net Net:Radio Radio:Storage Audio:Radio Audio:Storage Ir:Radio Haptic:Imu; do
    user="AO_${pair%%:*}"
    owner="AO_${pair##*:}"
    user_obj="$(printf '%s\n' "$kaos_defs" | awk -v s="$user" '$2 == s { print $1 }')"
    owner_obj="$(printf '%s\n' "$kaos_defs" | awk -v s="$owner" '$2 == s { print $1 }')"
    if [ -z "$user_obj" ] || [ -z "$owner_obj" ]; then
        continue
    fi
    if ! awk -v u="$user_obj" -v o="$owner_obj" '
            NR == FNR { if ($1 == o) { def[$2] = 1 } next }
            $1 == u && ($2 in def) { found = 1 }
            END { exit found ? 0 : 1 }' \
            <(printf '%s\n' "$kaos_defs") <(printf '%s\n' "$kaos_needs"); then
        printf 'FAIL  %s (%s) names nothing of %s (%s) strongly: linking it would not start %s\n' \
            "$user" "$user_obj" "$owner" "$owner_obj" "$owner"
        fail=1
    fi
done

# 5. The doors with no AO pointer in them, beside their start.
for pair in touch_subscribe:touch radio_amp_request:radio ble_app_subscribe:radio; do
    door="${pair%%:*}"
    start="kaos_${pair##*:}_start"
    door_obj="$(awk -v s="$door" '$2 == s { print $1 }' <<< "$kaos_defs")"
    if [ -z "$door_obj" ]; then
        continue
    fi
    if ! grep -qx "$door_obj $start" <<< "$kaos_defs"; then
        printf 'FAIL  %s is in %s and %s is not: the door would link without its start\n' \
            "$door" "$door_obj" "$start"
        fail=1
    fi
done

if [ "$aos" -eq 0 ]; then
    echo "FAIL  no AO_<Name> found in $kaos" >&2
    fail=1
fi
# The image's promise, read from each owner's start: kode_init()'s table
# names every start, so a linked one stays in the image, while the AO_<Name>
# pointer is a constant the link may fold away.
start_of() {   # start_of <AO_Name>
    printf 'kaos_%s_start' "$(printf '%s' "${1#AO_}" | tr '[:upper:]' '[:lower:]')"
}
if [ -n "$promise" ]; then
    elf="$(ls "$build"/*.elf 2>/dev/null | head -n 1)"
    if [ -z "$elf" ]; then
        echo "error: missing required input: $build/*.elf" >&2
        exit 1
    fi
    in_image="$("$nm_tool" -g "$elf" 2>/dev/null | awk 'NF >= 3 && $2 != "U" && $2 != "w" { sym = $3; sub(/^_/, "", sym); print sym }')"
    if [ "$promise" = "--all" ]; then
        config="$build/config/sdkconfig.h"
        if [ ! -f "$config" ]; then
            echo "error: missing required input: $config" >&2
            exit 1
        fi
        if grep -q '^#define CONFIG_KODEOS_BOOT_ROLE_OS 1' "$config"; then
            init_obj="$(awk '$2 == "kode_init" { print $1 }' <<< "$sys_defs")"
            if [ -z "$init_obj" ]; then
                printf 'FAIL  no object defines kode_init in %s\n' "$sys"
                fail=1
            else
                sys_needs="$(needed "$sys")" || exit 1
                unnamed=""
                for sym in $(printf '%s\n' "$kaos_defs" | awk '$2 ~ /^kaos_[a-z0-9]+_start$/ { print $2 }') \
                           $(printf '%s\n' "$sys_defs" | awk '$2 ~ /^kode_sys_[a-z0-9]+_init$/ { print $2 }'); do
                    if ! grep -qx "$init_obj $sym" <<< "$sys_needs"; then
                        unnamed="$unnamed $sym"
                    fi
                done
                if [ -n "$unnamed" ]; then
                    printf 'FAIL  in the OS role, %s names weakly or not at all:%s\n' \
                        "$init_obj" "$unnamed"
                    fail=1
                else
                    echo "starts: the OS role names every start and door set-up strongly"
                fi
            fi
        fi
        missing=""
        for sym in $(printf '%s\n' "$kaos_defs" | awk '$2 ~ /^AO_[A-Za-z0-9]+$/ { print $2 }'); do
            if ! grep -qx "$(start_of "$sym")" <<< "$in_image"; then
                missing="$missing $sym"
            fi
        done
        if [ -n "$missing" ]; then
            printf 'FAIL  compiled but not in the image:%s\n' "$missing"
            fail=1
        else
            echo "starts: every Active Object compiled is in the image"
        fi
    else
        present=""
        for sym in $(printf '%s' "$listed" | tr ',' ' '); do
            if grep -qx "$(start_of "$sym")" <<< "$in_image"; then
                present="$present $sym"
            fi
        done
        if [ -n "$present" ]; then
            printf 'FAIL  in the image, and the app never uses them:%s\n' "$present"
            fail=1
        else
            printf 'starts: none of %s is in the image\n' "$listed"
        fi
    fi
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi
printf 'starts: %u Active Objects and %u door set-ups, each beside what it starts\n' "$aos" "$inits"
