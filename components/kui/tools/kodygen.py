#!/usr/bin/env python3
"""kody/assets/ into Kody's pack (kody/pack/), and the built-in scene (kody/src/, kody/include/kui_kody_anims.h).

    tools/kodygen.py           write the files
    tools/kodygen.py --check   regenerate into a temporary directory and exit 1
                               when anything differs from what is checked in
    tools/kodygen.py --lint    refuse what the board does not draw, print each
                               animation's cost, and write nothing

Each entry of kody/assets/kody.json names a Lottie file beside it and the
canvas it plays on. The script lints the source, reframes it onto that canvas,
replaces the colours the entry lists, minifies the result and deflates it.
Every entry becomes kody/pack/<name>.json.z, the zlib stream as it is, and the
manifest's `pack` block becomes kody/pack/kode.json: the official pack, which
kodeOS's root CMake copies into the factory's FAT image as /storage/kode/ and
which the server publishes. An entry marked "builtin" is also written as a byte
array in kody/src/kody_<name>.c with the descriptor kui_kody.h defines
(deflated), and kody/include/kui_kody_anims.h declares those alone: the scene a
firmware falls back on when no pack is usable. There is no table: a firmware
links the descriptors it names and no others.

The pack block is checked here as the firmware will check it (kui_pack.h):
scene names of 1-15 characters of [a-z0-9_], file names of at most 40
characters, every name an entry of this manifest, a scene's files on the
face's canvas and each role's on its own.

The reframe composes one more transform into each top-level layer: the layer
is scaled by S about the origin and moved by D, so its own position p and
scale s become D + S p and S s. That is exact for any uniform S, keyframed or
static, and adds nothing to the file.

Minifying drops the keys the player never reads, at every depth: an exporter's
`meta` block and the `mn` and `nm` names of the animation, its layers, shapes
and effects. They are gone from the generated files alone; the sources keep
them. Without this a Glaxnimate export packs 1.7 times larger than the
animation it was made from; with it, smaller. Everything else is the source as
it is: the JSON is parsed and written back with the same key order and the same
separators.

The lint refuses a source that holds what the ThorVG on the board ignores, and
exits 1 before it writes anything. It prints one line per finding, naming the
animation, the layer (its place in its list and its name, when it has one) and
the property, as a path from the layer:

    pucker & bloat and zig-zag shapes (`ty` "pb" and "zz")
    layer effects other than blur (`ef` entries whose `ty` is not 29)
    expressions (a string `x` on a property)
    image layers (`ty` 2), text layers (`ty` 5) and 3D (`ddd` 1)

The cost report is one line per animation: its shapes (paths, rectangles,
ellipses and polystars), its keyframes (those of every animated property) and
the depth its JSON nests to, which the board limits to 32. It is printed after
a write and by --lint, measured on the animation as the board gets it.

A data file is judged by what its stream inflates to, not by the stream's
bytes: two zlib builds can deflate the same JSON into different streams, and
both are right. So the script keeps the stream a file already holds while that
stream still inflates to the entry's JSON and is no more than 5 % longer than
the stream this script would write, and deflates anew when it is not or the
file is missing. The length is checked because the deflate is there to save
flash: a stream made at a low level, or stored, inflates to the right JSON
and takes several times the room. --check therefore fails on a file whose
stream is not the entry's JSON or is longer than that, or whose text around
the stream is not what the script writes, and on no other difference. Delete
a data file to have its stream made anew.

The generated files are checked in, so building never needs Python; run
--check after touching kody/assets/ so the two cannot drift.
"""
import argparse
import hashlib
import json
import re
import sys
import tempfile
import zlib
from pathlib import Path

KIT = Path(__file__).resolve().parent.parent
SRC = KIT / "kody" / "assets"
MANIFEST = SRC / "kody.json"
DATA_DIR = "kody/src"
PACK_DIR = "kody/pack"
PACK_MANIFEST = "kode.json"
HEADER = Path("kody") / "include" / "kui_kody_anims.h"
SCENE = re.compile(r"^[a-z0-9_]{1,15}$")
FILE_NAME_MAX = 40
FACE_CANVAS = (410, 334)
ROLE_CANVAS = {"empty": (410, 369), "writing": (410, 160), "goodbye": (410, 160), "loading": (264, 264)}
TRIGGERS = ("look", "hour", "notice", "plug", "joke")

