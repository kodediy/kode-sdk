/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_bq25896.h
 * @brief The BQ25896 charger and power-path PMIC at 0x6B, every register of it.
 */
#ifndef KDRV_BQ25896_H_
#define KDRV_BQ25896_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Every readable field of REG00..REG14, in register order.
 *
 * REG0C (the faults) is deliberately absent: reading it clears the latch, so
 * it is only reachable through kdrv_bq25896_faults().
 */
typedef enum {
    /* REG00 */
    KDRV_BQ_EN_HIZ = 0,      /**< Input in high impedance (no VBUS draw).     */
    KDRV_BQ_EN_ILIM,         /**< The ILIM pin clamps the input current.      */
    KDRV_BQ_IINLIM_MA,       /**< Input current limit, 100..3250 mA.          */
    /* REG01 */
    KDRV_BQ_BHOT,            /**< Boost hot threshold code 0..3 (3 = off).    */
    KDRV_BQ_BCOLD,           /**< Boost cold threshold code 0..1.             */
    KDRV_BQ_VINDPM_OS_MV,    /**< Relative VINDPM offset, 0..3100 mV.         */
    /* REG02 */
    KDRV_BQ_CONV_START,      /**< One-shot ADC conversion (self-clearing).    */
    KDRV_BQ_CONV_RATE,       /**< Continuous 1 s ADC conversion.              */
    KDRV_BQ_BOOST_FREQ,      /**< Boost 0 = 1.5 MHz, 1 = 500 kHz.             */
    KDRV_BQ_ICO_EN,          /**< Input current optimiser.                    */
    KDRV_BQ_FORCE_DPDM,      /**< Force input detection (self-clearing).      */
    KDRV_BQ_AUTO_DPDM_EN,    /**< Detect the input on plug-in.                */
    /* REG03 */
    KDRV_BQ_BAT_LOADEN,      /**< The 30 mA battery test load.                */
    KDRV_BQ_WD_RST,          /**< Kick the I2C watchdog (self-clearing).      */
    KDRV_BQ_OTG_CONFIG,      /**< Boost (OTG) enable.                         */
    KDRV_BQ_CHG_CONFIG,      /**< Charge enable.                              */
    KDRV_BQ_SYS_MIN_MV,      /**< Minimum system voltage, 3000..3700 mV.      */
    KDRV_BQ_MIN_VBAT_SEL,    /**< Boost exit 0 = 2.9 V, 1 = 2.5 V.            */
    /* REG04 */
    KDRV_BQ_EN_PUMPX,        /**< Current pulse control enable.               */
    KDRV_BQ_ICHG_MA,         /**< Fast charge current, 0..3008 mA.            */
    /* REG05 */
    KDRV_BQ_IPRECHG_MA,      /**< Precharge current, 64..1024 mA.             */
    KDRV_BQ_ITERM_MA,        /**< Termination current, 64..1024 mA.           */
    /* REG06 */
    KDRV_BQ_VREG_MV,         /**< Charge voltage, 3840..4608 mV.              */
    KDRV_BQ_BATLOWV,         /**< Pre to fast threshold 0 = 2.8 V, 1 = 3.0 V. */
    KDRV_BQ_VRECHG,          /**< Recharge 0 = 100 mV, 1 = 200 mV below VREG. */
    /* REG07 */
    KDRV_BQ_EN_TERM,         /**< Charge termination.                         */
    KDRV_BQ_STAT_DIS,        /**< STAT pin off.                               */
    KDRV_BQ_WATCHDOG,        /**< I2C watchdog code: 0 off, 1 40 s, 2 80 s, 3 160 s. */
    KDRV_BQ_EN_TIMER,        /**< Charge safety timer.                        */
    KDRV_BQ_CHG_TIMER,       /**< Safety timer code: 0 5 h, 1 8 h, 2 12 h, 3 20 h. */
    KDRV_BQ_JEITA_ISET,      /**< Cool-range current 0 = 50 %, 1 = 20 % of ICHG. */
    /* REG08 */
    KDRV_BQ_BAT_COMP_MOHM,   /**< IR compensation resistance, 0..140 mOhm.    */
    KDRV_BQ_VCLAMP_MV,       /**< IR compensation clamp, 0..224 mV.           */
    KDRV_BQ_TREG_C,          /**< Thermal regulation, 60..120 C.              */
    /* REG09 */
    KDRV_BQ_FORCE_ICO,       /**< Start ICO now (self-clearing).              */
    KDRV_BQ_TMR2X_EN,        /**< Safety timer at half rate under DPM/thermal. */
    KDRV_BQ_BATFET_DIS,      /**< BATFET off: ship mode.                      */
    KDRV_BQ_JEITA_VSET,      /**< Warm-range voltage 0 = VREG-200 mV, 1 = VREG. */
    KDRV_BQ_BATFET_DLY,      /**< Ship mode after ~10 s instead of at once.   */
    KDRV_BQ_BATFET_RST_EN,   /**< QON long press resets the system.           */
    KDRV_BQ_PUMPX_UP,        /**< Pulse the adapter voltage up (self-clearing). */
    KDRV_BQ_PUMPX_DN,        /**< Pulse the adapter voltage down (self-clearing). */
    /* REG0A */
    KDRV_BQ_BOOSTV_MV,       /**< Boost voltage, 4550..5510 mV.               */
    KDRV_BQ_PFM_OTG_DIS,     /**< No PFM in boost mode.                       */
    KDRV_BQ_BOOST_LIM,       /**< Boost current code 0..6 (0.5 .. 2.15 A).    */
    /* REG0B, read only */
    KDRV_BQ_VBUS_STAT,       /**< kdrv_bq25896_vbus_stat_t.                   */
    KDRV_BQ_CHRG_STAT,       /**< kdrv_bq25896_chrg_stat_t.                   */
    KDRV_BQ_PG_STAT,         /**< Input power good.                           */
    KDRV_BQ_VSYS_STAT,       /**< In minimum-system-voltage regulation.       */
    /* REG0D */
    KDRV_BQ_FORCE_VINDPM,    /**< Absolute VINDPM (else relative).            */
    KDRV_BQ_VINDPM_MV,       /**< Input voltage limit, 3900..15300 mV.        */
    /* REG0E .. REG13, read only: the last ADC conversion */
    KDRV_BQ_THERM_STAT,      /**< In thermal regulation.                      */
    KDRV_BQ_BATV_MV,         /**< Battery, 2304..4848 mV.                     */
    KDRV_BQ_SYSV_MV,         /**< System rail, 2304..4848 mV.                 */
    KDRV_BQ_TS_PCT_X1000,    /**< TS pin as thousandths of a percent of REGN. */
    KDRV_BQ_VBUS_GD,         /**< VBUS attached.                              */
    KDRV_BQ_VBUSV_MV,        /**< Input, 2600..15300 mV.                      */
    KDRV_BQ_ICHGR_MA,        /**< Charge current, 0..6350 mA.                 */
    KDRV_BQ_VDPM_STAT,       /**< In input voltage regulation.                */
    KDRV_BQ_IDPM_STAT,       /**< In input current regulation.                */
    KDRV_BQ_IDPM_LIM_MA,     /**< ICO's input limit, 100..3250 mA.            */
    /* REG14 */
    KDRV_BQ_REG_RST,         /**< Reset every register (self-clearing).       */
    KDRV_BQ_ICO_OPTIMIZED,   /**< ICO finished.                               */
    KDRV_BQ_PN,              /**< Part number code; 0 is the bq25896.         */
    KDRV_BQ_TS_PROFILE,      /**< 1 = JEITA.                                  */
    KDRV_BQ_DEV_REV,         /**< Silicon revision.                           */
    KDRV_BQ_FIELD_MAX
} kdrv_bq25896_field_t;

