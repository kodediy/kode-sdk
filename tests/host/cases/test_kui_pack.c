/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kui_pack.c
 * @brief kui_pack: the manifest, its limits, and the hour's pick.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "fakes.h"
#include "kui_pack.h"

/** A manifest with every key the format knows. */
static const char FULL[] =
    "{\"format\":1,\"name\":\"Kody\",\"version\":\"2026.10.14\","
    "\"scenes\":["
    "{\"name\":\"tren\",\"wake\":\"despierta_tren.json.z\",\"loop\":\"escena_tren.json.z\","
    " \"anchor\":45,\"on\":{\"look\":[\"tren_te_mira.json.z\",\"tren_saluda.json\"]}},"
    "{\"name\":\"reloj\",\"wake\":\"despierta_reloj.json.z\",\"loop\":\"reloj.json.z\"}],"
    "\"face\":{\"default\":\"reloj\",\"hours\":[{\"from\":6,\"to\":10,\"scene\":\"tren\"}]},"
    "\"empty\":\"duda.json.z\",\"writing\":\"portatil.json.z\","
    "\"goodbye\":\"despide.json.z\",\"loading\":\"cargando.json.z\"}";

static kui_pack_err_t parse(const char *text, kui_pack_t *p)
{
    return kui_pack_parse(text, strlen(text), p);
}

TEST_CASE("a full manifest parses into scenes, files, roles and a window", "[kui_pack]")
{
    kui_pack_t p;
    TEST_ASSERT_EQUAL_INT(KUI_PACK_OK, parse(FULL, &p));
    TEST_ASSERT_EQUAL_STRING("Kody", p.name);
    TEST_ASSERT_EQUAL_STRING("2026.10.14", p.version);
    TEST_ASSERT_EQUAL_UINT16(2, p.n_scenes);
    TEST_ASSERT_EQUAL_STRING("tren", p.scenes[0].name);
    TEST_ASSERT_EQUAL_UINT16(45, p.scenes[0].anchor);
    TEST_ASSERT_EQUAL_UINT16(2, p.scenes[0].on_count[KUI_PACK_TRIGGER_LOOK]);
    TEST_ASSERT_EQUAL_UINT16(0, p.scenes[1].on_count[KUI_PACK_TRIGGER_LOOK]);
    TEST_ASSERT_EQUAL_STRING("tren_saluda.json",
        p.files[p.react[p.scenes[0].on_first[KUI_PACK_TRIGGER_LOOK] + 1U]].name);
    TEST_ASSERT_EQUAL_UINT16(10, p.n_files);
    TEST_ASSERT_EQUAL_STRING("duda.json.z", p.files[p.role[KUI_PACK_ROLE_EMPTY]].name);
    TEST_ASSERT_EQUAL_UINT16(1, p.default_scene);
    TEST_ASSERT_EQUAL_UINT16(1, p.n_windows);
    TEST_ASSERT_NOT_EQUAL(0U, p.crc);
    kui_pack_free(&p);
    TEST_ASSERT_NULL(p.files);
}

TEST_CASE("the hour picks its window's scene and the default outside it", "[kui_pack]")
{
    kui_pack_t p;
    TEST_ASSERT_EQUAL_INT(KUI_PACK_OK, parse(FULL, &p));
    TEST_ASSERT_EQUAL_UINT16(0, kui_pack_scene_for_hour(&p, 6));
    TEST_ASSERT_EQUAL_UINT16(0, kui_pack_scene_for_hour(&p, 9));
    TEST_ASSERT_EQUAL_UINT16(1, kui_pack_scene_for_hour(&p, 10));
    TEST_ASSERT_EQUAL_UINT16(1, kui_pack_scene_for_hour(&p, 23));
    TEST_ASSERT_EQUAL_UINT16(1, kui_pack_scene_for_hour(&p, -1));
    kui_pack_free(&p);
}

