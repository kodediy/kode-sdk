/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kqp_isr.c
 * @brief The one door an interrupt has into an Active Object.
 */
#include "kqp.h"

void kqp_post_from_isr(QActive *ao, QEvt const *e)
{
    BaseType_t woken = pdFALSE;

    QACTIVE_POST_FROM_ISR(ao, e, &woken, (void *)0);
    portYIELD_FROM_ISR(woken);
}
