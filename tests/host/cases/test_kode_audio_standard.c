/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_audio_standard.c
 * @brief The standard layer's sound: WAV files, tones and the settings by name.
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "qpc.h"
#include "Mockkdrv_es8311.h"

#include "ao_spy.h"
#include "fake_khal_worker.h"
#include "fakes.h"
#include "kaos.h"
#include "kbsp_owners.h"
#include "kdrv_es8311.h"
#include "kdrv_storage.h"
#include "kes_fields.h"
#include "khal_once.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "kode.h"
#include "kode_entry.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_sys_priv.h"
#include "kqp.h"
#include "ktone.h"
#include "kwav.h"
#include "qf_support.h"

#define TEST_STACK_BYTES 8192U        /**< Stack of the second task, bytes.                           */
#define TEST_TASK_PRIO 1U              /**< Priority of the second task.                               */
#define TEST_WAIT_MS 4000U             /**< The longest a case waits for the player, ms.               */
#define TEST_CAP_FRAMES 64000U         /**< The most frames the capture holds.                         */
#define TEST_FILE_BYTES 140000U        /**< The most bytes of a WAV file a case builds.                */
#define TEST_CHUNK_FRAMES 4800U        /**< Frames in one chunk the player writes: 100 ms.             */
#define TEST_TWO_CHUNKS 7000U          /**< Frames of the shortest file that spans two chunks.         */
#define TEST_TWO_WINDOWS 20000U        /**< Frames of a file that spans two windows of the file read.  */
#define TEST_LONG_FRAMES 30000U        /**< Frames of a file the player can be stopped inside.         */
#define TEST_SEED_A 11U                /**< Pattern seed of one file a case tells apart.               */
#define TEST_SEED_B 5003U              /**< Pattern seed of the other.                                 */
#define TEST_VOLUME_PCT 30U            /**< A volume a case sets, percent.                             */
#define TEST_TONE_AMP 25U              /**< The tone's peak the layer makes, percent.                  */
#define TEST_FULL_SCALE 32767          /**< The peak of a full-scale sample.                           */
#define TEST_PERCENT 100               /**< Percent in the whole.                                      */
#define TEST_TONE_AMP_Q15 ((int16_t)((((int32_t)TEST_TONE_AMP) * TEST_FULL_SCALE) / TEST_PERCENT)) /**< The peak, out of 32768. */
#define TEST_TONE_HZ 1000U             /**< A tone's frequency, Hz.                                    */
#define TEST_TONE_HZ_B 1500U           /**< Another tone's.                                            */
#define TEST_TONE_MS 300U              /**< A tone that spans three chunks, ms.                        */
#define TEST_SHORT_TONE_MS 100U        /**< A tone of one chunk, ms.                                   */
#define TEST_LONG_TONE_MS 1000U        /**< A tone a stop can end, ms.                                 */
#define TEST_TONES 3U                  /**< Tones a loop plays back to back.                           */
#define TEST_LIST_BYTES 7U             /**< Payload of the LIST chunk, pad byte not counted.           */
#define TEST_BIG_LIST_BYTES 40000U     /**< Bytes of a LIST chunk longer than the first read window.   */
#define TEST_MEET_TICKS 5U             /**< Ticks the writer waits for the player to meet its lock.    */
#define TEST_OTHER_BYTES 70000U        /**< Bytes the other task writes: three parts of the file layer. */
#define TEST_RIFF_HEADER 12U           /**< Bytes of the RIFF header: a tag, a size and the WAVE tag.  */
#define TEST_CHUNK_HEADER 8U           /**< Bytes of a chunk's header.                                 */
#define TEST_FMT_BODY 16U              /**< Bytes of the format chunk's body.                          */
#define TEST_SEED_REC 9001U            /**< Pattern seed of the recording AO_Audio brings back.        */
#define TEST_REC_FILE_BYTES 4096U      /**< Bytes of the largest file the recording stand-in writes.   */
#define TEST_FRAMES_PER_MS 48U         /**< Frames in a millisecond.                                   */
#define TEST_FRAME_BYTES 2U            /**< Bytes of a frame.                                          */
#define TEST_BYTE_BITS 8U              /**< Bits of a byte.                                            */
#define TEST_BYTE_MASK 0xFFU           /**< A byte's mask.                                             */
#define TEST_HIGH_PRIO 20U             /**< A priority above every background task.                    */
#define TEST_BUSY_NAPS 3U              /**< Naps a recorder sleeps while the codec is held.            */
#define TEST_STOP_TRIES 4U             /**< Stops a halt posts at most: the layer's own.               */
#define TEST_STOP_LOOP 64U             /**< Stops a caller looping on record_stop makes.               */
#define TEST_SET_LOOP 64U              /**< Sets a caller looping on a setting makes.                  */
#define TEST_HOLD_MAX 256U             /**< Most pool blocks the dry-pool case holds.                  */
#define TEST_REC_MS 20U                /**< A short recording, ms.                                     */
#define TEST_REC_TINY_MS 10U           /**< A shorter one.                                             */
#define TEST_REC_MID_MS 50U            /**< A recording that a new one cuts short, ms.                 */
#define TEST_REC_LONG_MS 100U          /**< A recording that does not end by itself in a case, ms.     */
#define TEST_REC_FULL_MS 500U          /**< A recording a case lets run to its end, ms.                */
#define TEST_ELAPSED_MS 30U            /**< Time a recording has run when its status is read, ms.      */
#define TEST_PAST_END_MS 5000U         /**< Time well past a recording's end, ms.                      */
#define TEST_PARTIAL_FRAMES 1000U      /**< Frames a stopped recording holds.                          */
#define TEST_SHORT_FRAMES 240U         /**< Frames a short second recording brings.                    */
#define TEST_MID_FRAMES 480U           /**< Frames the recording that replaces a running one brings.   */
#define TEST_VOLUME "/storage/"        /**< The volume path every file of a case starts with.          */

/* Stand-ins only record: an assertion in the player's task would end the wrong thread. */
/** @brief What the settle hook has seen and been told to do. */
static const char *s_fault;                   /**< A thing a stand-in could not do.            */
static bool        s_answer_audio;            /**< AO_Audio answers (false: it never does).    */
static kode_status_t s_open_rc;               /**< What a stream open answers.                 */
static kode_status_t s_write_rc;              /**< What a stream write answers.                */
static uint32_t    s_opens;                   /**< Stream opens answered.                      */
static uint32_t    s_closes;                  /**< Stream closes answered.                     */
static kdrv_es8311_info_t s_info;             /**< What the codec says it is set to.           */
static bool        s_rec_hold;                /**< A recording stays open until the gate opens. */
static uint32_t    s_rec_give;                /**< Frames a recording brings back at most.      */
static kode_status_t s_rec_rc;                /**< What a recording answers.                   */
static uint32_t    s_recs;                    /**< Recording requests seen.                    */
static uint32_t    s_rec_asked;               /**< Frames the last one asked for.              */
static uint32_t    s_rec_stops;               /**< Stop requests seen.                         */
static uint32_t    s_rec_files;               /**< File recordings seen.                       */
static uint32_t    s_rec_file_ms;             /**< Length the last one asked for.              */
static char        s_rec_file_path[KODE_PATH_MAX]; /**< Path the last one asked for.           */
static uint8_t     s_rec_body[TEST_REC_FILE_BYTES]; /**< The file the stand-in makes.          */

/** @brief The capture: every frame the stream was given, in order. */
static int16_t  s_cap[TEST_CAP_FRAMES];
static uint32_t s_cap_n;                      /**< Frames captured.                            */
static bool     s_cap_off;                    /**< The capture only counts: a long sound.      */
static uint32_t s_seen;                       /**< Frames the stream was given, kept or not.   */
static uint32_t s_writes;                     /**< Stream write calls seen.                    */
static uint32_t s_park_at;                    /**< The write call the player parks in; 0: none. */
static SemaphoreHandle_t s_gate;              /**< The parked player waits on it.              */
static SemaphoreHandle_t s_parked;            /**< Given when the player parks.                */
static atomic_bool       s_is_parked;         /**< The player waits on the gate.               */

/** @brief The WAV file a case builds. */
static uint8_t s_file[TEST_FILE_BYTES];

/* ---- The WAV builder ------------------------------------------------------- */

/** @brief What a file is made of. */
typedef struct {
    uint32_t rate;      /**< The format chunk's rate.                          */
    uint16_t channels;  /**< Its channels.                                     */
    uint16_t bits;      /**< Its sample width.                                 */
    uint16_t format;    /**< Its format tag: 1 is PCM.                         */
    uint32_t list_bytes;/**< Payload of a LIST chunk (padded to even) before the data; 0: none. */
    uint32_t declared;  /**< The data chunk's size field, bytes.               */
    uint32_t present;   /**< Sample bytes the file really holds.               */
    uint32_t seed;      /**< The sample pattern's seed.                        */
} wav_t;

/** @brief Sample @p i of the pattern @p seed. */
static int16_t smp(uint32_t seed, uint32_t i)
{
    return (int16_t)((int32_t)((seed + (i * 37U)) % 60001U) - 30000);
}

/** @brief Write @p v into @p out, @p n bytes, low first. */
static void put_le(uint8_t *out, uint32_t v, uint32_t n)
{
    for ( uint32_t i = 0U; i < n; i++ )
    {
        out[i] = (uint8_t)((v >> (8U * i)) & 0xFFU);
    }
}

/** @brief A plain file of @p frames frames of pattern @p seed: 48 kHz, mono, 16-bit PCM. */
static wav_t wav_plain(uint32_t seed, uint32_t frames)
{
    const wav_t w = { 48000U, 1U, 16U, 1U, 0U, frames * 2U, frames * 2U, seed };

    return w;
}

/** @brief Build @p w in s_file. @return Its length. */
static uint32_t wav_build(const wav_t *w)
{
    uint32_t pos = TEST_RIFF_HEADER;

    (void)memset(s_file, 0, sizeof(s_file));
    (void)memcpy(&s_file[0], "RIFF", 4U);
    (void)memcpy(&s_file[8], "WAVE", 4U);
    (void)memcpy(&s_file[pos], "fmt ", 4U);
    put_le(&s_file[pos + 4U], TEST_FMT_BODY, 4U);
    put_le(&s_file[pos + 8U], w->format, 2U);
    put_le(&s_file[pos + 10U], w->channels, 2U);
    put_le(&s_file[pos + 12U], w->rate, 4U);
    put_le(&s_file[pos + 16U], w->rate * w->channels * (w->bits / 8U), 4U);
    put_le(&s_file[pos + 20U], w->channels * (w->bits / 8U), 2U);
    put_le(&s_file[pos + 22U], w->bits, 2U);
    pos += TEST_CHUNK_HEADER + TEST_FMT_BODY;
    if ( 0U != w->list_bytes )
    {
        (void)memcpy(&s_file[pos], "LIST", 4U);
        put_le(&s_file[pos + 4U], w->list_bytes, 4U);
        (void)memset(&s_file[pos + TEST_CHUNK_HEADER], 'x', w->list_bytes);
        pos += TEST_CHUNK_HEADER + w->list_bytes + (w->list_bytes % 2U);
    }
    (void)memcpy(&s_file[pos], "data", 4U);
    put_le(&s_file[pos + 4U], w->declared, 4U);
    pos += TEST_CHUNK_HEADER;
    for ( uint32_t i = 0U; i < w->present; i++ )
    {
        const uint16_t s = (uint16_t)smp(w->seed, i / 2U);

        s_file[pos + i] = (uint8_t)(s >> ((i % 2U) * 8U));
    }
    pos += w->present;
    put_le(&s_file[4], pos - TEST_CHUNK_HEADER, 4U);
    return pos;
}

/** @brief Put @p w on the internal volume as @p name. */
static void put_wav(const char *name, const wav_t *w)
{
    fake_media_put_fs_path(name, s_file, wav_build(w));
}

