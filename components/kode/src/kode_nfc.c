/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_nfc.c
 * @brief The standard layer's NFC: tags read, written and emulated by name.
 */
#include "kode.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "khal_once.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "knfc_emulate.h"
#include "knfc_tag.h"
#include "kode_entry.h"
#include "kode_nfc.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_tag.h"
#include "kqp.h"

_Static_assert(KODE_NFC_TEXT_MAX == (KODE_SYS_NFC_TEXT_MAX + 1U),
               "the NFC text and the facade's longest message have drifted apart");
_Static_assert(KODE_NFC_TEXT_MAX == (KNFC_EMU_NDEF_MAX + 1U),
               "the NFC text and the longest emulated message have drifted apart");
_Static_assert(KODE_NFC_TAG_DATA_MAX == (KODE_SYS_NFC_TEXT_MAX - 1U),
               "a tag's text and the facade's longest message have drifted apart");
_Static_assert(KODE_TAG_ID_MAX >= (3U * KNFC_UID_MAX),
               "a tag's ID text does not hold the longest UID");

/** @brief The longest text or link of a profile, NUL excluded. */
#define KODE_NFC_DATA_MAX ((uint32_t)KODE_NFC_TEXT_MAX - 1U)

/** @brief The longest profile name, NUL excluded. */
#define KODE_NFC_NAME_LEN_MAX ((uint32_t)KODE_NFC_PROFILE_NAME_MAX - 1U)

/** @brief Bytes of the reading task's stack. */
#define KODE_NFC_STACK_BYTES 8192U

/** @brief The name of the reading task. */
#define KODE_NFC_TASK_NAME "kode_nfc"

/** @brief How long the window that tells a card the reader cannot use from a busy reader listens, ms. */
#define KODE_NFC_PROBE_MS 100U

/** @brief How long the reading rests between two looks at its stop flag, ms. */
#define KODE_NFC_REST_STEP_MS 100U

/** @brief Bytes of a single, double and triple UID. */
#define KODE_NFC_UID_SINGLE 4U
#define KODE_NFC_UID_DOUBLE 7U
#define KODE_NFC_UID_TRIPLE 10U

/** @brief The ATQA a clone presents for each UID length: the two size bits follow the length. */
#define KODE_NFC_ATQA_SINGLE 0x0004U
#define KODE_NFC_ATQA_DOUBLE 0x0044U
#define KODE_NFC_ATQA_TRIPLE 0x0084U

/** @brief The SAK a clone of a 4-byte UID presents: a Mifare Classic 1K. */
#define KODE_NFC_SAK_SINGLE 0x08U

/** @brief The SAK of every longer clone: a Type-2 tag. */
#define KODE_NFC_SAK_LONG 0x00U

/** @brief The text of a call refused while the Dot presents a card. */
#define KODE_NFC_EMULATING_TEXT "NFC is emulating a card; kode_nfc_emulate_stop() ends it"

/** @brief The text of a call refused while the background reading runs. */
#define KODE_NFC_READING_TEXT "NFC is reading; kode_nfc_scan_stop() ends it"

/** @brief The text of a stop that did not see the reading end. */
#define KODE_NFC_ENDING_TEXT "the NFC reading is still ending; call kode_nfc_scan_stop() again"

/** @brief The text of a stop that did not see the reader let go of the card. */
#define KODE_NFC_RELEASING_TEXT "the NFC reader is still letting the card go; call kode_nfc_emulate_stop() again"

/** @brief The text of a call that had no time to look for a tag. */
#define KODE_NFC_NO_TIME_TEXT "no NFC tag in 0 ms"

/** @brief The lock every public NFC call holds; made on the first call. */
static khal_mutex_t s_nfc_turn_lock;

/** @brief The reading's task exists, from before it starts to its last statement. */
static atomic_bool s_nfc_running;

/** @brief The reading is to end; the task reads it between windows. */
static atomic_bool s_nfc_stop;

/** @brief What a profile presents. */
typedef enum {
    NFC_TYPE_TEXT,  /**< A Type-2 tag with a Text record.  */
    NFC_TYPE_URI,   /**< A Type-2 tag with a URI record.   */
    NFC_TYPE_UID    /**< A clone of a card's identity.     */
} nfc_type_t;

