/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_flash.c
 * @brief The app-facing flash verb, and what it decides before anyone acts.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "qpc.h"

#include "fakes.h"
#include "kaos.h"
#include "kdrv_kfw.h"
#include "kode_sys.h"
#include "kqp.h"
#include "qf_support.h"

/**
 * @brief Give AO_AppMgr somewhere to receive, without constructing it.
 *
 * kode_sys_flash() posts a real FlashReqEvt to the real AO, so what a case reads
 * is the event the manager would have acted on - not a recording made by a
 * stand-in. The AO itself is never dispatched here; only its queue is needed,
 * and the case owns what lands in it.
 */
static void flash_fresh(void)
{
    qf_support_init();
    qf_support_give_queue(AO_AppMgr);

    /* Whatever a previous case left. */
    for ( ;; )
    {
        const QEvt *e = NULL;
        if ( xQueueReceive(AO_AppMgr->eQueue, &e, 0U) != pdTRUE )
        {
            break;
        }
        QF_gc(e);
    }
}

/** @brief How many requests are waiting for AO_AppMgr. */
static unsigned flash_count(void)
{
    return (unsigned)uxQueueMessagesWaiting(AO_AppMgr->eQueue);
}

/** @brief Take the next request; fails the case when there is none. */
static const FlashReqEvt *flash_take(void)
{
    static const QEvt *s_held;
    const QEvt        *e = NULL;

    if ( NULL != s_held )
    {
        QF_gc(s_held);
        s_held = NULL;
    }
    TEST_ASSERT_EQUAL_INT(pdTRUE, xQueueReceive(AO_AppMgr->eQueue, &e, 0U));
    TEST_ASSERT_EQUAL_INT(FLASH_REQ_SIG, (int)e->sig);
    s_held = e;
    return (const FlashReqEvt *)e;
}

/* ---- The guard that keeps this file honest -------------------------------- */

/* Everything below lives inside `#if CONFIG_KODEOS_APPMGR`, and kaos and kdrv
 * reach this binary as loose sources rather than as components carrying their
 * Kconfig - so unless the flag is spelled out in the build, the whole verb
 * compiles down to a refusal and every case here would pass while asserting
 * about code that is not present. The count check cannot see that: the cases
 * are all there and all green. This is the only thing that can. */
TEST_CASE("the build really has the app manager compiled in", "[kode_sys_flash]")
{
#if !CONFIG_KODEOS_APPMGR
    TEST_FAIL_MESSAGE("CONFIG_KODEOS_APPMGR is not set: every case in this "
                      "file would be asserting about code that compiled out");
#endif
    flash_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_flash("probe.bin", "app", false));
    TEST_ASSERT_EQUAL_UINT(1U, flash_count());
}

/* ---- Where to look -------------------------------------------------------- */

TEST_CASE("a bare name searches the media in the usual order", "[kode_sys_flash]")
{
    flash_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_flash("blink.bin", "app", false));
    TEST_ASSERT_EQUAL_UINT(1U, flash_count());

    const FlashReqEvt * const req = flash_take();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KFW_SRC_FS, req->src_kind);
    TEST_ASSERT_EQUAL_STRING("blink.bin", req->src);
}

/* The prefix is stripped before the name travels on: AO_AppMgr is handed a
 * file name, not a spec to parse a second time. Two parsers for one string is
 * how they start to disagree. */
TEST_CASE("an explicit medium is honoured and its prefix removed",
          "[kode_sys_flash]")
{
    flash_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_flash("sd:blink.bin", "app", false));
    const FlashReqEvt * const sd = flash_take();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KFW_SRC_SD, sd->src_kind);
    TEST_ASSERT_EQUAL_STRING("blink.bin", sd->src);

    flash_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_flash("fs:kodeos.bin", "os", false));
    const FlashReqEvt * const fs = flash_take();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KFW_SRC_FS, fs->src_kind);
    TEST_ASSERT_EQUAL_STRING("kodeos.bin", fs->src);
}

/* A URL is a download, and it travels whole, scheme included: AO_AppMgr hands
 * it to AO_Net as it came. */
TEST_CASE("an http or https URL is a download", "[kode_sys_flash]")
{
    static const char URL[] = "https://example.invalid/app.bin?token=abc";

    flash_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_flash(URL, "app", true));
    const FlashReqEvt * const s = flash_take();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KFW_SRC_HTTP, s->src_kind);
    TEST_ASSERT_EQUAL_STRING(URL, s->src);
    TEST_ASSERT_TRUE(s->launch);

    flash_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_sys_flash("http://example.invalid/a.bin", "app", false));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KFW_SRC_HTTP, flash_take()->src_kind);
}

