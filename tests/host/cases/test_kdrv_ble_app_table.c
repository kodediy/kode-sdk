/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_ble_app_table.c
 * @brief The app's own GATT service without NimBLE: its table, its values and its notify ring.
 */
#include <string.h>

#include "unity.h"

#include "fakes.h"
#include "kdrv_ble.h"
#include "kdrv_ble_app.h"

/* NimBLE's ble_gatt_chr_flags, which do not build for the host. */
#define NB_READ           0x0002U   /**< BLE_GATT_CHR_F_READ.                   */
#define NB_WRITE          0x0008U   /**< BLE_GATT_CHR_F_WRITE.                  */
#define NB_NOTIFY         0x0010U   /**< BLE_GATT_CHR_F_NOTIFY.                 */
#define NB_READ_ENC       0x0200U   /**< BLE_GATT_CHR_F_READ_ENC.               */
#define NB_READ_AUTHEN    0x0400U   /**< BLE_GATT_CHR_F_READ_AUTHEN.            */
#define NB_WRITE_ENC      0x1000U   /**< BLE_GATT_CHR_F_WRITE_ENC.              */
#define NB_WRITE_AUTHEN   0x2000U   /**< BLE_GATT_CHR_F_WRITE_AUTHEN.           */
#define NB_NOTIFY_ENC     0x8000U   /**< BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC.    */
#define NB_NOTIFY_AUTHEN  0x10000U  /**< BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN. */

/** @brief The service's UUID in the cases. */
#define SVC_UUID  "6e400001-b5a3-f393-e0a9-e50e24dcca9e"

/** @brief The console's masks with passkey pairing, as kdrv_ble.c passes them. */
static const kdrv_ble_app_perm_t s_perm = {
    .read   = NB_READ | NB_READ_ENC | NB_READ_AUTHEN,
    .write  = NB_WRITE | NB_WRITE_ENC | NB_WRITE_AUTHEN,
    .notify = NB_NOTIFY | NB_NOTIFY_ENC | NB_NOTIFY_AUTHEN,
};

/** @brief Two characteristics: one read and notified, one written. */
static const kdrv_ble_app_char_t s_two[] = {
    { "temp", "6E400002-B5A3-F393-E0A9-E50E24DCCA9E", KDRV_BLE_APP_READ | KDRV_BLE_APP_NOTIFY },
    { "cmd",  "6e400003-b5a3-f393-e0a9-e50e24dcca9e", KDRV_BLE_APP_WRITE },
};

/** @brief The service of two. */
static const kdrv_ble_app_service_t s_svc_two = { SVC_UUID, s_two, 2U };

/** @brief Five characteristics, one more than a service may have. */
static const kdrv_ble_app_char_t s_five[] = {
    { "a", "6e400002-b5a3-f393-e0a9-e50e24dcca9e", KDRV_BLE_APP_READ },
    { "b", "6e400003-b5a3-f393-e0a9-e50e24dcca9e", KDRV_BLE_APP_READ },
    { "c", "6e400004-b5a3-f393-e0a9-e50e24dcca9e", KDRV_BLE_APP_READ },
    { "d", "6e400005-b5a3-f393-e0a9-e50e24dcca9e", KDRV_BLE_APP_READ },
    { "e", "6e400006-b5a3-f393-e0a9-e50e24dcca9e", KDRV_BLE_APP_READ },
};

/** @brief A fresh case: the service of two declared, its link forgotten. */
static void fresh(void)
{
    fake_klog_reset();
    fake_ble_app_service_set(&s_svc_two);
    kdrv_ble_app_link_reset();
}

/** @brief Put everything back as a case found it. */
static void done(void)
{
    kdrv_ble_app_link_reset();
    fake_ble_app_service_set(NULL);
}

/* ---- The table ----------------------------------------------------------- */

