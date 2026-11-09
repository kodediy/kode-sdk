/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_nvs.c
 * @brief The store, straight onto khal_nvs with the ceilings enforced here: the
 *        read, and the private set (kode_sys_internal.h).
 */
#include "kode_sys.h"
#include "kode_sys_internal.h"

#include "kaos.h"
#include "khal_nvs.h"

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

kode_status_t kode_block_nvs_set(const char *key, const char *value)
{
    if ( !nvs_key_ok(key) || (NULL == value)
        || (strnlen(value, KODE_SYS_NVS_VALUE_MAX) >= KODE_SYS_NVS_VALUE_MAX) )
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
