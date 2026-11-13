/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_apps.c
 * @brief `nvs_del`, `app_erase` and `app_list`: the lines that remove things.
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
