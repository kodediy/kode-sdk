/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_rfid_standard.c
 * @brief The standard layer's RFID: read an EM4100, write a T5577 and emulate a tag.
 */
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "fake_kdrv_rfid_lf.h"
#include "fake_khal_worker.h"
#include "fakes.h"
#include "kaos.h"
#include "khal_once.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "kode.h"
#include "kode_cert.h"
#include "kode_entry.h"
#include "kode_sys.h"
#include "kode_sys_priv.h"
#include "kode_tag.h"
#include "kqp.h"
#include "qf_support.h"

/** @brief Stack of a test task, in bytes. */
#define TEST_STACK_BYTES 8192U

/** @brief Priority of a test task: above the case's own, so it runs the moment it is let go. */
#define TEST_TASK_PRIO 10U

/** @brief The longest a case waits for a task, in ms. */
#define TEST_WAIT_MS 4000U

/** @brief How long the first reader sleeps inside its wait, so the second reaches the lock, in ms. */
#define TEST_YIELD_MS 100U

/** @brief QP ticks that outlast every deadline AO_Rfid arms in these cases. */
#define TEST_QUIET_TICKS (QF_SUPPORT_STEPS((uint32_t)KODE_RFID_TEST_BUDGET_MS) + 1U)

/** @brief Bytes of an EM4100 ID. */
#define TEST_ID_BYTES 5U

/** @brief The bit rate the faked decode reports. */
#define TEST_RF_DIV 64U

/** @brief The ID text of the tag the cases present. */
#define TEST_ID "01:02:03:04:05"

/** @brief The text of a call refused while the Dot plays a tag. */
#define TEST_EMULATING "RFID is emulating a tag; kode_rfid_emulate_stop() ends it"

/** @brief The text of a call that waited for a front end nobody frees. */
#define TEST_BUSY "RFID is busy"

/** @brief The text of a write the tag did not take. */
#define TEST_NOT_TAKEN "the tag did not take the ID (is it a T5577?)"

/** @brief Delays an outside holder keeps the front end for, in these cases. */
#define TEST_HOLD_DELAYS 3U

/* ---- The world ---------------------------------------------------------------- */

static const uint8_t s_tag_id[TEST_ID_BYTES] = { 0x01U, 0x02U, 0x03U, 0x04U, 0x05U };  /**< The tag on the antenna.   */
static uint32_t      s_delays;        /**< Delays any task made.                                  */
static uint32_t      s_release_in;    /**< Delays left before an outside holder gives the front end back; 0: none. */
static uint32_t      s_tag_in;        /**< Delays left before a tag comes to the antenna; 0: none. */
static bool          s_second_here;   /**< The first reader has slept inside its wait.            */

static SemaphoreHandle_t s_reader_in;    /**< Given when the first reader sleeps inside its wait.  */
static SemaphoreHandle_t s_reader_done;  /**< Given by each reader when its call returned.         */
static bool              s_reader_ok[2]; /**< What each answered.                                  */

/** @brief Run every job the worker holds, the AO's events between them, until nothing is left. */
static void world_pump(void)
{
    uint32_t moved;

    do
    {
        moved = 0U;
        while ( fake_khal_worker_run_next() )
        {
            moved += 1U;
        }
        moved += qf_support_settle(AO_Rfid);
    } while ( 0U < moved );
}

/* Stand-ins only record: an assertion there would skip the extended layer's claim release. */
/** @brief Whatever waits on AO_Rfid is answered, from inside the wait; armed again first. */
static void world_wait(void)
{
    fake_sem_wait_hook(&world_wait);
    world_pump();
    khal_sem_signal(NULL);
}

/** @brief The clock hook: count the delay, let an outside holder go, bring a tag; armed again first. */
static void world_delay(void)
{
    fake_time_delay_hook(&world_delay);
    s_delays++;
    if ( 0U < s_release_in )
    {
        s_release_in--;
        if ( 0U == s_release_in )
        {
            kode_sys_rfid_release();
        }
    }
    if ( 0U < s_tag_in )
    {
        s_tag_in--;
        if ( 0U == s_tag_in )
        {
            fake_rfid_set_window(0, s_tag_id, TEST_RF_DIV);
        }
    }
}

