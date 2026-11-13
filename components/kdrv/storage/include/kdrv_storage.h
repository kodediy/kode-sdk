/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_storage.h
 * @brief The two places an app image can live, behind one contract.
 */
#ifndef KDRV_STORAGE_H_
#define KDRV_STORAGE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Which medium an image was found on. */
typedef enum {
    KDRV_MEDIUM_SD = 0,   /**< The microSD card.            */
    KDRV_MEDIUM_FS,       /**< The internal FAT partition.  */
    KDRV_MEDIUM_COUNT
} kdrv_medium_t;

/* ---- Mounting ----------------------------------------------------------- */

/**
 * @brief Mount @p m because someone asked. @return The mount point, or NULL.
 * @note Always retries, however many times it has already failed: this is the
 *       console's `sd mount`, and an operator who types it again has usually
 *       just done something about the reason it failed.
 */
const char *kdrv_storage_mount(kdrv_medium_t m);

/**
 * @brief Mount @p m because we are about to look for a file on it.
 *
 * Differs from kdrv_storage_mount() in one way that matters on this board: the
 * microSD is attempted at most once per boot. With no card in the slot every
 * attempt costs an LDO setup, a bus probe and two error lines - and `app list`
 * would pay that every single time. Once it has failed, only an explicit
 * kdrv_storage_mount() tries again.
 */
const char *kdrv_storage_probe(kdrv_medium_t m);

/** @brief Unmount @p m. */
void kdrv_storage_unmount(kdrv_medium_t m);

/** @brief Whether @p m is mounted right now. */
bool kdrv_storage_mounted(kdrv_medium_t m);

/** @brief The mount point string for @p m, mounted or not. */
const char *kdrv_storage_mount_point(kdrv_medium_t m);

/** @brief Total and free bytes on @p m. */
kode_status_t kdrv_storage_usage(kdrv_medium_t m, uint64_t *total, uint64_t *free_bytes);

/**
 * @brief The allocation unit of @p m's volume: what a file of one byte takes.
 *
 * The internal partition answers; the card has no caller that needs it yet and
 * answers 0, as a volume that is not mounted does.
 *
 * @param m The medium.
 * @return The cluster size in bytes, or 0 when it is not known.
 */
uint32_t kdrv_storage_cluster_bytes(kdrv_medium_t m);

/** @brief Log what @p m is - for the console's `sd info` / `fs info`. */
void kdrv_storage_dump(kdrv_medium_t m);

/* ---- Paths -----------------------------------------------------------
 * The start of a real path model: absolute, virtual paths - "/", "/sdcard/..."
 * and "/storage/..." - mapped onto whichever volume they actually name.
 * kdrv_storage_stat() is the first path function; every one that follows it
 * shares the same mapping, so this is the contract they all read against.
 */

/**
 * @brief What one path is.
 *
 * @c exists is the answer to the question, not the success of asking it - a
 * path that is simply not there comes back KODE_OK with @c exists false, the
 * same way an HTTP 404 is a successful request.
 */
typedef struct {
    bool     exists;
    bool     is_dir;
    uint32_t size;    /**< Zero for a directory. */
} kdrv_stat_t;

/**
 * @brief What @p path is.
 *
 * @param path Absolute, normalised, virtual: "/", "/sdcard/..." or
 *             "/storage/...". Never a host path.
 * @param out  Filled on KODE_OK. Required.
 * @return KODE_OK when the question was answered; KODE_DENIED for a malformed
 *         path, a NULL argument or a path naming no volume; KODE_ERROR when
 *         the volume will not mount.
 */
kode_status_t kdrv_storage_stat(const char *path, kdrv_stat_t *out);

/**
 * @brief Which volume @p vpath names.
 *
 * The one place outside this file that needs the answer is AO_Storage,
 * filling in StorageEntryEvt.from_sd for a path-based listing - exported
 * rather than left for a second component to work out for itself by
 * matching "/sdcard", which would be a second spelling of a volume name.
 *
 * @param vpath Absolute virtual path.
 * @return KDRV_MEDIUM_SD or KDRV_MEDIUM_FS; KDRV_MEDIUM_COUNT for "/" and for
 *         a path naming no volume at all.
 */
