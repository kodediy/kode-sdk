/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kbsp_owners.c
 * @brief The ownership table, which is the whole of invariant 1.
 */
#include "unity.h"

#include "kbsp_owners.h"

/* Owner tokens. Any stable address will do; distinct objects so that "the
 * same owner" and "a different owner" are distinguishable. */
static int s_alice;
static int s_bob;

/**
 * @brief Hand back everything either token might still be holding.
 *
 * Unity abandons a case at the first failed assertion, so a case that fails
 * before its own release would leave a resource held and the failure would
 * spread. Releasing is safe on a resource nobody holds.
 */
static void owners_fresh(void)
{
    for ( int r = 0; (int)KBSP_RES_MAX > r; ++r )
    {
        kbsp_release((kbsp_res_t)r, &s_alice);
        kbsp_release((kbsp_res_t)r, &s_bob);
    }
}

TEST_CASE("a free resource is granted, and names its owner", "[kbsp_owners]")
{
    owners_fresh();

    TEST_ASSERT_TRUE(kbsp_claim(KBSP_RES_PMIC, &s_alice, "AO_Power"));
    TEST_ASSERT_EQUAL_STRING("AO_Power", kbsp_owner_name(KBSP_RES_PMIC));

    owners_fresh();
}

/* The refusal is the point of the whole file. Two owners of one device is a
 * design error the board answers by killing the boot and naming both. */
TEST_CASE("a second claimant is refused and does not displace the first",
          "[kbsp_owners]")
{
    owners_fresh();

    TEST_ASSERT_TRUE(kbsp_claim(KBSP_RES_PMIC, &s_alice, "AO_Power"));
    TEST_ASSERT_FALSE(kbsp_claim(KBSP_RES_PMIC, &s_bob, "AO_Impostor"));

    /* And the table still names the first, not the one that just asked. */
    TEST_ASSERT_EQUAL_STRING("AO_Power", kbsp_owner_name(KBSP_RES_PMIC));

    owners_fresh();
}

/* The claim is one compare-and-swap, so a loser can never take the resource
 * over by asking again, and its name never reaches the table; only the
 * holder's release frees it, and then the next claimant wins. */
TEST_CASE("the loser of a claim stays refused, and its name never reaches the table",
          "[kbsp_owners]")
{
    owners_fresh();

    TEST_ASSERT_TRUE(kbsp_claim(KBSP_RES_PMIC, &s_alice, "console (bench)"));
    for ( int i = 0; 3 > i; ++i )
    {
        TEST_ASSERT_FALSE(kbsp_claim(KBSP_RES_PMIC, &s_bob, "app"));
        TEST_ASSERT_EQUAL_STRING("console (bench)", kbsp_owner_name(KBSP_RES_PMIC));
    }
    /* The winner re-claims what it holds; the loser's release is a no-op. */
    TEST_ASSERT_TRUE(kbsp_claim(KBSP_RES_PMIC, &s_alice, "renamed"));
    kbsp_release(KBSP_RES_PMIC, &s_bob);
    TEST_ASSERT_EQUAL_STRING("console (bench)", kbsp_owner_name(KBSP_RES_PMIC));

    kbsp_release(KBSP_RES_PMIC, &s_alice);
    TEST_ASSERT_NULL(kbsp_owner_name(KBSP_RES_PMIC));
    TEST_ASSERT_TRUE(kbsp_claim(KBSP_RES_PMIC, &s_bob, "app"));
    TEST_ASSERT_EQUAL_STRING("app", kbsp_owner_name(KBSP_RES_PMIC));

    owners_fresh();
}

/* Re-claiming your own is harmless: an AO that brings a device up twice is
 * not a second claimant. The name is deliberately not updated - the first
 * claim named it, and a later one is the same owner, not a rename. */
TEST_CASE("re-claiming your own resource succeeds and keeps the first name",
          "[kbsp_owners]")
{
    owners_fresh();

    TEST_ASSERT_TRUE(kbsp_claim(KBSP_RES_GAUGE, &s_alice, "AO_Power"));
    TEST_ASSERT_TRUE(kbsp_claim(KBSP_RES_GAUGE, &s_alice, "something else"));
    TEST_ASSERT_EQUAL_STRING("AO_Power", kbsp_owner_name(KBSP_RES_GAUGE));

    owners_fresh();
}

