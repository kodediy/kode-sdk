/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_nfc_standard.c
 * @brief The standard layer's NFC: read, write text or links, background reads and emulation.
 */
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "unity.h"

#include "Mockkdrv_st25r3916.h"
#include "qpc.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "khal_once.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "knfc_emulate.h"
#include "knfc_tag.h"
#include "kode.h"
#include "kode_entry.h"
#include "kode_nfc.h"
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

/** @brief Ticks of 10 ms a served request may take before the world gives up on it. */
#define TEST_TICKS_MAX 400U

/** @brief Events the world lets AO_Nfc dispatch for one request. */
#define TEST_PUMP_MAX 8U

/** @brief Windows a case can script. */
#define TEST_WINDOWS_MAX 8U

/** @brief Hits the world remembers the time of. */
#define TEST_HITS_MAX 8U

/** @brief Bytes of the fake tag: four head pages and the 144-byte data area its capability container declares. */
#define TEST_T2T_BYTES 160U

/** @brief Where the data area starts in the fake tag. */
#define TEST_T2T_AREA 16U

/** @brief Bytes a Type-2 read returns. */
#define TEST_T2T_READ 16U

/** @brief Bytes of one Type-2 page. */
#define TEST_T2T_PAGE 4U

/** @brief Bytes of the largest message a case builds. */
#define TEST_TEXT_BYTES 128U

/** @brief The ATQA and SAK of the Type-2 tag. */
#define TEST_ATQA 0x0044U
#define TEST_SAK  0x00U

/** @brief The ID text of the tag the cases present. */
#define TEST_ID "04:A2:3B:91"

/** @brief The text kode_nfc_read() and the error say for a tag that holds the other type. */
#define TEST_EMULATING "NFC is emulating a card; kode_nfc_emulate_stop() ends it"

/** @brief The text of a call that waited for a reader nobody frees. */
#define TEST_BUSY "NFC is busy, or no reader is fitted"

/* ---- The world ---------------------------------------------------------------- */

static uint8_t  s_uid[KNFC_UID_MAX];       /**< The UID on the antenna.                          */
static uint8_t  s_uid_len;                 /**< Its length.                                      */
static uint16_t s_atqa;                    /**< Its ATQA.                                        */
static uint8_t  s_sak;                     /**< Its SAK.                                         */
static uint8_t  s_mem[TEST_T2T_BYTES];     /**< The fake tag's memory.                           */
static bool     s_tag_here;                /**< A tag is on the antenna for the open window.     */
static bool     s_tag_default;             /**< Whether a window past the script has one.        */
static bool     s_script[TEST_WINDOWS_MAX];  /**< Whether the window of that index has a tag.    */
static uint32_t s_script_count;            /**< Windows scripted.                                */
static uint32_t s_windows;                 /**< Windows AO_Nfc has opened.                       */
static uint32_t s_hits;                    /**< Tags the world has handed out.                   */
static uint32_t s_hit_ms[TEST_HITS_MAX];   /**< The fake clock at each.                          */
static knfc_emu_card_t s_card;             /**< The card AO_Nfc last told the driver to present. */
static uint32_t s_emu_starts;              /**< Presentations started.                           */
static uint32_t s_emu_stops;               /**< Presentations stopped.                           */
static bool     s_emu_active;              /**< The driver's window is up.                       */
static bool     s_emu_stopping;            /**< A stop was told to the driver, not yet released. */
static bool     s_emu_hold;                /**< The driver never lets the reader go.             */
static uint32_t s_delays;                  /**< Delays any task made.                            */

static TaskHandle_t      s_case_task;      /**< The task the case runs in.                       */
static bool              s_park_at_end;    /**< The next window past the script parks the task.  */
static bool              s_release_next_delay;  /**< The case's next delay lets a parked window go. */
static bool              s_yield_case;     /**< The case's delays let the other tasks run.       */
static bool              s_yield_others;   /**< Every other task's delays let the rest run.      */
static bool              s_tick_on_delay;  /**< The case's delays tick AO_Nfc.                   */
static uint32_t          s_claim_tries;    /**< The case's delays left before an outside holder gives the reader back; 0: none. */
static SemaphoreHandle_t s_world_in;       /**< Given when a window is parked.                   */
static SemaphoreHandle_t s_world_go;       /**< Lets the parked window end.                      */

/** @brief The fake tag's memory: blank, formatted for 144 bytes of NDEF, writable. */
static void mem_blank(void)
{
    (void)memset(s_mem, 0, sizeof(s_mem));
    s_mem[0]  = 0x04U;
    s_mem[1]  = 0xA2U;
    s_mem[2]  = 0x3BU;
    s_mem[12] = 0xE1U;
    s_mem[13] = 0x10U;
    s_mem[14] = 0x12U;
    s_mem[15] = 0x00U;
}

/**
 * @brief Put a data area in the fake tag.
 * @param area The bytes, TLV and all.
 * @param n    How many.
 */
static void mem_area(const uint8_t *area, uint32_t n)
{
    mem_blank();
    (void)memcpy(&s_mem[TEST_T2T_AREA], area, n);
}

/** @brief The area of a tag that holds the text "hi". */
static const uint8_t s_area_text[] = {
    0x03U, 0x09U, 0xD1U, 0x01U, 0x05U, 'T', 0x02U, 'e', 'n', 'h', 'i', 0xFEU,
};

/** @brief The area of a tag that holds the link "https://a.io". */
static const uint8_t s_area_uri[] = {
    0x03U, 0x09U, 0xD1U, 0x01U, 0x05U, 'U', 0x04U, 'a', '.', 'i', 'o', 0xFEU,
};

/** @brief Whether a window of the script has a tag; windows past it follow s_tag_default. */
static bool window_has_tag(uint32_t index)
{
    bool here = s_tag_default;

    if ( index < s_script_count )
    {
        here = s_script[index];
    }
    return here;
}

static kode_status_t cb_discover(int calls)
{
    (void)calls;
    s_tag_here = window_has_tag(s_windows);
    s_windows++;
    return KODE_OK;
}

static bool cb_activated(int calls)
{
    (void)calls;
    return s_tag_here;
}

static kode_status_t cb_active_tag(uint8_t *uid, uint8_t *uid_len, uint16_t *atqa, uint8_t *sak, int calls)
{
    (void)calls;
    (void)memset(uid, 0, KDRV_ST25R3916_UID_MAX);
    (void)memcpy(uid, s_uid, s_uid_len);
    *uid_len = s_uid_len;
    *atqa    = s_atqa;
    *sak     = s_sak;
    if ( TEST_HITS_MAX > s_hits )
    {
        s_hit_ms[s_hits] = kode_sys_time_ms();
    }
    s_hits++;
    return KODE_OK;
}

static kode_status_t cb_t2t_read(uint8_t block, uint8_t *buf, uint16_t cap, uint16_t *got, int calls)
{
    const uint32_t at = (uint32_t)block * TEST_T2T_PAGE;

    (void)calls;
    (void)cap;
    if ( (at + TEST_T2T_READ) > TEST_T2T_BYTES )
    {
        return KODE_ERROR;
    }
    (void)memcpy(buf, &s_mem[at], TEST_T2T_READ);
    *got = TEST_T2T_READ;
    return KODE_OK;
}

static kode_status_t cb_t2t_write(uint8_t block, const uint8_t *data, int calls)
{
    const uint32_t at = (uint32_t)block * TEST_T2T_PAGE;

    (void)calls;
    if ( (at + TEST_T2T_PAGE) > TEST_T2T_BYTES )
    {
        return KODE_ERROR;
    }
    (void)memcpy(&s_mem[at], data, TEST_T2T_PAGE);
    return KODE_OK;
}

static kode_status_t cb_emu_start(const knfc_emu_card_t *card, int calls)
{
    (void)calls;
    s_card       = *card;
    s_emu_active = true;
    s_emu_starts++;
    return KODE_OK;
}

static kode_status_t cb_emu_stop(int calls)
{
    (void)calls;
    s_emu_stops++;
    s_emu_stopping = true;
    return KODE_OK;
}

