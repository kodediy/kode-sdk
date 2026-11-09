/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file nfc_ao.c
 * @brief AO_Nfc: the reader's owner, and a field that lives only while asked.
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "kaos.h"
#include "kaos_ask.h"
#include "kaos_start.h"
#include "kaos_claim.h"
#include "kbsp_owners.h"
#include "kdrv_st25r3916.h"
#include "khal_identity.h"
#include "khal_sync.h"
#include "khal_time.h"
#include "knfc_emulate.h"
#include "knfc_mfc.h"
#include "knfc_mfc_dict.h"
#include "knfc_tag.h"
#include "kode_log.h"
#include "kqp.h"
#include "kverbs.h"
#include "krows.h"

#define TAG "kaos.nfc"

/* cppcheck-suppress misra-c2012-5.9 ; QP's module name for Q_ASSERT: one static of that name per file is QP's design */
Q_DEFINE_THIS_MODULE("nfc_ao")

_Static_assert(sizeof(NfcCard) == sizeof(knfc_emu_card_t),
               "an ask's card is copied into the driver's by size");

/** @brief The pump's period: one discovery step, or one listen burst. */
#define NFC_TICK_MS  10U

/** @brief What kind of request is in flight. */
typedef enum {
    REQ_NONE = 0,
    REQ_TAG,     /**< Identity only: NFC_READ_REQ_SIG.        */
    REQ_NDEF,    /**< Read or write a message.                */
    REQ_ROW,     /**< The production row's own discovery.     */
    REQ_SWEEP,   /**< A Mifare dictionary sweep.              */
    REQ_DUMP,    /**< A Mifare full-card dump.                */
    REQ_FIELD,   /**< A listen window: NFC_FIELD_REQ_SIG.     */
    REQ_EMU,     /**< A held emulation window: NFC_EMU_REQ_SIG. */
    REQ_CERT,    /**< A held FCC certification carrier (cert build only). */
} req_kind_t;

/** @brief True from the moment an emulation window is up until its reader is
 *         released; read from any task through nfc_is_emulating(). */
static atomic_bool s_emu_window;

/** @brief The request in flight, and where its answer goes. */
typedef struct {
    req_kind_t  kind;
    void       *out;    /**< knfc_tag_t * for REQ_TAG, knfc_field_t *
                         *   for REQ_FIELD.                     */
    char       *text;   /**< For REQ_NDEF.                       */
    size_t      cap;
    size_t     *n;
    bool       *is_uri; /**< For REQ_NDEF reads: the record's kind. */
    bool        write;
    bool        uri;
    uint32_t   *rc;     /**< For REQ_TAG, REQ_NDEF and REQ_FIELD. */
    QActive    *who;    /**< Set when an AO asked: the answer is posted. */
    khal_sem_t  done;
    uint32_t    gen;
    uint32_t   *ack;
    QActive    *ask;    /**< An ask's AO: the answer below is posted to it. */
    NfcReadRspEvt  *rd; /**< REQ_TAG's ask answer, taken with the request.   */
    NfcNdefRspEvt  *nd; /**< REQ_NDEF's.                                     */
    NfcFieldRspEvt *fld; /**< REQ_FIELD's.                                    */
} Req;

/** @brief The production row's state. */
typedef struct {
    bool    running;
    uint8_t attempt;
    uint8_t mode;     /**< READER_TEST_*: what this run of the row judges. */
} Row;

/** @brief The last tag activated, whoever asked for it. */
typedef struct {
    bool       have;
    knfc_tag_t tag;
    uint32_t   at_ms;
} Last;

typedef struct {
    QActive  super;
    QTimeEvt tick;        /**< NFC_TICK_SIG, re-armed after each pass.    */
    QTimeEvt deadline;    /**< NFC_DEADLINE_SIG, one per request.         */
    uint32_t tick_stale;  /**< Expiries that belong to an older arming.   */
    uint32_t dl_stale;
    bool     tick_live;
    bool     dl_live;
    bool     present;
    bool     field;
    uint8_t  identity;
    uint32_t tags;
    uint32_t windows;
    uint32_t errors;
    Req      req;
    Row      row;
    Last     last;
    /* A dictionary sweep in progress: which block, where in the dictionary,
     * and who to tell. Batched one NFC_TICK at a time to keep each step short. */
    bool     sweeping;
    uint8_t  sweep_block;
    size_t   sweep_idx;
    QActive *sweep_who;
    knfc_tag_t sweep_tag;
    /* A full dump in progress: which sector next, and the running totals. */
    bool     dumping;
    uint8_t  dump_sector;
    uint8_t  dump_sectors;
    uint8_t  dump_read;
    uint16_t dump_blocks;
    QActive *dump_who;
    knfc_tag_t dump_tag;
    /* A held emulation window, with no deadline: the tick publishes each edge
     * of the driver's snapshot. emu_stopping: a stop is waiting for the worker
     * to release the reader. emu_pub_*: the last state published. */
    bool     emu_stopping;
    QActive *emu_ask;     /**< The window's opener: its ask's AO, NULL for a
                           *   waiting task or the console. Only it may stop it. */
    bool     emu_pub_field;
    bool     emu_pub_selected;
    uint32_t emu_pub_reads;
    /* An NDEF ask's message in or out, and whether the record read was a URI:
     * the request event is recycled long before a tag turns up. */
    char     ask_text[KQP_NFC_TEXT_MAX];
    bool     ask_uri;
#if CONFIG_KODE_LOWPOWER
    uint8_t  lp_who;      /**< The id lowpower_join() gave.                 */
#endif
} Nfc;

static Nfc l_nfc;
QActive * const AO_Nfc = &l_nfc.super;

static QState Nfc_initial(Nfc * const me, void const * const par);
static QState Nfc_ready(Nfc * const me, QEvt const * const e);

/* The row's retry is reached from the tag path above it as well as from the
 * deadline below, so it is declared here rather than moved. */
static void row_retry(Nfc * const me);
static void dump_step(Nfc * const me);
static void dump_finish(Nfc * const me);
static void dump_refuse(Nfc * const me, QActive *who);

/* ---- The asks: kode_sys's requests, answered to the asking AO --------------
 * One door per asker and kind: an ask takes it, the answer opens it again
 * just before it is posted, so the asker may ask again from the answer's
 * handler. The reader's own contention is the handler's refusal, answered.
 */

/** @brief The kinds of ask, one door each per asker. */
enum {
    NFC_ASK_READ = 0,  /**< nfc_read_ask()                              */
    NFC_ASK_NDEF,      /**< nfc_ndef_ask(), a read or a write           */
    NFC_ASK_EMU,       /**< nfc_emulate_ask() and nfc_emulate_stop_ask() */
    NFC_ASK_FIELD,     /**< nfc_field_ask()                             */
    NFC_ASK_CERT,      /**< nfc_cert_ask()                              */
    NFC_ASK_KINDS      /**< How many there are. */
};

/** @brief The doors, one per kind of ask. */
static kaos_ask_door_t s_nfc_doors[NFC_ASK_KINDS];

/**
 * @brief Fill a read's answer.
 * @param a   The answer, taken with the request.
 * @param rc  What the read came to.
 * @param tag The tag, or NULL for none.
 */
static void read_pack(NfcReadRspEvt * const a, kode_status_t rc, const knfc_tag_t *tag)
{
    a->rc = (uint8_t)rc;
    (void)memset(&a->tag, 0, sizeof(a->tag));
    if ( NULL != tag )
    {
        (void)memcpy(a->tag.uid, tag->uid, KQP_NFC_UID_MAX);
        a->tag.uid_len = tag->uid_len;
        a->tag.atqa    = tag->atqa;
        a->tag.sak     = tag->sak;
        a->tag.type    = (uint8_t)tag->type;
    }
}

/* ---- Timers -------------------------------------------------------------- *
 * A one-shot that has expired and posted cannot be told from one never armed,
 * so each arming is counted and a late expiry is dropped rather than allowed
 * to end the request that replaced it.
 */

/**
 * @brief @p ms as QP ticks.
 *
 * @param ms The interval.
 * @return The tick count, never zero - QP asserts on a one-shot of zero.
 */
static uint32_t ms_ticks(uint32_t ms)
{
    uint32_t ticks = KQP_TICKS_PER_MS(ms);

    if ( 0U == ticks )
    {
        ticks = 1U;
    }
    return ticks;
}

/**
 * @brief Disarm a one-shot and say whether its event is already on the way.
 *
 * @param te   The one-shot.
 * @param live Whether the caller considers a previous arming outstanding.
 * @return true when an expired event for @p te is still queued.
 */
static bool timer_stale(QTimeEvt * const te, bool live)
{
    bool stale = false;

    if ( !QTimeEvt_disarm(te) )
    {
        stale = live;
    }
    return stale;
}

/** @brief Arm the pump for its next pass, NFC_TICK_MS from now. */
static void tick_arm(Nfc * const me)
{
    if ( timer_stale(&me->tick, me->tick_live) )
    {
        me->tick_stale += 1U;
    }
    me->tick_live = true;
    QTimeEvt_armX(&me->tick, KQP_TICKS_PER_MS(NFC_TICK_MS), 0U);
}

/** @brief Stop the pump. */
static void tick_stop(Nfc * const me)
{
    if ( timer_stale(&me->tick, me->tick_live) )
    {
        me->tick_stale += 1U;
    }
    me->tick_live = false;
}

/** @brief Arm the request's deadline for @p ms. */
static void deadline_arm(Nfc * const me, uint32_t ms)
{
    if ( timer_stale(&me->deadline, me->dl_live) )
    {
        me->dl_stale += 1U;
    }
    me->dl_live = true;
    QTimeEvt_armX(&me->deadline, ms_ticks(ms), 0U);
}

/** @brief Stop the request's deadline. */
static void deadline_stop(Nfc * const me)
{
    if ( timer_stale(&me->deadline, me->dl_live) )
    {
        me->dl_stale += 1U;
    }
    me->dl_live = false;
}

/* ---- The field ----------------------------------------------------------- */

/** @brief Take the field down and stop pumping. Safe when it is already down. */
static void field_down(Nfc * const me)
{
    if ( me->field )
    {
        kdrv_st25r3916_field_off();
        me->field = false;
    }
    tick_stop(me);
}

/* ---- Answering ----------------------------------------------------------- */

/**
 * @brief Hand the request its answer and let its caller go.
 *
 * The generation is written into @c ack before @c done is signalled, and that
 * write is what orders everything else written through the requester's
 * pointers against a reader on the other core.
 *
 * @param me The AO.
 */
static void req_release(Nfc * const me)
{
    Req * const r = &me->req;

    if ( REQ_NONE == r->kind )
    {
        return;
    }
    if ( NULL != r->ack )
    {
        *r->ack = r->gen;
    }
    if ( NULL != r->done )
    {
        khal_sem_signal(r->done);
    }
    if ( NULL != r->ask )
    {
        QActive * const who = r->ask;

        r->ask = NULL;
        switch ( r->kind )
        {
            case REQ_TAG: {
                kaos_ask_answer(&s_nfc_doors[NFC_ASK_READ], who, &r->rd->super, AO_Nfc);
                break;
            }
            case REQ_NDEF: {
                kaos_ask_answer(&s_nfc_doors[NFC_ASK_NDEF], who, &r->nd->super, AO_Nfc);
                break;
            }
            case REQ_FIELD: {
                kaos_ask_answer(&s_nfc_doors[NFC_ASK_FIELD], who, &r->fld->super, AO_Nfc);
                break;
            }
            default: {
                break;
            }
        }
    }
    r->kind = REQ_NONE;
    r->done = NULL;
    r->ack  = NULL;
    r->who  = NULL;
}

