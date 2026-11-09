/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kmirror_proto.c
 * @brief The mirror's wire format and flow control.
 */
#include "kmirror_proto.h"

#include <stddef.h>

/** @brief Store @p v little-endian, which is what the wire format says. */
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

/** @brief Read a little-endian 16-bit value. */
static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

void kmirror_hdr_put(uint8_t *dst, const kmirror_hdr_t *h)
{
    if ( (NULL == dst) || (NULL == h) )
    {
        return;
    }
    put_u16(&dst[0],  h->seq);
    put_u16(&dst[2],  h->x);
    put_u16(&dst[4],  h->y);
    put_u16(&dst[6],  h->w);
    put_u16(&dst[8],  h->h);
    put_u16(&dst[10], (uint16_t)(h->len & 0xFFFFU));
    put_u16(&dst[12], (uint16_t)((h->len >> 16) & 0xFFFFU));
}

bool kmirror_hdr_get(const uint8_t *src, kmirror_hdr_t *h)
{
    if ( (NULL == src) || (NULL == h) )
    {
        return false;
    }
    h->seq = get_u16(&src[0]);
    h->x   = get_u16(&src[2]);
    h->y   = get_u16(&src[4]);
    h->w   = get_u16(&src[6]);
    h->h   = get_u16(&src[8]);
    h->len = (uint32_t)get_u16(&src[10])
           | ((uint32_t)get_u16(&src[12]) << 16);
    return true;
}

void kmirror_credit_init(kmirror_credit_t *c, uint8_t window)
{
    if ( NULL == c )
    {
        return;
    }
    c->next   = 0U;
    c->acked  = 0xFFFFU;   /* "nothing sent yet" is one before the first */
    c->window = (0U == window) ? 1U : window;
}

uint16_t kmirror_in_flight(const kmirror_credit_t *c)
{
    if ( NULL == c )
    {
        return 0U;
    }
    /* Unsigned subtraction, which is the whole trick: it is correct across
     * the wrap and a signed comparison is not. At 10 fps the sequence wraps
     * every 109 minutes, so `next > acked` works for an afternoon and then
     * stalls the stream with nothing in any log to say why. */
    return (uint16_t)(c->next - c->acked - 1U);
}

bool kmirror_may_send(const kmirror_credit_t *c)
{
    if ( NULL == c )
    {
        return false;
    }
    return kmirror_in_flight(c) < (uint16_t)c->window;
}

uint16_t kmirror_sent(kmirror_credit_t *c)
{
    if ( NULL == c )
    {
        return 0U;   /* no sentinel sequence exists; this only avoids a crash */
    }
    uint16_t const seq = c->next;
    c->next = (uint16_t)(c->next + 1U);
    return seq;
}

bool kmirror_ack(kmirror_credit_t *c, uint16_t seq)
{
    if ( NULL == c )
    {
        return false;
    }
    /* Only a sequence that is actually outstanding may move the window. The
     * distance from the last ack must be within what is in flight; anything
     * else is a stale ack from a previous stream or a host inventing numbers,
     * and honouring one would open the window without bound. */
    uint16_t const ahead = (uint16_t)(seq - c->acked);
    if ( (0U == ahead) || (kmirror_in_flight(c) < ahead) )
    {
        return false;
    }
    c->acked = seq;
    return true;
}

void kmirror_band(int32_t y1, int32_t y2, uint32_t scale, uint32_t v_res,
                  uint32_t *out_y, uint32_t *out_h)
{
    if ( (NULL == out_y) || (NULL == out_h) )
    {
        return;
    }
    uint32_t const s = (0U == scale) ? 1U : scale;

    if ( 0 > y1 ) { y1 = 0; }
    if ( 0 > y2 ) { y2 = 0; }
    if ( (uint32_t)y1 > v_res ) { y1 = (int32_t)v_res; }
    if ( (uint32_t)y2 > v_res ) { y2 = (int32_t)v_res; }
    if ( y2 <= y1 )
    {
        *out_y = 0U;
        *out_h = 0U;
        return;
    }

    uint32_t const top  = (uint32_t)y1 / s;
    /* The row count depends on where the band SITS, not just how long it is:
     * source rows [y1, y2) land on destination rows [y1/s, (y2-1)/s], and
     * that span is one wider than its length alone would suggest whenever
     * y1 does not fall on a scale boundary. A length-only formula such as
     * ceil((y2-y1)/s) gets this wrong for a misaligned band - (1,3) at half
     * scale touches destination rows 0 and 1, two rows, but a 2-row source
     * span divided by 2 and rounded up says one, and the row covering
     * source row 2 is never sent. y2 > y1 here (checked above), so y2-1
     * cannot underflow. The cap below still stops the result running past
     * the panel once scaled. */
    uint32_t const bottom = ((uint32_t)y2 - 1U) / s;
    uint32_t const rows   = (bottom - top) + 1U;
    uint32_t const cap    = v_res / s;

    *out_y = top;
    *out_h = ((top + rows) > cap) ? (cap - top) : rows;
}
