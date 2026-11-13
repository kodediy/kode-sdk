/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_ble_adv.c
 * @brief The BLE scan response's packing: name and manufacturer data, refused whole if too big;
 *        and what follows each pulse of a pulsed advert.
 */
#include <string.h>

#include "unity.h"

#include "kdrv_ble.h"
#include "khal_identity.h"

/** @brief A byte the packing never writes, to see what it left alone. */
#define ADV_UNTOUCHED  0xA5U

/** @brief A name of 29 characters: the longest a scan response holds alone. */
#define ADV_NAME_29  "0123456789abcdefghijklmnopqrs"

/** @brief The packet buffer, filled with ADV_UNTOUCHED. */
static uint8_t s_out[KDRV_BLE_ADV_MAX + 4U];

/** @brief Fill the buffer with a byte the packing never writes. */
static void adv_fresh(void)
{
    (void)memset(s_out, (int)ADV_UNTOUCHED, sizeof(s_out));
}

TEST_CASE("a name and data pack as a complete local name, then manufacturer data", "[kdrv_ble_adv]")
{
    static const uint8_t data[4]  = { 0xFFU, 0xFFU, 0x01U, 0x02U };
    static const uint8_t want[13] = { 0x06U, 0x09U, 'D', 'o', 't', '-', '7',
                                      0x05U, 0xFFU, 0xFFU, 0xFFU, 0x01U, 0x02U };
    size_t n = 0U;

    adv_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_ble_adv_pack("Dot-7", data, sizeof(data), s_out,
                                                     KDRV_BLE_ADV_MAX, &n));
    TEST_ASSERT_EQUAL_UINT(13U, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, s_out, 13U);
    TEST_ASSERT_EQUAL_HEX8(ADV_UNTOUCHED, s_out[13]);
}

TEST_CASE("a name alone, or data alone, packs one structure; neither packs none", "[kdrv_ble_adv]")
{
    static const uint8_t data[2] = { 0x4CU, 0x00U };
    size_t n = 0U;

    adv_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_ble_adv_pack("ab", NULL, 0U, s_out, KDRV_BLE_ADV_MAX, &n));
    TEST_ASSERT_EQUAL_UINT(4U, n);
    TEST_ASSERT_EQUAL_HEX8(0x03U, s_out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x09U, s_out[1]);

    adv_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_ble_adv_pack(NULL, data, sizeof(data), s_out,
                                                     KDRV_BLE_ADV_MAX, &n));
    TEST_ASSERT_EQUAL_UINT(4U, n);
    TEST_ASSERT_EQUAL_HEX8(0x03U, s_out[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFFU, s_out[1]);
    TEST_ASSERT_EQUAL_HEX8(0x4CU, s_out[2]);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, s_out[3], "the company id was not the data's own");

    adv_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_ble_adv_pack("", NULL, 0U, s_out, KDRV_BLE_ADV_MAX, &n));
    TEST_ASSERT_EQUAL_UINT(0U, n);
    TEST_ASSERT_EQUAL_HEX8(ADV_UNTOUCHED, s_out[0]);
}

TEST_CASE("a packet of exactly 31 bytes fits, one more is refused", "[kdrv_ble_adv]")
{
    uint8_t data[KDRV_BLE_ADV_MAX];
    size_t  n = 0U;

    (void)memset(data, 0x11, sizeof(data));
    adv_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_ble_adv_pack(ADV_NAME_29, NULL, 0U, s_out,
                                                     KDRV_BLE_ADV_MAX, &n));
    TEST_ASSERT_EQUAL_UINT(KDRV_BLE_ADV_MAX, n);

    /* 2 + 10 and 2 + 17: 31. */
    adv_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_ble_adv_pack("0123456789", data, 17U, s_out,
                                                     KDRV_BLE_ADV_MAX, &n));
    TEST_ASSERT_EQUAL_UINT(KDRV_BLE_ADV_MAX, n);
}

TEST_CASE("a name and data that do not fit are refused and change nothing", "[kdrv_ble_adv]")
{
    uint8_t data[KDRV_BLE_ADV_MAX + 1U];
    uint8_t before[sizeof(s_out)];
    size_t  n = 7U;

    (void)memset(data, 0x22, sizeof(data));
    adv_fresh();
    (void)memcpy(before, s_out, sizeof(before));

    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kdrv_ble_adv_pack("0123456789", data, 18U, s_out,
                                                               KDRV_BLE_ADV_MAX, &n));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kdrv_ble_adv_pack(ADV_NAME_29 "t", NULL, 0U, s_out,
                                                               KDRV_BLE_ADV_MAX, &n));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kdrv_ble_adv_pack(NULL, data, sizeof(data), s_out,
                                                               KDRV_BLE_ADV_MAX, &n));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_NO_RESOURCES,
                                  kdrv_ble_adv_pack("Dot-7", NULL, 0U, s_out, 6U, &n),
                                  "a buffer smaller than the packet was written past");
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(before, s_out, sizeof(s_out), "a refusal cut and wrote");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(7U, n, "a refusal set the length");
}

