/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_audio.c
 * @brief Audio through AO_Audio: the commands, the requests and the blocking calls.
 */
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_sys_priv.h"

#include "kaos.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "kdrv_es8311.h"
#include "kes_fields.h"
#include "ktone.h"

#include <stddef.h>
#include <string.h>

_Static_assert(KODE_SYS_AUDIO_RATE_HZ == KDRV_ES8311_RATE_HZ,
               "the facade's sample rate is the codec's");
_Static_assert(KODE_SYS_AUDIO_BUF_SECONDS == KDRV_ES8311_BUF_SECONDS,
               "the facade's longest sound is the codec buffer's");
_Static_assert(KODE_SYS_AUDIO_STREAM_FRAMES == KDRV_ES8311_STREAM_FRAMES,
               "the facade's stream window is the codec ring's");
_Static_assert(KODE_SYS_AUDIO_HZ_MIN == KTONE_HZ_MIN, "the facade's lowest tone is the detector's");
_Static_assert(KODE_SYS_AUDIO_HZ_MAX == KTONE_HZ_MAX, "the facade's highest tone is the detector's");

/* ---- The answer's payload against the driver's type it carries ---- */
_Static_assert(sizeof(kode_sys_audio_info_t) == sizeof(kdrv_es8311_info_t), "the info is the driver's");
_Static_assert(offsetof(kode_sys_audio_info_t, present) == offsetof(kdrv_es8311_info_t, present),
               "present where the driver's is");
_Static_assert(offsetof(kode_sys_audio_info_t, open) == offsetof(kdrv_es8311_info_t, open),
               "open where the driver's is");
_Static_assert(offsetof(kode_sys_audio_info_t, busy) == offsetof(kdrv_es8311_info_t, busy),
               "busy where the driver's is");
_Static_assert(offsetof(kode_sys_audio_info_t, bits) == offsetof(kdrv_es8311_info_t, bits),
               "bits where the driver's is");
_Static_assert(offsetof(kode_sys_audio_info_t, volume) == offsetof(kdrv_es8311_info_t, volume),
               "volume where the driver's is");
_Static_assert(offsetof(kode_sys_audio_info_t, mic_db) == offsetof(kdrv_es8311_info_t, mic_db),
               "mic_db where the driver's is");
_Static_assert(offsetof(kode_sys_audio_info_t, mute) == offsetof(kdrv_es8311_info_t, mute),
               "mute where the driver's is");
_Static_assert(KODE_SYS_AUDIO_PATH_MAX == KODE_PATH_MAX, "the facade's path is AO_Audio's");

/* ---- Audio state: one claim for the waiting calls, frames staged in this file's own buffer ---- */
static khal_mutex_t s_audio_lock;             /**< Guards the busy flags and the generation. */
static khal_sem_t   s_audio_sem;              /**< The waiting calls sleep here.      */
static bool         s_audio_busy;             /**< A waiting audio call holds the claim. */
static uint32_t     s_audio_gen;              /**< Never 0.                           */
static uint32_t     s_audio_frames;           /**< What a recording brought back.     */
static uint32_t     s_audio_rc;               /**< The activity's kode_status_t.      */
static uint32_t     s_audio_ack;              /**< Whose answer that is.              */
static khal_sem_t   s_audio_info_sem;         /**< kode_block_audio_info() sleeps here. */
static bool         s_audio_info_busy;        /**< An info call holds its claim.      */
static uint32_t     s_audio_info_ack;         /**< Whose info answer that is.         */
static kdrv_es8311_info_t s_audio_info;       /**< What AO_Audio answers an info call into. */
static uint32_t     s_audio_bytes;            /**< The length kode_block_audio_record_file() is told. */
static int16_t     *s_audio_stage;            /**< See audio_stage().                 */

