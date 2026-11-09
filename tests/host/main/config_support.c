/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file config_support.c
 * @brief config_ask(): see config_support.h.
 */
#include "config_support.h"

#include <stdio.h>
#include <string.h>

#include "ao_spy.h"

/** @brief The sequence number every request carries; the answer echoes it. */
#define CONFIG_ASK_SEQ  7U

const KodeConfigAnsEvt *config_ask(QActive *ao, const config_ask_t *ask)
{
    KodeConfigReqEvt r;
    const QEvt      *e;

    (void)memset(&r, 0, sizeof(r));
    r.super = (QEvt)QEVT_INITIALIZER(CONFIG_REQ_SIG);
    r.seq   = CONFIG_ASK_SEQ;
    r.set   = ask->set;
    (void)snprintf(r.key, sizeof(r.key), "%s", ask->key);
    r.i = ask->i;
    r.b = ask->b;
    r.e = ask->e;
    if ( NULL != ask->s )
    {
        (void)snprintf(r.s, sizeof(r.s), "%s", ask->s);
    }

    ao_spy_subscribe(CONFIG_ANS_SIG);
    QASM_DISPATCH(ao, &r.super, 0U);
    /* Whatever the owner published before its answer - an LED word, a
     * reading - is passed over: what a set did is asserted on its effect. */
    e = ao_spy_take();
    while ( (NULL != e) && ((QSignal)CONFIG_ANS_SIG != e->sig) )
    {
        e = ao_spy_take();
    }
    return (const KodeConfigAnsEvt *)e;
}
