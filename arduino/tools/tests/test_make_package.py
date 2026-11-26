#!/usr/bin/env python3
"""Tests for arduino/tools/make_package.py: the platform.txt it writes, the index it writes, and what it refuses.

    python3 -m unittest discover -s arduino/tools/tests

Needs nothing installed and fetches nothing: Espressif's index is a small one
written here.
"""
import copy
import hashlib
import stat
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.dont_write_bytecode = True
TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))
import make_package  # noqa: E402

RELEASE = "4.0.0"
VERSION = "0.1.5"
BASE_URL = "https://example.org/kode"

# platform.txt as Espressif's release writes it, cut down; one line ends in \r\n.
PLATFORM_TXT = """name=ESP32 Arduino
version=4.0.0\r

tools.esp32-arduino-libs.path={runtime.tools.{build.chip_variant}-libs.path}
tools.esp32-arduino-libs.path.windows={runtime.tools.{build.chip_variant}-libs.path}
compiler.sdk.path={tools.esp32-arduino-libs.path}
build.chip_variant={build.mcu}
"""


def system(host, name, version, digest, size):
    """One host's entry of a tool."""
    return {
        "host": host,
        "url": f"https://espressif.example/{name}-{version}-{host}.zip",
        "archiveFileName": f"{name}-{version}-{host}.zip",
        "checksum": f"SHA-256:{digest}",
        "size": size,
    }


def tool(name, version, hosts=("x86_64-pc-linux-gnu", "arm64-apple-darwin")):
    """A tool of Espressif's index, each host with its own url, checksum and size."""
    systems = [system(host, name, version, f"{n:064x}", 1000 + n) for n, host in enumerate(hosts)]
    return {"name": name, "version": version, "systems": systems}


ESPRESSIF_TOOLS = [
    tool("esp-x32", "2512"),
    tool("xtensa-esp-elf-gdb", "17.1_20260402"),
    tool("esp-rv32", "2511"),
    tool("esp-rv32", "2512"),
    tool("riscv32-esp-elf-gdb", "17.1_20260402"),
    tool("openocd-esp32", "v0.12.0-esp32-20260831"),
    tool("esptool_py", "5.3.1"),
    tool("mklittlefs", "4.0.2-db0513a"),
    tool("mkspiffs", "0.2.3"),
    tool("esp32-libs", RELEASE),
    tool("esp32p4-libs", RELEASE, ("i686-mingw32", "x86_64-mingw32", "arm64-apple-darwin", "x86_64-pc-linux-gnu")),
]

ESPRESSIF_DEPENDENCIES = [
    {"packager": "esp32", "name": "esp-x32", "version": "2512"},
    {"packager": "esp32", "name": "xtensa-esp-elf-gdb", "version": "17.1_20260402"},
    {"packager": "esp32", "name": "esp-rv32", "version": "2512"},
    {"packager": "esp32", "name": "riscv32-esp-elf-gdb", "version": "17.1_20260402"},
    {"packager": "esp32", "name": "openocd-esp32", "version": "v0.12.0-esp32-20260831"},
    {"packager": "esp32", "name": "esptool_py", "version": "5.3.1"},
    {"packager": "esp32", "name": "mkspiffs", "version": "0.2.3"},
    {"packager": "esp32", "name": "mklittlefs", "version": "4.0.2-db0513a"},
    {"packager": "arduino", "name": "dfu-util", "version": "0.11.0-arduino5"},
    {"packager": "esp32", "name": "esp32-libs", "version": RELEASE},
    {"packager": "esp32", "name": "esp32p4-libs", "version": RELEASE},
]

ESPRESSIF_INDEX = {
    "packages": [{
        "name": "esp32",
        "maintainer": "Espressif Systems",
        "platforms": [{
            "name": "esp32",
            "architecture": "esp32",
            "version": RELEASE,
            "category": "ESP32",
            "url": f"https://espressif.example/esp32-core-{RELEASE}.zip",
            "archiveFileName": f"esp32-core-{RELEASE}.zip",
            "checksum": "SHA-256:" + "ab" * 32,
            "size": "62781672",
            "boards": [{"name": "ESP32 Dev Board"}],
            "toolsDependencies": ESPRESSIF_DEPENDENCIES,
        }],
        "tools": ESPRESSIF_TOOLS,
    }],
}

