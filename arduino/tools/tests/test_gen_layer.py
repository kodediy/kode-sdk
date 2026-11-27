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
# A header with a narrow integer of each kind by value, one of them in a handle's method beside a pointer to one.
NARROW = """
bool kode_led_set_level(uint8_t level);
bool kode_audio_beep(uint16_t hz, uint32_t ms);
bool kode_ble_nudge(int8_t dx, int16_t dy);
bool kode_draw_dot(kode_canvas_t c, uint8_t size, const uint8_t *mask);
"""
# What a sketch does with a handle: a global with no value, one from a call, a copy, an assignment and a test.
HANDLES_SKETCH = """KodeWidget s_line; // None yet.

void setup(void)
{
    KodeWidget made = Kode.screen.addText("t");
    KodeWidget copy = made;
    s_line = copy;
    if ( s_line )
    {
        (void)s_line.setText("made");
    }
}

void loop(void)
{
}
"""
# Two ways to take a number, or another handle, for a handle: neither builds.
NUMBER_AS_WIDGET = "void setup(void)\n{\n    KodeWidget w = 5;\n    (void)w;\n}\n\nvoid loop(void)\n{\n}\n"
WIDGET_AS_CANVAS = ("void setup(void)\n{\n    KodeCanvas c(Kode.screen.addText(\"t\"));\n    (void)c;\n}\n\n"
                    "void loop(void)\n{\n}\n")
