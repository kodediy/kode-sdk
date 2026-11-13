/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_sys_cmd.c
 * @brief kode_sys.h's commands post with their owner's margin, so a full queue is refused.
 */
#include <stdbool.h>
#include <stdint.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "kaos.h"
#include "kode_sys.h"
#include "kqp.h"
#include "qf_support.h"
#include "sdkconfig.h"

/** @brief Calls the queue takes before it is within the margin. */
#define CMD_TAKEN  2U

/** @brief Calls a loop makes: more than the queue takes. */
#define CMD_CALLS  (CMD_TAKEN + 6U)

/** @brief Most pool blocks the dry-pool case holds. */
#define HOLD_MAX  256U

/** @brief The host's event pools: the ordinary and the jumbo. */
#define POOLS  2U

/** @brief A static event no owner sees: it only takes a slot. */
static const QEvt s_filler = QEVT_INITIALIZER(Q_USER_SIG);

/** @brief A command with its arguments fixed. */
typedef kode_status_t (*cmd_fn)(void);

/**
 * @brief The free slots of @p ao's queue.
 * @param ao The owner.
 * @return The count.
 */
static uint32_t queue_free(QActive *ao)
{
    return (uint32_t)uxQueueSpacesAvailable(ao->eQueue);
}

/**
 * @brief Recycle everything waiting in @p ao's queue.
 * @param ao The owner.
 */
