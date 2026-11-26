/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ir.c
 * @brief The infrared block: kdrv_ir.h, spoken to the ESP32-C5.
 */
#include "kdrv_ir.h"

#include <string.h>

#include "sdkconfig.h"
#include "kbsp.h"
#include "kode_log.h"

#define TAG "kdrv.ir"

#if CONFIG_KODEOS_RADIO

#include "kdrv_radio.h"
#include "khal_sync.h"
#include "kir_wire.h"

_Static_assert(KDRV_IR_TX_MAX_SYMBOLS <= KIR_MAX_SYMBOLS,
               "a transmission fits one request");
_Static_assert(KDRV_IR_RX_MAX_SYMBOLS <= KIR_MAX_SYMBOLS,
               "a capture fits this driver's buffer");
_Static_assert(sizeof(kdrv_ir_symbol_t) == sizeof(kir_symbol_t),
               "the driver's symbol and the wire's are the same four bytes");
_Static_assert(KDRV_IR_DURATION_MAX_US <= UINT16_MAX, "a duration fits the wire's field");
_Static_assert(KDRV_IR_FRAME_MAX_MS < KDRV_IR_C5_TX_DONE_MS,
               "a frame ends before the C5 stops waiting for it");

/** @brief Microseconds in a millisecond. */
#define IR_US_PER_MS  1000U

static kdrv_ir_rx_cb_t s_rx_cb;
static void           *s_rx_ctx;
static bool            s_init;

/* The last capture, as the C5 sent it. Written on esp-hosted's thread and
 * read on AO_Ir's; s_rx_ready is the hand-over, set last and cleared first. */
static kdrv_ir_symbol_t s_rx[KDRV_IR_RX_MAX_SYMBOLS];
static volatile size_t  s_rx_len;
static volatile bool    s_rx_ready;
static volatile bool    s_armed;

/* The C5's word on the transmission in flight: KIR_EVT_SENT's status. */
static khal_sem_t       s_sent;
static volatile uint8_t s_sent_status;

/* The one line reading in flight, and the C5's answer to it. */
static khal_sem_t       s_reply;
static volatile uint8_t s_reply_op;
static volatile uint8_t s_reply_status;
static volatile bool    s_reply_high;

/** @brief The request as it goes out: header, then the symbols. */
static uint8_t s_req[sizeof(kir_send_t)
                     + (KDRV_IR_TX_MAX_SYMBOLS * sizeof(kir_symbol_t))];

/**
 * @brief An event from the C5. esp-hosted's thread: copy, signal, return.
 *
 * @param data The event.
 * @param len  Its length.
 * @param ctx  Unused.
 */
static void ir_on_event(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    if ( (NULL == data) || (2U > len) || ((uint8_t)KIR_WIRE_VERSION != data[1]) )
    {
        return;
    }

    if ( ((uint8_t)KIR_EVT_CAPTURE == data[0]) && (sizeof(kir_capture_t) <= len) )
    {
        kir_capture_t hdr;
        size_t        n;

        (void)memcpy(&hdr, data, sizeof(hdr));
        n = (size_t)hdr.n;
        if ( (size_t)KDRV_IR_RX_MAX_SYMBOLS < n )
        {
            n = (size_t)KDRV_IR_RX_MAX_SYMBOLS;
        }
        if ( (sizeof(hdr) + (n * sizeof(kir_symbol_t))) > len )
        {
            return;   /* shorter than it says it is: not a capture */
        }
        s_rx_ready = false;
        (void)memcpy(s_rx, &data[sizeof(hdr)], n * sizeof(kir_symbol_t));
        s_rx_len   = n;
        s_armed    = false;   /* one capture per arming, there as here */
        s_rx_ready = true;
        if ( NULL != s_rx_cb )
        {
            s_rx_cb(s_rx_ctx);
        }
    }
    else if ( ((uint8_t)KIR_EVT_SENT == data[0]) && (sizeof(kir_status_t) <= len) )
    {
        s_sent_status = data[2];
        khal_sem_signal(s_sent);
    }
    else if ( ((uint8_t)KIR_EVT_ARMED == data[0]) && (sizeof(kir_status_t) <= len) )
    {
        if ( (uint8_t)KIR_OK != data[2] )
        {
            KLOG_W(TAG, "the C5's receiver would not arm");
            s_armed = false;
        }
    }
    else if ( ((uint8_t)KIR_EVT_LEVEL == data[0]) && (sizeof(kir_level_t) <= len) )
    {
        kir_level_t l;

        (void)memcpy(&l, data, sizeof(l));
        if ( l.op == s_reply_op )
        {
            s_reply_status = l.status;
            s_reply_high   = (0U != l.high);
            if ( (uint8_t)KIR_REQ_PRESENT == l.op )
            {
                /* The probe re-arms the receiver on its way out, and loses
                 * whatever was waiting: the header's own words. */
                s_rx_ready = false;
                s_armed    = ((uint8_t)KIR_OK == l.status);
            }
            khal_sem_signal(s_reply);
        }
    }
    else
    {
        /* an event this driver does not know: a newer slave, and harmless */
    }
}

