/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_nfc_ao.c
 * @brief AO_Nfc's listen window: its bursts, who is answered, and what it refuses or survives.
 */
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "qpc.h"
#include "Mockkdrv_st25r3916.h"

#include "ao_spy.h"
#include "ask_support.h"
#include "fakes.h"
#include "kaos.h"
#include "khal_sync.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_cert.h"
#include "knfc_field.h"
#include "kqp.h"
#include "lowpower_cases.h"
#include "qf_support.h"

#ifndef CONFIG_KODEOS_NFC_CERT
#error "main/CMakeLists.txt must define CONFIG_KODEOS_NFC_CERT=1 for this file"
#endif

/* 50 ms at the 10 ms tick. */
#define WIN_MS     50U
#define WIN_TICKS  5U

/** @brief What s_rc holds until AO_Nfc writes it: no kode_status_t. */
#define RC_UNWRITTEN  0xA5U

static knfc_field_t s_out;
static uint32_t     s_rc;
static uint32_t     s_ack;
static bool         s_fitted = true; /**< How AO_Nfc last came up.        */

/**
 * @brief Run AO_Nfc's initial transition again, with the reader answering or
 *        not.
 *
 * Whether a reader is fitted is decided once, in that transition, so a case
 * about an absent one has to run it again. kaos_claim() accepts the same
 * owner twice, and QP keeps the registration and the queue across the
 * constructor. One tick goes first, and it is not tidiness: QP unlinks a
 * spent one-shot only on the next tick, and QTimeEvt_ctorX() on a timer that
 * is still linked cuts QF's list there (test_boot_ao.c pays the same).
 *
 * @param fitted Whether kdrv_st25r3916_init() finds the reader.
 */
static void nfc_restart(bool fitted)
{
    const QEvt *stray;

    (void)qf_support_pump(AO_Nfc, 0U);
    while ( xQueueReceive(AO_Nfc->eQueue, &stray, 0U) == pdTRUE )
    {
        QF_gc(stray);
    }
    if ( fitted )
    {
        kdrv_st25r3916_init_ExpectAndReturn(KODE_OK);
        kdrv_st25r3916_identity_ExpectAnyArgsAndReturn(KODE_OK);
    }
    else
    {
        kdrv_st25r3916_init_ExpectAndReturn(KODE_ERROR);
    }
    Nfc_ctor();
    QASM_INIT(AO_Nfc, (void *)0, 0U);
    Mockkdrv_st25r3916_Verify();
    s_fitted = fitted;
}

/**
 * @brief Bring AO_Nfc up once for the binary, with a reader present.
 *
 * A case that left the reader absent is undone here, on the way into the
 * next one, rather than at its own end: a case that fails leaves by a jump
 * and never reaches its end, and every case after it would then run against
 * a board with no reader.
 */
static void nfc_up(void)
{
    Mockkdrv_st25r3916_Init();
    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();

    if ( qf_support_claim_ao(AO_Nfc) )
    {
        kdrv_st25r3916_init_ExpectAndReturn(KODE_OK);
        kdrv_st25r3916_identity_ExpectAnyArgsAndReturn(KODE_OK);
        Nfc_ctor();
        qf_support_give_queue(AO_Nfc);
        AO_Nfc->prio  = KODE_PRIO_NFC;
        AO_Nfc->pthre = KODE_PRIO_NFC;
        QActive_register_(AO_Nfc);
        QASM_INIT(AO_Nfc, (void *)0, 0U);
    }
    {
        const QEvt *stray;

        while ( (NULL != AO_Nfc->eQueue)
               && (xQueueReceive(AO_Nfc->eQueue, &stray, 0U) == pdTRUE) )
        {
            QF_gc(stray);
        }
    }
    if ( !s_fitted )
    {
        nfc_restart(true);
    }
    (void)memset(&s_out, 0xA5, sizeof(s_out));
    s_rc  = RC_UNWRITTEN;
    s_ack = 0U;
    fake_sem_reset();
    ao_spy_drain();
}

/** @brief The next thing the spy was posted, which must be a field answer. */
static const NfcFieldRspEvt *take_rsp(void)
{
    const QEvt *e = ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "nothing was posted back");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)NFC_FIELD_RSP_SIG, (int)e->sig,
                                  "not an NfcFieldRspEvt");
    return (const NfcFieldRspEvt *)e;
}

/** @brief Whether s_out is still the sentinel nfc_up() painted. */
static bool out_untouched(void)
{
    knfc_field_t sentinel;

    (void)memset(&sentinel, 0xA5, sizeof(sentinel));
    return memcmp(&s_out, &sentinel, sizeof(s_out)) == 0;
}

/** @brief Open a window of @p ms and expect its opening burst. */
static void open_window(uint32_t ms, QActive *to)
{
    kdrv_st25r3916_listen_begin_ExpectAndReturn(KODE_OK);
    kdrv_st25r3916_listen_step_ExpectAndReturn(KODE_OK);
    nfc_field_request(&s_out, &s_rc, khal_sem_create(), ms, 7U, &s_ack, to);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_sem_signals(),
                                   "the window answered as it opened");
}

TEST_CASE("a listen window is one burst per tick and answers both channels",
          "[nfc_ao]")
{
    nfc_up();
    knfc_field_t stats = { .min = 3U, .mean = 9U, .peak = 40U, .mod = 37U,
                           .ext = true, .n = 80U };

    open_window(WIN_MS, ao_spy());
    for ( unsigned t = 1U; WIN_TICKS > t; ++t )
    {
        kdrv_st25r3916_listen_step_ExpectAndReturn(KODE_OK);
    }
    TEST_ASSERT_EQUAL_UINT(WIN_TICKS - 1U,
                           qf_support_tick(AO_Nfc, WIN_TICKS - 1U));
    TEST_ASSERT_EQUAL_UINT(0U, fake_sem_signals());

    /* The last tick carries the deadline AND the pump's own re-arm. The
     * deadline must come first, or a sixth burst is an unexpected call. */
    kdrv_st25r3916_listen_end_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_st25r3916_listen_end_ReturnThruPtr_out(&stats);
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_tick(AO_Nfc, 1U));

    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());
    TEST_ASSERT_EQUAL_UINT32(7U, s_ack);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, s_rc);
    TEST_ASSERT_EQUAL_MEMORY(&stats, &s_out, sizeof(stats));

    const NfcFieldRspEvt *r = take_rsp();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, r->st);
    TEST_ASSERT_EQUAL_UINT8(3U, r->min);
    TEST_ASSERT_EQUAL_UINT8(9U, r->mean);
    TEST_ASSERT_EQUAL_UINT8(40U, r->peak);
    TEST_ASSERT_EQUAL_UINT8(37U, r->mod);
    TEST_ASSERT_TRUE(r->ext);
    TEST_ASSERT_EQUAL_UINT16(80U, r->n);

    /* Nothing is left armed behind it. */
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Nfc, 30U));
    Mockkdrv_st25r3916_Verify();
    ao_spy_drain();
}

TEST_CASE("a window shorter than a tick still measures once", "[nfc_ao]")
{
    nfc_up();
    knfc_field_t stats = { .min = 1U, .mean = 1U, .peak = 1U, .mod = 0U,
                           .ext = false, .n = 16U };

    open_window(5U, NULL);
    kdrv_st25r3916_listen_end_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_st25r3916_listen_end_ReturnThruPtr_out(&stats);
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_tick(AO_Nfc, 1U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, s_rc);
    TEST_ASSERT_EQUAL_UINT16(16U, s_out.n);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    Mockkdrv_st25r3916_Verify();
}

TEST_CASE("a window longer than the maximum is capped at it", "[nfc_ao]")
{
    nfc_up();
    const unsigned ticks = KODE_NFC_FIELD_MS_MAX / 10U;

    open_window(5000U, NULL);
    for ( unsigned t = 1U; t < ticks; ++t )
    {
        kdrv_st25r3916_listen_step_ExpectAndReturn(KODE_OK);
    }
    TEST_ASSERT_EQUAL_UINT(ticks - 1U, qf_support_tick(AO_Nfc, ticks - 1U));
    kdrv_st25r3916_listen_end_ExpectAnyArgsAndReturn(KODE_OK);
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_tick(AO_Nfc, 1U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, s_rc);
    Mockkdrv_st25r3916_Verify();
}

TEST_CASE("a zero window is refused at once and touches no driver",
          "[nfc_ao]")
{
    nfc_up();

    nfc_field_request(&s_out, &s_rc, khal_sem_create(), 0U, 3U, &s_ack,
                      ao_spy());
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());
    TEST_ASSERT_EQUAL_UINT32(3U, s_ack);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, s_rc);
    TEST_ASSERT_TRUE(out_untouched());
    const NfcFieldRspEvt *r = take_rsp();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, r->st);
    TEST_ASSERT_EQUAL_UINT16(0U, r->n);
    Mockkdrv_st25r3916_Verify();
    ao_spy_drain();
}

TEST_CASE("a second request while a window is open is refused busy",
          "[nfc_ao]")
{
    nfc_up();
    uint32_t rc2   = RC_UNWRITTEN;
    uint32_t ack2  = 0U;
    uint32_t rc3   = RC_UNWRITTEN;
    uint32_t ack3  = 0U;
    uint8_t  tag[64] = { 0U };

    open_window(WIN_MS, NULL);

    /* The console's shape: every pointer NULL, answered by event. The pump's
     * tick delivers the open window's second burst too. */
    kdrv_st25r3916_listen_step_ExpectAndReturn(KODE_OK);
    nfc_field_request(NULL, &rc2, khal_sem_create(), WIN_MS, 9U, &ack2,
                      ao_spy());
    /* And a tag read, which would raise the field under the window. */
    nfc_read_request(tag, &rc3, khal_sem_create(), 1000U, 11U, &ack3);
    TEST_ASSERT_EQUAL_UINT(3U, qf_support_pump(AO_Nfc, 4U));

    TEST_ASSERT_EQUAL_UINT(2U, fake_sem_signals());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_DENIED, rc2,
        "the blocked channel did not say busy");
    TEST_ASSERT_EQUAL_UINT32(9U, ack2);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_DENIED, rc3,
        "the refused tag read did not say busy");
    TEST_ASSERT_EQUAL_UINT32(11U, ack3);
    const NfcFieldRspEvt *r = take_rsp();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, r->st);
    ao_spy_drain();

    /* The first window runs on to its own deadline, whole. */
    for ( unsigned t = 2U; WIN_TICKS > t; ++t )
    {
        kdrv_st25r3916_listen_step_ExpectAndReturn(KODE_OK);
    }
    TEST_ASSERT_EQUAL_UINT(WIN_TICKS - 2U,
                           qf_support_tick(AO_Nfc, WIN_TICKS - 2U));
    kdrv_st25r3916_listen_end_ExpectAnyArgsAndReturn(KODE_OK);
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_tick(AO_Nfc, 1U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, s_rc);
    TEST_ASSERT_EQUAL_UINT32(7U, s_ack);
    TEST_ASSERT_EQUAL_UINT(3U, fake_sem_signals());
    Mockkdrv_st25r3916_Verify();
}

