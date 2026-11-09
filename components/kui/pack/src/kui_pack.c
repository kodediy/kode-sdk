/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_pack.c
 * @brief Kody's pack: parsing kode.json and checking a Lottie (kui_pack.h).
 */
#include "kui_pack.h"

#include <string.h>

#include "cJSON.h"
#include "khal_crc32.h"
#include "khal_system.h"
#include "kode_log.h"

/** @brief Log tag. */
static const char *const TAG = "kui.pack";

/** @brief The triggers this firmware knows, in kui_pack_trigger_t's order. */
static const char *const TRIGGERS[KUI_PACK_TRIGGER_COUNT] = { "look", "hour", "notice", "plug", "joke" };


/** @brief Each role's canvas, in kui_pack_role_t's order. */
static const uint16_t ROLE_W[KUI_PACK_ROLE_COUNT] = {
    KUI_PACK_EMPTY_W, KUI_PACK_CARD_W, KUI_PACK_CARD_W, KUI_PACK_LOADING_SIDE
};
/** @brief Each role's canvas height, in kui_pack_role_t's order. */
static const uint16_t ROLE_H[KUI_PACK_ROLE_COUNT] = {
    KUI_PACK_EMPTY_H, KUI_PACK_CARD_H, KUI_PACK_CARD_H, KUI_PACK_LOADING_SIDE
};

/** @brief Largest value of a 16-bit field the manifest or a Lottie gives. */
#define PACK_U16_MAX   0xFFFFU
/** @brief Largest frame number a Lottie may give (its ip and op). */
#define PACK_FRAME_NUM_MAX 0xFFFFFFU
/** @brief The base of a hex digit pair. */
#define PACK_HEX_BASE  16

/**
 * @brief Copy a string from the manifest for a log line: printable ASCII
 *        only, cut at KUI_PACK_LOG_TEXT_MAX.
 *
 * A key or a value can hold any byte once cJSON has decoded its escapes - a
 * line break, a terminal escape - and a log line that carried it could forge
 * a `>` reply on the console. Every other byte is shown as '?'.
 *
 * @param src The manifest's string.
 * @param out Where the copy goes.
 * @param cap Bytes at @p out, its NUL included.
 * @return @p out.
 */
static const char *log_text(const char *src, char *out, size_t cap)
{
    size_t n = 0U;

    while ( (NULL != src) && ('\0' != src[n]) && ((n + 1U) < cap) )
    {
        unsigned char const c = (unsigned char)src[n];

        if ( ((unsigned char)' ' <= c) && ((unsigned char)'~' >= c) )
        {
            out[n] = (char)c;
        }
        else
        {
            out[n] = '?';
        }
        ++n;
    }
    out[n] = '\0';
    return out;
}

/**
 * @brief cJSON's answers as the bool they mean.
 *
 * cJSON says yes or no with an int (cJSON_bool); a controlling expression is
 * essentially boolean here, so every question goes through these.
 */
static bool json_is_number(const cJSON *j)
{
    return cJSON_IsNumber(j) != 0;
}

/** @brief Whether @p j is a string (see json_is_number()). @param j The item. @return The answer. */
static bool json_is_string(const cJSON *j)
{
    return cJSON_IsString(j) != 0;
}

/** @brief Whether @p j is an object (see json_is_number()). @param j The item. @return The answer. */
static bool json_is_object(const cJSON *j)
{
    return cJSON_IsObject(j) != 0;
}

/** @brief Whether @p j is an array (see json_is_number()). @param j The item. @return The answer. */
static bool json_is_array(const cJSON *j)
{
    return cJSON_IsArray(j) != 0;
}

/** @brief Room the first pass found, which the second may not exceed. */
typedef struct {
    kui_pack_t *p;          /**< The pack being filled.          */
    uint16_t    files_cap;  /**< Slots in p->files.              */
    uint16_t    react_cap;  /**< Slots in p->react.              */
} pack_ctx_t;

/**
 * @brief cJSON's allocator: PSRAM, aligned for the doubles cJSON keeps.
 * @param size Bytes.
 * @return The block, or NULL.
 */
static void *pack_malloc(size_t size)
{
    return khal_alloc_psram(sizeof(double), size);
}

/**
 * @brief cJSON's free.
 * @param p A block from pack_malloc().
 */
static void pack_free(void *p)
{
    khal_free(p);
}

/** @brief Point cJSON at PSRAM; cheap, and correct whoever set it before. */
static void pack_hooks(void)
{
    cJSON_Hooks hooks;

    hooks.malloc_fn = &pack_malloc;
    hooks.free_fn   = &pack_free;
    cJSON_InitHooks(&hooks);
}

/**
 * @brief A whole non-negative number no larger than @p max.
 * @param j   The item; NULL is "absent" and refused.
 * @param max The largest value accepted.
 * @param out Receives it.
 * @return true when @p j is such a number.
 */
