/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kaos.h
 * @brief kodeOS: every Active Object in the system, and nothing else.
 */
#ifndef KAOS_H_
#define KAOS_H_

#include <stdbool.h>
#include <stddef.h>

#include "sdkconfig.h" /* CONFIG_KODE_LOWPOWER */
#include "kqp.h"
#include "khal_sync.h"
#include "kode_log.h"
#include "kdrv_slot.h"
#include "kdrv_qmc5883p.h"
#include "kdrv_radio.h"
#include "kdrv_storage.h"
#include "knfc_emulate.h"
#include "kode_functions.h"  /* function_ticket() and the rest of a function that answers */
#include "kode_config.h"     /* kode_config_post(), kode_config_answer() */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Linked or not --------------------------------------------------------
 * An owner starts only when the app links it: kode_init() names each optional
 * start through a weak reference, NULL when the owner is not linked. A door
 * shares its owner's object, so linking a door links the start
 * (tools/check_starts.sh). In the OS role kode_init() names every start strongly.
 *
 * MISRA C:2012 deviation, the only one in this tree: a weak reference to an
 * unlinked module breaks Rule 1.2 (GCC extension), Rule 8.6 (no external
 * definition) and Rule 1.3 (C11 6.9p5). C has no other way to ask the linker
 * whether a module is in the image; GNU ld resolves an undefined weak symbol
 * to 0, and nothing calls through one without kaos_linked() or equivalent.
 * cppcheck sees none of the three, so no site suppresses them.
 */
#define KAOS_WEAK __attribute__((weak))   /**< A reference that does not link what it names. */

/** @brief An owner's start, as kode_init() and the linked-or-not test name it. */
typedef void (*kaos_start_fn_t)(void);

/**
 * @brief Whether the owner whose start is @p start is in the image.
 *
 * Its doors share its object: when this says yes, every door of that owner
 * named weakly resolves too.
 *
 * @param start The owner's start, through a KAOS_WEAK declaration.
 * @return true when the app linked the owner.
 */
bool kaos_linked(kaos_start_fn_t start);

/**
 * @brief Whether @p who is a started Active Object, one an answer can reach.
 *
 * Every door that takes an asker refuses with KODE_DENIED when this says no.
 *
 * @param who The asking AO.
 * @return false for NULL, or an AO not started (priority 0) or out of range.
 */
bool kaos_asker_started(QActive const *who);

/* ---- Parked: an owner the app did not link --------------------------------
 * kode_init() calls one of these for each optional owner found absent. Each
 * claims the owner's resources as `parked(AO_<x>)`, so `ps` names it and an
 * app's kaos_claim() still fails, and leaves the part in its lowest-power safe
 * state. They live in kaos_park.c: a reference to a park never links an owner.
 */
/** @brief AO_Storage absent: claim the slot and the FAT, and switch the card's supply off. */
void kaos_park_storage(void);
/**
 * @brief AO_Radio absent (KODEOS_RADIO=n included): claim the C5.
 *
 * Held in reset on C5_EN, the amplifier with it. With KODEOS_C5_RUN, or with
 * AO_Audio linked (IO4 is the amplifier's enable), reset once and left running.
 */
void kaos_park_c5(void);
/** @brief AO_LedMgr absent: claim the KTD2026 and bring it up dark. */
void kaos_park_led(void);
/** @brief AO_Ir absent: claim the infrared block; it is the C5's, and idle unless asked. */
void kaos_park_ir(void);
/** @brief AO_Nfc absent: claim the ST25R3916B and put it in power-down, its field off. */
void kaos_park_nfc(void);
/** @brief AO_Rfid absent: claim the LF front end; the carrier down, the receiver at rest. */
void kaos_park_rfid(void);
/** @brief AO_Audio absent: claim the ES8311 and power it down; with AO_Radio linked, ask it for the amplifier off. */
void kaos_park_audio(void);
/** @brief AO_Imu absent: claim the QMI8658A and the QMC5883P and leave both at rest. */
void kaos_park_imu(void);
/** @brief AO_Haptic absent: claim the AW86233 and leave it in standby. */
void kaos_park_haptic(void);
/** @brief AO_AppMgr absent: claim the apps region, the mailbox and the kodeOS slot. */
void kaos_park_appmgr(void);
/** @brief AO_Touch absent: claim the CST820; with no AO_Display either, send it to deep sleep (the panel's bring-up would reset it). */
void kaos_park_touch(void);
/** @brief AO_Display absent: claim the panel and ask AO_IoExp to cut its supply, VCI_EN on P7. */
void kaos_park_display(void);

/* ---- AO_LedMgr: owner of the LED ---------------------------------------
 * Subscribes to every LED signal (see kqp_signals.h), so a publisher needs no
 * reference to it; the facade's LED commands post to it directly, and naming
 * it is what links it into an app.
 */
extern QActive * const AO_LedMgr;   /**< The LED owner AO. */
void LedMgr_ctor(void);             /**< Construct it; kaos_ledmgr_start() calls it. */
/** @brief Construct AO_LedMgr and start it at KODE_PRIO_LED, on the queue and stack its own file holds. */
void kaos_ledmgr_start(void);
/** @brief Register its console verbs and its self-test rows; kaos_ledmgr_start() calls it. */
void kaos_ledmgr_register(void);

/**
 * @brief Ask AO_LedMgr to self-test the LED - `test component=led`.
 *
 * Verdict: a published TEST_RESULT_SIG "led". Red, green and blue are lit in
 * turn as LED_TEST_SIG questions; BUTTON_TOP confirms, BUTTON_BOT denies, and
 * KODE_LED_TEST_STEP_MS with no answer fails. Ends on the first unconfirmed colour.
 * While it runs, other LED signals are refused and button presses are answers:
 * no other listener may act on them between LED_TEST_ASK and LED_TEST_DONE.
 */
void led_test_request(void);

/* ---- AO_Ir: owner of the infrared block ---------------------------------
 * Two emitters and a TSOP75338 receiver, wired to the C5 and driven by its
 * infrared service, so infrared needs KODEOS_RADIO; AO_Ir asks AO_Radio for the
 * link (radio_up_request()) when it is down. The receiver stays armed: a remote's
 * frame arrives as a published IR_RX_SIG. It is deaf to its own transmissions
 * plus KODE_IR_ECHO_TAIL_MS. Raw frames are lent by pointer (ir_send_raw_request()).
 */
extern QActive * const AO_Ir;       /**< The infrared owner AO. */
void Ir_ctor(void);                 /**< Construct it; kaos_ir_start() calls it. */
/** @brief Construct AO_Ir and start it at KODE_PRIO_IR, on the queue and stack its own file holds. */
void kaos_ir_start(void);
/** @brief Register its console verbs and its self-test rows; kaos_ir_start() calls it. */
void kaos_ir_register(void);

/* ---- AO_Autotest: the self-test sequencer --------------------------------
 * Owns no device. Holds the table of self-test rows, asks each of its owner
 * one at a time, and publishes what happened: AUTOTEST_BEGIN_SIG,
 * AUTOTEST_ROW_SIG, AUTOTEST_END_SIG. One run at a time, whoever asked.
 */
extern QActive * const AO_Autotest; /**< The sequencer AO. */
void Autotest_ctor(void);           /**< Construct it; kaos_autotest_start() calls it. */
/** @brief Construct AO_Autotest and start it at KODE_PRIO_AUTOTEST, on the queue and stack its own file holds. */
void kaos_autotest_start(void);
/** @brief Register its self-test rows; kaos_autotest_start() calls it. */
void kaos_autotest_register(void);

/**
 * @brief Ask AO_Autotest for a run (AUTOTEST_RUN_REQ_SIG).
 *
 * Answered with AUTOTEST_BEGIN_SIG: taken, busy, or unknown @p name. A run then
 * publishes AUTOTEST_ROW_SIG at each row's start and end, AUTOTEST_HOLD_SIG where
 * `user` waits, and AUTOTEST_END_SIG. All carry @p client; a client reads only its own.
 *
 * @param client AUTOTEST_CLIENT_*: who asks.
 * @param group  AUTOTEST_G_MAIN or AUTOTEST_G_USER, or
 *               AUTOTEST_G_NONE for one row by name.
 * @param name   The row when @p group is AUTOTEST_G_NONE; NULL otherwise.
 */
void autotest_run_request(uint8_t client, uint8_t group, const char *name);

/**
 * @brief End the run in flight (AUTOTEST_CANCEL_REQ_SIG).
 *
 * Only the run's own client may end it. A row in flight finishes first;
 * AUTOTEST_END_SIG says cancelled, and no report is kept.
 *
 * @param client AUTOTEST_CLIENT_*: who asks.
 */
void autotest_cancel_request(uint8_t client);

/**
 * @brief Answer a hold: the run goes on to the rows that need hands
 *        (AUTOTEST_CONTINUE_SIG).
 *
 * @param client AUTOTEST_CLIENT_*: who answers; the run's own client.
 */
void autotest_continue(uint8_t client);

/**
 * @brief Ask for the report kept from the last whole `user` run
 *        (AUTOTEST_REPORT_REQ_SIG).
 *
 * Published as AUTOTEST_REPORT_SIG: one event per row, then a marker with
 * the count, the problems and the time of the run. No report kept is a
 * marker alone, with no rows.
 *
 * @param client AUTOTEST_CLIENT_*: who asks.
 */
void autotest_report_request(uint8_t client);

/**
 * @brief What a row is, in plain words.
 *
 * A constant string of the table, the same for the life of the image.
 *
 * @param id AUTOTEST_ID_*.
 * @return The label; an empty string for an id this build has no row for.
 */
const char *autotest_row_label(uint8_t id);

/**
 * @brief What to tell somebody when a row fails, in plain words.
 *
 * @param id AUTOTEST_ID_*.
 * @return The line; an empty string for an id this build has no row for.
 */
const char *autotest_row_problem(uint8_t id);

/**
 * @brief The word that selects a row on the console.
 *
 * @param id AUTOTEST_ID_*.
 * @return The name; an empty string for an id this build has no row for.
 */
const char *autotest_row_name(uint8_t id);

/* ---- AO_Audio: owner of the audio path ------------------------------------
 * One ES8311 does both directions: the analog MEMS mic into its ADC, the speaker
 * off its DAC through the amplifier. Nothing here blocks: sample moves run on
 * kdrv_es8311's worker and come back as events (its priority rests on this).
 * The codec is closed at rest, opens on demand and closes a couple of seconds
 * after the last job. One activity at a time, no queue: a second is refused.
 * Without a fitted codec it still starts, and refuses every request.
 */
extern QActive * const AO_Audio;    /**< The audio path's owner AO. */
void Audio_ctor(void);              /**< Construct it; kaos_audio_start() calls it. */
/**
 * @brief Construct AO_Audio and start it at KODE_PRIO_AUDIO, on its own queue and
 *        stack, then register its settings.
 */
void kaos_audio_start(void);
/** @brief Register its console verbs and its self-test rows; kaos_audio_start() calls it. */
void kaos_audio_register(void);

/**
 * @brief Ask AO_Audio to emit one tone (AUDIO_TONE_REQ_SIG).
 *
 * @p hz is snapped to the nearest of the eight detector bins, and the outcome
 * reports the snapped value. Posts and returns - safe from an Active Object.
 *
 * @param hz      What to emit. Snapped.
 * @param ms      How long to hold it.
 * @param amp_pct Peak amplitude as a percentage of full scale.
 */
void audio_tone_request(uint16_t hz, uint16_t ms, uint8_t amp_pct);

/**
 * @brief The event audio_tone_request() posts, built for a caller that posts it itself.
 * @param hz      What to emit; snapped by AO_Audio.
 * @param ms      How long to hold it.
 * @param amp_pct Peak amplitude as a percentage of full scale.
 * @return The event, or NULL when the pool is dry.
 */
QEvt const *audio_tone_evt(uint16_t hz, uint16_t ms, uint8_t amp_pct);

/**
 * @brief Read, write or dump one codec field (AUDIO_FIELD_REQ_SIG).
 *
 * The same FIELD_OP_* verbs as the PMIC, the gauge and the expander. Answered
 * with a published AUDIO_FIELD_SIG in every state; a dry pool drops the request.
 *
 * @param op    FIELD_OP_GET, FIELD_OP_SET or FIELD_OP_DUMP.
 * @param field kdrv_es8311_field_t, already resolved from its name.
 * @param value For FIELD_OP_SET, in the field's own unit.
 */
void audio_field_request(uint8_t op, uint16_t field, int32_t value);

/**
 * @brief Set the volume, the mic gain or the mute; only the latest value counts
 *        (AUDIO_SETTING_REQ_SIG).
 *
 * Queues at most one request, which writes the value held when handled, so a
 * loop cannot fill AO_Audio's queue. Answered with AUDIO_FIELD_SIG. Safe from an AO.
 *
 * @param field KDRV_ES_VOL, KDRV_ES_MIC_DB or KDRV_ES_OUT_MUTE; any other is ignored.
 * @param value In the field's own unit.
 * @return true when queued or folded into the queued request; false for another
 *         field or a dry pool, and then nothing applies it until the next call.
 */
bool audio_setting_request(uint16_t field, int32_t value);

/**
 * @brief Take the latest value of a setting and open its door again.
 *
 * FOR AO_AUDIO, and for a stand-in that handles AUDIO_SETTING_REQ_SIG.
 *
 * @param field As for audio_setting_request().
 * @return The latest value; 0 for a field without a door.
 */
int32_t audio_setting_take(uint16_t field);

/**
 * @brief Ask AO_Audio to self-test the audio path - `test component=audio`.
 *
 * Verdict: a published TEST_RESULT_SIG "audio". Step 1 listens in silence and
 * refuses a bench too loud; step 2 emits two random frequencies, each of which
 * must come back above the others and above step 1's floor. Readings are logged.
 */
void audio_test_request(void);

/**
 * @brief Ask what the audio path is and what it is set to (AUDIO_INFO_REQ_SIG).
 *
 * Answered within the step, through @p out, generation stamp included. @p out
 * must outlive the request, which the bounded wait on @p done guarantees.
 *
 * @param out  A kdrv_es8311_info_t to fill in.
 * @param done Signalled once @p out is written.
 * @param gen  The caller's tag for this request.
 * @param ack  Where @p gen is written back, just before @p done.
 */
void audio_info_request(void *out, khal_sem_t done, uint32_t gen,
                        uint32_t *ack);

/**
 * @brief Record for @p ms and put the result in @p path (AUDIO_REC_FILE_REQ_SIG).
 *
 * @p path arrives already resolved: AO_Audio does not know the current directory.
 * The bytes go out through AO_Storage's whole-buffer write, with no size ceiling.
 * The waiter is optional; the outcome is published as well (AudioFileRspEvt).
 *
 * @param path  Absolute, already resolved.
 * @param ms    How long to record.
 * @param rc    Where the outcome goes. May be NULL.
 * @param bytes How long the file is. May be NULL.
 * @param done  Signalled when it is over. May be NULL.
 * @param gen   The caller's tag for this request.
 * @param ack   Where @p gen is written back, just before @p done.
 */
void audio_record_file_request(const char *path, uint32_t ms, uint32_t *rc,
                               uint32_t *bytes, khal_sem_t done, uint32_t gen,
                               uint32_t *ack);

/**
 * @brief Record a whole sound and wake the caller (AUDIO_REC_REQ_SIG).
 *
 * From a task, never from an Active Object: the caller sleeps on @p done until
 * the recording ends. @p buf must outlive the request; it has one writer, and
 * nothing reads it until that writer is done.
 *
 * @param buf    Where the frames go.
 * @param frames The most to record.
 * @param got    How many arrived. May be NULL.
 * @param rc     Where the outcome goes, as a kode_status_t.
 * @param done   Signalled when it is over, either way.
 * @param gen    The caller's tag for this request.
 * @param ack    Where @p gen is written back, just before @p done.
 */
void audio_record_request(int16_t *buf, uint32_t frames, uint32_t *got,
                          uint32_t *rc, khal_sem_t done, uint32_t gen,
                          uint32_t *ack);

/**
 * @brief Open the stream, and wake the caller once it is playing
 *        (AUDIO_STREAM_OPEN_REQ_SIG).
 *
 * From a task, never from an AO. Frames then go straight into the driver's ring
 * with kdrv_es8311_stream_write(); the stream holds the codec until closed, and
 * every other sound is refused meanwhile.
 *
 * @param rc   Where the outcome goes, as a kode_status_t.
 * @param done Signalled once the stream is playing, or refused.
 * @param gen  The caller's tag for this request.
 * @param ack  Where @p gen is written back, just before @p done.
 */
void audio_stream_open_request(uint32_t *rc, khal_sem_t done, uint32_t gen,
                               uint32_t *ack);

/**
 * @brief End the stream, and wake the caller once what it held has been heard
 *        (AUDIO_STREAM_CLOSE_REQ_SIG).
 *
 * The same contract as audio_stream_open_request().
 *
 * @param rc   Where the outcome goes, as a kode_status_t.
 * @param done Signalled once the stream has drained, or when there is none.
 * @param gen  The caller's tag for this request.
 * @param ack  Where @p gen is written back, just before @p done.
 */
void audio_stream_close_request(uint32_t *rc, khal_sem_t done, uint32_t gen,
                                uint32_t *ack);

/*
 * ---- The asks: kode_sys's requests, answered to the asking AO ----
 * Each posts a request carrying its answer (taken from the pool with it) and
 * returns; AO_Audio posts that answer to @p who exactly once, refusals included,
 * so @p who keeps a slot for it. One ask of each kind per asker at a time.
 * Returns KODE_OK when posted; KODE_DENIED for a NULL or unstarted @p who or one
 * still waiting; KODE_NO_RESOURCES when the pool or queue is short, nothing posted.
 */

/**
 * @brief Ask what the audio path is; answered with AUDIO_INFO_RSP_SIG (AudioInfoRspEvt).
 * @param who The asking AO.
 * @return As the asks above.
 */
kode_status_t audio_info_ask(QActive *who);

/**
 * @brief Play @p frames; answered with AUDIO_DONE_SIG once heard or refused.
 * @param who    The asking AO.
 * @param buf    Mono frames at the board's rate, read in place until the answer.
 * @param frames How many.
 * @return As the asks above.
 */
kode_status_t audio_play_ask(QActive *who, const int16_t *buf, uint32_t frames);

/**
 * @brief Record into @p buf; answered with AUDIO_REC_RSP_SIG, @c frames recorded.
 * @param who    The asking AO.
 * @param buf    Written in place, and never after the answer is posted.
 * @param frames The most to record.
 * @return As the asks above.
 */
kode_status_t audio_record_ask(QActive *who, int16_t *buf, uint32_t frames);

/**
 * @brief Record for @p ms into @p path; answered with AUDIO_FILE_RSP_SIG, posted rather than published.
 * @param who  The asking AO.
 * @param path Absolute, already resolved; copied.
 * @param ms   How long to record.
 * @return As the asks above.
 */
kode_status_t audio_record_file_ask(QActive *who, const char *path, uint32_t ms);

/**
 * @brief Open the stream; answered with AUDIO_STREAM_OPEN_RSP_SIG once it plays, or refused.
 * @param who The asking AO.
 * @return As the asks above.
 */
kode_status_t audio_stream_open_ask(QActive *who);

/**
 * @brief Close the stream @p who opened; answered with AUDIO_STREAM_CLOSE_RSP_SIG once it has drained.
 * @param who The asking AO.
 * @return As the asks above.
 */
kode_status_t audio_stream_close_ask(QActive *who);

/**
 * @brief Whether @p who is the AO whose ask opened the stream now playing.
 * @note Reads one atomic AO_Audio publishes; safe from any task.
 * @param who An AO.
 * @return true only while @p who's open stands.
 */
bool audio_stream_writer_is(QActive const *who);

/**
 * @brief End the samples recording early; its caller gets the frames so far
 *        (AUDIO_REC_STOP_REQ_SIG).
 *
 * One stop is queued at a time. Ignored when none runs, and for a file recording.
 */
void audio_rec_stop_request(void);

/**
 * @brief Open the recording stop's door again, once its request is taken.
 *
 * FOR AO_AUDIO, and for a stand-in that handles AUDIO_REC_STOP_REQ_SIG.
 */
void audio_rec_stop_take(void);

/* ---- AO_Imu: owner of the QMI8658A and the QMC5883P ------------------------
 * The IMU, its FIFO included, and the magnetometer beside it. Draining the FIFO
 * empties it, so nothing else touches either part: the haptic row asks this AO
 * for its witness capture. Nothing blocks: parts rest until asked and go back to
 * standby after KODE_SYS_IMU_REST_AFTER_MS. One capture at a time, no queue; a
 * capture nobody drains is disarmed after KODE_IMU_CAPTURE_MAX_MS. A part not
 * fitted is said once and its reads refused.
 */
extern QActive * const AO_Imu;      /**< The IMU's owner AO. */
void Imu_ctor(void);                /**< Construct it; kaos_imu_start() calls it. */
/**
 * @brief Construct AO_Imu and start it at KODE_PRIO_IMU, on its own queue and
 *        stack, then register its settings.
 */
void kaos_imu_start(void);
/** @brief Register its self-test rows; kaos_imu_start() calls it. */
void kaos_imu_register(void);

/* ---- AO_Haptic: owner of the AW86233 ---------------------------------------
 * The LRA driver on the Antenna PCB; this AO is the only code that speaks to it.
 * A buzz is fed from an RTP FIFO this AO refills on its own tick; the FIFO drains
 * in about 5 ms and an underrun stutters, hence its priority above the IMU's.
 * One buzz at a time, no queue: a second is refused. Without the Antenna PCB it
 * still starts, and refuses every request.
 */
/**
 * @brief Whether AO_Display has finished its first bring-up, well or badly.
 *
 * The panel's reset line is also the touch controller's, and that reset can
 * disturb the shared I2C bus: kode_init() waits for this before the rows that
 * probe the bus.
 */
bool kaos_display_settled(void);

extern QActive * const AO_Haptic;   /**< The haptic's owner AO. */
void Haptic_ctor(void);             /**< Construct it; kaos_haptic_start() calls it. */
/**
 * @brief Construct AO_Haptic and start it at KODE_PRIO_HAPTIC, on its own queue and
 *        stack, then register its settings.
 */
void kaos_haptic_start(void);
/** @brief Register its console verbs and its self-test rows; kaos_haptic_start() calls it. */
void kaos_haptic_register(void);

/**
 * @brief Ask AO_Imu for one sample (IMU_READ_REQ_SIG).
 *
 * Answered within the step while the part is awake, about 200 ms later if it
 * rests. Writes @p out, @p rc, @p ack (a release), then signals @p done. @p out
 * is facade storage, never a stack frame: the wait is bounded. A @p gen that no
 * longer matches @p cur has nobody waiting, and nothing is written. Refused
 * (KODE_IO_ERROR) when not fitted, KODE_DENIED while a row or capture holds it.
 *
 * @param out  A kdrv_qmi8658_sample_t*. Written only on KODE_OK.
 * @param rc   The read's kode_status_t. May be NULL.
 * @param done Signalled once @p out and @p rc are written.
 * @param gen  The requester's tag for this request.
 * @param cur  The requester's current tag; NULL means always answered.
 * @param ack  Where @p gen is written back, just before @p done. May be NULL.
 */
void imu_read_request(void *out, uint32_t *rc, khal_sem_t done, uint32_t gen,
                      uint32_t const *cur, uint32_t *ack);

/**
 * @brief Arm the IMU's FIFO for a witness capture (IMU_CAPTURE_REQ_SIG).
 *
 * Posts and returns - safe from an AO (the caller is AO_Haptic). The outcome
 * comes back to @p who as IMU_ARMED_SIG, KODE_DENIED when another holds it.
 *
 * @param who Where the ImuArmedEvt goes. Never NULL.
 */
void imu_capture_request(QActive *who);

/**
 * @brief Drain the armed window and measure it at @p hz (IMU_DRAIN_REQ_SIG).
 *
 * Posts and returns. Answered to @p who as IMU_ENERGY_SIG: the energy at @p hz,
 * the broadband RMS and the sample count, which the caller judges.
 *
 * @param who Where the ImuEnergyEvt goes. Never NULL.
 * @param hz  The bin to measure.
 */
void imu_drain_request(QActive *who, uint16_t hz);

/**
 * @brief Ask AO_Imu to self-test the IMU - `test component=imu`.
 *
 * Verdict: a published TEST_RESULT_SIG "imu". Checks the part's identity, each
 * sensor's built-in self-test, and at rest one g, no rotation and a plausible
 * die temperature. Every reading is logged before the verdict.
 */
void imu_test_request(void);

/**
 * @brief Ask AO_Imu for one magnetometer sample (MAG_READ_REQ_SIG).
 *
 * imu_read_request()'s contract. The part converts at 50 Hz while awake; a read
 * before its first conversion waits one period inside the step. KODE_IO_ERROR
 * when not fitted, KODE_ERROR when no conversion was ready or one overflowed.
 * Raw milligauss on the chip's own axes, not remapped.
 *
 * @param out  An int32_t[3]. Written only on KODE_OK.
 * @param rc   The read's kode_status_t. May be NULL.
 * @param done Signalled once @p out and @p rc are written.
 * @param gen  The requester's tag for this request.
 * @param cur  The requester's current tag; NULL means always answered.
 * @param ack  Where @p gen is written back, just before @p done. May be NULL.
 * @return true when queued, or answered at once for a dry pool; false when
 *         AO_Imu's queue has no room, nothing written: ask again later.
 */
bool mag_read_request(void *out, uint32_t *rc, khal_sem_t done, uint32_t gen,
                      uint32_t const *cur, uint32_t *ack);

/** @brief What a get of the magnetometer's calibration brings back. */
typedef struct {
    kdrv_qmc5883p_cal_t corr;     /**< The calibration; meaningful when @c present. */
    bool                present;  /**< One is stored.                               */
} MagCalSlot;

/**
 * @brief Ask AO_Imu for the magnetometer's calibration (MAG_CAL_REQ_SIG).
 *
 * mag_read_request()'s contract. The calibration is the `mag_cal` setting,
 * stored by AO_Imu alone; a get with none stored answers KODE_OK, @c present false.
 *
 * @param op   MAG_CAL_OP_GET, MAG_CAL_OP_SET or MAG_CAL_OP_CLEAR.
 * @param corr  SET: the calibration. Otherwise NULL.
 * @param out  GET: a MagCalSlot. Written only on KODE_OK. Otherwise NULL.
 * @param rc   KODE_OK; KODE_DENIED for a set past the limits; KODE_ERROR when
 *             the store refused.
 * @param done Signalled once @p out and @p rc are written.
 * @param gen  The requester's tag for this request.
 * @param cur  The requester's current tag; NULL means always answered.
 * @param ack  Where @p gen is written back, just before @p done. May be NULL.
 * @return As mag_read_request().
 */
