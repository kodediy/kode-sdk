/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_es8311.h
 * @brief The audio path: ES8311 codec, NS4150B amplifier, ZTS6211E microphone.
 */
#ifndef KDRV_ES8311_H_
#define KDRV_ES8311_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"
#include "ktone.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The chip ------------------------------------------------------------ */

/** @brief What CHIP ID1 (0xFD) reads on an ES8311. */
#define KDRV_ES8311_ID1          0x83U
/** @brief What CHIP ID2 (0xFE) reads on an ES8311. */
#define KDRV_ES8311_ID2          0x11U

/** @brief The one sample rate. Asserted equal to KTONE_RATE_HZ in the .c. */
#define KDRV_ES8311_RATE_HZ     48000U
/** @brief The one sample width. */
#define KDRV_ES8311_BITS           16U
/**
 * @brief The one MCLK multiple.
 *
 * Not a preference. The codec layer re-initialises the I2S bus at open() with
 * this multiple whatever was asked for, so passing anything else only
 * desynchronises the chip's internal divider from the real clock - measured,
 * and it poisons capture with a rock-steady false tone at a eighteenth of the
 * sample rate. For many rate-by-384 products the chip driver's coefficient
 * table has no row at all, and open() then fails with no sound and no error
 * anyone sees.
 */
#define KDRV_ES8311_MCLK_MULT     256U

/**
 * @brief The longest sound one buffer holds, in seconds.
 *
 * 96 KB a second. Here rather than with the facade because the buffer is
 * here: raising it is what a streaming slice would replace rather than
 * change, and every caller above sees the same ceiling.
 */
#define KDRV_ES8311_BUF_SECONDS    30U
/** @brief The same, in frames. */
#define KDRV_ES8311_BUF_FRAMES  (KDRV_ES8311_BUF_SECONDS * KDRV_ES8311_RATE_HZ)

/**
 * @brief What register 0x44 must read once the device is open.
 *
 * The chip can route its own DAC output into the ADC's right slot, and the
 * codec layer does exactly that unless told not to. With it on, an acoustic
 * test would pass with a dead microphone AND a dead speaker - a digital
 * loopback with no air in it. This driver turns it off; the production row
 * reads it back and abandons itself if it is not this value, because nothing
 * else the row measures would mean anything.
 */
#define KDRV_ES8311_DAC2ADC_OFF  0x08U

/* ---- The speaker's cap --------------------------------------------------- */

/** @brief The top of the volume scale every caller sees, the capsule too. */
#define KDRV_ES8311_VOL_MAX        100U

/** @brief The top of the microphone gain scale, dB: six-decibel steps. */
#define KDRV_ES8311_MIC_DB_MAX      42U

/**
 * @brief The loudest the DAC is ever set, in tenth-decibels of full scale.
 *
 * Volume 100 is this and nothing reaches past it. Zero: the DAC's full
 * scale, register 0x32 at 0xBF. It is the speaker's limit rather than the
 * codec's, from the validation bench's model. Full scale is 1.9 Vrms at the
 * codec; the NS4150B multiplies that by its internal feedback resistor
 * (measured 220 to 250 kOhm, datasheet 240) over R2/R10; and the YN1811 may
 * dissipate 0.5 W, its maker's 96-hour test, at the 5.85 Ohm valley of its
 * impedance, which is 1.71 Vrms. Every board that ships, V1 (the DVT) and
 * V2, fits 270 kOhm at R2/R10, which meets that at full scale (1.69 Vrms,
 * 0.49 W measured). A board with smaller resistors needs a value below zero
 * here: V0 fitted 100 kOhm.
 */
#define KDRV_ES8311_OUT_DB_X10_MAX  0

/* ---- Bring-up ------------------------------------------------------------ */

/**
 * @brief Bring the codec up: control interface, identity, I2S channels.
 *
 * Makes no sound and enables nothing. The device stays CLOSED - the analog
 * blocks are down, the DAC is muted and both I2S channels are idle - until
 * an OPEN job has been through kdrv_es8311_run(). The I2S master clock's pad
 * is an input with the pull-up until then, and again after every CLOSE.
 * That is deliberate: the amplifier is enabled by the C5 from its own boot,
 * so an open DAC at rest would be a live class-D on a battery product.
 *
 * The identity is read here and checked, which the codec layer does not do.
 * Absence is a state, not an error: on failure kdrv_es8311_present() is false
 * for good and every other call in this header returns KODE_DENIED at once
 * rather than retrying a chip that is not there.
 *
 * @return KODE_OK when 0x18 answered and identified as an ES8311.
 *         KODE_ERROR when it did not. KODE_DENIED on a second call.
 */
