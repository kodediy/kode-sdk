/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file runner.c
 * @brief Entry point of the NVS suite.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "unity.h"

/* An NVS page header as nvs_flash lays it out (nvs_page.hpp). */
#define NVS_PAGE_BYTES      4096U         /**< One page: a flash sector.                          */
#define NVS_HDR_BYTES       32U           /**< The page header.                                   */
#define NVS_HDR_SEQ_AT      4U            /**< The sequence number; the header's CRC starts here.  */
#define NVS_HDR_VERSION_AT  8U            /**< The format version byte.                           */
#define NVS_HDR_CRC_AT      28U           /**< The header's CRC-32.                               */
#define NVS_PAGE_ACTIVE     0xFFFFFFFEU   /**< The state of a page in use.                        */
#define NVS_VERSION_NEWER   0xFDU         /**< One below this NVS's 0xFE: a newer format.         */
#define NVS_CRC_SEED        0xFFFFFFFFU   /**< What nvs_flash seeds the header's CRC with.        */

/**
 * @brief Leave the nvs partition as a newer NVS would: its first page in a
 *        format this one cannot read.
 *
 * So the suite's first khal_nvs_init() finds it and erases it, as a board does
 * after an app built on a newer ESP-IDF wrote its settings.
 */
static void nvs_from_newer_idf(void)
{
    const esp_partition_t * const part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
    uint8_t        hdr[NVS_HDR_BYTES];
    uint32_t const state = NVS_PAGE_ACTIVE;
    uint32_t       crc;

    if ( NULL == part )
    {
        abort();
    }
    (void)memset(hdr, 0xFF, sizeof(hdr));
    (void)memcpy(hdr, &state, sizeof(state));
    (void)memset(&hdr[NVS_HDR_SEQ_AT], 0, sizeof(uint32_t));
    hdr[NVS_HDR_VERSION_AT] = NVS_VERSION_NEWER;
    crc = esp_rom_crc32_le(NVS_CRC_SEED, &hdr[NVS_HDR_SEQ_AT],
                           NVS_HDR_CRC_AT - NVS_HDR_SEQ_AT);
    (void)memcpy(&hdr[NVS_HDR_CRC_AT], &crc, sizeof(crc));
    if ( (esp_partition_erase_range(part, 0U, NVS_PAGE_BYTES) != ESP_OK)
        || (esp_partition_write(part, 0U, hdr, sizeof(hdr)) != ESP_OK) )
    {
        abort();
    }
}

void setUp(void)
{
}

void tearDown(void)
{
}

void app_main(void)
{
    nvs_from_newer_idf();
    UNITY_BEGIN();
    unity_run_all_tests();
    exit(UNITY_END());
}
