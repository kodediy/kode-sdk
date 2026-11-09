/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_screen.c
 * @brief The standard layer's screen: kode_screen_new() and its text calls, down to the AO.
 */
#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "fakes.h"
#include "khal_system.h"
#include "khal_time.h"
#include "kode.h"
#include "kode_entry.h"
#include "kaos.h"
#include "kdrv_qmc5883p.h"
#include "kode_screen_priv.h"
#include "kode_standard_ao.h"
#include "kode_sys.h"
#include "kode_sys_priv.h"
#include "kqp.h"
#include "qf_support.h"

/** @brief Stack of a test task, in bytes. */
#define TEST_STACK_BYTES 8192U

/** @brief Level of a test task: above the main task, below the Active Object. */
#define TEST_PRIO 2U

/** @brief The longest a case waits for a task or the fake, in ms. */
#define TEST_WAIT_MS 4000U

/** @brief Requests the fake can count before the semaphore is full. */
#define SEEN_MAX 4096U

/** @brief Credits one sleep of the caller hands the gated fake. */
#define CREDITS_PER_SLEEP 4U

/** @brief set_text calls of the long loop. */
#define LOOP_CALLS 2000U

/** @brief Bytes of a text longer than a widget takes. */
#define LONG_TEXT_BYTES (KODE_WIDGET_TEXT_MAX + 8U)

/** @brief Bytes of an error a case words itself. */
#define WANT_MAX 96U

/** @brief Requests that fill the queue while the fake is held, with room to spare. */
#define FILL_CALLS 64U

/* ---- The fake screen ------------------------------------------------------- */

static SemaphoreHandle_t s_seen;          /**< Given once per request handled.    */
static SemaphoreHandle_t s_credit;        /**< Taken once per request while gated. */
static atomic_bool       s_gated;         /**< The fake waits for a credit.       */
static _Atomic uint32_t  s_attach_n;      /**< attach() calls since boot.         */
static _Atomic uint32_t  s_req_n;         /**< Requests handled since boot.       */
static KodeScreenReqEvt  s_last;          /**< A copy of the last request.        */
static _Atomic uint32_t  s_gen;           /**< The last screen request's generation. */
static kode_canvasbuf_t *s_canvas;        /**< The pixels of the last canvas request. */
static atomic_bool       s_retired;       /**< A screen request has retired its canvases. */
static atomic_bool       s_drop_text;     /**< The fake drops the texts it is handed.    */

/** @brief Texts of the changes (KODE_SCREEN_OP_SET) the fake saw, in order, and how long each may be. */
#define SET_LOG_MAX  16U
#define SET_LOG_TEXT 16U
static char              s_set_log[SET_LOG_MAX][SET_LOG_TEXT];  /**< The first characters of each. */
static _Atomic uint32_t  s_set_n;                               /**< How many there were.          */

/** @brief The first characters of the last text each kind of request carried. */
#define OP_TEXT_MAX  40U
#define OP_KINDS     16U
static char              s_op_text[OP_KINDS][OP_TEXT_MAX];

static atomic_bool       s_prepare_ok = true;   /**< prepare() has memory to give.     */

/** @brief The fake's prepare: the screen's memory, or none. */
static bool fake_prepare(void)
{
    return atomic_load(&s_prepare_ok);
}

/** @brief The fake's attach: count it. */
static void fake_attach(QActive *ao)
{
    (void)ao;
    (void)atomic_fetch_add(&s_attach_n, 1U);
}

/**
 * @brief What the fake draws for a text: one pixel of its colour at its x and y.
 * @param r The request.
 */
static void text_paint(const KodeScreenReqEvt *r)
{
    static const uint8_t full = 255U;
    kode_canvasbuf_t *const b = kode_canvasbuf_get(r->widget >> KODE_WIDGET_PLACE_BITS,
                                                    (r->widget & KODE_WIDGET_PLACE_MASK) - 1U);

    if ( NULL != b )
    {
        (void)kode_canvasbuf_blend_mask(b, r->num[0], r->num[1], &full, 1U, 1U, 1U,
                                        (kode_color_t)((uint32_t)r->num[2] & KODE_TEXT_RGB_MASK));
    }
}

/**
 * @brief The fake's event: copy a request, after a credit when gated; and
 *        take a touch for a tap on the button at place x of the screen it last saw,
 *        as the real table does for a click.
 */
static bool fake_event(QEvt const *e)
{
    bool handled = false;

    if ( (QSignal)KODE_SCREEN_REQ_SIG == e->sig )
    {
        if ( atomic_load(&s_gated) )
        {
            (void)xSemaphoreTake(s_credit, pdMS_TO_TICKS(TEST_WAIT_MS));
        }
        s_last = *(const KodeScreenReqEvt *)e;
        if ( OP_KINDS > s_last.op )
        {
            (void)snprintf(s_op_text[s_last.op], OP_TEXT_MAX, "%.*s",
                           (int)(OP_TEXT_MAX - 1U), s_last.text);
        }
        if ( (uint8_t)KODE_SCREEN_OP_SET == s_last.op )
        {
            const uint32_t n = atomic_load(&s_set_n);

            if ( SET_LOG_MAX > n )
            {
                (void)snprintf(s_set_log[n], SET_LOG_TEXT, "%.*s",
                               (int)(SET_LOG_TEXT - 1U), s_last.text);
            }
            atomic_store(&s_set_n, n + 1U);
        }
        if ( (uint8_t)KODE_SCREEN_OP_NEW == s_last.op )
        {
            const uint32_t left = atomic_load(&s_gen);

            atomic_store(&s_gen, (uint32_t)s_last.num[0]);
            kode_canvasbuf_retire(left);
            atomic_store(&s_retired, true);
        }
        else if ( (uint8_t)KODE_SCREEN_OP_DRAW_TEXT == s_last.op )
        {
            if ( !atomic_load(&s_drop_text) )
            {
                text_paint(&s_last);
            }
        }
        else if ( ((uint8_t)KODE_SCREEN_OP_CANVAS == s_last.op)
                 || ((uint8_t)KODE_SCREEN_OP_IMAGE == s_last.op) )
        {
            s_canvas = kode_canvasbuf_get((uint32_t)s_last.num[0], s_last.widget);
        }
        else
        {
            /* nothing more for the fake to do */
        }
        (void)atomic_fetch_add(&s_req_n, 1U);
        (void)xSemaphoreGive(s_seen);
        if ( (uint8_t)KODE_SCREEN_OP_DRAW_TEXT == s_last.op )
        {
            /* Last, as the real table does: the caller goes on from here. */
            kode_widget_text_done((uint32_t)s_last.num[3], !atomic_load(&s_drop_text));
        }
        handled = true;
    }
    else if ( (QSignal)TOUCH_EVT_SIG == e->sig )
    {
        kode_widget_tapped(atomic_load(&s_gen), (uint32_t)((const TouchEvt *)e)->x);
        handled = true;
    }
    else
    {
        /* not the screen's */
    }
    return handled;
}

/** The table kode_screen.c gives on the board; this file's fake here. */
const kode_screen_ops_t kode_screen_ops = { &fake_prepare, &fake_attach, &fake_event };

/** @brief Hand the gated fake a few requests' worth, and do it again at the next sleep. */
static void credit_hook(void)
{
    for ( uint32_t i = 0U; CREDITS_PER_SLEEP > i; i++ )
    {
        (void)xSemaphoreGive(s_credit);
    }
    fake_time_delay_hook(&credit_hook);
}

/** @brief Wait until the fake has handled @p n more requests. */
static bool seen_wait(uint32_t n)
{
    bool ok = true;

    for ( uint32_t i = 0U; (i < n) && ok; i++ )
    {
        ok = (xSemaphoreTake(s_seen, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    }
    return ok;
}

/** @brief Open the gate, let a held request go and wait for @p n requests in all. */
static bool gate_open(uint32_t n)
{
    atomic_store(&s_gated, false);
    (void)xSemaphoreGive(s_credit);
    return seen_wait(n);
}

/** @brief Make kode_init() a no-op, as it is once the system is up. */
static void system_is_up(void)
{
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
}

/** @brief Real locks, a fresh fake, and the layer up as far as a case needs. */
static void screen_case(void)
{
    qf_support_init();
    system_is_up();
    fake_sync_real(true);
    if ( NULL == s_seen )
    {
        s_seen   = xSemaphoreCreateCounting(SEEN_MAX, 0U);
        s_credit = xSemaphoreCreateCounting(SEEN_MAX, 0U);
    }
    atomic_store(&s_gated, false);
    atomic_store(&s_drop_text, false);
    atomic_store(&s_set_n, 0U);
    while ( xSemaphoreTake(s_seen, 0U) == pdTRUE )
    {
    }
    while ( xSemaphoreTake(s_credit, 0U) == pdTRUE )
    {
    }
}

/* ---- Two tasks at once -------------------------------------------------------- */

static SemaphoreHandle_t s_done[2];        /**< Given by each task on return. */
static volatile kode_widget_t s_got[2];       /**< What each task's call returned. */

/** @brief One of two tasks adding a widget. */
static void add_task(void *arg)
{
    const uint32_t i = (uint32_t)(uintptr_t)arg;

    s_got[i] = kode_screen_add_text("hello");
    (void)xSemaphoreGive(s_done[i]);
}

/* First in the file on purpose, and no other file draws: the attach it counts
 * is the binary's only one, so it starts from none. */
TEST_CASE("two tasks drawing at once attach the screen once", "[kode_screen]")
{
    screen_case();
    TEST_ASSERT_EQUAL_UINT32(0U, atomic_load(&s_attach_n));

    /* No memory for the screen: the first call fails whole, nothing attaches,
     * and the next call tries again. */
    atomic_store(&s_prepare_ok, false);
    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_text("hello"));
    TEST_ASSERT_EQUAL_STRING("out of memory", kode_error());
    TEST_ASSERT_FALSE(kode_screen_new("Title"));
    TEST_ASSERT_EQUAL_STRING("out of memory", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_canvas(10U, 10U));
    TEST_ASSERT_EQUAL_STRING("out of memory", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, atomic_load(&s_attach_n));
    atomic_store(&s_prepare_ok, true);

    kode_status_t started = KODE_OK;

    for ( uint32_t i = 0U; (2U > i) && (KODE_OK == started); ++i )
    {
        s_done[i] = xSemaphoreCreateBinary();
        s_got[i]  = 0U;
        started = khal_task_start(&add_task, "t_draw", TEST_STACK_BYTES,
                                  TEST_PRIO, (void *)(uintptr_t)i);
    }
    TEST_ASSERT_EQUAL(KODE_OK, started);
    for ( uint32_t i = 0U; 2U > i; ++i )
    {
        TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_done[i], pdMS_TO_TICKS(TEST_WAIT_MS)));
    }
    TEST_ASSERT_TRUE(seen_wait(2U));
    TEST_ASSERT_EQUAL_UINT32(1U, atomic_load(&s_attach_n));
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, s_got[0]);
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, s_got[1]);
    TEST_ASSERT_NOT_EQUAL_UINT32(s_got[0], s_got[1]);
}

/* ---- What is posted ------------------------------------------------------------ */

TEST_CASE("a screen, a text and a change each post a copy", "[kode_screen]")
{
    screen_case();

    char text[KODE_WIDGET_TEXT_MAX];

    (void)snprintf(text, sizeof(text), "%s", "Hello");
    TEST_ASSERT_TRUE(kode_screen_new(text));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_EQUAL_UINT8(KODE_SCREEN_OP_NEW, s_last.op);
    TEST_ASSERT_EQUAL_STRING("Hello", s_last.text);
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    /* The fake holds the request until the caller's text has changed: what it
     * reads is the copy made before the call returned. */
    atomic_store(&s_gated, true);
    (void)snprintf(text, sizeof(text), "%s", "first line");
    const kode_widget_t p = kode_screen_add_text(text);
    (void)snprintf(text, sizeof(text), "%s", "changed");
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, p);
    TEST_ASSERT_TRUE(gate_open(1U));
    TEST_ASSERT_EQUAL_UINT8(KODE_SCREEN_OP_TEXT, s_last.op);
    TEST_ASSERT_EQUAL_UINT32(0U, s_last.widget);
    TEST_ASSERT_EQUAL_STRING("first line", s_last.text);

    const kode_widget_t q = kode_screen_add_text("second line");
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_last.widget);

    TEST_ASSERT_TRUE(kode_widget_set_text(p, "count %" PRIu32, (uint32_t)7U));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_EQUAL_UINT8(KODE_SCREEN_OP_SET, s_last.op);
    TEST_ASSERT_EQUAL_UINT32(0U, s_last.widget);
    TEST_ASSERT_EQUAL_STRING("count 7", s_last.text);

    TEST_ASSERT_TRUE(kode_widget_set_text(q, "%s", "two"));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_last.widget);
    TEST_ASSERT_EQUAL_STRING("two", s_last.text);
}

TEST_CASE("a screen with no title posts an empty one", "[kode_screen]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new(NULL));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_EQUAL_UINT8(KODE_SCREEN_OP_NEW, s_last.op);
    TEST_ASSERT_EQUAL_STRING("", s_last.text);
}

/* ---- What is refused ------------------------------------------------------------ */

