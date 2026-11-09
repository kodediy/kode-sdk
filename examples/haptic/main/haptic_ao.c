/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file haptic_ao.c
 * @brief Asking the motor's owner for a buzz, and hearing how it went.
 */
#include "haptic_ao.h"

#define TAG "haptic"

/** The tick: short enough to read as a click rather than a buzz. */
#define HD_TICK_MS      ((uint16_t)30U)

/** The tick's strength, as a percentage of full scale. */
#define HD_TICK_GAIN    ((uint8_t)50U)

/** The buzz: long enough to be unmistakable in the hand. */
#define HD_BUZZ_MS      ((uint16_t)400U)

/** The buzz's strength. 80 is the LRA's rating, the ceiling the driver clamps
 *  every request to, so asking for 100 would only promise more than the motor
 *  is allowed to give. */
#define HD_BUZZ_GAIN    ((uint8_t)80U)

/** @brief The AO's own state, private to this file. It has no members beyond
 *         the QP base, because nothing here needs remembering: the motor's
 *         owner keeps the "busy" fact, and answers it at the door. */
typedef struct {
    QActive super;
} HapticDemo;

/* The two arrays kodeOS declares for every AO of its own; buttons_led_ao.c
 * says what each number decides. The queue's depth follows kode_sys.h's one
 * queue rule ("Queue depth"): here no request is ever out, so it holds a
 * button's press and release and one HAPTIC_DONE_SIG. */
static QEvtPtr     l_queue[KQP_AO_QUEUE_LEN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static HapticDemo l_ao;
QActive * const AO_HapticDemo = &l_ao.super;

static QState HapticDemo_initial(HapticDemo * const me, void const * const par);
static QState HapticDemo_running(HapticDemo * const me, QEvt const * const e);

/**
 * @brief Ask AO_Haptic for one buzz and say what it answered.
 *
 * The return value is the whole of the answer to "was it taken", and it is
 * checked on every call: a caller that was told KODE_OK has been promised a
 * buzz, and one that was told KODE_DENIED must not assume it got one.
 *
 * @param what     What the buzz is called, for the log.
 * @param ms       How long to hold it.
 * @param gain_pct Drive strength as a percentage of full scale.
 */
static void buzz(char const * const what, uint16_t const ms,
                 uint8_t const gain_pct)
{
    /* Posts and returns; kode.h's kode_haptic_play() waits for the motor and
     * belongs to a task. */
    kode_status_t const rc = kode_sys_haptic_play_buzz(ms, gain_pct);

    if ( KODE_OK == rc )
    {
        KLOG_I(TAG, "%s: asked for %u ms at gain %u",
               what, (unsigned)ms, (unsigned)gain_pct);
    }
    else if ( KODE_DENIED == rc )
    {
        /* The one refusal that is the caller's business: there is one motor
         * and it is still playing (the arguments here are in range). Nothing
         * was posted. */
        KLOG_W(TAG, "%s: refused, a buzz is still playing", what);
    }
    else
    {
        KLOG_E(TAG, "%s: refused, no event to send it in (status %d)",
               what, (int)rc);
    }
}

/**
 * @brief Log how a buzz that has just ended went.
 *
 * @param de The published answer.
 */
static void report_done(HapticDoneEvt const * const de)
{
    if ( (uint8_t)KODE_OK != de->rc )
    {
        KLOG_E(TAG, "a buzz failed (status %u) after %u samples",
               (unsigned)de->rc, (unsigned)de->samples);
    }
    else if ( de->starved )
    {
        /* Not a fault: the chip was ahead of the feed at least once, and the
         * buzz played out whole. It says how close the feed came to the FIFO's
         * own back-pressure, which is the one thing the status cannot. */
        KLOG_I(TAG, "buzz over: %u samples; the FIFO pushed back once or more",
               (unsigned)de->samples);
    }
    else
    {
        KLOG_I(TAG, "buzz over: %u samples", (unsigned)de->samples);
    }
}

void HapticDemo_ctor(void)
{
    HapticDemo * const me = &l_ao;
    QActive_ctor(&me->super, Q_STATE_CAST(&HapticDemo_initial));
}

void HapticDemo_start(void)
{
    /* Name the FreeRTOS task after the AO, so a full queue or an assert names
     * the right one. */
    QActive_setAttr(AO_HapticDemo, TASK_NAME_ATTR, "haptic");
    QACTIVE_START(AO_HapticDemo, KODE_PRIO_USER_LOW,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
}

/**
 * @brief The initial transition: subscribe to both answers, then run.
 *
 * Both subscriptions come before anything is requested, so the first buzz's
 * end cannot arrive to nobody. BUTTON_EVT_SIG is AO_Buttons' published
 * change; HAPTIC_DONE_SIG is AO_Haptic's published end of a buzz.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to HapticDemo_running.
 */
static QState HapticDemo_initial(HapticDemo * const me, void const * const par)
{
    (void)par;

    QActive_subscribe(&me->super, BUTTON_EVT_SIG);
    QActive_subscribe(&me->super, HAPTIC_DONE_SIG);

    KLOG_I(TAG, "up; the top button ticks, the bottom one buzzes");
    return Q_TRAN(&HapticDemo_running);
}

/**
 * @brief The one state: a button asks for a buzz, and DONE says how it went.
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState HapticDemo_running(HapticDemo * const me, QEvt const * const e)
{
    QState status;

    (void)me;

    switch ( e->sig )
    {
        case BUTTON_EVT_SIG: {
            ButtonEvt const * const be = (ButtonEvt const *)e;

            /* id is a button's number: KODE_SYS_BUTTON_x in kode_sys.h. */
            if ( be->pressed && (KODE_SYS_BUTTON_TOP == (uint32_t)be->id) )
            {
                buzz("tick", HD_TICK_MS, HD_TICK_GAIN);
            }
            else if ( be->pressed && (KODE_SYS_BUTTON_BOT == (uint32_t)be->id) )
            {
                buzz("buzz", HD_BUZZ_MS, HD_BUZZ_GAIN);
            }
            else
            {
                /* A release, or another button: nothing to do. */
            }
            status = Q_HANDLED();
            break;
        }
        /* Never waited for before asking again: a board without the AW86233 takes
         * the request and never publishes HAPTIC_DONE_SIG. */
        case HAPTIC_DONE_SIG: {
            report_done((HapticDoneEvt const *)e);
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
