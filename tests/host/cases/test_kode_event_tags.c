/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_event_tags.c
 * @brief The ir_received and nfc_tag events, from a heard frame or an activated tag.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "Mockkdrv_ir.h"

#include "fakes.h"
#include "fake_khal_worker.h"
#include "kaos.h"
#include "kode.h"
#include "kode_entry.h"
#include "kode_standard_ao.h"
#include "kode_sys_priv.h"
#include "knec.h"
#include "knfc_tag.h"
#include "kqp.h"
#include "qf_support.h"
#include "standard_ao_support.h"

/** @brief The longest a case waits for a listener, in ms. */
#define TEST_WAIT_MS 4000U

/** @brief Frames published to the held dispatch task. */
#define FLOOD_FRAMES 40U

/** @brief Waiting events in the Active Object's queue above which the case lets it run. */
#define PACE_HIGH 8U

/** @brief Longest a pacing loop waits, in ticks of 1. */
#define PACE_SPINS 400U

/** @brief The NEC address the cases publish. */
#define TEST_IR_ADDR 0x0004U

/** @brief The NEC command the cases publish. */
#define TEST_IR_CMD 0x0008U

/** @brief The ATQA of the tag the cases present. */
#define TEST_ATQA 0x0004U

/** @brief The SAK of the tag the cases present. */
#define TEST_SAK 0x08U

/** @brief Bytes of the UID the cases present. */
#define TEST_UID_LEN 4U

/** @brief Longest string a record keeps. */
#define REC_TEXT 64U

/** @brief What a listener saw. */
typedef struct {
    char    name[REC_TEXT];    /**< The event's name.   */
    char    source[REC_TEXT];  /**< Its source.         */
    char    text[REC_TEXT];    /**< Its text.           */
    int32_t value;             /**< Its value.          */
} rec_t;

static rec_t             s_rec;          /**< The last event a listener saw.        */
static SemaphoreHandle_t s_given;        /**< Given by a listener per event.        */
static SemaphoreHandle_t s_gate_in;      /**< Given when a gate listener starts.    */
static SemaphoreHandle_t s_gate_open;    /**< Each give lets one gate listener go.  */
static const uint8_t     s_uid[TEST_UID_LEN] = { 0x04U, 0xA2U, 0x3BU, 0x5CU };  /**< The tag's UID. */

/** @brief Record the event and tell the case. */
static void listener_rec(const kode_event_t *e)
{
    (void)snprintf(s_rec.name, REC_TEXT, "%s", e->name);
    (void)snprintf(s_rec.source, REC_TEXT, "%s", e->source);
    (void)snprintf(s_rec.text, REC_TEXT, "%s", e->text);
    s_rec.value = e->value;
    (void)xSemaphoreGive(s_given);
}

/** @brief Hold the dispatch task until the case lets it go. */
static void listener_gate(const kode_event_t *e)
{
    (void)e;
    (void)xSemaphoreGive(s_gate_in);
    (void)xSemaphoreTake(s_gate_open, pdMS_TO_TICKS(TEST_WAIT_MS));
}

/** @brief Publish a frame the way AO_Ir does. */
static void publish_ir(uint16_t addr, uint16_t cmd, bool nec)
{
    IrRxEvt *const e = Q_NEW(IrRxEvt, IR_RX_SIG);

    e->addr    = addr;
    e->cmd     = cmd;
    e->symbols = 0U;
    e->nec     = nec;
    QACTIVE_PUBLISH(&e->super, (void *)0);
}

/**
 * @brief Publish a tag the way AO_Nfc does.
 * @param uid     The UID bytes, @p have of them; the rest of the field is zero.
 * @param have    How many bytes @p uid holds, at most KQP_NFC_UID_MAX.
 * @param uid_len The length the event claims.
 */
static void publish_tag_with(const uint8_t *uid, uint32_t have, uint8_t uid_len)
{
    NfcTagEvt *const e = Q_NEW(NfcTagEvt, NFC_TAG_SIG);

    (void)memset(e->uid, 0, sizeof(e->uid));
    (void)memcpy(e->uid, uid, have);
    e->uid_len = uid_len;
    e->atqa    = (uint16_t)TEST_ATQA;
    e->sak     = (uint8_t)TEST_SAK;
    e->type    = 0U;
    QACTIVE_PUBLISH(&e->super, (void *)0);
}

/** @brief The QP registry of Active Objects, indexed by priority. */
extern QActive *QActive_registry_[QF_MAX_ACTIVE + 1U];