static void cb_emu_snapshot(knfc_emu_stat_t *out, int calls)
{
    (void)calls;
    (void)memset(out, 0, sizeof(*out));
    out->active = s_emu_active;
}

/** @brief The reader lets go of the card, if it is told to and not held. */
static void emu_release_step(void)
{
    if ( s_emu_stopping && !s_emu_hold )
    {
        s_emu_active   = false;
        s_emu_stopping = false;
    }
}

/**
 * @brief AO_Nfc answers what waits for it: the request is dispatched and the clock is turned until it is answered.
 */
static void world_serve(void)
{
    const uint32_t before = (uint32_t)fake_sem_signals();
    uint32_t       guard  = 0U;

    (void)qf_support_pump(AO_Nfc, TEST_PUMP_MAX);
    while ( ((uint32_t)fake_sem_signals() == before) && (TEST_TICKS_MAX > guard) )
    {
        (void)qf_support_tick(AO_Nfc, 1U);
        fake_time_advance_ms(10U);
        guard++;
    }
}

/* Stand-ins only record: an assertion there would skip the extended layer's claim release. */
/** @brief Whatever waits on AO_Nfc is answered, from inside the wait; the hook is armed again before anything is done. */
static void world_hook(void)
{
    fake_sem_wait_hook(&world_hook);
    if ( (xTaskGetCurrentTaskHandle() != s_case_task) && s_park_at_end && (s_windows >= s_script_count) )
    {
        s_park_at_end = false;
        (void)xSemaphoreGive(s_world_in);
        (void)xSemaphoreTake(s_world_go, pdMS_TO_TICKS(TEST_WAIT_MS));
    }
    world_serve();
}

/** @brief The clock hook: count the delay, let a parked window go, let the reader release a card; armed again first, so a task that sleeps in it leaves it for the others. */
static void world_delay(void)
{
    fake_time_delay_hook(&world_delay);
    s_delays++;
    if ( xTaskGetCurrentTaskHandle() == s_case_task )
    {
        if ( s_release_next_delay )
        {
            s_release_next_delay = false;
            (void)xSemaphoreGive(s_world_go);
        }
        if ( 0U < s_claim_tries )
        {
            s_claim_tries--;
            if ( 0U == s_claim_tries )
            {
                kode_sys_nfc_release();
            }
        }
        if ( s_tick_on_delay )
        {
            emu_release_step();
            (void)qf_support_tick(AO_Nfc, 1U);
        }
        if ( s_yield_case )
        {
            vTaskDelay(1U);
        }
    }
    else if ( s_yield_others )
    {
        vTaskDelay(1U);
    }
    else
    {
        /* another task's pause: nothing to do */
    }
}

/**
 * @brief Bring AO_Nfc up with a reader that answers: the first time in the binary, or again.
 *
 * Whether a reader is fitted is decided once, in the initial transition, so a
 * case file that finds the AO brought up by another runs the transition again.
 */
static void nfc_ao_up(void)
{
    const QEvt *stray;

    Mockkdrv_st25r3916_Init();
    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();
    kdrv_st25r3916_init_IgnoreAndReturn(KODE_OK);
    kdrv_st25r3916_identity_IgnoreAndReturn(KODE_OK);
    if ( qf_support_claim_ao(AO_Nfc) )
    {
        Nfc_ctor();
        qf_support_give_queue(AO_Nfc);
        AO_Nfc->prio  = KODE_PRIO_NFC;
        AO_Nfc->pthre = KODE_PRIO_NFC;
        QActive_register_(AO_Nfc);
        QASM_INIT(AO_Nfc, (void *)0, 0U);
    }
    else
    {
        (void)qf_support_pump(AO_Nfc, 0U);
        while ( (NULL != AO_Nfc->eQueue) && (xQueueReceive(AO_Nfc->eQueue, &stray, 0U) == pdTRUE) )
        {
            QF_gc(stray);
        }
        Nfc_ctor();
        QASM_INIT(AO_Nfc, (void *)0, 0U);
    }
    while ( (NULL != AO_Nfc->eQueue) && (xQueueReceive(AO_Nfc->eQueue, &stray, 0U) == pdTRUE) )
    {
        QF_gc(stray);
    }
}

/** @brief A clean world: AO_Nfc and a reader with a Type-2 tag on it, real locks, hooks armed. */
static void world_fresh(void)
{
    /* A claim a failed case left behind is given back. */
    kode_sys_nfc_release();
    fake_mutex_real(true);
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    /* The sys init is skipped, so its answer semaphore is made here: AO_Nfc signals none that is NULL. */
    kode_sys_nfc_sem = khal_sem_create();
    nfc_ao_up();
    fake_sem_reset();
    fake_time_reset();

    kdrv_st25r3916_discover_start_StubWithCallback(&cb_discover);
    kdrv_st25r3916_worker_Ignore();
    kdrv_st25r3916_activated_StubWithCallback(&cb_activated);
    kdrv_st25r3916_active_tag_StubWithCallback(&cb_active_tag);
    kdrv_st25r3916_field_off_Ignore();
    kdrv_st25r3916_t2t_read_StubWithCallback(&cb_t2t_read);
    kdrv_st25r3916_t2t_write_StubWithCallback(&cb_t2t_write);
    kdrv_st25r3916_emulate_start_StubWithCallback(&cb_emu_start);
    kdrv_st25r3916_emulate_stop_StubWithCallback(&cb_emu_stop);
    kdrv_st25r3916_emulate_snapshot_StubWithCallback(&cb_emu_snapshot);
    kdrv_st25r3916_transceive_bits_IgnoreAndReturn(KODE_ERROR);
    kdrv_st25r3916_reselect_IgnoreAndReturn(KODE_OK);

    (void)memset(s_uid, 0, sizeof(s_uid));
    s_uid[0]  = 0x04U;
    s_uid[1]  = 0xA2U;
    s_uid[2]  = 0x3BU;
    s_uid[3]  = 0x91U;
    s_uid_len = 4U;
    s_atqa    = TEST_ATQA;
    s_sak     = TEST_SAK;
    mem_blank();
    s_tag_here      = false;
    s_tag_default   = true;
    s_script_count  = 0U;
    s_windows       = 0U;
    s_hits          = 0U;
    s_emu_starts    = 0U;
    s_emu_stops     = 0U;
    s_emu_active    = false;
    s_emu_stopping  = false;
    s_emu_hold      = false;
    s_delays        = 0U;
    (void)memset(&s_card, 0, sizeof(s_card));
    (void)memset(s_hit_ms, 0, sizeof(s_hit_ms));

    if ( NULL == s_world_in )
    {
        s_world_in = xSemaphoreCreateBinary();
        s_world_go = xSemaphoreCreateBinary();
    }
    while ( xSemaphoreTake(s_world_in, 0U) == pdTRUE )
    {
    }
    while ( xSemaphoreTake(s_world_go, 0U) == pdTRUE )
    {
    }
    s_case_task          = xTaskGetCurrentTaskHandle();
    s_park_at_end        = false;
    s_release_next_delay = false;
    s_yield_case         = false;
    s_yield_others       = true;
    s_tick_on_delay      = false;
    s_claim_tries        = 0U;
    ao_spy_subscribe(NFC_TAG_SIG);
    fake_sem_wait_hook(&world_hook);
    fake_time_delay_hook(&world_delay);
}

/** @brief The case is over: a background reading ends, a card is let go, nothing is hooked and the AO is idle. */
static void world_end(void)
{
    s_release_next_delay = true;
    s_yield_case         = true;
    s_tick_on_delay      = true;
    s_emu_hold           = false;
    fake_sem_wait_hook(&world_hook);
    fake_time_delay_hook(&world_delay);
    (void)kode_nfc_scan_stop();
    (void)kode_nfc_emulate_stop();
    fake_sem_wait_hook(NULL);
    fake_time_delay_hook(NULL);
    (void)qf_support_tick(AO_Nfc, 1U);
    QActive_unsubscribe(ao_spy(), NFC_TAG_SIG);
    ao_spy_drain();
}