kdrv_medium_t kdrv_storage_medium_of(const char *vpath);

/* ---- Listing -------------------------------------------------------------
 * A callback rather than a returned table: the caller is an Active Object
 * scanning on its own thread, and handing it a table would mean someone else
 * could read it while the next scan rewrites it.
 */

/**
 * @brief Called once per directory entry.
 *
 * @param name One path component, not a path. Valid for this call only.
 * @param size The entry's size in bytes, 0 when it will not stat.
 * @param ctx  Whatever the caller handed kdrv_storage_list().
 * @return false to stop the walk.
 */
typedef bool (*kdrv_storage_entry_cb)(const char *name, uint32_t size, void *ctx);

/** @brief Walk @p m, calling @p cb for every regular file. */
kode_status_t kdrv_storage_list(kdrv_medium_t m, kdrv_storage_entry_cb cb, void *ctx);

/**
 * @brief Called once per directory entry, files and directories alike.
 *
 * kdrv_storage_entry_cb above answers "what files are here"; a path-based
 * caller wants "what is here", now that directories exist to be shown.
 *
 * @param name   One path component, not a path. Valid for this call only.
 * @param is_dir Whether the entry is a directory.
 * @param size   The entry's size in bytes; 0 for a directory or a failed stat.
 * @param ctx    Whatever the caller handed kdrv_storage_list_path().
 * @return false to stop the walk.
 */
typedef bool (*kdrv_path_entry_cb)(const char *name, bool is_dir,
                                   uint32_t size, void *ctx);

/**
 * @brief Walk one level of @p path.
 *
 * "/" is answered from here without touching either volume: it is where the
 * two volumes become one tree, and both appear whether or not they are
 * mounted - a card missing from the listing would look exactly like a card
 * with no files on it, which is a lie by omission the console must not tell.
 *
 * @param path Absolute, normalised, virtual.
 * @param cb   Called once per entry. Required.
 * @param ctx  Passed through unexamined.
 * @return KODE_OK, KODE_DENIED for a bad path or a NULL callback, KODE_ERROR
 *         when the volume will not mount or the directory will not open.
 */
kode_status_t kdrv_storage_list_path(const char *path,
                                     kdrv_path_entry_cb cb, void *ctx);

/* ---- Changing the tree -----------------------------------------------
 * The four ways the console may reorganise a volume. Every one of them is
 * "real_path() then one libc call" - the same shape kdrv_storage_stat() has -
 * with one deliberate exception: kdrv_storage_unlink() stats first. See its
 * own note for why deleting is not allowed to be as thin as the other three.
 */

/**
 * @brief Make one directory at @p path.
 *
 * One level only: the parent must already exist. There is no `-p` here any
 * more than there is a `-r` on the delete side (kdrv_storage_unlink(),
 * kdrv_storage_rmdir()) - a console verb that quietly created two more
 * directories to satisfy the one it was asked for would be doing more than
 * it was told, and "more than asked" is exactly the kind of surprise a
 * filesystem verb must not spring on whoever typed it.
 *
 * @param path Absolute, normalised, virtual.
 * @return KODE_OK; KODE_DENIED for a malformed path or one naming no volume;
 *         KODE_ERROR when the volume will not mount or the parent does not
 *         exist; KODE_EXISTS when the name is already taken; KODE_NOT_DIR
 *         when a component of the path is a file; KODE_IO_ERROR when the
 *         parent is there and the medium itself refused the write that
 *         would have created the entry.
 */
kode_status_t kdrv_storage_mkdir(const char *path);

/**
 * @brief Delete the file at @p path. Files only.
 *
 * Stats first, and that is deliberately not this file's usual "real_path()
 * then one libc call" shape. remove() on a directory succeeds on some libcs -
 * glibc's quietly hands an empty one to rmdir() - and that difference must
 * never be the reason an operator's `rm` of a directory does something other
 * than what `rmdir` exists to do instead.
 *
 * @param path Absolute, normalised, virtual.
 * @return KODE_OK; KODE_DENIED for a malformed path, one naming no volume, or
 *         a path that answers and is a directory; KODE_ERROR when the volume
 *         will not mount, the file is not there, or it will not go.
 */
