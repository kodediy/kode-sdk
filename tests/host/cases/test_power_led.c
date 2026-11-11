/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_power_led.c
 * @brief What AO_Power says with the LED.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "qpc.h"
#include "Mockkhal_i2c.h"

#include "ao_spy.h"
#include "config_support.h"
#include "fakes.h"
#include "kaos.h"
#include "kbsp.h"
#include "kdrv_bq25896.h"
#include "kdrv_cw2217.h"
#include "kqp.h"
#include "kset.h"
#include "qf_support.h"

#define PMIC   0U
#define GAUGE  1U

#define BQ_REG0B_STATUS  0x0BU
#define BQ_REG14_ID      0x14U

#define CW_VCELL_H       0x02U
#define CW_CURRENT_H     0x0EU
#define CW_IC_STATE      0xA7U

/** @brief REG0B as the poll reads it: a charge phase, on a good input. */
#define STATUS_CHARGING  0x14U   /**< fast charge, power good              */
#define STATUS_DONE      0x5EU   /**< adapter, termination reached, good   */
#define STATUS_NONE      0x02U   /**< nothing on VBUS, not charging        */

/** @brief One half of the write pulse, in QP ticks. */
#define PULSE_TICKS      QF_SUPPORT_STEPS(KODE_LED_PULSE_MS)
/** @brief How many halves a case watches: two whole seconds of pulse. */
#define PULSE_HALVES     4U

/** @brief The orange the write pulses, the glass's, very low. */
#define ORANGE_R  0x14U
#define ORANGE_G  0x07U
#define ORANGE_B  0x00U

/** @brief The green that says a write ended well. */
#define GREEN_R   0x01U
#define GREEN_G   0x10U
#define GREEN_B   0x05U

/** @brief How long that green stays: POWER_LED_DONE_MS, as a literal so a changed
 *         constant is caught. */
#define DONE_MS   1000U

/* ---- The two chips, as a register file ------------------------------------ */

static uint8_t        s_regs[2][256];
static khal_i2c_dev_t s_devs[2];

/** @brief Which chip a handle belongs to, assigned in order of first use. */
static unsigned chip_of(khal_i2c_dev_t dev)
{
    for ( unsigned i = 0U; 2U > i; ++i )
    {
        if ( s_devs[i] == dev )
        {
            return i;
        }
    }
    for ( unsigned i = 0U; 2U > i; ++i )
    {
        if ( NULL == s_devs[i] )
        {
            s_devs[i] = dev;
            return i;
        }
    }
    return GAUGE;
}

/** @brief The PMIC's reset values, plus an ADC that has converted a 3.904 V cell. */
static void pmic_defaults(void)
{
    static const uint8_t d[0x15] = {
        0x08U, 0x06U, 0x11U, 0x1AU, 0x20U, 0x13U, 0x5EU, 0x9DU, 0x03U, 0x44U,
        0x73U, 0x14U, 0x00U, 0x12U, 0x50U, 0x50U, 0x4CU, 0x98U, 0x00U, 0x00U,
        0x02U,
    };
    (void)memset(s_regs[PMIC], 0, sizeof(s_regs[PMIC]));
    (void)memcpy(s_regs[PMIC], d, sizeof(d));
}

/** @brief A gauge that is awake, reading 3720 mV with nothing through the shunt. */
static void gauge_defaults(void)
{
    (void)memset(s_regs[GAUGE], 0, sizeof(s_regs[GAUGE]));
    s_regs[GAUGE][0x00U]       = 0xA0U;
    s_regs[GAUGE][CW_VCELL_H]  = 0x2EU;
    s_regs[GAUGE][0x03U]       = 0x80U;
    s_regs[GAUGE][CW_IC_STATE] = 0x0CU;
    s_regs[GAUGE][0xABU]       = 0x4AU;
}

/** @brief What the charger reports in REG0B from now on. */
static void charger_says(uint8_t status)
{
    s_regs[PMIC][BQ_REG0B_STATUS] = status;
}