/** @brief Add a profile or fail the case. */
static void profile_add(const char *name, const char *type, const char *data)
{
    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_add_profile(name, type, data), kode_error());
}

/* ---- Constants ---------------------------------------------------------------- */

TEST_CASE("the NFC defines agree with the layers below", "[kode_nfc_standard]")
{
    TEST_ASSERT_EQUAL_UINT32(KODE_SYS_NFC_TEXT_MAX + 1U, KODE_NFC_TEXT_MAX);
    TEST_ASSERT_EQUAL_UINT32(KNFC_EMU_NDEF_MAX + 1U, KODE_NFC_TEXT_MAX);
    TEST_ASSERT_EQUAL_UINT32(KODE_SYS_NFC_TEXT_MAX - 1U, KODE_NFC_TAG_DATA_MAX);
    TEST_ASSERT_EQUAL_UINT32(KODE_NFC_TEXT_MAX, (uint32_t)sizeof(((struct kode_tag_state *)0)->nfc_text));
    TEST_ASSERT_TRUE(KODE_TAG_ID_MAX >= (3U * KNFC_UID_MAX));
}

/* ---- kode_nfc_read_id -------------------------------------------------------- */

TEST_CASE("a tag read gives the ID as hex and the model's name", "[kode_nfc_standard]")
{
    world_fresh();
    const kode_tag_t t = kode_nfc_read_id(1000U);

    TEST_ASSERT_TRUE_MESSAGE(t.ok, kode_error());
    TEST_ASSERT_EQUAL_STRING(TEST_ID, t.id);
    TEST_ASSERT_EQUAL_STRING(knfc_tag_model(TEST_ATQA, TEST_SAK), t.name);
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_windows);
    world_end();
}

TEST_CASE("a tag read gives a longer ID in full", "[kode_nfc_standard]")
{
    world_fresh();
    s_uid_len = 7U;
    for ( uint8_t i = 0U; 7U > i; i++ )
    {
        s_uid[i] = (uint8_t)(0xA0U + i);
    }
    const kode_tag_t t = kode_nfc_read_id(1000U);

    TEST_ASSERT_TRUE_MESSAGE(t.ok, kode_error());
    TEST_ASSERT_EQUAL_STRING("A0:A1:A2:A3:A4:A5:A6", t.id);
    world_end();
}

TEST_CASE("a tag read with nothing on the antenna says so after the time asked, window by window", "[kode_nfc_standard]")
{
    world_fresh();
    s_tag_default = false;

    const uint32_t   before = kode_sys_time_ms();
    const kode_tag_t t      = kode_nfc_read_id(1200U);
    const uint32_t   spent  = kode_sys_time_ms() - before;

    TEST_ASSERT_FALSE(t.ok);
    TEST_ASSERT_EQUAL_STRING("", t.id);
    TEST_ASSERT_EQUAL_STRING("", t.name);
    TEST_ASSERT_EQUAL_STRING("no NFC tag in 1200 ms", kode_error());
    TEST_ASSERT_EQUAL_UINT32(3U, s_windows);
    TEST_ASSERT_TRUE(1200U <= spent);
    TEST_ASSERT_TRUE((1200U + (3U * 100U)) >= spent);
    world_end();
}

TEST_CASE("a tag read with no time looks for nothing", "[kode_nfc_standard]")
{
    world_fresh();
    const kode_tag_t t = kode_nfc_read_id(0U);

    TEST_ASSERT_FALSE(t.ok);
    TEST_ASSERT_EQUAL_STRING("no NFC tag in 0 ms", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_windows);
    world_end();
}

TEST_CASE("a tag read waits for a tag that comes in a later window", "[kode_nfc_standard]")
{
    world_fresh();
    s_script[0]    = false;
    s_script[1]    = false;
    s_script[2]    = true;
    s_script_count = 3U;

    const kode_tag_t t = kode_nfc_read_id(KODE_FOREVER);

    TEST_ASSERT_TRUE_MESSAGE(t.ok, kode_error());
    TEST_ASSERT_EQUAL_UINT32(3U, s_windows);
    world_end();
}

TEST_CASE("a tag read behind a holder of the reader gives up within its time, calling it busy", "[kode_nfc_standard]")
{
    world_fresh();
    /* Whoever holds the reader holds it for good: the claim is taken and never given back. */
    uint32_t gen = 0U;

    TEST_ASSERT_TRUE(kode_sys_nfc_claim(&gen));

    const uint32_t   before = kode_sys_time_ms();
    const kode_tag_t t      = kode_nfc_read_id(300U);
    const uint32_t   spent  = kode_sys_time_ms() - before;

    TEST_ASSERT_FALSE(t.ok);
    TEST_ASSERT_EQUAL_STRING(TEST_BUSY, kode_error());
    TEST_ASSERT_TRUE_MESSAGE(300U >= spent, "the read kept trying after its time");

    const uint32_t   before_long = kode_sys_time_ms();
    const kode_tag_t forever     = kode_nfc_read_id(KODE_FOREVER);
    const uint32_t   spent_long  = kode_sys_time_ms() - before_long;

    TEST_ASSERT_FALSE(forever.ok);
    TEST_ASSERT_EQUAL_STRING(TEST_BUSY, kode_error());
    TEST_ASSERT_TRUE(KODE_NFC_BUSY_MS >= spent_long);
    TEST_ASSERT_TRUE((KODE_NFC_BUSY_MS - KODE_RETRY_STEP_MS) <= spent_long);
    TEST_ASSERT_EQUAL_UINT32(0U, s_windows);

    kode_sys_nfc_release();
    world_end();
}

TEST_CASE("a tag read that waits for a holder still ends within its time", "[kode_nfc_standard]")
{
    uint32_t gen = 0U;

    world_fresh();
    s_tag_default = false;
    /* The holder gives the reader back after 700 ms: a full window would end at 1200 ms. */
    TEST_ASSERT_TRUE(kode_sys_nfc_claim(&gen));
    s_claim_tries = 70U;

    const uint32_t   before = kode_sys_time_ms();
    const kode_tag_t t      = kode_nfc_read_id(1000U);
    const uint32_t   spent  = kode_sys_time_ms() - before;

    TEST_ASSERT_FALSE(t.ok);
    TEST_ASSERT_EQUAL_STRING("no NFC tag in 1000 ms", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_windows);
    TEST_ASSERT_TRUE_MESSAGE(1000U >= spent, "the read overstayed its time");
    world_end();
}

TEST_CASE("a read that spent its time waiting for a holder says busy, never that the tag is unreadable", "[kode_nfc_standard]")
{
    uint32_t gen = 0U;

    world_fresh();
    /* The holder gives the reader back on the very try that has no time left. */
    TEST_ASSERT_TRUE(kode_sys_nfc_claim(&gen));
    s_claim_tries = 10U;

    TEST_ASSERT_EQUAL_STRING("", kode_nfc_read("text", 100U));
    TEST_ASSERT_EQUAL_STRING(TEST_BUSY, kode_error());
    TEST_ASSERT_FALSE(kode_sys_nfc_is_emulating());

    TEST_ASSERT_TRUE(kode_sys_nfc_claim(&gen));
    s_claim_tries = 10U;
    TEST_ASSERT_FALSE(kode_nfc_write("text", "hi", 100U));
    TEST_ASSERT_EQUAL_STRING(TEST_BUSY, kode_error());
    world_end();
}

/* ---- kode_nfc_read ------------------------------------------------------------ */

TEST_CASE("reading a link gives the link, and reading it as text is refused", "[kode_nfc_standard]")
{
    world_fresh();
    mem_area(s_area_uri, sizeof(s_area_uri));

    const char *link = kode_nfc_read("uri", 1000U);

    TEST_ASSERT_EQUAL_STRING_MESSAGE("https://a.io", link, kode_error());
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    const char *text = kode_nfc_read("text", 1000U);

    TEST_ASSERT_EQUAL_STRING("", text);
    TEST_ASSERT_EQUAL_STRING("the tag holds a link, not text", kode_error());
    world_end();
}

