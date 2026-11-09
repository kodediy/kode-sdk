/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_qmc5883p.h
 * @brief QST QMC5883P: the Kode Dot's magnetometer, as far as a production row needs.
 */
#ifndef KDRV_QMC5883P_H_
#define KDRV_QMC5883P_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Axes of the part, and of every vector the compass handles. */
#define KDRV_QMC5883P_AXES 3U

/** @brief What the chip-id register reads on a QMC5883P. */
#define KDRV_QMC5883P_CHIP_ID 0x80U

/* ---- The calibration ----------------------------------------------------
 * The part's own field and the board's are in every reading, so a raw one is
 * no compass. The correction is a diagonal one: an offset per axis (hard
 * iron) and a scale per axis (soft iron), measured by turning the board in
 * every direction. The limits keep its text within one setting's value.
 */
#define KDRV_QMC5883P_CAL_OFFSET_MAX_MG  9999   /**< Largest offset either way, mG.      */
#define KDRV_QMC5883P_CAL_SCALE_UNIT     1000   /**< A scale of 1.0, in thousandths.     */
#define KDRV_QMC5883P_CAL_SCALE_MIN      250    /**< Smallest scale, in thousandths.     */
#define KDRV_QMC5883P_CAL_SCALE_MAX      4000   /**< Largest scale, in thousandths.      */
#define KDRV_QMC5883P_CAL_TEXT_MAX       32U    /**< Longest text, NUL not counted: three
                                                 *   "-9999", three "4000", five spaces.  */

/** @brief A magnetometer calibration. */
typedef struct {
    int32_t offset_mg[KDRV_QMC5883P_AXES];    /**< Subtracted from the raw reading, mG.       */
    int32_t scale_milli[KDRV_QMC5883P_AXES];  /**< Multiplies what is left, in thousandths.   */
} kdrv_qmc5883p_cal_t;

/**
 * @brief Read a calibration from its text, "ox oy oz sx sy sz".
 * @param text Six numbers split by one space; offsets within
 *             KDRV_QMC5883P_CAL_OFFSET_MAX_MG, scales within the SCALE limits.
 * @param[out] corr Written on true only.
 * @return true when @p text is exactly such a calibration.
 */
bool kdrv_qmc5883p_cal_parse(const char *text, kdrv_qmc5883p_cal_t *corr);

/**
 * @brief Write a calibration as the text kdrv_qmc5883p_cal_parse() reads.
 * @param corr The calibration.
 * @param[out] out Where the text goes, NUL ended.
 * @param cap Its size.
 * @return The text's length; 0 when @p cap is too small, an argument is NULL
 *         or a number is out of its limits.
 */
size_t kdrv_qmc5883p_cal_format(const kdrv_qmc5883p_cal_t *corr, char *out, size_t cap);

/**
 * @brief Correct a raw reading.
 * @param corr The calibration.
 * @param raw_mg The part's reading, mG.
 * @param[out] out_mg The corrected field, mG.
 */
void kdrv_qmc5883p_cal_apply(const kdrv_qmc5883p_cal_t *corr,
                             const int32_t raw_mg[KDRV_QMC5883P_AXES],
                             float out_mg[KDRV_QMC5883P_AXES]);

/**
 * @brief Work a calibration out of the least and greatest reading on each axis.
 *
 * The offset is the middle of the range; the scale makes each axis's half
 * range the average of the three, clamped to the SCALE limits.
 *
 * @param lo The least reading per axis, mG.
 * @param hi The greatest reading per axis, mG.
 * @param[out] corr Written on KODE_OK only.
 * @return KODE_OK; KODE_DENIED for a NULL or an axis whose @p hi is not above
 *         its @p lo; KODE_ERROR when an offset is past the OFFSET limit.
 */
kode_status_t kdrv_qmc5883p_cal_fit(const int32_t lo[KDRV_QMC5883P_AXES],
                                    const int32_t hi[KDRV_QMC5883P_AXES],
                                    kdrv_qmc5883p_cal_t *corr);

