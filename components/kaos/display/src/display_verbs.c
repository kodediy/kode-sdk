/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file display_verbs.c
 * @brief AO_Display's console verbs: `display_info` and `mirror_*`, registered by kaos_display_start().
 */
#include <inttypes.h>

#include "sdkconfig.h"

#include "kaos.h"
#include "kdrv_display.h"

#include "console_verbs.h"
#include "kverbs.h"

/**
 * @brief display_info: the DSI link's counters since the panel came up.
 *
 * Synchronous: the counters are statics the frame interrupt writes, and reading
 * them touches no device. An underrun shows only on the glass, never in a capture.
 */
static void h_display_info(const kverb_t *c, const karg_val_t *v)
{
    kdrv_display_link_t link;

    (void)c; (void)v;
    kdrv_display_link(&link);
    console_reply(">|display frames=%" PRIu32 " underruns=%" PRIu32 " dpi=%" PRIu32 " ack=%" PRIu32 " phy=%" PRIu32 " "
          "other=%" PRIu32 " st0=0x%08" PRIx32 " st1=0x%08" PRIx32 " missed=%" PRIu32,
          (uint32_t)link.frames, (uint32_t)link.underruns,
          (uint32_t)link.dpi, (uint32_t)link.ack,
          (uint32_t)link.phy, (uint32_t)link.other,
          (uint32_t)link.st0, (uint32_t)link.st1,
          (uint32_t)link.missed);
    console_reply(">ok");
}

#if CONFIG_KODEOS_MIRROR
/* ---- The mirror ------------------------------------------------------------
 * AO_Display owns the codec, the scaler and the credit window, so these verbs
 * only post events: start/stop/ack cannot fail and answer `>ok` at once.
 * mirror_info waits for AO_Display's status reply.
 */

/* The pair the JPEG measurements found usable on the air (9.4 KB a frame at
 * 205x251): the verb's defaults, not just the state machine's fallback. Bare,
 * so `help` prints them. */
#define DISPLAY_V_MIRROR_SCALE_DEF_N  2   /**< `scale=` when absent. */
#define DISPLAY_V_MIRROR_Q_DEF_N      70  /**< `q=` when absent. */

/** @brief mirror_info holds the console: DISPLAY_MIRROR_INFO_SIG is its answer. */
static bool s_display_info_asked;

/**
 * @brief mirror_start: begin streaming the panel. Always answers `>ok`.
 *        Refused from the Kode channel: its frames are `>|m=` records.
 */
static void h_mirror_start(const kverb_t *c, const karg_val_t *v)
{
    uint8_t scale   = (uint8_t)DISPLAY_V_MIRROR_SCALE_DEF_N;
    uint8_t quality = (uint8_t)DISPLAY_V_MIRROR_Q_DEF_N;

    (void)c;
    if ( console_src_allows(CONSOLE_NEED_WIRE) )
    {
        if ( v[0].present )
        {
            scale = (uint8_t)v[0].u;
        }
        if ( v[1].present )
        {
            quality = (uint8_t)v[1].u;
        }
        display_mirror_start(scale, quality);
        console_reply(">ok");
    }
}

/** @brief mirror_stop: stop streaming; a mirror nobody acks stalls anyway. */
static void h_mirror_stop(const kverb_t *c, const karg_val_t *v)
{
    (void)c; (void)v;
    display_mirror_stop();
    console_reply(">ok");
}

/**
 * @brief mirror_ack seq=<n>: the host has received up to @c seq. Required:
 *        display_mirror_ack() will not open the credit window without it.
 */
static void h_mirror_ack(const kverb_t *c, const karg_val_t *v)
{
    (void)c;
    display_mirror_ack((uint16_t)v[0].u);
    console_reply(">ok");
}

/** @brief mirror_info: closed by DISPLAY_MIRROR_INFO_SIG. */
static void h_mirror_info(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_display_info_asked = true;
    display_mirror_info_request();
}

/** @brief AO_Display's answer to mirror_info. */
static bool display_answer(QEvt const *e)
{
    bool handled = false;

    if ( ((QSignal)DISPLAY_MIRROR_INFO_SIG == e->sig) && s_display_info_asked )
    {
        MirrorInfoEvt const * const m = Q_EVT_CAST(MirrorInfoEvt);

        console_reply(">|mirror=%s paused=%s scale=%" PRIu32 " q=%" PRIu32
                      " window=%" PRIu32 " in_flight=%" PRIu32 " sent=%" PRIu32
                      " stalls=%" PRIu32,
                      console_on_off(m->on), console_on_off(m->paused),
                      (uint32_t)m->scale, (uint32_t)m->quality,
                      (uint32_t)m->window, (uint32_t)m->in_flight,
                      (uint32_t)m->sent, (uint32_t)m->stalls);
        console_reply(">ok");
        console_end();
        s_display_info_asked = false;
        handled = true;
    }
    return handled;
}

/* Same bounds as display_mirror_start(): it rewrites any scale but 1 or 2 to 1
 * and a quality outside 1..100 to 70. */
static const karg_spec_t A_MIRROR[] = {
    { .key = "scale", .required = false, .type = KARG_UINT,
      .min = 1U, .max = 2U, .def = CONSOLE_STRINGIFY(DISPLAY_V_MIRROR_SCALE_DEF_N) },
    { .key = "q", .required = false, .type = KARG_UINT,
      .min = 1U, .max = 100U, .def = CONSOLE_STRINGIFY(DISPLAY_V_MIRROR_Q_DEF_N) },
};
static const karg_spec_t A_MIRROR_ACK[] = {
    { .key = "seq", .required = true, .type = KARG_UINT,
      .min = 0U, .max = UINT16_MAX },
};
#endif /* CONFIG_KODEOS_MIRROR */

static const kverb_t DISPLAY_VERBS[] = {
#if CONFIG_KODEOS_MIRROR
    { "mirror_start", A_MIRROR,     KVERB_KEYS(A_MIRROR),     h_mirror_start },
    { "mirror_stop",  NULL,         0U,                       h_mirror_stop  },
    { "mirror_ack",   A_MIRROR_ACK, KVERB_KEYS(A_MIRROR_ACK), h_mirror_ack   },
    { "mirror_info",  NULL,         0U,                       h_mirror_info  },
#endif
    { "display_info", NULL,         0U,                       h_display_info },
};

const kverbs_t display_verbs = {
    .verbs = DISPLAY_VERBS, .n = sizeof(DISPLAY_VERBS) / sizeof(DISPLAY_VERBS[0]),
#if CONFIG_KODEOS_MIRROR
    .answer = display_answer,
#endif
};
