/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file knfc_mfc_dict.h
 * @brief The published Mifare Classic key dictionary - see the .c for its
 *        source and the line it does not cross.
 */
#ifndef KNFC_MFC_DICT_H_
#define KNFC_MFC_DICT_H_

#include <stddef.h>
#include <stdint.h>

#include "knfc_mfc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The keys, six bytes each, common ones first. */
extern const uint8_t KNFC_MFC_DICT[][KNFC_MFC_KEY_LEN];

/** @brief How many. */
extern const size_t KNFC_MFC_DICT_COUNT;

#ifdef __cplusplus
}
#endif

#endif /* KNFC_MFC_DICT_H_ */