/** @brief The QP subscriber sets, one per published signal. */
extern QSubscrList *QActive_subscrList_;

/** @brief The published signals the Active Object hears from this file's point of view. */
static const enum_t s_sigs[] = { IR_RX_SIG, NFC_TAG_SIG };

/** @brief Signals in s_sigs. */
#define SIGS_N (sizeof(s_sigs) / sizeof(s_sigs[0]))

/** @brief Whether the Active Object at a priority was subscribed to a signal of s_sigs before the file took it off. */
static bool s_was[SIGS_N][QF_MAX_ACTIVE + 1U];

/** @brief Whether the other Active Objects were taken off the bus already. */
static bool s_others_off;

/**
 * @brief Take every other Active Object off the two signals, once.
 *
 * Other files' spies stay subscribed with nobody to take their events out:
 * every frame this file publishes would stay allocated in their queues until
 * the event pool is empty, and QP asserts a publish that finds no block. Their
 * queues are left as they are; restore_others() puts the subscriptions back.
 */
static void others_off_the_bus(void)
{
    if ( !s_others_off )
    {
        s_others_off = true;
        for ( uint32_t i = 0U; SIGS_N > i; i++ )
        {
            for ( uint32_t p = 1U; QF_MAX_ACTIVE >= p; p++ )
            {
                QActive *const ao = QActive_registry_[p];

                if ( (NULL != ao) && (ao != AO_KodeStandard) &&
                    QPSet_hasElement(&QActive_subscrList_[s_sigs[i]].set, (uint_fast8_t)p) )
                {
                    s_was[i][p] = true;
                    QActive_unsubscribe(ao, s_sigs[i]);
                }
            }
        }
    }
}

/** @brief Subscribe again the Active Objects others_off_the_bus() took off. */
static void restore_others(void)
{
    for ( uint32_t i = 0U; SIGS_N > i; i++ )
    {
        for ( uint32_t p = 1U; QF_MAX_ACTIVE >= p; p++ )
        {
            if ( s_was[i][p] )
            {
                QActive_subscribe(QActive_registry_[p], s_sigs[i]);
                s_was[i][p] = false;
            }
        }
    }
    s_others_off = false;
}

/** @brief Publish the 4-byte tag the cases present. */
static void publish_tag(void)
{
    publish_tag_with(s_uid, TEST_UID_LEN, (uint8_t)TEST_UID_LEN);
}

/** @brief Give AO_Ir a mailbox, emptied: an ir_received listener posts to it. */
static void ir_mailbox_empty(void)
{
    const QEvt *e = NULL;

    qf_support_give_queue(AO_Ir);
    while ( xQueueReceive(AO_Ir->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief Start the layer with real locks and reset what the cases share. */
static void events_up(void)
{
    qf_support_init();
    ir_mailbox_empty();
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    fake_sync_real(true);
    others_off_the_bus();
    TEST_ASSERT_TRUE(kode_standard_ao_ensure());
    if ( NULL == s_given )
    {
        s_given     = xSemaphoreCreateCounting(FLOOD_FRAMES, 0U);
        s_gate_in   = xSemaphoreCreateCounting(FLOOD_FRAMES, 0U);
        s_gate_open = xSemaphoreCreateCounting(FLOOD_FRAMES, 0U);
    }
    while ( xSemaphoreTake(s_given, 0U) == pdTRUE )
    {
    }
    while ( xSemaphoreTake(s_gate_in, 0U) == pdTRUE )
    {
    }
    while ( xSemaphoreTake(s_gate_open, 0U) == pdTRUE )
    {
    }
    (void)memset(&s_rec, 0, sizeof(s_rec));
    s_rec.value = -1;
}

/** @brief Wait for one listener call. */
static void given_wait(void)
{
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_given, pdMS_TO_TICKS(TEST_WAIT_MS)));
}

TEST_CASE("a NEC frame becomes ir_received with the command and the address", "[kode_event_tags]")
{
    events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener("ir_received", &listener_rec));

    publish_ir(TEST_IR_ADDR, TEST_IR_CMD, true);
    given_wait();

    TEST_ASSERT_EQUAL_STRING("ir_received", s_rec.name);
    TEST_ASSERT_EQUAL_STRING("NEC", s_rec.source);
    TEST_ASSERT_EQUAL_INT32((int32_t)TEST_IR_CMD, s_rec.value);
    TEST_ASSERT_EQUAL_STRING("0x0004", s_rec.text);
    TEST_ASSERT_TRUE(kode_event_remove_listener("ir_received", &listener_rec));
}

