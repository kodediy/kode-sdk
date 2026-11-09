/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file power_ao.h
 * @brief The example's own Active Object: the battery, and the way to switch the board off.
 */
#ifndef POWER_AO_H_
#define POWER_AO_H_

#include "kode_sys.h"

/* PowerDemo, not Power: kodeOS already has AO_Power, and a second one of
 * that name is a link error. */
/** @brief The singleton, as an opaque QActive - the pattern every kodeOS AO
 *         follows, so nothing outside power_ao.c can reach its members. */
extern QActive * const AO_PowerDemo;

/**
 * @brief Construct it.
 *
 * Call after kode_init() and before PowerDemo_start(). It only initialises
 * memory: nothing is posted and nothing is subscribed until PowerDemo_start()
 * runs the initial transition, in the caller's thread, before the AO's own
 * task exists.
 */
void PowerDemo_ctor(void);

/**
 * @brief Give it a thread, a queue and a stack, and let it run.
 *
 * Kept next to the storage it uses rather than open-coded in app_main(), so
 * the queue depth and the stack size stay in the same file as the state
 * machine whose appetite decides them.
 */
void PowerDemo_start(void);

#endif /* POWER_AO_H_ */
