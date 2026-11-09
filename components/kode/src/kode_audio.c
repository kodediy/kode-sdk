/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_audio.c
 * @brief The standard layer's sound: WAV files, tones and the settings by name.
 */
#include "kode.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "kdrv_es8311.h"
#include "khal_once.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "kode_entry.h"
#include "kode_file.h"
#include "kode_log.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kqp.h"
#include "ktone.h"
#include "kwav.h"

/** @brief The log tag. */
#define TAG "kode"

/** @brief Milliseconds in a second. */
#define AUDIO_MS_PER_S 1000U

/** @brief Frames in a millisecond. */
#define AUDIO_FRAMES_PER_MS (KODE_SYS_AUDIO_RATE_HZ / AUDIO_MS_PER_S)

/** @brief Length of the chunk the player hands the stream at a time, ms: a stop is heard within one. */
#define AUDIO_CHUNK_MS 100U

/** @brief Frames in one chunk. */
#define AUDIO_CHUNK_FRAMES (AUDIO_FRAMES_PER_MS * AUDIO_CHUNK_MS)

/** @brief Bytes in a KB, for the text of the window's size. */
#define AUDIO_KB 1024U

/** @brief The longest one write waits for room in the stream, ms. */
#define AUDIO_WRITE_MS 1000U

/** @brief Bytes of a background task's stack. */
#define AUDIO_TASK_STACK_BYTES 8192U

/** @brief A background task's level; kqp_config.h says why. */
#define AUDIO_TASK_PRIO KODE_PRIO_STANDARD_AUDIO

/** @brief The name of the player's task. */
#define AUDIO_TASK_NAME "kode_play"

/** @brief The name of the recorder's task. */
#define AUDIO_REC_TASK_NAME "kode_rec"

/** @brief Times a halt asks a recording to stop, one after another. */
#define AUDIO_STOP_TRIES 4U

/** @brief How long a halt waits for the recorder after each ask, ms. */
#define AUDIO_STOP_WAIT_MS (KODE_AUDIO_BUSY_MS / AUDIO_STOP_TRIES)

/** @brief The longest recording the extended layer holds, ms. */
#define AUDIO_REC_MAX_MS (KODE_SYS_AUDIO_BUF_SECONDS * AUDIO_MS_PER_S)

/** @brief Bytes of a RIFF tag. */
#define AUDIO_RIFF_TAG_BYTES 4U

/** @brief Where the WAVE tag sits in a RIFF file. */
#define AUDIO_RIFF_WAVE_OFF 8U

/** @brief Bytes of the RIFF header: a tag, a size and the WAVE tag. */
#define AUDIO_RIFF_BYTES 12U

/** @brief Bits in a byte. */
#define AUDIO_BYTE_BITS 8U

/** @brief The low byte of a sample. */
#define AUDIO_BYTE_MASK 0xFFU

/** @brief A tone's peak, percent of full scale: the production row's quarter. */
#define AUDIO_TONE_AMP_PCT 25U

/** @brief The peak of a full-scale sample. */
#define AUDIO_FULL_SCALE 32767

/** @brief Percent in the whole. */
#define AUDIO_PERCENT 100

/** @brief A tone's peak, out of 32768. */
#define AUDIO_TONE_AMP_Q15 ((int16_t)(((int32_t)AUDIO_TONE_AMP_PCT * AUDIO_FULL_SCALE) / AUDIO_PERCENT))

/** @brief The highest value of a switch. */
#define AUDIO_SWITCH_MAX 1U

/** @brief Bytes of one mono 16-bit frame. */
#define AUDIO_FRAME_BYTES ((uint32_t)sizeof(int16_t))

/** @brief Channels of a stereo file. */
#define AUDIO_STEREO 2U

/** @brief The extension that is refused for what it is. */
#define AUDIO_MP3_EXT ".mp3"

/** @brief What a .mp3 fails with. */
#define AUDIO_MP3_TEXT "MP3 is not supported yet; use a 48 kHz mono WAV"

/** @brief What a call that never found the speaker free fails with. */
#define AUDIO_BUSY_TEXT "audio is busy"

/** @brief What a call that found a background sound in its way fails with. */
#define AUDIO_PLAYING_TEXT "audio is busy: a sound is playing"

/** @brief What a speaker that stopped answering is called. */
#define AUDIO_DEAF_TEXT "the speaker did not answer"

/** @brief What a microphone that stopped answering is called. */
#define AUDIO_MIC_DEAF_TEXT "the microphone did not answer"

/** @brief What a microphone that failed is called. */
#define AUDIO_MIC_FAIL_TEXT "the microphone failed"

/** @brief What a record call that found a recording in its way fails with. */
#define AUDIO_RECORDING_TEXT "a recording is already running"

/** @brief What a sound that found a recording in its way fails with. */
#define AUDIO_RECORDING_BUSY_TEXT "audio is busy: a recording is running"

/** @brief What a save or a stop with no recording fails with. */
#define AUDIO_NOTHING_TEXT "nothing was recorded"

_Static_assert(KODE_AUDIO_TONE_HZ_MIN == KTONE_HZ_MIN, "the lowest tone is the board's");
_Static_assert(KODE_AUDIO_TONE_HZ_MAX == KTONE_HZ_MAX, "the highest tone is the board's");
_Static_assert((KODE_SYS_FILE_READ_MAX % AUDIO_FRAME_BYTES) == 0U, "a read window holds whole frames");
_Static_assert(KODE_SYS_AUDIO_RATE_HZ == KWAV_RATE_HZ, "the stream and the WAV reader speak one rate");
_Static_assert(KODE_AUDIO_RECORD_MAX_MS == AUDIO_REC_MAX_MS, "the longest recording is the extended layer's");
_Static_assert((AUDIO_CHUNK_FRAMES * (uint32_t)sizeof(int16_t)) <= KODE_SYS_FILE_READ_MAX,
               "a chunk fits the window");