static bool num_u(const cJSON *j, uint32_t max, uint32_t *out)
{
    bool ok = false;

    if ( json_is_number(j) )
    {
        double const v = j->valuedouble;

        if ( (0.0 <= v) && ((double)max >= v) )
        {
            uint32_t const u = (uint32_t)v;

            /* Whole: the cast lost nothing. */
            if ( (double)u == v )
            {
                *out = u;
                ok = true;
            }
        }
    }
    return ok;
}

/**
 * @brief Copy a string item into a fixed buffer.
 * @param j   The item.
 * @param out The buffer.
 * @param cap Its size, terminator included.
 * @return true when @p j is a string that fits.
 */
static bool text_copy(const cJSON *j, char *out, size_t cap)
{
    bool ok = false;

    if ( json_is_string(j) && (NULL != j->valuestring) )
    {
        size_t const n = strlen(j->valuestring);

        if ( n < cap )
        {
            (void)memcpy(out, j->valuestring, n + 1U);
            ok = true;
        }
    }
    return ok;
}

/**
 * @brief Whether a scene name is 1-15 characters of [a-z0-9_].
 * @param s The name.
 * @return true when it is.
 */
static bool scene_name_ok(const char *s)
{
    size_t const n = strlen(s);
    bool ok = (0U < n) && (KUI_PACK_NAME_MAX >= n);

    for ( size_t i = 0U; ok && (i < n); ++i )
    {
        char const c = s[i];

        ok = (('a' <= c) && ('z' >= c)) || (('0' <= c) && ('9' >= c)) || ('_' == c);
    }
    return ok;
}

/**
 * @brief Log, at debug level, every key of @p obj that is not in @p known.
 * @param obj   An object.
 * @param known The keys the format knows there.
 * @param n     How many.
 * @param where Which object, for the log.
 */
static void unknown_keys(const cJSON *obj, const char *const *known, size_t n,
                         const char *where)
{
    const cJSON *k = NULL;

    cJSON_ArrayForEach(k, obj) {
        bool seen = false;

        for ( size_t i = 0U; (!seen) && (i < n); ++i )
        {
            seen = (NULL != k->string) && (strcmp(k->string, known[i]) == 0);
        }
        if ( (!seen) && (NULL != k->string) )
        {
            char shown[KUI_PACK_LOG_TEXT_MAX + 1U];

            kode_log(KODE_LOG_DEBUG, TAG, "%s: '%s' is not a key this firmware knows; ignored",
                     where, log_text(k->string, shown, sizeof(shown)));
        }
    }
}

/**
 * @brief The index of a file name, added once to the pack's files.
 * @param c   The context.
 * @param j   The item naming the file.
 * @param out Receives the index.
 * @return KUI_PACK_OK, or KUI_PACK_E_FILE when the name breaks the rule.
 */
static kui_pack_err_t file_ref(pack_ctx_t *c, const cJSON *j, uint16_t *out)
{
    kui_pack_err_t err = KUI_PACK_E_FILE;

    if ( json_is_string(j) && kui_pack_file_name_ok(j->valuestring) )
    {
        uint16_t idx = kui_pack_file_find(c->p, j->valuestring);

        if ( KUI_PACK_NONE == idx )
        {
            if ( c->p->n_files < c->files_cap )
            {
                idx = c->p->n_files;
                (void)memcpy(c->p->files[idx].name, j->valuestring,
                             strlen(j->valuestring) + 1U);
                c->p->n_files++;
            }
        }
        if ( KUI_PACK_NONE != idx )
        {
            *out = idx;
            err = KUI_PACK_OK;
        }
    }
    return err;
}

/**
 * @brief Fill scene @p idx from its object.
 * @param c   The context.
 * @param s   The scene's object.
 * @param idx Its index.
 * @return KUI_PACK_OK or the refusal.
 */
