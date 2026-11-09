/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_compass.c
 * @brief kode_mag_calibrate(): the guided sweep of the field and its fit.
 */
#include "kode.h"

#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "khal_system.h"
#include "kode_screen_priv.h"

#define COMPASS_PERCENT        100U    /**< Progress once every span agrees with the widest.   */
#define COMPASS_WHO            "kode_mag_calibrate"  /**< The call, as errors name it.          */
#define COMPASS_TITLE          "Compass"                              /**< The screen's title.        */
#define COMPASS_ASK            "Turn the Dot in every direction"      /**< What the screen asks.      */
#define COMPASS_DONE           "Compass ready"                        /**< What it says on success. */
#define COMPASS_AGAIN          "Try again"                            /**< And when it failed.      */
#define COMPASS_NOT_ENOUGH     "not enough directions were covered"   /**< The error of a thin sweep. */
#define COMPASS_UNSETTLED      "the Dot was still reaching new directions when the time ran out; keep turning it" /**< Still growing at the limit. */
#define COMPASS_NO_MEMORY      "no memory for the calibration"        /**< The readings' block was refused. */
#define COMPASS_OUT_OF_RANGE   "the field is too strong to calibrate" /**< The error of a fit past the limits. */
#define COMPASS_WHY_MAX        128U    /**< Bytes of the error kept across the last words. */
#define COMPASS_READS_ALIGN    4U      /**< Alignment of the readings' block.                    */
#define COMPASS_HALF           0.5F    /**< A range's middle and half width are half its ends' sum and gap. */

/** @brief The least and greatest reading on each axis, and when a span last grew. */
typedef struct {
    int32_t  lo[KDRV_QMC5883P_AXES];    /**< Least, mG.                              */
    int32_t  hi[KDRV_QMC5883P_AXES];    /**< Greatest, mG.                           */
    int32_t  mark[KDRV_QMC5883P_AXES];  /**< The spans when one last grew, mG.       */
    uint32_t grew_ms;                   /**< kode_uptime_ms() of that growth.         */
    int16_t *kept;                      /**< The readings, KDRV_QMC5883P_AXES each.  */
    uint32_t n;                         /**< Readings kept, up to KODE_COMPASS_READS_MAX. */
} sweep_t;

/** @brief Start a sweep that has seen nothing. */
static void sweep_start(sweep_t *s, uint32_t now)
{
    for ( uint32_t i = 0U; KDRV_QMC5883P_AXES > i; i++ )
    {
        s->lo[i]   = INT32_MAX;
        s->hi[i]   = INT32_MIN;
        s->mark[i] = 0;
    }
    s->grew_ms = now;
    s->n       = 0U;
}

/** @brief The range axis @p i has covered, mG; 0 before its first reading. */
static int32_t sweep_axis(const sweep_t *s, uint32_t i)
{
    int32_t span = 0;

    if ( s->hi[i] >= s->lo[i] )
    {
        span = s->hi[i] - s->lo[i];
    }
    return span;
}

/** @brief Take one reading into the sweep, noting when a span grows past its mark. */
static void sweep_add(sweep_t *s, const int32_t mg[KDRV_QMC5883P_AXES], uint32_t now)
{
    bool grew = false;

    if ( KODE_COMPASS_READS_MAX > s->n )
    {
        for ( uint32_t i = 0U; KDRV_QMC5883P_AXES > i; i++ )
        {
            int32_t v = mg[i];

            if ( INT16_MAX < v )
            {
                v = INT16_MAX;
            }
            if ( INT16_MIN > v )
            {
                v = INT16_MIN;
            }
            s->kept[(s->n * KDRV_QMC5883P_AXES) + i] = (int16_t)v;
        }
        s->n++;
    }
    for ( uint32_t i = 0U; KDRV_QMC5883P_AXES > i; i++ )
    {
        if ( mg[i] < s->lo[i] )
        {
            s->lo[i] = mg[i];
        }
        if ( mg[i] > s->hi[i] )
        {
            s->hi[i] = mg[i];
        }
        if ( sweep_axis(s, i) > (s->mark[i] + KODE_COMPASS_GROWTH_MG) )
        {
            grew = true;
        }
    }
    if ( grew )
    {
        for ( uint32_t i = 0U; KDRV_QMC5883P_AXES > i; i++ )
        {
            s->mark[i] = sweep_axis(s, i);
        }
        s->grew_ms = now;
    }
}

