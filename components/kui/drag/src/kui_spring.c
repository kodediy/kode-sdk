/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_spring.c
 * @brief The spring's closed forms, critical and underdamped, from a start and the clock.
 */
#include "kui_spring.h"

#include <math.h>
#include <stddef.h>

#define SPRING_TAU    6.28318531F /**< A full turn in radians: the natural frequency is this over the period. */
#define AGE_AHEAD_BIT 0x80000000U /**< The top bit of a time difference: set when the stamp is ahead of the clock. */

/**
 * @brief Keeps a damping ratio inside what a spring is given.
 *
 * The first test is written so that a NaN fails it and is 0, like a negative.
 *
 * @param zeta The damping ratio asked for.
 * @return zeta, from 0 up to KUI_SPRING_CRITICAL.
 */
static float spring_zeta(float zeta)
{
    if ( !(0.0F <= zeta) )
    {
        return 0.0F;
    }
    if ( zeta > KUI_SPRING_CRITICAL )
    {
        return KUI_SPRING_CRITICAL;
    }
    return zeta;
}

/**
 * @brief The critically damped form, x(t) = (x0 + (v0 + w x0) t) e^(-w t).
 *
 * @param s The spring.
 * @param t The time since the start, in milliseconds.
 * @param x Receives the distance from the target, in pixels.
 * @param v Receives the speed, in pixels per millisecond.
 */
static void spring_critical(const kui_spring_t *s, float t, float *x, float *v)
{
    float const e = expf(-(s->w * t));
    float const k = s->v0 + (s->w * s->x0);

    *x = (s->x0 + (k * t)) * e;
    *v = (s->v0 - ((s->w * k) * t)) * e;
}

/**
 * @brief The underdamped form, with a = zeta w and wd = w sqrt(1 - zeta^2).
 *
 * x(t) = e^(-a t) (x0 cos(wd t) + ((v0 + a x0) / wd) sin(wd t)) and its
 * derivative. The damping is below KUI_SPRING_CRITICAL, so wd is not zero.
 *
 * @param s The spring.
 * @param t The time since the start, in milliseconds.
 * @param x Receives the distance from the target, in pixels.
 * @param v Receives the speed, in pixels per millisecond.
 */
static void spring_under(const kui_spring_t *s, float t, float *x, float *v)
{
    float const a = s->zeta * s->w;
    float const wd = s->w * sqrtf(1.0F - (s->zeta * s->zeta));
    float const e = expf(-(a * t));
    float const c = cosf(wd * t);
    float const n = sinf(wd * t);
    float const kx = (s->v0 + (a * s->x0)) / wd;
    float const kv = ((a * s->v0) + ((s->w * s->w) * s->x0)) / wd;

    *x = e * ((s->x0 * c) + (kx * n));
    *v = e * ((s->v0 * c) - (kv * n));
}

/**
 * @brief The state of a spring at t_ms, as a distance from its target.
 *
 * @param s    The spring.
 * @param t_ms The clock now.
 * @param x    Receives the distance from the target, in pixels.
 * @param v    Receives the speed, in pixels per millisecond.
 */
static void spring_eval(const kui_spring_t *s, uint32_t t_ms, float *x, float *v)
{
    float const t = (float)kui_age_ms(s->t0, t_ms);

    if ( s->zeta >= KUI_SPRING_CRITICAL )
    {
        spring_critical(s, t, x, v);
    }
    else
    {
        spring_under(s, t, x, v);
    }
}

uint32_t kui_age_ms(uint32_t stamp_ms, uint32_t now_ms)
{
    uint32_t const diff = now_ms - stamp_ms;

    if ( 0U != (diff & AGE_AHEAD_BIT) )
    {
        return 0U;
    }
    return diff;
}

void kui_spring_start(kui_spring_t *s, float pos, float vel_px_per_ms, float target, uint32_t period_ms, float zeta,
                      uint32_t t_ms)
{
    if ( NULL == s )
    {
        return;
    }
    s->target = target;
    s->t0 = t_ms;
    if ( 0U == period_ms )
    {
        /* No time to land: the critical form with nothing to move is at the target at any time. */
        s->x0 = 0.0F;
        s->v0 = 0.0F;
        s->w = 0.0F;
        s->zeta = KUI_SPRING_CRITICAL;
    }
    else
    {
        s->x0 = pos - target;
        s->v0 = vel_px_per_ms;
        s->w = SPRING_TAU / (float)period_ms;
        s->zeta = spring_zeta(zeta);
    }
}

void kui_spring_at(const kui_spring_t *s, uint32_t t_ms, float *pos, float *vel_px_per_ms)
{
    float x = 0.0F;
    float v = 0.0F;
    float p = 0.0F;

    if ( NULL != s )
    {
        spring_eval(s, t_ms, &x, &v);
        p = s->target + x;
    }
    if ( NULL != pos )
    {
        *pos = p;
    }
    if ( NULL != vel_px_per_ms )
    {
        *vel_px_per_ms = v;
    }
}

bool kui_spring_rested(const kui_spring_t *s, uint32_t t_ms, float pos_eps_px, float vel_eps_px_per_ms)
{
    float x = 0.0F;
    float v = 0.0F;

    if ( NULL == s )
    {
        return false;
    }
    spring_eval(s, t_ms, &x, &v);
    return (fabsf(x) <= pos_eps_px) && (fabsf(v) <= vel_eps_px_per_ms);
}

void kui_spring_retarget(kui_spring_t *s, float target, uint32_t t_ms)
{
    float pos = 0.0F;
    float vel = 0.0F;

    if ( NULL == s )
    {
        return;
    }
    kui_spring_at(s, t_ms, &pos, &vel);
    s->target = target;
    s->t0 = t_ms;
    if ( s->w > 0.0F )
    {
        s->x0 = pos - target;
        s->v0 = vel;
    }
    else
    {
        /* No period (see kui_spring_start()): nothing of its own to keep, it is at the new target at once. */
        s->x0 = 0.0F;
        s->v0 = 0.0F;
    }
}