/**
 * @brief Fill an NfcNdefRspEvt.
 *
 * @param ev   The answer.
 * @param rc   What the exchange came to.
 * @param tag  The tag, or NULL when the window closed with nothing.
 * @param text What was read, or NULL for a write.
 * @param uri  A read's record was a URI.
 */
static void rsp_pack(NfcNdefRspEvt * const ev, kode_status_t rc,
                     const knfc_tag_t *tag, const char *text, bool uri)
{
    /* Every field by hand, NOT a memset over the whole event: zeroing QEvt's
     * header makes a pooled event look static, it is never returned, and the
     * pool drains. */
    ev->rc       = (uint32_t)rc;
    ev->have_tag = NULL != tag;
    (void)memset(ev->uid, 0, sizeof(ev->uid));
    ev->uid_len  = 0U;
    ev->atqa     = 0U;
    ev->sak      = 0U;
    ev->type     = 0U;
    ev->len      = 0U;
    ev->uri      = uri;
    ev->text[0]  = '\0';
    if ( NULL != tag )
    {
        (void)memcpy(ev->uid, tag->uid, KQP_NFC_UID_MAX);
        ev->uid_len = tag->uid_len;
        ev->atqa    = tag->atqa;
        ev->sak     = tag->sak;
        ev->type    = (uint8_t)tag->type;
    }
    if ( NULL != text )
    {
        (void)snprintf(ev->text, sizeof(ev->text), "%s", text);
        ev->len = (uint16_t)strlen(ev->text);
    }
}

/**
 * @brief Post an NfcNdefRspEvt to an Active Object that asked for an exchange.
 *
 * The recipient is named by the caller rather than read from @c req.who, so a
 * refusal answers its own requester without touching the request in flight,
 * whose @c who may be a listen window's @c to.
 *
 * @param me   The AO.
 * @param to   Where it goes. NULL posts nothing: a blocked task was asking.
 * @param rc   What the exchange came to.
 * @param tag  The tag, or NULL when the window closed with nothing.
 * @param text What was read, or NULL for a write.
 */
static void rsp_post(Nfc * const me, QActive *to, kode_status_t rc,
                     const knfc_tag_t *tag, const char *text)
{
    if ( NULL == to )
    {
        return;
    }
    NfcNdefRspEvt *ev = Q_NEW_X(NfcNdefRspEvt, 0U, NFC_NDEF_RSP_SIG);
    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; an NFC answer is lost");
        return;
    }
    rsp_pack(ev, rc, tag, text, false);
    QACTIVE_POST(to, &ev->super, &me->super);
}

/**
 * @brief End the request with nothing: the window closed.
 *
 * @param me The AO.
 */
static void req_empty(Nfc * const me)
{
    Req * const r = &me->req;

    if ( REQ_TAG == r->kind )
    {
        if ( NULL != r->rc )
        {
            *r->rc = (uint32_t)KODE_TIMEOUT;
        }
        if ( NULL != r->ask )
        {
            read_pack(r->rd, KODE_TIMEOUT, NULL);
        }
    }
    if ( REQ_NDEF == r->kind )
    {
        if ( NULL != r->rc )
        {
            *r->rc = (uint32_t)KODE_TIMEOUT;
        }
        rsp_post(me, r->who, KODE_TIMEOUT, NULL, NULL);
        if ( NULL != r->ask )
        {
            rsp_pack(r->nd, KODE_TIMEOUT, NULL, NULL, false);
        }
    }
    if ( REQ_DUMP == r->kind )
    {
        dump_refuse(me, me->dump_who);
        me->dump_who = NULL;
        me->windows += 1U;
        r->kind = REQ_NONE;
        return;
    }
    if ( REQ_SWEEP == r->kind )
    {
        NfcKeysRspEvt *ev = Q_NEW_X(NfcKeysRspEvt, 0U, NFC_KEYS_RSP_SIG);
        if ( NULL != ev )
        {
            (void)memset(ev, 0, sizeof(*ev));
            ev->super.sig = (QSignal)NFC_KEYS_RSP_SIG;
            ev->block     = me->sweep_block;
            if ( NULL != me->sweep_who )
            {
                QACTIVE_POST(me->sweep_who, &ev->super, &me->super);
            }
        }
        me->sweep_who = NULL;
        me->windows  += 1U;
        r->kind = REQ_NONE;
        return;
    }
    me->windows += 1U;
    req_release(me);
}

/**
 * @brief Publish the verdict of `test component=nfc`.
 *
 * @param me The AO.
 * @param ok Whether it passed.
 */
static void row_finish(Nfc * const me, bool ok)
{
    me->row.running = false;
    me->row.attempt = 0U;
    me->row.mode    = READER_TEST_ROW;

    TestResultEvt *ev = Q_NEW_X(TestResultEvt, 0U, TEST_RESULT_SIG);
    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; the NFC verdict is lost");
        return;
    }
    (void)snprintf(ev->name, sizeof(ev->name), "%s", "nfc");
    ev->ok = ok;
    ev->origin = TEST_ORIGIN_AUTOTEST;
    QACTIVE_PUBLISH(&ev->super, &me->super);
}

/**
 * @brief End the row with nothing judged: no card was held to the reader.
 *
 * @param me The AO.
 */
static void nfc_row_skip(Nfc * const me)
{
    me->row.running = false;
    me->row.attempt = 0U;
    me->row.mode    = READER_TEST_ROW;

    TestResultEvt *ev = Q_NEW_X(TestResultEvt, 0U, TEST_SKIP_SIG);
    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; the NFC row's answer is lost");
        return;
    }
    (void)snprintf(ev->name, sizeof(ev->name), "%s", "nfc");
    ev->ok = false;
    ev->origin = TEST_ORIGIN_AUTOTEST;
    QACTIVE_PUBLISH(&ev->super, &me->super);
}

/* ---- What a tag on the antenna leads to ---------------------------------- */

/**
 * @brief The row's second and third steps: write the serial, read it back.
 *
 * The board's serial goes onto the tag as an NDEF Text record, is read back
 * and compared: what comes back must be the string THIS board just wrote.
 * Both readings are logged before the verdict, so a failure says which half.
 *
 * @param me  The AO.
 * @param tag The tag that activated, still selected.
 */
static void row_judge(Nfc * const me, const knfc_tag_t *tag)
{
    const char *serial = khal_identity_serial();
    char        back[KHAL_ID_LEN + 1U];

    KLOG_N(TAG, "row: tag UID %02X%02X%02X%02X, SAK 0x%02X, type %u",
           (unsigned)tag->uid[0], (unsigned)tag->uid[1],
           (unsigned)tag->uid[2], (unsigned)tag->uid[3],
           (unsigned)tag->sak, (unsigned)tag->type);

    /* Write the serial only to a tag with nothing to lose: no NDEF message, or
     * one that begins with the serial prefix every board shares. Never
     * overwrite a customer's tag. Any other tag, or one kodeOS cannot read,
     * takes the read-twice fallback below. Only a write the tag refuses falls
     * back; a write that does not read back fails the row. */
    bool may_write = false;
    if ( (NULL != serial) && ('\0' != serial[0]) )
    {
        char existing[KHAL_ID_LEN + 1U];
        (void)memset(existing, 0, sizeof(existing));
        kode_status_t rd0 = knfc_tag_ndef_read(tag, existing, sizeof(existing),
                                               NULL, NULL);
        if ( KODE_ERROR == rd0 )
        {
            may_write = true;
        }
        else if ( (KODE_OK == rd0) && (strncmp(existing, serial, 4U) == 0) )
        {
            may_write = true;
        }
        else
        {
            KLOG_W(TAG, "row: the tag already holds content that is not this "
                        "fixture's - leaving it untouched, proving the antenna "
                        "by reading it instead");
        }
    }
    if ( may_write )
    {
        kode_status_t wr = knfc_tag_ndef_write(tag, serial, false);
        if ( KODE_OK == wr )
        {
            (void)memset(back, 0, sizeof(back));
            kode_status_t rd = knfc_tag_ndef_read(tag, back, sizeof(back),
                                                  NULL, NULL);
            KLOG_N(TAG, "row: wrote and read back \"%s\" -> %d", back,
                   (int)rd);
            bool ok = (KODE_OK == rd) && (strcmp(back, serial) == 0);
            if ( !ok )
            {
                KLOG_E(TAG, "row: wrote \"%s\" and read back \"%s\" - the "
                            "write path works and the read path does not",
                       serial, back);
            }
            me->req.kind = REQ_NONE;
            row_finish(me, ok);
            return;
        }
        KLOG_N(TAG, "row: the tag is not writable by kodeOS (a locked or "
                    "secret-keyed card); proving the antenna by reading it "
                    "instead");
    }

    /* The fallback, for a tag kodeOS cannot write: reading the SAME UID a
     * second time proves a stable link, not one lucky frame. Any card of the
     * right UID passes, so the write-back above is preferred. */
    knfc_tag_t again;
    kode_status_t rc = knfc_tag_identify(&again);
    bool same = (KODE_OK == rc) && (again.uid_len == tag->uid_len)
              && (memcmp(again.uid, tag->uid, tag->uid_len) == 0);
    if ( same )
    {
        KLOG_N(TAG, "row: read the tag twice, UID stable - the antenna holds");
    }
    else
    {
        KLOG_E(TAG, "row: could not read the tag a second time - the link is "
                    "marginal");
    }
    me->req.kind = REQ_NONE;
    row_finish(me, same);
}

/**
 * @brief A tag activated: publish it, serve the request, drop the field.
 *
 * @param me The AO.
 */
