/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_apps.c
 * @brief `nvs_set`, held to the apps' share, and the lines that remove things:
 *        `nvs_del`, `app_erase` and `app_list`.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kcli_proto.h"
#include "khal_nvs.h"
#include "kode_sys.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

/* The command watchdog, in QP ticks, as the other console files repeat it. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)

/** @brief A key the cases store and delete. */
#define KEY_DOOMED  "doomed"

/** @brief A key that must survive every delete aimed at the one above. */
#define KEY_BYSTANDER  "bystander"

/** @brief The value kept under both keys. */
#define VALUE_KEPT  "kept"

/** @brief Room for a console line the cases build. */
#define LINE_MAX_TEST  96U

/* ---- The harness ------------------------------------------------------------ */

/** @brief Empty AO_AppMgr's queue, releasing what the console posted. */
static void appmgr_queue_drained(void)
{
    const QEvt *e;

    qf_support_give_queue(AO_AppMgr);
    while ( xQueueReceive(AO_AppMgr->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief AO_Console up, the store and every queue empty. */
static void console_up(void)
{
    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();
    owners_up();
    if ( qf_support_claim_ao(AO_Console) )
    {
        Console_ctor();
        qf_support_give_queue(AO_Console);
        AO_Console->prio  = KODE_PRIO_CONSOLE;
        AO_Console->pthre = KODE_PRIO_CONSOLE;
        QActive_register_(AO_Console);
        QASM_INIT(AO_Console, (void *)0, 0U);
        qf_support_background(AO_Console);
    }
    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS + 1U);
    appmgr_queue_drained();
    fake_klog_reset();
    fake_neighbours_reset();
    fake_nvs_reset();
}

/** @brief Leave the store and AO_AppMgr's queue as the next case expects. */
static void console_end(void)
{
    fake_nvs_reset();
    appmgr_queue_drained();
    ao_spy_drain();
}

/** @brief Send @p text in as a line typed on the cable. */
static void type_line(const char *text)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = (uint8_t)KODE_CONSOLE_SRC_SERIAL;
    (void)snprintf(e.line, sizeof(e.line), "%s", text);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

/** @brief Send @p text in as a line from the port @p src (KODE_CONSOLE_SRC_*). */
static void type_line_from(const char *text, uint8_t src)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = src;
    (void)snprintf(e.line, sizeof(e.line), "%s", text);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

/** @brief Type @p text with the captured replies cleared first. */
static void ask(const char *text)
{
    fake_klog_reset();
    type_line(text);
}

/** @brief Whether any captured reply contains @p needle. */
static bool replied(const char *needle)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Take what the console posted to AO_AppMgr, oldest first.
 *
 * @param sig     The signal of the one event wanted.
 * @param file    Where its file name is copied, for APP_ERASE_REQ_SIG;
 *                may be NULL.
 * @param file_sz Size of @p file.
 * @param skip    Where its page offset is copied, for APP_LIST_REQ_SIG;
 *                may be NULL.
 * @param who_ok  Set to whether a list request names the console as asker;
 *                may be NULL.
 * @return How many events of @p sig were found; anything else is released.
 */
static unsigned take_posted(enum_t sig, char *file, size_t file_sz,
                            uint16_t *skip, bool *who_ok)
{
    const QEvt *e;
    unsigned    n = 0U;

    while ( xQueueReceive(AO_AppMgr->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)sig == e->sig )
        {
            ++n;
            if ( (APP_ERASE_REQ_SIG == sig) && (NULL != file) )
            {
                (void)snprintf(file, file_sz, "%s", ((AppFlashEvt const *)e)->file);
            }
            if ( (APP_LIST_REQ_SIG == sig) && (NULL != skip) )
            {
                AppListReqEvt const * const req = (AppListReqEvt const *)e;
                *skip = req->skip;
                if ( NULL != who_ok )
                {
                    *who_ok = (req->who == AO_Console);
                }
            }
        }
        QF_gc(e);
    }
    return n;
}

/** @brief Store @p value under @p key where the console's verbs look. */
static void nvs_put(const char *key, const char *value)
{
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_nvs_set_str(KODE_NVS_NAMESPACE, key, value));
}

/** @brief Whether the console's namespace still holds @p key. */
static bool nvs_has(const char *key)
{
    char v[KODE_SYS_NVS_VALUE_MAX];

    return khal_nvs_get_str(KODE_NVS_NAMESPACE, key, v, sizeof(v)) == KODE_OK;
}

/* ---- nvs_set ----------------------------------------------------------------- */