# What each fitting function answers, as (function, number given, number answered): the ends of each range and past them.
FITTED = [
    ("kode_arduino_fit_u8", -1, 0),
    ("kode_arduino_fit_u8", 0, 0),
    ("kode_arduino_fit_u8", 255, 255),
    ("kode_arduino_fit_u8", 256, 255),
    ("kode_arduino_fit_u8", 70000, 255),
    ("kode_arduino_fit_u16", -1, 0),
    ("kode_arduino_fit_u16", 65535, 65535),
    ("kode_arduino_fit_u16", 65536, 65535),
    ("kode_arduino_fit_i8", -129, -128),
    ("kode_arduino_fit_i8", -128, -128),
    ("kode_arduino_fit_i8", 127, 127),
    ("kode_arduino_fit_i8", 128, 127),
    ("kode_arduino_fit_i16", -32769, -32768),
    ("kode_arduino_fit_i16", -32768, -32768),
    ("kode_arduino_fit_i16", 32767, 32767),
    ("kode_arduino_fit_i16", 32768, 32767),
]
FIT_DEFINED = re.compile(r"^inline u?int(?:8|16)_t (kode_arduino_fit_[a-z0-9]+)\(int32_t value\)$", re.M)
FIT_USED = re.compile(r"\b(kode_arduino_fit_[a-z0-9]+)\(")
# A narrow integer declared by value in a method: the type with no `*` after it.
NARROW_BY_VALUE = re.compile(r"\bu?int(?:8|16)_t [A-Za-z_]\w*\s*[,)=]")
# A format function's three methods: the format with arguments, a text alone, and an object that holds one.
PRINTF_METHODS = [
    "    /** @brief kode_print(). */",
    "    template <typename First, typename... Rest>",
    "    void printf(const char *fmt, const First &first, const Rest &...rest) const",
    "    {",
    "        kode_print(fmt, kode_arduino_text(first, 0), kode_arduino_text(rest, 0)...);",
    "    }",
    "",
    "    /** @brief kode_print() with a text as it is: a '%' in it is shown, not read. */",
    "    void printf(const char *text) const",
    "    {",
    "        if ( nullptr == text )",
    "        {",
    "            kode_print(text);",
    "        }",
    "        else",
    "        {",
    "            kode_print(\"%s\", text);",
    "        }",
    "    }",
    "",
    "    /** @brief kode_print() with the text an object holds, a String for one, as it is. */",
    "    template <typename Text>",
    "    auto printf(const Text &text) const -> decltype(text.c_str(), void())",
    "    {",
    "        kode_print(\"%s\", text.c_str());",
    "    }",
]
SET_TEXT_METHODS = [
    "    /** @brief kode_widget_set_text(). */",
    "    template <typename First, typename... Rest>",
    "    bool setText(const char *fmt, const First &first, const Rest &...rest) const",
    "    {",
    "        return kode_widget_set_text(handle, fmt, kode_arduino_text(first, 0), kode_arduino_text(rest, 0)...);",
    "    }",
    "",
    "    /** @brief kode_widget_set_text() with a text as it is: a '%' in it is shown, not read. */",
    "    bool setText(const char *text) const",
    "    {",
    "        bool answer;",
    "        if ( nullptr == text )",
    "        {",
    "            answer = kode_widget_set_text(handle, text);",
    "        }",
    "        else",
    "        {",
    "            answer = kode_widget_set_text(handle, \"%s\", text);",
    "        }",
    "        return answer;",
    "    }",
    "",
    "    /** @brief kode_widget_set_text() with the text an object holds, a String for one, as it is. */",
    "    template <typename Text>",
    "    auto setText(const Text &text) const -> decltype(text.c_str(), bool())",
    "    {",
    "        return kode_widget_set_text(handle, \"%s\", text.c_str());",
    "    }",
]
# What a format's arguments go through: an object that holds a text as its characters, anything else as it is.
TEXT_HELPERS = [
    "",
    "/**",
    " * @brief A format's argument as kode.h takes it: the characters of an object that holds a text.",
    " * @param value The argument, a String for one.",
    " * @return Its characters.",
    " */",
    "template <typename T>",
    "inline auto kode_arduino_text(const T &value, int) -> decltype(value.c_str())",
    "{",
    "    return value.c_str();",
    "}",
    "",
    "/**",
    " * @brief A format's argument as kode.h takes it: any other argument, as it is.",
    " * @param value The argument.",
    " * @return The argument.",
    " */",
    "template <typename T>",
    "inline const T &kode_arduino_text(const T &value, long)",
    "{",
    "    return value;",
    "}",
]
# A program that stands in for kode.h's two format calls and shows, a line each, what they were handed.
# Text stands for Arduino's String; the long one is past what the core's String keeps inside itself.
TEXTS_PROGRAM = r"""#include <cinttypes>
#include <cstdarg>
#include <cstdio>

static const uint32_t SHOWN_MAX = 128U; /**< Room for what a call shows. */
static char s_shown[SHOWN_MAX];         /**< What the last call showed. */

/** @brief The format and arguments a call was handed, as they show, or "(null)" for no format. */
static void show(const char *fmt, va_list args)
{
    if ( nullptr == fmt )
    {
        (void)std::snprintf(s_shown, SHOWN_MAX, "(null)");
    }
    else
    {
        (void)std::vsnprintf(s_shown, SHOWN_MAX, fmt, args);
    }
}

extern "C" void kode_print(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    show(fmt, args);
    va_end(args);
}

extern "C" bool kode_widget_set_text(kode_widget_t p, const char *fmt, ...)
{
    va_list args;
    (void)p;
    va_start(args, fmt);
    show(fmt, args);
    va_end(args);
    return true;
}

/** @brief An object that holds a text, as Arduino's String does. */
class Text
{
public:
    explicit Text(const char *from) : chars(from)
    {
    }

    const char *c_str(void) const
    {
        return chars;
    }

private:
    const char *chars;
};

/** @brief What the last call showed, on a line of its own. */
static void said(void)
{
    (void)std::printf("%s\n", s_shown);
}

int main(void)
{
    const KodeWidget widget(static_cast<kode_widget_t>(1U));
    const Text name("Ada");
    const Text longer("a text of more than fourteen characters");
    const char *none = nullptr;

    (void)widget.setText("50%");
    said();
    (void)widget.setText("100% done");
    said();
    (void)widget.setText(name);
    said();
    (void)widget.setText("hi %s, %" PRId32 " %%", name, static_cast<int32_t>(7));
    said();
    (void)widget.setText("%" PRIu32 " of %s", static_cast<uint32_t>(3U), "five");
    said();
    (void)widget.setText(none);
    said();
    (void)widget.setText(longer);
    said();
    (void)widget.setText("<%s>", longer);
    said();

    Kode.printf("50%");
    said();
    Kode.printf("100% done");
    said();
    Kode.printf(name);
    said();
    Kode.printf("hi %s, %" PRId32 " %%", name, static_cast<int32_t>(7));
    said();
    Kode.printf("%" PRIu32 " of %s", static_cast<uint32_t>(3U), "five");
    said();
    Kode.printf(none);
    said();
    Kode.printf(longer);
    said();
    Kode.printf("<%s>", longer);
    said();
    return 0;
}
"""
# What TEXTS_PROGRAM shows, once through the widget and once through Kode.printf().
TEXTS_SHOWN = [
    "50%",
    "100% done",
    "Ada",
    "hi Ada, 7 %",
    "3 of five",
    "(null)",
    "a text of more than fourteen characters",
    "<a text of more than fourteen characters>",
] * 2
# A number is no text: it does not build.
NUMBER_AS_TEXT = ("void setup(void)\n{\n    KodeWidget widget;\n    (void)widget.setText(42);\n}\n\n"
                  "void loop(void)\n{\n}\n")
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


