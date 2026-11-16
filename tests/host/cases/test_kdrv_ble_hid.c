/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_ble_hid.c
 * @brief The HID service's rules: the choice, the report map, the ring's claims and the table's print.
 */
#include <stddef.h>
#include <string.h>

#include "unity.h"

#include "fakes.h"
#include "kdrv_ble_hid.h"
#include "kdrv_ble_hid_table.h"

/** @brief The first bytes of a collection: Usage Page (Generic Desktop), Usage. */
#define MAP_USAGE_AT   3U

/** @brief Usage (Mouse) and Usage (Keyboard) in the Generic Desktop page. */
#define USAGE_MOUSE    0x02U
#define USAGE_KEYBOARD 0x06U

/** @brief The item that names a collection's report: Report ID. */
#define ITEM_REPORT_ID 0x85U

/** @brief A fresh boot: nothing chosen, no table, no link. */
static void fresh(void)
{
    kdrv_ble_hid_select_forget();
}

/** @brief A boot whose table serves @p kinds, with a host subscribed to all of them. */
static void serving(uint8_t kinds)
{
    fresh();
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_hid_select(kinds));
    TEST_ASSERT_EQUAL_UINT8(kinds, kdrv_ble_hid_fix());
    if ( 0U != (kinds & KDRV_BLE_HID_MOUSE) )
    {
        kdrv_ble_hid_subscribed(KDRV_BLE_HID_MOUSE, true);
    }
    if ( 0U != (kinds & KDRV_BLE_HID_KEYBOARD) )
    {
        kdrv_ble_hid_subscribed(KDRV_BLE_HID_KEYBOARD, true);
    }
}

/**
 * @brief Claim and push one mouse report whose x is @p dx.
 * @return What the claim answered.
 */
static kode_status_t mouse_send(uint8_t dx)
{
    uint8_t const       rep[KDRV_BLE_HID_MOUSE_LEN] = { 0U, dx, 0U, 0U };
    kode_status_t const rc = kdrv_ble_hid_claim(KDRV_BLE_HID_MOUSE, 1U);

    if ( KODE_OK == rc )
    {
        kdrv_ble_hid_push(KDRV_BLE_HID_MOUSE, rep, sizeof(rep));
    }
    return rc;
}

TEST_CASE("a board that asks for nothing serves no HID", "[kdrv_ble_hid]")
{
    fresh();
    TEST_ASSERT_EQUAL_UINT8(0U, kdrv_ble_hid_kinds());
    TEST_ASSERT_EQUAL_UINT8(0U, kdrv_ble_hid_fix());
    TEST_ASSERT_FALSE(kdrv_ble_hid_served());
    TEST_ASSERT_FALSE(kdrv_ble_hid_ready());
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_hid_claim(KDRV_BLE_HID_MOUSE, 1U));
    fresh();
}

TEST_CASE("the kinds chosen before the table is built are the kinds served", "[kdrv_ble_hid]")
{
    fresh();
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_hid_select(KDRV_BLE_HID_MOUSE));
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_hid_select(KDRV_BLE_HID_KINDS));
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HID_KINDS, kdrv_ble_hid_kinds());
    TEST_ASSERT_FALSE(kdrv_ble_hid_served());
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HID_KINDS, kdrv_ble_hid_fix());
    TEST_ASSERT_TRUE(kdrv_ble_hid_served());
    fresh();
}

TEST_CASE("a choice after the table is registered is refused and changes nothing", "[kdrv_ble_hid]")
{
    fresh();
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_hid_select(KDRV_BLE_HID_KEYBOARD));
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HID_KEYBOARD, kdrv_ble_hid_fix());
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_hid_select(0U));
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_hid_select(KDRV_BLE_HID_KINDS));
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HID_KEYBOARD, kdrv_ble_hid_kinds());
    /* A second build in the same boot keeps the first one's kinds. */
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HID_KEYBOARD, kdrv_ble_hid_fix());
    fresh();
}

TEST_CASE("a kind that does not exist is refused", "[kdrv_ble_hid]")
{
    fresh();
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_hid_select(0x04U));
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_hid_select(0x80U | KDRV_BLE_HID_MOUSE));
    TEST_ASSERT_EQUAL_UINT8(0U, kdrv_ble_hid_kinds());
    fresh();
}

TEST_CASE("a ring PSRAM refuses serves no kind", "[kdrv_ble_hid]")
{
    fresh();
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_hid_select(KDRV_BLE_HID_MOUSE));
    fake_psram_refuse(1U);
    TEST_ASSERT_EQUAL_UINT8(0U, kdrv_ble_hid_fix());
    TEST_ASSERT_FALSE(kdrv_ble_hid_served());
    fresh();
}

TEST_CASE("the report map holds one collection per kind served", "[kdrv_ble_hid]")
{
    uint8_t mouse[KDRV_BLE_HID_MAP_MAX];
    uint8_t keys[KDRV_BLE_HID_MAP_MAX];
    uint8_t both[KDRV_BLE_HID_MAP_MAX];

    size_t const n_mouse = kdrv_ble_hid_report_map(KDRV_BLE_HID_MOUSE, mouse, sizeof(mouse));
    size_t const n_keys  = kdrv_ble_hid_report_map(KDRV_BLE_HID_KEYBOARD, keys, sizeof(keys));
    size_t const n_both  = kdrv_ble_hid_report_map(KDRV_BLE_HID_KINDS, both, sizeof(both));

    TEST_ASSERT_GREATER_THAN(MAP_USAGE_AT, n_mouse);
    TEST_ASSERT_GREATER_THAN(MAP_USAGE_AT, n_keys);
    TEST_ASSERT_EQUAL_UINT8(USAGE_MOUSE, mouse[MAP_USAGE_AT]);
    TEST_ASSERT_EQUAL_UINT8(USAGE_KEYBOARD, keys[MAP_USAGE_AT]);
    /* Both: the mouse's collection, then the keyboard's, each whole. */
    TEST_ASSERT_EQUAL(n_mouse + n_keys, n_both);
    TEST_ASSERT_EQUAL_MEMORY(mouse, both, n_mouse);
    TEST_ASSERT_EQUAL_MEMORY(keys, &both[n_mouse], n_keys);
    TEST_ASSERT_EQUAL(0U, kdrv_ble_hid_report_map(0U, both, sizeof(both)));
    TEST_ASSERT_EQUAL(0U, kdrv_ble_hid_report_map(KDRV_BLE_HID_KINDS, both, n_both - 1U));
}

