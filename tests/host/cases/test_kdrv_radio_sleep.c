/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_radio_sleep.c
 * @brief The radio sleep sentinel's handshake with the C5, down and back up.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "unity.h"

#include "kdrv_radio.h"
#include "kdrv_radio_sleep.h"
#include "kqp_signals.h"

/** @brief What the fakes write into the log; a delay is DELAY_BASE plus its milliseconds. */
enum {
    EV_PS_ON = 1,
    EV_PS_OFF,
    EV_TASKS_RUN,
    EV_TASKS_STOP,
    EV_MARK_DOWN,
    EV_MARK_UP,
    EV_DELAY_BASE = 100000
};

#define LOG_MAX  256U    /**< Room for a deaf wake: 101 knocks and 100 waits. */

static uint32_t s_log[LOG_MAX];       /**< The events, in order.                       */
static size_t   s_log_n;              /**< How many.                                   */
static uint32_t s_off_fail;           /**< Power-save-off messages left to fail.       */
static bool     s_on_fails;           /**< Power-save-on is refused.                   */
static uint32_t s_gap;                /**< The fake GAP count.                         */
static uint32_t s_gap_bumps;          /**< Awake windows during which the count moves. */
static bool     s_marked;             /**< The bus is marked silent: a frame queued now would ring the sentinel. */
static bool     s_line_fails;         /**< The C5's line will not arm. */
static uint32_t s_unsettled;          /**< Looks at the SDIO tasks that still find one mid-transaction. */
static uint32_t s_looks;              /**< Looks made. */
static bool     s_marked_at_announce; /**< s_marked when the C5 was told power save is on. */
static bool     s_marked_at_suspend;  /**< s_marked when the tasks were suspended. */
static bool     s_cmd_waiting;        /**< A command waits for the sentinel's task. */
static uint32_t s_cmd_in;             /**< GAP-sized waits left before a command arrives; 0 = none planned. */

static void note(uint32_t ev)
{
    if ( LOG_MAX > s_log_n )
    {
        s_log[s_log_n] = ev;
        s_log_n++;
    }
}

static int fake_power_save(bool on)
{
    if ( on )
    {
        note(EV_PS_ON);
        s_marked_at_announce = s_marked;
        if ( s_on_fails )
        {
            return -1;
        }
        return 0;
    }
    note(EV_PS_OFF);
    if ( 0U < s_off_fail )
    {
        s_off_fail--;
        return -1;
    }
    return 0;
}

static void fake_tasks(bool run)
{
    if ( run )
    {
        note(EV_TASKS_RUN);
    }
    else
    {
        note(EV_TASKS_STOP);
        s_marked_at_suspend = s_marked;
    }
}

static bool fake_settled(void)
{
    s_looks++;
    if ( 0U < s_unsettled )
    {
        s_unsettled--;
        return false;
    }
    return true;
}

static bool fake_bus_down(bool down)
{
    s_marked = down;
    if ( down )
    {
        note(EV_MARK_DOWN);
        return !s_line_fails;
    }
    note(EV_MARK_UP);
    return true;
}

static void fake_delay(uint32_t ms)
{
    note((uint32_t)EV_DELAY_BASE + ms);
    /* The link moves while the board is held awake for the traffic. */
    if ( (KRADIO_SLEEP_GAP_MS == ms) && (0U < s_cmd_in) )
    {
        s_cmd_in--;
        if ( 0U == s_cmd_in )
        {
            s_cmd_waiting = true;
        }
    }
    if ( (KRADIO_SLEEP_AWAKE_MS == ms) || (KRADIO_SLEEP_GAP_MS == ms) )
    {
        if ( 0U < s_gap_bumps )
        {
            s_gap_bumps--;
            s_gap++;
        }
    }
}

static uint32_t fake_gap(void)
{
    return s_gap;
}

static bool fake_cmd_waiting(void)
{
    return s_cmd_waiting;
}

static const kradio_sleep_seam_t SEAM = {
    .power_save   = &fake_power_save,
    .sdio_tasks   = &fake_tasks,
    .sdio_settled = &fake_settled,
    .bus_down     = &fake_bus_down,
    .delay_ms     = &fake_delay,
    .gap_activity = &fake_gap,
    .cmd_waiting  = &fake_cmd_waiting,
};

