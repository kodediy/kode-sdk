/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_dot_verbs.c
 * @brief AO_TestDot's console verbs: `test_dot_*`, and `test component=all` from the console.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "kaos.h"
#include "kdrv_pcf8574.h"
#include "khal_crc32.h"
#include "kode_log.h"
#include "kqp.h"

#include "console_verbs.h"
#include "kaos_wire.h"
#include "kverbs.h"

/** @brief Characters of one `d=` line's base64, its terminator included. */
#define TD_V_B64_CHARS   ((((KODE_TEST_DOT_BLOB_LINE + 2U) / 3U) * 4U) + 1U)
/** @brief The decimal base, for the numbers this file reads itself. */
#define TD_V_DEC         10U
/** @brief The hexadecimal base, for the relay byte. */
#define TD_V_HEX         16U
/** @brief The largest relay byte. */
#define TD_V_BYTE_MAX    0xFFU
/** @brief A place window's coordinate the line left out: the AO keeps the last good one. */
#define TD_V_NOT_GIVEN   (-1)
/** @brief The largest window coordinate, sensor px. */
#define TD_V_WIN_MAX     2592U
/** @brief The relay pair that must never read R2 on with R1 at rest. */
#define TD_V_PAIR        (KDRV_PCF8574_R1_BIT | KDRV_PCF8574_R2_BIT)
/** @brief The pair's forbidden value: R1 at rest (1) and R2 on (0). */
#define TD_V_FORBIDDEN   KDRV_PCF8574_R1_BIT

/** @brief The command in flight, as this file knows it. */
static uint8_t s_seq;            /**< The last number given.                       */
static uint8_t s_cur;            /**< The number of the command in flight.         */
static bool    s_live;           /**< A command of this table holds the console.  */
/** @brief The blob being written. */
static const uint8_t *s_data;    /**< Its bytes, lent by AO_TestDot.               */
static uint32_t       s_len;     /**< How many.                                   */
static uint32_t       s_pos;     /**< How many are written.                       */
static bool           s_lent;    /**< A blob is being written.                    */

/* ---- The way in ------------------------------------------------------------------ */

/**
 * @brief Post @p proto to AO_TestDot as the command in flight.
 * @param verb  The verb, for the console's books.
 * @param proto The request, its keys filled in.
 * @param fail  The refusal's code when the post is refused.
 * @param why   Its word.
 */
static void td_post(const char *verb, TestDotReqEvt const *proto, kcli_err_t fail, const char *why)
{
    TestDotReqEvt *e;

    console_begin(verb);
    console_extend(KODE_TEST_DOT_LINE_MS);
    s_seq++;
    e = Q_NEW_X(TestDotReqEvt, KODE_TEST_DOT_POOL_MARGIN, TEST_DOT_REQ_SIG);
    if ( NULL != e )
    {
        e->who  = AO_Console;
        e->verb = proto->verb;
        e->seq  = s_seq;
        e->a    = proto->a;
        if ( QACTIVE_POST_X(AO_TestDot, &e->super, KODE_TEST_DOT_REQ_MARGIN, AO_Console) )
        {
            s_cur  = s_seq;
            s_live = true;
            return;
        }
    }
    console_reply_err(fail, why);
    console_end();
}

/** @brief A plain request for @p verb with no keys. */
static void td_plain(const kverb_t *c, uint8_t verb)
{
    TestDotReqEvt r;

    if ( !console_src_allows(CONSOLE_NEED_CABLE) )
    {
        return;
    }
    (void)memset(&r, 0, sizeof(r));
    r.verb = verb;
    td_post(c->verb, &r, KCLI_ERR_BUSY, "busy");
}

void test_dot_run_request(void)
{
    TestDotReqEvt r;

    (void)memset(&r, 0, sizeof(r));
    r.verb = (uint8_t)TEST_DOT_VERB_RUN;
    td_post("test", &r, KCLI_ERR_REJECT, "test");
}

