/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_event.c
 * @brief The standard layer's events: listeners, kode_event_wait() and the task that calls them.
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "fakes.h"
#include "khal_system.h"
#include "kdrv_buttons.h"
#include "kode.h"
#include "kode_button.h"
#include "kode_entry.h"
#include "kode_standard_ao.h"
#include "kode_sys.h"
#include "kode_sys_priv.h"
#include "kode_task.h"
#include "kqp.h"
#include "qf_support.h"

/** @brief Stack of a test task, in bytes. */
#define TEST_STACK_BYTES 8192U

/** @brief Level of a test task: above the main task, so it runs at once. */
#define TEST_PRIO 2U

/** @brief The longest a case waits for a task or a listener, in ms. */
#define TEST_WAIT_MS 4000U

/** @brief The one real timeout of a case, in ms. */
#define TEST_SHORT_WAIT_MS 20U

/** @brief Calls a case can record. */
#define REC_MAX 64U

/** @brief Longest text a record keeps. */
#define REC_TEXT 64U

/** @brief Who the barrier listener is in a record. */
#define WHO_BARRIER 99

/** @brief Taller than any name the layer takes. */
#define LONG_NAME_LEN 40U

/** @brief What a listener saw. */
typedef struct {
    int     who;               /**< Which listener function.          */
    char    name[REC_TEXT];    /**< The event's name.                 */
    char    source[REC_TEXT];  /**< Its source.                       */
    char    text[REC_TEXT];    /**< Its text.                         */
    int32_t value;             /**< Its value.                        */
    int16_t x;                 /**< Its point's x.                    */
    int16_t y;                 /**< Its point's y.                    */
} rec_t;

static rec_t             s_rec[REC_MAX];  /**< What the listeners saw, in order.    */
static volatile uint32_t s_rec_n;         /**< How many records.                     */
static SemaphoreHandle_t s_given;         /**< Counts the calls; a listener gives it. */

/* ---- The listeners --------------------------------------------------------- */

/** @brief Record one call and tell the case. */
static void record(int who, const kode_event_t *e)
{
    const uint32_t i = s_rec_n;

    if ( REC_MAX > i )
    {
        s_rec[i].who = who;
        (void)snprintf(s_rec[i].name, REC_TEXT, "%s", e->name);
        (void)snprintf(s_rec[i].source, REC_TEXT, "%s", e->source);
        (void)snprintf(s_rec[i].text, REC_TEXT, "%s", e->text);
        s_rec[i].value = e->value;
        s_rec[i].x     = e->x;
        s_rec[i].y     = e->y;
    }
    s_rec_n = i + 1U;
    (void)xSemaphoreGive(s_given);
}

static void listener_a(const kode_event_t *e) { record(1, e); }
static void listener_b(const kode_event_t *e) { record(2, e); }
static void listener_c(const kode_event_t *e) { record(3, e); }
static void listener_barrier(const kode_event_t *e) { record(WHO_BARRIER, e); }

/** @brief Forget what was recorded and drain the counting semaphore. */
static void rec_reset(void)
{
    if ( NULL == s_given )
    {
        s_given = xSemaphoreCreateCounting(REC_MAX, 0U);
    }
    while ( xSemaphoreTake(s_given, 0U) == pdTRUE )
    {
    }
    s_rec_n = 0U;
}