/** @brief Whether capture frames from @p at are pattern @p seed from frame @p from, @p n of them. */
static bool cap_is(uint32_t at, uint32_t seed, uint32_t from, uint32_t n)
{
    bool same = ((at + n) <= s_cap_n);

    for ( uint32_t i = 0U; same && (i < n); i++ )
    {
        same = (smp(seed, from + i) == s_cap[at + i]);
    }
    return same;
}

/* ---- AO_Storage and AO_Audio, by hand ---------------------------------------- */

/** @brief Drop what sits, undispatched, in @p ao's queue. */
static void drain(QActive *ao)
{
    const QEvt *e;

    while ( (NULL != ao->eQueue) && (xQueueReceive(ao->eQueue, &e, 0U) == pdTRUE) )
    {
        QF_gc(e);
    }
}

/** @brief Let AO_Storage answer what is queued, and its worker. */
static void storage_settle(void)
{
    (void)qf_support_pump(AO_Storage, 8U);
    while ( fake_khal_worker_run_next_for("storage_sd") )
    {
        (void)qf_support_pump(AO_Storage, 8U);
    }
}

/** @brief Answer a buffer request: its outcome, its tag, its waiter. */
static void buf_answer(const AudioBufEvt *q, kode_status_t rc)
{
    if ( NULL != q->rc )
    {
        *q->rc = (uint32_t)rc;
    }
    if ( NULL != q->ack )
    {
        *q->ack = q->gen;
        /* The semaphore is NULL on the host, where the extended layer never made one. */
        khal_sem_signal(q->done);
    }
}

/** @brief Apply a setting's value to the codec's info. */
static void setting_apply(uint16_t field, int32_t value)
{
    if ( (uint16_t)KDRV_ES_VOL == field )
    {
        s_info.volume = (uint8_t)value;
    }
    else if ( (uint16_t)KDRV_ES_MIC_DB == field )
    {
        s_info.mic_db = (uint8_t)value;
    }
    else if ( (uint16_t)KDRV_ES_OUT_MUTE == field )
    {
        s_info.mute = (0 != value);
    }
    else
    {
        s_fault = "a field the layer never writes";
    }
}

/** @brief AO_Audio recording: wait if told to, then bring back the pattern. */
static void rec_answer(const AudioBufEvt *q)
{
    uint32_t n = q->frames;

    s_recs++;
    s_rec_asked = q->frames;
    if ( s_rec_hold )
    {
        atomic_store(&s_is_parked, true);
        (void)xSemaphoreGive(s_parked);
        (void)xSemaphoreTake(s_gate, pdMS_TO_TICKS(TEST_WAIT_MS));
        atomic_store(&s_is_parked, false);
    }
    if ( n > s_rec_give )
    {
        n = s_rec_give;
    }
    for ( uint32_t f = 0U; f < n; f++ )
    {
        q->buf[f] = smp(TEST_SEED_REC, f);
    }
    *q->got = n;
    *q->rc  = (uint32_t)s_rec_rc;
    *q->ack = q->gen;
    khal_sem_signal(q->done);
}

/** @brief AO_Audio recording to a file: put a WAV of the pattern on the volume. */
static void rec_file_answer(const AudioFileReqEvt *q)
{
    const uint32_t frames = q->ms * TEST_FRAMES_PER_MS;
    const uint32_t bytes  = KWAV_HEADER_BYTES + (frames * TEST_FRAME_BYTES);

    s_rec_files++;
    s_rec_file_ms = q->ms;
    (void)snprintf(s_rec_file_path, sizeof(s_rec_file_path), "%s", q->path);
    if ( (TEST_REC_FILE_BYTES >= bytes) && (KODE_OK == s_rec_rc) )
    {
        (void)kwav_build(frames, s_rec_body, sizeof(s_rec_body));
        for ( uint32_t f = 0U; f < frames; f++ )
        {
            put_le(&s_rec_body[KWAV_HEADER_BYTES + (f * TEST_FRAME_BYTES)], (uint16_t)smp(TEST_SEED_REC, f), TEST_FRAME_BYTES);
        }
        fake_media_put_fs_path(&q->path[strlen(TEST_VOLUME)], s_rec_body, bytes);
    }
    *q->bytes = bytes;
    *q->rc    = (uint32_t)s_rec_rc;
    *q->ack   = q->gen;
    khal_sem_signal(q->done);
}

/** @brief AO_Audio, by hand: everything queued is answered. */
static void audio_answer(void)
{
    const QEvt *e = NULL;

    while ( s_answer_audio && (xQueueReceive(AO_Audio->eQueue, &e, 0U) == pdTRUE) )
    {
        switch ( e->sig )
        {
        case AUDIO_STREAM_OPEN_REQ_SIG:
            s_opens++;
            /* cppcheck-suppress misra-c2012-11.3 ; the signal says which event this is */
            buf_answer((const AudioBufEvt *)e, s_open_rc);
            break;
        case AUDIO_STREAM_CLOSE_REQ_SIG:
            s_closes++;
            /* cppcheck-suppress misra-c2012-11.3 ; the signal says which event this is */
            buf_answer((const AudioBufEvt *)e, KODE_OK);
            break;
        case AUDIO_SETTING_REQ_SIG: {
            /* cppcheck-suppress misra-c2012-11.3 ; the signal says which event this is */
            const uint16_t field = ((const AudioSettingReqEvt *)e)->field;

            setting_apply(field, audio_setting_take(field));
            break;
        }
        case AUDIO_REC_REQ_SIG:
            /* cppcheck-suppress misra-c2012-11.3 ; the signal says which event this is */
            rec_answer((const AudioBufEvt *)e);
            break;
        case AUDIO_REC_STOP_REQ_SIG:
            s_rec_stops++;
            audio_rec_stop_take();
            break;
        case AUDIO_REC_FILE_REQ_SIG:
            /* cppcheck-suppress misra-c2012-11.3 ; the signal says which event this is */
            rec_file_answer((const AudioFileReqEvt *)e);
            break;
        case AUDIO_INFO_REQ_SIG: {
            /* cppcheck-suppress misra-c2012-11.3 ; the signal says which event this is */
            const AudioInfoReqEvt *const q = (const AudioInfoReqEvt *)e;

            /* cppcheck-suppress misra-c2012-11.5 ; the request lends a kdrv_es8311_info_t */
            *(kdrv_es8311_info_t *)q->out = s_info;
            *q->ack = q->gen;
            khal_sem_signal(q->done);
            break;
        }
        default:
            s_fault = "a request the layer never posts here";
            break;
        }
        QF_gc(e);
    }
}

static void settle(void);

/** @brief Arm the hook for the next wait of a sys call. */
static void settle_arm(void)
{
    fake_sem_wait_hook(&settle);
}

static atomic_bool s_release_on_wait;  /**< The next wait lets a parked player go, and gives it time to meet a lock. */

static void gate_open(void);

/** @brief Inside a wait: answer storage and the speaker, and stay armed. */
static void settle(void)
{
    if ( atomic_exchange(&s_release_on_wait, false) )
    {
        gate_open();
        vTaskDelay(TEST_MEET_TICKS);
    }
    storage_settle();
    audio_answer();
    settle_arm();
}

/** @brief The stream write of the codec: capture the frames, and park when told to. */
static kode_status_t on_stream_write(const int16_t *frames, size_t n, uint8_t channels,
                                     uint32_t ms, size_t *taken, int calls)
{
    (void)ms;
    (void)calls;
    s_writes++;
    if ( s_cap_off && (KODE_OK == s_write_rc) )
    {
        s_seen += (uint32_t)n;
        *taken  = n;
        return KODE_OK;
    }
    if ( (1U != channels) || (NULL == frames) || ((s_cap_n + n) > TEST_CAP_FRAMES) )
    {
        s_fault = "a write that is not mono frames within the capture";
        return KODE_DENIED;
    }
    if ( KODE_OK == s_write_rc )
    {
        (void)memcpy(&s_cap[s_cap_n], frames, n * sizeof(int16_t));
        s_cap_n += (uint32_t)n;
        *taken   = n;
    }
    else
    {
        *taken = 0U;
    }
    if ( (0U != s_park_at) && (s_writes == s_park_at) )
    {
        atomic_store(&s_is_parked, true);
        (void)xSemaphoreGive(s_parked);
        (void)xSemaphoreTake(s_gate, pdMS_TO_TICKS(TEST_WAIT_MS));
        atomic_store(&s_is_parked, false);
    }
    return s_write_rc;
}

/** @brief Let a parked player go. */
static void gate_open(void)
{
    if ( atomic_load(&s_is_parked) )
    {
        (void)xSemaphoreGive(s_gate);
    }
}

static void yield_release(void);

/** @brief The clock hook that lets the task under test wait on the player: release it, yield, stay armed. */
static void yield_release(void)
{
    gate_open();
    vTaskDelay(1U);
    fake_time_delay_hook(&yield_release);
}

/** @brief Wait, bounded, until the background player has ended. */
static void wait_idle(void)
{
    for ( uint32_t i = 0U; (pdMS_TO_TICKS(TEST_WAIT_MS) > i) && kode_audio_is_playing(); i++ )
    {
        vTaskDelay(1U);
    }
}

/** @brief Wait, bounded, until the player is parked. @return true when it is. */
static bool wait_parked(void)
{
    return xSemaphoreTake(s_parked, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE;
}

/** @brief Let any player of an earlier case end. */
static void player_end(void)
{
    s_park_at  = 0U;
    s_rec_hold = false;
    gate_open();
    fake_time_delay_hook(&yield_release);
    (void)kode_audio_play_stop();
    (void)kode_audio_record_stop();
    fake_time_delay_hook(NULL);
    wait_idle();
}

/** @brief Put a clean sound world together: storage, a speaker that answers, an empty capture. */
static void audio_fresh(void)
{
    /* The locks are made on the first call of the binary: make them real ones. */
    fake_mutex_real(true);
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    Audio_ctor();
    if ( NULL == s_gate )
    {
        s_gate   = xSemaphoreCreateBinary();
        s_parked = xSemaphoreCreateBinary();
    }
    player_end();

    Mockkdrv_es8311_Init();
    kdrv_es8311_stream_write_StubWithCallback(&on_stream_write);
    (void)kdrv_storage_close();
    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();
    qf_support_reap_timers();
    Storage_ctor();
    qf_support_give_queue(AO_Storage);
    AO_Storage->prio  = KODE_PRIO_STORAGE;
    AO_Storage->pthre = KODE_PRIO_STORAGE;
    QASM_INIT(AO_Storage, (void *)0, 0U);
    kbsp_release(KBSP_RES_SDMMC, AO_Storage);
    kbsp_release(KBSP_RES_FATFS, AO_Storage);
    qf_support_give_queue(AO_Audio);
    drain(AO_Storage);
    drain(AO_Audio);

    fake_media_reset();
    fake_sem_reset();
    fake_time_reset();
    while ( xSemaphoreTake(s_gate, 0U) == pdTRUE )
    {
    }
    while ( xSemaphoreTake(s_parked, 0U) == pdTRUE )
    {
    }
    atomic_store(&s_is_parked, false);
    atomic_store(&s_release_on_wait, false);
    s_fault        = NULL;
    s_answer_audio = true;
    s_open_rc      = KODE_OK;
    s_write_rc     = KODE_OK;
    s_opens        = 0U;
    s_closes       = 0U;
    s_cap_off      = false;
    s_seen         = 0U;
    s_cap_n        = 0U;
    s_writes       = 0U;
    s_park_at      = 0U;
    s_rec_hold     = false;
    s_rec_give     = UINT32_MAX;
    s_rec_rc       = KODE_OK;
    s_recs         = 0U;
    s_rec_asked    = 0U;
    s_rec_stops    = 0U;
    s_rec_files    = 0U;
    s_rec_file_ms  = 0U;
    s_rec_file_path[0] = '\0';
    (void)memset(&s_info, 0, sizeof(s_info));
    s_info.present = true;
    s_info.rate    = KWAV_RATE_HZ;
    settle_arm();
}

/** @brief Fail the case if a stand-in saw something it could not do. */
static void fault_verdict(void)
{
    if ( NULL != s_fault )
    {
        TEST_FAIL_MESSAGE(s_fault);
    }
}

/** @brief The case is over: the player ends, nothing is hooked any more. */
static void audio_done(void)
{
    player_end();
    fault_verdict();
    fake_sem_wait_hook(NULL);
    drain(AO_Audio);
}

/* ---- The blocking play ------------------------------------------------------- */

TEST_CASE("a 48 kHz mono WAV with a LIST chunk plays its exact samples through the stream",
          "[kode_audio_standard]")
{
    wav_t w = wav_plain(TEST_SEED_A, TEST_TWO_CHUNKS);

    audio_fresh();
    w.list_bytes = TEST_LIST_BYTES;
    put_wav("a.wav", &w);

    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play("flash/a.wav"), kode_error());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_opens);
    TEST_ASSERT_EQUAL_UINT32(1U, s_closes);
    TEST_ASSERT_EQUAL_UINT32(TEST_TWO_CHUNKS, s_cap_n);
    TEST_ASSERT_TRUE(cap_is(0U, TEST_SEED_A, 0U, TEST_TWO_CHUNKS));
    TEST_ASSERT_FALSE(kode_audio_is_playing());
    audio_done();
}