/** @brief One emulation profile. */
typedef struct {
    bool       used;                              /**< The slot holds a profile.          */
    nfc_type_t type;                              /**< What it presents.                  */
    uint8_t    uid_len;                           /**< Bytes of @c uid, for a UID.        */
    uint8_t    uid[KNFC_UID_MAX];                 /**< The identity, for a UID.           */
    char       name[KODE_NFC_PROFILE_NAME_MAX];   /**< Its name, in lower case.           */
    char       text[KODE_NFC_TEXT_MAX];           /**< The text or link, for those types. */
} nfc_profile_t;

/** @brief The profiles; read and written under the lock. */
static nfc_profile_t s_profiles[KODE_NFC_PROFILES_MAX];

/** @brief Make the NFC lock. @return true when it exists. */
static bool nfc_lock_make(void)
{
    s_nfc_turn_lock = khal_mutex_create();
    return NULL != s_nfc_turn_lock;
}

/**
 * @brief Take the NFC lock, after kode_enter().
 * @param name The public name of the call.
 * @return true when the call has its turn; false with the error set.
 */
static bool nfc_lock_take(const char *name)
{
    static khal_once_t lock_once;  /* the one-shot the lock is made by */
    bool               ok = khal_once_try(&lock_once, &nfc_lock_make);

    if ( ok )
    {
        khal_mutex_lock(s_nfc_turn_lock);
    }
    else
    {
        kode_err_set("%s: no memory for the NFC lock", name);
    }
    return ok;
}

/** @brief Give the turn back, at the end of a call that took the lock. */
static void nfc_leave(void)
{
    khal_mutex_unlock(s_nfc_turn_lock);
}

/**
 * @brief Refuse a call that needs the reader while the Dot presents a card.
 * @return true when the reader is free of a card; false with the error set.
 */
static bool nfc_card_free(void)
{
    const bool free_of_card = !kode_sys_nfc_is_emulating();

    if ( !free_of_card )
    {
        kode_err_set("%s", KODE_NFC_EMULATING_TEXT);
    }
    return free_of_card;
}

/**
 * @brief Say why a call to the reader failed.
 * @param rc           The sys answer.
 * @param error_text   What KODE_ERROR means for the call.
 * @param no_resources What KODE_NO_RESOURCES means for the call.
 */
static void nfc_fail(kode_status_t rc, const char *error_text, const char *no_resources)
{
    switch ( rc )
    {
    case KODE_DENIED:
        kode_err_set("NFC is busy, or no reader is fitted");
        break;
    case KODE_NO_RESOURCES:
        kode_err_set("%s", no_resources);
        break;
    case KODE_TIMEOUT:
        kode_err_set("the NFC reader did not answer");
        break;
    case KODE_ERROR:
        kode_err_set("%s", error_text);
        break;
    default:
        kode_err_set("the NFC reader could not be used");
        break;
    }
}

/**
 * @brief Read a type word.
 * @param type      As typed.
 * @param allow_uid Whether "uid" is a type here.
 * @param out       Out: the type; written only on success.
 * @return true when it is one; false with the error set.
 */
static bool type_parse(const char *type, bool allow_uid, nfc_type_t *out)
{
    bool ok = true;

    if ( NULL == type )
    {
        kode_err_set("no NFC type given");
        ok = false;
    }
    else if ( kode_name_is(type, "text") )
    {
        *out = NFC_TYPE_TEXT;
    }
    else if ( kode_name_is(type, "uri") )
    {
        *out = NFC_TYPE_URI;
    }
    else if ( allow_uid && kode_name_is(type, "uid") )
    {
        *out = NFC_TYPE_UID;
    }
    else
    {
        kode_err_set("unknown NFC type '%s'", type);
        ok = false;
    }
    return ok;
}

/**
 * @brief Check the text or link of a write or a profile.
 * @param data As given.
 * @param max  The most bytes it may hold, NUL excluded.
 * @return true when it can be used; false with the error set.
 */
