#!/usr/bin/env bash
#
# The host suites: every test that does not need the board.
#
# There are eight binaries, not one, and the reason is a rule C gives us no way
# around - one definition per binary. The main suite fakes or mocks a seam
# that some other project wants to test for real: khal_nvs, khal_time,
# kdrv_slot, kdrv_net, khal_pwm and kdrv_es8311 each get a project of their
# own rather than taking their stand-in away from every driver and AO case in
# the main one.
# The eighth, the screen analysis of the end-of-line test, fakes nothing: it
# is pure C judged on photo frames, and its own project keeps those out of the
# main one.
#
# Every project is built and run even when one before it failed, so one red
# suite never hides the state of the other seven; the last lines say which
# failed and why, and the exit is 1 if any did.
#
#   usage: tools/host_tests.sh [--sanitize]
#
# --sanitize builds every project in its own build_san directory with
# AddressSanitizer and UndefinedBehaviorSanitizer on the code under test (see
# tests/sanitize.cmake) and runs it; a memory error or undefined
# behaviour aborts the case. The plain build is left alone.
#   exit:  0 every case passed, 1 something failed or a suite shrank
#
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

BUILD=build
if [ "${1:-}" = "--sanitize" ]; then
    BUILD=build_san
    export KODE_SANITIZE=1
    # Leaks off: the cases keep their fixtures for the life of the process.
    export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:strict_string_checks=1"
    export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
fi

# "<project directory>:<elf name>", in the order they run.
PROJECTS=(
    "tests/host:kode_host_tests"
    "tests/host_nvs:kode_host_nvs"
    "tests/host_time:kode_host_time"
    "tests/host_slot:kode_host_slot"
    "tests/host_net:kode_host_net"
    "tests/host_pwm:kode_host_pwm"
    "tests/host_codec:kode_host_codec"
    "tests/host_tdan:kode_host_tdan"
)

if [ -z "${IDF_PATH:-}" ]; then
    printf '\033[31mFAIL\033[0m  ESP-IDF is not activated.\n'
    printf '      . ~/.espressif/v6.1/esp-idf/export.sh\n'
    exit 1
fi

# Every project here is built for `linux`, and idf.py refuses a target that
# contradicts IDF_TARGET rather than letting the command line win: "Target
# 'linux' specified on command line is not consistent with target 'esp32p4'
# in the environment". So an exported IDF_TARGET stops the suite dead, and it
# gets exported by ordinary things - espressif/esp-idf-ci-action sets it from
# its `target` input, and so does building for the board in the same shell.
#
# Unset rather than honoured: the target is not a choice this script offers.
unset IDF_TARGET

total_declared=0
total_ran=0
passed=()
failed=()

