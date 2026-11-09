/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_audio.c
 * @brief The waiting audio calls, and the memory each of them lends.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "qpc.h"
#include "Mockkdrv_es8311.h"

#include "fakes.h"
#include "kaos.h"
#include "kes_fields.h"
#include "khal_sync.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kqp.h"
#include "qf_support.h"

/**
 * @brief What a stand-in running inside a wait could not do, if anything.
 *
 * Set instead of asserted, and read back by the case afterwards: a Unity
 * assertion longjmps to the end of the case, and one firing inside a hook -
 * which runs inside the facade call - would skip the claim's release and fail
 * every later audio case with KODE_DENIED. test_kode_ir.c's rule.
 */
static const char *s_hook_fault;

/** @brief What the stand-in reports as the codec's state. */
static const kdrv_es8311_info_t ON_BOARD = {
    .rate    = 48000U,
    .present = true,
    .open    = false,
    .busy    = false,
    .bits    = 16U,
    .volume  = 73U,
    .mic_db  = 18U,
    .mute    = true,
};

/** @brief The caller's output buffer, which the stand-in must NOT be handed. */
static const int16_t *s_out;

/** @brief How many frames the recording stand-in says arrived. */
#define RECORDED  480U

#define ASKED 960U         /**< Frames the recording stand-in is asked for.   */
#define PATTERN_STEP 31U   /**< The pattern it brings back is f * step + offset. */
#define PATTERN_OFFSET 7U  /**< Its offset.                                   */

/** @brief Fail the case if a hook saw something wrong. */
static void hook_verdict(void)
{
    if ( NULL != s_hook_fault )
    {
        TEST_FAIL_MESSAGE(s_hook_fault);
    }
}

/**
 * @brief Take the one request the call posted, or note that it did not.
 *
 * @param sig The signal it should carry.
 * @return The event, still owned by the queue's reference, or NULL.
 */
static const QEvt *take(enum_t sig)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Audio->eQueue, &e, 0U) != pdTRUE )
    {
        s_hook_fault = "the call posted nothing to AO_Audio";
        return NULL;
    }
    if ( e->sig != sig )
    {
        s_hook_fault = "the call posted the wrong request to AO_Audio";
        QF_gc(e);
        return NULL;
    }
    return e;
}

/** @brief AO_Audio's info_answer(), by hand. */
static void answer_info(void)
{
    const QEvt * const e = take(AUDIO_INFO_REQ_SIG);

    if ( NULL != e )
    {
        AudioInfoReqEvt const * const q = (AudioInfoReqEvt const *)e;
        if ( NULL == q->out )
        {
            s_hook_fault = "AO_Audio was lent nowhere to answer into";
        }
        else
        {
            *(kdrv_es8311_info_t *)q->out = ON_BOARD;
        }
        if ( NULL != q->ack )
        {
            *q->ack = q->gen;
        }
        khal_sem_signal(q->done);
        QF_gc(e);
    }
}

/** @brief AO_Audio recording RECORDED frames of a known pattern, by hand. */
static void answer_record(void)
{
    const QEvt * const e = take(AUDIO_REC_REQ_SIG);

    if ( NULL != e )
    {
        AudioBufEvt const * const q = (AudioBufEvt const *)e;
        uint32_t f;

        if ( NULL == q->buf )
        {
            s_hook_fault = "AO_Audio was handed nowhere to record into";
        }
        else if ( q->buf == s_out )
        {
            s_hook_fault = "AO_Audio was handed the caller's own buffer";
        }
        else if ( RECORDED > q->frames )
        {
            s_hook_fault = "AO_Audio was asked for fewer frames than it gives";
        }
        else
        {
            for ( f = 0U; RECORDED > f; ++f )
            {
                q->buf[f] = (int16_t)((f * PATTERN_STEP) + PATTERN_OFFSET);
            }
        }
        if ( NULL != q->got )
        {
            *q->got = RECORDED;
        }
        if ( NULL != q->rc )
        {
            *q->rc = (uint32_t)KODE_OK;
        }
        if ( NULL != q->ack )
        {
            *q->ack = q->gen;
        }
        khal_sem_signal(q->done);
        QF_gc(e);
    }
}

