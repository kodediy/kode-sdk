/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_sd.h
 * @brief The microSD card, every register it answered with.
 */
#ifndef KDRV_SD_H_
#define KDRV_SD_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Every field of the card, in the order `sd_help` prints them. */
typedef enum {
    /* CID: who made it. */
    KDRV_SD_MFG_ID = 0,     /**< CID MID: manufacturer.                     */
    KDRV_SD_OEM_ID,         /**< CID OID: OEM or application.               */
    KDRV_SD_REVISION,       /**< CID PRV: product revision, BCD.            */
    KDRV_SD_SERIAL_HI,      /**< CID PSN, top 16 bits.                      */
    KDRV_SD_SERIAL_LO,      /**< CID PSN, bottom 16 bits.                   */
    KDRV_SD_MFG_YEAR,       /**< CID MDT, decoded.                          */
    KDRV_SD_MFG_MONTH,      /**< CID MDT, decoded.                          */

    /* CSD: how big it is and how fast it claims to be. */
    KDRV_SD_CSD_VER,        /**< CSD structure: 0 is SDSC, 1 SDHC/SDXC.     */
    KDRV_SD_CAPACITY_MB,    /**< Sectors times sector size.                 */
    KDRV_SD_SECTORS,        /**< Capacity in sectors: where the card ends.  */
    KDRV_SD_SECTOR_SIZE,    /**< Bytes per sector. 512 on every SDHC.       */
    KDRV_SD_READ_BLOCK_LEN, /**< Read block length in bytes.                */
    KDRV_SD_TR_SPEED_KHZ,   /**< The rate the CSD declares.                 */
    KDRV_SD_CCC,            /**< Card command classes, as a bitmap.         */

    /* SCR: what it supports. */
    KDRV_SD_SD_SPEC,        /**< Physical layer specification version.      */
    KDRV_SD_BUS_WIDTHS,     /**< Bit 0: 1-bit. Bit 2: 4-bit.                */
    KDRV_SD_ERASE_STATE,    /**< What an erased block reads back as.        */

    /* SSR: what an erase would cost. */
    KDRV_SD_AU_KB,          /**< Allocation unit.                           */
    KDRV_SD_ERASE_SIZE_AU,  /**< Erase size, in allocation units.           */
    KDRV_SD_ERASE_TIMEOUT_S,/**< Timeout for erasing one allocation unit.   */
    KDRV_SD_ERASE_OFFSET_S, /**< Constant added to any erase timeout.       */
    KDRV_SD_DISCARD,        /**< The card supports DISCARD.                 */
    KDRV_SD_FULE,           /**< Full User area Logical Erase.              */

    /* The link, as it was actually negotiated. */
    KDRV_SD_HIGH_CAPACITY,  /**< OCR CCS: addressed by block, not by byte.  */
    KDRV_SD_BUS_WIDTH,      /**< Data lines in use: 1 or 4. THE one to
                             *   watch - a card that came up on one line
                             *   has three that did not reach the slot.     */
    KDRV_SD_FREQ_KHZ,       /**< What the host clock ended up at.           */
    KDRV_SD_CARD_MAX_KHZ,   /**< What the card said it could take.          */
    KDRV_SD_RCA,            /**< Relative card address.                     */
    KDRV_SD_MMC,            /**< An MMC rather than an SD card.             */
    KDRV_SD_UHS1,           /**< UHS-I capable. Needs 1.8 V to be used.     */

    /* This driver's own state, answered with or without a card. */
    KDRV_SD_MOUNTED,        /**< Whether anything is mounted right now.     */
    KDRV_SD_HOSTED,         /**< Whether the next mount reuses esp-hosted's
                             *   controller.                                */

    /* The volume, asked afresh every time. */
    KDRV_SD_TOTAL_MB,       /**< How big the filesystem is.                 */
    KDRV_SD_FREE_MB,        /**< How much of it is unused.                  */

    /* What the next mount will ask for. Writable; neither touches the card. */
    KDRV_SD_WIDTH_REQ,      /**< Data lines to ask for: 1 or 4, nothing
                             *   between. Default KBSP_SD_WIDTH.            */
    KDRV_SD_FREQ_REQ_KHZ,   /**< Clock ceiling to ask for. Default
                             *   KBSP_SD_FREQ_KHZ.                          */

    KDRV_SD_FIELD_MAX
} kdrv_sd_field_t;

