/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_ir.c
 * @brief The driver's send: what it refuses before the wire, and the C5's word on the frame.
 *
 * The suite mocks kdrv_ir.h for every AO case, so the real driver is compiled
 * here under other names (real_kdrv_ir_*, main/CMakeLists.txt): this file
 * calls kdrv_ir_send() and reaches the real one. The C5 is fakes/
 * fake_kdrv_radio.c's peer data; its answer is delivered from inside the
 * driver's wait, where it arrives on the board too.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "fakes.h"
#include "kdrv_ir.h"
#include "kir_wire.h"
#include "knec.h"

/** @brief Symbols the cases send. */
#define TEST_N 2U

/** @brief The status the C5's KIR_EVT_SENT carries in the next answer. */
static uint8_t s_sent_status;

/** @brief Whether the C5's answer was delivered, from inside the driver's wait. */
static bool s_answered;

/** @brief The C5's KIR_EVT_SENT with s_sent_status, as its firmware sends it. */
static void c5_says_sent(void)
{
    const kir_status_t evt = { (uint8_t)KIR_EVT_SENT, (uint8_t)KIR_WIRE_VERSION, s_sent_status };

    s_answered = true;
    fake_peer_deliver(KIR_MSG_EVT, (const uint8_t *)&evt, sizeof(evt));
}

/** @brief The driver up with the link up, nothing in flight and nothing sent. */
static void fresh(void)
{
    fake_radio_set_link(true);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_ir_init(NULL, NULL));
    /* A transmission an earlier case left in flight ends here. */
    s_sent_status = (uint8_t)KIR_OK;
    c5_says_sent();
    fake_sem_reset();
    fake_peer_reset();
    s_answered = false;
}

/**
 * @brief Send a frame of TEST_N symbols that the C5 answers with @p status.
 * @param mark_us    Every mark.
 * @param space_us   Every space.
 * @param carrier_hz The carrier.
 * @param status     What the C5 says of the frame.
 * @return What kdrv_ir_send() answered.
 */
static kode_status_t send_answered(uint16_t mark_us, uint16_t space_us, uint32_t carrier_hz,
                                   uint8_t status)
{
    kdrv_ir_symbol_t sym[TEST_N];

    for ( uint32_t i = 0U; TEST_N > i; i++ )
    {
        sym[i].mark_us  = mark_us;
        sym[i].space_us = space_us;
    }
    s_sent_status = status;
    fake_sem_wait_hook(&c5_says_sent);
    return kdrv_ir_send(sym, TEST_N, carrier_hz);
}

/** @brief Expect a frame to be refused before the wire: nothing is sent. */
static void expect_refused(uint16_t mark_us, uint16_t space_us, uint32_t carrier_hz)
{
    size_t len = 1U;

    fresh();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, send_answered(mark_us, space_us, carrier_hz, (uint8_t)KIR_OK));
    (void)fake_peer_last_sent(&len);
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)len);
    fake_sem_reset();
}

/** @brief Expect a frame to go out and the C5's word on it to be waited for. */
static void expect_sent(uint16_t mark_us, uint16_t space_us, uint32_t carrier_hz)
{
    fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, send_answered(mark_us, space_us, carrier_hz, (uint8_t)KIR_OK));
    TEST_ASSERT_TRUE(s_answered);
    TEST_ASSERT_EQUAL_HEX32(KIR_MSG_REQ, fake_peer_last_msg_id());
}

TEST_CASE("a send waits for the C5 to say the frame went out", "[kdrv_ir]")
{
    fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, send_answered(560U, 560U, KDRV_IR_CARRIER_HZ, (uint8_t)KIR_OK));
    TEST_ASSERT_TRUE_MESSAGE(s_answered, "the send returned before the C5 said anything");
    TEST_ASSERT_EQUAL_UINT32(KDRV_IR_SENT_MS, fake_sem_last_wait_ms());
}

TEST_CASE("a frame the C5 says failed is an error", "[kdrv_ir]")
{
    fresh();
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, send_answered(560U, 560U, KDRV_IR_CARRIER_HZ, (uint8_t)KIR_FAIL));
    TEST_ASSERT_TRUE(s_answered);
    fresh();
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
                          send_answered(560U, 560U, KDRV_IR_CARRIER_HZ, (uint8_t)KIR_BAD_REQUEST));
    TEST_ASSERT_TRUE(s_answered);
}

TEST_CASE("a frame the C5 says nothing of is a timeout", "[kdrv_ir]")
{
    kdrv_ir_symbol_t sym[TEST_N] = { { 560U, 560U }, { 560U, 0U } };

    fresh();
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kdrv_ir_send(sym, TEST_N, KDRV_IR_CARRIER_HZ));
    TEST_ASSERT_EQUAL_HEX32(KIR_MSG_REQ, fake_peer_last_msg_id());
}