/**
 * @brief A number with at most one decimal, in tenths: `15` is 150, `1.5` is 15.
 * @param s   The text.
 * @param max The largest whole number.
 * @param out Out: the tenths.
 * @return true when it read as one.
 */
static bool td_tenths(const char *s, uint32_t max, uint32_t *out)
{
    char *end = NULL;
    unsigned long whole;
    uint32_t frac = 0U;
    bool ok = false;

    if ( (NULL != s) && ('0' <= s[0]) && ('9' >= s[0]) )
    {
        whole = strtoul(s, &end, (int)TD_V_DEC);
        if ( (NULL != end) && ('.' == *end) && ('0' <= end[1]) && ('9' >= end[1]) && ('\0' == end[2]) )
        {
            frac = (uint32_t)(end[1] - '0');
            end  = &end[2];
        }
        if ( (NULL != end) && ('\0' == *end) && ((unsigned long)max >= whole) )
        {
            *out = ((uint32_t)whole * TD_V_DEC) + frac;
            ok   = true;
        }
    }
    return ok;
}

/**
 * @brief A window as `x,y`, sensor px.
 * @param s The text.
 * @param x Out: the column.
 * @param y Out: the row.
 * @return true when it read as one.
 */
static bool td_window(const char *s, int16_t *x, int16_t *y)
{
    char *end = NULL;
    unsigned long a;
    unsigned long b;
    bool ok = false;

    if ( ('0' <= s[0]) && ('9' >= s[0]) )
    {
        a = strtoul(s, &end, (int)TD_V_DEC);
        if ( (NULL != end) && (',' == *end) && ('0' <= end[1]) && ('9' >= end[1]) )
        {
            b = strtoul(&end[1], &end, (int)TD_V_DEC);
            if ( ('\0' == *end) && (TD_V_WIN_MAX >= a) && (TD_V_WIN_MAX >= b) )
            {
                *x = (int16_t)a;
                *y = (int16_t)b;
                ok = true;
            }
        }
    }
    return ok;
}

/** @brief Where `test_dot_place`'s keys land. */
enum { TD_A_WIN = 0, TD_A_CAM_MS, TD_A_MARK_MS, TD_A_DX_MM, TD_A_DEG };

/**
 * @brief test_dot_place [win="x,y"] [cam_ms=] [mark_ms=] [dx_mm=] [deg=]: wake
 *        the station if need be and look for the marks until the screen sits
 *        within the limits, in whole millimetres and degrees or with one decimal.
 *
 * `cam_ms` is checked but bounds nothing: the camera's start is bounded by
 * KODE_TEST_DOT_RAIL_MS and KODE_TEST_DOT_PROBE_MS.
 */
static void h_place(const kverb_t *c, const karg_val_t *v)
{
    TestDotReqEvt r;
    uint32_t t = 0U;

    if ( !console_src_allows(CONSOLE_NEED_CABLE) )
    {
        return;
    }
    (void)memset(&r, 0, sizeof(r));
    r.verb = (uint8_t)TEST_DOT_VERB_PLACE;
    r.a.place.win_x   = (int16_t)TD_V_NOT_GIVEN;
    r.a.place.win_y   = (int16_t)TD_V_NOT_GIVEN;
    r.a.place.mark_ms = KODE_TEST_DOT_PLACE_MARK_MS;
    r.a.place.dx_mm10 = (uint16_t)KODE_TEST_DOT_PLACE_MM10;
    r.a.place.deg10   = (uint16_t)KODE_TEST_DOT_PLACE_DEG10;
    if ( v[TD_A_WIN].present && !td_window(v[TD_A_WIN].s, &r.a.place.win_x, &r.a.place.win_y) )
    {
        console_reply_err(KCLI_ERR_VALUE, "win");
        return;
    }
    if ( v[TD_A_MARK_MS].present )
    {
        r.a.place.mark_ms = v[TD_A_MARK_MS].u;
    }
    if ( v[TD_A_DX_MM].present )
    {
        if ( v[TD_A_DX_MM].quoted || !td_tenths(v[TD_A_DX_MM].s, KODE_TEST_DOT_PLACE_ARG_MAX / TD_V_DEC, &t) )
        {
            console_reply_err(KCLI_ERR_VALUE, "dx_mm");
            return;
        }
        r.a.place.dx_mm10 = (uint16_t)t;
    }
    if ( v[TD_A_DEG].present )
    {
        if ( v[TD_A_DEG].quoted || !td_tenths(v[TD_A_DEG].s, KODE_TEST_DOT_PLACE_ARG_MAX / TD_V_DEC, &t) )
        {
            console_reply_err(KCLI_ERR_VALUE, "deg");
            return;
        }
        r.a.place.deg10 = (uint16_t)t;
    }
    td_post(c->verb, &r, KCLI_ERR_BUSY, "busy");
}

