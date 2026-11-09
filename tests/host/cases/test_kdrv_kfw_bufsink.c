/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_kfw_bufsink.c
 * @brief The buffer sink: the adapter between a decoder and a slow transport.
 */
#include <string.h>

#include "unity.h"

#include "kdrv_kfw.h"

#define CAP 64U

static uint8_t       s_buf[CAP];
static kfw_buf_ctx_t s_ctx;

/** @brief A fresh, empty sink over the whole buffer. */
static const kfw_sink_t *fresh(void)
{
    (void)memset(s_buf, 0, sizeof(s_buf));
    (void)memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.buf = s_buf;
    s_ctx.cap = CAP;
    return kfw_buf_sink();
}

/* ---- Opening --------------------------------------------------------------- */

TEST_CASE("a sink over a real buffer opens", "[kfw_bufsink]")
{
    const kfw_sink_t * const s = fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK, s->begin(&s_ctx, 32U));
    TEST_ASSERT_EQUAL_UINT32(32U, s_ctx.img_len);
    TEST_ASSERT_EQUAL_UINT32(0U, s_ctx.fill);
    TEST_ASSERT_EQUAL_UINT32(0U, s_ctx.total);
    TEST_ASSERT_FALSE(s_ctx.full);
}

/* A sink with nowhere to put anything is refused at the door rather than on
 * the first write, when a transfer is already in flight. */
TEST_CASE("a sink with no buffer, or no room, is refused", "[kfw_bufsink]")
{
    const kfw_sink_t * const s = kfw_buf_sink();
    kfw_buf_ctx_t            bad;

    /* KODE_DENIED rather than KODE_ERROR, and the distinction is worth
     * keeping: a sink with no buffer is a caller that wired it wrong, not a
     * transfer that went wrong. */
    (void)memset(&bad, 0, sizeof(bad));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, s->begin(NULL, 16U));

    bad.buf = NULL;
    bad.cap = CAP;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, s->begin(&bad, 16U));

    bad.buf = s_buf;
    bad.cap = 0U;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, s->begin(&bad, 16U));
}

/* ---- Filling --------------------------------------------------------------- */

TEST_CASE("bytes accumulate in order", "[kfw_bufsink]")
{
    const kfw_sink_t * const s = fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK, s->begin(&s_ctx, 8U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, s->write(&s_ctx, "abcd", 4U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, s->write(&s_ctx, "efgh", 4U));

    TEST_ASSERT_EQUAL_UINT32(8U, s_ctx.fill);
    TEST_ASSERT_EQUAL_UINT32(8U, s_ctx.total);
    TEST_ASSERT_EQUAL_MEMORY("abcdefgh", s_buf, 8U);
}

/* Exactly full is full: the flag is what tells the owner to drain, and it has
 * to go up on the write that fills the last byte rather than on the next one
 * that fails. */
TEST_CASE("filling the buffer exactly marks it full without refusing",
          "[kfw_bufsink]")
{
    const kfw_sink_t * const s = fresh();
    static uint8_t           payload[CAP];

    (void)memset(payload, 'x', sizeof(payload));

    TEST_ASSERT_EQUAL_INT(KODE_OK, s->begin(&s_ctx, CAP));
    TEST_ASSERT_EQUAL_INT(KODE_OK, s->write(&s_ctx, payload, CAP));

    TEST_ASSERT_EQUAL_UINT32(CAP, s_ctx.fill);
    TEST_ASSERT_TRUE(s_ctx.full);
}

/* The case the header is written for. A write that does not fit is refused
 * **with nothing copied**, so the owner can drain and offer the very same
 * chunk again - and the decoder's own accounting stays right, because it only
 * advances on success. */
TEST_CASE("a write that will not fit copies nothing and says so",
          "[kfw_bufsink]")
{
    const kfw_sink_t * const s = fresh();
    static uint8_t           big[CAP + 1U];

    (void)memset(big, 'y', sizeof(big));

    TEST_ASSERT_EQUAL_INT(KODE_OK, s->begin(&s_ctx, sizeof(big)));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES,
                          s->write(&s_ctx, big, sizeof(big)));

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, s_ctx.fill,
        "a partial copy would corrupt the image the digest then refuses");
    TEST_ASSERT_EQUAL_UINT32(0U, s_ctx.total);
    TEST_ASSERT_TRUE(s_ctx.full);
}

/* Drain and offer the same chunk again: that is the whole protocol, and the
 * counters have to survive it. */
TEST_CASE("draining lets the same chunk through, and total keeps counting",
          "[kfw_bufsink]")
{
    const kfw_sink_t * const s = fresh();
    static uint8_t           half[CAP];

    (void)memset(half, 'z', sizeof(half));

    TEST_ASSERT_EQUAL_INT(KODE_OK, s->begin(&s_ctx, CAP * 2U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, s->write(&s_ctx, half, CAP));
    TEST_ASSERT_TRUE(s_ctx.full);

    /* The refused chunk. */
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, s->write(&s_ctx, half, CAP));

    /* The owner drains: fill goes back, total does not. */
    s_ctx.fill = 0U;
    s_ctx.full = false;

    TEST_ASSERT_EQUAL_INT(KODE_OK, s->write(&s_ctx, half, CAP));
    TEST_ASSERT_EQUAL_UINT32(CAP, s_ctx.fill);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(CAP * 2U, s_ctx.total,
        "total counts what passed through, not what is sitting in the buffer");
}

/* ---- Closing --------------------------------------------------------------- */

/* A sink told how long the image is holds the transfer to it. Anything short
 * is a truncated image that would otherwise be handed on as complete. */
TEST_CASE("closing short of the declared length is refused", "[kfw_bufsink]")
{
    const kfw_sink_t * const s = fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK, s->begin(&s_ctx, 16U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, s->write(&s_ctx, "12345678", 8U));

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, s->end(&s_ctx));

    /* And the full length closes. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, s->write(&s_ctx, "abcdefgh", 8U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, s->end(&s_ctx));
}

/* A source that could not say how long the image is - a raw stream off the
 * console - passes 0, and then there is nothing to hold it to. */
TEST_CASE("a sink told no length accepts whatever arrived", "[kfw_bufsink]")
{
    const kfw_sink_t * const s = fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK, s->begin(&s_ctx, 0U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, s->write(&s_ctx, "abc", 3U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, s->end(&s_ctx));
}

/* Abandoning clears what is in the buffer but not the running total: the
 * owner may still want to know how far the transfer got before it died. */
TEST_CASE("abandoning empties the buffer but remembers how far it got",
          "[kfw_bufsink]")
{
    const kfw_sink_t * const s = fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK, s->begin(&s_ctx, 32U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, s->write(&s_ctx, "abcd", 4U));

    s->abort(&s_ctx);

    TEST_ASSERT_EQUAL_UINT32(0U, s_ctx.fill);
    TEST_ASSERT_FALSE(s_ctx.full);
    TEST_ASSERT_EQUAL_UINT32(4U, s_ctx.total);
}

/* Abandoning before anything opened is safe, which is the state a transfer
 * refused at its header dies in. */
TEST_CASE("abandoning an unopened sink is safe", "[kfw_bufsink]")
{
    const kfw_sink_t * const s = kfw_buf_sink();

    s->abort(NULL);
    (void)memset(&s_ctx, 0, sizeof(s_ctx));
    s->abort(&s_ctx);
}
