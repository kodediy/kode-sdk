/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_khal_pwm.c
 * @brief khal_pwm's table over a mocked LEDC: which channel and which timer each call touches.
 */
#include <stdint.h>

#include "unity.h"

#include "Mockkhal_pwm_port.h"
#include "khal_pwm.h"

extern int32_t g_fake_mutex_depth;

/** @brief Every channel and timer free, and a mock with no expectation left. */
static void pwm_fresh(void)
{
    Mockkhal_pwm_port_Init();
    TEST_ASSERT_EQUAL(KODE_OK, khal_pwm_init());
    /* The table is static across the binary: close whatever the case before left. */
    khal_pwm_port_chan_stop_Ignore();
    khal_pwm_port_timer_stop_Ignore();
    for ( uint8_t ch = 0U; KHAL_PWM_CHANNELS > ch; ++ch )
    {
        khal_pwm_close(ch);
    }
    khal_pwm_port_chan_stop_StopIgnore();
    khal_pwm_port_timer_stop_StopIgnore();
}

/** @brief The mock saw every call it expected, and every lock was given back. */
static void pwm_done(void)
{
    TEST_ASSERT_EQUAL_INT32(0, g_fake_mutex_depth);
    Mockkhal_pwm_port_Verify();
    Mockkhal_pwm_port_Destroy();
}

/**
 * @brief Open a channel from loose arguments.
 * @param gpio The pin.
 * @param hz   The frequency.
 * @param bits The resolution, or KHAL_PWM_BITS_WIDEST.
 * @param duty Tenths of a percent.
 * @param own  Whether it asks for a timer of its own.
 * @param h    The handle written.
 * @return What khal_pwm_open() answered.
 */
static kode_status_t open_ch(uint8_t gpio, uint32_t hz, uint8_t bits,
                             uint16_t duty, bool own, khal_pwm_t *h)
{
    const khal_pwm_cfg_t cfg = {
        .gpio      = gpio,
        .hz        = hz,
        .bits      = bits,
        .duty_x10  = duty,
        .own_timer = own,
    };
    return khal_pwm_open(&cfg, h);
}

TEST_CASE("open takes the lowest channel and a free timer at the widest bits",
          "[khal_pwm]")
{
    khal_pwm_t h = KHAL_PWM_NONE;

    pwm_fresh();
    khal_pwm_port_timer_set_ExpectAndReturn(0U, 1000U, 16U, KODE_OK);
    khal_pwm_port_chan_start_ExpectAndReturn(0U, 8U, 0U, 16384U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK,
                      open_ch(8U, 1000U, KHAL_PWM_BITS_WIDEST, 250U, false, &h));
    TEST_ASSERT_EQUAL_UINT8(0U, h);
    pwm_done();
}

TEST_CASE("a second channel at the same hz and bits shares the timer",
          "[khal_pwm]")
{
    khal_pwm_t a = KHAL_PWM_NONE;
    khal_pwm_t b = KHAL_PWM_NONE;

    pwm_fresh();
    khal_pwm_port_timer_set_ExpectAndReturn(0U, 1000U, 16U, KODE_OK);
    khal_pwm_port_chan_start_ExpectAndReturn(0U, 8U, 0U, 16384U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK,
                      open_ch(8U, 1000U, KHAL_PWM_BITS_WIDEST, 250U, false, &a));
    /* No timer_set: the timer already runs at 1000 Hz and 16 bits. */
    khal_pwm_port_chan_start_ExpectAndReturn(1U, 9U, 0U, 32768U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK,
                      open_ch(9U, 1000U, KHAL_PWM_BITS_WIDEST, 500U, false, &b));
    TEST_ASSERT_EQUAL_UINT8(1U, b);
    pwm_done();
}

