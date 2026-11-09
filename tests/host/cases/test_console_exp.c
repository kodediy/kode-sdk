/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_exp.c
 * @brief `exp`, `exp_help` and the header's lines in `ps`, from the wire.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "unity.h"

#include "qpc.h"

#include "Mockkhal_exp_port.h"
#include "Mockkhal_gpio.h"
#include "Mockkhal_pwm.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kaos_wire.h"
#include "kbsp_owners.h"
#include "kcli_proto.h"
#include "khal_exp.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

/* The command watchdog, in QP ticks. Repeated rather than imported for the
 * same reason the other console files repeat it. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)

/* Ticks enough to drain every slice of `ps` in any build this suite compiles.
 * The listing is the AO roster (at most twenty rows), the tasks that are not
 * Active Objects (at most four), KBSP_RES_MAX resources and two records at
 * the end, at KAOS_WIRE_SLICE records a tick. A ceiling rather than an exact
 * count on purpose: the last slice disarms the pacer, so a surplus tick costs
 * nothing and this number does not have to move when an AO is added. */
#define PS_DRAIN_TICKS \
    ((((23U + (unsigned)KBSP_RES_MAX) + 2U) / (unsigned)KAOS_WIRE_SLICE) + 1U)

/** @brief A token for an owner that is not the console. */
static int s_owner;

static void console_up(void)
{
    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();
    owners_up();
    if ( qf_support_claim_ao(AO_Console) )
    {
        Console_ctor();
        qf_support_give_queue(AO_Console);
        AO_Console->prio  = KODE_PRIO_CONSOLE;
        AO_Console->pthre = KODE_PRIO_CONSOLE;
        QActive_register_(AO_Console);
        QASM_INIT(AO_Console, (void *)0, 0U);
        qf_support_background(AO_Console);
    }
    /* Whatever an earlier file left in flight - test_console_parse.c ends
     * with a command deliberately waiting - is timed out here, so every case
     * starts at an idle prompt. */
    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS + 1U);
    fake_klog_reset();
    fake_neighbours_reset();
}

static void type_line(const char *text)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = (uint8_t)KODE_CONSOLE_SRC_SERIAL;
    (void)snprintf(e.line, sizeof(e.line), "%s", text);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

static bool replied(const char *needle)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            return true;
        }
    }
    return false;
}

TEST_CASE("exp with no keys prints fourteen positions and >ok", "[console_exp]")
{
    console_up();
    Mockkhal_gpio_Init();
    Mockkhal_exp_port_Init();
    khal_gpio_release_Ignore();
    (void)khal_exp_init();
    khal_gpio_level_IgnoreAndReturn(false);

    type_line("exp");
    TEST_ASSERT_TRUE(replied(">|exp=1 gpio=8 caps=dml mode=free"));
    TEST_ASSERT_TRUE(replied(">|exp=14 gpio=16 caps=dma mode=free"));
    TEST_ASSERT_TRUE(replied(">ok"));
    Mockkhal_gpio_Destroy();
    Mockkhal_exp_port_Destroy();
}

TEST_CASE("an exp write claims the header for the bench when nobody owns it",
          "[console_exp]")
{
    console_up();
    Mockkhal_gpio_Init();
    Mockkhal_exp_port_Init();
    khal_gpio_release_Ignore();
    (void)khal_exp_init();
    kbsp_release(KBSP_RES_EXP, &s_owner);

    khal_gpio_output_ExpectAndReturn(10U, true, KODE_OK);   /* EXP3 */
    type_line("exp pos=3 mode=out level=1");
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_EQUAL_STRING("console (bench)", kbsp_owner_name(KBSP_RES_EXP));
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
    Mockkhal_exp_port_Destroy();
    /* The console claims with its own address, which kaos.h publishes as
     * AO_Console, so a case can hand the header back and the binary's later
     * files start from a free table. */
    kbsp_release(KBSP_RES_EXP, AO_Console);
}

