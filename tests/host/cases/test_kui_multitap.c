/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kui_multitap.c
 * @brief kui_multitap: the multitap keypad's characters, pending character, cap and wipe.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "kui_multitap.h"

/** The time every case starts at, in milliseconds. */
#define T0 1000U
/** One millisecond. */
#define ONE_MS 1U
/** A step between two presses of one cycle, well inside the window. */
#define QUICK_MS 100U
/** A clock 16 ms before the 32-bit counter wraps. */
#define T_NEAR_WRAP 0xFFFFFFF0U
/** A sentinel written into a buffer to tell that nothing touched it. */
#define SENTINEL 0xA5U
/** A position past the end of a short text, where a stray secret is planted. */
#define STRAY_AT 40U
/** A key value no keypad has. */
#define KEY_NONE 200U
/** A mode value no keypad has. */
#define MODE_NONE 77U
/** A cap larger than the buffer. */
#define CAP_HUGE 255U

/** What a key types in each mode, written out. */
typedef struct {
    kui_multitap_key_t key; /**< The key. */
    const char *lower;      /**< Its characters in the lower-case mode. */
    const char *upper;      /**< Its characters in the upper-case mode. */
    const char *digits;     /**< Its characters in the digits mode. */
    const char *symbols;    /**< Its characters in the symbols mode. */
} key_row_t;

/** The ten character keys. */
static const key_row_t k_rows[KUI_MULTITAP_CHAR_KEYS] = {
    { KUI_MULTITAP_KEY_0, " 0", " 0", "0", " 0" },
    { KUI_MULTITAP_KEY_1, ".@!#?-_1", ".@!#?-_1", "1", ".,?!1" },
    { KUI_MULTITAP_KEY_2, "abc2", "ABC2", "2", "@#$%2" },
    { KUI_MULTITAP_KEY_3, "def3", "DEF3", "3", "&*+=3" },
    { KUI_MULTITAP_KEY_4, "ghi4", "GHI4", "4", "-_/\\4" },
    { KUI_MULTITAP_KEY_5, "jkl5", "JKL5", "5", "()[]5" },
    { KUI_MULTITAP_KEY_6, "mno6", "MNO6", "6", "{}<>6" },
    { KUI_MULTITAP_KEY_7, "pqrs7", "PQRS7", "7", ":;'\"7" },
    { KUI_MULTITAP_KEY_8, "tuv8", "TUV8", "8", "~^`|8" },
    { KUI_MULTITAP_KEY_9, "wxyz9", "WXYZ9", "9", "9" },
};

/**
 * @brief The characters of a row in a mode.
 *
 * @param r The row.
 * @param mode The mode.
 * @return The row's characters in that mode.
 */
static const char *row_chars(const key_row_t *r, kui_multitap_mode_t mode)
{
    if ( KUI_MULTITAP_MODE_LOWER == mode )
    {
        return r->lower;
    }
    if ( KUI_MULTITAP_MODE_UPPER == mode )
    {
        return r->upper;
    }
    if ( KUI_MULTITAP_MODE_DIGITS == mode )
    {
        return r->digits;
    }
    return r->symbols;
}

/**
 * @brief Presses the mode key until the multitap is in the wanted mode.
 *
 * @param m The multitap.
 * @param mode The mode to reach.
 * @param t The clock.
 */
static void go_to_mode(kui_multitap_t *m, kui_multitap_mode_t mode, uint32_t t)
{
    for ( uint32_t i = 0U; (KUI_MULTITAP_MODES > i) && (kui_multitap_mode(m) != mode); i++ )
    {
        TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_STEPPED, kui_multitap_press(m, KUI_MULTITAP_KEY_MODE, t));
    }
    TEST_ASSERT_EQUAL_INT(mode, kui_multitap_mode(m));
}

/**
 * @brief Types n letters a, each 800 ms after the last, so each is a new character.
 *
 * @param m The multitap, in the lower-case mode.
 * @param n How many.
 * @return The time of the last press.
 */
static uint32_t fill_a(kui_multitap_t *m, size_t n)
{
    uint32_t now = T0;
    uint32_t last = T0;

    for ( size_t i = 0U; i < n; i++ )
    {
        TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(m, KUI_MULTITAP_KEY_2, now));
        last = now;
        now += KUI_MULTITAP_MS;
    }
    return last;
}

/**
 * @brief Whether the text is n times the letter a and a NUL.
 *
 * @param m The multitap.
 * @param n How many a.
 * @return true when it is.
 */