TEST_CASE("a WAV longer than one read window plays every sample once", "[kode_audio_standard]")
{
    const wav_t w = wav_plain(TEST_SEED_B, TEST_TWO_WINDOWS);

    audio_fresh();
    put_wav("b.wav", &w);

    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play("flash/b.wav"), kode_error());
    TEST_ASSERT_EQUAL_UINT32(TEST_TWO_WINDOWS, s_cap_n);
    TEST_ASSERT_TRUE(cap_is(0U, TEST_SEED_B, 0U, TEST_TWO_WINDOWS));
    TEST_ASSERT_EQUAL_UINT32(1U, s_closes);
    audio_done();
}

TEST_CASE("a path with another case on the volume plays", "[kode_audio_standard]")
{
    const wav_t w = wav_plain(TEST_SEED_A, TEST_CHUNK_FRAMES);

    audio_fresh();
    put_wav("c.wav", &w);

    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play("FLASH/c.wav"), kode_error());
    TEST_ASSERT_EQUAL_UINT32(TEST_CHUNK_FRAMES, s_cap_n);
    audio_done();
}

/* ---- What it refuses --------------------------------------------------------- */

/** @brief Expect the file @p name, just put on the volume, to be refused with @p text and nothing streamed. */
static void expect_refused(const char *name, const char *text)
{
    char path[KODE_FILE_PATH_MAX];

    (void)snprintf(path, sizeof(path), "flash/%s", name);
    TEST_ASSERT_FALSE(kode_audio_play(path));
    TEST_ASSERT_EQUAL_STRING(text, kode_error());
    TEST_ASSERT_FALSE(kode_audio_play_start(path));
    TEST_ASSERT_EQUAL_STRING(text, kode_error());
    TEST_ASSERT_FALSE(kode_audio_is_playing());
    TEST_ASSERT_EQUAL_UINT32(0U, s_opens);
    TEST_ASSERT_EQUAL_UINT32(0U, s_cap_n);
}

TEST_CASE("a 44.1 kHz WAV is refused with its rate", "[kode_audio_standard]")
{
    wav_t w = wav_plain(TEST_SEED_A, TEST_CHUNK_FRAMES);

    audio_fresh();
    w.rate = 44100U;
    put_wav("r.wav", &w);
    expect_refused("r.wav", "flash/r.wav: 44100 Hz; only 48000 Hz plays");
    audio_done();
}

TEST_CASE("a stereo WAV is refused", "[kode_audio_standard]")
{
    wav_t w = wav_plain(TEST_SEED_A, TEST_CHUNK_FRAMES);

    audio_fresh();
    w.channels = 2U;
    put_wav("s.wav", &w);
    expect_refused("s.wav", "flash/s.wav: stereo; only mono plays");
    audio_done();
}

TEST_CASE("an 8-bit WAV is refused", "[kode_audio_standard]")
{
    wav_t w = wav_plain(TEST_SEED_A, TEST_CHUNK_FRAMES);

    audio_fresh();
    w.bits = 8U;
    put_wav("e.wav", &w);
    expect_refused("e.wav", "flash/e.wav: 8-bit; only 16-bit plays");
    audio_done();
}

TEST_CASE("a WAV that is not plain PCM is refused", "[kode_audio_standard]")
{
    wav_t w = wav_plain(TEST_SEED_A, TEST_CHUNK_FRAMES);

    audio_fresh();
    w.format = 3U;
    put_wav("f.wav", &w);
    expect_refused("f.wav", "flash/f.wav: not plain PCM; only 16-bit PCM plays");
    audio_done();
}

TEST_CASE("a file that is not a WAV, a stub and an empty file are refused", "[kode_audio_standard]")
{
    static const char junk[] = "this is certainly not a RIFF file, however long it is made to be";
    static const char stub[] = "RIFF";

    audio_fresh();
    fake_media_put_fs_path("g.wav", junk, sizeof(junk));
    fake_media_put_fs_path("t.wav", stub, sizeof(stub) - 1U);
    fake_media_put_fs_path("z.wav", junk, 0U);
    expect_refused("g.wav", "flash/g.wav: not a WAV");
    expect_refused("t.wav", "flash/t.wav: not a WAV");
    expect_refused("z.wav", "flash/z.wav: not a WAV");
    audio_done();
}

TEST_CASE("an mp3 is refused by its name, whatever it holds", "[kode_audio_standard]")
{
    const wav_t w = wav_plain(TEST_SEED_A, TEST_CHUNK_FRAMES);

    audio_fresh();
    put_wav("m.mp3", &w);
    put_wav("M.MP3", &w);
    expect_refused("m.mp3", "MP3 is not supported yet; use a 48 kHz mono WAV");
    expect_refused("M.MP3", "MP3 is not supported yet; use a 48 kHz mono WAV");
    audio_done();
}

TEST_CASE("a missing file, a folder and a bad path are refused with the file layer's words",
          "[kode_audio_standard]")
{
    audio_fresh();
    fake_media_put_fs_dir("d");
    expect_refused("none.wav", "flash/none.wav: no such file");
    expect_refused("d", "flash/d: it is a folder");
    TEST_ASSERT_FALSE(kode_audio_play("/flash/a.wav"));
    TEST_ASSERT_EQUAL_STRING("bad path '/flash/a.wav': start with sd/ or flash/", kode_error());
    TEST_ASSERT_FALSE(kode_audio_play(NULL));
    TEST_ASSERT_EQUAL_STRING("bad path '(null)': start with sd/ or flash/", kode_error());
    audio_done();
}

/* ---- A file that is not what its header says ---------------------------------- */

TEST_CASE("a file shorter than its header says plays what is there and ends", "[kode_audio_standard]")
{
    wav_t w = wav_plain(TEST_SEED_A, 3000U);

    audio_fresh();
    w.declared = 10000U * 2U;
    put_wav("short.wav", &w);

    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play("flash/short.wav"), kode_error());
    TEST_ASSERT_EQUAL_UINT32(3000U, s_cap_n);
    TEST_ASSERT_TRUE(cap_is(0U, TEST_SEED_A, 0U, 3000U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_closes);
    audio_done();
}

TEST_CASE("a file shorter than its header says, past the first read window, ends at the file", "[kode_audio_standard]")
{
    wav_t w = wav_plain(TEST_SEED_B, TEST_TWO_WINDOWS + 5000U);

    audio_fresh();
    w.declared = 60000U * 2U;
    put_wav("long.wav", &w);

    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play("flash/long.wav"), kode_error());
    TEST_ASSERT_EQUAL_UINT32(TEST_TWO_WINDOWS + 5000U, s_cap_n);
    TEST_ASSERT_TRUE(cap_is(0U, TEST_SEED_B, 0U, TEST_TWO_WINDOWS + 5000U));
    audio_done();
}

TEST_CASE("an odd trailing byte is dropped", "[kode_audio_standard]")
{
    wav_t w = wav_plain(TEST_SEED_A, 500U);

    audio_fresh();
    w.declared = (500U * 2U) + 1U;
    w.present  = (500U * 2U) + 1U;
    put_wav("odd.wav", &w);
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play("flash/odd.wav"), kode_error());
    TEST_ASSERT_EQUAL_UINT32(500U, s_cap_n);
    TEST_ASSERT_TRUE(cap_is(0U, TEST_SEED_A, 0U, 500U));

    w.declared = 500U * 2U;
    put_wav("odd2.wav", &w);
    s_cap_n = 0U;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play("flash/odd2.wav"), kode_error());
    TEST_ASSERT_EQUAL_UINT32(500U, s_cap_n);

    w.declared = 2000U;
    put_wav("odd3.wav", &w);
    s_cap_n = 0U;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play("flash/odd3.wav"), kode_error());
    TEST_ASSERT_EQUAL_UINT32(500U, s_cap_n);
    audio_done();
}

/* ---- The speaker says no ------------------------------------------------------- */

TEST_CASE("a speaker held by another stream is busy past the bound, and nothing is lost", "[kode_audio_standard]")
{
    const wav_t w = wav_plain(TEST_SEED_A, TEST_CHUNK_FRAMES);

    audio_fresh();
    put_wav("a.wav", &w);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_audio_stream_open(KODE_SYS_AUDIO_RATE_HZ, 1U));
    fake_time_reset();
    TEST_ASSERT_FALSE(kode_audio_play("flash/a.wav"));
    TEST_ASSERT_EQUAL_STRING("audio is busy", kode_error());
    TEST_ASSERT_TRUE(kode_sys_time_ms() >= KODE_AUDIO_BUSY_MS);
    TEST_ASSERT_FALSE(kode_audio_play_start("flash/a.wav"));
    TEST_ASSERT_EQUAL_STRING("audio is busy", kode_error());
    TEST_ASSERT_FALSE(kode_audio_is_playing());
    TEST_ASSERT_EQUAL_UINT32(0U, s_cap_n);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_audio_stream_close());

    /* The speaker is free again. */
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play("flash/a.wav"), kode_error());
    TEST_ASSERT_EQUAL_UINT32(TEST_CHUNK_FRAMES, s_cap_n);
    audio_done();
}

TEST_CASE("a speaker that never answers is reported, and the claim is given back", "[kode_audio_standard]")
{
    const wav_t w = wav_plain(TEST_SEED_A, TEST_CHUNK_FRAMES);

    audio_fresh();
    put_wav("a.wav", &w);

    s_answer_audio = false;
    TEST_ASSERT_FALSE(kode_audio_play("flash/a.wav"));
    TEST_ASSERT_EQUAL_STRING("the speaker did not answer", kode_error());
    TEST_ASSERT_FALSE(kode_audio_play_start("flash/a.wav"));
    TEST_ASSERT_EQUAL_STRING("the speaker did not answer", kode_error());
    TEST_ASSERT_FALSE(kode_audio_is_playing());
    drain(AO_Audio);

    s_answer_audio = true;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play("flash/a.wav"), kode_error());
    audio_done();
}

TEST_CASE("a write the speaker does not take ends the sound and closes the stream", "[kode_audio_standard]")
{
    const wav_t w = wav_plain(TEST_SEED_A, TEST_TWO_CHUNKS);

    audio_fresh();
    put_wav("a.wav", &w);

    s_write_rc = KODE_TIMEOUT;
    TEST_ASSERT_FALSE(kode_audio_play("flash/a.wav"));
    TEST_ASSERT_EQUAL_STRING("the speaker did not answer", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_writes);
    TEST_ASSERT_EQUAL_UINT32(1U, s_opens);
    TEST_ASSERT_EQUAL_UINT32(1U, s_closes);
    audio_done();
}