TEST_CASE("a window the driver will not open is an error, never busy",
          "[nfc_ao]")
{
    nfc_up();

    /* The driver's DENIED here is a transmitter that would not go off: the
     * window failed, and busy would send the requester back for more. */
    kdrv_st25r3916_listen_begin_ExpectAndReturn(KODE_DENIED);
    nfc_field_request(&s_out, &s_rc, khal_sem_create(), WIN_MS, 4U, &s_ack,
                      ao_spy());
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, take_rsp()->st);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, s_rc);
    TEST_ASSERT_EQUAL_UINT32(4U, s_ack);
    ao_spy_drain();

    /* A first burst that fails is an error, and the driver has closed the
     * window itself, so nothing ends it again. */
    kdrv_st25r3916_listen_begin_ExpectAndReturn(KODE_OK);
    kdrv_st25r3916_listen_step_ExpectAndReturn(KODE_ERROR);
    s_rc = RC_UNWRITTEN;
    nfc_field_request(&s_out, &s_rc, khal_sem_create(), WIN_MS, 5U, &s_ack,
                      ao_spy());
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, take_rsp()->st);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, s_rc);
    TEST_ASSERT_TRUE(out_untouched());
    TEST_ASSERT_EQUAL_UINT32(5U, s_ack);
    TEST_ASSERT_EQUAL_UINT(2U, fake_sem_signals());
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Nfc, 30U));

    /* Neither refusal left the reader busy. `nfc` also reads the emulation
     * snapshot, which is mocked here and reports nothing emulating. */
    kdrv_st25r3916_emulate_snapshot_ExpectAnyArgs();
    nfc_info_request(ao_spy());
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    const QEvt *e = ao_spy_take();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT((int)NFC_INFO_RSP_SIG, (int)e->sig);
    TEST_ASSERT_FALSE(((const NfcInfoEvt *)e)->busy);
    TEST_ASSERT_FALSE(((const NfcInfoEvt *)e)->field);
    TEST_ASSERT_FALSE(((const NfcInfoEvt *)e)->emulating);
    Mockkdrv_st25r3916_Verify();
    ao_spy_drain();
}

TEST_CASE("a burst that fails mid-window ends it with an error", "[nfc_ao]")
{
    nfc_up();

    open_window(WIN_MS, ao_spy());
    kdrv_st25r3916_listen_step_ExpectAndReturn(KODE_ERROR);
    kdrv_st25r3916_listen_end_ExpectAnyArgsAndReturn(KODE_DENIED);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_Nfc, 1U));

    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, s_rc);
    TEST_ASSERT_TRUE(out_untouched());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, take_rsp()->st);
    /* The deadline went with it: no late answer, no stray burst. */
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Nfc, 30U));
    Mockkdrv_st25r3916_Verify();
    ao_spy_drain();
}

TEST_CASE("a close that cannot restore the reader is an error, not a reading",
          "[nfc_ao]")
{
    nfc_up();
    knfc_field_t stats = { .min = 2U, .mean = 2U, .peak = 2U, .mod = 0U,
                           .ext = false, .n = 80U };

    open_window(WIN_MS, ao_spy());
    for ( unsigned t = 1U; WIN_TICKS > t; ++t )
    {
        kdrv_st25r3916_listen_step_ExpectAndReturn(KODE_OK);
    }
    TEST_ASSERT_EQUAL_UINT(WIN_TICKS - 1U,
                           qf_support_tick(AO_Nfc, WIN_TICKS - 1U));
    kdrv_st25r3916_listen_end_ExpectAnyArgsAndReturn(KODE_ERROR);
    kdrv_st25r3916_listen_end_ReturnThruPtr_out(&stats);
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_tick(AO_Nfc, 1U));

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, s_rc);
    TEST_ASSERT_TRUE(out_untouched());
    const NfcFieldRspEvt *r = take_rsp();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, r->st);
    TEST_ASSERT_EQUAL_UINT16(0U, r->n);
    Mockkdrv_st25r3916_Verify();
    ao_spy_drain();
}

/**
 * @brief Run a whole window whose close the driver refuses with @p end, and
 *        return what its requester was told, insisting both channels say
 *        the same.
 *
 * @param end What kdrv_st25r3916_listen_end() answers.
 * @return The NfcFieldRspEvt's @c st.
 */
static uint32_t window_closed_with(kode_status_t end)
{
    (void)memset(&s_out, 0xA5, sizeof(s_out));
    s_rc = RC_UNWRITTEN;
    fake_sem_reset();
    open_window(WIN_MS, ao_spy());
    for ( unsigned t = 1U; WIN_TICKS > t; ++t )
    {
        kdrv_st25r3916_listen_step_ExpectAndReturn(KODE_OK);
    }
    TEST_ASSERT_EQUAL_UINT(WIN_TICKS - 1U,
                           qf_support_tick(AO_Nfc, WIN_TICKS - 1U));
    kdrv_st25r3916_listen_end_ExpectAnyArgsAndReturn(end);
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_tick(AO_Nfc, 1U));

    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());
    TEST_ASSERT_TRUE(out_untouched());
    const NfcFieldRspEvt *r = take_rsp();
    TEST_ASSERT_EQUAL_UINT16(0U, r->n);
    uint32_t const st = r->st;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(st, s_rc,
        "the blocked channel and the event disagree");
    ao_spy_drain();
    return st;
}

TEST_CASE("a close the driver refuses is a failed window, never busy or absent",
          "[nfc_ao]")
{
    nfc_up();

    /* The requester reads DENIED as busy and IO_ERROR as absent; a window
     * that ran and could not close cleanly is neither. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_ERROR,
        window_closed_with(KODE_DENIED), "a failed close read as busy");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_ERROR,
        window_closed_with(KODE_IO_ERROR), "a failed close read as absent");
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Nfc, 30U));
    Mockkdrv_st25r3916_Verify();
}

TEST_CASE("the certification carrier is refused busy while a window is open",
          "[nfc_ao]")
{
    nfc_up();
    uint32_t rc   = 0xFFU;
    uint32_t cack = 0U;

    open_window(WIN_MS, NULL);

    /* A stop is refused: dropping the field would stop the window's tick and
     * deadline and leave its requester unanswered. No field_off is expected,
     * and a strict mock fails on one. */
    kdrv_st25r3916_listen_step_ExpectAndReturn(KODE_OK);
    nfc_cert_request(0U, &rc, khal_sem_create(), 21U, &cack);
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_pump(AO_Nfc, 4U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, rc);
    TEST_ASSERT_EQUAL_UINT32(21U, cack);

    /* So is a carrier. */
    rc = 0xFFU;
    kdrv_st25r3916_listen_step_ExpectAndReturn(KODE_OK);
    nfc_cert_request(1U, &rc, khal_sem_create(), 22U, &cack);
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_pump(AO_Nfc, 4U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, rc);
    TEST_ASSERT_EQUAL_UINT32(22U, cack);

    /* The window runs on to its own deadline and answers its requester. */
    for ( unsigned t = 3U; WIN_TICKS > t; ++t )
    {
        kdrv_st25r3916_listen_step_ExpectAndReturn(KODE_OK);
    }
    TEST_ASSERT_EQUAL_UINT(WIN_TICKS - 3U,
                           qf_support_tick(AO_Nfc, WIN_TICKS - 3U));
    kdrv_st25r3916_listen_end_ExpectAnyArgsAndReturn(KODE_OK);
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_tick(AO_Nfc, 1U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, s_rc);
    TEST_ASSERT_EQUAL_UINT32(7U, s_ack);
    TEST_ASSERT_EQUAL_UINT(3U, fake_sem_signals());
    Mockkdrv_st25r3916_Verify();
}

TEST_CASE("an NDEF refusal during a window leaves the window's answer alone",
          "[nfc_ao]")
{
    nfc_up();
    char text[16] = { 0 };

    open_window(WIN_MS, ao_spy());

    /* An Active Object's NDEF read, refused busy. Its refusal goes to its own
     * requester and must not take the window's recipient with it. The pump's
     * tick delivers the window's second burst too. */
    kdrv_st25r3916_listen_step_ExpectAndReturn(KODE_OK);
    nfc_ndef_request(ao_spy(), text, sizeof(text), false, false, 1000U);
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_pump(AO_Nfc, 4U));
    const QEvt *e = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the NDEF refusal was not posted");
    TEST_ASSERT_EQUAL_INT((int)NFC_NDEF_RSP_SIG, (int)e->sig);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED,
                             ((const NfcNdefRspEvt *)e)->rc);

    for ( unsigned t = 2U; WIN_TICKS > t; ++t )
    {
        kdrv_st25r3916_listen_step_ExpectAndReturn(KODE_OK);
    }
    TEST_ASSERT_EQUAL_UINT(WIN_TICKS - 2U,
                           qf_support_tick(AO_Nfc, WIN_TICKS - 2U));
    kdrv_st25r3916_listen_end_ExpectAnyArgsAndReturn(KODE_OK);
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_tick(AO_Nfc, 1U));

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, take_rsp()->st);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, s_rc);
    Mockkdrv_st25r3916_Verify();
    ao_spy_drain();
}

TEST_CASE("an absent reader is refused absent on both channels and never asked",
          "[nfc_ao]")
{
    nfc_up();
    nfc_restart(false);

    /* A strict mock with no expectation: any driver call fails the case. */
    nfc_field_request(&s_out, &s_rc, khal_sem_create(), WIN_MS, 6U, &s_ack,
                      ao_spy());
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));

    const NfcFieldRspEvt *r = take_rsp();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_IO_ERROR, r->st,
                                     "absent must not read as busy");
    TEST_ASSERT_EQUAL_UINT16(0U, r->n);
    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());
    TEST_ASSERT_EQUAL_UINT32(6U, s_ack);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_IO_ERROR, s_rc,
                                     "the blocked channel did not say absent");
    TEST_ASSERT_TRUE(out_untouched());
    /* Nothing was paced behind the refusal. */
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Nfc, 30U));
    Mockkdrv_st25r3916_Verify();
    ao_spy_drain();
    /* The next case's nfc_up() puts the reader back. */
}