static void tag_arrived(Nfc * const me)
{
    knfc_tag_t tag;

    if ( knfc_tag_identify(&tag) != KODE_OK )
    {
        /* Activated and then gone: the window closing, not an error. The row
         * retries instead of req_empty(), which would leave it running with
         * no verdict. */
        field_down(me);
        deadline_stop(me);
        if ( REQ_ROW == me->req.kind )
        {
            row_retry(me);
        }
        else
        {
            req_empty(me);
        }
        return;
    }

    me->tags     += 1U;
    me->last.have  = true;
    me->last.tag   = tag;
    me->last.at_ms = khal_time_ms();

    NfcTagEvt *ev = Q_NEW_X(NfcTagEvt, 0U, NFC_TAG_SIG);
    if ( NULL != ev )
    {
        (void)memcpy(ev->uid, tag.uid, KQP_NFC_UID_MAX);
        ev->uid_len = tag.uid_len;
        ev->atqa    = tag.atqa;
        ev->sak     = tag.sak;
        ev->type    = (uint8_t)tag.type;
        QACTIVE_PUBLISH(&ev->super, &me->super);
    }

    Req * const r = &me->req;
    switch ( r->kind )
    {
        case REQ_TAG: {
            if ( NULL != r->out )
            {
                (void)memcpy(r->out, &tag, sizeof(tag));
            }
            if ( NULL != r->rc )
            {
                *r->rc = (uint32_t)KODE_OK;
            }
            if ( NULL != r->ask )
            {
                read_pack(r->rd, KODE_OK, &tag);
            }
            break;
        }
        case REQ_NDEF: {
            kode_status_t rc = KODE_DENIED;
            if ( r->write )
            {
                rc = knfc_tag_ndef_write(&tag, r->text, r->uri);
            }
            else
            {
                knfc_ndef_kind_t kind = KNFC_NDEF_TEXT;

                rc = knfc_tag_ndef_read(&tag, r->text, r->cap, r->n, &kind);
                if ( (KODE_OK == rc) && (NULL != r->is_uri) )
                {
                    *r->is_uri = (KNFC_NDEF_URI == kind);
                }
            }
            if ( KODE_OK != rc )
            {
                me->errors += 1U;
            }
            if ( NULL != r->rc )
            {
                *r->rc = (uint32_t)rc;
            }
            if ( r->write )
            {
                rsp_post(me, r->who, rc, &tag, NULL);
                if ( NULL != r->ask )
                {
                    rsp_pack(r->nd, rc, &tag, NULL, false);
                }
            }
            else
            {
                /* Only a read that succeeded has a message: a failed one
                 * carries no fragment the driver may have left. */
                const char *got = NULL;
                bool        uri = false;

                if ( KODE_OK == rc )
                {
                    got = r->text;
                    uri = me->ask_uri;
                }
                rsp_post(me, r->who, rc, &tag, got);
                if ( NULL != r->ask )
                {
                    rsp_pack(r->nd, rc, &tag, got, uri);
                }
            }
            break;
        }
        case REQ_ROW: {
            if ( READER_TEST_ROW == me->row.mode )
            {
                row_judge(me, &tag);
            }
            else
            {
                /* The owner's rows: a tag that answers proves the antenna,
                 * and it is somebody's card - it is named and left alone. */
                KLOG_N(TAG, "row: tag UID %02X%02X%02X%02X answered; nothing "
                            "is read from it or written to it",
                       (unsigned)tag.uid[0], (unsigned)tag.uid[1],
                       (unsigned)tag.uid[2], (unsigned)tag.uid[3]);
                me->req.kind = REQ_NONE;
                row_finish(me, true);
            }
            break;
        }
        case REQ_SWEEP: {
            me->sweep_tag = tag;
            me->sweep_idx = 0U;
            me->sweeping  = true;
            KLOG_N(TAG, "sweep: card UID %02X%02X%02X%02X, trying %u keys on "
                        "block %u", (unsigned)tag.uid[0], (unsigned)tag.uid[1],
                   (unsigned)tag.uid[2], (unsigned)tag.uid[3],
                   (unsigned)knfc_mfc_dict_size(), (unsigned)me->sweep_block);
            /* The window stays up and the tick keeps coming, but from here it
             * drives sweep_step(), not the discovery worker. */
            tick_arm(me);
            break;
        }
        case REQ_DUMP: {
            me->dump_tag     = tag;
            me->dump_sector  = 0U;
            me->dump_sectors = knfc_mfc_sectors(tag.sak);
            me->dump_read    = 0U;
            me->dump_blocks  = 0U;
            me->dumping      = true;
            KLOG_N(TAG, "dump: card UID %02X%02X%02X%02X, %u sectors",
                   (unsigned)tag.uid[0], (unsigned)tag.uid[1],
                   (unsigned)tag.uid[2], (unsigned)tag.uid[3],
                   (unsigned)me->dump_sectors);
            tick_arm(me);
            break;
        }
        default: {
            break;
        }
    }

    if ( me->sweeping || me->dumping )
    {
        /* The sweep and the dump both run on from here across many ticks;
         * the field stays up and their own _finish() ends them. */
        return;
    }

    field_down(me);
    deadline_stop(me);

    if ( REQ_NONE == r->kind )
    {
        /* row_judge() has already published its verdict and cleared the
         * request; there is nobody left to release. */
        return;
    }
    req_release(me);
}

/**
 * @brief Turn the field on and start looking, for @p ms.
 *
 * @param me The AO.
 * @param ms How long the window is.
 * @return KODE_OK when the field is up and the pump is armed.
 */
static kode_status_t window_open(Nfc * const me, uint32_t ms)
{
    kode_status_t rc = kdrv_st25r3916_discover_start();

    if ( KODE_OK != rc )
    {
        return rc;
    }
    me->field = true;
    tick_arm(me);
    deadline_arm(me, ms);
    return KODE_OK;
}

/* ---- Listening ----------------------------------------------------------- */

/**
 * @brief Whether a listen window holds the reader.
 *
 * The request's kind is the one record of it: REQ_FIELD is taken before the
 * window opens and released as it closes, so no second flag can disagree.
 *
 * @param me The AO.
 * @return true while a listen window is open.
 */
static bool nfc_listening(const Nfc * const me)
{
    return REQ_FIELD == me->req.kind;
}

/**
 * @brief Open a listen window, take its first burst, and pace it for @p ms.
 *
 * The first burst is taken here, so a window shorter than one tick still
 * measures. Every refusal here is the window failing, never the reader being
 * busy (req_take() turned that away), so all are KODE_ERROR to the requester;
 * the driver's answer is logged.
 *
 * @param me The AO.
 * @param ms How long the window is.
 * @return KODE_OK when the window is open and paced; KODE_ERROR when it would
 *         not open or its first burst failed, with the window already closed
 *         and the reader at rest.
 */
static kode_status_t listen_open(Nfc * const me, uint32_t ms)
{
    kode_status_t rc = kdrv_st25r3916_listen_begin();

    /* A burst that fails closes the window inside the driver, so there is
     * nothing left here to end. */
    if ( KODE_OK == rc )
    {
        rc = kdrv_st25r3916_listen_step();
    }
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "listen: the window would not open (driver answered %d)",
               (int)rc);
        return KODE_ERROR;
    }
    /* The tick first: QF expires one-shots armed together newest first, so the deadline wins
     * the tick they share. */
    tick_arm(me);
    deadline_arm(me, ms);
    return KODE_OK;
}

/**
 * @brief Fill an NfcFieldRspEvt.
 *
 * @param ev The answer.
 * @param st What the request came to.
 * @param f  The window's stats, or NULL when there are none to report.
 */
static void field_rsp_pack(NfcFieldRspEvt * const ev, kode_status_t st,
                           const knfc_field_t *f)
{
    /* Every field by hand and never a memset over the event, on rsp_post()'s
     * rule: QEvt's header belongs to the pool. */
    ev->st   = (uint32_t)st;
    ev->min  = 0U;
    ev->mean = 0U;
    ev->peak = 0U;
    ev->mod  = 0U;
    ev->ext  = false;
    ev->n    = 0U;
    if ( NULL != f )
    {
        ev->min  = f->min;
        ev->mean = f->mean;
        ev->peak = f->peak;
        ev->mod  = f->mod;
        ev->ext  = f->ext;
        ev->n    = f->n;
    }
}

/**
 * @brief Post an NfcFieldRspEvt to an Active Object that asked for a window.
 *
 * @param me The AO.
 * @param to Where it goes. NULL posts nothing: a blocked task was asking.
 * @param st What the request came to.
 * @param f  The window's stats, or NULL when there are none to report.
 */
static void field_rsp_post(Nfc * const me, QActive *to, kode_status_t st,
                           const knfc_field_t *f)
{
    NfcFieldRspEvt *ev;

    if ( NULL == to )
    {
        return;
    }
    ev = Q_NEW_X(NfcFieldRspEvt, 0U, NFC_FIELD_RSP_SIG);
    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; an NFC listen answer is lost");
        return;
    }
    field_rsp_pack(ev, st, f);
    QACTIVE_POST(to, &ev->super, &me->super);
}

/**
 * @brief Close the listen window and answer its request.
 *
 * The stats are trusted only when the window ended cleanly AND the driver
 * restored its registers. The answer is KODE_OK or KODE_ERROR only: the
 * requester reads KODE_DENIED as busy and KODE_IO_ERROR as absent, and a
 * window that ran is neither.
 *
 * @param me The AO.
 * @param st KODE_OK when the deadline ended the window, KODE_ERROR when a
 *           burst did.
 */
static void listen_finish(Nfc * const me, kode_status_t st)
{
    Req * const   r  = &me->req;
    kode_status_t rc = st;
    knfc_field_t  f;

    (void)memset(&f, 0, sizeof(f));
    /* After a failed burst the driver has closed the window already and this
     * answers KODE_DENIED; st is the failure that matters then. */
    kode_status_t end = kdrv_st25r3916_listen_end(&f);
    tick_stop(me);
    deadline_stop(me);

    if ( (KODE_OK == rc) && (KODE_OK != end) )
    {
        rc = KODE_ERROR;
    }
    if ( (KODE_OK == rc) && (NULL != r->out) )
    {
        (void)memcpy(r->out, &f, sizeof(f));
    }
    /* Before req_release(), whose write of @c ack is what orders this and the
     * copy above against the requester on the other core. */
    if ( NULL != r->rc )
    {
        *r->rc = (uint32_t)rc;
    }
    if ( KODE_OK == rc )
    {
        field_rsp_post(me, r->who, rc, &f);
    }
    else
    {
        field_rsp_post(me, r->who, rc, NULL);
    }
    if ( NULL != r->ask )
    {
        if ( KODE_OK == rc )
        {
            field_rsp_pack(r->fld, rc, &f);
        }
        else
        {
            field_rsp_pack(r->fld, rc, NULL);
        }
    }
    req_release(me);
}

/* ---- Requests ------------------------------------------------------------ */

/**
 * @brief Take a request, or refuse it without disturbing anything.
 *
 * A listen window is opened with listen_open(); every other kind raises the
 * field and starts discovery with window_open().
 *
 * @param me    The AO.
 * @param kind  Which kind.
 * @param ms    The window it asked for.
 * @param done  Its semaphore. May be NULL: the row, and an Active Object
 *              answered by event.
 * @param gen   Its generation.
 * @param ack   Where to write @p gen. May be NULL.
 * @return KODE_OK when the request was taken and its window is open;
 *         KODE_DENIED while another request is in flight; otherwise why the
 *         window would not open - what the driver answered for discovery,
 *         and KODE_ERROR for a listen window (listen_open()).
 */
static kode_status_t req_take(Nfc * const me, req_kind_t kind, uint32_t ms,
                              khal_sem_t done, uint32_t gen, uint32_t *ack)
{
    kode_status_t rc = KODE_DENIED;

    if ( REQ_NONE != me->req.kind )
    {
        return KODE_DENIED;
    }
    me->req.kind = kind;
    me->req.done = done;
    me->req.gen  = gen;
    me->req.ack  = ack;

    if ( REQ_FIELD == kind )
    {
        rc = listen_open(me, ms);
    }
    else
    {
        rc = window_open(me, ms);
    }
    if ( KODE_OK != rc )
    {
        me->req.kind = REQ_NONE;
    }
    return rc;
}

/**
 * @brief Refuse a request that cannot be served, at once.
 *
 * @param done Its semaphore.
 * @param gen  Its generation.
 * @param ack  Where to write @p gen. May be NULL.
 */
static void req_refuse(khal_sem_t done, uint32_t gen, uint32_t *ack)
{
    if ( NULL != ack )
    {
        *ack = gen;
    }
    if ( NULL != done )
    {
        khal_sem_signal(done);
    }
}

/**
 * @brief Serve NFC_READ_REQ_SIG.
 *
 * @param me The AO.
 * @param q  The request.
 */