kode_status_t kdrv_es8311_init(void);

/**
 * @brief Power the codec down, for a board with no audio owner.
 *
 * The codec layer's own suspend, written straight to 0x18 over the shared
 * bus: no codec component, no I2S channel, so the link drops both. The chip
 * keeps no handle afterwards. Never with AO_Audio running.
 *
 * @return KODE_OK with the codec down; KODE_ERROR when no ES8311 answered;
 *         KODE_IO_ERROR when a write was refused.
 */
kode_status_t kdrv_es8311_park(void);

/** @brief What the audio path is and what it is set to. */
typedef struct {
    uint32_t rate;     /**< Always KDRV_ES8311_RATE_HZ.                   */
    bool     present;  /**< A codec answered and identified itself.       */
    bool     open;     /**< It is open: analog up and the DAC live.       */
    bool     busy;     /**< Always false here: whether a job is with the
                        *   worker is the owner's to say, and AO_Audio
                        *   overwrites it when it answers.                 */
    uint8_t  bits;     /**< Always KDRV_ES8311_BITS.                      */
    uint8_t  volume;   /**< 0 to 100.                                     */
    uint8_t  mic_db;   /**< 0 to 42, in 6 dB steps.                       */
    bool     mute;     /**< The output is muted.                          */
} kdrv_es8311_info_t;

/**
 * @brief Fill in what the audio path is and what it is set to.
 *
 * Driver state and constants only: no bus, no wait, nothing touched. Safe
 * from any context.
 *
 * @param out Where to write. NULL is a no-op.
 */
void kdrv_es8311_info(kdrv_es8311_info_t *out);

/** @brief Did kdrv_es8311_init() find and identify the chip? */
bool kdrv_es8311_present(void);

/** @brief Is the device open - analog up, channels enabled, DAC live? */
bool kdrv_es8311_is_open(void);

/* ---- The owner's buffer, for a recording on its way out ------------------ */

/**
 * @brief The owner's PSRAM buffer, allocated on demand.
 *
 * FOR AO_AUDIO'S THREAD ONLY. A recording to a file lives in here from the
 * moment the worker fills it until AO_Storage has written it out, and the
 * owner taking one job at a time is what keeps a second job away from it
 * meanwhile - but only for callers that go through the owner. Anything
 * copying in or out on a thread of its own would meet that recording in the
 * middle, so nothing above the owner calls this: the facade stages its own
 * frames in memory it owns. With one thread as the only caller, the lazy
 * allocation needs no lock.
 *
 * Allocated on first use and never freed: a firmware that never makes a
 * sound should not carry megabytes for one, and asking for and releasing a
 * block that size repeatedly fragments PSRAM.
 *
 * There is room for a file header in front of the samples, so that writing a
 * recording out is one contiguous transfer rather than two - see
 * kdrv_es8311_buf_file().
 *
 * @param frames Out: how many frames it holds. May be NULL.
 * @return The samples, or NULL when it could not be allocated.
 */
int16_t *kdrv_es8311_buf(uint32_t *frames);

/**
 * @brief Put a WAV header in front of @p frames of the buffer.
 *
 * @param frames How many frames of it are the recording.
 * @param bytes  Out: how long the whole file image is.
 * @return Where the file image starts, or NULL.
 */
uint8_t *kdrv_es8311_buf_file(uint32_t frames, uint32_t *bytes);

/* ---- The jobs ------------------------------------------------------------ */

