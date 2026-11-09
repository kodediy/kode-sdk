/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_setting.c
 * @brief kode_setting_set() and kode_setting_get() over the real facade and the NVS fake.
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "fakes.h"
#include "khal_nvs.h"
#include "khal_once.h"
#include "khal_system.h"
#include "kode.h"
#include "kode_entry.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_sys_priv.h"
#include "kode_task.h"

#define SET_STACK    8192U  /**< Stack of a test task, in bytes.          */
#define SET_PRIO     1U     /**< Priority of a test task.                 */
#define SET_WAIT_MS  4000U  /**< The longest a case waits for a task, ms. */
#define SET_KEY_OK   15U    /**< Characters of the longest name accepted. */
#define SET_KEY_BAD  16U    /**< Characters of the shortest name refused. */
#define SET_VAL_OK   191U   /**< Characters of the longest value accepted. */
#define SET_VAL_BAD  192U   /**< Characters of the shortest value refused. */
#define SET_BUF      256U   /**< Room for any text a case builds.         */

#define SET_PSRAM_TEXT "settings cannot be used from this task: its stack is in PSRAM; use app_main or a task of your own"

/** @brief A fresh store, real locks and the system marked up. */
static void settings_fresh(void)
{
    fake_mutex_real(true);
    fake_nvs_reset();
    fake_task_stack_in_psram_set(false);
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
}

/** @brief @p n copies of @p c in @p out, then a NUL. */
static void fill(char *out, uint32_t n, char c)
{
    (void)memset(out, (int32_t)c, n);
    out[n] = '\0';
}