/**
 * @brief Bring AO_Rfid up afresh: registered once per binary, constructed for every case.
 */
static void rfid_up(void)
{
    const QEvt *stray = NULL;

    fake_rfid_reset();
    fake_khal_worker_reset();
    qf_support_init();

    if ( NULL != AO_Rfid->eQueue )
    {
        while ( xQueueReceive(AO_Rfid->eQueue, &stray, 0U) == pdTRUE )
        {
            QF_gc(stray);
        }
    }
    (void)qf_support_tick(AO_Rfid, TEST_QUIET_TICKS);

    (void)ao_spy();
    ao_spy_subscribe(RFID_TAG_SIG);
    Rfid_ctor();
    qf_support_give_queue(AO_Rfid);
    AO_Rfid->prio  = KODE_PRIO_RFID;
    AO_Rfid->pthre = KODE_PRIO_RFID;
    qf_support_register(AO_Rfid);
    while ( xQueueReceive(AO_Rfid->eQueue, &stray, 0U) == pdTRUE )
    {
        QF_gc(stray);
    }
    fake_klog_reset();
    QASM_INIT(AO_Rfid, (void *)0, 0U);
    ao_spy_drain();
    fake_sem_reset();
}

/** @brief A clean world: AO_Rfid with a front end that answers, real locks, hooks armed. */
static void world_fresh(void)
{
    /* A claim a failed case left behind is given back. */
    kode_sys_rfid_release();
    fake_mutex_real(true);
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    rfid_up();
    fake_time_reset();
    s_delays      = 0U;
    s_release_in  = 0U;
    s_tag_in      = 0U;
    s_second_here = false;
    fake_rfid_set_window(0, s_tag_id, TEST_RF_DIV);
    fake_sem_wait_hook(&world_wait);
    fake_time_delay_hook(&world_delay);
}

/** @brief The case is over: a tag is let go, nothing is hooked and the AO is idle. */
static void world_end(void)
{
    fake_sem_wait_hook(&world_wait);
    fake_time_delay_hook(&world_delay);
    kode_sys_rfid_release();
    (void)kode_rfid_emulate_stop();
    fake_sem_wait_hook(NULL);
    fake_time_delay_hook(NULL);
    ao_spy_drain();
}

/** @brief Read the ID the last CLONE job was asked to write and insist it is the tag's. */
static void clone_id_is(const uint8_t *want)
{
    uint8_t wrote[TEST_ID_BYTES];

    fake_rfid_last_clone_id(wrote);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, wrote, TEST_ID_BYTES);
}

/* ---- Reading --------------------------------------------------------------------- */

TEST_CASE("kode_rfid_read_id answers the ID as hex and the name EM4100", "[kode_rfid_standard]")
{
    static const uint8_t other[TEST_ID_BYTES] = { 0xABU, 0xCDU, 0xEFU, 0x01U, 0x23U };
    kode_tag_t           tag;

    world_fresh();
    tag = kode_rfid_read_id(500U);
    TEST_ASSERT_TRUE_MESSAGE(tag.ok, kode_error());
    TEST_ASSERT_EQUAL_STRING(TEST_ID, tag.id);
    TEST_ASSERT_EQUAL_STRING("EM4100", tag.name);

    fake_rfid_set_window(0, other, TEST_RF_DIV);
    tag = kode_rfid_read_id(500U);
    TEST_ASSERT_TRUE_MESSAGE(tag.ok, kode_error());
    TEST_ASSERT_EQUAL_STRING("AB:CD:EF:01:23", tag.id);
    world_end();
}

TEST_CASE("kode_rfid_read_id with nothing on the antenna says so, within its time", "[kode_rfid_standard]")
{
    uint32_t before;
    kode_tag_t tag;

    world_fresh();
    fake_rfid_set_window((int)KODE_TIMEOUT, NULL, 0U);
    before = kode_sys_time_ms();
    tag    = kode_rfid_read_id(100U);
    TEST_ASSERT_FALSE(tag.ok);
    TEST_ASSERT_EQUAL_STRING("no RFID tag in 100 ms", kode_error());
    TEST_ASSERT_EQUAL_STRING("", tag.id);
    TEST_ASSERT_TRUE((kode_sys_time_ms() - before) <= 100U);
    TEST_ASSERT_TRUE(fake_rfid_read_calls() >= 1);
    world_end();
}

