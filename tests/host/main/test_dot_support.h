/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_dot_support.h
 * @brief AO_TestDot brought up for a case, and what it said written down.
 */
#ifndef TEST_DOT_SUPPORT_H_
#define TEST_DOT_SUPPORT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kaos.h"
#include "kqp.h"

#if !CONFIG_KODE_TEST_DOT
#error "test_dot_support.h needs CONFIG_KODE_TEST_DOT: main/CMakeLists.txt spells it out"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define TD_LINE_MAX   200U  /**< One line of what the AO said.        */
#define TD_LINES_MAX  256U  /**< Lines kept between two td_clear().   */

/** @brief AO_TestDot built afresh, dormant, its neighbours' mailboxes empty and the station's fakes reset. */
void td_up(void);

/** @brief Same, with the lock stored before the AO starts. */
void td_up_locked(void);

/**
 * @brief Leave nothing behind for the files after this one: AO_TestDot's
 *        timers fired and thrown away, the asks it left in the owners'
 *        mailboxes emptied. Every case ends with it.
 */
void td_down(void);

/** @brief Post a station verb as the console would, with ao_spy as the asker, and dispatch it. */
void td_verb(TestDotReqEvt const *proto);

/** @brief A verb with no keys. */
void td_verb_plain(uint8_t verb);

/** @brief Dispatch @p e to AO_TestDot, then what that left in its queue. */
void td_dispatch(QEvt const *e);

/** @brief Dispatch what waits in AO_TestDot's queue. */
void td_settle(void);

/** @brief Run the worker's next job and dispatch its answer. @return false with no job waiting. */
bool td_job(void);

/** @brief Jobs the worker holds. */
unsigned td_jobs(void);

/** @brief Run every job the worker holds, and each answer, until none is left or @p max ran. */
unsigned td_jobs_run(unsigned max);

/** @brief Advance @p ms of QP time, dispatching AO_TestDot's own timers. */
void td_wait_ms(uint32_t ms);

/** @brief Read what ao_spy holds into the lines; blobs go back, screens are answered. */
void td_take(void);

/** @brief Forget the lines. */
void td_clear(void);

/** @brief Whether a line holds @p needle (after a td_take()). */
bool td_said(const char *needle);

/** @brief How many lines hold @p needle. */
unsigned td_said_n(const char *needle);

/** @brief The index of the first line holding @p needle, or TD_LINES_MAX. */
unsigned td_said_at(const char *needle);

/** @brief Print the lines, for a case that fails. */
void td_dump(void);

/** @brief Answer screens from now on (the default), or leave them unanswered. */
void td_screens_answer(bool on);

/**
 * @brief The last screen request td_take() saw.
 * @param kind Receives its kind.
 * @param seq  Receives its number.
 * @return false when none was seen since the up.
 */
bool td_screen_last(uint8_t *kind, uint8_t *seq);

/** @brief How many events with signal @p sig wait in @p ao's mailbox; the mailbox is emptied. */
unsigned td_posted(QActive *ao, enum_t sig);

/** @brief The last DISPLAY_BRIGHTNESS_SIG percent in AO_Display's mailbox, or -1; the mailbox is emptied. */
int td_brightness(void);

/** @brief Whether the last EXP_POWER_REQ_SIG in AO_Radio's mailbox asked for on; the mailbox is emptied.
 *  @param seen Out: whether there was one. */
bool td_rail_asked(bool *seen);

/** @brief Answer the rail as AO_Radio does. */
void td_rail_answer(bool on, kode_status_t rc);

/**
 * @brief The rows AO_Autotest was asked for since the last call, their names
 *        joined by spaces into @p out; the mailbox is emptied.
 * @return How many were asked.
 */
unsigned td_rows_asked(char *out, size_t cap);

/** @brief Answer the row in flight as AO_Autotest does: taken, then @p state, then its end. */
void td_row_answer(uint8_t state);

/** @brief Wake the station with every part answering: the verb is already posted. */
void td_wake_ok(void);

/** @brief Bring it down: `test_dot_end`, every owner answering. */
void td_end(void);

/** @brief Whether the station's two chips answer on its I2C; td_up() sets true. */
void td_station_answers(bool on);

/** @brief Whether the worker's JPEG encodes succeed (true) or are refused (false, the up's). */
void td_jpeg_encodes(bool on);

/** @brief Bytes the station's relay expander was last written, or -1 since the up. */
int td_relays_last(void);

/** @brief How many writes the relay expander took since the up. */
unsigned td_relays_writes(void);

#ifdef __cplusplus
}
#endif

#endif /* TEST_DOT_SUPPORT_H_ */
