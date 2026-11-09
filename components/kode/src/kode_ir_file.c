/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_ir_file.c
 * @brief The capture file's text, written and read.
 */
#include "kode_ir_file.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "kode_entry.h"

/** @brief The keyword of a decoded NEC block. */
#define IR_FILE_NEC "nec"

/** @brief The keyword of a raw block. */
#define IR_FILE_RAW "raw"

/** @brief Durations the writer puts on a line. */
#define IR_FILE_PER_LINE 16U

/** @brief Digits of the longest carrier, 1000000. */
#define IR_FILE_CARRIER_DIGITS 7U

/** @brief Digits of the longest duration, 65535. */
#define IR_FILE_VALUE_DIGITS 5U

/** @brief Characters of "raw " and the newline after the carrier. */
#define IR_FILE_RAW_LINE_FIXED 5U

/** @brief The blank line before a block, in characters. */
#define IR_FILE_BLANK 1U

/** @brief Characters a number takes in the writer's scratch, NUL included. */
#define IR_FILE_NUMBER_BYTES 12U

/** @brief Base of a decimal number. */
#define IR_FILE_DECIMAL 10U

/** @brief Base of a hexadecimal number. */
#define IR_FILE_HEX 16U

/** @brief A number that grew past this is out of range whatever follows. */
#define IR_FILE_NUMBER_SATURATED 0xFFFFFFFFULL

_Static_assert(KODE_IR_FILE_SIGNALS_MAX == 32U, "the text of the signal limit says 32");
_Static_assert(KODE_IR_RAW_MAX == 512U, "the text of the duration limit says 512");
_Static_assert(KODE_IR_FILE_VALUE_MAX == 65535U, "the text of the value limit says 65535");
_Static_assert(KODE_IR_FILE_CARRIER_MAX == 1000000U, "the text of the carrier limit says 1000000");

/** @brief Why a line is refused. */
typedef enum {
    WHY_KEYWORD,         /**< The keyword is neither nec nor raw.                */
    WHY_NEC_FEW,         /**< A nec line lacks its address or its command.       */
    WHY_NEC_MANY,        /**< A nec line has more than an address and a command. */
    WHY_NUMBER,          /**< A word is no number.                               */
    WHY_NEC_RANGE,       /**< An address or a command is out of range.           */
    WHY_CARRIER_NONE,    /**< A raw line lacks its carrier.                      */
    WHY_CARRIER_RANGE,   /**< The carrier is out of range.                       */
    WHY_DURATION_RANGE,  /**< A duration is out of range.                        */
    WHY_DURATIONS_NONE,  /**< A raw block has no duration.                       */
    WHY_DURATIONS_MANY,  /**< A raw block has too many durations.                */
    WHY_SIGNALS_MANY,    /**< The file has too many signals.                     */
    WHY_BLOCK,           /**< Two signals share a block.                         */
    WHY_COUNT            /**< How many reasons there are.                        */
} why_t;

/** @brief The first line of every file the writer makes. */
static const char s_header[] = "# kode ir capture v1\n";

/* ---- The writer ------------------------------------------------------------- */

/** @brief Where the writer is. */
typedef struct {
    char    *out;  /**< The text.                  */
    uint32_t cap;  /**< Its room.                  */
    uint32_t len;  /**< Written so far.            */
    bool     ok;   /**< Everything written fit.    */
} writer_t;

/**
 * @brief Add bytes to the text, if they fit; a NUL follows when there is room for it.
 * @param w    The writer.
 * @param data The bytes.
 * @param n    How many.
 */
static void put_bytes(writer_t *w, const char *data, uint32_t n)
{
    if ( w->ok && ((w->cap - w->len) >= n) )
    {
        (void)memcpy(&w->out[w->len], data, n);
        w->len += n;
        if ( w->len < w->cap )
        {
            w->out[w->len] = '\0';
        }
    }
    else
    {
        w->ok = false;
    }
}

/** @brief Add a text to the writer's. */
static void put_text(writer_t *w, const char *text)
{
    put_bytes(w, text, (uint32_t)strlen(text));
}

