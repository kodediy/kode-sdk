/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kmirror_proto.c
 * @brief The mirror's arithmetic, where a bench would never show the bug.
 */
#include "unity.h"

#include "kmirror_proto.h"

TEST_CASE("a header survives a round trip", "[kmirror]")
{
    kmirror_hdr_t const in = { .seq = 0x1234U, .x = 7U, .y = 250U,
                               .w = 205U, .h = 60U, .len = 0xDEADBEEFU };
    uint8_t  wire[KMIRROR_HDR_LEN];
    kmirror_hdr_t out;

    kmirror_hdr_put(wire, &in);
    TEST_ASSERT_TRUE(kmirror_hdr_get(wire, &out));
    TEST_ASSERT_EQUAL_UINT16(in.seq, out.seq);
    TEST_ASSERT_EQUAL_UINT16(in.x,   out.x);
    TEST_ASSERT_EQUAL_UINT16(in.y,   out.y);
    TEST_ASSERT_EQUAL_UINT16(in.w,   out.w);
    TEST_ASSERT_EQUAL_UINT16(in.h,   out.h);
    TEST_ASSERT_EQUAL_UINT32(in.len, out.len);
}

/* ---- The golden header: the one thing both implementations must agree on --
 *
 * There are TWO implementations of this wire format. kmirror_proto.c writes
 * it; tools/kmirror.py reads it, in Python, with its own `struct` format. Each
 * has its own tests, and each of those tests round-trips an implementation
 * against itself - so both stay green while the two disagree, and what breaks
 * is a board talking to a viewer, which is the one thing neither suite can
 * see.
 *
 * These fourteen bytes are the third party neither side can move. The same
 * literal appears in tools/tests/test_kmirror.py, hand-written there too, and that
 * duplication is the mechanism rather than a wart: a vector generated from
 * this C would follow this C's bugs, and one generated from the Python would
 * follow the Python's. tools/tests/test_mkkfw.sh makes the identical argument about
 * mkkfw.py's own header, in as many words.
 *
 * Changing the wire format means changing both literals, deliberately, and
 * this case failing is what makes "deliberately" happen.
 *
 * Field values are all distinct and none is symmetric, so a swapped x/y, a
 * transposed w/h or a byte order flipped anywhere shows up as a mismatch
 * rather than as a coincidence that still passes.
 */
#define GOLD_SEQ 0x1234U
#define GOLD_X   0x0005U
#define GOLD_Y   0x00FAU
#define GOLD_W   0x00CDU
#define GOLD_H   0x003CU
#define GOLD_LEN 0x00012345UL

static const uint8_t GOLDEN_HDR[KMIRROR_HDR_LEN] = {
    0x34U, 0x12U,               /* seq = 0x1234 */
    0x05U, 0x00U,               /* x   = 5      */
    0xFAU, 0x00U,               /* y   = 250    */
    0xCDU, 0x00U,               /* w   = 205    */
    0x3CU, 0x00U,               /* h   = 60     */
    0x45U, 0x23U, 0x01U, 0x00U, /* len = 74565  */
};

TEST_CASE("the header serialises to the exact bytes the host decoder expects",
          "[kmirror]")
{
    kmirror_hdr_t const in = { .seq = GOLD_SEQ, .x = GOLD_X, .y = GOLD_Y,
                               .w = GOLD_W, .h = GOLD_H,
                               .len = (uint32_t)GOLD_LEN };
    uint8_t wire[KMIRROR_HDR_LEN];

    kmirror_hdr_put(wire, &in);
    TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(GOLDEN_HDR, wire, KMIRROR_HDR_LEN,
        "tools/kmirror.py reads these fourteen bytes; if this case fails, the "
        "viewer no longer parses what the board sends - change the literal in "
        "tools/tests/test_kmirror.py too, and only on purpose");
}

