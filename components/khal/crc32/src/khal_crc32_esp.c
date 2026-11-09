/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_crc32_esp.c
 * @brief CRC-32 on ESP-IDF: the ROM routine, which costs no flash.
 */
#include "khal_crc32.h"

#include "esp_rom_crc.h"

uint32_t khal_crc32(uint32_t crc, const void *buf, size_t len)
{
    if ( (NULL == buf) || (0U == len) )
    {
        return crc;
    }
    return esp_rom_crc32_le(crc, (const uint8_t *)buf, (uint32_t)len);
}
