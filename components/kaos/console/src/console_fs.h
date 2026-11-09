/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file console_fs.h
 * @brief The file verbs' bodies, and the working directory they share.
 */
#ifndef CONSOLE_FS_H_
#define CONSOLE_FS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* kqp.h, not qpc.h: kqp is the one door to the framework. kqp_config.h is
 * named for KODE_PATH_MAX, this file's own bound. */
#include "kqp.h"
#include "kqp_config.h"

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Resolve @p in against @p cwd into an absolute, normalised path.
 *
 * Applies `.` and `..`, collapses repeated separators, strips a trailing one.
 * `..` at the root stays at the root.
 *
 * @param cwd Absolute, normalised. The directory a relative @p in is read
 *            against.
 * @param in  Absolute or relative. An empty string resolves to @p cwd.
 * @param out The result. Undefined unless KODE_OK is returned.
 * @param cap Size of @p out.
 * @return KODE_OK, or KODE_DENIED when an argument is NULL or the result does
 *         not fit @p cap. Never truncates.
 */
kode_status_t console_fs_resolve_from(const char *cwd, const char *in,
                                      char *out, size_t cap);

/**
 * @brief console_fs_resolve_from() against the current directory.
 * @param in  Absolute or relative. NULL and "" both mean the current one.
 * @param out The result.
 * @param cap Size of @p out.
 * @return KODE_OK, or KODE_DENIED when the result does not fit @p cap.
 */
kode_status_t console_fs_resolve(const char *in, char *out, size_t cap);

/**
 * @brief The current directory.
 * @return Never NULL; "/" until something moves it.
 */
const char *console_fs_cwd(void);

/**
 * @brief The path the request in flight named, already resolved.
 *
 * A StoragePathRspEvt does not carry the path it answers. `stat` prints this
 * back, and `cd` adopts it.
 *
 * @return Never NULL; "/" before anything has been asked.
 */
const char *console_fs_target(void);

/**
 * @brief Name the volume @p path lives on, for a record's `medium=`.
 * @param path Absolute virtual path.
 * @return "sd", "fs", or "-" for the root, which is on neither.
 */
const char *console_fs_medium_name(const char *path);

/**
 * @brief Ask AO_Storage whether @p in is a directory, answering to @p who.
 *
 * The working directory does NOT move here, but in console_fs_enter() once
 * the answer says directory; any other answer leaves it where it was.
 *
 * @param who Where STORAGE_PATH_RSP_SIG is posted.
 * @param in  Absolute or relative. NULL means the current directory.
 * @return KODE_OK when the question went out; KODE_DENIED when @p in does not
 *         resolve, in which case nothing was asked and nothing will answer.
 */
kode_status_t console_fs_chdir(QActive *who, const char *in);

/** @brief Adopt console_fs_target() as the working directory. */
void console_fs_enter(void);

/**
 * @brief STAT @p in, the first half of `ls`, answering to @p who.
 *
 * The LIST terminator carries no status, so without the STAT an empty
 * directory and a missing one would look the same. Once the answer says
 * directory, console_fs_list_begin() lists it.
 *
 * @param who Where STORAGE_PATH_RSP_SIG is posted.
 * @param in  Absolute or relative. NULL means the current directory.
 * @return KODE_OK when the question went out; KODE_DENIED when @p in does
 *         not resolve, and nothing will answer.
 */
kode_status_t console_fs_ls(QActive *who, const char *in);

/**
 * @brief List console_fs_target(), once its STAT has said directory.
 *
 * @param who The requester. The entries are published, so they reach every
 *            subscriber.
 */
void console_fs_list_begin(QActive *who);

/**
 * @brief Ask AO_Storage what @p in is, answering to @p who.
 * @param who Where STORAGE_PATH_RSP_SIG is posted.
 * @param in  Absolute or relative. NULL means the current directory.
 * @return KODE_OK when the question went out; KODE_DENIED when @p in does not
 *         resolve.
 */
kode_status_t console_fs_stat(QActive *who, const char *in);

/**
 * @brief STAT @p in, the first half of `cat`, answering to @p who.
 *
 * `cat` refuses a directory. Once the answer says file,
 * console_fs_cat_begin() reads it.
 *
 * @param who Where STORAGE_PATH_RSP_SIG is posted.
 * @param in  Absolute or relative. NULL means the current directory.
 * @return KODE_OK when the question went out; KODE_DENIED when @p in does
 *         not resolve, and nothing will answer.
 */
kode_status_t console_fs_cat(QActive *who, const char *in);

/**
 * @brief Read console_fs_target(), once its STAT has said file.
 *
 * @param who Where STORAGE_LINE_SIG lands, lines and terminator alike; a
 *            `cat` is posted to the asker, never published.
 * @param max Ceiling on bytes read, at most KODE_FS_CAT_MAX_CEIL.
 */
void console_fs_cat_begin(QActive *who, uint32_t max);

