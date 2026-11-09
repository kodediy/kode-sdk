/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kapps_req.c
 * @brief The boot request's geometry check, which two readers now share.
 */
#include "unity.h"

#include <stddef.h>

#include "kapps_fmt.h"

/** @brief The real `apps` region, from kbsp/partitions.csv. */
#define REGION 0xBC0000U

TEST_CASE("a whole image on a page inside the region is bootable",
          "[kapps_req]")
{
    TEST_ASSERT_TRUE(kapps_req_bounds_ok(KAPPS_DATA_OFF, 0x1000U, REGION));
    TEST_ASSERT_TRUE(kapps_req_bounds_ok(0x100000U, 0x20000U, REGION));
}

/* Below KAPPS_DATA_OFF is the directory and the boot request itself. An image
 * booted from there would be executing the bookkeeping that describes it. */
TEST_CASE("an image below the data area is refused", "[kapps_req]")
{
    TEST_ASSERT_FALSE(kapps_req_bounds_ok(0U, 0x1000U, REGION));
    TEST_ASSERT_FALSE(kapps_req_bounds_ok(KAPPS_REQ_OFF, 0x1000U, REGION));
    TEST_ASSERT_FALSE(kapps_req_bounds_ok(KAPPS_DATA_OFF - KAPPS_ALIGN,
                                          0x1000U, REGION));

    /* And the first legal page is legal. */
    TEST_ASSERT_TRUE(kapps_req_bounds_ok(KAPPS_DATA_OFF, 0x1000U, REGION));
}

/* The bootloader maps an image by MMU page; an unaligned start cannot be
 * mapped at all, so it must never reach the mapping code. */
TEST_CASE("an image not on a 64KB page is refused", "[kapps_req]")
{
    TEST_ASSERT_FALSE(kapps_req_bounds_ok(KAPPS_DATA_OFF + 1U, 0x1000U, REGION));
    TEST_ASSERT_FALSE(kapps_req_bounds_ok(KAPPS_DATA_OFF + 0x8000U, 0x1000U,
                                          REGION));
    TEST_ASSERT_FALSE(kapps_req_bounds_ok(KAPPS_ALIGN - 1U, 0x1000U, REGION));
}

TEST_CASE("an image of no bytes is refused", "[kapps_req]")
{
    TEST_ASSERT_FALSE(kapps_req_bounds_ok(KAPPS_DATA_OFF, 0U, REGION));
    TEST_ASSERT_FALSE(kapps_req_bounds_ok(0x100000U, 0U, REGION));
}

TEST_CASE("an image that runs past the end of the region is refused",
          "[kapps_req]")
{
    /* One byte too long, at the last page. */
    TEST_ASSERT_FALSE(kapps_req_bounds_ok(REGION - KAPPS_ALIGN,
                                          KAPPS_ALIGN + 1U, REGION));

    /* Exactly to the last byte is fine: the test is `size <= remaining`. */
    TEST_ASSERT_TRUE(kapps_req_bounds_ok(REGION - KAPPS_ALIGN,
                                         KAPPS_ALIGN, REGION));

    /* An image the size of the whole data area, starting at its foot. */
    TEST_ASSERT_TRUE(kapps_req_bounds_ok(KAPPS_DATA_OFF,
                                         REGION - KAPPS_DATA_OFF, REGION));
    TEST_ASSERT_FALSE(kapps_req_bounds_ok(KAPPS_DATA_OFF,
                                          (REGION - KAPPS_DATA_OFF) + 1U,
                                          REGION));
}

TEST_CASE("an image starting past the end of the region is refused",
          "[kapps_req]")
{
    TEST_ASSERT_FALSE(kapps_req_bounds_ok(REGION, 0x1000U, REGION));
    TEST_ASSERT_FALSE(kapps_req_bounds_ok(REGION + KAPPS_ALIGN, 0x1000U,
                                          REGION));
}

/* An offset past the end of the region makes `region_size - offset` wrap to
 * something enormous, and `size <= enormous` is true. What refuses these is
 * the standalone `offset <= region_size` bound, which is why that test is
 * there at all rather than being folded into the subtraction. (Swapping the
 * two does not change the answer - `&&` still reaches the standalone bound,
 * and unsigned wrap is well defined. Measured, because the comment here first
 * claimed otherwise.) */
TEST_CASE("an offset past the end cannot wrap the remaining-space test",
          "[kapps_req]")
{
    /* Wildly past the end, on a page, with a size that would look fine
     * against a wrapped remainder. */
    TEST_ASSERT_FALSE(kapps_req_bounds_ok(0x10000000U, 0x1000U, REGION));

    /* And the extreme: an offset near the top of the address space. Aligned,
     * non-zero size, and `REGION - offset` would wrap to something enormous. */
    TEST_ASSERT_FALSE(kapps_req_bounds_ok(0xFFFF0000U, 0x1000U, REGION));
}