TEST_CASE("without face the first scene is the default", "[kui_pack]")
{
    kui_pack_t p;
    TEST_ASSERT_EQUAL_INT(KUI_PACK_OK,
        parse("{\"format\":1,\"scenes\":[{\"name\":\"a\",\"wake\":\"w.json\",\"loop\":\"l.json\"}]}", &p));
    TEST_ASSERT_EQUAL_UINT16(0, kui_pack_scene_for_hour(&p, 12));
    TEST_ASSERT_EQUAL_UINT16(KUI_PACK_NONE, p.role[KUI_PACK_ROLE_LOADING]);
    kui_pack_free(&p);
}

TEST_CASE("unknown keys are ignored at every level", "[kui_pack]")
{
    kui_pack_t p;
    TEST_ASSERT_EQUAL_INT(KUI_PACK_OK, parse(
        "{\"format\":1,\"future\":[1,2],\"scenes\":[{\"name\":\"a\",\"wake\":\"w.json\","
        "\"loop\":\"l.json\",\"sparkle\":true,\"on\":{\"charge\":[\"c.json\"],\"look\":[]}}]}", &p));
    TEST_ASSERT_EQUAL_UINT16(2, p.n_files);
    kui_pack_free(&p);
}

TEST_CASE("each malformed manifest is refused with its word", "[kui_pack]")
{
    static const struct { const char *text; kui_pack_err_t want; } CASES[] = {
        { "{\"format\":1,\"scenes\":[", KUI_PACK_E_JSON },
        { "{\"format\":2,\"scenes\":[{\"name\":\"a\",\"wake\":\"w.json\",\"loop\":\"l.json\"}]}", KUI_PACK_E_FORMAT },
        { "{\"scenes\":[{\"name\":\"a\",\"wake\":\"w.json\",\"loop\":\"l.json\"}]}", KUI_PACK_E_FORMAT },
        { "{\"format\":1,\"scenes\":[]}", KUI_PACK_E_SCHEMA },
        { "{\"format\":1,\"scenes\":[{\"name\":\"A\",\"wake\":\"w.json\",\"loop\":\"l.json\"}]}", KUI_PACK_E_NAME },
        { "{\"format\":1,\"scenes\":[{\"name\":\"abcdefghijklmnop\",\"wake\":\"w.json\",\"loop\":\"l.json\"}]}", KUI_PACK_E_NAME },
        { "{\"format\":1,\"scenes\":[{\"name\":\"a\",\"wake\":\"w.json\",\"loop\":\"l.json\"},"
          "{\"name\":\"a\",\"wake\":\"w.json\",\"loop\":\"l.json\"}]}", KUI_PACK_E_NAME },
        { "{\"format\":1,\"scenes\":[{\"name\":\"a\",\"wake\":\"../w.json\",\"loop\":\"l.json\"}]}", KUI_PACK_E_FILE },
        { "{\"format\":1,\"scenes\":[{\"name\":\"a\",\"wake\":\"kody/w.json\",\"loop\":\"l.json\"}]}", KUI_PACK_E_FILE },
        { "{\"format\":1,\"scenes\":[{\"name\":\"a\",\"wake\":\".w.json\",\"loop\":\"l.json\"}]}", KUI_PACK_E_FILE },
        { "{\"format\":1,\"scenes\":[{\"name\":\"a\",\"wake\":\"w.png\",\"loop\":\"l.json\"}]}", KUI_PACK_E_FILE },
        { "{\"format\":1,\"scenes\":[{\"name\":\"a\",\"wake\":\"w.json\"}]}", KUI_PACK_E_FILE },
        { "{\"format\":1,\"scenes\":[{\"name\":\"a\",\"wake\":\"w.json\",\"loop\":\"l.json\",\"anchor\":-1}]}", KUI_PACK_E_SCHEMA },
        { "{\"format\":1,\"scenes\":[{\"name\":\"a\",\"wake\":\"w.json\",\"loop\":\"l.json\",\"anchor\":256}]}", KUI_PACK_E_SCHEMA },
        { "{\"format\":1,\"scenes\":[{\"name\":\"a\",\"wake\":\"w.json\",\"loop\":\"l.json\",\"on\":{\"look\":\"x.json\"}}]}", KUI_PACK_E_SCHEMA },
        { "{\"format\":1,\"scenes\":[{\"name\":\"a\",\"wake\":\"w.json\",\"loop\":\"l.json\"}],"
          "\"face\":{\"hours\":[{\"from\":10,\"to\":6,\"scene\":\"a\"}]}}", KUI_PACK_E_SCHEMA },
        { "{\"format\":1,\"scenes\":[{\"name\":\"a\",\"wake\":\"w.json\",\"loop\":\"l.json\"}],"
          "\"empty\":\"d/x.json\"}", KUI_PACK_E_FILE },
        { "{\"format\":1,\"scenes\":[{\"name\":\"a\",\"wake\":\"w.json\",\"loop\":\"l.json\"}],"
          "\"sha256\":{\"w.json\":\"zz\"}}", KUI_PACK_E_HASH },
    };
    for ( size_t i = 0U; (sizeof(CASES) / sizeof(CASES[0])) > i; ++i )
    {
        kui_pack_t p;
        char msg[16];
        (void)snprintf(msg, sizeof(msg), "case %u", (unsigned)i);
        TEST_ASSERT_EQUAL_INT_MESSAGE(CASES[i].want, parse(CASES[i].text, &p), msg);
        TEST_ASSERT_NULL_MESSAGE(p.files, msg);
    }
}

