/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_battery.c
 * @brief kode_battery_*() and kode_power_off() over a scripted reading and the real rest door (AO_LowPower), and the battery_low edge.
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
#include "kbsp_cell.h"
#include "kdrv_bq25896.h"
#include "kode.h"
#include "kode_battery.h"
#include "kode_entry.h"
#include "kode_standard_ao.h"
#include "kode_sys.h"
#include "kode_sys_priv.h"
#include "kqp.h"
#include "kqp_config.h"
#include "qf_support.h"
#include "sdkconfig.h"
#include "standard_ao_support.h"

#define POWER_FILL_MAX   64U    /**< Posts that surely fill AO_Power's or AO_LowPower's queue. */
#define READING_MV       3912U  /**< The cell voltage the scripted reading carries.      */
#define READING_PCT      57U    /**< Its level.                                          */
#define PMIC_MV          3900U  /**< A voltage in the PMIC's 20 mV steps.                */
#define READY_AT_READ    5U     /**< The read at which a late reading appears.           */
#define RETRY_SLACK_MS   (2U * KODE_RETRY_STEP_MS)  /**< How far past its bound a wait may run. */
#define CALLER_STACK     8192U  /**< Stack of a calling task, in bytes.                  */
#define CALLER_PRIO      2U     /**< Level of a calling task.                            */
#define TEST_WAIT_MS     4000U  /**< Longest a case waits for a call or a request.       */
#define QUIET_MS         50U    /**< How long a case watches for something not to come.  */

/* ---- The facade's seam: the reading, scripted --------------------------------- */

static kode_sys_battery_t s_seam_reading;   /**< What the scripted facade returns.               */
static bool            s_seam_has;       /**< The facade has a reading to return.             */
static uint32_t        s_seam_ready_at;  /**< The read from which it has one; 0 for the first. */
static uint32_t        s_seam_reads;     /**< How many reads were asked.                      */

static kode_status_t seam_battery_read(kode_sys_battery_t *out);
uint8_t seam_battery_level(void);
bool    seam_battery_is_charging(void);
uint32_t seam_battery_voltage(void);
bool    seam_power_off(void);
bool    seam_battery_usb_present(void);
bool    seam_power_charge(bool on);
bool    seam_power_charge_limit(uint16_t ma);
bool    seam_power_boost(bool on);
bool    seam_battery_low_edge(bool *low, const kode_sys_battery_t *r);

#define kode_sys_battery_read  seam_battery_read
#define kode_battery_level       seam_battery_level
#define kode_battery_is_charging seam_battery_is_charging
#define kode_battery_voltage_mv     seam_battery_voltage
#define kode_power_off           seam_power_off
#define kode_power_is_usb_connected seam_battery_usb_present
#define kode_power_set_charging        seam_power_charge
#define kode_power_set_charge_current  seam_power_charge_limit
#define kode_pin_power_5v         seam_power_boost
#define kode_battery_low_edge    seam_battery_low_edge
#include "kode_battery.c"
#undef kode_sys_battery_read
#undef kode_battery_level
#undef kode_battery_is_charging
#undef kode_battery_voltage_mv
#undef kode_power_off
#undef kode_power_is_usb_connected
#undef kode_power_set_charging
#undef kode_power_set_charge_current
#undef kode_pin_power_5v
#undef kode_battery_low_edge

static kode_status_t seam_battery_read(kode_sys_battery_t *out)
{
    kode_status_t rc = KODE_DENIED;

    s_seam_reads++;
    if ( s_seam_has && (s_seam_reads > s_seam_ready_at) )
    {
        *out = s_seam_reading;
        rc   = KODE_OK;
    }
    return rc;
}

