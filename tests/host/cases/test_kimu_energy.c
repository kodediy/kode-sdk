/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kimu_energy.c
 * @brief The accelerometer window measured at the motor's frequency.
 */
#include <math.h>
#include <string.h>

#include "unity.h"

#include "kimu_energy.h"

#define N   256U
#define FS  1920U
#define UG  122U    /* 0.122 mg/LSB at +-4 g */

static int16_t s_xyz[3U * N];

/** @brief Gravity of 1 g on z, plus a sine of @p amp_mg at @p hz on axis @p ax. */
static void fill(double hz, double amp_mg, unsigned ax)
{
    for ( unsigned i = 0U; N > i; ++i )
    {
        double const s = amp_mg * sin((2.0 * M_PI * hz * (double)i) / (double)FS);
        s_xyz[(3U * i) + 0U] = 0;
        s_xyz[(3U * i) + 1U] = 0;
        s_xyz[(3U * i) + 2U] = (int16_t)lrint(1000.0 / 0.122);
        s_xyz[(3U * i) + ax] = (int16_t)(s_xyz[(3U * i) + ax] + (int16_t)lrint(s / 0.122));
    }
}

TEST_CASE("a 100 mg sine at 235 Hz reads 100 mg in the bin, whichever axis carries it", "[kimu_energy]")
{
    kimu_energy_t e;
    for ( unsigned ax = 0U; 3U > ax; ++ax )
    {
        fill(235.0, 100.0, ax);
        TEST_ASSERT_EQUAL_INT(KODE_OK, kimu_energy(s_xyz, N, 235U, FS, UG, &e));
        TEST_ASSERT_INT_WITHIN(60, 1000, e.bin_x10);
        TEST_ASSERT_EQUAL_UINT8(ax, e.axis);
    }
}

TEST_CASE("gravity and a hand at 20 Hz do not enter the bin", "[kimu_energy]")
{
    kimu_energy_t e;
    fill(20.0, 500.0, 0U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kimu_energy(s_xyz, N, 235U, FS, UG, &e));
    TEST_ASSERT_TRUE_MESSAGE(100 > e.bin_x10, "500 mg at 20 Hz leaves under 10 mg at 235");
    TEST_ASSERT_TRUE_MESSAGE(3000 < e.rms_x10, "the RMS still sees the 500 mg swing");
}

TEST_CASE("a motor whose F0 drifted to 230 Hz is still heard", "[kimu_energy]")
{
    kimu_energy_t e;
    fill(230.0, 100.0, 1U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kimu_energy(s_xyz, N, 235U, FS, UG, &e));
    TEST_ASSERT_TRUE(500 < e.bin_x10);
}

TEST_CASE("a short window is refused rather than measured", "[kimu_energy]")
{
    kimu_energy_t e;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kimu_energy(s_xyz, 16U, 235U, FS, UG, &e));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kimu_energy(NULL, N, 235U, FS, UG, &e));
}
