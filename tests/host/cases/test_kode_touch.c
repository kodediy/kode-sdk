/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_touch.c
 * @brief The finger in the standard layer: touch events, kode_touch_read() and coalesced moves.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "fakes.h"
#include "kaos.h"
#include "kode.h"
#include "kode_entry.h"
#include "kode_standard_ao.h"
#include "kode_sys_priv.h"
#include "kqp.h"
#include "qf_support.h"
#include "standard_ao_support.h"

/** @brief The longest a case waits for a listener, in ms. */
#define TEST_WAIT_MS 4000U

/** @brief Calls a case can record. */
#define REC_MAX 64U

/** @brief Longest name a record keeps. */
#define REC_TEXT 32U

/** @brief The moves the coalescing case sends while the listener is held. */
#define DRAG_MOVES 50U

/** @brief The move after which the coalescing case pushes a button. */
#define DRAG_BUTTON_AT 25U

/** @brief The samples the screen case sends while the listener is held. */
#define SCREEN_SAMPLES 6U

/** @brief Waiting events in the Active Object's queue above which the case lets it run. */
#define PACE_HIGH 8U

/** @brief Longest a pacing loop waits, in ticks of 1. */
#define PACE_SPINS 400U

/** @brief Where the press of the cases lands: x, in glass pixels. */
#define PRESS_X 10

/** @brief Where the press of the cases lands: y, in glass pixels. */
#define PRESS_Y 20

/** @brief What a listener saw. */
typedef struct {
    char    name[REC_TEXT];   /**< The event's name. */
    char    source[REC_TEXT]; /**< Its source.       */
    int32_t value;            /**< Its value.        */
    int16_t x;                /**< Its point's x.    */
    int16_t y;                /**< Its point's y.    */
} rec_t;

static rec_t             s_rec[REC_MAX];  /**< What the listeners saw, in order.   */
static volatile uint32_t s_rec_n;         /**< How many records.                    */
static SemaphoreHandle_t s_given;         /**< Given by a listener per event.       */
static SemaphoreHandle_t s_gate_in;       /**< Given when a gate listener starts.   */
static SemaphoreHandle_t s_gate_open;     /**< Each give lets one gate listener go. */

/** @brief Record the event. */
static void record(const kode_event_t *e)
{
    const uint32_t i = s_rec_n;

    if ( REC_MAX > i )
    {
        (void)snprintf(s_rec[i].name, REC_TEXT, "%s", e->name);
        (void)snprintf(s_rec[i].source, REC_TEXT, "%s", e->source);
        s_rec[i].value = e->value;
        s_rec[i].x     = e->x;
        s_rec[i].y     = e->y;
    }
    s_rec_n = i + 1U;
}

/** @brief Record the event and tell the case. */
static void listener_rec(const kode_event_t *e)
{
    record(e);
    (void)xSemaphoreGive(s_given);
}

/** @brief Record the event, then hold the dispatch task until the case lets it go. */
static void listener_gate(const kode_event_t *e)
{
    record(e);
    (void)xSemaphoreGive(s_gate_in);
    (void)xSemaphoreTake(s_gate_open, pdMS_TO_TICKS(TEST_WAIT_MS));
}

/** @brief Publish a sample the way AO_Touch does. */
static void publish_touch(int16_t x, int16_t y, bool pressed)
{
    TouchEvt *const e = Q_NEW(TouchEvt, TOUCH_EVT_SIG);

    e->x       = x;
    e->y       = y;
    e->pressed = pressed;
    e->t_ms    = 0U;
    QACTIVE_PUBLISH(&e->super, (void *)0);
}

/** @brief Let the Active Object catch up when its queue grows past PACE_HIGH. */
static void pace(void)
{
    for ( uint32_t spin = 0U; (PACE_SPINS > spin) &&
         (uxQueueMessagesWaiting(AO_KodeStandard->eQueue) >= PACE_HIGH); spin++ )
    {
        vTaskDelay(1U);
    }
}

/** @brief The QP registry of Active Objects, indexed by priority. */
extern QActive *QActive_registry_[QF_MAX_ACTIVE + 1U];