/** @brief test_dot_poll: what the run said since the line closed, then where it is. */
static void h_poll(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    td_plain(c, (uint8_t)TEST_DOT_VERB_POLL);
}

/** @brief `test_dot_screen`'s phases and verdicts, in test_dot_phase_t order. */
static const char *const EN_PHASE[]   = { "connecting", "result" };
static const char *const EN_VERDICT[] = { "pass", "fail" };
/** @brief The verdict word that passes. */
#define TD_V_PASS  0U

/** @brief test_dot_screen phase=connecting|result [verdict=pass|fail]: a screen of the station's own. */
static void h_screen(const kverb_t *c, const karg_val_t *v)
{
    TestDotReqEvt r;

    if ( !console_src_allows(CONSOLE_NEED_CABLE) )
    {
        return;
    }
    (void)memset(&r, 0, sizeof(r));
    r.verb = (uint8_t)TEST_DOT_VERB_SCREEN;
    r.a.screen.phase = (uint8_t)v[0].u;
    if ( (uint8_t)TEST_DOT_PHASE_RESULT == r.a.screen.phase )
    {
        if ( !v[1].present )
        {
            console_reply_err(KCLI_ERR_MISSING, "verdict");
            return;
        }
        r.a.screen.pass = (TD_V_PASS == v[1].u);
    }
    td_post(c->verb, &r, KCLI_ERR_BUSY, "busy");
}

/** @brief test_dot_photos [only=]: the kept photos as JPEG blobs. */
static void h_photos(const kverb_t *c, const karg_val_t *v)
{
    TestDotReqEvt r;

    if ( !console_src_allows(CONSOLE_NEED_CABLE) )
    {
        return;
    }
    (void)memset(&r, 0, sizeof(r));
    r.verb = (uint8_t)TEST_DOT_VERB_PHOTOS;
    r.a.photos.one  = v[0].present;
    r.a.photos.only = (uint8_t)v[0].u;
    td_post(c->verb, &r, KCLI_ERR_BUSY, "busy");
}

/** @brief The blobs `test_dot_blob` names, in the request's order. */
static const char *const BLOB_NAMES[] = { "location", "colours" };

/** @brief test_dot_blob name="location"|"colours": one blob of the last pass. */
static void h_blob(const kverb_t *c, const karg_val_t *v)
{
    TestDotReqEvt r;
    bool found = false;

    if ( !console_src_allows(CONSOLE_NEED_CABLE) )
    {
        return;
    }
    (void)memset(&r, 0, sizeof(r));
    r.verb = (uint8_t)TEST_DOT_VERB_BLOB;
    for ( uint8_t i = 0U; (uint8_t)Q_DIM(BLOB_NAMES) > i; i++ )
    {
        if ( !found && (strcmp(v[0].s, BLOB_NAMES[i]) == 0) )
        {
            r.a.blob = i;
            found = true;
        }
    }
    if ( !found )
    {
        console_reply_err(KCLI_ERR_VALUE, "name");
        return;
    }
    td_post(c->verb, &r, KCLI_ERR_BUSY, "busy");
}

/**
 * @brief test_dot_relays state=<byte>: the relay expander's pins, `0xFE` or
 *        `254` alike. R2 on with R1 at rest is refused here, before the AO.
 */
