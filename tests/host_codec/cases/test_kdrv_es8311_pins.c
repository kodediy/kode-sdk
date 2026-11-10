/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_es8311_pins.c
 * @brief What kdrv_es8311.c does to the I2S master clock pad: idle until a sound, idle again after a close.
 *
 * The driver is the real source; the I2S driver, the codec component and khal
 * are the recorder in main/fake_idf.c. The driver keeps its state in statics
 * and takes one kdrv_es8311_init() per process, a failed one included, so
 * these cases run in the order they are written:
 *
 *  - the cases of an init that fails come first, each in a child process
 *    forked from this one before any init has run, so every failure path
 *    starts from a driver nobody has touched and the parent stays untouched;
 *  - then the case that brings the driver up for good;
 *  - then the rest, each of which starts from a closed codec.
 *
 * Two witnesses in the fakes count what the driver must never do: let go of
 * the clock pad while a channel is enabled, and use the chip's control
 * interface while a channel is enabled and the pad is not routed. tearDown()
 * asserts both counts are zero after every case.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "unity.h"

#include "fakes.h"
#include "kbsp.h"
#include "kdrv_es8311.h"
#include "khal_gpio.h"

/** @brief The pad that carries the master clock. */
#define MCLK_PIN  ((int)KBSP_PIN_I2S_MCLK)

/**
 * @brief Run one job on the driver, as AO_Audio's worker would.
 * @param type The job.
 * @return What the driver answered.
 */
static kode_status_t job(kdrv_es8311_job_t type)
{
    kdrv_es8311_req_t req = { .type = type };

    return kdrv_es8311_run(&req);
}

/** @brief What a child process answers when the init it ran left the clock pad as it should. */
#define CHILD_OK            0
/** @brief The init did not fail. */
#define CHILD_NOT_REFUSED   1
/** @brief The init failed before the I2S channels ever routed the clock: the path was not reached. */
#define CHILD_NOT_REACHED   2
/** @brief The init failed and left the clock routed to its pad. */
#define CHILD_ROUTED        3
/** @brief The init failed and the pad is not an input with the pull-up. */
#define CHILD_NO_PULLUP     4
/** @brief A witness saw something the driver must not do. */
#define CHILD_VIOLATION     5

/**
 * @brief Run kdrv_es8311_init() in a child process with one of the fakes set to fail.
 *
 * The child judges the pad itself and answers with one of the CHILD_ values,
 * so the parent's driver and fakes stay as they were.
 *
 * @param knob The fake's failure switch to turn on.
 * @return What the child answered, or -1 when it did not exit normally.
 */
static int init_in_child(bool *knob)
{
    int   status = 0;
    pid_t pid;
    pid_t waited;

    (void)fflush(stdout);
    pid = fork();
    if ( 0 == pid )
    {
        int verdict = CHILD_OK;

        fake_reset();
        *knob = true;
        if ( KODE_OK == kdrv_es8311_init() )
        {
            verdict = CHILD_NOT_REFUSED;
        }
        else if ( 0U == fake_count(EV_I2S_INIT) )
        {
            verdict = CHILD_NOT_REACHED;
        }
        else if ( g_mclk_routed )
        {
            verdict = CHILD_ROUTED;
        }
        else if ( !g_pin_pullup )
        {
            verdict = CHILD_NO_PULLUP;
        }
        else if ( 0U != fake_violations() )
        {
            verdict = CHILD_VIOLATION;
        }
        else
        {
            /* The pad is idle and nothing was done that must not be. */
        }
        _exit(verdict);
    }
    TEST_ASSERT_TRUE(0 < pid);
    /* The host port of FreeRTOS drives its tick with signals, and one of them
     * ends a wait early: the wait is taken up again until the child is done. */
    do
    {
        waited = waitpid(pid, &status, 0);
    } while ( ((pid_t)-1 == waited) && (EINTR == errno) );
    TEST_ASSERT_EQUAL_INT(pid, waited);
    if ( !WIFEXITED(status) )
    {
        return -1;
    }
    return WEXITSTATUS(status);
}

