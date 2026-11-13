/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_nvs.c
 * @brief The store, straight onto khal_nvs with the ceilings enforced here: the
 *        read, the private set (kode_sys_internal.h) and the set an Active
 *        Object asks AO_Boot for.
 */
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_sys_priv.h"

#include "kaos.h"
#include "khal_nvs.h"

#include <stdio.h>
#include <string.h>

/**
 * @brief Whether a key is not NULL, not empty and shorter than KODE_SYS_NVS_KEY_MAX.
 *
 * @param key The key.
 * @return true when the store takes @p key.
 */
static bool nvs_key_ok(const char *key)
{
    if ( NULL == key )
    {
        return false;
    }

    size_t const n = strnlen(key, KODE_SYS_NVS_KEY_MAX);
    return (0U < n) && (KODE_SYS_NVS_KEY_MAX > n);
}

/**
 * @brief Whether a value is not NULL and shorter than KODE_SYS_NVS_VALUE_MAX.
 *
 * @param value The value.
 * @return true when the store takes @p value.
 */
static bool nvs_value_ok(const char *value)
{
    if ( NULL == value )
    {
        return false;
    }
    return KODE_SYS_NVS_VALUE_MAX > strnlen(value, KODE_SYS_NVS_VALUE_MAX);
}

kode_status_t kode_block_nvs_set(const char *key, const char *value)
{
    if ( !nvs_key_ok(key) || !nvs_value_ok(value) )
    {
        return KODE_ERROR;
    }
    return khal_nvs_set_str(KODE_NVS_NAMESPACE, key, value);
}

kode_status_t kode_sys_nvs_get(const char *key, char *out, size_t cap)
{
    if ( !nvs_key_ok(key) || (NULL == out) || (0U == cap) )
    {
        return KODE_DENIED;
    }
    return khal_nvs_get_str(KODE_NVS_NAMESPACE, key, out, cap);
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_nvs_set(const char *key, const char *value)
{
    kode_status_t rc = KODE_DENIED;

    if ( nvs_key_ok(key) && nvs_value_ok(value) )
    {
        BootNvsSetEvt *e   = Q_NEW_X(BootNvsSetEvt, 0U, BOOT_NVS_SET_SIG);
        QEvt const    *evt = NULL;

        if ( NULL != e )
        {
            /* Both fit with their terminators: the two checks above say so. */
            (void)snprintf(e->key, sizeof(e->key), "%s", key);
            (void)snprintf(e->value, sizeof(e->value), "%s", value);
            evt = &e->super;
        }
        rc = kode_sys_cmd_post(AO_Boot, evt, KQP_BOOT_CMD_MARGIN);
    }
    return rc;
}