static void on_read_req(Nfc * const me, NfcReadEvt const * const q)
{
    /* One route per request: its makers set the waiter, the ask or the console, never two. */
    Q_ASSERT((NULL == q->ask) || (NULL == q->done));
    kode_status_t st = KODE_DENIED;

    if ( NULL != q->rc )
    {
        *q->rc = (uint32_t)KODE_DENIED;
    }
    if ( !me->present )
    {
        st = KODE_IO_ERROR;
    }
    else if ( 0U != q->ms )
    {
        st = req_take(me, REQ_TAG, q->ms, q->done, q->gen, q->ack);
    }
    else
    {
        /* Nobody is waiting: refused. */
    }
    if ( KODE_OK != st )
    {
        req_refuse(q->done, q->gen, q->ack);
        if ( NULL != q->ask )
        {
            read_pack(q->rsp, st, NULL);
            kaos_ask_answer(&s_nfc_doors[NFC_ASK_READ], q->ask, &q->rsp->super, AO_Nfc);
        }
        return;
    }
    me->req.out  = q->out;
    me->req.rc   = q->rc;
    me->req.ask  = q->ask;
    me->req.rd   = q->rsp;
}

/**
 * @brief Serve NFC_NDEF_REQ_SIG.
 *
 * @param me The AO.
 * @param q  The request.
 */
static void on_ndef_req(Nfc * const me, NfcNdefEvt const * const q)
{
    kode_status_t st = KODE_DENIED;

    if ( NULL != q->rc )
    {
        *q->rc = (uint32_t)KODE_DENIED;
    }
    if ( !me->present )
    {
        st = KODE_IO_ERROR;
    }
    else if ( (0U != q->ms) && (NULL != q->text) )
    {
        st = req_take(me, REQ_NDEF, q->ms, q->done, q->gen, q->ack);
    }
    else
    {
        /* Nobody is waiting, or nothing to say it with: refused. */
    }
    if ( KODE_OK != st )
    {
        /* Refused without disturbing the request in flight. An Active Object
         * gets it as an event: it has no semaphore to be woken on. */
        rsp_post(me, q->who, KODE_DENIED, NULL, NULL);
        req_refuse(q->done, q->gen, q->ack);
        if ( NULL != q->ask )
        {
            rsp_pack(q->rsp, st, NULL, NULL, false);
            kaos_ask_answer(&s_nfc_doors[NFC_ASK_NDEF], q->ask, &q->rsp->super, AO_Nfc);
        }
        return;
    }
    me->req.who   = q->who;
    me->req.text  = q->text;
    me->req.cap   = q->cap;
    me->req.n     = q->n;
    me->req.is_uri = q->is_uri;
    me->req.write = q->write;
    me->req.uri   = q->uri;
    me->req.rc    = q->rc;
    me->req.ask   = q->ask;
    me->req.nd    = q->rsp;
    if ( NULL != q->ask )
    {
        /* The event is recycled before a tag turns up: the message lives in the AO. */
        (void)memcpy(me->ask_text, q->wtext, sizeof(me->ask_text));
        me->ask_uri     = false;
        me->req.text    = me->ask_text;
        me->req.cap     = sizeof(me->ask_text);
        me->req.is_uri  = &me->ask_uri;
    }
}

/**
 * @brief Serve NFC_FIELD_REQ_SIG: open a listen window, or refuse at once.
 *
 * KODE_IO_ERROR: no reader is fitted (looked for once, at boot). KODE_DENIED:
 * another request holds @c req.kind; it passes. KODE_ERROR: a zero @c ms, or
 * a window that would not open. A window over KODE_NFC_FIELD_MS_MAX is capped.
 * A refusal reaches an AO as an NfcFieldRspEvt and a blocked task through its
 * semaphore, with @c rc holding the same status.
 *
 * @param me The AO.
 * @param q  The request.
 */
static void on_field_req(Nfc * const me, NfcFieldEvt const * const q)
{
    kode_status_t st = KODE_ERROR;
    uint32_t      ms = q->ms;

    if ( KODE_NFC_FIELD_MS_MAX < ms )
    {
        ms = KODE_NFC_FIELD_MS_MAX;
    }
    if ( !me->present )
    {
        st = KODE_IO_ERROR;
    }
    else if ( 0U == ms )
    {
        st = KODE_ERROR;
    }
    else
    {
        st = req_take(me, REQ_FIELD, ms, q->done, q->gen, q->ack);
    }
    if ( KODE_OK != st )
    {
        if ( NULL != q->rc )
        {
            *q->rc = (uint32_t)st;
        }
        field_rsp_post(me, q->to, st, NULL);
        req_refuse(q->done, q->gen, q->ack);
        if ( NULL != q->ask )
        {
            field_rsp_pack(q->rsp, st, NULL);
            kaos_ask_answer(&s_nfc_doors[NFC_ASK_FIELD], q->ask, &q->rsp->super, AO_Nfc);
        }
        return;
    }
    me->req.out = q->out;
    me->req.rc  = q->rc;
    me->req.who = q->to;
    me->req.ask = q->ask;
    me->req.fld  = q->rsp;
}

/* ---- The Mifare dictionary sweep ------------------------------------------ */

/** @brief Keys tried per NFC_TICK. Small, so the AO step stays short. */
#define NFC_SWEEP_BATCH  8U

/**
 * @brief 'A' or 'B' for a key type, without a ternary in a log line.
 *
 * @param t The key type.
 * @return The letter.
 */
static char sweep_type_ch(knfc_mfc_key_t t)
{
    if ( KNFC_MFC_KEY_B == t )
    {
        return 'B';
    }
    return 'A';
}

/**
 * @brief Report a finished sweep and let the field down.
 *
 * @param me    The AO.
 * @param found Whether a key opened the block.
 * @param type  A or B, when found.
 * @param key   The six bytes, when found.
 */
static void sweep_finish(Nfc * const me, bool found, knfc_mfc_key_t type,
                         const uint8_t *key)
{
    NfcKeysRspEvt *ev = Q_NEW_X(NfcKeysRspEvt, 0U, NFC_KEYS_RSP_SIG);
    if ( NULL != ev )
    {
        ev->found    = found;
        ev->have_tag = true;
        ev->block    = me->sweep_block;
        ev->type     = 0U;
        if ( KNFC_MFC_KEY_B == type )
        {
            ev->type = 1U;
        }
        ev->tried = (uint32_t)me->sweep_idx;
        (void)memset(ev->key, 0, sizeof(ev->key));
        if ( found && (NULL != key) )
        {
            (void)memcpy(ev->key, key, KNFC_MFC_KEY_LEN);
        }
        if ( NULL != me->sweep_who )
        {
            QACTIVE_POST(me->sweep_who, &ev->super, &me->super);
        }
    }
    me->sweeping  = false;
    me->sweep_who = NULL;
    me->req.kind  = REQ_NONE;
    field_down(me);
    deadline_stop(me);
}

/**
 * @brief Run one batch of the sweep; called from NFC_TICK.
 *
 * @param me The AO.
 */
static void sweep_step(Nfc * const me)
{
    const uint8_t *cuid = me->sweep_tag.uid;
    if ( 4U < me->sweep_tag.uid_len )
    {
        cuid = &me->sweep_tag.uid[me->sweep_tag.uid_len - 4U];
    }

    size_t         idx  = 0U;
    knfc_mfc_key_t type = KNFC_MFC_KEY_A;
    size_t         tried = 0U;
    kode_status_t  rc = knfc_mfc_sweep(me->sweep_block, cuid, me->sweep_idx,
                                       NFC_SWEEP_BATCH, &idx, &type, &tried);
    if ( KODE_OK == rc )
    {
        KLOG_N(TAG, "sweep: block %u opens with key %02X%02X%02X%02X%02X%02X "
                    "(%c), entry %u of %u", (unsigned)me->sweep_block,
               (unsigned)KNFC_MFC_DICT[idx][0], (unsigned)KNFC_MFC_DICT[idx][1],
               (unsigned)KNFC_MFC_DICT[idx][2], (unsigned)KNFC_MFC_DICT[idx][3],
               (unsigned)KNFC_MFC_DICT[idx][4], (unsigned)KNFC_MFC_DICT[idx][5],
               sweep_type_ch(type), (unsigned)(idx + 1U),
               (unsigned)knfc_mfc_dict_size());
        me->sweep_idx = idx;
        sweep_finish(me, true, type, KNFC_MFC_DICT[idx]);
        return;
    }

    me->sweep_idx += tried;
    if ( knfc_mfc_dict_size() <= me->sweep_idx )
    {
        KLOG_W(TAG, "sweep: block %u opened by none of the %u public keys - "
                    "its key is not on any list kodeOS carries",
               (unsigned)me->sweep_block, (unsigned)knfc_mfc_dict_size());
        sweep_finish(me, false, KNFC_MFC_KEY_A, NULL);
        return;
    }

    if ( (me->sweep_idx % 200U) < NFC_SWEEP_BATCH )
    {
        KLOG_I(TAG, "sweep: %u of %u keys tried on block %u",
               (unsigned)me->sweep_idx, (unsigned)knfc_mfc_dict_size(),
               (unsigned)me->sweep_block);
    }
    tick_arm(me);
}

/* ---- The Mifare full-card dump ------------------------------------------- */

/**
 * @brief Report a finished dump and let the field down.
 *
 * @param me The AO.
 */
static void dump_finish(Nfc * const me)
{
    NfcDumpRspEvt *ev = Q_NEW_X(NfcDumpRspEvt, 0U, NFC_DUMP_RSP_SIG);
    if ( NULL != ev )
    {
        ev->have_tag      = true;
        ev->sectors_total = me->dump_sectors;
        ev->sectors_read  = me->dump_read;
        ev->blocks_read   = me->dump_blocks;
        if ( NULL != me->dump_who )
        {
            QACTIVE_POST(me->dump_who, &ev->super, &me->super);
        }
    }
    me->dumping  = false;
    me->dump_who = NULL;
    me->req.kind = REQ_NONE;
    field_down(me);
    deadline_stop(me);
}

/**
 * @brief Read one sector and stream its blocks; called from NFC_TICK.
 *
 * One sector per tick keeps the step bounded. A sector no known key opens is
 * reported and skipped rather than failing the whole dump - a card with a few
 * custom-keyed sectors still yields the rest.
 *
 * @param me The AO.
 */
static void dump_step(Nfc * const me)
{
    const uint8_t *cuid = me->dump_tag.uid;
    if ( 4U < me->dump_tag.uid_len )
    {
        cuid = &me->dump_tag.uid[me->dump_tag.uid_len - 4U];
    }

    uint8_t        sector = me->dump_sector;
    uint8_t        first  = knfc_mfc_sector_first(sector);
    uint8_t        nblk   = knfc_mfc_sector_blocks(sector);
    knfc_mfc_key_t kt     = KNFC_MFC_KEY_A;
    uint8_t        key[KNFC_MFC_KEY_LEN];

    if ( knfc_mfc_auth_any(first, cuid, &kt, key) != KODE_OK )
    {
        KLOG_W(TAG, "dump: sector %u locked - no known key", (unsigned)sector);
    }
    else
    {
        char kc = 'A';
        if ( KNFC_MFC_KEY_B == kt )
        {
            kc = 'B';
        }
        KLOG_N(TAG, "dump: sector %u key %02X%02X%02X%02X%02X%02X (%c)",
               (unsigned)sector, (unsigned)key[0], (unsigned)key[1],
               (unsigned)key[2], (unsigned)key[3], (unsigned)key[4],
               (unsigned)key[5], kc);
        me->dump_read += 1U;
        for ( uint8_t b = 0U; b < nblk; ++b )
        {
            uint8_t blk[KNFC_MFC_BLOCK_LEN];
            if ( knfc_mfc_read((uint8_t)(first + b), blk) == KODE_OK )
            {
                me->dump_blocks += 1U;
                KLOG_N(TAG, "dump: blk %u %02X%02X%02X%02X%02X%02X%02X%02X"
                            "%02X%02X%02X%02X%02X%02X%02X%02X",
                       (unsigned)(first + b),
                       (unsigned)blk[0], (unsigned)blk[1], (unsigned)blk[2],
                       (unsigned)blk[3], (unsigned)blk[4], (unsigned)blk[5],
                       (unsigned)blk[6], (unsigned)blk[7], (unsigned)blk[8],
                       (unsigned)blk[9], (unsigned)blk[10], (unsigned)blk[11],
                       (unsigned)blk[12], (unsigned)blk[13], (unsigned)blk[14],
                       (unsigned)blk[15]);
            }
            else
            {
                KLOG_W(TAG, "dump: blk %u unreadable", (unsigned)(first + b));
            }
        }
    }

    me->dump_sector += 1U;
    if ( me->dump_sector >= me->dump_sectors )
    {
        KLOG_N(TAG, "dump: done, %u of %u sectors, %u blocks",
               (unsigned)me->dump_read, (unsigned)me->dump_sectors,
               (unsigned)me->dump_blocks);
        dump_finish(me);
        return;
    }
    tick_arm(me);
}