TEST_CASE("text over the limit is refused, naming the limit", "[kode_screen]")
{
    screen_case();

    char long_text[LONG_TEXT_BYTES];

    (void)memset(long_text, 'a', sizeof(long_text) - 1U);
    long_text[sizeof(long_text) - 1U] = '\0';
    TEST_ASSERT_TRUE(kode_screen_new("Limits"));
    TEST_ASSERT_TRUE(seen_wait(1U));
    const uint32_t before = atomic_load(&s_req_n);

    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_text(long_text));
    TEST_ASSERT_EQUAL_STRING("text is too long (127 bytes at most)", kode_error());
    TEST_ASSERT_FALSE(kode_screen_new(long_text));
    TEST_ASSERT_EQUAL_STRING("title is too long (127 bytes at most)", kode_error());

    const kode_widget_t p = kode_screen_add_text("short");
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_FALSE(kode_widget_set_text(p, "%s", long_text));
    TEST_ASSERT_EQUAL_STRING("text is too long (127 bytes at most)", kode_error());

    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_text(NULL));
    TEST_ASSERT_EQUAL_STRING("text is NULL", kode_error());
    TEST_ASSERT_FALSE(kode_widget_set_text(p, NULL));
    TEST_ASSERT_EQUAL_STRING("format is NULL", kode_error());

    /* Only "short" went out. */
    TEST_ASSERT_EQUAL_UINT32(before + 1U, atomic_load(&s_req_n));
}

TEST_CASE("a widget of an older screen is no such widget", "[kode_screen]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("Old"));
    const kode_widget_t old = kode_screen_add_text("old");
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, old);
    TEST_ASSERT_TRUE(kode_screen_new("New"));
    const kode_widget_t now = kode_screen_add_text("new");
    TEST_ASSERT_TRUE(seen_wait(4U));
    const uint32_t before = atomic_load(&s_req_n);

    TEST_ASSERT_FALSE(kode_widget_set_text(old, "x"));
    TEST_ASSERT_EQUAL_STRING("no such widget", kode_error());
    TEST_ASSERT_FALSE(kode_widget_set_text(0U, "x"));
    TEST_ASSERT_EQUAL_STRING("no such widget", kode_error());
    TEST_ASSERT_FALSE(kode_widget_set_text(now + 1U, "x"));
    TEST_ASSERT_EQUAL_STRING("no such widget", kode_error());
    TEST_ASSERT_EQUAL_UINT32(before, atomic_load(&s_req_n));

    TEST_ASSERT_TRUE(kode_widget_set_text(now, "y"));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_EQUAL_STRING("y", s_last.text);
}

TEST_CASE("a screen holds KODE_WIDGETS_MAX widgets and refuses the next", "[kode_screen]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("Full"));
    for ( uint32_t i = 0U; KODE_WIDGETS_MAX > i; i++ )
    {
        TEST_ASSERT_NOT_EQUAL_UINT32(0U, kode_screen_add_text("widget"));
    }
    TEST_ASSERT_TRUE(seen_wait(KODE_WIDGETS_MAX + 1U));
    TEST_ASSERT_EQUAL_UINT32(KODE_WIDGETS_MAX - 1U, s_last.widget);

    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_text("one more"));
    TEST_ASSERT_EQUAL_STRING("a screen holds at most 32 widgets", kode_error());
}

/* ---- A full queue ------------------------------------------------------------------ */

TEST_CASE("a full queue makes the call wait its turn, then it goes", "[kode_screen]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("Queue"));
    const kode_widget_t p = kode_screen_add_text("0");
    TEST_ASSERT_TRUE(seen_wait(2U));

    /* Held: the queue fills to its margin, then a call sleeps; its sleep lets
     * the fake take a few, and the call goes on the next try. */
    atomic_store(&s_gated, true);
    fake_time_reset();
    fake_time_delay_hook(&credit_hook);
    for ( uint32_t i = 0U; FILL_CALLS > i; i++ )
    {
        TEST_ASSERT_TRUE(kode_widget_set_text(p, "%" PRIu32, i));
    }
    fake_time_delay_hook(NULL);
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, khal_time_ms());   /* somebody slept */
    TEST_ASSERT_TRUE(gate_open(FILL_CALLS));
    TEST_ASSERT_EQUAL_STRING("63", s_last.text);
}

TEST_CASE("a queue that stays full fails the call: the screen is busy", "[kode_screen]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("Busy"));
    const kode_widget_t p = kode_screen_add_text("0");
    TEST_ASSERT_TRUE(seen_wait(2U));

    atomic_store(&s_gated, true);
    fake_time_reset();
    uint32_t taken = 0U;
    bool     ok    = true;

    while ( ok && (FILL_CALLS > taken) )
    {
        ok = kode_widget_set_text(p, "%" PRIu32, taken);
        if ( ok )
        {
            taken++;
        }
    }
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_STRING("the screen is busy", kode_error());
    TEST_ASSERT_TRUE(khal_time_ms() >= KODE_SCREEN_BUSY_MS);
    TEST_ASSERT_TRUE(gate_open(taken));
}

TEST_CASE("a queue full of screen requests still takes what is published", "[kode_screen]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("Publish"));
    const kode_widget_t p = kode_screen_add_text("0");
    TEST_ASSERT_TRUE(seen_wait(2U));

    /* Held, until a request is refused: the queue is as full as requests take it. */
    atomic_store(&s_gated, true);
    fake_time_reset();
    uint32_t taken = 0U;

    while ( (FILL_CALLS > taken) && kode_widget_set_text(p, "%" PRIu32, taken) )
    {
        taken++;
    }
    TEST_ASSERT_EQUAL_UINT32(KODE_SCREEN_POST_MARGIN,
                             (uint32_t)uxQueueSpacesAvailable(AO_KodeStandard->eQueue));

    /* What a publish does to a subscriber: a post with no margin, which
     * asserts on a full queue. Posted, not published, so no other subscriber
     * in this binary hears it; one static touch, so no pool is involved. */
    static const TouchEvt touch = { QEVT_INITIALIZER(TOUCH_EVT_SIG), 0, 0, false, 0U, false };

    for ( uint32_t i = 0U; KODE_SCREEN_POST_MARGIN > i; i++ )
    {
        QACTIVE_POST(AO_KodeStandard, &touch.super, NULL);
    }
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueSpacesAvailable(AO_KodeStandard->eQueue));

    TEST_ASSERT_TRUE(gate_open(taken));
    /* Behind the touches: once it is seen, they are gone too. */
    TEST_ASSERT_TRUE(kode_widget_set_text(p, "%s", "after"));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_EQUAL_STRING("after", s_last.text);
}

TEST_CASE("two thousand changes in a loop never assert", "[kode_screen]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("Loop"));
    const kode_widget_t p = kode_screen_add_text("0");
    TEST_ASSERT_TRUE(seen_wait(2U));
    const uint32_t before = atomic_load(&s_req_n);

    atomic_store(&s_gated, true);
    fake_time_reset();
    fake_time_delay_hook(&credit_hook);
    bool every = true;

    for ( uint32_t i = 0U; LOOP_CALLS > i; i++ )
    {
        every = kode_widget_set_text(p, "value %" PRIu32, i) && every;
    }
    fake_time_delay_hook(NULL);
    TEST_ASSERT_TRUE(every);
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, khal_time_ms());   /* the queue filled */
    TEST_ASSERT_TRUE(gate_open(LOOP_CALLS));
    TEST_ASSERT_EQUAL_UINT32(before + LOOP_CALLS, atomic_load(&s_req_n));
    TEST_ASSERT_EQUAL_STRING("value 1999", s_last.text);
    TEST_ASSERT_EQUAL_UINT32(1U, atomic_load(&s_attach_n));
}

/* ---- A value and a button --------------------------------------------------------- */

/** @brief Deliver a click on the button at @p place through the table's event(). */
static void click(uint32_t place)
{
    TouchEvt t = { QEVT_INITIALIZER(TOUCH_EVT_SIG), 0, 0, false, 0U, false };

    t.x = (int16_t)place;
    TEST_ASSERT_TRUE(kode_screen_ops.event(&t.super));
}

TEST_CASE("a value posts its number and its unit, and a change sets the number", "[kode_screen]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("Climate"));
    TEST_ASSERT_TRUE(seen_wait(1U));

    const kode_widget_t p = kode_screen_add_value("23.5", "C");
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, p);
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_EQUAL_UINT8(KODE_SCREEN_OP_VALUE, s_last.op);
    TEST_ASSERT_EQUAL_UINT32(0U, s_last.widget);
    TEST_ASSERT_EQUAL_STRING("23.5", s_last.text);
    TEST_ASSERT_EQUAL_STRING("C", &s_last.text[strlen(s_last.text) + 1U]);

    TEST_ASSERT_TRUE(kode_widget_set_text(p, "%.1f", 24.0));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_EQUAL_UINT8(KODE_SCREEN_OP_SET, s_last.op);
    TEST_ASSERT_EQUAL_STRING("24.0", s_last.text);

    /* No unit is an empty one. */
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, kode_screen_add_value("7", NULL));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_last.widget);
    TEST_ASSERT_EQUAL_STRING("", &s_last.text[strlen(s_last.text) + 1U]);
}

TEST_CASE("a value refuses what it cannot show", "[kode_screen]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("Limits"));
    TEST_ASSERT_TRUE(seen_wait(1U));
    const uint32_t before = atomic_load(&s_req_n);

    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_value(NULL, "C"));
    TEST_ASSERT_EQUAL_STRING("number is NULL", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_value("12345678901", "C"));
    TEST_ASSERT_EQUAL_STRING("number is too long (9 bytes at most)", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_value("1", "12345678901234567"));
    TEST_ASSERT_EQUAL_STRING("unit is too long (15 bytes at most)", kode_error());

    const kode_widget_t p = kode_screen_add_value("123456789", "123456789012345");
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, p);
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_FALSE(kode_widget_set_text(p, "%s", "1234567890"));
    TEST_ASSERT_EQUAL_STRING("number is too long (9 bytes at most)", kode_error());
    TEST_ASSERT_EQUAL_UINT32(before + 1U, atomic_load(&s_req_n));
}

TEST_CASE("a button posts its label, and its label cannot change", "[kode_screen]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("Controls"));
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, kode_screen_add_text("hello"));
    const kode_widget_t b = kode_screen_add_button("Start");
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, b);
    TEST_ASSERT_TRUE(seen_wait(3U));
    TEST_ASSERT_EQUAL_UINT8(KODE_SCREEN_OP_BUTTON, s_last.op);
    TEST_ASSERT_EQUAL_UINT32(1U, s_last.widget);
    TEST_ASSERT_EQUAL_STRING("Start", s_last.text);

    const uint32_t before = atomic_load(&s_req_n);

    TEST_ASSERT_FALSE(kode_widget_set_text(b, "%s", "Stop"));
    TEST_ASSERT_EQUAL_STRING("a button's label cannot change", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_button(NULL));
    TEST_ASSERT_EQUAL_STRING("label is NULL", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_button("a label of thirty-two bytes, or so"));
    TEST_ASSERT_EQUAL_STRING("label is too long (31 bytes at most)", kode_error());
    TEST_ASSERT_EQUAL_UINT32(before, atomic_load(&s_req_n));
}

TEST_CASE("a button, a text and a value take a colour, a canvas and an old widget do not", "[kode_screen]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("Paint"));
    const kode_widget_t t = kode_screen_add_text("hello");
    const kode_widget_t v = kode_screen_add_value("21", "C");
    const kode_widget_t b = kode_screen_add_button("Colour");
    const kode_canvas_t c = kode_screen_add_canvas(10U, 8U);
    TEST_ASSERT_TRUE(seen_wait(5U));

    TEST_ASSERT_TRUE(kode_widget_set_color(b, kode_rgb(0x12U, 0x34U, 0x56U)));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_EQUAL_UINT8(KODE_SCREEN_OP_COLOR, s_last.op);
    TEST_ASSERT_EQUAL_UINT32(2U, s_last.widget);
    TEST_ASSERT_EQUAL_HEX32(0x123456U, (uint32_t)s_last.num[0]);

    /* The top byte is not the colour's. */
    TEST_ASSERT_TRUE(kode_widget_set_color(t, (kode_color_t)0xAB00FF00U));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_EQUAL_UINT32(0U, s_last.widget);
    TEST_ASSERT_EQUAL_HEX32(0x00FF00U, (uint32_t)s_last.num[0]);

    TEST_ASSERT_TRUE(kode_widget_set_color(v, KODE_BLUE));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_last.widget);

    const uint32_t before = atomic_load(&s_req_n);

    TEST_ASSERT_FALSE(kode_widget_set_color(c, KODE_BLUE));
    TEST_ASSERT_EQUAL_STRING("a canvas has no colour of its own", kode_error());
    TEST_ASSERT_FALSE(kode_widget_set_color(0U, KODE_BLUE));
    TEST_ASSERT_EQUAL_STRING("no such widget", kode_error());
    TEST_ASSERT_EQUAL_UINT32(before, atomic_load(&s_req_n));

    TEST_ASSERT_TRUE(kode_screen_new("Next"));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_FALSE(kode_widget_set_color(b, KODE_BLUE));
    TEST_ASSERT_EQUAL_STRING("no such widget", kode_error());
}

TEST_CASE("a tap on a button is pressed once", "[kode_screen]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("Taps"));
    const kode_widget_t text  = kode_screen_add_text("hello");
    const kode_widget_t start = kode_screen_add_button("Start");
    const kode_widget_t stop  = kode_screen_add_button("Stop");

    TEST_ASSERT_NOT_EQUAL_UINT32(0U, text);
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, start);
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, stop);
    TEST_ASSERT_TRUE(seen_wait(4U));

    /* Nothing tapped yet. */
    TEST_ASSERT_FALSE(kode_widget_was_pressed(start));
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    /* The first ask counts since the button was added. */
    click(1U);
    TEST_ASSERT_TRUE(kode_widget_was_pressed(start));
    TEST_ASSERT_FALSE(kode_widget_was_pressed(start));
    TEST_ASSERT_FALSE(kode_widget_was_pressed(stop));

    /* Its own button only; taps between two asks are one answer. */
    click(2U);
    click(2U);
    TEST_ASSERT_FALSE(kode_widget_was_pressed(start));
    TEST_ASSERT_TRUE(kode_widget_was_pressed(stop));
    TEST_ASSERT_FALSE(kode_widget_was_pressed(stop));

    /* A place with no button, a text widget among them, counts nothing. */
    click(0U);
    click(9U);
    TEST_ASSERT_FALSE(kode_widget_was_pressed(start));
    TEST_ASSERT_FALSE(kode_widget_was_pressed(stop));

    /* A text is no button. */
    TEST_ASSERT_FALSE(kode_widget_was_pressed(text));
    TEST_ASSERT_EQUAL_STRING("this widget is not a button", kode_error());
}