bool mag_cal_request(uint8_t op, const kdrv_qmc5883p_cal_t *corr, MagCalSlot *out,
                     uint32_t *rc, khal_sem_t done, uint32_t gen, uint32_t const *cur,
                     uint32_t *ack);

/*
 * ---- The asks: kode_sys's requests, answered to the asking AO ----
 * The audio asks' contract (see audio_info_ask()'s block), with AO_Imu as the
 * owner: one of each kind per asker waits at a time, the answer is posted to
 * @p who exactly once, and the returns are the same three.
 */

/**
 * @brief Ask for one IMU sample; answered with IMU_SAMPLE_RSP_SIG (ImuSampleRspEvt).
 * @param who The asking AO.
 * @return As the audio asks.
 */
kode_status_t imu_read_ask(QActive *who);

/**
 * @brief Ask for one magnetometer sample; answered with MAG_SAMPLE_RSP_SIG (MagSampleRspEvt).
 * @param who The asking AO.
 * @return As the audio asks.
 */
kode_status_t mag_read_ask(QActive *who);

/**
 * @brief Read, store or clear the calibration; answered with MAG_CAL_RSP_SIG (MagCalRspEvt).
 * @param who  The asking AO.
 * @param op   MAG_CAL_OP_GET, MAG_CAL_OP_SET or MAG_CAL_OP_CLEAR.
 * @param corr SET: the calibration, copied. Otherwise NULL.
 * @return As the audio asks.
 */
kode_status_t mag_cal_ask(QActive *who, uint8_t op, const kdrv_qmc5883p_cal_t *corr);

/**
 * @brief Ask AO_Imu to test the magnetometer - `test component=mag`.
 *
 * Verdict: a published TEST_RESULT_SIG "mag", each step a MAG_TEST_SIG record
 * first. The part names itself, then the field at rest is published unjudged;
 * the self-test shift is logged, not judged (no datasheet threshold). Fails
 * while a witness window or the IMU's row holds the part.
 */
void mag_test_request(void);

/**
 * @brief Ask AO_Haptic for one buzz (HAPTIC_PLAY_REQ_SIG).
 *
 * Posts and returns - safe from an AO. The outcome is the published
 * HAPTIC_DONE_SIG: status, samples taken, and whether the FIFO refused a burst.
 * A board with no driver takes the request and refuses it inside AO_Haptic.
 *
 * @param ms       How long to hold it.
 * @param gain_pct Drive strength as a percentage of full scale.
 * @return KODE_DENIED while a buzz or the production row is in flight,
 *         KODE_OK when the request was posted.
 */
kode_status_t haptic_play_request(uint16_t ms, uint8_t gain_pct);

/**
 * @brief Post a pattern of on/off segments, on first (HAPTIC_PLAY_REQ_SIG).
 * @param seg_ms   Segment lengths in ms, copied.
 * @param n        How many, 1 to KODE_HAPTIC_SEGMENTS_MAX.
 * @param gain_pct Drive strength, per cent of full scale.
 * @return KODE_DENIED for NULL, a bad @p n, a zero segment or while busy;
 *         KODE_NO_RESOURCES for a dry pool; KODE_OK when posted.
 */
kode_status_t haptic_pattern_request(const uint16_t *seg_ms, uint8_t n,
                                     uint8_t gain_pct);

/**
 * @brief Post a buzz that lasts until haptic_stop_request().
 * @param gain_pct Drive strength, per cent of full scale.
 * @return KODE_DENIED while busy; KODE_NO_RESOURCES for a dry pool;
 *         KODE_OK when posted.
 */
kode_status_t haptic_hold_request(uint8_t gain_pct);

/** @brief End a pattern or a hold early (HAPTIC_STOP_REQ_SIG). At most one stop waits. */
void haptic_stop_request(void);

/** @brief What the motor is doing, as the request door sees it. */
typedef enum {
    HAPTIC_STATE_IDLE = 0,  /**< Free: a request is taken.                 */
    HAPTIC_STATE_PLAYING,   /**< A buzz or pattern, the row, or a hold told to stop. */
    HAPTIC_STATE_HOLDING    /**< A hold, until a stop.                     */
} haptic_state_t;

/**
 * @brief The motor's state: claimed by a request's door, freed by the AO.
 * @return HAPTIC_STATE_HOLDING from a hold's door until a stop is requested.
 */
haptic_state_t haptic_state(void);

/**
 * @brief Read, write or dump one haptic field (HAPTIC_FIELD_REQ_SIG).
 *
 * imu_field_request()'s contract, for the AW86233's own field table, and
 * answered with a published HAPTIC_FIELD_SIG.
 *
 * @param op    FIELD_OP_GET, FIELD_OP_SET or FIELD_OP_DUMP.
 * @param field kdrv_aw86233_field_t, already resolved from its name.
 * @param value For FIELD_OP_SET, in the field's own unit.
 */
void haptic_field_request(uint8_t op, uint16_t field, int32_t value);

/**
 * @brief Ask AO_Haptic to self-test the motor - `test component=haptic`.
 *
 * Verdict: a published TEST_RESULT_SIG "haptic", with the IMU as witness. A
 * still window first (refused if it already carries the motor's frequency), then
 * each gain must clear it by KODE_HAPTIC_TEST_RATIO_X10. Readings are logged.
 */
void haptic_test_request(void);

/* ---- AO_Nfc: owner of the NFC reader -------------------------------------
 * The ST25R3916B on the Antenna PCB, its interrupt line and its SPI bus. The
 * 13.56 MHz field is on only while a request is in flight. Without the Antenna
 * PCB it still starts and refuses every request. One request at a time: the
 * answer goes through the requester's memory (generation stamp included), so a
 * second requester is refused with KODE_DENIED.
 */
extern QActive * const AO_Nfc;      /**< The NFC reader's owner AO. */
void Nfc_ctor(void);                /**< Construct it; kaos_nfc_start() calls it. */
/**
 * @brief Construct AO_Nfc and start it at KODE_PRIO_NFC, on its own queue and
 *        stack.
 */
void kaos_nfc_start(void);
/** @brief Register its console verbs and its self-test rows; kaos_nfc_start() calls it. */
void kaos_nfc_register(void);

/**
 * @brief Ask AO_Nfc for the reader's status (NFC_INFO_REQ_SIG).
 *
 * Answered to @p who with an NfcInfoEvt, in every state - with no reader
 * fitted it says so rather than refusing.
 *
 * @param who Where the answer goes.
 */
void nfc_info_request(QActive *who);

/**
 * @brief Ask AO_Nfc to read or write one NDEF message (NFC_NDEF_REQ_SIG).
 *
 * For an Active Object: the answer is an NfcNdefRspEvt posted to @p who; a task
 * calls the facade instead. @p text must outlive the request: written, it is
 * read; read, it is filled and the bytes come back in the answer.
 *
 * @param who   Where the answer goes. NULL is a request nobody hears.
 * @param text  The message to write, or the buffer to read into.
 * @param cap   How many bytes @p text holds, terminator included.
 * @param write true to write @p text to the tag, false to read into it.
 * @param uri   Writing: a URI record rather than a Text record.
 * @param ms    How long to keep the field up looking for a tag.
 */
void nfc_ndef_request(QActive *who, char *text, size_t cap, bool write,
                      bool uri, uint32_t ms);

/**
 * @brief Run `test component=nfc` (NFC_TEST_REQ_SIG).
 *
 * Answered with a published TEST_RESULT_SIG "nfc" within KODE_NFC_TEST_BUDGET_MS.
 * Writes this board's serial to the tag and reads it back, so a stray card on
 * the bench cannot pass it.
 */
void nfc_test_request(void);

/**
 * @brief Run the reader's row in one of its three modes.
 *
 * READER_TEST_ROW is nfc_test_request(). READER_TEST_FIELD passes when the part
 * names itself and its field stands one window with nothing in front of it.
 * READER_TEST_CARD passes when any tag answers, TEST_SKIP_SIG when none does.
 * The last two neither read nor write a tag. All answer under the name "nfc".
 *
 * @param mode READER_TEST_*.
 */
void nfc_test_request_mode(uint8_t mode);

#if CONFIG_KODEOS_NFC_CERT
/**
 * @brief Hold or drop the maximum-drive certification field (NFC_CERT_REQ_SIG).
 *
 * The FCC 15.225 bench path. Posts and returns; the field is held with no
 * timeout until a mode-0 request drops it. @c rc carries the outcome, and @c gen
 * is written into @c ack just before @c done; all three may be NULL. A drop while
 * a listen window holds the reader is refused with KODE_DENIED.
 *
 * @param mode 0 drops the field, 1 raises a bare carrier, 2 a modulated one.
 * @param rc   Out: the outcome as kode_status_t. May be NULL.
 * @param done Signalled once @p rc is written. May be NULL.
 * @param gen  The caller's generation, stamped into @p ack before @p done.
 * @param ack  Where @p gen is written back. May be NULL.
 */
void nfc_cert_request(uint8_t mode, uint32_t *rc, khal_sem_t done,
                      uint32_t gen, uint32_t *ack);

/**
 * @brief Hold or drop the certification field; answered with NFC_CERT_RSP_SIG.
 * @param who  The asking AO.
 * @param mode 0 drops the field, 1 raises a bare carrier, 2 a modulated one.
 * @return As nfc_read_ask().
 */
kode_status_t nfc_cert_ask(QActive *who, uint8_t mode);
#endif

/**
 * @brief Ask AO_Nfc to sweep the public Mifare key dictionary on a block.
 *
 * A deliberate, minutes-long operation, paced one batch per tick so it does
 * not block the AO. Answered to @p who with an NfcKeysRspEvt when a key is
 * found or the dictionary is exhausted, with progress in the log.
 *
 * @param who   Where the answer goes.
 * @param block The block whose sector to open.
 */
void nfc_keys_request(QActive *who, uint8_t block);

/**
 * @brief Ask AO_Nfc to dump a Mifare Classic (NFC_DUMP_REQ_SIG).
 *
 * Reads every sector the fast dictionary opens and streams it to the log,
 * paced one sector per tick. Answered to @p who with an NfcDumpRspEvt.
 *
 * @param who Where the final answer goes.
 */
void nfc_dump_request(QActive *who);

/**
 * @brief Ask AO_Nfc for the next tag's identity, for a caller that can block.
 *
 * The answer goes through @p out and @p rc and is signalled on @p done.
 * Everything named here must outlive the request: a caller that gives up
 * leaves AO_Nfc holding these pointers, so not a stack frame.
 *
 * @param out  knfc_tag_t *, written only when a tag turned up.
 * @param rc   Out: KODE_OK for a tag, KODE_TIMEOUT for none, KODE_DENIED when
 *             refused (no reader, busy, @p ms 0); untouched if dropped. May be NULL.
 * @param done Signalled when the request is over, either way.
 * @param ms   How long to keep the field up.
 * @param gen  The caller's tag for this request.
 * @param ack  Where @p gen is written back, just before @p done. May be NULL.
 */
void nfc_read_request(void *out, uint32_t *rc, khal_sem_t done, uint32_t ms,
                      uint32_t gen, uint32_t *ack);

/**
 * @brief Read or write an NDEF message, for a caller that can block.
 *
 * nfc_read_request()'s rules: everything named here must outlive the request.
 *
 * @param text   The message to write, or the buffer to read into.
 * @param cap    How many bytes @p text holds.
 * @param n      Out: how many were read. May be NULL.
 * @param is_uri Out: reading, true for a URI record, false for Text; written
 *               only on KODE_OK. May be NULL.
 * @param write  true to write @p text to the tag.
 * @param uri    Writing: a URI record rather than a Text record.
 * @param rc     Out: kode_status_t of the exchange.
 * @param done   Signalled when the request is over, either way.
 * @param ms     How long to keep the field up.
 * @param gen    The caller's tag for this request.
 * @param ack    Where @p gen is written back, just before @p done.
 */
void nfc_ndef_task_request(char *text, size_t cap, size_t *n, bool *is_uri,
                           bool write, bool uri, uint32_t *rc, khal_sem_t done,
                           uint32_t ms, uint32_t gen, uint32_t *ack);

/** @brief The longest listen window AO_Nfc runs, in ms; a longer request is
 *         capped at it. */
#define KODE_NFC_FIELD_MS_MAX  200U

/**
 * @brief Listen on the antenna with the reader's own field off, and describe
 *        what was there (NFC_FIELD_REQ_SIG).
 *
 * A burst of measurements every 10 ms for @p ms (at least one), nothing radiated.
 * A task passes @p done and waits; an AO passes @p to for an NfcFieldRspEvt.
 * Status: KODE_IO_ERROR no reader; KODE_DENIED reader busy; KODE_ERROR zero @p ms
 * or a failed window. Everything named must outlive the request (not a stack frame).
 *
 * @param out  knfc_field_t *, written only on KODE_OK. May be NULL.
 * @param rc   Out: the outcome, written before @p done; KODE_NO_RESOURCES when the
 *             event pool was empty (answered at once, nothing posted). May be NULL.
 * @param done Signalled when the request is over, either way. May be NULL.
 * @param ms   How long to listen, 1..KODE_NFC_FIELD_MS_MAX; longer is capped.
 * @param gen  The caller's tag for this request.
 * @param ack  Where @p gen is written back, just before @p done. May be NULL.
 * @param to   Where an NfcFieldRspEvt goes, or NULL for a blocked task.
 */
void nfc_field_request(void *out, uint32_t *rc, khal_sem_t done, uint32_t ms,
                       uint32_t gen, uint32_t *ack, QActive *to);

/**
 * @brief Present the Dot as a passive NFC-A card (NFC_EMU_REQ_SIG).
 *
 * The field stays off; a driver worker serves Type-2 READs and AO_Nfc publishes
 * NFC_EMU_SIG events. Held until nfc_emulate_stop_request(); every other request
 * is refused meanwhile. Start outcome on @p done or @p to: KODE_OK up; KODE_ERROR
 * no reader or failed; KODE_DENIED busy; KODE_NO_RESOURCES too big or pool empty.
 *
 * @param card What to present; copied at once, so it need only outlive this call.
 * @param rc   Out: the start's outcome. May be NULL.
 * @param done Signalled once @p rc is written. May be NULL.
 * @param gen  The caller's tag for this request.
 * @param ack  Where @p gen is written back, just before @p done. May be NULL.
 * @param to   Where an NfcEmuRspEvt goes, or NULL for a blocked task.
 */
void nfc_emulate_request(const knfc_emu_card_t *card, uint32_t *rc,
                         khal_sem_t done, uint32_t gen, uint32_t *ack,
                         QActive *to);

/**
 * @brief Stop emulating (NFC_EMU_REQ_SIG with a stop).
 *
 * Accepted at once with the READs so far; when the worker releases the reader,
 * AO_Nfc publishes a stopped NFC_EMU_SIG. Refused KODE_DENIED when not emulating.
 *
 * @param rc    Out: KODE_OK when a window was stopping, else KODE_DENIED. May
 *              be NULL.
 * @param reads Out: READs served this session. May be NULL.
 * @param done  Signalled once @p rc is written. May be NULL.
 * @param gen   The caller's tag for this request.
 * @param ack   Where @p gen is written back, just before @p done. May be NULL.
 * @param to    Where an NfcEmuRspEvt goes, or NULL for a blocked task.
 */
void nfc_emulate_stop_request(uint32_t *rc, uint32_t *reads, khal_sem_t done,
                              uint32_t gen, uint32_t *ack, QActive *to);

/**
 * @brief Whether an emulation window is up (never blocks).
 * @return true while the Dot is presenting a card.
 */
bool nfc_is_emulating(void);

/*
 * ---- The asks: kode_sys's requests, answered to the asking AO ----
 * The audio asks' contract, with AO_Nfc as the owner: the answer is posted to
 * @p who exactly once, one ask of each kind per asker, and the same three
 * returns. A buffer an ask lends is the asker's again once answered.
 */

/**
 * @brief Wait up to @p ms for a tag; answered with NFC_READ_RSP_SIG.
 * @param who The asking AO.
 * @param ms  How long to keep the field up.
 * @return As the asks above.
 */
kode_status_t nfc_read_ask(QActive *who, uint32_t ms);

/**
 * @brief Read the NDEF message of the first tag, or write @p text to it;
 *        answered with NFC_NDEF_RSP_SIG.
 * @param who  The asking AO.
 * @param text What to write, copied; NULL to read. At most KQP_NFC_TEXT_MAX - 1
 *             characters: a longer one is refused, never cut.
 * @param uri  Writing: a URI record rather than a Text one.
 * @param ms   How long to keep the field up.
 * @return As the asks above; KODE_DENIED also for a @p text too long, nothing posted.
 */
kode_status_t nfc_ndef_ask(QActive *who, const char *text, bool uri, uint32_t ms);

/**
 * @brief Present the Dot as a card until nfc_emulate_stop_ask(); answered with
 *        NFC_EMU_RSP_SIG.
 * @param who  The asking AO.
 * @param card The card, copied.
 * @return As the asks above.
 */
kode_status_t nfc_emulate_ask(QActive *who, const NfcCard *card);

/**
 * @brief Stop presenting the card; answered with NFC_EMU_RSP_SIG, the READs served.
 * @param who The asking AO.
 * @return As the asks above.
 */
kode_status_t nfc_emulate_stop_ask(QActive *who);

/**
 * @brief Listen with the reader's field off for @p ms; answered with NFC_FIELD_RSP_SIG.
 * @param who The asking AO.
 * @param ms  The window, 1..KODE_NFC_FIELD_MS_MAX.
 * @return As the asks above.
 */
kode_status_t nfc_field_ask(QActive *who, uint32_t ms);

/* ---- AO_Rfid: owner of the 125 kHz LF front end --------------------------
 * The discrete carrier/demod chain on the Antenna PCB, its LEDC timer and ADC2
 * continuous handle; "off" means the carrier is not driven. The field is up only
 * while asked. Blocking analog work runs on the AO's own worker (kqp_worker,
 * stack in PSRAM). One job at a time: every request is refused with KODE_DENIED
 * until the worker reports its job, even after a deadline answered it.
 * @c gen is written into @c ack immediately before @c done is signalled.
 */
extern QActive * const AO_Rfid;     /**< The LF front end's owner AO. */
void Rfid_ctor(void);               /**< Construct it; kaos_rfid_start() calls it. */
/**
 * @brief Construct AO_Rfid and start it at KODE_PRIO_RFID, on its own queue and
 *        stack.
 */
void kaos_rfid_start(void);
/** @brief Register its console verbs and its self-test rows; kaos_rfid_start() calls it. */
void kaos_rfid_register(void);

/**
 * @brief Ask AO_Rfid for the front end's status (RFID_INFO_REQ_SIG).
 *
 * Answered to @p who with a RfidInfoEvt, in every state - the field is off at
 * rest, so the last decode's age is how an operator tells "no tag" from "no
 * antenna".
 *
 * @param who Where the answer goes.
 */
void rfid_info_request(QActive *who);

/**
 * @brief Run `test component=rfid` (RFID_TEST_REQ_SIG).
 *
 * Answered with a published TEST_RESULT_SIG "rfid" within KODE_RFID_TEST_BUDGET_MS.
 * Passes on a stable EM4100 ID decoded KODE_RFID_TEST_TRIES windows in a row;
 * nothing goes on RFID_TAG_SIG meanwhile. A busy front end fails it at once.
 */
void rfid_test_request(void);

/**
 * @brief Run the 125 kHz row in one of its three modes.
 *
 * READER_TEST_ROW is rfid_test_request(). READER_TEST_FIELD passes when the
 * front end energises and the worker brings one read window back, whatever
 * it held. READER_TEST_CARD passes on a stable EM4100 ID and answers
 * TEST_SKIP_SIG when none is read. All answer under the name "rfid".
 *
 * @param mode READER_TEST_*.
 */
void rfid_test_request_mode(uint8_t mode);

#if CONFIG_KODEOS_NFC_CERT
/**
 * @brief Hold or drop the maximum-drive 125 kHz certification field
 *        (RFID_CERT_REQ_SIG).
 *
 * The FCC §15.209 bench path; held with no timeout until a mode-0 request. The
 * antenna cannot be sensed, so success means commanded, not radiated. @c gen is
 * written into @c ack just before @c done; all three may be NULL. Refused with
 * KODE_DENIED while a read, clone, capture, the row or a worker job holds it.
 *
 * @param mode 0 drops the field, 1 raises a bare carrier, 2 an OOK-modulated one.
 * @param rc   Out: the outcome as kode_status_t. May be NULL.
 * @param done Signalled once @p rc is written. May be NULL.
 * @param gen  The caller's generation, stamped into @p ack before @p done.
 * @param ack  Where @p gen is written back. May be NULL.
 */
void rfid_cert_request(uint8_t mode, uint32_t *rc, khal_sem_t done,
                       uint32_t gen, uint32_t *ack);

/**
 * @brief Hold or drop the 125 kHz certification carrier; answered with RFID_CERT_RSP_SIG.
 * @param who  The asking AO.
 * @param mode 0 drops the field, 1 raises a bare carrier, 2 a modulated one.
 * @return As rfid_read_ask().
 */
kode_status_t rfid_cert_ask(QActive *who, uint8_t mode);
#endif

/**
 * @brief Read the next EM4100 ID, for a caller that can block.
 *
 * Answered through @p id / @p rf_div / @p have, then @p done; never boosts VO4.
 * Everything named must outlive the request (not a stack frame). A zero @p ms
 * or a busy front end is refused at once, @p have false.
 *
 * @param id     5-byte ID buffer, written only when a tag decoded.
 * @param rf_div Out: 16, 32 or 64, the bit rate decoded at.
 * @param have   Out: whether a tag decoded inside the window.
 * @param done   Signalled when the request is over, either way.
 * @param ms     How long to keep the field up.
 * @param gen    The caller's tag for this request.
 * @param ack    Where @p gen is written back, just before @p done. May be NULL.
 */
void rfid_read_request(uint8_t *id, uint8_t *rf_div, bool *have,
                       khal_sem_t done, uint32_t ms, uint32_t gen,
                       uint32_t *ack);

/**
 * @brief Clone a 5-byte ID onto a T5577, for a caller that can block.
 *
 * rfid_read_request()'s ownership rules and refusals, and the write's boost:
 * the clone raises VO4 to 3.3 V for the program and reads the tag back to
 * verify.
 *
 * @param id   The 5-byte ID to write. Must outlive the request.
 * @param ok   Out: whether the read-back matched.
 * @param done Signalled when the request is over, either way.
 * @param ms   How long to keep the field up.
 * @param gen  The caller's tag for this request.
 * @param ack  Where @p gen is written back, just before @p done. May be NULL.
 */
void rfid_write_request(const uint8_t *id, bool *ok, khal_sem_t done,
                        uint32_t ms, uint32_t gen, uint32_t *ack);

/**
 * @brief Read the next EM4100 ID, for an Active Object that must not block.
 *
 * AO_Rfid answers @p who with a RfidOpRspEvt on RFID_OP_RSP_SIG when the window
 * closes; the result is in that event.
 *
 * @param who The AO to answer with RFID_OP_RSP_SIG.
 * @param ms  How long to keep the field up.
 */
void rfid_read_console_request(QActive *who, uint32_t ms);

/**
 * @brief Clone a 5-byte ID onto a T5577, for an Active Object that must not block.
 *
 * The console's half of rfid_write_request(): answered to @p who with a
 * RfidOpRspEvt on RFID_OP_RSP_SIG. The clone boosts VO4 and verifies by
 * read-back.
 *
 * @param who The AO to answer with RFID_OP_RSP_SIG.
 * @param id  The 5-byte ID to write. Must outlive the request (the console's
 *            one-command-at-a-time buffer does).
 * @param ms  How long to keep the field up.
 */
void rfid_clone_console_request(QActive *who, const uint8_t *id, uint32_t ms);

/**
 * @brief Read the 125 kHz demod line with the carrier OFF, describe it, and
 *        optionally copy the waveform out (RFID_FIELD_REQ_SIG).
 *
 * The worker writes @p out, @p wave, @p n before reporting. A deadline may answer
 * first (KODE_ERROR) and the worker still write once, so lend memory kept for these
 * requests alone. Missing front end is not sensed: it answers KODE_OK. Channels as
 * nfc_field_request(). Busy: KODE_DENIED; zero @p ms or failed read: KODE_ERROR.
 *
 * @param out  krfid_field_t *, written only when the window was read. May be NULL.
 * @param wave Where the waveform goes, @p cap points, or NULL for statistics only.
 * @param cap  How many points @p wave holds.
 * @param n    Out: points written to @p wave, 0 with no wave. May be NULL.
 * @param rc   Out: the outcome, written before @p done; KODE_NO_RESOURCES when the
 *             event pool was empty (answered at once, nothing posted). May be NULL.
 * @param done Signalled when the request is over, either way. May be NULL.
 * @param ms   How long to read, 1..150 (KDRV_RFID_LF_CAPTURE_MS_MAX); longer
 *             is capped.
 * @param gen  The caller's tag for this request.
 * @param ack  Where @p gen is written back, just before @p done. May be NULL.
 * @param to   Where a RfidFieldRspEvt goes, or NULL for a blocked task.
 */
void rfid_capture_request(void *out, uint16_t *wave, size_t cap, size_t *n,
                          uint32_t *rc, khal_sem_t done, uint32_t ms,
                          uint32_t gen, uint32_t *ack, QActive *to);

/**
 * @brief Play an EM4100 ID as a tag until rfid_emulate_stop_request().
 *
 * @param id   The 5-byte ID, copied; NULL answers KODE_ERROR.
 * @param rc   Out: KODE_OK with the loop running, the driver's refusal,
 *             KODE_DENIED while the front end is held, KODE_TIMEOUT.
 * @param done Signalled once @p rc is written. May be NULL.
 * @param gen  The caller's generation, stamped into @p ack before @p done.
 * @param ack  Where @p gen is written back. May be NULL.
 */
