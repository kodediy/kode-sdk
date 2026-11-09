/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_rfid_lf.h
 * @brief The 125 kHz LF front end: the carrier, the demod read window and the T5577 downlink.
 */
#ifndef KDRV_RFID_LF_H_
#define KDRV_RFID_LF_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"
#include "krfid_em4100.h"
#include "krfid_wave.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Bytes of an EM4100 tag's ID (40 bits). */
#define KDRV_RFID_LF_ID_BYTES  5U

/* The demod input is read with adc_continuous on ADC2_CH5 (RFID_RF_IN =
 * GPIO54, kbsp_pins.h), so the unit, channel and the single-unit conversion
 * mode are all named here in one place, and a result is kept only when it is
 * a valid conversion of this unit and channel. ADC2 rather than ADC1 is not
 * cosmetic: ADC1_CH7 is GPIO23, which is C5_EN, and an energised read there
 * would reconfigure the C5's enable line and drop the radio. */
#define KDRV_RFID_LF_ADC_UNIT      ((adc_unit_t)KBSP_ADC_RFID) /**< the demod line is on ADC2. */
#define KDRV_RFID_LF_ADC_CHANNEL   ADC_CHANNEL_5          /**< RFID_RF_IN = GPIO54 = ADC2_CH5. */
#define KDRV_RFID_LF_ADC_CONV      ADC_CONV_SINGLE_UNIT_2 /**< convert on ADC2 alone. */

/* VDD_IO_5 (which RFID_OUT sits in) is fed from internal LDO channel 4; a clone
 * raises it to 3.3 V so the carrier swings full strength and a T5577 harvests
 * enough energy to burn its EEPROM. A read leaves the LDO alone. */
#define KDRV_RFID_LF_LDO_CHANNEL   4                   /**< esp_ldo channel feeding VDD_IO_5 (VO4). */
#define KDRV_RFID_LF_LDO_MV        3300                /**< the boosted rail, in millivolts. */

/**
 * @brief Prepare the front end; energise nothing.
 *
 * Leaves RFID_PULL high at rest (kdrv_rfid_lf_rest()), RFID_OUT driven low and
 * RFID_CARRIER a plain input. Wires no peripheral hot: the carrier stays down
 * and the LDO unheld until kdrv_rfid_lf_field_on(). Safe to call once at
 * startup, and what the park of an absent AO_Rfid calls. Nothing on these pins
 * answers, so it cannot tell whether the front end is fitted.
 *
 * @return KODE_OK, always - a pin that would not configure is logged, not
 *         returned.
 */
kode_status_t kdrv_rfid_lf_init(void);

/**
 * @brief Bring the 125 kHz carrier up.
 *
 * @param boost true to raise VDD_IO_5 to 3.3 V through VO4 for a T5577 write;
 *              false to leave the rail at its boot level, which reads fine.
 * @return KODE_OK with the field energised; KODE_NO_RESOURCES when the
 *         expansion header holds every LEDC timer; KODE_ERROR if the carrier
 *         or the LDO would not come up.
 */
kode_status_t kdrv_rfid_lf_field_on(bool boost);

/**
 * @brief Take the carrier down and release the ADC handle and the LDO.
 *
 * At rest after this: carrier off, the ADC2 continuous handle deleted, the
 * read buffers handed back to PSRAM, VO4 not held. RFID_PULL is left as it
 * was: kdrv_rfid_lf_rest() is what lets it go.
 *
 * @return KODE_OK, always.
 */
kode_status_t kdrv_rfid_lf_field_off(void);

/**
 * @brief Let RFID_PULL go high while nothing uses the receiver.
 *
 * Held low, RFID_PULL sinks 3.3 V through R11 (10 k, on the main PCB): about
 * 0.33 mA for as long as it stays low. High it costs nothing, since it drives
 * Q2's gate (a MOSFET on the antenna PCB) and Q2 then shunts an envelope
 * detector that has no carrier to detect. Every way into the receiver -
 * kdrv_rfid_lf_run(), kdrv_rfid_lf_field_on(), kdrv_rfid_lf_cert_carrier()
 * - drives it low again first. Call it with no job in flight: the owner
 * knows that, the driver keeps no running flag.
 *
 * @return KODE_OK, or KODE_DENIED while the carrier is up.
 */
kode_status_t kdrv_rfid_lf_rest(void);