for entry in "${PROJECTS[@]}"; do
    proj="${entry%%:*}"
    elf="$proj/$BUILD/${entry##*:}.elf"

    # How many cases this project's sources declare. Compared with how many
    # actually ran, because a case file that nothing references is dropped
    # from the link and Unity reports OK over whatever survived - a suite that
    # silently shrinks still prints green. This is the only cheap defence
    # against that, and it has to be per project or the second binary becomes
    # the hole the first one's check was closing.
    declared=$(grep -rh --include='*.c' -c 'TEST_CASE(' "$proj/cases" \
               | awk '{ n += $1 } END { print n + 0 }')

    # A build directory remembers which ESP-IDF configured it - the path of
    # that IDF's Python environment is written into CMakeCache.txt - and every
    # idf.py afterwards refuses to touch it from a different one: "is
    # currently active in the environment while the project was configured
    # with". That is what an IDF version change looks like from in here, and
    # it is not an error worth reporting: the cached configuration is simply
    # worthless now. Drop it and let the rest of the loop rebuild.
    cache="$proj/$BUILD/CMakeCache.txt"
    if [ -f "$cache" ] && ! grep -q "^PYTHON:.*=$(command -v python)$" "$cache"; then
        rm -rf "${proj:?}/${BUILD:?}"
    fi

    # set-target rewrites sdkconfig and forces a full rebuild, so only do it
    # when the project is not already pointed at the host.
    if ! grep -q '^CONFIG_IDF_TARGET="linux"' "$proj/sdkconfig" 2>/dev/null; then
        if ! idf.py -C "$proj" -B "$proj/$BUILD" --preview set-target linux > "$proj/build.log" 2>&1; then
            printf '\033[31mFAIL\033[0m  %s: could not set the linux target\n\n' "$proj"
            tail -30 "$proj/build.log"
            failed+=("$proj: could not set the linux target")
            continue
        fi
    fi

    # The case and fake lists are globbed, and CONFIGURE_DEPENDS is
    # unavailable (IDF re-reads the component file in CMake script mode, where
    # the option is invalid). So an added or removed file would be silently
    # left out of an incremental build - exactly the kind of quiet shrinking
    # the count check above exists to catch. Watch the list and reconfigure
    # when it moves.
    stamp="$proj/$BUILD/.case-list"
    current=$( (ls "$proj"/cases/test_*.c "$proj"/main/fakes/fake_*.c 2>/dev/null) | sort)
    if [ ! -f "$stamp" ] || [ "$current" != "$(cat "$stamp" 2>/dev/null)" ]; then
        if ! idf.py -C "$proj" -B "$proj/$BUILD" reconfigure > "$proj/build.log" 2>&1; then
            printf '\033[31mFAIL\033[0m  %s: did not configure\n\n' "$proj"
            tail -30 "$proj/build.log"
            failed+=("$proj: did not configure")
            continue
        fi
        printf '%s' "$current" > "$stamp"
    fi

    if ! idf.py -C "$proj" -B "$proj/$BUILD" build > "$proj/build.log" 2>&1; then
        printf '\033[31mFAIL\033[0m  %s: did not build\n\n' "$proj"
        tail -30 "$proj/build.log"
        failed+=("$proj: did not build")
        continue
    fi

    printf -- '-- %s --\n' "$proj"
    out=$("$elf" < /dev/null 2>&1)
    status=$?
    echo "$out"

    # The failure count from Unity's own summary line, not from the exit
    # status: a process status is eight bits, and 256 failures would read 0.
    ran=$(echo "$out" | awk '/[0-9]+ Tests [0-9]+ Failures/ { print $1; exit }')
    bad=$(echo "$out" | awk '/[0-9]+ Tests [0-9]+ Failures/ { print $3; exit }')

    if [ "$status" -ne 0 ] || [ "${bad:-1}" != "0" ]; then
        printf '\n\033[31mFAIL\033[0m  %s: %s case(s) failed (exit %s)\n' \
               "$proj" "${bad:-?}" "$status"
        failed+=("$proj: ${bad:-?} case(s) failed")
        continue
    fi
    if [ "${ran:-0}" != "$declared" ]; then
        printf '\n\033[31mFAIL\033[0m  %s: %s cases declared, %s ran - the link dropped some\n' \
               "$proj" "$declared" "${ran:-0}"
        failed+=("$proj: $declared cases declared, ${ran:-0} ran")
        continue
    fi
    passed+=("$proj: $ran cases")

    total_declared=$((total_declared + declared))
    total_ran=$((total_ran + ran))
done

printf '\n'
for line in "${passed[@]+"${passed[@]}"}"; do
    printf '\033[32mok\033[0m    %s\n' "$line"
done
for line in "${failed[@]+"${failed[@]}"}"; do
    printf '\033[31mFAIL\033[0m  %s\n' "$line"
done
if [ "${#failed[@]}" -ne 0 ]; then
    printf '\n\033[31m%s of %s suites failed\033[0m\n' "${#failed[@]}" "${#PROJECTS[@]}"
    exit 1
fi
printf '\n\033[32mok\033[0m    %s cases across %s suites, all passing\n' \
       "$total_ran" "${#PROJECTS[@]}"
exit 0