/** @brief Wait for @p n more calls; false if they do not all come. */
static bool rec_wait(uint32_t n)
{
    bool ok = true;

    for ( uint32_t i = 0U; (i < n) && ok; i++ )
    {
        ok = (xSemaphoreTake(s_given, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    }
    return ok;
}

/* ---- The bus --------------------------------------------------------------- */

/** @brief Publish a button edge the way AO_Buttons does. */
static void publish_button(kdrv_button_t id, bool pressed)
{
    ButtonEvt *const e = Q_NEW(ButtonEvt, BUTTON_EVT_SIG);

    e->id      = (uint8_t)id;
    e->pressed = pressed;
    e->t_ms    = 0U;
    QACTIVE_PUBLISH(&e->super, (void *)0);
}

/** @brief Publish the hold of the top or the bottom button. */
static void publish_hold(kdrv_button_t id)
{
    ButtonHoldEvt *const e = Q_NEW(ButtonHoldEvt, BUTTON_HOLD_SIG);

    e->id = (uint8_t)id;
    QACTIVE_PUBLISH(&e->super, (void *)0);
}

/** @brief Make kode_init() a no-op, as it is once the system is up. */
static void system_is_up(void)
{
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
}

/** @brief Start the layer if no case has, with real locks; fail the case if it will not.
 *
 * The locks stay real for the rest of the case (setUp turns them off for the
 * next): every task that waits makes its own semaphore on its first wait, and
 * that one has to be real as well.
 */
static void events_up(void)
{
    qf_support_init();
    system_is_up();
    rec_reset();
    fake_sync_real(true);
    TEST_ASSERT_TRUE(kode_standard_ao_ensure());
}

/* ---- Two tasks using the layer together ------------------------------------ */

static SemaphoreHandle_t s_finished[2];       /**< Given by each caller on return.             */
static volatile bool     s_added[2];          /**< What each caller's add answered.            */
static volatile bool     s_up_at_return[2];   /**< The Active Object had a queue at return.    */

/** @brief One of the two users: add a listener, as an app's task would. */
static void first_user(void *arg)
{
    const uint32_t i = (uint32_t)(uintptr_t)arg;

    if ( 0U == i )
    {
        s_added[i] = kode_event_add_listener("button_top_pressed", &listener_a);
    }
    else
    {
        s_added[i] = kode_event_add_listener("button_top_pressed", &listener_b);
    }
    s_up_at_return[i] = (NULL != AO_KodeStandard->eQueue);
    (void)xSemaphoreGive(s_finished[i]);
}

TEST_CASE("two tasks using the layer together both get a working listener", "[kode_event]")
{
    qf_support_init();
    system_is_up();
    rec_reset();
    fake_sync_real(true);

    kode_status_t started = KODE_OK;
    BaseType_t    took    = pdTRUE;

    for ( uint32_t i = 0U; (2U > i) && (KODE_OK == started); ++i )
    {
        s_finished[i] = xSemaphoreCreateBinary();
        started = khal_task_start(&first_user, "t_first", TEST_STACK_BYTES,
                                  TEST_PRIO, (void *)(uintptr_t)i);
    }
    for ( uint32_t i = 0U; (2U > i) && (KODE_OK == started) && (took == pdTRUE); ++i )
    {
        took = xSemaphoreTake(s_finished[i], pdMS_TO_TICKS(TEST_WAIT_MS));
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK, started);
    TEST_ASSERT_EQUAL_INT(pdTRUE, took);

    /* Neither came back before the Active Object was up. */
    for ( uint32_t i = 0U; 2U > i; ++i )
    {
        TEST_ASSERT_TRUE(s_added[i]);
        TEST_ASSERT_TRUE(s_up_at_return[i]);
        vSemaphoreDelete(s_finished[i]);
    }

    /* One Active Object, at the level the layer promises. */
    const char *holder = NULL;

    TEST_ASSERT_TRUE(kqp_prio_taken(KODE_PRIO_USER_GUI, &holder));
    TEST_ASSERT_EQUAL_STRING("kode_standard", holder);

    /* Both listeners are in, and both hear the button. */
    publish_button(KDRV_BTN_TOP, true);
    TEST_ASSERT_TRUE(rec_wait(2U));
    TEST_ASSERT_EQUAL_UINT32(2U, s_rec_n);
    TEST_ASSERT_EQUAL_INT(3, s_rec[0].who + s_rec[1].who);
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_top_pressed", &listener_a));
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_top_pressed", &listener_b));
}

/* ---- Buttons become events ------------------------------------------------- */

TEST_CASE("a listener gets the button that was pressed", "[kode_event]")
{
    events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener("button_top_pressed", &listener_a));

    publish_button(KDRV_BTN_TOP, true);
    TEST_ASSERT_TRUE(rec_wait(1U));

    TEST_ASSERT_EQUAL_UINT32(1U, s_rec_n);
    TEST_ASSERT_EQUAL_STRING("button_top_pressed", s_rec[0].name);
    TEST_ASSERT_EQUAL_STRING("top", s_rec[0].source);
    TEST_ASSERT_EQUAL_INT32(0, s_rec[0].value);
    TEST_ASSERT_EQUAL_STRING("", s_rec[0].text);
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_top_pressed", &listener_a));
}

TEST_CASE("released and held are events of their own", "[kode_event]")
{
    events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener("button_bottom_released", &listener_a));
    TEST_ASSERT_TRUE(kode_event_add_listener("button_bottom_held", &listener_b));
    TEST_ASSERT_TRUE(kode_event_add_listener("button_left_pressed", &listener_c));

    publish_hold(KDRV_BTN_BOT);
    publish_button(KDRV_BTN_BOT, false);
    publish_button(KDRV_BTN_LEFT, true);
    TEST_ASSERT_TRUE(rec_wait(3U));

    TEST_ASSERT_EQUAL_UINT32(3U, s_rec_n);
    TEST_ASSERT_EQUAL_STRING("button_bottom_held", s_rec[0].name);
    TEST_ASSERT_EQUAL_STRING("bottom", s_rec[0].source);
    TEST_ASSERT_EQUAL_STRING("button_bottom_released", s_rec[1].name);
    TEST_ASSERT_EQUAL_STRING("bottom", s_rec[1].source);
    TEST_ASSERT_EQUAL_STRING("button_left_pressed", s_rec[2].name);
    TEST_ASSERT_EQUAL_STRING("left", s_rec[2].source);
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_bottom_released", &listener_a));
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_bottom_held", &listener_b));
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_left_pressed", &listener_c));
}

/* ---- The table ------------------------------------------------------------- */

TEST_CASE("listeners of one event run in the order they were added", "[kode_event]")
{
    events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener("button_up_pressed", &listener_a));
    TEST_ASSERT_TRUE(kode_event_add_listener("button_up_pressed", &listener_b));
    TEST_ASSERT_TRUE(kode_event_add_listener("button_up_pressed", &listener_c));

    kode_event_push("button_up_pressed", "src", 7, "txt");
    TEST_ASSERT_TRUE(rec_wait(3U));

    TEST_ASSERT_EQUAL_UINT32(3U, s_rec_n);
    TEST_ASSERT_EQUAL_INT(1, s_rec[0].who);
    TEST_ASSERT_EQUAL_INT(2, s_rec[1].who);
    TEST_ASSERT_EQUAL_INT(3, s_rec[2].who);
    /* The whole event reaches each of them. */
    TEST_ASSERT_EQUAL_STRING("src", s_rec[2].source);
    TEST_ASSERT_EQUAL_INT32(7, s_rec[2].value);
    TEST_ASSERT_EQUAL_STRING("txt", s_rec[2].text);
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_up_pressed", &listener_a));
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_up_pressed", &listener_b));
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_up_pressed", &listener_c));
}

TEST_CASE("a removed listener is not called again", "[kode_event]")
{
    events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener("button_down_pressed", &listener_a));
    TEST_ASSERT_TRUE(kode_event_add_listener("button_down_pressed", &listener_barrier));
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_down_pressed", &listener_a));

    kode_event_push("button_down_pressed", "", 0, "");
    TEST_ASSERT_TRUE(rec_wait(1U));

    /* The barrier, added after the listener that was removed, answered; the
     * removed one did not, and it would have been called first. */
    TEST_ASSERT_EQUAL_UINT32(1U, s_rec_n);
    TEST_ASSERT_EQUAL_INT(WHO_BARRIER, s_rec[0].who);
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_down_pressed", &listener_barrier));
}

