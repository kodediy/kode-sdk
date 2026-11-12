/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file expansion_io_ao.c
 * @brief The expansion header J3 from an AO: a blinking LED, a dimming knob, a contact's interrupt.
 */
#include "expansion_io_ao.h"

#define TAG "expansion_io"

/**
 * @name The positions of J3 this example uses
 * @brief Connector positions, 1..14.
 * @{
 */
#define EXPIO_LED_POS   ((uint8_t)4U)   /**< Digital output, blinked.          */
#define EXPIO_PWM_POS   ((uint8_t)5U)   /**< PWM output, dimmed by the knob.   */
#define EXPIO_BTN_POS   ((uint8_t)6U)   /**< Input with a pull-up and an IRQ.  */
#define EXPIO_ADC_POS   ((uint8_t)8U)   /**< Analog input: the knob.           */
/** @} */

/** Half a blink: the LED changes level this often, so a full blink is 1 s. */
#define EXPIO_BLINK_MS      500U

/** How often the knob is read. Ten times a second follows a hand well enough. */
#define EXPIO_SAMPLE_MS     100U

/**
 * How long the contact is left alone after an edge before its level is read.
 * The same 30 ms AO_Buttons waits for the pad.
 */
#define EXPIO_SETTLE_MS     30U

/** The PWM frequency: fast enough that an LED shows no flicker. */
#define EXPIO_PWM_HZ        1000U

/**
 * About where the ADC's 12 dB range tops out. A knob that reaches 3.3 V reads
 * this at its end of travel and above it the count saturates, so the LED
 * reaches full brightness a little before the knob does.
 */
#define EXPIO_ADC_FULL_MV   3100U

/** The knob's whole travel, in percent. */
#define EXPIO_PCT_FULL      100U

/** A change of the knob's reading smaller than this is not worth a log line. */
#define EXPIO_LOG_STEP_MV   100

/** @brief This AO's private signals. Posted to itself, never published. */
enum ExpansionIoSignals {
    EXPIO_BLINK_SIG = MAX_SIG,  /**< The LED's QTimeEvt: change its level.     */
    EXPIO_SAMPLE_SIG,           /**< The knob's QTimeEvt: read it.             */
    EXPIO_EDGE_SIG,             /**< From the ISR: the contact moved.          */
    EXPIO_SETTLE_SIG            /**< One-shot after an edge: read the contact. */
};

/** @brief The AO's own state, private to this file. */
typedef struct {
    QActive  super;
    QTimeEvt blink;      /**< Periodic while the positions are held.          */
    QTimeEvt sample;     /**< Periodic while the positions are held.          */
    QTimeEvt settle;     /**< One-shot: armed by an edge, read on expiry.     */
    bool     blinking;   /**< Whether the blink timer is running.             */
    bool     led;        /**< The level last driven on the LED's position.    */
    bool     contact;    /**< The contact's last settled level: true is high. */
    uint16_t percent;    /**< The duty last applied to the PWM, in percent.   */
    int32_t  logged_mv;  /**< The knob's reading at the last log line.        */
} ExpansionIo;

/* The two arrays kodeOS declares for every AO of its own, written out here
 * rather than hidden behind a macro: they are exactly the two numbers you have
 * to think about. KQP_AO_QUEUE_LEN and KQP_AO_STACK_BYTES are the SDK's own
 * defaults and a fine place to start. The queue holds QEvtPtr, QP/C's own
 * pointer wrapper, and the stack is measured in StackType_t words even though
 * KQP_AO_STACK_BYTES counts bytes. */