TEST_CASE("kode_rfid_read_id with no time asks nothing", "[kode_rfid_standard]")
{
    world_fresh();
    TEST_ASSERT_FALSE(kode_rfid_read_id(0U).ok);
    TEST_ASSERT_EQUAL_STRING("no RFID tag in 0 ms", kode_error());
    TEST_ASSERT_EQUAL_INT(0, fake_rfid_read_calls());
    world_end();
}

TEST_CASE("kode_rfid_read_id for ever waits until a tag comes", "[kode_rfid_standard]")
{
    kode_tag_t tag;

    world_fresh();
    fake_rfid_set_window((int)KODE_TIMEOUT, NULL, 0U);
    s_tag_in = 5U;
    tag      = kode_rfid_read_id(KODE_FOREVER);
    TEST_ASSERT_TRUE_MESSAGE(tag.ok, kode_error());
    TEST_ASSERT_EQUAL_STRING(TEST_ID, tag.id);
    TEST_ASSERT_TRUE(fake_rfid_read_calls() >= 2);
    world_end();
}

/* ---- Writing --------------------------------------------------------------------- */

TEST_CASE("kode_rfid_write submits a clone of the ID and is true on a matching read-back", "[kode_rfid_standard]")
{
    world_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_rfid_write(TEST_ID, 500U), kode_error());
    TEST_ASSERT_EQUAL_INT(1, fake_rfid_clone_calls());
    clone_id_is(s_tag_id);
    world_end();
}

TEST_CASE("kode_rfid_write takes the ID with or without separators, in any case", "[kode_rfid_standard]")
{
    static const char *const forms[] = { "0102030405", "01-02-03-04-05", "01 02 03 04 05", "01:02:03:04:05" };
    static const uint8_t     lower[TEST_ID_BYTES] = { 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU };

    world_fresh();
    for ( uint32_t i = 0U; (sizeof(forms) / sizeof(forms[0])) > i; i++ )
    {
        TEST_ASSERT_TRUE_MESSAGE(kode_rfid_write(forms[i], 500U), forms[i]);
        clone_id_is(s_tag_id);
    }
    TEST_ASSERT_TRUE_MESSAGE(kode_rfid_write("0a:0b:0c:0d:0e", 500U), kode_error());
    clone_id_is(lower);
    world_end();
}

TEST_CASE("kode_rfid_write says the tag did not take the ID when the read-back differs", "[kode_rfid_standard]")
{
    world_fresh();
    fake_rfid_set_clone_mismatch(true);
    TEST_ASSERT_FALSE(kode_rfid_write(TEST_ID, 500U));
    TEST_ASSERT_EQUAL_STRING(TEST_NOT_TAKEN, kode_error());
    TEST_ASSERT_EQUAL_INT(1, fake_rfid_clone_calls());
    world_end();
}

TEST_CASE("kode_rfid_write refuses an ID that is not five bytes before asking anything", "[kode_rfid_standard]")
{
    world_fresh();
    TEST_ASSERT_FALSE(kode_rfid_write("010203", 500U));
    TEST_ASSERT_EQUAL_STRING("an EM4100 ID is 5 bytes ('010203')", kode_error());
    TEST_ASSERT_FALSE(kode_rfid_write("01:02:03:04:05:06", 500U));
    TEST_ASSERT_EQUAL_STRING("an EM4100 ID is 5 bytes ('01:02:03:04:05:06')", kode_error());
    TEST_ASSERT_FALSE(kode_rfid_write("zz", 500U));
    TEST_ASSERT_EQUAL_STRING("an EM4100 ID is 5 bytes ('zz')", kode_error());
    TEST_ASSERT_FALSE(kode_rfid_write("", 500U));
    TEST_ASSERT_EQUAL_STRING("an EM4100 ID is 5 bytes ('')", kode_error());
    TEST_ASSERT_FALSE(kode_rfid_write(NULL, 500U));
    TEST_ASSERT_EQUAL_STRING("no RFID ID given", kode_error());
    TEST_ASSERT_EQUAL_INT(0, fake_rfid_clone_calls());
    world_end();
}

