/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_led_mgr.c
 * @brief AO_LedMgr's state machine, driven by hand over a mocked bus.
 */
#include "unity.h"

#include "qpc.h"
#include "Mockkhal_i2c.h"

#include "fakes.h"
#include "kaos.h"
#include "kbsp_owners.h"
#include "kdrv_buttons.h"
#include "kqp.h"
#include "lowpower_cases.h"
#include "qf_support.h"

/* The same register map the driver case pins down, repeated for the same
 * reason: importing the driver's own constants would agree by construction. */
#define REG_EN_RST      0x00U
#define EN_SLEEP        0x08U   /* the chip's sleep: nothing lit */
#define REG_LED_EN      0x04U
#define REG_ISET1       0x06U
#define REG_ISET2       0x07U
#define REG_ISET3       0x08U
#define CODE_FULL       39U
#define CODE_ZERO       0U

/* QF's publish/subscribe list. LedMgr subscribes to five signals during its
 * initial transition, and QActive_subscribe() writes into this. */
static QSubscrList l_subscrSto[MAX_PUB_SIG];

/**
 * @brief Construct LedMgr and take its initial transition.
 *
 * The AO is given a priority by hand. QACTIVE_START would normally do it,
 * and starting it would bring a scheduler with it - which is precisely what
 * this tier does not want. QActive_subscribe() is the one call in the initial
 * transition that needs the number.
 *
 * **No bus traffic is expected here**, and that is a fact about kdrv_led
 * rather than about LedMgr. The driver keeps its device handle in a file
 * static and kdrv_led_init() returns early once it is set, so by the time
 * these cases run the chip has already been brought up by the driver's own
 * cases earlier in the binary. If that ever stops being true - a renamed case
 * file reorders the link - these cases fail loudly with "khal_i2c_init called
 * more times than expected" rather than quietly asserting the wrong thing.
 */
static void led_mgr_bring_up(void)
{
    /* Once per binary, not once per case. QActive_register_() asserts on a
     * priority slot that is already taken (qf_qact.c), which is right for a
     * system where two AOs at one priority is a design error - and means the
     * registry is process-wide state a suite has to respect rather than
     * fight. QF has no unregister to undo it with.
     *
     * The flag is qf_support's rather than a static of this file's, because
     * test_led_test.c wants the same AO: two private once-flags are two
     * registrations, whichever order the linker puts the files in. */
    if ( qf_support_claim_ao(AO_LedMgr) )
    {
        QF_init();
        QActive_psInit(l_subscrSto, Q_DIM(l_subscrSto));

        LedMgr_ctor();
        AO_LedMgr->prio  = KODE_PRIO_LED;
        AO_LedMgr->pthre = KODE_PRIO_LED;
        QActive_register_(AO_LedMgr);

        QASM_INIT(AO_LedMgr, (void *)0, 0U);

        /* The initial transition claims the LED before it touches it. Two
         * owners of one device is the bug the ownership table exists to make
         * impossible, so an AO that forgot to claim is worth catching here
         * rather than on a board.
         *
         * This asks the real table, not a recording of it. It used to ask a
         * fake that granted every claim unconditionally - which meant the
         * assertion was true of a table that would have said yes to anything,
         * and proved rather less than it read as proving. */
        TEST_ASSERT_EQUAL_STRING("AO_LedMgr", kbsp_owner_name(KBSP_RES_LED));
    }
}