/* The console writes where the apps do, so it is held to the same share. */
TEST_CASE("nvs_set stores the pair, held to the apps' share", "[console_apps]")
{
    console_up();

    ask("nvs_set key=\"" KEY_DOOMED "\" value=\"" VALUE_KEPT "\"");

    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_TRUE(nvs_has(KEY_DOOMED));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_NVS_SHARE_MAX, (uint32_t)fake_nvs_last_share());

    console_end();
}

TEST_CASE("nvs_set past the apps' share is refused as full and keeps nothing",
          "[console_apps]")
{
    console_up();
    fake_nvs_full(true);

    ask("nvs_set key=\"" KEY_DOOMED "\" value=\"" VALUE_KEPT "\"");

    TEST_ASSERT_TRUE(replied(">err code=5 msg=full"));
    TEST_ASSERT_FALSE(replied(">ok"));
    TEST_ASSERT_FALSE(nvs_has(KEY_DOOMED));

    console_end();
}

/* ---- nvs_del ----------------------------------------------------------------- */

/* The one key named goes, and only it: a delete that took its neighbours would
 * wipe an app's provisioning on a typo. */
TEST_CASE("nvs_del removes the key it names and no other", "[console_apps]")
{
    console_up();
    nvs_put(KEY_DOOMED, VALUE_KEPT);
    nvs_put(KEY_BYSTANDER, VALUE_KEPT);

    ask("nvs_del key=\"" KEY_DOOMED "\"");

    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_FALSE(replied(">err"));
    TEST_ASSERT_FALSE(nvs_has(KEY_DOOMED));
    TEST_ASSERT_TRUE(nvs_has(KEY_BYSTANDER));

    console_end();
}

/* Forgetting what was never remembered is a success, so a host script can
 * delete without asking first. */
TEST_CASE("nvs_del of a key that was never stored still answers ok",
          "[console_apps]")
{
    console_up();
    nvs_put(KEY_BYSTANDER, VALUE_KEPT);

    ask("nvs_del key=\"neverwritten\"");

    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_TRUE(nvs_has(KEY_BYSTANDER));

    console_end();
}

/* A bare word is not a string on this wire. If it were accepted the delete
 * would go through for a line the grammar says is wrong, and there would be
 * two ways to type one thing. */
TEST_CASE("nvs_del with an unquoted key is refused and deletes nothing",
          "[console_apps]")
{
    console_up();
    nvs_put(KEY_DOOMED, VALUE_KEPT);

    ask("nvs_del key=" KEY_DOOMED);

    TEST_ASSERT_TRUE(replied(">err code=3 msg=key"));
    TEST_ASSERT_FALSE(replied(">ok"));
    TEST_ASSERT_TRUE(nvs_has(KEY_DOOMED));

    console_end();
}

TEST_CASE("nvs_del with no key is refused and deletes nothing", "[console_apps]")
{
    console_up();
    nvs_put(KEY_DOOMED, VALUE_KEPT);

    ask("nvs_del");

    TEST_ASSERT_TRUE(replied(">err code=2 msg=key"));
    TEST_ASSERT_FALSE(replied(">ok"));
    TEST_ASSERT_TRUE(nvs_has(KEY_DOOMED));

    console_end();
}

/* The store's ceiling is a name one character shorter than
 * KODE_SYS_NVS_KEY_MAX; one that long is a mistyped line, and it is told so
 * instead of being cut to a different key and deleting that. */
TEST_CASE("nvs_del with a key too long for the store is refused", "[console_apps]")
{
    char key[KODE_SYS_NVS_KEY_MAX + 1U];
    char line[LINE_MAX_TEST];

    console_up();
    (void)memset(key, 'k', sizeof(key));
    key[KODE_SYS_NVS_KEY_MAX] = '\0';
    nvs_put(KEY_DOOMED, VALUE_KEPT);

    (void)snprintf(line, sizeof(line), "nvs_del key=\"%s\"", key);
    ask(line);

    TEST_ASSERT_TRUE(replied(">err code=3 msg=key"));
    TEST_ASSERT_FALSE(replied(">ok"));
    TEST_ASSERT_TRUE(nvs_has(KEY_DOOMED));

    console_end();
}

/* ---- app_run ------------------------------------------------------------------ */