TEST_CASE("each collection names the report ID its characteristic refers to", "[kdrv_ble_hid]")
{
    uint8_t map[KDRV_BLE_HID_MAP_MAX];
    uint8_t ids[2] = { 0U, 0U };
    uint8_t found  = 0U;

    size_t const n = kdrv_ble_hid_report_map(KDRV_BLE_HID_KINDS, map, sizeof(map));

    /* Every item here is one prefix byte and one byte of data. */
    for ( size_t i = 0U; (i + 1U) < n; i += 2U )
    {
        if ( (ITEM_REPORT_ID == map[i]) && (found < 2U) )
        {
            ids[found] = map[i + 1U];
            ++found;
        }
        if ( 0xC0U == map[i] )
        {
            --i;   /* End Collection carries no data */
        }
    }
    TEST_ASSERT_EQUAL_UINT8(2U, found);
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HID_ID_MOUSE, ids[0]);
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HID_ID_KEYBOARD, ids[1]);
}

TEST_CASE("the appearance follows the kinds", "[kdrv_ble_hid]")
{
    TEST_ASSERT_EQUAL_HEX16(KDRV_BLE_HID_APPEAR_MOUSE, kdrv_ble_hid_appearance(KDRV_BLE_HID_MOUSE));
    TEST_ASSERT_EQUAL_HEX16(KDRV_BLE_HID_APPEAR_KEYS, kdrv_ble_hid_appearance(KDRV_BLE_HID_KEYBOARD));
    TEST_ASSERT_EQUAL_HEX16(KDRV_BLE_HID_APPEAR_HID, kdrv_ble_hid_appearance(KDRV_BLE_HID_KINDS));
    TEST_ASSERT_EQUAL_HEX16(0U, kdrv_ble_hid_appearance(0U));
}

TEST_CASE("a host is ready once it subscribed to every kind served", "[kdrv_ble_hid]")
{
    fresh();
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_hid_select(KDRV_BLE_HID_KINDS));
    (void)kdrv_ble_hid_fix();
    TEST_ASSERT_FALSE(kdrv_ble_hid_ready());
    kdrv_ble_hid_subscribed(KDRV_BLE_HID_MOUSE, true);
    TEST_ASSERT_FALSE(kdrv_ble_hid_ready());
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_hid_claim(KDRV_BLE_HID_MOUSE, 1U));
    kdrv_ble_hid_unclaim(KDRV_BLE_HID_MOUSE, 1U);
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_hid_claim(KDRV_BLE_HID_KEYBOARD, 1U));
    kdrv_ble_hid_subscribed(KDRV_BLE_HID_KEYBOARD, true);
    TEST_ASSERT_TRUE(kdrv_ble_hid_ready());
    kdrv_ble_hid_subscribed(KDRV_BLE_HID_MOUSE, false);
    TEST_ASSERT_FALSE(kdrv_ble_hid_ready());
    fresh();
}

TEST_CASE("a kind the table does not serve is never claimed", "[kdrv_ble_hid]")
{
    serving(KDRV_BLE_HID_MOUSE);
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_hid_claim(KDRV_BLE_HID_KEYBOARD, 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_hid_claim(KDRV_BLE_HID_KINDS, 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_hid_claim(0U, 1U));
    fresh();
}

TEST_CASE("reports leave the ring in the order they came", "[kdrv_ble_hid]")
{
    uint8_t  kind = 0U;
    uint8_t  out[KDRV_BLE_HID_REPORT_MAX];
    uint16_t len  = 0U;

    serving(KDRV_BLE_HID_KINDS);
    TEST_ASSERT_FALSE(kdrv_ble_hid_queued());
    TEST_ASSERT_EQUAL(KODE_OK, mouse_send(7U));

    uint8_t const keys[KDRV_BLE_HID_KEYS_LEN] = { 0x02U, 0U, 0x04U, 0U, 0U, 0U, 0U, 0U };

    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_hid_claim(KDRV_BLE_HID_KEYBOARD, 1U));
    kdrv_ble_hid_push(KDRV_BLE_HID_KEYBOARD, keys, sizeof(keys));
    TEST_ASSERT_TRUE(kdrv_ble_hid_queued());

    TEST_ASSERT_TRUE(kdrv_ble_hid_peek(&kind, out, &len));
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HID_MOUSE, kind);
    TEST_ASSERT_EQUAL_UINT16(KDRV_BLE_HID_MOUSE_LEN, len);
    TEST_ASSERT_EQUAL_UINT8(7U, out[1]);
    kdrv_ble_hid_pop(true);

    TEST_ASSERT_TRUE(kdrv_ble_hid_peek(&kind, out, &len));
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HID_KEYBOARD, kind);
    TEST_ASSERT_EQUAL_UINT16(KDRV_BLE_HID_KEYS_LEN, len);
    TEST_ASSERT_EQUAL_MEMORY(keys, out, sizeof(keys));
    kdrv_ble_hid_pop(true);

    TEST_ASSERT_FALSE(kdrv_ble_hid_peek(&kind, out, &len));
    TEST_ASSERT_FALSE(kdrv_ble_hid_queued());
    fresh();
}

TEST_CASE("a full ring refuses the claim and takes it again once a report left", "[kdrv_ble_hid]")
{
    serving(KDRV_BLE_HID_MOUSE);
    for ( uint8_t i = 0U; KDRV_BLE_HID_RING_SLOTS > i; ++i )
    {
        TEST_ASSERT_EQUAL(KODE_OK, mouse_send(i));
    }
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, kdrv_ble_hid_claim(KDRV_BLE_HID_MOUSE, 1U));
    kdrv_ble_hid_pop(true);
    TEST_ASSERT_EQUAL(KODE_OK, mouse_send(99U));
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, kdrv_ble_hid_claim(KDRV_BLE_HID_MOUSE, 1U));
    fresh();
}

