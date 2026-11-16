/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_ble_bonds.c
 * @brief The count of bonded peers the driver keeps: every order of a count made outside the lock and the changes of the store.
 */
#include <stdio.h>

#include "unity.h"

#include "kdrv_ble_bonds.h"

/** @brief The changes of the store a case makes. */
#define BONDS_CHANGES      3U

/** @brief The steps of a case: two for each change, three for the count made outside the lock. */
#define BONDS_STEPS        ((2U * BONDS_CHANGES) + 3U)

/** @brief The fewest orders the lock allows; fewer is a case that ran nothing. */
#define BONDS_ORDERS_MIN   40U

/** @brief A step of the count made outside the lock: the ticket. */
#define STEP_BEGIN         0U
/** @brief A step of the count made outside the lock: the peers are counted. */
#define STEP_COUNT         1U
/** @brief A step of the count made outside the lock: the count is handed in. */
#define STEP_SETTLE        2U

/** @brief A count made of a store that has changed since: never kept. */
#define BONDS_OLD_COUNT    3U

/** @brief What the store holds after each change of a case, in turn. */
static const uint8_t s_after[BONDS_CHANGES] = { 1U, 2U, 1U };

/** @brief Put the keeper where a start leaves it: @p n peers, counted with the store still. */
static void keeper_at(uint8_t n)
{
    (void)kdrv_ble_bonds_settle(kdrv_ble_bonds_begin(), n);
    TEST_ASSERT_EQUAL_UINT8(n, kdrv_ble_bonds_kept());
}

TEST_CASE("a change of the store is kept and says whether the count moved", "[ble_bonds]")
{
    keeper_at(0U);
    TEST_ASSERT_TRUE(kdrv_ble_bonds_changed(1U));
    TEST_ASSERT_EQUAL_UINT8(1U, kdrv_ble_bonds_kept());
    TEST_ASSERT_FALSE_MESSAGE(kdrv_ble_bonds_changed(1U), "a key written over a peer's was said to move the count");
    TEST_ASSERT_TRUE(kdrv_ble_bonds_changed(0U));
    TEST_ASSERT_EQUAL_UINT8(0U, kdrv_ble_bonds_kept());
}

TEST_CASE("a count made with the store still is kept and says whether it moved", "[ble_bonds]")
{
    keeper_at(0U);
    uint32_t const first = kdrv_ble_bonds_begin();
    TEST_ASSERT_TRUE(kdrv_ble_bonds_settle(first, 2U));
    TEST_ASSERT_EQUAL_UINT8(2U, kdrv_ble_bonds_kept());
    uint32_t const again = kdrv_ble_bonds_begin();
    TEST_ASSERT_FALSE(kdrv_ble_bonds_settle(again, 2U));
    TEST_ASSERT_EQUAL_UINT8(2U, kdrv_ble_bonds_kept());
}

TEST_CASE("a count begun before a change never replaces the count the change left", "[ble_bonds]")
{
    keeper_at(1U);
    uint32_t const ticket = kdrv_ble_bonds_begin();   /* the count sees one peer... */
    TEST_ASSERT_TRUE(kdrv_ble_bonds_changed(0U));     /* ...and the peer is unpaired */
    TEST_ASSERT_FALSE_MESSAGE(kdrv_ble_bonds_settle(ticket, 1U), "the old count was kept");
    TEST_ASSERT_EQUAL_UINT8(0U, kdrv_ble_bonds_kept());

    /* The same count as the one kept, after a change that left it as it was: still old. */
    uint32_t const second = kdrv_ble_bonds_begin();
    TEST_ASSERT_FALSE(kdrv_ble_bonds_changed(0U));
    TEST_ASSERT_FALSE(kdrv_ble_bonds_settle(second, BONDS_OLD_COUNT));
    TEST_ASSERT_EQUAL_UINT8(0U, kdrv_ble_bonds_kept());
}

/**
 * @brief Run one order of a case's steps and judge what is kept at its end.
 *
 * A change is two steps, the store's write and the count kept for it, both
 * inside one hold of the lock: the count made outside the lock reads the
 * store before the write or after the count is kept, never between, but its
 * ticket and its hand-in take no lock and fall anywhere.
 *
 * @param order For each step, STEP_BEGIN, STEP_COUNT or STEP_SETTLE, or a
 *              value past them for the next step of the changes.
 * @return false when the order is one the lock does not allow.
 */
static bool order_run(const uint8_t order[BONDS_STEPS])
{
    uint8_t  truth   = 0U;      /* what the store holds */
    uint8_t  counted = 0U;      /* what the count outside the lock saw */
    uint32_t ticket  = 0U;
    uint32_t change  = 0U;      /* steps of the changes taken so far */
    bool     allowed = true;

    keeper_at(0U);
    for ( uint32_t i = 0U; (BONDS_STEPS > i) && allowed; ++i )
    {
        uint8_t const before = kdrv_ble_bonds_kept();
        bool          moved  = false;
        bool          told   = false;

        if ( STEP_BEGIN == order[i] )
        {
            ticket = kdrv_ble_bonds_begin();
        }
        else if ( STEP_COUNT == order[i] )
        {
            /* Inside a change's hold of the lock the store cannot be read. */
            allowed = 0U == (change % 2U);
            counted = truth;
        }
        else if ( STEP_SETTLE == order[i] )
        {
            moved = kdrv_ble_bonds_settle(ticket, counted);
            told  = true;
        }
        else if ( 0U == (change % 2U) )
        {
            truth = s_after[change / 2U];
            change++;
        }
        else
        {
            moved = kdrv_ble_bonds_changed(truth);
            told  = true;
            change++;
        }
        if ( told && allowed )
        {
            TEST_ASSERT_EQUAL_MESSAGE(before != kdrv_ble_bonds_kept(), moved,
                                      "the count kept moved and nobody was told, or stood and somebody was");
        }
    }
    if ( allowed )
    {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(truth, kdrv_ble_bonds_kept(),
                                        "the count kept is not what the store holds");
    }
    return allowed;
}

TEST_CASE("in every order of a count made outside the lock and three changes the count kept is the store's",
          "[ble_bonds][orders]")
{
    unsigned orders = 0U;

    /* The three steps of the count take places a < b < c among the steps. */
    for ( uint32_t a = 0U; BONDS_STEPS > a; ++a )
    {
        for ( uint32_t b = a + 1U; BONDS_STEPS > b; ++b )
        {
            for ( uint32_t c = b + 1U; BONDS_STEPS > c; ++c )
            {
                uint8_t order[BONDS_STEPS];

                for ( uint32_t i = 0U; BONDS_STEPS > i; ++i )
                {
                    order[i] = STEP_SETTLE + 1U;
                }
                order[a] = STEP_BEGIN;
                order[b] = STEP_COUNT;
                order[c] = STEP_SETTLE;
                if ( order_run(order) )
                {
                    ++orders;
                }
            }
        }
    }
    (void)printf("  %u orders run\n", orders);
    TEST_ASSERT_TRUE_MESSAGE(BONDS_ORDERS_MIN < orders, "the orders were not run");
}