/** @brief What one job asks the driver to do. */
typedef enum {
    KDRV_ES8311_JOB_OPEN = 0, /**< Open at the board's format and apply the
                               *   volume, gain and mutes the field table
                               *   holds. There is no second way to set
                               *   those: a job that carried its own copy
                               *   would be a mechanism doing a job another
                               *   mechanism already does. The master clock
                               *   is routed to its pad first.              */
    KDRV_ES8311_JOB_CLOSE,    /**< Mute the DAC, let its ramp reach silence,
                               *   then close; the analog blocks go down. The
                               *   order is what keeps the close from putting
                               *   a step into an amplifier this side never
                               *   disables. The master clock's pad is let go
                               *   last.                                      */
    KDRV_ES8311_JOB_TONE,     /**< Emit one candidate for a while. Captures
                               *   nothing, and is the console's verb. After
                               *   a fresh open the settle blocks of silence
                               *   come first, once, and the DAC is un-muted
                               *   at the end of them; the sound ends with a
                               *   drain of silence.                          */
    KDRV_ES8311_JOB_FLOOR,    /**< Capture one window while emitting silence.  */
    KDRV_ES8311_JOB_PROBE,    /**< Emit one candidate AND capture one window,
                               *   then drain: the tail blocks are still the
                               *   tone, so without it the row ends every
                               *   probe on a step.                           */
    KDRV_ES8311_JOB_PLAY,     /**< Write @c frames mono frames from @c buf.
                               *   After a fresh open the settle blocks of
                               *   silence come first, once, and the DAC is
                               *   un-muted at the end of them. The job ends
                               *   with a drain of silence, so its outcome
                               *   means the SPEAKER has heard the sound and
                               *   not merely that the ring has taken it.     */
    KDRV_ES8311_JOB_RECORD,   /**< Read up to @c frames mono frames into @c buf. */
    KDRV_ES8311_JOB_STREAM,   /**< Play the stream's ring for as long as the
                               *   stream is open: whatever the writer put in,
                               *   a block at a time, and silence while it is
                               *   dry. Once kdrv_es8311_stream_end() has been
                               *   called it plays what is left and ends with
                               *   the usual drain.                           */
} kdrv_es8311_job_t;

/**
 * @brief One job for kdrv_es8311_run().
 *
 * Only @c buf is retained, and only for the job's own duration: whatever it
 * points at must outlive the job.
 */
typedef struct {
    kdrv_es8311_job_t type;
    uint8_t  bin;      /**< TONE and PROBE: which ktone candidate.           */
    uint16_t ms;       /**< TONE: how long. PROBE has its own timing.        */
    int16_t  amp;      /**< TONE and PROBE: peak amplitude out of 32768.     */
    int16_t *buf;      /**< PLAY and RECORD: mono 16-bit frames.             */
    uint32_t frames;   /**< PLAY and RECORD: how many.                       */
} kdrv_es8311_req_t;

/**
 * @brief Run one job, and return when it has ended.
 *
 * Blocking: a job moves samples, waits out a ramp or sits on the stream's
 * ring, and takes as long as that takes. It is meant for the owner's worker
 * task, never for an Active Object's step, and one at a time - the result
 * table below and the device's state are shared by every job, so the owner
 * runs the next only after the last has returned. The driver keeps no task,
 * queue or callback of its own.
 *
 * @param req The job. Only @c buf is kept, until the job returns.
 * @return What the job came to: KODE_OK, or the status of the step that
 *         failed. KODE_DENIED for a NULL @p req, a job type this driver does
 *         not have, or an absent chip.
 */
kode_status_t kdrv_es8311_run(const kdrv_es8311_req_t *req);

/** @brief What a FLOOR, PROBE, PLAY, RECORD or STREAM job came back with. */
typedef struct {
    ktone_measure_t win;    /**< FLOOR and PROBE: the window.                */
    uint32_t        frames; /**< PLAY, RECORD, STREAM: how many frames moved. */
    uint8_t         bin;    /**< PROBE and TONE: which candidate was emitted. */
} kdrv_es8311_result_t;

/**
 * @brief Copy out the last job's result.
 *
 * Read after the job returned and before the next one runs. Left untouched
 * when @p out is NULL.
 *
 * @param out Where to write.
 */
void kdrv_es8311_result(kdrv_es8311_result_t *out);

/* ---- The stream ------------------------------------------------------------
 * Sound that arrives a piece at a time, for as long as its writer likes - a
 * game's mixer, a decoder - rather than as one buffer handed over whole. The
 * frames travel through a ring this driver owns, 200 ms of them: the writer
 * puts, KDRV_ES8311_JOB_STREAM takes a block at a time, and nothing but the
 * ring sits between the two. Opening and closing it are the owner's, like
 * every other job; only the frames skip the owner, the way the panel's pixels
 * skip AO_Display, because one event per few milliseconds of sound would be
 * the owner's whole queue.
 */

