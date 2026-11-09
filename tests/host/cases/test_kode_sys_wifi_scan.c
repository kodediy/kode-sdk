/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_sys_wifi_scan.c
 * @brief The Wi-Fi scan in kode_sys: an AO's request and the blocking call under kode_wifi_scan().
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "fakes.h"
#include "kaos.h"
#include "kdrv_wifi.h"
#include "khal_sync.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kqp.h"
#include "kqp_config.h"
#include "qf_support.h"

/** @brief The bound the cases hand the blocking piece, in ms. */
#define SCAN_TEST_MS  1000U

/** @brief Posts that surely fill AO_Radio's queue. */
#define SCAN_FILL_MAX  64U

/** @brief Rows a case's callback keeps. */
#define SCAN_KEEP_MAX  KODE_SYS_WIFI_SCAN_MAX

/** @brief What the stand-in could not do, if anything; read once the call returned. */
static const char *s_hook_fault;

/** @brief The rows the callback was offered, in order. */
static kode_sys_wifi_ap_t s_kept[SCAN_KEEP_MAX];

/** @brief How many rows the callback was offered. */
static unsigned s_kept_n;

/** @brief Rows after which the callback asks to stop; 0 = never. */
static unsigned s_stop_after;

/** @brief Empty AO_Radio's queue, recycling what it holds. */
static void radio_flush(void)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief A fresh case: AO_Radio's queue empty, the semaphore without a token. */
static void scan_fresh(void)
{
    qf_support_init();
    qf_support_give_queue(AO_Radio);
    radio_flush();
    fake_sem_reset();
    fake_time_reset();
    s_hook_fault = NULL;
    s_kept_n     = 0U;
    s_stop_after = 0U;
    (void)memset(s_kept, 0, sizeof(s_kept));
}

/** @brief The callback: keep every row offered, stop when asked to. */
static bool keep_row(const kode_sys_wifi_ap_t *ap, void *ctx)
{
    (void)ctx;
    if ( SCAN_KEEP_MAX > s_kept_n )
    {
        s_kept[s_kept_n] = *ap;
    }
    ++s_kept_n;
    return (0U == s_stop_after) || (s_kept_n < s_stop_after);
}

/**
 * @brief Answer the request at the head of AO_Radio's queue, as AO_Radio would.
 * @param n      Rows the sweep found, written into the waiter's table.
 * @param prefix What the rows' names start with.
 * @param rc     The answer's status.
 */
static void answer_head(unsigned n, const char *prefix, kode_status_t rc)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Radio->eQueue, &e, 0U) != pdTRUE )
    {
        s_hook_fault = "nothing was posted to AO_Radio";
        return;
    }
    if ( (QSignal)RADIO_SCAN_ASK_SIG != e->sig )
    {
        s_hook_fault = "AO_Radio was posted something other than a scan";
        QF_gc(e);
        return;
    }

    const ScanAskEvt *const a = (const ScanAskEvt *)(const void *)e;
    ScanWaiter *const       w = a->wait;

    if ( (NULL == w) || (NULL != a->ask) || (NULL != a->rsp) )
    {
        s_hook_fault = "a task's scan came without its waiter, or with an AO's answer";
        QF_gc(e);
        return;
    }
    kdrv_wifi_ap_t *const rows = (kdrv_wifi_ap_t *)w->rows;
    unsigned              got  = 0U;

    for ( unsigned i = 0U; (i < n) && ((unsigned)w->cap > i); ++i )
    {
        (void)memset(&rows[i], 0, sizeof(rows[i]));
        (void)snprintf(rows[i].ssid, sizeof(rows[i].ssid), "%s-%u", prefix, i);
        rows[i].rssi     = (int8_t)(-40 - (int)i);
        rows[i].channel  = (uint8_t)(1U + i);
        rows[i].authmode = (uint8_t)(i % 2U);
        ++got;
    }
    w->rc  = (uint8_t)rc;
    w->got = (uint8_t)got;
    __atomic_store_n(&w->ack, a->gen, __ATOMIC_RELEASE);
    khal_sem_signal(w->done);
    QF_gc(e);
}

/** @brief AO_Radio, inside the wait: three networks. */
static void radio_answers_three(void)
{
    answer_head(3U, "net", KODE_OK);
}

/** @brief AO_Radio, inside the wait: the sweep failed. */
static void radio_answers_failure(void)
{
    answer_head(2U, "net", KODE_ERROR);
}

/** @brief AO_Radio, inside the wait: the radio already scans for someone else. */
static void radio_answers_busy(void)
{
    answer_head(0U, "net", KODE_NO_RESOURCES);
}

/** @brief Answers given by radio_answers_late(), in order. */
static unsigned s_late_step;

/**
 * @brief AO_Radio, late: the abandoned request first, then the one asked now.
 *
 * Re-arms itself once, so it runs in both waits the next call makes: the one
 * that waits the abandoned answer out, and its own.
 */
static void radio_answers_late(void)
{
    if ( 0U == s_late_step )
    {
        answer_head(4U, "old", KODE_OK);
        s_late_step = 1U;
        fake_sem_wait_hook(&radio_answers_late);
    }
    else
    {
        answer_head(2U, "new", KODE_OK);
        s_late_step = 2U;
    }
}

/** @brief How many requests wait for AO_Radio. */
static unsigned radio_pending(void)
{
    return (unsigned)uxQueueMessagesWaiting(AO_Radio->eQueue);
}

/* ---- The request: kode_sys_wifi_scan() ----------------------------------------------- */