TEST_CASE("kode_rfid_write with no time asks nothing, and for ever is one try", "[kode_rfid_standard]")
{
    world_fresh();
    TEST_ASSERT_FALSE(kode_rfid_write(TEST_ID, 0U));
    TEST_ASSERT_EQUAL_STRING("no RFID tag in 0 ms", kode_error());
    TEST_ASSERT_EQUAL_INT(0, fake_rfid_clone_calls());
    TEST_ASSERT_TRUE_MESSAGE(kode_rfid_write(TEST_ID, KODE_FOREVER), kode_error());
    TEST_ASSERT_EQUAL_INT(1, fake_rfid_clone_calls());
    world_end();
}

/* ---- Emulating ------------------------------------------------------------------- */

TEST_CASE("kode_rfid_emulate_start plays the ID, then a read and a write are refused at once", "[kode_rfid_standard]")
{
    uint8_t played[TEST_ID_BYTES];

    world_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_rfid_emulate_start(TEST_ID), kode_error());
    TEST_ASSERT_TRUE(kode_sys_rfid_is_emulating());
    TEST_ASSERT_EQUAL_UINT32(1U, fake_rfid_emulate_calls());
    fake_rfid_last_emulate_id(played);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_tag_id, played, TEST_ID_BYTES);

    TEST_ASSERT_FALSE(kode_rfid_read_id(500U).ok);
    TEST_ASSERT_EQUAL_STRING(TEST_EMULATING, kode_error());
    TEST_ASSERT_FALSE(kode_rfid_write(TEST_ID, 500U));
    TEST_ASSERT_EQUAL_STRING(TEST_EMULATING, kode_error());
    TEST_ASSERT_EQUAL_INT(0, fake_rfid_read_calls());
    TEST_ASSERT_EQUAL_INT(0, fake_rfid_clone_calls());
    TEST_ASSERT_EQUAL_UINT32(0U, s_delays);
    world_end();
}

TEST_CASE("kode_rfid_emulate_start while emulating stops the old tag, then plays the new one", "[kode_rfid_standard]")
{
    static const uint8_t second[TEST_ID_BYTES] = { 0x11U, 0x22U, 0x33U, 0x44U, 0x55U };
    uint8_t              played[TEST_ID_BYTES];

    world_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_rfid_emulate_start(TEST_ID), kode_error());
    TEST_ASSERT_TRUE_MESSAGE(kode_rfid_emulate_start("11:22:33:44:55"), kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, fake_rfid_emu_stop_calls());
    TEST_ASSERT_EQUAL_UINT32(2U, fake_rfid_emulate_calls());
    TEST_ASSERT_TRUE(fake_rfid_emulating());
    fake_rfid_last_emulate_id(played);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(second, played, TEST_ID_BYTES);
    world_end();
}

TEST_CASE("kode_rfid_emulate_stop ends the tag and reads work again", "[kode_rfid_standard]")
{
    world_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_rfid_emulate_start(TEST_ID), kode_error());
    TEST_ASSERT_TRUE_MESSAGE(kode_rfid_emulate_stop(), kode_error());
    TEST_ASSERT_FALSE(kode_sys_rfid_is_emulating());
    TEST_ASSERT_FALSE(fake_rfid_emulating());
    TEST_ASSERT_EQUAL_UINT32(1U, fake_rfid_emu_stop_calls());
    TEST_ASSERT_TRUE_MESSAGE(kode_rfid_read_id(500U).ok, kode_error());
    TEST_ASSERT_TRUE_MESSAGE(kode_rfid_write(TEST_ID, 500U), kode_error());
    world_end();
}

TEST_CASE("kode_rfid_emulate_stop with nothing played is true and stops nothing", "[kode_rfid_standard]")
{
    world_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_rfid_emulate_stop(), kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_rfid_emu_stop_calls());
    world_end();
}

