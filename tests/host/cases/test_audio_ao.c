/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_audio_ao.c
 * @brief AO_Audio: the production row's sequence, what it refuses, and kode_sys's audio requests.
 */
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "qpc.h"
#include "Mockkdrv_es8311.h"

#include "ao_spy.h"
#include "ask_support.h"
#include "config_support.h"
#include "fake_khal_worker.h"
#include "fakes.h"
#include "kaos.h"
#include "kbsp_owners.h"
#include "kes_fields.h"
#include "khal_nvs.h"
#include "kset.h"
#include "khal_sync.h"
#include "kode_log.h"
#include "kode_sys.h"
#include "kqp.h"
#include "kqp_worker.h"
#include "ktone.h"
#include "lowpower_cases.h"
#include "qf_support.h"

/** @brief Ticks well past any quiet spell. There is no idle close any more -
 *         the codec opens once and stays open - and this is what the cases
 *         that say so wait out. */
#define QUIET_TICKS  400U
/** @brief KODE_AUDIO_GUARD_MS in whole QP ticks. */
#define GUARD_TICKS  ((KODE_AUDIO_GUARD_MS * 100U) / 1000U)

/** @brief The name AO_Audio gives its worker. */
#define WORKER_NAME  "audio_worker"

/** @brief Most deliveries it takes to wind up what a case left behind. */
#define LEFTOVER_MAX  8U

/** @brief Every job the driver was asked to run since the last reset. */
#define SUBMITS_MAX  24U
static kdrv_es8311_req_t s_sub[SUBMITS_MAX];
static unsigned          s_nsub;

/** @brief What kdrv_es8311_run() answers. */
static kode_status_t     s_run_rc;

/** @brief Whether run() plays the worker: runs the jobs the AO submitted and
 *         keeps their done events back until finish(). */
static bool              s_hold;

/** @brief The done event of the job the worker is "still inside", if any. */
static const QEvt       *s_held;

/** @brief The job that event is for. */
static kdrv_es8311_req_t s_held_req;

/** @brief How many times the codec was hushed synchronously. */
static unsigned s_hush;

static void count_hush(int ncalls)
{
    (void)ncalls;
    s_hush += 1U;
}

/** @brief What the next kdrv_es8311_result() hands back. */
static kdrv_es8311_result_t s_res;

/** @brief How many times the stream's ring was emptied, and shut. */
static unsigned s_stream_resets;
static unsigned s_stream_ends;

static kode_status_t count_stream_reset(int ncalls)
{
    (void)ncalls;
    s_stream_resets += 1U;
    return KODE_OK;
}

static void count_stream_end(int ncalls)
{
    (void)ncalls;
    s_stream_ends += 1U;
}

/** @brief How many times a recording was told to stop, and how many times
 *         its stop was cleared. */
static uint32_t s_rec_ends;
static uint32_t s_rec_resets;
/** @brief What s_nsub read at the first reset, and at the first stop. */
static uint32_t s_reset_at_sub;
static uint32_t s_end_at_sub;

static void count_record_reset(int ncalls)
{
    (void)ncalls;
    if ( 0U == s_rec_resets )
    {
        s_reset_at_sub = (uint32_t)s_nsub;
    }
    s_rec_resets += 1U;
}

static void count_record_end(int ncalls)
{
    (void)ncalls;
    if ( 0U == s_rec_ends )
    {
        s_end_at_sub = (uint32_t)s_nsub;
    }
    s_rec_ends += 1U;
}

static kode_status_t record_run(const kdrv_es8311_req_t *req, int ncalls)
{
    (void)ncalls;
    if ( SUBMITS_MAX > s_nsub )
    {
        s_sub[s_nsub] = *req;
    }
    s_nsub += 1U;
    s_held_req = *req;
    return s_run_rc;
}

static void codec_at_rest(void);
static unsigned run(void);
static void deliver(kode_status_t rc);

static void hand_result(kdrv_es8311_result_t *out, int ncalls)
{
    (void)ncalls;
    if ( NULL != out )
    {
        *out = s_res;
    }
}

/**
 * @brief Bring AO_Audio up, once for the whole binary, and reset the rest.
 *
 * The mock is re-initialised on every case; only the registration is once,
 * because QActive_register_() asserts on a priority already taken.
 *
 * Leaves the AO as a board that holds its C5 leaves it: nobody has said the
 * C5 is running, so a first sound would ask for it and wait. That is true
 * for exactly one case in the binary - the AO is constructed once and never
 * forgets the answer - which is why the case about it is the first in this
 * file. Everything else uses audio_up().
 */
static void audio_up_raw(void)
{
    /* The AO outlives the cases, and so does the job one of them left with
     * the worker: until its done event comes the AO refuses every request.
     * It is answered here, as a failure, before the mock it would call is
     * replaced. */
    for ( unsigned i = 0U; (NULL != s_held) && (LEFTOVER_MAX > i); ++i )
    {
        deliver(KODE_ERROR);
        (void)run();
    }
    if ( NULL != s_held )
    {
        QF_gc(s_held);
        s_held = NULL;
    }
    Mockkdrv_es8311_Init();
    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();
    ao_spy_subscribe(AUDIO_TEST_SIG);
    ao_spy_subscribe(AUDIO_FIELD_SIG);
    ao_spy_subscribe(TEST_RESULT_SIG);
    ao_spy_subscribe(AUDIO_FILE_RSP_SIG);
    ao_spy_subscribe(C5_RUN_REQ_SIG);

    if ( qf_support_claim_ao(AO_Audio) )
    {
        kdrv_es8311_init_ExpectAndReturn(KODE_OK);
        /* The boot defaults go through the REAL field table - it is compiled
         * under test - so what the mock sees is the push at the chip that
         * follows, and the values themselves are asserted below. */
        kdrv_es8311_apply_settings_Ignore();
        Audio_ctor();
        qf_support_give_queue(AO_Audio);
        AO_Audio->prio  = KODE_PRIO_AUDIO;
        AO_Audio->pthre = KODE_PRIO_AUDIO;
        QActive_register_(AO_Audio);
        QASM_INIT(AO_Audio, (void *)0, 0U);
    }
    kdrv_es8311_run_StubWithCallback(&record_run);
    kdrv_es8311_result_StubWithCallback(&hand_result);
    kdrv_es8311_hush_StubWithCallback(&count_hush);
    kdrv_es8311_stream_reset_StubWithCallback(&count_stream_reset);
    kdrv_es8311_stream_end_StubWithCallback(&count_stream_end);
    kdrv_es8311_record_end_StubWithCallback(&count_record_end);
    kdrv_es8311_record_reset_StubWithCallback(&count_record_reset);

    s_rec_ends      = 0U;
    s_rec_resets    = 0U;
    s_reset_at_sub  = 0U;
    s_end_at_sub    = 0U;
    s_stream_resets = 0U;
    s_stream_ends   = 0U;
    s_hush      = 0U;
    s_nsub      = 0U;
    s_run_rc    = KODE_OK;
    s_hold      = true;
    (void)memset(&s_held_req, 0, sizeof(s_held_req));
    (void)memset(s_sub, 0, sizeof(s_sub));
    (void)memset(&s_res, 0, sizeof(s_res));
    fake_klog_reset();
    fake_system_reset();
    ao_spy_drain();
    codec_at_rest();
}

/** @brief AO_Radio's word that the C5, and the amplifier with it, is up. */
static void c5_running(void)
{
    static const QEvt running = QEVT_INITIALIZER(C5_RUNNING_SIG);

    QASM_DISPATCH(AO_Audio, &running, 0U);
}

/** @brief audio_up_raw(), on a board whose C5 is already running. */
static void audio_up(void)
{
    audio_up_raw();
    c5_running();
}

/**
 * @brief Play the worker: run every job the AO has submitted, and take each
 *        job's done event back out of the AO's queue.
 *
 * Only while s_hold is set. The AO's queue is empty when this is called - the
 * AO has just been pumped - so the one event the job posts is the done event.
 */
static void pull_jobs(void)
{
    while ( s_hold && (fake_khal_worker_pending_for(WORKER_NAME) > 0U) )
    {
        TEST_ASSERT_NULL_MESSAGE(s_held,
            "a second job reached a worker that is still inside one");
        TEST_ASSERT_TRUE(fake_khal_worker_run_next_for(WORKER_NAME));
        TEST_ASSERT_TRUE_MESSAGE(
            xQueueReceive(AO_Audio->eQueue, &s_held, 0U) == pdTRUE,
            "the worker's done event was not queued");
        TEST_ASSERT_EQUAL_INT((int)AUDIO_JOB_SIG, (int)s_held->sig);
    }
}

/** @brief Let the AO run whatever is on its queue, then the jobs it submitted. */
static unsigned run(void)
{
    unsigned const n = qf_support_pump(AO_Audio, 8U);

    pull_jobs();
    return n;
}

/**
 * @brief Hand the AO the done event held back, carrying @p rc, and leave the
 *        AO's queue alone.
 *
 * @param rc The status the job "returned".
 */
static void deliver(kode_status_t rc)
{
    KqpWorkerDoneEvt *d;

    TEST_ASSERT_NOT_NULL_MESSAGE(s_held, "no job is with the worker");
    d      = (KqpWorkerDoneEvt *)(void *)s_held;
    s_held = NULL;
    d->status = (uint8_t)rc;
    QASM_DISPATCH(AO_Audio, &d->super, 0U);
    QF_gc(&d->super);
}

/**
 * @brief The job of kind @p type ends with @p rc: the worker reports at last
 *        and the AO works through what follows.
 *
 * @param type The job the case believes the worker holds.
 * @param rc   What it returned.
 */
static void finish(kdrv_es8311_job_t type, kode_status_t rc)
{
    TEST_ASSERT_NOT_NULL_MESSAGE(s_held, "no job is with the worker");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)type, (int)s_held_req.type,
        "the job the worker holds is not the one being finished");
    deliver(rc);
    (void)run();
}

/**
 * @brief Put the codec back to rest, whatever the case before this left.
 *
 * Since 2026-09-16 nothing closes it for being idle, so a case that opened it
 * hands an OPEN device to the next one - and most of the cases in this file
 * are written about a codec that has to be opened first. The board's own way
 * back is the power-off heads-up, so that is what this uses. The counters are
 * reset afterwards, so a case still starts from zero submits and a closed
 * device, exactly as it did while the idle timer existed.
 */
static void codec_at_rest(void)
{
    static const QEvt sleeping = QEVT_INITIALIZER(POWER_SHIP_SIG);

    QASM_DISPATCH(AO_Audio, &sleeping, 0U);
    (void)run();
    if ( (NULL != s_held) && (KDRV_ES8311_JOB_CLOSE == s_held_req.type) )
    {
        finish(KDRV_ES8311_JOB_CLOSE, KODE_OK);
    }
    s_hush = 0U;
    s_nsub = 0U;
    (void)memset(s_sub, 0, sizeof(s_sub));
    fake_klog_reset();
    ao_spy_drain();
}

/** @brief Take everything the spy holds; how many were @p sig. */
static unsigned spy_count_sig(enum_t sig)
{
    unsigned n = 0U;

    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();

        if ( NULL == e )
        {
            break;
        }
        if ( (QSignal)sig == e->sig )
        {
            n += 1U;
        }
    }
    return n;
}

/** @brief The type of submit @p i, or a value no job has. */
static int sub_type(unsigned i)
{
    if ( i >= s_nsub )
    {
        return -1;
    }
    return (int)s_sub[i].type;
}

/** @brief Was any job of @p type submitted? */
static bool submitted(kdrv_es8311_job_t type)
{
    for ( unsigned i = 0U; i < s_nsub; ++i )
    {
        if ( s_sub[i].type == type )
        {
            return true;
        }
    }
    return false;
}