static kui_pack_err_t scene_build(pack_ctx_t *c, const cJSON *s, uint16_t idx)
{
    static const char *const KNOWN[] = { "name", "wake", "loop", "anchor", "on" };
    kui_pack_scene_t *const sc = &c->p->scenes[idx];
    kui_pack_err_t err = KUI_PACK_OK;
    uint32_t anchor = 0U;

    if ( !json_is_object(s) )
    {
        return KUI_PACK_E_SCHEMA;
    }
    unknown_keys(s, KNOWN, sizeof(KNOWN) / sizeof(KNOWN[0]), "scene");
    if ( !text_copy(cJSON_GetObjectItemCaseSensitive(s, "name"), sc->name, sizeof(sc->name))
        || !scene_name_ok(sc->name) )
    {
        return KUI_PACK_E_NAME;
    }
    for ( uint16_t j = 0U; j < idx; ++j )
    {
        if ( strcmp(c->p->scenes[j].name, sc->name) == 0 )
        {
            return KUI_PACK_E_NAME;
        }
    }
    err = file_ref(c, cJSON_GetObjectItemCaseSensitive(s, "wake"), &sc->wake);
    if ( KUI_PACK_OK == err )
    {
        err = file_ref(c, cJSON_GetObjectItemCaseSensitive(s, "loop"), &sc->loop);
    }
    if ( KUI_PACK_OK != err )
    {
        return err;
    }
    const cJSON *const a = cJSON_GetObjectItemCaseSensitive(s, "anchor");
    if ( NULL != a )
    {
        if ( !num_u(a, KUI_PACK_ANCHOR_MAX, &anchor) )
        {
            return KUI_PACK_E_SCHEMA;
        }
    }
    sc->anchor = (uint16_t)anchor;

    const cJSON *const on = cJSON_GetObjectItemCaseSensitive(s, "on");
    if ( (NULL != on) && !json_is_object(on) )
    {
        return KUI_PACK_E_SCHEMA;
    }
    if ( NULL != on )
    {
        unknown_keys(on, TRIGGERS, KUI_PACK_TRIGGER_COUNT, "on");
    }
    for ( uint32_t t = 0U; (uint32_t)KUI_PACK_TRIGGER_COUNT > t; ++t )
    {
        const cJSON *list = NULL;
        const cJSON *f = NULL;

        sc->on_first[t] = c->p->n_react;
        sc->on_count[t] = 0U;
        if ( NULL != on )
        {
            list = cJSON_GetObjectItemCaseSensitive(on, TRIGGERS[t]);
        }
        if ( (NULL != list) && !json_is_array(list) )
        {
            return KUI_PACK_E_SCHEMA;
        }
        cJSON_ArrayForEach(f, list) {
            uint16_t fi = KUI_PACK_NONE;

            if ( c->p->n_react >= c->react_cap )
            {
                return KUI_PACK_E_SCHEMA;
            }
            err = file_ref(c, f, &fi);
            if ( KUI_PACK_OK != err )
            {
                return err;
            }
            c->p->react[c->p->n_react] = fi;
            c->p->n_react++;
            sc->on_count[t]++;
        }
    }
    return KUI_PACK_OK;
}

/**
 * @brief Read `face`: the default scene and the hour's windows.
 * @param c    The context.
 * @param face The object, or NULL.
 * @return KUI_PACK_OK or KUI_PACK_E_SCHEMA / KUI_PACK_E_MEMORY.
 */
static kui_pack_err_t face_build(pack_ctx_t *c, const cJSON *face)
{
    kui_pack_t *const p = c->p;
    const cJSON *w = NULL;

    p->default_scene = 0U;
    if ( NULL == face )
    {
        return KUI_PACK_OK;
    }
    if ( !json_is_object(face) )
    {
        return KUI_PACK_E_SCHEMA;
    }
    const cJSON *const def = cJSON_GetObjectItemCaseSensitive(face, "default");
    if ( NULL != def )
    {
        uint16_t idx = KUI_PACK_NONE;

        if ( !json_is_string(def) )
        {
            return KUI_PACK_E_SCHEMA;
        }
        idx = kui_pack_scene_find(p, def->valuestring);
        if ( KUI_PACK_NONE == idx )
        {
            char shown[KUI_PACK_LOG_TEXT_MAX + 1U];

            kode_log(KODE_LOG_WARN, TAG, "face: default '%s' is no scene; the first scene is",
                     log_text(def->valuestring, shown, sizeof(shown)));
        }
        else
        {
            p->default_scene = idx;
        }
    }
    const cJSON *const hours = cJSON_GetObjectItemCaseSensitive(face, "hours");
    if ( NULL == hours )
    {
        return KUI_PACK_OK;
    }
    if ( !json_is_array(hours) )
    {
        return KUI_PACK_E_SCHEMA;
    }
    int const n = cJSON_GetArraySize(hours);
    if ( 0 < n )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() answers void *: PSRAM is taken by name (rule 8) */
        p->windows = (kui_pack_window_t *)khal_alloc_psram(sizeof(uint32_t),
                                                           (size_t)n * sizeof(kui_pack_window_t));
        if ( NULL == p->windows )
        {
            return KUI_PACK_E_MEMORY;
        }
    }
    cJSON_ArrayForEach(w, hours) {
        uint32_t from = 0U;
        uint32_t to = 0U;
        const cJSON *const sn = cJSON_GetObjectItemCaseSensitive(w, "scene");

        if ( !json_is_object(w) || !num_u(cJSON_GetObjectItemCaseSensitive(w, "from"), KUI_PACK_HOUR_LAST, &from)
            || !num_u(cJSON_GetObjectItemCaseSensitive(w, "to"), KUI_PACK_HOUR_END, &to) || (from >= to)
            || !json_is_string(sn) )
        {
            return KUI_PACK_E_SCHEMA;
        }
        uint16_t const idx = kui_pack_scene_find(p, sn->valuestring);
        if ( KUI_PACK_NONE == idx )
        {
            char shown[KUI_PACK_LOG_TEXT_MAX + 1U];

            kode_log(KODE_LOG_WARN, TAG, "face: a window names '%s', no scene; dropped",
                     log_text(sn->valuestring, shown, sizeof(shown)));
        }
        else
        {
            p->windows[p->n_windows].from  = (uint8_t)from;
            p->windows[p->n_windows].to    = (uint8_t)to;
            p->windows[p->n_windows].scene = idx;
            p->n_windows++;
        }
    }
    return KUI_PACK_OK;
}