TEST_CASE("a claim in flight holds its slot until it is pushed or given back", "[kdrv_ble_hid]")
{
    serving(KDRV_BLE_HID_MOUSE);
    for ( uint8_t i = 0U; KDRV_BLE_HID_RING_SLOTS > i; ++i )
    {
        TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_hid_claim(KDRV_BLE_HID_MOUSE, 1U));
    }
    /* Sixteen in flight, none queued: the ring is spoken for. */
    TEST_ASSERT_FALSE(kdrv_ble_hid_queued());
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, kdrv_ble_hid_claim(KDRV_BLE_HID_MOUSE, 1U));
    kdrv_ble_hid_unclaim(KDRV_BLE_HID_MOUSE, 1U);
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_hid_claim(KDRV_BLE_HID_MOUSE, 1U));
    fresh();
}

TEST_CASE("a link that ends empties the ring and gives its slots back", "[kdrv_ble_hid]")
{
    uint32_t sent    = 0U;
    uint32_t dropped = 0U;

    serving(KDRV_BLE_HID_MOUSE);
    for ( uint8_t i = 0U; KDRV_BLE_HID_RING_SLOTS > i; ++i )
    {
        TEST_ASSERT_EQUAL(KODE_OK, mouse_send(i));
    }
    kdrv_ble_hid_link_reset();
    TEST_ASSERT_FALSE(kdrv_ble_hid_queued());
    TEST_ASSERT_FALSE(kdrv_ble_hid_ready());
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_hid_claim(KDRV_BLE_HID_MOUSE, 1U));
    kdrv_ble_hid_counts(&sent, &dropped);
    TEST_ASSERT_EQUAL_UINT32(0U, sent);
    TEST_ASSERT_EQUAL_UINT32(KDRV_BLE_HID_RING_SLOTS, dropped);

    /* The next host finds the whole ring free. */
    kdrv_ble_hid_subscribed(KDRV_BLE_HID_MOUSE, true);
    for ( uint8_t i = 0U; KDRV_BLE_HID_RING_SLOTS > i; ++i )
    {
        TEST_ASSERT_EQUAL(KODE_OK, mouse_send(i));
    }
    fresh();
}

TEST_CASE("a report claimed before the host left is dropped and frees its slot", "[kdrv_ble_hid]")
{
    uint8_t const rep[KDRV_BLE_HID_MOUSE_LEN] = { 1U, 2U, 3U, 4U };
    uint32_t      dropped = 0U;

    serving(KDRV_BLE_HID_MOUSE);
    for ( uint8_t i = 0U; KDRV_BLE_HID_RING_SLOTS > i; ++i )
    {
        TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_hid_claim(KDRV_BLE_HID_MOUSE, 1U));
    }
    kdrv_ble_hid_link_reset();
    for ( uint8_t i = 0U; KDRV_BLE_HID_RING_SLOTS > i; ++i )
    {
        kdrv_ble_hid_push(KDRV_BLE_HID_MOUSE, rep, sizeof(rep));
    }
    TEST_ASSERT_FALSE(kdrv_ble_hid_queued());
    kdrv_ble_hid_counts(NULL, &dropped);
    TEST_ASSERT_EQUAL_UINT32(KDRV_BLE_HID_RING_SLOTS, dropped);
    kdrv_ble_hid_subscribed(KDRV_BLE_HID_MOUSE, true);
    for ( uint8_t i = 0U; KDRV_BLE_HID_RING_SLOTS > i; ++i )
    {
        TEST_ASSERT_EQUAL(KODE_OK, mouse_send(i));
    }
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, kdrv_ble_hid_claim(KDRV_BLE_HID_MOUSE, 1U));
    fresh();
}

TEST_CASE("a report of the wrong length is dropped, not queued", "[kdrv_ble_hid]")
{
    uint8_t const rep[KDRV_BLE_HID_KEYS_LEN] = { 0U };
    uint32_t      dropped = 0U;

    serving(KDRV_BLE_HID_MOUSE);
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_hid_claim(KDRV_BLE_HID_MOUSE, 1U));
    kdrv_ble_hid_push(KDRV_BLE_HID_MOUSE, rep, sizeof(rep));
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_hid_claim(KDRV_BLE_HID_MOUSE, 1U));
    kdrv_ble_hid_push(KDRV_BLE_HID_MOUSE, NULL, KDRV_BLE_HID_MOUSE_LEN);
    TEST_ASSERT_FALSE(kdrv_ble_hid_queued());
    kdrv_ble_hid_counts(NULL, &dropped);
    TEST_ASSERT_EQUAL_UINT32(2U, dropped);
    fresh();
}

TEST_CASE("a read of an input report gets the last one the link took", "[kdrv_ble_hid]")
{
    uint8_t out[KDRV_BLE_HID_REPORT_MAX];
    uint8_t const zero[KDRV_BLE_HID_REPORT_MAX] = { 0U };

    serving(KDRV_BLE_HID_KINDS);
    TEST_ASSERT_EQUAL_UINT16(KDRV_BLE_HID_MOUSE_LEN, kdrv_ble_hid_last(KDRV_BLE_HID_MOUSE, out));
    TEST_ASSERT_EQUAL_MEMORY(zero, out, KDRV_BLE_HID_MOUSE_LEN);
    TEST_ASSERT_EQUAL(KODE_OK, mouse_send(5U));
    kdrv_ble_hid_pop(true);
    TEST_ASSERT_EQUAL(KODE_OK, mouse_send(9U));
    kdrv_ble_hid_pop(false);
    TEST_ASSERT_EQUAL_UINT16(KDRV_BLE_HID_MOUSE_LEN, kdrv_ble_hid_last(KDRV_BLE_HID_MOUSE, out));
    TEST_ASSERT_EQUAL_UINT8(5U, out[1]);
    TEST_ASSERT_EQUAL_UINT16(KDRV_BLE_HID_KEYS_LEN, kdrv_ble_hid_last(KDRV_BLE_HID_KEYBOARD, out));
    TEST_ASSERT_EQUAL_MEMORY(zero, out, KDRV_BLE_HID_KEYS_LEN);
    TEST_ASSERT_EQUAL_UINT16(0U, kdrv_ble_hid_last(KDRV_BLE_HID_KINDS, out));
    /* The next host starts from zeroes. */
    kdrv_ble_hid_link_reset();
    TEST_ASSERT_EQUAL_UINT16(KDRV_BLE_HID_MOUSE_LEN, kdrv_ble_hid_last(KDRV_BLE_HID_MOUSE, out));
    TEST_ASSERT_EQUAL_MEMORY(zero, out, KDRV_BLE_HID_MOUSE_LEN);
    fresh();
}