/** @brief The world: the lock real, the system up, AO_Power's and AO_LowPower's queues empty, no reading, the clock at zero. */
static void fresh(void)
{
    const QEvt *e = NULL;

    fake_mutex_real(true);
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    qf_support_init();
    qf_support_give_queue(AO_Power);
    while ( xQueueReceive(AO_Power->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
#if CONFIG_KODE_LOWPOWER
    qf_support_give_queue(AO_LowPower);
    while ( xQueueReceive(AO_LowPower->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
#endif
    fake_sem_reset();
    fake_time_reset();
    (void)memset(&s_seam_reading, 0, sizeof(s_seam_reading));
    s_seam_has      = false;
    s_seam_ready_at = 0U;
    s_seam_reads    = 0U;
}

/** @brief Script a reading with the gauge answering. */
static void script(uint16_t mv, uint8_t pct, uint8_t charging, bool vbus, bool gauge_ok)
{
    s_seam_reading.vbat_mv  = mv;
    s_seam_reading.soc_pct  = pct;
    s_seam_reading.charging = charging;
    s_seam_reading.vbus     = vbus;
    s_seam_reading.gauge_ok = gauge_ok;
    s_seam_has              = true;
}

/** @brief Fill AO_Power's queue until only its margin is free. */
static void power_fill_to_margin(void)
{
    static const QEvt filler = QEVT_INITIALIZER(POWER_DUMP_REQ_SIG);

    for ( uint32_t guard = 0U;
         (POWER_FILL_MAX > guard)
         && (uxQueueSpacesAvailable(AO_Power->eQueue) > KQP_POWER_CMD_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_Power, &filler, NULL);
    }
    TEST_ASSERT_EQUAL_UINT(KQP_POWER_CMD_MARGIN, uxQueueSpacesAvailable(AO_Power->eQueue));
}

#if CONFIG_KODE_LOWPOWER
/** @brief Whether AO_LowPower's queue holds a request for @p target from an app; empties it. */
static bool lowpower_queue_has(kode_lp_state_t target)
{
    const QEvt *e = NULL;
    bool        found = false;

    while ( xQueueReceive(AO_LowPower->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)LOWPOWER_REQ_SIG == e->sig )
        {
            const LowPowerReqEvt *r = (const LowPowerReqEvt *)e;

            found = found || (((uint8_t)target == r->target)
                              && ((uint8_t)KODE_LOWPOWER_SRC_APP == r->src));
        }
        QF_gc(e);
    }
    return found;
}

/** @brief Fill AO_LowPower's queue until only its command margin is free. */
static void lowpower_fill_to_margin(void)
{
    static const QEvt filler = QEVT_INITIALIZER(LOWPOWER_DARK_DONE_SIG);

    for ( uint32_t guard = 0U;
         (POWER_FILL_MAX > guard)
         && (uxQueueSpacesAvailable(AO_LowPower->eQueue) > KQP_LOWPOWER_CMD_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_LowPower, &filler, NULL);
    }
    TEST_ASSERT_EQUAL_UINT(KQP_LOWPOWER_CMD_MARGIN, uxQueueSpacesAvailable(AO_LowPower->eQueue));
}
#endif

/* ---- The readings -------------------------------------------------------------- */

TEST_CASE("level, voltage and charging come from the reading", "[kode_battery]")
{
    fresh();
    script(READING_MV, READING_PCT, KODE_SYS_CHARGE_NONE, false, true);

    TEST_ASSERT_EQUAL_UINT8(READING_PCT, seam_battery_level());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(READING_MV, seam_battery_voltage());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_FALSE(seam_battery_is_charging());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, kode_sys_time_ms(), "a reading that is there is not waited for");
}

TEST_CASE("pre-charge and fast charge are charging, done and none are not", "[kode_battery]")
{
    fresh();
    script(READING_MV, READING_PCT, KODE_SYS_CHARGE_PRE, true, true);
    TEST_ASSERT_TRUE(seam_battery_is_charging());
    s_seam_reading.charging = KODE_SYS_CHARGE_FAST;
    TEST_ASSERT_TRUE(seam_battery_is_charging());
    s_seam_reading.charging = KODE_SYS_CHARGE_DONE;
    TEST_ASSERT_FALSE(seam_battery_is_charging());
    s_seam_reading.charging = KODE_SYS_CHARGE_NONE;
    TEST_ASSERT_FALSE(seam_battery_is_charging());
}

TEST_CASE("with no reading every call waits its bound, then answers 0 and says why", "[kode_battery]")
{
    fresh();

    TEST_ASSERT_EQUAL_UINT8(0U, seam_battery_level());
    TEST_ASSERT_EQUAL_STRING("the battery has not been read yet", kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_BATTERY_WAIT_MS, kode_sys_time_ms());
    TEST_ASSERT_LESS_THAN_UINT32(KODE_BATTERY_WAIT_MS + RETRY_SLACK_MS, kode_sys_time_ms());

    fake_time_reset();
    TEST_ASSERT_EQUAL_UINT32(0U, seam_battery_voltage());
    TEST_ASSERT_EQUAL_STRING("the battery has not been read yet", kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_BATTERY_WAIT_MS, kode_sys_time_ms());

    fake_time_reset();
    TEST_ASSERT_FALSE(seam_battery_is_charging());
    TEST_ASSERT_EQUAL_STRING("the battery has not been read yet", kode_error());
}

TEST_CASE("a reading that appears within the bound is the answer", "[kode_battery]")
{
    fresh();
    script(READING_MV, READING_PCT, KODE_SYS_CHARGE_NONE, false, true);
    s_seam_ready_at = READY_AT_READ;

    TEST_ASSERT_EQUAL_UINT8(READING_PCT, seam_battery_level());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(READY_AT_READ * KODE_RETRY_STEP_MS, kode_sys_time_ms());
    TEST_ASSERT_LESS_THAN_UINT32(KODE_BATTERY_WAIT_MS, kode_sys_time_ms());
}

TEST_CASE("a gauge that does not answer gives no level, but the voltage and the charge are the PMIC's",
          "[kode_battery]")
{
    fresh();
    script(PMIC_MV, 0U, KODE_SYS_CHARGE_FAST, true, false);

    TEST_ASSERT_EQUAL_UINT8(0U, seam_battery_level());
    TEST_ASSERT_EQUAL_STRING("the battery gauge does not answer", kode_error());
    TEST_ASSERT_EQUAL_UINT32(PMIC_MV, seam_battery_voltage());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_TRUE(seam_battery_is_charging());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

/* ---- Power off ------------------------------------------------------------------ */

TEST_CASE("power off with USB plugged in is refused and posts nothing", "[kode_battery]")
{
    fresh();
    script(READING_MV, READING_PCT, KODE_SYS_CHARGE_FAST, true, true);

    TEST_ASSERT_FALSE(seam_power_off());
    TEST_ASSERT_EQUAL_STRING("the board cannot switch off while USB is plugged in", kode_error());
    TEST_ASSERT_EQUAL_UINT(0U, uxQueueMessagesWaiting(AO_Power->eQueue));
#if CONFIG_KODE_LOWPOWER
    TEST_ASSERT_EQUAL_UINT(0U, uxQueueMessagesWaiting(AO_LowPower->eQueue));
#endif
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, kode_sys_time_ms(), "a refusal does not wait");
}

#if CONFIG_KODE_LOWPOWER
TEST_CASE("power off on battery asks AO_LowPower for OFF, waits, and says the board is still running",
          "[kode_battery]")
{
    fresh();
    script(READING_MV, READING_PCT, KODE_SYS_CHARGE_NONE, false, true);

    TEST_ASSERT_FALSE(seam_power_off());
    TEST_ASSERT_EQUAL_STRING("the board did not switch off", kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(POWER_OFF_WAIT_MS, kode_sys_time_ms());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, uxQueueMessagesWaiting(AO_Power->eQueue),
                                   "an app reached ship mode");
    TEST_ASSERT_EQUAL_UINT(1U, uxQueueMessagesWaiting(AO_LowPower->eQueue));
    TEST_ASSERT_TRUE(lowpower_queue_has(KODE_LP_OFF));
}
#endif

TEST_CASE("a request the power manager does not know is refused, named, and switches nothing off",
          "[kode_battery]")
{
    fresh();
    script(READING_MV, READING_PCT, KODE_SYS_CHARGE_NONE, false, true);

    TEST_ASSERT_FALSE(power_ask((power_ask_t)((int)POWER_ASK_BOOST + 1), 1U));
    TEST_ASSERT_EQUAL_STRING("the power manager was asked for a request it does not know",
                             kode_error());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, uxQueueMessagesWaiting(AO_Power->eQueue),
                                   "an unknown request posted something");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, kode_sys_time_ms(), "a refusal does not wait");
}