TEST_CASE("a button that is not on the screen is an error, and a new screen forgets the taps", "[kode_screen]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("One"));
    const kode_widget_t old = kode_screen_add_button("Go");

    TEST_ASSERT_NOT_EQUAL_UINT32(0U, old);
    TEST_ASSERT_TRUE(seen_wait(2U));
    const uint32_t old_gen = atomic_load(&s_gen);

    TEST_ASSERT_FALSE(kode_widget_was_pressed(0U));
    TEST_ASSERT_EQUAL_STRING("no such widget", kode_error());

    click(0U);
    TEST_ASSERT_TRUE(kode_screen_new("Two"));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_FALSE(kode_widget_was_pressed(old));
    TEST_ASSERT_EQUAL_STRING("no such widget", kode_error());

    /* The new screen's button in the same place starts unpressed, and a tap
     * that was meant for the old screen does not reach it. */
    const kode_widget_t go = kode_screen_add_button("Go");

    TEST_ASSERT_NOT_EQUAL_UINT32(0U, go);
    TEST_ASSERT_TRUE(seen_wait(1U));
    kode_widget_tapped(old_gen, 0U);
    TEST_ASSERT_FALSE(kode_widget_was_pressed(go));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_FALSE(kode_widget_was_pressed(old));
    click(0U);
    TEST_ASSERT_TRUE(kode_widget_was_pressed(go));
}

TEST_CASE("two buttons with one label are refused, in any case", "[kode_screen]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("Twins"));
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, kode_screen_add_button("Go"));
    TEST_ASSERT_TRUE(seen_wait(2U));
    const uint32_t before = atomic_load(&s_req_n);

    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_button("GO"));
    TEST_ASSERT_EQUAL_STRING("a button called 'GO' is already on the screen", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_button("Go"));
    TEST_ASSERT_EQUAL_UINT32(before, atomic_load(&s_req_n));
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, kode_screen_add_button("Gone"));

    /* A new screen starts the labels over. */
    TEST_ASSERT_TRUE(kode_screen_new("Again"));
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, kode_screen_add_button("go"));
}

TEST_CASE("values and buttons count toward the screen's widgets", "[kode_screen]")
{
    screen_case();

    kode_widget_t buttons[KODE_WIDGETS_MAX];

    TEST_ASSERT_TRUE(kode_screen_new("Full"));
    for ( uint32_t i = 0U; KODE_WIDGETS_MAX > i; i++ )
    {
        char label[KODE_BUTTON_LABEL_MAX];

        (void)snprintf(label, sizeof(label), "b%" PRIu32, i);
        buttons[i] = kode_screen_add_button(label);
        TEST_ASSERT_NOT_EQUAL_UINT32(0U, buttons[i]);
    }
    TEST_ASSERT_TRUE(seen_wait(KODE_WIDGETS_MAX + 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_value("1", "x"));
    TEST_ASSERT_EQUAL_STRING("a screen holds at most 32 widgets", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_button("one more"));
    TEST_ASSERT_EQUAL_STRING("a screen holds at most 32 widgets", kode_error());

    click(KODE_WIDGETS_MAX - 1U);
    TEST_ASSERT_TRUE(kode_widget_was_pressed(buttons[KODE_WIDGETS_MAX - 1U]));
    TEST_ASSERT_FALSE(kode_widget_was_pressed(buttons[0]));
}

/* ---- A canvas ----------------------------------------------------------------------- */

/** @brief Width of the race case's canvas. */
#define RACE_WIDTH 346U

/** @brief And its height. */
#define RACE_HEIGHT 264U

/** @brief Draws the race case's task makes at most. */
#define RACE_DRAWS_MAX 20000U

/** @brief Refusals in a row that tell that task the canvas is gone. */
#define RACE_FAILS 50U

static SemaphoreHandle_t s_drawer_ready;   /**< Given by the drawing task after its first draw.   */
static SemaphoreHandle_t s_drawer_done;    /**< Given by the drawing task when it leaves.          */
static kode_canvas_t     s_race_canvas;    /**< The canvas it draws on.                            */
static _Atomic uint32_t  s_race_drawn;     /**< Draws that went.                                   */
static _Atomic uint32_t  s_race_failed;    /**< Draws refused.                                     */
static _Atomic uint32_t  s_race_late;      /**< Draws that began after the screen retired and went. */

/** @brief The pixel (x, y) of the canvas the fake last heard of. */
static uint16_t canvas_px(uint32_t x, uint32_t y)
{
    const kode_raster_t v = kode_canvasbuf_view(s_canvas);

    return v.px[(y * v.w) + x];
}

/** @brief Whether every pixel of the canvas the fake last heard of is @p want. */
static bool canvas_uniform(uint16_t want)
{
    const kode_raster_t v = kode_canvasbuf_view(s_canvas);
    bool                same = true;

    for ( uint32_t i = 0U; (v.w * v.h) > i; i++ )
    {
        same = same && (v.px[i] == want);
    }
    return same;
}

/** @brief A screen with one canvas of w by h, heard by the fake. */
static kode_canvas_t canvas_screen(uint32_t w, uint32_t h)
{
    TEST_ASSERT_TRUE(kode_screen_new("Draw"));
    const kode_canvas_t c = kode_screen_add_canvas(w, h);

    TEST_ASSERT_NOT_EQUAL_UINT32(0U, c);
    TEST_ASSERT_TRUE(seen_wait(2U));
    TEST_ASSERT_NOT_NULL(s_canvas);
    return c;
}

TEST_CASE("a canvas posts its place and starts black", "[kode_canvas_t]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("Draw"));
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, kode_screen_add_text("above"));
    const kode_canvas_t c = kode_screen_add_canvas(10U, 8U);

    TEST_ASSERT_TRUE(seen_wait(3U));
    TEST_ASSERT_EQUAL_UINT8(KODE_SCREEN_OP_CANVAS, s_last.op);
    TEST_ASSERT_EQUAL_UINT32(1U, s_last.widget);
    TEST_ASSERT_EQUAL_UINT32(atomic_load(&s_gen), (uint32_t)s_last.num[0]);
    TEST_ASSERT_EQUAL_UINT32(kode_widget_make(atomic_load(&s_gen), 1U), c);
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    const kode_raster_t v = kode_canvasbuf_view(s_canvas);

    TEST_ASSERT_EQUAL_UINT32(10U, v.w);
    TEST_ASSERT_EQUAL_UINT32(8U, v.h);
    TEST_ASSERT_TRUE(canvas_uniform(0U));
    TEST_ASSERT_FALSE(kode_canvasbuf_take_dirty(s_canvas));
}

/** @brief Whether a canvas of w by h is accepted on the current screen (and heard, if so). */
static bool canvas_accepted(uint32_t w, uint32_t h)
{
    const uint32_t before = atomic_load(&s_req_n);
    const bool     ok     = (kode_screen_add_canvas(w, h) != 0U);

    if ( ok )
    {
        TEST_ASSERT_TRUE(seen_wait(1U));
        TEST_ASSERT_EQUAL_UINT32(before + 1U, atomic_load(&s_req_n));
    }
    else
    {
        TEST_ASSERT_EQUAL_UINT32(before, atomic_load(&s_req_n));
    }
    return ok;
}

TEST_CASE("a canvas is the glass or fits the body, with a title or without", "[kode_canvas_t]")
{
    screen_case();

    static const char titled[] = "a canvas is 410 x 502 (the glass) or up to 346 x 264 on this screen";
    static const char bare[]   = "a canvas is 410 x 502 (the glass) or up to 346 x 422 on this screen";

    TEST_ASSERT_TRUE(kode_screen_new("Title"));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_TRUE(canvas_accepted(KODE_CANVAS_BODY_W, KODE_CANVAS_BODY_H_TITLE));
    TEST_ASSERT_TRUE(kode_screen_new("Title"));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_FALSE(canvas_accepted(KODE_CANVAS_BODY_W + 1U, 10U));
    TEST_ASSERT_EQUAL_STRING(titled, kode_error());
    TEST_ASSERT_FALSE(canvas_accepted(10U, KODE_CANVAS_BODY_H_TITLE + 1U));
    TEST_ASSERT_EQUAL_STRING(titled, kode_error());
    TEST_ASSERT_FALSE(canvas_accepted(0U, 5U));
    TEST_ASSERT_FALSE(canvas_accepted(5U, 0U));
    TEST_ASSERT_FALSE(canvas_accepted(UINT32_MAX, 1U));
    TEST_ASSERT_FALSE(canvas_accepted(KODE_CANVAS_WIDTH_MAX, KODE_CANVAS_HEIGHT_MAX - 1U));
    TEST_ASSERT_FALSE(canvas_accepted(KODE_CANVAS_WIDTH_MAX - 1U, KODE_CANVAS_HEIGHT_MAX));
    TEST_ASSERT_FALSE(canvas_accepted(KODE_CANVAS_BODY_W, KODE_CANVAS_BODY_H_BARE));
    TEST_ASSERT_TRUE(canvas_accepted(1U, 1U));
    /* The glass is a canvas on a screen with a title too. */
    TEST_ASSERT_TRUE(canvas_accepted(KODE_CANVAS_WIDTH_MAX, KODE_CANVAS_HEIGHT_MAX));
    const kode_raster_t glass = kode_canvasbuf_view(s_canvas);

    TEST_ASSERT_EQUAL_UINT32(410U, glass.w);
    TEST_ASSERT_EQUAL_UINT32(502U, glass.h);
    TEST_ASSERT_TRUE(canvas_uniform(0U));

    /* No header: the body is taller. */
    TEST_ASSERT_TRUE(kode_screen_new(NULL));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_FALSE(canvas_accepted(KODE_CANVAS_BODY_W, KODE_CANVAS_BODY_H_BARE + 1U));
    TEST_ASSERT_EQUAL_STRING(bare, kode_error());
    TEST_ASSERT_FALSE(canvas_accepted(KODE_CANVAS_BODY_W + 1U, 1U));
    TEST_ASSERT_EQUAL_STRING(bare, kode_error());
    TEST_ASSERT_TRUE(canvas_accepted(KODE_CANVAS_BODY_W, KODE_CANVAS_BODY_H_BARE));
    TEST_ASSERT_TRUE(canvas_accepted(KODE_CANVAS_WIDTH_MAX, KODE_CANVAS_HEIGHT_MAX));

    /* An empty title is no header either. */
    TEST_ASSERT_TRUE(kode_screen_new(""));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_TRUE(canvas_accepted(KODE_CANVAS_BODY_W, KODE_CANVAS_BODY_H_BARE));
    TEST_ASSERT_TRUE(kode_screen_new("Back"));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_FALSE(canvas_accepted(KODE_CANVAS_BODY_W, KODE_CANVAS_BODY_H_BARE));
}

TEST_CASE("a canvas under other widgets fits the height they leave", "[kode_canvas_t]")
{
    char           want[WANT_MAX];
    const uint32_t under_text  = KODE_CANVAS_BODY_H_TITLE - KODE_SCREEN_LINE_H - KODE_SCREEN_GAP_H;
    const uint32_t under_three = KODE_CANVAS_BODY_H_BARE - (KODE_SCREEN_NUMBER_H + KODE_SCREEN_LINE_H)
                                 - KODE_SCREEN_GAP_H - KODE_SCREEN_BUTTON_H - KODE_SCREEN_GAP_H;

    screen_case();

    /* A line of text over a canvas as tall as the whole body would make the screen scroll. */
    TEST_ASSERT_TRUE(kode_screen_new("Score"));
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, kode_screen_add_text("Score"));
    TEST_ASSERT_TRUE(seen_wait(2U));
    TEST_ASSERT_FALSE(canvas_accepted(KODE_CANVAS_BODY_W, KODE_CANVAS_BODY_H_TITLE));
    (void)snprintf(want, sizeof(want), "a canvas is 410 x 502 (the glass) or up to 346 x %" PRIu32
                   " on this screen", under_text);
    TEST_ASSERT_EQUAL_STRING(want, kode_error());
    TEST_ASSERT_TRUE(canvas_accepted(KODE_CANVAS_BODY_W, under_text));

    /* A value with its unit, a button and the glass, which floats and takes no room. */
    TEST_ASSERT_TRUE(kode_screen_new(NULL));
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, kode_screen_add_value("7", "km"));
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, kode_screen_add_button("Go"));
    TEST_ASSERT_TRUE(seen_wait(3U));
    TEST_ASSERT_TRUE(canvas_accepted(KODE_CANVAS_WIDTH_MAX, KODE_CANVAS_HEIGHT_MAX));
    TEST_ASSERT_FALSE(canvas_accepted(KODE_CANVAS_BODY_W, under_three + 1U));
    TEST_ASSERT_TRUE(canvas_accepted(KODE_CANVAS_BODY_W, under_three));

    /* Nothing is left under a full body. */
    TEST_ASSERT_FALSE(canvas_accepted(1U, 1U));
    TEST_ASSERT_EQUAL_STRING("a canvas is 410 x 502 (the glass) or up to 346 x 0 on this screen", kode_error());
}