/* ---- The facade: kode_block_nfc_read_tag ----------------------------------------
 * The blocking calls. AO_Nfc runs from inside the facade's wait, armed with
 * fake_sem_wait_hook(), which is where it runs on the board too. The facade's
 * semaphore is NULL on the host (kode_init() never ran), so AO_Nfc's answer
 * signals nothing and the hook's khal_sem_signal(NULL) is that missing wake -
 * the test_rfid_ao.c idiom.
 */

/** @brief AO_Nfc answers whatever is queued, from inside the facade's wait. */
static void nfc_answers_while_asleep(void)
{
    (void)qf_support_pump(AO_Nfc, 4U);
    khal_sem_signal(NULL);
}

/** @brief A discovery window that closes with nothing on the antenna. */
static void nfc_empty_window_while_asleep(void)
{
    kdrv_st25r3916_discover_start_ExpectAndReturn(KODE_OK);
    kdrv_st25r3916_worker_Ignore();
    kdrv_st25r3916_activated_IgnoreAndReturn(false);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    kdrv_st25r3916_field_off_Expect();
    (void)qf_support_tick(AO_Nfc, WIN_TICKS + 1U);
    khal_sem_signal(NULL);
}

TEST_CASE("kode_block_nfc_read_tag is KODE_TIMEOUT when the window closes empty",
          "[nfc_ao]")
{
    nfc_up();
    knfc_tag_t tag;
    (void)memset(&tag, 0xA5, sizeof(tag));
    fake_sem_wait_hook(&nfc_empty_window_while_asleep);

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_TIMEOUT, kode_block_nfc_read_tag(&tag, WIN_MS),
        "an empty window reached the caller as something else");
    /* The window is over: nothing paced behind it. */
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Nfc, 30U));
    Mockkdrv_st25r3916_Verify();
}

TEST_CASE("kode_block_nfc_read_tag is KODE_DENIED with no reader fitted", "[nfc_ao]")
{
    nfc_up();
    nfc_restart(false);
    knfc_tag_t tag;
    fake_sem_wait_hook(&nfc_answers_while_asleep);

    /* A strict mock with no expectation: the absent reader is never asked. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kode_block_nfc_read_tag(&tag, WIN_MS),
        "a missing reader reached the caller as something else");
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Nfc, 30U));
    Mockkdrv_st25r3916_Verify();
    /* The next case's nfc_up() puts the reader back. */
}

/* ---- emulation ---------------------------------------------------------- *
 * The driver is mocked, so what is under test is AO_Nfc's half: it takes the
 * request, holds REQ_EMU while the driver's worker runs, samples the snapshot
 * once a tick and publishes an edge for each thing that changed, and lets go
 * only when the worker reports the window closed.
 *
 * The sampler is armed the whole time a window is held, so every tick fires it
 * and reads the snapshot - the tests account for that, one snapshot per tick.
 * Where a case does not care what the sampler sees, it lets the snapshot be
 * ignored, and AO_Nfc reads an all-zero one (emu_sample clears it first), which
 * publishes nothing.
 */

/** @brief The next published emulation edge. */
static const NfcEmuEvt *take_emu(void)
{
    const QEvt *e = ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "no emulation edge was published");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)NFC_EMU_SIG, (int)e->sig,
                                  "not an NfcEmuEvt");
    return (const NfcEmuEvt *)e;
}

/** @brief The next thing posted to the spy, which must be an emulation answer. */
static const NfcEmuRspEvt *take_emu_rsp(void)
{
    const QEvt *e = ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "no emulation answer was posted");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)NFC_EMU_RSP_SIG, (int)e->sig,
                                  "not an NfcEmuRspEvt");
    return (const NfcEmuRspEvt *)e;
}

/** @brief Fire one tick that hands the sampler @p snap. */
static void emu_tick(const knfc_emu_stat_t *snap)
{
    kdrv_st25r3916_emulate_snapshot_ExpectAnyArgs();
    kdrv_st25r3916_emulate_snapshot_ReturnThruPtr_out((knfc_emu_stat_t *)snap);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_Nfc, 1U));
}

/** @brief Start a `link` emulation and take the OK answer. */
static void emu_start_link(void)
{
    static knfc_emu_card_t card;

    (void)memset(&card, 0, sizeof(card));
    card.uid_len = 0U;   /* present a Type-2 tag */
    card.uri     = true;
    (void)snprintf(card.ndef, sizeof(card.ndef), "%s", "https://kode.diy");

    kdrv_st25r3916_emulate_start_ExpectAnyArgsAndReturn(KODE_OK);
    nfc_emulate_request(&card, NULL, NULL, 0U, NULL, ao_spy());
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, take_emu_rsp()->rc);
}

TEST_CASE("emulation plays field, select, read and a stop with the total",
          "[nfc_ao]")
{
    nfc_up();
    ao_spy_subscribe(NFC_EMU_SIG);
    emu_start_link();

    knfc_emu_stat_t s = { .active = true, .field = true, .selected = false,
                          .sessions = 0U, .reads = 0U };
    emu_tick(&s);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)NFC_EMU_KIND_FIELD, take_emu()->kind);

    s.selected = true;
    s.sessions = 1U;
    emu_tick(&s);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)NFC_EMU_KIND_SELECTED, take_emu()->kind);

    s.reads = 1U;
    emu_tick(&s);
    const NfcEmuEvt *rd = take_emu();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)NFC_EMU_KIND_READ, rd->kind);
    TEST_ASSERT_EQUAL_UINT32(1U, rd->reads);

    /* Nothing changed this tick: no edge. */
    emu_tick(&s);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    /* Stop. One tick dispatches the request and fires the sampler: on_emu_stop
     * reads the count for its answer, then the sampler sees the worker gone and
     * publishes the total. */
    knfc_emu_stat_t gone = { .active = false, .field = false, .selected = false,
                             .sessions = 1U, .reads = 1U };
    kdrv_st25r3916_emulate_snapshot_ExpectAnyArgs();
    kdrv_st25r3916_emulate_snapshot_ReturnThruPtr_out(&s);       /* on_emu_stop */
    kdrv_st25r3916_emulate_stop_ExpectAndReturn(KODE_OK);
    kdrv_st25r3916_emulate_snapshot_ExpectAnyArgs();
    kdrv_st25r3916_emulate_snapshot_ReturnThruPtr_out(&gone);    /* the sampler */
    nfc_emulate_stop_request(NULL, NULL, NULL, 0U, NULL, ao_spy());
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_tick(AO_Nfc, 1U));

    const NfcEmuRspEvt *sr = take_emu_rsp();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, sr->rc);
    TEST_ASSERT_EQUAL_UINT32(1U, sr->reads);
    const NfcEmuEvt *st = take_emu();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)NFC_EMU_KIND_STOPPED, st->kind);
    TEST_ASSERT_EQUAL_UINT32(1U, st->reads);

    /* REQ_EMU is free again: nothing armed, nothing left. */
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Nfc, 30U));
    Mockkdrv_st25r3916_Verify();
    ao_spy_drain();
}

TEST_CASE("a second request while emulating is refused as busy", "[nfc_ao]")
{
    nfc_up();
    /* The sampler runs throughout; its snapshot is not what this case is
     * about, so let it be ignored (read as all-zero, publishing nothing). */
    kdrv_st25r3916_emulate_snapshot_Ignore();
    emu_start_link();

    /* A read now finds the reader held: refused, no window opened. One tick
     * dispatches the read request and fires the sampler. */
    s_rc = RC_UNWRITTEN;
    nfc_field_request(&s_out, &s_rc, khal_sem_create(), WIN_MS, 9U, &s_ack,
                      ao_spy());
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_tick(AO_Nfc, 1U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, take_rsp()->st);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, s_rc);

    /* Clean up: stop and let the worker release, so the next case is idle. */
    kdrv_st25r3916_emulate_stop_ExpectAndReturn(KODE_OK);
    nfc_emulate_stop_request(NULL, NULL, NULL, 0U, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_tick(AO_Nfc, 1U));
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Nfc, 30U));
    Mockkdrv_st25r3916_Verify();
    ao_spy_drain();
}

TEST_CASE("emulation on a board with no reader is refused", "[nfc_ao]")
{
    knfc_emu_card_t card;

    nfc_up();
    nfc_restart(false);   /* come up with no reader fitted */

    (void)memset(&card, 0, sizeof(card));
    card.uid_len = 0U;

    /* No driver call, no sampler armed: absence is answered from the AO. */
    nfc_emulate_request(&card, NULL, NULL, 0U, NULL, ao_spy());
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, take_emu_rsp()->rc);
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Nfc, 30U));
    Mockkdrv_st25r3916_Verify();
    ao_spy_drain();
}

TEST_CASE("a driver that refuses to emulate passes the status back and frees",
          "[nfc_ao]")
{
    knfc_emu_card_t card;

    nfc_up();

    (void)memset(&card, 0, sizeof(card));
    card.uid_len = 0U;

    kdrv_st25r3916_emulate_start_ExpectAnyArgsAndReturn(KODE_ERROR);
    nfc_emulate_request(&card, NULL, NULL, 0U, NULL, ao_spy());
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, take_emu_rsp()->rc);

    /* REQ_EMU was freed, not stuck: a fresh start is accepted, not refused
     * busy. */
    kdrv_st25r3916_emulate_snapshot_Ignore();
    kdrv_st25r3916_emulate_start_ExpectAnyArgsAndReturn(KODE_OK);
    nfc_emulate_request(&card, NULL, NULL, 0U, NULL, ao_spy());
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, take_emu_rsp()->rc);

    /* Clean up. */
    kdrv_st25r3916_emulate_stop_ExpectAndReturn(KODE_OK);
    nfc_emulate_stop_request(NULL, NULL, NULL, 0U, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_tick(AO_Nfc, 1U));
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Nfc, 30U));
    Mockkdrv_st25r3916_Verify();
    ao_spy_drain();
}

/* ---- Which record an NDEF read found --------------------------------------
 * kode_block_nfc_ndef_read() says whether the message was a link or a text, from
 * the record the parser met, through the same answer the length travels in.
 */