TEST_CASE("an init whose receive channel refuses its format leaves the clock pad idle",
          "[kdrv_es8311][pins]")
{
    TEST_ASSERT_EQUAL_INT(CHILD_OK, init_in_child(&g_fail_rx_init));
}

TEST_CASE("an init whose codec data interface cannot be made leaves the clock pad idle",
          "[kdrv_es8311][pins]")
{
    TEST_ASSERT_EQUAL_INT(CHILD_OK, init_in_child(&g_fail_data_if));
}

TEST_CASE("an init whose codec object cannot be made leaves the clock pad idle",
          "[kdrv_es8311][pins]")
{
    TEST_ASSERT_EQUAL_INT(CHILD_OK, init_in_child(&g_fail_codec_new));
}

TEST_CASE("an init whose codec device cannot be made leaves the clock pad idle",
          "[kdrv_es8311][pins]")
{
    TEST_ASSERT_EQUAL_INT(CHILD_OK, init_in_child(&g_fail_dev_new));
}

TEST_CASE("an init whose speaker cap is refused leaves the clock pad idle",
          "[kdrv_es8311][pins]")
{
    TEST_ASSERT_EQUAL_INT(CHILD_OK, init_in_child(&g_fail_vol_curve));
}

/** @brief A closed codec and an empty log, whatever the case before left. */
static void driver_idle(void)
{
    (void)job(KDRV_ES8311_JOB_CLOSE);
    fake_reset();
}

TEST_CASE("after init, with no sound, the clock pad is an input with the pull-up",
          "[kdrv_es8311][pins]")
{
    fake_reset();
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_es8311_init());
    TEST_ASSERT_TRUE(kdrv_es8311_present());
    TEST_ASSERT_FALSE(kdrv_es8311_is_open());

    TEST_ASSERT_FALSE(g_mclk_routed);
    TEST_ASSERT_TRUE(g_pin_pullup);
    TEST_ASSERT_EQUAL_INT(MCLK_PIN, g_mclk_pin);
    TEST_ASSERT_EQUAL_UINT(1U, fake_count(EV_PIN_RELEASE));
    TEST_ASSERT_EQUAL_UINT(1U, fake_count(EV_PIN_PULLUP));
    TEST_ASSERT_EQUAL_UINT(0U, fake_count(EV_PIN_OTHER));
    TEST_ASSERT_EQUAL_INT(MCLK_PIN, g_log[fake_last(EV_PIN_RELEASE)].arg);
    TEST_ASSERT_EQUAL_INT(MCLK_PIN, g_log[fake_last(EV_PIN_PULLUP)].arg);
    /* The channels are built first and the pad goes quiet after them. */
    TEST_ASSERT_TRUE(fake_last(EV_I2S_INIT) < fake_last(EV_PIN_RELEASE));
}

TEST_CASE("an open routes the clock pad to both channels before the codec layer enables them",
          "[kdrv_es8311][pins]")
{
    driver_idle();
    TEST_ASSERT_EQUAL(KODE_OK, job(KDRV_ES8311_JOB_OPEN));

    TEST_ASSERT_TRUE(kdrv_es8311_is_open());
    TEST_ASSERT_TRUE(g_mclk_routed);
    TEST_ASSERT_EQUAL_UINT(1U, fake_count(EV_I2S_ROUTE_TX));
    TEST_ASSERT_EQUAL_UINT(1U, fake_count(EV_I2S_ROUTE_RX));
    TEST_ASSERT_EQUAL_INT(MCLK_PIN, g_log[fake_last(EV_I2S_ROUTE_TX)].arg);
    TEST_ASSERT_EQUAL_INT(MCLK_PIN, g_log[fake_last(EV_I2S_ROUTE_RX)].arg);
    TEST_ASSERT_TRUE(fake_last(EV_I2S_ROUTE_TX) < fake_last(EV_CODEC_OPEN));
    TEST_ASSERT_TRUE(fake_last(EV_I2S_ROUTE_RX) < fake_last(EV_CODEC_OPEN));
}

