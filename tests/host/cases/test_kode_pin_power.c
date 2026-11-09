/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_pin_power.c
 * @brief kode_pin_power_3v3(): the request to AO_Radio, its wait, refusal and timeout.
 */
#include <stdatomic.h>
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
#include "khal_once.h"
#include "kode.h"
#include "kode_entry.h"
#include "kode_standard_ao.h"
#include "kode_sys.h"
#include "kode_sys_priv.h"
#include "kqp.h"
#include "qf_support.h"
#include "standard_ao_support.h"

#define CALLER_STACK   8192U   /**< Stack of a calling task, in bytes.                  */
#define CALLER_PRIO    2U      /**< Level of a calling task.                            */
#define TEST_WAIT_MS   4000U   /**< Longest a case waits for a call or a request.       */
#define QUIET_MS       50U     /**< How long a case watches for something not to come.  */
#define POLL_MS        1U      /**< Between two looks at AO_Radio's queue.              */
#define RADIO_FILL_MAX 64U     /**< Posts that surely fill AO_Radio's queue.            */

/** @brief One call of kode_pin_power_3v3() in a task of its own. */
typedef struct {
    bool              on;                     /**< What it asks.               */
    bool              result;                 /**< What it answered.           */
    char              error[KODE_ERROR_MAX];  /**< kode_error() after it.      */
    SemaphoreHandle_t done;                   /**< Given when it has answered. */
} caller_t;

/** @brief The ends the case publishes as AO_Radio would. */
static const ExpPowerDoneEvt s_on_ok      = { QEVT_INITIALIZER(EXP_POWER_SIG), true,  (uint8_t)KODE_OK };
static const ExpPowerDoneEvt s_on_no      = { QEVT_INITIALIZER(EXP_POWER_SIG), true,  (uint8_t)KODE_IO_ERROR };
static const ExpPowerDoneEvt s_off_ok     = { QEVT_INITIALIZER(EXP_POWER_SIG), false, (uint8_t)KODE_OK };
static const ExpPowerDoneEvt s_off_owed   = { QEVT_INITIALIZER(EXP_POWER_SIG), false, (uint8_t)KODE_DENIED };

/** @brief Call kode_pin_power_3v3(), keep what it said, and say it is over. */
static void caller_task(void *arg)
{
    caller_t *const c = (caller_t *)arg;

    c->result = kode_pin_power_3v3(c->on);
    (void)snprintf(c->error, sizeof(c->error), "%s", kode_error());
    (void)xSemaphoreGive(c->done);
    vTaskDelete(NULL);
}

/** @brief Start a call of kode_pin_power_3v3(@p on) in its own task. */
static void caller_start(caller_t *c, bool on)
{
    c->on       = on;
    c->result   = false;
    c->error[0] = '\0';
    if ( NULL == c->done )
    {
        c->done = xSemaphoreCreateBinary();
    }
    (void)xSemaphoreTake(c->done, 0U);
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(&caller_task, "t_pin_power", CALLER_STACK, c,
                                          CALLER_PRIO, NULL));
}

/** @brief Whether the call answered within @p ms. */
static bool caller_done(caller_t *c, uint32_t ms)
{
    return xSemaphoreTake(c->done, pdMS_TO_TICKS(ms)) == pdTRUE;
}

/** @brief Empty AO_Radio's queue, recycling what it holds. */
static void radio_flush(void)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief Wait for one request in AO_Radio's queue, take it, and say what it asks. */
static bool radio_request_on(void)
{
    const QEvt *e = NULL;

    for ( uint32_t i = 0U; (TEST_WAIT_MS > i) && (uxQueueMessagesWaiting(AO_Radio->eQueue) == 0U);
         i++ )
    {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, uxQueueMessagesWaiting(AO_Radio->eQueue),
                                   "one request reached AO_Radio");
    TEST_ASSERT_TRUE(xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)EXP_POWER_REQ_SIG, (uint32_t)e->sig);

    const bool on = ((const ExpPowerEvt *)e)->on;

    QF_gc(e);
    return on;
}

/** @brief Publish an end, as AO_Radio does, and let the layer hear it. */
static void radio_says(const ExpPowerDoneEvt *e)
{
    QACTIVE_PUBLISH(&e->super, NULL);
    standard_ao_support_drained();
}

/** @brief The layer up, AO_Radio a queue with nothing in it, locks that lock. */
static void power_fresh(void)
{
    qf_support_init();
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    fake_sync_real(true);
    fake_mutex_real(true);
    TEST_ASSERT_TRUE(kode_standard_ao_ensure());
    qf_support_give_queue(AO_Radio);
    radio_flush();
}

/** @brief Nothing left in AO_Radio's queue or in the layer's. */
static void power_done(void)
{
    radio_flush();
    standard_ao_support_drained();
}

TEST_CASE("kode_pin_power_3v3(true) asks AO_Radio for on and answers true once the switch is said ok",
          "[kode_pin_power_3v3]")
{
    static caller_t c;

    power_fresh();
    caller_start(&c, true);
    TEST_ASSERT_TRUE_MESSAGE(radio_request_on(), "the request asks for on");
    TEST_ASSERT_FALSE_MESSAGE(caller_done(&c, QUIET_MS), "answered before the switch was said");

    radio_says(&s_on_ok);
    TEST_ASSERT_TRUE(caller_done(&c, TEST_WAIT_MS));
    TEST_ASSERT_TRUE(c.result);
    TEST_ASSERT_EQUAL_STRING("", c.error);
    power_done();
}