# The keys the player never reads: an exporter's block, and the match name and the name of every
# animation, layer, shape and effect. Minifying drops them at every depth.
STRIPPED = ("meta", "mn", "nm")
# What ThorVG on the board does not draw. A layer's `ty` is a number, a shape's a string.
LAYER_REFUSED = {2: "an image layer", 5: "a text layer"}
SHAPE_REFUSED = {"pb": "a pucker & bloat", "zz": "a zig-zag"}
EFFECT_BLUR = 29   # the `ty` of the layer effect Gaussian Blur: the one effect the board draws
# The `ty` of the shapes that make a path, the ones the cost report counts.
SHAPE_PATHS = ("sh", "rc", "el", "sr")

HEX = re.compile(r"^#[0-9A-Fa-f]{6}$")
NAME = re.compile(r"^[a-z][a-z0-9_]*$")
GENERATED = ("/* SPDX-License-Identifier: GPL-3.0-or-later */\n"
             "/* Written by tools/kodygen.py from kody/assets/: change the sources there and run "
             "the script, never this file. */")
BYTES_PER_LINE = 16
DEFLATE_LEVEL = 9  # zlib's smallest stream; the time it costs is this script's, not the board's
SLACK_PCT = 5      # a held stream may be this much longer than a new one: room for another zlib build
STREAM = re.compile(r"^static const uint8_t s_zlib\[(\d+)\] = \{\n(.*?)\n\};$", re.M | re.S)


def fail(msg):
    sys.exit(f"kodygen: {msg}")


def num(v):
    """A computed coordinate, to the hundredth of a pixel, as the sources write theirs."""
    r = round(float(v), 2)
    if r == int(r):
        return int(r)
    return r


def load():
    data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    seen = set()
    for a in data["animations"]:
        name = a.get("name", "")
        if not NAME.match(name) or name in seen:
            fail(f"{name!r} is not a new lower_snake_case name")
        seen.add(name)
        if not (SRC / a["source"]).is_file():
            fail(f"{name}: no source {a['source']} in kody/assets/")
        for key in ("w", "h"):
            if not isinstance(a[key], int) or not 0 < a[key] <= 0xFFFF:
                fail(f"{name}: {key} {a[key]!r} is not a canvas size")
        if not isinstance(a["anchor"], int) or not 0 <= a["anchor"] <= 0xFF:
            fail(f"{name}: anchor {a['anchor']!r} does not fit a uint8_t")
        for old, new in a["recolour"].items():
            if not HEX.match(old) or not HEX.match(new):
                fail(f"{name}: recolour {old!r} -> {new!r} is not #RRGGBB -> #RRGGBB")
        if not a.get("help"):
            fail(f"{name}: no help line")
        if not isinstance(a.get("builtin", False), bool):
            fail(f"{name}: builtin is not true or false")
    check_pack(data.get("pack"), {a["name"]: a for a in data["animations"]})
    return data["animations"], data["pack"]


def pack_file(name):
    return f"{name}.json.z"


def check_pack(pack, entries):
    """The pack block, held to the rules kui_pack.h holds a pack to."""
    if not isinstance(pack, dict):
        fail("no pack block")
    for key in ("name", "version"):
        if not isinstance(pack.get(key), str) or not 0 < len(pack[key]) <= 31:
            fail(f"pack: {key} is not 1-31 characters")

    def entry(name, where, canvas):
        if name not in entries:
            fail(f"pack: {where} names {name!r}, which is no entry")
        if len(pack_file(name)) > FILE_NAME_MAX:
            fail(f"pack: {pack_file(name)} is over {FILE_NAME_MAX} characters")
        if (entries[name]["w"], entries[name]["h"]) != canvas:
            fail(f"pack: {where} {name} is {entries[name]['w']} x {entries[name]['h']}, not "
                 f"{canvas[0]} x {canvas[1]}")

    seen = set()
    scenes = pack.get("scenes")
    if not isinstance(scenes, list) or not scenes:
        fail("pack: no scenes")
    for sc in scenes:
        if not SCENE.match(sc.get("name", "")) or sc["name"] in seen:
            fail(f"pack: scene {sc.get('name')!r} is not a new name of 1-15 [a-z0-9_]")
        seen.add(sc["name"])
        for key in ("wake", "loop"):
            entry(sc.get(key, ""), f"{sc['name']}.{key}", FACE_CANVAS)
        for trigger, files in sc.get("on", {}).items():
            if trigger not in TRIGGERS:
                fail(f"pack: {sc['name']}.on.{trigger} is no trigger this firmware knows")
            for name in files:
                entry(name, f"{sc['name']}.on.{trigger}", FACE_CANVAS)
    face = pack.get("face", {})
    if face.get("default") is not None and face["default"] not in seen:
        fail(f"pack: face.default {face['default']!r} is no scene")
    for w in face.get("hours", []):
        if w.get("scene") not in seen or not 0 <= w.get("from", -1) < w.get("to", -1) <= 24:
            fail(f"pack: face.hours window {w!r} is not from < to on a scene")
    for role, canvas in ROLE_CANVAS.items():
        if role in pack:
            entry(pack[role], role, canvas)


