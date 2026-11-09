/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_multitap.c
 * @brief Multitap keypad text rules: the character table, the pending character, the cap, the wipe.
 */
#include "kui_multitap.h"

#include <string.h>

/* The text is a secret: nothing here logs or prints it. */
/**
 * @brief Zeroes every byte of the text buffer in a way the compiler may not drop.
 *
 * @param m The multitap.
 */
static void multitap_wipe_text(kui_multitap_t *m)
{
    volatile char *p = m->text;

    for ( size_t i = 0U; i < sizeof(m->text); i++ )
    {
        p[i] = '\0';
    }
}

/**
 * @brief The cap a value stands for.
 *
 * @param cap The wanted cap.
 * @return `cap` from 1 to KUI_MULTITAP_TEXT_MAX; KUI_MULTITAP_TEXT_MAX for anything else.
 */
static uint8_t multitap_cap(uint8_t cap)
{
    if ( (0U == cap) || (cap > KUI_MULTITAP_TEXT_MAX) )
    {
        return (uint8_t)KUI_MULTITAP_TEXT_MAX;
    }
    return cap;
}

/**
 * @brief Empties the text and the state, wiping the buffer, and sets the cap.
 *
 * @param m The multitap.
 * @param cap The cap, already in range.
 */
static void multitap_reset(kui_multitap_t *m, uint8_t cap)
{
    multitap_wipe_text(m);
    m->len = 0U;
    m->cap = cap;
    m->used = false;
    m->mode = (uint8_t)KUI_MULTITAP_MODE_LOWER;
    m->key = 0U;
    m->idx = 0U;
    m->pending = false;
    m->fresh = false;
    m->press_ms = 0U;
}

/**
 * @brief Whether a multitap was started and holds a state it can act on.
 *
 * @param m The multitap.
 * @return true for a non-NULL multitap whose cap is in range and whose text fits it.
 */
static bool multitap_usable(const kui_multitap_t *m)
{
    return (NULL != m) && (m->cap >= 1U) && (m->cap <= KUI_MULTITAP_TEXT_MAX) && (m->len <= m->cap);
}

/**
 * @brief Commits the pending character when its window has passed.
 *
 * @param m The multitap.
 * @param t_ms The clock now.
 */
static void multitap_expire(kui_multitap_t *m, uint32_t t_ms)
{
    if ( m->pending && ((t_ms - m->press_ms) >= KUI_MULTITAP_MS) )
    {
        m->pending = false;
    }
}

/**
 * @brief The mode key: commits the pending character and steps the mode.
 *
 * @param m The multitap.
 * @return KUI_MULTITAP_STEPPED.
 */
static kui_multitap_result_t multitap_step(kui_multitap_t *m)
{
    /* Somebody who went to another mode and typed in it is done with it: the
     * next press is back to lower case, not a walk through the others. */
    unsigned int next = m->used ? (unsigned int)KUI_MULTITAP_MODE_LOWER
                                : ((unsigned int)m->mode + 1U);

    m->used = false;

    if ( next >= KUI_MULTITAP_MODES )
    {
        next = 0U;
    }
    m->pending = false;
    m->mode = (uint8_t)next;
    return KUI_MULTITAP_STEPPED;
}

/**
 * @brief Delete: removes the last character, pending or committed, and leaves no trace of it.
 *
 * @param m The multitap.
 * @return KUI_MULTITAP_DELETED, or KUI_MULTITAP_EMPTY when nothing is typed.
 */
static kui_multitap_result_t multitap_delete(kui_multitap_t *m)
{
    if ( 0U == m->len )
    {
        return KUI_MULTITAP_EMPTY;
    }
    m->len--;
    m->text[m->len] = '\0';
    m->key = 0U;
    m->idx = 0U;
    m->pending = false;
    m->fresh = false;
    return KUI_MULTITAP_DELETED;
}

/**
 * @brief A character key: cycles the pending character when it is this key's, else adds a character.
 *
 * @param m The multitap.
 * @param key A character key.
 * @param t_ms The clock now.
 * @return KUI_MULTITAP_CYCLED, KUI_MULTITAP_TYPED, KUI_MULTITAP_FULL, or KUI_MULTITAP_IGNORED when the mode holds no table row.
 */
