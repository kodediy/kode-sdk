#!/usr/bin/env python3
"""kode.h into the Arduino library Kode: src/KodeArduino.h and keywords.txt.

    arduino/tools/gen_layer.py                  write both files
    arduino/tools/gen_layer.py --check          exit 1 when a file on disk is not what kode.h says
    arduino/tools/gen_layer.py --all-calls FILE write a sketch that names every method once

Every function of kode.h becomes one method that calls it and does nothing
else, so a sketch reads as Arduino and runs the SDK itself:

    kode_led_set_color(c)         Kode.led.setColor(c)
    kode_widget_set_text(w, ...)  w.setText(...)         w is a KodeWidget
    kode_draw_pixel(c, ...)       c.drawPixel(...)       c is a KodeCanvas
    kode_exit()                   Kode.exit()

The rule is place(); the names it cannot give are in RENAMED. A function the
rule would name with a C++ keyword, two that would share a name, or one named
as a group stop the run, so kode.h cannot grow a call this file passes over.

The layer's names at file scope are the object Kode, its class Kode_Sdk, a
class Kode_<group> behind each Kode.<group>, the handles' classes and an alias
Kode<Type> for each kode_<type>_t. An alias is "Kode" and capitalised words
with no underscore, so no class of a group can be one; check_names() stops
the run on any two names that are one all the same.

The board's variant includes the header from its pins_arduino.h, so a sketch
names Kode with no include, as it names Serial. The variant also starts the
SDK before setup(), so kode_init.h is no part of the layer: a sketch has no
start to call.

Both files are checked in, so a sketch never needs Python; CI runs --check so
they cannot drift. The --all-calls sketch only has to build: its calls are
linked and never made.
"""
import argparse
import re
import sys
from collections import namedtuple
from pathlib import Path

SDK = Path(__file__).resolve().parent.parent.parent
LIBRARY = SDK / "arduino" / "libraries" / "Kode"
HEADER = Path("components") / "kode" / "include" / "kode.h"
COLORS_HEADER = Path("components") / "kode" / "internal" / "include" / "kode_colors.h"
INPUTS = (HEADER, COLORS_HEADER)
OUT_HEADER = Path("src") / "KodeArduino.h"
OUT_KEYWORDS = Path("keywords.txt")

# The handles: a function whose first parameter has one of these types is a
# method of the handle's class, and a function that answers one answers the class.
HANDLES = {
    "kode_widget_t": "KodeWidget",
    "kode_canvas_t": "KodeCanvas",
}

# What a handle's class says it is, for its @brief.
HANDLE_BRIEFS = {
    "kode_widget_t": "A widget of the screen",
    "kode_canvas_t": "A canvas of the screen",
}

# The names the rule cannot give, as (group, method); "" is Kode itself.
# `new` and `delete` are C++ keywords, so a screen is shown and a file removed;
# kode_rest() shares its group's name; kode_print() takes a format, which
# Arduino calls printf.
RENAMED = {
    "kode_print":       ("", "printf"),
    "kode_rest":        ("rest", "begin"),
    "kode_screen_new":  ("screen", "show"),
    "kode_file_delete": ("file", "remove"),
}

CPP_KEYWORDS = frozenset("""
    alignas alignof and and_eq asm auto bitand bitor bool break case catch char
    char8_t char16_t char32_t class compl concept const consteval constexpr
    constinit const_cast continue co_await co_return co_yield decltype default
    delete do double dynamic_cast else enum explicit export extern false float
    for friend goto if inline int long mutable namespace new noexcept not
    not_eq nullptr operator or or_eq private protected public register
    reinterpret_cast requires return short signed sizeof static static_assert
    static_cast struct switch template this thread_local throw true try typedef
    typeid typename union unsigned using virtual void volatile wchar_t while
    xor xor_eq
""".split())

COMMENT = re.compile(r"/\*.*?\*/", re.S)
PROTOTYPE = re.compile(
    r"\n(?P<ret>[A-Za-z_][\w \*]*?)\b(?P<name>kode_[a-z0-9_]+)\s*"
    r"\((?P<params>[^;{}()]*)\)\s*(?:KODE_PRINTF_FORMAT\([^)]*\))?\s*;")