static bool text_is_a(const kui_multitap_t *m, size_t n)
{
    const char *t = kui_multitap_text(m);

    if ( (kui_multitap_len(m) != n) || (strlen(t) != n) )
    {
        return false;
    }
    for ( size_t i = 0U; i < n; i++ )
    {
        if ( 'a' != t[i] )
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief Whether every byte of the buffer from the text's length on is 0.
 *
 * @param m The multitap.
 * @return true when no character is left past the end.
 */
static bool tail_is_zero(const kui_multitap_t *m)
{
    for ( size_t i = m->len; sizeof(m->text) > i; i++ )
    {
        if ( 0 != m->text[i] )
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief Whether the whole buffer is 0.
 *
 * @param m The multitap.
 * @return true when every byte is 0.
 */
static bool buffer_is_zero(const kui_multitap_t *m)
{
    for ( size_t i = 0U; sizeof(m->text) > i; i++ )
    {
        if ( 0 != m->text[i] )
        {
            return false;
        }
    }
    return true;
}

TEST_CASE("the named values are the plan's", "[kui_multitap]")
{
    TEST_ASSERT_EQUAL_UINT32(800U, KUI_MULTITAP_MS);
    TEST_ASSERT_EQUAL_UINT32(1000U, KUI_MULTITAP_SHOW_MS);
    TEST_ASSERT_EQUAL_UINT32(63U, KUI_MULTITAP_TEXT_MAX);
    TEST_ASSERT_EQUAL_UINT32(32U, KUI_MULTITAP_SSID_MAX);
    TEST_ASSERT_EQUAL_UINT32(12U, KUI_MULTITAP_KEYS);
    TEST_ASSERT_EQUAL_UINT32(10U, KUI_MULTITAP_CHAR_KEYS);
    TEST_ASSERT_EQUAL_UINT32(4U, KUI_MULTITAP_MODES);
}

TEST_CASE("the table gives each key its characters in each mode", "[kui_multitap]")
{
    for ( size_t r = 0U; KUI_MULTITAP_CHAR_KEYS > r; r++ )
    {
        TEST_ASSERT_EQUAL_STRING(k_rows[r].lower, kui_multitap_chars(k_rows[r].key, KUI_MULTITAP_MODE_LOWER));
        TEST_ASSERT_EQUAL_STRING(k_rows[r].upper, kui_multitap_chars(k_rows[r].key, KUI_MULTITAP_MODE_UPPER));
        TEST_ASSERT_EQUAL_STRING(k_rows[r].digits, kui_multitap_chars(k_rows[r].key, KUI_MULTITAP_MODE_DIGITS));
        TEST_ASSERT_EQUAL_STRING(k_rows[r].symbols, kui_multitap_chars(k_rows[r].key, KUI_MULTITAP_MODE_SYMBOLS));
    }
}

/** The size of a table that counts every ASCII value. */
#define ASCII_COUNT 128U
/** How many printable ASCII symbols there are: every printable character but the space, the 10 digits and the 52 letters. */
#define ASCII_SYMBOLS 32U

/**
 * @brief Whether an ASCII value is a printable symbol: printable, and neither space, digit nor letter.
 *
 * @param c The value.
 * @return true for the 32 symbols ! to /, : to @, [ to ` and { to ~ (0x21-0x2F, 0x3A-0x40, 0x5B-0x60, 0x7B-0x7E).
 */
static bool is_ascii_symbol(unsigned int c)
{
    return (((unsigned int)'!' <= c) && ((unsigned int)'/' >= c)) || (((unsigned int)':' <= c) && ((unsigned int)'@' >= c)) ||
           (((unsigned int)'[' <= c) && ((unsigned int)'`' >= c)) || (((unsigned int)'{' <= c) && ((unsigned int)'~' >= c));
}

TEST_CASE("the symbols mode holds each of the 32 printable ASCII symbols exactly once", "[kui_multitap]")
{
    unsigned int seen[ASCII_COUNT];
    unsigned int symbols = 0U;

    (void)memset(seen, 0, sizeof(seen));
    for ( size_t r = 0U; KUI_MULTITAP_CHAR_KEYS > r; r++ )
    {
        const char *chars = kui_multitap_chars((kui_multitap_key_t)r, KUI_MULTITAP_MODE_SYMBOLS);

        TEST_ASSERT_NOT_NULL(chars);
        /* Every byte of the column is counted, whatever it is. */
        for ( size_t i = 0U; '\0' != chars[i]; i++ )
        {
            const unsigned int c = (unsigned char)chars[i];

            TEST_ASSERT_TRUE(ASCII_COUNT > c);
            seen[c]++;
            if ( is_ascii_symbol(c) )
            {
                symbols++;
            }
        }
        /* What is not a symbol is the key's own digit at the end (the key 0 also its space). */
        TEST_ASSERT_EQUAL_CHAR((char)('0' + (char)r), chars[strlen(chars) - 1U]);
    }
    for ( unsigned int c = 0U; ASCII_COUNT > c; c++ )
    {
        /* Each symbol once, each of the ten digits once (a key's own), the space once (the key 0's), and no other byte. */
        if ( is_ascii_symbol(c) || (((unsigned int)'0' <= c) && ((unsigned int)'9' >= c)) || ((unsigned int)' ' == c) )
        {
            TEST_ASSERT_EQUAL_UINT32(1U, seen[c]);
        }
        else
        {
            TEST_ASSERT_EQUAL_UINT32(0U, seen[c]);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(ASCII_SYMBOLS, symbols);
}

TEST_CASE("every printable ASCII character from the space to the tilde is typed in some mode", "[kui_multitap]")
{
    for ( unsigned int c = (unsigned int)' '; (unsigned int)'~' >= c; c++ )
    {
        bool found = false;

        for ( size_t r = 0U; (KUI_MULTITAP_CHAR_KEYS > r) && !found; r++ )
        {
            for ( unsigned int mode = 0U; (KUI_MULTITAP_MODES > mode) && !found; mode++ )
            {
                const char *chars = kui_multitap_chars((kui_multitap_key_t)r, (kui_multitap_mode_t)mode);

                found = (NULL != chars) && (NULL != strchr(chars, (int)c));
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(found, "a printable ASCII character no key types");
    }
}

TEST_CASE("the mode key, Delete and anything outside the keypad have no characters", "[kui_multitap]")
{
    TEST_ASSERT_NULL(kui_multitap_chars(KUI_MULTITAP_KEY_MODE, KUI_MULTITAP_MODE_LOWER));
    TEST_ASSERT_NULL(kui_multitap_chars(KUI_MULTITAP_KEY_DELETE, KUI_MULTITAP_MODE_LOWER));
    TEST_ASSERT_NULL(kui_multitap_chars((kui_multitap_key_t)KEY_NONE, KUI_MULTITAP_MODE_LOWER));
    TEST_ASSERT_NULL(kui_multitap_chars(KUI_MULTITAP_KEY_2, (kui_multitap_mode_t)MODE_NONE));
}

TEST_CASE("a letter key cycles its letters then its digit and wraps, in lower case", "[kui_multitap]")
{
    for ( size_t r = 0U; KUI_MULTITAP_CHAR_KEYS > r; r++ )
    {
        kui_multitap_t m;
        const char *chars = k_rows[r].lower;
        const size_t n = strlen(chars);
        uint32_t t = T0;

        kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
        TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, k_rows[r].key, t));
        TEST_ASSERT_EQUAL_CHAR(chars[0], kui_multitap_text(&m)[0]);
        /* Two whole turns of the key: the wrap is crossed, and the chain of
         * presses runs far past 800 ms because each one is measured from the
         * one before it. */
        for ( size_t i = 1U; (2U * n) >= i; i++ )
        {
            t += QUICK_MS;
            TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_CYCLED, kui_multitap_press(&m, k_rows[r].key, t));
            TEST_ASSERT_EQUAL_CHAR(chars[i % n], kui_multitap_text(&m)[0]);
            TEST_ASSERT_EQUAL_UINT32(1U, kui_multitap_len(&m));
            TEST_ASSERT_TRUE(kui_multitap_pending(&m));
        }
        TEST_ASSERT_EQUAL_CHAR('\0', kui_multitap_text(&m)[1]);
    }
}

TEST_CASE("a letter key cycles its capitals then its digit and wraps, in upper case", "[kui_multitap]")
{
    for ( size_t r = 0U; KUI_MULTITAP_CHAR_KEYS > r; r++ )
    {
        kui_multitap_t m;
        const char *chars = k_rows[r].upper;
        const size_t n = strlen(chars);
        uint32_t t = T0;

        kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
        go_to_mode(&m, KUI_MULTITAP_MODE_UPPER, t);
        TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, k_rows[r].key, t));
        TEST_ASSERT_EQUAL_CHAR(chars[0], kui_multitap_text(&m)[0]);
        for ( size_t i = 1U; (2U * n) >= i; i++ )
        {
            t += QUICK_MS;
            TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_CYCLED, kui_multitap_press(&m, k_rows[r].key, t));
            TEST_ASSERT_EQUAL_CHAR(chars[i % n], kui_multitap_text(&m)[0]);
            TEST_ASSERT_EQUAL_UINT32(1U, kui_multitap_len(&m));
        }
    }
}

TEST_CASE("a symbol key cycles its symbols then its digit and wraps, in the symbols mode", "[kui_multitap]")
{
    for ( size_t r = 0U; KUI_MULTITAP_CHAR_KEYS > r; r++ )
    {
        kui_multitap_t m;
        const char *chars = k_rows[r].symbols;
        const size_t n = strlen(chars);
        uint32_t t = T0;

        if ( 2U > n )
        {
            continue;
        }
        kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
        go_to_mode(&m, KUI_MULTITAP_MODE_SYMBOLS, t);
        TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, k_rows[r].key, t));
        TEST_ASSERT_EQUAL_CHAR(chars[0], kui_multitap_text(&m)[0]);
        for ( size_t i = 1U; (2U * n) >= i; i++ )
        {
            t += QUICK_MS;
            TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_CYCLED, kui_multitap_press(&m, k_rows[r].key, t));
            TEST_ASSERT_EQUAL_CHAR(chars[i % n], kui_multitap_text(&m)[0]);
            TEST_ASSERT_EQUAL_UINT32(1U, kui_multitap_len(&m));
            TEST_ASSERT_TRUE(kui_multitap_pending(&m));
        }
    }
}