/* ---- The background play --------------------------------------------------------- */

TEST_CASE("play_start plays in the background; stop ends it and closes the stream", "[kode_audio_standard]")
{
    const wav_t w = wav_plain(TEST_SEED_A, TEST_LONG_FRAMES);

    audio_fresh();
    put_wav("a.wav", &w);

    s_park_at = 1U;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play_start("flash/a.wav"), kode_error());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_TRUE(wait_parked());
    TEST_ASSERT_TRUE(kode_audio_is_playing());
    TEST_ASSERT_EQUAL_UINT32(1U, s_opens);
    TEST_ASSERT_EQUAL_UINT32(0U, s_closes);

    fake_time_delay_hook(&yield_release);
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play_stop(), kode_error());
    fake_time_delay_hook(NULL);
    TEST_ASSERT_FALSE(kode_audio_is_playing());
    TEST_ASSERT_EQUAL_UINT32(1U, s_closes);
    TEST_ASSERT_EQUAL_UINT32(TEST_CHUNK_FRAMES, s_cap_n);
    TEST_ASSERT_TRUE(cap_is(0U, TEST_SEED_A, 0U, TEST_CHUNK_FRAMES));

    /* The next play works. */
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play("flash/a.wav"), kode_error());
    TEST_ASSERT_EQUAL_UINT32(TEST_CHUNK_FRAMES + TEST_LONG_FRAMES, s_cap_n);
    audio_done();
}

TEST_CASE("a background play that is left alone plays to its end and frees the speaker", "[kode_audio_standard]")
{
    const wav_t w = wav_plain(TEST_SEED_B, TEST_TWO_WINDOWS);

    audio_fresh();
    put_wav("b.wav", &w);

    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play_start("flash/b.wav"), kode_error());
    wait_idle();
    TEST_ASSERT_FALSE(kode_audio_is_playing());
    TEST_ASSERT_EQUAL_UINT32(TEST_TWO_WINDOWS, s_cap_n);
    TEST_ASSERT_TRUE(cap_is(0U, TEST_SEED_B, 0U, TEST_TWO_WINDOWS));
    TEST_ASSERT_EQUAL_UINT32(1U, s_closes);
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play_stop(), kode_error());
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play("flash/b.wav"), kode_error());
    audio_done();
}

TEST_CASE("stop with nothing playing is true", "[kode_audio_standard]")
{
    audio_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play_stop(), kode_error());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_closes);
    audio_done();
}

TEST_CASE("a second play_start stops the first and plays the second", "[kode_audio_standard]")
{
    const wav_t a = wav_plain(TEST_SEED_A, TEST_LONG_FRAMES);
    const wav_t b = wav_plain(TEST_SEED_B, TEST_TWO_WINDOWS);

    audio_fresh();
    put_wav("a.wav", &a);
    put_wav("b.wav", &b);

    s_park_at = 1U;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play_start("flash/a.wav"), kode_error());
    TEST_ASSERT_TRUE(wait_parked());

    s_park_at = 0U;
    fake_time_delay_hook(&yield_release);
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play_start("flash/b.wav"), kode_error());
    fake_time_delay_hook(NULL);
    wait_idle();

    TEST_ASSERT_EQUAL_UINT32(2U, s_opens);
    TEST_ASSERT_EQUAL_UINT32(2U, s_closes);
    TEST_ASSERT_EQUAL_UINT32(TEST_CHUNK_FRAMES + TEST_TWO_WINDOWS, s_cap_n);
    TEST_ASSERT_TRUE(cap_is(0U, TEST_SEED_A, 0U, TEST_CHUNK_FRAMES));
    TEST_ASSERT_TRUE(cap_is(TEST_CHUNK_FRAMES, TEST_SEED_B, 0U, TEST_TWO_WINDOWS));
    audio_done();
}

TEST_CASE("a blocking play during a background play waits its turn and never mixes with it",
          "[kode_audio_standard]")
{
    const wav_t a = wav_plain(TEST_SEED_A, TEST_LONG_FRAMES);
    const wav_t b = wav_plain(TEST_SEED_B, TEST_TWO_CHUNKS);

    audio_fresh();
    put_wav("a.wav", &a);
    put_wav("b.wav", &b);

    s_park_at = 1U;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play_start("flash/a.wav"), kode_error());
    TEST_ASSERT_TRUE(wait_parked());

    s_park_at = 0U;
    fake_time_delay_hook(&yield_release);
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play("flash/b.wav"), kode_error());
    fake_time_delay_hook(NULL);
    wait_idle();

    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(2U, s_opens);
    TEST_ASSERT_EQUAL_UINT32(2U, s_closes);
    TEST_ASSERT_EQUAL_UINT32(TEST_LONG_FRAMES + TEST_TWO_CHUNKS, s_cap_n);
    TEST_ASSERT_TRUE(cap_is(0U, TEST_SEED_A, 0U, TEST_LONG_FRAMES));
    TEST_ASSERT_TRUE(cap_is(TEST_LONG_FRAMES, TEST_SEED_B, 0U, TEST_TWO_CHUNKS));
    audio_done();
}

TEST_CASE("a blocking play that waits out the whole bound while a sound plays in the background is busy",
          "[kode_audio_standard]")
{
    const wav_t a = wav_plain(TEST_SEED_A, TEST_LONG_FRAMES);
    const wav_t b = wav_plain(TEST_SEED_B, TEST_CHUNK_FRAMES);

    audio_fresh();
    put_wav("a.wav", &a);
    put_wav("b.wav", &b);

    s_park_at = 1U;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play_start("flash/a.wav"), kode_error());
    TEST_ASSERT_TRUE(wait_parked());

    TEST_ASSERT_FALSE(kode_audio_play("flash/b.wav"));
    TEST_ASSERT_EQUAL_STRING("audio is busy", kode_error());
    TEST_ASSERT_TRUE(kode_audio_is_playing());
    TEST_ASSERT_EQUAL_UINT32(TEST_CHUNK_FRAMES, s_cap_n);
    audio_done();
    TEST_ASSERT_FALSE(kode_audio_is_playing());
}

/* ---- The tone ---------------------------------------------------------------------- */

/** @brief Whether capture frames from @p at are tone @p hz from frame @p from, @p n of them. */
static bool cap_is_tone(uint32_t at, uint16_t hz, uint32_t from, uint32_t n)
{
    static int16_t want[TEST_CHUNK_FRAMES];
    bool           same = ((at + n) <= s_cap_n);

    for ( uint32_t i = 0U; same && (i < n); i += TEST_CHUNK_FRAMES )
    {
        uint32_t m = n - i;

        if ( TEST_CHUNK_FRAMES < m )
        {
            m = TEST_CHUNK_FRAMES;
        }
        same = (ktone_fill(ktone_bin_nearest(hz), from + i, TEST_TONE_AMP_Q15, want, m) == KODE_OK)
               && (memcmp(&s_cap[at + i], want, m * sizeof(int16_t)) == 0);
    }
    return same;
}

TEST_CASE("a tone writes ktone's samples through the stream and returns once it has drained",
          "[kode_audio_standard]")
{
    audio_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_tone(TEST_TONE_HZ, TEST_TONE_MS), kode_error());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_opens);
    TEST_ASSERT_EQUAL_UINT32(1U, s_closes);
    TEST_ASSERT_EQUAL_UINT32(TEST_TONE_MS * TEST_FRAMES_PER_MS, s_cap_n);
    TEST_ASSERT_TRUE(cap_is_tone(0U, TEST_TONE_HZ, 0U, s_cap_n));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Audio->eQueue));
    audio_done();
}

TEST_CASE("two tones back to back both reach the stream in full", "[kode_audio_standard]")
{
    const uint32_t frames = TEST_SHORT_TONE_MS * TEST_FRAMES_PER_MS;

    audio_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_tone(TEST_TONE_HZ, TEST_SHORT_TONE_MS), kode_error());
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_tone(TEST_TONE_HZ_B, TEST_SHORT_TONE_MS), kode_error());
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_tone(TEST_TONE_HZ, TEST_SHORT_TONE_MS), kode_error());
    TEST_ASSERT_EQUAL_UINT32(TEST_TONES, s_opens);
    TEST_ASSERT_EQUAL_UINT32(TEST_TONES, s_closes);
    TEST_ASSERT_EQUAL_UINT32(TEST_TONES * frames, s_cap_n);
    TEST_ASSERT_TRUE(cap_is_tone(0U, TEST_TONE_HZ, 0U, frames));
    TEST_ASSERT_TRUE(cap_is_tone(frames, TEST_TONE_HZ_B, 0U, frames));
    TEST_ASSERT_TRUE(cap_is_tone(2U * frames, TEST_TONE_HZ, 0U, frames));
    audio_done();
}

TEST_CASE("the longest tone and the edges of the range are accepted", "[kode_audio_standard]")
{
    audio_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_tone(KODE_AUDIO_TONE_HZ_MIN, 1U), kode_error());
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_tone(KODE_AUDIO_TONE_HZ_MAX, 1U), kode_error());
    TEST_ASSERT_EQUAL_UINT32(2U * TEST_FRAMES_PER_MS, s_cap_n);
    s_cap_off = true;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_tone(TEST_TONE_HZ, KODE_AUDIO_TONE_MAX_MS), kode_error());
    TEST_ASSERT_EQUAL_UINT32(KODE_AUDIO_TONE_MAX_MS * TEST_FRAMES_PER_MS, s_seen);
    audio_done();
}

TEST_CASE("a tone out of range is refused naming the range and touches nothing", "[kode_audio_standard]")
{
    audio_fresh();
    TEST_ASSERT_FALSE(kode_audio_tone(KODE_AUDIO_TONE_HZ_MIN - 1U, TEST_SHORT_TONE_MS));
    TEST_ASSERT_EQUAL_STRING("tone frequency is 400 to 3400 Hz", kode_error());
    TEST_ASSERT_FALSE(kode_audio_tone(KODE_AUDIO_TONE_HZ_MAX + 1U, TEST_SHORT_TONE_MS));
    TEST_ASSERT_EQUAL_STRING("tone frequency is 400 to 3400 Hz", kode_error());
    TEST_ASSERT_FALSE(kode_audio_tone(TEST_TONE_HZ, KODE_AUDIO_TONE_MAX_MS + 1U));
    TEST_ASSERT_EQUAL_STRING("tone length is 1 to 65535 ms", kode_error());
    TEST_ASSERT_FALSE(kode_audio_tone(TEST_TONE_HZ, 0U));
    TEST_ASSERT_EQUAL_STRING("tone length is 1 to 65535 ms", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_opens);
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Audio->eQueue));
    TEST_ASSERT_EQUAL_UINT32(0U, kode_sys_time_ms());
    audio_done();
}

TEST_CASE("samples from memory reach the stream exactly and return once they have drained",
          "[kode_audio_standard]")
{
    static int16_t samples[TEST_SHORT_FRAMES];

    for ( uint32_t i = 0U; TEST_SHORT_FRAMES > i; ++i )
    {
        samples[i] = (int16_t)((int32_t)(i * 37U) - 4000);
    }
    audio_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play_samples(samples, TEST_SHORT_FRAMES), kode_error());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_opens);
    TEST_ASSERT_EQUAL_UINT32(1U, s_closes);
    TEST_ASSERT_EQUAL_UINT32(TEST_SHORT_FRAMES, s_cap_n);
    TEST_ASSERT_EQUAL_INT16_ARRAY(samples, s_cap, TEST_SHORT_FRAMES);
    audio_done();
}