static void reset(void)
{
    s_log_n = 0U;
    s_off_fail = 0U;
    s_on_fails = false;
    s_gap = 0U;
    s_gap_bumps = 0U;
    s_marked = false;
    s_line_fails = false;
    s_unsettled = 0U;
    s_looks = 0U;
    s_marked_at_announce = false;
    s_marked_at_suspend = false;
    s_cmd_waiting = false;
    s_cmd_in = 0U;
}

/** @brief How many times the log holds @p ev. */
static size_t count(uint32_t ev)
{
    size_t n = 0U;

    for ( size_t i = 0U; i < s_log_n; i++ )
    {
        if ( s_log[i] == ev )
        {
            n++;
        }
    }
    return n;
}

TEST_CASE("going down marks the bus, announces, waits, then silences it", "[radio_sleep]")
{
    reset();
    TEST_ASSERT_EQUAL_INT(KRADIO_DOWN_OK, kradio_sleep_down(&SEAM));
    TEST_ASSERT_EQUAL_UINT32(4U, (uint32_t)s_log_n);
    TEST_ASSERT_EQUAL_UINT32(EV_MARK_DOWN, s_log[0]);
    TEST_ASSERT_EQUAL_UINT32(EV_PS_ON, s_log[1]);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)EV_DELAY_BASE + KRADIO_SLEEP_PS_WAIT_MS, s_log[2]);
    TEST_ASSERT_EQUAL_UINT32(EV_TASKS_STOP, s_log[3]);
    TEST_ASSERT_TRUE(s_marked);
}

TEST_CASE("a frame queued while the C5 is being told, or just before the suspend, wakes", "[radio_sleep]")
{
    reset();
    TEST_ASSERT_EQUAL_INT(KRADIO_DOWN_OK, kradio_sleep_down(&SEAM));
    /* The mark is what makes the hook ring; it must already stand at both. */
    TEST_ASSERT_TRUE(s_marked_at_announce);
    TEST_ASSERT_TRUE(s_marked_at_suspend);
}

TEST_CASE("a C5 that refuses the announcement leaves the bus running and unmarked", "[radio_sleep]")
{
    reset();
    s_on_fails = true;
    TEST_ASSERT_EQUAL_INT(KRADIO_DOWN_NO_PS, kradio_sleep_down(&SEAM));
    TEST_ASSERT_EQUAL_size_t(0U, count(EV_TASKS_STOP));
    TEST_ASSERT_FALSE(s_marked);
    TEST_ASSERT_EQUAL_UINT32(EV_MARK_UP, s_log[s_log_n - 1U]);
}

TEST_CASE("a line that will not arm stops the way down before the C5 hears anything", "[radio_sleep]")
{
    reset();
    s_line_fails = true;
    TEST_ASSERT_EQUAL_INT(KRADIO_DOWN_NO_LINE, kradio_sleep_down(&SEAM));
    TEST_ASSERT_EQUAL_size_t(0U, count(EV_PS_ON));
    TEST_ASSERT_EQUAL_size_t(0U, count(EV_TASKS_STOP));
    TEST_ASSERT_FALSE(s_marked);
}

TEST_CASE("tasks that settle late are waited for, then suspended", "[radio_sleep]")
{
    reset();
    s_unsettled = 3U;
    TEST_ASSERT_EQUAL_INT(KRADIO_DOWN_OK, kradio_sleep_down(&SEAM));
    TEST_ASSERT_EQUAL_UINT32(4U, s_looks);
    TEST_ASSERT_EQUAL_size_t(3U, count((uint32_t)EV_DELAY_BASE + KRADIO_SLEEP_SETTLE_MS));
    /* Suspended only after the last look found them parked. */
    TEST_ASSERT_EQUAL_UINT32(EV_TASKS_STOP, s_log[s_log_n - 1U]);
}

TEST_CASE("tasks that never settle refuse the way down with the bus up", "[radio_sleep]")
{
    reset();
    s_unsettled = 1000U;
    TEST_ASSERT_EQUAL_INT(KRADIO_DOWN_BUSY, kradio_sleep_down(&SEAM));
    TEST_ASSERT_EQUAL_UINT32(KRADIO_SLEEP_SETTLE_TRIES + 1U, s_looks);
    /* Never suspended; the C5 is told power save is off again and the tasks
     * are left running (resume is harmless on tasks that were never stopped). */
    TEST_ASSERT_EQUAL_size_t(0U, count(EV_TASKS_STOP));
    TEST_ASSERT_EQUAL_size_t(1U, count(EV_PS_ON));
    TEST_ASSERT_TRUE(count(EV_PS_OFF) >= 1U);
    TEST_ASSERT_EQUAL_size_t(1U, count(EV_TASKS_RUN));
    TEST_ASSERT_FALSE(s_marked);
    TEST_ASSERT_EQUAL_UINT32(EV_MARK_UP, s_log[s_log_n - 1U]);
}