#if CONFIG_KODE_LOWPOWER
TEST_CASE("power off on a queue within the margin waits its bound and says power is busy", "[kode_battery]")
{
    fresh();
    script(READING_MV, READING_PCT, KODE_SYS_CHARGE_NONE, false, true);
    lowpower_fill_to_margin();

    TEST_ASSERT_FALSE(seam_power_off());
    TEST_ASSERT_EQUAL_STRING("the power manager is busy", kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_BATTERY_WAIT_MS, kode_sys_time_ms());
    TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(2U * KODE_BATTERY_WAIT_MS, kode_sys_time_ms(),
                                         "no wait for the switch-off after a post that never happened");
    TEST_ASSERT_FALSE(lowpower_queue_has(KODE_LP_OFF));
}
#endif

TEST_CASE("power off with no reading cannot tell whether USB is there, so it posts nothing", "[kode_battery]")
{
    fresh();

    TEST_ASSERT_FALSE(seam_power_off());
    TEST_ASSERT_EQUAL_STRING("the battery has not been read yet", kode_error());
    TEST_ASSERT_EQUAL_UINT(0U, uxQueueMessagesWaiting(AO_Power->eQueue));
#if CONFIG_KODE_LOWPOWER
    TEST_ASSERT_EQUAL_UINT(0U, uxQueueMessagesWaiting(AO_LowPower->eQueue));
#endif
}

