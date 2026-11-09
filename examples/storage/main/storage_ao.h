/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file storage_ao.h
 * @brief The example's own Active Object: a log file on the internal FAT,
 *        asked of AO_Storage without waiting.
 */
#ifndef STORAGE_AO_H_
#define STORAGE_AO_H_

#include "kode_sys.h"

/** @brief The singleton, as an opaque QActive - the pattern every kodeOS AO
 *         follows, so nothing outside storage_ao.c can reach its members. */
extern QActive * const AO_StorageDemo;

/**
 * @brief Construct it.
 *
 * Call after kode_init() and before StorageDemo_start(). It only initialises
 * memory: nothing is posted and nothing is subscribed until
 * StorageDemo_start() runs the initial transition, in the caller's thread,
 * before the AO's own task exists.
 *
 * @param boots This run's boot number, which app_main() counted in the store.
 */
void StorageDemo_ctor(uint32_t boots);

/**
 * @brief Give it a thread, a queue and a stack, and let it run.
 *
 * Kept next to the storage it uses rather than open-coded in app_main(), so
 * the queue depth and the stack size stay in the same file as the state
 * machine whose appetite decides them.
 */
void StorageDemo_start(void);

#endif /* STORAGE_AO_H_ */
