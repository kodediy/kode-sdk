/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_qmi8658.h
 * @brief QST QMI8658A: the Kode Dot's six-axis IMU, as far as a production row needs.
 */
#ifndef KDRV_QMI8658_H_
#define KDRV_QMI8658_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What `WHO_AM_I` reads on every QMI8658. */
#define KDRV_QMI8658_WHOAMI 0x05U

/** @brief The least every axis must shift under the accelerometer's
 *         self-test, mg: the datasheet's own figure (section 15.1). */
#define KDRV_QMI8658_ST_XL_MIN_MG    200
/** @brief The same for the gyroscope, mdps (section 15.2: 300 dps). */
#define KDRV_QMI8658_ST_GY_MIN_MDPS  300000

/** @brief Samples in one FIFO window: the part's largest FIFO. */
#define KDRV_QMI8658_FIFO_DEPTH      128U
/** @brief The rate a window is sampled at, Hz. With the gyroscope off the
 *         accelerometer's rate code 0011 is 1000 Hz exactly, where with both
 *         on it is 896.8: the window turns the gyroscope off for that. */
#define KDRV_QMI8658_WITNESS_HZ      1000U
/** @brief A window's scale: +-8 g over sixteen bits, in micro-g per LSB. */
#define KDRV_QMI8658_UG_PER_LSB      244U

/** @brief Which of the part's two sensors. */
typedef enum {
    KDRV_QMI8658_XL = 0,  /**< The accelerometer. */
    KDRV_QMI8658_GY = 1   /**< The gyroscope.     */
} kdrv_qmi8658_sensor_t;

/** @brief The part's identity, as read. */
typedef struct {
    uint8_t whoami;    /**< Register 0x00; KDRV_QMI8658_WHOAMI on the part. */
    uint8_t revision;  /**< Register 0x01; 0x7C on the QMI8658A.            */
} kdrv_qmi8658_id_t;

/** @brief One sample, in the units the IMU row already speaks. */
typedef struct {
    int32_t a_mg[3];    /**< Acceleration per axis, mg.           */
    int32_t w_mdps[3];  /**< Rotation per axis, mdps.             */
    int32_t temp_dc;    /**< Die temperature, tenths of a degree. */
} kdrv_qmi8658_sample_t;

/**
 * @brief Find the part, check who it is, configure it and leave it at rest.
 *
 * +-8 g and +-512 dps at the part's 896.8 Hz rate, little-endian, register
 * auto-increment on, and both sensors left disabled: a part nobody is
 * reading converts for nobody, and kdrv_qmi8658_standby() is what its owner
 * starts it with.
 * INT1 is made a driven output, with nothing mapped to it: that pin is the
 * expander's P5, which has no pull-up, and a floating input there is
 * an interrupt source nobody asked for. INT2, wired to nothing, is driven
 * too; kdrv_qmi8658.c says why.
 *
 * May be called again after a failure; once it has succeeded, a further call
 * changes nothing.
 *
 * @param[out] id What the two identity registers read; filled whenever the
 *                part answered at all, whatever it answered, and with what
 *                bring-up read once the part is up. May be NULL.
 * @return KODE_OK when the part is a QMI8658 and took its configuration,
 *         KODE_IO_ERROR when nothing answers at KBSP_I2C_ADDR_IMU or a
 *         transfer failed, KODE_ERROR when something else answered there.
 */
kode_status_t kdrv_qmi8658_create(kdrv_qmi8658_id_t *id);

/**
 * @brief Leave the QMI8658A at rest, for a board with no owner for it.
 *
 * One probe, quiet, then the bring-up, which leaves the part at rest.
 *
 * @return KODE_OK with the part at rest; KODE_ERROR when nothing answered.
 */
kode_status_t kdrv_qmi8658_park(void);

/**
 * @brief Put both sensors at rest, or start them converting.
 *
 * Leaving standby enables the accelerometer and the gyroscope together at
 * the rates kdrv_qmi8658_create() set. The accelerometer has a sample within
 * a few periods; the gyroscope needs about 150 ms before its own means
 * anything, and waiting that out is the owner's business, not this call's.
 * In standby the output registers keep the last conversion, so a read there
 * is a stale one - the owner is what stops it.
 *
 * @param on true for standby, false to convert.
 * @return KODE_OK, KODE_DENIED before a successful kdrv_qmi8658_create(),
 *         KODE_IO_ERROR when the part did not take the write.
 */
kode_status_t kdrv_qmi8658_standby(bool on);

/**
 * @brief Read the latest sample: die temperature, acceleration, rotation.
 *
 * One fourteen-byte burst, so the three belong to the same instant.
 *
 * @param[out] out The sample.
 * @return KODE_OK, KODE_DENIED for a NULL argument or before a successful
 *         kdrv_qmi8658_create(), KODE_IO_ERROR when the transfer failed.
 */
kode_status_t kdrv_qmi8658_read(kdrv_qmi8658_sample_t *out);