def pack_names(pack):
    """Every entry the pack names, once, in the order the manifest names them."""
    names = []
    for sc in pack["scenes"]:
        for name in [sc["wake"], sc["loop"]] + [n for t in TRIGGERS for n in sc.get("on", {}).get(t, [])]:
            if name not in names:
                names.append(name)
    for role in ROLE_CANVAS:
        if role in pack and pack[role] not in names:
            names.append(pack[role])
    return names


def pack_json(pack, entries, streams):
    """kody/pack/kode.json: the pack block in the firmware's format, with every digest."""
    scenes = []
    for sc in pack["scenes"]:
        o = {"name": sc["name"], "wake": pack_file(sc["wake"]), "loop": pack_file(sc["loop"])}
        if entries[sc["loop"]]["anchor"]:
            o["anchor"] = entries[sc["loop"]]["anchor"]
        if sc.get("on"):
            o["on"] = {t: [pack_file(n) for n in files] for t, files in sc["on"].items()}
        scenes.append(o)
    m = {"format": 1, "name": pack["name"], "version": pack["version"], "scenes": scenes}
    if "face" in pack:
        m["face"] = pack["face"]
    for role in ROLE_CANVAS:
        if role in pack:
            m[role] = pack_file(pack[role])
    m["sha256"] = {pack_file(n): hashlib.sha256(streams[n]).hexdigest() for n in pack_names(pack)}
    return json.dumps(m, indent=2, ensure_ascii=False) + "\n"


# ------------------------------------------------------------------ the reframe
def point(v, s, dx, dy):
    """A position: [x, y, ...] -> [D + S x, D + S y, ...]."""
    return [num(dx + s * v[0]), num(dy + s * v[1])] + list(v[2:])


def scaled(v, s):
    """A scale in percent, or a tangent: every planar component times S."""
    return [num(s * v[0]), num(s * v[1])] + list(v[2:])


def reframe_prop(prop, fn, tangents, where):
    """Apply fn to a static value or to every keyframe's start and end values."""
    if prop.get("a", 0) == 0:
        prop["k"] = fn(prop["k"])
        return
    for key in prop["k"]:
        for side in ("s", "e"):
            if side in key:
                key[side] = fn(key[side])
        for side in ("ti", "to"):
            if side in key:
                if not tangents:
                    fail(f"{where}: tangents where none belong")
                key[side] = tangents(key[side])


def reframe_layer(layer, s, dx, dy, where):
    if layer.get("ddd", 0) != 0:
        fail(f"{where}: a 3D layer cannot be reframed in the plane")
    ks = layer.setdefault("ks", {})
    pos = ks.setdefault("p", {"a": 0, "k": [0, 0]})
    if pos.get("s"):
        fail(f"{where}: a split position is not supported")
    reframe_prop(pos, lambda v: point(v, s, dx, dy), lambda v: scaled(v, s), where)
    reframe_prop(ks.setdefault("s", {"a": 0, "k": [100, 100]}), lambda v: scaled(v, s), None, where)


def reframe(doc, a):
    doc["w"] = a["w"]
    doc["h"] = a["h"]
    roots = [L for L in doc["layers"] if "parent" not in L]
    if not roots:
        fail(f"{a['name']}: no top-level layer")
    for L in roots:
        reframe_layer(L, float(a["scale"]), float(a["dx"]), float(a["dy"]), f"{a['name']}/{L.get('nm')}")


