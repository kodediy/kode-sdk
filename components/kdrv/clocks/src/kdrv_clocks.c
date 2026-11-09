/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_clocks.c
 * @brief kdrv_clocks: the readings, and the two checks worth failing on.
 */
#include "kdrv_clocks.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "kbsp_board.h"
#include "khal_clk.h"
#include "khal_nvs.h"
#include "khal_time.h"
#include "kode_log.h"

#define TAG "kdrv.clocks"

/** @brief One part per million, as the numerator of a ppm calculation. */
#define PPM_SCALE  1000000

#define ISO_MONTHS      12    /**< Months in a year. */
#define ISO_FEBRUARY    2     /**< February's number. */
#define ISO_LEAP_DAYS   29    /**< Days February has in a leap year. */
#define ISO_LEAP_EVERY  4     /**< Years between leap years... */
#define ISO_LEAP_SKIP   100   /**< ...except a multiple of this... */
#define ISO_LEAP_KEEP   400   /**< ...unless it is also a multiple of this. */

/**
 * @brief Translate khal's view of the slow mux into this driver's.
 *
 * The two enums are deliberately separate: khal names what the silicon has,
 * this one names what the board means by it, and a target with a different
 * set of sources would change the first without touching the second.
 *
 * @param[in] s What khal reported.
 * @return The matching driver-level source.
 */
static kdrv_clocks_slow_t slow_from_khal(khal_clk_slow_t s)
{
    kdrv_clocks_slow_t out;

    switch ( s )
    {
    case KHAL_CLK_SLOW_RC:
        out = KDRV_CLOCKS_SLOW_RC;
        break;
    case KHAL_CLK_SLOW_XTAL32K:
        out = KDRV_CLOCKS_SLOW_XTAL32K;
        break;
    case KHAL_CLK_SLOW_RC32K:
        out = KDRV_CLOCKS_SLOW_RC32K;
        break;
    default:
        out = KDRV_CLOCKS_SLOW_UNKNOWN;
        break;
    }

    return out;
}

/** @brief What to call the slow source in a log line. */
static const char *slow_name(kdrv_clocks_slow_t s)
{
    switch ( s )
    {
    case KDRV_CLOCKS_SLOW_RC:
        return "RC (internal, the fallback)";
    case KDRV_CLOCKS_SLOW_XTAL32K:
        return "XTAL32K (X3)";
    case KDRV_CLOCKS_SLOW_RC32K:
        return "RC32K (internal 32k)";
    default:
        return "unknown";
    }
}

const char *kdrv_clocks_slow_token(kdrv_clocks_slow_t s)
{
    switch ( s )
    {
    case KDRV_CLOCKS_SLOW_RC:
        return "rc";
    case KDRV_CLOCKS_SLOW_XTAL32K:
        return "xtal32k";
    case KDRV_CLOCKS_SLOW_RC32K:
        return "rc32k";
    default:
        return "unknown";
    }
}

/**
 * @brief Count one source, folding a timeout into a flag.
 *
 * @param[in]  src    Source to count.
 * @param[in]  cycles Cycles to count.
 * @param[out] mhz    The reading in milli-hertz; set to 0 on a timeout.
 * @return true when the source produced edges.
 */
static bool measure(khal_clk_src_t src, uint32_t cycles, uint32_t *mhz)
{
    *mhz = 0U;

    return khal_clk_measure(src, cycles, mhz) == KODE_OK;
}

/**
 * @brief Read one derived clock, folding any refusal into 0.
 *
 * @param[in] clk Clock to read.
 * @return Its frequency in Hz, or 0 when the chip did not answer.
 */
static uint32_t tree_hz(khal_clk_tree_t clk)
{
    uint32_t hz = 0U;

    if ( khal_clk_tree_hz(clk, &hz) != KODE_OK )
    {
        hz = 0U;
    }

    return hz;
}

