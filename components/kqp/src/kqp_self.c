/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kqp_self.c
 * @brief Whether the caller is an AO's thread, which AO holds a QP level, and how full queues are.
 */
#include "kqp.h"

#include "khal_system.h"

/** @brief What kqp_prio_taken() reports for a holder whose task is not made yet. */
static const char s_no_task_name[] = "?";

bool kqp_prio_taken(uint8_t prio, const char **name)
{
    QActive const * const ao = QF_aoFromPrio(prio);
    const bool found = (NULL != ao);

    if ( (NULL != name) && found )
    {
        *name = s_no_task_name;
        if ( NULL != ao->thread )
        {
            *name = pcTaskGetName(ao->thread);
        }
    }
    return found;
}

bool kqp_current_is_ao(const char **name)
{
    QActive const * const ao = QF_aoFromThread(khal_task_self());
    const bool found = (NULL != ao);

    if ( (NULL != name) && found )
    {
        *name = pcTaskGetName(ao->thread);
    }
    return found;
}

bool kqp_queues_half_free(void)
{
    bool    room = true;
    uint8_t prio = 1U;

    while ( room && (prio <= (uint8_t)QF_MAX_ACTIVE) )
    {
        /* 0 and 0 for a level no started AO holds: it has room. */
        uint32_t const len  = QF_queueLen(prio);
        uint32_t const left = QActive_getQueueFree(prio);

        room = (left * 2U) >= len;
        prio += 1U;
    }
    return room;
}

uint16_t kqp_queue_len(QActive const *ao)
{
    uint16_t len = 0U;

    /* The port reads the queue of whichever AO holds the level: only the
     * one asked about counts. */
    if ( (NULL != ao) && (QF_aoFromPrio(ao->prio) == ao) )
    {
        len = QF_queueLen(ao->prio);
    }
    return len;
}
