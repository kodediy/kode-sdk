/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_pin_event.c
 * @brief A J3 pin that changes: pin_<n>_changed, the 30 ms settle and what a listened pin refuses.
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "Mockkhal_exp_port.h"
#include "Mockkhal_gpio.h"
#include "Mockkhal_pwm.h"

#include "fakes.h"
#include "khal_exp.h"
#include "khal_once.h"
#include "kode.h"
#include "kode_entry.h"
#include "kode_pin_lock.h"
#include "kode_standard_ao.h"
#include "kode_sys.h"
#include "kode_task.h"
#include "kode_sys_priv.h"
#include "kqp.h"
#include "qf_support.h"
#include "standard_ao_support.h"

#define PIN_POS        5U               /**< The pin the cases listen to: EXP5.      */
#define PIN_GPIO       12U              /**< The GPIO behind it.                     */
#define PIN_EVENT      "pin_5_changed"  /**< Its event.                              */
#define PIN_OTHER      6U               /**< A second pin: EXP6.                     */
#define PIN_OTHER_GPIO 13U              /**< The GPIO behind it.                     */
#define PIN_LENT_TAG   "i2c"            /**< Who holds the pin in the lent case.     */
#define GPIO_SLOTS     64U              /**< Room for every GPIO number.             */
#define TEST_WAIT_MS   4000U            /**< Longest a case waits for a listener.    */
#define REC_MAX        16U              /**< Calls a case can record.                */
#define REC_TEXT       32U              /**< Longest name a record keeps.            */
#define BOUNCE_TICKS   10U              /**< Ticks between the bounces of a case.    */
#define BOUNCES        3U               /**< Edges inside one settle.                */
#define WAITER_STACK   8192U            /**< Stack of the waiting task, in bytes.    */
#define WAITER_PRIO    2U               /**< Level of the waiting task.              */
#define WAITER_LOOKS   400U             /**< Looks for the waiter's attach, then its wait. */

/** @brief The ticks a settle takes. */
#define SETTLE_TICKS KQP_TICKS_PER_MS(KODE_PIN_SETTLE_MS)

/** @brief What a listener saw. */
typedef struct {
    char    name[REC_TEXT];   /**< The event's name. */
    char    source[REC_TEXT]; /**< Its source.       */
    int32_t value;            /**< Its value.        */
    int16_t x;                /**< Its point's x.    */
    int16_t y;                /**< Its point's y.    */
} rec_t;

static rec_t             s_rec[REC_MAX];  /**< What the listeners saw, in order. */
static volatile uint32_t s_rec_n;         /**< How many records.                  */
static SemaphoreHandle_t s_given;         /**< Given by a listener per event.     */
static SemaphoreHandle_t s_barrier;       /**< Given by the barrier's listener.   */

static khal_gpio_cb_t s_cb[GPIO_SLOTS];        /**< The interrupt callback per GPIO.     */
static void          *s_arg[GPIO_SLOTS];       /**< Its argument.                         */
static atomic_uint    s_attach_n[GPIO_SLOTS];  /**< khal_gpio_irq_attach() per GPIO.     */
static atomic_uint    s_enable_n[GPIO_SLOTS];  /**< khal_gpio_irq_enable() per GPIO.     */
static atomic_uint    s_release_n[GPIO_SLOTS]; /**< khal_gpio_release() per GPIO.        */
static atomic_uint    s_input_n[GPIO_SLOTS];   /**< khal_gpio_input() per GPIO.          */
static atomic_uint    s_pull[GPIO_SLOTS];      /**< The pull the last input asked for.   */
static atomic_uint    s_kind[GPIO_SLOTS];      /**< What the last attach reacts to.      */
static atomic_bool    s_level[GPIO_SLOTS];     /**< The level each GPIO answers.         */
static atomic_uint    s_attach_fail;           /**< Attaches still to be refused.        */

/* The stand-ins run on the Active Object's thread too, so they only record; the case asserts. */
/** @brief Stand-in for khal_gpio_input(): note it. */
static kode_status_t input_stub(uint8_t pin, khal_gpio_pull_t pull, int calls)
{
    (void)calls;
    atomic_fetch_add(&s_input_n[pin], 1U);
    atomic_store(&s_pull[pin], (unsigned)pull);
    return KODE_OK;
}