/** @brief What the gauge measures: @p mv on the cell, nothing through the shunt. */
static void cell_reads(uint16_t mv)
{
    uint16_t const code = (uint16_t)(((uint32_t)mv * 10000U) / 3125U);

    s_regs[GAUGE][CW_IC_STATE]       = 0x0CU;
    s_regs[GAUGE][CW_VCELL_H]        = (uint8_t)(code >> 8);
    s_regs[GAUGE][CW_VCELL_H + 1U]   = (uint8_t)(code & 0xFFU);
    s_regs[GAUGE][CW_CURRENT_H]      = 0x00U;
    s_regs[GAUGE][CW_CURRENT_H + 1U] = 0x00U;
}

static kode_status_t on_read(khal_i2c_dev_t dev, uint8_t reg, uint8_t *buf,
                             size_t len, int calls)
{
    (void)calls;
    (void)memcpy(buf, &s_regs[chip_of(dev)][reg], len);
    return KODE_OK;
}

static kode_status_t on_write(khal_i2c_dev_t dev, uint8_t reg, uint8_t val,
                              int calls)
{
    unsigned const chip = chip_of(dev);

    (void)calls;
    /* REG_RST: everything back to its defaults, and the bit clears itself,
     * the way the silicon does. Nothing here asks for one; it is honoured so
     * that a construction taking place in THIS file behaves as it would in
     * test_power_ao.c. */
    if ( (PMIC == chip) && (BQ_REG14_ID == reg) && ((val & 0x80U) != 0U) )
    {
        pmic_defaults();
        return KODE_OK;
    }
    s_regs[chip][reg] = val;
    return KODE_OK;
}

/* ---- The words the LED was given ------------------------------------------ */

/** @brief How many LED words one case may keep. A few pulses, and slack. */
#define WORDS_MAX  16U

/** @brief The three channels of one published LED_RGB_SIG, by index. */
#define R  0U
#define G  1U
#define B  2U

static uint8_t  s_words[WORDS_MAX][3];
static unsigned s_nwords;

/** @brief Forget what has been said so far. */
static void words_forget(void)
{
    s_nwords = 0U;
}

/**
 * @brief Take everything the spy holds and keep the LED words.
 *
 * Called after every dispatch, and that is not tidiness: the spy's queue is
 * 32 deep and QP answers a full one with an assertion. Other signals are
 * dropped - the spy is shared with
 * test_power_ao.c, which subscribes it to the reading this poll also
 * publishes.
 *
 * AO_LedMgr is drained here for a harder reason. It is the real subscriber to
 * LED_RGB_SIG and the two led files bring it up, so every word this file
 * provokes lands in its queue as well - and a queue nobody drains holds the
 * pool block behind each event for the rest of the binary. Thirty-two words
 * empty the harness's pool, after which Q_NEW_X answers NULL everywhere and
 * the symptom is an unrelated file's request going unanswered.
 */
static void ledmgr_drain(void)
{
    const QEvt *e = NULL;

    while ( (NULL != AO_LedMgr->eQueue)
           && (xQueueReceive(AO_LedMgr->eQueue, &e, 0U) == pdTRUE) )
    {
        QF_gc(e);
    }
}

static void words_collect(void)
{
    ledmgr_drain();
    while ( ao_spy_count() > 0U )
    {
        const QEvt * const e = ao_spy_take();
        if ( (NULL != e) && ((QSignal)LED_RGB_SIG == e->sig)
            && (WORDS_MAX > s_nwords) )
        {
            const LedRgbEvt * const w = (const LedRgbEvt *)e;
            s_words[s_nwords][R] = w->r;
            s_words[s_nwords][G] = w->g;
            s_words[s_nwords][B] = w->b;
            ++s_nwords;
        }
    }
    ao_spy_drain();
}

/** @brief Insist the only word said was @p r, @p g, @p b. */
static void one_word(uint8_t r, uint8_t g, uint8_t b, const char *what)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_nwords, what);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(r, s_words[0][R], what);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(g, s_words[0][G], what);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(b, s_words[0][B], what);
}

