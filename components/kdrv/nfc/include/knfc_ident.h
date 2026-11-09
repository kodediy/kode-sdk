/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file knfc_ident.h
 * @brief Whether an IC_IDENTITY names a reader this driver drives.
 */
#ifndef KNFC_IDENT_H_
#define KNFC_IDENT_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Whether @p identity names a supported reader.
 * @param identity What IC_IDENTITY read.
 * @return true for an ST25R3916B of revision KDRV_ST25R3916_IC_REV_MIN or later.
 */
bool knfc_ident_supported(uint8_t identity);

#ifdef __cplusplus
}
#endif

#endif /* KNFC_IDENT_H_ */
