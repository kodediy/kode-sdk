/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file standard_ao_support.h
 * @brief Waiting for the standard layer's Active Object to take its queue empty.
 */
#ifndef STANDARD_AO_SUPPORT_H_
#define STANDARD_AO_SUPPORT_H_

/** @brief Wait, bounded, for the standard layer's queue to be taken empty. */
void standard_ao_support_drained(void);

#endif /* STANDARD_AO_SUPPORT_H_ */