TEST_CASE("the linking names are the events' names, and a listener added by one hears its event",
          "[kode_event_tags]")
{
    TEST_ASSERT_EQUAL_STRING("ir_received", KODE_EVENT_IR_RECEIVED);
    TEST_ASSERT_EQUAL_STRING("wifi_connected", KODE_EVENT_WIFI_CONNECTED);
    TEST_ASSERT_EQUAL_STRING("wifi_lost", KODE_EVENT_WIFI_LOST);

    events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener(KODE_EVENT_IR_RECEIVED, &listener_rec));
    publish_ir(TEST_IR_ADDR, TEST_IR_CMD, true);
    given_wait();
    TEST_ASSERT_EQUAL_STRING("ir_received", s_rec.name);
    TEST_ASSERT_TRUE(kode_event_remove_listener("ir_received", &listener_rec));
}

TEST_CASE("an ir_received listener has AO_Ir arm its receiver, with a capture nobody waits for",
          "[kode_event_tags]")
{
    const QEvt *e = NULL;

    events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener(KODE_EVENT_IR_RECEIVED, &listener_rec));
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)uxQueueMessagesWaiting(AO_Ir->eQueue));
    TEST_ASSERT_TRUE(xQueueReceive(AO_Ir->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_INT((int)IR_CAPTURE_REQ_SIG, (int)e->sig);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, ((const IrCaptureEvt *)(const void *)e)->ms,
                                     "no deadline: AO_Ir arms and answers nobody");
    TEST_ASSERT_NULL(((const IrCaptureEvt *)(const void *)e)->done);
    QF_gc(e);
    TEST_ASSERT_TRUE(kode_event_remove_listener("ir_received", &listener_rec));

    TEST_ASSERT_TRUE(kode_event_add_listener("nfc_tag", &listener_rec));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, (uint32_t)uxQueueMessagesWaiting(AO_Ir->eQueue),
                                     "another event asks nothing of AO_Ir");
    TEST_ASSERT_TRUE(kode_event_remove_listener("nfc_tag", &listener_rec));
}

/* ---- A listener and the C5 link -------------------------------------------
 * The real AO_Ir, stepped by the case: the listener's arm finds the link down,
 * waits for it, and the frame heard after the link is up reaches the listener
 * through the real Active Object and dispatch task. AO_Radio is a mailbox, and
 * its state of the link is posted to AO_Ir by hand.
 */

/** @brief The infrared worker's task name, as `ps` prints it. */
#define IR_WORKER "ir_worker"

/** @brief Steps of 10 ms past AO_Ir's wait for the link. */
#define LINK_WAIT_STEPS (QF_SUPPORT_STEPS(KODE_IR_LINK_WAIT_MS) + 1U)

/** @brief Make sure AO_Ir is constructed and dispatching, and give it its own queue, emptied. */
static void ir_real_up(void)
{
    Mockkdrv_ir_Init();
    qf_support_init();
    fake_radio_set_link(true);
    qf_support_margin_queue(AO_Ir);
    if ( qf_support_claim_ao(AO_Ir) )
    {
        kdrv_ir_init_IgnoreAndReturn(KODE_OK);
        Ir_ctor();
        qf_support_give_queue(AO_Ir);
        AO_Ir->prio  = KODE_PRIO_IR;
        AO_Ir->pthre = KODE_PRIO_IR;
        QActive_register_(AO_Ir);
        QASM_INIT(AO_Ir, (void *)0, 0U);
        kdrv_ir_rx_arm_IgnoreAndReturn(KODE_OK);
        (void)fake_khal_worker_run_next_for(IR_WORKER);
        (void)qf_support_settle(AO_Ir);
        Mockkdrv_ir_Init();
    }
    while ( fake_khal_worker_pending_for(IR_WORKER) > 0U )
    {
        (void)fake_khal_worker_run_next_for(IR_WORKER);
    }
    qf_support_margin_queue(AO_Ir);
    qf_support_margin_queue(AO_Radio);
}

/** @brief How many link requests reached AO_Radio's mailbox; empties it. */
static unsigned radio_ups(void)
{
    const QEvt *e = NULL;
    unsigned    n = 0U;

    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)RADIO_UP_REQ_SIG == e->sig )
        {
            n++;
        }
        QF_gc(e);
    }
    return n;
}