/** @brief How many frames the stream's ring holds: 200 ms at the board's rate. */
#define KDRV_ES8311_STREAM_FRAMES  (KDRV_ES8311_RATE_HZ / 5U)

/**
 * @brief Empty the stream's ring and mark the stream open.
 *
 * FOR AO_AUDIO'S THREAD ONLY, before it submits KDRV_ES8311_JOB_STREAM. The
 * ring is allocated in PSRAM on first use and never freed, for the reason
 * kdrv_es8311_buf() gives.
 *
 * @return KODE_OK, or KODE_NO_RESOURCES when there is no PSRAM for the ring.
 */
kode_status_t kdrv_es8311_stream_reset(void);

/**
 * @brief Put frames into the open stream. The writer's call, from its own task.
 *
 * Blocks only while the ring is full, for at most @p ms in all, and never
 * from an Active Object. One writer at a time.
 *
 * @param frames   @p n mono samples, or @p n left-right pairs when
 *                 @p channels is 2; a pair is played as its mean.
 * @param n        How many frames.
 * @param channels 1 or 2.
 * @param ms       The longest to wait for room.
 * @param taken    Out: how many frames went in. May be NULL.
 * @return KODE_OK when all @p n went in, KODE_TIMEOUT when @p ms ran out
 *         first (@p taken says how far it got), KODE_DENIED when no stream is
 *         open or an argument is bad.
 */
kode_status_t kdrv_es8311_stream_write(const int16_t *frames, size_t n,
                                       uint8_t channels, uint32_t ms,
                                       size_t *taken);

/**
 * @brief Say that no more frames are coming.
 *
 * FOR AO_AUDIO'S THREAD ONLY. The stream job plays what is still in the ring,
 * drains and returns; writes are refused from this call on.
 */
void kdrv_es8311_stream_end(void);

/**
 * @brief Clear a recording's early end, before its job is submitted.
 *
 * FOR AO_AUDIO'S THREAD ONLY.
 */
void kdrv_es8311_record_reset(void);

/**
 * @brief End the recording in progress at the next block.
 *
 * FOR AO_AUDIO'S THREAD ONLY. The RECORD job stops at its next block
 * boundary and returns KODE_OK with the frames captured so far.
 */
void kdrv_es8311_record_end(void);

/* ---- Registers, and the field table -------------------------------------- */

/**
 * @brief Read one ES8311 register.
 *
 * Goes through the codec's I2C control interface rather than the codec
 * layer's own register call, which refuses unless the device is open. This
 * one answers in every state, so the console's `audio` is a question about
 * the codec and never a change to it. Safe from an Active Object: one I2C
 * transaction, no DMA, no wait.
 *
 * @param reg The register.
 * @param val Out. Left untouched unless KODE_OK, so a failed bus read never
 *            becomes a plausible reading of the chip.
 * @return KODE_OK, KODE_DENIED when the chip is absent or @p val is NULL,
 *         KODE_IO_ERROR when the bus said no.
 */
kode_status_t kdrv_es8311_reg_read(uint8_t reg, uint8_t *val);

/**
 * @brief Write one ES8311 register. Same path, same statelessness.
 *
 * @param reg The register.
 * @param val What to write.
 * @return KODE_OK, KODE_DENIED when the chip is absent, KODE_IO_ERROR when
 *         the bus said no.
 */
kode_status_t kdrv_es8311_reg_write(uint8_t reg, uint8_t val);

/**
 * @brief Push the field table's volume, gain and mutes at an open device.
 *
 * A no-op when it is closed: the next open applies them anyway. Called from
 * kes_fields.c when one of them is written, and from this file after every
 * open - which is the whole reason it is public rather than static.
 */
void kdrv_es8311_apply_settings(void);

/**
 * @brief Silence the converter NOW, from the calling thread, and stay silent.
 *
 * One masked register write and no wait, so it is safe from an Active
 * Object's step - the same budget a field read already spends. It takes the
 * driver's own hold, never the caller's @c out_mute, and the next sound
 * lifts it the way every sound does.
 *
 * It exists for the one moment that cannot be scheduled: the board is about
 * to open the BATFET, and a mute handed to the codec worker might not run
 * before the rails go. A no-op on a closed or absent device.
 */
void kdrv_es8311_hush(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_ES8311_H_ */
