/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_clocks.c
 * @brief The clocks driver, and the boards this bench does not have.
 */
#include "unity.h"

#include "Mockkhal_clk.h"

#include <string.h>

#include "fakes.h"
#include "kdrv_clocks.h"
#include "khal_nvs.h"
#include "khal_time.h"

/** @brief 32.768 kHz in milli-hertz. Deliberately not KBSP_XTAL_32K_MHZ. */
#define NOMINAL_MHZ   32768000U

/** @brief The +/-1 % window, in milli-hertz. Also deliberately a literal. */
#define TOL_MHZ         327680U

/** @brief 40 MHz, what everything below is counted against. */
#define MAIN_HZ       40000000U

/**
 * @brief Queue the calls one kdrv_clocks_read() makes, with X3's reading
 *        under the case's control and everything else plausible.
 *
 * The three counted sources are asked for in a fixed order - X3, the slow RC,
 * the fast RC - and the four derived clocks after them. CMock enforces that
 * order, which is worth having: a reordering that measured the RC while the
 * crystal was still settling would be a real bug and would show up here.
 *
 * @param[in] mux      What the slow-clock mux reports.
 * @param[in] x32k_rc  What the counter returns for X3.
 * @param[in] x32k_mhz The reading handed back when @p x32k_rc is KODE_OK.
 */
static void expect_read(khal_clk_slow_t mux, kode_status_t x32k_rc,
                        uint32_t x32k_mhz)
{
    static uint32_t x32k;
    static uint32_t rc_slow;
    static uint32_t tree;

    x32k    = x32k_mhz;
    rc_slow = 121300000U;   /* what this board's RC actually measures */
    tree    = 400000000U;

    khal_clk_main_hz_ExpectAndReturn(MAIN_HZ);
    khal_clk_slow_src_ExpectAndReturn(mux);

    khal_clk_measure_ExpectAndReturn(KHAL_CLK_SRC_XTAL32K,
                                     KDRV_CLOCKS_CYCLES_FAST, NULL, x32k_rc);
    khal_clk_measure_IgnoreArg_milli_hz();
    if ( KODE_OK == x32k_rc )
    {
        khal_clk_measure_ReturnThruPtr_milli_hz(&x32k);
    }

    khal_clk_measure_ExpectAndReturn(KHAL_CLK_SRC_RC_SLOW,
                                     KDRV_CLOCKS_CYCLES_FAST, NULL, KODE_OK);
    khal_clk_measure_IgnoreArg_milli_hz();
    khal_clk_measure_ReturnThruPtr_milli_hz(&rc_slow);

    /* CPU, APB, RTC slow and RTC fast, in that order. The values are not
     * asserted anywhere - what matters is that the driver asks for four and
     * survives whatever comes back. */
    for ( int i = 0; 4 > i; ++i )
    {
        khal_clk_tree_hz_ExpectAndReturn(KHAL_CLK_TREE_CPU, NULL, KODE_OK);
        khal_clk_tree_hz_IgnoreArg_clk();
        khal_clk_tree_hz_IgnoreArg_hz();
        khal_clk_tree_hz_ReturnThruPtr_hz(&tree);
    }
}

/**
 * @brief Run the production row against one arranged board.
 *
 * @param[in] mux      What the slow-clock mux reports.
 * @param[in] x32k_rc  What the counter returns for X3.
 * @param[in] x32k_mhz X3's reading when the counter answers.
 * @return The row's verdict.
 */
static bool row(khal_clk_slow_t mux, kode_status_t x32k_rc, uint32_t x32k_mhz)
{
    bool ok;

    Mockkhal_clk_Init();
    fake_klog_reset();
    expect_read(mux, x32k_rc, x32k_mhz);
    ok = kdrv_clocks_test();
    Mockkhal_clk_Verify();
    Mockkhal_clk_Destroy();

    return ok;
}

TEST_CASE("a board on its crystal, at nominal, passes", "[kdrv_clocks]")
{
    TEST_ASSERT_TRUE(row(KHAL_CLK_SLOW_XTAL32K, KODE_OK, NOMINAL_MHZ));
}