/* The refusal that costs no file: a pack drawn for another canvas says so. */
TEST_CASE("a pack's canvas is read when it gives one and refused when it is not the face's",
          "[kui_pack]")
{
    kui_pack_t p;
    kui_pack_err_t const fits = parse(
        "{\"format\":1,\"canvas\":{\"w\":410,\"h\":334},\"scenes\":[{\"name\":\"a\","
        "\"wake\":\"w.json\",\"loop\":\"l.json\"}]}", &p);

    TEST_ASSERT_EQUAL_INT(KUI_PACK_OK, fits);
    kui_pack_free(&p);

    TEST_ASSERT_EQUAL_INT(KUI_PACK_E_CANVAS, parse(
        "{\"format\":1,\"canvas\":{\"w\":410,\"h\":288},\"scenes\":[{\"name\":\"a\","
        "\"wake\":\"w.json\",\"loop\":\"l.json\"}]}", &p));
    TEST_ASSERT_NULL_MESSAGE(p.files, "a refusal holds no arrays");
    TEST_ASSERT_EQUAL_STRING("canvas", kui_pack_err_word(KUI_PACK_E_CANVAS));

    TEST_ASSERT_EQUAL_INT(KUI_PACK_E_SCHEMA, parse(
        "{\"format\":1,\"canvas\":[410,334],\"scenes\":[{\"name\":\"a\","
        "\"wake\":\"w.json\",\"loop\":\"l.json\"}]}", &p));
    TEST_ASSERT_EQUAL_INT(KUI_PACK_E_SCHEMA, parse(
        "{\"format\":1,\"canvas\":{\"w\":410},\"scenes\":[{\"name\":\"a\","
        "\"wake\":\"w.json\",\"loop\":\"l.json\"}]}", &p));
}

TEST_CASE("a window naming an unknown scene is dropped, the rest stands", "[kui_pack]")
{
    kui_pack_t p;
    TEST_ASSERT_EQUAL_INT(KUI_PACK_OK, parse(
        "{\"format\":1,\"scenes\":[{\"name\":\"a\",\"wake\":\"w.json\",\"loop\":\"l.json\"}],"
        "\"face\":{\"default\":\"nope\",\"hours\":[{\"from\":1,\"to\":2,\"scene\":\"nope\"}]}}", &p));
    TEST_ASSERT_EQUAL_UINT16(0, p.n_windows);
    TEST_ASSERT_EQUAL_UINT16(0, p.default_scene);
    kui_pack_free(&p);
}