/**
 * @brief Find the part, check who it is, configure it and leave it at rest.
 *
 * The +-30 G range, where one LSB is one milligauss: the board's own field
 * measures several gauss, and a range that clips is a reading that lies.
 * The part is left suspended; kdrv_qmc5883p_standby() is what its owner
 * starts it with.
 *
 * May be called again after a failure; once it has succeeded a further call
 * changes nothing.
 *
 * @param[out] chip_id What the chip-id register read; filled whenever the
 *                     part answered at all, and with what bring-up read once
 *                     the part is up. May be NULL.
 * @return KODE_OK when the part is a QMC5883P and took its configuration,
 *         KODE_IO_ERROR when nothing answers at KBSP_I2C_ADDR_MAG or a
 *         transfer failed, KODE_ERROR when something else answered there.
 */
kode_status_t kdrv_qmc5883p_create(uint8_t *chip_id);

/**
 * @brief Leave the QMC5883P suspended, for a board with no owner for it.
 *
 * One probe, quiet, then the bring-up, which leaves the part suspended.
 *
 * @return KODE_OK with the part suspended; KODE_ERROR when nothing answered.
 */
kode_status_t kdrv_qmc5883p_park(void);

/**
 * @brief Whether the QMC5883P answers at KBSP_I2C_ADDR_MAG_STRAY instead.
 *
 * The part reads its I2C address from its NVM at power-on (datasheet 5.3).
 * A Kode Dot whose part had come up at 0x0C, bit 5 of 0x2C missing, answered
 * there with the chip id 0x80 until its supply was cut; a soft reset did not
 * bring it back. This is the question a caller asks once nothing answered at
 * KBSP_I2C_ADDR_MAG, so that the answer can say so.
 *
 * Reads the chip id and nothing else; the part is not configured there.
 *
 * @return true when a QMC5883P's chip id answers at KBSP_I2C_ADDR_MAG_STRAY.
 */
bool kdrv_qmc5883p_stray(void);

/**
 * @brief Suspend the part, or start it converting.
 *
 * Out of standby it converts continuously at 50 Hz, so its first conversion
 * is one period away; kdrv_qmc5883p_read() waits that out once by itself.
 *
 * @param on true to suspend, false to convert.
 * @return KODE_OK, KODE_DENIED before a successful kdrv_qmc5883p_create(),
 *         KODE_IO_ERROR when the transfer failed.
 */
kode_status_t kdrv_qmc5883p_standby(bool on);

/**
 * @brief Read the latest conversion.
 *
 * @param[out] mg The field on the chip's own three axes, milligauss.
 * @return KODE_OK, KODE_DENIED for a NULL argument or before a successful
 *         kdrv_qmc5883p_create(), KODE_IO_ERROR when the transfer failed,
 *         KODE_ERROR when no conversion is ready or one overflowed.
 */
kode_status_t kdrv_qmc5883p_read(int32_t mg[KDRV_QMC5883P_AXES]);

/**
 * @brief Run the part's self-test and report what each axis moved.
 *
 * A reading with the stimulus off, the stimulus, a reading with it on. The
 * datasheet describes the procedure and gives no threshold ("compare with
 * threshold value"), so this reports and does not judge: the number to judge
 * by has to be measured on known-good boards first. Blocks for about 10 ms,
 * and leaves the part converting as kdrv_qmc5883p_create() left it.
 *
 * @param[out] delta_mg The datasheet's three deltas, milligauss: `off - on`
 *                      for X and `on - off` for Y and Z, as section 7.3
 *                      writes them.
 * @return KODE_OK, KODE_DENIED for a NULL argument or before create,
 *         KODE_ERROR when no conversion was ready to compare against,
 *         KODE_IO_ERROR when a transfer failed.
 */
kode_status_t kdrv_qmc5883p_self_test(int32_t delta_mg[KDRV_QMC5883P_AXES]);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_QMC5883P_H_ */