static kui_multitap_result_t multitap_type(kui_multitap_t *m, kui_multitap_key_t key, uint32_t t_ms)
{
    const char *const chars = kui_multitap_chars(key, (kui_multitap_mode_t)m->mode);
    size_t n;

    if ( NULL == chars )
    {
        return KUI_MULTITAP_IGNORED;
    }
    n = strlen(chars);

    if ( m->pending && (m->len > 0U) && (m->key == (uint8_t)key) )
    {
        size_t next = (size_t)m->idx + 1U;

        if ( next >= n )
        {
            next = 0U;
        }
        m->text[m->len - 1U] = chars[next];
        m->idx = (uint8_t)next;
        m->fresh = true;
        m->press_ms = t_ms;
        m->used = (m->mode != (uint8_t)KUI_MULTITAP_MODE_LOWER);
        return KUI_MULTITAP_CYCLED;
    }

    /* Another key: whatever was pending is text from here on, even when the new character is refused. */
    m->pending = false;
    if ( m->len >= m->cap )
    {
        return KUI_MULTITAP_FULL;
    }
    m->text[m->len] = chars[0];
    m->len++;
    m->key = (uint8_t)key;
    m->idx = 0U;
    m->pending = (n > 1U);
    m->fresh = true;
    m->press_ms = t_ms;
    m->used = (m->mode != (uint8_t)KUI_MULTITAP_MODE_LOWER);
    return KUI_MULTITAP_TYPED;
}

void kui_multitap_init(kui_multitap_t *m, uint8_t cap)
{
    if ( NULL == m )
    {
        return;
    }
    multitap_reset(m, multitap_cap(cap));
}

kui_multitap_result_t kui_multitap_press(kui_multitap_t *m, kui_multitap_key_t key, uint32_t t_ms)
{
    if ( !multitap_usable(m) || ((uint32_t)key >= KUI_MULTITAP_KEYS) )
    {
        return KUI_MULTITAP_IGNORED;
    }
    multitap_expire(m, t_ms);
    if ( KUI_MULTITAP_KEY_MODE == key )
    {
        return multitap_step(m);
    }
    if ( KUI_MULTITAP_KEY_DELETE == key )
    {
        return multitap_delete(m);
    }
    return multitap_type(m, key, t_ms);
}

void kui_multitap_tick(kui_multitap_t *m, uint32_t t_ms)
{
    if ( NULL == m )
    {
        return;
    }
    multitap_expire(m, t_ms);
}

const char *kui_multitap_text(const kui_multitap_t *m)
{
    if ( NULL == m )
    {
        return "";
    }
    return m->text;
}

size_t kui_multitap_len(const kui_multitap_t *m)
{
    if ( NULL == m )
    {
        return 0U;
    }
    return m->len;
}

bool kui_multitap_pending(const kui_multitap_t *m)
{
    return (NULL != m) && m->pending;
}

bool kui_multitap_show_plain(const kui_multitap_t *m, uint32_t t_ms)
{
    if ( (NULL == m) || !m->fresh )
    {
        return false;
    }
    if ( m->pending )
    {
        return true;
    }
    return (t_ms - m->press_ms) < KUI_MULTITAP_SHOW_MS;
}

kui_multitap_mode_t kui_multitap_mode(const kui_multitap_t *m)
{
    if ( (NULL == m) || (m->mode >= KUI_MULTITAP_MODES) )
    {
        return KUI_MULTITAP_MODE_LOWER;
    }
    return (kui_multitap_mode_t)m->mode;
}

const char *kui_multitap_chars(kui_multitap_key_t key, kui_multitap_mode_t mode)
{
    /* The characters of each character key, by key and by mode: lower case, upper case, digits, symbols. A press
     * types the first one; the presses after it cycle through the rest and wrap. A string of one character is a key
     * that does not cycle. The symbols column holds the 32 printable ASCII symbols, each once. */
    static const char *const chars[KUI_MULTITAP_CHAR_KEYS][KUI_MULTITAP_MODES] = {
        /* KUI_MULTITAP_KEY_0 */ { " 0", " 0", "0", " 0" },
        /* KUI_MULTITAP_KEY_1 */ { ".@!#?-_1", ".@!#?-_1", "1", ".,?!1" },
        /* KUI_MULTITAP_KEY_2 */ { "abc2", "ABC2", "2", "@#$%2" },
        /* KUI_MULTITAP_KEY_3 */ { "def3", "DEF3", "3", "&*+=3" },
        /* KUI_MULTITAP_KEY_4 */ { "ghi4", "GHI4", "4", "-_/\\" "4" },
        /* KUI_MULTITAP_KEY_5 */ { "jkl5", "JKL5", "5", "()[]5" },
        /* KUI_MULTITAP_KEY_6 */ { "mno6", "MNO6", "6", "{}<>6" },
        /* KUI_MULTITAP_KEY_7 */ { "pqrs7", "PQRS7", "7", ":;'\"7" },
        /* KUI_MULTITAP_KEY_8 */ { "tuv8", "TUV8", "8", "~^`|8" },
        /* KUI_MULTITAP_KEY_9 */ { "wxyz9", "WXYZ9", "9", "9" },
    };

    if ( ((uint32_t)key >= KUI_MULTITAP_CHAR_KEYS) || ((uint32_t)mode >= KUI_MULTITAP_MODES) )
    {
        return NULL;
    }
    return chars[(size_t)key][(size_t)mode];
}

void kui_multitap_clear(kui_multitap_t *m)
{
    if ( NULL == m )
    {
        return;
    }
    multitap_reset(m, multitap_cap(m->cap));
}