TEST_CASE("kode_rfid_emulate_start refuses a bad ID before touching the tag playing", "[kode_rfid_standard]")
{
    world_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_rfid_emulate_start(TEST_ID), kode_error());
    TEST_ASSERT_FALSE(kode_rfid_emulate_start("0102"));
    TEST_ASSERT_EQUAL_STRING("an EM4100 ID is 5 bytes ('0102')", kode_error());
    TEST_ASSERT_FALSE(kode_rfid_emulate_start(NULL));
    TEST_ASSERT_EQUAL_STRING("no RFID ID given", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_rfid_emu_stop_calls());
    TEST_ASSERT_TRUE(kode_sys_rfid_is_emulating());
    world_end();
}

TEST_CASE("kode_rfid_emulate_start says why when the driver refuses the tag", "[kode_rfid_standard]")
{
    world_fresh();
    fake_rfid_set_emulate_rc((uint32_t)KODE_NO_RESOURCES);
    TEST_ASSERT_FALSE(kode_rfid_emulate_start(TEST_ID));
    TEST_ASSERT_EQUAL_STRING("no memory for the RFID front end", kode_error());
    TEST_ASSERT_FALSE(kode_sys_rfid_is_emulating());
    TEST_ASSERT_TRUE_MESSAGE(kode_rfid_read_id(500U).ok, kode_error());
    world_end();
}

TEST_CASE("kode_rfid_emulate_start that times out and plays no tag fails after a bounded look", "[kode_rfid_standard]")
{
    world_fresh();
    fake_rfid_set_emulate_rc((uint32_t)KODE_TIMEOUT);

    const uint32_t before = kode_sys_time_ms();

    TEST_ASSERT_FALSE(kode_rfid_emulate_start(TEST_ID));
    TEST_ASSERT_EQUAL_STRING("the RFID front end did not answer", kode_error());
    TEST_ASSERT_FALSE(kode_sys_rfid_is_emulating());
    TEST_ASSERT_TRUE((kode_sys_time_ms() - before) <= 500U);
    world_end();
}

/* ---- A front end held from outside the layer --------------------------------------- */

TEST_CASE("a stop behind an outside holder is tried again until it goes through", "[kode_rfid_standard]")
{
    uint32_t gen = 0U;

    world_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_rfid_emulate_start(TEST_ID), kode_error());
    TEST_ASSERT_TRUE(kode_sys_rfid_claim(&gen));
    s_release_in = TEST_HOLD_DELAYS;

    TEST_ASSERT_TRUE_MESSAGE(kode_rfid_emulate_stop(), kode_error());
    TEST_ASSERT_FALSE(kode_sys_rfid_is_emulating());
    TEST_ASSERT_EQUAL_UINT32(1U, fake_rfid_emu_stop_calls());
    world_end();
}

TEST_CASE("a stop behind a holder that never lets go says the front end is busy, within its bound", "[kode_rfid_standard]")
{
    uint32_t gen = 0U;
    uint32_t before;

    world_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_rfid_emulate_start(TEST_ID), kode_error());
    TEST_ASSERT_TRUE(kode_sys_rfid_claim(&gen));
    before = kode_sys_time_ms();

    TEST_ASSERT_FALSE(kode_rfid_emulate_stop());
    TEST_ASSERT_EQUAL_STRING(TEST_BUSY, kode_error());
    TEST_ASSERT_TRUE((kode_sys_time_ms() - before) <= KODE_RFID_BUSY_MS);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_rfid_emu_stop_calls());
    TEST_ASSERT_TRUE(kode_sys_rfid_is_emulating());
    world_end();
}

TEST_CASE("a replacing emulation behind a holder that never lets go leaves the old tag playing", "[kode_rfid_standard]")
{
    uint32_t gen = 0U;

    world_fresh();
    TEST_ASSERT_TRUE_MESSAGE(kode_rfid_emulate_start(TEST_ID), kode_error());
    TEST_ASSERT_TRUE(kode_sys_rfid_claim(&gen));

    TEST_ASSERT_FALSE(kode_rfid_emulate_start("11:22:33:44:55"));
    TEST_ASSERT_EQUAL_STRING(TEST_BUSY, kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, fake_rfid_emulate_calls());
    TEST_ASSERT_TRUE(kode_sys_rfid_is_emulating());
    world_end();
}