/** @brief A window with nothing wrong with it: quiet, moving, unclipped. */
static void quiet_floor(void)
{
    (void)memset(&s_res, 0, sizeof(s_res));
    s_res.win.rms_x10  = -600;
    s_res.win.peak_x10 = -500;
    for ( uint8_t b = 0U; (uint8_t)KTONE_BIN_N > b; ++b )
    {
        s_res.win.bin_x10[b] = -800;
    }
}

/**
 * @brief A window in which the tone at @p bin came back, or did not.
 *
 * Judged by the real ktone, so these are the levels a board would have to
 * produce rather than a flag this file sets.
 */
static void probe_window(uint8_t bin, bool loud)
{
    (void)memset(&s_res, 0, sizeof(s_res));
    s_res.bin = bin;
    for ( uint8_t b = 0U; (uint8_t)KTONE_BIN_N > b; ++b )
    {
        s_res.win.bin_x10[b] = -800;
    }
    if ( loud )
    {
        s_res.win.bin_x10[bin] = -200;
    }
    else
    {
        s_res.win.bin_x10[bin] = -750;
    }
    s_res.win.peak_bin = bin;
    s_res.win.rms_x10  = -400;
}

/**
 * @brief Let the row read register 0x44 and see @p val there.
 *
 * The field table is real here, so the loopback check reaches the chip as an
 * ordinary register read - which is what the mock covers.
 *
 * @param val What the register should read.
 */
static void expect_loopback(uint8_t val)
{
    static uint8_t byte;

    byte = val;
    kdrv_es8311_present_ExpectAndReturn(true);
    kdrv_es8311_reg_read_ExpectAndReturn(0x44U, NULL, KODE_OK);
    kdrv_es8311_reg_read_IgnoreArg_val();
    kdrv_es8311_reg_read_ReturnThruPtr_val(&byte);
}

/** @brief Open the codec and get past step 1, leaving step 2 waiting. */
static void row_to_step2(void)
{
    audio_test_request();
    (void)run();
    expect_loopback((uint8_t)KDRV_ES8311_DAC2ADC_OFF);
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    quiet_floor();
    finish(KDRV_ES8311_JOB_FLOOR, KODE_OK);
}

/**
 * @brief Did a verdict go out, and was it a pass?
 *
 * Consumes everything up to and including it. The spy holds the references
 * until ao_spy_drain(), so nothing here releases one.
 *
 * @param ok Out: the verdict, when there was one.
 * @return true when a TEST_RESULT_SIG named "audio" was waiting.
 */
static bool verdict(bool *ok)
{
    for ( ;; )
    {
        const QEvt *e = ao_spy_take();
        if ( NULL == e )
        {
            return false;
        }
        if ( TEST_RESULT_SIG == e->sig )
        {
            TestResultEvt const * const r = (TestResultEvt const *)e;
            TEST_ASSERT_EQUAL_STRING("audio", r->name);
            *ok = r->ok;
            return true;
        }
    }
}

/**
 * @brief Take the outcome a recording to a file published, if there was one.
 *
 * Consumes everything up to and including it, exactly as verdict() does, and
 * the spy holds the reference until ao_spy_drain().
 *
 * @return The outcome, or NULL when none was published.
 */
static AudioFileRspEvt const *file_rsp(void)
{
    for ( ;; )
    {
        const QEvt *e = ao_spy_take();
        if ( NULL == e )
        {
            return NULL;
        }
        if ( AUDIO_FILE_RSP_SIG == e->sig )
        {
            return (AudioFileRspEvt const *)e;
        }
    }
}

/** @brief How many captured lines contain @p word. */
static unsigned said(const char *word)
{
    unsigned n = 0U;

    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, word) != NULL) )
        {
            n += 1U;
        }
    }
    return n;
}

/**
 * @brief How many AUDIO_TEST_SIG records of @p phase were published.
 *
 * Drains the spy, so a case calls it once.
 *
 * @param phase AUDIO_TEST_*
 * @return How many there were.
 */
static unsigned records(uint8_t phase)
{
    unsigned n = 0U;

    for ( ;; )
    {
        const QEvt *e = ao_spy_take();
        if ( NULL == e )
        {
            break;
        }
        if ( AUDIO_TEST_SIG == e->sig )
        {
            AudioTestEvt const * const t = (AudioTestEvt const *)e;
            if ( t->phase == phase )
            {
                n += 1U;
            }
        }
    }
    return n;
}

/* The first case in the file, and it has to be: see audio_up_raw(). The
 * amplifier's enable is the C5's pin, and a board that holds its C5 in reset
 * has no amplifier until AO_Radio has let the C5 go and it has booted. */
TEST_CASE("the first sound of a boot asks for the C5 and waits for it",
          "[audio_ao]")
{
    unsigned asked = 0U;

    audio_up_raw();
    audio_tone_request(1020U, 300U, 25U);
    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nsub,
                                   "the codec was opened into an amplifier "
                                   "that is not there yet");
    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();

        if ( NULL == e )
        {
            break;
        }
        if ( C5_RUN_REQ_SIG == e->sig )
        {
            asked += 1U;
        }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, asked, "nobody was asked for the C5");

    /* The activity holds the codec while it waits, like any other. */
    audio_tone_request(2040U, 300U, 25U);
    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nsub,
                                   "a second sound got past the first one's "
                                   "wait");

    c5_running();
    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_nsub, "the open never followed");
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_OPEN, sub_type(0U));

    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KDRV_ES8311_JOB_TONE, sub_type(1U),
                                  "the sound that was asked for is the one "
                                  "that plays");
    TEST_ASSERT_EQUAL_UINT(300U, s_sub[1].ms);
    finish(KDRV_ES8311_JOB_TONE, KODE_OK);

    /* A close gives the amplifier back, so the next sound asks again. */
    codec_at_rest();
    audio_tone_request(1020U, 100U, 25U);
    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nsub,
                                   "a sound after a close opened the codec "
                                   "into an amplifier that is off");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, spy_count_sig(C5_RUN_REQ_SIG),
                                   "the amplifier was not asked for again");
    c5_running();
    (void)run();
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_OPEN, sub_type(0U));
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    finish(KDRV_ES8311_JOB_TONE, KODE_OK);
}

TEST_CASE("a closed codec asks for the amplifier off", "[audio_ao]")
{
    static const QEvt sleeping = QEVT_INITIALIZER(POWER_SHIP_SIG);

    audio_up();
    ao_spy_subscribe(C5_AMP_OFF_SIG);
    audio_tone_request(1020U, 100U, 25U);
    (void)run();
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    finish(KDRV_ES8311_JOB_TONE, KODE_OK);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, spy_count_sig(C5_AMP_OFF_SIG),
                                   "the amplifier was taken away while the "
                                   "codec was open");

    QASM_DISPATCH(AO_Audio, &sleeping, 0U);
    (void)run();
    finish(KDRV_ES8311_JOB_CLOSE, KODE_OK);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, spy_count_sig(C5_AMP_OFF_SIG),
                                   "the amplifier stayed on over a closed "
                                   "codec, playing its idle noise");
}

TEST_CASE("AO_Audio owns the audio path and says so by name", "[audio_ao]")
{
    audio_up();
    TEST_ASSERT_EQUAL_STRING("AO_Audio", kbsp_owner_name(KBSP_RES_AUDIO));
    TEST_ASSERT_EQUAL_STRING("audio (ES8311 + NS4150B)",
                             kbsp_res_name(KBSP_RES_AUDIO));
}

TEST_CASE("the boot defaults are what an open will apply", "[audio_ao]")
{
    /* Volume 100 is exactly zero decibels on this board, which is the whole
     * point of the hw_gain the driver sets: anything above it would be
     * digital gain, and the DAC does not have the headroom. */
    kes_settings_t s;

    audio_up();
    kes_settings(&s);
    TEST_ASSERT_EQUAL_UINT8(KODE_AUDIO_VOL_DEFAULT, s.vol);
    TEST_ASSERT_EQUAL_UINT8(KODE_AUDIO_MIC_DB_DEFAULT, s.mic_db);
    TEST_ASSERT_FALSE(s.out_mute);
    TEST_ASSERT_FALSE(s.in_mute);
}

TEST_CASE("a tone with the codec closed opens it first, then emits",
          "[audio_ao]")
{
    audio_up();
    audio_tone_request(1020U, 300U, 25U);
    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_nsub, "one job, not two");
    TEST_ASSERT_EQUAL_INT_MESSAGE(KDRV_ES8311_JOB_OPEN, sub_type(0U),
                                  "the codec is opened before it is used");

    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(2U, s_nsub);
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_TONE, sub_type(1U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(300U, s_sub[1].ms, "for as long as asked");

    finish(KDRV_ES8311_JOB_TONE, KODE_OK);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_nsub, "and nothing else");
}

TEST_CASE("a tone's frequency is snapped to a candidate before it is emitted",
          "[audio_ao]")
{
    /* 1500 Hz is not one of the eight, and what is emitted is the nearest
     * that is - snapped by the real ktone, not by a number this file made up. */
    audio_up();
    audio_tone_request(1500U, 100U, 10U);
    (void)run();
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(ktone_bin_nearest(1500U), s_sub[1].bin,
                                   "the emitted bin is the snapped one");
    TEST_ASSERT_NOT_EQUAL_UINT16_MESSAGE(1500U,
                                         ktone_bin_hz(s_sub[1].bin),
                                         "and it really is a different one");
    finish(KDRV_ES8311_JOB_TONE, KODE_OK);
}

TEST_CASE("a second request while one is in flight is refused, not queued",
          "[audio_ao]")
{
    audio_up();
    audio_tone_request(1020U, 300U, 25U);
    (void)run();
    TEST_ASSERT_EQUAL_UINT(1U, s_nsub);

    audio_tone_request(1250U, 300U, 25U);
    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_nsub,
                                   "the codec is busy; nothing was queued");

    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_nsub,
                                   "and the refused one did not come back");
    finish(KDRV_ES8311_JOB_TONE, KODE_OK);
}

TEST_CASE("a codec job is a job on the worker and its answer is one done event",
          "[audio_ao]")
{
    audio_up();
    s_hold = false;   /* this case runs the worker itself */

    TEST_ASSERT_TRUE_MESSAGE(fake_khal_worker_last_psram_for(WORKER_NAME),
                             "the codec worker's stack is in PSRAM");

    audio_tone_request(1020U, 300U, 25U);
    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U,
        fake_khal_worker_pending_for(WORKER_NAME),
        "the open is a job waiting for the worker");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nsub,
        "and the driver has not been asked anything yet");

    TEST_ASSERT_TRUE(fake_khal_worker_run_next_for(WORKER_NAME));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_nsub, "the job ran on the worker");
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_OPEN, sub_type(0U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U,
        fake_khal_worker_pending_for(WORKER_NAME),
        "nothing follows until the AO has the answer");

    (void)run();   /* the done event is the AO's, and the tone follows */
    TEST_ASSERT_EQUAL_UINT(1U, fake_khal_worker_pending_for(WORKER_NAME));
    TEST_ASSERT_TRUE(fake_khal_worker_run_next_for(WORKER_NAME));
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_TONE, sub_type(1U));
    TEST_ASSERT_EQUAL_UINT(300U, s_sub[1].ms);
    (void)run();

    /* The answer freed the codec: the next sound is a job at once. */
    audio_tone_request(1250U, 100U, 25U);
    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U,
        fake_khal_worker_pending_for(WORKER_NAME),
        "the codec took the next sound");
    TEST_ASSERT_TRUE(fake_khal_worker_run_next_for(WORKER_NAME));
    (void)run();
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_TONE, sub_type(2U));
}

