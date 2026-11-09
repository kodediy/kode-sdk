#!/bin/sh
# The kit's faces as C bitmap fonts, with lv_font_conv (MIT): eight in Outfit
# whose digits are DM Sans, the six digits-only faces in DM Sans Bold that
# carry them, and four faces that are DM Sans from end to end: the pairing
# code, the clock, and the two big figures (the code of the system screens and
# the hero percentage). The sizes 28, 34, 40, 60 and 80 are the scale of
# kshell's look (kode-dot-os), 22 and 26 the kit's own small ones.
#
#   tools/fontgen.sh                              writes fonts/src/kui_font_*.c
#   FONT_CONV="<command>" tools/fontgen.sh        another lv_font_conv, e.g.
#                                                 "node <checkout>/lv_font_conv.js"
#   OUT=/some/dir tools/fontgen.sh                somewhere else (a byte comparison)
#
# The faces in fonts/src/ are this script's output with lv_font_conv at commit
# 2107ab79b615a6ff7051796fa9f7ae52f042437b, byte for byte. The npm releases up
# to 1.5.3 do not emit the cap_height, x_height and static_bitmap fields these
# faces carry, so the default fetches that commit from GitHub through npx (the
# first run builds it, and takes a few minutes).
#
# Runs from the kit's own root, so lv_font_conv's Opts: line in each face names
# no local absolute path.
#
# 4 bpp and uncompressed, as the glass has always drawn them. Each face carries
# only the characters something sets in it: caption, body, name and title are
# Latin-1 (an app's name or description may carry an accent), the 40 px
# display face too, and the 60 px one is ASCII; font_code is the pairing passkey
# and the arrows that stand for a typed symbol, and a 64 px face cut to Latin-1
# would be flash spent on glyphs no passkey draws; font_clock is the time on
# kodeOS's face, ten digits and the colon, and the hyphen it reads --:-- with
# while the time is not known.
#
# The brand sets numbers in DM Sans and the words around them in Outfit. Every
# Outfit face leaves out 0x30-0x39 and names the digits face of its own size
# as its fallback (lv_font_conv's --lv-fallback), so LVGL draws each digit
# from DM Sans Bold on the Outfit baseline and nothing has to remember to ask
# for it. The signs between digits (: . % and the degree) stay in Outfit.
# Two faces of one size share a digits face. The 80 and 160 px faces are DM
# Sans from end to end and need none: font_code_80 holds the signs a code or a
# percentage is written with, font_hero_160 only the figures and the percent.
set -eu
FONT_CONV=${FONT_CONV:-"npx --yes github:lvgl/lv_font_conv#2107ab79b615a6ff7051796fa9f7ae52f042437b"}
KIT=$(cd "$(dirname "$0")/.." && pwd)
cd "$KIT"
OUT_REL=${OUT:-"fonts/src"}
mkdir -p "$OUT_REL"

# face <symbol> <ttf> <size> <range> [<fallback symbol>]
face() {
    $FONT_CONV --font "fonts/assets/$2" -o "$OUT_REL/$1.c" --size "$3" --bpp 4 \
               --format lvgl --no-compress --range "$4" ${5:+--lv-fallback $5}
    # The face is the font's: its licence line on top.
    { echo '/* SPDX-License-Identifier: OFL-1.1 */'; cat "$OUT_REL/$1.c"; } > "$OUT_REL/$1.c.tmp"
    mv "$OUT_REL/$1.c.tmp" "$OUT_REL/$1.c"
}

# ASCII without the ten digits, which the digits faces draw.
TEXT=0x20-0x2F,0x3A-0x7F
# The same with Latin-1 (0xA0-0xFF, which holds the middle dot and the degree).
LATIN1=0x20-0x2F,0x3A-0x7F,0xA0-0xFF

face kui_font_digits_22 DMSans-Bold.ttf      22  0x30-0x39
face kui_font_digits_26 DMSans-Bold.ttf      26  0x30-0x39
face kui_font_digits_28 DMSans-Bold.ttf      28  0x30-0x39
face kui_font_digits_34 DMSans-Bold.ttf      34  0x30-0x39
face kui_font_digits_40 DMSans-Bold.ttf      40  0x30-0x39
face kui_font_digits_60 DMSans-Bold.ttf      60  0x30-0x39

face kui_font_caption   Outfit-Regular.ttf   22  $LATIN1,0x2190-0x2193          kui_font_digits_22
face kui_font_body      Outfit-Regular.ttf   28  $LATIN1                        kui_font_digits_28
face kui_font_button    Outfit-Bold.ttf      26  $TEXT                          kui_font_digits_26
face kui_font_name      Outfit-Bold.ttf      28  $LATIN1                        kui_font_digits_28
face kui_font_title     Outfit-Bold.ttf      34  $LATIN1                        kui_font_digits_34
face kui_font_heading   Outfit-ExtraBold.ttf 34  $TEXT                          kui_font_digits_34
face kui_font_display_40 Outfit-ExtraBold.ttf 40 $LATIN1                        kui_font_digits_40
face kui_font_title_60  Outfit-ExtraBold.ttf 60  $TEXT                          kui_font_digits_60

face kui_font_code      DMSans-Bold.ttf      64  0x20,0x30-0x39,0x2190-0x2193
face kui_font_clock     DMSans-Bold.ttf      104 0x2D,0x30-0x3A
face kui_font_code_80   DMSans-Bold.ttf      80  0x20,0x25,0x2B,0x2D-0x3A
face kui_font_hero_160  DMSans-Bold.ttf      160 0x20,0x25,0x30-0x39
