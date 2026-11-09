/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_nfc_field.c
 * @brief `nfc_field`: the 13.56 MHz antenna as a field meter, from the wire.
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
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

/* The command watchdog, in QP ticks - repeated rather than imported for the
 * same reason the other console files repeat it. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)

/** @brief Bring AO_Console up once for the binary, idle, and AO_Nfc's mailbox empty. */
static void console_up(void)
{
    const QEvt *e;

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

    /* AO_Nfc's queue is the mailbox `nfc_field` posts into; emptied here so a
     * request left by an earlier case cannot be mistaken for this one's. */
    qf_support_give_queue(AO_Nfc);
    while ( xQueueReceive(AO_Nfc->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    fake_klog_reset();
    fake_neighbours_reset();
    fake_time_reset();
}

/** @brief Send one line in, exactly as AO_Log publishes it. */
static void type_line(const char *text)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = (uint8_t)KODE_CONSOLE_SRC_SERIAL;
    (void)snprintf(e.line, sizeof(e.line), "%s", text);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

/** @brief How many replies so far contain @p needle. */
static unsigned replies_with(const char *needle)
{
    unsigned n = 0U;

    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            n += 1U;
        }
    }
    return n;
}

/** @brief Whether any reply so far contains @p needle. */
static bool replied(const char *needle)
{
    return replies_with(needle) > 0U;
}

/**
 * @brief Take the listen request the console posted to AO_Nfc, if any.
 * @param[out] out A copy of it.
 * @return false when nothing was posted.
 */
static bool posted_field(NfcFieldEvt *out)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Nfc->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(NFC_FIELD_REQ_SIG, e->sig);
    *out = *(const NfcFieldEvt *)e;
    QF_gc(e);
    return true;
}

/** @brief Deliver AO_Nfc's answer, the way listen_finish() posts it. */
static void answer_field(kode_status_t st, uint8_t min, uint8_t mean,
                         uint8_t peak, uint8_t mod, bool ext, uint16_t n)
{
    NfcFieldRspEvt a;

    (void)memset(&a, 0, sizeof(a));
    a.super = (QEvt)QEVT_INITIALIZER(NFC_FIELD_RSP_SIG);
    a.st    = (uint32_t)st;
    a.min   = min;
    a.mean  = mean;
    a.peak  = peak;
    a.mod   = mod;
    a.ext   = ext;
    a.n     = n;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
}

/* ---- A window answered ---------------------------------------------------- */