/** @brief AO_Radio's state of the link, as AO_Ir hears it. */
static void link_says(kradio_link_t link)
{
    RadioStateEvt *const e = Q_NEW(RadioStateEvt, RADIO_STATE_SIG);

    e->link      = (uint8_t)link;
    e->fw_known  = false;
    e->fw_major  = 0U;
    e->fw_minor  = 0U;
    e->fw_patch  = 0U;
    e->sd_hosted = false;
    QACTIVE_POST(AO_Ir, &e->super, NULL);
    (void)qf_support_settle(AO_Ir);
}

/** @brief Run every job AO_Ir gave its worker, and the ones their answers submit. */
static unsigned ir_jobs_run(void)
{
    unsigned n = 0U;

    while ( (fake_khal_worker_pending_for(IR_WORKER) > 0U) && (8U > n) )
    {
        TEST_ASSERT_TRUE(fake_khal_worker_run_next_for(IR_WORKER));
        (void)qf_support_settle(AO_Ir);
        n++;
    }
    return n;
}

/** @brief Whether AO_Ir hears AO_Radio's state of the link. */
static bool ir_hears_the_link(void)
{
    return QPSet_hasElement(&QActive_subscrList_[RADIO_STATE_SIG].set, (uint_fast8_t)AO_Ir->prio);
}

/** @brief The link comes up: the receiver is armed, and the listener's arm served. */
static void link_comes_up(void)
{
    fake_radio_set_link(true);
    kdrv_ir_rx_arm_IgnoreAndReturn(KODE_OK);
    link_says(KRADIO_UP);
    TEST_ASSERT_TRUE_MESSAGE(ir_jobs_run() > 0U, "the link's coming up arms the receiver");
    TEST_ASSERT_FALSE_MESSAGE(ir_hears_the_link(), "nothing more is owed to a link-up");
}

/** @brief AO_Ir hears one NEC frame through its own receive path, and the listener is given it. */
static void ir_hears_and_listener_gets(void)
{
    static const QEvt rdy = QEVT_INITIALIZER(IR_RX_RDY_SIG);
    kdrv_ir_symbol_t  sym[KNEC_SYMBOLS];
    size_t            n = 0U;

    TEST_ASSERT_EQUAL_INT(KODE_OK, knec_encode(TEST_IR_ADDR, TEST_IR_CMD, sym, KNEC_SYMBOLS, &n));
    kdrv_ir_rx_take_ExpectAnyArgsAndReturn(n);
    kdrv_ir_rx_take_ReturnMemThruPtr_out((void *)sym, (int)(n * sizeof(kdrv_ir_symbol_t)));
    QACTIVE_POST(AO_Ir, &rdy, NULL);
    (void)qf_support_settle(AO_Ir);
    (void)ir_jobs_run();
    given_wait();
    TEST_ASSERT_EQUAL_STRING("ir_received", s_rec.name);
    TEST_ASSERT_EQUAL_STRING("NEC", s_rec.source);
    TEST_ASSERT_EQUAL_INT32((int32_t)TEST_IR_CMD, s_rec.value);
}

TEST_CASE("a listener added with the C5 link down has the receiver armed once it is up, "
          "and hears the next frame", "[kode_event_tags][ir_link]")
{
    events_up();
    ir_real_up();
    fake_radio_set_link(false);

    TEST_ASSERT_TRUE(kode_event_add_listener(KODE_EVENT_IR_RECEIVED, &listener_rec));
    (void)qf_support_settle(AO_Ir);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, radio_ups(), "the listener's arm asks AO_Radio for the link");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_khal_worker_pending_for(IR_WORKER),
                                   "nothing goes to the C5 with no link to carry it");

    link_comes_up();
    ir_hears_and_listener_gets();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, radio_ups(), "nothing asked again");

    TEST_ASSERT_TRUE(kode_event_remove_listener("ir_received", &listener_rec));
    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

TEST_CASE("a listener's arm with the link down is owed to the next link-up, "
          "with no wait running", "[kode_event_tags][ir_link]")
{
    events_up();
    ir_real_up();
    fake_radio_set_link(false);

    TEST_ASSERT_TRUE(kode_event_add_listener(KODE_EVENT_IR_RECEIVED, &listener_rec));
    (void)qf_support_settle(AO_Ir);
    TEST_ASSERT_EQUAL_UINT(1U, radio_ups());

    for ( unsigned step = 0U; LINK_WAIT_STEPS > step; step++ )
    {
        fake_time_advance_ms(10U);
        (void)qf_support_tick(AO_Ir, 1U);
    }
    TEST_ASSERT_TRUE_MESSAGE(ir_hears_the_link(), "the arm is owed: AO_Ir still hears the link");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Ir, 50U),
                                   "and no check runs while it is owed: nothing loops");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, radio_ups(), "nor asks again");

    link_says(KRADIO_STARTING);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_khal_worker_pending_for(IR_WORKER), "on its way is not up");
    link_comes_up();
    ir_hears_and_listener_gets();

    TEST_ASSERT_TRUE(kode_event_remove_listener("ir_received", &listener_rec));
    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

