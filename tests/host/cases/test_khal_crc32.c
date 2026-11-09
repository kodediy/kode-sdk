/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_khal_crc32.c
 * @brief The one CRC-32 variant this system uses.
 */
#include "unity.h"

#include "khal_crc32.h"

/* zlib.crc32(b"123456789") == 0xCBF43926, and the header promises this
 * function agrees with it byte for byte - which is what kcli.py relies on
 * when it checksums a transfer on the host side. */
TEST_CASE("crc32 matches the standard check vector", "[khal_crc32]")
{
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926U,
        khal_crc32(KHAL_CRC32_INIT, "123456789", 9U));
}

TEST_CASE("crc32 chains across chunks", "[khal_crc32]")
{
    uint32_t c = khal_crc32(KHAL_CRC32_INIT, "1234", 4U);

    c = khal_crc32(c, "56789", 5U);
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926U, c);
}

TEST_CASE("crc32 of no bytes returns the seed untouched", "[khal_crc32]")
{
    TEST_ASSERT_EQUAL_HEX32(KHAL_CRC32_INIT,
        khal_crc32(KHAL_CRC32_INIT, "abc", 0U));
    TEST_ASSERT_EQUAL_HEX32(0x1234U, khal_crc32(0x1234U, NULL, 8U));
}

/* zlib.crc32(b"\x00"). A single zero byte is the case a broken implementation
 * most often gets wrong by returning zero, which would then look like "no
 * data" everywhere a checksum is compared against an erased field. */
TEST_CASE("crc32 of a single zero byte is not zero", "[khal_crc32]")
{
    static const uint8_t zero = 0U;

    TEST_ASSERT_EQUAL_HEX32(0xD202EF8DU,
        khal_crc32(KHAL_CRC32_INIT, &zero, 1U));
}