void rfid_emulate_request(const uint8_t *id, uint32_t *rc, khal_sem_t done,
                          uint32_t gen, uint32_t *ack);

/**
 * @brief End the tag emulation.
 *
 * @param rc   Out: KODE_OK, or KODE_DENIED when none plays.
 * @param done Signalled once @p rc is written. May be NULL.
 * @param gen  The caller's generation, stamped into @p ack before @p done.
 * @param ack  Where @p gen is written back. May be NULL.
 */
void rfid_emulate_stop_request(uint32_t *rc, khal_sem_t done, uint32_t gen,
                               uint32_t *ack);

/**
 * @brief Whether a tag is being emulated; safe from any task.
 *
 * @return true from the start's KODE_OK until the stop ends.
 */
bool rfid_is_emulating(void);

/*
 * ---- The asks: kode_sys's requests, answered to the asking AO ----
 * The audio asks' contract, with AO_Rfid as the owner: the answer is posted to
 * @p who exactly once, one ask of each kind per asker, and the same three
 * returns. A buffer an ask lends is the asker's again once answered.
 */

/**
 * @brief Energise for up to @p ms and read the first EM4100 ID; answered with
 *        RFID_OP_RSP_SIG.
 * @param who The asking AO.
 * @param ms  How long to keep the field up.
 * @return As the asks above.
 */
kode_status_t rfid_read_ask(QActive *who, uint32_t ms);

/**
 * @brief Clone @p id onto a T5577 and read it back; answered with RFID_OP_RSP_SIG.
 * @param who The asking AO.
 * @param id  The 5-byte ID, copied.
 * @param ms  How long to keep the field up.
 * @return As the asks above.
 */
kode_status_t rfid_write_ask(QActive *who, const uint8_t *id, uint32_t ms);

/**
 * @brief Play @p id as a tag until rfid_emulate_stop_ask(); answered with RFID_EMU_RSP_SIG.
 * @param who The asking AO.
 * @param id  The 5-byte ID, copied.
 * @return As the asks above.
 */
kode_status_t rfid_emulate_ask(QActive *who, const uint8_t *id);

/**
 * @brief End the emulation; answered with RFID_EMU_RSP_SIG.
 * @param who The asking AO.
 * @return As the asks above.
 */
kode_status_t rfid_emulate_stop_ask(QActive *who);

/**
 * @brief Read the demod line for @p ms, carrier off; answered with RFID_FIELD_RSP_SIG.
 *
 * The worker writes @p wave while the window runs, so AO_Rfid holds the answer
 * until the worker has let go of it: a deadline that fires first is answered
 * KODE_TIMEOUT once the worker returns.
 * @param who  The asking AO.
 * @param wave A buffer of @p cap points, written in place before the answer and
 *             never after; NULL for statistics only.
 * @param cap  How many points it holds.
 * @param ms   The window, 1..KDRV_RFID_LF_CAPTURE_MS_MAX.
 * @return As the asks above.
 */
kode_status_t rfid_field_ask(QActive *who, uint16_t *wave, size_t cap, uint32_t ms);

/**
 * @brief Ask AO_Ir to transmit one NEC frame (IR_SEND_REQ_SIG).
 *
 * Returns at once; safe from an AO. AO_Ir's worker sends it over the link to the
 * C5's RMT, so AO_Ir never waits the 52 to 86 ms on the wire. A dry pool, or more
 * than KODE_IR_DEFER_DEPTH requests held for the C5, drops it with a log line.
 *
 * @param addr The address.
 * @param cmd  The command.
 */
void ir_send_request(uint16_t addr, uint16_t cmd);

/**
 * @brief The event ir_send_request() posts, built for a caller that posts it itself.
 * @param addr The address.
 * @param cmd  The command.
 * @return The event, or NULL when the pool is dry.
 */
QEvt const *ir_send_evt(uint16_t addr, uint16_t cmd);

/**
 * @brief Ask AO_Ir to transmit a frame of raw symbols, lent by pointer.
 *
 * The blocking facade's way in. AO_Ir copies @p sym when it starts the call, so
 * keep it unchanged until answered: status in @p rc, @p gen in @p ack, then
 * @p done. An empty pool writes KODE_NO_RESOURCES and answers at once.
 *
 * @param sym        IrSymbol[], mark first.
 * @param n          How many, 1..KDRV_IR_TX_MAX_SYMBOLS; more is answered
 *                   KODE_DENIED, unsent.
 * @param carrier_hz The carrier.
 * @param start_ms   How long from now AO_Ir may still start the call; later
 *                   it is answered KODE_TIMEOUT, unsent.
 * @param rc         Where the status goes.
 * @param done       Posted once @p rc is written.
 * @param gen        The requester's tag.
 * @param ack        Where @p gen is written back, or NULL.
 */
void ir_send_raw_request(const IrSymbol *sym, uint16_t n, uint32_t carrier_hz,
                         uint32_t start_ms, uint32_t *rc, khal_sem_t done,
                         uint32_t gen, uint32_t *ack);

/**
 * @brief Ask AO_Ir for the symbols of the next frame it hears, within @p ms.
 *
 * Fills the lent memory and signals @p done once: first frame or deadline. A dry
 * pool or a near-full queue answers at once, empty. The request may wait in the
 * queue past a bounded caller's wait, so lend static storage, not a stack frame.
 * @p gen goes into @p ack (release; read it with acquire) just before @p done.
 * A second request displaces the first. Own transmissions and frames heard during
 * `test component=ir` are never offered: a request then is answered empty.
 *
 * @param out  IrSymbol[], at least @p cap entries, outliving the request.
 * @param cap  How many symbols @p out holds; a longer capture is truncated.
 * @param n    Where to write how many were copied; may be NULL.
 * @param done Signalled when @p out and @p n are filled; may be NULL for a
 *             caller that does not wait.
 * @param ms   How long to wait, clamped to KODE_SYS_IR_TIMER_MS_MAX; zero answers
 *             at once. Zero with @p out, @p n, @p done NULL is a listener's arm:
 *             it displaces nothing, answers nobody, arms when the link is up.
 * @param gen  This request's tag, written back to @p ack before @p done.
 * @param ack  Where that tag goes; may be NULL for a caller whose semaphore is
 *             its own.
 */
void ir_capture_request(IrSymbol *out, size_t cap, size_t *n, khal_sem_t done,
                        uint32_t ms, uint32_t gen, uint32_t *ack);

/**
 * @brief Ask AO_Ir to run the infrared row - `test component=ir`.
 *
 * Verdict: a published TEST_RESULT_SIG "ir". The receiver is judged first without
 * emitting, then a NEC frame with a random command must echo back exactly. For up
 * to KODE_IR_TEST_BUDGET_MS the row owns the receiver: nothing on IR_RX_SIG, and
 * a pending capture is answered empty. A second request meanwhile is refused.
 */
void ir_test_request(void);

/**
 * @brief Ask AO_Ir for its own status - the console's `ir`.
 *
 * From AO_Ir's state plus one resting-level read of the receiver
 * (kdrv_ir_rx_idle(), which emits nothing). If that read cannot be had, the line
 * is reported low at once; it never waits for the link. Posted to @p who as
 * IR_INFO_RSP_SIG (IrInfoEvt), in every state.
 *
 * @param who Where the answer goes. A NULL @p who is served and not
 *            answered.
 */
void ir_info_request(QActive *who);

/**
 * @brief Ask AO_Ir for the last frame it heard, into memory that outlives the
 *        call.
 *
 * For a caller that is not an AO (ir_last_ask() is the AO's): the frame the
 * receiver last produced, published or not. Answered within the step, but the
 * request may sit in the queue past a bounded wait, so lend storage that outlives
 * the call. @p gen goes into @p ack (release) just before @p done.
 *
 * @param out  kdrv_ir_frame_t *, filled only when a frame has been heard;
 *             may be NULL. Outlives the request.
 * @param have Where to write whether anything has ever been heard; may be
 *             NULL. False leaves @p out untouched.
 * @param done Signalled once, when @p out and @p have are filled. May be
 *             NULL for a caller that does not wait.
 * @param gen  This request's tag, written back to @p ack before @p done.
 * @param ack  Where that tag goes; may be NULL for a caller whose semaphore is
 *             its own.
 */
void ir_last_request(void *out, bool *have, khal_sem_t done,
                     uint32_t gen, uint32_t *ack);

/*
 * ---- The asks: kode_sys's requests, answered to the asking AO ----
 * The audio asks' contract, with AO_Ir as the owner: the answer is posted to
 * @p who exactly once, one ask of each kind per asker, and the same three
 * returns. A buffer an ask lends is the asker's again once answered.
 */

/**
 * @brief Send the frame @p sym lends; answered with IR_SEND_RAW_RSP_SIG once it
 *        went or was refused.
 * @param who        The asking AO.
 * @param sym        IrSymbol[@p n], mark first, copied when the call starts.
 * @param n          How many, 1..KDRV_IR_TX_MAX_SYMBOLS; more is answered
 *                   KODE_DENIED, unsent.
 * @param carrier_hz The carrier to send on.
 * @param start_ms   How long AO_Ir may hold it before it must start; later it
 *                   is answered KODE_TIMEOUT, unsent.
 * @return As the asks above.
 */
kode_status_t ir_send_raw_ask(QActive *who, const IrSymbol *sym, uint16_t n,
                              uint32_t carrier_hz, uint32_t start_ms);

/**
 * @brief Listen for a frame; answered with IR_CAPTURE_RSP_SIG when one is
 *        heard or @p ms has gone by.
 * @param who The asking AO.
 * @param out IrSymbol[@p cap], filled in place before the answer and never after.
 * @param cap How many it holds.
 * @param ms  How long to listen.
 * @return As the asks above.
 */
kode_status_t ir_capture_ask(QActive *who, IrSymbol *out, size_t cap, uint32_t ms);

/**
 * @brief Ask for the last frame heard; answered with IR_LAST_RSP_SIG.
 * @param who The asking AO.
 * @return As the asks above.
 */
kode_status_t ir_last_ask(QActive *who);

/* ---- AO_Display: owner of the AMOLED panel ------------------------------
 * Owns power, brightness and the pixel path (DISPLAY_FLUSH_SIG). Publishes
 * DISPLAY_STATE_SIG (up, bright) once the panel's fate is known, and on request.
 * Two exceptions to events: flush completion is kdrv_display's semaphore, given
 * from the DMA ISR (cache disabled, so no callback) because LVGL waits inside its
 * render call (kdrv_display_flush_wait()); and kdrv_display's shutdown handler
 * resets the panel inside esp_restart(), in the restarting task.
 */
extern QActive * const AO_Display;  /**< The panel owner AO. */
void Display_ctor(void);            /**< Construct it; kaos_display_start() calls it. */
/**
 * @brief Construct AO_Display and start it at KODE_PRIO_DISPLAY, on its own queue and
 *        stack, then register its settings.
 */
void kaos_display_start(void);
/** @brief Register its console verbs and its self-test rows; kaos_display_start() calls it. */
void kaos_display_register(void);

/**
 * @brief Ask AO_Display to publish DISPLAY_STATE_SIG again.
 *
 * For an AO started after kode_init()'s announcement: subscribe, then ask. Call
 * it only once kode_init() has returned: before, AO_Display has no queue and the
 * post asserts.
 */
void display_state_request(void);

#if CONFIG_KODE_LOWPOWER
/**
 * @brief Ask AO_Display to light the glass for the minute's glyphs, then let it
 *        go dark.
 *
 * AO_LowPower's minute, while the board is ASLEEP. A static event: nothing to refuse.
 */
void display_minute_request(void);
#endif

/**
 * @brief Ask AO_Display for a panel brightness.
 *
 * @param pct     5..100 for a user setting; AO_Display enforces that floor only
 *                when @p persist, so the black is never stored. Otherwise
 *                @p pct passes through untouched, 0 included.
 * @param persist true: the user's setting, stored for the next boot. false:
 *                transient (a slider mid-drag), nothing written.
 */
void display_brightness_request(uint8_t pct, bool persist);

/* ---- Capture: a picture of the panel ------------------------------------
 * AO_Display owns the panel, so a capture is asked for. The answer is a loan:
 * the 402 KB buffer is collected separately and held until released, like
 * storage_buf(). Every path that begins a capture must end it, or its PSRAM
 * stays held and every later capture is refused.
 */

/**
 * @brief Ask AO_Display for a copy of the frame on the glass.
 *
 * Answered with DISPLAY_CAPTURE_RSP_SIG to @p who: size, and whether anything is
 * drawn. Refused without KODEOS_SCREENSHOT, with no framebuffer or PSRAM for the
 * copy, or while a capture is open.
 */
void display_capture_request(QActive *who);

/**
 * @brief The captured image: a complete BMP, header included.
 *
 * Valid only between a DISPLAY_CAPTURE_RSP_SIG that said KODE_OK and the
 * matching display_capture_release(). Read it, do not keep it.
 */
const uint8_t *display_capture_buf(void);

/** @brief Give the capture back, and its buffer with it. */
void display_capture_release(void);

/* ---- Shot: the glass as one JPEG -----------------------------------------
 * A capture is a 402 KB BMP; a shot is the same frame, top-down, compressed
 * by the P4's codec to a few tens of KB - small enough to upload. The loan
 * works as the capture's does: the buffer is collected with
 * display_shot_buf() and held until display_shot_release(), and every path
 * that begins a shot must end it.
 */

/** @brief The largest JPEG a shot holds, and the size of its buffer. A frame
 *         that encodes larger is refused with KODE_ERROR. */
#define KSHOT_JPEG_CAP  (64U * 1024U)

/**
 * @brief Ask AO_Display for the glass as one JPEG.
 *
 * Answered with DISPLAY_SHOT_RSP_SIG to @p who: the length and KODE_OK;
 * KODE_DENIED codec busy or a shot already lent; KODE_NO_RESOURCES no codec (no
 * KODEOS_MIRROR, or start-up failed), no framebuffer or no PSRAM; KODE_ERROR
 * larger than KSHOT_JPEG_CAP. Only KODE_OK lends. The buffer is kept after the
 * first shot; a shot touches no mirror credit or dirty range.
 *
 * @param who     The AO the answer goes to.
 * @param quality JPEG quality, 1..100.
 */
void display_shot_request(QActive *who, uint8_t quality);

/**
 * @brief The shot: a complete JPEG.
 *
 * Valid only between a DISPLAY_SHOT_RSP_SIG that said KODE_OK and the
 * matching display_shot_release(). Read it, do not keep it.
 *
 * @return The JPEG, or NULL when nothing is lent.
 */
const uint8_t *display_shot_buf(void);

/** @brief Give the shot back: the buffer is kept for the next one, but it is
 *         no longer lent, and display_shot_buf() answers NULL again. */
void display_shot_release(void);

/**
 * @brief Ask AO_Display to encode the caller's own RGB565 into the caller's
 *        own buffer. A blocking request: for a worker task, never an AO.
 *
 * AO_Display owns the JPEG engine. The JPEG is copied into @p job's @c dst in its
 * step. Answer in @p len and @p rc (KODE_OK; KODE_DENIED busy or malformed;
 * KODE_NO_RESOURCES no engine or larger than @c cap; KODE_ERROR engine refused),
 * then @p ack and @p done. A stale @p gen (not @p *cur) is skipped entirely.
 *
 * @param job  The pixels and the buffer; copied into the request.
 * @param len  Receives the JPEG's length.
 * @param rc   Receives the kode_status_t; KODE_NO_RESOURCES here for a dry pool.
 * @param done Signalled once the answer is written.
 * @param gen  The caller's tag for this request.
 * @param cur  The caller's current tag.
 * @param ack  Receives @p gen, just before @p done.
 */
void display_jpeg_request(display_jpeg_t const *job, uint32_t *len, uint32_t *rc, khal_sem_t done,
                          uint32_t gen, uint32_t const *cur, uint32_t *ack);

/* ---- Mirror: the panel, live -------------------------------------------
 * A live stream of small JPEG frames from the panel's framebuffer, paced by a
 * credit window the host operates with display_mirror_ack(). AO_Display owns the
 * codec and the PPA scaler because it owns the panel. Compiled under
 * CONFIG_KODEOS_MIRROR (which needs CONFIG_KODEOS_SCREENSHOT): off, these four
 * symbols, their two claims and ~530 KB of buffers do not exist.
 */

/**
 * @brief Start streaming the panel.
 *
 * The next frame is always the whole panel. Calling again while streaming is a
 * reattach: it restarts the credit window and forces a full frame.
 *
 * @param scale   1 for full size, 2 to also halve both axes. Any other value
 *                is corrected to 1.
 * @param quality 1..100 for the JPEG encoder. 0 or out of range is corrected
 *                to 70, the mirror spec's own default.
 */
void display_mirror_start(uint8_t scale, uint8_t quality);

/** @brief Stop streaming. A mirror nobody is watching should not hold the
 *         codec, so this disarms the mirror's own pacer outright. */
void display_mirror_stop(void);

/**
 * @brief Tell AO_Display the host has received up to @p seq.
 *
 * Cumulative, like TCP's. A stale or invented sequence is refused, so a bad ack
 * cannot open the credit window without bound.
 */
void display_mirror_ack(uint16_t seq);

/**
 * @brief Ask AO_Display what the mirror is doing right now.
 *
 * Answered with a published DISPLAY_MIRROR_INFO_SIG. The state changes tick to
 * tick, so ask fresh rather than cache it.
 */
void display_mirror_info_request(void);

/**
 * @brief Ask AO_Display to self-test the panel - `test component=display`.
 *
 * Verdict: a published TEST_RESULT_SIG "display". It opens with
 * DISPLAY_TEST_INTRO, the glass still the renderer's to say what is coming,
 * until BUTTON_TOP or KODE_DISPLAY_TEST_INTRO_MS. Then white, red, green, blue,
 * black are painted as DISPLAY_TEST_SIG questions: BUTTON_TOP confirms,
 * BUTTON_BOT denies, KODE_DISPLAY_TEST_STEP_MS fails; no panel answers
 * `panel=absent` at once. Flushes after the opening are completed but not
 * painted, so it ends with DISPLAY_REPAINT_SIG. Presses are answers until
 * DISPLAY_TEST_DONE: do not act on them.
 */
void display_test_request(void);

/* ---- AO_Touch: owner of the touch panel ---------------------------------
 * The controller's IRQ is P6 of the expander, so this AO reads nothing until
 * AO_IoExp publishes that line falling; from then until the release it polls
 * every KODE_TOUCH_POLL_MS, and publishes TOUCH_EVT_SIG on every change.
 * Waits for DISPLAY_STATE_SIG before touching the chip, because panel and
 * touch share a reset line.
 */
extern QActive * const AO_Touch;    /**< The touch owner AO. */
void Touch_ctor(void);              /**< Construct it; kaos_touch_start() calls it. */
/**
 * @brief Construct AO_Touch and start it at KODE_PRIO_TOUCH, on its own queue and
 *        stack.
 */
void kaos_touch_start(void);
/** @brief Register its console verbs and its self-test rows; kaos_touch_start() calls it. */
void kaos_touch_register(void);

/**
 * @brief Ask AO_Touch to read, write or dump one field of the CST820.
 *
 * @p field is a resolved kdrv_cst820_field_t; @p value is read only for
 * FIELD_OP_SET. Answered to @p who as TOUCH_FIELD_RSP_SIG (TouchFieldEvt); a NULL
 * @p who is not answered. No controller: KODE_DENIED. A dry pool drops it.
 */
void touch_field_request(QActive *who, uint8_t op, uint16_t field, int32_t value);

/**
 * @brief Ask AO_Touch to run the guided touch test - `test component=tp`.
 *
 * Checks the identity, then shows two targets as TOUCH_TEST_SIG prompts; a tap's
 * average must land within KODE_TOUCH_TEST_TOL_PX. A miss repeats the target, the
 * KODE_TOUCH_TEST_TRIES-th fails; KODE_TOUCH_TEST_STEP_MS with none times out.
 * Nothing is calibrated. Verdict: TEST_RESULT_SIG "tp"; no controller fails at once.
 */
void touch_test_request(void);

/**
 * @brief Subscribe @p who to TOUCH_EVT_SIG, AO_Touch's report of the finger.
 *
 * A bare subscription names nobody: this call is what links AO_Touch into an
 * app that listens for touch, so that kode_init() starts it.
 *
 * @param who The listening Active Object.
 */
void touch_subscribe(QActive const *who);

/**
 * @brief Ask AO_Touch for the guided touch test on behalf of another client.
 *
 * touch_test_request()'s test; each TOUCH_TEST_SIG step carries @p origin, so
 * AO_Autotest and the console (TEST_ORIGIN_AUTOTEST only) ignore it. A dry pool
 * drops the request.
 *
 * @param origin The client's own number; TEST_ORIGIN_AUTOTEST (0) is the
 *               autotest's, what touch_test_request() uses.
 */
void touch_test_request_from(uint8_t origin);

/**
 * @brief End the guided touch test client @p origin asked for, with no verdict.
 *
 * The same as buttons_test_cancel_request(), for AO_Touch.
 *
 * @param origin The client's own number, as touch_test_request_from() had it.
 */
void touch_test_cancel_request(uint8_t origin);

/**
 * @brief One step of the guided touch test as its record reads on the wire.
 *
 * The one format every client prints: `step=<n> of=<total> target=<x>,<y>` for a
 * prompt, `step=<n> target=<x>,<y> got=<x>,<y> result=pass|fail|miss` for a tap,
 * `step=<n> target=<x>,<y> result=timeout` for none, without the `>|`.
 *
 * @param t    The step, as AO_Touch published it.
 * @param out  Receives the record, NUL-terminated.
 * @param size Bytes at @p out.
 * @return The record's length; 0 for a step with no record (TP_TEST_DONE)
 *         or a NULL argument.
 */
size_t touch_test_record(TouchTestEvt const *t, char *out, size_t size);

/* ---- AO_IoExp: owner of the TCA6408A expander ----------------------------
 * Reads the eight pins on the chip's interrupt (GPIO6) and on request, publishes
 * IOEXP_INPUT_SIG, and drives an output when asked by event (AO_Display's P7
 * supply, the console). Priority above AO_Display: an edge is one read, one publish.
 */
extern QActive * const AO_IoExp;    /**< The expander owner AO. */
void IoExp_ctor(void);              /**< Construct it; kaos_ioexp_start() calls it. */
/**
 * @brief Construct AO_IoExp and start it at KODE_PRIO_IOEXP, on its own queue and
 *        stack.
 */
void kaos_ioexp_start(void);

/**
 * @brief Ask AO_IoExp to read the input register now and publish it.
 *
 * The debounce in AO_Buttons asks for its confirming sample this way, and
 * `btn_read` asks for its snapshot. The answer is a published
 * IOEXP_INPUT_SIG, changed or not.
 */
void ioexp_read_request(void);

/**
 * @brief Ask AO_IoExp to read, write or dump one field of the expander.
 *
 * @p field is a resolved kdrv_tca6408a_field_t; @p value is read only for
 * FIELD_OP_SET. Posted to @p who (not published) as IOEXP_FIELD_RSP_SIG
 * (IoexpFieldEvt); a NULL @p who is not answered. A dry pool drops it.
 */
void ioexp_field_request(QActive *who, uint8_t op, uint16_t field, int32_t value);

/**
 * @brief Ask AO_IoExp to self-test the expander - `test component=ioexp`.
 *
 * The verdict is a published TEST_RESULT_SIG named "ioexp": the chip
 * answers, P0..P6 read as inputs, and reading the input register releases
 * the INT line. Every reading goes out as a log line before the verdict.
 */
void ioexp_test_request(void);

/* ---- AO_Buttons: owner of the six push-buttons --------------------------
 * Five are bits of AO_IoExp's publish, the sixth is GPIO35. Two equal samples
 * KODE_SYS_BUTTON_SETTLE_MS apart move a button; BUTTON_EVT_SIG goes out on change.
 * Top and bottom publish BUTTON_HOLD_SIG after KODE_BUTTON_HOLD_MS; top publishes
 * BUTTON_DOUBLE_SIG for a second press within KODE_BUTTON_DOUBLE_MS of the first's
 * release (not after a hold).
 */
extern QActive * const AO_Buttons;  /**< The button owner AO. */
void Buttons_ctor(void);            /**< Construct it; kaos_buttons_start() calls it. */
/**
 * @brief Construct AO_Buttons and start it at KODE_PRIO_BUTTONS, on its own queue
 *        and stack.
 */
void kaos_buttons_start(void);

/**
 * @brief Ask AO_Buttons to log what every button reads right now.
 *
 * The console's `btn_read`: tells "nobody pressed" from "the expander is silent".
 */
void buttons_dump_request(void);

/**
 * @brief Ask AO_Buttons to run the guided button test - `test component=buttons`.
 *
 * Six BTN_TEST_SIG prompts, one button each, alternating pad and single buttons to
 * expose bridges. A press passes if alone and released in KODE_BUTTONS_TEST_RELEASE_MS.
 * Wrong button: forgiven twice, then fail. Two at once: forgiven once. Each prompt
 * waits for all released (two held reset the P4). KODE_BUTTONS_TEST_STEP_MS times
 * out. Verdict: TEST_RESULT_SIG "buttons"; a button held at the start fails at once.
 */
void buttons_test_request(void);

/**
 * @brief Ask AO_Buttons for the guided button test on behalf of another client.
 *
 * buttons_test_request()'s test; each BTN_TEST_SIG step carries @p origin, so
 * AO_Autotest and the console (TEST_ORIGIN_AUTOTEST only) ignore it. A dry pool
 * drops the request.
 *
 * @param origin The client's own number; TEST_ORIGIN_AUTOTEST (0) is the
 *               autotest's, what buttons_test_request() uses.
 */
void buttons_test_request_from(uint8_t origin);

/**
 * @brief End the guided button test client @p origin asked for, with no verdict.
 *
 * The prompts stop and nothing more is published for it. A test not running, or
 * another client's, is left alone. A dry pool drops it; the test then times out.
 *
 * @param origin The client's own number, as buttons_test_request_from() had it.
 */
void buttons_test_cancel_request(uint8_t origin);