/* The rest of the request - the door, the entries, the end - is AO_Radio's and
 * is walked in test_radio_ao.c with the AO dispatching. */
TEST_CASE("a scan asked by no AO is refused and posts nothing", "[kode_sys_wifi_scan]")
{
    scan_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_wifi_scan(NULL));
    TEST_ASSERT_EQUAL_UINT(0U, radio_pending());
}

/* ---- The blocking piece: kode_block_wifi_scan() ----------------------------------------- */

TEST_CASE("a blocking scan offers every row AO_Radio wrote, in its order", "[kode_sys_wifi_scan]")
{
    scan_fresh();
    fake_sem_wait_hook(&radio_answers_three);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_wifi_scan(&keep_row, NULL, SCAN_TEST_MS));
    TEST_ASSERT_NULL_MESSAGE(s_hook_fault, s_hook_fault);
    TEST_ASSERT_EQUAL_UINT(3U, s_kept_n);
    TEST_ASSERT_EQUAL_STRING("net-0", s_kept[0].ssid);
    TEST_ASSERT_EQUAL_INT8(-40, s_kept[0].rssi);
    TEST_ASSERT_EQUAL_UINT8(1U, s_kept[0].channel);
    TEST_ASSERT_TRUE_MESSAGE(s_kept[0].open, "authmode 0 is an open network");
    TEST_ASSERT_FALSE(s_kept[1].open);
    TEST_ASSERT_EQUAL_STRING("net-2", s_kept[2].ssid);
    TEST_ASSERT_EQUAL_UINT32(SCAN_TEST_MS, fake_sem_last_wait_ms());
    TEST_ASSERT_EQUAL_UINT(0U, radio_pending());
}

TEST_CASE("a blocking scan stops offering rows when the callback says so", "[kode_sys_wifi_scan]")
{
    scan_fresh();
    s_stop_after = 2U;
    fake_sem_wait_hook(&radio_answers_three);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_wifi_scan(&keep_row, NULL, SCAN_TEST_MS));
    TEST_ASSERT_NULL_MESSAGE(s_hook_fault, s_hook_fault);
    TEST_ASSERT_EQUAL_UINT(2U, s_kept_n);
}

TEST_CASE("a blocking scan with no callback is refused and posts nothing", "[kode_sys_wifi_scan]")
{
    scan_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kode_block_wifi_scan(NULL, NULL, SCAN_TEST_MS));
    TEST_ASSERT_EQUAL_UINT(0U, radio_pending());
}

TEST_CASE("a blocking scan says what AO_Radio answered, and offers nothing on a failure",
          "[kode_sys_wifi_scan]")
{
    scan_fresh();
    fake_sem_wait_hook(&radio_answers_failure);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kode_block_wifi_scan(&keep_row, NULL, SCAN_TEST_MS));
    TEST_ASSERT_NULL_MESSAGE(s_hook_fault, s_hook_fault);
    TEST_ASSERT_EQUAL_UINT(0U, s_kept_n);

    fake_sem_wait_hook(&radio_answers_busy);
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_block_wifi_scan(&keep_row, NULL, SCAN_TEST_MS));
    TEST_ASSERT_NULL_MESSAGE(s_hook_fault, s_hook_fault);
    TEST_ASSERT_EQUAL_UINT(0U, s_kept_n);
}

TEST_CASE("a blocking scan AO_Radio has no room for is refused at once", "[kode_sys_wifi_scan]")
{
    static const QEvt filler = QEVT_INITIALIZER(WIFI_STATE_REQ_SIG);

    scan_fresh();
    for ( uint32_t guard = 0U;
         (SCAN_FILL_MAX > guard)
         && (uxQueueSpacesAvailable(AO_Radio->eQueue) > KQP_RADIO_CMD_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_Radio, &filler, NULL);
    }
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_block_wifi_scan(&keep_row, NULL, SCAN_TEST_MS));
    TEST_ASSERT_EQUAL_UINT(0U, s_kept_n);
    radio_flush();
}

TEST_CASE("test_wifi_scan_late_answer_is_dropped", "[kode_sys_wifi_scan]")
{
    scan_fresh();
    s_late_step = 0U;

    /* Nobody answers in time: the wait runs out, and the request stays owed. */
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_wifi_scan(&keep_row, NULL, SCAN_TEST_MS));
    TEST_ASSERT_EQUAL_UINT(1U, radio_pending());

    /* The owed answer never lands: the storage is not lent again, nothing is posted. */
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_block_wifi_scan(&keep_row, NULL, SCAN_TEST_MS));
    TEST_ASSERT_EQUAL_UINT(1U, radio_pending());
    TEST_ASSERT_EQUAL_UINT(0U, s_kept_n);

    /* It lands at last, tagged with the old generation, inside the next call's wait:
     * that call waits it out, asks again, and offers its own rows alone. */
    fake_sem_wait_hook(&radio_answers_late);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_wifi_scan(&keep_row, NULL, SCAN_TEST_MS));
    TEST_ASSERT_NULL_MESSAGE(s_hook_fault, s_hook_fault);
    TEST_ASSERT_EQUAL_UINT(2U, s_late_step);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_kept_n, "the abandoned answer's rows were offered");
    TEST_ASSERT_EQUAL_STRING("new-0", s_kept[0].ssid);
    TEST_ASSERT_EQUAL_STRING("new-1", s_kept[1].ssid);
    TEST_ASSERT_EQUAL_UINT(0U, radio_pending());
}
