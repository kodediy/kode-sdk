#!/usr/bin/env bash
#
# The SDK version the components, the examples and the README carry, checked
# against the releases.
#
# A release is one version for every component: each manifest says it
# (`version: "X"`) and names its siblings at it exactly (`version: "==X"`).
# An example and the README pin the release's tag on GitHub (`version: vX`
# under `git:`). An example is compiled against the release its
# main/idf_component.yml pins, not against the tree it sits in, so its
# sources may only use what that release has. tools/set_version.sh moves them
# all; this makes a missed one something CI asks for rather than something a
# release remembers.
#
# Two rules:
#   1. Every one says the same version - the manifests, every example and the
#      README's pin, which is what someone copies into a project of their own.
#   2. That version is not older than the newest v* tag. A release commit
#      bumps it before its tag exists, so newer is allowed; older is the
#      release that forgot.
#
# Rule 2 needs the tags. Without any (a shallow clone), it says so and
# checks rule 1 only, rather than passing a comparison it could not make.
#
#   usage: tools/check_example_pins.sh
#   exit:  0 the versions agree and are current, 1 otherwise
#
set -uo pipefail
cd "$(dirname "$0")/.."

# Every component and every example that is an ESP-IDF project, found rather
# than listed, so a new one is checked the day it lands. The Arduino sketch
# has no pin.
FILES=( components/*/idf_component.yml examples/*/main/idf_component.yml README.md )

pins=""
for f in "${FILES[@]}"; do
    # A component's own `version: "X"`, every kodediy/ dependency's `"==X"`
    # and a git pin's tag, `version: vX`. The registry's other pins
    # (esp-hosted, LVGL) are not ours.
    v=$( { grep -Eo '^version: "[0-9.]+"' "$f"
           grep -Eo '^ +version: v[0-9.]+$' "$f"
           grep -E 'kodediy/[a-z_]+:' "$f" | grep -Eo '"==[0-9.]+"'; } \
         | grep -Eo '[0-9]+\.[0-9]+\.[0-9]+' | sort -u)
    if [ -z "$v" ]; then
        printf '\033[31mFAIL\033[0m  %s: no SDK version found\n' "$f"
        exit 1
    fi
    printf '      %-44s %s\n' "$f" "$(echo $v)"
    pins="$pins$v"$'\n'
done

distinct=$(printf '%s' "$pins" | sort -u)
if [ "$(printf '%s\n' "$distinct" | wc -l)" -ne 1 ]; then
    printf '\033[31mFAIL\033[0m  the versions disagree; one release, one version\n'
    exit 1
fi
pin="$distinct"

newest=$(git tag --list 'v*' | sort -V | tail -1)
newest="${newest#v}"
if [ -z "$newest" ]; then
    printf '\033[33mskip\033[0m  no tags in this clone: the versions agree (%s), but whether\n' "$pin"
    printf '      that is the newest release was not checked\n'
    exit 0
fi

if [ "$(printf '%s\n%s\n' "$pin" "$newest" | sort -V | head -1)" != "$newest" ]; then
    printf '\033[31mFAIL\033[0m  the SDK is at %s, and %s is out. An example builds\n' "$pin" "$newest"
    printf '      against its pin, not this tree - run tools/set_version.sh %s.\n' "$newest"
    exit 1
fi

printf '\033[32mok\033[0m    the SDK is at %s; newest release %s\n' "$pin" "$newest"
exit 0
