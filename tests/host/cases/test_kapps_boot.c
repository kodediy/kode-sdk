/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kapps_boot.c
 * @brief The bootloader's decisions about a boot request, run on the host.
 */
#include "unity.h"

#include <stddef.h>
#include <stdint.h>

#include "kapps_fmt.h"

/* The P4's reset reasons as numbers, not imported: the bootloader asserts its names carry them. */
/** @brief The P4's power-on reset. */
#define CAUSE_POWER_ON   0x01U
/** @brief The core's main watchdog. */
#define CAUSE_CORE_MWDT  0x07U
/** @brief The core's RTC watchdog. */
#define CAUSE_CORE_RWDT  0x09U
/** @brief A brownout. */
#define CAUSE_BROWN_OUT  0x0FU
/** @brief The super watchdog. */
#define CAUSE_SUPER_WDT  0x12U
/** @brief A CPU lockup. */
#define CAUSE_LOCKUP     0x1AU
/** @brief A value no P4 reset reason carries. */
#define CAUSE_UNKNOWN    0x7FU

/** @brief A CRC the cases treat as the right one for their request. */
#define GOOD_CRC         0x1234ABCDU

/** @brief A request with the right magic and @p state. */
static kapps_req_t req_in(uint32_t state)
{
    kapps_req_t r = { .magic = KAPPS_REQ_MAGIC, .offset = KAPPS_DATA_OFF,
                      .size = 0x8000U, .crc = GOOD_CRC, .state = state };
    return r;
}

/* The closed list. Each of these is a reset a working sketch or its host
 * produces, and an armed request has to outlive it or the Serial Monitor and
 * the debugger would send the board home. */
TEST_CASE("each reset a working app or its host makes keeps an armed request",
          "[kapps_boot]")
{
    TEST_ASSERT_TRUE(kapps_req_cause_keeps(KAPPS_RESET_USB_UART));
    TEST_ASSERT_TRUE(kapps_req_cause_keeps(KAPPS_RESET_USB_JTAG));
    TEST_ASSERT_TRUE(kapps_req_cause_keeps(KAPPS_RESET_CPU_JTAG));
    TEST_ASSERT_TRUE(kapps_req_cause_keeps(KAPPS_RESET_CORE_SW));
    TEST_ASSERT_TRUE(kapps_req_cause_keeps(KAPPS_RESET_CPU_SW));
    TEST_ASSERT_TRUE(kapps_req_cause_keeps(KAPPS_RESET_DEEP_SLEEP));
}

/* Everything else retires it: power-on (BOT+LEFT included) is how a user
 * gets home from a sketch that loops, and a watchdog or a brownout is not a
 * reset anybody asked for. A number nobody named is refused too - the list
 * is closed, so an IDF that adds a cause cannot widen it silently. */
TEST_CASE("every other reset retires an armed request", "[kapps_boot]")
{
    TEST_ASSERT_FALSE(kapps_req_cause_keeps(CAUSE_POWER_ON));
    TEST_ASSERT_FALSE(kapps_req_cause_keeps(CAUSE_CORE_MWDT));
    TEST_ASSERT_FALSE(kapps_req_cause_keeps(CAUSE_CORE_RWDT));
    TEST_ASSERT_FALSE(kapps_req_cause_keeps(CAUSE_BROWN_OUT));
    TEST_ASSERT_FALSE(kapps_req_cause_keeps(CAUSE_SUPER_WDT));
    TEST_ASSERT_FALSE(kapps_req_cause_keeps(CAUSE_LOCKUP));
    TEST_ASSERT_FALSE(kapps_req_cause_keeps(CAUSE_UNKNOWN));
    TEST_ASSERT_FALSE(kapps_req_cause_keeps(0U));
}

/* A full mailbox means an Upload is waiting for kodeOS to adopt it. The armed
 * sketch must give way, or the new one would never be installed. */
TEST_CASE("an armed request stands only on a kept reset with an empty mailbox",
          "[kapps_boot]")
{
    TEST_ASSERT_TRUE(kapps_req_armed_stands(KAPPS_RESET_USB_UART, false));
    TEST_ASSERT_FALSE(kapps_req_armed_stands(KAPPS_RESET_USB_UART, true));
    TEST_ASSERT_FALSE(kapps_req_armed_stands(CAUSE_POWER_ON, false));
    TEST_ASSERT_FALSE(kapps_req_armed_stands(CAUSE_POWER_ON, true));
}

/* Each step only clears bits, so the bootloader advances the record with one
 * 4-byte write and no erase. ARMED stays ARMED: the bootloader writes nothing
 * for it, which is what lets it boot the same sketch again and again. */
TEST_CASE("a request advances NEW to TRIED, ALWAYS to ARMED, ARMED to itself",
          "[kapps_boot]")
{
    TEST_ASSERT_EQUAL_HEX32(KAPPS_REQ_STATE_TRIED,
                            kapps_req_next_state(KAPPS_REQ_STATE_NEW));
    TEST_ASSERT_EQUAL_HEX32(KAPPS_REQ_STATE_ARMED,
                            kapps_req_next_state(KAPPS_REQ_STATE_ALWAYS));
    TEST_ASSERT_EQUAL_HEX32(KAPPS_REQ_STATE_ARMED,
                            kapps_req_next_state(KAPPS_REQ_STATE_ARMED));
}

/* Only a request whose magic and CRC both hold, in one of the three states
 * that ask for a boot, sends the bootloader anywhere but kodeOS. */
TEST_CASE("only a whole request in a live state is live", "[kapps_boot]")
{
    kapps_req_t r = req_in(KAPPS_REQ_STATE_NEW);
    TEST_ASSERT_TRUE(kapps_req_live(&r, GOOD_CRC));
    r = req_in(KAPPS_REQ_STATE_ALWAYS);
    TEST_ASSERT_TRUE(kapps_req_live(&r, GOOD_CRC));
    r = req_in(KAPPS_REQ_STATE_ARMED);
    TEST_ASSERT_TRUE(kapps_req_live(&r, GOOD_CRC));

    r = req_in(KAPPS_REQ_STATE_NEW);
    TEST_ASSERT_FALSE_MESSAGE(kapps_req_live(&r, GOOD_CRC + 1U), "a torn request booted");
    r.magic = KAPPS_DIR_MAGIC;
    TEST_ASSERT_FALSE_MESSAGE(kapps_req_live(&r, GOOD_CRC), "another record's magic booted");

    r = req_in(KAPPS_REQ_STATE_EMPTY);
    TEST_ASSERT_FALSE(kapps_req_live(&r, GOOD_CRC));
    r = req_in(KAPPS_REQ_STATE_TRIED);
    TEST_ASSERT_FALSE(kapps_req_live(&r, GOOD_CRC));
    r = req_in(0x12345678U);
    TEST_ASSERT_FALSE_MESSAGE(kapps_req_live(&r, GOOD_CRC), "a state nobody writes booted");
}

/* The CRC stops at the state word, so advancing the state never breaks it:
 * an ALWAYS request the bootloader armed is still whole the next time. */
TEST_CASE("advancing the state leaves the request whole", "[kapps_boot]")
{
    kapps_req_t r = req_in(KAPPS_REQ_STATE_ALWAYS);
    r.state = kapps_req_next_state(r.state);
    TEST_ASSERT_TRUE(kapps_req_live(&r, GOOD_CRC));
    TEST_ASSERT_TRUE(offsetof(kapps_req_t, crc) < offsetof(kapps_req_t, state));
}