/** @brief The QP subscriber sets, one per published signal. */
extern QSubscrList *QActive_subscrList_;

/** @brief Whether the Active Object at a priority was on TOUCH_EVT_SIG before the file took it off. */
static bool s_was[QF_MAX_ACTIVE + 1U];

/** @brief Whether the other Active Objects were taken off the bus already. */
static bool s_others_off;

/** @brief Whether the layer's Active Object hears TOUCH_EVT_SIG. */
static bool touch_subscribed(void)
{
    return QPSet_hasElement(&QActive_subscrList_[TOUCH_EVT_SIG].set,
                            (uint_fast8_t)AO_KodeStandard->prio);
}

/**
 * @brief Take every other Active Object off TOUCH_EVT_SIG, once.
 *
 * Their queues would hold every sample this file publishes until the event
 * pool is empty; restore_others() puts the subscriptions back.
 */
static void others_off_the_bus(void)
{
    if ( !s_others_off )
    {
        s_others_off = true;
        for ( uint32_t p = 1U; QF_MAX_ACTIVE >= p; p++ )
        {
            QActive *const ao = QActive_registry_[p];

            if ( (NULL != ao) && (ao != AO_KodeStandard) &&
                QPSet_hasElement(&QActive_subscrList_[TOUCH_EVT_SIG].set, (uint_fast8_t)p) )
            {
                s_was[p] = true;
                QActive_unsubscribe(ao, TOUCH_EVT_SIG);
            }
        }
    }
}

/** @brief Subscribe again the Active Objects others_off_the_bus() took off. */
static void restore_others(void)
{
    for ( uint32_t p = 1U; QF_MAX_ACTIVE >= p; p++ )
    {
        if ( s_was[p] )
        {
            QActive_subscribe(QActive_registry_[p], TOUCH_EVT_SIG);
            s_was[p] = false;
        }
    }
    s_others_off = false;
}

/** @brief Start the layer with real locks and reset what the cases share. */
static void events_up(void)
{
    qf_support_init();
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    fake_sync_real(true);
    TEST_ASSERT_TRUE(kode_standard_ao_ensure());
    others_off_the_bus();
    if ( NULL == s_given )
    {
        s_given     = xSemaphoreCreateCounting(REC_MAX, 0U);
        s_gate_in   = xSemaphoreCreateCounting(REC_MAX, 0U);
        s_gate_open = xSemaphoreCreateCounting(REC_MAX, 0U);
    }
    while ( xSemaphoreTake(s_given, 0U) == pdTRUE )
    {
    }
    while ( xSemaphoreTake(s_gate_in, 0U) == pdTRUE )
    {
    }
    while ( xSemaphoreTake(s_gate_open, 0U) == pdTRUE )
    {
    }
    (void)memset(s_rec, 0, sizeof(s_rec));
    s_rec_n = 0U;
}