TEST_CASE("no crystal fitted: the counter times out and the mux is on the RC",
          "[kdrv_clocks]")
{
    /* The board that exists and boots perfectly. Both checks fail, and both
     * failures are the same physical fact seen from two sides. */
    TEST_ASSERT_FALSE(row(KHAL_CLK_SLOW_RC, KODE_TIMEOUT, 0U));
}

TEST_CASE("the silent fallback: the crystal runs but the mux is not on it",
          "[kdrv_clocks]")
{
    /* Nothing else on the chip differs, which is the whole reason the mux is
     * read at all rather than trusting the reading. */
    TEST_ASSERT_FALSE(row(KHAL_CLK_SLOW_RC, KODE_OK, NOMINAL_MHZ));
}

TEST_CASE("a crystal that stopped after the mux switched to it fails",
          "[kdrv_clocks]")
{
    TEST_ASSERT_FALSE(row(KHAL_CLK_SLOW_XTAL32K, KODE_TIMEOUT, 0U));
}

TEST_CASE("the internal RC standing in for a 32 kHz crystal cannot pass",
          "[kdrv_clocks]")
{
    /* Four times the nominal: the window is 1 %, so this is never close. */
    TEST_ASSERT_FALSE(row(KHAL_CLK_SLOW_XTAL32K, KODE_OK, 121300000U));
}

TEST_CASE("the tolerance window is inclusive on both edges", "[kdrv_clocks]")
{
    TEST_ASSERT_TRUE(row(KHAL_CLK_SLOW_XTAL32K, KODE_OK,
                         NOMINAL_MHZ - TOL_MHZ));
    TEST_ASSERT_TRUE(row(KHAL_CLK_SLOW_XTAL32K, KODE_OK,
                         NOMINAL_MHZ + TOL_MHZ));
    TEST_ASSERT_FALSE(row(KHAL_CLK_SLOW_XTAL32K, KODE_OK,
                          NOMINAL_MHZ - TOL_MHZ - 1U));
    TEST_ASSERT_FALSE(row(KHAL_CLK_SLOW_XTAL32K, KODE_OK,
                          NOMINAL_MHZ + TOL_MHZ + 1U));
}

/**
 * @brief Take one reading of an arranged board.
 *
 * @param[out] r        The reading.
 * @param[in]  x32k_mhz What the counter reports for X3.
 */
static void read_at(kdrv_clocks_t *r, uint32_t x32k_mhz)
{
    Mockkhal_clk_Init();
    expect_read(KHAL_CLK_SLOW_XTAL32K, KODE_OK, x32k_mhz);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_clocks_read(r,
                                                    KDRV_CLOCKS_CYCLES_FAST));
    Mockkhal_clk_Verify();
    Mockkhal_clk_Destroy();
}

TEST_CASE("ppm is signed and X1 is derived from X3, not measured",
          "[kdrv_clocks]")
{
    kdrv_clocks_t r;

    /* 8192 milli-hertz is exactly 250 ppm of 32.768 kHz, chosen so the
     * assertion is about the driver's arithmetic and not about where an
     * integer division happened to round. The board's real figure is +221. */
    uint32_t const off = 8192U;

    read_at(&r, NOMINAL_MHZ + off);
    TEST_ASSERT_TRUE(r.x32k_ok);
    TEST_ASSERT_EQUAL_INT32(250, r.x32k_ppm);

    /* A crystal counted fast implies an X1 counted slow, because the counter
     * divides by X1 and this is that same ratio read backwards. It is not a
     * second instrument, and the pair moving in opposite directions is the
     * whole reason it must never be printed as one. */
    TEST_ASSERT_LESS_THAN_UINT32(MAIN_HZ, r.main_from_x32k_hz);

    read_at(&r, NOMINAL_MHZ - off);
    TEST_ASSERT_EQUAL_INT32(-250, r.x32k_ppm);
    TEST_ASSERT_GREATER_THAN_UINT32(MAIN_HZ, r.main_from_x32k_hz);

    read_at(&r, NOMINAL_MHZ);
    TEST_ASSERT_EQUAL_INT32(0, r.x32k_ppm);
    TEST_ASSERT_EQUAL_UINT32(MAIN_HZ, r.main_from_x32k_hz);
}

