/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_khal_temp.c
 * @brief The die temperature, as a case sets it.
 */
#include <stddef.h>

#include "fakes.h"

#include "khal_temp.h"

#define FAKE_TEMP_DEFAULT_DC  415   /**< 41.5 C: what the sensor answers unless a case says otherwise. */

static kode_status_t s_status;   /**< What khal_temp_read() returns. */
static int32_t       s_deci_c;   /**< What it reads on KODE_OK. */

void fake_temp_reset(void)
{
    s_status = KODE_OK;
    s_deci_c = FAKE_TEMP_DEFAULT_DC;
}

void fake_temp_set(kode_status_t status, int32_t deci_c)
{
    s_status = status;
    s_deci_c = deci_c;
}

kode_status_t khal_temp_read(int32_t *deci_c)
{
    if ( NULL == deci_c )
    {
        return KODE_DENIED;
    }
    if ( KODE_OK == s_status )
    {
        *deci_c = s_deci_c;
    }
    return s_status;
}