/**
 * @brief The value of one hex digit, or -1.
 * @param c The character.
 * @return 0-15, or -1 when @p c is not a lowercase hex digit.
 */
static int hex_val(char c)
{
    int v = -1;

    if ( ('0' <= c) && ('9' >= c) )
    {
        v = (int)c - (int)'0';
    }
    else if ( ('a' <= c) && ('f' >= c) )
    {
        v = 10 + ((int)c - (int)'a');
    }
    else
    {
        v = -1;
    }
    return v;
}

/**
 * @brief Read `sha256`: one digest per file name.
 * @param c   The context.
 * @param obj The object, or NULL.
 * @return KUI_PACK_OK or KUI_PACK_E_HASH.
 */
static kui_pack_err_t hash_build(pack_ctx_t *c, const cJSON *obj)
{
    const cJSON *h = NULL;

    if ( NULL == obj )
    {
        return KUI_PACK_OK;
    }
    if ( !json_is_object(obj) )
    {
        return KUI_PACK_E_HASH;
    }
    cJSON_ArrayForEach(h, obj) {
        uint8_t sha[KUI_PACK_SHA256_LEN];

        if ( !kui_pack_file_name_ok(h->string) || !json_is_string(h)
            || (strlen(h->valuestring) != (2U * KUI_PACK_SHA256_LEN)) )
        {
            return KUI_PACK_E_HASH;
        }
        for ( uint32_t i = 0U; KUI_PACK_SHA256_LEN > i; ++i )
        {
            int const hi = hex_val(h->valuestring[2U * i]);
            int const lo = hex_val(h->valuestring[(2U * i) + 1U]);

            if ( (0 > hi) || (0 > lo) )
            {
                return KUI_PACK_E_HASH;
            }
            int const byte = (hi * PACK_HEX_BASE) + lo;

            sha[i] = (uint8_t)byte;
        }
        uint16_t const idx = kui_pack_file_find(c->p, h->string);
        if ( KUI_PACK_NONE != idx )
        {
            (void)memcpy(c->p->files[idx].sha256, sha, sizeof(sha));
            c->p->files[idx].hashed = true;
        }
    }
    return KUI_PACK_OK;
}

/**
 * @brief The pack's own word on its scenes' canvas, when it gives one.
 *
 * Every scene plays on one canvas, so a pack can say which, and a pack drawn for
 * another one is refused here, before a single file is read.
 *
 * @param canvas The manifest's `canvas`, or NULL: older packs do not carry it.
 * @return KUI_PACK_OK; KUI_PACK_E_SCHEMA for a malformed one; KUI_PACK_E_CANVAS
 *         for one that is not this firmware's.
 */
static kui_pack_err_t pack_canvas_check(const cJSON *canvas)
{
    uint32_t w = 0U;
    uint32_t h = 0U;
    kui_pack_err_t err = KUI_PACK_OK;

    if ( NULL != canvas )
    {
        if ( !json_is_object(canvas)
            || !num_u(cJSON_GetObjectItemCaseSensitive(canvas, "w"), PACK_U16_MAX, &w)
            || !num_u(cJSON_GetObjectItemCaseSensitive(canvas, "h"), PACK_U16_MAX, &h) )
        {
            err = KUI_PACK_E_SCHEMA;
        }
        else if ( ((uint32_t)KUI_PACK_SCENE_W != w) || ((uint32_t)KUI_PACK_SCENE_H != h) )
        {
            err = KUI_PACK_E_CANVAS;
        }
        else
        {
            /* Drawn for this firmware's face. */
        }
    }
    return err;
}

/**
 * @brief Both passes over a parsed manifest.
 * @param root The document.
 * @param p    The pack, zeroed.
 * @return KUI_PACK_OK or the refusal.
 */