static QEvtPtr     l_queue[KQP_AO_QUEUE_LEN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static ExpansionIo l_ao;
QActive * const AO_ExpansionIo = &l_ao.super;

/* The contact's edge, as an event. Static and immutable: the ISR allocates
 * nothing, so it can never run a pool dry from interrupt context. */
static const QEvt EDGE_EVT = QEVT_INITIALIZER(EXPIO_EDGE_SIG);

/* Every position the example holds, so that releasing them is one loop that
 * cannot forget one. */
static const uint8_t l_positions[] = {
    EXPIO_LED_POS, EXPIO_PWM_POS, EXPIO_BTN_POS, EXPIO_ADC_POS
};

static QState ExpansionIo_initial(ExpansionIo * const me, void const * const par);
static QState ExpansionIo_holding(ExpansionIo * const me, QEvt const * const e);
static QState ExpansionIo_released(ExpansionIo * const me, QEvt const * const e);

/**
 * @brief The contact's interrupt. Interrupt context: make an event of it and leave.
 * @param arg The pointer given to kode_sys_exp_irq(); unused.
 */
static void on_edge(void *arg)
{
    (void)arg;
    /* All an ISR may do: post a static event (or give a semaphore). No Q_NEW,
     * no KLOG, no other kode_* call. */
    kqp_post_from_isr(AO_ExpansionIo, &EDGE_EVT);
}

/**
 * @brief Whether @p e is @p which going down.
 *
 * ButtonEvt.id is a button's number, KODE_SYS_BUTTON_x in kode_sys.h.
 *
 * @param e     A BUTTON_EVT_SIG event.
 * @param which The button asked about, a KODE_SYS_BUTTON_x.
 * @return true for the press of @p which, false for its release or any other button.
 */
static bool pressed(QEvt const * const e, uint32_t const which)
{
    ButtonEvt const * const be = (ButtonEvt const *)e;
    return be->pressed && (which == (uint32_t)be->id);
}

/**
 * @brief Give every position back: high impedance, no pull, no interrupt.
 *
 * kode_sys_exp_release() also closes the ADC channel and stops the PWM
 * channel a position was using, so this is the one call that undoes each
 * mode. Releasing a position that is already free is not an error.
 */
static void release_all(void)
{
    uint32_t refused = 0U;

    for ( size_t i = 0U; Q_DIM(l_positions) > i; ++i )
    {
        if ( kode_sys_exp_release(l_positions[i]) != KODE_OK )
        {
            ++refused;
        }
    }
    if ( 0U == refused )
    {
        KLOG_I(TAG, "released: EXP%u, EXP%u, EXP%u and EXP%u are free and high impedance",
               (unsigned)EXPIO_LED_POS, (unsigned)EXPIO_PWM_POS,
               (unsigned)EXPIO_BTN_POS, (unsigned)EXPIO_ADC_POS);
    }
    else
    {
        KLOG_W(TAG, "%u of %u positions would not release", (unsigned)refused,
               (unsigned)Q_DIM(l_positions));
    }
}

/**
 * @brief Take the four positions and put each in the mode this example uses.
 *
 * Each call answers with a status and a refusal stops the chain, so the log
 * names the first thing that went wrong rather than the last. The very first
 * call is also the claim on the header (see below): KODE_EXISTS
 * there means it is not this app's to have.
 *
 * @param me The AO.
 * @return true when all four positions are configured and the interrupt is armed.
 */
static bool acquire(ExpansionIo * const me)
{
    uint16_t raw = 0U;
    int32_t  mv  = 0;
    bool     cal = false;

    /* The first kode_sys_exp_*() call claims all of J3 for this app, for good:
     * a release frees a position, never the header. */
    kode_status_t rc = kode_sys_exp_output(EXPIO_LED_POS, false);
    if ( KODE_OK == rc )
    {
        rc = kode_sys_exp_pwm(EXPIO_PWM_POS, EXPIO_PWM_HZ, (uint16_t)0U);
    }
    if ( KODE_OK == rc )
    {
        rc = kode_sys_exp_input(EXPIO_BTN_POS, KODE_SYS_GPIO_PULL_UP);
    }
    if ( KODE_OK == rc )
    {
        /* The position must already be an input, which is why this comes
         * after kode_sys_exp_input(). Any edge, because a contact has two. */
        rc = kode_sys_exp_irq(EXPIO_BTN_POS, KODE_SYS_GPIO_IRQ_ANY_EDGE,
                              &on_edge, (void *)0);
    }
    if ( KODE_OK == rc )
    {
        /* Configures the position on its first call and reads it in the same
         * one, so there is no separate "make it analog" step to forget. */
        rc = kode_sys_exp_adc(EXPIO_ADC_POS, &raw, &mv, &cal);
    }

    if ( KODE_OK == rc )
    {
        me->led       = false;
        me->percent   = 0U;
        me->logged_mv = mv;
        me->contact   = kode_sys_exp_level(EXPIO_BTN_POS);
        KLOG_I(TAG, "took J3: EXP%u LED, EXP%u PWM, EXP%u contact, EXP%u knob at %u (%ld mV)",
               (unsigned)EXPIO_LED_POS, (unsigned)EXPIO_PWM_POS,
               (unsigned)EXPIO_BTN_POS, (unsigned)EXPIO_ADC_POS,
               (unsigned)raw, (long)mv);
    }
    else if ( KODE_EXISTS == rc )
    {
        KLOG_E(TAG, "J3 belongs to something else in this image; `ps` on the console says what");
    }
    else
    {
        /* Half a header is worse than none: give back what was taken. */
        KLOG_E(TAG, "J3 would not set up (status %d)", (int)rc);
        release_all();
    }
    return (KODE_OK == rc);
}

/**
 * @brief Start or stop the blinking, and leave the LED dark when it stops.
 * @param me The AO.
 */
static void toggle_blinking(ExpansionIo * const me)
{
    if ( me->blinking )
    {
        (void)QTimeEvt_disarm(&me->blink);
        me->blinking = false;
        me->led      = false;
        (void)kode_sys_exp_write(EXPIO_LED_POS, false);
        KLOG_I(TAG, "blinking off, EXP%u low", (unsigned)EXPIO_LED_POS);
    }
    else
    {
        QTimeEvt_armX(&me->blink, KQP_TICKS_PER_MS(EXPIO_BLINK_MS), 0U);
        me->blinking = true;
        KLOG_I(TAG, "blinking on");
    }
}

/**
 * @brief Read the knob, set the second LED's brightness to match, and say so
 *        when the knob has moved enough to matter.
 *
 * kode_sys_exp_pwm() on a position that is already PWM retunes it, and it is
 * only called when the percentage changed: a retune at the same frequency
 * writes the channel's duty, and a knob that has not moved is not a reason
 * to.
 *
 * @param me The AO.
 */
static void follow_knob(ExpansionIo * const me)
{
    uint16_t raw = 0U;
    int32_t  mv  = 0;
    bool     cal = false;

    if ( kode_sys_exp_adc(EXPIO_ADC_POS, &raw, &mv, &cal) != KODE_OK )
    {
        KLOG_W(TAG, "EXP%u would not convert", (unsigned)EXPIO_ADC_POS);
    }
    else
    {
        int32_t clamped = mv;
        int32_t moved   = mv - me->logged_mv;

        if ( (int32_t)EXPIO_ADC_FULL_MV < clamped )
        {
            clamped = (int32_t)EXPIO_ADC_FULL_MV;
        }
        if ( 0 > clamped )
        {
            clamped = 0;
        }
        uint16_t const percent = (uint16_t)(((uint32_t)clamped * EXPIO_PCT_FULL)
                                            / EXPIO_ADC_FULL_MV);
        /* Tenths of a percent, 0..KODE_SYS_EXP_PWM_DUTY_MAX: the API's own
         * unit. */
        uint16_t const duty =
            (uint16_t)(percent * (KODE_SYS_EXP_PWM_DUTY_MAX / EXPIO_PCT_FULL));

        if ( percent != me->percent )
        {
            if ( KODE_OK == kode_sys_exp_pwm(EXPIO_PWM_POS, EXPIO_PWM_HZ,
                                             duty) )
            {
                me->percent = percent;
            }
            else
            {
                KLOG_W(TAG, "EXP%u would not take %u %%",
                       (unsigned)EXPIO_PWM_POS, (unsigned)percent);
            }
        }

        if ( 0 > moved )
        {
            moved = -moved;
        }
        if ( EXPIO_LOG_STEP_MV <= moved )
        {
            me->logged_mv = mv;
            KLOG_I(TAG, "knob %ld mV, raw %u -> EXP%u at %u %%", (long)mv,
                   (unsigned)raw, (unsigned)EXPIO_PWM_POS, (unsigned)me->percent);
        }
    }
}

/**
 * @brief Read the contact once it has settled, say so if it changed, and let
 *        its interrupt fire again.
 *
 * The level is read now, not when the edge happened, because the edge was
 * the start of the bouncing and this is the end of it. Re-arming last is the
 * whole policy: until kode_sys_exp_irq_enable() the pin cannot post anything.
 *
 * @param me The AO.
 */
static void read_contact(ExpansionIo * const me)
{
    bool const level = kode_sys_exp_level(EXPIO_BTN_POS);

    if ( level != me->contact )
    {
        me->contact = level;
        if ( level )
        {
            KLOG_I(TAG, "EXP%u contact opened (high)", (unsigned)EXPIO_BTN_POS);
        }
        else
        {
            KLOG_I(TAG, "EXP%u contact closed (low)", (unsigned)EXPIO_BTN_POS);
        }
    }
    kode_sys_exp_irq_enable(EXPIO_BTN_POS);
}

void ExpansionIo_ctor(void)
{
    ExpansionIo * const me = &l_ao;
    QActive_ctor(&me->super, Q_STATE_CAST(&ExpansionIo_initial));
    QTimeEvt_ctorX(&me->blink,  &me->super, (enum_t)EXPIO_BLINK_SIG,  0U);
    QTimeEvt_ctorX(&me->sample, &me->super, (enum_t)EXPIO_SAMPLE_SIG, 0U);
    QTimeEvt_ctorX(&me->settle, &me->super, (enum_t)EXPIO_SETTLE_SIG, 0U);
    me->blinking  = false;
    me->led       = false;
    me->contact   = true;
    me->percent   = 0U;
    me->logged_mv = 0;
}

void ExpansionIo_start(void)
{
    /* Name the FreeRTOS task after the AO. Without this the port calls every
     * one of them "AO", which is fine until something asserts. */
    QActive_setAttr(AO_ExpansionIo, TASK_NAME_ATTR, "expansion_io");
    QACTIVE_START(AO_ExpansionIo, KODE_PRIO_USER_LOW,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
}

/**
 * @brief The initial transition: subscribe, take the header, then run.
 *
 * Runs in the thread that called QACTIVE_START(), on app_main()'s stack,
 * before the AO's own task exists - so nothing races the calls made here.
 * The board's main task stack is sized for exactly this.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to ExpansionIo_holding, or to ExpansionIo_released
 *         when the header could not be had.
 */
static QState ExpansionIo_initial(ExpansionIo * const me, void const * const par)
{
    QState status;

    (void)par;

    /* AO_Buttons debounces and publishes on change only. */
    QActive_subscribe(&me->super, BUTTON_EVT_SIG);

    if ( acquire(me) )
    {
        KLOG_I(TAG, "up; the top button starts and stops the blinking, the bottom one releases J3");
        status = Q_TRAN(&ExpansionIo_holding);
    }
    else
    {
        KLOG_I(TAG, "up without J3; the top button tries again");
        status = Q_TRAN(&ExpansionIo_released);
    }
    return status;
}

/**
 * @brief The positions are held: blink, follow the knob, watch the contact.
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_TRAN() to release, Q_SUPER() otherwise.
 */
static QState ExpansionIo_holding(ExpansionIo * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            /* Both are one-shots that each tick arms again, NOT periodic
             * timers. QP posts a time event with no margin, so a periodic one goes on posting
             * whether or not this AO has run, and a queue it fills is a reset.
             * A one-shot armed again by the tick it sent keeps one waiting at most. */
            me->blinking = true;
            QTimeEvt_armX(&me->blink, KQP_TICKS_PER_MS(EXPIO_BLINK_MS), 0U);
            QTimeEvt_armX(&me->sample, KQP_TICKS_PER_MS(EXPIO_SAMPLE_MS), 0U);
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            (void)QTimeEvt_disarm(&me->blink);
            (void)QTimeEvt_disarm(&me->sample);
            (void)QTimeEvt_disarm(&me->settle);
            status = Q_HANDLED();
            break;
        }
        case BUTTON_EVT_SIG: {
            if ( pressed(e, KODE_SYS_BUTTON_TOP) )
            {
                toggle_blinking(me);
                status = Q_HANDLED();
            }
            else if ( pressed(e, KODE_SYS_BUTTON_BOT) )
            {
                release_all();
                status = Q_TRAN(&ExpansionIo_released);
            }
            else
            {
                /* A release, or another button: nothing to do. */
                status = Q_HANDLED();
            }
            break;
        }
        case EXPIO_BLINK_SIG: {
            /* A tick already queued when the blinking was stopped still
             * arrives, and must neither light the LED again nor arm the
             * next one. */
            if ( me->blinking )
            {
                (void)QTimeEvt_rearm(&me->blink, KQP_TICKS_PER_MS(EXPIO_BLINK_MS));
                me->led = !me->led;
                (void)kode_sys_exp_write(EXPIO_LED_POS, me->led);
                if ( me->led )
                {
                    KLOG_D(TAG, "EXP%u high", (unsigned)EXPIO_LED_POS);
                }
                else
                {
                    KLOG_D(TAG, "EXP%u low", (unsigned)EXPIO_LED_POS);
                }
            }
            status = Q_HANDLED();
            break;
        }
        case EXPIO_SAMPLE_SIG: {
            (void)QTimeEvt_rearm(&me->sample, KQP_TICKS_PER_MS(EXPIO_SAMPLE_MS));
            follow_knob(me);
            status = Q_HANDLED();
            break;
        }
        case EXPIO_EDGE_SIG: {
            /* The interrupt disarmed itself before it posted this, so no
             * second edge can arrive while the timer runs. QTimeEvt_rearm
             * arms a one-shot that is not running, which is what is wanted. */
            (void)QTimeEvt_rearm(&me->settle, KQP_TICKS_PER_MS(EXPIO_SETTLE_MS));
            status = Q_HANDLED();
            break;
        }
        case EXPIO_SETTLE_SIG: {
            read_contact(me);
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
 * @brief Nothing is held: the header is free until the top button takes it.
 *
 * Every private signal of the holding state is ignored here on purpose. A
 * tick or an edge that was already queued when the positions were released
 * arrives late, finds no handler, and falls through to the top state - which is
 * the right answer, since there is nothing left for it to act on.
 *
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_TRAN() to take the header again,
 *         Q_SUPER() otherwise.
 */
static QState ExpansionIo_released(ExpansionIo * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case BUTTON_EVT_SIG: {
            if ( pressed(e, KODE_SYS_BUTTON_TOP) && acquire(me) )
            {
                status = Q_TRAN(&ExpansionIo_holding);
            }
            else
            {
                status = Q_HANDLED();
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