static void h_relays(const kverb_t *c, const karg_val_t *v)
{
    TestDotReqEvt r;
    const char *s = v[0].s;
    char *end = NULL;
    unsigned long b = TD_V_BYTE_MAX + 1UL;

    if ( !console_src_allows(CONSOLE_NEED_CABLE) )
    {
        return;
    }
    if ( !v[0].quoted && (NULL != s) && ('0' <= s[0]) && ('9' >= s[0]) )
    {
        if ( ('0' == s[0]) && (('x' == s[1]) || ('X' == s[1])) && ('\0' != s[2]) )
        {
            b = strtoul(&s[2], &end, (int)TD_V_HEX);
        }
        else
        {
            b = strtoul(s, &end, (int)TD_V_DEC);
        }
    }
    if ( (NULL == end) || ('\0' != *end) || (TD_V_BYTE_MAX < b) )
    {
        console_reply_err(KCLI_ERR_VALUE, "state");
        return;
    }
    if ( ((uint8_t)b & TD_V_PAIR) == TD_V_FORBIDDEN )
    {
        console_reply_err(KCLI_ERR_REJECT, "forbidden");
        return;
    }
    (void)memset(&r, 0, sizeof(r));
    r.verb     = (uint8_t)TEST_DOT_VERB_RELAYS;
    r.a.relays = (uint8_t)b;
    td_post(c->verb, &r, KCLI_ERR_BUSY, "busy");
}

/** @brief test_dot_adc ch=0..3: one input of the station's ADC, mV at its pin. */
static void h_adc(const kverb_t *c, const karg_val_t *v)
{
    TestDotReqEvt r;

    if ( !console_src_allows(CONSOLE_NEED_CABLE) )
    {
        return;
    }
    (void)memset(&r, 0, sizeof(r));
    r.verb = (uint8_t)TEST_DOT_VERB_ADC;
    r.a.ch = (uint8_t)v[0].u;
    td_post(c->verb, &r, KCLI_ERR_BUSY, "busy");
}

/** @brief test_dot_loop [count=1..100]: frames round the dock's pads at two speeds and back. */
static void h_loop(const kverb_t *c, const karg_val_t *v)
{
    TestDotReqEvt r;

    if ( !console_src_allows(CONSOLE_NEED_CABLE) )
    {
        return;
    }
    (void)memset(&r, 0, sizeof(r));
    r.verb    = (uint8_t)TEST_DOT_VERB_LOOP;
    r.a.count = (uint8_t)KODE_TEST_DOT_LOOP_DEF;
    if ( v[0].present )
    {
        r.a.count = (uint8_t)v[0].u;
    }
    td_post(c->verb, &r, KCLI_ERR_BUSY, "busy");
}

/** @brief Where `test_dot_radio`'s keys land. */
enum { TD_R_SSID24 = 0, TD_R_CH24, TD_R_SSID5, TD_R_CH5, TD_R_BLE, TD_R_DISCOVER };

/** @brief test_dot_radio [ssid24=] [ch24=] [ssid5=] [ch5=] [ble=] [discover=]: the C5 and its radios. */
static void h_radio(const kverb_t *c, const karg_val_t *v)
{
    TestDotReqEvt r;

    if ( !console_src_allows(CONSOLE_NEED_CABLE) )
    {
        return;
    }
    (void)memset(&r, 0, sizeof(r));
    r.verb = (uint8_t)TEST_DOT_VERB_RADIO;
    if ( v[TD_R_SSID24].present )
    {
        (void)snprintf(r.a.radio.ssid24, sizeof(r.a.radio.ssid24), "%s", v[TD_R_SSID24].s);
    }
    if ( v[TD_R_CH24].present )
    {
        r.a.radio.ch24 = (uint8_t)v[TD_R_CH24].u;
    }
    if ( v[TD_R_SSID5].present )
    {
        (void)snprintf(r.a.radio.ssid5, sizeof(r.a.radio.ssid5), "%s", v[TD_R_SSID5].s);
    }
    if ( v[TD_R_CH5].present )
    {
        r.a.radio.ch5 = (uint8_t)v[TD_R_CH5].u;
    }
    if ( v[TD_R_BLE].present )
    {
        (void)snprintf(r.a.radio.ble, sizeof(r.a.radio.ble), "%s", v[TD_R_BLE].s);
    }
    r.a.radio.discover = v[TD_R_DISCOVER].present && v[TD_R_DISCOVER].b;
    r.a.radio.card     = true;
    td_post(c->verb, &r, KCLI_ERR_BUSY, "busy");
}

