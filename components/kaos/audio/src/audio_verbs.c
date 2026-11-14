/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file audio_verbs.c
 * @brief AO_Audio's console verbs: `audio`, `audio_help`, `audio_tone` and `audio_rec`, registered by kaos_audio_start().
 */
#include <inttypes.h>

#include "kaos.h"
#include "kdrv_es8311.h"
#include "kes_fields.h"
#include "kqp.h"
#include "ktone.h"

#include "console_fs.h"
#include "console_verbs.h"
#include "kverbs.h"

/** @brief `audio_rec` holds the console: AUDIO_FILE_RSP_SIG is its answer. */
static bool s_audio_rec;

/** @brief Resolve a codec field name. */
static kode_status_t audio_field_by_name(const char *name, uint16_t *field)
{
    kdrv_es8311_field_t f = KDRV_ES_FIELD_MAX;
    kode_status_t const rc = kdrv_es8311_field_by_name(name, &f);

    *field = (uint16_t)f;
    return rc;
}

/** @brief Describe a codec field. */
static bool audio_field_row(uint16_t field, console_field_t *out)
{
    /* The codec's table: driver state, the settings an open applies, the chip
     * decoded, then the raw registers. dac_vol_db and adc_vol_db are 0 at full
     * scale and clip above it; adc_scale_db and pga are DIFFERENT microphone
     * gains in different registers. */
    static const char *const AUDIO_FIELD_DESC[] = {
        [KDRV_ES_PRESENT]      = "The codec answered at 0x18 and identified itself as an ES8311.",
        [KDRV_ES_OPEN]         = "The device is open: analog up, DAC live, both I2S channels running.",
        [KDRV_ES_BUSY]         = "A job is with the audio worker right now.",
        [KDRV_ES_RATE]         = "Sample rate. Fixed at 48000: at 16 kHz the reconstruction image is audible.",
        [KDRV_ES_BITS]         = "Sample width. Fixed at 16.",
        [KDRV_ES_MCLK_MULT]    = "MCLK over the sample rate. Fixed at 256; the codec layer re-applies it at every open.",
        [KDRV_ES_VOL]          = "Output volume, 0 to 100. 100 is exactly 0 dB on this board; the rest is attenuation.",
        [KDRV_ES_MIC_DB]       = "Microphone gain applied at the next open, 0 to 42 dB in 6 dB steps.",
        [KDRV_ES_OUT_MUTE]     = "Mute the DAC.",
        [KDRV_ES_IN_MUTE]      = "Mute the ADC.",
        [KDRV_ES_DAC_VOL_DB]   = "Register 0x32 as tenth-decibels, reg/2 - 95.5. Zero is full scale; above zero the DAC clips.",
        [KDRV_ES_ADC_VOL_DB]   = "Register 0x17, the same law on the capture side.",
        [KDRV_ES_ADC_SCALE_DB] = "Register 0x16 as 0 to 42 dB in 6 dB steps. This is what the gain call writes; it is NOT the analog front end.",
        [KDRV_ES_PGA]          = "Register 0x14 bits 3:0, the true analog PGA. Raw.",
        [KDRV_ES_DMIC]         = "Digital microphone select. Must read 0: this board's microphone is analog.",
        [KDRV_ES_HPSW]         = "Headphone drive. The codec layer forces 1 at open; the output goes into an RC filter and a class-D.",
        [KDRV_ES_DAC2ADC]      = "The internal DAC-to-ADC loopback. Must read 8. At 0x58 the chip measures itself and no capture means anything.",
        [KDRV_ES_DRC_EN]       = "DAC dynamic range control. Off and read only: it levels toward -6 dBFS whatever the volume is.",
        [KDRV_ES_RAMPRATE]     = "DAC volume ramp rate. It sets how long a tone has to settle before it can be measured.",
        [KDRV_ES_REG00] = "Reset and the clock state machine.",
        [KDRV_ES_REG01] = "Clock manager: which clocks are running.",
        [KDRV_ES_REG02] = "Clock manager: pre-divider and pre-multiplier.",
        [KDRV_ES_REG03] = "Clock manager: fs mode and the ADC oversample ratio.",
        [KDRV_ES_REG04] = "Clock manager: the DAC oversample ratio.",
        [KDRV_ES_REG05] = "Clock manager: the ADC and DAC dividers.",
        [KDRV_ES_REG06] = "Clock manager: the BCLK divider.",
        [KDRV_ES_REG07] = "LRCK divider, high byte.",
        [KDRV_ES_REG08] = "LRCK divider, low byte.",
        [KDRV_ES_REG09] = "Serial port in: format, word length and mute.",
        [KDRV_ES_REG0A] = "Serial port out: format and word length.",
        [KDRV_ES_REG0D] = "System: analog power up and down.",
        [KDRV_ES_REG0E] = "System: analog power up and down.",
        [KDRV_ES_REG12] = "System: enable the DAC.",
        [KDRV_ES_REG13] = "System: the headphone switch is bit 4.",
        [KDRV_ES_REG14] = "System: the DMIC select is bit 6, the analog PGA is bits 3:0.",
        [KDRV_ES_REG16] = "ADC scale, 0 to 7.",
        [KDRV_ES_REG17] = "ADC volume.",
        [KDRV_ES_REG31] = "DAC mute.",
        [KDRV_ES_REG32] = "DAC volume. Read only: volume 100 leaves it at 0xBF, full scale.",
        [KDRV_ES_REG34] = "DAC dynamic range control. Read only, like drc_en.",
        [KDRV_ES_REG37] = "DAC volume ramp rate.",
        [KDRV_ES_REG44] = "The DAC-to-ADC loopback, raw.",
        [KDRV_ES_REG45] = "General purpose.",
        [KDRV_ES_CHD1]  = "Chip identity 1. Reads 131 (0x83) on an ES8311.",
        [KDRV_ES_CHD2]  = "Chip identity 2. Reads 17 (0x11) on an ES8311.",
        [KDRV_ES_CHVER] = "Chip version.",
    };
    _Static_assert((sizeof(AUDIO_FIELD_DESC) / sizeof(AUDIO_FIELD_DESC[0]))
                   == (size_t)KDRV_ES_FIELD_MAX,
                   "AUDIO_FIELD_DESC is missing an entry for an es8311 field");

    const kdrv_es8311_field_info_t * const i = kdrv_es8311_field_info((kdrv_es8311_field_t)field);
    bool const ok = (NULL != i);

    if ( ok )
    {
        out->name = i->name; out->unit = i->unit;
        out->min = i->min;   out->max = i->max;
        out->writable = i->writable;
        out->desc = AUDIO_FIELD_DESC[field];
    }
    return ok;
}

