/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file led_verbs.c
 * @brief AO_LedMgr's console verbs: `led_*`, registered by kaos_ledmgr_start().
 */
#include <inttypes.h>
#include <string.h>

#include "kaos.h"
#include "kqp.h"

#include "console_verbs.h"
#include "kverbs.h"

/**
 * @brief Publish a LedEvt-carrying signal to LedMgr (bounded, never asserts).
 * @return true when the event was actually published.
 */
static bool led_pub(enum KodeSignals sig, bool on)
{
    LedEvt *e = Q_NEW_X(LedEvt, 0U, (QSignal)sig);
    if ( NULL != e )
    {
        e->on = on;
        QACTIVE_PUBLISH(&e->super, AO_Console);
    }
    return NULL != e;
}

/** @brief led_on / led_off: LED_SET_SIG with on=true/false. */
static void h_led_set(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_reply_posted(c->verb, led_pub(LED_SET_SIG, strcmp(c->verb, "led_on") == 0));
}

/** @brief led_toggle: LED_TOGGLE_SIG. */
static void h_led_toggle(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_reply_posted(c->verb, led_pub(LED_TOGGLE_SIG, false));
}

/** @brief led_blink period=<ms>: new heartbeat period, heartbeat on. */
static void h_led_blink(const kverb_t *c, const karg_val_t *v)
{
    LedBlinkEvt *e = Q_NEW_X(LedBlinkEvt, 0U, LED_BLINK_PERIOD_SIG);
    if ( NULL != e )
    {
        e->period_ms = (uint16_t)v[0].u;
        QACTIVE_PUBLISH(&e->super, AO_Console);
    }
    bool const hb = led_pub(LED_HEARTBEAT_ENABLE_SIG, true);
    console_reply_posted(c->verb, (NULL != e) && hb);
}

/** @brief led_hb state=on|off: heartbeat on or off. */
static void h_led_hb(const kverb_t *c, const karg_val_t *v)
{
    console_reply_posted(c->verb, led_pub(LED_HEARTBEAT_ENABLE_SIG, v[0].b));
}

/** @brief led_rgb rgb=r,g,b: set the colour. */
static void h_led_rgb(const kverb_t *c, const karg_val_t *v)
{
    LedRgbEvt *e = Q_NEW_X(LedRgbEvt, 0U, LED_RGB_SIG);
    if ( NULL != e )
    {
        e->r = v[0].rgb[0];
        e->g = v[0].rgb[1];
        e->b = v[0].rgb[2];
        QACTIVE_PUBLISH(&e->super, AO_Console);
    }
    console_reply_posted(c->verb, NULL != e);
}

static const karg_spec_t A_PERIOD[] = {
    { .key = "period", .required = true, .type = KARG_UINT,
      .min = 0U, .max = 65535U },
};

static const karg_spec_t A_RGB[] = {
    { .key = "rgb", .required = true, .type = KARG_RGB },
};

static const kverb_t LED_VERBS[] = {
    { "led_on",     NULL,               0U,                   h_led_set    },
    { "led_off",    NULL,               0U,                   h_led_set    },
    { "led_toggle", NULL,               0U,                   h_led_toggle },
    { "led_blink",  A_PERIOD,           KVERB_KEYS(A_PERIOD), h_led_blink  },
    { "led_hb",     console_state_args, CONSOLE_STATE_ARGS_N, h_led_hb     },
    { "led_rgb",    A_RGB,              KVERB_KEYS(A_RGB),    h_led_rgb    },
};

const kverbs_t led_verbs = {
    .verbs = LED_VERBS, .n = sizeof(LED_VERBS) / sizeof(LED_VERBS[0]),
};