/** @brief The settings' names, as the caller types them, in AUDIO_SET_* order. */
enum {
    AUDIO_SET_VOLUME = 0,  /**< volume   */
    AUDIO_SET_MIC_GAIN,    /**< mic_gain */
    AUDIO_SET_MUTE,        /**< mute     */
    AUDIO_SET_COUNT        /**< How many there are. */
};

/** @brief One sound being played, from the file's start to its end. */
typedef struct {
    char               user[KODE_FILE_PATH_MAX];  /**< The path as typed, for the errors.          */
    char               vol[KODE_PATH_MAX];        /**< The volume path.                            */
    void              *buf;                       /**< The window, KODE_SYS_FILE_READ_MAX bytes.   */
    uint32_t           size;                      /**< The file's size when it was asked about.    */
    uint32_t           next;                      /**< Where the file's next window starts.        */
    uint32_t           left;                      /**< Sample bytes still in the file after the window. */
    uint32_t           held;                      /**< Sample bytes in the window not yet written. */
    const atomic_bool *stop;                      /**< Ends the sound when set; NULL: never.       */
} play_t;

/** @brief What one try carries. */
typedef struct {
    kode_status_t rc;  /**< The last answer. */
} try_t;

/** @brief The lock every public audio call holds; made on the first call. */
static khal_mutex_t s_audio_lock;

/** @brief The background sound; the caller fills it before its task starts and the task alone uses it after. */
static play_t s_bg;

/** @brief The window of the sound a call plays and waits for; PSRAM, taken on first use and kept. */
static void *s_fg_window;

/** @brief The background task exists, from before it starts to its last statement. */
static atomic_bool s_bg_running;

/** @brief A play or a tone a call waits for is sounding, from its stream opening to its closing. */
static atomic_bool s_fg_running;

/** @brief The sound, background or not, is to end; cleared as a sound arms. */
static atomic_bool s_sound_stop;

/** @brief The recording in memory: a WAV image, the header and then the frames as bytes. */
typedef struct {
    uint8_t *image;  /**< KWAV_HEADER_BYTES and cap frames; NULL: none.  */
    uint32_t cap;    /**< Frames the image holds.                        */
} rec_t;

/** @brief The recording; the caller sets it, under the audio lock, before the task starts, and the task alone uses it after. */
static rec_t s_rec;

/** @brief The recorder's task exists, from before it starts to its last statement. */
static atomic_bool s_rec_running;

/** @brief The recording is to end; set before the stop is posted. */
static atomic_bool s_rec_stop;

/** @brief Frames the recording brought, set when it ends. */
static atomic_uint_least32_t s_rec_got;

/** @brief The time the recording started, ms. */
static atomic_uint_least32_t s_rec_t0;

/** @brief The length the recording was asked for, ms. */
static atomic_uint_least32_t s_rec_max_ms;

/** @brief Make the audio lock. @return true when it exists. */
static bool audio_lock_make(void)
{
    s_audio_lock = khal_mutex_create();
    return NULL != s_audio_lock;
}

/**
 * @brief The prologue of a public audio call: kode_enter(), then its turn.
 * @param name The public name of the call.
 * @return true when the call has its turn; false with the error set.
 */
static bool audio_enter(const char *name)
{
    static khal_once_t lock_once;  /* the one-shot the lock is made by */
    bool               ok = kode_enter(name);

    if ( ok )
    {
        ok = khal_once_try(&lock_once, &audio_lock_make);
        if ( ok )
        {
            khal_mutex_lock(s_audio_lock);
        }
        else
        {
            kode_err_set("%s: no memory for the audio lock", name);
        }
    }
    return ok;
}

/** @brief Give the turn back, at the end of a call audio_enter() let in. */
static void audio_leave(void)
{
    khal_mutex_unlock(s_audio_lock);
}

/**
 * @brief The window in @p slot, taken from PSRAM the first time.
 * @param slot Where the window is kept.
 * @return The window; NULL with the error set if PSRAM has none to give.
 */
static void *window_get(void **slot)
{
    if ( NULL == *slot )
    {
        *slot = khal_alloc_psram(sizeof(void *), KODE_SYS_FILE_READ_MAX);
        if ( NULL == *slot )
        {
            kode_err_set("%s", kode_file_why(KODE_NO_RESOURCES));
        }
    }
    return *slot;
}

/** @brief Whether @p path ends in ".mp3", in any case. */
static bool is_mp3(const char *path)
{
    const uint32_t len = (uint32_t)strlen(path);
    const uint32_t ext = (uint32_t)strlen(AUDIO_MP3_EXT);
    bool           same = (len >= ext);

    for ( uint32_t i = 0U; same && (i < ext); i++ )
    {
        same = (kode_ascii_lower(path[(len - ext) + i]) == AUDIO_MP3_EXT[i]);
    }
    return same;
}

/**
 * @brief Whether the four bytes at @p bytes are the tag @p tag.
 * @param bytes At least AUDIO_RIFF_TAG_BYTES bytes.
 * @param tag   AUDIO_RIFF_TAG_BYTES characters.
 * @return true when they match.
 */
static bool tag_is(const uint8_t *bytes, const char *tag)
{
    bool same = true;

    for ( uint32_t i = 0U; same && (AUDIO_RIFF_TAG_BYTES > i); i++ )
    {
        same = ((uint8_t)tag[i] == bytes[i]);
    }
    return same;
}

/**
 * @brief Whether the window starts a RIFF/WAVE file.
 * @param p   The sound, its window just read.
 * @param got Bytes read.
 * @return true when the file's first twelve bytes say so.
 */
static bool riff_seen(const play_t *p, uint32_t got)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the window holds the file's bytes */
    const uint8_t *const bytes = (const uint8_t *)p->buf;

    return (AUDIO_RIFF_BYTES <= got) && tag_is(&bytes[0], "RIFF") && tag_is(&bytes[AUDIO_RIFF_WAVE_OFF], "WAVE");
}

