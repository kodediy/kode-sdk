#!/usr/bin/env python3
"""Tests for arduino/tools/gen_layer.py: where each function of kode.h goes, the names it refuses, and the files it writes.

    python3 -m unittest discover -s arduino/tools/tests

Needs nothing installed; the sketch's compile skips on a machine with no c++.
"""
import contextlib
import io
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.dont_write_bytecode = True
TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))
import gen_layer  # noqa: E402

HEADER = """
/** @brief Not a prototype: kode_led_off() in a comment. */
bool kode_led_set_color(kode_color_t c);
bool kode_led_off(void);
void kode_exit(void);
void kode_print(const char *fmt, ...) KODE_PRINTF_FORMAT(1, 2);
bool kode_screen_new(const char *title);
kode_widget_t kode_screen_add_text(const char *text);
bool kode_widget_set_text(kode_widget_t p, const char *fmt, ...) KODE_PRINTF_FORMAT(2, 3);
bool kode_draw_line(kode_canvas_t c, int32_t x0, int32_t y0,
                    int32_t x1, int32_t y1, kode_color_t color);
bool kode_canvas_fill(kode_canvas_t c, kode_color_t color);
bool kode_rest(uint32_t parts, const kode_rest_wake_t *wake);
bool kode_rest_end(void);
"""

QUOTED_INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
METHOD_BRIEF = re.compile(r"/\*\* @brief (kode_[a-z0-9_]+)\(\)\. \*/")
SIGNATURE = re.compile(r"^    .*\) const$", re.M)
ALIAS = re.compile(r"^using Kode[A-Za-z0-9]+ = (kode_[a-z0-9_]+_t);", re.M)
ALIAS_NAME = re.compile(r"^using (\w+) = ", re.M)
CLASS_NAME = re.compile(r"^class (\w+)$", re.M)
INCLUDE = re.compile(r"^\s*#\s*include\b", re.M)
KEYWORD2 = re.compile(r"^(\w+)\tKEYWORD2$", re.M)
VARIANT = gen_layer.SDK / "arduino" / "variants" / "kodedot" / "pins_arduino.h"
# The calls a sketch makes leaning on the defaults of DEFAULTS.
DEFAULTS_SKETCH = """void setup(void)
{
    (void)Kode.http.get("u");
    (void)Kode.wifi.connect();
    (void)Kode.screen.show();
    (void)Kode.touch.read();
    (void)Kode.event.wait("x", 1);
    (void)Kode.rest.begin(0);
    (void)Kode.screen.addValue("1");
}

void loop(void)
{
}
"""
# A header with an event's name, a function of a group, two of no group and a call with trailing pointers.
TABLED = """
#define KODE_EVENT_IR_RECEIVED kode_event_ir_received()
const char *kode_event_ir_received(void);
bool kode_event_wait(const char *event, uint32_t ms, kode_event_t *out);
bool kode_i2c_probe(uint8_t addr);
void kode_delay(uint32_t ms);
bool kode_led_off(void);
bool kode_touch_read(int16_t *x, int16_t *y);
"""
# The variant's lines that bring the layer: C++ linkage, and the header by a path from the variant's folder.
LAYER_INCLUDE = re.compile(r'#ifdef __cplusplus\nextern "C\+\+" \{\n#include "([^"]+)"\n\}\n#endif\n')


def placed(text):
    """{function: (owner, method)} for a header's text."""
    return {fn.name: gen_layer.place(fn) for fn in gen_layer.functions(text)}


def include_dirs():
    """The folders that hold kode.h and every header it includes in quotes, sdkconfig.h aside."""
    folders = [path for path in (gen_layer.SDK / "components").glob("**/include")
               if path.is_dir() and "managed_components" not in path.parts]
    found = []
    waiting = [gen_layer.SDK / gen_layer.HEADER]
    seen = set()
    while waiting:
        path = waiting.pop()
        if path in seen:
            continue
        seen.add(path)
        if path.parent not in found:
            found.append(path.parent)
        for name in QUOTED_INCLUDE.findall(path.read_text(encoding="utf-8")):
            if name == "sdkconfig.h":
                continue
            holders = [folder for folder in folders if (folder / name).is_file()]
            if not holders:
                raise AssertionError(f"{path.name} includes {name}, which no include folder holds")
            waiting.append(holders[0] / name)
    return found