/**
 * @brief Fill one ~150 ms demod window and decode the EM4100 ID in it. Blocks.
 *
 * Takes the read buffers from PSRAM and creates the adc_continuous handle on
 * first use (both released by kdrv_rfid_lf_field_off()), samples the demod line
 * at 80 kSPS, and runs krfid_em4100_decode() over the window. A DMA result
 * that is not a valid conversion of the demod line is dropped, never taken
 * into the window. The field must already be up.
 * From a task, never from app_main: there the ADC-continuous DMA returns zeros on this silicon.
 *
 * @param[out] id 5-byte ID on a decode (40-bit, most-significant nibble first).
 * @param[out] d  Decoder diagnostics, always filled. May be NULL.
 * @return KODE_OK with @p id on a decode; KODE_TIMEOUT on a flat window or one
 *         with signal but no clean frame; KODE_ERROR for a NULL @p id or if the
 *         ADC could not be brought up; KODE_NO_RESOURCES if PSRAM had no room for
 *         the read buffers.
 */
kode_status_t kdrv_rfid_lf_read_window(uint8_t id[5], krfid_em4100_diag_t *d);

/**
 * @brief Clone a 5-byte EM4100 ID onto the T5577 in the field.
 *
 * Writes three field-gapped T5577 blocks - the EM4100 config word, then the
 * two 32-bit halves of krfid_em4100_frame(id) - yielding one tick between
 * blocks so lower priorities breathe. The field must already be up, boosted.
 *
 * @param id The 5-byte ID to program, most-significant nibble first.
 * @return KODE_OK when the three blocks were sent; KODE_ERROR for a NULL @p id;
 *         KODE_DENIED when the field is not up.
 */
kode_status_t kdrv_rfid_lf_clone(const uint8_t id[5]);

/**
 * @brief Raise, switch or drop the held maximum-drive certification carrier.
 *
 * The FCC §15.209 bench path (the general radiated-emission limits below
 * 490 kHz that govern the 125 kHz fundamental and its harmonics). Brings the
 * carrier up boosted (VDD_IO_5 at 3.3 V, the most drive the half-bridge can
 * give) and HOLDS it - no read window, no worker job - until
 * kdrv_rfid_lf_field_off() takes it back down. With @p modulated false the
 * carrier is bare, for the fundamental field-strength and harmonic
 * measurements. With it true the carrier is OOK-gated continuously by a
 * driver-internal task - the LEDC timer paused and resumed in a fixed pattern -
 * for the occupied-bandwidth measurement, the fixed-pattern fallback this front
 * end allows since it has no modulation primitive of its own.
 *
 * Switching carrier<->modulated is safe: this joins any prior modulation task
 * and drops the field first, so no task or half-gated timer is left behind.
 *
 * Only compiled when CONFIG_KODEOS_NFC_CERT is set; the symbol is absent in a
 * production build, which is why nothing outside the certification app links a
 * maximum-drive LF capability.
 *
 * @param modulated false for a bare carrier, true for an OOK-modulated one.
 * @return KODE_OK when the field is up; KODE_ERROR when the carrier or its
 *         modulation task would not come up.
 */
kode_status_t kdrv_rfid_lf_cert_carrier(bool modulated);

/* ---- The jobs -------------------------------------------------------------
 * The blocking primitives above must run from a task, not from an Active
 * Object step (a read window is ~150 ms). The driver owns no task: AO_Rfid
 * runs kdrv_rfid_lf_run() as a job on its own worker (kqp_worker), which
 * answers with the status and leaves the ID and the bit rate in the memory
 * the AO lent. One job at a time, because a job's answer travels through this
 * driver's own field and a second job would energise it under the first;
 * the AO serialises, as it always has.
 */

/** @brief What a job can be asked to do. */
typedef enum {
    KDRV_RFID_JOB_READ = 0,   /**< Energise, read windows for @c ms, decode.  */
    KDRV_RFID_JOB_CLONE,      /**< Boost, clone @c id, read one back to verify.*/
    KDRV_RFID_JOB_CAPTURE,    /**< No carrier: one @c ms window, described.  */
    KDRV_RFID_JOB_EMULATE,    /**< Play @c id as an EM4100 tag until EMU_STOP. */
    KDRV_RFID_JOB_EMU_STOP,   /**< End the tag emulation; harmless with none. */
} kdrv_rfid_lf_job_type_t;

/**
 * @brief One job, carrying copies of everything it needs (kodeOS rule 7) -
 *        except a CAPTURE's answer, which it writes through pointers.
 *
 * @c ms bounds a READ's window loop and a CLONE's field-up time, and is a
 * CAPTURE's window length; @c id is the ID to write or to play and is
 * meaningless for a READ, a CAPTURE or an EMU_STOP.
 *
 * The four pointers are a CAPTURE's and NULL for anything else. They are the
 * submitter's, lent for this one job: kdrv_rfid_lf_run() writes through them
 * before it returns and never after, so the submitter owns them again once
 * the job's answer has arrived, and not before - whatever it may have told a
 * requester of its own meanwhile. They are written only when the capture
 * answers KODE_OK; any other answer leaves them as they were. Each may be
 * NULL, and a NULL one is skipped, not refused.
 */