TEST_CASE("the counts tell the reports sent from the reports dropped", "[kdrv_ble_hid]")
{
    uint32_t sent    = 0U;
    uint32_t dropped = 0U;

    serving(KDRV_BLE_HID_MOUSE);
    TEST_ASSERT_EQUAL(KODE_OK, mouse_send(1U));
    TEST_ASSERT_EQUAL(KODE_OK, mouse_send(2U));
    TEST_ASSERT_EQUAL(KODE_OK, mouse_send(3U));
    kdrv_ble_hid_pop(true);
    kdrv_ble_hid_pop(true);
    kdrv_ble_hid_pop(false);
    kdrv_ble_hid_counts(&sent, &dropped);
    TEST_ASSERT_EQUAL_UINT32(2U, sent);
    TEST_ASSERT_EQUAL_UINT32(1U, dropped);
    fresh();
}

TEST_CASE("the keyboard LEDs a host wrote are kept until the link ends", "[kdrv_ble_hid]")
{
    serving(KDRV_BLE_HID_KEYBOARD);
    TEST_ASSERT_EQUAL_UINT8(0U, kdrv_ble_hid_leds());
    kdrv_ble_hid_leds_set(0x02U);
    TEST_ASSERT_EQUAL_UINT8(0x02U, kdrv_ble_hid_leds());
    kdrv_ble_hid_link_reset();
    TEST_ASSERT_EQUAL_UINT8(0U, kdrv_ble_hid_leds());
    fresh();
}

TEST_CASE("a new device pairs in an app only while the time runs and HID is served", "[kdrv_ble_hid]")
{
    fresh();
    kdrv_ble_hid_pair_window(true);
    TEST_ASSERT_FALSE(kdrv_ble_hid_pair_open());   /* no HID: an app pairs nobody */
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_hid_select(KDRV_BLE_HID_MOUSE));
    TEST_ASSERT_FALSE(kdrv_ble_hid_pair_open());   /* chosen, not served yet */
    (void)kdrv_ble_hid_fix();
    TEST_ASSERT_TRUE(kdrv_ble_hid_pair_open());
    kdrv_ble_hid_pair_window(false);
    TEST_ASSERT_FALSE(kdrv_ble_hid_pair_open());
    fresh();
}

TEST_CASE("a table's print moves with its bytes and their order", "[kdrv_ble_hid]")
{
    uint8_t const a[] = { 0x12U, 0x18U };
    uint8_t const b[] = { 0x0FU, 0x18U };

    uint32_t const ab = kdrv_ble_hid_print_add(
        kdrv_ble_hid_print_add(KDRV_BLE_HID_PRINT_SEED, a, sizeof(a)), b, sizeof(b));
    uint32_t const ba = kdrv_ble_hid_print_add(
        kdrv_ble_hid_print_add(KDRV_BLE_HID_PRINT_SEED, b, sizeof(b)), a, sizeof(a));
    uint32_t const again = kdrv_ble_hid_print_add(
        kdrv_ble_hid_print_add(KDRV_BLE_HID_PRINT_SEED, a, sizeof(a)), b, sizeof(b));

    TEST_ASSERT_EQUAL_HEX32(ab, again);
    TEST_ASSERT_NOT_EQUAL(ab, ba);
    TEST_ASSERT_NOT_EQUAL(KDRV_BLE_HID_PRINT_SEED, ab);
    TEST_ASSERT_EQUAL_HEX32(KDRV_BLE_HID_PRINT_SEED,
                            kdrv_ble_hid_print_add(KDRV_BLE_HID_PRINT_SEED, NULL, 0U));
}

TEST_CASE("a table without HID has no print, and one with HID never has that one", "[kdrv_ble_hid]")
{
    TEST_ASSERT_EQUAL_HEX32(KDRV_BLE_HID_PRINT_NONE, kdrv_ble_hid_print_end(0x1234U, false));
    TEST_ASSERT_EQUAL_HEX32(0x1234U, kdrv_ble_hid_print_end(0x1234U, true));
    TEST_ASSERT_NOT_EQUAL(KDRV_BLE_HID_PRINT_NONE,
                          kdrv_ble_hid_print_end(KDRV_BLE_HID_PRINT_NONE, true));
}