/* ---- The production row -------------------------------------------------- */

/**
 * @brief Run `test component=nfc`.
 *
 * Step one reads the identity register without emitting: it proves the part,
 * the flex and J10. Step two energises and requires a tag: the crystal, the
 * regulator, the field and ANT2. If only step two fails, the antenna or its
 * flex is the half to replace.
 *
 * @param me The AO.
 */
static void row_start(Nfc * const me, uint8_t mode)
{
    if ( me->row.running || (REQ_NONE != me->req.kind) )
    {
        KLOG_W(TAG, "row: the reader is busy");
        row_finish(me, false);
        return;
    }

    me->row.running = true;
    me->row.attempt = 0U;
    me->row.mode    = mode;

    uint8_t identity = 0U;
    if ( kdrv_st25r3916_identity(&identity) != KODE_OK )
    {
        KLOG_E(TAG, "row: no reader answers - the Antenna PCB or its flex");
        row_finish(me, false);
        return;
    }
    KLOG_N(TAG, "row: identity 0x%02X (ic_type %u, revision %u)",
           (unsigned)identity, (unsigned)((identity >> 3) & 0x1FU),
           (unsigned)(identity & 0x07U));
    if ( KDRV_ST25R3916_IDENTITY != identity )
    {
        KLOG_E(TAG, "row: identity is not 0x%02X - wrong part or bad wiring",
               (unsigned)KDRV_ST25R3916_IDENTITY);
        row_finish(me, false);
        return;
    }

    me->row.attempt = 1U;
    if ( req_take(me, REQ_ROW, KODE_NFC_TEST_FIELD_MS, NULL, 0U, NULL)
        != KODE_OK )
    {
        KLOG_E(TAG, "row: the field would not come up");
        row_finish(me, false);
        return;
    }
    KLOG_N(TAG, "row: attempt %u of %u, present a tag",
           (unsigned)me->row.attempt, (unsigned)KODE_NFC_TEST_TRIES);
}

/**
 * @brief One row window closed with no tag: try again, or give up.
 *
 * @param me The AO.
 */
static void row_retry(Nfc * const me)
{
    me->req.kind = REQ_NONE;

    if ( READER_TEST_FIELD == me->row.mode )
    {
        /* The part named itself and its field stood for a whole window
         * with nothing in front of it. That is all this row asks. */
        KLOG_N(TAG, "row: the field held for %u ms with no tag; the reader "
                    "and its field answer", (unsigned)KODE_NFC_TEST_FIELD_MS);
        row_finish(me, true);
        return;
    }
    if ( (READER_TEST_CARD == me->row.mode)
        && (KODE_NFC_TEST_TRIES <= me->row.attempt) )
    {
        KLOG_N(TAG, "row: no card was held to the reader; nothing to judge");
        nfc_row_skip(me);
        return;
    }
    if ( KODE_NFC_TEST_TRIES <= me->row.attempt )
    {
        /* Two things look identical from here and the message says both:
         * on the production fixture the tag is always there, so this is the
         * antenna; on a bench it is usually nobody having put one down. */
        KLOG_E(TAG, "row: no tag in %u attempts - either none was presented, "
                    "or the antenna or its flex is at fault; the reader "
                    "itself answered", (unsigned)KODE_NFC_TEST_TRIES);
        row_finish(me, false);
        return;
    }
    me->row.attempt += 1U;
    if ( req_take(me, REQ_ROW, KODE_NFC_TEST_FIELD_MS, NULL, 0U, NULL)
        != KODE_OK )
    {
        row_finish(me, false);
        return;
    }
    KLOG_N(TAG, "row: attempt %u of %u, present a tag",
           (unsigned)me->row.attempt, (unsigned)KODE_NFC_TEST_TRIES);
}

/**
 * @brief Post an empty NfcDumpRspEvt - a refusal or a window with no card.
 *
 * @param who Where it goes.
 */
static void dump_refuse(Nfc * const me, QActive *who)
{
    NfcDumpRspEvt *ev = Q_NEW_X(NfcDumpRspEvt, 0U, NFC_DUMP_RSP_SIG);
    if ( NULL == ev )
    {
        return;
    }
    ev->have_tag      = false;
    ev->sectors_total = 0U;
    ev->sectors_read  = 0U;
    ev->blocks_read   = 0U;
    if ( NULL != who )
    {
        QACTIVE_POST(who, &ev->super, &me->super);
    }
}

/**
 * @brief Begin a Mifare full-card dump.
 *
 * Opens the field and discovers a card; tag_arrived() sees REQ_DUMP and starts
 * the per-sector reads. A modest deadline covers a start with no card in range.
 *
 * @param me  The AO.
 * @param who Where the final answer goes.
 */
static void dump_start(Nfc * const me, QActive *who)
{
    if ( !me->present || (REQ_NONE != me->req.kind) || me->sweeping
        || me->dumping )
    {
        dump_refuse(me, who);
        return;
    }
    me->req.kind = REQ_DUMP;
    me->dump_who = who;
    me->dumping  = false;

    if ( kdrv_st25r3916_discover_start() != KODE_OK )
    {
        me->req.kind = REQ_NONE;
        dump_refuse(me, who);
        return;
    }
    me->field = true;
    deadline_arm(me, KODE_NFC_TEST_FIELD_MS);
    tick_arm(me);
}

/**
 * @brief Begin a Mifare dictionary sweep on @p block.
 *
 * Opens the field and discovers a card; tag_arrived() sees REQ_SWEEP and
 * starts the batches. No deadline is armed - a full sweep is minutes, and it
 * ends by finding a key or exhausting the dictionary, not by a timer.
 *
 * @param me    The AO.
 * @param who   Where the final answer goes.
 * @param block The block whose sector to open.
 */
static void keys_start(Nfc * const me, QActive *who, uint8_t block)
{
    if ( !me->present || (REQ_NONE != me->req.kind) || me->sweeping )
    {
        NfcKeysRspEvt *ev = Q_NEW_X(NfcKeysRspEvt, 0U, NFC_KEYS_RSP_SIG);
        if ( NULL != ev )
        {
            (void)memset(ev, 0, sizeof(*ev));
            ev->super.sig = (QSignal)NFC_KEYS_RSP_SIG;
            ev->block     = block;
            if ( NULL != who )
            {
                QACTIVE_POST(who, &ev->super, &me->super);
            }
        }
        return;
    }

    me->req.kind    = REQ_SWEEP;
    me->sweep_block = block;
    me->sweep_who   = who;
    me->sweeping    = false;   /* set true once a card activates */

    if ( kdrv_st25r3916_discover_start() != KODE_OK )
    {
        me->req.kind = REQ_NONE;
        NfcKeysRspEvt *ev = Q_NEW_X(NfcKeysRspEvt, 0U, NFC_KEYS_RSP_SIG);
        if ( NULL != ev )
        {
            (void)memset(ev, 0, sizeof(*ev));
            ev->super.sig = (QSignal)NFC_KEYS_RSP_SIG;
            ev->block     = block;
            if ( NULL != who )
            {
                QACTIVE_POST(who, &ev->super, &me->super);
            }
        }
        return;
    }
    me->field = true;
    /* One discovery window to find the card; after that, sweep_step drives.
     * A modest deadline so a sweep started with no card in range does not
     * sit with the field up forever. */
    deadline_arm(me, KODE_NFC_TEST_FIELD_MS);
    tick_arm(me);
}

/* ---- The FCC certification carrier (cert build only) --------------------- */

#if CONFIG_KODEOS_NFC_CERT

/**
 * @brief Hand a certification request its outcome and let its caller go.
 *
 * @c ack is written before @c done is signalled, so a caller woken by a token
 * it did not earn reads a generation not its own and keeps waiting.
 *
 * @param q  The request.
 * @param rc The outcome.
 */
static void cert_answer(NfcCertEvt const * const q, kode_status_t rc)
{
    /* One route per request: its makers set the waiter, the ask or the console, never two. */
    Q_ASSERT((NULL == q->ask) || (NULL == q->done));
    if ( NULL != q->rc )
    {
        *q->rc = (uint32_t)rc;
    }
    if ( NULL != q->ack )
    {
        *q->ack = q->gen;
    }
    if ( NULL != q->done )
    {
        khal_sem_signal(q->done);
    }
    if ( NULL != q->ask )
    {
        q->rsp->rc = (uint8_t)rc;
        kaos_ask_answer(&s_nfc_doors[NFC_ASK_CERT], q->ask, &q->rsp->super, AO_Nfc);
    }
}

/**
 * @brief Raise, switch or drop the held maximum-drive field (NFC_CERT_REQ_SIG).
 *
 * No window and no tick: held at maximum drive until a stop, for a chamber
 * measurement. Held as REQ_CERT, so other requests are refused while it is up.
 * The outcome is answered in this step, so the cert app learns whether the
 * field came up. Every mode, a stop included, is refused busy while another
 * request holds the reader: a stop would leave that request unanswered.
 *
 * @param me The AO.
 * @param q  The request: its @c mode and its answer channel.
 */
static void cert_serve(Nfc * const me, NfcCertEvt const * const q)
{
    uint8_t mode = q->mode;

    if ( ((REQ_NONE != me->req.kind) && (REQ_CERT != me->req.kind))
        || me->sweeping || me->dumping )
    {
        if ( 0U == mode )
        {
            KLOG_W(TAG, "certification carrier off refused: the reader is busy");
        }
        else
        {
            KLOG_W(TAG, "certification carrier refused: the reader is busy");
        }
        cert_answer(q, KODE_DENIED);
        return;
    }

    if ( 0U == mode )
    {
        /* Only a held certification carrier, or nothing, is left to drop.
         * Off is reliable, present reader or not. */
        field_down(me);
        me->req.kind = REQ_NONE;
        KLOG_N(TAG, "certification carrier off");
        cert_answer(q, KODE_OK);
        return;
    }

    if ( !me->present )
    {
        /* No reader on this board: the field cannot come up, and the screen
         * must say so rather than paint a green ON AIR over nothing. */
        KLOG_W(TAG, "certification carrier refused: no NFC reader on this board");
        cert_answer(q, KODE_ERROR);
        return;
    }

    /* The driver takes any prior carrier down itself before it raises the next
     * one - joining a running modulation and handing the drive strength back -
     * so switching carrier<->modulated needs no field_down() here. */
    bool modulated = (2U == mode);
    if ( kdrv_st25r3916_cert_carrier(modulated) != KODE_OK )
    {
        field_down(me);
        me->req.kind = REQ_NONE;
        KLOG_E(TAG, "the certification carrier would not come up");
        cert_answer(q, KODE_ERROR);
        return;
    }
    me->req.kind = REQ_CERT;
    me->field    = true;
    cert_answer(q, KODE_OK);
}

