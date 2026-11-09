/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kring.c
 * @brief The stream's ring: one writer and one reader, so no lock.
 */
#include "kring.h"

/**
 * @brief Read an index the other side writes.
 * @param p The index.
 * @return Its value, ordered after everything written before it was stored.
 */
static uint32_t index_load(const uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

/**
 * @brief Publish an index the other side reads.
 * @param p The index.
 * @param v Its new value, stored after everything written before this call.
 */
static void index_store(uint32_t *p, uint32_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

/**
 * @brief How many frames lie between two indices.
 * @param r    The ring, with a capacity that is not 0.
 * @param from The earlier index.
 * @param to   The later one.
 * @return 0 .. cap.
 */
static uint32_t distance(const kring_t *r, uint32_t from, uint32_t to)
{
    uint32_t const span = 2U * r->cap;

    return (to + span - from) % span;
}

/**
 * @brief An index moved on by some frames, kept below twice the capacity.
 * @param r   The ring, with a capacity that is not 0.
 * @param idx The index.
 * @param k   How many frames.
 * @return The new index.
 */
static uint32_t advance(const kring_t *r, uint32_t idx, uint32_t k)
{
    return (idx + k) % (2U * r->cap);
}

void kring_init(kring_t *r, int16_t *buf, uint32_t cap)
{
    if ( NULL == r )
    {
        return;
    }
    r->buf = buf;
    r->cap = cap;
    if ( NULL == buf )
    {
        r->cap = 0U;
    }
    index_store(&r->head, 0U);
    index_store(&r->tail, 0U);
}

uint32_t kring_fill(const kring_t *r)
{
    uint32_t fill = 0U;

    if ( (NULL != r) && (0U != r->cap) )
    {
        fill = distance(r, index_load(&r->tail), index_load(&r->head));
    }
    return fill;
}

size_t kring_put(kring_t *r, const int16_t *frames, size_t n, uint8_t channels)
{
    uint32_t head;
    uint32_t room;
    size_t   put = 0U;

    if ( (NULL == r) || (NULL == frames) || (0U == r->cap)
        || ((1U != channels) && (2U != channels)) )
    {
        return 0U;
    }
    /* The writer's own index needs no ordering; the reader's does, because
     * the room it reports is room the reader has already finished with. */
    head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    room = r->cap - distance(r, index_load(&r->tail), head);

    while ( (put < n) && ((uint32_t)put < room) )
    {
        int16_t sample;

        if ( 2U == channels )
        {
            int32_t const left  = (int32_t)frames[put * 2U];
            int32_t const right = (int32_t)frames[(put * 2U) + 1U];
            sample = (int16_t)((left + right) / 2);
        }
        else
        {
            sample = frames[put];
        }
        r->buf[(head + (uint32_t)put) % r->cap] = sample;
        put += 1U;
    }
    index_store(&r->head, advance(r, head, (uint32_t)put));
    return put;
}

uint32_t kring_take(kring_t *r, int16_t *out, uint32_t max)
{
    uint32_t tail;
    uint32_t fill;
    uint32_t n = 0U;

    if ( (NULL == r) || (NULL == out) || (0U == r->cap) )
    {
        return 0U;
    }
    tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    fill = distance(r, tail, index_load(&r->head));

    while ( (n < max) && (n < fill) )
    {
        out[n] = r->buf[(tail + n) % r->cap];
        n += 1U;
    }
    index_store(&r->tail, advance(r, tail, n));
    return n;
}