CORE = make_package.Archive(f"kode-esp32-{VERSION}.zip", "SHA-256:" + "c0" * 32, 123)
LIBS = make_package.Archive(f"kodedot-libs-{VERSION}.zip", "SHA-256:" + "11" * 32, 456)


def espressif_index(rv32="2512"):
    """The small Espressif index above, its platform naming esp-rv32 at rv32."""
    made = copy.deepcopy(ESPRESSIF_INDEX)
    for depends in made["packages"][0]["platforms"][0]["toolsDependencies"]:
        if depends["name"] == "esp-rv32":
            depends["version"] = rv32
    return made


def build(version=VERSION, previous=None, espressif=None):
    """The index for VERSION, made from the small Espressif index above."""
    if espressif is None:
        espressif = espressif_index()
    old = None
    if previous is not None:
        old = make_package.published(previous, version)
    core = make_package.Archive(f"kode-esp32-{version}.zip", CORE.checksum, CORE.size)
    libs = make_package.Archive(f"kodedot-libs-{version}.zip", LIBS.checksum, LIBS.size)
    return make_package.index(espressif, RELEASE, version, core, libs, BASE_URL, old)


def refusal(call):
    """What the script said when it stopped."""
    with unittest.TestCase().assertRaises(SystemExit) as stopped:
        call()
    return str(stopped.exception.code)


class PlatformTxt(unittest.TestCase):
    def test_platform_txt_gets_our_name_version_and_libraries(self):
        out = make_package.platform_txt(PLATFORM_TXT, VERSION).splitlines()
        before = PLATFORM_TXT.splitlines()
        self.assertEqual(len(out), len(before))
        self.assertIn("name=Kode Dot", out)
        self.assertIn(f"version={VERSION}", out)
        self.assertIn("tools.esp32-arduino-libs.path={runtime.tools.kodedot-libs.path}", out)
        self.assertIn("tools.esp32-arduino-libs.path.windows={runtime.tools.kodedot-libs.path}", out)
        self.assertIn(f"version={VERSION}\r\n", make_package.platform_txt(PLATFORM_TXT, VERSION))
        changed = [n for n, (old, new) in enumerate(zip(before, out)) if old != new]
        self.assertEqual(changed, [0, 1, 3, 4])

    def test_a_platform_txt_without_one_of_the_lines_is_refused_naming_it(self):
        for key in ("name", "version", "tools.esp32-arduino-libs.path"):
            text = "".join(line for line in PLATFORM_TXT.splitlines(keepends=True)
                           if not line.startswith(key + "="))
            said = refusal(lambda: make_package.platform_txt(text, VERSION))
            self.assertIn(key + "=", said)


