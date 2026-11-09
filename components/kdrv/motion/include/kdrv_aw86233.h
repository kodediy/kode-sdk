/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_aw86233.h
 * @brief The AW86233 LRA haptic driver at 0x5A: a streamed waveform, clamped.
 */
#ifndef KDRV_AW86233_H_
#define KDRV_AW86233_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The RTP sample rate SYSCTRL2 is left at, in Hz. */
#define KDRV_AW_RATE_HZ      12000U

/** @brief The LRA's resonance, in Hz. Off it, the motor barely moves. */
#define KDRV_AW_F0_HZ        235U

/** @brief The longest burst one kdrv_aw86233_rtp_feed() writes, in bytes. */
#define KDRV_AW_BURST        64U

/** @brief The amplitude ceiling, per cent of full scale: the LRA's 1.8 Vrms rating. */
#define KDRV_AW_GAIN_MAX_PCT 80U

/** @brief What CHIPID reads on an AW86233; the high byte, 0x23, is the family. */
#define KDRV_AW_CHIPID       0x2330U

/** @brief How many samples @p ms_ milliseconds of playback needs. */
#define KDRV_AW_SAMPLES(ms_) (((ms_) * KDRV_AW_RATE_HZ) / 1000U)

/** @brief The console's table: the identity, the FIFO's state, the two configs. */
typedef enum {
    KDRV_AW_PRESENT = 0,   /**< Did create() find the chip?                  */
    KDRV_AW_CHIPID_FIELD,  /**< 0x57:0x69, as read at bring-up: 0x2330.      */
    KDRV_AW_SYSST,         /**< 0x01: the whole status byte.                 */
    KDRV_AW_FF_AFS,        /**< 0x01 bit 3: the RTP FIFO is almost full.     */
    KDRV_AW_GAIN,          /**< 0x07: the amplitude, 0 to 255.               */
    KDRV_AW_PLAY_MODE,     /**< 0x08 bits [1:0]: 1 is RTP.                   */
    KDRV_AW_GLB_STATE,     /**< 0x3F bits [3:0]: 0 standby, 8 RTP, 11 brake. */
    KDRV_AW_SYSCTRL2,      /**< 0x46: standby and the sample rate.           */
    KDRV_AW_STANDBY,       /**< 0x46 bit 6: nothing plays while this is set. */
    KDRV_AW_FIELD_MAX
} kdrv_aw86233_field_t;

/** @brief What a field is, for whoever prints or validates one. */
typedef struct {
    const char *name;     /**< The field's name, lower case.      */
    const char *unit;     /**< "bool", "code", "bits".            */
    int32_t     min;      /**< Smallest value the field can hold. */
    int32_t     max;      /**< Largest.                           */
    uint16_t    step;     /**< Resolution; always 1 here.         */
    bool        writable; /**< false for the identity and the status. */
} kdrv_aw86233_field_info_t;

/**
 * @brief Bring the chip up: probe, identify, soft reset, out of standby.
 *
 * The ACK at KBSP_I2C_ADDR_HAPTIC says something is there, and CHIPID says
 * what: 0x23 in the high byte is the AW8623X family, whose three members
 * share this register map, and anything else answering at 0x5A - the touch
 * controller of a panel this SDK does not drive answers there - is refused
 * rather than driven. Both bytes go in the log.
 *
 * SYSCTRL2 is the step that cannot be skipped: without it the chip
 * acknowledges every write and never drives the motor. It is written and then
 * undone - the chip is taken out of standby, which is what proves it can
 * leave, and put back into it, which is where a board nobody has asked to
 * buzz belongs.
 *
 * Calls khal_delay_ms() over the reset, so this belongs to a task's
 * start-up, never to an AO's step.
 *
 * @return KODE_OK; KODE_IO_ERROR when nothing answered at 0x5A, when what
 *         answered is not an AW8623X, or when the bring-up writes did not
 *         take.
 */
kode_status_t kdrv_aw86233_create(void);

/**
 * @brief Leave the AW86233 in standby, for a board with no owner for it.
 *
 * One probe, quiet, then the bring-up, which leaves the part in standby.
 *
 * @return KODE_OK with the part in standby; KODE_ERROR when nothing answered.
 */
kode_status_t kdrv_aw86233_park(void);

/** @brief Did the last kdrv_aw86233_create() find a chip? */
bool kdrv_aw86233_present(void);

/**
 * @brief Park the driver stage, or bring it back.
 *
 * A read-modify-write of SYSCTRL2 bit 6, so the sample rate under it - which
 * a user may have set through the field table - is left alone. Awinic quote
 * 0.4 ms to start up, which is shorter than one QP tick: the owner spends it
 * as a declared busy-wait rather than as a timer.
 *
 * @param on true parks the chip, false makes it able to drive again.
 * @return KODE_DENIED when there is no chip, KODE_IO_ERROR for the bus.
 */
kode_status_t kdrv_aw86233_standby(bool on);