/* ---- USB ------------------------------------------------------------------------ */

TEST_CASE("usb_present is the reading's vbus", "[kode_battery]")
{
    fresh();
    script(READING_MV, READING_PCT, KODE_SYS_CHARGE_FAST, true, true);
    TEST_ASSERT_TRUE(seam_battery_usb_present());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    s_seam_reading.vbus = false;
    TEST_ASSERT_FALSE(seam_battery_usb_present());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, kode_sys_time_ms(), "a reading that is there is not waited for");
}

TEST_CASE("usb_present with no reading waits its bound, answers false and says why", "[kode_battery]")
{
    fresh();

    TEST_ASSERT_FALSE(seam_battery_usb_present());
    TEST_ASSERT_EQUAL_STRING("the battery has not been read yet", kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_BATTERY_WAIT_MS, kode_sys_time_ms());
    TEST_ASSERT_LESS_THAN_UINT32(KODE_BATTERY_WAIT_MS + RETRY_SLACK_MS, kode_sys_time_ms());
}

/* ---- Charge, charge limit and boost -------------------------------------------- */

/** @brief The request the next event in AO_Power's queue carries; the queue ends up empty. */
static PowerCtrlReqEvt s_ctrl;

/** @brief Take AO_Power's one waiting request into s_ctrl. @return true when there was one. */
static bool power_take_ctrl(void)
{
    const QEvt *e     = NULL;
    bool        found = false;

    if ( xQueueReceive(AO_Power->eQueue, &e, 0U) == pdTRUE )
    {
        TEST_ASSERT_EQUAL_INT(POWER_CTRL_REQ_SIG, e->sig);
        s_ctrl = *(const PowerCtrlReqEvt *)(const void *)e;
        QF_gc(e);
        found = true;
    }
    return found;
}

TEST_CASE("charge posts the bit and does not need a reading", "[kode_battery]")
{
    fresh();

    TEST_ASSERT_TRUE(seam_power_charge(false));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_TRUE(power_take_ctrl());
    TEST_ASSERT_EQUAL_UINT8(POWER_CTRL_CHARGE, s_ctrl.op);
    TEST_ASSERT_EQUAL_INT32(0, s_ctrl.value);

    TEST_ASSERT_TRUE(seam_power_charge(true));
    TEST_ASSERT_TRUE(power_take_ctrl());
    TEST_ASSERT_EQUAL_UINT8(POWER_CTRL_CHARGE, s_ctrl.op);
    TEST_ASSERT_EQUAL_INT32(1, s_ctrl.value);
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_reads);
    TEST_ASSERT_EQUAL_UINT32(0U, kode_sys_time_ms());
}

TEST_CASE("charge on a queue within the margin waits its bound and says power is busy", "[kode_battery]")
{
    fresh();
    power_fill_to_margin();

    TEST_ASSERT_FALSE(seam_power_charge(true));
    TEST_ASSERT_EQUAL_STRING("the power manager is busy", kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_BATTERY_WAIT_MS, kode_sys_time_ms());
    TEST_ASSERT_LESS_THAN_UINT32(KODE_BATTERY_WAIT_MS + RETRY_SLACK_MS, kode_sys_time_ms());
}