/** @brief How long kode_block_audio_info() waits for AO_Audio: a liveness bound. */
#define KODE_AUDIO_INFO_WAIT_MS  100U
/** @brief On top of a sound's own length: opening the codec and the settling either side of it. */
#define KODE_AUDIO_WAIT_MARGIN_MS  3000U
/** @brief On top of that again when the sound goes to a medium: one chunked whole-buffer write onto FAT. */
#define KODE_AUDIO_FILE_MARGIN_MS 30000U

void kode_sys_audio_init(void)
{
    s_audio_sem       = khal_sem_create();
    s_audio_lock      = khal_mutex_create();
    s_audio_busy      = false;
    s_audio_gen       = 0U;
    s_audio_info_sem  = khal_sem_create();
    s_audio_info_busy = false;
    s_audio_info_ack  = 0U;
}

/*
 * ---- Audio ----
 * The calls that wait share one claim, one semaphore and one generation counter,
 * and move their frames through a buffer of this file's: the driver's buffer
 * belongs to AO_Audio and its worker alone.
 */

/** @brief Alignment of the staging buffer. */
#define KODE_AUDIO_STAGE_ALIGN  64U

/** @brief Frames per millisecond at the one sample rate. */
#define KODE_AUDIO_FRAMES_PER_MS  (KODE_SYS_AUDIO_RATE_HZ / KODE_SYS_MS_PER_S)

/** @brief The longest sound one call carries, in milliseconds. */
#define KODE_AUDIO_MS_MAX  (KODE_SYS_AUDIO_BUF_SECONDS * KODE_SYS_MS_PER_S)

/**
 * @brief Draw the next audio generation. Call with s_audio_lock held.
 * @return A tag that is never 0.
 */
static uint32_t audio_gen_bump(void)
{
    s_audio_gen += 1U;
    if ( 0U == s_audio_gen )
    {
        s_audio_gen = 1U;
    }
    return s_audio_gen;
}

/**
 * @brief Take the audio claim and draw a generation for this call.
 * @param[out] gen This call's own tag.
 * @return true when this caller took it, and must release it again.
 */
static bool audio_claim(uint32_t *gen)
{
    bool took = false;

    khal_mutex_lock(s_audio_lock);
    if ( !s_audio_busy )
    {
        s_audio_busy = true;
        took         = true;
        *gen         = audio_gen_bump();
    }
    khal_mutex_unlock(s_audio_lock);
    return took;
}

/** @brief Give the audio claim back. */
static void audio_release(void)
{
    khal_mutex_lock(s_audio_lock);
    s_audio_busy = false;
    khal_mutex_unlock(s_audio_lock);
}

/**
 * @brief Take the info claim and draw a generation for this call.
 * @param[out] gen This call's own tag.
 * @return true when this caller took it, and must release it again.
 */
static bool audio_info_claim(uint32_t *gen)
{
    bool took = false;

    khal_mutex_lock(s_audio_lock);
    if ( !s_audio_info_busy )
    {
        s_audio_info_busy = true;
        took              = true;
        *gen              = audio_gen_bump();
    }
    khal_mutex_unlock(s_audio_lock);
    return took;
}

/** @brief Give the info claim back. */
static void audio_info_release(void)
{
    khal_mutex_lock(s_audio_lock);
    s_audio_info_busy = false;
    khal_mutex_unlock(s_audio_lock);
}

/**
 * @brief How long to wait for @p frames to move: their own length plus KODE_AUDIO_WAIT_MARGIN_MS.
 * @param frames How many.
 * @return Milliseconds.
 */
static uint32_t audio_wait_ms(uint32_t frames)
{
    return (frames / KODE_AUDIO_FRAMES_PER_MS) + KODE_AUDIO_WAIT_MARGIN_MS;
}

/**
 * @brief The staging buffer the waiting calls move frames through: PSRAM, taken on first use, never freed.
 * @return The buffer, KODE_SYS_AUDIO_MAX_FRAMES long, or NULL.
 */