TEST_CASE("an exp write is refused naming the owner when an AO holds the header",
          "[console_exp]")
{
    console_up();
    Mockkhal_gpio_Init();
    Mockkhal_exp_port_Init();
    khal_gpio_release_Ignore();
    (void)khal_exp_init();
    /* A board where an AO got there first. */
    TEST_ASSERT_TRUE(kbsp_claim(KBSP_RES_EXP, &s_owner, "AO_Tenant"));

    type_line("exp pos=3 mode=in");
    TEST_ASSERT_TRUE(replied(">err"));
    TEST_ASSERT_TRUE(replied("owned by AO_Tenant"));
    Mockkhal_gpio_Verify();   /* no gpio call was made */
    Mockkhal_gpio_Destroy();
    Mockkhal_exp_port_Destroy();
    kbsp_release(KBSP_RES_EXP, &s_owner);
}

/*
 * Printing the table is a read and stays allowed under any owner - but an ADC
 * row is not only a read. It reaches the port, whose per-channel calibration
 * handle belongs to whoever opened the channel, so converting through one a
 * tenant is closing is a use-after-free. The row still prints; what it carries
 * is the same `?` a failed conversion carries.
 */
TEST_CASE("an adc row under another owner prints ? rather than converting",
          "[console_exp]")
{
    console_up();
    Mockkhal_gpio_Init();
    Mockkhal_exp_port_Init();
    khal_gpio_release_Ignore();
    (void)khal_exp_init();
    khal_gpio_level_IgnoreAndReturn(false);
    khal_exp_port_adc_open_ExpectAndReturn(0U, KODE_OK);   /* EXP14 = ch0 */
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_adc(14U));
    /* The tenant arrives after the position was configured, which is the
     * order that matters: the table still says adc. */
    TEST_ASSERT_TRUE(kbsp_claim(KBSP_RES_EXP, &s_owner, "AO_Tenant"));

    type_line("exp");
    TEST_ASSERT_TRUE(
        replied(">|exp=14 gpio=16 caps=dma mode=adc raw=? mv=? cal=?"));
    TEST_ASSERT_TRUE(replied(">ok"));

    /* That the port was never reached is the assertion: CMock fails an
     * unexpected call, and no adc_read was expected above. */
    kbsp_release(KBSP_RES_EXP, &s_owner);
    khal_exp_port_adc_close_Expect(0U);
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_release(14U));
    Mockkhal_exp_port_Verify();
    Mockkhal_gpio_Destroy();
    Mockkhal_exp_port_Destroy();
}

TEST_CASE("exp refuses a mode the position cannot take", "[console_exp]")
{
    console_up();
    Mockkhal_gpio_Init();
    Mockkhal_exp_port_Init();
    khal_gpio_release_Ignore();
    (void)khal_exp_init();

    type_line("exp pos=1 mode=adc");
    TEST_ASSERT_TRUE(replied(">err"));
    Mockkhal_exp_port_Verify();   /* the port was not called */
    Mockkhal_gpio_Destroy();
    Mockkhal_exp_port_Destroy();
    /* A refused write is still a write: the claim was taken before the mode
     * was judged, so this case owes the header back like the second one. */
    kbsp_release(KBSP_RES_EXP, AO_Console);
}

TEST_CASE("a pos with no mode is refused, and nothing is touched",
          "[console_exp]")
{
    console_up();
    Mockkhal_gpio_Init();
    Mockkhal_exp_port_Init();
    khal_gpio_release_Ignore();
    (void)khal_exp_init();

    /* Naming a position is not asking for anything yet - the mode is the
     * verb of this sentence - so the refusal names the key that is missing
     * rather than the one that is present. */
    type_line("exp pos=3");
    TEST_ASSERT_TRUE(replied(">err code=2 msg=mode"));
    Mockkhal_gpio_Verify();
    Mockkhal_exp_port_Verify();
    Mockkhal_gpio_Destroy();
    Mockkhal_exp_port_Destroy();
}

static khal_pwm_cfg_t s_open_cfg;   /**< What the last khal_pwm_open() was asked. */

/**
 * @brief Stand-in for khal_pwm_open(): record the request, hand out channel 0.
 * @param cfg   The request.
 * @param out   The handle written.
 * @param calls How many calls CMock has seen.
 * @return KODE_OK.
 */
static kode_status_t open_stub(const khal_pwm_cfg_t *cfg, khal_pwm_t *out,
                               int calls)
{
    (void)calls;
    s_open_cfg = *cfg;
    *out = 0U;
    return KODE_OK;
}