static bool data_check(const char *data, uint32_t max)
{
    bool ok = true;

    if ( NULL == data )
    {
        kode_err_set("no data given");
        ok = false;
    }
    else if ( strlen(data) > max )
    {
        kode_err_set("%zu bytes is too long (%" PRIu32 " at most)", strlen(data), max);
        ok = false;
    }
    else
    {
        /* fits */
    }
    return ok;
}

/* ---- One call to the reader, window after window ----------------------------- */

/** @brief What a call to the reader asks of it. */
typedef enum {
    NFC_OP_TAG,    /**< Find a tag.                      */
    NFC_OP_READ,   /**< Read the text or link on a tag.  */
    NFC_OP_WRITE   /**< Write a text or link to a tag.   */
} nfc_op_t;

/** @brief What one try of a call carries. */
typedef struct {
    nfc_op_t      op;        /**< What is asked.                                           */
    knfc_tag_t    tag;       /**< The tag found, or the card a probe met.                  */
    char         *text;      /**< A read: where the text goes, KODE_NFC_TEXT_MAX bytes.    */
    const char   *data;      /**< A write: what to write.                                  */
    size_t        len;       /**< A read: bytes of text.                                   */
    bool          uri;       /**< A read: the record was a link. A write: write a link.    */
    bool          unusable;  /**< A card is on the antenna and cannot be used this way.    */
    uint32_t      start;     /**< When the call began, in ms.                              */
    uint32_t      ms;        /**< The whole time, or KODE_FOREVER.                         */
    uint32_t      window;    /**< How long this try listens, in ms.                        */
    kode_status_t rc;        /**< The last answer.                                         */
} nfc_try_t;

/** @brief One try of a call, sized by what is left of it: true unless another holder has the reader. */
static bool nfc_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the nfc_try_t nfc_run() passed */
    nfc_try_t *const t = (nfc_try_t *)ctx;

    t->window = kode_tag_window(t->start, t->ms, KODE_NFC_POLL_MS);
    /* No time left is a timeout, or busy when a holder was the last answer: the reader is never asked for 0 ms. */
    if ( 0U == t->window )
    {
        if ( KODE_DENIED != t->rc )
        {
            t->rc = KODE_TIMEOUT;
        }
    }
    else
    {
        switch ( t->op )
        {
        case NFC_OP_READ:
            t->rc = kode_block_nfc_ndef_read(t->text, KODE_NFC_TEXT_MAX, &t->len, &t->uri, t->window);
            break;
        case NFC_OP_WRITE:
            if ( t->uri )
            {
                t->rc = kode_block_nfc_ndef_write_uri(t->data, t->window);
            }
            else
            {
                t->rc = kode_block_nfc_ndef_write_text(t->data, t->window);
            }
            break;
        default:
            t->rc = kode_block_nfc_read_tag(&t->tag, t->window);
            break;
        }
        /* DENIED is a busy reader, no reader, or a card the call cannot use: a card on the antenna tells which. */
        if ( (KODE_DENIED == t->rc) && (NFC_OP_TAG != t->op) )
        {
            const uint32_t probe = kode_tag_window(t->start, t->ms, KODE_NFC_PROBE_MS);

            t->unusable = (0U < probe) && (kode_block_nfc_read_tag(&t->tag, probe) == KODE_OK);
        }
    }
    return (KODE_DENIED != t->rc) || t->unusable;
}

/**
 * @brief The name of a card, for a message.
 * @param tag The card.
 * @return A static text.
 */
static const char *card_name(const knfc_tag_t *tag)
{
    const char *name = knfc_tag_model(tag->atqa, tag->sak);

    if ( KNFC_TAG_MIFARE == tag->type )
    {
        name = "Mifare Classic";
    }
    return name;
}

/**
 * @brief Say why a call ended without its answer.
 * @param t  The last try.
 * @param ms The time the call had.
 */
