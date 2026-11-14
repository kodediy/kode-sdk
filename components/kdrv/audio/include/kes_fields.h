/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kes_fields.h
 * @brief The codec's field table: what `audio` can print or set.
 */
#ifndef KES_FIELDS_H_
#define KES_FIELDS_H_

#include <stdbool.h>
#include <stdint.h>

#include "kdrv_es8311.h"
#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Everything `audio` can print or set.
 *
 * The driver's own state first, then the settings it applies at the next
 * open, then the chip's fields decoded, then the raw registers underneath
 * them. A register and its bits are both fields here, which is the `pmic` and
 * `ioexp` precedent: an operator reading a clock chain wants the byte, and an
 * operator checking one thing wants the one thing.
 */
typedef enum {
    /* The driver, answered without touching the bus (busy: the owner). */
    KDRV_ES_PRESENT = 0,  /**< The chip answered and identified itself.      */
    KDRV_ES_OPEN,         /**< Analog up, both channels enabled.             */
    KDRV_ES_BUSY,         /**< A job is with the worker. The owner answers
                           *   it: the driver keeps no such state.          */
    KDRV_ES_RATE,         /**< Hz. Fixed at 48 kHz.                          */
    KDRV_ES_BITS,         /**< Fixed.                                        */
    KDRV_ES_MCLK_MULT,    /**< Fixed.                                        */
    /* Settings, applied at the next open. */
    KDRV_ES_VOL,          /**< 0 to 100. 100 is the speaker's cap,
                           *   KDRV_ES8311_OUT_DB_X10_MAX: full scale.     */
    KDRV_ES_MIC_DB,       /**< 0 to 42, rounded down to a 6 dB step.         */
    KDRV_ES_OUT_MUTE,     /**< Mute the DAC.                                 */
    KDRV_ES_IN_MUTE,      /**< Mute the ADC.                                 */
    /* Decoded from the chip. */
    KDRV_ES_DAC_VOL_DB,   /**< 0x32 by reg/2 - 95.5, in tenth-dB. Volume 100
                           *   reads the speaker's cap; zero is full scale. */
    KDRV_ES_ADC_VOL_DB,   /**< 0x17, the same law on the capture side.       */
    KDRV_ES_ADC_SCALE_DB, /**< 0x16 as 0 to 42 dB in 6 dB steps. This is what
                           *   the codec layer's gain call actually writes,
                           *   and it is NOT the analog front end - see 0x14. */
    KDRV_ES_PGA,          /**< 0x14[3:0], the true analog PGA, raw.          */
    KDRV_ES_DMIC,         /**< 0x14[6]. Must read 0: this mic is analog.     */
    KDRV_ES_HPSW,         /**< 0x13[4]. The codec layer forces 1 at open.    */
    KDRV_ES_DAC2ADC,      /**< 0x44. Must read KDRV_ES8311_DAC2ADC_OFF.      */
    KDRV_ES_DRC_EN,       /**< 0x34[7]. Off, and read only: it levels toward
                           *   -6 dBFS whatever the volume is.               */
    KDRV_ES_RAMPRATE,     /**< 0x37[7:4]. Sets how long a tone must settle.  */
    /* The raw registers, so a bad divider is visible without a scope. */
    KDRV_ES_REG00, KDRV_ES_REG01, KDRV_ES_REG02, KDRV_ES_REG03,
    KDRV_ES_REG04, KDRV_ES_REG05, KDRV_ES_REG06, KDRV_ES_REG07,
    KDRV_ES_REG08, KDRV_ES_REG09, KDRV_ES_REG0A, KDRV_ES_REG0D,
    KDRV_ES_REG0E, KDRV_ES_REG12, KDRV_ES_REG13, KDRV_ES_REG14,
    KDRV_ES_REG16, KDRV_ES_REG17, KDRV_ES_REG31, KDRV_ES_REG32,
    KDRV_ES_REG34, KDRV_ES_REG37, KDRV_ES_REG44, KDRV_ES_REG45,
    KDRV_ES_CHD1,         /**< 0xFD. Must read KDRV_ES8311_ID1.              */
    KDRV_ES_CHD2,         /**< 0xFE. Must read KDRV_ES8311_ID2.              */
    KDRV_ES_CHVER,        /**< 0xFF.                                         */
    KDRV_ES_FIELD_MAX
} kdrv_es8311_field_t;

/** @brief What a field is, for whoever prints or validates one. */
typedef struct {
    const char *name;     /**< Lower case, as the console spells it.        */
    const char *unit;     /**< "bits", "bool", "dB", "x10dB", "Hz", "%".    */
    int32_t     min;      /**< Smallest value the field can hold.           */
    int32_t     max;      /**< Largest.                                     */
    uint16_t    step;     /**< Resolution.                                  */
    bool        writable; /**< Whether set() will take it.                  */
} kdrv_es8311_field_info_t;

/**
 * @brief Read one field.
 *
 * @param f     Which.
 * @param value Out. Left untouched unless KODE_OK.
 * @return KODE_OK, KODE_DENIED for a bad field or an absent chip,
 *         KODE_IO_ERROR when the bus said no.
 */
kode_status_t kdrv_es8311_get(kdrv_es8311_field_t f, int32_t *value);

/**
 * @brief Write one field.
 *
 * A field that lives in a register is a masked read-modify-write and is read
 * back, comparing only this field's bits: the rest of the byte belongs to
 * other things and may have moved between the two accesses.
 *
 * @param f     Which.
 * @param value What to set it to.
 * @return KODE_OK, KODE_DENIED for a bad field, a read-only one, a value out
 *         of range or an absent chip, KODE_IO_ERROR when the bus said no.
 */
kode_status_t kdrv_es8311_set(kdrv_es8311_field_t f, int32_t value);

/**
 * @brief What a field is called and what it may hold.
 *
 * @param f Which.
 * @return Its description, or NULL when @p f is out of range.
 */
const kdrv_es8311_field_info_t *kdrv_es8311_field_info(kdrv_es8311_field_t f);

/**
 * @brief Look a field up by the name the console spells.
 *
 * @param name The name.
 * @param out  Out.
 * @return KODE_OK, or KODE_DENIED when nothing is called that.
 */
kode_status_t kdrv_es8311_field_by_name(const char *name,
                                        kdrv_es8311_field_t *out);

/** @brief Log every field at info level. The bench's one-liner. */
void kdrv_es8311_dump(void);

/** @brief What an open pushes at the chip. */
typedef struct {
    uint8_t vol;       /**< 0 to 100.                                       */
    uint8_t mic_db;    /**< 0 to 42, in 6 dB steps.                         */
    bool    out_mute;  /**< Mute the DAC.                                   */
    bool    in_mute;   /**< Mute the ADC.                                   */
} kes_settings_t;

/**
 * @brief Copy out what an open should apply.
 *
 * The settings live with the table that sets them rather than with the device
 * that pushes them, and they survive a close on purpose: a close is the
 * owner's idle timer, not something a caller asked for, so a beep after one
 * must come back at the volume the last caller set.
 *
 * @param out Where to write. NULL is a no-op.
 */
void kes_settings(kes_settings_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KES_FIELDS_H_ */