TEST_CASE("a pwm with no hz or duty runs at the defaults help prints",
          "[console_exp]")
{
    console_up();
    Mockkhal_gpio_Init();
    Mockkhal_exp_port_Init();
    Mockkhal_pwm_Init();
    khal_gpio_release_Ignore();
    (void)khal_exp_init();

    /* The parser fills an absent key with zero rather than with the default
     * it prints, so a handler that read `hz` straight would ask for 0 Hz and
     * be refused. The numbers here are the ones `exp_help` promises. */
    khal_pwm_open_Stub(&open_stub);
    type_line("exp pos=4 mode=pwm");        /* EXP4 is GPIO11 */
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_EQUAL_UINT8(11U, s_open_cfg.gpio);
    TEST_ASSERT_EQUAL_UINT32(1000U, s_open_cfg.hz);
    TEST_ASSERT_EQUAL_UINT16(500U, s_open_cfg.duty_x10);

    /* And `mode=free` hands the LEDC channel back, which is what keeps the
     * next case's khal_exp_init() from having to know about it. */
    khal_pwm_close_Expect(0U);
    fake_klog_reset();
    type_line("exp pos=4 mode=free");
    TEST_ASSERT_TRUE(replied(">ok"));

    Mockkhal_gpio_Verify();
    Mockkhal_exp_port_Verify();
    Mockkhal_pwm_Verify();
    Mockkhal_gpio_Destroy();
    Mockkhal_exp_port_Destroy();
    Mockkhal_pwm_Destroy();
    kbsp_release(KBSP_RES_EXP, AO_Console);
}

TEST_CASE("a mode with no pos is refused rather than printing the table",
          "[console_exp]")
{
    console_up();
    Mockkhal_gpio_Init();
    Mockkhal_exp_port_Init();
    khal_gpio_release_Ignore();
    (void)khal_exp_init();

    /* The mirror of the case above. `exp` with no keys at all is the table,
     * but a key that only means anything with a position is a write that
     * named no pin - and answering it with the table would put `>ok` in
     * front of a script that believes it just drove something. */
    type_line("exp mode=out level=1");
    TEST_ASSERT_TRUE(replied(">err code=2 msg=pos"));
    TEST_ASSERT_FALSE(replied(">ok"));
    TEST_ASSERT_FALSE(replied("gpio="));   /* no table row was printed */
    Mockkhal_gpio_Verify();                /* and no pin was touched */
    Mockkhal_exp_port_Verify();
    Mockkhal_gpio_Destroy();
    Mockkhal_exp_port_Destroy();
}

TEST_CASE("a loan tag with a space reaches the wire as one token",
          "[console_exp]")
{
    static const uint8_t one[1] = { 5U };
    uint8_t gpio[1];

    console_up();
    Mockkhal_gpio_Init();
    Mockkhal_exp_port_Init();
    khal_gpio_release_Ignore();
    (void)khal_exp_init();
    khal_gpio_level_IgnoreAndReturn(false);
    /* A tag is whatever the tenant called itself, and nothing stops it
     * carrying a space or running long. It is text, so it goes out quoted:
     * a space stays inside the one value instead of splitting the record
     * where the next key=value belongs. */
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_lend(one, 1U, "ov 5640 dvp", gpio));

    type_line("exp");
    /* Quoted with its space intact, and clipped to what a record budgets for
     * a tag - the same treatment that `ps` gives it. */
    TEST_ASSERT_TRUE(replied("mode=lent tag=\"ov 5640 \""));
    TEST_ASSERT_FALSE(replied("5640 dvp"));
    TEST_ASSERT_TRUE(replied(">ok"));

    (void)khal_exp_return("ov 5640 dvp");
    Mockkhal_gpio_Destroy();
    Mockkhal_exp_port_Destroy();
}