TEST_CASE("a silent crystal leaves no stale ppm or derived X1 behind",
          "[kdrv_clocks]")
{
    kdrv_clocks_t r;

    /* Nothing may divide by the zero a timeout leaves, and nothing may print
     * last pass's number as if it were this one. */
    Mockkhal_clk_Init();
    expect_read(KHAL_CLK_SLOW_XTAL32K, KODE_TIMEOUT, 0U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_clocks_read(&r,
                                                    KDRV_CLOCKS_CYCLES_FAST));
    Mockkhal_clk_Verify();
    Mockkhal_clk_Destroy();

    TEST_ASSERT_FALSE(r.x32k_ok);
    TEST_ASSERT_EQUAL_INT32(0, r.x32k_ppm);
    TEST_ASSERT_EQUAL_UINT32(0U, r.main_from_x32k_hz);
}

TEST_CASE("a bad argument is refused without touching the clock block",
          "[kdrv_clocks]")
{
    kdrv_clocks_t r;

    Mockkhal_clk_Init();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_clocks_read(NULL,
                                                        KDRV_CLOCKS_CYCLES_FAST));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_clocks_read(&r, 0U));
    Mockkhal_clk_Verify();
    Mockkhal_clk_Destroy();
}

TEST_CASE("every slow source has a one-word name for the wire", "[kdrv_clocks]")
{
    /* The console prints these after `src=`, so a space in one would split a
     * record into two keys. */
    TEST_ASSERT_EQUAL_STRING("xtal32k",
                             kdrv_clocks_slow_token(KDRV_CLOCKS_SLOW_XTAL32K));
    TEST_ASSERT_EQUAL_STRING("rc",
                             kdrv_clocks_slow_token(KDRV_CLOCKS_SLOW_RC));
    TEST_ASSERT_EQUAL_STRING("rc32k",
                             kdrv_clocks_slow_token(KDRV_CLOCKS_SLOW_RC32K));
    TEST_ASSERT_EQUAL_STRING("unknown",
                             kdrv_clocks_slow_token(KDRV_CLOCKS_SLOW_UNKNOWN));
}

/* ---- The wall clock ---------------------------------------------------
 * The mechanism is khal's and is a fake here; what these cases judge is the
 * policy: when a reading counts as a time at all, and the two directions of
 * ISO-8601. The threshold is written out rather than imported for the same
 * reason the tolerance above is - a test that imports the constant it checks
 * agrees with it by construction.
 */
#define KNOWN_AFTER_S  1767225600L   /* 2026-01-01T00:00:00Z */

TEST_CASE("a clock that was never set is not known", "[kdrv_clocks_wall]")
{
    fake_time_wall_set(0U);
    TEST_ASSERT_FALSE(kdrv_clocks_wall_known());

    /* The boundary is exclusive: the threshold instant itself is still the
     * default a chip powers up with as far as this is concerned. */
    fake_time_wall_set((uint64_t)KNOWN_AFTER_S * 1000000ULL);
    TEST_ASSERT_FALSE(kdrv_clocks_wall_known());

    fake_time_wall_set(((uint64_t)KNOWN_AFTER_S + 1ULL) * 1000000ULL);
    TEST_ASSERT_TRUE(kdrv_clocks_wall_known());
}

TEST_CASE("a well-formed UTC timestamp is accepted and read back",
          "[kdrv_clocks_wall]")
{
    char iso[32];

    fake_time_wall_set(0U);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_clocks_wall_set_iso("2026-09-03T21:09:44Z"));
    TEST_ASSERT_TRUE(kdrv_clocks_wall_known());
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_clocks_wall_iso(iso, sizeof(iso)));
    TEST_ASSERT_EQUAL_STRING("2026-09-03T21:09:44Z", iso);
}