/** @brief Stand-in for khal_gpio_output(): accept it. */
static kode_status_t output_stub(uint8_t pin, bool level, int calls)
{
    (void)pin;
    (void)level;
    (void)calls;
    return KODE_OK;
}

/** @brief Stand-in for khal_gpio_irq_attach(): refuse while the case asks, else keep the callback. */
static kode_status_t attach_stub(uint8_t pin, khal_gpio_irq_t kind, khal_gpio_cb_t cb, void *arg,
                                 int calls)
{
    (void)calls;
    if ( atomic_load(&s_attach_fail) > 0U )
    {
        atomic_fetch_sub(&s_attach_fail, 1U);
        return KODE_ERROR;
    }
    s_cb[pin]  = cb;
    s_arg[pin] = arg;
    atomic_store(&s_kind[pin], (unsigned)kind);
    atomic_fetch_add(&s_attach_n[pin], 1U);
    return KODE_OK;
}

/** @brief Stand-in for khal_gpio_irq_enable(): count it. */
static void enable_stub(uint8_t pin, int calls)
{
    (void)calls;
    atomic_fetch_add(&s_enable_n[pin], 1U);
}

/** @brief Stand-in for khal_gpio_release(): the callback is gone. */
static void release_stub(uint8_t pin, int calls)
{
    (void)calls;
    s_cb[pin]  = NULL;
    s_arg[pin] = NULL;
    atomic_fetch_add(&s_release_n[pin], 1U);
}

/** @brief Stand-in for khal_gpio_level(): the case's level. */
static bool level_stub(uint8_t pin, int calls)
{
    (void)calls;
    return atomic_load(&s_level[pin]);
}

/** @brief Record the event and tell the case. */
static void listener_rec(const kode_event_t *e)
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
    (void)xSemaphoreGive(s_given);
}

/** @brief Record the barrier and say it came. */
static void listener_barrier(const kode_event_t *e)
{
    const uint32_t i = s_rec_n;

    if ( REC_MAX > i )
    {
        (void)snprintf(s_rec[i].name, REC_TEXT, "%s", e->name);
    }
    s_rec_n = i + 1U;
    (void)xSemaphoreGive(s_barrier);
}

/** @brief A second listener, to count listeners on one pin. */
static void listener_two(const kode_event_t *e)
{
    (void)e;
}

/** @brief The header's table FREE, the GPIO stood in for, the layer up, the records empty. */
static void pin_event_fresh(void)
{
    Mockkhal_gpio_Init();
    Mockkhal_exp_port_Init();
    Mockkhal_pwm_Init();
    khal_gpio_release_Ignore();
    khal_exp_port_adc_close_Ignore();
    khal_pwm_close_Ignore();
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_init());
    khal_gpio_release_StopIgnore();
    khal_exp_port_adc_close_StopIgnore();
    khal_pwm_close_StopIgnore();
    khal_gpio_input_Stub(&input_stub);
    khal_gpio_output_Stub(&output_stub);
    khal_gpio_irq_attach_Stub(&attach_stub);
    khal_gpio_irq_enable_Stub(&enable_stub);
    khal_gpio_release_Stub(&release_stub);
    khal_gpio_level_Stub(&level_stub);
    for ( uint32_t i = 0U; GPIO_SLOTS > i; i++ )
    {
        s_cb[i]  = NULL;
        s_arg[i] = NULL;
        atomic_store(&s_attach_n[i], 0U);
        atomic_store(&s_enable_n[i], 0U);
        atomic_store(&s_release_n[i], 0U);
        atomic_store(&s_input_n[i], 0U);
        atomic_store(&s_level[i], false);
    }
    atomic_store(&s_attach_fail, 0U);

    qf_support_init();
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    fake_sync_real(true);
    fake_mutex_real(true);
    TEST_ASSERT_TRUE(kode_standard_ao_ensure());
    if ( NULL == s_given )
    {
        s_given   = xSemaphoreCreateCounting(REC_MAX, 0U);
        s_barrier = xSemaphoreCreateBinary();
    }
    while ( xSemaphoreTake(s_given, 0U) == pdTRUE )
    {
    }
    (void)xSemaphoreTake(s_barrier, 0U);
    (void)memset(s_rec, 0, sizeof(s_rec));
    s_rec_n = 0U;
}