TEST_CASE("every draw call reaches the canvas's pixels", "[kode_canvas_t]")
{
    screen_case();

    const kode_canvas_t c = canvas_screen(12U, 9U);
    const uint16_t    red  = kode_raster_rgb565(KODE_RED);
    const uint16_t    blue = kode_raster_rgb565(KODE_BLUE);

    TEST_ASSERT_TRUE(kode_canvas_fill(c, KODE_BLUE));
    TEST_ASSERT_TRUE(canvas_uniform(blue));
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    TEST_ASSERT_TRUE(kode_draw_pixel(c, 3, 2, KODE_RED));
    TEST_ASSERT_EQUAL_HEX16(red, canvas_px(3U, 2U));
    TEST_ASSERT_EQUAL_HEX16(blue, canvas_px(4U, 2U));

    TEST_ASSERT_TRUE(kode_canvas_fill(c, KODE_BLACK));
    TEST_ASSERT_TRUE(kode_draw_line(c, 0, 0, 5, 5, KODE_RED));
    TEST_ASSERT_EQUAL_HEX16(red, canvas_px(0U, 0U));
    TEST_ASSERT_EQUAL_HEX16(red, canvas_px(5U, 5U));
    TEST_ASSERT_EQUAL_HEX16(0U, canvas_px(5U, 4U));

    TEST_ASSERT_TRUE(kode_canvas_fill(c, KODE_BLACK));
    TEST_ASSERT_TRUE(kode_fill_rect(c, 2, 2, 3, 3, KODE_RED));
    TEST_ASSERT_EQUAL_HEX16(red, canvas_px(3U, 3U));
    TEST_ASSERT_TRUE(kode_draw_rect(c, 6, 1, 4, 4, KODE_BLUE));
    TEST_ASSERT_EQUAL_HEX16(blue, canvas_px(6U, 1U));
    TEST_ASSERT_EQUAL_HEX16(0U, canvas_px(7U, 2U));

    TEST_ASSERT_TRUE(kode_canvas_fill(c, KODE_BLACK));
    TEST_ASSERT_TRUE(kode_fill_circle(c, 6, 4, 3, KODE_RED));
    TEST_ASSERT_EQUAL_HEX16(red, canvas_px(6U, 4U));
    TEST_ASSERT_EQUAL_HEX16(red, canvas_px(9U, 4U));
    TEST_ASSERT_EQUAL_HEX16(0U, canvas_px(10U, 4U));
    TEST_ASSERT_TRUE(kode_draw_circle(c, 6, 4, 4, KODE_BLUE));
    TEST_ASSERT_EQUAL_HEX16(blue, canvas_px(10U, 4U));

    /* Off the canvas is clipped, not an error. */
    TEST_ASSERT_TRUE(kode_draw_pixel(c, -5, 100, KODE_RED));
    TEST_ASSERT_TRUE(kode_draw_line(c, -50, -50, 90, 90, KODE_RED));
    TEST_ASSERT_TRUE(kode_fill_rect(c, 8, 6, 100, 100, KODE_RED));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_HEX16(red, canvas_px(11U, 8U));
}

TEST_CASE("a canvas of an older screen fails every draw, and says so", "[kode_canvas_t]")
{
    screen_case();

    const kode_canvas_t old = canvas_screen(8U, 8U);

    TEST_ASSERT_TRUE(kode_screen_new("Next"));
    TEST_ASSERT_TRUE(seen_wait(1U));
    const kode_canvas_t now = kode_screen_add_canvas(8U, 8U);

    TEST_ASSERT_NOT_EQUAL_UINT32(0U, now);
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_NOT_EQUAL_UINT32(old, now);

    static const char stale[] = "this canvas belongs to an old screen";

    TEST_ASSERT_FALSE(kode_canvas_fill(old, KODE_RED));
    TEST_ASSERT_EQUAL_STRING(stale, kode_error());
    TEST_ASSERT_FALSE(kode_draw_pixel(old, 1, 1, KODE_RED));
    TEST_ASSERT_EQUAL_STRING(stale, kode_error());
    TEST_ASSERT_FALSE(kode_draw_line(old, 0, 0, 3, 3, KODE_RED));
    TEST_ASSERT_EQUAL_STRING(stale, kode_error());
    TEST_ASSERT_FALSE(kode_fill_rect(old, 0, 0, 3, 3, KODE_RED));
    TEST_ASSERT_EQUAL_STRING(stale, kode_error());
    TEST_ASSERT_FALSE(kode_draw_circle(old, 3, 3, 2, KODE_RED));
    TEST_ASSERT_EQUAL_STRING(stale, kode_error());

    /* Nor is a handle that never was one, or a text widget's, a canvas. */
    TEST_ASSERT_FALSE(kode_draw_pixel(0U, 1, 1, KODE_RED));
    TEST_ASSERT_EQUAL_STRING("no such canvas", kode_error());
    TEST_ASSERT_FALSE(kode_draw_pixel(now + 1U, 1, 1, KODE_RED));
    TEST_ASSERT_EQUAL_STRING("no such canvas", kode_error());
    const kode_widget_t text = kode_screen_add_text("not a canvas");

    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_FALSE(kode_draw_pixel(text, 1, 1, KODE_RED));
    TEST_ASSERT_EQUAL_STRING("no such canvas", kode_error());
    TEST_ASSERT_FALSE(kode_widget_set_text(now, "x"));
    TEST_ASSERT_EQUAL_STRING("a canvas has no text", kode_error());

    TEST_ASSERT_TRUE(kode_draw_pixel(now, 1, 1, KODE_RED));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("a canvas's pixels are freed once, and only after the screen has let go", "[kode_canvas_t]")
{
    screen_case();

    const kode_canvas_t c = canvas_screen(20U, 20U);

    fake_task_slot_watch(kode_canvasbuf_view(s_canvas).px);
    TEST_ASSERT_TRUE(kode_draw_pixel(c, 1, 1, KODE_RED));

    /* The screen is on its way, and the screen still holds the old one. */
    atomic_store(&s_gated, true);
    TEST_ASSERT_TRUE(kode_screen_new("Away"));
    TEST_ASSERT_FALSE(kode_draw_pixel(c, 1, 1, KODE_RED));
    TEST_ASSERT_EQUAL_STRING("this canvas belongs to an old screen", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)fake_task_slot_watch_frees());

    /* The screen has dropped its objects and retires them. */
    TEST_ASSERT_TRUE(gate_open(1U));
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)fake_task_slot_watch_frees());
    TEST_ASSERT_FALSE(kode_draw_pixel(c, 1, 1, KODE_RED));
    TEST_ASSERT_EQUAL_STRING("this canvas belongs to an old screen", kode_error());

    /* Another screen retires nothing more of it. */
    TEST_ASSERT_TRUE(kode_screen_new("Again"));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)fake_task_slot_watch_frees());
}

/** @brief The task of the race case: draw until the canvas is gone. */
static void drawer_task(void *arg)
{
    (void)arg;
    bool     ready = false;
    uint32_t fails = 0U;

    for ( uint32_t n = 0U; (RACE_DRAWS_MAX > n) && (RACE_FAILS > fails); n++ )
    {
        const bool after = atomic_load(&s_retired);
        const bool ok    = kode_canvas_fill(s_race_canvas, KODE_RED);

        if ( ok )
        {
            (void)atomic_fetch_add(&s_race_drawn, 1U);
            if ( after )
            {
                (void)atomic_fetch_add(&s_race_late, 1U);
            }
            if ( !ready )
            {
                ready = true;
                (void)xSemaphoreGive(s_drawer_ready);
            }
        }
        else
        {
            (void)atomic_fetch_add(&s_race_failed, 1U);
            fails++;
        }
    }
    (void)xSemaphoreGive(s_drawer_done);
}

TEST_CASE("a new screen while another task still draws writes into no freed memory", "[kode_canvas_t]")
{
    screen_case();

    s_drawer_ready = xSemaphoreCreateBinary();
    s_drawer_done  = xSemaphoreCreateBinary();
    atomic_store(&s_race_drawn, 0U);
    atomic_store(&s_race_failed, 0U);
    atomic_store(&s_race_late, 0U);
    s_race_canvas = canvas_screen(RACE_WIDTH, RACE_HEIGHT);
    fake_task_slot_watch(kode_canvasbuf_view(s_canvas).px);
    atomic_store(&s_retired, false);     /* the screen just built retired the one before it */

    TEST_ASSERT_EQUAL(KODE_OK, khal_task_start(&drawer_task, "t_drawer", TEST_STACK_BYTES, 1U, NULL));
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_drawer_ready, pdMS_TO_TICKS(TEST_WAIT_MS)));

    /* The screen retires on its own thread, whenever it hears the screen. */
    TEST_ASSERT_TRUE(kode_screen_new("Next"));
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_drawer_done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    TEST_ASSERT_TRUE(seen_wait(1U));

    TEST_ASSERT_TRUE(atomic_load(&s_retired));
    TEST_ASSERT_TRUE(atomic_load(&s_race_drawn) > 0U);
    TEST_ASSERT_TRUE(atomic_load(&s_race_failed) >= RACE_FAILS);
    /* A draw that began after the screen had freed the pixels never went. */
    TEST_ASSERT_EQUAL_UINT32(0U, atomic_load(&s_race_late));
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)fake_task_slot_watch_frees());
    TEST_ASSERT_FALSE(kode_canvas_fill(s_race_canvas, KODE_RED));
    TEST_ASSERT_EQUAL_STRING("this canvas belongs to an old screen", kode_error());
}

TEST_CASE("drawing marks the canvas, and the screen's tick takes the mark once", "[kode_canvas_t]")
{
    screen_case();

    const kode_canvas_t c = canvas_screen(30U, 30U);

    TEST_ASSERT_FALSE(kode_canvasbuf_take_dirty(s_canvas));
    for ( int32_t i = 0; 1000 > i; i++ )
    {
        TEST_ASSERT_TRUE(kode_draw_pixel(c, i % 30, i / 40, KODE_RED));
    }
    TEST_ASSERT_TRUE(kode_canvasbuf_take_dirty(s_canvas));     /* one invalidate for a thousand draws */
    TEST_ASSERT_FALSE(kode_canvasbuf_take_dirty(s_canvas));

    TEST_ASSERT_TRUE(kode_draw_pixel(c, 1, 1, KODE_BLUE));
    TEST_ASSERT_TRUE(kode_canvasbuf_take_dirty(s_canvas));
    TEST_ASSERT_FALSE(kode_canvasbuf_take_dirty(s_canvas));

    /* A draw that fails marks nothing. */
    TEST_ASSERT_FALSE(kode_draw_pixel(c, KODE_DRAW_RANGE + 1, 0, KODE_BLUE));
    TEST_ASSERT_FALSE(kode_canvasbuf_take_dirty(s_canvas));
}

TEST_CASE("a draw refuses a coordinate or a size it cannot trust", "[kode_canvas_t]")
{
    screen_case();

    const kode_canvas_t c = canvas_screen(16U, 16U);
    static const char range[] = "a coordinate, size or radius is beyond 32767 either way";
    const int32_t     far = KODE_DRAW_RANGE + 1;

    TEST_ASSERT_FALSE(kode_draw_pixel(c, far, 0, KODE_RED));
    TEST_ASSERT_EQUAL_STRING(range, kode_error());
    TEST_ASSERT_FALSE(kode_draw_pixel(c, 0, -far, KODE_RED));
    TEST_ASSERT_EQUAL_STRING(range, kode_error());
    TEST_ASSERT_FALSE(kode_draw_pixel(c, INT32_MIN, 0, KODE_RED));
    TEST_ASSERT_EQUAL_STRING(range, kode_error());
    TEST_ASSERT_FALSE(kode_draw_line(c, 0, 0, far, 0, KODE_RED));
    TEST_ASSERT_EQUAL_STRING(range, kode_error());
    TEST_ASSERT_FALSE(kode_draw_line(c, 0, -far, 0, 0, KODE_RED));
    TEST_ASSERT_EQUAL_STRING(range, kode_error());
    TEST_ASSERT_FALSE(kode_fill_rect(c, 0, 0, far, 1, KODE_RED));
    TEST_ASSERT_EQUAL_STRING(range, kode_error());
    TEST_ASSERT_FALSE(kode_draw_circle(c, 0, 0, far, KODE_RED));
    TEST_ASSERT_EQUAL_STRING(range, kode_error());
    TEST_ASSERT_FALSE(kode_draw_circle(c, far, 0, 3, KODE_RED));
    TEST_ASSERT_EQUAL_STRING(range, kode_error());

    TEST_ASSERT_FALSE(kode_fill_rect(c, 0, 0, -1, 4, KODE_RED));
    TEST_ASSERT_EQUAL_STRING("a width or height is negative", kode_error());
    TEST_ASSERT_FALSE(kode_draw_rect(c, 0, 0, 4, -1, KODE_RED));
    TEST_ASSERT_EQUAL_STRING("a width or height is negative", kode_error());
    TEST_ASSERT_FALSE(kode_fill_circle(c, 5, 5, -1, KODE_RED));
    TEST_ASSERT_EQUAL_STRING("a radius is negative", kode_error());
    TEST_ASSERT_TRUE(canvas_uniform(0U));

    /* The limits themselves are fine. */
    TEST_ASSERT_TRUE(kode_draw_pixel(c, KODE_DRAW_RANGE, -KODE_DRAW_RANGE, KODE_RED));
    TEST_ASSERT_TRUE(kode_draw_line(c, -KODE_DRAW_RANGE, -KODE_DRAW_RANGE,
                                    KODE_DRAW_RANGE, KODE_DRAW_RANGE, KODE_RED));
    TEST_ASSERT_TRUE(kode_draw_rect(c, -KODE_DRAW_RANGE, -KODE_DRAW_RANGE,
                                    KODE_DRAW_RANGE, KODE_DRAW_RANGE, KODE_RED));
    TEST_ASSERT_TRUE(kode_fill_circle(c, 0, 0, KODE_DRAW_RANGE, KODE_RED));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_TRUE(canvas_uniform(kode_raster_rgb565(KODE_RED)));    /* the disc covers it */
}