/**
 * @brief Keep why a file was refused as the task's error.
 * @param p      The sound.
 * @param got    Bytes of the first window read.
 * @param reason What kwav_parse() said.
 * @param info   What it found.
 */
static void wav_refused(const play_t *p, uint32_t got, kwav_reason_t reason, const kwav_info_t *info)
{
    const char *const user = p->user;

    switch ( reason )
    {
    case KWAV_BAD_RATE:
        kode_err_set("%s: %" PRIu32 " Hz; only %" PRIu32 " Hz plays", user, info->rate,
                     (uint32_t)KWAV_RATE_HZ);
        break;
    case KWAV_BAD_BITS:
        kode_err_set("%s: %" PRIu32 "-bit; only %" PRIu32 "-bit plays", user, (uint32_t)info->bits,
                     (uint32_t)KWAV_BITS);
        break;
    case KWAV_BAD_CHANNELS:
        if ( (uint16_t)AUDIO_STEREO == info->channels )
        {
            kode_err_set("%s: stereo; only mono plays", user);
        }
        else
        {
            kode_err_set("%s: %" PRIu32 " channels; only mono plays", user, (uint32_t)info->channels);
        }
        break;
    case KWAV_BAD_FORMAT:
        kode_err_set("%s: not plain PCM; only %" PRIu32 "-bit PCM plays", user, (uint32_t)KWAV_BITS);
        break;
    case KWAV_NO_FMT:
    case KWAV_NO_DATA:
    case KWAV_TRUNCATED:
        if ( riff_seen(p, got) && (KODE_SYS_FILE_READ_MAX == got) && (p->size > got) )
        {
            const uint32_t window_kb = KODE_SYS_FILE_READ_MAX / AUDIO_KB;

            kode_err_set("%s: its header is longer than %" PRIu32 " KB", user, window_kb);
        }
        else
        {
            kode_err_set("%s: not a WAV", user);
        }
        break;
    default:
        kode_err_set("%s: not a WAV", user);
        break;
    }
}

/**
 * @brief Read the file's first window, check its header and leave the first samples at its start.
 * @param p The sound; its volume path and size are set, its window is taken.
 * @return true when the window holds the first samples; false with the error set.
 */
static bool play_header(play_t *p)
{
    uint32_t    got = 0U;
    uint32_t    cap = p->size;
    bool        ok  = false;

    if ( KODE_SYS_FILE_READ_MAX < cap )
    {
        cap = KODE_SYS_FILE_READ_MAX;
    }
    const kode_status_t rc = kode_file_window(p->vol, p->size, 0U, p->buf, cap, &got);

    if ( KODE_OK != rc )
    {
        kode_err_set("%s: %s", p->user, kode_file_why(rc));
    }
    else
    {
        /* cppcheck-suppress misra-c2012-11.5 ; the window holds the file's bytes */
        uint8_t *const bytes = (uint8_t *)p->buf;
        kwav_info_t    info;
        kwav_reason_t  reason;

        (void)memset(&info, 0, sizeof(info));
        reason = kwav_parse(bytes, got, &info);
        /* Truncated with a data chunk found: the format is good, the samples run past the window. */
        if ( (KWAV_TRUNCATED == reason) && (0U != info.data_off) )
        {
            reason = KWAV_OK;
        }
        if ( KWAV_OK == reason )
        {
            const uint32_t declared = info.frames * AUDIO_FRAME_BYTES;
            uint32_t       total    = p->size - info.data_off;
            uint32_t       avail    = got - info.data_off;

            if ( declared < total )
            {
                total = declared;
            }
            if ( total < avail )
            {
                avail = total;
            }
            (void)memmove(bytes, &bytes[info.data_off], avail);
            p->held = avail;
            p->left = total - avail;
            p->next = info.data_off + avail;
            if ( got < cap )
            {
                /* A short read is the file's end, as in play_more(). */
                p->left = 0U;
            }
            ok = true;
        }
        else
        {
            wav_refused(p, got, reason, &info);
        }
    }
    return ok;
}

/**
 * @brief Check the file @p user and read what its header says, ready to play.
 * @param p    The sound to fill; its window is already taken.
 * @param user The path as typed.
 * @return true when the file is a WAV that plays; false with the error set.
 */
static bool play_load(play_t *p, const char *user)
{
    kode_sys_file_status_t st = { false, false, 0U };
    bool                   ok = false;

    p->stop = NULL;
    if ( kode_file_path(user, p->vol, sizeof(p->vol)) )
    {
        (void)snprintf(p->user, sizeof(p->user), "%s", user);
        if ( is_mp3(user) )
        {
            kode_err_set("%s", AUDIO_MP3_TEXT);
        }
        else
        {
            const kode_status_t rc = kode_file_stat(p->vol, &st);

            if ( KODE_OK != rc )
            {
                kode_err_set("%s: %s", user, kode_file_why(rc));
            }
            else if ( !st.exists )
            {
                kode_err_set("%s: no such file", user);
            }
            else if ( st.is_dir )
            {
                kode_err_set("%s: it is a folder", user);
            }
            else if ( KWAV_HEADER_BYTES > st.size )
            {
                kode_err_set("%s: not a WAV", user);
            }
            else
            {
                p->size = st.size;
                ok      = play_header(p);
            }
        }
    }
    return ok;
}

/** @brief Whether the sound is to end. */
static bool play_stopped(const play_t *p)
{
    return (NULL != p->stop) && atomic_load(p->stop);
}

/**
 * @brief Write @p total frames to the stream, a chunk at a time.
 * @param frames The samples.
 * @param total  How many.
 * @param stop   Ends the writing when set; NULL: never.
 * @return true when they were all written or the sound was stopped; false with the error set.
 */
