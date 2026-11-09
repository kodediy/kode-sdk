/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file audio_ao.c
 * @brief The speaker and the microphone, asked for through AO_Audio.
 */
#include "audio_ao.h"

#define TAG "audio"

/** How long each note is held. */
#define AU_TONE_MS       250U

/** The level of a note, in percent of full scale. A quarter scale is what the
 *  console's `audio_tone` and the production row both use: audible across a
 *  room and nowhere near the speaker's thermal limit. */
#define AU_TONE_AMP_PCT  25U

/** How long the microphone listens. Two seconds of 48 kHz, 16-bit, mono is
 *  about 190 kB, which the 2 MB internal FAT takes with room to spare. */
#define AU_REC_MS        2000U

/** Where the recording goes. Absolute and virtual, and already resolved: the
 *  request takes a finished path because AO_Audio owns the codec, not the
 *  filesystem. The parent directory must exist, and the root of /storage
 *  always does. Every recording replaces the last. */
#define AU_REC_PATH      "/storage/example_rec.wav"

/** The level of the LED while the microphone listens. Dim on purpose. */
#define AU_LED_LEVEL     32U

/** The eight frequencies the codec's own tone detector uses, one octave
 *  wide, listed so that what a press asks for is what it hears: AO_Audio
 *  snaps any request to the nearest of them, and a value already on the list
 *  is left alone. The list is the driver's, not part of the API, so if it
 *  ever moved the snap would still play the nearest note. Each sits inside
 *  KODE_SYS_AUDIO_HZ_MIN..KODE_SYS_AUDIO_HZ_MAX. */
static const uint16_t AU_NOTES_HZ[] = {
    1020U, 1137U, 1254U, 1395U, 1535U, 1676U, 1816U, 1957U
};

/** @brief The AO's own state, private to this file. */
typedef struct {
    QActive super;
    uint8_t note;   /**< Which entry of AU_NOTES_HZ the next press plays. */
} AudioDemo;

/* The queue and the stack, at the SDK's own defaults; buttons_led_ao.c says
 * what each number decides. The queue's depth follows kode_sys.h's one queue
 * rule ("Queue depth"): here one recording out at most and a button's press
 * and release. */