kode_status_t kdrv_clocks_read(kdrv_clocks_t *out, uint32_t cycles)
{
    if ( (NULL == out) || (0U == cycles) )
    {
        return KODE_DENIED;
    }

    out->main_hz  = khal_clk_main_hz();
    out->slow_src = slow_from_khal(khal_clk_slow_src());

    out->x32k_ok           = measure(KHAL_CLK_SRC_XTAL32K, cycles,
                                     &out->x32k_mhz);
    out->x32k_ppm          = 0;
    out->main_from_x32k_hz = 0U;
    if ( out->x32k_ok )
    {
        int64_t const err = (int64_t)out->x32k_mhz - (int64_t)KBSP_XTAL_32K_MHZ;
        out->x32k_ppm = (int32_t)((err * PPM_SCALE) / (int64_t)KBSP_XTAL_32K_MHZ);

        /* X1 as X3 sees it. The counter divides by X1, so this is the same
         * ratio read the other way round - it moves when either crystal
         * moves and cannot say which did. */
        out->main_from_x32k_hz =
            (uint32_t)(((uint64_t)out->main_hz * (uint64_t)KBSP_XTAL_32K_MHZ)
                       / (uint64_t)out->x32k_mhz);
    }

    out->rc_slow_ok = measure(KHAL_CLK_SRC_RC_SLOW, cycles, &out->rc_slow_mhz);

    out->cpu_hz      = tree_hz(KHAL_CLK_TREE_CPU);
    out->apb_hz      = tree_hz(KHAL_CLK_TREE_APB);
    out->rtc_slow_hz = tree_hz(KHAL_CLK_TREE_RTC_SLOW);
    out->rtc_fast_hz = tree_hz(KHAL_CLK_TREE_RTC_FAST);

    return KODE_OK;
}

/**
 * @brief Log one counted source, or say it produced nothing.
 *
 * @param[in] what The name to print.
 * @param[in] ok   Whether the source produced edges.
 * @param[in] mhz  Its frequency in milli-hertz, when it did.
 */
static void log_counted(const char *what, bool ok, uint32_t mhz)
{
    if ( ok )
    {
        KLOG_N(TAG, "%s %u.%03u Hz", what, (unsigned)(mhz / 1000U),
               (unsigned)(mhz % 1000U));
    }
    else
    {
        KLOG_N(TAG, "%s not oscillating", what);
    }
}

void kdrv_clocks_log(const kdrv_clocks_t *r)
{
    if ( NULL == r )
    {
        return;
    }

    KLOG_N(TAG, "X1 nominal %u Hz - its own error is not measurable on this "
                "board, every reading below is counted against it",
           (unsigned)r->main_hz);
    KLOG_N(TAG, "RTC slow mux %s", slow_name(r->slow_src));

    log_counted("X3", r->x32k_ok, r->x32k_mhz);
    if ( r->x32k_ok )
    {
        KLOG_N(TAG, "X3 %+d ppm of %u Hz, relative to X1",
               (int)r->x32k_ppm, (unsigned)(KBSP_XTAL_32K_MHZ / 1000U));
        KLOG_N(TAG, "X1 %u Hz if X3 were exact - the same ratio read "
                    "backwards, not a second measurement",
               (unsigned)r->main_from_x32k_hz);
    }

    log_counted("RC slow", r->rc_slow_ok, r->rc_slow_mhz);

    KLOG_N(TAG, "CPU %u Hz, APB %u Hz", (unsigned)r->cpu_hz,
           (unsigned)r->apb_hz);
    KLOG_N(TAG, "RTC slow %u Hz, RTC fast %u Hz", (unsigned)r->rtc_slow_hz,
           (unsigned)r->rtc_fast_hz);
}