TEST_CASE("a table of two builds a service of two, with the console's permissions",
          "[kdrv_ble_app]")
{
    /* 6e400002-... in wire order, least significant byte first. */
    static const uint8_t want_temp[KDRV_BLE_UUID128_BYTES] = {
        0x9EU, 0xCAU, 0xDCU, 0x24U, 0x0EU, 0xE5U, 0xA9U, 0xE0U,
        0x93U, 0xF3U, 0xA3U, 0xB5U, 0x02U, 0x00U, 0x40U, 0x6EU };
    kdrv_ble_app_table_t t;

    fresh();
    TEST_ASSERT_TRUE(kdrv_ble_app_build(&s_svc_two, &s_perm, &t));
    TEST_ASSERT_EQUAL_UINT8(2U, t.count);
    TEST_ASSERT_EQUAL_UINT8(0x01U, t.svc_uuid[KDRV_BLE_UUID128_BYTES - 4U]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want_temp, t.uuid[0], KDRV_BLE_UUID128_BYTES);
    TEST_ASSERT_EQUAL_UINT8(0x03U, t.uuid[1][KDRV_BLE_UUID128_BYTES - 4U]);
    TEST_ASSERT_EQUAL_HEX32(NB_READ | NB_READ_ENC | NB_READ_AUTHEN | NB_NOTIFY | NB_NOTIFY_ENC
                            | NB_NOTIFY_AUTHEN, t.flags[0]);
    TEST_ASSERT_EQUAL_HEX32(NB_WRITE | NB_WRITE_ENC | NB_WRITE_AUTHEN, t.flags[1]);
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());
    done();
}

TEST_CASE("no service of the app's leaves the table the console's alone, and says nothing",
          "[kdrv_ble_app]")
{
    kdrv_ble_app_table_t t;

    fresh();
    fake_ble_app_service_set(NULL);
    TEST_ASSERT_FALSE(kdrv_ble_app_build(NULL, &s_perm, &t));
    TEST_ASSERT_EQUAL_UINT8(0U, t.count);
    TEST_ASSERT_FALSE(kdrv_ble_app_fault(NULL, NULL, 0U));
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_app_notify("temp", "x", 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_app_set_value("temp", "x", 1U));
    done();
}

TEST_CASE("test_ble_service_bad_table_left_out: a bad UUID or five characteristics leave the "
          "service out with one log line", "[kdrv_ble_app]")
{
    static const kdrv_ble_app_char_t bad_chr[] = {
        { "temp", "6e400002-b5a3-f393-e0a9-e50e24dcca9", KDRV_BLE_APP_READ },
    };
    static const kdrv_ble_app_service_t bad_svc_uuid = { "6e400001-b5a3-f393-e0a9_e50e24dcca9e",
                                                         s_two, 2U };
    static const kdrv_ble_app_service_t bad_chr_uuid = { SVC_UUID, bad_chr, 1U };
    static const kdrv_ble_app_service_t five         = { SVC_UUID, s_five, 5U };
    kdrv_ble_app_table_t t;
    char                 why[KDRV_BLE_APP_WHY_MAX];

    fresh();
    TEST_ASSERT_TRUE(kdrv_ble_app_fault(&five, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("the service has 5 characteristics; at most 4", why);
    TEST_ASSERT_FALSE(kdrv_ble_app_build(&five, &s_perm, &t));
    TEST_ASSERT_EQUAL_UINT8(0U, t.count);
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());
    TEST_ASSERT_NOT_NULL(strstr(fake_klog_last(),
                                "the app's service is left out: the service has 5 characteristics"));

    fake_klog_reset();
    TEST_ASSERT_FALSE(kdrv_ble_app_build(&bad_svc_uuid, &s_perm, &t));
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());
    TEST_ASSERT_NOT_NULL(strstr(fake_klog_last(), "the service's UUID is not 128 bits"));

    fake_klog_reset();
    TEST_ASSERT_FALSE(kdrv_ble_app_build(&bad_chr_uuid, &s_perm, &t));
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());
    TEST_ASSERT_NOT_NULL(strstr(fake_klog_last(), "'temp': the UUID is not 128 bits"));

    /* What the service's calls answer while it is left out. */
    fake_ble_app_service_set(&five);
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_app_notify("a", "x", 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_app_set_value("a", "x", 1U));
    done();
}

