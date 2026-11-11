#!/usr/bin/env python3
"""The kit's line glyphs as white-on-transparent PNGs, and their header.

    tools/glyphgen.py           write images/assets/glyph_<name>_<size>.png and core/include/kui_glyphs.h
    tools/glyphgen.py --check   exit 1 when a PNG or the header on disk is not what this script draws

This script adapts kshell's tools/gen_glyphs.py:
the SVG bodies, the sun, the drawing and the table of sizes are its own; what
changes is the output (PNG files for the kit's imagegen flow and a header,
where kshell writes C files).

Each glyph is an SVG body on a 24 x 24 grid, 2.2 px round strokes, drawn at
the size the screen sets it. The alpha of each picture is the glyph:
tools/imagegen.sh turns the PNGs into A8 images (images/src/kui_glyph_<name>_
<size>.c), which a screen recolours as it draws them, the way kshell does.

Needs cairosvg and Pillow. cairosvg loads libcairo by name: on macOS with
Homebrew's cairo, run it as

    DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib python3 tools/glyphgen.py

The PNGs and the header are checked in, so building never needs any of this.
"""
import argparse
import io
import math
import sys
from pathlib import Path

import cairosvg
from PIL import Image

KIT = Path(__file__).resolve().parent.parent
ASSETS = KIT / "images" / "assets"
HEADER = KIT / "core" / "include" / "kui_glyphs.h"

