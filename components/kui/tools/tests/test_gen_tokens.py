#!/usr/bin/env python3
"""Tests for tools/gen_tokens.py: the two headers it writes and the drift it refuses.

    python3 -m unittest discover -s components/kui/tools/tests

Needs nothing installed.
"""
import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))
import gen_tokens  # noqa: E402


class Generated(unittest.TestCase):
    def test_check_passes_on_the_tree(self):
        done = subprocess.run([sys.executable, str(TOOLS / "gen_tokens.py"), "--check"],
                              capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn("kode_colors.h", done.stdout)

    def test_colors_header_names_every_token_and_white(self):
        text = gen_tokens.colors_header(gen_tokens.load())
        for name in gen_tokens.COLOR_NAMES.values():
            self.assertIn(f"#define KODE_{name} ", text)
        self.assertRegex(text, r"#define KODE_WHITE\s+0xFFFFFFU")
        self.assertIn("not a palette token", text)
        self.assertIn("Do not edit", text)
        self.assertIn("kode_rgb(r, g, b)", text)

    def test_colors_header_has_the_six_pure_colours(self):
        text = gen_tokens.colors_header(gen_tokens.load())
        for name, value in (("RED", "FF0000"), ("MAGENTA", "FF00FF"), ("CYAN", "00FFFF"),
                            ("PURE_GREEN", "00FF00"), ("PURE_BLUE", "0000FF"),
                            ("PURE_YELLOW", "FFFF00")):
            self.assertRegex(text, rf"#define KODE_{name}\s+0x{value}U")
        self.assertIn("for the LED", text)
        self.assertIn("Any other colour is kode_rgb(r, g, b)", text)

    def test_blue_is_kody_blue(self):
        text = gen_tokens.colors_header(gen_tokens.load())
        self.assertRegex(text, r"#define KODE_BLUE\s+0x028AC2U")


class Drift(unittest.TestCase):
    def load_with(self, edit):
        data = json.loads(gen_tokens.SRC.read_text(encoding="utf-8"))
        edit(data)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "tokens.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            saved = gen_tokens.SRC
            gen_tokens.SRC = path
            try:
                with contextlib.redirect_stderr(io.StringIO()):
                    return gen_tokens.load()
            finally:
                gen_tokens.SRC = saved

    def test_a_palette_entry_without_a_name_fails(self):
        def add(data):
            data["palette"].append({"name": "teal", "value": "#008080", "help": "x"})
        with self.assertRaises(SystemExit) as caught:
            self.load_with(add)
        self.assertIn("teal", str(caught.exception))

    def test_a_name_without_a_palette_entry_fails(self):
        def drop(data):
            data["palette"] = [p for p in data["palette"] if p["name"] != "off_grey"]
            data["roles"] = [r for r in data["roles"] if r["palette"] != "off_grey"]
        with self.assertRaises(SystemExit) as caught:
            self.load_with(drop)
        self.assertIn("off_grey", str(caught.exception))

    def test_two_entries_with_one_name_fail(self):
        saved = dict(gen_tokens.COLOR_NAMES)
        gen_tokens.COLOR_NAMES["off_grey"] = "BLUE"
        try:
            with self.assertRaises(SystemExit):
                self.load_with(lambda data: None)
        finally:
            gen_tokens.COLOR_NAMES.clear()
            gen_tokens.COLOR_NAMES.update(saved)

    def test_a_pure_name_colliding_with_a_token_name_fails(self):
        saved = dict(gen_tokens.COLOR_NAMES)
        gen_tokens.COLOR_NAMES["off_grey"] = "RED"
        try:
            with self.assertRaises(SystemExit):
                self.load_with(lambda data: None)
        finally:
            gen_tokens.COLOR_NAMES.clear()
            gen_tokens.COLOR_NAMES.update(saved)

    def test_a_pure_value_equal_to_a_token_value_is_allowed(self):
        def pure_blue(data):
            for p in data["palette"]:
                if p["name"] == "off_grey":
                    p["value"] = "#0000FF"
        data = self.load_with(pure_blue)
        text = gen_tokens.colors_header(data)
        self.assertRegex(text, r"#define KODE_CHARCOAL\s+0x0000FFU")
        self.assertRegex(text, r"#define KODE_PURE_BLUE\s+0x0000FFU")


if __name__ == "__main__":
    unittest.main()
