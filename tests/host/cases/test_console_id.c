/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_id.c
 * @brief `id_sign`, `id_cert` and the factory's `ident_*` verbs: the unit's identity.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "autotest_support.h"
#include "fakes.h"
#include "kaos.h"
#include "kcli_proto.h"
#include "kode_wire.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

#define ANY_BUDGET_TICKS   ((200000U * 100U) / 1000U)

static void console_up(void)
{
    const QEvt *e;

    qf_support_init();
    autotest_up();
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
    (void)qf_support_tick(AO_Console, ANY_BUDGET_TICKS);

    qf_support_give_queue(AO_Radio);
    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    fake_klog_reset();
    fake_neighbours_reset();
}

static void type_from(const char *text, uint8_t src)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = src;
    (void)snprintf(e.line, sizeof(e.line), "%s", text);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
    autotest_typed(text);
}

static void type_line(const char *text)
{
    type_from(text, (uint8_t)KODE_CONSOLE_SRC_SERIAL);
}

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

/** @brief Whatever the console posted to AO_Radio, or a zero signal. */
static enum_t posted(void *out, size_t size)
{
    const QEvt *e = NULL;
    enum_t sig = 0;

    if ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        sig = e->sig;
        (void)memcpy(out, e, size);
        QF_gc(e);
    }
    return sig;
}

static bool nothing_posted(void)
{
    IdReqEvt q;
    return posted(&q, sizeof(q)) == 0;
}

static void answer_end(uint8_t st, uint16_t len, uint16_t unit_len)
{
    IdEndEvt a;

    (void)memset(&a, 0, sizeof(a));
    a.super    = (QEvt)QEVT_INITIALIZER(ID_END_SIG);
    a.status   = st;
    a.len      = len;
    a.unit_len = unit_len;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
}

static void answer_part(uint8_t n, uint8_t of, const uint8_t *der, uint8_t len)
{
    IdPartEvt a;

    (void)memset(&a, 0, sizeof(a));
    a.super = (QEvt)QEVT_INITIALIZER(ID_PART_SIG);
    a.n     = n;
    a.of    = of;
    a.len   = len;
    (void)memcpy(a.der, der, len);
    QASM_DISPATCH(AO_Console, &a.super, 0U);
}

/* ---- id_sign -------------------------------------------------------------- */

TEST_CASE("id_sign refuses a nonce outside the alphabet and asks nobody",
          "[console_id]")
{
    static const char *const bad[] = {
        "id_sign nonce=\"a b\"",
        "id_sign nonce=\"a/b\"",
        "id_sign nonce=abc",
    };
    for ( unsigned i = 0U; (sizeof(bad) / sizeof(bad[0])) > i; i++ )
    {
        console_up();
        type_line(bad[i]);
        TEST_ASSERT_TRUE_MESSAGE(replied(">err code=3 msg=nonce"), bad[i]);
        TEST_ASSERT_TRUE_MESSAGE(nothing_posted(), bad[i]);
    }
}

TEST_CASE("id_sign refuses a nonce of 201 characters", "[console_id]")
{
    char line[240];
    char n201[202];

    (void)memset(n201, 'a', 201U);
    n201[201] = '\0';
    (void)snprintf(line, sizeof(line), "id_sign nonce=\"%s\"", n201);
    console_up();
    type_line(line);
    TEST_ASSERT_TRUE(replied(">err code=3 msg=nonce"));
    TEST_ASSERT_TRUE(nothing_posted());
}