/** @brief Blocks 0..3 of a formatted Type-2 tag: the CC opens the data area. */
static const uint8_t s_t2t_head[16] = {
    0x04U, 0x4BU, 0x4FU, 0x44U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0xE1U, 0x10U, 0x12U, 0x00U,
};

/** @brief The data area of a tag carrying the URI "https://a.io". */
static const uint8_t s_t2t_uri_area[16] = {
    0x03U, 0x09U, 0xD1U, 0x01U, 0x05U, 'U', 0x04U, 'a', '.', 'i', 'o',
    0xFEU, 0x00U, 0x00U, 0x00U, 0x00U,
};

/** @brief The data area of a tag carrying the text "hi". */
static const uint8_t s_t2t_text_area[16] = {
    0x03U, 0x09U, 0xD1U, 0x01U, 0x05U, 'T', 0x02U, 'e', 'n', 'h', 'i',
    0xFEU, 0x00U, 0x00U, 0x00U, 0x00U,
};

static const uint8_t *s_t2t_area;   /**< What the tag in front answers with. */

/** @brief Bytes of a Type-2 block, the unit the reader writes. */
#define T2T_BLOCK_BYTES  4U
/** @brief Bytes of the tag image the write stub keeps: the head and a data area. */
#define T2T_IMAGE_BYTES  256U
/** @brief The block the data area starts at, after the UID, lock and CC blocks. */
#define T2T_AREA_BLOCK   4U

static uint8_t s_t2t_written[T2T_IMAGE_BYTES];   /**< What the reader wrote, block by block. */

/** @brief The mock reader's Type-2 WRITE: one block into the image. */
static kode_status_t t2t_write_stub(uint8_t block, const uint8_t *data, int calls)
{
    size_t const at = (size_t)block * T2T_BLOCK_BYTES;

    (void)calls;
    TEST_ASSERT_TRUE_MESSAGE((at + T2T_BLOCK_BYTES) <= sizeof(s_t2t_written), "a write past the image");
    (void)memcpy(&s_t2t_written[at], data, T2T_BLOCK_BYTES);
    return KODE_OK;
}

/** @brief A data area longer than one READ, served block by block; NULL serves s_t2t_area. */
static const uint8_t *s_t2t_image;

/** @brief The mock reader's Type-2 READ: the head below block 4, else the area. */
static kode_status_t t2t_read_stub(uint8_t block, uint8_t *buf, uint16_t cap,
                                   uint16_t *got, int calls)
{
    const uint8_t *src = s_t2t_area;

    (void)calls;
    (void)cap;
    if ( 4U > block )
    {
        src = s_t2t_head;
    }
    else if ( NULL != s_t2t_image )
    {
        src = &s_t2t_image[(size_t)(block - T2T_AREA_BLOCK) * T2T_BLOCK_BYTES];
    }
    else
    {
        /* the one area, whatever the block */
    }
    (void)memcpy(buf, src, sizeof(s_t2t_head));
    *got = (uint16_t)sizeof(s_t2t_head);
    return KODE_OK;
}

/** @brief A Type-2 tag turns up while the facade sleeps on its read. */
static void nfc_t2t_arrives_while_asleep(void)
{
    static uint8_t  uid[KDRV_ST25R3916_UID_MAX] = { 0x04U, 0x4BU, 0x4FU, 0x44U };
    static uint8_t  uid_len = 4U;
    static uint16_t atqa    = 0x0044U;
    static uint8_t  sak     = 0x00U;

    kdrv_st25r3916_discover_start_ExpectAndReturn(KODE_OK);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));

    kdrv_st25r3916_worker_Ignore();
    kdrv_st25r3916_activated_IgnoreAndReturn(true);
    kdrv_st25r3916_active_tag_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_st25r3916_active_tag_ReturnArrayThruPtr_uid(uid, KDRV_ST25R3916_UID_MAX);
    kdrv_st25r3916_active_tag_ReturnThruPtr_uid_len(&uid_len);
    kdrv_st25r3916_active_tag_ReturnThruPtr_atqa(&atqa);
    kdrv_st25r3916_active_tag_ReturnThruPtr_sak(&sak);
    kdrv_st25r3916_t2t_read_StubWithCallback(&t2t_read_stub);
    kdrv_st25r3916_field_off_Expect();
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_Nfc, 1U));
    khal_sem_signal(NULL);
}

TEST_CASE("kode_block_nfc_ndef_read says a URI record was a URI", "[nfc_ao]")
{
    char   out[64];
    size_t n   = 0U;
    bool   uri = false;

    nfc_up();
    s_t2t_area = s_t2t_uri_area;
    fake_sem_wait_hook(&nfc_t2t_arrives_while_asleep);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kode_block_nfc_ndef_read(out, sizeof(out), &n, &uri, WIN_MS));
    TEST_ASSERT_EQUAL_STRING("https://a.io", out);
    TEST_ASSERT_EQUAL_UINT(strlen("https://a.io"), n);
    TEST_ASSERT_TRUE(uri);
    Mockkdrv_st25r3916_Verify();
}

TEST_CASE("kode_block_nfc_ndef_read says a Text record was not a URI", "[nfc_ao]")
{
    char   out[64];
    size_t n   = 0U;
    bool   uri = true;

    nfc_up();
    s_t2t_area = s_t2t_text_area;
    fake_sem_wait_hook(&nfc_t2t_arrives_while_asleep);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kode_block_nfc_ndef_read(out, sizeof(out), &n, &uri, WIN_MS));
    TEST_ASSERT_EQUAL_STRING("hi", out);
    TEST_ASSERT_FALSE(uri);
    Mockkdrv_st25r3916_Verify();
}

TEST_CASE("kode_block_nfc_ndef_read takes a NULL uri", "[nfc_ao]")
{
    char out[64];

    nfc_up();
    s_t2t_area = s_t2t_uri_area;
    fake_sem_wait_hook(&nfc_t2t_arrives_while_asleep);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kode_block_nfc_ndef_read(out, sizeof(out), NULL, NULL, WIN_MS));
    TEST_ASSERT_EQUAL_STRING("https://a.io", out);
    Mockkdrv_st25r3916_Verify();
}

TEST_CASE("kode_block_nfc_ndef_read leaves uri alone when no tag turns up",
          "[nfc_ao]")
{
    char out[64];
    bool uri = true;

    nfc_up();
    fake_sem_wait_hook(&nfc_empty_window_while_asleep);

    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT,
        kode_block_nfc_ndef_read(out, sizeof(out), NULL, &uri, WIN_MS));
    TEST_ASSERT_TRUE(uri);
    Mockkdrv_st25r3916_Verify();
}

/* ---- When AO_Nfc emulates --------------------------------------------------
 * nfc_is_emulating() is true from the moment a window is really up until the
 * reader is released, and false on every way out.
 */

TEST_CASE("nfc_is_emulating is false at rest and mirrored by the sys call",
          "[nfc_ao]")
{
    nfc_up();

    TEST_ASSERT_FALSE(nfc_is_emulating());
    TEST_ASSERT_FALSE(kode_sys_nfc_is_emulating());
    Mockkdrv_st25r3916_Verify();
}

TEST_CASE("nfc_is_emulating is true once the window is up and false after stop",
          "[nfc_ao]")
{
    nfc_up();
    emu_start_link();

    TEST_ASSERT_TRUE(nfc_is_emulating());
    TEST_ASSERT_TRUE(kode_sys_nfc_is_emulating());

    /* A stop is accepted at once, but the reader is held until the worker
     * lets go: the snapshot below says it has not. */
    knfc_emu_stat_t held = { .active = true, .field = false, .selected = false,
                             .sessions = 0U, .reads = 0U };
    kdrv_st25r3916_emulate_snapshot_ExpectAnyArgs();
    kdrv_st25r3916_emulate_snapshot_ReturnThruPtr_out(&held);
    kdrv_st25r3916_emulate_stop_ExpectAndReturn(KODE_OK);
    kdrv_st25r3916_emulate_snapshot_ExpectAnyArgs();
    kdrv_st25r3916_emulate_snapshot_ReturnThruPtr_out(&held);
    nfc_emulate_stop_request(NULL, NULL, NULL, 0U, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_tick(AO_Nfc, 1U));
    TEST_ASSERT_TRUE_MESSAGE(nfc_is_emulating(),
                             "the reader was reported free while still held");

    knfc_emu_stat_t gone = { .active = false, .field = false, .selected = false,
                             .sessions = 0U, .reads = 0U };
    emu_tick(&gone);
    TEST_ASSERT_FALSE(nfc_is_emulating());
    TEST_ASSERT_FALSE(kode_sys_nfc_is_emulating());
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Nfc, 30U));
    Mockkdrv_st25r3916_Verify();
    ao_spy_drain();
}

TEST_CASE("nfc_is_emulating stays false when the window would not open",
          "[nfc_ao]")
{
    knfc_emu_card_t card;

    nfc_up();
    (void)memset(&card, 0, sizeof(card));

    kdrv_st25r3916_emulate_start_ExpectAnyArgsAndReturn(KODE_ERROR);
    nfc_emulate_request(&card, NULL, NULL, 0U, NULL, ao_spy());
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, take_emu_rsp()->rc);

    TEST_ASSERT_FALSE(nfc_is_emulating());
    TEST_ASSERT_FALSE(kode_sys_nfc_is_emulating());
    Mockkdrv_st25r3916_Verify();
    ao_spy_drain();
}

TEST_CASE("nfc_is_emulating stays false on a board with no reader", "[nfc_ao]")
{
    knfc_emu_card_t card;

    nfc_up();
    nfc_restart(false);
    (void)memset(&card, 0, sizeof(card));

    nfc_emulate_request(&card, NULL, NULL, 0U, NULL, ao_spy());
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, take_emu_rsp()->rc);
    TEST_ASSERT_FALSE(nfc_is_emulating());
    Mockkdrv_st25r3916_Verify();
    ao_spy_drain();
}

/* ---- kode_sys's requests and the certification ones: answered to the asking AO ---
 * They post and return; the answer is an event posted to the AO that asked, the
 * spy here, carrying the status and a copy: a message travels by value, so
 * nothing is lent. The reader's own refusals - absent, busy, a window that
 * would not open - come in the answer, never as a silence.
 */

/** @brief A static event AO_Nfc ignores, to fill its queue with. */
static const QEvt s_nfc_filler = QEVT_INITIALIZER(IMU_TEST_REQ_SIG);

/** @brief Most blocks a case takes out of a pool to leave it dry. */
#define DRY_MAX  128U