PARAM = re.compile(r"(?P<type>.*?)(?P<name>[A-Za-z_]\w*)")
STRUCT_TYPE = re.compile(r"^\}\s*(kode_[a-z0-9_]+_t)\s*;", re.M)
CALLBACK_TYPE = re.compile(r"^typedef\s[^;]*\(\*(kode_[a-z0-9_]+_t)\)", re.M)
SCALAR_TYPE = re.compile(r"^typedef\s+[A-Za-z_][\w ]*\s(kode_[a-z0-9_]+_t)\s*;", re.M)
NAMED_TYPE = re.compile(r"\bkode_[a-z0-9_]+_t\b")
DIRECTIVE = re.compile(r"^[ \t]*#(?:[^\n]*\\\n)*[^\n]*", re.M)
CALL = re.compile(r"\b(kode_[a-z0-9_]+)\s*\(")
# The words a return type may be spelled with; anything else, a macro before
# the type included, stops the run rather than be copied into the layer.
RETURN_WORD = re.compile(r"const|void|bool|char|int|float|double|size_t|u?int(?:8|16|32|64)_t|kode_[a-z0-9_]+_t")
LITERAL = re.compile(r"^#define\s+(KODE_[A-Z0-9_]+)\s+\S", re.M)

Param = namedtuple("Param", "type name text")
Function = namedtuple("Function", "ret name params variadic")
Method = namedtuple("Method", "name fn")

OBJECT = "Kode"            # The object a sketch names.
OBJECT_CLASS = "Kode_Sdk"  # Its class: a capital after the underscore, where a group's class has kode.h's lower case.

PREAMBLE = """/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file KodeArduino.h
 * @brief kode.h in Arduino's style: every call is a method of Kode.
 *
 * Generated by arduino/tools/gen_layer.py from kode.h. Do not edit.
 */
#ifndef KODE_ARDUINO_H_
#define KODE_ARDUINO_H_

#include "kode.h"

#define KODE_ARDUINO_NO_HANDLE 0U /**< What kode.h answers for a widget or a canvas it could not make. */
"""


def fail(msg):
    sys.exit(f"gen_layer: {msg}")


def camel(words):
    """["set", "color"] as setColor."""
    return words[0] + "".join(word.capitalize() for word in words[1:])


def pascal(words):
    """["file", "status"] as FileStatus."""
    return "".join(word.capitalize() for word in words)


def type_words(c_type):
    """kode_file_status_t as ["file", "status"]."""
    return c_type[len("kode_"):-len("_t")].split("_")


def parse_params(text, name):
    """A parameter list as ([Param], whether it ends in `...`)."""
    text = " ".join(text.split())
    if text in ("", "void"):
        return [], False
    params = []
    variadic = False
    for part in (piece.strip() for piece in text.split(",")):
        if part == "...":
            variadic = True
            continue
        found = PARAM.fullmatch(part)
        if found is None or not found.group("type").strip():
            fail(f"{name}(): cannot read the parameter '{part}'")
        params.append(Param(found.group("type").strip(), found.group("name"), part))
    return params, variadic


def check_return(ret, name):
    """Stop on a return type the layer cannot spell."""
    for word in ret.replace("*", " ").split():
        if RETURN_WORD.fullmatch(word) is None:
            fail(f"{name}(): cannot spell the return type '{ret}' ('{word}')")


def functions(text):
    """Every kode_*() prototype of a header, in the header's order.

    Any kode_*( left in the header outside a comment or a directive that is
    not one of them stops the run, so no call is passed over in silence.
    """
    bare = DIRECTIVE.sub("", "\n" + COMMENT.sub("", text))
    found = []
    for match in PROTOTYPE.finditer(bare):
        ret = " ".join(match.group("ret").split())
        name = match.group("name")
        check_return(ret, name)
        params, variadic = parse_params(match.group("params"), name)
        found.append(Function(ret, name, params, variadic))
    read_names = {fn.name for fn in found}
    for name in CALL.findall(bare):
        if name not in read_names:
            fail(f"{name}() is in the header but not read as a prototype: give it kode.h's usual form")
    return found


