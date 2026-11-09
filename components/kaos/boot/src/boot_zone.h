/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file boot_zone.h
 * @brief AO_Boot's time zone check: whether a string is a POSIX TZ string.
 */
#ifndef BOOT_ZONE_H_
#define BOOT_ZONE_H_

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Whether @p zone is a POSIX TZ string of 1 to KSET_ZONE_MAX characters,
 *        terminated inside @p cap.
 *
 * Grammar: `std offset [dst [offset] [,start[/time],end[/time]]]`. The zone is
 * stored and the face reads the hour through it, so text the C library cannot
 * parse is refused here. A line break or a non-ASCII byte fails the grammar.
 *
 * @param zone The candidate.
 * @param cap  The bytes @p zone may span, terminator included.
 * @return true when it may be applied.
 */
bool boot_zone_valid(const char *zone, size_t cap);

#endif /* BOOT_ZONE_H_ */