/** @brief Every stand-in quiet again, after the layer's queue is empty. */
static void pin_event_done(void)
{
    standard_ao_support_drained();
    Mockkhal_gpio_Verify();
    Mockkhal_exp_port_Verify();
    Mockkhal_pwm_Verify();
    Mockkhal_gpio_Destroy();
    Mockkhal_exp_port_Destroy();
    Mockkhal_pwm_Destroy();
}

/** @brief What @p pos is doing. */
static kode_sys_exp_mode_t pin_mode(uint8_t pos)
{
    kode_sys_exp_info_t info;

    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_info(pos, &info));
    return info.mode;
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

/**
 * @brief Push a barrier behind everything queued and wait for it: once it is
 *        heard, every event before it has been delivered.
 */
static void barrier(void)
{
    standard_ao_support_drained();
    TEST_ASSERT_TRUE(kode_event_add_listener("button_up_released", &listener_barrier));
    kode_event_push("button_up_released", "", 0, "");
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(s_barrier, pdMS_TO_TICKS(TEST_WAIT_MS)));
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_up_released", &listener_barrier));
}

/** @brief The pin's interrupt, as the GPIO driver calls it, then the layer's queue taken empty. */
static void edge(uint8_t gpio)
{
    TEST_ASSERT_NOT_NULL_MESSAGE(s_cb[gpio], "the pin has an interrupt");
    s_cb[gpio](s_arg[gpio]);
    standard_ao_support_drained();
}

/** @brief Turn QP's clock by @p n ticks, then let the layer take what came. */
static void ticks(uint32_t n)
{
    for ( uint32_t i = 0U; i < n; i++ )
    {
        QTIMEEVT_TICK_X(0U, (void *)0);
    }
    standard_ao_support_drained();
}

TEST_CASE("a listener on a pin makes it an input with no pull and attaches its interrupt, any edge",
          "[kode_pin_event]")
{
    pin_event_fresh();
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(PIN_POS));

    TEST_ASSERT_TRUE(kode_event_add_listener(PIN_EVENT, &listener_rec));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_IN, pin_mode(PIN_POS));
    TEST_ASSERT_EQUAL_UINT(1U, atomic_load(&s_input_n[PIN_GPIO]));
    TEST_ASSERT_EQUAL_UINT((unsigned)KHAL_GPIO_PULL_NONE, atomic_load(&s_pull[PIN_GPIO]));
    TEST_ASSERT_EQUAL_UINT(1U, atomic_load(&s_attach_n[PIN_GPIO]));
    TEST_ASSERT_EQUAL_UINT((unsigned)KHAL_GPIO_IRQ_ANY_EDGE, atomic_load(&s_kind[PIN_GPIO]));
    TEST_ASSERT_EQUAL_UINT(1U, atomic_load(&s_enable_n[PIN_GPIO]));

    /* A second listener on the same pin asks nothing more of it. */
    TEST_ASSERT_TRUE(kode_event_add_listener("PIN_5_CHANGED", &listener_two));
    TEST_ASSERT_EQUAL_UINT(1U, atomic_load(&s_attach_n[PIN_GPIO]));

    /* Removing one leaves the pin listened to; removing the last detaches. */
    TEST_ASSERT_TRUE(kode_event_remove_listener(PIN_EVENT, &listener_two));
    TEST_ASSERT_EQUAL_UINT(0U, atomic_load(&s_release_n[PIN_GPIO]));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_IN, pin_mode(PIN_POS));
    TEST_ASSERT_TRUE(kode_event_remove_listener(PIN_EVENT, &listener_rec));
    TEST_ASSERT_EQUAL_UINT(1U, atomic_load(&s_release_n[PIN_GPIO]));
    TEST_ASSERT_NULL(s_cb[PIN_GPIO]);
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(PIN_POS));
    pin_event_done();
}