static kui_pack_err_t pack_build(const cJSON *root, kui_pack_t *p)
{
    static const char *const KNOWN[] = { "format", "name", "version", "canvas", "scenes", "face",
                                         "empty", "writing", "goodbye", "loading", "sha256" };
    /* The roles outside the face, in kui_pack_role_t's order. */
    static const char *const ROLES[KUI_PACK_ROLE_COUNT] = { "empty", "writing", "goodbye",
                                                            "loading" };
    pack_ctx_t c = { p, 0U, 0U };
    uint32_t fmt = 0U;
    uint32_t n_react = 0U;
    const cJSON *s = NULL;

    if ( !json_is_object(root) )
    {
        return KUI_PACK_E_SCHEMA;
    }
    if ( !num_u(cJSON_GetObjectItemCaseSensitive(root, "format"), PACK_U16_MAX, &fmt)
        || (KUI_PACK_FORMAT != fmt) )
    {
        return KUI_PACK_E_FORMAT;
    }
    kui_pack_err_t const fit = pack_canvas_check(cJSON_GetObjectItemCaseSensitive(root, "canvas"));
    if ( KUI_PACK_OK != fit )
    {
        return fit;
    }
    unknown_keys(root, KNOWN, sizeof(KNOWN) / sizeof(KNOWN[0]), "kode.json");
    const cJSON *const name = cJSON_GetObjectItemCaseSensitive(root, "name");
    const cJSON *const ver = cJSON_GetObjectItemCaseSensitive(root, "version");
    if ( ((NULL != name) && !text_copy(name, p->name, sizeof(p->name)))
        || ((NULL != ver) && !text_copy(ver, p->version, sizeof(p->version))) )
    {
        return KUI_PACK_E_SCHEMA;
    }

    /* First pass: how many of everything. */
    const cJSON *const scenes = cJSON_GetObjectItemCaseSensitive(root, "scenes");
    if ( !json_is_array(scenes) )
    {
        return KUI_PACK_E_SCHEMA;
    }
    int const n_scenes = cJSON_GetArraySize(scenes);
    if ( (1 > n_scenes) || ((int)KUI_PACK_SCENES_MAX < n_scenes) )
    {
        return KUI_PACK_E_SCHEMA;
    }
    cJSON_ArrayForEach(s, scenes) {
        const cJSON *const on = cJSON_GetObjectItemCaseSensitive(s, "on");

        for ( uint32_t t = 0U; (uint32_t)KUI_PACK_TRIGGER_COUNT > t; ++t )
        {
            const cJSON *const list = cJSON_GetObjectItemCaseSensitive(on, TRIGGERS[t]);

            if ( json_is_array(list) )
            {
                n_react += (uint32_t)cJSON_GetArraySize(list);
            }
        }
    }
    /* Bounded here, before anything is looked up: every name is checked
     * against the ones before it, so the work grows as the square of these. */
    if ( KUI_PACK_REACT_MAX < n_react )
    {
        return KUI_PACK_E_SCHEMA;
    }
    uint32_t const files_cap = (2U * (uint32_t)n_scenes) + n_react + (uint32_t)KUI_PACK_ROLE_COUNT;
    if ( KUI_PACK_NONE <= files_cap )
    {
        return KUI_PACK_E_SCHEMA;
    }
    c.files_cap = (uint16_t)files_cap;
    c.react_cap = (uint16_t)n_react;
    /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() answers void *: PSRAM is taken by name (rule 8) */
    p->files  = (kui_pack_file_t *)khal_alloc_psram(sizeof(uint32_t),
                                                    files_cap * sizeof(kui_pack_file_t));
    /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() answers void *: PSRAM is taken by name (rule 8) */
    p->scenes = (kui_pack_scene_t *)khal_alloc_psram(sizeof(uint32_t),
                                                     (size_t)n_scenes * sizeof(kui_pack_scene_t));
    if ( 0U < n_react )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() answers void *: PSRAM is taken by name (rule 8) */
        p->react = (uint16_t *)khal_alloc_psram(sizeof(uint32_t), n_react * sizeof(uint16_t));
    }
    if ( (NULL == p->files) || (NULL == p->scenes) || ((0U < n_react) && (NULL == p->react)) )
    {
        return KUI_PACK_E_MEMORY;
    }

    /* Second pass: fill. */
    cJSON_ArrayForEach(s, scenes) {
        kui_pack_err_t const err = scene_build(&c, s, p->n_scenes);

        if ( KUI_PACK_OK != err )
        {
            return err;
        }
        p->n_scenes++;
    }
    kui_pack_err_t err = face_build(&c, cJSON_GetObjectItemCaseSensitive(root, "face"));
    for ( uint32_t r = 0U; (KUI_PACK_OK == err) && ((uint32_t)KUI_PACK_ROLE_COUNT > r); ++r )
    {
        const cJSON *const j = cJSON_GetObjectItemCaseSensitive(root, ROLES[r]);

        p->role[r] = KUI_PACK_NONE;
        if ( NULL != j )
        {
            err = file_ref(&c, j, &p->role[r]);
        }
    }
    if ( KUI_PACK_OK == err )
    {
        err = hash_build(&c, cJSON_GetObjectItemCaseSensitive(root, "sha256"));
    }
    return err;
}

const char *kui_pack_err_word(kui_pack_err_t e)
{
    static const char *const WORDS[] = { "ok", "size", "depth", "json", "format", "schema",
                                         "name", "file", "hash", "canvas", "frames", "fps",
                                         "precomp", "memory" };
    const char *w = "error";

    if ( (uint32_t)e < (sizeof(WORDS) / sizeof(WORDS[0])) )
    {
        w = WORDS[e];
    }
    return w;
}

uint16_t kui_pack_json_depth(const char *text, size_t len)
{
    uint16_t depth = 0U;
    uint16_t deepest = 0U;
    bool in_str = false;
    bool escaped = false;

    for ( size_t i = 0U; (NULL != text) && (i < len); ++i )
    {
        char const ch = text[i];

        if ( in_str )
        {
            if ( escaped )
            {
                escaped = false;
            }
            else if ( '\\' == ch )
            {
                escaped = true;
            }
            else if ( '"' == ch )
            {
                in_str = false;
            }
            else
            {
                /* Inside a string: not structure. */
            }
        }
        else if ( '"' == ch )
        {
            in_str = true;
        }
        else if ( ('{' == ch) || ('[' == ch) )
        {
            if ( UINT16_MAX > depth )
            {
                depth++;
            }
            if ( depth > deepest )
            {
                deepest = depth;
            }
        }
        else if ( ('}' == ch) || (']' == ch) )
        {
            if ( 0U < depth )
            {
                depth--;
            }
        }
        else
        {
            /* A scalar, a separator or white space. */
        }
    }
    return deepest;
}

