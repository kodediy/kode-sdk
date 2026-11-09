/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file power_ao.c
 * @brief The battery and the power path, from an Active Object of your own.
 */
#include "power_ao.h"

#define TAG "power"

/** How long an armed switch-off waits for its second press. */
#define PX_ARM_MS   3000U

/** Milliseconds in a second, for the log. */
#define PX_MS_PER_S 1000U

/** @brief This AO's private signals. Posted to itself, never published. */
enum PowerDemoSignals {
    PX_DISARM_SIG = MAX_SIG   /**< The arm window closed with no second press. */
};

/** @brief The AO's own state, private to this file. */
typedef struct {
    QActive  super;
    QTimeEvt window;     /**< One-shot: the time left to confirm.            */
    bool     valid;      /**< A POWER_EVT_SIG has arrived.                   */
    bool     vbus;       /**< USB was present at the last one.               */
} PowerDemo;

/* The queue and the stack every AO of your own declares, at the SDK's
 * defaults; buttons_led_ao.c says what each number decides. The queue's depth
 * follows kode_sys.h's one queue rule ("Queue depth"): here no request, the
 * arm window's one-shot, a POWER_EVT_SIG every two seconds and a button's
 * press and release. */
static QEvtPtr     l_queue[KQP_AO_QUEUE_LEN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static PowerDemo l_ao;
QActive * const AO_PowerDemo = &l_ao.super;

static QState PowerDemo_initial(PowerDemo * const me, void const * const par);
static QState PowerDemo_running(PowerDemo * const me, QEvt const * const e);
static QState PowerDemo_safe(PowerDemo * const me, QEvt const * const e);
static QState PowerDemo_armed(PowerDemo * const me, QEvt const * const e);

/**
 * @brief Say whether USB is present.
 * @param vbus kode_sys_battery_t.vbus.
 * @return A word for the log.
 */
static const char *usb_name(bool vbus)
{
    const char *name = "absent";

    if ( vbus )
    {
        name = "present";
    }
    return name;
}

/**
 * @brief Name a charge phase.
 * @param charging kode_sys_battery_t.charging, a KODE_SYS_CHARGE_x.
 * @return A word for the log.
 */
static const char *charge_name(uint8_t charging)
{
    const char *name = "not charging";

    switch ( charging )
    {
        case (uint8_t)KODE_SYS_CHARGE_PRE: {
            name = "pre-charging";
            break;
        }
        case (uint8_t)KODE_SYS_CHARGE_FAST: {
            name = "charging";
            break;
        }
        case (uint8_t)KODE_SYS_CHARGE_DONE: {
            name = "charge done";
            break;
        }
        default: {
            break;
        }
    }
    return name;
}

/**
 * @brief Log AO_Power's last reading, or say there is none yet.
 *
 * The level is the gauge's and is printed only when the gauge answered: on a
 * board running without a cell the voltage is the PMIC's alone.
 *
 * @param why What made this line: the log reads better when it says.
 */
static void log_reading(char const * const why)
{
    kode_sys_battery_t b;

    if ( KODE_OK != kode_sys_battery_read(&b) )
    {
        /* AO_Power has not read the PMIC yet. */
        KLOG_I(TAG, "%s: no reading yet; AO_Power reads one every couple of "
                    "seconds once the PMIC answers", why);
    }
    else if ( b.gauge_ok )
    {
        KLOG_I(TAG, "%s: %u%%, %u mV, USB %s, %s",
               why, (unsigned)b.soc_pct, (unsigned)b.vbat_mv,
               usb_name(b.vbus), charge_name(b.charging));
    }
    else
    {
        KLOG_I(TAG, "%s: no gauge, %u mV, USB %s, %s",
               why, (unsigned)b.vbat_mv, usb_name(b.vbus),
               charge_name(b.charging));
    }
}

void PowerDemo_ctor(void)
{
    PowerDemo * const me = &l_ao;
    QActive_ctor(&me->super, Q_STATE_CAST(&PowerDemo_initial));
    QTimeEvt_ctorX(&me->window, &me->super, (enum_t)PX_DISARM_SIG, 0U);
    me->valid = false;
    me->vbus  = false;
}

void PowerDemo_start(void)
{
    QActive_setAttr(AO_PowerDemo, TASK_NAME_ATTR, "power");
    QACTIVE_START(AO_PowerDemo, KODE_PRIO_USER_LOW,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
}

/**
 * @brief The initial transition: subscribe to both signals, then run.
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to PowerDemo_safe.
 */
static QState PowerDemo_initial(PowerDemo * const me, void const * const par)
{
    (void)par;

    /* One published signal for the battery and one for the buttons. Neither
     * is the answer to a request, so there is nothing to ask after these. */
    QActive_subscribe(&me->super, POWER_EVT_SIG);
    QActive_subscribe(&me->super, BUTTON_EVT_SIG);

    KLOG_I(TAG, "up; top logs the battery, bottom arms the power-off");
    return Q_TRAN(&PowerDemo_safe);
}

/**
 * @brief The parent of both states: follow the cable, and say what changed.
 *
 * Here rather than in each child because a reading arrives whether or not the
 * switch-off is armed, and arming must not blind the AO to the cable.
 *
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState PowerDemo_running(PowerDemo * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case POWER_EVT_SIG: {
            PowerEvt const * const pe = (PowerEvt const *)e;
            bool const first   = !me->valid;
            bool const changed = me->valid && (me->vbus != pe->vbus);

            /* A copy, not the pointer: the event goes back to its pool when
             * this step ends. */
            me->vbus  = pe->vbus;
            me->valid = true;

            if ( first )
            {
                log_reading("first reading");
            }
            else if ( changed && me->vbus )
            {
                log_reading("USB plugged in");
            }
            else if ( changed )
            {
                log_reading("USB unplugged");
            }
            else
            {
                /* The same cable as two seconds ago: nothing to say. */
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
 * @brief Nothing armed: top logs the battery, bottom arms the power-off.
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() or a transition for what it handles, Q_SUPER() otherwise.
 */
static QState PowerDemo_safe(PowerDemo * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case BUTTON_EVT_SIG: {
            ButtonEvt const * const be = (ButtonEvt const *)e;

            /* id is a button's number: KODE_SYS_BUTTON_x in kode_sys.h. */
            if ( be->pressed && (KODE_SYS_BUTTON_TOP == (uint32_t)be->id) )
            {
                log_reading("battery");
                status = Q_HANDLED();
            }
            else if ( be->pressed && (KODE_SYS_BUTTON_BOT == (uint32_t)be->id) )
            {
                /* Destructive, so it only arms: a second press confirms. */
                status = Q_TRAN(&PowerDemo_armed);
            }
            else
            {
                /* A release, or a button this example has no use for. */
                status = Q_HANDLED();
            }
            break;
        }
        default: {
            status = Q_SUPER(&PowerDemo_running);
            break;
        }
    }

    return status;
}

/**
 * @brief The power-off is armed: a second bottom press does it.
 *
 * The window is a QTimeEvt of this AO's own, disarmed on the way out, so
 * every way of leaving - the confirm, another button, the timeout - closes it.
 *
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() or a transition for what it handles, Q_SUPER() otherwise.
 */
static QState PowerDemo_armed(PowerDemo * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            KLOG_I(TAG, "press bottom again within %u s to switch off",
                   (unsigned)(PX_ARM_MS / PX_MS_PER_S));
            QTimeEvt_armX(&me->window, KQP_TICKS_PER_MS(PX_ARM_MS), 0U);
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            (void)QTimeEvt_disarm(&me->window);
            status = Q_HANDLED();
            break;
        }
        case BUTTON_EVT_SIG: {
            ButtonEvt const * const be = (ButtonEvt const *)e;

            if ( !be->pressed )
            {
                /* The release of the press that armed it must not disarm it. */
                status = Q_HANDLED();
            }
            else if ( KODE_SYS_BUTTON_BOT == (uint32_t)be->id )
            {
                /* Say it before the command: on battery nothing after it
                 * runs, and this is the last line anybody reads. */
                KLOG_I(TAG, "switching off; on USB the request is refused, and "
                            "AO_LowPower's log says so");
                if ( KODE_OK != kode_sys_power_off() )
                {
                    KLOG_W(TAG, "AO_LowPower did not take the request; arm it again");
                }
                status = Q_TRAN(&PowerDemo_safe);
            }
            else
            {
                KLOG_I(TAG, "power-off disarmed");
                status = Q_TRAN(&PowerDemo_safe);
            }
            break;
        }
        case PX_DISARM_SIG: {
            KLOG_I(TAG, "power-off disarmed: no second press");
            status = Q_TRAN(&PowerDemo_safe);
            break;
        }
        default: {
            status = Q_SUPER(&PowerDemo_running);
            break;
        }
    }

    return status;
}