kode_status_t kdrv_storage_unlink(const char *path);

/**
 * @brief Remove the directory at @p path. Empty directories only.
 *
 * No recursive form and none is coming: a recursive delete needs either
 * recursion on a task stack or an explicit walk stack, and it is the one
 * file operation whose bugs are unrecoverable. A directory that still holds
 * something refuses exactly the way the host libc's own rmdir() already
 * refuses one - ENOTEMPTY becomes KODE_ERROR, not a special case invented
 * here.
 *
 * @param path Absolute, normalised, virtual.
 * @return KODE_OK; KODE_DENIED for a malformed path or one naming no volume;
 *         KODE_ERROR when the volume will not mount, the path is not there,
 *         is not a directory, or is a directory that is not empty.
 */
kode_status_t kdrv_storage_rmdir(const char *path);

/**
 * @brief Rename or move @p from to @p to. Same volume only.
 *
 * FAT cannot rename across a mount, and pretending otherwise by silently
 * copying would make one verb mean two very different amounts of time and
 * risk - crossing volumes is `cp` followed by `rm`, spelled out at the
 * console rather than hidden inside a call that looks like this one but is
 * not. The two volumes are compared with kdrv_storage_medium_of() before
 * either path is touched, so a refused crossing never opens anything.
 *
 * FAT never replaces a file: a @p to that is another object than @p from is
 * refused, and the caller removes it first.
 *
 * @param from Absolute, normalised, virtual.
 * @param to   Absolute, normalised, virtual.
 * @return KODE_OK; KODE_DENIED for a malformed path, either path naming no
 *         volume, or the two paths naming different volumes; KODE_ERROR when
 *         the volume will not mount, @p to is there, or the rename itself
 *         fails.
 */
kode_status_t kdrv_storage_rename(const char *from, const char *to);

/* ---- Reading and writing -------------------------------------------------
 * One reader and one writer at a time, system-wide. It was one handle for
 * both until copying existed, and copying is exactly the pair: a second
 * reader is still refused, and so is a second writer. AO_Storage is the only
 * caller and it is one thread, so two handles here is a copy loop rather
 * than a race.
 *
 * What is allowed is that pair and nothing else. Nobody may read two files,
 * nobody may write two files - a second reader or a second writer is refused
 * exactly as before. What DID widen: kdrv_storage_create_path() (and
 * kdrv_storage_create()) used to refuse outright while a read was open,
 * because there was only one handle for both; that refusal was an accident
 * of the single handle rather than a rule anybody wanted, and it is gone.
 * A write may now open and run while a read is in flight - that is a real
 * widening of what this file will accept, not a wording change, because it
 * is the one refusal that used to stand in for a guard nobody had written.
 *
 * kdrv does not decide who is allowed to call in at all - it never has. It
 * is AO_Storage's job, not this header's, to refuse a second asker while one
 * is already being served; kdrv only enforces the shape of the one pair it
 * will hold. Callers outside AO_Storage never reach this file directly, so
 * that job is done in exactly one place (`busy()` in storage_ao.c) rather
 * than here.
 */

/**
 * @brief Find @p name and open it, looking on the SD first.
 *
 * A bare file name, not a path: this is the flat call `app_run`, `install src=`
 * and `file_get` all reach storage through, and the search order is its whole
 * contribution. Everything below the order is kdrv_storage_open_path().
 *
 * @param[out] size    The file's size in bytes. Required.
 * @param[out] medium  Where it was found. May be NULL.
 * @return KODE_OK; KODE_DENIED for a NULL argument, a file already open for
 *         READING, or a file that is there and zero length; KODE_ERROR when
 *         the name is on neither medium.
 */
kode_status_t kdrv_storage_open(const char *name, uint32_t *size,
                                kdrv_medium_t *medium);