/** @brief test_dot_boost state=on|off: the battery boost at the station's limit, for the load. */
static void h_boost(const kverb_t *c, const karg_val_t *v)
{
    TestDotReqEvt r;

    if ( !console_src_allows(CONSOLE_NEED_CABLE) )
    {
        return;
    }
    (void)memset(&r, 0, sizeof(r));
    r.verb = (uint8_t)TEST_DOT_VERB_BOOST;
    r.a.on = v[0].b;
    td_post(c->verb, &r, KCLI_ERR_BUSY, "busy");
}

/** @brief test_dot_result verdict=pass|fail: a pass erases the Kode for the box first. */
static void h_result(const kverb_t *c, const karg_val_t *v)
{
    TestDotReqEvt r;

    if ( !console_src_allows(CONSOLE_NEED_CABLE) )
    {
        return;
    }
    (void)memset(&r, 0, sizeof(r));
    r.verb   = (uint8_t)TEST_DOT_VERB_RESULT;
    r.a.pass = (TD_V_PASS == v[0].u);
    td_post(c->verb, &r, KCLI_ERR_BUSY, "busy");
}

/** @brief test_dot_lock confirm=on: the station verbs are refused on this Kode from now on. */
static void h_lock(const kverb_t *c, const karg_val_t *v)
{
    if ( !v[0].b )
    {
        console_reply_err(KCLI_ERR_VALUE, "confirm");
        return;
    }
    td_plain(c, (uint8_t)TEST_DOT_VERB_LOCK);
}

/** @brief test_dot_end: give back everything the wake took. */
static void h_end(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    td_plain(c, (uint8_t)TEST_DOT_VERB_END);
}

/** @brief test_dot_info: where AO_TestDot is. */
static void h_info(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    td_plain(c, (uint8_t)TEST_DOT_VERB_INFO);
}

/** @brief The camera's parts `test_dot_camera` takes, in test_dot_cam_t order. */
static const char *const EN_CAM[] = { "framing", "pixels", "lines", "zones", "colour", "led" };

/** @brief test_dot_camera [only=]: the camera part alone, or one of its parts. */
static void h_camera(const kverb_t *c, const karg_val_t *v)
{
    TestDotReqEvt r;

    if ( !console_src_allows(CONSOLE_NEED_CABLE) )
    {
        return;
    }
    (void)memset(&r, 0, sizeof(r));
    r.verb  = (uint8_t)TEST_DOT_VERB_CAMERA;
    r.a.cam = (uint8_t)TEST_DOT_CAM_ALL;
    if ( v[0].present )
    {
        r.a.cam = (uint8_t)v[0].u;
    }
    td_post(c->verb, &r, KCLI_ERR_BUSY, "busy");
}

/* ---- The way out --------------------------------------------------------------------- */

/** @brief The blob is written, or the command died under it: AO_TestDot may use it again. */
static void td_give_back(void)
{
    if ( s_lent )
    {
        s_lent = false;
        s_data = NULL;
        test_dot_blob_back();
    }
}

/** @brief Write one slice of the blob's `d=` lines; the last gives it back. */
static void td_blob_slice(void)
{
    char b64[TD_V_B64_CHARS];
    uint32_t lines = 0U;

    while ( (s_pos < s_len) && (KAOS_WIRE_SLICE > lines) )
    {
        uint32_t n = s_len - s_pos;
        if ( KODE_TEST_DOT_BLOB_LINE < n )
        {
            n = KODE_TEST_DOT_BLOB_LINE;
        }
        (void)kaos_wire_b64_encode(&s_data[s_pos], n, b64);
        console_reply(">|d=%s", b64);
        s_pos += n;
        lines++;
    }
    if ( s_pos < s_len )
    {
        console_pace();
    }
    else
    {
        td_give_back();
        console_extend(KODE_TEST_DOT_LINE_MS);
    }
}