static int16_t *audio_stage(void)
{
    if ( NULL == s_audio_stage )
    {
        s_audio_stage = (int16_t *)khal_alloc_psram(
            KODE_AUDIO_STAGE_ALIGN, (size_t)KODE_SYS_AUDIO_MAX_FRAMES * sizeof(int16_t));
    }
    return s_audio_stage;
}

kode_status_t kode_sys_audio_tone(uint16_t hz, uint16_t ms, uint8_t amp_pct)
{
    if ( (KODE_SYS_AUDIO_HZ_MIN > hz) || (KODE_SYS_AUDIO_HZ_MAX < hz) )
    {
        return KODE_DENIED;
    }
    if ( (0U == ms) || (0U == amp_pct) || (KODE_SYS_AUDIO_AMP_MAX_PCT < amp_pct) )
    {
        return KODE_DENIED;
    }
    return kode_sys_cmd_post(AO_Audio, audio_tone_evt(hz, ms, amp_pct),
                             KQP_AUDIO_CMD_MARGIN);
}

/**
 * @brief What a setting's request came to.
 * @param taken What audio_setting_request() answered.
 * @return KODE_OK when taken, KODE_NO_RESOURCES when the pool was dry.
 */
static kode_status_t audio_setting_rc(bool taken)
{
    kode_status_t rc = KODE_NO_RESOURCES;

    if ( taken )
    {
        rc = KODE_OK;
    }
    return rc;
}

kode_status_t kode_sys_audio_volume(uint8_t pct)
{
    if ( KDRV_ES8311_VOL_MAX < pct )
    {
        return KODE_DENIED;
    }
    return audio_setting_rc(audio_setting_request((uint16_t)KDRV_ES_VOL, (int32_t)pct));
}

kode_status_t kode_sys_audio_mic_gain(uint8_t db)
{
    if ( KDRV_ES8311_MIC_DB_MAX < db )
    {
        return KODE_DENIED;
    }
    return audio_setting_rc(audio_setting_request((uint16_t)KDRV_ES_MIC_DB, (int32_t)db));
}

kode_status_t kode_sys_audio_mute(bool on)
{
    int32_t value = 0;

    if ( on )
    {
        value = 1;
    }
    return audio_setting_rc(audio_setting_request((uint16_t)KDRV_ES_OUT_MUTE, value));
}

void kode_sys_audio_record_stop(void)
{
    audio_rec_stop_request();
}

kode_status_t kode_block_audio_info(kdrv_es8311_info_t *out)
{
    kode_status_t rc  = KODE_TIMEOUT;
    uint32_t      gen = 0U;

    if ( NULL == out )
    {
        return KODE_DENIED;
    }
    if ( !audio_info_claim(&gen) )
    {
        return KODE_DENIED;
    }
    /* AO_Audio fills this file's struct, not the caller's: the wait is bounded and can end first. */
    audio_info_request((void *)&s_audio_info, s_audio_info_sem, gen, &s_audio_info_ack);
    if ( kode_sys_answer_await(s_audio_info_sem, &s_audio_info_ack, gen, KODE_AUDIO_INFO_WAIT_MS) )
    {
        *out = s_audio_info;
        rc   = KODE_OK;
    }
    audio_info_release();
    return rc;
}

kode_status_t kode_block_audio_record_file(const char *path, uint32_t ms,
                                     uint32_t *bytes)
{
    kode_status_t rc  = KODE_TIMEOUT;
    uint32_t      gen = 0U;

    if ( (NULL == path) || (0U == ms) )
    {
        return KODE_DENIED;
    }
    if ( !audio_claim(&gen) )
    {
        return KODE_DENIED;
    }
    /* The length is lent from this file's storage: AO_Audio writes it once the file is out. */
    s_audio_rc    = (uint32_t)KODE_ERROR;
    s_audio_bytes = 0U;
    audio_record_file_request(path, ms, &s_audio_rc, &s_audio_bytes,
                              s_audio_sem, gen, &s_audio_ack);

    /* The recording's length, the codec's open, and a write that can be a megabyte onto FAT. */
    if ( kode_sys_answer_await(s_audio_sem, &s_audio_ack, gen,
                              audio_wait_ms(ms * KODE_AUDIO_FRAMES_PER_MS)
                              + KODE_AUDIO_FILE_MARGIN_MS) )
    {
        rc = (kode_status_t)s_audio_rc;
        if ( NULL != bytes )
        {
            *bytes = s_audio_bytes;
        }
    }
    audio_release();
    return rc;
}

