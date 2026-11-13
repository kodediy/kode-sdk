/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_file.c
 * @brief The standard layer's files below the calls: paths, busy retry, status texts, task state.
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "qpc.h"

#include "ao_spy.h"
#include "fake_khal_worker.h"
#include "fakes.h"
#include "kaos.h"
#include "kbsp_owners.h"
#include "kdrv_storage.h"
#include "khal_system.h"
#include "kode.h"
#include "kode_entry.h"
#include "kode_file.h"
#include "kode_screen_priv.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_sys_priv.h"
#include "kode_task.h"
#include "kqp.h"
#include "probe_ao.h"
#include "qf_support.h"

/** @brief Bytes of the buffer a path is turned into: the extended layer's own. */
#define TEST_OUT_BYTES KODE_PATH_MAX

/** @brief Stack of a test task, in bytes. */
#define TEST_STACK_BYTES 8192U

/** @brief Priority of a test task. */
#define TEST_PRIO 1U

/** @brief The longest a case waits for a task, in ms. */
#define TEST_WAIT_MS 4000U

/** @brief Bytes of the buffer a long path is built in. */
#define TEST_LONG_BYTES 128U

/* ---- kode_file_path -------------------------------------------------------- */

/** @brief Turn @p user into a volume path and expect it to be @p want. */
static void expect_path(const char *user, const char *want)
{
    char out[TEST_OUT_BYTES];

    (void)memset(out, 'x', sizeof(out));
    TEST_ASSERT_TRUE_MESSAGE(kode_file_path(user, out, sizeof(out)), user);
    TEST_ASSERT_EQUAL_STRING(want, out);
}

