/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file haptic_verbs.c
 * @brief AO_Haptic's console verbs: `haptic`, `haptic_help` and `haptic_play`, registered by kaos_haptic_start().
 */
#include <inttypes.h>

#include "kaos.h"
#include "kdrv_aw86233.h"
#include "kqp.h"

#include "console_verbs.h"
#include "kverbs.h"

/** @brief Resolve a haptic driver field name. */
static kode_status_t haptic_field_by_name(const char *name, uint16_t *field)
{
    kdrv_aw86233_field_t f = KDRV_AW_FIELD_MAX;
    kode_status_t const rc = kdrv_aw86233_field_by_name(name, &f);

    *field = (uint16_t)f;
    return rc;
}

/** @brief Describe a haptic driver field. */
static bool haptic_field_row(uint16_t field, console_field_t *out)
{
    /* The haptic driver's table. Small on purpose: the AW86233 is driven in RTP,
     * where the waveform comes from this board rather than from the chip's own
     * memory, so the only registers worth a name are the ones a buzz passes
     * through. */
    static const char *const HAPTIC_FIELD_DESC[] = {
        [KDRV_AW_PRESENT]   = "A chip answered at 0x5A, said it is an AW8623X and came up.",
        [KDRV_AW_CHIPID_FIELD] = "CHIPID (0x57:0x69) as read at bring-up: 0x2330 on an AW86233.",
        [KDRV_AW_SYSST]     = "0x01: the whole status byte.",
        [KDRV_AW_FF_AFS]    = "0x01 bit 3: the RTP FIFO is almost full, which is what paces the feed.",
        [KDRV_AW_GAIN]      = "0x07: the amplitude the chip drives at, 0 to 255. The motor's rating caps what a buzz may ask for.",
        [KDRV_AW_PLAY_MODE] = "0x08 bits 1:0: 1 is RTP, the only mode this board plays in.",
        [KDRV_AW_GLB_STATE] = "Register 0x3F bits [3:0], what the chip is doing: 0 standby, 8 RTP, 11 brake.",
        [KDRV_AW_SYSCTRL2]  = "0x46: standby and the playback sample rate.",
        [KDRV_AW_STANDBY]   = "0x46 bit 6: nothing plays while this is set.",
    };
    _Static_assert((sizeof(HAPTIC_FIELD_DESC) / sizeof(HAPTIC_FIELD_DESC[0]))
                   == (size_t)KDRV_AW_FIELD_MAX,
                   "HAPTIC_FIELD_DESC is missing an entry for an aw86233 field");

    const kdrv_aw86233_field_info_t * const i = kdrv_aw86233_field_info((kdrv_aw86233_field_t)field);
    bool const ok = (NULL != i);

    if ( ok )
    {
        out->name = i->name; out->unit = i->unit;
        out->min = i->min;   out->max = i->max;
        out->writable = i->writable;
        out->desc = HAPTIC_FIELD_DESC[field];
    }
    return ok;
}

/** @brief Ask the owner about a haptic driver field. */
static void haptic_field_ask(uint8_t op, uint16_t field, int32_t value)
{
    haptic_field_request(op, field, value);
}

static const console_chip_t HAPTIC_CHIP = {
    .chip = "haptic", .count = (uint16_t)KDRV_AW_FIELD_MAX,
    .by_name = haptic_field_by_name, .row = haptic_field_row, .ask = haptic_field_ask,
};

/** @brief haptic: the haptic driver field table. */
static void h_haptic(const kverb_t *c, const karg_val_t *v)
{
    console_field_cmd(&HAPTIC_CHIP, c->verb, v);
}

/** @brief haptic_help: the haptic driver field table, in slices. */
static void h_haptic_help(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_field_help(&HAPTIC_CHIP, c->verb);
}

/** @brief A buzz with no `ms=`. The production row's own, so what a bench
 *         feels by hand is what the row measures. */