TEST_CASE("a frame that is not NEC becomes ir_received from unknown", "[kode_event_tags]")
{
    events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener("IR_Received", &listener_rec));

    publish_ir(TEST_IR_ADDR, TEST_IR_CMD, false);
    given_wait();

    TEST_ASSERT_EQUAL_STRING("ir_received", s_rec.name);
    TEST_ASSERT_EQUAL_STRING("unknown", s_rec.source);
    TEST_ASSERT_EQUAL_INT32(0, s_rec.value);
    TEST_ASSERT_EQUAL_STRING("", s_rec.text);
    TEST_ASSERT_TRUE(kode_event_remove_listener("ir_received", &listener_rec));
}

TEST_CASE("a tag becomes nfc_tag with its UID and its model", "[kode_event_tags]")
{
    events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener("nfc_tag", &listener_rec));

    publish_tag();
    given_wait();

    TEST_ASSERT_EQUAL_STRING("nfc_tag", s_rec.name);
    TEST_ASSERT_EQUAL_STRING(knfc_tag_model((uint16_t)TEST_ATQA, (uint8_t)TEST_SAK), s_rec.source);
    TEST_ASSERT_TRUE(strlen(s_rec.source) > 0U);
    TEST_ASSERT_EQUAL_INT32(0, s_rec.value);
    TEST_ASSERT_EQUAL_STRING("04:A2:3B:5C", s_rec.text);
    TEST_ASSERT_TRUE(kode_event_remove_listener("nfc_tag", &listener_rec));
}

/** @brief Bytes of the longest UID. */
#define LONG_UID_LEN 10U

/** @brief Characters of the longest UID as text: two digits a byte and a colon between. */
#define LONG_UID_CHARS 29U

/** @brief A uid_len no tag has, to be clamped. */
#define WILD_UID_LEN 255U

static const uint8_t s_long_uid[LONG_UID_LEN] = {
    0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U, 0x09U, 0x0AU
};  /**< A ten-byte UID. */

TEST_CASE("a ten-byte UID gives the whole 29-character text", "[kode_event_tags]")
{
    events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener("nfc_tag", &listener_rec));

    publish_tag_with(s_long_uid, LONG_UID_LEN, (uint8_t)LONG_UID_LEN);
    given_wait();

    TEST_ASSERT_EQUAL_UINT32(LONG_UID_CHARS, (uint32_t)strlen(s_rec.text));
    TEST_ASSERT_EQUAL_STRING("01:02:03:04:05:06:07:08:09:0A", s_rec.text);
    TEST_ASSERT_TRUE(kode_event_remove_listener("nfc_tag", &listener_rec));
}

TEST_CASE("a uid_len past the field is clamped to the field", "[kode_event_tags]")
{
    events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener("nfc_tag", &listener_rec));

    publish_tag_with(s_long_uid, LONG_UID_LEN, (uint8_t)WILD_UID_LEN);
    given_wait();

    TEST_ASSERT_EQUAL_UINT32(KQP_NFC_UID_MAX, LONG_UID_LEN);
    TEST_ASSERT_EQUAL_STRING("01:02:03:04:05:06:07:08:09:0A", s_rec.text);
    TEST_ASSERT_TRUE(kode_event_remove_listener("nfc_tag", &listener_rec));
}

/** @brief What a second task does while this one sleeps: present the tag. */
static void present_tag_while_waiting(void)
{
    publish_tag();
}

TEST_CASE("waiting for NFC_TAG returns the tag", "[kode_event_tags]")
{
    kode_event_t out = { NULL, NULL, -1, NULL, -1, -1 };

    events_up();
    fake_sem_wait_hook(&present_tag_while_waiting);
    const bool got = kode_event_wait("NFC_TAG", TEST_WAIT_MS, &out);
    fake_sem_wait_hook(NULL);

    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL_STRING("nfc_tag", out.name);
    TEST_ASSERT_EQUAL_STRING("04:A2:3B:5C", out.text);
    TEST_ASSERT_EQUAL_INT32(0, out.value);
}