def place(fn):
    """Where a function goes: (owner, method). The owner is a handle's type, a group, or "" for Kode."""
    if fn.name in RENAMED:
        return RENAMED[fn.name]
    words = fn.name.split("_")[1:]
    if fn.params and fn.params[0].type in HANDLES:
        handle = fn.params[0].type
        if words[0] == "_".join(type_words(handle)):
            words = words[1:]
        return handle, camel(words)
    if len(words) == 1:
        return "", words[0]
    return words[0], camel(words[1:])


def layout(fns):
    """The functions by owner, each owner's methods in kode.h's order."""
    owners = {}
    for fn in fns:
        owner, method = place(fn)
        if method in CPP_KEYWORDS:
            fail(f"{fn.name}() would be the method '{method}', a C++ keyword: name it in RENAMED")
        methods = owners.setdefault(owner, [])
        for other in methods:
            if other.name == method:
                fail(f"{fn.name}() and {other.fn.name}() would both be '{method}': name one in RENAMED")
        methods.append(Method(method, fn))
    for method in owners.get("", []):
        if method.name in owners:
            fail(f"{method.fn.name}() would be {OBJECT}.{method.name}, which is a group: name it in RENAMED")
    return owners


def signature(ret, method, params, variadic):
    """The lines that declare one method, up to its body."""
    lines = []
    listed = [param.text for param in params]
    if variadic:
        lines.append("    template <typename... Args>")
        listed.append("Args... args")
    if not listed:
        listed = ["void"]
    lines.append(f"    {ret}{'' if ret.endswith('*') else ' '}{method}({', '.join(listed)}) const")
    return lines


def method_lines(method, handle):
    """One method: its @brief and a body that is the call alone."""
    fn = method.fn
    params = fn.params
    passed = [param.name for param in params]
    if handle:
        params = params[1:]
        passed = ["handle"] + passed[1:]
    if fn.variadic:
        passed.append("args...")
    call = f"{fn.name}({', '.join(passed)})"
    ret = HANDLES.get(fn.ret, fn.ret)
    if fn.ret in HANDLES:
        body = f"return {ret}({call});"
    elif fn.ret == "void":
        body = f"{call};"
    else:
        body = f"return {call};"
    lines = [f"    /** @brief {fn.name}(). */"]
    lines += signature(ret, method.name, params, fn.variadic)
    lines += ["    {", f"        {body}", "    }"]
    return lines


def handle_class(handle, methods):
    """The class of one handle type."""
    name = HANDLES[handle]
    lines = [
        f"/** @brief {HANDLE_BRIEFS[handle]}: the calls of kode.h that take a {handle}. */",
        f"class {name}",
        "{",
        "public:",
        "    /** @brief None yet. */",
        f"    {name}(void) : handle(KODE_ARDUINO_NO_HANDLE)",
        "    {",
        "    }",
        "",
        "    /** @brief The one kode.h answered. */",
        f"    {name}({handle} from) : handle(from)",
        "    {",
        "    }",
        "",
        "    /** @brief The handle, for a call of kode.h. */",
        f"    operator {handle}(void) const",
        "    {",
        "        return handle;",
        "    }",
    ]
    for method in methods:
        lines.append("")
        lines += method_lines(method, handle)
    lines += ["", "private:", f"    {handle} handle; /**< What kode.h knows it by. */", "};"]
    return lines


def group_class_name(group):
    """The class behind Kode.<group>: Kode_led. An underscore no alias of a type can hold."""
    return f"Kode_{group}"


def alias_name(c_type):
    """Arduino's spelling of a type: kode_tag_t as KodeTag."""
    return f"Kode{pascal(type_words(c_type))}"


def check_names(groups, c_types):
    """Stop the run when two names the layer declares at file scope would be one."""
    named = [(OBJECT, "the object"), (OBJECT_CLASS, "the object's class")]
    named += [(group_class_name(group), f"the class of {OBJECT}.{group}") for group in groups]
    named += [(name, f"the class of {handle}") for handle, name in HANDLES.items()]
    named += [(alias_name(c_type), f"the alias of {c_type}") for c_type in c_types]
    seen = {}
    for name, what in named:
        if name in seen:
            fail(f"{seen[name]} and {what} would both be {name}")
        seen[name] = what


