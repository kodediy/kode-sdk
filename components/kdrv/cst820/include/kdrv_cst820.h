/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_cst820.h
 * @brief The CST820 capacitive touch controller at 0x15, every register of it.
 */
#ifndef KDRV_CST820_H_
#define KDRV_CST820_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief What register 0xA7 reads on a CST820.
 *
 * Pinned from the bench, not from a datasheet: the first bring-up logs the
 * three identity bytes, and this is what the panel supplier's part said
 * (board e8:f6:0a:e7:6c:1d, 2026-09-04: chip_id=0xB7 proj_id=0x28
 * fw_ver=0xC3).
 */
#define KDRV_CST820_CHIP_ID  0xB7U

/**
 * @brief Every field of the map: the frame, the identity, then each
 *        configuration register whole and by its named bits.
 */
typedef enum {
    KDRV_CST_GESTURE = 0,       /**< 0x01: the last gesture (0 none, 1 up, 2 down, 3 left, 4 right, 5 click, 0x0B double, 0x0C long). */
    KDRV_CST_FINGERS,           /**< 0x02: how many fingers are down, 0 or 1.        */
    KDRV_CST_EVENT,             /**< 0x03[7:6]: 0 down, 1 up, 2 contact.             */
    KDRV_CST_X,                 /**< 0x03[3:0] << 8 | 0x04: x, in panel pixels.      */
    KDRV_CST_Y,                 /**< 0x05[3:0] << 8 | 0x06: y, in panel pixels.      */
    KDRV_CST_CHIP_ID,           /**< 0xA7: the part.                                 */
    KDRV_CST_PROJ_ID,           /**< 0xA8: the panel supplier's project.             */
    KDRV_CST_FW_VER,            /**< 0xA9: the controller's firmware.                */
    KDRV_CST_MOTION_MASK,       /**< 0xEC: which gestures are reported.              */
    KDRV_CST_EN_DCLICK,         /**< 0xEC[0]: double click.                          */
    KDRV_CST_EN_CON_UD,         /**< 0xEC[1]: continuous up/down slide.              */
    KDRV_CST_EN_CON_LR,         /**< 0xEC[2]: continuous left/right slide.           */
    KDRV_CST_IRQ_PULSE_WIDTH,   /**< 0xED: INT low pulse, in 0.1 ms.                 */
    KDRV_CST_NOR_SCAN_PER,      /**< 0xEE: normal-mode scan period, in 10 ms.        */
    KDRV_CST_MOTION_SL_ANGLE,   /**< 0xEF: the slide-direction angle threshold.      */
    KDRV_CST_LP_SCAN_RAW1,      /**< 0xF0 << 8 | 0xF1: low-power channel 1 raw.      */
    KDRV_CST_LP_SCAN_RAW2,      /**< 0xF2 << 8 | 0xF3: low-power channel 2 raw.      */
    KDRV_CST_LP_AUTO_WAKE_TIME, /**< 0xF4: low-power auto-recalibration, in minutes. */
    KDRV_CST_LP_SCAN_TH,        /**< 0xF5: low-power wake threshold.                 */
    KDRV_CST_LP_SCAN_WIN,       /**< 0xF6: low-power scan window.                    */
    KDRV_CST_LP_SCAN_FREQ,      /**< 0xF7: low-power scan frequency.                 */
    KDRV_CST_LP_SCAN_IDAC,      /**< 0xF8: low-power scan current.                   */
    KDRV_CST_AUTO_SLEEP_TIME,   /**< 0xF9: idle seconds before the chip sleeps.      */
    KDRV_CST_IRQ_CTL,           /**< 0xFA: what pulses INT.                          */
    KDRV_CST_EN_TEST,           /**< 0xFA[7]: INT pulses periodically, a test.       */
    KDRV_CST_EN_TOUCH,          /**< 0xFA[6]: INT pulses on every touch report.      */
    KDRV_CST_EN_CHANGE,         /**< 0xFA[5]: INT pulses on a change of touch state. */
    KDRV_CST_EN_MOTION,         /**< 0xFA[4]: INT pulses on a gesture.               */
    KDRV_CST_ONCE_WLP,          /**< 0xFA[0]: one long-press pulse rather than many. */
    KDRV_CST_AUTO_RESET,        /**< 0xFB: seconds of a touch with no gesture before a reset; 0 off. */
    KDRV_CST_LONG_PRESS_TIME,   /**< 0xFC: seconds a press is held before it is a long press; 0 off. */
    KDRV_CST_IO_CTL,            /**< 0xFD: the I/O block.                            */
    KDRV_CST_SOFT_RST,          /**< 0xFD[2]: hold the chip in reset from software.  */
    KDRV_CST_IIC_OD,            /**< 0xFD[1]: I2C pins open drain (1) or push-pull.  */
    KDRV_CST_EN_1V8,            /**< 0xFD[0]: I/O at 1.8 V (1) or VDD.               */
    KDRV_CST_DIS_AUTO_SLEEP,    /**< 0xFE: non-zero keeps the chip awake for good.   */
    KDRV_CST_FIELD_MAX
} kdrv_cst820_field_t;