TEST_CASE("no samples, or samples during a background play, are refused", "[kode_audio_standard]")
{
    static int16_t one[1];
    const wav_t    w = wav_plain(TEST_SEED_A, TEST_LONG_FRAMES);

    audio_fresh();
    TEST_ASSERT_FALSE(kode_audio_play_samples(NULL, 1U));
    TEST_ASSERT_EQUAL_STRING("no samples to play", kode_error());
    TEST_ASSERT_FALSE(kode_audio_play_samples(one, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, s_opens);

    put_wav("a.wav", &w);
    s_park_at = 1U;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play_start("flash/a.wav"), kode_error());
    TEST_ASSERT_TRUE(wait_parked());
    TEST_ASSERT_FALSE(kode_audio_play_samples(one, 1U));
    TEST_ASSERT_EQUAL_STRING("audio is busy: a sound is playing", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_opens);
    audio_done();
}

TEST_CASE("a tone during a background play is refused", "[kode_audio_standard]")
{
    const wav_t w = wav_plain(TEST_SEED_A, TEST_LONG_FRAMES);

    audio_fresh();
    put_wav("a.wav", &w);
    s_park_at = 1U;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play_start("flash/a.wav"), kode_error());
    TEST_ASSERT_TRUE(wait_parked());

    TEST_ASSERT_FALSE(kode_audio_tone(TEST_TONE_HZ, TEST_SHORT_TONE_MS));
    TEST_ASSERT_EQUAL_STRING("audio is busy: a sound is playing", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_opens);
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Audio->eQueue));
    audio_done();
}

TEST_CASE("a tone waits for a speaker held by another stream, is busy past the bound, and works after",
          "[kode_audio_standard]")
{
    audio_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_audio_stream_open(KODE_SYS_AUDIO_RATE_HZ, 1U));
    fake_time_reset();
    TEST_ASSERT_FALSE(kode_audio_tone(TEST_TONE_HZ, TEST_SHORT_TONE_MS));
    TEST_ASSERT_EQUAL_STRING("audio is busy", kode_error());
    TEST_ASSERT_TRUE(kode_sys_time_ms() >= KODE_AUDIO_BUSY_MS);
    TEST_ASSERT_EQUAL_UINT32(0U, s_cap_n);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_audio_stream_close());

    TEST_ASSERT_TRUE_MESSAGE(kode_audio_tone(TEST_TONE_HZ, TEST_SHORT_TONE_MS), kode_error());
    TEST_ASSERT_EQUAL_UINT32(TEST_SHORT_TONE_MS * TEST_FRAMES_PER_MS, s_cap_n);
    audio_done();
}

TEST_CASE("a speaker that does not answer or take the tone is reported and the stream is closed",
          "[kode_audio_standard]")
{
    audio_fresh();
    s_answer_audio = false;
    TEST_ASSERT_FALSE(kode_audio_tone(TEST_TONE_HZ, TEST_SHORT_TONE_MS));
    TEST_ASSERT_EQUAL_STRING("the speaker did not answer", kode_error());
    drain(AO_Audio);

    s_answer_audio = true;
    s_write_rc     = KODE_TIMEOUT;
    TEST_ASSERT_FALSE(kode_audio_tone(TEST_TONE_HZ, TEST_SHORT_TONE_MS));
    TEST_ASSERT_EQUAL_STRING("the speaker did not answer", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_closes);

    s_write_rc = KODE_OK;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_tone(TEST_TONE_HZ, TEST_SHORT_TONE_MS), kode_error());
    audio_done();
}

/* ---- The settings -------------------------------------------------------------------- */

/** @brief Expect the next request to AO_Audio to be a write of @p field to @p value. */
static void expect_field(uint16_t field, int32_t value)
{
    const QEvt *e = NULL;

    TEST_ASSERT_TRUE(xQueueReceive(AO_Audio->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_INT((int32_t)AUDIO_SETTING_REQ_SIG, (int32_t)e->sig);
    /* cppcheck-suppress misra-c2012-11.3 ; the signal says which event this is */
    TEST_ASSERT_EQUAL_UINT16(field, ((const AudioSettingReqEvt *)e)->field);
    TEST_ASSERT_EQUAL_INT32(value, audio_setting_take(field));
    QF_gc(e);
}

TEST_CASE("set posts the three settings as setting requests", "[kode_audio_standard]")
{
    audio_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_set("volume", 40U), kode_error());
    expect_field((uint16_t)KDRV_ES_VOL, 40);
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_set("mic_gain", 18U), kode_error());
    expect_field((uint16_t)KDRV_ES_MIC_DB, 18);
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_set("mute", 1U), kode_error());
    expect_field((uint16_t)KDRV_ES_OUT_MUTE, 1);
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_set("mute", 0U), kode_error());
    expect_field((uint16_t)KDRV_ES_OUT_MUTE, 0);
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    audio_done();
}

TEST_CASE("the settings read back what was set", "[kode_audio_standard]")
{
    audio_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_set("volume", 73U), kode_error());
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_set("mic_gain", 30U), kode_error());
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_set("mute", 1U), kode_error());
    TEST_ASSERT_EQUAL_UINT32(73U, kode_audio_get("volume"));
    TEST_ASSERT_EQUAL_UINT32(30U, kode_audio_get("mic_gain"));
    TEST_ASSERT_EQUAL_UINT32(1U, kode_audio_get("mute"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    TEST_ASSERT_TRUE_MESSAGE(kode_audio_set("mute", 0U), kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_audio_get("mute"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    audio_done();
}

TEST_CASE("a setting is named in any case", "[kode_audio_standard]")
{
    audio_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_set("Volume", 12U), kode_error());
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_set("MIC_GAIN", 6U), kode_error());
    TEST_ASSERT_EQUAL_UINT32(12U, kode_audio_get("VOLUME"));
    TEST_ASSERT_EQUAL_UINT32(6U, kode_audio_get("Mic_Gain"));
    audio_done();
}

TEST_CASE("an unknown or NULL setting is refused", "[kode_audio_standard]")
{
    audio_fresh();
    TEST_ASSERT_FALSE(kode_audio_set("bass", 1U));
    TEST_ASSERT_EQUAL_STRING("unknown setting 'bass'", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_audio_get("bass"));
    TEST_ASSERT_EQUAL_STRING("unknown setting 'bass'", kode_error());
    TEST_ASSERT_FALSE(kode_audio_set(NULL, 1U));
    TEST_ASSERT_EQUAL_STRING("setting name is NULL", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_audio_get(NULL));
    TEST_ASSERT_EQUAL_STRING("setting name is NULL", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Audio->eQueue));
    audio_done();
}

TEST_CASE("a value over the limit is refused naming the limit and posts nothing", "[kode_audio_standard]")
{
    audio_fresh();
    TEST_ASSERT_FALSE(kode_audio_set("volume", KDRV_ES8311_VOL_MAX + 1U));
    TEST_ASSERT_EQUAL_STRING("volume is 0 to 100", kode_error());
    TEST_ASSERT_FALSE(kode_audio_set("mic_gain", KDRV_ES8311_MIC_DB_MAX + 1U));
    TEST_ASSERT_EQUAL_STRING("mic_gain is 0 to 42 dB", kode_error());
    TEST_ASSERT_FALSE(kode_audio_set("mute", 2U));
    TEST_ASSERT_EQUAL_STRING("mute is 0 or 1", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Audio->eQueue));

    TEST_ASSERT_TRUE_MESSAGE(kode_audio_set("volume", KDRV_ES8311_VOL_MAX), kode_error());
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_set("mic_gain", KDRV_ES8311_MIC_DB_MAX), kode_error());
    TEST_ASSERT_EQUAL_UINT32(2U, (uint32_t)uxQueueMessagesWaiting(AO_Audio->eQueue));
    audio_done();
}

TEST_CASE("64 sets of one setting queue one request and the last value is applied", "[kode_audio_standard]")
{
    audio_fresh();
    for ( uint32_t i = 0U; TEST_SET_LOOP > i; i++ )
    {
        TEST_ASSERT_TRUE_MESSAGE(kode_audio_set("volume", i), kode_error());
        TEST_ASSERT_TRUE_MESSAGE(kode_audio_set("mute", i % 2U), kode_error());
    }
    TEST_ASSERT_EQUAL_UINT32(2U, (uint32_t)uxQueueMessagesWaiting(AO_Audio->eQueue));
    expect_field((uint16_t)KDRV_ES_VOL, (int32_t)(TEST_SET_LOOP - 1U));
    expect_field((uint16_t)KDRV_ES_OUT_MUTE, 1);
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Audio->eQueue));
    audio_done();
}

TEST_CASE("a setting with no event block free is refused and says so", "[kode_audio_standard]")
{
    static const QEvt *held[TEST_HOLD_MAX];
    uint32_t           n = 0U;
    QEvt              *e;

    audio_fresh();
    e = QF_newX_((uint_fast16_t)sizeof(AudioSettingReqEvt), 0U, (enum_t)Q_USER_SIG);
    while ( (NULL != e) && (TEST_HOLD_MAX > n) )
    {
        held[n] = e;
        ++n;
        e = QF_newX_((uint_fast16_t)sizeof(AudioSettingReqEvt), 0U, (enum_t)Q_USER_SIG);
    }
    TEST_ASSERT_TRUE_MESSAGE(TEST_HOLD_MAX > n, "the pool ran dry");

    TEST_ASSERT_FALSE(kode_audio_set("volume", 20U));
    TEST_ASSERT_EQUAL_STRING("audio did not take the request", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Audio->eQueue));

    while ( 0U < n )
    {
        --n;
        QF_gc(held[n]);
    }
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_set("volume", 20U), kode_error());
    expect_field((uint16_t)KDRV_ES_VOL, 20);
    audio_done();
}

TEST_CASE("a setting reads while a stream is open", "[kode_audio_standard]")
{
    audio_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_set("volume", 55U), kode_error());
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_audio_stream_open(KODE_SYS_AUDIO_RATE_HZ, 1U));
    TEST_ASSERT_EQUAL_UINT32(55U, kode_audio_get("volume"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_audio_stream_close());
    audio_done();
}

TEST_CASE("the sys info answers while a stream is open", "[kode_audio_standard]")
{
    kdrv_es8311_info_t info;

    audio_fresh();
    s_info.volume = 66U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_audio_stream_open(KODE_SYS_AUDIO_RATE_HZ, 1U));
    (void)memset(&info, 0, sizeof(info));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_audio_info(&info));
    TEST_ASSERT_EQUAL_UINT8(66U, info.volume);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_audio_stream_close());
    audio_done();
}

TEST_CASE("reading a setting with no answer from the speaker says so", "[kode_audio_standard]")
{
    audio_fresh();
    s_answer_audio = false;
    TEST_ASSERT_EQUAL_UINT32(0U, kode_audio_get("volume"));
    TEST_ASSERT_EQUAL_STRING("the speaker did not answer", kode_error());
    drain(AO_Audio);
    audio_done();
}

