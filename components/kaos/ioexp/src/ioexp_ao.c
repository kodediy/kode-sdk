/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file ioexp_ao.c
 * @brief AO_IoExp: the Active Object that owns the TCA6408A expander.
 */
#include "kaos.h"
#include "kaos_start.h"

#include <stdio.h>

#include "kbsp_pins.h"   /* which pins the board wires as inputs, and nothing else */
#include "kqp.h"
#include "kaos_claim.h"
#include "kdrv_buttons.h"
#include "kdrv_tca6408a.h"
#include "kode_log.h"

#define TAG "kaos.ioexp"

typedef struct {
    QActive  super;
    QTimeEvt retry;   /**< One-shot: INT fired, the register did not read,
                       *   look again - see int_serve().              */
    uint8_t  last;    /**< The input register as last published.      */
#if CONFIG_KODE_LOWPOWER
    uint8_t  lp_who;  /**< The id lowpower_join() gave.               */
    bool     retrying;/**< INT fired and the read failed: a retry waits. */
    bool     waking;  /**< INT is armed as a light-sleep wake.        */
#endif
} IoExp;

static IoExp l_ioexp;
QActive * const AO_IoExp = &l_ioexp.super;

static QState IoExp_initial(IoExp * const me, void const * const par);
static QState IoExp_dead(IoExp * const me, QEvt const * const e);
static QState IoExp_ready(IoExp * const me, QEvt const * const e);

/* The edge, as an event. Static and immutable: the ISR allocates nothing. */
static const QEvt INT_EVT = QEVT_INITIALIZER(IOEXP_INT_SIG);

/* ---- Requests ------------------------------------------------------------ */

void ioexp_read_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(IOEXP_READ_REQ_SIG);
    QACTIVE_POST(AO_IoExp, &evt, NULL);
}

void ioexp_field_request(QActive *who, uint8_t op, uint16_t field, int32_t value)
{
    IoexpFieldReqEvt *e = Q_NEW_X(IoexpFieldReqEvt, 0U, IOEXP_FIELD_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; field request dropped");
        return;
    }
    e->who   = who;
    e->op    = op;
    e->field = field;
    e->value = value;
    QACTIVE_POST(AO_IoExp, &e->super, NULL);
}

void ioexp_test_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(IOEXP_TEST_REQ_SIG);
    QACTIVE_POST(AO_IoExp, &evt, NULL);
}

/* ---- The interrupt ------------------------------------------------------- */

/** @brief GPIO6 went low. Interrupt context: make an event of it and leave. */
static void on_int(void *arg)
{
    (void)arg;
    kqp_post_from_isr(AO_IoExp, &INT_EVT);
}

/* ---- Construction -------------------------------------------------------- */

void IoExp_ctor(void)
{
    IoExp * const me = &l_ioexp;
    QActive_ctor(&me->super, Q_STATE_CAST(&IoExp_initial));
    QTimeEvt_ctorX(&me->retry, &me->super, IOEXP_RETRY_SIG, 0U);
    me->last = 0xFFU;   /* every input at rest, until the first read says otherwise */
#if CONFIG_KODE_LOWPOWER
    me->lp_who   = KODE_LOWPOWER_WHO_NONE;
    me->retrying = false;
    me->waking   = false;
#endif
}

AO_STORAGE(ioexp, KQP_AO_QUEUE_LEN, KQP_AO_STACK_MEDIUM);

void kaos_ioexp_start(void)
{
    IoExp_ctor();
    AO_START(AO_IoExp, ioexp, KODE_PRIO_IOEXP);
}

/**
 * @brief Publish what the input register reads, or that it could not be read.
 *
 * `changed` is measured against the last reading that succeeded, so a chip
 * that went away and came back reports whatever moved while it was gone.
 *
 * @return true when the register read.
 */