/** @brief A lent blob: its header now, its lines in slices. */
static void td_blob_start(TestDotLendEvt const *l)
{
    char name[CONSOLE_QUOTED_MAX(KODE_TEST_DOT_NAME_MAX)];
    uint32_t const crc = khal_crc32(KHAL_CRC32_INIT, l->data, l->len);

    s_data = l->data;
    s_len  = l->len;
    s_pos  = 0U;
    s_lent = true;
    console_reply(">|blob name=%s len=%lu crc=%lu", console_kv_text(name, sizeof(name), l->name),
                  (unsigned long)l->len, (unsigned long)crc);
    td_blob_slice();
}

/**
 * @brief The events of a station verb while it holds the console.
 *
 * A record or an end is written only when it carries the number of the
 * command in flight; one of a command already closed is dropped. A blob of a
 * command already closed goes straight back.
 *
 * @param e The event.
 * @return true when it was this table's.
 */
static bool td_answer(QEvt const *e)
{
    bool handled = true;

    switch ( e->sig )
    {
        case TEST_DOT_REC_SIG: {
            TestDotRecEvt const * const r = Q_EVT_CAST(TestDotRecEvt);
            if ( s_live && (r->seq == s_cur) )
            {
                console_reply(">|%s", r->text);
            }
            break;
        }
        case TEST_DOT_ANS_SIG: {
            TestDotAnsEvt const * const a = Q_EVT_CAST(TestDotAnsEvt);
            if ( s_live && (a->seq == s_cur) )
            {
                if ( 0U == a->code )
                {
                    console_reply(">ok");
                }
                else
                {
                    console_reply_err((kcli_err_t)a->code, a->msg);
                }
                s_live = false;
                td_give_back();
                console_end();
            }
            break;
        }
        case TEST_DOT_LEND_SIG: {
            TestDotLendEvt const * const l = Q_EVT_CAST(TestDotLendEvt);
            if ( s_live && !s_lent && (l->seq == s_cur) )
            {
                td_blob_start(l);
            }
            else
            {
                test_dot_blob_back();
            }
            break;
        }
        case CONSOLE_SLICE_SIG:
            if ( !s_lent )
            {
                handled = false;
            }
            else if ( console_congested() )
            {
                console_pace();
            }
            else
            {
                td_blob_slice();
            }
            break;
        default:
            handled = false;
            break;
    }
    return handled;
}

/** @brief The watchdog fired on a station verb: the blob back, and the console answers the timeout. */
static bool td_timeout(void)
{
    s_live = false;
    td_give_back();
    return false;
}

/* ---- The table ----------------------------------------------------------------------- */