static bool frames_write(const int16_t *frames, uint32_t total, const atomic_bool *stop)
{
    uint32_t done = 0U;
    bool     ok   = true;

    while ( ok && (done < total) && ((NULL == stop) || !atomic_load(stop)) )
    {
        uint32_t n     = total - done;
        size_t   taken = 0U;

        if ( AUDIO_CHUNK_FRAMES < n )
        {
            n = AUDIO_CHUNK_FRAMES;
        }
        const kode_status_t rc = kode_block_audio_stream_write(&frames[done], n, AUDIO_WRITE_MS, &taken);

        if ( KODE_OK == rc )
        {
            done += n;
        }
        else if ( (KODE_TIMEOUT == rc) && (0U < taken) )
        {
            done += (uint32_t)taken;
        }
        else
        {
            kode_err_set("%s", AUDIO_DEAF_TEXT);
            ok = false;
        }
    }
    return ok;
}

/**
 * @brief Write the window's samples to the stream, a chunk at a time.
 * @param p The sound.
 * @return true when they were all written or the sound was stopped; false with the error set.
 */
static bool play_write(play_t *p)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the window holds 16-bit samples */
    const int16_t *const frames = (const int16_t *)p->buf;
    /* an odd last byte is half a sample: dropped */
    const bool ok = frames_write(frames, p->held / AUDIO_FRAME_BYTES, p->stop);

    p->held = 0U;
    return ok;
}

/**
 * @brief Read the file's next window into the sound; a file that ends early ends the sound.
 * @param p The sound.
 * @return true when the window was read or there was none to read; false with the error set.
 */
static bool play_more(play_t *p)
{
    uint32_t want = p->left;
    uint32_t got  = 0U;
    bool     ok   = true;

    if ( KODE_SYS_FILE_READ_MAX < want )
    {
        want = KODE_SYS_FILE_READ_MAX;
    }
    if ( 0U < want )
    {
        const kode_status_t rc = kode_file_window(p->vol, p->size, p->next, p->buf, want, &got);

        if ( KODE_OK != rc )
        {
            kode_err_set("%s: %s", p->user, kode_file_why(rc));
            ok = false;
        }
        else if ( got < want )
        {
            p->left = 0U;
        }
        else
        {
            p->left -= got;
        }
        p->next += got;
        p->held  = got;
    }
    return ok;
}

/**
 * @brief Play the sound through the open stream, window by window, until it ends or is stopped.
 * @param p The sound, loaded.
 * @return true when it ended or was stopped; false with the error set.
 */
static bool play_pump(play_t *p)
{
    bool ok = true;

    while ( ok && (0U < p->held) && !play_stopped(p) )
    {
        ok = play_write(p) && (play_stopped(p) || play_more(p));
    }
    return ok;
}

/** @brief One try of opening the stream: true unless another task holds the speaker. */
static bool open_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the try_t stream_begin() passed */
    try_t *const t = (try_t *)ctx;

    t->rc = kode_block_audio_stream_open(KODE_SYS_AUDIO_RATE_HZ, 1U);
    return KODE_DENIED != t->rc;
}

/**
 * @brief Open the stream, waiting its turn.
 * @return true when it is open; false with the error set.
 */
static bool stream_begin(void)
{
    try_t t = { KODE_ERROR };

    (void)kode_retry(&open_attempt, &t, KODE_AUDIO_BUSY_MS);
    switch ( t.rc )
    {
    case KODE_OK:
        break;
    case KODE_DENIED:
        kode_err_set("%s", AUDIO_BUSY_TEXT);
        break;
    case KODE_TIMEOUT:
        kode_err_set("%s", AUDIO_DEAF_TEXT);
        break;
    case KODE_NO_RESOURCES:
        kode_err_set("%s", kode_file_why(KODE_NO_RESOURCES));
        break;
    default:
        kode_err_set("the speaker could not be opened");
        break;
    }
    return KODE_OK == t.rc;
}

/**
 * @brief A sound has its stream: clear the stop and say it sounds.
 * @param running The flag of the kind of sound.
 */
static void sound_arm(atomic_bool *running)
{
    atomic_store(&s_sound_stop, false);
    atomic_store(running, true);
}

/** @brief Whether no sound sounds. */
static bool sound_ended(void *ctx)
{
    (void)ctx;
    return !atomic_load(&s_bg_running) && !atomic_load(&s_fg_running);
}

/**
 * @brief Ask the sound to end, and wait for it to. Takes no lock.
 * @return true when nothing sounds; false if it did not end within KODE_AUDIO_BUSY_MS.
 */
static bool sound_halt(void)
{
    bool idle = sound_ended(NULL);

    if ( !idle )
    {
        atomic_store(&s_sound_stop, true);
        idle = kode_retry(&sound_ended, NULL, KODE_AUDIO_BUSY_MS);
    }
    return idle;
}

bool kode_audio_play(const char *path)
{
    bool played = false;

    if ( audio_enter("kode_audio_play") )
    {
        play_t p;

        (void)memset(&p, 0, sizeof(p));
        if ( atomic_load(&s_rec_running) )
        {
            kode_err_set("%s", AUDIO_RECORDING_BUSY_TEXT);
        }
        else
        {
            p.buf = window_get(&s_fg_window);
            if ( (NULL != p.buf) && play_load(&p, path) && stream_begin() )
            {
                sound_arm(&s_fg_running);
                p.stop = &s_sound_stop;
                played = play_pump(&p);
                (void)kode_block_audio_stream_close();
                atomic_store(&s_fg_running, false);
            }
        }
        audio_leave();
    }
    return played;
}

/** @brief The background player's task: play the sound, close the stream, say it is over. */
static void bg_task(void *arg)
{
    (void)arg;
    if ( !play_pump(&s_bg) )
    {
        KLOG_W(TAG, "%s: the sound ended early: %s", s_bg.user, kode_error());
    }
    (void)kode_block_audio_stream_close();
    atomic_store(&s_bg_running, false);
}