TEST_CASE("in the symbols mode the key 9 has only its digit, so it types it each press", "[kui_multitap]")
{
    kui_multitap_t m;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    go_to_mode(&m, KUI_MULTITAP_MODE_SYMBOLS, T0);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_9, T0));
    TEST_ASSERT_FALSE(kui_multitap_pending(&m));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_9, T0 + ONE_MS));
    TEST_ASSERT_EQUAL_STRING("99", kui_multitap_text(&m));
    /* The key 8 next to it still cycles its four symbols. */
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_8, T0 + QUICK_MS));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_CYCLED, kui_multitap_press(&m, KUI_MULTITAP_KEY_8, T0 + (2U * QUICK_MS)));
    TEST_ASSERT_EQUAL_STRING("99^", kui_multitap_text(&m));
}

TEST_CASE("the digits mode types a digit and never cycles, so the same key twice types it twice", "[kui_multitap]")
{
    kui_multitap_t m;
    uint32_t t = T0;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    go_to_mode(&m, KUI_MULTITAP_MODE_DIGITS, t);
    for ( size_t r = 0U; KUI_MULTITAP_CHAR_KEYS > r; r++ )
    {
        TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, k_rows[r].key, t));
        TEST_ASSERT_FALSE(kui_multitap_pending(&m));
        t += ONE_MS;
    }
    TEST_ASSERT_EQUAL_STRING("0123456789", kui_multitap_text(&m));

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    go_to_mode(&m, KUI_MULTITAP_MODE_DIGITS, t);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_7, t));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_7, t + ONE_MS));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_7, t + ONE_MS + ONE_MS));
    TEST_ASSERT_EQUAL_STRING("777", kui_multitap_text(&m));
}

TEST_CASE("a press 799 ms after the last cycles and one 800 ms after it types a new character", "[kui_multitap]")
{
    kui_multitap_t m;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, T0));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_CYCLED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, T0 + KUI_MULTITAP_MS - ONE_MS));
    TEST_ASSERT_EQUAL_STRING("b", kui_multitap_text(&m));

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, T0));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, T0 + KUI_MULTITAP_MS));
    TEST_ASSERT_EQUAL_STRING("aa", kui_multitap_text(&m));
    TEST_ASSERT_TRUE(kui_multitap_pending(&m));
}

TEST_CASE("the window runs from the last press that cycled, not from the first", "[kui_multitap]")
{
    kui_multitap_t m;
    uint32_t t = T0;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_5, t));
    for ( uint32_t i = 0U; 4U > i; i++ )
    {
        t += KUI_MULTITAP_MS - ONE_MS;
        TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_CYCLED, kui_multitap_press(&m, KUI_MULTITAP_KEY_5, t));
    }
    /* Four presses, 799 ms apart: 3196 ms in all, and still one character. */
    TEST_ASSERT_EQUAL_STRING("j", kui_multitap_text(&m));
    t += KUI_MULTITAP_MS;
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_5, t));
    TEST_ASSERT_EQUAL_STRING("jj", kui_multitap_text(&m));
}

TEST_CASE("a press of the same key too late for the window commits without a tick", "[kui_multitap]")
{
    kui_multitap_t m;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_3, T0));
    TEST_ASSERT_TRUE(kui_multitap_pending(&m));
    /* No tick ran: the character is still marked pending, and the press at 5 s settles it. */
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_3, T0 + 5000U));
    TEST_ASSERT_EQUAL_STRING("dd", kui_multitap_text(&m));
}

TEST_CASE("another key commits the pending character and starts its own", "[kui_multitap]")
{
    kui_multitap_t m;
    uint32_t t = T0;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, t));
    t += QUICK_MS;
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_3, t));
    TEST_ASSERT_EQUAL_STRING("ad", kui_multitap_text(&m));
    TEST_ASSERT_TRUE(kui_multitap_pending(&m));
    t += QUICK_MS;
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_CYCLED, kui_multitap_press(&m, KUI_MULTITAP_KEY_3, t));
    TEST_ASSERT_EQUAL_STRING("ae", kui_multitap_text(&m));
    /* The first key again, inside the window of the second: it cannot reach the a it committed. */
    t += QUICK_MS;
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, t));
    TEST_ASSERT_EQUAL_STRING("aea", kui_multitap_text(&m));
    t += QUICK_MS;
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_CYCLED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, t));
    TEST_ASSERT_EQUAL_STRING("aeb", kui_multitap_text(&m));
}

TEST_CASE("a word is typed with a pause between two letters of one key", "[kui_multitap]")
{
    kui_multitap_t m;
    uint32_t t = T0;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    /* h */
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_4, t);
    t += QUICK_MS;
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_4, t);
    /* e */
    t += QUICK_MS;
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_3, t);
    t += QUICK_MS;
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_3, t);
    /* l, a pause, l: the tick is the pause */
    for ( uint32_t i = 0U; 3U > i; i++ )
    {
        t += QUICK_MS;
        (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_5, t);
    }
    t += KUI_MULTITAP_MS;
    kui_multitap_tick(&m, t);
    for ( uint32_t i = 0U; 3U > i; i++ )
    {
        t += QUICK_MS;
        (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_5, t);
    }
    /* o */
    for ( uint32_t i = 0U; 3U > i; i++ )
    {
        t += QUICK_MS;
        (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_6, t);
    }
    TEST_ASSERT_EQUAL_STRING("hello", kui_multitap_text(&m));
}

