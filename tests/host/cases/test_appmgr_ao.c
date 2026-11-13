/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_appmgr_ao.c
 * @brief AO_AppMgr's deadline on a wait that used to have none.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "qpc.h"
#include "Mockkdrv_slot.h"

#include "ao_spy.h"
#include "fakes.h"
#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_partition.h"

#include "kaos.h"
#include "kapp_cat.h"
#include "kapp_desc.h"
#include "kapps_fmt.h"
#include "kdrv_apps.h"
#include "kdrv_kfw.h"
#include "kbsp_owners.h"
#include "khal_crc32.h"
#include "khal_nvs.h"
#include "khal_system.h"
#include "kqp.h"
#include "lowpower_cases.h"
#include "miniz.h"
#include "qf_support.h"

/**
 * @brief Construct and start AO_AppMgr over what the board holds, and leave what its
 *        initial transition posted in its queue.
 */
static void appmgr_make(void)
{
    static bool s_registered;

    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();

    AppMgr_ctor();
    qf_support_give_queue(AO_AppMgr);

    /* The neighbours it actually talks to. AO_Display is not one of them: a
     * launch asks it for nothing, and the queue the fakes keep for it is how
     * the launch cases prove that. AO_Storage, AO_Net and AO_Boot are real
     * AOs that live in this binary, and a post into an AO with no queue is a
     * FreeRTOS assertion rather than a test failure. AO_Log joined this list
     * once the receive protocol arrived: AppMgr_idle's APP_RECV_REQ_SIG
     * handler posts CONSOLE_RAW_REQ_SIG there to arm raw mode (appmgr_ao.c's
     * own comment marks the line), and AppMgr_receiving's idle watchdog posts
     * CONSOLE_RAW_END_SIG there on a stall - neither has anywhere to land
     * without this. */
    qf_support_give_queue(AO_Storage);
    qf_support_give_queue(AO_Net);
    qf_support_give_queue(AO_Boot);
    qf_support_give_queue(AO_Log);
    AO_AppMgr->prio  = KODE_PRIO_APPMGR;
    AO_AppMgr->pthre = KODE_PRIO_APPMGR;

    if ( !s_registered )
    {
        QActive_register_(AO_AppMgr);
        s_registered = true;
    }
    QASM_INIT(AO_AppMgr, (void *)0, 0U);
}

/** @brief Put AO_AppMgr back to idle. */
static void appmgr_fresh(void)
{
    appmgr_make();

    /* Drop whatever the initial transition announced, and whatever a previous
     * case left in the neighbour's queue - that queue is the discriminator
     * every case below reads. AO_Log's queue gets the same treatment: a
     * receive case that forgot to drain it would otherwise hand the next
     * case a stale CONSOLE_RAW_REQ_SIG or CONSOLE_RAW_END_SIG to trip over. */
    for ( ;; )
    {
        const QEvt *e = NULL;
        if ( xQueueReceive(AO_AppMgr->eQueue, &e, 0U) != pdTRUE )
        {
            break;
        }
        QF_gc(e);
    }
    for ( ;; )
    {
        const QEvt *e = NULL;
        if ( xQueueReceive(AO_Storage->eQueue, &e, 0U) != pdTRUE )
        {
            break;
        }
        QF_gc(e);
    }
    for ( ;; )
    {
        const QEvt *e = NULL;
        if ( xQueueReceive(AO_Log->eQueue, &e, 0U) != pdTRUE )
        {
            break;
        }
        QF_gc(e);
    }
}

/**
 * @brief Empty AO_Storage's queue and report how many requests were in it.
 *
 * This is the discriminator the whole file turns on. Whether AO_AppMgr
 * *accepted* a request is not visible in its log - a refusal writes a line
 * too - but an accepted one becomes an open request posted to AO_Storage, and
 * a refused one posts nothing.
 */
static unsigned storage_requests(void)
{
    const QEvt *e;
    unsigned    n = 0U;

    while ( xQueueReceive(AO_Storage->eQueue, &e, 0U) == pdTRUE )
    {
        ++n;
        QF_gc(e);
    }
    return n;
}

/**
 * @brief Ask for an update from a file, which sends the AO into opening.
 *
 * The OS slot is the default destination here because it is behind a mock, so
 * a case can simply say the slot is there. The apps region needs no such help
 * any more - this project declares the board's partition table and the region
 * is genuinely present - which is what `ask_flash_dst()` below is for.
 */
static void ask_flash_dst(const char *name, uint8_t dst)
{
    FlashReqEvt e;

    kdrv_slot_present_IgnoreAndReturn(true);
    kdrv_slot_sink_IgnoreAndReturn(NULL);

    (void)memset(&e, 0, sizeof(e));
    e.super    = (QEvt)QEVT_INITIALIZER(FLASH_REQ_SIG);
    e.src_kind = (uint8_t)KFW_SRC_FS;
    e.dst_kind = dst;
    e.launch   = false;
    (void)snprintf(e.src, sizeof(e.src), "%s", name);

    QASM_DISPATCH(AO_AppMgr, &e.super, 0U);
}

static void ask_flash(const char *name)
{
    ask_flash_dst(name, (uint8_t)KFW_TARGET_OS);
}

/** @brief The deadline AppMgr_opening arms, in QP ticks. */
#define APPMGR_OPEN_DEADLINE_TICKS 3000U

/**
 * @brief Run the clock forward, delivering whatever the AO posts itself.
 * @param ticks How many QP ticks to advance.
 */
static void run_ticks(unsigned ticks)
{
    for ( unsigned i = 0U; i < ticks; ++i )
    {
        (void)qf_support_pump(AO_AppMgr, 8U);
    }
}

/* ---- Bring-up -------------------------------------------------------------- */

TEST_CASE("AO_AppMgr claims the three things it owns", "[appmgr_ao]")
{
    Mockkdrv_slot_Init();
    appmgr_fresh();

    TEST_ASSERT_EQUAL_STRING("AO_AppMgr",
                             kbsp_owner_name(KBSP_RES_APPS_REGION));
    TEST_ASSERT_EQUAL_STRING("AO_AppMgr",
                             kbsp_owner_name(KBSP_RES_APP_PENDING));
    TEST_ASSERT_EQUAL_STRING("AO_AppMgr", kbsp_owner_name(KBSP_RES_OTA_SLOT));
}

/* ---- The wait that had no deadline ----------------------------------------- */

/* The case the audit found. Nothing answers the open, and the AO has to give
 * up on its own rather than hold the region for the rest of the boot.
 *
 * The assertion is the line it writes, not the state it is in. That is
 * deliberate: "did it leave AppMgr_opening?" has no clean observable from
 * outside - a refusal and an acceptance both write to the log, and the
 * neighbour's queue depends on how far the accepted one got. The give-up
 * message is unambiguous and it is what an operator would see. */
TEST_CASE("an open nobody answers is abandoned, not waited on forever",
          "[appmgr_ao]")
{
    bool gave_up = false;

    Mockkdrv_slot_Init();
    appmgr_fresh();

    ask_flash("never-answered.bin");

    /* It asked the medium, and is now waiting for an answer that will not
     * come. */
    TEST_ASSERT_EQUAL_UINT(1U, storage_requests());

    /* Past the deadline. Before the fix, no number here was large enough. */
    run_ticks(APPMGR_OPEN_DEADLINE_TICKS + 50U);

    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const line = fake_klog_line(i);
        if ( (NULL != line) && (strstr(line, "was never answered") != NULL) )
        {
            gave_up = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(gave_up,
        "the AO is still in AppMgr_opening, and will be until a reset");

    Mockkdrv_slot_Destroy();
}

/* Every failure that exists today is a failure of the write itself, and the
 * Opening card keeps its words for it: the reason travels in the event so the
 * one new failure - the card could not keep an app - can say something else.
 * It sits beside the deadline case, which it repeats, because it ticks: AppMgr's
 * constructor runs again over a timer that a case left armed, and a ticking
 * case after that walks a list it has broken. */
TEST_CASE("a write that fails says it was the write", "[appmgr_ao]")
{
    Mockkdrv_slot_Init();
    appmgr_fresh();
    ao_spy_subscribe(APP_FLASH_FAIL_SIG);

    ask_flash("never-answered.bin");
    (void)storage_requests();
    run_ticks(APPMGR_OPEN_DEADLINE_TICKS + 50U);

    const QEvt *e = ao_spy_take();
    while ( (NULL != e) && ((int)e->sig != APP_FLASH_FAIL_SIG) )
    {
        e = ao_spy_take();
    }
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KFAIL_WRITE,
                            ((AppFlashFailEvt const *)e)->why);

    QActive_unsubscribe(ao_spy(), APP_FLASH_FAIL_SIG);
    ao_spy_drain();
    Mockkdrv_slot_Destroy();
}

/** @brief Empty AO_Net's queue and count the events with signal @p sig in it. */
static unsigned net_requests(enum_t sig)
{
    const QEvt *e;
    unsigned    n = 0U;

    while ( xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (enum_t)e->sig == sig )
        {
            ++n;
        }
        QF_gc(e);
    }
    return n;
}

/** @brief Ask for an install of an app from a URL. */
static void ask_https(const char *url)
{
    FlashReqEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super    = (QEvt)QEVT_INITIALIZER(FLASH_REQ_SIG);
    e.src_kind = (uint8_t)KFW_SRC_HTTP;
    e.dst_kind = (uint8_t)KFW_TARGET_APP;
    e.launch   = false;
    (void)snprintf(e.src, sizeof(e.src), "%s", url);

    QASM_DISPATCH(AO_AppMgr, &e.super, 0U);
}

/* AO_Net keeps its download open after a read error, and says "a transfer is
 * already open" to the next one until Wi-Fi cycles - unless AppMgr closed it.
 * The failure's cleanup used to clear which medium it was before closing it,
 * and closed the storage reader the install never had. */
TEST_CASE("a failed https install closes the download, and the next one is "
          "asked for", "[appmgr_ao]")
{
    StorageOpenRspEvt rsp;

    appmgr_fresh();
    (void)net_requests(NET_STREAM_CLOSE_REQ_SIG);

    ask_https("https://example.invalid/app.bin");
    TEST_ASSERT_EQUAL_UINT(1U, net_requests(NET_STREAM_OPEN_REQ_SIG));
    (void)storage_requests();

    /* The server says no: the install fails and cleans up. */
    (void)memset(&rsp, 0, sizeof(rsp));
    rsp.super  = (QEvt)QEVT_INITIALIZER(NET_STREAM_OPEN_RSP_SIG);
    rsp.status = (uint8_t)KODE_ERROR;
    QASM_DISPATCH(AO_AppMgr, &rsp.super, 0U);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, net_requests(NET_STREAM_CLOSE_REQ_SIG),
        "the failed download was not closed on AO_Net");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_requests(),
        "the storage reader was closed for a download");

    ask_https("https://example.invalid/app.bin");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, net_requests(NET_STREAM_OPEN_REQ_SIG),
        "the second download was not asked for");
    (void)storage_requests();

    /* Leave nothing armed: the open's deadline runs out and is cleaned up. */
    run_ticks(APPMGR_OPEN_DEADLINE_TICKS + 50U);
    (void)net_requests(NET_STREAM_CLOSE_REQ_SIG);
    (void)storage_requests();
}

/* An `app_cat` request is the one thing answered rather than dropped while
 * busy, because a console command is waiting on the response. Everything else
 * can be told "busy" in the log; this one has somebody holding the line. */
TEST_CASE("a category request is answered even while busy", "[appmgr_ao]")
{
    AppCatReqEvt e;

    Mockkdrv_slot_Init();
    appmgr_fresh();
    ask_flash("first.bin");

    /* The answer goes on the bus rather than back to a requester, so the spy
     * has to ask for it the way a real subscriber would. */
    ao_spy_subscribe(APP_CAT_RSP_SIG);

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(APP_CAT_REQ_SIG);
    (void)snprintf(e.project, sizeof(e.project), "%s", "someapp");

    QASM_DISPATCH(AO_AppMgr, &e.super, 0U);

    const QEvt * const rsp = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(rsp, "the console would wait forever");
    TEST_ASSERT_EQUAL_INT(APP_CAT_RSP_SIG, (int)rsp->sig);

    run_ticks(4000U);
    Mockkdrv_slot_Destroy();
}

/* ---- Listing --------------------------------------------------------------- */

/* A listing always ends with its marker, whatever the region said. The GUI and
 * the console both close their listing on it, so a round that stopped without
 * one would leave the launcher showing "Scanning..." for good. */
TEST_CASE("a listing always ends with its end marker", "[appmgr_ao]")
{
    AppListReqEvt e = {
        .super = QEVT_INITIALIZER(APP_LIST_REQ_SIG), .skip = 0U,
        .who = ao_spy() };
    bool saw_end = false;

    Mockkdrv_slot_Init();
    appmgr_fresh();

    QASM_DISPATCH(AO_AppMgr, &e.super, 0U);

    for ( unsigned i = 0U; 64U > i; ++i )
    {
        const QEvt * const got = ao_spy_take();
        if ( NULL == got )
        {
            break;
        }
        if ( (int)got->sig == APP_LIST_END_SIG )
        {
            saw_end = true;
        }
    }

    /* The region is unreadable in this binary, so the listing is empty - and
     * an empty listing still has to be closed. */
    TEST_ASSERT_TRUE_MESSAGE(saw_end, "the listing never ended");
    Mockkdrv_slot_Destroy();
}


/* Every installed app of a listing carries its place in it, and the end
 * marker says whether another page follows. */
TEST_CASE("an installed listing numbers its entries and ends with no more",
          "[appmgr_ao]")
{
    AppListReqEvt req;
    kapps_dir_t   dir;

    Mockkdrv_slot_Init();
    appmgr_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_init());
    kapps_dir_t const saved = *kdrv_apps_dir();

    dir = saved;
    dir.count = 3U;
    for ( uint32_t i = 0U; 3U > i; ++i )
    {
        dir.e[i].offset = KAPPS_DATA_OFF + (i * KAPPS_ALIGN);
        dir.e[i].size   = 0x1000U;
        dir.e[i].seq    = i + 1U;
        dir.e[i].flags  = KAPPS_ENTRY_VALID;
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));

    /* Nothing subscribed: the page goes to its asker alone. */
    (void)memset(&req, 0, sizeof(req));
    req.super = (QEvt)QEVT_INITIALIZER(APP_LIST_REQ_SIG);
    req.skip  = 0U;
    req.who   = ao_spy();
    QASM_DISPATCH(AO_AppMgr, &req.super, 0U);

    for ( uint16_t i = 0U; 3U > i; ++i )
    {
        AppEntryEvt const * const en = (AppEntryEvt const *)ao_spy_take();
        TEST_ASSERT_NOT_NULL(en);
        TEST_ASSERT_EQUAL_INT(APP_ENTRY_SIG, (int)en->super.sig);
        TEST_ASSERT_EQUAL_UINT16(i, en->pos);
    }
    ListEndEvt const * const end = (ListEndEvt const *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_INT(APP_LIST_END_SIG, (int)end->super.sig);
    TEST_ASSERT_FALSE(end->more);

    kapps_dir_t restore = saved;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&restore));
    Mockkdrv_slot_Destroy();
}

/* The entry carries where its image sits in the region: the number the boot
 * request names and boot_app_offset() answers with, so a launcher can match a
 * row to the app that ran. It is the directory entry's own, copied whole; it is
 * not worked out from the entry's place in the listing, so the entries below
 * sit out of order and apart. */
TEST_CASE("a listed entry carries its offset in the region", "[appmgr_ao]")
{
    AppListReqEvt        req;
    kapps_dir_t          dir;
    static const uint32_t at[] = {
        KAPPS_DATA_OFF + (4U * KAPPS_ALIGN),
        KAPPS_DATA_OFF,
        KAPPS_DATA_OFF + (9U * KAPPS_ALIGN),
    };
    uint32_t const       n = (uint32_t)(sizeof(at) / sizeof(at[0]));

    Mockkdrv_slot_Init();
    appmgr_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_init());
    kapps_dir_t const saved = *kdrv_apps_dir();

    dir = saved;
    dir.count = n;
    for ( uint32_t i = 0U; i < n; ++i )
    {
        dir.e[i].offset = at[i];
        dir.e[i].size   = 0x1000U;
        dir.e[i].seq    = i + 1U;
        dir.e[i].flags  = KAPPS_ENTRY_VALID;
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));

    (void)memset(&req, 0, sizeof(req));
    req.super = (QEvt)QEVT_INITIALIZER(APP_LIST_REQ_SIG);
    req.skip  = 0U;
    req.who   = ao_spy();
    QASM_DISPATCH(AO_AppMgr, &req.super, 0U);

    for ( uint32_t i = 0U; i < n; ++i )
    {
        AppEntryEvt const * const en = (AppEntryEvt const *)ao_spy_take();
        TEST_ASSERT_NOT_NULL(en);
        TEST_ASSERT_EQUAL_INT(APP_ENTRY_SIG, (int)en->super.sig);
        TEST_ASSERT_EQUAL_HEX32(at[i], en->offset);
    }
    ao_spy_drain();

    kapps_dir_t restore = saved;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&restore));
    Mockkdrv_slot_Destroy();
}

/* ---- The installed apps, in pages ---------------------------------------- */

/** @brief The most pages a case below may take before it calls it a loop. */
#define APP_PAGES_MAX  6U

/** @brief Commit a directory of @p n valid entries, seq 1..n. */
static void dir_of(uint32_t n)
{
    kapps_dir_t dir = *kdrv_apps_dir();

    dir.count = n;
    for ( uint32_t i = 0U; i < n; ++i )
    {
        dir.e[i].offset = KAPPS_DATA_OFF + (i * KAPPS_ALIGN);
        dir.e[i].size   = 0x1000U;
        dir.e[i].seq    = i + 1U;
        dir.e[i].flags  = KAPPS_ENTRY_VALID;
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));
}

/** @brief What read_app_pages() saw. */
typedef struct {
    unsigned total;                  /**< Entries over every page.  */
    unsigned pages;                  /**< Pages asked.              */
    unsigned size[APP_PAGES_MAX];    /**< Entries in each page.     */
    unsigned seq_seen[KAPPS_MAX_ENTRIES + 1U]; /**< Times each seq came. */
} app_pages_t;

/**
 * @brief Read the installed apps page by page, the way an asker does: the spy
 *        asks, subscribes to nothing, and gets each page by post.
 */
static void read_app_pages(app_pages_t *p)
{
    (void)memset(p, 0, sizeof(*p));

    for ( ;; )
    {
        AppListReqEvt req;

        TEST_ASSERT_TRUE_MESSAGE(APP_PAGES_MAX > p->pages, "the pages do not end");
        ao_spy_drain();
        (void)memset(&req, 0, sizeof(req));
        req.super = (QEvt)QEVT_INITIALIZER(APP_LIST_REQ_SIG);
        req.skip  = (uint16_t)p->total;
        req.who   = ao_spy();
        QASM_DISPATCH(AO_AppMgr, &req.super, 0U);

        bool more = false;
        for ( ;; )
        {
            const QEvt * const e = ao_spy_take();
            TEST_ASSERT_NOT_NULL_MESSAGE(e, "the page never ended");
            if ( (int)e->sig == APP_LIST_END_SIG )
            {
                more = ((ListEndEvt const *)e)->more;
                break;
            }
            AppEntryEvt const * const en = (AppEntryEvt const *)e;
            TEST_ASSERT_EQUAL_UINT16((uint16_t)p->total, en->pos);
            TEST_ASSERT_TRUE(KAPPS_MAX_ENTRIES >= en->seq);
            ++p->seq_seen[en->seq];
            ++p->total;
            ++p->size[p->pages];
        }
        ++p->pages;
        if ( !more )
        {
            break;
        }
    }
    ao_spy_drain();
}

/** @brief Run one paged listing over a directory of @p n apps. */
static void app_pages_of(uint32_t n, app_pages_t *p)
{
    Mockkdrv_slot_Init();
    appmgr_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_init());
    kapps_dir_t const saved = *kdrv_apps_dir();

    dir_of(n);
    read_app_pages(p);

    kapps_dir_t restore = saved;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&restore));
    Mockkdrv_slot_Destroy();
}

TEST_CASE("an empty region is one empty page", "[appmgr_ao][pages]")
{
    static app_pages_t p;

    app_pages_of(0U, &p);
    TEST_ASSERT_EQUAL_UINT(0U, p.total);
    TEST_ASSERT_EQUAL_UINT(1U, p.pages);
}

TEST_CASE("a page of installed apps is one page", "[appmgr_ao][pages]")
{
    static app_pages_t p;

    app_pages_of(KODE_STORAGE_LIST_MAX, &p);
    TEST_ASSERT_EQUAL_UINT(KODE_STORAGE_LIST_MAX, p.total);
    TEST_ASSERT_EQUAL_UINT(1U, p.pages);
}

TEST_CASE("installed apps past a page come in pages", "[appmgr_ao][pages]")
{
    static app_pages_t p;

    app_pages_of(KODE_STORAGE_LIST_MAX + 1U, &p);
    TEST_ASSERT_EQUAL_UINT(KODE_STORAGE_LIST_MAX + 1U, p.total);
    TEST_ASSERT_EQUAL_UINT(2U, p.pages);
    TEST_ASSERT_EQUAL_UINT(KODE_STORAGE_LIST_MAX, p.size[0]);
    TEST_ASSERT_EQUAL_UINT(1U, p.size[1]);
}

TEST_CASE("a full region comes in three pages", "[appmgr_ao][pages]")
{
    static app_pages_t p;

    app_pages_of(KAPPS_MAX_ENTRIES, &p);
    TEST_ASSERT_EQUAL_UINT(KAPPS_MAX_ENTRIES, p.total);
    TEST_ASSERT_EQUAL_UINT(3U, p.pages);
    TEST_ASSERT_EQUAL_UINT(KODE_STORAGE_LIST_MAX, p.size[0]);
    TEST_ASSERT_EQUAL_UINT(KODE_STORAGE_LIST_MAX, p.size[1]);
    TEST_ASSERT_EQUAL_UINT(KAPPS_MAX_ENTRIES - (2U * KODE_STORAGE_LIST_MAX),
                           p.size[2]);
    for ( uint32_t seq = 1U; KAPPS_MAX_ENTRIES >= seq; ++seq )
    {
        TEST_ASSERT_EQUAL_UINT(1U, p.seq_seen[seq]);
    }
}

/* ---- Where an image is allowed to go --------------------------------------
 *
 * Reachable only because this project carries the board's real partition
 * table: the apps region exists, so `dst=app` is accepted rather than refused
 * at the first check, and the AO gets as far as asking the medium.
 */

TEST_CASE("an install into the apps region reaches the medium", "[appmgr_ao]")
{
    Mockkdrv_slot_Init();
    appmgr_fresh();

    ask_flash_dst("blinky.bin", (uint8_t)KFW_TARGET_APP);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, storage_requests(),
        "the region is present, so nothing should have refused this");

    Mockkdrv_slot_Destroy();
}

/* The coprocessor is AO_Radio's device, and the one-owner rule is what makes
 * that a refusal here rather than a second path to the same chip. */
TEST_CASE("an image for the coprocessor is refused, not routed", "[appmgr_ao]")
{
    Mockkdrv_slot_Init();
    appmgr_fresh();

    ask_flash_dst("c5.bin", (uint8_t)KFW_TARGET_C5);

    TEST_ASSERT_EQUAL_UINT(0U, storage_requests());

    Mockkdrv_slot_Destroy();
}

/* A destination nobody wired up is refused rather than silently treated as
 * one of the ones that exist. */
TEST_CASE("a destination outside the set is refused", "[appmgr_ao]")
{
    Mockkdrv_slot_Init();
    appmgr_fresh();

    ask_flash_dst("whatever.bin", 0x7FU);

    TEST_ASSERT_EQUAL_UINT(0U, storage_requests());

    Mockkdrv_slot_Destroy();
}

/* A URL may be longer than a file name, and the two buffers differ on purpose.
 * Truncating instead of refusing would send the AO looking for a different
 * file and then report that one missing - a wrong answer that reads as a
 * right one. */
TEST_CASE("a file name too long for the buffer is refused, not shortened",
          "[appmgr_ao]")
{
    static char name[KODE_FLASH_SRC_MAX];

    Mockkdrv_slot_Init();
    appmgr_fresh();

    (void)memset(name, 'a', sizeof(name) - 1U);
    name[sizeof(name) - 1U] = '\0';

    ask_flash_dst(name, (uint8_t)KFW_TARGET_APP);

    TEST_ASSERT_EQUAL_UINT(0U, storage_requests());

    Mockkdrv_slot_Destroy();
}

/* ---- Launching what is installed ------------------------------------------
 *
 * These need a real directory, which this binary now has: the entry is
 * committed to flash, the AO looks it up there, and what it posts to AO_Boot
 * is read out of AO_Boot's own queue.
 */

/**
 * @brief Write an image prefix into the region so an entry has a name.
 *
 * A directory entry records where an app is and how big it is - never what it
 * is called. The name lives in the image's own IDF descriptor at
 * KAPP_IDF_DESC_OFF, which is why a listing costs a flash read per entry, and
 * why an entry pointing at erased flash is nameless and so unfindable.
 * Planting one here is what a real install would have left behind.
 *
 * @param offset Region-relative offset of the image, a multiple of KAPPS_ALIGN.
 * @param project The project name the descriptor should carry.
 */
static void plant_app(uint32_t offset, const char *project)
{
    static uint8_t prefix[KAPP_PREFIX_LEN];
    esp_app_desc_t desc;

    (void)memset(prefix, 0xFF, sizeof(prefix));
    (void)memset(&desc, 0, sizeof(desc));
    desc.magic_word = ESP_APP_DESC_MAGIC_WORD;
    (void)snprintf(desc.project_name, sizeof(desc.project_name), "%s", project);
    (void)snprintf(desc.version, sizeof(desc.version), "%s", "0.0.1");
    (void)snprintf(desc.date, sizeof(desc.date), "%s", "Jan  1 2026");
    (void)snprintf(desc.time, sizeof(desc.time), "%s", "00:00:00");
    (void)snprintf(desc.idf_ver, sizeof(desc.idf_ver), "%s", "v9.9.9");
    (void)memcpy(&prefix[KAPP_IDF_DESC_OFF], &desc, sizeof(desc));

    kdrv_apps_write_abort();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_apps_write_begin(offset, sizeof(prefix)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write(prefix, sizeof(prefix)));
    kdrv_apps_write_abort();
}

/** @brief Drain AO_Boot's queue, returning the first BOOT_APP_RUN_SIG in it. */
static bool boot_run_request(uint32_t *offset, uint32_t *size,
                             kboot_run_mode_t *mode)
{
    const QEvt *e;
    bool        found = false;

    while ( xQueueReceive(AO_Boot->eQueue, &e, 0U) == pdTRUE )
    {
        if ( !found && (BOOT_APP_RUN_SIG == e->sig) )
        {
            BootRunEvt const * const ev = (BootRunEvt const *)e;
            *offset = ev->offset;
            *size   = ev->size;
            *mode   = ev->mode;
            found   = true;
        }
        QF_gc(e);
    }
    return found;
}

TEST_CASE("running an app that is not installed asks nobody to boot anything",
          "[appmgr_ao]")
{
    AppFlashEvt      e;
    uint32_t         off  = 0U;
    uint32_t         size = 0U;
    kboot_run_mode_t mode = KBOOT_RUN_ONCE;

    Mockkdrv_slot_Init();
    appmgr_fresh();
    (void)boot_run_request(&off, &size, &mode);   /* start from an empty queue */

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(APP_RUN_REQ_SIG);
    (void)snprintf(e.file, sizeof(e.file), "%s", "nothing-here");

    QASM_DISPATCH(AO_AppMgr, &e.super, 0U);

    TEST_ASSERT_FALSE_MESSAGE(boot_run_request(&off, &size, &mode),
        "a name that is not in the directory must not become a boot request");

    Mockkdrv_slot_Destroy();
}

/* The one that matters: the offset and size the bootloader will jump to come
 * out of the directory, not out of the request. A caller cannot name a place
 * to jump to - it can only name an app that was installed at one. */
TEST_CASE("running an installed app boots the extent the directory recorded",
          "[appmgr_ao]")
{
    AppFlashEvt      e;
    kapps_dir_t      dir;
    uint32_t         off  = 0U;
    uint32_t         size = 0U;
    kboot_run_mode_t mode = KBOOT_RUN_STICKY;

    Mockkdrv_slot_Init();
    appmgr_fresh();
    (void)boot_run_request(&off, &size, &mode);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_init());
    kapps_dir_t const saved = *kdrv_apps_dir();

    dir = saved;
    dir.count       = 1U;
    dir.e[0].offset = KAPPS_DATA_OFF + (2U * KAPPS_ALIGN);
    dir.e[0].size   = 0x12340U;
    dir.e[0].seq    = 1U;
    dir.e[0].flags  = KAPPS_ENTRY_VALID;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));

    /* The name is not in the directory - it comes from the image's own IDF
     * descriptor - so an app has to be planted in flash for the lookup to
     * find anything. */
    plant_app(dir.e[0].offset, "blinky");

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(APP_RUN_REQ_SIG);
    (void)snprintf(e.file, sizeof(e.file), "%s", "blinky");

    QASM_DISPATCH(AO_AppMgr, &e.super, 0U);

    TEST_ASSERT_TRUE(boot_run_request(&off, &size, &mode));
    TEST_ASSERT_EQUAL_HEX32(KAPPS_DATA_OFF + (2U * KAPPS_ALIGN), off);
    TEST_ASSERT_EQUAL_HEX32(0x12340U, size);
    /* `app run` is one launch, not a standing order: the console asked for
     * this boot, and the next reset must come home to kodeOS. */
    TEST_ASSERT_EQUAL_INT(KBOOT_RUN_ONCE, mode);
    /* The boot request is the whole launch. The panel goes dark across the
     * restart by its own driver's hand (kdrv_display's shutdown handler holds
     * its reset line low), so a brightness asked of AO_Display here would be
     * a second mechanism for the same darkness. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_neighbour_display_posts(),
        "a launch asked AO_Display for something; the panel's driver darkens it");

    kapps_dir_t restore = saved;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&restore));

    Mockkdrv_slot_Destroy();
}

/** @brief The image image_named() builds; a case needs one at a time. */
static uint8_t s_named_img[512];

/** @brief Bytes of the one segment image_chain() declares. */
#define CHAIN_SEG_LEN  16U

/** @brief The least bytes image_chain() needs: both headers, the segment, the checksum pad. */
#define CHAIN_MIN_LEN  64U

/**
 * @brief Make the head of a stand-in image a header chain that holds.
 *
 * The region walks an image's chain of segments before it hashes it, and
 * refuses one that leaves the image's own bytes. A stand-in whose case is
 * about something else still has to be an image that far: one segment of
 * CHAIN_SEG_LEN bytes, no appended hash, this chip. What follows the two
 * headers is left as the caller filled it.
 *
 * @param img The image, at least CHAIN_MIN_LEN bytes.
 */
static void image_chain(uint8_t *img)
{
    esp_image_header_t         header;
    esp_image_segment_header_t seg;

    (void)memset(&header, 0, sizeof(header));
    header.magic         = ESP_IMAGE_HEADER_MAGIC;
    header.chip_id       = ESP_CHIP_ID_ESP32P4;
    header.segment_count = 1U;
    header.hash_appended = 0U;
    (void)memcpy(&img[0], &header, sizeof(header));

    (void)memset(&seg, 0, sizeof(seg));
    seg.load_addr = 0x40000000U;
    seg.data_len  = CHAIN_SEG_LEN;
    (void)memcpy(&img[sizeof(header)], &seg, sizeof(seg));
}

/**
 * @brief Build a minimal, walkable ESP image whose descriptor names
 *        @p project, in s_named_img.
 *
 * kdrv_apps_pending_present() does not verify what is parked, it WALKS it:
 * the image header, then one segment header per segment, then the 1-byte
 * checksum rounded up to 16. So the smallest thing adoption will accept is a
 * header, a single segment, and the pad - and making that segment exactly as
 * long as an IDF descriptor puts the descriptor at KAPP_IDF_DESC_OFF, which
 * is what gives the app a name once it is in the region. The same bytes serve
 * a terminal install: the header's magic is the raw image the decoder sniffs,
 * so they go straight to the region, where kdrv_apps_desc() finds the
 * name.
 *
 * @param project The project name the descriptor should carry.
 * @return The image length the walk will report, which is what gets copied.
 */
static uint32_t image_named(const char *project)
{
    esp_image_header_t         header;
    esp_image_segment_header_t seg;
    esp_app_desc_t             desc;
    uint32_t                   len;

    TEST_ASSERT_EQUAL_UINT(KAPP_IDF_DESC_OFF,
                           sizeof(header) + sizeof(seg));

    (void)memset(s_named_img, 0, sizeof(s_named_img));

    (void)memset(&header, 0, sizeof(header));
    header.magic         = ESP_IMAGE_HEADER_MAGIC;
    header.chip_id       = ESP_CHIP_ID_ESP32P4;  /* the decoder checks it */
    header.segment_count = 1U;
    header.hash_appended = 0U;
    (void)memcpy(&s_named_img[0], &header, sizeof(header));

    (void)memset(&seg, 0, sizeof(seg));
    seg.load_addr = 0x40000000U;
    seg.data_len  = (uint32_t)sizeof(desc);
    (void)memcpy(&s_named_img[sizeof(header)], &seg, sizeof(seg));

    (void)memset(&desc, 0, sizeof(desc));
    desc.magic_word = ESP_APP_DESC_MAGIC_WORD;
    (void)snprintf(desc.project_name, sizeof(desc.project_name), "%s", project);
    (void)snprintf(desc.version, sizeof(desc.version), "%s", "0.0.1");
    (void)snprintf(desc.date, sizeof(desc.date), "%s", "Jan  1 2026");
    (void)snprintf(desc.time, sizeof(desc.time), "%s", "00:00:00");
    (void)snprintf(desc.idf_ver, sizeof(desc.idf_ver), "%s", "v9.9.9");
    (void)memcpy(&s_named_img[KAPP_IDF_DESC_OFF], &desc, sizeof(desc));

    len = (uint32_t)(sizeof(header) + sizeof(seg) + sizeof(desc));
    len = (len | 15U) + 1U;
    TEST_ASSERT_TRUE((uint32_t)sizeof(s_named_img) >= len);
    return len;
}

/**
 * @brief Park image_named()'s image in the development mailbox.
 *
 * The whole image is well under one adoption chunk, so the copy is a single
 * APP_FLASH_CHUNK_SIG.
 *
 * @param project The project name the descriptor should carry.
 * @return The image length the walk will report, which is what gets copied.
 */
static uint32_t park_pending(const char *project)
{
    uint32_t const len = image_named(project);

    kdrv_apps_pending_write_abort();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_write_begin(len));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_write(s_named_img, len));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_write_end());
    return len;
}

/* The other half of the same question, and the one the mode exists for. An
 * app that arrived through the mailbox was flashed by a host tool that is
 * about to open the console port, and opening the port resets the board - so
 * a one-shot request would be spent on a reset nobody asked for and the
 * operator would be looking at the launcher. Sticky is what survives that.
 *
 * The mailbox is planted AFTER appmgr_fresh(), because AppMgr_initial looks
 * at it and self-posts APP_ADOPT_SIG when it finds something - and
 * appmgr_fresh() drains that queue. Dispatching the signal by hand is the
 * same entry, at a moment the case controls. */
TEST_CASE("an app adopted from the mailbox is launched sticky", "[appmgr_ao]")
{
    static const QEvt adopt = QEVT_INITIALIZER(APP_ADOPT_SIG);
    kapps_dir_t       dir;
    uint32_t          off  = 0U;
    uint32_t          size = 0U;
    kboot_run_mode_t  mode = KBOOT_RUN_ONCE;

    Mockkdrv_slot_Init();
    appmgr_fresh();
    (void)boot_run_request(&off, &size, &mode);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_init());
    kapps_dir_t const saved = *kdrv_apps_dir();

    /* An empty directory, so the plan puts the image at the first slot and
     * nothing has to be evicted to make room for 304 bytes. */
    dir       = saved;
    dir.count = 0U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));

    uint32_t const len = park_pending("adopted");

    QASM_DISPATCH(AO_AppMgr, &adopt, 0U);
    /* The copy is paced by a QTimeEvt, one chunk per tick; this image is one
     * chunk, and the ticks after it are what leave the pacer disarmed. */
    run_ticks(4U);

    TEST_ASSERT_TRUE_MESSAGE(boot_run_request(&off, &size, &mode),
        "adoption must end in a boot request, or an uploaded app never runs");
    TEST_ASSERT_EQUAL_HEX32(KAPPS_DATA_OFF, off);
    TEST_ASSERT_EQUAL_HEX32(len, size);
    TEST_ASSERT_EQUAL_INT(KBOOT_RUN_STICKY, mode);
    /* The same launch as `app run`, sticky: nothing for AO_Display. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_neighbour_display_posts(),
        "an adoption's launch asked AO_Display for something");

    /* Adoption consumes the mailbox itself; leaving anything there would send
     * the next case's AppMgr_initial straight back into adoption. */
    TEST_ASSERT_FALSE(kdrv_apps_pending_present(NULL, NULL));

    kapps_dir_t restore = saved;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&restore));

    Mockkdrv_slot_Destroy();
}

/* An adoption's launch and the listing name the same place. The first slot is
 * taken, so the image lands on the second, and the offset the sticky request
 * carries (the one boot_app_offset() answers with after the run) is the one the
 * listing shows beside the app's name. */
TEST_CASE("an adopted app's listed offset is the one its launch asked for",
          "[appmgr_ao]")
{
    static const QEvt adopt = QEVT_INITIALIZER(APP_ADOPT_SIG);
    AppListReqEvt     list  = { .super = QEVT_INITIALIZER(APP_LIST_REQ_SIG),
                                .skip = 0U, .who = ao_spy() };
    kapps_dir_t       dir;
    uint32_t          off  = 0U;
    uint32_t          size = 0U;
    kboot_run_mode_t  mode = KBOOT_RUN_ONCE;
    bool              seen = false;

    Mockkdrv_slot_Init();
    appmgr_fresh();
    (void)boot_run_request(&off, &size, &mode);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_init());
    kapps_dir_t const saved = *kdrv_apps_dir();

    dir             = saved;
    dir.count       = 1U;
    dir.e[0].offset = KAPPS_DATA_OFF;
    dir.e[0].size   = 0x1000U;
    dir.e[0].seq    = 1U;
    dir.e[0].flags  = KAPPS_ENTRY_VALID;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));

    (void)park_pending("adopted");
    QASM_DISPATCH(AO_AppMgr, &adopt, 0U);
    run_ticks(4U);

    TEST_ASSERT_TRUE(boot_run_request(&off, &size, &mode));
    TEST_ASSERT_EQUAL_INT(KBOOT_RUN_STICKY, mode);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(KAPPS_DATA_OFF, off,
        "the image landed on the slot that was taken");

    ao_spy_drain();
    QASM_DISPATCH(AO_AppMgr, &list.super, 0U);
    for ( const QEvt *e = ao_spy_take(); NULL != e; e = ao_spy_take() )
    {
        if ( (int)e->sig == APP_ENTRY_SIG )
        {
            AppEntryEvt const * const en = (AppEntryEvt const *)e;
            if ( strcmp(en->name, "adopted") == 0 )
            {
                TEST_ASSERT_EQUAL_HEX32(off, en->offset);
                seen = true;
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(seen, "the listing never named the adopted app");
    ao_spy_drain();

    kapps_dir_t restore = saved;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&restore));
    Mockkdrv_slot_Destroy();
}

/* The keep cases at the end of the file own these; the mailbox case below
 * answers the round they describe. */
static const QEvt *storage_take(enum_t sig);
static bool storage_has_for(enum_t sig, const char *project);
static void rm_rsp(const char *project, kode_status_t st);
static void wipe_rsp(const char *project, kode_status_t st, uint8_t fs_files,
                     uint8_t sd_files, uint8_t left);
static void        answer_stat(bool exists, uint8_t rc);

/* Fix 2: adoption plans and evicts BEFORE the copy, and the copy is what the
 * crypto verify judges - so without a source check a torn upload (headers
 * intact, data or hash bad) would evict a live app to make room and only then
 * find the image is junk. kdrv_apps_pending_verify() checks the mailbox in
 * place first, but only when the plan would evict, so a normal upload with room
 * to spare pays no extra hash. This drives the eviction path deterministically:
 * one app filling the region, a mailbox image the verify rejects, and the
 * installed app must survive. */
TEST_CASE("a mailbox image that fails verification does not evict an app for room",
          "[appmgr_ao]")
{
    static const QEvt adopt = QEVT_INITIALIZER(APP_ADOPT_SIG);
    uint32_t          off  = 0U;
    uint32_t          size = 0U;
    kboot_run_mode_t  mode = KBOOT_RUN_ONCE;

    Mockkdrv_slot_Init();
    appmgr_fresh();
    (void)boot_run_request(&off, &size, &mode);   /* drain any standing request */

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_init());
    kapps_dir_t const saved = *kdrv_apps_dir();

    /* One live app that fills the whole region, so adopting anything forces
     * the byte planner to evict it - the path the pre-verify guards. It is
     * the only app, so it is the eviction victim. */
    kapps_dir_t dir = saved;
    dir.count       = 1U;
    dir.e[0].offset = KAPPS_DATA_OFF;
    dir.e[0].size   = kdrv_apps_region_size() - KAPPS_DATA_OFF;
    dir.e[0].seq    = 1U;
    dir.e[0].flags  = KAPPS_ENTRY_VALID;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));

    /* A torn/corrupt upload: the headers walk, but the crypto verify rejects
     * it. */
    (void)park_pending("torn");
    fake_image_verify_set_result(ESP_ERR_IMAGE_INVALID);

    QASM_DISPATCH(AO_AppMgr, &adopt, 0U);
    run_ticks(4U);

    /* Nothing adopted, and - the point of the fix - the installed app is still
     * there: it was not evicted for an image that never verified. */
    TEST_ASSERT_FALSE_MESSAGE(boot_run_request(&off, &size, &mode),
        "a mailbox image that fails verification must not be adopted");
    kapps_dir_t const after = *kdrv_apps_dir();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, after.count,
        "a bad upload must not cost the installed app its slot");
    TEST_ASSERT_EQUAL_HEX32(KAPPS_DATA_OFF, after.e[0].offset);
    TEST_ASSERT_FALSE(kdrv_apps_pending_present(NULL, NULL));   /* mailbox consumed */

    /* And a verifying image still adopts, evicting the old app as planned. */
    fake_image_verify_set_result(ESP_OK);
    (void)park_pending("good");
    QASM_DISPATCH(AO_AppMgr, &adopt, 0U);
    /* The victim is copied first, now: say it is already on the card. The
     * directory entry may name nothing the round can read, in which case it
     * restarts at once - pump that too. */
    (void)qf_support_pump(AO_AppMgr, 8U);
    const QEvt *s = storage_take(STORAGE_PATH_REQ_SIG);
    if ( NULL != s )
    {
        QF_gc(s);
        answer_stat(true, (uint8_t)KODE_OK);
    }
    run_ticks(4U);
    TEST_ASSERT_TRUE_MESSAGE(boot_run_request(&off, &size, &mode),
        "a verifying image must adopt even when the region was full");

    fake_image_verify_set_result(ESP_OK);   /* the suite is order-coupled */
    kapps_dir_t restore2 = saved;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&restore2));
    Mockkdrv_slot_Destroy();
}

/* ---- The receive protocol (`install src="terminal"`) ---------------------
 *
 * Every case above stops in AppMgr_opening, at the latest - none of them
 * ever reaches AppMgr_receiving, the state that owns the whole streaming
 * install: the ready handshake, the ack ledger, the CRC verdict, the idle
 * watchdog that gives up on a host that went quiet. A mutation exercise
 * found that gap by breaking the ack accounting on purpose and re-running
 * the suite - zero failures, because nothing here ever sent AO_AppMgr a
 * single byte. The four cases below are what closes it.
 *
 * These cases drive the REAL kdrv_kfw decoder, not a mock of it, and that is
 * a deliberate correction of the plan this file started from. Mockkdrv_kfw
 * exists (see mocks/kdrv_kfw/ and its already-generated header) and looked
 * like the obvious tool - until using it was tried here and it broke every
 * OTHER case in this binary that touches kfw_hdr_parse/kfw_crc32/kfw_sha256/
 * kfw_buf_sink, including test_kdrv_kfw.c's own suite. The reason is that
 * this file shares ONE executable with those tests (unlike kdrv_net or
 * kdrv_slot, which get the real driver tested in a wholly separate `net`/
 * `slot` project and only ever see the mock here): pulling in even one
 * Mockkdrv_kfw_* symbol drags its whole translation unit into the link,
 * which defines every kfw_* function a second time, and the mock's
 * definition wins process-wide, permanently, for every case that runs after
 * - there is no per-TEST_CASE scoping for which implementation a symbol
 * resolves to. `main/CMakeLists.txt`'s "kdrv_kfw" REQUIRES entry is a
 * latent trap for exactly this reason (a real defect - see the report this
 * task hands back rather than a fix, since fixing it means giving kdrv_kfw
 * its own suite the way net/slot/time/nvs already have theirs, which is a
 * bigger, separate change).
 *
 * Driving the real decoder turned out to need nothing exotic. A payload
 * whose first byte is KFW_ESP_MAGIC (0xE9) sniffs as a raw ESP image and
 * skips the whole KFW1 container/SHA-256 path (kdrv_kfw.c's KFW_ST_RAW),
 * going straight to the sink - kdrv_apps_sink(), which is never mocked in
 * this suite (this project carries the board's real partition table, the
 * same reason the cases above can ask for `dst=app` at all). Flash-level
 * image validation is a non-issue on the host: fake_khal_system.c's
 * esp_image_verify() always returns ESP_OK, unconditionally, so kdrv_apps_
 * write_end() never actually judges the bytes these cases write - it is the
 * receive protocol under test here, not the ESP image format.
 *
 * One piece of bookkeeping this buys nothing for: kdrv_kfw.c keeps its own
 * decode session in a file-static struct with no reset hook except
 * kfw_end()/kfw_abort(), and kfw_begin() refuses a second session outright
 * while one is open. AppMgr_receiving closes it on every path except one
 * (see the ready-handshake case) - anywhere production code would not close
 * it for us, this file closes it by hand, or the very next case (in this
 * file OR in test_kdrv_kfw.c, alphabetically right after it) opens against
 * a decoder that is still busy.
 */

/**
 * @brief appmgr_fresh() plus the pieces of module state it does not reach.
 *
 * Three things this file has to clean up by hand, none of which
 * AppMgr_ctor() touches:
 *
 * kdrv_kfw's decode session and kdrv_apps' write session both live in
 * file-static structs private to their own .c files - a case that leaves
 * either open hands it to whichever case runs next. Both abort calls are
 * safe to make unconditionally: kfw_abort() on an idle session and
 * kdrv_apps_write_abort() on a closed one are both defined as no-ops (see
 * their own headers), so this is cheap insurance whether or not the
 * previous case actually needed it.
 *
 * The tick, first, is the one that took a hang to find. AppMgr_ctor()
 * re-constructs me->idle/pacer/answer on every call via QTimeEvt_ctorX(),
 * which zeroes QTimeEvt.flags unconditionally - including the bit that says
 * the timer is linked into QF's own list. QP/C's own comment on
 * QTimeEvt_rearm() explains why that matters: a freshly-armed timer goes on
 * a SEPARATE side-list first, and "unlinking is performed exclusively in
 * QTimeEvt_tick_()" - nothing else ever reconciles it. A case that arms
 * me->idle (every one of these four does, through raw_armed()) and then
 * ends without a single tick leaves it on that side-list with the linked
 * flag now lyingly cleared, so the NEXT case's arm sees "unlinked" and
 * inserts the very same node again - onto a list that node is already the
 * head of. Two insertions of one node make it point at itself, and the
 * first run_ticks() anywhere downstream spins forever trying to walk off
 * the end of a one-node cycle that has none. One tick here, before
 * appmgr_fresh() ever re-ctors anything, is what flushes that side-list
 * while it can still be flushed honestly.
 */
static void recv_fresh(void)
{
    (void)qf_support_pump(AO_AppMgr, 8U);
    appmgr_fresh();
    kfw_abort();
    kdrv_apps_write_abort();
}

/** @brief ask_recv(), with the project name the host read from the image. */
static void ask_recv_named(uint32_t size, uint32_t crc32_val, bool launch,
                           const char *name)
{
    AppRecvReqEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super  = (QEvt)QEVT_INITIALIZER(APP_RECV_REQ_SIG);
    e.dst    = (uint8_t)KFW_TARGET_APP;
    e.size   = size;
    e.crc32  = crc32_val;
    e.launch = launch;
    e.src    = (uint8_t)KODE_CONSOLE_SRC_SERIAL;
    (void)snprintf(e.name, sizeof(e.name), "%s", name);
    QASM_DISPATCH(AO_AppMgr, &e.super, 0U);
}

/** @brief Ask AO_AppMgr to receive an image, exactly as a terminal install
 *         to `dst=app` does. */
static void ask_recv(uint32_t size, uint32_t crc32_val, bool launch)
{
    ask_recv_named(size, crc32_val, launch, "");
}

/**
 * @brief Answer the console-arm request the way AO_Log would, once raw mode
 *        is (or could not be) armed.
 */
static void raw_armed(bool ok)
{
    ConsoleRawRspEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_RAW_RSP_SIG);
    if ( ok )
    {
        e.status = (uint8_t)KODE_OK;
    }
    else
    {
        e.status = (uint8_t)KODE_ERROR;
    }
    QASM_DISPATCH(AO_AppMgr, &e.super, 0U);
}

/**
 * @brief Feed one chunk of the arriving image, exactly as the console
 *        reader task posts RECV_DATA_SIG.
 *
 * Static, not a stack variable: RecvDataEvt carries a whole KODE_RECV_CHUNK
 * buffer, which is why it is the biggest event in the system and the entire
 * reason the jumbo pool exists (see AppMgr.chunk's own comment in
 * appmgr_ao.c). Dispatched directly with QASM_DISPATCH rather than posted
 * through a queue, its poolId stays 0 - a QEVT_INITIALIZER event is never
 * pool-allocated - so nothing here ever touches that pool at all.
 */
static void feed(const uint8_t *data, uint16_t len)
{
    static RecvDataEvt e;

    e.super = (QEvt)QEVT_INITIALIZER(RECV_DATA_SIG);
    e.len   = len;
    (void)memcpy(e.data, data, len);
    QASM_DISPATCH(AO_AppMgr, &e.super, 0U);
}

/**
 * @brief Drain AO_Log's queue, reporting whether @p sig was among what
 *        landed there.
 *
 * The same shape storage_requests() above reads from AO_Storage: AO_Log is a
 * stand-in with a queue and no task here, so whatever AO_AppMgr posted to it
 * just sits there until a case asks.
 */
static bool log_saw(enum_t sig)
{
    const QEvt *e;
    bool        found = false;

    while ( xQueueReceive(AO_Log->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (enum_t)e->sig == sig )
        {
            found = true;
        }
        QF_gc(e);
    }
    return found;
}

/** @brief The idle deadline AppMgr_receiving arms once writing starts, in QP
 *  ticks - the same arithmetic appmgr_ao.c's own (private)
 *  APPMGR_RECV_IDLE_TICKS makes, from the same config symbol. */
#define APPMGR_RECV_IDLE_DEADLINE_TICKS (KODE_RECV_IDLE_MS / 10U)

/* The ready handshake: nothing may be ready before the console has agreed to
 * go raw, and it must be ready exactly once after - never zero (the host
 * would wait out its own watchdog for an answer that is not coming) and
 * never twice (a second `>|ready` on the wire is a bug the host has no way
 * to make sense of).
 *
 * The one case of these four that never feeds a byte, so it is also the one
 * case that never closes the kfw session AppMgr_receiving opened
 * (kfw_begin() succeeds unconditionally for any real sink, and nothing here
 * ever gets far enough to call kfw_feed()) - hence the explicit kfw_abort()
 * at the end, where every other case gets that for free from recv_close().
 */
TEST_CASE("the ready handshake opens the region and answers exactly once",
          "[appmgr_ao]")
{
    unsigned ready_count = 0U;

    recv_fresh();
    ao_spy_subscribe(RECV_READY_SIG);

    ask_recv(4096U, 0x00000000U, false);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "nothing may be ready before the console has agreed to go raw");

    raw_armed(true);

    for ( ;; )
    {
        const QEvt * const got = ao_spy_take();
        if ( NULL == got )
        {
            break;
        }
        if ( (int)got->sig == RECV_READY_SIG )
        {
            ++ready_count;
        }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, ready_count,
        "`>|ready` must reach the host exactly once, or the host either "
        "waits forever or hears a signal it cannot explain");

    kfw_abort();   /* this case's own session never closed itself - see above */
}

/**
 * @brief The ack ledger: one `>|ack` per whole chunk, none for a trailing
 *        partial one, and neither count moved by a write that fails
 *        part-way through - and the verdict that write failure earns.
 *
 * Two scenarios in one case, each getting its own recv_fresh() so neither
 * inherits the other's tally:
 *
 *   Phase A - a clean transfer of 3 whole chunks plus a 100-byte remainder.
 *   floor(12388 / 4096) is 3, and that is exactly how many acks must go out -
 *   this is the accounting the ack-count mutation was caught breaking, and
 *   this phase is what has to die when that mutation comes back.
 *
 *   Phase B - the SAME byte layout, but the region write fails right after
 *   the first chunk (kdrv_apps_write_abort() called by hand, between the
 *   first feed() and the second, to force the next real kdrv_apps_write()
 *   to see a closed session and refuse). AppMgr_receiving's own comment says
 *   a failed write "stops the writing, never the draining or the acks" -
 *   and reading the handler confirms the acking half: the ack for a whole
 *   chunk is published unconditionally on `whole`, never gated on whether
 *   that chunk's write succeeded, so the count must still be 3.
 *
 *   What the write failure earns is the verdict, and getting that right
 *   needed two separate counters where there used to be one. `me->written`
 *   is the sink's own produced count (kfw_produced(), which does not
 *   advance on a write that failed) - it used to be the ONLY count the
 *   completion check compared against `me->total`, and xfer_feed()
 *   reassigned it unconditionally on every call, including the failing
 *   one. So the instant a write failed, `me->written` was silently
 *   rewound by exactly that chunk's size, and since `recv_ok` then keeps
 *   xfer_feed() from ever running again for the rest of the transfer,
 *   nothing ever paid the rewind back: `written` could never again reach
 *   `total`, the data path's own completion check never fired, and the
 *   only thing that ever closed the transfer out was the five-second idle
 *   watchdog - which always reports a STALL (kcli error 6, "the host went
 *   quiet"), the wrong diagnosis for what was actually a write failure
 *   (kcli error 5, the one the operator needed to see). `me->recv_wire` is
 *   the fix: it counts what has left the wire, independently of what the
 *   sink produced, so a failed write cannot rewind it. It is what the
 *   completion check compares against `total` now, and it reaches `total`
 *   the instant the last byte is drained - the very feed() call that used
 *   to fall silent. Phase B below asserts that: RECV_FAIL_SIG the moment
 *   the payload is fully drained, never a STALL, and nothing left over for
 *   the watchdog to report.
 *
 * Both phases deliberately promise a CRC that will not match, so neither
 * one reaches region_install() - this case is about the ack ledger, not the
 * directory, and staying off that path means there is nothing to restore
 * afterward.
 */
TEST_CASE("the ack count is one per whole chunk, none for the remainder, "
          "and a mid-transfer write failure moves neither",
          "[appmgr_ao]")
{
    static uint8_t zeros[KODE_RECV_CHUNK];
    uint32_t const total = (3U * (uint32_t)KODE_RECV_CHUNK) + 100U;
    unsigned       ack_count;
    unsigned       done_count;
    unsigned       fail_count;
    unsigned       stall_count;
    const QEvt    *got;

    /* Byte 0 is the only one that matters to the decoder: it is what makes
     * kfw_feed() sniff this as a raw ESP image on its very first call and
     * skip straight to writing, rather than buffering 72 bytes looking for
     * a KFW1 container header it will never find. Everything after it is
     * filler - the ack ledger does not care what the image contains. */
    zeros[0] = KFW_ESP_MAGIC;
    zeros[12] = (uint8_t)ESP_CHIP_ID_ESP32P4;   /* its chip_id: this one */
    zeros[13] = 0U;

    /* ---- Phase A: every write succeeds ------------------------------- */
    recv_fresh();
    ao_spy_subscribe(RECV_ACK_SIG);
    ao_spy_subscribe(RECV_DONE_SIG);
    ao_spy_subscribe(RECV_FAIL_SIG);

    ask_recv(total, 0xDEADBEEFU, false);   /* deliberately wrong CRC */
    raw_armed(true);
    feed(zeros, (uint16_t)KODE_RECV_CHUNK);
    feed(zeros, (uint16_t)KODE_RECV_CHUNK);
    feed(zeros, (uint16_t)KODE_RECV_CHUNK);
    feed(zeros, 100U);

    ack_count = 0U; done_count = 0U; fail_count = 0U;
    for ( ;; )
    {
        got = ao_spy_take();
        if ( NULL == got )
        {
            break;
        }
        if ( (int)got->sig == RECV_ACK_SIG )
        {
            ++ack_count;
        }
        if ( (int)got->sig == RECV_DONE_SIG )
        {
            ++done_count;
        }
        if ( (int)got->sig == RECV_FAIL_SIG )
        {
            ++fail_count;
        }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, ack_count,
        "floor(12388 / 4096) is 3 - the trailing 100 bytes must not earn "
        "a fourth ack the host is not waiting for");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, done_count, "the CRC was wrong on purpose");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fail_count,
        "the transfer must still close out, not hang mid-drain");

    /* ---- Phase B: the region write fails after the first chunk -------- */
    recv_fresh();
    ao_spy_subscribe(RECV_ACK_SIG);
    ao_spy_subscribe(RECV_STALL_SIG);
    ao_spy_subscribe(RECV_DONE_SIG);
    ao_spy_subscribe(RECV_FAIL_SIG);

    ask_recv(total, 0xDEADBEEFU, false);
    raw_armed(true);
    feed(zeros, (uint16_t)KODE_RECV_CHUNK);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_RECV_CHUNK, kfw_produced(),
        "the first chunk must have gone through cleanly before this test "
        "breaks the session on purpose");

    /* The induced failure: close the region's write session out from under
     * the decoder, exactly what a real flash error would leave behind. */
    kdrv_apps_write_abort();

    feed(zeros, (uint16_t)KODE_RECV_CHUNK);   /* this write now fails */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_RECV_CHUNK, kfw_produced(),
        "kfw_produced() must not move past the failed chunk - nothing new "
        "was actually written");

    feed(zeros, (uint16_t)KODE_RECV_CHUNK);   /* drained, not written */
    feed(zeros, 100U);                        /* drained, not written, no ack */

    ack_count = 0U; done_count = 0U; fail_count = 0U; stall_count = 0U;
    for ( ;; )
    {
        got = ao_spy_take();
        if ( NULL == got )
        {
            break;
        }
        if ( (int)got->sig == RECV_ACK_SIG )
        {
            ++ack_count;
        }
        if ( (int)got->sig == RECV_DONE_SIG )
        {
            ++done_count;
        }
        if ( (int)got->sig == RECV_FAIL_SIG )
        {
            ++fail_count;
        }
        if ( (int)got->sig == RECV_STALL_SIG )
        {
            ++stall_count;
        }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, ack_count,
        "a write failure must not cost the host a single ack it is owed - "
        "the wire protocol keeps going even though the flash write did not");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, done_count,
        "a write that failed anywhere must never install");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fail_count,
        "the transfer must close out the moment the payload is fully "
        "drained, honestly reporting the write failure - see the case "
        "comment for why this used to need a five-second watchdog instead");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, stall_count,
        "a write failure is diagnosed on the spot; the idle watchdog never "
        "gets a chance to run, let alone misreport it as a stall");

    /* Nothing is left to end: recv_close() already disarmed the idle timer
     * and transitioned back to AppMgr_idle when the last byte was drained
     * above. Sweeping well past the old watchdog deadline here proves that -
     * a leftover armed timer or a state that never left AppMgr_receiving
     * would show up as a second verdict on top of the one already seen. */
    run_ticks(APPMGR_RECV_IDLE_DEADLINE_TICKS + 50U);

    done_count = 0U; fail_count = 0U; stall_count = 0U;
    for ( ;; )
    {
        got = ao_spy_take();
        if ( NULL == got )
        {
            break;
        }
        if ( (int)got->sig == RECV_DONE_SIG )
        {
            ++done_count;
        }
        if ( (int)got->sig == RECV_FAIL_SIG )
        {
            ++fail_count;
        }
        if ( (int)got->sig == RECV_STALL_SIG )
        {
            ++stall_count;
        }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, done_count, "still never installed");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fail_count,
        "the verdict above was the only one - nothing fires a second time");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, stall_count,
        "the idle watchdog has nothing left to time out; the transfer was "
        "already closed before this sweep started");
}

/**
 * @brief The CRC verdict: a byte-for-byte match installs and says so; one
 *        flipped byte on the wire never does.
 *
 * The expected CRC is computed with the real khal_crc32() (test_khal_crc32.c
 * already covers that function on its own) rather than hard-coded, so this
 * case is honest about testing AO_AppMgr's use of the checksum and not a
 * magic number that happens to agree with it.
 *
 * Phase A is the only one of these four cases that reaches region_install()
 * and commits a real directory entry - this project carries the board's
 * actual partition table, so that commit is real flash, not a stub, and it
 * has to be put back before the case returns or the next case (in this file
 * or any other sharing the binary) inherits a directory entry it never
 * asked for. What region_install() writes verifies against
 * fake_khal_system.c's unconditional esp_image_verify() stub rather than a
 * genuine ESP image format check - this case is about the wire's CRC, not
 * about whether these 64 bytes would boot.
 */
/* The chunk buffer is PSRAM taken once at the AO's start. A boot that had
 * none for it refuses every stream at the request, before the console is
 * armed, so the wire is never taken for a transfer with nowhere to stage. */
TEST_CASE("no PSRAM for the chunk buffer refuses a stream before the console "
          "is armed", "[appmgr_ao]")
{
    unsigned    fail_count = 0U;
    const QEvt *got;

    recv_fresh();
    fake_psram_refuse(1U);                    /* the chunk buffer */
    AppMgr_ctor();
    QASM_INIT(AO_AppMgr, (void *)0, 0U);
    ao_spy_subscribe(RECV_FAIL_SIG);

    ask_recv(64U, 0U, false);

    for ( ;; )
    {
        got = ao_spy_take();
        if ( NULL == got )
        {
            break;
        }
        if ( (int)got->sig == RECV_FAIL_SIG )
        {
            ++fail_count;
        }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fail_count,
        "a stream with no buffer to stage in must be refused at once");
    TEST_ASSERT_FALSE_MESSAGE(log_saw(CONSOLE_RAW_REQ_SIG),
        "...and the console must never have been asked to go raw");

    recv_fresh();                             /* the next case gets a buffer */
}

TEST_CASE("the CRC verdict is RECV_DONE_SIG on a match, RECV_FAIL_SIG on one "
          "flipped byte", "[appmgr_ao]")
{
    static uint8_t payload[64];
    uint32_t       good_crc;
    unsigned       i;
    kapps_dir_t    saved;
    unsigned       done_count;
    unsigned       fail_count;
    const QEvt    *got;

    for ( i = 0U; sizeof(payload) > i; ++i )
    {
        /* Any fixed, non-repeating pattern - an all-zero payload would still
         * exercise the comparison, but this one cannot be mistaken for an
         * accidental match against an uninitialised want. */
        payload[i] = (uint8_t)((i * 7U) + 3U);
    }
    /* Byte 0 must be the raw-image magic - see the ack case above for why -
     * and the rest of the head a chain the region's walk accepts. */
    TEST_ASSERT_TRUE(sizeof(payload) >= CHAIN_MIN_LEN);
    image_chain(payload);
    good_crc = khal_crc32(KHAL_CRC32_INIT, payload, sizeof(payload));

    /* ---- Phase A: the wire matches the promise ------------------------- */
    recv_fresh();
    saved = *kdrv_apps_dir();
    ao_spy_subscribe(RECV_DONE_SIG);
    ao_spy_subscribe(RECV_FAIL_SIG);

    ask_recv((uint32_t)sizeof(payload), good_crc, false);
    raw_armed(true);
    feed(payload, (uint16_t)sizeof(payload));

    done_count = 0U; fail_count = 0U;
    for ( ;; )
    {
        got = ao_spy_take();
        if ( NULL == got )
        {
            break;
        }
        if ( (int)got->sig == RECV_DONE_SIG )
        {
            ++done_count;
        }
        if ( (int)got->sig == RECV_FAIL_SIG )
        {
            ++fail_count;
        }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, done_count,
        "a byte-for-byte match must install and say so");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fail_count, "...and only that");

    /* Put the directory back exactly as this case found it. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&saved));

    /* ---- Phase B: one byte on the wire betrays the promise ------------- */
    recv_fresh();
    ao_spy_subscribe(RECV_DONE_SIG);
    ao_spy_subscribe(RECV_FAIL_SIG);

    {
        static uint8_t corrupted[sizeof(payload)];
        (void)memcpy(corrupted, payload, sizeof(payload));
        corrupted[10] ^= 0xFFU;

        /* Still promising the ORIGINAL crc: the host committed to that
         * number in the `install` command line, before a single byte
         * moved. What went wrong here is the wire, never the promise. */
        ask_recv((uint32_t)sizeof(corrupted), good_crc, false);
        raw_armed(true);
        feed(corrupted, (uint16_t)sizeof(corrupted));
    }

    done_count = 0U; fail_count = 0U;
    for ( ;; )
    {
        got = ao_spy_take();
        if ( NULL == got )
        {
            break;
        }
        if ( (int)got->sig == RECV_DONE_SIG )
        {
            ++done_count;
        }
        if ( (int)got->sig == RECV_FAIL_SIG )
        {
            ++fail_count;
        }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, done_count,
        "a corrupted transfer must never be installed");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fail_count, "...and must say so");
}

/**
 * @brief A host that goes quiet mid-transfer is a stall, not a hang - and a
 *        stall must take raw mode down with it.
 *
 * Nothing after the ready handshake ever sends a byte. Sweeping past
 * APPMGR_RECV_IDLE_DEADLINE_TICKS is what a genuinely dead USB link or a
 * host process that vanished looks like from AO_AppMgr's side, and the two
 * things that have to be true afterward are the two things a wedged console
 * would get wrong: the verdict is RECV_STALL_SIG specifically (kcli error 6,
 * never a plain RECV_FAIL_SIG), and AO_Log is told CONSOLE_RAW_END_SIG so
 * the line editor comes back - the reader task may still be sitting in raw
 * mode waiting for bytes that are never coming, and a transfer that died
 * has to be the one to say so, because a transfer that finished on the wire
 * never needs to (the reader stops itself at the count).
 */
TEST_CASE("a stalled transfer reports RECV_STALL_SIG and takes raw mode down",
          "[appmgr_ao]")
{
    bool saw_stall;
    bool saw_done;
    bool saw_fail;
    bool saw_raw_end;
    const QEvt *got;

    recv_fresh();
    ao_spy_subscribe(RECV_STALL_SIG);
    ao_spy_subscribe(RECV_DONE_SIG);
    ao_spy_subscribe(RECV_FAIL_SIG);

    ask_recv(4096U, 0x00000000U, false);
    raw_armed(true);

    /* Not one RECV_DATA_SIG after this. */
    run_ticks(APPMGR_RECV_IDLE_DEADLINE_TICKS + 50U);

    saw_stall = false;
    saw_done  = false;
    saw_fail  = false;
    for ( ;; )
    {
        got = ao_spy_take();
        if ( NULL == got )
        {
            break;
        }
        if ( (int)got->sig == RECV_STALL_SIG )
        {
            saw_stall = true;
        }
        if ( (int)got->sig == RECV_DONE_SIG )
        {
            saw_done = true;
        }
        if ( (int)got->sig == RECV_FAIL_SIG )
        {
            saw_fail = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(saw_stall,
        "a host that goes silent must be reported as a stall (kcli error 6)");
    TEST_ASSERT_FALSE_MESSAGE(saw_done, "the transfer never finished");
    TEST_ASSERT_FALSE_MESSAGE(saw_fail,
        "a stall is its own verdict, not a plain failure");

    saw_raw_end = log_saw(CONSOLE_RAW_END_SIG);
    TEST_ASSERT_TRUE_MESSAGE(saw_raw_end,
        "a stalled transfer must abandon raw mode, or the console is left "
        "wedged with the line editor off");
}

/**
 * @brief An image arriving on the console reports its progress on the bus,
 *        as every other write does.
 *
 * APP_FLASH_PROGRESS_SIG is what raises and moves the Opening sheet and what
 * makes AO_Power pulse the LED blue. A terminal install writes the region
 * exactly as a medium's copy does, so it owes the same reports: 0 % with the
 * ready, then one per whole percent written, rising to 100, each saying it is
 * an app that stays installed (run=off), and with no file name - a stream has
 * none until it is installed, and the name of whatever this AO wrote before
 * must not end up on the card instead.
 *
 * 12388 bytes is three whole chunks and a 100-byte remainder, so the reports
 * land at 0, 33, 66, 99 and 100 %. The CRC is wrong on purpose, as in the ack
 * case above: the reports belong to the write, not to the verdict, and
 * staying off region_install() leaves no directory entry to put back.
 */
TEST_CASE("a terminal install reports its progress on the bus as it writes",
          "[appmgr_ao]")
{
    static uint8_t image[KODE_RECV_CHUNK];
    static const unsigned want[] = { 0U, 33U, 66U, 99U, 100U };
    uint32_t const total = (3U * (uint32_t)KODE_RECV_CHUNK) + 100U;
    unsigned       reports = 0U;
    const QEvt    *got;

    image[0] = KFW_ESP_MAGIC;   /* a bare image: straight to the sink */
    image[12] = (uint8_t)ESP_CHIP_ID_ESP32P4;   /* its chip_id: this one */
    image[13] = 0U;

    /* Leave a file name behind, the way a launcher's write would: ask for
     * one, let the open go unanswered until the AO gives up on it. */
    Mockkdrv_slot_Init();
    recv_fresh();
    ask_flash_dst("stale.bin", (uint8_t)KFW_TARGET_APP);
    (void)storage_requests();
    run_ticks(APPMGR_OPEN_DEADLINE_TICKS + 50U);

    ao_spy_drain();
    ao_spy_subscribe(APP_FLASH_PROGRESS_SIG);

    ask_recv(total, 0xDEADBEEFU, false);
    raw_armed(true);
    feed(image, (uint16_t)KODE_RECV_CHUNK);
    feed(image, (uint16_t)KODE_RECV_CHUNK);
    feed(image, (uint16_t)KODE_RECV_CHUNK);
    feed(image, 100U);

    for ( ;; )
    {
        got = ao_spy_take();
        if ( NULL == got )
        {
            break;
        }
        if ( (int)got->sig != APP_FLASH_PROGRESS_SIG )
        {
            continue;
        }
        AppProgressEvt const * const p = (AppProgressEvt const *)got;
        TEST_ASSERT_LESS_THAN_UINT_MESSAGE(Q_DIM(want), reports,
            "more reports than whole percents moved");
        TEST_ASSERT_EQUAL_UINT_MESSAGE(want[reports], p->percent,
            "0 % with the ready, then each chunk's whole percent");
        TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)KWRITE_APP, p->what,
            "run=off: an app that stays installed, no restart");
        TEST_ASSERT_EQUAL_STRING_MESSAGE("", p->file,
            "a stream has no file name, and the last write's must not leak");
        ++reports;
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(Q_DIM(want), reports,
        "none at all leaves the card down and the LED dark");

    QActive_unsubscribe(ao_spy(), APP_FLASH_PROGRESS_SIG);
    ao_spy_drain();
    Mockkdrv_slot_Destroy();
}

/** @brief How many of @p sig the spy collected, draining it. */
static unsigned spy_count(enum_t sig)
{
    unsigned n = 0U;

    for ( ;; )
    {
        const QEvt * const got = ao_spy_take();
        if ( NULL == got )
        {
            break;
        }
        if ( (enum_t)got->sig == sig )
        {
            ++n;
        }
    }
    return n;
}

/* The glass and the LED follow APP_FLASH_* alone, so an install they saw
 * start - its 0 % went out with the ready - has to end there too: a CRC that
 * does not add up is RECV_FAIL_SIG for the console and APP_FLASH_FAIL_SIG
 * for the card. One refused before its ready was never on the glass, and
 * the console already has the reason; a failure card for it would be about
 * nothing the operator saw. */
TEST_CASE("an install the glass saw start ends there as a failure; one "
          "refused before its ready does not", "[appmgr_ao]")
{
    static uint8_t image[64];

    image[0] = KFW_ESP_MAGIC;
    image[12] = (uint8_t)ESP_CHIP_ID_ESP32P4;   /* its chip_id: this one */
    image[13] = 0U;

    recv_fresh();
    ao_spy_subscribe(APP_FLASH_FAIL_SIG);

    ask_recv((uint32_t)sizeof(image), 0xDEADBEEFU, false);   /* wrong CRC */
    raw_armed(true);
    feed(image, (uint16_t)sizeof(image));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, spy_count(APP_FLASH_FAIL_SIG),
        "announced, so the card is owed its failure");

    recv_fresh();
    ask_recv(0U, 0U, false);                                  /* refused */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, spy_count(APP_FLASH_FAIL_SIG),
        "refused before the ready: nothing on the glass to end");

    recv_fresh();
    ask_recv((uint32_t)sizeof(image), 0U, false);
    raw_armed(false);                                         /* no raw */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, spy_count(APP_FLASH_FAIL_SIG),
        "the console would not go raw: still nothing announced");

    QActive_unsubscribe(ao_spy(), APP_FLASH_FAIL_SIG);
    ao_spy_drain();
}

/* ---- install src="terminal" dst=os: the same wire, the kodeOS slot --------
 * The same conversation with another destination: the bytes go through the
 * decoder into kdrv_slot's sink instead of the region's, no
 * directory is touched, and the `>ok` is followed by the switch that boots
 * the new kodeOS. kdrv_slot is a mock in this binary, so the sink it hands
 * back is one of this file's own, and what it records is the assertion.
 */

static struct {
    unsigned begins;
    unsigned writes;
    unsigned ends;
    unsigned aborts;
    uint32_t img_len;
    size_t   bytes;
} s_slot_log;

static kode_status_t slot_fake_begin(void *ctx, uint32_t img_len)
{
    (void)ctx;
    ++s_slot_log.begins;
    s_slot_log.img_len = img_len;
    return KODE_OK;
}

static kode_status_t slot_fake_write(void *ctx, const void *data, size_t len)
{
    (void)ctx;
    (void)data;
    ++s_slot_log.writes;
    s_slot_log.bytes += len;
    return KODE_OK;
}

static kode_status_t slot_fake_end(void *ctx)
{
    (void)ctx;
    ++s_slot_log.ends;
    return KODE_OK;
}

static void slot_fake_abort(void *ctx)
{
    (void)ctx;
    ++s_slot_log.aborts;
}

static const kfw_sink_t SLOT_FAKE_SINK = {
    slot_fake_begin, slot_fake_write, slot_fake_end, slot_fake_abort
};

/** @brief The pause AppMgr_launching takes before the reset, in QP ticks -
 *  appmgr_ao.c's own (private) APPMGR_ANSWER_DRAIN_TICKS, repeated the way
 *  APPMGR_RECV_IDLE_DEADLINE_TICKS is above. */
#define APPMGR_ANSWER_DRAIN_DEADLINE_TICKS (5U)

/** @brief How many data-folder requests AO_Storage's queue holds, emptying it. */
static unsigned storage_folder_requests(void)
{
    const QEvt *e = NULL;
    unsigned    n = 0U;

    while ( xQueueReceive(AO_Storage->eQueue, &e, 0U) == pdTRUE )
    {
        if ( ((enum_t)e->sig == APPDATA_PUT_REQ_SIG)
            || ((enum_t)e->sig == APPDATA_RM_REQ_SIG) )
        {
            ++n;
        }
        QF_gc(e);
    }
    return n;
}

/** @brief Ask AO_AppMgr to receive a kodeOS image, exactly as a terminal
 *         install to `dst=os` does. */
static void ask_recv_os(uint32_t size, uint32_t crc32_val)
{
    AppRecvReqEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super  = (QEvt)QEVT_INITIALIZER(APP_RECV_REQ_SIG);
    e.dst    = (uint8_t)KFW_TARGET_OS;
    e.size   = size;
    e.crc32  = crc32_val;
    e.launch = false;
    e.src    = (uint8_t)KODE_CONSOLE_SRC_SERIAL;
    QASM_DISPATCH(AO_AppMgr, &e.super, 0U);
}

/** @brief Drain AO_Boot's queue, reporting whether @p sig was among what landed. */
static bool boot_saw(enum_t sig)
{
    const QEvt *e;
    bool        found = false;

    while ( xQueueReceive(AO_Boot->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (enum_t)e->sig == sig )
        {
            found = true;
        }
        QF_gc(e);
    }
    return found;
}

/** @brief How many RECV_DONE_SIG / RECV_FAIL_SIG the spy collected. */
static void count_verdicts(unsigned *done, unsigned *fail)
{
    const QEvt *got;

    *done = 0U;
    *fail = 0U;
    for ( ;; )
    {
        got = ao_spy_take();
        if ( NULL == got )
        {
            break;
        }
        if ( (int)got->sig == RECV_DONE_SIG )
        {
            ++*done;
        }
        if ( (int)got->sig == RECV_FAIL_SIG )
        {
            ++*fail;
        }
    }
}

TEST_CASE("install dst=os writes the inactive slot, touches no directory, and "
          "then asks AO_Boot to switch to it", "[appmgr_ao]")
{
    static uint8_t payload[96];
    kapps_dir_t    before;
    unsigned       done;
    unsigned       fail;

    for ( unsigned i = 0U; sizeof(payload) > i; ++i )
    {
        payload[i] = (uint8_t)((i * 5U) + 1U);
    }
    TEST_ASSERT_TRUE(sizeof(payload) >= CHAIN_MIN_LEN);
    image_chain(payload);         /* a bare image: straight to the sink */

    Mockkdrv_slot_Init();
    recv_fresh();
    (void)memset(&s_slot_log, 0, sizeof(s_slot_log));
    before = *kdrv_apps_dir();
    ao_spy_subscribe(RECV_READY_SIG);
    ao_spy_subscribe(RECV_DONE_SIG);
    ao_spy_subscribe(RECV_FAIL_SIG);

    kdrv_slot_present_ExpectAndReturn(true);
    kdrv_slot_size_IgnoreAndReturn(0x400000U);
    kdrv_slot_address_IgnoreAndReturn(0x420000U);   /* the placement's log line */
    kdrv_slot_sink_ExpectAndReturn(&SLOT_FAKE_SINK);
    kdrv_slot_desc_IgnoreAndReturn(KODE_ERROR);   /* the log line after the write */

    ask_recv_os((uint32_t)sizeof(payload),
                khal_crc32(KHAL_CRC32_INIT, payload, sizeof(payload)));
    TEST_ASSERT_TRUE_MESSAGE(log_saw(CONSOLE_RAW_REQ_SIG),
        "the console must be armed for the payload");
    raw_armed(true);
    feed(payload, (uint16_t)sizeof(payload));

    count_verdicts(&done, &fail);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, done, "a verified image is a result");
    TEST_ASSERT_EQUAL_UINT(0U, fail);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_folder_requests(),
        "an OS install asked AO_Storage about an app's folder");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_slot_log.begins, "the slot was opened");
    TEST_ASSERT_EQUAL_UINT(sizeof(payload), s_slot_log.bytes);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_slot_log.ends, "...and closed");
    TEST_ASSERT_EQUAL_UINT(0U, s_slot_log.aborts);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&before, kdrv_apps_dir(), sizeof(before),
        "a kodeOS image is not an app: the directory must not move");

    /* The answer drains first, then AO_Boot is asked - never the other way
     * round, or the `>ok` would die with the reset. */
    TEST_ASSERT_FALSE(boot_saw(BOOT_OS_SWITCH_SIG));
    run_ticks(APPMGR_ANSWER_DRAIN_DEADLINE_TICKS + 2U);
    TEST_ASSERT_TRUE_MESSAGE(boot_saw(BOOT_OS_SWITCH_SIG),
        "after the drain the switch to the new slot must be requested");

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("install dst=os with no kodeOS slot in the table is refused before "
          "the wire is touched", "[appmgr_ao]")
{
    unsigned done;
    unsigned fail;

    Mockkdrv_slot_Init();
    recv_fresh();
    ao_spy_subscribe(RECV_DONE_SIG);
    ao_spy_subscribe(RECV_FAIL_SIG);

    kdrv_slot_present_ExpectAndReturn(false);
    ask_recv_os(4096U, 0U);

    count_verdicts(&done, &fail);
    TEST_ASSERT_EQUAL_UINT(0U, done);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fail, "refused, and said so");
    TEST_ASSERT_FALSE_MESSAGE(log_saw(CONSOLE_RAW_REQ_SIG),
        "nothing was armed for a transfer that is not going to happen");

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("install dst=os of an image bigger than the slot is refused before "
          "the wire is touched", "[appmgr_ao]")
{
    unsigned done;
    unsigned fail;

    Mockkdrv_slot_Init();
    recv_fresh();
    ao_spy_subscribe(RECV_DONE_SIG);
    ao_spy_subscribe(RECV_FAIL_SIG);

    kdrv_slot_present_ExpectAndReturn(true);
    kdrv_slot_size_IgnoreAndReturn(0x400000U);
    ask_recv_os(0x400001U, 0U);

    count_verdicts(&done, &fail);
    TEST_ASSERT_EQUAL_UINT(0U, done);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fail, "refused, and said so");
    TEST_ASSERT_FALSE(log_saw(CONSOLE_RAW_REQ_SIG));

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* ---- install src="terminal" dst=pending: the same wire, the mailbox --------
 * The bytes skip the decoder and land in the mailbox as they came, the way
 * `install dst=pending` parks a file off a medium: no directory moves, nothing
 * boots, and the mailbox walks to the image that was sent. kdrv_apps is real
 * in this binary, so the mailbox it writes is the assertion.
 */
TEST_CASE("install from the terminal to the mailbox parks the image as it "
          "came, touches no directory, and boots nothing", "[appmgr_ao]")
{
    AppRecvReqEvt e;
    kapps_dir_t   before;
    uint32_t      parked = 0U;
    uint32_t      len;
    unsigned      done;
    unsigned      fail;
    bool          present;
    bool          booted;

    len = image_named("parked");
    recv_fresh();
    (void)boot_saw(BOOT_APP_RUN_SIG);   /* AO_Boot's queue starts empty */
    kdrv_apps_pending_write_abort();
    (void)kdrv_apps_pending_consume();
    before = *kdrv_apps_dir();
    ao_spy_subscribe(RECV_DONE_SIG);
    ao_spy_subscribe(RECV_FAIL_SIG);

    (void)memset(&e, 0, sizeof(e));
    e.super  = (QEvt)QEVT_INITIALIZER(APP_RECV_REQ_SIG);
    e.dst    = (uint8_t)KODE_FLASH_DST_PENDING;
    e.size   = len;
    e.crc32  = khal_crc32(KHAL_CRC32_INIT, s_named_img, len);
    e.launch = false;
    e.src    = (uint8_t)KODE_CONSOLE_SRC_SERIAL;
    QASM_DISPATCH(AO_AppMgr, &e.super, 0U);
    TEST_ASSERT_TRUE_MESSAGE(log_saw(CONSOLE_RAW_REQ_SIG),
        "the console must be armed for the payload");
    raw_armed(true);
    feed(s_named_img, (uint16_t)len);

    count_verdicts(&done, &fail);
    present = kdrv_apps_pending_present(&parked, NULL);
    run_ticks(APPMGR_ANSWER_DRAIN_DEADLINE_TICKS + 2U);
    booted = (uxQueueMessagesWaiting(AO_Boot->eQueue) > 0U);
    (void)boot_saw(BOOT_APP_RUN_SIG);
    (void)kdrv_apps_pending_consume();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, done, "a parked image is a result");
    TEST_ASSERT_EQUAL_UINT(0U, fail);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_folder_requests(),
        "a parked install asked AO_Storage about an app's folder");
    TEST_ASSERT_TRUE_MESSAGE(present, "the mailbox holds what was sent");
    TEST_ASSERT_EQUAL_UINT32(len, parked);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&before, kdrv_apps_dir(), sizeof(before),
        "a park is not an install: the directory must not move");
    TEST_ASSERT_FALSE_MESSAGE(booted, "the mailbox has no launch: AO_Boot was "
                                      "asked for nothing");
}

/* ---- The end of an install names what it installed ------------------------
 * APP_FLASH_DONE_SIG is an AppFlashDoneEvt: the app the write put in the
 * directory, by the name a run asks for, so a write asked for without a
 * launch can be run afterwards.
 */

/**
 * @brief Count the APP_FLASH_DONE_SIGs the spy collected, draining it, and
 *        keep a copy of the last one.
 *
 * @param last Overwritten by every done collected.
 * @return How many there were.
 */
static unsigned spy_done(AppFlashDoneEvt *last)
{
    unsigned n = 0U;

    for ( ;; )
    {
        const QEvt * const got = ao_spy_take();
        if ( NULL == got )
        {
            break;
        }
        if ( (enum_t)got->sig == APP_FLASH_DONE_SIG )
        {
            *last = *(AppFlashDoneEvt const *)got;
            ++n;
        }
    }
    return n;
}

/* The end of an install names what it installed, so whoever asked for it
 * without a launch can run it afterwards: the project name the written
 * image's own descriptor carries, which is what the directory is searched
 * by. run=off boots nothing - the name is the asker's to use - and a run
 * asked for by that name boots the extent just written.
 *
 * Everything is collected and the directory put back before the first
 * assertion: the suite is one binary, and a case that stopped half-way would
 * hand the next one its directory entry and its subscription. */
TEST_CASE("install dst=app run=off ends in a done that names the app, and "
          "boots nothing until it is run by that name", "[appmgr_ao]")
{
    AppFlashDoneEvt  done;
    AppFlashEvt      run;
    kapps_dir_t      saved;
    uint32_t         off  = 0U;
    uint32_t         size = 0U;
    kboot_run_mode_t mode = KBOOT_RUN_STICKY;
    uint32_t         len;
    unsigned         dones;
    bool             booted_by_install;
    bool             booted_by_name;

    len = image_named("notes");
    TEST_ASSERT_EQUAL_HEX8(KFW_ESP_MAGIC, s_named_img[0]);

    recv_fresh();
    (void)boot_run_request(&off, &size, &mode);   /* start from an empty queue */
    saved = *kdrv_apps_dir();
    ao_spy_subscribe(APP_FLASH_DONE_SIG);

    ask_recv(len, khal_crc32(KHAL_CRC32_INIT, s_named_img, len), false);
    raw_armed(true);
    feed(s_named_img, (uint16_t)len);

    (void)memset(&done, 0xA5, sizeof(done));
    dones = spy_done(&done);
    done.name[sizeof(done.name) - 1U] = '\0';   /* the fill, if none came */
    booted_by_install = boot_run_request(&off, &size, &mode);

    (void)memset(&run, 0, sizeof(run));
    run.super = (QEvt)QEVT_INITIALIZER(APP_RUN_REQ_SIG);
    (void)snprintf(run.file, sizeof(run.file), "%s", done.name);
    QASM_DISPATCH(AO_AppMgr, &run.super, 0U);
    booted_by_name = boot_run_request(&off, &size, &mode);

    QActive_unsubscribe(ao_spy(), APP_FLASH_DONE_SIG);
    ao_spy_drain();
    kode_status_t const restored = kdrv_apps_dir_commit(&saved);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, dones, "one install, one end");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("notes", done.name,
        "the end names the app its descriptor names");
    TEST_ASSERT_FALSE_MESSAGE(booted_by_install,
        "run=off boots nothing; the name is the asker's to run");
    TEST_ASSERT_TRUE_MESSAGE(booted_by_name,
        "the name in the end is not the one the directory is searched by");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(len, size,
        "the run booted something other than the extent just written");
    TEST_ASSERT_EQUAL_INT(KBOOT_RUN_ONCE, mode);
    TEST_ASSERT_EQUAL_INT(KODE_OK, restored);
}

/* A dry pool must not lose the end of a write: the Opening sheet and the LED
 * both end on it, and nothing else would. It still arrives - the static one,
 * the same struct under the same signal with an empty name - and the log
 * says what was lost.
 *
 * Every block the end could come from is taken just before the last byte
 * lands, so the end's own allocation is the one that fails, and given back
 * before anything is asserted: a case that stopped with the pool held would
 * starve every case after it. The hoard asks at the end's own size because
 * QP serves an event from the one pool its size picks: on kqp's four tiers,
 * smaller blocks would empty another pool and leave the end's untouched. */
TEST_CASE("an install's end that finds the pool dry still arrives, unnamed, "
          "and says so", "[appmgr_ao]")
{
    static QEvt     *hoard[96];
    AppFlashDoneEvt  done;
    kapps_dir_t      saved;
    unsigned         held = 0U;
    uint32_t         len;
    unsigned         dones;
    bool             said = false;

    len = image_named("dry");

    recv_fresh();
    saved = *kdrv_apps_dir();
    ao_spy_subscribe(APP_FLASH_DONE_SIG);

    ask_recv(len, khal_crc32(KHAL_CRC32_INIT, s_named_img, len), false);
    raw_armed(true);

    while ( Q_DIM(hoard) > held )
    {
        AppFlashDoneEvt * const e = Q_NEW_X(AppFlashDoneEvt, 0U, Q_USER_SIG);
        if ( NULL == e )
        {
            break;
        }
        hoard[held] = &e->super;
        ++held;
    }
    bool const dry = (Q_DIM(hoard) > held);

    feed(s_named_img, (uint16_t)len);

    for ( unsigned i = 0U; i < held; ++i )
    {
        QF_gc(hoard[i]);
    }

    (void)memset(&done, 0xA5, sizeof(done));
    dones = spy_done(&done);
    done.name[sizeof(done.name) - 1U] = '\0';   /* the fill, if none came */
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const line = fake_klog_line(i);
        if ( (NULL != line) && (strstr(line, "out of events") != NULL)
            && (strstr(line, "'dry'") != NULL) )
        {
            said = true;
        }
    }

    QActive_unsubscribe(ao_spy(), APP_FLASH_DONE_SIG);
    ao_spy_drain();
    kode_status_t const restored = kdrv_apps_dir_commit(&saved);

    TEST_ASSERT_TRUE_MESSAGE(dry,
        "the pool outlasted the hoard, so nothing here ran dry");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, dones,
        "a dry pool lost the end of the write");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", done.name,
        "the static end names nothing");
    TEST_ASSERT_TRUE_MESSAGE(said, "the name was lost without a word");
    TEST_ASSERT_EQUAL_INT(KODE_OK, restored);
}

/* ---- Keeping an app on the card before an install evicts it ----------------
 *
 * A full region used to evict the oldest app with a log line and nothing
 * else. Now AO_AppMgr stops the install at its plan, copies each victim that
 * is not already on the card to /sdcard/<project>.bin, and starts the same
 * install again from the top. AO_Storage is a queue with no task here, so
 * these cases are its voice: they read what AppMgr asked it and dispatch the
 * answers.
 *
 * Every case ticks - the pacer, the card write's heartbeat, the open's
 * deadline - and so they all stand where the timer list is safe, for the
 * reason the case after "an open nobody answers is abandoned" gives: AppMgr's
 * constructor runs again over a timer a case left armed, and the first tick
 * after that never comes back. Each case therefore ends with nothing armed,
 * either because the install ran to its end or because the open's deadline
 * ran out, and keep_end() ticks twice more so that a disarmed timer is
 * unlinked before the next constructor sees it.
 *
 * Eight of the ten share one picture: a region with one app, "old", that
 * fills it, so any install has to evict it. Nothing about a victim's size is
 * special - the host's flash and PSRAM are memory, and the whole 13.7 MB
 * copy runs in a fraction of a second.
 */

/** @brief The QP ticks between a heartbeat and the next, as appmgr_ao.c has it. */
#define APPMGR_KEEP_BEAT_TICKS (1000U / 10U)

/** @brief The directory a keep case replaced, and whether it is still out. */
static kapps_dir_t s_keep_dir;
static bool        s_keep_dir_held;

/** @brief Remember the directory as it is, before a case changes it. */
static void keep_hold_dir(void)
{
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_init());
    s_keep_dir      = *kdrv_apps_dir();
    s_keep_dir_held = true;
}

/**
 * @brief Put the remembered directory back.
 *
 * Called by every case when it ends and again by the next one when it
 * starts: an assertion that fails stops its case where it stands, and the
 * suite is one binary whose cases all read the same directory.
 */
static void keep_put_dir_back(void)
{
    if ( s_keep_dir_held )
    {
        s_keep_dir_held = false;
        kapps_dir_t restore = s_keep_dir;
        TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&restore));
    }
}

/**
 * @brief Give AO_Storage the buffers answer_data() copies into.
 *
 * They are allocated by Storage's initial transition, which a file that runs
 * before this one may never have run. Constructed and initialised the way
 * test_console_appdata.c does it, without registering it - test_storage_ao.c
 * registers it for itself, and a second registration is an assertion - and
 * with the two claims given back.
 */
static void storage_buffers(void)
{
    if ( storage_buf(0U) != NULL )
    {
        return;
    }
    qf_support_reap_timers();
    Storage_ctor();
    qf_support_give_queue(AO_Storage);
    AO_Storage->prio  = KODE_PRIO_STORAGE;
    AO_Storage->pthre = KODE_PRIO_STORAGE;
    qf_support_register(AO_Storage);
    QASM_INIT(AO_Storage, (void *)0, 0U);
    kbsp_release(KBSP_RES_SDMMC, AO_Storage);
    kbsp_release(KBSP_RES_FATFS, AO_Storage);
}

/** @brief The start of every keep case: a fresh AppMgr and a fresh Storage. */
static void keep_fresh(void)
{
    keep_put_dir_back();
    recv_fresh();
    storage_buffers();
    ao_spy_drain();
}

static void tidy_settle(void);

/** @brief The end of every keep case. */
static void keep_end(void)
{
    tidy_settle();
    run_ticks(2U);
    keep_put_dir_back();
    fake_psram_reset();
    Mockkdrv_slot_Destroy();
}

/**
 * @brief Pop AO_Storage's queue up to the first event of @p sig.
 * @return That event (the caller QF_gc()s it), or NULL when none was queued.
 */
static const QEvt *storage_take(enum_t sig)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_Storage->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (enum_t)e->sig == sig )
        {
            return e;
        }
        QF_gc(e);
    }
    return NULL;
}

/** @brief Take an event of @p sig out of AO_Storage's queue, or fail. */
static void storage_expect(enum_t sig, const char *what)
{
    const QEvt * const e = storage_take(sig);

    TEST_ASSERT_NOT_NULL_MESSAGE(e, what);
    QF_gc(e);
}

/** @brief What AO_Storage's queue held, by kind; see storage_tally(). */
typedef struct {
    unsigned open;    /**< STORAGE_OPEN_REQ_SIG   */
    unsigned path;    /**< STORAGE_PATH_REQ_SIG   */
    unsigned write;   /**< STORAGE_WRITE_REQ_SIG  */
} storage_seen_t;

/** @brief Count what AO_Storage's queue holds, and empty it. */
static storage_seen_t storage_tally(void)
{
    storage_seen_t seen = { 0U, 0U, 0U };
    const QEvt    *e    = NULL;

    while ( xQueueReceive(AO_Storage->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (enum_t)e->sig == STORAGE_OPEN_REQ_SIG )
        {
            ++seen.open;
        }
        if ( (enum_t)e->sig == STORAGE_PATH_REQ_SIG )
        {
            ++seen.path;
        }
        if ( (enum_t)e->sig == STORAGE_WRITE_REQ_SIG )
        {
            ++seen.write;
        }
        QF_gc(e);
    }
    return seen;
}

/**
 * @brief Tick until AO_Storage has been asked something of kind @p sig.
 * @param sig       The request to wait for.
 * @param max_ticks The most ticks to spend; a copy of the whole region takes
 *                  about 440 (one 32 KB step a tick).
 * @return The event (the caller QF_gc()s it), or NULL when none came.
 */
static const QEvt *storage_wait_for(enum_t sig, unsigned max_ticks)
{
    for ( unsigned i = 0U; i < max_ticks; ++i )
    {
        run_ticks(1U);
        const QEvt * const e = storage_take(sig);
        if ( NULL != e )
        {
            return e;
        }
    }
    return NULL;
}

/** @brief Did klog capture a line holding @p needle this case? */
static bool klog_has(const char *needle)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const line = fake_klog_line(i);
        if ( (NULL != line) && (strstr(line, needle) != NULL) )
        {
            return true;
        }
    }
    return false;
}

/** @brief Answer a path request of kind @p op the way AO_Storage would. */
static void answer_path(uint8_t op, uint8_t rc, bool exists, bool is_dir)
{
    StoragePathRspEvt r;

    (void)memset(&r, 0, sizeof(r));
    r.super  = (QEvt)QEVT_INITIALIZER(STORAGE_PATH_RSP_SIG);
    r.op     = op;
    r.rc     = rc;
    r.exists = exists;
    r.is_dir = is_dir;
    QASM_DISPATCH(AO_AppMgr, &r.super, 0U);
}

/** @brief Answer AppMgr's STAT: the path is there or it is not. */
static void answer_stat(bool exists, uint8_t rc)
{
    answer_path((uint8_t)STORAGE_PATH_STAT, rc, exists, false);
}

/** @brief Answer AppMgr's whole-buffer write. */
static void answer_write(uint8_t st, uint32_t len)
{
    StorageWriteRspEvt w;

    (void)memset(&w, 0, sizeof(w));
    w.super   = (QEvt)QEVT_INITIALIZER(STORAGE_WRITE_RSP_SIG);
    w.status  = st;
    w.written = len;
    w.medium  = (uint8_t)KDRV_MEDIUM_SD;
    QASM_DISPATCH(AO_AppMgr, &w.super, 0U);
}

/** @brief Answer the open of the install's source file. */
static void answer_open_with(uint32_t size, uint8_t medium, uint8_t st)
{
    StorageOpenRspEvt o;

    (void)memset(&o, 0, sizeof(o));
    o.super  = (QEvt)QEVT_INITIALIZER(STORAGE_OPEN_RSP_SIG);
    o.size   = size;
    o.status = st;
    o.medium = medium;
    QASM_DISPATCH(AO_AppMgr, &o.super, 0U);
}

/** @brief Answer the open: the file is there, @p size bytes, on @p medium. */
static void answer_open(uint32_t size, uint8_t medium)
{
    answer_open_with(size, medium, (uint8_t)KODE_OK);
}

/** @brief The most a medium's read hands over in one chunk: AO_Storage's
 *         STORAGE_CHUNK, which is the size of the buffer a chunk lies in. */
#define MEDIUM_CHUNK_MAX  (32U * 1024U)

/**
 * @brief Hand AppMgr @p len bytes of @p src, as chunks of a medium's size.
 *
 * Each chunk lies in AO_Storage's buffer 0 and is answered at once, the way a
 * read hands them over: a longer run is as many chunks as it takes.
 */
static void answer_data_bytes(const uint8_t *src, uint32_t len)
{
    uint8_t * const buf = (uint8_t *)storage_buf(0U);

    TEST_ASSERT_NOT_NULL(buf);
    for ( uint32_t off = 0U; off < len; off += MEDIUM_CHUNK_MAX )
    {
        StorageDataEvt d;
        uint32_t       n = len - off;

        if ( n > MEDIUM_CHUNK_MAX )
        {
            n = MEDIUM_CHUNK_MAX;
        }
        (void)memcpy(buf, &src[off], n);
        (void)memset(&d, 0, sizeof(d));
        d.super  = (QEvt)QEVT_INITIALIZER(STORAGE_DATA_SIG);
        d.len    = (uint16_t)n;
        d.idx    = 0U;
        d.status = (uint8_t)KODE_OK;
        QASM_DISPATCH(AO_AppMgr, &d.super, 0U);
    }
}

/** @brief Hand AppMgr the whole of image_named()'s image as one chunk. */
static void answer_data(uint32_t len)
{
    answer_data_bytes(s_named_img, len);
}

/** @brief The medium has nothing more to give: the end of the file. */
static void answer_eof(void)
{
    StorageDataEvt d;

    (void)memset(&d, 0, sizeof(d));
    d.super  = (QEvt)QEVT_INITIALIZER(STORAGE_DATA_SIG);
    d.status = (uint8_t)KODE_OK;
    d.eof    = true;
    QASM_DISPATCH(AO_AppMgr, &d.super, 0U);
}

/**
 * @brief Ask for "new.bin" and bring its header to AppMgr, which plans.
 *
 * With a victim to keep, that is where the install stops: the transfer is
 * abandoned and AO_Storage is asked about the victim's path.
 *
 * @param medium  Where the file claims to have been found.
 * @param project The project name the image carries.
 * @return The image's length.
 */
static uint32_t begin_install(uint8_t medium, const char *project)
{
    uint32_t const len = image_named(project);

    ask_flash_dst("new.bin", (uint8_t)KFW_TARGET_APP);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install never asked for its file");
    answer_open(len, medium);
    answer_data(len);
    return len;
}

/* ---- A park ends on the same events as an install --------------------------
 * The glass and the LED follow APP_FLASH_* alone: the 0 % that raises the
 * Opening card goes out for a park as for any write, so the end has to go out
 * with it. A park installs no app, so its done names none.
 */

/**
 * @brief Count the write events the spy collected, draining it.
 *
 * @param[out] progress How many APP_FLASH_PROGRESS_SIG.
 * @param[out] done     How many APP_FLASH_DONE_SIG.
 * @param[out] fail     How many APP_FLASH_FAIL_SIG.
 * @param[out] last     Overwritten by every done collected.
 */
static void spy_write_ends(uint32_t *progress, uint32_t *done, uint32_t *fail,
                           AppFlashDoneEvt *last)
{
    *progress = 0U;
    *done     = 0U;
    *fail     = 0U;
    for ( ;; )
    {
        const QEvt * const got = ao_spy_take();
        if ( NULL == got )
        {
            break;
        }
        if ( (enum_t)got->sig == APP_FLASH_PROGRESS_SIG )
        {
            ++*progress;
        }
        if ( (enum_t)got->sig == APP_FLASH_DONE_SIG )
        {
            *last = *(AppFlashDoneEvt const *)got;
            ++*done;
        }
        if ( (enum_t)got->sig == APP_FLASH_FAIL_SIG )
        {
            ++*fail;
        }
    }
}

/** @brief Listen for the write events a park publishes. */
static void spy_write_listen(void)
{
    ao_spy_drain();
    ao_spy_subscribe(APP_FLASH_PROGRESS_SIG);
    ao_spy_subscribe(APP_FLASH_DONE_SIG);
    ao_spy_subscribe(APP_FLASH_FAIL_SIG);
}

/** @brief Stop listening for the write events and empty the spy. */
static void spy_write_unlisten(void)
{
    QActive_unsubscribe(ao_spy(), APP_FLASH_PROGRESS_SIG);
    QActive_unsubscribe(ao_spy(), APP_FLASH_DONE_SIG);
    QActive_unsubscribe(ao_spy(), APP_FLASH_FAIL_SIG);
    ao_spy_drain();
}

/** @brief Ask AO_AppMgr to receive @p len bytes into the mailbox, as the console does. */
static void ask_recv_pending(uint32_t len, uint32_t crc32_val)
{
    AppRecvReqEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super  = (QEvt)QEVT_INITIALIZER(APP_RECV_REQ_SIG);
    e.dst    = (uint8_t)KODE_FLASH_DST_PENDING;
    e.size   = len;
    e.crc32  = crc32_val;
    e.launch = false;
    e.src    = (uint8_t)KODE_CONSOLE_SRC_SERIAL;
    QASM_DISPATCH(AO_AppMgr, &e.super, 0U);
}

TEST_CASE("a park from the terminal that lands ends in a done, so the card "
          "that rose for it closes", "[appmgr_ao]")
{
    AppFlashDoneEvt done;
    uint32_t        len;
    uint32_t        progress;
    uint32_t        dones;
    uint32_t        fails;

    len = image_named("parked");
    recv_fresh();
    kdrv_apps_pending_write_abort();
    (void)kdrv_apps_pending_consume();
    spy_write_listen();

    (void)memset(&done, 0xA5, sizeof(done));
    ask_recv_pending(len, khal_crc32(KHAL_CRC32_INIT, s_named_img, len));
    raw_armed(true);
    feed(s_named_img, (uint16_t)len);

    spy_write_ends(&progress, &dones, &fails, &done);
    spy_write_unlisten();
    (void)kdrv_apps_pending_consume();

    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0U, progress,
        "the park is on the glass: it announced its 0 %");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, dones,
        "a card the park raised is closed by a done, and only by one");
    TEST_ASSERT_EQUAL_UINT(0U, fails);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", done.name,
        "a park installs no app: its done names none");
}

TEST_CASE("a park from the terminal whose bytes do not add up ends in a "
          "failure and no done", "[appmgr_ao]")
{
    AppFlashDoneEvt done;
    uint32_t        len;
    uint32_t        progress;
    uint32_t        dones;
    uint32_t        fails;

    len = image_named("parked");
    recv_fresh();
    kdrv_apps_pending_write_abort();
    (void)kdrv_apps_pending_consume();
    spy_write_listen();

    ask_recv_pending(len, 0xDEADBEEFU);                       /* wrong CRC */
    raw_armed(true);
    feed(s_named_img, (uint16_t)len);

    spy_write_ends(&progress, &dones, &fails, &done);
    spy_write_unlisten();
    (void)kdrv_apps_pending_consume();

    TEST_ASSERT_EQUAL_UINT(0U, dones);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fails, "the card is owed its failure");
}

TEST_CASE("a park from a medium that lands ends in a done, so the card that "
          "rose for it closes", "[appmgr_ao]")
{
    AppFlashDoneEvt done;
    uint32_t        len;
    uint32_t        progress;
    uint32_t        dones;
    uint32_t        fails;

    Mockkdrv_slot_Init();
    len = image_named("parked");
    keep_fresh();
    kdrv_apps_pending_write_abort();
    (void)kdrv_apps_pending_consume();
    spy_write_listen();

    (void)memset(&done, 0xA5, sizeof(done));
    ask_flash_dst("parked.bin", (uint8_t)KODE_FLASH_DST_PENDING);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the park never asked for its file");
    answer_open(len, (uint8_t)KDRV_MEDIUM_FS);
    answer_data(len);
    (void)qf_support_pump(AO_AppMgr, 8U);
    answer_eof();

    spy_write_ends(&progress, &dones, &fails, &done);
    spy_write_unlisten();
    (void)kdrv_apps_pending_consume();
    keep_end();

    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0U, progress,
        "the park is on the glass: it announced its 0 %");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, dones,
        "a card the park raised is closed by a done, and only by one");
    TEST_ASSERT_EQUAL_UINT(0U, fails);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", done.name,
        "a park installs no app: its done names none");
}

TEST_CASE("a park from a medium that cannot be read ends in a failure and no "
          "done", "[appmgr_ao]")
{
    AppFlashDoneEvt done;
    StorageDataEvt  d;
    uint32_t        len;
    uint32_t        progress;
    uint32_t        dones;
    uint32_t        fails;

    Mockkdrv_slot_Init();
    len = image_named("parked");
    keep_fresh();
    kdrv_apps_pending_write_abort();
    (void)kdrv_apps_pending_consume();
    spy_write_listen();

    ask_flash_dst("parked.bin", (uint8_t)KODE_FLASH_DST_PENDING);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the park never asked for its file");
    answer_open(len, (uint8_t)KDRV_MEDIUM_FS);
    (void)memset(&d, 0, sizeof(d));
    d.super  = (QEvt)QEVT_INITIALIZER(STORAGE_DATA_SIG);
    d.status = (uint8_t)KODE_ERROR;
    QASM_DISPATCH(AO_AppMgr, &d.super, 0U);

    spy_write_ends(&progress, &dones, &fails, &done);
    spy_write_unlisten();
    (void)kdrv_apps_pending_consume();
    keep_end();

    TEST_ASSERT_EQUAL_UINT(0U, dones);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fails, "the card is owed its failure");
}

/** @brief The bytes "old" fills: the whole data area. */
static uint32_t old_bytes(void)
{
    return kdrv_apps_region_size() - KAPPS_DATA_OFF;
}

/**
 * @brief Make "old" the only app, @p size bytes at the start of the data area.
 *
 * Does not remember the directory it replaces: a case that needs the picture
 * twice has already asked keep_hold_dir() for the original.
 */
static void put_old(uint32_t size)
{
    kapps_dir_t dir = *kdrv_apps_dir();

    dir.count       = 1U;
    dir.e[0].offset = KAPPS_DATA_OFF;
    dir.e[0].size   = size;
    dir.e[0].seq    = 1U;
    dir.e[0].flags  = KAPPS_ENTRY_VALID;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));
    plant_app(KAPPS_DATA_OFF, "old");
}

/**
 * @brief A region holding one app, "old", that fills it: any install has to
 *        evict it. The directory as it was is remembered, for keep_end().
 */
static void region_full_of_old(void)
{
    keep_hold_dir();
    put_old(old_bytes());
}

/** @brief Is the entry with sequence number @p seq still in the directory? */
static bool seq_installed(uint32_t seq)
{
    kapps_dir_t const *d = kdrv_apps_dir();

    for ( uint32_t i = 0U; i < d->count; ++i )
    {
        if ( ((d->e[i].flags & KAPPS_ENTRY_VALID) != 0U) && (d->e[i].seq == seq) )
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Is "old" still in the directory?
 *
 * By its extent and not by its sequence number: once it is evicted the
 * directory is empty, and the install that took its place is numbered 1.
 */
static bool old_installed(void)
{
    kapps_dir_t const *d = kdrv_apps_dir();

    for ( uint32_t i = 0U; i < d->count; ++i )
    {
        if ( ((d->e[i].flags & KAPPS_ENTRY_VALID) != 0U)
            && (old_bytes() == d->e[i].size) )
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Let the restarted install's open run out of time, so nothing is left
 *        armed: the AO is idle again afterwards.
 */
static void let_the_open_expire(void)
{
    run_ticks(APPMGR_OPEN_DEADLINE_TICKS + 50U);
}

/* A write off a medium waits on the medium between chunks. A chunk lost on a
 * dry pool - the request for it, or the data event itself - used to leave
 * AppMgr_flashing writing forever, refusing every request as busy and holding
 * the reader. The same 30 s long-stop as the open now ends it as a failed
 * write, and gives the reader back. */
TEST_CASE("a write whose medium goes quiet is abandoned, not waited on forever",
          "[appmgr_ao]")
{
    Mockkdrv_slot_Init();
    keep_fresh();
    keep_hold_dir();
    kapps_dir_t empty = *kdrv_apps_dir();
    empty.count = 0U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&empty));
    ao_spy_subscribe(APP_FLASH_FAIL_SIG);

    (void)begin_install((uint8_t)KDRV_MEDIUM_FS, "new");   /* then silence */
    run_ticks(APPMGR_OPEN_DEADLINE_TICKS - 50U);
    TEST_ASSERT_NULL_MESSAGE(ao_spy_take(), "gave up before the deadline");

    run_ticks(100U);
    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the AO is still writing, and will be "
                                    "until a reset");
    TEST_ASSERT_EQUAL_INT(APP_FLASH_FAIL_SIG, (int)e->sig);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KFAIL_WRITE,
                            ((AppFlashFailEvt const *)e)->why);
    TEST_ASSERT_TRUE(klog_has("the medium stopped sending"));
    storage_expect(STORAGE_CLOSE_REQ_SIG, "the reader was never given back");
    TEST_ASSERT_FALSE_MESSAGE(kfw_busy(), "the decode session is still open");

    QActive_unsubscribe(ao_spy(), APP_FLASH_FAIL_SIG);
    ao_spy_drain();
    keep_end();
}

/* The whole round on the happy path: the plan wants "old" gone, it is not on
 * the card, so it is read out of the region and written there, and only then
 * does the install start again and evict it. */
TEST_CASE("a victim missing from the card is written there before it goes",
          "[appmgr_ao][keep]")
{
    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();
    (void)begin_install((uint8_t)KDRV_MEDIUM_FS, "new");   /* the plan defers */

    TEST_ASSERT_TRUE_MESSAGE(old_installed(), "nothing may be evicted yet");
    const QEvt *stat = storage_take(STORAGE_PATH_REQ_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(stat, "the victim's path was never asked about");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)STORAGE_PATH_STAT,
                            ((StoragePathEvt const *)stat)->op);
    TEST_ASSERT_EQUAL_STRING("/sdcard/old.bin", ((StoragePathEvt const *)stat)->path);
    QF_gc(stat);

    answer_stat(false, (uint8_t)KODE_OK);
    const QEvt *wr = storage_wait_for(STORAGE_WRITE_REQ_SIG, 1000U);   /* the read, 32 KB a tick */
    TEST_ASSERT_NOT_NULL_MESSAGE(wr, "the copy never reached AO_Storage");
    TEST_ASSERT_EQUAL_STRING("/sdcard/old.bin", ((StorageWriteEvt const *)wr)->name);
    TEST_ASSERT_EQUAL_UINT32(old_bytes(), ((StorageWriteEvt const *)wr)->len);
    QF_gc(wr);
    TEST_ASSERT_TRUE(old_installed());
    TEST_ASSERT_TRUE(klog_has("keeping 'old' on the card before it is evicted"));

    answer_write((uint8_t)KODE_OK, old_bytes());
    /* The restart is the same request, posted to itself. */
    (void)qf_support_pump(AO_AppMgr, 8U);
    storage_expect(STORAGE_OPEN_REQ_SIG,
                   "the install did not start again after the copy");
    TEST_ASSERT_TRUE(klog_has("'old' kept as /sdcard/old.bin"));

    let_the_open_expire();
    keep_end();
}

TEST_CASE("a victim already on the card is not copied again",
          "[appmgr_ao][keep]")
{
    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();
    (void)begin_install((uint8_t)KDRV_MEDIUM_FS, "new");
    storage_expect(STORAGE_PATH_REQ_SIG, "the victim's path was never asked about");

    answer_stat(true, (uint8_t)KODE_OK);
    run_ticks(50U);                           /* the restart, and then some */

    storage_seen_t const seen = storage_tally();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, seen.write,
        "a victim that is on the card was written there again");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, seen.open,
        "the install did not start again once every victim was kept");
    TEST_ASSERT_TRUE(klog_has("'old' is already on the card"));
    TEST_ASSERT_TRUE_MESSAGE(old_installed(), "the copy round evicts nothing");

    let_the_open_expire();
    keep_end();
}

/* The card is where the new image comes from, and a failed copy on a card can
 * poison every later read of it: so the install is called off with nothing
 * evicted, and the glass is told the card is what failed. */
TEST_CASE("a failed copy cancels an install read from the card, and evicts nothing",
          "[appmgr_ao][keep]")
{
    unsigned fails = 0U;
    uint8_t  why   = 0xFFU;

    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();
    ao_spy_subscribe(APP_FLASH_FAIL_SIG);
    (void)begin_install((uint8_t)KDRV_MEDIUM_SD, "new");
    storage_expect(STORAGE_PATH_REQ_SIG, "the victim's path was never asked about");

    answer_stat(false, (uint8_t)KODE_OK);
    const QEvt *wr = storage_wait_for(STORAGE_WRITE_REQ_SIG, 1000U);
    TEST_ASSERT_NOT_NULL_MESSAGE(wr, "the copy never reached AO_Storage");
    QF_gc(wr);
    answer_write((uint8_t)KODE_ERROR, 0U);
    run_ticks(4U);

    for ( const QEvt *e = ao_spy_take(); NULL != e; e = ao_spy_take() )
    {
        if ( (int)e->sig == APP_FLASH_FAIL_SIG )
        {
            ++fails;
            why = ((AppFlashFailEvt const *)e)->why;
        }
    }
    QActive_unsubscribe(ao_spy(), APP_FLASH_FAIL_SIG);
    ao_spy_drain();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fails, "one cancelled install, one failure");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KFAIL_KEEP, why,
        "the failure must say the card could not keep the app");
    TEST_ASSERT_TRUE_MESSAGE(old_installed(),
        "the cancelled install evicted the app it could not keep");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_tally().open,
        "the install started again after it was cancelled");
    keep_end();
}

/* From anywhere else, the card failing to keep a victim is no reason to
 * refuse the install the user asked for: it goes ahead, the victim is evicted
 * as it always was, and the log says it could not be kept. */
TEST_CASE("a failed copy does not stop an install from elsewhere",
          "[appmgr_ao][keep]")
{
    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();
    uint32_t const len = begin_install((uint8_t)KDRV_MEDIUM_FS, "new");
    storage_expect(STORAGE_PATH_REQ_SIG, "the victim's path was never asked about");

    answer_stat(false, (uint8_t)KODE_ERROR);  /* no card to look on */
    (void)qf_support_pump(AO_AppMgr, 8U);     /* the restart */
    storage_expect(STORAGE_OPEN_REQ_SIG,
                   "the install did not start again after the copy failed");
    TEST_ASSERT_TRUE_MESSAGE(old_installed(), "the second pass has not run yet");

    /* The second pass: the plan finds the round done, warns, and evicts. */
    answer_open(len, (uint8_t)KDRV_MEDIUM_FS);
    answer_data(len);
    TEST_ASSERT_TRUE(klog_has("evicting 'old' to make room; no copy kept"));
    TEST_ASSERT_FALSE_MESSAGE(old_installed(),
        "the install from elsewhere never went ahead");

    answer_eof();                             /* ends the write: nothing armed */
    run_ticks(4U);
    keep_end();
}

TEST_CASE("no memory for the copy is a failed copy", "[appmgr_ao][keep]")
{
    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();
    (void)begin_install((uint8_t)KDRV_MEDIUM_FS, "new");
    storage_expect(STORAGE_PATH_REQ_SIG, "the victim's path was never asked about");

    fake_psram_refuse(1U);                    /* the buffer for the extent */
    answer_stat(false, (uint8_t)KODE_OK);
    (void)qf_support_pump(AO_AppMgr, 8U);

    storage_seen_t const seen = storage_tally();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, seen.write,
        "there was no buffer, yet a write was asked for");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, seen.open,
        "a copy that failed for want of memory stopped the install");
    TEST_ASSERT_TRUE(old_installed());

    let_the_open_expire();
    keep_end();
}

/* A terminal install cannot pause - the host keeps sending - so it copies
 * nothing: the victim goes with the warning, as it did before the copies
 * existed. */
TEST_CASE("the console install evicts without copying", "[appmgr_ao][keep]")
{
    AppFlashDoneEvt done;
    uint32_t        len;

    Mockkdrv_slot_Init();
    keep_fresh();
    len = image_named("new");
    region_full_of_old();
    ao_spy_subscribe(APP_FLASH_DONE_SIG);

    ask_recv(len, khal_crc32(KHAL_CRC32_INIT, s_named_img, len), false);
    raw_armed(true);
    feed(s_named_img, (uint16_t)len);

    unsigned const early = spy_done(&done);
    tidy_settle();                            /* the victim's folder goes first */
    unsigned const dones = spy_done(&done);
    QActive_unsubscribe(ao_spy(), APP_FLASH_DONE_SIG);
    ao_spy_drain();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, early,
        "the install ended before the evicted app's folder was deleted");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_tally().path,
        "the console path asked AO_Storage about a victim it cannot wait for");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, dones, "the install did not complete");
    TEST_ASSERT_FALSE_MESSAGE(old_installed(), "the victim was not evicted");
    TEST_ASSERT_TRUE_MESSAGE(klog_has("evicting 'old' to make room; no copy kept"),
        "the victim went without a word");
    keep_end();
}

/** @brief How many live entries the directory holds. */
static uint32_t live_entries(void)
{
    kapps_dir_t const *d = kdrv_apps_dir();
    uint32_t           n = 0U;

    for ( uint32_t i = 0U; i < d->count; ++i )
    {
        if ( (d->e[i].flags & KAPPS_ENTRY_VALID) != 0U )
        {
            ++n;
        }
    }
    return n;
}

/**
 * @brief "old" (seq 1) in the region's first 64 KB and "mine" (seq 2) in the
 *        rest: no gap anywhere, so an install has to evict.
 */
static void region_of_old_and_mine(void)
{
    keep_hold_dir();
    kapps_dir_t dir = s_keep_dir;
    dir.count       = 2U;
    dir.e[0].offset = KAPPS_DATA_OFF;
    dir.e[0].size   = KAPPS_ALIGN;
    dir.e[0].seq    = 1U;
    dir.e[0].flags  = KAPPS_ENTRY_VALID;
    dir.e[1].offset = KAPPS_DATA_OFF + KAPPS_ALIGN;
    dir.e[1].size   = kdrv_apps_region_size() - dir.e[1].offset;
    dir.e[1].seq    = 2U;
    dir.e[1].flags  = KAPPS_ENTRY_VALID;
    plant_app(dir.e[0].offset, "old");
    plant_app(dir.e[1].offset, "mine");
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));
}

/** @brief Whether an app named @p project is in the directory. */
static bool named_installed(const char *project)
{
    kapps_dir_t const *d = kdrv_apps_dir();

    for ( uint32_t i = 0U; i < d->count; ++i )
    {
        kdrv_slot_desc_t desc;
        if ( ((d->e[i].flags & KAPPS_ENTRY_VALID) != 0U)
            && (kdrv_apps_desc(&d->e[i], &desc, NULL) == KODE_OK)
            && (strcmp(desc.project, project) == 0) )
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Stream "mine" again from the console, named or not, and count the
 *        APP_EVICTED_SIG it publishes.
 * @param name   The name= the host sends.
 * @param[out] last The last eviction published, when there was one.
 * @return How many were published.
 */
static unsigned reinstall_mine(const char *name, AppEvictedEvt *last)
{
    unsigned n = 0U;
    uint32_t const len = image_named("mine");

    region_of_old_and_mine();
    ao_spy_subscribe(APP_EVICTED_SIG);
    ask_recv_named(len, khal_crc32(KHAL_CRC32_INIT, s_named_img, len), false,
                   name);
    raw_armed(true);
    feed(s_named_img, (uint16_t)len);
    tidy_settle();
    for ( ;; )
    {
        const QEvt * const got = ao_spy_take();
        if ( NULL == got )
        {
            break;
        }
        if ( (enum_t)got->sig == APP_EVICTED_SIG )
        {
            *last = *(AppEvictedEvt const *)got;
            ++n;
        }
    }
    QActive_unsubscribe(ao_spy(), APP_EVICTED_SIG);
    ao_spy_drain();
    return n;
}

/* Without a name the board cannot tell the image is a new "mine": the oldest
 * app goes, and the install says so on the bus. */
TEST_CASE("an unnamed reinstall into a full region evicts the oldest app",
          "[appmgr_ao]")
{
    AppEvictedEvt last;

    Mockkdrv_slot_Init();
    keep_fresh();
    TEST_ASSERT_EQUAL_UINT(1U, reinstall_mine("", &last));
    TEST_ASSERT_EQUAL_STRING("old", last.name);
    TEST_ASSERT_FALSE(last.kept);
    TEST_ASSERT_FALSE(named_installed("old"));
    TEST_ASSERT_TRUE(named_installed("mine"));
    keep_end();
}

/* Named, the app's own old copy makes room and nobody else pays for it. */
TEST_CASE("a named reinstall into a full region gives up its own old copy",
          "[appmgr_ao]")
{
    AppEvictedEvt last;

    Mockkdrv_slot_Init();
    keep_fresh();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, reinstall_mine("mine", &last),
        "an eviction was published for the app being replaced");
    TEST_ASSERT_TRUE_MESSAGE(named_installed("old"),
        "the oldest app paid for the reinstall");
    TEST_ASSERT_TRUE(named_installed("mine"));
    TEST_ASSERT_EQUAL_UINT32(2U, live_entries());
    keep_end();
}


/* The victim here fills the region, so the image that then fails its CRC was
 * written over all of it: it is gone, and what is left is a directory without
 * the victim and without the image that never verified. A failed image can
 * never be left listed, or a boot request armed for it. */
TEST_CASE("an install that evicts and then fails its CRC lists nothing new",
          "[appmgr_ao][keep]")
{
    uint32_t len;

    Mockkdrv_slot_Init();
    keep_fresh();
    len = image_named("new");
    region_full_of_old();

    ask_recv(len, khal_crc32(KHAL_CRC32_INIT, s_named_img, len) ^ 1U, false);
    raw_armed(true);
    feed(s_named_img, (uint16_t)len);
    (void)qf_support_pump(AO_AppMgr, 8U);

    TEST_ASSERT_FALSE_MESSAGE(old_installed(),
        "an app the write overwrote came back");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, live_entries(),
        "an image that failed its CRC is listed");
    TEST_ASSERT_FALSE_MESSAGE(kdrv_apps_req_present(),
        "a failed install left a boot request behind");
    keep_end();
}

/* ---- An install that fails gives back what its write never reached --------
 * The plan takes the victims out of the directory before the first byte
 * arrives, but the region erases 64 KB as the data gets there: an install that
 * stops early has erased only the blocks it reached. The victims on blocks it
 * never touched are whole, and the failure puts their entries back.
 */

/** @brief The bytes of the image the restore cases stream: two blocks. */
#define UNDO_IMG_LEN  (2U * KAPPS_ALIGN)

/** @brief The restore cases' image: image_named()'s header, then zeros. */
static uint8_t s_undo_img[UNDO_IMG_LEN];

/**
 * @brief Five apps, one block each but the last, in install order.
 *
 * "v1" lies at the start of the data area, "k" one block on, then "v2", "v3"
 * and the filler "f" to the end of the region. Install order is v1, v2, v3,
 * k, f, so an install that needs two blocks evicts v1, v2 and v3, in that
 * order, and lands on the blocks of v2 and v3 - which leaves v1, a victim
 * that is never overwritten.
 */
static void region_of_victims(void)
{
    static const char *const name[5] = { "v1", "v2", "v3", "k", "f" };
    static const uint32_t    at[5]   = { 0U, 2U, 3U, 1U, 4U };   /* block of each */
    kapps_dir_t dir;

    if ( !s_keep_dir_held )
    {
        keep_hold_dir();
    }
    dir       = s_keep_dir;
    dir.count = 5U;
    for ( uint32_t i = 0U; 5U > i; ++i )
    {
        dir.e[i].offset = KAPPS_DATA_OFF + (at[i] * KAPPS_ALIGN);
        dir.e[i].size   = KAPPS_ALIGN;
        dir.e[i].seq    = i + 1U;
        dir.e[i].flags  = KAPPS_ENTRY_VALID;
        plant_app(dir.e[i].offset, name[i]);
    }
    dir.e[4].size = kdrv_apps_region_size() - dir.e[4].offset;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));
}

/**
 * @brief The start of a restore case: a fresh AO and Storage, and a spy that
 *        listens to nothing an earlier case asked it to.
 *
 * Thirty-two chunks put more on the bus than the spy's queue holds, and the
 * guarantee QP gives a subscriber is an assertion when it cannot keep it.
 */
static void undo_fresh(void)
{
    keep_fresh();
    QActive_unsubscribeAll(ao_spy());
    ao_spy_drain();
}

/** @brief Build the image the restore cases stream, named @p project. */
static void undo_image(const char *project)
{
    (void)image_named(project);
    (void)memset(s_undo_img, 0, sizeof(s_undo_img));
    (void)memcpy(s_undo_img, s_named_img, sizeof(s_named_img));
}

/** @brief Stream the first @p bytes of the restore image from the console, a chunk at a time. */
static void feed_undo_image(uint32_t bytes)
{
    for ( uint32_t off = 0U; off < bytes; off += KODE_RECV_CHUNK )
    {
        feed(&s_undo_img[off], (uint16_t)KODE_RECV_CHUNK);
    }
}

/** @brief Which of the five apps are installed, as a string like "v1 k f". */
static const char *victims_left(void)
{
    static char out[32];
    static const char *const name[5] = { "v1", "v2", "v3", "k", "f" };
    size_t n = 0U;

    out[0] = '\0';
    for ( unsigned i = 0U; 5U > i; ++i )
    {
        if ( named_installed(name[i]) )
        {
            if ( 0U < n )
            {
                out[n++] = ' ';
            }
            n += (size_t)snprintf(&out[n], sizeof(out) - n, "%s", name[i]);
        }
    }
    return out;
}

TEST_CASE("a terminal install that fails its CRC gives back the apps it did not reach",
          "[appmgr_ao][undo]")
{
    Mockkdrv_slot_Init();
    undo_fresh();
    region_of_victims();
    undo_image("new");

    ask_recv_named(UNDO_IMG_LEN, khal_crc32(KHAL_CRC32_INIT, s_undo_img, UNDO_IMG_LEN) ^ 1U,
                   false, "new");
    raw_armed(true);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("k f", victims_left(),
        "the plan did not take v1, v2 and v3 out");
    feed_undo_image(UNDO_IMG_LEN);
    (void)qf_support_pump(AO_AppMgr, 8U);

    TEST_ASSERT_EQUAL_STRING_MESSAGE("v1 k f", victims_left(),
        "the install's failure did not give back v1, whose blocks it never "
        "touched, or gave back what it overwrote");
    TEST_ASSERT_FALSE(named_installed("new"));
    TEST_ASSERT_TRUE(klog_has("kept 'v1' after the failed install"));
    TEST_ASSERT_TRUE(klog_has("'v2' was overwritten and is gone"));
    TEST_ASSERT_TRUE(klog_has("'v3' was overwritten and is gone"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_folder_requests(),
        "a failed install deleted or wrote a data folder");
    keep_end();
}

TEST_CASE("a terminal install cut by the watchdog gives back every app it did not reach",
          "[appmgr_ao][undo]")
{
    Mockkdrv_slot_Init();
    undo_fresh();
    region_of_victims();
    undo_image("new");

    ask_recv_named(UNDO_IMG_LEN, khal_crc32(KHAL_CRC32_INIT, s_undo_img, UNDO_IMG_LEN),
                   false, "new");
    raw_armed(true);
    feed_undo_image(KODE_RECV_CHUNK);          /* the first block is erased, no more */
    run_ticks(APPMGR_RECV_IDLE_DEADLINE_TICKS + 50U);

    TEST_ASSERT_TRUE_MESSAGE(klog_has("the host is presumed gone"),
        "the watchdog did not run");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("v1 v3 k f", victims_left(),
        "a cut install lost an app whose blocks it never reached");
    TEST_ASSERT_TRUE(klog_has("kept 'v1' after the failed install"));
    TEST_ASSERT_TRUE(klog_has("kept 'v3' after the failed install"));
    TEST_ASSERT_TRUE(klog_has("'v2' was overwritten and is gone"));
    (void)log_saw(CONSOLE_RAW_END_SIG);
    keep_end();
}

TEST_CASE("an install from a medium that fails part-way gives back the apps it did not reach",
          "[appmgr_ao][undo]")
{
    Mockkdrv_slot_Init();
    undo_fresh();
    region_of_victims();
    undo_image("new");

    ask_flash_dst("new.bin", (uint8_t)KFW_TARGET_APP);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install never asked for its file");
    answer_open(UNDO_IMG_LEN, (uint8_t)KDRV_MEDIUM_FS);
    answer_data_bytes(s_undo_img, sizeof(s_named_img));
    for ( unsigned victim = 0U; 3U > victim; ++victim )
    {
        storage_expect(STORAGE_PATH_REQ_SIG, "a victim's path was never asked about");
        answer_stat(false, (uint8_t)KODE_ERROR);     /* no card: nothing to copy */
        (void)qf_support_pump(AO_AppMgr, 8U);
    }
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install did not start again");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("v1 v2 v3 k f", victims_left(),
        "the round evicted before the restart");

    answer_open(UNDO_IMG_LEN, (uint8_t)KDRV_MEDIUM_FS);
    answer_data_bytes(s_undo_img, sizeof(s_named_img));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("k f", victims_left(),
        "the second pass did not take the victims out");
    answer_eof();                                     /* the file ended early */
    run_ticks(4U);

    TEST_ASSERT_TRUE(klog_has("the file ended early"));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("v1 v3 k f", victims_left(),
        "the failure did not give back what the write never reached");
    TEST_ASSERT_TRUE(klog_has("kept 'v1' after the failed install"));
    TEST_ASSERT_TRUE(klog_has("kept 'v3' after the failed install"));
    TEST_ASSERT_TRUE(klog_has("'v2' was overwritten and is gone"));
    keep_end();
}

TEST_CASE("an install that lands evicts its victims for good", "[appmgr_ao][undo]")
{
    Mockkdrv_slot_Init();
    undo_fresh();
    region_of_victims();
    undo_image("new");

    ask_recv_named(UNDO_IMG_LEN, khal_crc32(KHAL_CRC32_INIT, s_undo_img, UNDO_IMG_LEN),
                   false, "new");
    raw_armed(true);
    feed_undo_image(UNDO_IMG_LEN);
    tidy_settle();

    TEST_ASSERT_EQUAL_STRING_MESSAGE("k f", victims_left(),
        "an install that landed did not leave its victims evicted");
    TEST_ASSERT_TRUE(named_installed("new"));
    TEST_ASSERT_FALSE_MESSAGE(klog_has("after the failed install"),
        "an install that landed gave something back");
    keep_end();
}

TEST_CASE("a second failed install in a row gives back what it did not reach too",
          "[appmgr_ao][undo]")
{
    Mockkdrv_slot_Init();
    undo_fresh();
    undo_image("new");

    for ( unsigned run = 0U; 2U > run; ++run )
    {
        fake_klog_reset();
        region_of_victims();
        ask_recv_named(UNDO_IMG_LEN,
                       khal_crc32(KHAL_CRC32_INIT, s_undo_img, UNDO_IMG_LEN) ^ 1U,
                       false, "new");
        raw_armed(true);
        feed_undo_image(UNDO_IMG_LEN);
        (void)qf_support_pump(AO_AppMgr, 8U);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("v1 k f", victims_left(),
            "a failure in a row gave back the wrong apps");
        TEST_ASSERT_TRUE(klog_has("kept 'v1' after the failed install"));
    }
    keep_end();
}

/* ---- A request that asked for a verdict gets one ---------------------------
 * `install` and `app_run` wait for AO_AppMgr: `>ok` is "taken, and under way",
 * never "posted". The asker is the AO the request names, and the verdict is
 * posted to it alone.
 */

/** @brief Ask for an install of @p src, naming the spy as the asker. */
static void ask_install_from(uint8_t src_kind, const char *src, uint8_t dst_kind)
{
    FlashReqEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super    = (QEvt)QEVT_INITIALIZER(FLASH_REQ_SIG);
    e.src_kind = src_kind;
    e.dst_kind = dst_kind;
    e.launch   = false;
    e.who      = ao_spy();
    (void)snprintf(e.src, sizeof(e.src), "%s", src);
    QASM_DISPATCH(AO_AppMgr, &e.super, 0U);
}

/** @brief Ask to run the installed app @p name, naming the spy as the asker. */
static void ask_run_from(const char *name)
{
    AppFlashEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(APP_RUN_REQ_SIG);
    e.who   = ao_spy();
    (void)snprintf(e.file, sizeof(e.file), "%s", name);
    QASM_DISPATCH(AO_AppMgr, &e.super, 0U);
}

/** @brief The verdicts the spy was posted, oldest first; the spy is drained. */
typedef struct {
    unsigned count;                  /**< How many APP_REQ_RSP_SIG came.    */
    uint8_t  first;                  /**< The first one's verdict.          */
} verdicts_t;

/** @brief Count the verdicts the spy holds, and keep the first. */
static verdicts_t spy_verdicts(void)
{
    verdicts_t v = { 0U, 0xFFU };

    for ( ;; )
    {
        const QEvt * const got = ao_spy_take();

        if ( NULL == got )
        {
            break;
        }
        if ( (enum_t)got->sig == APP_REQ_RSP_SIG )
        {
            if ( 0U == v.count )
            {
                v.first = ((AppReqRspEvt const *)got)->verdict;
            }
            ++v.count;
        }
    }
    ao_spy_drain();
    return v;
}

TEST_CASE("an https install is taken the moment it starts opening, before the server answers",
          "[appmgr_ao][verdict]")
{
    Mockkdrv_slot_Init();
    undo_fresh();
    (void)net_requests(NET_STREAM_OPEN_REQ_SIG);

    ask_install_from((uint8_t)KFW_SRC_HTTP, "https://example.invalid/a.bin",
                     (uint8_t)KFW_TARGET_APP);

    verdicts_t const v = spy_verdicts();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, v.count, "an https install owes one verdict");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)APP_REQ_TAKEN, v.first);

    let_the_open_expire();
    (void)net_requests(NET_STREAM_CLOSE_REQ_SIG);
    keep_end();
}

TEST_CASE("an install from a medium is taken once the medium has opened the file",
          "[appmgr_ao][verdict]")
{
    Mockkdrv_slot_Init();
    undo_fresh();

    ask_install_from((uint8_t)KFW_SRC_SD, "a.bin", (uint8_t)KFW_TARGET_APP);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install never asked for its file");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, spy_verdicts().count,
        "the verdict came before the medium said the file is there");

    answer_open(512U, (uint8_t)KDRV_MEDIUM_SD);
    verdicts_t const v = spy_verdicts();
    TEST_ASSERT_EQUAL_UINT(1U, v.count);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)APP_REQ_TAKEN, v.first);

    answer_eof();
    run_ticks(4U);
    keep_end();
}

TEST_CASE("an install of a file that is not on the medium is missing",
          "[appmgr_ao][verdict]")
{
    Mockkdrv_slot_Init();
    undo_fresh();

    ask_install_from((uint8_t)KFW_SRC_SD, "nope.bin", (uint8_t)KFW_TARGET_APP);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install never asked for its file");
    answer_open_with(0U, (uint8_t)KDRV_MEDIUM_SD, (uint8_t)KODE_ERROR);

    verdicts_t const v = spy_verdicts();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, v.count, "a missing file owes one verdict");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)APP_REQ_MISSING, v.first);
    keep_end();
}

TEST_CASE("an install from an empty mailbox is empty, from a full one taken",
          "[appmgr_ao][verdict]")
{
    Mockkdrv_slot_Init();
    undo_fresh();
    (void)kdrv_apps_pending_consume();

    ask_install_from((uint8_t)KFW_SRC_PENDING, "", (uint8_t)KFW_TARGET_APP);
    verdicts_t v = spy_verdicts();
    TEST_ASSERT_EQUAL_UINT(1U, v.count);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)APP_REQ_EMPTY, v.first,
        "an empty mailbox was taken for an install");

    (void)park_pending("new");
    ask_install_from((uint8_t)KFW_SRC_PENDING, "", (uint8_t)KFW_TARGET_APP);
    v = spy_verdicts();
    TEST_ASSERT_EQUAL_UINT(1U, v.count);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)APP_REQ_TAKEN, v.first);

    (void)qf_support_pump(AO_AppMgr, 64U);
    (void)kdrv_apps_pending_consume();
    kfw_abort();
    keep_end();
}

TEST_CASE("an install or a run that arrives while an install is open is busy, and said",
          "[appmgr_ao][verdict]")
{
    Mockkdrv_slot_Init();
    undo_fresh();

    ask_install_from((uint8_t)KFW_SRC_SD, "a.bin", (uint8_t)KFW_TARGET_APP);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install never asked for its file");
    (void)spy_verdicts();

    fake_klog_reset();
    ask_install_from((uint8_t)KFW_SRC_SD, "b.bin", (uint8_t)KFW_TARGET_APP);
    verdicts_t v = spy_verdicts();
    TEST_ASSERT_EQUAL_UINT(1U, v.count);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)APP_REQ_BUSY, v.first,
        "an install that was not taken was not refused as busy");
    TEST_ASSERT_TRUE(klog_has("busy opening"));

    ask_run_from("anything");
    v = spy_verdicts();
    TEST_ASSERT_EQUAL_UINT(1U, v.count);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)APP_REQ_BUSY, v.first);

    let_the_open_expire();
    keep_end();
}

TEST_CASE("an install or a run that arrives while an image streams in is busy, and said",
          "[appmgr_ao][verdict]")
{
    Mockkdrv_slot_Init();
    undo_fresh();
    undo_image("new");

    ask_recv_named(UNDO_IMG_LEN, 0U, false, "new");
    raw_armed(true);
    fake_klog_reset();

    ask_install_from((uint8_t)KFW_SRC_SD, "b.bin", (uint8_t)KFW_TARGET_APP);
    verdicts_t v = spy_verdicts();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, v.count, "a request dropped with no answer");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)APP_REQ_BUSY, v.first);
    TEST_ASSERT_TRUE_MESSAGE(klog_has("busy receiving"),
        "the request was dropped without a word");

    ask_run_from("anything");
    v = spy_verdicts();
    TEST_ASSERT_EQUAL_UINT(1U, v.count);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)APP_REQ_BUSY, v.first);

    run_ticks(APPMGR_RECV_IDLE_DEADLINE_TICKS + 50U);
    (void)log_saw(CONSOLE_RAW_END_SIG);
    keep_end();
}

TEST_CASE("a terminal install that arrives while another transfer is open is refused, and said",
          "[appmgr_ao][verdict]")
{
    Mockkdrv_slot_Init();
    undo_fresh();
    ao_spy_subscribe(RECV_FAIL_SIG);

    ask_install_from((uint8_t)KFW_SRC_SD, "a.bin", (uint8_t)KFW_TARGET_APP);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install never asked for its file");
    (void)spy_verdicts();

    fake_klog_reset();
    ask_recv(4096U, 0U, false);
    TEST_ASSERT_TRUE_MESSAGE(klog_has("busy opening"),
        "a terminal install met a busy AO and nobody was told");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, spy_count(RECV_FAIL_SIG),
        "the console waiting for the install was left to its watchdog");

    QActive_unsubscribe(ao_spy(), RECV_FAIL_SIG);
    let_the_open_expire();
    keep_end();
}

TEST_CASE("running an app is taken when it is installed and missing when it is not",
          "[appmgr_ao][verdict]")
{
    uint32_t         off  = 0U;
    uint32_t         size = 0U;
    kboot_run_mode_t mode = KBOOT_RUN_STICKY;

    Mockkdrv_slot_Init();
    undo_fresh();
    keep_hold_dir();
    put_old(KAPPS_ALIGN);
    (void)boot_run_request(&off, &size, &mode);

    ask_run_from("nothing-here");
    verdicts_t v = spy_verdicts();
    TEST_ASSERT_EQUAL_UINT(1U, v.count);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)APP_REQ_MISSING, v.first,
        "an app that is not installed was taken for a run");
    TEST_ASSERT_FALSE(boot_run_request(&off, &size, &mode));

    ask_run_from("old");
    v = spy_verdicts();
    TEST_ASSERT_EQUAL_UINT(1U, v.count);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)APP_REQ_TAKEN, v.first);
    TEST_ASSERT_TRUE(boot_run_request(&off, &size, &mode));
    keep_end();
}

/* ---- A reset in the middle of an install gives the victims back ---------------
 * AO_AppMgr writes what an install evicts into NVS before it commits the
 * eviction - each victim's entry, name and the CRC-32 of its extent - and takes
 * it out when the install ends. A table found at start-up means a reset cut an
 * install. It leaves NVS first of all; then each victim the directory no
 * longer holds, and that nothing the directory holds now overlaps, is put back
 * if the CRC of its extent, folded a part at a time, is the one recorded.
 */

/** @brief The five apps of region_of_victims(), by index: where each lay is its block there. */
static const char *const s_victim_name[5] = { "v1", "v2", "v3", "k", "f" };

/** @brief The CRC-32 of the extent [offset, offset + size), as the driver folds it. */
static uint32_t extent_crc(uint32_t offset, uint32_t size)
{
    uint32_t crc = KHAL_CRC32_INIT;

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_extent_crc(offset, size, &crc));
    return crc;
}

/** @brief The record the table holds for app @p i of region_of_victims(), taken from the full directory as the flash holds it. */
static appmgr_undo_rec_t victim_rec(const kapps_dir_t *full, unsigned i)
{
    appmgr_undo_rec_t r;

    (void)memset(&r, 0, sizeof(r));
    r.offset = full->e[i].offset;
    r.size   = full->e[i].size;
    r.seq    = full->e[i].seq;
    r.flags  = full->e[i].flags;
    r.crc    = extent_crc(r.offset, r.size);
    (void)snprintf(r.project, sizeof(r.project), "%s", s_victim_name[i]);
    return r;
}

/** @brief Overwrite the start of the block at @p offset: what an install does to a victim it reaches. */
static void overwrite_extent(uint32_t offset)
{
    static const uint8_t other[16] = { 0x11U, 0x22U };

    kdrv_apps_write_abort();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write_begin(offset, sizeof(other)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write(other, sizeof(other)));
    kdrv_apps_write_abort();
}

/** @brief Store @p n records as AO_AppMgr's table, with the header @p version and @p count. */
static void seed_table_as(const void *rec, size_t rec_len, uint8_t n, uint8_t version,
                          uint8_t count)
{
    static uint8_t blob[APPMGR_UNDO_HDR_LEN + (APPMGR_UNDO_MAX * APPMGR_UNDO_REC_LEN)];
    appmgr_undo_hdr_t h;

    (void)memset(&h, 0, sizeof(h));
    h.version = version;
    h.count   = count;
    (void)memcpy(blob, &h, sizeof(h));
    (void)memcpy(&blob[sizeof(h)], rec, (size_t)n * rec_len);
    fake_nvs_seed(APPMGR_UNDO_NS, APPMGR_UNDO_KEY, blob, sizeof(h) + ((size_t)n * rec_len));
}

/** @brief Store @p n records as a table of this build's layout. */
static void seed_table(const appmgr_undo_rec_t *rec, uint8_t n)
{
    seed_table_as(rec, sizeof(*rec), n, (uint8_t)APPMGR_UNDO_VERSION, n);
}

/** @brief Is AO_AppMgr's table in NVS? */
static bool table_in_nvs(void)
{
    size_t len = 0U;

    return khal_nvs_get_blob(APPMGR_UNDO_NS, APPMGR_UNDO_KEY, NULL, &len) == KODE_OK;
}

/** @brief How many captured lines hold @p needle. */
static unsigned klog_count_of(const char *needle)
{
    unsigned n = 0U;

    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const line = fake_klog_line(i);

        if ( (NULL != line) && (strstr(line, needle) != NULL) )
        {
            ++n;
        }
    }
    return n;
}

/**
 * @brief The five apps laid out, the directory holding those in @p mask (bit i = app i).
 * @param full Receives the directory with all five, for the records' sake.
 */
static void region_holding(unsigned mask, kapps_dir_t *full)
{
    region_of_victims();
    *full = *kdrv_apps_dir();

    kapps_dir_t dir = *full;
    uint32_t    w   = 0U;

    for ( uint32_t i = 0U; i < full->count; ++i )
    {
        if ( 0U != (mask & (1U << i)) )
        {
            dir.e[w++] = full->e[i];
        }
    }
    dir.count = w;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));
}

/** @brief One pace of the recovery, in QP ticks, as appmgr_ao.c has it. */
#define APPMGR_UNDO_PACE_TEST_TICKS  (50U / 10U)

/** @brief The bytes of an image one step of the recovery folds into its CRC. */
#define APPMGR_UNDO_STEP_TEST_BYTES  (512U * 1024U)

/** @brief A start-up: the AO is made again over what NVS and the directory hold, with nothing run yet. */
static void start_over(void)
{
    (void)qf_support_pump(AO_AppMgr, 8U);
    appmgr_make();
    kfw_abort();
    kdrv_apps_write_abort();
}

/** @brief A start-up, run until the recovery has had every step it needs. */
static void start_up(void)
{
    start_over();
    run_ticks(20U * APPMGR_UNDO_PACE_TEST_TICKS);
}

/** @brief What the directory held at the moment NVS was written, and how many writes there were. */
static unsigned s_dir_count_at_write;
static unsigned s_writes_seen;

/** @brief Run inside every NVS write the fake is asked for. */
static void note_directory_at_write(void)
{
    s_dir_count_at_write = kdrv_apps_dir()->count;
    ++s_writes_seen;
}

TEST_CASE("an install that evicts records its victims and their CRCs in NVS before the "
          "eviction is committed", "[appmgr_ao][reset]")
{
    uint32_t want[3];

    Mockkdrv_slot_Init();
    undo_fresh();
    region_of_victims();
    undo_image("new");
    for ( uint32_t i = 0U; 3U > i; ++i )
    {
        want[i] = extent_crc(kdrv_apps_dir()->e[i].offset, kdrv_apps_dir()->e[i].size);
    }
    s_dir_count_at_write = 0U;
    s_writes_seen        = 0U;

    fake_nvs_set_hook(&note_directory_at_write);
    ask_recv_named(UNDO_IMG_LEN, khal_crc32(KHAL_CRC32_INIT, s_undo_img, UNDO_IMG_LEN),
                   false, "new");
    raw_armed(true);
    fake_nvs_set_hook(NULL);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_writes_seen,
        "an install that evicts three apps wrote NVS more or less than once");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(5U, s_dir_count_at_write,
        "the table was written after the directory lost its victims");
    uint8_t blob[APPMGR_UNDO_HDR_LEN + (APPMGR_UNDO_MAX * APPMGR_UNDO_REC_LEN)];
    size_t  len = sizeof(blob);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_nvs_get_blob(APPMGR_UNDO_NS, APPMGR_UNDO_KEY, blob, &len));
    TEST_ASSERT_EQUAL_UINT(APPMGR_UNDO_HDR_LEN + (3U * APPMGR_UNDO_REC_LEN), len);
    appmgr_undo_hdr_t hdr;
    (void)memcpy(&hdr, blob, sizeof(hdr));
    TEST_ASSERT_EQUAL_UINT8(2U, hdr.version);
    TEST_ASSERT_EQUAL_UINT8(3U, hdr.count);
    appmgr_undo_rec_t rec[3];
    (void)memcpy(rec, &blob[sizeof(hdr)], sizeof(rec));
    TEST_ASSERT_EQUAL_STRING("v1", rec[0].project);
    TEST_ASSERT_EQUAL_STRING("v2", rec[1].project);
    TEST_ASSERT_EQUAL_STRING("v3", rec[2].project);
    TEST_ASSERT_EQUAL_UINT32(2U, rec[1].seq);
    TEST_ASSERT_EQUAL_UINT32(KAPPS_DATA_OFF + (2U * KAPPS_ALIGN), rec[1].offset);
    TEST_ASSERT_EQUAL_UINT32(KAPPS_ALIGN, rec[1].size);
    for ( uint32_t i = 0U; 3U > i; ++i )
    {
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(want[i], rec[i].crc,
            "a record carries a CRC that is not its extent's before the eviction");
    }

    run_ticks(APPMGR_RECV_IDLE_DEADLINE_TICKS + 50U);      /* the host never sends */
    (void)log_saw(CONSOLE_RAW_END_SIG);
    keep_end();
}

TEST_CASE("a landed install takes its table out of NVS, a failed one after its restore",
          "[appmgr_ao][reset]")
{
    Mockkdrv_slot_Init();
    undo_fresh();
    region_of_victims();
    undo_image("new");

    ask_recv_named(UNDO_IMG_LEN, khal_crc32(KHAL_CRC32_INIT, s_undo_img, UNDO_IMG_LEN) ^ 1U,
                   false, "new");
    raw_armed(true);
    TEST_ASSERT_TRUE(table_in_nvs());
    feed_undo_image(UNDO_IMG_LEN);                         /* the CRC fails at the end */
    (void)qf_support_pump(AO_AppMgr, 8U);
    TEST_ASSERT_EQUAL_STRING("v1 k f", victims_left());
    TEST_ASSERT_FALSE_MESSAGE(table_in_nvs(),
        "a failed install left its table in NVS after giving the apps back");

    region_of_victims();
    ask_recv_named(UNDO_IMG_LEN, khal_crc32(KHAL_CRC32_INIT, s_undo_img, UNDO_IMG_LEN),
                   false, "new");
    raw_armed(true);
    TEST_ASSERT_TRUE(table_in_nvs());
    feed_undo_image(UNDO_IMG_LEN);
    tidy_settle();
    TEST_ASSERT_TRUE(named_installed("new"));
    TEST_ASSERT_FALSE_MESSAGE(table_in_nvs(),
        "a landed install left its table in NVS");
    keep_end();
}

TEST_CASE("an install that evicts nothing writes nothing to NVS, and one whose table "
          "cannot be written goes on", "[appmgr_ao][reset]")
{
    uint32_t const len = image_named("new");

    Mockkdrv_slot_Init();
    undo_fresh();
    keep_hold_dir();
    kapps_dir_t empty = *kdrv_apps_dir();
    empty.count = 0U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&empty));

    ask_recv_named(len, khal_crc32(KHAL_CRC32_INIT, s_named_img, len), false, "new");
    raw_armed(true);
    feed(s_named_img, (uint16_t)len);
    tidy_settle();
    TEST_ASSERT_TRUE(named_installed("new"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_nvs_set_count(),
        "an install with no victim wrote NVS");
    TEST_ASSERT_FALSE(table_in_nvs());

    region_of_victims();
    undo_image("new2");
    fake_nvs_fail_next_set();
    ask_recv_named(UNDO_IMG_LEN, khal_crc32(KHAL_CRC32_INIT, s_undo_img, UNDO_IMG_LEN),
                   false, "new2");
    raw_armed(true);
    TEST_ASSERT_TRUE_MESSAGE(klog_has("cannot record the apps this install evicts"),
        "a table that could not be written went unsaid");
    feed_undo_image(UNDO_IMG_LEN);
    tidy_settle();
    TEST_ASSERT_TRUE_MESSAGE(named_installed("new2"),
        "an install stopped because its table could not be written");
    keep_end();
}

TEST_CASE("a start-up takes the table out of NVS before it checks anything",
          "[appmgr_ao][reset]")
{
    kapps_dir_t full;

    Mockkdrv_slot_Init();
    undo_fresh();
    region_holding((1U << 3) | (1U << 4), &full);
    appmgr_undo_rec_t const rec[2] = { victim_rec(&full, 0U), victim_rec(&full, 2U) };
    seed_table(rec, 2U);
    TEST_ASSERT_TRUE(table_in_nvs());

    start_over();                                           /* the initial transition and no step */

    TEST_ASSERT_FALSE_MESSAGE(table_in_nvs(),
        "the table was still in NVS when the checking began: a reset in it would loop the boot");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("k f", victims_left(),
        "the initial transition did the recovery's work");
    TEST_ASSERT_FALSE(klog_has("kept 'v1'"));

    run_ticks(20U * APPMGR_UNDO_PACE_TEST_TICKS);
    TEST_ASSERT_EQUAL_STRING("v1 v3 k f", victims_left());
    TEST_ASSERT_FALSE(table_in_nvs());
    keep_end();
}

TEST_CASE("a start-up that finds a table puts back the victims whose extent still has "
          "its CRC and says which changed", "[appmgr_ao][reset]")
{
    kapps_dir_t full;

    Mockkdrv_slot_Init();
    undo_fresh();
    region_holding((1U << 3) | (1U << 4), &full);           /* k and f: v1, v2, v3 were evicted */
    appmgr_undo_rec_t const rec[3] = { victim_rec(&full, 0U), victim_rec(&full, 1U),
                                       victim_rec(&full, 2U) };
    seed_table(rec, 3U);
    overwrite_extent(full.e[1].offset);                     /* the install reached v2 */

    start_up();

    TEST_ASSERT_EQUAL_STRING("v1 v3 k f", victims_left());
    TEST_ASSERT_TRUE(klog_has("kept 'v1' after an install a reset cut"));
    TEST_ASSERT_TRUE(klog_has("kept 'v3' after an install a reset cut"));
    TEST_ASSERT_TRUE(klog_has("'v2' was overwritten by an install a reset cut and is gone"));
    TEST_ASSERT_FALSE_MESSAGE(table_in_nvs(), "the table is back in NVS");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_folder_requests(),
        "a recovered app's data folder was touched");
    keep_end();
}

TEST_CASE("a victim a reset left half written is told from one it left whole by its CRC "
          "alone", "[appmgr_ao][reset]")
{
    kapps_dir_t full;

    Mockkdrv_slot_Init();
    undo_fresh();
    region_holding((1U << 3) | (1U << 4), &full);
    appmgr_undo_rec_t rec[2] = { victim_rec(&full, 0U), victim_rec(&full, 2U) };
    rec[1].crc ^= 1U;                                       /* v3's bytes are not the recorded ones */
    seed_table(rec, 2U);

    start_up();

    TEST_ASSERT_EQUAL_STRING("v1 k f", victims_left());
    TEST_ASSERT_TRUE(klog_has("'v3' was overwritten by an install a reset cut and is gone"));
    keep_end();
}

TEST_CASE("a start-up whose table names only apps the directory still holds changes "
          "nothing and erases it", "[appmgr_ao][reset]")
{
    kapps_dir_t full;

    Mockkdrv_slot_Init();
    undo_fresh();
    region_holding(0x1FU, &full);
    uint32_t const gen = kdrv_apps_dir()->gen;
    appmgr_undo_rec_t const rec[2] = { victim_rec(&full, 0U), victim_rec(&full, 1U) };
    seed_table(rec, 2U);

    start_up();

    TEST_ASSERT_EQUAL_STRING("v1 v2 v3 k f", victims_left());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(gen, kdrv_apps_dir()->gen,
        "the directory was committed with nothing to put back");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, klog_count_of("checking '"),
        "an app the directory holds was checked");
    TEST_ASSERT_FALSE(table_in_nvs());
    keep_end();
}

TEST_CASE("a victim whose extent now overlaps an entry the directory holds is not put back",
          "[appmgr_ao][reset]")
{
    kapps_dir_t full;

    Mockkdrv_slot_Init();
    undo_fresh();
    region_holding((1U << 3) | (1U << 4), &full);
    appmgr_undo_rec_t const rec[3] = { victim_rec(&full, 0U), victim_rec(&full, 1U),
                                       victim_rec(&full, 2U) };
    /* The install that was cut landed after all: its entry lies over v2. */
    kapps_dir_t dir = *kdrv_apps_dir();
    dir.e[dir.count].offset = full.e[1].offset;
    dir.e[dir.count].size   = 2U * KAPPS_ALIGN;
    dir.e[dir.count].seq    = 9U;
    dir.e[dir.count].flags  = KAPPS_ENTRY_VALID;
    ++dir.count;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));
    seed_table(rec, 3U);

    start_up();

    /* The entry over v2's blocks reads as "v2": its image is the one planted there. */
    TEST_ASSERT_EQUAL_STRING("v1 v2 k f", victims_left());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(4U, kdrv_apps_dir()->count,
        "v2 or v3 came back over the entry that overwrote them");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, klog_count_of("checking '"),
        "an app lying under another entry was checked, or the free one was not");
    TEST_ASSERT_TRUE(klog_has("'v2' was overwritten by an install a reset cut and is gone"));
    TEST_ASSERT_TRUE(klog_has("'v3' was overwritten by an install a reset cut and is gone"));
    TEST_ASSERT_TRUE(klog_has("kept 'v1' after an install a reset cut"));
    keep_end();
}

TEST_CASE("a table of another version or size is ignored and erased, with a warning; "
          "the first layout's is one", "[appmgr_ao][reset]")
{
    struct {
        uint32_t offset, size, seq, flags;
        char     project[KODE_APP_PROJECT_MAX];
    } old_rec[1];
    kapps_dir_t full;

    Mockkdrv_slot_Init();
    undo_fresh();
    region_holding((1U << 3) | (1U << 4), &full);
    appmgr_undo_rec_t const rec[1] = { victim_rec(&full, 0U) };

    seed_table_as(rec, sizeof(rec[0]), 1U, (uint8_t)APPMGR_UNDO_VERSION + 1U, 1U);
    start_up();
    TEST_ASSERT_EQUAL_STRING("k f", victims_left());
    TEST_ASSERT_TRUE_MESSAGE(klog_has("not this build's"), "a table of another version went unsaid");
    TEST_ASSERT_FALSE(table_in_nvs());

    fake_klog_reset();
    seed_table_as(rec, sizeof(rec[0]), 1U, (uint8_t)APPMGR_UNDO_VERSION, 2U);   /* a count the bytes do not carry */
    start_up();
    TEST_ASSERT_EQUAL_STRING("k f", victims_left());
    TEST_ASSERT_TRUE(klog_has("not this build's"));
    TEST_ASSERT_FALSE(table_in_nvs());

    /* Version 1: records of 48 bytes, no digest. Nothing in them can be checked. */
    fake_klog_reset();
    (void)memset(old_rec, 0, sizeof(old_rec));
    old_rec[0].offset = rec[0].offset;
    old_rec[0].size   = rec[0].size;
    old_rec[0].seq    = rec[0].seq;
    old_rec[0].flags  = rec[0].flags;
    (void)snprintf(old_rec[0].project, sizeof(old_rec[0].project), "%s", "v1");
    seed_table_as(old_rec, sizeof(old_rec[0]), 1U, 1U, 1U);
    start_up();
    TEST_ASSERT_EQUAL_STRING("k f", victims_left());
    TEST_ASSERT_TRUE(klog_has("not this build's"));
    TEST_ASSERT_FALSE(table_in_nvs());
    keep_end();
}

TEST_CASE("a start-up with no table does no work and writes nothing", "[appmgr_ao][reset]")
{
    kapps_dir_t full;

    Mockkdrv_slot_Init();
    undo_fresh();
    region_holding((1U << 3) | (1U << 4), &full);
    uint32_t const gen = kdrv_apps_dir()->gen;

    start_up();

    TEST_ASSERT_EQUAL_STRING("k f", victims_left());
    TEST_ASSERT_EQUAL_UINT32(gen, kdrv_apps_dir()->gen);
    TEST_ASSERT_EQUAL_UINT(0U, klog_count_of("checking '"));
    TEST_ASSERT_EQUAL_UINT(0U, fake_nvs_set_count());
    TEST_ASSERT_FALSE(klog_has("a reset cut"));
    keep_end();
}

TEST_CASE("the recovery goes one paced step at a time, an extent in parts", "[appmgr_ao][reset]")
{
    uint32_t const big = 20U * KAPPS_ALIGN;                 /* 1.25 MB: three steps of 512 KB */

    Mockkdrv_slot_Init();
    undo_fresh();
    keep_hold_dir();
    kapps_dir_t empty = *kdrv_apps_dir();
    empty.count = 0U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&empty));
    plant_app(KAPPS_DATA_OFF, "big");
    appmgr_undo_rec_t rec;
    (void)memset(&rec, 0, sizeof(rec));
    rec.offset = KAPPS_DATA_OFF;
    rec.size   = big;
    rec.seq    = 1U;
    rec.flags  = KAPPS_ENTRY_VALID;
    rec.crc    = extent_crc(rec.offset, rec.size);
    (void)snprintf(rec.project, sizeof(rec.project), "%s", "big");
    seed_table(&rec, 1U);

    start_over();
    run_ticks(APPMGR_UNDO_PACE_TEST_TICKS - 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, klog_count_of("checking '"),
        "a step ran before the timer's period");
    run_ticks(1U);
    TEST_ASSERT_EQUAL_UINT(1U, klog_count_of("checking 'big': 524288 bytes from 0"));
    run_ticks(APPMGR_UNDO_PACE_TEST_TICKS - 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, klog_count_of("checking '"),
        "a step followed another without waiting for the timer");
    run_ticks(1U);
    TEST_ASSERT_EQUAL_UINT(2U, klog_count_of("checking '"));
    TEST_ASSERT_TRUE(klog_has("checking 'big': 524288 bytes from 524288"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, klog_count_of("kept 'big'"),
        "the app was put back before its last part was read");

    run_ticks(APPMGR_UNDO_PACE_TEST_TICKS);                 /* the third part: 262144 bytes */
    TEST_ASSERT_TRUE(klog_has("checking 'big': 262144 bytes from 1048576"));
    TEST_ASSERT_EQUAL_UINT(3U, klog_count_of("checking '"));
    run_ticks(2U * APPMGR_UNDO_PACE_TEST_TICKS);            /* the end */
    TEST_ASSERT_TRUE(klog_has("kept 'big' after an install a reset cut"));
    TEST_ASSERT_TRUE(named_installed("big"));
    TEST_ASSERT_EQUAL_UINT(3U, klog_count_of("checking '"));
    keep_end();
}

TEST_CASE("a list asked during the recovery is answered with the recovered directory, "
          "and an install is told busy", "[appmgr_ao][reset]")
{
    kapps_dir_t full;

    Mockkdrv_slot_Init();
    undo_fresh();
    region_holding((1U << 3) | (1U << 4), &full);
    appmgr_undo_rec_t const rec[2] = { victim_rec(&full, 0U), victim_rec(&full, 2U) };
    seed_table(rec, 2U);

    start_over();                                           /* recovering: no step has run */
    AppListReqEvt list;
    (void)memset(&list, 0, sizeof(list));
    list.super = (QEvt)QEVT_INITIALIZER(APP_LIST_REQ_SIG);
    list.who   = ao_spy();
    QASM_DISPATCH(AO_AppMgr, &list.super, 0U);
    ask_install_from((uint8_t)KFW_SRC_SD, "a.bin", (uint8_t)KFW_TARGET_APP);

    unsigned entries = 0U;
    unsigned ends    = 0U;
    unsigned busy    = 0U;
    for ( const QEvt *e = ao_spy_take(); NULL != e; e = ao_spy_take() )
    {
        if ( (enum_t)e->sig == APP_ENTRY_SIG ) { ++entries; }
        if ( (enum_t)e->sig == APP_LIST_END_SIG ) { ++ends; }
        if ( ((enum_t)e->sig == APP_REQ_RSP_SIG)
            && ((uint8_t)APP_REQ_BUSY == ((AppReqRspEvt const *)e)->verdict) ) { ++busy; }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, entries,
        "a list was answered while the directory was about to change");
    TEST_ASSERT_EQUAL_UINT(0U, ends);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, busy, "an install was not told the AO is busy");
    ao_spy_drain();

    run_ticks(20U * APPMGR_UNDO_PACE_TEST_TICKS);          /* the recovery ends */
    for ( const QEvt *e = ao_spy_take(); NULL != e; e = ao_spy_take() )
    {
        if ( (enum_t)e->sig == APP_ENTRY_SIG ) { ++entries; }
        if ( (enum_t)e->sig == APP_LIST_END_SIG ) { ++ends; }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(4U, entries,
        "the deferred list did not show the recovered apps, in the directory's order");
    TEST_ASSERT_EQUAL_UINT(1U, ends);
    TEST_ASSERT_EQUAL_STRING("v1 v3 k f", victims_left());
    keep_end();
}

/* ---- Something shows from the first moment, and keeps showing -------------
 * The write card and the LED follow APP_FLASH_PROGRESS_SIG alone, and AO_Power
 * takes the LED back three seconds after the last one. A download spends
 * longer than that in DNS and TLS before its first byte, so the 0 % goes out
 * the moment the install is accepted and the percent is said again every
 * second while the network is waited on.
 */

/** @brief What the progress events the spy holds said, in order. */
typedef struct {
    unsigned count;                   /**< How many.                          */
    unsigned pct[8];                  /**< The first eight percents.          */
    AppProgressEvt first;             /**< The first, whole.                  */
} progress_seen_t;

/** @brief Count the APP_FLASH_PROGRESS_SIG the spy holds and keep what they said; drains the spy. */
static progress_seen_t spy_progress(void)
{
    progress_seen_t seen;

    (void)memset(&seen, 0, sizeof(seen));
    for ( ;; )
    {
        const QEvt * const got = ao_spy_take();

        if ( NULL == got )
        {
            break;
        }
        if ( (enum_t)got->sig == APP_FLASH_PROGRESS_SIG )
        {
            AppProgressEvt const * const p = (AppProgressEvt const *)got;

            if ( 0U == seen.count )
            {
                seen.first = *p;
            }
            if ( Q_DIM(seen.pct) > seen.count )
            {
                seen.pct[seen.count] = p->percent;
            }
            ++seen.count;
        }
    }
    ao_spy_drain();
    return seen;
}

/** @brief The gap AppMgr leaves between two chunks, in QP ticks. */
#define APPMGR_CHUNK_GAP_TEST_TICKS (10U / 10U)

/** @brief One second of waiting on the network, in QP ticks, as appmgr_ao.c has it. */
#define APPMGR_WAIT_BEAT_TICKS (1000U / 10U)

/** @brief Answer the open of the download in hand: @p size bytes, status @p st. */
static void answer_net_open(uint32_t size, uint8_t st)
{
    StorageOpenRspEvt o;

    (void)memset(&o, 0, sizeof(o));
    o.super  = (QEvt)QEVT_INITIALIZER(NET_STREAM_OPEN_RSP_SIG);
    o.size   = size;
    o.status = st;
    QASM_DISPATCH(AO_AppMgr, &o.super, 0U);
}

TEST_CASE("an https install shows its 0 % the moment it is accepted, with its label",
          "[appmgr_ao][beat]")
{
    Mockkdrv_slot_Init();
    undo_fresh();
    ao_spy_subscribe(APP_FLASH_PROGRESS_SIG);
    (void)net_requests(NET_STREAM_OPEN_REQ_SIG);

    ask_https("https://example.invalid/app.bin");

    progress_seen_t const seen = spy_progress();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, seen.count,
        "the card and the LED wait for the server to answer");
    TEST_ASSERT_EQUAL_UINT(0U, seen.pct[0]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KWRITE_APP, seen.first.what);
    TEST_ASSERT_TRUE_MESSAGE('\0' != seen.first.file[0],
        "the 0 % does not say what is being fetched");

    QActive_unsubscribe(ao_spy(), APP_FLASH_PROGRESS_SIG);
    let_the_open_expire();
    (void)net_requests(NET_STREAM_CLOSE_REQ_SIG);
    keep_end();
}

TEST_CASE("an https install keeps saying its percent every second while it opens, "
          "and stops when it is over", "[appmgr_ao][beat]")
{
    Mockkdrv_slot_Init();
    undo_fresh();
    ao_spy_subscribe(APP_FLASH_PROGRESS_SIG);
    (void)net_requests(NET_STREAM_OPEN_REQ_SIG);

    ask_https("https://example.invalid/app.bin");
    TEST_ASSERT_EQUAL_UINT(1U, spy_progress().count);

    run_ticks(APPMGR_WAIT_BEAT_TICKS - 5U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, spy_progress().count, "a beat came early");
    run_ticks(10U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, spy_progress().count,
        "no beat after a second of waiting for the server");
    run_ticks(APPMGR_WAIT_BEAT_TICKS);
    TEST_ASSERT_EQUAL_UINT(1U, spy_progress().count);

    answer_net_open(0U, (uint8_t)KODE_ERROR);          /* the server says no */
    (void)spy_progress();
    run_ticks(3U * APPMGR_WAIT_BEAT_TICKS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, spy_progress().count,
        "the beat went on after the install ended");

    QActive_unsubscribe(ao_spy(), APP_FLASH_PROGRESS_SIG);
    (void)net_requests(NET_STREAM_CLOSE_REQ_SIG);
    keep_end();
}

TEST_CASE("a download that stalls keeps its percent on the glass",
          "[appmgr_ao][beat]")
{
    Mockkdrv_slot_Init();
    undo_fresh();
    keep_hold_dir();
    kapps_dir_t empty = *kdrv_apps_dir();
    empty.count = 0U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&empty));
    ao_spy_subscribe(APP_FLASH_PROGRESS_SIG);
    (void)net_requests(NET_STREAM_OPEN_REQ_SIG);

    ask_https("https://example.invalid/app.bin");
    (void)spy_progress();
    answer_net_open(UNDO_IMG_LEN, (uint8_t)KODE_OK);   /* open, then silence */
    progress_seen_t seen = spy_progress();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, seen.count, "the write began and said nothing");

    run_ticks((2U * APPMGR_WAIT_BEAT_TICKS) + 10U);
    seen = spy_progress();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, seen.count,
        "a download with no bytes coming fell silent");
    TEST_ASSERT_EQUAL_UINT(0U, seen.pct[0]);

    QActive_unsubscribe(ao_spy(), APP_FLASH_PROGRESS_SIG);
    let_the_open_expire();
    (void)net_requests(NET_STREAM_CLOSE_REQ_SIG);
    keep_end();
}

TEST_CASE("a percent the pool had no room for is said again, not counted as said",
          "[appmgr_ao][beat]")
{
    static QEvt *hoard[96];
    unsigned     held = 0U;
    uint32_t const total = 400000U;
    uint32_t const one_percent = total / 100U;

    Mockkdrv_slot_Init();
    undo_fresh();
    keep_hold_dir();
    kapps_dir_t empty = *kdrv_apps_dir();
    empty.count = 0U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&empty));
    undo_image("new");

    ask_flash_dst("new.bin", (uint8_t)KFW_TARGET_APP);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install never asked for its file");
    answer_open(total, (uint8_t)KDRV_MEDIUM_FS);
    ao_spy_subscribe(APP_FLASH_PROGRESS_SIG);
    (void)spy_progress();

    while ( Q_DIM(hoard) > held )
    {
        AppProgressEvt * const e = Q_NEW_X(AppProgressEvt, 0U, Q_USER_SIG);

        if ( NULL == e )
        {
            break;
        }
        hoard[held] = &e->super;
        ++held;
    }
    TEST_ASSERT_TRUE_MESSAGE(Q_DIM(hoard) > held, "the pool outlasted the hoard");
    answer_data_bytes(s_undo_img, one_percent + 1U);   /* 1 %: no room to say so */
    for ( unsigned i = 0U; i < held; ++i )
    {
        QF_gc(hoard[i]);
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, spy_progress().count, "the pool was not dry");

    run_ticks(APPMGR_CHUNK_GAP_TEST_TICKS + 2U);       /* the chunk's buffer goes back */
    answer_data_bytes(&s_undo_img[one_percent + 1U], 16U);   /* still 1 % */
    progress_seen_t const seen = spy_progress();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, seen.count,
        "a percent that was dropped for want of an event was taken as said");
    TEST_ASSERT_EQUAL_UINT(1U, seen.pct[0]);

    QActive_unsubscribe(ao_spy(), APP_FLASH_PROGRESS_SIG);
    let_the_open_expire();
    keep_end();
}

/* ---- A slow link is not a dead one -----------------------------------------
 * The phone app waits 10 s for an ack before it sends a chunk again, so a
 * board that gave up after 5 s of silence lost installs the host was still
 * retrying; and a `>ok` still queued for the link when the board reset into
 * the new app never arrived, so the phone retried against a running app.
 */

/** @brief ask_recv_named(), from the port @p src (KODE_CONSOLE_SRC_*). */
static void ask_recv_from(uint8_t src, uint32_t size, uint32_t crc32_val,
                          bool launch, const char *name)
{
    AppRecvReqEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super  = (QEvt)QEVT_INITIALIZER(APP_RECV_REQ_SIG);
    e.dst    = (uint8_t)KFW_TARGET_APP;
    e.size   = size;
    e.crc32  = crc32_val;
    e.launch = launch;
    e.src    = src;
    (void)snprintf(e.name, sizeof(e.name), "%s", name);
    QASM_DISPATCH(AO_AppMgr, &e.super, 0U);
}

TEST_CASE("a terminal install over BLE is given 15 s of silence, not the cable's 5",
          "[appmgr_ao][ble]")
{
    Mockkdrv_slot_Init();
    undo_fresh();
    ask_recv_from((uint8_t)KODE_CONSOLE_SRC_BLE, 4096U, 0U, false, "");
    raw_armed(true);

    run_ticks(APPMGR_RECV_IDLE_DEADLINE_TICKS + 50U);          /* past the cable's bound */
    TEST_ASSERT_FALSE_MESSAGE(klog_has("the host is presumed gone"),
        "a BLE transfer was given up on after the cable's 5 s");

    run_ticks(((KODE_RECV_IDLE_BLE_MS - KODE_RECV_IDLE_MS) / 10U) + 50U);
    TEST_ASSERT_TRUE_MESSAGE(klog_has("no data for 15000 ms"),
        "a BLE transfer that went silent was never given up on");
    (void)log_saw(CONSOLE_RAW_END_SIG);
    keep_end();
}

/** @brief A terminal install of "new" with run=on, from @p src, landed: the AO is launching. */
static void landed_over(uint8_t src)
{
    uint32_t const len = image_named("new");

    undo_fresh();
    keep_hold_dir();
    kapps_dir_t empty = *kdrv_apps_dir();
    empty.count = 0U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&empty));
    fake_ble_console_set_backlog(0U);
    ask_recv_from(src, len, khal_crc32(KHAL_CRC32_INIT, s_named_img, len), true,
                  "new");
    raw_armed(true);
    feed(s_named_img, (uint16_t)len);
    tidy_settle();
}

TEST_CASE("a launch over BLE waits for the console's bytes to leave, up to 2 s",
          "[appmgr_ao][ble]")
{
    uint32_t         off  = 0U;
    uint32_t         size = 0U;
    kboot_run_mode_t mode = KBOOT_RUN_STICKY;

    Mockkdrv_slot_Init();
    landed_over((uint8_t)KODE_CONSOLE_SRC_BLE);
    (void)boot_run_request(&off, &size, &mode);
    fake_ble_console_set_backlog(40U);                        /* the `>ok`, still queued */

    run_ticks(APPMGR_ANSWER_DRAIN_DEADLINE_TICKS + 2U);
    TEST_ASSERT_FALSE_MESSAGE(boot_run_request(&off, &size, &mode),
        "the board reset with the `>ok` still queued for the link");
    run_ticks(100U);
    TEST_ASSERT_FALSE(boot_run_request(&off, &size, &mode));

    fake_ble_console_set_backlog(0U);                         /* the link took it */
    run_ticks(APPMGR_ANSWER_DRAIN_DEADLINE_TICKS + 2U);
    TEST_ASSERT_TRUE_MESSAGE(boot_run_request(&off, &size, &mode),
        "the launch did not follow the answer once the link had it");
    keep_end();
}

TEST_CASE("a launch over BLE is not held for ever by a link that never drains",
          "[appmgr_ao][ble]")
{
    uint32_t         off  = 0U;
    uint32_t         size = 0U;
    kboot_run_mode_t mode = KBOOT_RUN_STICKY;

    Mockkdrv_slot_Init();
    landed_over((uint8_t)KODE_CONSOLE_SRC_BLE);
    (void)boot_run_request(&off, &size, &mode);
    fake_ble_console_set_backlog(40U);

    run_ticks((KODE_RECV_DRAIN_BLE_MS / 10U) - 10U);
    TEST_ASSERT_FALSE(boot_run_request(&off, &size, &mode));
    run_ticks(30U);
    TEST_ASSERT_TRUE_MESSAGE(boot_run_request(&off, &size, &mode),
        "a link that never drained held the launch past its bound");
    TEST_ASSERT_TRUE(klog_has("launching anyway"));
    fake_ble_console_set_backlog(0U);
    keep_end();
}

TEST_CASE("a launch over the cable does not wait for the BLE link",
          "[appmgr_ao][ble]")
{
    uint32_t         off  = 0U;
    uint32_t         size = 0U;
    kboot_run_mode_t mode = KBOOT_RUN_STICKY;

    Mockkdrv_slot_Init();
    landed_over((uint8_t)KODE_CONSOLE_SRC_SERIAL);
    (void)boot_run_request(&off, &size, &mode);
    fake_ble_console_set_backlog(40U);                        /* somebody else's bytes */

    run_ticks(APPMGR_ANSWER_DRAIN_DEADLINE_TICKS + 2U);
    TEST_ASSERT_TRUE_MESSAGE(boot_run_request(&off, &size, &mode),
        "a cable install waited on a BLE link it does not use");
    fake_ble_console_set_backlog(0U);
    keep_end();
}

/* Erase takes the entry out of the directory and the image's first sector
 * off the flash - the descriptor with it - so nothing bootable or nameable is
 * left at that offset. */
TEST_CASE("erasing an installed app removes its entry and its header",
          "[appmgr_ao]")
{
    AppFlashEvt named;
    uint32_t    word = 0U;

    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();
    TEST_ASSERT_TRUE(old_installed());
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_read(KAPPS_DATA_OFF + KAPP_IDF_DESC_OFF,
                                                  &word, sizeof(word)));
    TEST_ASSERT_EQUAL_HEX32(ESP_APP_DESC_MAGIC_WORD, word);

    (void)memset(&named, 0, sizeof(named));
    named.super = (QEvt)QEVT_INITIALIZER(APP_ERASE_REQ_SIG);
    (void)snprintf(named.file, sizeof(named.file), "%s", "old");
    QASM_DISPATCH(AO_AppMgr, &named.super, 0U);
    /* The erase is done once the app's files have gone with it. */
    TEST_ASSERT_TRUE(storage_has_for(STORAGE_APP_WIPE_REQ_SIG, "old"));
    wipe_rsp("old", KODE_OK, 0U, 0U, 0U);

    TEST_ASSERT_FALSE_MESSAGE(old_installed(), "the entry is still listed");
    TEST_ASSERT_TRUE(klog_has("'old' removed"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_read(KAPPS_DATA_OFF + KAPP_IDF_DESC_OFF,
                                                  &word, sizeof(word)));
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(UINT32_MAX, word,
        "the image's descriptor survived the erase");
    keep_end();
}

/* A name that is not installed may still have files: AO_Storage is asked, and
 * the apps that are installed are left as they were. */
TEST_CASE("erasing a name nobody installed touches no installed app", "[appmgr_ao]")
{
    AppFlashEvt named;

    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();

    (void)memset(&named, 0, sizeof(named));
    named.super = (QEvt)QEVT_INITIALIZER(APP_ERASE_REQ_SIG);
    (void)snprintf(named.file, sizeof(named.file), "%s", "nobody");
    QASM_DISPATCH(AO_AppMgr, &named.super, 0U);

    TEST_ASSERT_TRUE_MESSAGE(old_installed(), "an app nobody named was erased");
    TEST_ASSERT_TRUE_MESSAGE(storage_has_for(STORAGE_APP_WIPE_REQ_SIG, "nobody"),
        "the files of a name that is not installed were not asked about");
    wipe_rsp("nobody", KODE_OK, 0U, 0U, 0U);
    TEST_ASSERT_TRUE_MESSAGE(old_installed(), "an app nobody named was erased");
    keep_end();
}

/* The copies run with the AO busy: whatever asks for it meanwhile is refused
 * where it stands - answered, when something waits on the answer - and
 * nothing it would have done happens. */
TEST_CASE("a request while copies run is refused", "[appmgr_ao][keep]")
{
    AppFlashEvt      named;
    AppCatReqEvt     cat;
    AppListReqEvt    list = {
        .super = QEVT_INITIALIZER(APP_LIST_REQ_SIG), .skip = 0U };
    uint32_t         off  = 0U;
    uint32_t         size = 0U;
    kboot_run_mode_t mode = KBOOT_RUN_ONCE;

    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();
    ao_spy_subscribe(APP_CAT_RSP_SIG);
    (void)boot_run_request(&off, &size, &mode);
    (void)begin_install((uint8_t)KDRV_MEDIUM_FS, "new");
    storage_expect(STORAGE_PATH_REQ_SIG, "the victim's path was never asked about");
    answer_stat(false, (uint8_t)KODE_OK);     /* a copy is under way */

    QASM_DISPATCH(AO_AppMgr, &list.super, 0U);
    ask_flash_dst("other.bin", (uint8_t)KFW_TARGET_APP);
    (void)memset(&named, 0, sizeof(named));
    named.super = (QEvt)QEVT_INITIALIZER(APP_RUN_REQ_SIG);
    (void)snprintf(named.file, sizeof(named.file), "%s", "old");
    QASM_DISPATCH(AO_AppMgr, &named.super, 0U);
    named.super = (QEvt)QEVT_INITIALIZER(APP_ERASE_REQ_SIG);
    QASM_DISPATCH(AO_AppMgr, &named.super, 0U);
    ask_recv(old_bytes(), 0U, false);
    (void)memset(&cat, 0, sizeof(cat));
    cat.super = (QEvt)QEVT_INITIALIZER(APP_CAT_REQ_SIG);
    (void)snprintf(cat.project, sizeof(cat.project), "%s", "old");
    QASM_DISPATCH(AO_AppMgr, &cat.super, 0U);

    const QEvt * const rsp = ao_spy_take();
    QActive_unsubscribe(ao_spy(), APP_CAT_RSP_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(rsp, "the console would wait forever");
    TEST_ASSERT_EQUAL_INT(APP_CAT_RSP_SIG, (int)rsp->sig);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_DENIED,
                            ((AppCatRspEvt const *)rsp)->status);
    ao_spy_drain();

    TEST_ASSERT_TRUE(klog_has("busy keeping apps on the card; request ignored"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_tally().open,
        "a second install was started in the middle of the copies");
    TEST_ASSERT_FALSE_MESSAGE(log_saw(CONSOLE_RAW_REQ_SIG),
        "a terminal install was armed in the middle of the copies");
    TEST_ASSERT_FALSE_MESSAGE(boot_run_request(&off, &size, &mode),
        "an app was run in the middle of the copies");
    TEST_ASSERT_TRUE_MESSAGE(old_installed(),
        "an app was erased in the middle of the copies");

    /* Finish the round so that nothing is left armed. */
    const QEvt *wr = storage_wait_for(STORAGE_WRITE_REQ_SIG, 1000U);
    TEST_ASSERT_NOT_NULL(wr);
    QF_gc(wr);
    answer_write((uint8_t)KODE_OK, old_bytes());
    (void)qf_support_pump(AO_AppMgr, 8U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, storage_tally().open,
        "the restart was not the one install it should have been");
    let_the_open_expire();
    keep_end();
}

/* An install that fails after its copies leaves no round behind: the next one
 * starts its own, instead of finding itself the restart of an install that is
 * long over and evicting at once. */
TEST_CASE("a restarted install that fails clears the round", "[appmgr_ao][keep]")
{
    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();
    uint32_t const len = begin_install((uint8_t)KDRV_MEDIUM_FS, "new");
    storage_expect(STORAGE_PATH_REQ_SIG, "the victim's path was never asked about");
    answer_stat(true, (uint8_t)KODE_OK);
    (void)qf_support_pump(AO_AppMgr, 8U);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install did not start again");

    answer_open_with(0U, (uint8_t)KDRV_MEDIUM_FS, (uint8_t)KODE_ERROR);   /* gone */
    TEST_ASSERT_TRUE_MESSAGE(old_installed(), "the failed restart evicted nothing");

    /* The next install, over the same full region: a fresh round. */
    ask_flash_dst("new.bin", (uint8_t)KFW_TARGET_APP);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the next install never asked for its file");
    answer_open(len, (uint8_t)KDRV_MEDIUM_FS);
    answer_data(len);
    TEST_ASSERT_TRUE_MESSAGE(old_installed(),
        "the next install evicted at once: the last one's round was still held");
    storage_expect(STORAGE_PATH_REQ_SIG,
                   "the next install did not start a round of its own");

    answer_stat(true, (uint8_t)KODE_OK);      /* and ends it, so nothing is armed */
    (void)qf_support_pump(AO_AppMgr, 8U);
    let_the_open_expire();
    keep_end();
}

/* A victim that cannot be named cannot have a path on the card: it is a
 * failed copy, with nothing asked of AO_Storage, and the round carries on. */
TEST_CASE("a victim with no name is a failed copy", "[appmgr_ao][keep]")
{
    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_erase_header(KAPPS_DATA_OFF));
    (void)begin_install((uint8_t)KDRV_MEDIUM_FS, "new");
    (void)qf_support_pump(AO_AppMgr, 8U);     /* the round ends at once */

    storage_seen_t const seen = storage_tally();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, seen.path,
        "a victim with no name was looked for on the card");
    TEST_ASSERT_TRUE(klog_has("an app with no readable name"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, seen.open,
        "the install did not start again after the nameless victim");

    let_the_open_expire();
    keep_end();
}

/* A card write of several MB gives AppMgr nothing to report, and AO_Power
 * takes the blue pulse back after three seconds of that: so the same percent
 * goes out again every second until the card answers. */
TEST_CASE("the bar does not go quiet while the card writes", "[appmgr_ao][keep]")
{
    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();
    ao_spy_subscribe(APP_FLASH_PROGRESS_SIG);
    (void)begin_install((uint8_t)KDRV_MEDIUM_FS, "new");
    storage_expect(STORAGE_PATH_REQ_SIG, "the victim's path was never asked about");
    answer_stat(false, (uint8_t)KODE_OK);
    const QEvt *wr = storage_wait_for(STORAGE_WRITE_REQ_SIG, 1000U);
    TEST_ASSERT_NOT_NULL_MESSAGE(wr, "the copy never reached AO_Storage");
    QF_gc(wr);

    ao_spy_drain();                           /* what the read published */
    run_ticks(3U * APPMGR_KEEP_BEAT_TICKS);   /* the card has not answered */
    unsigned const beats = spy_count(APP_FLASH_PROGRESS_SIG);
    QActive_unsubscribe(ao_spy(), APP_FLASH_PROGRESS_SIG);
    ao_spy_drain();

    TEST_ASSERT_TRUE_MESSAGE(2U <= beats,
        "the bar and the LED pulse went quiet for three seconds");

    answer_write((uint8_t)KODE_OK, old_bytes());
    (void)qf_support_pump(AO_AppMgr, 8U);
    let_the_open_expire();
    keep_end();
}

/** @brief A directory of 64 tiny apps, "a00" (seq 1) to "a63" (seq 64). */
static void region_of_tiny_apps(void)
{
    keep_hold_dir();
    kapps_dir_t dir = s_keep_dir;
    dir.count = KAPPS_MAX_ENTRIES;
    for ( uint32_t i = 0U; KAPPS_MAX_ENTRIES > i; ++i )
    {
        char name[8];
        (void)snprintf(name, sizeof(name), "a%02u", (unsigned)i);
        dir.e[i].offset = KAPPS_DATA_OFF + (i * KAPPS_ALIGN);
        dir.e[i].size   = 4096U;
        dir.e[i].seq    = i + 1U;
        dir.e[i].flags  = KAPPS_ENTRY_VALID;
        plant_app(dir.e[i].offset, name);
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));
}

/* The directory is full of apps of other names, but the install replaces its
 * namesake, which frees its own entry: the plan names no slot victim, so
 * nothing is copied to the card and the oldest app stays where it is. */
TEST_CASE("a full directory and a namesake copy nothing and lose nothing",
          "[appmgr_ao][keep]")
{
    Mockkdrv_slot_Init();
    keep_fresh();
    region_of_tiny_apps();
    (void)begin_install((uint8_t)KDRV_MEDIUM_FS, "a10");
    answer_eof();
    run_ticks(4U);

    kapps_dir_t const *d = kdrv_apps_dir();
    TEST_ASSERT_FALSE_MESSAGE(klog_has("to keep on the card"),
        "an app was copied though nothing is evicted");
    TEST_ASSERT_TRUE_MESSAGE(klog_has("install complete"),
        "the install did not finish");
    TEST_ASSERT_TRUE_MESSAGE(seq_installed(1U),
        "the oldest app went, though the install replaced its namesake");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(KAPPS_MAX_ENTRIES, d->count,
        "the namesake's slot is the one the install took");
    TEST_ASSERT_FALSE_MESSAGE(klog_has("evicting"), "something was evicted");
    keep_end();
}

/* Two victims in one round: the first is already on the card and is skipped,
 * the second is copied, and the bar - one 0-100 % for the copies and the
 * write together - has moved through both by the time the install starts
 * again, and starts again from there rather than from nothing. */
TEST_CASE("two victims: one already kept, one copied, one bar",
          "[appmgr_ao][keep]")
{
    uint32_t const mb  = 0x100000U;
    uint32_t const len = image_named("new");
    unsigned       last = 0U;
    unsigned       first_after = 0U;
    bool           monotonic = true;

    Mockkdrv_slot_Init();
    keep_fresh();
    keep_hold_dir();
    kapps_dir_t dir = s_keep_dir;
    dir.count = 3U;
    dir.e[0].offset = KAPPS_DATA_OFF;
    dir.e[0].size   = 3U * mb;
    dir.e[0].seq    = 1U;
    dir.e[0].flags  = KAPPS_ENTRY_VALID;
    dir.e[1].offset = KAPPS_DATA_OFF + (3U * mb);
    dir.e[1].size   = 3U * mb;
    dir.e[1].seq    = 2U;
    dir.e[1].flags  = KAPPS_ENTRY_VALID;
    dir.e[2].offset = KAPPS_DATA_OFF + (6U * mb);
    dir.e[2].size   = kdrv_apps_region_size() - dir.e[2].offset;
    dir.e[2].seq    = 3U;
    dir.e[2].flags  = KAPPS_ENTRY_VALID;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));
    plant_app(dir.e[0].offset, "first");
    plant_app(dir.e[1].offset, "second");
    plant_app(dir.e[2].offset, "tail");
    ao_spy_subscribe(APP_FLASH_PROGRESS_SIG);

    /* An image of 5 MB by its file's size: the plan has to evict two apps. */
    ask_flash_dst("new.bin", (uint8_t)KFW_TARGET_APP);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install never asked for its file");
    answer_open(5U * mb, (uint8_t)KDRV_MEDIUM_FS);
    answer_data(len);

    const QEvt *stat = storage_take(STORAGE_PATH_REQ_SIG);
    TEST_ASSERT_NOT_NULL(stat);
    TEST_ASSERT_EQUAL_STRING("/sdcard/first.bin", ((StoragePathEvt const *)stat)->path);
    QF_gc(stat);
    answer_stat(true, (uint8_t)KODE_OK);
    stat = storage_take(STORAGE_PATH_REQ_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(stat, "the second victim was not looked for");
    TEST_ASSERT_EQUAL_STRING("/sdcard/second.bin", ((StoragePathEvt const *)stat)->path);
    QF_gc(stat);

    /* A tick of the pacer that fired just before a write's answer disarmed
     * it arrives with the next victim's STAT out: it is not the end of the
     * round. */
    QEvt stale = QEVT_INITIALIZER(APP_FLASH_CHUNK_SIG);
    QASM_DISPATCH(AO_AppMgr, &stale, 0U);
    (void)qf_support_pump(AO_AppMgr, 8U);     /* a restart would be posted */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_tally().open,
        "a stale tick ended the round with a victim still to be looked for");

    answer_stat(false, (uint8_t)KODE_OK);

    const QEvt *wr = storage_wait_for(STORAGE_WRITE_REQ_SIG, 1000U);
    TEST_ASSERT_NOT_NULL_MESSAGE(wr, "the second victim was not copied");
    TEST_ASSERT_EQUAL_STRING("/sdcard/second.bin", ((StorageWriteEvt const *)wr)->name);
    TEST_ASSERT_EQUAL_UINT32(3U * mb, ((StorageWriteEvt const *)wr)->len);
    QF_gc(wr);
    answer_write((uint8_t)KODE_OK, 3U * mb);
    (void)qf_support_pump(AO_AppMgr, 8U);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install did not start again");
    TEST_ASSERT_TRUE(seq_installed(1U));
    TEST_ASSERT_TRUE(seq_installed(2U));

    answer_open(5U * mb, (uint8_t)KDRV_MEDIUM_FS);   /* the restart's announce */
    answer_eof();                                    /* ends it: nothing armed */
    run_ticks(4U);

    /* 6 MB of copies against 5 MB of image: the bar is at 6/11 when the
     * install starts again, and it only ever rose. */
    for ( const QEvt *e = ao_spy_take(); NULL != e; e = ao_spy_take() )
    {
        if ( (int)e->sig == APP_FLASH_PROGRESS_SIG )
        {
            unsigned const pct = ((AppProgressEvt const *)e)->percent;
            if ( pct < last )
            {
                monotonic = false;
            }
            last = pct;
            if ( (0U == first_after) && (50U <= pct) )
            {
                first_after = pct;
            }
        }
    }
    QActive_unsubscribe(ao_spy(), APP_FLASH_PROGRESS_SIG);
    ao_spy_drain();

    TEST_ASSERT_TRUE_MESSAGE(monotonic, "the bar went backwards");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(54U, last,
        "the restarted install did not resume the bar where the copies left it");
    TEST_ASSERT_EQUAL_UINT(54U, first_after);
    keep_end();
}

/* ---- A round that AO_Storage never answers ---------------------------------
 * AO_Storage drops a path or a write request when the event pool is dry, and
 * says nothing back. AppMgr_keeping has the deadline AppMgr_opening has for
 * the same failure, or the AO would be refusing every request until a reset.
 */

/** @brief The deadline of a copy of @p len bytes, as appmgr_ao.c has it. */
#define KEEP_WRITE_DEADLINE_TICKS(len) \
    ((60U + ((len) / (128U * 1024U))) * (1000U / 10U))

/* The STAT is lost: the victim is a failed copy, the round goes on, and from
 * a file that is not on the card the install starts again. */
TEST_CASE("a STAT nobody answers is a failed copy and the round goes on",
          "[appmgr_ao][keep]")
{
    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();
    (void)begin_install((uint8_t)KDRV_MEDIUM_FS, "new");
    storage_expect(STORAGE_PATH_REQ_SIG, "the victim's path was never asked about");

    run_ticks(APPMGR_OPEN_DEADLINE_TICKS + 50U);   /* then the restart's open */

    TEST_ASSERT_TRUE(klog_has("nothing answered about 'old' on the card"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, storage_tally().open,
        "the round did not go on after the lost STAT");
    TEST_ASSERT_TRUE(old_installed());

    let_the_open_expire();
    keep_end();
}

/* The write is lost: the round goes on, and the buffer is not freed - AO_Storage
 * may still hold the pointer, and the data must stay valid until its answer. */
TEST_CASE("a write nobody answers is given up on without freeing its buffer",
          "[appmgr_ao][keep]")
{
    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();
    (void)begin_install((uint8_t)KDRV_MEDIUM_FS, "new");
    storage_expect(STORAGE_PATH_REQ_SIG, "the victim's path was never asked about");
    answer_stat(false, (uint8_t)KODE_OK);
    const QEvt *wr = storage_wait_for(STORAGE_WRITE_REQ_SIG, 1000U);
    TEST_ASSERT_NOT_NULL_MESSAGE(wr, "the copy never reached AO_Storage");
    QF_gc(wr);

    unsigned const frees = fake_psram_free_count();
    run_ticks(KEEP_WRITE_DEADLINE_TICKS(old_bytes()) + 50U);

    TEST_ASSERT_TRUE(klog_has("the card never answered for 'old'"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(frees, fake_psram_free_count(),
        "the buffer AO_Storage may still hold was freed");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, storage_tally().open,
        "the round did not go on after the lost write");
    TEST_ASSERT_TRUE(old_installed());

    let_the_open_expire();
    keep_end();
}

/* The abandoned write answers after all, while the next victim's STAT is out:
 * it belongs to nobody, and must settle nothing. */
TEST_CASE("a late write answer settles nothing", "[appmgr_ao][keep]")
{
    uint32_t const mb  = 0x100000U;
    uint32_t const len = image_named("new");

    Mockkdrv_slot_Init();
    keep_fresh();
    keep_hold_dir();
    kapps_dir_t dir = s_keep_dir;
    dir.count = 3U;
    dir.e[0].offset = KAPPS_DATA_OFF;
    dir.e[0].size   = 3U * mb;
    dir.e[0].seq    = 1U;
    dir.e[0].flags  = KAPPS_ENTRY_VALID;
    dir.e[1].offset = KAPPS_DATA_OFF + (3U * mb);
    dir.e[1].size   = 3U * mb;
    dir.e[1].seq    = 2U;
    dir.e[1].flags  = KAPPS_ENTRY_VALID;
    dir.e[2].offset = KAPPS_DATA_OFF + (6U * mb);
    dir.e[2].size   = kdrv_apps_region_size() - dir.e[2].offset;
    dir.e[2].seq    = 3U;
    dir.e[2].flags  = KAPPS_ENTRY_VALID;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));
    plant_app(dir.e[0].offset, "first");
    plant_app(dir.e[1].offset, "second");
    plant_app(dir.e[2].offset, "tail");

    ask_flash_dst("new.bin", (uint8_t)KFW_TARGET_APP);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install never asked for its file");
    answer_open(5U * mb, (uint8_t)KDRV_MEDIUM_FS);
    answer_data(len);
    storage_expect(STORAGE_PATH_REQ_SIG, "the first victim was not looked for");
    answer_stat(false, (uint8_t)KODE_OK);
    const QEvt *wr = storage_wait_for(STORAGE_WRITE_REQ_SIG, 1000U);
    TEST_ASSERT_NOT_NULL_MESSAGE(wr, "the first victim was not copied");
    QF_gc(wr);

    /* Never answered: past the deadline the second victim's STAT is out. */
    run_ticks(KEEP_WRITE_DEADLINE_TICKS(3U * mb) + 50U);
    const QEvt *stat = storage_take(STORAGE_PATH_REQ_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(stat, "the round did not go on to the second victim");
    TEST_ASSERT_EQUAL_STRING("/sdcard/second.bin", ((StoragePathEvt const *)stat)->path);
    QF_gc(stat);

    answer_write((uint8_t)KODE_OK, 3U * mb);       /* the late one */
    (void)qf_support_pump(AO_AppMgr, 8U);
    TEST_ASSERT_FALSE_MESSAGE(klog_has("kept as"),
        "a late answer was taken for a copy that landed");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_tally().open,
        "a late answer ended the round with a victim's STAT still out");

    answer_stat(true, (uint8_t)KODE_OK);           /* the round ends properly */
    (void)qf_support_pump(AO_AppMgr, 8U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, storage_tally().open,
        "the install did not start again");
    let_the_open_expire();
    keep_end();
}

/* The first victim's STAT is given up on, and its answer comes while the
 * second victim's STAT is out: "already there" is the first one's word, and
 * taking it for the second would evict that app with no copy anywhere. */
TEST_CASE("a late STAT answer is not taken for the next victim's",
          "[appmgr_ao][keep]")
{
    uint32_t const mb  = 0x100000U;
    uint32_t const len = image_named("new");

    Mockkdrv_slot_Init();
    keep_fresh();
    keep_hold_dir();
    kapps_dir_t dir = s_keep_dir;
    dir.count = 3U;
    dir.e[0].offset = KAPPS_DATA_OFF;
    dir.e[0].size   = 3U * mb;
    dir.e[0].seq    = 1U;
    dir.e[0].flags  = KAPPS_ENTRY_VALID;
    dir.e[1].offset = KAPPS_DATA_OFF + (3U * mb);
    dir.e[1].size   = 3U * mb;
    dir.e[1].seq    = 2U;
    dir.e[1].flags  = KAPPS_ENTRY_VALID;
    dir.e[2].offset = KAPPS_DATA_OFF + (6U * mb);
    dir.e[2].size   = kdrv_apps_region_size() - dir.e[2].offset;
    dir.e[2].seq    = 3U;
    dir.e[2].flags  = KAPPS_ENTRY_VALID;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));
    plant_app(dir.e[0].offset, "first");
    plant_app(dir.e[1].offset, "second");
    plant_app(dir.e[2].offset, "tail");

    ask_flash_dst("new.bin", (uint8_t)KFW_TARGET_APP);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install never asked for its file");
    answer_open(5U * mb, (uint8_t)KDRV_MEDIUM_FS);
    answer_data(len);
    storage_expect(STORAGE_PATH_REQ_SIG, "the first victim was not looked for");

    /* Never answered in time: past the deadline the second STAT is out. */
    run_ticks(APPMGR_OPEN_DEADLINE_TICKS + 50U);
    const QEvt *stat = storage_take(STORAGE_PATH_REQ_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(stat, "the round did not go on to the second victim");
    TEST_ASSERT_EQUAL_STRING("/sdcard/second.bin", ((StoragePathEvt const *)stat)->path);
    QF_gc(stat);

    answer_stat(true, (uint8_t)KODE_OK);           /* the first one's, late */
    (void)qf_support_pump(AO_AppMgr, 8U);
    TEST_ASSERT_FALSE_MESSAGE(klog_has("'second' is already on the card"),
        "the late answer was taken for the second victim");

    answer_stat(false, (uint8_t)KODE_OK);          /* the second one's own */
    const QEvt *wr = storage_wait_for(STORAGE_WRITE_REQ_SIG, 1000U);
    TEST_ASSERT_NOT_NULL_MESSAGE(wr, "the second victim was not copied");
    QF_gc(wr);
    answer_write((uint8_t)KODE_OK, 3U * mb);
    (void)qf_support_pump(AO_AppMgr, 8U);
    TEST_ASSERT_TRUE(klog_has("'second' kept as /sdcard/second.bin"));

    let_the_open_expire();
    keep_end();
}

/* ---- The mailbox keeps its victims too --------------------------------------
 * Adoption evicts like any install, so it makes the same copies first. It has
 * no Opening card and no medium to reopen: its round ends by asking for the
 * adoption again, and the second plan is the one that evicts.
 */

/**
 * @brief The next install over a full region must start a round of its own.
 *
 * A round left standing by the adoption before it would be taken for that
 * install's, and the install would evict without a copy. Asked through a file
 * install, which defers at its plan whenever there is a victim to keep, and
 * wound down the way the other keep cases end.
 */
static void next_install_keeps(void)
{
    put_old(old_bytes());
    (void)begin_install((uint8_t)KDRV_MEDIUM_FS, "new");
    storage_expect(STORAGE_PATH_REQ_SIG,
                   "the install after the adoption did not look for its victim: "
                   "the adoption's round was still standing");
    answer_stat(true, (uint8_t)KODE_OK);
    (void)qf_support_pump(AO_AppMgr, 8U);
    storage_expect(STORAGE_OPEN_REQ_SIG,
                   "the install did not start again after its round");
    let_the_open_expire();
}

/**
 * @brief Park a compressed container in the mailbox: a few bytes on the
 *        partition, an image of @p image_len bytes once unwrapped.
 *
 * What the mailbox's walk plans with is the container's own length, and only
 * the decoder, once it has read the header, learns the real one.
 */
static void park_container(uint32_t image_len)
{
    uint8_t   buf[KFW_HDR_LEN + 16U];
    kfw_hdr_t h;

    (void)memset(buf, 0xA5, sizeof(buf));
    (void)memset(&h, 0, sizeof(h));
    h.magic       = KFW_MAGIC;
    h.hdr_version = (uint16_t)KFW_HDR_VERSION;
    h.hdr_len     = (uint16_t)KFW_HDR_LEN;
    h.target      = (uint8_t)KFW_TARGET_APP;
    h.comp        = (uint8_t)KFW_COMP_DEFLATE;
    h.payload_len = 16U;
    h.image_len   = image_len;
    (void)snprintf(h.name, sizeof(h.name), "%s", "big");
    h.crc = kfw_crc32(&h, offsetof(kfw_hdr_t, crc));
    (void)memcpy(buf, &h, sizeof(h));

    kdrv_apps_pending_write_abort();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_write_begin(sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_write(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_write_end());
}

/* `install src="pending" dst=app` is the same adoption asked for at the
 * console, and there `run` decides as it does for every other source: off
 * installs and boots nothing, on boots the app once - never the sticky boot
 * an upload gets, which would hold the board in the app across resets. */
TEST_CASE("install src=pending boots the adopted app only on run, and once",
          "[appmgr_ao]")
{
    FlashReqEvt       req;
    kapps_dir_t       dir;
    uint32_t          off  = 0U;
    uint32_t          size = 0U;
    kboot_run_mode_t  mode = KBOOT_RUN_STICKY;
    bool              booted_off;
    bool              booted_on;
    bool              consumed_off;
    kboot_run_mode_t  mode_on;

    Mockkdrv_slot_Init();
    appmgr_fresh();
    (void)boot_run_request(&off, &size, &mode);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_init());
    kapps_dir_t const saved = *kdrv_apps_dir();
    dir       = saved;
    dir.count = 0U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));

    (void)memset(&req, 0, sizeof(req));
    req.super    = (QEvt)QEVT_INITIALIZER(FLASH_REQ_SIG);
    req.src_kind = (uint8_t)KFW_SRC_PENDING;
    req.dst_kind = (uint8_t)KFW_TARGET_APP;
    (void)snprintf(req.src, sizeof(req.src), "%s", "pending");

    (void)park_pending("asked");
    req.launch = false;
    QASM_DISPATCH(AO_AppMgr, &req.super, 0U);
    run_ticks(4U);
    booted_off   = boot_run_request(&off, &size, &mode);
    consumed_off = !kdrv_apps_pending_present(NULL, NULL);
    /* The directory started empty: one entry is the app just adopted. */
    bool const installed = (1U == kdrv_apps_dir()->count);

    (void)park_pending("asked");
    req.launch = true;
    QASM_DISPATCH(AO_AppMgr, &req.super, 0U);
    run_ticks(4U);
    booted_on = boot_run_request(&off, &size, &mode);
    mode_on   = mode;
    (void)kdrv_apps_pending_consume();

    kapps_dir_t restore = saved;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&restore));
    Mockkdrv_slot_Destroy();

    TEST_ASSERT_TRUE_MESSAGE(installed, "run=off still installs the app");
    TEST_ASSERT_TRUE_MESSAGE(consumed_off, "the mailbox is consumed either way");
    TEST_ASSERT_FALSE_MESSAGE(booted_off, "run=off asked AO_Boot for a boot");
    TEST_ASSERT_TRUE_MESSAGE(booted_on, "run=on never asked AO_Boot to boot it");
    TEST_ASSERT_EQUAL_INT_MESSAGE(KBOOT_RUN_ONCE, mode_on,
        "a console install boots once: any reset must come home");
}

/* An install asked for with nothing parked does nothing, and a host that
 * reads the log has to be told. The boot's own look at an empty mailbox stays
 * quiet: that is every boot. */
TEST_CASE("an install from an empty mailbox says there is nothing to install",
          "[appmgr_ao]")
{
    static const QEvt adopt = QEVT_INITIALIZER(APP_ADOPT_SIG);
    FlashReqEvt       req;

    Mockkdrv_slot_Init();
    appmgr_fresh();
    (void)kdrv_apps_pending_consume();
    TEST_ASSERT_FALSE(kdrv_apps_pending_present(NULL, NULL));

    fake_klog_reset();
    QASM_DISPATCH(AO_AppMgr, &adopt, 0U);
    run_ticks(2U);
    TEST_ASSERT_FALSE_MESSAGE(klog_has("the mailbox is empty"),
        "the boot's own look at an empty mailbox said so");

    (void)memset(&req, 0, sizeof(req));
    req.super    = (QEvt)QEVT_INITIALIZER(FLASH_REQ_SIG);
    req.src_kind = (uint8_t)KFW_SRC_PENDING;
    req.dst_kind = (uint8_t)KFW_TARGET_APP;
    (void)snprintf(req.src, sizeof(req.src), "%s", "pending");
    fake_klog_reset();
    QASM_DISPATCH(AO_AppMgr, &req.super, 0U);
    run_ticks(4U);
    TEST_ASSERT_TRUE_MESSAGE(klog_has("the mailbox is empty"),
        "an install of nothing said nothing");
}

/* The first plan evicts "old", so the round runs before anything is evicted:
 * the STAT, the copy, the write's answer, and only then the second pass. */
TEST_CASE("adoption over a full region keeps the victim first",
          "[appmgr_ao][keep]")
{
    static const QEvt adopt = QEVT_INITIALIZER(APP_ADOPT_SIG);
    uint32_t          off  = 0U;
    uint32_t          size = 0U;
    kboot_run_mode_t  mode = KBOOT_RUN_ONCE;

    Mockkdrv_slot_Init();
    keep_fresh();
    (void)boot_run_request(&off, &size, &mode);
    region_full_of_old();
    (void)park_pending("fresh");

    QASM_DISPATCH(AO_AppMgr, &adopt, 0U);
    TEST_ASSERT_TRUE_MESSAGE(old_installed(), "nothing may be evicted yet");
    const QEvt *stat = storage_take(STORAGE_PATH_REQ_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(stat, "the victim's path was never asked about");
    TEST_ASSERT_EQUAL_STRING("/sdcard/old.bin", ((StoragePathEvt const *)stat)->path);
    QF_gc(stat);
    TEST_ASSERT_TRUE(klog_has("to keep on the card before adopting the mailbox"));

    answer_stat(false, (uint8_t)KODE_OK);
    const QEvt *wr = storage_wait_for(STORAGE_WRITE_REQ_SIG, 1000U);
    TEST_ASSERT_NOT_NULL_MESSAGE(wr, "the copy never reached AO_Storage");
    TEST_ASSERT_EQUAL_UINT32(old_bytes(), ((StorageWriteEvt const *)wr)->len);
    QF_gc(wr);
    TEST_ASSERT_TRUE_MESSAGE(old_installed(), "evicted before its copy landed");
    TEST_ASSERT_FALSE(boot_run_request(&off, &size, &mode));

    answer_write((uint8_t)KODE_OK, old_bytes());
    run_ticks(8U);                       /* the LIFO adopt, then the copy */

    TEST_ASSERT_TRUE(klog_has("'old' kept as /sdcard/old.bin"));
    TEST_ASSERT_FALSE_MESSAGE(old_installed(), "the mailbox did not evict after the copy");
    TEST_ASSERT_TRUE_MESSAGE(boot_run_request(&off, &size, &mode),
                             "the adoption did not end in a boot request");
    TEST_ASSERT_FALSE(kdrv_apps_pending_present(NULL, NULL));

    next_install_keeps();                /* and the round ended with the adoption */
    keep_end();
}

/* The glass raises a locked Opening card on the first progress report and
 * only a done or a fail lowers it; adoption publishes neither, so a round that
 * reported would leave the card up for good. The write is left outstanding for
 * several heartbeats (one a second) before it is answered. */
TEST_CASE("an adoption's round publishes no progress, heartbeats included",
          "[appmgr_ao][keep]")
{
    static const QEvt adopt = QEVT_INITIALIZER(APP_ADOPT_SIG);
    uint32_t          off  = 0U;
    uint32_t          size = 0U;
    kboot_run_mode_t  mode = KBOOT_RUN_ONCE;

    Mockkdrv_slot_Init();
    keep_fresh();
    (void)boot_run_request(&off, &size, &mode);
    region_full_of_old();
    (void)park_pending("fresh");
    ao_spy_subscribe(APP_FLASH_PROGRESS_SIG);

    QASM_DISPATCH(AO_AppMgr, &adopt, 0U);
    storage_expect(STORAGE_PATH_REQ_SIG, "the victim's path was never asked about");
    answer_stat(false, (uint8_t)KODE_OK);
    const QEvt *wr = storage_wait_for(STORAGE_WRITE_REQ_SIG, 1000U);
    TEST_ASSERT_NOT_NULL_MESSAGE(wr, "the copy never reached AO_Storage");
    QF_gc(wr);

    run_ticks((3U * APPMGR_KEEP_BEAT_TICKS) + 20U);    /* three heartbeats due */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, spy_count(APP_FLASH_PROGRESS_SIG),
        "a heartbeat of the copy reached the glass");

    answer_write((uint8_t)KODE_OK, old_bytes());
    run_ticks(8U);
    TEST_ASSERT_TRUE(klog_has("'old' kept as /sdcard/old.bin"));
    TEST_ASSERT_TRUE(boot_run_request(&off, &size, &mode));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, spy_count(APP_FLASH_PROGRESS_SIG),
        "the round's end raised a card nothing will lower");

    /* The spy's queue holds 32 and a publish into a full one asserts: what
     * this case subscribed must not outlive it. */
    QActive_unsubscribe(ao_spy(), APP_FLASH_PROGRESS_SIG);
    ao_spy_drain();
    keep_end();
}

/* A copy that cannot be made is a warning and no more: the mailbox is a
 * development door, and the upload it carries has already been paid for. */
TEST_CASE("adoption goes on when the victim cannot be kept",
          "[appmgr_ao][keep]")
{
    static const QEvt adopt = QEVT_INITIALIZER(APP_ADOPT_SIG);
    uint32_t          off  = 0U;
    uint32_t          size = 0U;
    kboot_run_mode_t  mode = KBOOT_RUN_ONCE;

    Mockkdrv_slot_Init();
    keep_fresh();
    (void)boot_run_request(&off, &size, &mode);
    region_full_of_old();
    (void)park_pending("fresh");

    QASM_DISPATCH(AO_AppMgr, &adopt, 0U);
    storage_expect(STORAGE_PATH_REQ_SIG, "the victim's path was never asked about");
    answer_stat(false, (uint8_t)KODE_ERROR);     /* no card */
    run_ticks(8U);

    TEST_ASSERT_TRUE(klog_has("evicting 'old' to make room; no copy kept"));
    TEST_ASSERT_FALSE(old_installed());
    TEST_ASSERT_FALSE_MESSAGE(boot_run_request(&off, &size, &mode),
        "the app booted before the evicted app's folder was deleted");
    tidy_settle();
    TEST_ASSERT_TRUE(boot_run_request(&off, &size, &mode));
    TEST_ASSERT_FALSE(kdrv_apps_pending_present(NULL, NULL));

    next_install_keeps();
    keep_end();
}

/** @brief Three apps that fill the region: "first" and "second" of 3 MB, then
 *         "tail" up to its end. A 5 MB image has to evict the first two. */
static void three_apps(void)
{
    uint32_t const mb = 0x100000U;

    keep_hold_dir();
    kapps_dir_t dir = s_keep_dir;
    dir.count = 3U;
    dir.e[0].offset = KAPPS_DATA_OFF;
    dir.e[0].size   = 3U * mb;
    dir.e[0].seq    = 1U;
    dir.e[0].flags  = KAPPS_ENTRY_VALID;
    dir.e[1].offset = KAPPS_DATA_OFF + (3U * mb);
    dir.e[1].size   = 3U * mb;
    dir.e[1].seq    = 2U;
    dir.e[1].flags  = KAPPS_ENTRY_VALID;
    dir.e[2].offset = KAPPS_DATA_OFF + (6U * mb);
    dir.e[2].size   = kdrv_apps_region_size() - dir.e[2].offset;
    dir.e[2].seq    = 3U;
    dir.e[2].flags  = KAPPS_ENTRY_VALID;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));
    plant_app(dir.e[0].offset, "first");
    plant_app(dir.e[1].offset, "second");
    plant_app(dir.e[2].offset, "tail");
}

/** @brief Take AO_Storage's next path request and check what it asks. */
static void path_req_expect(uint8_t op, const char *path, const char *what)
{
    const QEvt * const e = storage_take(STORAGE_PATH_REQ_SIG);

    TEST_ASSERT_NOT_NULL_MESSAGE(e, what);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(op, ((StoragePathEvt const *)e)->op, what);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(path, ((StoragePathEvt const *)e)->path, what);
    QF_gc(e);
}

/* The mailbox plans with the container's decoded length, the one the decoder
 * plans with on its first chunk. Planned with the container's own few bytes,
 * the walk named the first victim only, the round copied that one, and the
 * decoder's plan then named the second after the round was over: evicted
 * uncopied. Every victim is looked for before anything is evicted. */
TEST_CASE("a deflated container is planned for its image, so every victim is kept first",
          "[appmgr_ao][keep]")
{
    static const QEvt adopt = QEVT_INITIALIZER(APP_ADOPT_SIG);
    uint32_t const    mb   = 0x100000U;
    uint32_t          off  = 0U;
    uint32_t          size = 0U;
    kboot_run_mode_t  mode = KBOOT_RUN_ONCE;

    Mockkdrv_slot_Init();
    keep_fresh();
    (void)boot_run_request(&off, &size, &mode);
    three_apps();
    park_container(5U * mb);

    QASM_DISPATCH(AO_AppMgr, &adopt, 0U);
    path_req_expect((uint8_t)STORAGE_PATH_STAT, "/sdcard/first.bin",
                    "the first victim was not looked for");
    TEST_ASSERT_TRUE_MESSAGE(klog_has("2 app(s) to keep on the card before adopting"),
        "the mailbox's plan did not name both victims");
    answer_stat(false, (uint8_t)KODE_OK);
    const QEvt *wr = storage_wait_for(STORAGE_WRITE_REQ_SIG, 1000U);
    TEST_ASSERT_NOT_NULL_MESSAGE(wr, "the first victim was not copied");
    QF_gc(wr);
    answer_write((uint8_t)KODE_OK, 3U * mb);

    /* The second victim is looked for with the first one's copy done and
     * nothing evicted yet. */
    path_req_expect((uint8_t)STORAGE_PATH_STAT, "/sdcard/second.bin",
                    "the second victim was never looked for");
    TEST_ASSERT_TRUE(seq_installed(1U));
    TEST_ASSERT_TRUE(seq_installed(2U));
    answer_stat(false, (uint8_t)KODE_OK);
    wr = storage_wait_for(STORAGE_WRITE_REQ_SIG, 1000U);
    TEST_ASSERT_NOT_NULL_MESSAGE(wr, "the second victim was not copied");
    TEST_ASSERT_EQUAL_STRING("/sdcard/second.bin", ((StorageWriteEvt const *)wr)->name);
    QF_gc(wr);
    TEST_ASSERT_TRUE_MESSAGE(seq_installed(2U), "evicted before its copy landed");
    answer_write((uint8_t)KODE_OK, 3U * mb);
    run_ticks(8U);                       /* the LIFO adopt: the second pass */

    TEST_ASSERT_TRUE(klog_has("evicting 'first'"));
    TEST_ASSERT_TRUE(klog_has("evicting 'second'"));
    /* The fake inflater yields no image, so the adoption fails before its
     * write erases a block: both victims come back with the failure. */
    TEST_ASSERT_TRUE_MESSAGE(seq_installed(1U) && seq_installed(2U),
        "the failed adoption did not give back the apps its write never reached");
    TEST_ASSERT_TRUE(klog_has("kept 'first' after the failed install"));
    TEST_ASSERT_TRUE(klog_has("kept 'second' after the failed install"));
    TEST_ASSERT_FALSE_MESSAGE(klog_has("no copy kept"),
        "a victim the card had taken went down with a warning");
    TEST_ASSERT_FALSE_MESSAGE(klog_has("before this install"),
        "the decoder's plan named victims the mailbox's did not");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_tally().path,
        "a third victim was looked for");

    (void)kdrv_apps_pending_consume();   /* the fake inflater yields no image */
    kfw_abort();
    next_install_keeps();
    keep_end();
}

/* The deferral inside adoption is what is left of the decoder's plan being
 * able to disagree with the mailbox's: here the directory changes after the
 * mailbox planned (nothing else writes it, so this is a guard, not a flow).
 * There is no `writing` flag to close the session by, and it is no failure. */
TEST_CASE("a plan that changes under an adoption is deferred from inside it",
          "[appmgr_ao][keep]")
{
    static const QEvt adopt = QEVT_INITIALIZER(APP_ADOPT_SIG);
    uint32_t          off  = 0U;
    uint32_t          size = 0U;
    kboot_run_mode_t  mode = KBOOT_RUN_ONCE;

    Mockkdrv_slot_Init();
    keep_fresh();
    (void)boot_run_request(&off, &size, &mode);
    keep_hold_dir();
    put_old(0x100000U);
    park_container(0x200000U);

    QASM_DISPATCH(AO_AppMgr, &adopt, 0U);     /* room to spare: no victim */
    TEST_ASSERT_FALSE(klog_has("before adopting the mailbox"));
    put_old(old_bytes());                     /* and now "old" is in the way */
    const QEvt *stat = storage_wait_for(STORAGE_PATH_REQ_SIG, 10U);
    TEST_ASSERT_NOT_NULL_MESSAGE(stat, "the decoder's victim was never asked about");
    TEST_ASSERT_EQUAL_STRING("/sdcard/old.bin", ((StoragePathEvt const *)stat)->path);
    QF_gc(stat);
    TEST_ASSERT_TRUE_MESSAGE(klog_has("before this install"),
        "the deferral did not come from the decoder's own plan");
    TEST_ASSERT_FALSE_MESSAGE(klog_has("mid-copy"),
        "a deferral was reported as a failed copy");
    TEST_ASSERT_TRUE_MESSAGE(seq_installed(1U), "nothing may be evicted yet");
    TEST_ASSERT_TRUE_MESSAGE(kdrv_apps_pending_present(NULL, NULL),
                             "the mailbox must be left where it was");

    answer_stat(true, (uint8_t)KODE_OK);
    run_ticks(8U);
    TEST_ASSERT_TRUE_MESSAGE(klog_has("evicting 'old'"), "the second pass did not evict");
    /* The fake inflater yields no image: the adoption fails before its write
     * erases a block, and "old" comes back with the failure. */
    TEST_ASSERT_TRUE_MESSAGE(seq_installed(1U),
        "the failed adoption did not give back the app its write never reached");
    TEST_ASSERT_TRUE(klog_has("kept 'old' after the failed install"));

    (void)kdrv_apps_pending_consume();   /* the fake inflater yields no image */
    kfw_abort();
    next_install_keeps();
    keep_end();
}

/* ---- What the copies leave behind --------------------------------------------
 */

/* A write that fails may leave its file (a close that failed on a counterfeit
 * card does): the next round would STAT it, find it, and call the app kept. */
TEST_CASE("a copy whose write failed is unlinked, so no later round calls it kept",
          "[appmgr_ao][keep]")
{
    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();
    (void)begin_install((uint8_t)KDRV_MEDIUM_FS, "new");
    storage_expect(STORAGE_PATH_REQ_SIG, "the victim's path was never asked about");
    answer_stat(false, (uint8_t)KODE_OK);
    const QEvt *wr = storage_wait_for(STORAGE_WRITE_REQ_SIG, 1000U);
    TEST_ASSERT_NOT_NULL_MESSAGE(wr, "the copy never reached AO_Storage");
    QF_gc(wr);

    answer_write((uint8_t)KODE_ERROR, 0U);
    path_req_expect((uint8_t)STORAGE_PATH_UNLINK, "/sdcard/old.bin",
                    "the stub of the failed write was left on the card");
    (void)qf_support_pump(AO_AppMgr, 8U);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install did not start again");

    /* The unlink's answer reaches the restarted install and means nothing. */
    answer_path((uint8_t)STORAGE_PATH_UNLINK, (uint8_t)KODE_OK, false, false);
    TEST_ASSERT_TRUE(old_installed());
    let_the_open_expire();
    keep_end();
}

/* The unlink is asked ahead of the next victim's STAT and answered ahead of
 * it, and its answer is a path answer like the STAT's: it must not be taken
 * for the STAT's, or the next victim would be copied on the strength of an
 * answer about something else. */
TEST_CASE("an unlink's answer settles nothing while the next victim's STAT is out",
          "[appmgr_ao][keep]")
{
    uint32_t const mb  = 0x100000U;
    uint32_t const len = image_named("new");

    Mockkdrv_slot_Init();
    keep_fresh();
    three_apps();
    ask_flash_dst("new.bin", (uint8_t)KFW_TARGET_APP);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install never asked for its file");
    answer_open(5U * mb, (uint8_t)KDRV_MEDIUM_FS);
    answer_data(len);
    path_req_expect((uint8_t)STORAGE_PATH_STAT, "/sdcard/first.bin",
                    "the first victim was not looked for");
    answer_stat(false, (uint8_t)KODE_OK);
    const QEvt *wr = storage_wait_for(STORAGE_WRITE_REQ_SIG, 1000U);
    TEST_ASSERT_NOT_NULL_MESSAGE(wr, "the first victim was not copied");
    QF_gc(wr);

    answer_write((uint8_t)KODE_ERROR, 0U);
    path_req_expect((uint8_t)STORAGE_PATH_UNLINK, "/sdcard/first.bin",
                    "the stub of the failed write was left on the card");
    path_req_expect((uint8_t)STORAGE_PATH_STAT, "/sdcard/second.bin",
                    "the round did not go on to the second victim");

    /* An answer that says "not there", as a STAT's would: if it settled the
     * STAT the second victim would be read out and written. */
    answer_path((uint8_t)STORAGE_PATH_UNLINK, (uint8_t)KODE_OK, false, false);
    TEST_ASSERT_NULL_MESSAGE(storage_wait_for(STORAGE_WRITE_REQ_SIG, 150U),
        "an unlink's answer was taken for the second victim's STAT");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_tally().open,
        "an unlink's answer ended the round with a STAT still out");

    answer_stat(true, (uint8_t)KODE_OK);         /* the round ends properly */
    (void)qf_support_pump(AO_AppMgr, 8U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, storage_tally().open,
        "the install did not start again");
    let_the_open_expire();
    keep_end();
}

/* A folder named like the copy is not a copy, and nothing can be written over
 * it: the victim is a failed one, and no write is asked for. */
TEST_CASE("a folder where the copy would go is a failed copy, not a kept one",
          "[appmgr_ao][keep]")
{
    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();
    uint32_t const len = begin_install((uint8_t)KDRV_MEDIUM_FS, "new");
    storage_expect(STORAGE_PATH_REQ_SIG, "the victim's path was never asked about");

    answer_path((uint8_t)STORAGE_PATH_STAT, (uint8_t)KODE_OK, true, true);
    (void)qf_support_pump(AO_AppMgr, 8U);

    storage_seen_t const seen = storage_tally();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, seen.write,
        "a copy was written over a folder");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, seen.path,
        "a folder was unlinked or asked about again");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, seen.open,
        "the install did not start again once the victim was settled");
    TEST_ASSERT_FALSE_MESSAGE(klog_has("is already on the card"),
        "a folder was counted as the app kept");
    TEST_ASSERT_TRUE(old_installed());

    answer_open(len, (uint8_t)KDRV_MEDIUM_FS);   /* the second pass */
    answer_data(len);
    TEST_ASSERT_TRUE_MESSAGE(klog_has("evicting 'old' to make room; no copy kept"),
        "the victim went without a word");
    answer_eof();
    run_ticks(4U);
    keep_end();
}

/* ---- The second plan --------------------------------------------------------
 */

/* One round per install. The file is not the same size the second time (it was
 * replaced meanwhile), so the second plan names a second victim the round did
 * not know: it goes uncopied, with the warning, and the install is not
 * deferred again. */
TEST_CASE("a second plan that names a new victim evicts it uncopied, with a warning",
          "[appmgr_ao][keep]")
{
    uint32_t const mb  = 0x100000U;
    uint32_t const len = image_named("new");

    Mockkdrv_slot_Init();
    keep_fresh();
    three_apps();
    ask_flash_dst("new.bin", (uint8_t)KFW_TARGET_APP);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install never asked for its file");
    answer_open(len, (uint8_t)KDRV_MEDIUM_FS);    /* small: "first" is enough */
    answer_data(len);
    path_req_expect((uint8_t)STORAGE_PATH_STAT, "/sdcard/first.bin",
                    "the first victim was not looked for");
    answer_stat(false, (uint8_t)KODE_OK);
    const QEvt *wr = storage_wait_for(STORAGE_WRITE_REQ_SIG, 1000U);
    TEST_ASSERT_NOT_NULL_MESSAGE(wr, "the first victim was not copied");
    QF_gc(wr);
    answer_write((uint8_t)KODE_OK, 3U * mb);
    (void)qf_support_pump(AO_AppMgr, 8U);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install did not start again");

    answer_open(5U * mb, (uint8_t)KDRV_MEDIUM_FS); /* now it needs "second" too */
    answer_data(len);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_tally().path,
        "the second plan started a second round");
    TEST_ASSERT_TRUE(klog_has("evicting 'second'"));
    TEST_ASSERT_TRUE_MESSAGE(klog_has("evicting 'second' to make room; no copy kept"),
        "the new victim went without a word");
    TEST_ASSERT_FALSE_MESSAGE(klog_has("evicting 'first' to make room; no copy kept"),
        "the victim the card took was reported as lost");
    TEST_ASSERT_FALSE(seq_installed(1U));
    TEST_ASSERT_FALSE(seq_installed(2U));
    TEST_ASSERT_TRUE(seq_installed(3U));

    answer_eof();
    run_ticks(4U);
    keep_end();
}

/* The directory is full of other names and the install is a new one, so it
 * takes a slot: the oldest app is copied with the round and evicted by
 * appdir_make_room() once the image is written, without a warning. */
TEST_CASE("a slot victim without a namesake is copied, then evicted after the write",
          "[appmgr_ao][keep]")
{
    Mockkdrv_slot_Init();
    keep_fresh();
    region_of_tiny_apps();
    uint32_t const len = begin_install((uint8_t)KDRV_MEDIUM_FS, "zz");

    path_req_expect((uint8_t)STORAGE_PATH_STAT, "/sdcard/a00.bin",
                    "the slot's victim was not looked for");
    answer_stat(false, (uint8_t)KODE_OK);
    const QEvt *wr = storage_wait_for(STORAGE_WRITE_REQ_SIG, 100U);
    TEST_ASSERT_NOT_NULL_MESSAGE(wr, "the slot's victim was not copied");
    TEST_ASSERT_EQUAL_STRING("/sdcard/a00.bin", ((StorageWriteEvt const *)wr)->name);
    QF_gc(wr);
    TEST_ASSERT_TRUE_MESSAGE(seq_installed(1U), "evicted before its copy landed");
    answer_write((uint8_t)KODE_OK, 4096U);
    (void)qf_support_pump(AO_AppMgr, 8U);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install did not start again");
    TEST_ASSERT_TRUE_MESSAGE(seq_installed(1U), "evicted before the image was written");

    answer_open(len, (uint8_t)KDRV_MEDIUM_FS);
    answer_data(len);
    answer_eof();
    run_ticks(4U);

    TEST_ASSERT_FALSE_MESSAGE(seq_installed(1U),
        "the oldest app was not evicted for the new one's slot");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(KAPPS_MAX_ENTRIES, kdrv_apps_dir()->count,
        "the new app did not take the freed slot");
    TEST_ASSERT_TRUE(klog_has("evicting 'a00'"));
    TEST_ASSERT_TRUE(klog_has("'a00' kept as /sdcard/a00.bin"));
    TEST_ASSERT_FALSE_MESSAGE(klog_has("no copy kept"),
        "a victim that was copied went with a warning");
    keep_end();
}

/* ---- A heartbeat that outlives its round ------------------------------------
 */

/**
 * @brief Park a bare image of @p data_len bytes of segment in the mailbox:
 *        more than one adoption chunk when @p data_len is over 4096.
 *
 * @param project  The project name the descriptor carries.
 * @param data_len The one segment's length, at least a descriptor.
 */
static void park_long_image(const char *project, uint32_t data_len)
{
    static uint8_t             img[8192];
    esp_image_segment_header_t seg;
    uint32_t                   len;

    (void)image_named(project);
    (void)memset(img, 0, sizeof(img));
    (void)memcpy(img, s_named_img, sizeof(s_named_img));
    (void)memset(&seg, 0, sizeof(seg));
    seg.load_addr = 0x40000000U;
    seg.data_len  = data_len;
    (void)memcpy(&img[sizeof(esp_image_header_t)], &seg, sizeof(seg));
    len = (uint32_t)(sizeof(esp_image_header_t) + sizeof(seg)) + data_len;
    len = (len | 15U) + 1U;
    TEST_ASSERT_TRUE((uint32_t)sizeof(img) >= len);

    kdrv_apps_pending_write_abort();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_write_begin(len));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_write(img, len));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_write_end());
}

/* The last copy's answer posts the adoption to the front of the queue, and a
 * heartbeat the pacer had already queued is left behind it: it reaches the
 * adopting state after the adoption armed the pacer. Arming an armed timer is
 * a QP assertion, and an assertion here is a board reset. */
TEST_CASE("a heartbeat queued behind the last answer reaches the adoption safely",
          "[appmgr_ao][keep]")
{
    static const QEvt adopt = QEVT_INITIALIZER(APP_ADOPT_SIG);
    static const QEvt beat  = QEVT_INITIALIZER(APP_FLASH_CHUNK_SIG);
    uint32_t          off   = 0U;
    uint32_t          size  = 0U;
    kboot_run_mode_t  mode  = KBOOT_RUN_ONCE;

    Mockkdrv_slot_Init();
    keep_fresh();
    (void)boot_run_request(&off, &size, &mode);
    region_full_of_old();
    park_long_image("fresh", 6000U);

    QASM_DISPATCH(AO_AppMgr, &adopt, 0U);
    storage_expect(STORAGE_PATH_REQ_SIG, "the victim's path was never asked about");
    answer_stat(false, (uint8_t)KODE_OK);
    const QEvt *wr = storage_wait_for(STORAGE_WRITE_REQ_SIG, 1000U);
    TEST_ASSERT_NOT_NULL_MESSAGE(wr, "the copy never reached AO_Storage");
    QF_gc(wr);

    answer_write((uint8_t)KODE_OK, old_bytes());   /* the adopt goes in front */
    QACTIVE_POST(AO_AppMgr, &beat, NULL);          /* the heartbeat, behind it */
    run_ticks(8U);

    TEST_ASSERT_TRUE_MESSAGE(boot_run_request(&off, &size, &mode),
        "the adoption did not end in a boot request");
    TEST_ASSERT_FALSE(old_installed());
    keep_end();
}

/* ---- Asking what an install would evict -------------------------------------
 *
 * A question about the next install, answered to the asker alone: the apps the
 * install would remove, in the order they would go, each with whether its copy
 * is already on the card, and an end that counts them. The spy is the asker;
 * AO_Storage's queue is where the STATs go and these cases answer them. It is
 * the same region and the same cleanup as the keep cases above, and for the
 * same reason: a case ends with nothing armed.
 */

/** @brief The answers an asker has collected so far. */
typedef struct {
    unsigned       entries;                          /**< APP_EVICT_ENTRY_SIG.  */
    char           name[KODE_APP_EVICT_MAX + 1U][KODE_APP_PROJECT_MAX];
    bool           on_card[KODE_APP_EVICT_MAX + 1U];
    unsigned       ends;                             /**< APP_EVICT_END_SIG.    */
    AppEvictEndEvt end;                              /**< The last end.         */
    AppEvictEndEvt first_end;                        /**< The first end.        */
} evict_answer_t;

/**
 * @brief Put the question: what would an install of @p size bytes, whose
 *        project is called @p project, evict?
 * @param size    Bytes the image takes in the region.
 * @param project Its project name; "" when it is not known.
 */
static void ask_evict_as(uint32_t size, const char *project)
{
    AppEvictReqEvt q;

    (void)memset(&q, 0, sizeof(q));
    q.super = (QEvt)QEVT_INITIALIZER(APP_EVICT_REQ_SIG);
    q.size  = size;
    q.who   = ao_spy();
    (void)snprintf(q.project, sizeof(q.project), "%s", project);
    QASM_DISPATCH(AO_AppMgr, &q.super, 0U);
}

/** @brief Put the question with no project name: what would @p size evict? */
static void ask_evict(uint32_t size)
{
    ask_evict_as(size, "");
}

/** @brief Take what the asker has been told since the last call, adding to @p a. */
static void collect_answer(evict_answer_t *a)
{
    for ( const QEvt *e = ao_spy_take(); NULL != e; e = ao_spy_take() )
    {
        if ( (int)e->sig == APP_EVICT_ENTRY_SIG )
        {
            AppEvictEntryEvt const * const en = (AppEvictEntryEvt const *)e;

            TEST_ASSERT_TRUE(KODE_APP_EVICT_MAX >= a->entries);
            (void)snprintf(a->name[a->entries], sizeof(a->name[0]), "%s", en->name);
            a->on_card[a->entries] = en->on_card;
            ++a->entries;
        }
        else if ( (int)e->sig == APP_EVICT_END_SIG )
        {
            if ( 0U == a->ends )
            {
                a->first_end = *(AppEvictEndEvt const *)e;
            }
            a->end = *(AppEvictEndEvt const *)e;
            ++a->ends;
        }
        else
        {
            TEST_FAIL_MESSAGE("the asker was told something that is no part of an answer");
        }
    }
}

/** @brief Take AO_Storage's next STAT and check it asks about @p path. */
static void stat_expect(const char *path, const char *what)
{
    path_req_expect((uint8_t)STORAGE_PATH_STAT, path, what);
}

/** @brief An empty directory; the one it replaces is remembered for keep_end(). */
static void empty_region(void)
{
    keep_hold_dir();
    kapps_dir_t dir = s_keep_dir;

    dir.count = 0U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));
}

/** @brief Is the directory the one @p before was, down to its generation? */
static bool dir_is(const kapps_dir_t *before)
{
    kapps_dir_t const *now = kdrv_apps_dir();

    if ( (now->gen != before->gen) || (now->count != before->count) )
    {
        return false;
    }
    for ( uint32_t i = 0U; i < before->count; ++i )
    {
        if ( (now->e[i].offset != before->e[i].offset)
            || (now->e[i].size != before->e[i].size)
            || (now->e[i].seq != before->e[i].seq)
            || (now->e[i].flags != before->e[i].flags) )
        {
            return false;
        }
    }
    return true;
}

TEST_CASE("an install that needs no room evicts nothing, and the answer is the "
          "end alone", "[appmgr_ao][evict]")
{
    evict_answer_t a;

    (void)memset(&a, 0, sizeof(a));
    Mockkdrv_slot_Init();
    keep_fresh();
    empty_region();

    ask_evict(0x100000U);
    collect_answer(&a);

    TEST_ASSERT_EQUAL_UINT(0U, a.entries);
    TEST_ASSERT_EQUAL_UINT(1U, a.ends);
    TEST_ASSERT_EQUAL_UINT8(0U, a.end.victims);
    TEST_ASSERT_TRUE(a.end.fits);
    TEST_ASSERT_FALSE(a.end.busy);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_tally().path,
        "the card was asked about an app nobody would remove");
    keep_end();
}

TEST_CASE("the oldest app is the one an install is told it would evict",
          "[appmgr_ao][evict]")
{
    evict_answer_t a;

    (void)memset(&a, 0, sizeof(a));
    Mockkdrv_slot_Init();
    keep_fresh();
    three_apps();                                  /* first, second, tail: full */

    ask_evict(2U * 0x100000U);                     /* "first" alone frees 3 MB */
    stat_expect("/sdcard/first.bin", "the oldest app's copy was not looked for");
    answer_stat(false, (uint8_t)KODE_OK);
    collect_answer(&a);

    TEST_ASSERT_EQUAL_UINT(1U, a.entries);
    TEST_ASSERT_EQUAL_STRING("first", a.name[0]);
    TEST_ASSERT_FALSE(a.on_card[0]);
    TEST_ASSERT_EQUAL_UINT(1U, a.ends);
    TEST_ASSERT_EQUAL_UINT8(1U, a.end.victims);
    TEST_ASSERT_TRUE(a.end.fits);
    TEST_ASSERT_FALSE(a.end.busy);
    keep_end();
}

TEST_CASE("two victims come in the order they would go, one with a copy on "
          "the card and one without", "[appmgr_ao][evict]")
{
    evict_answer_t a;

    (void)memset(&a, 0, sizeof(a));
    Mockkdrv_slot_Init();
    keep_fresh();
    three_apps();

    ask_evict(5U * 0x100000U);                     /* "first" and "second" go */
    stat_expect("/sdcard/first.bin", "the first victim's copy was not looked for");
    collect_answer(&a);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, a.entries,
        "an app was reported before the card said whether it had a copy");
    answer_stat(true, (uint8_t)KODE_OK);
    stat_expect("/sdcard/second.bin", "the second victim's copy was not looked for");
    answer_stat(false, (uint8_t)KODE_OK);
    collect_answer(&a);

    TEST_ASSERT_EQUAL_UINT(2U, a.entries);
    TEST_ASSERT_EQUAL_STRING("first", a.name[0]);
    TEST_ASSERT_TRUE(a.on_card[0]);
    TEST_ASSERT_EQUAL_STRING("second", a.name[1]);
    TEST_ASSERT_FALSE(a.on_card[1]);
    TEST_ASSERT_EQUAL_UINT(1U, a.ends);
    TEST_ASSERT_EQUAL_UINT8(2U, a.end.victims);
    TEST_ASSERT_TRUE(a.end.fits);
    TEST_ASSERT_FALSE(a.end.busy);
    keep_end();
}

TEST_CASE("an install bigger than the region does not fit, and names no victim",
          "[appmgr_ao][evict]")
{
    evict_answer_t a;

    (void)memset(&a, 0, sizeof(a));
    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();

    ask_evict(old_bytes() + 1U);
    ask_evict(0U);                                 /* nothing to place either */
    collect_answer(&a);

    TEST_ASSERT_EQUAL_UINT(0U, a.entries);
    TEST_ASSERT_EQUAL_UINT(2U, a.ends);
    TEST_ASSERT_EQUAL_UINT8(0U, a.first_end.victims);
    TEST_ASSERT_FALSE(a.first_end.fits);
    TEST_ASSERT_FALSE(a.first_end.busy);
    TEST_ASSERT_FALSE(a.end.fits);
    TEST_ASSERT_FALSE(a.end.busy);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_tally().path,
        "the card was asked about an app no install could make room for");
    TEST_ASSERT_TRUE_MESSAGE(old_installed(), "an app was evicted for an image "
        "that does not fit");
    keep_end();
}

TEST_CASE("a card that is absent, fails, or holds a folder there reads as no "
          "copy", "[appmgr_ao][evict]")
{
    static const struct {
        uint8_t rc;
        bool    exists;
        bool    is_dir;
        bool    on_card;
    } rows[] = {
        { (uint8_t)KODE_OK,     true,  false, true  },   /* the file is there  */
        { (uint8_t)KODE_OK,     false, false, false },   /* nothing there      */
        { (uint8_t)KODE_ERROR,  false, false, false },   /* no card            */
        { (uint8_t)KODE_ERROR,  true,  false, false },   /* it did not say     */
        { (uint8_t)KODE_OK,     true,  true,  false },   /* a folder, no copy  */
    };

    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();

    for ( unsigned i = 0U; (sizeof(rows) / sizeof(rows[0])) > i; ++i )
    {
        evict_answer_t a;

        (void)memset(&a, 0, sizeof(a));
        ask_evict(0x10000U);
        stat_expect("/sdcard/old.bin", "the victim's copy was not looked for");
        answer_path((uint8_t)STORAGE_PATH_STAT, rows[i].rc, rows[i].exists,
                    rows[i].is_dir);
        collect_answer(&a);

        TEST_ASSERT_EQUAL_UINT(1U, a.entries);
        TEST_ASSERT_EQUAL_STRING("old", a.name[0]);
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)rows[i].on_card, (int)a.on_card[0],
            "the card's answer was read wrongly");
        TEST_ASSERT_EQUAL_UINT8(1U, a.end.victims);
        ao_spy_release();
    }
    keep_end();
}

/* The answer to a question never evicts, writes the directory or starts
 * keeping: AO_AppMgr does the install that follows, and it asks the same
 * plan again then. */
TEST_CASE("a question changes nothing: not the directory, not the card, not "
          "the apps", "[appmgr_ao][evict]")
{
    evict_answer_t a;
    kapps_dir_t    before;
    uint32_t       off  = 0U;
    uint32_t       size = 0U;
    kboot_run_mode_t mode = KBOOT_RUN_ONCE;

    (void)memset(&a, 0, sizeof(a));
    Mockkdrv_slot_Init();
    keep_fresh();
    three_apps();
    before = *kdrv_apps_dir();
    (void)boot_run_request(&off, &size, &mode);

    ask_evict(5U * 0x100000U);
    stat_expect("/sdcard/first.bin", "the first victim's copy was not looked for");
    answer_stat(false, (uint8_t)KODE_OK);
    stat_expect("/sdcard/second.bin", "the second victim's copy was not looked for");
    answer_stat(false, (uint8_t)KODE_OK);
    run_ticks(50U);                                /* all the time it could take */
    collect_answer(&a);

    TEST_ASSERT_EQUAL_UINT(2U, a.entries);
    TEST_ASSERT_TRUE_MESSAGE(dir_is(&before), "the directory moved");
    storage_seen_t const seen = storage_tally();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, seen.write, "a copy was started");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, seen.open, "an install was started");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, seen.path, "the card was asked more than "
        "once about a victim");
    TEST_ASSERT_FALSE_MESSAGE(klog_has("evicting"), "something was evicted");
    TEST_ASSERT_FALSE_MESSAGE(klog_has("to keep on the card"),
        "the question started a round of copies");
    TEST_ASSERT_FALSE_MESSAGE(boot_run_request(&off, &size, &mode),
        "the question booted something");

    /* And idle again: the install it was a question about is taken. */
    ask_flash_dst("new.bin", (uint8_t)KFW_TARGET_APP);
    storage_expect(STORAGE_OPEN_REQ_SIG, "AO_AppMgr did not go back to idle");
    let_the_open_expire();
    keep_end();
}

/* The list is as long as the asker's queue can hold, and the end still counts
 * every app that would go. */
TEST_CASE("a long list is cut at the cap, and the end counts every victim",
          "[appmgr_ao][evict]")
{
    evict_answer_t a;

    (void)memset(&a, 0, sizeof(a));
    Mockkdrv_slot_Init();
    keep_fresh();
    region_of_tiny_apps();                         /* a00 .. a63, oldest first */

    ask_evict(old_bytes());                        /* all 64 have to go */
    for ( unsigned i = 0U; KODE_APP_EVICT_MAX > i; ++i )
    {
        char path[KODE_APP_FILE_MAX];

        (void)snprintf(path, sizeof(path), "/sdcard/a%02u.bin", i);
        stat_expect(path, "a listed victim's copy was not looked for");
        answer_stat((i % 2U) == 0U, (uint8_t)KODE_OK);
    }
    collect_answer(&a);

    TEST_ASSERT_EQUAL_UINT(KODE_APP_EVICT_MAX, a.entries);
    TEST_ASSERT_EQUAL_STRING("a00", a.name[0]);
    TEST_ASSERT_TRUE(a.on_card[0]);
    TEST_ASSERT_EQUAL_STRING("a07", a.name[KODE_APP_EVICT_MAX - 1U]);
    TEST_ASSERT_FALSE(a.on_card[KODE_APP_EVICT_MAX - 1U]);
    TEST_ASSERT_EQUAL_UINT(1U, a.ends);
    TEST_ASSERT_EQUAL_UINT8(KAPPS_MAX_ENTRIES, a.end.victims);
    TEST_ASSERT_TRUE(a.end.fits);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_tally().path,
        "the card was asked about a victim past the cap");
    keep_end();
}

/* A directory with every slot taken costs the oldest app its place whatever
 * the bytes say. */
TEST_CASE("a full directory names the oldest app though the bytes would fit",
          "[appmgr_ao][evict]")
{
    evict_answer_t a;

    (void)memset(&a, 0, sizeof(a));
    Mockkdrv_slot_Init();
    keep_fresh();
    region_of_tiny_apps();

    ask_evict(4096U);
    stat_expect("/sdcard/a00.bin", "the slot's victim was not looked for");
    answer_stat(false, (uint8_t)KODE_OK);
    collect_answer(&a);

    TEST_ASSERT_EQUAL_UINT(1U, a.entries);
    TEST_ASSERT_EQUAL_STRING("a00", a.name[0]);
    TEST_ASSERT_EQUAL_UINT8(1U, a.end.victims);
    TEST_ASSERT_TRUE(a.end.fits);
    keep_end();
}

/* The card can lose the STAT as it can lose any request. The victim reads as
 * having no copy, the answer still ends, and when the answer finally turns up
 * it is not taken for the next question's. */
TEST_CASE("a STAT nobody answers is no copy, and its late answer is not "
          "the next question's", "[appmgr_ao][evict]")
{
    evict_answer_t a;

    (void)memset(&a, 0, sizeof(a));
    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();

    ask_evict(0x10000U);
    stat_expect("/sdcard/old.bin", "the victim's copy was not looked for");
    run_ticks(APPMGR_OPEN_DEADLINE_TICKS + 50U);
    collect_answer(&a);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, a.entries, "the answer never came");
    TEST_ASSERT_FALSE(a.on_card[0]);
    TEST_ASSERT_EQUAL_UINT(1U, a.ends);
    TEST_ASSERT_EQUAL_UINT8(1U, a.end.victims);

    (void)memset(&a, 0, sizeof(a));
    ask_evict(0x10000U);                           /* idle again: not busy */
    stat_expect("/sdcard/old.bin", "the second question never asked the card");
    answer_stat(true, (uint8_t)KODE_OK);           /* the first STAT's, late */
    collect_answer(&a);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, a.entries,
        "a late answer was taken for the next victim's");
    answer_stat(false, (uint8_t)KODE_OK);          /* the second STAT's own */
    collect_answer(&a);
    TEST_ASSERT_EQUAL_UINT(1U, a.entries);
    TEST_ASSERT_FALSE(a.on_card[0]);
    TEST_ASSERT_FALSE(a.end.busy);
    keep_end();
}

/* ---- Busy ------------------------------------------------------------------
 * AppMgr_busy is the parent of every state with work in hand. A question put
 * to one is answered at once with the end alone, never kept: a launcher that
 * asks again when it is ready gets the real answer.
 */

/** @brief The question, asked of an AO that is busy: the end alone, `busy`. */
static void busy_expect(const char *state)
{
    evict_answer_t a;

    (void)memset(&a, 0, sizeof(a));
    ask_evict(0x10000U);
    collect_answer(&a);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, a.entries, state);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, a.ends, state);
    TEST_ASSERT_TRUE_MESSAGE(a.end.busy, state);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, a.end.victims, state);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_tally().path, state);
}

TEST_CASE("a question while an install waits for its file is answered busy",
          "[appmgr_ao][evict]")
{
    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();

    ask_flash_dst("new.bin", (uint8_t)KFW_TARGET_APP);   /* AppMgr_opening */
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install never asked for its file");
    busy_expect("opening");

    let_the_open_expire();
    keep_end();
}

TEST_CASE("a question while an image is being written is answered busy",
          "[appmgr_ao][evict]")
{
    StorageDataEvt d;
    uint32_t const len = image_named("new");

    Mockkdrv_slot_Init();
    keep_fresh();
    empty_region();

    ask_flash_dst("new.bin", (uint8_t)KFW_TARGET_APP);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install never asked for its file");
    answer_open(len, (uint8_t)KDRV_MEDIUM_FS);            /* AppMgr_flashing */
    (void)storage_tally();                                /* its two reads */
    busy_expect("flashing");

    /* The file fails to read: the install ends and the AO is idle again. */
    (void)memset(&d, 0, sizeof(d));
    d.super  = (QEvt)QEVT_INITIALIZER(STORAGE_DATA_SIG);
    d.status = (uint8_t)KODE_ERROR;
    QASM_DISPATCH(AO_AppMgr, &d.super, 0U);
    keep_end();
}

TEST_CASE("a question while apps are being kept on the card is answered busy",
          "[appmgr_ao][evict]")
{
    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();
    (void)begin_install((uint8_t)KDRV_MEDIUM_FS, "new");  /* AppMgr_keeping */
    storage_expect(STORAGE_PATH_REQ_SIG, "the victim's path was never asked about");
    busy_expect("keeping");

    answer_stat(true, (uint8_t)KODE_OK);
    run_ticks(50U);
    (void)storage_tally();
    TEST_ASSERT_TRUE_MESSAGE(old_installed(), "the question moved the install on");
    let_the_open_expire();
    keep_end();
}

TEST_CASE("a question while an image arrives on the console is answered busy",
          "[appmgr_ao][evict]")
{
    Mockkdrv_slot_Init();
    keep_fresh();
    empty_region();

    ask_recv(1024U, 0U, false);                           /* AppMgr_receiving */
    busy_expect("receiving");

    raw_armed(false);                                     /* the console says no */
    (void)log_saw(CONSOLE_RAW_REQ_SIG);
    keep_end();
}

TEST_CASE("a second question while the card is being asked is answered busy, "
          "and the first is still answered", "[appmgr_ao][evict]")
{
    evict_answer_t a;

    (void)memset(&a, 0, sizeof(a));
    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();

    ask_evict(0x10000U);                                  /* AppMgr_asking */
    stat_expect("/sdcard/old.bin", "the victim's copy was not looked for");
    ask_evict(0x10000U);
    collect_answer(&a);
    TEST_ASSERT_EQUAL_UINT(0U, a.entries);
    TEST_ASSERT_EQUAL_UINT(1U, a.ends);
    TEST_ASSERT_TRUE(a.end.busy);

    answer_stat(true, (uint8_t)KODE_OK);
    collect_answer(&a);
    TEST_ASSERT_EQUAL_UINT(1U, a.entries);
    TEST_ASSERT_EQUAL_STRING("old", a.name[0]);
    TEST_ASSERT_TRUE(a.on_card[0]);
    TEST_ASSERT_EQUAL_UINT(2U, a.ends);
    TEST_ASSERT_TRUE(a.first_end.busy);
    TEST_ASSERT_FALSE_MESSAGE(a.end.busy, "the first question was lost");
    TEST_ASSERT_EQUAL_UINT8(1U, a.end.victims);
    keep_end();
}

/* ---- The facade ------------------------------------------------------------- */

TEST_CASE("appmgr_evict_query posts the question to AO_AppMgr, which answers "
          "its asker", "[appmgr_ao][evict]")
{
    evict_answer_t a;

    (void)memset(&a, 0, sizeof(a));
    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();

    TEST_ASSERT_FALSE_MESSAGE(appmgr_evict_query(0x10000U, "other", NULL),
        "a question with no asker was posted");
    TEST_ASSERT_TRUE(appmgr_evict_query(0x10000U, NULL, ao_spy()));
    (void)qf_support_pump(AO_AppMgr, 8U);
    stat_expect("/sdcard/old.bin", "the facade's question never reached AO_AppMgr");
    answer_stat(false, (uint8_t)KODE_OK);
    collect_answer(&a);

    TEST_ASSERT_EQUAL_UINT(1U, a.entries);
    TEST_ASSERT_EQUAL_STRING("old", a.name[0]);
    TEST_ASSERT_EQUAL_UINT(1U, a.ends);

    /* The name travels with it: "old" is what the install replaces. */
    (void)memset(&a, 0, sizeof(a));
    TEST_ASSERT_TRUE(appmgr_evict_query(0x10000U, "old", ao_spy()));
    (void)qf_support_pump(AO_AppMgr, 8U);
    collect_answer(&a);
    TEST_ASSERT_EQUAL_UINT(0U, a.entries);
    TEST_ASSERT_EQUAL_UINT(1U, a.ends);
    TEST_ASSERT_EQUAL_UINT8(0U, a.end.victims);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_tally().path,
        "the card was asked about the app the install replaces");
    keep_end();
}

/* ---- What a listing carries of the build -------------------------------------- */

TEST_CASE("an installed app's entry carries the date and time it was built",
          "[appmgr_ao][evict]")
{
    AppListReqEvt list = {
        .super = QEVT_INITIALIZER(APP_LIST_REQ_SIG), .skip = 0U,
        .who = ao_spy() };
    bool seen = false;

    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();                        /* plant_app() builds it on "Jan  1 2026" */

    QASM_DISPATCH(AO_AppMgr, &list.super, 0U);

    for ( const QEvt *e = ao_spy_take(); NULL != e; e = ao_spy_take() )
    {
        if ( (int)e->sig == APP_ENTRY_SIG )
        {
            AppEntryEvt const * const en = (AppEntryEvt const *)e;

            TEST_ASSERT_EQUAL_STRING("old", en->name);
            TEST_ASSERT_EQUAL_STRING("Jan  1 2026", en->build_date);
            TEST_ASSERT_EQUAL_STRING("00:00:00", en->build_time);
            TEST_ASSERT_EQUAL_UINT16(0U, en->pos);
            seen = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(seen, "the listing never named the app");
    keep_end();
}

/* ---- A STAT the card never answers, in every state it can land in ------------
 * AO_Storage answers in order and its answers carry no path, so a STAT given
 * up on is a debt: the next answer of its kind is its late one. The debt is
 * one counter for keeping and for questions alike, and every state settles it.
 */

TEST_CASE("the late answer to a question's lost STAT is settled where it lands, "
          "and keeping's next answer counts", "[appmgr_ao][evict]")
{
    evict_answer_t a;
    uint32_t       len;

    (void)memset(&a, 0, sizeof(a));
    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();

    ask_evict(0x10000U);
    stat_expect("/sdcard/old.bin", "the victim's copy was not looked for");
    run_ticks(APPMGR_OPEN_DEADLINE_TICKS + 50U);           /* nobody answers */
    collect_answer(&a);
    TEST_ASSERT_EQUAL_UINT(1U, a.entries);
    TEST_ASSERT_EQUAL_UINT(1U, a.ends);

    /* An install is confirmed, and the card's answer to the lost STAT turns
     * up while AppMgr waits for the file to open. */
    len = image_named("new");
    ask_flash_dst("new.bin", (uint8_t)KFW_TARGET_APP);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install never asked for its file");
    answer_stat(true, (uint8_t)KODE_OK);
    answer_open(len, (uint8_t)KDRV_MEDIUM_FS);
    answer_data(len);                                       /* "old" must go */

    /* Keeping asks the card about "old" and its own answer is what comes
     * next: it counts. Eaten as the late one, the round would sit out its
     * whole deadline. */
    stat_expect("/sdcard/old.bin", "the victim's copy was not looked for");
    answer_stat(true, (uint8_t)KODE_OK);
    run_ticks(50U);
    TEST_ASSERT_TRUE_MESSAGE(klog_has("'old' is already on the card"),
        "keeping's answer was taken for the question's late one");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, storage_tally().open,
        "the install did not start again once its victim was kept");

    let_the_open_expire();
    keep_end();
}

TEST_CASE("the late answer to a lost copy's write is settled by the install "
          "that opens, and frees what AO_Storage held", "[appmgr_ao][keep]")
{
    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();
    (void)begin_install((uint8_t)KDRV_MEDIUM_FS, "new");
    storage_expect(STORAGE_PATH_REQ_SIG, "the victim's path was never asked about");
    answer_stat(false, (uint8_t)KODE_OK);
    const QEvt *wr = storage_wait_for(STORAGE_WRITE_REQ_SIG, 1000U);
    TEST_ASSERT_NOT_NULL_MESSAGE(wr, "the copy never reached AO_Storage");
    QF_gc(wr);

    /* The write is given up on and the install starts again: AppMgr waits
     * for its file to open, and the abandoned buffer is still held. */
    run_ticks(KEEP_WRITE_DEADLINE_TICKS(old_bytes()) + 50U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, storage_tally().open,
        "the round did not go on after the lost write");
    unsigned const frees = fake_psram_free_count();

    answer_write((uint8_t)KODE_OK, old_bytes());    /* late, and it lands in opening */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(frees + 1U, fake_psram_free_count(),
        "the abandoned buffer stayed held: the late answer was not settled");

    let_the_open_expire();
    keep_end();
}

TEST_CASE("once the card lets a STAT go, the victims still to report are not "
          "asked about", "[appmgr_ao][evict]")
{
    evict_answer_t a;

    (void)memset(&a, 0, sizeof(a));
    Mockkdrv_slot_Init();
    keep_fresh();
    three_apps();

    ask_evict(5U * 0x100000U);                              /* first, second */
    stat_expect("/sdcard/first.bin", "the first victim's copy was not looked for");
    run_ticks(APPMGR_OPEN_DEADLINE_TICKS + 50U);            /* no answer */
    collect_answer(&a);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, a.entries,
        "the victim still to report waited for a card that does not answer");
    TEST_ASSERT_EQUAL_STRING("first", a.name[0]);
    TEST_ASSERT_FALSE(a.on_card[0]);
    TEST_ASSERT_EQUAL_STRING("second", a.name[1]);
    TEST_ASSERT_FALSE(a.on_card[1]);
    TEST_ASSERT_EQUAL_UINT(1U, a.ends);
    TEST_ASSERT_EQUAL_UINT8(2U, a.end.victims);
    TEST_ASSERT_FALSE(a.end.busy);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_tally().path,
        "the card was asked about a second victim after it let one go");

    /* One debt, not two: the next question's first genuine answer comes
     * after exactly one late one. */
    (void)memset(&a, 0, sizeof(a));
    ask_evict(5U * 0x100000U);
    stat_expect("/sdcard/first.bin", "the first victim's copy was not looked for");
    answer_stat(false, (uint8_t)KODE_OK);                   /* the late one */
    collect_answer(&a);
    TEST_ASSERT_EQUAL_UINT(0U, a.entries);
    answer_stat(true, (uint8_t)KODE_OK);                    /* first's own */
    stat_expect("/sdcard/second.bin", "the second victim's copy was not looked for");
    answer_stat(false, (uint8_t)KODE_OK);
    collect_answer(&a);
    TEST_ASSERT_EQUAL_UINT(2U, a.entries);
    TEST_ASSERT_TRUE_MESSAGE(a.on_card[0],
        "a genuine answer was taken for a late one");
    TEST_ASSERT_FALSE(a.on_card[1]);
    keep_end();
}

/* ---- The install's own name ---------------------------------------------------
 * An install replaces the installed app of its name in one commit: that app is
 * no loss, and its slot is not taken from another.
 */

TEST_CASE("a same-name install that takes its own slot lists nobody; without "
          "the name the oldest is predicted", "[appmgr_ao][evict]")
{
    evict_answer_t a;

    (void)memset(&a, 0, sizeof(a));
    Mockkdrv_slot_Init();
    keep_fresh();
    region_of_tiny_apps();                         /* every slot is taken */

    ask_evict_as(4096U, "a10");                    /* replaces a10: no slot lost */
    collect_answer(&a);
    TEST_ASSERT_EQUAL_UINT(0U, a.entries);
    TEST_ASSERT_EQUAL_UINT(1U, a.ends);
    TEST_ASSERT_EQUAL_UINT8(0U, a.end.victims);
    TEST_ASSERT_TRUE(a.end.fits);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_tally().path,
        "the card was asked about an app nobody would remove");

    (void)memset(&a, 0, sizeof(a));
    ask_evict_as(4096U, "fresh");                  /* a new name: the oldest goes */
    stat_expect("/sdcard/a00.bin", "the slot's victim was not looked for");
    answer_stat(false, (uint8_t)KODE_OK);
    collect_answer(&a);
    TEST_ASSERT_EQUAL_UINT(1U, a.entries);
    TEST_ASSERT_EQUAL_STRING("a00", a.name[0]);
    TEST_ASSERT_EQUAL_UINT8(1U, a.end.victims);

    (void)memset(&a, 0, sizeof(a));
    ask_evict(4096U);                              /* no name: the prediction */
    stat_expect("/sdcard/a00.bin", "the slot's victim was not looked for");
    answer_stat(false, (uint8_t)KODE_OK);
    collect_answer(&a);
    TEST_ASSERT_EQUAL_UINT(1U, a.entries);
    TEST_ASSERT_EQUAL_STRING("a00", a.name[0]);
    TEST_ASSERT_EQUAL_UINT8(1U, a.end.victims);
    keep_end();
}

TEST_CASE("an app the install replaces is no victim of its own bytes either",
          "[appmgr_ao][evict]")
{
    evict_answer_t a;

    (void)memset(&a, 0, sizeof(a));
    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();                          /* "old" fills the region */

    ask_evict_as(0x10000U, "old");                 /* "old" replaces itself */
    collect_answer(&a);
    TEST_ASSERT_EQUAL_UINT(0U, a.entries);
    TEST_ASSERT_EQUAL_UINT8(0U, a.end.victims);
    TEST_ASSERT_TRUE(a.end.fits);
    TEST_ASSERT_EQUAL_UINT(0U, storage_tally().path);

    (void)memset(&a, 0, sizeof(a));
    ask_evict_as(0x10000U, "other");               /* another name: "old" goes */
    stat_expect("/sdcard/old.bin", "the victim's copy was not looked for");
    answer_stat(false, (uint8_t)KODE_OK);
    collect_answer(&a);
    TEST_ASSERT_EQUAL_UINT(1U, a.entries);
    TEST_ASSERT_EQUAL_STRING("old", a.name[0]);
    TEST_ASSERT_EQUAL_UINT8(1U, a.end.victims);
    keep_end();
}

TEST_CASE("the install's own app is left out of a list of several, and the "
          "others keep their place", "[appmgr_ao][evict]")
{
    evict_answer_t a;

    (void)memset(&a, 0, sizeof(a));
    Mockkdrv_slot_Init();
    keep_fresh();
    three_apps();                                  /* first, second, tail */

    ask_evict_as(5U * 0x100000U, "first");         /* first, second go for bytes */
    stat_expect("/sdcard/second.bin", "the other victim's copy was not looked for");
    answer_stat(true, (uint8_t)KODE_OK);
    collect_answer(&a);

    TEST_ASSERT_EQUAL_UINT(1U, a.entries);
    TEST_ASSERT_EQUAL_STRING("second", a.name[0]);
    TEST_ASSERT_TRUE(a.on_card[0]);
    TEST_ASSERT_EQUAL_UINT8(1U, a.end.victims);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_tally().path,
        "the card was asked about the app the install replaces");
    keep_end();
}

/* The planner frees a slot with every app it frees for bytes, so a plan names
 * the directory's own victim only when no app goes for bytes. */
TEST_CASE("a full directory that needs bytes freed names those apps and no extra "
          "one for the slot", "[appmgr_ao][evict]")
{
    evict_answer_t a;
    kapps_dir_t    dir;

    (void)memset(&a, 0, sizeof(a));
    Mockkdrv_slot_Init();
    keep_fresh();
    keep_hold_dir();
    dir = s_keep_dir;
    dir.count = KAPPS_MAX_ENTRIES;
    for ( uint32_t i = 0U; KAPPS_MAX_ENTRIES > i; ++i )
    {
        char name[8];

        (void)snprintf(name, sizeof(name), "a%02u", (unsigned)i);
        dir.e[i].offset = KAPPS_DATA_OFF + (i * KAPPS_ALIGN);
        dir.e[i].size   = 4096U;
        dir.e[i].seq    = i + 1U;
        dir.e[i].flags  = KAPPS_ENTRY_VALID;
        if ( (KAPPS_MAX_ENTRIES - 1U) == i )
        {
            dir.e[i].size = kdrv_apps_region_size() - dir.e[i].offset;
        }
        plant_app(dir.e[i].offset, name);
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));

    ask_evict(KAPPS_ALIGN);                        /* no gap: a00 goes for bytes */
    stat_expect("/sdcard/a00.bin", "the victim's copy was not looked for");
    answer_stat(false, (uint8_t)KODE_OK);
    collect_answer(&a);

    TEST_ASSERT_EQUAL_UINT(1U, a.entries);
    TEST_ASSERT_EQUAL_STRING("a00", a.name[0]);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1U, a.end.victims,
        "the slot's victim was named as well as the one freed for bytes");
    TEST_ASSERT_EQUAL_UINT(0U, storage_tally().path);
    keep_end();
}

/* ---- A victim whose image cannot be read --------------------------------------- */

TEST_CASE("a victim whose image cannot be read is named by where it lies, with "
          "no copy", "[appmgr_ao][evict]")
{
    evict_answer_t a;
    kapps_dir_t    dir;
    char           label[16];

    (void)memset(&a, 0, sizeof(a));
    Mockkdrv_slot_Init();
    keep_fresh();
    keep_hold_dir();
    dir = s_keep_dir;
    dir.count       = 1U;
    dir.e[0].offset = KAPPS_DATA_OFF;
    dir.e[0].size   = old_bytes();
    dir.e[0].seq    = 1U;
    dir.e[0].flags  = KAPPS_ENTRY_VALID;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_erase_header(KAPPS_DATA_OFF));

    ask_evict(0x10000U);
    collect_answer(&a);

    (void)snprintf(label, sizeof(label), "@0x%06lx", (unsigned long)KAPPS_DATA_OFF);
    TEST_ASSERT_EQUAL_UINT(1U, a.entries);
    TEST_ASSERT_EQUAL_STRING(label, a.name[0]);
    TEST_ASSERT_FALSE(a.on_card[0]);
    TEST_ASSERT_EQUAL_UINT(1U, a.ends);
    TEST_ASSERT_EQUAL_UINT8(1U, a.end.victims);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_tally().path,
        "the card was asked about an app with no name to look for");
    keep_end();
}

/* ---- Busy in the two states the other cases do not reach ------------------------ */

TEST_CASE("a question while a mailbox app is being adopted is answered busy",
          "[appmgr_ao][evict]")
{
    static const QEvt adopt = QEVT_INITIALIZER(APP_ADOPT_SIG);
    uint32_t          off   = 0U;
    uint32_t          size  = 0U;
    kboot_run_mode_t  mode  = KBOOT_RUN_ONCE;

    Mockkdrv_slot_Init();
    keep_fresh();
    (void)boot_run_request(&off, &size, &mode);
    empty_region();
    (void)park_pending("adopted");

    QASM_DISPATCH(AO_AppMgr, &adopt, 0U);                  /* AppMgr_adopting */
    busy_expect("adopting");

    run_ticks(4U);                                         /* the copy ends */
    TEST_ASSERT_TRUE_MESSAGE(boot_run_request(&off, &size, &mode),
        "the question stopped the adoption");
    TEST_ASSERT_FALSE(kdrv_apps_pending_present(NULL, NULL));
    keep_end();
}

TEST_CASE("a question in the pause between an install and its launch is "
          "answered busy", "[appmgr_ao][evict]")
{
    uint32_t         off  = 0U;
    uint32_t         size = 0U;
    kboot_run_mode_t mode = KBOOT_RUN_ONCE;
    uint32_t         len;

    Mockkdrv_slot_Init();
    keep_fresh();
    (void)boot_run_request(&off, &size, &mode);
    empty_region();

    len = image_named("launched");
    ask_recv(len, khal_crc32(KHAL_CRC32_INIT, s_named_img, len), true);
    raw_armed(true);
    feed(s_named_img, (uint16_t)len);                      /* AppMgr_launching */
    ao_spy_drain();             /* what the install published to the spy's subscriptions */
    busy_expect("launching");

    run_ticks(APPMGR_ANSWER_DRAIN_DEADLINE_TICKS + 2U);
    TEST_ASSERT_TRUE_MESSAGE(boot_run_request(&off, &size, &mode),
        "the question stopped the launch");
    keep_end();
}

/* ---- The board's rest ------------------------------------------------------- */

/* AppMgr owns no device that sleeps; it owns work that must not be cut. */
TEST_CASE("an idle AppMgr is ready for the round and a writing one is busy",
          "[appmgr_ao][lowpower]")
{
    Mockkdrv_slot_Init();
    appmgr_fresh();

    lp_round(AO_AppMgr, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();

    ask_flash("big.bin");                     /* the image is on its way in */
    lp_round(AO_AppMgr, KODE_LP_OFF);
    lp_expect_answer(true, "app write");
    lp_expect_no_more();

    run_ticks(APPMGR_OPEN_DEADLINE_TICKS + 50U);   /* the open gives up: idle again */
    lp_round(AO_AppMgr, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    Mockkdrv_slot_Destroy();
}

/* ---- Tidying: the app's folder is in order before the install ends ----------
 *
 * An install of a container ends in the directory's commit and then in a
 * conversation with AO_Storage: the folder of the installed app is written
 * (a block), deleted (a container without one) or left alone (a bare
 * image), and the folders of the apps the install evicted without a copy on
 * the card are deleted. Only after the last answer - or its deadline - does
 * the install end: the done, the launch, the console's `>ok`, the boot
 * request. These cases are AO_Storage's voice, as the keep cases are: they
 * read what AppMgr asked and dispatch the answers.
 */

/** @brief The app.json every block of these cases carries. */
#define TIDY_JSON      "{\"kode\":\"1\"}"

/** @brief The deadline of one folder request, in the 10 ms steps run_ticks() turns. */
#define APPMGR_TIDY_DEADLINE_TICKS QF_SUPPORT_STEPS(APPMGR_TIDY_MS)

/** @brief The pattern the test icon's bytes cycle through (a prime, so no
 *         piece size lines up with it). */
#define TIDY_ICON_PERIOD  251U

/** @brief The most answer rounds tidy_settle() plays before giving up. */
#define TIDY_SETTLE_ROUNDS 100U

/** @brief The steps run_ticks() turns for an adoption to read a container
 *         with a 128x128 icon out of the mailbox, a piece at a time. */
#define TIDY_ADOPT_TICKS   40U

/** @brief The size of a short RECV_DATA event of the streaming cases. */
#define TIDY_SHORT_PIECE   100U

/** @brief The bytes of a container, laid out by container_named(). */
static uint8_t s_cont[KFW_HDR_LEN + KFW_AD_HDR_LEN + KFW_AD_JSON_MAX
                      + KFW_AD_ICON_LEN + sizeof(s_named_img)];

/**
 * @brief Build a container for an app named @p project in s_cont.
 *
 * The header, then - when @p with_block - the app-data block (TIDY_JSON, and
 * an icon of @p clen bytes), then image_named()'s image uncompressed.
 *
 * @param project    The project name the image's descriptor carries.
 * @param with_block Set KFW_FLAG_APPDATA and lay the block out.
 * @param clen       The icon's length: 0 for none, else what the block claims.
 * @return The container's length.
 */
static uint32_t container_icon(const char *project, bool with_block,
                               uint16_t clen)
{
    uint32_t const ilen  = image_named(project);
    uint16_t const jlen  = (uint16_t)(sizeof(TIDY_JSON) - 1U);
    uint32_t       at    = (uint32_t)KFW_HDR_LEN;
    kfw_hdr_t      h;
    kfw_ad_hdr_t   ad;

    (void)memset(s_cont, 0, sizeof(s_cont));
    if ( with_block )
    {
        (void)memcpy(&s_cont[at + KFW_AD_HDR_LEN], TIDY_JSON, jlen);
        for ( uint32_t i = 0U; i < clen; ++i )
        {
            s_cont[at + KFW_AD_HDR_LEN + jlen + i] = (uint8_t)(i % TIDY_ICON_PERIOD);
        }
        ad.magic    = KFW_AD_MAGIC;
        ad.json_len = jlen;
        ad.icon_len = clen;
        ad.crc      = kfw_crc32(&s_cont[at + KFW_AD_HDR_LEN],
                                (size_t)jlen + (size_t)clen);
        (void)memcpy(&s_cont[at], &ad, sizeof(ad));
        at += (uint32_t)KFW_AD_HDR_LEN + jlen + clen;
    }
    (void)memcpy(&s_cont[at], s_named_img, ilen);

    (void)memset(&h, 0, sizeof(h));
    h.magic       = KFW_MAGIC;
    h.hdr_version = (uint16_t)KFW_HDR_VERSION;
    h.hdr_len     = (uint16_t)KFW_HDR_LEN;
    h.target      = (uint8_t)KFW_TARGET_APP;
    h.comp        = (uint8_t)KFW_COMP_NONE;
    if ( with_block )
    {
        h.flags = KFW_FLAG_APPDATA;
    }
    h.payload_len = ilen;
    h.image_len   = ilen;
    kfw_sha256(s_named_img, ilen, h.sha256);
    h.crc = kfw_crc32(&h, offsetof(kfw_hdr_t, crc));
    (void)memcpy(s_cont, &h, sizeof(h));
    return at + ilen;
}

/**
 * @brief container_icon() with the icon the board holds, or none.
 *
 * @param project    The project name the image's descriptor carries.
 * @param with_block Set KFW_FLAG_APPDATA and lay the block out.
 * @param with_icon  Give the block a 128x128 icon, KFW_AD_ICON_LEN bytes.
 * @return The container's length.
 */
static uint32_t container_named(const char *project, bool with_block,
                                bool with_icon)
{
    uint16_t clen = 0U;

    if ( with_icon )
    {
        clen = (uint16_t)KFW_AD_ICON_LEN;
    }
    return container_icon(project, with_block, clen);
}

/**
 * @brief Install s_cont's @p len bytes the way `install src="terminal"` does,
 *        one RECV_DATA event of @p piece bytes at a time.
 */
static void install_container_in(uint32_t len, bool launch, uint32_t piece)
{
    ask_recv(len, khal_crc32(KHAL_CRC32_INIT, s_cont, len), launch);
    raw_armed(true);
    for ( uint32_t off = 0U; off < len; off += piece )
    {
        uint32_t n = len - off;
        if ( n > piece )
        {
            n = piece;
        }
        feed(&s_cont[off], (uint16_t)n);
    }
}

/** @brief The size of the pieces the console reader task delivers, give or take. */
#define TIDY_PIECE  1000U

/** @brief Install s_cont's @p len bytes, in pieces of TIDY_PIECE. */
static void install_container(uint32_t len, bool launch)
{
    install_container_in(len, launch, TIDY_PIECE);
}

/** @brief The payload bytes of the deflated container: opaque to the stand-in inflater. */
#define TIDY_ZIP_LEN  40U

/** @brief The bytes the stand-in inflater produces: the image of the deflated container. */
#define TIDY_ZIP_IMG  304U

/** @brief The image the deflated container inflates to: what the stand-in inflater is told to write. */
static uint8_t s_zip_img[TIDY_ZIP_IMG];

/**
 * @brief Build in s_cont a deflated container for an app, with no block.
 *
 * The host has no inflater (fake_tinfl.c): the stand-in turns any payload into
 * the bytes it is given, s_zip_img, so the container's sha256 is that of
 * such an image. What matters is that the payload is shorter than the image
 * it becomes, so the wire and the image part ways the other way round from a
 * container with a block.
 *
 * @return The container's length.
 */
static uint32_t container_deflated(void)
{
    uint8_t * const img = s_zip_img;
    kfw_hdr_t       h;

    (void)memset(img, 'Z', sizeof(s_zip_img));
    image_chain(img);                         /* an image as far as its chain, with no descriptor */
    (void)memset(s_cont, 0xA5, sizeof(s_cont));
    (void)memset(&h, 0, sizeof(h));
    h.magic       = KFW_MAGIC;
    h.hdr_version = (uint16_t)KFW_HDR_VERSION;
    h.hdr_len     = (uint16_t)KFW_HDR_LEN;
    h.target      = (uint8_t)KFW_TARGET_APP;
    h.comp        = (uint8_t)KFW_COMP_DEFLATE;
    h.payload_len = TIDY_ZIP_LEN;
    h.image_len   = TIDY_ZIP_IMG;
    kfw_sha256(img, sizeof(s_zip_img), h.sha256);
    h.crc = kfw_crc32(&h, offsetof(kfw_hdr_t, crc));
    (void)memcpy(s_cont, &h, sizeof(h));
    return (uint32_t)KFW_HDR_LEN + TIDY_ZIP_LEN;
}

/**
 * @brief AO_Storage's side of a put it has read: it owns the buffer and gives
 *        it back, and the event goes back to the pool.
 * @param e The APPDATA_PUT_REQ_SIG taken from AO_Storage's queue; may be NULL.
 */
static void put_done(const QEvt *e)
{
    if ( NULL != e )
    {
        if ( (enum_t)e->sig == APPDATA_PUT_REQ_SIG )
        {
            khal_free(((AppDataPutEvt const *)e)->buf);
        }
        QF_gc(e);
    }
}

/** @brief AO_Storage reads and drops whatever put is in its queue. */
static void storage_takes_the_put(void)
{
    const QEvt * const put = storage_take(APPDATA_PUT_REQ_SIG);

    TEST_ASSERT_NOT_NULL_MESSAGE(put, "the folder was never asked for");
    put_done(put);
}

/** @brief Answer AppMgr's put of a data folder. */
static void put_rsp(const char *project, kode_status_t rc)
{
    AppDataPutRspEvt r;

    (void)memset(&r, 0, sizeof(r));
    r.super = (QEvt)QEVT_INITIALIZER(APPDATA_PUT_RSP_SIG);
    (void)snprintf(r.project, sizeof(r.project), "%s", project);
    r.rc = rc;
    QASM_DISPATCH(AO_AppMgr, &r.super, 0U);
}

/** @brief Answer AppMgr's delete of @p project's data folder. */
static void rm_rsp(const char *project, kode_status_t st)
{
    AppDataRmRspEvt r;

    (void)memset(&r, 0, sizeof(r));
    r.super  = (QEvt)QEVT_INITIALIZER(APPDATA_RM_RSP_SIG);
    (void)snprintf(r.project, sizeof(r.project), "%s", project);
    r.status = (uint8_t)st;
    QASM_DISPATCH(AO_AppMgr, &r.super, 0U);
}

/**
 * @brief Answer AppMgr's wipe of @p project's files, with both media there.
 * @param project  The project the wipe was for.
 * @param st       How the wipe ended.
 * @param fs_files Images removed from the internal partition.
 * @param sd_files Images removed from the card.
 * @param left     Images that would not go from the card.
 */
static void wipe_rsp(const char *project, kode_status_t st, uint8_t fs_files,
                     uint8_t sd_files, uint8_t left)
{
    StorageAppWipeRspEvt r;

    (void)memset(&r, 0, sizeof(r));
    r.super = (QEvt)QEVT_INITIALIZER(STORAGE_APP_WIPE_RSP_SIG);
    (void)snprintf(r.project, sizeof(r.project), "%s", project);
    r.status     = (uint8_t)st;
    r.fs.present = true;
    r.fs.files   = fs_files;
    r.sd.present = true;
    r.sd.files   = sd_files;
    r.sd.left    = left;
    QASM_DISPATCH(AO_AppMgr, &r.super, 0U);
}

/**
 * @brief The one APP_ERASE_DONE_SIG the spy heard, or none yet.
 * @param out Receives it.
 * @return true when exactly one was heard.
 */
static bool erase_done(AppEraseDoneEvt *out)
{
    uint32_t n = 0U;

    for ( const QEvt *e = ao_spy_take(); NULL != e; e = ao_spy_take() )
    {
        if ( (enum_t)e->sig == APP_ERASE_DONE_SIG )
        {
            *out = *(AppEraseDoneEvt const *)e;
            ++n;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(1U >= n, "an erase ended more than once");
    return 1U == n;
}

/** @brief Ask AO_AppMgr to erase @p name. */
static void ask_erase(const char *name)
{
    AppFlashEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(APP_ERASE_REQ_SIG);
    (void)snprintf(e.file, sizeof(e.file), "%s", name);
    QASM_DISPATCH(AO_AppMgr, &e.super, 0U);
}

/** @brief Install one app named "demo" at the region's first extent. */
static void one_demo_installed(void)
{
    kapps_dir_t dir = s_keep_dir;

    dir.count       = 1U;
    dir.e[0].offset = KAPPS_DATA_OFF;
    dir.e[0].size   = 0x1000U;
    dir.e[0].seq    = 1U;
    dir.e[0].flags  = KAPPS_ENTRY_VALID;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));
    plant_app(KAPPS_DATA_OFF, "demo");
}

/**
 * @brief Answer every folder request AO_Storage's queue holds, and the ones
 *        those answers bring, as success.
 *
 * What a case that is done asserting does, so no install is left tidying
 * with its deadline armed for the next case's constructor to trip over.
 */
static void tidy_settle(void)
{
    for ( unsigned round = 0U; TIDY_SETTLE_ROUNDS > round; ++round )
    {
        bool       answered = false;
        const QEvt *e       = NULL;

        while ( xQueueReceive(AO_Storage->eQueue, &e, 0U) == pdTRUE )
        {
            enum_t const sig = (enum_t)e->sig;
            char         project[KDRV_SLOT_PROJECT_MAX] = "";

            if ( APPDATA_PUT_REQ_SIG == sig )
            {
                (void)snprintf(project, sizeof(project), "%s",
                               ((AppDataPutEvt const *)e)->project);
            }
            if ( (APPDATA_RM_REQ_SIG == sig) || (STORAGE_APP_WIPE_REQ_SIG == sig) )
            {
                (void)snprintf(project, sizeof(project), "%s",
                               ((AppDataReqEvt const *)e)->project);
            }
            put_done(e);
            if ( APPDATA_PUT_REQ_SIG == sig )
            {
                put_rsp(project, KODE_OK);
                answered = true;
            }
            else if ( APPDATA_RM_REQ_SIG == sig )
            {
                rm_rsp(project, KODE_OK);
                answered = true;
            }
            else if ( STORAGE_APP_WIPE_REQ_SIG == sig )
            {
                wipe_rsp(project, KODE_OK, 0U, 0U, 0U);
                answered = true;
            }
            else
            {
                /* not a folder request */
            }
        }
        if ( !answered )
        {
            break;
        }
    }
}

/** @brief What the spy heard of the end of an install. */
typedef struct {
    unsigned flash_done;     /**< APP_FLASH_DONE_SIGs.                        */
    unsigned recv_done;      /**< RECV_DONE_SIGs.                             */
    char     name[32];       /**< The last done's name.                       */
} ends_seen_t;

/** @brief Collect what the spy heard of the end of an install, draining it. */
static ends_seen_t spy_ends(void)
{
    ends_seen_t seen;

    (void)memset(&seen, 0, sizeof(seen));
    for ( const QEvt *e = ao_spy_take(); NULL != e; e = ao_spy_take() )
    {
        if ( (enum_t)e->sig == APP_FLASH_DONE_SIG )
        {
            ++seen.flash_done;
            (void)snprintf(seen.name, sizeof(seen.name), "%s",
                           ((AppFlashDoneEvt const *)e)->name);
        }
        if ( (enum_t)e->sig == RECV_DONE_SIG )
        {
            ++seen.recv_done;
        }
    }
    return seen;
}

/**
 * @brief Make the apps region refuse writes and erases, or take them again.
 *
 * The directory's commit is the one step of an install that cannot be made to
 * fail from outside, so the case that needs it flips the partition's own
 * read-only flag.
 *
 * @param on true to refuse, false to accept.
 */
static void region_readonly(bool on)
{
    const esp_partition_t * const found = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)KAPPS_PART_SUBTYPE,
        "apps");

    TEST_ASSERT_NOT_NULL(found);
    ((esp_partition_t *)(uintptr_t)found)->readonly = on;
}

/** @brief The start of every tidy case. */
static void tidy_begin_case(void)
{
    Mockkdrv_slot_Init();
    keep_fresh();
    keep_hold_dir();
    fake_psram_reset();
    ao_spy_subscribe(APP_FLASH_DONE_SIG);
    ao_spy_subscribe(RECV_DONE_SIG);
    ao_spy_subscribe(APP_ERASE_DONE_SIG);
}

/** @brief The end of every tidy case. */
static void tidy_end_case(void)
{
    QActive_unsubscribe(ao_spy(), APP_FLASH_DONE_SIG);
    QActive_unsubscribe(ao_spy(), RECV_DONE_SIG);
    QActive_unsubscribe(ao_spy(), APP_ERASE_DONE_SIG);
    ao_spy_drain();
    keep_end();
}

/** @brief Is AO_Storage's queue holding a request of @p sig for @p project? */
static bool storage_has_for(enum_t sig, const char *project)
{
    const QEvt *e = NULL;
    bool        found = false;

    while ( xQueueReceive(AO_Storage->eQueue, &e, 0U) == pdTRUE )
    {
        if ( ((enum_t)e->sig == sig)
            && (strcmp(((AppDataReqEvt const *)e)->project, project) == 0) )
        {
            found = true;
        }
        put_done(e);
    }
    return found;
}

TEST_CASE("an install with a block puts the folder before it is done",
          "[appmgr_ao][tidy]")
{
    tidy_begin_case();
    uint32_t const len = container_named("blockapp", true, true);

    /* From a file: two chunks of a medium's size carry the 48 KB icon. */
    ask_flash_dst("blockapp.bin", (uint8_t)KFW_TARGET_APP);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install never asked for its file");
    answer_open(len, (uint8_t)KDRV_MEDIUM_FS);
    answer_data_bytes(s_cont, len);
    answer_eof();

    const QEvt * const put = storage_take(APPDATA_PUT_REQ_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(put, "the folder was never asked for");
    AppDataPutEvt const * const pe = (AppDataPutEvt const *)put;
    TEST_ASSERT_EQUAL_STRING("blockapp", pe->project);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(sizeof(TIDY_JSON) - 1U), pe->json_len);
    TEST_ASSERT_EQUAL_MEMORY(TIDY_JSON, pe->buf, pe->json_len);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)KFW_AD_ICON_LEN, pe->icon_len);
    /* The icon is the bytes right after app.json, in the same allocation. */
    TEST_ASSERT_EQUAL_UINT8(0U, pe->buf[pe->json_len]);
    TEST_ASSERT_EQUAL_UINT8(1U, pe->buf[pe->json_len + 1U]);
    put_done(put);

    ends_seen_t const before = spy_ends();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, before.flash_done,
        "the install ended before its folder was written");

    put_rsp("blockapp", KODE_OK);
    ends_seen_t const after = spy_ends();
    TEST_ASSERT_EQUAL_UINT(1U, after.flash_done);
    TEST_ASSERT_EQUAL_STRING("blockapp", after.name);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(fake_psram_alloc_count(),
                                   fake_psram_free_count(),
        "the block's buffer was not given back");
    tidy_end_case();
}

TEST_CASE("a container without a block removes the folder before it is done",
          "[appmgr_ao][tidy]")
{
    tidy_begin_case();
    uint32_t const len = container_named("noblock", false, false);

    install_container(len, false);

    TEST_ASSERT_TRUE_MESSAGE(storage_has_for(APPDATA_RM_REQ_SIG, "noblock"),
        "the folder of a container with no block was not deleted");
    ends_seen_t const before = spy_ends();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, before.flash_done,
        "the install ended before its folder was deleted");

    rm_rsp("noblock", KODE_OK);
    ends_seen_t const after = spy_ends();
    TEST_ASSERT_EQUAL_UINT(1U, after.flash_done);
    TEST_ASSERT_EQUAL_STRING("noblock", after.name);
    tidy_end_case();
}

/**
 * @brief Build in s_cont a container for an app whose block cannot be believed.
 *
 * The block's CRC is wrong, so the decoder drops it and installs the image:
 * the container did not say the folder should go, it said nothing that could
 * be read.
 *
 * @param project The project the image carries.
 * @return The container's length.
 */
static uint32_t container_dropped(const char *project)
{
    uint32_t const len = container_named(project, true, false);

    s_cont[KFW_HDR_LEN + KFW_AD_OFS_CRC] ^= 1U;
    return len;
}

TEST_CASE("a container whose block was dropped leaves the folder alone, as a bare image does",
          "[appmgr_ao][tidy]")
{
    tidy_begin_case();
    uint32_t const len = container_dropped("damaged");

    install_container(len, false);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_folder_requests(),
        "a block that could not be read deleted or wrote the app's folder");
    ends_seen_t const seen = spy_ends();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, seen.flash_done,
        "the install waited for a folder nobody asked about");
    TEST_ASSERT_EQUAL_STRING("damaged", seen.name);
    TEST_ASSERT_TRUE_MESSAGE(klog_has("left as it is"),
        "the folder was left without a word");
    tidy_end_case();
}

TEST_CASE("a raw image leaves the folder alone", "[appmgr_ao][tidy]")
{
    tidy_begin_case();
    uint32_t const len = image_named("bare");

    ask_recv(len, khal_crc32(KHAL_CRC32_INIT, s_named_img, len), false);
    raw_armed(true);
    feed(s_named_img, (uint16_t)len);

    TEST_ASSERT_FALSE_MESSAGE(storage_has_for(APPDATA_PUT_REQ_SIG, "bare"),
        "a bare image put a folder");
    ends_seen_t const seen = spy_ends();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, seen.flash_done,
        "a bare image waited for a folder nobody asked about");
    TEST_ASSERT_EQUAL_UINT(1U, seen.recv_done);
    tidy_end_case();
}

TEST_CASE("launch waits for the folder", "[appmgr_ao][tidy]")
{
    uint32_t         off  = 0U;
    uint32_t         size = 0U;
    kboot_run_mode_t mode = KBOOT_RUN_STICKY;

    tidy_begin_case();
    (void)boot_run_request(&off, &size, &mode);
    uint32_t const len = container_named("runner", true, false);

    install_container(len, true);
    run_ticks(APPMGR_ANSWER_DRAIN_DEADLINE_TICKS + 20U);
    TEST_ASSERT_FALSE_MESSAGE(boot_run_request(&off, &size, &mode),
        "the app was launched before its folder was written");

    put_rsp("runner", KODE_OK);
    run_ticks(APPMGR_ANSWER_DRAIN_DEADLINE_TICKS + 2U);
    TEST_ASSERT_TRUE_MESSAGE(boot_run_request(&off, &size, &mode),
        "the app was never launched once its folder was written");
    TEST_ASSERT_EQUAL_INT(KBOOT_RUN_ONCE, mode);
    tidy_end_case();
}

/** @brief APPMGR_TIDY_MS in QP ticks, as appmgr_ao.c has it. */
#define APPMGR_TIDY_TEST_TICKS       (APPMGR_TIDY_MS / 10U)

/** @brief APPMGR_TIDY_LATE_MS in QP ticks. */
#define APPMGR_TIDY_LATE_TEST_TICKS  (APPMGR_TIDY_LATE_MS / 10U)

/* A folder AO_Storage has not answered for may still be being written: the
 * reset that follows a launch would cut it. The install that ends in a boot
 * gives the last request a little longer, once. */
TEST_CASE("an install that boots does not reset over a folder write still under way",
          "[appmgr_ao][tidy]")
{
    uint32_t         off  = 0U;
    uint32_t         size = 0U;
    kboot_run_mode_t mode = KBOOT_RUN_STICKY;

    tidy_begin_case();
    (void)boot_run_request(&off, &size, &mode);
    uint32_t const len = container_named("slowfolder", true, false);

    install_container(len, true);
    TEST_ASSERT_TRUE(storage_has_for(APPDATA_PUT_REQ_SIG, "slowfolder"));

    run_ticks(APPMGR_TIDY_TEST_TICKS + 5U);
    TEST_ASSERT_TRUE(klog_has("did not answer for the data folder"));
    run_ticks(APPMGR_ANSWER_DRAIN_DEADLINE_TICKS + 20U);
    TEST_ASSERT_FALSE_MESSAGE(boot_run_request(&off, &size, &mode),
        "the board reset over a folder AO_Storage was still writing");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, spy_ends().flash_done,
        "the install ended with its folder still out");

    put_rsp("slowfolder", KODE_OK);                      /* the late answer */
    TEST_ASSERT_EQUAL_UINT(1U, spy_ends().flash_done);
    run_ticks(APPMGR_ANSWER_DRAIN_DEADLINE_TICKS + 20U);
    TEST_ASSERT_TRUE_MESSAGE(boot_run_request(&off, &size, &mode),
        "the launch did not follow the folder's late answer");
    tidy_end_case();
}

TEST_CASE("an install that boots waits for a late folder answer once, and no longer",
          "[appmgr_ao][tidy]")
{
    uint32_t         off  = 0U;
    uint32_t         size = 0U;
    kboot_run_mode_t mode = KBOOT_RUN_STICKY;

    tidy_begin_case();
    (void)boot_run_request(&off, &size, &mode);
    uint32_t const len = container_named("lostfolder", true, false);

    install_container(len, true);
    TEST_ASSERT_TRUE(storage_has_for(APPDATA_PUT_REQ_SIG, "lostfolder"));

    run_ticks(APPMGR_TIDY_TEST_TICKS + APPMGR_TIDY_LATE_TEST_TICKS - 10U);
    TEST_ASSERT_FALSE(boot_run_request(&off, &size, &mode));
    run_ticks(APPMGR_ANSWER_DRAIN_DEADLINE_TICKS + 30U);
    TEST_ASSERT_TRUE_MESSAGE(boot_run_request(&off, &size, &mode),
        "a folder AO_Storage never answered held the launch for ever");
    TEST_ASSERT_TRUE(klog_has("still has not answered"));
    tidy_end_case();
}

TEST_CASE("an install that does not boot ends at the first deadline, as it did",
          "[appmgr_ao][tidy]")
{
    tidy_begin_case();
    uint32_t const len = container_named("quietfolder", true, false);

    install_container(len, false);
    TEST_ASSERT_TRUE(storage_has_for(APPDATA_PUT_REQ_SIG, "quietfolder"));

    run_ticks(APPMGR_TIDY_TEST_TICKS + 5U);
    ends_seen_t const seen = spy_ends();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, seen.flash_done,
        "an install with no reset ahead waited for a folder it can do without");
    tidy_end_case();
}

TEST_CASE("the terminal install's ok waits for the folder",
          "[appmgr_ao][tidy]")
{
    tidy_begin_case();
    uint32_t const len = container_named("answers", true, false);

    install_container(len, false);
    ends_seen_t const before = spy_ends();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, before.recv_done,
        "the host heard `>ok` before the folder existed");

    put_rsp("answers", KODE_OK);
    TEST_ASSERT_EQUAL_UINT(1U, spy_ends().recv_done);
    tidy_end_case();
}

TEST_CASE("adoption at boot waits for the folder", "[appmgr_ao][tidy]")
{
    static const QEvt adopt = QEVT_INITIALIZER(APP_ADOPT_SIG);
    uint32_t          off   = 0U;
    uint32_t          size  = 0U;
    kboot_run_mode_t  mode  = KBOOT_RUN_ONCE;

    tidy_begin_case();
    (void)boot_run_request(&off, &size, &mode);
    uint32_t const len = container_named("adopted", true, true);
    kdrv_apps_pending_write_abort();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_write_begin(len));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_write(s_cont, len));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_write_end());

    QASM_DISPATCH(AO_AppMgr, &adopt, 0U);
    run_ticks(TIDY_ADOPT_TICKS);
    TEST_ASSERT_FALSE_MESSAGE(boot_run_request(&off, &size, &mode),
        "the adopted app booted before its folder was written");
    const QEvt * const put = storage_take(APPDATA_PUT_REQ_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(put, "the adopted app's folder was never asked for");
    TEST_ASSERT_EQUAL_STRING("adopted", ((AppDataPutEvt const *)put)->project);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE((uint16_t)KFW_AD_ICON_LEN,
                                     ((AppDataPutEvt const *)put)->icon_len,
        "the mailbox lost the icon, or the block");
    put_done(put);

    put_rsp("adopted", KODE_OK);
    TEST_ASSERT_TRUE_MESSAGE(boot_run_request(&off, &size, &mode),
        "the adopted app never booted once its folder was written");
    TEST_ASSERT_EQUAL_INT(KBOOT_RUN_STICKY, mode);
    tidy_end_case();
}

TEST_CASE("an install that fails after the decode gives the block back",
          "[appmgr_ao][tidy]")
{
    tidy_begin_case();
    uint32_t const len = container_named("nocommit", true, true);

    ask_flash_dst("nocommit.bin", (uint8_t)KFW_TARGET_APP);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install never asked for its file");
    answer_open(len, (uint8_t)KDRV_MEDIUM_FS);
    answer_data_bytes(s_cont, len);
    /* The image is in and the block is kept: only the commit of the
     * directory is left, and that one fails. */
    region_readonly(true);
    answer_eof();
    region_readonly(false);

    TEST_ASSERT_TRUE_MESSAGE(klog_has("cannot update the directory"),
        "the install did not fail where the case means it to");
    TEST_ASSERT_FALSE_MESSAGE(storage_has_for(APPDATA_PUT_REQ_SIG, "nocommit"),
        "a failed install asked for a folder");
    ends_seen_t const seen = spy_ends();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, seen.flash_done,
        "a failed install was reported done");
    TEST_ASSERT_TRUE_MESSAGE(fake_psram_alloc_count() > 0U,
        "the container's block was never kept");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(fake_psram_alloc_count(),
                                   fake_psram_free_count(),
        "the block stayed held after the install failed");
    tidy_end_case();
}

TEST_CASE("a failed put still ends the install as done", "[appmgr_ao][tidy]")
{
    tidy_begin_case();
    uint32_t const len = container_named("failing", true, false);

    install_container(len, false);
    storage_takes_the_put();
    put_rsp("failing", KODE_IO_ERROR);

    ends_seen_t const seen = spy_ends();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, seen.flash_done,
        "a folder that would not be written failed the install");
    TEST_ASSERT_TRUE(klog_has("'failing' was not written"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(fake_psram_alloc_count(),
                                   fake_psram_free_count(),
        "the block's buffer was not given back");
    tidy_end_case();
}

/** @brief How many captured log lines hold @p needle. */
static unsigned klog_count(const char *needle)
{
    unsigned n = 0U;

    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const line = fake_klog_line(i);
        if ( (NULL != line) && (strstr(line, needle) != NULL) )
        {
            ++n;
        }
    }
    return n;
}

TEST_CASE("a silent AO_Storage times out and the install ends",
          "[appmgr_ao][tidy]")
{
    tidy_begin_case();
    uint32_t const len = container_named("silent", true, false);

    install_container(len, false);
    run_ticks(APPMGR_TIDY_DEADLINE_TICKS / 2U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, spy_ends().flash_done,
        "the install ended before the deadline");
    run_ticks((APPMGR_TIDY_DEADLINE_TICKS / 2U) + 20U);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, spy_ends().flash_done,
        "a silent AO_Storage held the install for good");
    TEST_ASSERT_TRUE(klog_has("did not answer for the data folder of 'silent'"));

    /* The buffer was AO_Storage's from the post: AppMgr holds nothing, so
     * what the queue still holds is the one thing left to give back. */
    storage_takes_the_put();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(fake_psram_alloc_count(),
                                   fake_psram_free_count(),
        "a buffer was lost across the deadline");
    put_rsp("silent", KODE_OK);              /* the answer, long late: ignored */
    TEST_ASSERT_EQUAL_UINT(0U, spy_ends().flash_done);
    tidy_end_case();
}

/* The old design counted late answers, and one answer that never came made the
 * next install's real answer look late: every later folder waited out its
 * deadline. An answer is taken only when it names the request in flight. */
TEST_CASE("a put that timed out does not eat the next install's answer",
          "[appmgr_ao][tidy]")
{
    tidy_begin_case();
    uint32_t len = container_named("lost", true, false);

    install_container(len, false);
    run_ticks(APPMGR_TIDY_DEADLINE_TICKS + 20U);
    TEST_ASSERT_EQUAL_UINT(1U, spy_ends().flash_done);
    storage_takes_the_put();
    unsigned const warned = klog_count("did not answer for the data folder");
    TEST_ASSERT_EQUAL_UINT(1U, warned);

    len = container_named("prompt", true, false);
    install_container(len, false);
    storage_takes_the_put();
    put_rsp("lost", KODE_OK);                /* the first one's, late */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, spy_ends().flash_done,
        "an answer that names another project ended this install");
    put_rsp("prompt", KODE_OK);
    ends_seen_t const seen = spy_ends();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, seen.flash_done,
        "the answer that came promptly was not taken");
    TEST_ASSERT_EQUAL_STRING("prompt", seen.name);

    run_ticks(APPMGR_TIDY_DEADLINE_TICKS + 20U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(warned,
                                   klog_count("did not answer for the data folder"),
        "a false warning: the answered install still waited out its deadline");
    TEST_ASSERT_EQUAL_UINT(0U, spy_ends().flash_done);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(fake_psram_alloc_count(),
                                   fake_psram_free_count(), "a buffer leaked");
    tidy_end_case();
}

/* The deadline of a request whose answer was taken after the deadline fired is
 * in the queue behind it: it must not cut the next request short. The ticks
 * alone are run (a ceiling of zero), so the expiry is queued and not yet
 * delivered when the answer is dispatched. */
TEST_CASE("a deadline queued behind its answer does not cut the next request",
          "[appmgr_ao][tidy]")
{
    tidy_begin_case();
    region_full_of_old();
    uint32_t const len = container_named("fresh", true, false);

    install_container(len, false);
    storage_takes_the_put();
    run_ticks(APPMGR_TIDY_DEADLINE_TICKS - 3U);
    for ( unsigned i = 0U; (8U > i) && (uxQueueMessagesWaiting(AO_AppMgr->eQueue) == 0U); ++i )
    {
        (void)qf_support_pump(AO_AppMgr, 0U);
    }
    TEST_ASSERT_TRUE_MESSAGE(uxQueueMessagesWaiting(AO_AppMgr->eQueue) > 0U,
        "the deadline never fired: the case is not testing what it says");

    put_rsp("fresh", KODE_OK);               /* the delete of "old" is out now */
    (void)qf_support_pump(AO_AppMgr, 8U);    /* the stale expiry is delivered */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, spy_ends().flash_done,
        "a stale deadline ended the delete's wait");
    TEST_ASSERT_FALSE(klog_has("did not answer"));
    rm_rsp("old", KODE_OK);
    TEST_ASSERT_EQUAL_UINT(1U, spy_ends().flash_done);
    tidy_end_case();
}

TEST_CASE("an answer that arrives while opening is ignored", "[appmgr_ao][tidy]")
{
    tidy_begin_case();

    ask_flash_dst("x.bin", (uint8_t)KFW_TARGET_APP);   /* now opening */
    put_rsp("anything", KODE_OK);
    rm_rsp("anything", KODE_OK);
    let_the_open_expire();

    TEST_ASSERT_TRUE_MESSAGE(klog_has("was never answered"),
        "a folder answer disturbed the state it found the AO in");
    tidy_end_case();
}

TEST_CASE("a delete that times out ends the install too", "[appmgr_ao][tidy]")
{
    tidy_begin_case();
    uint32_t const len = container_named("rmtimeout", false, false);

    install_container(len, false);
    run_ticks(APPMGR_TIDY_DEADLINE_TICKS + 20U);

    ends_seen_t const seen = spy_ends();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, seen.flash_done,
        "a delete nobody answered held the install for good");
    TEST_ASSERT_EQUAL_UINT(1U, seen.recv_done);
    TEST_ASSERT_TRUE(klog_has("did not answer for the data folder of 'rmtimeout'"));
    tidy_end_case();
}

/* A put that cannot even be posted is no wait: the pool of its tier is dry, so
 * nothing will answer. The buffer was never handed over, and AO_AppMgr gives
 * it back itself. */
TEST_CASE("a folder request that cannot be posted ends the install at once",
          "[appmgr_ao][tidy]")
{
    static QEvt *hoard[2U * 64U];
    unsigned     held = 0U;

    tidy_begin_case();
    uint32_t const len = container_named("nopool", true, false);

    ask_recv(len, khal_crc32(KHAL_CRC32_INIT, s_cont, len), false);
    raw_armed(true);
    while ( Q_DIM(hoard) > held )
    {
        AppDataPutEvt * const e = Q_NEW_X(AppDataPutEvt, 0U, Q_USER_SIG);
        if ( NULL == e )
        {
            break;
        }
        hoard[held] = &e->super;
        ++held;
    }
    bool const dry = (Q_DIM(hoard) > held);
    for ( uint32_t off = 0U; off < len; off += TIDY_PIECE )
    {
        uint32_t n = len - off;
        if ( TIDY_PIECE < n )
        {
            n = TIDY_PIECE;
        }
        feed(&s_cont[off], (uint16_t)n);
    }
    for ( unsigned i = 0U; i < held; ++i )
    {
        QF_gc(hoard[i]);
    }

    TEST_ASSERT_TRUE_MESSAGE(dry, "the pool outlasted the hoard");
    TEST_ASSERT_FALSE_MESSAGE(storage_has_for(APPDATA_PUT_REQ_SIG, "nopool"),
        "a put was posted into a dry pool");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, spy_ends().flash_done,
        "an install waited for an answer that could not come");
    TEST_ASSERT_EQUAL_UINT(1U, klog_count("could not be asked for"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(fake_psram_alloc_count(),
                                   fake_psram_free_count(),
        "AO_AppMgr kept the buffer of a put it could not post");
    tidy_end_case();
}

TEST_CASE("an evicted app that was not kept loses its folder",
          "[appmgr_ao][tidy]")
{
    tidy_begin_case();
    region_full_of_old();
    uint32_t const len = container_named("fresh", true, false);

    install_container(len, false);
    const QEvt * const put = storage_take(APPDATA_PUT_REQ_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(put, "the new app's folder was never asked for");
    put_done(put);
    TEST_ASSERT_EQUAL_UINT(0U, spy_ends().flash_done);

    put_rsp("fresh", KODE_OK);
    TEST_ASSERT_TRUE_MESSAGE(storage_has_for(APPDATA_RM_REQ_SIG, "old"),
        "the evicted app's folder was not deleted");
    ends_seen_t const mid = spy_ends();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, mid.flash_done,
        "the install ended before the evicted app's folder was deleted");

    rm_rsp("old", KODE_OK);
    TEST_ASSERT_EQUAL_UINT(1U, spy_ends().flash_done);
    tidy_end_case();
}

TEST_CASE("an evicted app kept on the card keeps its folder",
          "[appmgr_ao][tidy]")
{
    tidy_begin_case();
    region_full_of_old();
    uint32_t const len = container_named("fresh", true, false);

    ask_flash_dst("new.bin", (uint8_t)KFW_TARGET_APP);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install never asked for its file");
    answer_open(len, (uint8_t)KDRV_MEDIUM_FS);
    answer_data_bytes(s_cont, len);
    storage_expect(STORAGE_PATH_REQ_SIG, "the victim's path was never asked about");
    answer_stat(true, (uint8_t)KODE_OK);          /* already on the card */
    (void)qf_support_pump(AO_AppMgr, 8U);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install did not start again");

    answer_open(len, (uint8_t)KDRV_MEDIUM_FS);
    answer_data_bytes(s_cont, len);
    answer_eof();
    TEST_ASSERT_FALSE_MESSAGE(old_installed(), "the install did not evict it");

    const QEvt * const put = storage_take(APPDATA_PUT_REQ_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(put, "the new app's folder was never asked for");
    put_done(put);
    put_rsp("fresh", KODE_OK);
    TEST_ASSERT_FALSE_MESSAGE(storage_has_for(APPDATA_RM_REQ_SIG, "old"),
        "a kept app lost its folder");
    TEST_ASSERT_EQUAL_UINT(1U, spy_ends().flash_done);
    tidy_end_case();
}

TEST_CASE("an evicted app with the installed app's name keeps the new folder",
          "[appmgr_ao][tidy]")
{
    tidy_begin_case();
    region_full_of_old();
    uint32_t const len = container_named("old", true, false);

    install_container(len, false);
    const QEvt * const put = storage_take(APPDATA_PUT_REQ_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(put, "the new app's folder was never asked for");
    put_done(put);
    put_rsp("old", KODE_OK);

    TEST_ASSERT_FALSE_MESSAGE(storage_has_for(APPDATA_RM_REQ_SIG, "old"),
        "the folder just written was deleted with the app it replaced");
    TEST_ASSERT_EQUAL_UINT(1U, spy_ends().flash_done);
    tidy_end_case();
}

/* ---- The erase: everything of the app, and an end that says so --------------- */

TEST_CASE("app_erase takes the image, then every file of the app, then says so",
          "[appmgr_ao][tidy][erase]")
{
    AppEraseDoneEvt done;

    tidy_begin_case();
    one_demo_installed();

    ask_erase("demo");

    TEST_ASSERT_EQUAL_UINT32(0U, kdrv_apps_dir()->count);
    TEST_ASSERT_TRUE_MESSAGE(storage_has_for(STORAGE_APP_WIPE_REQ_SIG, "demo"),
        "the app's files went with nothing asked of AO_Storage");
    TEST_ASSERT_FALSE_MESSAGE(erase_done(&done),
        "the erase ended before its files were dealt with");
    TEST_ASSERT_FALSE_MESSAGE(klog_has("'demo' removed"),
        "the erase was reported before its files were deleted");

    wipe_rsp("demo", KODE_OK, 1U, 2U, 0U);

    TEST_ASSERT_TRUE_MESSAGE(erase_done(&done), "the erase never said it ended");
    TEST_ASSERT_EQUAL_STRING("demo", done.name);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, done.status);
    TEST_ASSERT_TRUE(done.image);
    TEST_ASSERT_EQUAL_UINT8(1U, done.fs.files);
    TEST_ASSERT_EQUAL_UINT8(2U, done.sd.files);
    TEST_ASSERT_TRUE(klog_has("'demo' removed"));
    tidy_end_case();
}

/* A file of an app that was never installed, or was evicted, is the same row
 * in the launcher: the erase reaches it by the same door. */
TEST_CASE("app_erase of an app that is not installed still takes its files",
          "[appmgr_ao][tidy][erase]")
{
    AppEraseDoneEvt done;

    tidy_begin_case();
    one_demo_installed();

    ask_erase("ghost");
    TEST_ASSERT_TRUE(storage_has_for(STORAGE_APP_WIPE_REQ_SIG, "ghost"));
    wipe_rsp("ghost", KODE_OK, 0U, 1U, 0U);

    TEST_ASSERT_TRUE(erase_done(&done));
    TEST_ASSERT_EQUAL_STRING("ghost", done.name);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, done.status);
    TEST_ASSERT_FALSE(done.image);
    TEST_ASSERT_EQUAL_UINT8(1U, done.sd.files);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, kdrv_apps_dir()->count,
        "an installed app went with a name that is not its own");
    tidy_end_case();
}

TEST_CASE("app_erase of a name with nothing anywhere ends as an error",
          "[appmgr_ao][tidy][erase]")
{
    AppEraseDoneEvt done;

    tidy_begin_case();

    ask_erase("nobody");
    TEST_ASSERT_TRUE(storage_has_for(STORAGE_APP_WIPE_REQ_SIG, "nobody"));
    wipe_rsp("nobody", KODE_OK, 0U, 0U, 0U);

    TEST_ASSERT_TRUE(erase_done(&done));
    TEST_ASSERT_EQUAL_UINT8(KODE_ERROR, done.status);
    TEST_ASSERT_FALSE(done.image);
    TEST_ASSERT_FALSE(klog_has("'nobody' removed"));
    tidy_end_case();
}

/* No name is no app: nothing is asked of AO_Storage, which would otherwise be
 * asked for the files of every image that names no project. */
TEST_CASE("app_erase with no name ends as an error and asks nobody",
          "[appmgr_ao][tidy][erase]")
{
    AppEraseDoneEvt done;

    tidy_begin_case();
    one_demo_installed();

    ask_erase("");

    TEST_ASSERT_TRUE(erase_done(&done));
    TEST_ASSERT_EQUAL_UINT8(KODE_ERROR, done.status);
    TEST_ASSERT_FALSE_MESSAGE(storage_has_for(STORAGE_APP_WIPE_REQ_SIG, ""),
        "AO_Storage was asked for the files of no app");
    TEST_ASSERT_EQUAL_UINT32(1U, kdrv_apps_dir()->count);
    tidy_end_case();
}

/* A card that fails its deletes leaves the file where it was: the end says
 * so, and carries what did go. */
TEST_CASE("app_erase says what would not go", "[appmgr_ao][tidy][erase]")
{
    AppEraseDoneEvt done;

    tidy_begin_case();
    one_demo_installed();

    ask_erase("demo");
    TEST_ASSERT_TRUE(storage_has_for(STORAGE_APP_WIPE_REQ_SIG, "demo"));
    wipe_rsp("demo", KODE_OK, 1U, 0U, 1U);

    TEST_ASSERT_TRUE(erase_done(&done));
    TEST_ASSERT_EQUAL_UINT8(KODE_IO_ERROR, done.status);
    TEST_ASSERT_TRUE(done.image);
    TEST_ASSERT_EQUAL_UINT8(1U, done.fs.files);
    TEST_ASSERT_EQUAL_UINT8(1U, done.sd.left);
    TEST_ASSERT_FALSE_MESSAGE(klog_has("'demo' removed"),
        "an app with a file left behind was called removed");
    tidy_end_case();
}

TEST_CASE("app_erase says a data folder that would not empty was left",
          "[appmgr_ao][tidy][erase]")
{
    StorageAppWipeRspEvt r;
    AppEraseDoneEvt      done;

    tidy_begin_case();
    one_demo_installed();

    ask_erase("demo");
    TEST_ASSERT_TRUE(storage_has_for(STORAGE_APP_WIPE_REQ_SIG, "demo"));
    (void)memset(&r, 0, sizeof(r));
    r.super = (QEvt)QEVT_INITIALIZER(STORAGE_APP_WIPE_RSP_SIG);
    (void)snprintf(r.project, sizeof(r.project), "%s", "demo");
    r.status     = (uint8_t)KODE_OK;
    r.fs.present = true;
    r.fs.folder  = (uint8_t)APP_WIPE_FOLDER_LEFT;
    QASM_DISPATCH(AO_AppMgr, &r.super, 0U);

    TEST_ASSERT_TRUE(erase_done(&done));
    TEST_ASSERT_EQUAL_UINT8(KODE_IO_ERROR, done.status);
    TEST_ASSERT_EQUAL_UINT8(APP_WIPE_FOLDER_LEFT, done.fs.folder);
    TEST_ASSERT_FALSE(done.sd.present);
    tidy_end_case();
}

/* AO_Storage in the middle of a transfer refuses the wipe: the image is gone,
 * and the end says the files were not looked at rather than calling them gone. */
TEST_CASE("app_erase with AO_Storage busy says the files were not looked at",
          "[appmgr_ao][tidy][erase]")
{
    AppEraseDoneEvt done;

    tidy_begin_case();
    one_demo_installed();

    ask_erase("demo");
    TEST_ASSERT_TRUE(storage_has_for(STORAGE_APP_WIPE_REQ_SIG, "demo"));
    wipe_rsp("demo", KODE_DENIED, 0U, 0U, 0U);

    TEST_ASSERT_TRUE(erase_done(&done));
    TEST_ASSERT_EQUAL_UINT8(KODE_DENIED, done.status);
    TEST_ASSERT_TRUE(done.image);
    TEST_ASSERT_FALSE(klog_has("'demo' removed"));
    tidy_end_case();
}

TEST_CASE("a silent AO_Storage ends the erase as a timeout",
          "[appmgr_ao][tidy][erase]")
{
    AppEraseDoneEvt done;

    tidy_begin_case();
    one_demo_installed();

    ask_erase("demo");
    run_ticks(QF_SUPPORT_STEPS(APPMGR_WIPE_MS) / 2U);
    TEST_ASSERT_FALSE_MESSAGE(erase_done(&done),
        "the erase gave up before its deadline");
    run_ticks((QF_SUPPORT_STEPS(APPMGR_WIPE_MS) / 2U) + 20U);

    TEST_ASSERT_TRUE_MESSAGE(erase_done(&done),
        "a silent AO_Storage held the erase for good");
    TEST_ASSERT_EQUAL_UINT8(KODE_TIMEOUT, done.status);
    TEST_ASSERT_TRUE(done.image);

    /* The answer, long late, ends nothing a second time. */
    wipe_rsp("demo", KODE_OK, 1U, 0U, 0U);
    TEST_ASSERT_FALSE(erase_done(&done));
    (void)storage_has_for(STORAGE_APP_WIPE_REQ_SIG, "demo");
    tidy_end_case();
}

/* An answer is taken only when it names the erase in flight. */
TEST_CASE("a wipe answer for another project does not end the erase",
          "[appmgr_ao][tidy][erase]")
{
    AppEraseDoneEvt done;

    tidy_begin_case();
    one_demo_installed();

    ask_erase("demo");
    TEST_ASSERT_TRUE(storage_has_for(STORAGE_APP_WIPE_REQ_SIG, "demo"));
    wipe_rsp("other", KODE_OK, 3U, 0U, 0U);
    TEST_ASSERT_FALSE_MESSAGE(erase_done(&done),
        "an answer that names another project ended this erase");

    wipe_rsp("demo", KODE_OK, 0U, 0U, 0U);
    TEST_ASSERT_TRUE(erase_done(&done));
    TEST_ASSERT_EQUAL_UINT8(0U, done.fs.files);
    tidy_end_case();
}

/* The console and the launcher both wait on the end of an erase: one that
 * cannot start is told so, never dropped. */
TEST_CASE("an erase asked while another is under way is answered busy",
          "[appmgr_ao][tidy][erase]")
{
    AppEraseDoneEvt done;

    tidy_begin_case();
    one_demo_installed();

    ask_erase("demo");
    TEST_ASSERT_TRUE(storage_has_for(STORAGE_APP_WIPE_REQ_SIG, "demo"));
    ask_erase("second");

    TEST_ASSERT_TRUE_MESSAGE(erase_done(&done),
        "an erase refused while busy went unanswered");
    TEST_ASSERT_EQUAL_STRING("second", done.name);
    TEST_ASSERT_EQUAL_UINT8(KODE_DENIED, done.status);
    TEST_ASSERT_FALSE(done.image);
    TEST_ASSERT_FALSE_MESSAGE(storage_has_for(STORAGE_APP_WIPE_REQ_SIG, "second"),
        "a refused erase asked AO_Storage all the same");

    wipe_rsp("demo", KODE_OK, 0U, 0U, 0U);
    TEST_ASSERT_TRUE(erase_done(&done));
    TEST_ASSERT_EQUAL_STRING("demo", done.name);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, done.status);
    tidy_end_case();
}

TEST_CASE("an erase asked while an install is opening is answered busy",
          "[appmgr_ao][tidy][erase]")
{
    AppEraseDoneEvt done;

    tidy_begin_case();
    one_demo_installed();
    ask_flash_dst("x.bin", (uint8_t)KFW_TARGET_APP);   /* now opening */

    ask_erase("demo");

    TEST_ASSERT_TRUE(erase_done(&done));
    TEST_ASSERT_EQUAL_UINT8(KODE_DENIED, done.status);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, kdrv_apps_dir()->count,
        "an app was erased in the middle of an install");
    let_the_open_expire();
    tidy_end_case();
}

/* The override is the person's word about that app: it goes with the app, or
 * the next app of that name would wear it. */
TEST_CASE("app_erase drops the app's category override",
          "[appmgr_ao][tidy][erase]")
{
    char cats[KODE_APP_CATEGORY_MAX];

    tidy_begin_case();
    one_demo_installed();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kapp_cat_set("demo", "games"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kapp_cat_set("other", "tools"));

    ask_erase("demo");

    TEST_ASSERT_FALSE_MESSAGE(kapp_cat_resolve("demo", cats, sizeof(cats)),
        "the erased app's override stayed");
    TEST_ASSERT_TRUE_MESSAGE(kapp_cat_resolve("other", cats, sizeof(cats)),
        "another app's override went");
    (void)kapp_cat_clear("other");
    tidy_settle();
    tidy_end_case();
}

/* A mailbox that still holds the app would be adopted at the next boot, and
 * the app would be back. */
TEST_CASE("app_erase empties a mailbox that holds the same app",
          "[appmgr_ao][tidy][erase]")
{
    uint32_t img = 0U;
    uint32_t dec = 0U;

    tidy_begin_case();
    one_demo_installed();
    (void)park_pending("demo");
    TEST_ASSERT_TRUE(kdrv_apps_pending_present(&img, &dec));

    ask_erase("demo");

    TEST_ASSERT_FALSE_MESSAGE(kdrv_apps_pending_present(&img, &dec),
        "the mailbox still holds the erased app");
    tidy_settle();
    tidy_end_case();
}

TEST_CASE("app_erase leaves a mailbox that holds another app",
          "[appmgr_ao][tidy][erase]")
{
    uint32_t img = 0U;
    uint32_t dec = 0U;

    tidy_begin_case();
    one_demo_installed();
    (void)park_pending("other");

    ask_erase("demo");

    TEST_ASSERT_TRUE_MESSAGE(kdrv_apps_pending_present(&img, &dec),
        "another app's mailbox image went with the erase");
    (void)kdrv_apps_pending_consume();
    tidy_settle();
    tidy_end_case();
}

/* The directory is committed before the tidy starts, so a listing is safe
 * while the files go: the launcher asks for one right behind its erase, and it
 * lands in tidying. */
TEST_CASE("app_list is answered while the folder is tidied",
          "[appmgr_ao][tidy]")
{
    AppListReqEvt     list = {
        .super = QEVT_INITIALIZER(APP_LIST_REQ_SIG), .skip = 0U,
        .who = ao_spy() };
    AppFlashEvt       e;
    bool              saw_end = false;

    tidy_begin_case();
    kapps_dir_t dir = s_keep_dir;
    dir.count       = 1U;
    dir.e[0].offset = KAPPS_DATA_OFF;
    dir.e[0].size   = 0x1000U;
    dir.e[0].seq    = 1U;
    dir.e[0].flags  = KAPPS_ENTRY_VALID;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));
    plant_app(KAPPS_DATA_OFF, "demo");

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(APP_ERASE_REQ_SIG);
    (void)snprintf(e.file, sizeof(e.file), "%s", "demo");
    QASM_DISPATCH(AO_AppMgr, &e.super, 0U);
    TEST_ASSERT_TRUE_MESSAGE(storage_has_for(STORAGE_APP_WIPE_REQ_SIG, "demo"),
        "the app's files went with nothing asked of AO_Storage");

    QASM_DISPATCH(AO_AppMgr, &list.super, 0U);
    for ( const QEvt *got = ao_spy_take(); NULL != got; got = ao_spy_take() )
    {
        if ( (enum_t)got->sig == APP_LIST_END_SIG )
        {
            saw_end = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(saw_end,
        "the listing asked for while tidying never ended");
    TEST_ASSERT_FALSE_MESSAGE(klog_has("busy tidying"),
        "the listing was refused as a busy request");

    wipe_rsp("demo", KODE_OK, 0U, 0U, 0U);
    TEST_ASSERT_TRUE_MESSAGE(klog_has("'demo' removed"),
        "answering the listing disturbed the tidy");
    tidy_end_case();
}

TEST_CASE("a request during tidying is ignored, and the next one runs",
          "[appmgr_ao][tidy]")
{
    tidy_begin_case();
    uint32_t const len = container_named("busy", true, false);

    install_container(len, false);
    {
        const QEvt * const first = storage_take(APPDATA_PUT_REQ_SIG);
        TEST_ASSERT_NOT_NULL_MESSAGE(first, "the folder was never asked for");
        put_done(first);
    }

    ask_flash_dst("other.bin", (uint8_t)KFW_TARGET_APP);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_tally().open,
        "a request was taken while the folder was still being written");
    TEST_ASSERT_TRUE(klog_has("busy tidying"));

    put_rsp("busy", KODE_OK);
    ask_flash_dst("other.bin", (uint8_t)KFW_TARGET_APP);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, storage_tally().open,
        "the AO did not go back to taking requests");
    let_the_open_expire();
    tidy_end_case();
}

TEST_CASE("a name that cannot be a folder skips the put", "[appmgr_ao][tidy]")
{
    tidy_begin_case();
    uint32_t const len = container_named("bad name", true, false);

    install_container(len, false);

    TEST_ASSERT_FALSE_MESSAGE(storage_has_for(APPDATA_PUT_REQ_SIG, "bad name"),
        "a folder was asked for under a name FAT cannot hold");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, spy_ends().flash_done,
        "the install waited for a folder that was never asked for");
    TEST_ASSERT_TRUE(klog_has("cannot be the name of a data folder"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(fake_psram_alloc_count(),
                                   fake_psram_free_count(),
        "the block's buffer was not given back");
    tidy_end_case();
}

TEST_CASE("an install whose block holds a 64 px icon puts the folder with it as it is",
          "[appmgr_ao][tidy]")
{
    tidy_begin_case();
    uint32_t const len = container_icon("halfapp", true,
                                        (uint16_t)KFW_AD_ICON_LEN_HALF);

    ask_flash_dst("halfapp.bin", (uint8_t)KFW_TARGET_APP);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install never asked for its file");
    answer_open(len, (uint8_t)KDRV_MEDIUM_FS);
    answer_data_bytes(s_cont, len);
    answer_eof();

    const QEvt * const put = storage_take(APPDATA_PUT_REQ_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(put, "the folder was never asked for");
    TEST_ASSERT_EQUAL_UINT16((uint16_t)KFW_AD_ICON_LEN_HALF,
                             ((AppDataPutEvt const *)put)->icon_len);
    put_done(put);
    put_rsp("halfapp", KODE_OK);
    TEST_ASSERT_EQUAL_UINT(1U, spy_ends().flash_done);
    tidy_end_case();
}

/* `kcli install app.kfw` and `kble install` go down the terminal, a container
 * with its block and icon longer than one RECV_DATA event and longer than
 * the image the decoder places. The transfer ends at the byte the console
 * announced, not at the image's length. */
TEST_CASE("a container with a block and an icon streams in over several events",
          "[appmgr_ao][tidy]")
{
    tidy_begin_case();
    uint32_t const len = container_named("streamed", true, true);

    install_container(len, false);

    const QEvt * const put = storage_take(APPDATA_PUT_REQ_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(put,
        "the container's folder was never asked for: the transfer ended early");
    TEST_ASSERT_EQUAL_STRING("streamed", ((AppDataPutEvt const *)put)->project);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)KFW_AD_ICON_LEN,
                             ((AppDataPutEvt const *)put)->icon_len);
    put_done(put);
    TEST_ASSERT_FALSE_MESSAGE(klog_has("CRC mismatch"), "a CRC taken too early");
    TEST_ASSERT_EQUAL_UINT(0U, spy_ends().recv_done);

    put_rsp("streamed", KODE_OK);
    ends_seen_t const seen = spy_ends();
    TEST_ASSERT_EQUAL_UINT(1U, seen.flash_done);
    TEST_ASSERT_EQUAL_UINT(1U, seen.recv_done);
    TEST_ASSERT_EQUAL_STRING("streamed", seen.name);
    tidy_end_case();
}

TEST_CASE("a small container streams in a few bytes at a time",
          "[appmgr_ao][tidy]")
{
    tidy_begin_case();
    uint32_t const len = container_named("dribble", true, false);

    install_container_in(len, false, TIDY_SHORT_PIECE);

    TEST_ASSERT_TRUE_MESSAGE(storage_has_for(APPDATA_PUT_REQ_SIG, "dribble"),
        "the container's folder was never asked for");
    put_rsp("dribble", KODE_OK);
    ends_seen_t const seen = spy_ends();
    TEST_ASSERT_EQUAL_UINT(1U, seen.flash_done);
    TEST_ASSERT_EQUAL_UINT(1U, seen.recv_done);
    tidy_end_case();
}

TEST_CASE("a compressed container streams in: the wire is shorter than the image",
          "[appmgr_ao][tidy]")
{
    tidy_begin_case();
    fake_tinfl_reset();
    fake_tinfl_set((int)TINFL_STATUS_DONE, 4096U, TIDY_ZIP_IMG);
    uint32_t const len = container_deflated();
    fake_tinfl_set_payload(s_zip_img, sizeof(s_zip_img));

    /* The header in one event, the payload in the next: the inflater runs once. */
    install_container_in(len, false, (uint32_t)KFW_HDR_LEN);
    fake_tinfl_reset();

    TEST_ASSERT_FALSE_MESSAGE(klog_has("CRC mismatch"),
        "the transfer ended at the image's length, not the wire's");
    TEST_ASSERT_TRUE_MESSAGE(klog_has("could not be read"),
        "the stand-in's image has no descriptor, so no folder is named");
    ends_seen_t const seen = spy_ends();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, seen.flash_done,
        "the compressed container did not install");
    TEST_ASSERT_EQUAL_UINT(1U, seen.recv_done);
    tidy_end_case();
}

/* ---- Where an app is, and keeping only one ----------------------------------
 * What a factory reset asks of the apps region: the extent of the welcome app
 * (or whether the card has it), and every other app gone in one commit. The
 * answers are posted to the asker alone.
 */

/**
 * @brief Install @p n apps of one KAPPS_ALIGN page each, named @p names, with
 *        sequence numbers from 1. The directory as it was is remembered.
 */
static void small_apps(unsigned n, const char *const names[])
{
    keep_hold_dir();
    kapps_dir_t dir = s_keep_dir;

    dir.count = n;
    for ( unsigned i = 0U; i < n; ++i )
    {
        dir.e[i].offset = KAPPS_DATA_OFF + (i * KAPPS_ALIGN);
        dir.e[i].size   = KAPPS_ALIGN;
        dir.e[i].seq    = i + 1U;
        dir.e[i].flags  = KAPPS_ENTRY_VALID;
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&dir));
    for ( unsigned i = 0U; i < n; ++i )
    {
        plant_app(dir.e[i].offset, names[i]);
    }
}

/** @brief The one APP_FIND_RSP_SIG the spy was sent; fails if none or two. */
static AppFindRspEvt find_answer(void)
{
    AppFindRspEvt rsp;
    unsigned      n = 0U;

    (void)memset(&rsp, 0, sizeof(rsp));
    for ( const QEvt *e = ao_spy_take(); NULL != e; e = ao_spy_take() )
    {
        TEST_ASSERT_EQUAL_INT((int)APP_FIND_RSP_SIG, (int)e->sig);
        rsp = *(AppFindRspEvt const *)e;
        ++n;
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, n, "the asker did not get exactly one answer");
    return rsp;
}

/** @brief The one APP_KEEP_ONLY_RSP_SIG the spy was sent, or none yet. */
static bool keep_only_answer(AppKeepOnlyRspEvt *out)
{
    unsigned n = 0U;

    for ( const QEvt *e = ao_spy_take(); NULL != e; e = ao_spy_take() )
    {
        TEST_ASSERT_EQUAL_INT((int)APP_KEEP_ONLY_RSP_SIG, (int)e->sig);
        *out = *(AppKeepOnlyRspEvt const *)e;
        ++n;
    }
    TEST_ASSERT_TRUE_MESSAGE(1U >= n, "the asker got more than one answer");
    return 1U == n;
}

/** @brief Take AO_Storage's next folder delete and check it names @p project. */
static void path_rm_expect(const char *project)
{
    const QEvt * const e = storage_take(APPDATA_RM_REQ_SIG);

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the folder of an erased app was not asked for");
    TEST_ASSERT_EQUAL_STRING(project, ((AppDataReqEvt const *)e)->project);
    put_done(e);
}

/** @brief Is the app whose image starts at @p offset still named by its header? */
static bool header_named(uint32_t offset)
{
    kapps_entry_t const probe = {
        .offset = offset, .size = 0U, .seq = 0U, .flags = KAPPS_ENTRY_VALID };
    kdrv_slot_desc_t desc;

    return kdrv_apps_desc(&probe, &desc, NULL) == KODE_OK;
}

TEST_CASE("test_find_installed_gives_its_extent", "[appmgr_ao][find]")
{
    static const char *const names[] = { "welcome", "other" };

    Mockkdrv_slot_Init();
    keep_fresh();
    small_apps(2U, names);

    TEST_ASSERT_TRUE(appmgr_find_request("other", ao_spy()));
    (void)qf_support_pump(AO_AppMgr, 8U);
    AppFindRspEvt const rsp = find_answer();

    TEST_ASSERT_FALSE(rsp.busy);
    TEST_ASSERT_TRUE(rsp.installed);
    TEST_ASSERT_EQUAL_STRING("other", rsp.project);
    TEST_ASSERT_EQUAL_HEX32(KAPPS_DATA_OFF + KAPPS_ALIGN, rsp.offset);
    TEST_ASSERT_EQUAL_UINT32(KAPPS_ALIGN, rsp.size);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_tally().path,
        "the card was asked about an app that is installed");
    keep_end();
}

TEST_CASE("test_find_missing_asks_the_card", "[appmgr_ao][find]")
{
    static const char *const names[] = { "other" };

    Mockkdrv_slot_Init();
    keep_fresh();
    small_apps(1U, names);

    TEST_ASSERT_TRUE(appmgr_find_request("welcome", ao_spy()));
    (void)qf_support_pump(AO_AppMgr, 8U);
    stat_expect("/sdcard/welcome.bin", "the card was not asked for the missing app");
    TEST_ASSERT_NULL_MESSAGE(ao_spy_take(), "answered before the card did");

    answer_stat(true, (uint8_t)KODE_OK);
    AppFindRspEvt const rsp = find_answer();
    TEST_ASSERT_FALSE(rsp.busy);
    TEST_ASSERT_FALSE(rsp.installed);
    TEST_ASSERT_TRUE(rsp.on_card);
    TEST_ASSERT_EQUAL_STRING("welcome", rsp.project);

    /* And the AO is idle again: the next question is looked at. */
    TEST_ASSERT_TRUE(appmgr_find_request("other", ao_spy()));
    (void)qf_support_pump(AO_AppMgr, 8U);
    TEST_ASSERT_TRUE(find_answer().installed);
    keep_end();
}

TEST_CASE("test_find_missing_with_a_silent_card_is_not_on_card", "[appmgr_ao][find]")
{
    static const char *const names[] = { "other" };

    Mockkdrv_slot_Init();
    keep_fresh();
    small_apps(1U, names);

    TEST_ASSERT_TRUE(appmgr_find_request("welcome", ao_spy()));
    (void)qf_support_pump(AO_AppMgr, 8U);
    stat_expect("/sdcard/welcome.bin", "the card was not asked for the missing app");
    run_ticks(APPMGR_OPEN_DEADLINE_TICKS + 50U);          /* nobody answers */

    AppFindRspEvt const rsp = find_answer();
    TEST_ASSERT_FALSE(rsp.installed);
    TEST_ASSERT_FALSE(rsp.on_card);
    TEST_ASSERT_FALSE(rsp.busy);
    keep_end();
}

TEST_CASE("test_find_while_busy_says_busy", "[appmgr_ao][find]")
{
    Mockkdrv_slot_Init();
    keep_fresh();
    region_full_of_old();

    ask_evict(0x10000U);                                   /* AppMgr_asking */
    stat_expect("/sdcard/old.bin", "the victim's copy was not looked for");
    ao_spy_drain();

    TEST_ASSERT_TRUE(appmgr_find_request("old", ao_spy()));
    (void)qf_support_pump(AO_AppMgr, 8U);
    TEST_ASSERT_TRUE(find_answer().busy);

    answer_stat(false, (uint8_t)KODE_OK);                  /* settle the question */
    ao_spy_drain();
    keep_end();
}

TEST_CASE("test_keep_only_erases_the_others_in_one_commit", "[appmgr_ao][find]")
{
    static const char *const names[] = { "other", "welcome", "third" };
    AppKeepOnlyRspEvt        rsp;

    Mockkdrv_slot_Init();
    keep_fresh();
    small_apps(3U, names);
    uint32_t const gen = kdrv_apps_dir()->gen;

    TEST_ASSERT_TRUE(appmgr_keep_only_request("welcome", ao_spy()));
    (void)qf_support_pump(AO_AppMgr, 8U);

    /* One commit for both victims, and nothing answered until the folders. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(gen + 1U, kdrv_apps_dir()->gen,
        "the victims were not removed in one directory commit");
    TEST_ASSERT_FALSE(header_named(KAPPS_DATA_OFF));
    TEST_ASSERT_TRUE(header_named(KAPPS_DATA_OFF + KAPPS_ALIGN));
    TEST_ASSERT_FALSE(header_named(KAPPS_DATA_OFF + (2U * KAPPS_ALIGN)));
    TEST_ASSERT_FALSE_MESSAGE(keep_only_answer(&rsp), "answered before the folders went");

    /* One folder at a time, in the directory's order. */
    path_rm_expect("other");
    TEST_ASSERT_FALSE(keep_only_answer(&rsp));
    rm_rsp("other", KODE_OK);
    path_rm_expect("third");
    TEST_ASSERT_FALSE(keep_only_answer(&rsp));
    rm_rsp("third", KODE_OK);

    TEST_ASSERT_TRUE_MESSAGE(keep_only_answer(&rsp), "no answer after the last folder");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, rsp.status);
    TEST_ASSERT_EQUAL_UINT8(2U, rsp.erased);

    kapps_dir_t const * const d = kdrv_apps_dir();
    unsigned valid = 0U;
    for ( uint32_t i = 0U; i < d->count; ++i )
    {
        if ( (d->e[i].flags & KAPPS_ENTRY_VALID) != 0U )
        {
            ++valid;
            TEST_ASSERT_EQUAL_HEX32(KAPPS_DATA_OFF + KAPPS_ALIGN, d->e[i].offset);
        }
    }
    TEST_ASSERT_EQUAL_UINT(1U, valid);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_folder_requests(),
        "the kept app's folder, or one folder twice, was asked for");
    keep_end();
}

TEST_CASE("test_keep_only_erases_every_header_one_step_at_a_time",
          "[appmgr_ao][find]")
{
    static const char *const names[] = { "a", "keeper", "b", "c" };
    AppKeepOnlyRspEvt        rsp;

    Mockkdrv_slot_Init();
    keep_fresh();
    small_apps(4U, names);

    TEST_ASSERT_TRUE(appmgr_keep_only_request("keeper", ao_spy()));
    (void)qf_support_pump(AO_AppMgr, 1U);                 /* the request: one commit */
    TEST_ASSERT_TRUE_MESSAGE(header_named(KAPPS_DATA_OFF),
        "a header went before its own step");
    (void)qf_support_pump(AO_AppMgr, 1U);                 /* one step: one header */
    TEST_ASSERT_FALSE(header_named(KAPPS_DATA_OFF));
    TEST_ASSERT_TRUE_MESSAGE(header_named(KAPPS_DATA_OFF + (2U * KAPPS_ALIGN)),
        "two headers were erased in one step");
    (void)qf_support_pump(AO_AppMgr, 8U);

    TEST_ASSERT_FALSE(header_named(KAPPS_DATA_OFF));
    TEST_ASSERT_TRUE(header_named(KAPPS_DATA_OFF + KAPPS_ALIGN));
    TEST_ASSERT_FALSE(header_named(KAPPS_DATA_OFF + (2U * KAPPS_ALIGN)));
    TEST_ASSERT_FALSE(header_named(KAPPS_DATA_OFF + (3U * KAPPS_ALIGN)));
    path_rm_expect("a");
    rm_rsp("a", KODE_OK);
    path_rm_expect("b");
    rm_rsp("b", KODE_OK);
    path_rm_expect("c");
    rm_rsp("c", KODE_OK);
    TEST_ASSERT_TRUE(keep_only_answer(&rsp));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, rsp.status);
    TEST_ASSERT_EQUAL_UINT8(3U, rsp.erased);
    keep_end();
}

TEST_CASE("test_keep_only_a_failed_commit_erases_nothing", "[appmgr_ao][find]")
{
    static const char *const names[] = { "other", "welcome", "third" };
    AppKeepOnlyRspEvt        rsp;

    Mockkdrv_slot_Init();
    keep_fresh();
    small_apps(3U, names);
    uint32_t const gen = kdrv_apps_dir()->gen;

    region_readonly(true);
    TEST_ASSERT_TRUE(appmgr_keep_only_request("welcome", ao_spy()));
    (void)qf_support_pump(AO_AppMgr, 8U);
    region_readonly(false);

    TEST_ASSERT_TRUE_MESSAGE(keep_only_answer(&rsp), "no answer to a failed commit");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_ERROR, rsp.status);
    TEST_ASSERT_EQUAL_UINT8(0U, rsp.erased);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(gen, kdrv_apps_dir()->gen,
        "the directory changed though its commit failed");
    for ( unsigned i = 0U; 3U > i; ++i )
    {
        TEST_ASSERT_TRUE_MESSAGE(header_named(KAPPS_DATA_OFF + (i * KAPPS_ALIGN)),
            "an app was half-erased");
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, storage_folder_requests(),
        "a folder was asked for after a failed commit");

    /* Idle again: the next question is looked at. */
    TEST_ASSERT_TRUE(appmgr_find_request("third", ao_spy()));
    (void)qf_support_pump(AO_AppMgr, 8U);
    TEST_ASSERT_TRUE(find_answer().installed);
    keep_end();
}

TEST_CASE("test_keep_only_a_failed_folder_does_not_change_the_status",
          "[appmgr_ao][find]")
{
    static const char *const names[] = { "other", "welcome" };
    AppKeepOnlyRspEvt        rsp;

    Mockkdrv_slot_Init();
    keep_fresh();
    small_apps(2U, names);

    TEST_ASSERT_TRUE(appmgr_keep_only_request("welcome", ao_spy()));
    (void)qf_support_pump(AO_AppMgr, 8U);
    path_rm_expect("other");
    rm_rsp("other", KODE_DENIED);                         /* the folder stays */

    TEST_ASSERT_TRUE(keep_only_answer(&rsp));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, rsp.status);
    TEST_ASSERT_EQUAL_UINT8(1U, rsp.erased);
    TEST_ASSERT_TRUE(klog_has("was not deleted"));
    keep_end();
}

TEST_CASE("test_keep_only_without_the_kept_app_erases_nothing", "[appmgr_ao][find]")
{
    static const char *const names[] = { "other", "third" };
    AppKeepOnlyRspEvt        rsp;

    Mockkdrv_slot_Init();
    keep_fresh();
    small_apps(2U, names);
    uint32_t const gen = kdrv_apps_dir()->gen;

    TEST_ASSERT_TRUE(appmgr_keep_only_request("welcome", ao_spy()));
    (void)qf_support_pump(AO_AppMgr, 8U);

    TEST_ASSERT_TRUE(keep_only_answer(&rsp));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_DENIED, rsp.status);
    TEST_ASSERT_EQUAL_UINT8(0U, rsp.erased);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(gen, kdrv_apps_dir()->gen,
        "the directory was written");
    TEST_ASSERT_TRUE(header_named(KAPPS_DATA_OFF));
    TEST_ASSERT_TRUE(header_named(KAPPS_DATA_OFF + KAPPS_ALIGN));
    TEST_ASSERT_EQUAL_UINT(0U, storage_folder_requests());
    keep_end();
}

TEST_CASE("test_keep_only_with_no_name_erases_every_app", "[appmgr_ao][find]")
{
    static const char *const names[] = { "other", "third" };
    AppKeepOnlyRspEvt        rsp;

    Mockkdrv_slot_Init();
    keep_fresh();
    small_apps(2U, names);

    TEST_ASSERT_TRUE(appmgr_keep_only_request("", ao_spy()));
    (void)qf_support_pump(AO_AppMgr, 8U);

    TEST_ASSERT_FALSE(header_named(KAPPS_DATA_OFF));
    TEST_ASSERT_FALSE(header_named(KAPPS_DATA_OFF + KAPPS_ALIGN));
    path_rm_expect("other");
    rm_rsp("other", KODE_OK);
    path_rm_expect("third");
    rm_rsp("third", KODE_OK);

    TEST_ASSERT_TRUE_MESSAGE(keep_only_answer(&rsp), "no answer after the last folder");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, rsp.status);
    TEST_ASSERT_EQUAL_UINT8(2U, rsp.erased);
    TEST_ASSERT_EQUAL_UINT32(0U, kdrv_apps_dir()->count);
    keep_end();
}

TEST_CASE("test_keep_only_while_busy_is_denied", "[appmgr_ao][find]")
{
    static const char *const names[] = { "other", "welcome" };
    AppKeepOnlyRspEvt        rsp;

    Mockkdrv_slot_Init();
    keep_fresh();
    small_apps(2U, names);

    TEST_ASSERT_TRUE(appmgr_find_request("third", ao_spy()));   /* AppMgr_finding */
    (void)qf_support_pump(AO_AppMgr, 8U);
    stat_expect("/sdcard/third.bin", "the card was not asked");

    TEST_ASSERT_TRUE(appmgr_keep_only_request("welcome", ao_spy()));
    (void)qf_support_pump(AO_AppMgr, 8U);
    TEST_ASSERT_TRUE(keep_only_answer(&rsp));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_DENIED, rsp.status);
    TEST_ASSERT_TRUE(header_named(KAPPS_DATA_OFF));

    answer_stat(false, (uint8_t)KODE_OK);                  /* settle the find */
    ao_spy_drain();
    keep_end();
}

TEST_CASE("test_find_and_keep_only_refuse_a_missing_asker", "[appmgr_ao][find]")
{
    Mockkdrv_slot_Init();
    keep_fresh();

    TEST_ASSERT_FALSE(appmgr_find_request("x", NULL));
    TEST_ASSERT_FALSE(appmgr_keep_only_request("x", NULL));
    keep_end();
}

/* A URL is bounded by the request, not by a file name: one of 256 characters
 * with a token in its query reaches AO_Net whole, and the log names the file
 * alone. */
TEST_CASE("a URL longer than a file name is asked for whole and named by its "
          "last segment", "[appmgr_ao]")
{
    static char       url[KODE_FLASH_SRC_MAX];
    static const char HEAD[] = "https://example.invalid/apps/app.bin?token=";
    StorageOpenRspEvt rsp;
    const QEvt       *e;
    bool              asked = false;

    (void)snprintf(url, sizeof(url), "%s", HEAD);
    (void)memset(&url[sizeof(HEAD) - 1U], 'x', sizeof(url) - sizeof(HEAD));
    url[sizeof(url) - 1U] = '\0';
    TEST_ASSERT_EQUAL_UINT(256U, strlen(url));

    appmgr_fresh();
    (void)net_requests(NET_STREAM_CLOSE_REQ_SIG);
    ask_https(url);
    while ( xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (enum_t)e->sig == NET_STREAM_OPEN_REQ_SIG )
        {
            TEST_ASSERT_EQUAL_STRING_MESSAGE(url,
                ((NetStreamOpenEvt const *)e)->url,
                "the URL did not reach AO_Net whole");
            asked = true;
        }
        QF_gc(e);
    }
    TEST_ASSERT_TRUE_MESSAGE(asked, "the download was refused, not asked for");
    (void)storage_requests();

    (void)memset(&rsp, 0, sizeof(rsp));
    rsp.super  = (QEvt)QEVT_INITIALIZER(NET_STREAM_OPEN_RSP_SIG);
    rsp.status = (uint8_t)KODE_ERROR;
    QASM_DISPATCH(AO_AppMgr, &rsp.super, 0U);

    TEST_ASSERT_TRUE_MESSAGE(
        klog_has("'app.bin' did not come back from the server"),
        "the download was not named by its last segment");
    TEST_ASSERT_FALSE_MESSAGE(klog_has("token="),
        "the log printed the URL's query");
    (void)net_requests(NET_STREAM_CLOSE_REQ_SIG);
    (void)storage_requests();
}

/* ---- The project in the write reports -------------------------------------
 * An app's data folder is named for its project and travels apart from the
 * image, so it may be on the board before the app is. The Opening card can
 * only show that folder's name, icon and colour when the reports say which
 * project is being written: the end event names it too late. */

/** @brief One write report, as far as these cases read it. */
typedef struct {
    uint8_t percent;
    char    project[KODE_APP_PROJECT_MAX];
} report_t;

/** @brief The write reports the spy holds, in order, emptying it. */
static uint32_t reports_take(report_t *out, uint32_t max)
{
    uint32_t n = 0U;

    for ( const QEvt *e = ao_spy_take(); NULL != e; e = ao_spy_take() )
    {
        if ( ((int)e->sig == APP_FLASH_PROGRESS_SIG) && (n < max) )
        {
            AppProgressEvt const * const p = (AppProgressEvt const *)e;
            out[n].percent = p->percent;
            (void)snprintf(out[n].project, sizeof(out[n].project), "%s",
                           p->project);
            ++n;
        }
    }
    return n;
}

TEST_CASE("a terminal install that names its project says it from 0 %",
          "[appmgr_ao][project]")
{
    report_t       r[8];
    uint32_t const len = image_named("snake");

    Mockkdrv_slot_Init();
    recv_fresh();
    ao_spy_drain();
    ao_spy_subscribe(APP_FLASH_PROGRESS_SIG);

    ask_recv_named(len, 0xDEADBEEFU, false, "snake");
    raw_armed(true);
    uint32_t n = reports_take(r, Q_DIM(r));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, n, "the ready's 0 % and nothing else yet");
    TEST_ASSERT_EQUAL_UINT(0U, r[0].percent);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("snake", r[0].project,
        "the host named the project: the card must not wait for the image");

    /* The image says the same name: nothing new to tell, so no extra report. */
    feed(s_named_img, (uint16_t)len);
    n = reports_take(r, Q_DIM(r));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, n, "one report for the one chunk");
    TEST_ASSERT_EQUAL_UINT(100U, r[0].percent);
    TEST_ASSERT_EQUAL_STRING("snake", r[0].project);

    QActive_unsubscribe(ao_spy(), APP_FLASH_PROGRESS_SIG);
    ao_spy_drain();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("a terminal install with no name says its project at the first chunk",
          "[appmgr_ao][project]")
{
    report_t       r[8];
    uint32_t const len = image_named("snake");

    Mockkdrv_slot_Init();
    recv_fresh();
    ao_spy_drain();
    ao_spy_subscribe(APP_FLASH_PROGRESS_SIG);

    ask_recv(len, 0xDEADBEEFU, false);
    raw_armed(true);
    uint32_t n = reports_take(r, Q_DIM(r));
    TEST_ASSERT_EQUAL_UINT(1U, n);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", r[0].project,
        "nobody has said what this is yet");

    feed(s_named_img, (uint16_t)len);
    n = reports_take(r, Q_DIM(r));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, n,
        "the name is told as soon as it is read, then the chunk's percent");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, r[0].percent,
        "told at the first chunk, not a whole percent later");
    TEST_ASSERT_EQUAL_STRING("snake", r[0].project);
    TEST_ASSERT_EQUAL_UINT(100U, r[1].percent);
    TEST_ASSERT_EQUAL_STRING("snake", r[1].project);

    QActive_unsubscribe(ao_spy(), APP_FLASH_PROGRESS_SIG);
    ao_spy_drain();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("the name the image carries wins over the one the host gave",
          "[appmgr_ao][project]")
{
    report_t       r[8];
    uint32_t const len = image_named("snake");

    Mockkdrv_slot_Init();
    recv_fresh();
    ao_spy_drain();
    ao_spy_subscribe(APP_FLASH_PROGRESS_SIG);

    ask_recv_named(len, 0xDEADBEEFU, false, "ladder");
    raw_armed(true);
    feed(s_named_img, (uint16_t)len);
    uint32_t const n = reports_take(r, Q_DIM(r));
    TEST_ASSERT_EQUAL_UINT(3U, n);
    TEST_ASSERT_EQUAL_STRING("ladder", r[0].project);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("snake", r[1].project,
        "the image is what is being written, whatever the host called it");
    TEST_ASSERT_EQUAL_UINT(0U, r[1].percent);
    TEST_ASSERT_EQUAL_STRING("snake", r[2].project);

    QActive_unsubscribe(ao_spy(), APP_FLASH_PROGRESS_SIG);
    ao_spy_drain();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("an install from a medium says its project once the first chunk is read",
          "[appmgr_ao][project]")
{
    report_t       r[8];
    uint32_t const len = image_named("new");

    Mockkdrv_slot_Init();
    keep_fresh();
    keep_hold_dir();
    kapps_dir_t empty = *kdrv_apps_dir();
    empty.count = 0U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&empty));
    ao_spy_subscribe(APP_FLASH_PROGRESS_SIG);

    ask_flash_dst("new.bin", (uint8_t)KFW_TARGET_APP);
    storage_expect(STORAGE_OPEN_REQ_SIG, "the install never asked for its file");
    answer_open(len, (uint8_t)KDRV_MEDIUM_FS);
    uint32_t n = reports_take(r, Q_DIM(r));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, n, "the announce, before any byte");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", r[0].project,
        "a file's name is not its project: nothing to say until it is read");

    answer_data(len);
    n = reports_take(r, Q_DIM(r));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(1U, n);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, r[0].percent,
        "told at the first chunk, not a whole percent later");
    TEST_ASSERT_EQUAL_STRING("new", r[0].project);

    QActive_unsubscribe(ao_spy(), APP_FLASH_PROGRESS_SIG);
    let_the_open_expire();
    (void)storage_requests();
    keep_end();
}

TEST_CASE("the project of one write does not leak into the next",
          "[appmgr_ao][project]")
{
    report_t       r[8];
    uint32_t const len = image_named("snake");

    Mockkdrv_slot_Init();
    recv_fresh();
    ao_spy_drain();

    ask_recv(len, 0xDEADBEEFU, false);      /* a failed write of "snake" */
    raw_armed(true);
    feed(s_named_img, (uint16_t)len);
    run_ticks(APPMGR_ANSWER_DRAIN_DEADLINE_TICKS + 2U);
    ao_spy_drain();
    ao_spy_subscribe(APP_FLASH_PROGRESS_SIG);

    ask_recv(len, 0xDEADBEEFU, false);      /* and one nobody has named */
    raw_armed(true);
    uint32_t const n = reports_take(r, Q_DIM(r));
    TEST_ASSERT_EQUAL_UINT(1U, n);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", r[0].project,
        "the last write's project named a stranger");

    feed(s_named_img, (uint16_t)len);
    QActive_unsubscribe(ao_spy(), APP_FLASH_PROGRESS_SIG);
    ao_spy_drain();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("a kodeOS write names no project", "[appmgr_ao][project]")
{
    report_t       r[8];
    uint32_t const len = image_named("kodeos");

    Mockkdrv_slot_Init();
    recv_fresh();
    (void)memset(&s_slot_log, 0, sizeof(s_slot_log));
    ao_spy_drain();
    ao_spy_subscribe(APP_FLASH_PROGRESS_SIG);

    kdrv_slot_present_ExpectAndReturn(true);
    kdrv_slot_size_IgnoreAndReturn(0x400000U);
    kdrv_slot_address_IgnoreAndReturn(0x420000U);
    kdrv_slot_sink_ExpectAndReturn(&SLOT_FAKE_SINK);
    kdrv_slot_desc_IgnoreAndReturn(KODE_ERROR);

    ask_recv_os(len, 0xDEADBEEFU);
    raw_armed(true);
    feed(s_named_img, (uint16_t)len);
    uint32_t const n = reports_take(r, Q_DIM(r));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(2U, n);
    for ( uint32_t i = 0U; i < n; ++i )
    {
        TEST_ASSERT_EQUAL_STRING_MESSAGE("", r[i].project,
            "kodeOS is no app: there is no data folder to look for");
    }

    QActive_unsubscribe(ao_spy(), APP_FLASH_PROGRESS_SIG);
    ao_spy_drain();
    Mockkdrv_slot_Destroy();
}

/* ---- An app's data folder fetched from a URL -------------------------------
 * AO_AppMgr reads `<url>app.json` and `<url>icon.k64` through AO_Net's
 * transfer and hands both to AO_Storage. AO_Net is a mailbox in this file, so
 * its buffers do not exist here: these cases cover the requests, the lengths
 * judged before a byte is read, the refusals and the deadline. The bytes'
 * own road, from AO_Net's buffer to AO_Storage's folder, is not a host case;
 * what an app.json may say is kapp_data_fetched()'s, in test_kapp_data.c. */

#define GET_URL  "http://example.invalid/apps/snake/"

/** @brief Ask AO_AppMgr for @p project's folder from @p url, answered to the spy. */
static void ask_get(const char *project, const char *url)
{
    AppDataGetReqEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(APPDATA_GET_REQ_SIG);
    (void)snprintf(e.project, sizeof(e.project), "%s", project);
    (void)snprintf(e.url, sizeof(e.url), "%s", url);
    e.who = ao_spy();
    QASM_DISPATCH(AO_AppMgr, &e.super, 0U);
}

/** @brief What AO_Net's queue holds: the opens and closes, and the last URL opened. */
typedef struct {
    uint32_t opens;
    uint32_t closes;
    uint32_t reads;
    char     url[KODE_FLASH_SRC_MAX];
} net_seen_t;

/** @brief Read and empty AO_Net's queue. */
static net_seen_t net_seen(void)
{
    net_seen_t  n;
    const QEvt *e;

    (void)memset(&n, 0, sizeof(n));
    while ( xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (enum_t)e->sig == NET_STREAM_OPEN_REQ_SIG )
        {
            ++n.opens;
            (void)snprintf(n.url, sizeof(n.url), "%s",
                           ((NetStreamOpenEvt const *)e)->url);
        }
        if ( (enum_t)e->sig == NET_STREAM_CLOSE_REQ_SIG )
        {
            ++n.closes;
        }
        if ( (enum_t)e->sig == NET_STREAM_READ_REQ_SIG )
        {
            ++n.reads;
        }
        QF_gc(e);
    }
    return n;
}

/** @brief Answer the open in flight the way AO_Net would. */
static void fetch_opened(uint8_t status, uint32_t size)
{
    StorageOpenRspEvt rsp;

    (void)memset(&rsp, 0, sizeof(rsp));
    rsp.super  = (QEvt)QEVT_INITIALIZER(NET_STREAM_OPEN_RSP_SIG);
    rsp.status = status;
    rsp.size   = size;
    QASM_DISPATCH(AO_AppMgr, &rsp.super, 0U);
}

/**
 * @brief The one answer a fetch owes, taken from the spy.
 *
 * @param project Whose folder the answer must name.
 * @return How it ended; fails the case when there is no answer, or two.
 */
static appdata_get_why_t fetch_answer(const char *project)
{
    appdata_get_why_t why     = APPDATA_GET_OK;
    uint32_t          answers = 0U;

    for ( const QEvt *e = ao_spy_take(); NULL != e; e = ao_spy_take() )
    {
        if ( (int)e->sig == APPDATA_GET_RSP_SIG )
        {
            AppDataGetRspEvt const * const g = (AppDataGetRspEvt const *)e;
            TEST_ASSERT_EQUAL_STRING(project, g->project);
            TEST_ASSERT_FALSE_MESSAGE(g->icon, "no folder was written");
            why = (appdata_get_why_t)g->why;
            ++answers;
        }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, answers, "a fetch is answered once");
    return why;
}

/* AO_Net's stream buffers do not exist here, so the tests CMake renames
 * AppMgr's one call that reads them to this function: a fetched file's bytes
 * are in s_net_chunk, as buffer 0. Every other index is a buffer AO_Net has
 * not got. */
static uint8_t s_net_chunk[KAPP_DATA_JSON_MAX];

const uint8_t *host_net_stream_buf(uint8_t idx);

const uint8_t *host_net_stream_buf(uint8_t idx)
{
    const uint8_t *buf = NULL;

    if ( 0U == idx )
    {
        buf = s_net_chunk;
    }
    return buf;
}

/** @brief The app.json every fetch below brings: it names no icon. */
#define GET_JSON_BODY  "{\"kode\":\"1\",\"name\":\"Snake\",\"category\":\"games\"}"

/**
 * @brief Run a fetch of "snake" up to the put it asks AO_Storage for: app.json
 *        arrives whole in one chunk and names no icon.
 */
static void fetch_to_the_put(void)
{
    StorageDataEvt d;
    uint32_t const len = (uint32_t)(sizeof(GET_JSON_BODY) - 1U);

    ask_get("snake", GET_URL);
    (void)net_seen();
    fetch_opened((uint8_t)KODE_OK, len);

    (void)memcpy(s_net_chunk, GET_JSON_BODY, len);
    (void)memset(&d, 0, sizeof(d));
    d.super  = (QEvt)QEVT_INITIALIZER(NET_STREAM_DATA_SIG);
    d.len    = (uint16_t)len;
    d.idx    = 0U;
    d.status = (uint8_t)KODE_OK;
    d.eof    = true;
    QASM_DISPATCH(AO_AppMgr, &d.super, 0U);
}

/**
 * @brief AO_Storage reads the put AO_AppMgr asked for: it must name "snake"
 *        and carry app.json whole, and it gives the buffer back.
 */
static void fetch_put_is_whole(void)
{
    const QEvt * const e = storage_take(APPDATA_PUT_REQ_SIG);

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the folder was not asked for");
    AppDataPutEvt const * const put = (AppDataPutEvt const *)e;
    TEST_ASSERT_EQUAL_STRING("snake", put->project);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(sizeof(GET_JSON_BODY) - 1U), put->json_len);
    TEST_ASSERT_EQUAL_UINT16(0U, put->icon_len);
    TEST_ASSERT_EQUAL_MEMORY(GET_JSON_BODY, put->buf, put->json_len);
    put_done(e);
}

/** @brief AO_Storage answers "snake"'s put: refused for want of a turn. */
static void put_rsp_busy(void)
{
    AppDataPutRspEvt r;

    (void)memset(&r, 0, sizeof(r));
    r.super = (QEvt)QEVT_INITIALIZER(APPDATA_PUT_RSP_SIG);
    (void)snprintf(r.project, sizeof(r.project), "%s", "snake");
    r.rc   = KODE_DENIED;
    r.busy = true;
    QASM_DISPATCH(AO_AppMgr, &r.super, 0U);
}

/** @brief The start of a fetch case: an idle AppMgr, empty mailboxes, a clean heap. */
static void fetch_fresh(void)
{
    Mockkdrv_slot_Init();
    recv_fresh();
    fake_psram_reset();
    (void)net_seen();
    (void)storage_requests();
    ao_spy_drain();
}

/** @brief The end of one: nothing is held, and the AO takes the next fetch. */
static void fetch_end(void)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(fake_psram_alloc_count(), fake_psram_free_count(),
        "the fetch kept its buffer");
    (void)net_seen();
    ask_get("after", GET_URL);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, net_seen().opens,
        "the AO did not go back to taking fetches");
    fetch_opened((uint8_t)KODE_ERROR, 0U);
    ao_spy_drain();
    (void)net_seen();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("a fetch opens app.json at the base URL and answers nothing yet",
          "[appmgr_ao][appdata_get]")
{
    fetch_fresh();

    ask_get("snake", GET_URL);
    net_seen_t const n = net_seen();
    TEST_ASSERT_EQUAL_UINT(1U, n.opens);
    TEST_ASSERT_EQUAL_STRING(GET_URL "app.json", n.url);
    TEST_ASSERT_NULL_MESSAGE(ao_spy_take(), "answered before anything came");

    fetch_opened((uint8_t)KODE_OK, 120U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, net_seen().reads,
        "a file of a good length is read");

    fetch_opened((uint8_t)KODE_ERROR, 0U);         /* ends it */
    TEST_ASSERT_EQUAL_INT(APPDATA_GET_FETCH, fetch_answer("snake"));
    fetch_end();
}

TEST_CASE("a fetch for a name that cannot be a folder opens nothing",
          "[appmgr_ao][appdata_get]")
{
    fetch_fresh();

    ask_get("../etc", GET_URL);
    TEST_ASSERT_EQUAL_INT(APPDATA_GET_NAME, fetch_answer("../etc"));
    TEST_ASSERT_EQUAL_UINT(0U, net_seen().opens);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_psram_alloc_count(),
        "a refused fetch took a buffer");
    fetch_end();
}

TEST_CASE("a fetch whose file does not open says fetch",
          "[appmgr_ao][appdata_get]")
{
    fetch_fresh();

    ask_get("snake", GET_URL);
    (void)net_seen();
    fetch_opened((uint8_t)KODE_ERROR, 0U);
    TEST_ASSERT_EQUAL_INT(APPDATA_GET_FETCH, fetch_answer("snake"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, net_seen().closes,
        "nothing was opened: there is nothing to close");
    fetch_end();
}

TEST_CASE("a fetched file with no length is not read",
          "[appmgr_ao][appdata_get]")
{
    fetch_fresh();

    ask_get("snake", GET_URL);
    (void)net_seen();
    fetch_opened((uint8_t)KODE_OK, 0U);            /* no Content-Length */
    TEST_ASSERT_EQUAL_INT(APPDATA_GET_FETCH, fetch_answer("snake"));
    net_seen_t const n = net_seen();
    TEST_ASSERT_EQUAL_UINT(0U, n.reads);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, n.closes, "the transfer was left open");
    fetch_end();
}

TEST_CASE("a fetched app.json longer than the reader takes is not read",
          "[appmgr_ao][appdata_get]")
{
    fetch_fresh();

    ask_get("snake", GET_URL);
    (void)net_seen();
    fetch_opened((uint8_t)KODE_OK, KAPP_DATA_JSON_MAX + 1U);
    TEST_ASSERT_EQUAL_INT(APPDATA_GET_JSON, fetch_answer("snake"));
    net_seen_t const n = net_seen();
    TEST_ASSERT_EQUAL_UINT(0U, n.reads);
    TEST_ASSERT_EQUAL_UINT(1U, n.closes);
    fetch_end();
}

TEST_CASE("a chunk in a buffer AO_Net has not got ends the fetch",
          "[appmgr_ao][appdata_get]")
{
    StorageDataEvt d;

    fetch_fresh();

    ask_get("snake", GET_URL);
    fetch_opened((uint8_t)KODE_OK, 64U);
    (void)net_seen();
    (void)memset(&d, 0, sizeof(d));
    d.super  = (QEvt)QEVT_INITIALIZER(NET_STREAM_DATA_SIG);
    d.len    = 64U;
    d.idx    = UINT8_MAX;                        /* no such buffer */
    d.status = (uint8_t)KODE_OK;
    d.eof    = true;
    QASM_DISPATCH(AO_AppMgr, &d.super, 0U);
    TEST_ASSERT_EQUAL_INT(APPDATA_GET_FETCH, fetch_answer("snake"));
    TEST_ASSERT_EQUAL_UINT(1U, net_seen().closes);
    TEST_ASSERT_FALSE_MESSAGE(storage_has_for(APPDATA_PUT_REQ_SIG, "snake"),
        "a folder was written from a file that never arrived");
    fetch_end();
}

TEST_CASE("a chunk that arrives before the file is opened is not that file's",
          "[appmgr_ao][appdata_get]")
{
    StorageDataEvt d;

    fetch_fresh();

    ask_get("snake", GET_URL);
    (void)net_seen();
    /* The end of a file closed a moment ago, crossing the next one's open. */
    (void)memset(&d, 0, sizeof(d));
    d.super  = (QEvt)QEVT_INITIALIZER(NET_STREAM_DATA_SIG);
    d.status = (uint8_t)KODE_OK;
    d.eof    = true;
    QASM_DISPATCH(AO_AppMgr, &d.super, 0U);
    TEST_ASSERT_NULL_MESSAGE(ao_spy_take(), "a stray chunk ended the fetch");
    TEST_ASSERT_FALSE_MESSAGE(storage_has_for(APPDATA_PUT_REQ_SIG, "snake"),
        "a stray chunk was written as an empty file");
    TEST_ASSERT_EQUAL_UINT(0U, net_seen().closes);

    fetch_opened((uint8_t)KODE_ERROR, 0U);       /* the open's own answer ends it */
    TEST_ASSERT_EQUAL_INT(APPDATA_GET_FETCH, fetch_answer("snake"));
    fetch_end();
}

TEST_CASE("a chunk that says the read failed ends the fetch",
          "[appmgr_ao][appdata_get]")
{
    StorageDataEvt d;

    fetch_fresh();

    ask_get("snake", GET_URL);
    fetch_opened((uint8_t)KODE_OK, 64U);
    (void)net_seen();
    (void)memset(&d, 0, sizeof(d));
    d.super  = (QEvt)QEVT_INITIALIZER(NET_STREAM_DATA_SIG);
    d.status = (uint8_t)KODE_TIMEOUT;
    QASM_DISPATCH(AO_AppMgr, &d.super, 0U);
    TEST_ASSERT_EQUAL_INT(APPDATA_GET_FETCH, fetch_answer("snake"));
    TEST_ASSERT_EQUAL_UINT(1U, net_seen().closes);
    fetch_end();
}

TEST_CASE("a fetch whose put is refused as busy asks again after a pause, and "
          "lands on the second try", "[appmgr_ao][appdata_get]")
{
    fetch_fresh();

    fetch_to_the_put();
    fetch_put_is_whole();
    put_rsp_busy();

    TEST_ASSERT_NULL_MESSAGE(ao_spy_take(), "a busy put ended the fetch");
    run_ticks((KODE_APPMGR_PUT_RETRY_MS / 10U) - 5U);
    TEST_ASSERT_FALSE_MESSAGE(storage_has_for(APPDATA_PUT_REQ_SIG, "snake"),
        "asked again before the pause was over");

    run_ticks(10U);
    fetch_put_is_whole();                 /* the same folder, whole, again */
    put_rsp("snake", KODE_OK);

    TEST_ASSERT_EQUAL_INT(APPDATA_GET_OK, fetch_answer("snake"));
    fetch_end();
}

TEST_CASE("a fetch whose put stays refused as busy tells busy after its tries",
          "[appmgr_ao][appdata_get]")
{
    fetch_fresh();

    fetch_to_the_put();
    for ( uint32_t i = 0U; KODE_APPMGR_PUT_TRIES > i; ++i )
    {
        fetch_put_is_whole();
        put_rsp_busy();
        TEST_ASSERT_NULL_MESSAGE(ao_spy_take(), "gave up before its last try");
        run_ticks((KODE_APPMGR_PUT_RETRY_MS / 10U) + 2U);
    }
    fetch_put_is_whole();                 /* the last try */
    put_rsp_busy();

    TEST_ASSERT_EQUAL_INT(APPDATA_GET_BUSY, fetch_answer("snake"));
    TEST_ASSERT_FALSE_MESSAGE(storage_has_for(APPDATA_PUT_REQ_SIG, "snake"),
        "asked once more than it may");
    fetch_end();
}

TEST_CASE("a put refused for another reason is not asked again",
          "[appmgr_ao][appdata_get]")
{
    fetch_fresh();

    fetch_to_the_put();
    fetch_put_is_whole();
    put_rsp("snake", KODE_DENIED);

    TEST_ASSERT_EQUAL_INT(APPDATA_GET_BUSY, fetch_answer("snake"));
    run_ticks((KODE_APPMGR_PUT_RETRY_MS / 10U) + 2U);
    TEST_ASSERT_FALSE(storage_has_for(APPDATA_PUT_REQ_SIG, "snake"));
    fetch_end();
}

/* A fetch that ends in AO_Storage's put is told by AO_Storage; every other end
 * is told here, once. */
TEST_CASE("a fetch that fails before its put says why no folder was written",
          "[appmgr_ao][appdata_get][say]")
{
    fetch_fresh();
    fake_klog_reset();

    ask_get("snake", GET_URL);
    (void)net_seen();
    fetch_opened((uint8_t)KODE_ERROR, 0U);

    TEST_ASSERT_EQUAL_INT(APPDATA_GET_FETCH, fetch_answer("snake"));
    TEST_ASSERT_EQUAL_UINT(1U, klog_count("the data folder of 'snake' was not "
                                          "written"));
    TEST_ASSERT_TRUE(klog_has("the data folder of 'snake' was not written: "
                              "its files could not be fetched"));
    fetch_end();
}

TEST_CASE("a fetch for a name that cannot be a folder says why too",
          "[appmgr_ao][appdata_get][say]")
{
    fetch_fresh();
    fake_klog_reset();

    ask_get("../etc", GET_URL);

    TEST_ASSERT_EQUAL_INT(APPDATA_GET_NAME, fetch_answer("../etc"));
    TEST_ASSERT_TRUE(klog_has("the data folder of '../etc' was not written: "
                              "it is not a name a data folder takes"));
    fetch_end();
}

TEST_CASE("a fetch whose put is answered adds no line of its own",
          "[appmgr_ao][appdata_get][say]")
{
    fetch_fresh();

    fetch_to_the_put();
    fetch_put_is_whole();
    fake_klog_reset();
    put_rsp("snake", KODE_OK);

    TEST_ASSERT_EQUAL_INT(APPDATA_GET_OK, fetch_answer("snake"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, klog_count("the data folder of 'snake' "),
        "AO_Storage says how its put ended; this AO says nothing more");
    fetch_end();
}

TEST_CASE("a second fetch asked while one runs is told busy",
          "[appmgr_ao][appdata_get]")
{
    fetch_fresh();

    ask_get("snake", GET_URL);
    (void)net_seen();
    ask_get("ladder", GET_URL);
    TEST_ASSERT_EQUAL_INT(APPDATA_GET_BUSY, fetch_answer("ladder"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, net_seen().opens,
        "the second fetch opened a transfer over the first's");

    fetch_opened((uint8_t)KODE_ERROR, 0U);         /* the first goes on, and ends */
    TEST_ASSERT_EQUAL_INT(APPDATA_GET_FETCH, fetch_answer("snake"));
    fetch_end();
}

TEST_CASE("a fetch asked while an image is written is told busy",
          "[appmgr_ao][appdata_get]")
{
    uint32_t const len = image_named("snake");

    fetch_fresh();

    ask_recv(len, 0xDEADBEEFU, false);           /* a write in hand */
    raw_armed(true);
    ask_get("snake", GET_URL);
    TEST_ASSERT_EQUAL_INT(APPDATA_GET_BUSY, fetch_answer("snake"));
    TEST_ASSERT_EQUAL_UINT(0U, net_seen().opens);

    feed(s_named_img, (uint16_t)len);            /* the write ends, CRC refused */
    run_ticks(APPMGR_ANSWER_DRAIN_DEADLINE_TICKS + 2U);
    fetch_end();
}

TEST_CASE("a fetch nobody answers is given up on", "[appmgr_ao][appdata_get]")
{
    fetch_fresh();

    ask_get("snake", GET_URL);
    (void)net_seen();
    run_ticks(APPMGR_OPEN_DEADLINE_TICKS - 50U);
    TEST_ASSERT_NULL_MESSAGE(ao_spy_take(), "gave up before the deadline");

    run_ticks(100U);
    TEST_ASSERT_EQUAL_INT(APPDATA_GET_FETCH, fetch_answer("snake"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, net_seen().closes,
        "the transfer asked for was never given back");
    fetch_end();
}

TEST_CASE("the fetch's request refuses what it cannot post",
          "[appmgr_ao][appdata_get]")
{
    static char long_url[KODE_APPDATA_URL_MAX + 1U];

    fetch_fresh();
    (void)memset(long_url, 'u', sizeof(long_url) - 1U);
    long_url[sizeof(long_url) - 1U] = '\0';

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, appmgr_appdata_get_request(NULL, GET_URL, ao_spy()));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, appmgr_appdata_get_request("snake", NULL, ao_spy()));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, appmgr_appdata_get_request("snake", GET_URL, NULL));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_ERROR,
        appmgr_appdata_get_request("snake", long_url, ao_spy()),
        "a URL that does not fit the request was cut, not refused");
    TEST_ASSERT_EQUAL_UINT(0U, (unsigned)uxQueueMessagesWaiting(AO_AppMgr->eQueue));

    TEST_ASSERT_EQUAL_INT(KODE_OK, appmgr_appdata_get_request("snake", GET_URL, ao_spy()));
    (void)qf_support_pump(AO_AppMgr, 8U);
    net_seen_t const n = net_seen();
    TEST_ASSERT_EQUAL_UINT(1U, n.opens);
    TEST_ASSERT_EQUAL_STRING(GET_URL "app.json", n.url);
    fetch_opened((uint8_t)KODE_ERROR, 0U);
    ao_spy_drain();
    fetch_end();
}

TEST_CASE("a fetch of an app's data folder shows from its first moment and ends the card",
          "[appmgr_ao][beat]")
{
    fetch_fresh();
    ao_spy_subscribe(APP_FLASH_PROGRESS_SIG);
    ao_spy_subscribe(APP_FLASH_FAIL_SIG);

    ask_get("snake", GET_URL);
    (void)net_seen();
    progress_seen_t seen = spy_progress();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, seen.count,
        "a fetch raises nothing on the glass until it is over");
    TEST_ASSERT_EQUAL_UINT(0U, seen.pct[0]);
    TEST_ASSERT_EQUAL_STRING("snake", seen.first.project);

    run_ticks((2U * APPMGR_WAIT_BEAT_TICKS) + 10U);
    seen = spy_progress();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, seen.count,
        "a fetch waiting on the server fell silent");

    fetch_opened((uint8_t)KODE_ERROR, 0U);             /* the server says no */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, spy_count(APP_FLASH_FAIL_SIG),
        "the card the fetch raised is left up");
    run_ticks(3U * APPMGR_WAIT_BEAT_TICKS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, spy_progress().count,
        "the beat went on after the fetch ended");

    QActive_unsubscribe(ao_spy(), APP_FLASH_PROGRESS_SIG);
    QActive_unsubscribe(ao_spy(), APP_FLASH_FAIL_SIG);
    fetch_end();
}