TEST_CASE("id_sign posts the nonce and prints the signature as base64",
          "[console_id]")
{
    IdSignReqEvt q;

    console_up();
    type_line("id_sign nonce=\"test.nonce-1\"");
    TEST_ASSERT_EQUAL_INT((int)ID_SIGN_REQ_SIG, (int)posted(&q, sizeof(q)));
    TEST_ASSERT_EQUAL_PTR(AO_Console, q.who);
    TEST_ASSERT_EQUAL_STRING("test.nonce-1", q.nonce);
    TEST_ASSERT_FALSE(replied(">ok"));

    IdSignRspEvt a;
    (void)memset(&a, 0, sizeof(a));
    a.super   = (QEvt)QEVT_INITIALIZER(ID_SIGN_RSP_SIG);
    a.status  = KODE_ST_OK;
    a.der_len = 3U;
    a.der[0]  = 0x30U;
    a.der[1]  = 0x01U;
    a.der[2]  = 0x02U;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
    TEST_ASSERT_TRUE(replied(">|sig der=\"MAEC\""));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("id_sign on a unit with no key says there is no identity",
          "[console_id]")
{
    IdSignReqEvt q;

    console_up();
    type_line("id_sign nonce=\"abc\"");
    TEST_ASSERT_EQUAL_INT((int)ID_SIGN_REQ_SIG, (int)posted(&q, sizeof(q)));
    IdSignRspEvt a;
    (void)memset(&a, 0, sizeof(a));
    a.super  = (QEvt)QEVT_INITIALIZER(ID_SIGN_RSP_SIG);
    a.status = KODE_ST_NOT_FOUND;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
    TEST_ASSERT_TRUE(replied(">err code=5 msg=identity"));
}

/* ---- id_cert and ident_csr ------------------------------------------------- */

TEST_CASE("id_cert prints the serial, the parts in order, then ok", "[console_id]")
{
    IdReqEvt q;
    uint8_t const a[3] = { 0x30U, 0x01U, 0x02U };
    uint8_t const b[3] = { 0x03U, 0x04U, 0x05U };

    console_up();
    type_line("id_cert");
    TEST_ASSERT_EQUAL_INT((int)ID_REQ_SIG, (int)posted(&q, sizeof(q)));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)ID_OP_CERT, q.op);
    TEST_ASSERT_TRUE(replied(">|id serial="));
    answer_part(1U, 2U, a, 3U);
    answer_part(2U, 2U, b, 3U);
    answer_end(KODE_ST_OK, 6U, 3U);
    TEST_ASSERT_TRUE(replied(">|id part=1 of=2 der=\"MAEC\""));
    TEST_ASSERT_TRUE(replied(">|id part=2 of=2 der=\"AwQF\""));
    TEST_ASSERT_TRUE(replied(">|id unit_len=3 len=6"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("id_cert on a unit with no certificate says there is no identity",
          "[console_id]")
{
    IdReqEvt q;

    console_up();
    type_line("id_cert");
    TEST_ASSERT_EQUAL_INT((int)ID_REQ_SIG, (int)posted(&q, sizeof(q)));
    answer_end(KODE_ST_NOT_FOUND, 0U, 0U);
    TEST_ASSERT_TRUE(replied(">err code=5 msg=identity"));
}

TEST_CASE("ident_csr prints the CSR in parts", "[console_id]")
{
    IdReqEvt q;
    uint8_t const a[3] = { 0x30U, 0x01U, 0x02U };

    console_up();
    type_line("ident_csr");
    TEST_ASSERT_EQUAL_INT((int)ID_REQ_SIG, (int)posted(&q, sizeof(q)));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)ID_OP_CSR, q.op);
    answer_part(1U, 1U, a, 3U);
    answer_end(KODE_ST_OK, 3U, 0U);
    TEST_ASSERT_TRUE(replied(">|csr part=1 of=1 der=\"MAEC\""));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("the factory verbs are refused over BLE", "[console_id]")
{
    console_up();
    type_from("ident_csr", (uint8_t)KODE_CONSOLE_SRC_BLE);
    TEST_ASSERT_TRUE(replied(">err code=5 msg=cable"));
    TEST_ASSERT_TRUE(nothing_posted());
}

/* ---- ident_cert ------------------------------------------------------------ */

TEST_CASE("ident_cert decodes its part and posts it", "[console_id]")
{
    IdReqEvt q;

    console_up();
    type_line("ident_cert part=1 of=2 der=\"MAEC\"");
    TEST_ASSERT_EQUAL_INT((int)ID_REQ_SIG, (int)posted(&q, sizeof(q)));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)ID_OP_CERT_PUT, q.op);
    TEST_ASSERT_EQUAL_UINT8(1U, q.n);
    TEST_ASSERT_EQUAL_UINT8(2U, q.of);
    TEST_ASSERT_EQUAL_UINT8(3U, q.len);
    TEST_ASSERT_EQUAL_HEX8(0x30U, q.der[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02U, q.der[2]);
    answer_end(KODE_ST_OK, 0U, 0U);
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("ident_cert refuses a part that is not base64", "[console_id]")
{
    console_up();
    type_line("ident_cert part=1 of=2 der=\"M@EC\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=der"));
    TEST_ASSERT_TRUE(nothing_posted());
}

