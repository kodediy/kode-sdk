/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file nfc_read_ao.c
 * @brief Reading an NFC tag from an Active Object, without waiting for it.
 */
#include "nfc_read_ao.h"

#include <stdio.h>

#define TAG "nfc_read"

/** How long AO_Nfc keeps the field up looking for a tag, per request. */
#define NR_WINDOW_MS  3000U

/** Hex digits of one UID byte. */
#define NR_HEX_DIGITS 2U

/** @brief The AO's own state, private to this file. */
typedef struct {
    QActive super;
    bool    absent;   /**< AO_Nfc answered KODE_IO_ERROR: no reader fitted. */
} NfcRead;

/* The queue and the stack, written out the way every example here does. The
 * queue holds a burst of button edges and the one answer out at a time. */
static QEvtPtr     l_queue[KQP_AO_QUEUE_LEN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static NfcRead l_ao;
QActive * const AO_NfcRead = &l_ao.super;

/* No step here waits: kode.h's kode_nfc_read_id() and kode_nfc_read() do,
 * and an AO that calls them is refused. */
static QState NfcRead_initial(NfcRead * const me, void const * const par);
static QState NfcRead_idle(NfcRead * const me, QEvt const * const e);
static QState NfcRead_busy(NfcRead * const me, QEvt const * const e);
static QState NfcRead_identifying(NfcRead * const me, QEvt const * const e);
static QState NfcRead_reading(NfcRead * const me, QEvt const * const e);

/**
 * @brief Name a tag type the way a person would.
 * @param type A KODE_SYS_NFC_TYPE_x, as the answer carries it.
 * @return A static string, never NULL.
 */
static char const *type_name(uint8_t const type)
{
    char const *name = "NFC-A";

    switch ( (uint32_t)type )
    {
        case KODE_SYS_NFC_TYPE_T2T: {
            name = "Type-2";
            break;
        }
        case KODE_SYS_NFC_TYPE_T4T: {
            name = "Type-4";
            break;
        }
        case KODE_SYS_NFC_TYPE_MIFARE: {
            name = "Mifare Classic";
            break;
        }
        default: {
            /* KODE_SYS_NFC_TYPE_UNKNOWN: nothing the reader can read. */
            break;
        }
    }
    return name;
}

/**
 * @brief Log what identifies a tag: its type, its UID, ATQA and SAK.
 *
 * Both answers that carry a tag mirror the same five fields, so both call
 * this rather than each spelling the format. kode.h's kode_nfc_read_id()
 * also names the card's model, for a task.
 *
 * @param uid     The NFCID1, @p uid_len bytes of it.
 * @param uid_len 4, 7 or 10.
 * @param atqa    SENS_RES.
 * @param sak     SEL_RES.
 * @param type    A KODE_SYS_NFC_TYPE_x.
 */
static void log_tag(uint8_t const uid[KODE_SYS_NFC_UID_MAX],
                    uint8_t const uid_len, uint16_t const atqa,
                    uint8_t const sak, uint8_t const type)
{
    char     hex[(KODE_SYS_NFC_UID_MAX * NR_HEX_DIGITS) + 1U];
    uint8_t  n = uid_len;
    uint8_t  i;

    if ( (uint8_t)KODE_SYS_NFC_UID_MAX < n )
    {
        n = (uint8_t)KODE_SYS_NFC_UID_MAX;
    }
    for ( i = 0U; i < n; ++i )
    {
        (void)snprintf(&hex[(size_t)i * NR_HEX_DIGITS], NR_HEX_DIGITS + 1U,
                       "%02X", (unsigned)uid[i]);
    }
    hex[(size_t)n * NR_HEX_DIGITS] = '\0';

    KLOG_I(TAG, "tag: %s, UID %s, ATQA 0x%04X SAK 0x%02X",
           type_name(type), hex, (unsigned)atqa, (unsigned)sak);
}

/**
 * @brief Say what an NDEF exchange came to, for the answer that carries a tag.
 *
 * With a tag there, a failure is one of the KODE_SYS_NFC_NDEF_x statuses.
 *
 * @param a The answer to kode_sys_nfc_ndef_read().
 */
static void log_ndef(kode_sys_nfc_ndef_answer_t const * const a)
{
    log_tag(a->uid, a->uid_len, a->atqa, a->sak, a->type);

    switch ( (kode_status_t)a->rc )
    {
        case KODE_OK: {
            /* The first URI or Text record the tag carries, as text: a URI
             * with its prefix expanded, a Text record as its text. */
            KLOG_I(TAG, "ndef: \"%s\" (%u bytes)", a->text, (unsigned)a->len);
            break;
        }
        case KODE_SYS_NFC_NDEF_NO_MESSAGE: {
            KLOG_I(TAG, "ndef: the tag opened but carries no message - "
                        "`nfc_write text=\"hello\"` in the console puts one "
                        "on it");
            break;
        }
        case KODE_SYS_NFC_NDEF_LOCKED: {
            KLOG_I(TAG, "ndef: refused - a Mifare Classic opens only with a "
                        "key kodeOS knows");
            break;
        }
        case KODE_SYS_NFC_NDEF_LOST: {
            KLOG_I(TAG, "ndef: the tag stopped answering - hold it still");
            break;
        }
        case KODE_SYS_NFC_NDEF_TOO_LONG: {
            KLOG_I(TAG, "ndef: the message is longer than the %u characters "
                        "an answer carries",
                   (unsigned)(KODE_SYS_NFC_TEXT_MAX - 1U));
            break;
        }
        default: {
            KLOG_I(TAG, "ndef: the exchange failed (status %d)", (int)a->rc);
            break;
        }
    }
}

/**
 * @brief Note an answer that says no reader is fitted.
 * @param me The AO.
 * @param rc The answer's status.
 * @return true when @p rc is KODE_IO_ERROR, which this logs once.
 */
static bool no_reader(NfcRead * const me, uint32_t const rc)
{
    bool const none = ((uint32_t)KODE_IO_ERROR == rc);

    if ( none )
    {
        me->absent = true;
        KLOG_W(TAG, "no reader fitted - this board has no Antenna PCB, so "
                    "there is nothing to read a tag with");
    }
    return none;
}

void NfcRead_ctor(void)
{
    NfcRead * const me = &l_ao;
    QActive_ctor(&me->super, Q_STATE_CAST(&NfcRead_initial));
    me->absent = false;
}

void NfcRead_start(void)
{
    QActive_setAttr(AO_NfcRead, TASK_NAME_ATTR, "nfc_read");
    QACTIVE_START(AO_NfcRead, KODE_PRIO_USER_LOW,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
}

/**
 * @brief The initial transition: subscribe to the buttons, then wait.
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to NfcRead_idle.
 */
static QState NfcRead_initial(NfcRead * const me, void const * const par)
{
    (void)par;

    /* Only the buttons: the answers are posted to the AO that asked, and a
     * subscriber to them would get the console's answers too. */
    QActive_subscribe(&me->super, BUTTON_EVT_SIG);

    KLOG_I(TAG, "up; the top button identifies a tag, the bottom one reads "
                "its NDEF message");
    return Q_TRAN(&NfcRead_idle);
}

/**
 * @brief Nothing asked: a press starts a request.
 *
 * The request is posted here and its answer is waited for in the next state,
 * so the transition is the only thing that says a request is out. A request
 * refused at once posted nothing, so there is nothing to wait for.
 *
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED(), Q_TRAN() to the state that waits, or Q_SUPER().
 */
static QState NfcRead_idle(NfcRead * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case BUTTON_EVT_SIG: {
            ButtonEvt const * const be = (ButtonEvt const *)e;
            /* id is a button's number: KODE_SYS_BUTTON_x in kode_sys.h. */
            bool const top =
                be->pressed && (KODE_SYS_BUTTON_TOP == (uint32_t)be->id);
            bool const bot =
                be->pressed && (KODE_SYS_BUTTON_BOT == (uint32_t)be->id);
            kode_status_t rc = KODE_OK;

            status = Q_HANDLED();
            if ( (top || bot) && me->absent )
            {
                KLOG_W(TAG, "no reader was found; not asking");
            }
            else if ( top )
            {
                rc = kode_sys_nfc_read_tag(&me->super, NR_WINDOW_MS);
                if ( KODE_OK == rc )
                {
                    KLOG_I(TAG, "identify: field up for %u ms, present a tag",
                           (unsigned)NR_WINDOW_MS);
                    status = Q_TRAN(&NfcRead_identifying);
                }
            }
            else if ( bot )
            {
                /* The identity and the message in one exchange. */
                rc = kode_sys_nfc_ndef_read(&me->super, NR_WINDOW_MS);
                if ( KODE_OK == rc )
                {
                    KLOG_I(TAG, "read NDEF: field up for %u ms, present a tag",
                           (unsigned)NR_WINDOW_MS);
                    status = Q_TRAN(&NfcRead_reading);
                }
            }
            else
            {
                /* A release, or another button: nothing to do. */
            }
            if ( KODE_OK != rc )
            {
                KLOG_W(TAG, "request not taken (status %d)", (int)rc);
            }
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }

    return status;
}

/**
 * @brief A request is out: say so to an impatient finger.
 *
 * This AO has one door per kind of request and AO_Nfc takes one request at a
 * time, so a press here would only be refused. The answer always comes, a
 * window that closed empty included, so no clock of this AO's own is needed.
 *
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState NfcRead_busy(NfcRead * const me, QEvt const * const e)
{
    QState status;

    (void)me;
    switch ( e->sig )
    {
        case BUTTON_EVT_SIG: {
            ButtonEvt const * const be = (ButtonEvt const *)e;

            if ( be->pressed )
            {
                KLOG_I(TAG, "the reader is still looking; wait for its answer");
            }
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }

    return status;
}

/**
 * @brief Waiting for kode_sys_nfc_read_tag()'s answer.
 *
 * @param me The AO.
 * @param e  The event.
 * @return Q_TRAN() back to idle once answered, Q_SUPER() otherwise.
 */
static QState NfcRead_identifying(NfcRead * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case NFC_READ_RSP_SIG: {
            kode_sys_nfc_read_answer_t const * const a =
                (kode_sys_nfc_read_answer_t const *)e;

            if ( no_reader(me, a->rc) )
            {
                /* Logged once, in no_reader(). */
            }
            else if ( (uint32_t)KODE_OK == a->rc )
            {
                log_tag(a->tag.uid, a->tag.uid_len, a->tag.atqa, a->tag.sak,
                        a->tag.type);
            }
            else if ( (uint32_t)KODE_TIMEOUT == a->rc )
            {
                KLOG_I(TAG, "nothing there: no tag came within %u ms",
                       (unsigned)NR_WINDOW_MS);
            }
            else
            {
                KLOG_W(TAG, "the reader refused the request - it is busy "
                            "with somebody else's (status %d)", (int)a->rc);
            }
            status = Q_TRAN(&NfcRead_idle);
            break;
        }
        default: {
            status = Q_SUPER(&NfcRead_busy);
            break;
        }
    }

    return status;
}

/**
 * @brief Waiting for kode_sys_nfc_ndef_read()'s answer.
 *
 * @param me The AO.
 * @param e  The event.
 * @return Q_TRAN() back to idle once answered, Q_SUPER() otherwise.
 */
static QState NfcRead_reading(NfcRead * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case NFC_NDEF_RSP_SIG: {
            kode_sys_nfc_ndef_answer_t const * const a =
                (kode_sys_nfc_ndef_answer_t const *)e;

            if ( no_reader(me, a->rc) )
            {
                /* Logged once, in no_reader(). */
            }
            else if ( a->have_tag )
            {
                log_ndef(a);
            }
            else if ( (uint32_t)KODE_TIMEOUT == a->rc )
            {
                KLOG_I(TAG, "nothing there: no tag came within %u ms",
                       (unsigned)NR_WINDOW_MS);
            }
            else
            {
                KLOG_W(TAG, "the reader refused the request - it is busy "
                            "with somebody else's (status %d)", (int)a->rc);
            }
            status = Q_TRAN(&NfcRead_idle);
            break;
        }
        default: {
            status = Q_SUPER(&NfcRead_busy);
            break;
        }
    }

    return status;
}
