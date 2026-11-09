/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file miniz.h
 * @brief ESP-IDF's miniz.h, wrapped so its own redefinitions stay its own.
 */
#ifndef KODE_HOST_SHIM_MINIZ_H_
#define KODE_HOST_SHIM_MINIZ_H_

/* miniz defines its platform macros twice on a 64-bit host; as a system header, GCC and Clang
 * both stay quiet. */
#pragma GCC system_header
#include_next "miniz.h"

#endif /* KODE_HOST_SHIM_MINIZ_H_ */