/* ---- Bringing AO_Power up -------------------------------------------------- */

/**
 * @brief Bring AO_Power up over this file's register file.
 *
 * qf_support_claim_ao() is what keeps this file and test_power_ao.c from
 * registering one Active Object twice, whichever of them the linker puts
 * first: constructing it twice is worse than it looks.
 */
static void power_led_up(void)
{
    Mockkhal_i2c_Init();
    khal_i2c_init_IgnoreAndReturn(KODE_OK);
    khal_i2c_probe_IgnoreAndReturn(true);
    khal_i2c_add_IgnoreAndReturn(KODE_OK);
    khal_i2c_read_reg_StubWithCallback(&on_read);
    khal_i2c_write_reg_StubWithCallback(&on_write);

    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();
    ao_spy_subscribe(LED_RGB_SIG);

    pmic_defaults();
    gauge_defaults();

    if ( qf_support_claim_ao(AO_Power) )
    {
        Power_ctor();
        qf_support_give_queue(AO_Power);
        AO_Power->prio  = KODE_PRIO_POWER;
        AO_Power->pthre = KODE_PRIO_POWER;
        QActive_register_(AO_Power);
        QASM_INIT(AO_Power, (void *)0, 0U);
    }
    qf_support_give_queue(AO_Power);
    /* The poll timer is armed for the rest of the run; say so, or a later
     * file's ticks fill a queue nobody drains. Harmless to repeat. */
    qf_support_background(AO_Power);

    /* Whatever an earlier case left in the mailbox is not this one's. */
    {
        const QEvt *stray;
        while ( xQueueReceive(AO_Power->eQueue, &stray, 0U) == pdTRUE )
        {
            QF_gc(stray);
        }
    }
    ao_spy_drain();
    words_forget();
}

/* ---- Driving it ------------------------------------------------------------ */

/** @brief One poll, and whatever it said. */
static void poll_once(void)
{
    static const QEvt poll = QEVT_INITIALIZER(POWER_POLL_SIG);

    QASM_DISPATCH(AO_Power, &poll, 0U);
    words_collect();
}

/** @brief Poll until the slew-limited percentage has settled wherever it goes. */
static void soc_settle(void)
{
    for ( unsigned i = 0U; 80U > i; ++i )
    {
        poll_once();
    }
}

/** @brief One report from AO_AppMgr's writer. */
static void progress(uint8_t percent)
{
    AppProgressEvt e;

    e.super   = (QEvt)QEVT_INITIALIZER(APP_FLASH_PROGRESS_SIG);
    e.percent = percent;
    e.what    = (uint8_t)KWRITE_APP;
    (void)snprintf(e.file, sizeof(e.file), "doom.bin");
    QASM_DISPATCH(AO_Power, &e.super, 0U);
    words_collect();
}

/**
 * @brief The write ended, one way or the other.
 *
 * Built as the done travels, an AppFlashDoneEvt naming what was installed. A
 * failure is a bare QEvt, which is that struct's first member, so the same
 * object serves both: AO_Power reads the signal alone.
 */
static void flash_ends(QSignal sig)
{
    AppFlashDoneEvt e = { QEVT_INITIALIZER(APP_FLASH_DONE_SIG), "doom" };

    e.super.sig = sig;
    QASM_DISPATCH(AO_Power, &e.super, 0U);
    words_collect();
}

/** @brief Let @p n halves of the pulse pass, collecting what each one said. */
static void half_pulses(unsigned n)
{
    for ( unsigned i = 0U; i < n; ++i )
    {
        (void)qf_support_tick(AO_Power, PULSE_TICKS);
        words_collect();
    }
}

/** @brief Let @p ms of QP time pass without singling out a half of the pulse. */
static void quiet_ms(uint32_t ms)
{
    (void)qf_support_tick(AO_Power, (unsigned)QF_SUPPORT_STEPS(ms));
    words_collect();
}

