/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file expansion_io_ao.h
 * @brief The example's own Active Object: the expansion header J3 in use.
 */
#ifndef EXPANSION_IO_AO_H_
#define EXPANSION_IO_AO_H_

#include "kode_sys.h"

/** @brief The singleton, as an opaque QActive - the pattern every kodeOS AO
 *         follows, so nothing outside expansion_io_ao.c can reach its members. */
extern QActive * const AO_ExpansionIo;

/**
 * @brief Construct it.
 *
 * Call after kode_init() and before ExpansionIo_start(). It only initialises
 * memory: nothing is posted, nothing is subscribed and no position of J3 is
 * touched until the initial transition runs.
 */
void ExpansionIo_ctor(void);

/**
 * @brief Give it a thread, a queue and a stack, and let it run.
 *
 * The initial transition runs here, in the caller's thread, and it is where
 * the AO takes the header: on return J3's positions are already configured.
 */
void ExpansionIo_start(void);

#endif /* EXPANSION_IO_AO_H_ */