TEST_CASE("a request while a job runs is refused and reaches no second job",
          "[audio_ao]")
{
    audio_up();
    s_hold = false;

    audio_tone_request(1020U, 300U, 25U);
    (void)run();
    TEST_ASSERT_EQUAL_UINT(1U, fake_khal_worker_pending_for(WORKER_NAME));

    audio_tone_request(1250U, 300U, 25U);
    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U,
        fake_khal_worker_pending_for(WORKER_NAME),
        "the worker holds one job, not two");
    TEST_ASSERT_NOT_EQUAL_UINT_MESSAGE(0U, said("busy"),
        "and the refusal was said");

    TEST_ASSERT_TRUE(fake_khal_worker_run_next_for(WORKER_NAME));
    (void)run();
    TEST_ASSERT_TRUE(fake_khal_worker_run_next_for(WORKER_NAME));
    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_nsub, "the open and the first tone");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(ktone_bin_nearest(1020U), s_sub[1].bin,
                                    "the refused one did not come back");
    TEST_ASSERT_EQUAL_UINT(0U, fake_khal_worker_pending_for(WORKER_NAME));
}

TEST_CASE("an answer carrying another job's sequence number is dropped",
          "[audio_ao]")
{
    KqpWorkerDoneEvt *foreign;

    audio_up();
    audio_tone_request(1020U, 300U, 25U);
    (void)run();
    TEST_ASSERT_NOT_NULL(s_held);
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_OPEN, s_held_req.type);

    /* An answer to a job this AO is not waiting for: the open's number plus
     * one. It must end nothing and free nothing. */
    foreign = Q_NEW(KqpWorkerDoneEvt, AUDIO_JOB_SIG);
    foreign->seq    = (uint16_t)(((KqpWorkerDoneEvt const *)(void const *)s_held)->seq + 1U);
    foreign->status = (uint8_t)KODE_OK;
    foreign->value  = 0U;
    QASM_DISPATCH(AO_Audio, &foreign->super, 0U);
    QF_gc(&foreign->super);
    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_nsub,
        "a stranger's answer opened the codec's next step");
    TEST_ASSERT_EQUAL_UINT(0U, fake_khal_worker_pending_for(WORKER_NAME));

    audio_tone_request(1250U, 100U, 25U);
    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_nsub,
        "the job is still with the worker, so the codec is still busy");

    /* The right answer, and the sound that was asked for follows. */
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_TONE, sub_type(1U));
    TEST_ASSERT_EQUAL_UINT(300U, s_sub[1].ms);
    finish(KDRV_ES8311_JOB_TONE, KODE_OK);
}

static void fill_info(kdrv_es8311_info_t *out, int ncalls)
{
    (void)ncalls;
    (void)memset(out, 0, sizeof(*out));
    out->present = true;
}

/** @brief The `busy` of one info question, asked of the AO. */
static bool info_busy(void)
{
    kdrv_es8311_info_t info;
    khal_sem_t         sem = khal_sem_create();
    uint32_t           ack = 0U;

    (void)memset(&info, 0, sizeof(info));
    kdrv_es8311_info_StubWithCallback(&fill_info);
    audio_info_request(&info, sem, 7U, &ack);
    (void)run();
    TEST_ASSERT_EQUAL_UINT32(7U, ack);
    return info.busy;
}

/** @brief The value of the `busy` field, asked of the AO. */
static int32_t field_busy(void)
{
    audio_field_request(FIELD_OP_GET, (uint16_t)KDRV_ES_BUSY, 0);
    (void)run();
    for ( ;; )
    {
        const QEvt *e = ao_spy_take();
        TEST_ASSERT_NOT_NULL_MESSAGE(e, "the field question went unanswered");
        if ( AUDIO_FIELD_SIG == e->sig )
        {
            AudioFieldEvt const * const a = (AudioFieldEvt const *)e;
            TEST_ASSERT_EQUAL_UINT(KDRV_ES_BUSY, a->field);
            TEST_ASSERT_EQUAL_UINT(KODE_OK, a->rc);
            return a->value;
        }
    }
}

TEST_CASE("busy is the AO's: a job with the worker, from submit to its answer",
          "[audio_ao]")
{
    audio_up();
    TEST_ASSERT_FALSE(info_busy());
    TEST_ASSERT_EQUAL_INT32(0, field_busy());

    audio_tone_request(1020U, 100U, 25U);
    (void)run();
    TEST_ASSERT_TRUE_MESSAGE(info_busy(), "the open is with the worker");
    TEST_ASSERT_EQUAL_INT32(1, field_busy());

    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    TEST_ASSERT_TRUE_MESSAGE(info_busy(), "and so is the tone after it");
    finish(KDRV_ES8311_JOB_TONE, KODE_OK);
    TEST_ASSERT_FALSE(info_busy());
    TEST_ASSERT_EQUAL_INT32(0, field_busy());
    ao_spy_drain();
}

TEST_CASE("a refused recording leaves the play in flight alone", "[audio_ao]")
{
    static int16_t         frames[8];
    uint32_t               frc   = 0xA5A5A5A5U;
    uint32_t               fack  = 0xA5A5A5A5U;
    uint32_t               bytes = 0xA5A5A5A5U;
    AudioFileRspEvt const *r;
    AudioDoneEvt const    *d;

    audio_up();

    /* An AO's play, its answer owed once the sound is heard. The codec is
     * closed, so this opens first and the play is still owed. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, audio_play_ask(ao_spy(), frames, 8U));
    (void)run();
    TEST_ASSERT_EQUAL_UINT(1U, s_nsub);

    /* And `audio_rec ms= file=` lands on top of it. This is the one that used
     * to write the AO's wait record, path and byte counters BEFORE asking
     * whether the codec was free, and then end the activity it had just
     * overwritten - leaving a real job in the worker with no guard and an
     * AO that believed it was idle. */
    audio_record_file_request("/storage/x.wav", 2000U, &frc, &bytes, NULL,
                              3U, &fack);
    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_nsub,
                                   "the codec is busy; nothing was queued");

    /* It is answered through its own fields, and on the bus, because the
     * console is holding a command open on the published outcome. */
    TEST_ASSERT_EQUAL_HEX32_MESSAGE((uint32_t)KODE_DENIED, frc,
                                    "the refusal reached whoever asked for it");
    TEST_ASSERT_EQUAL_UINT32(3U, fack);
    TEST_ASSERT_EQUAL_UINT32(0U, bytes);
    r = file_rsp();
    TEST_ASSERT_NOT_NULL_MESSAGE(r, "and the console was never answered");
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)KODE_DENIED, (unsigned)r->rc,
                                   "the outcome on the bus is the refusal");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("/storage/x.wav", r->path,
                                     "named by the request's own path");
    TEST_ASSERT_EQUAL_UINT32(0U, r->bytes);
    TEST_ASSERT_NULL_MESSAGE(ao_spy_take(),
                             "and the play is not answered before its job has run");

    /* And the codec still reads busy. A refusal that marked it idle would
     * let the next request start a second job on top of the first. */
    audio_tone_request(1020U, 10U, 25U);
    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_nsub,
                                   "the play is still the activity in flight");

    /* The job the play's asker waits on runs to the end, and answers it. */
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_nsub,
                                   "the open was followed by the play");
    finish(KDRV_ES8311_JOB_PLAY, KODE_OK);
    d = (AudioDoneEvt const *)ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(d, "the play's asker was answered");
    TEST_ASSERT_EQUAL_INT((int)AUDIO_DONE_SIG, (int)d->super.sig);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)KODE_OK, (unsigned)d->rc,
                                   "with the play it asked for");
    ao_spy_drain();
}

TEST_CASE("a refused recording leaves the guard on the job in flight armed",
          "[audio_ao]")
{
    static int16_t      frames[8];
    AudioDoneEvt const *d;

    audio_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, audio_play_ask(ao_spy(), frames, 8U));
    (void)run();
    TEST_ASSERT_EQUAL_UINT(1U, s_nsub);

    audio_record_file_request("/storage/x.wav", 2000U, NULL, NULL, NULL,
                              0U, NULL);
    (void)run();
    TEST_ASSERT_EQUAL_UINT(1U, s_nsub);
    TEST_ASSERT_NOT_NULL(file_rsp());

    /* The guard belongs to the job with the worker, never to whatever was
     * refused on top of it. Disarming it there is how a wedged worker came
     * to leave an asker waiting with nothing left to answer it. */
    (void)qf_support_tick(AO_Audio, GUARD_TICKS + 1U);
    d = (AudioDoneEvt const *)ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(d, "the guard on the job in flight still fired");
    TEST_ASSERT_EQUAL_INT((int)AUDIO_DONE_SIG, (int)d->super.sig);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)KODE_TIMEOUT, (unsigned)d->rc,
                                   "and answered the asker it belonged to");
    ao_spy_drain();
}

TEST_CASE("the codec opens once and is never closed by a quiet spell",
          "[audio_ao]")
{
    /* The decision of 2026-09-16. Every close powers the analog blocks down
     * into an amplifier the C5 keeps enabled, and that was the pop the owner
     * heard two seconds after every sound. What must not be live at rest is
     * the CONVERTER, and the driver mutes that between sounds - so the close
     * bought nothing and cost the one thing anybody could hear.
     *
     * There is no idle timer left to test the absence of, which is the point:
     * what is asserted here is that no amount of quiet produces a job. */
    audio_up();
    audio_tone_request(1020U, 10U, 25U);
    (void)run();
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_OPEN, sub_type(0U));
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    finish(KDRV_ES8311_JOB_TONE, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(2U, s_nsub);

    (void)qf_support_tick(AO_Audio, QUIET_TICKS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_nsub,
        "nothing closes the codec for being idle any more");
}

TEST_CASE("a second sound finds the device open and is played, not opened",
          "[audio_ao]")
{
    /* The other half of the same decision, and what a burst of
     * fire-and-forget sounds costs: one open for the life of the board. */
    audio_up();
    audio_tone_request(1020U, 10U, 25U);
    (void)run();
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_OPEN, sub_type(0U));
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    finish(KDRV_ES8311_JOB_TONE, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(2U, s_nsub);

    (void)qf_support_tick(AO_Audio, QUIET_TICKS);
    audio_tone_request(1020U, 10U, 25U);
    (void)run();
    TEST_ASSERT_EQUAL_UINT(3U, s_nsub);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KDRV_ES8311_JOB_TONE, sub_type(2U),
        "still open however long the board has been quiet, so no second open");
    finish(KDRV_ES8311_JOB_TONE, KODE_OK);
}

TEST_CASE("the board going to sleep mutes the codec and then closes it",
          "[audio_ao]")
{
    /* The one close left. The hush comes first and is synchronous, because on
     * battery the rails go while the worker is still waiting out a ramp; the
     * close behind it is what makes the USB case tidy, where the write fails
     * and the board stays up with the codec put away. */
    static const QEvt sleeping = QEVT_INITIALIZER(POWER_SHIP_SIG);

    audio_up();
    audio_tone_request(1020U, 10U, 25U);
    (void)run();
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    finish(KDRV_ES8311_JOB_TONE, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(2U, s_nsub);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_hush,
                                   "a sound does not hush the codec for good");

    QASM_DISPATCH(AO_Audio, &sleeping, 0U);
    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_hush,
        "the converter is silenced in the step, not handed to the worker");
    TEST_ASSERT_EQUAL_UINT(3U, s_nsub);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KDRV_ES8311_JOB_CLOSE, sub_type(2U),
                                  "a board that is switching off puts it away");
    finish(KDRV_ES8311_JOB_CLOSE, KODE_OK);

    /* And it really is closed: the next sound asks for the amplifier and
     * opens it again. */
    audio_tone_request(1020U, 10U, 25U);
    (void)run();
    TEST_ASSERT_EQUAL_UINT(3U, s_nsub);
    c5_running();
    (void)run();
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_OPEN, sub_type(3U));
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    finish(KDRV_ES8311_JOB_TONE, KODE_OK);
}

