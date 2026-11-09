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

/* ---- app_erase --------------------------------------------------------------- */

/* The erase itself is AO_AppMgr's, on its own thread; the console's whole job
 * is to hand it the name exactly as typed, once, and say ok. */
TEST_CASE("app_erase posts the erase request with the name it was typed with",
          "[console_apps]")
{
    char name[KODE_APP_FILE_MAX];

    console_up();

    ask("app_erase name=\"scope\"");

    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_EQUAL_UINT(1U, take_posted(APP_ERASE_REQ_SIG, name, sizeof(name),
                                           NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("scope", name);

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