/** @brief What a field is, for whoever prints or validates one. */
typedef struct {
    const char *name;     /**< The datasheet's name, lower case.           */
    const char *unit;     /**< "mV", "mA", "mohm", "C", "bool", "code"...   */
    int32_t     min;      /**< Smallest value the field can hold.          */
    int32_t     max;      /**< Largest.                                    */
    uint16_t    step;     /**< Resolution; 1 for codes and bools.          */
    bool        writable; /**< false for status and ADC fields.            */
} kdrv_bq25896_field_info_t;

/** @brief REG0B[7:5]: what is on VBUS. */
typedef enum {
    KDRV_BQ_VBUS_NONE    = 0,  /**< No input.                              */
    KDRV_BQ_VBUS_SDP     = 1,  /**< USB host (500 mA).                     */
    KDRV_BQ_VBUS_ADAPTER = 2,  /**< Adapter (3.25 A).                      */
    KDRV_BQ_VBUS_OTG     = 7,  /**< The PMIC's own boost output.           */
} kdrv_bq25896_vbus_stat_t;

/**
 * @brief REG0B[4:3]: the charge phase.
 *
 * The values are the register's, and they are also what `bat_info` puts on
 * the wire as `charging=`; they must not be renumbered.
 */
typedef enum {
    KDRV_BQ_CHRG_NONE = 0,   /**< Not charging.                              */
    KDRV_BQ_CHRG_PRE  = 1,   /**< Pre-charge (below BATLOWV).                */
    KDRV_BQ_CHRG_FAST = 2,   /**< Constant current or constant voltage.      */
    KDRV_BQ_CHRG_DONE = 3,   /**< Termination reached.                       */
} kdrv_bq25896_chrg_stat_t;

