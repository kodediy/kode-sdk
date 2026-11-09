/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_tag.c
 * @brief The tag identity as text, and the per-task block the tag calls keep.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "fakes.h"
#include "khal_system.h"
#include "kode.h"
#include "kode_entry.h"
#include "kode_sys.h"
#include "kode_tag.h"
#include "kode_task.h"

/** @brief Bytes of the sample UID. */
#define TEST_UID_BYTES 3U

/** @brief The sample UID, as a tag reports it. */
static const uint8_t s_uid[TEST_UID_BYTES] = { 0x04U, 0xA2U, 0x3BU };

/** @brief The sample UID as text: three bytes, two separators, the NUL. */
#define TEST_UID_TEXT "04:A2:3B"

/** @brief Bytes of the text of the sample UID, NUL included. */
#define TEST_UID_TEXT_BYTES 9U

/** @brief Bytes of the buffers the parse cases read into. */
#define TEST_PARSE_BYTES 8U

/** @brief Stack of a test task, in bytes. */
#define TEST_STACK_BYTES 8192U

/** @brief Priority of a test task. */
#define TEST_PRIO 1U

/** @brief The longest a case waits for a task, in ms. */
#define TEST_WAIT_MS 4000U

/** @brief The sentinel a refused call must leave in its output. */
#define TEST_SENTINEL 0x5AU