def group_class(group, methods):
    """The class behind Kode.<group>."""
    lines = [
        f"/** @brief {OBJECT}.{group}: the kode_{group}_*() calls. */",
        f"class {group_class_name(group)}",
        "{",
        "public:",
    ]
    for index, method in enumerate(methods):
        if index > 0:
            lines.append("")
        lines += method_lines(method, None)
    lines.append("};")
    return lines


def object_class(groups, loose):
    """Kode_Sdk: a member per group, then the calls that belong to no group."""
    lines = [
        "/** @brief The Kode SDK: a member for each group of kode.h, and the calls of no group. */",
        f"class {OBJECT_CLASS}",
        "{",
        "public:",
    ]
    width = max(len(f"{group_class_name(group)} {group};") for group in groups)
    for group in groups:
        member = f"{group_class_name(group)} {group};"
        lines.append(f"    {member.ljust(width)} /**< The kode_{group}_*() calls. */")
    for method in loose:
        lines.append("")
        lines += method_lines(method, None)
    lines.append("};")
    return lines


def alias_types(text, fns):
    """The types that get an alias: every one kode.h declares, and every one a call names."""
    bare = COMMENT.sub("", text)
    found = set(STRUCT_TYPE.findall(bare) + CALLBACK_TYPE.findall(bare) + SCALAR_TYPE.findall(bare))
    for fn in fns:
        found.update(NAMED_TYPE.findall(fn.ret))
        for param in fn.params:
            found.update(NAMED_TYPE.findall(param.type))
    return sorted(found - set(HANDLES))


def aliases(text, fns):
    """Arduino's spelling of each type: kode_tag_t as KodeTag."""
    rows = []
    for c_type in alias_types(text, fns):
        rows.append((f"using {alias_name(c_type)} = {c_type};", f"/**< {c_type}. */"))
    width = max(len(row[0]) for row in rows)
    return [f"{row[0].ljust(width)} {row[1]}" for row in rows]


def read(path):
    """One input of the SDK's, as text."""
    return (SDK / path).read_text(encoding="utf-8")


def sdk_functions():
    """kode.h's functions."""
    return functions(read(HEADER))


def owners_of_the_sdk():
    """The layout of kode.h's functions."""
    return layout(sdk_functions())


def header():
    """KodeArduino.h."""
    text = read(HEADER)
    owners = owners_of_the_sdk()
    groups = [owner for owner in owners if owner and owner not in HANDLES]
    check_names(groups, alias_types(text, sdk_functions()))
    lines = PREAMBLE.split("\n")
    lines += ["/* ---- Types ---- */", ""] + aliases(text, sdk_functions()) + [""]
    lines += ["/* ---- Handles ---- */"]
    for handle in HANDLES:
        if handle not in owners:
            fail(f"no function of kode.h takes a {handle}: drop it from HANDLES")
        lines += [""] + handle_class(handle, owners[handle])
    lines += ["", "/* ---- Groups ---- */"]
    for group in groups:
        lines += [""] + group_class(group, owners[group])
    lines += ["", "/* ---- Kode ---- */", ""]
    lines += object_class(groups, owners.get("", []))
    lines += ["", f"inline {OBJECT_CLASS} {OBJECT}; /**< The SDK, as a sketch names it. */", "",
              "#endif /* KODE_ARDUINO_H_ */", ""]
    return "\n".join(lines)


def keywords():
    """keywords.txt: what the Arduino editor colours."""
    text = read(HEADER)
    colors = read(COLORS_HEADER)
    owners = owners_of_the_sdk()
    bare = COMMENT.sub("", text)
    types = {OBJECT} | set(HANDLES.values())
    for c_type in alias_types(text, sdk_functions()):
        types.add(alias_name(c_type))
    methods = set()
    for owner, listed in owners.items():
        if owner and owner not in HANDLES:
            methods.add(owner)
        methods.update(method.name for method in listed)
    literals = set(LITERAL.findall(bare)) | set(LITERAL.findall(COMMENT.sub("", colors)))
    lines = []
    for kind, names in (("KEYWORD1", types), ("KEYWORD2", methods), ("LITERAL1", literals)):
        lines += [f"{name}\t{kind}" for name in sorted(names)]
        lines.append("")
    return "\n".join(lines)