static void queue_drain(QActive *ao)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(ao->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief The free blocks of every event pool, added up. */
static uint32_t pools_free(void)
{
    uint32_t total = 0U;

    for ( uint8_t id = 1U; POOLS >= id; ++id )
    {
        total += (uint32_t)QF_getPoolFree(id);
    }
    return total;
}

/**
 * @brief Call @p cmd in a loop against @p ao's queue, then once more after it empties.
 * @param ao     The command's owner.
 * @param cmd    The command.
 * @param margin The owner's command margin.
 */
static void loop_past_margin(QActive *ao, cmd_fn cmd, uint32_t margin)
{
    uint32_t taken   = 0U;
    uint32_t refused = 0U;
    uint32_t pools;

    qf_support_init();
    qf_support_margin_queue(ao);
    pools = pools_free();
    while ( queue_free(ao) > (margin + CMD_TAKEN) )
    {
        TEST_ASSERT_TRUE(QACTIVE_POST_X(ao, &s_filler, 0U, NULL));
    }
    for ( uint32_t i = 0U; CMD_CALLS > i; ++i )
    {
        kode_status_t const rc = cmd();

        if ( KODE_OK == rc )
        {
            ++taken;
        }
        else
        {
            TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, rc);
            ++refused;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(CMD_TAKEN, taken);
    TEST_ASSERT_EQUAL_UINT32(CMD_CALLS - CMD_TAKEN, refused);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(margin, queue_free(ao),
                                     "the margin stays free for the owner");

    queue_drain(ao);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(pools, pools_free(), "every refused event went back to its pool");
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, cmd(), "the owner takes a call again");
    queue_drain(ao);
}

/* ---- The commands, their arguments fixed ---- */

static kode_status_t led_set(void)      { return kode_sys_led_set(true); }
static kode_status_t led_toggle(void)   { return kode_sys_led_toggle(); }
static kode_status_t led_rgb(void)      { return kode_sys_led_set_rgb(1U, 2U, 3U); }
static kode_status_t ir_nec(void)       { return kode_sys_ir_send_nec(0x00FFU, 0x0001U); }
static kode_status_t tone(void)         { return kode_sys_audio_tone(KODE_SYS_AUDIO_HZ_MIN, 10U, 50U); }
static kode_status_t boost(void)        { return kode_sys_power_boost(false); }
static kode_status_t charge(void)       { return kode_sys_power_charge(true); }
static kode_status_t charge_limit(void) { return kode_sys_power_charge_limit(512U); }
#if CONFIG_KODE_LOWPOWER
static kode_status_t sleep_cmd(void)    { return kode_sys_power_sleep(); }
static kode_status_t off_cmd(void)      { return kode_sys_power_off(); }
#endif
static kode_status_t home_now(void)     { return kode_sys_home_now(); }
static kode_status_t exit_os(void)      { return kode_sys_exit_to_os(); }
#if CONFIG_KODEOS_APPMGR
static kode_status_t flash(void)        { return kode_sys_flash("fs:x.bin", "app", false); }
#endif
#if CONFIG_KODEOS_RADIO
static kode_status_t exp_power(void)    { return kode_sys_exp_power(false); }
#endif
#if CONFIG_KODEOS_WIFI
static kode_status_t wifi_join(void)    { return kode_sys_wifi_connect("lab", "secret"); }
static kode_status_t wifi_auto(void)    { return kode_sys_wifi_connect(NULL, NULL); }
static kode_status_t wifi_leave(void)   { return kode_sys_wifi_disconnect(); }
#endif
#if CONFIG_KODEOS_NET
static kode_status_t mqtt_up(void)      { return kode_sys_mqtt_connect("mqtt://broker", NULL, NULL); }
static kode_status_t mqtt_down(void)    { return kode_sys_mqtt_disconnect(); }
static kode_status_t mqtt_pub(void)     { return kode_sys_mqtt_publish("t", "x", 1U); }
static kode_status_t mqtt_sub(void)     { return kode_sys_mqtt_subscribe("t"); }
static kode_status_t mqtt_unsub(void)   { return kode_sys_mqtt_unsubscribe("t"); }
#endif

TEST_CASE("the LED commands in a loop are refused past the margin, then taken again",
          "[kode_sys_cmd]")
{
    loop_past_margin(AO_LedMgr, &led_set, KQP_LEDMGR_CMD_MARGIN);
    loop_past_margin(AO_LedMgr, &led_toggle, KQP_LEDMGR_CMD_MARGIN);
    loop_past_margin(AO_LedMgr, &led_rgb, KQP_LEDMGR_CMD_MARGIN);
}

TEST_CASE("the infrared NEC command in a loop is refused past the margin, then taken again",
          "[kode_sys_cmd]")
{
    loop_past_margin(AO_Ir, &ir_nec, KQP_IR_CMD_MARGIN);
}

TEST_CASE("the tone command in a loop is refused past the margin, then taken again",
          "[kode_sys_cmd]")
{
    loop_past_margin(AO_Audio, &tone, KQP_AUDIO_CMD_MARGIN);
}

TEST_CASE("the power commands in a loop are refused past the margin, then taken again",
          "[kode_sys_cmd]")
{
    loop_past_margin(AO_Power, &boost, KQP_POWER_CMD_MARGIN);
    loop_past_margin(AO_Power, &charge, KQP_POWER_CMD_MARGIN);
    loop_past_margin(AO_Power, &charge_limit, KQP_POWER_CMD_MARGIN);
}

#if CONFIG_KODE_LOWPOWER
/* Sleep and off go to the rest round's owner, not to AO_Power: AO_LowPower
 * asks every owner first, and only the console reaches ship mode. */
TEST_CASE("the rest commands in a loop are refused past the margin, then taken again",
          "[kode_sys_cmd]")
{
    loop_past_margin(AO_LowPower, &sleep_cmd, KQP_LOWPOWER_CMD_MARGIN);
    loop_past_margin(AO_LowPower, &off_cmd, KQP_LOWPOWER_CMD_MARGIN);
}
#endif

TEST_CASE("the going-home commands in a loop are refused past the margin, then taken again",
          "[kode_sys_cmd]")
{
    loop_past_margin(AO_Boot, &home_now, KQP_BOOT_CMD_MARGIN);
    loop_past_margin(AO_Boot, &exit_os, KQP_BOOT_CMD_MARGIN);
}

#if CONFIG_KODEOS_APPMGR
TEST_CASE("the image command in a loop is refused past the margin, then taken again",
          "[kode_sys_cmd]")
{
    loop_past_margin(AO_AppMgr, &flash, KQP_APPMGR_CMD_MARGIN);
}
#endif

#if CONFIG_KODEOS_RADIO
TEST_CASE("the radio's commands in a loop are refused past the margin, then taken again",
          "[kode_sys_cmd]")
{
    loop_past_margin(AO_Radio, &exp_power, KQP_RADIO_CMD_MARGIN);
#if CONFIG_KODEOS_WIFI
    loop_past_margin(AO_Radio, &wifi_join, KQP_RADIO_CMD_MARGIN);
    loop_past_margin(AO_Radio, &wifi_auto, KQP_RADIO_CMD_MARGIN);
    loop_past_margin(AO_Radio, &wifi_leave, KQP_RADIO_CMD_MARGIN);
#endif
}
#endif

#if CONFIG_KODEOS_NET
TEST_CASE("the MQTT commands in a loop are refused past the margin, then taken again",
          "[kode_sys_cmd]")
{
    loop_past_margin(AO_Net, &mqtt_up, KQP_NET_CMD_MARGIN);
    loop_past_margin(AO_Net, &mqtt_down, KQP_NET_CMD_MARGIN);
    loop_past_margin(AO_Net, &mqtt_pub, KQP_NET_CMD_MARGIN);
    loop_past_margin(AO_Net, &mqtt_sub, KQP_NET_CMD_MARGIN);
    loop_past_margin(AO_Net, &mqtt_unsub, KQP_NET_CMD_MARGIN);
}
#endif

/** @brief The pool blocks the dry-pool case holds. */
static const QEvt *s_held[HOLD_MAX];

/** @brief How many of s_held are in hand. */
static uint32_t s_held_n;

/**
 * @brief Take every block of the pool that serves events of @p size.
 * @param size An event size.
 */
static void pool_take_all(uint_fast16_t size)
{
    QEvt *e = QF_newX_(size, 0U, (enum_t)Q_USER_SIG);

    while ( (NULL != e) && (HOLD_MAX > s_held_n) )
    {
        s_held[s_held_n] = e;
        ++s_held_n;
        e = QF_newX_(size, 0U, (enum_t)Q_USER_SIG);
    }
    TEST_ASSERT_TRUE_MESSAGE(HOLD_MAX > s_held_n, "the pool ran dry");
}

/** @brief Give back every block pool_take_all() took. */
static void pool_give_back(void)
{
    while ( 0U < s_held_n )
    {
        --s_held_n;
        QF_gc(s_held[s_held_n]);
    }
}

/**
 * @brief Whether @p ao's queue holds nothing.
 * @param ao The owner.
 * @return true when it is empty.
 */
static bool queue_empty(QActive *ao)
{
    return uxQueueMessagesWaiting(ao->eQueue) == 0U;
}

TEST_CASE("a command with the pools dry is KODE_NO_RESOURCES and posts nothing",
          "[kode_sys_cmd]")
{
    QActive *const OWNERS[] = {
        AO_LedMgr, AO_Ir, AO_Audio, AO_Power, AO_AppMgr, AO_Radio, AO_Net,
#if CONFIG_KODE_LOWPOWER
        AO_LowPower,
#endif
    };

    qf_support_init();
    for ( uint32_t i = 0U; (uint32_t)(sizeof(OWNERS) / sizeof(OWNERS[0])) > i; ++i )
    {
        qf_support_margin_queue(OWNERS[i]);
    }
    /* Both pools: an event too big for the ordinary block takes a jumbo one. */
    s_held_n = 0U;
    pool_take_all((uint_fast16_t)sizeof(LedEvt));
    pool_take_all((uint_fast16_t)sizeof(RecvDataEvt));

    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_led_set(true));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_led_toggle());
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_led_set_rgb(1U, 2U, 3U));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_ir_send_nec(0U, 0U));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_audio_tone(KODE_SYS_AUDIO_HZ_MIN, 10U, 50U));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_audio_volume(10U));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_power_boost(false));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_power_charge(true));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_power_charge_limit(512U));
#if CONFIG_KODE_LOWPOWER
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_power_sleep());
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_power_off());
#endif
#if CONFIG_KODEOS_APPMGR
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_flash("fs:x.bin", "app", false));
#endif
#if CONFIG_KODEOS_WIFI
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_wifi_connect("lab", "secret"));
#endif
#if CONFIG_KODEOS_NET
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_mqtt_connect("mqtt://broker", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_mqtt_publish("t", "x", 1U));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_mqtt_subscribe("t"));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_mqtt_unsubscribe("t"));
#endif
    for ( uint32_t i = 0U; (uint32_t)(sizeof(OWNERS) / sizeof(OWNERS[0])) > i; ++i )
    {
        TEST_ASSERT_TRUE_MESSAGE(queue_empty(OWNERS[i]), "nothing reached an owner");
    }

    pool_give_back();
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_power_boost(false), "the pool is back");
    queue_drain(AO_Power);
}