TEST_CASE("a timestamp becomes the instant it names, on every calendar edge",
          "[kdrv_clocks_wall]")
{
    /* The conversion is the driver's own, not the C library's timegm(),
     * because newlib (ESP-IDF 5.5) has none. So it is pinned here against
     * values worked out independently, on the days a hand-written calendar
     * gets wrong first: a leap day, a century that is not a leap year, one
     * that is, and the last second the parser can express. The read-back
     * goes through gmtime_r, so each row is checked from both directions. */
    static const struct {
        const char *iso;
        uint64_t    secs;
    } rows[] = {
        { "2026-09-03T21:09:44Z", 1788469784ULL   },
        { "2026-12-31T23:59:59Z", 1798761599ULL   },
        { "2028-02-29T23:59:59Z", 1835481599ULL   },
        { "2100-03-01T00:00:00Z", 4107542400ULL   },
        { "2104-02-29T12:00:00Z", 4233729600ULL },
        { "2105-12-31T23:59:59Z", 4291747199ULL },
    };
    char iso[32];

    for ( size_t i = 0U; (sizeof(rows) / sizeof(rows[0])) > i; ++i )
    {
        fake_time_wall_set(0U);
        TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK,
                                      kdrv_clocks_wall_set_iso(rows[i].iso),
                                      rows[i].iso);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(rows[i].secs * 1000000ULL,
                                         khal_time_wall_us(), rows[i].iso);
        TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_clocks_wall_iso(iso, sizeof(iso)));
        TEST_ASSERT_EQUAL_STRING(rows[i].iso, iso);
    }
}

TEST_CASE("a day the month does not have, or a year past the limit, is refused",
          "[kdrv_clocks_wall]")
{
    static const char *const bad[] = {
        "2026-02-29T12:00:00Z",   /* 2026 is not a leap year               */
        "2026-02-30T00:00:00Z",   /* no February has a 30th                */
        "2026-02-31T00:00:00Z",
        "2026-04-31T00:00:00Z",   /* April has thirty days                 */
        "2100-02-29T00:00:00Z",   /* a century year, not a leap year       */
        "2106-02-07T06:28:16Z",   /* past a 32-bit unsigned time_t         */
        "2106-01-01T00:00:00Z",   /* past the last whole year              */
        "9999-12-31T23:59:59Z",   /* the clock would roll into year 10000  */
    };
    uint64_t const before = ((uint64_t)KNOWN_AFTER_S + 500ULL) * 1000000ULL;

    for ( size_t i = 0U; (sizeof(bad) / sizeof(bad[0])) > i; ++i )
    {
        fake_time_wall_set(before);
        TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED,
                                      kdrv_clocks_wall_set_iso(bad[i]), bad[i]);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(before, khal_time_wall_us(), bad[i]);
    }
}

TEST_CASE("what a malformed timestamp does is refuse, not guess",
          "[kdrv_clocks_wall]")
{
    /* Every one of these has been typed by somebody at a console. None of
     * them may move the clock, and the clock must still read what it did. */
    static const char *const bad[] = {
        "",
        "2026-09-03",             /* a date with no time                  */
        "2026-09-03 21:09:44Z",   /* a space where the T belongs          */
        "2026-13-03T21:09:44Z",   /* month 13                             */
        "2026-09-32T21:09:44Z",   /* day 32                               */
        "2026-09-03T24:09:44Z",   /* hour 24                              */
        "2026-09-03T21:60:44Z",   /* minute 60                            */
        "2026-09-03T21:09:60Z",   /* second 60                            */
        "2026-09-03T21:09:44",    /* no zone, and this board only has UTC */
        "2026-09-03T21:09:44+02:00",
        "not-a-time-at-all",
    };
    uint64_t const before = ((uint64_t)KNOWN_AFTER_S + 500ULL) * 1000000ULL;

    for ( size_t i = 0U; (sizeof(bad) / sizeof(bad[0])) > i; ++i )
    {
        fake_time_wall_set(before);
        TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED,
                                      kdrv_clocks_wall_set_iso(bad[i]), bad[i]);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(before, khal_time_wall_us(), bad[i]);
    }

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_clocks_wall_set_iso(NULL));
}

TEST_CASE("a time before the threshold is refused as a typo",
          "[kdrv_clocks_wall]")
{
    /* 1999 is not a date anybody means on this board; accepting it would set
     * the clock to a value that then reads as never set, which is a state
     * nothing downstream can make sense of. */
    fake_time_wall_set(0U);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kdrv_clocks_wall_set_iso("1999-12-31T23:59:59Z"));
    TEST_ASSERT_FALSE(kdrv_clocks_wall_known());
}

TEST_CASE("reading an unset clock refuses rather than printing 1970",
          "[kdrv_clocks_wall]")
{
    char iso[32];

    fake_time_wall_set(0U);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_clocks_wall_iso(iso, sizeof(iso)));
}