bool kdrv_clocks_test(void)
{
    kdrv_clocks_t r;
    bool          ok = true;

    if ( kdrv_clocks_read(&r, KDRV_CLOCKS_CYCLES_FAST) != KODE_OK )
    {
        KLOG_E(TAG, "test: the clock block did not read");
        return false;
    }

    kdrv_clocks_log(&r);

    /* The mux, not a flag: this is where the silent fallback shows. */
    if ( KDRV_CLOCKS_SLOW_XTAL32K != r.slow_src )
    {
        KLOG_E(TAG, "test: the RTC slow clock is %s, not X3 - the crystal "
                    "did not start and the boot fell back",
               slow_name(r.slow_src));
        ok = false;
    }

    if ( !r.x32k_ok )
    {
        KLOG_E(TAG, "test: X3 produced no edges - not fitted, not started, "
                    "or not powered");
        ok = false;
    }
    else
    {
        uint32_t const lo = KBSP_XTAL_32K_MHZ - KBSP_XTAL_32K_TOL_MHZ;
        uint32_t const hi = KBSP_XTAL_32K_MHZ + KBSP_XTAL_32K_TOL_MHZ;

        if ( (r.x32k_mhz < lo) || (r.x32k_mhz > hi) )
        {
            KLOG_E(TAG, "test: X3 at %u.%03u Hz is outside %u..%u Hz",
                   (unsigned)(r.x32k_mhz / 1000U),
                   (unsigned)(r.x32k_mhz % 1000U),
                   (unsigned)(lo / 1000U), (unsigned)(hi / 1000U));
            ok = false;
        }
        else
        {
            KLOG_N(TAG, "test: X3 is within 1 %% of nominal, which is "
                        "presence and sanity - the part's 20 ppm needs an "
                        "off-board reference");
        }
    }

    return ok;
}

/* ---- The wall clock -----------------------------------------------------
 * khal holds the value; what lives here is what the board means by it. The
 * split matters on the host, where the value is a fake and this is the half
 * with something to get wrong.
 */

/** @brief `YYYY-MM-DDThh:mm:ssZ` and its terminator. */
#define ISO_LEN  21U

/** @brief Where the value currently in the clock came from. */
static kdrv_clocks_wall_src_t s_wall_src = KDRV_CLOCKS_WALL_RTC;

bool kdrv_clocks_wall_known(void)
{
    return khal_time_wall_known();
}

kdrv_clocks_wall_src_t kdrv_clocks_wall_source(void)
{
    return s_wall_src;
}

const char *kdrv_clocks_wall_source_token(kdrv_clocks_wall_src_t s)
{
    switch ( s )
    {
    case KDRV_CLOCKS_WALL_SET:
        return "set";
    case KDRV_CLOCKS_WALL_SNTP:
        return "sntp";
    case KDRV_CLOCKS_WALL_SAVED:
        return "saved";
    default:
        return "rtc";
    }
}

/* Whether the next keep writes at once: a value somebody just gave. */
static bool s_wall_kept;   /**< The clock was written since it last changed. */

static kdrv_clocks_wall_hook_t s_wall_hook;   /**< Told after each write of the clock; NULL for none. */

void kdrv_clocks_wall_on_set(kdrv_clocks_wall_hook_t hook)
{
    s_wall_hook = hook;
}

/**
 * @brief The value was written: keep where it came from, and tell the hook.
 *
 * Every writer of the wall clock (the console, the time server, the Time page)
 * ends in one of the two notes below, which makes this the one place to learn
 * that the clock moved.
 *
 * @param src Where the value came from.
 */
static void wall_written(kdrv_clocks_wall_src_t src)
{
    s_wall_src  = src;
    s_wall_kept = false;
    if ( NULL != s_wall_hook )
    {
        s_wall_hook();
    }
}

void kdrv_clocks_wall_note_sntp(void)
{
    wall_written(KDRV_CLOCKS_WALL_SNTP);
}

void kdrv_clocks_wall_note_set(void)
{
    wall_written(KDRV_CLOCKS_WALL_SET);
}

/* ---- Keeping it across a power cut ----------------------------------------
 * The RTC keeps the clock across every reset that leaves the chip powered,
 * and loses it with the supply: ship mode, BOT+LEFT (CHIP_PU), a flat cell.
 * The Dot has no battery-backed RTC, so the clock is written to NVS while it
 * runs and put back at boot. What is put back is a lower bound - late by
 * however long the board was off - so it says so until a time server or a
 * person replaces it. That mark lives in NVS beside the value, not in RAM:
 * the reset that follows a restore (opening the console port is one) must
 * not turn a guess into a known time.
 */