/**
 * @brief mkdir path=: resolve @p in and ask AO_Storage to make it.
 *
 * One round trip, no STAT first.
 *
 * @param who Where STORAGE_PATH_RSP_SIG is posted.
 * @param in  Absolute or relative. NULL means the current directory.
 * @return KODE_OK when the question went out; KODE_DENIED when @p in does
 *         not resolve, and nothing will answer.
 */
kode_status_t console_fs_mkdir(QActive *who, const char *in);

/**
 * @brief STAT @p in, the first half of `rm`, answering to @p who.
 *
 * `rm` refuses a directory. Once the answer says file, console_fs_rm_begin()
 * deletes it.
 *
 * @param who Where STORAGE_PATH_RSP_SIG is posted.
 * @param in  Absolute or relative. NULL means the current directory.
 * @return KODE_OK when the question went out; KODE_DENIED when @p in does
 *         not resolve, and nothing will answer.
 */
kode_status_t console_fs_rm(QActive *who, const char *in);

/** @brief Delete console_fs_target(), once its STAT has said file. */
void console_fs_rm_begin(QActive *who);

/**
 * @brief STAT @p in, the first half of `rmdir`, answering to @p who.
 *
 * `rmdir` refuses a file.
 *
 * @param who Where STORAGE_PATH_RSP_SIG is posted.
 * @param in  Absolute or relative. NULL means the current directory.
 * @return KODE_OK when the question went out; KODE_DENIED when @p in does
 *         not resolve, and nothing will answer.
 */
kode_status_t console_fs_rmdir(QActive *who, const char *in);

/**
 * @brief Remove console_fs_target(), once its STAT has said directory.
 *
 * A non-empty directory is refused by kdrv_storage_rmdir(), not by the STAT.
 */
void console_fs_rmdir_begin(QActive *who);

/**
 * @brief mv src= dst= and cp src= dst=: resolve both paths and ask
 *        AO_Storage to move or copy one to the other.
 *
 * No STAT phase: kdrv_storage_rename() refuses a move across volumes, and
 * kdrv_storage_copy_open() a missing source or a copy onto itself.
 *
 * @param who Where STORAGE_PATH_RSP_SIG is posted; for a copy, once the whole
 *            transfer has ended.
 * @param op  storage_pair_op_t: STORAGE_PAIR_MOVE or STORAGE_PAIR_COPY.
 * @param src Absolute or relative, read against the working directory.
 * @param dst Absolute or relative, read against the working directory.
 * @return KODE_OK when the request went out; KODE_DENIED when either path
 *         does not resolve, and nothing will answer.
 */
kode_status_t console_fs_pair(QActive *who, uint8_t op, const char *src,
                              const char *dst);

/**
 * @brief write path= [append=] [line=<n>] text=<tail>: resolve @p in and ask
 *        AO_Storage to put @p text there, or to replace line @p line of it.
 *
 * No STAT phase. @p line nonzero asks AO_Storage to replace that one line,
 * rewriting the file through a temporary and a rename (32 KB ceiling).
 * @p text is already unescaped: `\n` is a real newline here.
 *
 * @param who    Where STORAGE_PATH_RSP_SIG is posted.
 * @param in     Absolute or relative. NULL means the current directory.
 * @param text   The bytes to write, already unescaped.
 * @param append Keep what is there and add after it. Otherwise truncate.
 *               Ignored when @p line is nonzero.
 * @param line   0 replaces the whole file; n replaces line n of it.
 * @return KODE_OK when the request went out; KODE_DENIED when @p in does not
 *         resolve, in which case nothing was asked and nothing will answer.
 */
kode_status_t console_fs_write(QActive *who, const char *in, const char *text,
                               bool append, uint16_t line);

/**
 * @brief file_put path= size= crc32=: resolve @p in and ask AO_Storage to
 *        receive a payload arriving on the console into it.
 *
 * AO_Storage answers @p who with RECV_READY_SIG, a RECV_ACK_SIG per chunk, and
 * one of RECV_DONE_SIG / RECV_FAIL_SIG / RECV_STALL_SIG. A failed transfer
 * leaves nothing on disk. No STAT phase: an existing file is replaced. Raw
 * mode takes the port @p src names; verified on the cable, untried on BLE.
 *
 * @param who   Where the RECV_* conversation is posted.
 * @param in    Absolute or relative. NULL means the current directory.
 * @param size  Payload length in bytes, from the command line.
 * @param crc32 What khal_crc32() over the payload must equal.
 * @param src   KODE_CONSOLE_SRC_* of the port the command arrived on.
 * @return KODE_OK when the request went out and AO_Storage will answer;
 *         KODE_DENIED when @p in does not resolve or is on no volume;
 *         KODE_NO_RESOURCES when the request event could not be allocated.
 *         On either refusal nothing will answer: the caller closes the command.
 */
kode_status_t console_fs_put(QActive *who, const char *in, uint32_t size,
                             uint32_t crc32, uint8_t src);

/**
 * @brief Put the working directory back to "/". For the host suite.
 *
 * The cwd is a static, and would otherwise leak from one case to the next.
 */
void console_fs_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* CONSOLE_FS_H_ */