/**
 * @brief End any write, whatever the case before this one left.
 *
 * No write, no cable, a cell comfortably charged: the state every case starts
 * from. A write that was still going ends in its green, which is let run out
 * here so its dark word does not land in the case. The percentage is
 * slew-limited, so it is polled until it has climbed back from whatever an
 * earlier case left.
 */
static void led_quiet(void)
{
    flash_ends(APP_FLASH_DONE_SIG);
    quiet_ms(DONE_MS + KODE_LED_PULSE_MS);
    charger_says(STATUS_NONE);
    cell_reads(3900U);
    soc_settle();
    words_forget();
}

/**
 * @brief Leave AO_Power where the next file would want to find it.
 *
 * There is one AO_Power in the binary and the write is a field of it, so a
 * case that ended mid-pulse leaves a timer publishing into queues other files
 * do not drain. No write, every time, whichever way the case ended.
 */
static void power_led_down(void)
{
    const QEvt *stray;

    led_quiet();
    while ( xQueueReceive(AO_Power->eQueue, &stray, 0U) == pdTRUE )
    {
        QF_gc(stray);
    }
    ledmgr_drain();
    ao_spy_drain();
}

/* ---- The cases -------------------------------------------------------------- */

/* The battery has no colour: charging, a full pack and a nearly empty one all
 * leave the LED alone, on the poll that sees them and for as long as they
 * last, so a colour an app or the console set is never overwritten by the
 * charger's state. */
TEST_CASE("the battery gives the LED no colour", "[power_led]")
{
    power_led_up();
    led_quiet();

    charger_says(STATUS_CHARGING);
    cell_reads(3800U);
    poll_once();
    quiet_ms(2000U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nwords, "charging lit the LED");

    charger_says(STATUS_DONE);
    poll_once();
    quiet_ms(2000U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nwords, "a full pack lit the LED");

    charger_says(STATUS_NONE);
    cell_reads(3300U);        /* the bottom of the cell's curve */
    soc_settle();             /* the percentage moves 2 % a reading */
    quiet_ms(2000U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nwords,
                                   "a nearly empty pack lit the LED");

    power_led_down();
    Mockkhal_i2c_Destroy();
}

/* A flash write is what makes a reset dangerous, so the LED says so: a very low
 * orange, half a second lit and half a second dark, from the first report until
 * the write ends - then a second of green, and dark, with no timer left behind. */
TEST_CASE("writing pulses orange, ends in a second of green and then dark", "[power_led]")
{
    power_led_up();
    led_quiet();

    progress(0U);
    one_word(ORANGE_R, ORANGE_G, ORANGE_B, "a write starts with the orange lit");

    words_forget();
    progress(1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nwords,
        "a report in the middle of a write restarted the pulse");

    half_pulses(PULSE_HALVES);
    TEST_ASSERT_EQUAL_UINT(PULSE_HALVES, s_nwords);
    for ( unsigned i = 0U; i < s_nwords; ++i )
    {
        uint8_t r = 0U;
        if ( (i % 2U) == 1U )
        {
            r = ORANGE_R;     /* dark, lit, dark, lit */
        }
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(r, s_words[i][R],
                                        "the pulse is not half lit, half dark");
    }

    words_forget();
    flash_ends(APP_FLASH_DONE_SIG);
    /* The pulse stops, which says dark, and the green follows in the same step. */
    TEST_ASSERT_EQUAL_UINT(2U, s_nwords);
    TEST_ASSERT_EQUAL_UINT8(0U, s_words[0][R]);
    TEST_ASSERT_EQUAL_UINT8(0U, s_words[0][G]);
    TEST_ASSERT_EQUAL_UINT8(0U, s_words[0][B]);
    TEST_ASSERT_EQUAL_UINT8(GREEN_R, s_words[1][R]);
    TEST_ASSERT_EQUAL_UINT8(GREEN_G, s_words[1][G]);
    TEST_ASSERT_EQUAL_UINT8(GREEN_B, s_words[1][B]);

    words_forget();
    quiet_ms(DONE_MS - KODE_LED_PULSE_MS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nwords, "the green went before its second");

    quiet_ms(2U * KODE_LED_PULSE_MS);
    one_word(0U, 0U, 0U, "the green's second over leaves the LED dark");

    words_forget();
    quiet_ms(2000U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nwords,
        "the pulse kept going after the write ended");

    power_led_down();
    Mockkhal_i2c_Destroy();
}