TEST_CASE("charge_limit posts the current inside the pack's range", "[kode_battery]")
{
    fresh();

    TEST_ASSERT_TRUE(seam_power_charge_limit(KODE_CHARGE_CURRENT_MIN_MA));
    TEST_ASSERT_TRUE(power_take_ctrl());
    TEST_ASSERT_EQUAL_UINT8(POWER_CTRL_ICHG, s_ctrl.op);
    TEST_ASSERT_EQUAL_INT32(KODE_CHARGE_CURRENT_MIN_MA, s_ctrl.value);

    TEST_ASSERT_TRUE(seam_power_charge_limit(KBSP_CELL_ICHG_MAX_MA));
    TEST_ASSERT_TRUE(power_take_ctrl());
    TEST_ASSERT_EQUAL_INT32(KBSP_CELL_ICHG_MAX_MA, s_ctrl.value);
    TEST_ASSERT_EQUAL_UINT32(KBSP_CELL_ICHG_MAX_MA, KODE_CHARGE_CURRENT_MAX_MA);
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("charge_limit outside the range is refused naming it, and posts nothing", "[kode_battery]")
{
    const uint16_t bad[] = { 0U, KODE_CHARGE_CURRENT_MIN_MA - 1U, KBSP_CELL_ICHG_MAX_MA + 1U, UINT16_MAX };

    fresh();
    for ( uint32_t i = 0U; (sizeof(bad) / sizeof(bad[0])) > i; ++i )
    {
        TEST_ASSERT_FALSE(seam_power_charge_limit(bad[i]));
        TEST_ASSERT_EQUAL_STRING("charge limit out of range: use 64 to 896 mA", kode_error());
        TEST_ASSERT_EQUAL_UINT(0U, uxQueueMessagesWaiting(AO_Power->eQueue));
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, kode_sys_time_ms(), "a refusal does not wait");
}

/** @brief One call of kode_pin_power_5v() in a task of its own. */
typedef struct {
    bool              on;                     /**< What it asks.               */
    bool              result;                 /**< What it answered.           */
    char              error[KODE_ERROR_MAX];  /**< kode_error() after it.      */
    SemaphoreHandle_t done;                   /**< Given when it has answered. */
} boost_caller_t;

/** @brief Call kode_pin_power_5v(), keep what it said, and say it is over. */
static void boost_caller_task(void *arg)
{
    boost_caller_t *const c = (boost_caller_t *)arg;

    c->result = seam_power_boost(c->on);
    (void)snprintf(c->error, sizeof(c->error), "%s", kode_error());
    (void)xSemaphoreGive(c->done);
    vTaskDelete(NULL);
}

/** @brief The layer up, locks and semaphores real, then a call of kode_pin_power_5v(@p on) in its own task. */
static void boost_call(boost_caller_t *c, bool on)
{
    fake_sync_real(true);
    TEST_ASSERT_TRUE(kode_standard_ao_ensure());
    QActive_subscribe(AO_KodeStandard, POWER_CTRL_SIG);
    c->on       = on;
    c->result   = false;
    c->error[0] = '\0';
    if ( NULL == c->done )
    {
        c->done = xSemaphoreCreateBinary();
    }
    (void)xSemaphoreTake(c->done, 0U);
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(&boost_caller_task, "t_boost", CALLER_STACK, c,
                                          CALLER_PRIO, NULL));
}

/** @brief Whether the call answered within @p ms. */
static bool boost_call_done(boost_caller_t *c, uint32_t ms)
{
    return xSemaphoreTake(c->done, pdMS_TO_TICKS(ms)) == pdTRUE;
}

