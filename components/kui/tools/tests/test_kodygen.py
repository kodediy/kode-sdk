#!/usr/bin/env python3
"""Tests for tools/kodygen.py: the names it strips, the lint, the cost report and the packed sizes.

    python3 -m unittest discover -s components/kui/tools/tests

Needs nothing installed. The Glaxnimate files in fixtures/glaxnimate/ are three
of Kody's own animations (duda, escena_tren, reloj_te_mira) drawn in
kody/assets/, opened in Glaxnimate 0.6 and exported again; fixtures/pack_before/
holds the streams the pack held for the same three before the script dropped
names, which is the size an export has to pack within.
"""
import contextlib
import copy
import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.dont_write_bytecode = True
TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))
import kodygen  # noqa: E402

FIXTURES = Path(__file__).resolve().parent / "fixtures"
ROUND_TRIPS = ("duda", "escena_tren", "reloj_te_mira")


def shape_layer(name, shapes, **extra):
    """A shape layer of the usual form, with one static transform."""
    layer = {"ddd": 0, "ty": 4, "ind": 1, "nm": name, "ip": 0, "op": 30, "st": 0,
             "ks": {"p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]}}, "shapes": shapes}
    layer.update(extra)
    return layer


def lottie(layers, **extra):
    """A 30 frame Lottie on the face's canvas."""
    doc = {"v": "5.7.1", "fr": 30, "ip": 0, "op": 30, "w": 410, "h": 300, "assets": [], "layers": layers}
    doc.update(extra)
    return doc


def group(*items):
    return {"ty": "gr", "it": list(items) + [{"ty": "tr"}]}


def path():
    return {"ty": "sh", "ks": {"a": 0, "k": {"c": True, "v": [], "i": [], "o": []}}}


def fill():
    return {"ty": "fl", "c": {"a": 0, "k": [1, 1, 1, 1]}}


def keyframes(count):
    """An animated property of count keyframes, easing handles included."""
    return {"a": 1, "k": [{"t": t, "s": [t], "i": {"x": [0.5], "y": [1]}, "o": {"x": [0.5], "y": [0]}}
                          for t in range(count)]}


class StripTests(unittest.TestCase):
    def test_drops_meta_mn_nm_at_every_depth(self):
        doc = {"nm": "a", "meta": {"g": "tool"}, "mn": "{1}", "w": 1,
               "layers": [{"nm": "l", "mn": "{2}", "shapes": [{"ty": "gr", "nm": "g",
                                                              "it": [{"ty": "sh", "nm": "p", "mn": "{3}"}]}]}],
               "assets": [{"id": "c", "nm": "c", "layers": [{"nm": "inner", "ef": [{"ty": 29, "nm": "Blur"}]}]}]}
        kodygen.strip_names(doc)
        self.assertEqual(doc, {"w": 1,
                               "layers": [{"shapes": [{"ty": "gr", "it": [{"ty": "sh"}]}]}],
                               "assets": [{"id": "c", "layers": [{"ef": [{"ty": 29}]}]}]})

    def test_keeps_the_order_of_what_stays(self):
        doc = {"v": 1, "nm": "x", "fr": 30, "mn": "y", "ip": 0}
        kodygen.strip_names(doc)
        self.assertEqual(list(doc), ["v", "fr", "ip"])

    def test_the_minified_output_has_no_names(self):
        a = self.entry("duda")
        doc = json.loads((FIXTURES / "glaxnimate" / "kody_duda.json").read_text(encoding="utf-8"))
        self.assertIn('"meta"', json.dumps(doc))
        blob = kodygen.prepare(a, doc)[0].decode("utf-8")
        for key in ("meta", "mn", "nm"):
            self.assertFalse(f'"{key}"' in blob, key)

    def test_the_sources_keep_their_names(self):
        for source in sorted(kodygen.SRC.glob("kody_*.json")):
            self.assertTrue('"nm"' in source.read_text(encoding="utf-8"), source.name)

    @staticmethod
    def entry(name):
        return {a["name"]: a for a in kodygen.load()[0]}[name]


class LintTests(unittest.TestCase):
    def one(self, doc):
        found = kodygen.lint("scn", doc)
        self.assertEqual(len(found), 1, found)
        return found[0]

    def test_a_clean_animation_has_no_findings(self):
        doc = lottie([shape_layer("Kody", [group(path(), fill())])])
        self.assertEqual(kodygen.lint("scn", doc), [])

    def test_pucker_and_bloat(self):
        doc = lottie([shape_layer("Body", [group(path(), {"ty": "pb", "nm": "Pucker"}, fill())])])
        self.assertEqual(self.one(doc),
                         'scn: layer 0 "Body": shapes[0].it[1].ty: the board ignores a pucker & bloat')

    def test_zig_zag(self):
        doc = lottie([shape_layer("Wave", [path(), {"ty": "zz"}])])
        self.assertEqual(self.one(doc), 'scn: layer 0 "Wave": shapes[1].ty: the board ignores a zig-zag')

    def test_an_effect_that_is_not_a_blur(self):
        shadow = {"ty": 25, "nm": "Drop Shadow", "mn": "ADBE Drop Shadow", "ef": []}
        doc = lottie([shape_layer("Kody", [group(path())], ef=[shadow])])
        self.assertEqual(self.one(doc), 'scn: layer 0 "Kody": ef[0]: the board ignores '
                                        'an effect, Drop Shadow, that is not a blur')

    def test_a_blur_is_drawn(self):
        blur = {"ty": 29, "nm": "Blur", "ef": [{"ty": 0, "nm": "Blurriness", "v": {"a": 0, "k": 4}}]}
        doc = lottie([shape_layer("Kody", [group(path())], ef=[blur])])
        self.assertEqual(kodygen.lint("scn", doc), [])

    def test_an_expression(self):
        layer = shape_layer("Kody", [group(path())])
        layer["ks"]["p"] = {"a": 0, "k": [0, 0], "x": "var $bm_rt = time * 10;"}
        self.assertEqual(self.one(lottie([layer])),
                         'scn: layer 0 "Kody": ks.p.x: the board ignores an expression')

    def test_an_expression_inside_a_shape(self):
        rc = {"ty": "rc", "s": {"a": 0, "k": [1, 1], "x": "wiggle(2, 3)"}}
        doc = lottie([shape_layer("Kody", [group(rc)])])
        self.assertEqual(self.one(doc), 'scn: layer 0 "Kody": shapes[0].it[0].s.x: the board ignores '
                                        'an expression')

    def test_easing_handles_and_split_positions_are_not_expressions(self):
        layer = shape_layer("Kody", [group(path())])
        layer["ks"]["r"] = keyframes(3)
        layer["ks"]["p"] = {"s": True, "x": {"a": 0, "k": 5}, "y": {"a": 0, "k": 6}}
        self.assertEqual(kodygen.lint("scn", lottie([layer])), [])

    def test_an_image_layer(self):
        layer = {"ddd": 0, "ty": 2, "ind": 1, "nm": "Photo", "refId": "img_0"}
        self.assertEqual(self.one(lottie([shape_layer("Kody", []), layer])),
                         'scn: layer 1 "Photo": ty: the board ignores an image layer')

    def test_a_text_layer(self):
        layer = {"ddd": 0, "ty": 5, "ind": 1, "nm": "Title", "t": {}}
        self.assertEqual(self.one(lottie([layer])),
                         'scn: layer 0 "Title": ty: the board ignores a text layer')

    def test_a_3d_layer(self):
        doc = lottie([shape_layer("Kody", [group(path())], ddd=1)])
        self.assertEqual(self.one(doc), 'scn: layer 0 "Kody": ddd: the board ignores a 3D layer')

    def test_a_3d_animation(self):
        doc = lottie([shape_layer("Kody", [group(path())])], ddd=1)
        self.assertEqual(self.one(doc), 'scn: the animation: ddd: the board ignores a 3D animation')

    def test_a_layer_without_a_name_is_told_by_its_place(self):
        layer = shape_layer("", [{"ty": "zz"}])
        del layer["nm"]
        self.assertEqual(self.one(lottie([shape_layer("Kody", []), layer])),
                         "scn: layer 1: shapes[0].ty: the board ignores a zig-zag")

    def test_a_precomp_layer_is_linted_too(self):
        inner = shape_layer("Inner", [{"ty": "pb"}])
        doc = lottie([shape_layer("Kody", [])], assets=[{"id": "comp_0", "layers": [inner]}])
        self.assertEqual(self.one(doc), 'scn: precomp "comp_0" layer 0 "Inner": shapes[0].ty: '
                                        'the board ignores a pucker & bloat')

    def test_every_finding_is_listed(self):
        doc = lottie([shape_layer("A", [{"ty": "zz"}, {"ty": "pb"}]), shape_layer("B", [], ddd=1)])
        self.assertEqual(len(kodygen.lint("scn", doc)), 3)

    def test_the_sources_and_the_round_trips_are_clean(self):
        for source in sorted(kodygen.SRC.glob("kody_*.json")) + sorted((FIXTURES / "glaxnimate").glob("*.json")):
            doc = json.loads(source.read_text(encoding="utf-8"))
            self.assertEqual(kodygen.lint(source.stem, doc), [], source.name)


class CostTests(unittest.TestCase):
    def test_a_known_animation(self):
        doc = lottie([shape_layer("Kody", [
            group(path(), path(), fill()),
            group(group({"ty": "rc"}, {"ty": "el"}, {"ty": "sr"}, fill()), {"ty": "st"}),
        ])])
        doc["layers"][0]["ks"]["r"] = keyframes(4)
        doc["layers"][0]["shapes"][0]["it"][0]["ks"] = keyframes(3)
        self.assertEqual(kodygen.cost(doc), (5, 7, 12))

    def test_a_static_property_has_no_keyframes(self):
        doc = lottie([shape_layer("Kody", [group(path())])])
        doc["layers"][0]["ks"]["p"] = {"a": 0, "k": [1, 2, 3]}
        self.assertEqual(kodygen.cost(doc)[1], 0)

    def test_the_depth_is_the_json_depth(self):
        doc = lottie([shape_layer("Kody", [group(path())])])
        self.assertEqual(kodygen.cost(doc)[2], kodygen.json_depth(doc))

    def test_a_round_trip_of_kodys_own_duda(self):
        doc = json.loads((FIXTURES / "glaxnimate" / "kody_duda.json").read_text(encoding="utf-8"))
        self.assertEqual(kodygen.cost(doc), (18, 68, 22))

    def test_the_other_round_trips(self):
        for name, expected in (("escena_tren", (38, 369, 28)), ("reloj_te_mira", (69, 71, 25))):
            doc = json.loads((FIXTURES / "glaxnimate" / f"kody_{name}.json").read_text(encoding="utf-8"))
            self.assertEqual(kodygen.cost(doc), expected, name)

    def test_a_line_per_animation(self):
        summary = [("duda", 0, 0, 0, 0, 0, 0, 0, (18, 68, 22)),
                   ("escena_tren", 0, 0, 0, 0, 0, 0, 0, (38, 369, 28))]
        self.assertEqual(kodygen.cost_lines(summary),
                         ["duda           18 shapes,    68 keyframes, depth 22",
                          "escena_tren    38 shapes,   369 keyframes, depth 28"])

    def test_stripping_loses_no_shape_and_no_keyframe(self):
        for name in ROUND_TRIPS:
            doc = json.loads((FIXTURES / "glaxnimate" / f"kody_{name}.json").read_text(encoding="utf-8"))
            before = kodygen.cost(doc)
            kodygen.prepare({a["name"]: a for a in kodygen.load()[0]}[name], doc)
            self.assertEqual(kodygen.cost(doc), before, name)


class PackTests(unittest.TestCase):
    def test_a_glaxnimate_round_trip_packs_within_the_original(self):
        entries = {a["name"]: a for a in kodygen.load()[0]}
        for name in ROUND_TRIPS:
            doc = json.loads((FIXTURES / "glaxnimate" / f"kody_{name}.json").read_text(encoding="utf-8"))
            blob = kodygen.prepare(entries[name], doc)[0]
            packed = len(zlib.compress(blob, kodygen.DEFLATE_LEVEL))
            before = len((FIXTURES / "pack_before" / f"{name}.json.z").read_bytes())
            self.assertLessEqual(packed, before, f"{name}: {packed} B against the original's {before} B")

    def test_the_names_are_what_the_export_added(self):
        """Without the strip an export packs well past the original: the reason the script strips."""
        entries = {a["name"]: a for a in kodygen.load()[0]}
        doc = json.loads((FIXTURES / "glaxnimate" / "kody_reloj_te_mira.json").read_text(encoding="utf-8"))
        named = len(zlib.compress(json.dumps(doc, separators=(",", ":")).encode("utf-8"), kodygen.DEFLATE_LEVEL))
        stripped = len(zlib.compress(kodygen.prepare(entries["reloj_te_mira"], doc)[0], kodygen.DEFLATE_LEVEL))
        self.assertGreater(named, stripped * 3 // 2)

    def test_the_checked_in_files_match_the_sources(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(kodygen.main(["--check"]), 0)


class ScriptTests(unittest.TestCase):
    """The script as a command, in a kit of its own: the sources are crafted and nothing else is touched."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.kit = Path(self.tmp.name)
        (self.kit / "tools").mkdir()
        shutil.copy(TOOLS / "kodygen.py", self.kit / "tools" / "kodygen.py")
        (self.kit / "kody" / "assets").mkdir(parents=True)

    def tearDown(self):
        self.tmp.cleanup()

    def kit_with(self, doc):
        manifest = {
            "animations": [{"name": "scn", "source": "kody_scn.json", "w": 410, "h": 300, "scale": 1,
                            "dx": 0, "dy": 0, "anchor": 0, "recolour": {}, "builtin": True,
                            "help": "A scene for a test"}],
            "pack": {"name": "Test", "version": "1", "scenes": [{"name": "s", "wake": "scn", "loop": "scn"}]},
        }
        assets = self.kit / "kody" / "assets"
        (assets / "kody.json").write_text(json.dumps(manifest), encoding="utf-8")
        (assets / "kody_scn.json").write_text(json.dumps(doc), encoding="utf-8")

    def run_script(self, *args):
        return subprocess.run([sys.executable, "-B", str(self.kit / "tools" / "kodygen.py"), *args],
                              capture_output=True, text=True, check=False)

    def written(self):
        return sorted(p.relative_to(self.kit).as_posix() for p in (self.kit / "kody").rglob("*")
                      if p.is_file() and "assets" not in p.parts)

    def test_lint_passes_a_clean_source_and_writes_nothing(self):
        self.kit_with(lottie([shape_layer("Kody", [group(path(), fill())])]))
        done = self.run_script("--lint")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("lint: 1 animations, nothing the board ignores", done.stdout)
        self.assertIn("scn     1 shapes,     0 keyframes, depth 10", done.stdout)
        self.assertEqual(self.written(), [])

    def test_lint_refuses_and_names_the_layer_and_the_property(self):
        self.kit_with(lottie([shape_layer("Kody", []), shape_layer("Mouth", [group(path(), {"ty": "zz"})])]))
        done = self.run_script("--lint")
        self.assertEqual(done.returncode, 1)
        self.assertIn('kodygen: lint: scn: layer 1 "Mouth": shapes[0].it[1].ty: the board ignores a zig-zag',
                      done.stderr)
        self.assertEqual(self.written(), [])

    def test_a_write_is_refused_before_anything_is_written(self):
        self.kit_with(lottie([shape_layer("Kody", [], ddd=1)]))
        done = self.run_script()
        self.assertEqual(done.returncode, 1)
        self.assertIn('layer 0 "Kody": ddd', done.stderr)
        self.assertEqual(self.written(), [])

    def test_a_write_strips_the_names_and_reports_the_cost(self):
        self.kit_with(lottie([shape_layer("Kody", [group(path(), fill())])], nm="scene", meta={"g": "tool"}))
        done = self.run_script()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("scn     1 shapes,     0 keyframes, depth 10", done.stdout)
        stream = (self.kit / "kody" / "pack" / "scn.json.z").read_bytes()
        packed = json.loads(zlib.decompress(stream))
        self.assertNotIn('"nm"', json.dumps(packed))
        self.assertNotIn("meta", packed)
        source = (self.kit / "kody" / "assets" / "kody_scn.json").read_text(encoding="utf-8")
        self.assertIn('"nm": "Kody"', source)
        self.assertEqual(self.run_script("--check").returncode, 0)

    def test_check_and_lint_exclude_each_other(self):
        self.kit_with(lottie([]))
        self.assertEqual(self.run_script("--check", "--lint").returncode, 2)

    def test_the_real_tree_lints_clean(self):
        done = subprocess.run([sys.executable, "-B", str(TOOLS / "kodygen.py"), "--lint"],
                              capture_output=True, text=True, check=False)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertRegex(done.stdout, r"lint: \d+ animations, nothing the board ignores")


if __name__ == "__main__":
    unittest.main()