TEST_CASE("removing what is not registered fails and says so", "[kode_event]")
{
    events_up();
    TEST_ASSERT_FALSE(kode_event_remove_listener("never_added", &listener_a));
    TEST_ASSERT_NOT_NULL(strstr(kode_error(), "never_added"));
    TEST_ASSERT_FALSE(kode_event_remove_listener(NULL, &listener_a));
    TEST_ASSERT_NOT_NULL(strstr(kode_error(), "NULL"));
    TEST_ASSERT_FALSE(kode_event_remove_listener("never_added", NULL));
    TEST_ASSERT_NOT_NULL(strstr(kode_error(), "NULL"));
}

TEST_CASE("adding the same listener twice registers it once", "[kode_event]")
{
    events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener("button_left_pressed", &listener_a));
    TEST_ASSERT_TRUE(kode_event_add_listener("BUTTON_Left_Pressed", &listener_a));
    TEST_ASSERT_TRUE(kode_event_add_listener("button_left_pressed", &listener_barrier));

    kode_event_push("button_left_pressed", "", 0, "");
    TEST_ASSERT_TRUE(rec_wait(2U));

    /* Once, then the barrier behind it: a second call of A would come
     * between them or after. */
    TEST_ASSERT_EQUAL_UINT32(2U, s_rec_n);
    TEST_ASSERT_EQUAL_INT(1, s_rec[0].who);
    TEST_ASSERT_EQUAL_INT(WHO_BARRIER, s_rec[1].who);

    TEST_ASSERT_TRUE(kode_event_remove_listener("button_left_pressed", &listener_a));
    TEST_ASSERT_FALSE(kode_event_remove_listener("button_left_pressed", &listener_a));
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_left_pressed", &listener_barrier));
}

/** @brief The three kinds of button event, in the order the helper below walks them. */
static const char *const s_whats[] = { "pressed", "released", "held" };

/** @brief How many buttons there are. */
#define TEST_BUTTON_COUNT ((uint32_t)KDRV_BTN_COUNT)

/** @brief Spell the @p i-th real event name: every button, pressed and released, then held for top and bottom. */
static void event_name(uint32_t i, char *out, size_t size)
{
    const uint32_t plain = TEST_BUTTON_COUNT * 2U;

    if ( i < plain )
    {
        (void)snprintf(out, size, "button_%s_%s",
                       kode_button_name((uint8_t)(i % TEST_BUTTON_COUNT)), s_whats[i / TEST_BUTTON_COUNT]);
    }
    else if ( (i - plain) == 0U )
    {
        (void)snprintf(out, size, "button_top_held");
    }
    else
    {
        (void)snprintf(out, size, "button_bottom_held");
    }
}

/** @brief How many real event names event_name() has. */
#define TEST_EVENT_NAMES (TEST_BUTTON_COUNT * 2U + 2U)

TEST_CASE("a full table refuses the next listener and says why", "[kode_event]")
{
    char name[KODE_EVENT_NAME_MAX];
    const kode_listener_t fns[] = { &listener_a, &listener_b, &listener_c };

    events_up();
    /* Real names, several functions: the table is full of distinct pairs. */
    for ( uint32_t i = 0U; KODE_LISTENERS_MAX > i; i++ )
    {
        event_name(i % TEST_EVENT_NAMES, name, sizeof(name));
        TEST_ASSERT_TRUE(kode_event_add_listener(name, fns[i / TEST_EVENT_NAMES]));
    }
    TEST_ASSERT_FALSE(kode_event_add_listener("button_up_pressed", &listener_barrier));
    TEST_ASSERT_NOT_NULL(strstr(kode_error(), "too many listeners"));

    /* One that is in already is not a new one. */
    TEST_ASSERT_TRUE(kode_event_add_listener("button_up_pressed", &listener_a));

    /* Room made, room taken. */
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_up_pressed", &listener_a));
    TEST_ASSERT_TRUE(kode_event_add_listener("button_up_pressed", &listener_barrier));
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    TEST_ASSERT_TRUE(kode_event_remove_listener("button_up_pressed", &listener_barrier));
    for ( uint32_t i = 1U; KODE_LISTENERS_MAX > i; i++ )
    {
        event_name(i % TEST_EVENT_NAMES, name, sizeof(name));
        TEST_ASSERT_TRUE(kode_event_remove_listener(name, fns[i / TEST_EVENT_NAMES]));
    }
}

TEST_CASE("every button event is a known event, and held only for top and bottom", "[kode_event]")
{
    char name[KODE_EVENT_NAME_MAX];

    events_up();
    for ( uint32_t i = 0U; TEST_EVENT_NAMES > i; i++ )
    {
        event_name(i, name, sizeof(name));
        TEST_ASSERT_TRUE_MESSAGE(kode_event_add_listener(name, &listener_a), name);
        TEST_ASSERT_TRUE(kode_event_remove_listener(name, &listener_a));
    }
    TEST_ASSERT_FALSE(kode_event_add_listener("button_up_held", &listener_a));
    TEST_ASSERT_EQUAL_STRING("unknown event 'button_up_held'", kode_error());
}

TEST_CASE("a mistyped event name fails and names what was typed", "[kode_event]")
{
    events_up();
    TEST_ASSERT_FALSE(kode_event_add_listener("Button_Topp_Pressed", &listener_a));
    TEST_ASSERT_EQUAL_STRING("unknown event 'Button_Topp_Pressed'", kode_error());
    TEST_ASSERT_FALSE(kode_event_add_listener("button_top_pressedd", &listener_a));
    TEST_ASSERT_EQUAL_STRING("unknown event 'button_top_pressedd'", kode_error());
    TEST_ASSERT_FALSE(kode_event_add_listener("button_bot_pressed", &listener_a));
    TEST_ASSERT_EQUAL_STRING("unknown event 'button_bot_pressed'", kode_error());
    TEST_ASSERT_FALSE(kode_event_add_listener("button_", &listener_a));
    TEST_ASSERT_FALSE(kode_event_add_listener("wifi_gone", &listener_a));

    kode_event_t out = { NULL, NULL, -1, NULL, -1, -1 };

    TEST_ASSERT_FALSE(kode_event_wait("button_topp_pressed", 1U, &out));
    TEST_ASSERT_EQUAL_STRING("unknown event 'button_topp_pressed'", kode_error());
    TEST_ASSERT_NULL(out.name);
}

