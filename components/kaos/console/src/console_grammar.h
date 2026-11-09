/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file console_grammar.h
 * @brief The console's grammar: a command key's shape, splitting a line, and quoted strings.
 */
#ifndef CONSOLE_GRAMMAR_H_
#define CONSOLE_GRAMMAR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Room for a value of @p n characters once console_quote() has written
 *        it: every character escaped, both quotes and the terminator.
 */
#define CONSOLE_QUOTED_MAX(n)  ((2U * (size_t)(n)) + 3U)

/** @brief What a value must parse as, and so which form it is typed in. */
typedef enum {
    KARG_UINT = 0,  /**< Unsigned decimal, bounded by spec min/max. Bare.   */
    KARG_BOOL,      /**< `on` or `off`. Bare.                               */
    KARG_ENUM,      /**< One member of the spec's word list. Bare.          */
    KARG_STRING,    /**< Quoted, and not empty: `path="/storage/a b.txt"`.  */
    KARG_RGB,       /**< Three comma-separated 0..255, e.g. `0,0,200`. Bare. */
    KARG_TEXT,      /**< Quoted, and may be empty: `text=""` is a value,
                     *   the text a file or a tag is left holding.          */
    KARG_ROW,       /**< Either form: the registry row decides. The handler
                     *   checks it; the parser reports which form arrived.  */
} karg_type_t;

/** @brief One key a command accepts, and how to validate its value. */
typedef struct {
    const char        *key;      /**< The word left of `=`.                */
    bool               required; /**< Absent -> `>err code=2 msg=<key>`.   */
    karg_type_t        type;     /**< How to parse the value.              */
    uint32_t           min;      /**< KARG_UINT lower bound.               */
    uint32_t           max;      /**< KARG_UINT upper bound.               */
    const char *const *enums;    /**< KARG_ENUM members.                   */
    uint8_t            n_enums;  /**< How many members.                   */
    const char        *def;      /**< Default, for help; NULL: none. */
} karg_spec_t;

/** @brief Keys any one command may declare: the length of a line's value array. */
#define KARG_KEYS_MAX  6U

/** @brief One key's parsed value, indexed like the command's spec array. */
typedef struct {
    bool        present;  /**< The key appeared on the line.               */
    uint32_t    u;        /**< KARG_UINT value, or KARG_ENUM member index. */
    bool        b;        /**< KARG_BOOL value.                            */
    const char *s;        /**< KARG_STRING, KARG_TEXT or KARG_ROW value,
                           *   quotes and escapes already read (points into
                           *   the line).                                  */
    bool        quoted;   /**< KARG_ROW: the value arrived in quotes.      */
    uint8_t     rgb[3];   /**< KARG_RGB components.                        */
} karg_val_t;

/** @brief What console_token() found. */
typedef enum {
    CONSOLE_TOK_END = 0,  /**< Nothing but separators was left.            */
    CONSOLE_TOK_WORD,     /**< One token, terminated in place.             */
    CONSOLE_TOK_BAD,      /**< A malformed quoted value: unclosed, an
                           *   unknown escape, or junk after its close.    */
} console_tok_t;

/**
 * @brief Cut the next token off @p *cursor, in place.
 *
 * Spaces and tabs separate tokens, except inside a quoted value: a quote right
 * after a token's first `=` opens one, running to the next unescaped quote. A
 * quote anywhere else is an ordinary byte. Quotes and escapes are left in
 * place; console_unquote() reads them.
 *
 * @param cursor Where to start. Advanced past the token and its separator.
 * @param tok    Out: the token, null-terminated. For CONSOLE_TOK_BAD, the key
 *               alone, so a refusal never echoes the value (it may be a password).
 * @return What was found.
 */
console_tok_t console_token(char **cursor, char **tok);

/**
 * @brief Read a quoted value in place: `"..."` becomes what the quotes hold,
 *        with the three escapes turned into the bytes they name.
 *
 * @param val A value exactly as console_token() left it. Modified in place
 *            when it is quoted; the result is never longer than the input.
 * @return true when @p val was one well-formed quoted value; false, and
 *         @p val untouched, otherwise.
 */
bool console_unquote(char *val);

/**
 * @brief Write @p src into @p dst as a quoted value.
 *
 * `"`, `\` and a newline are escaped. A value too long for @p dst loses its
 * tail but never its closing quote or half an escape. Size @p dst with
 * CONSOLE_QUOTED_MAX() and nothing is cut.
 *
 * @param dst Where to write. At least 3 bytes: two quotes and a terminator.
 * @param cap Its size.
 * @param src The value. NULL is written as an empty value.
 * @return @p dst, for use inside a reply() argument list.
 */
const char *console_quote(char *dst, size_t cap, const char *src);

/**
 * @brief The bytes console_quote() needs to write @p src whole: both quotes,
 *        every escape, and the terminator.
 *
 * Lets a caller tell a cut value from a whole one.
 *
 * @param src The value. NULL counts as empty.
 * @return The size a buffer needs; 3 for an empty value.
 */
size_t console_quoted_size(const char *src);

#ifdef __cplusplus
}
#endif

#endif /* CONSOLE_GRAMMAR_H_ */
