/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_internal.h
 * @brief Not part of the API: the seam between the standard layer and kode_screen, its only users.
 */
#ifndef KODE_INTERNAL_H_
#define KODE_INTERNAL_H_

#ifndef KODE_INTERNAL_ALLOWED
#error "kode_internal.h is private to kode and kode_screen: an app uses kode.h"
#endif

#include <stdbool.h>
#include <stdint.h>

#include "kdrv_qmc5883p.h"
#include "kode.h"
#include "kqp.h"

/** @brief Blocks a screen request leaves in its event pool for the owners. */
#define KODE_SCREEN_POOL_MARGIN 8U

/** @brief The screen's half of the standard layer's Active Object; an app that never draws links none of it. */
typedef struct {
    bool (*prepare)(void);         /**< In the first screen call's task: take the memory; false when none.  */
    void (*attach)(QActive *ao);   /**< On the Active Object's thread, once, when the table is taken.      */
    bool (*event)(QEvt const *e);  /**< The panel's, the touch's and the screen's signals; true if handled. */
} kode_screen_ops_t;

/**
 * @brief Bring the layer up and hand its Active Object the screen's table, once.
 * @param ops The table; it must outlive the program.
 * @return true when posted; false with the task's error set.
 */
bool kode_standard_ao_screen_attach(const kode_screen_ops_t *ops);

/**
 * @brief Post one screen request, keeping room in the queue for what the owners publish.
 * @param e A KODE_SCREEN_REQ_SIG event from Q_NEW_X(); recycled when refused.
 * @return true when queued; false when the queue is too full.
 */
bool kode_standard_ao_screen_post(QEvt const *e);

/**
 * @brief The first thing every kode.h call does: refuse an Active Object, else
 *        bring the system up and clear the task's error.
 * @param name The public name of the call: the name a refusal carries.
 * @return true when the call may go on; false when it must return its failure value.
 */
bool kode_enter(const char *name);

/**
 * @brief Keep @p fmt, formatted, as the calling task's last error, at most
 *        KODE_ERROR_MAX bytes; a longer text is cut and ends in an ellipsis.
 * @param fmt printf-style format.
 */
void kode_err_set(const char *fmt, ...) KODE_PRINTF_FORMAT(1, 2);

/**
 * @brief Call @p attempt at once, then every KODE_RETRY_STEP_MS, until it answers
 *        true or @p bound_ms has passed.
 * @param attempt  One try; true ends the loop. NULL gives up.
 * @param ctx      Handed to @p attempt.
 * @param bound_ms How long to keep trying, in ms: 0 is one try, KODE_FOREVER never gives up.
 * @return true when an attempt answered true; false when the bound passed.
 */
bool kode_retry(bool (*attempt)(void *ctx), void *ctx, uint32_t bound_ms);

/**
 * @brief Read a whole file into a block the caller frees with khal_free(); the
 *        task's earlier file results stay valid.
 * @param name  The public call that reads: the name its errors carry.
 * @param path  "sd/..." or "flash/...".
 * @param block Out: the bytes, a NUL after the last; NULL for an empty file.
 * @param len   Out: the bytes in the file.
 * @return true on success; false with the task's error set.
 */
bool kode_file_read_owned(const char *name, const char *path, uint8_t **block, uint32_t *len);

/**
 * @brief One raw magnetometer reading: the part's axes, mG, no calibration.
 * @param name The public call that reads: the name its errors carry.
 * @param mg   Out: the field.
 * @return true when @p mg is filled; false with the task's error set.
 */
bool kode_mag_raw_read(const char *name, int32_t mg[KDRV_QMC5883P_AXES]);

/**
 * @brief Store the magnetometer's calibration, or clear it.
 * @param name The public call that stores: the name its errors carry.
 * @param corr The calibration; NULL clears it.
 * @return true when stored; false with the task's error set.
 */
bool kode_mag_cal_store(const char *name, const kdrv_qmc5883p_cal_t *corr);

/**
 * @brief @p c with an ASCII capital turned into its lower-case letter.
 * @param c Any character.
 * @return The lower-case letter for 'A' to 'Z'; @p c unchanged for the rest.
 */
char kode_ascii_lower(char c);

/**
 * @brief Whether @p name spells @p word, ignoring ASCII case: no trimming, no locale.
 * @param name What the caller typed.
 * @param word The name to match, already in lower case.
 * @return true when the two are the same text apart from case.
 */
bool kode_name_is(const char *name, const char *word);

#endif /* KODE_INTERNAL_H_ */