TEST_CASE("battery_low is a known event, in any case", "[kode_event]")
{
    events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener("battery_low", &listener_a));
    TEST_ASSERT_TRUE(kode_event_remove_listener("Battery_LOW", &listener_a));
    TEST_ASSERT_FALSE(kode_event_add_listener("battery_empty", &listener_a));
    TEST_ASSERT_EQUAL_STRING("unknown event 'battery_empty'", kode_error());
}

TEST_CASE("event names are matched without regard to case", "[kode_event]")
{
    events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener("Button_TOP_Pressed", &listener_a));

    publish_button(KDRV_BTN_TOP, true);
    TEST_ASSERT_TRUE(rec_wait(1U));

    /* The event is spelled the way kodeOS spells it, not the way it was asked for. */
    TEST_ASSERT_EQUAL_STRING("button_top_pressed", s_rec[0].name);
    TEST_ASSERT_TRUE(kode_event_remove_listener("BUTTON_top_PRESSED", &listener_a));
}

TEST_CASE("bad arguments are refused with the reason", "[kode_event]")
{
    char too_long[LONG_NAME_LEN + 1U];

    events_up();
    (void)memset(too_long, 'x', LONG_NAME_LEN);
    too_long[LONG_NAME_LEN] = '\0';

    TEST_ASSERT_FALSE(kode_event_add_listener(NULL, &listener_a));
    TEST_ASSERT_NOT_NULL(strstr(kode_error(), "NULL"));
    TEST_ASSERT_FALSE(kode_event_add_listener("", &listener_a));
    TEST_ASSERT_NOT_NULL(strstr(kode_error(), "empty"));
    TEST_ASSERT_FALSE(kode_event_add_listener(too_long, &listener_a));
    TEST_ASSERT_NOT_NULL(strstr(kode_error(), "too long"));
    TEST_ASSERT_FALSE(kode_event_add_listener("button_top_pressed", NULL));
    TEST_ASSERT_NOT_NULL(strstr(kode_error(), "NULL"));
    TEST_ASSERT_FALSE(kode_event_wait(NULL, 1U, NULL));
    TEST_ASSERT_NOT_NULL(strstr(kode_error(), "NULL"));

    /* A call that works clears the reason. */
    TEST_ASSERT_TRUE(kode_event_add_listener("button_top_released", &listener_a));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_top_released", &listener_a));
}

/* ---- A listener that uses the layer ---------------------------------------- */

static volatile bool s_inner_added;     /**< What the listener's own add answered.    */
static volatile bool s_inner_removed;   /**< What the listener's own remove answered. */

/** @brief A listener that registers another and removes itself. */
static void listener_adder(const kode_event_t *e)
{
    (void)e;
    s_inner_added   = kode_event_add_listener("button_up_released", &listener_b);
    s_inner_removed = kode_event_remove_listener("button_right_pressed", &listener_adder);
    (void)xSemaphoreGive(s_given);
}

TEST_CASE("a listener may add and remove listeners", "[kode_event]")
{
    events_up();
    s_inner_added   = false;
    s_inner_removed = false;
    TEST_ASSERT_TRUE(kode_event_add_listener("button_right_pressed", &listener_adder));

    kode_event_push("button_right_pressed", "", 0, "");
    TEST_ASSERT_TRUE(rec_wait(1U));
    TEST_ASSERT_TRUE(s_inner_added);
    TEST_ASSERT_TRUE(s_inner_removed);

    /* The one it added is live, and the one that removed itself is gone. */
    kode_event_push("button_up_released", "", 0, "");
    TEST_ASSERT_TRUE(rec_wait(1U));
    TEST_ASSERT_EQUAL_INT(2, s_rec[0].who);
    TEST_ASSERT_FALSE(kode_event_remove_listener("button_right_pressed", &listener_adder));
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_up_released", &listener_b));
}

/** @brief A listener that removes the next one of the same event. */
static void listener_remover(const kode_event_t *e)
{
    (void)e;
    s_inner_removed = kode_event_remove_listener("button_left_released", &listener_b);
    (void)xSemaphoreGive(s_given);
}

TEST_CASE("a listener removed during a delivery is not called in it", "[kode_event]")
{
    events_up();
    s_inner_removed = false;
    TEST_ASSERT_TRUE(kode_event_add_listener("button_left_released", &listener_remover));
    TEST_ASSERT_TRUE(kode_event_add_listener("button_left_released", &listener_b));
    TEST_ASSERT_TRUE(kode_event_add_listener("button_left_released", &listener_barrier));

    kode_event_push("button_left_released", "", 0, "");
    /* The remover gives once, the barrier records once; B was chosen for the
     * delivery before it was removed and must not run. */
    TEST_ASSERT_TRUE(rec_wait(2U));
    TEST_ASSERT_TRUE(s_inner_removed);
    TEST_ASSERT_EQUAL_UINT32(1U, s_rec_n);
    TEST_ASSERT_EQUAL_INT(WHO_BARRIER, s_rec[0].who);
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_left_released", &listener_remover));
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_left_released", &listener_barrier));
}

static volatile bool s_inner_wait;               /**< What the listener's wait answered. */
static char          s_inner_error[REC_TEXT];    /**< The error it left.                 */

/** @brief A listener that tries to wait for an event. */
static void listener_waiter(const kode_event_t *e)
{
    (void)e;
    s_inner_wait = kode_event_wait("button_up_pressed", 1U, NULL);
    (void)snprintf(s_inner_error, sizeof(s_inner_error), "%s", kode_error());
    (void)xSemaphoreGive(s_given);
}

TEST_CASE("waiting inside a listener is refused", "[kode_event]")
{
    events_up();
    s_inner_wait = true;
    TEST_ASSERT_TRUE(kode_event_add_listener("button_top_held", &listener_waiter));

    kode_event_push("button_top_held", "", 0, "");
    TEST_ASSERT_TRUE(rec_wait(1U));

    TEST_ASSERT_FALSE(s_inner_wait);
    TEST_ASSERT_EQUAL_STRING("cannot wait for an event inside a listener", s_inner_error);
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_top_held", &listener_waiter));
}