TEST_CASE("reading text gives the text, and reading it as a link is refused", "[kode_nfc_standard]")
{
    world_fresh();
    mem_area(s_area_text, sizeof(s_area_text));

    TEST_ASSERT_EQUAL_STRING("hi", kode_nfc_read("text", 1000U));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_STRING("", kode_nfc_read("uri", 1000U));
    TEST_ASSERT_EQUAL_STRING("the tag holds text, not a link", kode_error());
    world_end();
}

TEST_CASE("the type is read in any case, and the text is the task's until its next read", "[kode_nfc_standard]")
{
    world_fresh();
    mem_area(s_area_text, sizeof(s_area_text));

    const char *first = kode_nfc_read("TEXT", 1000U);

    TEST_ASSERT_EQUAL_STRING("hi", first);

    mem_area(s_area_uri, sizeof(s_area_uri));

    const char *second = kode_nfc_read("Uri", 1000U);

    TEST_ASSERT_EQUAL_STRING("https://a.io", second);
    TEST_ASSERT_TRUE(first == second);
    world_end();
}

TEST_CASE("an unknown or missing type is refused before the reader is asked", "[kode_nfc_standard]")
{
    world_fresh();

    TEST_ASSERT_EQUAL_STRING("", kode_nfc_read("Bogus", 1000U));
    TEST_ASSERT_EQUAL_STRING("unknown NFC type 'Bogus'", kode_error());
    TEST_ASSERT_EQUAL_STRING("", kode_nfc_read("uid", 1000U));
    TEST_ASSERT_EQUAL_STRING("unknown NFC type 'uid'", kode_error());
    TEST_ASSERT_EQUAL_STRING("", kode_nfc_read(NULL, 1000U));
    TEST_ASSERT_EQUAL_STRING("no NFC type given", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_windows);
    world_end();
}

TEST_CASE("a read with nothing on the antenna gives an empty text and says why", "[kode_nfc_standard]")
{
    world_fresh();
    s_tag_default = false;

    TEST_ASSERT_EQUAL_STRING("", kode_nfc_read("text", 500U));
    TEST_ASSERT_EQUAL_STRING("no NFC tag in 500 ms", kode_error());
    TEST_ASSERT_EQUAL_STRING("", kode_nfc_read("text", 0U));
    TEST_ASSERT_EQUAL_STRING("no NFC tag in 0 ms", kode_error());
    world_end();
}

TEST_CASE("a tag with no message gives an empty text and says so", "[kode_nfc_standard]")
{
    world_fresh();

    TEST_ASSERT_EQUAL_STRING("", kode_nfc_read("text", 1000U));
    TEST_ASSERT_EQUAL_STRING("the tag holds no text or link", kode_error());
    world_end();
}

TEST_CASE("a Mifare Classic card is named as one that cannot be read", "[kode_nfc_standard]")
{
    world_fresh();
    s_atqa = 0x0004U;
    s_sak  = 0x08U;

    TEST_ASSERT_EQUAL_STRING("", kode_nfc_read("text", 1000U));
    TEST_ASSERT_EQUAL_STRING("this card's data cannot be read (Mifare Classic)", kode_error());
    world_end();
}

/** @brief A Text record's TLV length over its message: record head, status and language. */
#define TEST_TEXT_TLV_EXTRA      7U
/** @brief A Text record's payload over its message: status and language. */
#define TEST_TEXT_PAYLOAD_EXTRA  3U

/**
 * @brief Put a Text record of "en" and @p len letters 'a' on the tag.
 * @param len The message's length, at most KODE_NFC_TAG_DATA_MAX + 1.
 */
static void long_text_on_tag(uint32_t len)
{
    uint8_t  area[KODE_NFC_TEXT_MAX + 16U];
    uint32_t n = 0U;

    /* TLV, then a short record: its payload is the status byte, "en", the letters. */
    area[n++] = 0x03U;
    area[n++] = (uint8_t)(len + TEST_TEXT_TLV_EXTRA);
    area[n++] = 0xD1U;
    area[n++] = 0x01U;
    area[n++] = (uint8_t)(len + TEST_TEXT_PAYLOAD_EXTRA);
    area[n++] = 'T';
    area[n++] = 0x02U;
    area[n++] = 'e';
    area[n++] = 'n';
    for ( uint32_t i = 0U; i < len; i++ )
    {
        area[n++] = 'a';
    }
    area[n++] = 0xFEU;
    mem_area(area, n);
}

TEST_CASE("a message of the longest size is read whole", "[kode_nfc_standard]")
{
    world_fresh();
    long_text_on_tag(KODE_NFC_TAG_DATA_MAX);

    const char *text = kode_nfc_read("text", 1000U);

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(KODE_NFC_TAG_DATA_MAX, (uint32_t)strlen(text), kode_error());
    world_end();
}

TEST_CASE("a message one longer than the longest is a failed read, never a cut one", "[kode_nfc_standard]")
{
    world_fresh();
    long_text_on_tag(KODE_NFC_TAG_DATA_MAX + 1U);

    const char *text = kode_nfc_read("text", 1000U);

    TEST_ASSERT_EQUAL_STRING("", text);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, (int32_t)strlen(kode_error()), "the failure is said");
    world_end();
}

/* ---- kode_nfc_write ----------------------------------------------------------- */

TEST_CASE("writing text puts a Text record on the tag", "[kode_nfc_standard]")
{
    world_fresh();

    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_write("text", "hi", 1000U), kode_error());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_area_text, &s_mem[TEST_T2T_AREA], sizeof(s_area_text));
    TEST_ASSERT_EQUAL_STRING("hi", kode_nfc_read("text", 1000U));
    world_end();
}

TEST_CASE("writing a link puts a URI record on the tag", "[kode_nfc_standard]")
{
    world_fresh();

    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_write("URI", "https://a.io", 1000U), kode_error());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_area_uri, &s_mem[TEST_T2T_AREA], sizeof(s_area_uri));
    TEST_ASSERT_EQUAL_STRING("https://a.io", kode_nfc_read("uri", 1000U));
    world_end();
}

TEST_CASE("data one longer than a tag takes is refused and the tag is untouched", "[kode_nfc_standard]")
{
    char data[TEST_TEXT_BYTES];

    world_fresh();
    (void)memset(data, 'x', KODE_NFC_TAG_DATA_MAX + 1U);
    data[KODE_NFC_TAG_DATA_MAX + 1U] = '\0';

    TEST_ASSERT_FALSE(kode_nfc_write("text", data, 1000U));
    TEST_ASSERT_EQUAL_STRING("96 bytes is too long (95 at most)", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_windows);
    TEST_ASSERT_FALSE(kode_nfc_write("uri", data, 1000U));
    TEST_ASSERT_EQUAL_STRING("96 bytes is too long (95 at most)", kode_error());
    world_end();
}

TEST_CASE("data of the longest a tag takes is written whole and read back whole", "[kode_nfc_standard]")
{
    char data[TEST_TEXT_BYTES];

    world_fresh();
    (void)memset(data, 'y', KODE_NFC_TAG_DATA_MAX);
    data[KODE_NFC_TAG_DATA_MAX] = '\0';

    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_write("text", data, 1000U), kode_error());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(data, kode_nfc_read("text", 1000U), kode_error());
    world_end();
}

TEST_CASE("a write with a wrong type or no data is refused before the reader is asked", "[kode_nfc_standard]")
{
    world_fresh();

    TEST_ASSERT_FALSE(kode_nfc_write("vcard", "x", 1000U));
    TEST_ASSERT_EQUAL_STRING("unknown NFC type 'vcard'", kode_error());
    TEST_ASSERT_FALSE(kode_nfc_write("uid", "04:A2:3B:91", 1000U));
    TEST_ASSERT_EQUAL_STRING("unknown NFC type 'uid'", kode_error());
    TEST_ASSERT_FALSE(kode_nfc_write(NULL, "x", 1000U));
    TEST_ASSERT_EQUAL_STRING("no NFC type given", kode_error());
    TEST_ASSERT_FALSE(kode_nfc_write("text", NULL, 1000U));
    TEST_ASSERT_EQUAL_STRING("no data given", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_windows);
    world_end();
}