static QEvtPtr     l_queue[KQP_AO_QUEUE_LEN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static AudioDemo l_ao;
QActive * const AO_AudioDemo = &l_ao.super;

/* No step here waits: kode.h's kode_audio_tone() and kode_audio_record_file()
 * do, so they belong to a task. */
static QState AudioDemo_initial(AudioDemo * const me, void const * const par);
static QState AudioDemo_idle(AudioDemo * const me, QEvt const * const e);
static QState AudioDemo_recording(AudioDemo * const me, QEvt const * const e);

/**
 * @brief Ask for the next note and move on to the one after it.
 *
 * kode_sys_audio_tone() posts and returns, so this is safe in a state
 * handler. Its return is all there is: what came out of the speaker, and
 * whether the codec was too busy to make it, is AO_Audio's to say in its own
 * log.
 *
 * @param me The AO.
 */
static void play_next_note(AudioDemo * const me)
{
    uint16_t const      hz = AU_NOTES_HZ[me->note];
    kode_status_t const rc = kode_sys_audio_tone(hz, (uint16_t)AU_TONE_MS,
                                                 (uint8_t)AU_TONE_AMP_PCT);

    if ( KODE_OK == rc )
    {
        KLOG_I(TAG, "note %u: %u Hz for %u ms", (unsigned)(me->note + 1U),
               (unsigned)hz, (unsigned)AU_TONE_MS);
    }
    else
    {
        KLOG_W(TAG, "note %u not taken (status %d)", (unsigned)(me->note + 1U),
               (int)rc);
    }

    me->note = (uint8_t)(me->note + 1U);
    if ( (uint8_t)Q_DIM(AU_NOTES_HZ) <= me->note )
    {
        me->note = 0U;
    }
}

/**
 * @brief Say how a recording ended.
 *
 * KODE_DENIED is a codec that was already busy, or a board with no codec;
 * KODE_NO_RESOURCES is no PSRAM for the recording; anything else is what
 * AO_Storage said when the medium would not take the file.
 *
 * @param r The answer, a copy QP recycles after this step: read it here,
 *          keep none of it.
 */
static void report_recording(kode_sys_audio_file_answer_t const * const r)
{
    if ( (uint8_t)KODE_OK == r->rc )
    {
        KLOG_I(TAG, "recorded %u ms into %s, %u bytes",
               (unsigned)r->ms, r->path, (unsigned)r->bytes);
    }
    else
    {
        KLOG_W(TAG, "no recording in %s: status %u", r->path,
               (unsigned)r->rc);
    }
}

void AudioDemo_ctor(void)
{
    AudioDemo * const me = &l_ao;
    QActive_ctor(&me->super, Q_STATE_CAST(&AudioDemo_initial));
    me->note = 0U;
}

void AudioDemo_start(void)
{
    QActive_setAttr(AO_AudioDemo, TASK_NAME_ATTR, "audio");
    QACTIVE_START(AO_AudioDemo, KODE_PRIO_USER_LOW,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
}

/**
 * @brief The initial transition: subscribe to the buttons, then run.
 *
 * The recording's answer needs no subscription: it is posted to this AO.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to AudioDemo_idle.
 */
static QState AudioDemo_initial(AudioDemo * const me, void const * const par)
{
    (void)par;

    QActive_subscribe(&me->super, BUTTON_EVT_SIG);

    KLOG_I(TAG, "up; the top button plays a note, the bottom one records "
                "%u ms", (unsigned)AU_REC_MS);
    return Q_TRAN(&AudioDemo_idle);
}

/**
 * @brief Nothing in flight: the top button plays, the bottom one records.
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_TRAN() to start listening,
 *         Q_SUPER() otherwise.
 */
static QState AudioDemo_idle(AudioDemo * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case BUTTON_EVT_SIG: {
            ButtonEvt const * const be = (ButtonEvt const *)e;

            /* id is a button's number: KODE_SYS_BUTTON_x in kode_sys.h. */
            if ( be->pressed && (KODE_SYS_BUTTON_TOP == (uint32_t)be->id) )
            {
                play_next_note(me);
                status = Q_HANDLED();
            }
            else if ( be->pressed && (KODE_SYS_BUTTON_BOT == (uint32_t)be->id) )
            {
                /* Asked first, then the state changes, and the order cannot
                 * lose the answer: even one that AO_Audio posts from its
                 * higher priority in the middle of this step waits in this
                 * AO's queue until the step is over, by which time the
                 * recording state is the one that will read it. A request
                 * refused at once posted nothing, so there is nothing to
                 * wait for and the state stays. */
                kode_status_t const rc =
                    kode_sys_audio_record_file(&me->super, AU_REC_PATH,
                                               AU_REC_MS);

                if ( KODE_OK == rc )
                {
                    status = Q_TRAN(&AudioDemo_recording);
                }
                else
                {
                    KLOG_W(TAG, "recording not taken (status %d)", (int)rc);
                    status = Q_HANDLED();
                }
            }
            else
            {
                /* A release, or another button: nothing to do. */
                status = Q_HANDLED();
            }
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }

    return status;
}

/**
 * @brief A recording is in flight: wait for its answer and ask for nothing.
 *
 * The codec takes one thing at a time, so a note asked for now would only be
 * refused. Saying so here, in the state, costs less than a refused request.
 *
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_TRAN() back to idle once the
 *         answer arrives, Q_SUPER() otherwise.
 */
static QState AudioDemo_recording(AudioDemo * const me, QEvt const * const e)
{
    QState status;

    (void)me;
    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            /* The microphone makes no sound of its own, so the LED is what
             * tells the person when to speak. AO_LedMgr owns the chip; a
             * refused colour costs the cue, not the recording. */
            (void)kode_sys_led_set_rgb((uint8_t)AU_LED_LEVEL, 0U, 0U);
            KLOG_I(TAG, "listening");
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            (void)kode_sys_led_set_rgb(0U, 0U, 0U);
            status = Q_HANDLED();
            break;
        }
        /* AO_Audio answers every request it takes, refusals included, so this
         * state needs no timer of its own. */
        case AUDIO_FILE_RSP_SIG: {
            report_recording((kode_sys_audio_file_answer_t const *)e);
            status = Q_TRAN(&AudioDemo_idle);
            break;
        }
        case BUTTON_EVT_SIG: {
            ButtonEvt const * const be = (ButtonEvt const *)e;

            if ( be->pressed )
            {
                KLOG_I(TAG,
                       "still recording; the codec takes one thing at a time");
            }
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }

    return status;
}