bool kui_pack_file_name_ok(const char *name)
{
    static const char *const ENDS[] = { ".json", ".json.z" };
    bool ok = false;

    if ( NULL != name )
    {
        size_t const n = strlen(name);

        ok = (0U < n) && (KUI_PACK_FILE_NAME_MAX >= n) && ('.' != name[0]);
        for ( size_t i = 0U; ok && (i < n); ++i )
        {
            char const ch = name[i];

            ok = (('a' <= ch) && ('z' >= ch)) || (('A' <= ch) && ('Z' >= ch))
                 || (('0' <= ch) && ('9' >= ch)) || ('_' == ch) || ('.' == ch) || ('-' == ch);
        }
        if ( ok )
        {
            bool ended = false;

            for ( size_t e = 0U; (sizeof(ENDS) / sizeof(ENDS[0])) > e; ++e )
            {
                size_t const end_len = strlen(ENDS[e]);

                if ( (n > end_len) && (strcmp(&name[n - end_len], ENDS[e]) == 0) )
                {
                    ended = true;
                }
            }
            ok = ended;
        }
    }
    return ok;
}

kui_pack_err_t kui_pack_parse(const char *text, size_t len, kui_pack_t *out)
{
    kui_pack_err_t err = KUI_PACK_E_JSON;

    if ( NULL == out )
    {
        return KUI_PACK_E_JSON;
    }
    (void)memset(out, 0, sizeof(*out));
    for ( uint32_t r = 0U; (uint32_t)KUI_PACK_ROLE_COUNT > r; ++r )
    {
        out->role[r] = KUI_PACK_NONE;
    }
    if ( NULL == text )
    {
        return KUI_PACK_E_JSON;
    }
    if ( (0U == len) || (KUI_PACK_MANIFEST_MAX < len) )
    {
        return KUI_PACK_E_SIZE;
    }
    if ( kui_pack_json_depth(text, len) > KUI_PACK_MAX_DEPTH )
    {
        return KUI_PACK_E_DEPTH;
    }
    pack_hooks();
    cJSON *const root = cJSON_ParseWithLength(text, len);
    if ( NULL == root )
    {
        return KUI_PACK_E_JSON;
    }
    err = pack_build(root, out);
    cJSON_Delete(root);
    if ( KUI_PACK_OK == err )
    {
        out->crc = khal_crc32(KHAL_CRC32_INIT, text, len);
    }
    else
    {
        kui_pack_free(out);
    }
    return err;
}

void kui_pack_free(kui_pack_t *p)
{
    if ( NULL != p )
    {
        khal_free(p->files);
        khal_free(p->scenes);
        khal_free(p->react);
        khal_free(p->windows);
        (void)memset(p, 0, sizeof(*p));
        for ( uint32_t r = 0U; (uint32_t)KUI_PACK_ROLE_COUNT > r; ++r )
        {
            p->role[r] = KUI_PACK_NONE;
        }
    }
}

uint16_t kui_pack_scene_find(const kui_pack_t *p, const char *name)
{
    uint16_t found = KUI_PACK_NONE;

    for ( uint16_t i = 0U; (NULL != p) && (NULL != name) && (KUI_PACK_NONE == found)
                          && (i < p->n_scenes); ++i )
    {
        if ( strcmp(p->scenes[i].name, name) == 0 )
        {
            found = i;
        }
    }
    return found;
}

uint16_t kui_pack_file_find(const kui_pack_t *p, const char *name)
{
    uint16_t found = KUI_PACK_NONE;

    for ( uint16_t i = 0U; (NULL != p) && (NULL != name) && (KUI_PACK_NONE == found)
                          && (i < p->n_files); ++i )
    {
        if ( strcmp(p->files[i].name, name) == 0 )
        {
            found = i;
        }
    }
    return found;
}

uint16_t kui_pack_scene_for_hour(const kui_pack_t *p, int8_t hour)
{
    uint16_t pick = 0U;

    if ( NULL != p )
    {
        bool found = false;

        pick = p->default_scene;
        for ( uint16_t i = 0U; (0 <= hour) && (!found) && (i < p->n_windows); ++i )
        {
            if ( ((uint8_t)hour >= p->windows[i].from) && ((uint8_t)hour < p->windows[i].to) )
            {
                pick = p->windows[i].scene;
                found = true;
            }
        }
    }
    return pick;
}

bool kui_pack_hashed(const kui_pack_t *p)
{
    bool every = (NULL != p) && (0U < p->n_files);

    for ( uint16_t i = 0U; every && (i < p->n_files); ++i )
    {
        every = p->files[i].hashed;
    }
    return every;
}

