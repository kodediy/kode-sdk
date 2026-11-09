/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ble_find.c
 * @brief Which advertisement is the one a find waits for, in pure C.
 */
#include "kdrv_ble_find.h"

#include <string.h>

bool kdrv_ble_find_begin(kdrv_ble_find_t *f, const char *name)
{
    if ( (NULL == f) || (NULL == name) || ('\0' == name[0]) )
    {
        return false;
    }
    size_t const len = strlen(name);

    if ( KDRV_BLE_FIND_NAME_MAX < len )
    {
        return false;
    }
    (void)memset(&f->dev, 0, sizeof(f->dev));
    (void)memset(f->name, 0, sizeof(f->name));
    (void)memcpy(f->name, name, len);
    f->hit = false;
    f->on  = true;
    return true;
}

bool kdrv_ble_find_offer(kdrv_ble_find_t *f, const uint8_t *addr_le, uint8_t addr_type,
                         int8_t rssi, const uint8_t *name, size_t name_len)
{
    if ( (NULL == f) || (!f->on) || f->hit || (NULL == addr_le) || (NULL == name)
         || (0U == name_len) )
    {
        return false;
    }
    size_t const want = strlen(f->name);

    if ( (name_len < want) || (0 != memcmp(name, f->name, want)) )
    {
        return false;
    }

    (void)memset(&f->dev, 0, sizeof(f->dev));
    for ( uint8_t i = 0U; KDRV_BLE_FIND_ADDR_LEN > i; ++i )
    {
        f->dev.addr[i] = addr_le[(KDRV_BLE_FIND_ADDR_LEN - 1U) - i];
    }
    f->dev.addr_type = addr_type;
    f->dev.rssi      = rssi;

    size_t kept = name_len;

    if ( (sizeof(f->dev.name) - 1U) < kept )
    {
        kept = sizeof(f->dev.name) - 1U;
    }
    (void)memcpy(f->dev.name, name, kept);
    f->hit = true;
    return true;
}

void kdrv_ble_find_end(kdrv_ble_find_t *f)
{
    if ( NULL != f )
    {
        f->on = false;
    }
}