TEST_CASE("data shorter than its company id, or missing, is refused", "[kdrv_ble_adv]")
{
    static const uint8_t one[1] = { 0x4CU };
    size_t n = 0U;

    adv_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_ble_adv_pack("x", one, 1U, s_out, KDRV_BLE_ADV_MAX, &n));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_ble_adv_pack("x", NULL, 4U, s_out, KDRV_BLE_ADV_MAX, &n));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_ble_adv_pack("x", NULL, 0U, NULL, KDRV_BLE_ADV_MAX, &n));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_ble_adv_pack("x", NULL, 0U, s_out, KDRV_BLE_ADV_MAX, NULL));
    TEST_ASSERT_EQUAL_HEX8(ADV_UNTOUCHED, s_out[0]);
}

TEST_CASE("the board's own name is its serial, or KODE-DOT when it has none", "[kdrv_ble_adv]")
{
    const char *const name = kdrv_ble_serial_name();

    TEST_ASSERT_NOT_NULL(name);
    if ( khal_identity_valid() )
    {
        TEST_ASSERT_EQUAL_STRING(khal_identity_serial(), name);
    }
    else
    {
        TEST_ASSERT_EQUAL_STRING("KODE-DOT", name);
    }
}

/* ---- The connection parameters the board asks for ---- */

TEST_CASE("the awake connection set is 15 ms with no latency and keeps Apple's rules", "[kdrv_ble_adv]")
{
    kdrv_ble_conn_params_t p;

    kdrv_ble_conn_params(false, &p);
    TEST_ASSERT_EQUAL_UINT16(12U, p.itvl_min);   /* 15 ms */
    TEST_ASSERT_EQUAL_UINT16(12U, p.itvl_max);
    TEST_ASSERT_EQUAL_UINT16(0U, p.latency);
    TEST_ASSERT_EQUAL_UINT16(400U, p.timeout);   /* 4 s */
    TEST_ASSERT_TRUE(kdrv_ble_conn_params_ok(&p));
}

TEST_CASE("the rest connection set is 300-330 ms with latency 4 and keeps Apple's rules", "[kdrv_ble_adv]")
{
    kdrv_ble_conn_params_t p;

    kdrv_ble_conn_params(true, &p);
    TEST_ASSERT_EQUAL_UINT16(240U, p.itvl_min);  /* 300 ms */
    TEST_ASSERT_EQUAL_UINT16(264U, p.itvl_max);  /* 330 ms */
    TEST_ASSERT_EQUAL_UINT16(4U, p.latency);
    TEST_ASSERT_EQUAL_UINT16(600U, p.timeout);   /* 6 s */
    TEST_ASSERT_TRUE(kdrv_ble_conn_params_ok(&p));
}

TEST_CASE("Apple's connection rules refuse each set that breaks one", "[kdrv_ble_adv]")
{
    kdrv_ble_conn_params_t p;

    TEST_ASSERT_FALSE(kdrv_ble_conn_params_ok(NULL));

    kdrv_ble_conn_params(true, &p);
    p.itvl_min = 11U;                 /* under 15 ms */
    TEST_ASSERT_FALSE(kdrv_ble_conn_params_ok(&p));

    kdrv_ble_conn_params(true, &p);
    p.itvl_max = 251U;                /* max < min + 15 ms (300 + 13.75) */
    TEST_ASSERT_FALSE(kdrv_ble_conn_params_ok(&p));

    kdrv_ble_conn_params(true, &p);
    p.latency = 6U;                   /* 330 ms x 7 = 2.31 s, over 2 s */
    TEST_ASSERT_FALSE(kdrv_ble_conn_params_ok(&p));

    kdrv_ble_conn_params(false, &p);
    p.latency = 31U;                  /* over 30 */
    TEST_ASSERT_FALSE(kdrv_ble_conn_params_ok(&p));

    kdrv_ble_conn_params(true, &p);
    p.timeout = 601U;                 /* over 6 s */
    TEST_ASSERT_FALSE(kdrv_ble_conn_params_ok(&p));

    kdrv_ble_conn_params(true, &p);
    p.timeout = 495U;                 /* 4.95 s: not more than 3 x 1.65 s */
    TEST_ASSERT_FALSE(kdrv_ble_conn_params_ok(&p));

    kdrv_ble_conn_params(false, &p);
    p.timeout = 199U;                 /* under 2 s */
    TEST_ASSERT_FALSE(kdrv_ble_conn_params_ok(&p));
}