static void nfc_explain(const nfc_try_t *t, uint32_t ms)
{
    if ( KODE_TIMEOUT == t->rc )
    {
        kode_err_set("no NFC tag in %" PRIu32 " ms", ms);
    }
    else if ( t->unusable && (NFC_OP_READ == t->op) )
    {
        kode_err_set("this card's data cannot be read (%s)", card_name(&t->tag));
    }
    else if ( t->unusable )
    {
        kode_err_set("this card cannot be written (%s)", card_name(&t->tag));
    }
    else if ( NFC_OP_READ == t->op )
    {
        nfc_fail(t->rc, "the tag holds no text or link", "the tag's text is too long");
    }
    else
    {
        nfc_fail(t->rc, "the tag could not be written", "the data does not fit on the tag");
    }
}

/**
 * @brief Ask the reader window after window until it answers or @p ms is up; the error is set on failure.
 * @param t  The call; @c op and what it needs are set.
 * @param ms The time to wait, or KODE_FOREVER; above 0.
 * @return true when the reader answered KODE_OK.
 */
static bool nfc_run(nfc_try_t *t, uint32_t ms)
{
    bool going = true;
    bool ok    = false;

    t->start    = kode_sys_time_ms();
    t->ms       = ms;
    t->unusable = false;
    while ( going )
    {
        const uint32_t began = kode_sys_time_ms();

        /* The wait for a holder is part of the time asked for, never past it. */
        (void)kode_retry(&nfc_attempt, t, kode_tag_window(t->start, ms, KODE_NFC_BUSY_MS));
        if ( KODE_OK == t->rc )
        {
            ok    = true;
            going = false;
        }
        else if ( KODE_TIMEOUT != t->rc )
        {
            nfc_explain(t, ms);
            going = false;
        }
        else if ( (kode_sys_time_ms() - began) < t->window )
        {
            /* The window answered empty before its time: do not spin on it. */
            kode_block_delay_ms(KODE_RETRY_STEP_MS);
        }
        else
        {
            /* The window ran its length and found nothing: ask for what is left. */
        }
        /* The time is up, also when the pause above used the last of it: never a window of 0 ms. */
        if ( going && (kode_tag_window(t->start, ms, KODE_FOREVER) == 0U) )
        {
            nfc_explain(t, ms);
            going = false;
        }
    }
    return ok;
}

/* ---- Tags, text and links ----------------------------------------------------- */

kode_tag_t kode_nfc_read_id(uint32_t ms)
{
    kode_tag_t r = { false, "", "" };

    /* Refused before the lock too: a call that presents or releases a card holds it while it waits. */
    if ( kode_enter("kode_nfc_read_id") && nfc_card_free() && nfc_lock_take("kode_nfc_read_id") )
    {
        if ( !nfc_card_free() )
        {
            /* The error is set. */
        }
        else if ( 0U == ms )
        {
            kode_err_set(KODE_NFC_NO_TIME_TEXT);
        }
        else
        {
            nfc_try_t t;

            (void)memset(&t, 0, sizeof(t));
            t.op = NFC_OP_TAG;
            if ( nfc_run(&t, ms) )
            {
                r.ok = kode_tag_hex_format(t.tag.uid, t.tag.uid_len, r.id, sizeof(r.id));
                (void)snprintf(r.name, sizeof(r.name), "%s", knfc_tag_model(t.tag.atqa, t.tag.sak));
            }
        }
        nfc_leave();
    }
    return r;
}

/**
 * @brief Read the text or link of a tag into the task's buffer, with the lock held.
 * @param text     Where it goes, KODE_NFC_TEXT_MAX bytes.
 * @param want_uri true for a link, false for text.
 * @param ms       The time to wait, or KODE_FOREVER.
 */
static void text_read(char *text, bool want_uri, uint32_t ms)
{
    if ( !nfc_card_free() )
    {
        /* The error is set. */
    }
    else if ( 0U == ms )
    {
        kode_err_set(KODE_NFC_NO_TIME_TEXT);
    }
    else
    {
        nfc_try_t t;

        (void)memset(&t, 0, sizeof(t));
        t.op   = NFC_OP_READ;
        t.text = text;
        if ( nfc_run(&t, ms) && (t.uri != want_uri) )
        {
            text[0] = '\0';
            if ( want_uri )
            {
                kode_err_set("the tag holds text, not a link");
            }
            else
            {
                kode_err_set("the tag holds a link, not text");
            }
        }
    }
}