TEST_CASE("a close puts the clock pad back to an input with the pull-up",
          "[kdrv_es8311][pins]")
{
    driver_idle();
    TEST_ASSERT_EQUAL(KODE_OK, job(KDRV_ES8311_JOB_OPEN));
    TEST_ASSERT_EQUAL(KODE_OK, job(KDRV_ES8311_JOB_CLOSE));

    TEST_ASSERT_FALSE(kdrv_es8311_is_open());
    TEST_ASSERT_FALSE(g_mclk_routed);
    TEST_ASSERT_TRUE(g_pin_pullup);
    TEST_ASSERT_EQUAL_INT(MCLK_PIN, g_log[fake_last(EV_PIN_RELEASE)].arg);
    TEST_ASSERT_EQUAL_INT(MCLK_PIN, g_log[fake_last(EV_PIN_PULLUP)].arg);
    /* The channels are disabled first: the clock stops being wanted, then the pad lets go. */
    TEST_ASSERT_TRUE(fake_last(EV_CODEC_CLOSE) < fake_last(EV_PIN_RELEASE));
    TEST_ASSERT_TRUE(fake_last(EV_PIN_RELEASE) < fake_last(EV_PIN_PULLUP));
}

TEST_CASE("a second open routes the clock pad again and opens",
          "[kdrv_es8311][pins]")
{
    driver_idle();
    TEST_ASSERT_EQUAL(KODE_OK, job(KDRV_ES8311_JOB_OPEN));
    TEST_ASSERT_EQUAL(KODE_OK, job(KDRV_ES8311_JOB_CLOSE));
    TEST_ASSERT_EQUAL(KODE_OK, job(KDRV_ES8311_JOB_OPEN));

    TEST_ASSERT_TRUE(kdrv_es8311_is_open());
    TEST_ASSERT_TRUE(g_mclk_routed);
    TEST_ASSERT_EQUAL_UINT(2U, fake_count(EV_I2S_ROUTE_TX));
    TEST_ASSERT_EQUAL_UINT(2U, fake_count(EV_I2S_ROUTE_RX));
    TEST_ASSERT_EQUAL_UINT(2U, fake_count(EV_CODEC_OPEN));
    TEST_ASSERT_TRUE(fake_last(EV_PIN_PULLUP) < fake_last(EV_I2S_ROUTE_TX));
}

TEST_CASE("an open on an open codec leaves the clock pad alone",
          "[kdrv_es8311][pins]")
{
    driver_idle();
    TEST_ASSERT_EQUAL(KODE_OK, job(KDRV_ES8311_JOB_OPEN));
    TEST_ASSERT_EQUAL(KODE_OK, job(KDRV_ES8311_JOB_OPEN));

    TEST_ASSERT_EQUAL_UINT(1U, fake_count(EV_I2S_ROUTE_TX));
    TEST_ASSERT_EQUAL_UINT(1U, fake_count(EV_CODEC_OPEN));
    TEST_ASSERT_TRUE(g_mclk_routed);
}

TEST_CASE("a close with nothing open leaves the clock pad alone",
          "[kdrv_es8311][pins]")
{
    driver_idle();
    TEST_ASSERT_EQUAL(KODE_OK, job(KDRV_ES8311_JOB_CLOSE));

    TEST_ASSERT_EQUAL_UINT(0U, g_log_n);
}