/* A length near the top of the range must not wrap either. */
TEST_CASE("a size near the maximum is refused rather than wrapped",
          "[kapps_req]")
{
    TEST_ASSERT_FALSE(kapps_req_bounds_ok(KAPPS_DATA_OFF, 0xFFFFFFFFU, REGION));
    TEST_ASSERT_FALSE(kapps_req_bounds_ok(KAPPS_DATA_OFF, 0xFFFF0000U, REGION));
}

/* A region smaller than its own data offset has nowhere to put an image. The
 * bootloader reads the region size from the partition table, so this is the
 * shape a corrupt or mis-generated table would take. */
TEST_CASE("a region with no data area accepts nothing", "[kapps_req]")
{
    TEST_ASSERT_FALSE(kapps_req_bounds_ok(KAPPS_DATA_OFF, 0x1000U, 0U));
    TEST_ASSERT_FALSE(kapps_req_bounds_ok(KAPPS_DATA_OFF, 0x1000U,
                                          KAPPS_DATA_OFF));

    /* One page of data area, and exactly one page fits in it. */
    TEST_ASSERT_TRUE(kapps_req_bounds_ok(KAPPS_DATA_OFF, KAPPS_ALIGN,
                                         KAPPS_DATA_OFF + KAPPS_ALIGN));
    TEST_ASSERT_FALSE(kapps_req_bounds_ok(KAPPS_DATA_OFF, KAPPS_ALIGN + 1U,
                                          KAPPS_DATA_OFF + KAPPS_ALIGN));
}

/* ---- The directory a reader may walk -------------------------------------- */

/** @brief A directory holding one live image, @p size bytes at @p offset. */
static kapps_dir_t dir_one(uint32_t offset, uint32_t size)
{
    kapps_dir_t d = { 0 };

    d.count       = 1U;
    d.e[0].offset = offset;
    d.e[0].size   = size;
    d.e[0].flags  = KAPPS_ENTRY_VALID;
    return d;
}

TEST_CASE("an empty directory and a well-placed image are sane", "[kapps_dir]")
{
    kapps_dir_t const empty = { 0 };
    kapps_dir_t const one   = dir_one(KAPPS_DATA_OFF, 0x1000U);

    TEST_ASSERT_TRUE(kapps_dir_sane(&empty, REGION));
    TEST_ASSERT_TRUE(kapps_dir_sane(&one, REGION));
}

/* A count past the array would walk every reader off the end of e[]. */
TEST_CASE("a count past the array is not sane", "[kapps_dir]")
{
    kapps_dir_t d = { 0 };

    d.count = KAPPS_MAX_ENTRIES;
    TEST_ASSERT_TRUE(kapps_dir_sane(&d, REGION));
    d.count = KAPPS_MAX_ENTRIES + 1U;
    TEST_ASSERT_FALSE(kapps_dir_sane(&d, REGION));
}

/* A live entry is an image somebody may boot or read back: the same geometry
 * the boot request is held to. */
TEST_CASE("a live entry outside the data area is not sane", "[kapps_dir]")
{
    kapps_dir_t const below = dir_one(0U, 0x1000U);
    kapps_dir_t const off   = dir_one(KAPPS_DATA_OFF + 0x1000U, 0x1000U);
    kapps_dir_t const empty = dir_one(KAPPS_DATA_OFF, 0U);
    kapps_dir_t const past  = dir_one(REGION - KAPPS_ALIGN, 0xFFFFFFF0U);

    TEST_ASSERT_FALSE(kapps_dir_sane(&below, REGION));
    TEST_ASSERT_FALSE(kapps_dir_sane(&off, REGION));
    TEST_ASSERT_FALSE(kapps_dir_sane(&empty, REGION));
    TEST_ASSERT_FALSE(kapps_dir_sane(&past, REGION));
}

/* A dead entry is a hole the next install fills; what it held is not read. */
TEST_CASE("a dead entry is not judged", "[kapps_dir]")
{
    kapps_dir_t d = dir_one(0U, 0U);

    d.e[0].flags = 0U;
    TEST_ASSERT_TRUE(kapps_dir_sane(&d, REGION));
}

/* The state transition the bootloader performs with a 4-byte write and no
 * erase. It works only because NEW's bits are a superset of TRIED's, so
 * clearing bits is enough - and because the CRC deliberately stops before
 * `state`, so flipping it does not invalidate the record. */