TEST_CASE("a setting set comes back from get", "[kode_setting]")
{
    settings_fresh();
    TEST_ASSERT_TRUE(kode_setting_set("name", "Ada"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_STRING("Ada", kode_setting_get("name"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("a second set replaces the first", "[kode_setting]")
{
    settings_fresh();
    TEST_ASSERT_TRUE(kode_setting_set("mode", "one"));
    TEST_ASSERT_TRUE(kode_setting_set("mode", "two"));
    TEST_ASSERT_EQUAL_STRING("two", kode_setting_get("mode"));
}

TEST_CASE("a setting lands in the app's own namespace", "[kode_setting]")
{
    char out[SET_BUF];

    settings_fresh();
    TEST_ASSERT_TRUE(kode_setting_set("room", "kitchen"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_get_str(KODE_NVS_NAMESPACE, "room", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("kitchen", out);

    /* A value put there through the extended layer is read by the standard one. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_nvs_set("lamp", "on"));
    TEST_ASSERT_EQUAL_STRING("on", kode_setting_get("lamp"));
}

TEST_CASE("a setting never set is empty text and says it is not set", "[kode_setting]")
{
    settings_fresh();
    TEST_ASSERT_EQUAL_STRING("", kode_setting_get("nothing"));
    TEST_ASSERT_EQUAL_STRING("setting 'nothing' is not set", kode_error());
}

TEST_CASE("an empty value is a value and is no error", "[kode_setting]")
{
    settings_fresh();
    TEST_ASSERT_TRUE(kode_setting_set("blank", "full"));
    TEST_ASSERT_TRUE(kode_setting_set("blank", ""));
    TEST_ASSERT_EQUAL_STRING("", kode_setting_get("blank"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("names are exact: a capital is another setting", "[kode_setting]")
{
    settings_fresh();
    TEST_ASSERT_TRUE(kode_setting_set("Name", "upper"));
    TEST_ASSERT_TRUE(kode_setting_set("name", "lower"));
    TEST_ASSERT_EQUAL_STRING("upper", kode_setting_get("Name"));
    TEST_ASSERT_EQUAL_STRING("lower", kode_setting_get("name"));
    TEST_ASSERT_EQUAL_STRING("", kode_setting_get("NAME"));
    TEST_ASSERT_EQUAL_STRING("setting 'NAME' is not set", kode_error());
}

TEST_CASE("a 15-character name is accepted and a 16-character one refused", "[kode_setting]")
{
    char key[SET_BUF];
    char want[SET_BUF];

    settings_fresh();
    fill(key, SET_KEY_OK, 'k');
    TEST_ASSERT_TRUE(kode_setting_set(key, "x"));
    TEST_ASSERT_EQUAL_STRING("x", kode_setting_get(key));

    fill(key, SET_KEY_BAD, 'k');
    (void)snprintf(want, sizeof(want), "bad setting name '%.*s'", (int)SET_KEY_BAD, key);
    TEST_ASSERT_FALSE(kode_setting_set(key, "x"));
    TEST_ASSERT_EQUAL_STRING(want, kode_error());
    TEST_ASSERT_EQUAL_STRING("", kode_setting_get(key));
    TEST_ASSERT_EQUAL_STRING(want, kode_error());
}

TEST_CASE("an empty or missing name is refused", "[kode_setting]")
{
    settings_fresh();
    TEST_ASSERT_FALSE(kode_setting_set("", "x"));
    TEST_ASSERT_EQUAL_STRING("bad setting name ''", kode_error());
    TEST_ASSERT_FALSE(kode_setting_set(NULL, "x"));
    TEST_ASSERT_EQUAL_STRING("bad setting name '(null)'", kode_error());
    TEST_ASSERT_EQUAL_STRING("", kode_setting_get(NULL));
    TEST_ASSERT_EQUAL_STRING("bad setting name '(null)'", kode_error());
    TEST_ASSERT_EQUAL_STRING("", kode_setting_get(""));
    TEST_ASSERT_EQUAL_STRING("bad setting name ''", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)fake_nvs_count());
}

TEST_CASE("a 191-character value is accepted and a 192-character one refused", "[kode_setting]")
{
    char value[SET_BUF];

    settings_fresh();
    fill(value, SET_VAL_OK, 'v');
    TEST_ASSERT_TRUE(kode_setting_set("big", value));
    TEST_ASSERT_EQUAL_STRING(value, kode_setting_get("big"));

    fill(value, SET_VAL_BAD, 'w');
    TEST_ASSERT_FALSE(kode_setting_set("big", value));
    TEST_ASSERT_EQUAL_STRING("setting 'big': a value is at most 191 characters", kode_error());
    /* The refusal left the kept value alone. */
    fill(value, SET_VAL_OK, 'v');
    TEST_ASSERT_EQUAL_STRING(value, kode_setting_get("big"));
}

TEST_CASE("a missing value is refused and nothing is kept", "[kode_setting]")
{
    settings_fresh();
    TEST_ASSERT_FALSE(kode_setting_set("none", NULL));
    TEST_ASSERT_EQUAL_STRING("setting 'none' has no value: use \"\" for an empty one", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)fake_nvs_count());
}

TEST_CASE("a failed write is a failed set that says so", "[kode_setting]")
{
    settings_fresh();
    fake_nvs_fail_next_set();
    TEST_ASSERT_FALSE(kode_setting_set("flaky", "x"));
    TEST_ASSERT_EQUAL_STRING("setting 'flaky' could not be kept", kode_error());
    TEST_ASSERT_TRUE(kode_setting_set("flaky", "x"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("a task whose stack is in PSRAM cannot set or get and is told", "[kode_setting]")
{
    settings_fresh();
    TEST_ASSERT_TRUE(kode_setting_set("safe", "kept"));
    fake_task_stack_in_psram_set(true);

    TEST_ASSERT_FALSE(kode_setting_set("safe", "changed"));
    TEST_ASSERT_EQUAL_STRING(SET_PSRAM_TEXT, kode_error());
    TEST_ASSERT_EQUAL_STRING("", kode_setting_get("safe"));
    TEST_ASSERT_EQUAL_STRING(SET_PSRAM_TEXT, kode_error());

    fake_task_stack_in_psram_set(false);
    TEST_ASSERT_EQUAL_STRING("kept", kode_setting_get("safe"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("a second get replaces the first one's text", "[kode_setting]")
{
    const char *first;
    const char *second;

    settings_fresh();
    TEST_ASSERT_TRUE(kode_setting_set("a", "alpha"));
    TEST_ASSERT_TRUE(kode_setting_set("b", "beta"));
    first = kode_setting_get("a");
    TEST_ASSERT_EQUAL_STRING("alpha", first);
    second = kode_setting_get("b");
    TEST_ASSERT_EQUAL_STRING("beta", second);
    /* One block per task: the first text is now the second. */
    TEST_ASSERT_EQUAL_STRING("beta", first);
}

TEST_CASE("a get that fails after one that worked is empty text, not the old one", "[kode_setting]")
{
    settings_fresh();
    TEST_ASSERT_TRUE(kode_setting_set("a", "alpha"));
    TEST_ASSERT_EQUAL_STRING("alpha", kode_setting_get("a"));
    TEST_ASSERT_EQUAL_STRING("", kode_setting_get("gone"));
}

/* ---- The task's state goes with the task ----------------------------------- */

/** @brief What the task of a task-end case leaves for the case. */
typedef struct {
    SemaphoreHandle_t done;      /**< Given when the task has noted what it holds. */
    void             *slot;      /**< The task's khal slot (its kode_task_t).      */
    bool              refuse;    /**< PSRAM refuses the settings state after the task's own struct. */
    char              text[8];   /**< The value read, copied.                      */
    char              error[KODE_ERROR_MAX];  /**< The error after the read.       */
    uint32_t          allocs;    /**< PSRAM blocks the task took.                  */
} ender_t;

/** @brief A task that sets once, reads once and ends. */
static void setting_task_ends(void *arg)
{
    ender_t *const e      = (ender_t *)arg;
    const uint32_t before = (uint32_t)fake_psram_alloc_count();
    const char    *text   = NULL;

    (void)kode_setting_set("task", "seen");
    if ( e->refuse )
    {
        fake_psram_refuse_after(1U, 1U);
    }
    text = kode_setting_get("task");
    fake_psram_refuse(0U);
    (void)snprintf(e->text, sizeof(e->text), "%s", text);
    (void)snprintf(e->error, sizeof(e->error), "%s", kode_error());
    e->allocs = (uint32_t)fake_psram_alloc_count() - before;
    e->slot   = khal_task_slot_get();
    fake_task_slot_watch(e->slot);
    (void)xSemaphoreGive(e->done);
}

/** @brief Run the task to its end and wait for the slot's clean-up. */
static void run_setting_task(ender_t *e)
{
    e->done = xSemaphoreCreateBinary();
    fake_task_slot_watch(NULL);

    const uint32_t allocs_before = (uint32_t)fake_psram_alloc_count();
    const uint32_t frees_before  = (uint32_t)fake_psram_free_count();

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&setting_task_ends, "t_set", SET_STACK, SET_PRIO, e));
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(e->done, pdMS_TO_TICKS(SET_WAIT_MS)));
    TEST_ASSERT_NOT_NULL(e->slot);
    TEST_ASSERT_TRUE(fake_task_slot_wait_ended(SET_WAIT_MS));
    TEST_ASSERT_EQUAL_UINT32(1U, fake_task_slot_watch_frees());

    /* Everything the task and its start took was given back. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)fake_psram_alloc_count() - allocs_before,
                             (uint32_t)fake_psram_free_count() - frees_before);
    fake_task_slot_watch(NULL);
    vSemaphoreDelete(e->done);
}

TEST_CASE("a task's settings state is freed when the task ends", "[kode_setting]")
{
    ender_t e = { NULL, NULL, false, "", "", 0U };

    settings_fresh();
    run_setting_task(&e);
    TEST_ASSERT_EQUAL_STRING("seen", e.text);
    TEST_ASSERT_EQUAL_STRING("", e.error);
    /* The task's struct and the settings state. */
    TEST_ASSERT_EQUAL_UINT32(2U, e.allocs);
}

TEST_CASE("a task whose settings state PSRAM refuses gets empty text and leaks nothing", "[kode_setting]")
{
    ender_t e = { NULL, NULL, true, "", "", 0U };

    settings_fresh();
    run_setting_task(&e);
    TEST_ASSERT_EQUAL_STRING("", e.text);
    TEST_ASSERT_EQUAL_STRING("setting 'task': no memory to read it", e.error);
    /* The task's own struct alone. */
    TEST_ASSERT_EQUAL_UINT32(1U, e.allocs);
}