TEST_CASE("a mouse report is its buttons and three steps", "[kdrv_ble_hid]")
{
    uint8_t out[KDRV_BLE_HID_MOUSE_LEN];

    kdrv_ble_hid_mouse_pack(0xFFU, 5, -5, 127, out);
    TEST_ASSERT_EQUAL_HEX8(KDRV_BLE_HID_BUTTONS, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x05U, out[1]);
    TEST_ASSERT_EQUAL_HEX8(0xFBU, out[2]);
    TEST_ASSERT_EQUAL_HEX8(0x7FU, out[3]);
    /* -128 is outside what the map allows: the report says -127. */
    kdrv_ble_hid_mouse_pack(0x01U, INT8_MIN, 0, INT8_MIN, out);
    TEST_ASSERT_EQUAL_HEX8(0x01U, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x81U, out[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, out[2]);
    TEST_ASSERT_EQUAL_HEX8(0x81U, out[3]);
}

TEST_CASE("a keyboard report is its modifiers, a zero and six keys", "[kdrv_ble_hid]")
{
    uint8_t const keys[KDRV_BLE_HID_KEYS_MAX] = { 0x04U, 0x05U, 0U, 0U, 0U, 0x06U };
    uint8_t const want[KDRV_BLE_HID_KEYS_LEN] = { 0x02U, 0U, 0x04U, 0x05U, 0U, 0U, 0U, 0x06U };
    uint8_t const none[KDRV_BLE_HID_KEYS_LEN] = { 0U };
    uint8_t       out[KDRV_BLE_HID_KEYS_LEN];

    kdrv_ble_hid_keys_pack(0x02U, keys, out);
    TEST_ASSERT_EQUAL_MEMORY(want, out, sizeof(out));
    /* No keys at all is every key up. */
    kdrv_ble_hid_keys_pack(0U, NULL, out);
    TEST_ASSERT_EQUAL_MEMORY(none, out, sizeof(out));
}

TEST_CASE("several slots are claimed together or not at all", "[kdrv_ble_hid]")
{
    serving(KDRV_BLE_HID_KEYBOARD);
    for ( uint8_t i = 0U; (KDRV_BLE_HID_RING_SLOTS - 1U) > i; ++i )
    {
        TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_hid_claim(KDRV_BLE_HID_KEYBOARD, 1U));
    }
    /* One slot is left: a key pressed and let go needs two. */
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, kdrv_ble_hid_claim(KDRV_BLE_HID_KEYBOARD, 2U));
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_hid_claim(KDRV_BLE_HID_KEYBOARD, 1U));
    kdrv_ble_hid_unclaim(KDRV_BLE_HID_KEYBOARD, 2U);
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_hid_claim(KDRV_BLE_HID_KEYBOARD, 2U));
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_hid_claim(KDRV_BLE_HID_KEYBOARD, 0U));
    TEST_ASSERT_EQUAL(KODE_DENIED,
                      kdrv_ble_hid_claim(KDRV_BLE_HID_KEYBOARD, (uint8_t)(KDRV_BLE_HID_RING_SLOTS + 1U)));
    fresh();
}

/* ---- The report of zeroes, which is never refused for want of room ----------- */

TEST_CASE("an owed report of zeroes goes once its kind's reports have gone, with no slot of its own",
          "[kdrv_ble_hid]")
{
    static const uint8_t ZERO[KDRV_BLE_HID_REPORT_MAX] = { 0U };
    uint8_t  kind = 0U;
    uint8_t  out[KDRV_BLE_HID_REPORT_MAX];
    uint16_t len  = 0U;

    serving(KDRV_BLE_HID_KINDS);
    for ( uint8_t i = 0U; KDRV_BLE_HID_RING_SLOTS > i; ++i )
    {
        TEST_ASSERT_EQUAL(KODE_OK, mouse_send((uint8_t)(i + 1U)));
    }
    /* The ring is full of the mouse's: letting its buttons go is still taken. */
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, kdrv_ble_hid_claim(KDRV_BLE_HID_MOUSE, 1U));
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_hid_owe(KDRV_BLE_HID_MOUSE));
    /* The reports ahead of it go first, in their order. */
    for ( uint8_t i = 0U; KDRV_BLE_HID_RING_SLOTS > i; ++i )
    {
        TEST_ASSERT_TRUE(kdrv_ble_hid_peek(&kind, out, &len));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(i + 1U), out[1]);
        kdrv_ble_hid_pop(true);
    }
    TEST_ASSERT_TRUE_MESSAGE(kdrv_ble_hid_queued(), "the owed report was forgotten");
    TEST_ASSERT_TRUE(kdrv_ble_hid_peek(&kind, out, &len));
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HID_MOUSE, kind);
    TEST_ASSERT_EQUAL_UINT16(KDRV_BLE_HID_MOUSE_LEN, len);
    TEST_ASSERT_EQUAL_MEMORY(ZERO, out, KDRV_BLE_HID_MOUSE_LEN);
    kdrv_ble_hid_pop(true);
    TEST_ASSERT_FALSE(kdrv_ble_hid_queued());
    /* It held no slot: the whole ring is free again. */
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_hid_claim(KDRV_BLE_HID_MOUSE, (uint8_t)KDRV_BLE_HID_RING_SLOTS));
    fresh();
}

TEST_CASE("a kind that owes its report of zeroes takes nothing behind it; the other kind goes on",
          "[kdrv_ble_hid]")
{
    uint8_t  kind = 0U;
    uint8_t  out[KDRV_BLE_HID_REPORT_MAX];
    uint16_t len  = 0U;

    serving(KDRV_BLE_HID_KINDS);
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_hid_claim(KDRV_BLE_HID_KEYBOARD, 1U));   /* a key, on its way */
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_hid_owe(KDRV_BLE_HID_KEYBOARD));
    /* A second press would overtake the letting go: refused until that has gone. */
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, kdrv_ble_hid_claim(KDRV_BLE_HID_KEYBOARD, 1U));
    TEST_ASSERT_EQUAL(KODE_OK, mouse_send(3U));
    /* The key's press has not arrived yet: the mouse goes, the zeroes wait. */
    TEST_ASSERT_TRUE(kdrv_ble_hid_peek(&kind, out, &len));
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HID_MOUSE, kind);
    kdrv_ble_hid_pop(true);
    TEST_ASSERT_FALSE(kdrv_ble_hid_queued());

    uint8_t const press[KDRV_BLE_HID_KEYS_LEN] = { 0U, 0U, 0x04U, 0U, 0U, 0U, 0U, 0U };

    kdrv_ble_hid_push(KDRV_BLE_HID_KEYBOARD, press, sizeof(press));
    TEST_ASSERT_TRUE(kdrv_ble_hid_peek(&kind, out, &len));
    TEST_ASSERT_EQUAL_HEX8(0x04U, out[KDRV_BLE_HID_KEYS_AT]);
    kdrv_ble_hid_pop(true);
    TEST_ASSERT_TRUE(kdrv_ble_hid_peek(&kind, out, &len));
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HID_KEYBOARD, kind);
    TEST_ASSERT_EQUAL_HEX8(0x00U, out[KDRV_BLE_HID_KEYS_AT]);
    kdrv_ble_hid_pop(true);
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_hid_claim(KDRV_BLE_HID_KEYBOARD, 1U));
    fresh();
}

