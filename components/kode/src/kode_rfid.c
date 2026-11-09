/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_rfid.c
 * @brief The standard layer's RFID: EM4100 tags read, written and emulated.
 */
#include "kode.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "khal_once.h"
#include "khal_sync.h"
#include "kode_entry.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_tag.h"

/** @brief Bytes of an EM4100 ID. */
#define KODE_RFID_ID_BYTES 5U

/** @brief The name of every tag the front end reads. */
#define KODE_RFID_NAME "EM4100"

/** @brief How long one window of a read listens, ms. */
#define KODE_RFID_POLL_MS 500U

/** @brief The longest the field stays up for a write, ms: what KODE_FOREVER means there. */
#define KODE_RFID_WRITE_MAX_MS 60000U

/** @brief How long a start that timed out is watched for a late success, ms. */
#define KODE_RFID_LOOK_MS 500U

/** @brief The text of a call refused while the Dot plays a tag. */
#define KODE_RFID_EMULATING_TEXT "RFID is emulating a tag; kode_rfid_emulate_stop() ends it"

/** @brief The text of a stop that did not see the front end let go of the tag. */
#define KODE_RFID_RELEASING_TEXT "the RFID front end is still letting the tag go; call kode_rfid_emulate_stop() again"

/** @brief The text of a write the tag did not take. */
#define KODE_RFID_NOT_TAKEN_TEXT "the tag did not take the ID (is it a T5577?)"

_Static_assert(KODE_TAG_ID_MAX >= (3U * KODE_RFID_ID_BYTES), "a tag's ID text does not hold an EM4100 ID");

/** @brief The lock every public RFID call holds; made on the first call. */
static khal_mutex_t s_rfid_turn_lock;

/** @brief Make the RFID lock. @return true when it exists. */
static bool rfid_lock_make(void)
{
    s_rfid_turn_lock = khal_mutex_create();
    return NULL != s_rfid_turn_lock;
}

/**
 * @brief Take the RFID lock, after kode_enter().
 * @param name The public name of the call.
 * @return true when the call has its turn; false with the error set.
 */
static bool rfid_lock_take(const char *name)
{
    static khal_once_t lock_once;  /* the one-shot the lock is made by */
    bool               ok = khal_once_try(&lock_once, &rfid_lock_make);

    if ( ok )
    {
        khal_mutex_lock(s_rfid_turn_lock);
    }
    else
    {
        kode_err_set("%s: no memory for the RFID lock", name);
    }
    return ok;
}

/** @brief Give the turn back, at the end of a call that took the lock. */
static void rfid_leave(void)
{
    khal_mutex_unlock(s_rfid_turn_lock);
}

/**
 * @brief Refuse a call that needs the front end while the Dot plays a tag.
 * @return true when no tag is played; false with the error set.
 */
static bool rfid_tag_free(void)
{
    const bool free_of_tag = !kode_sys_rfid_is_emulating();

    if ( !free_of_tag )
    {
        kode_err_set("%s", KODE_RFID_EMULATING_TEXT);
    }
    return free_of_tag;
}

/**
 * @brief Say why a call to the front end failed.
 * @param rc The sys answer.
 */
static void rfid_fail(kode_status_t rc)
{
    switch ( rc )
    {
    case KODE_DENIED:
        kode_err_set("RFID is busy");
        break;
    case KODE_NO_RESOURCES:
        kode_err_set("no memory for the RFID front end");
        break;
    case KODE_TIMEOUT:
        kode_err_set("the RFID front end did not answer");
        break;
    default:
        kode_err_set("the RFID front end could not be used");
        break;
    }
}

/**
 * @brief Read an ID typed as text.
 * @param text As typed.
 * @param id   Out: the bytes; written only on success.
 * @return true when it is five bytes of hex; false with the error set.
 */
static bool rfid_id_parse(const char *text, uint8_t id[KODE_RFID_ID_BYTES])
{
    uint8_t  bytes[KODE_RFID_ID_BYTES];
    uint32_t n  = 0U;
    bool     ok = false;

    if ( NULL == text )
    {
        kode_err_set("no RFID ID given");
    }
    else if ( kode_tag_hex_parse(text, bytes, KODE_RFID_ID_BYTES, &n) && (KODE_RFID_ID_BYTES == n) )
    {
        (void)memcpy(id, bytes, KODE_RFID_ID_BYTES);
        ok = true;
    }
    else
    {
        kode_err_set("an EM4100 ID is 5 bytes ('%s')", text);
    }
    return ok;
}