/**
 * @brief How far the spans agree, per cent of KODE_COMPASS_AGREE_PCT.
 *
 * The widest span is twice the field; 0 until it reaches KODE_COMPASS_SPAN_MIN_MG.
 * Readings are 16-bit, so no product overflows.
 */
static uint32_t sweep_percent(const sweep_t *s)
{
    int32_t  least = INT32_MAX;
    int32_t  most  = 0;
    uint32_t pct   = 0U;

    for ( uint32_t i = 0U; KDRV_QMC5883P_AXES > i; i++ )
    {
        const int32_t span = sweep_axis(s, i);

        if ( span < least )
        {
            least = span;
        }
        if ( span > most )
        {
            most = span;
        }
    }
    if ( KODE_COMPASS_SPAN_MIN_MG <= most )
    {
        pct = ((uint32_t)least * COMPASS_PERCENT * COMPASS_PERCENT) / ((uint32_t)most * KODE_COMPASS_AGREE_PCT);
        if ( COMPASS_PERCENT < pct )
        {
            pct = COMPASS_PERCENT;
        }
    }
    return pct;
}

/**
 * @brief Read the field until the spans agree and have settled, or the time is up.
 * @param io   The readings.
 * @param s    The sweep.
 * @param prog The progress value on the screen.
 * @param done Out: true when the sweep agreed and settled in time.
 * @return false when a reading failed, with the error set.
 */
static bool sweep_collect(const kode_compass_io_t *io, sweep_t *s, kode_widget_t prog, bool *done)
{
    const uint32_t start = kode_uptime_ms();
    uint32_t       shown = 0U;
    bool           ok    = true;
    bool           more  = true;

    sweep_start(s, start);
    *done = false;
    while ( ok && more )
    {
        int32_t mg[KDRV_QMC5883P_AXES];

        ok = io->read(mg);
        if ( ok )
        {
            const uint32_t now = kode_uptime_ms();
            uint32_t       pct;

            sweep_add(s, mg, now);
            pct = sweep_percent(s);
            if ( pct != shown )
            {
                shown = pct;
                (void)kode_widget_set_text(prog, "%" PRIu32, pct);
            }
            *done = (COMPASS_PERCENT == pct) && ((now - s->grew_ms) >= KODE_COMPASS_SETTLE_MS);
            more  = (!*done) && ((now - start) < KODE_MAG_CALIBRATE_MS) && (KODE_COMPASS_READS_MAX > s->n);
            if ( more )
            {
                kode_delay(KODE_COMPASS_STEP_MS);
            }
        }
    }
    return ok;
}

/**
 * @brief Whether the readings lie on one sphere about the sweep's centre.
 *
 * Each reading, centred on the middle of each axis's range and divided by its
 * half range, has a length; a sweep that covered every direction gives them
 * all the same, a partial cover does not.
 *
 * @param s The sweep, every span above 0.
 * @return true when the longest and shortest are within KODE_COMPASS_ROUND_PCT.
 */