def build_sketch(case, sketch, output):
    """Build a sketch as C++ the way the board does, the variant's pins_arduino.h above it; skip with no c++.

    With no output the build checks the syntax alone; with one it links a program there.
    """
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
    if output is None:
        flags.append("-fsyntax-only")
    else:
        flags += ["-o", str(output)]
    return subprocess.run([compiler, "-std=gnu++2b", "-x", "c++"] + flags + [str(sketch)],
                          capture_output=True, text=True)


def compile_sketch(case, sketch):
    """Build a sketch as C++ the way the board does: it must build."""
    built = build_sketch(case, sketch, None)
    case.assertEqual(built.returncode, 0, built.stdout + built.stderr)


def refused_sketch(case, sketch):
    """Build a sketch as C++ the way the board does: it must not build. Answers what the compiler said."""
    built = build_sketch(case, sketch, None)
    case.assertNotEqual(built.returncode, 0, f"{sketch.name} builds")
    return built.stderr


def run_program(case, source):
    """Build a C++ program the way the board builds a sketch, run it, and answer what it printed."""
    program = source.parent / "program"
    built = build_sketch(case, source, program)
    case.assertEqual(built.returncode, 0, built.stdout + built.stderr)
    ran = subprocess.run([str(program)], capture_output=True, text=True)
    case.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
    return ran.stdout


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

    def test_a_format_is_three_methods_a_format_a_text_and_an_object(self):
        self.assertEqual(self.lines("kode_print"), PRINTF_METHODS)
        self.assertEqual(self.lines("kode_widget_set_text"), SET_TEXT_METHODS)

    def test_a_format_method_has_no_ternary_and_one_return(self):
        for name in ("kode_print", "kode_widget_set_text"):
            lines = self.lines(name)
            self.assertNotIn("?", "\n".join(lines))
            bodies = "\n".join(lines).split("\n\n")
            for body in bodies:
                self.assertLessEqual(body.count("return "), 1, body)

    def test_the_text_helpers_come_only_with_a_format(self):
        helpers = gen_layer.texts(gen_layer.layout(gen_layer.functions(HEADER)))
        self.assertEqual(helpers, TEXT_HELPERS)
        self.assertEqual(gen_layer.texts(gen_layer.layout(gen_layer.functions("\nbool kode_led_off(void);\n"))), [])

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

    def test_a_handle_after_the_first_parameter(self):
        said = self.refusal("\nbool kode_screen_focus(uint32_t index, kode_widget_t w);\n")
        self.assertIn("kode_screen_focus()", said)
        self.assertIn("raw()", said)

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


