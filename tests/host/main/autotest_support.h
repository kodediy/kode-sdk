/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file autotest_support.h
 * @brief AO_Autotest in a case: brought up once and relaying the two AOs' exchange on every tick.
 */
#ifndef AUTOTEST_SUPPORT_H_
#define AUTOTEST_SUPPORT_H_

#include <stdbool.h>

/** @brief Bring AO_Autotest up if no case has yet. */
void autotest_up(void);

/**
 * @brief Deliver what AO_Autotest and AO_Console have for each other, until
 *        neither has anything left. Simulated time does not move.
 *
 * AO_Console is dispatched only when a case has given it a queue.
 */
void autotest_settle(void);

/**
 * @brief Call after dispatching a console line by hand: settles when the
 *        line is the `test` verb, and leaves every other verb's events
 *        where the case expects to find them.
 *
 * @param line The line that was typed.
 */
void autotest_typed(const char *line);

/**
 * @brief Hand an owner's verdict to AO_Autotest and to AO_Console, then
 *        settle.
 *
 * Both, because both listen: a row's verdict is AO_Autotest's, and
 * `sd_verify`'s is the console's own.
 *
 * @param name The row answering.
 * @param ok   Its verdict.
 */
void autotest_verdict(const char *name, bool ok);

/**
 * @brief Let the PSRAM row run to its verdict.
 *
 * Nobody answers that row: it paces itself on AO_Autotest's own timer, so
 * what moves it is time. Ticks enough for the run fake_system_reset()
 * leaves - two blocks - through AO_Console when it has a queue, so the
 * console prints as it goes.
 */
void autotest_psram_run(void);

#endif /* AUTOTEST_SUPPORT_H_ */