const char *kode_nfc_read(const char *type, uint32_t ms)
{
    /* What a read that cannot keep a result answers: no text. */
    static const char none[] = "";
    const char       *result = none;

    if ( kode_enter("kode_nfc_read") )
    {
        nfc_type_t kind = NFC_TYPE_TEXT;

        if ( type_parse(type, false, &kind) )
        {
            struct kode_tag_state *const state = kode_tag_state_ensure();

            if ( NULL == state )
            {
                kode_err_set("out of memory");
            }
            else
            {
                state->nfc_text[0] = '\0';
                result             = state->nfc_text;
                if ( nfc_card_free() && nfc_lock_take("kode_nfc_read") )
                {
                    text_read(state->nfc_text, NFC_TYPE_URI == kind, ms);
                    nfc_leave();
                }
            }
        }
    }
    return result;
}

bool kode_nfc_write(const char *type, const char *data, uint32_t ms)
{
    bool ok = false;

    if ( kode_enter("kode_nfc_write") )
    {
        nfc_type_t kind = NFC_TYPE_TEXT;

        if ( type_parse(type, false, &kind) && data_check(data, KODE_NFC_TAG_DATA_MAX) && nfc_card_free() &&
            nfc_lock_take("kode_nfc_write") )
        {
            if ( !nfc_card_free() )
            {
                /* The error is set. */
            }
            else if ( 0U == ms )
            {
                kode_err_set(KODE_NFC_NO_TIME_TEXT);
            }
            else
            {
                nfc_try_t t;

                (void)memset(&t, 0, sizeof(t));
                t.op   = NFC_OP_WRITE;
                t.data = data;
                t.uri  = (NFC_TYPE_URI == kind);
                ok     = nfc_run(&t, ms);
            }
            nfc_leave();
        }
    }
    return ok;
}

/* ---- The background reading --------------------------------------------------- */

/**
 * @brief Pause, in steps that look at the stop flag.
 * @param ms How long.
 */
static void reading_rest(uint32_t ms)
{
    uint32_t left = ms;

    while ( (0U < left) && !atomic_load(&s_nfc_stop) )
    {
        uint32_t step = KODE_NFC_REST_STEP_MS;

        if ( left < step )
        {
            step = left;
        }
        kode_block_delay_ms(step);
        left -= step;
    }
}

/** @brief The reading's task: a window after a window until told to stop. */
static void reading_task(void *arg)
{
    knfc_tag_t tag;

    (void)arg;
    while ( !atomic_load(&s_nfc_stop) )
    {
        uint32_t rest = KODE_RETRY_STEP_MS;

        khal_mutex_lock(s_nfc_turn_lock);
        if ( kode_block_nfc_read_tag(&tag, KODE_NFC_POLL_MS) != KODE_TIMEOUT )
        {
            /* A tag is heard once a second; a reader held from outside or absent is not hammered. */
            rest = KODE_NFC_REPEAT_MS;
        }
        khal_mutex_unlock(s_nfc_turn_lock);
        /* Out of the reader for a moment, so a call waiting for its turn gets it. */
        reading_rest(rest);
    }
    atomic_store(&s_nfc_running, false);
}

/** @brief Whether the reading's task has ended. */
static bool reading_ended(void *ctx)
{
    (void)ctx;
    return !atomic_load(&s_nfc_running);
}

/**
 * @brief Answer a start that finds a reading: it runs on unless it is being stopped.
 * @return true when the reading runs on; false with the error set.
 */
static bool reading_already(void)
{
    const bool ending = atomic_load(&s_nfc_stop);

    if ( ending )
    {
        kode_err_set("%s", KODE_NFC_ENDING_TEXT);
    }
    return !ending;
}

/**
 * @brief Start the reading's task, with the lock held.
 * @return true when it runs; false with the error set.
 */
