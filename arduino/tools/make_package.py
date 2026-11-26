#!/usr/bin/env python3
"""The Kode Dot's Boards Manager package: two archives and the index that lists them.

    arduino/tools/make_package.py --libs DIR --out DIR --base-url URL
                                  [--previous-index FILE] [--espressif-index FILE_OR_URL]

writes into --out:

    kode-esp32-<version>.zip     the platform
    kodedot-libs-<version>.zip   the libraries, DIR under one folder
    package_kode_index.json      the index, its addresses under --base-url

The platform is Espressif's core at the release arduino/pins.env pins, taken
by the address and checksum Espressif's index gives, with four things of ours:
boards.txt (the Kode Dot alone), variants/kodedot, libraries/KodeDot, and
three lines of platform.txt (the name, the version and the path of the
libraries' tool). The archive written is read back, and any file under cores/
or Espressif's libraries/ that is not Espressif's stops the run.

DIR is the ESP32-P4 libraries the library builder made with the SDK inside:
the folder that holds flags/, include/, lib/ and sdkconfig.

The index lists the platform, the libraries and, copied from Espressif's
index with every host's address, checksum and size, the tools Espressif's
platform names for a RISC-V chip. With --previous-index the platforms and
tools already published stay listed, and a version already there stops the
run. The version is the SDK's, from components/kode/idf_component.yml.
"""
import argparse
import copy
import hashlib
import json
import re
import shutil
import sys
import tempfile
import urllib.request
import zipfile
from collections import namedtuple
from pathlib import Path

SDK = Path(__file__).resolve().parent.parent.parent
ARDUINO = SDK / "arduino"
PINS = ARDUINO / "pins.env"
BOARDS = ARDUINO / "boards.txt"
VARIANT = ARDUINO / "variants" / "kodedot"
LIBRARY = ARDUINO / "libraries" / "KodeDot"
COMPONENT = SDK / "components" / "kode" / "idf_component.yml"

ESPRESSIF_INDEX = "https://espressif.github.io/arduino-esp32/package_esp32_index.json"
ESPRESSIF_ARCHITECTURE = "esp32"
# Espressif's libraries for the ESP32-P4: ours are listed for the hosts it is.
ESPRESSIF_LIBS = "esp32p4-libs"
# Espressif's tools a RISC-V chip needs, listed at the versions its platform names.
ESPRESSIF_TOOLS = ("esp-rv32", "riscv32-esp-elf-gdb", "openocd-esp32", "esptool_py", "mklittlefs", "mkspiffs")

PACKAGER = "kode"
MAINTAINER = "Kode"
WEBSITE = "https://kode.diy"
HELP = {"online": "https://docs.kode.diy"}
PLATFORM = "Kode Dot"
CATEGORY = "Contributed"
BOARD = "Kode Dot (SDK)"
LIBS_TOOL = "kodedot-libs"
CORE_NAME = "kode-esp32"
INDEX_FILE = "package_kode_index.json"

# platform.txt's lines that are ours; the libraries' path also has a Windows spelling.
LIBS_KEY = "tools.esp32-arduino-libs.path"
WINDOWS = ".windows"

UNTOUCHED = ("cores/", "libraries/")
KODEDOT_LIBRARY = "libraries/KodeDot/"
# Left out of the libraries' archive, as Espressif's release leaves them out of its own.
LIBS_SKIPPED = ("dependencies.lock", "pioarduino-build.py")
JUNK = (".DS_Store", "__pycache__")
# Our files carry one date, one mode and one system, so the same tree makes the same archive anywhere.
FIXED_TIME = (1980, 1, 1, 0, 0, 0)
FILE_MODE = 0o644 << 16
UNIX = 3
CHECKSUM = "SHA-256"
CHUNK = 1 << 20
# How long a fetch waits for the server before it gives up, in seconds.
URL_TIMEOUT_S = 60

Archive = namedtuple("Archive", "file checksum size")


def fail(msg):
    sys.exit(f"make_package: {msg}")


def read_pins():
    """arduino/pins.env as {key: value}."""
    pins = {}
    for line in PINS.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            key, sep, value = line.partition("=")
            if not sep:
                fail(f"{PINS.relative_to(SDK)}: '{line}' is not KEY=value")
            pins[key.strip()] = value.strip()
    return pins