TEST_CASE("a buffer too small to hold a timestamp is refused whole",
          "[kdrv_clocks_wall]")
{
    char small[8];

    fake_time_wall_set(((uint64_t)KNOWN_AFTER_S + 500ULL) * 1000000ULL);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_clocks_wall_iso(small, sizeof(small)));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_clocks_wall_iso(NULL, 32U));
}

TEST_CASE("every source has a one-word name for the wire", "[kdrv_clocks_wall]")
{
    /* The console prints these after `source=`, so a space in one would split
     * a record into two keys. Asserted on the enum directly rather than on
     * the driver's current source: that lives in a static for the life of the
     * binary, and what any earlier case left in it is not this case's
     * business. */
    TEST_ASSERT_EQUAL_STRING("rtc",
        kdrv_clocks_wall_source_token(KDRV_CLOCKS_WALL_RTC));
    TEST_ASSERT_EQUAL_STRING("set",
        kdrv_clocks_wall_source_token(KDRV_CLOCKS_WALL_SET));
    TEST_ASSERT_EQUAL_STRING("sntp",
        kdrv_clocks_wall_source_token(KDRV_CLOCKS_WALL_SNTP));
}

TEST_CASE("the clock says where its value came from", "[kdrv_clocks_wall]")
{
    /* An operator reading `time` wants to know whether to trust it, and
     * "the RTC still had it after a sleep" is a different claim from "a
     * server said so ten seconds ago". What this case owns is the two
     * transitions - the only part that is a behaviour rather than a starting
     * value the suite's link order decides. */
    fake_time_wall_set(0U);

    (void)kdrv_clocks_wall_set_iso("2026-09-03T21:09:44Z");
    TEST_ASSERT_EQUAL_STRING("set",
        kdrv_clocks_wall_source_token(kdrv_clocks_wall_source()));

    kdrv_clocks_wall_note_sntp();
    TEST_ASSERT_EQUAL_STRING("sntp",
        kdrv_clocks_wall_source_token(kdrv_clocks_wall_source()));
}

/* ---- Keeping the wall clock across a power cut -------------------------
 * The P4's RTC keeps the clock across every reset that leaves the chip
 * powered, and loses it with the supply: ship mode, BOT+LEFT, a flat cell.
 * These judge the policy that brings it back - restore only a clock that was
 * lost, say it is approximate until something real replaces it, across
 * restarts too, and write only when due.
 */
#define T0_S       1790000000ULL   /* 2026-09-21, comfortably known */
#define KEEP_S     60U             /* written out: see KNOWN_AFTER_S above */

/** @brief A fresh boot: nothing kept, a clock at @p wall_s seconds. */
static void wall_boot(uint64_t wall_s)
{
    fake_nvs_reset();
    fake_time_reset();
    fake_time_wall_set(wall_s * 1000000ULL);
}

/** @brief Let @p s seconds pass on both clocks. */
static void run_on(uint32_t s)
{
    fake_time_advance_ms(s * 1000U);
    khal_time_wall_set(khal_time_wall_us() + ((uint64_t)s * 1000000ULL));
}

/** @brief What NVS holds for the kept clock, or 0 when nothing. */
static uint64_t kept_wall(void)
{
    uint64_t v = 0U;
    (void)khal_nvs_get_u64("kclock", "wall", &v);
    return v;
}

/** @brief What NVS holds for the approximate mark, or 0xFF when nothing. */
static uint8_t kept_approx(void)
{
    uint8_t v = 0xFFU;
    (void)khal_nvs_get_u8("kclock", "approx", &v);
    return v;
}

TEST_CASE("with nothing kept an unknown clock stays unknown", "[kdrv_clocks_wall]")
{
    wall_boot(0U);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_clocks_wall_restore());
    TEST_ASSERT_FALSE(kdrv_clocks_wall_known());
    TEST_ASSERT_FALSE(kdrv_clocks_wall_approx());
}