/* The green is for a write that ended well, and only for that. */
TEST_CASE("a write that failed goes dark with no green", "[power_led]")
{
    power_led_up();
    led_quiet();

    progress(0U);
    words_forget();
    flash_ends(APP_FLASH_FAIL_SIG);
    one_word(0U, 0U, 0U, "a write that failed leaves the LED dark");

    words_forget();
    quiet_ms(2000U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nwords,
        "something went on saying after the write failed");

    power_led_down();
    Mockkhal_i2c_Destroy();
}

/* A done with no write behind it is no good news: an install the LED never
 * showed, or led_activity off. */
TEST_CASE("a done with no write in flight says nothing", "[power_led]")
{
    power_led_up();
    led_quiet();

    flash_ends(APP_FLASH_DONE_SIG);
    quiet_ms(2000U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nwords, "a done with no write lit the LED");

    power_led_down();
    Mockkhal_i2c_Destroy();
}

/* The next write takes the LED off the green at once, and the green's end does
 * not reach into its pulse. */
TEST_CASE("a write that starts during the green pulses from its first report", "[power_led]")
{
    power_led_up();
    led_quiet();

    progress(0U);
    flash_ends(APP_FLASH_DONE_SIG);
    words_forget();

    progress(0U);
    one_word(ORANGE_R, ORANGE_G, ORANGE_B, "the new write did not take the LED");

    words_forget();
    half_pulses(PULSE_HALVES);
    TEST_ASSERT_EQUAL_UINT(PULSE_HALVES, s_nwords);
    for ( unsigned i = 0U; i < s_nwords; ++i )
    {
        uint8_t r = 0U;
        if ( (i % 2U) == 1U )
        {
            r = ORANGE_R;     /* dark, lit, dark, lit */
        }
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(r, s_words[i][R],
                                        "the green's end broke the pulse");
    }

    power_led_down();
    Mockkhal_i2c_Destroy();
}

/* A host that dies mid-transfer stops reporting, and nothing ever arrives to
 * say the write ended. Three seconds without a report is the guard: the poll
 * takes the LED back rather than leaving it pulsing until the next boot. */
TEST_CASE("a write that stops reporting gives the LED back after three seconds",
          "[power_led]")
{
    power_led_up();
    led_quiet();
    words_forget();

    progress(50U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_nwords, "the write said nothing");

    words_forget();
    fake_time_advance_ms(1000U);
    poll_once();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nwords,
        "a second between chunks is a slow host, not a dead one");

    fake_time_advance_ms(2100U);
    poll_once();
    one_word(0U, 0U, 0U, "a write nobody is reporting on still holds the LED");

    power_led_down();
    Mockkhal_i2c_Destroy();
}

/* `led_activity=off`: a write says nothing with the LED, and one already
 * pulsing goes dark at once rather than at its end. */
TEST_CASE("led_activity off keeps a write dark, a pulse in flight included",
          "[power_led]")
{
    const KodeConfigAnsEvt *a;

    fake_nvs_reset();
    power_led_up();
    led_quiet();

    progress(0U);
    one_word(ORANGE_R, ORANGE_G, ORANGE_B, "a write starts with the orange lit");

    a = config_ask(AO_Power, &(config_ask_t){ .key = KSET_KEY_LED_ACT, .set = true });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("off", a->value);
    TEST_ASSERT_TRUE(a->stored);
    words_forget();
    ao_spy_drain();

    progress(10U);
    half_pulses(PULSE_HALVES);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nwords,
        "the pulse went on with led_activity off");

    a = config_ask(AO_Power, &(config_ask_t){ .key = KSET_KEY_LED_ACT, .set = true,
                                              .b = true });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("on", a->value);
    fake_nvs_reset();
    power_led_down();
    Mockkhal_i2c_Destroy();
}

