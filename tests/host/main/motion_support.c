/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file motion_support.c
 * @brief AO_Haptic as a mailbox in a case, and a queue emptied unrun.
 */
#include "motion_support.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "kaos.h"
#include "kqp.h"
#include "qf_support.h"

/** @brief Ticks run first so QP unlinks a disarmed QTimeEvt before the AO is rebuilt. */
#define MOTION_SUPPORT_TICKS 64U

void motion_support_drain(QActive *ao)
{
    const QEvt *e;

    while ( (NULL != ao->eQueue)
           && (xQueueReceive(ao->eQueue, &e, 0U) == pdTRUE) )
    {
        QF_gc(e);
    }
}

void motion_support_haptic_up(void)
{
    qf_support_init();
    /* Unrun, before the ticks: they would dispatch it into a machine that was never initialised. */
    motion_support_drain(AO_Haptic);
    (void)qf_support_tick(AO_Haptic, MOTION_SUPPORT_TICKS);

    Haptic_ctor();
    qf_support_give_queue(AO_Haptic);
    AO_Haptic->prio  = KODE_PRIO_HAPTIC;
    AO_Haptic->pthre = KODE_PRIO_HAPTIC;
    if ( qf_support_claim_ao(AO_Haptic) )
    {
        QActive_register_(AO_Haptic);
    }
    motion_support_drain(AO_Haptic);
}