/** @brief Wait for @p n more calls of listener_rec(); false if they do not all come. */
static bool rec_wait(uint32_t n)
{
    bool ok = true;

    for ( uint32_t i = 0U; (i < n) && ok; i++ )
    {
        ok = (xSemaphoreTake(s_given, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    }
    return ok;
}

/** @brief Assert record @p i is @p name at (@p x, @p y). */
static void rec_is(uint32_t i, const char *name, int16_t x, int16_t y)
{
    TEST_ASSERT_TRUE(i < s_rec_n);
    TEST_ASSERT_EQUAL_STRING(name, s_rec[i].name);
    TEST_ASSERT_EQUAL_INT16(x, s_rec[i].x);
    TEST_ASSERT_EQUAL_INT16(y, s_rec[i].y);
}

/**
 * @brief Push a barrier behind everything queued and wait for it: once it is
 *        heard, every event before it has been delivered.
 */
static void barrier(void)
{
    TEST_ASSERT_TRUE(kode_event_add_listener("button_up_released", &listener_rec));
    kode_event_push("button_up_released", "", 0, "");
    TEST_ASSERT_TRUE(rec_wait(1U));
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_up_released", &listener_rec));
}

/** @brief Blocks the cases can hold: more than the small pool has. */
#define POOL_HOLD_MAX 256U

static TouchEvt *s_held[POOL_HOLD_MAX];  /**< Blocks taken out of the small pool. */
static uint32_t  s_held_n;               /**< How many of s_held are in hand.     */
static bool      s_read_while_posting;   /**< What a read answered during another call's post. */
static char      s_read_error[KODE_ERROR_MAX];  /**< The error that read left. */

/** @brief Take every block of the small pool, where the subscription's request comes from. */
static void pool_exhaust(void)
{
    TouchEvt *e = Q_NEW_X(TouchEvt, 0U, TOUCH_EVT_SIG);

    s_held_n = 0U;
    while ( (NULL != e) && (POOL_HOLD_MAX > s_held_n) )
    {
        s_held[s_held_n] = e;
        s_held_n++;
        e = Q_NEW_X(TouchEvt, 0U, TOUCH_EVT_SIG);
    }
    TEST_ASSERT_NULL_MESSAGE(e, "the small pool is empty");
}

/** @brief Give every block back. */
static void pool_free_all(void)
{
    while ( 0U < s_held_n )
    {
        s_held_n--;
        QF_gc(&s_held[s_held_n]->super);
    }
}

/**
 * @brief A second caller while a listener's request is still being posted:
 *        run from inside that call's first wait for a free event.
 */
static void read_while_posting(void)
{
    s_read_while_posting = kode_touch_read(NULL, NULL);
    (void)snprintf(s_read_error, sizeof(s_read_error), "%s", kode_error());
}

/* First, on purpose: it proves that no case before this file subscribed the layer to the finger. */
TEST_CASE("no subscription to the finger exists before the first touch listener, and one after; "
          "a request that cannot be posted fails the listener, the wait and the read",
          "[kode_touch]")
{
    events_up();
    TEST_ASSERT_FALSE_MESSAGE(touch_subscribed(),
                              "nothing before a touch listener, wait or read subscribes");

    /* No free event for the request: each call fails, names what cannot be
     * heard and leaves nothing behind. */
    pool_exhaust();
    fake_time_delay_hook(&read_while_posting);
    s_read_while_posting = true;
    const bool added = kode_event_add_listener(KODE_EVENT_TOUCH_PRESSED, &listener_rec);
    char add_error[KODE_ERROR_MAX];

    (void)snprintf(add_error, sizeof(add_error), "%s", kode_error());
    kode_event_t out = { NULL, NULL, -1, NULL, -1, -1 };
    const bool waited = kode_event_wait(KODE_EVENT_TOUCH_MOVED, TEST_WAIT_MS, &out);
    char wait_error[KODE_ERROR_MAX];

    (void)snprintf(wait_error, sizeof(wait_error), "%s", kode_error());
    const bool read = kode_touch_read(NULL, NULL);
    char read_error[KODE_ERROR_MAX];

    (void)snprintf(read_error, sizeof(read_error), "%s", kode_error());
    fake_time_delay_hook(NULL);
    pool_free_all();

    TEST_ASSERT_FALSE(added);
    TEST_ASSERT_EQUAL_STRING("'touch_pressed' cannot be heard now: the standard layer is busy, try again",
                             add_error);
    TEST_ASSERT_FALSE_MESSAGE(kode_event_remove_listener(KODE_EVENT_TOUCH_PRESSED, &listener_rec),
                              "a refused listener is not left registered");
    TEST_ASSERT_FALSE_MESSAGE(s_read_while_posting,
                              "no caller answers true while the only request may yet fail");
    TEST_ASSERT_EQUAL_STRING("the touch panel cannot be heard now: the standard layer is busy, try again",
                             s_read_error);
    TEST_ASSERT_FALSE(waited);
    TEST_ASSERT_EQUAL_STRING("'touch_moved' cannot be heard now: the standard layer is busy, try again",
                             wait_error);
    TEST_ASSERT_FALSE(read);
    TEST_ASSERT_EQUAL_STRING("the touch panel cannot be heard now: the standard layer is busy, try again",
                             read_error);
    standard_ao_support_drained();
    TEST_ASSERT_FALSE(touch_subscribed());

    /* With room again, the next listener subscribes. */
    TEST_ASSERT_TRUE(kode_event_add_listener(KODE_EVENT_TOUCH_PRESSED, &listener_rec));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    standard_ao_support_drained();
    TEST_ASSERT_TRUE(touch_subscribed());
    TEST_ASSERT_TRUE(kode_event_remove_listener(KODE_EVENT_TOUCH_PRESSED, &listener_rec));
}

TEST_CASE("the three touch events are known, in any case, and named by their constants",
          "[kode_touch]")
{
    events_up();
    TEST_ASSERT_EQUAL_STRING("touch_pressed", KODE_EVENT_TOUCH_PRESSED);
    TEST_ASSERT_EQUAL_STRING("touch_moved", KODE_EVENT_TOUCH_MOVED);
    TEST_ASSERT_EQUAL_STRING("touch_released", KODE_EVENT_TOUCH_RELEASED);
    TEST_ASSERT_TRUE(kode_event_add_listener("TOUCH_Moved", &listener_rec));
    TEST_ASSERT_TRUE(kode_event_remove_listener("touch_moved", &listener_rec));
    TEST_ASSERT_FALSE(kode_event_add_listener("touch_held", &listener_rec));
    TEST_ASSERT_EQUAL_STRING("unknown event 'touch_held'", kode_error());
}

TEST_CASE("kode_touch_read is false before any touch and gives the point while pressed",
          "[kode_touch]")
{
    int16_t x = -1;
    int16_t y = -1;

    events_up();
    TEST_ASSERT_FALSE(kode_touch_read(&x, &y));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_INT16(-1, x);
    TEST_ASSERT_EQUAL_INT16(-1, y);
    TEST_ASSERT_TRUE(touch_subscribed());

    publish_touch(PRESS_X, PRESS_Y, true);
    standard_ao_support_drained();
    TEST_ASSERT_TRUE(kode_touch_read(&x, &y));
    TEST_ASSERT_EQUAL_INT16(PRESS_X, x);
    TEST_ASSERT_EQUAL_INT16(PRESS_Y, y);
    TEST_ASSERT_TRUE(kode_touch_read(NULL, NULL));

    publish_touch(PRESS_X + 1, PRESS_Y + 1, false);
    standard_ao_support_drained();
    x = -1;
    TEST_ASSERT_FALSE(kode_touch_read(&x, &y));
    TEST_ASSERT_EQUAL_INT16(-1, x);
    barrier();
}

TEST_CASE("a press, three moves and a release reach a slow listener as a press, "
          "one move at the last point and the release", "[kode_touch]")
{
    events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener(KODE_EVENT_TOUCH_PRESSED, &listener_gate));
    TEST_ASSERT_TRUE(kode_event_add_listener(KODE_EVENT_TOUCH_MOVED, &listener_rec));
    TEST_ASSERT_TRUE(kode_event_add_listener(KODE_EVENT_TOUCH_RELEASED, &listener_rec));

    publish_touch(PRESS_X, PRESS_Y, true);
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_gate_in, pdMS_TO_TICKS(TEST_WAIT_MS)));
    publish_touch(11, 21, true);
    publish_touch(12, 22, true);
    publish_touch(13, 23, true);
    publish_touch(14, 24, false);
    standard_ao_support_drained();

    (void)xSemaphoreGive(s_gate_open);
    TEST_ASSERT_TRUE(rec_wait(2U));
    barrier();

    TEST_ASSERT_EQUAL_UINT32(4U, s_rec_n);
    rec_is(0U, "touch_pressed", PRESS_X, PRESS_Y);
    rec_is(1U, "touch_moved", 13, 23);
    rec_is(2U, "touch_released", 14, 24);
    TEST_ASSERT_EQUAL_STRING("", s_rec[0].source);
    TEST_ASSERT_EQUAL_INT32(0, s_rec[0].value);

    TEST_ASSERT_TRUE(kode_event_remove_listener(KODE_EVENT_TOUCH_PRESSED, &listener_gate));
    TEST_ASSERT_TRUE(kode_event_remove_listener(KODE_EVENT_TOUCH_MOVED, &listener_rec));
    TEST_ASSERT_TRUE(kode_event_remove_listener(KODE_EVENT_TOUCH_RELEASED, &listener_rec));
}

