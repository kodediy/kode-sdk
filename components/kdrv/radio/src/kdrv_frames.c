/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_frames.c
 * @brief The P4's end of the Wi-Fi frames service: batches decoded into a hand-over ring and a tick.
 *
 * kdrv_frames_feed() runs on esp-hosted's thread and the takers on the
 * owner's task, so one mutex guards the ring and the tick; it is held for the
 * copy and nothing else. The ring lives in PSRAM: internal RAM has no room
 * for a table of this size.
 */
#include "kdrv_frames.h"

#include <stdbool.h>
#include <string.h>

#include "kode_wire.h"
#include "khal_sync.h"
#include "khal_system.h"

/** @brief Alignment of the ring's block: a frame holds a uint16_t. */
#define FRAMES_RING_ALIGN  4U

/** @brief The type bits of a frame control's first byte (802.11, bits 2-3). */
#define FRAMES_FC_TYPE_MASK  0x0CU

/** @brief Their value in a data frame. */
#define FRAMES_FC_TYPE_DATA  0x08U

/** @brief What the driver keeps between calls. */
static struct {
    bool                ready;   /**< kdrv_frames_init() succeeded.           */
    khal_mutex_t        lock;    /**< Guards everything below.                */
    kdrv_frame_t       *ring;    /**< KDRV_FRAMES_RING_LEN frames, PSRAM.     */
    uint8_t             head;    /**< Where the next frame goes.              */
    uint8_t             tail;    /**< The oldest waiting frame.               */
    uint8_t             used;    /**< Frames waiting.                         */
    uint32_t            dropped; /**< Frames refused since the last forget.   */
    bool                tick_new; /**< tick holds a batch not yet taken.      */
    kdrv_frames_tick_t  tick;    /**< The newest batch's summary.             */
} s_frames;

/* A C5 sends kode_sniff_rec_short_t unless asked for the wide record, and a
 * C5 from before KODE_SNIFF_WIDE always does (kode_wire.h); this end reads
 * both, told apart by the size of a batch's records. */
_Static_assert(KODE_SNIFF_SHORT_HEAD_LEN <= KODE_SNIFF_HEAD_LEN,
               "a short head fits where a head is kept");

_Static_assert(KDRV_FRAMES_RING_LEN <= 255U, "the ring's indexes are uint8_t");
_Static_assert(sizeof(((kode_sniff_rec_t *)NULL)->fc) == KDRV_FRAME_FC_LEN,
               "a record keeps the frame control the wire carries");
_Static_assert(sizeof(((kode_sniff_rec_t *)NULL)->addr1) == KDRV_FRAME_ADDR_LEN,
               "a record keeps the addresses the wire carries");

/**
 * @brief The ring's next free place, or NULL with the frame counted as dropped.
 *        Called with the lock held.
 * @return The place, already counted as used and cleared.
 */
static kdrv_frame_t *slot_take(void)
{
    kdrv_frame_t *slot = NULL;

    if ( KDRV_FRAMES_RING_LEN <= s_frames.used )
    {
        s_frames.dropped++;
    }
    else
    {
        slot = &s_frames.ring[s_frames.head];
        (void)memset(slot, 0, sizeof(*slot));
        s_frames.head = (uint8_t)((s_frames.head + 1U) % KDRV_FRAMES_RING_LEN);
        s_frames.used++;
    }
    return slot;
}

/**
 * @brief Whether a frame is a data frame, whose head is a third party's traffic.
 * @param fc The frame control, as on the air.
 * @return true for a data frame.
 */
static bool frame_is_data(const uint8_t fc[KDRV_FRAME_FC_LEN])
{
    return FRAMES_FC_TYPE_DATA == (fc[0] & FRAMES_FC_TYPE_MASK);
}

/**
 * @brief Copy one wire record into the ring, or count it as dropped; a data
 *        frame keeps no head. Called with the lock held.
 * @param channel The batch's channel.
 * @param rec     The record as the C5 sent it.
 */
