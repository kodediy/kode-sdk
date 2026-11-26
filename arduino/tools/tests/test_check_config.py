#!/usr/bin/env python3
"""Tests for arduino/tools/check_config.py: which differences between two sdkconfigs it lets through, and which lines of the list it calls stale.

    python3 -m unittest discover -s arduino/tools/tests

Needs nothing installed: the configurations and the list are small files
written here.
"""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parent.parent / "check_config.py"


class CheckConfig(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)

    def tearDown(self):
        self.dir.cleanup()

    def run_check(self, app, builder, keep):
        """(exit code, stdout) of the check on three texts."""
        paths = []
        for name, text in (("app", app), ("builder", builder), ("keep", keep)):
            path = self.root / name
            path.write_text(text)
            paths.append(str(path))
        done = subprocess.run([sys.executable, str(SCRIPT), *paths],
                              capture_output=True, text=True, check=False)
        return done.returncode, done.stdout

    def test_an_option_that_differs_and_is_not_listed_fails(self):
        code, out = self.run_check("CONFIG_A=y\nCONFIG_B=1\n", "CONFIG_A=y\nCONFIG_B=2\n", "")
        self.assertEqual(1, code)
        self.assertIn("CONFIG_B", out)
        self.assertNotIn("CONFIG_A", out)

    def test_a_listed_difference_passes(self):
        code, out = self.run_check("CONFIG_B=1\n", "CONFIG_B=2\n",
                                   "# Arduino's own.\nCONFIG_B  the core needs two\n")
        self.assertEqual(0, code, out)

    def test_a_listed_option_that_no_longer_differs_fails(self):
        code, out = self.run_check("CONFIG_B=1\n", "CONFIG_B=1\n", "CONFIG_B  the core needs two\n")
        self.assertEqual(1, code)
        self.assertIn("CONFIG_B", out)
        self.assertIn("stale", out)

    def test_an_option_only_one_file_names_is_not_compared(self):
        code, out = self.run_check("CONFIG_A=y\nCONFIG_ONLY_APP=y\n", "CONFIG_A=y\nCONFIG_ONLY_BUILDER=3\n", "")
        self.assertEqual(0, code, out)

    def test_not_set_equals_n(self):
        code, out = self.run_check("# CONFIG_X is not set\n", "CONFIG_X=n\n", "")
        self.assertEqual(0, code, out)

    def test_a_listed_option_with_no_reason_fails(self):
        code, out = self.run_check("CONFIG_B=1\n", "CONFIG_B=2\n", "CONFIG_B\n")
        self.assertEqual(1, code)
        self.assertIn("reason", out)

    def test_a_listed_option_gone_from_one_file_is_stale(self):
        code, out = self.run_check("CONFIG_B=1\n", "CONFIG_A=1\n", "CONFIG_B  the core needs two\n")
        self.assertEqual(1, code)
        self.assertIn("CONFIG_B", out)
        self.assertIn("stale", out)

    def test_a_choice_whose_members_flip_names_both(self):
        code, out = self.run_check("CONFIG_C_ONE=y\n# CONFIG_C_TWO is not set\n",
                                   "# CONFIG_C_ONE is not set\nCONFIG_C_TWO=y\n", "")
        self.assertEqual(1, code)
        self.assertIn("CONFIG_C_ONE", out)
        self.assertIn("CONFIG_C_TWO", out)

    def test_quoted_strings_are_compared_whole(self):
        same = 'CONFIG_S="a b"\n'
        self.assertEqual(0, self.run_check(same, same, "")[0])
        code, out = self.run_check('CONFIG_S="a b"\n', 'CONFIG_S="a c"\n', "")
        self.assertEqual(1, code)
        self.assertIn('"a c"', out)

    def test_the_count_of_one_sided_options_is_printed(self):
        code, out = self.run_check("CONFIG_A=y\nCONFIG_X=y\n", "CONFIG_A=y\nCONFIG_Y=y\nCONFIG_Z=y\n", "")
        self.assertEqual(0, code, out)
        self.assertIn("3 options only one file names", out)

    def test_an_sdk_option_missing_from_the_builder_fails(self):
        for name in ("CONFIG_KODE_X", "CONFIG_KODEOS_X", "CONFIG_KHAL_X", "CONFIG_KBSP_X",
                     "CONFIG_KLOG_X", "CONFIG_KQP_X", "CONFIG_KSCREEN_X"):
            with self.subTest(name=name):
                code, out = self.run_check(f"CONFIG_A=y\n{name}=y\n", "CONFIG_A=y\n", "")
                self.assertEqual(1, code)
                self.assertIn(name, out)

    def test_an_sdk_option_only_the_builder_names_passes(self):
        code, out = self.run_check("CONFIG_A=y\n", "CONFIG_A=y\nCONFIG_KODE_X=y\n", "")
        self.assertEqual(0, code, out)

    def test_a_list_that_names_an_option_twice_fails(self):
        code, out = self.run_check("CONFIG_B=1\n", "CONFIG_B=2\n",
                                   "CONFIG_B  one reason\nCONFIG_B  another\n")
        self.assertEqual(1, code)
        self.assertIn("twice", out)

    def test_esp_idf_deprecated_aliases_are_not_compared(self):
        tail = "\n# Deprecated options for backward compatibility\n"
        code, out = self.run_check(f"CONFIG_A=1\n{tail}CONFIG_OLD_A=1\n",
                                   f"CONFIG_A=1\n{tail}CONFIG_OLD_A=2\n", "")
        self.assertEqual(0, code, out)


if __name__ == "__main__":
    unittest.main()