static bool reading_launch(void)
{
    bool ok = false;

    atomic_store(&s_nfc_stop, false);
    atomic_store(&s_nfc_running, true);
    if ( khal_task_start(&reading_task, KODE_NFC_TASK_NAME, KODE_NFC_STACK_BYTES, KODE_PRIO_STANDARD_READER,
                        NULL) == KODE_OK )
    {
        ok = true;
    }
    else
    {
        atomic_store(&s_nfc_running, false);
        kode_err_set("no memory for the NFC reading's task");
    }
    return ok;
}

bool kode_nfc_scan_start(void)
{
    bool ok = false;

    if ( kode_enter("kode_nfc_scan_start") )
    {
        if ( atomic_load(&s_nfc_running) )
        {
            /* Answered without the lock: the reading holds it for its windows. */
            ok = reading_already();
        }
        else if ( nfc_lock_take("kode_nfc_scan_start") )
        {
            if ( !nfc_card_free() )
            {
                /* The error is set. */
            }
            else if ( atomic_load(&s_nfc_running) )
            {
                ok = reading_already();
            }
            else
            {
                ok = reading_launch();
            }
            nfc_leave();
        }
        else
        {
            /* The error is set. */
        }
    }
    return ok;
}

/**
 * @brief End the task, as the one stop in progress.
 * @return true when the task ended; false with the error set.
 */
static bool reading_end(void)
{
    bool ok = true;

    if ( atomic_load(&s_nfc_running) )
    {
        /* The lock is not taken to wait: the task holds it for every window. */
        atomic_store(&s_nfc_stop, true);
        ok = kode_retry(&reading_ended, NULL, KODE_NFC_STOP_MS);
        if ( !ok )
        {
            kode_err_set("%s", KODE_NFC_ENDING_TEXT);
        }
    }
    return ok;
}

bool kode_nfc_scan_stop(void)
{
    /* A stop is waiting for the task: the only caller that may set the stop flag. */
    static atomic_bool s_nfc_closing;
    bool               ok = false;

    if ( kode_enter("kode_nfc_scan_stop") )
    {
        if ( !atomic_load(&s_nfc_running) )
        {
            ok = true;
        }
        else if ( atomic_exchange(&s_nfc_closing, true) )
        {
            kode_err_set("the NFC reading is already being stopped");
        }
        else
        {
            /* Only a stop that holds the exchange sets the flag, and it looks again first: a flag set
             * after the task ended would end a reading started since. */
            ok = reading_end();
            atomic_store(&s_nfc_closing, false);
        }
    }
    return ok;
}

/* ---- Emulation ----------------------------------------------------------------- */

/** @brief Tell nfc_fail() why a stop failed. */
static void card_stop_fail(kode_status_t rc)
{
    nfc_fail(rc, "", "");
}

/** @brief Ask the reader to stop presenting. @return The sys answer. */
static kode_status_t card_stop(void)
{
    return kode_block_nfc_emulate_stop(NULL);
}

/**
 * @brief Stop presenting a card and wait for the reader to be free, with the lock held.
 * @return true when no card is presented; false with the error set.
 */
static bool card_release(void)
{
    static const kode_tag_emu_t card_emu = {
        &card_stop, &kode_sys_nfc_is_emulating, &card_stop_fail, KODE_NFC_RELEASING_TEXT, KODE_NFC_BUSY_MS
    };

    return kode_tag_emu_release(&card_emu);
}

/**
 * @brief The ATQA a clone of a UID of this length presents.
 * @param uid_len 4, 7 or 10.
 * @return The ATQA.
 */
static uint16_t uid_atqa(uint8_t uid_len)
{
    uint16_t atqa = KODE_NFC_ATQA_SINGLE;

    if ( KODE_NFC_UID_DOUBLE == uid_len )
    {
        atqa = KODE_NFC_ATQA_DOUBLE;
    }
    else if ( KODE_NFC_UID_TRIPLE == uid_len )
    {
        atqa = KODE_NFC_ATQA_TRIPLE;
    }
    else
    {
        /* a 4-byte UID */
    }
    return atqa;
}

/**
 * @brief Fill the card a profile presents.
 * @param p    The profile.
 * @param card Out: the card.
 */