/** @brief Drop whatever sits, undispatched, in AO_Audio's queue. */
static void drain(void)
{
    const QEvt *e;

    while ( xQueueReceive(AO_Audio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/**
 * @brief A clean facade, AO_Audio's mailbox empty, and a proof of both.
 *
 * The proof is one ordinary info call with the stand-in answering it. A leaked
 * claim is the one failure that spreads, and naming it at the top of the first
 * case after the damage is worth more than the refusals it would cascade into.
 */
static void audio_fresh(void)
{
    kdrv_es8311_info_t probe;

    Mockkdrv_es8311_Init();
    qf_support_init();
    Audio_ctor();
    qf_support_give_queue(AO_Audio);
    drain();
    fake_sem_reset();
    fake_time_reset();
    fake_klog_reset();
    s_hook_fault = NULL;

    fake_sem_wait_hook(&answer_info);
    TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(
        KODE_DENIED, kode_block_audio_info(&probe),
        "the audio facade was still dirty when this case began: an earlier "
        "case left a waiting audio call without releasing its claim");

    drain();
    fake_sem_reset();
    fake_time_reset();
    s_hook_fault = NULL;
}

TEST_CASE("kode_block_audio_info fills the caller's struct once the answer arrives",
          "[kode_audio]")
{
    kdrv_es8311_info_t got;

    audio_fresh();
    (void)memset(&got, 0, sizeof(got));

    fake_sem_wait_hook(&answer_info);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_audio_info(&got));
    hook_verdict();
    TEST_ASSERT_TRUE(got.present);
    TEST_ASSERT_EQUAL_UINT32(48000U, got.rate);
    TEST_ASSERT_EQUAL_UINT8(73U, got.volume);
    TEST_ASSERT_EQUAL_UINT8(18U, got.mic_db);
    TEST_ASSERT_TRUE_MESSAGE(got.mute, "the info lost the mute");
}

TEST_CASE("kode_block_audio_info refuses a NULL struct and posts nothing",
          "[kode_audio]")
{
    audio_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_audio_info(NULL));
    TEST_ASSERT_EQUAL_UINT(0U, (unsigned)uxQueueMessagesWaiting(AO_Audio->eQueue));
}

/* The case M5 of the release audit is about. The wait runs out, the claim is
 * given back, and the request is still in AO_Audio's queue owed an answer.
 * When AO_Audio gets to it, it must write into memory the facade owns: the
 * caller's struct is, on the board, a stack frame that has returned. */
TEST_CASE("a late info answer lands in the facade's own storage, never in the caller's",
          "[kode_audio]")
{
    kdrv_es8311_info_t got;
    kdrv_es8311_info_t sentinel;

    audio_fresh();

    /* A sentinel in every byte, so a write of any width through a stale
     * pointer shows up rather than happening to match. */
    (void)memset(&got, 0x5A, sizeof(got));
    (void)memset(&sentinel, 0x5A, sizeof(sentinel));

    fake_sem_wait_hook(NULL);
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_audio_info(&got));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&sentinel, &got, sizeof(got),
                                     "a timeout writes nothing at all");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(
        1U, (unsigned)uxQueueMessagesWaiting(AO_Audio->eQueue),
        "there was no abandoned request to be late with");

    answer_info();   /* AO_Audio, late */
    hook_verdict();
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(
        &sentinel, &got, sizeof(got),
        "AO_Audio answered an abandoned request into the caller's struct: on "
        "the board that address is a stack frame that has returned");

    /* And the late answer is not handed to whoever comes next - its
     * generation is not theirs, and theirs arrives on its own. */
    fake_sem_wait_hook(&answer_info);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_audio_info(&got));
    hook_verdict();
    TEST_ASSERT_EQUAL_UINT8(73U, got.volume);
}

/* The stream speaks the board's one rate and one or two channels, and says no
 * to anything else before a single request is posted - a rate refused here is
 * the whole of the 48 kHz rule, and nothing downstream has to repeat it. */
