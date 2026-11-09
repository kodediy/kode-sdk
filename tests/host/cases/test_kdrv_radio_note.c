/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_radio_note.c
 * @brief The rule that keeps a parked C5 across a restart, or resets it.
 */
#include <stddef.h>

#include "unity.h"

#include "kdrv_radio.h"
#include "kdrv_radio_note.h"

/** @brief The note a word stores, for a target the way the driver leaves it. */
static kradio_note_t note_for(uint32_t target)
{
    return kradio_note_unpack(kradio_note_pack(target));
}

TEST_CASE("a note for this role after a software reset keeps the C5",
          "[radio_note]")
{
    kradio_note_t os  = note_for((uint32_t)KDRV_RADIO_NOTE_OS);
    kradio_note_t app = note_for((uint32_t)KDRV_RADIO_NOTE_APP);

    TEST_ASSERT_TRUE(kradio_note_keeps_c5(&os, true, true, true));
    TEST_ASSERT_TRUE(kradio_note_keeps_c5(&app, true, false, true));
}

TEST_CASE("a note for the other role resets the C5", "[radio_note]")
{
    kradio_note_t os  = note_for((uint32_t)KDRV_RADIO_NOTE_OS);
    kradio_note_t app = note_for((uint32_t)KDRV_RADIO_NOTE_APP);

    TEST_ASSERT_FALSE(kradio_note_keeps_c5(&app, true, true, true));
    TEST_ASSERT_FALSE(kradio_note_keeps_c5(&os, true, false, true));
}

TEST_CASE("a hardware or panic reset resets the C5", "[radio_note]")
{
    kradio_note_t os  = note_for((uint32_t)KDRV_RADIO_NOTE_OS);
    kradio_note_t app = note_for((uint32_t)KDRV_RADIO_NOTE_APP);

    TEST_ASSERT_FALSE(kradio_note_keeps_c5(&os, false, true, true));
    TEST_ASSERT_FALSE(kradio_note_keeps_c5(&app, false, false, true));
}

TEST_CASE("no magic resets the C5", "[radio_note]")
{
    /* An erased register, a register of ones, and a wrong magic over a good
     * target. */
    static const uint32_t WORDS[] = {
        0x00000001UL, 0xFFFF0001UL, 0xFFFFFFFFUL, 0x4B510001UL, 0x00010001UL,
    };

    for ( size_t i = 0U; (sizeof(WORDS) / sizeof(WORDS[0])) > i; ++i )
    {
        kradio_note_t n = kradio_note_unpack(WORDS[i]);

        TEST_ASSERT_FALSE(kradio_note_keeps_c5(&n, true, true, true));
        TEST_ASSERT_FALSE(kradio_note_keeps_c5(&n, true, false, true));
    }
    {
        kradio_note_t erased = kradio_note_unpack(0U);

        TEST_ASSERT_FALSE(kradio_note_keeps_c5(&erased, true, true, true));
    }
}

TEST_CASE("a boot that does not want Wi-Fi resets the C5", "[radio_note]")
{
    kradio_note_t os  = note_for((uint32_t)KDRV_RADIO_NOTE_OS);
    kradio_note_t app = note_for((uint32_t)KDRV_RADIO_NOTE_APP);

    TEST_ASSERT_FALSE(kradio_note_keeps_c5(&os, true, true, false));
    TEST_ASSERT_FALSE(kradio_note_keeps_c5(&app, true, false, false));
}

TEST_CASE("an unknown target resets the C5", "[radio_note]")
{
    kradio_note_t zero  = note_for(0U);
    kradio_note_t three = note_for(3U);

    TEST_ASSERT_FALSE(kradio_note_keeps_c5(&zero, true, true, true));
    TEST_ASSERT_FALSE(kradio_note_keeps_c5(&zero, true, false, true));
    TEST_ASSERT_FALSE(kradio_note_keeps_c5(&three, true, true, true));
    TEST_ASSERT_FALSE(kradio_note_keeps_c5(&three, true, false, true));
}

TEST_CASE("a note is one word: the magic above, the target below",
          "[radio_note]")
{
    uint32_t const os  = kradio_note_pack((uint32_t)KDRV_RADIO_NOTE_OS);
    uint32_t const app = kradio_note_pack((uint32_t)KDRV_RADIO_NOTE_APP);
    kradio_note_t n    = kradio_note_unpack(os);

    TEST_ASSERT_EQUAL_HEX32(0x4B500001UL, os);
    TEST_ASSERT_EQUAL_HEX32(0x4B500002UL, app);
    TEST_ASSERT_EQUAL_HEX32(KRADIO_NOTE_MAGIC, n.magic);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KDRV_RADIO_NOTE_OS, n.target);
    n = kradio_note_unpack(app);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KDRV_RADIO_NOTE_APP, n.target);
    /* A target wider than its half cannot spill into the magic. */
    TEST_ASSERT_EQUAL_HEX32(0x4B50FFFFUL, kradio_note_pack(0xFFFFFFFFUL));
}

TEST_CASE("a missing note resets the C5", "[radio_note]")
{
    TEST_ASSERT_FALSE(kradio_note_keeps_c5(NULL, true, true, true));
}

TEST_CASE("the note module is really compiled in", "[radio_note]")
{
    /* A source nobody lists is a source nobody links: the rule answering
     * true for one input and false for its neighbour is the proof that the
     * real function ran, not a header's promise. */
    kradio_note_t os = note_for((uint32_t)KDRV_RADIO_NOTE_OS);

    TEST_ASSERT_EQUAL_HEX32(0x4B50U, KRADIO_NOTE_MAGIC);
    TEST_ASSERT_TRUE(kradio_note_keeps_c5(&os, true, true, true));
    TEST_ASSERT_FALSE(kradio_note_keeps_c5(&os, true, true, false));
}