/** @brief Add a decimal number to the writer's text. */
static void put_dec(writer_t *w, uint32_t v)
{
    char tmp[IR_FILE_NUMBER_BYTES];
    const int32_t got = snprintf(tmp, sizeof(tmp), "%" PRIu32, v);

    if ( 0 < got )
    {
        put_bytes(w, tmp, (uint32_t)got);
    }
    else
    {
        w->ok = false;
    }
}

/** @brief Add 0x and four hex digits, upper case, to the writer's text. */
static void put_hex(writer_t *w, uint32_t v)
{
    char tmp[IR_FILE_NUMBER_BYTES];
    const int32_t got = snprintf(tmp, sizeof(tmp), "0x%04" PRIX32, v);

    if ( 0 < got )
    {
        put_bytes(w, tmp, (uint32_t)got);
    }
    else
    {
        w->ok = false;
    }
}

/**
 * @brief Whether a signal is one the reader accepts.
 * @param s The signal.
 * @return true when it is.
 */
static bool signal_valid(const kode_ir_signal_t *s)
{
    bool ok = true;

    if ( !s->raw )
    {
        ok = (KODE_IR_FILE_VALUE_MAX >= s->address) && (KODE_IR_FILE_VALUE_MAX >= s->command);
    }
    else
    {
        ok = (1U <= s->carrier_hz) && (KODE_IR_FILE_CARRIER_MAX >= s->carrier_hz)
             && (1U <= s->count) && (KODE_IR_RAW_MAX >= s->count);
        for ( uint32_t i = 0U; ok && (i < s->count); i++ )
        {
            ok = (1U <= s->us[i]);
        }
    }
    return ok;
}

/**
 * @brief Write one block, with the blank line before it.
 * @param w The writer.
 * @param s The signal, valid.
 */
static void put_block(writer_t *w, const kode_ir_signal_t *s)
{
    put_text(w, "\n");
    if ( !s->raw )
    {
        put_text(w, IR_FILE_NEC " ");
        put_hex(w, s->address);
        put_text(w, " ");
        put_hex(w, s->command);
        put_text(w, "\n");
    }
    else
    {
        put_text(w, IR_FILE_RAW " ");
        put_dec(w, s->carrier_hz);
        put_text(w, "\n");
        for ( uint32_t i = 0U; i < s->count; i++ )
        {
            const bool end = ((i + 1U) == s->count) || (((i + 1U) % IR_FILE_PER_LINE) == 0U);

            put_dec(w, s->us[i]);
            if ( end )
            {
                put_text(w, "\n");
            }
            else
            {
                put_text(w, " ");
            }
        }
    }
}

uint32_t kode_ir_file_format(const kode_ir_signal_t *sig, uint32_t n, char *out, uint32_t cap)
{
    writer_t w = { out, cap, 0U, true };

    if ( (NULL == sig) || (NULL == out) || (KODE_IR_FILE_SIGNALS_MAX < n) )
    {
        w.ok = false;
    }
    for ( uint32_t i = 0U; w.ok && (i < n); i++ )
    {
        w.ok = signal_valid(&sig[i]);
    }
    put_text(&w, s_header);
    for ( uint32_t i = 0U; w.ok && (i < n); i++ )
    {
        put_block(&w, &sig[i]);
    }
    if ( !w.ok )
    {
        w.len = 0U;
    }
    return w.len;
}

uint32_t kode_ir_file_size_max(void)
{
    const uint32_t durations = KODE_IR_RAW_MAX * (IR_FILE_VALUE_DIGITS + 1U);
    const uint32_t block     = IR_FILE_BLANK + IR_FILE_RAW_LINE_FIXED + IR_FILE_CARRIER_DIGITS + durations;

    return (uint32_t)(sizeof(s_header) - 1U) + (KODE_IR_FILE_SIGNALS_MAX * block);
}

/* ---- The reader ------------------------------------------------------------- */

/** @brief What a number is. */
typedef enum {
    NUM_OK,     /**< A number within the limit.     */
    NUM_BAD,    /**< Not a number.                  */
    NUM_RANGE   /**< A number past the limit.       */
} num_t;