TEST_CASE("the tick commits at 800 ms and not before", "[kui_multitap]")
{
    kui_multitap_t m;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, T0));
    kui_multitap_tick(&m, T0 + KUI_MULTITAP_MS - ONE_MS);
    TEST_ASSERT_TRUE(kui_multitap_pending(&m));
    kui_multitap_tick(&m, T0 + KUI_MULTITAP_MS);
    TEST_ASSERT_FALSE(kui_multitap_pending(&m));
    TEST_ASSERT_EQUAL_STRING("a", kui_multitap_text(&m));
    /* Committed: the same key types a second letter instead of cycling the first. */
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, T0 + KUI_MULTITAP_MS + ONE_MS));
    TEST_ASSERT_EQUAL_STRING("aa", kui_multitap_text(&m));
}

TEST_CASE("a tick with nothing pending, or on a single-character key, changes nothing", "[kui_multitap]")
{
    kui_multitap_t m;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    kui_multitap_tick(&m, T0);
    TEST_ASSERT_EQUAL_UINT32(0U, kui_multitap_len(&m));
    go_to_mode(&m, KUI_MULTITAP_MODE_DIGITS, T0);
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_4, T0);
    kui_multitap_tick(&m, T0 + KUI_MULTITAP_MS);
    kui_multitap_tick(&m, T0 + (10U * KUI_MULTITAP_MS));
    TEST_ASSERT_EQUAL_STRING("4", kui_multitap_text(&m));
}

TEST_CASE("the mode key commits the pending character and, with nothing typed, steps abc, ABC, 123, symbols and abc", "[kui_multitap]")
{
    kui_multitap_t m;
    uint32_t t = T0;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_MODE_LOWER, kui_multitap_mode(&m));
    /* What is typed in lower case commits like anything else, and does not stop the walk. */
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, t));
    t += QUICK_MS;
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_STEPPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_MODE, t));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_MODE_UPPER, kui_multitap_mode(&m));
    TEST_ASSERT_FALSE(kui_multitap_pending(&m));
    TEST_ASSERT_EQUAL_STRING("a", kui_multitap_text(&m));
    t += QUICK_MS;
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_STEPPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_MODE, t));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_MODE_DIGITS, kui_multitap_mode(&m));
    t += QUICK_MS;
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_STEPPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_MODE, t));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_MODE_SYMBOLS, kui_multitap_mode(&m));
    t += QUICK_MS;
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_STEPPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_MODE, t));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_MODE_LOWER, kui_multitap_mode(&m));
    TEST_ASSERT_EQUAL_STRING("a", kui_multitap_text(&m));
}

/* Somebody who went to another mode and typed in it is done with it. */
TEST_CASE("after typing in another mode the mode key commits and goes back to abc", "[kui_multitap]")
{
    kui_multitap_t m;
    uint32_t t = T0;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, t));
    t += QUICK_MS;
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_STEPPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_MODE, t));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_MODE_UPPER, kui_multitap_mode(&m));
    /* The same key, inside the old window: a new character in the new mode, not a cycle. */
    t += QUICK_MS;
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, t));
    TEST_ASSERT_EQUAL_STRING("aA", kui_multitap_text(&m));
    t += QUICK_MS;
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_CYCLED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, t));
    TEST_ASSERT_EQUAL_STRING("aB", kui_multitap_text(&m));
    t += QUICK_MS;
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_STEPPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_MODE, t));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_MODE_LOWER, kui_multitap_mode(&m));
    TEST_ASSERT_FALSE(kui_multitap_pending(&m));
    TEST_ASSERT_EQUAL_STRING("aB", kui_multitap_text(&m));

    /* Back in abc the walk is whole again: from digits, typed in, straight back. */
    t += QUICK_MS;
    go_to_mode(&m, KUI_MULTITAP_MODE_DIGITS, t);
    t += QUICK_MS;
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, t));
    TEST_ASSERT_EQUAL_STRING("aB2", kui_multitap_text(&m));
    t += QUICK_MS;
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_STEPPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_MODE, t));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_MODE_LOWER, kui_multitap_mode(&m));

    /* From symbols too, the pending symbol committed on the way. */
    t += QUICK_MS;
    go_to_mode(&m, KUI_MULTITAP_MODE_SYMBOLS, t);
    t += QUICK_MS;
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, t));
    t += QUICK_MS;
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_CYCLED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, t));
    TEST_ASSERT_EQUAL_STRING("aB2#", kui_multitap_text(&m));
    t += QUICK_MS;
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_STEPPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_MODE, t));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_MODE_LOWER, kui_multitap_mode(&m));
    TEST_ASSERT_FALSE(kui_multitap_pending(&m));
    TEST_ASSERT_EQUAL_STRING("aB2#", kui_multitap_text(&m));
    t += QUICK_MS;
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, t));
    TEST_ASSERT_EQUAL_STRING("aB2#a", kui_multitap_text(&m));
}

/* What is on the stack before init, and what the last text left before a clear,
 * are not something typed: the first press of the mode key is the first step. */
TEST_CASE("init and clear forget what was typed: the mode key starts its walk", "[kui_multitap]")
{
    kui_multitap_t m;

    (void)memset(&m, 0xFF, sizeof(m));
    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_STEPPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_MODE, T0));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_MODE_UPPER, kui_multitap_mode(&m));

    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, T0));
    kui_multitap_clear(&m);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_MODE_LOWER, kui_multitap_mode(&m));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_STEPPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_MODE, T0));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_MODE_UPPER, kui_multitap_mode(&m));
}

/* A press the cap refused typed nothing, so the mode key goes on with its walk. */
TEST_CASE("a character the cap refused does not count as typed for the mode key", "[kui_multitap]")
{
    kui_multitap_t m;

    kui_multitap_init(&m, 1U);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, T0));
    go_to_mode(&m, KUI_MULTITAP_MODE_UPPER, T0);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_FULL, kui_multitap_press(&m, KUI_MULTITAP_KEY_3, T0));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_STEPPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_MODE, T0));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_MODE_DIGITS, kui_multitap_mode(&m));
}

TEST_CASE("the mode key steps an empty text and leaves it empty", "[kui_multitap]")
{
    kui_multitap_t m;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_STEPPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_MODE, T0));
    TEST_ASSERT_EQUAL_UINT32(0U, kui_multitap_len(&m));
    TEST_ASSERT_EQUAL_STRING("", kui_multitap_text(&m));
    TEST_ASSERT_FALSE(kui_multitap_show_plain(&m, T0));
}

TEST_CASE("Delete removes the pending character", "[kui_multitap]")
{
    kui_multitap_t m;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, T0));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_DELETED, kui_multitap_press(&m, KUI_MULTITAP_KEY_DELETE, T0 + ONE_MS));
    TEST_ASSERT_EQUAL_UINT32(0U, kui_multitap_len(&m));
    TEST_ASSERT_EQUAL_STRING("", kui_multitap_text(&m));
    TEST_ASSERT_FALSE(kui_multitap_pending(&m));
    TEST_ASSERT_TRUE(tail_is_zero(&m));
}