TEST_CASE("wakes_on_first_answer", "[radio_sleep]")
{
    uint16_t knocks = 0U;

    reset();
    TEST_ASSERT_TRUE(kradio_sleep_up(&SEAM, &knocks));
    TEST_ASSERT_EQUAL_UINT16(1U, knocks);
    TEST_ASSERT_EQUAL_UINT32(3U, (uint32_t)s_log_n);
    TEST_ASSERT_EQUAL_UINT32(EV_PS_OFF, s_log[0]);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)EV_DELAY_BASE + KRADIO_SLEEP_RESUME_WAIT_MS, s_log[1]);
    TEST_ASSERT_EQUAL_UINT32(EV_TASKS_RUN, s_log[2]);
}

TEST_CASE("retries_until_answer", "[radio_sleep]")
{
    uint16_t knocks = 0U;

    reset();
    s_off_fail = 17U;
    TEST_ASSERT_TRUE(kradio_sleep_up(&SEAM, &knocks));
    TEST_ASSERT_EQUAL_UINT16(18U, knocks);
    TEST_ASSERT_EQUAL_size_t(18U, count(EV_PS_OFF));
    /* Seventeen waits between the eighteen knocks, then the one before resuming. */
    TEST_ASSERT_EQUAL_size_t(17U, count((uint32_t)EV_DELAY_BASE + KRADIO_SLEEP_OFF_RETRY_MS));
    TEST_ASSERT_EQUAL_size_t(1U, count((uint32_t)EV_DELAY_BASE + KRADIO_SLEEP_RESUME_WAIT_MS));
    TEST_ASSERT_EQUAL_UINT32(EV_TASKS_RUN, s_log[s_log_n - 1U]);
}

TEST_CASE("deaf_after_100", "[radio_sleep]")
{
    uint16_t knocks = 0U;

    reset();
    s_off_fail = 1000U;
    TEST_ASSERT_FALSE(kradio_sleep_up(&SEAM, &knocks));
    /* The first knock and one hundred retries. */
    TEST_ASSERT_EQUAL_UINT16(KRADIO_SLEEP_OFF_RETRIES + 1U, knocks);
    TEST_ASSERT_EQUAL_size_t(KRADIO_SLEEP_OFF_RETRIES + 1U, count(EV_PS_OFF));
    TEST_ASSERT_EQUAL_size_t(KRADIO_SLEEP_OFF_RETRIES,
                             count((uint32_t)EV_DELAY_BASE + KRADIO_SLEEP_OFF_RETRY_MS));
    /* Never resumed against a C5 that did not answer. */
    TEST_ASSERT_EQUAL_size_t(0U, count(EV_TASKS_RUN));
    TEST_ASSERT_EQUAL_size_t(0U, count((uint32_t)EV_DELAY_BASE + KRADIO_SLEEP_RESUME_WAIT_MS));

    /* The next wake starts over from one knock. */
    s_log_n = 0U;
    s_off_fail = 0U;
    TEST_ASSERT_TRUE(kradio_sleep_up(&SEAM, &knocks));
    TEST_ASSERT_EQUAL_UINT16(1U, knocks);
    TEST_ASSERT_EQUAL_size_t(1U, count(EV_TASKS_RUN));
}

TEST_CASE("a wake serves the traffic for the awake window and no longer", "[radio_sleep]")
{
    uint16_t knocks = 0U;

    reset();
    TEST_ASSERT_TRUE(kradio_sleep_wake_once(&SEAM, &knocks));
    TEST_ASSERT_EQUAL_UINT16(1U, knocks);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)EV_DELAY_BASE + KRADIO_SLEEP_AWAKE_MS, s_log[s_log_n - 1U]);
    TEST_ASSERT_EQUAL_size_t(0U, count((uint32_t)EV_DELAY_BASE + KRADIO_SLEEP_GAP_MS));
}

TEST_CASE("a deaf wake ends at once, without the window", "[radio_sleep]")
{
    uint16_t knocks = 0U;

    reset();
    s_off_fail = 1000U;
    TEST_ASSERT_FALSE(kradio_sleep_wake_once(&SEAM, &knocks));
    TEST_ASSERT_EQUAL_UINT16(KRADIO_SLEEP_OFF_RETRIES + 1U, knocks);
    TEST_ASSERT_EQUAL_size_t(0U, count((uint32_t)EV_DELAY_BASE + KRADIO_SLEEP_AWAKE_MS));
    TEST_ASSERT_EQUAL_size_t(0U, count(EV_TASKS_RUN));
}