TEST_CASE("NEW can become TRIED by clearing bits alone", "[kapps_req]")
{
    TEST_ASSERT_EQUAL_HEX32(KAPPS_REQ_STATE_TRIED,
        KAPPS_REQ_STATE_NEW & KAPPS_REQ_STATE_TRIED);

    /* And erased flash is neither, so a blank sector is not a boot request. */
    TEST_ASSERT_NOT_EQUAL(KAPPS_REQ_STATE_NEW, KAPPS_REQ_STATE_EMPTY);
    TEST_ASSERT_NOT_EQUAL(KAPPS_REQ_STATE_TRIED, KAPPS_REQ_STATE_EMPTY);
}

/* The sticky request is the same trick one more time: the bootloader retires
 * ALWAYS with the same 4-byte write and no erase it uses on NEW, because NOR
 * programming only clears bits and setting one back would need a sector erase.
 *
 * ALWAYS is every other bit set, which is why it is 0x55555555 and not just
 * any unused value: it shares no bit with NEW. That is the property with
 * teeth, so it is the one asserted first. A value overlapping NEW could be
 * misread as a pending NEW while it was being cleared. */
TEST_CASE("ALWAYS can become TRIED by clearing bits alone", "[kapps_req]")
{
    TEST_ASSERT_EQUAL_HEX32(0U, KAPPS_REQ_STATE_ALWAYS & KAPPS_REQ_STATE_NEW);

    /* Trivially true while TRIED is all zeros; it stands as the statement of
     * what the bootloader's write does, and would start biting the day TRIED
     * stopped being 0. */
    TEST_ASSERT_EQUAL_HEX32(KAPPS_REQ_STATE_TRIED,
                            KAPPS_REQ_STATE_ALWAYS & KAPPS_REQ_STATE_TRIED);

    TEST_ASSERT_NOT_EQUAL(KAPPS_REQ_STATE_NEW, KAPPS_REQ_STATE_ALWAYS);
    TEST_ASSERT_NOT_EQUAL(KAPPS_REQ_STATE_EMPTY, KAPPS_REQ_STATE_ALWAYS);
}

/* A sticky request does not go straight to TRIED: its first boot leaves ARMED
 * behind, and only from there does the USB rule decide. That makes the chain
 * ALWAYS -> ARMED -> TRIED, and every step of it has to be reachable by
 * clearing bits alone, because the bootloader never erases the sector.
 *
 * `(ALWAYS & ARMED) == ARMED` is the statement that ARMED needs no bit ALWAYS
 * has already lost - the property the first write depends on. ARMED shares no
 * bit with NEW for the same reason ALWAYS does not: a half-written record must
 * never read as a pending one-shot. */
TEST_CASE("ALWAYS can become ARMED and then TRIED by clearing bits alone",
          "[kapps_req]")
{
    TEST_ASSERT_EQUAL_HEX32(KAPPS_REQ_STATE_ARMED,
        KAPPS_REQ_STATE_ALWAYS & KAPPS_REQ_STATE_ARMED);

    /* Trivially true while TRIED is all zeros, and it says what the second
     * write does; it would start biting the day TRIED stopped being 0. */
    TEST_ASSERT_EQUAL_HEX32(KAPPS_REQ_STATE_TRIED,
        KAPPS_REQ_STATE_ARMED & KAPPS_REQ_STATE_TRIED);

    TEST_ASSERT_EQUAL_HEX32(0U, KAPPS_REQ_STATE_ARMED & KAPPS_REQ_STATE_NEW);

    /* And it is its own state: nothing else in the vocabulary reads as ARMED. */
    TEST_ASSERT_NOT_EQUAL(KAPPS_REQ_STATE_EMPTY, KAPPS_REQ_STATE_ARMED);
    TEST_ASSERT_NOT_EQUAL(KAPPS_REQ_STATE_ALWAYS, KAPPS_REQ_STATE_ARMED);
    TEST_ASSERT_NOT_EQUAL(KAPPS_REQ_STATE_NEW, KAPPS_REQ_STATE_ARMED);
}

/* The CRC covers magic..size and stops before `crc` itself, which is what
 * leaves `state` outside it. The bootloader and kodeOS both compute it over
 * offsetof(kapps_req_t, crc) bytes, so that offset is the contract. */
TEST_CASE("the checksummed prefix stops before the checksum", "[kapps_req]")
{
    TEST_ASSERT_EQUAL_UINT(12U, offsetof(kapps_req_t, crc));
    TEST_ASSERT_EQUAL_UINT(16U, offsetof(kapps_req_t, state));
    TEST_ASSERT_EQUAL_UINT(20U, sizeof(kapps_req_t));
}