bool kode_audio_play_start(const char *path)
{
    bool started = false;

    if ( audio_enter("kode_audio_play_start") )
    {
        if ( atomic_load(&s_rec_running) )
        {
            kode_err_set("%s", AUDIO_RECORDING_BUSY_TEXT);
        }
        else if ( !sound_halt() )
        {
            kode_err_set("%s", AUDIO_BUSY_TEXT);
        }
        else
        {
            static void *window;  /* taken on the first background play, kept */

            (void)memset(&s_bg, 0, sizeof(s_bg));
            s_bg.buf = window_get(&window);
            if ( (NULL != s_bg.buf) && play_load(&s_bg, path) && stream_begin() )
            {
                sound_arm(&s_bg_running);
                s_bg.stop = &s_sound_stop;
                if ( khal_task_start(&bg_task, AUDIO_TASK_NAME, AUDIO_TASK_STACK_BYTES, AUDIO_TASK_PRIO,
                                    NULL) == KODE_OK )
                {
                    started = true;
                }
                else
                {
                    atomic_store(&s_bg_running, false);
                    (void)kode_block_audio_stream_close();
                    kode_err_set("no memory for the sound's task");
                }
            }
        }
        audio_leave();
    }
    return started;
}

bool kode_audio_is_playing(void)
{
    bool playing = false;

    if ( kode_enter("kode_audio_is_playing") )
    {
        playing = atomic_load(&s_bg_running);
    }
    return playing;
}

bool kode_audio_play_stop(void)
{
    bool idle = false;

    if ( kode_enter("kode_audio_play_stop") )
    {
        idle = sound_halt();
        if ( !idle )
        {
            kode_err_set("%s", AUDIO_BUSY_TEXT);
        }
    }
    return idle;
}

/**
 * @brief Write @p ms of the tone @p bin to the open stream, a chunk at a time.
 * @param p   The sound; its window is taken.
 * @param bin The tone's index for ktone_fill().
 * @param ms  The length.
 * @return true when it was all written or the sound was stopped; false with the error set.
 */
static bool tone_pump(play_t *p, uint8_t bin, uint32_t ms)
{
    const uint32_t total = ms * AUDIO_FRAMES_PER_MS;
    uint32_t       done  = 0U;
    bool           ok    = true;

    while ( ok && (done < total) && !play_stopped(p) )
    {
        uint32_t n = total - done;

        if ( AUDIO_CHUNK_FRAMES < n )
        {
            n = AUDIO_CHUNK_FRAMES;
        }
        /* cppcheck-suppress misra-c2012-11.5 ; the window holds 16-bit samples */
        if ( ktone_fill(bin, done, AUDIO_TONE_AMP_Q15, (int16_t *)p->buf, n) != KODE_OK )
        {
            kode_err_set("the tone could not be made");
            ok = false;
        }
        else
        {
            p->held = n * AUDIO_FRAME_BYTES;
            ok      = play_write(p);
            done += n;
        }
    }
    return ok;
}

/**
 * @brief Play the tone through the stream and wait until it has drained.
 * @param hz The frequency, valid.
 * @param ms The length, valid.
 * @return true when it was heard; false with the error set.
 */
static bool tone_play(uint16_t hz, uint32_t ms)
{
    play_t p;
    bool   heard = false;

    (void)memset(&p, 0, sizeof(p));
    p.buf = window_get(&s_fg_window);
    if ( (NULL != p.buf) && stream_begin() )
    {
        sound_arm(&s_fg_running);
        p.stop = &s_sound_stop;
        heard  = tone_pump(&p, ktone_bin_nearest(hz), ms);
        (void)kode_block_audio_stream_close();
        atomic_store(&s_fg_running, false);
    }
    return heard;
}

