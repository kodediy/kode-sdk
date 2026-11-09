/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file motion_support.h
 * @brief AO_Haptic as a mailbox in a case, and a queue emptied unrun.
 */
#ifndef MOTION_SUPPORT_H_
#define MOTION_SUPPORT_H_

#include "qpc.h"

/**
 * @brief Give AO_Haptic a mailbox and a free door; constructed, never initialised.
 *
 * The constructor clears the door's flags. What waits in the queue is dropped
 * first, unrun: a tick would dispatch it into a machine that was never
 * initialised, and QEP asserts.
 */
void motion_support_haptic_up(void);

/**
 * @brief Drop what waits in @p ao's queue without running it.
 * @param ao The AO.
 */
void motion_support_drain(QActive *ao);

#endif /* MOTION_SUPPORT_H_ */