static void push(uint8_t channel, const kode_sniff_rec_t *rec)
{
    kdrv_frame_t * const slot = slot_take();

    if ( NULL != slot )
    {
        slot->channel  = channel;
        slot->rssi     = rec->rssi;
        slot->len      = rec->len;
        (void)memcpy(slot->fc, rec->fc, sizeof(slot->fc));
        (void)memcpy(slot->addr1, rec->addr1, sizeof(slot->addr1));
        (void)memcpy(slot->addr2, rec->addr2, sizeof(slot->addr2));
        (void)memcpy(slot->addr3, rec->addr3, sizeof(slot->addr3));
        if ( !frame_is_data(slot->fc) )
        {
            slot->head_len = (uint8_t)KODE_SNIFF_HEAD_LEN;
            (void)memcpy(slot->head, rec->head, sizeof(slot->head));
        }
    }
}

/**
 * @brief Copy one record of the size it had into the ring, or count it as
 *        dropped: its eight bytes of head, none in a data frame, zeros behind
 *        them and for the address it did not carry. Called with the lock held.
 * @param channel The batch's channel.
 * @param rec     The short record, as a C5 from before KODE_SNIFF_WIDE sends it.
 */
static void push_short(uint8_t channel, const kode_sniff_rec_short_t *rec)
{
    kdrv_frame_t * const slot = slot_take();

    if ( NULL != slot )
    {
        slot->channel  = channel;
        slot->rssi     = rec->rssi;
        slot->len      = rec->len;
        (void)memcpy(slot->fc, rec->fc, sizeof(slot->fc));
        (void)memcpy(slot->addr1, rec->addr1, sizeof(slot->addr1));
        (void)memcpy(slot->addr2, rec->addr2, sizeof(slot->addr2));
        if ( !frame_is_data(slot->fc) )
        {
            slot->head_len = (uint8_t)KODE_SNIFF_SHORT_HEAD_LEN;
            (void)memcpy(slot->head, rec->head, sizeof(rec->head));
        }
    }
}

kode_status_t kdrv_frames_init(void)
{
    if ( s_frames.ready )
    {
        return KODE_OK;
    }
    if ( NULL == s_frames.lock )
    {
        s_frames.lock = khal_mutex_create();
    }
    if ( NULL == s_frames.ring )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() hands back an untyped block aligned for any type */
        s_frames.ring = (kdrv_frame_t *)khal_alloc_psram(FRAMES_RING_ALIGN,
            (size_t)KDRV_FRAMES_RING_LEN * sizeof(kdrv_frame_t));
    }
    if ( (NULL == s_frames.lock) || (NULL == s_frames.ring) )
    {
        return KODE_NO_RESOURCES;
    }
    s_frames.ready = true;
    return KODE_OK;
}

/**
 * @brief Keep a batch header's period as the newest tick. Called with the
 *        lock held.
 * @param h The header.
 */
static void tick_keep(const kode_sniff_batch_t *h)
{
    s_frames.tick.channel  = h->channel;
    s_frames.tick.joined   = (0U != h->joined);
    s_frames.tick.rssi_max = h->rssi_max;
    s_frames.tick.heard    = h->heard;
    s_frames.tick.mgmt     = h->mgmt;
    s_frames.tick.data     = h->data;
    s_frames.tick_new      = true;
}

/**
 * @brief Count a batch whose records are not whole as the records it claims
 *        lost, one at least, and keep its period as a tick: the tick carries
 *        the drop count to a listener even when every batch is refused.
 * @param h The batch's header.
 */
static void batch_refused(const kode_sniff_batch_t *h)
{
    uint32_t lost = (uint32_t)h->count;

    if ( 0U == lost )
    {
        lost = 1U;
    }
    khal_mutex_lock(s_frames.lock);
    s_frames.dropped += lost;
    tick_keep(h);
    khal_mutex_unlock(s_frames.lock);
}