/** @brief Expect @p user to be refused with the error @p want. */
static void expect_refused(const char *user, const char *want)
{
    char out[TEST_OUT_BYTES];

    (void)memset(out, 'x', sizeof(out));
    kode_err_set("%s", "");
    TEST_ASSERT_FALSE(kode_file_path(user, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(want, kode_error());
}

TEST_CASE("a path on sd becomes a path under /sdcard", "[kode_file]")
{
    expect_path("sd/a.txt", "/sdcard/a.txt");
    expect_path("sd/dir/sub/b.csv", "/sdcard/dir/sub/b.csv");
}

TEST_CASE("a path on flash becomes a path under /storage", "[kode_file]")
{
    expect_path("flash/x", "/storage/x");
    expect_path("flash/dir/y.bin", "/storage/dir/y.bin");
}

TEST_CASE("the volume name takes any case", "[kode_file]")
{
    expect_path("FLASH/x", "/storage/x");
    expect_path("Sd/x", "/sdcard/x");
    expect_path("sD/x", "/sdcard/x");
    expect_path("fLaSh", "/storage");
}

TEST_CASE("the bare volume and a trailing slash name the root", "[kode_file]")
{
    expect_path("sd", "/sdcard");
    expect_path("sd/", "/sdcard");
    expect_path("flash", "/storage");
    expect_path("flash/", "/storage");
    expect_path("sd/dir/", "/sdcard/dir");
}

TEST_CASE("the rest of the path keeps its case", "[kode_file]")
{
    expect_path("sd/Data/Log.TXT", "/sdcard/Data/Log.TXT");
}

TEST_CASE("a path of the longest length is accepted whole", "[kode_file]")
{
    char user[TEST_LONG_BYTES];
    char want[TEST_LONG_BYTES];

    /* 90 characters: the most KODE_FILE_PATH_MAX holds with its NUL. */
    (void)memset(user, 'a', sizeof(user));
    (void)memcpy(user, "sd/", 3U);
    user[KODE_FILE_PATH_MAX - 1U] = '\0';
    (void)memcpy(want, "/sdcard/", 8U);
    (void)memset(&want[8], 'a', (KODE_FILE_PATH_MAX - 1U) - 3U);
    want[8U + ((KODE_FILE_PATH_MAX - 1U) - 3U)] = '\0';
    expect_path(user, want);
}

TEST_CASE("a path that is NULL or empty is refused", "[kode_file]")
{
    expect_refused(NULL, "bad path '(null)': start with sd/ or flash/");
    expect_refused("", "bad path '': start with sd/ or flash/");
}

TEST_CASE("a path that starts with a slash is refused", "[kode_file]")
{
    expect_refused("/sd/a", "bad path '/sd/a': start with sd/ or flash/");
    expect_refused("/", "bad path '/': start with sd/ or flash/");
    expect_refused("/sdcard/a", "bad path '/sdcard/a': start with sd/ or flash/");
}

TEST_CASE("another volume, or a name that only starts like one, is refused", "[kode_file]")
{
    expect_refused("usb/a", "bad path 'usb/a': start with sd/ or flash/");
    expect_refused("sdx/a", "bad path 'sdx/a': start with sd/ or flash/");
    expect_refused("flashy", "bad path 'flashy': start with sd/ or flash/");
    expect_refused("s", "bad path 's': start with sd/ or flash/");
    expect_refused("a.txt", "bad path 'a.txt': start with sd/ or flash/");
}

TEST_CASE("an empty part, a dot or a double dot is refused", "[kode_file]")
{
    expect_refused("sd//a", "bad path 'sd//a': no empty, '.' or '..' part");
    expect_refused("sd/a//b", "bad path 'sd/a//b': no empty, '.' or '..' part");
    expect_refused("sd//", "bad path 'sd//': no empty, '.' or '..' part");
    expect_refused("sd/../x", "bad path 'sd/../x': no empty, '.' or '..' part");
    expect_refused("sd/./x", "bad path 'sd/./x': no empty, '.' or '..' part");
    expect_refused("flash/a/..", "bad path 'flash/a/..': no empty, '.' or '..' part");
    expect_refused("flash/.", "bad path 'flash/.': no empty, '.' or '..' part");
}

TEST_CASE("dots inside a name are no part of their own", "[kode_file]")
{
    expect_path("sd/.hidden", "/sdcard/.hidden");
    expect_path("sd/a..b", "/sdcard/a..b");
    expect_path("sd/...", "/sdcard/...");
}

TEST_CASE("a path of KODE_FILE_PATH_MAX characters is too long", "[kode_file]")
{
    char user[TEST_LONG_BYTES];
    char want[TEST_LONG_BYTES + 32U];

    (void)memset(user, 'a', sizeof(user));
    (void)memcpy(user, "sd/", 3U);
    user[KODE_FILE_PATH_MAX] = '\0';

    (void)snprintf(want, sizeof(want), "path '%s' is too long", user);
    expect_refused(user, want);
}

TEST_CASE("a buffer too small for the result is a path too long", "[kode_file]")
{
    char out[8];

    kode_err_set("%s", "");
    TEST_ASSERT_FALSE(kode_file_path("sd/abcdef", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("path 'sd/abcdef' is too long", kode_error());

    /* "/sdcard" and its NUL are 8 bytes: the bare volume just fits. */
    TEST_ASSERT_TRUE(kode_file_path("sd", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/sdcard", out);

    TEST_ASSERT_FALSE(kode_file_path("flash", out, sizeof(out)));
    TEST_ASSERT_FALSE(kode_file_path("sd", out, 0U));
    TEST_ASSERT_FALSE(kode_file_path("sd", NULL, sizeof(out)));
}

TEST_CASE("the longest accepted path still fits the extended layer's buffer", "[kode_file]")
{
    char user[TEST_LONG_BYTES];
    char out[TEST_OUT_BYTES];

    /* "sd" grows by five bytes, the most any volume does. */
    (void)memset(user, 'a', sizeof(user));
    (void)memcpy(user, "sd/", 3U);
    user[KODE_FILE_PATH_MAX - 1U] = '\0';
    TEST_ASSERT_TRUE(kode_file_path(user, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT32((KODE_FILE_PATH_MAX - 1U) + 5U, (uint32_t)strlen(out));
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(TEST_OUT_BYTES, (uint32_t)strlen(out) + 1U);
}

/* ---- kode_file_sys --------------------------------------------------------- */

/** @brief What a fake storage call counts and answers. */
typedef struct {
    uint32_t      calls;      /**< Times the op ran.                          */
    uint32_t      denied;     /**< Answers KODE_DENIED this many times first;
                               *   UINT32_MAX never stops.                    */
    kode_status_t then;       /**< What it answers after those.               */
} fake_op_t;

/** @brief A storage call that is busy for a while, then answers. */
static kode_status_t fake_op(void *ctx)
{
    fake_op_t *const f = (fake_op_t *)ctx;

    f->calls++;
    if ( 0U == f->denied )
    {
        return f->then;
    }
    if ( UINT32_MAX != f->denied )
    {
        f->denied--;
    }
    return KODE_DENIED;
}

TEST_CASE("a file call retried after a denial goes through", "[kode_file]")
{
    fake_op_t f = { 0U, 2U, KODE_OK };

    fake_time_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_file_sys(&fake_op, &f));
    TEST_ASSERT_EQUAL_UINT32(3U, f.calls);

    /* Two sleeps of one step each: nothing else moved the fake clock. */
    TEST_ASSERT_EQUAL_UINT32(2U * KODE_RETRY_STEP_MS, kode_sys_time_ms());
}

TEST_CASE("a file call that is always denied gives up after the bound", "[kode_file]")
{
    fake_op_t f = { 0U, UINT32_MAX, KODE_OK };

    fake_time_reset();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_file_sys(&fake_op, &f));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_FILE_BUSY_MS, kode_sys_time_ms());
    TEST_ASSERT_GREATER_THAN_UINT32(1U, f.calls);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32((KODE_FILE_BUSY_MS / KODE_RETRY_STEP_MS) + 1U, f.calls);
}

TEST_CASE("an answer other than denied is returned at once", "[kode_file]")
{
    const kode_status_t others[] = { KODE_OK, KODE_ERROR, KODE_TIMEOUT, KODE_NO_RESOURCES,
                                     KODE_IO_ERROR, KODE_EXISTS, KODE_NOT_DIR, KODE_IS_DIR };

    for ( size_t i = 0U; (sizeof(others) / sizeof(others[0])) > i; i++ )
    {
        fake_op_t f = { 0U, 0U, others[i] };

        fake_time_reset();
        TEST_ASSERT_EQUAL_INT(others[i], kode_file_sys(&fake_op, &f));
        TEST_ASSERT_EQUAL_UINT32(1U, f.calls);
        TEST_ASSERT_EQUAL_UINT32(0U, kode_sys_time_ms());
    }
}

TEST_CASE("the last answer after the busy spell is the one returned", "[kode_file]")
{
    fake_op_t f = { 0U, 1U, KODE_IO_ERROR };

    fake_time_reset();
    TEST_ASSERT_EQUAL_INT(KODE_IO_ERROR, kode_file_sys(&fake_op, &f));
    TEST_ASSERT_EQUAL_UINT32(2U, f.calls);
}

TEST_CASE("a file call without an op is an error and tries nothing", "[kode_file]")
{
    fake_time_reset();
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kode_file_sys(NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(0U, kode_sys_time_ms());
}

/* ---- kode_file_why --------------------------------------------------------- */

TEST_CASE("each status a file call can fail with has its phrase", "[kode_file]")
{
    TEST_ASSERT_EQUAL_STRING("storage is busy", kode_file_why(KODE_DENIED));
    TEST_ASSERT_EQUAL_STRING("the volume is not there or the call failed",
                             kode_file_why(KODE_ERROR));
    TEST_ASSERT_EQUAL_STRING("storage did not answer", kode_file_why(KODE_TIMEOUT));
    TEST_ASSERT_EQUAL_STRING("out of memory", kode_file_why(KODE_NO_RESOURCES));
    TEST_ASSERT_EQUAL_STRING("a folder in the path is a file", kode_file_why(KODE_NOT_DIR));
    TEST_ASSERT_EQUAL_STRING("it is a folder", kode_file_why(KODE_IS_DIR));
    TEST_ASSERT_EQUAL_STRING("already exists", kode_file_why(KODE_EXISTS));
    TEST_ASSERT_EQUAL_STRING("the volume refused the write", kode_file_why(KODE_IO_ERROR));
}

TEST_CASE("a status with no phrase of its own still has one", "[kode_file]")
{
    const char *const ok  = kode_file_why(KODE_OK);
    const char *const odd = kode_file_why((kode_status_t)999);

    TEST_ASSERT_NOT_NULL(ok);
    TEST_ASSERT_NOT_NULL(odd);
    TEST_ASSERT_NOT_EQUAL_CHAR('\0', odd[0]);
    TEST_ASSERT_EQUAL_STRING(ok, odd);
}

/* ---- The files state of a task --------------------------------------------- */

TEST_CASE("freeing no state does nothing", "[kode_file]")
{
    kode_file_state_free(NULL);
}

TEST_CASE("a task gets one files state and keeps it", "[kode_file]")
{
    struct kode_file_state *const a = kode_file_state_ensure();
    struct kode_file_state *const b = kode_file_state_ensure();
    const kode_task_t *const task = kode_task_get();

    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_PTR(a, b);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQUAL_PTR(a, task->file);
}

/** @brief What the task of a task-end case leaves for the case. */
typedef struct {
    SemaphoreHandle_t done;      /**< Given when the task has noted what it holds. */
    void             *slot;      /**< The task's khal slot (its kode_task_t).      */
    bool              refuse;    /**< PSRAM refuses the state, after the task's own struct. */
    bool              had_state; /**< The files state existed.                     */
    uint32_t          allocs;    /**< PSRAM blocks the task took.                  */
} ender_t;

/** @brief A task that takes its files state and ends. */
static void state_task_ends(void *arg)
{
    ender_t *const e = (ender_t *)arg;
    const uint32_t before = (uint32_t)fake_psram_alloc_count();

    if ( e->refuse )
    {
        fake_psram_refuse_after(1U, 1U);
    }
    e->had_state = (kode_file_state_ensure() != NULL);
    fake_psram_refuse(0U);
    e->allocs = (uint32_t)fake_psram_alloc_count() - before;
    e->slot   = khal_task_slot_get();
    fake_task_slot_watch(e->slot);
    (void)xSemaphoreGive(e->done);
}

/** @brief Run @p entry to its end and wait for the slot's clean-up. */
static void run_state_task(ender_t *e, void (*entry)(void *arg))
{
    e->done = xSemaphoreCreateBinary();
    fake_task_slot_watch(NULL);

    const uint32_t allocs_before = (uint32_t)fake_psram_alloc_count();
    const uint32_t frees_before  = (uint32_t)fake_psram_free_count();

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(entry, "t_fstate",
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

TEST_CASE("a task's files state is freed when the task ends", "[kode_file]")
{
    ender_t e = { NULL, NULL, false, false, 0U };

    run_state_task(&e, &state_task_ends);
    TEST_ASSERT_TRUE(e.had_state);
    /* The task's struct and the state. */
    TEST_ASSERT_EQUAL_UINT32(2U, e.allocs);
}

TEST_CASE("a task whose files state PSRAM refuses gets NULL and leaks nothing", "[kode_file]")
{
    ender_t e = { NULL, NULL, true, true, 0U };

    run_state_task(&e, &state_task_ends);
    TEST_ASSERT_FALSE(e.had_state);
    TEST_ASSERT_EQUAL_UINT32(1U, e.allocs);
}

/* ---- The whole-file calls -------------------------------------------------- */

/** @brief The extended layer's ceiling for one call, as a count of bytes. */
#define TEST_PART KODE_SYS_FILE_READ_MAX

/** @brief The size of the binary file of the byte-exact case. */
#define TEST_BIG_BYTES 100000U

/** @brief The size of the text of the multi-part write cases. */
#define TEST_TEXT_BYTES 70000U

/** @brief The bytes of the put_bytes case repeat after this many, and it writes, appends and then replaces with these lengths. */
#define TEST_BYTE_PERIOD 7U

/** @brief The bytes the put_bytes case appends. */
#define TEST_APPEND_BYTES 3U

/** @brief The bytes the put_bytes case replaces the file with. */
#define TEST_REPLACE_BYTES 2U

/** @brief The size of a file that grows between two parts. */
#define TEST_GROW_BYTES 40000U

/** @brief Bytes a hook adds to a file that grows. */
#define TEST_GROWTH 10U

/** @brief Bytes a hook leaves in a file that shrinks. */
#define TEST_SHRUNK 100U

/** @brief The byte at position i of the patterned files: i % TEST_MOD. */
#define TEST_MOD 251U

/** @brief Bytes of a host path. */
#define TEST_HOST_PATH 512U

/** @brief The error text of a file over the ceiling. */
#define TEST_TOO_BIG "flash/huge.bin: too big (over 4 MiB)"

/** @brief The task's file-state blocks that stay while the task lives. */
#define TEST_NO_BLOCKS 0U

/** @brief What a hook that runs inside a wait could not do. */
static const char *s_file_fault;

/** @brief The wait a changing hook acts after, counted from 1. */
static uint32_t s_change_at;

/** @brief Waits seen by the changing hook. */
static uint32_t s_waits;

/** @brief What the changing hook does to the file. */
static bool (*s_change)(void);

/** @brief Whether the change landed. */
static bool s_change_ok;

/** @brief Drop what sits, undispatched, in @p ao's queue. */
static void file_drain(QActive *ao)
{
    const QEvt *e;

    while ( (NULL != ao->eQueue) && (xQueueReceive(ao->eQueue, &e, 0U) == pdTRUE) )
    {
        QF_gc(e);
    }
}

/** @brief Let AO_Storage answer what is queued, and its worker. */
static void file_settle_once(void)
{
    (void)qf_support_pump(AO_Storage, 8U);
    while ( fake_khal_worker_run_next_for("storage_sd") )
    {
        (void)qf_support_pump(AO_Storage, 8U);
    }
}

static void file_settle_and_rearm(void);

/** @brief Arm the settle hook for the next round trip of a call. */
static void file_will_settle(void)
{
    fake_sem_wait_hook(&file_settle_and_rearm);
}

/** @brief Settle one round trip and stay armed for the next. */
static void file_settle_and_rearm(void)
{
    file_settle_once();
    file_will_settle();
}

/** @brief Disarm the hook: the next wait finds nobody home. */
static void file_no_settle(void)
{
    fake_sem_wait_hook(NULL);
}

/** @brief A hook that settles, then does its change after the wait it is set for. */
static void file_change_hook(void)
{
    file_settle_once();
    s_waits++;
    if ( (s_waits == s_change_at) && (NULL != s_change) )
    {
        s_change_ok = s_change();
    }
    fake_sem_wait_hook(&file_change_hook);
}

/** @brief A clean AO_Storage over empty media, the system marked up. */
static void file_fresh(void)
{
    kode_sys_file_status_t st;

    /* The files lock is made once, on the first call of the binary: make it a real one. */
    fake_mutex_real(true);
    (void)kdrv_storage_close();
    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();
    qf_support_reap_timers();
    Storage_ctor();
    qf_support_give_queue(AO_Storage);
    AO_Storage->prio  = KODE_PRIO_STORAGE;
    AO_Storage->pthre = KODE_PRIO_STORAGE;
    QASM_INIT(AO_Storage, (void *)0, 0U);
    kbsp_release(KBSP_RES_SDMMC, AO_Storage);
    kbsp_release(KBSP_RES_FATFS, AO_Storage);
    file_drain(AO_Storage);

    fake_media_reset();
    fake_sem_reset();
    fake_time_reset();
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    s_file_fault = NULL;
    s_change     = NULL;
    s_change_ok  = false;
    s_waits      = 0U;

    /* One ordinary call proves the facade is clean, and takes its staging buffer. */
    file_will_settle();
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_block_file_status("/storage/barrier.none", &st),
                                  "an earlier case left the file facade dirty");
    file_drain(AO_Storage);
    fake_sem_reset();
    fake_time_reset();
    file_will_settle();
}

/** @brief The path of @p name in the directory of the internal volume or the card. */
static void host_path(char *out, uint32_t cap, const char *dir, const char *name)
{
    (void)snprintf(out, cap, "%s/%s", dir, name);
}

/** @brief The size of a host file; UINT32_MAX when it is not there. */
static uint32_t host_size(const char *dir, const char *name)
{
    char     path[TEST_HOST_PATH];
    uint32_t size = UINT32_MAX;
    FILE    *f;

    host_path(path, sizeof(path), dir, name);
    f = fopen(path, "rb");
    if ( NULL != f )
    {
        (void)fseek(f, 0, SEEK_END);
        size = (uint32_t)ftell(f);
        (void)fclose(f);
    }
    return size;
}

/**
 * @brief Leave a case that needs the board's case folding when the host has none.
 *
 * The board's FAT finds b.txt under B.TXT. The host folder the fake volume
 * lives in does the same on macOS and not on Linux, so there the case cannot
 * be told and is ignored. Call it once b.txt is written.
 */
static void need_case_folding(void)
{
    if ( UINT32_MAX == host_size(fake_media_fs_dir(), "B.TXT") )
    {
        TEST_IGNORE_MESSAGE("the host's folder tells B.TXT from b.txt; the board's FAT does not");
    }
}

/** @brief Write @p n bytes of the pattern to a host file, from position @p first. */
static bool host_pattern(const char *name, uint32_t first, uint32_t n, const char *mode)
{
    char  path[TEST_HOST_PATH];
    FILE *f;
    bool  ok = true;

    host_path(path, sizeof(path), fake_media_fs_dir(), name);
    f = fopen(path, mode);
    if ( NULL == f )
    {
        return false;
    }
    for ( uint32_t i = 0U; ok && (i < n); i++ )
    {
        ok = (fputc((int)((first + i) % TEST_MOD), f) != EOF);
    }
    (void)fclose(f);
    return ok;
}

/** @brief Make a host file of @p n zero bytes without writing them. */
static bool host_sparse(const char *name, uint32_t n)
{
    char  path[TEST_HOST_PATH];
    FILE *f;
    bool  ok;

    host_path(path, sizeof(path), fake_media_fs_dir(), name);
    f = fopen(path, "wb");
    if ( NULL == f )
    {
        return false;
    }
    ok = (fseek(f, (long)(n - 1U), SEEK_SET) == 0) && (fputc(0, f) != EOF);
    (void)fclose(f);
    return ok;
}

/** @brief Whether @p n bytes at @p buf are the pattern from position @p first. */
static bool pattern_is(const uint8_t *buf, uint32_t first, uint32_t n)
{
    bool ok = true;

    for ( uint32_t i = 0U; ok && (i < n); i++ )
    {
        ok = ((uint8_t)((first + i) % TEST_MOD) == buf[i]);
    }
    return ok;
}

/** @brief PSRAM blocks live now: taken and not given back. */
static uint32_t psram_live(void)
{
    return (uint32_t)fake_psram_alloc_count() - (uint32_t)fake_psram_free_count();
}

/** @brief Put @p n pattern bytes at @p path through the extended layer, a part at a time. */
static bool sys_put_pattern(const char *path, uint32_t n)
{
    uint8_t *const buf  = (uint8_t *)malloc(n);
    bool           ok   = (NULL != buf);
    uint32_t       done = 0U;

    while ( ok && (done < n) )
    {
        uint32_t part = n - done;

        if ( TEST_PART < part )
        {
            part = TEST_PART;
        }
        for ( uint32_t i = 0U; i < part; i++ )
        {
            buf[done + i] = (uint8_t)((done + i) % TEST_MOD);
        }
        ok = (kode_block_file_write_buffer(path, &buf[done], part, 0U < done) == KODE_OK);
        done += part;
    }
    free(buf);
    return ok;
}

TEST_CASE("a text written to flash reads back", "[kode_file]")
{
    file_fresh();
    TEST_ASSERT_TRUE(kode_file_write("flash/h.txt", "hello"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(5U, host_size(fake_media_fs_dir(), "h.txt"));
    TEST_ASSERT_EQUAL_STRING("hello", kode_file_read("flash/h.txt"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    file_no_settle();
}

TEST_CASE("a text written to the card reads back", "[kode_file]")
{
    file_fresh();
    fake_media_set_sd_present(true);
    TEST_ASSERT_TRUE(kode_file_write("sd/h.txt", "hello"));
    TEST_ASSERT_EQUAL_UINT32(5U, host_size(fake_media_sd_dir(), "h.txt"));
    TEST_ASSERT_EQUAL_STRING("hello", kode_file_read("sd/h.txt"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    file_no_settle();
}

TEST_CASE("a missing file reads as nothing and says so", "[kode_file]")
{
    uint32_t len = 77U;

    file_fresh();
    TEST_ASSERT_EQUAL_STRING("", kode_file_read("flash/nope.txt"));
    TEST_ASSERT_EQUAL_STRING("flash/nope.txt: no such file", kode_error());

    TEST_ASSERT_NULL(kode_file_read_bytes("flash/nope.bin", &len));
    TEST_ASSERT_EQUAL_UINT32(0U, len);
    TEST_ASSERT_EQUAL_STRING("flash/nope.bin: no such file", kode_error());
    TEST_ASSERT_NULL(kode_file_read_bytes("flash/nope.bin", NULL));
    file_no_settle();
}

TEST_CASE("a bad path is refused by the read and the write alike", "[kode_file]")
{
    uint32_t len = 77U;

    file_fresh();
    TEST_ASSERT_EQUAL_STRING("", kode_file_read("usb/a"));
    TEST_ASSERT_EQUAL_STRING("bad path 'usb/a': start with sd/ or flash/", kode_error());
    TEST_ASSERT_NULL(kode_file_read_bytes("/sd/a", &len));
    TEST_ASSERT_EQUAL_UINT32(0U, len);
    TEST_ASSERT_FALSE(kode_file_write("sd/../a", "x"));
    TEST_ASSERT_EQUAL_STRING("bad path 'sd/../a': no empty, '.' or '..' part", kode_error());
    TEST_ASSERT_FALSE(kode_file_append(NULL, "x"));
    TEST_ASSERT_EQUAL_STRING("bad path '(null)': start with sd/ or flash/", kode_error());
    file_no_settle();
}

TEST_CASE("a file of 100,000 bytes reads back byte-exact", "[kode_file]")
{
    const uint8_t *bytes;
    uint32_t       len = 0U;

    file_fresh();
    TEST_ASSERT_TRUE(sys_put_pattern("/storage/big.bin", TEST_BIG_BYTES));
    TEST_ASSERT_EQUAL_UINT32(TEST_BIG_BYTES, host_size(fake_media_fs_dir(), "big.bin"));

    bytes = kode_file_read_bytes("flash/big.bin", &len);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT32(TEST_BIG_BYTES, len);
    TEST_ASSERT_TRUE(pattern_is(bytes, 0U, len));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    file_no_settle();
}

TEST_CASE("a file whose size is a multiple of the part reads whole", "[kode_file]")
{
    const uint8_t *bytes;
    uint32_t       len = 0U;

    file_fresh();
    TEST_ASSERT_TRUE(host_pattern("two.bin", 0U, 2U * TEST_PART, "wb"));
    bytes = kode_file_read_bytes("flash/two.bin", &len);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT32(2U * TEST_PART, len);
    TEST_ASSERT_TRUE(pattern_is(bytes, 0U, len));
    file_no_settle();
}

TEST_CASE("a file with a NUL in it reads as text up to it and as bytes whole", "[kode_file]")
{
    const uint8_t *bytes;
    uint32_t       len = 0U;

    file_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_write_buffer("/storage/n.bin", "a\0b", 3U, false));
    TEST_ASSERT_EQUAL_STRING("a", kode_file_read("flash/n.bin"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    bytes = kode_file_read_bytes("flash/n.bin", &len);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT32(3U, len);
    TEST_ASSERT_EQUAL_UINT8('a', bytes[0]);
    TEST_ASSERT_EQUAL_UINT8(0U, bytes[1]);
    TEST_ASSERT_EQUAL_UINT8('b', bytes[2]);
    file_no_settle();
}

TEST_CASE("an append adds to what a write left", "[kode_file]")
{
    file_fresh();
    TEST_ASSERT_TRUE(kode_file_write("flash/w.txt", "abc"));
    TEST_ASSERT_TRUE(kode_file_append("flash/w.txt", "de"));
    TEST_ASSERT_EQUAL_STRING("abcde", kode_file_read("flash/w.txt"));

    /* A write replaces what an append built. */
    TEST_ASSERT_TRUE(kode_file_write("flash/w.txt", "z"));
    TEST_ASSERT_EQUAL_STRING("z", kode_file_read("flash/w.txt"));
    file_no_settle();
}

TEST_CASE("an append creates a file that is not there", "[kode_file]")
{
    file_fresh();
    TEST_ASSERT_TRUE(kode_file_append("flash/new.txt", "first"));
    TEST_ASSERT_EQUAL_STRING("first", kode_file_read("flash/new.txt"));
    file_no_settle();
}

TEST_CASE("writing an empty text leaves a file of no bytes", "[kode_file]")
{
    const uint8_t *bytes;
    uint32_t       len = 77U;

    file_fresh();
    TEST_ASSERT_TRUE(kode_file_write("flash/e.txt", "something"));
    TEST_ASSERT_TRUE(kode_file_write("flash/e.txt", ""));
    TEST_ASSERT_EQUAL_UINT32(0U, host_size(fake_media_fs_dir(), "e.txt"));

    /* An empty file reads as an empty text, and no failure. */
    TEST_ASSERT_EQUAL_STRING("", kode_file_read("flash/e.txt"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    bytes = kode_file_read_bytes("flash/e.txt", &len);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT32(0U, len);
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    /* An empty write creates a file that is not there. */
    TEST_ASSERT_TRUE(kode_file_write("flash/e2.txt", ""));
    TEST_ASSERT_EQUAL_UINT32(0U, host_size(fake_media_fs_dir(), "e2.txt"));
    file_no_settle();
}

TEST_CASE("a text of several parts is written and appended whole", "[kode_file]")
{
    char          *text = (char *)malloc(TEST_TEXT_BYTES + 1U);
    const uint8_t *bytes;
    uint32_t       len = 0U;

    TEST_ASSERT_NOT_NULL(text);
    for ( uint32_t i = 0U; TEST_TEXT_BYTES > i; i++ )
    {
        text[i] = (char)('a' + (char)(i % 26U));
    }
    text[TEST_TEXT_BYTES] = '\0';

    file_fresh();
    TEST_ASSERT_TRUE(kode_file_write("flash/long.txt", text));
    TEST_ASSERT_EQUAL_UINT32(TEST_TEXT_BYTES, host_size(fake_media_fs_dir(), "long.txt"));
    TEST_ASSERT_TRUE(kode_file_append("flash/long.txt", text));
    TEST_ASSERT_EQUAL_UINT32(2U * TEST_TEXT_BYTES, host_size(fake_media_fs_dir(), "long.txt"));

    bytes = kode_file_read_bytes("flash/long.txt", &len);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT32(2U * TEST_TEXT_BYTES, len);
    TEST_ASSERT_EQUAL_MEMORY(text, bytes, TEST_TEXT_BYTES);
    TEST_ASSERT_EQUAL_MEMORY(text, &bytes[TEST_TEXT_BYTES], TEST_TEXT_BYTES);
    free(text);
    file_no_settle();
}

TEST_CASE("put_bytes writes and appends bytes with NULs in them, in parts", "[kode_file]")
{
    uint8_t *const data = (uint8_t *)malloc(TEST_TEXT_BYTES);
    const uint8_t *bytes;
    uint32_t       len = 0U;

    TEST_ASSERT_NOT_NULL(data);
    for ( uint32_t i = 0U; TEST_TEXT_BYTES > i; i++ )
    {
        data[i] = (uint8_t)(i % TEST_BYTE_PERIOD);
    }

    file_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_file_put_bytes("flash/b.bin", data, TEST_TEXT_BYTES, false), kode_error());
    TEST_ASSERT_TRUE_MESSAGE(kode_file_put_bytes("flash/b.bin", data, TEST_APPEND_BYTES, true), kode_error());
    TEST_ASSERT_EQUAL_UINT32(TEST_TEXT_BYTES + TEST_APPEND_BYTES, host_size(fake_media_fs_dir(), "b.bin"));
    bytes = kode_file_read_bytes("flash/b.bin", &len);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT32(TEST_TEXT_BYTES + TEST_APPEND_BYTES, len);
    TEST_ASSERT_EQUAL_MEMORY(data, bytes, TEST_TEXT_BYTES);
    TEST_ASSERT_EQUAL_MEMORY(data, &bytes[TEST_TEXT_BYTES], TEST_APPEND_BYTES);

    /* A write replaces; no data is refused; a bad path is refused. */
    TEST_ASSERT_TRUE(kode_file_put_bytes("flash/b.bin", data, TEST_REPLACE_BYTES, false));
    TEST_ASSERT_EQUAL_UINT32(TEST_REPLACE_BYTES, host_size(fake_media_fs_dir(), "b.bin"));
    TEST_ASSERT_FALSE(kode_file_put_bytes("flash/b.bin", NULL, TEST_REPLACE_BYTES, false));
    TEST_ASSERT_EQUAL_STRING("flash/b.bin: no data to write", kode_error());
    TEST_ASSERT_FALSE(kode_file_put_bytes("sd/../b.bin", data, TEST_REPLACE_BYTES, false));
    free(data);
    file_no_settle();
}

TEST_CASE("a write with no text is refused", "[kode_file]")
{
    file_fresh();
    TEST_ASSERT_FALSE(kode_file_write("flash/x.txt", NULL));
    TEST_ASSERT_EQUAL_STRING("flash/x.txt: no text to write", kode_error());
    TEST_ASSERT_FALSE(kode_file_append("flash/x.txt", NULL));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, host_size(fake_media_fs_dir(), "x.txt"));
    file_no_settle();
}

TEST_CASE("a write with no path and no text names the path", "[kode_file]")
{
    file_fresh();
    TEST_ASSERT_FALSE(kode_file_write(NULL, NULL));
    TEST_ASSERT_NOT_NULL(strstr(kode_error(), "path"));
    TEST_ASSERT_NULL(strstr(kode_error(), "no text"));
    TEST_ASSERT_FALSE(kode_file_append(NULL, NULL));
    TEST_ASSERT_NULL(strstr(kode_error(), "no text"));
    TEST_ASSERT_FALSE(kode_file_write("sd/../x.txt", NULL));
    TEST_ASSERT_NULL(strstr(kode_error(), "no text"));
    file_no_settle();
}

TEST_CASE("a write into a folder that is not there says so", "[kode_file]")
{
    file_fresh();
    TEST_ASSERT_FALSE(kode_file_write("flash/nodir/x.txt", "a"));
    TEST_ASSERT_EQUAL_STRING("flash/nodir/x.txt: the folder above does not exist", kode_error());
    TEST_ASSERT_FALSE(kode_file_append("flash/nodir/x.txt", "a"));
    TEST_ASSERT_EQUAL_STRING("flash/nodir/x.txt: the folder above does not exist", kode_error());
    file_no_settle();
}

TEST_CASE("a write into a file that is in the way says why", "[kode_file]")
{
    file_fresh();
    TEST_ASSERT_TRUE(kode_file_write("flash/f", "a"));
    TEST_ASSERT_FALSE(kode_file_write("flash/f/x.txt", "a"));
    TEST_ASSERT_EQUAL_STRING("flash/f/x.txt: a folder in the path is a file", kode_error());
    file_no_settle();
}

TEST_CASE("a file of 4 MiB reads and one byte more does not", "[kode_file]")
{
    const uint8_t *bytes;
    uint32_t       len = 0U;

    file_fresh();
    TEST_ASSERT_TRUE(host_sparse("huge.bin", KODE_FILE_READ_MAX));
    bytes = kode_file_read_bytes("flash/huge.bin", &len);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT32(KODE_FILE_READ_MAX, len);
    TEST_ASSERT_EQUAL_UINT8(0U, bytes[len - 1U]);

    TEST_ASSERT_TRUE(host_sparse("huge.bin", KODE_FILE_READ_MAX + 1U));
    TEST_ASSERT_EQUAL_STRING("", kode_file_read("flash/huge.bin"));
    TEST_ASSERT_EQUAL_STRING(TEST_TOO_BIG, kode_error());
    len = 77U;
    TEST_ASSERT_NULL(kode_file_read_bytes("flash/huge.bin", &len));
    TEST_ASSERT_EQUAL_UINT32(0U, len);
    TEST_ASSERT_EQUAL_STRING(TEST_TOO_BIG, kode_error());
    file_no_settle();
}

TEST_CASE("a folder is not read", "[kode_file]")
{
    file_fresh();
    fake_media_put_fs_dir("dir");
    TEST_ASSERT_EQUAL_STRING("", kode_file_read("flash/dir"));
    TEST_ASSERT_EQUAL_STRING("flash/dir: it is a folder", kode_error());
    TEST_ASSERT_NULL(kode_file_read_bytes("flash/dir/", NULL));
    TEST_ASSERT_EQUAL_STRING("flash/dir/: it is a folder", kode_error());
    TEST_ASSERT_FALSE(kode_file_write("flash/dir", "a"));
    TEST_ASSERT_EQUAL_STRING("flash/dir: it is a folder", kode_error());
    file_no_settle();
}

/* ---- A read at an offset, and bytes written ---- */

/** @brief Bytes the offset read cases ask for. */
#define TEST_AT_CAP 50000U

/** @brief Where the offset read cases start: inside the first window and across the next. */
#define TEST_AT_OFFSET 40000U

/** @brief Bytes of guard after a read buffer, which a read leaves as they were. */
#define TEST_GUARD_BYTES 8U

/** @brief The value of a guard byte. */
#define TEST_GUARD_FILL 0xA5U

/** @brief Zero bytes ahead of the tail of the file that is longer than a whole read. */
#define TEST_PAST_ZEROS 1000U

/** @brief Bytes of that tail. */
#define TEST_PAST_TAIL 100U

/** @brief Bytes the read at the end of a file asks for beyond it. */
#define TEST_PAST_ASK 100U

/** @brief The size of the file the read ends past. */
#define TEST_END_FILE 100000U

/** @brief Bytes of that file the read that ends past it starts before the end of. */
#define TEST_END_LEFT 10U

/** @brief A buffer of @p cap bytes and a guard after them, all filled with the guard value. */
static uint8_t *at_buf_make(uint32_t cap)
{
    uint8_t *const buf = (uint8_t *)malloc(cap + TEST_GUARD_BYTES);

    if ( NULL != buf )
    {
        (void)memset(buf, (int)TEST_GUARD_FILL, cap + TEST_GUARD_BYTES);
    }
    return buf;
}

/** @brief Whether @p n bytes at @p buf all hold the guard value. */
static bool guard_intact(const uint8_t *buf, uint32_t n)
{
    bool ok = true;

    for ( uint32_t i = 0U; ok && (i < n); i++ )
    {
        ok = (TEST_GUARD_FILL == buf[i]);
    }
    return ok;
}

TEST_CASE("a read at an offset of a large file is byte-exact across windows", "[kode_file]")
{
    uint8_t *const buf = at_buf_make(TEST_AT_CAP);
    uint32_t       got = UINT32_MAX;
    uint32_t       live;

    file_fresh();
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_TRUE(sys_put_pattern("/storage/big.bin", TEST_BIG_BYTES));

    /* A failed read first, so the task's own blocks are made before the count. */
    TEST_ASSERT_FALSE(kode_file_read_at("flash/nope.bin", 0U, buf, TEST_AT_CAP, &got));
    live = psram_live();

    TEST_ASSERT_TRUE_MESSAGE(kode_file_read_at("flash/big.bin", TEST_AT_OFFSET, buf, TEST_AT_CAP, &got),
                             kode_error());
    TEST_ASSERT_EQUAL_UINT32(TEST_AT_CAP, got);
    TEST_ASSERT_TRUE(pattern_is(buf, TEST_AT_OFFSET, TEST_AT_CAP));
    TEST_ASSERT_TRUE(guard_intact(&buf[TEST_AT_CAP], TEST_GUARD_BYTES));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(live, psram_live(), "the read took a block of its own");

    /* A small read from the middle of a window. */
    (void)memset(buf, (int)TEST_GUARD_FILL, TEST_AT_CAP + TEST_GUARD_BYTES);
    TEST_ASSERT_TRUE(kode_file_read_at("flash/big.bin", 5U, buf, 10U, &got));
    TEST_ASSERT_EQUAL_UINT32(10U, got);
    TEST_ASSERT_TRUE(pattern_is(buf, 5U, 10U));
    TEST_ASSERT_TRUE(guard_intact(&buf[10], TEST_AT_CAP - 10U));

    /* The whole file in one read. */
    TEST_ASSERT_TRUE(kode_file_read_at("flash/big.bin", 0U, buf, TEST_AT_CAP, &got));
    TEST_ASSERT_EQUAL_UINT32(TEST_AT_CAP, got);
    TEST_ASSERT_TRUE(pattern_is(buf, 0U, TEST_AT_CAP));
    free(buf);
    file_no_settle();
}

TEST_CASE("a read at an offset reaches past the whole-file ceiling", "[kode_file]")
{
    uint8_t *const buf = at_buf_make(TEST_PAST_TAIL * 2U);
    uint32_t       got = UINT32_MAX;

    file_fresh();
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_TRUE(host_sparse("huge.bin", KODE_FILE_READ_MAX + TEST_PAST_ZEROS));
    TEST_ASSERT_TRUE(host_pattern("huge.bin", 0U, TEST_PAST_TAIL, "ab"));
    TEST_ASSERT_EQUAL_STRING("", kode_file_read("flash/huge.bin"));
    TEST_ASSERT_EQUAL_STRING(TEST_TOO_BIG, kode_error());

    TEST_ASSERT_TRUE_MESSAGE(kode_file_read_at("flash/huge.bin", KODE_FILE_READ_MAX + TEST_PAST_ZEROS, buf,
                                               TEST_PAST_TAIL * 2U, &got),
                             kode_error());
    TEST_ASSERT_EQUAL_UINT32(TEST_PAST_TAIL, got);
    TEST_ASSERT_TRUE(pattern_is(buf, 0U, TEST_PAST_TAIL));
    TEST_ASSERT_TRUE(guard_intact(&buf[TEST_PAST_TAIL], TEST_PAST_TAIL + TEST_GUARD_BYTES));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    free(buf);
    file_no_settle();
}

TEST_CASE("a read that ends past the end of the file returns what there is", "[kode_file]")
{
    uint8_t *const buf = at_buf_make(TEST_PAST_ASK);
    uint32_t       got = UINT32_MAX;

    file_fresh();
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_TRUE(host_pattern("end.bin", 0U, TEST_END_FILE, "wb"));

    TEST_ASSERT_TRUE_MESSAGE(kode_file_read_at("flash/end.bin", TEST_END_FILE - TEST_END_LEFT, buf,
                                               TEST_PAST_ASK, &got),
                             kode_error());
    TEST_ASSERT_EQUAL_UINT32(TEST_END_LEFT, got);
    TEST_ASSERT_TRUE(pattern_is(buf, TEST_END_FILE - TEST_END_LEFT, got));
    TEST_ASSERT_TRUE(guard_intact(&buf[got], TEST_PAST_ASK + TEST_GUARD_BYTES - got));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    free(buf);
    file_no_settle();
}

TEST_CASE("a read at or past the end reads nothing and is not an error", "[kode_file]")
{
    uint8_t *const buf = at_buf_make(TEST_PAST_ASK);
    uint32_t       got = UINT32_MAX;

    file_fresh();
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_TRUE(host_pattern("end.bin", 0U, TEST_END_FILE, "wb"));
    TEST_ASSERT_TRUE(host_pattern("none.bin", 0U, 0U, "wb"));

    TEST_ASSERT_TRUE_MESSAGE(kode_file_read_at("flash/end.bin", TEST_END_FILE, buf, TEST_PAST_ASK, &got),
                             kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, got);
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    got = UINT32_MAX;
    TEST_ASSERT_TRUE(kode_file_read_at("flash/end.bin", TEST_END_FILE + 1U, buf, TEST_PAST_ASK, &got));
    TEST_ASSERT_EQUAL_UINT32(0U, got);

    got = UINT32_MAX;
    TEST_ASSERT_TRUE(kode_file_read_at("flash/end.bin", UINT32_MAX, buf, TEST_PAST_ASK, &got));
    TEST_ASSERT_EQUAL_UINT32(0U, got);

    got = UINT32_MAX;
    TEST_ASSERT_TRUE(kode_file_read_at("flash/none.bin", 0U, buf, TEST_PAST_ASK, &got));
    TEST_ASSERT_EQUAL_UINT32(0U, got);

    /* Asking for no bytes of a file that is there reads none. */
    got = UINT32_MAX;
    TEST_ASSERT_TRUE(kode_file_read_at("flash/end.bin", 0U, buf, 0U, &got));
    TEST_ASSERT_EQUAL_UINT32(0U, got);
    TEST_ASSERT_TRUE(guard_intact(buf, TEST_PAST_ASK + TEST_GUARD_BYTES));
    free(buf);
    file_no_settle();
}

TEST_CASE("a read at an offset of nothing, of a folder or by a bad path is refused", "[kode_file]")
{
    uint8_t *const buf = at_buf_make(TEST_PAST_ASK);
    uint32_t       got = UINT32_MAX;

    file_fresh();
    TEST_ASSERT_NOT_NULL(buf);
    fake_media_put_fs_dir("dir");

    TEST_ASSERT_FALSE(kode_file_read_at("flash/nope.bin", 0U, buf, TEST_PAST_ASK, &got));
    TEST_ASSERT_EQUAL_UINT32(0U, got);
    TEST_ASSERT_EQUAL_STRING("flash/nope.bin: no such file", kode_error());

    got = UINT32_MAX;
    TEST_ASSERT_FALSE(kode_file_read_at("flash/dir", 0U, buf, TEST_PAST_ASK, &got));
    TEST_ASSERT_EQUAL_UINT32(0U, got);
    TEST_ASSERT_EQUAL_STRING("flash/dir: it is a folder", kode_error());

    got = UINT32_MAX;
    TEST_ASSERT_FALSE(kode_file_read_at("usb/a", 0U, buf, TEST_PAST_ASK, &got));
    TEST_ASSERT_EQUAL_UINT32(0U, got);
    TEST_ASSERT_EQUAL_STRING("bad path 'usb/a': start with sd/ or flash/", kode_error());

    TEST_ASSERT_FALSE(kode_file_read_at(NULL, 0U, buf, TEST_PAST_ASK, &got));
    TEST_ASSERT_EQUAL_STRING("bad path '(null)': start with sd/ or flash/", kode_error());

    TEST_ASSERT_FALSE(kode_file_read_at("sd/../a", 0U, buf, TEST_PAST_ASK, &got));
    TEST_ASSERT_EQUAL_STRING("bad path 'sd/../a': no empty, '.' or '..' part", kode_error());
    TEST_ASSERT_TRUE(guard_intact(buf, TEST_PAST_ASK + TEST_GUARD_BYTES));
    free(buf);
    file_no_settle();
}

TEST_CASE("a read at an offset with no buffer or no count is refused", "[kode_file]")
{
    uint8_t *const buf = at_buf_make(TEST_PAST_ASK);
    uint32_t       got = UINT32_MAX;

    file_fresh();
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_TRUE(host_pattern("a.bin", 0U, TEST_PAST_ASK, "wb"));

    TEST_ASSERT_FALSE(kode_file_read_at("flash/a.bin", 0U, NULL, TEST_PAST_ASK, &got));
    TEST_ASSERT_EQUAL_UINT32(0U, got);
    TEST_ASSERT_EQUAL_STRING("flash/a.bin: no buffer to read into", kode_error());

    TEST_ASSERT_FALSE(kode_file_read_at("flash/a.bin", 0U, buf, TEST_PAST_ASK, NULL));
    TEST_ASSERT_EQUAL_STRING("flash/a.bin: no place for the count of bytes read", kode_error());
    TEST_ASSERT_TRUE(guard_intact(buf, TEST_PAST_ASK + TEST_GUARD_BYTES));

    /* A bad path is the error that wins over a missing buffer or count. */
    TEST_ASSERT_FALSE(kode_file_read_at("usb/a", 0U, NULL, TEST_PAST_ASK, NULL));
    TEST_ASSERT_EQUAL_STRING("bad path 'usb/a': start with sd/ or flash/", kode_error());
    free(buf);
    file_no_settle();
}

/** @brief Bytes of a flash file the write cases hold: two parts and a bit, with NULs in them. */
#define TEST_WB_BYTES TEST_TEXT_BYTES

/** @brief Fill @p data with the NUL-bearing pattern of the write cases. */
static void wb_fill(uint8_t *data, uint32_t n)
{
    for ( uint32_t i = 0U; i < n; i++ )
    {
        data[i] = (uint8_t)(i % TEST_BYTE_PERIOD);
    }
}

TEST_CASE("bytes with NULs are written, appended to and read back exactly", "[kode_file]")
{
    uint8_t *const data = (uint8_t *)malloc(TEST_WB_BYTES);
    uint8_t *const back = at_buf_make(TEST_WB_BYTES + TEST_APPEND_BYTES);
    uint32_t       got  = UINT32_MAX;

    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_NOT_NULL(back);
    wb_fill(data, TEST_WB_BYTES);

    file_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_file_write_bytes("flash/w.bin", data, TEST_WB_BYTES), kode_error());
    TEST_ASSERT_EQUAL_UINT32(TEST_WB_BYTES, host_size(fake_media_fs_dir(), "w.bin"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    TEST_ASSERT_TRUE_MESSAGE(kode_file_append_bytes("flash/w.bin", data, TEST_APPEND_BYTES), kode_error());
    TEST_ASSERT_EQUAL_UINT32(TEST_WB_BYTES + TEST_APPEND_BYTES, host_size(fake_media_fs_dir(), "w.bin"));

    TEST_ASSERT_TRUE_MESSAGE(kode_file_read_at("flash/w.bin", 0U, back, TEST_WB_BYTES + TEST_APPEND_BYTES, &got),
                             kode_error());
    TEST_ASSERT_EQUAL_UINT32(TEST_WB_BYTES + TEST_APPEND_BYTES, got);
    TEST_ASSERT_EQUAL_MEMORY(data, back, TEST_WB_BYTES);
    TEST_ASSERT_EQUAL_MEMORY(data, &back[TEST_WB_BYTES], TEST_APPEND_BYTES);
    TEST_ASSERT_EQUAL_UINT8(0U, back[0]);

    /* A write replaces what the file held. */
    TEST_ASSERT_TRUE(kode_file_write_bytes("flash/w.bin", data, TEST_REPLACE_BYTES));
    TEST_ASSERT_EQUAL_UINT32(TEST_REPLACE_BYTES, host_size(fake_media_fs_dir(), "w.bin"));
    free(data);
    free(back);
    file_no_settle();
}

TEST_CASE("an append of bytes creates a file that is not there", "[kode_file]")
{
    static const uint8_t data[] = { 0U, 1U, 0U, 2U };
    uint8_t              back[sizeof(data)];
    uint32_t             got = UINT32_MAX;

    file_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_file_append_bytes("flash/n.bin", data, sizeof(data)), kode_error());
    TEST_ASSERT_TRUE(kode_file_read_at("flash/n.bin", 0U, back, sizeof(back), &got));
    TEST_ASSERT_EQUAL_UINT32(sizeof(data), got);
    TEST_ASSERT_EQUAL_MEMORY(data, back, sizeof(data));
    file_no_settle();
}

TEST_CASE("a write of bytes with a length of 0 leaves a file of no bytes", "[kode_file]")
{
    static const uint8_t data[] = { 7U };

    file_fresh();
    TEST_ASSERT_TRUE(kode_file_write_bytes("flash/e.bin", data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT32(sizeof(data), host_size(fake_media_fs_dir(), "e.bin"));
    TEST_ASSERT_TRUE_MESSAGE(kode_file_write_bytes("flash/e.bin", data, 0U), kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, host_size(fake_media_fs_dir(), "e.bin"));

    /* An append of no bytes keeps the file and makes it when it is not there. */
    TEST_ASSERT_TRUE(kode_file_append_bytes("flash/e.bin", data, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, host_size(fake_media_fs_dir(), "e.bin"));
    TEST_ASSERT_TRUE(kode_file_append_bytes("flash/f.bin", data, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, host_size(fake_media_fs_dir(), "f.bin"));
    file_no_settle();
}

TEST_CASE("a write of bytes with no data is refused, whatever the length", "[kode_file]")
{
    static const uint8_t data[] = { 1U };

    file_fresh();
    TEST_ASSERT_TRUE(kode_file_write_bytes("flash/k.bin", data, sizeof(data)));

    TEST_ASSERT_FALSE(kode_file_write_bytes("flash/k.bin", NULL, 1U));
    TEST_ASSERT_EQUAL_STRING("flash/k.bin: no data to write", kode_error());
    TEST_ASSERT_FALSE(kode_file_write_bytes("flash/k.bin", NULL, 0U));
    TEST_ASSERT_EQUAL_STRING("flash/k.bin: no data to write", kode_error());
    TEST_ASSERT_FALSE(kode_file_append_bytes("flash/k.bin", NULL, 1U));
    TEST_ASSERT_EQUAL_STRING("flash/k.bin: no data to write", kode_error());
    TEST_ASSERT_FALSE(kode_file_append_bytes("flash/k.bin", NULL, 0U));
    TEST_ASSERT_EQUAL_STRING("flash/k.bin: no data to write", kode_error());
    TEST_ASSERT_EQUAL_UINT32(sizeof(data), host_size(fake_media_fs_dir(), "k.bin"));

    /* A bad path is the error that wins; nothing is made. */
    TEST_ASSERT_FALSE(kode_file_write_bytes("sd/../k.bin", NULL, 0U));
    TEST_ASSERT_EQUAL_STRING("bad path 'sd/../k.bin': no empty, '.' or '..' part", kode_error());
    TEST_ASSERT_FALSE(kode_file_append_bytes(NULL, data, sizeof(data)));
    TEST_ASSERT_EQUAL_STRING("bad path '(null)': start with sd/ or flash/", kode_error());
    file_no_settle();
}

TEST_CASE("a write of bytes says why when the folder is missing, a file is in the way or it is a folder",
          "[kode_file]")
{
    static const uint8_t data[] = { 1U, 2U };

    file_fresh();
    TEST_ASSERT_FALSE(kode_file_write_bytes("flash/nodir/x.bin", data, sizeof(data)));
    TEST_ASSERT_EQUAL_STRING("flash/nodir/x.bin: the folder above does not exist", kode_error());
    TEST_ASSERT_FALSE(kode_file_append_bytes("flash/nodir/x.bin", data, sizeof(data)));
    TEST_ASSERT_EQUAL_STRING("flash/nodir/x.bin: the folder above does not exist", kode_error());

    TEST_ASSERT_TRUE(kode_file_write_bytes("flash/f", data, sizeof(data)));
    TEST_ASSERT_FALSE(kode_file_write_bytes("flash/f/x.bin", data, sizeof(data)));
    TEST_ASSERT_EQUAL_STRING("flash/f/x.bin: a folder in the path is a file", kode_error());

    fake_media_put_fs_dir("dir");
    TEST_ASSERT_FALSE(kode_file_write_bytes("flash/dir", data, sizeof(data)));
    TEST_ASSERT_EQUAL_STRING("flash/dir: it is a folder", kode_error());
    TEST_ASSERT_FALSE(kode_file_append_bytes("flash/dir", data, sizeof(data)));
    TEST_ASSERT_EQUAL_STRING("flash/dir: it is a folder", kode_error());
    file_no_settle();
}

TEST_CASE("a new read replaces the last text and leaks nothing", "[kode_file]")
{
    uint32_t live;

    file_fresh();
    TEST_ASSERT_TRUE(kode_file_write("flash/a.txt", "first"));
    TEST_ASSERT_TRUE(kode_file_write("flash/b.txt", "second one"));

    TEST_ASSERT_EQUAL_STRING("first", kode_file_read("flash/a.txt"));
    live = psram_live();
    TEST_ASSERT_EQUAL_STRING("second one", kode_file_read("flash/b.txt"));
    TEST_ASSERT_EQUAL_UINT32(live, psram_live());
    TEST_ASSERT_EQUAL_STRING("first", kode_file_read("flash/a.txt"));
    TEST_ASSERT_EQUAL_UINT32(live, psram_live());

    /* The bytes keep their own block, one live at a time. */
    TEST_ASSERT_NOT_NULL(kode_file_read_bytes("flash/a.txt", NULL));
    live = psram_live();
    TEST_ASSERT_NOT_NULL(kode_file_read_bytes("flash/b.txt", NULL));
    TEST_ASSERT_EQUAL_UINT32(live, psram_live());

    /* The text is still the last text: the two calls do not share a block. */
    TEST_ASSERT_EQUAL_STRING("first", kode_file_read("flash/a.txt"));
    file_no_settle();
}

TEST_CASE("a read that fails gives back the last text's block", "[kode_file]")
{
    uint32_t live;

    file_fresh();
    TEST_ASSERT_TRUE(kode_file_write("flash/a.txt", "first"));

    /* The task's state is already there; a read of nothing holds no block. */
    TEST_ASSERT_EQUAL_STRING("", kode_file_read("flash/nope.txt"));
    live = psram_live();
    TEST_ASSERT_EQUAL_STRING("first", kode_file_read("flash/a.txt"));
    TEST_ASSERT_EQUAL_UINT32(live + 1U, psram_live());
    TEST_ASSERT_EQUAL_STRING("", kode_file_read("flash/nope.txt"));
    TEST_ASSERT_EQUAL_UINT32(live, psram_live());
    file_no_settle();
}

TEST_CASE("a read that PSRAM refuses says it is out of memory", "[kode_file]")
{
    uint32_t len = 77U;

    file_fresh();
    TEST_ASSERT_TRUE(kode_file_write("flash/a.txt", "first"));
    TEST_ASSERT_EQUAL_STRING("first", kode_file_read("flash/a.txt"));

    fake_psram_refuse(1U);
    TEST_ASSERT_EQUAL_STRING("", kode_file_read("flash/a.txt"));
    TEST_ASSERT_EQUAL_STRING("flash/a.txt: out of memory", kode_error());
    fake_psram_refuse(1U);
    TEST_ASSERT_NULL(kode_file_read_bytes("flash/a.txt", &len));
    TEST_ASSERT_EQUAL_UINT32(0U, len);
    TEST_ASSERT_EQUAL_STRING("flash/a.txt: out of memory", kode_error());
    fake_psram_refuse(0U);

    TEST_ASSERT_EQUAL_STRING("first", kode_file_read("flash/a.txt"));
    file_no_settle();
}

/** @brief What the error of the refused-state task said. */
static char s_refused_error[KODE_ERROR_MAX];

/** @brief A task whose files state PSRAM refuses, reading a NULL path. */
static void null_path_task_ends(void *arg)
{
    ender_t *const e = (ender_t *)arg;

    /* The task's own struct is taken first (by the error), then PSRAM refuses. */
    fake_psram_refuse_after(1U, 2U);
    e->had_state = ('\0' == kode_file_read(NULL)[0]);
    (void)snprintf(s_refused_error, sizeof(s_refused_error), "%s", kode_error());
    fake_psram_refuse(0U);
    e->slot = khal_task_slot_get();
    fake_task_slot_watch(e->slot);
    (void)xSemaphoreGive(e->done);
}

TEST_CASE("a NULL path is shown as (null) when the files state is refused", "[kode_file]")
{
    ender_t e = { NULL, NULL, false, false, 0U };

    s_refused_error[0] = '\0';
    run_state_task(&e, &null_path_task_ends);
    TEST_ASSERT_TRUE(e.had_state);
    TEST_ASSERT_EQUAL_STRING("(null): out of memory", s_refused_error);
}

/* ---- Busy storage ---- */

/** @brief Let the abandoned request land and arm the next round trips. */
static void file_busy_ends(void)
{
    file_settle_once();
    file_will_settle();
}

TEST_CASE("a write waits out busy storage and goes through", "[kode_file]")
{
    kode_sys_file_status_t st;

    file_fresh();

    /* A request nobody answered holds the facade: every call is turned away. */
    file_no_settle();
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_file_status("/storage/x", &st));
    file_no_settle();
    fake_time_reset();

    /* It lands at the call's first sleep. */
    fake_time_delay_hook(&file_busy_ends);
    TEST_ASSERT_TRUE(kode_file_write("flash/b.txt", "abc"));
    TEST_ASSERT_EQUAL_UINT32(3U, host_size(fake_media_fs_dir(), "b.txt"));
    TEST_ASSERT_EQUAL_UINT32(KODE_RETRY_STEP_MS, kode_sys_time_ms());
    file_no_settle();
}

TEST_CASE("a write held past the bound fails saying storage is busy", "[kode_file]")
{
    kode_sys_file_status_t st;

    file_fresh();
    file_no_settle();
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_file_status("/storage/x", &st));
    file_no_settle();
    fake_time_reset();

    TEST_ASSERT_FALSE(kode_file_write("flash/b.txt", "abc"));
    TEST_ASSERT_EQUAL_STRING("flash/b.txt: storage is busy", kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_FILE_BUSY_MS, kode_sys_time_ms());
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, host_size(fake_media_fs_dir(), "b.txt"));

    /* The reads turn away the same way. */
    TEST_ASSERT_EQUAL_STRING("", kode_file_read("flash/b.txt"));
    TEST_ASSERT_EQUAL_STRING("flash/b.txt: storage is busy", kode_error());

    /* Let the abandoned request land, so the next case finds a clean facade. */
    file_will_settle();
    TEST_ASSERT_TRUE(kode_file_write("flash/b.txt", "abc"));
    file_no_settle();
}

/* ---- A file that changes while it is read ---- */

/** @brief Add TEST_GROWTH bytes to the end of g.bin. */
static bool change_grow(void)
{
    return host_pattern("g.bin", 0U, TEST_GROWTH, "ab");
}

/** @brief Rewrite g.bin as TEST_SHRUNK bytes. */
static bool change_shrink(void)
{
    return host_pattern("g.bin", 0U, TEST_SHRUNK, "wb");
}

/** @brief Read g.bin with a change landing after wait @p after. */
static void read_with_change(bool (*change)(void), uint32_t after)
{
    s_waits     = 0U;
    s_change_at = after;
    s_change    = change;
    s_change_ok = false;
    fake_sem_wait_hook(&file_change_hook);
}

TEST_CASE("a file that grows between two parts is reported changed", "[kode_file]")
{
    uint32_t len = 77U;
    uint32_t live;

    file_fresh();
    TEST_ASSERT_TRUE(host_pattern("g.bin", 0U, TEST_GROW_BYTES, "wb"));

    /* A read of nothing leaves the task's state with no block. */
    TEST_ASSERT_NULL(kode_file_read_bytes("flash/nope.bin", NULL));
    live = psram_live();

    /* Round trips: the status, the first part; the change lands after the first part. */
    read_with_change(&change_grow, 2U);
    TEST_ASSERT_NULL(kode_file_read_bytes("flash/g.bin", &len));
    TEST_ASSERT_TRUE(s_change_ok);
    TEST_ASSERT_EQUAL_UINT32(0U, len);
    TEST_ASSERT_EQUAL_STRING("flash/g.bin: changed while it was read", kode_error());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(live, psram_live(), "the torn read kept its block");
    file_no_settle();
}

TEST_CASE("a file that is exactly one part and grows is reported changed", "[kode_file]")
{
    file_fresh();
    TEST_ASSERT_TRUE(host_pattern("g.bin", 0U, TEST_PART, "wb"));
    read_with_change(&change_grow, 2U);
    TEST_ASSERT_EQUAL_STRING("", kode_file_read("flash/g.bin"));
    TEST_ASSERT_TRUE(s_change_ok);
    TEST_ASSERT_EQUAL_STRING("flash/g.bin: changed while it was read", kode_error());
    file_no_settle();
}

TEST_CASE("a file that shrinks between two parts is reported changed", "[kode_file]")
{
    file_fresh();
    TEST_ASSERT_TRUE(host_pattern("g.bin", 0U, TEST_GROW_BYTES, "wb"));
    read_with_change(&change_shrink, 2U);
    TEST_ASSERT_EQUAL_STRING("", kode_file_read("flash/g.bin"));
    TEST_ASSERT_TRUE(s_change_ok);
    TEST_ASSERT_EQUAL_STRING("flash/g.bin: changed while it was read", kode_error());
    file_no_settle();
}

/** @brief Empty g.bin. */
static bool change_empty(void)
{
    return host_pattern("g.bin", 0U, 0U, "wb");
}

TEST_CASE("a file emptied after its status is reported changed, not busy", "[kode_file]")
{
    file_fresh();
    TEST_ASSERT_TRUE(host_pattern("g.bin", 0U, TEST_GROW_BYTES, "wb"));
    read_with_change(&change_empty, 1U);
    TEST_ASSERT_EQUAL_STRING("", kode_file_read("flash/g.bin"));
    TEST_ASSERT_TRUE(s_change_ok);
    TEST_ASSERT_EQUAL_STRING("flash/g.bin: changed while it was read", kode_error());
    TEST_ASSERT_LESS_THAN_UINT32(KODE_FILE_BUSY_MS, kode_sys_time_ms());
    file_no_settle();
}

TEST_CASE("a file that grows after its status is reported changed", "[kode_file]")
{
    file_fresh();
    TEST_ASSERT_TRUE(host_pattern("g.bin", 0U, TEST_GROWTH, "wb"));
    read_with_change(&change_grow, 1U);
    TEST_ASSERT_EQUAL_STRING("", kode_file_read("flash/g.bin"));
    TEST_ASSERT_TRUE(s_change_ok);
    TEST_ASSERT_EQUAL_STRING("flash/g.bin: changed while it was read", kode_error());
    file_no_settle();
}

TEST_CASE("a read at an offset of a file that shrinks between two windows is reported changed",
          "[kode_file]")
{
    uint8_t *const buf = at_buf_make(TEST_GROW_BYTES);
    uint32_t       got = UINT32_MAX;

    file_fresh();
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_TRUE(host_pattern("g.bin", 0U, TEST_GROW_BYTES, "wb"));

    /* Round trips: the status, the first window; the change lands after the first window. */
    read_with_change(&change_shrink, 2U);
    TEST_ASSERT_FALSE(kode_file_read_at("flash/g.bin", 0U, buf, TEST_GROW_BYTES, &got));
    TEST_ASSERT_TRUE(s_change_ok);
    TEST_ASSERT_EQUAL_UINT32(0U, got);
    TEST_ASSERT_EQUAL_STRING("flash/g.bin: changed while it was read", kode_error());
    free(buf);
    file_no_settle();
}

/* ---- A write that stops partway ---- */

/** @brief Make the card refuse files after the first part has landed. */
static bool change_card_fails(void)
{
    fake_sdmmc_fail_file_writes(true);
    return true;
}

TEST_CASE("a write that fails after the first part says how far it got", "[kode_file]")
{
    char *text = (char *)malloc(TEST_TEXT_BYTES + 1U);

    TEST_ASSERT_NOT_NULL(text);
    (void)memset(text, 'q', TEST_TEXT_BYTES);
    text[TEST_TEXT_BYTES] = '\0';

    file_fresh();
    fake_media_set_sd_present(true);
    s_waits     = 0U;
    s_change_at = 1U;
    s_change    = &change_card_fails;
    s_change_ok = false;
    fake_sem_wait_hook(&file_change_hook);

    TEST_ASSERT_FALSE(kode_file_write("sd/p.txt", text));
    free(text);
    TEST_ASSERT_TRUE(s_change_ok);
    TEST_ASSERT_EQUAL_STRING_LEN("sd/p.txt: wrote 32768 of 70000 bytes: ", kode_error(), 38U);
    TEST_ASSERT_GREATER_THAN_UINT32(38U, (uint32_t)strlen(kode_error()));
    fake_sdmmc_fail_file_writes(false);
    file_no_settle();
}

/* ---- Two tasks, one files layer -------------------------------------------- */

/** @brief Bytes of the file the other task finds in place. */
#define TEST_RACE_OLD_BYTES 40000U

/** @brief Bytes of the text the first task writes: three parts. */
#define TEST_RACE_NEW_BYTES 70000U

/** @brief Ticks the first task sleeps between two looks at the second. */
#define TEST_RACE_POLL_TICKS 1U

/** @brief What the second task does. */
typedef enum {
    RACE_READ,    /**< Read the file.            */
    RACE_APPEND   /**< Append one letter to it.  */
} race_mode_t;

static SemaphoreHandle_t s_race_go;       /**< Given to let the second task begin.     */
static SemaphoreHandle_t s_race_at_door;  /**< Given by it just before its call.       */
static SemaphoreHandle_t s_race_done;     /**< Given by it when its call returned.     */
static TaskHandle_t      s_race_task;     /**< The second task.                        */
static atomic_bool       s_race_returned; /**< Its call returned.                      */
static race_mode_t       s_race_mode;     /**< What it does.                           */
static uint32_t          s_race_len;      /**< Bytes its read gave.                    */
static bool              s_race_uniform;  /**< They are all one letter.                */
static char              s_race_letter;   /**< The first of them.                      */
static char              s_race_error[KODE_ERROR_MAX]; /**< Its error after the call.  */

/** @brief The second task: one file call at the moment it is let go. */
static void race_second(void *arg)
{
    (void)arg;
    (void)xSemaphoreTake(s_race_go, pdMS_TO_TICKS(TEST_WAIT_MS));
    s_race_task = xTaskGetCurrentTaskHandle();
    file_will_settle();
    (void)xSemaphoreGive(s_race_at_door);
    if ( RACE_READ == s_race_mode )
    {
        const char *const text = kode_file_read("flash/race.txt");

        s_race_len     = (uint32_t)strlen(text);
        s_race_letter  = text[0];
        s_race_uniform = true;
        for ( uint32_t i = 0U; i < s_race_len; i++ )
        {
            s_race_uniform = s_race_uniform && (text[i] == s_race_letter);
        }
    }
    else
    {
        (void)kode_file_append("flash/race.txt", "B");
    }
    (void)snprintf(s_race_error, sizeof(s_race_error), "%s", kode_error());
    atomic_store(&s_race_returned, true);
    (void)xSemaphoreGive(s_race_done);
}

/** @brief Let the second task call in, and wait until it is blocked or has returned. */
static bool race_let_in(void)
{
    bool settled = (xSemaphoreGive(s_race_go) == pdTRUE)
                   && (xSemaphoreTake(s_race_at_door, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);

    for ( uint32_t i = 0U; settled && (pdMS_TO_TICKS(TEST_WAIT_MS) > i) && !atomic_load(&s_race_returned)
                          && (eTaskGetState(s_race_task) != eBlocked); i++ )
    {
        vTaskDelay(TEST_RACE_POLL_TICKS);
    }
    return settled;
}

/** @brief The file the first task works on: @p n bytes of @p letter. */
static void race_file(char letter, uint32_t n)
{
    char *const body = (char *)malloc(n + 1U);

    TEST_ASSERT_NOT_NULL(body);
    (void)memset(body, letter, n);
    body[n] = '\0';
    fake_media_put_fs_path("race.txt", body, n);
    free(body);
}

/** @brief Start the second task in @p mode and arm the first task's hook to let it in. */
static void race_begin(race_mode_t mode)
{
    s_race_go      = xSemaphoreCreateBinary();
    s_race_at_door = xSemaphoreCreateBinary();
    s_race_done    = xSemaphoreCreateBinary();
    s_race_task    = NULL;
    s_race_mode    = mode;
    s_race_len     = 0U;
    s_race_uniform = false;
    s_race_letter  = '?';
    s_race_error[0] = '\0';
    atomic_store(&s_race_returned, false);
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&race_second, "t_race", TEST_STACK_BYTES,
                                                    TEST_PRIO, NULL));
    /* The first task's first part is its first round trip: the second comes in after it. */
    s_waits     = 0U;
    s_change_at = 1U;
    s_change    = &race_let_in;
    s_change_ok = false;
    fake_sem_wait_hook(&file_change_hook);
}

/** @brief Wait for the second task's call to return and free the semaphores. */
static void race_end(void)
{
    TEST_ASSERT_TRUE(s_change_ok);
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_race_done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    vSemaphoreDelete(s_race_go);
    vSemaphoreDelete(s_race_at_door);
    vSemaphoreDelete(s_race_done);
    file_will_settle();
}

TEST_CASE("a read of a file another task is rewriting sees all of it or none", "[kode_file]")
{
    char *text = (char *)malloc(TEST_RACE_NEW_BYTES + 1U);

    TEST_ASSERT_NOT_NULL(text);
    (void)memset(text, 'N', TEST_RACE_NEW_BYTES);
    text[TEST_RACE_NEW_BYTES] = '\0';

    file_fresh();
    race_file('O', TEST_RACE_OLD_BYTES);
    race_begin(RACE_READ);
    TEST_ASSERT_TRUE(kode_file_write("flash/race.txt", text));
    free(text);
    race_end();

    TEST_ASSERT_EQUAL_STRING("", s_race_error);
    TEST_ASSERT_TRUE(s_race_uniform);
    TEST_ASSERT_TRUE_MESSAGE((('O' == s_race_letter) && (TEST_RACE_OLD_BYTES == s_race_len))
                                 || (('N' == s_race_letter) && (TEST_RACE_NEW_BYTES == s_race_len)),
                             "the reader saw a file torn between the old and the new");
    TEST_ASSERT_EQUAL_UINT32(TEST_RACE_NEW_BYTES, host_size(fake_media_fs_dir(), "race.txt"));
    file_no_settle();
}

TEST_CASE("an append waits for another task's append to finish", "[kode_file]")
{
    char *text = (char *)malloc(TEST_RACE_NEW_BYTES + 1U);

    TEST_ASSERT_NOT_NULL(text);
    (void)memset(text, 'A', TEST_RACE_NEW_BYTES);
    text[TEST_RACE_NEW_BYTES] = '\0';

    file_fresh();
    race_file('O', 1U);
    race_begin(RACE_APPEND);
    TEST_ASSERT_TRUE(kode_file_append("flash/race.txt", text));
    free(text);
    race_end();

    TEST_ASSERT_EQUAL_STRING("", s_race_error);
    /* The one letter is last: it did not land inside the other's three parts. */
    TEST_ASSERT_EQUAL_UINT32(1U + TEST_RACE_NEW_BYTES + 1U, host_size(fake_media_fs_dir(), "race.txt"));
    TEST_ASSERT_EQUAL_STRING("B", kode_file_read("flash/race.txt") + TEST_RACE_NEW_BYTES + 1U);
    file_no_settle();
}

/* ---- A task that ends ---- */

/** @brief A task that reads a file and a byte block, then ends. */
static void reader_task_ends(void *arg)
{
    ender_t *const e = (ender_t *)arg;
    uint32_t       len = 0U;

    file_will_settle();
    e->had_state = (strcmp(kode_file_read("flash/t.txt"), "hello") == 0);
    e->had_state = e->had_state && (kode_file_read_bytes("flash/t.txt", &len) != NULL)
                   && (5U == len);
    file_no_settle();
    e->slot = khal_task_slot_get();
    fake_task_slot_watch(e->slot);
    (void)xSemaphoreGive(e->done);
}

TEST_CASE("a task that read a file frees its blocks when it ends", "[kode_file]")
{
    ender_t e = { NULL, NULL, false, false, 0U };

    file_fresh();
    TEST_ASSERT_TRUE(kode_file_write("flash/t.txt", "hello"));
    file_no_settle();
    run_state_task(&e, &reader_task_ends);
    TEST_ASSERT_TRUE(e.had_state);
}

/* ---- Status, existence, listing, folders and delete ------------------------ */

/** @brief Files in the folder of the listing that overflows. */
#define TEST_MANY_FILES 30U

/** @brief Bytes of a name built for the many-files folder. */
#define TEST_NAME_BYTES 32U

/** @brief Bytes of the file the status and listing cases use. */
#define TEST_SMALL_BYTES 12U

/** @brief Files in the folder of the plain listing. */
#define TEST_FEW_FILES 3U

/** @brief Put a file of @p n 'x' bytes at @p rel under the internal volume. */
static void put_fs_bytes(const char *rel, uint32_t n)
{
    char *const body = (char *)malloc(n + 1U);

    TEST_ASSERT_NOT_NULL(body);
    (void)memset(body, 'x', n);
    body[n] = '\0';
    fake_media_put_fs_path(rel, body, n);
    free(body);
}

/** @brief The entry of @p list called @p name, or NULL. */
static const kode_file_entry_t *entry_named(const kode_file_list_t *list, const char *name)
{
    const kode_file_entry_t *found = NULL;

    for ( uint32_t i = 0U; (NULL == found) && (i < list->count); i++ )
    {
        if ( strcmp(list->entries[i].name, name) == 0 )
        {
            found = &list->entries[i];
        }
    }
    return found;
}

TEST_CASE("the status of a file is its size", "[kode_file]")
{
    kode_file_status_t st;

    file_fresh();
    put_fs_bytes("a.txt", TEST_SMALL_BYTES);
    st = kode_file_status("flash/a.txt");
    TEST_ASSERT_TRUE(st.exists);
    TEST_ASSERT_FALSE(st.is_folder);
    TEST_ASSERT_EQUAL_UINT32(TEST_SMALL_BYTES, st.size);
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    file_no_settle();
}

TEST_CASE("the status of a folder has no size", "[kode_file]")
{
    kode_file_status_t st;

    file_fresh();
    fake_media_put_fs_dir("dir");
    st = kode_file_status("flash/dir");
    TEST_ASSERT_TRUE(st.exists);
    TEST_ASSERT_TRUE(st.is_folder);
    TEST_ASSERT_EQUAL_UINT32(0U, st.size);
    st = kode_file_status("flash");
    TEST_ASSERT_TRUE(st.exists);
    TEST_ASSERT_TRUE(st.is_folder);
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    file_no_settle();
}

TEST_CASE("the status of nothing is an answer, not an error", "[kode_file]")
{
    kode_file_status_t st;

    file_fresh();
    kode_err_set("%s", "stale");
    st = kode_file_status("flash/nope");
    TEST_ASSERT_FALSE(st.exists);
    TEST_ASSERT_FALSE(st.is_folder);
    TEST_ASSERT_EQUAL_UINT32(0U, st.size);
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    file_no_settle();
}

TEST_CASE("the status of a bad path is nothing and an error", "[kode_file]")
{
    kode_file_status_t st;

    file_fresh();
    st = kode_file_status("usb/a");
    TEST_ASSERT_FALSE(st.exists);
    TEST_ASSERT_EQUAL_STRING("bad path 'usb/a': start with sd/ or flash/", kode_error());
    st = kode_file_status(NULL);
    TEST_ASSERT_FALSE(st.exists);
    TEST_ASSERT_EQUAL_STRING("bad path '(null)': start with sd/ or flash/", kode_error());
    file_no_settle();
}

TEST_CASE("the status of a volume that is not there is an error", "[kode_file]")
{
    kode_file_status_t st;

    file_fresh();
    fake_media_set_sd_present(false);
    st = kode_file_status("sd/a.txt");
    TEST_ASSERT_FALSE(st.exists);
    TEST_ASSERT_EQUAL_STRING("sd/a.txt: the volume is not there or the call failed", kode_error());
    TEST_ASSERT_FALSE(kode_file_exists("sd/a.txt"));
    TEST_ASSERT_NOT_EQUAL_CHAR('\0', kode_error()[0]);
    file_no_settle();
}

TEST_CASE("exists is true for a file and a folder and false for nothing", "[kode_file]")
{
    file_fresh();
    put_fs_bytes("a.txt", 1U);
    fake_media_put_fs_dir("dir");
    TEST_ASSERT_TRUE(kode_file_exists("flash/a.txt"));
    TEST_ASSERT_TRUE(kode_file_exists("flash/dir"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    TEST_ASSERT_FALSE(kode_file_exists("flash/nope"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    TEST_ASSERT_FALSE(kode_file_exists("/flash/a.txt"));
    TEST_ASSERT_EQUAL_STRING("bad path '/flash/a.txt': start with sd/ or flash/", kode_error());
    file_no_settle();
}

TEST_CASE("a status waits out busy storage and says so past the bound", "[kode_file]")
{
    kode_file_status_t     st;
    kode_sys_file_status_t raw;

    file_fresh();
    put_fs_bytes("a.txt", TEST_SMALL_BYTES);
    file_no_settle();
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_file_status("/storage/x", &raw));
    file_no_settle();
    fake_time_reset();

    st = kode_file_status("flash/a.txt");
    TEST_ASSERT_FALSE(st.exists);
    TEST_ASSERT_EQUAL_STRING("flash/a.txt: storage is busy", kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_FILE_BUSY_MS, kode_sys_time_ms());

    file_will_settle();
    TEST_ASSERT_TRUE(kode_file_exists("flash/a.txt"));
    file_no_settle();
}

/** @brief The internal volume's size as the host fake reports it. */
#define TEST_SPACE_TOTAL (12ULL * 1024ULL * 1024ULL)

/** @brief What is free of it. */
#define TEST_SPACE_FREE (8ULL * 1024ULL * 1024ULL)

/** @brief A byte count no answer holds: what an output must keep when the call fails. */
#define TEST_SPACE_KEPT 7ULL

TEST_CASE("the space of a volume is its size and what is free", "[kode_file]")
{
    uint64_t total = TEST_SPACE_KEPT;
    uint64_t free_bytes = TEST_SPACE_KEPT;

    file_fresh();
    kode_err_set("%s", "stale");
    TEST_ASSERT_TRUE(kode_file_space("flash", &total, &free_bytes));
    TEST_ASSERT_TRUE(TEST_SPACE_TOTAL == total);
    TEST_ASSERT_TRUE(TEST_SPACE_FREE == free_bytes);
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    total      = 0U;
    free_bytes = 0U;
    TEST_ASSERT_TRUE(kode_file_space("FLASH/", &total, &free_bytes));
    TEST_ASSERT_TRUE(TEST_SPACE_TOTAL == total);
    file_no_settle();
}

TEST_CASE("the space of the card is asked by its name and is no more than the card", "[kode_file]")
{
    uint64_t total = 0U;
    uint64_t free_bytes = 0U;

    file_fresh();
    fake_media_set_sd_present(true);
    (void)kdrv_storage_mount(KDRV_MEDIUM_SD);
    TEST_ASSERT_TRUE(kode_file_space("sd", &total, &free_bytes));
    TEST_ASSERT_TRUE(0ULL < total);
    TEST_ASSERT_TRUE(free_bytes <= total);
    TEST_ASSERT_TRUE_MESSAGE(0ULL == (total % 1024ULL), "the figures are whole kilobytes");
    TEST_ASSERT_TRUE(0ULL == (free_bytes % 1024ULL));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    file_no_settle();
}

TEST_CASE("the space of a path inside a volume, of another name or of nothing is refused, storage not asked",
          "[kode_file]")
{
    uint64_t total = TEST_SPACE_KEPT;
    uint64_t free_bytes = TEST_SPACE_KEPT;

    file_fresh();
    TEST_ASSERT_FALSE(kode_file_space("flash/dir", &total, &free_bytes));
    TEST_ASSERT_EQUAL_STRING("'flash/dir' is a path, not a volume: sd or flash", kode_error());
    TEST_ASSERT_FALSE(kode_file_space("usb", &total, &free_bytes));
    TEST_ASSERT_EQUAL_STRING("bad path 'usb': start with sd/ or flash/", kode_error());
    TEST_ASSERT_FALSE(kode_file_space("/storage", &total, &free_bytes));
    TEST_ASSERT_FALSE(kode_file_space(NULL, &total, &free_bytes));
    TEST_ASSERT_EQUAL_STRING("bad path '(null)': start with sd/ or flash/", kode_error());
    TEST_ASSERT_TRUE(0ULL == total);
    TEST_ASSERT_TRUE(0ULL == free_bytes);
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Storage->eQueue));
    file_no_settle();
}

TEST_CASE("the space with no place for the sizes is refused, storage not asked", "[kode_file]")
{
    uint64_t total = TEST_SPACE_KEPT;

    file_fresh();
    TEST_ASSERT_FALSE(kode_file_space("flash", &total, NULL));
    TEST_ASSERT_EQUAL_STRING("flash: no place for the sizes", kode_error());
    TEST_ASSERT_TRUE(0ULL == total);
    TEST_ASSERT_FALSE(kode_file_space("flash", NULL, &total));
    TEST_ASSERT_EQUAL_STRING("flash: no place for the sizes", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Storage->eQueue));
    file_no_settle();
}

TEST_CASE("the space of a volume that is not there is an error and counts nothing", "[kode_file]")
{
    uint64_t total = TEST_SPACE_KEPT;
    uint64_t free_bytes = TEST_SPACE_KEPT;

    file_fresh();
    fake_media_set_sd_present(false);
    storage_request(KDRV_MEDIUM_SD, STORAGE_OP_UNMOUNT);
    file_settle_once();
    file_drain(AO_Storage);

    TEST_ASSERT_FALSE(kode_file_space("sd", &total, &free_bytes));
    TEST_ASSERT_EQUAL_STRING("sd: the volume is not there or the call failed", kode_error());
    TEST_ASSERT_TRUE(0ULL == total);
    TEST_ASSERT_TRUE(0ULL == free_bytes);
    file_no_settle();
}

TEST_CASE("the space waits out busy storage and says so past the bound", "[kode_file]")
{
    kode_sys_file_status_t raw;
    uint64_t               total = TEST_SPACE_KEPT;
    uint64_t               free_bytes = TEST_SPACE_KEPT;

    file_fresh();
    file_no_settle();
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_file_status("/storage/x", &raw));
    file_no_settle();
    fake_time_reset();

    TEST_ASSERT_FALSE(kode_file_space("flash", &total, &free_bytes));
    TEST_ASSERT_EQUAL_STRING("flash: storage is busy", kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_FILE_BUSY_MS, kode_sys_time_ms());
    TEST_ASSERT_TRUE(0ULL == total);
    TEST_ASSERT_TRUE(0ULL == free_bytes);

    /* Let the abandoned request land, so the next case finds a clean facade. */
    file_will_settle();
    TEST_ASSERT_TRUE(kode_file_space("flash", &total, &free_bytes));
    file_no_settle();
}

TEST_CASE("the space that storage never answers is a timeout, said as storage not answering", "[kode_file]")
{
    uint64_t total = TEST_SPACE_KEPT;
    uint64_t free_bytes = TEST_SPACE_KEPT;

    file_fresh();
    file_no_settle();
    TEST_ASSERT_FALSE(kode_file_space("flash", &total, &free_bytes));
    TEST_ASSERT_EQUAL_STRING("flash: storage did not answer", kode_error());
    TEST_ASSERT_TRUE(0ULL == total);

    /* The abandoned request is waited out by the next call. */
    file_will_settle();
    TEST_ASSERT_TRUE(kode_file_space("flash", &total, &free_bytes));
    file_no_settle();
}

/** @brief What kode_file_space() answered inside the probe Active Object. */
typedef struct {
    bool     ok;                      /**< Its answer.                 */
    char     error[KODE_ERROR_MAX];   /**< kode_error() right after.   */
    uint64_t total;                   /**< The total it was handed.    */
    uint64_t free_bytes;              /**< The free it was handed.     */
} SpaceFromAo;

/** @brief The probe's job: ask for a volume's space from inside an Active Object. */
static void space_from_ao_job(void *ctx)
{
    SpaceFromAo *const r = (SpaceFromAo *)ctx;

    r->ok = kode_file_space("flash", &r->total, &r->free_bytes);
    (void)snprintf(r->error, sizeof(r->error), "%s", kode_error());
}

TEST_CASE("the space is refused inside an Active Object, which is named", "[kode_file]")
{
    SpaceFromAo r = { true, "", TEST_SPACE_KEPT, TEST_SPACE_KEPT };

    file_fresh();
    file_no_settle();
    TEST_ASSERT_TRUE(probe_ao_run(&space_from_ao_job, &r, TEST_WAIT_MS));
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING("kode_file_space was called from an Active Object (" PROBE_AO_NAME
                             "); kode.h calls block - call kode_sys_* instead",
                             r.error);
    TEST_ASSERT_TRUE(0ULL == r.total);
    TEST_ASSERT_TRUE(0ULL == r.free_bytes);
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Storage->eQueue));
}

TEST_CASE("a folder of three entries lists them with kind and size", "[kode_file]")
{
    const kode_file_list_t  *list;
    const kode_file_entry_t *e;

    file_fresh();
    put_fs_bytes("one.txt", TEST_SMALL_BYTES);
    put_fs_bytes("two.bin", 1U);
    fake_media_put_fs_dir("sub");

    list = kode_file_list("flash");
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(TEST_FEW_FILES, list->count);
    TEST_ASSERT_FALSE(list->more);

    e = entry_named(list, "one.txt");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_FALSE(e->is_folder);
    TEST_ASSERT_EQUAL_UINT32(TEST_SMALL_BYTES, e->size);
    e = entry_named(list, "two.bin");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT32(1U, e->size);
    e = entry_named(list, "sub");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->is_folder);
    TEST_ASSERT_EQUAL_UINT32(0U, e->size);
    file_no_settle();
}

TEST_CASE("a sub folder lists by its typed path with or without a slash", "[kode_file]")
{
    const kode_file_list_t *list;

    file_fresh();
    put_fs_bytes("dir/a.txt", 2U);
    list = kode_file_list("flash/dir/");
    TEST_ASSERT_EQUAL_UINT32(1U, list->count);
    TEST_ASSERT_EQUAL_STRING("a.txt", list->entries[0].name);
    list = kode_file_list("flash/dir");
    TEST_ASSERT_EQUAL_UINT32(1U, list->count);
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    file_no_settle();
}

TEST_CASE("the card's root lists", "[kode_file]")
{
    const kode_file_list_t *list;

    file_fresh();
    fake_media_set_sd_present(true);
    fake_media_put_sd_path("card.txt", "abc", 3U);
    list = kode_file_list("sd");
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, list->count);
    TEST_ASSERT_EQUAL_STRING("card.txt", list->entries[0].name);
    TEST_ASSERT_EQUAL_UINT32(3U, list->entries[0].size);
    file_no_settle();
}

TEST_CASE("an empty folder lists as nothing with no error", "[kode_file]")
{
    const kode_file_list_t *list;

    file_fresh();
    fake_media_put_fs_dir("empty");
    list = kode_file_list("flash/empty");
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_EQUAL_UINT32(0U, list->count);
    TEST_ASSERT_FALSE(list->more);
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    file_no_settle();
}

TEST_CASE("a folder of thirty files lists twenty-four and says more", "[kode_file]")
{
    const kode_file_list_t *list;
    char                    name[TEST_NAME_BYTES];

    file_fresh();
    for ( uint32_t i = 0U; TEST_MANY_FILES > i; i++ )
    {
        (void)snprintf(name, sizeof(name), "f%02lu.txt", (unsigned long)i);
        put_fs_bytes(name, 1U);
    }
    list = kode_file_list("flash");
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_EQUAL_UINT32(KODE_FILE_LIST_MAX, list->count);
    TEST_ASSERT_TRUE(list->more);
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_NOT_EQUAL_CHAR('\0', list->entries[KODE_FILE_LIST_MAX - 1U].name[0]);
    file_no_settle();
}

TEST_CASE("a folder of exactly twenty-four files does not say more", "[kode_file]")
{
    const kode_file_list_t *list;
    char                    name[TEST_NAME_BYTES];

    file_fresh();
    for ( uint32_t i = 0U; KODE_FILE_LIST_MAX > i; i++ )
    {
        (void)snprintf(name, sizeof(name), "f%02lu.txt", (unsigned long)i);
        put_fs_bytes(name, 1U);
    }
    list = kode_file_list("flash");
    TEST_ASSERT_EQUAL_UINT32(KODE_FILE_LIST_MAX, list->count);
    TEST_ASSERT_FALSE(list->more);
    file_no_settle();
}

/** @brief Fill the internal volume's root with TEST_MANY_FILES files, f00.txt on. */
static void put_many_files(void)
{
    char name[TEST_NAME_BYTES];

    for ( uint32_t i = 0U; TEST_MANY_FILES > i; i++ )
    {
        (void)snprintf(name, sizeof(name), "f%02lu.txt", (unsigned long)i);
        put_fs_bytes(name, 1U);
    }
}

/** @brief Count each fNN.txt of @p list in @p seen, one slot a file. */
static void mark_seen(const kode_file_list_t *list, uint8_t seen[TEST_MANY_FILES])
{
    for ( uint32_t i = 0U; i < list->count; i++ )
    {
        const unsigned long n = strtoul(&list->entries[i].name[1], NULL, 10);

        if ( TEST_MANY_FILES > n )
        {
            seen[n]++;
        }
    }
}

TEST_CASE("a listing from the twenty-fourth entry gives the six left and no more", "[kode_file]")
{
    const kode_file_list_t *list;
    uint8_t                 seen[TEST_MANY_FILES] = { 0U };

    file_fresh();
    put_many_files();

    list = kode_file_list_from("flash", 0U);
    TEST_ASSERT_EQUAL_UINT32(KODE_FILE_LIST_MAX, list->count);
    TEST_ASSERT_TRUE(list->more);
    mark_seen(list, seen);

    list = kode_file_list_from("flash", KODE_FILE_LIST_MAX);
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_EQUAL_UINT32(TEST_MANY_FILES - KODE_FILE_LIST_MAX, list->count);
    TEST_ASSERT_FALSE(list->more);
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    mark_seen(list, seen);

    /* The two pages name every file once. */
    for ( uint32_t i = 0U; TEST_MANY_FILES > i; i++ )
    {
        TEST_ASSERT_EQUAL_UINT8(1U, seen[i]);
    }
    file_no_settle();
}

/** @brief Entries to skip so that exactly one full page is left of the many files. */
#define TEST_SKIP_TO_LAST_PAGE (TEST_MANY_FILES - KODE_FILE_LIST_MAX)

/** @brief A skip into the first page, two entries in. */
#define TEST_SKIP_SOME 2U

/** @brief How far past the last entry the too-far listing asks. */
#define TEST_SKIP_PAST 100U

/** @brief Whether a captured log line holds @p words. */
static bool file_log_says(const char *words)
{
    bool found = false;

    for ( unsigned i = 0U; (!found) && (fake_klog_count() > i); ++i )
    {
        found = (strstr(fake_klog_line(i), words) != NULL);
    }
    return found;
}

TEST_CASE("a listing that leaves exactly one page gives it whole and no more", "[kode_file]")
{
    const kode_file_list_t *list;

    file_fresh();
    put_many_files();

    list = kode_file_list_from("flash", TEST_SKIP_TO_LAST_PAGE);
    TEST_ASSERT_EQUAL_UINT32(KODE_FILE_LIST_MAX, list->count);
    TEST_ASSERT_FALSE(list->more);
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    file_no_settle();
}

TEST_CASE("a folder read page by page names every entry once and logs no warning", "[kode_file]")
{
    const kode_file_list_t *list;
    uint8_t                 seen[TEST_MANY_FILES] = { 0U };
    uint32_t                skip  = 0U;
    uint32_t                pages = 0U;

    file_fresh();
    put_many_files();
    fake_klog_reset();

    do
    {
        list = kode_file_list_from("flash", skip);
        mark_seen(list, seen);
        skip += list->count;
        pages++;
    } while ( list->more && (TEST_MANY_FILES > pages) );

    TEST_ASSERT_EQUAL_UINT32(2U, pages);
    TEST_ASSERT_EQUAL_UINT32(TEST_MANY_FILES, skip);
    for ( uint32_t i = 0U; TEST_MANY_FILES > i; i++ )
    {
        TEST_ASSERT_EQUAL_UINT8(1U, seen[i]);
    }
    /* A full page is how a long folder is read, not something that went wrong. */
    TEST_ASSERT_FALSE_MESSAGE(file_log_says("listing stopped"), "a full page was logged as a warning");
    file_no_settle();
}

TEST_CASE("a listing from the third entry of thirty is a full page and says more", "[kode_file]")
{
    const kode_file_list_t *list;
    char                    third[KODE_FILE_NAME_MAX];
    char                    fourth[KODE_FILE_NAME_MAX];

    file_fresh();
    put_many_files();

    /* The listing is the task's until its next one: keep the two names. */
    list = kode_file_list_from("flash", 0U);
    (void)snprintf(third, sizeof(third), "%s", list->entries[TEST_SKIP_SOME].name);
    (void)snprintf(fourth, sizeof(fourth), "%s", list->entries[TEST_SKIP_SOME + 1U].name);

    list = kode_file_list_from("flash", TEST_SKIP_SOME);
    TEST_ASSERT_EQUAL_UINT32(KODE_FILE_LIST_MAX, list->count);
    TEST_ASSERT_TRUE(list->more);
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_STRING(third, list->entries[0].name);
    TEST_ASSERT_EQUAL_STRING(fourth, list->entries[1].name);
    file_no_settle();
}

TEST_CASE("a listing from past the last entry is empty and is no error", "[kode_file]")
{
    const kode_file_list_t *list;

    file_fresh();
    put_many_files();

    list = kode_file_list_from("flash", TEST_MANY_FILES);
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_EQUAL_UINT32(0U, list->count);
    TEST_ASSERT_FALSE(list->more);
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    list = kode_file_list_from("flash", TEST_MANY_FILES + TEST_SKIP_PAST);
    TEST_ASSERT_EQUAL_UINT32(0U, list->count);
    TEST_ASSERT_FALSE(list->more);
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    file_no_settle();
}

TEST_CASE("a listing from an entry of a file or of nothing is empty and says why", "[kode_file]")
{
    const kode_file_list_t *list;

    file_fresh();
    put_fs_bytes("a.txt", 1U);

    list = kode_file_list_from("flash/a.txt", 1U);
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_EQUAL_UINT32(0U, list->count);
    TEST_ASSERT_EQUAL_STRING("flash/a.txt: it is a file", kode_error());

    list = kode_file_list_from("flash/nope", 1U);
    TEST_ASSERT_EQUAL_UINT32(0U, list->count);
    TEST_ASSERT_EQUAL_STRING("flash/nope: no such folder", kode_error());
    file_no_settle();
}

TEST_CASE("the list of a file or of nothing is empty and says why", "[kode_file]")
{
    const kode_file_list_t *list;

    file_fresh();
    put_fs_bytes("a.txt", 1U);

    list = kode_file_list("flash/a.txt");
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_EQUAL_UINT32(0U, list->count);
    TEST_ASSERT_FALSE(list->more);
    TEST_ASSERT_EQUAL_STRING("flash/a.txt: it is a file", kode_error());

    list = kode_file_list("flash/nope");
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_EQUAL_UINT32(0U, list->count);
    TEST_ASSERT_EQUAL_STRING("flash/nope: no such folder", kode_error());

    list = kode_file_list("usb");
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_EQUAL_UINT32(0U, list->count);
    TEST_ASSERT_EQUAL_STRING("bad path 'usb': start with sd/ or flash/", kode_error());

    list = kode_file_list(NULL);
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_EQUAL_UINT32(0U, list->count);
    file_no_settle();
}

TEST_CASE("a failed list empties the one the last call returned", "[kode_file]")
{
    const kode_file_list_t *list;

    file_fresh();
    put_fs_bytes("a.txt", 1U);
    list = kode_file_list("flash");
    TEST_ASSERT_EQUAL_UINT32(1U, list->count);
    list = kode_file_list("flash/nope");
    TEST_ASSERT_EQUAL_UINT32(0U, list->count);
    file_no_settle();
}

TEST_CASE("a list of a volume that is not there says so", "[kode_file]")
{
    const kode_file_list_t *list;

    file_fresh();
    fake_media_set_sd_present(false);
    list = kode_file_list("sd");
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_EQUAL_UINT32(0U, list->count);
    TEST_ASSERT_EQUAL_STRING("sd: the volume is not there or the call failed", kode_error());
    file_no_settle();
}

TEST_CASE("a new list reuses the task's block and leaks nothing", "[kode_file]")
{
    const kode_file_list_t *a;
    const kode_file_list_t *b;
    uint32_t                live;

    file_fresh();
    put_fs_bytes("a.txt", 1U);
    a = kode_file_list("flash");
    TEST_ASSERT_EQUAL_UINT32(1U, a->count);
    live = psram_live();
    b = kode_file_list("flash");
    TEST_ASSERT_EQUAL_PTR(a, b);
    TEST_ASSERT_EQUAL_UINT32(live, psram_live());
    (void)kode_file_list("flash/nope");
    TEST_ASSERT_EQUAL_UINT32(live, psram_live());
    file_no_settle();
}

/** @brief What the error of the refused-list task said. */
static char s_list_error[KODE_ERROR_MAX];

/** @brief A task whose list block PSRAM refuses. */
static void refused_list_task_ends(void *arg)
{
    ender_t *const e = (ender_t *)arg;

    /* The task's struct and its files state pass; the list block is refused. */
    fake_psram_refuse_after(2U, 1U);
    e->had_state = (0U == kode_file_list("flash")->count);
    (void)snprintf(s_list_error, sizeof(s_list_error), "%s", kode_error());
    fake_psram_refuse(0U);
    e->slot = khal_task_slot_get();
    fake_task_slot_watch(e->slot);
    (void)xSemaphoreGive(e->done);
}

TEST_CASE("a list that PSRAM refuses is empty and out of memory", "[kode_file]")
{
    ender_t e = { NULL, NULL, false, false, 0U };

    s_list_error[0] = '\0';
    run_state_task(&e, &refused_list_task_ends);
    TEST_ASSERT_TRUE(e.had_state);
    TEST_ASSERT_EQUAL_STRING("flash: out of memory", s_list_error);
}

/** @brief A task that lists a folder, then ends. */
static void lister_task_ends(void *arg)
{
    ender_t *const e = (ender_t *)arg;

    file_will_settle();
    e->had_state = (1U == kode_file_list("flash")->count);
    file_no_settle();
    e->slot = khal_task_slot_get();
    fake_task_slot_watch(e->slot);
    (void)xSemaphoreGive(e->done);
}

TEST_CASE("a task that listed a folder frees its list when it ends", "[kode_file]")
{
    ender_t e = { NULL, NULL, false, false, 0U };

    file_fresh();
    put_fs_bytes("a.txt", 1U);
    file_no_settle();
    run_state_task(&e, &lister_task_ends);
    TEST_ASSERT_TRUE(e.had_state);
}

TEST_CASE("a file is deleted", "[kode_file]")
{
    file_fresh();
    put_fs_bytes("a.txt", 1U);
    TEST_ASSERT_TRUE(kode_file_delete("flash/a.txt"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, host_size(fake_media_fs_dir(), "a.txt"));
    TEST_ASSERT_FALSE(kode_file_exists("flash/a.txt"));
    file_no_settle();
}

TEST_CASE("an empty folder is deleted", "[kode_file]")
{
    file_fresh();
    fake_media_put_fs_dir("dir");
    TEST_ASSERT_TRUE(kode_file_delete("flash/dir"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_FALSE(kode_file_exists("flash/dir"));
    file_no_settle();
}

TEST_CASE("a folder with something in it is not deleted", "[kode_file]")
{
    file_fresh();
    put_fs_bytes("dir/a.txt", 1U);
    TEST_ASSERT_FALSE(kode_file_delete("flash/dir"));
    TEST_ASSERT_EQUAL_STRING("flash/dir: the folder is not empty", kode_error());
    TEST_ASSERT_TRUE(kode_file_exists("flash/dir/a.txt"));
    file_no_settle();
}

TEST_CASE("deleting nothing says there is no such file", "[kode_file]")
{
    file_fresh();
    TEST_ASSERT_FALSE(kode_file_delete("flash/nope"));
    TEST_ASSERT_EQUAL_STRING("flash/nope: no such file", kode_error());
    TEST_ASSERT_FALSE(kode_file_delete("usb/a"));
    TEST_ASSERT_EQUAL_STRING("bad path 'usb/a': start with sd/ or flash/", kode_error());
    TEST_ASSERT_FALSE(kode_file_delete(NULL));
    file_no_settle();
}

TEST_CASE("a volume is not deleted, and storage is not asked", "[kode_file]")
{
    const char *const volumes[] = { "sd", "SD", "sd/", "flash", "Flash", "flash/" };
    char              want[TEST_LONG_BYTES];

    file_fresh();
    fake_media_set_sd_present(true);
    file_no_settle();
    fake_time_reset();
    for ( size_t i = 0U; (sizeof(volumes) / sizeof(volumes[0])) > i; i++ )
    {
        TEST_ASSERT_FALSE_MESSAGE(kode_file_delete(volumes[i]), volumes[i]);
        (void)snprintf(want, sizeof(want), "%s: a volume cannot be deleted", volumes[i]);
        TEST_ASSERT_EQUAL_STRING(want, kode_error());
    }
    TEST_ASSERT_EQUAL_UINT32(0U, kode_sys_time_ms());
    file_no_settle();
}

/** @brief Eject the card, so the next storage question fails. */
static bool change_card_ejected(void)
{
    fake_media_set_sd_present(false);
    return true;
}

TEST_CASE("a folder that exists but cannot be checked says why, not a file", "[kode_file]")
{
    file_fresh();
    fake_media_set_sd_present(true);
    fake_media_put_sd_path("dir/x", "a", 1U);

    /* Round trips: the create answers EXISTS (the first), then the check. */
    s_waits     = 0U;
    s_change_at = 1U;
    s_change    = &change_card_ejected;
    s_change_ok = false;
    fake_sem_wait_hook(&file_change_hook);
    TEST_ASSERT_FALSE(kode_file_create_folder("sd/dir"));
    TEST_ASSERT_TRUE(s_change_ok);
    TEST_ASSERT_EQUAL_STRING("sd/dir: the volume is not there or the call failed", kode_error());
    fake_media_set_sd_present(true);
    file_no_settle();
}

TEST_CASE("a folder is made", "[kode_file]")
{
    kode_file_status_t st;

    file_fresh();
    TEST_ASSERT_TRUE(kode_file_create_folder("flash/new"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    st = kode_file_status("flash/new");
    TEST_ASSERT_TRUE(st.exists);
    TEST_ASSERT_TRUE(st.is_folder);
    file_no_settle();
}

TEST_CASE("a folder that is already there is made without an error", "[kode_file]")
{
    file_fresh();
    fake_media_put_fs_dir("dir");
    TEST_ASSERT_TRUE(kode_file_create_folder("flash/dir"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_TRUE(kode_file_create_folder("flash/dir/"));
    file_no_settle();
}

TEST_CASE("a folder cannot take the name of a file", "[kode_file]")
{
    file_fresh();
    put_fs_bytes("a.txt", 1U);
    TEST_ASSERT_FALSE(kode_file_create_folder("flash/a.txt"));
    TEST_ASSERT_EQUAL_STRING("flash/a.txt: a file has that name", kode_error());
    TEST_ASSERT_FALSE(kode_file_status("flash/a.txt").is_folder);
    file_no_settle();
}

TEST_CASE("a folder whose parent is missing says so", "[kode_file]")
{
    file_fresh();
    TEST_ASSERT_FALSE(kode_file_create_folder("flash/up/down"));
    TEST_ASSERT_EQUAL_STRING("flash/up/down: the folder above does not exist", kode_error());
    TEST_ASSERT_FALSE(kode_file_exists("flash/up"));
    file_no_settle();
}

TEST_CASE("a folder inside a file says a folder in the path is a file", "[kode_file]")
{
    file_fresh();
    put_fs_bytes("a.txt", 1U);
    TEST_ASSERT_FALSE(kode_file_create_folder("flash/a.txt/sub"));
    TEST_ASSERT_EQUAL_STRING("flash/a.txt/sub: a folder in the path is a file", kode_error());
    file_no_settle();
}

TEST_CASE("a folder with a bad path is refused", "[kode_file]")
{
    file_fresh();
    TEST_ASSERT_FALSE(kode_file_create_folder("sd/../x"));
    TEST_ASSERT_EQUAL_STRING("bad path 'sd/../x': no empty, '.' or '..' part", kode_error());
    TEST_ASSERT_FALSE(kode_file_create_folder(NULL));
    TEST_ASSERT_EQUAL_STRING("bad path '(null)': start with sd/ or flash/", kode_error());
    file_no_settle();
}

/** @brief Bytes of the file moved across the volumes: three windows and a tail. */
#define TEST_MOVE_BYTES 70000U

/** @brief What the moved file is cut to while it is copied. */
#define TEST_CUT_BYTES 40000U

/** @brief Characters of the shortest name that makes "flash/<name>" outgrow KODE_FILE_PATH_MAX. */
#define TEST_OVERLONG_NAME 85U

/** @brief Characters of the longest name "flash/<name>" holds. */
#define TEST_LONGEST_NAME (TEST_OVERLONG_NAME - 1U)

/** @brief Characters of the longest name "sd/<name>" holds. */
#define TEST_LONGEST_SD_NAME (TEST_LONGEST_NAME + 3U)

/** @brief The error text of a name that is no name. */
#define TEST_BAD_NAME(n) "bad name '" n "'"

TEST_CASE("a file is renamed in place", "[kode_file]")
{
    file_fresh();
    put_fs_bytes("a.txt", TEST_SMALL_BYTES);
    TEST_ASSERT_TRUE(kode_file_rename("flash/a.txt", "b.txt"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, host_size(fake_media_fs_dir(), "a.txt"));
    TEST_ASSERT_EQUAL_UINT32(TEST_SMALL_BYTES, host_size(fake_media_fs_dir(), "b.txt"));
    TEST_ASSERT_EQUAL_STRING("xxxxxxxxxxxx", kode_file_read("flash/b.txt"));
    file_no_settle();
}

TEST_CASE("a file in a folder is renamed inside that folder", "[kode_file]")
{
    file_fresh();
    put_fs_bytes("dir/a.txt", 1U);
    TEST_ASSERT_TRUE(kode_file_rename("flash/dir/a.txt", "b.txt"));
    TEST_ASSERT_TRUE(kode_file_exists("flash/dir/b.txt"));
    TEST_ASSERT_FALSE(kode_file_exists("flash/dir/a.txt"));
    TEST_ASSERT_TRUE(kode_file_rename("flash/dir", "other"));
    TEST_ASSERT_TRUE(kode_file_exists("flash/other/b.txt"));
    TEST_ASSERT_FALSE(kode_file_exists("flash/dir"));
    file_no_settle();
}

TEST_CASE("a name that is not one name is refused", "[kode_file]")
{
    file_fresh();
    put_fs_bytes("a.txt", 1U);
    TEST_ASSERT_FALSE(kode_file_rename("flash/a.txt", "x/y"));
    TEST_ASSERT_EQUAL_STRING(TEST_BAD_NAME("x/y"), kode_error());
    TEST_ASSERT_FALSE(kode_file_rename("flash/a.txt", ""));
    TEST_ASSERT_EQUAL_STRING(TEST_BAD_NAME(""), kode_error());
    TEST_ASSERT_FALSE(kode_file_rename("flash/a.txt", "."));
    TEST_ASSERT_EQUAL_STRING(TEST_BAD_NAME("."), kode_error());
    TEST_ASSERT_FALSE(kode_file_rename("flash/a.txt", ".."));
    TEST_ASSERT_EQUAL_STRING(TEST_BAD_NAME(".."), kode_error());
    TEST_ASSERT_FALSE(kode_file_rename("flash/a.txt", NULL));
    TEST_ASSERT_EQUAL_STRING(TEST_BAD_NAME("(null)"), kode_error());
    TEST_ASSERT_TRUE(kode_file_exists("flash/a.txt"));
    file_no_settle();
}

TEST_CASE("a name too long for a path is refused, whole in the message", "[kode_file]")
{
    char name[TEST_LONG_BYTES];
    char want[TEST_LONG_BYTES];

    file_fresh();
    put_fs_bytes("a.txt", 1U);
    (void)memset(name, 'n', TEST_OVERLONG_NAME);
    name[TEST_OVERLONG_NAME] = '\0';
    TEST_ASSERT_FALSE(kode_file_rename("flash/a.txt", name));
    (void)snprintf(want, sizeof(want), "bad name '%.*s': the path would be too long",
                   (int)TEST_OVERLONG_NAME, name);
    TEST_ASSERT_EQUAL_STRING(want, kode_error());
    TEST_ASSERT_TRUE(kode_file_exists("flash/a.txt"));
    file_no_settle();
}

TEST_CASE("a renamed file keeps a path the caller can type again", "[kode_file]")
{
    char name[TEST_LONG_BYTES];
    char typed[TEST_LONG_BYTES];

    file_fresh();
    fake_media_set_sd_present(true);
    put_fs_bytes("a.txt", 1U);
    fake_media_put_sd_path("a.txt", "abc", 3U);

    /* The longest name the typed path holds is renamed, and found by its typed path. */
    (void)memset(name, 'n', TEST_LONGEST_NAME);
    name[TEST_LONGEST_NAME] = '\0';
    TEST_ASSERT_TRUE(kode_file_rename("flash/a.txt", name));
    (void)snprintf(typed, sizeof(typed), "flash/%.*s", (int)TEST_LONGEST_NAME, name);
    TEST_ASSERT_EQUAL_UINT32(KODE_FILE_PATH_MAX - 1U, (uint32_t)strlen(typed));
    TEST_ASSERT_TRUE(kode_file_exists(typed));

    /* On the card the volume word is shorter, so the same typed bound allows a longer name. */
    (void)memset(name, 'm', TEST_LONGEST_SD_NAME);
    name[TEST_LONGEST_SD_NAME] = '\0';
    TEST_ASSERT_TRUE(kode_file_rename("sd/a.txt", name));
    (void)snprintf(typed, sizeof(typed), "sd/%.*s", (int)TEST_LONGEST_SD_NAME, name);
    TEST_ASSERT_EQUAL_UINT32(KODE_FILE_PATH_MAX - 1U, (uint32_t)strlen(typed));
    TEST_ASSERT_TRUE(kode_file_exists(typed));

    /* One character more is refused on either volume, and nothing moved. */
    (void)memset(name, 'p', TEST_LONGEST_SD_NAME + 1U);
    name[TEST_LONGEST_SD_NAME + 1U] = '\0';
    TEST_ASSERT_FALSE(kode_file_rename(typed, name));
    TEST_ASSERT_TRUE(kode_file_exists(typed));
    name[TEST_OVERLONG_NAME] = '\0';
    put_fs_bytes("b.txt", 1U);
    TEST_ASSERT_FALSE(kode_file_rename("flash/b.txt", name));
    TEST_ASSERT_TRUE(kode_file_exists("flash/b.txt"));
    file_no_settle();
}

TEST_CASE("a name far too long is cut in the message, not overrun", "[kode_file]")
{
    char name[TEST_LONG_BYTES];

    file_fresh();
    put_fs_bytes("a.txt", 1U);
    (void)memset(name, 'n', sizeof(name) - 1U);
    name[sizeof(name) - 1U] = '\0';
    TEST_ASSERT_FALSE(kode_file_rename("flash/a.txt", name));
    TEST_ASSERT_LESS_THAN_UINT32(KODE_ERROR_MAX, (uint32_t)strlen(kode_error()));
    TEST_ASSERT_EQUAL_STRING("\xE2\x80\xA6", &kode_error()[strlen(kode_error()) - 3U]);
    TEST_ASSERT_TRUE(kode_file_exists("flash/a.txt"));
    file_no_settle();
}

TEST_CASE("a rename that only changes the case works", "[kode_file]")
{
    file_fresh();
    put_fs_bytes("a.txt", TEST_SMALL_BYTES);
    TEST_ASSERT_TRUE(kode_file_rename("flash/a.txt", "A.TXT"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(TEST_SMALL_BYTES, host_size(fake_media_fs_dir(), "A.TXT"));
    TEST_ASSERT_TRUE(kode_file_rename("flash/A.TXT", "a.txt"));
    TEST_ASSERT_EQUAL_UINT32(TEST_SMALL_BYTES, host_size(fake_media_fs_dir(), "a.txt"));
    file_no_settle();
}

TEST_CASE("a rename onto another name that differs in case is still taken", "[kode_file]")
{
    file_fresh();
    put_fs_bytes("a.txt", 1U);
    put_fs_bytes("b.txt", 2U);
    need_case_folding();
    TEST_ASSERT_FALSE(kode_file_rename("flash/a.txt", "B.TXT"));
    TEST_ASSERT_EQUAL_STRING("B.TXT: already exists", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, host_size(fake_media_fs_dir(), "a.txt"));
    file_no_settle();
}

TEST_CASE("a rename onto a taken name is refused and both stay", "[kode_file]")
{
    file_fresh();
    put_fs_bytes("a.txt", 1U);
    put_fs_bytes("b.txt", 2U);
    TEST_ASSERT_FALSE(kode_file_rename("flash/a.txt", "b.txt"));
    TEST_ASSERT_EQUAL_STRING("b.txt: already exists", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, host_size(fake_media_fs_dir(), "a.txt"));
    TEST_ASSERT_EQUAL_UINT32(2U, host_size(fake_media_fs_dir(), "b.txt"));
    file_no_settle();
}

TEST_CASE("renaming nothing says there is no such file", "[kode_file]")
{
    file_fresh();
    fake_time_reset();
    TEST_ASSERT_FALSE(kode_file_rename("flash/nope", "x"));
    TEST_ASSERT_EQUAL_STRING("flash/nope: no such file", kode_error());
    TEST_ASSERT_FALSE(kode_file_rename("usb/a", "x"));
    TEST_ASSERT_EQUAL_STRING("bad path 'usb/a': start with sd/ or flash/", kode_error());
    TEST_ASSERT_FALSE(kode_file_rename(NULL, "x"));
    TEST_ASSERT_EQUAL_UINT32(0U, kode_sys_time_ms());
    file_no_settle();
}

TEST_CASE("a volume is neither renamed nor moved, and storage is not asked", "[kode_file]")
{
    const char *const volumes[] = { "sd", "SD", "sd/", "flash", "Flash", "flash/" };
    char              want[TEST_LONG_BYTES];

    file_fresh();
    fake_media_set_sd_present(true);
    file_no_settle();
    fake_time_reset();
    for ( size_t i = 0U; (sizeof(volumes) / sizeof(volumes[0])) > i; i++ )
    {
        TEST_ASSERT_FALSE_MESSAGE(kode_file_rename(volumes[i], "x"), volumes[i]);
        (void)snprintf(want, sizeof(want), "%s: a volume cannot be renamed", volumes[i]);
        TEST_ASSERT_EQUAL_STRING(want, kode_error());
        TEST_ASSERT_FALSE_MESSAGE(kode_file_move(volumes[i], "flash/x"), volumes[i]);
        (void)snprintf(want, sizeof(want), "%s: a volume cannot be moved", volumes[i]);
        TEST_ASSERT_EQUAL_STRING(want, kode_error());
    }
    TEST_ASSERT_EQUAL_UINT32(0U, kode_sys_time_ms());
    file_no_settle();
}

TEST_CASE("a file moves within flash to another folder", "[kode_file]")
{
    file_fresh();
    put_fs_bytes("a.txt", TEST_SMALL_BYTES);
    fake_media_put_fs_dir("dir");
    TEST_ASSERT_TRUE(kode_file_move("flash/a.txt", "flash/dir/c.txt"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, host_size(fake_media_fs_dir(), "a.txt"));
    TEST_ASSERT_EQUAL_UINT32(TEST_SMALL_BYTES, host_size(fake_media_fs_dir(), "dir/c.txt"));
    file_no_settle();
}

TEST_CASE("a file of 70,000 bytes moves from flash to the card byte-exact", "[kode_file]")
{
    const uint8_t *bytes;
    uint32_t       len  = 0U;
    uint32_t       live;

    file_fresh();
    fake_media_set_sd_present(true);
    TEST_ASSERT_TRUE(host_pattern("big.bin", 0U, TEST_MOVE_BYTES, "wb"));
    live = psram_live();
    TEST_ASSERT_TRUE(kode_file_move("flash/big.bin", "sd/big.bin"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(live, psram_live());
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, host_size(fake_media_fs_dir(), "big.bin"));
    TEST_ASSERT_EQUAL_UINT32(TEST_MOVE_BYTES, host_size(fake_media_sd_dir(), "big.bin"));
    bytes = kode_file_read_bytes("sd/big.bin", &len);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT32(TEST_MOVE_BYTES, len);
    TEST_ASSERT_TRUE(pattern_is(bytes, 0U, len));
    file_no_settle();
}

TEST_CASE("a file of a whole window moves across the volumes", "[kode_file]")
{
    file_fresh();
    fake_media_set_sd_present(true);
    TEST_ASSERT_TRUE(host_pattern("w.bin", 0U, TEST_PART, "wb"));
    TEST_ASSERT_TRUE(kode_file_move("flash/w.bin", "sd/w.bin"));
    TEST_ASSERT_EQUAL_UINT32(TEST_PART, host_size(fake_media_sd_dir(), "w.bin"));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, host_size(fake_media_fs_dir(), "w.bin"));
    file_no_settle();
}

TEST_CASE("an empty file moves across the volumes", "[kode_file]")
{
    file_fresh();
    fake_media_set_sd_present(true);
    put_fs_bytes("e.txt", 0U);
    TEST_ASSERT_TRUE(kode_file_move("flash/e.txt", "sd/e.txt"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, host_size(fake_media_sd_dir(), "e.txt"));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, host_size(fake_media_fs_dir(), "e.txt"));
    file_no_settle();
}

TEST_CASE("a move into a missing folder fails and leaves the source alone", "[kode_file]")
{
    file_fresh();
    fake_media_set_sd_present(true);
    put_fs_bytes("a.txt", TEST_SMALL_BYTES);
    fake_time_reset();
    TEST_ASSERT_FALSE(kode_file_move("flash/a.txt", "flash/up/a.txt"));
    TEST_ASSERT_EQUAL_STRING("flash/up/a.txt: the folder above does not exist", kode_error());
    TEST_ASSERT_FALSE(kode_file_move("flash/a.txt", "sd/up/a.txt"));
    TEST_ASSERT_EQUAL_STRING("sd/up/a.txt: the folder above does not exist", kode_error());
    TEST_ASSERT_EQUAL_UINT32(TEST_SMALL_BYTES, host_size(fake_media_fs_dir(), "a.txt"));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, host_size(fake_media_sd_dir(), "up/a.txt"));
    TEST_ASSERT_FALSE(kode_file_exists("flash/up"));
    TEST_ASSERT_FALSE(kode_file_exists("sd/up"));
    TEST_ASSERT_EQUAL_UINT32(0U, kode_sys_time_ms());
    file_no_settle();
}

TEST_CASE("a move below a file says a folder in the path is a file", "[kode_file]")
{
    file_fresh();
    put_fs_bytes("a.txt", 1U);
    put_fs_bytes("b.txt", 1U);
    TEST_ASSERT_FALSE(kode_file_move("flash/a.txt", "flash/b.txt/c"));
    TEST_ASSERT_EQUAL_STRING("flash/b.txt/c: a folder in the path is a file", kode_error());
    file_no_settle();
}

TEST_CASE("a folder moves within a volume", "[kode_file]")
{
    file_fresh();
    put_fs_bytes("dir/a.txt", 1U);
    fake_media_put_fs_dir("dest");
    TEST_ASSERT_TRUE(kode_file_move("flash/dir", "flash/dest/dir"));
    TEST_ASSERT_TRUE(kode_file_exists("flash/dest/dir/a.txt"));
    TEST_ASSERT_FALSE(kode_file_exists("flash/dir"));
    file_no_settle();
}

TEST_CASE("a folder does not move across the volumes", "[kode_file]")
{
    file_fresh();
    fake_media_set_sd_present(true);
    put_fs_bytes("dir/a.txt", 1U);
    fake_time_reset();
    TEST_ASSERT_FALSE(kode_file_move("flash/dir", "sd/dir"));
    TEST_ASSERT_EQUAL_STRING("flash/dir: only files move between sd/ and flash/", kode_error());
    TEST_ASSERT_TRUE(kode_file_exists("flash/dir/a.txt"));
    TEST_ASSERT_FALSE(kode_file_exists("sd/dir"));
    TEST_ASSERT_EQUAL_UINT32(0U, kode_sys_time_ms());
    file_no_settle();
}

TEST_CASE("a move onto a path that is taken fails and both stay", "[kode_file]")
{
    file_fresh();
    fake_media_set_sd_present(true);
    put_fs_bytes("a.txt", 1U);
    put_fs_bytes("b.txt", 2U);
    fake_media_put_sd_path("s.txt", "abc", 3U);
    TEST_ASSERT_FALSE(kode_file_move("flash/a.txt", "flash/b.txt"));
    TEST_ASSERT_EQUAL_STRING("flash/b.txt: already exists", kode_error());
    TEST_ASSERT_FALSE(kode_file_move("flash/a.txt", "sd/s.txt"));
    TEST_ASSERT_EQUAL_STRING("sd/s.txt: already exists", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, host_size(fake_media_fs_dir(), "a.txt"));
    TEST_ASSERT_EQUAL_UINT32(2U, host_size(fake_media_fs_dir(), "b.txt"));
    TEST_ASSERT_EQUAL_UINT32(3U, host_size(fake_media_sd_dir(), "s.txt"));
    file_no_settle();
}

TEST_CASE("moving nothing, or by a bad path, fails with the reason", "[kode_file]")
{
    file_fresh();
    TEST_ASSERT_FALSE(kode_file_move("flash/nope", "flash/x"));
    TEST_ASSERT_EQUAL_STRING("flash/nope: no such file", kode_error());
    TEST_ASSERT_FALSE(kode_file_move("usb/a", "flash/x"));
    TEST_ASSERT_EQUAL_STRING("bad path 'usb/a': start with sd/ or flash/", kode_error());
    TEST_ASSERT_FALSE(kode_file_move("flash/a", NULL));
    TEST_ASSERT_EQUAL_STRING("bad path '(null)': start with sd/ or flash/", kode_error());
    TEST_ASSERT_FALSE(kode_file_move(NULL, "flash/a"));
    file_no_settle();
}

/** @brief Cut the file being moved short. */
static bool change_cut(void)
{
    return host_pattern("big.bin", 0U, TEST_CUT_BYTES, "wb");
}

/** @brief Settle a round trip; once the copy has begun on the card, do the change. */
static void file_copy_begun_hook(void)
{
    file_settle_once();
    if ( !s_change_ok && (host_size(fake_media_sd_dir(), "big.bin") != UINT32_MAX) )
    {
        s_change_ok = s_change();
    }
    fake_sem_wait_hook(&file_copy_begun_hook);
}

TEST_CASE("a copy that fails midway removes the copy and keeps the original", "[kode_file]")
{
    uint32_t live;

    file_fresh();
    fake_media_set_sd_present(true);
    TEST_ASSERT_TRUE(host_pattern("big.bin", 0U, TEST_MOVE_BYTES, "wb"));
    live = psram_live();
    s_change    = &change_cut;
    s_change_ok = false;
    fake_sem_wait_hook(&file_copy_begun_hook);
    TEST_ASSERT_FALSE(kode_file_move("flash/big.bin", "sd/big.bin"));
    TEST_ASSERT_TRUE(s_change_ok);
    TEST_ASSERT_EQUAL_STRING("flash/big.bin: changed while it was moved", kode_error());
    TEST_ASSERT_EQUAL_UINT32(TEST_CUT_BYTES, host_size(fake_media_fs_dir(), "big.bin"));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, host_size(fake_media_sd_dir(), "big.bin"));
    TEST_ASSERT_EQUAL_UINT32(live, psram_live());
    file_no_settle();
}

TEST_CASE("a copy that cannot write says the step and keeps the original", "[kode_file]")
{
    file_fresh();
    fake_media_set_sd_present(true);
    TEST_ASSERT_TRUE(host_pattern("big.bin", 0U, TEST_MOVE_BYTES, "wb"));
    s_change    = &change_card_ejected;
    s_change_ok = false;
    fake_sem_wait_hook(&file_copy_begun_hook);
    TEST_ASSERT_FALSE(kode_file_move("flash/big.bin", "sd/big.bin"));
    TEST_ASSERT_TRUE(s_change_ok);
    TEST_ASSERT_NOT_NULL(strstr(kode_error(), "flash/big.bin: move failed while writing the copy"));
    TEST_ASSERT_EQUAL_UINT32(TEST_MOVE_BYTES, host_size(fake_media_fs_dir(), "big.bin"));
    fake_media_set_sd_present(true);
    file_no_settle();
}

/** @brief Settle a round trip; once the copy is whole on flash, do the change. */
static void file_copy_whole_hook(void)
{
    file_settle_once();
    if ( !s_change_ok && (host_size(fake_media_fs_dir(), "big.bin") == TEST_MOVE_BYTES) )
    {
        s_change_ok = s_change();
    }
    fake_sem_wait_hook(&file_copy_whole_hook);
}

TEST_CASE("a move whose last step fails discards the copy and keeps the original", "[kode_file]")
{
    uint32_t live;

    file_fresh();
    fake_media_set_sd_present(true);
    TEST_ASSERT_TRUE(sys_put_pattern("/sdcard/big.bin", TEST_MOVE_BYTES));
    live = psram_live();
    /* The card goes the moment the copy on flash is whole: removing the original fails. */
    s_change    = &change_card_ejected;
    s_change_ok = false;
    fake_sem_wait_hook(&file_copy_whole_hook);
    TEST_ASSERT_FALSE(kode_file_move("sd/big.bin", "flash/big.bin"));
    TEST_ASSERT_TRUE(s_change_ok);
    TEST_ASSERT_EQUAL_STRING_LEN("sd/big.bin: move failed while removing the original: ",
                                 kode_error(), 52U);
    TEST_ASSERT_GREATER_THAN_UINT32(52U, (uint32_t)strlen(kode_error()));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, host_size(fake_media_fs_dir(), "big.bin"));
    TEST_ASSERT_EQUAL_UINT32(TEST_MOVE_BYTES, host_size(fake_media_sd_dir(), "big.bin"));
    TEST_ASSERT_EQUAL_UINT32(live, psram_live());
    fake_media_set_sd_present(true);
    file_no_settle();
}

/** @brief Empty the file being moved. */
static bool change_emptied(void)
{
    return host_pattern("big.bin", 0U, 0U, "wb");
}

TEST_CASE("a moved file emptied before its first window is reported changed, not busy",
          "[kode_file]")
{
    file_fresh();
    fake_media_set_sd_present(true);
    TEST_ASSERT_TRUE(host_pattern("big.bin", 0U, TEST_MOVE_BYTES, "wb"));
    /* Round trips: the source, the target, the folder above; then the first window. */
    s_waits     = 0U;
    s_change_at = 3U;
    s_change    = &change_emptied;
    s_change_ok = false;
    fake_sem_wait_hook(&file_change_hook);
    TEST_ASSERT_FALSE(kode_file_move("flash/big.bin", "sd/big.bin"));
    TEST_ASSERT_TRUE(s_change_ok);
    TEST_ASSERT_EQUAL_STRING("flash/big.bin: changed while it was moved", kode_error());
    TEST_ASSERT_LESS_THAN_UINT32(KODE_FILE_BUSY_MS, kode_sys_time_ms());
    TEST_ASSERT_EQUAL_UINT32(0U, host_size(fake_media_fs_dir(), "big.bin"));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, host_size(fake_media_sd_dir(), "big.bin"));
    file_no_settle();
}

TEST_CASE("a move that only changes the case works, as a rename does", "[kode_file]")
{
    file_fresh();
    fake_media_set_sd_present(true);
    put_fs_bytes("a.txt", TEST_SMALL_BYTES);
    fake_media_put_sd_path("s.txt", "abc", 3U);
    put_fs_bytes("dir/x.txt", 1U);

    TEST_ASSERT_TRUE(kode_file_move("flash/a.txt", "flash/A.TXT"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(TEST_SMALL_BYTES, host_size(fake_media_fs_dir(), "A.TXT"));
    TEST_ASSERT_TRUE(kode_file_move("sd/s.txt", "sd/S.TXT"));
    TEST_ASSERT_EQUAL_UINT32(3U, host_size(fake_media_sd_dir(), "S.TXT"));
    TEST_ASSERT_TRUE(kode_file_move("flash/dir", "flash/DIR"));
    TEST_ASSERT_TRUE(kode_file_exists("flash/DIR/x.txt"));
    file_no_settle();
}

TEST_CASE("a move onto a name that differs in case from another file is still refused",
          "[kode_file]")
{
    file_fresh();
    put_fs_bytes("a.txt", 1U);
    put_fs_bytes("b.txt", 2U);
    need_case_folding();
    TEST_ASSERT_FALSE(kode_file_move("flash/a.txt", "flash/B.TXT"));
    TEST_ASSERT_EQUAL_STRING("flash/B.TXT: already exists", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, host_size(fake_media_fs_dir(), "a.txt"));
    file_no_settle();
}

TEST_CASE("a folder is refused a move into itself, and storage is not asked to move it", "[kode_file]")
{
    const char *const inside[] = { "flash/dir/sub", "flash/dir/a/b", "flash/DIR/sub", "flash/dir",
                                   "flash/dir/" };

    file_fresh();
    put_fs_bytes("dir/a.txt", 1U);
    fake_media_put_fs_dir("dir/sub");
    for ( size_t i = 0U; (sizeof(inside) / sizeof(inside[0])) > i; i++ )
    {
        kode_err_set("%s", "");
        TEST_ASSERT_FALSE_MESSAGE(kode_file_move("flash/dir", inside[i]), inside[i]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("flash/dir: a folder cannot move into itself", kode_error(),
                                         inside[i]);
    }
    TEST_ASSERT_TRUE(kode_file_exists("flash/dir/a.txt"));
    TEST_ASSERT_TRUE(kode_file_status("flash/dir/sub").is_folder);
    file_no_settle();
}

TEST_CASE("a folder moves to a sibling that only shares its name's start", "[kode_file]")
{
    file_fresh();
    put_fs_bytes("dir/a.txt", 1U);
    TEST_ASSERT_TRUE(kode_file_move("flash/dir", "flash/dir2"));
    TEST_ASSERT_TRUE(kode_file_exists("flash/dir2/a.txt"));
    TEST_ASSERT_FALSE(kode_file_exists("flash/dir"));
    file_no_settle();
}

TEST_CASE("a file of 70,000 bytes moves from the card to flash byte-exact", "[kode_file]")
{
    const uint8_t *bytes;
    uint32_t       len = 0U;
    uint32_t       live;

    file_fresh();
    fake_media_set_sd_present(true);
    TEST_ASSERT_TRUE(sys_put_pattern("/sdcard/big.bin", TEST_MOVE_BYTES));
    live = psram_live();
    TEST_ASSERT_TRUE(kode_file_move("sd/big.bin", "flash/big.bin"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(live, psram_live());
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, host_size(fake_media_sd_dir(), "big.bin"));
    TEST_ASSERT_EQUAL_UINT32(TEST_MOVE_BYTES, host_size(fake_media_fs_dir(), "big.bin"));
    bytes = kode_file_read_bytes("flash/big.bin", &len);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT32(TEST_MOVE_BYTES, len);
    TEST_ASSERT_TRUE(pattern_is(bytes, 0U, len));
    file_no_settle();
}

TEST_CASE("from the card, a folder stays and a missing folder above fails", "[kode_file]")
{
    file_fresh();
    fake_media_set_sd_present(true);
    fake_media_put_sd_path("dir/a.txt", "a", 1U);
    fake_media_put_sd_path("f.txt", "abc", 3U);
    TEST_ASSERT_FALSE(kode_file_move("sd/dir", "flash/dir"));
    TEST_ASSERT_EQUAL_STRING("sd/dir: only files move between sd/ and flash/", kode_error());
    TEST_ASSERT_FALSE(kode_file_move("sd/f.txt", "flash/up/f.txt"));
    TEST_ASSERT_EQUAL_STRING("flash/up/f.txt: the folder above does not exist", kode_error());
    TEST_ASSERT_EQUAL_UINT32(3U, host_size(fake_media_sd_dir(), "f.txt"));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, host_size(fake_media_fs_dir(), "up/f.txt"));
    file_no_settle();
}

/* ---- A PNG file on a canvas ---------------------------------------------------- */

/**
 * A 2 x 2 RGBA PNG whose data chunk holds two bytes the stand-in inflater
 * ignores: it answers with the scanlines below.
 */
static const uint8_t TEST_PNG[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
    0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x02, 0x49, 0x44, 0x41, 0x54, 0x7A, 0x7A, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0x00, 0x00, 0x00, 0x00,
};

/** @brief Its scanlines: red, clear / half white, blue. */
static const uint8_t TEST_PNG_RAW[] = {
    0x00, 0xFF, 0x00, 0x00, 0xFF, 0x12, 0x34, 0x56, 0x00,
    0x00, 0xFF, 0xFF, 0xFF, 0x80, 0x00, 0x00, 0xFF, 0xFF,
};

/** @brief The canvas of the PNG cases: screen 1, place 0. */
static kode_canvas_t s_png_canvas;

/** @brief The pixels of that canvas. */
static kode_canvasbuf_t *s_png_buf;

/** @brief A fresh canvas of w by h on screen 1, and the inflater told what the PNG holds. */
static void png_case(uint32_t w, uint32_t h)
{
    file_fresh();
    fake_tinfl_reset();
    fake_tinfl_set_payload(TEST_PNG_RAW, sizeof(TEST_PNG_RAW));
    kode_canvasbuf_screen_changed(1U);
    s_png_canvas = kode_widget_make(1U, 0U);
    s_png_buf    = kode_canvasbuf_open(s_png_canvas, w, h);
    TEST_ASSERT_NOT_NULL(s_png_buf);
}

/** @brief Let the case's canvas go, and the inflater go back to its filler. */
static void png_case_end(void)
{
    kode_canvasbuf_retire(1U);
    fake_tinfl_reset();
    file_no_settle();
}

/** @brief The pixel (x, y) of the case's canvas. */
static uint16_t png_px(uint32_t x, uint32_t y)
{
    const kode_raster_t v = kode_canvasbuf_view(s_png_buf);

    return v.px[(y * v.w) + x];
}

TEST_CASE("a PNG file is drawn on a canvas at its offset, blended and clipped", "[kode_file]")
{
    png_case(6U, 6U);
    fake_media_put_fs_path("a.png", TEST_PNG, sizeof(TEST_PNG));
    /* The task's file buffer is taken by its first read and kept: start from there. */
    TEST_ASSERT_NOT_NULL(kode_file_read_bytes("flash/a.png", NULL));
    const uint32_t live = psram_live();

    TEST_ASSERT_TRUE(kode_draw_png(s_png_canvas, 1, 2, "flash/a.png"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_HEX16(0xF800U, png_px(1U, 2U));
    TEST_ASSERT_EQUAL_HEX16(0x0000U, png_px(2U, 2U));
    TEST_ASSERT_EQUAL_HEX16(0x8410U, png_px(1U, 3U));
    TEST_ASSERT_EQUAL_HEX16(0x001FU, png_px(2U, 3U));
    TEST_ASSERT_EQUAL_HEX16(0x0000U, png_px(0U, 0U));
    TEST_ASSERT_TRUE(kode_canvasbuf_take_dirty(s_png_buf));

    /* Half off the canvas: only the part on it shows, and it is no error. */
    TEST_ASSERT_TRUE(kode_draw_png(s_png_canvas, 5, 5, "flash/a.png"));
    TEST_ASSERT_EQUAL_HEX16(0xF800U, png_px(5U, 5U));
    TEST_ASSERT_TRUE(kode_draw_png(s_png_canvas, -1, -1, "flash/a.png"));
    TEST_ASSERT_EQUAL_HEX16(0x001FU, png_px(0U, 0U));
    TEST_ASSERT_TRUE(kode_draw_png(s_png_canvas, 100, 100, "flash/a.png"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    /* The decoded picture and the work buffers are all given back; the canvas is the case's. */
    TEST_ASSERT_EQUAL_UINT32(live, psram_live());
    png_case_end();
}

TEST_CASE("a PNG file that is missing, not a PNG, too big or cut says so with its path", "[kode_file]")
{
    static const uint8_t TOO_BIG_PNG[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x07, 0xD0, 0x00, 0x00, 0x00, 0x28,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };

    png_case(4U, 4U);
    fake_media_put_fs_path("prime.bin", "x", 1U);
    TEST_ASSERT_NOT_NULL(kode_file_read_bytes("flash/prime.bin", NULL));
    const uint32_t live = psram_live();

    TEST_ASSERT_FALSE(kode_draw_png(s_png_canvas, 0, 0, "flash/nope.png"));
    TEST_ASSERT_EQUAL_STRING("flash/nope.png: no such file", kode_error());

    fake_media_put_fs_path("t.png", "this is a text file", 19U);
    TEST_ASSERT_FALSE(kode_draw_png(s_png_canvas, 0, 0, "flash/t.png"));
    TEST_ASSERT_EQUAL_STRING("flash/t.png: not a PNG", kode_error());

    fake_media_put_fs_path("empty.png", "", 0U);
    TEST_ASSERT_FALSE(kode_draw_png(s_png_canvas, 0, 0, "flash/empty.png"));
    TEST_ASSERT_EQUAL_STRING("flash/empty.png: not a PNG", kode_error());

    fake_media_put_fs_path("big.png", TOO_BIG_PNG, sizeof(TOO_BIG_PNG));
    TEST_ASSERT_FALSE(kode_draw_png(s_png_canvas, 0, 0, "flash/big.png"));
    TEST_ASSERT_EQUAL_STRING("flash/big.png: too big for the glass (2000x40)", kode_error());

    fake_media_put_fs_path("cut.png", TEST_PNG, sizeof(TEST_PNG) - 9U);
    TEST_ASSERT_FALSE(kode_draw_png(s_png_canvas, 0, 0, "flash/cut.png"));
    TEST_ASSERT_EQUAL_STRING("flash/cut.png: damaged PNG", kode_error());

    TEST_ASSERT_FALSE(kode_canvasbuf_take_dirty(s_png_buf));
    TEST_ASSERT_EQUAL_UINT32(live, psram_live());
    png_case_end();
}

TEST_CASE("a PNG file is not read for a canvas that is gone", "[kode_file]")
{
    png_case(4U, 4U);
    TEST_ASSERT_FALSE(kode_draw_png(s_png_canvas + 1U, 0, 0, "flash/nope.png"));
    TEST_ASSERT_EQUAL_STRING("no such canvas", kode_error());
    TEST_ASSERT_FALSE(kode_draw_png(0U, 0, 0, "flash/nope.png"));
    TEST_ASSERT_EQUAL_STRING("no such canvas", kode_error());
    kode_canvasbuf_screen_changed(2U);
    TEST_ASSERT_FALSE(kode_draw_png(s_png_canvas, 0, 0, "flash/nope.png"));
    TEST_ASSERT_EQUAL_STRING("this canvas belongs to an old screen", kode_error());
    png_case_end();
}

TEST_CASE("a PNG drawn does not free what the task's last byte read returned", "[kode_file]")
{
    uint32_t       len = 0U;
    uint8_t        first[3];

    png_case(6U, 6U);
    fake_media_put_fs_path("a.png", TEST_PNG, sizeof(TEST_PNG));
    fake_media_put_fs_path("keep.bin", "abc", 3U);
    const uint8_t *kept = kode_file_read_bytes("flash/keep.bin", &len);

    TEST_ASSERT_NOT_NULL(kept);
    TEST_ASSERT_EQUAL_UINT32(3U, len);
    (void)memcpy(first, kept, sizeof(first));
    fake_task_slot_watch((void *)(uintptr_t)kept);
    TEST_ASSERT_TRUE(kode_draw_png(s_png_canvas, 0, 0, "flash/a.png"));
    TEST_ASSERT_FALSE(kode_draw_png(s_png_canvas, 0, 0, "flash/nope.png"));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)fake_task_slot_watch_frees());
    TEST_ASSERT_EQUAL_MEMORY(first, kept, sizeof(first));
    png_case_end();
}