def compile_sketch(case, sketch):
    """Build a sketch as C++ the way the board does, the variant's pins_arduino.h above it; skip with no c++."""
    compiler = shutil.which("c++")
    if compiler is None:
        case.skipTest("this machine has no c++")
    folder = sketch.parent
    (folder / "sdkconfig.h").write_text("", encoding="utf-8")
    # What the board's Arduino.h does: the variant's pins_arduino.h, inside extern "C", above the sketch.
    arduino_h = folder / "Arduino.h"
    arduino_h.write_text(f'extern "C" {{\n#include "{VARIANT}"\n}}\n', encoding="utf-8")
    # A GNU dialect on a Unix host predefines unix and linux, which the board's compiler does not:
    # Kode.time.unix() would read as Kode.time.1().
    flags = ["-U", "unix", "-U", "linux", "-include", str(arduino_h), "-I", str(folder)]
    for include in include_dirs():
        flags += ["-I", str(include)]
    built = subprocess.run([compiler, "-std=gnu++2b", "-fsyntax-only", "-x", "c++"] + flags + [str(sketch)],
                           capture_output=True, text=True)
    case.assertEqual(built.returncode, 0, built.stdout + built.stderr)


TABLED_LEFT_OUT = {"kode_delay": "Arduino's delay()"}
TABLED_GROUPS = {"i2c": "Arduino's Wire"}
TABLED_DEFAULTS = {"kode_touch_read": {"x": "nullptr", "y": "nullptr"}}


@contextlib.contextmanager
def tables(left_out=TABLED_LEFT_OUT, groups=TABLED_GROUPS, defaults=TABLED_DEFAULTS):
    """LEFT_OUT, LEFT_OUT_GROUPS and DEFAULTS as a synthetic header needs them: TABLED's unless a case says."""
    with mock.patch.dict(gen_layer.LEFT_OUT, left_out, clear=True), \
            mock.patch.dict(gen_layer.LEFT_OUT_GROUPS, groups, clear=True), \
            mock.patch.dict(gen_layer.DEFAULTS, defaults, clear=True):
        yield


class Rule(unittest.TestCase):
    def test_a_function_of_a_group_is_that_groups_method(self):
        self.assertEqual(placed(HEADER)["kode_led_set_color"], ("led", "setColor"))
        self.assertEqual(placed(HEADER)["kode_led_off"], ("led", "off"))

    def test_a_function_of_no_group_is_the_objects_own(self):
        self.assertEqual(placed(HEADER)["kode_exit"], ("", "exit"))

    def test_a_handles_first_parameter_makes_a_method_of_the_handle(self):
        where = placed(HEADER)
        self.assertEqual(where["kode_widget_set_text"], ("kode_widget_t", "setText"))
        self.assertEqual(where["kode_canvas_fill"], ("kode_canvas_t", "fill"))
        self.assertEqual(where["kode_draw_line"], ("kode_canvas_t", "drawLine"))

    def test_the_renamed_ones(self):
        where = placed(HEADER)
        self.assertEqual(where["kode_screen_new"], ("screen", "show"))
        self.assertEqual(where["kode_print"], ("", "printf"))
        self.assertEqual(where["kode_rest"], ("rest", "begin"))
        self.assertEqual(where["kode_rest_end"], ("rest", "end"))

    def test_a_comment_is_not_a_prototype(self):
        names = [fn.name for fn in gen_layer.functions(HEADER)]
        self.assertEqual(names.count("kode_led_off"), 1)

    def test_a_prototype_on_the_first_line_is_read(self):
        names = [fn.name for fn in gen_layer.functions("bool kode_first(void);\nbool kode_second(void);\n")]
        self.assertEqual(names, ["kode_first", "kode_second"])

    def test_a_prototype_on_two_lines_is_read_whole(self):
        line = [fn for fn in gen_layer.functions(HEADER) if fn.name == "kode_draw_line"][0]
        self.assertEqual([param.name for param in line.params], ["c", "x0", "y0", "x1", "y1", "color"])