# ------------------------------------------------------------------ colours
def hex8(c):
    return "#" + "".join(f"{round(float(x) * 255):02X}" for x in c[:3])


def channel(byte):
    """An 8-bit channel as a Lottie colour component, to four decimals as the sources write them."""
    r = round(byte / 255, 4)
    if r == int(r):
        return int(r)
    return r


def rgb(hexstr, alpha):
    return [channel(int(hexstr[i:i + 2], 16)) for i in (1, 3, 5)] + list(alpha)


def recolour_items(items, table, counts):
    for it in items:
        ty = it.get("ty")
        if ty in ("fl", "st"):
            c = it["c"]
            values = [c] if c.get("a", 0) == 0 else [k for k in c["k"]]
            for holder in values:
                for side in ("k", "s", "e"):
                    v = holder.get(side)
                    if not isinstance(v, list) or not v or not isinstance(v[0], (int, float)):
                        continue
                    key = hex8(v).upper()
                    if key in table:
                        holder[side] = rgb(table[key], v[3:])
                        counts[key] += 1
        elif ty == "gr":
            recolour_items(it["it"], table, counts)


def recolour(doc, a):
    """Replace the entry's colours; [(old, new, places)] in the manifest's order."""
    table = {old.upper(): new.upper() for old, new in a["recolour"].items()}
    counts = {old: 0 for old in table}
    layers = list(doc["layers"])
    for asset in doc.get("assets", []):
        layers += asset.get("layers", [])
    for L in layers:
        recolour_items(L.get("shapes", []), table, counts)
    for old, n in counts.items():
        if n == 0:
            fail(f"{a['name']}: {old} is not a colour of {a['source']}")
    return [(old, table[old], n) for old, n in counts.items()]


# ------------------------------------------------------------------ the names
def strip_names(node):
    """Drop STRIPPED from every object under node, node included, in place."""
    if isinstance(node, dict):
        for key in STRIPPED:
            node.pop(key, None)
        for child in node.values():
            strip_names(child)
    elif isinstance(node, list):
        for child in node:
            strip_names(child)


# ------------------------------------------------------------------ the lint
def join(path, key):
    return f"{path}.{key}" if path else key


def refused_under(node, path=""):
    """[(path, why)] for every expression and every refused shape under node.

    An expression is a string `x` on a property; the `x` of an easing handle is
    a number and the `x` of a split position is a property. A shape is told by
    its `ty`, a string, where a layer's and an effect's are numbers.
    """
    found = []
    if isinstance(node, dict):
        if isinstance(node.get("x"), str):
            found.append((join(path, "x"), "an expression"))
        if isinstance(node.get("ty"), str) and node["ty"] in SHAPE_REFUSED:
            found.append((join(path, "ty"), SHAPE_REFUSED[node["ty"]]))
        for key, child in node.items():
            found += refused_under(child, join(path, key))
    elif isinstance(node, list):
        for i, child in enumerate(node):
            found += refused_under(child, f"{path}[{i}]")
    return found


def lint_layers(layers, owner):
    """[(layer, property, why)] for each of layers; owner names the precomp they sit in."""
    found = []
    for i, layer in enumerate(layers):
        where = f"{owner}layer {i}"
        if layer.get("nm"):
            where += f' "{layer["nm"]}"'
        if layer.get("ddd", 0) != 0:
            found.append((where, "ddd", "a 3D layer"))
        if layer.get("ty") in LAYER_REFUSED:
            found.append((where, "ty", LAYER_REFUSED[layer["ty"]]))
        for j, effect in enumerate(layer.get("ef", [])):
            if effect.get("ty") != EFFECT_BLUR:
                label = effect.get("nm") or effect.get("mn") or f"of type {effect.get('ty')}"
                found.append((where, f"ef[{j}]", f"an effect, {label}, that is not a blur"))
        found += [(where, prop, why) for prop, why in refused_under(layer)]
    return found


def lint(name, doc):
    """What the ThorVG on the board does not draw in a source: one line each, none when clean.

    Each line names the animation, the layer (its place in its list, and its
    name when it has one) and the property, as a path from the layer.
    """
    found = []
    if doc.get("ddd", 0) != 0:
        found.append(("the animation", "ddd", "a 3D animation"))
    found += lint_layers(doc.get("layers", []), "")
    for asset in doc.get("assets", []):
        found += lint_layers(asset.get("layers", []), f'precomp "{asset.get("id")}" ')
    return [f"{name}: {where}: {prop}: the board ignores {why}" for where, prop, why in found]