def sdk_version():
    """The version: of components/kode/idf_component.yml."""
    found = re.search(r'^version:\s*"?([^"\s]+)"?\s*$', COMPONENT.read_text(encoding="utf-8"), re.M)
    if found is None:
        fail(f"{COMPONENT.relative_to(SDK)} has no version:")
    return found.group(1)


def platform_txt(text, version):
    """Espressif's released platform.txt with our name, our version and our libraries' tool."""
    ours = {"name": PLATFORM, "version": version, LIBS_KEY: "{runtime.tools.%s.path}" % LIBS_TOOL}
    found = set()
    out = []
    for line in text.splitlines(keepends=True):
        key, sep, _ = line.partition("=")
        base = key
        if key == LIBS_KEY + WINDOWS:
            base = LIBS_KEY
        if sep and base in ours:
            if key in found:
                fail(f"platform.txt has {key}= twice")
            found.add(key)
            body = line.rstrip("\r\n")
            line = f"{key}={ours[base]}{line[len(body):]}"
        out.append(line)
    for key in ours:
        if key not in found:
            fail(f"platform.txt has no line {key}=: is it Espressif's release?")
    return "".join(out)


def sha256(path):
    """The SHA-256 of a file, in hex."""
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def checked(path, checksum, size):
    """Stop the run unless the file is the one an index entry names."""
    algorithm, _, digest = checksum.partition(":")
    if algorithm != CHECKSUM:
        fail(f"{path.name}: the index gives a {algorithm} checksum, not {CHECKSUM}")
    got = path.stat().st_size
    if got != int(size):
        fail(f"{path.name} is {got} bytes, the index says {size}")
    got = sha256(path)
    if got != digest.lower():
        fail(f"{path.name} has {CHECKSUM} {got}, the index says {digest}")


def archive_of(path):
    """An archive as the index lists it."""
    return Archive(path.name, f"{CHECKSUM}:{sha256(path)}", path.stat().st_size)


def espressif_platform(espressif, release):
    """(package, platform) of Espressif's index at the release."""
    for package in espressif.get("packages", []):
        for platform in package.get("platforms", []):
            if platform.get("architecture") == ESPRESSIF_ARCHITECTURE and platform.get("version") == release:
                return package, platform
    fail(f"Espressif's index has no {ESPRESSIF_ARCHITECTURE} platform {release}")


def espressif_tool(package, platform, name):
    """The tool of Espressif's package at the version its platform names."""
    for depends in platform["toolsDependencies"]:
        if depends["name"] == name:
            if depends["packager"] != package["name"]:
                fail(f"Espressif's platform takes {name} from {depends['packager']}, not {package['name']}")
            for tool in package["tools"]:
                if (tool["name"], tool["version"]) == (name, depends["version"]):
                    return tool
            fail(f"Espressif's index names {name} {depends['version']} and does not list it")
    fail(f"Espressif's platform {platform['version']} does not name {name}")


def published(previous, version):
    """Our package of the previous index; stop the run when it has the version already."""
    for package in previous.get("packages", []):
        if package.get("name") == PACKAGER:
            listed = [p["version"] for p in package.get("platforms", [])]
            listed += [t["version"] for t in package.get("tools", []) if t["name"] == LIBS_TOOL]
            if version in listed:
                fail(f"version {version} is already published in the previous index")
            return package
    fail(f"the previous index has no package {PACKAGER}")


