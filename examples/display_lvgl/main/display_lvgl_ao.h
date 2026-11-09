/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file display_lvgl_ao.h
 * @brief The example's own Active Object: LVGL on the glass - a counter,
 *        the local time, the finger's point and the brightness.
 */
#ifndef DISPLAY_LVGL_AO_H_
#define DISPLAY_LVGL_AO_H_

#include "kode_sys.h"

/** @brief The singleton, as an opaque QActive - the pattern every kodeOS AO
 *         follows, so nothing outside display_lvgl_ao.c can reach its members. */
extern QActive * const AO_DisplayLvgl;

/**
 * @brief Construct it.
 *
 * Call after kode_init() and before DisplayLvgl_start(). It only initialises
 * memory: nothing is posted and nothing is subscribed until
 * DisplayLvgl_start() runs the initial transition, in the caller's thread,
 * before the AO's own task exists. LVGL is touched only on the AO's own task,
 * once the panel is up.
 */
void DisplayLvgl_ctor(void);

/**
 * @brief Give it a thread, a queue and a stack, and let it run.
 *
 * Kept next to the storage it uses so the queue depth and the stack size stay
 * in the same file as the state machine whose appetite decides them.
 */
void DisplayLvgl_start(void);

#endif /* DISPLAY_LVGL_AO_H_ */