# ------------------------------------------------------------------ the cost
def count(node):
    """(shapes that make a path, keyframes) under node."""
    shapes = keyframes = 0
    if isinstance(node, dict):
        if node.get("ty") in SHAPE_PATHS:
            shapes = 1
        if node.get("a") == 1 and isinstance(node.get("k"), list):
            keyframes = len(node["k"])
        children = node.values()
    elif isinstance(node, list):
        children = node
    else:
        return 0, 0
    for child in children:
        s, k = count(child)
        shapes += s
        keyframes += k
    return shapes, keyframes


def cost(doc):
    """(shapes, keyframes, depth) of an animation: what ThorVG draws and interpolates, and how deep it nests."""
    shapes, keyframes = count(doc)
    return shapes, keyframes, json_depth(doc)


def cost_lines(summary):
    """One line per animation of summary: its shapes, keyframes and the depth of its JSON."""
    width = max((len(s[0]) for s in summary), default=0)
    return [f"{s[0].ljust(width)}  {s[8][0]:>4} shapes, {s[8][1]:>5} keyframes, depth {s[8][2]:>2}"
            for s in summary]


# ------------------------------------------------------------------ the stream
def inflate(stream):
    """The payload of one whole zlib stream, or None when the bytes are not one."""
    d = zlib.decompressobj()
    try:
        payload = d.decompress(stream) + d.flush()
    except zlib.error:
        return None
    if not d.eof or d.unused_data:
        return None
    return payload


def held(path):
    """The stream a data file holds, or None when there is no such file or no whole array in it.

    A pack file is the stream itself; a .c file holds it as a byte array.
    """
    if not path.is_file():
        return None
    if path.suffix == ".z":
        return path.read_bytes()
    found = STREAM.search(path.read_text(encoding="utf-8"))
    if not found:
        return None
    stream = bytes(int(b, 16) for b in re.findall(r"0x([0-9a-f]{2}),", found.group(2)))
    if len(stream) != int(found.group(1)):
        return None
    return stream


def stream_for(paths, blob):
    """The zlib stream of blob: the one path holds, or a new one.

    The held stream is kept while it inflates to blob and is at most SLACK_PCT
    longer than the new one. A longer one is a stream of another level: right,
    and a waste of the flash the deflate saves.
    """
    fresh = zlib.compress(blob, DEFLATE_LEVEL)
    for path in paths:
        stream = held(path)
        if stream is None or inflate(stream) != blob:
            continue
        if len(stream) * 100 > len(fresh) * (100 + SLACK_PCT):
            continue
        return stream
    return fresh


# ------------------------------------------------------------------ output
def describe(a, frames, fps):
    text = f"{a['help']}. {a['w']} x {a['h']}, {frames} frames at {fps} fps"
    if a["anchor"]:
        text += f", an anchor every {a['anchor']}"
    return text + "."


def c_file(a, stream, json_len, frames, fps, recoloured):
    sym = f"kody_{a['name']}"
    lines = [
        GENERATED,
        "/**",
        f" * @file {sym}.c",
        f" * @brief {sym}: kody/assets/{a['source']} on a {a['w']} x {a['h']} canvas.",
        " *",
        f" * Reframed by S {a['scale']}, D ({a['dx']}, {a['dy']}).",
    ]
    for old, new, n in recoloured:
        places = "place" if n == 1 else "places"
        lines.append(f" * Recoloured {old} -> {new} ({n} {places}).")
    lines += [
        " */",
        '#include "kui_kody_anims.h"',
        "",
        f"/** The Lottie, minified and deflated: a zlib stream of {len(stream)} bytes that inflates to "
        f"{json_len}. */",
        f"static const uint8_t s_zlib[{len(stream)}] = {{",
    ]
    for i in range(0, len(stream), BYTES_PER_LINE):
        lines.append("    " + " ".join(f"0x{b:02x}," for b in stream[i:i + BYTES_PER_LINE]))
    lines += [
        "};",
        "",
        f"const kui_kody_anim_t {sym} = {{",
        "    .zlib     = s_zlib,",
        "    .zlib_len = (uint32_t)sizeof(s_zlib),",
        f"    .json_len = {json_len}U,",
        f"    .w        = {a['w']}U,",
        f"    .h        = {a['h']}U,",
        f"    .frames   = {frames}U,",
        f"    .fps      = {fps}U,",
        f"    .anchor   = {a['anchor']}U,",
        "    .deflated = true,",
        "};",
        "",
    ]
    return "\n".join(lines)