/** @brief The next thing the spy holds, which must be @p sig. */
static const void *ask_take(enum_t sig)
{
    const QEvt *e = ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "every accepted request is answered");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)sig, (uint32_t)e->sig, "the wrong answer");
    return (const void *)e;
}

/** @brief AO_Nfc up, its queue one a margin post can read, both askers' mailboxes empty. */
static void ask_up(void)
{
    s_t2t_image = NULL;
    nfc_up();
    qf_support_margin_queue(AO_Nfc);
    ao_spy_drain();
    ao_spy_other_drain();
}

/** @brief Leave nothing owed behind a case. */
static void ask_down(void)
{
    ao_spy_drain();
    ao_spy_other_drain();
    Mockkdrv_st25r3916_Verify();
}

/** @brief Expect the discovery a tag request opens, and let AO_Nfc take the request. */
static void tag_window_opens(void)
{
    kdrv_st25r3916_discover_start_ExpectAndReturn(KODE_OK);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
}

/** @brief A Type-2 tag, UID 04 4B 4F 44, turns up on the next tick of an open window. */
static void tag_turns_up(void)
{
    static uint8_t  uid[KDRV_ST25R3916_UID_MAX] = { 0x04U, 0x4BU, 0x4FU, 0x44U };
    static uint8_t  uid_len = 4U;
    static uint16_t atqa    = 0x0044U;
    static uint8_t  sak     = 0x00U;

    kdrv_st25r3916_worker_Ignore();
    kdrv_st25r3916_activated_IgnoreAndReturn(true);
    kdrv_st25r3916_active_tag_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_st25r3916_active_tag_ReturnArrayThruPtr_uid(uid, KDRV_ST25R3916_UID_MAX);
    kdrv_st25r3916_active_tag_ReturnThruPtr_uid_len(&uid_len);
    kdrv_st25r3916_active_tag_ReturnThruPtr_atqa(&atqa);
    kdrv_st25r3916_active_tag_ReturnThruPtr_sak(&sak);
    kdrv_st25r3916_t2t_read_StubWithCallback(&t2t_read_stub);
    kdrv_st25r3916_t2t_write_StubWithCallback(&t2t_write_stub);
    kdrv_st25r3916_field_off_Expect();
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_Nfc, 1U));
}

/** @brief A window that closes with nothing on the antenna, after the request opened it. */
static void tag_window_closes_empty(void)
{
    kdrv_st25r3916_worker_Ignore();
    kdrv_st25r3916_activated_IgnoreAndReturn(false);
    kdrv_st25r3916_field_off_Expect();
    (void)qf_support_tick(AO_Nfc, WIN_TICKS + 1U);
}

TEST_CASE("kode_sys_nfc_read_tag answers the asking AO once, with the tag by value",
          "[nfc_ao][ask]")
{
    const kode_sys_nfc_read_answer_t *a;

    ask_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_read_tag(ao_spy(), WIN_MS));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kode_sys_nfc_read_tag(ao_spy(), WIN_MS),
                                  "a second request of the kind waits for the first's answer");
    tag_window_opens();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "nothing is answered before a tag turns up");
    tag_turns_up();

    a = (const kode_sys_nfc_read_answer_t *)ask_take(NFC_READ_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT8(4U, a->tag.uid_len);
    TEST_ASSERT_EQUAL_HEX8(0x04U, a->tag.uid[0]);
    TEST_ASSERT_EQUAL_HEX8(0x44U, a->tag.uid[3]);
    TEST_ASSERT_EQUAL_HEX16(0x0044U, a->tag.atqa);
    TEST_ASSERT_EQUAL_UINT8(0U, a->tag.sak);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KNFC_TAG_T2T, a->tag.type);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "posted once, not published as well");
    ao_spy_release();

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_nfc_read_tag(ao_spy(), WIN_MS),
                                  "the answer opens the door again");
    tag_window_opens();
    tag_window_closes_empty();
    a = (const kode_sys_nfc_read_answer_t *)ask_take(NFC_READ_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_TIMEOUT, a->rc);
    TEST_ASSERT_EQUAL_UINT8(0U, a->tag.uid_len);
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Nfc, 30U));
    ask_down();
}

TEST_CASE("a read ask with no reader fitted is answered KODE_IO_ERROR and asks the driver nothing",
          "[nfc_ao][ask]")
{
    const kode_sys_nfc_read_answer_t *a;

    ask_up();
    nfc_restart(false);
    qf_support_margin_queue(AO_Nfc);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_read_tag(ao_spy(), WIN_MS));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    a = (const kode_sys_nfc_read_answer_t *)ask_take(NFC_READ_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_IO_ERROR, a->rc);
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Nfc, 30U));
    ask_down();
    /* The next case's nfc_up() puts the reader back. */
}

TEST_CASE("two AOs asking for a tag are both answered: the busy reader refuses the second",
          "[nfc_ao][ask]")
{
    const kode_sys_nfc_read_answer_t *a;
    const QEvt                       *o;

    ask_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_read_tag(ao_spy(), WIN_MS));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_nfc_read_tag(ao_spy_other(), WIN_MS),
                                  "the door is the asker's, not the kind's");
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_nfc_read_tag(ao_spy_other(), WIN_MS));
    kdrv_st25r3916_discover_start_ExpectAndReturn(KODE_OK);
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_pump(AO_Nfc, 4U));

    o = ao_spy_other_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(o, "the second asker is answered too");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)NFC_READ_RSP_SIG, (uint32_t)o->sig);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED, ((const kode_sys_nfc_read_answer_t *)(const void *)o)->rc);
    QF_gc(o);
    TEST_ASSERT_NULL(ao_spy_other_take());

    tag_turns_up();
    a = (const kode_sys_nfc_read_answer_t *)ask_take(NFC_READ_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    ask_down();
}

TEST_CASE("kode_sys_nfc_ndef_read answers the message and whether it was a link",
          "[nfc_ao][ask]")
{
    const kode_sys_nfc_ndef_answer_t *a;

    ask_up();
    s_t2t_area = s_t2t_uri_area;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_ndef_read(ao_spy(), WIN_MS));
    tag_window_opens();
    tag_turns_up();
    a = (const kode_sys_nfc_ndef_answer_t *)ask_take(NFC_NDEF_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_TRUE(a->have_tag);
    TEST_ASSERT_TRUE(a->uri);
    TEST_ASSERT_EQUAL_STRING("https://a.io", a->text);
    TEST_ASSERT_EQUAL_UINT(strlen("https://a.io"), a->len);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    ao_spy_release();

    s_t2t_area = s_t2t_text_area;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_ndef_read(ao_spy(), WIN_MS));
    tag_window_opens();
    tag_turns_up();
    a = (const kode_sys_nfc_ndef_answer_t *)ask_take(NFC_NDEF_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_FALSE(a->uri);
    TEST_ASSERT_EQUAL_STRING("hi", a->text);
    ask_down();
}

TEST_CASE("a window that closes with no tag answers an NDEF read KODE_TIMEOUT with nothing in it",
          "[nfc_ao][ask]")
{
    const kode_sys_nfc_ndef_answer_t *a;

    ask_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_ndef_read(ao_spy(), WIN_MS));
    tag_window_opens();
    tag_window_closes_empty();
    a = (const kode_sys_nfc_ndef_answer_t *)ask_take(NFC_NDEF_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_TIMEOUT, a->rc);
    TEST_ASSERT_FALSE(a->have_tag);
    TEST_ASSERT_EQUAL_UINT(0U, a->len);
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Nfc, 30U));
    ask_down();
}

/** @brief Bytes of the long data area the read cases serve: past the CC's 144, and one READ more. */
#define T2T_LONG_IMAGE_BYTES  176U
/** @brief The bytes of a Text record of "en" around its message: TLV, record head, status, language. */
#define T2T_TEXT_HEAD_BYTES   9U
/** @brief A Text record's TLV length over its message: record head, status and language. */
#define T2T_TEXT_TLV_EXTRA    7U
/** @brief A Text record's payload over its message: status and language. */
#define T2T_TEXT_PAYLOAD_EXTRA 3U

static uint8_t s_t2t_long[T2T_LONG_IMAGE_BYTES];   /**< A long message's data area. */

/**
 * @brief Put a Text record of "en" and @p len letters in s_t2t_long.
 * @param len The message's length.
 * @param c   The letter it repeats.
 */
static void t2t_long_text(size_t len, char c)
{
    const uint8_t head[T2T_TEXT_HEAD_BYTES] = {
        0x03U, (uint8_t)(len + T2T_TEXT_TLV_EXTRA), 0xD1U, 0x01U,
        (uint8_t)(len + T2T_TEXT_PAYLOAD_EXTRA), 'T', 0x02U, 'e', 'n',
    };

    (void)memset(s_t2t_long, 0, sizeof(s_t2t_long));
    (void)memcpy(s_t2t_long, head, sizeof(head));
    (void)memset(&s_t2t_long[sizeof(head)], (int)c, len);
    s_t2t_long[sizeof(head) + len] = 0xFEU;
}

TEST_CASE("an NDEF read of the longest message answers it whole, and a failed one answers no fragment",
          "[nfc_ao][ask]")
{
    size_t const                      longest = (size_t)KODE_SYS_NFC_TEXT_MAX - 1U;
    char                              want[KODE_SYS_NFC_TEXT_MAX];
    const kode_sys_nfc_ndef_answer_t *a;

    ask_up();
    t2t_long_text(longest, 'r');
    s_t2t_image = s_t2t_long;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_ndef_read(ao_spy(), WIN_MS));
    tag_window_opens();
    tag_turns_up();
    a = (const kode_sys_nfc_ndef_answer_t *)ask_take(NFC_NDEF_RSP_SIG);
    (void)memset(want, 'r', longest);
    want[longest] = '\0';
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT((uint32_t)longest, (uint32_t)a->len);
    TEST_ASSERT_EQUAL_STRING(want, a->text);
    ao_spy_release();

    /* One letter more does not fit the answer: refused, and nothing of it travels. */
    t2t_long_text(longest + 1U, 's');
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_ndef_read(ao_spy(), WIN_MS));
    tag_window_opens();
    tag_turns_up();
    a = (const kode_sys_nfc_ndef_answer_t *)ask_take(NFC_NDEF_RSP_SIG);
    TEST_ASSERT_NOT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, (uint32_t)a->len, "a failed read carried a fragment");
    TEST_ASSERT_EQUAL_UINT8(0U, (uint8_t)a->text[0]);
    TEST_ASSERT_FALSE(a->uri);
    s_t2t_image = NULL;
    ask_down();
}

