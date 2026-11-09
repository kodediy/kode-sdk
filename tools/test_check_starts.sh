#!/usr/bin/env bash
# Exercises tools/check_starts.sh against archives built on the spot. Each
# case is a few one-line C files put into a kaos and a kode_sys archive, and an
# expected exit code.
#
# With ESP-IDF's toolchain on the PATH (export.sh puts it there) the archives
# are ELF, built with its gcc and ar and read with its nm, which tells a weak
# reference from a strong one, so the weak cases run too. Without it, the
# host's cc, ar and nm, and a Mach-O nm skips them. CC, AR and NM name others.
#   usage: tools/test_check_starts.sh
#   exit:  0 all cases passed, 1 something regressed
set -uo pipefail
cd "$(dirname "$0")/.."

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

toolchain=riscv32-esp-elf
if command -v "$toolchain-nm" > /dev/null 2>&1; then
    cc_tool="${CC:-$toolchain-gcc}"
    ar_tool="${AR:-$toolchain-ar}"
    nm_tool="${NM:-$toolchain-nm}"
else
    cc_tool="${CC:-cc}"
    ar_tool="${AR:-ar}"
    nm_tool="${NM:-nm}"
fi

# archive <case> <lib> <name=source>...: compile each source and archive it
# where check_starts.sh looks for <lib>.
archive() {
    local name="$1" lib="$2" dir
    shift 2
    dir="$work/$name/esp-idf/$lib"
    mkdir -p "$dir"
    for entry in "$@"; do
        printf '%s\n' "${entry#*=}" > "$dir/${entry%%=*}.c"
        "$cc_tool" -c -o "$dir/${entry%%=*}.o" "$dir/${entry%%=*}.c" || exit 1
    done
    (cd "$dir" && "$ar_tool" rcs "lib$lib.a" ./*.o) || exit 1
}

fail=0
check() {   # check <name> <expected_exit> [<text expected in the output> [<option>...]]
    local name="$1" expected_exit="$2" expected_text="${3:-}"
    local out rc
    shift 3 2>/dev/null || shift $#
    out="$(NM="$nm_tool" tools/check_starts.sh "$work/$name" "$@" 2>&1)"
    rc=$?
    if printf '%s' "$out" | grep -q 'check_starts.sh: line'; then
        printf 'FAIL  %s  the script tripped over its own input:\n%s\n' "$name" "$out"
        fail=1
    elif [ "$rc" -ne "$expected_exit" ]; then
        printf 'FAIL  %s  expected exit %s, got %s\n%s\n' "$name" "$expected_exit" "$rc" "$out"
        fail=1
    elif [ -n "$expected_text" ] && ! printf '%s' "$out" | grep -qF -- "$expected_text"; then
        printf 'FAIL  %s  expected the output to say %q, got:\n%s\n' "$name" "$expected_text" "$out"
        fail=1
    else
        printf 'ok    %s\n' "$name"
    fi
}

SYS_OK='kode_sys_ir=void kode_sys_ir_init(void) {} void kode_sys_ir_send(void) {}'

# ---- every queue beside its start, every set-up beside its doors -----------
archive good kaos \
    'radio_ao=void *const AO_Radio = 0; void kaos_radio_start(void) {}' \
    'led_mgr=void *const AO_LedMgr = 0; void kaos_ledmgr_start(void) {}'
archive good kode_sys "$SYS_OK"
check good 0 "starts: 2 Active Objects and 1 door set-ups"

# ---- a queue whose start lives elsewhere ------------------------------------
archive split kaos \
    'radio_ao=void *const AO_Radio = 0; void radio_park_request(void) {}' \
    'radio_start=void kaos_radio_start(void) {}'
archive split kode_sys "$SYS_OK"
check split 1 "AO_Radio is in radio_ao"

# ---- a start with no queue beside it ----------------------------------------
archive orphan kaos \
    'led_mgr=void *const AO_LedMgr = 0; void kaos_ledmgr_start(void) {}' \
    'nfc_ao=void kaos_nfc_start(void) {}'
archive orphan kode_sys "$SYS_OK"
check orphan 1 "kaos_nfc_start is in nfc_ao"

# ---- a door set-up away from its doors --------------------------------------
archive setup kaos 'led_mgr=void *const AO_LedMgr = 0; void kaos_ledmgr_start(void) {}'
archive setup kode_sys 'kode_sys=void kode_sys_ir_init(void) {}'
check setup 1 "kode_sys_ir_init is in kode_sys"

# ---- a dependency that is a real reference, and one that is not --------------
archive dep_ok kaos \
    'audio_ao=void radio_amp_request(int on); void *const AO_Audio = 0; void kaos_audio_start(void) { radio_amp_request(1); }' \
    'radio_ao=void *const AO_Radio = 0; void kaos_radio_start(void) {} void radio_amp_request(int on) { (void)on; }'
archive dep_ok kode_sys "$SYS_OK"
check dep_ok 0 "starts: 2 Active Objects"

# The pairs the start order and the C5 rely on beyond the radio's: infrared is
# served over the C5 link, and the haptic row posts to AO_Imu. A user that
# names nothing of its owner fails.
archive dep_ir kaos \
    'ir_ao=void *const AO_Ir = 0; void kaos_ir_start(void) {}' \
    'radio_ao=void *const AO_Radio = 0; void kaos_radio_start(void) {} void radio_up_request(void) {}'
archive dep_ir kode_sys "$SYS_OK"
check dep_ir 1 "AO_Ir (ir_ao"

archive dep_haptic kaos \
    'haptic_ao=void *const AO_Haptic = 0; void kaos_haptic_start(void) {}' \
    'imu_ao=void *const AO_Imu = 0; void kaos_imu_start(void) {} void imu_capture_request(void) {}'
archive dep_haptic kode_sys "$SYS_OK"
check dep_haptic 1 "AO_Haptic (haptic_ao"

# Only where nm tells a weak reference from a strong one, as GNU nm on ELF
# does ("w"); Mach-O's prints both as "U", so the case skips there.
printf '__attribute__((weak)) void probe(void); void f(void) { if (probe) { probe(); } }\n' \
    > "$work/weak_probe.c"
"$cc_tool" -c -o "$work/weak_probe.o" "$work/weak_probe.c" || exit 1
if "$nm_tool" "$work/weak_probe.o" | grep -q ' w '; then
    archive dep_weak kaos \
        'audio_ao=__attribute__((weak)) void radio_amp_request(int on); void *const AO_Audio = 0; void kaos_audio_start(void) { if (radio_amp_request) { radio_amp_request(1); } }' \
        'radio_ao=void *const AO_Radio = 0; void kaos_radio_start(void) {} void radio_amp_request(int on) { (void)on; }'
    archive dep_weak kode_sys "$SYS_OK"
    check dep_weak 1 "AO_Audio (audio_ao"
else
    printf 'skip  dep_weak  this nm prints a weak reference as U\n'
fi

# ---- a publish-only door away from its start ----------------------------------
archive door kaos \
    'touch_ao=void *const AO_Touch = 0; void kaos_touch_start(void) {}' \
    'touch_door=void touch_subscribe(void) {}'
archive door kode_sys "$SYS_OK"
check door 1 "touch_subscribe is in touch_door"

# ---- the image's promises ------------------------------------------------------
# image <case> <source>: the "image" the promises read; an object is all nm needs.
image() {
    printf '%s\n' "$2" > "$work/$1/app.c"
    "$cc_tool" -c -o "$work/$1/app.elf" "$work/$1/app.c" || exit 1
}

# role <case> <APP|OS>: the configuration header a build writes, naming its role.
role() {
    mkdir -p "$work/$1/config"
    printf '#define CONFIG_KODEOS_BOOT_ROLE_%s 1\n' "$2" > "$work/$1/config/sdkconfig.h"
}
image good 'void kaos_radio_start(void) {} void kaos_ledmgr_start(void) {}'
check good 1 "missing required input" --all
role good APP
check good 0 "every Active Object compiled is in the image" --all
check good 0 "none of AO_Display,AO_Net is in the image" --absent AO_Display,AO_Net
check good 1 "in the image, and the app never uses them: AO_Radio" --absent AO_Radio
check good 1 "usage" --absent

archive missing kaos \
    'radio_ao=void *const AO_Radio = 0; void kaos_radio_start(void) {}' \
    'led_mgr=void *const AO_LedMgr = 0; void kaos_ledmgr_start(void) {}'
archive missing kode_sys "$SYS_OK"
image missing 'void kaos_ledmgr_start(void) {}'
role missing APP
check missing 1 "compiled but not in the image: AO_Radio" --all

# ---- the OS role: kode_init() names everything strongly ----------------------
# kodeOS uses every module, so its kode_init() links them all by name; a start
# or a door set-up it names weakly, or not at all, would be parked in kodeOS.
OS_KAOS=(
    'radio_ao=void *const AO_Radio = 0; void kaos_radio_start(void) {}'
    'led_mgr=void *const AO_LedMgr = 0; void kaos_ledmgr_start(void) {}'
)
OS_IMAGE='void kaos_radio_start(void) {} void kaos_ledmgr_start(void) {}'

archive os_ok kaos "${OS_KAOS[@]}"
archive os_ok kode_sys "$SYS_OK" \
    'kode_sys=void kaos_radio_start(void); void kaos_ledmgr_start(void); void kode_sys_ir_init(void); void (*const rows[])(void) = { kaos_radio_start, kaos_ledmgr_start, kode_sys_ir_init }; void kode_init(void) {}'
image os_ok "$OS_IMAGE"
role os_ok OS
check os_ok 0 "the OS role names every start and door set-up strongly" --all

archive os_unnamed kaos "${OS_KAOS[@]}"
archive os_unnamed kode_sys "$SYS_OK" \
    'kode_sys=void kaos_ledmgr_start(void); void kode_sys_ir_init(void); void (*const rows[])(void) = { kaos_ledmgr_start, kode_sys_ir_init }; void kode_init(void) {}'
image os_unnamed "$OS_IMAGE"
role os_unnamed OS
check os_unnamed 1 "names weakly or not at all: kaos_radio_start" --all
# The same build in the app role is the one where weak is the point.
role os_unnamed APP
check os_unnamed 0 "every Active Object compiled is in the image" --all

archive os_noinit kaos "${OS_KAOS[@]}"
archive os_noinit kode_sys "$SYS_OK"
image os_noinit "$OS_IMAGE"
role os_noinit OS
check os_noinit 1 "no object defines kode_init" --all

if "$nm_tool" "$work/weak_probe.o" | grep -q ' w '; then
    archive os_weak kaos "${OS_KAOS[@]}"
    archive os_weak kode_sys "$SYS_OK" \
        'kode_sys=__attribute__((weak)) void kaos_radio_start(void); void kaos_ledmgr_start(void); void kode_sys_ir_init(void); void (*const rows[])(void) = { kaos_radio_start, kaos_ledmgr_start, kode_sys_ir_init }; void kode_init(void) {}'
    image os_weak "$OS_IMAGE"
    role os_weak OS
    check os_weak 1 "names weakly or not at all: kaos_radio_start" --all
else
    printf 'skip  os_weak  this nm prints a weak reference as U\n'
fi

# ---- no archive at all -------------------------------------------------------
mkdir -p "$work/empty"
check empty 1 "missing required input"

exit "$fail"