static bool sweep_round(const sweep_t *s)
{
    float centre[KDRV_QMC5883P_AXES];
    float half[KDRV_QMC5883P_AXES];
    float least = FLT_MAX;
    float most  = 0.0F;
    bool  ok    = (0U < s->n);

    for ( uint32_t i = 0U; KDRV_QMC5883P_AXES > i; i++ )
    {
        centre[i] = ((float)s->lo[i] + (float)s->hi[i]) * COMPASS_HALF;
        half[i]   = ((float)s->hi[i] - (float)s->lo[i]) * COMPASS_HALF;
        if ( 0.0F >= half[i] )
        {
            ok = false;
        }
    }
    for ( uint32_t k = 0U; ok && (k < s->n); k++ )
    {
        float sum = 0.0F;

        for ( uint32_t i = 0U; KDRV_QMC5883P_AXES > i; i++ )
        {
            const float d = ((float)s->kept[(k * KDRV_QMC5883P_AXES) + i] - centre[i]) / half[i];

            sum += d * d;
        }
        const float m = sqrtf(sum);

        if ( m < least )
        {
            least = m;
        }
        if ( m > most )
        {
            most = m;
        }
    }
    if ( ok )
    {
        ok = ((most - least) * (float)COMPASS_PERCENT) <= (float)KODE_COMPASS_ROUND_PCT;
    }
    return ok;
}

/**
 * @brief Judge the sweep and keep what it gives.
 * @param io   The store.
 * @param s    The sweep.
 * @param done The sweep agreed and settled in time.
 * @return false with the error set when it covered too little or the store refused.
 */
static bool sweep_keep(const kode_compass_io_t *io, const sweep_t *s, bool done)
{
    kdrv_qmc5883p_cal_t corr;
    bool                ok = false;

    if ( !done )
    {
        if ( sweep_percent(s) < COMPASS_PERCENT )
        {
            kode_err_set(COMPASS_NOT_ENOUGH);
        }
        else
        {
            kode_err_set(COMPASS_UNSETTLED);
        }
    }
    else if ( kdrv_qmc5883p_cal_fit(s->lo, s->hi, &corr) != KODE_OK )
    {
        kode_err_set(COMPASS_OUT_OF_RANGE);
    }
    else if ( !sweep_round(s) )
    {
        kode_err_set(COMPASS_NOT_ENOUGH);
    }
    else
    {
        ok = io->store(&corr);
    }
    return ok;
}

bool kode_compass_run(const kode_compass_io_t *io)
{
    bool ok = false;

    if ( kode_enter(COMPASS_WHO) && kode_screen_new(COMPASS_TITLE) )
    {
        const kode_widget_t ask  = kode_screen_add_text(COMPASS_ASK);
        const kode_widget_t prog = kode_screen_add_value("0", "%");

        if ( (0U != ask) && (0U != prog) )
        {
            sweep_t sweep;
            bool    done = false;
            char    why[COMPASS_WHY_MAX];

            /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block is readings */
            sweep.kept = (int16_t *)khal_alloc_psram(COMPASS_READS_ALIGN, sizeof(int16_t) * KDRV_QMC5883P_AXES
                                                     * KODE_COMPASS_READS_MAX);
            if ( NULL == sweep.kept )
            {
                kode_err_set(COMPASS_NO_MEMORY);
            }
            else
            {
                ok = sweep_collect(io, &sweep, prog, &done) && sweep_keep(io, &sweep, done);
                khal_free(sweep.kept);
            }
            /* kode_widget_set_text() clears the task's error, so the reason is kept across it. */
            (void)snprintf(why, sizeof(why), "%s", kode_error());
            if ( ok )
            {
                (void)kode_widget_set_text(ask, "%s", COMPASS_DONE);
                (void)kode_enter(COMPASS_WHO);   /* the call worked: no error, whatever the words met */
            }
            else
            {
                (void)kode_widget_set_text(ask, "%s", COMPASS_AGAIN);
                kode_err_set("%s", why);
            }
        }
    }
    return ok;
}

/** @brief One raw reading, under the name of kode_mag_calibrate(). */
static bool compass_read(int32_t mg[KDRV_QMC5883P_AXES])
{
    return kode_mag_raw_read(COMPASS_WHO, mg);
}

/** @brief Keep the calibration, under the name of kode_mag_calibrate(). */
static bool compass_store(const kdrv_qmc5883p_cal_t *corr)
{
    return kode_mag_cal_store(COMPASS_WHO, corr);
}

bool kode_mag_calibrate(void)
{
    static const kode_compass_io_t io = { &compass_read, &compass_store };

    return kode_compass_run(&io);
}