/**
 * @brief A manifest of @p n_scenes scenes, the first with @p n_react looks.
 * @return A heap string the caller frees.
 */
static char *many(unsigned n_scenes, unsigned n_react)
{
    size_t const cap = 64U + (n_scenes * 96U) + (n_react * 24U);
    char *const out = malloc(cap);
    size_t len = 0U;

    TEST_ASSERT_NOT_NULL(out);
    len += (size_t)snprintf(&out[len], cap - len, "{\"format\":1,\"scenes\":[");
    for ( unsigned s = 0U; s < n_scenes; ++s )
    {
        if ( 0U < s )
        {
            out[len] = ',';
            ++len;
        }
        len += (size_t)snprintf(&out[len], cap - len,
                                "{\"name\":\"s%u\",\"wake\":\"w%u.json\",\"loop\":\"l%u.json\"",
                                s, s, s);
        if ( (0U == s) && (0U < n_react) )
        {
            len += (size_t)snprintf(&out[len], cap - len, ",\"on\":{\"look\":[");
            for ( unsigned r = 0U; r < n_react; ++r )
            {
                if ( 0U < r )
                {
                    out[len] = ',';
                    ++len;
                }
                len += (size_t)snprintf(&out[len], cap - len, "\"r%u.json\"", r);
            }
            len += (size_t)snprintf(&out[len], cap - len, "]}");
        }
        len += (size_t)snprintf(&out[len], cap - len, "}");
    }
    (void)snprintf(&out[len], cap - len, "]}");
    return out;
}

TEST_CASE("scenes and reactions past their bounds are refused before any lookup",
          "[kui_pack]")
{
    kui_pack_t p;
    char *text = many(KUI_PACK_SCENES_MAX, KUI_PACK_REACT_MAX);
    TEST_ASSERT_EQUAL_INT(KUI_PACK_OK, parse(text, &p));
    kui_pack_free(&p);
    free(text);

    text = many(KUI_PACK_SCENES_MAX + 1U, 0U);
    TEST_ASSERT_EQUAL_INT(KUI_PACK_E_SCHEMA, parse(text, &p));
    free(text);

    text = many(1U, KUI_PACK_REACT_MAX + 1U);
    TEST_ASSERT_EQUAL_INT(KUI_PACK_E_SCHEMA, parse(text, &p));
    free(text);
}

TEST_CASE("a manifest string is logged as printable text only", "[kui_pack]")
{
    kui_pack_t p;
    bool       seen = false;

    fake_klog_reset();
    TEST_ASSERT_EQUAL_INT(KUI_PACK_OK, parse(
        "{\"format\":1,\"scenes\":[{\"name\":\"a\",\"wake\":\"w.json\",\"loop\":\"l.json\"}],"
        "\"face\":{\"hours\":[{\"from\":1,\"to\":2,\"scene\":\"x\\n>ok\\u001b\"}]}}", &p));
    kui_pack_free(&p);
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char *const line = fake_klog_line(i);

        TEST_ASSERT_NULL_MESSAGE(strstr(line, "\n>ok"), "a forged reply reached the log");
        seen = seen || (strstr(line, "'x?>ok?'") != NULL);
    }
    TEST_ASSERT_TRUE_MESSAGE(seen, "the window's name was not logged, shown safe");
}

TEST_CASE("an empty or oversized manifest is refused for its size", "[kui_pack]")
{
    kui_pack_t p;
    TEST_ASSERT_EQUAL_INT(KUI_PACK_E_SIZE, kui_pack_parse("{}", 0U, &p));
    TEST_ASSERT_EQUAL_INT(KUI_PACK_E_SIZE, kui_pack_parse("{}", KUI_PACK_MANIFEST_MAX + 1U, &p));
}

