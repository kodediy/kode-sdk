/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file misra_libc.h
 * @brief What the C library's headers would have defined, for cppcheck.
 *
 * check_misra.sh includes this before every source: cppcheck does not read
 * the toolchain's own headers, and without these an ESP-IDF header stops the
 * whole source that includes it. Never compiled.
 */
#ifndef MISRA_LIBC_H_
#define MISRA_LIBC_H_

/* <sys/select.h>: esp_vfs.h refuses to be read without the library's fd_set. */
#define _SYS_TYPES_FD_SET

/* <sys/param.h>: FatFs sizes its sectors with these in an #if. */
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

/* <sys/queue.h>: the list fields of the structs in NimBLE's headers. */
#define SLIST_HEAD(name, type)  struct name { struct type *slh_first; }
#define SLIST_ENTRY(type)       struct { struct type *sle_next; }
#define STAILQ_HEAD(name, type) struct name { struct type *stqh_first; struct type **stqh_last; }
#define STAILQ_ENTRY(type)      struct { struct type *stqe_next; }
#define TAILQ_HEAD(name, type)  struct name { struct type *tqh_first; struct type **tqh_last; }
#define TAILQ_ENTRY(type)       struct { struct type *tqe_next; struct type **tqe_prev; }

#endif /* MISRA_LIBC_H_ */
