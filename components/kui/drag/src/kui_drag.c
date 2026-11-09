/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_drag.c
 * @brief The follower's rules: resistance, the release decision and the landing on a spring.
 */
#include "kui_drag.h"

#include <stddef.h>

#define DRAG_MS_PER_S   1000.0F      /**< Milliseconds in a second. */
#define DRAG_PERCENT    100.0F       /**< A whole, in percent. */
#define DRAG_HALF       0.5F         /**< Added to a positive float before it is cut, to round to the nearest integer. */
#define DRAG_SMOOTH_A   3.0F         /**< The smoothstep's cubic is u^2 (A - B u): this is A. */
#define DRAG_SMOOTH_B   2.0F         /**< The smoothstep's cubic is u^2 (A - B u): this is B. */

/**
 * @brief Keeps a value between two bounds.
 *
 * @param v  The value.
 * @param lo The lower bound.
 * @param hi The upper bound.
 * @return v, or the bound it passed.
 */
static float drag_clamp(float v, float lo, float hi)
{
    if ( v < lo )
    {
        return lo;
    }
    if ( v > hi )
    {
        return hi;
    }
    return v;
}

/**
 * @brief A float cut to the nearest integer, halves away from zero.
 *
 * @param v The value, within the range of an int32_t.
 * @return The nearest integer.
 */
static int32_t drag_nearest(float v)
{
    if ( v < 0.0F )
    {
        return -(int32_t)(DRAG_HALF - v);
    }
    return (int32_t)(v + DRAG_HALF);
}

/**
 * @brief The offset of the end the page is at or going to.
 *
 * @param d The page.
 * @return The span for the open end, 0 for the closed one.
 */
static float drag_end_px(const kui_drag_t *d)
{
    if ( d->target_open )
    {
        return (float)d->span;
    }
    return 0.0F;
}

/**
 * @brief How much of a finger's distance the page shows, with the resistance at the start.
 *
 * The page keeps the share of the distance up to KUI_DRAG_RESIST_FROM_PX, and
 * the part it gave up fades along a smoothstep until KUI_DRAG_RESIST_TO_PX,
 * where it is none. Written as the distance less what it holds back, so past
 * that point the answer is exactly the distance.
 *
 * @param dist The finger's distance from the grab, not negative.
 * @return The distance the page moves.
 */
static float drag_resist(float dist)
{
    float const share = (float)KUI_DRAG_RESIST_SHARE_PCT / DRAG_PERCENT;
    float const ramp = (float)KUI_DRAG_RESIST_TO_PX - (float)KUI_DRAG_RESIST_FROM_PX;
    float const u = drag_clamp((dist - (float)KUI_DRAG_RESIST_FROM_PX) / ramp, 0.0F, 1.0F);
    float const smooth = (u * u) * (DRAG_SMOOTH_A - (DRAG_SMOOTH_B * u));

    return dist - ((dist * (1.0F - share)) * (1.0F - smooth));
}

/**
 * @brief The distance the page moves for the finger at pos.
 *
 * @param d   The page.
 * @param pos The finger's position.
 * @return The signed distance from the page at the grab, with resistance when the drag has it.
 */
static float drag_shown(const kui_drag_t *d, int32_t pos)
{
    int32_t const moved = pos - d->grab_pos;
    float const delta = (float)moved;

    if ( !d->resist )
    {
        return delta;
    }
    if ( delta < 0.0F )
    {
        return -drag_resist(-delta);
    }
    return drag_resist(delta);
}

/**
 * @brief Writes a finger sample into the ring, over the oldest when it is full.
 *
 * @param d    The page.
 * @param pos  The finger's position.
 * @param t_ms The clock when it was sampled.
 */
static void drag_record(kui_drag_t *d, int32_t pos, uint32_t t_ms)
{
    d->samples[d->head].pos = pos;
    d->samples[d->head].t_ms = t_ms;
    d->head = (uint8_t)(((uint32_t)d->head + 1U) % KUI_DRAG_SAMPLES);
    if ( d->n_samples < KUI_DRAG_SAMPLES )
    {
        d->n_samples++;
    }
}

/**
 * @brief A sample counted back from the newest.
 *
 * @param d The page.
 * @param k How many samples back, 0 for the newest; less than the samples held.
 * @return The sample.
 */
