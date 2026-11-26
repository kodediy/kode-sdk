#!/usr/bin/env bash
#
# The Kode Dot's Arduino libraries: Espressif's library builder run for the
# ESP32-P4 alone, with this SDK among its components.
#
# What it does, in order:
#   1. Clones, into WORK_DIR, the library builder at LIB_BUILDER, Espressif's
#      Arduino core at the tag ARDUINO_CORE as the builder's
#      components/arduino, and TinyUSB at TINYUSB where the builder looks for
#      it (arduino/pins.env). A clone already at its pin is used as it is; one
#      at another pin stops the run.
#   2. Names kode and kode_screen in the builder's main manifest, by path, so
#      every SDK component is compiled and lands on the link line the builder
#      copies its libraries from.
#   3. Lets the Arduino core take the esp-hosted release kdrv pins: the C5's
#      firmware speaks that one.
#   4. Makes the builder's export script pass over an include directory it
#      never finishes on: the folder that holds LVGL, which LVGL names itself.
#   5. Writes WORK_DIR/sdkconfig.paths, the Dot's partition table by its
#      absolute path, and configures the builder's project with, later files
#      winning: the builder's common and ESP32-P4 defaults with DIO flash at
#      80 MHz (the Kode Dot entry of boards.txt), kbsp's sdkconfig.board,
#      arduino/sdkconfig.sdk, arduino/sdkconfig.builder, sdkconfig.paths.
#   6. Builds the builder's `idf-libs` target, then `copy-bootloader`: the
#      core's build recipe asks for that file.
#   7. Makes the libraries relocatable. ESP-IDF compiles and links with a
#      copy of the toolchain's picolibc.specs in its build folder, and the
#      export names that copy by its absolute path in four flags files; the
#      step names the toolchain's own file instead, which differs only by the
#      --gc-sections that ld_flags passes anyway. It then stops the run when
#      the text the platform reads at compile or link time names a path of
#      this machine: flags/ any absolute path, the linker scripts outside
#      their comments and the headers the work folder, the SDK or ESP-IDF.
#      sdkconfig and sdkconfig.h, which name the partition table by its path,
#      are the configuration and are not read for it.
#
# Steps 2 to 4 each stop the run when the file they edit has changed so that
# the edit no longer applies.
#
#   usage:  arduino/tools/build_libs.sh [--configure-only] WORK_DIR
#           arduino/tools/build_libs.sh --relocate DIR
#           --configure-only stops after step 5, leaving
#           WORK_DIR/lib-builder/sdkconfig
#           --relocate runs step 7 alone on libraries built already, DIR
#           being the folder that holds flags/
#   output: WORK_DIR/lib-builder/out/tools/esp32-arduino-libs/esp32p4
#   exit:   0 the libraries (or the configuration) are there, 1 a step failed
#
# Needs ESP-IDF exported at the version pins.env names
# (. ~/.espressif/v6.1/esp-idf/export.sh), git, perl, and on macOS the GNU
# tools the builder's scripts call by name: gsed, gawk and grealpath.
# --relocate needs perl alone.
set -euo pipefail

usage="usage: arduino/tools/build_libs.sh [--configure-only] WORK_DIR | --relocate DIR"
configure_only=0
relocate_only=0
if [ "${1:-}" = "--configure-only" ]; then
    configure_only=1
    shift
elif [ "${1:-}" = "--relocate" ]; then
    relocate_only=1
    shift
fi
if [ "$#" -ne 1 ] || [ -z "$1" ]; then
    echo "$usage" >&2
    exit 1
fi

sdk="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# The pins: ARDUINO_CORE, LIB_BUILDER, TINYUSB, ESP_IDF.
. "$sdk/arduino/pins.env"
target="esp32p4"

fail() {
    echo "build_libs: $*" >&2
    exit 1
}