class Fit(unittest.TestCase):
    """A narrow integer by value is taken as int32_t and brought into its type's range for the call."""

    def lines(self, text, name):
        fn = [fn for fn in gen_layer.functions(text) if fn.name == name][0]
        owner, method = gen_layer.place(fn)
        handle = owner if owner in gen_layer.HANDLES else None
        return gen_layer.method_lines(gen_layer.Method(method, fn), handle)

    def test_each_narrow_type_is_taken_as_int32_and_fitted(self):
        self.assertIn("    bool setLevel(int32_t level) const", self.lines(NARROW, "kode_led_set_level"))
        self.assertIn("        return kode_led_set_level(kode_arduino_fit_u8(level));",
                      self.lines(NARROW, "kode_led_set_level"))
        beep = self.lines(NARROW, "kode_audio_beep")
        self.assertIn("    bool beep(int32_t hz, uint32_t ms) const", beep)
        self.assertIn("        return kode_audio_beep(kode_arduino_fit_u16(hz), ms);", beep)
        nudge = self.lines(NARROW, "kode_ble_nudge")
        self.assertIn("    bool nudge(int32_t dx, int32_t dy) const", nudge)
        self.assertIn("        return kode_ble_nudge(kode_arduino_fit_i8(dx), kode_arduino_fit_i16(dy));", nudge)

    def test_a_handles_method_fits_its_narrow_parameter_and_keeps_a_pointer(self):
        dot = self.lines(NARROW, "kode_draw_dot")
        self.assertIn("    bool drawDot(int32_t size, const uint8_t *mask) const", dot)
        self.assertIn("        return kode_draw_dot(handle, kode_arduino_fit_u8(size), mask);", dot)

    def test_the_fitting_functions_are_the_ones_used(self):
        every = "\n".join(gen_layer.fitting(gen_layer.layout(gen_layer.functions(NARROW))))
        self.assertEqual(FIT_DEFINED.findall(every), ["kode_arduino_fit_u8", "kode_arduino_fit_u16",
                                                      "kode_arduino_fit_i8", "kode_arduino_fit_i16"])
        one = "\n".join(gen_layer.fitting(gen_layer.layout(gen_layer.functions(
            "\nbool kode_audio_beep(uint16_t hz, uint32_t ms);\n"))))
        self.assertEqual(FIT_DEFINED.findall(one), ["kode_arduino_fit_u16"])
        self.assertEqual(gen_layer.fitting(gen_layer.layout(gen_layer.functions("\nbool kode_led_off(void);\n"))), [])

    def test_a_fitting_function_has_no_ternary_and_no_bare_limit(self):
        text = "\n".join(gen_layer.fitting(gen_layer.layout(gen_layer.functions(NARROW))))
        self.assertNotIn("?", text)
        for limit in ("UINT8_MAX", "UINT16_MAX", "INT8_MIN", "INT8_MAX", "INT16_MIN", "INT16_MAX"):
            self.assertIn(limit, text)
        self.assertIsNone(re.search(r"\b(?:255|65535|127|128|32767|32768)\b", text))

    def test_a_default_on_a_narrow_parameter_is_refused(self):
        text = "\nbool kode_led_glow(uint32_t ms, uint8_t level);\n"
        with tables(left_out={}, groups={}, defaults={"kode_led_glow": {"level": "0"}}):
            said = stopped(self, text)
        self.assertIn("kode_led_glow()", said)
        self.assertIn("'level'", said)
        self.assertIn("DEFAULTS", said)


