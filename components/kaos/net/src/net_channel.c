/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file net_channel.c
 * @brief The Kode channel's pure logic. The contract is net_channel.h.
 */
#include "net_channel.h"

#include <string.h>

#include "kaos.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "kode_wire.h"
#include "kqp_config.h"

/** @brief Whether @p c is JSON white space. */
static bool chan_is_ws(char c)
{
    bool ws = false;
    if ( (' ' == c) || ('\t' == c) || ('\n' == c) || ('\r' == c) )
    {
        ws = true;
    }
    return ws;
}

/** @brief Move @p p past white space, stopping at @p len. */
static size_t chan_skip_ws(const char *json, size_t len, size_t p)
{
    size_t q = p;
    while ( (q < len) && chan_is_ws(json[q]) )
    {
        q++;
    }
    return q;
}

/**
 * @brief Read the string whose opening quote is just before @p p into @p out.
 *
 * @param json The text.
 * @param len  Its length.
 * @param p    Index of the first character after the opening quote.
 * @param out  Receives the unescaped value and a terminator.
 * @param cap  Bytes @p out holds.
 * @return true when the closing quote was reached inside the buffer.
 */
static bool chan_read_value(const char *json, size_t len, size_t p, char *out,
                       size_t cap)
{
    bool   ok   = true;
    bool   done = false;
    size_t n    = 0U;
    size_t i    = p;

    while ( ok && !done )
    {
        char c = '\0';
        if ( i >= len )
        {
            ok = false;
        }
        else
        {
            c = json[i];
        }
        if ( ok && ('"' == c) )
        {
            done = true;
        }
        else if ( ok )
        {
            if ( '\\' == c )
            {
                char e = '\0';
                if ( (i + 1U) < len )
                {
                    e = json[i + 1U];
                }
                if ( ('"' == e) || ('\\' == e) || ('/' == e) )
                {
                    c = e;
                    i += 2U;
                }
                else
                {
                    ok = false;
                }
            }
            else if ( ((unsigned char)c) < 0x20U )
            {
                ok = false;
            }
            else
            {
                i++;
            }
            if ( ok && ((n + 1U) >= cap) )
            {
                ok = false;
            }
            if ( ok )
            {
                out[n] = c;
                n++;
            }
        }
        else
        {
            /* Refused above. */
        }
    }
    if ( ok )
    {
        out[n] = '\0';
    }
    return ok;
}

/** @brief Whether the first @p n bytes of @p a and @p b are the same. */
static bool chan_same(const char *a, const char *b, size_t n)
{
    bool same = true;

    for ( size_t i = 0U; same && (i < n); i++ )
    {
        same = (a[i] == b[i]);
    }
    return same;
}

/**
 * @brief Find where @p key's value starts.
 *
 * The key is looked up by the strings followed by a colon, first occurrence
 * first.
 *
 * @param json The text; not NULL.
 * @param len  Its length.
 * @param key  The key, without quotes; not NULL, not empty.
 * @param at   Receives the index of the value's first byte, past white space.
 * @return true when the key was found; false when the text ended first, or a
 *         string in it before the key went unterminated.
 */
static bool chan_find_value(const char *json, size_t len, const char *key,
                            size_t *at)
{
    bool         ok     = true;
    bool         found  = false;
    size_t const keylen = strlen(key);
    size_t       i      = 0U;

    while ( ok && !found && (i < len) )
    {
        if ( '"' != json[i] )
        {
            i++;
        }
        else
        {
            size_t const start = i + 1U;
            size_t       j     = start;

            /* The string's end: a backslash takes the next byte with it. */
            while ( (j < len) && ('"' != json[j]) )
            {
                if ( '\\' == json[j] )
                {
                    j += 2U;
                }
                else
                {
                    j++;
                }
            }
            if ( j >= len )
            {
                ok = false;   /* Unterminated. */
            }
            else
            {
                size_t const colon = chan_skip_ws(json, len, j + 1U);
                if ( (colon < len) && (':' == json[colon])
                    && ((j - start) == keylen)
                    && chan_same(&json[start], key, keylen) )
                {
                    *at   = chan_skip_ws(json, len, colon + 1U);
                    found = true;
                }
                else
                {
                    i = j + 1U;
                }
            }
        }
    }
    return ok && found;
}