/* ---- kode_event_wait() ----------------------------------------------------- */

/** @brief What a second task does while this one sleeps: press the top button. */
static void press_top_while_waiting(void)
{
    publish_button(KDRV_BTN_TOP, true);
}

TEST_CASE("waiting for an event returns it", "[kode_event]")
{
    kode_event_t out = { NULL, NULL, -1, NULL, -1, -1 };

    events_up();
    /* The press comes once the wait has registered and is asleep. */
    fake_sem_wait_hook(&press_top_while_waiting);
    const bool got = kode_event_wait("Button_Top_Pressed", TEST_WAIT_MS, &out);
    fake_sem_wait_hook(NULL);

    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    /* The strings are still good after the call returned. */
    TEST_ASSERT_EQUAL_STRING("button_top_pressed", out.name);
    TEST_ASSERT_EQUAL_STRING("top", out.source);
    TEST_ASSERT_EQUAL_INT32(0, out.value);
    TEST_ASSERT_EQUAL_STRING("", out.text);
    TEST_ASSERT_EQUAL_INT16(0, out.x);
    TEST_ASSERT_EQUAL_INT16(0, out.y);

    /* out may be NULL when only the fact matters. */
    fake_sem_wait_hook(&press_top_while_waiting);
    const bool again = kode_event_wait("button_top_pressed", TEST_WAIT_MS, NULL);
    fake_sem_wait_hook(NULL);
    TEST_ASSERT_TRUE(again);
}

TEST_CASE("waiting gives up when the time is out", "[kode_event]")
{
    kode_event_t out = { NULL, NULL, -1, NULL, -1, -1 };

    events_up();
    TEST_ASSERT_FALSE(kode_event_wait("button_up_pressed", TEST_SHORT_WAIT_MS, &out));
    TEST_ASSERT_NOT_NULL(strstr(kode_error(), "button_up_pressed"));
    TEST_ASSERT_NULL(out.name);
    TEST_ASSERT_EQUAL_INT32(-1, out.value);

    /* A wait of zero is a no. */
    TEST_ASSERT_FALSE(kode_event_wait("button_up_pressed", 0U, &out));

    /* An event that came before the wait is not remembered. */
    TEST_ASSERT_TRUE(kode_event_add_listener("button_up_pressed", &listener_a));
    publish_button(KDRV_BTN_UP, true);
    TEST_ASSERT_TRUE(rec_wait(1U));
    TEST_ASSERT_FALSE(kode_event_wait("button_up_pressed", TEST_SHORT_WAIT_MS, &out));
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_up_pressed", &listener_a));
}

/* ---- Overflow -------------------------------------------------------------- */

static SemaphoreHandle_t s_gate_in;     /**< Counts the times a gate listener has started.  */
static SemaphoreHandle_t s_gate_open;   /**< Each give lets one gate listener go.           */
static volatile bool     s_fill_gated;  /**< The next fill listener call waits at the gate. */

/** @brief Wait at the gate until the case lets the dispatch task go. */
static void gate_wait(void)
{
    (void)xSemaphoreGive(s_gate_in);
    (void)xSemaphoreTake(s_gate_open, pdMS_TO_TICKS(TEST_WAIT_MS));
}

/** @brief A listener that holds the dispatch task until the case lets it go. */
static void listener_gate(const kode_event_t *e)
{
    (void)e;
    gate_wait();
}

/** @brief A listener that records, and holds the dispatch task once if the case says so. */
static void listener_fill(const kode_event_t *e)
{
    record(1, e);
    if ( s_fill_gated )
    {
        s_fill_gated = false;
        gate_wait();
    }
}

/** @brief Wait until a gate listener is inside its gate. */
static void gate_entered(void)
{
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_gate_in, pdMS_TO_TICKS(TEST_WAIT_MS)));
}

/** @brief Push @p n fills with values from @p first. */
static void push_fills(uint32_t first, uint32_t n)
{
    for ( uint32_t i = 0U; i < n; i++ )
    {
        kode_event_push("button_down_released", "", (int32_t)(first + i), "");
    }
}