class Index(unittest.TestCase):
    def test_index_names_one_platform_and_one_board(self):
        made = build()
        self.assertEqual(len(made["packages"]), 1)
        package = made["packages"][0]
        self.assertEqual(package["name"], "kode")
        self.assertEqual(package["maintainer"], "Kode")
        self.assertEqual(len(package["platforms"]), 1)
        platform = package["platforms"][0]
        self.assertEqual(platform["name"], "Kode Dot")
        self.assertEqual(platform["architecture"], "esp32")
        self.assertEqual(platform["version"], VERSION)
        self.assertEqual(platform["boards"], [{"name": "Kode Dot (SDK)"}])
        self.assertEqual(platform["url"], f"{BASE_URL}/{CORE.file}")
        self.assertEqual(platform["archiveFileName"], CORE.file)
        self.assertEqual(platform["checksum"], CORE.checksum)
        self.assertEqual(platform["size"], str(CORE.size))
        self.assertEqual(platform["help"], {"online": "https://docs.kode.diy"})
        self.assertEqual(package["help"], {"online": "https://docs.kode.diy"})

    def test_index_copies_espressifs_tools_verbatim(self):
        package = build()["packages"][0]
        wanted = {d["name"]: d["version"] for d in ESPRESSIF_DEPENDENCIES
                  if d["name"] in make_package.ESPRESSIF_TOOLS}
        listed = {(t["name"], t["version"]): t for t in package["tools"]}
        for name, version in wanted.items():
            theirs = next(t for t in ESPRESSIF_TOOLS if (t["name"], t["version"]) == (name, version))
            self.assertEqual(listed[(name, version)], theirs)
        depends = package["platforms"][0]["toolsDependencies"]
        for name, version in wanted.items():
            self.assertIn({"packager": "kode", "name": name, "version": version}, depends)
        self.assertIn({"packager": "arduino", "name": "dfu-util", "version": "0.11.0-arduino5"}, depends)
        names = {d["name"] for d in depends}
        self.assertNotIn("esp-x32", names)
        self.assertNotIn("xtensa-esp-elf-gdb", names)
        self.assertNotIn("esp32p4-libs", names)
        self.assertNotIn(("esp-rv32", "2511"), listed)

    def test_index_lists_our_libraries_for_every_host_espressif_lists(self):
        package = build()["packages"][0]
        ours = next(t for t in package["tools"] if t["name"] == "kodedot-libs")
        self.assertEqual(ours["version"], VERSION)
        theirs = next(t for t in ESPRESSIF_TOOLS if t["name"] == "esp32p4-libs")
        self.assertEqual([s["host"] for s in ours["systems"]], [s["host"] for s in theirs["systems"]])
        for entry in ours["systems"]:
            self.assertEqual(entry["url"], f"{BASE_URL}/{LIBS.file}")
            self.assertEqual(entry["archiveFileName"], LIBS.file)
            self.assertEqual(entry["checksum"], LIBS.checksum)
            self.assertEqual(entry["size"], LIBS.size)
        self.assertIn({"packager": "kode", "name": "kodedot-libs", "version": VERSION},
                      package["platforms"][0]["toolsDependencies"])

    def test_index_keeps_the_versions_already_published(self):
        first = build("0.1.4", espressif=espressif_index("2511"))
        made = build(VERSION, first)
        package = made["packages"][0]
        self.assertEqual([p["version"] for p in package["platforms"]], ["0.1.4", VERSION])
        libs = sorted(t["version"] for t in package["tools"] if t["name"] == "kodedot-libs")
        self.assertEqual(libs, ["0.1.4", VERSION])
        pairs = [(t["name"], t["version"]) for t in package["tools"]]
        self.assertEqual(len(pairs), len(set(pairs)))
        self.assertIn(("esp-rv32", "2511"), pairs)
        self.assertIn(("esp-rv32", "2512"), pairs)
        old = next(t for t in package["tools"] if (t["name"], t["version"]) == ("esp-rv32", "2511"))
        theirs = next(t for t in ESPRESSIF_TOOLS if (t["name"], t["version"]) == ("esp-rv32", "2511"))
        self.assertEqual(old, theirs)

    def test_a_version_already_published_is_refused(self):
        first = build()
        said = refusal(lambda: make_package.published(first, VERSION))
        self.assertIn(VERSION, said)


class Download(unittest.TestCase):
    def test_a_mismatched_checksum_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "esp32-core.zip"
            path.write_bytes(b"not the release")
            good = "SHA-256:" + hashlib.sha256(b"not the release").hexdigest()
            size = path.stat().st_size
            make_package.checked(path, good, size)
            bad = "SHA-256:" + hashlib.sha256(b"the release").hexdigest()
            said = refusal(lambda: make_package.checked(path, bad, size))
            self.assertIn("esp32-core.zip", said)
            said = refusal(lambda: make_package.checked(path, good, size + 1))
            self.assertIn("esp32-core.zip", said)


TOP = f"esp32-core-{RELEASE}/"
EXECUTABLE = 0o755


