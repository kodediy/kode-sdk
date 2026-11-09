/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kbsp_identity.h
 * @brief The product name and the prefix every Kode serial starts with.
 */
#ifndef KBSP_IDENTITY_H_
#define KBSP_IDENTITY_H_

#ifdef __cplusplus
extern "C" {
#endif

#define KBSP_ID_PRODUCT     "KODE-DOT"  /**< `info` prints it as `product=` */
#define KBSP_ID_PREFIX      "KODE"      /**< The same for every Kode product */

#ifdef __cplusplus
}
#endif

#endif /* KBSP_IDENTITY_H_ */
