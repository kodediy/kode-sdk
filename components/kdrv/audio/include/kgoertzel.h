/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kgoertzel.h
 * @brief One Goertzel bin: the kernel ktone's eight and the IMU's one share.
 */
#ifndef KGOERTZEL_H_
#define KGOERTZEL_H_

#include <math.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief One bin's state. Zero it with kgoertzel_init*() before feeding. */
typedef struct {
    float coeff;   /**< 2 cos(2 pi f / fs).            */
    float s1;      /**< One delay back.                */
    float s2;      /**< Two delays back.               */
} kgoertzel_t;

/** @brief Two pi, to float precision. */
#define KGOERTZEL_TWO_PI_F  6.28318530717958647692f

/**
 * @brief Point the bin at @p hz sampled at @p fs_hz and clear its state.
 * @param g     The bin.
 * @param hz    The frequency to measure.
 * @param fs_hz The sample rate.
 */
static inline void kgoertzel_init(kgoertzel_t *g, float hz, float fs_hz)
{
    g->coeff = 2.0f * cosf((KGOERTZEL_TWO_PI_F * hz) / fs_hz);
    g->s1 = 0.0f;
    g->s2 = 0.0f;
}

/**
 * @brief Point the bin at DFT index @p k of an @p n sample window.
 * @param g The bin.
 * @param k The bin index.
 * @param n The window length.
 */
static inline void kgoertzel_init_bin(kgoertzel_t *g, uint32_t k, uint32_t n)
{
    kgoertzel_init(g, (float)k, (float)n);
}

/**
 * @brief Advance the bin by one sample.
 * @param g The bin.
 * @param x The sample, in whatever unit the caller normalises by later.
 */
static inline void kgoertzel_feed(kgoertzel_t *g, float x)
{
    float const s0 = ((g->coeff * g->s1) - g->s2) + x;
    g->s2 = g->s1;
    g->s1 = s0;
}

/**
 * @brief The bin's power after the samples fed so far.
 * @param g The bin.
 * @return s1^2 + s2^2 - coeff s1 s2: a full-scale sine at the bin over n
 *         samples gives (n/2)^2.
 */
static inline float kgoertzel_power(const kgoertzel_t *g)
{
    return ((g->s1 * g->s1) + (g->s2 * g->s2)) - (g->coeff * g->s1 * g->s2);
}

#ifdef __cplusplus
}
#endif

#endif /* KGOERTZEL_H_ */