TEST_CASE("nfc_field asks AO_Nfc by event and prints the window once answered",
          "[console_nfc_field]")
{
    NfcFieldEvt q;

    console_up();
    type_line("nfc_field ms=50");

    TEST_ASSERT_TRUE_MESSAGE(posted_field(&q), "nothing was asked of AO_Nfc");
    TEST_ASSERT_EQUAL_UINT32(50U, q.ms);
    TEST_ASSERT_EQUAL_PTR_MESSAGE(AO_Console, q.to,
                                  "the answer must come back here");
    /* An Active Object is answered by value: it lends AO_Nfc nothing. */
    TEST_ASSERT_NULL(q.out);
    TEST_ASSERT_NULL(q.rc);
    TEST_ASSERT_NULL(q.done);
    TEST_ASSERT_NULL(q.ack);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before AO_Nfc answered");

    answer_field(KODE_OK, 3U, 9U, 40U, 37U, true, 80U);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, replies_with(">|nfc "),
                                   "not exactly one record");
    TEST_ASSERT_TRUE(replied(">|nfc min=3 mean=9 peak=40 mod=37 ext=1 n=80"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("nfc_field prints a quiet window as a record, not as a failure",
          "[console_nfc_field]")
{
    NfcFieldEvt q;

    console_up();
    type_line("nfc_field ms=200");
    TEST_ASSERT_TRUE(posted_field(&q));
    TEST_ASSERT_EQUAL_UINT32(200U, q.ms);

    answer_field(KODE_OK, 1U, 1U, 2U, 1U, false, 320U);

    TEST_ASSERT_TRUE(replied(">|nfc min=1 mean=1 peak=2 mod=1 ext=0 n=320"));
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_FALSE(replied(">err"));
}

/* ---- Refusals ------------------------------------------------------------- */

TEST_CASE("nfc_field answered busy says busy, with no record",
          "[console_nfc_field]")
{
    NfcFieldEvt q;

    console_up();
    type_line("nfc_field ms=50");
    TEST_ASSERT_TRUE(posted_field(&q));

    answer_field(KODE_DENIED, 0U, 0U, 0U, 0U, false, 0U);

    TEST_ASSERT_TRUE(replied(">err code=4 msg=busy"));
    TEST_ASSERT_FALSE_MESSAGE(replied(">|nfc "), "a refusal printed a window");
    TEST_ASSERT_FALSE(replied(">ok"));

    /* And the console is free again: a second window is asked for. */
    fake_klog_reset();
    type_line("nfc_field ms=10");
    TEST_ASSERT_TRUE_MESSAGE(posted_field(&q), "the console stayed busy");
    answer_field(KODE_OK, 0U, 0U, 0U, 0U, false, 16U);
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("nfc_field says absent when no reader is fitted, never busy",
          "[console_nfc_field]")
{
    NfcFieldEvt q;

    console_up();
    type_line("nfc_field ms=50");
    TEST_ASSERT_TRUE(posted_field(&q));

    answer_field(KODE_IO_ERROR, 0U, 0U, 0U, 0U, false, 0U);

    TEST_ASSERT_TRUE(replied(">err code=5 msg=absent"));
    TEST_ASSERT_FALSE_MESSAGE(replied("msg=busy"), "a missing reader said busy");
    TEST_ASSERT_FALSE_MESSAGE(replied("msg=iofail"),
                              "a missing reader was named as a failing medium");
    TEST_ASSERT_FALSE(replied(">|nfc "));
    TEST_ASSERT_FALSE(replied(">ok"));
}

TEST_CASE("nfc_field names a failed window by its status",
          "[console_nfc_field]")
{
    NfcFieldEvt q;

    console_up();
    type_line("nfc_field ms=50");
    TEST_ASSERT_TRUE(posted_field(&q));

    answer_field(KODE_ERROR, 0U, 0U, 0U, 0U, false, 0U);

    TEST_ASSERT_TRUE(replied(">err code=5 msg=error"));
    TEST_ASSERT_FALSE(replied(">|nfc "));
    TEST_ASSERT_FALSE(replied(">ok"));
}

TEST_CASE("nfc_field refuses a window out of bounds before asking AO_Nfc",
          "[console_nfc_field]")
{
    NfcFieldEvt q;

    console_up();

    type_line("nfc_field ms=0");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=ms"));
    TEST_ASSERT_FALSE(posted_field(&q));

    fake_klog_reset();
    type_line("nfc_field ms=201");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=ms"));
    TEST_ASSERT_FALSE(posted_field(&q));

    /* The window is the whole point of the verb, so it has no default. */
    fake_klog_reset();
    type_line("nfc_field");
    TEST_ASSERT_TRUE(replied(">err code=2 msg=ms"));
    TEST_ASSERT_FALSE(posted_field(&q));
}

/* ---- Whose answer it is --------------------------------------------------- */

TEST_CASE("only a field answer closes nfc_field, and only while it waits",
          "[console_nfc_field]")
{
    NfcFieldEvt q;
    NfcInfoEvt  stray;

    console_up();

    /* An answer nobody is waiting for prints nothing. */
    answer_field(KODE_OK, 5U, 5U, 5U, 0U, false, 16U);
    TEST_ASSERT_FALSE(replied(">|nfc"));
    TEST_ASSERT_FALSE(replied(">ok"));

    type_line("nfc_field ms=50");
    TEST_ASSERT_TRUE(posted_field(&q));

    (void)memset(&stray, 0, sizeof(stray));
    stray.super = (QEvt)QEVT_INITIALIZER(NFC_INFO_RSP_SIG);
    QASM_DISPATCH(AO_Console, &stray.super, 0U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "the status answer closed it");
    TEST_ASSERT_FALSE(replied(">|nfc=chip"));

    answer_field(KODE_OK, 2U, 4U, 6U, 4U, false, 80U);
    TEST_ASSERT_TRUE(replied(">|nfc min=2 mean=4 peak=6 mod=4 ext=0 n=80"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("nfc_field names each status by its own word, and an unknown one "
          "as error", "[console_nfc_field]")
{
    NfcFieldEvt    q;
    NfcFieldRspEvt a;

    console_up();
    type_line("nfc_field ms=50");
    TEST_ASSERT_TRUE(posted_field(&q));
    answer_field(KODE_TIMEOUT, 0U, 0U, 0U, 0U, false, 0U);
    TEST_ASSERT_TRUE(replied(">err code=5 msg=timeout"));

    /* A value no kode_status_t has is still one word on the wire. */
    fake_klog_reset();
    type_line("nfc_field ms=50");
    TEST_ASSERT_TRUE(posted_field(&q));
    (void)memset(&a, 0, sizeof(a));
    a.super = (QEvt)QEVT_INITIALIZER(NFC_FIELD_RSP_SIG);
    a.st    = 99U;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
    TEST_ASSERT_TRUE(replied(">err code=5 msg=error"));
    TEST_ASSERT_FALSE(replied(">ok"));
}