/** @brief The data area a URI write of "https://kode.diy" leaves: the prefix code 0x04, then the rest. */
static const uint8_t s_t2t_uri_written[] = {
    0x03U, 0x0DU, 0xD1U, 0x01U, 0x09U, 'U', 0x04U, 'k', 'o', 'd', 'e', '.', 'd', 'i', 'y', 0xFEU,
};

/** @brief The data area a Text write of "hello" leaves. */
static const uint8_t s_t2t_hello_written[] = {
    0x03U, 0x0CU, 0xD1U, 0x01U, 0x08U, 'T', 0x02U, 'e', 'n', 'h', 'e', 'l', 'l', 'o', 0xFEU,
};

/** @brief Where the data area starts in the image the write stub keeps. */
#define T2T_AREA_AT  (T2T_AREA_BLOCK * T2T_BLOCK_BYTES)

TEST_CASE("kode_sys_nfc_ndef_write_uri and _text write a copy of the message and answer OK",
          "[nfc_ao][ask]")
{
    char                              uri[] = "https://kode.diy";
    const kode_sys_nfc_ndef_answer_t *a;

    ask_up();
    s_t2t_area = s_t2t_text_area;
    (void)memset(s_t2t_written, 0, sizeof(s_t2t_written));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_ndef_write_uri(ao_spy(), uri, WIN_MS));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kode_sys_nfc_ndef_write_text(ao_spy(), "x", WIN_MS),
                                  "a read and a write share the NDEF door");
    uri[0] = 'X';   /* the message was copied: changing the source now changes nothing */
    tag_window_opens();
    tag_turns_up();
    a = (const kode_sys_nfc_ndef_answer_t *)ask_take(NFC_NDEF_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_TRUE(a->have_tag);
    TEST_ASSERT_EQUAL_UINT(0U, a->len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(s_t2t_uri_written, &s_t2t_written[T2T_AREA_AT],
                                         sizeof(s_t2t_uri_written), "the tag holds the link as asked");
    ao_spy_release();

    (void)memset(s_t2t_written, 0, sizeof(s_t2t_written));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_ndef_write_text(ao_spy(), "hello", WIN_MS));
    tag_window_opens();
    tag_turns_up();
    a = (const kode_sys_nfc_ndef_answer_t *)ask_take(NFC_NDEF_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_t2t_hello_written, &s_t2t_written[T2T_AREA_AT], sizeof(s_t2t_hello_written));
    ask_down();
}

TEST_CASE("an NDEF message of the longest length is written whole, and one longer is refused in the owner",
          "[nfc_ao][ask]")
{
    char                              text[KODE_SYS_NFC_TEXT_MAX + 1U];
    size_t const                      longest = (size_t)KODE_SYS_NFC_TEXT_MAX - 1U;
    const uint8_t                     head[T2T_TEXT_HEAD_BYTES] = {
        0x03U, (uint8_t)(longest + T2T_TEXT_TLV_EXTRA), 0xD1U, 0x01U,
        (uint8_t)(longest + T2T_TEXT_PAYLOAD_EXTRA), 'T', 0x02U, 'e', 'n',
    };
    const kode_sys_nfc_ndef_answer_t *a;

    ask_up();
    s_t2t_area = s_t2t_text_area;
    (void)memset(text, 'w', sizeof(text));
    text[longest] = '\0';
    (void)memset(s_t2t_written, 0, sizeof(s_t2t_written));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_ndef_write_text(ao_spy(), text, WIN_MS));
    tag_window_opens();
    tag_turns_up();
    a = (const kode_sys_nfc_ndef_answer_t *)ask_take(NFC_NDEF_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(head, &s_t2t_written[T2T_AREA_AT], sizeof(head));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(text, &s_t2t_written[T2T_AREA_AT + sizeof(head)], longest,
                                     "the message reached the tag whole");
    TEST_ASSERT_EQUAL_HEX8(0xFEU, s_t2t_written[T2T_AREA_AT + sizeof(head) + longest]);
    ao_spy_release();

    /* One more: the owner refuses it at its own door, nothing is posted. */
    text[longest]      = 'w';
    text[longest + 1U] = '\0';
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, nfc_ndef_ask(ao_spy(), text, false, WIN_MS));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Nfc->eQueue));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, nfc_ndef_ask(ao_spy(), NULL, false, WIN_MS),
                                  "a refusal takes no door");
    tag_window_opens();
    tag_window_closes_empty();
    (void)ask_take(NFC_NDEF_RSP_SIG);
    ask_down();
}

/** @brief The card the driver was last asked to present. */
static knfc_emu_card_t s_emu_seen;

/** @brief kdrv_st25r3916_emulate_start() as AO_Nfc calls it: keep what it was handed. */
static kode_status_t emu_start_records(const knfc_emu_card_t *card, int calls)
{
    (void)calls;
    s_emu_seen = *card;
    return KODE_OK;
}

TEST_CASE("kode_sys_nfc_emulate_start presents a copy of the card and _stop answers the READs served",
          "[nfc_ao][ask]")
{
    kode_sys_nfc_card_t              card;
    knfc_emu_stat_t                  playing = { .active = true, .field = false, .selected = false,
                                                 .sessions = 0U, .reads = 3U };
    knfc_emu_stat_t                  gone    = { .active = false, .field = false, .selected = false,
                                                 .sessions = 0U, .reads = 3U };
    const kode_sys_nfc_emu_answer_t *a;

    ask_up();
    (void)memset(&s_emu_seen, 0, sizeof(s_emu_seen));
    (void)memset(&card, 0, sizeof(card));
    card.uri = true;
    (void)snprintf(card.ndef, sizeof(card.ndef), "%s", "https://kode.diy");

    kdrv_st25r3916_emulate_start_StubWithCallback(&emu_start_records);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_emulate_start(ao_spy(), &card));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_nfc_emulate_stop(ao_spy()));   /* one emulation request at a time */
    (void)memset(&card, 0, sizeof(card));   /* the card was copied */
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    a = (const kode_sys_nfc_emu_answer_t *)ask_take(NFC_EMU_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_TRUE(s_emu_seen.uri);
    TEST_ASSERT_EQUAL_STRING("https://kode.diy", s_emu_seen.ndef);
    TEST_ASSERT_TRUE(kode_sys_nfc_is_emulating());
    ao_spy_release();

    /* One tick dispatches the stop and fires the sampler: the stop reads the
     * count for its answer, then the sampler sees the worker gone. */
    kdrv_st25r3916_emulate_snapshot_ExpectAnyArgs();
    kdrv_st25r3916_emulate_snapshot_ReturnThruPtr_out(&playing);
    kdrv_st25r3916_emulate_stop_ExpectAndReturn(KODE_OK);
    kdrv_st25r3916_emulate_snapshot_ExpectAnyArgs();
    kdrv_st25r3916_emulate_snapshot_ReturnThruPtr_out(&gone);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_nfc_emulate_stop(ao_spy()),
                                  "the answer opens the door again");
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_tick(AO_Nfc, 1U));
    a = (const kode_sys_nfc_emu_answer_t *)ask_take(NFC_EMU_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT(3U, a->reads);
    TEST_ASSERT_FALSE(kode_sys_nfc_is_emulating());
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Nfc, 30U));
    ask_down();
}

TEST_CASE("a second stop from the opener while the card is stopping is answered, and the card ends once",
          "[nfc_ao][ask]")
{
    kode_sys_nfc_card_t              card;
    knfc_emu_stat_t                  playing = { .active = true, .field = false, .selected = false,
                                                 .sessions = 0U, .reads = 4U };
    knfc_emu_stat_t                  gone    = { .active = false, .field = false, .selected = false,
                                                 .sessions = 0U, .reads = 4U };
    const kode_sys_nfc_emu_answer_t *a;

    ask_up();
    (void)memset(&card, 0, sizeof(card));
    kdrv_st25r3916_emulate_start_ExpectAnyArgsAndReturn(KODE_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_emulate_start(ao_spy(), &card));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, ((const kode_sys_nfc_emu_answer_t *)ask_take(NFC_EMU_RSP_SIG))->rc);
    ao_spy_release();

    /* The first stop: the worker is told, and still holds the reader. */
    kdrv_st25r3916_emulate_snapshot_ExpectAnyArgs();
    kdrv_st25r3916_emulate_snapshot_ReturnThruPtr_out(&playing);
    kdrv_st25r3916_emulate_stop_ExpectAndReturn(KODE_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_emulate_stop(ao_spy()));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_settle(AO_Nfc));
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, ((const kode_sys_nfc_emu_answer_t *)ask_take(NFC_EMU_RSP_SIG))->rc);
    ao_spy_release();
    TEST_ASSERT_TRUE_MESSAGE(kode_sys_nfc_is_emulating(), "the worker has not let go yet");

    /* The second: the driver is told again, and the opener answered again. */
    kdrv_st25r3916_emulate_snapshot_ExpectAnyArgs();
    kdrv_st25r3916_emulate_snapshot_ReturnThruPtr_out(&playing);
    kdrv_st25r3916_emulate_stop_ExpectAndReturn(KODE_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_emulate_stop(ao_spy()));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_settle(AO_Nfc));
    a = (const kode_sys_nfc_emu_answer_t *)ask_take(NFC_EMU_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT(4U, a->reads);
    ao_spy_release();

    /* The worker lets go: the card ends once, and nothing is left ticking. */
    kdrv_st25r3916_emulate_snapshot_ExpectAnyArgs();
    kdrv_st25r3916_emulate_snapshot_ReturnThruPtr_out(&gone);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_Nfc, 1U));
    TEST_ASSERT_FALSE(kode_sys_nfc_is_emulating());
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Nfc, 30U));
    ask_down();
}