TEST_CASE("a UID is written as upper-case hex bytes joined by colons", "[kode_tag]")
{
    char out[TEST_UID_TEXT_BYTES];

    TEST_ASSERT_TRUE(kode_tag_hex_format(s_uid, TEST_UID_BYTES, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(TEST_UID_TEXT, out);
}

TEST_CASE("a lone byte has no colon and no bytes make an empty text", "[kode_tag]")
{
    char          out[TEST_UID_TEXT_BYTES];
    const uint8_t one = 0x0AU;

    TEST_ASSERT_TRUE(kode_tag_hex_format(&one, 1U, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("0A", out);
    TEST_ASSERT_TRUE(kode_tag_hex_format(NULL, 0U, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

TEST_CASE("a text that does not fit is refused whole", "[kode_tag]")
{
    char out[TEST_UID_TEXT_BYTES];

    (void)memset(out, TEST_SENTINEL, sizeof(out));
    /* One byte short of the text and its NUL. */
    TEST_ASSERT_FALSE(kode_tag_hex_format(s_uid, TEST_UID_BYTES, out, TEST_UID_TEXT_BYTES - 1U));
    TEST_ASSERT_FALSE(kode_tag_hex_format(s_uid, TEST_UID_BYTES, out, 0U));
    TEST_ASSERT_FALSE(kode_tag_hex_format(s_uid, TEST_UID_BYTES, NULL, TEST_UID_TEXT_BYTES));
    TEST_ASSERT_FALSE(kode_tag_hex_format(NULL, TEST_UID_BYTES, out, TEST_UID_TEXT_BYTES));
    /* And the exact fit works. */
    TEST_ASSERT_TRUE(kode_tag_hex_format(s_uid, TEST_UID_BYTES, out, TEST_UID_TEXT_BYTES));
}

TEST_CASE("three spellings of one UID parse to the same bytes", "[kode_tag]")
{
    static const char *const spellings[] = { "04a23b", "04:A2:3B", "04-a2 3b" };

    for ( uint32_t i = 0U; (uint32_t)(sizeof(spellings) / sizeof(spellings[0])) > i; i++ )
    {
        uint8_t  bytes[TEST_PARSE_BYTES] = { 0U };
        uint32_t n = 0U;

        TEST_ASSERT_TRUE_MESSAGE(kode_tag_hex_parse(spellings[i], bytes, sizeof(bytes), &n),
                                 spellings[i]);
        TEST_ASSERT_EQUAL_UINT32(TEST_UID_BYTES, n);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(s_uid, bytes, TEST_UID_BYTES);
    }
}

TEST_CASE("what the formatter writes the parser reads back", "[kode_tag]")
{
    char     text[TEST_UID_TEXT_BYTES];
    uint8_t  bytes[TEST_PARSE_BYTES] = { 0U };
    uint32_t n = 0U;

    TEST_ASSERT_TRUE(kode_tag_hex_format(s_uid, TEST_UID_BYTES, text, sizeof(text)));
    TEST_ASSERT_TRUE(kode_tag_hex_parse(text, bytes, sizeof(bytes), &n));
    TEST_ASSERT_EQUAL_UINT32(TEST_UID_BYTES, n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_uid, bytes, TEST_UID_BYTES);
}

TEST_CASE("an odd digit count, a stray letter and a text too long are refused", "[kode_tag]")
{
    uint8_t  bytes[TEST_UID_BYTES] = { 0U };
    uint32_t n = TEST_SENTINEL;

    TEST_ASSERT_FALSE(kode_tag_hex_parse("4A2", bytes, sizeof(bytes), &n));
    TEST_ASSERT_FALSE(kode_tag_hex_parse("04:A", bytes, sizeof(bytes), &n));
    TEST_ASSERT_FALSE(kode_tag_hex_parse("0G", bytes, sizeof(bytes), &n));
    TEST_ASSERT_FALSE(kode_tag_hex_parse("04:A2:3B:", bytes, 2U, &n));
    TEST_ASSERT_FALSE(kode_tag_hex_parse("04A23B91", bytes, sizeof(bytes), &n));
    TEST_ASSERT_FALSE(kode_tag_hex_parse("0x04", bytes, sizeof(bytes), &n));
    TEST_ASSERT_FALSE(kode_tag_hex_parse("04.A2", bytes, sizeof(bytes), &n));
    /* A separator between the two digits of one byte. */
    TEST_ASSERT_FALSE(kode_tag_hex_parse("0 4:A2", bytes, sizeof(bytes), &n));
    TEST_ASSERT_FALSE(kode_tag_hex_parse("04:A-2", bytes, sizeof(bytes), &n));
    TEST_ASSERT_FALSE(kode_tag_hex_parse("0:4", bytes, sizeof(bytes), &n));
    /* A refusal leaves the count alone. */
    TEST_ASSERT_EQUAL_UINT32(TEST_SENTINEL, n);
}

TEST_CASE("a parse with no text, no room or no count is refused", "[kode_tag]")
{
    uint8_t  bytes[TEST_UID_BYTES] = { 0U };
    uint32_t n = 0U;

    TEST_ASSERT_FALSE(kode_tag_hex_parse(NULL, bytes, sizeof(bytes), &n));
    TEST_ASSERT_FALSE(kode_tag_hex_parse("04", NULL, sizeof(bytes), &n));
    TEST_ASSERT_FALSE(kode_tag_hex_parse("04", bytes, sizeof(bytes), NULL));
}

TEST_CASE("an empty text parses to no bytes", "[kode_tag]")
{
    uint8_t  bytes[TEST_UID_BYTES] = { 0U };
    uint32_t n = TEST_SENTINEL;

    TEST_ASSERT_TRUE(kode_tag_hex_parse("", bytes, sizeof(bytes), &n));
    TEST_ASSERT_EQUAL_UINT32(0U, n);
}

/* ---- The time a call has left --------------------------------------------- */

/** @brief A window the cases ask for, in ms. */
#define TEST_WINDOW_MS 500U

/** @brief A call's whole time in the window cases, in ms. */
#define TEST_CALL_MS 1000U

TEST_CASE("a call with no end has a whole window, whatever the clock says", "[kode_tag]")
{
    fake_time_reset();
    fake_time_advance_ms(TEST_CALL_MS);
    TEST_ASSERT_EQUAL_UINT32(TEST_WINDOW_MS, kode_tag_window(0U, KODE_FOREVER, TEST_WINDOW_MS));
    TEST_ASSERT_EQUAL_UINT32(KODE_FOREVER, kode_tag_window(0U, KODE_FOREVER, KODE_FOREVER));
}

TEST_CASE("the window is what is left of the call, at most the cap", "[kode_tag]")
{
    fake_time_reset();
    const uint32_t start = kode_sys_time_ms();

    TEST_ASSERT_EQUAL_UINT32(TEST_WINDOW_MS, kode_tag_window(start, TEST_CALL_MS, TEST_WINDOW_MS));
    fake_time_advance_ms(700U);
    TEST_ASSERT_EQUAL_UINT32(300U, kode_tag_window(start, TEST_CALL_MS, TEST_WINDOW_MS));
    TEST_ASSERT_EQUAL_UINT32(300U, kode_tag_window(start, TEST_CALL_MS, KODE_FOREVER));
    fake_time_advance_ms(300U);
    TEST_ASSERT_EQUAL_UINT32(0U, kode_tag_window(start, TEST_CALL_MS, TEST_WINDOW_MS));
    fake_time_advance_ms(TEST_CALL_MS);
    TEST_ASSERT_EQUAL_UINT32(0U, kode_tag_window(start, TEST_CALL_MS, TEST_WINDOW_MS));
    TEST_ASSERT_EQUAL_UINT32(0U, kode_tag_window(start, TEST_CALL_MS, KODE_FOREVER));
}

TEST_CASE("the window counts across the wrap of the millisecond clock", "[kode_tag]")
{
    fake_time_reset();
    /* Began 100 ms before the clock wrapped; now 100 ms after. */
    fake_time_advance_ms(100U);
    const uint32_t start = kode_sys_time_ms() - 200U;

    TEST_ASSERT_TRUE(TEST_CALL_MS < start);
    TEST_ASSERT_EQUAL_UINT32(TEST_WINDOW_MS, kode_tag_window(start, 800U, TEST_WINDOW_MS));
    TEST_ASSERT_EQUAL_UINT32(300U, kode_tag_window(start, 500U, TEST_WINDOW_MS));
    TEST_ASSERT_EQUAL_UINT32(0U, kode_tag_window(start, 200U, TEST_WINDOW_MS));
}

/* ---- Letting go of an emulation ------------------------------------------- */

/** @brief The longest the release cases give the emulation, in ms. */
#define TEST_RELEASE_MS 100U

/** @brief The error text of an emulation that is slow to end. */
#define TEST_RELEASING "still letting go"

static kode_status_t s_stop_rc;       /**< What the stop answers.                        */
static uint32_t      s_stop_denied;   /**< Stops that answer DENIED before s_stop_rc.    */
static uint32_t      s_stops;         /**< Stops asked.                                  */
static bool          s_up;            /**< The emulation is up.                          */
static bool          s_stays_up;      /**< An accepted stop does not end it.             */
static uint32_t      s_fails;         /**< Failures reported.                            */
static kode_status_t s_fail_rc;       /**< The last failure reported.                    */

/** @brief The stop of the release cases. */
static kode_status_t emu_stop(void)
{
    kode_status_t rc = KODE_DENIED;

    s_stops++;
    if ( 0U < s_stop_denied )
    {
        s_stop_denied--;
    }
    else
    {
        rc = s_stop_rc;
        if ( (KODE_OK == rc) && !s_stays_up )
        {
            s_up = false;
        }
    }
    return rc;
}

/** @brief Whether the emulation of the release cases is up. */
static bool emu_up(void)
{
    return s_up;
}

/** @brief The failure report of the release cases. */
static void emu_fail(kode_status_t rc)
{
    s_fails++;
    s_fail_rc = rc;
}

/** @brief The release the cases run. */
static const kode_tag_emu_t s_emu = { &emu_stop, &emu_up, &emu_fail, TEST_RELEASING, TEST_RELEASE_MS };

/** @brief Start a release case: the clock at zero, the emulation up, a stop that works. */
static void release_fresh(void)
{
    fake_time_reset();
    s_stop_rc     = KODE_OK;
    s_stop_denied = 0U;
    s_stops       = 0U;
    s_up          = true;
    s_stays_up    = false;
    s_fails       = 0U;
    s_fail_rc     = KODE_OK;
}

TEST_CASE("releasing an emulation that is not up asks for nothing", "[kode_tag]")
{
    release_fresh();
    s_up = false;
    TEST_ASSERT_TRUE(kode_tag_emu_release(&s_emu));
    TEST_ASSERT_EQUAL_UINT32(0U, s_stops);
}

TEST_CASE("a release behind a holder asks again until the stop goes through", "[kode_tag]")
{
    release_fresh();
    s_stop_denied = 3U;
    TEST_ASSERT_TRUE(kode_tag_emu_release(&s_emu));
    TEST_ASSERT_EQUAL_UINT32(4U, s_stops);
    TEST_ASSERT_FALSE(s_up);
    TEST_ASSERT_EQUAL_UINT32(0U, s_fails);
}

TEST_CASE("a stop that times out fails the release at once", "[kode_tag]")
{
    release_fresh();
    s_stop_rc = KODE_TIMEOUT;
    TEST_ASSERT_FALSE(kode_tag_emu_release(&s_emu));
    TEST_ASSERT_EQUAL_UINT32(1U, s_stops);
    TEST_ASSERT_EQUAL_UINT32(1U, s_fails);
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, s_fail_rc);
}

TEST_CASE("a holder that never lets go fails the release within its bound", "[kode_tag]")
{
    release_fresh();
    s_stop_denied = UINT32_MAX;

    const uint32_t before = kode_sys_time_ms();

    TEST_ASSERT_FALSE(kode_tag_emu_release(&s_emu));
    TEST_ASSERT_TRUE((kode_sys_time_ms() - before) <= TEST_RELEASE_MS);
    TEST_ASSERT_EQUAL_UINT32(1U, s_fails);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, s_fail_rc);
    TEST_ASSERT_TRUE(s_up);
}

TEST_CASE("a stop and a wait for the end share one bound", "[kode_tag]")
{
    release_fresh();
    /* The stop is accepted after 30 ms of holder, and the emulation never ends. */
    s_stop_denied = 3U;
    s_stays_up    = true;

    const uint32_t before = kode_sys_time_ms();

    TEST_ASSERT_FALSE(kode_tag_emu_release(&s_emu));
    TEST_ASSERT_EQUAL_STRING(TEST_RELEASING, kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_fails);
    TEST_ASSERT_TRUE((kode_sys_time_ms() - before) <= TEST_RELEASE_MS);
    TEST_ASSERT_TRUE((kode_sys_time_ms() - before) >= (TEST_RELEASE_MS - KODE_RETRY_STEP_MS));
}

/* ---- The tag state of a task ---------------------------------------------- */

TEST_CASE("freeing no tag state does nothing", "[kode_tag]")
{
    kode_tag_state_free(NULL);
}

TEST_CASE("a task gets one tag state and keeps it", "[kode_tag]")
{
    struct kode_tag_state *const a = kode_tag_state_ensure();
    struct kode_tag_state *const b = kode_tag_state_ensure();
    const kode_task_t *const task = kode_task_get();

    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_PTR(a, b);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQUAL_PTR(a, task->tag);
}

/** @brief What the task of a task-end case leaves for the case. */
typedef struct {
    SemaphoreHandle_t done;       /**< Given when the task has noted what it holds. */
    void             *slot;       /**< The task's khal slot (its kode_task_t).      */
    bool              refuse;     /**< PSRAM refuses the state, after the task's own struct. */
    bool              had_state;  /**< The tag state existed.                       */
    uint32_t          allocs;     /**< PSRAM blocks the task took.                  */
} ender_t;

/** @brief A task that takes its tag state and ends. */
static void state_task_ends(void *arg)
{
    ender_t *const e = (ender_t *)arg;
    const uint32_t before = (uint32_t)fake_psram_alloc_count();

    if ( e->refuse )
    {
        fake_psram_refuse_after(1U, 1U);
    }
    e->had_state = (kode_tag_state_ensure() != NULL);
    fake_psram_refuse(0U);
    e->allocs = (uint32_t)fake_psram_alloc_count() - before;
    e->slot   = khal_task_slot_get();
    fake_task_slot_watch(e->slot);
    (void)xSemaphoreGive(e->done);
}

/** @brief Run the task to its end and wait for the slot's clean-up. */
static void run_state_task(ender_t *e)
{
    e->done = xSemaphoreCreateBinary();
    fake_task_slot_watch(NULL);

    const uint32_t allocs_before = (uint32_t)fake_psram_alloc_count();
    const uint32_t frees_before  = (uint32_t)fake_psram_free_count();

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&state_task_ends, "t_tstate",
                                                    TEST_STACK_BYTES, TEST_PRIO, e));
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(e->done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    TEST_ASSERT_NOT_NULL(e->slot);
    TEST_ASSERT_TRUE(fake_task_slot_wait_ended(TEST_WAIT_MS));
    TEST_ASSERT_EQUAL_UINT32(1U, fake_task_slot_watch_frees());

    /* Everything the task and its start took was given back. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)fake_psram_alloc_count() - allocs_before,
                             (uint32_t)fake_psram_free_count() - frees_before);
    fake_task_slot_watch(NULL);
    vSemaphoreDelete(e->done);
}

TEST_CASE("a task's tag state is freed when the task ends", "[kode_tag]")
{
    ender_t e = { NULL, NULL, false, false, 0U };

    run_state_task(&e);
    TEST_ASSERT_TRUE(e.had_state);
    /* The task's struct and the state. */
    TEST_ASSERT_EQUAL_UINT32(2U, e.allocs);
}

TEST_CASE("a task whose tag state PSRAM refuses gets NULL and leaks nothing", "[kode_tag]")
{
    ender_t e = { NULL, NULL, true, true, 0U };

    run_state_task(&e);
    TEST_ASSERT_FALSE(e.had_state);
    TEST_ASSERT_EQUAL_UINT32(1U, e.allocs);
}