/** @brief Wait for the call's request in AO_Power's queue and take it into s_ctrl. */
static void power_request_wait(void)
{
    for ( uint32_t i = 0U; (TEST_WAIT_MS > i) && (uxQueueMessagesWaiting(AO_Power->eQueue) == 0U); i++ )
    {
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
    TEST_ASSERT_TRUE_MESSAGE(power_take_ctrl(), "no request reached AO_Power");
    TEST_ASSERT_EQUAL_UINT8(POWER_CTRL_BOOST, s_ctrl.op);
}

/** @brief Publish AO_Power's answer to the request, and let the layer hear it. */
static void power_answers(uint8_t op, kode_status_t rc, uint8_t why, int32_t value)
{
    static PowerCtrlEvt a;

    a.super = (QEvt)QEVT_INITIALIZER(POWER_CTRL_SIG);
    a.op    = op;
    a.rc    = (uint8_t)rc;
    a.why   = why;
    a.value = value;
    QACTIVE_PUBLISH(&a.super, NULL);
    standard_ao_support_drained();
}

TEST_CASE("boost on, on battery with a healthy cell, asks and answers true once the boost runs",
          "[kode_battery]")
{
    static boost_caller_t c;

    fresh();
    script(READING_MV, READING_PCT, KODE_SYS_CHARGE_NONE, false, true);
    boost_call(&c, true);
    power_request_wait();
    TEST_ASSERT_EQUAL_INT32(1, s_ctrl.value);
    TEST_ASSERT_FALSE_MESSAGE(boost_call_done(&c, QUIET_MS), "answered before the power manager did");

    power_answers(POWER_CTRL_BOOST, KODE_OK, POWER_BOOST_WHY_NONE, 1);
    TEST_ASSERT_TRUE(boost_call_done(&c, TEST_WAIT_MS));
    TEST_ASSERT_TRUE(c.result);
    TEST_ASSERT_EQUAL_STRING("", c.error);
}

TEST_CASE("an answer to another request does not end the boost's wait", "[kode_battery]")
{
    static boost_caller_t c;

    fresh();
    script(READING_MV, READING_PCT, KODE_SYS_CHARGE_NONE, false, true);
    boost_call(&c, true);
    power_request_wait();
    power_answers(POWER_CTRL_CHARGE, KODE_OK, POWER_BOOST_WHY_NONE, 1);
    TEST_ASSERT_FALSE_MESSAGE(boost_call_done(&c, QUIET_MS), "the charge's answer ended the wait");

    power_answers(POWER_CTRL_BOOST, KODE_OK, POWER_BOOST_WHY_NONE, 1);
    TEST_ASSERT_TRUE(boost_call_done(&c, TEST_WAIT_MS));
    TEST_ASSERT_TRUE(c.result);
}

/* An earlier boost off posts and does not wait, so its answer may come while
 * a boost on waits: the off's answers are told from the on's. */
TEST_CASE("a boost off's answer does not end a boost on's wait", "[kode_battery]")
{
    static boost_caller_t c;

    fresh();
    script(READING_MV, READING_PCT, KODE_SYS_CHARGE_NONE, false, true);
    boost_call(&c, true);
    power_request_wait();
    power_answers(POWER_CTRL_BOOST, KODE_OK, POWER_BOOST_WHY_NONE, 0);
    TEST_ASSERT_FALSE_MESSAGE(boost_call_done(&c, QUIET_MS), "a boost off's success ended the wait");
    power_answers(POWER_CTRL_BOOST, KODE_ERROR, POWER_BOOST_WHY_NONE, 1);
    TEST_ASSERT_FALSE_MESSAGE(boost_call_done(&c, QUIET_MS), "a boost off's failure ended the wait");

    power_answers(POWER_CTRL_BOOST, KODE_OK, POWER_BOOST_WHY_NONE, 1);
    TEST_ASSERT_TRUE(boost_call_done(&c, TEST_WAIT_MS));
    TEST_ASSERT_TRUE(c.result);
}

/** @brief One answer that is not the boost on, and what kode_error() says for it. */
typedef struct {
    kode_status_t rc;     /**< The answer's code. */
    uint8_t       why;    /**< Its cause.          */
    const char   *text;   /**< kode_error().       */
} boost_no_t;

TEST_CASE("a boost that does not start is false, with the power manager's reason", "[kode_battery]")
{
    static const boost_no_t no[] = {
        { KODE_ERROR, POWER_BOOST_WHY_FAULT,
          "the boost did not start: an overload or an over-voltage on J3's 5 V, or the cell too low" },
        { KODE_ERROR, POWER_BOOST_WHY_TEMP,
          "the boost did not start: the cell is under -10 C or over 55 C, or no pack is fitted" },
        { KODE_ERROR, POWER_BOOST_WHY_CABLE, "the boost cannot start while USB is plugged in" },
        { KODE_ERROR, POWER_BOOST_WHY_PIN, "the boost did not start: the charger's OTG pin is low" },
        { KODE_DENIED, POWER_BOOST_WHY_CELL, "the cell is under the 3600 mV the boost needs" },
        { KODE_DENIED, POWER_BOOST_WHY_UNREAD, "the battery has not been read yet" },
        { KODE_DENIED, POWER_BOOST_WHY_OFF, "the board is switching off" },
        { KODE_NO_RESOURCES, POWER_BOOST_WHY_NONE, "the power manager is busy" },
        { KODE_ERROR, POWER_BOOST_WHY_NONE, "the charger did not start the boost" },
    };
    static boost_caller_t c;

    for ( uint32_t i = 0U; (sizeof(no) / sizeof(no[0])) > i; ++i )
    {
        fresh();
        script(READING_MV, READING_PCT, KODE_SYS_CHARGE_NONE, false, true);
        boost_call(&c, true);
        power_request_wait();
        power_answers(POWER_CTRL_BOOST, no[i].rc, no[i].why, 0);
        TEST_ASSERT_TRUE(boost_call_done(&c, TEST_WAIT_MS));
        TEST_ASSERT_FALSE(c.result);
        TEST_ASSERT_EQUAL_STRING(no[i].text, c.error);
    }
}

TEST_CASE("a boost the power manager never answers is false after its bound", "[kode_battery]")
{
    static boost_caller_t c;

    fresh();
    script(READING_MV, READING_PCT, KODE_SYS_CHARGE_NONE, false, true);
    boost_call(&c, true);
    power_request_wait();
    TEST_ASSERT_TRUE(boost_call_done(&c, KODE_PIN_POWER_5V_MS + TEST_WAIT_MS));
    TEST_ASSERT_FALSE(c.result);
    TEST_ASSERT_EQUAL_STRING("the power manager did not answer within 1000 ms", c.error);

    /* A late answer ends nothing and leaves nothing for the next call. */
    power_answers(POWER_CTRL_BOOST, KODE_OK, POWER_BOOST_WHY_NONE, 1);
    boost_call(&c, true);
    power_request_wait();
    TEST_ASSERT_FALSE_MESSAGE(boost_call_done(&c, QUIET_MS), "the late answer ended the next wait");
    power_answers(POWER_CTRL_BOOST, KODE_OK, POWER_BOOST_WHY_NONE, 1);
    TEST_ASSERT_TRUE(boost_call_done(&c, TEST_WAIT_MS));
    TEST_ASSERT_TRUE(c.result);
}

TEST_CASE("boost on with USB plugged in is refused and posts nothing", "[kode_battery]")
{
    fresh();
    script(READING_MV, READING_PCT, KODE_SYS_CHARGE_FAST, true, true);

    TEST_ASSERT_FALSE(seam_power_boost(true));
    TEST_ASSERT_EQUAL_STRING("the boost cannot start while USB is plugged in", kode_error());
    TEST_ASSERT_EQUAL_UINT(0U, uxQueueMessagesWaiting(AO_Power->eQueue));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, kode_sys_time_ms(), "a refusal does not wait");
}