TEST_CASE("the row opens, measures a floor, and emits NOTHING before it does",
          "[audio_ao]")
{
    bool ok = false;

    audio_up();
    audio_test_request();
    (void)run();
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_OPEN, sub_type(0U));

    expect_loopback((uint8_t)KDRV_ES8311_DAC2ADC_OFF);
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);

    TEST_ASSERT_EQUAL_INT_MESSAGE(KDRV_ES8311_JOB_FLOOR, sub_type(1U),
                                  "step 1 is a capture");
    TEST_ASSERT_FALSE_MESSAGE(submitted(KDRV_ES8311_JOB_TONE),
                              "nothing was emitted before the floor");
    TEST_ASSERT_FALSE_MESSAGE(submitted(KDRV_ES8311_JOB_PROBE),
                              "nothing was emitted before the floor");
    TEST_ASSERT_FALSE(verdict(&ok));
}

TEST_CASE("the floor's records carry every bin, and then the tones follow",
          "[audio_ao]")
{
    audio_up();
    row_to_step2();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, records(AUDIO_TEST_MIC),
                                   "one summary for step 1");
    audio_up();
    row_to_step2();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(KTONE_BIN_N, records(AUDIO_TEST_BIN),
                                   "and one record per candidate");
}

TEST_CASE("the loopback register is read before anything is measured",
          "[audio_ao]")
{
    /* With the chip's DAC routed into its own ADC, a pass would mean nothing
     * at all - a dead microphone and a dead speaker would both give one. */
    bool ok = true;

    audio_up();
    audio_test_request();
    (void)run();
    expect_loopback(0x58U);
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);

    TEST_ASSERT_FALSE_MESSAGE(submitted(KDRV_ES8311_JOB_FLOOR),
                              "not even the floor is measured through a loopback");
    TEST_ASSERT_TRUE(verdict(&ok));
    TEST_ASSERT_FALSE(ok);
}

TEST_CASE("the loopback is read even when the row finds the codec open",
          "[audio_ao]")
{
    /* The warm path, and since 2026-09-16 it is the NORMAL one: the codec is
     * already open when a row starts, so no OPEN_DONE is coming to carry the
     * check that used to ride on one. A check that only ran on the cold path
     * would be a check that had quietly stopped running, and this row would
     * go back to being able to pass with a dead microphone and a dead
     * speaker. */
    bool ok = true;

    audio_up();
    audio_tone_request(1020U, 10U, 25U);
    (void)run();
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    finish(KDRV_ES8311_JOB_TONE, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(2U, s_nsub);

    expect_loopback(0x58U);
    audio_test_request();
    (void)run();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_nsub,
        "nothing is measured through a loopback, open device or not");
    TEST_ASSERT_TRUE(verdict(&ok));
    TEST_ASSERT_FALSE(ok);
}

TEST_CASE("a floor that is not moving fails the row and names the microphone",
          "[audio_ao]")
{
    bool ok = true;

    audio_up();
    audio_test_request();
    (void)run();
    expect_loopback((uint8_t)KDRV_ES8311_DAC2ADC_OFF);
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);

    quiet_floor();
    s_res.win.stuck = true;
    finish(KDRV_ES8311_JOB_FLOOR, KODE_OK);

    TEST_ASSERT_FALSE_MESSAGE(submitted(KDRV_ES8311_JOB_PROBE),
                              "no tone is emitted at a microphone that is not there");
    TEST_ASSERT_TRUE(verdict(&ok));
    TEST_ASSERT_FALSE(ok);
}

TEST_CASE("a bench too loud is a refusal to judge, not a verdict on the board",
          "[audio_ao]")
{
    bool ok = true;

    audio_up();
    audio_test_request();
    (void)run();
    expect_loopback((uint8_t)KDRV_ES8311_DAC2ADC_OFF);
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);

    quiet_floor();
    s_res.win.rms_x10 = (int16_t)(KTONE_FLOOR_MAX_DBFS_X10 + 100);
    finish(KDRV_ES8311_JOB_FLOOR, KODE_OK);

    TEST_ASSERT_FALSE(submitted(KDRV_ES8311_JOB_PROBE));
    TEST_ASSERT_TRUE(verdict(&ok));
    TEST_ASSERT_FALSE(ok);
}

TEST_CASE("an attempt is two different tones, and both must come back",
          "[audio_ao]")
{
    bool     ok = false;
    unsigned first;
    uint8_t  a;
    uint8_t  b;

    audio_up();
    row_to_step2();
    first = s_nsub - 1U;
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_PROBE, sub_type(first));
    a = s_sub[first].bin;

    probe_window(a, true);
    finish(KDRV_ES8311_JOB_PROBE, KODE_OK);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(first + 2U, s_nsub,
                                   "one tone is not an attempt");
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_PROBE, sub_type(first + 1U));
    b = s_sub[first + 1U].bin;
    TEST_ASSERT_TRUE_MESSAGE(a != b,
        "the two tones of an attempt differ, or a steady interferer passes both");

    probe_window(b, true);
    finish(KDRV_ES8311_JOB_PROBE, KODE_OK);
    TEST_ASSERT_TRUE(verdict(&ok));
    TEST_ASSERT_TRUE_MESSAGE(ok, "both tones came back");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(first + 2U, s_nsub,
                                   "and no third tone was emitted");
}

TEST_CASE("a tone that does not come back costs the attempt, not the run",
          "[audio_ao]")
{
    bool     ok = true;
    unsigned first;
    uint8_t  drawn[2U * KODE_AUDIO_TEST_TRIES];
    unsigned n = 0U;

    audio_up();
    row_to_step2();
    first = s_nsub - 1U;

    /* Three attempts, each failing on its first tone. Every one of them draws
     * a fresh pair, which is what makes three attempts no easier to fake than
     * one. */
    for ( unsigned t = 0U; KODE_AUDIO_TEST_TRIES > t; ++t )
    {
        uint8_t const bin = s_sub[s_nsub - 1U].bin;
        drawn[n] = bin;
        n += 1U;
        probe_window(bin, false);
        finish(KDRV_ES8311_JOB_PROBE, KODE_OK);
    }
    TEST_ASSERT_TRUE(verdict(&ok));
    TEST_ASSERT_FALSE_MESSAGE(ok, "nothing came back in three attempts");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(first + KODE_AUDIO_TEST_TRIES, s_nsub,
                                   "one tone per attempt, and then it stops");
}

TEST_CASE("a job that never answers is a failure with a name, not silence",
          "[audio_ao]")
{
    bool ok = true;

    audio_up();
    audio_test_request();
    (void)run();
    TEST_ASSERT_EQUAL_UINT(1U, s_nsub);

    (void)qf_support_tick(AO_Audio, GUARD_TICKS - 1U);
    TEST_ASSERT_FALSE_MESSAGE(verdict(&ok), "the job has its whole guard");
    (void)qf_support_tick(AO_Audio, 2U);
    TEST_ASSERT_TRUE_MESSAGE(verdict(&ok), "and then it is a verdict");
    TEST_ASSERT_FALSE(ok);
}

TEST_CASE("an outcome arriving after its guard fired ends nothing", "[audio_ao]")
{
    bool ok = true;

    audio_up();
    audio_test_request();
    (void)run();
    (void)qf_support_tick(AO_Audio, GUARD_TICKS + 1U);
    TEST_ASSERT_TRUE(verdict(&ok));
    TEST_ASSERT_FALSE(ok);

    /* The worker answers late. There is no row any more, so this is nobody's
     * and must not start one. */
    unsigned const before = s_nsub;
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before, s_nsub,
                                   "a late outcome started nothing");
    TEST_ASSERT_FALSE(verdict(&ok));
}

TEST_CASE("a stale guard expiry does not time out the job that replaced it",
          "[audio_ao]")
{
    bool ok = true;

    audio_up();
    audio_test_request();
    (void)run();
    TEST_ASSERT_EQUAL_UINT(1U, s_nsub);

    /* Sequencing the race the stale counters exist for. qf_support_tick()
     * ticks AND dispatches, so the raw ticks below post the expiry and leave
     * it in the queue; the direct dispatch that follows is the step the board
     * runs while the tick task posts underneath it. */
    (void)qf_support_tick(AO_Audio, GUARD_TICKS - 1U);
    TEST_ASSERT_FALSE_MESSAGE(verdict(&ok), "the job still has its guard");
    QTIMEEVT_TICK_X(0U, (void *)0);
    QTIMEEVT_TICK_X(0U, (void *)0);

    /* The worker answers in that same moment. job_done() stops the guard, so
     * the expiry already in the queue belongs to nothing, and the row arms a
     * fresh guard for the floor it submits. */
    expect_loopback((uint8_t)KDRV_ES8311_DAC2ADC_OFF);
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_OPEN, s_held_req.type);
    deliver(KODE_OK);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U,
        fake_khal_worker_pending_for(WORKER_NAME),
        "the row moved on to its floor");

    (void)run();   /* and now the stale expiry is delivered */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_nsub, "the floor reached the driver");
    TEST_ASSERT_FALSE_MESSAGE(verdict(&ok), "a stale expiry ended the row");

    /* The second round is the one that pays for the first. Swallowing a stale
     * expiry must leave the guard it did NOT belong to still marked live: a
     * handler that cleared the flag here would stop guard_stop() counting,
     * and this expiry - stale in exactly the same way - would be taken as
     * genuine and time out a job that is answering normally. */
    /* One tick fewer than the first round: the run() above is itself a tick,
     * and the floor's guard has been counting since the dispatch before it. */
    (void)qf_support_tick(AO_Audio, GUARD_TICKS - 2U);
    TEST_ASSERT_FALSE_MESSAGE(verdict(&ok), "the floor still has its guard");
    QTIMEEVT_TICK_X(0U, (void *)0);
    QTIMEEVT_TICK_X(0U, (void *)0);

    quiet_floor();
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_FLOOR, s_held_req.type);
    deliver(KODE_OK);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U,
        fake_khal_worker_pending_for(WORKER_NAME),
        "the row moved on to its tones");

    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, s_nsub, "the first tone reached the driver");
    TEST_ASSERT_FALSE_MESSAGE(verdict(&ok),
        "a stale guard expiry timed out the job that replaced the one it "
        "was guarding");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, s_nsub, "and started nothing");

    /* And the row is put down before the case ends. The AO is constructed
     * once for the whole binary, so a case that walks away from a running row
     * leaves the next one asking a busy AO to start another. */
    (void)qf_support_tick(AO_Audio, GUARD_TICKS + 1U);
    TEST_ASSERT_TRUE_MESSAGE(verdict(&ok), "the tone's own guard ends the row");
    TEST_ASSERT_FALSE(ok);
}

TEST_CASE("a second row while one is running is ignored, not started",
          "[audio_ao]")
{
    audio_up();
    audio_test_request();
    (void)run();
    TEST_ASSERT_EQUAL_UINT(1U, s_nsub);
    audio_test_request();
    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_nsub, "one row at a time");
}