/** @brief One ADC conversion: REG0E..REG12 as the chip last converted them. */
typedef struct {
    uint16_t vbat_mv;       /**< 2304 + 20 mV steps.                        */
    uint16_t vsys_mv;       /**< 2304 + 20 mV steps.                        */
    uint16_t vbus_mv;       /**< 2600 + 100 mV steps.                       */
    uint16_t ichg_ma;       /**< 50 mA steps (not the 64 of the setting).   */
    uint16_t ts_pct_x1000;  /**< 21000 + 465 steps, thousandths of a %.     */
    bool     therm_stat;    /**< REG0E[7]: in thermal regulation.           */
    bool     vbus_gd;       /**< REG11[7]: VBUS attached.                   */
} kdrv_bq25896_adc_t;

/** @brief REG0C decoded. `raw` is the byte itself. */
typedef struct {
    bool    watchdog;   /**< The I2C watchdog expired (default mode).      */
    bool    boost;      /**< Boost overloaded, over voltage, or BAT too low. */
    uint8_t chrg;       /**< 0 ok, 1 input fault, 2 thermal shutdown, 3 safety timer. */
    bool    bat_ovp;    /**< Battery over voltage.                         */
    uint8_t ntc;        /**< 0 ok, 2 warm, 3 cool, 5 cold, 6 hot. Always live. */
    uint8_t raw;
} kdrv_bq25896_faults_t;

/**
 * @brief Bring the PMIC up: bus, ACK at 0x6B, identity.
 *
 * Writes nothing. Refuses a part whose PN is not the bq25896's, so a wrong
 * chip is found at boot rather than when a field decodes to nonsense.
 * The PMIC keeps its registers across a reflash of the P4: they hold what the last firmware wrote.
 *
 * @return KODE_OK, or KODE_ERROR when nothing answered or the PN is wrong.
 */
kode_status_t kdrv_bq25896_init(void);

/** @brief Did kdrv_bq25896_init() succeed? */
bool kdrv_bq25896_present(void);

/**
 * @brief Read one field in its natural unit.
 * @param f       The field.
 * @param[out] value Untouched unless KODE_OK: a failed bus read must never
 *                   become a plausible number.
 * @return KODE_DENIED for a bad field or NULL, KODE_ERROR for the bus.
 */
kode_status_t kdrv_bq25896_get(kdrv_bq25896_field_t f, int32_t *value);

/**
 * @brief Write one field in its natural unit.
 *
 * Rounds to the field's step, refuses anything outside its range, does a
 * masked read-modify-write so the register's other fields keep their values,
 * then reads the register back: a write that did not take is KODE_ERROR, not
 * a setting that "did not take" forty seconds later. Self-clearing fields
 * (the ones the datasheet marks as returning to 0 by themselves) are written
 * and not verified.
 *
 * @return KODE_DENIED for a read-only field or a value out of range,
 *         KODE_ERROR when the bus failed or the readback disagrees.
 */
kode_status_t kdrv_bq25896_set(kdrv_bq25896_field_t f, int32_t value);

/** @brief Name, unit and range of @p f; NULL for an invalid field. */
const kdrv_bq25896_field_info_t *kdrv_bq25896_field_info(kdrv_bq25896_field_t f);

/** @brief The field called @p name (as printed by field_info), or KODE_DENIED. */
kode_status_t kdrv_bq25896_field_by_name(const char *name,
                                         kdrv_bq25896_field_t *out);

/**
 * @brief The last ADC conversion, REG0E..REG12 in one transaction.
 *
 * Meaningful only after CONV_RATE or CONV_START and about 1.2 s of settling;
 * before that every register holds the bottom of its scale, and 2.304 V of
 * battery is what a flat cell reads like.
 */
kode_status_t kdrv_bq25896_adc_read(kdrv_bq25896_adc_t *out);

/**
 * @brief REG0C, twice.
 *
 * The register keeps everything that has happened since it was last read, so
 * the first read is the history and the second is the present. NTC_FAULT is
 * the exception and is live in both.
 *
 * @param[out] latched What has happened since the last read. May be NULL.
 * @param[out] live    What is true now. May be NULL.
 */
kode_status_t kdrv_bq25896_faults(kdrv_bq25896_faults_t *latched,
                                  kdrv_bq25896_faults_t *live);

/**
 * @brief Ship mode: open the BATFET and cut the battery from VSYS.
 *
 * One write of REG09 with BATFET_DIS set, BATFET_DLY as asked and every other
 * bit as it was, BATFET_RST_EN included so the QON long press keeps working.
 * On battery this never returns; with USB attached VSYS stays up and it
 * returns KODE_OK with the battery path open. Waking is hardware: QON or
 * VBUS.
 *
 * @param delayed true for the chip's ~10 s grace before the cut.
 */
kode_status_t kdrv_bq25896_ship_mode(bool delayed);

/** @brief Log REG00..REG14 in hex, every field decoded, and REG0C twice. */
void kdrv_bq25896_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_BQ25896_H_ */