bool kchan_json_str(const char *json, size_t len, const char *key,
                    char *out, size_t cap)
{
    bool   ok = (NULL != json) && (NULL != key) && (NULL != out)
                && (0U < cap);
    size_t v  = 0U;

    if ( ok )
    {
        out[0] = '\0';
        ok = ('\0' != key[0]);
    }
    if ( ok )
    {
        ok = chan_find_value(json, len, key, &v);
    }
    if ( ok )
    {
        if ( (v < len) && ('"' == json[v]) )
        {
            ok = chan_read_value(json, len, v + 1U, out, cap);
        }
        else
        {
            ok = false;   /* Not a string. */
        }
    }
    if ( !ok && (NULL != out) && (0U < cap) )
    {
        out[0] = '\0';
    }
    return ok;
}

/** @brief The numeric base of a JSON number. */
#define CHAN_DECIMAL  10U

/** @brief Whether @p c may follow a number in a JSON text. */
static bool chan_ends_number(char c)
{
    return (',' == c) || ('}' == c) || (']' == c) || chan_is_ws(c);
}

bool kchan_json_u32(const char *json, size_t len, const char *key,
                    uint32_t *out)
{
    bool     ok     = (NULL != json) && (NULL != key) && (NULL != out);
    size_t   v      = 0U;
    size_t   digits = 0U;
    uint32_t n      = 0U;

    if ( ok )
    {
        *out = 0U;
        ok = ('\0' != key[0]);
    }
    if ( ok )
    {
        ok = chan_find_value(json, len, key, &v);
    }
    while ( ok && (v < len) && ('0' <= json[v]) && ('9' >= json[v]) )
    {
        uint32_t const d = (uint32_t)((uint8_t)json[v]) - (uint32_t)((uint8_t)'0');

        if ( ((UINT32_MAX - d) / CHAN_DECIMAL) < n )
        {
            ok = false;   /* Past 32 bits. */
        }
        else
        {
            n = (n * CHAN_DECIMAL) + d;
            digits++;
            v++;
        }
    }
    if ( ok )
    {
        ok = (0U < digits) && (v < len) && chan_ends_number(json[v]);
    }
    if ( ok )
    {
        *out = n;
    }
    return ok;
}

bool kshot_token_valid(const char *token)
{
    bool ok = false;

    if ( NULL != token )
    {
        size_t len = 0U;

        ok = true;
        /* Stop at the terminator, a byte outside the set, or one byte past
         * the longest token. */
        while ( ok && (KSHOT_TOKEN_MAX >= len) && ('\0' != token[len]) )
        {
            char const c = token[len];

            ok = (('a' <= c) && ('z' >= c)) || (('A' <= c) && ('Z' >= c))
                 || (('0' <= c) && ('9' >= c)) || ('_' == c) || ('-' == c);
            len++;
        }
        ok = ok && (0U < len) && (KSHOT_TOKEN_MAX >= len);
    }
    return ok;
}

/** @brief Whether @p s is not empty. */
static bool chan_filled(const char *s)
{
    return '\0' != s[0];
}

/** @brief The scheme a broker must have: MQTT over TLS. */
#define CHAN_BROKER_SCHEME "mqtts://"

/** @brief Is @p t a topic with no MQTT wildcard in it? */
static bool chan_topic_plain(const char *t)
{
    return (strchr(t, '+') == NULL) && (strchr(t, '#') == NULL);
}

/** @brief The scheme an upload prefix must have: TLS. */
#define CHAN_SHOTS_SCHEME "https://"

/** @brief The lowest byte an upload prefix may hold (below it: space, controls). */
#define CHAN_SHOTS_BYTE_MIN 0x21U

bool kchan_shots_valid(const char *prefix)
{
    bool ok = false;

    if ( NULL != prefix )
    {
        size_t len = 0U;

        /* Stop at the terminator, or one byte past the longest prefix kept. */
        while ( (KSHOT_PREFIX_MAX >= len) && ('\0' != prefix[len]) )
        {
            len++;
        }
        size_t const scheme = sizeof(CHAN_SHOTS_SCHEME) - 1U;

        /* A host byte after the scheme: with none, esp_http_client cannot
         * parse the URL and its error line prints it whole, token and all. */
        ok = (len > scheme) && (KSHOT_PREFIX_MAX >= len)
             && (strncmp(prefix, CHAN_SHOTS_SCHEME, scheme) == 0)
             && ('/' != prefix[scheme])
             && ('/' == prefix[len - 1U]);
        for ( size_t i = 0U; ok && (i < len); i++ )
        {
            ok = ((uint8_t)prefix[i] >= CHAN_SHOTS_BYTE_MIN);
        }
    }
    return ok;
}