TEST_CASE("a move of a new stroke never takes the place of the last stroke's", "[kode_touch]")
{
    events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener("button_top_pressed", &listener_gate));
    TEST_ASSERT_TRUE(kode_event_add_listener(KODE_EVENT_TOUCH_PRESSED, &listener_rec));
    TEST_ASSERT_TRUE(kode_event_add_listener(KODE_EVENT_TOUCH_MOVED, &listener_rec));
    TEST_ASSERT_TRUE(kode_event_add_listener(KODE_EVENT_TOUCH_RELEASED, &listener_rec));

    kode_event_push("button_top_pressed", "top", 0, NULL);
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_gate_in, pdMS_TO_TICKS(TEST_WAIT_MS)));
    publish_touch(1, 1, true);
    publish_touch(2, 2, true);
    publish_touch(3, 3, false);
    publish_touch(4, 4, true);
    publish_touch(5, 5, true);
    publish_touch(6, 6, true);
    publish_touch(7, 7, false);
    standard_ao_support_drained();

    (void)xSemaphoreGive(s_gate_open);
    TEST_ASSERT_TRUE(rec_wait(6U));
    barrier();

    TEST_ASSERT_EQUAL_UINT32(8U, s_rec_n);
    rec_is(1U, "touch_pressed", 1, 1);
    rec_is(2U, "touch_moved", 2, 2);
    rec_is(3U, "touch_released", 3, 3);
    rec_is(4U, "touch_pressed", 4, 4);
    rec_is(5U, "touch_moved", 6, 6);
    rec_is(6U, "touch_released", 7, 7);

    TEST_ASSERT_TRUE(kode_event_remove_listener("button_top_pressed", &listener_gate));
    TEST_ASSERT_TRUE(kode_event_remove_listener(KODE_EVENT_TOUCH_PRESSED, &listener_rec));
    TEST_ASSERT_TRUE(kode_event_remove_listener(KODE_EVENT_TOUCH_MOVED, &listener_rec));
    TEST_ASSERT_TRUE(kode_event_remove_listener(KODE_EVENT_TOUCH_RELEASED, &listener_rec));
}