static const kui_drag_sample_t *drag_back(const kui_drag_t *d, uint8_t k)
{
    uint32_t const slot = (((uint32_t)d->head + KUI_DRAG_SAMPLES) - 1U - (uint32_t)k) % KUI_DRAG_SAMPLES;

    return &d->samples[slot];
}

/**
 * @brief The finger's speed over the samples inside the window.
 *
 * A sample is inside the window when kui_age_ms() of its stamp is at most
 * KUI_DRAG_VEL_WINDOW_MS, so one stamped ahead of t_ms is inside.
 *
 * @param d    The page.
 * @param t_ms The clock now.
 * @return Pixels per millisecond, signed toward open; 0 without two samples or without time between them.
 */
static float drag_speed(const kui_drag_t *d, uint32_t t_ms)
{
    uint8_t seen = 0U;

    while ( (seen < d->n_samples) && (kui_age_ms(drag_back(d, seen)->t_ms, t_ms) <= KUI_DRAG_VEL_WINDOW_MS) )
    {
        seen++;
    }
    if ( seen < KUI_DRAG_SPEED_MIN_SAMPLES )
    {
        return 0.0F;
    }

    kui_drag_sample_t const *const newest = drag_back(d, 0U);
    kui_drag_sample_t const *const oldest = drag_back(d, (uint8_t)(seen - 1U));
    uint32_t const dt = newest->t_ms - oldest->t_ms;
    if ( 0U == dt )
    {
        return 0.0F;
    }
    int32_t const travel = newest->pos - oldest->pos;
    return (float)travel / (float)dt;
}

/**
 * @brief The release decision for a speed.
 *
 * @param d           The page, following.
 * @param speed       The finger's speed in pixels per millisecond.
 * @param by_position true to ignore the speed.
 * @return true when the page goes to the open end.
 */
static bool drag_decide(const kui_drag_t *d, float speed, bool by_position)
{
    float const fling = (float)KUI_DRAG_FLING_PX_S / DRAG_MS_PER_S;

    if ( !by_position )
    {
        if ( speed > fling )
        {
            return true;
        }
        if ( speed < -fling )
        {
            return false;
        }
    }
    return (d->off + d->off) >= (float)d->span;
}

/**
 * @brief Where a settling page is at t_ms, and how fast it moves.
 *
 * @param d    The page, settling.
 * @param t_ms The clock now.
 * @param v    Receives the speed in pixels per millisecond.
 * @return The offset, kept between 0 and the span.
 */
static float drag_settle_now(const kui_drag_t *d, uint32_t t_ms, float *v)
{
    float pos = 0.0F;

    kui_spring_at(&d->spring, t_ms, &pos, v);
    return drag_clamp(pos, 0.0F, (float)d->span);
}

/**
 * @brief Starts the spring from the page's offset toward its target.
 *
 * @param d    The page.
 * @param v0   The start speed in pixels per millisecond.
 * @param t_ms The clock now.
 */
static void drag_start_spring(kui_drag_t *d, float v0, uint32_t t_ms)
{
    kui_spring_start(&d->spring, d->off, v0, drag_end_px(d), KUI_DRAG_SPRING_MS, KUI_SPRING_CRITICAL, t_ms);
    d->phase = KUI_DRAG_SETTLE;
}

void kui_drag_init(kui_drag_t *d, int32_t span_px)
{
    uint32_t i;

    if ( NULL == d )
    {
        return;
    }
    d->span = 0;
    if ( span_px > 0 )
    {
        d->span = span_px;
    }
    d->grab_pos = 0;
    d->grab_off = 0.0F;
    d->n_samples = 0U;
    d->head = 0U;
    for ( i = 0U; i < KUI_DRAG_SAMPLES; i++ )
    {
        d->samples[i].pos = 0;
        d->samples[i].t_ms = 0U;
    }
    kui_drag_set(d, false);
}

void kui_drag_set(kui_drag_t *d, bool open)
{
    if ( NULL == d )
    {
        return;
    }
    d->phase = KUI_DRAG_IDLE;
    d->resist = false;
    d->target_open = open;
    d->off = drag_end_px(d);
    kui_spring_start(&d->spring, d->off, 0.0F, d->off, KUI_DRAG_SPRING_MS, KUI_SPRING_CRITICAL, 0U);
}