TEST_CASE("forty frames at a held dispatch task drop without an assert", "[kode_event_tags]")
{
    events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener("ir_received", &listener_gate));

    /* The first frame puts the dispatch task inside the gate: nothing leaves the ring. */
    publish_ir(TEST_IR_ADDR, TEST_IR_CMD, true);
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_gate_in, pdMS_TO_TICKS(TEST_WAIT_MS)));

    const uint32_t dropped0 = kode_event_dropped();

    /* Paced on purpose: this case proves the ring keeps KODE_EVENT_QUEUE_DEPTH
     * and counts the rest dropped. The AO's own queue is protected on
     * the board by its non-blocking step and the publisher's rate, not here. */
    for ( uint32_t i = 1U; FLOOD_FRAMES > i; i++ )
    {
        publish_ir(TEST_IR_ADDR, (uint16_t)i, true);
        for ( uint32_t spin = 0U; (PACE_SPINS > spin) &&
             (uxQueueMessagesWaiting(AO_KodeStandard->eQueue) >= PACE_HIGH); spin++ )
        {
            vTaskDelay(1U);
        }
    }
    standard_ao_support_drained();
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_KodeStandard->eQueue));

    /* The ring holds 16 with the entry under delivery; the rest are counted.
     * The Active Object may still be on its last frame: wait for the count. */
    for ( uint32_t spin = 0U; (PACE_SPINS > spin) &&
         ((kode_event_dropped() - dropped0) < (FLOOD_FRAMES - KODE_EVENT_QUEUE_DEPTH)); spin++ )
    {
        vTaskDelay(1U);
    }
    TEST_ASSERT_EQUAL_UINT32(FLOOD_FRAMES - KODE_EVENT_QUEUE_DEPTH,
                             kode_event_dropped() - dropped0);

    /* Let everything go: the 15 kept after the first each pass the gate. Then
     * a barrier queued behind them, so no frame is in flight when the case ends. */
    TEST_ASSERT_TRUE(kode_event_add_listener("button_up_released", &listener_rec));
    for ( uint32_t i = 0U; KODE_EVENT_QUEUE_DEPTH > i; i++ )
    {
        (void)xSemaphoreGive(s_gate_open);
    }
    for ( uint32_t i = 1U; KODE_EVENT_QUEUE_DEPTH > i; i++ )
    {
        TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_gate_in, pdMS_TO_TICKS(TEST_WAIT_MS)));
    }
    kode_event_push("button_up_released", "", 0, "");
    given_wait();
    TEST_ASSERT_EQUAL_STRING("button_up_released", s_rec.name);
    TEST_ASSERT_TRUE(kode_event_remove_listener("button_up_released", &listener_rec));
    TEST_ASSERT_TRUE(kode_event_remove_listener("ir_received", &listener_gate));
}

/* Last, on purpose: the Active Object stays on the bus for the rest of the
 * binary and the cases of other files publish these signals on benches of
 * their own. It leaves the two signals, and the Active Objects this file took
 * off them go back. */
TEST_CASE("the layer leaves the infrared and tag signals for the cases that follow", "[kode_event_tags]")
{
    events_up();
    QActive_unsubscribe(AO_KodeStandard, IR_RX_SIG);
    QActive_unsubscribe(AO_KodeStandard, NFC_TAG_SIG);

    /* A frame and a tag now reach nobody: only the barrier behind them is heard. */
    const bool added = kode_event_add_listener("ir_received", &listener_rec) &&
                       kode_event_add_listener("nfc_tag", &listener_rec) &&
                       kode_event_add_listener("button_up_released", &listener_rec);

    publish_ir(TEST_IR_ADDR, TEST_IR_CMD, true);
    publish_tag();
    kode_event_push("button_up_released", "", 0, "");

    const bool barrier = (xSemaphoreTake(s_given, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    const bool named   = (strcmp(s_rec.name, "button_up_released") == 0);
    const bool nothing = (xSemaphoreTake(s_given, 0U) == pdFALSE);
    const bool removed = kode_event_remove_listener("ir_received", &listener_rec) &&
                         kode_event_remove_listener("nfc_tag", &listener_rec) &&
                         kode_event_remove_listener("button_up_released", &listener_rec);

    /* Before any assert: a failure here must not leave the other Active Objects off the bus. */
    restore_others();
    TEST_ASSERT_TRUE(added);
    TEST_ASSERT_TRUE(barrier);
    TEST_ASSERT_TRUE(named);
    TEST_ASSERT_TRUE(nothing);
    TEST_ASSERT_TRUE(removed);
}