class Body(unittest.TestCase):
    def lines(self, name):
        fn = [fn for fn in gen_layer.functions(HEADER) if fn.name == name][0]
        owner, method = gen_layer.place(fn)
        handle = owner if owner in gen_layer.HANDLES else None
        return gen_layer.method_lines(gen_layer.Method(method, fn), handle)

    def test_a_method_is_the_call_alone(self):
        self.assertEqual(self.lines("kode_led_set_color"), [
            "    /** @brief kode_led_set_color(). */",
            "    bool setColor(kode_color_t c) const",
            "    {",
            "        return kode_led_set_color(c);",
            "    }",
        ])

    def test_a_void_function_returns_nothing(self):
        self.assertIn("        kode_exit();", self.lines("kode_exit"))
        self.assertIn("    void exit(void) const", self.lines("kode_exit"))

    def test_a_format_is_forwarded_with_its_arguments(self):
        lines = self.lines("kode_print")
        self.assertIn("    template <typename... Args>", lines)
        self.assertIn("    void printf(const char *fmt, Args... args) const", lines)
        self.assertIn("        kode_print(fmt, args...);", lines)

    def test_a_handles_method_passes_the_handle_first(self):
        lines = self.lines("kode_canvas_fill")
        self.assertIn("    bool fill(kode_color_t color) const", lines)
        self.assertIn("        return kode_canvas_fill(handle, color);", lines)

    def test_a_handle_answered_is_its_class(self):
        lines = self.lines("kode_screen_add_text")
        self.assertIn("    KodeWidget addText(const char *text) const", lines)
        self.assertIn("        return KodeWidget(kode_screen_add_text(text));", lines)


class Refused(unittest.TestCase):
    def refusal(self, text):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as stopped:
                gen_layer.layout(gen_layer.functions(text))
        return str(stopped.exception.code)

    def test_a_function_pointer_parameter(self):
        said = self.refusal("\nbool kode_led_off(void);\nbool kode_a(void (*cb)(void));\n")
        self.assertIn("kode_a()", said)

    def test_an_unknown_trailing_attribute(self):
        said = self.refusal("\nbool kode_b(void) KODE_NORETURN;\n")
        self.assertIn("kode_b()", said)

    def test_a_return_type_on_its_own_line(self):
        said = self.refusal("\nbool\nkode_g(void);\n")
        self.assertIn("kode_g()", said)

    def test_a_macro_before_the_return_type(self):
        said = self.refusal("\nKODE_DEPRECATED bool kode_e(void);\n")
        self.assertIn("kode_e()", said)
        self.assertIn("return type", said)

    def test_a_cpp_keyword(self):
        said = self.refusal("\nbool kode_file_new(const char *path);\n")
        self.assertIn("kode_file_new()", said)
        self.assertIn("C++ keyword", said)

    def test_two_functions_with_one_name(self):
        said = self.refusal("\nbool kode_screen_new(void);\nbool kode_screen_show(void);\n")
        self.assertIn("kode_screen_show()", said)
        self.assertIn("kode_screen_new()", said)

    def test_a_function_named_as_a_group(self):
        said = self.refusal("\nbool kode_led(void);\nbool kode_led_off(void);\n")
        self.assertIn("kode_led()", said)
        self.assertIn("a group", said)