TEST_CASE("boost on with a cell under the pack's floor is refused and posts nothing", "[kode_battery]")
{
    static boost_caller_t c;

    fresh();
    script(KBSP_CELL_BOOST_MIN_MV - 1U, READING_PCT, KODE_SYS_CHARGE_NONE, false, true);

    TEST_ASSERT_FALSE(seam_power_boost(true));
    TEST_ASSERT_EQUAL_STRING("the cell is at 3599 mV: the boost needs 3600 mV", kode_error());
    TEST_ASSERT_EQUAL_UINT(0U, uxQueueMessagesWaiting(AO_Power->eQueue));

    s_seam_reading.vbat_mv = KBSP_CELL_BOOST_MIN_MV;
    boost_call(&c, true);
    power_request_wait();
    power_answers(POWER_CTRL_BOOST, KODE_OK, POWER_BOOST_WHY_NONE, 1);
    TEST_ASSERT_TRUE(boost_call_done(&c, TEST_WAIT_MS));
    TEST_ASSERT_TRUE(c.result);
}

TEST_CASE("boost on with no reading cannot tell USB from battery, so it posts nothing", "[kode_battery]")
{
    fresh();

    TEST_ASSERT_FALSE(seam_power_boost(true));
    TEST_ASSERT_EQUAL_STRING("the battery has not been read yet", kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_BATTERY_WAIT_MS, kode_sys_time_ms());
    TEST_ASSERT_EQUAL_UINT(0U, uxQueueMessagesWaiting(AO_Power->eQueue));
}