TEST_CASE("a characteristic with no name, a long one, a twice-used one or no properties is "
          "a fault", "[kdrv_ble_app]")
{
    static const kdrv_ble_app_char_t no_name[] = {
        { "", "6e400002-b5a3-f393-e0a9-e50e24dcca9e", KDRV_BLE_APP_READ } };
    static const kdrv_ble_app_char_t long_name[] = {
        { "sixteen-bytes-ab", "6e400002-b5a3-f393-e0a9-e50e24dcca9e", KDRV_BLE_APP_READ } };
    static const kdrv_ble_app_char_t twice[] = {
        { "x", "6e400002-b5a3-f393-e0a9-e50e24dcca9e", KDRV_BLE_APP_READ },
        { "x", "6e400003-b5a3-f393-e0a9-e50e24dcca9e", KDRV_BLE_APP_WRITE } };
    static const kdrv_ble_app_char_t no_props[] = {
        { "x", "6e400002-b5a3-f393-e0a9-e50e24dcca9e", 0U } };
    static const kdrv_ble_app_service_t s_no_name   = { SVC_UUID, no_name, 1U };
    static const kdrv_ble_app_service_t s_long_name = { SVC_UUID, long_name, 1U };
    static const kdrv_ble_app_service_t s_twice     = { SVC_UUID, twice, 2U };
    static const kdrv_ble_app_service_t s_no_props  = { SVC_UUID, no_props, 1U };
    static const kdrv_ble_app_service_t s_none      = { SVC_UUID, NULL, 0U };
    char why[KDRV_BLE_APP_WHY_MAX];

    fresh();
    TEST_ASSERT_TRUE(kdrv_ble_app_fault(&s_no_name, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("the characteristic at 0 has no name of 1 to 15 bytes", why);
    TEST_ASSERT_TRUE(kdrv_ble_app_fault(&s_long_name, why, sizeof(why)));
    TEST_ASSERT_TRUE(kdrv_ble_app_fault(&s_twice, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("'x' names two characteristics", why);
    TEST_ASSERT_TRUE(kdrv_ble_app_fault(&s_no_props, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("'x': the properties are not READ, WRITE or NOTIFY", why);
    TEST_ASSERT_TRUE(kdrv_ble_app_fault(&s_none, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("the service has no characteristics", why);
    TEST_ASSERT_FALSE(kdrv_ble_app_fault(&s_svc_two, why, sizeof(why)));
    done();
}

/* ---- Values and notifications --------------------------------------------- */

TEST_CASE("a value set is what a read gets, before the host has started", "[kdrv_ble_app]")
{
    static const uint8_t v[3] = { 0x00U, 0x41U, 0x00U };
    uint8_t              out[KDRV_BLE_APP_VALUE_MAX];

    fresh();
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_app_set_value("cmd", v, sizeof(v)));
    TEST_ASSERT_EQUAL_UINT16(3U, kdrv_ble_app_read(1U, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(v, out, sizeof(v));
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_app_set_value("cmd", NULL, 0U));
    TEST_ASSERT_EQUAL_UINT16(0U, kdrv_ble_app_read(1U, out, sizeof(out)));
    TEST_ASSERT_FALSE(kdrv_ble_app_queued());
    done();
}

TEST_CASE("notify refuses a characteristic without NOTIFY, an unknown name and 245 bytes",
          "[kdrv_ble_app]")
{
    static uint8_t big[KDRV_BLE_APP_VALUE_MAX + 1U];
    uint8_t        out[KDRV_BLE_APP_VALUE_MAX];

    fresh();
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_app_notify("cmd", "x", 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_app_notify("nope", "x", 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_app_notify(NULL, "x", 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_app_notify("temp", big, sizeof(big)));
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_app_notify("temp", NULL, 2U));
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_app_notify("temp", big, KDRV_BLE_APP_VALUE_MAX));
    TEST_ASSERT_EQUAL_UINT16(KDRV_BLE_APP_VALUE_MAX, kdrv_ble_app_read(0U, out, sizeof(out)));
    done();
}

TEST_CASE("the check before a set or a notify reads the declared table and touches no value",
          "[kdrv_ble_app]")
{
    static uint8_t big[KDRV_BLE_APP_VALUE_MAX + 1U];
    uint8_t        out[KDRV_BLE_APP_VALUE_MAX];

    fresh();
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_app_set_value("temp", NULL, 0U));
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_app_check("temp", true, "x", 1U));
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_app_check("cmd", false, "x", 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_app_check("cmd", true, "x", 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_app_check("nope", false, "x", 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_app_check(NULL, false, "x", 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_app_check("temp", true, big, sizeof(big)));
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_app_check("temp", true, NULL, 2U));
    TEST_ASSERT_EQUAL_UINT16(0U, kdrv_ble_app_read(0U, out, sizeof(out)));
    static const kdrv_ble_app_service_t five = { SVC_UUID, s_five, 5U };

    fake_ble_app_service_set(&five);
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_app_check("a", false, "x", 1U));
    done();
}

TEST_CASE("notify with nobody subscribed sets the value, queues nothing and answers OK",
          "[kdrv_ble_app]")
{
    uint8_t  out[KDRV_BLE_APP_VALUE_MAX];
    uint8_t  chr = 0xFFU;
    uint16_t len = 0U;

    fresh();
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_app_notify("temp", "21.5", 4U));
    TEST_ASSERT_EQUAL_UINT16(4U, kdrv_ble_app_read(0U, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY("21.5", out, 4U);
    TEST_ASSERT_FALSE(kdrv_ble_app_peek(&chr, out, &len));
    TEST_ASSERT_FALSE(kdrv_ble_app_queued());
    done();
}

TEST_CASE("a subscribed central's notifications queue in order for the drain, and a full "
          "ring answers NO_RESOURCES with the value set", "[kdrv_ble_app]")
{
    uint8_t  out[KDRV_BLE_APP_VALUE_MAX];
    uint8_t  chr = 0xFFU;
    uint16_t len = 0U;
    uint8_t  b;

    fresh();
    kdrv_ble_app_subscribed(0U, true);
    for ( b = 0U; (uint8_t)KDRV_BLE_APP_RING_SLOTS > b; b++ )
    {
        TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_app_notify("temp", &b, 1U));
    }
    TEST_ASSERT_TRUE(kdrv_ble_app_queued());
    b = 0x77U;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, kdrv_ble_app_notify("temp", &b, 1U));
    TEST_ASSERT_EQUAL_UINT16(1U, kdrv_ble_app_read(0U, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8(0x77U, out[0]);

    for ( b = 0U; (uint8_t)KDRV_BLE_APP_RING_SLOTS > b; b++ )
    {
        TEST_ASSERT_TRUE(kdrv_ble_app_peek(&chr, out, &len));
        TEST_ASSERT_EQUAL_UINT8(0U, chr);
        TEST_ASSERT_EQUAL_UINT16(1U, len);
        TEST_ASSERT_EQUAL_UINT8(b, out[0]);
        kdrv_ble_app_pop();
    }
    TEST_ASSERT_FALSE(kdrv_ble_app_peek(&chr, out, &len));
    TEST_ASSERT_FALSE(kdrv_ble_app_queued());

    /* A link that ends forgets the subscription and what was queued. */
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_app_notify("temp", "z", 1U));
    kdrv_ble_app_link_reset();
    TEST_ASSERT_FALSE(kdrv_ble_app_peek(&chr, out, &len));
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_app_notify("temp", "z", 1U));
    TEST_ASSERT_FALSE(kdrv_ble_app_peek(&chr, out, &len));
    done();
}

TEST_CASE("a write past the table or longer than a value is refused and kept nowhere",
          "[kdrv_ble_app]")
{
    static const uint8_t v[3] = { 0x00U, 0x41U, 0x00U };

    /* A write the table takes goes to AO_Radio's callback, which the radio's
     * cases drive (test_radio_ao.c, "[ble_app]"). */
    fresh();
    TEST_ASSERT_NOT_NULL(kdrv_ble_app_scratch());
    TEST_ASSERT_FALSE(kdrv_ble_app_written(2U, v, sizeof(v)));
    TEST_ASSERT_FALSE(kdrv_ble_app_written(KDRV_BLE_APP_CHARS_MAX, v, sizeof(v)));
    TEST_ASSERT_FALSE(kdrv_ble_app_written(1U, v, (uint16_t)(KDRV_BLE_APP_VALUE_MAX + 1U)));
    TEST_ASSERT_FALSE(kdrv_ble_app_written(1U, NULL, 2U));
    done();
}