static bool publish_inputs(IoExp * const me)
{
    int32_t raw = 0;
    bool const ok = (kdrv_tca6408a_get(KDRV_TCA_INPUT, &raw) == KODE_OK);

    IoexpInputEvt *ev = Q_NEW_X(IoexpInputEvt, 0U, IOEXP_INPUT_SIG);
    if ( NULL == ev )
    {
        return ok;   /* bounded: dropping a sample beats asserting */
    }
    ev->ok = ok;
    if ( ok )
    {
        uint8_t const levels = (uint8_t)raw;
        ev->levels  = levels;
        ev->changed = (uint8_t)(levels ^ me->last);
        me->last    = levels;
        KLOG_D(TAG, "input=0x%02X changed=0x%02X", (unsigned)levels,
               (unsigned)ev->changed);
    }
    else
    {
        ev->levels  = 0U;
        ev->changed = 0U;
    }
    QACTIVE_PUBLISH(&ev->super, &me->super);
    return ok;
}

/**
 * @brief Serve the INT line: read, publish, and re-arm only on a good read.
 *
 * The read is what releases INT. After a failed read the line is still low,
 * and re-arming it would fire again at once, each failed read holding the
 * shared I2C bus for its whole timeout. So a failed read arms a one-shot
 * instead and the line is read again every KODE_IOEXP_RETRY_MS.
 */
static void int_serve(IoExp * const me)
{
    if ( publish_inputs(me) )
    {
#if CONFIG_KODE_LOWPOWER
        me->retrying = false;
#endif
        kdrv_tca6408a_int_rearm();
    }
    else
    {
#if CONFIG_KODE_LOWPOWER
        me->retrying = true;
#endif
        (void)QTimeEvt_disarm(&me->retry);
        QTimeEvt_armX(&me->retry, KQP_TICKS_PER_MS(KODE_IOEXP_RETRY_MS), 0U);
    }
}

/**
 * @brief Every pin the board wires as an input, touching only the wrong bits.
 *
 * KBSP_IOEXP_DIR_INPUTS says which pins face in. P7, the panel's supply, is
 * not in it: it stays as the last firmware left it until AO_Display asks.
 */
static void inputs_face_in(void)
{
    int32_t config = 0;

    if ( kdrv_tca6408a_get(KDRV_TCA_CONFIG, &config) != KODE_OK )
    {
        KLOG_W(TAG, "cannot read the direction register; leaving it alone");
        return;
    }
    uint8_t const wrong = (uint8_t)(KBSP_IOEXP_DIR_INPUTS & (uint8_t)~(uint8_t)config);

    for ( uint8_t bit = 0U; 8U > bit; ++bit )
    {
        if ( (wrong & (uint8_t)(1U << bit)) != 0U )
        {
            kdrv_tca6408a_field_t const c = (kdrv_tca6408a_field_t)((uint8_t)KDRV_TCA_C0 + bit);
            KLOG_W(TAG, "P%u was left as an output; making it an input again",
                   (unsigned)bit);
            if ( kdrv_tca6408a_set(c, 1) != KODE_OK )
            {
                KLOG_E(TAG, "P%u would not go back to an input", (unsigned)bit);
            }
        }
    }
}

#if CONFIG_KODE_LOWPOWER
/**
 * @brief LOWPOWER_ROUND_SIG: arm INT as the light-sleep wake, and answer once.
 *
 * @param me The AO.
 * @param r  The round.
 */
static void ioexp_round(IoExp * const me, const LowPowerRoundEvt * const r)
{
    kode_status_t rc = KODE_OK;

    if ( me->retrying )
    {
        lowpower_ready(me->lp_who, r->round, true, "expander retry");
    }
    else
    {
        if ( !me->waking )
        {
            rc = kdrv_tca6408a_sleep_wake(true);
            /* The top button is on a P4 pin of its own, not on this chip. */
            if ( (KODE_OK == rc) && (kdrv_buttons_top_sleep_wake(true) != KODE_OK) )
            {
                KLOG_W(TAG, "the top button will not wake the board");
            }
        }
        if ( KODE_OK != rc )
        {
            lowpower_ready(me->lp_who, r->round, true, "no wake line");
        }
        else
        {
            me->waking = true;
            lowpower_ready(me->lp_who, r->round, false, NULL);
        }
    }
}

/**
 * @brief LOWPOWER_STATE_SIG: on AWAKE, INT is the plain interrupt again.
 *
 * Idempotent: it acts only on a wake that was armed, once.
 *
 * @param me The AO.
 * @param s  The change.
 */
