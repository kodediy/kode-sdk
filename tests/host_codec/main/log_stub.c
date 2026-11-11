/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file log_stub.c
 * @brief klog, silenced, for a binary that is not testing klog.
 */
#include <stdarg.h>

#include "kode_log.h"

void kode_log_init(void) {}
void kode_log(kode_log_lvl_t lvl, const char *tag, const char *fmt, ...)
{ (void)lvl; (void)tag; (void)fmt; }
void kode_log_va(kode_log_lvl_t lvl, const char *tag, const char *fmt, va_list ap)
{ (void)lvl; (void)tag; (void)fmt; (void)ap; }
void kode_log_set_level(const char *tag, kode_log_lvl_t min)
{ (void)tag; (void)min; }