TEST_CASE("a setting reads and writes during a background play", "[kode_audio_standard]")
{
    const wav_t w = wav_plain(TEST_SEED_A, TEST_LONG_FRAMES);

    audio_fresh();
    put_wav("a.wav", &w);
    s_park_at = 1U;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play_start("flash/a.wav"), kode_error());
    TEST_ASSERT_TRUE(wait_parked());

    TEST_ASSERT_TRUE_MESSAGE(kode_audio_set("volume", 20U), kode_error());
    TEST_ASSERT_EQUAL_UINT32(20U, kode_audio_get("volume"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_TRUE(kode_audio_is_playing());
    audio_done();
}

TEST_CASE("a header longer than the first read window is refused saying so", "[kode_audio_standard]")
{
    wav_t w = wav_plain(TEST_SEED_A, TEST_CHUNK_FRAMES);

    audio_fresh();
    w.list_bytes = TEST_BIG_LIST_BYTES;
    put_wav("h.wav", &w);
    expect_refused("h.wav", "flash/h.wav: its header is longer than 32 KB");
    audio_done();
}

TEST_CASE("a file over the first read window that is not a WAV is not called a long header", "[kode_audio_standard]")
{
    static uint8_t junk[TEST_BIG_LIST_BYTES];

    audio_fresh();
    (void)memset(junk, 'x', sizeof(junk));
    fake_media_put_fs_path("big.wav", junk, sizeof(junk));
    expect_refused("big.wav", "flash/big.wav: not a WAV");
    audio_done();
}

TEST_CASE("a background sound that fails ends, logs the file and the reason, and stops playing",
          "[kode_audio_standard]")
{
    const wav_t w = wav_plain(TEST_SEED_A, TEST_TWO_CHUNKS);
    const char *line;

    audio_fresh();
    put_wav("a.wav", &w);
    fake_klog_reset();
    s_write_rc = KODE_TIMEOUT;

    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play_start("flash/a.wav"), kode_error());
    wait_idle();
    TEST_ASSERT_FALSE(kode_audio_is_playing());
    TEST_ASSERT_EQUAL_UINT32(1U, s_closes);
    line = fake_klog_line(0U);
    TEST_ASSERT_NOT_NULL(line);
    TEST_ASSERT_NOT_NULL(strstr(line, "flash/a.wav"));
    TEST_ASSERT_NOT_NULL(strstr(line, "the speaker did not answer"));
    audio_done();
}

/* ---- Two tasks ------------------------------------------------------------------------ */

static SemaphoreHandle_t s_task_go;                    /**< Lets the second task begin.      */
static SemaphoreHandle_t s_task_done;                  /**< Given when its call returned.    */
static bool              s_task_ok;                    /**< What its call answered.          */
static char              s_task_error[KODE_ERROR_MAX]; /**< Its error after the call.        */
static const char       *s_task_text;                  /**< What the writer writes.          */

/** @brief The second task: write a long file, taking the file layer's turn for all of it. */
static void writer_task(void *arg)
{
    (void)arg;
    (void)xSemaphoreTake(s_task_go, pdMS_TO_TICKS(TEST_WAIT_MS));
    s_task_ok = kode_file_write("flash/other.txt", s_task_text);
    (void)snprintf(s_task_error, sizeof(s_task_error), "%s", kode_error());
    (void)xSemaphoreGive(s_task_done);
}

/** @brief The second task: a blocking play of a.wav. */
static void play_task(void *arg)
{
    (void)arg;
    (void)xSemaphoreTake(s_task_go, pdMS_TO_TICKS(TEST_WAIT_MS));
    s_task_ok = kode_audio_play("flash/a.wav");
    (void)snprintf(s_task_error, sizeof(s_task_error), "%s", kode_error());
    (void)xSemaphoreGive(s_task_done);
}

/** @brief The second task: a blocking tone. */
static void tone_task(void *arg)
{
    (void)arg;
    (void)xSemaphoreTake(s_task_go, pdMS_TO_TICKS(TEST_WAIT_MS));
    s_task_ok = kode_audio_tone(TEST_TONE_HZ, TEST_LONG_TONE_MS);
    (void)snprintf(s_task_error, sizeof(s_task_error), "%s", kode_error());
    (void)xSemaphoreGive(s_task_done);
}

/** @brief Start @p entry waiting for s_task_go. */
static void task_begin(void (*entry)(void *arg))
{
    s_task_go   = xSemaphoreCreateBinary();
    s_task_done = xSemaphoreCreateBinary();
    s_task_ok   = false;
    s_task_error[0] = '\0';
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(entry, "t_audio2", TEST_STACK_BYTES, TEST_TASK_PRIO, NULL));
}

/** @brief Let the second task go and wait for its call to return. */
static void task_end(void)
{
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreGive(s_task_go));
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_task_done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    vSemaphoreDelete(s_task_go);
    vSemaphoreDelete(s_task_done);
}

TEST_CASE("a background play reading windows and another task's file write both finish, neither busy",
          "[kode_audio_standard]")
{
    const wav_t w     = wav_plain(TEST_SEED_B, TEST_TWO_WINDOWS);
    char *const text  = (char *)malloc(TEST_OTHER_BYTES + 1U);

    TEST_ASSERT_NOT_NULL(text);
    (void)memset(text, 'W', TEST_OTHER_BYTES);
    text[TEST_OTHER_BYTES] = '\0';
    s_task_text = text;

    audio_fresh();
    put_wav("b.wav", &w);
    s_park_at = 1U;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play_start("flash/b.wav"), kode_error());
    TEST_ASSERT_TRUE(wait_parked());

    /* The writer's first wait lets the player go; the player meets the file layer's turn at its
     * second window, while the writer still holds it. */
    task_begin(&writer_task);
    atomic_store(&s_release_on_wait, true);
    task_end();
    wait_idle();

    TEST_ASSERT_TRUE(s_task_ok);
    TEST_ASSERT_EQUAL_STRING("", s_task_error);
    TEST_ASSERT_EQUAL_UINT32(TEST_OTHER_BYTES, kode_file_status("flash/other.txt").size);
    TEST_ASSERT_EQUAL_UINT32(TEST_TWO_WINDOWS, s_cap_n);
    TEST_ASSERT_TRUE(cap_is(0U, TEST_SEED_B, 0U, TEST_TWO_WINDOWS));
    TEST_ASSERT_EQUAL_UINT32(1U, s_closes);
    free(text);
    audio_done();
}

TEST_CASE("a setting is written while another task's play holds the audio turn", "[kode_audio_standard]")
{
    const wav_t w = wav_plain(TEST_SEED_A, TEST_LONG_FRAMES);

    audio_fresh();
    put_wav("a.wav", &w);
    s_park_at = 1U;
    task_begin(&play_task);
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreGive(s_task_go));
    TEST_ASSERT_TRUE(wait_parked());

    TEST_ASSERT_TRUE_MESSAGE(kode_audio_set("volume", TEST_VOLUME_PCT), kode_error());
    TEST_ASSERT_TRUE_MESSAGE(atomic_load(&s_is_parked), "the set waited for the play to end");

    gate_open();
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_task_done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    vSemaphoreDelete(s_task_go);
    vSemaphoreDelete(s_task_done);
    TEST_ASSERT_TRUE(s_task_ok);
    TEST_ASSERT_EQUAL_UINT32(TEST_VOLUME_PCT, kode_audio_get("volume"));
    audio_done();
}

TEST_CASE("a setting reads while another task's play holds the audio turn", "[kode_audio_standard]")
{
    const wav_t w = wav_plain(TEST_SEED_A, TEST_LONG_FRAMES);

    audio_fresh();
    put_wav("a.wav", &w);
    s_park_at = 1U;
    task_begin(&play_task);
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreGive(s_task_go));
    TEST_ASSERT_TRUE(wait_parked());

    TEST_ASSERT_TRUE_MESSAGE(kode_audio_set("volume", TEST_VOLUME_PCT), kode_error());
    TEST_ASSERT_EQUAL_UINT32(TEST_VOLUME_PCT, kode_audio_get("volume"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_TRUE_MESSAGE(atomic_load(&s_is_parked), "the read waited for the play to end");

    gate_open();
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_task_done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    vSemaphoreDelete(s_task_go);
    vSemaphoreDelete(s_task_done);
    audio_done();
}

TEST_CASE("a stop from another task ends a blocking play within one chunk and the next play is whole",
          "[kode_audio_standard]")
{
    const wav_t w = wav_plain(TEST_SEED_A, TEST_LONG_FRAMES);

    audio_fresh();
    put_wav("a.wav", &w);
    s_park_at = 1U;
    task_begin(&play_task);
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreGive(s_task_go));
    TEST_ASSERT_TRUE(wait_parked());

    fake_time_delay_hook(&yield_release);
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play_stop(), kode_error());
    fake_time_delay_hook(NULL);
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_task_done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    vSemaphoreDelete(s_task_go);
    vSemaphoreDelete(s_task_done);
    TEST_ASSERT_TRUE(s_task_ok);
    TEST_ASSERT_EQUAL_UINT32(TEST_CHUNK_FRAMES, s_cap_n);
    TEST_ASSERT_TRUE(cap_is(0U, TEST_SEED_A, 0U, TEST_CHUNK_FRAMES));
    TEST_ASSERT_EQUAL_UINT32(1U, s_closes);

    s_park_at = 0U;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play("flash/a.wav"), kode_error());
    TEST_ASSERT_EQUAL_UINT32(TEST_CHUNK_FRAMES + TEST_LONG_FRAMES, s_cap_n);
    audio_done();
}

TEST_CASE("a stop from another task ends a blocking tone", "[kode_audio_standard]")
{
    audio_fresh();
    s_park_at = 1U;
    task_begin(&tone_task);
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreGive(s_task_go));
    TEST_ASSERT_TRUE(wait_parked());

    fake_time_delay_hook(&yield_release);
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play_stop(), kode_error());
    fake_time_delay_hook(NULL);
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_task_done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    vSemaphoreDelete(s_task_go);
    vSemaphoreDelete(s_task_done);
    TEST_ASSERT_TRUE(s_task_ok);
    TEST_ASSERT_EQUAL_UINT32(TEST_CHUNK_FRAMES, s_cap_n);
    TEST_ASSERT_EQUAL_UINT32(1U, s_closes);
    audio_done();
}

TEST_CASE("a stop with the speaker held by another stream and nothing playing is true", "[kode_audio_standard]")
{
    audio_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_audio_stream_open(KODE_SYS_AUDIO_RATE_HZ, 1U));
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play_stop(), kode_error());
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_audio_stream_close());
    audio_done();
}

/* ---- Recording ----------------------------------------------------------------------- */

/** @brief Wait, bounded, until the background recording has ended. */
static void wait_rec_idle(void)
{
    for ( uint32_t i = 0U; (pdMS_TO_TICKS(TEST_WAIT_MS) > i) && kode_audio_record_status().recording; i++ )
    {
        vTaskDelay(1U);
    }
}

/** @brief Whether the file holds a header of @p frames frames and then the recording's pattern, as bytes. */
static bool wav_is_recording(const uint8_t *bytes, uint32_t len, uint32_t frames)
{
    kwav_info_t info;
    bool        same = ((KWAV_HEADER_BYTES + (frames * TEST_FRAME_BYTES)) == len);

    (void)memset(&info, 0, sizeof(info));
    same = same && (kwav_parse(bytes, len, &info) == KWAV_OK) && (info.frames == frames);
    for ( uint32_t f = 0U; same && (f < frames); f++ )
    {
        const uint16_t want = (uint16_t)smp(TEST_SEED_REC, f);

        const uint32_t at = KWAV_HEADER_BYTES + (f * TEST_FRAME_BYTES);

        same = ((uint8_t)(want & TEST_BYTE_MASK) == bytes[at])
               && ((uint8_t)(want >> TEST_BYTE_BITS) == bytes[at + 1U]);
    }
    return same;
}

/** @brief Read @p path and expect it to be the recording of @p frames frames. */
static void expect_recording_file(const char *path, uint32_t frames)
{
    uint32_t       len   = 0U;
    const uint8_t *bytes = kode_file_read_bytes(path, &len);

    TEST_ASSERT_NOT_NULL_MESSAGE(bytes, kode_error());
    TEST_ASSERT_TRUE_MESSAGE(wav_is_recording(bytes, len, frames), "the file is not the recording");
}

TEST_CASE("record_file asks for the file and the WAV it makes holds ms x 48 frames", "[kode_audio_standard]")
{
    audio_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_file("flash/r.wav", TEST_REC_MS), kode_error());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_rec_files);
    TEST_ASSERT_EQUAL_UINT32(TEST_REC_MS, s_rec_file_ms);
    TEST_ASSERT_EQUAL_STRING("/storage/r.wav", s_rec_file_path);
    TEST_ASSERT_EQUAL_UINT32(KWAV_HEADER_BYTES + (TEST_REC_MS * TEST_FRAMES_PER_MS * TEST_FRAME_BYTES),
                             kode_file_status("flash/r.wav").size);
    expect_recording_file("flash/r.wav", TEST_REC_MS * TEST_FRAMES_PER_MS);
    audio_done();
}