# name -> (what it stands for, SVG body)
G = {
    "wifi": ("Wi-Fi", '<path d="M2.5 9a14 14 0 0 1 19 0M5.8 12.6a9 9 0 0 1 12.4 0M9.2 16.1a4 4 0 0 1 5.6 0"/><circle cx="12" cy="19.3" r="1.3" fill="currentColor" stroke="none"/>'),
    "bt": ("Bluetooth", '<path d="M7 7l10 10-5 4V3l5 4L7 17"/>'),
    "torch": ("A torch", '<path d="M8 3h8v4l-2 3v10.5a2 2 0 0 1-4 0V10L8 7z"/><path d="M12 13v2.5"/>'),
    "cup": ("A cup: staying awake", '<path d="M5 9h11v5a5 5 0 0 1-5 5h-1a5 5 0 0 1-5-5z"/><path d="M16 11h1.5a2.5 2.5 0 0 1 0 5H16"/><path d="M8.5 3.5c0 1.2 1 1.4 1 2.6M12 3.5c0 1.2 1 1.4 1 2.6"/>'),
    "power": ("Power off", '<path d="M12 3v8"/><path d="M6.3 6.8a8 8 0 1 0 11.4 0"/>'),
    "more": ("A cog: more settings", '<path d="M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z"/><circle cx="12" cy="12" r="3"/>'),
    "spk0": ("The speaker, muted", '<path d="M4 9.5v5h3.5L12 18.5v-13L7.5 9.5z" fill="currentColor"/><path d="M16 9.5l5 5M21 9.5l-5 5"/>'),
    "vib": ("Vibration", '<rect x="8" y="4" width="8" height="16" rx="2"/><path d="M4.5 9v6M19.5 9v6"/>'),
    "spk1": ("The speaker, one wave", '<path d="M4 9.5v5h3.5L12 18.5v-13L7.5 9.5z" fill="currentColor"/><path d="M15.5 10a3 3 0 0 1 0 4"/>'),
    "spk2": ("The speaker, two waves", '<path d="M4 9.5v5h3.5L12 18.5v-13L7.5 9.5z" fill="currentColor"/><path d="M15.5 10a3 3 0 0 1 0 4"/><path d="M18 7.8a6.5 6.5 0 0 1 0 8.4"/>'),
    "spk3": ("The speaker, three waves", '<path d="M4 9.5v5h3.5L12 18.5v-13L7.5 9.5z" fill="currentColor"/><path d="M15.5 10a3 3 0 0 1 0 4"/><path d="M18 7.8a6.5 6.5 0 0 1 0 8.4"/><path d="M20.4 5.6a10 10 0 0 1 0 12.8"/>'),
    "clock": ("A clock", '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>'),
    "moon": ("A moon: rest", '<path d="M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5z"/>'),
    "watch": ("A watch", '<rect x="6" y="5" width="12" height="14" rx="4"/><path d="M9 5l.5-2.5h5L15 5M9 19l.5 2.5h5L15 19"/>'),
    "scene": ("A landscape: Kody's scene", '<path d="M3 18.5l5.5-7 4 4.5 3-3 5.5 5.5z"/><circle cx="16.5" cy="7" r="2"/>'),
    "chip": ("A chip: the board", '<rect x="6.5" y="6.5" width="11" height="11" rx="2"/><path d="M10 3v3.5M14 3v3.5M10 17.5V21M14 17.5V21M3 10h3.5M3 14h3.5M17.5 10H21M17.5 14H21"/>'),
    "info": ("About", '<circle cx="12" cy="12" r="9"/><path d="M12 11v6M12 7.5v.2"/>'),
    "lock": ("A padlock", '<rect x="5.5" y="11" width="13" height="9.5" rx="2"/><path d="M8.5 11V8a3.5 3.5 0 0 1 7 0v3"/>'),
    "check": ("A tick", '<path d="M5 12.5l4.5 4.5L19 7.5"/>'),
    "sd": ("The microSD card", '<path d="M7 3h7l4 4v14H7z" fill="currentColor" stroke="none"/>'),
    "phone": ("A phone", '<rect x="7" y="2.5" width="10" height="19" rx="2.5"/><path d="M11 18.5h2"/>'),
    "x": ("A cross", '<path d="M6 6l12 12M18 6L6 18"/>'),
    # The kit's own drawing, not kshell's: kshell has no game glyph.
    "games": ("A gamepad: the Games folder", '<path d="M7 6.5h10a4.5 4.5 0 0 1 4.4 3.6l1 5.2a2.6 2.6 0 0 1-4.5 2.2L16 15.5H8l-1.9 2a2.6 2.6 0 0 1-4.5-2.2l1-5.2A4.5 4.5 0 0 1 7 6.5z"/><path d="M7.5 9.5v4M5.5 11.5h4"/><circle cx="15.6" cy="10.4" r="1.2" fill="currentColor" stroke="none"/><circle cx="17.6" cy="12.8" r="1.2" fill="currentColor" stroke="none"/>'),
    # The five folders of the marketplace that had no glyph. The kit's own drawings.
    "tool": ("A wrench: the Tools folder", '<path d="M14.8 4.2a4.6 4.6 0 0 0-4.3 6.2L4.3 16.6a2.2 2.2 0 0 0 3.1 3.1l6.2-6.2a4.6 4.6 0 0 0 6.2-4.3l-3.1 3.1-2.6-.6-.6-2.6 3.1-3.1a4.6 4.6 0 0 0-1.8-.4z"/>'),
    "talk": ("A speech bubble: the Communication folder", '<path d="M6 4.5h12a2.5 2.5 0 0 1 2.5 2.5v7a2.5 2.5 0 0 1-2.5 2.5h-6.5L7 20v-3.5H6A2.5 2.5 0 0 1 3.5 14V7A2.5 2.5 0 0 1 6 4.5z"/><path d="M8 9h8M8 12.2h5"/>'),
    "brush": ("A pencil: the Creative folder", '<path d="M4.5 19.5l.9-4.2L16 4.7a2.1 2.1 0 0 1 3 0l.3.3a2.1 2.1 0 0 1 0 3L8.7 18.6z"/><path d="M13.8 6.9l3.3 3.3"/>'),
    "maker": ("A chip with its core: the Maker folder", '<rect x="6.5" y="6.5" width="11" height="11" rx="2"/><rect x="10" y="10" width="4" height="4" rx=".8"/><path d="M10 3v3.5M14 3v3.5M10 17.5V21M14 17.5V21M3 10h3.5M3 14h3.5M17.5 10H21M17.5 14H21"/>'),
    "spark": ("Two sparkles: the AI folder", '<path d="M10 6.5Q11 12.5 17 13.5Q11 14.5 10 20.5Q9 14.5 3 13.5Q9 12.5 10 6.5z"/><path d="M18 2.8Q18.4 5.6 21.2 6Q18.4 6.4 18 9.2Q17.6 6.4 14.8 6Q17.6 5.6 18 2.8z"/>'),
    "chev_l": ("A chevron, left", '<path d="M14.5 5.5L8 12l6.5 6.5"/>'),
    "chev_r": ("A chevron, right", '<path d="M9.5 5.5L16 12l-6.5 6.5"/>'),
    "share": ("Share: the tray with the arrow leaving it", '<path d="M12 3.5v10.5"/><path d="M8 7.5l4-4 4 4"/><path d="M8 10.5H7a2 2 0 0 0-2 2v6.5a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2v-6.5a2 2 0 0 0-2-2h-1"/>'),
    "apps": ("An app with no icon: four rounded squares", '<rect x="4" y="4" width="6.5" height="6.5" rx="1.8"/><rect x="13.5" y="4" width="6.5" height="6.5" rx="1.8"/><rect x="4" y="13.5" width="6.5" height="6.5" rx="1.8"/><rect x="13.5" y="13.5" width="6.5" height="6.5" rx="1.8"/>'),
}