static void ioexp_state(IoExp * const me, const LowPowerStateEvt * const s)
{
    if ( ((uint8_t)KODE_LP_AWAKE == s->state) && me->waking )
    {
        me->waking = false;
        (void)kdrv_buttons_top_sleep_wake(false);
        if ( kdrv_tca6408a_sleep_wake(false) != KODE_OK )
        {
            KLOG_W(TAG, "INT would not go back to a plain interrupt");
        }
    }
}
#endif /* CONFIG_KODE_LOWPOWER */

/**
 * @brief Claim the expander, bring it up, publish its inputs and arm its INT.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to IoExp_ready, or to IoExp_dead without the chip.
 */
static QState IoExp_initial(IoExp * const me, void const * const par)
{
    (void)par;

    kaos_claim(KBSP_RES_IOEXP, me, "AO_IoExp");
#if CONFIG_KODE_LOWPOWER
    me->lp_who = lowpower_join(&me->super, "ioexp");
    if ( KODE_LOWPOWER_WHO_NONE != me->lp_who )
    {
        QActive_subscribe(&me->super, LOWPOWER_ROUND_SIG);
        QActive_subscribe(&me->super, LOWPOWER_STATE_SIG);
    }
#endif

    if ( kdrv_tca6408a_init() != KODE_OK )
    {
        KLOG_W(TAG, "no expander; the buttons behind it, the touch IRQ and the "
                    "panel's supply are out of reach");
        return Q_TRAN(&IoExp_dead);
    }
    inputs_face_in();
    publish_inputs(me);
    if ( kdrv_tca6408a_int_attach(&on_int, me) != KODE_OK )
    {
        KLOG_E(TAG, "the INT could not be armed; inputs will only be read "
                    "on request");
    }
    else
    {
        KLOG_I(TAG, "up; INT armed");
    }
    return Q_TRAN(&IoExp_ready);
}

/* ---- Answering ----------------------------------------------------------- */

/** @brief Answer a field request to whoever asked. Bounded, never asserts. */
static void field_answer(IoexpFieldReqEvt const *q, kode_status_t rc, int32_t value)
{
    if ( NULL == q->who )
    {
        return;
    }
    IoexpFieldEvt *ev = Q_NEW_X(IoexpFieldEvt, 0U, IOEXP_FIELD_RSP_SIG);
    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; a field answer is lost");
        return;
    }
    ev->op    = q->op;
    ev->field = q->field;
    ev->rc    = (uint8_t)rc;
    ev->value = value;
    QACTIVE_POST(q->who, &ev->super, NULL);
}

/** @brief One field of the chip, as asked. SET is followed by a GET so the
 *  answer carries what the chip kept. */
static kode_status_t field_do(IoexpFieldReqEvt const *q, int32_t *value)
{
    kdrv_tca6408a_field_t const f = (kdrv_tca6408a_field_t)q->field;
    kode_status_t rc = KODE_DENIED;

    switch ( q->op )
    {
        case FIELD_OP_GET: {
            rc = kdrv_tca6408a_get(f, value);
            break;
        }
        case FIELD_OP_SET: {
            rc = kdrv_tca6408a_set(f, *value);
            if ( KODE_OK == rc )
            {
                rc = kdrv_tca6408a_get(f, value);
            }
            break;
        }
        case FIELD_OP_DUMP: {
            kdrv_tca6408a_dump();
            rc = KODE_OK;
            break;
        }
        default: {
            break;
        }
    }
    return rc;
}

/** @brief Publish one self-test verdict. Bounded, never asserts. */
static void test_verdict(IoExp * const me, bool ok)
{
    TestResultEvt *ev = Q_NEW_X(TestResultEvt, 0U, TEST_RESULT_SIG);
    if ( NULL != ev )
    {
        (void)snprintf(ev->name, sizeof(ev->name), "%s", "ioexp");
        ev->ok = ok;
        ev->origin = TEST_ORIGIN_AUTOTEST;
        QACTIVE_PUBLISH(&ev->super, &me->super);
    }
}