/**
 * @brief One step of the guided button test as its record reads on the wire.
 *
 * The one format every client prints, the console's `test
 * component=buttons` and AO_TestDot's run alike: `step=<n> of=<total>
 * press=<name>` for a prompt, `step=<n> button=<name> result=pass ms=<ms>`,
 * `step=<n> button=<name> result=timeout`, and for every other outcome
 * `step=<n> button=<name> result=stuck|wrong|double|doublefail|held|giveup
 * pressed=<names joined by '+'> ms=<ms>`, without the `>|`.
 *
 * @param t    The step, as AO_Buttons published it.
 * @param out  Receives the record, NUL-terminated.
 * @param size Bytes at @p out.
 * @return The record's length; 0 for a step with no record (BTN_TEST_DONE),
 *         a button out of range or a NULL argument.
 */
size_t buttons_test_record(ButtonsTestEvt const *t, char *out, size_t size);

/**
 * @brief Which buttons are down: bit (1 << kdrv_button_t) set = pressed.
 *
 * Each button as the last BUTTON_EVT_SIG about it said, injected or confirmed, so
 * a poll and a subscriber agree. Never blocks, touches no bus; safe from any task
 * or AO. What kode_sys_button_read_mask() answers. Ids are the published ones,
 * pad rotation included; `btn_read` and the guided test report them as wired.
 */
uint8_t buttons_down(void);

/**
 * @brief How many times a button has been pressed since boot.
 *
 * Counts up-to-down edges of the same stream as buttons_down(), injected presses
 * included. Monotonic, wraps at 2^32; one atomic 32-bit read, safe from any task
 * or core. What kode_sys_button_read_presses() answers. Ids are the published ones.
 *
 * @param id A kdrv_button_t.
 * @return The presses so far; 0 for an id out of range.
 */
uint32_t buttons_presses(uint8_t id);

/**
 * @brief Publish one button change as though AO_Buttons had confirmed it.
 *
 * The console's `btn`: indistinguishable downstream, and buttons_down() follows it.
 * Stamped with the call's time; safe from any task. It starts no hold clock and
 * feeds no double-press detector: a script publishes hold or double itself
 * (`btn ... state=hold|double`).
 *
 * @param id      A kdrv_button_t.
 * @param pressed The new state.
 * @return false for an id out of range or a dry event pool (nothing
 *         published, nothing recorded); true once published.
 */
bool buttons_inject(uint8_t id, bool pressed);

/**
 * @brief Turn the pad with the glass, by @p quarter_turns.
 *
 * For kscreen_set_rotation() with CONFIG_KSCREEN_ROTATE_PAD: the four pad buttons
 * publish the direction they point on the rotated glass; TOP and BOT never turn.
 * Applied once no pad button is down, so a press and its release name the same
 * button. Posts a static event: never lost. Safe from any AO.
 *
 * @param quarter_turns 0, 1, 2 or 3 for 0, 90, 180 or 270 degrees, the values
 *                      of lv_display_rotation_t. Anything else is ignored.
 */
void buttons_rotation_request(uint8_t quarter_turns);

/* ---- AO_Power: owner of the PMIC and the fuel gauge ---------------------
 * Two chips, one owner: the BQ25896 knows whether the cell is charging and
 * the CW2217 knows what its voltage is, and neither is useful alone.
 * Publishes POWER_EVT_SIG every couple of seconds.
 */
extern QActive * const AO_Power;    /**< The battery owner AO. */
void Power_ctor(void);              /**< Construct it; kaos_power_start() calls it. */
/**
 * @brief Construct AO_Power and start it at KODE_PRIO_POWER, on its own queue and
 *        stack, then register its settings.
 */
void kaos_power_start(void);

/** @brief Ask AO_Power to log everything both chips say - the console's `bat_info`. */
void power_dump_request(void);

/**
 * @brief Ask AO_Power for one reading, published as POWER_EVT_SIG, and for the
 *        wall clock to be kept if a keep is due. Logs nothing.
 *
 * What AO_LowPower asks once a minute while the board sleeps: the cable shows
 * in the reading, and the poll that keeps the clock does not run asleep.
 */
void power_peek_request(void);

/**
 * @brief Ask AO_Power to read, write or dump one field of one chip.
 *
 * The console's `pmic` and `gauge`. @p field is the resolved driver enum
 * (kdrv_bq25896_field_t or kdrv_cw2217_field_t); @p value is read only for
 * POWER_OP_SET. Answered with a published POWER_FIELD_SIG. A dry pool drops it.
 */
void power_field_request(uint8_t chip, uint8_t op, uint16_t field, int32_t value);

/**
 * @brief Ask AO_Power to self-test one chip - `test component=pmic|gauge`.
 *
 * Verdict: TEST_RESULT_SIG "pmic" or "gauge"; needs the battery and USB. PMIC: id,
 * boot policy, 5 V USB, a cell voltage, SYS regulated, thermistor in band, no
 * fault, charging. Gauge: id, algorithm up, voltage and current agree with the
 * PMIC's (at least 200 mA). POWER_DEPTH_ANSWER skips SYS, charging and current.
 *
 * @param chip  POWER_CHIP_PMIC or POWER_CHIP_GAUGE.
 * @param depth POWER_DEPTH_FULL or POWER_DEPTH_ANSWER.
 */
void power_test_request(uint8_t chip, uint8_t depth);

/**
 * @brief Run the `power_plug` row: the charger in, both chips judged, the
 *        charger out.
 *
 * Guided, its steps published as POWER_TEST_SIG. No charger within
 * KODE_POWER_PLUG_STEP_MS answers TEST_SKIP_SIG; a charger that is never
 * taken out again publishes a POWER_TEST_TIMEOUT step and fails. The gauge's
 * ammeter is judged only when the cell takes enough current to prove it. The
 * answer is named "power".
 */
void power_plug_test_request(void);

/**
 * @brief Ask AO_Power for one owner-level operation on the PMIC.
 *
 * The console's `boost` and `pmic_reset`; the facade's power calls post
 * power_ctrl_evt() themselves. @p op is POWER_CTRL_*, @p value its argument.
 * Answered with a published POWER_CTRL_SIG. A dry pool drops it.
 */
void power_ctrl_request(uint8_t op, int32_t value);

/**
 * @brief The event power_ctrl_request() posts, built for a caller that posts it itself.
 * @param op    POWER_CTRL_*.
 * @param value Its argument.
 * @return The event, or NULL when the pool is dry.
 */
QEvt const *power_ctrl_evt(uint8_t op, int32_t value);

/**
 * @brief Ask AO_Power to cut the battery from VSYS (BQ25896 ship mode).
 *
 * On battery the board powers off while AO_Power handles it, so nothing after
 * runs. Wake: BUTTON_BOT (QON) or USB. With USB present the board stays up and
 * AO_Power logs that. Posts with KQP_POWER_POST_MARGIN.
 *
 * @return true when the request was queued; false when the queue is within
 *         its margin, and nothing was queued.
 */
bool power_ship_request(void);

/** @brief AO_Power's last reading, as the facade needs it. */
typedef struct {
    uint16_t vbat_mv;   /**< The cell, mV: the gauge's, else the PMIC's (20 mV steps). */
    uint8_t  soc_pct;   /**< The level, 0-100; only meaningful when gauge_ok.                          */
    uint8_t  charging;  /**< kdrv_bq25896_chrg_stat_t.                                                 */
    bool     vbus;      /**< The PMIC sees a good input.                                               */
    bool     gauge_ok;  /**< The fuel gauge answered for this reading.                                 */
} power_reading_t;

/**
 * @brief Copy out AO_Power's last reading, whole, from any task.
 * @param out Receives the reading; untouched when there is none.
 * @return false before the first reading, or when @p out is NULL.
 */
bool power_reading(power_reading_t *out);

/**
 * @brief Ask AO_Power to tell @p who when the cable has been out for
 *        KODE_SHIP_UNPLUG_MS.
 *
 * For the factory reset's ship end; AO_Power never ships by itself. Counts from
 * the poll that sees VBUS gone (KODE_POWER_POLL_MS); a poll that sees it back
 * restarts the wait. One POWER_SHIP_DUE_SIG per arming. Not stored. A second call
 * replaces @p who; there is no cancel.
 *
 * @param who The Active Object to receive POWER_SHIP_DUE_SIG. Not NULL.
 * @return false when not posted (dry pool, full queue or NULL @p who), logged;
 *         then nothing is armed.
 */
bool power_ship_on_unplug_request(QActive *who);

/**
 * @brief Ask AO_Power for the pads delivery test's boost, on or off.
 *
 * On: BOOST_LIM to KODE_TEST_DOT_BOOST_LIM_CODE (1.2 A), then the boost on with
 * the usual checks. Off: boost off, KBSP_CELL_BOOST_LIM_CODE back. Never stored;
 * the board's limit also returns on VBUS falling and at start. Answered with one
 * POWER_BOOST_TEST_SIG to @p who, fault and current from the next poll.
 *
 * @param who The Active Object answered. Not NULL.
 * @param on  true for the boost on at the raised limit, false for off.
 * @return false when nothing was posted: the pool was dry, AO_Power's queue
 *         was within KQP_POWER_CMD_MARGIN, or @p who was NULL. No answer is
 *         coming then.
 */
bool power_boost_test_request(QActive *who, bool on);

/* ---- AO_Radio: owner of the ESP32-C5 radio coprocessor ------------------
 * The C5's EN line, the esp-hosted SDIO link, its GPIOs (by RPC) and its flash,
 * all through one sequential RPC channel: one job in flight, blocking work on the
 * AO's workers. With the link off the C5 runs its own firmware (microSD and amp
 * power included). Around the link it moves AO_Storage's card between mount paths:
 * esp-hosted takes the SDMMC controller. Compiled under CONFIG_KODEOS_RADIO;
 * publishes RADIO_STATE_SIG on every change and on request.
 */
extern QActive * const AO_Radio;    /**< The C5 owner AO. */
void Radio_ctor(void);              /**< Construct it; kaos_radio_start() calls it. */
/**
 * @brief Construct AO_Radio and start it at KODE_PRIO_RADIO, on its own queue and
 *        stack, then register its settings.
 */
void kaos_radio_start(void);
/** @brief Register its console verbs and its self-test rows; kaos_radio_start() calls it. */
void kaos_radio_register(void);

/** @brief Where the C5 link is (RadioStateEvt.link). */
typedef enum {
    KRADIO_OFF = 0,   /**< No link. The C5 runs its own firmware unbothered. */
    KRADIO_STARTING,  /**< Bring-up in flight: the C5 is being reset, the
                       *   card is unmounted until the link settles.         */
    KRADIO_UP,        /**< CP_INIT seen; the RPC channel answers.            */
    KRADIO_STOPPING,  /**< Teardown in flight.                               */
    KRADIO_FAILED,    /**< Bring-up failed or the transport dropped. A new
                       *   RADIO_UP_REQ_SIG retries from scratch.            */
} kradio_link_t;

/**
 * @brief Ask AO_Radio to bring the C5 link up. Outcome: RADIO_STATE_SIG.
 *
 * Dropped, with a line, when AO_Radio's queue is within KQP_RADIO_POST_MARGIN
 * of full: the asker's own budget for the link covers a lost ask.
 */
void radio_up_request(void);

/** @brief Ask AO_Radio to tear the link down. The C5 keeps running. */
void radio_down_request(void);

/**
 * @brief Ask AO_Radio to publish RADIO_STATE_SIG again.
 *
 * Same contract as display_state_request(): only once kode_init() has
 * returned - posting to an unstarted AO is a FreeRTOS assert, not a no-op.
 */
void radio_state_request(void);

/**
 * @brief Ask AO_Radio to leave the C5 parked for a jump that restarts the board.
 *
 * AO_Boot's request, once before it restarts. With the link up AO_Radio hangs up
 * BLE, asks the C5 to park and, only on its yes, leaves the note for @p target.
 * Always answered with one BOOT_RADIO_PARKED_SIG to AO_Boot; with no link, at once.
 * Compiled under CONFIG_KODEOS_RADIO; guard the call the same way.
 *
 * @param target The role of the firmware the restart goes to.
 */
void radio_park_request(kdrv_radio_note_t target);

/**
 * @brief The interface reads its first screen off the card: leave it mounted
 *        through this boot's first connect, and hold the link's first words.
 *
 * Called before AO_Radio starts. radio_boot_release() ends the hold, and
 * KDRV_RADIO_GATE_MS does if nobody calls it.
 */
void radio_boot_hold(void);

/** @brief The first read is over: the link may speak (radio_boot_hold()). */
void radio_boot_release(void);

/* The wifi_*_request() calls below exist in a firmware built with
 * CONFIG_KODEOS_WIFI, and only there: the declarations stay, as the BLE ones
 * do, and a build without WiFi that calls one fails at link time naming it -
 * which is the answer, since there is no station to ask. */

/**
 * @brief Ask for a WiFi scan through the C5's radio.
 *
 * Answered on the bus: one WIFI_SCAN_ENTRY_SIG per network, then WIFI_SCAN_END_SIG,
 * even with no link (ok=false), so silence never stands for "no networks".
 */
void wifi_scan_request(void);

/**
 * @brief Ask AO_Radio to end the listing in flight before its last channel.
 *
 * The sweep wifi_scan_request() started ends after its step on the air, a few
 * seconds at most, and its WIFI_SCAN_END_SIG says @c cut, with the networks
 * heard so far before it. No other sweep is touched: not an asked scan, the
 * automatic join's round or a self-test row. With no listing in flight
 * nothing happens. No answer of its own.
 *
 * @return true when the request was posted; false when AO_Radio's queue is
 *         within KQP_RADIO_POST_MARGIN of full, and then nothing was asked.
 */
bool wifi_scan_stop_request(void);

/**
 * @brief Scan for the AO @p who; kode_sys_wifi_scan()'s door.
 *
 * Brings the link up if needed. Entries are POSTED to @p who as WIFI_SCAN_ENTRY_SIG
 * while its queue keeps KODE_STORAGE_LIST_FREE_SLOTS free, then one
 * WIFI_SCAN_RSP_SIG (no margin, refusals included): the count, rc (KODE_OK;
 * KODE_ERROR failed or link lost; KODE_NO_RESOURCES radio busy with another scan,
 * a BLE listen, the row or an install: ask again) and whether entries were dropped.
 *
 * @param who The asking AO.
 * @return KODE_OK when posted; KODE_DENIED for a NULL or unstarted @p who,
 *         or its scan still running; KODE_NO_RESOURCES when the pool is dry
 *         or AO_Radio's queue is within KQP_RADIO_CMD_MARGIN, nothing posted.
 */
kode_status_t wifi_scan_ask(QActive *who);

/**
 * @brief Scan for a blocked task; kode_block_wifi_scan()'s request.
 *
 * As wifi_scan_ask(), answered in @p wait instead: the rows the sweep kept
 * copied into @c wait->rows, @c rc and @c got set, @p gen stamped into
 * @c wait->ack, then @c wait->done signalled. With the pool dry or AO_Radio's
 * queue within KQP_RADIO_CMD_MARGIN it is answered the same way at once,
 * KODE_NO_RESOURCES. @p wait must outlive the answer.
 *
 * @param wait Where the answer goes; its @c done, @c rows and @c cap set.
 * @param gen  The task's tag for this request, never 0.
 */
void wifi_scan_request_wait(ScanWaiter *wait, uint32_t gen);

/**
 * @brief Ask AO_Radio to start or stop telling @p who about Wi-Fi frames.
 *
 * Posts and returns. Always answered to @p who with WIFI_FRAMES_STATE_SIG, in
 * every state of the link: with it down the answer is KWIFI_FRAMES_NO_LINK.
 * While on, @p who gets one WIFI_FRAMES_TICK_SIG per period in which the C5
 * heard a frame, and after it one WIFI_FRAME_SIG per frame handed over; all of
 * them are posted to @p who with a margin, never published, and what its queue
 * has no room for is counted in the next tick's @c dropped. Receive only.
 * Starts nothing that was off and never leaves a network: joined, only channel
 * 0 or the network's. One asker listens at a time: another is answered
 * KWIFI_FRAMES_BUSY, and its stop changes nothing.
 *
 * @param who     Where the answers go. Never NULL.
 * @param on      true to listen, false to stop.
 * @param channel 1..KWIFI_FRAMES_CHANNEL_MAX, or 0 to stay where the radio is.
 *                Ignored for a stop.
 * @return true when the request was posted; false when the event pool was dry
 *         or AO_Radio's queue is within KQP_RADIO_CMD_MARGIN of full, and then
 *         nothing answers.
 */
bool wifi_frames_request(QActive *who, bool on, uint8_t channel);

/**
 * @brief Ask AO_Radio to join @p ssid.
 *
 * Answered with WIFI_STATE_SIG: got-ip, or the refusal. Passwords live in the C5's
 * vault: a saved network asked with an empty @p pass is joined without a password
 * crossing the link; otherwise @p pass is sent once and kept if @p remember.
 *
 * @param ssid     The network to join.
 * @param pass     Its password; empty for a saved network or an open one.
 * @param remember Save the network in the vault once it has worked.
 * @return true when the request was posted; false when the event pool was
 *         dry or AO_Radio's queue is within KQP_RADIO_POST_MARGIN of full,
 *         and then nothing answers.
 */
bool wifi_join_request(const char *ssid, const char *pass, bool remember);

/**
 * @brief The event wifi_join_request() posts, built for a caller that posts it itself.
 * @param ssid     The network; NULL for none.
 * @param pass     Its password; NULL or empty for a saved or an open network.
 * @param remember Save the network in the vault once it has worked.
 * @return The event, or NULL when the pool is dry.
 */
QEvt const *wifi_join_evt(const char *ssid, const char *pass, bool remember);

/**
 * @brief Leave the network. The credentials stay remembered.
 *
 * wifi_forget_request() forgets them.
 *
 * @return true when the request was posted; false when AO_Radio's queue is
 *         within KQP_RADIO_POST_MARGIN of full, and then nothing answers.
 */
bool wifi_leave_request(void);

/**
 * @brief The event wifi_leave_request() posts, for a caller that posts it itself.
 * @return A static event; never NULL.
 */
QEvt const *wifi_leave_evt(void);

/**
 * @brief Switch the WiFi service on, and remember that it is on.
 *
 * Persists the wish, then delivers it: link up, hand off to wifi_auto_request().
 * The flag is the wish, not the outcome: a call refused while the C5 is busy is
 * still delivered by the next reconcile or boot. WIFI_STATE_SIG answers every call
 * (`enabled` is the flag). wifi_stop_request() is the opposite.
 */
void wifi_start_request(void);

/**
 * @brief Switch the WiFi service off, and remember that it is off.
 *
 * Takes the station all the way down and clears the stored flag. The link stays
 * up: esp-hosted cannot rebuild its transport within a boot (an idle link costs
 * ~27 mA). Answered with WIFI_STATE_SIG, even when it waits for a job in flight.
 */
void wifi_stop_request(void);

/** @brief Ask AO_Radio to publish WIFI_STATE_SIG now. Always answered. */
void wifi_state_request(void);

/**
 * @brief Ask AO_Radio to refresh the current association's signal strength.
 *
 * `wifi_info` reports the last known value without waiting. Answered with
 * WIFI_STATE_SIG once measured, or at once with no link or no association.
 */
void wifi_rssi_request(void);

/**
 * @brief Ask AO_Radio for the station's fast mode, or for the default back.
 *
 * On: no power save, every frame answered at once, about 27 mA more. Off (default)
 * is modem save. Applied while connected; a (re)connect or stop resets it. No
 * answer: a missed fast mode is only a slow reply. AO_Net is the one caller: it
 * asks while the Kode channel receives command lines and while a job is with one
 * of its workers, and gives the default back when the last of them ends.
 *
 * @param on true for fast, false for the default.
 */
void wifi_fast_request(bool on);

/**
 * @brief Ask AO_Radio to get onto a remembered network by itself.
 *
 * Scans first and tries only the saved networks in range, strongest first (each
 * absent one would cost a 20 s timeout). Answered with WIFI_STATE_SIG. AO_Radio
 * also runs it when the link comes up with WiFi on, unless `wifi_join` is off.
 *
 * @return true when the request was posted; false when AO_Radio's queue is
 *         within KQP_RADIO_POST_MARGIN of full, and then nothing answers.
 */
bool wifi_auto_request(void);

/**
 * @brief The event wifi_auto_request() posts, for a caller that posts it itself.
 * @return A static event; never NULL.
 */
QEvt const *wifi_auto_evt(void);

/**
 * @brief Ask for the remembered networks.
 *
 * Answered on the bus: one WIFI_SAVED_ENTRY_SIG per network, most recently
 * used first, then WIFI_SAVED_END_SIG. Names only - a stored password is
 * never handed back.
 */
void wifi_list_request(void);

/** @brief Ask AO_Radio to forget @p ssid. Does not disconnect. */
void wifi_forget_request(const char *ssid);

/* ---- BLE: a host here, the controller on the C5 -------------------------
 * The radio is the coprocessor's and the stack above HCI is the P4's, so a
 * BLE connection costs the link - which is why AO_Radio owns this too, and
 * why it takes BLE down before anything that resets the C5.
 */

/**
 * @brief Switch the BLE service on, and remember that it is on.
 *
 * On means advertising whenever the flag is set and nobody is connected. The
 * pairing gate on the console service bounds exposure. As wifi_start_request():
 * persisted, link brought up, restored after reboot. Answered with BLE_STATE_SIG.
 */
void ble_start_request(void);

/**
 * @brief Ask for a BLE scan that listens for @p ms.
 *
 * Answered on the bus: one BLE_SCAN_ENTRY_SIG per device (once, at its strongest),
 * then BLE_SCAN_END_SIG. A length out of range gets `ok = false` and no scan.
 *
 * @param ms How long to listen; 0 for KODE_BLE_SCAN_MS.
 * @return true when the request was posted, false when the pool was dry (logged,
 *         nothing answers).
 */
bool ble_scan_request(uint32_t ms);

/**
 * @brief Ask for the listen ble_scan_request() runs, for an AO; the door
 *        kode_sys_ble_scan() opens.
 *
 * Posts and answers as wifi_scan_ask() (BLE_SCAN_ENTRY_SIG, then BLE_SCAN_RSP_SIG),
 * sharing its one slot. rc: KODE_OK; KODE_ERROR failed or link lost; KODE_DENIED
 * Bluetooth off; KODE_NO_RESOURCES radio busy, ask again. The autojoin round on a
 * cold link with Wi-Fi on makes the first listen nearly always busy.
 *
 * @param who The asking AO.
 * @param ms  How long to listen, KODE_BLE_SCAN_MIN_MS..KODE_BLE_SCAN_MAX_MS.
 * @return KODE_OK when posted; KODE_DENIED for a NULL or unstarted @p who,
 *         its scan still running, or @p ms out of range; KODE_NO_RESOURCES
 *         when the pool is dry or AO_Radio's queue is within
 *         KQP_RADIO_CMD_MARGIN, nothing posted.
 */
kode_status_t ble_scan_ask(QActive *who, uint32_t ms);

/**
 * @brief Listen for a blocked task; kode_block_ble_scan()'s request.
 *
 * As ble_scan_ask(), answered in @p wait: devices (kdrv_ble_dev_t) in @c wait->rows,
 * @c rc and @c got set, @p gen in @c wait->ack, then @c wait->done. A dry pool or
 * full queue answers at once KODE_NO_RESOURCES; @p ms out of range, KODE_DENIED.
 * @p wait must outlive the answer.
 *
 * @param wait Where the answer goes; its @c done, @c rows and @c cap set.
 * @param gen  The task's tag for this request, never 0.
 * @param ms   As ble_scan_ask().
 */
void ble_scan_request_wait(ScanWaiter *wait, uint32_t gen, uint32_t ms);

/**
 * @brief The request that sets the scan response's name and maker data.
 *
 * Both together. AO_Radio keeps them for the boot (never stored) and hands them to
 * the radio whenever BLE is up; the advert never changes. A pair that does not fit
 * KDRV_BLE_ADV_MAX is refused there with a log line.
 *
 * @param name The name, shorter than KQP_BLE_NAME_MAX; NULL or "" for the
 *             board's serial.
 * @param data The maker data, company id (little-endian) first; NULL when
 *             @p len is 0.
 * @param len  Its bytes, 0 or 2 to KQP_BLE_DATA_MAX.
 * @return The event, for kode_sys_cmd_post(); NULL when the pool is dry or an
 *         argument does not fit the event.
 */
QEvt const *ble_rsp_evt(const char *name, const uint8_t *data, size_t len);

/**
 * @brief The request that sets, or sets and notifies, one value of the app's
 *        own service, or pushes a value on a queued characteristic or empties
 *        it: AO_Radio, its owner, hands the copy to the driver.
 *
 * A command carries no waiter and has no answer; a blocked task passes its
 * waiter and its tag, and AO_Radio writes the driver's status into it in the
 * step that receives it.
 *
 * @param chr  The characteristic's name, shorter than KQP_BLE_APP_NAME_MAX.
 * @param op   KQP_BLE_APP_OP_SET, _NOTIFY, _PUSH or _CLEAR.
 * @param data The bytes; NULL when @p len is 0.
 * @param len  Their count, 0 to KQP_BLE_APP_VALUE_MAX.
 * @param wait A blocked task's waiter; NULL for a command.
 * @param gen  The task's tag; 0 for a command.
 * @return The event, for kode_sys_cmd_post(); NULL when the pool is dry or an
 *         argument does not fit the event.
 */
QEvt const *ble_value_evt(const char *chr, uint8_t op, const void *data, size_t len,
                          BleValueWaiter *wait, uint32_t gen);

/**
 * @brief Whether Bluetooth is on in Settings: the `ble` flag, as AO_Radio
 *        last wrote it. Never waits; false before AO_Radio has started.
 * @return true when it is on.
 */
bool ble_enabled(void);

/**
 * @brief Switch the BLE service off, and remember that it is off.
 *
 * Takes the host down and disables the C5's controller and clears the
 * stored flag. The link itself stays up for the rest of the boot, for the
 * reason wifi_stop_request() gives. Answered with BLE_STATE_SIG.
 */
void ble_stop_request(void);

/**
 * @brief How many console lines the event pool could not carry.
 *
 * Dropping is deliberate; `ps` reports the count so a lost terminator is not
 * mistaken for a board that stopped answering.
 */
uint32_t log_dropped(void);

/**
 * @brief Bytes the console's input side lost outright.
 *
 * Bytes that never became an event on the way in, past the reader's bounded
 * retry (log_dropped() counts the way out). `ps` reports both.
 *
 * @return How many bytes were lost since boot. Only ever grows.
 */
