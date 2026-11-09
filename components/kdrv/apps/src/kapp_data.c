/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kapp_data.c
 * @brief The app.json subset reader, described at https://docs.kode.diy/sdk/kdrv/apps.
 */
#include "kapp_data.h"
#include "kapp_cat.h"
#include <string.h>

/** @brief The cursor: where we are and where the text ends. */
typedef struct {
    const char *p;
    const char *end;
} cur_t;

/** @brief Advance the cursor past spaces, tabs, CRs and LFs. */
static void skip_ws(cur_t *c)
{
    while ( (c->p < c->end) && ((' ' == *c->p) || ('\t' == *c->p)
                               || ('\n' == *c->p) || ('\r' == *c->p)) )
    {
        ++c->p;
    }
}

/** @brief Consume @p ch after any whitespace. @return false and leave the cursor put when it is not there. */
static bool take(cur_t *c, char ch)
{
    skip_ws(c);
    if ( (c->p < c->end) && (*c->p == ch) )
    {
        ++c->p;
        return true;
    }
    return false;
}

/**
 * @brief Read one "string" into @p out (at most @p cap-1 chars).
 * @return false on a missing quote, a control byte, an unknown escape or a
 *         string longer than the cap: length is a refusal, not a cut.
 */
static bool take_string(cur_t *c, char *out, size_t cap)
{
    size_t n = 0U;
    if ( !take(c, '"') )
    {
        return false;
    }
    while ( c->p < c->end )
    {
        char ch = *c->p++;
        if ( '"' == ch )
        {
            out[n] = '\0';
            return true;
        }
        if ( '\\' == ch )
        {
            if ( c->p >= c->end )
            {
                return false;
            }
            ch = *c->p++;
            if ( ('"' != ch) && ('\\' != ch) )
            {
                return false;
            }
        }
        else if ( (unsigned char)ch < 0x20U )
        {
            return false;
        }
        else
        {
            /* an ordinary byte */
        }
        if ( (cap - 1U) <= n )
        {
            return false;
        }
        out[n++] = ch;
    }
    return false;   /* ran out before the closing quote */
}

/** @brief Read the two hex digits at @p s into @p out. @return false on a non-hex byte. */
static bool hex_pair(const char *s, uint8_t *out)
{
    uint8_t v = 0U;
    for ( uint8_t i = 0U; 2U > i; ++i )
    {
        char const ch = s[i];
        uint8_t d;
        if ( ('0' <= ch) && ('9' >= ch) )      { d = (uint8_t)(ch - '0'); }
        else if ( ('a' <= ch) && ('f' >= ch) ) { d = (uint8_t)(10 + (ch - 'a')); }
        else if ( ('A' <= ch) && ('F' >= ch) ) { d = (uint8_t)(10 + (ch - 'A')); }
        else                                 { return false; }
        v = (uint8_t)((v << 4) | d);
    }
    *out = v;
    return true;
}