void kui_pack_role_canvas(kui_pack_role_t role, uint16_t *w, uint16_t *h)
{
    uint16_t cw = 0U;
    uint16_t ch = 0U;

    if ( (uint32_t)role < (uint32_t)KUI_PACK_ROLE_COUNT )
    {
        cw = ROLE_W[role];
        ch = ROLE_H[role];
    }
    if ( NULL != w )
    {
        *w = cw;
    }
    if ( NULL != h )
    {
        *h = ch;
    }
}

/**
 * @brief Record one use of a file, or say it conflicts with an earlier one.
 * @param seen   Whether a use was recorded already.
 * @param w,h,a  The recorded use.
 * @param nw,nh,na This use.
 * @return false when this use differs from the recorded one.
 */
static bool use_add(bool *seen, uint16_t *w, uint16_t *h, uint16_t *a,
                    uint16_t nw, uint16_t nh, uint16_t na)
{
    bool ok = true;

    if ( *seen )
    {
        ok = (*w == nw) && (*h == nh) && (*a == na);
    }
    else
    {
        *seen = true;
        *w = nw;
        *h = nh;
        *a = na;
    }
    return ok;
}

bool kui_pack_file_canvas(const kui_pack_t *p, uint16_t file, uint16_t *w, uint16_t *h,
                          uint16_t *anchor)
{
    bool seen = false;
    bool ok = (NULL != p) && (file < p->n_files) && (NULL != w) && (NULL != h) && (NULL != anchor);

    for ( uint16_t s = 0U; ok && (s < p->n_scenes); ++s )
    {
        const kui_pack_scene_t *const sc = &p->scenes[s];

        if ( sc->wake == file )
        {
            ok = use_add(&seen, w, h, anchor, KUI_PACK_SCENE_W, KUI_PACK_SCENE_H, 0U);
        }
        if ( ok && (sc->loop == file) )
        {
            ok = use_add(&seen, w, h, anchor, KUI_PACK_SCENE_W, KUI_PACK_SCENE_H, sc->anchor);
        }
        for ( uint32_t t = 0U; ok && ((uint32_t)KUI_PACK_TRIGGER_COUNT > t); ++t )
        {
            for ( uint16_t k = 0U; ok && (k < sc->on_count[t]); ++k )
            {
                if ( p->react[sc->on_first[t] + k] == file )
                {
                    ok = use_add(&seen, w, h, anchor, KUI_PACK_SCENE_W, KUI_PACK_SCENE_H, 0U);
                }
            }
        }
    }
    for ( uint32_t r = 0U; ok && ((uint32_t)KUI_PACK_ROLE_COUNT > r); ++r )
    {
        if ( p->role[r] == file )
        {
            ok = use_add(&seen, w, h, anchor, ROLE_W[r], ROLE_H[r], 0U);
        }
    }
    return ok && seen;
}

/**
 * @brief The index of the precomp asset @p id names, or -1.
 * @param assets The `assets` array, or NULL.
 * @param id     The `refId`.
 * @return The index of an asset with that id that holds layers; -1 otherwise.
 */
static int asset_index(const cJSON *assets, const char *id)
{
    int found = -1;
    int i = 0;
    const cJSON *a = NULL;

    cJSON_ArrayForEach(a, assets) {
        const cJSON *const aid = cJSON_GetObjectItemCaseSensitive(a, "id");

        if ( (0 > found) && json_is_string(aid) && (strcmp(aid->valuestring, id) == 0)
            && json_is_array(cJSON_GetObjectItemCaseSensitive(a, "layers")) )
        {
            found = i;
        }
        i++;
    }
    return found;
}

/**
 * @brief Mark in @p next every precomp asset a layer of @p layers refers to.
 * @param assets The `assets` array.
 * @param layers A `layers` array.
 * @param next   One flag per asset.
 * @return true when at least one was marked.
 */
static bool precomp_mark(const cJSON *assets, const cJSON *layers, uint8_t *next)
{
    bool any = false;
    const cJSON *l = NULL;

    cJSON_ArrayForEach(l, layers) {
        const cJSON *const ref = cJSON_GetObjectItemCaseSensitive(l, "refId");

        if ( json_is_string(ref) )
        {
            int const idx = asset_index(assets, ref->valuestring);

            if ( 0 <= idx )
            {
                next[idx] = 1U;
                any = true;
            }
        }
    }
    return any;
}

/**
 * @brief How deep the precomps nest, level by level, without recursion.
 *
 * Level 1 is every precomp the top layers name; level k+1 every precomp a
 * layer of a level-k precomp names. A cycle never empties a level, so it
 * runs past the bound like any chain that is too deep.
 *
 * @param root The document.
 * @param out  Receives the deepest level reached.
 * @return KUI_PACK_OK, KUI_PACK_E_PRECOMP past the bound, KUI_PACK_E_MEMORY.
 */
