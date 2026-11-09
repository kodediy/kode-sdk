/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file console_actions_ao.h
 * @brief The example's own Active Object: a counter the console can read and
 *        change.
 */
#ifndef CONSOLE_ACTIONS_AO_H_
#define CONSOLE_ACTIONS_AO_H_

#include "kode_sys.h"

/** @brief The singleton, as an opaque QActive - the pattern every kodeOS AO
 *         follows, so nothing outside console_actions_ao.c can reach its
 *         members. */
extern QActive * const AO_ConsoleActions;

/**
 * @brief Construct it.
 *
 * Call after kode_init() and before ConsoleActions_start(). It only
 * initialises memory: nothing is posted and nothing is subscribed until
 * ConsoleActions_start() runs the initial transition, in the caller's thread,
 * before the AO's own task exists.
 */
void ConsoleActions_ctor(void);

/**
 * @brief Give it a thread, a queue and a stack, and let it run, then hand the
 *        console its variable and its functions.
 *
 * The tables go in after the queue exists, so a `function` typed the moment
 * the console comes up has somewhere to post.
 */
void ConsoleActions_start(void);

#endif /* CONSOLE_ACTIONS_AO_H_ */