/** @brief A relative path: no leading slash, no backslash, no ".." piece. */
static bool icon_path_ok(const char *s)
{
    if ( ('/' == s[0]) || (strchr(s, '\\') != NULL) )
    {
        return false;
    }
    for ( const char *q = s; '\0' != *q; ++q )
    {
        if ( ('.' == q[0]) && ('.' == q[1])
            && ((q == s) || ('/' == q[-1])) && (('/' == q[2]) || ('\0' == q[2])) )
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief Zero @p out and hand back @p st.
 *
 * Every mid-parse refusal goes through this rather than a bare return, so a
 * caller can never see a struct half-filled by the fields read before the
 * refusing one - the file's siblings avoid goto, and a struct that is
 * already zeroed at the top of kapp_data_parse() makes a second memset here
 * cheap enough not to earn one anyway.
 */
static kapp_json_status_t fail(kapp_data_t *out, kapp_json_status_t st)
{
    (void)memset(out, 0, sizeof(*out));
    return st;
}

kapp_json_status_t kapp_data_parse(const char *text, size_t len, kapp_data_t *out)
{
    cur_t c;
    /* Both wide enough to hold anything the size cap below could ever let
     * through, so an unknown key is never refused for being long - only an
     * unknown key is possible past this width, since every key this reader
     * acts on is a handful of characters. */
    char  key[KAPP_DATA_JSON_MAX];
    char  val[KAPP_DATA_JSON_MAX];
    bool  kode_seen = false;
    bool  kode_ok   = false;
    bool  first     = true;

    (void)memset(out, 0, sizeof(*out));
    if ( KAPP_DATA_JSON_MAX < len )
    {
        return KAPP_JSON_SIZE;
    }
    c.p   = text;
    c.end = text + len;
    if ( !take(&c, '{') )
    {
        return fail(out, KAPP_JSON_SYNTAX);
    }
    while ( !take(&c, '}') )
    {
        if ( !first && !take(&c, ',') )
        {
            return fail(out, KAPP_JSON_SYNTAX);
        }
        first = false;
        if ( !take_string(&c, key, sizeof(key)) )
        {
            return fail(out, KAPP_JSON_SYNTAX);
        }
        if ( !take(&c, ':') )
        {
            return fail(out, KAPP_JSON_SYNTAX);
        }
        if ( !take_string(&c, val, sizeof(val)) )
        {
            return fail(out, KAPP_JSON_SYNTAX);   /* a number, array, object or bad string */
        }
        if ( strcmp(key, "kode") == 0 )
        {
            kode_seen = true;
            kode_ok   = (strcmp(val, "1") == 0);
        }
        else if ( strcmp(key, "name") == 0 )
        {
            if ( strlen(val) >= KAPP_NAME_MAX ) { return fail(out, KAPP_JSON_SYNTAX); }
            (void)strcpy(out->name, val);
        }
        else if ( strcmp(key, "category") == 0 )
        {
            if ( strlen(val) >= KAPP_CATEGORY_MAX ) { return fail(out, KAPP_JSON_SYNTAX); }
            kapp_cat_normalise(val, out->category, sizeof(out->category));
        }
        else if ( strcmp(key, "version") == 0 )
        {
            if ( strlen(val) >= KAPP_DATA_VERSION_MAX ) { return fail(out, KAPP_JSON_SYNTAX); }
            (void)strcpy(out->version, val);
        }
        else if ( strcmp(key, "description") == 0 )
        {
            if ( strlen(val) > 52U ) { return fail(out, KAPP_JSON_SYNTAX); }
            (void)strcpy(out->description, val);
        }
        else if ( strcmp(key, "icon") == 0 )
        {
            if ( (strlen(val) >= KAPP_DATA_ICON_MAX) || !icon_path_ok(val) )
            {
                return fail(out, KAPP_JSON_SYNTAX);
            }
            (void)strcpy(out->icon, val);
        }
        else if ( strcmp(key, "color") == 0 )
        {
            if ( (strlen(val) != 7U) || ('#' != val[0])
                || !hex_pair(&val[1], &out->color[0])
                || !hex_pair(&val[3], &out->color[1])
                || !hex_pair(&val[5], &out->color[2]) )
            {
                return fail(out, KAPP_JSON_SYNTAX);
            }
            out->color_valid = true;
        }
        else
        {
            /* unknown: ignored, whatever its length, so a newer tool's
             * file still works on an older board */
        }
    }
    skip_ws(&c);
    if ( c.p != c.end )
    {
        return fail(out, KAPP_JSON_SYNTAX);   /* trailing bytes after the object */
    }
    if ( !kode_seen || !kode_ok )
    {
        return fail(out, KAPP_JSON_REVISION);
    }
    return KAPP_JSON_OK;
}

const char *kapp_json_status_name(kapp_json_status_t st)
{
    static const char *const NAME[] = { "ok", "none", "size", "syntax", "revision" };
    return ((unsigned)st < (sizeof(NAME) / sizeof(NAME[0]))) ? NAME[st] : "?";
}