TEST_CASE("a manifest nested past the bound is refused before cJSON sees it", "[kui_pack]")
{
    static char deep[2U * (KUI_PACK_MAX_DEPTH + 2U) + 1U];
    size_t n = 0U;
    for ( uint32_t i = 0U; (KUI_PACK_MAX_DEPTH + 1U) > i; ++i ) { deep[n++] = '['; }
    for ( uint32_t i = 0U; (KUI_PACK_MAX_DEPTH + 1U) > i; ++i ) { deep[n++] = ']'; }
    deep[n] = '\0';
    kui_pack_t p;
    TEST_ASSERT_EQUAL_INT(KUI_PACK_E_DEPTH, kui_pack_parse(deep, n, &p));
}

TEST_CASE("the depth scan counts brackets outside strings only", "[kui_pack]")
{
    TEST_ASSERT_EQUAL_UINT16(0, kui_pack_json_depth("1", 1U));
    TEST_ASSERT_EQUAL_UINT16(1, kui_pack_json_depth("{\"a\":1}", 7U));
    TEST_ASSERT_EQUAL_UINT16(3, kui_pack_json_depth("{\"a\":[{}]}", 10U));
    TEST_ASSERT_EQUAL_UINT16(1, kui_pack_json_depth("{\"a\":\"[[[\\\"{{\"}", 15U));
}

TEST_CASE("file names: the allowed set, the two endings, forty characters", "[kui_pack]")
{
    TEST_ASSERT_TRUE(kui_pack_file_name_ok("reloj.json.z"));
    TEST_ASSERT_TRUE(kui_pack_file_name_ok("Mi-escena_2.json"));
    TEST_ASSERT_TRUE(kui_pack_file_name_ok("abcdefghijklmnopqrstuvwxyzabcdefghi.json"));   /* 40 */
    TEST_ASSERT_FALSE(kui_pack_file_name_ok("abcdefghijklmnopqrstuvwxyzabcdefghij.json")); /* 41 */
    TEST_ASSERT_FALSE(kui_pack_file_name_ok("a/b.json"));
    TEST_ASSERT_FALSE(kui_pack_file_name_ok(".json"));
    TEST_ASSERT_FALSE(kui_pack_file_name_ok("a.json.zz"));
    TEST_ASSERT_FALSE(kui_pack_file_name_ok("a b.json"));
    TEST_ASSERT_FALSE(kui_pack_file_name_ok(""));
    TEST_ASSERT_FALSE(kui_pack_file_name_ok(NULL));
}