TEST_CASE("record_file refuses a length of 0 or over the limit naming it, and asks for nothing",
          "[kode_audio_standard]")
{
    audio_fresh();
    TEST_ASSERT_FALSE(kode_audio_record_file("flash/r.wav", 0U));
    TEST_ASSERT_EQUAL_STRING("record length is 1 to 30000 ms", kode_error());
    TEST_ASSERT_FALSE(kode_audio_record_file("flash/r.wav", KODE_AUDIO_RECORD_MAX_MS + 1U));
    TEST_ASSERT_EQUAL_STRING("record length is 1 to 30000 ms", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_rec_files);
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Audio->eQueue));

    /* The limit itself is accepted. */
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_file("flash/r.wav", KODE_AUDIO_RECORD_MAX_MS), kode_error());
    TEST_ASSERT_EQUAL_UINT32(KODE_AUDIO_RECORD_MAX_MS, s_rec_file_ms);
    audio_done();
}

TEST_CASE("record_file refuses a bad path with the file layer's words and asks for nothing",
          "[kode_audio_standard]")
{
    audio_fresh();
    TEST_ASSERT_FALSE(kode_audio_record_file("sd/../a.wav", TEST_REC_MS));
    TEST_ASSERT_TRUE(strlen(kode_error()) > 0U);
    TEST_ASSERT_FALSE(kode_audio_record_file(NULL, TEST_REC_MS));
    TEST_ASSERT_TRUE(strlen(kode_error()) > 0U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_rec_files);
    audio_done();
}

TEST_CASE("record_file says why a medium or a codec refused it", "[kode_audio_standard]")
{
    audio_fresh();
    s_rec_rc = KODE_IO_ERROR;
    TEST_ASSERT_FALSE(kode_audio_record_file("flash/r.wav", TEST_REC_MS));
    TEST_ASSERT_EQUAL_STRING("flash/r.wav: the volume refused the write", kode_error());
    TEST_ASSERT_FALSE(kode_file_status("flash/r.wav").exists);

    s_rec_rc       = KODE_OK;
    s_answer_audio = false;
    TEST_ASSERT_FALSE(kode_audio_record_file("flash/r.wav", TEST_REC_MS));
    TEST_ASSERT_EQUAL_STRING("the microphone did not answer", kode_error());
    audio_done();
}

TEST_CASE("record_file is busy past the bound while another holds the codec, and works after",
          "[kode_audio_standard]")
{
    audio_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_audio_stream_open(KODE_SYS_AUDIO_RATE_HZ, 1U));
    fake_time_reset();
    TEST_ASSERT_FALSE(kode_audio_record_file("flash/r.wav", TEST_REC_MS));
    TEST_ASSERT_EQUAL_STRING("audio is busy", kode_error());
    TEST_ASSERT_TRUE(kode_sys_time_ms() >= KODE_AUDIO_BUSY_MS);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_audio_stream_close());
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_file("flash/r.wav", TEST_REC_MS), kode_error());
    audio_done();
}

TEST_CASE("record_start records in the background; stop keeps the frames so far; save writes the WAV",
          "[kode_audio_standard]")
{
    kode_audio_record_status_t st;

    audio_fresh();
    s_rec_hold = true;
    s_rec_give = TEST_PARTIAL_FRAMES;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_start(TEST_REC_LONG_MS), kode_error());
    TEST_ASSERT_TRUE(wait_parked());
    TEST_ASSERT_EQUAL_UINT32(TEST_REC_LONG_MS * TEST_FRAMES_PER_MS, s_rec_asked);

    st = kode_audio_record_status();
    TEST_ASSERT_TRUE(st.recording);
    TEST_ASSERT_EQUAL_UINT32(0U, st.ms);
    TEST_ASSERT_EQUAL_UINT32(0U, st.frames);
    fake_time_advance_ms(TEST_ELAPSED_MS);
    TEST_ASSERT_EQUAL_UINT32(TEST_ELAPSED_MS, kode_audio_record_status().ms);
    fake_time_advance_ms(TEST_PAST_END_MS);
    st = kode_audio_record_status();
    TEST_ASSERT_TRUE(st.recording);
    TEST_ASSERT_EQUAL_UINT32(TEST_REC_LONG_MS, st.ms);

    fake_time_delay_hook(&yield_release);
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_stop(), kode_error());
    fake_time_delay_hook(NULL);
    TEST_ASSERT_TRUE(1U <= s_rec_stops);
    st = kode_audio_record_status();
    TEST_ASSERT_FALSE(st.recording);
    TEST_ASSERT_EQUAL_UINT32(TEST_PARTIAL_FRAMES, st.frames);
    TEST_ASSERT_EQUAL_UINT32(TEST_PARTIAL_FRAMES / TEST_FRAMES_PER_MS, st.ms);

    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_save("flash/mem.wav"), kode_error());
    expect_recording_file("flash/mem.wav", TEST_PARTIAL_FRAMES);
    audio_done();
}

TEST_CASE("a recording left alone fills its time, saves in several parts and stop is then true",
          "[kode_audio_standard]")
{
    const uint32_t frames = TEST_REC_FULL_MS * TEST_FRAMES_PER_MS;

    audio_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_start(TEST_REC_FULL_MS), kode_error());
    wait_rec_idle();
    TEST_ASSERT_FALSE(kode_audio_record_status().recording);
    TEST_ASSERT_EQUAL_UINT32(frames, kode_audio_record_status().frames);
    TEST_ASSERT_EQUAL_UINT32(TEST_REC_FULL_MS, kode_audio_record_status().ms);

    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_stop(), kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_rec_stops);
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_save("flash/long.wav"), kode_error());
    TEST_ASSERT_TRUE(KODE_SYS_FILE_READ_MAX < kode_file_status("flash/long.wav").size);
    expect_recording_file("flash/long.wav", frames);

    /* The recording is kept: it saves again, to another file. */
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_save("flash/again.wav"), kode_error());
    expect_recording_file("flash/again.wav", frames);
    audio_done();
}

TEST_CASE("stop and save with nothing recorded are false with a reason and write no file",
          "[kode_audio_standard]")
{
    audio_fresh();

    /* The memory for the recording is refused: the recording that was is dropped and none is there. */
    (void)kode_audio_record_status();
    fake_psram_refuse(1U);
    TEST_ASSERT_FALSE(kode_audio_record_start(TEST_REC_MS));
    TEST_ASSERT_EQUAL_STRING("out of memory", kode_error());
    fake_psram_refuse(0U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_recs);

    TEST_ASSERT_FALSE(kode_audio_record_stop());
    TEST_ASSERT_EQUAL_STRING("nothing was recorded", kode_error());
    TEST_ASSERT_FALSE(kode_audio_record_save("flash/n.wav"));
    TEST_ASSERT_EQUAL_STRING("nothing was recorded", kode_error());
    TEST_ASSERT_FALSE(kode_file_status("flash/n.wav").exists);
    TEST_ASSERT_FALSE(kode_audio_record_status().recording);
    TEST_ASSERT_EQUAL_UINT32(0U, kode_audio_record_status().frames);
    audio_done();
}

TEST_CASE("a recording that brought no frames cannot be saved", "[kode_audio_standard]")
{
    audio_fresh();
    s_rec_give = 0U;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_start(TEST_REC_MS), kode_error());
    wait_rec_idle();
    TEST_ASSERT_EQUAL_UINT32(0U, kode_audio_record_status().frames);
    TEST_ASSERT_FALSE(kode_audio_record_save("flash/n.wav"));
    TEST_ASSERT_EQUAL_STRING("nothing was recorded", kode_error());
    TEST_ASSERT_FALSE(kode_file_status("flash/n.wav").exists);
    audio_done();
}

TEST_CASE("record_start refuses a length of 0 or over the limit naming it", "[kode_audio_standard]")
{
    audio_fresh();
    TEST_ASSERT_FALSE(kode_audio_record_start(0U));
    TEST_ASSERT_EQUAL_STRING("record length is 1 to 30000 ms", kode_error());
    TEST_ASSERT_FALSE(kode_audio_record_start(KODE_AUDIO_RECORD_MAX_MS + 1U));
    TEST_ASSERT_EQUAL_STRING("record length is 1 to 30000 ms", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_recs);
    TEST_ASSERT_FALSE(kode_audio_record_status().recording);
    audio_done();
}

TEST_CASE("a second record_start drops the first recording", "[kode_audio_standard]")
{
    audio_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_start(TEST_REC_MS), kode_error());
    wait_rec_idle();
    TEST_ASSERT_EQUAL_UINT32(TEST_REC_MS * TEST_FRAMES_PER_MS, kode_audio_record_status().frames);

    s_rec_give = TEST_SHORT_FRAMES;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_start(TEST_REC_TINY_MS), kode_error());
    wait_rec_idle();
    TEST_ASSERT_EQUAL_UINT32(2U, s_recs);
    TEST_ASSERT_EQUAL_UINT32(TEST_SHORT_FRAMES, kode_audio_record_status().frames);
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_save("flash/two.wav"), kode_error());
    expect_recording_file("flash/two.wav", TEST_SHORT_FRAMES);
    audio_done();
}

TEST_CASE("record_start while one still records stops it and starts the new one", "[kode_audio_standard]")
{
    audio_fresh();
    s_rec_hold = true;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_start(TEST_REC_LONG_MS), kode_error());
    TEST_ASSERT_TRUE(wait_parked());

    s_rec_hold = false;
    s_rec_give = TEST_MID_FRAMES;
    fake_time_delay_hook(&yield_release);
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_start(TEST_REC_MID_MS), kode_error());
    fake_time_delay_hook(NULL);
    wait_rec_idle();
    TEST_ASSERT_TRUE(1U <= s_rec_stops);
    TEST_ASSERT_EQUAL_UINT32(2U, s_recs);
    TEST_ASSERT_EQUAL_UINT32(TEST_REC_MID_MS * TEST_FRAMES_PER_MS, s_rec_asked);
    TEST_ASSERT_EQUAL_UINT32(TEST_MID_FRAMES, kode_audio_record_status().frames);
    audio_done();
}

TEST_CASE("record_file during a background recording is refused and asks for nothing",
          "[kode_audio_standard]")
{
    audio_fresh();
    s_rec_hold = true;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_start(TEST_REC_LONG_MS), kode_error());
    TEST_ASSERT_TRUE(wait_parked());

    TEST_ASSERT_FALSE(kode_audio_record_file("flash/r.wav", TEST_REC_MS));
    TEST_ASSERT_EQUAL_STRING("a recording is already running", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_rec_files);
    TEST_ASSERT_TRUE(kode_audio_record_status().recording);
    audio_done();
}

TEST_CASE("save during a background recording is refused and writes no file", "[kode_audio_standard]")
{
    audio_fresh();
    s_rec_hold = true;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_start(TEST_REC_LONG_MS), kode_error());
    TEST_ASSERT_TRUE(wait_parked());

    TEST_ASSERT_FALSE(kode_audio_record_save("flash/s.wav"));
    TEST_ASSERT_EQUAL_STRING("a recording is already running", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Storage->eQueue));
    audio_done();
}

TEST_CASE("a recording the codec refuses ends with no frames and a warning", "[kode_audio_standard]")
{
    const char *line;

    audio_fresh();
    fake_klog_reset();
    s_rec_rc = KODE_DENIED;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_start(TEST_REC_MS), kode_error());
    wait_rec_idle();
    TEST_ASSERT_FALSE(kode_audio_record_status().recording);
    TEST_ASSERT_EQUAL_UINT32(0U, kode_audio_record_status().frames);
    line = fake_klog_line(0U);
    TEST_ASSERT_NOT_NULL(line);
    TEST_ASSERT_NOT_NULL(strstr(line, "the recording ended early"));
    TEST_ASSERT_NOT_NULL(strstr(line, "audio is busy"));
    TEST_ASSERT_FALSE(kode_audio_record_save("flash/n.wav"));
    audio_done();
}