TEST_CASE("Delete removes the last committed character when none is pending", "[kui_multitap]")
{
    kui_multitap_t m;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_2, T0);
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_3, T0 + ONE_MS);
    kui_multitap_tick(&m, T0 + ONE_MS + KUI_MULTITAP_MS);
    TEST_ASSERT_FALSE(kui_multitap_pending(&m));
    TEST_ASSERT_EQUAL_STRING("ad", kui_multitap_text(&m));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_DELETED, kui_multitap_press(&m, KUI_MULTITAP_KEY_DELETE, T0 + 2000U));
    TEST_ASSERT_EQUAL_STRING("a", kui_multitap_text(&m));
    TEST_ASSERT_TRUE(tail_is_zero(&m));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_DELETED, kui_multitap_press(&m, KUI_MULTITAP_KEY_DELETE, T0 + 2001U));
    TEST_ASSERT_EQUAL_STRING("", kui_multitap_text(&m));
    TEST_ASSERT_TRUE(tail_is_zero(&m));
}

TEST_CASE("Delete with nothing typed says so and changes nothing", "[kui_multitap]")
{
    kui_multitap_t m;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_EMPTY, kui_multitap_press(&m, KUI_MULTITAP_KEY_DELETE, T0));
    TEST_ASSERT_EQUAL_UINT32(0U, kui_multitap_len(&m));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_MODE_LOWER, kui_multitap_mode(&m));
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_2, T0);
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_DELETE, T0);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_EMPTY, kui_multitap_press(&m, KUI_MULTITAP_KEY_DELETE, T0));
    TEST_ASSERT_TRUE(tail_is_zero(&m));
}

TEST_CASE("Delete ends the cycle: the same key then types a new first character", "[kui_multitap]")
{
    kui_multitap_t m;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_2, T0);
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_3, T0 + QUICK_MS);
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_3, T0 + (2U * QUICK_MS));
    TEST_ASSERT_EQUAL_STRING("ae", kui_multitap_text(&m));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_DELETED, kui_multitap_press(&m, KUI_MULTITAP_KEY_DELETE, T0 + (3U * QUICK_MS)));
    TEST_ASSERT_EQUAL_STRING("a", kui_multitap_text(&m));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_3, T0 + (4U * QUICK_MS)));
    TEST_ASSERT_EQUAL_STRING("ad", kui_multitap_text(&m));
}

TEST_CASE("63 characters fit and the 64th is refused", "[kui_multitap]")
{
    kui_multitap_t m;
    uint32_t last;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    last = fill_a(&m, KUI_MULTITAP_TEXT_MAX);
    TEST_ASSERT_TRUE(text_is_a(&m, KUI_MULTITAP_TEXT_MAX));
    TEST_ASSERT_EQUAL_CHAR('\0', m.text[KUI_MULTITAP_TEXT_MAX]);

    /* The same key a window later, and another key: both would add a 64th character. */
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_FULL, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, last + KUI_MULTITAP_MS));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_FULL, kui_multitap_press(&m, KUI_MULTITAP_KEY_3, last + KUI_MULTITAP_MS));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_FULL, kui_multitap_press(&m, KUI_MULTITAP_KEY_0, last + KUI_MULTITAP_MS));
    TEST_ASSERT_TRUE(text_is_a(&m, KUI_MULTITAP_TEXT_MAX));
    TEST_ASSERT_EQUAL_CHAR('\0', m.text[KUI_MULTITAP_TEXT_MAX]);

    /* The mode key and Delete are not additions: they work at the cap. */
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_STEPPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_MODE, last + KUI_MULTITAP_MS));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_FULL, kui_multitap_press(&m, KUI_MULTITAP_KEY_4, last + KUI_MULTITAP_MS));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_DELETED, kui_multitap_press(&m, KUI_MULTITAP_KEY_DELETE, last + KUI_MULTITAP_MS));
    TEST_ASSERT_TRUE(text_is_a(&m, KUI_MULTITAP_TEXT_MAX - 1U));
    TEST_ASSERT_TRUE(tail_is_zero(&m));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_4, last + KUI_MULTITAP_MS));
    TEST_ASSERT_EQUAL_UINT32(KUI_MULTITAP_TEXT_MAX, kui_multitap_len(&m));
}

TEST_CASE("a digit at the cap is refused too", "[kui_multitap]")
{
    kui_multitap_t m;
    uint32_t last;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    last = fill_a(&m, KUI_MULTITAP_TEXT_MAX);
    go_to_mode(&m, KUI_MULTITAP_MODE_DIGITS, last);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_FULL, kui_multitap_press(&m, KUI_MULTITAP_KEY_5, last + KUI_MULTITAP_MS));
    TEST_ASSERT_TRUE(text_is_a(&m, KUI_MULTITAP_TEXT_MAX));
}

TEST_CASE("cycling the pending character at the cap is allowed", "[kui_multitap]")
{
    kui_multitap_t m;
    uint32_t last;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    last = fill_a(&m, KUI_MULTITAP_TEXT_MAX);
    TEST_ASSERT_TRUE(kui_multitap_pending(&m));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_CYCLED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, last + QUICK_MS));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_CYCLED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, last + (2U * QUICK_MS)));
    TEST_ASSERT_EQUAL_UINT32(KUI_MULTITAP_TEXT_MAX, kui_multitap_len(&m));
    TEST_ASSERT_EQUAL_CHAR('c', kui_multitap_text(&m)[KUI_MULTITAP_TEXT_MAX - 1U]);
    TEST_ASSERT_EQUAL_CHAR('\0', m.text[KUI_MULTITAP_TEXT_MAX]);
}

TEST_CASE("at the cap another key is refused but still commits the pending character", "[kui_multitap]")
{
    kui_multitap_t m;
    uint32_t last;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    last = fill_a(&m, KUI_MULTITAP_TEXT_MAX);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_FULL, kui_multitap_press(&m, KUI_MULTITAP_KEY_3, last + QUICK_MS));
    TEST_ASSERT_FALSE(kui_multitap_pending(&m));
    /* The a is committed: the key that typed it no longer cycles it. */
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_FULL, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, last + (2U * QUICK_MS)));
    TEST_ASSERT_TRUE(text_is_a(&m, KUI_MULTITAP_TEXT_MAX));
}