static void card_build(const nfc_profile_t *p, knfc_emu_card_t *card)
{
    (void)memset(card, 0, sizeof(*card));
    if ( NFC_TYPE_UID == p->type )
    {
        (void)memcpy(card->uid, p->uid, p->uid_len);
        card->uid_len = p->uid_len;
        card->atqa    = uid_atqa(p->uid_len);
        card->sak     = KODE_NFC_SAK_LONG;
        if ( KODE_NFC_UID_SINGLE == p->uid_len )
        {
            card->sak = KODE_NFC_SAK_SINGLE;
        }
    }
    else
    {
        card->uri = (NFC_TYPE_URI == p->type);
        (void)snprintf(card->ndef, sizeof(card->ndef), "%s", p->text);
    }
}

/** @brief What one try of a start carries. */
typedef struct {
    const knfc_emu_card_t *card;  /**< What to present.     */
    kode_status_t          rc;    /**< The last answer.     */
} card_try_t;

/** @brief One try of a start: true unless another holder has the reader. */
static bool card_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the card_try_t card_present() passed */
    card_try_t *const t = (card_try_t *)ctx;

    t->rc = kode_block_nfc_emulate_start(t->card);
    return KODE_DENIED != t->rc;
}

/**
 * @brief Find a profile by name, in any case.
 * @param name As typed.
 * @return The profile; NULL when there is none.
 */
static nfc_profile_t *nfc_profile_find(const char *name)
{
    nfc_profile_t *found = NULL;

    for ( uint32_t i = 0U; (NULL == found) && (KODE_NFC_PROFILES_MAX > i); i++ )
    {
        if ( s_profiles[i].used && kode_name_is(name, s_profiles[i].name) )
        {
            found = &s_profiles[i];
        }
    }
    return found;
}

/**
 * @brief Present the card of a profile, replacing a card already presented; the lock is held.
 * @param name The profile's name.
 * @return true when the card is up; false with the error set.
 */
static bool card_present(const char *name)
{
    const nfc_profile_t *const p  = nfc_profile_find(name);
    bool                       ok = false;

    if ( atomic_load(&s_nfc_running) )
    {
        kode_err_set("%s", KODE_NFC_READING_TEXT);
    }
    else if ( NULL == p )
    {
        kode_err_set("no emulation profile '%s'; add one with kode_nfc_add_profile()", name);
    }
    else if ( card_release() )
    {
        knfc_emu_card_t card;
        card_try_t      t = { &card, KODE_ERROR };

        card_build(p, &card);
        (void)kode_retry(&card_attempt, &t, KODE_NFC_BUSY_MS);
        ok = (KODE_OK == t.rc);
        if ( !ok )
        {
            nfc_fail(t.rc, "no NFC reader is fitted, or it would not present the card", "out of memory");
        }
    }
    else
    {
        /* The error is set. */
    }
    return ok;
}

bool kode_nfc_emulate_start(const char *profile)
{
    bool ok = false;

    if ( kode_enter("kode_nfc_emulate_start") )
    {
        if ( NULL == profile )
        {
            kode_err_set("no emulation profile name given");
        }
        else if ( atomic_load(&s_nfc_running) )
        {
            /* Refused without the lock: the reading holds it for its windows. */
            kode_err_set("%s", KODE_NFC_READING_TEXT);
        }
        else if ( nfc_lock_take("kode_nfc_emulate_start") )
        {
            ok = card_present(profile);
            nfc_leave();
        }
        else
        {
            /* The error is set. */
        }
    }
    return ok;
}

bool kode_nfc_emulate_stop(void)
{
    bool ok = false;

    if ( kode_enter("kode_nfc_emulate_stop") && nfc_lock_take("kode_nfc_emulate_stop") )
    {
        ok = card_release();
        nfc_leave();
    }
    return ok;
}

/* ---- Profiles ------------------------------------------------------------------ */

/**
 * @brief Read the identity of a UID profile.
 * @param data As typed.
 * @param p    Out: its UID and length; written only on success.
 * @return true when it is 4, 7 or 10 bytes of hex; false with the error set.
 */
