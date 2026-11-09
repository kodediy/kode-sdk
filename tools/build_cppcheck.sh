#!/usr/bin/env bash
#
# Builds the cppcheck a MISRA baseline was taken with, for a machine whose
# package manager carries another one.
#
# tools/check_misra.sh refuses to judge a baseline with any cppcheck but the
# one named on the baseline's first line, and a distribution's package is
# whatever version that distribution froze. This script reads the version
# from the baseline, fetches that release's source and installs it under a
# prefix. A prefix that already holds that version is left alone, so a CI
# cache of the prefix makes this a no-op.
#
# cppcheck compiles the path of its own data files into the binary, so the
# prefix is used where it was built: cache it and restore it at the same path.
#
#   usage: tools/build_cppcheck.sh <baseline> <prefix>
#   stdout: "cppcheck <version> is at <prefix>/bin/cppcheck"
#   exit:   0 the prefix holds that version, 1 it could not be built
#
# Needs curl, tar, cmake and a C++ compiler on the PATH.
set -uo pipefail

usage='usage: tools/build_cppcheck.sh <baseline> <prefix>'
baseline="${1:?$usage}"
prefix="${2:?$usage}"

if [ ! -f "$baseline" ]; then
    echo "error: missing required input: $baseline" >&2
    exit 1
fi
version="$(awk 'NR == 1 && $1 == "cppcheck" { print $2 }' "$baseline")"
case "$version" in
    ''|*[!0-9.]*)
        echo "error: $baseline does not start with \`cppcheck <version>\`" >&2
        exit 1
        ;;
esac

mkdir -p "$prefix" || exit 1
prefix="$(cd "$prefix" && pwd -P)"

if [ -x "$prefix/bin/cppcheck" ] \
   && [ "$("$prefix/bin/cppcheck" --version | awk '{ print $2 }')" = "$version" ]; then
    echo "cppcheck $version is at $prefix/bin/cppcheck"
    exit 0
fi

for tool in curl tar cmake c++; do
    if ! command -v "$tool" > /dev/null 2>&1; then
        echo "error: $tool is not on the PATH" >&2
        exit 1
    fi
done

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

url="https://github.com/danmar/cppcheck/archive/refs/tags/$version.tar.gz"
if ! curl -fsSL "$url" | tar -xz -C "$work"; then
    echo "error: could not fetch $url" >&2
    exit 1
fi

src="$work/cppcheck-$version"
cmake -S "$src" -B "$work/build" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$prefix" > "$work/log.txt" 2>&1 \
    && cmake --build "$work/build" -j "$(getconf _NPROCESSORS_ONLN)" >> "$work/log.txt" 2>&1 \
    && cmake --install "$work/build" >> "$work/log.txt" 2>&1
if [ "$?" -ne 0 ]; then
    tail -n 40 "$work/log.txt" >&2
    echo "error: cppcheck $version did not build" >&2
    exit 1
fi

echo "cppcheck $version is at $prefix/bin/cppcheck"