bool kdrv_frames_feed(const uint8_t *payload, size_t len)
{
    kode_sniff_batch_t h;

    if ( (!s_frames.ready) || (NULL == payload) || (sizeof(h) > len) )
    {
        return false;
    }
    /* cppcheck-suppress misra-c2012-21.15 ; the wire is bytes; kode_sniff_batch_t is packed and its size asserted */
    (void)memcpy(&h, payload, sizeof(h));
    if ( KODE_SNIFF_BATCH_MAX < h.count )
    {
        batch_refused(&h);
        return true;
    }
    /* A batch does not say how big its records are: what is left after its
     * header, shared out among them, does. Whole records of the size the
     * record is, or of the size it was; anything else is not a batch. */
    size_t const body = len - sizeof(h);
    size_t       each = sizeof(kode_sniff_rec_t);

    if ( 0U < h.count )
    {
        if ( (body % (size_t)h.count) != 0U )
        {
            batch_refused(&h);
            return true;
        }
        each = body / (size_t)h.count;
        if ( (sizeof(kode_sniff_rec_t) != each) && (sizeof(kode_sniff_rec_short_t) != each) )
        {
            batch_refused(&h);
            return true;
        }
    }
    else if ( 0U != body )
    {
        batch_refused(&h);
        return true;
    }
    else
    {
        /* A period that heard nothing: the header alone. */
    }

    khal_mutex_lock(s_frames.lock);
    tick_keep(&h);
    for ( uint8_t i = 0U; h.count > i; i++ )
    {
        const uint8_t * const at = &payload[sizeof(h) + ((size_t)i * each)];

        if ( sizeof(kode_sniff_rec_t) == each )
        {
            kode_sniff_rec_t rec;
            /* cppcheck-suppress misra-c2012-21.15 ; the wire is bytes; kode_sniff_rec_t is packed and its size asserted */
            (void)memcpy(&rec, at, sizeof(rec));
            push(h.channel, &rec);
        }
        else
        {
            kode_sniff_rec_short_t rec;
            /* cppcheck-suppress misra-c2012-21.15 ; the wire is bytes; kode_sniff_rec_short_t is packed and its size asserted */
            (void)memcpy(&rec, at, sizeof(rec));
            push_short(h.channel, &rec);
        }
    }
    khal_mutex_unlock(s_frames.lock);
    return true;
}

void kdrv_frames_answer(uint8_t status, const uint8_t *payload, size_t len,
                        kdrv_frames_state_t *out)
{
    kode_sniff_state_t s;

    if ( NULL == out )
    {
        return;
    }
    (void)memset(out, 0, sizeof(*out));
    if ( ((KODE_ST_OK != status) && (KODE_ST_REFUSED != status))
        || (NULL == payload) || (sizeof(s) > len) )
    {
        return;
    }
    /* cppcheck-suppress misra-c2012-21.15 ; the wire is bytes; kode_sniff_state_t is packed and its size asserted */
    (void)memcpy(&s, payload, sizeof(s));
    out->present = true;
    out->on      = (0U != s.on);
    out->channel = s.channel;
    out->joined  = (0U != s.joined);
    out->why     = s.why;
}

bool kdrv_frames_tick_take(kdrv_frames_tick_t *out)
{
    bool taken = false;

    if ( (!s_frames.ready) || (NULL == out) )
    {
        return false;
    }
    khal_mutex_lock(s_frames.lock);
    if ( s_frames.tick_new )
    {
        *out = s_frames.tick;
        s_frames.tick_new = false;
        taken = true;
    }
    khal_mutex_unlock(s_frames.lock);
    return taken;
}

bool kdrv_frames_next(kdrv_frame_t *out)
{
    bool taken = false;

    if ( (!s_frames.ready) || (NULL == out) )
    {
        return false;
    }
    khal_mutex_lock(s_frames.lock);
    if ( 0U < s_frames.used )
    {
        *out = s_frames.ring[s_frames.tail];
        s_frames.tail = (uint8_t)((s_frames.tail + 1U) % KDRV_FRAMES_RING_LEN);
        s_frames.used--;
        taken = true;
    }
    khal_mutex_unlock(s_frames.lock);
    return taken;
}

uint32_t kdrv_frames_dropped(void)
{
    uint32_t n = 0U;

    if ( s_frames.ready )
    {
        khal_mutex_lock(s_frames.lock);
        n = s_frames.dropped;
        khal_mutex_unlock(s_frames.lock);
    }
    return n;
}

void kdrv_frames_forget(void)
{
    if ( !s_frames.ready )
    {
        return;
    }
    khal_mutex_lock(s_frames.lock);
    s_frames.head     = 0U;
    s_frames.tail     = 0U;
    s_frames.used     = 0U;
    s_frames.dropped  = 0U;
    s_frames.tick_new = false;
    khal_mutex_unlock(s_frames.lock);
}