/** @brief Hand the console AO_AppMgr's verdict on a request, as it posts it. */
static void verdict(app_req_verdict_t v)
{
    AppReqRspEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super   = (QEvt)QEVT_INITIALIZER(APP_REQ_RSP_SIG);
    e.verdict = (uint8_t)v;
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

/** @brief The one request of kind @p sig AO_AppMgr's queue holds, or NULL; the caller frees. */
static const QEvt *appmgr_took(enum_t sig)
{
    const QEvt *e = NULL;

    if ( (xQueueReceive(AO_AppMgr->eQueue, &e, 0U) == pdTRUE) && ((enum_t)e->sig != sig) )
    {
        QF_gc(e);
        e = NULL;
    }
    return e;
}

/* `>ok` on an `app_run` says AO_AppMgr has the app on its way to boot. A name
 * nobody installed, or an AO_AppMgr with work in hand, is told to the person
 * who typed it. */
TEST_CASE("app_run waits for AO_AppMgr's verdict on the installed app it names",
          "[console_apps]")
{
    console_up();
    ask("app_run name=\"scope\"");

    const AppFlashEvt * const req = (const AppFlashEvt *)appmgr_took(APP_RUN_REQ_SIG);
    TEST_ASSERT_NOT_NULL(req);
    TEST_ASSERT_EQUAL_STRING("scope", req->file);
    TEST_ASSERT_TRUE_MESSAGE(AO_Console == req->who,
        "the request does not say who is waiting for the verdict");
    QF_gc(&req->super);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "the line answered before the verdict");

    verdict(APP_REQ_TAKEN);
    TEST_ASSERT_TRUE(replied(">ok"));
    console_end();
}

TEST_CASE("app_run of an app that is not installed, or while AO_AppMgr is busy, is an error",
          "[console_apps]")
{
    console_up();
    ask("app_run name=\"ghost\"");
    appmgr_queue_drained();
    verdict(APP_REQ_MISSING);
    TEST_ASSERT_TRUE(replied(">err code=5 msg=missing"));
    TEST_ASSERT_FALSE(replied(">ok"));

    ask("app_run name=\"scope\"");
    appmgr_queue_drained();
    verdict(APP_REQ_BUSY);
    TEST_ASSERT_TRUE(replied(">err code=4 msg=busy"));
    TEST_ASSERT_FALSE(replied(">ok"));
    console_end();
}

TEST_CASE("app_run of a .bin asks for it to be installed and booted, and waits the same way",
          "[console_apps]")
{
    console_up();
    ask("app_run name=\"scope.bin\"");

    const FlashReqEvt * const req = (const FlashReqEvt *)appmgr_took(FLASH_REQ_SIG);
    TEST_ASSERT_NOT_NULL(req);
    TEST_ASSERT_EQUAL_STRING("scope.bin", req->src);
    TEST_ASSERT_TRUE(req->launch);
    TEST_ASSERT_TRUE(AO_Console == req->who);
    QF_gc(&req->super);
    TEST_ASSERT_FALSE(replied(">ok"));

    verdict(APP_REQ_MISSING);
    TEST_ASSERT_TRUE(replied(">err code=5 msg=missing"));
    console_end();
}

/* `install_info` is AO_AppMgr's to answer: the records are its, and it writes
 * them to the port the command came on before the console ends the line. */
TEST_CASE("install_info asks AO_AppMgr for the records, from the port it came on, "
          "and ends on its answer", "[console_apps]")
{
    console_up();
    ask("install_info");

    const InstallInfoReqEvt * const req =
        (const InstallInfoReqEvt *)appmgr_took(INSTALL_INFO_REQ_SIG);
    TEST_ASSERT_NOT_NULL(req);
    TEST_ASSERT_TRUE(AO_Console == req->who);
    TEST_ASSERT_EQUAL_UINT8(KODE_CONSOLE_SRC_SERIAL, req->src);
    QF_gc(&req->super);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "the line answered before the records");

    verdict(APP_REQ_TAKEN);
    TEST_ASSERT_TRUE(replied(">ok"));
    console_end();
}

/* The port a command came on is the one its records go to: `install` and
 * `install_info` from BLE name BLE to AO_AppMgr. */
TEST_CASE("install and install_info from BLE name BLE as the asker",
          "[console_apps]")
{
    console_up();
    fake_klog_reset();
    type_line_from("install_info", (uint8_t)KODE_CONSOLE_SRC_BLE);
    const InstallInfoReqEvt * const info =
        (const InstallInfoReqEvt *)appmgr_took(INSTALL_INFO_REQ_SIG);
    uint8_t info_src = (uint8_t)KODE_LOG_SRC_NONE;
    if ( NULL != info )
    {
        info_src = info->src;
        QF_gc(&info->super);
    }
    verdict(APP_REQ_TAKEN);
    console_end();

    console_up();
    fake_klog_reset();
    type_line_from("install src=\"sd:/notes.bin\" dst=app", (uint8_t)KODE_CONSOLE_SRC_BLE);
    const FlashReqEvt * const req = (const FlashReqEvt *)appmgr_took(FLASH_REQ_SIG);
    uint8_t req_src = (uint8_t)KODE_LOG_SRC_NONE;
    if ( NULL != req )
    {
        req_src = req->asker_src;
        QF_gc(&req->super);
    }
    verdict(APP_REQ_MISSING);
    console_end();

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(KODE_CONSOLE_SRC_BLE, info_src,
        "install_info from BLE named another port");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(KODE_CONSOLE_SRC_BLE, req_src,
        "install from BLE named another port as its asker");
}

