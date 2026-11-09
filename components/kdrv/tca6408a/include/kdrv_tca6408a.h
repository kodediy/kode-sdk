/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_tca6408a.h
 * @brief The TCA6408A I/O expander at 0x20, every register of it.
 */
#ifndef KDRV_TCA6408A_H_
#define KDRV_TCA6408A_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Every field of the four registers: each whole, then each bit.
 *
 * The bit fields carry the datasheet's letters - I for the input port, O for
 * the output port, N for the polarity inversion, C for the configuration -
 * and the pin number.
 */
typedef enum {
    KDRV_TCA_INPUT = 0,   /**< 0x00: the eight pins as they read, whatever their direction. */
    KDRV_TCA_OUTPUT,      /**< 0x01: what the pins configured as outputs drive. Reset 0xFF. */
    KDRV_TCA_POLARITY,    /**< 0x02: a 1 inverts that pin's input reading. Reset 0x00.    */
    KDRV_TCA_CONFIG,      /**< 0x03: 1 = input (high impedance), 0 = output. Reset 0xFF. */
    KDRV_TCA_I0,          /**< 0x00[0]: pin P0 reads.                                       */
    KDRV_TCA_I1,          /**< 0x00[1]                                                      */
    KDRV_TCA_I2,          /**< 0x00[2]                                                      */
    KDRV_TCA_I3,          /**< 0x00[3]                                                      */
    KDRV_TCA_I4,          /**< 0x00[4]                                                      */
    KDRV_TCA_I5,          /**< 0x00[5]                                                      */
    KDRV_TCA_I6,          /**< 0x00[6]                                                      */
    KDRV_TCA_I7,          /**< 0x00[7]                                                      */
    KDRV_TCA_O0,          /**< 0x01[0]: what P0 drives when it is an output.                */
    KDRV_TCA_O1,          /**< 0x01[1]                                                      */
    KDRV_TCA_O2,          /**< 0x01[2]                                                      */
    KDRV_TCA_O3,          /**< 0x01[3]                                                      */
    KDRV_TCA_O4,          /**< 0x01[4]                                                      */
    KDRV_TCA_O5,          /**< 0x01[5]                                                      */
    KDRV_TCA_O6,          /**< 0x01[6]                                                      */
    KDRV_TCA_O7,          /**< 0x01[7]                                                      */
    KDRV_TCA_N0,          /**< 0x02[0]: invert P0's reading.                                */
    KDRV_TCA_N1,          /**< 0x02[1]                                                      */
    KDRV_TCA_N2,          /**< 0x02[2]                                                      */
    KDRV_TCA_N3,          /**< 0x02[3]                                                      */
    KDRV_TCA_N4,          /**< 0x02[4]                                                      */
    KDRV_TCA_N5,          /**< 0x02[5]                                                      */
    KDRV_TCA_N6,          /**< 0x02[6]                                                      */
    KDRV_TCA_N7,          /**< 0x02[7]                                                      */
    KDRV_TCA_C0,          /**< 0x03[0]: P0 is an input (1) or an output (0).               */
    KDRV_TCA_C1,          /**< 0x03[1]                                                      */
    KDRV_TCA_C2,          /**< 0x03[2]                                                      */
    KDRV_TCA_C3,          /**< 0x03[3]                                                      */
    KDRV_TCA_C4,          /**< 0x03[4]                                                      */
    KDRV_TCA_C5,          /**< 0x03[5]                                                      */
    KDRV_TCA_C6,          /**< 0x03[6]                                                      */
    KDRV_TCA_C7,          /**< 0x03[7]                                                      */
    KDRV_TCA_FIELD_MAX
} kdrv_tca6408a_field_t;

/** @brief What a field is, for whoever prints or validates one. */
typedef struct {
    const char *name;     /**< The datasheet's name, lower case.           */
    const char *unit;     /**< "bits" for a register, "bool" for a pin.     */
    int32_t     min;      /**< Smallest value the field can hold.          */
    int32_t     max;      /**< Largest.                                    */
    uint16_t    step;     /**< Resolution; always 1 here.                  */
    bool        writable; /**< false for the input port and its bits.      */
} kdrv_tca6408a_field_info_t;