TEST_CASE("a write with nothing on the antenna says so after the time asked", "[kode_nfc_standard]")
{
    world_fresh();
    s_tag_default = false;

    TEST_ASSERT_FALSE(kode_nfc_write("text", "hi", 700U));
    TEST_ASSERT_EQUAL_STRING("no NFC tag in 700 ms", kode_error());
    TEST_ASSERT_EQUAL_UINT32(2U, s_windows);
    world_end();
}

TEST_CASE("a read-only tag is named as one that cannot be written", "[kode_nfc_standard]")
{
    world_fresh();
    s_mem[15] = 0x0FU;

    TEST_ASSERT_FALSE(kode_nfc_write("text", "hi", 1000U));
    TEST_ASSERT_EQUAL_STRING("this card cannot be written (Ultralight / NTAG (Type-2))", kode_error());
    world_end();
}

TEST_CASE("a tag too small for the data is left untouched and says so", "[kode_nfc_standard]")
{
    world_fresh();
    s_mem[14] = 0x01U;

    TEST_ASSERT_FALSE(kode_nfc_write("text", "a long enough text to overflow eight bytes", 1000U));
    TEST_ASSERT_EQUAL_STRING("the data does not fit on the tag", kode_error());
    TEST_ASSERT_EQUAL_UINT8(0U, s_mem[TEST_T2T_AREA]);
    world_end();
}

/* ---- Profiles and emulation --------------------------------------------------- */

TEST_CASE("a UID profile presents a coherent clone: ATQA and SAK follow the length", "[kode_nfc_standard]")
{
    world_fresh();
    profile_add("door", "uid", "04:A2:3B:91");
    profile_add("tag7", "uid", "04-a2-3b-91-5c-6d-7e");
    profile_add("tag10", "uid", "04A23B915C6D7E8F9011");

    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_emulate_start("door"), kode_error());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_TRUE(kode_sys_nfc_is_emulating());
    TEST_ASSERT_EQUAL_UINT8(4U, s_card.uid_len);
    TEST_ASSERT_EQUAL_HEX8(0x04U, s_card.uid[0]);
    TEST_ASSERT_EQUAL_HEX8(0x91U, s_card.uid[3]);
    TEST_ASSERT_EQUAL_HEX16(0x0004U, s_card.atqa);
    TEST_ASSERT_EQUAL_HEX8(0x08U, s_card.sak);
    TEST_ASSERT_EQUAL_STRING("", s_card.ndef);
    TEST_ASSERT_EQUAL_UINT8(0U, s_card.raw_len);

    s_tick_on_delay = true;
    s_yield_case    = true;
    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_emulate_start("tag7"), kode_error());
    TEST_ASSERT_EQUAL_UINT8(7U, s_card.uid_len);
    TEST_ASSERT_EQUAL_HEX8(0x7EU, s_card.uid[6]);
    TEST_ASSERT_EQUAL_HEX16(0x0044U, s_card.atqa);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_card.sak);

    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_emulate_start("tag10"), kode_error());
    TEST_ASSERT_EQUAL_UINT8(10U, s_card.uid_len);
    TEST_ASSERT_EQUAL_HEX8(0x11U, s_card.uid[9]);
    TEST_ASSERT_EQUAL_HEX16(0x0084U, s_card.atqa);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_card.sak);
    world_end();
}

TEST_CASE("a link profile presents a Type-2 tag that carries the link", "[kode_nfc_standard]")
{
    world_fresh();
    profile_add("page", "uri", "https://kode.diy");

    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_emulate_start("page"), kode_error());
    TEST_ASSERT_EQUAL_UINT8(0U, s_card.uid_len);
    TEST_ASSERT_TRUE(s_card.uri);
    TEST_ASSERT_EQUAL_STRING("https://kode.diy", s_card.ndef);
    TEST_ASSERT_EQUAL_UINT8(0U, s_card.raw_len);
    world_end();
}

TEST_CASE("a text profile presents a Type-2 tag that carries the text", "[kode_nfc_standard]")
{
    world_fresh();
    profile_add("note", "TEXT", "hello");

    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_emulate_start("note"), kode_error());
    TEST_ASSERT_EQUAL_UINT8(0U, s_card.uid_len);
    TEST_ASSERT_FALSE(s_card.uri);
    TEST_ASSERT_EQUAL_STRING("hello", s_card.ndef);
    world_end();
}

TEST_CASE("an unknown profile is refused and nothing is presented", "[kode_nfc_standard]")
{
    world_fresh();

    TEST_ASSERT_FALSE(kode_nfc_emulate_start("nothing"));
    TEST_ASSERT_EQUAL_STRING("no emulation profile 'nothing'; add one with kode_nfc_add_profile()",
                             kode_error());
    TEST_ASSERT_FALSE(kode_nfc_emulate_start(NULL));
    TEST_ASSERT_EQUAL_STRING("no emulation profile name given", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_emu_starts);
    TEST_ASSERT_FALSE(kode_sys_nfc_is_emulating());
    world_end();
}

TEST_CASE("a second emulation replaces the first card", "[kode_nfc_standard]")
{
    world_fresh();
    profile_add("door", "uid", "04:A2:3B:91");
    profile_add("page", "uri", "https://kode.diy");

    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_emulate_start("door"), kode_error());
    s_tick_on_delay = true;
    s_yield_case    = true;
    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_emulate_start("page"), kode_error());
    TEST_ASSERT_EQUAL_UINT32(2U, s_emu_starts);
    TEST_ASSERT_EQUAL_UINT32(1U, s_emu_stops);
    TEST_ASSERT_EQUAL_UINT8(0U, s_card.uid_len);
    TEST_ASSERT_EQUAL_STRING("https://kode.diy", s_card.ndef);
    TEST_ASSERT_TRUE(kode_sys_nfc_is_emulating());
    world_end();
}

TEST_CASE("reads and writes while emulating answer false at once, naming the way out", "[kode_nfc_standard]")
{
    world_fresh();
    profile_add("door", "uid", "04:A2:3B:91");
    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_emulate_start("door"), kode_error());
    s_windows = 0U;

    const kode_tag_t t = kode_nfc_read_id(1000U);

    TEST_ASSERT_FALSE(t.ok);
    TEST_ASSERT_EQUAL_STRING(TEST_EMULATING, kode_error());
    TEST_ASSERT_EQUAL_STRING("", kode_nfc_read("text", 1000U));
    TEST_ASSERT_EQUAL_STRING(TEST_EMULATING, kode_error());
    TEST_ASSERT_FALSE(kode_nfc_write("text", "hi", 1000U));
    TEST_ASSERT_EQUAL_STRING(TEST_EMULATING, kode_error());
    TEST_ASSERT_FALSE(kode_nfc_scan_start());
    TEST_ASSERT_EQUAL_STRING(TEST_EMULATING, kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_windows);
    TEST_ASSERT_EQUAL_UINT32(0U, s_delays);
    world_end();
}

TEST_CASE("stopping an emulation with none running answers true", "[kode_nfc_standard]")
{
    world_fresh();

    TEST_ASSERT_TRUE(kode_nfc_emulate_stop());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_emu_stops);
    world_end();
}

TEST_CASE("stopping an emulation returns once the reader is free again", "[kode_nfc_standard]")
{
    world_fresh();
    profile_add("door", "uid", "04:A2:3B:91");
    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_emulate_start("door"), kode_error());
    s_tick_on_delay = true;
    s_yield_case    = true;

    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_emulate_stop(), kode_error());
    TEST_ASSERT_FALSE(kode_sys_nfc_is_emulating());
    TEST_ASSERT_EQUAL_UINT32(1U, s_emu_stops);

    /* The reader is free for a read. */
    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_read_id(1000U).ok, kode_error());
    world_end();
}