TEST_CASE("nothing is owed to a host that is not there, and a link's end forgets what was",
          "[kdrv_ble_hid]")
{
    fresh();
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_hid_owe(KDRV_BLE_HID_MOUSE));
    serving(KDRV_BLE_HID_MOUSE);
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_hid_owe(KDRV_BLE_HID_KEYBOARD));
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ble_hid_owe(KDRV_BLE_HID_MOUSE));
    TEST_ASSERT_TRUE(kdrv_ble_hid_queued());
    kdrv_ble_hid_link_reset();
    TEST_ASSERT_FALSE(kdrv_ble_hid_queued());
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_ble_hid_owe(KDRV_BLE_HID_MOUSE));
    fresh();
}

TEST_CASE("the keyboard's map declares its keys up to the last one a caller may press",
          "[kdrv_ble_hid]")
{
    uint8_t map[KDRV_BLE_HID_MAP_MAX];
    size_t const n = kdrv_ble_hid_report_map(KDRV_BLE_HID_KEYBOARD, map, sizeof(map));
    bool   found   = false;

    /* Usage Maximum (0x29) of the keys' array, the last item of its kind in the map. */
    for ( size_t i = 0U; (i + 1U) < n; i += 2U )
    {
        if ( 0x29U == map[i] )
        {
            found = (KDRV_BLE_HID_KEY_MAX == map[i + 1U]);
        }
    }
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_UINT(101U, KDRV_BLE_HID_KEY_MAX);
}

/* ---- The forgetting of stored subscriptions ----------------------------------- */

TEST_CASE("a board that never serves HID forgets no subscription", "[kdrv_ble_hid]")
{
    uint32_t const none = kdrv_ble_hid_print_end(0xABCD1234U, false);

    /* Whatever its table is, and whatever the last boot's was, without HID. */
    TEST_ASSERT_FALSE(kdrv_ble_hid_prune_due(KDRV_BLE_HID_PRINT_NONE, none));
    TEST_ASSERT_FALSE(kdrv_ble_hid_prune_due(kdrv_ble_hid_print_end(0x11U, false),
                                             kdrv_ble_hid_print_end(0x22U, false)));
}

TEST_CASE("subscriptions are forgotten when HID comes, goes or moves, and kept on the same table",
          "[kdrv_ble_hid]")
{
    uint32_t const a = kdrv_ble_hid_print_end(0x1111U, true);
    uint32_t const b = kdrv_ble_hid_print_end(0x2222U, true);

    TEST_ASSERT_TRUE(kdrv_ble_hid_prune_due(KDRV_BLE_HID_PRINT_NONE, a));
    TEST_ASSERT_TRUE(kdrv_ble_hid_prune_due(a, KDRV_BLE_HID_PRINT_NONE));
    TEST_ASSERT_TRUE(kdrv_ble_hid_prune_due(a, b));
    TEST_ASSERT_FALSE_MESSAGE(kdrv_ble_hid_prune_due(a, a),
                              "a host would have to subscribe again at every boot");
}

TEST_CASE("the forgetting takes every subscription but Service Changed's", "[kdrv_ble_hid]")
{
    TEST_ASSERT_FALSE(kdrv_ble_hid_prune_takes(0x0008U, 0x0008U));
    TEST_ASSERT_TRUE(kdrv_ble_hid_prune_takes(0x0012U, 0x0008U));
    /* A table with no Service Changed keeps none. */
    TEST_ASSERT_TRUE(kdrv_ble_hid_prune_takes(0x0012U, 0U));
}

/* ---- The hosts of the HID service ---------------------------------------------- */

/** @brief Three devices' identity addresses, and a fourth. */
static const uint8_t HOST_A[KDRV_BLE_HID_ADDR_LEN] = { 0U, 1U, 1U, 1U, 1U, 1U, 1U };
static const uint8_t HOST_B[KDRV_BLE_HID_ADDR_LEN] = { 1U, 2U, 2U, 2U, 2U, 2U, 2U };
static const uint8_t HOST_C[KDRV_BLE_HID_ADDR_LEN] = { 0U, 3U, 3U, 3U, 3U, 3U, 3U };
static const uint8_t HOST_D[KDRV_BLE_HID_ADDR_LEN] = { 1U, 4U, 4U, 4U, 4U, 4U, 4U };

TEST_CASE("a host is remembered once, and the oldest gives its place to a fourth",
          "[kdrv_ble_hid]")
{
    kdrv_ble_hid_hosts_t hosts;

    (void)memset(&hosts, 0, sizeof(hosts));
    TEST_ASSERT_FALSE(kdrv_ble_hid_hosts_has(&hosts, HOST_A));
    TEST_ASSERT_TRUE(kdrv_ble_hid_hosts_add(&hosts, HOST_A));
    TEST_ASSERT_FALSE_MESSAGE(kdrv_ble_hid_hosts_add(&hosts, HOST_A),
                              "a host that comes back would be stored again at every link");
    TEST_ASSERT_TRUE(kdrv_ble_hid_hosts_add(&hosts, HOST_B));
    TEST_ASSERT_TRUE(kdrv_ble_hid_hosts_add(&hosts, HOST_C));
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HID_HOSTS_MAX, hosts.count);
    TEST_ASSERT_TRUE(kdrv_ble_hid_hosts_add(&hosts, HOST_D));
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HID_HOSTS_MAX, hosts.count);
    TEST_ASSERT_FALSE(kdrv_ble_hid_hosts_has(&hosts, HOST_A));
    TEST_ASSERT_TRUE(kdrv_ble_hid_hosts_has(&hosts, HOST_B));
    TEST_ASSERT_TRUE(kdrv_ble_hid_hosts_has(&hosts, HOST_D));
    /* The same six bytes under the other address type are another device. */
    uint8_t other[KDRV_BLE_HID_ADDR_LEN];

    (void)memcpy(other, HOST_B, sizeof(other));
    other[0] = 0U;
    TEST_ASSERT_FALSE(kdrv_ble_hid_hosts_has(&hosts, other));
}