static bool uid_parse(const char *data, nfc_profile_t *p)
{
    uint8_t  uid[KNFC_UID_MAX];
    uint32_t n  = 0U;
    bool     ok = false;

    if ( NULL == data )
    {
        kode_err_set("no data given");
    }
    else if ( kode_tag_hex_parse(data, uid, KNFC_UID_MAX, &n) &&
             ((KODE_NFC_UID_SINGLE == n) || (KODE_NFC_UID_DOUBLE == n) || (KODE_NFC_UID_TRIPLE == n)) )
    {
        (void)memcpy(p->uid, uid, n);
        p->uid_len = (uint8_t)n;
        ok         = true;
    }
    else
    {
        kode_err_set("a UID is 4, 7 or 10 bytes of hex, such as 04:A2:3B:91");
    }
    return ok;
}

/**
 * @brief Check a profile's name.
 * @param name As typed.
 * @return true when it can name a profile; false with the error set.
 */
static bool profile_name_check(const char *name)
{
    bool ok = true;

    if ( (NULL == name) || ('\0' == name[0]) )
    {
        kode_err_set("no profile name given");
        ok = false;
    }
    else if ( strlen(name) > KODE_NFC_NAME_LEN_MAX )
    {
        kode_err_set("profile name '%s' is too long (%" PRIu32 " bytes at most)", name,
                     KODE_NFC_NAME_LEN_MAX);
        ok = false;
    }
    else
    {
        /* fits */
    }
    return ok;
}

/**
 * @brief Check a profile's arguments and make it.
 * @param name As typed.
 * @param type As typed.
 * @param data As typed.
 * @param p    Out: the profile, not yet named or marked used.
 * @return true when it can be added; false with the error set.
 */
static bool profile_make(const char *name, const char *type, const char *data, nfc_profile_t *p)
{
    bool ok = false;

    (void)memset(p, 0, sizeof(*p));
    if ( profile_name_check(name) && type_parse(type, true, &p->type) )
    {
        if ( NFC_TYPE_UID == p->type )
        {
            ok = uid_parse(data, p);
        }
        else
        {
            ok = data_check(data, KODE_NFC_DATA_MAX);
            if ( ok && ('\0' == data[0]) )
            {
                /* The reader presents its own default message for an empty one. */
                kode_err_set("a text or link profile needs some data");
                ok = false;
            }
            if ( ok )
            {
                (void)snprintf(p->text, sizeof(p->text), "%s", data);
            }
        }
    }
    return ok;
}

/**
 * @brief Copy a profile name in lower case, so the table compares names without case.
 * @param dst KODE_NFC_PROFILE_NAME_MAX bytes.
 * @param src The name, already checked to fit.
 */
static void name_store(char *dst, const char *src)
{
    uint32_t i = 0U;

    while ( (KODE_NFC_NAME_LEN_MAX > i) && ('\0' != src[i]) )
    {
        dst[i] = kode_ascii_lower(src[i]);
        i++;
    }
    dst[i] = '\0';
}

/**
 * @brief Keep a profile in the slot of its name, or the first free one; the lock is held.
 * @param p The profile, made.
 * @return true when kept; false with the error set (the table is full).
 */
static bool profile_keep(nfc_profile_t *p)
{
    nfc_profile_t *slot = nfc_profile_find(p->name);

    for ( uint32_t i = 0U; (NULL == slot) && (KODE_NFC_PROFILES_MAX > i); i++ )
    {
        if ( !s_profiles[i].used )
        {
            slot = &s_profiles[i];
        }
    }
    if ( NULL == slot )
    {
        kode_err_set("all %" PRIu32 " emulation profiles are in use", (uint32_t)KODE_NFC_PROFILES_MAX);
    }
    else
    {
        p->used = true;
        *slot   = *p;
    }
    return NULL != slot;
}

bool kode_nfc_add_profile(const char *name, const char *type, const char *data)
{
    bool ok = false;

    if ( kode_enter("kode_nfc_add_profile") )
    {
        nfc_profile_t p;

        if ( profile_make(name, type, data, &p) && nfc_lock_take("kode_nfc_add_profile") )
        {
            name_store(p.name, name);
            ok = profile_keep(&p);
            nfc_leave();
        }
    }
    return ok;
}