class Names(unittest.TestCase):
    """No two names the layer declares at file scope are one, a group's class and a type's alias above all."""

    def refusal(self, text):
        fns = gen_layer.functions(text)
        owners = gen_layer.layout(fns)
        groups = [owner for owner in owners if owner and owner not in gen_layer.HANDLES]
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as stopped:
                gen_layer.check_names(groups, gen_layer.alias_types(text, fns))
        return str(stopped.exception.code)

    def test_a_groups_class_is_never_a_types_alias(self):
        text = "\ntypedef struct\n{\n    int32_t x;\n} kode_imu_t;\nbool kode_imu_read(kode_imu_t *out);\n"
        fns = gen_layer.functions(text)
        self.assertEqual(gen_layer.group_class_name("imu"), "Kode_imu")
        self.assertEqual([gen_layer.alias_name(c_type) for c_type in gen_layer.alias_types(text, fns)], ["KodeImu"])
        gen_layer.check_names(["imu"], gen_layer.alias_types(text, fns))

    def test_a_group_class_spelled_as_an_alias_is_refused(self):
        text = "\ntypedef struct\n{\n    int32_t x;\n} kode_imu_t;\nbool kode_imu_read(kode_imu_t *out);\n"
        with mock.patch.object(gen_layer, "group_class_name", lambda group: "Kode" + gen_layer.pascal([group])):
            said = self.refusal(text)
        self.assertIn("KodeImu", said)
        self.assertIn("kode_imu_t", said)
        self.assertIn("Kode.imu", said)

    def test_a_type_whose_alias_is_the_object_is_refused(self):
        said = self.refusal("\ntypedef uint8_t kode___t;\nbool kode_led_off(void);\n")
        self.assertIn("the object", said)
        self.assertIn("kode___t", said)

    def test_a_type_whose_alias_is_a_handles_class_is_refused(self):
        with mock.patch.dict(gen_layer.HANDLES, {"kode_screen_t": "KodeTag"}):
            said = self.refusal("\ntypedef struct\n{\n    int32_t x;\n} kode_tag_t;\nbool kode_led_off(void);\n")
        self.assertIn("KodeTag", said)


def stopped(case, text):
    """What the run says when it stops on a header's tables."""
    with contextlib.redirect_stderr(io.StringIO()):
        with case.assertRaises(SystemExit) as stop:
            gen_layer.layer_functions(text)
    return str(stop.exception.code)


class LeftOut(unittest.TestCase):
    """A function the layer leaves out has no method; a stale line of a table stops the run."""

    def test_by_name_by_group_and_an_events_name(self):
        with tables():
            out = gen_layer.left_out(gen_layer.functions(TABLED), TABLED)
            kept = [fn.name for fn in gen_layer.layer_functions(TABLED)]
        self.assertEqual(out, {
            "kode_event_ir_received": "KODE_EVENT_IR_RECEIVED",
            "kode_i2c_probe": "Arduino's Wire",
            "kode_delay": "Arduino's delay()",
        })
        self.assertEqual(kept, ["kode_event_wait", "kode_led_off", "kode_touch_read"])

    def test_a_name_that_is_no_function(self):
        with tables(left_out={"kode_delay": "Arduino's delay()", "kode_gone": "nothing"}):
            said = stopped(self, TABLED)
        self.assertIn("kode_gone", said)
        self.assertIn("LEFT_OUT", said)

    def test_a_group_with_no_function(self):
        with tables(groups={"i2c": "Arduino's Wire", "spi": "Arduino's SPI"}):
            said = stopped(self, TABLED)
        self.assertIn("spi", said)
        self.assertIn("LEFT_OUT_GROUPS", said)

    def test_an_events_name_with_no_constant(self):
        text = TABLED.replace("#define KODE_EVENT_IR_RECEIVED kode_event_ir_received()\n", "")
        with tables():
            said = stopped(self, text)
        self.assertIn("kode_event_ir_received()", said)
        self.assertIn("KODE_EVENT_IR_RECEIVED", said)

    def test_an_events_name_whose_constant_is_another_call(self):
        text = TABLED.replace("KODE_EVENT_IR_RECEIVED kode_event_ir_received()", "KODE_EVENT_IR_RECEIVED \"ir\"")
        with tables():
            said = stopped(self, text)
        self.assertIn("KODE_EVENT_IR_RECEIVED", said)

    def test_a_question_that_takes_a_parameter_is_no_events_name(self):
        text = TABLED + "const char *kode_event_name(uint32_t index);\n"
        with tables():
            kept = [fn.name for fn in gen_layer.layer_functions(text)]
        self.assertIn("kode_event_name", kept)