TEST_CASE("a kept time is put back on a clock that lost it, approximate",
          "[kdrv_clocks_wall]")
{
    wall_boot(0U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_u64("kclock", "wall", T0_S));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_clocks_wall_restore());
    TEST_ASSERT_TRUE(khal_time_wall_us() == (T0_S * 1000000ULL));
    TEST_ASSERT_EQUAL(KDRV_CLOCKS_WALL_SAVED, kdrv_clocks_wall_source());
    TEST_ASSERT_TRUE(kdrv_clocks_wall_approx());
    TEST_ASSERT_EQUAL_STRING("saved",
        kdrv_clocks_wall_source_token(kdrv_clocks_wall_source()));
}

TEST_CASE("a clock that survived is left alone, and is RTC unless marked",
          "[kdrv_clocks_wall]")
{
    wall_boot(T0_S + 500U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_u64("kclock", "wall", T0_S));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_clocks_wall_restore());
    TEST_ASSERT_TRUE(khal_time_wall_us() == ((T0_S + 500U) * 1000000ULL));
    TEST_ASSERT_EQUAL(KDRV_CLOCKS_WALL_RTC, kdrv_clocks_wall_source());
    TEST_ASSERT_FALSE(kdrv_clocks_wall_approx());
}

TEST_CASE("a clock that survived a restore stays approximate across a restart",
          "[kdrv_clocks_wall]")
{
    wall_boot(T0_S + 500U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_u8("kclock", "approx", 1U));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_clocks_wall_restore());
    TEST_ASSERT_EQUAL(KDRV_CLOCKS_WALL_SAVED, kdrv_clocks_wall_source());
    TEST_ASSERT_TRUE(kdrv_clocks_wall_approx());
}

TEST_CASE("a kept value under the threshold is refused", "[kdrv_clocks_wall]")
{
    wall_boot(0U);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_nvs_set_u64("kclock", "wall", (uint64_t)KNOWN_AFTER_S));

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_clocks_wall_restore());
    TEST_ASSERT_FALSE(kdrv_clocks_wall_known());
}

TEST_CASE("the first keep writes, then nothing until the period",
          "[kdrv_clocks_wall]")
{
    wall_boot(0U);
    (void)kdrv_clocks_wall_restore();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_clocks_wall_set_iso("2026-09-21T14:13:20Z"));

    TEST_ASSERT_TRUE(kdrv_clocks_wall_keep(false));
    TEST_ASSERT_TRUE(kept_wall() == T0_S);
    run_on(KEEP_S - 1U);
    TEST_ASSERT_FALSE(kdrv_clocks_wall_keep(false));
    TEST_ASSERT_TRUE(kept_wall() == T0_S);
    run_on(1U);
    TEST_ASSERT_TRUE(kdrv_clocks_wall_keep(false));
    TEST_ASSERT_TRUE(kept_wall() == (T0_S + KEEP_S));
}

TEST_CASE("a jump of a minute writes at once", "[kdrv_clocks_wall]")
{
    wall_boot(T0_S);
    (void)kdrv_clocks_wall_restore();
    TEST_ASSERT_TRUE(kdrv_clocks_wall_keep(false));

    khal_time_wall_set((T0_S + 60U) * 1000000ULL);
    TEST_ASSERT_TRUE(kdrv_clocks_wall_keep(false));
    TEST_ASSERT_TRUE(kept_wall() == (T0_S + 60U));
}

TEST_CASE("force writes whatever the schedule", "[kdrv_clocks_wall]")
{
    wall_boot(T0_S);
    (void)kdrv_clocks_wall_restore();
    TEST_ASSERT_TRUE(kdrv_clocks_wall_keep(true));
    TEST_ASSERT_TRUE(kdrv_clocks_wall_keep(true));
}

TEST_CASE("an unknown clock is never kept", "[kdrv_clocks_wall]")
{
    wall_boot(0U);
    (void)kdrv_clocks_wall_restore();
    TEST_ASSERT_FALSE(kdrv_clocks_wall_keep(true));
    TEST_ASSERT_TRUE(kept_wall() == 0U);
}

