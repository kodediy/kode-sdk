/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_haptic.c
 * @brief kode_haptic_vibrate(), kode_haptic_play() and kode_haptic_add_profile().
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "qpc.h"

#include "fakes.h"
#include "kaos.h"
#include "khal_once.h"
#include "kode.h"
#include "kode_sys.h"
#include "kode_sys_priv.h"
#include "khal_system.h"
#include "kqp.h"
#include "motion_support.h"
#include "qf_support.h"

/** @brief Stack of the second task, in bytes. */
#define TEST_STACK_BYTES 8192U

/** @brief Priority of the second task. */
#define TEST_PRIO 1U

/** @brief The longest the case waits for the second task, in ms. */
#define TEST_WAIT_MS 4000U

/** @brief The hold's gain, percent: the LRA's ceiling. */
#define TEST_HOLD_GAIN 80U

/** @brief The text a play during a hold fails with. */
#define TEST_HELD_TEXT "the motor is held on; call kode_haptic_vibrate(false) first"

/** @brief The text a call that never found the motor free fails with. */
#define TEST_BUSY_TEXT "the motor is busy"

/** @brief Most characters of an error a case copies. */
#define TEST_ERROR_COPY 128U

/** @brief Most segments of a built-in. */
#define TEST_BUILTIN_SEGS_MAX 5U

/** @brief One built-in profile as the case expects it. */
typedef struct {
    const char *name;                       /**< The name typed.      */
    uint8_t     n;                          /**< Segments.            */
    uint16_t    seg[TEST_BUILTIN_SEGS_MAX]; /**< Segment lengths, ms. */
    uint8_t     intensity;                  /**< Percent, as declared. */
    uint8_t     gain;                       /**< Percent the motor gets: 80 % of it. */
} expect_t;

/** @brief The six built-ins. */
static const expect_t s_builtin[] = {
    { "click",   1U, { 12U },                       60U, 48U },
    { "tick",    1U, { 6U },                        40U, 32U },
    { "double",  3U, { 12U, 80U, 12U },             60U, 48U },
    { "success", 3U, { 20U, 60U, 40U },             60U, 48U },
    { "warning", 5U, { 40U, 80U, 40U, 80U, 40U },   70U, 56U },
    { "error",   5U, { 60U, 60U, 60U, 60U, 120U },  80U, 64U }
};

/** @brief How many built-ins there are. */
#define BUILTIN_COUNT ((uint32_t)(sizeof(s_builtin) / sizeof(s_builtin[0])))

/** @brief A system that is up, a free motor and a clean clock. */
static void fresh(void)
{
    qf_support_init();
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    fake_time_delay_hook(NULL);
    fake_time_reset();
    motion_support_haptic_up();
}

/** @brief Requests waiting in AO_Haptic's queue. */
static uint32_t queued(void)
{
    return (uint32_t)uxQueueMessagesWaiting(AO_Haptic->eQueue);
}

