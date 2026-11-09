/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file probe_ao.h
 * @brief A real Active Object, with a real task, that runs a case's job inside its step.
 */
#ifndef PROBE_AO_H_
#define PROBE_AO_H_

#include <stdbool.h>
#include <stdint.h>

#include "qpc.h"

/** @brief The probe's QP level: QF_MAX_ACTIVE, which no owner holds. */
#define PROBE_AO_PRIO ((uint8_t)QF_MAX_ACTIVE)

/** @brief Task name of the probe. */
#define PROBE_AO_NAME "ao_probe"

/** @brief A job the probe runs inside its step. */
typedef void (*probe_ao_job_t)(void *ctx);

/**
 * @brief Run @p job inside the probe's step and wait for it to end.
 *
 * The probe is started on the first call and stays up for the binary.
 *
 * @param job     What to run.
 * @param ctx     Its context.
 * @param wait_ms The longest to wait, in ms.
 * @return true when the job ran and ended within @p wait_ms.
 */
bool probe_ao_run(probe_ao_job_t job, void *ctx, uint32_t wait_ms);

#endif /* PROBE_AO_H_ */