#endif /* CONFIG_KODEOS_NFC_CERT */

/* ---- Emulation: a held window the driver's worker actually runs ----------- *
 * The driver owns the listen worker and the RF timing (a Type-2 READ inside
 * ~87 us). AO_Nfc arms it, samples its snapshot once a tick, and publishes each
 * edge: a field, a select, a READ served, the stop. Held until a stop, no deadline.
 */

/**
 * @brief Publish one turn of an emulation window.
 *
 * @param me    The AO.
 * @param kind  nfc_emu_kind_t.
 * @param reads READs served so far.
 */
static void emu_publish(Nfc * const me, uint8_t kind, uint32_t reads)
{
    NfcEmuEvt *ev = Q_NEW_X(NfcEmuEvt, 0U, NFC_EMU_SIG);

    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; an emulation edge is lost");
        return;
    }
    ev->kind  = kind;
    ev->reads = reads;
    QACTIVE_PUBLISH(&ev->super, &me->super);
}

/**
 * @brief Answer an emulation request on whichever channel it named.
 *
 * A blocked task's semaphore, the console's posted NfcEmuRspEvt, or both.
 * @c ack is written before @c done, so a caller woken by a token it did not
 * earn keeps waiting.
 *
 * @param me    The AO.
 * @param q     The request.
 * @param rc    The outcome.
 * @param reads READs served, on a stop.
 */
static void emu_answer(Nfc * const me, NfcEmuReqEvt const * const q,
                       kode_status_t rc, uint32_t reads)
{
    /* One route per request: its makers set the waiter, the ask or the console, never two. */
    Q_ASSERT((NULL == q->ask) || ((NULL == q->done) && (NULL == q->who)));
    if ( NULL != q->rc )
    {
        *q->rc = (uint32_t)rc;
    }
    if ( NULL != q->reads )
    {
        *q->reads = reads;
    }
    if ( NULL != q->ack )
    {
        *q->ack = q->gen;
    }
    if ( NULL != q->done )
    {
        khal_sem_signal(q->done);
    }
    if ( NULL != q->ask )
    {
        q->rsp->rc    = (uint32_t)rc;
        q->rsp->reads = reads;
        kaos_ask_answer(&s_nfc_doors[NFC_ASK_EMU], q->ask, &q->rsp->super, AO_Nfc);
    }
    if ( NULL != q->who )
    {
        NfcEmuRspEvt *ev = Q_NEW_X(NfcEmuRspEvt, 0U, NFC_EMU_RSP_SIG);
        if ( NULL == ev )
        {
            KLOG_E(TAG, "out of events; an emulation answer is lost");
            return;
        }
        ev->rc    = (uint32_t)rc;
        ev->reads = reads;
        QACTIVE_POST(q->who, &ev->super, &me->super);
    }
}

/**
 * @brief Sample the driver's snapshot and publish what changed; called from
 *        NFC_TICK while REQ_EMU is held.
 *
 * @param me The AO.
 */
static void emu_sample(Nfc * const me)
{
    knfc_emu_stat_t s;

    (void)memset(&s, 0, sizeof(s));
    kdrv_st25r3916_emulate_snapshot(&s);

    if ( s.field && !me->emu_pub_field )
    {
        emu_publish(me, (uint8_t)NFC_EMU_KIND_FIELD, s.reads);
    }
    if ( s.selected && !me->emu_pub_selected )
    {
        emu_publish(me, (uint8_t)NFC_EMU_KIND_SELECTED, s.reads);
    }
    if ( s.reads > me->emu_pub_reads )
    {
        emu_publish(me, (uint8_t)NFC_EMU_KIND_READ, s.reads);
    }
    me->emu_pub_field    = s.field;
    me->emu_pub_selected = s.selected;
    me->emu_pub_reads    = s.reads;

    if ( me->emu_stopping && !s.active )
    {
        /* The worker has released the reader. Publish the total and let go. */
        emu_publish(me, (uint8_t)NFC_EMU_KIND_STOPPED, s.reads);
        me->emu_stopping = false;
        me->emu_ask      = NULL;
        me->req.kind     = REQ_NONE;
        atomic_store(&s_emu_window, false);
        tick_stop(me);
        return;
    }
    tick_arm(me);
}

/**
 * @brief Serve a stop: signal the worker and wait for it to release.
 *
 * The answer is given at once - the stop is accepted, with the READs served so
 * far - but REQ_EMU is held and the tick kept running until the worker clears
 * the window, when emu_sample() publishes STOPPED and releases it. A stop from
 * anyone but the window's opener is KODE_DENIED, the window untouched: the
 * console overrides no ask, as no asker overrides another.
 *
 * @param me The AO.
 * @param q  The request.
 */
static void on_emu_stop(Nfc * const me, NfcEmuReqEvt const * const q)
{
    if ( (REQ_EMU != me->req.kind) || (q->ask != me->emu_ask) )
    {
        emu_answer(me, q, KODE_DENIED, 0U);
        return;
    }

    knfc_emu_stat_t s;
    (void)memset(&s, 0, sizeof(s));
    kdrv_st25r3916_emulate_snapshot(&s);
    (void)kdrv_st25r3916_emulate_stop();
    /* The sampler is already armed - emulation has been running - so it will
     * see the worker release and publish STOPPED; no need to re-arm here. */
    me->emu_stopping = true;
    emu_answer(me, q, KODE_OK, s.reads);
}

/**
 * @brief Serve NFC_EMU_REQ_SIG: start or stop emulation.
 *
 * Start refuses with KODE_ERROR when no reader is fitted and KODE_DENIED while
 * another request holds the reader; otherwise the driver's own status, which
 * is KODE_OK when the window is up. A held window is kept as REQ_EMU so the
 * one-request-at-a-time rule turns away everything else until it stops.
 *
 * @param me The AO.
 * @param q  The request.
 */
static void on_emu_req(Nfc * const me, NfcEmuReqEvt const * const q)
{
    if ( 0U == q->on )
    {
        on_emu_stop(me, q);
        return;
    }
    if ( !me->present )
    {
        emu_answer(me, q, KODE_ERROR, 0U);
        return;
    }
    if ( REQ_NONE != me->req.kind )
    {
        emu_answer(me, q, KODE_DENIED, 0U);
        return;
    }
    if ( NULL == q->card )
    {
        emu_answer(me, q, KODE_ERROR, 0U);
        return;
    }

    me->req.kind = REQ_EMU;
    me->emu_ask  = q->ask;
    const knfc_emu_card_t *card = (const knfc_emu_card_t *)q->card;
    knfc_emu_card_t        own;
    if ( NULL != q->ask )
    {
        /* An ask carries a payload NfcCard of the driver's layout: copied, not
         * read through the driver's type. */
        (void)memcpy(&own, &q->copy, sizeof(own));
        card = &own;
    }
    kode_status_t rc = kdrv_st25r3916_emulate_start(card);
    if ( KODE_OK != rc )
    {
        me->req.kind = REQ_NONE;
        me->emu_ask  = NULL;
        emu_answer(me, q, rc, 0U);
        return;
    }

    me->emu_stopping     = false;
    me->emu_pub_field    = false;
    me->emu_pub_selected = false;
    me->emu_pub_reads    = 0U;
    atomic_store(&s_emu_window, true);
    tick_arm(me);
    emu_answer(me, q, KODE_OK, 0U);
}

/* ---- The console's question ---------------------------------------------- */

/**
 * @brief Answer NFC_INFO_REQ_SIG with everything `nfc` prints.
 *
 * @param me  The AO.
 * @param who Where the answer goes.
 */
static void info_serve(Nfc * const me, QActive *who)
{
    if ( NULL == who )
    {
        return;
    }
    NfcInfoEvt *ev = Q_NEW_X(NfcInfoEvt, 0U, NFC_INFO_RSP_SIG);
    if ( NULL == ev )
    {
        return;
    }

    knfc_emu_stat_t emu;
    (void)memset(&emu, 0, sizeof(emu));
    kdrv_st25r3916_emulate_snapshot(&emu);

    ev->present   = me->present;
    ev->busy      = REQ_NONE != me->req.kind;
    ev->field     = me->field;
    ev->emulating = emu.active;
    ev->emu_reads = emu.reads;
    ev->identity  = me->identity;
    ev->have_last = me->last.have;
    ev->tags      = me->tags;
    ev->windows   = me->windows;
    ev->errors    = me->errors;
    ev->age_ms    = 0U;
    (void)memset(ev->uid, 0, sizeof(ev->uid));
    ev->uid_len   = 0U;
    ev->atqa      = 0U;
    ev->sak       = 0U;
    ev->type      = 0U;
    if ( me->last.have )
    {
        (void)memcpy(ev->uid, me->last.tag.uid, KQP_NFC_UID_MAX);
        ev->uid_len = me->last.tag.uid_len;
        ev->atqa    = me->last.tag.atqa;
        ev->sak     = me->last.tag.sak;
        ev->type    = (uint8_t)me->last.tag.type;
        ev->age_ms  = khal_time_ms() - me->last.at_ms;
    }
    QACTIVE_POST(who, &ev->super, &me->super);
}

/* ---- The AO -------------------------------------------------------------- */

void Nfc_ctor(void)
{
    Nfc * const me = &l_nfc;

    QActive_ctor(&me->super, Q_STATE_CAST(&Nfc_initial));
    QTimeEvt_ctorX(&me->tick, &me->super, NFC_TICK_SIG, 0U);
    QTimeEvt_ctorX(&me->deadline, &me->super, NFC_DEADLINE_SIG, 0U);
    me->tick_stale = 0U;
    me->dl_stale   = 0U;
    me->tick_live  = false;
    me->dl_live    = false;
    me->present    = false;
    me->field      = false;
    me->identity   = 0U;
    me->tags       = 0U;
    me->windows    = 0U;
    me->errors     = 0U;
    (void)memset(&me->req, 0, sizeof(me->req));
    me->row.running = false;
    me->row.attempt = 0U;
    me->last.have   = false;
    me->last.at_ms  = 0U;
    me->sweeping    = false;
    me->dumping     = false;
    me->dump_who    = NULL;
    me->emu_stopping     = false;
    me->emu_ask          = NULL;
    me->emu_pub_field    = false;
    me->emu_pub_selected = false;
    me->emu_pub_reads    = 0U;
    atomic_store(&s_emu_window, false);
#if CONFIG_KODE_LOWPOWER
    me->lp_who           = KODE_LOWPOWER_WHO_NONE;
#endif
}

AO_STORAGE(nfc, KQP_AO_QUEUE_LEN, KQP_AO_STACK_LARGE);

#if CONFIG_KODEOS_AUTOTEST
/* ---- The self-test rows: registered by kaos_nfc_register() ---------------- */

/** @brief The NFC row: AO_Nfc runs it and answers TEST_RESULT_SIG. */
static bool nfc_row_nfc(void)
{
    nfc_test_request();
    return false;
}

