/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_ble_app_select.c
 * @brief The app's choice of service: made before the table is built, refused after.
 */
#include <stddef.h>

#include "unity.h"

#include "fakes.h"
#include "kdrv_ble.h"
#include "kdrv_ble_app.h"
#include "kode_sys.h"

/** @brief One characteristic, enough for a service. */
static const kdrv_ble_app_char_t s_one[] = {
    { "tx", "6e400003-b5a3-f393-e0a9-e50e24dcca9e", KDRV_BLE_APP_READ | KDRV_BLE_APP_NOTIFY },
};

/** @brief The service the app declared, KODE_BLE_SERVICE()'s. */
static const kdrv_ble_app_service_t s_declared = { "6e400001-b5a3-f393-e0a9-e50e24dcca9e", s_one, 1U };

/** @brief A second service the app may choose. */
static const kdrv_ble_app_service_t s_other = { "6ba1b218-15a8-461f-9fa8-5dcae273eafd", s_one, 1U };

/** @brief A fresh boot: the declared service, nothing chosen, no table. */
static void fresh(void)
{
    kdrv_ble_app_select_forget();
    fake_ble_app_service_set(&s_declared);
}

/** @brief Put everything back as a case found it. */
static void done(void)
{
    kdrv_ble_app_select_forget();
    fake_ble_app_service_set(NULL);
}

TEST_CASE("with nothing chosen the declared service is served", "[kdrv_ble_app]")
{
    fresh();
    TEST_ASSERT_EQUAL_PTR(&s_declared, kdrv_ble_app_service());
    TEST_ASSERT_EQUAL_PTR(&s_declared, kdrv_ble_app_fix());
    TEST_ASSERT_EQUAL_PTR(&s_declared, kdrv_ble_app_service());
    done();
}

TEST_CASE("a service chosen before the start is the one the table is built from", "[kdrv_ble_app]")
{
    fresh();
    TEST_ASSERT_TRUE(kdrv_ble_app_select(&s_other));
    TEST_ASSERT_EQUAL_PTR(&s_other, kdrv_ble_app_service());
    TEST_ASSERT_EQUAL_PTR(&s_other, kdrv_ble_app_fix());
    TEST_ASSERT_EQUAL_PTR(&s_other, kdrv_ble_app_service());
    done();
}

TEST_CASE("NULL chosen serves none of the app's services", "[kdrv_ble_app]")
{
    fresh();
    TEST_ASSERT_TRUE(kdrv_ble_app_select(NULL));
    TEST_ASSERT_NULL(kdrv_ble_app_service());
    TEST_ASSERT_NULL(kdrv_ble_app_fix());
    TEST_ASSERT_NULL(kdrv_ble_app_service());
    done();
}

TEST_CASE("a choice after the table is registered is refused and changes nothing", "[kdrv_ble_app]")
{
    fresh();
    TEST_ASSERT_TRUE(kdrv_ble_app_select(&s_other));
    TEST_ASSERT_EQUAL_PTR(&s_other, kdrv_ble_app_fix());
    TEST_ASSERT_FALSE(kdrv_ble_app_select(NULL));
    TEST_ASSERT_FALSE(kdrv_ble_app_select(&s_declared));
    TEST_ASSERT_EQUAL_PTR(&s_other, kdrv_ble_app_service());
    /* A second build in the same boot keeps the first one's service. */
    TEST_ASSERT_EQUAL_PTR(&s_other, kdrv_ble_app_fix());
    done();
}

TEST_CASE("kode_sys_ble_service_select keeps a choice before the start and refuses one after",
          "[kode_sys_ble]")
{
    static const kode_sys_ble_char_t   chars[] = {
        { "rx", "6e400002-b5a3-f393-e0a9-e50e24dcca9e", KODE_SYS_BLE_WRITE },
    };
    static KODE_SYS_BLE_SERVICE_NAMED(named, "6e400001-b5a3-f393-e0a9-e50e24dcca9e", chars);

    fresh();
    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_ble_service_select(&named));
    TEST_ASSERT_EQUAL_PTR((const void *)&named, (const void *)kdrv_ble_app_service());
    (void)kdrv_ble_app_fix();
    TEST_ASSERT_EQUAL(KODE_DENIED, kode_sys_ble_service_select(NULL));
    TEST_ASSERT_EQUAL_PTR((const void *)&named, (const void *)kdrv_ble_app_service());
    done();
}