/* ---- One call to the front end, window after window --------------------------- */

/** @brief What a call to the front end asks of it. */
typedef enum {
    RFID_OP_READ,   /**< Find a tag.             */
    RFID_OP_WRITE   /**< Write an ID to a tag.   */
} rfid_op_t;

/** @brief What one call carries. */
typedef struct {
    rfid_op_t     op;                      /**< What is asked.                                   */
    uint8_t       id[KODE_RFID_ID_BYTES];  /**< A read: the ID found. A write: the ID to write.  */
    uint8_t       rate;                    /**< A read: the bit rate it came at.                 */
    uint32_t      start;                   /**< When the call began, ms.                         */
    uint32_t      ms;                      /**< The time asked for, or KODE_FOREVER.             */
    uint32_t      limit;                   /**< The longest one window listens, ms.              */
    uint32_t      window;                  /**< How long this try listened, ms.                  */
    bool          asked;                   /**< The front end has answered a window.             */
    kode_status_t rc;                      /**< The last answer.                                 */
} rfid_try_t;

/** @brief One try of a call: true unless another holder has the front end. */
static bool rfid_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the rfid_try_t rfid_run() passed */
    rfid_try_t *const t = (rfid_try_t *)ctx;

    t->window = kode_tag_window(t->start, t->ms, t->limit);
    /* No time left is a timeout, or busy when a holder was the last answer: the front end is never asked for 0 ms. */
    if ( 0U == t->window )
    {
        if ( KODE_DENIED != t->rc )
        {
            t->rc = KODE_TIMEOUT;
        }
    }
    else
    {
        if ( RFID_OP_READ == t->op )
        {
            t->rc = kode_block_rfid_read(t->id, &t->rate, t->window);
        }
        else
        {
            t->rc = kode_block_rfid_write(t->id, t->window);
        }
        t->asked = (KODE_DENIED != t->rc);
    }
    return KODE_DENIED != t->rc;
}

/**
 * @brief Say why a call ended without its answer.
 * @param t The last try.
 */
static void rfid_explain(const rfid_try_t *t)
{
    if ( (KODE_TIMEOUT == t->rc) && (RFID_OP_WRITE == t->op) && t->asked )
    {
        kode_err_set("%s", KODE_RFID_NOT_TAKEN_TEXT);
    }
    else if ( KODE_TIMEOUT == t->rc )
    {
        kode_err_set("no RFID tag in %" PRIu32 " ms", t->ms);
    }
    else
    {
        rfid_fail(t->rc);
    }
}

/**
 * @brief Ask the front end until it answers or the time is up; the error is set on failure.
 * @param t The call; @c op, @c id for a write, @c ms and @c limit are set. A read goes window
 *          after window; a write is one try, the field staying up for its time.
 * @return true when the front end answered KODE_OK.
 */
static bool rfid_run(rfid_try_t *t)
{
    bool going = true;
    bool ok    = false;

    t->start = kode_sys_time_ms();
    t->rc    = KODE_TIMEOUT;
    while ( going )
    {
        if ( !rfid_tag_free() )
        {
            going = false;
        }
        else if ( kode_tag_window(t->start, t->ms, KODE_FOREVER) == 0U )
        {
            rfid_explain(t);
            going = false;
        }
        else
        {
            const uint32_t began = kode_sys_time_ms();

            (void)kode_retry(&rfid_attempt, t, kode_tag_window(t->start, t->ms, KODE_RFID_BUSY_MS));
            if ( KODE_OK == t->rc )
            {
                ok    = true;
                going = false;
            }
            else if ( (KODE_TIMEOUT != t->rc) || (RFID_OP_WRITE == t->op) )
            {
                rfid_explain(t);
                going = false;
            }
            else if ( (kode_sys_time_ms() - began) < t->window )
            {
                /* The window answered empty before its time: do not spin on it. */
                kode_block_delay_ms(kode_tag_window(t->start, t->ms, KODE_RETRY_STEP_MS));
            }
            else
            {
                /* The window ran its length and found nothing: ask for what is left. */
            }
        }
    }
    return ok;
}