def call_lines(owner, method):
    """One method of the layer called with a zero for each argument."""
    fn = method.fn
    params = fn.params
    if owner in HANDLES:
        params = params[1:]
        target = f"{handle_variable(owner)}.{method.name}"
    elif owner:
        target = f"{OBJECT}.{owner}.{method.name}"
    else:
        target = f"{OBJECT}.{method.name}"
    passed = ", ".join(f"zero<{param.type}>()" for param in params)
    cast = ""
    if fn.ret != "void":
        cast = "(void)"
    return [f"    {cast}{target}({passed});"]


def handle_variable(handle):
    """The sketch's variable for a handle's class: KodeWidget as widget."""
    return HANDLES[handle][len("Kode"):].lower()


def all_calls(name):
    """A sketch that names every method of the layer once, from a function no call reaches at run time."""
    owners = owners_of_the_sdk()
    lines = [
        "/* SPDX-License-Identifier: GPL-3.0-or-later */",
        "/**",
        f" * @file {name}",
        " * @brief Every method of the Kode layer once: linked, and never called. It includes nothing,",
        " * as a sketch does: the board brings the layer.",
        " *",
        " * Generated by arduino/tools/gen_layer.py --all-calls from kode.h. Do not edit.",
        " */",
        "",
        "/** @brief A zero of any type: the argument of every call below. */",
        "template <typename T>",
        "static T zero(void);",
        "",
        "static volatile bool s_never = false; /**< Never true, and the compiler cannot know it. */",
        "",
        "/** @brief Every method of the layer once. */",
        "static void all_calls(void)",
        "{",
    ]
    for handle in HANDLES:
        lines.append(f"    {HANDLES[handle]} {handle_variable(handle)};")
    lines.append("")
    ordered = list(HANDLES) + [owner for owner in owners if owner and owner not in HANDLES] + [""]
    for owner in ordered:
        for method in owners.get(owner, []):
            lines += call_lines(owner, method)
    # zero() is defined below all_calls(): Arduino writes its prototypes above the first function a
    # sketch defines, and there they would cut a template header from its function.
    lines += [
        "}",
        "",
        "template <typename T>",
        "static T zero(void)",
        "{",
        "    return T{};",
        "}",
        "",
        "void setup(void)",
        "{",
        "    if ( s_never )",
        "    {",
        "        all_calls();",
        "    }",
        "}",
        "",
        "void loop(void)",
        "{",
        "}",
        "",
    ]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    what = ap.add_mutually_exclusive_group()
    what.add_argument("--check", action="store_true",
                      help="write nothing; exit 1 when a file on disk is not what kode.h says")
    what.add_argument("--all-calls", type=Path, metavar="FILE",
                      help="write a sketch that names every method of the layer once")
    args = ap.parse_args()
    for needed in INPUTS:
        if not (SDK / needed).is_file():
            fail(f"{SDK / needed} is not there: is {SDK} a kode-sdk checkout?")
    if args.all_calls is not None:
        args.all_calls.parent.mkdir(parents=True, exist_ok=True)
        args.all_calls.write_text(all_calls(args.all_calls.name), encoding="utf-8")
        print(f"wrote {args.all_calls}")
        return 0
    outputs = [(LIBRARY / OUT_HEADER, header()), (LIBRARY / OUT_KEYWORDS, keywords())]
    if args.check:
        stale = False
        for path, text in outputs:
            rel = path.relative_to(SDK)
            if not path.exists() or path.read_text(encoding="utf-8") != text:
                print(f"{rel} is stale: run arduino/tools/gen_layer.py")
                stale = True
            else:
                print(f"{rel} matches kode.h")
        return 1 if stale else 0
    for path, text in outputs:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        print(f"wrote {path.relative_to(SDK)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