uint32_t console_rx_dropped(void);

/**
 * @brief Whether a producer of many records should wait before the next slice.
 *
 * Output is paced by the slowest attached port: BLE carries far less than the
 * cable, and a listing at full speed would lose its tail there. AO_Log owns the
 * ports, so it answers.
 *
 * @return true while the output is backed up; ask again shortly.
 */
bool console_congested(void);

/** @brief Ask AO_Radio to publish BLE_STATE_SIG now. Always answered. */
void ble_state_request(void);

/**
 * @brief Ask AO_Radio to forget every bonded peer.
 *
 * Refused while a central is attached: its link would outlive its key. Use
 * ble_stop_request() to disconnect.
 */
void ble_unpair_request(void);

/**
 * @brief Ask AO_Radio to show the board for five minutes, so a new device can
 *        pair.
 *
 * With bonds, the board is nameless, not discoverable and refuses new pairings;
 * this lifts that until the time runs out or a device pairs. Answered by
 * BLE_STATE_SIG (`pair_open`); refused, with a log line, while Bluetooth is off.
 */
void ble_pair_open_request(void);

/**
 * @brief Refuse the pairing whose code is currently on the glass.
 *
 * The peer is hung up on rather than left for the security manager's 30 s.
 * Answered by BLE_PAIR_SIG (DONE, REJECTED); nothing in flight, nothing published.
 * Compiled under CONFIG_KODEOS_BLE_PAIRING_PASSKEY (default on). Only kodeOS shows
 * a code; an app refuses new pairings first.
 */
void ble_pair_reject_request(void);

/**
 * @brief Send an image to the coprocessor from @p src.
 *
 * Reported as every flash write: APP_FLASH_PROGRESS_SIG from 0 % once the medium
 * opens, then APP_FLASH_DONE_SIG or APP_FLASH_FAIL_SIG.
 *
 * @param src_kind  KFW_SRC_FS, KFW_SRC_SD or KFW_SRC_HTTP. The image is decoded
 *                  whole into PSRAM and verified before the C5's session opens.
 * @param src       File name, or URL.
 */
void c5_flash_request(uint8_t src_kind, const char *src);

/**
 * @brief Ask AO_Radio to install a coprocessor image arriving on the console.
 *
 * `install src="terminal" dst=c5`: AO_Log hands AO_Radio exactly @p size raw bytes,
 * which are CRC'd, decoded to PSRAM and fully verified (digest, header, chip id)
 * before the C5 is asked; the link is brought up then if off. Answered with
 * RECV_READY_SIG, RECV_ACK_SIG per chunk, then RECV_DONE_SIG (the C5 checked and
 * activated it) / RECV_FAIL_SIG / RECV_STALL_SIG, plus APP_FLASH_* progress.
 * The link stays down for the rest of the boot.
 *
 * @param size  Payload length in bytes.
 * @param crc32 What khal_crc32() over the whole payload must equal.
 * @param src   KODE_CONSOLE_SRC_*: the port the payload follows on.
 * @return false if the request event could not be allocated - nothing is
 *         coming, and the caller must not wait.
 */
bool c5_recv_request(uint32_t size, uint32_t crc32, uint8_t src);

/* ---- The unit's identity, held by the C5 --------------------------------- */

/**
 * @brief Ask the C5 to sign the cloud's challenge with the unit's key.
 *
 * The C5 signs only "kode-auth/v1\n" + nonce, so the key proves the unit and
 * nothing else. Always answered by one ID_SIGN_RSP_SIG: KODE_ST_OK with the DER
 * signature, KODE_ST_BAD_REQUEST bad nonce, KODE_ST_NOT_FOUND no key yet, or
 * KODE_ST_REFUSED (link down, busy, or no answer within 3 s).
 *
 * @param who   Where the answer goes.
 * @param nonce 1..KODE_ID_NONCE_MAX characters of [A-Za-z0-9._-].
 * @return false if the request event could not be allocated - nothing is
 *         coming, and the caller must not wait.
 */
bool id_sign_request(QActive *who, const char *nonce);

/**
 * @brief Ask for one identity operation that is not a signature.
 *
 * Answered to @p who by ID_PART_SIG records - the certificate pair for
 * ID_OP_CERT, the CSR for ID_OP_CSR - then one ID_END_SIG, always.
 * ID_OP_CERT_PUT parts go in order from 1; each is answered by its own end,
 * and the last one's end says whether the C5 kept the pair (it refuses a
 * certificate for another key).
 *
 * @param who Where the answer goes.
 * @param op  IdOp.
 * @param n   ID_OP_CERT_PUT: this part, 1..of; 0 otherwise.
 * @param of  ID_OP_CERT_PUT: parts in all, 1..KODE_ID_PARTS_MAX; 0 otherwise.
 * @param der ID_OP_CERT_PUT: the part's DER; NULL otherwise.
 * @param len Bytes of der, 1..KODE_ID_PART_BYTES; 0 otherwise.
 * @return false if the request event could not be allocated - nothing is
 *         coming, and the caller must not wait.
 */
bool id_request(QActive *who, uint8_t op, uint8_t n, uint8_t of,
                const uint8_t *der, size_t len);

/**
 * @brief Forget the user's secrets: the C5's saved networks, then its BLE
 *        keys, then this chip's bond store. The unit's identity stays - it
 *        belongs to the board, not to the user.
 *
 * Answered to @p who by one C5_WIPE_RSP_SIG, always: C5_WIPE_DONE, or the
 * step that failed and why (KODE_ST_REFUSED for the link down, a step busy
 * or no answer within 3 s). A failed step stops the wipe there.
 *
 * @param who Where the answer goes.
 * @return false if the request event could not be allocated - nothing is
 *         coming, and the caller must not wait.
 */
bool c5_wipe_request(QActive *who);

/**
 * @brief Run the coprocessor's production row - `test component=c5`.
 *
 * Brings the link up if down (never resets it if up), snapshots, scans. Verdict:
 * TEST_RESULT_SIG "c5": an ESP32-C5 on the host's esp-hosted version and a Kode
 * slave build, SDIO at four lines and board clock, card supply on, strongest
 * network above KODE_C5_TEST_RSSI_MIN_DBM. Readings are logged; `wifi` and `ble`
 * flags untouched. Waits for a job; fails at once during an update or after `radio_down`.
 */
void c5_test_request(void);

/**
 * @brief The same row as the owner of a unit runs it.
 *
 * The link, the snapshot, a WiFi sweep that completes and the BLE
 * controller, with no floor on what either scan hears: what is in range
 * depends on where the unit is. Answers TEST_RESULT_SIG named "c5".
 */
void c5_test_request_user(void);

/**
 * @brief Read, write or dump one field of the coprocessor - the console's `c5`.
 *
 * @p field is a resolved kdrv_c5_field_t. Answered to @p who as RADIO_FIELD_RSP_SIG
 * (RadioFieldEvt) from the last bring-up's snapshot: no link crossed, answered in
 * every state, refusals included. A NULL @p who is not answered.
 */
void c5_field_request(QActive *who, uint8_t op, uint16_t field, int32_t value);

/**
 * @brief Ask AO_Radio for the expansion header's +3.3V_EXT on or off.
 *
 * The enable is the C5's IO5 (pulled up by R72). The wish is kept for the boot,
 * off by default, and applied when the C5 can: link up, as a job after the one in
 * flight; no link, "on" resets the C5 for RADIO_EXP_PULSE_MS, "off" waits. kodeOS
 * never asks, so its link-up switches the rail off. No answer: the end is published
 * as EXP_POWER_SIG (ExpPowerDoneEvt); `c5` reads back `pwr_ext`.
 *
 * @param on true for the rail on, false for off.
 */
void exp_power_request(bool on);

/**
 * @brief The event exp_power_request() posts, for a caller that posts it itself.
 * @param on true for the rail on, false for off.
 * @return A static event; never NULL.
 */
QEvt const *exp_power_evt(bool on);

/**
 * @brief Ask AO_Radio for the radio check, in a few seconds.
 *
 * One run at a time; only what @p req asks, in order: the C5's card (fresh chip,
 * esp-hosted and MAC snapshot); the 2.4 and 5 GHz networks (KODE_RADIO_PROBE_TRIES
 * passes each, strongest kept) or, with @c discover, one sweep of every channel;
 * then the BLE name (KODE_RADIO_FIND_MS). A running sweep is cut (KODE_DENIED),
 * except the production row's. Bounded by KODE_RADIO_PROBE_MS between jobs.
 *
 * @param who The AO the answer goes to: one RADIO_PROBE_SIG with the readings and
 *            KODE_DENIED no link, KODE_NO_RESOURCES busy or updating, KODE_TIMEOUT,
 *            or a failed job's status.
 * @param req What to check.
 * @return true when the request was posted and will be answered; false when
 *         the pool is dry, AO_Radio's queue is within KQP_RADIO_CMD_MARGIN
 *         or an argument is NULL, nothing posted.
 */
bool radio_probe_request(QActive *who, const radio_probe_t *req);

/**
 * @brief Ask AO_Radio for the speaker amplifier on or off.
 *
 * The NS4150B's enable is the C5's IO4, raised by the C5 at its boot. On: a held
 * C5 (KODEOS_C5_HOLD) is let go, C5_RUNNING_SIG once booted; over the link IO4 is
 * driven high. Off: IO4 low now or at the next link-up. AO_Audio asks on before its
 * first sound. Published (C5_RUN_REQ_SIG, C5_AMP_OFF_SIG), never blocks, no answer.
 *
 * @param on true for the amplifier on.
 */
void radio_amp_request(bool on);

/**
 * @brief Subscribe @p who to the app's own Bluetooth service: BLE_APP_WRITE_SIG,
 *        a copy of each write a central makes, BLE_APP_LINK_SIG, each edge of
 *        the link, and BLE_APP_TAKEN_SIG, each value a read takes from a queue.
 *
 * Beside AO_Radio's start, so the call links AO_Radio (tools/check_starts.sh).
 * The service itself is the app's KODE_SYS_BLE_SERVICE() or KODE_BLE_SERVICE().
 *
 * @param who The listening Active Object.
 */
void ble_app_subscribe(QActive const *who);

/* ---- AO_Net: owner of the host's network services -----------------------
 * lwIP runs on the P4 and the C5 only carries frames, so these are not AO_Radio's:
 * a GET must not queue behind a 13 s WiFi scan (https://docs.kode.diy/sdk/kaos/net).
 * It owns the MQTT client and sequences one blocking job at a time on its worker,
 * answering every request, refusals included. Compiled under CONFIG_KODEOS_NET
 * (needs KODEOS_WIFI). An app that only fetches uses kode_sys.h's blocking helpers.
 */
extern QActive * const AO_Net;      /**< The network services AO. */
void Net_ctor(void);                /**< Construct it; kaos_net_start() calls it. */
/**
 * @brief Construct AO_Net and start it at KODE_PRIO_NET, on its own queue and
 *        stack, then register its settings.
 */
void kaos_net_start(void);
/** @brief Register its console verbs and its self-test rows; kaos_net_start() calls it. */
void kaos_net_register(void);

/**
 * @brief Ask for an ICMP ping of @p host.
 *
 * Answered on the bus: one NET_PING_ENTRY_SIG per echo, timeouts included, then
 * NET_PING_END_SIG, even with no network (ok false).
 *
 * @param host  Name or dotted-quad literal.
 * @param count Echoes to send, one per second, capped at KODE_NET_PING_MAX.
 */
void net_ping_request(const char *host, uint8_t count);

/**
 * @brief Ask for an HTTP(S) GET of @p url.
 *
 * Answered with up to KODE_NET_HTTP_CHUNKS NET_HTTP_DATA_SIG events carrying
 * a *printable preview* of the body, then NET_HTTP_DONE_SIG with the status
 * and the real lengths. Something that wants the bytes themselves wants
 * kode_sys_http_get(), which puts them in its caller's buffer.
 */
void net_http_get_request(const char *url);

/**
 * @brief Ask for one UDP datagram. Answered with NET_UDP_DONE_SIG.
 *
 * @param len Cut to KODE_NET_UDP_MAX, because the payload rides in an event
 *        and events are bounded. An app with more to send calls
 *        kode_sys_udp_send(), which lends its own buffer to AO_Net's worker
 *        and has no such ceiling.
 */
void net_udp_send_request(const char *host, uint16_t port,
                          const void *data, uint16_t len);

/*
 * ---- The asks: kode_sys's requests, answered to the asking AO ----
 * The audio asks' contract, with AO_Net as the owner: the answer is posted to
 * @p who exactly once, one ask of each kind per asker, and the same three
 * returns. I/O runs on AO_Net's worker; a lent buffer is the asker's again at the
 * answer. No address: KODE_IO_ERROR; worker busy: KODE_DENIED. kode_sys checks
 * the arguments.
 */

/**
 * @brief Resolve @p host; answered with NET_LOOKUP_RSP_SIG.
 * @param who  The asking AO.
 * @param host Name or dotted quad, shorter than KODE_NET_HOST_MAX; copied.
 * @return As the asks above.
 */
kode_status_t net_lookup_ask(QActive *who, const char *host);

/**
 * @brief Ping @p host; answered with NET_PING_RSP_SIG.
 * @param who   The asking AO.
 * @param host  As net_lookup_ask().
 * @param count Echoes, 1..KODE_NET_PING_MAX.
 * @return As the asks above.
 */
kode_status_t net_ping_ask(QActive *who, const char *host, uint8_t count);

/**
 * @brief GET @p url, or POST @p body to it; answered with NET_HTTP_RSP_SIG.
 * @param who   The asking AO.
 * @param post  POST @p body; false GETs and ignores it.
 * @param url   Shorter than KODE_NET_URL_MAX; copied.
 * @param body  A POST's body, lent; NULL only when @p len is 0.
 * @param len   Its bytes.
 * @param ctype A POST's Content-Type, shorter than KODE_NET_CTYPE_MAX, copied; NULL for none.
 * @param buf   Where the answer's body goes, lent; NULL keeps none.
 * @param cap   Its room.
 * @return As the asks above.
 */
kode_status_t net_http_ask(QActive *who, bool post, const char *url, const void *body,
                           uint32_t len, const char *ctype, void *buf, uint32_t cap);

/**
 * @brief Send one datagram; answered with NET_UDP_RSP_SIG.
 * @param who  The asking AO.
 * @param host As net_lookup_ask().
 * @param port The port.
 * @param data Lent.
 * @param len  Its bytes.
 * @return As the asks above.
 */
kode_status_t net_udp_ask(QActive *who, const char *host, uint16_t port, const void *data,
                          uint16_t len);

/** @brief Ask AO_Net to publish NET_INFO_SIG now. Always answered. */
void net_info_request(void);

/** @brief Ask AO_Net to run its self-test. Answered with TEST_RESULT_SIG. */
void net_test_request(void);

/**
 * @brief Ask AO_Net to leave the Kode channel before a restart.
 *
 * AO_Boot's request. With the channel live AO_Net says "offline" and stops the
 * client cleanly (no last will); then one BOOT_NET_LEFT_SIG to AO_Boot. No new
 * session starts for the rest of the boot. A second request while one is owed is
 * dropped. Compiled under CONFIG_KODEOS_NET; guard the call the same way.
 */
void net_leave_request(void);

/**
 * @brief Tell AO_Net when the scheduler's `mqtt` entry next falls due.
 *
 * The radio wake that lets held messages in while the board sleeps. AO_Net keeps
 * one time, gives AO_LowPower the earlier of it and the channel's deadline, and
 * holds the wake KODE_NET_CHECK_DWELL_MS (an ASLEEP round meanwhile is "check
 * dwell"). Nothing to listen to: passed at once. Each check is told to AO_Scheduler
 * (sched_check_done()). CONFIG_KODEOS_NET only; acts only with CONFIG_KODE_LOWPOWER.
 *
 * @param due_wall_s Unix seconds (UTC) of the minute the check falls due; -1
 *                   for none (the entry is off, or the clock is not set).
 *                   Posts NET_CHECK_AT_SIG.
 * @return true when the note is on its way; false when no event was left or
 *         AO_Net's queue would not take it.
 */
bool net_check_at(int64_t due_wall_s);

/* ---- MQTT: AO_Net owns the client; the doors post with KQP_NET_CMD_MARGIN and answer false when full ---- */

/**
 * @brief Ask AO_Net to connect to @p uri (mqtt:// or mqtts://).
 *
 * @param uri  The broker.
 * @param user NULL or "" for an anonymous broker.
 * @param pass NULL or "" likewise.
 * @return true when the request was queued.
 */
bool mqtt_up_request(const char *uri, const char *user, const char *pass);

/**
 * @brief The event mqtt_up_request() posts, built for a caller that posts it itself.
 * @param uri  The broker.
 * @param user NULL or "" for an anonymous broker.
 * @param pass NULL or "" likewise.
 * @return The event, or NULL when the pool is dry.
 */
QEvt const *mqtt_up_evt(const char *uri, const char *user, const char *pass);

/**
 * @brief Ask AO_Net to stop the client. Answered with MQTT_STATE_SIG.
 *
 * @return true when the request was queued.
 */
bool mqtt_down_request(void);

/**
 * @brief The event mqtt_down_request() posts, for a caller that posts it itself.
 * @return A static event; never NULL.
 */
QEvt const *mqtt_down_evt(void);

/**
 * @brief Publish @p data to @p topic.
 *
 * Fire-and-forget, like appmgr_flash_request(): queued in the client's
 * outbox and reported in '!' lines. A payload longer than
 * KODE_MQTT_PAYLOAD_MAX is cut to it. AO_Net answers with MQTT_STATE_SIG:
 * no error when the client took it, KDRV_MQTT_ERR_REFUSED when no client is
 * up, KDRV_MQTT_ERR_NO_NET with no address, KDRV_MQTT_ERR_BUSY while the Kode
 * channel holds the client. mqtt_sub_request() and mqtt_unsub_request() are
 * answered the same way.
 *
 * @param topic The topic.
 * @param data  The payload.
 * @param len   Its length in bytes.
 * @return true when the request was queued.
 */
bool mqtt_pub_request(const char *topic, const void *data, uint16_t len);

/**
 * @brief The event mqtt_pub_request() posts, built for a caller that posts it itself.
 * @param topic The topic.
 * @param data  The payload.
 * @param len   Its length; cut to KODE_MQTT_PAYLOAD_MAX.
 * @return The event, or NULL when the pool is dry.
 */
QEvt const *mqtt_pub_evt(const char *topic, const void *data, uint16_t len);

/**
 * @brief Subscribe to @p topic at QoS 0.
 *
 * Messages arrive as MQTT_MSG_SIG on the bus, so receiving them means being
 * an Active Object. They are bounded and say when they were cut.
 *
 * @param topic The topic.
 * @return true when the request was queued.
 */
bool mqtt_sub_request(const char *topic);

/**
 * @brief The event mqtt_sub_request() posts, built for a caller that posts it itself.
 * @param topic The topic filter.
 * @return The event, or NULL when the pool is dry.
 */
QEvt const *mqtt_sub_evt(const char *topic);

/**
 * @brief Stop receiving @p topic.
 *
 * @param topic The topic.
 * @return true when the request was queued.
 */
bool mqtt_unsub_request(const char *topic);

/**
 * @brief The event mqtt_unsub_request() posts, built for a caller that posts it itself.
 * @param topic The topic filter.
 * @return The event, or NULL when the pool is dry.
 */
QEvt const *mqtt_unsub_evt(const char *topic);

/**
 * @brief How many MQTT_STATE_SIG answers AO_Net has published since boot.
 *
 * @param err Out: the last answer's kdrv_mqtt_err_t; NULL to skip.
 * @return The count; a change means a new answer. Any task.
 */
uint32_t mqtt_answers(uint8_t *err);

/** @brief Whether the Kode channel holds the MQTT client. Any task. */
bool mqtt_channel_holds(void);

/** @brief Ask AO_Net to publish MQTT_STATE_SIG now. Always answered. */
void mqtt_state_request(void);

/* ---- The Kode channel, run by AO_Net --------------------------------------
 * With Wi-Fi and a certificate on the C5, AO_Net joins the Kode channel by itself
 * and stays joined, unless the `chan` setting is off. This is a window onto it
 * (CONFIG_KODEOS_CHANNEL, which needs MQTT and the console).
 */

/** @brief Where the Kode channel stands (ChannelStateEvt.state). */
typedef enum {
    CHAN_OFF = 0,       /**< Not part of this firmware, or `chan` is off.   */
    CHAN_NO_NET,        /**< No address: Wi-Fi is off or not joined.        */
    CHAN_NO_IDENTITY,   /**< The C5 holds no certificate. Terminal.         */
    CHAN_JOINING,       /**< Challenge, signature, connect, broker.         */
    CHAN_UP,            /**< Connected: commands may arrive.                */
    CHAN_WAITING,       /**< A try failed; the next one is on a timer.      */
    CHAN_REFUSED        /**< The api refused the certificate. Terminal.     */
} chan_state_t;

/** @brief What the broker did with the channel's session (ChannelStateEvt.session). */
/* cppcheck-suppress misra-c2012-2.4 ; the type names the values of a uint8_t event field, which keeps ChannelStateEvt small */
typedef enum {
    CHAN_SESSION_NONE = 0,  /**< Not up: no connect to speak of.            */
    CHAN_SESSION_NEW,       /**< The broker held nothing for this Kode.     */
    CHAN_SESSION_RESUMED    /**< The broker resumed the session it kept.    */
/* cppcheck-suppress misra-c2012-2.3 ; the type names the values of a uint8_t event field, which keeps ChannelStateEvt small */
} chan_session_t;

/**
 * @brief Ask AO_Net to publish CHANNEL_STATE_SIG now. Always answered, with
 *        CHAN_OFF in a firmware built without the channel.
 *
 * A post like the other requests here: a full queue is a reset, not a false.
 *
 * @return true; the form is the facade's, where a request can be refused.
 */
bool channel_state_request(void);

/**
 * @brief The console has answered the line AO_Net gave it from the Kode
 *        channel's cmd/in (`>ok` or `>err`): the next one may go. Posts
 *        NET_LINE_DONE_SIG.
 *
 * AO_Console calls it once per NET line at its terminator, with the line's
 * ConsoleLineEvt::seq. A sequence that is not the line out runs nothing, so a verb
 * ending after KODE_CHAN_LINE_BOUND_MS cannot end its successor. A failed post is
 * logged. Safe from any task. CONFIG_KODEOS_CHANNEL only; guard the call the same way.
 *
 * @param seq The sequence of the line that ended.
 */
void net_line_done(uint8_t seq);

/**
 * @brief The console was busy when AO_Net's channel line arrived: it was not
 *        taken, and AO_Net offers it again shortly. Posts NET_LINE_BUSY_SIG.
 *
 * AO_Console calls it instead of refusing a NET line as busy. Safe from any task.
 * CONFIG_KODEOS_CHANNEL only; guard the call the same way.
 */
void net_line_busy(void);

/* ---- Screen uploads, over the Kode channel's session ---------------------
 * The connect answer names an HTTPS prefix, `shots`; an order names a token.
 * AO_Net PUTs the glass as JPEG to `shots` + token with the channel's JWT
 * (CONFIG_KODEOS_SHOT_HTTP). KSHOT_TOKEN_MAX and KSHOT_ID_MAX are in kqp_config.h.
 */
#define KSHOT_FPS_MAX       10U   /**< Most frames a second a stream asks for. */
#define KSHOT_FPS_DEFAULT    2U   /**< A stream's rate when the order names none. */
#define KSHOT_SECS_MAX     300U   /**< Longest stream, in seconds.             */
#define KSHOT_SECS_DEFAULT  60U   /**< A stream's length when the order names none. */
#define KSHOT_QUALITY       80U   /**< JPEG quality of every upload.           */

/** @brief How an upload ended (NetShotRspEvt.result). */
typedef enum {
    KSHOT_R_OK = 0,      /**< The server took the capture (201).            */
    KSHOT_R_REFUSED,     /**< Any other answer, `http` says which; no
                          *   redirect is followed.                          */
    KSHOT_R_NO_ANSWER,   /**< No answer: TLS, socket or timeout.            */
    KSHOT_R_BUSY,        /**< The codec or the network worker was taken, or
                          *   an upload is already under way.                */
    KSHOT_R_SIZE,        /**< The glass encodes past KSHOT_JPEG_CAP.        */
    KSHOT_R_CHANNEL,     /**< Channel down, no `shots` link, or link and
                          *   token too long for one URL.                    */
    KSHOT_R_STREAM,      /**< A stream was accepted and runs on its own.     */
    KSHOT_R_STOPPED,     /**< A stop order ended the running stream.         */
    KSHOT_R_NO_STREAM    /**< A stop order found no stream running.          */
} kshot_result_t;

/** @brief Which upload runs now (ChannelStateEvt.shot). */
/* cppcheck-suppress misra-c2012-2.4 ; the type names the values of a uint8_t event field, which keeps ChannelStateEvt small */
typedef enum {
    KSHOT_IDLE = 0,      /**< None.                                          */
    KSHOT_ONE,           /**< A capture, until it is answered.               */
    KSHOT_STREAM         /**< A stream, until it ends.                       */
/* cppcheck-suppress misra-c2012-2.3 ; the type names the values of a uint8_t event field, which keeps ChannelStateEvt small */
} kshot_mode_t;

/**
 * @brief Whether @p token may name an upload.
 *
 * 1 to KSHOT_TOKEN_MAX characters of `[A-Za-z0-9_-]`: nothing that could
 * leave the link's last path segment or start a query.
 *
 * @param token The token; NULL is refused.
 * @return true when the token is acceptable.
 */
bool kshot_token_valid(const char *token);

/**
 * @brief Ask AO_Net to upload the glass to the Kode channel's `shots` link.
 *
 * Answered with one NET_SHOT_RSP_SIG (NetShotRspEvt) to @p who. The token is
 * copied and never logged. A full pool drops the request with an error line.
 *
 * @param who   The AO the answer goes to.
 * @param token The upload token (kshot_token_valid()).
 * @param fps   Frames a second; 0 with @p secs 0 asks for one capture.
 * @param secs  How long a stream runs, in seconds.
 */
void net_shot_request(QActive *who, const char *token, uint8_t fps,
                      uint16_t secs);