TEST_CASE("a second channel at another hz gets a timer of its own",
          "[khal_pwm]")
{
    khal_pwm_t a = KHAL_PWM_NONE;
    khal_pwm_t b = KHAL_PWM_NONE;

    pwm_fresh();
    khal_pwm_port_timer_set_ExpectAndReturn(0U, 1000U, 16U, KODE_OK);
    khal_pwm_port_chan_start_ExpectAndReturn(0U, 8U, 0U, 16384U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK,
                      open_ch(8U, 1000U, KHAL_PWM_BITS_WIDEST, 250U, false, &a));
    khal_pwm_port_timer_set_ExpectAndReturn(1U, 2000U, 15U, KODE_OK);
    khal_pwm_port_chan_start_ExpectAndReturn(1U, 9U, 1U, 16384U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK,
                      open_ch(9U, 2000U, KHAL_PWM_BITS_WIDEST, 500U, false, &b));
    pwm_done();
}

TEST_CASE("the same hz at other bits does not share", "[khal_pwm]")
{
    khal_pwm_t a = KHAL_PWM_NONE;
    khal_pwm_t b = KHAL_PWM_NONE;

    pwm_fresh();
    khal_pwm_port_timer_set_ExpectAndReturn(0U, 125000U, 8U, KODE_OK);
    khal_pwm_port_chan_start_ExpectAndReturn(0U, 8U, 0U, 128U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK, open_ch(8U, 125000U, 8U, 500U, false, &a));
    khal_pwm_port_timer_set_ExpectAndReturn(1U, 125000U, 9U, KODE_OK);
    khal_pwm_port_chan_start_ExpectAndReturn(1U, 9U, 1U, 256U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK,
                      open_ch(9U, 125000U, KHAL_PWM_BITS_WIDEST, 500U, false, &b));
    pwm_done();
}

TEST_CASE("own_timer is never shared and never joins", "[khal_pwm]")
{
    khal_pwm_t own = KHAL_PWM_NONE;
    khal_pwm_t shared = KHAL_PWM_NONE;
    khal_pwm_t own2 = KHAL_PWM_NONE;

    pwm_fresh();
    khal_pwm_port_timer_set_ExpectAndReturn(0U, 125000U, 8U, KODE_OK);
    khal_pwm_port_chan_start_ExpectAndReturn(0U, 8U, 0U, 128U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK, open_ch(8U, 125000U, 8U, 500U, true, &own));
    /* Same hz and bits, but timer 0 is somebody's own. */
    khal_pwm_port_timer_set_ExpectAndReturn(1U, 125000U, 8U, KODE_OK);
    khal_pwm_port_chan_start_ExpectAndReturn(1U, 9U, 1U, 128U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK, open_ch(9U, 125000U, 8U, 500U, false, &shared));
    /* And an own_timer request does not join the shared one. */
    khal_pwm_port_timer_set_ExpectAndReturn(2U, 125000U, 8U, KODE_OK);
    khal_pwm_port_chan_start_ExpectAndReturn(2U, 10U, 2U, 128U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK, open_ch(10U, 125000U, 8U, 500U, true, &own2));
    pwm_done();
}

TEST_CASE("the ninth channel and the fifth timer are no resources",
          "[khal_pwm]")
{
    static const uint32_t HZ[4] = { 1000U, 2000U, 3000U, 4000U };
    static const uint8_t BITS[4] = { 16U, 15U, 14U, 14U };
    khal_pwm_t h = KHAL_PWM_NONE;

    pwm_fresh();
    for ( uint8_t t = 0U; 4U > t; ++t )
    {
        khal_pwm_port_timer_set_ExpectAndReturn(t, HZ[t], BITS[t], KODE_OK);
        khal_pwm_port_chan_start_ExpectAndReturn(t, 8U, t, 0U, KODE_OK);
        TEST_ASSERT_EQUAL(KODE_OK,
                          open_ch(8U, HZ[t], KHAL_PWM_BITS_WIDEST, 0U, false, &h));
    }
    /* A fifth frequency: no port call at all. */
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES,
                      open_ch(8U, 5000U, KHAL_PWM_BITS_WIDEST, 0U, false, &h));
    TEST_ASSERT_EQUAL_UINT8(KHAL_PWM_NONE, h);
    pwm_done();

    pwm_fresh();
    khal_pwm_port_timer_set_ExpectAndReturn(0U, 1000U, 16U, KODE_OK);
    for ( uint8_t ch = 0U; KHAL_PWM_CHANNELS > ch; ++ch )
    {
        khal_pwm_port_chan_start_ExpectAndReturn(ch, 8U, 0U, 0U, KODE_OK);
        TEST_ASSERT_EQUAL(KODE_OK,
                          open_ch(8U, 1000U, KHAL_PWM_BITS_WIDEST, 0U, false, &h));
    }
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES,
                      open_ch(8U, 1000U, KHAL_PWM_BITS_WIDEST, 0U, false, &h));
    TEST_ASSERT_EQUAL_UINT8(KHAL_PWM_NONE, h);
    pwm_done();
}