TEST_CASE("a full ring drops events, counts them and says so once per burst", "[kode_event]")
{
    enum { EXTRA = 4 };

    events_up();
    if ( NULL == s_gate_in )
    {
        s_gate_in   = xSemaphoreCreateCounting(REC_MAX, 0U);
        s_gate_open = xSemaphoreCreateCounting(REC_MAX, 0U);
    }
    while ( xSemaphoreTake(s_gate_in, 0U) == pdTRUE )
    {
    }
    while ( xSemaphoreTake(s_gate_open, 0U) == pdTRUE )
    {
    }
    s_fill_gated = false;
    TEST_ASSERT_TRUE(kode_event_add_listener("button_bottom_pressed", &listener_gate));
    TEST_ASSERT_TRUE(kode_event_add_listener("button_down_released", &listener_fill));

    /* The dispatch task is inside the gate listener: nothing leaves the ring. */
    kode_event_push("button_bottom_pressed", "", 0, "");
    gate_entered();

    const uint32_t dropped0 = kode_event_dropped();
    const unsigned lines0   = fake_klog_count();

    /* The entry being delivered holds its place until its listener returns,
     * so DEPTH - 1 more fit. */
    push_fills(0U, KODE_EVENT_QUEUE_DEPTH + (uint32_t)EXTRA);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)EXTRA + 1U, kode_event_dropped() - dropped0);

    /* One line for the burst, naming the event; not one per drop. */
    TEST_ASSERT_EQUAL_UINT(lines0 + 1U, fake_klog_count());
    TEST_ASSERT_NOT_NULL(strstr(fake_klog_last(), "button_down_released"));

    /* Sustained: the gate lets one entry go and the first fill waits in its
     * turn, so there is room for one more - which is taken - and the rest of
     * the overload is dropped. The ring has not emptied, so it is the same
     * burst and the log says nothing more. */
    s_fill_gated = true;
    (void)xSemaphoreGive(s_gate_open);
    gate_entered();
    push_fills(100U, 3U);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)EXTRA + 1U + 2U, kode_event_dropped() - dropped0);
    TEST_ASSERT_EQUAL_UINT(lines0 + 1U, fake_klog_count());

    /* Let everything go: what was kept arrives, in order, oldest first. */
    (void)xSemaphoreGive(s_gate_open);
    TEST_ASSERT_TRUE(rec_wait(KODE_EVENT_QUEUE_DEPTH));
    TEST_ASSERT_EQUAL_UINT32(KODE_EVENT_QUEUE_DEPTH, s_rec_n);
    for ( uint32_t i = 0U; (KODE_EVENT_QUEUE_DEPTH - 1U) > i; i++ )
    {
        TEST_ASSERT_EQUAL_INT32((int32_t)i, s_rec[i].value);
    }
    TEST_ASSERT_EQUAL_INT32(100, s_rec[KODE_EVENT_QUEUE_DEPTH - 1U].value);

    /* The ring emptied, so the next overload is a new burst and is reported. */
    kode_event_push("button_bottom_pressed", "", 0, "");
    gate_entered();
    push_fills(200U, KODE_EVENT_QUEUE_DEPTH);
    TEST_ASSERT_EQUAL_UINT(lines0 + 2U, fake_klog_count());
    rec_reset();
    (void)xSemaphoreGive(s_gate_open);
    TEST_ASSERT_TRUE(rec_wait(KODE_EVENT_QUEUE_DEPTH - 1U));

    /* And the ring takes events again, with nothing dropped. */
    const uint32_t dropped1 = kode_event_dropped();

    push_fills(300U, 1U);
    TEST_ASSERT_TRUE(rec_wait(1U));
    TEST_ASSERT_EQUAL_UINT32(dropped1, kode_event_dropped());

    TEST_ASSERT_TRUE(kode_event_remove_listener("button_bottom_pressed", &listener_gate));
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_down_released", &listener_fill));
}

/* ---- Strings --------------------------------------------------------------- */

/** @brief Longest source and text this case pushes. */
#define CUT_PUSHED 300U

static char s_seen_source[KODE_EVENT_SOURCE_MAX + 1U];   /**< The source the listener saw. */
static char s_seen_text[KODE_EVENT_TEXT_MAX + 1U];       /**< The text the listener saw.   */
static char s_seen_name[KODE_EVENT_NAME_MAX + 1U];       /**< The name the listener saw.   */

/** @brief A listener that keeps the strings whole, to measure them. */
static void listener_measure(const kode_event_t *e)
{
    (void)snprintf(s_seen_name, sizeof(s_seen_name), "%s", e->name);
    (void)snprintf(s_seen_source, sizeof(s_seen_source), "%s", e->source);
    (void)snprintf(s_seen_text, sizeof(s_seen_text), "%s", e->text);
    (void)xSemaphoreGive(s_given);
}

TEST_CASE("a push cuts its strings to the limits and keeps the name in lower case", "[kode_event]")
{
    char source[CUT_PUSHED + 1U];
    char text[CUT_PUSHED + 1U];

    events_up();
    (void)memset(source, 's', CUT_PUSHED);
    source[KODE_EVENT_SOURCE_MAX + 6U] = '\0';
    (void)memset(text, 't', CUT_PUSHED);
    text[CUT_PUSHED] = '\0';

    TEST_ASSERT_TRUE(kode_event_add_listener("button_top_released", &listener_measure));
    /* A later module may push in any case; the listener still matches. */
    kode_event_push("Button_TOP_Released", source, 5, text);
    TEST_ASSERT_TRUE(rec_wait(1U));

    TEST_ASSERT_EQUAL_STRING("button_top_released", s_seen_name);
    TEST_ASSERT_EQUAL_UINT(KODE_EVENT_SOURCE_MAX - 1U, strlen(s_seen_source));
    TEST_ASSERT_EQUAL_UINT(KODE_EVENT_TEXT_MAX - 1U, strlen(s_seen_text));
    TEST_ASSERT_EQUAL_CHAR('s', s_seen_source[KODE_EVENT_SOURCE_MAX - 2U]);
    TEST_ASSERT_EQUAL_CHAR('t', s_seen_text[KODE_EVENT_TEXT_MAX - 2U]);
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_top_released", &listener_measure));

    /* A name longer than the limit is cut too, and still ends in a NUL. */
    char name[CUT_PUSHED + 1U];

    (void)memset(name, 'n', CUT_PUSHED);
    name[CUT_PUSHED] = '\0';
    kode_event_push(name, NULL, 0, NULL);
    kode_event_push("button_up_pressed", NULL, 0, NULL);
    TEST_ASSERT_TRUE(kode_event_add_listener("button_up_pressed", &listener_measure));
    kode_event_push("button_up_pressed", NULL, 0, NULL);
    TEST_ASSERT_TRUE(rec_wait(1U));
    TEST_ASSERT_EQUAL_STRING("", s_seen_source);
    TEST_ASSERT_EQUAL_STRING("", s_seen_text);
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_up_pressed", &listener_measure));
}

/* ---- Tasks that wait ------------------------------------------------------- */

/** @brief Tasks that wait together: more than any fixed table of places would hold. */
#define WAITER_TASKS 6U

static SemaphoreHandle_t s_waiter_done[WAITER_TASKS];             /**< Given by each task when it is through. */
static volatile bool     s_waiter_got[WAITER_TASKS];              /**< What each wait answered.               */
static char              s_waiter_name[WAITER_TASKS][KODE_EVENT_NAME_MAX];   /**< The name each task read after the wait. */

/** @brief One of the tasks: wait for the up button and keep what it was told. */
static void waiter_task(void *arg)
{
    const uint32_t i = (uint32_t)(uintptr_t)arg;
    kode_event_t   out = { NULL, NULL, -1, NULL, -1, -1 };

    s_waiter_got[i] = kode_event_wait("button_up_pressed", TEST_WAIT_MS, &out);
    if ( s_waiter_got[i] )
    {
        (void)snprintf(s_waiter_name[i], KODE_EVENT_NAME_MAX, "%s", out.name);
    }
    (void)xSemaphoreGive(s_waiter_done[i]);
}