TEST_CASE("ident_cert out of order is answered as a bad part", "[console_id]")
{
    IdReqEvt q;

    console_up();
    type_line("ident_cert part=2 of=3 der=\"MAEC\"");
    TEST_ASSERT_EQUAL_INT((int)ID_REQ_SIG, (int)posted(&q, sizeof(q)));
    answer_end(KODE_ST_BAD_REQUEST, 0U, 0U);
    TEST_ASSERT_TRUE(replied(">err code=3 msg=part"));
}

/* ---- ident_lock ------------------------------------------------------------ */

TEST_CASE("ident_lock without confirm=on locks nothing", "[console_id]")
{
    console_up();
    type_line("ident_lock");
    TEST_ASSERT_TRUE(replied(">err code=2 msg=confirm"));
    TEST_ASSERT_TRUE(nothing_posted());

    console_up();
    type_line("ident_lock confirm=off");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=confirm"));
    TEST_ASSERT_TRUE(nothing_posted());
}

TEST_CASE("ident_lock confirm=on asks for the lock", "[console_id]")
{
    IdReqEvt q;

    console_up();
    type_line("ident_lock confirm=on");
    TEST_ASSERT_EQUAL_INT((int)ID_REQ_SIG, (int)posted(&q, sizeof(q)));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)ID_OP_LOCK, q.op);
    answer_end(KODE_ST_OK, 0U, 0U);
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* ---- c5_wipe --------------------------------------------------------------- */

TEST_CASE("c5_wipe without confirm=on wipes nothing", "[console_id]")
{
    console_up();
    type_line("c5_wipe");
    TEST_ASSERT_TRUE(replied(">err code=2 msg=confirm"));
    TEST_ASSERT_TRUE(nothing_posted());
}

TEST_CASE("c5_wipe is refused over BLE", "[console_id]")
{
    console_up();
    type_from("c5_wipe confirm=on", (uint8_t)KODE_CONSOLE_SRC_BLE);
    TEST_ASSERT_TRUE(replied(">err code=5 msg=cable"));
    TEST_ASSERT_TRUE(nothing_posted());
}

TEST_CASE("c5_wipe confirm=on asks AO_Radio and names a failed step", "[console_id]")
{
    C5WipeReqEvt q;

    console_up();
    type_line("c5_wipe confirm=on");
    TEST_ASSERT_EQUAL_INT((int)C5_WIPE_REQ_SIG, (int)posted(&q, sizeof(q)));
    TEST_ASSERT_EQUAL_PTR(AO_Console, q.who);

    C5WipeRspEvt a;
    (void)memset(&a, 0, sizeof(a));
    a.super  = (QEvt)QEVT_INITIALIZER(C5_WIPE_RSP_SIG);
    a.status = KODE_ST_STORAGE;
    a.step   = (uint8_t)C5_WIPE_BTKEYS;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
    TEST_ASSERT_TRUE(replied(">err code=5 msg=btkeys"));
}

TEST_CASE("c5_wipe closes with ok when every step did", "[console_id]")
{
    C5WipeReqEvt q;

    console_up();
    type_line("c5_wipe confirm=on");
    TEST_ASSERT_EQUAL_INT((int)C5_WIPE_REQ_SIG, (int)posted(&q, sizeof(q)));
    C5WipeRspEvt a;
    (void)memset(&a, 0, sizeof(a));
    a.super  = (QEvt)QEVT_INITIALIZER(C5_WIPE_RSP_SIG);
    a.status = KODE_ST_OK;
    a.step   = (uint8_t)C5_WIPE_DONE;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
    TEST_ASSERT_TRUE(replied(">ok"));
}
