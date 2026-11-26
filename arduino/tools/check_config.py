#!/usr/bin/env python3
"""The Arduino libraries' configuration against an SDK app's: every difference is listed, with its reason.

    arduino/tools/check_config.py APP_SDKCONFIG BUILDER_SDKCONFIG KEEP_LIST

APP_SDKCONFIG is the sdkconfig of the target build's variant `arduino`, an
SDK app with the SDK's lines for Arduino's core; BUILDER_SDKCONFIG is the
library builder's, configured by arduino/tools/build_libs.sh --configure-only.

Every option both files name is compared, set or not ("# CONFIG_X is not
set" reads as CONFIG_X=n); an option only one names is counted, not compared,
nor are ESP-IDF's deprecated aliases at the end of a file, which repeat the
options above them. An option of the SDK's own (SDK_PREFIXES) that the app's
file names and the builder's does not fails: a whole SDK component missing
from the builder's project shows as its options missing.

KEEP_LIST (arduino/sdkconfig.keep) holds the options allowed to differ:
`#` comments, blank lines, and one `CONFIG_NAME reason` a line.

One line on stdout for each option that differs and is not in the list, each
SDK option the builder lacks, each line of the list whose option no longer
differs, each line with no reason and each option the list names twice; then
one line with the counts. Exit 1 if there is any problem, 0 otherwise.
"""
import argparse
import re
import sys
from pathlib import Path

SET = re.compile(r"^(CONFIG_[A-Za-z0-9_]+)=(.*)$")
NOT_SET = re.compile(r"^# (CONFIG_[A-Za-z0-9_]+) is not set$")
KEEP = re.compile(r"^(CONFIG_[A-Za-z0-9_]+)(?:\s+(.*))?$")
DEPRECATED = "# Deprecated options for backward compatibility"

# The prefixes of the SDK's own Kconfig options, from components/*/Kconfig.
SDK_PREFIXES = (
    "CONFIG_KBSP_",
    "CONFIG_KDRV_",
    "CONFIG_KHAL_",
    "CONFIG_KLOG_",
    "CONFIG_KODE_",
    "CONFIG_KODEOS_",
    "CONFIG_KQP_",
    "CONFIG_KSCREEN_",
    "CONFIG_KUI_",
)


def options(text):
    """{name: value} of an sdkconfig's text, up to ESP-IDF's deprecated aliases."""
    found = {}
    for line in text.splitlines():
        line = line.strip()
        if line == DEPRECATED:
            break
        match = SET.match(line)
        if match:
            found[match.group(1)] = match.group(2)
            continue
        match = NOT_SET.match(line)
        if match:
            found[match.group(1)] = "n"
    return found


def keep_list(text):
    """([(name, reason)], [bad lines]) of the list's text; a bad line has no option or no reason."""
    kept = []
    bad = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        match = KEEP.match(line)
        if match and match.group(2):
            kept.append((match.group(1), match.group(2)))
        else:
            bad.append(line)
    return kept, bad


def problems(app, builder, kept, bad):
    """The lines to print: differences not listed, SDK options the builder lacks, stale and bad list lines, names listed twice."""
    listed = {name for name, _ in kept}
    out = []
    for name in sorted(app.keys() & builder.keys()):
        if app[name] != builder[name] and name not in listed:
            out.append(f"{name}: app {app[name]}, builder {builder[name]}, not in the list")
    for name in sorted(app.keys() - builder.keys()):
        if name.startswith(SDK_PREFIXES):
            out.append(f"{name}: an SDK option the app names and the builder does not")
    for name, _ in kept:
        if name in app and name in builder and app[name] != builder[name]:
            continue
        out.append(f"{name}: in the list and no longer differs: stale")
    for line in bad:
        out.append(f"'{line}': a line of the list needs an option and its reason")
    seen = set()
    for name, _ in kept:
        if name in seen:
            out.append(f"{name}: the list names it twice")
        seen.add(name)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("app", type=Path, help="the SDK app's sdkconfig")
    ap.add_argument("builder", type=Path, help="the library builder's sdkconfig")
    ap.add_argument("keep", type=Path, help="the options allowed to differ")
    args = ap.parse_args()

    app = options(args.app.read_text())
    builder = options(args.builder.read_text())
    kept, bad = keep_list(args.keep.read_text())
    found = problems(app, builder, kept, bad)
    for line in found:
        print(line)
    both = len(app.keys() & builder.keys())
    one = len(app.keys() ^ builder.keys())
    print(f"check_config: {both} options compared, {one} options only one file names, "
          f"{len(kept)} lines in the list, {len(found)} problems")
    if found:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