TEST_CASE("a screen holds KODE_CANVAS_MAX canvases, and they count among its widgets", "[kode_canvas_t]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("Many"));
    for ( uint32_t i = 0U; KODE_CANVAS_MAX > i; i++ )
    {
        TEST_ASSERT_NOT_EQUAL_UINT32(0U, kode_screen_add_canvas(4U, 4U));
    }
    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_canvas(4U, 4U));
    TEST_ASSERT_EQUAL_STRING("a screen holds at most 4 canvases", kode_error());
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, kode_screen_add_text("room for a text"));
    TEST_ASSERT_TRUE(seen_wait(KODE_CANVAS_MAX + 2U));

    /* Glass canvases, which take no room in the body under thirty lines of text. */
    TEST_ASSERT_TRUE(kode_screen_new("Fresh"));
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, kode_screen_add_canvas(4U, 4U));
    for ( uint32_t i = 0U; (KODE_WIDGETS_MAX - 2U) > i; i++ )
    {
        TEST_ASSERT_NOT_EQUAL_UINT32(0U, kode_screen_add_text("widget"));
    }
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, kode_screen_add_canvas(KODE_CANVAS_WIDTH_MAX, KODE_CANVAS_HEIGHT_MAX));  /* the 32nd widget */
    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_canvas(KODE_CANVAS_WIDTH_MAX, KODE_CANVAS_HEIGHT_MAX));
    TEST_ASSERT_EQUAL_STRING("a screen holds at most 32 widgets", kode_error());
    TEST_ASSERT_TRUE(seen_wait(KODE_WIDGETS_MAX + 1U));
}

TEST_CASE("a canvas that cannot be posted is given back at once", "[kode_canvas_t]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("Busy"));
    const kode_widget_t p = kode_screen_add_text("0");

    TEST_ASSERT_TRUE(seen_wait(2U));
    atomic_store(&s_gated, true);
    fake_time_reset();
    uint32_t taken = 0U;

    while ( (FILL_CALLS > taken) && kode_widget_set_text(p, "%" PRIu32, taken) )
    {
        taken++;
    }
    TEST_ASSERT_TRUE(FILL_CALLS > taken);

    const uint32_t allocs = (uint32_t)fake_psram_alloc_count();
    const uint32_t frees  = (uint32_t)fake_psram_free_count();

    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_canvas(50U, 50U));
    TEST_ASSERT_EQUAL_STRING("the screen is busy", kode_error());
    TEST_ASSERT_TRUE((uint32_t)fake_psram_alloc_count() > allocs);          /* it had the pixels */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)fake_psram_alloc_count() - allocs,
                             (uint32_t)fake_psram_free_count() - frees);    /* and gave them all back */
    TEST_ASSERT_TRUE(gate_open(taken));

    /* The place it would have had is free for the next widget. */
    const kode_canvas_t c = kode_screen_add_canvas(5U, 5U);

    TEST_ASSERT_NOT_EQUAL_UINT32(0U, c);
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_last.widget);
}

TEST_CASE("a canvas with no PSRAM to hold it fails whole", "[kode_canvas_t]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("Memory"));
    TEST_ASSERT_TRUE(seen_wait(1U));
    const uint32_t before = atomic_load(&s_req_n);

    fake_psram_refuse(1U);
    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_canvas(10U, 10U));
    TEST_ASSERT_EQUAL_STRING("no memory for a canvas of 10 x 10 pixels", kode_error());

    /* The pixels refused after the first block was given: that one goes back. */
    fake_psram_refuse_after(1U, 1U);
    const uint32_t allocs = (uint32_t)fake_psram_alloc_count();
    const uint32_t frees  = (uint32_t)fake_psram_free_count();

    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_canvas(10U, 10U));
    TEST_ASSERT_EQUAL_STRING("no memory for a canvas of 10 x 10 pixels", kode_error());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)fake_psram_alloc_count() - allocs,
                             (uint32_t)fake_psram_free_count() - frees);
    TEST_ASSERT_EQUAL_UINT32(before, atomic_load(&s_req_n));

    TEST_ASSERT_NOT_EQUAL_UINT32(0U, kode_screen_add_canvas(10U, 10U));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_EQUAL_UINT32(0U, s_last.widget);
}

/* ---- Text on a canvas -------------------------------------------------------------- */

/** @brief A colour's three bytes, as a request carries them. */
#define RGB24(c) ((int32_t)((uint32_t)(c) & 0x00FFFFFFU))

/** @brief A request's colour word: 0xRRGGBB under the alignment in the top byte. */
#define SHADE_RGB_MASK   0x00FFFFFFU
#define SHADE_ALIGN_SHIFT 24U
#define SHADE(c, a) ((int32_t)(((uint32_t)(c) & SHADE_RGB_MASK) | ((uint32_t)(a) << SHADE_ALIGN_SHIFT)))

/** @brief The slot is the low byte of a request's wait word; a screen has this many. */
#define WAIT_SLOT_MASK 0xFFU
#define WAIT_SLOTS     4U

/** @brief Draw-text-then-shape rounds the order case makes. */
#define ORDER_LOOPS 50

TEST_CASE("text on a canvas posts its own copy, place, colour and alignment", "[kode_canvas_t]")
{
    screen_case();

    const kode_canvas_t c = canvas_screen(100U, 50U);
    char              text[] = "Hello";

    TEST_ASSERT_TRUE(kode_draw_text(c, 10, -4, text, KODE_BLUE, KODE_ALIGN_CENTER));
    text[0] = 'J';
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_EQUAL_UINT8(KODE_SCREEN_OP_DRAW_TEXT, s_last.op);
    TEST_ASSERT_EQUAL_UINT32(c, s_last.widget);
    TEST_ASSERT_EQUAL_STRING("Hello", s_last.text);
    TEST_ASSERT_EQUAL_INT32(10, s_last.num[0]);
    TEST_ASSERT_EQUAL_INT32(-4, s_last.num[1]);
    TEST_ASSERT_EQUAL_INT32(SHADE(KODE_BLUE, KODE_ALIGN_CENTER), s_last.num[2]);
    TEST_ASSERT_TRUE(((uint32_t)s_last.num[3] & WAIT_SLOT_MASK) < WAIT_SLOTS);
    TEST_ASSERT_TRUE(0 < s_last.num[3]);                                  /* stays positive */
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    TEST_ASSERT_TRUE(kode_draw_text(c, 99, 49, "Right", KODE_RED | 0xAB000000U, KODE_ALIGN_RIGHT));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_EQUAL_INT32(SHADE(KODE_RED, KODE_ALIGN_RIGHT), s_last.num[2]);
    TEST_ASSERT_TRUE(kode_draw_text(c, 0, 0, "Left\nTwo", KODE_WHITE, KODE_ALIGN_LEFT));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_EQUAL_INT32(SHADE(KODE_WHITE, KODE_ALIGN_LEFT), s_last.num[2]);
    TEST_ASSERT_EQUAL_STRING("Left\nTwo", s_last.text);
    TEST_ASSERT_TRUE(kode_draw_text(c, 0, 0, "", KODE_WHITE, KODE_ALIGN_LEFT));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_EQUAL_STRING("", s_last.text);
}

TEST_CASE("a text the screen dropped is a failed call, not a drawn one", "[kode_canvas_t]")
{
    screen_case();

    const kode_canvas_t c = canvas_screen(20U, 20U);

    atomic_store(&s_drop_text, true);
    TEST_ASSERT_FALSE(kode_draw_text(c, 1, 1, "lost", KODE_RED, KODE_ALIGN_LEFT));
    TEST_ASSERT_EQUAL_STRING("the text could not be drawn", kode_error());
    TEST_ASSERT_EQUAL_HEX16(0U, canvas_px(1U, 1U));
    atomic_store(&s_drop_text, false);
    TEST_ASSERT_TRUE(kode_draw_text(c, 1, 1, "kept", KODE_RED, KODE_ALIGN_LEFT));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_HEX16(kode_raster_rgb565(KODE_RED), canvas_px(1U, 1U));
}

TEST_CASE("a shape drawn after the text returns lands on top of it, never under", "[kode_canvas_t]")
{
    screen_case();

    const kode_canvas_t c = canvas_screen(20U, 20U);
    const uint16_t    red  = kode_raster_rgb565(KODE_RED);
    const uint16_t    blue = kode_raster_rgb565(KODE_BLUE);

    for ( int32_t i = 0; ORDER_LOOPS > i; i++ )
    {
        TEST_ASSERT_TRUE(kode_canvas_fill(c, KODE_BLACK));
        TEST_ASSERT_TRUE(kode_draw_text(c, 3, 4, "t", KODE_RED, KODE_ALIGN_LEFT));
        /* The screen has drawn it by the time the call returns. */
        TEST_ASSERT_EQUAL_HEX16(red, canvas_px(3U, 4U));
        TEST_ASSERT_TRUE(kode_draw_pixel(c, 3, 4, KODE_BLUE));
        TEST_ASSERT_EQUAL_HEX16(blue, canvas_px(3U, 4U));
    }
    TEST_ASSERT_TRUE(seen_wait((uint32_t)ORDER_LOOPS));
    TEST_ASSERT_EQUAL_HEX16(blue, canvas_px(3U, 4U));
}

/** @brief Calls of draw_text each of the waiting tasks makes. */
#define WAITERS_CALLS 5U

/** @brief Tasks that draw text at once: more than the waiting slots. */
#define WAITERS 6U

static SemaphoreHandle_t s_waiter_done[WAITERS];   /**< Given by each task on return.   */
static _Atomic uint32_t  s_waiter_ok;              /**< Calls that went.                 */
static kode_canvas_t     s_waiter_canvas;          /**< The canvas they write on.        */

/** @brief One of the tasks writing text. */
static void waiter_task(void *arg)
{
    const uint32_t i = (uint32_t)(uintptr_t)arg;

    for ( uint32_t n = 0U; WAITERS_CALLS > n; n++ )
    {
        if ( kode_draw_text(s_waiter_canvas, (int32_t)i, (int32_t)n, "w", KODE_RED, KODE_ALIGN_LEFT) )
        {
            (void)atomic_fetch_add(&s_waiter_ok, 1U);
        }
    }
    (void)xSemaphoreGive(s_waiter_done[i]);
}

TEST_CASE("more tasks than slots write text at once and every call returns drawn", "[kode_canvas_t]")
{
    screen_case();

    s_waiter_canvas = canvas_screen(20U, 20U);
    atomic_store(&s_waiter_ok, 0U);
    for ( uint32_t i = 0U; WAITERS > i; i++ )
    {
        s_waiter_done[i] = xSemaphoreCreateBinary();
        TEST_ASSERT_EQUAL(KODE_OK, khal_task_start(&waiter_task, "t_text", TEST_STACK_BYTES, TEST_PRIO,
                                                   (void *)(uintptr_t)i));
    }
    for ( uint32_t i = 0U; WAITERS > i; i++ )
    {
        TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_waiter_done[i], pdMS_TO_TICKS(TEST_WAIT_MS)));
    }
    TEST_ASSERT_EQUAL_UINT32(WAITERS * WAITERS_CALLS, atomic_load(&s_waiter_ok));
    TEST_ASSERT_TRUE(seen_wait(WAITERS * WAITERS_CALLS));
    for ( uint32_t i = 0U; WAITERS > i; i++ )
    {
        for ( uint32_t n = 0U; WAITERS_CALLS > n; n++ )
        {
            TEST_ASSERT_EQUAL_HEX16(kode_raster_rgb565(KODE_RED), canvas_px(i, n));
        }
    }
}

TEST_CASE("a text the screen is too slow to draw times out as busy, and the next waits for its own", "[kode_canvas_t]")
{
    screen_case();

    const kode_canvas_t c = canvas_screen(20U, 20U);

    atomic_store(&s_gated, true);
    /* Posted, held by the screen: the call gives up after KODE_SCREEN_BUSY_MS (a real wait here). */
    TEST_ASSERT_FALSE(kode_draw_text(c, 1, 1, "slow", KODE_RED, KODE_ALIGN_LEFT));
    TEST_ASSERT_EQUAL_STRING("the screen is busy", kode_error());
    TEST_ASSERT_EQUAL_HEX16(0U, canvas_px(1U, 1U));
    /* The screen catches up; it draws the first text and gives its wait late. */
    TEST_ASSERT_TRUE(gate_open(1U));
    TEST_ASSERT_EQUAL_HEX16(kode_raster_rgb565(KODE_RED), canvas_px(1U, 1U));

    /* That late give must not satisfy the next call before its own text is drawn. */
    atomic_store(&s_gated, true);
    TEST_ASSERT_FALSE(kode_draw_text(c, 2, 2, "slow", KODE_BLUE, KODE_ALIGN_LEFT));
    TEST_ASSERT_TRUE(gate_open(1U));
    TEST_ASSERT_TRUE(kode_draw_text(c, 3, 3, "fast", KODE_BLUE, KODE_ALIGN_LEFT));
    TEST_ASSERT_EQUAL_HEX16(kode_raster_rgb565(KODE_BLUE), canvas_px(3U, 3U));
}