TEST_CASE("asking for the connection set into NULL writes nothing", "[kdrv_ble_adv]")
{
    kdrv_ble_conn_params(true, NULL);
    kdrv_ble_conn_params(false, NULL);
    TEST_ASSERT_FALSE(kdrv_ble_conn_params_ok(NULL));
}

/* ---- The pulses of a pulsed advert ------------------------------------------ */

/** @brief The slow interval's period, ms: 1636 units of 0.625 ms, rounded down. */
#define PULSE_SLOW_PERIOD_MS  1022U

/** @brief The pause after a pulse of KDRV_BLE_PULSE_ON_MS at the slow interval, ms. */
#define PULSE_SLOW_PAUSE_MS   (PULSE_SLOW_PERIOD_MS - KDRV_BLE_PULSE_ON_MS)

/** @brief A leg with time left to run, ms: what Pair new device leaves after the fast one. */
#define PULSE_LEG_MS          270000U

/** @brief A leg that ends before the next pulse is due, ms. */
#define PULSE_SHORT_LEG_MS    1000U

/** @brief A train at the end of its first pulse of the slow advert, with no end to the leg. */
static kdrv_ble_pulse_t pulse_first_end(void)
{
    kdrv_ble_pulse_t p;

    (void)memset(&p, 0, sizeof(p));
    p.live         = true;
    p.timed_out    = true;
    p.itvl         = (uint16_t)KDRV_BLE_ITVL_SLOW;
    p.leg_run_ms   = KDRV_BLE_PULSE_ON_MS;
    p.pulse_run_ms = KDRV_BLE_PULSE_ON_MS;
    return p;
}

TEST_CASE("the slow interval's period is 1022 ms and a 100 ms pulse leaves 922 of it", "[kdrv_ble_adv][pulse]")
{
    TEST_ASSERT_EQUAL_UINT32(PULSE_SLOW_PERIOD_MS, kdrv_ble_pulse_period_ms((uint16_t)KDRV_BLE_ITVL_SLOW));
    TEST_ASSERT_EQUAL_UINT32(PULSE_SLOW_PAUSE_MS,
                             kdrv_ble_pulse_pause_ms((uint16_t)KDRV_BLE_ITVL_SLOW, KDRV_BLE_PULSE_ON_MS));
}

TEST_CASE("a pulse that ended late is followed at once, never after a wait of 0", "[kdrv_ble_adv][pulse]")
{
    TEST_ASSERT_EQUAL_UINT32(KDRV_BLE_PULSE_WAIT_MIN_MS,
                             kdrv_ble_pulse_pause_ms((uint16_t)KDRV_BLE_ITVL_SLOW, PULSE_SLOW_PERIOD_MS));
    TEST_ASSERT_EQUAL_UINT32(KDRV_BLE_PULSE_WAIT_MIN_MS,
                             kdrv_ble_pulse_pause_ms((uint16_t)KDRV_BLE_ITVL_SLOW, UINT32_MAX));
}

TEST_CASE("a pulse's end with no end to the leg leads to the next pulse after the pause", "[kdrv_ble_adv][pulse]")
{
    kdrv_ble_pulse_t const p    = pulse_first_end();
    uint32_t               wait = 0U;

    TEST_ASSERT_EQUAL_INT((int)KDRV_BLE_PULSE_NEXT, (int)kdrv_ble_pulse_next(&p, &wait));
    TEST_ASSERT_EQUAL_UINT32(PULSE_SLOW_PAUSE_MS, wait);
}

TEST_CASE("a pulse's end with time left in the leg leads to the next pulse after the pause", "[kdrv_ble_adv][pulse]")
{
    kdrv_ble_pulse_t p    = pulse_first_end();
    uint32_t         wait = 0U;

    p.leg_ms = PULSE_LEG_MS;
    TEST_ASSERT_EQUAL_INT((int)KDRV_BLE_PULSE_NEXT, (int)kdrv_ble_pulse_next(&p, &wait));
    TEST_ASSERT_EQUAL_UINT32(PULSE_SLOW_PAUSE_MS, wait);
}