def h_file(entries):
    width = max(len(f"kody_{a['name']};") for a, _, _ in entries)
    lines = [
        GENERATED,
        "/**",
        " * @file kui_kody_anims.h",
        " * @brief Kody's built-in scene, one descriptor per animation, for the player in kui_kody.h.",
        " */",
        "#ifndef KUI_KODY_ANIMS_H",
        "#define KUI_KODY_ANIMS_H",
        "",
        '#include "kui_kody.h"',
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
    ]
    for a, frames, fps in entries:
        decl = f"kody_{a['name']};".ljust(width)
        lines.append(f"extern const kui_kody_anim_t {decl} /**< {describe(a, frames, fps)} */")
    lines += [
        "",
        "#ifdef __cplusplus",
        "}",
        "#endif",
        "",
        "#endif /* KUI_KODY_ANIMS_H */",
        "",
    ]
    return "\n".join(lines)


# The firmware's own limits on a pack file (kui_pack.h); a file past one is
# refused on the board, so it is refused here first.
PACK_JSON_MAX = 96 * 1024    # KUI_PACK_JSON_MAX: bytes of the inflated JSON
PACK_FILE_MAX = 64 * 1024    # KUI_PACK_FILE_MAX: bytes of the file as stored
PACK_DEPTH_MAX = 32          # KUI_PACK_MAX_DEPTH: deepest nesting
PACK_FPS_MAX = 60            # KUI_PACK_FPS_MAX
PACK_FRAMES_MAX = 1800       # KUI_PACK_FRAMES_MAX


def json_depth(node):
    """The deepest nesting of objects and arrays under node, node included."""
    if isinstance(node, dict):
        return 1 + max((json_depth(v) for v in node.values()), default=0)
    if isinstance(node, list):
        return 1 + max((json_depth(v) for v in node), default=0)
    return 0


def check_limits(a, doc, blob, stream, fps, frames):
    """Fail on anything kui_pack_check_anim() would refuse on the board."""
    if not 0 < fps <= PACK_FPS_MAX:
        fail(f"{a['name']}: {fps} fps is past the board's {PACK_FPS_MAX}")
    if frames > PACK_FRAMES_MAX:
        fail(f"{a['name']}: {frames} frames is past the board's {PACK_FRAMES_MAX}")
    if len(blob) > PACK_JSON_MAX:
        fail(f"{a['name']}: {len(blob)} bytes of JSON is past the board's {PACK_JSON_MAX}")
    if len(stream) > PACK_FILE_MAX:
        fail(f"{a['name']}: {len(stream)} bytes stored is past the board's {PACK_FILE_MAX}")
    if json_depth(doc) > PACK_DEPTH_MAX:
        fail(f"{a['name']}: nested {json_depth(doc)} deep, past the board's {PACK_DEPTH_MAX}")