TEST_CASE("a refused channel stops the fresh timer and changes nothing",
          "[khal_pwm]")
{
    khal_pwm_t h = KHAL_PWM_NONE;

    pwm_fresh();
    khal_pwm_port_timer_set_ExpectAndReturn(0U, 1000U, 16U, KODE_OK);
    khal_pwm_port_chan_start_ExpectAndReturn(0U, 8U, 0U, 16384U, KODE_ERROR);
    khal_pwm_port_timer_stop_Expect(0U);
    TEST_ASSERT_EQUAL(KODE_ERROR,
                      open_ch(8U, 1000U, KHAL_PWM_BITS_WIDEST, 250U, false, &h));
    TEST_ASSERT_EQUAL_UINT8(KHAL_PWM_NONE, h);
    /* The same channel and the same timer, set up again from nothing. */
    khal_pwm_port_timer_set_ExpectAndReturn(0U, 1000U, 16U, KODE_OK);
    khal_pwm_port_chan_start_ExpectAndReturn(0U, 8U, 0U, 16384U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK,
                      open_ch(8U, 1000U, KHAL_PWM_BITS_WIDEST, 250U, false, &h));
    TEST_ASSERT_EQUAL_UINT8(0U, h);
    pwm_done();
}

TEST_CASE("bad arguments are denied before the port", "[khal_pwm]")
{
    const khal_pwm_cfg_t good = {
        .gpio = 8U, .hz = 1000U, .bits = KHAL_PWM_BITS_WIDEST,
        .duty_x10 = 250U, .own_timer = false,
    };
    khal_pwm_t h = 0U;

    pwm_fresh();
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_pwm_open(NULL, &h));
    TEST_ASSERT_EQUAL_UINT8(KHAL_PWM_NONE, h);
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_pwm_open(&good, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED,
                      open_ch(8U, 0U, KHAL_PWM_BITS_WIDEST, 250U, false, &h));
    TEST_ASSERT_EQUAL(KODE_DENIED,
                      open_ch(8U, 1000U, KHAL_PWM_BITS_WIDEST, 1001U, false, &h));
    TEST_ASSERT_EQUAL(KODE_DENIED, open_ch(8U, 1U, 21U, 250U, false, &h));
    /* 80000 << 11 is past the 80 MHz clock. */
    TEST_ASSERT_EQUAL(KODE_DENIED, open_ch(8U, 80000U, 11U, 250U, false, &h));
    pwm_done();
}

TEST_CASE("set at the same hz changes the duty alone", "[khal_pwm]")
{
    khal_pwm_t h = KHAL_PWM_NONE;

    pwm_fresh();
    khal_pwm_port_timer_set_ExpectAndReturn(0U, 1000U, 16U, KODE_OK);
    khal_pwm_port_chan_start_ExpectAndReturn(0U, 8U, 0U, 16384U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK,
                      open_ch(8U, 1000U, KHAL_PWM_BITS_WIDEST, 250U, false, &h));
    khal_pwm_port_chan_duty_ExpectAndReturn(0U, 32768U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK, khal_pwm_set(h, 1000U, 500U));
    pwm_done();
}