void kui_drag_grab(kui_drag_t *d, int32_t pos, uint32_t t_ms)
{
    if ( NULL == d )
    {
        return;
    }
    d->resist = (KUI_DRAG_IDLE == d->phase);
    kui_drag_catch(d, t_ms);
    d->grab_pos = pos;
    d->grab_off = d->off;
    d->n_samples = 0U;
    d->head = 0U;
    drag_record(d, pos, t_ms);
    d->phase = KUI_DRAG_FOLLOW;
}

void kui_drag_catch(kui_drag_t *d, uint32_t t_ms)
{
    float v = 0.0F;

    if ( (NULL == d) || (KUI_DRAG_SETTLE != d->phase) )
    {
        return;
    }
    d->off = drag_settle_now(d, t_ms, &v);
    d->phase = KUI_DRAG_HELD;
}

void kui_drag_follow(kui_drag_t *d, int32_t pos, uint32_t t_ms)
{
    if ( (NULL == d) || (KUI_DRAG_FOLLOW != d->phase) )
    {
        return;
    }
    d->off = drag_clamp(d->grab_off + drag_shown(d, pos), 0.0F, (float)d->span);
    drag_record(d, pos, t_ms);
}

bool kui_drag_release(kui_drag_t *d, uint32_t t_ms, bool by_position)
{
    float v = 0.0F;

    if ( NULL == d )
    {
        return false;
    }
    if ( (KUI_DRAG_FOLLOW != d->phase) && (KUI_DRAG_HELD != d->phase) )
    {
        return d->target_open;
    }
    if ( KUI_DRAG_FOLLOW == d->phase )
    {
        v = drag_speed(d, t_ms);
        d->target_open = drag_decide(d, v, by_position);
    }
    drag_start_spring(d, v, t_ms);
    return d->target_open;
}

void kui_drag_aim(kui_drag_t *d, bool open, uint32_t t_ms)
{
    float v = 0.0F;

    if ( NULL == d )
    {
        return;
    }
    if ( KUI_DRAG_SETTLE == d->phase )
    {
        d->off = drag_settle_now(d, t_ms, &v);
    }
    d->target_open = open;
    drag_start_spring(d, v, t_ms);
}

bool kui_drag_step(kui_drag_t *d, uint32_t t_ms)
{
    float pos = 0.0F;
    bool landed;

    if ( (NULL == d) || (KUI_DRAG_SETTLE != d->phase) )
    {
        return false;
    }
    kui_spring_at(&d->spring, t_ms, &pos, NULL);
    d->off = pos;
    landed = (kui_age_ms(d->spring.t0, t_ms) >= KUI_DRAG_LAND_MAX_MS)
             || kui_spring_rested(&d->spring, t_ms, KUI_DRAG_REST_PX, KUI_DRAG_REST_SPEED_PX_MS)
             || (d->off < 0.0F) || (d->off > (float)d->span);
    if ( landed )
    {
        d->off = drag_end_px(d);
        d->phase = KUI_DRAG_IDLE;
    }
    return landed;
}

void kui_drag_finish(kui_drag_t *d)
{
    if ( (NULL == d) || ((KUI_DRAG_SETTLE != d->phase) && (KUI_DRAG_HELD != d->phase)) )
    {
        return;
    }
    d->off = drag_end_px(d);
    d->phase = KUI_DRAG_IDLE;
}

int32_t kui_drag_offset(const kui_drag_t *d)
{
    if ( NULL == d )
    {
        return 0;
    }
    return drag_nearest(d->off);
}

int32_t kui_drag_speed_px_s(const kui_drag_t *d, uint32_t t_ms)
{
    if ( NULL == d )
    {
        return 0;
    }
    return drag_nearest(drag_speed(d, t_ms) * DRAG_MS_PER_S);
}

bool kui_drag_would_open(const kui_drag_t *d, uint32_t t_ms, bool by_position)
{
    if ( NULL == d )
    {
        return false;
    }
    if ( KUI_DRAG_FOLLOW != d->phase )
    {
        return d->target_open;
    }
    return drag_decide(d, drag_speed(d, t_ms), by_position);
}

kui_drag_phase_t kui_drag_phase(const kui_drag_t *d)
{
    if ( NULL == d )
    {
        return KUI_DRAG_IDLE;
    }
    return d->phase;
}

bool kui_drag_is_open(const kui_drag_t *d)
{
    if ( NULL == d )
    {
        return false;
    }
    return (KUI_DRAG_IDLE == d->phase) && d->target_open;
}
