/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_touch.c
 * @brief The touch door: the subscription that links AO_Touch.
 */
#include "kode_sys.h"

#include "kaos.h"

/* cppcheck-suppress [misra-c2012-8.7,constParameterPointer] ; a facade call the apps outside this tree call; @p me is the asking AO, non-const as every request takes it */
kode_status_t kode_sys_touch_subscribe(QActive *me)
{
    if ( !kaos_asker_started(me) )
    {
        return KODE_DENIED;
    }
    touch_subscribe(me);
    return KODE_OK;
}