/**
 * @brief Ask AO_Net to end the running stream of uploads (`stopped`).
 *
 * Answered with one NET_SHOT_RSP_SIG to @p who: KSHOT_R_STOPPED or
 * KSHOT_R_NO_STREAM. A full pool drops the request with an error line.
 *
 * @param who The AO the answer goes to.
 */
void net_shot_stop_request(QActive *who);

/* ---- AO_Storage: owner of the microSD and the internal FAT --------------
 * The only code in kodeOS that opens a file. Listing is published on the bus
 * (STORAGE_ENTRY_SIG then STORAGE_LIST_END_SIG) so each subscriber ends up
 * with its own copy; reading is a loan of one of this AO's own buffers.
 */
extern QActive * const AO_Storage;  /**< The media owner AO. */
void Storage_ctor(void);            /**< Construct it; kaos_storage_start() calls it. */
/**
 * @brief Construct AO_Storage and start it at KODE_PRIO_STORAGE, on its own queue
 *        and stack.
 */
void kaos_storage_start(void);
/** @brief Register its self-test rows; kaos_storage_start() calls it. */
void kaos_storage_register(void);

/** @brief What to ask a medium to do (STORAGE_CMD_SIG). */
typedef enum {
    STORAGE_OP_MOUNT = 0,
    STORAGE_OP_UNMOUNT,
    STORAGE_OP_INFO,
    STORAGE_OP_LIST,       /**< Publish this medium's entries.               */
    STORAGE_OP_SELFTEST,   /**< Answers with TEST_RESULT_SIG: one scratch file
                            *   internally; on the card the production row,
                            *   which judges the slot and leaves the card.   */
    STORAGE_OP_VERIFY,     /**< The card: real capacity, full-size filesystem.
                            *   DESTRUCTIVE: writes the far end, then formats.
                            *   Answers with TEST_RESULT_SIG `sd_verify`.     */
} storage_op_t;

/**
 * @brief What to do to a path (STORAGE_PATH_REQ_SIG).
 *
 * Separate from storage_op_t, which is about a whole volume.
 */
typedef enum {
    STORAGE_PATH_STAT = 0,  /**< Answers with StoragePathRspEvt.            */
    STORAGE_PATH_LIST,      /**< Answers as entries then STORAGE_LIST_END.  */
    STORAGE_PATH_CAT,       /**< Answers as StorageLineEvt records then its
                             *   own terminator. Only via storage_cat_request()
                             *   (it carries a `max`).                       */
    STORAGE_PATH_MKDIR,     /**< Make one directory; the parent must exist
                             *   (no `-p`). Answers with StoragePathRspEvt.  */
    STORAGE_PATH_UNLINK,    /**< Delete a file. Files only: kdrv_storage_unlink()
                             *   refuses a directory, which remove() may not. */
    STORAGE_PATH_RMDIR,     /**< Remove an empty directory; no recursive form.
                             *   Answers with StoragePathRspEvt.             */
    STORAGE_PATH_WRITE,     /**< Not a request op: `write` travels as
                             *   StorageTextEvt. Names the `op` of its
                             *   StoragePathRspEvt answer.                   */
    STORAGE_PATH_WRITE_LINE, /**< As STORAGE_PATH_WRITE, for `line=<n>`: its
                             *   own failures (line past the end, file over
                             *   32 KB) must be told apart by path_answer(). */
    STORAGE_PATH_DELETE,    /**< UNLINK or RMDIR, whichever the path is, in
                             *   one step: storage_delete_ask()'s. Answered
                             *   with StoragePathRspEvt's shape.             */
    STORAGE_PATH_SPACE,     /**< How big the volume at the path is and how much
                             *   of it is free; the path is the volume's root.
                             *   Answered to storage_space_ask() and to a
                             *   blocked task's waiter only: a request that
                             *   names neither is refused with
                             *   StoragePathRspEvt's KODE_DENIED.            */
} storage_path_op_t;

/** @brief Ask AO_Storage to do @p op to @p medium. */
void storage_request(kdrv_medium_t medium, storage_op_t op);

/**
 * @brief The same, and tell @p who when the command has run.
 *
 * A card mount runs on AO_Storage's worker, so the post returning says nothing.
 * STORAGE_CMD_RSP_SIG, posted to @p who once per accepted request, does: when the
 * call returns or its deadline passes.
 *
 * @param medium The medium.
 * @param op     What to do to it.
 * @param who    Who is waiting; not NULL.
 * @return true when the request is with AO_Storage and the answer will come;
 *         false when no event was left to carry it, and none will.
 */
bool storage_request_answered(kdrv_medium_t medium, storage_op_t op,
                              QActive *who);

/**
 * @brief What to do to a pair of paths (STORAGE_PAIR_REQ_SIG).
 *
 * Answered like the path ops, with StoragePathRspEvt, but it takes two paths,
 * hence its own event (StoragePairEvt) and enum.
 */
typedef enum {
    STORAGE_PAIR_MOVE = 0,  /**< Rename @p src to @p dst. Same volume only:
                             *   a crossing is KODE_DENIED, untouched; use COPY. */
    STORAGE_PAIR_COPY,      /**< Copy @p src to @p dst across either volume, one
                             *   32 KB chunk per step paced by a QTimeEvt, so
                             *   lower AOs keep running. Answered once at the
                             *   end; a failed copy leaves no destination.   */
} storage_pair_op_t;

/**
 * @brief Ask AO_Storage about one path: STAT, LIST, MKDIR, UNLINK or RMDIR it.
 *
 * Every request is answered, refusals included: the console closes on it.
 * UNLINK and RMDIR, and the delete that tries either, wait for the transfer
 * AO_Storage has in flight to end, KODE_STORAGE_TURN_MS at most, and are refused
 * with KODE_DENIED after it; one request waits at a time, and a second is refused
 * at once.
 *
 * @param who  Where the response is posted. NULL is served and not answered.
 * @param op   storage_path_op_t, any but STORAGE_PATH_CAT (storage_cat_request()).
 * @param path Absolute, normalised, virtual. Copied into the event.
 */
void storage_path_request(QActive *who, uint8_t op, const char *path);

/**
 * @brief Ask AO_Storage to move or copy @p src to @p dst.
 *
 * Answered once with StoragePathRspEvt; `rc` says how it went. On KODE_ERROR,
 * `exists` true means the source was checked afterwards and is missing.
 * MOVE waits for a transfer in flight like storage_path_request()'s deletes.
 * MOVE: KODE_DENIED malformed, no volume, two volumes, or a transfer that outlasted
 * the wait; KODE_ERROR no mount or rename failed. COPY: KODE_DENIED malformed, no volume, same file, or a transfer in
 * flight; KODE_ERROR otherwise. COPY answers at its end: size deadlines for it.
 *
 * @param who Where the response is posted. NULL is served and not answered for
 *            MOVE; refused for COPY, whose only sign of life is @p who.
 * @param op  storage_pair_op_t: STORAGE_PAIR_MOVE or STORAGE_PAIR_COPY.
 * @param src Absolute, normalised, virtual. Copied into the event.
 * @param dst Absolute, normalised, virtual. Copied into the event.
 */
void storage_pair_request(QActive *who, uint8_t op, const char *src,
                          const char *dst);

/**
 * @brief Ask AO_Storage to put @p text in @p path, or to replace one line
 *        of it.
 *
 * @p line 0: whole-file write, truncating unless @p append; answered with
 * StoragePathRspEvt tagged STORAGE_PATH_WRITE. @p line n: replaces line n (1-based)
 * via a temporary renamed over the original, so a failure leaves it untouched; the
 * file must fit a 32 KB buffer (else KODE_NO_RESOURCES); tagged
 * STORAGE_PATH_WRITE_LINE so path_answer() can tell the failures apart. Either waits
 * for a transfer in flight like storage_path_request()'s deletes.
 *
 * @param who    Where the response is posted. NULL is served and not
 *               answered.
 * @param path   Absolute, normalised, virtual. Copied into the event.
 * @param text   The bytes to write, already unescaped. Copied, cut to
 *               KODE_FS_TEXT_MAX-1 bytes if longer (not a refusal).
 * @param append Keep what is there and add after it. Otherwise truncate.
 *               Ignored when @p line is nonzero.
 * @param line   0 replaces the whole file; n replaces line n of it.
 */
void storage_text_request(QActive *who, const char *path, const char *text,
                          bool append, uint16_t line);

/**
 * @brief Ask AO_Storage to `cat` one path.
 *
 * console_fs_cat_begin() calls it after its STAT said @p path is a file, so a
 * refusal means that changed, or another transfer is in flight (KODE_DENIED).
 *
 * @param who  Where STORAGE_LINE_SIG lands, records and terminator alike.
 *             NULL is served and not answered.
 * @param path Absolute, normalised, virtual. Copied into the event.
 * @param max  Ceiling on bytes read; the console bounds it to KODE_FS_CAT_MAX_CEIL
 *             and this clips again only to its own buffer.
 */
void storage_cat_request(QActive *who, const char *path, uint32_t max);

/* ---- The same four requests, asked by a task instead of an Active Object ---
 * For the `kode_sys_file_*()` facade, which runs on an app's task with no queue:
 * the answer goes into a StorageWaiter (kqp_signals.h) and its semaphore is
 * signalled. Each is answered exactly once, refusals included; a dry pool writes
 * KODE_NO_RESOURCES straight into the waiter. @p wait must outlive the request:
 * it may still be queued after a caller's deadline, so not a stack frame.
 */

/**
 * @brief storage_path_request(), answered into @p wait.
 *
 * LIST is answered into @p wait->rows and published nowhere.
 *
 * @param wait Where the answer goes, and the semaphore to signal. Not NULL.
 * @param gen  This request's tag, echoed into @p wait->ack just before the
 *             signal, to tell its own answer from an abandoned one.
 * @param op   storage_path_op_t, any member but STORAGE_PATH_CAT.
 * @param path Absolute, normalised, virtual. Copied into the event.
 */
void storage_path_request_wait(StorageWaiter *wait, uint32_t gen, uint8_t op,
                               const char *path);

/**
 * @brief A STORAGE_PATH_LIST answered into @p wait, from the @p skip-th entry on.
 *
 * storage_path_request_wait() with STORAGE_PATH_LIST is this with a @p skip
 * of 0. The entries left out are counted as the walk finds them, after the
 * names it never lists, so a page starts where the one before it ended.
 *
 * @param wait See storage_path_request_wait().
 * @param gen  See storage_path_request_wait().
 * @param path Absolute, normalised, virtual. Copied into the event.
 * @param skip Entries to leave out: the ones earlier pages gave.
 */
void storage_list_request_wait(StorageWaiter *wait, uint32_t gen, const char *path,
                               uint32_t skip);

/**
 * @brief storage_cat_request(), answered into @p wait.
 *
 * The bytes land in @p wait->text verbatim: not split on newlines, not cut at
 * KODE_FS_CAT_LINE_MAX, NULs kept. A file is bytes, not the text wire.
 *
 * @param wait Where the answer goes. @c text and @c cap must be set.
 * @param gen  See storage_path_request_wait().
 * @param path Absolute, normalised, virtual. Copied into the event.
 * @param offset First byte to read; at or past the end reads nothing.
 * @param max  Ceiling on bytes read from the file, counted from @p offset.
 */
void storage_cat_request_wait(StorageWaiter *wait, uint32_t gen,
                              const char *path, uint32_t offset, uint32_t max);

/**
 * @brief storage_pair_request()'s MOVE, answered into @p wait.
 *
 * COPY is refused with KODE_DENIED: AO_Storage tracks a copy by the `who` it
 * answers, so a task's copy could never be seen running.
 *
 * @param wait Where the answer goes. Not NULL.
 * @param gen  See storage_path_request_wait().
 * @param op   storage_pair_op_t. STORAGE_PAIR_MOVE is served; COPY is
 *             refused, and told so rather than left unanswered.
 * @param src  Absolute, normalised, virtual. Copied into the event.
 * @param dst  Absolute, normalised, virtual. Copied into the event.
 */
void storage_pair_request_wait(StorageWaiter *wait, uint32_t gen, uint8_t op,
                               const char *src, const char *dst);

/**
 * @brief storage_text_request()'s whole-file write, answered into @p wait
 *        and carrying its bytes by pointer rather than in the event.
 *
 * A counted buffer, so no KODE_FS_TEXT_MAX ceiling and NULs are written as bytes.
 * No @p line form: replacing a line is an operator's edit.
 *
 * @param wait   Where the answer goes. Not NULL.
 * @param gen    See storage_path_request_wait().
 * @param path   Absolute, normalised, virtual. Copied into the event.
 * @param data   The bytes to write. NOT copied: must outlive the request.
 *               NULL only when @p len is zero.
 * @param len    How many bytes at @p data.
 * @param append Keep what is there and add after it. Otherwise truncate.
 */
void storage_text_request_wait(StorageWaiter *wait, uint32_t gen,
                               const char *path, const void *data,
                               uint32_t len, bool append);

/*
 * ---- The asks: kode_sys's requests, answered to the asking AO ----
 * The audio asks' contract, with AO_Storage as the owner: the answer is posted to
 * @p who exactly once, one ask of each kind per asker, and the same three
 * returns. A lent buffer is the asker's again once answered. Paths are copied,
 * shorter than KODE_PATH_MAX; kode_sys checks the arguments.
 */

/**
 * @brief Ask what is at @p path; answered with FILE_STATUS_RSP_SIG.
 * @param who  The asking AO.
 * @param path Absolute, virtual.
 * @return As the asks above.
 */
kode_status_t storage_stat_ask(QActive *who, const char *path);

/**
 * @brief List a page of @p path: up to KODE_STORAGE_LIST_MAX STORAGE_ENTRY_SIG, then
 *        FILE_LIST_END_SIG.
 *
 * Each entry leaves KODE_STORAGE_LIST_FREE_SLOTS free in @p who's queue; no room
 * ends the page early with @c more set, or KODE_NO_RESOURCES if nothing fit.
 * @param who  The asking AO.
 * @param path A directory, absolute and virtual.
 * @param skip Entries earlier pages sent.
 * @return As the asks above.
 */
kode_status_t storage_list_ask(QActive *who, const char *path, uint16_t skip);

/**
 * @brief Make one directory; answered with FILE_FOLDER_RSP_SIG.
 * @param who  The asking AO.
 * @param path Its parent must exist.
 * @return As the asks above.
 */
kode_status_t storage_mkdir_ask(QActive *who, const char *path);

/**
 * @brief Delete a file or an empty directory; answered with FILE_DELETE_RSP_SIG.
 * @param who  The asking AO.
 * @param path Absolute, virtual.
 * @return As the asks above.
 */
kode_status_t storage_delete_ask(QActive *who, const char *path);

/**
 * @brief Rename @p from to @p to on one volume; answered with FILE_RENAME_RSP_SIG.
 * @param who  The asking AO.
 * @param from Absolute, virtual.
 * @param to   On the same volume.
 * @return As the asks above.
 */
kode_status_t storage_rename_ask(QActive *who, const char *from, const char *to);

/**
 * @brief Read up to @p cap bytes from @p offset into @p buf; answered with FILE_READ_RSP_SIG.
 * @param who    The asking AO.
 * @param path   A file, absolute and virtual.
 * @param offset The first byte; at or past the end reads nothing.
 * @param buf    Lent: written in place before the answer, never after.
 * @param cap    1..KODE_FS_CAT_MAX_CEIL.
 * @return As the asks above.
 */
kode_status_t storage_read_ask(QActive *who, const char *path, uint32_t offset, void *buf,
                               uint32_t cap);

/**
 * @brief Write @p len bytes of @p data to @p path; answered with FILE_WRITE_RSP_SIG.
 * @param who    The asking AO.
 * @param path   Absolute, virtual.
 * @param data   Lent: read in place before the answer.
 * @param len    0..UINT16_MAX.
 * @param append Keep what is there and add after it; otherwise truncate.
 * @return As the asks above.
 */
kode_status_t storage_write_ask(QActive *who, const char *path, const void *data, uint32_t len,
                                bool append);

/**
 * @brief Ask how big the volume at @p path is and how much of it is free;
 *        answered with FILE_SPACE_RSP_SIG.
 *
 * The answer's @c rc is KODE_DENIED when @p path is not a volume's root,
 * KODE_ERROR when the volume will not mount or cannot be counted, and its
 * @c space is zero then.
 * @param who  The asking AO.
 * @param path A volume's root, absolute and virtual: "/sdcard" or "/storage".
 * @return As the asks above.
 */
kode_status_t storage_space_ask(QActive *who, const char *path);

/**
 * @brief Ask AO_Storage to receive a file arriving on the console.
 *
 * `file_put`: AO_Log hands over exactly @p size raw bytes, written to @p path
 * (truncating). Answered by RECV_READY_SIG, RECV_ACK_SIG per KODE_RECV_CHUNK, then
 * one of RECV_DONE_SIG / RECV_FAIL_SIG / RECV_STALL_SIG, POSTED to @p who; no
 * APP_FLASH_* reporting. A transfer already in flight is waited for,
 * KODE_STORAGE_TURN_MS at most; past it, and for a second request while one
 * waits, the answer is RECV_BUSY_SIG and nothing was armed. Any failure removes
 * the file, so `app_run` never boots a stub. Designed and verified on the cable;
 * over BLE it is untried, not refused.
 *
 * @param who   Where the RECV_* conversation is posted. NULL is refused: it is
 *              AO_Storage's only sign the transfer is alive.
 * @param path  Absolute, normalised, virtual. Copied into the event.
 * @param size  Payload length in bytes. Zero is refused.
 * @param crc32 What khal_crc32() over the whole payload must equal.
 * @param src   KODE_CONSOLE_SRC_* of the port the payload follows on.
 * @return false if the request event could not be allocated: no RECV_* signal is
 *         coming, and the caller must close its own command.
 */
bool storage_recv_request(QActive *who, const char *path, uint32_t size,
                          uint32_t crc32, uint8_t src);

/**
 * @brief Read, write or dump one field of the microSD card.
 *
 * The console's `sd`. @p field is a resolved kdrv_sd_field_t, so a misspelling is
 * refused before the card is asked. Every request is answered, refusals included.
 *
 * @param who   Where STORAGE_FIELD_RSP_SIG is posted. NULL is served and not
 *              answered.
 * @param op    FIELD_OP_GET, _SET or _DUMP.
 * @param field kdrv_sd_field_t.
 * @param value For FIELD_OP_SET, in the field's own unit.
 */
void storage_field_request(QActive *who, uint8_t op, uint16_t field,
                           int32_t value);

/**
 * @brief Ask for one medium's entries.
 *
 * @param images_only Only bootable files (.bin): `app_list` yes, `sd_ls` no.
 * @param last        Publish STORAGE_LIST_END_SIG when this medium is done.
 */
void storage_list_one(kdrv_medium_t medium, bool images_only, bool last);

/**
 * @brief Ask for the bootable images on every medium, card first: remount the
 *        card, then the first page (storage_list_page(0, who)).
 * @param who The asker; the pages are posted to it alone.
 */
void storage_list_request(QActive *who);

/**
 * @brief Ask for one page of the bootable images on every medium.
 *
 * One listing, card first: from position @p skip, at most KODE_STORAGE_LIST_MAX
 * STORAGE_ENTRY_SIG (with `pos`), then STORAGE_LIST_END_SIG (`more`), posted to
 * @p who alone. Nothing is remounted, so the order holds across pages.
 *
 * @param skip Images to leave out: the ones earlier pages sent.
 * @param who  The asker.
 */
void storage_list_page(uint16_t skip, QActive *who);

/**
 * @brief Ask AO_Storage to open @p name for reading.
 *
 * A bare name is searched card first, then internal; a name starting with '/' is
 * an absolute virtual path on the volume it names. One open file system-wide: a
 * second is refused. Answered to @p who as STORAGE_OPEN_RSP_SIG.
 */
void storage_open_request(const char *name, QActive *who);

/**
 * @brief Ask what one file says it is, without opening it for reading.
 *
 * Open, read the first KAPP_PREFIX_LEN bytes, parse and close within one step.
 * It does not take the reader slot, so it can never strand the one open file.
 *
 * A read that meets a transfer in flight waits for it, in the same line as the
 * reads of appdata_request() and by the same rules, and is answered with a
 * failed status only when that wait is full or has run out.
 *
 * @param name The file, as a listing named it.
 * @param who  Where STORAGE_DESC_RSP_SIG is posted. Never NULL.
 */
void storage_desc_request(const char *name, QActive *who);

/**
 * @brief Ask AO_Storage what the app data folder says about @p project.
 *
 * Posted to @p who: APPDATA_FIELDS_SIG once, APPDATA_ICON_SIG zero or
 * KODE_APP_ICON_PIECES times (the icon, always the 128x128 block, whichever
 * size the file has), APPDATA_END_SIG once. No reader slot taken. The folder
 * is on the first medium, the internal one before the card, that holds its
 * app.json: a folder with no app.json is no data, and does not hide a folder
 * on the other medium that has one.
 * A read that meets a transfer in flight waits for it, with up to
 * KODE_STORAGE_READ_WAIT_DEPTH others and in arrival order, behind a write or
 * delete that waits; it is answered when the transfer ends. One that waits
 * KODE_STORAGE_TURN_MS from the first, or finds the wait full, is refused
 * (medium=none, json_status=none). An icon whose pieces the icon pool cannot
 * all take, because the last icon is still with its reader, is tried again every
 * KODE_STORAGE_ICON_RETRY_MS within the same bound, and then answered
 * icon_status=busy with the medium and the json it found; no event at all gives
 * icon_status=busy and an empty project.
 * Fields and end always arrive. See https://docs.kode.diy/sdk/kdrv/apps.
 *
 * @param project The app's project name, as the binary declares it.
 * @param who     Where the three signals are posted. Never NULL.
 */
void appdata_request(const char *project, QActive *who);

/**
 * @brief Delete @p project's data folder, from the medium that holds it.
 *
 * The one recursive delete, bounded to `<root>/.kode/appsdata/<project>` and one
 * subfolder level (https://docs.kode.diy/sdk/kdrv/apps). The internal partition's
 * folder is the one removed; a card folder of that name is then left where it is
 * and logged. A folder only on the card, where appdata_put_request() wrote it
 * for want of room inside, is removed from the card. Always answered once posted, refusals
 * included, with an AppDataRmRspEvt naming @p project. It waits for a transfer in
 * flight like storage_path_request()'s deletes; a transfer that outlasts the wait
 * is KODE_DENIED.
 *
 * @param project The app's project name, as the binary declares it.
 * @param who     Where APPDATA_RM_RSP_SIG is posted. Never NULL.
 * @return KODE_OK when the request is posted and an answer is owed;
 *         KODE_NO_RESOURCES when the event pool is dry: nothing was posted and
 *         no answer will come.
 */
kode_status_t appdata_rm_request(const char *project, QActive *who);

/**
 * @brief Take every file of @p project off both media.
 *
 * The images at the root of the internal partition and of the card whose
 * descriptor names @p project, whatever the files are called, and the app's
 * data folder on each. `kodeos.bin` and `c5_slave.bin` are never opened. One
 * file a step, so the walk never holds AO_Storage. One asked while a transfer
 * is in flight waits for it, KODE_STORAGE_TURN_MS at most, and is refused with
 * KODE_DENIED after it. Always answered once posted, with a
 * StorageAppWipeRspEvt naming @p project; an absent card is said there, and is
 * no failure.
 *
 * @param project The app's project name, as the binary declares it.
 * @param who     Where STORAGE_APP_WIPE_RSP_SIG is posted. Never NULL.
 * @return KODE_OK when the request is posted and an answer is owed;
 *         KODE_NO_RESOURCES when the event pool is dry: nothing was posted and
 *         no answer will come.
 */
kode_status_t storage_app_wipe_request(const char *project, QActive *who);

/** @brief Write one app's data folder (APPDATA_PUT_REQ_SIG). */
typedef struct {
    QEvt            super;
    char            project[KDRV_SLOT_PROJECT_MAX]; /**< The app's project name. */
    uint8_t        *buf;       /**< json_len bytes of app.json, then icon_len of
                                *   icon.k64. AO_Storage owns it from the post
                                *   and khal_free()s it on every path.          */
    uint16_t        json_len;  /**< Bytes of app.json, 1..KAPP_DATA_JSON_MAX.     */
    uint16_t        icon_len;  /**< Bytes of icon.k64 after it: 0, KAPP_ICON_BYTES or
                                *   KAPP_ICON_HALF_BYTES.                           */
    QActive        *who;       /**< Where APPDATA_PUT_RSP_SIG is posted.          */
} AppDataPutEvt;

/** @brief How a put of one app's data folder ended (APPDATA_PUT_RSP_SIG). */
typedef struct {
    QEvt          super;
    char          project[KDRV_SLOT_PROJECT_MAX]; /**< Echoed: answers can cross. */
    kode_status_t rc;      /**< KODE_OK, KODE_DENIED, KODE_ERROR, KODE_IO_ERROR,
                            *   or KODE_NO_RESOURCES: no room for the put.     */
    bool          busy;    /**< With KODE_DENIED: the put was good but AO_Storage
                            *   had no turn for it within KODE_STORAGE_TURN_MS,
                            *   so nothing was written and the same put may be
                            *   asked again. False for every other answer.   */
} AppDataPutRspEvt;

