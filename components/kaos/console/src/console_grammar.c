/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file console_grammar.c
 * @brief The console's grammar: splitting a line, and reading and writing a quoted string.
 */
#include "console_grammar.h"

/** @brief Whether @p c separates two tokens. */
static bool is_sep(char c)
{
    bool sep = false;
    if ( (' ' == c) || ('\t' == c) )
    {
        sep = true;
    }
    return sep;
}

/** @brief Whether @p c goes out as a two-byte escape. */
static bool needs_escape(char c)
{
    bool esc = false;
    if ( ('"' == c) || ('\\' == c) || ('\n' == c) )
    {
        esc = true;
    }
    return esc;
}

/**
 * @brief Whether the byte after a backslash is one of the three escapes.
 *
 * @param c The byte after the backslash.
 * @return true for `"`, `\\` and `n`.
 */
static bool is_escape(char c)
{
    bool esc = false;
    if ( ('"' == c) || ('\\' == c) || ('n' == c) )
    {
        esc = true;
    }
    return esc;
}

/**
 * @brief Walk a quoted value from its opening quote to its closing one.
 *
 * A backslash takes the byte after it along, so an escaped quote never closes
 * the value. A backslash before anything but the three escapes is malformed.
 *
 * @param open The opening quote.
 * @return The closing quote, or NULL when the line ends first or an escape is
 *         unknown.
 */
static char *closing_quote(char *open)
{
    char *p = &open[1];

    while ( (NULL != p) && ('\0' != *p) && ('"' != *p) )
    {
        if ( '\\' != p[0] )
        {
            ++p;
        }
        else if ( is_escape(p[1]) )
        {
            p = &p[2];
        }
        else
        {
            p = NULL;
        }
    }
    if ( (NULL != p) && ('\0' == *p) )
    {
        p = NULL;
    }
    return p;
}

console_tok_t console_token(char **cursor, char **tok)
{
    char *p = *cursor;

    while ( is_sep(*p) )
    {
        ++p;
    }
    if ( '\0' == *p )
    {
        *cursor = p;
        return CONSOLE_TOK_END;
    }

    *tok = p;
    while ( ('\0' != *p) && !is_sep(*p) && ('=' != *p) )
    {
        ++p;
    }
    if ( ('=' == p[0]) && ('"' == p[1]) )
    {
        char * const eq    = p;
        char * const close = closing_quote(&p[1]);
        if ( (NULL == close) || (('\0' != close[1]) && !is_sep(close[1])) )
        {
            /* Cut at the `=`: the key is all a refusal may carry. */
            *eq     = '\0';
            *cursor = &eq[1];
            return CONSOLE_TOK_BAD;
        }
        p = &close[1];
    }
    else
    {
        while ( ('\0' != *p) && !is_sep(*p) )
        {
            ++p;
        }
    }

    if ( '\0' != *p )
    {
        *p      = '\0';
        *cursor = &p[1];
    }
    else
    {
        *cursor = p;
    }
    return CONSOLE_TOK_WORD;
}

bool console_unquote(char *val)
{
    if ( '"' != val[0] )
    {
        return false;
    }
    char * const close = closing_quote(val);
    if ( (NULL == close) || ('\0' != close[1]) )
    {
        return false;
    }

    char       *w = val;
    const char *r = &val[1];
    while ( r < close )
    {
        if ( ('\\' == r[0]) && ('"' == r[1]) )
        {
            *w = '"';
            r  = &r[2];
        }
        else if ( ('\\' == r[0]) && ('\\' == r[1]) )
        {
            *w = '\\';
            r  = &r[2];
        }
        else if ( ('\\' == r[0]) && ('n' == r[1]) )
        {
            *w = '\n';
            r  = &r[2];
        }
        else
        {
            *w = *r;
            ++r;
        }
        ++w;
    }
    *w = '\0';
    return true;
}

const char *console_quote(char *dst, size_t cap, const char *src)
{
    size_t w = 0U;

    if ( 3U > cap )
    {
        if ( 0U < cap )
        {
            dst[0] = '\0';
        }
        return dst;
    }

    dst[w] = '"';
    ++w;
    if ( NULL != src )
    {
        for ( size_t r = 0U; '\0' != src[r]; ++r )
        {
            char const c    = src[r];
            size_t     need = 1U;
            if ( needs_escape(c) )
            {
                need = 2U;
            }
            /* Room for this byte or pair, the closing quote and the
             * terminator - or the value ends here, whole escapes only. */
            if ( (w + need + 2U) > cap )
            {
                break;
            }
            if ( 2U == need )
            {
                dst[w] = '\\';
                ++w;
                if ( '\n' == c )
                {
                    dst[w] = 'n';
                }
                else
                {
                    dst[w] = c;
                }
            }
            else
            {
                dst[w] = c;
            }
            ++w;
        }
    }
    dst[w] = '"';
    ++w;
    dst[w] = '\0';
    return dst;
}

size_t console_quoted_size(const char *src)
{
    size_t n = 3U;

    if ( NULL != src )
    {
        for ( size_t r = 0U; '\0' != src[r]; ++r )
        {
            if ( needs_escape(src[r]) )
            {
                n += 2U;
            }
            else
            {
                n += 1U;
            }
        }
    }
    return n;
}