/**
 * @brief The link just came up, so the C5 has just been reset: its receiver
 *        is not armed, whatever was true before. Without this a capture "in
 *        flight" across a reset would refuse every later arm, and the board
 *        would be deaf for the boot.
 *
 * @param ctx Unused.
 */
static void ir_on_link_up(void *ctx)
{
    (void)ctx;
    s_rx_ready = false;
    s_armed    = false;
}

/**
 * @brief Send one of the requests that carry nothing but themselves.
 *
 * @param op KIR_REQ_ARM, _IDLE or _PRESENT.
 * @return What kdrv_radio_peer_send() returned.
 */
static kode_status_t ir_plain(uint8_t op)
{
    kir_plain_t const req = { .op = op, .version = (uint8_t)KIR_WIRE_VERSION };

    return kdrv_radio_peer_send(KIR_MSG_REQ, (const uint8_t *)&req, sizeof(req));
}

/**
 * @brief Ask the C5 for a line reading and wait for it.
 *
 * @param op        KIR_REQ_IDLE or KIR_REQ_PRESENT.
 * @param[out] high What the line read.
 * @return KODE_OK, KODE_DENIED while the link is down, KODE_ERROR when the C5
 *         said the reading failed, KODE_TIMEOUT when it said nothing.
 */
static kode_status_t ir_reading(uint8_t op, bool *high)
{
    kode_status_t rc;

    /* Nobody is waited for while the semaphore is drained of an answer that
     * arrived after its own wait gave up: an op of zero matches no event, so
     * a late reply to the LAST request cannot be taken for this one's. */
    s_reply_op = 0U;
    (void)khal_sem_wait_timeout(s_reply, 0U);
    s_reply_op = op;

    rc = ir_plain(op);
    if ( KODE_OK != rc )
    {
        if ( KODE_DENIED == rc )
        {
            KLOG_W(TAG, "the C5 link is down; the infrared block is behind it "
                        "(radio_up first)");
        }
        return rc;
    }
    if ( !khal_sem_wait_timeout(s_reply, KDRV_IR_REPLY_MS) )
    {
        KLOG_E(TAG, "the C5 did not answer request %u within %u ms: a slave "
                    "without the infrared service?", (unsigned)op,
               (unsigned)KDRV_IR_REPLY_MS);
        return KODE_TIMEOUT;
    }
    *high = s_reply_high;
    if ( (uint8_t)KIR_OK != s_reply_status )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t kdrv_ir_init(kdrv_ir_rx_cb_t cb, void *ctx)
{
    if ( s_init )
    {
        KLOG_D(TAG, "already listening for the C5's infrared events");
        return KODE_OK;
    }
    s_rx_cb  = cb;
    s_rx_ctx = ctx;

    s_reply = khal_sem_create();
    s_sent  = khal_sem_create();
    if ( (NULL == s_reply) || (NULL == s_sent) )
    {
        return KODE_NO_RESOURCES;
    }
    if ( kdrv_radio_peer_listen(KIR_MSG_EVT, &ir_on_event, &ir_on_link_up,
                               NULL) != KODE_OK )
    {
        KLOG_E(TAG, "no peer-data slot for the infrared events");
        return KODE_NO_RESOURCES;
    }
    s_init = true;

    /* Nothing is armed here: with the link up that is a request over it, and
     * a bring-up that waits on the C5 is a bring-up that can stall the thread
     * starting its owner. The link is normally still down at this point
     * anyway. kdrv_ir_rx_arm() is what a caller that needs the receiver
     * listening calls, and AO_Ir makes it its first job. */
    KLOG_D(TAG, "TX on the C5's IO%d, RX on its IO%d, carrier %u Hz; reached "
                "once the link is up", KBSP_C5_IO_IR_TX, KBSP_C5_IO_IR_RX,
           (unsigned)KDRV_IR_CARRIER_HZ);
    return KODE_OK;
}

kode_status_t kdrv_ir_rx_idle(bool *high)
{
    if ( (NULL == high) || !s_init )
    {
        return KODE_DENIED;
    }
    return ir_reading((uint8_t)KIR_REQ_IDLE, high);
}

kode_status_t kdrv_ir_rx_present(bool *present)
{
    if ( (NULL == present) || !s_init )
    {
        return KODE_DENIED;
    }
    return ir_reading((uint8_t)KIR_REQ_PRESENT, present);
}

/**
 * @brief Whether a frame is one the C5 sends whole: every mark and space, and
 *        the time they add up to.
 *
 * A zero space counts as KDRV_IR_DURATION_MAX_US, as the C5 sends it.
 *
 * @param sym The symbols.
 * @param n   How many, at most KDRV_IR_TX_MAX_SYMBOLS.
 * @return true when none is longer than KDRV_IR_DURATION_MAX_US and the frame
 *         lasts at most KDRV_IR_FRAME_MAX_MS.
 */
static bool ir_frame_fits(const kdrv_ir_symbol_t *sym, size_t n)
{
    uint32_t us  = 0U;
    bool     fit = true;

    for ( size_t i = 0U; fit && (i < n); ++i )
    {
        uint32_t space = (uint32_t)sym[i].space_us;

        if ( 0U == space )
        {
            space = KDRV_IR_DURATION_MAX_US;
        }
        fit = ((uint32_t)KDRV_IR_DURATION_MAX_US >= (uint32_t)sym[i].mark_us)
              && ((uint32_t)KDRV_IR_DURATION_MAX_US >= space);
        us += (uint32_t)sym[i].mark_us + space;
    }
    return fit && ((KDRV_IR_FRAME_MAX_MS * IR_US_PER_MS) >= us);
}

kode_status_t kdrv_ir_send(const kdrv_ir_symbol_t *sym, size_t n,
                           uint32_t carrier_hz)
{
    kir_send_t    hdr;
    kode_status_t rc;

    if ( (NULL == sym) || (0U == n) || ((size_t)KDRV_IR_TX_MAX_SYMBOLS < n)
        || ((uint32_t)KDRV_IR_CARRIER_MIN_HZ > carrier_hz)
        || ((uint32_t)KDRV_IR_CARRIER_MAX_HZ < carrier_hz)
        || !ir_frame_fits(sym, n) || !s_init )
    {
        return KODE_DENIED;
    }

    hdr.op         = (uint8_t)KIR_REQ_SEND;
    hdr.version    = (uint8_t)KIR_WIRE_VERSION;
    hdr.n          = (uint16_t)n;
    hdr.carrier_hz = carrier_hz;
    (void)memcpy(s_req, &hdr, sizeof(hdr));
    (void)memcpy(&s_req[sizeof(hdr)], sym, n * sizeof(kir_symbol_t));

    /* A word on a frame whose own wait gave up is drained here, so it cannot
     * be taken for this frame's. */
    (void)khal_sem_wait_timeout(s_sent, 0U);
    rc = kdrv_radio_peer_send(KIR_MSG_REQ, s_req,
                              sizeof(hdr) + (n * sizeof(kir_symbol_t)));
    if ( KODE_OK != rc )
    {
        if ( KODE_DENIED == rc )
        {
            KLOG_W(TAG, "the C5 link is down; the infrared block is behind it "
                        "(radio_up first)");
        }
        return KODE_DENIED;
    }
    if ( !khal_sem_wait_timeout(s_sent, KDRV_IR_SENT_MS) )
    {
        KLOG_E(TAG, "the C5 did not say within %u ms whether the frame went out",
               KDRV_IR_SENT_MS);
        return KODE_TIMEOUT;
    }
    if ( (uint8_t)KIR_OK != s_sent_status )
    {
        KLOG_W(TAG, "the C5 reports a transmission that failed (status %u)",
               (unsigned)s_sent_status);
        return KODE_ERROR;
    }
    return KODE_OK;
}

size_t kdrv_ir_rx_take(kdrv_ir_symbol_t *out, size_t cap)
{
    size_t n;

    if ( (NULL == out) || (0U == cap) || !s_rx_ready )
    {
        return 0U;
    }
    n = s_rx_len;
    if ( n > cap )
    {
        KLOG_W(TAG, "a capture of %u symbols is truncated to %u",
               (unsigned)n, (unsigned)cap);
        n = cap;
    }
    (void)memcpy(out, s_rx, n * sizeof(kdrv_ir_symbol_t));
    s_rx_ready = false;
    return n;
}

kode_status_t kdrv_ir_rx_arm(void)
{
    if ( !s_init )
    {
        return KODE_DENIED;
    }
    if ( s_armed )
    {
        return KODE_ERROR;   /* a capture is in flight */
    }
    s_rx_len   = 0U;
    s_rx_ready = false;
    if ( ir_plain((uint8_t)KIR_REQ_ARM) != KODE_OK )
    {
        return KODE_DENIED;
    }
    s_armed = true;
    return KODE_OK;
}

#else /* !CONFIG_KODEOS_RADIO */

/* With no link in the build there is no way to the C5, and so no infrared
 * block on this board: bring-up refuses and says why, and every later call
 * answers as the header promises for a driver that was never brought up. */

kode_status_t kdrv_ir_init(kdrv_ir_rx_cb_t cb, void *ctx)
{
    (void)cb;
    (void)ctx;
    KLOG_W(TAG, "the infrared block is the ESP32-C5's (TX IO%d, RX IO%d) "
                "and this firmware has no link to it (KODEOS_RADIO)",
           KBSP_C5_IO_IR_TX, KBSP_C5_IO_IR_RX);
    return KODE_ERROR;
}

kode_status_t kdrv_ir_rx_idle(bool *high)
{
    (void)high;
    return KODE_DENIED;
}

kode_status_t kdrv_ir_rx_present(bool *present)
{
    (void)present;
    return KODE_DENIED;
}

kode_status_t kdrv_ir_send(const kdrv_ir_symbol_t *sym, size_t n,
                           uint32_t carrier_hz)
{
    (void)sym;
    (void)n;
    (void)carrier_hz;
    return KODE_DENIED;
}

size_t kdrv_ir_rx_take(kdrv_ir_symbol_t *out, size_t cap)
{
    (void)out;
    (void)cap;
    return 0U;
}

kode_status_t kdrv_ir_rx_arm(void)
{
    return KODE_DENIED;
}

#endif /* CONFIG_KODEOS_RADIO */