TEST_CASE("text on a canvas refuses what it cannot post and posts nothing", "[kode_canvas_t]")
{
    screen_case();

    const kode_canvas_t c = canvas_screen(20U, 20U);
    char              longest[KODE_WIDGET_TEXT_MAX];
    char              toolong[KODE_WIDGET_TEXT_MAX + 1U];

    (void)memset(longest, 'a', sizeof(longest) - 1U);
    longest[sizeof(longest) - 1U] = '\0';
    (void)memset(toolong, 'a', sizeof(toolong) - 1U);
    toolong[sizeof(toolong) - 1U] = '\0';
    const uint32_t before = atomic_load(&s_req_n);

    TEST_ASSERT_FALSE(kode_draw_text(c, 0, 0, NULL, KODE_RED, KODE_ALIGN_LEFT));
    TEST_ASSERT_EQUAL_STRING("text is NULL", kode_error());
    TEST_ASSERT_FALSE(kode_draw_text(c, 0, 0, toolong, KODE_RED, KODE_ALIGN_LEFT));
    TEST_ASSERT_EQUAL_STRING("text is too long (127 bytes at most)", kode_error());
    TEST_ASSERT_FALSE(kode_draw_text(c, KODE_DRAW_RANGE + 1, 0, "x", KODE_RED, KODE_ALIGN_LEFT));
    TEST_ASSERT_EQUAL_STRING("a coordinate, size or radius is beyond 32767 either way", kode_error());
    TEST_ASSERT_FALSE(kode_draw_text(c, 0, -KODE_DRAW_RANGE - 1, "x", KODE_RED, KODE_ALIGN_LEFT));
    TEST_ASSERT_EQUAL_STRING("a coordinate, size or radius is beyond 32767 either way", kode_error());
    TEST_ASSERT_FALSE(kode_draw_text(c, 0, 0, "x", KODE_RED, (kode_align_t)3));
    TEST_ASSERT_EQUAL_STRING("unknown alignment", kode_error());
    TEST_ASSERT_FALSE(kode_draw_text(c, 0, 0, "x", KODE_RED, (kode_align_t)-1));
    TEST_ASSERT_EQUAL_STRING("unknown alignment", kode_error());
    TEST_ASSERT_FALSE(kode_draw_text(0U, 0, 0, "x", KODE_RED, KODE_ALIGN_LEFT));
    TEST_ASSERT_EQUAL_STRING("no such canvas", kode_error());
    TEST_ASSERT_FALSE(kode_draw_text(c + 1U, 0, 0, "x", KODE_RED, KODE_ALIGN_LEFT));
    TEST_ASSERT_EQUAL_STRING("no such canvas", kode_error());
    const kode_widget_t txt = kode_screen_add_text("not a canvas");

    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_FALSE(kode_draw_text(txt, 0, 0, "x", KODE_RED, KODE_ALIGN_LEFT));
    TEST_ASSERT_EQUAL_STRING("no such canvas", kode_error());
    TEST_ASSERT_EQUAL_UINT32(before + 1U, atomic_load(&s_req_n));

    /* The longest text goes. */
    TEST_ASSERT_TRUE(kode_draw_text(c, 0, 0, longest, KODE_RED, KODE_ALIGN_LEFT));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_EQUAL_STRING(longest, s_last.text);

    /* A canvas of the old screen. */
    TEST_ASSERT_TRUE(kode_screen_new("Next"));
    TEST_ASSERT_TRUE(seen_wait(1U));
    const uint32_t now = atomic_load(&s_req_n);

    TEST_ASSERT_FALSE(kode_draw_text(c, 0, 0, "x", KODE_RED, KODE_ALIGN_LEFT));
    TEST_ASSERT_EQUAL_STRING("this canvas belongs to an old screen", kode_error());
    TEST_ASSERT_EQUAL_UINT32(now, atomic_load(&s_req_n));
}

TEST_CASE("text on a canvas waits its turn when the screen is behind, then says busy", "[kode_canvas_t]")
{
    screen_case();

    const kode_canvas_t c = canvas_screen(20U, 20U);
    const kode_widget_t  p = kode_screen_add_text("0");

    TEST_ASSERT_TRUE(seen_wait(1U));
    atomic_store(&s_gated, true);
    fake_time_reset();
    uint32_t taken = 0U;

    while ( (FILL_CALLS > taken) && kode_widget_set_text(p, "%" PRIu32, taken) )
    {
        taken++;
    }
    TEST_ASSERT_TRUE(FILL_CALLS > taken);
    TEST_ASSERT_FALSE(kode_draw_text(c, 0, 0, "late", KODE_RED, KODE_ALIGN_LEFT));
    TEST_ASSERT_EQUAL_STRING("the screen is busy", kode_error());
    TEST_ASSERT_TRUE(gate_open(taken));
    TEST_ASSERT_TRUE(kode_draw_text(c, 0, 0, "now", KODE_RED, KODE_ALIGN_LEFT));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_EQUAL_STRING("now", s_last.text);
}

/* ---- An image ----------------------------------------------------------------------- */

/**
 * @brief A decoded picture of w by h whose first pixel is (1, 2, 3, 4) and the rest 9.
 * @return The picture, its pixels from the allocator the reader uses.
 */
static kode_png_t picture(uint32_t w, uint32_t h)
{
    kode_png_t p = { w, h, NULL };

    p.rgba = (uint8_t *)khal_alloc_psram(KODE_CANVAS_ALIGN, (size_t)w * h * 4U);
    TEST_ASSERT_NOT_NULL(p.rgba);
    (void)memset(p.rgba, 9, (size_t)w * h * 4U);
    p.rgba[0] = 1U;
    p.rgba[1] = 2U;
    p.rgba[2] = 3U;
    p.rgba[3] = 4U;
    return p;
}

/** @brief Blocks held now: allocations less frees. */
static uint32_t held(void)
{
    return (uint32_t)fake_psram_alloc_count() - (uint32_t)fake_psram_free_count();
}

TEST_CASE("an image is posted at its place and keeps its pixels in LVGL's order", "[kode_image]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("Pic"));
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, kode_screen_add_text("above"));
    kode_png_t png = picture(10U, 8U);
    const kode_widget_t im = kode_screen_image_add("flash/a.png", &png);

    TEST_ASSERT_NOT_EQUAL_UINT32(0U, im);
    TEST_ASSERT_NULL(png.rgba);
    TEST_ASSERT_TRUE(seen_wait(3U));
    TEST_ASSERT_EQUAL_UINT8(KODE_SCREEN_OP_IMAGE, s_last.op);
    TEST_ASSERT_EQUAL_UINT32(1U, s_last.widget);
    TEST_ASSERT_EQUAL_UINT32(atomic_load(&s_gen), (uint32_t)s_last.num[0]);
    TEST_ASSERT_EQUAL_UINT32(kode_widget_make(atomic_load(&s_gen), 1U), im);
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    uint32_t       w = 0U;
    uint32_t       h = 0U;
    const uint8_t *px = kode_canvasbuf_image_view(s_canvas, &w, &h);

    TEST_ASSERT_NOT_NULL(px);
    TEST_ASSERT_EQUAL_UINT32(10U, w);
    TEST_ASSERT_EQUAL_UINT32(8U, h);
    /* Blue, green, red, alpha. */
    TEST_ASSERT_EQUAL_HEX8(3U, px[0]);
    TEST_ASSERT_EQUAL_HEX8(2U, px[1]);
    TEST_ASSERT_EQUAL_HEX8(1U, px[2]);
    TEST_ASSERT_EQUAL_HEX8(4U, px[3]);
    TEST_ASSERT_EQUAL_HEX8(9U, px[4]);
}

TEST_CASE("an image cannot be drawn on, written on or given text", "[kode_image]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("Pic"));
    kode_png_t png = picture(10U, 8U);
    const kode_widget_t im = kode_screen_image_add("flash/a.png", &png);

    TEST_ASSERT_NOT_EQUAL_UINT32(0U, im);
    TEST_ASSERT_TRUE(seen_wait(2U));
    TEST_ASSERT_FALSE(kode_draw_pixel(im, 1, 1, KODE_RED));
    TEST_ASSERT_EQUAL_STRING("no such canvas", kode_error());
    TEST_ASSERT_FALSE(kode_canvas_fill(im, KODE_RED));
    TEST_ASSERT_EQUAL_STRING("no such canvas", kode_error());
    TEST_ASSERT_FALSE(kode_draw_text(im, 1, 1, "x", KODE_RED, KODE_ALIGN_LEFT));
    TEST_ASSERT_EQUAL_STRING("no such canvas", kode_error());
    TEST_ASSERT_FALSE(kode_widget_set_text(im, "x"));
    TEST_ASSERT_EQUAL_STRING("an image has no text", kode_error());
    uint32_t w = 0U;
    uint32_t h = 0U;

    TEST_ASSERT_EQUAL_HEX8(3U, kode_canvasbuf_image_view(s_canvas, &w, &h)[0]);
}

/** @brief Add a picture of w by h as an image and say whether the screen took it (and heard it). */
static bool image_accepted(uint32_t w, uint32_t h)
{
    kode_png_t     png    = picture(w, h);
    const uint32_t before = atomic_load(&s_req_n);
    const uint32_t kept   = held();
    const bool     ok     = (kode_screen_image_add("flash/a.png", &png) != 0U);

    TEST_ASSERT_NULL(png.rgba);
    if ( ok )
    {
        TEST_ASSERT_TRUE(seen_wait(1U));
        TEST_ASSERT_EQUAL_UINT32(before + 1U, atomic_load(&s_req_n));
        TEST_ASSERT_TRUE(held() > kept);                  /* the screen holds the pixels now */
    }
    else
    {
        TEST_ASSERT_EQUAL_UINT32(before, atomic_load(&s_req_n));
        TEST_ASSERT_EQUAL_UINT32(kept - 1U, held());      /* the pixels went back */
    }
    return ok;
}

/** @brief The words for a picture of w by h that does not fit, on a screen of body height h_max. */
static void expect_unfit(uint32_t w, uint32_t h, uint32_t h_max)
{
    char want[160];

    (void)snprintf(want, sizeof(want),
                   "flash/a.png is %" PRIu32 " x %" PRIu32 ": an image is 410 x 502 (the glass) or up to 346 x %"
                   PRIu32 " on this screen", w, h, h_max);
    TEST_ASSERT_EQUAL_STRING(want, kode_error());
}

TEST_CASE("an image is the glass or fits the body, with a title or without", "[kode_image]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("Title"));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_TRUE(image_accepted(KODE_CANVAS_BODY_W, KODE_CANVAS_BODY_H_TITLE));
    TEST_ASSERT_TRUE(kode_screen_new("Title"));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_FALSE(image_accepted(KODE_CANVAS_BODY_W + 1U, 10U));
    expect_unfit(KODE_CANVAS_BODY_W + 1U, 10U, KODE_CANVAS_BODY_H_TITLE);
    TEST_ASSERT_FALSE(image_accepted(10U, KODE_CANVAS_BODY_H_TITLE + 1U));
    expect_unfit(10U, KODE_CANVAS_BODY_H_TITLE + 1U, KODE_CANVAS_BODY_H_TITLE);
    TEST_ASSERT_FALSE(image_accepted(KODE_CANVAS_WIDTH_MAX, KODE_CANVAS_HEIGHT_MAX - 1U));
    TEST_ASSERT_FALSE(image_accepted(KODE_CANVAS_BODY_W, KODE_CANVAS_BODY_H_BARE));
    TEST_ASSERT_TRUE(image_accepted(1U, 1U));
    TEST_ASSERT_TRUE(image_accepted(KODE_CANVAS_WIDTH_MAX, KODE_CANVAS_HEIGHT_MAX));
    uint32_t w = 0U;
    uint32_t h = 0U;

    TEST_ASSERT_NOT_NULL(kode_canvasbuf_image_view(s_canvas, &w, &h));
    TEST_ASSERT_EQUAL_UINT32(410U, w);
    TEST_ASSERT_EQUAL_UINT32(502U, h);

    /* No header: the body is taller. */
    TEST_ASSERT_TRUE(kode_screen_new(NULL));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_FALSE(image_accepted(KODE_CANVAS_BODY_W, KODE_CANVAS_BODY_H_BARE + 1U));
    expect_unfit(KODE_CANVAS_BODY_W, KODE_CANVAS_BODY_H_BARE + 1U, KODE_CANVAS_BODY_H_BARE);
    TEST_ASSERT_TRUE(image_accepted(KODE_CANVAS_BODY_W, KODE_CANVAS_BODY_H_BARE));

    /* Under a value with no unit, the room its number leaves. */
    const uint32_t under = KODE_CANVAS_BODY_H_TITLE - KODE_SCREEN_NUMBER_H - KODE_SCREEN_GAP_H;

    TEST_ASSERT_TRUE(kode_screen_new("Pic"));
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, kode_screen_add_value("1", NULL));
    TEST_ASSERT_TRUE(seen_wait(2U));
    TEST_ASSERT_FALSE(image_accepted(KODE_CANVAS_BODY_W, under + 1U));
    expect_unfit(KODE_CANVAS_BODY_W, under + 1U, under);
    TEST_ASSERT_TRUE(image_accepted(KODE_CANVAS_BODY_W, under));
}