/**
 * @brief Bring the expander up: bus, ACK at 0x20, the four registers read.
 *
 * Writes nothing. The chip has no identity register, so presence is an ACK
 * plus four registers that answer; what they hold is logged, and whether
 * they hold the power-on defaults says whether this is a cold start or a
 * P4 reboot under a chip that was never reset.
 *
 * @return KODE_OK, or KODE_ERROR when nothing answered.
 */
kode_status_t kdrv_tca6408a_init(void);

/** @brief Did kdrv_tca6408a_init() succeed? */
bool kdrv_tca6408a_present(void);

/**
 * @brief Whether a pin was already an output driving high when this boot found
 *        the expander: a supply the last firmware left on, through the reset.
 *
 * @param pin The pin, 0..7.
 * @return true when it was; false too before kdrv_tca6408a_init() has read it.
 */
bool kdrv_tca6408a_boot_drove_high(uint8_t pin);

/**
 * @brief Read one field.
 * @param f       The field.
 * @param[out] value Untouched unless KODE_OK: a failed bus read must never
 *                   become a plausible reading of a button.
 * @return KODE_DENIED for a bad field or NULL, KODE_ERROR for the bus.
 */
kode_status_t kdrv_tca6408a_get(kdrv_tca6408a_field_t f, int32_t *value);

/**
 * @brief Write one field.
 *
 * Refuses the input port and anything outside the field's range, does a
 * masked read-modify-write so the register's other pins keep their values,
 * then reads the register back and compares this field's bits alone - a
 * neighbouring input may legitimately have moved between the two reads.
 *
 * @return KODE_DENIED for a read-only field or a value out of range,
 *         KODE_ERROR when the bus failed or the readback disagrees.
 */
kode_status_t kdrv_tca6408a_set(kdrv_tca6408a_field_t f, int32_t value);

/** @brief Name, unit and range of @p f; NULL for an invalid field. */
const kdrv_tca6408a_field_info_t *kdrv_tca6408a_field_info(kdrv_tca6408a_field_t f);

/** @brief The field called @p name (as printed by field_info), or KODE_DENIED. */
kode_status_t kdrv_tca6408a_field_by_name(const char *name,
                                          kdrv_tca6408a_field_t *out);

/** @brief Log the four registers in hex and every pin decoded. */
void kdrv_tca6408a_dump(void);

/**
 * @brief Arm the INT line (GPIO6) and call @p cb from its interrupt.
 *
 * Armed as a low level, and the interrupt disables itself before @p cb runs:
 * the owner reads the input register, which releases the line, and then
 * calls kdrv_tca6408a_int_rearm(). A change between the read and the re-arm
 * leaves the line low and the interrupt fires again at once - nothing is
 * lost, and no polling is needed to be sure of that.
 *
 * @param cb  Runs in interrupt context; may post a static event and no more.
 * @param arg Handed to @p cb.
 * @return KODE_DENIED for a NULL callback, KODE_ERROR when the pin could not
 *         be armed.
 */
kode_status_t kdrv_tca6408a_int_attach(void (*cb)(void *arg), void *arg);

/**
 * @brief Re-enable the INT interrupt after the input register was read.
 *
 * While the sleep wake is armed (kdrv_tca6408a_sleep_wake()) the wake comes
 * back with it.
 */
void kdrv_tca6408a_int_rearm(void);

/**
 * @brief Let the INT line wake the P4 from light sleep, or stop it doing so.
 *
 * Armed, a low INT wakes the chip and the interrupt takes the wake away again
 * before it calls the owner's callback: a line that stays low would wake the
 * chip at once, every time. The owner reads the input register and calls
 * kdrv_tca6408a_int_rearm(), which puts the wake back. A read that fails
 * leaves the wake withdrawn until the owner arms it afresh, on purpose: the
 * alternative is a chip that wakes in a loop on a line it cannot release.
 * Disarmed, the line is the plain interrupt of kdrv_tca6408a_int_attach()
 * again. Idempotent either way.
 *
 * @param armed true to arm the wake, false to withdraw it.
 * @return KODE_OK; KODE_DENIED when no interrupt was attached;
 *         KODE_ERROR when the wake could not be set.
 */
kode_status_t kdrv_tca6408a_sleep_wake(bool armed);

/**
 * @brief Is the INT line released (high)?
 *
 * True means nothing has changed since the input register was last read.
 * The production test reads the register and then asks this.
 */
bool kdrv_tca6408a_int_idle(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_TCA6408A_H_ */