bool kchan_parse_connect(const char *json, size_t len, const char *own,
                         kchan_session_t *out)
{
    bool ok = false;

    if ( NULL != out )
    {
        (void)memset(out, 0, sizeof(*out));
        ok = kchan_json_str(json, len, "serial", out->serial, sizeof(out->serial))
          && kchan_json_str(json, len, "brokerUrl", out->broker, sizeof(out->broker))
          && kchan_json_str(json, len, "username", out->user, sizeof(out->user))
          && kchan_json_str(json, len, "clientId", out->client_id, sizeof(out->client_id))
          && kchan_json_str(json, len, "token", out->token, sizeof(out->token))
          && kchan_json_str(json, len, "in", out->t_in, sizeof(out->t_in))
          && kchan_json_str(json, len, "out", out->t_out, sizeof(out->t_out))
          && kchan_json_str(json, len, "status", out->t_status, sizeof(out->t_status));
        if ( ok )
        {
            ok = chan_filled(out->serial) && chan_filled(out->broker) && chan_filled(out->user)
                 && chan_filled(out->client_id) && chan_filled(out->token)
                 && chan_filled(out->t_in) && chan_filled(out->t_out)
                 && chan_filled(out->t_status);
        }
        if ( ok )
        {
            ok = (strncmp(out->broker, CHAN_BROKER_SCHEME,
                          sizeof(CHAN_BROKER_SCHEME) - 1U) == 0)
                 && chan_topic_plain(out->t_in) && chan_topic_plain(out->t_out)
                 && chan_topic_plain(out->t_status);
        }
        if ( ok && (NULL != own) && ('\0' != own[0]) )
        {
            ok = (strcmp(out->serial, own) == 0);
        }
        if ( ok )
        {
            /* Optional: absent, too long or unfit leaves it empty, no uploads. */
            if ( !kchan_json_str(json, len, "shots", out->shots, sizeof(out->shots))
                || !kchan_shots_valid(out->shots) )
            {
                (void)memset(out->shots, 0, sizeof(out->shots));
            }
        }
        if ( !ok )
        {
            (void)memset(out, 0, sizeof(*out));
        }
    }
    return ok;
}

bool kchan_parse_nonce(const char *json, size_t len, char *nonce, size_t cap)
{
    bool ok = kchan_json_str(json, len, "nonce", nonce, cap);

    if ( ok )
    {
        ok = kode_nonce_ok(nonce, strlen(nonce));
        if ( !ok )
        {
            nonce[0] = '\0';
        }
    }
    return ok;
}

/** @brief A body being written into a caller's buffer. */
typedef struct {
    char  *buf;   /**< The buffer.                                   */
    size_t cap;   /**< Its size.                                     */
    size_t n;     /**< Bytes written so far, terminator excluded.    */
    bool   ok;    /**< False once anything did not fit or was unfit. */
} chan_body_t;

/** @brief Append @p s verbatim. */
static void body_lit(chan_body_t *b, const char *s)
{
    size_t const l = strlen(s);

    if ( b->ok && ((b->n + l + 1U) <= b->cap) )
    {
        (void)memcpy(&b->buf[b->n], s, l);
        b->n += l;
    }
    else
    {
        b->ok = false;
    }
}

/** @brief Append @p s, refusing a character JSON would have to escape. */
static void body_str(chan_body_t *b, const char *s)
{
    if ( NULL == s )
    {
        b->ok = false;
    }
    else
    {
        for ( size_t i = 0U; b->ok && ('\0' != s[i]); i++ )
        {
            char const c = s[i];
            if ( ('"' == c) || ('\\' == c) || (((unsigned char)c) < 0x20U) )
            {
                b->ok = false;
            }
        }
        body_lit(b, s);
    }
}