TEST_CASE("releasing frees the resource for anybody", "[kbsp_owners]")
{
    owners_fresh();

    TEST_ASSERT_TRUE(kbsp_claim(KBSP_RES_SDMMC, &s_alice, "AO_Storage"));
    kbsp_release(KBSP_RES_SDMMC, &s_alice);

    TEST_ASSERT_NULL(kbsp_owner_name(KBSP_RES_SDMMC));
    TEST_ASSERT_TRUE(kbsp_claim(KBSP_RES_SDMMC, &s_bob, "somebody else"));

    owners_fresh();
}

/* A release from anyone but the holder is ignored, or releasing would be a
 * way around the claim. */
TEST_CASE("a release by someone who does not hold it does nothing",
          "[kbsp_owners]")
{
    owners_fresh();

    TEST_ASSERT_TRUE(kbsp_claim(KBSP_RES_FATFS, &s_alice, "AO_Storage"));
    kbsp_release(KBSP_RES_FATFS, &s_bob);

    TEST_ASSERT_EQUAL_STRING("AO_Storage", kbsp_owner_name(KBSP_RES_FATFS));
    TEST_ASSERT_FALSE(kbsp_claim(KBSP_RES_FATFS, &s_bob, "AO_Impostor"));

    owners_fresh();
}

TEST_CASE("a claim outside the table, or with no owner, is refused",
          "[kbsp_owners]")
{
    owners_fresh();

    TEST_ASSERT_FALSE(kbsp_claim(KBSP_RES_MAX, &s_alice, "nowhere"));
    TEST_ASSERT_FALSE(kbsp_claim((kbsp_res_t)(KBSP_RES_MAX + 3), &s_alice, "x"));
    TEST_ASSERT_FALSE(kbsp_claim(KBSP_RES_C5, NULL, "nobody"));

    /* The last one must not have taken the resource on the way to refusing. */
    TEST_ASSERT_NULL(kbsp_owner_name(KBSP_RES_C5));
    TEST_ASSERT_NULL(kbsp_owner_name(KBSP_RES_MAX));

    owners_fresh();
}

/* The expander is one resource, and it has to be: the chip has one input
 * register and one INT line, and whoever reads the register is the one who
 * clears the interrupt. So the TCA6408A is owned whole, INT included, and its
 * pins are asked for by event - the table has no per-pin slot to hand out.
 *
 * Checked without naming an owner, on purpose: the real AO_IoExp lives in
 * this binary and whether it has claimed by the time this case runs depends
 * on the order the case files sort in. Either it holds the chip, or the chip
 * is free and one claim takes all of it. */
TEST_CASE("the expander is one resource: the chip and its interrupt line",
          "[kbsp_owners]")
{
    owners_fresh();

    TEST_ASSERT_EQUAL_STRING("expander (TCA6408A) + INT",
                             kbsp_res_name(KBSP_RES_IOEXP));

    if ( kbsp_owner_name(KBSP_RES_IOEXP) == NULL )
    {
        TEST_ASSERT_TRUE(kbsp_claim(KBSP_RES_IOEXP, &s_alice, "AO_IoExp"));
        TEST_ASSERT_FALSE(kbsp_claim(KBSP_RES_IOEXP, &s_bob, "AO_Display"));
        kbsp_release(KBSP_RES_IOEXP, &s_alice);
        TEST_ASSERT_NULL(kbsp_owner_name(KBSP_RES_IOEXP));
    }
    else
    {
        TEST_ASSERT_FALSE(kbsp_claim(KBSP_RES_IOEXP, &s_bob, "AO_Display"));
    }

    owners_fresh();
}

/* Every resource across the whole range, rather than a sample: an off-by-one
 * in the range guard would only show at an end.
 *
 * Some of them are already held, and that is the point rather than a nuisance
 * - real Active Objects live in this binary and claim what they own during
 * their initial transitions, so this sweep runs against the table in the
 * state production leaves it in. It takes what is free, proves a second
 * claimant is refused on *every* slot whoever holds it, and gives back only
 * what it took. Taking something an AO owns would kill the AO's own case
 * later in the run, which is how this case first found out. */