typedef struct {
    kdrv_rfid_lf_job_type_t type;  /**< What to do.                          */
    uint32_t                ms;    /**< How long, in milliseconds.           */
    uint8_t                 id[5]; /**< A CLONE's ID.                        */
    krfid_field_t          *field; /**< A CAPTURE's statistics, or NULL.     */
    uint16_t               *wave;  /**< A CAPTURE's waveform, or NULL.       */
    size_t                  cap;   /**< Points @c wave holds.                */
    size_t                 *n;     /**< Points written to @c wave, or NULL.  */
} kdrv_rfid_lf_job_t;

/**
 * @brief Tell the driver how to recognise the task a capture may run on.
 *
 * kdrv_rfid_lf_field_capture() runs only there: ADC-continuous returns zeros
 * from app_main, and the AO's jobs are serialised, so the window buffers and
 * the ADC handle are never shared. The driver cannot name the AO's worker
 * (that type sits above it), so the AO hands it the question. Call it once,
 * before the first job.
 *
 * @param is_worker Answers true on the worker's own task and false anywhere
 *                  else. NULL clears it, and no capture then runs.
 */
void kdrv_rfid_lf_set_worker_check(bool (*is_worker)(void));

/**
 * @brief Run one job, blocking until it ends. Call it from the worker only.
 *
 * A READ energises the carrier, reads windows until a decode or @c ms, and
 * drops the carrier; a CLONE boosts, writes, reads one window back to verify
 * and drops the carrier; a CAPTURE is kdrv_rfid_lf_field_capture().
 *
 * @param job    The job.
 * @param id     Out: a READ's decoded ID, or a CLONE's read-back; zeros on
 *               failure and for a CAPTURE, an EMULATE and an EMU_STOP.
 * @param rf_div Out: the bit rate it was read at (16, 32 or 64), 0 when none.
 * @return KODE_OK with a decode, a capture, a loop running or stopped;
 *         KODE_TIMEOUT when nothing decoded; KODE_NO_RESOURCES or KODE_ERROR
 *         from an EMULATE; KODE_DENIED for a NULL argument, a capture off the
 *         worker, or an EMULATE while one runs.
 */
kode_status_t kdrv_rfid_lf_run(kdrv_rfid_lf_job_t const *job, uint8_t id[5],
                               uint8_t *rf_div);

/**
 * @brief The longest carrier-off capture, in milliseconds: one whole read
 *        window, KRFID_EM4100_MAX_SAMPLES (12000) samples at 80 kS/s.
 *
 * The one place the number lives: the driver cuts a longer capture to it, and
 * whoever caps a capture request above the driver caps it here.
 */
#define KDRV_RFID_LF_CAPTURE_MS_MAX 150U

/**
 * @brief Read the demod line for @p ms with the carrier OFF and describe it.
 *        Blocks for the window, on the worker only.
 *
 * What kdrv_rfid_lf_run() does for a KDRV_RFID_JOB_CAPTURE, and the one place
 * a capture is made: the read buffers and the ADC handle are taken for the one
 * window and handed back when it ends, as a read hands them back. AO_Rfid
 * submits the job; it never calls this. A caller on any other task than the
 * one kdrv_rfid_lf_set_worker_check() names is refused, app_main included,
 * where the DMA returns zeros - so the window is only ever filled by the
 * worker, one job at a time. The window holds only
 * valid conversions of the demod line, as a read's does.
 *
 * Nothing here can tell that the front end is missing: on a board without
 * the Antenna PCB the window is whatever the unconnected RFID_RF_IN floats
 * to, and the capture answers KODE_OK over it.
 *
 * @param ms   The window, 1..KDRV_RFID_LF_CAPTURE_MS_MAX ms; a longer one is
 *             cut to the maximum.
 * @param out  The statistics, or NULL to discard them.
 * @param wave Where the waveform goes, or NULL for statistics only.
 * @param cap  Points @p wave holds. The window is decimated by taking the peak
 *             of each bucket, so a narrow gap is kept, never averaged away.
 * @param n    Points written to @p wave, 0 with no @p wave; or NULL, which
 *             still fills @p wave.
 *
 * @p out, @p wave and @p n are written only on KODE_OK and left as they were
 * on any other answer.
 *
 * @return KODE_OK; KODE_DENIED from any task but the worker (and before the
 *         check is set), or while the carrier is up; KODE_ERROR for a zero @p ms, or when the ADC refused
 *         or delivered nothing; KODE_NO_RESOURCES when PSRAM had no room for
 *         the window.
 */
kode_status_t kdrv_rfid_lf_field_capture(uint32_t ms, krfid_field_t *out,
                                         uint16_t *wave, size_t cap, size_t *n);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_RFID_LF_H_ */