#define WALL_NVS_NS          "kclock" /**< The namespace of both keys.     */
#define WALL_NVS_KEY         "wall"   /**< Seconds since the epoch, u64.   */
#define WALL_NVS_KEY_APPROX  "approx" /**< u8: 1 while the value is a
                                       *   restored one.                   */
#define US_PER_S             1000000ULL /**< Microseconds in a second.     */
#define MS_PER_S             1000U      /**< Milliseconds in a second.     */

static bool s_wall_marked; /**< What NVS holds for the approximate mark.  */
static bool s_wall_tried;  /**< The mark the last write of it asked for.  */

bool kdrv_clocks_wall_approx(void)
{
    return (KDRV_CLOCKS_WALL_SAVED == s_wall_src);
}

/**
 * @brief Read the kept value, as this SDK writes it or as an older one did.
 *
 * Images built on an older SDK keep it as an 8-byte blob, and NVS will not read
 * a value by another type: a board that ran one of them and then lost its power
 * came up with no time at all. The next keep writes it as a u64 again.
 *
 * @param out Receives the seconds.
 * @return true when there is a value.
 */
static bool wall_saved(uint64_t *out)
{
    bool found = (khal_nvs_get_u64(WALL_NVS_NS, WALL_NVS_KEY, out) == KODE_OK);

    if ( !found )
    {
        size_t len = sizeof(*out);

        found = (khal_nvs_get_blob(WALL_NVS_NS, WALL_NVS_KEY, out, &len) == KODE_OK)
                && (sizeof(*out) == len);
    }
    return found;
}

kode_status_t kdrv_clocks_wall_restore(void)
{
    uint8_t  mark  = 0U;
    uint64_t saved = 0U;
    kode_status_t rc = KODE_OK;

    s_wall_kept   = false;
    s_wall_marked = (khal_nvs_get_u8(WALL_NVS_NS, WALL_NVS_KEY_APPROX, &mark)
                    == KODE_OK) && (0U != mark);

    if ( khal_time_wall_known() )
    {
        /* The chip kept its supply. The value is the one it held, run on:
         * a restored one still, if that is what NVS says it was. */
        s_wall_src = KDRV_CLOCKS_WALL_RTC;
        if ( s_wall_marked )
        {
            s_wall_src = KDRV_CLOCKS_WALL_SAVED;
        }
    }
    else if ( wall_saved(&saved) && ((uint64_t)KODE_CLOCK_KNOWN_AFTER_S < saved) )
    {
        char iso[ISO_LEN];

        khal_time_wall_set(saved * US_PER_S);
        s_wall_src = KDRV_CLOCKS_WALL_SAVED;
        /* Marked now, not at the first keep: the reset that can follow
         * within two seconds - a port opened - must find the mark. */
        if ( khal_nvs_set_u8(WALL_NVS_NS, WALL_NVS_KEY_APPROX, 1U) == KODE_OK )
        {
            s_wall_marked = true;
        }
        if ( kdrv_clocks_wall_iso(iso, sizeof(iso)) == KODE_OK )
        {
            KLOG_N(TAG, "the clock lost its power; restored to %s, approximate "
                        "until the network or a person sets it", iso);
        }
    }
    else
    {
        rc = KODE_DENIED;
    }
    s_wall_tried = s_wall_marked;
    return rc;
}