/** @brief The reader with nothing in front of it: the part names itself and
 *         its field stands for one discovery window. A tag that happens to
 *         be there is activated and left alone: nothing is read from it or
 *         written to it. */
static bool nfc_row_field(void)
{
    nfc_test_request_mode(READER_TEST_FIELD);
    return false;
}

/** @brief A card held to the back: any tag that answers is a pass, and none
 *         is nothing to judge. Detection only. */
static bool nfc_row_card(void)
{
    nfc_test_request_mode(READER_TEST_CARD);
    return false;
}
#endif

void kaos_nfc_register(void)
{
#if CONFIG_KODEOS_AUTOTEST
    static const krow_t NFC_ROWS[] = {
        { "nfc", AUTOTEST_ID_NFC, nfc_row_nfc, KROW_ASYNC, AUTOTEST_G_NONE, KODE_NFC_TEST_BUDGET_MS,
          NULL, 0U, "NFC reader",
          "Reader not working",
          AUTOTEST_ORDER_NFC },
        { "nfc_field", AUTOTEST_ID_NFC_FIELD, nfc_row_field, KROW_ASYNC,
          AUTOTEST_G_NONE, KODE_NFC_TEST_BUDGET_MS,
          "nfc", 0U, "NFC reader",
          "Reader not working",
          AUTOTEST_ORDER_NFC_FIELD },
        { "nfc_card", AUTOTEST_ID_NFC_CARD, nfc_row_card, KROW_ASYNC,
          AUTOTEST_G_NONE, KODE_NFC_TEST_BUDGET_MS,
          "nfc", 0U, "NFC card read",
          "Card not read",
          AUTOTEST_ORDER_NFC_CARD },
    };

    kaos_require(krows_add(NFC_ROWS, sizeof(NFC_ROWS) / sizeof(NFC_ROWS[0])),
                 "nfc rows");
#endif
#if CONFIG_KODEOS_CONSOLE
    kaos_require(kverbs_add(&nfc_verbs), "nfc verbs");
#endif
}

void kaos_nfc_start(void)
{
    Nfc_ctor();
    kaos_nfc_register();
    AO_START(AO_Nfc, nfc, KODE_PRIO_NFC);
}

#if CONFIG_KODE_LOWPOWER
/**
 * @brief LOWPOWER_ROUND_SIG: the reader down and resting, and answer once.
 *
 * @param me The AO.
 * @param r  The round.
 */
static void nfc_round(Nfc * const me, const LowPowerRoundEvt * const r)
{
    if ( (REQ_NONE != me->req.kind) || me->sweeping || me->dumping )
    {
        lowpower_ready(me->lp_who, r->round, true, "nfc busy");
    }
    else
    {
        if ( me->present )
        {
            kdrv_st25r3916_field_off();
            me->field = false;
        }
        lowpower_ready(me->lp_who, r->round, false, NULL);
    }
}
#endif /* CONFIG_KODE_LOWPOWER */

/** @brief Initial transition: claim the reader, bring it up if fitted, go ready. */
static QState Nfc_initial(Nfc * const me, void const * const par)
{
    (void)par;

    kaos_claim(KBSP_RES_NFC, me, "AO_Nfc");
#if CONFIG_KODE_LOWPOWER
    me->lp_who = lowpower_join(&me->super, "nfc");
    if ( KODE_LOWPOWER_WHO_NONE != me->lp_who )
    {
        QActive_subscribe(&me->super, LOWPOWER_ROUND_SIG);
    }
#endif

    if ( kdrv_st25r3916_init() != KODE_OK )
    {
        /* Normal on a board whose Antenna PCB is not fitted, and on the
         * production verifier that is every board. Said once, and every
         * later request is refused rather than retried. */
        KLOG_W(TAG, "no NFC reader on this board; the field stays down");
    }
    else
    {
        me->present = true;
        (void)kdrv_st25r3916_identity(&me->identity);
        KLOG_I(TAG, "up; reader 0x%02X, field down until somebody asks",
               (unsigned)me->identity);
    }
    return Q_TRAN(&Nfc_ready);
}

/** @brief The AO's only state: every request for the reader is served here. */
static QState Nfc_ready(Nfc * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case NFC_TICK_SIG: {
            /* One drop per counted generation: this may be the expiry of an
             * arming that a newer request has already replaced. */
            if ( 0U < me->tick_stale )
            {
                me->tick_stale -= 1U;
            }
            else if ( me->sweeping )
            {
                me->tick_live = false;
                sweep_step(me);
            }
            else if ( me->dumping )
            {
                me->tick_live = false;
                dump_step(me);
            }
            else if ( nfc_listening(me) )
            {
                me->tick_live = false;
                if ( kdrv_st25r3916_listen_step() != KODE_OK )
                {
                    listen_finish(me, KODE_ERROR);
                }
                else
                {
                    tick_arm(me);
                }
            }
            else if ( REQ_EMU == me->req.kind )
            {
                me->tick_live = false;
                emu_sample(me);
            }
            else if ( me->field )
            {
                me->tick_live = false;
                kdrv_st25r3916_worker();
                if ( kdrv_st25r3916_activated() )
                {
                    tag_arrived(me);
                }
                else
                {
                    tick_arm(me);
                }
            }
            else
            {
                me->tick_live = false;
            }
            status = Q_HANDLED();
            break;
        }
        case NFC_DEADLINE_SIG: {
            if ( 0U < me->dl_stale )
            {
                me->dl_stale -= 1U;
            }
            else
            {
                me->dl_live = false;
                if ( me->sweeping || me->dumping || (REQ_EMU == me->req.kind) )
                {
                    /* None of a sweep, a dump or an emulation window sets a
                     * run-length deadline; if one somehow fires, ignore it
                     * rather than tear the operation down mid-way. */
                }
                else if ( nfc_listening(me) )
                {
                    listen_finish(me, KODE_OK);
                }
                else
                {
                    field_down(me);
                    if ( REQ_ROW == me->req.kind )
                    {
                        row_retry(me);
                    }
                    else
                    {
                        req_empty(me);
                    }
                }
            }
            status = Q_HANDLED();
            break;
        }
        case NFC_READ_REQ_SIG: {
            on_read_req(me, Q_EVT_CAST(NfcReadEvt));
            status = Q_HANDLED();
            break;
        }
        case NFC_NDEF_REQ_SIG: {
            on_ndef_req(me, Q_EVT_CAST(NfcNdefEvt));
            status = Q_HANDLED();
            break;
        }
        case NFC_FIELD_REQ_SIG: {
            on_field_req(me, Q_EVT_CAST(NfcFieldEvt));
            status = Q_HANDLED();
            break;
        }
        case NFC_EMU_REQ_SIG: {
            on_emu_req(me, Q_EVT_CAST(NfcEmuReqEvt));
            status = Q_HANDLED();
            break;
        }
        case NFC_TEST_REQ_SIG: {
            row_start(me, Q_EVT_CAST(ReaderTestReqEvt)->mode);
            status = Q_HANDLED();
            break;
        }
        case NFC_KEYS_REQ_SIG: {
            NfcKeysReqEvt const * const q = Q_EVT_CAST(NfcKeysReqEvt);
            keys_start(me, q->who, q->block);
            status = Q_HANDLED();
            break;
        }
        case NFC_DUMP_REQ_SIG: {
            NfcDumpReqEvt const * const q = Q_EVT_CAST(NfcDumpReqEvt);
            dump_start(me, q->who);
            status = Q_HANDLED();
            break;
        }
        case NFC_INFO_REQ_SIG: {
            NfcInfoReqEvt const * const q = Q_EVT_CAST(NfcInfoReqEvt);
            info_serve(me, q->who);
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            nfc_round(me, Q_EVT_CAST(LowPowerRoundEvt));
            status = Q_HANDLED();
            break;
        }
#endif
#if CONFIG_KODEOS_NFC_CERT
        case NFC_CERT_REQ_SIG: {
            NfcCertEvt const * const q = Q_EVT_CAST(NfcCertEvt);
            cert_serve(me, q);
            status = Q_HANDLED();
            break;
        }
#endif
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }
    return status;
}

/* ---- What the rest of kodeOS asks with ----------------------------------- */

void nfc_info_request(QActive *who)
{
    NfcInfoReqEvt *e = Q_NEW_X(NfcInfoReqEvt, 0U, NFC_INFO_REQ_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; a status request is dropped");
        return;
    }
    e->who = who;
    QACTIVE_POST(AO_Nfc, &e->super, NULL);
}

void nfc_test_request_mode(uint8_t mode)
{
    static const ReaderTestReqEvt ROW   = {
        QEVT_INITIALIZER(NFC_TEST_REQ_SIG), READER_TEST_ROW
    };
    static const ReaderTestReqEvt FIELD = {
        QEVT_INITIALIZER(NFC_TEST_REQ_SIG), READER_TEST_FIELD
    };
    static const ReaderTestReqEvt CARD  = {
        QEVT_INITIALIZER(NFC_TEST_REQ_SIG), READER_TEST_CARD
    };

    if ( READER_TEST_FIELD == mode )
    {
        QACTIVE_POST(AO_Nfc, &FIELD.super, NULL);
    }
    else if ( READER_TEST_CARD == mode )
    {
        QACTIVE_POST(AO_Nfc, &CARD.super, NULL);
    }
    else
    {
        QACTIVE_POST(AO_Nfc, &ROW.super, NULL);
    }
}

void nfc_test_request(void)
{
    nfc_test_request_mode(READER_TEST_ROW);
}

#if CONFIG_KODEOS_NFC_CERT
void nfc_cert_request(uint8_t mode, uint32_t *rc, khal_sem_t done,
                      uint32_t gen, uint32_t *ack)
{
    NfcCertEvt *e = Q_NEW_X(NfcCertEvt, 0U, NFC_CERT_REQ_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; a certification carrier request is dropped");
        /* The caller is blocked on @p done; release it rather than leave it to
         * wait out its whole timeout for an answer that will never come. */
        if ( NULL != rc )
        {
            *rc = (uint32_t)KODE_NO_RESOURCES;
        }
        if ( NULL != ack )
        {
            *ack = gen;
        }
        if ( NULL != done )
        {
            khal_sem_signal(done);
        }
        return;
    }
    e->mode = mode;
    e->rc   = rc;
    e->done = done;
    e->gen  = gen;
    e->ack  = ack;
    e->ask = NULL;
    e->rsp = NULL;
    QACTIVE_POST(AO_Nfc, &e->super, NULL);
}

kode_status_t nfc_cert_ask(QActive *who, uint8_t mode)
{
    kode_status_t rc = kaos_ask_take(&s_nfc_doors[NFC_ASK_CERT], who);
    StatusRspEvt *a;
    NfcCertEvt   *e;

    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(StatusRspEvt, 0U, NFC_CERT_RSP_SIG);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_nfc_doors[NFC_ASK_CERT], who, NULL);
    }
    e = Q_NEW_X(NfcCertEvt, 0U, NFC_CERT_REQ_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_nfc_doors[NFC_ASK_CERT], who, &a->super);
    }
    e->mode = mode;
    e->rc   = NULL;
    e->done = NULL;
    e->gen  = 0U;
    e->ack  = NULL;
    e->ask  = who;
    e->rsp  = a;
    return kaos_ask_post(&s_nfc_doors[NFC_ASK_CERT], who, AO_Nfc, &e->super, &a->super,
                         KQP_NFC_CMD_MARGIN);
}
#endif /* CONFIG_KODEOS_NFC_CERT */