TEST_CASE("many tasks can wait at once, each with a result of its own", "[kode_event]")
{
    kode_event_t mine = { NULL, NULL, -1, NULL, -1, -1 };

    events_up();
    /* This task's own result first; the others must leave it alone. */
    fake_sem_wait_hook(&press_top_while_waiting);
    TEST_ASSERT_TRUE(kode_event_wait("button_top_pressed", TEST_WAIT_MS, &mine));
    fake_sem_wait_hook(NULL);

    kode_status_t started = KODE_OK;

    for ( uint32_t i = 0U; (WAITER_TASKS > i) && (KODE_OK == started); i++ )
    {
        s_waiter_done[i] = xSemaphoreCreateBinary();
        s_waiter_got[i]  = false;
        s_waiter_name[i][0] = '\0';
        /* Higher than this task: each runs until it sleeps in its wait. */
        started = khal_task_start(&waiter_task, "t_wait", TEST_STACK_BYTES,
                                  TEST_PRIO, (void *)(uintptr_t)i);
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK, started);

    publish_button(KDRV_BTN_UP, true);

    bool all = true;

    for ( uint32_t i = 0U; WAITER_TASKS > i; i++ )
    {
        all = all && (xSemaphoreTake(s_waiter_done[i], pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    }
    TEST_ASSERT_TRUE(all);
    for ( uint32_t i = 0U; WAITER_TASKS > i; i++ )
    {
        TEST_ASSERT_TRUE(s_waiter_got[i]);
        TEST_ASSERT_EQUAL_STRING("button_up_pressed", s_waiter_name[i]);
        vSemaphoreDelete(s_waiter_done[i]);
    }

    /* None of that touched what this task was told. */
    TEST_ASSERT_EQUAL_STRING("button_top_pressed", mine.name);
    TEST_ASSERT_EQUAL_STRING("top", mine.source);
}

/* ---- A task's storage goes with the task ----------------------------------- */

/** @brief What the task of one of the storage cases leaves for the case. */
typedef struct {
    SemaphoreHandle_t  done;       /**< Given when the task has noted what it holds.        */
    void              *slot;       /**< The task's khal slot when it was through.           */
    bool               had_error;  /**< kode_error() had text after the failed wait.        */
} ender_t;

/** @brief A task that fails once and waits once, then ends: it holds a struct and a semaphore. */
static void user_task_ends(void *arg)
{
    ender_t *const e = (ender_t *)arg;

    /* Zero ms: the wait is made and gives up at once, with an error text. */
    (void)kode_event_wait("button_up_pressed", 0U, NULL);
    e->had_error = ('\0' != kode_error()[0]);
    e->slot      = khal_task_slot_get();

    const kode_task_t *const t = kode_task_get();

    /* The watches are armed before the task is through: it ends after. */
    fake_task_slot_watch(e->slot);
    fake_sem_watch((NULL != t) ? t->wait_sem : NULL);
    (void)xSemaphoreGive(e->done);
}

/** @brief A task that never calls the layer. */
static void bystander_task_ends(void *arg)
{
    ender_t *const e = (ender_t *)arg;

    e->slot = khal_task_slot_get();
    (void)xSemaphoreGive(e->done);
}

TEST_CASE("a task's storage is freed when the task ends", "[kode_event]")
{
    ender_t e = { NULL, NULL, false };

    events_up();
    e.done = xSemaphoreCreateBinary();
    fake_task_slot_watch(NULL);
    fake_sem_watch(NULL);
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&user_task_ends, "t_ender", TEST_STACK_BYTES,
                                                    TEST_PRIO, &e));
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(e.done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    TEST_ASSERT_TRUE(e.had_error);
    TEST_ASSERT_NOT_NULL(e.slot);

    /* The task is deleted after it gave the semaphore; the cleanup says when. */
    TEST_ASSERT_TRUE(fake_task_slot_wait_ended(TEST_WAIT_MS));
    TEST_ASSERT_EQUAL_UINT32(1U, fake_task_slot_watch_frees());
    TEST_ASSERT_EQUAL_UINT32(1U, fake_sem_watch_deletes());
    vSemaphoreDelete(e.done);
}

TEST_CASE("a task that never used the layer leaves nothing to free", "[kode_event]")
{
    ender_t e = { NULL, NULL, false };

    events_up();
    e.done = xSemaphoreCreateBinary();
    const uint32_t owned = fake_task_slot_owned();

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&bystander_task_ends, "t_bystander",
                                                    TEST_STACK_BYTES, TEST_PRIO, &e));
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(e.done, pdMS_TO_TICKS(TEST_WAIT_MS)));

    /* It took no storage, so nothing was registered to be freed. */
    TEST_ASSERT_NULL(e.slot);
    TEST_ASSERT_EQUAL_UINT32(owned, fake_task_slot_owned());
    vSemaphoreDelete(e.done);
}

/* ---- A wait never receives an event older than itself ---------------------- */

/** @brief The value of the event pushed before the wait began. */
#define OLD_VALUE 1

/** @brief The value of the event pushed after the wait began. */
#define NEW_VALUE 2

/** @brief Where the stale-event waiter leaves its answer. */
static SemaphoreHandle_t s_late_done;           /**< Given by the task when its wait returns.  */
static volatile bool     s_late_got;            /**< What its wait answered.                   */
static volatile int32_t  s_late_value = -1;     /**< The value of the event it was given.      */

/** @brief The task of the stale-event case: wait for the up button and keep the value. */
static void late_waiter_task(void *arg)
{
    kode_event_t out = { NULL, NULL, -1, NULL, -1, -1 };

    (void)arg;
    s_late_got = kode_event_wait("button_up_pressed", TEST_WAIT_MS, &out);
    if ( s_late_got )
    {
        s_late_value = out.value;
    }
    (void)xSemaphoreGive(s_late_done);
}

