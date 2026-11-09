/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kaos_ask.c
 * @brief The doors every owner keeps for kode_sys's requests.
 */
#include "kaos_ask.h"

#include "kaos.h"   /* kaos_asker_started() */

#include <stddef.h>

bool kaos_asker_started(QActive const *who)
{
    return (NULL != who) && (0U != who->prio) && (QF_MAX_ACTIVE >= who->prio);
}

kode_status_t kaos_ask_take(kaos_ask_door_t *door, QActive const *who)
{
    if ( !kaos_asker_started(who) )
    {
        return KODE_DENIED;
    }
    if ( atomic_exchange(&door->shut[who->prio], true) )
    {
        return KODE_DENIED;
    }
    return KODE_OK;
}

void kaos_ask_release(kaos_ask_door_t *door, QActive const *who)
{
    atomic_store(&door->shut[who->prio], false);
}

kode_status_t kaos_ask_cancel(kaos_ask_door_t *door, QActive const *who, QEvt const *rsp)
{
    if ( NULL != rsp )
    {
        QF_gc(rsp);
    }
    kaos_ask_release(door, who);
    return KODE_NO_RESOURCES;
}

kode_status_t kaos_ask_post(kaos_ask_door_t *door, QActive const *who, QActive *owner,
                            QEvt const *req, QEvt const *rsp, uint_fast16_t margin)
{
    if ( !QACTIVE_POST_X(owner, req, margin, NULL) )
    {
        return kaos_ask_cancel(door, who, rsp);
    }
    return KODE_OK;
}

void kaos_ask_answer(kaos_ask_door_t *door, QActive *who, QEvt const *rsp,
                     QActive const *owner)
{
    (void)owner;    /* the sender QS names; unread without Q_SPY */
    kaos_ask_release(door, who);
    QACTIVE_POST(who, rsp, owner);
}