TEST_CASE("stays_while_gap_active", "[radio_sleep]")
{
    uint16_t knocks = 0U;

    reset();
    s_gap_bumps = 1U;    /* the link moves during the awake window, then settles */
    TEST_ASSERT_TRUE(kradio_sleep_wake_once(&SEAM, &knocks));
    TEST_ASSERT_EQUAL_size_t(1U, count((uint32_t)EV_DELAY_BASE + KRADIO_SLEEP_GAP_MS));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)EV_DELAY_BASE + KRADIO_SLEEP_GAP_MS, s_log[s_log_n - 1U]);

    /* A link that keeps moving extends the window each time. */
    s_log_n = 0U;
    s_gap_bumps = 3U;
    TEST_ASSERT_TRUE(kradio_sleep_wake_once(&SEAM, &knocks));
    TEST_ASSERT_EQUAL_size_t(3U, count((uint32_t)EV_DELAY_BASE + KRADIO_SLEEP_GAP_MS));
}

TEST_CASE("a command that waits ends the repeated window", "[radio_sleep]")
{
    uint16_t knocks = 0U;

    /* The link keeps moving, as it does while the console writes over BLE,
     * but a stop is waiting: the wake ends after the first window. */
    reset();
    s_gap_bumps = 5U;
    s_cmd_waiting = true;
    TEST_ASSERT_TRUE(kradio_sleep_wake_once(&SEAM, &knocks));
    TEST_ASSERT_EQUAL_size_t(0U, count((uint32_t)EV_DELAY_BASE + KRADIO_SLEEP_GAP_MS));

    /* A command that arrives during the second wait ends it there. */
    reset();
    s_gap_bumps = 10U;
    s_cmd_in = 2U;
    TEST_ASSERT_TRUE(kradio_sleep_wake_once(&SEAM, &knocks));
    TEST_ASSERT_EQUAL_size_t(2U, count((uint32_t)EV_DELAY_BASE + KRADIO_SLEEP_GAP_MS));
}

TEST_CASE("with no command waiting the window still extends while the link moves", "[radio_sleep]")
{
    uint16_t knocks = 0U;

    reset();
    s_gap_bumps = 4U;
    TEST_ASSERT_TRUE(kradio_sleep_wake_once(&SEAM, &knocks));
    TEST_ASSERT_EQUAL_size_t(4U, count((uint32_t)EV_DELAY_BASE + KRADIO_SLEEP_GAP_MS));
}

TEST_CASE("tx_kinds_counted", "[radio_sleep]")
{
    /* esp-hosted numbers STA = 1, SERIAL = 3, HCI = 4. */
    TEST_ASSERT_EQUAL_INT(KRADIO_SLEEP_TX_STA, kradio_sleep_tx_slot(1U));
    TEST_ASSERT_EQUAL_INT(KRADIO_SLEEP_TX_SERIAL, kradio_sleep_tx_slot(3U));
    TEST_ASSERT_EQUAL_INT(KRADIO_SLEEP_TX_HCI, kradio_sleep_tx_slot(4U));
    /* invalid (0), AP (2), private (5), test (6), ethernet (7) and anything else */
    TEST_ASSERT_EQUAL_INT(KRADIO_SLEEP_TX_OTHER, kradio_sleep_tx_slot(0U));
    TEST_ASSERT_EQUAL_INT(KRADIO_SLEEP_TX_OTHER, kradio_sleep_tx_slot(2U));
    TEST_ASSERT_EQUAL_INT(KRADIO_SLEEP_TX_OTHER, kradio_sleep_tx_slot(5U));
    TEST_ASSERT_EQUAL_INT(KRADIO_SLEEP_TX_OTHER, kradio_sleep_tx_slot(255U));
}

TEST_CASE("the note kinds are the signal's wake kinds", "[radio_sleep]")
{
    /* kdrv cannot include kqp, so the two lists are tied here. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_RADIO_WOKE_C5, KDRV_RADIO_WOKE_C5);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_RADIO_WOKE_TX, KDRV_RADIO_WOKE_TX);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_RADIO_WOKE_DEAF, KDRV_RADIO_WOKE_DEAF);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_RADIO_WOKE_STOOD_DOWN, KDRV_RADIO_WOKE_STOOD_DOWN);
}
