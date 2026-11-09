/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_sim.h
 * @brief What a consumer gives the simulator: its screens, its setup and its subjects.
 */
#ifndef KUI_SIM_H
#define KUI_SIM_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/** One screen the simulator can show. */
typedef struct {
    const char *name;        /**< What the command line calls it. */
    lv_obj_t *(*get)(void);  /**< Returns the screen, built by kui_sim_setup(). */
} kui_sim_screen_t;

/** The consumer's screens. */
extern const kui_sim_screen_t kui_sim_screens[];
/** How many kui_sim_screens holds. */
extern const uint32_t kui_sim_screen_count;

/** @brief Build what the screens need; called once, after kui_init(). */
void kui_sim_setup(void);

/**
 * @brief Feed one subject from the command line.
 * @param subject The subject's name, as the consumer spells it.
 * @param value   Its value as text.
 * @return false when the name is unknown or the value does not parse.
 */
bool kui_sim_set(const char *subject, const char *value);

#endif /* KUI_SIM_H */
