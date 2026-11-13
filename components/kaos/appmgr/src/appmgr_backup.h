/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file appmgr_backup.h
 * @brief Package-private: which installed apps have a copy on the card.
 */
#ifndef APPMGR_BACKUP_H_
#define APPMGR_BACKUP_H_

#include <stdbool.h>
#include <stdint.h>

#include "kapp_desc.h"   /* KAPP_BUILD_LEN */
#include "kapps_fmt.h"   /* KAPPS_MAX_ENTRIES */

/** @brief What is known of an installed app's copy on the card. */
typedef enum {
    APPMGR_BACKUP_UNSEEN = 0,  /**< The card has not been asked about it.       */
    APPMGR_BACKUP_COPIED,      /**< Its copy is on the card.                    */
    APPMGR_BACKUP_SKIPPED      /**< No copy, and one could not be made on this
                                *   card: not tried again until a card mounts.  */
} appmgr_backup_state_t;

/**
 * @brief The table: the apps something is known of, by install number.
 *
 * Kept in RAM alone: a card can be changed or taken out, and a mark in flash
 * would outlive the card it was true of.
 */
typedef struct {
    uint32_t seq[KAPPS_MAX_ENTRIES];    /**< Each app's install number.          */
    uint8_t  state[KAPPS_MAX_ENTRIES];  /**< Its appmgr_backup_state_t.          */
    uint32_t count;                     /**< Places in use.                      */
} appmgr_backup_t;

/**
 * @brief Make every app unseen: at boot, when a card mounts and when it goes.
 * @param t The table.
 */
void appmgr_backup_reset(appmgr_backup_t *t);

/**
 * @brief What is known of the app installed as @p seq.
 * @param t   The table.
 * @param seq The app's install number.
 * @return Its state; APPMGR_BACKUP_UNSEEN for an app the table does not hold.
 */
appmgr_backup_state_t appmgr_backup_get(const appmgr_backup_t *t, uint32_t seq);

/**
 * @brief Note what is known of the app installed as @p seq.
 *
 * APPMGR_BACKUP_UNSEEN forgets it. An app that finds the table full stays
 * unseen.
 *
 * @param t     The table.
 * @param seq   The app's install number.
 * @param state What is known now.
 */
void appmgr_backup_set(appmgr_backup_t *t, uint32_t seq, appmgr_backup_state_t state);

/**
 * @brief Drop the app installed as @p seq: it was erased or evicted.
 * @param t   The table.
 * @param seq The app's install number; one the table does not hold is ignored.
 */
void appmgr_backup_forget(appmgr_backup_t *t, uint32_t seq);

/**
 * @brief Whether a file on the card is the copy of an installed image.
 * @param file_len    The file's length in bytes.
 * @param file_build  The build id read from the file; KAPP_BUILD_LEN bytes.
 * @param image_len   The installed image's length in bytes.
 * @param image_build The installed image's build id; KAPP_BUILD_LEN bytes.
 * @return true at the same length and with the same build id.
 */
bool appmgr_backup_same(uint32_t file_len, const uint8_t *file_build,
                        uint32_t image_len, const uint8_t *image_build);

#endif /* APPMGR_BACKUP_H_ */