/* ---- app_erase --------------------------------------------------------------- */

/**
 * @brief Publish the end of an erase to the console, as AO_AppMgr does.
 * @param name   The app the erase was about.
 * @param st     How it ended.
 * @param image  An installed image was removed.
 * @param fs     What happened on the internal partition.
 * @param sd     What happened on the card.
 */
static void erase_ends(const char *name, kode_status_t st, bool image,
                       AppWipeMedium fs, AppWipeMedium sd)
{
    AppEraseDoneEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(APP_ERASE_DONE_SIG);
    (void)snprintf(e.name, sizeof(e.name), "%s", name);
    e.status = (uint8_t)st;
    e.image  = image;
    e.fs     = fs;
    e.sd     = sd;
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

/** @brief A medium that was there, with @p files removed and @p left behind. */
static AppWipeMedium medium_with(uint8_t files, uint8_t left, AppWipeFolder folder)
{
    AppWipeMedium const m = { .present = true, .files = files, .left = left,
                              .folder = (uint8_t)folder };
    return m;
}

/** @brief A medium that was not there. */
static AppWipeMedium medium_absent(void)
{
    AppWipeMedium const m = { .present = false, .files = 0U, .left = 0U,
                              .folder = (uint8_t)APP_WIPE_FOLDER_NONE };
    return m;
}

/* The erase itself is AO_AppMgr's, on its own thread; the console hands it the
 * name exactly as typed, once, and keeps the line open until the erase says
 * how it ended: an `>ok` before that would promise what has not happened. */
TEST_CASE("app_erase posts the erase request with the name it was typed with, "
          "and waits for its end", "[console_apps]")
{
    char name[KODE_APP_FILE_MAX];

    console_up();

    ask("app_erase name=\"scope\"");

    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"),
                              "the line closed before the erase ended");
    TEST_ASSERT_EQUAL_UINT(1U, take_posted(APP_ERASE_REQ_SIG, name, sizeof(name),
                                           NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("scope", name);

    erase_ends("scope", KODE_OK, true,
               medium_with(1U, 0U, APP_WIPE_FOLDER_REMOVED),
               medium_with(2U, 0U, APP_WIPE_FOLDER_NONE));

    TEST_ASSERT_TRUE(replied(">|erased name=\"scope\" image=removed"));
    TEST_ASSERT_TRUE(replied(">|erased where=fs files=1 left=0 folder=removed"));
    TEST_ASSERT_TRUE(replied(">|erased where=sd files=2 left=0 folder=none"));
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_FALSE(replied(">err"));

    console_end();
}

/* A board with no card has still lost the app: the card is said absent, and
 * that is no error. */
TEST_CASE("app_erase with no card says the card is absent and ends well",
          "[console_apps]")
{
    console_up();

    ask("app_erase name=\"scope\"");
    erase_ends("scope", KODE_OK, false,
               medium_with(1U, 0U, APP_WIPE_FOLDER_NONE), medium_absent());

    TEST_ASSERT_TRUE(replied(">|erased name=\"scope\" image=none"));
    TEST_ASSERT_TRUE(replied(">|erased where=fs files=1 left=0 folder=none"));
    TEST_ASSERT_TRUE(replied(">|erased where=sd state=absent"));
    TEST_ASSERT_TRUE(replied(">ok"));

    console_end();
}

/* What would not go is in the records, and the line ends as a failure: a
 * script that reads only the last line must not take a half erase for a
 * whole one. */
TEST_CASE("app_erase that leaves a file behind says where and ends as an error",
          "[console_apps]")
{
    console_up();

    ask("app_erase name=\"scope\"");
    erase_ends("scope", KODE_IO_ERROR, true,
               medium_with(1U, 0U, APP_WIPE_FOLDER_REMOVED),
               medium_with(0U, 1U, APP_WIPE_FOLDER_LEFT));

    TEST_ASSERT_TRUE(replied(">|erased name=\"scope\" image=removed"));
    TEST_ASSERT_TRUE(replied(">|erased where=sd files=0 left=1 folder=left"));
    TEST_ASSERT_TRUE(replied(">err code=5 msg=left"));
    TEST_ASSERT_FALSE(replied(">ok"));

    console_end();
}

TEST_CASE("app_erase of a name with nothing anywhere answers missing",
          "[console_apps]")
{
    console_up();

    ask("app_erase name=\"nobody\"");
    erase_ends("nobody", KODE_ERROR, false,
               medium_with(0U, 0U, APP_WIPE_FOLDER_NONE),
               medium_with(0U, 0U, APP_WIPE_FOLDER_NONE));

    TEST_ASSERT_TRUE(replied(">err code=5 msg=missing"));
    TEST_ASSERT_FALSE(replied(">|erased"));
    TEST_ASSERT_FALSE(replied(">ok"));

    console_end();
}

TEST_CASE("app_erase refused by a busy AO_AppMgr answers busy", "[console_apps]")
{
    console_up();

    ask("app_erase name=\"scope\"");
    erase_ends("scope", KODE_DENIED, false, medium_absent(), medium_absent());

    TEST_ASSERT_TRUE(replied(">err code=4 msg=busy"));
    TEST_ASSERT_FALSE(replied(">|erased"));
    TEST_ASSERT_FALSE(replied(">ok"));

    console_end();
}

/* The image went and AO_Storage then refused or kept silent: the record says
 * the image is gone, and the error that the files were not looked at. */
TEST_CASE("app_erase whose files were not looked at says the image went",
          "[console_apps]")
{
    console_up();

    ask("app_erase name=\"scope\"");
    erase_ends("scope", KODE_TIMEOUT, true, medium_absent(), medium_absent());

    TEST_ASSERT_TRUE(replied(">|erased name=\"scope\" image=removed"));
    TEST_ASSERT_FALSE(replied("where="));
    TEST_ASSERT_TRUE(replied(">err code=6 msg=storage"));
    TEST_ASSERT_FALSE(replied(">ok"));

    console_end();
}

/* The end of an erase is published: the launcher's erase of another app ends
 * on the same signal, and must not close this line. */
TEST_CASE("app_erase is not closed by the end of another app's erase",
          "[console_apps]")
{
    console_up();

    ask("app_erase name=\"scope\"");
    erase_ends("other", KODE_OK, true,
               medium_with(0U, 0U, APP_WIPE_FOLDER_NONE), medium_absent());

    TEST_ASSERT_FALSE(replied(">|erased"));
    TEST_ASSERT_FALSE(replied(">ok"));

    erase_ends("scope", KODE_OK, true,
               medium_with(0U, 0U, APP_WIPE_FOLDER_NONE), medium_absent());
    TEST_ASSERT_TRUE(replied(">ok"));

    console_end();
}

/* Erasing an app cannot be undone, so an unquoted name must not reach
 * AO_AppMgr at all. */
TEST_CASE("app_erase with an unquoted name is refused and posts nothing",
          "[console_apps]")
{
    console_up();

    ask("app_erase name=scope");

    TEST_ASSERT_TRUE(replied(">err code=3 msg=name"));
    TEST_ASSERT_FALSE(replied(">ok"));
    TEST_ASSERT_EQUAL_UINT(0U, take_posted(APP_ERASE_REQ_SIG, NULL, 0U, NULL, NULL));

    console_end();
}

TEST_CASE("app_erase with no name is refused and posts nothing", "[console_apps]")
{
    console_up();

    ask("app_erase");

    TEST_ASSERT_TRUE(replied(">err code=2 msg=name"));
    TEST_ASSERT_FALSE(replied(">ok"));
    TEST_ASSERT_EQUAL_UINT(0U, take_posted(APP_ERASE_REQ_SIG, NULL, 0U, NULL, NULL));

    console_end();
}

/* ---- app_list ---------------------------------------------------------------- */

/* The listing is a conversation: the console asks the first page, from the
 * start, and names itself so the page comes back to it alone. */
TEST_CASE("app_list asks the first page from the start and keeps the line open",
          "[console_apps]")
{
    uint16_t skip = 1U;
    bool     who  = false;

    console_up();

    ask("app_list");

    TEST_ASSERT_EQUAL_UINT(1U, take_posted(APP_LIST_REQ_SIG, NULL, 0U, &skip, &who));
    TEST_ASSERT_EQUAL_UINT16(0U, skip);
    TEST_ASSERT_TRUE(who);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"),
                              "the line closed before AO_AppMgr answered");
    TEST_ASSERT_FALSE(replied(">err"));

    /* The empty list closes it, so the case leaves no command open. */
    {
        static const ListEndEvt end = {
            .super = QEVT_INITIALIZER(APP_LIST_END_SIG), .more = false };
        QASM_DISPATCH(AO_Console, &end.super, 0U);
    }
    TEST_ASSERT_TRUE(replied(">ok"));

    console_end();
}

/* app_list takes no argument; one that is typed is a mistake the console
 * says so about, rather than a listing that quietly ignores it. */
TEST_CASE("app_list with an argument is refused and asks nobody", "[console_apps]")
{
    console_up();

    ask("app_list name=\"scope\"");

    TEST_ASSERT_TRUE(replied(">err code=3 msg=name"));
    TEST_ASSERT_FALSE(replied(">ok"));
    TEST_ASSERT_EQUAL_UINT(0U, take_posted(APP_LIST_REQ_SIG, NULL, 0U, NULL, NULL));

    console_end();
}

/* ---- app_list files=on -------------------------------------------------------- */

/** @brief A file name on the card, its project, and what its manifest says. */
#define FILE_SD        "kody.bin"
#define FILE_SD_PATH   "/sdcard/kody.bin"
#define FILE_FS        "servo.bin"
#define FILE_FS_PATH   "/storage/servo.bin"

/** @brief Empty AO_Storage's queue, releasing what the console posted. */
static void storage_queue_drained(void)
{
    const QEvt *e;

    qf_support_give_queue(AO_Storage);
    while ( xQueueReceive(AO_Storage->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/**
 * @brief Take what the console posted to AO_Storage, oldest first.
 *
 * @param sig  STORAGE_LIST_PAGE_SIG or STORAGE_DESC_REQ_SIG.
 * @param skip Where a page request's offset is copied; may be NULL.
 * @param name Where a manifest request's name is copied; may be NULL.
 * @param name_sz Size of @p name.
 * @return How many events of @p sig were found, each naming the console as
 *         its asker; anything else is released.
 */
static unsigned take_storage(enum_t sig, uint16_t *skip, char *name, size_t name_sz)
{
    const QEvt *e;
    unsigned    n = 0U;

    while ( xQueueReceive(AO_Storage->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)sig == e->sig )
        {
            ++n;
            if ( STORAGE_LIST_PAGE_SIG == sig )
            {
                StorageListPageEvt const * const req = (StorageListPageEvt const *)e;
                TEST_ASSERT_TRUE_MESSAGE(req->who == AO_Console,
                                         "the page would not come back to the console");
                if ( NULL != skip )
                {
                    *skip = req->skip;
                }
            }
            else
            {
                StorageDescReqEvt const * const req = (StorageDescReqEvt const *)e;
                TEST_ASSERT_TRUE_MESSAGE(req->who == AO_Console,
                                         "the manifest would not come back to the console");
                if ( NULL != name )
                {
                    (void)snprintf(name, name_sz, "%s", req->name);
                }
            }
        }
        QF_gc(e);
    }
    return n;
}

/** @brief One installed app at @p pos, as AO_AppMgr posts it. */
static void installed_app(uint16_t pos, const char *name)
{
    AppEntryEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(APP_ENTRY_SIG);
    (void)snprintf(e.name, sizeof(e.name), "%s", name);
    (void)snprintf(e.version, sizeof(e.version), "0.3.1");
    e.size = 240U * 1024U;
    e.pos  = pos;
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

/** @brief The end of a page: of the installed apps, or of the app files. */
static void page_ends(enum_t sig, bool more)
{
    ListEndEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(sig);
    e.more  = more;
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

/** @brief One app file at @p pos, as AO_Storage's listing posts it. */
static void app_file(uint16_t pos, const char *name, uint32_t size, bool from_sd)
{
    StorageEntryEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(STORAGE_ENTRY_SIG);
    (void)snprintf(e.name, sizeof(e.name), "%s", name);
    e.size    = size;
    e.from_sd = from_sd;
    e.pos     = pos;
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

/** @brief What the file asked as @p asked says it is, or that it would not be read. */
static void manifest(const char *asked, kode_status_t st, const char *project,
                     const char *version, const char *display, const char *category)
{
    StorageDescRspEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(STORAGE_DESC_RSP_SIG);
    (void)snprintf(e.name, sizeof(e.name), "%s", asked);
    (void)snprintf(e.project, sizeof(e.project), "%s", project);
    (void)snprintf(e.version, sizeof(e.version), "%s", version);
    (void)snprintf(e.display, sizeof(e.display), "%s", display);
    (void)snprintf(e.category, sizeof(e.category), "%s", category);
    e.status = (uint8_t)st;
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

/** @brief `app_list files=on` taken as far as the end of the installed apps. */
static void files_listing_begun(void)
{
    console_up();
    storage_queue_drained();
    ask("app_list files=on");
    installed_app(0U, "servo");
    page_ends(APP_LIST_END_SIG, false);
}

/* Without the key, and with it off, the answer is the one every host already
 * reads: no `at=`, and nothing asked of AO_Storage. */
TEST_CASE("app_list without files, or with files=off, answers the installed apps as before",
          "[console_apps][app_list_files]")
{
    static const char * const LINES[] = { "app_list", "app_list files=off" };

    for ( unsigned i = 0U; (sizeof(LINES) / sizeof(LINES[0])) > i; ++i )
    {
        console_up();
        storage_queue_drained();

        ask(LINES[i]);
        installed_app(0U, "servo");
        page_ends(APP_LIST_END_SIG, false);

        TEST_ASSERT_TRUE(replied(">|name=\"servo\" version=\"0.3.1\" size_kb=240 "
                                 "category=\"other\" display=\"servo\""));
        TEST_ASSERT_FALSE(replied(" at="));
        TEST_ASSERT_TRUE(replied(">ok"));
        TEST_ASSERT_EQUAL_UINT(0U, take_storage(STORAGE_LIST_PAGE_SIG, NULL, NULL, 0U));

        console_end();
    }
}

/* With the key on, an installed app says so, and the end of the installed
 * apps is not the end: the first page of the app files is asked, from the
 * start, and the line stays open for it. */
TEST_CASE("app_list files=on marks an installed app and goes on to the app files",
          "[console_apps][app_list_files]")
{
    uint16_t skip = 1U;

    files_listing_begun();

    TEST_ASSERT_TRUE(replied(">|name=\"servo\" version=\"0.3.1\" size_kb=240 "
                             "category=\"other\" display=\"servo\" at=app"));
    TEST_ASSERT_FALSE(replied(" file="));
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "the line closed before the files were listed");
    TEST_ASSERT_EQUAL_UINT(1U, take_storage(STORAGE_LIST_PAGE_SIG, &skip, NULL, 0U));
    TEST_ASSERT_EQUAL_UINT16(0U, skip);

    /* No file anywhere closes it. */
    page_ends(STORAGE_LIST_END_SIG, false);
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_FALSE(replied(">err"));

    console_end();
}

/* Each file is asked what it says it is, by the path of the medium the
 * listing found it on, one at a time; its record carries the manifest's
 * words, the file's size, where it is and its name. */
TEST_CASE("app_list files=on prints one record per app file, from its manifest",
          "[console_apps][app_list_files]")
{
    char asked[KODE_APP_FILE_MAX];

    files_listing_begun();
    storage_queue_drained();

    app_file(0U, FILE_SD, 812U * 1024U, true);
    app_file(1U, FILE_FS, 240U * 1024U, false);
    page_ends(STORAGE_LIST_END_SIG, false);

    asked[0] = '\0';
    TEST_ASSERT_EQUAL_UINT(1U, take_storage(STORAGE_DESC_REQ_SIG, NULL, asked, sizeof(asked)));
    TEST_ASSERT_EQUAL_STRING(FILE_SD_PATH, asked);
    TEST_ASSERT_FALSE(replied(">ok"));

    manifest(FILE_SD_PATH, KODE_OK, "kody-runner", "1.2.0", "Kody Runner", "games");
    TEST_ASSERT_TRUE(replied(">|name=\"kody-runner\" version=\"1.2.0\" size_kb=812 "
                             "category=\"games\" display=\"Kody Runner\" at=sd "
                             "file=\"" FILE_SD "\""));

    asked[0] = '\0';
    TEST_ASSERT_EQUAL_UINT(1U, take_storage(STORAGE_DESC_REQ_SIG, NULL, asked, sizeof(asked)));
    TEST_ASSERT_EQUAL_STRING(FILE_FS_PATH, asked);
    TEST_ASSERT_FALSE(replied(">ok"));

    /* An image with no manifest: the same fallbacks as an installed app. */
    manifest(FILE_FS_PATH, KODE_OK, "servo", "", "", "");
    TEST_ASSERT_TRUE(replied(">|name=\"servo\" version=- size_kb=240 "
                             "category=\"other\" display=\"servo\" at=fs "
                             "file=\"" FILE_FS "\""));
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_FALSE(replied(">err"));

    console_end();
}

/* A .bin that is no app image, or that would not be read, is no app: it gets
 * no record and the listing goes on. */
TEST_CASE("app_list files=on leaves out a file whose manifest does not read",
          "[console_apps][app_list_files]")
{
    files_listing_begun();
    storage_queue_drained();

    app_file(0U, FILE_SD, 4096U, true);
    app_file(1U, FILE_FS, 240U * 1024U, false);
    page_ends(STORAGE_LIST_END_SIG, false);

    manifest(FILE_SD_PATH, KODE_ERROR, "", "", "", "");
    TEST_ASSERT_FALSE(replied("file=\"" FILE_SD "\""));
    TEST_ASSERT_FALSE(replied(">ok"));

    /* Read, and with no project name in it. */
    manifest(FILE_FS_PATH, KODE_OK, "", "", "", "");
    TEST_ASSERT_FALSE(replied("file=\"" FILE_FS "\""));
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_FALSE(replied(">err"));

    console_end();
}

/* Answers can cross: one that echoes another name is not the manifest of the
 * file in hand, and neither prints a record nor moves the listing on. */
TEST_CASE("app_list files=on ignores a manifest it did not ask for",
          "[console_apps][app_list_files]")
{
    files_listing_begun();
    storage_queue_drained();

    app_file(0U, FILE_SD, 812U * 1024U, true);
    page_ends(STORAGE_LIST_END_SIG, false);
    TEST_ASSERT_EQUAL_UINT(1U, take_storage(STORAGE_DESC_REQ_SIG, NULL, NULL, 0U));

    manifest("/sdcard/other.bin", KODE_OK, "other", "9.9.9", "Other", "games");
    TEST_ASSERT_FALSE(replied("9.9.9"));
    TEST_ASSERT_FALSE(replied(">ok"));
    TEST_ASSERT_EQUAL_UINT(0U, take_storage(STORAGE_DESC_REQ_SIG, NULL, NULL, 0U));

    manifest(FILE_SD_PATH, KODE_OK, "kody-runner", "1.2.0", "Kody Runner", "games");
    TEST_ASSERT_TRUE(replied(" at=sd file=\"" FILE_SD "\""));
    TEST_ASSERT_TRUE(replied(">ok"));

    console_end();
}

/* A listing longer than a page: the next page is asked only once every file
 * of this one has its record, from where this one stopped. */
TEST_CASE("app_list files=on asks the next page of files after the manifests of this one",
          "[console_apps][app_list_files]")
{
    uint16_t skip = 0U;

    files_listing_begun();
    storage_queue_drained();

    app_file(0U, FILE_SD, 812U * 1024U, true);
    page_ends(STORAGE_LIST_END_SIG, true);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, take_storage(STORAGE_LIST_PAGE_SIG, NULL, NULL, 0U),
                                   "the next page was asked before this one was through");

    manifest(FILE_SD_PATH, KODE_OK, "kody-runner", "1.2.0", "Kody Runner", "games");
    TEST_ASSERT_FALSE(replied(">ok"));
    TEST_ASSERT_EQUAL_UINT(1U, take_storage(STORAGE_LIST_PAGE_SIG, &skip, NULL, 0U));
    TEST_ASSERT_EQUAL_UINT16(1U, skip);

    /* The same entry again is not new, and brings no second manifest. */
    app_file(0U, FILE_SD, 812U * 1024U, true);
    app_file(1U, FILE_FS, 240U * 1024U, false);
    page_ends(STORAGE_LIST_END_SIG, false);
    manifest(FILE_FS_PATH, KODE_OK, "servo", "0.3.1", "", "");
    TEST_ASSERT_TRUE(replied(" at=fs file=\"" FILE_FS "\""));
    TEST_ASSERT_TRUE(replied(">ok"));

    console_end();
}

/* A page that says there is more and brought nothing would be asked again for
 * ever; the listing ends as an error, as the installed apps' does. */
TEST_CASE("app_list files=on ends as an error on a page of files that brings nothing",
          "[console_apps][app_list_files]")
{
    files_listing_begun();
    storage_queue_drained();

    page_ends(STORAGE_LIST_END_SIG, true);

    TEST_ASSERT_TRUE(replied(">err code=5 msg=app_list"));
    TEST_ASSERT_FALSE(replied(">ok"));
    TEST_ASSERT_EQUAL_UINT(0U, take_storage(STORAGE_LIST_PAGE_SIG, NULL, NULL, 0U));

    console_end();
}

/* `files` is a bool: any other word is refused before anybody is asked. */
TEST_CASE("app_list files with a value that is not on or off is refused",
          "[console_apps][app_list_files]")
{
    console_up();

    ask("app_list files=yes");

    TEST_ASSERT_TRUE(replied(">err code=3 msg=files"));
    TEST_ASSERT_FALSE(replied(">ok"));
    TEST_ASSERT_EQUAL_UINT(0U, take_posted(APP_LIST_REQ_SIG, NULL, 0U, NULL, NULL));

    console_end();
}