TEST_CASE("a hidden network's name stops at 32 characters", "[kui_multitap]")
{
    kui_multitap_t m;
    uint32_t last;

    kui_multitap_init(&m, KUI_MULTITAP_SSID_MAX);
    last = fill_a(&m, KUI_MULTITAP_SSID_MAX);
    TEST_ASSERT_TRUE(text_is_a(&m, KUI_MULTITAP_SSID_MAX));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_FULL, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, last + KUI_MULTITAP_MS));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_FULL, kui_multitap_press(&m, KUI_MULTITAP_KEY_7, last + KUI_MULTITAP_MS));
    TEST_ASSERT_TRUE(text_is_a(&m, KUI_MULTITAP_SSID_MAX));
    TEST_ASSERT_TRUE(tail_is_zero(&m));
}

TEST_CASE("a cap of 1 holds one character and still cycles it", "[kui_multitap]")
{
    kui_multitap_t m;

    kui_multitap_init(&m, 1U);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_9, T0));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_CYCLED, kui_multitap_press(&m, KUI_MULTITAP_KEY_9, T0 + QUICK_MS));
    TEST_ASSERT_EQUAL_STRING("x", kui_multitap_text(&m));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_FULL, kui_multitap_press(&m, KUI_MULTITAP_KEY_8, T0 + (2U * QUICK_MS)));
    TEST_ASSERT_EQUAL_STRING("x", kui_multitap_text(&m));
}

TEST_CASE("a cap of 0 or past the buffer is the passphrase's cap", "[kui_multitap]")
{
    static const uint8_t caps[] = { 0U, (uint8_t)(KUI_MULTITAP_TEXT_MAX + 1U), CAP_HUGE };

    for ( size_t c = 0U; (sizeof(caps) / sizeof(caps[0])) > c; c++ )
    {
        kui_multitap_t m;
        uint32_t last;

        kui_multitap_init(&m, caps[c]);
        last = fill_a(&m, KUI_MULTITAP_TEXT_MAX);
        TEST_ASSERT_TRUE(text_is_a(&m, KUI_MULTITAP_TEXT_MAX));
        TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_FULL, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, last + KUI_MULTITAP_MS));
        TEST_ASSERT_TRUE(text_is_a(&m, KUI_MULTITAP_TEXT_MAX));
    }
}

TEST_CASE("the last character shows plain while pending and for 1000 ms after its press", "[kui_multitap]")
{
    kui_multitap_t m;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    TEST_ASSERT_FALSE(kui_multitap_show_plain(&m, T0));
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_2, T0);
    TEST_ASSERT_TRUE(kui_multitap_show_plain(&m, T0));
    TEST_ASSERT_TRUE(kui_multitap_show_plain(&m, T0 + KUI_MULTITAP_MS - ONE_MS));
    /* Pending with no tick yet: plain. */
    TEST_ASSERT_TRUE(kui_multitap_show_plain(&m, T0 + KUI_MULTITAP_MS));
    /* Committed at 800 ms, still plain until 1000 ms. */
    kui_multitap_tick(&m, T0 + KUI_MULTITAP_MS);
    TEST_ASSERT_FALSE(kui_multitap_pending(&m));
    TEST_ASSERT_TRUE(kui_multitap_show_plain(&m, T0 + KUI_MULTITAP_MS));
    TEST_ASSERT_TRUE(kui_multitap_show_plain(&m, T0 + KUI_MULTITAP_SHOW_MS - ONE_MS));
    TEST_ASSERT_FALSE(kui_multitap_show_plain(&m, T0 + KUI_MULTITAP_SHOW_MS));
    TEST_ASSERT_FALSE(kui_multitap_show_plain(&m, T0 + (10U * KUI_MULTITAP_SHOW_MS)));
}

TEST_CASE("a digit shows plain for 1000 ms though it is committed at once", "[kui_multitap]")
{
    kui_multitap_t m;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    go_to_mode(&m, KUI_MULTITAP_MODE_DIGITS, T0);
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_2, T0);
    TEST_ASSERT_FALSE(kui_multitap_pending(&m));
    TEST_ASSERT_TRUE(kui_multitap_show_plain(&m, T0));
    TEST_ASSERT_TRUE(kui_multitap_show_plain(&m, T0 + KUI_MULTITAP_SHOW_MS - ONE_MS));
    TEST_ASSERT_FALSE(kui_multitap_show_plain(&m, T0 + KUI_MULTITAP_SHOW_MS));
}

TEST_CASE("every press that cycles or types restarts the plain window", "[kui_multitap]")
{
    kui_multitap_t m;
    const uint32_t second = T0 + 700U;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_2, T0);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_CYCLED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, second));
    TEST_ASSERT_TRUE(kui_multitap_show_plain(&m, second + KUI_MULTITAP_SHOW_MS - ONE_MS));
    kui_multitap_tick(&m, second + KUI_MULTITAP_MS);
    TEST_ASSERT_FALSE(kui_multitap_show_plain(&m, second + KUI_MULTITAP_SHOW_MS));
    /* A new character: the old one is a dot, the new one plain. */
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_3, second + KUI_MULTITAP_SHOW_MS);
    TEST_ASSERT_TRUE(kui_multitap_show_plain(&m, second + KUI_MULTITAP_SHOW_MS));
}

TEST_CASE("after Delete no character shows plain, and the mode key does not hide the last one", "[kui_multitap]")
{
    kui_multitap_t m;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_2, T0);
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_3, T0 + ONE_MS);
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_MODE, T0 + QUICK_MS);
    TEST_ASSERT_TRUE(kui_multitap_show_plain(&m, T0 + QUICK_MS));
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_DELETE, T0 + QUICK_MS);
    /* The a that is left was typed a moment ago, but it was not typed by the last press: a dot. */
    TEST_ASSERT_EQUAL_STRING("a", kui_multitap_text(&m));
    TEST_ASSERT_FALSE(kui_multitap_show_plain(&m, T0 + QUICK_MS));
}

TEST_CASE("a refused press does not move the plain window", "[kui_multitap]")
{
    kui_multitap_t m;
    uint32_t last;

    kui_multitap_init(&m, KUI_MULTITAP_SSID_MAX);
    last = fill_a(&m, KUI_MULTITAP_SSID_MAX);
    kui_multitap_tick(&m, last + KUI_MULTITAP_MS);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_FULL, kui_multitap_press(&m, KUI_MULTITAP_KEY_3, last + 900U));
    TEST_ASSERT_TRUE(kui_multitap_show_plain(&m, last + KUI_MULTITAP_SHOW_MS - ONE_MS));
    TEST_ASSERT_FALSE(kui_multitap_show_plain(&m, last + KUI_MULTITAP_SHOW_MS));
}