TEST_CASE("a switch the C5 refused is false, with the C5's refusal", "[kode_pin_power_3v3]")
{
    static caller_t c;

    power_fresh();
    caller_start(&c, true);
    TEST_ASSERT_TRUE(radio_request_on());
    radio_says(&s_on_no);
    TEST_ASSERT_TRUE(caller_done(&c, TEST_WAIT_MS));
    TEST_ASSERT_FALSE(c.result);
    TEST_ASSERT_EQUAL_STRING("the C5 would not switch J3's supply", c.error);
    power_done();
}

TEST_CASE("a kode_pin_power_3v3(false) whose link would not come up is false at once, "
          "naming the link",
          "[kode_pin_power_3v3]")
{
    static caller_t c;

    power_fresh();
    caller_start(&c, false);
    TEST_ASSERT_FALSE_MESSAGE(radio_request_on(), "the request asks for off");

    /* AO_Radio's answer when the link it brought up for the switch failed,
     * or went with the switch: the C5 was not asked. */
    radio_says(&s_off_owed);
    TEST_ASSERT_TRUE_MESSAGE(caller_done(&c, QUIET_MS), "the call waited past the answer");
    TEST_ASSERT_FALSE(c.result);
    TEST_ASSERT_EQUAL_STRING("the C5's link would not come up or went down: "
                             "J3's supply switches at its next link-up", c.error);
    power_done();
}

TEST_CASE("an end that names the other switch does not end the wait", "[kode_pin_power_3v3]")
{
    static caller_t c;

    power_fresh();
    caller_start(&c, true);
    TEST_ASSERT_TRUE(radio_request_on());
    radio_says(&s_off_ok);
    TEST_ASSERT_FALSE_MESSAGE(caller_done(&c, QUIET_MS), "an off ended a wait for on");
    radio_says(&s_on_ok);
    TEST_ASSERT_TRUE(caller_done(&c, TEST_WAIT_MS));
    TEST_ASSERT_TRUE(c.result);
    power_done();
}

TEST_CASE("a switch nobody says is over is false after KODE_PIN_POWER_MS, with a timeout line",
          "[kode_pin_power_3v3]")
{
    static caller_t c;

    power_fresh();
    /* The calling task's semaphore is the one-thread fake, made in the call:
     * its wait answers at once and keeps how long it was asked to be. */
    fake_sync_real(false);
    fake_sem_reset();
    caller_start(&c, true);
    TEST_ASSERT_TRUE(caller_done(&c, TEST_WAIT_MS));
    fake_sync_real(true);
    TEST_ASSERT_FALSE(c.result);
    char said[KODE_ERROR_MAX];

    (void)snprintf(said, sizeof(said), "J3's supply did not switch within %u s",
                   (unsigned)(KODE_PIN_POWER_MS / KODE_MS_PER_S));
    TEST_ASSERT_EQUAL_STRING(said, c.error);
    TEST_ASSERT_EQUAL_UINT32(KODE_PIN_POWER_MS, fake_sem_last_wait_ms());
    TEST_ASSERT_TRUE(radio_request_on());

    /* The end that comes late is nobody's: the next call waits for its own. */
    radio_says(&s_on_ok);
    caller_start(&c, true);
    TEST_ASSERT_TRUE(radio_request_on());
    TEST_ASSERT_FALSE_MESSAGE(caller_done(&c, QUIET_MS), "a late end served the next call");
    radio_says(&s_on_ok);
    TEST_ASSERT_TRUE(caller_done(&c, TEST_WAIT_MS));
    TEST_ASSERT_TRUE(c.result);
    power_done();
}

TEST_CASE("two kode_pin_power_3v3() calls take turns: the second asks once the first is answered",
          "[kode_pin_power_3v3]")
{
    static caller_t a;
    static caller_t b;

    power_fresh();
    caller_start(&a, true);
    caller_start(&b, false);
    const bool first = radio_request_on();

    vTaskDelay(pdMS_TO_TICKS(QUIET_MS));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, uxQueueMessagesWaiting(AO_Radio->eQueue),
                                   "the second call asked while the first waited");

    if ( first )
    {
        radio_says(&s_on_ok);
    }
    else
    {
        radio_says(&s_off_ok);
    }
    const bool second = radio_request_on();

    TEST_ASSERT_NOT_EQUAL_MESSAGE(first, second, "each call asked its own switch");
    if ( second )
    {
        radio_says(&s_on_ok);
    }
    else
    {
        radio_says(&s_off_ok);
    }
    TEST_ASSERT_TRUE(caller_done(&a, TEST_WAIT_MS));
    TEST_ASSERT_TRUE(caller_done(&b, TEST_WAIT_MS));
    TEST_ASSERT_TRUE(a.result);
    TEST_ASSERT_TRUE(b.result);
    power_done();
}

TEST_CASE("a request AO_Radio has no room for is false, and leaves no wait behind",
          "[kode_pin_power_3v3]")
{
    static const QEvt filler = QEVT_INITIALIZER(WIFI_STATE_REQ_SIG);
    static caller_t   c;

    power_fresh();
    for ( uint32_t guard = 0U;
         (RADIO_FILL_MAX > guard)
         && (uxQueueSpacesAvailable(AO_Radio->eQueue) > KQP_RADIO_CMD_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_Radio, &filler, NULL);
    }
    TEST_ASSERT_FALSE(kode_pin_power_3v3(true));
    TEST_ASSERT_EQUAL_STRING("the radio is busy, try again", kode_error());
    radio_flush();

    /* An end now finds no wait; the next call waits for its own. */
    radio_says(&s_on_ok);
    caller_start(&c, true);
    TEST_ASSERT_TRUE(radio_request_on());
    TEST_ASSERT_FALSE(caller_done(&c, QUIET_MS));
    radio_says(&s_on_ok);
    TEST_ASSERT_TRUE(caller_done(&c, TEST_WAIT_MS));
    TEST_ASSERT_TRUE(c.result);
    power_done();
}