/** @brief Close a body: its length, or 0 with the buffer emptied. */
static size_t body_end(chan_body_t *b)
{
    size_t n = 0U;

    if ( b->ok )
    {
        b->buf[b->n] = '\0';
        n = b->n;
    }
    else if ( (NULL != b->buf) && (0U < b->cap) )
    {
        b->buf[0] = '\0';
    }
    else
    {
        /* No buffer, or no room even for a terminator. */
    }
    return n;
}

size_t kchan_connect_body(char *out, size_t cap, const char *cert_b64,
                          const char *nonce, const char *sig_b64)
{
    chan_body_t b = { out, cap, 0U, (NULL != out) && (0U < cap) };

    body_lit(&b, "{\"cert\":\"");
    body_str(&b, cert_b64);
    body_lit(&b, "\",\"nonce\":\"");
    body_str(&b, nonce);
    body_lit(&b, "\",\"signature\":\"");
    body_str(&b, sig_b64);
    body_lit(&b, "\"}");
    return body_end(&b);
}

size_t kchan_status_body(char *out, size_t cap, const char *app,
                         const char *version)
{
    chan_body_t b = { out, cap, 0U, (NULL != out) && (0U < cap) };

    body_lit(&b, "{\"online\":true,\"app\":\"");
    body_str(&b, app);
    body_lit(&b, "\",\"version\":\"");
    body_str(&b, version);
    body_lit(&b, "\"}");
    return body_end(&b);
}

kchan_verdict_t kchan_classify(uint16_t http_status, const char *error_word)
{
    kchan_verdict_t v = KCHAN_RETRY;

    if ( (401U == http_status) && (NULL != error_word) )
    {
        if ( (strcmp(error_word, "bad_cert") == 0)
            || (strcmp(error_word, "untrusted_cert") == 0)
            || (strcmp(error_word, "cert_expired") == 0)
            || (strcmp(error_word, "not_client_cert") == 0) )
        {
            v = KCHAN_TERMINAL;
        }
    }
    return v;
}

uint32_t kchan_backoff_ms(uint8_t attempt, uint32_t rnd)
{
    /* Doubled only while under the cap, so 5000 can never be shifted or
     * multiplied out of 32 bits however many attempts have failed. */
    uint32_t base = KCHAN_BACKOFF_MIN_MS;

    for ( uint8_t i = 0U; (i < attempt) && (KCHAN_BACKOFF_MAX_MS > base); i++ )
    {
        base *= 2U;
    }
    if ( KCHAN_BACKOFF_MAX_MS < base )
    {
        base = KCHAN_BACKOFF_MAX_MS;
    }
    /* 600000 * 125 = 75,000,000: inside 32 bits. */
    return (base * (75U + (rnd % 51U))) / 100U;
}

/* ---- The reply ring ------------------------------------------------------- */

/** @brief Bytes in front of each line in the ring: its length, little endian. */
#define OUT_HDR  2U

/** @brief The ring's state. Written under @c lock, except @c wake_owed. */
typedef struct {
    uint8_t         *buf;     /**< KCHAN_OUT_BYTES of PSRAM; NULL before init. */
    khal_mutex_t     lock;
    uint32_t         head;    /**< Where the next record is written.          */
    uint32_t         tail;    /**< Where the oldest record starts.            */
    uint32_t         used;    /**< Bytes held, headers included.              */
    uint32_t         lines;   /**< Records held.                              */
    uint32_t         lost;    /**< Lines lost since boot.                     */
    kchan_out_wake_t wake;    /**< Told when the ring stops being empty.      */
    bool             wake_owed; /**< The last wake could not be delivered.    */
} chan_ring_t;

static chan_ring_t s_replies;

/** @brief Copy @p n bytes into the ring at @p at, wrapping. */
static void ring_put(uint32_t at, const uint8_t *src, uint32_t n)
{
    for ( uint32_t i = 0U; i < n; i++ )
    {
        s_replies.buf[(at + i) % KCHAN_OUT_BYTES] = src[i];
    }
}

/** @brief Copy @p n bytes out of the ring from @p at, wrapping. */
static void ring_get(uint32_t at, uint8_t *dst, uint32_t n)
{
    for ( uint32_t i = 0U; i < n; i++ )
    {
        dst[i] = s_replies.buf[(at + i) % KCHAN_OUT_BYTES];
    }
}