TEST_CASE("the window holds across the 32-bit clock's wrap", "[kui_multitap]")
{
    kui_multitap_t m;

    /* 799 ms after a press 16 ms before the wrap: cycles. */
    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_2, T_NEAR_WRAP);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_CYCLED,
                          kui_multitap_press(&m, KUI_MULTITAP_KEY_2, (uint32_t)(T_NEAR_WRAP + KUI_MULTITAP_MS - ONE_MS)));
    TEST_ASSERT_EQUAL_STRING("b", kui_multitap_text(&m));

    /* 800 ms after it: a new character. */
    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_2, T_NEAR_WRAP);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED,
                          kui_multitap_press(&m, KUI_MULTITAP_KEY_2, (uint32_t)(T_NEAR_WRAP + KUI_MULTITAP_MS)));
    TEST_ASSERT_EQUAL_STRING("aa", kui_multitap_text(&m));

    /* One millisecond across the wrap, from the last value to 0. */
    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_2, UINT32_MAX);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_CYCLED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, 0U));
    TEST_ASSERT_EQUAL_STRING("b", kui_multitap_text(&m));
}

TEST_CASE("the tick and the plain window hold across the 32-bit clock's wrap", "[kui_multitap]")
{
    kui_multitap_t m;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_2, T_NEAR_WRAP);
    kui_multitap_tick(&m, (uint32_t)(T_NEAR_WRAP + KUI_MULTITAP_MS - ONE_MS));
    TEST_ASSERT_TRUE(kui_multitap_pending(&m));
    kui_multitap_tick(&m, (uint32_t)(T_NEAR_WRAP + KUI_MULTITAP_MS));
    TEST_ASSERT_FALSE(kui_multitap_pending(&m));
    TEST_ASSERT_TRUE(kui_multitap_show_plain(&m, (uint32_t)(T_NEAR_WRAP + KUI_MULTITAP_SHOW_MS - ONE_MS)));
    TEST_ASSERT_FALSE(kui_multitap_show_plain(&m, (uint32_t)(T_NEAR_WRAP + KUI_MULTITAP_SHOW_MS)));
}

TEST_CASE("init wipes a buffer that held anything", "[kui_multitap]")
{
    kui_multitap_t m;

    (void)memset(&m, SENTINEL, sizeof(m));
    kui_multitap_init(&m, KUI_MULTITAP_SSID_MAX);
    TEST_ASSERT_TRUE(buffer_is_zero(&m));
    TEST_ASSERT_EQUAL_UINT32(0U, kui_multitap_len(&m));
    TEST_ASSERT_FALSE(kui_multitap_pending(&m));
    TEST_ASSERT_FALSE(kui_multitap_show_plain(&m, T0));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_MODE_LOWER, kui_multitap_mode(&m));
    TEST_ASSERT_EQUAL_STRING("", kui_multitap_text(&m));
    TEST_ASSERT_EQUAL_UINT8(KUI_MULTITAP_SSID_MAX, m.cap);
}

TEST_CASE("clear zeroes every byte of the text buffer and the state", "[kui_multitap]")
{
    kui_multitap_t m;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    (void)fill_a(&m, KUI_MULTITAP_TEXT_MAX);
    go_to_mode(&m, KUI_MULTITAP_MODE_DIGITS, T0);
    TEST_ASSERT_FALSE(buffer_is_zero(&m));
    /* A stray byte past the length, as a leftover would be: clear is not a cut at the length. */
    m.text[STRAY_AT] = 'Z';
    kui_multitap_clear(&m);

    TEST_ASSERT_TRUE(buffer_is_zero(&m));
    TEST_ASSERT_EQUAL_UINT32(0U, kui_multitap_len(&m));
    TEST_ASSERT_EQUAL_STRING("", kui_multitap_text(&m));
    TEST_ASSERT_FALSE(kui_multitap_pending(&m));
    TEST_ASSERT_FALSE(kui_multitap_show_plain(&m, T0));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_MODE_LOWER, kui_multitap_mode(&m));
}

TEST_CASE("a cleared text is empty and keeps its cap", "[kui_multitap]")
{
    kui_multitap_t m;
    uint32_t last;

    kui_multitap_init(&m, KUI_MULTITAP_SSID_MAX);
    (void)fill_a(&m, 5U);
    kui_multitap_clear(&m);
    last = fill_a(&m, KUI_MULTITAP_SSID_MAX);
    TEST_ASSERT_TRUE(text_is_a(&m, KUI_MULTITAP_SSID_MAX));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_FULL, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, last + KUI_MULTITAP_MS));
}

TEST_CASE("nothing of a deleted character stays in the buffer", "[kui_multitap]")
{
    kui_multitap_t m;
    uint32_t t = T0;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    for ( uint32_t i = 0U; 10U > i; i++ )
    {
        (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_7, t);
        t += KUI_MULTITAP_MS;
    }
    for ( uint32_t i = 0U; 10U > i; i++ )
    {
        TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_DELETED, kui_multitap_press(&m, KUI_MULTITAP_KEY_DELETE, t));
        TEST_ASSERT_TRUE(tail_is_zero(&m));
    }
    TEST_ASSERT_TRUE(buffer_is_zero(&m));
}

TEST_CASE("Delete leaves no trace of the character in the state either", "[kui_multitap]")
{
    kui_multitap_t m;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_7, T0);
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_7, T0 + QUICK_MS);
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_7, T0 + (2U * QUICK_MS));
    TEST_ASSERT_EQUAL_STRING("r", kui_multitap_text(&m));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_DELETED, kui_multitap_press(&m, KUI_MULTITAP_KEY_DELETE, T0 + (3U * QUICK_MS)));
    TEST_ASSERT_EQUAL_UINT8(0U, m.key);
    TEST_ASSERT_EQUAL_UINT8(0U, m.idx);
    TEST_ASSERT_FALSE(m.pending);
    TEST_ASSERT_FALSE(m.fresh);
}

TEST_CASE("the text is NUL-terminated at every length up to the cap", "[kui_multitap]")
{
    kui_multitap_t m;
    uint32_t t = T0;

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    for ( size_t n = 1U; KUI_MULTITAP_TEXT_MAX >= n; n++ )
    {
        TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&m, KUI_MULTITAP_KEY_2, t));
        t += KUI_MULTITAP_MS;
        TEST_ASSERT_EQUAL_UINT32(n, strlen(kui_multitap_text(&m)));
        TEST_ASSERT_EQUAL_UINT32(n, kui_multitap_len(&m));
        TEST_ASSERT_TRUE(tail_is_zero(&m));
    }
}