TEST_CASE("test_touch_moves_coalesce_to_one", "[kode_touch]")
{
    events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener(KODE_EVENT_TOUCH_PRESSED, &listener_gate));
    TEST_ASSERT_TRUE(kode_event_add_listener(KODE_EVENT_TOUCH_MOVED, &listener_rec));
    TEST_ASSERT_TRUE(kode_event_add_listener("button_top_pressed", &listener_rec));
    const uint32_t dropped0 = kode_event_dropped();

    publish_touch(PRESS_X, PRESS_Y, true);
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_gate_in, pdMS_TO_TICKS(TEST_WAIT_MS)));

    /* A drag of 50 samples while the listener holds the press, a button in
     * the middle of it: the ring would hold 16. */
    for ( uint32_t i = 1U; DRAG_MOVES >= i; i++ )
    {
        publish_touch((int16_t)(PRESS_X + (int16_t)i), (int16_t)(PRESS_Y + (int16_t)i), true);
        pace();
        if ( DRAG_BUTTON_AT == i )
        {
            standard_ao_support_drained();
            kode_event_push("button_top_pressed", "top", 0, NULL);
        }
    }
    standard_ao_support_drained();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, kode_event_dropped() - dropped0,
                                     "a drag never fills the ring");

    (void)xSemaphoreGive(s_gate_open);
    TEST_ASSERT_TRUE(rec_wait(2U));
    barrier();

    /* The press, the one move at the drag's last point, the button, the barrier. */
    TEST_ASSERT_EQUAL_UINT32(4U, s_rec_n);
    rec_is(0U, "touch_pressed", PRESS_X, PRESS_Y);
    rec_is(1U, "touch_moved", (int16_t)(PRESS_X + (int16_t)DRAG_MOVES),
           (int16_t)(PRESS_Y + (int16_t)DRAG_MOVES));
    rec_is(2U, "button_top_pressed", 0, 0);
    TEST_ASSERT_EQUAL_STRING("top", s_rec[2].source);

    publish_touch(PRESS_X, PRESS_Y, false);
    standard_ao_support_drained();
    TEST_ASSERT_TRUE(kode_event_remove_listener(KODE_EVENT_TOUCH_PRESSED, &listener_gate));
    TEST_ASSERT_TRUE(kode_event_remove_listener(KODE_EVENT_TOUCH_MOVED, &listener_rec));
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_top_pressed", &listener_rec));
    barrier();
}