/* ---- Reading and writing ------------------------------------------------------- */

kode_tag_t kode_rfid_read_id(uint32_t ms)
{
    kode_tag_t r = { false, "", "" };

    /* Refused before the lock too: a call that plays or releases a tag holds it while it waits. */
    if ( kode_enter("kode_rfid_read_id") && rfid_tag_free() && rfid_lock_take("kode_rfid_read_id") )
    {
        rfid_try_t t;

        (void)memset(&t, 0, sizeof(t));
        t.op    = RFID_OP_READ;
        t.ms    = ms;
        t.limit = KODE_RFID_POLL_MS;
        if ( rfid_run(&t) )
        {
            r.ok = kode_tag_hex_format(t.id, KODE_RFID_ID_BYTES, r.id, sizeof(r.id));
            (void)snprintf(r.name, sizeof(r.name), "%s", KODE_RFID_NAME);
        }
        rfid_leave();
    }
    return r;
}

bool kode_rfid_write(const char *id, uint32_t ms)
{
    bool ok = false;

    if ( kode_enter("kode_rfid_write") )
    {
        rfid_try_t t;

        (void)memset(&t, 0, sizeof(t));
        t.op = RFID_OP_WRITE;
        if ( rfid_id_parse(id, t.id) && rfid_tag_free() && rfid_lock_take("kode_rfid_write") )
        {
            t.ms    = ms;
            t.limit = KODE_RFID_WRITE_MAX_MS;
            ok      = rfid_run(&t);
            rfid_leave();
        }
    }
    return ok;
}

/* ---- Emulation ----------------------------------------------------------------- */

/**
 * @brief Stop playing a tag and wait for the front end to be free, with the lock held.
 * @return true when no tag is played; false with the error set.
 */
static bool tag_release(void)
{
    static const kode_tag_emu_t tag_emu = {
        &kode_block_rfid_emulate_stop, &kode_sys_rfid_is_emulating, &rfid_fail, KODE_RFID_RELEASING_TEXT, KODE_RFID_BUSY_MS
    };

    return kode_tag_emu_release(&tag_emu);
}

/** @brief What one try of a start carries. */
typedef struct {
    const uint8_t *id;  /**< What to play.     */
    kode_status_t  rc;  /**< The last answer.  */
} tag_try_t;

/** @brief One try of a start: true unless another holder has the front end. */
static bool tag_start_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the tag_try_t tag_present() passed */
    tag_try_t *const t = (tag_try_t *)ctx;

    t->rc = kode_block_rfid_emulate_start(t->id);
    return KODE_DENIED != t->rc;
}

/** @brief Whether the front end plays a tag. */
static bool tag_up(void *ctx)
{
    (void)ctx;
    return kode_sys_rfid_is_emulating();
}

/**
 * @brief Play an ID, replacing a tag already played; the lock is held.
 * @param id The ID.
 * @return true when the tag plays; false with the error set.
 */
static bool tag_present(const uint8_t *id)
{
    bool ok = false;

    if ( tag_release() )
    {
        tag_try_t t = { id, KODE_ERROR };

        (void)kode_retry(&tag_start_attempt, &t, KODE_RFID_BUSY_MS);
        /* A start that timed out may still succeed late: a tag that plays is the answer. */
        if ( KODE_TIMEOUT == t.rc )
        {
            ok = kode_retry(&tag_up, NULL, KODE_RFID_LOOK_MS);
        }
        else
        {
            ok = (KODE_OK == t.rc);
        }
        if ( !ok )
        {
            rfid_fail(t.rc);
        }
    }
    return ok;
}

bool kode_rfid_emulate_start(const char *id)
{
    bool ok = false;

    if ( kode_enter("kode_rfid_emulate_start") )
    {
        uint8_t bytes[KODE_RFID_ID_BYTES];

        if ( rfid_id_parse(id, bytes) && rfid_lock_take("kode_rfid_emulate_start") )
        {
            ok = tag_present(bytes);
            rfid_leave();
        }
    }
    return ok;
}

bool kode_rfid_emulate_stop(void)
{
    bool ok = false;

    if ( kode_enter("kode_rfid_emulate_stop") && rfid_lock_take("kode_rfid_emulate_stop") )
    {
        ok = tag_release();
        rfid_leave();
    }
    return ok;
}