TEST_CASE("approx is written only when it changes", "[kdrv_clocks_wall]")
{
    wall_boot(0U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_u64("kclock", "wall", T0_S));
    (void)kdrv_clocks_wall_restore();

    (void)kdrv_clocks_wall_keep(false);
    TEST_ASSERT_EQUAL_UINT8(1U, kept_approx());

    /* Taken away behind its back: a keep that rewrote the mark every time
     * would put it back. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_erase("kclock", "approx"));
    run_on(1U);
    (void)kdrv_clocks_wall_keep(false);
    TEST_ASSERT_EQUAL_UINT8(0xFFU, kept_approx());

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_clocks_wall_set_iso("2026-09-21T14:00:00Z"));
    (void)kdrv_clocks_wall_keep(false);
    TEST_ASSERT_EQUAL_UINT8(0U, kept_approx());
}

TEST_CASE("SNTP clears the approximate mark", "[kdrv_clocks_wall]")
{
    wall_boot(0U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_u64("kclock", "wall", T0_S));
    (void)kdrv_clocks_wall_restore();
    (void)kdrv_clocks_wall_keep(false);

    kdrv_clocks_wall_note_sntp();
    TEST_ASSERT_FALSE(kdrv_clocks_wall_approx());
    run_on(1U);
    (void)kdrv_clocks_wall_keep(false);
    TEST_ASSERT_EQUAL_UINT8(0U, kept_approx());
}

/* A reset in the two seconds between `time set` and the keep that would
 * clear the mark: the next boot reads a right clock marked approximate.
 * That is the safe direction - it says less than it could. */
TEST_CASE("a set the keep has not written yet still reads approx after a restart",
          "[kdrv_clocks_wall]")
{
    wall_boot(0U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_u64("kclock", "wall", T0_S));
    (void)kdrv_clocks_wall_restore();
    (void)kdrv_clocks_wall_keep(false);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_clocks_wall_set_iso("2026-09-21T14:00:00Z"));

    /* The restart: the RTC kept the clock, the mark is still in NVS. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_clocks_wall_restore());
    TEST_ASSERT_TRUE(kdrv_clocks_wall_approx());
}

TEST_CASE("a failed write waits for the next period", "[kdrv_clocks_wall]")
{
    wall_boot(0U);
    (void)kdrv_clocks_wall_restore();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_clocks_wall_set_iso("2026-09-21T14:13:20Z"));

    fake_nvs_fail_next_set();
    TEST_ASSERT_FALSE(kdrv_clocks_wall_keep(false));
    TEST_ASSERT_TRUE(kept_wall() == 0U);
    run_on(KEEP_S - 1U);
    TEST_ASSERT_FALSE_MESSAGE(kdrv_clocks_wall_keep(false),
                              "not asked again every poll");
    run_on(1U);
    TEST_ASSERT_TRUE(kdrv_clocks_wall_keep(false));
    TEST_ASSERT_TRUE(kept_wall() == (T0_S + KEEP_S));
}

/* A restored time is marked at once, not at the first keep: the reset that
 * follows within two seconds - a port opened, a serial monitor - must not
 * read a time hours late as a known one. */
TEST_CASE("a restored time is marked in NVS at once", "[kdrv_clocks_wall]")
{
    wall_boot(0U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_u64("kclock", "wall", T0_S));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_clocks_wall_restore());
    TEST_ASSERT_EQUAL_UINT8(1U, kept_approx());

    /* The restart, before any keep ran: the RTC kept the restored value. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_clocks_wall_restore());
    TEST_ASSERT_TRUE(kdrv_clocks_wall_approx());
}

/* A mark the store refuses is tried again on the clock's own schedule, not
 * at every poll: a full NVS would otherwise be written, and warned about,
 * every two seconds. */
TEST_CASE("a mark the store refused waits for the next period",
          "[kdrv_clocks_wall]")
{
    wall_boot(0U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_u64("kclock", "wall", T0_S));
    (void)kdrv_clocks_wall_restore();
    (void)kdrv_clocks_wall_keep(false);
    kdrv_clocks_wall_note_sntp();

    fake_nvs_fail_next_set();
    run_on(1U);
    (void)kdrv_clocks_wall_keep(false);
    unsigned const after_refusal = fake_nvs_set_count();
    run_on(1U);
    (void)kdrv_clocks_wall_keep(false);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(after_refusal, fake_nvs_set_count(),
                                   "not tried again at the next poll");
    run_on(KEEP_S);
    (void)kdrv_clocks_wall_keep(false);
    TEST_ASSERT_EQUAL_UINT8(0U, kept_approx());
}
