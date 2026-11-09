/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file rfid_read_ao.c
 * @brief Ask for a 125 kHz tag, and hear the answer when the window closes.
 */
#include "rfid_read_ao.h"

#define TAG "rfid_read"

/** How long AO_Rfid keeps the field up looking for a tag, in milliseconds.
 *  AO_Rfid decodes in ~150 ms windows and ends the request at the first tag it
 *  decodes, so this is a ceiling: a tag already on the antenna answers at once.
 *  Long enough to bring a keyfob in after pressing, short enough that "no tag"
 *  arrives while the press is still fresh. The console's `rfid_read` defaults
 *  to the same. */
#define RR_WINDOW_MS   2000U

/** @brief The AO's own state, private to this file. It has no members of its
 *         own: a request is answered by an event, so nothing has to be kept
 *         between the press and the answer. */
typedef struct {
    QActive  super;
} RfidRead;

/* The two arrays kodeOS declares for every AO of its own; buttons_led_ao.c
 * explains both numbers, and the queue's depth follows kode_sys.h's one queue
 * rule ("Queue depth"): here one read out at most and a button's press and
 * release. The types are not interchangeable and the compiler will tell
 * you so: the queue holds QEvtPtr, and the stack is measured in StackType_t
 * words even though KQP_AO_STACK_BYTES counts bytes. */
static QEvtPtr     l_queue[KQP_AO_QUEUE_LEN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static RfidRead l_ao;
QActive * const AO_RfidRead = &l_ao.super;

/* No step here waits: kode.h's kode_rfid_read_id() waits for the whole
 * window, so it belongs to a task. */
static QState RfidRead_initial(RfidRead * const me, void const * const par);
static QState RfidRead_running(RfidRead * const me, QEvt const * const e);

/**
 * @brief Ask AO_Rfid for one read window and return at once.
 *
 * Posts one event and returns; the field comes up on AO_Rfid's side, and the
 * answer arrives later as RFID_OP_RSP_SIG. A second call while a window is
 * open is not queued behind it: it is refused at once with KODE_DENIED, and
 * the first one's answer still arrives when its window closes.
 *
 * @param me The AO, named as the one to answer.
 */
static void ask_for_a_tag(RfidRead * const me)
{
    kode_status_t const rc = kode_sys_rfid_read(&me->super, RR_WINDOW_MS);

    if ( KODE_OK == rc )
    {
        KLOG_I(TAG, "reading for up to %u ms, hold a tag on the antenna",
               (unsigned)RR_WINDOW_MS);
    }
    else if ( KODE_DENIED == rc )
    {
        KLOG_W(TAG, "a read is still out; its answer comes first");
    }
    else
    {
        KLOG_W(TAG, "read not taken (status %d)", (int)rc);
    }
}

/**
 * @brief Say what an answer means.
 *
 * Nothing here can tell a write's answer from a read's, because this AO never
 * asks for a clone, so every RfidOpRspEvt that reaches it is a read's.
 *
 * @param a The answer.
 */
static void report_answer(kode_sys_rfid_answer_t const * const a)
{
    if ( (uint32_t)KODE_OK == a->rc )
    {
        KLOG_I(TAG, "tag %02X%02X%02X%02X%02X, read at RF/%u",
               (unsigned)a->id[0], (unsigned)a->id[1], (unsigned)a->id[2],
               (unsigned)a->id[3], (unsigned)a->id[4], (unsigned)a->rf_div);
    }
    else if ( (uint32_t)KODE_TIMEOUT == a->rc )
    {
        KLOG_I(TAG, "no tag in %u ms", (unsigned)RR_WINDOW_MS);
    }
    else if ( (uint32_t)KODE_DENIED == a->rc )
    {
        KLOG_W(TAG, "refused: the front end is busy with another read");
    }
    else
    {
        KLOG_W(TAG, "unexpected status %u", (unsigned)a->rc);
    }
}

void RfidRead_ctor(void)
{
    RfidRead * const me = &l_ao;
    QActive_ctor(&me->super, Q_STATE_CAST(&RfidRead_initial));
}

void RfidRead_start(void)
{
    /* Name the FreeRTOS task after the AO, so an assert names it. */
    QActive_setAttr(AO_RfidRead, TASK_NAME_ATTR, "rfid_read");
    QACTIVE_START(AO_RfidRead, KODE_PRIO_USER_LOW,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
}

/**
 * @brief The initial transition: subscribe, then run.
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to RfidRead_running.
 */
static QState RfidRead_initial(RfidRead * const me, void const * const par)
{
    (void)par;

    /* The one published signal this AO listens to. The answer from AO_Rfid is
     * posted to this AO directly and needs no subscription. */
    QActive_subscribe(&me->super, BUTTON_EVT_SIG);

    KLOG_I(TAG, "up; the top button reads a tag, the bottom one resets");
    return Q_TRAN(&RfidRead_running);
}

/**
 * @brief The one state: a press asks for a read, an answer is reported.
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState RfidRead_running(RfidRead * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case BUTTON_EVT_SIG: {
            ButtonEvt const * const be = (ButtonEvt const *)e;

            /* id is a button's number: KODE_SYS_BUTTON_x in kode_sys.h. */
            if ( be->pressed && (KODE_SYS_BUTTON_TOP == (uint32_t)be->id) )
            {
                ask_for_a_tag(me);
            }
            else if ( be->pressed && (KODE_SYS_BUTTON_BOT == (uint32_t)be->id) )
            {
                /* Posts to AO_Boot, which resets the board a moment later:
                 * into kodeOS after a launcher start, into this app again
                 * after a mailbox install. */
                KLOG_I(TAG, "resetting");
                if ( KODE_OK != kode_sys_exit_to_os() )
                {
                    KLOG_W(TAG, "AO_Boot is busy; press again");
                }
            }
            else
            {
                /* A release, or another button: nothing to do. */
            }
            status = Q_HANDLED();
            break;
        }
        case RFID_OP_RSP_SIG: {
            report_answer((kode_sys_rfid_answer_t const *)e);
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