TEST_CASE("kode_audio_stream refuses another rate, a channel count, or no stream",
          "[kode_audio]")
{
    static const int16_t frames[4] = { 0 };
    size_t               taken     = 99U;

    audio_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_audio_stream_open(44100U, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_audio_stream_open(16000U, 2U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_audio_stream_open(48000U, 0U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_audio_stream_open(48000U, 3U));
    TEST_ASSERT_EQUAL_UINT(0U, (unsigned)uxQueueMessagesWaiting(AO_Audio->eQueue));

    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kode_block_audio_stream_write(frames, 4U, 0U, &taken));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0U, taken,
                                     "a refused write says nothing went in");
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_audio_stream_close());
}

/* The mirror image, which the audit did not name but which is the same fault:
 * AO_Audio is free the moment a recording ends and runs above the calling
 * task, so a copy OUT of the driver's buffer could come after a recording to a
 * file had already been written into it. The recording lands in the facade's
 * buffer instead, and CMock again says the driver's was never reached. */
TEST_CASE("kode_block_audio_record_samples fills a buffer the facade owns and copies out only what arrived",
          "[kode_audio]")
{
    static int16_t out[ASKED];
    size_t         n = 0U;
    uint32_t       f;

    audio_fresh();
    for ( f = 0U; ASKED > f; ++f )
    {
        out[f] = (int16_t)0x5A5A;
    }
    s_out = out;

    /* 20 ms at 48 kHz is 960 frames; the stand-in delivers half of them. */
    fake_sem_wait_hook(&answer_record);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_audio_record_samples(out, ASKED, &n, 20U));
    hook_verdict();

    TEST_ASSERT_EQUAL_UINT(RECORDED, n);
    for ( f = 0U; RECORDED > f; ++f )
    {
        TEST_ASSERT_EQUAL_INT16((int16_t)((f * PATTERN_STEP) + PATTERN_OFFSET), out[f]);
    }
    TEST_ASSERT_EQUAL_INT16_MESSAGE((int16_t)0x5A5A, out[RECORDED],
                                    "only what arrived is copied out");
}

TEST_CASE("an owner that never answers a recording writes nothing into the caller's buffer",
          "[kode_audio]")
{
    static int16_t out[96];
    size_t         n = 7U;
    uint32_t       f;

    audio_fresh();
    for ( f = 0U; 96U > f; ++f )
    {
        out[f] = (int16_t)0x5A5A;
    }

    fake_sem_wait_hook(NULL);
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_audio_record_samples(out, 96U, &n, 2U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(7U, n, "a timeout reports no count");
    for ( f = 0U; 96U > f; ++f )
    {
        TEST_ASSERT_EQUAL_INT16((int16_t)0x5A5A, out[f]);
    }

    /* The abandoned request still sits in AO_Audio's queue; the claim does
     * not, so the next case's proof gets straight in. */
    drain();
}

/** @brief How long the stand-in says the file came out, header included. */
#define FILE_BYTES  96044U

/** @brief A path, for calls that only need one to post. */
#define FILE_PATH   "/storage/mic.wav"

/** @brief Where the last file request asked AO_Audio to put the length. */
static uint32_t *s_lent_bytes;

/** @brief AO_Audio's file_done() for a recording written out, by hand. */
static void answer_record_file(void)
{
    const QEvt * const e = take(AUDIO_REC_FILE_REQ_SIG);

    if ( NULL != e )
    {
        AudioFileReqEvt const * const q = (AudioFileReqEvt const *)e;
        s_lent_bytes = q->bytes;
        if ( NULL != q->bytes )
        {
            *q->bytes = FILE_BYTES;
        }
        if ( NULL != q->rc )
        {
            *q->rc = (uint32_t)KODE_OK;
        }
        if ( NULL != q->ack )
        {
            *q->ack = q->gen;
        }
        khal_sem_signal(q->done);
        QF_gc(e);
    }
}

/**
 * @brief Make the same call from a frame a kilobyte deeper.
 *
 * A length lent from the waiting call's own locals moves with the depth it is
 * called at; one the facade owns stays where it is. Not inlined, so the depth
 * is real.
 *
 * @return Where AO_Audio was told to put the length.
 */
static uint32_t *lent_from_deeper_frame(void) __attribute__((noinline));
static uint32_t *lent_from_deeper_frame(void)
{
    volatile uint8_t ballast[1024];
    uint32_t         bytes = 0U;

    ballast[0] = 1U;
    ballast[sizeof(ballast) - 1U] = ballast[0];
    fake_sem_wait_hook(&answer_record_file);
    (void)kode_block_audio_record_file(FILE_PATH, 20U, &bytes);
    return s_lent_bytes;
}

TEST_CASE("kode_block_audio_record_file reports the file's length once the answer arrives",
          "[kode_audio]")
{
    uint32_t bytes = 0U;

    audio_fresh();
    fake_sem_wait_hook(&answer_record_file);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_audio_record_file(FILE_PATH, 20U, &bytes));
    hook_verdict();
    TEST_ASSERT_EQUAL_UINT32(FILE_BYTES, bytes);
}