TEST_CASE("set at a new hz retunes in place when the channel is alone",
          "[khal_pwm]")
{
    khal_pwm_t h = KHAL_PWM_NONE;

    pwm_fresh();
    khal_pwm_port_timer_set_ExpectAndReturn(0U, 1000U, 16U, KODE_OK);
    khal_pwm_port_chan_start_ExpectAndReturn(0U, 8U, 0U, 16384U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK,
                      open_ch(8U, 1000U, KHAL_PWM_BITS_WIDEST, 250U, false, &h));
    khal_pwm_port_timer_set_ExpectAndReturn(0U, 3000U, 14U, KODE_OK);
    khal_pwm_port_chan_duty_ExpectAndReturn(0U, 8192U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK, khal_pwm_set(h, 3000U, 500U));
    pwm_done();
}

TEST_CASE("set at a new hz moves off a shared timer and leaves the other user",
          "[khal_pwm]")
{
    khal_pwm_t a = KHAL_PWM_NONE;
    khal_pwm_t b = KHAL_PWM_NONE;

    pwm_fresh();
    khal_pwm_port_timer_set_ExpectAndReturn(0U, 1000U, 16U, KODE_OK);
    khal_pwm_port_chan_start_ExpectAndReturn(0U, 8U, 0U, 16384U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK,
                      open_ch(8U, 1000U, KHAL_PWM_BITS_WIDEST, 250U, false, &a));
    khal_pwm_port_chan_start_ExpectAndReturn(1U, 9U, 0U, 16384U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK,
                      open_ch(9U, 1000U, KHAL_PWM_BITS_WIDEST, 250U, false, &b));
    /* Nothing for A, and timer 0 keeps running for it. */
    khal_pwm_port_timer_set_ExpectAndReturn(1U, 2000U, 15U, KODE_OK);
    khal_pwm_port_chan_bind_ExpectAndReturn(1U, 1U, 16384U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK, khal_pwm_set(b, 2000U, 500U));
    /* A is still timer 0's user: its close is the one that stops it. */
    khal_pwm_port_chan_stop_Expect(0U, 8U);
    khal_pwm_port_timer_stop_Expect(0U);
    khal_pwm_close(a);
    /* B left with its own pin, and takes timer 1 down with it. */
    khal_pwm_port_chan_stop_Expect(1U, 9U);
    khal_pwm_port_timer_stop_Expect(1U);
    khal_pwm_close(b);
    pwm_done();
}

TEST_CASE("set joins a timer already at the new hz and frees the old one",
          "[khal_pwm]")
{
    khal_pwm_t a = KHAL_PWM_NONE;
    khal_pwm_t b = KHAL_PWM_NONE;

    pwm_fresh();
    khal_pwm_port_timer_set_ExpectAndReturn(0U, 1000U, 16U, KODE_OK);
    khal_pwm_port_chan_start_ExpectAndReturn(0U, 8U, 0U, 16384U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK,
                      open_ch(8U, 1000U, KHAL_PWM_BITS_WIDEST, 250U, false, &a));
    khal_pwm_port_timer_set_ExpectAndReturn(1U, 2000U, 15U, KODE_OK);
    khal_pwm_port_chan_start_ExpectAndReturn(1U, 9U, 1U, 16384U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK,
                      open_ch(9U, 2000U, KHAL_PWM_BITS_WIDEST, 500U, false, &b));
    khal_pwm_port_chan_bind_ExpectAndReturn(0U, 1U, 16384U, KODE_OK);
    khal_pwm_port_timer_stop_Expect(0U);
    TEST_ASSERT_EQUAL(KODE_OK, khal_pwm_set(a, 2000U, 500U));
    pwm_done();
}