/** @brief Ask the owner about a codec field. */
static void audio_field_ask(uint8_t op, uint16_t field, int32_t value)
{
    audio_field_request(op, field, value);
}

static const console_chip_t AUDIO_CHIP = {
    .chip = "audio", .count = (uint16_t)KDRV_ES_FIELD_MAX,
    .by_name = audio_field_by_name, .row = audio_field_row, .ask = audio_field_ask,
};

/** @brief audio: the codec field table. */
static void h_audio(const kverb_t *c, const karg_val_t *v)
{
    s_audio_rec = false;
    console_field_cmd(&AUDIO_CHIP, c->verb, v);
}

/** @brief audio_help: the codec field table, in slices. */
static void h_audio_help(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    s_audio_rec = false;
    console_field_help(&AUDIO_CHIP, c->verb);
}

/** @brief A recording with no `ms=`. One analysis window and a little more. */
#define AUDIO_V_REC_MS_DEF_N   1000                              /**< Bare, for `help`. */
#define AUDIO_V_REC_MS_DEF     ((uint32_t)AUDIO_V_REC_MS_DEF_N)
/** @brief On top of the recording: opening the codec, and the write. */
#define AUDIO_V_REC_MARGIN_MS 30000U

/** @brief A tone with no `ms=`. Long enough to hear, short enough to wait. */
#define AUDIO_V_TONE_MS_DEF_N  300                               /**< Bare, for `help`. */
#define AUDIO_V_TONE_MS_DEF    ((uint16_t)AUDIO_V_TONE_MS_DEF_N)
/** @brief A tone with no `amp=`. The production row's own quarter scale. */
#define AUDIO_V_TONE_AMP_DEF_N 25                                /**< Bare, for `help`. */
#define AUDIO_V_TONE_AMP_DEF   ((uint8_t)AUDIO_V_TONE_AMP_DEF_N)

/**
 * @brief audio_tone freq= ms= amp=: make a noise.
 *
 * The frequency is snapped to the nearest detector bin and the SNAPPED value
 * is what comes back. Fire and forget: the tone plays after the command has
 * closed, and a busy codec says so in the log, not in this reply.
 */
static void h_audio_tone(const kverb_t *c, const karg_val_t *v)
{
    /* Defaults are applied here; the parser only prints them. The frequency's
     * is the lowest detector bin; A_AUDIO_TONE prints the same number and the
     * host suite asserts the two agree. */
    uint16_t hz  = ktone_bin_hz(0U);
    uint16_t ms  = AUDIO_V_TONE_MS_DEF;
    uint8_t  amp = AUDIO_V_TONE_AMP_DEF;

    (void)c;
    if ( v[0].present )
    {
        hz = (uint16_t)v[0].u;
    }
    if ( v[1].present )
    {
        ms = (uint16_t)v[1].u;
    }
    if ( v[2].present )
    {
        amp = (uint8_t)v[2].u;
    }
    /* Snapped HERE, so the request and the record carry the same frequency. */
    hz = ktone_bin_hz(ktone_bin_nearest(hz));

    audio_tone_request(hz, ms, amp);
    console_reply(">|tone=%" PRIu32 " ms=%" PRIu32 " amp=%" PRIu32, (uint32_t)hz, (uint32_t)ms, (uint32_t)amp);
    console_reply(">ok");
}