/** @brief What a field is, for whoever prints or validates one. */
typedef struct {
    const char *name;     /**< The table's name, lower case.                */
    const char *unit;     /**< "MB", "KB", "B", "kHz", "bits", "bool",
                           *   "code", "mask", "s", "512B", "year",
                           *   "month".                                     */
    int32_t     min;      /**< Smallest value the field can hold.           */
    int32_t     max;      /**< Largest.                                     */
    uint16_t    step;     /**< Resolution; always 1 here.                   */
    bool        writable; /**< True for the two knobs and nothing else.     */
} kdrv_sd_field_info_t;

/* ---- How long a call may take --------------------------------------------
 * The bounds a caller that runs these on a task of its own waits by. They are
 * judgements about the card, not measurements, and each is built from the one
 * under it.
 */

/** @brief The wait for the card's rail to rise behind Q1, before the first
 *         bring-up of a boot. */
#define KDRV_SD_POWER_SETTLE_MS   50U
/** @brief The most one try at the card may take: the identification and the
 *         volume's mount behind it, each command waiting out the host's own
 *         timeout. */
#define KDRV_SD_TRY_MAX_MS        3000U
/** @brief A mount: the rail, four data lines and then, when the card does
 *         not take them, one. */
#define KDRV_SD_MOUNT_MAX_MS      (KDRV_SD_POWER_SETTLE_MS + (2U * KDRV_SD_TRY_MAX_MS))
/** @brief A transfer of 32 KB of sectors, which is the largest one asked. */
#define KDRV_SD_SECTORS_MAX_MS    5000U
/** @brief A format of the largest card the slot takes, with the volume's size
 *         read back. */
#define KDRV_SD_FORMAT_MAX_MS     120000U

/* ---- Mounting ------------------------------------------------------------ */

/**
 * @brief Mount the card, four data lines first and then one.
 *
 * The retry is not a policy, it is what makes a counterfeit card readable: the
 * cards shipped with some units advertise 4-bit in their SCR and then fail
 * 4-bit writes. Reads work either way. What a 1-bit mount MEANS is the
 * caller's to judge - the production test fails on it, because on a healthy
 * board all four lines reach the slot.
 *
 * Idempotent: a second call on a mounted card just answers where it is.
 *
 * While the link holds the controller and is down, nothing is tried: NULL,
 * at debug level, and the card is asked again on the next call. After a miss
 * that was reported, the repeat probes hold ESP-IDF's account of the empty
 * slot quiet until a card mounts.
 *
 * @return The mount point, or NULL when there is no card. On failure the
 *         reason is logged, including the one that is nearly always right:
 *         the card's supply is the ESP32-C5's to switch.
 */
const char *kdrv_sd_mount(void);

/**
 * @brief Switch mount mode: true = reuse the SDMMC controller esp-hosted
 *        already owns (radio link up), false = the plain path (radio off).
 *
 * AO_Radio's to call, around the link's lifetime, with the card unmounted:
 * the mode decides what the *next* mount does, it does not migrate a live
 * one.
 */
void kdrv_sd_set_hosted(bool hosted);

/** @brief Unmount the card. Safe with nothing mounted. */
void kdrv_sd_unmount(void);

/**
 * @brief Switch the card's supply off (SD_PWR high, Q1 off), for a board
 *        whose storage owner the app did not link. The next bring-up powers
 *        it again and waits it out.
 * @return KODE_OK, or KODE_ERROR when the pin would not drive.
 */
kode_status_t kdrv_sd_park(void);

/** @brief Whether a card is currently mounted. */
bool kdrv_sd_mounted(void);