bool kode_audio_tone(uint16_t hz, uint32_t ms)
{
    bool heard = false;

    if ( audio_enter("kode_audio_tone") )
    {
        if ( (KODE_AUDIO_TONE_HZ_MIN > hz) || (KODE_AUDIO_TONE_HZ_MAX < hz) )
        {
            kode_err_set("tone frequency is %" PRIu32 " to %" PRIu32 " Hz", (uint32_t)KODE_AUDIO_TONE_HZ_MIN,
                         (uint32_t)KODE_AUDIO_TONE_HZ_MAX);
        }
        else if ( (0U == ms) || (KODE_AUDIO_TONE_MAX_MS < ms) )
        {
            kode_err_set("tone length is 1 to %" PRIu32 " ms", (uint32_t)KODE_AUDIO_TONE_MAX_MS);
        }
        else if ( atomic_load(&s_bg_running) )
        {
            kode_err_set("%s", AUDIO_PLAYING_TEXT);
        }
        else if ( atomic_load(&s_rec_running) )
        {
            kode_err_set("%s", AUDIO_RECORDING_BUSY_TEXT);
        }
        else
        {
            heard = tone_play(hz, ms);
        }
        audio_leave();
    }
    return heard;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_audio_play_samples(const int16_t *samples, uint32_t count)
{
    bool heard = false;

    if ( audio_enter("kode_audio_play_samples") )
    {
        if ( (NULL == samples) || (0U == count) )
        {
            kode_err_set("no samples to play");
        }
        else if ( atomic_load(&s_bg_running) )
        {
            kode_err_set("%s", AUDIO_PLAYING_TEXT);
        }
        else if ( atomic_load(&s_rec_running) )
        {
            kode_err_set("%s", AUDIO_RECORDING_BUSY_TEXT);
        }
        else if ( stream_begin() )
        {
            sound_arm(&s_fg_running);
            heard = frames_write(samples, count, &s_sound_stop);
            (void)kode_block_audio_stream_close();
            atomic_store(&s_fg_running, false);
        }
        else
        {
            /* stream_begin() said why */
        }
        audio_leave();
    }
    return heard;
}

/**
 * @brief The index of the setting called @p setting, or AUDIO_SET_COUNT with the error set.
 * @param setting What the caller typed.
 * @return An AUDIO_SET_* value, or AUDIO_SET_COUNT.
 */
static uint32_t setting_find(const char *setting)
{
    static const char *const names[AUDIO_SET_COUNT] = {
        [AUDIO_SET_VOLUME] = "volume", [AUDIO_SET_MIC_GAIN] = "mic_gain", [AUDIO_SET_MUTE] = "mute"
    };
    uint32_t found = (uint32_t)AUDIO_SET_COUNT;

    if ( NULL == setting )
    {
        kode_err_set("setting name is NULL");
    }
    else
    {
        for ( uint32_t i = 0U; ((uint32_t)AUDIO_SET_COUNT > i) && ((uint32_t)AUDIO_SET_COUNT == found); i++ )
        {
            if ( kode_name_is(setting, names[i]) )
            {
                found = i;
            }
        }
        if ( (uint32_t)AUDIO_SET_COUNT == found )
        {
            kode_err_set("unknown setting '%s'", setting);
        }
    }
    return found;
}

/**
 * @brief Send setting @p idx to the speaker.
 * @param idx   An AUDIO_SET_* value.
 * @param value The new value; the error is set if it is out of range.
 * @return The sys answer; KODE_DENIED for a value out of range.
 */
static kode_status_t setting_send(uint32_t idx, uint32_t value)
{
    kode_status_t rc = KODE_DENIED;

    switch ( idx )
    {
    case (uint32_t)AUDIO_SET_VOLUME:
        if ( KDRV_ES8311_VOL_MAX < value )
        {
            kode_err_set("volume is 0 to %" PRIu32, (uint32_t)KDRV_ES8311_VOL_MAX);
        }
        else
        {
            rc = kode_sys_audio_volume((uint8_t)value);
        }
        break;
    case (uint32_t)AUDIO_SET_MIC_GAIN:
        if ( KDRV_ES8311_MIC_DB_MAX < value )
        {
            kode_err_set("mic_gain is 0 to %" PRIu32 " dB", (uint32_t)KDRV_ES8311_MIC_DB_MAX);
        }
        else
        {
            rc = kode_sys_audio_mic_gain((uint8_t)value);
        }
        break;
    case (uint32_t)AUDIO_SET_MUTE:
        if ( AUDIO_SWITCH_MAX < value )
        {
            kode_err_set("mute is 0 or %" PRIu32, (uint32_t)AUDIO_SWITCH_MAX);
        }
        else
        {
            rc = kode_sys_audio_mute(0U != value);
        }
        break;
    default:
        /* Not a setting. */
        break;
    }
    return rc;
}

bool kode_audio_set(const char *setting, uint32_t value)
{
    bool sent = false;

    if ( kode_enter("kode_audio_set") )
    {
        const uint32_t idx = setting_find(setting);

        if ( (uint32_t)AUDIO_SET_COUNT > idx )
        {
            kode_status_t const rc = setting_send(idx, value);

            if ( KODE_OK == rc )
            {
                sent = true;
            }
            else if ( KODE_NO_RESOURCES == rc )
            {
                kode_err_set("audio did not take the request");
            }
            else
            {
                /* setting_send() said why. */
            }
        }
    }
    return sent;
}

/** @brief What one try of the info carries. */
typedef struct {
    kdrv_es8311_info_t info;  /**< What the codec said.      */
    kode_status_t      rc;    /**< The last answer.          */
} info_try_t;

/** @brief One try of the info: true unless another task holds the speaker. */
static bool info_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the info_try_t kode_audio_get() passed */
    info_try_t *const t = (info_try_t *)ctx;

    t->rc = kode_block_audio_info(&t->info);
    return KODE_DENIED != t->rc;
}

/**
 * @brief The value of setting @p idx in @p info.
 * @param info What the codec said.
 * @param idx  An AUDIO_SET_* value.
 * @return The value; 0 past the last.
 */
static uint32_t setting_pick(const kdrv_es8311_info_t *info, uint32_t idx)
{
    uint32_t value = 0U;

    switch ( idx )
    {
    case (uint32_t)AUDIO_SET_VOLUME:
        value = info->volume;
        break;
    case (uint32_t)AUDIO_SET_MIC_GAIN:
        value = info->mic_db;
        break;
    case (uint32_t)AUDIO_SET_MUTE:
        if ( info->mute )
        {
            value = 1U;
        }
        break;
    default:
        /* Not a setting. */
        break;
    }
    return value;
}

uint32_t kode_audio_get(const char *setting)
{
    uint32_t value = 0U;

    if ( kode_enter("kode_audio_get") )
    {
        const uint32_t idx = setting_find(setting);

        if ( (uint32_t)AUDIO_SET_COUNT > idx )
        {
            info_try_t t;

            (void)memset(&t, 0, sizeof(t));
            t.rc = KODE_ERROR;
            (void)kode_retry(&info_attempt, &t, KODE_AUDIO_BUSY_MS);
            if ( KODE_OK == t.rc )
            {
                value = setting_pick(&t.info, idx);
            }
            else if ( KODE_DENIED == t.rc )
            {
                kode_err_set("%s", AUDIO_BUSY_TEXT);
            }
            else
            {
                kode_err_set("%s", AUDIO_DEAF_TEXT);
            }
        }
    }
    return value;
}

/**
 * @brief Whether @p ms is a length a recording can have; the error is set if not.
 * @param ms The length asked for.
 * @return true when it is 1 to KODE_AUDIO_RECORD_MAX_MS.
 */