def release_zip(path, extra=()):
    """A small release archive in Espressif's shape, with extra (name, bytes) entries."""
    files = [
        ("cores/esp32/Arduino.h", b"#define ARDUINO_H\n", 0o644),
        ("tools/get.py", b"#!/usr/bin/env python3\n", EXECUTABLE),
        ("boards.txt", b"esp32.name=ESP32 Dev Module\n", 0o644),
        ("variants/esp32/pins_arduino.h", b"#define ESP32_PINS\n", 0o644),
        ("variants/kodedot/pins_arduino.h", b"#define THEIR_KODEDOT\n", 0o644),
        ("platform.txt", PLATFORM_TXT.encode("utf-8"), 0o644),
        ("libraries/Wire/src/Wire.h", b"class TwoWire;\n", 0o644),
    ]
    files += [(name, data, 0o644) for name, data in extra]
    with zipfile.ZipFile(path, "w") as out:
        for name, data, mode in files:
            info = zipfile.ZipInfo(TOP + name)
            info.external_attr = mode << 16
            out.writestr(info, data)


def ours(root):
    """{path under root: bytes} of a folder of ours, as the archive should hold it."""
    return {path.relative_to(root).as_posix(): path.read_bytes() for path in make_package.files_of(root)}


class CoreArchive(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.source = self.dir / f"esp32-core-{RELEASE}.zip"
        self.out = self.dir / "out"
        self.out.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def test_the_platform_is_espressifs_with_our_four_things(self):
        release_zip(self.source)
        path = make_package.core_archive(self.source, VERSION, self.out)
        self.assertEqual(path.name, f"kode-esp32-{VERSION}.zip")
        top = f"kode-esp32-{VERSION}/"
        with zipfile.ZipFile(path) as made, zipfile.ZipFile(self.source) as theirs:
            names = made.namelist()
            self.assertEqual({name.split("/", 1)[0] for name in names}, {top.rstrip("/")})
            self.assertEqual(made.read(top + "boards.txt"), make_package.BOARDS.read_bytes())
            variants = {name[len(top + "variants/"):] for name in names if name.startswith(top + "variants/")}
            self.assertEqual(variants, {f"kodedot/{rel}" for rel in ours(make_package.VARIANT)})
            for rel, data in ours(make_package.VARIANT).items():
                self.assertEqual(made.read(f"{top}variants/kodedot/{rel}"), data)
            for rel, data in ours(make_package.LIBRARY).items():
                self.assertEqual(made.read(f"{top}libraries/KodeDot/{rel}"), data)
            for rel in ("cores/esp32/Arduino.h", "libraries/Wire/src/Wire.h", "tools/get.py"):
                self.assertEqual(made.read(top + rel), theirs.read(TOP + rel))
            mode = made.getinfo(top + "tools/get.py").external_attr >> 16
            self.assertTrue(mode & stat.S_IXUSR)
            platform = made.read(top + "platform.txt").decode("utf-8")
            self.assertEqual(platform, make_package.platform_txt(PLATFORM_TXT, VERSION))

    def test_a_cores_file_that_differs_is_refused_and_no_archive_is_left(self):
        release_zip(self.source)
        built = self.dir / "built.zip"
        with zipfile.ZipFile(self.source) as theirs, zipfile.ZipFile(built, "w") as out:
            for info in theirs.infolist():
                data = theirs.read(info)
                if info.filename.endswith("Arduino.h"):
                    data += b"// edited\n"
                out.writestr(info, data)
        said = refusal(lambda: make_package.same_untouched(self.source, built))
        self.assertIn("cores/esp32/Arduino.h", said)
        self.assertFalse(built.exists())

    def test_an_espressif_kodedot_library_is_refused(self):
        release_zip(self.source, [("libraries/KodeDot/src/KodeDot.h", b"theirs\n")])
        said = refusal(lambda: make_package.core_archive(self.source, VERSION, self.out))
        self.assertIn("libraries/KodeDot/", said)
        self.assertEqual(list(self.out.iterdir()), [])

    def test_two_top_level_folders_are_refused(self):
        release_zip(self.source)
        with zipfile.ZipFile(self.source, "a") as out:
            out.writestr("stray.txt", b"x")
        said = refusal(lambda: make_package.core_archive(self.source, VERSION, self.out))
        self.assertIn("not one folder", said)
        self.assertEqual(list(self.out.iterdir()), [])


class LibsArchive(unittest.TestCase):
    def test_the_libraries_sit_under_one_folder_named_as_the_tool(self):
        with tempfile.TemporaryDirectory() as tmp:
            libs = Path(tmp) / "esp32p4"
            (libs / "flags").mkdir(parents=True)
            (libs / "lib").mkdir()
            (libs / "flags" / "defines").write_text("-DX\n")
            (libs / "lib" / "libkode.a").write_bytes(b"!<arch>\n")
            (libs / "sdkconfig").write_text("CONFIG_X=y\n")
            (libs / "dependencies.lock").write_text("x\n")
            (libs / "pioarduino-build.py").write_text("x\n")
            out = Path(tmp) / "out"
            out.mkdir()
            path = make_package.libs_archive(libs, VERSION, out)
            self.assertEqual(path.name, f"kodedot-libs-{VERSION}.zip")
            with zipfile.ZipFile(path) as made:
                self.assertEqual(sorted(made.namelist()), [
                    "kodedot-libs/flags/defines",
                    "kodedot-libs/lib/libkode.a",
                    "kodedot-libs/sdkconfig",
                ])
                self.assertEqual(made.read("kodedot-libs/lib/libkode.a"), b"!<arch>\n")

    def test_a_folder_that_is_not_the_libraries_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            said = refusal(lambda: make_package.libs_archive(Path(tmp), VERSION, Path(tmp)))
            self.assertIn("flags/", said)


class Relocatable(unittest.TestCase):
    """The libraries' flags as the builder's export leaves them in its output folder."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.tree = Path(self.tmp.name).resolve() / "lib-builder"
        self.libs = self.tree / "out" / "tools" / "esp32-arduino-libs" / "esp32p4"
        (self.libs / "flags").mkdir(parents=True)
        (self.libs / "sdkconfig").write_text(f'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="{self.tree}/partitions.csv"\n')
        for name in ("c_flags", "cpp_flags", "S_flags", "ld_flags"):
            (self.libs / "flags" / name).write_text("-mabi=ilp32f -specs=picolibc.specs -ffunction-sections")
        (self.libs / "flags" / "includes").write_text("-iwithprefixbefore esp_libc/platform_include")
        self.out = Path(self.tmp.name) / "out"
        self.out.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def test_libraries_with_the_toolchains_spec_file_are_packed(self):
        path = make_package.libs_archive(self.libs, VERSION, self.out)
        with zipfile.ZipFile(path) as made:
            self.assertEqual(made.read("kodedot-libs/flags/c_flags"),
                             b"-mabi=ilp32f -specs=picolibc.specs -ffunction-sections")

    def test_a_spec_file_by_its_path_is_refused_naming_the_file(self):
        specs = "/app/kodediy/kode-sdk/build_arduino_libs/lib-builder/build/specs/picolibc.specs"
        (self.libs / "flags" / "cpp_flags").write_text(f'-mabi=ilp32f "-specs={specs}" -fno-rtti')
        said = refusal(lambda: make_package.libs_archive(self.libs, VERSION, self.out))
        self.assertIn("flags/cpp_flags", said)
        self.assertIn(specs, said)
        self.assertEqual(list(self.out.iterdir()), [])

    def test_a_path_of_the_build_tree_is_refused_naming_it(self):
        config = f"{self.tree}/build/config"
        (self.libs / "flags" / "includes").write_text(f"-iwithprefixbefore esp_libc/platform_include -I{config}")
        said = refusal(lambda: make_package.libs_archive(self.libs, VERSION, self.out))
        self.assertIn("flags/includes", said)
        self.assertIn(str(self.tree), said)
        self.assertEqual(list(self.out.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