class Defaults(unittest.TestCase):
    """A trailing parameter that is NULL in the common case has a default; a wrong line stops the run."""

    def test_the_declaration_carries_the_default(self):
        with tables():
            fn = [fn for fn in gen_layer.layer_functions(TABLED) if fn.name == "kode_touch_read"][0]
            lines = gen_layer.method_lines(gen_layer.Method("read", fn), None)
        self.assertIn("    bool read(int16_t *x = nullptr, int16_t *y = nullptr) const", lines)
        self.assertIn("        return kode_touch_read(x, y);", lines)

    def test_a_function_that_is_not_there(self):
        with tables(defaults={"kode_gone": {"x": "nullptr"}}):
            said = stopped(self, TABLED)
        self.assertIn("kode_gone", said)
        self.assertIn("DEFAULTS", said)

    def test_a_parameter_that_is_not_there(self):
        with tables(defaults={"kode_touch_read": {"z": "nullptr"}}):
            said = stopped(self, TABLED)
        self.assertIn("kode_touch_read()", said)
        self.assertIn("'z'", said)

    def test_a_function_that_is_left_out(self):
        with tables(defaults={"kode_delay": {"ms": "0"}}):
            said = stopped(self, TABLED)
        self.assertIn("kode_delay()", said)
        self.assertIn("left out", said)

    def test_parameters_that_are_not_the_last(self):
        with tables(defaults={"kode_touch_read": {"x": "nullptr"}}):
            said = stopped(self, TABLED)
        self.assertIn("kode_touch_read()", said)
        self.assertIn("last", said)


