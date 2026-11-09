/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file owners_support.h
 * @brief What every owner's start registers - its console verbs and its self-test rows - done the way a case brings an AO up: without starting it.
 */
#ifndef OWNERS_SUPPORT_H_
#define OWNERS_SUPPORT_H_

#include <stdbool.h>

#include "kbsp_owners.h"

/** @brief Register every owner's verbs and rows. Idempotent: a table already in is in. */
void owners_up(void);

/**
 * @brief Free @p res for a park: give it back from @p holder, its owner here.
 * @param res    The resource.
 * @param holder The owner a case before this one may have started.
 * @return true when @p holder held it, for owners_return().
 */
bool owners_lend(kbsp_res_t res, const void *holder);

/**
 * @brief Take @p res back from the parks, and give it to @p holder again if it held it.
 * @param res    The resource.
 * @param holder Its owner.
 * @param name   The owner's name.
 * @param held   What owners_lend() answered.
 */
void owners_return(kbsp_res_t res, const void *holder, const char *name, bool held);

#endif /* OWNERS_SUPPORT_H_ */