def index(espressif, release, version, core, libs, base_url, old=None):
    """The index of the package; old is our package of the previous index, as published() answers it."""
    theirs, platform = espressif_platform(espressif, release)
    base = base_url.rstrip("/")
    hosts = [entry["host"] for entry in espressif_tool(theirs, platform, ESPRESSIF_LIBS)["systems"]]
    tools = [{
        "name": LIBS_TOOL,
        "version": version,
        "systems": [{
            "host": host,
            "url": f"{base}/{libs.file}",
            "archiveFileName": libs.file,
            "checksum": libs.checksum,
            "size": libs.size,
        } for host in hosts],
    }]
    depends = [{"packager": PACKAGER, "name": LIBS_TOOL, "version": version}]
    for name in ESPRESSIF_TOOLS:
        tool = copy.deepcopy(espressif_tool(theirs, platform, name))
        tools.append(tool)
        depends.append({"packager": PACKAGER, "name": name, "version": tool["version"]})
    # A tool of another packager (Arduino's dfu-util) stays that packager's.
    depends += [copy.deepcopy(d) for d in platform["toolsDependencies"] if d["packager"] != theirs["name"]]
    platforms = [{
        "name": PLATFORM,
        "architecture": ESPRESSIF_ARCHITECTURE,
        "version": version,
        "category": CATEGORY,
        "url": f"{base}/{core.file}",
        "archiveFileName": core.file,
        "checksum": core.checksum,
        "size": str(core.size),
        "help": copy.deepcopy(HELP),
        "boards": [{"name": BOARD}],
        "toolsDependencies": depends,
    }]
    if old is not None:
        listed = {(t["name"], t["version"]) for t in old.get("tools", [])}
        platforms = copy.deepcopy(old.get("platforms", [])) + platforms
        tools = copy.deepcopy(old.get("tools", [])) + [t for t in tools if (t["name"], t["version"]) not in listed]
    return {"packages": [{
        "name": PACKAGER,
        "maintainer": MAINTAINER,
        "websiteURL": WEBSITE,
        "email": "",
        "help": copy.deepcopy(HELP),
        "platforms": platforms,
        "tools": tools,
    }]}


def load_index(where):
    """An index from a file or an address."""
    if "://" in where:
        with urllib.request.urlopen(where, timeout=URL_TIMEOUT_S) as answer:
            return json.loads(answer.read().decode("utf-8"))
    return json.loads(Path(where).read_text(encoding="utf-8"))


def download(url, path):
    print(f"fetching {url}")
    with urllib.request.urlopen(url, timeout=URL_TIMEOUT_S) as answer, open(path, "wb") as stream:
        shutil.copyfileobj(answer, stream, CHUNK)


def files_of(root):
    """The files of a folder of ours, in order, junk left out."""
    return [path for path in sorted(root.rglob("*"))
            if path.is_file() and not any(part in JUNK for part in path.relative_to(root).parts)]


def add_file(out, name, path):
    """A file of ours into an archive, with the fixed date, mode and system."""
    info = zipfile.ZipInfo(name, FIXED_TIME)
    info.external_attr = FILE_MODE
    info.create_system = UNIX
    info.compress_type = zipfile.ZIP_DEFLATED
    with open(path, "rb") as src, out.open(info, "w") as dst:
        shutil.copyfileobj(src, dst, CHUNK)


def add_text(out, name, text):
    """Text of ours into an archive, an ordinary file with the fixed date."""
    info = zipfile.ZipInfo(name, FIXED_TIME)
    info.external_attr = FILE_MODE
    info.create_system = UNIX
    info.compress_type = zipfile.ZIP_DEFLATED
    out.writestr(info, text.encode("utf-8"))


def copy_entry(src, info, out, name):
    """One entry of Espressif's archive, as it is, under a new name."""
    entry = zipfile.ZipInfo(name, info.date_time)
    entry.external_attr = info.external_attr
    entry.create_system = info.create_system
    entry.compress_type = zipfile.ZIP_DEFLATED
    if info.is_dir():
        entry.compress_type = zipfile.ZIP_STORED
    with src.open(info) as stream, out.open(entry, "w") as dst:
        shutil.copyfileobj(stream, dst, CHUNK)


def top_folder(archive, what):
    """The one folder an archive holds everything in, with its slash."""
    tops = {name.split("/", 1)[0] for name in archive.namelist()}
    if len(tops) != 1:
        fail(f"{what} has {len(tops)} top-level entries, not one folder")
    return tops.pop() + "/"


def untouched(archive):
    """{path under the top folder: entry} of the files under cores/ and Espressif's libraries/."""
    top = top_folder(archive, archive.filename)
    found = {}
    for info in archive.infolist():
        rel = info.filename[len(top):]
        if rel.startswith(UNTOUCHED) and not rel.startswith(KODEDOT_LIBRARY) and not info.is_dir():
            found[rel] = info
    return found


def same_untouched(source, built):
    """Stop the run, and drop the archive, when cores/ or Espressif's libraries/ are not Espressif's."""
    with zipfile.ZipFile(source) as theirs, zipfile.ZipFile(built) as ours:
        a = untouched(theirs)
        b = untouched(ours)
        differ = sorted(set(a) ^ set(b))
        differ += [rel for rel in sorted(set(a) & set(b)) if theirs.read(a[rel]) != ours.read(b[rel])]
    if differ:
        built.unlink()
        fail(f"{len(differ)} files under cores/ or libraries/ are not Espressif's, the first {differ[0]}")