/* ---- One time event waits at a time ---------------------------------------- */

/* QP posts a time event with no margin, and a queue that is full is an
 * assertion and a reset. A pulse that is periodic goes on posting whether or
 * not the AO took the last half, so one that did not run for a few halves
 * finds that many waiting. One half waits at most, and the half that is taken
 * arms the next. The two counts of what waits fail on the periodic timer; the
 * word the dark half says and the stale half's silence are guards. */
TEST_CASE("a half of the pulse nobody took is not followed by another; the one taken arms the next",
          "[power_led]")
{
    static const QEvt pulse = QEVT_INITIALIZER(POWER_PULSE_SIG);

    power_led_up();
    led_quiet();
    (void)qf_support_waiting_of(AO_Power, POWER_PULSE_SIG);

    progress(0U);
    one_word(ORANGE_R, ORANGE_G, ORANGE_B, "a write starts with the orange lit");

    /* Three halves go by and AO_Power never runs. */
    qf_support_time_passes(3U * KQP_TICKS_PER_MS(KODE_LED_PULSE_MS));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, qf_support_waiting_of(AO_Power, POWER_PULSE_SIG),
                                   "every half queued a pulse the AO had not taken");

    words_forget();
    QASM_DISPATCH(AO_Power, &pulse, 0U);
    words_collect();
    one_word(0U, 0U, 0U, "the half that was taken is the dark one");

    qf_support_time_passes(KQP_TICKS_PER_MS(KODE_LED_PULSE_MS));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, qf_support_waiting_of(AO_Power, POWER_PULSE_SIG),
                                   "one half after the one that was taken, the next");

    /* The write fails, and no pulse follows it: a half queued before the end
     * is taken after it, says nothing and arms nothing. */
    (void)qf_support_waiting_of(AO_Power, POWER_PULSE_SIG);
    flash_ends(APP_FLASH_FAIL_SIG);
    words_forget();
    QASM_DISPATCH(AO_Power, &pulse, 0U);
    words_collect();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nwords,
                                   "a half queued before the write ended lit the LED after it");
    qf_support_time_passes(2U * KQP_TICKS_PER_MS(KODE_LED_PULSE_MS));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_waiting_of(AO_Power, POWER_PULSE_SIG),
                                   "a pulse followed a write that had ended");

    power_led_down();
    Mockkhal_i2c_Destroy();
}

/* The same for a write that ends well: a half queued before the end must
 * neither cut the green short nor light anything after it. Only the stale
 * half's silence fails on the code that took it for the green's end. */
TEST_CASE("a half of the pulse queued before a write ended leaves the green its second",
          "[power_led]")
{
    static const QEvt pulse = QEVT_INITIALIZER(POWER_PULSE_SIG);

    power_led_up();
    led_quiet();
    (void)qf_support_waiting_of(AO_Power, POWER_PULSE_SIG);

    progress(0U);
    qf_support_time_passes(KQP_TICKS_PER_MS(KODE_LED_PULSE_MS));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, qf_support_waiting_of(AO_Power, POWER_PULSE_SIG),
                                   "a half is queued");

    words_forget();
    flash_ends(APP_FLASH_DONE_SIG);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_nwords, "the pulse stops dark and the green follows");

    words_forget();
    QASM_DISPATCH(AO_Power, &pulse, 0U);
    words_collect();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nwords,
                                   "a half queued before the end cut the green short");

    quiet_ms(DONE_MS - KODE_LED_PULSE_MS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nwords, "the green went before its second");
    quiet_ms(2U * KODE_LED_PULSE_MS);
    one_word(0U, 0U, 0U, "the green's second over leaves the LED dark");

    words_forget();
    quiet_ms(2000U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nwords, "something lit after the green");

    power_led_down();
    Mockkhal_i2c_Destroy();
}