/** @brief What a second task does while this one waits: lift the finger at (30, 40). */
static void release_while_waiting(void)
{
    publish_touch(30, 40, false);
}

TEST_CASE("waiting for a touch event returns it with its point", "[kode_touch]")
{
    kode_event_t out = { NULL, NULL, -1, NULL, -1, -1 };

    events_up();
    publish_touch(PRESS_X, PRESS_Y, true);
    standard_ao_support_drained();

    fake_sem_wait_hook(&release_while_waiting);
    const bool got = kode_event_wait(KODE_EVENT_TOUCH_RELEASED, TEST_WAIT_MS, &out);
    fake_sem_wait_hook(NULL);

    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL_STRING("touch_released", out.name);
    TEST_ASSERT_EQUAL_INT16(30, out.x);
    TEST_ASSERT_EQUAL_INT16(40, out.y);
    TEST_ASSERT_EQUAL_INT32(0, out.value);
    barrier();
}

TEST_CASE("with a screen attached the touch still gets every sample", "[kode_touch]")
{
    events_up();
    TEST_ASSERT_TRUE(kode_screen_new("Touch"));
    const kode_widget_t go = kode_screen_add_button("Go");

    TEST_ASSERT_NOT_EQUAL_UINT32(0U, go);
    standard_ao_support_drained();
    TEST_ASSERT_FALSE(kode_widget_was_pressed(go));

    TEST_ASSERT_TRUE(kode_event_add_listener(KODE_EVENT_TOUCH_PRESSED, &listener_gate));
    TEST_ASSERT_TRUE(kode_event_add_listener(KODE_EVENT_TOUCH_MOVED, &listener_rec));

    /* The fake taps the widget at the touch's x: the button, place 0. */
    publish_touch(0, PRESS_Y, true);
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_gate_in, pdMS_TO_TICKS(TEST_WAIT_MS)));
    standard_ao_support_drained();
    TEST_ASSERT_TRUE(kode_widget_was_pressed(go));

    /* Each move reaches the screen while the moves coalesce in the ring. */
    for ( uint32_t i = 1U; SCREEN_SAMPLES >= i; i++ )
    {
        publish_touch(0, (int16_t)(PRESS_Y + (int16_t)i), true);
        standard_ao_support_drained();
        TEST_ASSERT_TRUE(kode_widget_was_pressed(go));
    }

    (void)xSemaphoreGive(s_gate_open);
    TEST_ASSERT_TRUE(rec_wait(1U));
    barrier();
    TEST_ASSERT_EQUAL_UINT32(3U, s_rec_n);
    rec_is(1U, "touch_moved", 0, (int16_t)(PRESS_Y + (int16_t)SCREEN_SAMPLES));

    publish_touch(0, PRESS_Y, false);
    standard_ao_support_drained();
    TEST_ASSERT_TRUE(kode_event_remove_listener(KODE_EVENT_TOUCH_PRESSED, &listener_gate));
    TEST_ASSERT_TRUE(kode_event_remove_listener(KODE_EVENT_TOUCH_MOVED, &listener_rec));
    barrier();
}

/* Last, on purpose: the Active Object stays on the bus for the rest of the
 * binary, and test_touch_ao.c publishes the finger on a bench of its own. It
 * leaves the signal, and the Active Objects this file took off it go back. */
TEST_CASE("the layer leaves the finger for the cases that follow", "[kode_touch]")
{
    events_up();
    QActive_unsubscribe(AO_KodeStandard, TOUCH_EVT_SIG);
    const bool off = !touch_subscribed();

    restore_others();
    TEST_ASSERT_TRUE(off);
}