TEST_CASE("a host whose bond went is forgotten, and the others keep their order",
          "[kdrv_ble_hid]")
{
    kdrv_ble_hid_hosts_t hosts;

    (void)memset(&hosts, 0, sizeof(hosts));
    (void)kdrv_ble_hid_hosts_add(&hosts, HOST_A);
    (void)kdrv_ble_hid_hosts_add(&hosts, HOST_B);
    (void)kdrv_ble_hid_hosts_add(&hosts, HOST_C);
    TEST_ASSERT_TRUE(kdrv_ble_hid_hosts_drop(&hosts, HOST_B));
    TEST_ASSERT_FALSE(kdrv_ble_hid_hosts_drop(&hosts, HOST_B));
    TEST_ASSERT_EQUAL_UINT8(2U, hosts.count);
    TEST_ASSERT_EQUAL_MEMORY(HOST_A, hosts.addr[0], KDRV_BLE_HID_ADDR_LEN);
    TEST_ASSERT_EQUAL_MEMORY(HOST_C, hosts.addr[1], KDRV_BLE_HID_ADDR_LEN);
}

TEST_CASE("a record of hosts of another size or with too many is not taken", "[kdrv_ble_hid]")
{
    kdrv_ble_hid_hosts_t hosts;

    (void)memset(&hosts, 0, sizeof(hosts));
    TEST_ASSERT_TRUE(kdrv_ble_hid_hosts_ok(&hosts, sizeof(hosts)));
    TEST_ASSERT_FALSE(kdrv_ble_hid_hosts_ok(&hosts, sizeof(hosts) - 1U));
    hosts.count = (uint8_t)(KDRV_BLE_HID_HOSTS_MAX + 1U);
    TEST_ASSERT_FALSE(kdrv_ble_hid_hosts_ok(&hosts, sizeof(hosts)));
    TEST_ASSERT_FALSE(kdrv_ble_hid_hosts_ok(NULL, sizeof(hosts)));
}

TEST_CASE("a device with a HID bond that comes back while no HID is served is hung up on; "
          "any other stays", "[kdrv_ble_hid]")
{
    kdrv_ble_hid_hosts_t hosts;

    (void)memset(&hosts, 0, sizeof(hosts));
    (void)kdrv_ble_hid_hosts_add(&hosts, HOST_A);
    /* HID off: the computer that was its mouse's host would hold the one link. */
    TEST_ASSERT_EQUAL(KDRV_BLE_HID_HOST_HANG_UP, kdrv_ble_hid_host_judge(&hosts, HOST_A, false));
    /* The phone bonded for the console is left alone, HID or not. */
    TEST_ASSERT_EQUAL(KDRV_BLE_HID_HOST_STAY, kdrv_ble_hid_host_judge(&hosts, HOST_B, false));
    TEST_ASSERT_EQUAL(KDRV_BLE_HID_HOST_STAY, kdrv_ble_hid_host_judge(&hosts, HOST_B, true));
    /* With HID served a host is what the board is for. */
    TEST_ASSERT_EQUAL(KDRV_BLE_HID_HOST_STAY, kdrv_ble_hid_host_judge(&hosts, HOST_A, true));
    /* A board with no HID bond judges nobody. */
    (void)memset(&hosts, 0, sizeof(hosts));
    TEST_ASSERT_EQUAL(KDRV_BLE_HID_HOST_STAY, kdrv_ble_hid_host_judge(&hosts, HOST_A, false));
}

/* ---- What a bond was made for ---------------------------------------------------- */

TEST_CASE("a bond written while the board serves HID is a HID bond; any other is the console's",
          "[kdrv_ble_hid][bond]")
{
    kdrv_ble_hid_hosts_t marks;

    (void)memset(&marks, 0, sizeof(marks));
    /* The computer pairs with the board that is a mouse. */
    TEST_ASSERT_TRUE(kdrv_ble_hid_bond_note(&marks, HOST_A, true));
    TEST_ASSERT_TRUE(kdrv_ble_hid_hosts_has(&marks, HOST_A));
    /* The phone pairs with the board that is not: nothing to mark, nothing to store. */
    TEST_ASSERT_FALSE(kdrv_ble_hid_bond_note(&marks, HOST_B, false));
    TEST_ASSERT_FALSE(kdrv_ble_hid_hosts_has(&marks, HOST_B));
    /* A bond every boot made before the mark existed is the console's. */
    TEST_ASSERT_FALSE(kdrv_ble_hid_hosts_has(&marks, HOST_C));
}

TEST_CASE("a HID bond gets no console: its reads, writes and subscriptions there are refused",
          "[kdrv_ble_hid][bond]")
{
    kdrv_ble_hid_hosts_t marks;

    (void)memset(&marks, 0, sizeof(marks));
    (void)kdrv_ble_hid_bond_note(&marks, HOST_A, true);

    bool const hid_bond = kdrv_ble_hid_hosts_has(&marks, HOST_A);

    TEST_ASSERT_FALSE_MESSAGE(kdrv_ble_hid_access_ok(false, hid_bond),
                              "a computer paired as a mouse reached the console");
    TEST_ASSERT_TRUE(kdrv_ble_hid_access_ok(true, hid_bond));
}

