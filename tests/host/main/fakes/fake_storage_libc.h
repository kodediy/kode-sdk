/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_storage_libc.h
 * @brief The C library calls kdrv_storage.c sizes files, moves and reads with.
 *
 * Force-included into kdrv_storage.c alone by main/CMakeLists.txt. The system
 * headers come first, so their declarations keep their own names; the calls
 * in the driver then reach fake_storage_libc.c, which passes them on to the
 * host and, when a case asks, answers as the board's 32-bit off_t and FAT
 * would.
 */
#ifndef FAKE_STORAGE_LIBC_H_
#define FAKE_STORAGE_LIBC_H_

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* macOS's <fcntl.h> takes open() and O_RDONLY from <sys/fcntl.h>, and this
 * build finds lwip's linux shim of that name first, which has neither.
 * glibc's <fcntl.h> has both itself. */
#if defined(__APPLE__)
#ifndef O_RDONLY
#define O_RDONLY 0x0000 /**< macOS's value: open for reading only. */
#endif
/** @brief open(), as macOS declares it. @see open */
int open(const char *path, int flags, ...);
#endif

/** @brief fstat(), as the board answers it. @see fstat */
int fake_storage_fstat(int fd, struct stat *st);

/** @brief stat(), as the board answers it. @see stat */
int fake_storage_stat(const char *path, struct stat *st);

/** @brief lseek(), as the board answers it. @see lseek */
off_t fake_storage_lseek(int fd, off_t offset, int whence);

/** @brief read(), in the pieces and with the failure a case asks for. @see read */
ssize_t fake_storage_read(int fd, void *buf, size_t n);

#ifndef FAKE_STORAGE_LIBC_IMPL
#define fstat fake_storage_fstat
#define stat(path, st) fake_storage_stat((path), (st))
#define lseek fake_storage_lseek
#define read(fd, buf, n) fake_storage_read((fd), (buf), (n))
#endif

#endif /* FAKE_STORAGE_LIBC_H_ */