TEST_CASE("an edge, then 30 ms, pushes the settled level as pin_5_changed, and re-arms the pin",
          "[kode_pin_event]")
{
    pin_event_fresh();
    TEST_ASSERT_TRUE(kode_event_add_listener(PIN_EVENT, &listener_rec));
    const unsigned armed = atomic_load(&s_enable_n[PIN_GPIO]);

    atomic_store(&s_level[PIN_GPIO], true);
    edge(PIN_GPIO);
    ticks(SETTLE_TICKS - 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(armed, atomic_load(&s_enable_n[PIN_GPIO]),
                                   "nothing before the 30 ms are up");
    TEST_ASSERT_EQUAL_UINT32(0U, s_rec_n);
    ticks(1U);
    TEST_ASSERT_TRUE(rec_wait(1U));
    TEST_ASSERT_EQUAL_UINT(armed + 1U, atomic_load(&s_enable_n[PIN_GPIO]));
    TEST_ASSERT_EQUAL_STRING(PIN_EVENT, s_rec[0].name);
    TEST_ASSERT_EQUAL_STRING("", s_rec[0].source);
    TEST_ASSERT_EQUAL_INT32(1, s_rec[0].value);
    TEST_ASSERT_EQUAL_INT16(0, s_rec[0].x);
    TEST_ASSERT_EQUAL_INT16(0, s_rec[0].y);

    /* And back down. */
    atomic_store(&s_level[PIN_GPIO], false);
    edge(PIN_GPIO);
    ticks(SETTLE_TICKS);
    TEST_ASSERT_TRUE(rec_wait(1U));
    TEST_ASSERT_EQUAL_STRING(PIN_EVENT, s_rec[1].name);
    TEST_ASSERT_EQUAL_INT32(0, s_rec[1].value);

    TEST_ASSERT_TRUE(kode_event_remove_listener(PIN_EVENT, &listener_rec));
    pin_event_done();
}

TEST_CASE("three edges inside 30 ms push one event, at the settled level", "[kode_pin_event]")
{
    pin_event_fresh();
    TEST_ASSERT_TRUE(kode_event_add_listener(PIN_EVENT, &listener_rec));

    for ( uint32_t i = 0U; BOUNCES > i; i++ )
    {
        atomic_store(&s_level[PIN_GPIO], (i % 2U) == 0U);
        edge(PIN_GPIO);
        ticks(BOUNCE_TICKS);
    }
    atomic_store(&s_level[PIN_GPIO], true);
    ticks(SETTLE_TICKS);
    barrier();
    TEST_ASSERT_EQUAL_UINT32(2U, s_rec_n);
    TEST_ASSERT_EQUAL_STRING(PIN_EVENT, s_rec[0].name);
    TEST_ASSERT_EQUAL_INT32(1, s_rec[0].value);
    TEST_ASSERT_EQUAL_STRING("button_up_released", s_rec[1].name);

    TEST_ASSERT_TRUE(kode_event_remove_listener(PIN_EVENT, &listener_rec));
    atomic_store(&s_level[PIN_GPIO], false);
    pin_event_done();
}

TEST_CASE("an edge that settles back at the old level pushes nothing, and still re-arms the pin",
          "[kode_pin_event]")
{
    pin_event_fresh();
    TEST_ASSERT_TRUE(kode_event_add_listener(PIN_EVENT, &listener_rec));
    const unsigned armed = atomic_load(&s_enable_n[PIN_GPIO]);

    atomic_store(&s_level[PIN_GPIO], true);
    edge(PIN_GPIO);
    atomic_store(&s_level[PIN_GPIO], false);
    ticks(SETTLE_TICKS);
    TEST_ASSERT_EQUAL_UINT(armed + 1U, atomic_load(&s_enable_n[PIN_GPIO]));
    barrier();
    TEST_ASSERT_EQUAL_UINT32(1U, s_rec_n);
    TEST_ASSERT_EQUAL_STRING("button_up_released", s_rec[0].name);

    TEST_ASSERT_TRUE(kode_event_remove_listener(PIN_EVENT, &listener_rec));
    pin_event_done();
}

TEST_CASE("a listened pin refuses a write, PWM, an analog read and a bus, and reads", "[kode_pin_event]")
{
    pin_event_fresh();
    TEST_ASSERT_TRUE(kode_event_add_listener(PIN_EVENT, &listener_rec));

    TEST_ASSERT_FALSE(kode_pin_write(PIN_POS, true));
    TEST_ASSERT_EQUAL_STRING("pin 5 is listened to", kode_error());
    TEST_ASSERT_FALSE(kode_pin_write_pwm(PIN_POS, 1000U, 50U));
    TEST_ASSERT_EQUAL_STRING("pin 5 is listened to", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_pin_read_analog(PIN_POS));
    TEST_ASSERT_EQUAL_STRING("pin 5 is listened to", kode_error());
    TEST_ASSERT_TRUE(kode_pin_lock_take("kode_i2c_begin"));
    const uint8_t pins[] = { PIN_OTHER, PIN_POS };
    const bool ready = kode_pin_ready_for_bus(pins, sizeof(pins));
    kode_pin_lock_give();
    TEST_ASSERT_FALSE(ready);
    TEST_ASSERT_EQUAL_STRING("pin 5 is listened to", kode_error());
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_IN, pin_mode(PIN_POS));

    /* Reading is what a listened pin is for. */
    atomic_store(&s_level[PIN_GPIO], true);
    TEST_ASSERT_TRUE(kode_pin_read(PIN_POS));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    atomic_store(&s_level[PIN_GPIO], false);

    /* Another pin is not touched by it. */
    TEST_ASSERT_TRUE(kode_pin_write(PIN_OTHER, true));

    /* Once nothing listens, the write goes through. */
    TEST_ASSERT_TRUE(kode_event_remove_listener(PIN_EVENT, &listener_rec));
    TEST_ASSERT_TRUE(kode_pin_write(PIN_POS, true));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_OUT, pin_mode(PIN_POS));
    pin_event_done();
}