static bool rec_length_ok(uint32_t ms)
{
    const bool ok = (0U != ms) && (KODE_AUDIO_RECORD_MAX_MS >= ms);

    if ( !ok )
    {
        kode_err_set("record length is 1 to %" PRIu32 " ms", (uint32_t)KODE_AUDIO_RECORD_MAX_MS);
    }
    return ok;
}

/**
 * @brief What a failed recording of samples is called; the codec's answer.
 * @param rc The sys answer.
 * @return The phrase; never NULL.
 */
static const char *mic_why(kode_status_t rc)
{
    const char *why = NULL;

    switch ( rc )
    {
    case KODE_DENIED:
        why = AUDIO_BUSY_TEXT;
        break;
    case KODE_TIMEOUT:
        why = AUDIO_MIC_DEAF_TEXT;
        break;
    case KODE_NO_RESOURCES:
        why = kode_file_why(rc);
        break;
    default:
        why = AUDIO_MIC_FAIL_TEXT;
        break;
    }
    return why;
}

/**
 * @brief What a failed recording to a file is called.
 *
 * The sys call answers KODE_DENIED for a held codec and for a medium that
 * refuses the file alike; both read as busy.
 *
 * @param rc The sys answer.
 * @return The phrase; never NULL.
 */
static const char *rec_file_why(kode_status_t rc)
{
    const char *why = NULL;

    switch ( rc )
    {
    case KODE_DENIED:
        why = AUDIO_BUSY_TEXT;
        break;
    case KODE_TIMEOUT:
        why = AUDIO_MIC_DEAF_TEXT;
        break;
    default:
        why = kode_file_why(rc);
        break;
    }
    return why;
}

/** @brief What one try of recording to a file carries. */
typedef struct {
    const char   *vol;    /**< The volume path.        */
    uint32_t      ms;     /**< The length asked for.   */
    uint32_t      bytes;  /**< The file's length.      */
    kode_status_t rc;     /**< The last answer.        */
} rec_file_try_t;

/** @brief One try of recording to a file: true unless another task holds the codec. */
static bool rec_file_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the rec_file_try_t kode_audio_record_file() passed */
    rec_file_try_t *const t = (rec_file_try_t *)ctx;

    t->rc = kode_block_audio_record_file(t->vol, t->ms, &t->bytes);
    return KODE_DENIED != t->rc;
}

bool kode_audio_record_file(const char *path, uint32_t ms)
{
    bool saved = false;

    if ( audio_enter("kode_audio_record_file") )
    {
        char vol[KODE_PATH_MAX];

        if ( !rec_length_ok(ms) )
        {
            /* The error is set. */
        }
        else if ( atomic_load(&s_rec_running) )
        {
            kode_err_set("%s", AUDIO_RECORDING_TEXT);
        }
        else if ( atomic_load(&s_bg_running) )
        {
            kode_err_set("%s", AUDIO_PLAYING_TEXT);
        }
        else if ( kode_file_path(path, vol, sizeof(vol)) )
        {
            rec_file_try_t t = { vol, ms, 0U, KODE_ERROR };

            (void)kode_retry(&rec_file_attempt, &t, KODE_AUDIO_BUSY_MS);
            if ( KODE_OK == t.rc )
            {
                saved = true;
            }
            else if ( (KODE_DENIED == t.rc) || (KODE_TIMEOUT == t.rc) )
            {
                kode_err_set("%s", rec_file_why(t.rc));
            }
            else
            {
                kode_err_set("%s: %s", path, rec_file_why(t.rc));
            }
        }
        else
        {
            /* The error is set. */
        }
        audio_leave();
    }
    return saved;
}

/**
 * @brief Turn the frames in the image into little-endian bytes, in place.
 * @param image The recording's image.
 * @param frames How many frames follow its header.
 */
static void rec_pack(uint8_t *image, uint32_t frames)
{
    /* cppcheck-suppress misra-c2012-11.3 ; the image holds 16-bit samples after its header */
    const int16_t *const samples = (const int16_t *)&image[KWAV_HEADER_BYTES];

    for ( uint32_t i = 0U; i < frames; i++ )
    {
        const uint16_t value = (uint16_t)samples[i];
        uint8_t *const out   = &image[KWAV_HEADER_BYTES + (i * AUDIO_FRAME_BYTES)];

        out[0] = (uint8_t)(value & AUDIO_BYTE_MASK);
        out[1] = (uint8_t)(value >> AUDIO_BYTE_BITS);
    }
}

/** @brief What one try of the recorder carries. */
typedef struct {
    int16_t      *samples;  /**< Where the frames go.        */
    size_t        got;      /**< Frames the try brought.     */
    kode_status_t rc;       /**< The last answer.            */
} rec_try_t;

/** @brief One try of recording: done unless another task holds the codec; a stop ends the wait. */
static bool rec_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the rec_try_t rec_task() passed */
    rec_try_t *const t = (rec_try_t *)ctx;

    if ( atomic_load(&s_rec_stop) )
    {
        t->rc = KODE_DENIED;
        return true;
    }
    t->rc = kode_block_audio_record_samples(t->samples, s_rec.cap, &t->got, atomic_load(&s_rec_max_ms));
    return (KODE_DENIED != t->rc) || atomic_load(&s_rec_stop);
}

/** @brief The recorder's task: record into the image, make it a WAV, say it is over. */
static void rec_task(void *arg)
{
    /* cppcheck-suppress misra-c2012-11.3 ; the image holds 16-bit samples after its header */
    int16_t *const samples = (int16_t *)&s_rec.image[KWAV_HEADER_BYTES];
    rec_try_t      t       = { samples, 0U, KODE_OK };
    size_t         got     = 0U;

    (void)arg;
    if ( !atomic_load(&s_rec_stop) )
    {
        (void)kode_retry(&rec_attempt, &t, KODE_AUDIO_BUSY_MS);
        got = t.got;
        if ( KODE_OK != t.rc )
        {
            got = 0U;
            /* A stop ends the wait for a held codec; that is no failure. */
            if ( (KODE_DENIED != t.rc) || !atomic_load(&s_rec_stop) )
            {
                KLOG_W(TAG, "the recording ended early: %s", mic_why(t.rc));
            }
        }
    }
    if ( got > s_rec.cap )
    {
        got = s_rec.cap;
    }
    if ( 0U < got )
    {
        (void)kwav_build((uint32_t)got, s_rec.image, KWAV_HEADER_BYTES);
        rec_pack(s_rec.image, (uint32_t)got);
    }
    atomic_store(&s_rec_got, (uint32_t)got);
    atomic_store(&s_rec_running, false);
}