TEST_CASE("a reader that never lets the card go makes the stop give up within its bound", "[kode_nfc_standard]")
{
    world_fresh();
    profile_add("door", "uid", "04:A2:3B:91");
    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_emulate_start("door"), kode_error());
    s_tick_on_delay = true;
    s_emu_hold      = true;

    const uint32_t before = kode_sys_time_ms();

    TEST_ASSERT_FALSE(kode_nfc_emulate_stop());

    const uint32_t spent = kode_sys_time_ms() - before;

    TEST_ASSERT_EQUAL_STRING("the NFC reader is still letting the card go; call kode_nfc_emulate_stop() again",
                             kode_error());
    TEST_ASSERT_TRUE(KODE_NFC_BUSY_MS >= spent);
    TEST_ASSERT_TRUE((KODE_NFC_BUSY_MS - KODE_RETRY_STEP_MS) <= spent);
    TEST_ASSERT_TRUE(kode_sys_nfc_is_emulating());

    /* It lets go: the same stop finishes. */
    s_emu_hold = false;
    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_emulate_stop(), kode_error());
    TEST_ASSERT_FALSE(kode_sys_nfc_is_emulating());
    world_end();
}

/* ---- Profiles ----------------------------------------------------------------- */

TEST_CASE("a profile with a bad name, type or data is refused", "[kode_nfc_standard]")
{
    char data[TEST_TEXT_BYTES];

    world_fresh();
    (void)memset(data, 'z', 97U);
    data[97] = '\0';

    TEST_ASSERT_FALSE(kode_nfc_add_profile(NULL, "uid", "04:A2:3B:91"));
    TEST_ASSERT_EQUAL_STRING("no profile name given", kode_error());
    TEST_ASSERT_FALSE(kode_nfc_add_profile("", "uid", "04:A2:3B:91"));
    TEST_ASSERT_EQUAL_STRING("no profile name given", kode_error());
    TEST_ASSERT_FALSE(kode_nfc_add_profile("0123456789abcdef", "uid", "04:A2:3B:91"));
    TEST_ASSERT_EQUAL_STRING("profile name '0123456789abcdef' is too long (15 bytes at most)", kode_error());
    TEST_ASSERT_FALSE(kode_nfc_add_profile("x", "vcard", "x"));
    TEST_ASSERT_EQUAL_STRING("unknown NFC type 'vcard'", kode_error());
    TEST_ASSERT_FALSE(kode_nfc_add_profile("x", NULL, "x"));
    TEST_ASSERT_EQUAL_STRING("no NFC type given", kode_error());
    TEST_ASSERT_FALSE(kode_nfc_add_profile("x", "text", NULL));
    TEST_ASSERT_EQUAL_STRING("no data given", kode_error());
    TEST_ASSERT_FALSE(kode_nfc_add_profile("x", "uri", data));
    TEST_ASSERT_EQUAL_STRING("97 bytes is too long (96 at most)", kode_error());
    TEST_ASSERT_FALSE(kode_nfc_add_profile("x", "text", data));
    TEST_ASSERT_EQUAL_STRING("97 bytes is too long (96 at most)", kode_error());
    TEST_ASSERT_FALSE(kode_nfc_add_profile("x", "uid", "04:A2:3B"));
    TEST_ASSERT_EQUAL_STRING("a UID is 4, 7 or 10 bytes of hex, such as 04:A2:3B:91", kode_error());
    TEST_ASSERT_FALSE(kode_nfc_add_profile("x", "uid", "04:A2:3B:91:5C"));
    TEST_ASSERT_EQUAL_STRING("a UID is 4, 7 or 10 bytes of hex, such as 04:A2:3B:91", kode_error());
    TEST_ASSERT_FALSE(kode_nfc_add_profile("x", "uid", "zz:A2:3B:91"));
    TEST_ASSERT_EQUAL_STRING("a UID is 4, 7 or 10 bytes of hex, such as 04:A2:3B:91", kode_error());
    TEST_ASSERT_FALSE(kode_nfc_add_profile("x", "uid", "04:A2:3B:9"));
    TEST_ASSERT_EQUAL_STRING("a UID is 4, 7 or 10 bytes of hex, such as 04:A2:3B:91", kode_error());
    /* None of them was added. */
    TEST_ASSERT_FALSE(kode_nfc_emulate_start("x"));
    world_end();
}

TEST_CASE("a profile of the same name replaces the first", "[kode_nfc_standard]")
{
    world_fresh();
    profile_add("door", "uid", "04:A2:3B:91");
    profile_add("door", "uid", "AA:BB:CC:DD");

    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_emulate_start("door"), kode_error());
    TEST_ASSERT_EQUAL_HEX8(0xAAU, s_card.uid[0]);
    TEST_ASSERT_EQUAL_HEX8(0xDDU, s_card.uid[3]);
    world_end();
}

TEST_CASE("profile names are compared without case: one name, one slot", "[kode_nfc_standard]")
{
    world_fresh();
    profile_add("Door", "uid", "04:A2:3B:91");

    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_emulate_start("door"), kode_error());
    TEST_ASSERT_EQUAL_HEX8(0x04U, s_card.uid[0]);
    s_tick_on_delay = true;
    s_yield_case    = true;
    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_emulate_start("DOOR"), kode_error());

    /* The same name in another case replaces it, in the one slot. */
    profile_add("door", "uid", "AA:BB:CC:DD");
    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_emulate_start("Door"), kode_error());
    TEST_ASSERT_EQUAL_HEX8(0xAAU, s_card.uid[0]);
    TEST_ASSERT_EQUAL_HEX8(0xDDU, s_card.uid[3]);
    world_end();
}

TEST_CASE("the table holds eight profiles and refuses a ninth, while a replacement still works", "[kode_nfc_standard]")
{
    char name[KODE_NFC_PROFILE_NAME_MAX];
    bool refused = false;

    world_fresh();
    /* Fill whatever room the earlier cases left: the table is the binary's. */
    for ( uint32_t i = 0U; ((KODE_NFC_PROFILES_MAX + 1U) > i) && !refused; i++ )
    {
        (void)snprintf(name, sizeof(name), "fill%" PRIu32, i);
        if ( !kode_nfc_add_profile(name, "uid", "04:A2:3B:91") )
        {
            refused = true;
            TEST_ASSERT_EQUAL_STRING("all 8 emulation profiles are in use", kode_error());
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(refused, "the table never filled");
    TEST_ASSERT_FALSE(kode_nfc_add_profile("one more", "uid", "04:A2:3B:91"));
    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_add_profile("fill0", "uid", "11:22:33:44"), kode_error());
    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_emulate_start("fill0"), kode_error());
    TEST_ASSERT_EQUAL_HEX8(0x11U, s_card.uid[0]);
    world_end();
}

/* ---- Reading in the background ------------------------------------------------ */

/** @brief Start the background reading with a script of windows and wait until its task is parked past the script. */
static void reading_parked(void)
{
    s_park_at_end = true;
    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_scan_start(), kode_error());
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_world_in, pdMS_TO_TICKS(TEST_WAIT_MS)));
}

/** @brief Stop the background reading of a parked task: the clock lets its window go and the others run. */
static bool reading_stopped(void)
{
    s_release_next_delay = true;
    s_yield_case         = true;
    return kode_nfc_scan_stop();
}

TEST_CASE("the background reading publishes every tag it finds, a second apart", "[kode_nfc_standard]")
{
    world_fresh();
    s_tag_default  = false;
    s_script[0]    = true;
    s_script[1]    = false;
    s_script[2]    = true;
    s_script_count = 3U;
    reading_parked();

    TEST_ASSERT_EQUAL_UINT32(2U, s_hits);
    TEST_ASSERT_EQUAL_UINT32(2U, (uint32_t)ao_spy_count());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)NFC_TAG_SIG, (uint32_t)ao_spy_next_sig());
    TEST_ASSERT_TRUE_MESSAGE((s_hit_ms[1] - s_hit_ms[0]) >= KODE_NFC_REPEAT_MS, "the reading did not rest after a tag");
    TEST_ASSERT_TRUE(s_hit_ms[1] - s_hit_ms[0] < (KODE_NFC_REPEAT_MS + (2U * KODE_NFC_POLL_MS)));
    TEST_ASSERT_TRUE_MESSAGE(reading_stopped(), kode_error());
    world_end();
}