TEST_CASE("a pin lent to a bus cannot be listened to, and nothing is registered", "[kode_pin_event]")
{
    pin_event_fresh();
    uint8_t       gpio = 0U;
    const uint8_t pos  = PIN_POS;

    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_lend(&pos, 1U, PIN_LENT_TAG, &gpio));
    TEST_ASSERT_FALSE(kode_event_add_listener(PIN_EVENT, &listener_rec));
    TEST_ASSERT_EQUAL_STRING("'pin_5_changed' cannot be heard: pin 5 is lent to i2c", kode_error());
    TEST_ASSERT_EQUAL_UINT(0U, atomic_load(&s_attach_n[PIN_GPIO]));
    TEST_ASSERT_FALSE(kode_event_remove_listener(PIN_EVENT, &listener_rec));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_LENT, pin_mode(PIN_POS));
    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_return(PIN_LENT_TAG));

    /* Given back, it can be. */
    TEST_ASSERT_TRUE(kode_event_add_listener(PIN_EVENT, &listener_rec));
    TEST_ASSERT_TRUE(kode_event_remove_listener(PIN_EVENT, &listener_rec));
    pin_event_done();
}

TEST_CASE("pin_0_changed, pin_15_changed and other near names are refused as unknown",
          "[kode_pin_event]")
{
    static const char *const bad[] = {
        "pin_0_changed", "pin_15_changed", "pin_05_changed", "pin__changed", "pin_5",
        "pin_5_changed_", "pin_5_change", "pin_x_changed", "pin_100_changed",
    };
    kode_event_t out;

    pin_event_fresh();
    for ( size_t i = 0U; (sizeof(bad) / sizeof(bad[0])) > i; i++ )
    {
        char want[KODE_ERROR_MAX];

        (void)snprintf(want, sizeof(want), "unknown event '%s'", bad[i]);
        TEST_ASSERT_FALSE(kode_event_add_listener(bad[i], &listener_rec));
        TEST_ASSERT_EQUAL_STRING(want, kode_error());
        TEST_ASSERT_FALSE(kode_event_wait(bad[i], 0U, &out));
        TEST_ASSERT_EQUAL_STRING(want, kode_error());
    }

    /* Every pin from 1 to 14 has its event. */
    for ( uint8_t pin = KODE_PIN_FIRST; KODE_PIN_LAST >= pin; pin++ )
    {
        char name[KODE_EVENT_NAME_MAX];

        (void)snprintf(name, sizeof(name), "pin_%u_changed", (unsigned)pin);
        TEST_ASSERT_TRUE_MESSAGE(kode_event_add_listener(name, &listener_rec), name);
        TEST_ASSERT_TRUE(kode_event_remove_listener(name, &listener_rec));
    }
    pin_event_done();
}