/** @brief Check the next request is a pattern of @p n segments of @p seg at @p gain. */
static void expect_play(uint8_t n, const uint16_t *seg, uint8_t gain)
{
    const HapticPlayEvt *e = NULL;

    TEST_ASSERT_TRUE(xQueueReceive(AO_Haptic->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_INT((int32_t)HAPTIC_PLAY_REQ_SIG, (int32_t)e->super.sig);
    TEST_ASSERT_FALSE(e->hold);
    TEST_ASSERT_EQUAL_UINT8(n, e->n);
    for ( uint8_t i = 0U; i < n; i++ )
    {
        TEST_ASSERT_EQUAL_UINT16(seg[i], e->seg_ms[i]);
    }
    TEST_ASSERT_EQUAL_UINT8(gain, e->gain_pct);
    QF_gc(&e->super);
}

/** @brief Check the next request is a hold at @p gain. */
static void expect_hold(uint8_t gain)
{
    const HapticPlayEvt *e = NULL;

    TEST_ASSERT_TRUE(xQueueReceive(AO_Haptic->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_INT((int32_t)HAPTIC_PLAY_REQ_SIG, (int32_t)e->super.sig);
    TEST_ASSERT_TRUE(e->hold);
    TEST_ASSERT_EQUAL_UINT8(gain, e->gain_pct);
    QF_gc(&e->super);
}

/** @brief Check the next request is a stop. */
static void expect_stop(void)
{
    const QEvt *e = NULL;

    TEST_ASSERT_TRUE(xQueueReceive(AO_Haptic->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_INT((int32_t)HAPTIC_STOP_REQ_SIG, (int32_t)e->sig);
    QF_gc(e);
}

/* ---- The built-in profiles -------------------------------------------------- */

TEST_CASE("each built-in profile posts its pattern at its intensity", "[kode_haptic]")
{
    fresh();
    for ( uint32_t i = 0U; BUILTIN_COUNT > i; i++ )
    {
        TEST_ASSERT_TRUE_MESSAGE(kode_haptic_play(s_builtin[i].name), s_builtin[i].name);
        TEST_ASSERT_EQUAL_UINT32(1U, queued());
        expect_play(s_builtin[i].n, s_builtin[i].seg, s_builtin[i].gain);
        TEST_ASSERT_EQUAL_STRING("", kode_error());
        motion_support_haptic_up();
    }
}

TEST_CASE("a profile name is matched without regard to case", "[kode_haptic]")
{
    fresh();
    TEST_ASSERT_TRUE(kode_haptic_play("Click"));
    expect_play(s_builtin[0].n, s_builtin[0].seg, s_builtin[0].gain);
    motion_support_haptic_up();
    TEST_ASSERT_TRUE(kode_haptic_play("SUCCESS"));
    expect_play(s_builtin[3].n, s_builtin[3].seg, s_builtin[3].gain);
}

TEST_CASE("an unknown or NULL profile fails and posts nothing", "[kode_haptic]")
{
    fresh();
    TEST_ASSERT_FALSE(kode_haptic_play("buzz"));
    TEST_ASSERT_EQUAL_STRING("unknown vibration 'buzz'", kode_error());
    TEST_ASSERT_FALSE(kode_haptic_play(NULL));
    TEST_ASSERT_EQUAL_STRING("vibration name is NULL", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, queued());
}

/* ---- Hold ------------------------------------------------------------------- */

TEST_CASE("vibrate on holds once, blocks plays, and off frees the motor", "[kode_haptic]")
{
    fresh();

    TEST_ASSERT_TRUE(kode_haptic_vibrate(true));
    TEST_ASSERT_EQUAL_UINT32(1U, queued());
    expect_hold(TEST_HOLD_GAIN);

    TEST_ASSERT_TRUE(kode_haptic_vibrate(true));
    TEST_ASSERT_EQUAL_UINT32(0U, queued());

    TEST_ASSERT_FALSE(kode_haptic_play("click"));
    TEST_ASSERT_EQUAL_STRING(TEST_HELD_TEXT, kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, queued());

    TEST_ASSERT_TRUE(kode_haptic_vibrate(false));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, queued());
    expect_stop();

    motion_support_haptic_up();
    TEST_ASSERT_TRUE(kode_haptic_play("click"));
    TEST_ASSERT_EQUAL_UINT32(1U, queued());
    expect_play(s_builtin[0].n, s_builtin[0].seg, s_builtin[0].gain);
}

TEST_CASE("vibrate off while idle posts a stop and answers true", "[kode_haptic]")
{
    fresh();
    TEST_ASSERT_TRUE(kode_haptic_vibrate(false));
    TEST_ASSERT_EQUAL_UINT32(1U, queued());
    expect_stop();
}

/* ---- Waiting for the motor --------------------------------------------------- */

/** @brief The playing profile ends while the caller sleeps. */
static void profile_ends(void)
{
    motion_support_haptic_up();
}

TEST_CASE("a play behind a playing profile waits its turn and goes through", "[kode_haptic]")
{
    fresh();
    TEST_ASSERT_TRUE(kode_haptic_play("click"));
    expect_play(s_builtin[0].n, s_builtin[0].seg, s_builtin[0].gain);

    fake_time_delay_hook(&profile_ends);
    TEST_ASSERT_TRUE(kode_haptic_play("tick"));
    fake_time_delay_hook(NULL);

    TEST_ASSERT_EQUAL_UINT32(1U, queued());
    expect_play(s_builtin[1].n, s_builtin[1].seg, s_builtin[1].gain);
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("a vibrate on behind a playing profile waits its turn and holds", "[kode_haptic]")
{
    fresh();
    TEST_ASSERT_TRUE(kode_haptic_play("click"));
    expect_play(s_builtin[0].n, s_builtin[0].seg, s_builtin[0].gain);

    fake_time_delay_hook(&profile_ends);
    TEST_ASSERT_TRUE(kode_haptic_vibrate(true));
    fake_time_delay_hook(NULL);

    TEST_ASSERT_EQUAL_UINT32(1U, queued());
    expect_hold(TEST_HOLD_GAIN);
    TEST_ASSERT_FALSE(kode_haptic_play("click"));
    TEST_ASSERT_EQUAL_STRING(TEST_HELD_TEXT, kode_error());
}

TEST_CASE("a play behind a profile that never ends gives up after the bound", "[kode_haptic]")
{
    fresh();
    TEST_ASSERT_TRUE(kode_haptic_play("click"));
    expect_play(s_builtin[0].n, s_builtin[0].seg, s_builtin[0].gain);

    const uint32_t t0 = kode_sys_time_ms();

    TEST_ASSERT_FALSE(kode_haptic_play("tick"));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_HAPTIC_WAIT_MS, kode_sys_time_ms() - t0);
    TEST_ASSERT_EQUAL_STRING(TEST_BUSY_TEXT, kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, queued());
}

TEST_CASE("a vibrate on behind a profile that never ends gives up and holds nothing",
          "[kode_haptic]")
{
    fresh();
    TEST_ASSERT_TRUE(kode_haptic_play("click"));
    expect_play(s_builtin[0].n, s_builtin[0].seg, s_builtin[0].gain);

    TEST_ASSERT_FALSE(kode_haptic_vibrate(true));
    TEST_ASSERT_EQUAL_STRING(TEST_BUSY_TEXT, kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, queued());

    motion_support_haptic_up();
    TEST_ASSERT_TRUE(kode_haptic_play("tick"));
    expect_play(s_builtin[1].n, s_builtin[1].seg, s_builtin[1].gain);
}

/* ---- A hold or a profile this layer did not start ---------------------------- */

/** @brief The gain of a hold a sys caller starts. */
#define TEST_SYS_HOLD_GAIN 40U

TEST_CASE("a hold started by a sys caller is answered as a hold at once", "[kode_haptic]")
{
    fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_haptic_hold(TEST_SYS_HOLD_GAIN));
    expect_hold(TEST_SYS_HOLD_GAIN);

    const uint32_t t0 = kode_sys_time_ms();

    TEST_ASSERT_TRUE(kode_haptic_vibrate(true));
    TEST_ASSERT_EQUAL_UINT32(0U, queued());

    TEST_ASSERT_FALSE(kode_haptic_play("click"));
    TEST_ASSERT_EQUAL_STRING(TEST_HELD_TEXT, kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, queued());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, kode_sys_time_ms() - t0, "a call waited for a hold");

    TEST_ASSERT_TRUE(kode_haptic_vibrate(false));
    TEST_ASSERT_EQUAL_UINT32(1U, queued());
    expect_stop();
}

TEST_CASE("a hold that ended another way is not a hold: vibrate on posts a new one", "[kode_haptic]")
{
    fresh();
    TEST_ASSERT_TRUE(kode_haptic_vibrate(true));
    expect_hold(TEST_HOLD_GAIN);

    /* haptic=0, an absent board, a stalled FIFO or a sys stop: the AO gave the motor back. */
    motion_support_haptic_up();
    TEST_ASSERT_TRUE(kode_haptic_play("click"));
    expect_play(s_builtin[0].n, s_builtin[0].seg, s_builtin[0].gain);

    motion_support_haptic_up();
    TEST_ASSERT_TRUE(kode_haptic_vibrate(true));
    TEST_ASSERT_EQUAL_UINT32(1U, queued());
    expect_hold(TEST_HOLD_GAIN);
}

TEST_CASE("vibrate off while a profile plays queues a stop", "[kode_haptic]")
{
    fresh();
    TEST_ASSERT_TRUE(kode_haptic_play("click"));
    expect_play(s_builtin[0].n, s_builtin[0].seg, s_builtin[0].gain);

    TEST_ASSERT_TRUE(kode_haptic_vibrate(false));
    TEST_ASSERT_EQUAL_UINT32(1U, queued());
    expect_stop();
}

/* ---- Another task ----------------------------------------------------------- */

/** @brief What the second task leaves for the case. */
typedef struct {
    SemaphoreHandle_t done;                    /**< Given when the task has noted its result. */
    bool              played;                  /**< What kode_haptic_play() answered.         */
    char              error[TEST_ERROR_COPY];  /**< Its error text.                           */
} other_t;

/** @brief A task that plays "click" and notes what came of it. */
static void other_plays(void *arg)
{
    other_t *const o = (other_t *)arg;

    o->played = kode_haptic_play("click");
    (void)strncpy(o->error, kode_error(), sizeof(o->error) - 1U);
    o->error[sizeof(o->error) - 1U] = '\0';
    (void)xSemaphoreGive(o->done);
}

TEST_CASE("a second task playing while the first holds gets the hold error", "[kode_haptic]")
{
    other_t o;

    fresh();
    (void)memset(&o, 0, sizeof(o));
    o.played = true;
    o.done   = xSemaphoreCreateBinary();

    TEST_ASSERT_TRUE(kode_haptic_vibrate(true));
    expect_hold(TEST_HOLD_GAIN);

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&other_plays, "t_haptic",
                                                    TEST_STACK_BYTES, TEST_PRIO, &o));
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(o.done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    vSemaphoreDelete(o.done);

    TEST_ASSERT_FALSE(o.played);
    TEST_ASSERT_EQUAL_STRING(TEST_HELD_TEXT, o.error);
    TEST_ASSERT_EQUAL_UINT32(0U, queued());

    TEST_ASSERT_TRUE(kode_haptic_vibrate(false));
    expect_stop();
}

/* ---- The app's own profiles -------------------------------------------------- */

/** @brief Fail with @p reason: the call answered false, the error says @p reason, nothing posted. */
static void expect_refused(bool answer, const char *reason)
{
    TEST_ASSERT_FALSE(answer);
    TEST_ASSERT_EQUAL_STRING(reason, kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, queued());
}

TEST_CASE("an added profile plays, is copied, and the same name replaces it", "[kode_haptic]")
{
    uint16_t       seg[] = { 10U, 20U, 30U };
    static const uint16_t first[]  = { 10U, 20U, 30U };
    static const uint16_t second[] = { 5U };

    fresh();
    TEST_ASSERT_TRUE(kode_haptic_add_profile("pulse", seg, 3U, 55U));
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    seg[0] = 99U;
    TEST_ASSERT_TRUE(kode_haptic_play("PULSE"));
    expect_play(3U, first, 44U);
    motion_support_haptic_up();

    TEST_ASSERT_TRUE(kode_haptic_add_profile("Pulse", second, 1U, 20U));
    TEST_ASSERT_TRUE(kode_haptic_play("pulse"));
    expect_play(1U, second, 16U);
}

TEST_CASE("add_profile refuses a bad name or pattern and says why", "[kode_haptic]")
{
    static const uint16_t ok[]      = { 10U, 20U };
    static const uint16_t zero[]    = { 10U, 0U };
    uint16_t              many[KODE_HAPTIC_PATTERN_MAX + 1U];

    fresh();
    for ( uint32_t i = 0U; (uint32_t)(KODE_HAPTIC_PATTERN_MAX + 1U) > i; i++ )
    {
        many[i] = 10U;
    }

    expect_refused(kode_haptic_add_profile(NULL, ok, 2U, 50U), "profile name is NULL");
    expect_refused(kode_haptic_add_profile("", ok, 2U, 50U), "profile name is empty");
    expect_refused(kode_haptic_add_profile("0123456789abcdef", ok, 2U, 50U),
                   "profile name '0123456789abcdef' is too long (15 characters at most)");
    expect_refused(kode_haptic_add_profile("Click", ok, 2U, 50U),
                   "'Click' is a built-in vibration");
    expect_refused(kode_haptic_add_profile("bad1", NULL, 2U, 50U), "pattern is NULL");
    expect_refused(kode_haptic_add_profile("bad2", ok, 0U, 50U), "pattern has no segment");
    expect_refused(kode_haptic_add_profile("bad3", many, (uint32_t)(KODE_HAPTIC_PATTERN_MAX + 1U), 50U),
                   "pattern has more than 16 segments");
    expect_refused(kode_haptic_add_profile("bad4", zero, 2U, 50U), "a segment of 0 ms");
    expect_refused(kode_haptic_add_profile("bad5", ok, 2U, 101U), "intensity is over 100");

    /* The longest name that fits is taken, and none of the refused is there. */
    TEST_ASSERT_TRUE(kode_haptic_add_profile("0123456789abcde", ok, 2U, 50U));
    TEST_ASSERT_FALSE(kode_haptic_play("bad1"));
    TEST_ASSERT_EQUAL_STRING("unknown vibration 'bad1'", kode_error());
    TEST_ASSERT_FALSE(kode_haptic_play("bad5"));
    TEST_ASSERT_EQUAL_UINT32(0U, queued());
}

/* ---- Intensity onto the motor's range ---------------------------------------- */

/** @brief One intensity and the gain the motor is asked for. */
typedef struct {
    uint8_t intensity;  /**< Percent, as the app declares it. */
    uint8_t gain;       /**< Percent the extended layer gets.      */
} drive_t;

TEST_CASE("intensity 0 to 100 spreads over the motor's 0 to 80", "[kode_haptic]")
{
    static const uint16_t ok[] = { 10U };
    static const drive_t  table[] = {
        { 100U, 80U }, { 50U, 40U }, { 25U, 20U }, { 1U, 0U }, { 0U, 0U }
    };

    fresh();
    for ( uint32_t i = 0U; (uint32_t)(sizeof(table) / sizeof(table[0])) > i; i++ )
    {
        TEST_ASSERT_TRUE(kode_haptic_add_profile("pulse", ok, 1U, table[i].intensity));
        TEST_ASSERT_TRUE(kode_haptic_play("pulse"));
        expect_play(1U, ok, table[i].gain);
        motion_support_haptic_up();
    }
}

/** @brief Prefix of the names that fill the table. */
#define TEST_FILL_PREFIX "fill"

/** @brief Bytes of a fill name: the prefix, one digit and the NUL. */
#define TEST_FILL_NAME_BYTES (sizeof(TEST_FILL_PREFIX) + 1U)

/** @brief The longest name there is, 15 characters. */
#define TEST_LONG_NAME "0123456789abcde"

/** @brief The names the cases above leave in the table. */
#define TEST_LEFTOVERS 2U

/* This case leaves the app's table full: put it after every other that adds a profile. */
TEST_CASE("exactly KODE_HAPTIC_PROFILES_MAX profiles fit, the next is refused, a name there still replaces",
          "[kode_haptic]")
{
    static const uint16_t ok[]    = { 10U, 20U };
    static const uint16_t other[] = { 7U };
    char                  name[TEST_FILL_NAME_BYTES];
    uint32_t              taken = 0U;
    bool                  added = true;

    fresh();
    /* The two names earlier cases leave: adding them again replaces or fills, whatever ran first. */
    TEST_ASSERT_TRUE(kode_haptic_add_profile("pulse", ok, 2U, 50U));
    TEST_ASSERT_TRUE(kode_haptic_add_profile(TEST_LONG_NAME, ok, 2U, 50U));

    (void)memcpy(name, TEST_FILL_PREFIX, sizeof(TEST_FILL_PREFIX));
    name[sizeof(TEST_FILL_PREFIX)] = '\0';
    for ( uint32_t i = 0U; (KODE_HAPTIC_PROFILES_MAX > i) && added; i++ )
    {
        name[sizeof(TEST_FILL_PREFIX) - 1U] = (char)('0' + (char)i);
        added = kode_haptic_add_profile(name, ok, 2U, 50U);
        if ( added )
        {
            taken++;
        }
    }
    TEST_ASSERT_FALSE_MESSAGE(added, "the table never filled");
    TEST_ASSERT_EQUAL_UINT32(KODE_HAPTIC_PROFILES_MAX - TEST_LEFTOVERS, taken);
    TEST_ASSERT_EQUAL_STRING("no room for another profile (8 at most)", kode_error());

    TEST_ASSERT_TRUE(kode_haptic_add_profile("fill0", other, 1U, 33U));
    TEST_ASSERT_TRUE(kode_haptic_play("fill0"));
    expect_play(1U, other, 26U);
}