TEST_CASE("a screen holds KODE_IMAGE_MAX images, and they count among its widgets", "[kode_image]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("Many"));
    for ( uint32_t i = 0U; KODE_IMAGE_MAX > i; i++ )
    {
        TEST_ASSERT_TRUE(image_accepted(4U, 4U));
    }
    TEST_ASSERT_FALSE(image_accepted(4U, 4U));
    TEST_ASSERT_EQUAL_STRING("a screen holds at most 4 images", kode_error());
    /* Canvases are counted apart. */
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, kode_screen_add_canvas(4U, 4U));
    TEST_ASSERT_TRUE(seen_wait(1U));

    TEST_ASSERT_TRUE(kode_screen_new("Full"));
    TEST_ASSERT_TRUE(seen_wait(1U));
    for ( uint32_t i = 0U; (KODE_WIDGETS_MAX - 1U) > i; i++ )
    {
        TEST_ASSERT_NOT_EQUAL_UINT32(0U, kode_screen_add_text("widget"));
    }
    TEST_ASSERT_TRUE(seen_wait(KODE_WIDGETS_MAX - 1U));
    TEST_ASSERT_TRUE(image_accepted(KODE_CANVAS_WIDTH_MAX, KODE_CANVAS_HEIGHT_MAX));  /* the 32nd widget, on the glass */
    TEST_ASSERT_FALSE(image_accepted(KODE_CANVAS_WIDTH_MAX, KODE_CANVAS_HEIGHT_MAX));
    TEST_ASSERT_EQUAL_STRING("a screen holds at most 32 widgets", kode_error());
}

TEST_CASE("an image's pixels are freed once, after the screen has let go", "[kode_image]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("Pic"));
    TEST_ASSERT_TRUE(image_accepted(20U, 20U));
    uint32_t w = 0U;
    uint32_t h = 0U;

    fake_task_slot_watch((void *)(uintptr_t)kode_canvasbuf_image_view(s_canvas, &w, &h));
    atomic_store(&s_gated, true);
    TEST_ASSERT_TRUE(kode_screen_new("Away"));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)fake_task_slot_watch_frees());
    TEST_ASSERT_TRUE(gate_open(1U));
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)fake_task_slot_watch_frees());
    TEST_ASSERT_TRUE(kode_screen_new("Again"));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)fake_task_slot_watch_frees());
}

TEST_CASE("an image that cannot be posted is given back at once", "[kode_image]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("Busy"));
    const kode_widget_t p = kode_screen_add_text("0");

    TEST_ASSERT_TRUE(seen_wait(2U));
    atomic_store(&s_gated, true);
    fake_time_reset();
    uint32_t taken = 0U;

    while ( (FILL_CALLS > taken) && kode_widget_set_text(p, "%" PRIu32, taken) )
    {
        taken++;
    }
    TEST_ASSERT_TRUE(FILL_CALLS > taken);

    kode_png_t     png  = picture(30U, 30U);
    const uint32_t kept = held();

    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_image_add("flash/a.png", &png));
    TEST_ASSERT_EQUAL_STRING("the screen is busy", kode_error());
    TEST_ASSERT_NULL(png.rgba);
    TEST_ASSERT_EQUAL_UINT32(kept - 1U, held());
    TEST_ASSERT_TRUE(gate_open(taken));

    /* The place it would have had is free for the next widget. */
    TEST_ASSERT_TRUE(image_accepted(5U, 5U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_last.widget);
}

TEST_CASE("an image with no memory for its record gives the pixels back", "[kode_image]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("Memory"));
    TEST_ASSERT_TRUE(seen_wait(1U));
    kode_png_t     png    = picture(8U, 8U);
    const uint32_t before = atomic_load(&s_req_n);
    const uint32_t kept   = held();

    fake_psram_refuse(1U);
    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_image_add("flash/a.png", &png));
    TEST_ASSERT_EQUAL_STRING("flash/a.png: out of memory", kode_error());
    TEST_ASSERT_NULL(png.rgba);
    TEST_ASSERT_EQUAL_UINT32(kept - 1U, held());
    TEST_ASSERT_EQUAL_UINT32(before, atomic_load(&s_req_n));
}

TEST_CASE("an image of an older screen is no image of the new one", "[kode_image]")
{
    screen_case();

    TEST_ASSERT_TRUE(kode_screen_new("One"));
    kode_png_t png = picture(8U, 8U);
    const kode_widget_t old = kode_screen_image_add("flash/a.png", &png);

    TEST_ASSERT_NOT_EQUAL_UINT32(0U, old);
    TEST_ASSERT_TRUE(seen_wait(2U));
    TEST_ASSERT_TRUE(kode_screen_new("Two"));
    TEST_ASSERT_TRUE(seen_wait(1U));
    TEST_ASSERT_FALSE(kode_widget_set_text(old, "x"));
    TEST_ASSERT_EQUAL_STRING("no such widget", kode_error());
}

TEST_CASE("an image or a PNG drawn with no path is refused", "[kode_image]")
{
    screen_case();

    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_image(NULL));
    TEST_ASSERT_EQUAL_STRING("path is NULL", kode_error());
    const kode_canvas_t c = canvas_screen(8U, 8U);

    TEST_ASSERT_FALSE(kode_draw_png(c, 0, 0, NULL));
    TEST_ASSERT_EQUAL_STRING("path is NULL", kode_error());
    TEST_ASSERT_FALSE(kode_draw_png(c, KODE_DRAW_RANGE + 1, 0, "flash/a.png"));
    TEST_ASSERT_EQUAL_STRING("a coordinate, size or radius is beyond 32767 either way", kode_error());
}

/* ---- The compass --------------------------------------------------------------- */

#define SWEEP_STEP_MG   100      /**< How far each scripted reading moves every axis.      */
#define SWEEP_READS     5U       /**< Readings of a ramp: 0 to 400 mG.                     */
#define FULL_READS      6U       /**< Readings of the full sweep: the sphere's six poles.  */
#define FULL_RADIUS_MG  200      /**< The full sweep's sphere: its radius, mG.             */
#define SWEEP_TIME_READS 600U    /**< Readings that fit in the 30 s at 50 ms each.         */
#define SWEEP_FAIL_AT   3U       /**< The reading the failing script gives up at.          */
#define SWEEP_FAR_MG    19800    /**< Where the too-strong script starts on x, mG.         */
#define SWEEP_SLACK     2U       /**< Readings the clock may add to a count.               */

/** @brief Readings of a full sweep that then holds still until it has settled. */
#define SWEEP_SETTLED_READS (FULL_READS + (KODE_COMPASS_SETTLE_MS / KODE_COMPASS_STEP_MS))

#define FIELD_MG        450.0    /**< The synthetic Earth field, mG: Madrid's.             */
#define FIELD_OFF_X     150      /**< Its hard-iron offset on x, mG.                       */
#define FIELD_OFF_Y     (-90)    /**< On y.                                                */
#define FIELD_OFF_Z     60       /**< On z.                                                */
#define FIELD_NOISE_MG  5U       /**< Each reading's noise, up to this either way, mG.     */
#define FIELD_POINTS    300U     /**< Readings of one spiral from pole to pole.            */
#define FIELD_GOLDEN    2.39996323  /**< The golden angle, rad: spreads the spiral evenly. */
#define FIELD_TOL_MG    50       /**< How far a stored offset may be from the truth, mG.   */
#define FIELD_BAND_RAD  0.5235987756  /**< The band's tilt either side of the equator, rad (30 deg). */
#define FIELD_BAND_WAVE 37U      /**< Readings of one tilt up and down the band.           */
#define FIELD_SPIN_RAD  0.21     /**< The band's turn about z between two readings, rad.   */
#define FIELD_TURN_RAD  6.283185307  /**< A whole turn, rad.                               */
#define FIELD_CONE_RAD  1.0471975512 /**< The cone's half-angle about the diagonal, rad (60 deg). */
#define FIELD_HEMI_RAD  1.5707963268 /**< The hemisphere's, rad (90 deg).                  */
#define FIELD_ROOT2     1.4142135624 /**< The square root of 2.                            */
#define FIELD_ROOT3     1.7320508076 /**< The square root of 3.                            */
#define FIELD_ROOT6     2.4494897428 /**< The square root of 6.                            */
#define NOISE_MUL       1664525U     /**< The noise's linear congruential multiplier.      */
#define NOISE_ADD       1013904223U  /**< And its increment.                               */
#define NOISE_SHIFT     16U          /**< The state's high bits are the noisier ones.      */

/** @brief What the scripted io does. */
typedef enum {
    SCRIPT_FULL = 0,   /**< The six poles of a sphere about (200, -200, 200), then holds. */
    SCRIPT_FLAT_Z,     /**< x and y sweep, z stays put.                          */
    SCRIPT_FAILS,      /**< The reading at SWEEP_FAIL_AT fails.                  */
    SCRIPT_FAR,        /**< x sweeps about 20000 mG: past what is stored.        */
    SCRIPT_SPHERE,     /**< A field with an offset, turned through a sphere.     */
    SCRIPT_BAND,       /**< The same field, turned only through a band about z.  */
    SCRIPT_CONE,       /**< The same field, turned through a 60 deg cone about (1, 1, 1). */
    SCRIPT_HEMI        /**< The same field, turned through the hemisphere about (1, 1, 1). */
} script_t;

static script_t            s_script;         /**< The case's script.                    */
static uint32_t            s_reads;          /**< Readings asked for.                   */
static uint32_t            s_stores;         /**< Stores asked for.                     */
static bool                s_store_ok;       /**< The store succeeds.                   */
static kdrv_qmc5883p_cal_t s_stored;         /**< What was stored last.                 */
static uint32_t            s_noise;          /**< The noise's state.                    */

/** @brief The next noise, up to FIELD_NOISE_MG either way. */
static int32_t noise_next(void)
{
    s_noise = (s_noise * NOISE_MUL) + NOISE_ADD;
    return (int32_t)((s_noise >> NOISE_SHIFT) % ((2U * FIELD_NOISE_MG) + 1U)) - (int32_t)FIELD_NOISE_MG;
}

/**
 * @brief The synthetic field at reading @p k turned through a cap about (1, 1, 1),
 *        evenly by area, plus the offset and noise.
 * @param k  The reading, from 0.
 * @param mg Out: the field, mG.
 */
static void cap_read(uint32_t k, int32_t mg[KDRV_QMC5883P_AXES])
{
    static const int32_t off[KDRV_QMC5883P_AXES] = { FIELD_OFF_X, FIELD_OFF_Y, FIELD_OFF_Z };
    static const double  d[KDRV_QMC5883P_AXES]   = { 1.0 / FIELD_ROOT3, 1.0 / FIELD_ROOT3, 1.0 / FIELD_ROOT3 };
    static const double  u[KDRV_QMC5883P_AXES]   = { 1.0 / FIELD_ROOT2, -1.0 / FIELD_ROOT2, 0.0 };
    static const double  v[KDRV_QMC5883P_AXES]   = { 1.0 / FIELD_ROOT6, 1.0 / FIELD_ROOT6, -2.0 / FIELD_ROOT6 };
    const uint32_t       at   = k % FIELD_POINTS;
    double               half = FIELD_CONE_RAD;

    if ( SCRIPT_HEMI == s_script )
    {
        half = FIELD_HEMI_RAD;
    }
    const double c    = 1.0 - ((1.0 - cos(half)) * (double)at / (double)(FIELD_POINTS - 1U));
    const double side = sqrt(1.0 - (c * c));
    const double turn = (double)at * FIELD_GOLDEN;

    for ( uint32_t i = 0U; KDRV_QMC5883P_AXES > i; i++ )
    {
        const double dir = (c * d[i]) + (side * cos(turn) * u[i]) + (side * sin(turn) * v[i]);

        mg[i] = (int32_t)lround(FIELD_MG * dir) + off[i] + noise_next();
    }
}

/**
 * @brief The synthetic field at reading @p k: the Earth's turned, plus the offset and noise.
 * @param k  The reading, from 0.
 * @param mg Out: the field, mG.
 */
static void field_read(uint32_t k, int32_t mg[KDRV_QMC5883P_AXES])
{
    static const int32_t off[KDRV_QMC5883P_AXES] = { FIELD_OFF_X, FIELD_OFF_Y, FIELD_OFF_Z };
    double               polar;
    double               turn;

    if ( SCRIPT_SPHERE == s_script )
    {
        const uint32_t at = k % FIELD_POINTS;

        polar = acos(1.0 - ((2.0 * (double)at) / (double)(FIELD_POINTS - 1U)));
        turn  = (double)at * FIELD_GOLDEN;
    }
    else
    {
        polar = (FIELD_TURN_RAD / 4.0)
                + (FIELD_BAND_RAD * sin((FIELD_TURN_RAD * (double)(k % FIELD_BAND_WAVE)) / (double)FIELD_BAND_WAVE));
        turn  = (double)k * FIELD_SPIN_RAD;
    }
    mg[0] = (int32_t)lround(FIELD_MG * sin(polar) * cos(turn)) + off[0] + noise_next();
    mg[1] = (int32_t)lround(FIELD_MG * sin(polar) * sin(turn)) + off[1] + noise_next();
    mg[2] = (int32_t)lround(FIELD_MG * cos(polar)) + off[2] + noise_next();
}

