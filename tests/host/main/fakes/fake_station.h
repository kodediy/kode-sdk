/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_station.h
 * @brief The E5 station's fakes: the camera on J3 and the loop round the dock's pads.
 */
#ifndef FAKE_STATION_H_
#define FAKE_STATION_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Everything back to a camera that answers its probe and a loop that comes back whole. */
void fake_station_reset(void);

/** @brief What kdrv_ov5640_probe() answers from now on. */
void fake_station_probe_rc(kode_status_t rc);

/** @brief Whether kdrv_ov5640_grab() gives a flat grey frame (true) or times out (false, the reset's). */
void fake_station_frames(bool on);

/** @brief How many times kdrv_ov5640_probe() was called since the reset. */
unsigned fake_station_probes(void);

/** @brief How many times kdrv_ov5640_close() was called since the reset. */
unsigned fake_station_closes(void);

/** @brief How many times kdrv_ov5640_open() was called since the reset. */
unsigned fake_station_opens(void);

/** @brief What kdrv_dock_loop_run() answers from now on; KODE_OK gives every frame back. */
void fake_station_loop_rc(kode_status_t rc);

/** @brief How many times kdrv_dock_loop_run() was called since the reset. */
unsigned fake_station_loops(void);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_STATION_H_ */