TEST_CASE("a field is read, written and dumped, mid-row included", "[audio_ao]")
{
    /* The table is real here, so a field question reaches the chip as an
     * ordinary register read - and the row in flight for the whole of this
     * case is the point: a question about the codec is answered whatever the
     * codec is doing. */
    static uint8_t reg32 = 169U;

    audio_up();
    row_to_step2();

    kdrv_es8311_present_IgnoreAndReturn(true);
    kdrv_es8311_is_open_IgnoreAndReturn(true);
    kdrv_es8311_apply_settings_Ignore();
    kdrv_es8311_reg_read_ExpectAndReturn(0x32U, NULL, KODE_OK);
    kdrv_es8311_reg_read_IgnoreArg_val();
    kdrv_es8311_reg_read_ReturnThruPtr_val(&reg32);

    audio_field_request(FIELD_OP_GET, (uint16_t)KDRV_ES_REG32, 0);
    (void)run();

    for ( ;; )
    {
        const QEvt *e = ao_spy_take();
        TEST_ASSERT_NOT_NULL_MESSAGE(e, "the field question went unanswered");
        if ( AUDIO_FIELD_SIG == e->sig )
        {
            AudioFieldEvt const * const a = (AudioFieldEvt const *)e;
            TEST_ASSERT_EQUAL_UINT(KDRV_ES_REG32, a->field);
            TEST_ASSERT_EQUAL_INT_MESSAGE(169, a->value,
                "0xA9 is what volume 100 writes under the speaker's cap");
            TEST_ASSERT_EQUAL_UINT(KODE_OK, a->rc);
            break;
        }
    }

    /* A write reads back rather than echoing, so a field that ROUNDS reports
     * what the chip will actually do: the microphone gain has six-decibel
     * steps and 26 is not one of them. */
    audio_field_request(FIELD_OP_SET, (uint16_t)KDRV_ES_MIC_DB, 26);
    (void)run();
    for ( ;; )
    {
        const QEvt *e = ao_spy_take();
        TEST_ASSERT_NOT_NULL(e);
        if ( AUDIO_FIELD_SIG == e->sig )
        {
            AudioFieldEvt const * const a = (AudioFieldEvt const *)e;
            TEST_ASSERT_EQUAL_UINT(KDRV_ES_MIC_DB, a->field);
            TEST_ASSERT_EQUAL_INT_MESSAGE(24, a->value,
                                          "rounded down to a step the chip has");
            break;
        }
    }

    /* And a field that cannot be written says so rather than pretending. */
    audio_field_request(FIELD_OP_SET, (uint16_t)KDRV_ES_CHD1, 1);
    (void)run();
    for ( ;; )
    {
        const QEvt *e = ao_spy_take();
        TEST_ASSERT_NOT_NULL(e);
        if ( AUDIO_FIELD_SIG == e->sig )
        {
            AudioFieldEvt const * const a = (AudioFieldEvt const *)e;
            TEST_ASSERT_EQUAL_UINT(KODE_DENIED, a->rc);
            break;
        }
    }
    ao_spy_drain();
}

#define SETTING_LOOP 64U        /**< Requests a caller looping on a setting's door makes. */
#define SETTING_LAST_MIC_DB 24U /**< The microphone gain the loop ends on, dB.            */
#define SETTING_LAST_VOLUME 40U /**< The volume it ends on.                               */
#define SETTING_LAST_MUTE 1U    /**< The mute it ends on.                                 */
#define SETTING_MIC_STEP_DB 6U  /**< Step of the microphone gain the loop walks, dB.      */
#define SETTING_MIC_STEPS 7U    /**< How many steps it walks through.                     */

TEST_CASE("a loop on a setting's door queues one request each and the last value is applied",
          "[audio_ao]")
{
    uint32_t seen = 0U;

    audio_up();
    kdrv_es8311_present_IgnoreAndReturn(true);
    kdrv_es8311_is_open_IgnoreAndReturn(true);
    kdrv_es8311_apply_settings_Ignore();

    for ( uint32_t i = 0U; SETTING_LOOP > i; i++ )
    {
        audio_setting_request((uint16_t)KDRV_ES_MIC_DB, (int32_t)((i % SETTING_MIC_STEPS) * SETTING_MIC_STEP_DB));
        audio_setting_request((uint16_t)KDRV_ES_VOL, (int32_t)i);
        audio_setting_request((uint16_t)KDRV_ES_OUT_MUTE, (int32_t)(i % 2U));
    }
    audio_setting_request((uint16_t)KDRV_ES_MIC_DB, (int32_t)SETTING_LAST_MIC_DB);
    audio_setting_request((uint16_t)KDRV_ES_VOL, (int32_t)SETTING_LAST_VOLUME);
    audio_setting_request((uint16_t)KDRV_ES_OUT_MUTE, (int32_t)SETTING_LAST_MUTE);
    TEST_ASSERT_EQUAL_UINT32(3U, (uint32_t)uxQueueMessagesWaiting(AO_Audio->eQueue));

    (void)run();
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Audio->eQueue));
    for ( ;; )
    {
        const QEvt *e = ao_spy_take();

        if ( NULL == e )
        {
            break;
        }
        if ( AUDIO_FIELD_SIG == e->sig )
        {
            AudioFieldEvt const * const a = (AudioFieldEvt const *)e;

            TEST_ASSERT_EQUAL_UINT(KODE_OK, a->rc);
            if ( (uint16_t)KDRV_ES_MIC_DB == a->field )
            {
                TEST_ASSERT_EQUAL_INT32(SETTING_LAST_MIC_DB, a->value);
            }
            else if ( (uint16_t)KDRV_ES_VOL == a->field )
            {
                TEST_ASSERT_EQUAL_INT32(SETTING_LAST_VOLUME, a->value);
            }
            else
            {
                TEST_ASSERT_EQUAL_UINT16((uint16_t)KDRV_ES_OUT_MUTE, a->field);
                TEST_ASSERT_EQUAL_INT32(SETTING_LAST_MUTE, a->value);
            }
            seen++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3U, seen, "each setting is answered once");

    /* Taken, the door opens again: the next value is queued afresh. */
    audio_setting_request((uint16_t)KDRV_ES_VOL, 0);
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)uxQueueMessagesWaiting(AO_Audio->eQueue));
    (void)run();
    ao_spy_drain();
}

TEST_CASE("a setting's door ignores a field it has no slot for", "[audio_ao]")
{
    audio_up();
    audio_setting_request((uint16_t)KDRV_ES_CHD1, 1);
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Audio->eQueue));
}

TEST_CASE("a write whose readback fails is answered with the read's outcome",
          "[audio_ao]")
{
    /* The table is real here, so a byte field is a read-modify-write plus a
     * verify read at the chip, and then the answer reads the field back once
     * more to report what the chip actually kept. It is that last read that
     * fails: the write landed, and there is still no reading to report. */
    static uint8_t raw  = 0x00U;
    static uint8_t back = 0x5AU;

    audio_up();
    kdrv_es8311_present_IgnoreAndReturn(true);
    kdrv_es8311_is_open_IgnoreAndReturn(true);
    kdrv_es8311_apply_settings_Ignore();

    /* Register 0x45, general purpose: a raw byte anybody may write. 0x32 is
     * read only now - it would lift the speaker past its cap. */
    kdrv_es8311_reg_read_ExpectAndReturn(0x45U, NULL, KODE_OK);
    kdrv_es8311_reg_read_IgnoreArg_val();
    kdrv_es8311_reg_read_ReturnThruPtr_val(&raw);
    kdrv_es8311_reg_write_ExpectAndReturn(0x45U, 0x5AU, KODE_OK);
    kdrv_es8311_reg_read_ExpectAndReturn(0x45U, NULL, KODE_OK);
    kdrv_es8311_reg_read_IgnoreArg_val();
    kdrv_es8311_reg_read_ReturnThruPtr_val(&back);
    kdrv_es8311_reg_read_ExpectAndReturn(0x45U, NULL, KODE_IO_ERROR);
    kdrv_es8311_reg_read_IgnoreArg_val();

    audio_field_request(FIELD_OP_SET, (uint16_t)KDRV_ES_REG45, 0x5A);
    (void)run();

    for ( ;; )
    {
        const QEvt *e = ao_spy_take();
        TEST_ASSERT_NOT_NULL_MESSAGE(e, "the field question went unanswered");
        if ( AUDIO_FIELD_SIG == e->sig )
        {
            AudioFieldEvt const * const a = (AudioFieldEvt const *)e;
            TEST_ASSERT_EQUAL_UINT(KDRV_ES_REG45, a->field);
            TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)KODE_IO_ERROR,
                (unsigned)a->rc,
                "a write nobody could read back must not answer ok");
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, a->value,
                "and it carries no reading, because none was taken");
            break;
        }
    }
    ao_spy_drain();
}

TEST_CASE("the info question is answered inside the step that receives it",
          "[audio_ao]")
{
    kdrv_es8311_info_t info;
    khal_sem_t         sem = khal_sem_create();
    uint32_t           ack = 0U;

    audio_up();
    (void)memset(&info, 0, sizeof(info));
    kdrv_es8311_info_ExpectAnyArgs();
    audio_info_request(&info, sem, 7U, &ack);
    (void)run();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(7U, ack,
                                     "the generation is stamped before the wake");
    TEST_ASSERT_TRUE_MESSAGE(khal_sem_wait_timeout(sem, 0U),
                             "and the caller was woken");
}

/* ---- The stream ------------------------------------------------------------ */

/**
 * @brief AO_Audio up and holding nothing, whatever the case before left.
 *
 * A stream needs the codec free, and the cases above leave a row or a job in
 * flight on purpose. Its guard ends whatever that was; then the codec is put
 * back to rest and the counters start from zero.
 */
static void stream_ready(void)
{
    audio_up();
    (void)qf_support_tick(AO_Audio, GUARD_TICKS + 1U);
    (void)run();
    codec_at_rest();
    s_stream_resets = 0U;
    s_stream_ends   = 0U;
}

TEST_CASE("a stream opens the codec, starts its job and answers once it plays",
          "[audio_ao]")
{
    khal_sem_t sem = khal_sem_create();
    uint32_t   rc  = 0xA5A5A5A5U;
    uint32_t   ack = 0U;

    stream_ready();
    fake_sem_reset();
    audio_stream_open_request(&rc, sem, 5U, &ack);
    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_stream_resets,
                                   "the ring is emptied before the job starts");
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_OPEN, sub_type(0U));
    TEST_ASSERT_FALSE_MESSAGE(khal_sem_wait_timeout(sem, 0U),
                              "not answered before the stream is playing");

    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_STREAM, sub_type(1U));
    TEST_ASSERT_EQUAL_HEX32((uint32_t)KODE_OK, rc);
    TEST_ASSERT_EQUAL_HEX32(5U, ack);
    TEST_ASSERT_TRUE(khal_sem_wait_timeout(sem, 0U));

    /* A stream plays for as long as its writer likes: no guard runs out on
     * it while it is open. */
    (void)qf_support_tick(AO_Audio, GUARD_TICKS + 10U);
    TEST_ASSERT_EQUAL_UINT(2U, s_nsub);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_stream_ends,
                                   "an open stream is not ended by time");

    audio_stream_close_request(NULL, NULL, 0U, NULL);
    (void)run();
    finish(KDRV_ES8311_JOB_STREAM, KODE_OK);
}

TEST_CASE("while a stream is open every other sound is refused", "[audio_ao]")
{
    static int16_t frames[8];
    khal_sem_t     sem = khal_sem_create();
    uint32_t       rc  = 0U;
    uint32_t       ack = 0U;

    stream_ready();
    audio_stream_open_request(NULL, NULL, 0U, NULL);
    (void)run();
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(2U, s_nsub);

    audio_tone_request(1020U, 100U, 25U);
    (void)run();
    fake_sem_reset();
    audio_record_request(frames, 8U, NULL, &rc, sem, 7U, &ack);
    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_nsub, "nothing else reached the worker");
    TEST_ASSERT_EQUAL_HEX32((uint32_t)KODE_DENIED, rc);
    TEST_ASSERT_TRUE(khal_sem_wait_timeout(sem, 0U));

    audio_stream_open_request(&rc, sem, 8U, &ack);
    (void)run();
    TEST_ASSERT_EQUAL_HEX32_MESSAGE((uint32_t)KODE_DENIED, rc,
                                    "and so is a second stream");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_stream_resets,
        "a refused open leaves the open stream's ring alone");

    audio_stream_close_request(NULL, NULL, 0U, NULL);
    (void)run();
    finish(KDRV_ES8311_JOB_STREAM, KODE_OK);
}

TEST_CASE("closing a stream shuts its ring and answers once it has drained",
          "[audio_ao]")
{
    khal_sem_t sem = khal_sem_create();
    uint32_t   rc  = 0xA5A5A5A5U;
    uint32_t   ack = 0U;

    stream_ready();
    audio_stream_open_request(NULL, NULL, 0U, NULL);
    (void)run();
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);

    fake_sem_reset();
    audio_stream_close_request(&rc, sem, 6U, &ack);
    (void)run();
    TEST_ASSERT_EQUAL_UINT(1U, s_stream_ends);
    TEST_ASSERT_FALSE_MESSAGE(khal_sem_wait_timeout(sem, 0U),
                              "not answered while the ring is still playing");

    finish(KDRV_ES8311_JOB_STREAM, KODE_OK);
    TEST_ASSERT_EQUAL_HEX32((uint32_t)KODE_OK, rc);
    TEST_ASSERT_EQUAL_HEX32(6U, ack);
    TEST_ASSERT_TRUE(khal_sem_wait_timeout(sem, 0U));

    /* The codec is free again. */
    audio_tone_request(1020U, 10U, 25U);
    (void)run();
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_TONE, sub_type(s_nsub - 1U));
    finish(KDRV_ES8311_JOB_TONE, KODE_OK);
}

