/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_nfc_emulate.c
 * @brief `nfc_emulate`: the card the Dot presents, typed in the console's grammar.
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

    /* AO_Nfc's queue is the mailbox `nfc_emulate` posts into; emptied here so
     * a request left by an earlier case cannot be mistaken for this one's. */
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

/** @brief Whether any reply so far contains @p needle. */
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
 * @brief Take the emulation request the console posted to AO_Nfc, if any.
 * @param[out] out A copy of it. Its @c card still points at the console's
 *                 own card, which outlives the post by design.
 * @return false when nothing was posted.
 */
static bool posted_emu(NfcEmuReqEvt *out)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Nfc->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(NFC_EMU_REQ_SIG, e->sig);
    *out = *(const NfcEmuReqEvt *)e;
    QF_gc(e);
    return true;
}

/** @brief Deliver AO_Nfc's answer to a start, the way it posts one. */
static void answer_emu(kode_status_t rc, uint32_t reads)
{
    NfcEmuRspEvt a;

    (void)memset(&a, 0, sizeof(a));
    a.super = (QEvt)QEVT_INITIALIZER(NFC_EMU_RSP_SIG);
    a.rc    = (uint32_t)rc;
    a.reads = reads;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
}

/* ---- The text record ------------------------------------------------------ */

TEST_CASE("nfc_emulate takes a quoted text whole and asks AO_Nfc by event",
          "[console_nfc_emulate]")
{
    NfcEmuReqEvt q;

    console_up();
    type_line("nfc_emulate text=\"hello there\"");

    TEST_ASSERT_TRUE_MESSAGE(posted_emu(&q), "nothing was asked of AO_Nfc");
    TEST_ASSERT_EQUAL_UINT8(1U, q.on);
    TEST_ASSERT_EQUAL_PTR_MESSAGE(AO_Console, q.who,
                                  "the answer must come back here");
    TEST_ASSERT_NOT_NULL(q.card);

    const knfc_emu_card_t * const card = (const knfc_emu_card_t *)q.card;
    TEST_ASSERT_EQUAL_STRING_MESSAGE("hello there", card->ndef,
                                     "the space split the text");
    TEST_ASSERT_FALSE(card->uri);
    TEST_ASSERT_EQUAL_UINT8(0U, card->uid_len);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before AO_Nfc answered");

    answer_emu(KODE_OK, 0U);
    TEST_ASSERT_TRUE(replied(">ok reads=0"));
}

TEST_CASE("nfc_emulate takes an empty quoted text as the default record",
          "[console_nfc_emulate]")
{
    NfcEmuReqEvt q;

    console_up();
    type_line("nfc_emulate text=\"\"");

    TEST_ASSERT_TRUE_MESSAGE(posted_emu(&q),
                             "an empty text was taken for a missing key");
    const knfc_emu_card_t * const card = (const knfc_emu_card_t *)q.card;
    TEST_ASSERT_NOT_NULL(card);
    TEST_ASSERT_EQUAL_STRING("", card->ndef);
    TEST_ASSERT_FALSE(card->uri);

    answer_emu(KODE_OK, 0U);
    TEST_ASSERT_TRUE(replied(">ok reads=0"));
}

/* ---- The identity --------------------------------------------------------- */

TEST_CASE("nfc_emulate reads a quoted uid, atqa and sak as hex",
          "[console_nfc_emulate]")
{
    NfcEmuReqEvt  q;
    const uint8_t uid[4] = { 0x04U, 0xA1U, 0xB2U, 0xC3U };

    console_up();
    type_line("nfc_emulate uid=\"04:A1:B2:C3\" atqa=\"0004\" sak=\"08\"");

    TEST_ASSERT_TRUE(posted_emu(&q));
    const knfc_emu_card_t * const card = (const knfc_emu_card_t *)q.card;
    TEST_ASSERT_NOT_NULL(card);
    TEST_ASSERT_EQUAL_UINT8(4U, card->uid_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(uid, card->uid, 4U);
    TEST_ASSERT_EQUAL_HEX16(0x0004U, card->atqa);
    TEST_ASSERT_EQUAL_HEX8(0x08U, card->sak);
    TEST_ASSERT_EQUAL_STRING("", card->ndef);

    answer_emu(KODE_OK, 0U);
    TEST_ASSERT_TRUE(replied(">ok reads=0"));
}

/* ---- The other form ------------------------------------------------------- */

TEST_CASE("nfc_emulate refuses a string typed bare, before asking AO_Nfc",
          "[console_nfc_emulate]")
{
    NfcEmuReqEvt q;

    console_up();

    type_line("nfc_emulate text=hello");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=text"));
    TEST_ASSERT_FALSE_MESSAGE(posted_emu(&q), "a bare text reached AO_Nfc");

    fake_klog_reset();
    type_line("nfc_emulate uid=04A1B2C3");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=uid"));
    TEST_ASSERT_FALSE(posted_emu(&q));

    fake_klog_reset();
    type_line("nfc_emulate uri=https://kode.diy");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=uri"));
    TEST_ASSERT_FALSE(posted_emu(&q));

    /* Nothing was in flight, so the console is free for the quoted form. */
    fake_klog_reset();
    type_line("nfc_emulate uri=\"https://kode.diy\"");
    TEST_ASSERT_TRUE_MESSAGE(posted_emu(&q), "the console stayed busy");
    const knfc_emu_card_t * const card = (const knfc_emu_card_t *)q.card;
    TEST_ASSERT_NOT_NULL(card);
    TEST_ASSERT_TRUE(card->uri);
    TEST_ASSERT_EQUAL_STRING("https://kode.diy", card->ndef);

    answer_emu(KODE_OK, 0U);
    TEST_ASSERT_TRUE(replied(">ok reads=0"));
}