/** @brief Which block the reader is in. */
typedef enum {
    BLOCK_NONE,  /**< Between blocks.                          */
    BLOCK_NEC,   /**< A NEC block that has its one line.       */
    BLOCK_RAW    /**< A raw block taking durations.            */
} block_t;

/** @brief Where the reader is in the text. */
typedef struct {
    kode_ir_signal_t *sig;         /**< Where signals go.                         */
    uint32_t          cap;         /**< How many it holds.                        */
    uint32_t          n;           /**< Signals begun.                            */
    block_t           block;       /**< The block open.                           */
    uint32_t          block_line;  /**< The line the open block began at.         */
    uint32_t          line;        /**< The line being read, from 1.              */
    const char       *why;         /**< What is wrong; NULL: nothing.             */
    uint32_t          bad_line;    /**< Where.                                    */
} reader_t;

/** @brief A word of a line. */
typedef struct {
    const char *text;  /**< Its first character.  */
    uint32_t    len;   /**< Its characters; 0: none left on the line. */
} token_t;

/** @brief The rest of a line, for taking its words one at a time. */
typedef struct {
    const char *text;  /**< The line.                         */
    uint32_t    len;   /**< Its characters.                   */
    uint32_t    at;    /**< The next one to look at.          */
} cursor_t;

/** @brief Whether a character separates words. */
static bool ir_is_blank(char c)
{
    return (' ' == c) || ('\t' == c);
}

/**
 * @brief Take the next word of a line.
 * @param c The cursor.
 * @return The word; len 0 when the line has no more.
 */
static token_t token_next(cursor_t *c)
{
    token_t t = { NULL, 0U };

    while ( (c->at < c->len) && ir_is_blank(c->text[c->at]) )
    {
        c->at++;
    }
    t.text = &c->text[c->at];
    while ( ((c->at + t.len) < c->len) && !ir_is_blank(c->text[c->at + t.len]) )
    {
        t.len++;
    }
    c->at += t.len;
    return t;
}

/**
 * @brief The value of a digit.
 * @param c    The character, in either case.
 * @param base 10 or 16.
 * @return The value, or UINT32_MAX when @p c is no digit of that base.
 */
static uint32_t file_digit_value(char c, uint32_t base)
{
    static const char digits[] = "0123456789abcdef";  /* in order of value */
    uint32_t          v        = UINT32_MAX;

    for ( uint32_t i = 0U; (i < base) && (UINT32_MAX == v); i++ )
    {
        if ( kode_ascii_lower(c) == digits[i] )
        {
            v = i;
        }
    }
    return v;
}

/**
 * @brief Read a decimal or 0x hex number.
 * @param t   The word.
 * @param max The highest value allowed.
 * @param out Out: the value, when NUM_OK.
 * @return NUM_OK, NUM_BAD for anything that is not a number, NUM_RANGE past @p max.
 */
static num_t number_read(token_t t, uint32_t max, uint32_t *out)
{
    uint32_t base  = IR_FILE_DECIMAL;
    uint32_t first = 0U;
    uint64_t v     = 0U;
    num_t    r     = NUM_OK;

    if ( (2U <= t.len) && ('0' == t.text[0]) && (('x' == t.text[1]) || ('X' == t.text[1])) )
    {
        base  = IR_FILE_HEX;
        first = 2U;
    }
    if ( first >= t.len )
    {
        r = NUM_BAD;
    }
    for ( uint32_t i = first; (NUM_OK == r) && (i < t.len); i++ )
    {
        const uint32_t d = file_digit_value(t.text[i], base);

        if ( UINT32_MAX == d )
        {
            r = NUM_BAD;
        }
        else if ( IR_FILE_NUMBER_SATURATED >= v )
        {
            v = (v * base) + d;
        }
        else
        {
            /* already past any limit */
        }
    }
    if ( (NUM_OK == r) && (v > max) )
    {
        r = NUM_RANGE;
    }
    if ( NUM_OK == r )
    {
        *out = (uint32_t)v;
    }
    return r;
}

/**
 * @brief Whether a word spells a keyword, in any case.
 * @param t    The word.
 * @param word The keyword in lower case.
 * @return true when it does.
 */