/**
 * @brief Write @p project's data folder, replacing whatever folder was there
 *        whole, or not at all.
 *
 * A folder that is there is replaced on the medium that holds its app.json, the
 * internal partition before the card as the reader chooses, and never moved. A
 * new folder goes inside; when the internal partition has no room for it and a
 * card is mounted that has, it goes to the card, where the reader finds it, and
 * stays there. A card that will not take the write leaves nothing behind and the
 * answer is KODE_NO_RESOURCES, as with no card.
 *
 * Makes missing parents, writes `app.json` and, if @p icon_len, `icon.k64` in two
 * steps under temporary names beside the old files, and only when both are down
 * puts them in the old files' place and removes whatever else the folder held. A
 * put that fails, or never finishes, leaves the folder that was there as it was;
 * one it made is removed. Once posted, AO_Storage owns @p buf and
 * frees it on every path. A put that meets a transfer in flight waits for it
 * (KODE_STORAGE_TURN_MS). Always answered with an AppDataPutRspEvt: KODE_OK;
 * KODE_DENIED bad name, length, NULL buffer or busy (AppDataPutRspEvt.busy tells
 * the last apart); KODE_ERROR not mounted; KODE_IO_ERROR; KODE_NO_RESOURCES when
 * the put does not fit. It is charged what it weighs on the volume: its app.json
 * and its icon, each rounded up to the volume's cluster (kdrv_storage_cluster_bytes()).
 * A new folder is charged one cluster more, its own, and on the internal partition
 * must leave KODE_APPDATA_FREE_MIN_BYTES free besides; a folder that is there is
 * charged only that pair, the room of the temporaries it is written under beside
 * the old files. The card keeps no floor: it refuses only when it says it has less
 * free than the charge. The refusal is told in the log with the numbers.
 *
 * @param project  The app's project name, as the binary declares it.
 * @param buf      app.json then icon.k64, one allocation from khal_alloc_psram()
 *                 or khal_realloc_psram() (what kfw_appdata_take() gives).
 * @param json_len Bytes of app.json, 1 to KAPP_DATA_JSON_MAX.
 * @param icon_len Bytes of icon.k64 after it: 0 for none, else KAPP_ICON_BYTES or
 *                 KAPP_ICON_HALF_BYTES; the file is written as it is.
 * @param who      Where APPDATA_PUT_RSP_SIG is posted. Never NULL.
 * @return KODE_OK when the request is posted: @p buf is AO_Storage's and an
 *         answer is owed. KODE_NO_RESOURCES when the event pool is dry:
 *         nothing was posted, @p buf is still the caller's to free, and no
 *         answer will come.
 */
kode_status_t appdata_put_request(const char *project, uint8_t *buf,
                                  uint16_t json_len, uint16_t icon_len,
                                  QActive *who);

/** @brief How long AO_AppMgr waits for AO_Storage's answer to one data-folder
 *         request of an install, in ms: a put of 13 KB onto the FAT, or one
 *         recursive delete. */
#define APPMGR_TIDY_MS      (5000U)

/** @brief How much longer an install that ends in a boot waits, in ms, for the
 *         answer to the last data-folder request that went unanswered for
 *         APPMGR_TIDY_MS: the board resets right after, and a write AO_Storage
 *         is still making would be cut. */
#define APPMGR_TIDY_LATE_MS (5000U)

/**
 * @brief The folder's path on @p m, or false when the name cannot be a folder.
 *
 * @param out     Where the path goes.
 * @param cap     Size of @p out; 56 bytes hold the longest folder there is.
 * @param m       Which medium's tree.
 * @param project The app's project name.
 * @return true when @p out holds a path worth asking about.
 */
bool appdata_folder_path(char *out, size_t cap, AppDataMedium m,
                         const char *project);

/** @brief Ask for a chunk. The answer is STORAGE_DATA_SIG. */
void storage_read_request(void);

/** @brief Hand buffer @p idx back and ask for the next chunk in it. */
void storage_next_request(uint8_t idx);

/** @brief Close the open file and release any buffers still on loan. */
void storage_close_request(void);

/**
 * @brief Write @p len bytes of @p data to a file called @p name.
 *
 * One call, since the caller already holds the whole buffer: @p data must stay
 * valid and unchanged until the answer. AO_Storage chunks the write with self-posts.
 * A bare name goes internal first, then the card; a name starting with '/' goes
 * to the volume it names only.
 *
 * @param who Receives STORAGE_WRITE_RSP_SIG when it is over, either way.
 */
void storage_write_all_request(const char *name, const void *data,
                               uint32_t len, QActive *who);

/**
 * @brief The buffer a STORAGE_DATA_SIG is talking about.
 *
 * Valid only between receiving that event and handing the index back with
 * storage_next_request(). Read it, do not keep it.
 */
const uint8_t *storage_buf(uint8_t idx);

/* ---- A URL as a medium ---------------------------------------------------
 * storage_open_request()'s conversation with a URL for the name; the response,
 * data events and loan are identical.
 */

/** @brief Open @p url for reading; the answer goes to @p who. */
void net_stream_open_request(const char *url, QActive *who);

/** @brief Ask for the next chunk (NET_STREAM_DATA_SIG). */
void net_stream_read_request(void);

/** @brief Hand a borrowed buffer back so it can be refilled. */
void net_stream_return(uint8_t idx);

/** @brief Close the transfer. */
void net_stream_close_request(void);

/** @brief The bytes of a borrowed stream buffer. */
const uint8_t *net_stream_buf(uint8_t idx);

/* ---- AO_AppMgr: owner of the apps region --------------------------------
 * Copies a .bin from a medium into the apps region (evicting the oldest install
 * when short of room), lists, launches and removes apps by name. Which firmware
 * boots is AO_Boot's; the two write different flash regions.
 */
extern QActive * const AO_AppMgr;   /**< The apps-region AO. */
void AppMgr_ctor(void);             /**< Construct it; kaos_appmgr_start() calls it. */
/**
 * @brief Construct AO_AppMgr and start it at KODE_PRIO_APPMGR, on its own queue and
 *        stack.
 */
void kaos_appmgr_start(void);
/** @brief Register its console verbs; kaos_appmgr_start() calls it. */
void kaos_appmgr_register(void);

/**
 * @brief Ask AO_AppMgr to write "<medium>/<file>" into the apps region.
 *
 * Fire-and-forget: posts FLASH_REQ_SIG. Progress is logged; the outcome is published
 * as APP_FLASH_DONE_SIG (AppFlashDoneEvt naming the app, "" if unreadable or the pool
 * was dry) or APP_FLASH_FAIL_SIG. A same-name app is replaced in one directory
 * commit, keeping its eviction order.
 *
 * @param file   Storage-relative file name.
 * @param launch Boot the newly-installed app as soon as the write verifies.
 */
void appmgr_flash_request(const char *file, bool launch);

/**
 * @brief Ask AO_AppMgr to boot the installed app named @p name.
 *
 * The board resets; the custom bootloader jumps straight to that app's
 * extent. Logged and ignored if no installed app has that name - `app_list`
 * says what does.
 */
void appmgr_run_request(const char *name);

/**
 * @brief Leave the install in hand: nothing is committed, and it ends as
 *        APP_FLASH_FAIL_SIG.
 *
 * While victims are being kept on the card it ends when that copy does. With no
 * install, nothing happens.
 */
void appmgr_cancel_request(void);

/**
 * @brief Ask AO_AppMgr to take everything of the app named @p name off the
 *        board.
 *
 * The installed image and its directory entry, its category override, a
 * mailbox image of that project, and through AO_Storage every image file of
 * the project and its data folder on both media. The app need not be
 * installed: its files alone are removed the same way. The end is published
 * as APP_ERASE_DONE_SIG (AppEraseDoneEvt), a refusal included.
 *
 * @param name The app's project name.
 */
void appmgr_erase_request(const char *name);

/** @brief How long AO_AppMgr waits for AO_Storage to take an erased app's
 *         files off both media, in ms: one descriptor read per image file
 *         there is, and the deletes. */
#define APPMGR_WIPE_MS      (20000U)

/**
 * @brief Ask AO_AppMgr to assign or drop an app's categories.
 *
 * @param project The app's project name.
 * @param cats    A comma-separated list; ignored when @p clear is true.
 * @param clear   Drop the row rather than write one.
 */
void appmgr_cat_request(const char *project, const char *cats, bool clear);

/**
 * @brief Ask AO_AppMgr to install an image arriving on the console.
 *
 * `install src="terminal"`: AO_Log hands AO_AppMgr exactly @p size raw bytes; the
 * install is the same as from a file. @p dst: KFW_TARGET_APP the region,
 * KFW_TARGET_OS the inactive kodeOS slot (AO_Boot switches and resets), or
 * KODE_FLASH_DST_PENDING the mailbox (undecoded). Answered with RECV_READY_SIG,
 * then RECV_DONE_SIG / RECV_FAIL_SIG / RECV_STALL_SIG, plus APP_FLASH_* progress.
 *
 * @param dst    KFW_TARGET_APP or KFW_TARGET_OS.
 * @param size   Payload length in bytes.
 * @param crc32  What khal_crc32() over the whole payload must equal.
 * @param launch Boot the app as soon as the install verifies (region only).
 * @param src    KODE_CONSOLE_SRC_*: the port the payload follows on.
 * @param name   The image's project name as the host read it, or "" (NULL too):
 *               room is made first, and an app of that name gives its place first.
 * @return false if the request event could not be allocated: no RECV_* signal is
 *         coming, and the caller must not wait for one.
 */
bool appmgr_recv_request(uint8_t dst, uint32_t size, uint32_t crc32,
                         bool launch, uint8_t src, const char *name);

/**
 * @brief Write an image from one source into one destination.
 *
 * The grammar behind `install src= dst=`, and the only way to ask AO_AppMgr to
 * write an image.
 *
 * @param src_kind  kfw_src_t as a plain integer (kdrv_kfw.h).
 * @param src       File name for fs/sd, URL for http, ignored otherwise.
 * @param dst_kind  kfw_target_t as a plain integer.
 * @param launch    Boot the image once it is written and verified.
 */
void appmgr_image_request(uint8_t src_kind, const char *src,
                          uint8_t dst_kind, bool launch);

/**
 * @brief The event appmgr_image_request() posts, built for a caller that posts it itself.
 * @param src_kind kfw_src_t as a plain integer.
 * @param src      File name for fs/sd, URL for http; NULL for none.
 * @param dst_kind kfw_target_t as a plain integer.
 * @param launch   Boot the image once it is written and verified.
 * @return The event, or NULL when the pool is dry.
 */
QEvt const *appmgr_image_evt(uint8_t src_kind, const char *src,
                             uint8_t dst_kind, bool launch);

/**
 * @brief Ask AO_AppMgr for the first page of the installed apps
 *        (appmgr_list_page(0, who)).
 * @param who The asker; the pages are posted to it alone.
 */
void appmgr_list_request(QActive *who);

/**
 * @brief Ask AO_AppMgr for one page of the installed apps.
 *
 * As AO_Storage's listing: from @p skip, at most KODE_STORAGE_LIST_MAX APP_ENTRY_SIG
 * (with `pos`), then APP_LIST_END_SIG (`more`), posted to @p who alone.
 *
 * @param skip Entries to leave out: the ones earlier pages sent.
 * @param who  The asker.
 */
void appmgr_list_page(uint16_t skip, QActive *who);

/**
 * @brief Ask AO_AppMgr which installed apps an install of @p size bytes would
 *        evict, and whether each already has a copy on the card.
 *
 * Changes nothing. Posted to @p who: up to KODE_APP_EVICT_MAX APP_EVICT_ENTRY_SIG in
 * eviction order (`on_card`: `/sdcard/<name>.bin` exists), then APP_EVICT_END_SIG
 * (`victims`, `fits`). Exact only with @p project. When not idle it answers at once
 * with `busy` set; while a query is open other requests are treated as busy. No
 * answer is guaranteed: keep a deadline. Any task or AO.
 *
 * @param size    Bytes the image takes in the region: the file's size for a raw
 *                image, the header's `image_len` (kfw_hdr_t) for a KFW1 container.
 * @param project The project name the image will carry (its descriptor's), or
 *                NULL or "" if unknown. Cut to KODE_APP_PROJECT_MAX - 1.
 * @param who     The Active Object the answer is posted to. Not NULL.
 * @return false when the request could not be posted - the pool was dry, the
 *         queue was full or @p who was NULL; no answer is coming.
 */
bool appmgr_evict_query(uint32_t size, const char *project, QActive *who);

/**
 * @brief Ask AO_AppMgr where the app named @p project is.
 *
 * One APP_FIND_RSP_SIG (AppFindRspEvt) posted to @p who: an installed app's extent
 * (`offset`, `size`), or else `on_card` for `/sdcard/<project>.bin`. When not idle,
 * `busy` at once; ask again. Any task or AO; keep a deadline of your own.
 *
 * @param project The app's project name. Longer than KODE_APP_PROJECT_MAX - 1
 *                is cut.
 * @param who     The Active Object the answer is posted to. Not NULL.
 * @return false when the request could not be posted - the pool was dry, the
 *         queue was full, or @p project or @p who was NULL; no answer is
 *         coming.
 */
bool appmgr_find_request(const char *project, QActive *who);

/**
 * @brief Ask AO_AppMgr to remove every installed app but @p project.
 *
 * One directory commit, then each image header erased and data folder deleted.
 * One APP_KEEP_ONLY_RSP_SIG to @p who: `status` KODE_OK; KODE_DENIED busy or not
 * installed, KODE_ERROR commit failed (nothing erased either way); `erased` counts
 * removals. A folder not deleted is only logged. Any task or AO; keep a deadline.
 *
 * @param project The one app that stays, "" for none (then never KODE_DENIED for
 *                not installed). Cut to KODE_APP_PROJECT_MAX - 1.
 * @param who     The Active Object the answer is posted to. Not NULL.
 * @return false when the request could not be posted - the pool was dry, the
 *         queue was full, or @p project or @p who was NULL; no answer is
 *         coming.
 */
bool appmgr_keep_only_request(const char *project, QActive *who);

/** @brief How a fetch of an app's data folder ended (AppDataGetRspEvt.why). */
typedef enum {
    APPDATA_GET_OK = 0,   /**< The folder is written.                           */
    APPDATA_GET_BUSY,     /**< AO_AppMgr or AO_Storage was busy with a write.   */
    APPDATA_GET_NAME,     /**< The project cannot name a folder.                */
    APPDATA_GET_FETCH,    /**< A file did not come: no network, no 200, no
                           *   Content-Length, fewer bytes than it said, or
                           *   silence.                                         */
    APPDATA_GET_JSON,     /**< app.json is longer than KAPP_DATA_JSON_MAX, is
                           *   not what the board reads, or names an icon
                           *   other than icon.k64.                             */
    APPDATA_GET_ICON,     /**< icon.k64 is neither KAPP_ICON_BYTES nor
                           *   KAPP_ICON_HALF_BYTES long.                       */
    APPDATA_GET_FULL,     /**< The internal partition has no room for it.       */
    APPDATA_GET_STORE     /**< The folder could not be written.                 */
} appdata_get_why_t;

/** @brief Fetch one app's data folder from a URL (APPDATA_GET_REQ_SIG). */
typedef struct {
    QEvt     super;
    char     project[KDRV_SLOT_PROJECT_MAX]; /**< The app's project name.       */
    char     url[KODE_APPDATA_URL_MAX];      /**< The base URL; the two file
                                              *   names are added to it.        */
    QActive *who;                            /**< Where APPDATA_GET_RSP_SIG is
                                              *   posted.                       */
} AppDataGetReqEvt;

/** @brief How a fetch of one app's data folder ended (APPDATA_GET_RSP_SIG). */
typedef struct {
    QEvt    super;
    char    project[KDRV_SLOT_PROJECT_MAX]; /**< Echoed.                        */
    uint8_t why;                            /**< appdata_get_why_t.             */
    bool    icon;                           /**< The folder holds an icon.      */
} AppDataGetRspEvt;

/**
 * @brief Ask AO_AppMgr to fetch @p project's data folder from @p url and write it.
 *
 * An app's data travels apart from its image and may be on the board before the
 * app is. Two files are read, `<url>app.json` and, when that names an icon,
 * `<url>icon.k64`; both must come with a Content-Length, and they are checked
 * as the folder's own reader checks them before AO_Storage writes the folder
 * (appdata_put_request()), replacing one that was there. One APPDATA_GET_RSP_SIG
 * to @p who, refusals included: AO_AppMgr does one thing at a time and answers
 * APPDATA_GET_BUSY while it writes an image. Any task or AO.
 *
 * @param project The app's project name, as its image declares it.
 * @param url     The base URL, ending where the file names begin; shorter than
 *                KODE_APPDATA_URL_MAX.
 * @param who     The Active Object the answer is posted to. Not NULL.
 * @return KODE_OK when the request is posted and an answer is owed; KODE_ERROR
 *         for a NULL argument or a URL that does not fit; KODE_NO_RESOURCES when
 *         the pool is dry or the queue full. Nothing was posted for the last two.
 */
kode_status_t appmgr_appdata_get_request(const char *project, const char *url,
                                         QActive *who);

/* ---- AO_Boot: owner of otadata AND the apps boot request ----------------
 * kodeOS lives in `factory` or its OTA slot; apps live in the `apps` region.
 *   1. BOOT_OS_SWITCH_SIG points otadata at the inactive kodeOS slot and resets;
 *      the bootloader marks it PENDING_VERIFY until kdrv_slot_confirm_self(),
 *      and a reset before that goes back to the previous slot.
 *   2. BOOT_APP_RUN_SIG writes a boot request (kdrv_apps_req_set) and resets; the
 *      bootloader marks it TRIED before jumping, so the next reset is kodeOS.
 */
extern QActive * const AO_Boot;     /**< The otadata + apps-bootreq owner AO. */
void Boot_ctor(void);               /**< Construct it; start FIRST of all. */
/**
 * @brief Construct AO_Boot and start it at KODE_PRIO_BOOT, on its own queue and
 *        stack, then register its settings.
 */
void kaos_boot_start(void);

/** @brief How this boot of kodeOS came to be. */
typedef enum {
    KBOOT_COLD,       /**< Power-on or a plain reset; no app request was pending. */
    KBOOT_FROM_APP,   /**< We are back from a user app (the bootloader tried it). */
    KBOOT_UNEXPECTED, /**< otadata held a stale entry on factory; it has been cleared. */
} kboot_reason_t;

/** @brief Why we booted, as AO_Boot worked it out on the way up. */
kboot_reason_t boot_reason(void);

/**
 * @brief How the app before this boot ended.
 *
 * From the chip's reset reason. Clean is every way back that is not panic,
 * watchdog or brownout, the person's own (BOT+LEFT, USB port opening) included.
 */
typedef enum {
    KBOOT_APP_END_NONE,      /**< This boot is not back from an app.            */
    KBOOT_APP_END_CLEAN,     /**< The app left or was left; any other reason.   */
    KBOOT_APP_END_PANIC,     /**< The app panicked, or the CPU locked up.       */
    KBOOT_APP_END_WATCHDOG,  /**< A watchdog ended the app.                     */
    KBOOT_APP_END_BROWNOUT   /**< The supply dipped under the app.              */
} kboot_app_end_t;

/**
 * @brief How the app before this boot ended, as AO_Boot worked it out on the
 *        way up.
 *
 * @return KBOOT_APP_END_NONE when this boot is not back from an app.
 */
kboot_app_end_t boot_app_end(void);

/**
 * @brief What boot_app_offset() answers when no app ran before this boot.
 *
 * No image sits here: every one starts on a KAPPS_ALIGN page of the apps
 * region, so this word is never an app's offset.
 */
#define KODE_APP_OFFSET_NONE  UINT32_MAX

/**
 * @brief Where, in the apps region, the app that ran before this boot sits.
 *
 * Read once from the bootloader's request; matches AppEntryEvt.offset. Named when
 * the request is TRIED or ARMED (launched); NEW or ALWAYS (refused) name nobody.
 * Independent of boot_reason(); in the app role always KODE_APP_OFFSET_NONE.
 *
 * @return The app's offset in the apps region, or KODE_APP_OFFSET_NONE when
 *         no app ran before this boot.
 */
uint32_t boot_app_offset(void);

/**
 * @brief Classify the end of an app from the boot and the chip's reset reason.
 *
 * Pure, so a table test holds the whole mapping. An unknown reason reads as
 * CLEAN, never PANIC: a false panic would blame a healthy app.
 *
 * @param r      What kind of boot this is.
 * @param reason What the chip says reset it: a KDRV_RESET_ value (kdrv_slot.h),
 *               or any other number.
 * @return KBOOT_APP_END_NONE unless @p r is KBOOT_FROM_APP; then PANIC (panic, CPU
 *         lockup), WATCHDOG (any watchdog), BROWNOUT (brownout, power glitch) or
 *         CLEAN for every other reason.
 */
kboot_app_end_t boot_app_end_of(kboot_reason_t r, int reason);

/**
 * @brief The crashes in a row of the running kodeOS slot, this boot counted.
 *
 * Pure, with boot_app_end_of()'s classes: a panic or watchdog adds one, up to
 * KODE_BOOT_CRASHES_MAX; a brownout or glitch changes nothing; anything else resets
 * the count to none.
 *
 * @param crashes   What was kept for this slot.
 * @param reason    What the chip says reset it: a KDRV_RESET_ value, or any other number.
 * @param after_app An app ran before this boot, so the reason is the app's, not
 *                  kodeOS's, and changes nothing.
 * @return The crashes to keep.
 */
uint32_t boot_crashes_next(uint32_t crashes, int reason, bool after_app);

/**
 * @brief The interface's first frame has reached the glass: a kodeOS update has
 *        shown it works, and confirms itself.
 *
 * Any task or AO, any number of times; it posts and returns. A lost post is
 * covered by AO_Boot's own timer. Does nothing in an app's firmware.
 */
void boot_first_frame(void);

/**
 * @brief Every row of kode_init() has been walked: AO_Boot's counts start.
 *
 * After KODE_BOOT_CONFIRM_MS an update with no first frame confirms itself; after
 * KODE_BOOT_STABLE_MS the crash count resets. Called once by kode_init(), after the
 * rows. Does nothing in an app's firmware.
 */
void boot_system_up(void);

/**
 * @brief The word for an app end, as the log and `boot_info` print it.
 *
 * @param e The class.
 * @return "none", "clean", "panic", "watchdog" or "brownout"; "?" for a value
 *         that is none of them. Never NULL.
 */
const char *boot_app_end_name(kboot_app_end_t e);

/** @brief Ask AO_Boot to switch kodeOS to its other OTA slot. The board resets. */
void boot_os_switch_request(void);

/**
 * @brief Ask AO_Boot to boot the app extent at @p offset/@p size. The board
 *        resets; the bootloader jumps there directly.
 *
 * @param offset Region-relative offset of the image.
 * @param size   Image length in bytes.
 * @param mode   KBOOT_RUN_ONCE for one boot, then back to kodeOS (launcher,
 *               console). KBOOT_RUN_STICKY survives the USB port's resets, and
 *               is for mailbox adoption only.
 */
void boot_app_run_request(uint32_t offset, uint32_t size,
                          kboot_run_mode_t mode);

/** @brief Ask AO_Boot to log the boot state - what `boot_info` prints. */
void boot_info_request(void);

/**
 * @brief The request that has AO_Boot reset the board (BOOT_EXIT_REQ_SIG); it
 *        does not touch otadata.
 *
 * How an app goes home (kode_sys_exit_to_os()): the app never confirmed its
 * PENDING_VERIFY slot, so the next boot is kodeOS.
 * @return A static event; never NULL.
 */
QEvt const *boot_exit_evt(void);

/** @brief How a factory reset ends once everything is erased (FactoryResetReqEvt.end). */
typedef enum {
    KBOOT_RESET_RESTART = 0,  /**< The board restarts into the welcome app.     */
    KBOOT_RESET_SHIP          /**< The board waits for the cable to leave for
                               *   KODE_SHIP_UNPLUG_MS, then switches off.      */
} kboot_reset_end_t;

/* What can fail comes before what cannot be undone, so a failed step leaves the apps and the
 * settings where they were. */
/** @brief The steps of a factory reset, in order (FactoryResetRspEvt.step). */
typedef enum {
    KBOOT_RESET_STEP_FIND = 0,  /**< Where the welcome app is.                  */
    KBOOT_RESET_STEP_LINK,      /**< The C5 link up.                            */
    KBOOT_RESET_STEP_INSTALL,   /**< The welcome app written from the card.     */
    KBOOT_RESET_STEP_WIPE,      /**< The C5's networks and BLE keys, and this
                                 *   chip's bond store.                         */
    KBOOT_RESET_STEP_APPS,      /**< Every other app removed.                   */
    KBOOT_RESET_STEP_NVS,       /**< Every other NVS namespace.                 */
    KBOOT_RESET_STEP_REQUEST,   /**< The closing request: the welcome app's boot
                                 *   request or AO_Power's cable arm. Only ever
                                 *   a failure step: all is erased by then.     */
    KBOOT_RESET_STEP_DONE       /**< Everything is erased.                      */
} kboot_reset_step_t;

/**
 * @brief The word for a step of the factory reset, as the console prints it.
 *
 * @param step A kboot_reset_step_t.
 * @return "find", "install", "apps", "link", "wipe", "nvs", "request" or
 *         "done"; "?" for a value that is none of them. Never NULL.
 */
const char *boot_reset_step_name(uint8_t step);

/**
 * @brief Ask AO_Boot to forget everything the board remembers.
 *
 * Keeps the identity cache, the station lock and the welcome app. Steps, each with
 * a KODE_RESET_*_MS budget: find, link, install the welcome app, wipe the C5 and
 * bonds, remove apps, erase NVS. What can fail comes first: a C5 that will not
 * answer stops it with nothing erased. Then RESTART writes the welcome app's boot
 * request and restarts at once (no radio park, no clean channel leave); SHIP waits
 * for the cable out KODE_SHIP_UNPLUG_MS, erases NVS again and ships. kodeOS role only.
 *
 * @param end How the reset ends: KBOOT_RESET_RESTART or KBOOT_RESET_SHIP. With no
 *            welcome app, RESTART goes on into kodeOS; SHIP refuses (FIND,
 *            KODE_DENIED, nothing erased) or fails at INSTALL.
 * @param who The Active Object answered with one FACTORY_RESET_RSP_SIG: `step` that
 *            stopped it or DONE, and `status` (success only when KODE_OK). FIND with
 *            KODE_NO_RESOURCES: busy, nothing changed; ask again later. Not NULL.
 * @return false when the request could not be posted - the pool was dry, the
 *         queue was full, @p who was NULL or @p end names no end; no answer
 *         is coming.
 */
bool boot_factory_reset_request(kboot_reset_end_t end, QActive *who);

/**
 * @brief Tell AO_Boot that AO_Radio has done with the C5 for the jump.
 *
 * AO_Radio's answer to radio_park_request(), exactly once per request: the C5
 * is parked and the note left, or AO_Radio gave up on it and no note was left.
 * Either way the jump may go on.
 */
void boot_radio_parked(void);

/**
 * @brief Tell AO_Boot that AO_Net has left the Kode channel for the jump.
 *
 * AO_Net's answer to net_leave_request(), exactly once per request: the
 * client is stopped cleanly (the broker discards the last will) or there was
 * none. The jump may go on.
 */
void boot_net_left(void);