TEST_CASE("digests: parsed, matched to named files, and all-or-not", "[kui_pack]")
{
    static const char H[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char text[512];
    kui_pack_t p;
    (void)snprintf(text, sizeof(text),
        "{\"format\":1,\"scenes\":[{\"name\":\"a\",\"wake\":\"w.json\",\"loop\":\"l.json\"}],"
        "\"sha256\":{\"w.json\":\"%s\",\"other.json\":\"%s\"}}", H, H);
    TEST_ASSERT_EQUAL_INT(KUI_PACK_OK, parse(text, &p));
    TEST_ASSERT_TRUE(p.files[p.scenes[0].wake].hashed);
    TEST_ASSERT_EQUAL_HEX8(0x01, p.files[p.scenes[0].wake].sha256[0]);
    TEST_ASSERT_EQUAL_HEX8(0xef, p.files[p.scenes[0].wake].sha256[31]);
    TEST_ASSERT_FALSE(kui_pack_hashed(&p));          /* l.json has none */
    kui_pack_free(&p);
}

TEST_CASE("a file serves one canvas; two different ones are a conflict", "[kui_pack]")
{
    kui_pack_t p;
    uint16_t w = 0U, h = 0U, a = 0U;
    TEST_ASSERT_EQUAL_INT(KUI_PACK_OK, parse(FULL, &p));
    TEST_ASSERT_TRUE(kui_pack_file_canvas(&p, p.scenes[0].loop, &w, &h, &a));
    TEST_ASSERT_EQUAL_UINT16(410, w); TEST_ASSERT_EQUAL_UINT16(334, h); TEST_ASSERT_EQUAL_UINT16(45, a);
    TEST_ASSERT_TRUE(kui_pack_file_canvas(&p, p.scenes[0].wake, &w, &h, &a));
    TEST_ASSERT_EQUAL_UINT16(0, a);
    TEST_ASSERT_TRUE(kui_pack_file_canvas(&p, p.role[KUI_PACK_ROLE_LOADING], &w, &h, &a));
    TEST_ASSERT_EQUAL_UINT16(264, w); TEST_ASSERT_EQUAL_UINT16(264, h);
    kui_pack_free(&p);
    TEST_ASSERT_EQUAL_INT(KUI_PACK_OK, parse(
        "{\"format\":1,\"scenes\":[{\"name\":\"a\",\"wake\":\"x.json\",\"loop\":\"l.json\"}],"
        "\"loading\":\"x.json\"}", &p));
    TEST_ASSERT_FALSE(kui_pack_file_canvas(&p, p.scenes[0].wake, &w, &h, &a));
    kui_pack_free(&p);
}

TEST_CASE("err words name every refusal", "[kui_pack]")
{
    TEST_ASSERT_EQUAL_STRING("depth", kui_pack_err_word(KUI_PACK_E_DEPTH));
    TEST_ASSERT_EQUAL_STRING("precomp", kui_pack_err_word(KUI_PACK_E_PRECOMP));
    TEST_ASSERT_EQUAL_STRING("error", kui_pack_err_word((kui_pack_err_t)99));
}

/** A minimal Lottie on the face's canvas, 90 frames at 30 fps. */
#define LOTTIE(w, h, ip, op, fr, extra) \
    "{\"v\":\"5.7.0\",\"w\":" #w ",\"h\":" #h ",\"ip\":" #ip ",\"op\":" #op ",\"fr\":" #fr \
    ",\"layers\":[{\"ty\":4,\"ks\":{}}]" extra "}"

static kui_pack_err_t check(const char *json, uint16_t anchor, kui_pack_anim_t *a)
{
    return kui_pack_check_anim(json, strlen(json), KUI_PACK_SCENE_W, KUI_PACK_SCENE_H, anchor, a);
}

TEST_CASE("a Lottie on the right canvas passes and says what it is", "[kui_pack]")
{
    kui_pack_anim_t a;
    TEST_ASSERT_EQUAL_INT(KUI_PACK_OK, check(LOTTIE(410, 334, 0, 90, 30, ""), 45U, &a));
    TEST_ASSERT_EQUAL_UINT16(90, a.frames);
    TEST_ASSERT_EQUAL_UINT8(30, a.fps);
    TEST_ASSERT_EQUAL_UINT16(0, a.precomp);
    TEST_ASSERT_EQUAL_UINT16(4, a.depth);
}

TEST_CASE("canvas, frames, rate and anchor are refused with their words", "[kui_pack]")
{
    kui_pack_anim_t a;
    TEST_ASSERT_EQUAL_INT(KUI_PACK_E_CANVAS, check(LOTTIE(410, 335, 0, 90, 30, ""), 0U, &a));
    TEST_ASSERT_EQUAL_INT(KUI_PACK_E_FRAMES, check(LOTTIE(410, 334, 10, 10, 30, ""), 0U, &a));
    TEST_ASSERT_EQUAL_INT(KUI_PACK_E_FRAMES, check(LOTTIE(410, 334, 0, 1801, 30, ""), 0U, &a));
    TEST_ASSERT_EQUAL_INT(KUI_PACK_E_FRAMES, check(LOTTIE(410, 334, 0, 100, 30, ""), 45U, &a));
    TEST_ASSERT_EQUAL_INT(KUI_PACK_E_FPS, check(LOTTIE(410, 334, 0, 90, 0, ""), 0U, &a));
    TEST_ASSERT_EQUAL_INT(KUI_PACK_E_FPS, check(LOTTIE(410, 334, 0, 90, 61, ""), 0U, &a));
    TEST_ASSERT_EQUAL_INT(KUI_PACK_E_FPS, check(LOTTIE(410, 334, 0, 90, 29.97, ""), 0U, &a));
    TEST_ASSERT_EQUAL_INT(KUI_PACK_E_JSON, check("{\"w\":410,", 0U, &a));
}

TEST_CASE("precomps: a chain within the bound passes, deeper or circular is refused", "[kui_pack]")
{
    kui_pack_anim_t a;
    TEST_ASSERT_EQUAL_INT(KUI_PACK_OK, check(
        "{\"w\":410,\"h\":334,\"ip\":0,\"op\":30,\"fr\":30,\"layers\":[{\"refId\":\"a\"}],"
        "\"assets\":[{\"id\":\"a\",\"layers\":[{\"refId\":\"b\"}]},{\"id\":\"b\",\"layers\":[]}]}", 0U, &a));
    TEST_ASSERT_EQUAL_UINT16(2, a.precomp);
    TEST_ASSERT_EQUAL_INT(KUI_PACK_E_PRECOMP, check(
        "{\"w\":410,\"h\":334,\"ip\":0,\"op\":30,\"fr\":30,\"layers\":[{\"refId\":\"a\"}],"
        "\"assets\":[{\"id\":\"a\",\"layers\":[{\"refId\":\"b\"}]},{\"id\":\"b\",\"layers\":[{\"refId\":\"c\"}]},"
        "{\"id\":\"c\",\"layers\":[]}]}", 0U, &a));
    TEST_ASSERT_EQUAL_INT(KUI_PACK_E_PRECOMP, check(
        "{\"w\":410,\"h\":334,\"ip\":0,\"op\":30,\"fr\":30,\"layers\":[{\"refId\":\"a\"}],"
        "\"assets\":[{\"id\":\"a\",\"layers\":[{\"refId\":\"a\"}]}]}", 0U, &a));
    /* An image asset is no precomp: it has no layers. */
    TEST_ASSERT_EQUAL_INT(KUI_PACK_OK, check(
        "{\"w\":410,\"h\":334,\"ip\":0,\"op\":30,\"fr\":30,\"layers\":[{\"refId\":\"img\"}],"
        "\"assets\":[{\"id\":\"img\",\"p\":\"x.png\"}]}", 0U, &a));
}

TEST_CASE("a Lottie nested past the bound is refused for depth", "[kui_pack]")
{
    static char deep[64U + (2U * (KUI_PACK_MAX_DEPTH + 1U))];
    size_t n = (size_t)snprintf(deep, sizeof(deep), "{\"w\":410,\"h\":334,\"x\":");
    for ( uint32_t i = 0U; KUI_PACK_MAX_DEPTH > i; ++i ) { deep[n++] = '['; }
    for ( uint32_t i = 0U; KUI_PACK_MAX_DEPTH > i; ++i ) { deep[n++] = ']'; }
    deep[n++] = '}';
    deep[n] = '\0';
    kui_pack_anim_t a;
    TEST_ASSERT_EQUAL_INT(KUI_PACK_E_DEPTH, kui_pack_check_anim(deep, n, KUI_PACK_SCENE_W, KUI_PACK_SCENE_H, 0U, &a));
    TEST_ASSERT_EQUAL_UINT16(KUI_PACK_MAX_DEPTH + 1U, a.depth);
}

TEST_CASE("a Lottie over the inflated limit is refused for size", "[kui_pack]")
{
    kui_pack_anim_t a;
    TEST_ASSERT_EQUAL_INT(KUI_PACK_E_SIZE,
        kui_pack_check_anim("{}", KUI_PACK_JSON_MAX + 1U, KUI_PACK_SCENE_W, KUI_PACK_SCENE_H, 0U, &a));
}