static bool token_is(token_t t, const char *word)
{
    bool same = ((uint32_t)strlen(word) == t.len);

    for ( uint32_t i = 0U; same && (i < t.len); i++ )
    {
        same = (kode_ascii_lower(t.text[i]) == word[i]);
    }
    return same;
}

/** @brief Remember what is wrong, the first time. */
static void reader_fail(reader_t *r, uint32_t line, why_t why)
{
    /* The numbers in the texts are the limits above. */
    static const char *const text[WHY_COUNT] = {
        [WHY_KEYWORD]        = "unknown signal type (use nec or raw)",
        [WHY_NEC_FEW]        = "nec needs an address and a command",
        [WHY_NEC_MANY]       = "nec takes only an address and a command",
        [WHY_NUMBER]         = "not a number",
        [WHY_NEC_RANGE]      = "out of range (0 to 65535)",
        [WHY_CARRIER_NONE]   = "raw needs a carrier in Hz",
        [WHY_CARRIER_RANGE]  = "carrier out of range (1 to 1000000)",
        [WHY_DURATION_RANGE] = "duration out of range (1 to 65535)",
        [WHY_DURATIONS_NONE] = "raw has no durations",
        [WHY_DURATIONS_MANY] = "raw holds at most 512 durations",
        [WHY_SIGNALS_MANY]   = "a file holds at most 32 signals",
        [WHY_BLOCK]          = "leave a blank line between signals",
    };

    if ( NULL == r->why )
    {
        r->why      = text[why];
        r->bad_line = line;
    }
}

/** @brief Close the open block: a raw block with no duration is wrong. */
static void block_close(reader_t *r)
{
    if ( (BLOCK_RAW == r->block) && (0U == r->sig[r->n - 1U].count) )
    {
        reader_fail(r, r->block_line, WHY_DURATIONS_NONE);
    }
    r->block = BLOCK_NONE;
}

/**
 * @brief Read the words left on a line as durations of the open raw block.
 * @param r The reader.
 * @param c The line's cursor.
 */
static void durations_read(reader_t *r, cursor_t *c)
{
    kode_ir_signal_t *const s = &r->sig[r->n - 1U];
    token_t                 t = token_next(c);

    while ( (0U != t.len) && (NULL == r->why) )
    {
        uint32_t    v = 0U;
        const num_t k = number_read(t, KODE_IR_FILE_VALUE_MAX, &v);

        if ( NUM_BAD == k )
        {
            reader_fail(r, r->line, WHY_NUMBER);
        }
        else if ( (NUM_RANGE == k) || (1U > v) )
        {
            reader_fail(r, r->line, WHY_DURATION_RANGE);
        }
        else if ( KODE_IR_RAW_MAX <= s->count )
        {
            reader_fail(r, r->line, WHY_DURATIONS_MANY);
        }
        else
        {
            s->us[s->count] = (uint16_t)v;
            s->count++;
            t = token_next(c);
        }
    }
}

/**
 * @brief Read the rest of a NEC line: its address and its command.
 * @param r The reader.
 * @param c The line's cursor, after the keyword.
 */
static void nec_read(reader_t *r, cursor_t *c)
{
    const token_t a     = token_next(c);
    const token_t m     = token_next(c);
    const token_t extra = token_next(c);
    uint32_t      address = 0U;
    uint32_t      command = 0U;

    if ( (0U == a.len) || (0U == m.len) )
    {
        reader_fail(r, r->line, WHY_NEC_FEW);
    }
    else if ( 0U != extra.len )
    {
        reader_fail(r, r->line, WHY_NEC_MANY);
    }
    else
    {
        const num_t ka = number_read(a, KODE_IR_FILE_VALUE_MAX, &address);
        const num_t km = number_read(m, KODE_IR_FILE_VALUE_MAX, &command);

        if ( (NUM_BAD == ka) || (NUM_BAD == km) )
        {
            reader_fail(r, r->line, WHY_NUMBER);
        }
        else if ( (NUM_RANGE == ka) || (NUM_RANGE == km) )
        {
            reader_fail(r, r->line, WHY_NEC_RANGE);
        }
        else
        {
            r->sig[r->n - 1U].address = address;
            r->sig[r->n - 1U].command = command;
            r->block                  = BLOCK_NEC;
        }
    }
}

