#!/usr/bin/env bash
#
# Moves the SDK to one version for a release: every component's own
# `version: "X"`, every kodediy/ dependency's `"==X"` in the manifests, and
# the tag the examples and the README pin (`version: vX`). tools/check_example_pins.sh checks that
# they agree; the release workflow checks that they equal the tag.
#
#   usage: tools/set_version.sh <X.Y.Z>
#   exit:  0 done, 2 not a version
#
set -euo pipefail
cd "$(dirname "$0")/.."

v="${1:-}"
if ! [[ "$v" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "usage: tools/set_version.sh <X.Y.Z>" >&2
    exit 2
fi

for f in components/*/idf_component.yml examples/*/main/idf_component.yml README.md; do
    sed -i.bak -E \
        -e "s/^version: \"[0-9.]+\"/version: \"$v\"/" \
        -e "/kodediy\/[a-z_]+:/ s/\"==[0-9.]+\"/\"==$v\"/" \
        -e "s/^( +version: )v[0-9.]+$/\1v$v/" \
        "$f"
    rm -f "$f.bak"
done
tools/check_example_pins.sh