TEST_CASE("a recording the codec fails reads as the microphone failing, not as a storage error",
          "[kode_audio_standard]")
{
    const char *line;

    audio_fresh();
    fake_klog_reset();
    s_rec_rc = KODE_IO_ERROR;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_start(TEST_REC_MS), kode_error());
    wait_rec_idle();
    line = fake_klog_line(0U);
    TEST_ASSERT_NOT_NULL(line);
    TEST_ASSERT_NOT_NULL(strstr(line, "the microphone failed"));
    TEST_ASSERT_EQUAL_UINT32(0U, kode_audio_record_status().frames);
    audio_done();
}

TEST_CASE("a recording cannot start while a sound plays in the background", "[kode_audio_standard]")
{
    const wav_t w = wav_plain(TEST_SEED_A, TEST_LONG_FRAMES);

    audio_fresh();
    put_wav("a.wav", &w);
    s_park_at = 1U;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_play_start("flash/a.wav"), kode_error());
    TEST_ASSERT_TRUE(wait_parked());

    TEST_ASSERT_FALSE(kode_audio_record_start(TEST_REC_MS));
    TEST_ASSERT_EQUAL_STRING("audio is busy: a sound is playing", kode_error());
    TEST_ASSERT_FALSE(kode_audio_record_file("flash/r.wav", TEST_REC_MS));
    TEST_ASSERT_EQUAL_STRING("audio is busy: a sound is playing", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_recs);
    TEST_ASSERT_EQUAL_UINT32(0U, s_rec_files);
    audio_done();
}

TEST_CASE("a save to a bad path fails and the recording is kept", "[kode_audio_standard]")
{
    audio_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_start(TEST_REC_MS), kode_error());
    wait_rec_idle();

    TEST_ASSERT_FALSE(kode_audio_record_save("sd/../a.wav"));
    TEST_ASSERT_TRUE(strlen(kode_error()) > 0U);
    TEST_ASSERT_FALSE(kode_audio_record_save("flash/nodir/a.wav"));
    TEST_ASSERT_NOT_NULL(strstr(kode_error(), "flash/nodir/a.wav"));
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_save("flash/ok.wav"), kode_error());
    expect_recording_file("flash/ok.wav", TEST_REC_MS * TEST_FRAMES_PER_MS);
    audio_done();
}

TEST_CASE("the status reads while another task's play holds the audio turn", "[kode_audio_standard]")
{
    const wav_t w = wav_plain(TEST_SEED_A, TEST_LONG_FRAMES);

    audio_fresh();
    put_wav("a.wav", &w);
    s_park_at = 1U;
    task_begin(&play_task);
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreGive(s_task_go));
    TEST_ASSERT_TRUE(wait_parked());

    TEST_ASSERT_FALSE(kode_audio_record_status().recording);
    TEST_ASSERT_TRUE_MESSAGE(atomic_load(&s_is_parked), "the status waited for the play to end");

    gate_open();
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_task_done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    vSemaphoreDelete(s_task_go);
    vSemaphoreDelete(s_task_done);
    audio_done();
}

TEST_CASE("64 record_stop calls against a recording queue one stop and do not assert", "[kode_audio_standard]")
{
    audio_fresh();
    s_rec_hold = true;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_start(TEST_REC_LONG_MS), kode_error());
    TEST_ASSERT_TRUE(wait_parked());

    /* AO_Audio does not run: the recorder is parked, so nothing ends. */
    for ( uint32_t i = 0U; TEST_STOP_LOOP > i; i++ )
    {
        TEST_ASSERT_FALSE(kode_audio_record_stop());
        TEST_ASSERT_EQUAL_STRING("audio is busy", kode_error());
    }
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)uxQueueMessagesWaiting(AO_Audio->eQueue));
    TEST_ASSERT_TRUE(kode_audio_record_status().recording);
    audio_done();
}

TEST_CASE("a stop that comes before the recorder asks the codec ends the recording with no frames",
          "[kode_audio_standard]")
{
    audio_fresh();

    /* This task outranks the recorder until the stop is posted, so the recorder cannot run before it. */
    const UBaseType_t prio = uxTaskPriorityGet(NULL);

    vTaskPrioritySet(NULL, TEST_HIGH_PRIO);
    TEST_ASSERT_TRUE(kode_audio_record_start(TEST_REC_LONG_MS));
    TEST_ASSERT_FALSE(kode_audio_record_stop());
    vTaskPrioritySet(NULL, prio);

    wait_rec_idle();
    TEST_ASSERT_FALSE(kode_audio_record_status().recording);
    TEST_ASSERT_EQUAL_UINT32(0U, kode_audio_record_status().frames);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, s_recs, "the codec was never asked to record");
    TEST_ASSERT_FALSE(kode_audio_record_save("flash/n.wav"));
    TEST_ASSERT_EQUAL_STRING("nothing was recorded", kode_error());

    /* The next recording is not cut short by it. */
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_start(TEST_REC_MS), kode_error());
    wait_rec_idle();
    TEST_ASSERT_EQUAL_UINT32(TEST_REC_MS * TEST_FRAMES_PER_MS, kode_audio_record_status().frames);
    audio_done();
}

/* ---- Waiting for the codec, and ending a recording that waits ---------------------------- */

static uint32_t s_naps;  /**< Naps the hook below has seen. */

/** @brief A nap with the codec held: after TEST_BUSY_NAPS of them, give the codec back. */
static void free_codec_hook(void)
{
    s_naps++;
    if ( TEST_BUSY_NAPS == s_naps )
    {
        (void)kode_block_audio_stream_close();
    }
    fake_time_delay_hook(&free_codec_hook);
}

TEST_CASE("a recording started while the codec is held waits for it and then records", "[kode_audio_standard]")
{
    audio_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_audio_stream_open(KODE_SYS_AUDIO_RATE_HZ, 1U));
    s_naps = 0U;
    fake_time_delay_hook(&free_codec_hook);
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_start(TEST_REC_MS), kode_error());
    wait_rec_idle();
    fake_time_delay_hook(NULL);

    TEST_ASSERT_TRUE(TEST_BUSY_NAPS <= s_naps);
    TEST_ASSERT_EQUAL_UINT32(1U, s_recs);
    TEST_ASSERT_EQUAL_UINT32(TEST_REC_MS * TEST_FRAMES_PER_MS, kode_audio_record_status().frames);
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_save("flash/w.wav"), kode_error());
    expect_recording_file("flash/w.wav", TEST_REC_MS * TEST_FRAMES_PER_MS);
    audio_done();
}

/** @brief A nap with the codec held: stop the recording that waits for it. */
static void stop_waiting_hook(void)
{
    s_naps++;
    if ( 1U == s_naps )
    {
        (void)kode_audio_record_stop();
    }
    fake_time_delay_hook(&stop_waiting_hook);
}

TEST_CASE("a recording that waits for the codec gives up at once when it is stopped", "[kode_audio_standard]")
{
    audio_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_audio_stream_open(KODE_SYS_AUDIO_RATE_HZ, 1U));
    fake_klog_reset();
    s_naps = 0U;
    fake_time_delay_hook(&stop_waiting_hook);
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_start(TEST_REC_MS), kode_error());
    wait_rec_idle();
    fake_time_delay_hook(NULL);

    TEST_ASSERT_FALSE(kode_audio_record_status().recording);
    TEST_ASSERT_EQUAL_UINT32(0U, kode_audio_record_status().frames);
    TEST_ASSERT_EQUAL_UINT32(0U, s_recs);
    TEST_ASSERT_NULL_MESSAGE(fake_klog_line(0U), "a stopped recording is not a failed one");
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_audio_stream_close());
    audio_done();
}

/** @brief Take the stop requests queued for AO_Audio, as it would, and count them. */
static void stops_take(void)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_Audio->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)AUDIO_REC_STOP_REQ_SIG == e->sig )
        {
            s_rec_stops++;
            audio_rec_stop_take();
        }
        else
        {
            s_fault = "a request other than a stop";
        }
        QF_gc(e);
    }
}

/** @brief A nap while a stop waits for the recorder: AO_Audio takes the stop; the third lets the recorder end. */
static void third_stop_hook(void)
{
    stops_take();
    if ( TEST_BUSY_NAPS <= s_rec_stops )
    {
        gate_open();
    }
    vTaskDelay(1U);
    fake_time_delay_hook(&third_stop_hook);
}

TEST_CASE("a stop that is lost is posted again until the recorder ends", "[kode_audio_standard]")
{
    audio_fresh();
    s_rec_hold = true;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_start(TEST_REC_LONG_MS), kode_error());
    TEST_ASSERT_TRUE(wait_parked());

    s_rec_hold = false;
    fake_time_reset();
    fake_time_delay_hook(&third_stop_hook);
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_stop(), kode_error());
    fake_time_delay_hook(NULL);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(TEST_BUSY_NAPS, s_rec_stops, "one stop a try, none once it ended");
    TEST_ASSERT_TRUE(kode_sys_time_ms() >= 2U * (KODE_AUDIO_BUSY_MS / TEST_STOP_TRIES));
    TEST_ASSERT_FALSE(kode_audio_record_status().recording);
    audio_done();
}

/** @brief A nap while a stop waits for a recorder that never ends: AO_Audio takes the stop. */
static void stops_only_hook(void)
{
    stops_take();
    fake_time_delay_hook(&stops_only_hook);
}

TEST_CASE("a recorder that never ends is asked to stop once a try and the wait is bounded", "[kode_audio_standard]")
{
    audio_fresh();
    s_rec_hold = true;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_start(TEST_REC_LONG_MS), kode_error());
    TEST_ASSERT_TRUE(wait_parked());

    fake_time_reset();
    fake_time_delay_hook(&stops_only_hook);
    TEST_ASSERT_FALSE(kode_audio_record_stop());
    fake_time_delay_hook(NULL);
    stops_take();
    TEST_ASSERT_EQUAL_STRING("audio is busy", kode_error());
    TEST_ASSERT_EQUAL_UINT32(TEST_STOP_TRIES, s_rec_stops);
    TEST_ASSERT_TRUE(kode_sys_time_ms() >= KODE_AUDIO_BUSY_MS);
    TEST_ASSERT_TRUE(kode_audio_record_status().recording);
    audio_done();
}

TEST_CASE("a sound or a tone during a background recording is refused at once and the recording goes on",
          "[kode_audio_standard]")
{
    audio_fresh();
    s_rec_hold = true;
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_record_start(TEST_REC_LONG_MS), kode_error());
    TEST_ASSERT_TRUE(wait_parked());

    fake_time_reset();
    TEST_ASSERT_FALSE(kode_audio_play("flash/a.wav"));
    TEST_ASSERT_EQUAL_STRING("audio is busy: a recording is running", kode_error());
    TEST_ASSERT_FALSE(kode_audio_play_start("flash/a.wav"));
    TEST_ASSERT_EQUAL_STRING("audio is busy: a recording is running", kode_error());
    TEST_ASSERT_FALSE(kode_audio_tone(TEST_TONE_HZ, TEST_SHORT_TONE_MS));
    TEST_ASSERT_EQUAL_STRING("audio is busy: a recording is running", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_sys_time_ms());
    TEST_ASSERT_EQUAL_UINT32(0U, s_opens);

    /* Settings still work, and the recording is still there. */
    TEST_ASSERT_TRUE_MESSAGE(kode_audio_set("volume", TEST_VOLUME_PCT), kode_error());
    TEST_ASSERT_TRUE(kode_audio_record_status().recording);
    audio_done();
}
