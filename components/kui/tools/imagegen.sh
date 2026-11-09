#!/bin/sh
# The kit's glyphs and wordmark as C images, with LVGL's own LVGLImage.py (MIT).
#
#   tools/imagegen.sh                  writes images/src/kui_*.c
#   LVGL_IMAGE=/path/LVGLImage.py      the script, when it is not in the usual place
#   PYTHON=/path/python3               an interpreter with pypng and lz4
#   OUT=/some/dir                      somewhere else (a byte comparison)
#
# The thirteen glyphs are ARGB8888 (recoloured and dimmed in place, so they need
# their alpha); the wordmark is RGB565A8, a 300 x 90 picture that alpha-blends.
# The line glyphs, glyph_<name>_<size>.png, are A8: tools/glyphgen.py draws
# their pictures, white on transparent, and a screen recolours each as it draws
# it (kui_glyphs.h).
set -eu
KIT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-"$KIT/images/src"}
PYTHON=${PYTHON:-python3}
LVGL_IMAGE=${LVGL_IMAGE:-"$KIT/../../../managed_components/lvgl__lvgl/scripts/LVGLImage.py"}
[ -f "$LVGL_IMAGE" ] || { echo "imagegen: no LVGLImage.py at $LVGL_IMAGE; set LVGL_IMAGE" >&2; exit 1; }
mkdir -p "$OUT"

for g in wifi ble sun battery power info check lock usb bolt sd radio x; do
    "$PYTHON" "$LVGL_IMAGE" --ofmt C --cf ARGB8888 -o "$OUT" --name "kui_glyph_$g" \
              "$KIT/images/assets/glyph_$g.png"
done
for f in "$KIT"/images/assets/glyph_*_[0-9]*.png; do
    name=$(basename "$f" .png)
    "$PYTHON" "$LVGL_IMAGE" --ofmt C --cf A8 -o "$OUT" --name "kui_$name" "$f"
done
"$PYTHON" "$LVGL_IMAGE" --ofmt C --cf RGB565A8 -o "$OUT" --name kui_wordmark \
          "$KIT/images/assets/wordmark.png"

# The licence line on top of each image.
for f in "$OUT"/kui_*.c; do
    { echo '/* SPDX-License-Identifier: GPL-3.0-or-later */'; cat "$f"; } > "$f.tmp" && mv "$f.tmp" "$f"
done