/* 256 characters fit; one more would be cut, and a cut URL is another URL. */
TEST_CASE("a source longer than 256 characters is refused, not cut",
          "[kode_sys_flash]")
{
    static char       url[KODE_FLASH_SRC_MAX + 1U];
    static const char HEAD[] = "https://example.invalid/";

    (void)snprintf(url, sizeof(url), "%s", HEAD);
    (void)memset(&url[sizeof(HEAD) - 1U], 'x', sizeof(url) - sizeof(HEAD));
    url[sizeof(url) - 1U] = '\0';

    flash_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_flash(url, "app", false));
    TEST_ASSERT_EQUAL_UINT(0U, flash_count());

    url[sizeof(url) - 2U] = '\0';   /* 256 characters */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_flash(url, "app", false));
    TEST_ASSERT_EQUAL_STRING(url, flash_take()->src);
}

/* Only those prefixes exist. Anything else is part of the name, so a file
 * genuinely called "http:thing.bin" is looked for under that name rather than
 * being half-understood as a URL. */
TEST_CASE("an unrecognised prefix is part of the name, not a medium",
          "[kode_sys_flash]")
{
    flash_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_flash("http:thing.bin", "app", false));
    const FlashReqEvt * const url = flash_take();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KFW_SRC_FS, url->src_kind);
    TEST_ASSERT_EQUAL_STRING("http:thing.bin", url->src);

    /* And the match is exact: a near miss is a name too. */
    flash_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_flash("SD:x.bin", "app", false));
    TEST_ASSERT_EQUAL_STRING("SD:x.bin", flash_take()->src);
}

/* A prefix with nothing behind it names no file. Refusing here means the
 * refusal reaches whoever typed it, rather than becoming a failed open
 * several seconds later. */
TEST_CASE("a prefix with no name behind it is refused", "[kode_sys_flash]")
{
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_flash("sd:", "app", false));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_flash("fs:", "app", false));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_flash("", "app", false));

    TEST_ASSERT_EQUAL_UINT(0U, flash_count());
}

/* ---- Where it goes -------------------------------------------------------- */

TEST_CASE("the two destinations are the OS slot and the apps region",
          "[kode_sys_flash]")
{
    flash_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_flash("x.bin", "os", false));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KFW_TARGET_OS,
                            flash_take()->dst_kind);

    flash_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_flash("x.bin", "app", false));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KFW_TARGET_APP,
                            flash_take()->dst_kind);
}

/* The coprocessor is AO_Radio's, and this door does not open onto it. Naming
 * it has to be a refusal rather than a fall-through to "app": writing an app
 * image over the C5's firmware is not a mistake anyone recovers from over the
 * console. */
TEST_CASE("the coprocessor is not a destination this door offers",
          "[kode_sys_flash]")
{
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_flash("fw.bin", "c5", false));
    TEST_ASSERT_EQUAL_UINT(0U, flash_count());
}

TEST_CASE("an unknown destination is refused, never guessed", "[kode_sys_flash]")
{
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_flash("x.bin", "", false));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_flash("x.bin", "apps", false));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_flash("x.bin", "OS", false));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_flash("x.bin", "slot", false));

    TEST_ASSERT_EQUAL_UINT(0U, flash_count());
}

/* ---- Whether it runs afterwards ------------------------------------------- */

/* The flag survives the parse untouched. It decides whether the board resets
 * into what was just written, so a parse that dropped it would install
 * silently where the operator asked to launch - or, worse, the other way. */
TEST_CASE("the run flag is passed through, both ways", "[kode_sys_flash]")
{
    flash_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_flash("x.bin", "app", true));
    TEST_ASSERT_TRUE(flash_take()->launch);

    flash_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_flash("x.bin", "app", false));
    TEST_ASSERT_FALSE(flash_take()->launch);
}

/* ---- Nothing at all ------------------------------------------------------- */

TEST_CASE("a null argument is refused rather than dereferenced",
          "[kode_sys_flash]")
{
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_flash(NULL, "app", false));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_flash("x.bin", NULL, false));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_flash(NULL, NULL, true));

    TEST_ASSERT_EQUAL_UINT(0U, flash_count());
}

/* Every refusal happens before anyone is asked to do anything. That is the
 * property the whole file is really about: a bad request costs nothing, and
 * the operator hears about it while there is still a console listening. */
TEST_CASE("no refusal ever reaches the app manager", "[kode_sys_flash]")
{
    (void)kode_sys_flash(NULL, "app", false);
    (void)kode_sys_flash("x.bin", "c5", false);
    (void)kode_sys_flash("sd:", "app", false);
    (void)kode_sys_flash("x.bin", "nowhere", true);

    TEST_ASSERT_EQUAL_UINT(0U, flash_count());
}