/** @brief What a field is, for whoever prints or validates one. */
typedef struct {
    const char *name;     /**< The register list's name, lower case.       */
    const char *unit;     /**< "bits", "bool", "px", "code", "0.1ms", "10ms", "min", "s". */
    int32_t     min;      /**< Smallest value the field can hold.          */
    int32_t     max;      /**< Largest.                                    */
    uint16_t    step;     /**< Resolution; always 1 here.                  */
    bool        writable; /**< false for the frame and the identity.       */
} kdrv_cst820_field_info_t;

/**
 * @brief Bring the controller up: bus, ACK at 0x15, the identity read once.
 *
 * Probes a few times with a pause between, because a CST816 that has gone
 * to sleep NAKs until it wakes. Writes nothing. Must run after the panel:
 * the two share the GPIO7 reset line, and resetting the panel resets the
 * touch as well. A chip_id outside the family (0xBx) is refused: an ACK
 * alone would let any part at 0x15 feed the UI fingers that are not there.
 *
 * @return KODE_OK, or KODE_ERROR when nothing answered, the identity would
 *         not read, or it is not a CST8xx.
 */
kode_status_t kdrv_cst820_init(void);

/** @brief Did kdrv_cst820_init() succeed? */
bool kdrv_cst820_present(void);

/**
 * @brief Does anything answer at the controller's address right now?
 *
 * One short probe, for an owner that looks again for a controller that was
 * silent at the bring-up: on a board where nothing answers it costs a small
 * part of kdrv_cst820_init(). Writes nothing and brings nothing up.
 *
 * @return true when the controller is up already, or something ACKs at 0x15.
 */
bool kdrv_cst820_answers(void);

/**
 * @brief Put the controller in deep sleep, for a board with no owner for it.
 *
 * Brought up as kdrv_cst820_init() does, then sent to deep sleep: no scan and
 * no ACK until the GPIO7 reset it shares with the panel, which the next panel
 * bring-up gives. The driver reports absent afterwards. Never with AO_Touch
 * running.
 *
 * @return KODE_OK with the chip asleep; KODE_ERROR when it did not answer;
 *         KODE_IO_ERROR when the sleep was refused.
 */
kode_status_t kdrv_cst820_park(void);

/**
 * @brief The three identity bytes, as read at bring-up. No bus traffic.
 * @param[out] chip_id 0xA7. @param[out] proj_id 0xA8. @param[out] fw_ver 0xA9.
 * @return KODE_DENIED for a NULL or before a successful init.
 */
kode_status_t kdrv_cst820_identity(uint8_t *chip_id, uint8_t *proj_id,
                                   uint8_t *fw_ver);

/**
 * @brief The frame, in one burst: is a finger down, and where.
 *
 * @param[out] pressed Whether the finger count is non-zero.
 * @param[out] x,y     Written only while pressed, in the controller's own
 *                     pixels - no correction is applied here.
 * @return KODE_DENIED for a NULL or no controller; KODE_ERROR when the chip
 *         did not answer, with nothing written: a NAK is not a release.
 */
kode_status_t kdrv_cst820_read(bool *pressed, int16_t *x, int16_t *y);

/**
 * @brief Read one field.
 * @param f       The field.
 * @param[out] value Untouched unless KODE_OK.
 * @return KODE_DENIED for a bad field or NULL, KODE_ERROR for the bus - which
 *         on this part includes a chip that is asleep.
 */
kode_status_t kdrv_cst820_get(kdrv_cst820_field_t f, int32_t *value);

/**
 * @brief Write one field.
 *
 * Refuses the frame, the identity and anything outside the field's range,
 * does a masked read-modify-write so the register's other bits keep their
 * values, then reads the register back and compares this field's bits.
 *
 * @return KODE_DENIED for a read-only field or a value out of range,
 *         KODE_ERROR when the bus failed or the readback disagrees.
 */
kode_status_t kdrv_cst820_set(kdrv_cst820_field_t f, int32_t value);

/** @brief Name, unit and range of @p f; NULL for an invalid field. */
const kdrv_cst820_field_info_t *kdrv_cst820_field_info(kdrv_cst820_field_t f);

/** @brief The field called @p name (as printed by field_info), or KODE_DENIED. */
kode_status_t kdrv_cst820_field_by_name(const char *name, kdrv_cst820_field_t *out);

/**
 * @brief Log the cached identity, then every register the chip answers.
 *
 * A chip that is asleep answers nothing: that is one line saying so, not a
 * line per register.
 */
void kdrv_cst820_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_CST820_H_ */