/** @brief What the waiting task got. */
static _Atomic(kode_task_t *) s_waiter_state;   /**< The waiting task's layer state, once it has one. */
static volatile bool s_waited;
static kode_event_t  s_waited_ev;
static int32_t       s_waited_value;
static char          s_waited_name[REC_TEXT];
static SemaphoreHandle_t s_waiter_done;

/** @brief Wait for pin_5_changed, then say so. */
static void waiter_task(void *arg)
{
    (void)arg;
    atomic_store(&s_waiter_state, kode_task_ensure());
    s_waited = kode_event_wait(PIN_EVENT, TEST_WAIT_MS, &s_waited_ev);
    if ( s_waited )
    {
        s_waited_value = s_waited_ev.value;
        (void)snprintf(s_waited_name, sizeof(s_waited_name), "%s", s_waited_ev.name);
    }
    (void)xSemaphoreGive(s_waiter_done);
    vTaskDelete(NULL);
}

/**
 * @brief Whether the waiting task is on the layer's list of waiters: from
 *        then on an event pushed is its, even before it sleeps.
 */
static bool waiter_is_waiting(void)
{
    kode_task_t *const t = atomic_load(&s_waiter_state);

    return (NULL != t) && __atomic_load_n(&t->waiting, __ATOMIC_ACQUIRE);
}

TEST_CASE("kode_event_wait on a pin listens for the wait alone, and gets the settled level",
          "[kode_pin_event]")
{
    pin_event_fresh();
    if ( NULL == s_waiter_done )
    {
        s_waiter_done = xSemaphoreCreateBinary();
    }
    s_waited = false;
    s_waited_name[0] = '\0';
    atomic_store(&s_waiter_state, NULL);
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(&waiter_task, "t_pin_wait", WAITER_STACK, NULL,
                                          WAITER_PRIO, NULL));
    for ( uint32_t i = 0U; (WAITER_LOOKS > i) && (atomic_load(&s_attach_n[PIN_GPIO]) == 0U); i++ )
    {
        vTaskDelay(1U);
    }
    TEST_ASSERT_EQUAL_UINT(1U, atomic_load(&s_attach_n[PIN_GPIO]));
    for ( uint32_t i = 0U; (WAITER_LOOKS > i) && !waiter_is_waiting(); i++ )
    {
        vTaskDelay(1U);
    }
    TEST_ASSERT_TRUE_MESSAGE(waiter_is_waiting(), "the waiter never reached its wait");
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_IN, pin_mode(PIN_POS));

    atomic_store(&s_level[PIN_GPIO], true);
    edge(PIN_GPIO);
    ticks(SETTLE_TICKS);
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(s_waiter_done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    TEST_ASSERT_TRUE(s_waited);
    TEST_ASSERT_EQUAL_STRING(PIN_EVENT, s_waited_name);
    TEST_ASSERT_EQUAL_INT32(1, s_waited_value);

    /* The wait was the only listener: the pin is let go. */
    TEST_ASSERT_EQUAL_UINT(1U, atomic_load(&s_release_n[PIN_GPIO]));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(PIN_POS));
    atomic_store(&s_level[PIN_GPIO], false);
    pin_event_done();
}

TEST_CASE("a wait that times out lets the pin go, and a listener keeps it past a wait",
          "[kode_pin_event]")
{
    kode_event_t out;

    pin_event_fresh();
    TEST_ASSERT_FALSE(kode_event_wait(PIN_EVENT, 1U, &out));
    TEST_ASSERT_EQUAL_STRING("no 'pin_5_changed' event within 1 ms", kode_error());
    TEST_ASSERT_EQUAL_UINT(1U, atomic_load(&s_attach_n[PIN_GPIO]));
    TEST_ASSERT_EQUAL_UINT(1U, atomic_load(&s_release_n[PIN_GPIO]));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(PIN_POS));

    TEST_ASSERT_TRUE(kode_event_add_listener(PIN_EVENT, &listener_rec));
    TEST_ASSERT_FALSE(kode_event_wait(PIN_EVENT, 1U, &out));
    TEST_ASSERT_EQUAL_UINT(1U, atomic_load(&s_release_n[PIN_GPIO]));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_IN, pin_mode(PIN_POS));
    TEST_ASSERT_TRUE(kode_event_remove_listener(PIN_EVENT, &listener_rec));
    TEST_ASSERT_EQUAL_UINT(2U, atomic_load(&s_release_n[PIN_GPIO]));
    pin_event_done();
}