TEST_CASE("a leg that ends before the next pulse waits for its own end, and is IDLE then", "[kdrv_ble_adv][pulse]")
{
    kdrv_ble_pulse_t p    = pulse_first_end();
    uint32_t         wait = 0U;

    p.leg_ms = PULSE_SHORT_LEG_MS;
    TEST_ASSERT_EQUAL_INT((int)KDRV_BLE_PULSE_NEXT, (int)kdrv_ble_pulse_next(&p, &wait));
    TEST_ASSERT_EQUAL_UINT32(PULSE_SHORT_LEG_MS - KDRV_BLE_PULSE_ON_MS, wait);

    p.leg_run_ms = PULSE_SHORT_LEG_MS;
    TEST_ASSERT_EQUAL_INT((int)KDRV_BLE_PULSE_IDLE, (int)kdrv_ble_pulse_next(&p, &wait));
    TEST_ASSERT_EQUAL_UINT32(0U, wait);
}

TEST_CASE("a pulse's end with the leg over is IDLE, a leg of no end never is", "[kdrv_ble_adv][pulse]")
{
    kdrv_ble_pulse_t p    = pulse_first_end();
    uint32_t         wait = 0U;

    p.leg_ms     = PULSE_LEG_MS;
    p.leg_run_ms = PULSE_LEG_MS + KDRV_BLE_PULSE_ON_MS;
    TEST_ASSERT_EQUAL_INT((int)KDRV_BLE_PULSE_IDLE, (int)kdrv_ble_pulse_next(&p, &wait));

    p.leg_ms     = 0U;
    p.leg_run_ms = UINT32_MAX;
    TEST_ASSERT_EQUAL_INT((int)KDRV_BLE_PULSE_NEXT, (int)kdrv_ble_pulse_next(&p, &wait));
}

TEST_CASE("after a stop or a connection nothing follows, whatever else holds", "[kdrv_ble_adv][pulse]")
{
    kdrv_ble_pulse_t p    = pulse_first_end();
    uint32_t         wait = 1U;

    p.live       = false;
    p.leg_ms     = PULSE_SHORT_LEG_MS;
    p.leg_run_ms = PULSE_LEG_MS;
    p.refused    = (uint8_t)KDRV_BLE_PULSE_REFUSALS_MAX;
    p.timed_out  = false;
    TEST_ASSERT_EQUAL_INT((int)KDRV_BLE_PULSE_DONE, (int)kdrv_ble_pulse_next(&p, &wait));
    TEST_ASSERT_EQUAL_UINT32(0U, wait);
}

TEST_CASE("a pulse that NimBLE or the controller ended is IDLE: AO_Radio plans again", "[kdrv_ble_adv][pulse]")
{
    kdrv_ble_pulse_t p    = pulse_first_end();
    uint32_t         wait = 0U;

    p.timed_out = false;
    TEST_ASSERT_EQUAL_INT((int)KDRV_BLE_PULSE_IDLE, (int)kdrv_ble_pulse_next(&p, &wait));
}

TEST_CASE("the third refusal in a row is IDLE, the first two wait for a whole period", "[kdrv_ble_adv][pulse]")
{
    kdrv_ble_pulse_t p    = pulse_first_end();
    uint32_t         wait = 0U;

    p.pulse_run_ms = 0U;
    for ( uint8_t n = 1U; n < (uint8_t)KDRV_BLE_PULSE_REFUSALS_MAX; ++n )
    {
        p.refused = n;
        TEST_ASSERT_EQUAL_INT((int)KDRV_BLE_PULSE_NEXT, (int)kdrv_ble_pulse_next(&p, &wait));
        TEST_ASSERT_EQUAL_UINT32(PULSE_SLOW_PERIOD_MS, wait);
    }
    p.refused = (uint8_t)KDRV_BLE_PULSE_REFUSALS_MAX;
    TEST_ASSERT_EQUAL_INT((int)KDRV_BLE_PULSE_IDLE, (int)kdrv_ble_pulse_next(&p, &wait));
}

TEST_CASE("judging nothing follows with nothing, and a verdict needs no place for its wait", "[kdrv_ble_adv][pulse]")
{
    kdrv_ble_pulse_t const p    = pulse_first_end();
    uint32_t               wait = 1U;

    TEST_ASSERT_EQUAL_INT((int)KDRV_BLE_PULSE_DONE, (int)kdrv_ble_pulse_next(NULL, &wait));
    TEST_ASSERT_EQUAL_UINT32(0U, wait);
    TEST_ASSERT_EQUAL_INT((int)KDRV_BLE_PULSE_NEXT, (int)kdrv_ble_pulse_next(&p, NULL));
}