TEST_CASE("a tag left on the antenna is heard once a second", "[kode_nfc_standard]")
{
    world_fresh();
    s_tag_default  = false;
    s_script[0]    = true;
    s_script[1]    = true;
    s_script[2]    = true;
    s_script_count = 3U;
    reading_parked();

    TEST_ASSERT_EQUAL_UINT32(3U, s_hits);
    TEST_ASSERT_TRUE((s_hit_ms[1] - s_hit_ms[0]) >= KODE_NFC_REPEAT_MS);
    TEST_ASSERT_TRUE((s_hit_ms[2] - s_hit_ms[1]) >= KODE_NFC_REPEAT_MS);
    TEST_ASSERT_TRUE_MESSAGE(reading_stopped(), kode_error());
    world_end();
}

TEST_CASE("a second start does nothing, and a stop with nothing running answers true", "[kode_nfc_standard]")
{
    world_fresh();
    TEST_ASSERT_TRUE(kode_nfc_scan_stop());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    s_tag_default = false;
    reading_parked();
    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_scan_start(), kode_error());
    TEST_ASSERT_TRUE_MESSAGE(reading_stopped(), kode_error());
    TEST_ASSERT_TRUE(kode_nfc_scan_stop());
    world_end();
}

static SemaphoreHandle_t s_reader_done;   /**< Given when the reader task's call returned.    */
static kode_tag_t        s_reader_tag;    /**< What it answered.                              */

/** @brief A task that reads a tag once. */
static void reader_task(void *arg)
{
    (void)arg;
    s_reader_tag = kode_nfc_read_id(2000U);
    (void)xSemaphoreGive(s_reader_done);
}

TEST_CASE("an explicit tag read while reading in the background waits its turn and gets the tag", "[kode_nfc_standard]")
{
    world_fresh();
    s_tag_default = false;
    reading_parked();
    s_reader_done = xSemaphoreCreateBinary();
    (void)memset(&s_reader_tag, 0, sizeof(s_reader_tag));

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&reader_task, "t_nfcrd", TEST_STACK_BYTES, TEST_TASK_PRIO, NULL));
    /* The reading holds the reader for its window: the read cannot go until the window is let go. */
    TEST_ASSERT_EQUAL_INT(pdFALSE, xSemaphoreTake(s_reader_done, pdMS_TO_TICKS(100U)));
    s_tag_default = true;
    (void)xSemaphoreGive(s_world_go);
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_reader_done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    TEST_ASSERT_TRUE(s_reader_tag.ok);
    TEST_ASSERT_EQUAL_STRING(TEST_ID, s_reader_tag.id);
    vSemaphoreDelete(s_reader_done);
    s_tag_default = false;
    TEST_ASSERT_TRUE_MESSAGE(reading_stopped(), kode_error());
    world_end();
}

TEST_CASE("an emulation while reading in the background is refused", "[kode_nfc_standard]")
{
    world_fresh();
    profile_add("door", "uid", "04:A2:3B:91");
    s_tag_default = false;
    reading_parked();

    /* Refused at once, though the reading holds the reader in its window. */
    TEST_ASSERT_FALSE(kode_nfc_emulate_start("door"));
    TEST_ASSERT_EQUAL_STRING("NFC is reading; kode_nfc_scan_stop() ends it", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_emu_starts);
    TEST_ASSERT_TRUE_MESSAGE(reading_stopped(), kode_error());
    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_emulate_start("door"), kode_error());
    world_end();
}

TEST_CASE("a stop in the middle of a window waits for it and returns within its bound", "[kode_nfc_standard]")
{
    world_fresh();
    s_tag_default = false;
    reading_parked();

    /* The window stays open: the wait ends at its bound, not before and not after. */
    const uint32_t before = kode_sys_time_ms();

    TEST_ASSERT_FALSE(kode_nfc_scan_stop());

    const uint32_t spent = kode_sys_time_ms() - before;

    TEST_ASSERT_TRUE(KODE_NFC_STOP_MS >= spent);
    TEST_ASSERT_TRUE((KODE_NFC_STOP_MS - KODE_RETRY_STEP_MS) <= spent);
    TEST_ASSERT_EQUAL_STRING("the NFC reading is still ending; call kode_nfc_scan_stop() again", kode_error());

    /* A start meanwhile is not a start. */
    TEST_ASSERT_FALSE(kode_nfc_scan_start());
    TEST_ASSERT_EQUAL_STRING("the NFC reading is still ending; call kode_nfc_scan_stop() again", kode_error());

    /* The window ends: the same stop now finishes. */
    TEST_ASSERT_TRUE_MESSAGE(reading_stopped(), kode_error());

    /* And a new reading can begin. */
    reading_parked();
    TEST_ASSERT_TRUE_MESSAGE(reading_stopped(), kode_error());
    world_end();
}

TEST_CASE("a reading that finds the reader held from outside rests and tries again", "[kode_nfc_standard]")
{
    uint32_t gen = 0U;

    world_fresh();
    s_script[0]    = true;
    s_script_count = 1U;
    TEST_ASSERT_TRUE(kode_sys_nfc_claim(&gen));
    s_park_at_end = true;
    /* The window is refused at once; no window parks, so the case lets time pass instead. */
    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_scan_start(), kode_error());
    s_yield_case = true;
    for ( uint32_t i = 0U; 20U > i; i++ )
    {
        vTaskDelay(1U);
    }
    TEST_ASSERT_EQUAL_UINT32(0U, s_windows);
    kode_sys_nfc_release();
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_world_in, pdMS_TO_TICKS(TEST_WAIT_MS)));
    TEST_ASSERT_EQUAL_UINT32(1U, s_hits);
    TEST_ASSERT_TRUE_MESSAGE(reading_stopped(), kode_error());
    world_end();
}

/* ---- Two tasks at once -------------------------------------------------------- */

static SemaphoreHandle_t s_writer_done;   /**< Given by each writer when its call returned.   */
static bool              s_writer_ok[2];  /**< What each answered.                            */

/** @brief A task that writes one text. */
static void writer_task(void *arg)
{
    const uint32_t index = (uint32_t)(uintptr_t)arg;
    const char    *text  = "first";

    if ( 0U != index )
    {
        text = "second";
    }
    s_writer_ok[index] = kode_nfc_write("text", text, 2000U);
    (void)xSemaphoreGive(s_writer_done);
}

TEST_CASE("two tasks writing at once are served one after the other", "[kode_nfc_standard]")
{
    world_fresh();
    s_writer_done = xSemaphoreCreateCounting(2U, 0U);
    s_writer_ok[0] = false;
    s_writer_ok[1] = false;
    /* The first writer parks inside its window, with the reader. */
    s_park_at_end = true;
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&writer_task, "t_nfcw1", TEST_STACK_BYTES, TEST_TASK_PRIO,
                                                    (void *)(uintptr_t)0U));
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_world_in, pdMS_TO_TICKS(TEST_WAIT_MS)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&writer_task, "t_nfcw2", TEST_STACK_BYTES, TEST_TASK_PRIO,
                                                    (void *)(uintptr_t)1U));
    /* The second cannot go while the first holds the reader. */
    TEST_ASSERT_EQUAL_INT(pdFALSE, xSemaphoreTake(s_writer_done, pdMS_TO_TICKS(100U)));
    TEST_ASSERT_EQUAL_UINT32(0U, s_windows);

    (void)xSemaphoreGive(s_world_go);
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_writer_done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_writer_done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    TEST_ASSERT_TRUE(s_writer_ok[0]);
    TEST_ASSERT_TRUE(s_writer_ok[1]);
    TEST_ASSERT_EQUAL_UINT32(2U, s_windows);
    /* Nobody waited for a reader somebody else held: no retry slept. */
    TEST_ASSERT_EQUAL_UINT32(0U, s_delays);
    TEST_ASSERT_EQUAL_STRING("second", kode_nfc_read("text", 1000U));
    vSemaphoreDelete(s_writer_done);
    world_end();
}