class Generated(unittest.TestCase):
    def test_check_fails_on_a_stale_header(self):
        with tempfile.TemporaryDirectory() as tmp:
            tree = Path(tmp)
            script = tree / "arduino" / "tools" / "gen_layer.py"
            script.parent.mkdir(parents=True)
            shutil.copy(TOOLS / "gen_layer.py", script)
            for needed in gen_layer.INPUTS:
                (tree / needed).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy(gen_layer.SDK / needed, tree / needed)
            wrote = subprocess.run([sys.executable, str(script)], capture_output=True, text=True)
            self.assertEqual(wrote.returncode, 0, wrote.stdout + wrote.stderr)
            fresh = subprocess.run([sys.executable, str(script), "--check"], capture_output=True, text=True)
            self.assertEqual(fresh.returncode, 0, fresh.stdout + fresh.stderr)
            with (tree / gen_layer.LIBRARY.relative_to(gen_layer.SDK) / gen_layer.OUT_HEADER).open("a") as out:
                out.write(" ")
            stale = subprocess.run([sys.executable, str(script), "--check"], capture_output=True, text=True)
            self.assertEqual(stale.returncode, 1, stale.stdout + stale.stderr)
            self.assertIn("KodeArduino.h is stale", stale.stdout)

    def test_the_object_is_kode_and_there_is_no_dot(self):
        text = (gen_layer.LIBRARY / gen_layer.OUT_HEADER).read_text(encoding="utf-8")
        self.assertIn("\ninline Kode_Sdk Kode; ", text)
        self.assertIsNone(re.search(r"\bDot\b", text))
        self.assertIsNone(re.search(r"\bDot\b", (gen_layer.LIBRARY / gen_layer.OUT_KEYWORDS).read_text(encoding="utf-8")))

    def test_no_class_is_a_types_alias(self):
        text = (gen_layer.LIBRARY / gen_layer.OUT_HEADER).read_text(encoding="utf-8")
        classes = CLASS_NAME.findall(text)
        aliases = ALIAS_NAME.findall(text)
        self.assertIn("Kode_imu", classes)
        self.assertIn("KodeImu", aliases)
        self.assertEqual(len(classes), len(set(classes)))
        self.assertEqual(sorted(set(classes) & set(aliases)), [])
        self.assertNotIn("Kode", classes + aliases)

    def test_the_variant_brings_the_layer_with_cpp_linkage(self):
        text = VARIANT.read_text(encoding="utf-8")
        found = LAYER_INCLUDE.search(text)
        self.assertIsNotNone(found, "pins_arduino.h does not include the layer inside extern \"C++\"")
        self.assertEqual((VARIANT.parent / found.group(1)).resolve(), (gen_layer.LIBRARY / gen_layer.OUT_HEADER).resolve())
        self.assertLess(found.end(), text.index("#endif /* Pins_Arduino_h */"))

    def test_every_function_of_kode_h_is_a_method_or_left_out_never_both(self):
        text = (gen_layer.SDK / gen_layer.HEADER).read_text(encoding="utf-8")
        names = [fn.name for fn in gen_layer.functions(text)]
        out = gen_layer.left_out(gen_layer.functions(text), text)
        methods = METHOD_BRIEF.findall((gen_layer.LIBRARY / gen_layer.OUT_HEADER).read_text(encoding="utf-8"))
        self.assertEqual(len(names), len(set(names)))
        self.assertEqual(len(methods), len(set(methods)))
        self.assertTrue(out)
        self.assertEqual(sorted(set(methods) & set(out)), [])
        self.assertEqual(set(names), set(methods) | set(out))

    def test_the_traps_have_no_method(self):
        text = (gen_layer.SDK / gen_layer.HEADER).read_text(encoding="utf-8")
        methods = set(METHOD_BRIEF.findall((gen_layer.LIBRARY / gen_layer.OUT_HEADER).read_text(encoding="utf-8")))
        for name in ("kode_delay", "kode_uptime_ms", "kode_random", "kode_pin_write", "kode_pin_read",
                     "kode_pin_read_analog", "kode_pin_write_pwm"):
            self.assertNotIn(name, methods)
        for fn in gen_layer.functions(text):
            if fn.name.startswith(("kode_i2c_", "kode_spi_", "kode_uart_")):
                self.assertNotIn(fn.name, methods)
        for name in ("kode_event_ir_received", "kode_event_wifi_connected", "kode_event_ble_pair_code",
                     "kode_event_touch_released"):
            self.assertNotIn(name, methods)
        for name in ("kode_pin_power_3v3", "kode_pin_power_5v", "kode_event_add_listener",
                     "kode_event_remove_listener", "kode_event_wait"):
            self.assertIn(name, methods)
        self.assertNotIn("Kode_uptime", CLASS_NAME.findall((gen_layer.LIBRARY / gen_layer.OUT_HEADER).read_text(encoding="utf-8")))

    def test_the_declarations_carry_the_defaults(self):
        text = (gen_layer.LIBRARY / gen_layer.OUT_HEADER).read_text(encoding="utf-8")
        self.assertIn("const char *get(const char *url, const kode_http_header_t *headers = nullptr) const", text)
        self.assertIn("bool connect(const char *ssid = nullptr, const char *password = nullptr) const", text)
        self.assertIn("bool show(const char *title = nullptr) const", text)

    def test_the_left_out_names_are_no_keywords(self):
        named = set(KEYWORD2.findall((gen_layer.LIBRARY / gen_layer.OUT_KEYWORDS).read_text(encoding="utf-8")))
        for name in ("ms", "readAnalog", "writePwm", "writeRead", "transfer", "probe", "delay", "random",
                     "uptime", "i2c", "spi", "uart", "irReceived", "wifiConnected", "touchReleased"):
            self.assertNotIn(name, named)
        for name in ("begin", "read", "write", "end", "power3v3", "wait", "header", "event"):
            self.assertIn(name, named)
        self.assertNotIn("pin", named)

    def test_the_header_power_is_under_header(self):
        # kode_pin_power_*() feed the expansion header and are all the layer keeps of kode_pin_*().
        text = (gen_layer.LIBRARY / gen_layer.OUT_HEADER).read_text(encoding="utf-8")
        classes = CLASS_NAME.findall(text)
        self.assertIn("Kode_header", classes)
        self.assertNotIn("Kode_pin", classes)
        self.assertIn("/** @brief Kode.header: kode_pin_power_3v3() and kode_pin_power_5v(). */", text)
        self.assertIn("/**< kode_pin_power_3v3() and kode_pin_power_5v(). */", text)
        self.assertNotIn("kode_header_", text)
        self.assertIn("/** @brief Kode.led: the kode_led_*() calls. */", text)
        self.assertIn("/**< The kode_rest_*() calls. */", text)

    def test_the_start_of_the_sdk_is_not_a_method(self):
        # The board's variant starts the SDK before setup(): a sketch has no start to call.
        methods = METHOD_BRIEF.findall((gen_layer.LIBRARY / gen_layer.OUT_HEADER).read_text(encoding="utf-8"))
        self.assertNotIn("kode_init", methods)
        self.assertNotIn("kode_up", methods)

    def test_every_type_a_signature_names_has_its_alias(self):
        text = (gen_layer.LIBRARY / gen_layer.OUT_HEADER).read_text(encoding="utf-8")
        named = set()
        for line in SIGNATURE.findall(text):
            named.update(gen_layer.NAMED_TYPE.findall(line))
        self.assertTrue(named)
        self.assertEqual(sorted(named - set(ALIAS.findall(text)) - set(gen_layer.HANDLES)), [])

    def test_all_calls_sketch_defines_no_template_before_its_first_function(self):
        # Arduino writes its prototypes above the first function the sketch defines: a template
        # header there would be cut from its function. The template is declared above and defined below.
        lines = gen_layer.all_calls("AllCalls.ino").splitlines()
        first = lines.index("{") - 1
        self.assertEqual(lines[first], "static void all_calls(void)")
        self.assertEqual(lines[:first].count("static T zero(void);"), 1)
        self.assertEqual(lines[first:].count("static T zero(void)"), 1)

    def test_all_calls_sketch_includes_nothing(self):
        self.assertIsNone(INCLUDE.search(gen_layer.all_calls("AllCalls.ino")))

    def test_all_calls_sketch_names_no_left_out_function(self):
        text = (gen_layer.SDK / gen_layer.HEADER).read_text(encoding="utf-8")
        sketch = gen_layer.all_calls("AllCalls.ino")
        self.assertNotIn("Kode.delay(", sketch)
        self.assertNotIn("Kode.uptime.", sketch)
        self.assertNotIn("Kode.i2c.", sketch)
        self.assertNotIn("Kode.event.wifiConnected(", sketch)
        self.assertIn("Kode.event.wait(", sketch)
        calls = re.findall(r"^    (?:\(void\))?(?:Kode|widget|canvas)\.", sketch, re.M)
        fns = gen_layer.functions(text)
        self.assertEqual(len(calls), len(fns) - len(gen_layer.left_out(fns, text)))

    def test_all_calls_sketch_compiles_as_cpp(self):
        with tempfile.TemporaryDirectory() as tmp:
            sketch = Path(tmp) / "AllCalls.ino"
            done = subprocess.run([sys.executable, str(TOOLS / "gen_layer.py"), "--all-calls", str(sketch)],
                                  capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            compile_sketch(self, sketch)

    def test_calls_that_lean_on_the_defaults_compile_as_cpp(self):
        with tempfile.TemporaryDirectory() as tmp:
            sketch = Path(tmp) / "Defaults.ino"
            sketch.write_text(DEFAULTS_SKETCH, encoding="utf-8")
            compile_sketch(self, sketch)


if __name__ == "__main__":
    unittest.main()