bool kdrv_clocks_wall_keep(bool force)
{
    bool wrote = false;

    if ( khal_time_wall_known() )
    {
        static uint64_t kept_wall_s; /* what the last write wrote        */
        static uint32_t kept_ms;     /* when, on the monotonic clock     */
        uint64_t const now_s = khal_time_wall_us() / US_PER_S;
        uint32_t const ms    = khal_time_ms();
        bool due = force || !s_wall_kept;
        bool const approx = kdrv_clocks_wall_approx();

        if ( !due )
        {
            uint32_t const ran_s = (ms - kept_ms) / MS_PER_S;
            int64_t const drift = (int64_t)now_s
                                  - ((int64_t)kept_wall_s + (int64_t)ran_s);

            due = (KDRV_CLOCKS_WALL_KEEP_S <= ran_s)
                  || ((int64_t)KDRV_CLOCKS_WALL_JUMP_S <= drift)
                  || (-(int64_t)KDRV_CLOCKS_WALL_JUMP_S >= drift);
        }
        if ( due )
        {
            /* The schedule moves on whether the write lands or not: a store
             * that refuses once is asked again at the next period, not at
             * every poll. */
            s_wall_kept = true;
            kept_wall_s = now_s;
            kept_ms     = ms;
            wrote = (khal_nvs_set_u64(WALL_NVS_NS, WALL_NVS_KEY, now_s)
                     == KODE_OK);
        }
        /* A mark that changed is written at once; one the store refused
         * waits for the clock's own schedule, not the next poll. */
        if ( (approx != s_wall_marked) && (due || (approx != s_wall_tried)) )
        {
            uint8_t mark = 0U;

            if ( approx )
            {
                mark = 1U;
            }
            s_wall_tried = approx;
            if ( khal_nvs_set_u8(WALL_NVS_NS, WALL_NVS_KEY_APPROX, mark)
                == KODE_OK )
            {
                s_wall_marked = approx;
            }
        }
    }
    return wrote;
}

/**
 * @brief Days in a month of the Gregorian calendar.
 *
 * @param year  The year.
 * @param month The month, 1 to 12.
 * @return Its days.
 */
static int month_days(int year, int month)
{
    /* Days of each month in a common year, January first. */
    static const uint8_t MONTH_DAYS[ISO_MONTHS] = {
        31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U
    };
    int days = (int)MONTH_DAYS[month - 1];
    bool const leap = ((0 == (year % ISO_LEAP_EVERY)) && (0 != (year % ISO_LEAP_SKIP)))
                      || (0 == (year % ISO_LEAP_KEEP));

    if ( (ISO_FEBRUARY == month) && leap )
    {
        days = ISO_LEAP_DAYS;
    }
    return days;
}

/**
 * @brief Parse `YYYY-MM-DDThh:mm:ssZ` into its seven fields.
 *
 * Hand-written rather than strptime() because the point is to refuse
 * everything that is not exactly this shape, and strptime is built to be
 * forgiving - it would take a missing zone and a two-digit year and hand
 * back something plausible.
 *
 * @param[in]  iso The candidate.
 * @param[out] tm  Filled only on success.
 * @return true when @p iso is exactly the accepted shape, names a day that
 *         exists and its year is at most KDRV_CLOCKS_WALL_YEAR_MAX.
 */
static bool iso_parse(const char *iso, struct tm *tm)
{
    static const char *const SHAPE = "dddd-dd-ddTdd:dd:ddZ";

    if ( strnlen(iso, ISO_LEN) != (ISO_LEN - 1U) )
    {
        return false;
    }
    for ( size_t i = 0U; (ISO_LEN - 1U) > i; ++i )
    {
        if ( 'd' == SHAPE[i] )
        {
            if ( ('0' > iso[i]) || ('9' < iso[i]) )
            {
                return false;
            }
        }
        else if ( iso[i] != SHAPE[i] )
        {
            return false;
        }
        else
        {
            /* A literal that matched. */
        }
    }

    int const y  = ((iso[0] - '0') * 1000) + ((iso[1] - '0') * 100)
                   + ((iso[2] - '0') * 10) + (iso[3] - '0');
    int const mo = ((iso[5] - '0') * 10) + (iso[6] - '0');
    int const d  = ((iso[8] - '0') * 10) + (iso[9] - '0');
    int const h  = ((iso[11] - '0') * 10) + (iso[12] - '0');
    int const mi = ((iso[14] - '0') * 10) + (iso[15] - '0');
    int const sc = ((iso[17] - '0') * 10) + (iso[18] - '0');

    if ( (1 > mo) || (ISO_MONTHS < mo) || (1 > d) || (23 < h) || (59 < mi) || (59 < sc)
        || ((int)KDRV_CLOCKS_WALL_YEAR_MAX < y) )
    {
        return false;
    }
    if ( d > month_days(y, mo) )
    {
        return false;
    }

    tm->tm_year  = y - 1900;
    tm->tm_mon   = mo - 1;
    tm->tm_mday  = d;
    tm->tm_hour  = h;
    tm->tm_min   = mi;
    tm->tm_sec   = sc;
    tm->tm_isdst = 0;

    return true;
}