TEST_CASE("a wait ignores an event pushed before it began", "[kode_event]")
{
    events_up();
    if ( NULL == s_gate_in )
    {
        s_gate_in   = xSemaphoreCreateCounting(REC_MAX, 0U);
        s_gate_open = xSemaphoreCreateCounting(REC_MAX, 0U);
    }
    while ( xSemaphoreTake(s_gate_in, 0U) == pdTRUE )
    {
    }
    while ( xSemaphoreTake(s_gate_open, 0U) == pdTRUE )
    {
    }
    if ( NULL == s_late_done )
    {
        s_late_done = xSemaphoreCreateBinary();
    }
    while ( xSemaphoreTake(s_late_done, 0U) == pdTRUE )
    {
    }
    s_late_got   = false;
    s_late_value = -1;
    TEST_ASSERT_TRUE(kode_event_add_listener("button_bottom_pressed", &listener_gate));
    TEST_ASSERT_TRUE(kode_event_add_listener("button_down_pressed", &listener_barrier));

    /* The dispatch task is held inside the gate listener, so what is pushed
     * now sits in the ring, undelivered, while the wait begins. */
    kode_event_push("button_bottom_pressed", "", 0, "");
    gate_entered();
    kode_event_push("button_up_pressed", "", OLD_VALUE, "");

    /* Higher than this task: it runs until it sleeps in its wait. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&late_waiter_task, "t_late", TEST_STACK_BYTES,
                                                    TEST_PRIO, NULL));

    /* The barrier, behind the old event in the ring, is delivered after it:
     * its listener's call proves the old event has been served to whoever
     * was to have it. */
    kode_event_push("button_down_pressed", "", 0, "");
    (void)xSemaphoreGive(s_gate_open);
    TEST_ASSERT_TRUE(rec_wait(1U));
    TEST_ASSERT_EQUAL_INT(WHO_BARRIER, s_rec[0].who);

    /* The wait is still going: the old event did not end it. */
    TEST_ASSERT_EQUAL_INT(pdFALSE, xSemaphoreTake(s_late_done, 0U));

    /* One pushed after the wait began is the one it receives. */
    kode_event_push("button_up_pressed", "", NEW_VALUE, "");
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_late_done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    TEST_ASSERT_TRUE(s_late_got);
    TEST_ASSERT_EQUAL_INT32(NEW_VALUE, s_late_value);

    TEST_ASSERT_TRUE(kode_event_remove_listener("button_bottom_pressed", &listener_gate));
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_down_pressed", &listener_barrier));
}

TEST_CASE("an entry is after a wait by a count that survives the counter's wrap", "[kode_event]")
{
    /* Plain order. */
    TEST_ASSERT_TRUE(kode_event_is_after(2U, 1U));
    TEST_ASSERT_FALSE(kode_event_is_after(1U, 1U));
    TEST_ASSERT_FALSE(kode_event_is_after(1U, 2U));

    /* Across the wrap: 0 comes after UINT32_MAX, which comes after the one before. */
    TEST_ASSERT_TRUE(kode_event_is_after(0U, UINT32_MAX));
    TEST_ASSERT_TRUE(kode_event_is_after(1U, UINT32_MAX));
    TEST_ASSERT_FALSE(kode_event_is_after(UINT32_MAX, 0U));
    TEST_ASSERT_FALSE(kode_event_is_after(UINT32_MAX - 1U, UINT32_MAX));

    /* The farthest apart that still has an order: half the range minus one. */
    TEST_ASSERT_TRUE(kode_event_is_after(INT32_MAX, 0U));
    TEST_ASSERT_FALSE(kode_event_is_after(0U, INT32_MAX));
}

/* ---- The point ------------------------------------------------------------ */

TEST_CASE("every event that is not the finger carries the point 0, 0", "[kode_event]")
{
    events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener("button_top_pressed", &listener_a));
    TEST_ASSERT_TRUE(kode_event_add_listener("button_top_held", &listener_a));
    TEST_ASSERT_TRUE(kode_event_add_listener("mqtt_message", &listener_a));
    TEST_ASSERT_TRUE(kode_event_add_listener("battery_low", &listener_a));

    publish_button(KDRV_BTN_TOP, true);
    publish_hold(KDRV_BTN_TOP);
    kode_event_push("mqtt_message", "a/topic", 7, "payload");
    kode_event_push("battery_low", NULL, 9, NULL);
    TEST_ASSERT_TRUE(rec_wait(4U));

    TEST_ASSERT_EQUAL_UINT32(4U, s_rec_n);
    for ( uint32_t i = 0U; i < s_rec_n; i++ )
    {
        TEST_ASSERT_EQUAL_INT16(0, s_rec[i].x);
        TEST_ASSERT_EQUAL_INT16(0, s_rec[i].y);
    }
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_top_pressed", &listener_a));
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_top_held", &listener_a));
    TEST_ASSERT_TRUE(kode_event_remove_listener("mqtt_message", &listener_a));
    TEST_ASSERT_TRUE(kode_event_remove_listener("battery_low", &listener_a));
}

/* ---- The cases of other files ---------------------------------------------- */

/* Last, on purpose. The Active Object cannot be stopped, and it is on the bus
 * for the rest of the binary; the cases of test_led_test.c and the like
 * publish a button on a bench of their own and do not wait for the layer's
 * queue to empty. Off the bus, it hears nothing. */
TEST_CASE("the layer leaves the bus for the cases that follow", "[kode_event]")
{
    events_up();
    QActive_unsubscribe(AO_KodeStandard, BUTTON_EVT_SIG);
    QActive_unsubscribe(AO_KodeStandard, BUTTON_HOLD_SIG);
    QActive_unsubscribe(AO_KodeStandard, WIFI_STATE_SIG);
    QActive_unsubscribe(AO_KodeStandard, MQTT_MSG_SIG);
    QActive_unsubscribe(AO_KodeStandard, POWER_EVT_SIG);
    TEST_ASSERT_EQUAL_UINT32(0U, s_rec_n);
}
