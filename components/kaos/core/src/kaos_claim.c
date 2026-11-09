/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kaos_claim.c
 * @brief One place to fail from, so the message says something useful.
 */
#include "kaos_claim.h"

#include "kqp.h"
#include "khal_system.h"

Q_DEFINE_THIS_MODULE("kaos_claim")

void kaos_claim(kbsp_res_t res, const void *owner, const char *name)
{
    if ( kbsp_claim(res, owner, name) )
    {
        return;
    }

    /* khal_panic_print, not KLOG_E: the Q_ERROR() below resets the board
     * before AO_Log would get a turn to print an event, and a fatal error
     * whose explanation never arrives is the worst of both. */
    khal_panic_print("\r\nkodeOS: %s claimed by both %s and %s\r\n",
                     kbsp_res_name(res), kbsp_owner_name(res), name);
    Q_ERROR();
}

void kaos_require(kode_status_t st, const char *what)
{
    if ( KODE_OK != st )
    {
        /* khal_panic_print, for the reason kaos_claim() gives. */
        khal_panic_print("\r\nkodeOS: %s refused by its registry\r\n", what);
        Q_ERROR();
    }
}