/**
 * @brief `test component=ioexp`: the chip doing its job, not just answering.
 *
 * Three checks, all run, all logged: the direction register has P0..P6
 * facing in, the input register reads, and reading it released the INT line.
 */
static void self_test(IoExp * const me)
{
    int32_t config = 0;
    int32_t input  = 0;
    bool    ok     = true;

    if ( kdrv_tca6408a_get(KDRV_TCA_CONFIG, &config) != KODE_OK )
    {
        KLOG_E(TAG, "test: the direction register did not read");
        ok = false;
    }
    else if ( ((uint8_t)config & KBSP_IOEXP_DIR_INPUTS) != KBSP_IOEXP_DIR_INPUTS )
    {
        KLOG_E(TAG, "test: config=0x%02X, P0..P6 should be inputs (0x%02X)",
               (unsigned)config, (unsigned)KBSP_IOEXP_DIR_INPUTS);
        ok = false;
    }
    else
    {
        KLOG_N(TAG, "test: config=0x%02X, inputs face in", (unsigned)config);
    }

    if ( kdrv_tca6408a_get(KDRV_TCA_INPUT, &input) != KODE_OK )
    {
        KLOG_E(TAG, "test: the input register did not read");
        ok = false;
    }
    else
    {
        KLOG_N(TAG, "test: input=0x%02X", (unsigned)input);
    }

    if ( !kdrv_tca6408a_int_idle() )
    {
        KLOG_E(TAG, "test: INT still asserted after the read; the line or the "
                    "chip's interrupt logic is wrong");
        ok = false;
    }
    else
    {
        KLOG_N(TAG, "test: INT released by the read");
    }
    test_verdict(me, ok);
}

/* ---- States -------------------------------------------------------------- */

/** @brief No chip answered at boot. Every request is answered with a refusal. */
static QState IoExp_dead(IoExp * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case IOEXP_READ_REQ_SIG:
        case IOEXP_INT_SIG: {
            IoexpInputEvt *ev = Q_NEW_X(IoexpInputEvt, 0U, IOEXP_INPUT_SIG);
            if ( NULL != ev )
            {
                ev->ok      = false;
                ev->levels  = 0U;
                ev->changed = 0U;
                QACTIVE_PUBLISH(&ev->super, &me->super);
            }
            status = Q_HANDLED();
            break;
        }
        case IOEXP_FIELD_REQ_SIG: {
            IoexpFieldReqEvt const * const q = Q_EVT_CAST(IoexpFieldReqEvt);
            field_answer(q, KODE_ERROR, q->value);
            status = Q_HANDLED();
            break;
        }
        case IOEXP_TEST_REQ_SIG: {
            KLOG_E(TAG, "test: no expander answered at boot");
            test_verdict(me, false);
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            /* No chip: no line to wake on, and nothing to rest. */
            lowpower_ready(me->lp_who, Q_EVT_CAST(LowPowerRoundEvt)->round,
                           false, NULL);
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

/** @brief The chip is there: serve its interrupt and everybody's requests. */
static QState IoExp_ready(IoExp * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case IOEXP_INT_SIG: {
            KLOG_D(TAG, "INT");
            int_serve(me);
            status = Q_HANDLED();
            break;
        }
        case IOEXP_RETRY_SIG: {
            /* The line was left un-armed by a read that failed; the same
             * service, and a chip that came back re-arms it. */
            int_serve(me);
            status = Q_HANDLED();
            break;
        }
        case IOEXP_READ_REQ_SIG: {
            (void)publish_inputs(me);
            status = Q_HANDLED();
            break;
        }
        case IOEXP_FIELD_REQ_SIG: {
            IoexpFieldReqEvt const * const q = Q_EVT_CAST(IoexpFieldReqEvt);
            int32_t value = q->value;
            kode_status_t const rc = field_do(q, &value);
            field_answer(q, rc, value);
            status = Q_HANDLED();
            break;
        }
        case IOEXP_TEST_REQ_SIG: {
            self_test(me);
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            ioexp_round(me, Q_EVT_CAST(LowPowerRoundEvt));
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_STATE_SIG: {
            ioexp_state(me, Q_EVT_CAST(LowPowerStateEvt));
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