def sun(level):
    """The sun: the disc and the rays grow with the level, 0 to 1."""
    r = 3.2 + level * 1.4
    a = 1.6 + level * 2.2
    b = r + 2
    body = f'<circle cx="12" cy="12" r="{r:.1f}" fill="currentColor"/>'
    for i in range(8):
        t = i * math.pi / 4
        c, s = math.cos(t), math.sin(t)
        body += f'<path d="M{12 + c * b:.1f} {12 + s * b:.1f}L{12 + c * (b + a):.1f} {12 + s * (b + a):.1f}"/>'
    return body


for _i, _v in enumerate((0.0, 0.33, 0.66, 1.0)):
    G[f"sun{_i}"] = (f"Brightness, step {_i} of 3", sun(_v))

# size -> the glyphs drawn at it
SIZES = {
    16: ["sd"],
    28: ["cup"],
    30: ["clock", "wifi", "bt", "moon", "watch", "scene", "chip", "info", "lock", "check", "phone", "x",
         "power", "apps", "share", "chev_l", "chev_r"],
    40: ["wifi", "bt", "torch", "share", "cup", "power", "more", "spk0", "vib", "spk1", "spk2", "spk3",
         "sun0", "sun1", "sun2", "sun3", "games", "tool", "talk", "brush", "maker", "spark"],
    64: ["torch", "apps"],
    # A screen's protagonist, drawn at its own size: scaled up from 40 it comes out soft.
    96: ["bt", "wifi", "chip", "power", "share"],
}


def render(body, size):
    """The glyph as a white RGBA picture whose alpha is the drawing."""
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="{size}" height="{size}" '
           f'color="#fff"><g fill="none" stroke="#fff" stroke-width="2.2" stroke-linecap="round" '
           f'stroke-linejoin="round">{body.replace("currentColor", "#fff")}</g></svg>')
    png = cairosvg.svg2png(bytestring=svg.encode(), output_width=size, output_height=size)
    alpha = Image.open(io.BytesIO(png)).getchannel("A")
    out = Image.new("RGBA", alpha.size, (255, 255, 255, 0))
    out.putalpha(alpha)
    return out


def png_bytes(image):
    buf = io.BytesIO()
    image.save(buf, format="PNG", optimize=False)
    return buf.getvalue()


def header_text():
    lines = [
        "/* SPDX-License-Identifier: GPL-3.0-or-later */",
        "/**",
        " * @file kui_glyphs.h",
        " * @brief The kit's line glyphs: A8 images of one stroke, recoloured as they are drawn.",
        " *",
        " * Generated by tools/glyphgen.py. Do not edit.",
        " */",
        "#ifndef KUI_GLYPHS_H",
        "#define KUI_GLYPHS_H",
        "",
        '#include "lvgl.h"',
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
    ]
    for size, names in SIZES.items():
        lines += ["", f"/* {size} x {size}. */"]
        for n in names:
            lines.append(f"extern const lv_image_dsc_t kui_glyph_{n}_{size}; /**< {G[n][0]}. */")
    lines += ["", "#ifdef __cplusplus", "}", "#endif", "", "#endif /* KUI_GLYPHS_H */", ""]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true")
    check = ap.parse_args().check
    stale = []
    jobs = []
    for size, names in SIZES.items():
        for n in names:
            jobs.append((ASSETS / f"glyph_{n}_{size}.png", png_bytes(render(G[n][1], size))))
    jobs.append((HEADER, header_text().encode("utf-8")))
    for path, data in jobs:
        if check:
            if not path.exists() or path.read_bytes() != data:
                stale.append(path.relative_to(KIT))
        else:
            path.write_bytes(data)
    if check:
        for p in stale:
            print(f"{p} is stale: run tools/glyphgen.py")
        return 1 if stale else 0
    print(f"wrote {len(jobs) - 1} glyphs and {HEADER.relative_to(KIT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