/**
 * @brief Open exactly @p path for reading. No searching.
 *
 * kdrv_storage_open() above with the policy taken out: this one opens what it
 * is given and nothing else. The two failure codes are the difference that
 * lets a caller build a search on top - KODE_ERROR is "not on this volume",
 * KODE_DENIED is "here and unusable", and only the first is a reason to look
 * somewhere else.
 *
 * @param path Absolute, normalised, virtual: "/sdcard/..." or "/storage/...".
 * @param size Out: the file's size in bytes. Required.
 * @return KODE_OK; KODE_DENIED for a bad path, a file already open for
 *         READING, or a file that is there and zero length; KODE_ERROR when
 *         the file is not there or its volume will not mount.
 */
kode_status_t kdrv_storage_open_path(const char *path, uint32_t *size);

/**
 * @brief Read the next chunk of the open file.
 * @param[out] got  Bytes actually read; 0 means end of file.
 */
kode_status_t kdrv_storage_read(void *buf, size_t max, size_t *got);

/**
 * @brief Move the reader kdrv_storage_open_path() opened.
 * @param offset Byte the next read starts at; past the end is legal and the
 *               next read gets 0 bytes.
 * @return KODE_OK; KODE_ERROR with no reader open or when the seek fails.
 */
kode_status_t kdrv_storage_seek(uint32_t offset);

/**
 * @brief Create @p name for writing on @p first, or on another medium if that
 *        one will not take it.
 *
 * Truncates an existing file of that name. The writer's handle is its own -
 * it stopped being shared with kdrv_storage_open() when copying arrived - so
 * a second create is refused and a read in progress is not: see this
 * section's own note on why the old refusal was an accident of one handle
 * rather than a rule.
 *
 * Reading looks at the card first and writing does **not**, and the asymmetry
 * is the point: those are different questions. A read asks where somebody is
 * most likely to have put a file, and somebody who went to the trouble of
 * inserting a card meant it. A write asks where the bytes will actually
 * survive, and the answer is the medium the board owns - always present,
 * always writable, and impossible to swap for a counterfeit.
 *
 * That is not a hypothetical. Writing to one of the fake cards some units
 * shipped with does not merely fail: FATFS is left holding dirty sectors it
 * can never flush, and every later read of that volume comes back as a
 * zero-length file. One capture written to the card cost the rest of the
 * session's reads from it.
 *
 * @param[out] medium  Where it was created. May be NULL.
 * @return KODE_OK; KODE_DENIED for a NULL name or a file already open for
 *         WRITING; KODE_ERROR when neither medium would take it.
 */
kode_status_t kdrv_storage_create(const char *name, kdrv_medium_t first,
                                  kdrv_medium_t *medium);

/**
 * @brief Create or reopen exactly @p path for writing. No fallback.
 *
 * kdrv_storage_create() above with the fallback taken out. The choice of
 * medium is a policy and it stays up there; what is left here is opening one
 * named file, on the one volume its path names.
 *
 * @param path   Absolute, normalised, virtual.
 * @param append Keep what is there and write after it. Otherwise truncate.
 *               This also decides what kdrv_storage_discard() will do if the
 *               write that follows fails half way: a truncating create (the
 *               default) starts the file from nothing, so discard() may
 *               remove it whole; an append reopens a file that can already
 *               hold good data from before this call, so discard() leaves it
 *               in place rather than deleting content this call did not
 *               write. See kdrv_storage_discard()'s own note - a caller
 *               reading only this one must not come away thinking discard()
 *               undoes an append the same way it undoes a truncating create.
 * @return KODE_OK; KODE_DENIED for a bad path or a file already open for
 *         WRITING; KODE_ERROR when the volume will not mount or the parent
 *         does not exist; KODE_NOT_DIR when a component of the path is a
 *         file; KODE_IS_DIR when the path itself names a directory;
 *         KODE_IO_ERROR when the parent is there and the medium itself
 *         refused the open.
 */
kode_status_t kdrv_storage_create_path(const char *path, bool append);

/** @brief Append @p len bytes to the file kdrv_storage_create() opened. */
kode_status_t kdrv_storage_write(const void *buf, size_t len);