TEST_CASE("a start behind an outside holder is tried again until it goes through", "[kode_rfid_standard]")
{
    uint32_t gen = 0U;

    world_fresh();
    TEST_ASSERT_TRUE(kode_sys_rfid_claim(&gen));
    s_release_in = TEST_HOLD_DELAYS;

    TEST_ASSERT_TRUE_MESSAGE(kode_rfid_emulate_start(TEST_ID), kode_error());
    TEST_ASSERT_TRUE(kode_sys_rfid_is_emulating());
    world_end();
}

TEST_CASE("a read behind a holder that never lets go says the front end is busy, within its bound", "[kode_rfid_standard]")
{
    uint32_t gen = 0U;
    uint32_t before;

    world_fresh();
    TEST_ASSERT_TRUE(kode_sys_rfid_claim(&gen));
    before = kode_sys_time_ms();

    TEST_ASSERT_FALSE(kode_rfid_read_id(KODE_FOREVER).ok);
    TEST_ASSERT_EQUAL_STRING(TEST_BUSY, kode_error());
    TEST_ASSERT_TRUE((kode_sys_time_ms() - before) <= KODE_RFID_BUSY_MS);
    TEST_ASSERT_EQUAL_INT(0, fake_rfid_read_calls());

    /* The wait for a holder is part of the time asked for, never past it. */
    before = kode_sys_time_ms();
    TEST_ASSERT_FALSE(kode_rfid_write(TEST_ID, 100U));
    TEST_ASSERT_EQUAL_STRING(TEST_BUSY, kode_error());
    TEST_ASSERT_TRUE((kode_sys_time_ms() - before) <= 100U);
    TEST_ASSERT_EQUAL_INT(0, fake_rfid_clone_calls());
    world_end();
}

TEST_CASE("a read behind a holder that lets go is served", "[kode_rfid_standard]")
{
    uint32_t gen = 0U;

    world_fresh();
    TEST_ASSERT_TRUE(kode_sys_rfid_claim(&gen));
    s_release_in = TEST_HOLD_DELAYS;

    TEST_ASSERT_TRUE_MESSAGE(kode_rfid_read_id(1000U).ok, kode_error());
    world_end();
}

/* ---- Two tasks at once ------------------------------------------------------------- */

/** @brief The first reader sleeps inside its wait, so the second reaches the lock; both then get answered. */
static void reader_wait(void)
{
    fake_sem_wait_hook(&reader_wait);
    if ( !s_second_here )
    {
        s_second_here = true;
        (void)xSemaphoreGive(s_reader_in);
        vTaskDelay(pdMS_TO_TICKS(TEST_YIELD_MS));
    }
    world_pump();
    khal_sem_signal(NULL);
}

/** @brief A task that reads one tag. */
static void reader_task(void *arg)
{
    const uint32_t index = (uint32_t)(uintptr_t)arg;

    fake_sem_wait_hook(&reader_wait);
    s_reader_ok[index] = kode_rfid_read_id(1000U).ok;
    (void)xSemaphoreGive(s_reader_done);
}

TEST_CASE("two tasks reading at once are served one after the other", "[kode_rfid_standard]")
{
    world_fresh();
    s_reader_in   = xSemaphoreCreateBinary();
    s_reader_done = xSemaphoreCreateCounting(2U, 0U);
    s_reader_ok[0] = false;
    s_reader_ok[1] = false;

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&reader_task, "t_rfid1", TEST_STACK_BYTES, TEST_TASK_PRIO,
                                                    (void *)(uintptr_t)0U));
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_reader_in, pdMS_TO_TICKS(TEST_WAIT_MS)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&reader_task, "t_rfid2", TEST_STACK_BYTES, TEST_TASK_PRIO,
                                                    (void *)(uintptr_t)1U));
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_reader_done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_reader_done, pdMS_TO_TICKS(TEST_WAIT_MS)));

    TEST_ASSERT_TRUE(s_reader_ok[0]);
    TEST_ASSERT_TRUE(s_reader_ok[1]);
    TEST_ASSERT_EQUAL_INT(2, fake_rfid_read_calls());
    /* Nobody was turned away by a front end somebody else held: no retry slept. */
    TEST_ASSERT_EQUAL_UINT32(0U, s_delays);
    vSemaphoreDelete(s_reader_in);
    vSemaphoreDelete(s_reader_done);
    world_end();
}
