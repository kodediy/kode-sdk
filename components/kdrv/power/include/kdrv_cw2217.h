/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_cw2217.h
 * @brief The CW2217 fuel gauge at 0x64, every register of it.
 */
#ifndef KDRV_CW2217_H_
#define KDRV_CW2217_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The battery profile is 80 bytes at 0x10..0x5F. */
#define KDRV_CW2217_PROFILE_LEN 80U

/** @brief Every field of the register map, plus the two from the vendor driver. */
typedef enum {
    KDRV_CW_VERSION = 0,    /**< 0x00: 0xA0 for any CW221x, asleep or awake.  */
    KDRV_CW_VCELL_MV,       /**< 0x02-03: the cell, 312.5 uV steps, in mV.     */
    KDRV_CW_SOC_X256,       /**< 0x04-05: %, integer in the high byte, 1/256 % in the low. Above 100 % means no profile. */
    KDRV_CW_TEMP_C2,        /**< 0x06: the TS pin in half degrees, -80 = -40 C. */
    KDRV_CW_EN_SOC_INT,     /**< 0x0A[6]: SOC alarm enable.                    */
    KDRV_CW_EN_TMX_INT,     /**< 0x0A[5]: maximum temperature alarm enable.    */
    KDRV_CW_EN_TMN_INT,     /**< 0x0A[4]: minimum temperature alarm enable.    */
    KDRV_CW_INT_FLAGS,      /**< 0x0A[2:0]: SOC, TMX, TMN pending; write 0 to clear. */
    KDRV_CW_UPDATE_FLAG,    /**< 0x0B[7]: a profile was loaded since the last power-on. */
    KDRV_CW_SOC_ALERT_PCT,  /**< 0x0B[6:0]: alarm threshold; 101..127 = every 1 % change. */
    KDRV_CW_TEMP_MAX_C2,    /**< 0x0C: alarm threshold, half degrees.          */
    KDRV_CW_TEMP_MIN_C2,    /**< 0x0D: alarm threshold, half degrees.          */
    KDRV_CW_CURRENT_MA,     /**< 0x0E-0F: through the shunt, charging positive. */
    KDRV_CW_T_HOST_C2,      /**< 0xA0-A1: host-reported temperature, half degrees. */
    KDRV_CW_USER_CONF,      /**< 0xA2: a byte for whoever wants one.           */
    KDRV_CW_CYCLES_X16,     /**< 0xA4-05: charge cycles in sixteenths.         */
    KDRV_CW_SOH_PCT,        /**< 0xA6: state of health, 0..100.                */
    KDRV_CW_READY,          /**< 0xA7: the algorithm is up (vendor register).  */
    KDRV_CW_AVE_CURRENT_MA, /**< 0xA8-A9: 16 s mean of the current.            */
    KDRV_CW_FW_VERSION,     /**< 0xAB: 0x00 asleep; bits 7:6 = 01 for a CW2217. */
    KDRV_CW_FIELD_MAX
} kdrv_cw2217_field_t;

/** @brief What a field is, for whoever prints or validates one. */
typedef struct {
    const char *name;     /**< The datasheet's name, lower case.           */
    const char *unit;     /**< "mV", "mA", "0.5C", "bool", "code", ...      */
    int32_t     min;      /**< Smallest value the field can hold.          */
    int32_t     max;      /**< Largest.                                    */
    uint16_t    step;     /**< Resolution in the unit; 1 unless coarser.   */
    bool        writable; /**< false for measurements and status.          */
} kdrv_cw2217_field_info_t;

/**
 * @brief Bring the gauge up: bus, ACK at 0x64, identity. Writes nothing.
 *
 * No ACK means no battery (the chip runs off VBAT) and is reported as
 * KODE_ERROR with a warning, not an error line; the owner treats it as an
 * optional part that is absent.
 *
 * @return KODE_OK, or KODE_ERROR when nothing answered or VERSION is not 0xA0.
 */
kode_status_t kdrv_cw2217_init(void);

/** @brief Did kdrv_cw2217_init() succeed? */
bool kdrv_cw2217_present(void);

/**
 * @brief Leave shutdown: CONFIG <- 0x30, 20 ms, CONFIG <- 0x00.
 *
 * Returns at once. The algorithm takes about a second to come up; poll
 * KDRV_CW_READY before believing any other register.
 */
kode_status_t kdrv_cw2217_wake(void);

/**
 * @brief Enter shutdown: CONFIG <- 0x30, 20 ms, CONFIG <- 0xF0.
 *
 * SOH and the cycle count clear; the profile and UPDATE_FLAG survive.
 */
kode_status_t kdrv_cw2217_sleep(void);

/**
 * @brief The datasheet's restart: 0xF0, 0x30, 0x00, then wait for ready.
 *
 * The algorithm reloads its parameters and starts a fresh SOC estimate.
 * Blocks up to three seconds; KODE_TIMEOUT if the chip never reports ready.
 */
kode_status_t kdrv_cw2217_reset(void);

/**
 * @brief Read one field in its natural unit.
 * @param[out] value Untouched unless KODE_OK.
 * @return KODE_DENIED for a bad field or NULL, KODE_ERROR for the bus.
 */
kode_status_t kdrv_cw2217_get(kdrv_cw2217_field_t f, int32_t *value);

/**
 * @brief Write one field in its natural unit.
 *
 * Masked read-modify-write for the fields that share a register, then a
 * readback, as the PMIC driver does. T_HOST is the exception: the algorithm
 * ignores a low byte equal to the previous one, so the driver alternates it
 * and does not read it back.
 *
 * @return KODE_DENIED for a read-only field or a value out of range,
 *         KODE_ERROR when the bus failed or the readback disagrees.
 */
kode_status_t kdrv_cw2217_set(kdrv_cw2217_field_t f, int32_t value);

/** @brief Name, unit and range of @p f; NULL for an invalid field. */
const kdrv_cw2217_field_info_t *kdrv_cw2217_field_info(kdrv_cw2217_field_t f);

/** @brief The field called @p name (as printed by field_info), or KODE_DENIED. */
kode_status_t kdrv_cw2217_field_by_name(const char *name,
                                        kdrv_cw2217_field_t *out);

/**
 * @brief Load a battery profile, the reference driver's way, and verify it.
 *
 * Sleep, write the 80 bytes to 0x10..0x5F, set UPDATE_FLAG together with
 * @p soc_alert_pct (the two share a register; writing the flag alone would
 * silence the low-battery alarm), disable every interrupt, wake, wait for
 * ready, then read the 80 bytes back. Blocks for one to two seconds.
 *
 * @return KODE_ERROR when a byte did not stick, KODE_TIMEOUT when the
 *         algorithm never came up, KODE_DENIED for NULL or a threshold above 127.
 */
kode_status_t kdrv_cw2217_profile_load(const uint8_t profile[KDRV_CW2217_PROFILE_LEN],
                                       uint8_t soc_alert_pct);

/** @brief The 80 profile bytes as the chip holds them. */
kode_status_t kdrv_cw2217_profile_read(uint8_t out[KDRV_CW2217_PROFILE_LEN]);

/** @brief Log every register the map names, the readiness byte and the profile. */
void kdrv_cw2217_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_CW2217_H_ */