TEST_CASE("a console bond gets no reports: the HID service is refused to it",
          "[kdrv_ble_hid][bond]")
{
    kdrv_ble_hid_hosts_t marks;

    (void)memset(&marks, 0, sizeof(marks));
    (void)kdrv_ble_hid_bond_note(&marks, HOST_A, true);    /* some computer, a host */
    (void)kdrv_ble_hid_bond_note(&marks, HOST_B, false);   /* the phone, for the console */

    bool const hid_bond = kdrv_ble_hid_hosts_has(&marks, HOST_B);

    TEST_ASSERT_FALSE_MESSAGE(kdrv_ble_hid_access_ok(true, hid_bond),
                              "an app could type at the phone paired for the console");
    TEST_ASSERT_TRUE(kdrv_ble_hid_access_ok(false, hid_bond));
}

TEST_CASE("a device that pairs again while no HID is served has a console bond from then on",
          "[kdrv_ble_hid][bond]")
{
    kdrv_ble_hid_hosts_t marks;

    (void)memset(&marks, 0, sizeof(marks));
    (void)kdrv_ble_hid_bond_note(&marks, HOST_A, true);
    TEST_ASSERT_FALSE(kdrv_ble_hid_access_ok(false, kdrv_ble_hid_hosts_has(&marks, HOST_A)));
    /* Forgotten on the computer and paired anew with HID off: the console's. */
    TEST_ASSERT_TRUE(kdrv_ble_hid_bond_note(&marks, HOST_A, false));
    TEST_ASSERT_TRUE(kdrv_ble_hid_access_ok(false, kdrv_ble_hid_hosts_has(&marks, HOST_A)));
    TEST_ASSERT_EQUAL(KDRV_BLE_HID_HOST_STAY, kdrv_ble_hid_host_judge(&marks, HOST_A, false));
    /* And the other way: paired again while the board is a mouse, it is a host. */
    TEST_ASSERT_TRUE(kdrv_ble_hid_bond_note(&marks, HOST_A, true));
    TEST_ASSERT_FALSE(kdrv_ble_hid_access_ok(false, kdrv_ble_hid_hosts_has(&marks, HOST_A)));
}

TEST_CASE("the mark of a bond survives a reboot", "[kdrv_ble_hid][bond]")
{
    kdrv_ble_hid_hosts_t marks;
    kdrv_ble_hid_hosts_t after;

    fake_nvs_reset();
    (void)memset(&marks, 0, sizeof(marks));
    (void)kdrv_ble_hid_bond_note(&marks, HOST_A, true);
    (void)kdrv_ble_hid_bond_note(&marks, HOST_D, true);
    kdrv_ble_hid_hosts_store(&marks);

    /* The next boot starts from nothing but the store. */
    (void)memset(&after, 0xA5, sizeof(after));
    kdrv_ble_hid_hosts_load(&after);
    TEST_ASSERT_EQUAL_UINT8(2U, after.count);
    TEST_ASSERT_TRUE(kdrv_ble_hid_hosts_has(&after, HOST_A));
    TEST_ASSERT_TRUE(kdrv_ble_hid_hosts_has(&after, HOST_D));
    TEST_ASSERT_FALSE(kdrv_ble_hid_hosts_has(&after, HOST_B));
    TEST_ASSERT_FALSE_MESSAGE(kdrv_ble_hid_access_ok(false, kdrv_ble_hid_hosts_has(&after, HOST_A)),
                              "a reboot let the mouse's host into the console");

    /* The last HID bond gone leaves no record, and a board with none reads none. */
    (void)kdrv_ble_hid_hosts_drop(&after, HOST_A);
    (void)kdrv_ble_hid_hosts_drop(&after, HOST_D);
    kdrv_ble_hid_hosts_store(&after);
    (void)memset(&after, 0xA5, sizeof(after));
    kdrv_ble_hid_hosts_load(&after);
    TEST_ASSERT_EQUAL_UINT8(0U, after.count);
    fake_nvs_reset();
}

TEST_CASE("a record of marks this build did not write is read as none", "[kdrv_ble_hid][bond]")
{
    static const uint8_t JUNK[5] = { 9U, 1U, 2U, 3U, 4U };
    kdrv_ble_hid_hosts_t marks;

    fake_nvs_reset();
    fake_nvs_seed(KDRV_BLE_HID_NVS_NS, KDRV_BLE_HID_NVS_HOSTS, JUNK, sizeof(JUNK));
    (void)memset(&marks, 0xA5, sizeof(marks));
    kdrv_ble_hid_hosts_load(&marks);
    TEST_ASSERT_EQUAL_UINT8(0U, marks.count);
    fake_nvs_reset();
}

/** @brief kdrv_ble_hid_bonded_t: every device has a bond but the one @p ctx names. */
static bool bonded_but(const uint8_t *addr, void *ctx)
{
    return memcmp(addr, ctx, KDRV_BLE_HID_ADDR_LEN) != 0;
}

TEST_CASE("a mark whose bond is gone from the store is dropped at the next start",
          "[kdrv_ble_hid][bond]")
{
    kdrv_ble_hid_hosts_t marks;
    uint8_t              gone[KDRV_BLE_HID_ADDR_LEN];

    (void)memset(&marks, 0, sizeof(marks));
    (void)kdrv_ble_hid_bond_note(&marks, HOST_A, true);
    (void)kdrv_ble_hid_bond_note(&marks, HOST_B, true);
    (void)kdrv_ble_hid_bond_note(&marks, HOST_C, true);
    (void)memcpy(gone, HOST_B, sizeof(gone));
    TEST_ASSERT_TRUE(kdrv_ble_hid_hosts_sift(&marks, &bonded_but, gone));
    TEST_ASSERT_EQUAL_UINT8(2U, marks.count);
    TEST_ASSERT_TRUE(kdrv_ble_hid_hosts_has(&marks, HOST_A));
    TEST_ASSERT_FALSE(kdrv_ble_hid_hosts_has(&marks, HOST_B));
    TEST_ASSERT_TRUE(kdrv_ble_hid_hosts_has(&marks, HOST_C));
    /* Nothing more to drop: nothing to store. */
    TEST_ASSERT_FALSE(kdrv_ble_hid_hosts_sift(&marks, &bonded_but, gone));
}