TEST_CASE("LedMgr turns a colour event into the chip's register writes",
          "[led_mgr]")
{
    LedRgbEvt e = { .super = QEVT_INITIALIZER(LED_RGB_SIG),
                    .r = 255U, .g = 0U, .b = 0U };

    Mockkhal_i2c_Init();
    led_mgr_bring_up();

    /* Red is the die on OUT3 (the board's wiring): the third current, the
     * third enable bit. */
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET1, CODE_ZERO, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET2, CODE_ZERO, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET3, CODE_FULL, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_LED_EN, 0x10U, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();

    QASM_DISPATCH(AO_LedMgr, &e.super, 0U);

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* A period below KODE_LED_BLINK_MIN_MS is refused rather than rounded to
 * something else, which would be a lie told to whoever asked. */
TEST_CASE("LedMgr rejects a blink period it cannot keep", "[led_mgr]")
{
    LedBlinkEvt e = { .super = QEVT_INITIALIZER(LED_BLINK_PERIOD_SIG),
                      .period_ms = (uint16_t)(KODE_LED_BLINK_MIN_MS - 1U) };

    Mockkhal_i2c_Init();
    led_mgr_bring_up();

    /* No bus traffic at all: the rejection happens in the AO, above the
     * driver, so the chip never hears about it. */
    QASM_DISPATCH(AO_LedMgr, &e.super, 0U);

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* Disabling the heartbeat must also put the LED out. Leaving it lit at
 * whatever phase the last toggle happened to stop on is the kind of bug that
 * only shows up as "the light sometimes stays on". */
TEST_CASE("disabling the heartbeat darkens the LED", "[led_mgr]")
{
    LedEvt e = { .super = QEVT_INITIALIZER(LED_HEARTBEAT_ENABLE_SIG),
                 .on = false };

    Mockkhal_i2c_Init();
    led_mgr_bring_up();

    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET1, CODE_ZERO, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET2, CODE_ZERO, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET3, CODE_ZERO, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_LED_EN, 0x00U, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    /* and, dark, the chip goes to sleep */
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_EN_RST, EN_SLEEP, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();

    QASM_DISPATCH(AO_LedMgr, &e.super, 0U);

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- The board's rest ------------------------------------------------------- */

/** @brief Register writes the chip has seen, whatever they were. */
static unsigned s_led_writes;

static kode_status_t count_write(khal_i2c_dev_t dev, uint8_t reg, uint8_t val, int ncalls)
{
    (void)dev; (void)reg; (void)val; (void)ncalls;
    s_led_writes += 1U;
    return KODE_OK;
}

/** @brief Start from a bus that counts, and an AO that has a queue to tick into. */
static void rest_up(void)
{
    Mockkhal_i2c_Init();
    led_mgr_bring_up();
    qf_support_give_queue(AO_LedMgr);
    khal_i2c_write_reg_StubWithCallback(&count_write);
    s_led_writes = 0U;
}

static void rest_down(void)
{
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/** @brief Ask for the heartbeat on, or off. */
static void heartbeat(bool on)
{
    LedEvt e = { .super = QEVT_INITIALIZER(LED_HEARTBEAT_ENABLE_SIG), .on = on };

    QASM_DISPATCH(AO_LedMgr, &e.super, 0U);
}

/* The LED goes dark and the heartbeat stops with the round; it comes back,
 * once, when the board wakes, if it was wanted. */
TEST_CASE("a round darkens the LED and stops the heartbeat; AWAKE brings it back",
          "[led_mgr][lowpower]")
{
    unsigned after_rest;

    rest_up();
    heartbeat(true);
    s_led_writes = 0U;

    lp_round(AO_LedMgr, KODE_LP_ASLEEP);
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0U, s_led_writes, "the LED was not written dark");
    lp_expect_answer(false, NULL);
    lp_expect_no_more();
    after_rest = s_led_writes;

    (void)qf_support_tick(AO_LedMgr, QF_SUPPORT_STEPS(KODE_LED_BLINK_MS) * 3U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(after_rest, s_led_writes, "the heartbeat beat in the rest");

    lp_awake(AO_LedMgr);
    (void)qf_support_tick(AO_LedMgr, QF_SUPPORT_STEPS(KODE_LED_BLINK_MS) + 2U);
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(after_rest, s_led_writes,
                                          "the heartbeat did not come back");

    /* A second AWAKE (an abandoned round publishes one) arms nothing twice. */
    lp_awake(AO_LedMgr);

    heartbeat(false);
    (void)qf_support_tick(AO_LedMgr, 2U);
    rest_down();
}

/* A heartbeat nobody asked for is not started by the wake. */
TEST_CASE("AWAKE leaves a LED that never blinked alone, and a round after it rests again",
          "[led_mgr][lowpower]")
{
    rest_up();
    heartbeat(false);
    s_led_writes = 0U;

    lp_round(AO_LedMgr, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    lp_round(AO_LedMgr, KODE_LP_OFF);
    lp_expect_answer(false, NULL);
    s_led_writes = 0U;

    lp_awake(AO_LedMgr);
    (void)qf_support_tick(AO_LedMgr, QF_SUPPORT_STEPS(KODE_LED_BLINK_MS) * 3U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_led_writes, "the wake started a heartbeat");
    rest_down();
}

/* The operator is looking at the LED, and the pad is their answer. */
TEST_CASE("the led row refuses the round, and rests once it is over", "[led_mgr][lowpower]")
{
    ButtonEvt no = { .super = QEVT_INITIALIZER(BUTTON_EVT_SIG),
                     .id = (uint8_t)KDRV_BTN_BOT, .pressed = true, .t_ms = 0U };

    rest_up();
    led_test_request();
    (void)qf_support_pump(AO_LedMgr, 4U);
    s_led_writes = 0U;

    lp_round(AO_LedMgr, KODE_LP_ASLEEP);
    lp_expect_answer(true, "led test");
    lp_expect_no_more();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_led_writes, "the colour under the operator's eye was put out");

    QASM_DISPATCH(AO_LedMgr, &no.super, 0U);    /* "no": the row ends, failed */
    (void)qf_support_tick(AO_LedMgr, 2U);
    lp_round(AO_LedMgr, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    lp_awake(AO_LedMgr);                        /* leave it as the next file finds it */
    rest_down();
}

/* A radio wake lights nothing: the LED stays dark and the heartbeat stays
 * stopped until the whole board is AWAKE. */
TEST_CASE("STATE(RADIO) leaves a rested LED alone; AWAKE still brings it back",
          "[led_mgr][lowpower]")
{
    unsigned after_rest;

    rest_up();
    heartbeat(true);
    lp_round(AO_LedMgr, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    after_rest = s_led_writes;

    lp_radio(AO_LedMgr);
    (void)qf_support_tick(AO_LedMgr, QF_SUPPORT_STEPS(KODE_LED_BLINK_MS) * 3U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(after_rest, s_led_writes, "the radio wake wrote the LED");

    lp_awake(AO_LedMgr);
    (void)qf_support_tick(AO_LedMgr, QF_SUPPORT_STEPS(KODE_LED_BLINK_MS) + 2U);
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(after_rest, s_led_writes,
                                          "the heartbeat did not come back on AWAKE");
    heartbeat(false);
    (void)qf_support_tick(AO_LedMgr, 2U);
    rest_down();
}
