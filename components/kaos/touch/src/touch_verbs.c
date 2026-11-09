/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file touch_verbs.c
 * @brief AO_Touch's console verbs: `tp` and `tp_help`, registered by kaos_touch_start().
 */
#include <inttypes.h>

#include "kaos.h"
#include "kdrv_cst820.h"
#include "kqp.h"

#include "console_verbs.h"
#include "kverbs.h"

/** @brief Resolve a touch controller field name. */
static kode_status_t tp_field_by_name(const char *name, uint16_t *field)
{
    kdrv_cst820_field_t f = KDRV_CST_FIELD_MAX;
    kode_status_t const rc = kdrv_cst820_field_by_name(name, &f);

    *field = (uint16_t)f;
    return rc;
}

/** @brief Describe a touch controller field. */
static bool tp_field_row(uint16_t field, console_field_t *out)
{
    /* The touch controller's table: the CST816 family's register list, with
     * what each field does to a finger on this glass. The frame and the identity
     * are read-only; the identity comes from the bring-up cache, so it answers
     * with the chip asleep - the frame and the rest do not. */
    static const char *const TP_FIELD_DESC[] = {
        [KDRV_CST_GESTURE]           = "The last gesture: 0 none, 1 up, 2 down, 3 left, 4 right, 5 click, 11 double, 12 long.",
        [KDRV_CST_FINGERS]           = "How many fingers are down, 0 or 1.",
        [KDRV_CST_EVENT]             = "The event flag of the frame: 0 down, 1 up, 2 contact.",
        [KDRV_CST_X]                 = "x of the finger, in the controller's own pixels, uncalibrated.",
        [KDRV_CST_Y]                 = "y of the finger, in the controller's own pixels, uncalibrated.",
        [KDRV_CST_CHIP_ID]           = "The part, as read at bring-up; a CST820 reads 0xB7.",
        [KDRV_CST_PROJ_ID]           = "The panel supplier's project, as read at bring-up.",
        [KDRV_CST_FW_VER]            = "The controller's firmware, as read at bring-up.",
        [KDRV_CST_MOTION_MASK]       = "Which gestures are reported; bits 0..2 below.",
        [KDRV_CST_EN_DCLICK]         = "Report a double click as a gesture.",
        [KDRV_CST_EN_CON_UD]         = "Report a continuous up/down slide.",
        [KDRV_CST_EN_CON_LR]         = "Report a continuous left/right slide.",
        [KDRV_CST_IRQ_PULSE_WIDTH]   = "How long INT (the expander's P6) is held low per report, in 0.1 ms.",
        [KDRV_CST_NOR_SCAN_PER]      = "How often the panel is scanned while awake, in 10 ms.",
        [KDRV_CST_MOTION_SL_ANGLE]   = "The angle that separates a horizontal slide from a vertical one.",
        [KDRV_CST_LP_SCAN_RAW1]      = "Low-power scan, channel 1, raw reading.",
        [KDRV_CST_LP_SCAN_RAW2]      = "Low-power scan, channel 2, raw reading.",
        [KDRV_CST_LP_AUTO_WAKE_TIME] = "How often the low-power scan recalibrates, in minutes.",
        [KDRV_CST_LP_SCAN_TH]        = "The low-power scan's wake threshold.",
        [KDRV_CST_LP_SCAN_WIN]       = "The low-power scan's window.",
        [KDRV_CST_LP_SCAN_FREQ]      = "The low-power scan's frequency.",
        [KDRV_CST_LP_SCAN_IDAC]      = "The low-power scan's current.",
        [KDRV_CST_AUTO_SLEEP_TIME]   = "Idle seconds before the chip sleeps and stops answering the bus.",
        [KDRV_CST_IRQ_CTL]           = "What pulses INT; bits 7..4 and 0 below.",
        [KDRV_CST_EN_TEST]           = "INT pulses on its own, periodically: a wiring test.",
        [KDRV_CST_EN_TOUCH]          = "INT pulses on every touch report.",
        [KDRV_CST_EN_CHANGE]         = "INT pulses when the touch state changes.",
        [KDRV_CST_EN_MOTION]         = "INT pulses on a gesture.",
        [KDRV_CST_ONCE_WLP]          = "One INT pulse for a long press rather than a train.",
        [KDRV_CST_AUTO_RESET]        = "Seconds of a touch with no gesture before the chip resets itself; 0 off.",
        [KDRV_CST_LONG_PRESS_TIME]   = "Seconds a press is held before it is a long press; 0 off.",
        [KDRV_CST_IO_CTL]            = "The I/O block; bits 2..0 below.",
        [KDRV_CST_SOFT_RST]          = "Hold the chip in reset from software.",
        [KDRV_CST_IIC_OD]            = "I2C pins open drain (1) or push-pull (0).",
        [KDRV_CST_EN_1V8]            = "I/O at 1.8 V (1) or at VDD (0).",
        [KDRV_CST_DIS_AUTO_SLEEP]    = "Non-zero keeps the chip awake; shipped as 1. Writing 0 puts it to sleep within seconds, and only a finger or a reset wakes it.",
    };
    _Static_assert((sizeof(TP_FIELD_DESC) / sizeof(TP_FIELD_DESC[0]))
                   == (size_t)KDRV_CST_FIELD_MAX,
                   "TP_FIELD_DESC is missing an entry for a cst820 field");

    const kdrv_cst820_field_info_t * const i = kdrv_cst820_field_info((kdrv_cst820_field_t)field);
    bool const ok = (NULL != i);

    if ( ok )
    {
        out->name = i->name; out->unit = i->unit;
        out->min = i->min;   out->max = i->max;
        out->writable = i->writable;
        out->desc = TP_FIELD_DESC[field];
    }
    return ok;
}

/** @brief Ask the owner about a touch controller field. */
static void tp_field_ask(uint8_t op, uint16_t field, int32_t value)
{
    touch_field_request(AO_Console, op, field, value);
}

static const console_chip_t TP_CHIP = {
    .chip = "tp", .count = (uint16_t)KDRV_CST_FIELD_MAX,
    .by_name = tp_field_by_name, .row = tp_field_row, .ask = tp_field_ask,
};

/** @brief tp: the touch controller field table. */
static void h_tp(const kverb_t *c, const karg_val_t *v)
{
    console_field_cmd(&TP_CHIP, c->verb, v);
}

/** @brief tp_help: the touch controller field table, in slices. */
static void h_tp_help(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_field_help(&TP_CHIP, c->verb);
}

/** @brief AO_Touch's answer, posted to the console alone. */
static bool touch_answer(QEvt const *e)
{
    bool handled = false;

    if ( (QSignal)TOUCH_FIELD_RSP_SIG == e->sig )
    {
        TouchFieldEvt const * const a = Q_EVT_CAST(TouchFieldEvt);

        console_field_answer(&TP_CHIP, a->op, a->field, a->rc, a->value);
        handled = true;
    }
    return handled;
}

static const kverb_t TOUCH_VERBS[] = {
    { "tp",      console_field_args, CONSOLE_FIELD_ARGS_N, h_tp      },
    { "tp_help", NULL,               0U,                   h_tp_help },
};

const kverbs_t touch_verbs = {
    .verbs  = TOUCH_VERBS, .n = sizeof(TOUCH_VERBS) / sizeof(TOUCH_VERBS[0]),
    .answer = touch_answer,
};