/**
 * @brief Close the file being written and delete it - unless it was opened
 *        for append, in which case it is closed and left alone.
 *
 * For a *truncating* create that failed half way: the file it removes was
 * started from nothing by this session (kdrv_storage_create_path(path,
 * false), or kdrv_storage_create(), which never appends), so removing it
 * whole costs nothing that existed before. Leaving the stub behind would put
 * a name in every listing that promises bytes it does not have, which is
 * worse than no file at all - `app_run` would try to boot it.
 *
 * An *append* is a different call wearing the same name. The file
 * kdrv_storage_create_path(path, true) reopened can already hold good data
 * from before this session - a log, a capture resumed after a reset - and
 * this function must not take that down with whatever partial write just
 * failed. Calling it after a failed append closes the handle, logs why
 * nothing was removed, and leaves the file exactly as the failed write left
 * it: whole, but with whatever it managed to append still in place. Undoing
 * an append's own bytes is the caller's job, because only the caller knows
 * how much of what it wrote is salvageable.
 */
void kdrv_storage_discard(void);

/**
 * @brief Close whatever is open - the reader, the writer, or both.
 *
 * One call for both handles, deliberately. Every caller in the tree already
 * meant "I am done with the file", and splitting this into a close_read()
 * and a close_write() would make every one of them pick, which is exactly
 * the kind of choice a caller gets wrong once and then lives with: a read
 * that ended without releasing its handle refuses the next command for the
 * rest of the session, and the operator sees "a file is already open" about
 * a file nothing is using.
 *
 * @return KODE_OK when everything that was open closed cleanly, and when
 *         nothing was open at all; KODE_ERROR when the WRITER did not close
 *         cleanly, in which case its last buffered bytes may never have
 *         reached the medium.
 *
 * That return value is not decoration and it used to not exist. fclose() is
 * where a buffered write finally meets the card, so it is where a card that
 * cannot take the bytes says so - and throwing the answer away meant a write
 * reported success for a file that did not fully land. On the counterfeit
 * cards some units shipped with that is not a corner: one failed write
 * leaves FATFS holding sectors it can never flush, and every later open on
 * that volume returns length 0 for the rest of the session. A `write` or a
 * `cp` that answers `>ok` for bytes that never landed is worse than one that
 * fails loudly, so every path that writes checks this. A caller that
 * genuinely has nothing at stake - one that only ever read - may ignore it,
 * and says so where it does.
 *
 * Calling this while a copy is open is not the normal way to end one -
 * kdrv_storage_copy_end() is - but it is not left to corrupt the volume
 * either: kdrv_storage_copy_end() always clears the copy's own flag before
 * it does anything else, so this function can tell an ordinary close from
 * one that caught a copy still open, and it REMOVES that copy's destination
 * rather than leaving a half-written file with a name in every listing. That
 * guarantee belongs to this call, not to whichever caller happens to reach
 * it - AO_Storage additionally refuses to reach this path while a copy is in
 * flight, but this function does not depend on that refusal to keep the
 * promise.
 */
kode_status_t kdrv_storage_close(void);

/* ---- Copying -------------------------------------------------------------
 * The one thing the two handles above exist for. A copy is a reader and a
 * writer held at the same instant, which a single handle could not do at
 * all, and it is driven in steps because the caller is an Active Object with
 * three lower-priority ones underneath it: AO_Gui, AO_Radio and AO_Net all
 * sit below AO_Storage, and a copy that ran to completion inside one
 * dispatch step would freeze the screen for its whole duration.
 *
 * The buffer belongs to the caller, and that is the point: AO_Storage
 * already owns two 32 KB PSRAM buffers for streaming, marks one lent for the
 * length of the copy, and hands it in here. A buffer of this layer's own
 * would be 32 KB of internal RAM sitting idle most of the time, which is the
 * exact reason those two live in PSRAM in the first place.
 *
 * The three calls are one sequence and only one: open, step until it reports
 * nothing, end. There is no way to run two copies at once and no reason to
 * want one - the pair of handles is a pair, not a pool.
 */