TEST_CASE("an open the codec layer refuses leaves the clock pad idle and the next open working",
          "[kdrv_es8311][pins]")
{
    driver_idle();
    g_open_result = -1;
    TEST_ASSERT_EQUAL(KODE_ERROR, job(KDRV_ES8311_JOB_OPEN));

    /* The component enabled the channels before it refused: the driver closes. */
    TEST_ASSERT_EQUAL_UINT(1U, fake_count(EV_CODEC_OPEN));
    TEST_ASSERT_EQUAL_UINT(1U, fake_count(EV_CODEC_CLOSE));
    TEST_ASSERT_TRUE(fake_last(EV_CODEC_OPEN) < fake_last(EV_CODEC_CLOSE));
    TEST_ASSERT_TRUE(fake_last(EV_CODEC_CLOSE) < fake_last(EV_PIN_RELEASE));
    TEST_ASSERT_FALSE(g_chan_running);
    TEST_ASSERT_FALSE(kdrv_es8311_is_open());
    TEST_ASSERT_FALSE(g_mclk_routed);
    TEST_ASSERT_TRUE(g_pin_pullup);

    /* The next open finds channels that can take the pins again. */
    g_open_result = 0;
    TEST_ASSERT_EQUAL(KODE_OK, job(KDRV_ES8311_JOB_OPEN));
    TEST_ASSERT_EQUAL_UINT(2U, fake_count(EV_I2S_ROUTE_TX));
    TEST_ASSERT_EQUAL_UINT(2U, fake_count(EV_I2S_ROUTE_RX));
    TEST_ASSERT_TRUE(kdrv_es8311_is_open());
    TEST_ASSERT_TRUE(g_mclk_routed);
}

TEST_CASE("an open whose receive channel refuses the pins is refused before the codec layer runs and leaves the pad idle",
          "[kdrv_es8311][pins]")
{
    driver_idle();
    g_fail_route_rx = true;
    TEST_ASSERT_EQUAL(KODE_ERROR, job(KDRV_ES8311_JOB_OPEN));

    /* The transmit half had routed the clock by then; the driver takes it back. */
    TEST_ASSERT_EQUAL_UINT(1U, fake_count(EV_I2S_ROUTE_TX));
    TEST_ASSERT_FALSE(kdrv_es8311_is_open());
    TEST_ASSERT_EQUAL_UINT(0U, fake_count(EV_CODEC_OPEN));
    TEST_ASSERT_FALSE(g_mclk_routed);
    TEST_ASSERT_TRUE(g_pin_pullup);
}

TEST_CASE("the witnesses see a pad let go or a register written while a channel is enabled",
          "[kdrv_es8311][pins]")
{
    driver_idle();

    /* The pad: let go, or set as an input, while a channel is enabled. */
    g_chan_running = true;
    khal_gpio_release((uint8_t)KBSP_PIN_I2S_MCLK);
    TEST_ASSERT_EQUAL_UINT(1U, g_viol_pad);
    (void)khal_gpio_input((uint8_t)KBSP_PIN_I2S_MCLK, KHAL_GPIO_PULL_UP);
    TEST_ASSERT_EQUAL_UINT(2U, g_viol_pad);
    /* Another pad is none of its business, and neither is a pad nobody enabled. */
    khal_gpio_release((uint8_t)KBSP_PIN_I2S_SCLK);
    g_chan_running = false;
    khal_gpio_release((uint8_t)KBSP_PIN_I2S_MCLK);
    TEST_ASSERT_EQUAL_UINT(2U, g_viol_pad);
    TEST_ASSERT_EQUAL_UINT(0U, g_viol_ctrl);

    /* The chip: a write while a channel is enabled and the clock is not on its pad. */
    g_chan_running = true;
    g_mclk_routed  = false;
    fake_ctrl_poke();
    TEST_ASSERT_EQUAL_UINT(1U, g_viol_ctrl);
    /* With the clock routed it is fine; with the channels idle it is fine too. */
    g_mclk_routed = true;
    fake_ctrl_poke();
    g_chan_running = false;
    g_mclk_routed  = false;
    fake_ctrl_poke();
    TEST_ASSERT_EQUAL_UINT(1U, g_viol_ctrl);
    TEST_ASSERT_EQUAL_UINT(3U, fake_violations());

    /* They were provoked on purpose: nothing is left for tearDown(). */
    g_viol_pad  = 0U;
    g_viol_ctrl = 0U;
}
