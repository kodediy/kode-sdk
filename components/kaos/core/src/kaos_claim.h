/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kaos_claim.h
 * @brief Package-private: how an AO says what it owns.
 */
#ifndef KAOS_CLAIM_H_
#define KAOS_CLAIM_H_

#include <stdint.h>

#include "kbsp_owners.h"
#include "kode_defs.h"

/**
 * @brief Take ownership of @p res, or bring the system down saying who has it.
 *
 * Call from an AO's initial transition. Fatal on failure: two owners for one
 * device must fail at boot, the same way every time.
 *
 * @param owner The AO.
 * @param name  What to call it in `ps` and in the failure message.
 */
void kaos_claim(kbsp_res_t res, const void *owner, const char *name);

/** @brief The owner every park claims for (kaos_park.c); the claim's name says whose part it is. */
extern const uint8_t kaos_parked;

/**
 * @brief Bring the system down unless an owner's table was registered.
 *
 * Call from an owner's register function with what kverbs_add() or
 * krows_add() answered. Their tables are first-party and static, so a
 * refusal is a build fault, failed at boot the way a double claim is.
 *
 * @param st   What the registry answered.
 * @param what What was registered, for the failure message.
 */
void kaos_require(kode_status_t st, const char *what);

#endif /* KAOS_CLAIM_H_ */
