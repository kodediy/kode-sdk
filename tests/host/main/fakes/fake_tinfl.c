/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_tinfl.c
 * @brief The inflate call, under the case's control.
 */
#include <string.h>

#include "fakes.h"
#include "miniz.h"

static tinfl_status s_status = TINFL_STATUS_DONE;
static size_t       s_out_per_call;
static size_t       s_in_per_call;
static uint8_t      s_fill = 'Z';
static unsigned     s_calls;
static uint32_t     s_last_flags;
static const uint8_t *s_payload;
static size_t       s_payload_len;
static uint8_t      s_in_seen[FAKE_TINFL_IN_KEEP];
static size_t       s_in_seen_len;

void fake_tinfl_reset(void)
{
    s_status       = TINFL_STATUS_DONE;
    s_out_per_call = 0U;
    s_in_per_call  = 0U;
    s_fill         = 'Z';
    s_calls        = 0U;
    s_last_flags   = 0U;
    s_payload      = NULL;
    s_payload_len  = 0U;
    s_in_seen_len  = 0U;
}

void fake_tinfl_set_payload(const uint8_t *bytes, size_t len)
{
    s_payload     = bytes;
    s_payload_len = len;
}

size_t fake_tinfl_in_len(void)
{
    return s_in_seen_len;
}

const uint8_t *fake_tinfl_in(void)
{
    return s_in_seen;
}

void fake_tinfl_set(int status, size_t in_consumed, size_t out_written)
{
    s_status       = (tinfl_status)status;
    s_in_per_call  = in_consumed;
    s_out_per_call = out_written;
}

void fake_tinfl_set_fill(uint8_t byte)
{
    s_fill = byte;
}

unsigned fake_tinfl_calls(void)
{
    return s_calls;
}

uint32_t fake_tinfl_last_flags(void)
{
    return s_last_flags;
}

/**
 * @brief Stands in for the ROM's inflate.
 *
 * Honours the caller's buffer sizes rather than the case's numbers when the
 * two disagree, because that is what a real decoder does and it is how the
 * dictionary-wrap arithmetic gets exercised at the boundary.
 */
tinfl_status tinfl_decompress(tinfl_decompressor *r,
                              const mz_uint8 *pIn_buf_next,
                              size_t *pIn_buf_size,
                              mz_uint8 *pOut_buf_start,
                              mz_uint8 *pOut_buf_next,
                              size_t *pOut_buf_size,
                              const mz_uint32 decomp_flags)
{
    (void)r;
    (void)pOut_buf_start;

    ++s_calls;
    s_last_flags = (uint32_t)decomp_flags;
    if ( (NULL != pIn_buf_size) && (NULL != pIn_buf_next) )
    {
        size_t keep = *pIn_buf_size;

        if ( sizeof(s_in_seen) < keep )
        {
            keep = sizeof(s_in_seen);
        }
        s_in_seen_len = *pIn_buf_size;
        (void)memcpy(s_in_seen, pIn_buf_next, keep);
    }
    if ( (NULL != s_payload) && (NULL != pOut_buf_size) )
    {
        /* A stream that inflates to exactly these bytes, read whole. */
        size_t n = s_payload_len;
        tinfl_status st = s_status;

        if ( n > *pOut_buf_size )
        {
            n  = *pOut_buf_size;
            st = TINFL_STATUS_HAS_MORE_OUTPUT;
        }
        if ( (NULL != pOut_buf_next) && (0U < n) )
        {
            (void)memcpy(pOut_buf_next, s_payload, n);
        }
        *pOut_buf_size = n;
        return st;
    }

    if ( NULL != pIn_buf_size )
    {
        size_t const avail = *pIn_buf_size;
        *pIn_buf_size = (s_in_per_call < avail) ? s_in_per_call : avail;
    }
    if ( NULL != pOut_buf_size )
    {
        size_t const room = *pOut_buf_size;
        size_t const n    = (s_out_per_call < room) ? s_out_per_call : room;

        if ( (NULL != pOut_buf_next) && (0U < n) )
        {
            (void)memset(pOut_buf_next, (int)s_fill, n);
        }
        *pOut_buf_size = n;
    }
    return s_status;
}