TEST_CASE("closing when no stream is open is refused", "[audio_ao]")
{
    khal_sem_t sem = khal_sem_create();
    uint32_t   rc  = 0U;
    uint32_t   ack = 0U;

    stream_ready();
    fake_sem_reset();
    audio_stream_close_request(&rc, sem, 3U, &ack);
    (void)run();
    TEST_ASSERT_EQUAL_HEX32((uint32_t)KODE_DENIED, rc);
    TEST_ASSERT_EQUAL_HEX32(3U, ack);
    TEST_ASSERT_TRUE(khal_sem_wait_timeout(sem, 0U));
    TEST_ASSERT_EQUAL_UINT(0U, s_stream_ends);
}

/* ---- Ending a recording early ---------------------------------------------- */

/** @brief Frames a recording of the cases below asks for: well past any stop. */
#define REC_FRAMES      32000U
/** @brief Frames a stopped recording is said to have captured. */
#define REC_CAPTURED     1024U
/** @brief Room the shared recording buffer reports in the file cases. */
#define REC_FILE_CAP     8000U

TEST_CASE("a stop request ends a samples recording and the answer carries "
          "what was captured", "[audio_ao]")
{
    static int16_t frames[8];
    khal_sem_t     sem  = khal_sem_create();
    uint32_t       rc   = 0xA5A5A5A5U;
    uint32_t       got  = 0xA5A5A5A5U;
    uint32_t       ack  = 0U;

    stream_ready();
    fake_sem_reset();
    audio_record_request(frames, REC_FRAMES, &got, &rc, sem, 4U, &ack);
    (void)run();
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_RECORD, sub_type(1U));
    TEST_ASSERT_EQUAL_UINT32(REC_FRAMES, s_sub[1].frames);
    TEST_ASSERT_EQUAL_UINT32(0U, s_rec_ends);

    audio_rec_stop_request();
    (void)run();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, s_rec_ends,
        "the stop reached the driver while the recording ran");
    TEST_ASSERT_FALSE_MESSAGE(khal_sem_wait_timeout(sem, 0U),
        "the caller waits for the job's own answer, not for the stop");

    (void)memset(&s_res, 0, sizeof(s_res));
    s_res.frames = REC_CAPTURED;
    finish(KDRV_ES8311_JOB_RECORD, KODE_OK);
    TEST_ASSERT_EQUAL_HEX32((uint32_t)KODE_OK, rc);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(REC_CAPTURED, got,
        "the frames so far are what the facade is told");
    TEST_ASSERT_EQUAL_UINT32(4U, ack);
    TEST_ASSERT_TRUE(khal_sem_wait_timeout(sem, 0U));
}

TEST_CASE("the early end is cleared before a recording is submitted and a "
          "stop right after it is kept", "[audio_ao]")
{
    static int16_t frames[8];

    stream_ready();
    audio_record_request(frames, REC_FRAMES, NULL, NULL, NULL, 0U, NULL);
    (void)run();
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_RECORD, sub_type(1U));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, s_rec_resets,
        "the AO cleared the early end for the recording it submitted");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, s_reset_at_sub,
        "and cleared it before the recording reached the worker");

    audio_rec_stop_request();
    (void)run();
    TEST_ASSERT_EQUAL_UINT32(1U, s_rec_ends);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2U, s_end_at_sub,
        "the stop came after the recording was submitted");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, s_rec_resets,
        "and nothing cleared it again behind the stop");
    finish(KDRV_ES8311_JOB_RECORD, KODE_OK);
}

TEST_CASE("a stop while the codec is still opening is applied right after the recording is submitted",
          "[audio_ao]")
{
    static int16_t frames[8];
    uint32_t       rc  = 0xA5A5A5A5U;
    uint32_t       got = 0xA5A5A5A5U;

    stream_ready();
    audio_record_request(frames, REC_FRAMES, &got, &rc, NULL, 0U, NULL);
    (void)run();
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_OPEN, sub_type(0U));

    audio_rec_stop_request();
    (void)run();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, s_rec_ends,
        "no recording job exists yet; the driver's flag is not touched");

    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_RECORD, sub_type(1U));
    TEST_ASSERT_EQUAL_UINT32(REC_FRAMES, s_sub[1].frames);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, s_rec_resets, "the early end was cleared for the job");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, s_rec_ends, "and the held stop ended it");

    (void)memset(&s_res, 0, sizeof(s_res));
    finish(KDRV_ES8311_JOB_RECORD, KODE_OK);
    TEST_ASSERT_EQUAL_HEX32((uint32_t)KODE_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(0U, got);

    /* The held stop ended with the recording: the next one runs its length. */
    audio_record_request(frames, REC_FRAMES, NULL, NULL, NULL, 0U, NULL);
    (void)run();
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_RECORD, sub_type(2U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_rec_ends);
    finish(KDRV_ES8311_JOB_RECORD, KODE_OK);
}

/** @brief How many stops a caller looping on the door makes. */
#define REC_STOP_LOOP 64U

TEST_CASE("64 stops posted while the AO does not run queue one, and after it dispatches it a new stop posts",
          "[audio_ao]")
{
    static int16_t frames[8];

    stream_ready();
    audio_record_request(frames, REC_FRAMES, NULL, NULL, NULL, 0U, NULL);
    (void)run();
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_RECORD, sub_type(1U));

    for ( uint32_t i = 0U; REC_STOP_LOOP > i; i++ )
    {
        audio_rec_stop_request();
    }
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)uxQueueMessagesWaiting(AO_Audio->eQueue));
    (void)run();
    TEST_ASSERT_EQUAL_UINT32(1U, s_rec_ends);

    audio_rec_stop_request();
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)uxQueueMessagesWaiting(AO_Audio->eQueue));
    (void)run();
    TEST_ASSERT_EQUAL_UINT32(2U, s_rec_ends);
    finish(KDRV_ES8311_JOB_RECORD, KODE_OK);
}

TEST_CASE("a stop dispatched with nothing recording leaves the door open for the next", "[audio_ao]")
{
    stream_ready();
    audio_rec_stop_request();
    (void)run();
    audio_rec_stop_request();
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)uxQueueMessagesWaiting(AO_Audio->eQueue));
    (void)run();
    TEST_ASSERT_EQUAL_UINT32(0U, s_rec_ends);
}

TEST_CASE("a stop with nothing recording is ignored", "[audio_ao]")
{
    stream_ready();
    audio_rec_stop_request();
    (void)run();
    TEST_ASSERT_EQUAL_UINT32(0U, s_rec_ends);
    TEST_ASSERT_EQUAL_UINT(0U, s_nsub);

    /* Nor does it leave anything behind for the next recording. */
    {
        static int16_t frames[8];
        uint32_t       got = 0U;

        audio_record_request(frames, REC_FRAMES, &got, NULL, NULL, 0U, NULL);
        (void)run();
        finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(REC_FRAMES, s_sub[1].frames,
            "an earlier stop does not shorten the recording that follows");
        finish(KDRV_ES8311_JOB_RECORD, KODE_OK);
    }

    /* Once that one has ended the AO is idle again, and a stop is ignored. */
    audio_rec_stop_request();
    (void)run();
    TEST_ASSERT_EQUAL_UINT32(0U, s_rec_ends);
}

TEST_CASE("a stop during a recording to a file does not cut it", "[audio_ao]")
{
    static int16_t fbuf[8];
    static uint32_t fcap = REC_FILE_CAP;
    uint32_t       rc    = 0xA5A5A5A5U;
    uint32_t       bytes = 0U;

    stream_ready();
    kdrv_es8311_buf_ExpectAndReturn(NULL, fbuf);
    kdrv_es8311_buf_IgnoreArg_frames();
    kdrv_es8311_buf_ReturnThruPtr_frames(&fcap);
    audio_record_file_request("/storage/x.wav", 500U, &rc, &bytes, NULL, 2U,
                              NULL);
    (void)run();
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_RECORD, sub_type(1U));

    audio_rec_stop_request();
    (void)run();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, s_rec_ends,
        "a file recording runs its full length");

    /* Wind the job up the way the cases above leave theirs. */
    deliver(KODE_ERROR);
    (void)run();
    ao_spy_drain();
}

TEST_CASE("a stop during a play or a tone is ignored", "[audio_ao]")
{
    static int16_t frames[8];

    stream_ready();
    TEST_ASSERT_EQUAL_INT(KODE_OK, audio_play_ask(ao_spy(), frames, 8U));
    (void)run();
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_PLAY, sub_type(1U));

    audio_rec_stop_request();
    (void)run();
    TEST_ASSERT_EQUAL_UINT32(0U, s_rec_ends);
    finish(KDRV_ES8311_JOB_PLAY, KODE_OK);
    ao_spy_drain();
}

/* ---- The settings ---------------------------------------------------------- */

/** @brief A driver field's value, as the open would push it. */
static int32_t field(kdrv_es8311_field_t f)
{
    int32_t v = -1;

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_es8311_get(f, &v));
    return v;
}

TEST_CASE("vol, mic_db and mute are stored, handed to the driver and answered",
          "[audio_ao]")
{
    const KodeConfigAnsEvt *a;
    uint8_t                 v = 0U;

    audio_up();
    fake_nvs_reset();
    /* A set reaches the chip only through the open; what is asserted here
     * is the driver's copy, which that push would carry. */
    kdrv_es8311_apply_settings_Ignore();

    a = config_ask(AO_Audio, &(config_ask_t){ .key = KSET_KEY_VOL });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("100", a->value);
    TEST_ASSERT_FALSE(a->stored);

    a = config_ask(AO_Audio, &(config_ask_t){ .key = KSET_KEY_VOL, .set = true,
                                              .i = 40 });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->status);
    TEST_ASSERT_EQUAL_STRING("40", a->value);
    TEST_ASSERT_TRUE(a->stored);
    TEST_ASSERT_EQUAL_INT32(40, field(KDRV_ES_VOL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_get_u8(KSET_NAMESPACE, KSET_KEY_VOL, &v));
    TEST_ASSERT_EQUAL_UINT8(40U, v);

    a = config_ask(AO_Audio, &(config_ask_t){ .key = KSET_KEY_MUTE, .set = true,
                                              .b = true });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("on", a->value);
    TEST_ASSERT_EQUAL_INT32(1, field(KDRV_ES_OUT_MUTE));

    a = config_ask(AO_Audio, &(config_ask_t){ .key = KSET_KEY_MIC_DB, .set = true,
                                              .i = 12 });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("12", a->value);
    TEST_ASSERT_EQUAL_INT32(12, field(KDRV_ES_MIC_DB));

    /* Back to the boot's values: the driver's table outlives the case. */
    (void)config_ask(AO_Audio, &(config_ask_t){ .key = KSET_KEY_VOL, .set = true,
                                                .i = (int32_t)KODE_AUDIO_VOL_DEFAULT });
    (void)config_ask(AO_Audio, &(config_ask_t){ .key = KSET_KEY_MUTE, .set = true });
    (void)config_ask(AO_Audio, &(config_ask_t){ .key = KSET_KEY_MIC_DB, .set = true,
                                                .i = (int32_t)KODE_AUDIO_MIC_DB_DEFAULT });
    fake_nvs_reset();
    ao_spy_drain();
}

