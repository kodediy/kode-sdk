/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_http_off.h
 * @brief kode_http_off_t, declared once for kode.h and kode_sys.h.
 */
#ifndef KODE_HTTP_OFF_H_
#define KODE_HTTP_OFF_H_

#include <stdint.h>

#include "sdkconfig.h"   /* CONFIG_KODE_HTTP_OFFSET_64 */

/* cppcheck-suppress misra-c2012-20.9 ; sdkconfig.h leaves an option that is off undefined, and #if reads it as 0 */
#if CONFIG_KODE_HTTP_OFFSET_64
/** @brief Where in a resource a ranged GET starts: 64 bits with CONFIG_KODE_HTTP_OFFSET_64. */
typedef uint64_t kode_http_off_t;
/** @brief The last offset the type holds. */
#define KODE_HTTP_OFF_MAX  UINT64_MAX
#else
/** @brief Where in a resource a ranged GET starts: 32 bits, so the first 4 GB of it. */
typedef uint32_t kode_http_off_t;
/** @brief The last offset the type holds. */
#define KODE_HTTP_OFF_MAX  UINT32_MAX
#endif

#endif /* KODE_HTTP_OFF_H_ */
