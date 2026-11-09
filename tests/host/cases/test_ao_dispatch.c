/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_ao_dispatch.c
 * @brief State machines driven by hand, with no scheduler underneath.
 */
#include <stdio.h>
#include <stdlib.h>

#include "unity.h"
#include "qpc.h"

enum { EVT_UP = Q_USER_SIG, EVT_DOWN, EVT_MAX };

typedef struct {
    QHsm super;
    int  entered_idle;
    int  entered_busy;
} Probe;

static QState Probe_initial(Probe *me, void const *par);
static QState Probe_idle(Probe *me, QEvt const *e);
static QState Probe_busy(Probe *me, QEvt const *e);

/** @brief The initial transition. @param me Self. @param par Unused. */
static QState Probe_initial(Probe *me, void const *par)
{
    (void)par;
    return Q_TRAN(&Probe_idle);
}

/** @brief Idle: counts its entries, leaves on EVT_UP. */
static QState Probe_idle(Probe *me, QEvt const *e)
{
    switch ( e->sig )
    {
        case Q_ENTRY_SIG: me->entered_idle++; return Q_HANDLED();
        case EVT_UP:      return Q_TRAN(&Probe_busy);
        default:          break;
    }
    return Q_SUPER(&QHsm_top);
}

/** @brief Busy: counts its entries, leaves on EVT_DOWN. */
static QState Probe_busy(Probe *me, QEvt const *e)
{
    switch ( e->sig )
    {
        case Q_ENTRY_SIG: me->entered_busy++; return Q_HANDLED();
        case EVT_DOWN:    return Q_TRAN(&Probe_idle);
        default:          break;
    }
    return Q_SUPER(&QHsm_top);
}

TEST_CASE("a state machine transitions under direct dispatch", "[hsm]")
{
    Probe p = { .entered_idle = 0, .entered_busy = 0 };
    QEvt  up   = QEVT_INITIALIZER(EVT_UP);
    QEvt  down = QEVT_INITIALIZER(EVT_DOWN);

    QHsm_ctor(&p.super, Q_STATE_CAST(&Probe_initial));
    QHSM_INIT(&p.super, (void *)0, 0U);
    TEST_ASSERT_EQUAL_INT(1, p.entered_idle);
    TEST_ASSERT_EQUAL_INT(0, p.entered_busy);

    QHSM_DISPATCH(&p.super, &up, 0U);
    TEST_ASSERT_EQUAL_INT(1, p.entered_busy);

    QHSM_DISPATCH(&p.super, &down, 0U);
    TEST_ASSERT_EQUAL_INT(2, p.entered_idle);

    /* An event the active state ignores must change nothing at all: no exit,
     * no entry, no self-transition anybody has to pay for. */
    QHSM_DISPATCH(&p.super, &down, 0U);
    TEST_ASSERT_EQUAL_INT(2, p.entered_idle);
    TEST_ASSERT_EQUAL_INT(1, p.entered_busy);
}

/**
 * @brief What QP calls instead of asserting, so a tripped case fails loudly.
 * @param module The QP module that raised it.
 * @param id     Its assertion id.
 */
Q_NORETURN Q_onError(char const *module, int_t const id)
{
    printf("Q_onError %s:%d\n", module, (int)id);
    abort();
}