TEST_CASE("a microphone gain off the 6 dB grid is refused and nothing is stored",
          "[audio_ao]")
{
    const KodeConfigAnsEvt *a;
    uint8_t                 v = 0U;

    audio_up();
    fake_nvs_reset();
    kdrv_es8311_apply_settings_Ignore();

    a = config_ask(AO_Audio, &(config_ask_t){ .key = KSET_KEY_MIC_DB, .set = true,
                                              .i = 25 });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8(KODE_DENIED, a->status);
    TEST_ASSERT_EQUAL_STRING("24", a->value);
    TEST_ASSERT_FALSE(a->stored);
    TEST_ASSERT_EQUAL_INT32((int32_t)KODE_AUDIO_MIC_DB_DEFAULT, field(KDRV_ES_MIC_DB));
    TEST_ASSERT_NOT_EQUAL(KODE_OK, khal_nvs_get_u8(KSET_NAMESPACE, KSET_KEY_MIC_DB, &v));
    ao_spy_drain();
}

TEST_CASE("another owner's key is not AO_Audio's to answer", "[audio_ao]")
{
    audio_up();
    TEST_ASSERT_NULL(config_ask(AO_Audio, &(config_ask_t){ .key = KSET_KEY_BRIGHT }));
    ao_spy_drain();
}

/* ---- kode_sys's requests: answered to the asking AO ------------------------- */

/** @brief Frames of a lent buffer in these cases. */
#define SYS_FRAMES   8U
/** @brief A recording's length, ms. */
#define SYS_REC_MS   10U
/** @brief What a lent buffer holds before the owner touches it. */
#define SYS_FILL     0x5A5A

/** @brief A static event AO_Audio ignores, to fill its queue with. */
static const QEvt s_sys_filler = QEVT_INITIALIZER(IMU_TEST_REQ_SIG);

/** @brief AO_Audio idle, its queue one a margin post can read, the spy empty. */
static void sys_ready(void)
{
    stream_ready();
    qf_support_margin_queue(AO_Audio);
    ao_spy_drain();
}

/** @brief The next answer the spy holds, which must be @p sig. */
static const void *sys_take(enum_t sig)
{
    const QEvt *e = ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "every accepted request is answered");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)sig, (int)e->sig, "the wrong answer");
    return (const void *)e;
}

/** @brief Whether any job the driver was handed carried @p buf. */
static bool sub_carried(const int16_t *buf)
{
    for ( unsigned i = 0U; (i < s_nsub) && (SUBMITS_MAX > i); ++i )
    {
        if ( s_sub[i].buf == buf )
        {
            return true;
        }
    }
    return false;
}

TEST_CASE("kode_sys_audio_play plays the lent frames and answers the asking AO once",
          "[audio_ao]")
{
    static int16_t                 frames[SYS_FRAMES];
    const kode_sys_audio_answer_t *a;

    sys_ready();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_play(ao_spy(), frames, SYS_FRAMES));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kode_sys_audio_play(ao_spy(), frames, SYS_FRAMES),
                                  "a second play before the answer is refused at once");
    (void)run();
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    TEST_ASSERT_EQUAL_PTR_MESSAGE(frames, s_held_req.buf, "the job plays the lent buffer itself");
    TEST_ASSERT_EQUAL_UINT32(SYS_FRAMES, s_held_req.frames);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "not answered before the sound ends");

    finish(KDRV_ES8311_JOB_PLAY, KODE_OK);
    a = (const kode_sys_audio_answer_t *)sys_take(AUDIO_DONE_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "posted once, not published as well");
    ao_spy_release();

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_audio_play(ao_spy(), frames, SYS_FRAMES),
                                  "the answer opens the door again");
    (void)run();
    finish(KDRV_ES8311_JOB_PLAY, KODE_OK);
    (void)sys_take(AUDIO_DONE_SIG);
    ao_spy_drain();
}

TEST_CASE("a codec that will not open still answers kode_sys's request with the status",
          "[audio_ao]")
{
    static int16_t                 frames[SYS_FRAMES];
    const kode_sys_audio_answer_t *a;

    sys_ready();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_play(ao_spy(), frames, SYS_FRAMES));
    (void)run();
    finish(KDRV_ES8311_JOB_OPEN, KODE_IO_ERROR);
    a = (const kode_sys_audio_answer_t *)sys_take(AUDIO_DONE_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_IO_ERROR, a->rc);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    ao_spy_drain();
}

TEST_CASE("kode_sys_audio_record records into the lent buffer and says how much",
          "[audio_ao]")
{
    static int16_t                 buf[SYS_FRAMES];
    const kode_sys_audio_answer_t *a;

    sys_ready();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_record(ao_spy(), buf, SYS_FRAMES, SYS_REC_MS));
    (void)run();
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    TEST_ASSERT_EQUAL_PTR(buf, s_held_req.buf);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(SYS_FRAMES, s_held_req.frames, "capped at the lent capacity");

    s_res.frames = SYS_FRAMES - 1U;
    finish(KDRV_ES8311_JOB_RECORD, KODE_OK);
    a = (const kode_sys_audio_answer_t *)sys_take(AUDIO_REC_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT32(SYS_FRAMES - 1U, a->frames);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    (void)memset(&s_res, 0, sizeof(s_res));
    ao_spy_drain();
}

TEST_CASE("a lent recording whose guard fires is answered only once the worker lets go",
          "[audio_ao]")
{
    static int16_t                 buf[SYS_FRAMES];
    const kode_sys_audio_answer_t *a;

    sys_ready();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_record(ao_spy(), buf, SYS_FRAMES, SYS_REC_MS));
    (void)run();
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    s_rec_ends = 0U;

    (void)qf_support_tick(AO_Audio, GUARD_TICKS + 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "the worker may still be writing the lent buffer: no answer yet");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, s_rec_ends, "the recording is told to end");

    s_res.frames = 2U;
    finish(KDRV_ES8311_JOB_RECORD, KODE_OK);
    a = (const kode_sys_audio_answer_t *)sys_take(AUDIO_REC_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_TIMEOUT, a->rc);
    TEST_ASSERT_EQUAL_UINT32(2U, a->frames);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    (void)memset(&s_res, 0, sizeof(s_res));
    ao_spy_drain();
}

TEST_CASE("a refused lent recording never reaches the buffer", "[audio_ao]")
{
    static int16_t                 buf[SYS_FRAMES];
    const kode_sys_audio_answer_t *a;

    sys_ready();
    for ( uint32_t i = 0U; SYS_FRAMES > i; ++i )
    {
        buf[i] = (int16_t)SYS_FILL;
    }
    audio_tone_request(1020U, 100U, 25U);
    (void)run();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_record(ao_spy(), buf, SYS_FRAMES, SYS_REC_MS));
    (void)run();
    a = (const kode_sys_audio_answer_t *)sys_take(AUDIO_REC_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED, a->rc);
    TEST_ASSERT_EQUAL_UINT32(0U, a->frames);
    TEST_ASSERT_FALSE_MESSAGE(sub_carried(buf), "no job was handed the refused buffer");
    for ( uint32_t i = 0U; SYS_FRAMES > i; ++i )
    {
        TEST_ASSERT_EQUAL_INT16((int16_t)SYS_FILL, buf[i]);
    }
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    finish(KDRV_ES8311_JOB_TONE, KODE_OK);
    ao_spy_drain();
}

TEST_CASE("kode_sys_audio_record_file is answered to the asking AO, refusals too",
          "[audio_ao]")
{
    const kode_sys_audio_file_answer_t *a;

    sys_ready();
    audio_tone_request(1020U, 100U, 25U);
    (void)run();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_record_file(ao_spy(), "/storage/x.wav", SYS_REC_MS));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_audio_record_file(ao_spy(), "/storage/x.wav", SYS_REC_MS));
    (void)run();
    a = (const kode_sys_audio_file_answer_t *)sys_take(AUDIO_FILE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED, a->rc);
    TEST_ASSERT_EQUAL_STRING("/storage/x.wav", a->path);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "posted, not published as well");
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    finish(KDRV_ES8311_JOB_TONE, KODE_OK);
    ao_spy_drain();
}

TEST_CASE("kode_sys_audio_info_read answers a copy of the path's state", "[audio_ao]")
{
    const kode_sys_audio_info_answer_t *a;

    sys_ready();
    kdrv_es8311_info_StubWithCallback(&fill_info);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_info_read(ao_spy()));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_audio_info_read(ao_spy()));
    (void)run();
    a = (const kode_sys_audio_info_answer_t *)sys_take(AUDIO_INFO_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_TRUE(a->info.present);
    TEST_ASSERT_FALSE_MESSAGE(a->info.busy, "busy is the AO's: nothing is with the worker");
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    ao_spy_drain();
}

TEST_CASE("kode_sys's stream opens and closes with an answer each; its write never waits",
          "[audio_ao]")
{
    static int16_t                 frames[SYS_FRAMES];
    size_t                         taken = 0U;
    const kode_sys_audio_answer_t *a;

    sys_ready();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_stream_open(ao_spy()));
    (void)run();
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    a = (const kode_sys_audio_answer_t *)sys_take(AUDIO_STREAM_OPEN_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    ao_spy_release();

    kdrv_es8311_stream_write_ExpectAndReturn(frames, SYS_FRAMES, KODE_SYS_AUDIO_CHANNELS_MONO,
                                             0U, &taken, KODE_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_stream_write(ao_spy(), frames, SYS_FRAMES,
                                                               KODE_SYS_AUDIO_CHANNELS_MONO,
                                                               &taken));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_stream_close(ao_spy()));
    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "not before the ring has drained");
    finish(KDRV_ES8311_JOB_STREAM, KODE_OK);
    a = (const kode_sys_audio_answer_t *)sys_take(AUDIO_STREAM_CLOSE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_stream_close(ao_spy()));
    (void)run();
    a = (const kode_sys_audio_answer_t *)sys_take(AUDIO_STREAM_CLOSE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((uint32_t)KODE_DENIED, a->rc, "no stream to close");
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    ao_spy_drain();
}

TEST_CASE("a close before the open is answered is refused, and the open answers once",
          "[audio_ao]")
{
    const kode_sys_audio_answer_t *a;

    sys_ready();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_stream_open(ao_spy()));
    (void)run();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_stream_close(ao_spy()));
    (void)run();
    a = (const kode_sys_audio_answer_t *)sys_take(AUDIO_STREAM_CLOSE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((uint32_t)KODE_DENIED, a->rc, "the open is still owed");
    ao_spy_release();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, s_stream_ends, "the ring is left alone");

    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    a = (const kode_sys_audio_answer_t *)sys_take(AUDIO_STREAM_OPEN_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    ao_spy_release();
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_stream_close(ao_spy()));
    (void)run();
    finish(KDRV_ES8311_JOB_STREAM, KODE_OK);
    a = (const kode_sys_audio_answer_t *)sys_take(AUDIO_STREAM_CLOSE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    ao_spy_release();

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_audio_stream_open(ao_spy()),
                                  "the open's door is free again");
    (void)run();
    a = (const kode_sys_audio_answer_t *)sys_take(AUDIO_STREAM_OPEN_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    ao_spy_release();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_stream_close(ao_spy()));
    (void)run();
    finish(KDRV_ES8311_JOB_STREAM, KODE_OK);
    (void)sys_take(AUDIO_STREAM_CLOSE_RSP_SIG);
    ao_spy_drain();
}

TEST_CASE("a plain task's audio request is refused at once and posts nothing",
          "[audio_ao]")
{
    static int16_t frames[SYS_FRAMES];

    sys_ready();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_audio_info_read(NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_audio_play(NULL, frames, SYS_FRAMES));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_audio_record(NULL, frames, SYS_FRAMES, SYS_REC_MS));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_audio_record_file(NULL, "/storage/x.wav", SYS_REC_MS));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_audio_stream_open(NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_audio_stream_close(NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_audio_play(ao_spy(), NULL, SYS_FRAMES));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_audio_play(ao_spy(), frames, 0U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_audio_record(ao_spy(), frames, SYS_FRAMES, 0U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_audio_record_file(ao_spy(), NULL, SYS_REC_MS));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Audio->eQueue));
    ao_spy_drain();
}