TEST_CASE("an emulation stop from another asker is refused and the opener's card keeps playing",
          "[nfc_ao][ask]")
{
    kode_sys_nfc_card_t              card;
    knfc_emu_stat_t                  playing = { .active = true, .field = false, .selected = false,
                                                 .sessions = 0U, .reads = 2U };
    knfc_emu_stat_t                  gone    = { .active = false, .field = false, .selected = false,
                                                 .sessions = 0U, .reads = 2U };
    const kode_sys_nfc_emu_answer_t *a;
    const QEvt                      *o;

    ask_up();
    (void)memset(&card, 0, sizeof(card));
    kdrv_st25r3916_emulate_start_ExpectAnyArgsAndReturn(KODE_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_emulate_start(ao_spy(), &card));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    a = (const kode_sys_nfc_emu_answer_t *)ask_take(NFC_EMU_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    ao_spy_release();

    /* Another AO, then the console: neither opened it, so neither ends it. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_emulate_stop(ao_spy_other()));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_settle(AO_Nfc));   /* no tick: the sampler stays put */
    o = ao_spy_other_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(o, "the foreign stop is answered");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)NFC_EMU_RSP_SIG, (uint32_t)o->sig);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED,
                           ((const kode_sys_nfc_emu_answer_t *)(const void *)o)->rc);
    QF_gc(o);
    nfc_emulate_stop_request(NULL, NULL, NULL, 0U, NULL, ao_spy_other());
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_settle(AO_Nfc));
    o = ao_spy_other_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(o, "the console's stop is answered");
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED, ((const kode_sys_nfc_emu_answer_t *)(const void *)o)->rc);
    QF_gc(o);
    TEST_ASSERT_TRUE_MESSAGE(kode_sys_nfc_is_emulating(), "a foreign stop ended the opener's card");

    /* The opener's own stop ends it. */
    kdrv_st25r3916_emulate_snapshot_ExpectAnyArgs();
    kdrv_st25r3916_emulate_snapshot_ReturnThruPtr_out(&playing);
    kdrv_st25r3916_emulate_stop_ExpectAndReturn(KODE_OK);
    kdrv_st25r3916_emulate_snapshot_ExpectAnyArgs();
    kdrv_st25r3916_emulate_snapshot_ReturnThruPtr_out(&gone);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_emulate_stop(ao_spy()));
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_tick(AO_Nfc, 1U));
    a = (const kode_sys_nfc_emu_answer_t *)ask_take(NFC_EMU_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT(2U, a->reads);
    TEST_ASSERT_FALSE(kode_sys_nfc_is_emulating());
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Nfc, 30U));
    ask_down();
}

TEST_CASE("emulation asks are answered with the status: no reader, a refusing driver, nothing playing",
          "[nfc_ao][ask]")
{
    kode_sys_nfc_card_t              card;
    const kode_sys_nfc_emu_answer_t *a;

    ask_up();
    (void)memset(&card, 0, sizeof(card));

    kdrv_st25r3916_emulate_start_ExpectAnyArgsAndReturn(KODE_ERROR);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_emulate_start(ao_spy(), &card));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    a = (const kode_sys_nfc_emu_answer_t *)ask_take(NFC_EMU_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_ERROR, a->rc);
    ao_spy_release();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_emulate_stop(ao_spy()));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    a = (const kode_sys_nfc_emu_answer_t *)ask_take(NFC_EMU_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((uint32_t)KODE_DENIED, a->rc, "nothing was playing");
    ao_spy_release();

    nfc_restart(false);
    qf_support_margin_queue(AO_Nfc);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_emulate_start(ao_spy(), &card));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    a = (const kode_sys_nfc_emu_answer_t *)ask_take(NFC_EMU_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_ERROR, a->rc);
    ask_down();
    /* The next case's nfc_up() puts the reader back. */
}

TEST_CASE("the certification listen window is answered to the asking AO with its statistics",
          "[nfc_ao][ask]")
{
    knfc_field_t                          stats = { .min = 3U, .mean = 9U, .peak = 40U, .mod = 37U,
                                                    .ext = true, .n = 80U };
    const kode_cert_nfc_field_answer_t   *r;

    ask_up();
    kdrv_st25r3916_listen_begin_ExpectAndReturn(KODE_OK);
    kdrv_st25r3916_listen_step_ExpectAndReturn(KODE_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_cert_nfc_field_read(ao_spy(), WIN_MS));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kode_cert_nfc_field_read(ao_spy(), WIN_MS),
                                  "a second window of the asker's waits for the first's answer");
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    for ( uint32_t t = 1U; WIN_TICKS > t; ++t )
    {
        kdrv_st25r3916_listen_step_ExpectAndReturn(KODE_OK);
    }
    (void)qf_support_tick(AO_Nfc, WIN_TICKS - 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "answered before the window closed");
    kdrv_st25r3916_listen_end_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_st25r3916_listen_end_ReturnThruPtr_out(&stats);
    (void)qf_support_tick(AO_Nfc, 1U);

    r = (const kode_cert_nfc_field_answer_t *)ask_take(NFC_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, r->st);
    TEST_ASSERT_EQUAL_UINT8(3U, r->min);
    TEST_ASSERT_EQUAL_UINT8(9U, r->mean);
    TEST_ASSERT_EQUAL_UINT8(40U, r->peak);
    TEST_ASSERT_EQUAL_UINT8(37U, r->mod);
    TEST_ASSERT_TRUE(r->ext);
    TEST_ASSERT_EQUAL_UINT16(80U, r->n);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "posted once, not published as well");
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Nfc, 30U));
    ask_down();
}

TEST_CASE("a listen ask is answered with the status when the reader is absent, busy or will not listen",
          "[nfc_ao][ask]")
{
    const kode_cert_nfc_field_answer_t *r;
    const QEvt                         *o;

    ask_up();

    /* The driver's own DENIED is a window that failed, never busy. */
    kdrv_st25r3916_listen_begin_ExpectAndReturn(KODE_DENIED);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_cert_nfc_field_read(ao_spy(), WIN_MS));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    r = (const kode_cert_nfc_field_answer_t *)ask_take(NFC_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, r->st);
    TEST_ASSERT_EQUAL_UINT16(0U, r->n);
    ao_spy_release();

    /* Another AO's window holds the reader: the second asker is told so, and the first window runs on. */
    open_window(WIN_MS, NULL);
    kdrv_st25r3916_listen_step_ExpectAndReturn(KODE_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_cert_nfc_field_read(ao_spy_other(), WIN_MS));
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_pump(AO_Nfc, 4U));
    o = ao_spy_other_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(o, "the second asker is answered too");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)NFC_FIELD_RSP_SIG, (uint32_t)o->sig);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, ((const kode_cert_nfc_field_answer_t *)(const void *)o)->st);
    QF_gc(o);
    for ( uint32_t t = 2U; WIN_TICKS > t; ++t )
    {
        kdrv_st25r3916_listen_step_ExpectAndReturn(KODE_OK);
    }
    (void)qf_support_tick(AO_Nfc, WIN_TICKS - 2U);
    kdrv_st25r3916_listen_end_ExpectAnyArgsAndReturn(KODE_OK);
    (void)qf_support_tick(AO_Nfc, 1U);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, s_rc);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    nfc_restart(false);
    qf_support_margin_queue(AO_Nfc);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_cert_nfc_field_read(ao_spy(), WIN_MS));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    r = (const kode_cert_nfc_field_answer_t *)ask_take(NFC_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_IO_ERROR, r->st, "absent must not read as busy");
    ask_down();
    /* The next case's nfc_up() puts the reader back. */
}

TEST_CASE("the certification carrier is commanded and dropped, and every refusal is an answer",
          "[nfc_ao][ask]")
{
    const kode_cert_carrier_answer_t *a;

    ask_up();
    kdrv_st25r3916_cert_carrier_ExpectAndReturn(false, KODE_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_cert_nfc_carrier(ao_spy(), KODE_CERT_NFC_CARRIER_ON));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_cert_nfc_carrier(ao_spy(), KODE_CERT_NFC_CARRIER_OFF));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    a = (const kode_cert_carrier_answer_t *)ask_take(NFC_CERT_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    ao_spy_release();

    /* Another request now finds the reader held. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_read_tag(ao_spy(), WIN_MS));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED,
                           ((const kode_sys_nfc_read_answer_t *)ask_take(NFC_READ_RSP_SIG))->rc);
    ao_spy_release();

    kdrv_st25r3916_field_off_Expect();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_cert_nfc_carrier(ao_spy(), KODE_CERT_NFC_CARRIER_OFF));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    a = (const kode_cert_carrier_answer_t *)ask_take(NFC_CERT_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    ao_spy_release();

    kdrv_st25r3916_cert_carrier_ExpectAndReturn(true, KODE_ERROR);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_cert_nfc_carrier(ao_spy(), KODE_CERT_NFC_CARRIER_MOD));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    a = (const kode_cert_carrier_answer_t *)ask_take(NFC_CERT_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((uint32_t)KODE_ERROR, a->rc, "a carrier the driver refused");
    ao_spy_release();

    nfc_restart(false);
    qf_support_margin_queue(AO_Nfc);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_cert_nfc_carrier(ao_spy(), KODE_CERT_NFC_CARRIER_ON));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    a = (const kode_cert_carrier_answer_t *)ask_take(NFC_CERT_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((uint32_t)KODE_ERROR, a->rc, "no reader fitted");
    ask_down();
    /* The next case's nfc_up() puts the reader back. */
}

TEST_CASE("the certification carrier off is refused while a read holds the reader, which ends on its deadline",
          "[nfc_ao][ask]")
{
    const kode_cert_carrier_answer_t *a;
    const QEvt                       *o;

    ask_up();
    ao_spy_other_drain();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_read_tag(ao_spy(), WIN_MS));
    tag_window_opens();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_cert_nfc_carrier(ao_spy_other(), KODE_CERT_NFC_CARRIER_OFF));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_settle(AO_Nfc));   /* no tick: the window's worker stays put */
    o = ao_spy_other_take();
    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_EQUAL_INT((int)NFC_CERT_RSP_SIG, (int)o->sig);
    a = (const kode_cert_carrier_answer_t *)(const void *)o;
    TEST_ASSERT_EQUAL_UINT_MESSAGE((uint32_t)KODE_DENIED, a->rc, "the read holds the reader");
    QF_gc(o);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the read is not answered by the refusal");

    tag_window_closes_empty();
    TEST_ASSERT_EQUAL_UINT_MESSAGE((uint32_t)KODE_TIMEOUT,
                                   ((const kode_sys_nfc_read_answer_t *)ask_take(NFC_READ_RSP_SIG))->rc,
                                   "the read ends on its own deadline");
    ao_spy_release();

    kdrv_st25r3916_discover_start_ExpectAndReturn(KODE_OK);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_nfc_read_tag(ao_spy(), WIN_MS),
                                  "the read's door is free again");
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    tag_window_closes_empty();
    TEST_ASSERT_EQUAL_UINT_MESSAGE((uint32_t)KODE_TIMEOUT,
                                   ((const kode_sys_nfc_read_answer_t *)ask_take(NFC_READ_RSP_SIG))->rc,
                                   "and the reader serves the next request");
    ask_down();
}