TEST_CASE("set that finds no timer leaves the channel running as it was",
          "[khal_pwm]")
{
    static const uint32_t HZ[3] = { 2000U, 3000U, 4000U };
    static const uint8_t BITS[3] = { 15U, 14U, 14U };
    khal_pwm_t a = KHAL_PWM_NONE;
    khal_pwm_t b = KHAL_PWM_NONE;
    khal_pwm_t h = KHAL_PWM_NONE;

    pwm_fresh();
    khal_pwm_port_timer_set_ExpectAndReturn(0U, 1000U, 16U, KODE_OK);
    khal_pwm_port_chan_start_ExpectAndReturn(0U, 8U, 0U, 16384U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK,
                      open_ch(8U, 1000U, KHAL_PWM_BITS_WIDEST, 250U, false, &a));
    khal_pwm_port_chan_start_ExpectAndReturn(1U, 9U, 0U, 16384U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK,
                      open_ch(9U, 1000U, KHAL_PWM_BITS_WIDEST, 250U, false, &b));
    for ( uint8_t t = 1U; 4U > t; ++t )
    {
        khal_pwm_port_timer_set_ExpectAndReturn(t, HZ[t - 1U], BITS[t - 1U], KODE_OK);
        khal_pwm_port_chan_start_ExpectAndReturn((uint8_t)(t + 1U), 10U, t, 0U, KODE_OK);
        TEST_ASSERT_EQUAL(KODE_OK, open_ch(10U, HZ[t - 1U], KHAL_PWM_BITS_WIDEST,
                                           0U, false, &h));
    }
    /* Timer 0 is shared, the other three are busy: no port call at all. */
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, khal_pwm_set(b, 5000U, 500U));
    /* B is still on timer 0 at 1000 Hz: the same hz is a duty change alone. */
    khal_pwm_port_chan_duty_ExpectAndReturn(1U, 32768U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK, khal_pwm_set(b, 1000U, 500U));
    pwm_done();
}

TEST_CASE("set keeps explicit bits and refuses an hz they cannot reach",
          "[khal_pwm]")
{
    khal_pwm_t h = KHAL_PWM_NONE;

    pwm_fresh();
    khal_pwm_port_timer_set_ExpectAndReturn(0U, 125000U, 8U, KODE_OK);
    khal_pwm_port_chan_start_ExpectAndReturn(0U, 8U, 0U, 128U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK, open_ch(8U, 125000U, 8U, 500U, true, &h));
    /* 400000 << 8 is past the 80 MHz clock. */
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_pwm_set(h, 400000U, 500U));
    /* And a reachable one stays at 8 bits rather than widening. */
    khal_pwm_port_timer_set_ExpectAndReturn(0U, 100000U, 8U, KODE_OK);
    khal_pwm_port_chan_duty_ExpectAndReturn(0U, 128U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK, khal_pwm_set(h, 100000U, 500U));
    pwm_done();
}

static uint32_t s_pause_calls;     /**< Pauses and resumes the stubs saw. */
static int32_t  s_depth_inside;    /**< The mutex depth inside the last of them. */
static uint8_t  s_timer_seen;      /**< The timer the last of them was given. */

/**
 * @brief Stand-in for the port's pause and resume: record, never refuse.
 * @param timer The timer asked for.
 * @param calls How many calls CMock has seen.
 * @return KODE_OK.
 */
static kode_status_t pause_stub(uint8_t timer, int calls)
{
    (void)calls;
    ++s_pause_calls;
    s_depth_inside = g_fake_mutex_depth;
    s_timer_seen = timer;
    return KODE_OK;
}

