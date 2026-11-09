/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file haptic_ao.h
 * @brief The example's own Active Object: the buttons buzz the motor.
 */
#ifndef HAPTIC_AO_H_
#define HAPTIC_AO_H_

#include "kode_sys.h"

/* HapticDemo, not Haptic: kodeOS's AO_Haptic is a global symbol, and a second
 * one of that name is a link error. */
/** @brief The singleton, as an opaque QActive - the pattern every kodeOS AO
 *         follows, so nothing outside haptic_ao.c can reach its members. */
extern QActive * const AO_HapticDemo;

/**
 * @brief Construct it.
 *
 * Call after kode_init() and before HapticDemo_start(). It only initialises
 * memory: nothing is posted and nothing is subscribed until HapticDemo_start()
 * runs the initial transition, in the caller's thread, before the AO's own
 * task exists.
 */
void HapticDemo_ctor(void);

/**
 * @brief Give it a thread, a queue and a stack, and let it run.
 *
 * Kept next to the storage it uses rather than open-coded in app_main(), so
 * the queue depth and the stack size stay in the same file as the state
 * machine whose appetite decides them.
 */
void HapticDemo_start(void);

#endif /* HAPTIC_AO_H_ */
