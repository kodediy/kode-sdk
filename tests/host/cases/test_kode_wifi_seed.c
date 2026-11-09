/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_wifi_seed.c
 * @brief The standard layer's Active Object takes the radio's address as it finds it when it starts.
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "fakes.h"
#include "kdrv_wifi.h"
#include "khal_once.h"
#include "kode.h"
#include "kode_standard_ao.h"
#include "kode_sys.h"
#include "kode_sys_priv.h"
#include "kqp.h"
#include "kqp_config.h"
#include "qf_support.h"

/** @brief The longest a case waits for a listener, in ms. */
#define SEED_WAIT_MS 4000U

/** @brief How long a case waits to be sure no further event comes, in ms. */
#define SEED_QUIET_MS 50U

extern QActive *QActive_registry_[QF_MAX_ACTIVE + 1U];

static kode_sys_wifi_status_t s_seed_status;  /**< What the scripted status reads. */

static kode_status_t seed_status(kode_sys_wifi_status_t *out);
bool seed_ao_screen_attach(const kode_screen_ops_t *ops);  /* the copy's names, declared before it uses them */
bool seed_ao_screen_post(QEvt const *e);

#define kode_sys_wifi_status         seed_status
#define kode_standard_ao_ensure        seed_ao_ensure
#define kode_standard_ao_screen_attach seed_ao_screen_attach
#define kode_standard_ao_screen_post   seed_ao_screen_post
#define kode_standard_ao_touch_listen  seed_ao_touch_listen
#define kode_standard_ao_pin_arm       seed_ao_pin_arm
#define kode_standard_ao_power_expect seed_ao_power_expect
#define kode_standard_ao_power_await  seed_ao_power_await
#define kode_standard_ao_subscribe     seed_ao_subscribe
#define AO_KodeStandard                AO_Seed
#undef CONFIG_KODEOS_BOOT_ROLE_OS
#define CONFIG_KODEOS_BOOT_ROLE_OS 0
#include "kode_standard_ao.c"
#undef kode_sys_wifi_status
#undef kode_standard_ao_ensure
#undef kode_standard_ao_screen_attach
#undef kode_standard_ao_screen_post
#undef kode_standard_ao_touch_listen
#undef kode_standard_ao_pin_arm
#undef kode_standard_ao_power_expect
#undef kode_standard_ao_power_await
#undef kode_standard_ao_subscribe
#undef AO_KodeStandard

static kode_status_t seed_status(kode_sys_wifi_status_t *out)
{
    *out = s_seed_status;
    return KODE_OK;
}

/** @brief What the listener saw. */
typedef struct {
    char name[KODE_EVENT_NAME_MAX];      /**< The event's name. */
    char source[KODE_EVENT_SOURCE_MAX];  /**< Its source.       */
} seed_rec_t;

static seed_rec_t        s_seed_rec[2];  /**< The first two events heard.     */
static volatile uint32_t s_seed_heard;   /**< How many events were heard.     */
static SemaphoreHandle_t s_seed_given;   /**< Given for every event heard.    */

/** @brief Keep the event and tell the case. */
static void seed_listener(const kode_event_t *e)
{
    const uint32_t i = s_seed_heard;

    if ( 2U > i )
    {
        (void)snprintf(s_seed_rec[i].name, sizeof(s_seed_rec[i].name), "%s", e->name);
        (void)snprintf(s_seed_rec[i].source, sizeof(s_seed_rec[i].source), "%s", e->source);
    }
    s_seed_heard = i + 1U;
    (void)xSemaphoreGive(s_seed_given);
}

/** @brief The real layer up with a listener on both events; nothing heard yet. */
static void seed_layer_up(void)
{
    qf_support_init();
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    fake_sync_real(true);
    TEST_ASSERT_TRUE(kode_standard_ao_ensure());
    if ( NULL == s_seed_given )
    {
        s_seed_given = xSemaphoreCreateCounting(2U, 0U);
    }
    while ( xSemaphoreTake(s_seed_given, 0U) == pdTRUE )
    {
    }
    s_seed_heard = 0U;
    TEST_ASSERT_TRUE(kode_event_add_listener("wifi_connected", &seed_listener));
    TEST_ASSERT_TRUE(kode_event_add_listener("wifi_lost", &seed_listener));
}

/** @brief The highest QP level nobody holds. */
static uint8_t seed_free_prio(void)
{
    uint8_t p = QF_MAX_ACTIVE;

    while ( (0U < p) && (NULL != QActive_registry_[p]) )
    {
        p--;
    }
    return p;
}