TEST_CASE("a plain task's or a badly asked NFC request is refused at once and posts nothing",
          "[nfc_ao][ask]")
{
    kode_sys_nfc_card_t card;
    char                long_text[KODE_SYS_NFC_TEXT_MAX + 1U];

    ask_up();
    (void)memset(&card, 0, sizeof(card));
    (void)memset(long_text, 'x', sizeof(long_text));
    long_text[KODE_SYS_NFC_TEXT_MAX] = '\0';

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_nfc_read_tag(NULL, WIN_MS));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_nfc_read_tag(ao_spy(), 0U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_nfc_ndef_read(NULL, WIN_MS));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_nfc_ndef_read(ao_spy(), 0U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_nfc_ndef_write_uri(NULL, "https://x", WIN_MS));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_nfc_ndef_write_uri(ao_spy(), NULL, WIN_MS));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_nfc_ndef_write_text(ao_spy(), "x", 0U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kode_sys_nfc_ndef_write_text(ao_spy(), long_text, WIN_MS),
                                  "a message that does not fit is refused, never cut");
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_nfc_emulate_start(NULL, &card));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_nfc_emulate_start(ao_spy(), NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_nfc_emulate_stop(NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_cert_nfc_field_read(NULL, WIN_MS));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_cert_nfc_field_read(ao_spy(), 0U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_cert_nfc_field_read(ao_spy(), KODE_CERT_NFC_FIELD_MS_MAX + 1U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_cert_nfc_carrier(NULL, KODE_CERT_NFC_CARRIER_ON));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_cert_nfc_carrier(ao_spy(), 3));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_cert_nfc_carrier(ao_spy(), -1));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, (uint32_t)uxQueueMessagesWaiting(AO_Nfc->eQueue),
                                   "a refused request posted anyway");

    kdrv_st25r3916_discover_start_ExpectAndReturn(KODE_OK);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_nfc_read_tag(ao_spy(), WIN_MS), "a refusal takes no door");
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    tag_window_closes_empty();
    (void)ask_take(NFC_READ_RSP_SIG);
    ask_down();
}

TEST_CASE("a full AO_Nfc queue refuses kode_sys's requests with no resources",
          "[nfc_ao][ask]")
{
    kode_sys_nfc_card_t card;
    const QEvt         *e;

    ask_up();
    (void)memset(&card, 0, sizeof(card));
    TEST_ASSERT_TRUE_MESSAGE(qf_support_queue_stuff(AO_Nfc, &s_nfc_filler), "the queue never filled");
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_nfc_read_tag(ao_spy(), WIN_MS));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_nfc_ndef_read(ao_spy(), WIN_MS));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_nfc_emulate_start(ao_spy(), &card));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_cert_nfc_field_read(ao_spy(), WIN_MS));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_cert_nfc_carrier(ao_spy(), KODE_CERT_NFC_CARRIER_ON));
    while ( xQueueReceive(AO_Nfc->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }

    kdrv_st25r3916_discover_start_ExpectAndReturn(KODE_OK);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_nfc_read_tag(ao_spy(), WIN_MS),
                                  "nothing was posted, so the door is open");
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    tag_window_closes_empty();
    (void)ask_take(NFC_READ_RSP_SIG);
    ask_down();
}

TEST_CASE("a dry pool refuses kode_sys's NFC request with no resources and posts nothing",
          "[nfc_ao][ask]")
{
    static NfcReadRspEvt *held[DRY_MAX];
    uint32_t              n = 0U;

    ask_up();
    while ( DRY_MAX > n )
    {
        held[n] = Q_NEW_X(NfcReadRspEvt, 0U, NFC_READ_RSP_SIG);
        if ( NULL == held[n] )
        {
            break;
        }
        n++;
    }
    TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(DRY_MAX, n, "the pool never ran dry");
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_nfc_read_tag(ao_spy(), WIN_MS));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Nfc->eQueue));
    for ( uint32_t i = 0U; i < n; i++ )
    {
        QF_gc(&held[i]->super);
    }

    kdrv_st25r3916_discover_start_ExpectAndReturn(KODE_OK);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_nfc_read_tag(ao_spy(), WIN_MS),
                                  "nothing was posted, so the door is open");
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    tag_window_closes_empty();
    (void)ask_take(NFC_READ_RSP_SIG);
    ask_down();
}

TEST_CASE("an NFC ask whose request finds its pool dry gives the answer back and posts nothing",
          "[nfc_ao][ask]")
{
    static qf_support_dry_t dry;
    uint16_t                before;

    ask_up();
    before = qf_support_pool_dry_second(&dry, (uint16_t)sizeof(NfcReadEvt), (uint16_t)sizeof(NfcReadRspEvt));
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0U, before, "the pools could not be set up");
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_nfc_read_tag(ao_spy(), WIN_MS));
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(before, qf_support_pool_free(&dry), "the answer went back to its pool");
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Nfc->eQueue));
    qf_support_pool_give_back(&dry);

    kdrv_st25r3916_discover_start_ExpectAndReturn(KODE_OK);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_nfc_read_tag(ao_spy(), WIN_MS),
                                  "nothing was posted, so the door is open");
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Nfc, 4U));
    tag_window_closes_empty();
    (void)ask_take(NFC_READ_RSP_SIG);
    ask_down();
}

/** @brief The body of the full-queue case, run in a child: the asker's queue is full, then AO_Nfc answers into it. */
static void full_asker_body(void *arg)
{
    (void)arg;
    (void)qf_support_queue_stuff(ao_spy_other(), &s_nfc_filler);
    (void)kode_sys_nfc_emulate_stop(ao_spy_other());   /* nothing plays: refused in the step, answered */
    (void)qf_support_settle(AO_Nfc);
}

TEST_CASE("an answer into an asker's full queue is QP's assert, not a drop",
          "[nfc_ao][ask]")
{
    ask_up();

    /* QP asserts with Q_onError(), which aborts this binary, so the post runs
     * in a child and the parent reads how it died. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)ASK_CHILD_ABORTED,
                                    (uint32_t)ask_support_fork_run(&full_asker_body, NULL),
                                  "the child was not stopped by QP's assert");
    ask_down();
}

TEST_CASE("an app AO's stop of a card the console opened is refused, and the console's ends it",
          "[nfc_ao][ask]")
{
    knfc_emu_stat_t playing = { .active = true, .field = false, .selected = false,
                                .sessions = 0U, .reads = 1U };
    knfc_emu_stat_t gone    = { .active = false, .field = false, .selected = false,
                                .sessions = 0U, .reads = 1U };
    const QEvt     *o;

    ask_up();
    emu_start_link();   /* the console's: no asking AO opened it */
    ao_spy_release();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nfc_emulate_stop(ao_spy_other()));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_settle(AO_Nfc));
    o = ao_spy_other_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(o, "the app's stop is answered");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)NFC_EMU_RSP_SIG, (uint32_t)o->sig);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED,
                           ((const kode_sys_nfc_emu_answer_t *)(const void *)o)->rc);
    QF_gc(o);
    TEST_ASSERT_TRUE_MESSAGE(kode_sys_nfc_is_emulating(), "an app ended the console's card");

    kdrv_st25r3916_emulate_snapshot_ExpectAnyArgs();
    kdrv_st25r3916_emulate_snapshot_ReturnThruPtr_out(&playing);
    kdrv_st25r3916_emulate_stop_ExpectAndReturn(KODE_OK);
    kdrv_st25r3916_emulate_snapshot_ExpectAnyArgs();
    kdrv_st25r3916_emulate_snapshot_ReturnThruPtr_out(&gone);
    nfc_emulate_stop_request(NULL, NULL, NULL, 0U, NULL, ao_spy());
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_tick(AO_Nfc, 1U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, take_emu_rsp()->rc);
    TEST_ASSERT_FALSE(kode_sys_nfc_is_emulating());
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Nfc, 30U));
    ask_down();
}

/* ---- The board's rest ------------------------------------------------------- */

/* The field down and the oscillator asleep is what field_off() is, and it is
 * the driver's one call for it: the mock sees exactly one per round. */
TEST_CASE("a round takes the reader down to rest and answers once", "[nfc_ao][lowpower]")
{
    nfc_up();

    kdrv_st25r3916_field_off_Expect();
    lp_round(AO_Nfc, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();

    kdrv_st25r3916_field_off_Expect();
    lp_round(AO_Nfc, KODE_LP_OFF);
    lp_expect_answer(false, NULL);
    Mockkdrv_st25r3916_Verify();
}

/* Nothing is restored on AWAKE: the next request wakes the reader itself. */
TEST_CASE("after the rest AWAKE does nothing and the next window opens", "[nfc_ao][lowpower]")
{
    knfc_field_t stats = { .min = 1U, .mean = 1U, .peak = 1U, .mod = 0U,
                           .ext = false, .n = 16U };

    nfc_up();
    kdrv_st25r3916_field_off_Expect();
    lp_round(AO_Nfc, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);

    lp_awake(AO_Nfc);               /* no driver call expected */

    open_window(5U, NULL);
    kdrv_st25r3916_listen_end_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_st25r3916_listen_end_ReturnThruPtr_out(&stats);
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_tick(AO_Nfc, 1U));
    Mockkdrv_st25r3916_Verify();
}

/* A window is somebody's measurement; a tag in the field somebody's read. */
TEST_CASE("a request in flight refuses the round and is not cut", "[nfc_ao][lowpower]")
{
    knfc_field_t stats = { .min = 1U, .mean = 1U, .peak = 1U, .mod = 0U,
                           .ext = false, .n = 16U };

    nfc_up();
    open_window(5U, NULL);

    lp_round(AO_Nfc, KODE_LP_ASLEEP);       /* no field_off expected */
    lp_expect_answer(true, "nfc busy");
    lp_expect_no_more();

    kdrv_st25r3916_listen_end_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_st25r3916_listen_end_ReturnThruPtr_out(&stats);
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_tick(AO_Nfc, 1U));

    /* Done: the same round is now answered ready. */
    kdrv_st25r3916_field_off_Expect();
    lp_round(AO_Nfc, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    Mockkdrv_st25r3916_Verify();
}

TEST_CASE("no reader: the round is answered ready and the driver is left alone",
          "[nfc_ao][lowpower]")
{
    nfc_up();
    nfc_restart(false);

    lp_round(AO_Nfc, KODE_LP_ASLEEP);       /* no field_off expected */
    lp_expect_answer(false, NULL);
    lp_expect_no_more();
    Mockkdrv_st25r3916_Verify();
}