# 7. The libraries in $1 made relocatable, then checked against the roots
# that follow it, ESP-IDF's two folders added when they are exported.
relocate() {
    local libs="$1"
    shift
    local roots file
    if [ ! -d "$libs/flags" ]; then
        fail "$libs has no flags/: is it the ESP32-P4 libraries?"
    fi
    for file in c_flags cpp_flags S_flags ld_flags; do
        if [ ! -f "$libs/flags/$file" ]; then
            fail "$libs/flags/$file is not there"
        fi
        perl -pi -e 's{"?(-specs=)[^"\s]*/(picolibc\.specs)"?}{$1$2}g' "$libs/flags/$file"
    done
    # An absolute path starts a word, follows = or , (-specs=, -Wl,), or
    # follows a one-letter option (-I, -L, -T).
    if ! perl -ne 'while (m{(?:^|[\s"=,]|(?<![\w-])-[A-Za-z])(/[^\s"]+)}g) { print STDERR "build_libs: $ARGV names $1\n"; $bad = 1 } END { exit $bad }' "$libs"/flags/*; then
        fail "the flags name a path of this machine: the libraries work on no other"
    fi
    roots="$(printf '%s\n' "$@" "${IDF_PATH:-}" "${IDF_TOOLS_PATH:-}")"
    if ! find "$libs" -type f \( -path "$libs/flags/*" -o -path "$libs/ld/*" -o -path "*/include/*" \) \
            ! -name sdkconfig.h -print0 \
        | ROOTS="$roots" xargs -0 perl -0777 -ne 'BEGIN { @roots = grep { length } split /\n/, $ENV{ROOTS} } s{/\*.*?\*/}{}gs; for $root (@roots) { if (index($_, $root) >= 0) { print STDERR "build_libs: $ARGV names $root\n"; $bad = 1 } } END { exit $bad }'; then
        fail "the libraries name a folder of this machine: they work on no other"
    fi
    echo "build_libs: $libs is relocatable"
}

if [ "$relocate_only" -eq 1 ]; then
    if [ ! -d "$1" ]; then
        fail "$1 is not a folder"
    fi
    libs="$(cd "$1" && pwd)"
    relocate "$libs" "$sdk" "$libs"
    exit 0
fi

if [ -z "${IDF_PATH:-}" ]; then
    fail "export ESP-IDF $ESP_IDF first"
fi
idf_version="$(idf.py --version)"
case "$idf_version" in
    "ESP-IDF $ESP_IDF" | "ESP-IDF $ESP_IDF-"*) ;;
    *) fail "$idf_version is exported, pins.env names $ESP_IDF" ;;
esac
if [ "$(uname -s)" = "Darwin" ]; then
    missing=""
    for tool in gsed gawk grealpath; do
        if ! command -v "$tool" >/dev/null; then
            missing="$missing $tool"
        fi
    done
    if [ -n "$missing" ]; then
        fail "the library builder's scripts call GNU tools by name; missing:$missing"
    fi
fi

mkdir -p "$1"
work="$(cd "$1" && pwd)"
builder="$work/lib-builder"
core="$builder/components/arduino"
tinyusb="$builder/components/arduino_tinyusb/tinyusb"
manifest="$builder/main/idf_component.yml"
core_manifest="$core/idf_component.yml"
export_script="$builder/tools/copy-libs.sh"

# 1. The clones. A commit is cloned without its history's files, a tag at
# depth 1; a clone already there must be at its pin.
clone_commit() {
    local url="$1" dir="$2" pin="$3"
    if [ ! -d "$dir/.git" ]; then
        git clone --quiet --filter=blob:none --no-checkout "$url" "$dir"
        git -C "$dir" checkout --quiet "$pin"
    fi
    case "$(git -C "$dir" rev-parse HEAD)" in
        "$pin"*) ;;
        *) fail "$dir is not at $pin" ;;
    esac
}

clone_tag() {
    local url="$1" dir="$2" tag="$3"
    if [ ! -d "$dir/.git" ]; then
        git clone --quiet --depth 1 --branch "$tag" "$url" "$dir"
    fi
    if [ "$(git -C "$dir" rev-parse HEAD)" != "$(git -C "$dir" rev-parse "$tag^{commit}" 2>/dev/null)" ]; then
        fail "$dir is not at the tag $tag"
    fi
}