TEST_CASE("every resource refuses a second claimant, and gives back what it lent",
          "[kbsp_owners]")
{
    static int  tokens[KBSP_RES_MAX];
    static bool took[KBSP_RES_MAX];
    unsigned    taken = 0U;

    owners_fresh();

    for ( int r = 0; (int)KBSP_RES_MAX > r; ++r )
    {
        took[r] = (kbsp_owner_name((kbsp_res_t)r) == NULL);
        if ( took[r] )
        {
            TEST_ASSERT_TRUE(kbsp_claim((kbsp_res_t)r, &tokens[r], "sweep"));
            ++taken;
        }
    }

    /* Every slot in the table is now owned by somebody, and every one of them
     * refuses a stranger. */
    for ( int r = 0; (int)KBSP_RES_MAX > r; ++r )
    {
        TEST_ASSERT_NOT_NULL(kbsp_owner_name((kbsp_res_t)r));
        TEST_ASSERT_FALSE(kbsp_claim((kbsp_res_t)r, &s_bob, "impostor"));
    }

    for ( int r = 0; (int)KBSP_RES_MAX > r; ++r )
    {
        if ( took[r] )
        {
            kbsp_release((kbsp_res_t)r, &tokens[r]);
            TEST_ASSERT_NULL(kbsp_owner_name((kbsp_res_t)r));
        }
    }

    /* The sweep is worth nothing if it swept nothing. */
    TEST_ASSERT_GREATER_THAN_UINT(0U, taken);
}

/* Every resource has a name. RES_NAMES is built with designated initialisers,
 * so an enumerator added without one leaves a NULL that the range guard waves
 * through - and the caller is kaos_claim(), feeding it to a printf on the
 * path that is already dying. This loop is what makes that impossible. */
TEST_CASE("every resource in the table has a name", "[kbsp_owners]")
{
    for ( int r = 0; (int)KBSP_RES_MAX > r; ++r )
    {
        const char *name = kbsp_res_name((kbsp_res_t)r);
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_TRUE('\0' != name[0]);
    }
    TEST_ASSERT_EQUAL_STRING("?", kbsp_res_name(KBSP_RES_MAX));
}

/* The LF RFID front end joined the table as one resource. Named directly
 * rather than by owner: no Active Object claims it in this binary, so its
 * name is the table's, not a claimant's. */
TEST_CASE("the owner table names the RFID front end", "[kbsp_owners]")
{
    TEST_ASSERT_EQUAL_STRING("rfid-lf", kbsp_res_name(KBSP_RES_RFID));
}

/**
 * @brief The first resource no Active Object in this binary holds.
 *
 * Naming one directly does not survive: real AOs live here and claim what
 * they own, and which resources are spare changes every time one joins the
 * suite. This case named KBSP_RES_APPS_REGION once and broke the day
 * AO_AppMgr arrived, then named KBSP_RES_CONSOLE and was passing only because
 * `test_kbsp_owners.c` happens to sort before `test_log_ao.c`, whose AO
 * takes the console. Asking is the only thing that keeps working.
 */
static kbsp_res_t first_free(void)
{
    for ( int r = 0; (int)KBSP_RES_MAX > r; ++r )
    {
        if ( kbsp_owner_name((kbsp_res_t)r) == NULL )
        {
            return (kbsp_res_t)r;
        }
    }
    TEST_FAIL_MESSAGE("every resource is spoken for; this case has nothing to take");
    return KBSP_RES_MAX;
}

/* Naming an owner that is not there. */
TEST_CASE("a released resource has no owner name again", "[kbsp_owners]")
{
    owners_fresh();

    kbsp_res_t const res = first_free();

    TEST_ASSERT_TRUE(kbsp_claim(res, &s_alice, "somebody"));
    TEST_ASSERT_NOT_NULL(kbsp_owner_name(res));

    kbsp_release(res, &s_alice);
    TEST_ASSERT_NULL(kbsp_owner_name(res));

    /* And out of range is never owned by anyone. */
    TEST_ASSERT_NULL(kbsp_owner_name(KBSP_RES_MAX));
}
