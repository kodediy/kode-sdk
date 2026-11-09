/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file imu_ao.h
 * @brief The example's own Active Object: a stream of motion readings.
 */
#ifndef IMU_AO_H_
#define IMU_AO_H_

#include "kode_sys.h"

/* ImuDemo, not Imu: kodeOS already has AO_Imu and Imu_ctor(), and a second
 * one of either name is a link error. */
/** @brief The singleton, as an opaque QActive - the pattern every kodeOS AO
 *         follows, so nothing outside imu_ao.c can reach its members. */
extern QActive * const AO_ImuDemo;

/**
 * @brief Construct it.
 *
 * Call after kode_init() and before ImuDemo_start(). It only initialises
 * memory: nothing is posted and nothing is subscribed until ImuDemo_start()
 * runs the initial transition, in the caller's thread, before the AO's own
 * task exists.
 */
void ImuDemo_ctor(void);

/**
 * @brief Give it a thread, a queue and a stack, and let it run.
 *
 * Kept next to the storage it uses rather than open-coded in app_main(), so
 * the queue depth and the stack size stay in the same file as the state
 * machine whose appetite decides them.
 */
void ImuDemo_start(void);

#endif /* IMU_AO_H_ */