/** @brief The card's mount point string, mounted or not. */
const char *kdrv_sd_mount_point(void);

/** @brief FAT usage of the mounted card. KODE_DENIED when no card. */
kode_status_t kdrv_sd_usage(uint64_t *total, uint64_t *free_bytes);

/* ---- The table ----------------------------------------------------------- */

/**
 * @brief Read one field.
 *
 * @param[in]  f     Which field.
 * @param[out] value Its value, in the unit field_info() names. Untouched
 *                   unless KODE_OK is returned.
 * @return KODE_OK; KODE_DENIED for an unknown field, a NULL pointer, or a
 *         field of a card when none is mounted; KODE_ERROR when the volume
 *         itself will not answer.
 */
kode_status_t kdrv_sd_get(kdrv_sd_field_t f, int32_t *value);

/**
 * @brief Write one field.
 *
 * Only the two next-mount knobs can be written, and neither reaches the card:
 * they are read by the next kdrv_sd_mount(). `width_req` takes 1 or 4 and
 * nothing between, because those are the only widths an SD slot has.
 *
 * @param[in] f     Which field.
 * @param[in] value Its new value, in the field's unit.
 * @return KODE_OK; KODE_DENIED for an unknown field, a read-only one, or a
 *         value the field cannot hold.
 */
kode_status_t kdrv_sd_set(kdrv_sd_field_t f, int32_t value);

/** @brief What @p f is. NULL for a field that is not in the table. */
const kdrv_sd_field_info_t *kdrv_sd_field_info(kdrv_sd_field_t f);

/**
 * @brief Resolve a field's name.
 * @param[in]  name The table's spelling, lower case.
 * @param[out] f    The field. Untouched unless KODE_OK is returned.
 * @return KODE_OK, or KODE_DENIED when nothing is called that.
 */
kode_status_t kdrv_sd_field_by_name(const char *name, kdrv_sd_field_t *f);

/** @brief Log the whole table, as name=value pairs. */
void kdrv_sd_dump(void);

/* ---- What the table cannot say ------------------------------------------- */

/**
 * @brief The card's product name, the eight characters of its CID.
 * @param[out] out Buffer for it, NUL-terminated.
 * @param[in]  len Its size. Nine bytes is enough for any card.
 * @return KODE_OK, or KODE_DENIED with no card or no room.
 */
kode_status_t kdrv_sd_name(char *out, size_t len);

/**
 * @brief Make a fresh, empty filesystem on the card.
 *
 * Every file on it becomes unreachable. Nothing may be open.
 *
 * @return KODE_OK with the card still mounted and empty; KODE_DENIED with no
 *         card; KODE_ERROR with the card NO LONGER MOUNTED - a format that
 *         fails cannot leave a volume behind to talk to, so the caller must
 *         mount again before anything else.
 */
kode_status_t kdrv_sd_format(void);

/**
 * @brief Read whole sectors, past the filesystem, on a mounted card.
 *
 * @param[in]  first First sector, from the start of the card.
 * @param[in]  count How many.
 * @param[out] dst   At least count times the card's sector size.
 * @return KODE_OK; KODE_DENIED with no card or a NULL buffer; KODE_ERROR when
 *         the card refused, which includes reading past its capacity.
 */
kode_status_t kdrv_sd_read_sectors(uint32_t first, uint32_t count, void *dst);

/**
 * @brief Write whole sectors, past the filesystem.
 *
 * On a mounted card this writes over whatever the filesystem had there and the
 * filesystem will not notice: only the production test does that, to a card
 * about to be formatted, to find out whether its far end is real.
 *
 * @param[in] first First sector, from the start of the card.
 * @param[in] count How many.
 * @param[in] src   At least count times the card's sector size.
 * @return KODE_OK; KODE_DENIED with no card or a NULL buffer; KODE_ERROR when
 *         the card refused, which includes writing past its capacity.
 */
kode_status_t kdrv_sd_write_sectors(uint32_t first, uint32_t count,
                                    const void *src);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_SD_H_ */