bool kchan_out_init(void)
{
    if ( NULL == s_replies.buf )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; PSRAM storage aligned for any object, and used as bytes */
        uint8_t * const mem = (uint8_t *)khal_alloc_psram(4U, KCHAN_OUT_BYTES);
        if ( NULL != mem )
        {
            if ( NULL == s_replies.lock )
            {
                s_replies.lock = khal_mutex_create();
            }
            if ( NULL == s_replies.lock )
            {
                khal_free(mem);
            }
            else
            {
                s_replies.head  = 0U;
                s_replies.tail  = 0U;
                s_replies.used  = 0U;
                s_replies.lines = 0U;
                s_replies.buf   = mem;
            }
        }
    }
    return NULL != s_replies.buf;
}

void kchan_out_set_wake(kchan_out_wake_t wake)
{
    s_replies.wake = wake;
}

bool kchan_out_push(const char *line, size_t len)
{
    bool stored    = false;
    bool tell      = false;
    bool ring_live = (NULL != s_replies.buf);

    if ( ring_live )
    {
        khal_mutex_lock(s_replies.lock);
        if ( (0U < len) && ((size_t)KODE_LOG_LINE_MAX >= len)
            && ((s_replies.used + OUT_HDR + (uint32_t)len) <= KCHAN_OUT_BYTES) )
        {
            uint8_t hdr[OUT_HDR];

            hdr[0] = (uint8_t)(len & 0xFFU);
            hdr[1] = (uint8_t)((len >> 8) & 0xFFU);
            ring_put(s_replies.head, hdr, OUT_HDR);
            ring_put((s_replies.head + OUT_HDR) % KCHAN_OUT_BYTES,
                     (const uint8_t *)line, (uint32_t)len);
            s_replies.head = (s_replies.head + OUT_HDR + (uint32_t)len) % KCHAN_OUT_BYTES;
            tell = (0U == s_replies.lines) || s_replies.wake_owed;
            s_replies.used += OUT_HDR + (uint32_t)len;
            s_replies.lines++;
            stored = true;
        }
        else
        {
            s_replies.lost++;
        }
        khal_mutex_unlock(s_replies.lock);
    }
    else
    {
        s_replies.lost++;
    }
    if ( tell && (NULL != s_replies.wake) )
    {
        s_replies.wake_owed = !s_replies.wake();
    }
    return stored;
}

size_t kchan_out_pop(char *line, size_t cap)
{
    size_t got = 0U;

    if ( (NULL == s_replies.buf) || (NULL == line) || (0U == cap) )
    {
        return 0U;
    }
    khal_mutex_lock(s_replies.lock);
    if ( 0U < s_replies.lines )
    {
        uint8_t  hdr[OUT_HDR];
        uint32_t n;
        uint32_t keep;

        ring_get(s_replies.tail, hdr, OUT_HDR);
        n    = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8);
        keep = n;
        if ( (uint32_t)(cap - 1U) < keep )
        {
            keep = (uint32_t)(cap - 1U);
        }
        ring_get((s_replies.tail + OUT_HDR) % KCHAN_OUT_BYTES, (uint8_t *)line, keep);
        line[keep] = '\0';
        s_replies.tail = (s_replies.tail + OUT_HDR + n) % KCHAN_OUT_BYTES;
        s_replies.used -= OUT_HDR + n;
        s_replies.lines--;
        got = keep;
    }
    khal_mutex_unlock(s_replies.lock);
    return got;
}

bool kchan_out_pending(void)
{
    bool waiting = false;

    if ( NULL != s_replies.buf )
    {
        khal_mutex_lock(s_replies.lock);
        waiting = (0U < s_replies.lines);
        khal_mutex_unlock(s_replies.lock);
    }
    return waiting;
}

uint32_t kchan_out_lost(void)
{
    return s_replies.lost;
}

void kchan_out_lose(uint32_t n)
{
    if ( NULL != s_replies.buf )
    {
        khal_mutex_lock(s_replies.lock);
        s_replies.lost += n;
        khal_mutex_unlock(s_replies.lock);
    }
}

void kchan_out_clear(void)
{
    if ( NULL != s_replies.buf )
    {
        khal_mutex_lock(s_replies.lock);
        s_replies.lost += s_replies.lines;
        s_replies.head  = 0U;
        s_replies.tail  = 0U;
        s_replies.used  = 0U;
        s_replies.lines = 0U;
        khal_mutex_unlock(s_replies.lock);
    }
}