/**
 * @brief Put the chip in RTP mode at @p gain_pct and start it playing.
 *
 * GO is issued before a single sample exists, which is the mode's contract:
 * the chip plays what arrives and waits on an empty FIFO. The datasheet's
 * playback sequence then has the host confirm the chip actually entered RTP
 * - GLB_STATE reads 8 - before the first sample, and so does this: a GO that
 * is acknowledged and not acted on is how a parked driver stage fails, and
 * it is reported here rather than discovered as a silent buzz. Feeding can
 * start immediately after. The chip must be out of standby first - see
 * kdrv_aw86233_standby().
 *
 * @param gain_pct Amplitude, 0 to 100 per cent of full scale. Clamped to
 *                 KDRV_AW_GAIN_MAX_PCT first: asking for more is not an
 *                 error, it is simply not granted.
 * @param[out] reg_written The PLAYCFG2 byte this actually wrote, or NULL. The
 *                 percentage does not survive the round trip - it is clamped
 *                 and then truncated - so a log line or a production record
 *                 that quotes the request quotes a number the chip never saw.
 *                 Written only when this returns KODE_OK.
 * @return KODE_DENIED when there is no chip, KODE_IO_ERROR for the bus or
 *         for a chip that took GO and did not start.
 */
kode_status_t kdrv_aw86233_rtp_begin(uint8_t gain_pct, uint8_t *reg_written);

/**
 * @brief Offer up to KDRV_AW_BURST samples to the RTP FIFO.
 *
 * One status read and at most one burst, then it returns: the back-pressure
 * is reported rather than waited on, so the caller decides whether to spin,
 * yield or come back next tick.
 *
 * @param pcm            Signed 8-bit samples.
 * @param n              How many are on offer.
 * @param[out] accepted  How many the chip took: 0 when the FIFO is almost
 *                       full, otherwise min(@p n, KDRV_AW_BURST). Always
 *                       written when this returns KODE_OK.
 * @return KODE_DENIED for a NULL or no chip, KODE_IO_ERROR for the bus.
 */
kode_status_t kdrv_aw86233_rtp_feed(const int8_t *pcm, size_t n, size_t *accepted);

/**
 * @brief Stop playing.
 *
 * Unconditional, and the owner issues it even on a path that failed: without
 * it the LRA keeps ringing on whatever is left in the FIFO.
 *
 * @return KODE_DENIED when there is no chip, KODE_IO_ERROR for the bus.
 */
kode_status_t kdrv_aw86233_rtp_end(void);

/**
 * @brief Fill @p out with a KDRV_AW_F0_HZ sine, continuing from @p phase.
 *
 * The waveform is generated rather than stored because the FIFO is fed in
 * bursts and a table would have to be as long as the buzz. @p phase carries
 * the sample index across calls, so two bursts join without a step in the
 * waveform - and a step is a click the motor reproduces faithfully.
 *
 * The peak is KDRV_AW_GAIN_MAX_PCT of full scale, the same ceiling
 * kdrv_aw86233_rtp_begin() applies to the gain: amplitude is limited in both
 * the samples and the register, so neither one alone can drive the LRA past
 * its rating.
 *
 * @param out          At least @p n samples.
 * @param n            How many to write.
 * @param[in,out] phase The sample index. Zero starts a fresh waveform.
 */
void kdrv_aw86233_sine_fill(int8_t *out, size_t n, uint32_t *phase);

/**
 * @brief How long @p samples take to play, in milliseconds.
 *
 * The chip's clock, not the bus's. Handing the FIFO 200 ms of audio returns
 * in about 70 ms, so this is what an owner waits out before STOP.
 *
 * @param samples How many samples were handed over.
 * @return Milliseconds, rounded up: a partial millisecond is still playing.
 */
uint32_t kdrv_aw86233_playing_ms(size_t samples);

/**
 * @brief Read one field.
 * @param f          The field.
 * @param[out] value Untouched unless KODE_OK.
 * @return KODE_DENIED for a bad field, a NULL or no chip; KODE_IO_ERROR for
 *         the bus.
 */
kode_status_t kdrv_aw86233_get(kdrv_aw86233_field_t f, int32_t *value);

/**
 * @brief Write one field.
 *
 * The console's way into a register the driver does not otherwise touch, not
 * a second configuration path: what the driver decides it decides in
 * create(), rtp_begin() and kdrv_aw86233_standby(), and a write here is
 * overwritten by the next of those. The standby bit is the one that goes
 * first: every buzz clears it and every tail sets it again.
 *
 * @param f     The field.
 * @param value The value, inside the field's declared range.
 * @return KODE_DENIED for a read-only field, a value out of range or no
 *         chip; KODE_IO_ERROR when the bus failed.
 */
kode_status_t kdrv_aw86233_set(kdrv_aw86233_field_t f, int32_t value);

/** @brief Name, unit and range of @p f; NULL for an invalid field. */
const kdrv_aw86233_field_info_t *kdrv_aw86233_field_info(kdrv_aw86233_field_t f);

/** @brief The field called @p name (as printed by field_info), or KODE_DENIED. */
kode_status_t kdrv_aw86233_field_by_name(const char *name, kdrv_aw86233_field_t *out);

/** @brief Log every field, a few to a line. One line when there is no chip. */
void kdrv_aw86233_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_AW86233_H_ */