static kui_pack_err_t precomp_depth(const cJSON *root, uint16_t *out)
{
    const cJSON *const assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
    int const n = cJSON_GetArraySize(assets);
    kui_pack_err_t err = KUI_PACK_OK;

    *out = 0U;
    if ( 0 >= n )
    {
        return KUI_PACK_OK;
    }
    /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() answers void *: PSRAM is taken by name (rule 8) */
    uint8_t *cur = (uint8_t *)khal_alloc_psram(sizeof(uint32_t), (size_t)n);
    /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() answers void *: PSRAM is taken by name (rule 8) */
    uint8_t *next = (uint8_t *)khal_alloc_psram(sizeof(uint32_t), (size_t)n);
    if ( (NULL == cur) || (NULL == next) )
    {
        err = KUI_PACK_E_MEMORY;
    }
    else
    {
        bool any = precomp_mark(assets, cJSON_GetObjectItemCaseSensitive(root, "layers"), next);
        uint16_t level = 0U;

        while ( any && (KUI_PACK_OK == err) )
        {
            level++;
            *out = level;
            if ( KUI_PACK_MAX_PRECOMP < level )
            {
                err = KUI_PACK_E_PRECOMP;
            }
            else
            {
                uint8_t *const t = cur;
                int i = 0;
                const cJSON *a = NULL;

                cur = next;
                next = t;
                (void)memset(next, 0, (size_t)n);
                any = false;
                cJSON_ArrayForEach(a, assets) {
                    if ( 0U != cur[i] )
                    {
                        if ( precomp_mark(assets, cJSON_GetObjectItemCaseSensitive(a, "layers"), next) )
                        {
                            any = true;
                        }
                    }
                    i++;
                }
            }
        }
    }
    khal_free(cur);
    khal_free(next);
    return err;
}

/**
 * @brief The checks that need the parsed document.
 * @param root The document.
 * @param w,h  The role's canvas.
 * @param anchor The anchor the file plays with, or 0.
 * @param out  Filled as far as the checks get.
 * @return KUI_PACK_OK or the refusal.
 */
static kui_pack_err_t anim_build(const cJSON *root, uint16_t w, uint16_t h, uint16_t anchor,
                                 kui_pack_anim_t *out)
{
    uint32_t lw = 0U;
    uint32_t lh = 0U;
    uint32_t fr = 0U;
    uint32_t ip = 0U;
    uint32_t op = 0U;

    if ( !json_is_object(root) )
    {
        return KUI_PACK_E_JSON;
    }
    if ( !num_u(cJSON_GetObjectItemCaseSensitive(root, "w"), PACK_U16_MAX, &lw)
        || !num_u(cJSON_GetObjectItemCaseSensitive(root, "h"), PACK_U16_MAX, &lh) )
    {
        return KUI_PACK_E_CANVAS;
    }
    out->w = (uint16_t)lw;
    out->h = (uint16_t)lh;
    if ( ((uint32_t)w != lw) || ((uint32_t)h != lh) )
    {
        return KUI_PACK_E_CANVAS;
    }
    if ( !num_u(cJSON_GetObjectItemCaseSensitive(root, "fr"), KUI_PACK_FPS_MAX, &fr) || (0U == fr) )
    {
        return KUI_PACK_E_FPS;
    }
    out->fps = (uint8_t)fr;
    if ( !num_u(cJSON_GetObjectItemCaseSensitive(root, "ip"), PACK_FRAME_NUM_MAX, &ip)
        || !num_u(cJSON_GetObjectItemCaseSensitive(root, "op"), PACK_FRAME_NUM_MAX, &op) || (op <= ip)
        || ((op - ip) > KUI_PACK_FRAMES_MAX) )
    {
        return KUI_PACK_E_FRAMES;
    }
    out->frames = (uint16_t)(op - ip);
    if ( (0U != anchor) && ((out->frames % anchor) != 0U) )
    {
        return KUI_PACK_E_FRAMES;
    }
    if ( !json_is_array(cJSON_GetObjectItemCaseSensitive(root, "layers")) )
    {
        return KUI_PACK_E_JSON;
    }
    return precomp_depth(root, &out->precomp);
}

kui_pack_err_t kui_pack_check_anim(const char *json, size_t len, uint16_t w, uint16_t h,
                                   uint16_t anchor, kui_pack_anim_t *out)
{
    kui_pack_err_t err = KUI_PACK_E_JSON;

    if ( NULL == out )
    {
        return KUI_PACK_E_JSON;
    }
    (void)memset(out, 0, sizeof(*out));
    if ( NULL == json )
    {
        return KUI_PACK_E_JSON;
    }
    if ( (0U == len) || (KUI_PACK_JSON_MAX < len) )
    {
        return KUI_PACK_E_SIZE;
    }
    out->depth = kui_pack_json_depth(json, len);
    if ( KUI_PACK_MAX_DEPTH < out->depth )
    {
        return KUI_PACK_E_DEPTH;
    }
    pack_hooks();
    cJSON *const root = cJSON_ParseWithLength(json, len);
    if ( NULL == root )
    {
        return KUI_PACK_E_JSON;
    }
    err = anim_build(root, w, h, anchor, out);
    cJSON_Delete(root);
    return err;
}
