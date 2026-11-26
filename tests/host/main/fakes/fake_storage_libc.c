/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_storage_libc.c
 * @brief fstat(), stat(), lseek() and read() under kdrv_storage.c, as the board answers them.
 *
 * Every call reaches the host's own. The host's off_t is 64 bits and the
 * board's 32 bits and signed, so a size or a position from 2 GiB on is
 * negative there: narrowed, these answer with what the board's 32-bit off_t
 * would hold - a size the case names for every regular file, and the
 * position lseek() reached. lseek() also keeps what it was asked. read() can
 * hand a file over in pieces, and fail after its first piece as the board's
 * FAT does after a disk error: the bytes it had, then -1.
 */
#define FAKE_STORAGE_LIBC_IMPL
#include "fake_storage_libc.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "fakes.h"

static bool     s_narrow;       /**< Answer as the board's 32-bit off_t does. */
static uint32_t s_size;         /**< The size every regular file has while narrowed. */
static bool     s_fstat_fails;  /**< fstat() fails with EIO. */
static bool     s_seek_fails;   /**< lseek() fails with EIO and moves nothing. */
static uint32_t s_seek_asked;   /**< The offset of the last lseek(), its low 32 bits. */
static int      s_seek_whence;  /**< The whence of the last lseek(). */
static uint32_t s_seeks;        /**< lseek() calls since the reset. */
static size_t   s_read_most;    /**< The most one read() gives; 0 is no limit. */
static bool     s_read_cut;     /**< Every read() after one that gave bytes fails. */
static bool     s_read_failing; /**< The cut has come: read() fails with EIO. */

/**
 * @brief What the board's off_t holds for @p v.
 * @param v A size or a position, unsigned.
 * @return @p v in 32 signed bits: from 2 GiB on, negative.
 */
static off_t board_off(uint32_t v)
{
    return (off_t)(int32_t)v;
}

void fake_storage_libc_reset(void)
{
    s_narrow       = false;
    s_size         = 0U;
    s_fstat_fails  = false;
    s_seek_fails   = false;
    s_seek_asked   = 0U;
    s_seek_whence  = -1;
    s_seeks        = 0U;
    s_read_most    = 0U;
    s_read_cut     = false;
    s_read_failing = false;
}

void fake_storage_libc_narrow(uint32_t size)
{
    s_narrow = true;
    s_size   = size;
}

void fake_storage_libc_fstat_fails(bool fails)
{
    s_fstat_fails = fails;
}

void fake_storage_libc_seek_fails(bool fails)
{
    s_seek_fails = fails;
}

void fake_storage_libc_read_pieces(size_t most)
{
    s_read_most = most;
}

void fake_storage_libc_read_cut(size_t most)
{
    s_read_most = most;
    s_read_cut  = true;
}

uint32_t fake_storage_libc_seek_asked(void)
{
    return s_seek_asked;
}

int fake_storage_libc_seek_whence(void)
{
    return s_seek_whence;
}

uint32_t fake_storage_libc_seeks(void)
{
    return s_seeks;
}

int fake_storage_fstat(int fd, struct stat *st)
{
    if ( s_fstat_fails )
    {
        errno = EIO;
        return -1;
    }

    int const r = fstat(fd, st);

    if ( (0 == r) && s_narrow && S_ISREG(st->st_mode) )
    {
        st->st_size = board_off(s_size);
    }
    return r;
}

int fake_storage_stat(const char *path, struct stat *st)
{
    int const r = stat(path, st);

    if ( (0 == r) && s_narrow && S_ISREG(st->st_mode) )
    {
        st->st_size = board_off(s_size);
    }
    return r;
}

off_t fake_storage_lseek(int fd, off_t offset, int whence)
{
    s_seeks++;
    s_seek_asked  = (uint32_t)offset;
    s_seek_whence = whence;
    if ( s_seek_fails )
    {
        errno = EIO;
        return (off_t)-1;
    }

    off_t r = lseek(fd, offset, whence);
    if ( s_narrow && (0 <= r) )
    {
        r = board_off((uint32_t)r);
    }
    return r;
}

ssize_t fake_storage_read(int fd, void *buf, size_t n)
{
    if ( s_read_failing )
    {
        errno = EIO;
        return -1;
    }

    size_t ask = n;
    if ( (0U != s_read_most) && (s_read_most < ask) )
    {
        ask = s_read_most;
    }

    ssize_t const r = read(fd, buf, ask);
    if ( s_read_cut && (0 < r) )
    {
        s_read_failing = true;
    }
    return r;
}