void nfc_dump_request(QActive *who)
{
    NfcDumpReqEvt *e = Q_NEW_X(NfcDumpReqEvt, 0U, NFC_DUMP_REQ_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; a dump request is dropped");
        return;
    }
    e->who = who;
    QACTIVE_POST(AO_Nfc, &e->super, NULL);
}

void nfc_keys_request(QActive *who, uint8_t block)
{
    NfcKeysReqEvt *e = Q_NEW_X(NfcKeysReqEvt, 0U, NFC_KEYS_REQ_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; a key sweep is dropped");
        return;
    }
    e->who   = who;
    e->block = block;
    QACTIVE_POST(AO_Nfc, &e->super, NULL);
}

void nfc_ndef_request(QActive *who, char *text, size_t cap, bool write,
                      bool uri, uint32_t ms)
{
    NfcNdefEvt *e = Q_NEW_X(NfcNdefEvt, 0U, NFC_NDEF_REQ_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; an NFC request is dropped");
        return;
    }
    e->who   = who;
    e->text  = text;
    e->cap   = cap;
    e->n     = NULL;
    e->is_uri = NULL;
    e->write = write;
    e->uri   = uri;
    e->done  = NULL;
    e->ms    = ms;
    e->rc    = NULL;
    e->gen   = 0U;
    e->ack   = NULL;
    e->ask = NULL;
    e->rsp = NULL;
    QACTIVE_POST(AO_Nfc, &e->super, NULL);
}

void nfc_read_request(void *out, uint32_t *rc, khal_sem_t done, uint32_t ms,
                      uint32_t gen, uint32_t *ack)
{
    NfcReadEvt *e = Q_NEW_X(NfcReadEvt, 0U, NFC_READ_REQ_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; an NFC read is dropped");
        if ( NULL != ack )
        {
            *ack = gen;
        }
        if ( NULL != done )
        {
            khal_sem_signal(done);
        }
        return;
    }
    e->out  = out;
    e->rc   = rc;
    e->done = done;
    e->ms   = ms;
    e->gen  = gen;
    e->ack  = ack;
    e->ask = NULL;
    e->rsp = NULL;
    QACTIVE_POST(AO_Nfc, &e->super, NULL);
}

void nfc_field_request(void *out, uint32_t *rc, khal_sem_t done, uint32_t ms,
                       uint32_t gen, uint32_t *ack, QActive *to)
{
    NfcFieldEvt *e = Q_NEW_X(NfcFieldEvt, 0U, NFC_FIELD_REQ_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; an NFC listen is dropped");
        if ( NULL != rc )
        {
            *rc = (uint32_t)KODE_NO_RESOURCES;
        }
        req_refuse(done, gen, ack);
        return;
    }
    e->out  = out;
    e->rc   = rc;
    e->done = done;
    e->ms   = ms;
    e->gen  = gen;
    e->ack  = ack;
    e->to   = to;
    e->ask = NULL;
    e->rsp = NULL;
    QACTIVE_POST(AO_Nfc, &e->super, NULL);
}

void nfc_ndef_task_request(char *text, size_t cap, size_t *n, bool *is_uri,
                           bool write, bool uri, uint32_t *rc, khal_sem_t done,
                           uint32_t ms, uint32_t gen, uint32_t *ack)
{
    NfcNdefEvt *e = Q_NEW_X(NfcNdefEvt, 0U, NFC_NDEF_REQ_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; an NFC exchange is dropped");
        if ( NULL != ack )
        {
            *ack = gen;
        }
        if ( NULL != done )
        {
            khal_sem_signal(done);
        }
        return;
    }
    e->who   = NULL;
    e->text  = text;
    e->cap   = cap;
    e->n     = n;
    e->is_uri = is_uri;
    e->write = write;
    e->uri   = uri;
    e->done  = done;
    e->ms    = ms;
    e->rc    = rc;
    e->gen   = gen;
    e->ack   = ack;
    e->ask = NULL;
    e->rsp = NULL;
    QACTIVE_POST(AO_Nfc, &e->super, NULL);
}

bool nfc_is_emulating(void)
{
    return atomic_load(&s_emu_window);
}

void nfc_emulate_request(const knfc_emu_card_t *card, uint32_t *rc,
                         khal_sem_t done, uint32_t gen, uint32_t *ack,
                         QActive *to)
{
    NfcEmuReqEvt *e = Q_NEW_X(NfcEmuReqEvt, 0U, NFC_EMU_REQ_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; an emulation start is dropped");
        if ( NULL != rc )
        {
            *rc = (uint32_t)KODE_NO_RESOURCES;
        }
        req_refuse(done, gen, ack);
        return;
    }
    e->on    = 1U;
    e->card  = card;
    e->rc    = rc;
    e->reads = NULL;
    e->done  = done;
    e->gen   = gen;
    e->ack   = ack;
    e->who   = to;
    e->ask = NULL;
    e->rsp = NULL;
    QACTIVE_POST(AO_Nfc, &e->super, NULL);
}

void nfc_emulate_stop_request(uint32_t *rc, uint32_t *reads, khal_sem_t done,
                              uint32_t gen, uint32_t *ack, QActive *to)
{
    NfcEmuReqEvt *e = Q_NEW_X(NfcEmuReqEvt, 0U, NFC_EMU_REQ_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; an emulation stop is dropped");
        if ( NULL != rc )
        {
            *rc = (uint32_t)KODE_NO_RESOURCES;
        }
        if ( NULL != reads )
        {
            *reads = 0U;
        }
        req_refuse(done, gen, ack);
        return;
    }
    e->on    = 0U;
    e->card  = NULL;
    e->rc    = rc;
    e->reads = reads;
    e->done  = done;
    e->gen   = gen;
    e->ack   = ack;
    e->who   = to;
    e->ask = NULL;
    e->rsp = NULL;
    QACTIVE_POST(AO_Nfc, &e->super, NULL);
}

/* ---- kode_sys's asks ------------------------------------------------------- */

kode_status_t nfc_read_ask(QActive *who, uint32_t ms)
{
    kode_status_t  rc = kaos_ask_take(&s_nfc_doors[NFC_ASK_READ], who);
    NfcReadRspEvt *a;
    NfcReadEvt    *e;

    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(NfcReadRspEvt, 0U, NFC_READ_RSP_SIG);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_nfc_doors[NFC_ASK_READ], who, NULL);
    }
    e = Q_NEW_X(NfcReadEvt, 0U, NFC_READ_REQ_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_nfc_doors[NFC_ASK_READ], who, &a->super);
    }
    e->out  = NULL;
    e->rc   = NULL;
    e->done = NULL;
    e->ms   = ms;
    e->gen  = 0U;     /* an ask is answered by its event, not matched by a tag */
    e->ack  = NULL;
    e->ask  = who;
    e->rsp  = a;
    return kaos_ask_post(&s_nfc_doors[NFC_ASK_READ], who, AO_Nfc, &e->super, &a->super,
                         KQP_NFC_CMD_MARGIN);
}

kode_status_t nfc_ndef_ask(QActive *who, const char *text, bool uri, uint32_t ms)
{
    kode_status_t  rc;
    NfcNdefRspEvt *a;
    NfcNdefEvt    *e;

    /* No terminator within the room the request has: refused, never cut. */
    if ( (NULL != text) && (memchr(text, '\0', KQP_NFC_TEXT_MAX) == NULL) )
    {
        return KODE_DENIED;
    }
    rc = kaos_ask_take(&s_nfc_doors[NFC_ASK_NDEF], who);
    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(NfcNdefRspEvt, 0U, NFC_NDEF_RSP_SIG);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_nfc_doors[NFC_ASK_NDEF], who, NULL);
    }
    e = Q_NEW_X(NfcNdefEvt, 0U, NFC_NDEF_REQ_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_nfc_doors[NFC_ASK_NDEF], who, &a->super);
    }
    e->who    = NULL;
    e->text   = e->wtext;
    e->cap    = sizeof(e->wtext);
    e->n      = NULL;
    e->is_uri = NULL;
    e->write  = NULL != text;
    e->uri    = uri;
    e->done   = NULL;
    e->ms     = ms;
    e->rc     = NULL;
    e->gen    = 0U;
    e->ack    = NULL;
    e->ask    = who;
    e->rsp    = a;
    (void)memset(e->wtext, 0, sizeof(e->wtext));
    if ( NULL != text )
    {
        (void)memcpy(e->wtext, text, strlen(text));
    }
    return kaos_ask_post(&s_nfc_doors[NFC_ASK_NDEF], who, AO_Nfc, &e->super, &a->super,
                         KQP_NFC_CMD_MARGIN);
}

/**
 * @brief The start and the stop of an emulation ask, which differ in one field.
 * @param who  The asking AO.
 * @param card The card to present, or NULL for a stop.
 * @return As nfc_emulate_ask().
 */
static kode_status_t nfc_emu_ask(QActive *who, const NfcCard *card)
{
    kode_status_t rc = kaos_ask_take(&s_nfc_doors[NFC_ASK_EMU], who);
    NfcEmuRspEvt *a;
    NfcEmuReqEvt *e;

    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(NfcEmuRspEvt, 0U, NFC_EMU_RSP_SIG);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_nfc_doors[NFC_ASK_EMU], who, NULL);
    }
    e = Q_NEW_X(NfcEmuReqEvt, 0U, NFC_EMU_REQ_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_nfc_doors[NFC_ASK_EMU], who, &a->super);
    }
    e->on    = 0U;
    e->card  = NULL;
    e->rc    = NULL;
    e->reads = NULL;
    e->done  = NULL;
    e->gen   = 0U;
    e->ack   = NULL;
    e->who   = NULL;
    e->ask   = who;
    e->rsp   = a;
    if ( NULL != card )
    {
        e->copy = *card;
        e->on   = 1U;
        e->card = &e->copy;
    }
    return kaos_ask_post(&s_nfc_doors[NFC_ASK_EMU], who, AO_Nfc, &e->super, &a->super,
                         KQP_NFC_CMD_MARGIN);
}

kode_status_t nfc_emulate_ask(QActive *who, const NfcCard *card)
{
    if ( NULL == card )
    {
        return KODE_DENIED;
    }
    return nfc_emu_ask(who, card);
}

kode_status_t nfc_emulate_stop_ask(QActive *who)
{
    return nfc_emu_ask(who, NULL);
}

kode_status_t nfc_field_ask(QActive *who, uint32_t ms)
{
    kode_status_t   rc = kaos_ask_take(&s_nfc_doors[NFC_ASK_FIELD], who);
    NfcFieldRspEvt *a;
    NfcFieldEvt    *e;

    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(NfcFieldRspEvt, 0U, NFC_FIELD_RSP_SIG);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_nfc_doors[NFC_ASK_FIELD], who, NULL);
    }
    e = Q_NEW_X(NfcFieldEvt, 0U, NFC_FIELD_REQ_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_nfc_doors[NFC_ASK_FIELD], who, &a->super);
    }
    e->out  = NULL;
    e->rc   = NULL;
    e->done = NULL;
    e->ms   = ms;
    e->gen  = 0U;
    e->ack  = NULL;
    e->to   = NULL;
    e->ask  = who;
    e->rsp  = a;
    return kaos_ask_post(&s_nfc_doors[NFC_ASK_FIELD], who, AO_Nfc, &e->super, &a->super,
                         KQP_NFC_CMD_MARGIN);
}