/**
 * @brief Open @p from for reading and @p to for writing, as one copy.
 *
 * @p to is created truncating, never appending: a copy replaces whatever was
 * at the destination, and that is also what lets kdrv_storage_copy_end(false)
 * remove the destination whole - it holds nothing this call did not put
 * there.
 *
 * A copy onto its own source is refused rather than attempted. The two paths
 * are compared before either is touched, because the order the calls would
 * otherwise run in is fatal: the source is opened for reading, then the same
 * file is opened truncating, and the bytes are gone before the first step
 * ever runs. Only the two virtual paths are compared - a caller that spells
 * one file two ways gets no protection, which is why every path arriving
 * here is normalised above (spec section F).
 *
 * @param from Absolute, normalised, virtual.
 * @param to   Absolute, normalised, virtual.
 * @param size Out: the source's size in bytes, for a progress line. Required.
 * @return KODE_OK; KODE_DENIED for a NULL argument, a copy already running,
 *         the two paths naming one file, either path naming no volume, or a
 *         handle already open on the side that needs one; KODE_ERROR when a
 *         volume will not mount, the source is not there, or the destination
 *         will not open. Nothing is left open on any failure.
 */
kode_status_t kdrv_storage_copy_open(const char *from, const char *to,
                                     uint32_t *size);

/**
 * @brief Move one window of the copy opened by kdrv_storage_copy_open().
 *
 * @param buf  The caller's window. Required.
 * @param cap  How much of it to use. The caller's own chunk size - AO_Storage
 *             passes its whole 32 KB buffer.
 * @param done Out: bytes moved this step. ZERO MEANS THE SOURCE RAN OUT -
 *             the same "a successful read of nothing" end-of-file signal
 *             kdrv_storage_read() already uses, rather than a second
 *             vocabulary for the same event. Required.
 * @return KODE_OK; KODE_DENIED for a NULL argument, a zero @p cap, or no
 *         copy open; KODE_ERROR when the read or the write failed, in which
 *         case the copy is over and kdrv_storage_copy_end(false) is what
 *         cleans up after it.
 */
kode_status_t kdrv_storage_copy_step(void *buf, size_t cap, size_t *done);

/**
 * @brief End the copy: close both handles, and keep or remove what was
 *        written.
 *
 * @param keep true when every step succeeded and the source ran out. false
 *             for a copy that failed or was abandoned part way, in which
 *             case the destination is REMOVED - a name in a listing that
 *             promises bytes it does not have is worse than no file at all,
 *             the same reason kdrv_storage_discard() gives for the write it
 *             undoes, and `app_run` would try to boot the stub.
 * @return KODE_OK; KODE_ERROR when @p keep was true and the destination did
 *         not close cleanly - in which case the destination is removed too,
 *         because a file whose last bytes never reached the medium is the
 *         same stub by another route, and a `cp` that answered `>ok` over it
 *         is the failure this whole call chain exists to stop.
 */
kode_status_t kdrv_storage_copy_end(bool keep);

/**
 * @brief Delete @p name from @p m.
 *
 * Named rather than "the file that is open", which is kdrv_storage_discard()'s
 * job: this one is for a file that was written, closed and is no longer
 * wanted - the card test's megabyte, which must not be left behind on a card
 * the operator is about to use.
 *
 * @param name The file, as a listing named it.
 * @param m    Which medium to look on. Only that one; a delete that went
 *             hunting could remove the wrong copy of a name that exists twice.
 * @return KODE_OK; KODE_DENIED for a bad argument or a medium that will not
 *         mount; KODE_ERROR when the file is not there or will not go.
 */
kode_status_t kdrv_storage_remove(const char *name, kdrv_medium_t m);

/**
 * @brief Write a scratch file, read it back and delete it.
 *
 * The console's `test fs`. It lives here rather than in the console because
 * it is the only thing in kodeOS that writes to a medium, and a write from
 * the console's thread would be exactly the kind of access this component
 * exists to keep in one place.
 */
kode_status_t kdrv_storage_selftest(kdrv_medium_t m);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_STORAGE_H_ */