TEST_CASE("an edge after the last listener left pushes nothing and arms nothing", "[kode_pin_event]")
{
    pin_event_fresh();
    TEST_ASSERT_TRUE(kode_event_add_listener(PIN_EVENT, &listener_rec));
    khal_gpio_cb_t const cb  = s_cb[PIN_GPIO];
    void *const          arg = s_arg[PIN_GPIO];

    TEST_ASSERT_TRUE(kode_event_remove_listener(PIN_EVENT, &listener_rec));
    const unsigned armed = atomic_load(&s_enable_n[PIN_GPIO]);

    /* An interrupt already on its way as the listener left. */
    atomic_store(&s_level[PIN_GPIO], true);
    cb(arg);
    standard_ao_support_drained();
    ticks(SETTLE_TICKS);
    TEST_ASSERT_EQUAL_UINT(armed, atomic_load(&s_enable_n[PIN_GPIO]));
    barrier();
    TEST_ASSERT_EQUAL_UINT32(1U, s_rec_n);
    atomic_store(&s_level[PIN_GPIO], false);
    pin_event_done();
}

TEST_CASE("an interrupt that will not attach refuses the listener, names the event, and leaves the pin free",
          "[kode_pin_event]")
{
    pin_event_fresh();
    atomic_store(&s_attach_fail, 1U);
    TEST_ASSERT_FALSE(kode_event_add_listener(PIN_EVENT, &listener_rec));
    TEST_ASSERT_EQUAL_STRING("'pin_5_changed' cannot be heard: pin 5's interrupt could not be attached",
                             kode_error());
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(PIN_POS));
    TEST_ASSERT_EQUAL_UINT(0U, atomic_load(&s_attach_n[PIN_GPIO]));
    TEST_ASSERT_NULL(s_cb[PIN_GPIO]);
    TEST_ASSERT_FALSE_MESSAGE(kode_event_remove_listener(PIN_EVENT, &listener_rec),
                              "a refused listener was registered");

    /* Nothing of the refusal is left: the pin drives, then listens. */
    TEST_ASSERT_TRUE(kode_pin_write(PIN_POS, true));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_OUT, pin_mode(PIN_POS));
    TEST_ASSERT_TRUE(kode_event_add_listener(PIN_EVENT, &listener_rec));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_IN, pin_mode(PIN_POS));
    TEST_ASSERT_EQUAL_UINT(1U, atomic_load(&s_attach_n[PIN_GPIO]));
    TEST_ASSERT_TRUE(kode_event_remove_listener(PIN_EVENT, &listener_rec));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(PIN_POS));
    pin_event_done();
}

TEST_CASE("one listener added twice and removed once leaves the pin free", "[kode_pin_event]")
{
    pin_event_fresh();
    TEST_ASSERT_TRUE(kode_event_add_listener(PIN_EVENT, &listener_rec));
    TEST_ASSERT_TRUE(kode_event_add_listener(PIN_EVENT, &listener_rec));
    TEST_ASSERT_EQUAL_UINT(1U, atomic_load(&s_attach_n[PIN_GPIO]));

    TEST_ASSERT_TRUE(kode_event_remove_listener(PIN_EVENT, &listener_rec));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, atomic_load(&s_release_n[PIN_GPIO]),
                                   "the second add kept a hold on the pin");
    TEST_ASSERT_NULL(s_cb[PIN_GPIO]);
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(PIN_POS));
    TEST_ASSERT_FALSE(kode_event_remove_listener(PIN_EVENT, &listener_rec));
    pin_event_done();
}