def prepare(a, doc):
    """doc as the board gets it: reframed, recoloured and without its names, in place.

    Returns (the minified JSON, fps, frames, [(old, new, places)] recoloured).
    """
    fps = doc["fr"]
    frames = doc["op"] - doc["ip"]
    if fps != int(fps) or not 0 < int(fps) <= 0xFF:
        fail(f"{a['name']}: {fps} fps does not fit a uint8_t")
    if frames != int(frames) or not 0 < int(frames) <= 0xFFFF:
        fail(f"{a['name']}: {frames} frames do not fit a uint16_t")
    fps, frames = int(fps), int(frames)
    if a["anchor"] and frames % a["anchor"]:
        fail(f"{a['name']}: an anchor every {a['anchor']} does not divide {frames} frames")
    reframe(doc, a)
    recoloured = recolour(doc, a)
    strip_names(doc)
    blob = json.dumps(doc, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    return blob, fps, frames, recoloured


def generate(animations, pack, root):
    """{relative path: text or bytes} for every file the manifest makes, plus a summary per entry.

    root is where the data files already are: a stream found there is kept
    while it still inflates to its entry's JSON and is not too long (see the
    module's docstring). A source the lint refuses ends the script before
    anything is made.
    """
    docs = {a["name"]: json.loads((SRC / a["source"]).read_text(encoding="utf-8")) for a in animations}
    refused = [line for a in animations for line in lint(a["name"], docs[a["name"]])]
    if refused:
        sys.exit("\n".join(f"kodygen: lint: {line}" for line in refused))
    files = {}
    entries = []
    summary = []
    streams = {}
    for a in animations:
        doc = docs[a["name"]]
        blob, fps, frames, recoloured = prepare(a, doc)
        rel = f"{DATA_DIR}/kody_{a['name']}.c"
        packed = f"{PACK_DIR}/{pack_file(a['name'])}"
        stream = stream_for([root / packed, root / rel], blob)
        if inflate(stream) != blob:
            fail(f"{a['name']}: the stream does not inflate to the JSON it was made from")
        check_limits(a, doc, blob, stream, fps, frames)
        files[packed] = stream
        streams[a["name"]] = stream
        if a.get("builtin", False):
            files[rel] = c_file(a, stream, len(blob), frames, fps, recoloured)
            entries.append((a, frames, fps))
        summary.append((a["name"], a["w"], a["h"], frames, fps, a["anchor"], len(blob), len(stream),
                        cost(doc)))
    files[str(HEADER)] = h_file(entries)
    files[f"{PACK_DIR}/{PACK_MANIFEST}"] = pack_json(pack, {a["name"]: a for a in animations}, streams)
    return files, summary


def write(root, files):
    for rel, content in files.items():
        out = root / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(content, bytes):
            out.write_bytes(content)
        else:
            out.write_text(content, encoding="utf-8", newline="\n")


def stale_data(root, files):
    """Generated data files on disk that the manifest does not make."""
    found = {f"{DATA_DIR}/{p.name}" for p in (root / DATA_DIR).glob("kody_*.c")}
    found |= {f"{PACK_DIR}/{p.name}" for p in (root / PACK_DIR).glob("*.json.z")}
    return sorted(found - set(files))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true",
                      help="exit 1 when a checked-in file differs from what the sources make")
    mode.add_argument("--lint", action="store_true",
                      help="refuse what the board does not draw and print each animation's cost; "
                           "write nothing")
    args = ap.parse_args(argv)
    animations, pack = load()
    files, summary = generate(animations, pack, KIT)
    json_bytes = sum(s[6] for s in summary)
    stream_bytes = sum(s[7] for s in summary)
    if args.lint:
        print(f"lint: {len(summary)} animations, nothing the board ignores")
        for line in cost_lines(summary):
            print(line)
        return 0
    if args.check:
        bad = []
        with tempfile.TemporaryDirectory() as tmp:
            write(Path(tmp), files)
            for rel in files:
                fresh = (Path(tmp) / rel).read_bytes()
                disk = KIT / rel
                if not disk.exists() or disk.read_bytes() != fresh:
                    bad.append(rel)
        bad += [f"{rel} (not in the manifest)" for rel in stale_data(KIT, files)]
        if bad:
            for rel in bad:
                print(f"{rel} is stale: run tools/kodygen.py")
            return 1
        print(f"{PACK_DIR}/, {DATA_DIR}/ and {HEADER} match kody/assets/ ({len(summary)} animations, "
              f"{json_bytes} B of JSON in {stream_bytes} B)")
        return 0
    for rel in stale_data(KIT, files):
        (KIT / rel).unlink()
        print(f"deleted {rel}: not in the manifest's pack or built-in scene")
    write(KIT, files)
    for name, w, h, frames, fps, anchor, size, packed, _ in summary:
        print(f"wrote {PACK_DIR}/{pack_file(name)}  {w} x {h}, {frames} f at {fps} fps, anchor {anchor}, "
              f"{size} B in {packed}")
    for rel in sorted(r for r in files if r.startswith(DATA_DIR + "/")):
        print(f"wrote {rel}")
    print(f"wrote {HEADER}")
    print(f"wrote {PACK_DIR}/{PACK_MANIFEST}")
    print(f"{json_bytes} B of JSON in {stream_bytes} B")
    for line in cost_lines(summary):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