TEST_CASE("a carrier outside 20000 to 500000 Hz is refused and nothing goes out", "[kdrv_ir]")
{
    expect_refused(560U, 560U, 38U);
    expect_refused(560U, 560U, KDRV_IR_CARRIER_MIN_HZ - 1U);
    expect_refused(560U, 560U, KDRV_IR_CARRIER_MAX_HZ + 1U);
    expect_sent(560U, 560U, KDRV_IR_CARRIER_MIN_HZ);
    expect_sent(560U, 560U, 38000U);
    expect_sent(560U, 560U, KDRV_IR_CARRIER_MAX_HZ);
    TEST_ASSERT_EQUAL_UINT32(20000U, KDRV_IR_CARRIER_MIN_HZ);
    TEST_ASSERT_EQUAL_UINT32(500000U, KDRV_IR_CARRIER_MAX_HZ);
}

TEST_CASE("a mark or a space over 32767 us is refused and nothing goes out", "[kdrv_ir]")
{
    expect_refused(KDRV_IR_DURATION_MAX_US + 1U, 560U, KDRV_IR_CARRIER_HZ);
    expect_refused(560U, KDRV_IR_DURATION_MAX_US + 1U, KDRV_IR_CARRIER_HZ);
    expect_sent(KDRV_IR_DURATION_MAX_US, KDRV_IR_DURATION_MAX_US, KDRV_IR_CARRIER_HZ);
    TEST_ASSERT_EQUAL_UINT32(32767U, KDRV_IR_DURATION_MAX_US);
}

/** @brief A frame of exactly KDRV_IR_FRAME_MAX_MS: FRAME_N marks and spaces of FRAME_HALF_US. */
#define FRAME_N        50U
#define FRAME_HALF_US  15000U
#define US_PER_MS      1000U

/**
 * @brief Send @p sym, the C5 saying it went out, and say whether the wire saw it.
 * @param sym The symbols, FRAME_N of them.
 * @return What kdrv_ir_send() answered.
 */
static kode_status_t frame_answered(const kdrv_ir_symbol_t *sym)
{
    kode_status_t rc;

    fresh();
    s_sent_status = (uint8_t)KIR_OK;
    fake_sem_wait_hook(&c5_says_sent);
    rc = kdrv_ir_send(sym, FRAME_N, KDRV_IR_CARRIER_HZ);
    fake_sem_reset();
    return rc;
}

TEST_CASE("a frame longer than 1500 ms, a zero space counted as 32767 us, is refused and nothing goes out",
          "[kdrv_ir]")
{
    kdrv_ir_symbol_t sym[FRAME_N];
    size_t           len = 1U;

    TEST_ASSERT_EQUAL_UINT32(1500U, KDRV_IR_FRAME_MAX_MS);
    TEST_ASSERT_EQUAL_UINT32(KDRV_IR_FRAME_MAX_MS * US_PER_MS, FRAME_N * 2U * FRAME_HALF_US);
    for ( uint32_t i = 0U; FRAME_N > i; i++ )
    {
        sym[i].mark_us  = (uint16_t)FRAME_HALF_US;
        sym[i].space_us = (uint16_t)FRAME_HALF_US;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, frame_answered(sym), "a frame of exactly 1500 ms goes out");

    sym[FRAME_N - 1U].space_us = (uint16_t)(FRAME_HALF_US + 1U);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, frame_answered(sym));
    (void)fake_peer_last_sent(&len);
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)len);

    sym[FRAME_N - 1U].space_us = 0U;
    len                        = 1U;
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, frame_answered(sym), "the zero space is sent as 32767 us");
    (void)fake_peer_last_sent(&len);
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)len);
}

TEST_CASE("NEC's frames, on the board's own carrier, are ones a send takes", "[kdrv_ir]")
{
    static const uint16_t value[] = { 0x0000U, 0x00FFU, 0xFFFFU };
    kdrv_ir_symbol_t      sym[KNEC_SYMBOLS];

    TEST_ASSERT_TRUE(KDRV_IR_CARRIER_HZ >= KDRV_IR_CARRIER_MIN_HZ);
    TEST_ASSERT_TRUE(KDRV_IR_CARRIER_HZ <= KDRV_IR_CARRIER_MAX_HZ);
    for ( uint32_t v = 0U; (sizeof(value) / sizeof(value[0])) > v; v++ )
    {
        size_t n = 0U;

        TEST_ASSERT_EQUAL_INT(KODE_OK, knec_encode(value[v], value[v], sym, KNEC_SYMBOLS, &n));
        for ( size_t i = 0U; n > i; i++ )
        {
            TEST_ASSERT_TRUE(KDRV_IR_DURATION_MAX_US >= sym[i].mark_us);
            TEST_ASSERT_TRUE(KDRV_IR_DURATION_MAX_US >= sym[i].space_us);
        }
    }
}