/**
 * @brief Start one sensor's self-test (datasheet section 15).
 *
 * Both sensors are disabled, as the procedure asks, and the stimulus is
 * triggered: an electrostatic force inside the die, so the verdict does not
 * depend on how the board is lying or whether it is being carried. The part
 * then works on its own - about 25 ms for the accelerometer at this rate,
 * about 400 ms for the gyroscope - and kdrv_qmi8658_st_poll() says when.
 * Samples are meaningless until kdrv_qmi8658_st_end().
 *
 * @param which The sensor.
 * @return KODE_OK, KODE_DENIED before a successful kdrv_qmi8658_create(),
 *         KODE_IO_ERROR when a transfer failed.
 */
kode_status_t kdrv_qmi8658_st_begin(kdrv_qmi8658_sensor_t which);

/**
 * @brief Ask whether the self-test has finished, and collect it when it has.
 *
 * One register read while the part is still working. On the read that finds
 * it done, the trigger is cleared and the three shifts are read.
 *
 * @param which      The sensor kdrv_qmi8658_st_begin() was given.
 * @param[out] done  true once @p shift is filled.
 * @param[out] shift What each axis moved: mg for the accelerometer, mdps for
 *                   the gyroscope.
 * @return KODE_OK, KODE_DENIED for a NULL argument or before create,
 *         KODE_IO_ERROR when a transfer failed.
 */
kode_status_t kdrv_qmi8658_st_poll(kdrv_qmi8658_sensor_t which, bool *done,
                                   int32_t shift[3]);

/**
 * @brief Judge a self-test by the datasheet's own rule.
 *
 * @param which The sensor.
 * @param shift What kdrv_qmi8658_st_poll() reported.
 * @return true when every axis moved at least KDRV_QMI8658_ST_XL_MIN_MG or
 *         KDRV_QMI8658_ST_GY_MIN_MDPS, in either direction.
 */
bool kdrv_qmi8658_st_judge(kdrv_qmi8658_sensor_t which, const int32_t shift[3]);

/**
 * @brief Leave self-test: triggers cleared, scales rewritten, sensors on.
 *
 * Safe to call whether a self-test finished, timed out or never began. The
 * gyroscope needs about 150 ms after this before a sample means anything.
 *
 * @return KODE_OK, KODE_DENIED before create, KODE_IO_ERROR when the part
 *         would not take its configuration back.
 */
kode_status_t kdrv_qmi8658_st_end(void);

/**
 * @brief Start one FIFO window of the accelerometer.
 *
 * The gyroscope goes off, the FIFO is emptied and put in FIFO mode - it
 * fills once and stops - and the accelerometer fills it at
 * KDRV_QMI8658_WITNESS_HZ: KDRV_QMI8658_FIFO_DEPTH samples, 128 ms. Samples
 * read with kdrv_qmi8658_read() meanwhile carry no rotation.
 *
 * A failure leaves the sensors off, where the sequence stopped:
 * kdrv_qmi8658_fifo_disarm() is what puts the part back as it runs.
 *
 * @return KODE_OK, KODE_DENIED before create, KODE_IO_ERROR for the bus,
 *         KODE_TIMEOUT when the part did not acknowledge the FIFO reset.
 */
kode_status_t kdrv_qmi8658_fifo_arm(void);

/**
 * @brief Put the FIFO in read mode and say how many samples it holds.
 *
 * From here until kdrv_qmi8658_fifo_disarm() nothing new is stored, which is
 * what lets the samples come out a chunk at a time without the window moving
 * under the reader.
 *
 * @param[out] samples How many three-axis samples are waiting.
 * @return KODE_OK, KODE_DENIED for a NULL or before create, KODE_IO_ERROR,
 *         KODE_TIMEOUT when the part did not acknowledge the request.
 */
kode_status_t kdrv_qmi8658_fifo_open(size_t *samples);

/**
 * @brief Read @p n samples out of a FIFO kdrv_qmi8658_fifo_open() opened.
 *
 * One bus transaction of 6 * @p n bytes, so the caller sizes @p n to what
 * one of its steps may spend on the bus.
 *
 * @param[out] xyz 3 * @p n raw LSBs, x y z per sample, at
 *                 KDRV_QMI8658_UG_PER_LSB.
 * @param n        How many samples, at most 32.
 * @return KODE_OK, KODE_DENIED for a bad argument or before create,
 *         KODE_IO_ERROR for the bus.
 */
kode_status_t kdrv_qmi8658_fifo_read(int16_t *xyz, size_t n);

/**
 * @brief End the window: FIFO off, both sensors back on.
 *
 * Safe whether a window was armed, opened or neither.
 *
 * @return KODE_OK, KODE_DENIED before create, KODE_IO_ERROR when the part
 *         would not take its configuration back.
 */
kode_status_t kdrv_qmi8658_fifo_disarm(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_QMI8658_H_ */