def core_archive(source, version, out):
    """The platform: Espressif's archive with our board entry, variant, library and platform.txt."""
    folder = f"{CORE_NAME}-{version}"
    path = out / f"{folder}.zip"
    try:
        write_core(source, version, folder, path)
    except SystemExit:
        path.unlink(missing_ok=True)
        raise
    same_untouched(source, path)
    return path


def write_core(source, version, folder, path):
    """core_archive()'s writing, into path."""
    with zipfile.ZipFile(source) as theirs, zipfile.ZipFile(path, "w") as ours:
        top = top_folder(theirs, source.name)
        platform = None
        for info in theirs.infolist():
            rel = info.filename[len(top):]
            if rel.startswith(KODEDOT_LIBRARY):
                fail(f"{source.name} already has {KODEDOT_LIBRARY}")
            if rel == "platform.txt":
                platform = theirs.read(info).decode("utf-8")
            elif rel != "boards.txt" and not rel.startswith("variants/"):
                copy_entry(theirs, info, ours, f"{folder}/{rel}")
        if platform is None:
            fail(f"{source.name} has no platform.txt")
        add_text(ours, f"{folder}/platform.txt", platform_txt(platform, version))
        add_file(ours, f"{folder}/boards.txt", BOARDS)
        for root, under in ((VARIANT, "variants/kodedot"), (LIBRARY, KODEDOT_LIBRARY.rstrip("/"))):
            for file in files_of(root):
                add_file(ours, f"{folder}/{under}/{file.relative_to(root).as_posix()}", file)


def libs_archive(libs, version, out):
    """The libraries under one folder, named as the tool."""
    if not (libs / "flags").is_dir() or not (libs / "sdkconfig").is_file():
        fail(f"{libs} has no flags/ and sdkconfig: is it the ESP32-P4 libraries?")
    path = out / f"{LIBS_TOOL}-{version}.zip"
    with zipfile.ZipFile(path, "w") as ours:
        for file in files_of(libs):
            rel = file.relative_to(libs)
            if rel.parts[0] not in LIBS_SKIPPED:
                add_file(ours, f"{LIBS_TOOL}/{rel.as_posix()}", file)
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--libs", type=Path, required=True, metavar="DIR",
                    help="the ESP32-P4 libraries with the SDK inside")
    ap.add_argument("--out", type=Path, required=True, metavar="DIR",
                    help="where the two archives and the index are written")
    ap.add_argument("--base-url", required=True, metavar="URL",
                    help="where the two archives are served from")
    ap.add_argument("--previous-index", type=Path, metavar="FILE",
                    help="the index published last, whose versions stay listed")
    ap.add_argument("--espressif-index", default=ESPRESSIF_INDEX, metavar="FILE_OR_URL",
                    help="Espressif's index (default: its published address)")
    args = ap.parse_args()
    for needed in (PINS, COMPONENT, BOARDS, VARIANT, LIBRARY):
        if not needed.exists():
            fail(f"{needed} is not there: is {SDK} a kode-sdk checkout?")
    if not args.libs.is_dir():
        fail(f"{args.libs} is not a folder")
    release = read_pins().get("ARDUINO_CORE")
    if not release:
        fail(f"{PINS.relative_to(SDK)} has no ARDUINO_CORE")
    version = sdk_version()
    old = None
    if args.previous_index is not None:
        old = published(json.loads(args.previous_index.read_text(encoding="utf-8")), version)
    espressif = load_index(args.espressif_index)
    _, platform = espressif_platform(espressif, release)
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / platform["archiveFileName"]
        download(platform["url"], source)
        checked(source, platform["checksum"], platform["size"])
        core = archive_of(core_archive(source, version, args.out))
    libs = archive_of(libs_archive(args.libs, version, args.out))
    made = index(espressif, release, version, core, libs, args.base_url, old)
    (args.out / INDEX_FILE).write_text(json.dumps(made, indent=2) + "\n", encoding="utf-8")
    for written in (core.file, libs.file, INDEX_FILE):
        print(f"wrote {args.out / written}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