TEST_CASE("the header parses back from those same bytes", "[kmirror]")
{
    kmirror_hdr_t out;

    TEST_ASSERT_TRUE(kmirror_hdr_get(GOLDEN_HDR, &out));
    TEST_ASSERT_EQUAL_UINT16(GOLD_SEQ, out.seq);
    TEST_ASSERT_EQUAL_UINT16(GOLD_X,   out.x);
    TEST_ASSERT_EQUAL_UINT16(GOLD_Y,   out.y);
    TEST_ASSERT_EQUAL_UINT16(GOLD_W,   out.w);
    TEST_ASSERT_EQUAL_UINT16(GOLD_H,   out.h);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)GOLD_LEN, out.len);
}

/* Little-endian, spelled out rather than trusted: a struct written straight
 * to the wire would carry this compiler's padding and this CPU's order, and
 * the host reading it is neither. */
TEST_CASE("the header is little-endian on the wire", "[kmirror]")
{
    kmirror_hdr_t const in = { .seq = 0x0201U, .x = 0U, .y = 0U,
                               .w = 0U, .h = 0U, .len = 0x04030201U };
    uint8_t wire[KMIRROR_HDR_LEN];

    kmirror_hdr_put(wire, &in);
    TEST_ASSERT_EQUAL_UINT8(0x01U, wire[0]);
    TEST_ASSERT_EQUAL_UINT8(0x02U, wire[1]);
    TEST_ASSERT_EQUAL_UINT8(0x01U, wire[10]);
    TEST_ASSERT_EQUAL_UINT8(0x04U, wire[13]);
}

TEST_CASE("the window bounds what is in flight", "[kmirror]")
{
    kmirror_credit_t c;
    kmirror_credit_init(&c, 2U);

    TEST_ASSERT_TRUE(kmirror_may_send(&c));
    uint16_t const first = kmirror_sent(&c);
    TEST_ASSERT_TRUE(kmirror_may_send(&c));
    (void)kmirror_sent(&c);
    TEST_ASSERT_FALSE(kmirror_may_send(&c));   /* two out, window is two */

    TEST_ASSERT_TRUE(kmirror_ack(&c, first));
    TEST_ASSERT_TRUE(kmirror_may_send(&c));
}

/* kmirror_in_flight() is what mirror_info reports as "frames outstanding" -
 * exercised here on its own, against known sequence/ack pairs, rather than
 * only ever read back indirectly through kmirror_may_send()'s bool. The ack
 * lands on the LAST of three sent, not the first, so the drop straight to
 * zero is what proves kmirror_ack() really is cumulative and not "one ack,
 * one slot" - the same property kmirror_proto.c's own doc comment claims. */
TEST_CASE("in-flight counts what is out and unacknowledged, nothing more",
          "[kmirror]")
{
    kmirror_credit_t c;
    kmirror_credit_init(&c, 4U);

    TEST_ASSERT_EQUAL_UINT16(0U, kmirror_in_flight(&c));

    (void)kmirror_sent(&c);
    (void)kmirror_sent(&c);
    uint16_t const last = kmirror_sent(&c);
    TEST_ASSERT_EQUAL_UINT16(3U, kmirror_in_flight(&c));

    TEST_ASSERT_TRUE(kmirror_ack(&c, last));
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0U, kmirror_in_flight(&c),
        "acking the newest of three outstanding frames must retire all "
        "three, cumulatively - not leave two still counted as in flight");
}

/* NULL is the same "no crash, no lie" answer kmirror_may_send() gives a NULL
 * window - there is no sequence count that could mean anything for a window
 * that does not exist. */
TEST_CASE("in-flight on a NULL window is zero, not a crash", "[kmirror]")
{
    TEST_ASSERT_EQUAL_UINT16(0U, kmirror_in_flight(NULL));
}

/* A window of zero would stall forever, so it is corrected rather than
 * obeyed. A caller that passes zero has a bug; a stream that never sends a
 * frame and never says why is a worse one. */
TEST_CASE("a zero window is corrected to one", "[kmirror]")
{
    kmirror_credit_t c;
    kmirror_credit_init(&c, 0U);
    TEST_ASSERT_TRUE(kmirror_may_send(&c));
    (void)kmirror_sent(&c);
    TEST_ASSERT_FALSE(kmirror_may_send(&c));
}

