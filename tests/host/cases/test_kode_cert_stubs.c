/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_cert_stubs.c
 * @brief The certification calls of a build without CONFIG_KODEOS_NFC_CERT.
 */
#define kode_cert_nfc_carrier     nocert_nfc_carrier
#define kode_cert_rfid_carrier    nocert_rfid_carrier
#define kode_cert_nfc_field_read  nocert_nfc_field_read
#define kode_cert_rfid_field_read nocert_rfid_field_read

#include "../../../components/kode_sys/src/kode_cert_nfc.c"
#include "../../../components/kode_sys/src/kode_cert_rfid.c"

#include "unity.h"

#include "ao_spy.h"
#include "qf_support.h"

#if CONFIG_KODEOS_NFC_CERT
#error "this file tests the build without the certification carriers"
#endif

TEST_CASE("without the certification build both carriers are refused at once and post nothing",
          "[kode_cert]")
{
    qf_support_init();
    ao_spy_drain();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, nocert_nfc_carrier(ao_spy(), 1));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, nocert_nfc_carrier(ao_spy(), 0));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, nocert_rfid_carrier(ao_spy(), 2));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, nocert_rfid_carrier(ao_spy(), 0));
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
}

TEST_CASE("without the certification build a bad listen window is still refused at once",
          "[kode_cert]")
{
    qf_support_init();
    ao_spy_drain();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, nocert_nfc_field_read(ao_spy(), 0U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, nocert_nfc_field_read(ao_spy(), KODE_CERT_NFC_FIELD_MS_MAX + 1U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, nocert_rfid_field_read(ao_spy(), NULL, 0U, 0U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, nocert_rfid_field_read(ao_spy(), NULL, 0U,
                                                              KODE_CERT_RFID_FIELD_MS_MAX + 1U));
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
}