static const karg_spec_t A_PLACE[] = {
    { .key = "win",     .required = false, .type = KARG_TEXT },
    { .key = "cam_ms",  .required = false, .type = KARG_UINT, .min = 1U, .max = KODE_TEST_DOT_PLACE_ARG_MAX },
    { .key = "mark_ms", .required = false, .type = KARG_UINT, .min = 1U, .max = KODE_TEST_DOT_PLACE_ARG_MAX },
    { .key = "dx_mm",   .required = false, .type = KARG_ROW },
    { .key = "deg",     .required = false, .type = KARG_ROW },
};
static const karg_spec_t A_SCREEN[] = {
    { .key = "phase",   .required = true,  .type = KARG_ENUM, .enums = EN_PHASE,   .n_enums = (uint8_t)Q_DIM(EN_PHASE) },
    { .key = "verdict", .required = false, .type = KARG_ENUM, .enums = EN_VERDICT, .n_enums = (uint8_t)Q_DIM(EN_VERDICT) },
};
static const karg_spec_t A_PHOTOS[] = {
    { .key = "only", .required = false, .type = KARG_UINT, .min = TEST_DOT_PHOTO_FIRST, .max = TD_V_BYTE_MAX },
};
static const karg_spec_t A_BLOB[] = {
    { .key = "name", .required = true, .type = KARG_STRING },
};
static const karg_spec_t A_RELAYS[] = {
    { .key = "state", .required = true, .type = KARG_ROW },
};
static const karg_spec_t A_ADC[] = {
    { .key = "ch", .required = true, .type = KARG_UINT, .min = 0U, .max = KODE_TEST_DOT_ADC_CHANNELS - 1U },
};
static const karg_spec_t A_LOOP[] = {
    { .key = "count", .required = false, .type = KARG_UINT, .min = 1U, .max = KODE_TEST_DOT_LOOP_MAX },
};
static const karg_spec_t A_RADIO[] = {
    { .key = "ssid24",   .required = false, .type = KARG_TEXT },
    { .key = "ch24",     .required = false, .type = KARG_UINT, .min = 0U, .max = TD_V_BYTE_MAX },
    { .key = "ssid5",    .required = false, .type = KARG_TEXT },
    { .key = "ch5",      .required = false, .type = KARG_UINT, .min = 0U, .max = TD_V_BYTE_MAX },
    { .key = "ble",      .required = false, .type = KARG_TEXT },
    { .key = "discover", .required = false, .type = KARG_BOOL },
};
static const karg_spec_t A_BOOST[] = {
    { .key = "state", .required = true, .type = KARG_BOOL },
};
static const karg_spec_t A_RESULT[] = {
    { .key = "verdict", .required = true, .type = KARG_ENUM, .enums = EN_VERDICT, .n_enums = (uint8_t)Q_DIM(EN_VERDICT) },
};
static const karg_spec_t A_LOCK[] = {
    { .key = "confirm", .required = true, .type = KARG_BOOL },
};
static const karg_spec_t A_CAMERA[] = {
    { .key = "only", .required = false, .type = KARG_ENUM, .enums = EN_CAM, .n_enums = (uint8_t)Q_DIM(EN_CAM) },
};

static const kverb_t TD_VERBS[] = {
    { "test_dot_place",  A_PLACE,  KVERB_KEYS(A_PLACE),  h_place  },
    { "test_dot_poll",   NULL,     0U,                   h_poll   },
    { "test_dot_screen", A_SCREEN, KVERB_KEYS(A_SCREEN), h_screen },
    { "test_dot_photos", A_PHOTOS, KVERB_KEYS(A_PHOTOS), h_photos },
    { "test_dot_blob",   A_BLOB,   KVERB_KEYS(A_BLOB),   h_blob   },
    { "test_dot_relays", A_RELAYS, KVERB_KEYS(A_RELAYS), h_relays },
    { "test_dot_adc",    A_ADC,    KVERB_KEYS(A_ADC),    h_adc    },
    { "test_dot_loop",   A_LOOP,   KVERB_KEYS(A_LOOP),   h_loop   },
    { "test_dot_radio",  A_RADIO,  KVERB_KEYS(A_RADIO),  h_radio  },
    { "test_dot_boost",  A_BOOST,  KVERB_KEYS(A_BOOST),  h_boost  },
    { "test_dot_result", A_RESULT, KVERB_KEYS(A_RESULT), h_result },
    { "test_dot_lock",   A_LOCK,   KVERB_KEYS(A_LOCK),   h_lock   },
    { "test_dot_end",    NULL,     0U,                   h_end    },
    { "test_dot_info",   NULL,     0U,                   h_info   },
    { "test_dot_camera", A_CAMERA, KVERB_KEYS(A_CAMERA), h_camera },
};

const kverbs_t test_dot_verbs = {
    .verbs   = TD_VERBS,
    .n       = Q_DIM(TD_VERBS),
    .answer  = &td_answer,
    .timeout = &td_timeout,
    .info    = NULL,
};