/* ---- Round 1 of the review ---------------------------------------------------- */

TEST_CASE("a stop behind a holder of the reader is tried again until it goes through", "[kode_nfc_standard]")
{
    uint32_t gen = 0U;

    world_fresh();
    profile_add("door", "uid", "04:A2:3B:91");
    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_emulate_start("door"), kode_error());
    /* Somebody outside the layer holds the claim for three tries of the stop. */
    TEST_ASSERT_TRUE(kode_sys_nfc_claim(&gen));
    s_claim_tries   = 3U;
    s_tick_on_delay = true;
    s_yield_case    = true;

    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_emulate_stop(), kode_error());
    TEST_ASSERT_FALSE(kode_sys_nfc_is_emulating());
    TEST_ASSERT_EQUAL_UINT32(1U, s_emu_stops);
    world_end();
}

TEST_CASE("a stop behind a holder that never lets go says the reader is busy, within its bound", "[kode_nfc_standard]")
{
    uint32_t gen = 0U;

    world_fresh();
    profile_add("door", "uid", "04:A2:3B:91");
    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_emulate_start("door"), kode_error());
    TEST_ASSERT_TRUE(kode_sys_nfc_claim(&gen));

    const uint32_t before = kode_sys_time_ms();

    TEST_ASSERT_FALSE(kode_nfc_emulate_stop());
    TEST_ASSERT_EQUAL_STRING(TEST_BUSY, kode_error());
    TEST_ASSERT_TRUE((kode_sys_time_ms() - before) <= KODE_NFC_BUSY_MS);
    TEST_ASSERT_EQUAL_UINT32(0U, s_emu_stops);
    TEST_ASSERT_TRUE(kode_sys_nfc_is_emulating());
    kode_sys_nfc_release();
    s_tick_on_delay = true;
    world_end();
}

TEST_CASE("a window answered empty with the last of the time gone is a timeout, never a window of no time", "[kode_nfc_standard]")
{
    world_fresh();
    /* Nothing answers AO_Nfc's request: every window is an early empty answer, and the pauses use the time up. */
    fake_sem_wait_hook(NULL);

    TEST_ASSERT_FALSE(kode_nfc_write("text", "hi", 25U));
    TEST_ASSERT_EQUAL_STRING("no NFC tag in 25 ms", kode_error());
    TEST_ASSERT_EQUAL_STRING("", kode_nfc_read("text", 25U));
    TEST_ASSERT_EQUAL_STRING("no NFC tag in 25 ms", kode_error());
    world_end();
}

static SemaphoreHandle_t s_twice_done;                  /**< Given when the second stop has returned.  */
static bool              s_twice_ok;                    /**< What it answered.                         */
static char              s_twice_error[KODE_ERROR_MAX]; /**< Its error afterwards.                     */

/** @brief A task that stops the reading once. */
static void twice_task(void *arg)
{
    (void)arg;
    s_twice_ok = kode_nfc_scan_stop();
    (void)strncpy(s_twice_error, kode_error(), sizeof(s_twice_error) - 1U);
    (void)xSemaphoreGive(s_twice_done);
}

/** @brief The first stop's wait: a second stop is made and answered, then the window is let go. */
static void stop_twice_hook(void)
{
    if ( xTaskGetCurrentTaskHandle() == s_case_task )
    {
        (void)khal_task_start(&twice_task, "t_nfcstop2", TEST_STACK_BYTES, TEST_TASK_PRIO, NULL);
        (void)xSemaphoreTake(s_twice_done, pdMS_TO_TICKS(TEST_WAIT_MS));
        (void)xSemaphoreGive(s_world_go);
        vTaskDelay(1U);
        fake_time_delay_hook(&world_delay);
    }
    else
    {
        fake_time_delay_hook(&stop_twice_hook);
    }
}

TEST_CASE("a second stop while one waits is refused, and the first still finishes", "[kode_nfc_standard]")
{
    world_fresh();
    s_tag_default = false;
    reading_parked();
    s_twice_done     = xSemaphoreCreateBinary();
    s_twice_ok       = true;
    s_twice_error[0] = '\0';
    s_yield_case     = true;
    fake_time_delay_hook(&stop_twice_hook);

    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_scan_stop(), kode_error());
    TEST_ASSERT_FALSE(s_twice_ok);
    TEST_ASSERT_EQUAL_STRING("the NFC reading is already being stopped", s_twice_error);
    /* A reading started after the stops is not ended by either of them. */
    s_yield_case = false;
    fake_time_delay_hook(&world_delay);
    reading_parked();
    TEST_ASSERT_TRUE(kode_nfc_scan_start());
    TEST_ASSERT_TRUE_MESSAGE(reading_stopped(), kode_error());
    vSemaphoreDelete(s_twice_done);
    world_end();
}

static SemaphoreHandle_t s_stopper_done;  /**< Given when the stopping task's call returned. */
static bool              s_stopper_ok;    /**< What it answered.                             */

/** @brief A task that stops the emulation once. */
static void stopper_task(void *arg)
{
    (void)arg;
    s_stopper_ok = kode_nfc_emulate_stop();
    (void)xSemaphoreGive(s_stopper_done);
}

TEST_CASE("reads and writes are refused at once while a stop holds the lock waiting for the card to go",
          "[kode_nfc_standard]")
{
    world_fresh();
    profile_add("door", "uid", "04:A2:3B:91");
    TEST_ASSERT_TRUE_MESSAGE(kode_nfc_emulate_start("door"), kode_error());
    s_stopper_done = xSemaphoreCreateBinary();
    s_stopper_ok   = false;
    s_emu_hold     = true;
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&stopper_task, "t_nfcstop", TEST_STACK_BYTES, TEST_TASK_PRIO, NULL));
    for ( uint32_t i = 0U; (TEST_TICKS_MAX > i) && (0U == s_emu_stops); i++ )
    {
        vTaskDelay(1U);
    }
    TEST_ASSERT_EQUAL_UINT32(1U, s_emu_stops);

    /* The stop holds the lock and the card is still presented: nothing here waits behind it. */
    TEST_ASSERT_FALSE(kode_nfc_read_id(1000U).ok);
    TEST_ASSERT_EQUAL_STRING(TEST_EMULATING, kode_error());
    TEST_ASSERT_EQUAL_STRING("", kode_nfc_read("text", 1000U));
    TEST_ASSERT_EQUAL_STRING(TEST_EMULATING, kode_error());
    TEST_ASSERT_FALSE(kode_nfc_write("text", "hi", 1000U));
    TEST_ASSERT_EQUAL_STRING(TEST_EMULATING, kode_error());
    TEST_ASSERT_EQUAL_INT(pdFALSE, xSemaphoreTake(s_stopper_done, 0U));

    s_emu_hold = false;
    emu_release_step();
    (void)qf_support_tick(AO_Nfc, 1U);
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_stopper_done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    TEST_ASSERT_TRUE(s_stopper_ok);
    vSemaphoreDelete(s_stopper_done);
    world_end();
}

TEST_CASE("a text or link profile with no data is refused", "[kode_nfc_standard]")
{
    world_fresh();

    TEST_ASSERT_FALSE(kode_nfc_add_profile("empty", "text", ""));
    TEST_ASSERT_EQUAL_STRING("a text or link profile needs some data", kode_error());
    TEST_ASSERT_FALSE(kode_nfc_add_profile("empty", "uri", ""));
    TEST_ASSERT_EQUAL_STRING("a text or link profile needs some data", kode_error());
    TEST_ASSERT_FALSE(kode_nfc_emulate_start("empty"));
    world_end();
}