/** @brief Whether the recorder's task has ended. */
static bool rec_ended(void *ctx)
{
    (void)ctx;
    return !atomic_load(&s_rec_running);
}

/**
 * @brief Ask the recording to end, and wait for its task to.
 *
 * Asks up to AUDIO_STOP_TRIES times, AUDIO_STOP_WAIT_MS apart: a stop that
 * lands before the sys call has posted its request is lost to AO_Audio, and
 * the door lets a new one through once the lost one is taken.
 *
 * @return true when no recorder runs; false if it did not end within KODE_AUDIO_BUSY_MS.
 */
static bool rec_halt(void)
{
    bool idle = rec_ended(NULL);

    if ( !idle )
    {
        atomic_store(&s_rec_stop, true);
        for ( uint32_t i = 0U; !idle && (AUDIO_STOP_TRIES > i); i++ )
        {
            kode_sys_audio_record_stop();
            idle = kode_retry(&rec_ended, NULL, AUDIO_STOP_WAIT_MS);
        }
    }
    return idle;
}

/**
 * @brief Take a new image for @p max_ms and start the recorder's task. The previous recording is dropped.
 * @param max_ms The length, valid.
 * @return true when the recorder runs; false with the error set.
 */
static bool rec_launch(uint32_t max_ms)
{
    const uint32_t cap   = max_ms * AUDIO_FRAMES_PER_MS;
    const size_t   bytes = (size_t)KWAV_HEADER_BYTES + ((size_t)cap * AUDIO_FRAME_BYTES);
    bool           ok    = false;

    khal_free(s_rec.image);
    s_rec.image = NULL;
    s_rec.cap   = 0U;
    atomic_store(&s_rec_got, 0U);
    /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block holds bytes */
    s_rec.image = (uint8_t *)khal_alloc_psram(sizeof(void *), bytes);
    if ( NULL == s_rec.image )
    {
        kode_err_set("%s", kode_file_why(KODE_NO_RESOURCES));
    }
    else
    {
        s_rec.cap = cap;
        atomic_store(&s_rec_max_ms, max_ms);
        atomic_store(&s_rec_t0, kode_sys_time_ms());
        atomic_store(&s_rec_stop, false);
        atomic_store(&s_rec_running, true);
        if ( khal_task_start(&rec_task, AUDIO_REC_TASK_NAME, AUDIO_TASK_STACK_BYTES, AUDIO_TASK_PRIO,
                            NULL) == KODE_OK )
        {
            ok = true;
        }
        else
        {
            atomic_store(&s_rec_running, false);
            khal_free(s_rec.image);
            s_rec.image = NULL;
            s_rec.cap   = 0U;
            kode_err_set("no memory for the recording's task");
        }
    }
    return ok;
}

bool kode_audio_record_start(uint32_t max_ms)
{
    bool started = false;

    if ( audio_enter("kode_audio_record_start") )
    {
        if ( !rec_length_ok(max_ms) )
        {
            /* The error is set. */
        }
        else if ( atomic_load(&s_bg_running) )
        {
            kode_err_set("%s", AUDIO_PLAYING_TEXT);
        }
        else if ( !rec_halt() )
        {
            kode_err_set("%s", AUDIO_BUSY_TEXT);
        }
        else
        {
            started = rec_launch(max_ms);
        }
        audio_leave();
    }
    return started;
}

kode_audio_record_status_t kode_audio_record_status(void)
{
    kode_audio_record_status_t st = { false, 0U, 0U };

    if ( kode_enter("kode_audio_record_status") )
    {
        if ( atomic_load(&s_rec_running) )
        {
            const uint32_t spent = kode_sys_time_ms() - atomic_load(&s_rec_t0);
            const uint32_t max   = atomic_load(&s_rec_max_ms);

            st.recording = true;
            st.ms        = spent;
            if ( st.ms > max )
            {
                st.ms = max;
            }
        }
        else
        {
            st.frames = atomic_load(&s_rec_got);
            st.ms     = st.frames / AUDIO_FRAMES_PER_MS;
        }
    }
    return st;
}

bool kode_audio_record_stop(void)
{
    bool idle = false;

    if ( audio_enter("kode_audio_record_stop") )
    {
        if ( NULL == s_rec.image )
        {
            kode_err_set("%s", AUDIO_NOTHING_TEXT);
        }
        else
        {
            idle = rec_halt();
            if ( !idle )
            {
                kode_err_set("%s", AUDIO_BUSY_TEXT);
            }
        }
        audio_leave();
    }
    return idle;
}

bool kode_audio_record_save(const char *path)
{
    bool saved = false;

    if ( audio_enter("kode_audio_record_save") )
    {
        const uint32_t got = atomic_load(&s_rec_got);

        if ( atomic_load(&s_rec_running) )
        {
            kode_err_set("%s", AUDIO_RECORDING_TEXT);
        }
        else if ( (NULL == s_rec.image) || (0U == got) )
        {
            kode_err_set("%s", AUDIO_NOTHING_TEXT);
        }
        else
        {
            saved = kode_file_put_bytes(path, s_rec.image, KWAV_HEADER_BYTES + (got * AUDIO_FRAME_BYTES), false);
        }
        audio_leave();
    }
    return saved;
}
