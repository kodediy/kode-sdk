/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_tag.c
 * @brief The tag identity as text, and the per-task block the tag calls keep.
 */
#include "kode_tag.h"

#include <string.h>

#include "khal_system.h"
#include "kode_entry.h"
#include "kode_sys.h"
#include "kode_task.h"

/** @brief Characters one byte takes in the text: two digits and a separator. */
#define KODE_TAG_CHARS_PER_BYTE 3U

/** @brief Hex digits in one byte. */
#define KODE_TAG_DIGITS_PER_BYTE 2U

/** @brief Bits of a hex digit. */
#define KODE_TAG_NIBBLE_BITS 4U

/** @brief The mask of a hex digit. */
#define KODE_TAG_NIBBLE_MASK 0x0FU

/** @brief What the digit reader answers for a character that is none. */
#define KODE_TAG_NOT_DIGIT 0xFFU

/** @brief Hex digits there are. */
#define KODE_TAG_RADIX 16U

/** @brief The digits of the text, upper case. */
static const char s_digits[] = "0123456789ABCDEF";

bool kode_tag_hex_format(const uint8_t *bytes, uint32_t n, char *out, uint32_t cap)
{
    bool ok = (NULL != out) && (0U < cap) && ((NULL != bytes) || (0U == n));

    if ( ok && (0U < n) )
    {
        /* Three characters a byte: the last has no separator, and its place is the NUL's. */
        ok = (cap / KODE_TAG_CHARS_PER_BYTE) >= n;
    }
    if ( ok )
    {
        uint32_t at = 0U;

        for ( uint32_t i = 0U; i < n; i++ )
        {
            if ( 0U < i )
            {
                out[at] = ':';
                at++;
            }
            out[at] = s_digits[bytes[i] >> KODE_TAG_NIBBLE_BITS];
            at++;
            out[at] = s_digits[bytes[i] & KODE_TAG_NIBBLE_MASK];
            at++;
        }
        out[at] = '\0';
    }
    return ok;
}

/**
 * @brief The value of one hex digit, in either case.
 * @param c The character.
 * @return 0 to 15; KODE_TAG_NOT_DIGIT for any other character.
 */
static uint8_t digit_value(char c)
{
    uint8_t value = KODE_TAG_NOT_DIGIT;

    for ( uint8_t i = 0U; (KODE_TAG_RADIX > i) && (KODE_TAG_NOT_DIGIT == value); i++ )
    {
        if ( kode_ascii_lower(s_digits[i]) == kode_ascii_lower(c) )
        {
            value = i;
        }
    }
    return value;
}

/**
 * @brief Whether @p c may sit between two digits.
 * @param c The character.
 * @return true for ':', '-' and ' '.
 */
static bool is_separator(char c)
{
    return (':' == c) || ('-' == c) || (' ' == c);
}

bool kode_tag_hex_parse(const char *text, uint8_t *out, uint32_t cap, uint32_t *n)
{
    bool     ok = (NULL != text) && (NULL != out) && (NULL != n);
    uint32_t digits = 0U;

    for ( uint32_t i = 0U; ok && ('\0' != text[i]); i++ )
    {
        const uint8_t v = digit_value(text[i]);

        if ( KODE_TAG_NOT_DIGIT != v )
        {
            const uint32_t at = digits / KODE_TAG_DIGITS_PER_BYTE;

            if ( at >= cap )
            {
                ok = false;
            }
            else if ( (digits % KODE_TAG_DIGITS_PER_BYTE) == 0U )
            {
                out[at] = (uint8_t)(v << KODE_TAG_NIBBLE_BITS);
            }
            else
            {
                out[at] = (uint8_t)(out[at] | v);
            }
            digits++;
        }
        else if ( !is_separator(text[i]) || ((digits % KODE_TAG_DIGITS_PER_BYTE) != 0U) )
        {
            /* Any other character, or a separator between the two digits of a byte. */
            ok = false;
        }
        else
        {
            /* A separator between bytes is skipped. */
        }
    }
    if ( ok && ((digits % KODE_TAG_DIGITS_PER_BYTE) != 0U) )
    {
        ok = false;
    }
    if ( ok )
    {
        *n = digits / KODE_TAG_DIGITS_PER_BYTE;
    }
    return ok;
}

struct kode_tag_state *kode_tag_state_ensure(void)
{
    kode_task_t *const task = kode_task_ensure();
    struct kode_tag_state *state = NULL;

    if ( NULL != task )
    {
        if ( NULL == task->tag )
        {
            struct kode_tag_state *s = NULL;

            /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block is a kode_tag_state */
            s = (struct kode_tag_state *)khal_alloc_psram(sizeof(void *), sizeof(*s));

            if ( NULL != s )
            {
                (void)memset(s, 0, sizeof(*s));
                task->tag = s;
            }
        }
        state = task->tag;
    }
    return state;
}

void kode_tag_state_free(struct kode_tag_state *s)
{
    if ( NULL != s )
    {
        khal_free(s);
    }
}

uint32_t kode_tag_window(uint32_t start, uint32_t ms, uint32_t cap)
{
    uint32_t window = cap;

    if ( KODE_FOREVER != ms )
    {
        const uint32_t spent = kode_sys_time_ms() - start;
        uint32_t       left  = 0U;

        if ( spent < ms )
        {
            left = ms - spent;
        }
        if ( left < window )
        {
            window = left;
        }
    }
    return window;
}

/** @brief What the tries of a release carry. */
typedef struct {
    const kode_tag_emu_t *e;   /**< What to ask.         */
    kode_status_t         rc;  /**< The last stop's answer.  */
} release_try_t;

/** @brief One try of a stop: done unless another holder has the claim while the emulation is still up. */
static bool release_stop(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the release_try_t kode_tag_emu_release() passed */
    release_try_t *const t = (release_try_t *)ctx;

    t->rc = t->e->stop();
    return (KODE_DENIED != t->rc) || !t->e->playing();
}

/** @brief Whether the emulation has ended. */
/* cppcheck-suppress constParameterCallback ; kode_retry() fixes the signature, and its argument is void * */
static bool release_gone(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the release_try_t kode_tag_emu_release() passed */
    const release_try_t *const t = (const release_try_t *)ctx;

    return !t->e->playing();
}

bool kode_tag_emu_release(const kode_tag_emu_t *e)
{
    bool ok = true;

    if ( e->playing() )
    {
        release_try_t  t     = { e, KODE_ERROR };
        const uint32_t start = kode_sys_time_ms();

        /* DENIED with the emulation still up: a holder outside the layer has the claim, no stop was posted. */
        (void)kode_retry(&release_stop, &t, e->bound_ms);
        if ( (KODE_TIMEOUT == t.rc) || ((KODE_DENIED == t.rc) && e->playing()) )
        {
            e->fail(t.rc);
            ok = false;
        }
        else
        {
            /* The wait has what the stop left of the one bound. */
            ok = kode_retry(&release_gone, &t, kode_tag_window(start, e->bound_ms, e->bound_ms));
            if ( !ok )
            {
                kode_err_set("%s", e->releasing);
            }
        }
    }
    return ok;
}