TEST_CASE("pause and resume reach the timer only for own_timer", "[khal_pwm]")
{
    khal_pwm_t own = KHAL_PWM_NONE;
    khal_pwm_t shared = KHAL_PWM_NONE;

    pwm_fresh();
    khal_pwm_port_timer_set_ExpectAndReturn(0U, 125000U, 8U, KODE_OK);
    khal_pwm_port_chan_start_ExpectAndReturn(0U, 8U, 0U, 128U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK, open_ch(8U, 125000U, 8U, 500U, true, &own));
    khal_pwm_port_timer_set_ExpectAndReturn(1U, 1000U, 16U, KODE_OK);
    khal_pwm_port_chan_start_ExpectAndReturn(1U, 9U, 1U, 16384U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK,
                      open_ch(9U, 1000U, KHAL_PWM_BITS_WIDEST, 250U, false, &shared));
    s_pause_calls = 0U;
    s_depth_inside = -1;
    khal_pwm_port_timer_pause_Stub(&pause_stub);
    khal_pwm_port_timer_resume_Stub(&pause_stub);
    TEST_ASSERT_EQUAL(KODE_OK, khal_pwm_pause(own));
    TEST_ASSERT_EQUAL_INT32(0, s_depth_inside);
    TEST_ASSERT_EQUAL_UINT8(0U, s_timer_seen);
    TEST_ASSERT_EQUAL(KODE_OK, khal_pwm_resume(own));
    TEST_ASSERT_EQUAL_INT32(0, s_depth_inside);
    TEST_ASSERT_EQUAL_UINT32(2U, s_pause_calls);
    /* A shared timer is not the caller's to stop. */
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_pwm_pause(shared));
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_pwm_resume(shared));
    TEST_ASSERT_EQUAL_UINT32(2U, s_pause_calls);
    pwm_done();
}

TEST_CASE("close frees the timer with its last user", "[khal_pwm]")
{
    khal_pwm_t a = KHAL_PWM_NONE;
    khal_pwm_t b = KHAL_PWM_NONE;

    pwm_fresh();
    khal_pwm_port_timer_set_ExpectAndReturn(0U, 1000U, 16U, KODE_OK);
    khal_pwm_port_chan_start_ExpectAndReturn(0U, 8U, 0U, 16384U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK,
                      open_ch(8U, 1000U, KHAL_PWM_BITS_WIDEST, 250U, false, &a));
    khal_pwm_port_chan_start_ExpectAndReturn(1U, 9U, 0U, 16384U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK,
                      open_ch(9U, 1000U, KHAL_PWM_BITS_WIDEST, 250U, false, &b));
    khal_pwm_port_chan_stop_Expect(0U, 8U);
    khal_pwm_close(a);
    khal_pwm_port_chan_stop_Expect(1U, 9U);
    khal_pwm_port_timer_stop_Expect(0U);
    khal_pwm_close(b);
    pwm_done();
}

TEST_CASE("a bad handle or a second close calls nothing", "[khal_pwm]")
{
    khal_pwm_t h = KHAL_PWM_NONE;

    pwm_fresh();
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_pwm_set(KHAL_PWM_NONE, 1000U, 500U));
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_pwm_pause(KHAL_PWM_NONE));
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_pwm_set(5U, 1000U, 500U));
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_pwm_resume(5U));
    khal_pwm_close(KHAL_PWM_NONE);
    khal_pwm_port_timer_set_ExpectAndReturn(0U, 1000U, 16U, KODE_OK);
    khal_pwm_port_chan_start_ExpectAndReturn(0U, 8U, 0U, 16384U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK,
                      open_ch(8U, 1000U, KHAL_PWM_BITS_WIDEST, 250U, false, &h));
    khal_pwm_port_chan_stop_Expect(0U, 8U);
    khal_pwm_port_timer_stop_Expect(0U);
    khal_pwm_close(h);
    khal_pwm_close(h);
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_pwm_set(h, 1000U, 500U));
    pwm_done();
}

TEST_CASE("init twice is a no-op", "[khal_pwm]")
{
    khal_pwm_t h = KHAL_PWM_NONE;

    pwm_fresh();
    khal_pwm_port_timer_set_ExpectAndReturn(0U, 1000U, 16U, KODE_OK);
    khal_pwm_port_chan_start_ExpectAndReturn(0U, 8U, 0U, 16384U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK,
                      open_ch(8U, 1000U, KHAL_PWM_BITS_WIDEST, 250U, false, &h));
    TEST_ASSERT_EQUAL(KODE_OK, khal_pwm_init());
    /* The channel survived: the same hz is a duty change on it. */
    khal_pwm_port_chan_duty_ExpectAndReturn(0U, 32768U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK, khal_pwm_set(h, 1000U, 500U));
    pwm_done();
}