/**
 * @brief audio_rec ms= file=: record, and put it on a medium as a WAV.
 *
 * The path is resolved HERE, because the current directory is the console's,
 * not AO_Audio's. The bytes go out through AO_Storage's whole-buffer write, as
 * the facade's file calls are bounded to a few kilobytes. The bench uses the
 * waveform to set the thresholds `test component=audio` judges against.
 */
static void h_audio_rec(const kverb_t *c, const karg_val_t *v)
{
    char     path[KODE_PATH_MAX];
    uint32_t ms = AUDIO_V_REC_MS_DEF;

    if ( v[0].present )
    {
        ms = (uint32_t)v[0].u;
    }
    if ( console_fs_resolve(v[1].s, path, sizeof(path)) != KODE_OK )
    {
        console_reply_err(KCLI_ERR_VALUE, "file");
        return;
    }
    console_begin(c->verb);
    s_audio_rec = true;
    /* The recording's length plus the write; the margin is sized for a
     * megabyte of WAV onto the internal FAT. */
    console_extend(ms + AUDIO_V_REC_MARGIN_MS);
    audio_record_file_request(path, ms, NULL, NULL, NULL, 0U, NULL);
}

/** @brief AO_Audio's answers: the field table's, and the recording's. */
static bool audio_answer(QEvt const *e)
{
    bool handled = true;

    if ( (QSignal)AUDIO_FIELD_SIG == e->sig )
    {
        /* Published: AO_Audio does not need to know who asked, and the
         * chip gate is what makes that safe. */
        AudioFieldEvt const * const a = Q_EVT_CAST(AudioFieldEvt);

        console_field_answer(&AUDIO_CHIP, a->op, a->field, a->rc, a->value);
    }
    else if ( ((QSignal)AUDIO_FILE_RSP_SIG == e->sig) && s_audio_rec )
    {
        AudioFileRspEvt const * const a = Q_EVT_CAST(AudioFileRspEvt);
        if ( (uint8_t)KODE_OK == a->rc )
        {
            char qp[CONSOLE_QUOTED_MAX(sizeof(a->path))];
            console_reply(">|rec=%s ms=%" PRIu32 " bytes=%" PRIu32,
                  console_quote(qp, sizeof(qp), a->path),
                  (uint32_t)a->ms, (uint32_t)a->bytes);
            console_reply(">ok");
        }
        else
        {
            console_reply_err(KCLI_ERR_REJECT, console_verb());
        }
        console_end();
    }
    else
    {
        handled = false;
    }
    return handled;
}

/* `audio_rec ms= file=`. The ceiling on `ms` is the driver's buffer; `file` is
 * required, as the point of this verb is the waveform on a host. */
static const karg_spec_t A_AUDIO_REC[] = {
    { .key = "ms", .required = false, .type = KARG_UINT,
      .min = 100U, .max = (KDRV_ES8311_BUF_SECONDS * 1000U),
      .def = CONSOLE_STRINGIFY(AUDIO_V_REC_MS_DEF_N) },
    { .key = "file", .required = true, .type = KARG_STRING },
};
/* `audio_tone freq= ms= amp=`. Below the output filter's corner near 390 Hz a
 * tone is only heat; the ceiling is where the speaker's rub-and-buzz test stops.
 * `amp` is a percentage of full scale, capped under it: the production row runs
 * at a quarter (about 22 mW). */
static const karg_spec_t A_AUDIO_TONE[] = {
    { .key = "freq", .required = false, .type = KARG_UINT,
      .min = KTONE_HZ_MIN, .max = KTONE_HZ_MAX, .def = "1020" },
    { .key = "ms", .required = false, .type = KARG_UINT,
      .min = 10U, .max = 2000U,
      .def = CONSOLE_STRINGIFY(AUDIO_V_TONE_MS_DEF_N) },
    { .key = "amp", .required = false, .type = KARG_UINT,
      .min = 1U, .max = 50U,
      .def = CONSOLE_STRINGIFY(AUDIO_V_TONE_AMP_DEF_N) },
};

static const kverb_t AUDIO_VERBS[] = {
    { "audio",      console_field_args, CONSOLE_FIELD_ARGS_N,     h_audio      },
    { "audio_help", NULL,               0U,                       h_audio_help },
    { "audio_tone", A_AUDIO_TONE,       KVERB_KEYS(A_AUDIO_TONE), h_audio_tone },
    { "audio_rec",  A_AUDIO_REC,        KVERB_KEYS(A_AUDIO_REC),  h_audio_rec  },
};

const kverbs_t audio_verbs = {
    .verbs  = AUDIO_VERBS, .n = sizeof(AUDIO_VERBS) / sizeof(AUDIO_VERBS[0]),
    .answer = audio_answer,
};