/** @brief The scripted readings. */
static bool script_read(int32_t mg[KDRV_QMC5883P_AXES])
{
    uint32_t at = s_reads;
    bool     ok = true;

    s_reads++;
    if ( (SCRIPT_CONE == s_script) || (SCRIPT_HEMI == s_script) )
    {
        cap_read(at, mg);
    }
    else if ( (SCRIPT_SPHERE == s_script) || (SCRIPT_BAND == s_script) )
    {
        field_read(at, mg);
    }
    else if ( SCRIPT_FULL == s_script )
    {
        /* +x, -x, +y, -y, +z, -z about (200, -200, 200); a sweep that is over keeps its last reading */
        static const int32_t centre[KDRV_QMC5883P_AXES] = { FULL_RADIUS_MG, -FULL_RADIUS_MG, FULL_RADIUS_MG };
        uint32_t             pole = at;

        if ( FULL_READS <= pole )
        {
            pole = FULL_READS - 1U;
        }
        for ( uint32_t i = 0U; KDRV_QMC5883P_AXES > i; i++ )
        {
            mg[i] = centre[i];
        }
        if ( (pole % 2U) == 0U )
        {
            mg[pole / 2U] += FULL_RADIUS_MG;
        }
        else
        {
            mg[pole / 2U] -= FULL_RADIUS_MG;
        }
    }
    else
    {
        if ( (SCRIPT_FLAT_Z != s_script) && (SWEEP_READS <= at) )
        {
            at = SWEEP_READS - 1U;   /* a sweep that is over keeps its last reading */
        }
        mg[0] = (int32_t)at * SWEEP_STEP_MG;
        mg[1] = -(int32_t)at * SWEEP_STEP_MG;
        mg[2] = (int32_t)at * SWEEP_STEP_MG;
        if ( SCRIPT_FLAT_Z == s_script )
        {
            mg[2] = 0;
        }
        if ( SCRIPT_FAR == s_script )
        {
            mg[0] += SWEEP_FAR_MG;
        }
        if ( (SCRIPT_FAILS == s_script) && (SWEEP_FAIL_AT == s_reads) )
        {
            kode_err_set("magnetometer: the magnetometer did not answer");
            ok = false;
        }
    }
    return ok;
}

/** @brief The scripted store. */
static bool script_store(const kdrv_qmc5883p_cal_t *corr)
{
    s_stores++;
    s_stored = *corr;
    if ( !s_store_ok )
    {
        kode_err_set("magnetometer: the calibration could not be stored");
    }
    return s_store_ok;
}

/** @brief A fresh screen and a script. */
static void compass_case(script_t script)
{
    screen_case();
    s_script   = script;
    s_reads    = 0U;
    s_stores   = 0U;
    s_store_ok = true;
    s_noise    = 1U;
}

/** @brief Run the scripted calibration. */
static bool compass_run(void)
{
    static const kode_compass_io_t io = { &script_read, &script_store };

    return kode_compass_run(&io);
}

TEST_CASE("a full sweep stores the fit once it has settled and shows its progress", "[kode_compass]")
{
    compass_case(SCRIPT_FULL);

    TEST_ASSERT_TRUE(compass_run());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_UINT32_WITHIN(SWEEP_SLACK, SWEEP_SETTLED_READS, s_reads);
    TEST_ASSERT_EQUAL_UINT32(1U, s_stores);
    /* Centres 200, -200, 200; half 200 each. */
    TEST_ASSERT_EQUAL_INT32(200, s_stored.offset_mg[0]);
    TEST_ASSERT_EQUAL_INT32(-200, s_stored.offset_mg[1]);
    TEST_ASSERT_EQUAL_INT32(200, s_stored.offset_mg[2]);
    for ( uint32_t i = 0U; KDRV_QMC5883P_AXES > i; i++ )
    {
        TEST_ASSERT_EQUAL_INT32(KDRV_QMC5883P_CAL_SCALE_UNIT, s_stored.scale_milli[i]);
    }

    /* The screen, its text, its value, the progress as z joins in, and the last words. */
    TEST_ASSERT_TRUE(seen_wait(6U));
    TEST_ASSERT_EQUAL_UINT32(3U, atomic_load(&s_set_n));
    TEST_ASSERT_EQUAL_STRING("62", s_set_log[0]);
    TEST_ASSERT_EQUAL_STRING("100", s_set_log[1]);
    TEST_ASSERT_EQUAL_STRING("Compass ready", s_set_log[2]);
}

TEST_CASE("a field turned through a sphere stores its offset within the tolerance", "[kode_compass]")
{
    compass_case(SCRIPT_SPHERE);
    const uint32_t kept = held();

    TEST_ASSERT_TRUE(compass_run());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(kept, held(), "the readings' block was not given back");
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_stores);
    TEST_ASSERT_LESS_THAN_UINT32(SWEEP_TIME_READS, s_reads);
    TEST_ASSERT_INT32_WITHIN(FIELD_TOL_MG, FIELD_OFF_X, s_stored.offset_mg[0]);
    TEST_ASSERT_INT32_WITHIN(FIELD_TOL_MG, FIELD_OFF_Y, s_stored.offset_mg[1]);
    TEST_ASSERT_INT32_WITHIN(FIELD_TOL_MG, FIELD_OFF_Z, s_stored.offset_mg[2]);
}

TEST_CASE("a field turned only through a band about z fails after the time limit", "[kode_compass]")
{
    const uint32_t t0 = kode_uptime_ms();

    compass_case(SCRIPT_BAND);

    TEST_ASSERT_FALSE(compass_run());
    TEST_ASSERT_EQUAL_STRING("not enough directions were covered", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_stores);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_MAG_CALIBRATE_MS, kode_uptime_ms() - t0);
}

TEST_CASE("the screen asks for the Dot to be turned and shows a value", "[kode_compass]")
{
    compass_case(SCRIPT_FULL);

    TEST_ASSERT_TRUE(compass_run());
    TEST_ASSERT_TRUE(seen_wait(6U));
    TEST_ASSERT_EQUAL_STRING("Compass", s_op_text[KODE_SCREEN_OP_NEW]);
    TEST_ASSERT_EQUAL_STRING("Turn the Dot in every direction", s_op_text[KODE_SCREEN_OP_TEXT]);
    TEST_ASSERT_EQUAL_STRING("0", s_op_text[KODE_SCREEN_OP_VALUE]);
}

TEST_CASE("a sweep that leaves an axis flat fails after the time limit", "[kode_compass]")
{
    const uint32_t t0 = kode_uptime_ms();

    compass_case(SCRIPT_FLAT_Z);

    TEST_ASSERT_FALSE(compass_run());
    TEST_ASSERT_EQUAL_STRING("not enough directions were covered", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_stores);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_MAG_CALIBRATE_MS, kode_uptime_ms() - t0);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(SWEEP_TIME_READS, s_reads);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(SWEEP_TIME_READS + SWEEP_SLACK, s_reads);
    TEST_ASSERT_TRUE(seen_wait(4U));
    TEST_ASSERT_EQUAL_STRING("Try again", s_set_log[atomic_load(&s_set_n) - 1U]);
}

TEST_CASE("a field turned through a 60 degree cone about the diagonal is refused", "[kode_compass]")
{
    compass_case(SCRIPT_CONE);
    const uint32_t kept = held();

    TEST_ASSERT_FALSE(compass_run());
    TEST_ASSERT_EQUAL_STRING("not enough directions were covered", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_stores);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(kept, held(), "the readings' block was not given back");
}

TEST_CASE("a field turned through the hemisphere about the diagonal is refused", "[kode_compass]")
{
    compass_case(SCRIPT_HEMI);

    TEST_ASSERT_FALSE(compass_run());
    TEST_ASSERT_EQUAL_STRING("not enough directions were covered", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_stores);
}

TEST_CASE("a calibration with no memory for its readings fails and reads nothing", "[kode_compass]")
{
    compass_case(SCRIPT_SPHERE);
    fake_psram_refuse(1U);

    TEST_ASSERT_FALSE(compass_run());
    TEST_ASSERT_EQUAL_STRING("no memory for the calibration", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_reads);
    TEST_ASSERT_EQUAL_UINT32(0U, s_stores);
    TEST_ASSERT_TRUE(seen_wait(4U));
    TEST_ASSERT_EQUAL_STRING("Try again", s_set_log[atomic_load(&s_set_n) - 1U]);
}

TEST_CASE("a reading that fails ends the calibration with its reason", "[kode_compass]")
{
    compass_case(SCRIPT_FAILS);

    TEST_ASSERT_FALSE(compass_run());
    TEST_ASSERT_EQUAL_STRING("magnetometer: the magnetometer did not answer", kode_error());
    TEST_ASSERT_EQUAL_UINT32(SWEEP_FAIL_AT, s_reads);
    TEST_ASSERT_EQUAL_UINT32(0U, s_stores);
}

TEST_CASE("a store that fails ends the calibration with its reason", "[kode_compass]")
{
    compass_case(SCRIPT_FULL);
    s_store_ok = false;

    TEST_ASSERT_FALSE(compass_run());
    TEST_ASSERT_EQUAL_STRING("magnetometer: the calibration could not be stored", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_stores);
}

TEST_CASE("a field too strong to correct is not stored", "[kode_compass]")
{
    compass_case(SCRIPT_FAR);

    TEST_ASSERT_FALSE(compass_run());
    TEST_ASSERT_EQUAL_STRING("the field is too strong to calibrate", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_stores);
}

/* ---- The screen's brightness ---------------------------------------------------- */

/** @brief The brightness the cases below ask for: inside the lit range. */
#define BRIGHT_TEST_LEVEL 40U

/** @brief Pool blocks the dry-pool case holds at most. */
#define BRIGHT_HOLD_MAX 64U

/** @brief The bound a brightness call waits for a free event, in ms (the layer's). */
#define BRIGHT_BOUND_MS 100U

/** @brief Events waiting in AO_Display's queue. */
static uint32_t bright_count(void)
{
    return (uint32_t)uxQueueMessagesWaiting(AO_Display->eQueue);
}

/** @brief Recycle everything waiting in AO_Display's queue. */
static void bright_drain(void)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_Display->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief A system that is up, a clean clock and AO_Display's queue empty. */
static void bright_fresh(void)
{
    qf_support_init();
    qf_support_margin_queue(AO_Display);
    bright_drain();
    system_is_up();
    fake_time_reset();
}

/** @brief The pool blocks a case holds so the call finds none. */
static BrightnessEvt *s_bright_held[BRIGHT_HOLD_MAX];

/** @brief How many of s_bright_held are in hand. */
static unsigned s_bright_held_n;

/** @brief Take every pool block there is. */
static void bright_pool_exhaust(void)
{
    BrightnessEvt *e = Q_NEW_X(BrightnessEvt, 0U, DISPLAY_BRIGHTNESS_SIG);

    s_bright_held_n = 0U;
    while ( (NULL != e) && (BRIGHT_HOLD_MAX > s_bright_held_n) )
    {
        s_bright_held[s_bright_held_n] = e;
        ++s_bright_held_n;
        e = Q_NEW_X(BrightnessEvt, 0U, DISPLAY_BRIGHTNESS_SIG);
    }
}

/** @brief Give one block back: the pool has room for one event. */
static void bright_pool_free_one(void)
{
    if ( 0U < s_bright_held_n )
    {
        --s_bright_held_n;
        QF_gc(&s_bright_held[s_bright_held_n]->super);
    }
}

/** @brief Give every block still in hand back. */
static void bright_pool_free_all(void)
{
    while ( 0U < s_bright_held_n )
    {
        bright_pool_free_one();
    }
}

TEST_CASE("the screen's brightness is posted to AO_Display and never stored", "[kode_screen_bright]")
{
    const QEvt *e = NULL;

    bright_fresh();

    TEST_ASSERT_TRUE(kode_display_set_brightness(BRIGHT_TEST_LEVEL));
    TEST_ASSERT_EQUAL_UINT32(1U, bright_count());
    TEST_ASSERT_EQUAL_INT(pdTRUE, xQueueReceive(AO_Display->eQueue, &e, 0U));
    TEST_ASSERT_EQUAL_INT(DISPLAY_BRIGHTNESS_SIG, e->sig);
    TEST_ASSERT_EQUAL_UINT8(BRIGHT_TEST_LEVEL, ((const BrightnessEvt *)(const void *)e)->percent);
    TEST_ASSERT_FALSE(((const BrightnessEvt *)(const void *)e)->persist);
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    QF_gc(e);
}

TEST_CASE("the screen can be darkened with 0 and lit from the lowest level to 100", "[kode_screen_bright]")
{
    bright_fresh();

    TEST_ASSERT_TRUE(kode_display_set_brightness(0U));
    TEST_ASSERT_TRUE(kode_display_set_brightness(KODE_DISPLAY_BRIGHTNESS_MIN));
    TEST_ASSERT_TRUE(kode_display_set_brightness(KODE_DISPLAY_BRIGHTNESS_MAX));
    TEST_ASSERT_EQUAL_UINT32(3U, bright_count());
    bright_drain();
}

TEST_CASE("a brightness between dark and the lowest lit level is refused, naming the range", "[kode_screen_bright]")
{
    bright_fresh();

    TEST_ASSERT_FALSE(kode_display_set_brightness(3U));
    TEST_ASSERT_EQUAL_STRING("brightness out of range: use 0, or 5 to 100", kode_error());
    TEST_ASSERT_FALSE(kode_display_set_brightness(KODE_DISPLAY_BRIGHTNESS_MAX + 1U));
    TEST_ASSERT_EQUAL_STRING("brightness out of range: use 0, or 5 to 100", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, bright_count());
}

TEST_CASE("the brightness waits for a free event and goes through", "[kode_screen_bright]")
{
    bright_fresh();
    bright_pool_exhaust();
    fake_time_delay_hook(&bright_pool_free_one);

    TEST_ASSERT_TRUE(kode_display_set_brightness(BRIGHT_TEST_LEVEL));
    TEST_ASSERT_EQUAL_UINT32(1U, bright_count());
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    bright_drain();
    bright_pool_free_all();
}

TEST_CASE("a brightness that never finds a free event fails after its bound", "[kode_screen_bright]")
{
    bright_fresh();
    bright_pool_exhaust();

    TEST_ASSERT_FALSE(kode_display_set_brightness(BRIGHT_TEST_LEVEL));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(BRIGHT_BOUND_MS, kode_sys_time_ms());
    TEST_ASSERT_EQUAL_UINT32(0U, bright_count());
    TEST_ASSERT_EQUAL_STRING("the screen did not take the brightness", kode_error());
    bright_pool_free_all();
}
