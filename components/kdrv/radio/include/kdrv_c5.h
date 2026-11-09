/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_c5.h
 * @brief The ESP32-C5 radio coprocessor, everything the link can say about it.
 */
#ifndef KDRV_C5_H_
#define KDRV_C5_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The project name a Kode build of the slave carries in its image
 *         descriptor. Stock esp-hosted says `network_adapter`, and until the
 *         slave was renamed the two were indistinguishable over the link. */
#define KDRV_C5_PROJECT_NAME  "kode_c5"

/** @brief esp_chip_model_t's value for an ESP32-C5. Spelled here so the
 *         table's contract does not depend on an ESP-IDF header; the .c
 *         asserts it against the real enum. */
#define KDRV_C5_CHIP_ESP32C5  23

/** @brief Every field of the coprocessor, in the order `c5_help` prints them. */
typedef enum {
    /* This driver's own state: always answers. */
    KDRV_C5_VALID = 0,      /**< A refresh has succeeded since the last forget. */

    /* Identity: what the slave answered at the last refresh. */
    KDRV_C5_CHIP_ID,        /**< esp_chip_model_t; 23 is an ESP32-C5.         */
    KDRV_C5_FW_MAJOR,       /**< The esp-hosted version the slave runs.       */
    KDRV_C5_FW_MINOR,
    KDRV_C5_FW_PATCH,

    /* The esp-hosted version this firmware was built against: always answers,
     * because it is a fact about the P4's image, not the C5's. The row passes
     * when the two agree. */
    KDRV_C5_HOST_MAJOR,
    KDRV_C5_HOST_MINOR,
    KDRV_C5_HOST_PATCH,

    /* The image descriptor. `app_desc` always answers - it says whether the
     * slave answered that RPC - and `project_kode` is refused when it did not. */
    KDRV_C5_APP_DESC,       /**< The slave answered the descriptor RPC.       */
    KDRV_C5_PROJECT_KODE,   /**< Its project name is KDRV_C5_PROJECT_NAME.     */

    /* The station MAC, two bytes a row: the unit's radio identity. */
    KDRV_C5_MAC_HI,         /**< Bytes 0 and 1.                                */
    KDRV_C5_MAC_MID,        /**< Bytes 2 and 3.                                */
    KDRV_C5_MAC_LO,         /**< Bytes 4 and 5.                                */

    /* What the link itself reports: always answers. */
    KDRV_C5_RESET_REASON,   /**< esp_reset_reason_t of the C5's last boot, as
                             *   carried on CP_INIT; 0 before any link-up.    */
    KDRV_C5_LINK_MS,        /**< Connect to CP_INIT, in ms, at the last
                             *   bring-up; 0 before any.                       */

    /* The slave's heap, from the one-shot memory monitor. */
    KDRV_C5_HEAP_FREE_KB,   /**< Free internal heap.                           */
    KDRV_C5_HEAP_LARGEST_KB,/**< Its largest free block.                       */

    /* What the SDIO transport negotiated. */
    KDRV_C5_SDIO_KHZ,       /**< The clock the host ended up at.               */
    KDRV_C5_SDIO_WIDTH,     /**< Data lines in use: 1 or 4. The one to watch - a
                             *   link on one line has three that did not
                             *   reach the C5.                                 */

    /* The one card pin the C5 reads on the P4's behalf. */
    KDRV_C5_SD_CD,          /**< IO6: the card-detect level, as read. Its
                             *   polarity is not board-verified, so it is a
                             *   reading and never a verdict.                  */

    /* The expansion header's supply, as the pin reads. */
    KDRV_C5_PWR_EXT,        /**< IO5: the enable of J3's +3.3V_EXT, 1 = on.
                             *   Read at every refresh and after every
                             *   kdrv_c5_exp_power().                          */

    KDRV_C5_FIELD_MAX
} kdrv_c5_field_t;

/** @brief What a field is, for whoever prints or validates one. */
typedef struct {
    const char *name;     /**< The table's name, lower case.                  */
    const char *unit;     /**< "bool", "code", "kHz", "bits", "KB", "ms".     */
    int32_t     min;      /**< Smallest value the field can hold.             */
    int32_t     max;      /**< Largest.                                       */
    uint16_t    step;     /**< Resolution; always 1 here.                     */
    bool        writable; /**< false for every row: the table has no knobs.   */
} kdrv_c5_field_info_t;

/* ---- The table ----------------------------------------------------------- */

/**
 * @brief Read one field.
 *
 * @param[in]  f     Which field.
 * @param[out] value Its value, in the unit field_info() names. Untouched
 *                   unless KODE_OK is returned.
 * @return KODE_OK; KODE_DENIED for an unknown field, a NULL pointer, or a row
 *         whose group the last refresh did not fill.
 */
kode_status_t kdrv_c5_get(kdrv_c5_field_t f, int32_t *value);