clone_commit https://github.com/espressif/esp32-arduino-lib-builder.git "$builder" "$LIB_BUILDER"
clone_tag https://github.com/espressif/arduino-esp32.git "$core" "$ARDUINO_CORE"
clone_commit https://github.com/hathach/tinyusb.git "$tinyusb" "$TINYUSB"

# 2. The SDK, by path.
if ! grep -q 'kodediy/kode:' "$manifest"; then
    # The lines go at the end, so dependencies: must be the last top-level key.
    if [ "$(grep -E '^[^[:space:]#]' "$manifest" | tail -n 1)" != "dependencies:" ]; then
        fail "$manifest does not end with its dependencies: the SDK cannot be named"
    fi
    cat >> "$manifest" <<MANIFEST
  kodediy/kode:
    override_path: "$sdk/components/kode"
  kodediy/kode_screen:
    override_path: "$sdk/components/kode_screen"
MANIFEST
fi
for component in kode kode_screen; do
    if ! grep -A 1 "^  kodediy/$component:$" "$manifest" | grep -q "override_path: \"$sdk/components/$component\"$"; then
        fail "$manifest does not name $component in $sdk"
    fi
done

# 3. kdrv's esp-hosted release satisfies the core.
hosted="$(sed -n 's/^ *esp_hosted: *"==\(.*\)"$/\1/p' "$sdk/components/kdrv/idf_component.yml")"
if [ -z "$hosted" ]; then
    fail "kdrv's manifest does not pin esp_hosted"
fi
perl -0pi -e "s/(espressif\/esp_hosted:\n\s+version: )\"[^\"]*\"/\$1\">=$hosted\"/" "$core_manifest"
if ! perl -0ne "exit(!/espressif\/esp_hosted:\n\s+version: \">=\Q$hosted\E\"/)" "$core_manifest"; then
    fail "$core_manifest has changed: its esp_hosted requirement did not take >=$hosted"
fi

# 4. The walk that names a component's folder stops at `components`,
# `managed_components` or `build`; a directory that is one of those has none
# above it.
if ! grep -q 'LVGL names the folder' "$export_script"; then
    perl -0pi -e 's/(\t\twhile \[\[ "\$dname" != "components")/\t\t# LVGL names the folder that holds it as an include directory: there is no\n\t\t# component above it to file its headers under, and the walk below never ends.\n\t\tif [[ "\$fname" == "components" || "\$fname" == "managed_components" ]]; then\n\t\t\tcontinue\n\t\tfi\n$1/' "$export_script"
fi
if ! grep -q 'LVGL names the folder' "$export_script"; then
    fail "$export_script has changed: the guard did not go in"
fi

# 5. The configuration.
cat > "$work/sdkconfig.paths" <<PATHS
# The Kode Dot's partition table, kbsp's, by its path on this machine. (ESP-IDF)
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="$sdk/components/kbsp/partitions.csv"
PATHS

configs="configs/defconfig.common"
configs="$configs;configs/defconfig.$target"
configs="$configs;configs/defconfig.debug_default"
configs="$configs;configs/defconfig.qio_ram"
configs="$configs;configs/defconfig.esp_sr"
configs="$configs;configs/defconfig.dio"
configs="$configs;configs/defconfig.80m_200m"
configs="$configs;$sdk/components/kbsp/sdkconfig.board"
configs="$configs;$sdk/arduino/sdkconfig.sdk"
configs="$configs;$sdk/arduino/sdkconfig.builder"
configs="$configs;$work/sdkconfig.paths"

export CHIP_VARIANT="$target"
export IDF_COMPONENT_OVERWRITE_MANAGED_COMPONENTS=1
export IDF_CCACHE_ENABLE=0

cd "$builder"
rm -rf build sdkconfig
idf.py -DIDF_TARGET="$target" -DSDKCONFIG_DEFAULTS="$configs" reconfigure
if [ "$configure_only" -eq 1 ]; then
    echo "build_libs: $builder/sdkconfig"
    exit 0
fi

# 6. The build.
idf.py idf-libs
idf.py copy-bootloader

# 7. Relocatable.
relocate "$builder/out/tools/esp32-arduino-libs/$target" "$work" "$sdk"
echo "build_libs: $builder/out/tools/esp32-arduino-libs/$target"
