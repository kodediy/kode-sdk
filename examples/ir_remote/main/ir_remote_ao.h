/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file ir_remote_ao.h
 * @brief The example's own Active Object: learn a remote key, send it back.
 */
#ifndef IR_REMOTE_AO_H_
#define IR_REMOTE_AO_H_

#include "kode_sys.h"

/** @brief The singleton, as an opaque QActive - the pattern every kodeOS AO
 *         follows, so nothing outside ir_remote_ao.c can reach its members. */
extern QActive * const AO_IrRemote;

/**
 * @brief Construct it.
 *
 * Call after kode_init() and before IrRemote_start(). It only initialises
 * memory: nothing is posted and nothing is subscribed until IrRemote_start()
 * runs the initial transition, in the caller's thread, before the AO's own
 * task exists.
 */
void IrRemote_ctor(void);

/**
 * @brief Give it a thread, a queue and a stack, and let it run.
 *
 * Kept next to the storage it uses rather than open-coded in app_main(), so
 * the queue depth and the stack size stay in the same file as the state
 * machine whose appetite decides them.
 */
void IrRemote_start(void);

#endif /* IR_REMOTE_AO_H_ */