class Generated(unittest.TestCase):
    def test_the_narrow_parameters_are_taken_as_int32(self):
        text = (gen_layer.LIBRARY / gen_layer.OUT_HEADER).read_text(encoding="utf-8")
        self.assertIn("    bool setBrightness(int32_t pct) const\n    {\n"
                      "        return kode_display_set_brightness(kode_arduino_fit_u8(pct));\n", text)
        self.assertIn("    bool setRgb(int32_t r, int32_t g, int32_t b) const\n", text)
        self.assertIn("    bool tone(int32_t hz, uint32_t ms) const\n", text)
        self.assertIn("const uint8_t *keys", text)
        signatures = SIGNATURE.findall(text)
        self.assertEqual([line for line in signatures if NARROW_BY_VALUE.search(line)], [])

    def test_the_fitting_functions_are_the_ones_used_each_before_its_first_use(self):
        text = (gen_layer.LIBRARY / gen_layer.OUT_HEADER).read_text(encoding="utf-8")
        defined = FIT_DEFINED.findall(text)
        bodies = "\n".join(line for line in text.splitlines() if line.startswith("        "))
        used = set(FIT_USED.findall(bodies))
        self.assertTrue(used)
        self.assertEqual(sorted(defined), sorted(used))
        for name in defined:
            self.assertLess(text.index(f" {name}(int32_t value)"), text.index(f"{name}(", text.index("\nclass ")))

    def test_the_fitting_functions_fit(self):
        text = (gen_layer.LIBRARY / gen_layer.OUT_HEADER).read_text(encoding="utf-8")
        defined = set(FIT_DEFINED.findall(text))
        cases = [case for case in FITTED if case[0] in defined]
        self.assertIn("kode_arduino_fit_u8", defined)
        self.assertIn("kode_arduino_fit_u16", defined)
        lines = ["#include <iostream>", "", "int main(void)", "{"]
        for name, given, _ in cases:
            lines.append(f"    std::cout << static_cast<int32_t>({name}({given})) << \"\\n\";")
        lines += ["    return 0;", "}", ""]
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "Fit.cpp"
            source.write_text("\n".join(lines), encoding="utf-8")
            printed = run_program(self, source)
        self.assertEqual(printed.split(), [str(answered) for _, _, answered in cases])

    def test_each_format_is_three_methods_and_the_helpers_come_once_before_the_first_class(self):
        text = (gen_layer.LIBRARY / gen_layer.OUT_HEADER).read_text(encoding="utf-8")
        self.assertIn("\n".join(PRINTF_METHODS) + "\n", text)
        self.assertIn("\n".join(SET_TEXT_METHODS) + "\n", text)
        helpers = "\n".join(TEXT_HELPERS[1:]) + "\n"
        self.assertEqual(text.count(helpers), 1)
        self.assertLess(text.index(helpers), text.index("\nclass "))
        self.assertNotIn("Args... args", text)
        # The core reads the layer before it declares String: no code may need it.
        self.assertIsNone(re.search(r"\bString\b", gen_layer.COMMENT.sub("", text)))

    def test_a_text_is_shown_as_it_is_and_an_object_as_its_characters(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "Texts.cpp"
            source.write_text(TEXTS_PROGRAM, encoding="utf-8")
            printed = run_program(self, source)
        self.assertEqual(printed.splitlines(), TEXTS_SHOWN)

    def test_a_number_is_no_text(self):
        with tempfile.TemporaryDirectory() as tmp:
            sketch = Path(tmp) / "NumberAsText.ino"
            sketch.write_text(NUMBER_AS_TEXT, encoding="utf-8")
            self.assertIn("setText", refused_sketch(self, sketch))

    def test_the_files_on_disk_are_what_kode_h_says(self):
        checked = subprocess.run([sys.executable, str(TOOLS / "gen_layer.py"), "--check"],
                                 capture_output=True, text=True)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    def test_a_handle_is_not_a_number(self):
        text = (gen_layer.LIBRARY / gen_layer.OUT_HEADER).read_text(encoding="utf-8")
        for handle, name in gen_layer.HANDLES.items():
            self.assertIn(f"    explicit {name}({handle} from) : handle(from)\n", text)
            self.assertNotIn(f"operator {handle}", text)
        self.assertEqual(text.count("    explicit operator bool(void) const\n"), len(gen_layer.HANDLES))
        self.assertIn("        return KODE_ARDUINO_NO_HANDLE != handle;\n", text)

    def test_a_handle_builds_as_a_sketch_uses_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            sketch = Path(tmp) / "Handles.ino"
            sketch.write_text(HANDLES_SKETCH, encoding="utf-8")
            compile_sketch(self, sketch)

    def test_a_number_is_no_widget(self):
        with tempfile.TemporaryDirectory() as tmp:
            sketch = Path(tmp) / "NumberAsWidget.ino"
            sketch.write_text(NUMBER_AS_WIDGET, encoding="utf-8")
            self.assertIn("KodeWidget", refused_sketch(self, sketch))

    def test_a_widget_is_no_canvas(self):
        with tempfile.TemporaryDirectory() as tmp:
            sketch = Path(tmp) / "WidgetAsCanvas.ino"
            sketch.write_text(WIDGET_AS_CANVAS, encoding="utf-8")
            self.assertIn("KodeCanvas", refused_sketch(self, sketch))

    def test_all_calls_sketch_passes_a_zero_of_the_declared_type(self):
        sketch = gen_layer.all_calls("AllCalls.ino")
        self.assertIn("    (void)Kode.display.setBrightness(zero<int32_t>());", sketch)
        self.assertIn("    (void)Kode.audio.tone(zero<int32_t>(), zero<uint32_t>());", sketch)
        self.assertIsNone(re.search(r"zero<u?int(?:8|16)_t>", sketch))
        # A format function is one line, its text alone.
        self.assertEqual(sketch.count("    Kode.printf("), 1)
        self.assertIn("    Kode.printf(zero<const char *>());", sketch)
        self.assertEqual(sketch.count("    (void)widget.setText("), 1)
        self.assertIn("    (void)widget.setText(zero<const char *>());", sketch)

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