TEST_CASE("a full AO_Audio queue refuses kode_sys's requests with no resources",
          "[audio_ao]")
{
    static int16_t frames[SYS_FRAMES];
    const QEvt    *e;

    sys_ready();
    TEST_ASSERT_TRUE_MESSAGE(qf_support_queue_stuff(AO_Audio, &s_sys_filler), "the queue never filled");
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_audio_info_read(ao_spy()));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_audio_play(ao_spy(), frames, SYS_FRAMES));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_audio_stream_open(ao_spy()));
    while ( xQueueReceive(AO_Audio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }

    kdrv_es8311_info_StubWithCallback(&fill_info);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_audio_info_read(ao_spy()),
                                  "nothing was posted, so the door is open");
    (void)run();
    (void)sys_take(AUDIO_INFO_RSP_SIG);
    ao_spy_drain();
}

TEST_CASE("two AOs playing at once are both accepted, and the codec refuses the second",
          "[audio_ao]")
{
    static int16_t                 frames[SYS_FRAMES];
    const kode_sys_audio_answer_t *a;
    const QEvt                    *o;

    sys_ready();
    ao_spy_other_drain();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_play(ao_spy(), frames, SYS_FRAMES));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_audio_play(ao_spy_other(), frames, SYS_FRAMES),
                                  "the door is the asker's, not the kind's");
    (void)run();
    o = ao_spy_other_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(o, "the second play is answered, inside the event");
    TEST_ASSERT_EQUAL_INT((int)AUDIO_DONE_SIG, (int)o->sig);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED, ((const kode_sys_audio_answer_t *)(const void *)o)->rc);
    QF_gc(o);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    finish(KDRV_ES8311_JOB_PLAY, KODE_OK);
    a = (const kode_sys_audio_answer_t *)sys_take(AUDIO_DONE_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_NULL(ao_spy_other_take());
    ao_spy_drain();
}

TEST_CASE("a lent play whose guard fires is answered only once the worker lets go",
          "[audio_ao]")
{
    static int16_t                 frames[SYS_FRAMES];
    const kode_sys_audio_answer_t *a;

    sys_ready();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_play(ao_spy(), frames, SYS_FRAMES));
    (void)run();
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);

    (void)qf_support_tick(AO_Audio, GUARD_TICKS + 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "the worker may still be reading the lent frames: no answer yet");

    finish(KDRV_ES8311_JOB_PLAY, KODE_OK);
    a = (const kode_sys_audio_answer_t *)sys_take(AUDIO_DONE_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_TIMEOUT, a->rc);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    ao_spy_drain();
}

TEST_CASE("only the AO that opened the stream writes and closes it", "[audio_ao]")
{
    static int16_t                 frames[SYS_FRAMES];
    size_t                         taken = 1U;
    const kode_sys_audio_answer_t *a;
    const QEvt                    *o;

    sys_ready();
    ao_spy_other_drain();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_stream_open(ao_spy()));
    (void)run();
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    a = (const kode_sys_audio_answer_t *)sys_take(AUDIO_STREAM_OPEN_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    ao_spy_release();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_audio_stream_write(ao_spy_other(), frames, SYS_FRAMES,
                                                                   KODE_SYS_AUDIO_CHANNELS_MONO, &taken));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, taken, "a refused write takes nothing");
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_audio_stream_write(NULL, frames, SYS_FRAMES,
                                                                   KODE_SYS_AUDIO_CHANNELS_MONO, NULL));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_stream_close(ao_spy_other()));
    (void)run();
    o = ao_spy_other_take();
    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_EQUAL_INT((int)AUDIO_STREAM_CLOSE_RSP_SIG, (int)o->sig);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((uint32_t)KODE_DENIED,
                                   ((const kode_sys_audio_answer_t *)(const void *)o)->rc,
                                   "another asker's close is refused");
    QF_gc(o);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_stream_ends, "and the stream plays on");

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_stream_close(ao_spy()));
    (void)run();
    finish(KDRV_ES8311_JOB_STREAM, KODE_OK);
    a = (const kode_sys_audio_answer_t *)sys_take(AUDIO_STREAM_CLOSE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kode_sys_audio_stream_write(ao_spy(), frames, SYS_FRAMES,
                                                                           KODE_SYS_AUDIO_CHANNELS_MONO, NULL),
                                  "a closed stream has no writer");
    ao_spy_drain();
}

/** @brief The body of the full-queue case, run in a child: the asker's queue is full, then AO_Audio answers into it. */
static void full_asker_body(void *arg)
{
    (void)arg;
    (void)qf_support_queue_stuff(ao_spy_other(), &s_sys_filler);
    kdrv_es8311_info_StubWithCallback(&fill_info);
    (void)kode_sys_audio_info_read(ao_spy_other());
    (void)qf_support_settle(AO_Audio);
}

TEST_CASE("an answer into an asker's full queue is QP's assert, not a drop",
          "[audio_ao]")
{
    sys_ready();
    ao_spy_other_drain();

    /* QP asserts with Q_onError(), which aborts this binary, so the post runs
     * in a child and the parent reads how it died. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)ASK_CHILD_ABORTED,
                                    (uint32_t)ask_support_fork_run(&full_asker_body, NULL),
                                  "the child was not stopped by QP's assert");
}

/* ---- The board's rest ------------------------------------------------------- */

/** @brief The codec open and idle: one tone played to its end. */
static void codec_open_idle(void)
{
    audio_up();
    audio_tone_request(1020U, 10U, 25U);
    (void)run();
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    finish(KDRV_ES8311_JOB_TONE, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(2U, s_nsub);
}

/* The same hush and close the power-off heads-up gives, and the answer waits
 * for the worker to say the codec is closed. */
TEST_CASE("a round hushes and closes an open codec, and answers when it is closed",
          "[audio_ao][lowpower]")
{
    codec_open_idle();

    lp_round(AO_Audio, KODE_LP_ASLEEP);
    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_hush, "the converter was not silenced");
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_CLOSE, sub_type(2U));
    lp_expect_silence();            /* closed is not closing */

    finish(KDRV_ES8311_JOB_CLOSE, KODE_OK);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();

    /* A second round (OFF after ASLEEP) finds it closed and answers at once. */
    lp_round(AO_Audio, KODE_LP_OFF);
    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, s_nsub, "a closed codec was closed again");
    lp_expect_answer(false, NULL);
}

TEST_CASE("a codec that was never opened is rested by saying so", "[audio_ao][lowpower]")
{
    audio_up();

    lp_round(AO_Audio, KODE_LP_ASLEEP);
    (void)run();
    TEST_ASSERT_EQUAL_UINT(0U, s_nsub);
    TEST_ASSERT_EQUAL_UINT(0U, s_hush);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();
}

/* Nothing is restored on AWAKE: the first sound after it asks for the
 * amplifier and opens the codec as it always did. */
TEST_CASE("after the rest AWAKE does nothing and the next sound opens the codec",
          "[audio_ao][lowpower]")
{
    codec_open_idle();
    lp_round(AO_Audio, KODE_LP_ASLEEP);
    (void)run();
    finish(KDRV_ES8311_JOB_CLOSE, KODE_OK);
    lp_expect_answer(false, NULL);

    lp_awake(AO_Audio);
    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, s_nsub, "AWAKE reached the codec");

    audio_tone_request(1020U, 10U, 25U);
    (void)run();
    c5_running();
    (void)run();
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_JOB_OPEN, sub_type(3U));
    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    finish(KDRV_ES8311_JOB_TONE, KODE_OK);
}

/* A sound is a promise to whoever is listening. */
TEST_CASE("a sound playing refuses the round and is not cut", "[audio_ao][lowpower]")
{
    audio_up();
    audio_tone_request(1020U, 10U, 25U);
    (void)run();

    lp_round(AO_Audio, KODE_LP_ASLEEP);
    (void)run();
    lp_expect_answer(true, "audio playing");
    lp_expect_no_more();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_hush, "the sound was hushed under its listener");

    finish(KDRV_ES8311_JOB_OPEN, KODE_OK);
    finish(KDRV_ES8311_JOB_TONE, KODE_OK);
}

TEST_CASE("the production row refuses the round", "[audio_ao][lowpower]")
{
    audio_up();
    audio_test_request();
    (void)run();

    lp_round(AO_Audio, KODE_LP_ASLEEP);
    (void)run();
    lp_expect_answer(true, "audio test");
    TEST_ASSERT_EQUAL_UINT(0U, s_hush);

    /* Wind the row up with the first failure it can have. */
    deliver(KODE_ERROR);
    (void)run();
}

/* A codec the board cannot close is no reason to stay awake over a few mA. */
TEST_CASE("a close that fails still lets the board sleep, and says so",
          "[audio_ao][lowpower]")
{
    codec_open_idle();

    lp_round(AO_Audio, KODE_LP_ASLEEP);
    (void)run();
    fake_klog_reset();
    finish(KDRV_ES8311_JOB_CLOSE, KODE_ERROR);
    lp_expect_answer(false, NULL);
    TEST_ASSERT_TRUE_MESSAGE(said("did not close") > 0U, "a failed close went unsaid");
}

TEST_CASE("a close the worker never answers is the guard's, and the round is answered",
          "[audio_ao][lowpower]")
{
    codec_open_idle();

    lp_round(AO_Audio, KODE_LP_ASLEEP);
    (void)run();
    lp_expect_silence();

    (void)qf_support_tick(AO_Audio, GUARD_TICKS + 2U);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();
    deliver(KODE_OK);                   /* the late answer, so the worker is free */
    (void)run();
}

/* A second round before the close ends takes the first one's place: the
 * answer carries the round that is open. */
TEST_CASE("a round that finds the close already out waits for it too", "[audio_ao][lowpower]")
{
    codec_open_idle();

    lp_round(AO_Audio, KODE_LP_ASLEEP);
    (void)run();
    lp_round(AO_Audio, KODE_LP_OFF);
    (void)run();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, s_nsub, "the close was started twice");

    finish(KDRV_ES8311_JOB_CLOSE, KODE_OK);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();
}

/* LAST in this file on purpose: AO_Audio is rebuilt with no codec and left
 * that way. No later file dispatches it; those that use it build their own. */
TEST_CASE("with no codec every kode_sys audio request is answered with the status",
          "[audio_ao]")
{
    static int16_t                      frames[SYS_FRAMES];
    const kode_sys_audio_answer_t      *a;
    const kode_sys_audio_info_answer_t *i;
    const kode_sys_audio_file_answer_t *f;

    sys_ready();
    qf_support_reap_timers();
    Audio_ctor();
    qf_support_margin_queue(AO_Audio);
    kdrv_es8311_init_ExpectAndReturn(KODE_ERROR);
    QASM_INIT(AO_Audio, (void *)0, 0U);
    ao_spy_drain();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_play(ao_spy(), frames, SYS_FRAMES));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_record(ao_spy(), frames, SYS_FRAMES, SYS_REC_MS));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_record_file(ao_spy(), "/storage/x.wav", SYS_REC_MS));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_stream_open(ao_spy()));
    kdrv_es8311_info_StubWithCallback(&fill_info);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_info_read(ao_spy()));
    (void)qf_support_settle(AO_Audio);

    a = (const kode_sys_audio_answer_t *)sys_take(AUDIO_DONE_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED, a->rc);
    a = (const kode_sys_audio_answer_t *)sys_take(AUDIO_REC_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED, a->rc);
    f = (const kode_sys_audio_file_answer_t *)sys_take(AUDIO_FILE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED, f->rc);
    a = (const kode_sys_audio_answer_t *)sys_take(AUDIO_STREAM_OPEN_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED, a->rc);
    i = (const kode_sys_audio_info_answer_t *)sys_take(AUDIO_INFO_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, i->rc);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "each answered once");
    ao_spy_drain();
}