#define HAPTIC_V_PLAY_MS_DEF_N   200  /**< KODE_HAPTIC_TEST_MS, bare for `help`. */
#define HAPTIC_V_PLAY_MS_DEF     ((uint16_t)HAPTIC_V_PLAY_MS_DEF_N)
_Static_assert((uint32_t)HAPTIC_V_PLAY_MS_DEF_N == KODE_HAPTIC_TEST_MS,
               "haptic_play's default is the production row's buzz");
/** @brief A buzz with no `gain=`. Half scale: felt on a desk, and well under
 *         the motor's rating, which the driver clamps to anyway. */
#define HAPTIC_V_PLAY_GAIN_DEF_N 50   /**< Bare, for `help`. */
#define HAPTIC_V_PLAY_GAIN_DEF   ((uint8_t)HAPTIC_V_PLAY_GAIN_DEF_N)

/**
 * @brief haptic_play ms= gain=: buzz.
 *
 * Fire and forget: the buzz plays after the command has closed, and the record
 * says what was asked for. A motor that is not fitted is reported once in the
 * log by AO_Haptic. The one refusal on the wire is `busy`: a buzz is playing,
 * or the production row holds the motor.
 */
static void h_haptic_play(const kverb_t *c, const karg_val_t *v)
{
    uint16_t ms   = HAPTIC_V_PLAY_MS_DEF;
    uint8_t  gain = HAPTIC_V_PLAY_GAIN_DEF;

    if ( v[0].present )
    {
        ms = (uint16_t)v[0].u;
    }
    if ( v[1].present )
    {
        gain = (uint8_t)v[1].u;
    }

    kode_status_t const rc = haptic_play_request(ms, gain);
    if ( KODE_DENIED == rc )
    {
        console_reply_err(KCLI_ERR_REJECT, "busy");
        return;
    }
    if ( KODE_OK != rc )
    {
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        return;
    }
    console_reply(">|play=1 ms=%" PRIu32 " gain=%" PRIu32, (uint32_t)ms, (uint32_t)gain);
    console_reply(">ok");
}

/** @brief AO_Haptic's answer to the field table, published. */
static bool haptic_answer(QEvt const *e)
{
    bool handled = false;

    if ( (QSignal)HAPTIC_FIELD_SIG == e->sig )
    {
        HapticFieldEvt const * const a = Q_EVT_CAST(HapticFieldEvt);

        console_field_answer(&HAPTIC_CHIP, a->op, a->field, a->rc, a->value);
        handled = true;
    }
    return handled;
}

/* `haptic_play ms= gain=`. `ms` runs 1 to 2000: longer is a fault, not a buzz.
 * `gain` is a percentage of full scale, which the driver clamps to the motor's
 * rating, so 100 means "as hard as this board may drive it". */
static const karg_spec_t A_HAPTIC_PLAY[] = {
    { .key = "ms", .required = false, .type = KARG_UINT,
      .min = 1U, .max = 2000U,
      .def = CONSOLE_STRINGIFY(HAPTIC_V_PLAY_MS_DEF_N) },
    { .key = "gain", .required = false, .type = KARG_UINT,
      .min = 0U, .max = 100U,
      .def = CONSOLE_STRINGIFY(HAPTIC_V_PLAY_GAIN_DEF_N) },
};

static const kverb_t HAPTIC_VERBS[] = {
    { "haptic",      console_field_args, CONSOLE_FIELD_ARGS_N,      h_haptic      },
    { "haptic_help", NULL,               0U,                        h_haptic_help },
    { "haptic_play", A_HAPTIC_PLAY,      KVERB_KEYS(A_HAPTIC_PLAY), h_haptic_play },
};

const kverbs_t haptic_verbs = {
    .verbs  = HAPTIC_VERBS, .n = sizeof(HAPTIC_VERBS) / sizeof(HAPTIC_VERBS[0]),
    .answer = haptic_answer,
};
