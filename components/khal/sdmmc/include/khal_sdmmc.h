/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_sdmmc.h
 * @brief The microSD: its SDMMC slot, its FAT volume and its raw sectors.
 */
#ifndef KHAL_SDMMC_H_
#define KHAL_SDMMC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KHAL_SDMMC_NAME_LEN  8U   /**< Bytes of the product name, NUL included. */

/** @brief The card's registers, copied once per mount. */
typedef struct {
    /* CID: who made it. */
    uint8_t  mfg_id;         /**< Manufacturer ID (MID).                     */
    uint16_t oem_id;         /**< OEM/application ID (OID).                  */
    char     name[KHAL_SDMMC_NAME_LEN]; /**< Product name (PNM), NUL-terminated. */
    uint8_t  revision;       /**< Product revision (PRV).                    */
    uint32_t serial;         /**< Product serial number (PSN).               */
    uint16_t date;           /**< Manufacturing date (MDT), raw: year - 2000
                              *   in bits 11:4, month in bits 3:0.           */

    /* CSD: how big it is and how fast it says it goes. */
    uint8_t  csd_ver;        /**< CSD structure version: 0 is SDSC, 1 SDHC.  */
    uint32_t sectors;        /**< Capacity, in sectors.                      */
    uint16_t sector_size;    /**< Sector size in bytes. 512 on every SDHC.   */
    uint16_t read_block_len; /**< Read block length in bytes.                */
    uint16_t ccc;            /**< Card command classes bitmap.               */
    uint32_t tr_speed_khz;   /**< Maximum transfer rate the CSD declares.    */

    /* SCR: what the card supports. */
    uint8_t  sd_spec;        /**< Physical layer specification version.      */
    uint8_t  bus_widths;     /**< Bit 0: 1-bit. Bit 2: 4-bit.                */
    bool     erase_state;    /**< What an erased block reads back as.        */

    /* SSR: what an erase would cost. */
    uint16_t au_kb;          /**< Allocation unit, in KB.                    */
    uint16_t erase_size_au;  /**< Erase size, in allocation units.           */
    uint8_t  erase_timeout_s;/**< Timeout for erasing one allocation unit.   */
    uint8_t  erase_offset_s; /**< Constant added to any erase timeout.       */
    bool     discard;        /**< The card supports DISCARD.                 */
    bool     fule;           /**< The card supports Full User area Logical
                              *   Erase.                                     */

    /* The link, as it was actually negotiated. */
    bool     high_capacity;  /**< OCR CCS: SDHC/SDXC, addressed by block.    */
    uint8_t  bus_width;      /**< Data lines in use: 1 or 4. Not the log.    */
    uint32_t freq_khz;       /**< What the host clock ended up at.           */
    uint32_t max_freq_khz;   /**< What the card said it could take.          */
    uint16_t rca;            /**< Relative card address.                     */
    bool     is_mmc;         /**< An MMC rather than an SD card.             */
    bool     is_uhs1;        /**< UHS-I capable. Needs 1.8 V to be used.     */
} khal_sdmmc_card_t;

/**
 * @brief Power the slot, identify the card and mount its FAT volume. Never formats.
 * @param[in]  width   Data lines to ask for: 1 or 4. Retrying at 1 is the caller's.
 * @param[in]  max_khz Ceiling for the host clock, in kHz.
 * @param[out] out     The card's registers, written only on KODE_OK. May be NULL.
 * @return KODE_OK; KODE_DENIED, with nothing tried, for a bad argument, a
 *         card already mounted, or a controller that khal_sdmmc_ready()
 *         says is not there; KODE_TIMEOUT when no card answered;
 *         KODE_IO_ERROR when a card answered and has no FAT volume to mount
 *         (exFAT, or none); KODE_ERROR for any other failure. Only the try
 *         is logged here, at debug level: the caller tells the outcome.
 */
kode_status_t khal_sdmmc_mount(uint8_t width, uint32_t max_khz,
                               khal_sdmmc_card_t *out);

/**
 * @brief Whether a mount has a controller to join.
 *
 * In hosted mode the controller is esp-hosted's: the card's slot is added to
 * the one the link's slot created, and the controller goes with the link's
 * slot when the card's is not there. Between that teardown and the next link a
 * mount has no controller to add its slot to, and the errors ESP-IDF logs for
 * that are no answer about the card.
 *
 * @return false when the mount is hosted and the link's slot is not on the
 *         controller; true otherwise.
 */
bool khal_sdmmc_ready(void);

/**
 * @brief Where the card's files appear.
 * @return The mount point, the same mounted or not.
 */
const char *khal_sdmmc_mount_point(void);

/** @brief Unmount the volume and release the slot. Safe with nothing mounted. */
void khal_sdmmc_unmount(void);

/**
 * @brief Whether a card is mounted.
 * @return true while a volume is mounted.
 */
bool khal_sdmmc_mounted(void);

/**
 * @brief Choose whether the next mount reuses the controller esp-hosted owns.
 * @param hosted true to reuse it, false to take the controller. Call with the
 *               card unmounted: a live mount is never moved.
 */
void khal_sdmmc_set_hosted(bool hosted);

/**
 * @brief Whether the next mount reuses the controller esp-hosted owns.
 * @return true at boot whenever esp-hosted is compiled in.
 */
bool khal_sdmmc_hosted(void);

/**
 * @brief How big the mounted volume is and how much of it is free.
 * @param[out] total      Bytes the filesystem spans. May be NULL.
 * @param[out] free_bytes Bytes still unused. May be NULL.
 * @return KODE_OK; KODE_DENIED with nothing mounted; KODE_ERROR when the
 *         filesystem will not answer.
 */
kode_status_t khal_sdmmc_info(uint64_t *total, uint64_t *free_bytes);

/**
 * @brief Make a fresh, empty FAT volume on the mounted card.
 * @return KODE_OK with the card still mounted and empty; KODE_DENIED with
 *         nothing mounted; KODE_ERROR with NOTHING MOUNTED, and the caller
 *         mounts again.
 */
kode_status_t khal_sdmmc_format(void);

/**
 * @brief Read whole sectors, past the filesystem, on a mounted card.
 * @param[in]  first First sector, counted from the start of the card.
 * @param[in]  count How many.
 * @param[out] dst   At least count * sector_size bytes.
 * @return KODE_OK; KODE_DENIED with no card or a bad argument; KODE_ERROR when
 *         the card refused, past its capacity included.
 */
kode_status_t khal_sdmmc_read_sectors(uint32_t first, uint32_t count,
                                      void *dst);

/**
 * @brief Write whole sectors, past the filesystem.
 *
 * On a mounted card this overwrites what the filesystem holds without telling
 * it: only on a card about to be formatted, with no file open.
 *
 * @param[in] first First sector, counted from the start of the card.
 * @param[in] count How many.
 * @param[in] src   At least count * sector_size bytes.
 * @return KODE_OK; KODE_DENIED with no card or a bad argument; KODE_ERROR when
 *         the card refused, past its capacity included.
 */
kode_status_t khal_sdmmc_write_sectors(uint32_t first, uint32_t count,
                                       const void *src);

#ifdef __cplusplus
}
#endif

#endif /* KHAL_SDMMC_H_ */