/* The review's finding. AO_Audio writes the length in file_done(), after
 * AO_Storage has written the file out - on the internal partition longer
 * than the call waits for a long recording. So the call can have returned
 * and its frame be reused when the answer lands, and the length must be
 * lent from memory the facade owns. */
TEST_CASE("a late record-file answer lands in the facade's own storage, never in a returned frame",
          "[kode_audio]")
{
    uint32_t  bytes = 0U;
    uint32_t *shallow;
    uint32_t *deep;

    audio_fresh();
    fake_sem_wait_hook(&answer_record_file);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_audio_record_file(FILE_PATH, 20U, &bytes));
    hook_verdict();
    shallow = s_lent_bytes;
    deep    = lent_from_deeper_frame();
    hook_verdict();

    TEST_ASSERT_NOT_NULL_MESSAGE(shallow, "AO_Audio was lent nowhere to answer");
    TEST_ASSERT_TRUE_MESSAGE(shallow != &bytes,
                             "AO_Audio was lent the caller's own count");
    TEST_ASSERT_EQUAL_PTR_MESSAGE(shallow, deep,
        "the lent length moved with the caller's stack: it is a local of the "
        "waiting call, and a late answer writes into a frame that has returned");

    /* And late for real: the wait runs out, then AO_Audio answers. */
    bytes = 0x5A5A5A5AU;
    fake_sem_wait_hook(NULL);
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT,
                          kode_block_audio_record_file(FILE_PATH, 20U, &bytes));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0x5A5A5A5AU, bytes,
                                     "a timeout reports no length");
    answer_record_file();   /* AO_Audio, late */
    hook_verdict();
    TEST_ASSERT_EQUAL_PTR(shallow, s_lent_bytes);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0x5A5A5A5AU, bytes,
        "the late answer reached the caller's count");
}

/** @brief How many frames the early-ended recording stand-in brings back. */
#define PARTIAL  240U

/**
 * @brief The task that calls kode_sys_audio_record_stop(), then AO_Audio.
 *
 * Runs inside the recording call's wait: the stop is posted while the
 * recording task is blocked on the claim, then AO_Audio answers the recording
 * with the frames it had when the stop arrived.
 */
static void stop_then_answer_partial(void)
{
    const QEvt *rec;
    const QEvt *stop;

    kode_sys_audio_record_stop();

    rec  = take(AUDIO_REC_REQ_SIG);
    stop = take(AUDIO_REC_STOP_REQ_SIG);
    if ( (NULL != rec) && (NULL != stop) )
    {
        AudioBufEvt const * const q = (AudioBufEvt const *)rec;
        uint32_t f;

        for ( f = 0U; PARTIAL > f; ++f )
        {
            q->buf[f] = (int16_t)((f * PATTERN_STEP) + PATTERN_OFFSET);
        }
        *q->got = PARTIAL;
        *q->rc  = (uint32_t)KODE_OK;
        *q->ack = q->gen;
        khal_sem_signal(q->done);
    }
    if ( NULL != rec )
    {
        QF_gc(rec);
    }
    if ( NULL != stop )
    {
        QF_gc(stop);
    }
}

/** @brief How many stops a caller looping on the door makes. */
#define STOP_LOOP 64U

