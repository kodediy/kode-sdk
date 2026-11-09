/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file network_ao.h
 * @brief The example's own Active Object: ping and fetch through AO_Net.
 */
#ifndef NETWORK_AO_H_
#define NETWORK_AO_H_

#include "kode_sys.h"

/** @brief The singleton, as an opaque QActive - the pattern every kodeOS AO
 *         follows, so nothing outside network_ao.c can reach its members. */
extern QActive * const AO_Network;

/**
 * @brief Construct it.
 *
 * Call after kode_init() and before Network_start(). It only initialises
 * memory: nothing is posted and nothing is subscribed until Network_start()
 * runs the initial transition, in the caller's thread, before the AO's own
 * task exists.
 */
void Network_ctor(void);

/**
 * @brief Give it a thread, a queue and a stack, and let it run.
 *
 * Kept next to the storage it uses rather than open-coded in app_main(), so
 * the queue depth and the stack size stay in the same file as the state
 * machine whose appetite decides them.
 */
void Network_start(void);

#endif /* NETWORK_AO_H_ */