/** @brief Start the copy of the Active Object on the scripted status. */
static void seed_ao_start(void)
{
    const uint8_t prio = seed_free_prio();

    TEST_ASSERT_GREATER_THAN_UINT8(0U, prio);
    QActive_ctor(&l_standard.super, Q_STATE_CAST(&KodeStandard_initial));
    qf_support_give_queue(&l_standard.super);
    l_standard.super.prio  = prio;
    l_standard.super.pthre = prio;
    QActive_register_(&l_standard.super);
    QASM_INIT(&l_standard.super, (void *)0, 0U);
}

/** @brief Hand the copy a state of the radio. */
static void seed_dispatch(kdrv_wifi_state_t state, const char *ssid)
{
    WifiStateEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(WIFI_STATE_SIG);
    e.state = (uint8_t)state;
    (void)snprintf(e.ssid, sizeof(e.ssid), "%s", ssid);
    QASM_DISPATCH(&l_standard.super, &e.super, 0U);
}

/** @brief Take the copy and the listeners away. */
static void seed_down(void)
{
    QActive_unsubscribeAll(&l_standard.super);
    QActive_unregister_(&l_standard.super);
    TEST_ASSERT_TRUE(kode_event_remove_listener("wifi_connected", &seed_listener));
    TEST_ASSERT_TRUE(kode_event_remove_listener("wifi_lost", &seed_listener));
}

TEST_CASE("a board already online when the layer starts says it lost the link, and said nothing of the start",
          "[kode_wifi]")
{
    seed_layer_up();
    (void)memset(&s_seed_status, 0, sizeof(s_seed_status));
    s_seed_status.state = KODE_SYS_WIFI_STATE_GOT_IP;
    (void)snprintf(s_seed_status.ssid, sizeof(s_seed_status.ssid), "%s", "net");
    seed_ao_start();

    seed_dispatch(KDRV_WIFI_GOT_IP, "net");       /* the same link: no event */
    seed_dispatch(KDRV_WIFI_DISCONNECTED, "");
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_seed_given, pdMS_TO_TICKS(SEED_WAIT_MS)));
    TEST_ASSERT_EQUAL_INT(pdFALSE, xSemaphoreTake(s_seed_given, pdMS_TO_TICKS(SEED_QUIET_MS)));

    TEST_ASSERT_EQUAL_UINT32(1U, s_seed_heard);
    TEST_ASSERT_EQUAL_STRING("wifi_lost", s_seed_rec[0].name);
    TEST_ASSERT_EQUAL_STRING("net", s_seed_rec[0].source);
    seed_down();
}

TEST_CASE("a board offline when the layer starts hears only the address that comes", "[kode_wifi]")
{
    seed_layer_up();
    (void)memset(&s_seed_status, 0, sizeof(s_seed_status));
    seed_ao_start();

    seed_dispatch(KDRV_WIFI_DISCONNECTED, "");    /* nothing was up: no event */
    seed_dispatch(KDRV_WIFI_GOT_IP, "other");
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_seed_given, pdMS_TO_TICKS(SEED_WAIT_MS)));
    TEST_ASSERT_EQUAL_INT(pdFALSE, xSemaphoreTake(s_seed_given, pdMS_TO_TICKS(SEED_QUIET_MS)));

    TEST_ASSERT_EQUAL_UINT32(1U, s_seed_heard);
    TEST_ASSERT_EQUAL_STRING("wifi_connected", s_seed_rec[0].name);
    TEST_ASSERT_EQUAL_STRING("other", s_seed_rec[0].source);
    seed_down();
}

/* An app without AO_Radio has no Wi-Fi: the start reads nothing and seeds off,
 * whatever the driver would say. */
TEST_CASE("with no AO_Radio the layer starts with Wi-Fi off and reads nothing",
          "[kode_wifi]")
{
    seed_layer_up();
    (void)memset(&s_seed_status, 0, sizeof(s_seed_status));
    s_seed_status.state = KODE_SYS_WIFI_STATE_GOT_IP;
    (void)snprintf(s_seed_status.ssid, sizeof(s_seed_status.ssid), "%s", "net");
    fake_kaos_absent(&kaos_radio_start);
    seed_ao_start();

    seed_dispatch(KDRV_WIFI_GOT_IP, "net");       /* off at the start: an arrival */
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_seed_given, pdMS_TO_TICKS(SEED_WAIT_MS)));
    TEST_ASSERT_EQUAL_UINT32(1U, s_seed_heard);
    TEST_ASSERT_EQUAL_STRING("wifi_connected", s_seed_rec[0].name);
    seed_down();
}