TEST_CASE("kode_sys_audio_record_stop posts one static event however often it is called",
          "[kode_audio]")
{
    const QEvt *first;

    audio_fresh();

    for ( uint32_t i = 0U; STOP_LOOP > i; i++ )
    {
        kode_sys_audio_record_stop();
    }
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)uxQueueMessagesWaiting(AO_Audio->eQueue));
    first = take(AUDIO_REC_STOP_REQ_SIG);
    hook_verdict();
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, first->poolNum_,
                                    "the stop event is not static");
}

TEST_CASE("a recording ended early by a stop from another task returns OK with the partial count",
          "[kode_audio]")
{
    static int16_t out[ASKED];
    size_t         n = 0U;
    uint32_t       f;

    audio_fresh();
    for ( f = 0U; ASKED > f; ++f )
    {
        out[f] = (int16_t)0x5A5A;
    }

    /* The recording holds the audio claim for the whole wait: a stop that
     * needed it would be refused here, post nothing, and the stand-in would
     * find no stop request behind the recording. */
    fake_sem_wait_hook(&stop_then_answer_partial);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_audio_record_samples(out, ASKED, &n, 20U));
    hook_verdict();

    TEST_ASSERT_EQUAL_UINT(PARTIAL, n);
    for ( f = 0U; PARTIAL > f; ++f )
    {
        TEST_ASSERT_EQUAL_INT16((int16_t)((f * PATTERN_STEP) + PATTERN_OFFSET), out[f]);
    }
    TEST_ASSERT_EQUAL_INT16_MESSAGE((int16_t)0x5A5A, out[PARTIAL],
                                    "only the frames captured are copied out");
}

/** @brief Take the one setting request the call posted, and check what it carries. */
static void expect_setting(uint16_t field, int32_t value)
{
    const QEvt * const e = take(AUDIO_SETTING_REQ_SIG);

    if ( NULL != e )
    {
        AudioSettingReqEvt const * const q = (AudioSettingReqEvt const *)e;

        if ( q->field != field )
        {
            s_hook_fault = "the setting wrote another field";
        }
        else if ( audio_setting_take(field) != value )
        {
            s_hook_fault = "the setting carried the wrong value";
        }
        else
        {
            /* Everything the request should be. */
        }
        QF_gc(e);
    }
}

TEST_CASE("kode_sys_audio_mute posts the output mute setting with 1 and 0",
          "[kode_audio]")
{
    audio_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_mute(true));
    expect_setting((uint16_t)KDRV_ES_OUT_MUTE, 1);
    hook_verdict();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_mute(false));
    expect_setting((uint16_t)KDRV_ES_OUT_MUTE, 0);
    hook_verdict();
}

#define SETTING_LOOP 64U        /**< Calls a loop on a setting makes.        */
#define SETTING_MIC_PERIOD 7U   /**< The microphone gains the loop walks through. */
#define SETTING_LAST_VOLUME 41U /**< The volume the loop ends on.            */
#define SETTING_LAST_MIC_DB 36U /**< The microphone gain the loop ends on.   */

TEST_CASE("64 volume, mic gain and mute calls queue one request each and keep the last value",
          "[kode_audio]")
{
    audio_fresh();

    for ( uint32_t i = 0U; SETTING_LOOP > i; i++ )
    {
        TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_volume((uint8_t)i));
        TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_mic_gain((uint8_t)(i % SETTING_MIC_PERIOD)));
        TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_mute((i % 2U) != 0U));
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_volume(SETTING_LAST_VOLUME));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_mic_gain(SETTING_LAST_MIC_DB));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_mute(true));
    TEST_ASSERT_EQUAL_UINT32(3U, (uint32_t)uxQueueMessagesWaiting(AO_Audio->eQueue));

    expect_setting((uint16_t)KDRV_ES_VOL, (int32_t)SETTING_LAST_VOLUME);
    expect_setting((uint16_t)KDRV_ES_MIC_DB, (int32_t)SETTING_LAST_MIC_DB);
    expect_setting((uint16_t)KDRV_ES_OUT_MUTE, 1);
    hook_verdict();
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Audio->eQueue));

    /* Taken, the doors are open: the next call queues again. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_audio_volume(0U));
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)uxQueueMessagesWaiting(AO_Audio->eQueue));
    expect_setting((uint16_t)KDRV_ES_VOL, 0);
    hook_verdict();
}