/* ---- The stream: the open takes the audio claim and the close gives it back ---- */

static bool    s_stream_on;       /**< Open, and the claim is the stream's.  */
static uint8_t s_stream_channels; /**< What its writer said it writes.       */

kode_status_t kode_block_audio_stream_open(uint32_t rate_hz, uint8_t channels)
{
    kode_status_t rc  = KODE_TIMEOUT;
    uint32_t      gen = 0U;

    if ( (KODE_SYS_AUDIO_RATE_HZ != rate_hz)
        || ((KODE_SYS_AUDIO_CHANNELS_MONO != channels)
            && (KODE_SYS_AUDIO_CHANNELS_STEREO != channels)) )
    {
        return KODE_DENIED;
    }
    if ( !audio_claim(&gen) )
    {
        return KODE_DENIED;
    }
    s_audio_rc = (uint32_t)KODE_ERROR;
    audio_stream_open_request(&s_audio_rc, s_audio_sem, gen, &s_audio_ack);
    if ( kode_sys_answer_await(s_audio_sem, &s_audio_ack, gen, KODE_AUDIO_WAIT_MARGIN_MS) )
    {
        rc = (kode_status_t)s_audio_rc;
    }
    else
    {
        /* A stream that starts after this gives up has nobody to close it: the queued close shuts it down. */
        audio_stream_close_request(NULL, NULL, 0U, NULL);
    }
    if ( KODE_OK == rc )
    {
        s_stream_on       = true;
        s_stream_channels = channels;
    }
    else
    {
        audio_release();
    }
    return rc;
}

kode_status_t kode_block_audio_stream_write(const int16_t *frames, size_t n,
                                      uint32_t ms, size_t *taken)
{
    if ( NULL != taken )
    {
        *taken = 0U;
    }
    if ( !s_stream_on )
    {
        return KODE_DENIED;
    }
    return kdrv_es8311_stream_write(frames, n, s_stream_channels, ms, taken);
}

kode_status_t kode_block_audio_stream_close(void)
{
    kode_status_t rc = KODE_TIMEOUT;
    uint32_t      gen;

    if ( !s_stream_on )
    {
        return KODE_DENIED;
    }
    /* A generation of its own, so the open's answer is never taken for this one. */
    khal_mutex_lock(s_audio_lock);
    gen = audio_gen_bump();
    khal_mutex_unlock(s_audio_lock);

    s_audio_rc = (uint32_t)KODE_ERROR;
    audio_stream_close_request(&s_audio_rc, s_audio_sem, gen, &s_audio_ack);
    /* What the ring still holds, the drain behind it, and the usual margin. */
    if ( kode_sys_answer_await(s_audio_sem, &s_audio_ack, gen,
                              audio_wait_ms((uint32_t)KODE_SYS_AUDIO_STREAM_FRAMES)) )
    {
        rc = (kode_status_t)s_audio_rc;
    }
    s_stream_on = false;
    audio_release();
    return rc;
}

/* ---- kode_sys's requests: AO_Audio answers the asking AO; frames are lent ---- */

kode_status_t kode_sys_audio_info_read(QActive *me)
{
    return audio_info_ask(me);
}

kode_status_t kode_sys_audio_play(QActive *me, const int16_t *frames, size_t n)
{
    if ( (NULL == frames) || (0U == n) || ((size_t)KODE_SYS_AUDIO_MAX_FRAMES < n) )
    {
        return KODE_DENIED;
    }
    return audio_play_ask(me, frames, (uint32_t)n);
}