/**
 * @brief Whether a held bottom button is still waiting to reset the board,
 *        and a deferral would still be honoured.
 *
 * False once deferred (capped at KODE_SYS_HOME_DEFER_MAX_MS; no second ask), and
 * always without CONFIG_KODEOS_HOME_HOLD.
 *
 * @return true while the grace is running.
 */
bool boot_home_pending(void);

/**
 * @brief The request for a moment before AO_Boot resets on a held bottom button
 *        (BOOT_HOME_DEFER_SIG).
 *
 * The facade (kode_sys_home_defer()) is what an app calls; this is the event
 * behind it. Ignored unless boot_home_pending() is true.
 * @return A static event; never NULL.
 */
QEvt const *boot_home_defer_evt(void);

/**
 * @brief The request that tells AO_Boot the app is ready to go home (BOOT_HOME_NOW_SIG).
 * @return A static event; never NULL.
 */
QEvt const *boot_home_now_evt(void);

/* ---- AO_Log: owner of the console device --------------------------------
 * The transport, the character echo and the prompt: one device, one owner. klog
 * posts formatted lines here and the reader task posts bytes; nothing else writes
 * to the console, so no lock is needed.
 */
extern QActive * const AO_Log;      /**< The console device owner AO. */
void Log_ctor(void);                /**< Construct it; start EARLY. */
/**
 * @brief Construct AO_Log and start it at KODE_PRIO_LOG, on its own queue and
 *        stack, then register its settings.
 */
void kaos_log_start(void);

/** @brief Start the reader task that feeds AO_Log bytes. Call after QF_run. */
void log_start_reader(void);

/**
 * @brief Ask AO_Log to apply a log level (LOG_LEVEL_SET_SIG).
 *
 * The global default is stored under the `log` key, so it survives the reset that
 * opening the port causes; a value equal to CONFIG_KLOG_DEFAULT_LEVEL erases the
 * key. Per-tag levels are never stored. Applied before AO_Log's next line.
 *
 * @param tag   The tag, copied; NULL for the global default. A tag of
 *              sixteen characters or more is refused by klog, with a line.
 * @param level The minimum level; KODE_LOG_OFF suppresses every log line.
 */
void log_level_request(const char *tag, kode_log_lvl_t level);

/**
 * @brief Ask AO_Log to lend the dock console's pins (the magnetic pads,
 *        GPIO26 and GPIO27), or to take them back.
 *
 * For the pads delivery test's UART. Lend stops the dock's USB console
 * (CONFIG_KHAL_CONSOLE_DOCK) and frees the pins; take back restarts it. Answered
 * with one LOG_DOCK_SIG (LogDockEvt) to @p who; without the dock, KODE_OK at once.
 *
 * @param who  The Active Object answered. Not NULL.
 * @param lend true to lend the pins, false to take them back.
 * @return false when nothing was posted: the pool was dry, AO_Log's queue
 *         was within KQP_CMD_MARGIN, or @p who was NULL.
 */
bool log_dock_lend_request(QActive *who, bool lend);

/* ---- AO_Console: the command line ---------------------------------------
 * Owns no hardware. It receives whole lines from AO_Log, parses them
 * run-to-completion, and drives everything else by posting the events those
 * modules already understand.
 */
extern QActive * const AO_Console;  /**< The parser AO. */
void Console_ctor(void);            /**< Construct it; kaos_console_start() calls it. */
/**
 * @brief Construct AO_Console and start it at KODE_PRIO_CONSOLE, on its own queue
 *        and stack.
 */
void kaos_console_start(void);

/**
 * @brief Whether every row of the command table declares its keys legally.
 *
 * A row with more than KCLI_MAX_KEYS keys writes past console_exec()'s value
 * array. Console_ctor() asserts this before the console takes a line.
 *
 * @return true when every row fits.
 */
bool console_specs_ok(void);

/* ---- AO_LowPower: the board's three states -------------------------------
 * AWAKE, ASLEEP and OFF (kode_lp_state_t). AO_LowPower owns no device: it
 * runs a round of readiness among the owners that joined, and each owner
 * rests its own device on the state it publishes. The calls below that exist
 * without the option are the facade's: with it off they refuse and log.
 */

#if CONFIG_KODE_LOWPOWER
extern QActive * const AO_LowPower;  /**< The low-power AO. */
void LowPower_ctor(void);            /**< Construct it; kaos_lowpower_start() calls it. */
/**
 * @brief Construct, register and start AO_LowPower, before every owner that joins
 *        its rounds, then register its settings.
 */
void kaos_lowpower_start(void);
/** @brief Register its console verbs; kaos_lowpower_start() calls it. */
void kaos_lowpower_register(void);

/**
 * @brief The event lowpower_request() posts, for a door that posts it with its
 *        own margin (kode_sys_power_sleep(), kode_sys_power_off()).
 *
 * @param target The state wanted.
 * @param src    Who asked, for the log.
 * @return A pool event; NULL when the pool is dry.
 */
QEvt const *lowpower_req_evt(kode_lp_state_t target, uint8_t src);
#endif

/**
 * @brief Ask AO_LowPower to take the board to @p target.
 *
 * The request is judged by a round of readiness; a participant that is busy
 * refuses it, and lowpower_last_refusal() says who and why. With the option
 * off it logs a refusal and does nothing.
 *
 * @param target The state wanted.
 * @param src    Who asked, for the log: KODE_CONSOLE_SRC_* for a command.
 */
void lowpower_request(kode_lp_state_t target, uint8_t src);

/* cppcheck-suppress misra-c2012-2.5 ; the public name of a refusal lowpower_request_rest() documents, for a caller to compare with; nothing in the SDK compares with it */
#define KODE_LOWPOWER_WHY_USED  "input: board used" /**< A rest refused as
                                                     *   the board was used
                                                     *   since its last wake:
                                                     *   it is a person's. */

/** @brief The refusal of a rest that yields to another's request waiting to begin
 *         again: it passes within the retry window, so the scheduler may ask again. */
#define KODE_LOWPOWER_WHY_PENDING  "request: request pending"

/** @brief The refusal of a request or a rest asked while a round runs: it
 *         passes by itself when the round ends, so the scheduler may ask
 *         again. */
#define KODE_LOWPOWER_WHY_RUNNING  "request: a round is running"

/**
 * @brief Ask AO_LowPower to put the board back to sleep once an owner's work
 *        is done, unless somebody is using it.
 *
 * For AO_Scheduler, which cannot know who is at the board. AWAKE with no input
 * since the last wake (button, touch, console line not from KODE_CONSOLE_SRC_SCHED,
 * cable), it is lowpower_request(KODE_LP_ASLEEP, @p src); otherwise, or if a person
 * comes during the round, refused with KODE_LOWPOWER_WHY_USED. Answered exactly once:
 * LOWPOWER_STATE_SIG, or LOWPOWER_REFUSED_SIG with @p src. Safe from any task.
 *
 * @param src Who asks, for the log and the answer: KODE_LOWPOWER_SRC_SCHED
 *            for the scheduler.
 * @return true when the request was posted and an answer will follow; false
 *         when no event was left to post it with, or the option is off, and
 *         none will.
 */
bool lowpower_request_rest(uint8_t src);

/**
 * @brief Tell AO_LowPower a console line came in.
 *
 * Restarts the idle time; asleep, it wakes the board (MQTT, BLE or cable wake by
 * @p src). Buttons and touches need no call. KODE_CONSOLE_SRC_SCHED lines are not
 * input. Safe from any task: it posts a static event.
 *
 * @param src The KODE_CONSOLE_SRC_* port the line came on.
 */
void lowpower_input_note(uint8_t src);

/**
 * @brief The last refusal or busy reason a round met.
 * @return "who: why", or "" when none; "not built" in a build without the
 *         option. Valid until the next round; the console reads it from
 *         another task, so a round beginning at that instant can tear it.
 */
const char *lowpower_last_refusal(void);

#if CONFIG_KODE_LOWPOWER
/** @brief What `lowpower_info` prints of AO_LowPower itself. */
typedef struct {
    uint8_t  state;          /**< The kode_lp_state_t now.                          */
    uint8_t  cause;          /**< The kode_lp_cause_t of the last change of state.  */
    uint32_t for_s;          /**< Seconds in the state.                             */
    uint32_t sleep_after_s;  /**< The idle time before ASLEEP; 0 is never.          */
    uint32_t off_after_s;    /**< The time asleep before OFF; 0 is never.          */
    uint32_t wakes_bot;      /**< Wakes from ASLEEP by BOT.                         */
    uint32_t wakes_ble;      /**< Wakes from ASLEEP by a line on BLE.               */
    uint32_t wakes_mqtt;     /**< Wakes from ASLEEP by the Kode channel.            */
    uint32_t wakes_timer;    /**< Wakes from ASLEEP for an owner's time, radio
                              *   wakes included.                             */
} lowpower_info_t;

/**
 * @brief AO_LowPower's state and counters, for `lowpower_info`.
 *
 * Read from another task, a word at a time: a change of state at that instant
 * can leave the seconds one state behind. Counters are since boot.
 *
 * @param out Receives them; untouched when NULL.
 */
void lowpower_info_get(lowpower_info_t *out);

/** @brief The bytes of the summary's fixed part, before the samples. */
#define KODE_LOWPOWER_LAST_HEAD_BYTES  32U
/** @brief The bytes of one sample. */
#define KODE_LOWPOWER_LAST_SAMPLE_BYTES 8U

/** @brief One gauge reading taken at a minute of a rest. */
typedef struct {
    uint32_t t_s;   /**< Seconds from the start of the rest.                      */
    int16_t  ma;    /**< The cell's current, into it positive (PowerEvt).         */
    uint16_t mv;    /**< The cell's voltage.                                      */
} lowpower_sample_t;

/**
 * @brief The summary of the last rest, the record kept under the store's `lp_last`.
 *
 * The totals are those of the rest and not of the boot: the light sleeps and
 * the C5's wakes are counted from the rest's start. Written as it stands, so
 * the layout is the store's; @c version tells a record of another layout.
 */
typedef struct {
    uint8_t  version;       /**< KODE_LOWPOWER_LAST_VERSION.                          */
    uint8_t  end_cause;     /**< The kode_lp_cause_t that ended the rest, or
                             *   KODE_LOWPOWER_LAST_OPEN when none was written.       */
    uint8_t  slept_pct;     /**< The share of the rest spent in light sleep, 0..100.  */
    uint8_t  count;         /**< Samples held, at most KODE_LOWPOWER_LAST_SAMPLES.    */
    uint8_t  next;          /**< The slot the next sample takes; the oldest sample
                             *   once the ring is full.                               */
    uint8_t  reserved[3];   /**< Zero.                                                */
    uint32_t start_s;       /**< The wall clock when the rest began, in Unix seconds;
                             *   0 when the clock was not set.                        */
    uint32_t duration_s;    /**< How long the rest had lasted at the last write.      */
    uint32_t sleeps;        /**< Light sleeps entered and left in it.                 */
    uint32_t wakes_c5;      /**< Wakes of the P4 by the C5's line in it.              */
    uint32_t wakes_tx;      /**< Wakes of the P4 by its own traffic for the C5.       */
    uint32_t deaf;          /**< Wakes the C5 never answered.                         */
    lowpower_sample_t samples[KODE_LOWPOWER_LAST_SAMPLES]; /**< The ring.             */
} lowpower_last_t;

_Static_assert(sizeof(lowpower_sample_t) == KODE_LOWPOWER_LAST_SAMPLE_BYTES,
               "a sample has no padding: the store holds it as it stands");
_Static_assert(sizeof(lowpower_last_t) == (KODE_LOWPOWER_LAST_HEAD_BYTES
                   + (KODE_LOWPOWER_LAST_SAMPLES * KODE_LOWPOWER_LAST_SAMPLE_BYTES)),
               "the summary has no padding: the store holds it as it stands");

/**
 * @brief Read the summary of the last rest from the store.
 *
 * Safe from any task: the store hands over a whole record or none. A record
 * of another layout (version or size) reads as none.
 *
 * @param out Receives the summary; written only when true is returned.
 * @return true when a summary was there; false for none, a record of another
 *         layout or a NULL @p out.
 */
bool lowpower_last_read(lowpower_last_t *out);

/**
 * @brief The @p i-th sample of a summary, oldest first.
 *
 * @param l The summary.
 * @param i The place in the order, 0 the oldest.
 * @return The sample, or NULL past the last one.
 */
const lowpower_sample_t *lowpower_last_sample(const lowpower_last_t *l, uint8_t i);

/**
 * @brief Join the rounds of readiness.
 *
 * Once, from the owner's initial transition; it then subscribes to
 * LOWPOWER_ROUND_SIG and LOWPOWER_STATE_SIG. It rests its device on a round and
 * answers exactly once; restores on STATE(AWAKE), idempotently (it may repeat);
 * ignores KODE_LP_RADIO; arms no QTimeEvt on ASLEEP (the tick is stopped): work
 * due asleep is timed with khal_time_us() and given with lowpower_wake_at().
 *
 * @param ao   The participant's Active Object.
 * @param name Short name for the log and for `lowpower_info`.
 * @return The participant's id; KODE_LOWPOWER_WHO_NONE when the table
 *         (KODE_LOWPOWER_PARTICIPANTS) is full.
 */
uint8_t lowpower_join(QActive *ao, const char *name);

/**
 * @brief Join the rounds like lowpower_join(), and take part in radio wakes.
 *
 * A radio wake (from one of these owners' lowpower_wake_at() times) runs the tick
 * for them alone. On STATE(KODE_LP_RADIO) restore as for AWAKE, work, then call
 * lowpower_radio_done() once; when all are done, or after KODE_LOWPOWER_RADIO_MAX_MS,
 * a round for ASLEEP goes to them alone. A busy or silent owner, BOT, a console
 * line, a broker message or a cable turns it into a full AWAKE.
 *
 * @param ao   The participant's Active Object.
 * @param name Short name for the log and for `lowpower_info`.
 * @return The participant's id; KODE_LOWPOWER_WHO_NONE when the table is full.
 */
uint8_t lowpower_join_radio(QActive *ao, const char *name);

/**
 * @brief Say that the owner's work in a radio wake is finished (posts
 *        LOWPOWER_RADIO_DONE_SIG).
 *
 * Once for each LOWPOWER_STATE_SIG with state KODE_LP_RADIO. Outside a radio
 * wake it is dropped. Safe from any task.
 *
 * @param who The id lowpower_join_radio() gave.
 */
void lowpower_radio_done(uint8_t who);

/**
 * @brief Say when the owner's work next needs the board awake (posts
 *        LOWPOWER_WAKE_AT_SIG).
 *
 * Before answering a round for ASLEEP (also taken while asleep). AO_LowPower wakes
 * at the earliest time given (KODE_LP_CAUSE_TIMER), a radio wake for a radio owner;
 * a past time wakes at once. Times are forgotten at each round and wake, so say it
 * every round; a radio wake forgets only radio owners' times.
 *
 * @param who    The id lowpower_join() gave.
 * @param due_us khal_time_us() when the work is due; 0 asks for nothing.
 * @return true when the note is on its way; false when no event was left to
 *         carry it, and the owner should answer the round busy.
 */
bool lowpower_wake_at(uint8_t who, uint64_t due_us);

/**
 * @brief Answer a round of readiness (posts LOWPOWER_READY_SIG).
 *
 * Exactly once per round the owner hears, ready or busy; a second answer, or
 * one for an earlier round, is dropped. Ready means the work in hand is
 * finished and the device is at rest.
 *
 * @param who   The id lowpower_join() gave.
 * @param round LowPowerRoundEvt::round of the round answered.
 * @param busy  true to refuse the state change.
 * @param why   Why, at most KODE_LOWPOWER_WHY_MAX - 1 characters; NULL for none.
 */
void lowpower_ready(uint8_t who, uint32_t round, bool busy, const char *why);

/**
 * @brief Hand AO_Display the glyphs the minute is drawn from.
 *
 * The strip is not copied: it stays the caller's, in PSRAM, until the next
 * AWAKE (see DisplayGlyphsEvt). The event is copied; @p e need not outlive
 * the call.
 *
 * @param e The glyphs and where they go.
 */
void display_glyphs_hand(const DisplayGlyphsEvt *e);

/**
 * @brief Tell AO_Radio that AO_Net has closed its sessions for an OFF round.
 *
 * AO_Radio answers its own part of the round only after this, so no session is
 * cut by the power-off. Safe from any task: it posts.
 *
 * @param round LowPowerRoundEvt::round of the OFF round AO_Net answered.
 */
void radio_net_left(uint32_t round);

/**
 * @brief Say whether AO_Net takes part in the rounds, so an OFF round knows
 *        whether to wait for radio_net_left().
 *
 * AO_Net calls it from its initial transition once joined; without it AO_Radio
 * does not wait. Call it only from an initial transition, before any round.
 *
 * @param expected true when AO_Net will answer rounds and call
 *                 radio_net_left().
 */
void radio_lp_expect_net(bool expected);

/**
 * @brief The last wake note the C5 sentinel reported, for `lowpower_info`.
 *
 * @param out Receives a copy of the note (its QEvt header is not set).
 * @return false when no wake has been noted since boot. The console reads it
 *         from another task, so a note arriving at that instant can tear it.
 */
bool radio_sleep_last_note(RadioSleepNoteEvt *out);
#endif

/* ---- AO_Scheduler: entries that fire at a cron minute --------------------
 * One table, three owners (SCHED_BY_*): an entry is a name, five cron fields on
 * the UTC epoch clock, and work (a console line as source SCHED, or an event to an
 * AO). The built-in `mqtt` entry always holds the first slot; the console may
 * retime or switch it off. CONFIG_KODE_SCHEDULER (kodeOS only): off, the calls
 * refuse and log. Runs at KODE_PRIO_SCHEDULER, taking KODE_PRIO_USER_HIGH's level.
 *
 * Every request is answered by post: SCHED_ENTRY_SIG*, then SCHED_END_SIG with
 * KODE_OK, KODE_DENIED and a reason ("args", "name", "when", "cmd", "fields",
 * "range", "step", "syntax", "never", "full", "owner", "unknown", "memory"), or
 * KODE_ERROR "partial" with the count delivered (KODE_SCHED_END_MARGIN is kept).
 *
 * With CONFIG_KODE_LOWPOWER it joins the rounds: it gives the earliest entry
 * minute as a wake time, and after a timer wake's work asks lowpower_request_rest().
 * An entry still running at its minute is skipped. Nothing is posted before
 * sched_system_up(). `mqtt` minutes go to AO_Net (net_check_at()), a radio-only wake.
 */

#define KODE_SCHED_MQTT_NAME     "mqtt"           /**< The built-in entry's name, reserved. */
#define KODE_SCHED_MQTT_DEFAULT  "*/15 * * * *"   /**< Its time on a board with nothing stored. */
#define KODE_SCHED_WHEN_OFF      "off"            /**< The console's word for "remove this" and "switch `mqtt` off". */

/** @brief Who made an entry, and so who may change it. */
typedef enum {
    SCHED_BY_CONSOLE = 0,   /**< The console verb: stored across a reset.       */
    SCHED_BY_CODE,          /**< An app's call: made again by it at each boot.  */
    SCHED_BY_BUILTIN        /**< `mqtt`: stored its time or its being off.      */
} sched_owner_t;

#if CONFIG_KODE_SCHEDULER
extern QActive * const AO_Scheduler;   /**< The scheduler AO. */
void Scheduler_ctor(void);             /**< Construct it; kaos_scheduler_start() calls it. */
/** @brief Construct, register and start AO_Scheduler. */
void kaos_scheduler_start(void);
/** @brief Register its console verbs; kaos_scheduler_start() calls it. */
void kaos_scheduler_register(void);

/**
 * @brief Every AO has its queue: AO_Scheduler may post to them. Posts
 *        SCHED_UP_SIG, a static event.
 *
 * kode_init()'s call, once, after the last AO started. A refused post is logged.
 */
void sched_system_up(void);
#endif

/**
 * @brief Set an entry of the console: a name, a time, a line to run.
 *
 * Stored. @p when "off" removes the entry, or switches `mqtt` off (whose @p cmd
 * must be NULL or empty). Answered with the entry, then the end; the end alone
 * when it is gone. Safe from any task.
 *
 * @param who  Who is answered; NULL for nobody.
 * @param name The entry's name, at most KODE_SCHED_NAME_MAX characters.
 * @param when Five cron fields, or "off".
 * @param cmd  The console line, shorter than KODE_SCHED_CMD_MAX.
 * @return true when the request was posted and, with @p who, an answer
 *         follows; false when no event was left to carry it, AO_Scheduler's
 *         queue had fewer than KODE_SCHED_REQ_MARGIN slots free, or the
 *         option is off.
 */
bool sched_request_set(QActive *who, const char *name, const char *when,
                       const char *cmd);

/**
 * @brief Set an entry of code: an event posted to @p target at each match.
 *
 * Not stored: the code makes it again at every boot. The event is a
 * SchedFireEvt with signal @p sig. Answered as sched_request_set().
 *
 * @param who    Who is answered; NULL for nobody.
 * @param name   The entry's name.
 * @param when   Five cron fields.
 * @param target The AO each fire is posted to.
 * @param sig    The signal of that event.
 * @return true when the request was posted; false when no event was left,
 *         AO_Scheduler's queue had fewer than KODE_SCHED_REQ_MARGIN slots
 *         free, or the option is off.
 */
bool sched_request_set_ao(QActive *who, const char *name, const char *when,
                          QActive *target, enum_t sig);

/**
 * @brief Remove an entry of code.
 *
 * Another door's entry, and `mqtt`, answer "owner"; the console removes its
 * own with sched_request_set() and "off". Answered with the end alone.
 *
 * @param who  Who is answered; NULL for nobody.
 * @param name The entry's name.
 * @return true when the request was posted; false when @p name is NULL, no
 *         event was left, AO_Scheduler's queue had fewer than
 *         KODE_SCHED_REQ_MARGIN slots free, or the option is off.
 */
bool sched_request_remove(QActive *who, const char *name);

/**
 * @brief List one entry or all of them.
 *
 * One SCHED_ENTRY_SIG per entry in table order (`mqtt` first), then the end.
 * A list of everything is KODE_OK even when empty; a name that is not in
 * the table is "unknown".
 *
 * @param who  Who is answered; a list with nobody to hear it is refused.
 * @param name The entry, or NULL for every one.
 * @return true when the request was posted; false when @p who is NULL, no
 *         event was left, AO_Scheduler's queue had fewer than
 *         KODE_SCHED_REQ_MARGIN slots free, or the option is off.
 */
bool sched_request_list(QActive *who, const char *name);

/**
 * @brief The console has answered the line AO_Scheduler gave it (`>ok` or
 *        `>err`): the next one may go. Posts SCHED_LINE_DONE_SIG.
 *
 * Safe from any task; with no line out it runs nothing. It carries no name, so a
 * late answer is taken for the line now out. A refused post is logged.
 */
void sched_line_done(void);

/**
 * @brief The console was busy when AO_Scheduler's line arrived: it was not taken.
 *        AO_Scheduler gives it again every KODE_SCHED_RETRY_MS until taken or
 *        KODE_SCHED_WORK_BOUND_MS pass. Posts SCHED_LINE_BUSY_SIG.
 *
 * AO_Console calls it instead of refusing a SCHED line as busy. Safe from any
 * task. A refused post is logged.
 */
void sched_line_busy(void);

/**
 * @brief AO_Net made the check it was given the time of: AO_Scheduler gives it
 *        the next. Posts SCHED_CHECK_DONE_SIG.
 *
 * AO_Net's call, after the dwell of a radio wake, or at once when it had
 * nothing to wake or the board was awake. Safe from any task. Without
 * CONFIG_KODE_SCHEDULER it does nothing.
 *
 * @param passed_s The time (Unix seconds) the check was given for.
 */
void sched_check_done(int64_t passed_s);

/* ---- AO_TestDot: the E5 final test ---------------------------------------
 * CONFIG_KODE_TEST_DOT (kodeOS only). Dormant, holding nothing, until a station
 * verb arrives on the cable; the first wakes it (J3 rail, camera, station I2C).
 * `test component=all` is the run, `test_dot_end` gives everything back. Runs at
 * KODE_PRIO_TEST_DOT, taking KODE_PRIO_USER_LOW's level. The LVGL owner draws its
 * screens (TEST_DOT_SCREEN_REQ_SIG) and answers test_dot_screen_done().
 */
#if CONFIG_KODE_TEST_DOT
extern QActive * const AO_TestDot;   /**< The E5 final test's AO. */
void TestDot_ctor(void);             /**< Construct it; kaos_testdot_start() calls it. */
/** @brief Construct AO_TestDot, register its verbs and start it, dormant. */
void kaos_testdot_start(void);
/** @brief Register its console verbs; kaos_testdot_start() calls it. */
void kaos_test_dot_register(void);

/**
 * @brief Hand the console's `test component=all` to AO_TestDot.
 *
 * The console's call, from its `test` handler, with the command in flight and
 * its owner set to the station's verbs: the run's records and its end come
 * back to the console like any station verb's. A request that cannot be
 * posted closes the command with `>err code=5 msg=test`.
 */
void test_dot_run_request(void);

/**
 * @brief Whether a run is going: from `test component=all` until its end.
 *
 * For the console's `factory_reset`, refused `busy` while a run goes. Read
 * from another task, so it can be one step late.
 *
 * @return true while a run goes.
 */
bool test_dot_busy(void);

/**
 * @brief The screen host has @p kind on the panel: posts
 *        TEST_DOT_SCREEN_DONE_SIG to AO_TestDot.
 *
 * The LVGL owner's answer to TEST_DOT_SCREEN_REQ_SIG, once the frame that
 * shows it is flushed. A late answer, for a request no longer waited on, is
 * ignored: the request's number tells it from the one waited on now.
 *
 * @param kind TEST_DOT_SCR_* or TEST_DOT_PAT_*, as the request named it.
 * @param seq  The request's @c seq, echoed.
 */
void test_dot_screen_done(uint8_t kind, uint8_t seq);

/**
 * @brief The console has sent the blob TEST_DOT_LEND_SIG lent it and reads
 *        it no more: posts TEST_DOT_BACK_SIG to AO_TestDot.
 */
void test_dot_blob_back(void);
#endif

/* AO_Gui is not here: it lives in kgui and uses the app's public API. The console
 * reaches it by publishing GUI_DUMP_REQ_SIG, so nothing here links against it.
 */

#ifdef __cplusplus
}
#endif

#endif /* KAOS_H_ */