/**
 * @brief Write one field.
 *
 * Nothing here can be written: every row is a reading. The call exists so
 * the owner's field handler has the shape every other chip's has, and it
 * refuses every row the same way.
 *
 * @return KODE_DENIED, always.
 */
kode_status_t kdrv_c5_set(kdrv_c5_field_t f, int32_t value);

/** @brief What @p f is. NULL for a field that is not in the table. */
const kdrv_c5_field_info_t *kdrv_c5_field_info(kdrv_c5_field_t f);

/**
 * @brief Resolve a field's name.
 * @param[in]  name The table's spelling, lower case.
 * @param[out] f    The field. Untouched unless KODE_OK is returned.
 * @return KODE_OK, or KODE_DENIED when nothing is called that.
 */
kode_status_t kdrv_c5_field_by_name(const char *name, kdrv_c5_field_t *f);

/** @brief Log the whole table as name=value pairs, and the strings after it. */
void kdrv_c5_dump(void);

/* ---- What the table cannot say ------------------------------------------- */

/** @brief The chip's target name ("esp32c5"). KODE_DENIED before a refresh. */
kode_status_t kdrv_c5_target(char *out, size_t len);

/** @brief The image's project name. KODE_DENIED without a descriptor. */
kode_status_t kdrv_c5_project_name(char *out, size_t len);

/** @brief The image's own version string. KODE_DENIED without a descriptor. */
kode_status_t kdrv_c5_app_version(char *out, size_t len);

/** @brief The IDF the image was built with. KODE_DENIED without a descriptor. */
kode_status_t kdrv_c5_idf_ver(char *out, size_t len);

/** @brief The image's build date. KODE_DENIED when the slave did not send it. */
kode_status_t kdrv_c5_build_date(char *out, size_t len);

/** @brief The image's build time. KODE_DENIED when the slave did not send it. */
kode_status_t kdrv_c5_build_time(char *out, size_t len);

/** @brief The image's ELF sha256. KODE_DENIED when the slave did not send it. */
kode_status_t kdrv_c5_elf_sha(uint8_t out[32]);

/** @brief The station MAC, six bytes. KODE_DENIED before a refresh. */
kode_status_t kdrv_c5_mac(uint8_t out[6]);

/* ---- The worker's half ---------------------------------------------------- */

/**
 * @brief Take a fresh snapshot of the coprocessor. Blocks: worker only.
 *
 * Chip info, esp-hosted version, MAC and the transport's configuration are
 * the identity, and a failure among them fails the refresh and leaves the
 * snapshot invalid. The descriptor, the heap and the two pins are each their
 * own group: one that does not answer is recorded as absent and the refresh
 * still succeeds, because an older slave without the descriptor RPC is still
 * a C5 with a version and a MAC.
 *
 * @return KODE_OK, or KODE_ERROR when the identity could not be read.
 */
kode_status_t kdrv_c5_refresh(void);

/**
 * @brief Drive the expansion header's supply enable. Blocks: worker only.
 *
 * IO5 of the C5 is the enable of U18, the regulator behind J3's +3.3V_EXT,
 * and R72 pulls it up: a C5 that has not been told otherwise - one that has
 * just booted, or one held in reset - leaves the rail on. This drives the
 * pin through the RPC GPIO service, so it needs the link, and the C5 holds
 * the level until it is told again or reboots. The level read back is what
 * KDRV_C5_PWR_EXT says afterwards.
 *
 * @param on true for the rail on, false for off.
 * @return KODE_OK, or KODE_ERROR when the C5 would not take the pin.
 */
kode_status_t kdrv_c5_exp_power(bool on);

/**
 * @brief Drive the speaker amplifier's enable, the C5's IO4 (the NS4150B).
 *
 * The C5's firmware raises it at its own boot; this is the way to lower it
 * for a board with no sound, and to raise it again. One RPC over the link,
 * so it blocks: AO_Radio's worker runs it as a job.
 *
 * @param on true enables the amplifier.
 * @return KODE_OK, or KODE_ERROR when the RPC GPIO service refused.
 */
kode_status_t kdrv_c5_amp(bool on);

/**
 * @brief Forget the snapshot: the link is gone or going.
 *
 * Every group is cleared. The two facts the link reported - the reset reason
 * and the bring-up time - stay, since they are about the last link-up and
 * not about a conversation that has ended.
 */
void kdrv_c5_forget(void);

/**
 * @brief Record why the C5 booted, as CP_INIT carried it. Any thread.
 * @param reason esp_reset_reason_t, as an integer.
 */
void kdrv_c5_note_reset(int32_t reason);

/**
 * @brief Record how long the last bring-up took. Any thread.
 * @param ms Connect to CP_INIT, in milliseconds.
 */
void kdrv_c5_note_link_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_C5_H_ */