TEST_CASE("boost off is always taken: no reading, USB or a low cell do not stop it", "[kode_battery]")
{
    fresh();
    TEST_ASSERT_TRUE(seam_power_boost(false));
    TEST_ASSERT_TRUE(power_take_ctrl());
    TEST_ASSERT_EQUAL_UINT8(POWER_CTRL_BOOST, s_ctrl.op);
    TEST_ASSERT_EQUAL_INT32(0, s_ctrl.value);

    script(PMIC_MV, 0U, KODE_SYS_CHARGE_FAST, true, false);
    TEST_ASSERT_TRUE(seam_power_boost(false));
    TEST_ASSERT_TRUE(power_take_ctrl());
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_reads);
    TEST_ASSERT_EQUAL_UINT32(0U, kode_sys_time_ms());
}

TEST_CASE("boost waits for room in the power manager's queue, then says it is busy", "[kode_battery]")
{
    fresh();
    fake_sync_real(true);
    TEST_ASSERT_TRUE(kode_standard_ao_ensure());
    script(READING_MV, READING_PCT, KODE_SYS_CHARGE_NONE, false, true);
    power_fill_to_margin();

    TEST_ASSERT_FALSE(seam_power_boost(true));
    TEST_ASSERT_EQUAL_STRING("the power manager is busy", kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_BATTERY_WAIT_MS, kode_sys_time_ms());
}

/* ---- battery_low ---------------------------------------------------------------- */

/** @brief One reading to the edge: its answer, with the memory it keeps. */
static bool edge(bool *low, uint8_t pct, uint8_t charging, bool gauge_ok)
{
    kode_sys_battery_t r;

    (void)memset(&r, 0, sizeof(r));
    r.soc_pct  = pct;
    r.charging = charging;
    r.gauge_ok = gauge_ok;
    return seam_battery_low_edge(low, &r);
}

TEST_CASE("one battery_low at 14 percent, none at 13, one more after it climbed to 20 and fell to 14",
          "[kode_battery]")
{
    const uint8_t none = KODE_SYS_CHARGE_NONE;
    bool          low  = false;

    TEST_ASSERT_FALSE(edge(&low, 30U, none, true));
    TEST_ASSERT_TRUE(edge(&low, 14U, none, true));
    TEST_ASSERT_FALSE(edge(&low, 14U, none, true));
    TEST_ASSERT_FALSE(edge(&low, 13U, none, true));
    TEST_ASSERT_FALSE(edge(&low, 19U, none, true));   /* not far enough up to re-arm */
    TEST_ASSERT_FALSE(edge(&low, 14U, none, true));
    TEST_ASSERT_FALSE(edge(&low, 20U, none, true));   /* re-armed */
    TEST_ASSERT_TRUE(edge(&low, 14U, none, true));
}

TEST_CASE("the mark itself is not low", "[kode_battery]")
{
    bool low = false;

    TEST_ASSERT_FALSE(edge(&low, KODE_BATTERY_LOW_PCT, KODE_SYS_CHARGE_NONE, true));
    TEST_ASSERT_TRUE(edge(&low, KODE_BATTERY_LOW_PCT - 1U, KODE_SYS_CHARGE_NONE, true));
}

TEST_CASE("a gauge that does not answer fires nothing and changes nothing", "[kode_battery]")
{
    const uint8_t none = KODE_SYS_CHARGE_NONE;
    bool          low  = false;

    TEST_ASSERT_FALSE(edge(&low, 0U, none, false));
    TEST_ASSERT_FALSE(low);
    TEST_ASSERT_TRUE(edge(&low, 10U, none, true));
    TEST_ASSERT_FALSE(edge(&low, 100U, none, false));   /* not a reading: it does not re-arm */
    TEST_ASSERT_TRUE(low);
}

TEST_CASE("charging fires nothing and re-arms", "[kode_battery]")
{
    const uint8_t none = KODE_SYS_CHARGE_NONE;
    bool          low  = false;

    TEST_ASSERT_FALSE(edge(&low, 10U, KODE_SYS_CHARGE_FAST, true));
    TEST_ASSERT_FALSE(edge(&low, 10U, KODE_SYS_CHARGE_PRE, true));
    TEST_ASSERT_TRUE(edge(&low, 10U, none, true));
    TEST_ASSERT_FALSE(edge(&low, 11U, KODE_SYS_CHARGE_FAST, true));   /* plugged in: re-armed */
    TEST_ASSERT_TRUE(edge(&low, 10U, none, true));                          /* unplugged still low */
}