/**
 * @brief Seconds since the Unix epoch for a calendar time in UTC.
 *
 * What timegm() does, written out because the C library does not always carry
 * it: picolibc (ESP-IDF 6) has it, newlib (ESP-IDF 5.5) has neither the
 * declaration nor the symbol. The day count is Howard Hinnant's
 * days_from_civil over the proleptic Gregorian calendar: the year is taken to
 * start in March, so a leap day is the last day of the year it belongs to and
 * the month lengths before it never depend on whether there is one.
 *
 * The fields are taken the way iso_parse() leaves them, month 0..11 and a
 * day that exists in its month.
 *
 * @param[in] tm A calendar time in UTC; tm_isdst, tm_wday and tm_yday are
 *               ignored.
 * @return The instant, in seconds since 1970-01-01T00:00:00Z.
 */
static int64_t utc_seconds(const struct tm *tm)
{
    int64_t const mon  = (int64_t)tm->tm_mon + 1;                  /* 1..12  */
    int64_t const year = ((int64_t)tm->tm_year + 1900) - ((2 >= mon) ? 1 : 0);
    int64_t const era  = ((0 <= year) ? year : (year - 399)) / 400;
    int64_t const yoe  = year - (era * 400);                       /* 0..399 */
    int64_t const doy  = (((153 * (mon + ((2 < mon) ? -3 : 9))) + 2) / 5)
                         + ((int64_t)tm->tm_mday - 1);             /* 0..365 */
    int64_t const doe  = ((yoe * 365) + (yoe / 4)) - (yoe / 100) + doy;
    int64_t const days = ((era * 146097) + doe) - 719468;

    return (days * 86400) + ((int64_t)tm->tm_hour * 3600)
           + ((int64_t)tm->tm_min * 60) + (int64_t)tm->tm_sec;
}

kode_status_t kdrv_clocks_wall_set_iso(const char *iso)
{
    struct tm tm;

    if ( NULL == iso )
    {
        return KODE_DENIED;
    }
    (void)memset(&tm, 0, sizeof(tm));
    if ( !iso_parse(iso, &tm) )
    {
        return KODE_DENIED;
    }

    int64_t const secs = utc_seconds(&tm);
    if ( (int64_t)KODE_CLOCK_KNOWN_AFTER_S >= secs )
    {
        return KODE_DENIED;
    }

    khal_time_wall_set((uint64_t)secs * 1000000ULL);
    kdrv_clocks_wall_note_set();

    return KODE_OK;
}

kode_status_t kdrv_clocks_wall_iso(char *out, size_t n)
{
    struct tm tm;

    if ( (NULL == out) || (ISO_LEN > n) || !khal_time_wall_known() )
    {
        return KODE_DENIED;
    }

    time_t const secs = (time_t)(khal_time_wall_us() / 1000000ULL);
    if ( gmtime_r(&secs, &tm) == NULL )
    {
        return KODE_DENIED;
    }

    /* gmtime_r, not localtime_r: the board keeps UTC, and the local-time
     * machinery in newlib is weight bought for nothing. */
    (void)snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                   tm.tm_hour, tm.tm_min, tm.tm_sec);

    return KODE_OK;
}