/**
 * @brief Read the rest of a raw line: its carrier, and any durations after it.
 * @param r The reader.
 * @param c The line's cursor, after the keyword.
 */
static void raw_read(reader_t *r, cursor_t *c)
{
    const token_t t       = token_next(c);
    uint32_t      carrier = 0U;

    if ( 0U == t.len )
    {
        reader_fail(r, r->line, WHY_CARRIER_NONE);
    }
    else
    {
        const num_t k = number_read(t, KODE_IR_FILE_CARRIER_MAX, &carrier);

        if ( NUM_BAD == k )
        {
            reader_fail(r, r->line, WHY_NUMBER);
        }
        else if ( (NUM_RANGE == k) || (1U > carrier) )
        {
            reader_fail(r, r->line, WHY_CARRIER_RANGE);
        }
        else
        {
            r->sig[r->n - 1U].raw        = true;
            r->sig[r->n - 1U].carrier_hz = carrier;
            r->block                     = BLOCK_RAW;
            durations_read(r, c);
        }
    }
}

/**
 * @brief Begin a block on its keyword line.
 * @param r The reader.
 * @param c The line's cursor.
 * @param k The keyword word.
 */
static void block_begin(reader_t *r, cursor_t *c, token_t k)
{
    const bool nec = token_is(k, IR_FILE_NEC);

    if ( !nec && !token_is(k, IR_FILE_RAW) )
    {
        reader_fail(r, r->line, WHY_KEYWORD);
    }
    else if ( r->n >= r->cap )
    {
        reader_fail(r, r->line, WHY_SIGNALS_MANY);
    }
    else
    {
        (void)memset(&r->sig[r->n], 0, sizeof(r->sig[r->n]));
        r->n++;
        r->block_line = r->line;
        if ( nec )
        {
            nec_read(r, c);
        }
        else
        {
            raw_read(r, c);
        }
    }
}

/**
 * @brief Read one line.
 * @param r    The reader.
 * @param text The line.
 * @param len  Its characters, a closing carriage return included.
 */
static void line_read(reader_t *r, const char *text, uint32_t len)
{
    const char comment_mark = '#';
    cursor_t   c = { text, len, 0U };
    token_t    first;

    if ( (0U < len) && ('\r' == text[len - 1U]) )
    {
        c.len--;
    }
    first = token_next(&c);
    if ( 0U == first.len )
    {
        block_close(r);
    }
    else if ( first.text[0] == comment_mark )
    {
        /* a comment is no part of any block */
    }
    else if ( BLOCK_NONE == r->block )
    {
        block_begin(r, &c, first);
    }
    else if ( (BLOCK_NEC == r->block) || token_is(first, IR_FILE_NEC) || token_is(first, IR_FILE_RAW) )
    {
        reader_fail(r, r->line, WHY_BLOCK);
    }
    else
    {
        c.at = 0U;
        durations_read(r, &c);
    }
}

bool kode_ir_file_parse(const char *text, uint32_t len, kode_ir_signal_t *sig, uint32_t cap,
                        uint32_t *n, uint32_t *bad_line, const char **why)
{
    reader_t r  = { sig, cap, 0U, BLOCK_NONE, 0U, 0U, NULL, 0U };
    bool     ok = false;

    if ( (NULL != text) && (NULL != sig) && (NULL != n) && (NULL != bad_line) && (NULL != why) )
    {
        uint32_t at = 0U;

        while ( (at < len) && (NULL == r.why) )
        {
            uint32_t end = at;

            while ( (end < len) && ('\n' != text[end]) )
            {
                end++;
            }
            r.line++;
            line_read(&r, &text[at], end - at);
            at = end + 1U;
        }
        if ( NULL == r.why )
        {
            block_close(&r);
        }
        ok = (NULL == r.why);
        if ( ok )
        {
            *n = r.n;
        }
        else
        {
            *bad_line = r.bad_line;
            *why      = r.why;
        }
    }
    return ok;
}
