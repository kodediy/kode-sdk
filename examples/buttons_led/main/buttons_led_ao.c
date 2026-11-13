/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file buttons_led_ao.c
 * @brief An Active Object of your own, indistinguishable from one of ours.
 */
#include "buttons_led_ao.h"

#define TAG "buttons_led"

/** How often the heartbeat below says the AO is alive. */
#define BL_TICK_MS   5000U

/** The level of the colour showing. Dim on purpose - this LED is bright. */
#define BL_LEVEL     32U

/* From MAX_SIG up, beyond the bus's MAX_PUB_SIG: publishing one of these asserts. */
/** @brief This AO's private signals. Posted to itself, never published. */
enum ButtonsLedSignals {
    BL_TICK_SIG = MAX_SIG   /**< The heartbeat QTimeEvt. */
};

/** @brief The AO's own state, private to this file. */
typedef struct {
    QActive  super;
    QTimeEvt tick;
    bool     green;   /**< Which of the two colours is showing. */
} ButtonsLed;

/* The two arrays kodeOS declares for every AO of its own, written out here
 * rather than hidden behind a macro: they are exactly the two numbers you have
 * to think about. The queue holds everything that can wait in it at once,
 * because an event that finds it full is QP's assert, not a dropped event:
 * kode_sys.h states the one rule for counting that, in its Requests section
 * under "Queue depth". Counted here: the heartbeat and one button's press and
 * release, with no request ever out, so KQP_AO_QUEUE_LEN, the SDK's default,
 * has room to spare. The stack is what the deepest state function puts on
 * the stack, and KQP_AO_STACK_BYTES is the SDK's default too.
 *
 * The types are not interchangeable and the compiler will tell you so: the
 * queue holds QEvtPtr, which is QP/C's own pointer wrapper rather than a plain
 * QEvt const *, and the stack is measured in StackType_t words even though
 * KQP_AO_STACK_BYTES counts bytes. */
static QEvtPtr     l_queue[KQP_AO_QUEUE_LEN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static ButtonsLed l_ao;
QActive * const AO_ButtonsLed = &l_ao.super;

/* No step here waits: kode.h's calls wait for an answer, and an AO that calls
 * one is refused. kode_sys.h posts, and its answers arrive as events. */
static QState ButtonsLed_initial(ButtonsLed * const me, void const * const par);
static QState ButtonsLed_running(ButtonsLed * const me, QEvt const * const e);

/**
 * @brief Show the colour @p me->green names.
 *
 * kode_sys_led_set_rgb() posts to AO_LedMgr and returns. AO_LedMgr owns the
 * chip; nothing here touches I2C, and that is the rule, not a courtesy. A
 * refusal (KODE_NO_RESOURCES: its queue is full) leaves the old colour.
 *
 * @param me The AO.
 */
static void show_colour(ButtonsLed const * const me)
{
    kode_status_t rc;

    if ( me->green )
    {
        rc = kode_sys_led_set_rgb(0U, BL_LEVEL, 0U);
        KLOG_I(TAG, "green");
    }
    else
    {
        rc = kode_sys_led_set_rgb(BL_LEVEL, 0U, 0U);
        KLOG_I(TAG, "red");
    }
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "the LED refused the colour (status %d)", (int)rc);
    }
}

void ButtonsLed_ctor(void)
{
    ButtonsLed * const me = &l_ao;
    QActive_ctor(&me->super, Q_STATE_CAST(&ButtonsLed_initial));
    QTimeEvt_ctorX(&me->tick, &me->super, (enum_t)BL_TICK_SIG, 0U);
    me->green = true;
}

void ButtonsLed_start(void)
{
    /* Name the FreeRTOS task after the AO. Without this the port calls every
     * one of them "AO", which is fine until something asserts: a full queue
     * names the victim, never the culprit. */
    QActive_setAttr(AO_ButtonsLed, TASK_NAME_ATTR, "buttons_led");
    QACTIVE_START(AO_ButtonsLed, KODE_PRIO_USER_LOW,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
}

/**
 * @brief The initial transition: subscribe, then run.
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to ButtonsLed_running.
 */
static QState ButtonsLed_initial(ButtonsLed * const me, void const * const par)
{
    (void)par;

    /* Startup announcements are over by now: read state with an immediate call
     * (kode_sys_wifi_status(), kode_sys_battery_read()), then follow its signal. */
    /* One of kodeOS's published signals. AO_Buttons debounces and publishes on
     * change only, so a finger resting on the pad costs nothing here. */
    QActive_subscribe(&me->super, BUTTON_EVT_SIG);

    KLOG_I(TAG, "up; the top button swaps the colour, the bottom one resets");
    return Q_TRAN(&ButtonsLed_running);
}

/**
 * @brief The one state: a button press changes the LED or resets the board.
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState ButtonsLed_running(ButtonsLed * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            show_colour(me);
            /* A one-shot each tick arms again: a periodic one keeps posting
             * while this AO does not run, and a full queue is a reset. */
            QTimeEvt_armX(&me->tick, KQP_TICKS_PER_MS(BL_TICK_MS), 0U);
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            (void)QTimeEvt_disarm(&me->tick);
            status = Q_HANDLED();
            break;
        }
        case BUTTON_EVT_SIG: {
            ButtonEvt const * const be = (ButtonEvt const *)e;

            /* id is a button's number: KODE_SYS_BUTTON_x in kode_sys.h. */
            if ( be->pressed && (KODE_SYS_BUTTON_TOP == (uint32_t)be->id) )
            {
                me->green = !me->green;
                show_colour(me);
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
        case BL_TICK_SIG: {
            (void)QTimeEvt_rearm(&me->tick, KQP_TICKS_PER_MS(BL_TICK_MS));
            KLOG_D(TAG, "still here, %ums in", (unsigned)kode_sys_time_ms());
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