/**
 * @brief @p ms capped at the longest sound one call carries.
 * @param ms Milliseconds asked for.
 * @return At most KODE_AUDIO_MS_MAX.
 */
static uint32_t audio_ms_capped(uint32_t ms)
{
    uint32_t capped = ms;

    if ( KODE_AUDIO_MS_MAX < capped )
    {
        capped = KODE_AUDIO_MS_MAX;
    }
    return capped;
}

kode_status_t kode_sys_audio_record(QActive *me, int16_t *buf, size_t cap, uint32_t ms)
{
    uint32_t want;

    if ( (NULL == buf) || (0U == cap) || (0U == ms) )
    {
        return KODE_DENIED;
    }
    want = audio_ms_capped(ms) * KODE_AUDIO_FRAMES_PER_MS;
    if ( (size_t)want > cap )
    {
        want = (uint32_t)cap;
    }
    return audio_record_ask(me, buf, want);
}

kode_status_t kode_sys_audio_record_file(QActive *me, const char *path, uint32_t ms)
{
    if ( !kode_sys_text_fits(path, KODE_SYS_AUDIO_PATH_MAX) || (0U == ms) )
    {
        return KODE_DENIED;
    }
    return audio_record_file_ask(me, path, audio_ms_capped(ms));
}

kode_status_t kode_sys_audio_stream_open(QActive *me)
{
    return audio_stream_open_ask(me);
}

kode_status_t kode_sys_audio_stream_write(QActive const *me, const int16_t *frames, size_t n,
                                          uint8_t channels, size_t *taken)
{
    if ( NULL != taken )
    {
        *taken = 0U;
    }
    /* The opener is AO_Audio's to say; the ring is the driver's. */
    if ( !audio_stream_writer_is(me) )
    {
        return KODE_DENIED;
    }
    return kdrv_es8311_stream_write(frames, n, channels, 0U, taken);
}

kode_status_t kode_sys_audio_stream_close(QActive *me)
{
    return audio_stream_close_ask(me);
}

kode_status_t kode_block_audio_record_samples(int16_t *out, size_t cap, size_t *n,
                                uint32_t ms)
{
    kode_status_t rc  = KODE_TIMEOUT;
    uint32_t      gen = 0U;
    uint32_t      want;
    int16_t      *stage;

    if ( (NULL == out) || (0U == cap) || (0U == ms) )
    {
        return KODE_DENIED;
    }
    want = ms * KODE_AUDIO_FRAMES_PER_MS;
    if ( (uint32_t)cap < want )
    {
        want = (uint32_t)cap;
    }
    if ( (uint32_t)KODE_SYS_AUDIO_MAX_FRAMES < want )
    {
        want = (uint32_t)KODE_SYS_AUDIO_MAX_FRAMES;
    }
    if ( !audio_claim(&gen) )
    {
        return KODE_DENIED;
    }
    stage = audio_stage();
    if ( NULL == stage )
    {
        audio_release();
        return KODE_NO_RESOURCES;
    }
    /* Recorded into this file's buffer: a file recording accepted meanwhile owns the driver's. */
    s_audio_rc     = (uint32_t)KODE_ERROR;
    s_audio_frames = 0U;
    audio_record_request(stage, want, &s_audio_frames, &s_audio_rc,
                         s_audio_sem, gen, &s_audio_ack);

    if ( kode_sys_answer_await(s_audio_sem, &s_audio_ack, gen, audio_wait_ms(want)) )
    {
        /* The count came from another AO: clamp it to what was asked for. */
        uint32_t got = s_audio_frames;
        if ( got > want )
        {
            got = want;
        }
        if ( 0U < got )
        {
            (void)memcpy(out, stage, (size_t)got * sizeof(int16_t));
        }
        if ( NULL != n )
        {
            *n = (size_t)got;
        }
        rc = (kode_status_t)s_audio_rc;
    }
    audio_release();
    return rc;
}