TEST_CASE("exp_help names every position and the grammar of the keys",
          "[console_exp]")
{
    console_up();
    Mockkhal_gpio_Init();
    Mockkhal_exp_port_Init();
    khal_gpio_release_Ignore();
    (void)khal_exp_init();

    type_line("exp_help");
    /* What a position MAY be is kbsp's capability bits read back out, so a
     * table edit that dropped a channel would show up here rather than on a
     * meter: EXP1 has no ADC, EXP8 has one. */
    TEST_ASSERT_TRUE(replied(">|exp=1 gpio=8 caps=dml may=in,out,pwm"));
    TEST_ASSERT_TRUE(replied(">|exp=8 gpio=21 caps=dma may=in,out,adc,pwm"));
    TEST_ASSERT_TRUE(replied(">|syntax=exp pos=1..14 mode=free|in|out|adc|pwm"));
    TEST_ASSERT_TRUE(replied(">ok"));
    Mockkhal_gpio_Destroy();
    Mockkhal_exp_port_Destroy();
}

TEST_CASE("ps lists only the positions that are not free", "[console_exp]")
{
    static const uint8_t two[2] = { 5U, 6U };
    uint8_t gpio[2];

    console_up();
    Mockkhal_gpio_Init();
    Mockkhal_exp_port_Init();
    khal_gpio_release_Ignore();
    (void)khal_exp_init();
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_lend(two, 2U, "ov5640", gpio));

    /* ONE record for the whole header, not one per position: fourteen `free`
     * entries would bury the two that matter. A free position contributes no
     * entry, so EXP1 is simply absent.
     *
     * `ps` is paced - the whole listing outgrew the large event pool - so the
     * first KAOS_WIRE_SLICE records go out with the command and the rest
     * follow a tick at a time. The header's record is near the end of the
     * listing, so it does not exist until the pacer has run. */
    type_line("ps");
    for ( unsigned i = 0U; PS_DRAIN_TICKS > i; ++i )
    {
        (void)qf_support_tick(AO_Console, 1U);
    }
    TEST_ASSERT_TRUE(replied(">|exp=\"5:lent:ov5640,6:lent:ov5640\""));
    TEST_ASSERT_FALSE(replied(">|exp=\"1:"));
    TEST_ASSERT_TRUE(replied(">ok"));

    /* And with the loan returned the record is gone entirely rather than
     * carrying fourteen `free` entries. */
    (void)khal_exp_return("ov5640");
    fake_klog_reset();
    type_line("ps");
    for ( unsigned i = 0U; PS_DRAIN_TICKS > i; ++i )
    {
        (void)qf_support_tick(AO_Console, 1U);
    }
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_FALSE(replied(">|exp="));
    Mockkhal_gpio_Destroy();
    Mockkhal_exp_port_Destroy();
}

TEST_CASE("exp lists the app's controllers after the positions", "[console_exp]")
{
    uint8_t id = 0xFFU;

    console_up();
    Mockkhal_gpio_Init();
    Mockkhal_exp_port_Init();
    khal_gpio_release_Ignore();
    (void)khal_exp_init();
    khal_gpio_level_IgnoreAndReturn(false);

    type_line("exp");
    TEST_ASSERT_TRUE(replied(">|exp_ctrl bus=i2c id=1 state=free tag=-"));
    TEST_ASSERT_TRUE(replied(">|exp_ctrl bus=spi id=2 state=free tag=-"));
    TEST_ASSERT_TRUE(replied(">|exp_ctrl bus=uart id=4 state=free tag=-"));

    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_ctrl(KBSP_EXP_SPI, "fck", &id));
    fake_klog_reset();
    type_line("exp");
    TEST_ASSERT_TRUE(replied(">|exp_ctrl bus=spi id=2 state=taken tag=\"fck\""));
    TEST_ASSERT_TRUE(replied(">ok"));
    (void)khal_exp_return("fck");
    Mockkhal_gpio_Destroy();
    Mockkhal_exp_port_Destroy();
}

TEST_CASE("exp_help names the controllers kodeOS owns", "[console_exp]")
{
    console_up();
    Mockkhal_gpio_Init();
    Mockkhal_exp_port_Init();
    khal_gpio_release_Ignore();
    (void)khal_exp_init();

    type_line("exp_help");
    TEST_ASSERT_TRUE(replied(">|kodeos_ctrl i2c=0 spi=1 i2s=0 adc=1 "
                             "adc_dma=rfid exp_adc=0"));
    TEST_ASSERT_TRUE(replied(">ok"));
    Mockkhal_gpio_Destroy();
    Mockkhal_exp_port_Destroy();
}