/* The case this file exists for. At 10 fps the sequence wraps every 109
 * minutes, so a comparison written as `seq > acked` works all afternoon and
 * then wedges the stream once, and whoever is watching sees a frozen picture
 * with no error anywhere. */
TEST_CASE("the sequence survives its wrap", "[kmirror]")
{
    kmirror_credit_t c;
    kmirror_credit_init(&c, 2U);
    c.next  = 0xFFFFU;
    c.acked = 0xFFFEU;

    uint16_t const last = kmirror_sent(&c);      /* 0xFFFF */
    uint16_t const wrapped = kmirror_sent(&c);   /* 0x0000 */
    TEST_ASSERT_EQUAL_UINT16(0xFFFFU, last);
    TEST_ASSERT_EQUAL_UINT16(0x0000U, wrapped);
    TEST_ASSERT_FALSE(kmirror_may_send(&c));

    TEST_ASSERT_TRUE(kmirror_ack(&c, wrapped));
    TEST_ASSERT_TRUE(kmirror_may_send(&c));
}

TEST_CASE("an ack nobody issued is refused", "[kmirror]")
{
    kmirror_credit_t c;
    kmirror_credit_init(&c, 2U);
    (void)kmirror_sent(&c);
    TEST_ASSERT_FALSE(kmirror_ack(&c, 9999U));
}

TEST_CASE("a band scales and clamps", "[kmirror]")
{
    uint32_t y = 0U;
    uint32_t h = 0U;

    kmirror_band(0, 502, 1U, 502U, &y, &h);
    TEST_ASSERT_EQUAL_UINT32(0U, y);
    TEST_ASSERT_EQUAL_UINT32(502U, h);

    kmirror_band(0, 502, 2U, 502U, &y, &h);
    TEST_ASSERT_EQUAL_UINT32(0U, y);
    TEST_ASSERT_EQUAL_UINT32(251U, h);

    /* An empty band - nothing was drawn - is height zero, not height one.
     * A frame of one row is a frame the host has to decode for nothing. */
    kmirror_band(300, 300, 1U, 502U, &y, &h);
    TEST_ASSERT_EQUAL_UINT32(0U, h);

    /* Out of range in both directions is clamped to the panel, because the
     * alternative is encoding somebody else's memory. */
    kmirror_band(-40, 9000, 1U, 502U, &y, &h);
    TEST_ASSERT_EQUAL_UINT32(0U, y);
    TEST_ASSERT_EQUAL_UINT32(502U, h);
}

/* An odd band at half scale must not round its height down to nothing, and
 * must not run one row past the bottom either. */
TEST_CASE("an odd band at half scale keeps its last row", "[kmirror]")
{
    uint32_t y = 0U;
    uint32_t h = 0U;

    kmirror_band(100, 103, 2U, 502U, &y, &h);
    TEST_ASSERT_EQUAL_UINT32(50U, y);
    TEST_ASSERT_EQUAL_UINT32(2U, h);      /* 3 rows -> 2, never 1 */
    TEST_ASSERT_TRUE((y + h) <= 251U);
}

/* The bug a length-only formula hides: a band that does not start on a scale
 * boundary spans more destination rows than its source length divided by the
 * scale would suggest, because the row it starts in is already partway
 * spent. Source rows 1 and 2 downsample to distinct destination rows 0 and
 * 1 - two rows are touched, not one - so a formula that only looked at
 * (y2 - y1) and rounded up would report a single row and drop the one
 * covering source row 2, with nothing in any log to say so. Every other
 * band in this file starts on an even row, which is exactly why this one
 * needed writing on its own. */
TEST_CASE("a misaligned band still spans every destination row it touches", "[kmirror]")
{
    uint32_t y = 0U;
    uint32_t h = 0U;

    kmirror_band(1, 3, 2U, 502U, &y, &h);
    TEST_ASSERT_EQUAL_UINT32(0U, y);
    TEST_ASSERT_EQUAL_UINT32(2U, h);

    kmirror_band(1, 5, 2U, 502U, &y, &h);
    TEST_ASSERT_EQUAL_UINT32(0U, y);
    TEST_ASSERT_EQUAL_UINT32(3U, h);
}