TEST_CASE("a NULL multitap and a key outside the keypad are survived and change nothing", "[kui_multitap]")
{
    kui_multitap_t m;

    kui_multitap_init(NULL, KUI_MULTITAP_TEXT_MAX);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_IGNORED, kui_multitap_press(NULL, KUI_MULTITAP_KEY_2, T0));
    kui_multitap_tick(NULL, T0);
    kui_multitap_clear(NULL);
    TEST_ASSERT_EQUAL_STRING("", kui_multitap_text(NULL));
    TEST_ASSERT_EQUAL_UINT32(0U, kui_multitap_len(NULL));
    TEST_ASSERT_FALSE(kui_multitap_pending(NULL));
    TEST_ASSERT_FALSE(kui_multitap_show_plain(NULL, T0));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_MODE_LOWER, kui_multitap_mode(NULL));

    kui_multitap_init(&m, KUI_MULTITAP_TEXT_MAX);
    (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_2, T0);
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_IGNORED, kui_multitap_press(&m, (kui_multitap_key_t)KUI_MULTITAP_KEYS, T0 + ONE_MS));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_IGNORED, kui_multitap_press(&m, (kui_multitap_key_t)KEY_NONE, T0 + ONE_MS));
    TEST_ASSERT_EQUAL_STRING("a", kui_multitap_text(&m));
    TEST_ASSERT_TRUE(kui_multitap_pending(&m));
}

TEST_CASE("a multitap that was never started ignores every press and writes nothing", "[kui_multitap]")
{
    kui_multitap_t zeroed;
    kui_multitap_t garbage;

    (void)memset(&zeroed, 0, sizeof(zeroed));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_IGNORED, kui_multitap_press(&zeroed, KUI_MULTITAP_KEY_2, T0));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_IGNORED, kui_multitap_press(&zeroed, KUI_MULTITAP_KEY_DELETE, T0));
    TEST_ASSERT_EQUAL_UINT32(0U, kui_multitap_len(&zeroed));

    /* A cap and a length no buffer has room for: the press must not reach past the text. */
    (void)memset(&garbage, SENTINEL, sizeof(garbage));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_IGNORED, kui_multitap_press(&garbage, KUI_MULTITAP_KEY_2, T0));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_IGNORED, kui_multitap_press(&garbage, KUI_MULTITAP_KEY_MODE, T0));

    /* Clear turns either into a usable, empty text. */
    kui_multitap_clear(&garbage);
    TEST_ASSERT_TRUE(buffer_is_zero(&garbage));
    TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_TYPED, kui_multitap_press(&garbage, KUI_MULTITAP_KEY_2, T0));
    TEST_ASSERT_EQUAL_STRING("a", kui_multitap_text(&garbage));
}

/** The multiplier of the walk's linear congruential generator (Numerical Recipes). */
#define LCG_MUL 1664525U
/** The increment of the walk's generator. */
#define LCG_ADD 1013904223U
/** The seed of the walk, fixed so a failure repeats. */
#define LCG_SEED 20261025U
/** How many steps each walk takes. */
#define WALK_STEPS 4000U
/** The cap of the second walk, small enough to sit at it most of the time. */
#define WALK_SMALL_CAP 5U
/** A clock 4096 ms before the 32-bit counter wraps, so a walk crosses the wrap. */
#define WALK_START 0xFFFFF000U
/** The odds, out of 100, that a step presses a character key. */
#define WALK_P_CHAR 70U
/** The odds, out of 100, that a step presses a character key, the mode key or Delete: the 16 after the first 70 are split between the last two. */
#define WALK_P_KEYS 86U
/** The odds, out of 100, that a step presses a character key, the mode key, Delete or ticks; the rest press a key outside the keypad. */
#define WALK_P_TICK 94U
/** How many values past the last key of the keypad a key outside it may take. */
#define WALK_BAD_SPAN 200U
/** The shift that takes the better bits of the generator. */
#define LCG_SHIFT 16U
/** The size of the odds' scale. */
#define WALK_SCALE 100U

/** The time jumps a step may take, around the 800 ms and 1000 ms edges. */
static const uint32_t k_walk_dt[] = { 0U, 1U, 100U, 799U, 800U, 801U, 999U, 1000U, 1001U, 5000U };

/**
 * @brief The next number of the walk's generator.
 *
 * @param s The generator's state.
 * @return A pseudo-random number from the better bits of the state.
 */
static uint32_t walk_next(uint32_t *s)
{
    *s = (*s * LCG_MUL) + LCG_ADD;
    return *s >> LCG_SHIFT;
}

/**
 * @brief Asserts the invariants of a multitap after a step.
 *
 * @param m The multitap.
 * @param cap The cap it was started with.
 */
static void walk_check(const kui_multitap_t *m, size_t cap)
{
    TEST_ASSERT_EQUAL_UINT32(kui_multitap_len(m), strlen(kui_multitap_text(m)));
    TEST_ASSERT_TRUE(tail_is_zero(m));
    TEST_ASSERT_TRUE(kui_multitap_len(m) <= cap);
    if ( kui_multitap_pending(m) )
    {
        TEST_ASSERT_TRUE(kui_multitap_len(m) > 0U);
    }
}

/**
 * @brief Walks a multitap through a pseudo-random run of presses, ticks and time jumps, checking it after each step.
 *
 * @param cap The cap to start the multitap with.
 * @return How many presses were refused at the cap.
 */
static unsigned int walk(uint8_t cap)
{
    kui_multitap_t m;
    uint32_t rng = LCG_SEED;
    uint32_t t = WALK_START;
    unsigned int refused = 0U;

    kui_multitap_init(&m, cap);
    for ( uint32_t step = 0U; WALK_STEPS > step; step++ )
    {
        const uint32_t odds = walk_next(&rng) % WALK_SCALE;
        const uint32_t pick = walk_next(&rng);

        t += k_walk_dt[walk_next(&rng) % (sizeof(k_walk_dt) / sizeof(k_walk_dt[0]))];
        if ( WALK_P_CHAR > odds )
        {
            if ( KUI_MULTITAP_FULL == kui_multitap_press(&m, (kui_multitap_key_t)(pick % KUI_MULTITAP_CHAR_KEYS), t) )
            {
                refused++;
            }
        }
        else if ( WALK_P_KEYS > odds )
        {
            if ( 0U == (pick % 2U) )
            {
                (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_MODE, t);
            }
            else
            {
                (void)kui_multitap_press(&m, KUI_MULTITAP_KEY_DELETE, t);
            }
        }
        else if ( WALK_P_TICK > odds )
        {
            kui_multitap_tick(&m, t);
        }
        else
        {
            TEST_ASSERT_EQUAL_INT(KUI_MULTITAP_IGNORED, kui_multitap_press(&m, (kui_multitap_key_t)(KUI_MULTITAP_KEYS + (pick % WALK_BAD_SPAN)), t));
        }
        walk_check(&m, cap);
    }
    return refused;
}

TEST_CASE("a pseudo-random walk of presses, ticks and time jumps keeps the text consistent", "[kui_multitap]")
{
    /* Both walks must reach the cap, or the cap's edge was not walked. */
    TEST_ASSERT_TRUE(walk(KUI_MULTITAP_TEXT_MAX) > 0U);
    TEST_ASSERT_TRUE(walk(WALK_SMALL_CAP) > 0U);
}
