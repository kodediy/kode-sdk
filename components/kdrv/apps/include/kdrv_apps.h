/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_apps.h
 * @brief The `apps` region: a private flash filesystem for installed apps.
 */
#ifndef KDRV_APPS_H_
#define KDRV_APPS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kapp_desc.h"   /* kapp_desc_t: the app's own manifest shape */
#include "kapps_fmt.h"
#include "kdrv_slot.h"   /* kdrv_slot_desc_t: the manifest shape */
#include "kode_defs.h"
#include "kdrv_kfw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The region itself ---------------------------------------------------- */

/**
 * @brief Find the `apps` partition; load its directory, or format it if new.
 *
 * Safe to call more than once: AO_Boot calls it before asking about the boot
 * request, AO_AppMgr calls it when claiming the region - whoever runs first
 * pays the read.
 */
kode_status_t kdrv_apps_init(void);

/** @brief Is there an `apps` partition at all? */
bool kdrv_apps_present(void);

/** @brief The region's total size in bytes, 0 when it is absent. */
uint32_t kdrv_apps_region_size(void);

/** @brief The region's absolute flash address - for `boot info`. */
uint32_t kdrv_apps_address(void);

/** @brief The in-RAM working copy of the directory. Never NULL. */
const kapps_dir_t *kdrv_apps_dir(void);

/**
 * @brief Commit a new directory.
 *
 * Always writes the currently INACTIVE A/B copy (gen = current + 1, CRC
 * recalculated), erasing its sector first. The RAM working copy - and which
 * copy counts as active - only change once that write has succeeded.
 */
kode_status_t kdrv_apps_dir_commit(const kapps_dir_t *d);

/* ---- The allocator ---------------------------------------------------------
 * Log-structured: the next image goes right after the newest one (by install
 * order), sweeping toward the end of the region and wrapping to its start.
 * When nothing fits, the app the image replaces goes first, then the oldest
 * apps - by install order, not by any notion of use - until it fits, or until
 * eviction alone cannot help.
 */
/** @brief No directory entry: kapps_plan_t.slot_victim when none applies. */
#define KAPPS_NO_VICTIM  0xFFU

typedef struct {
    uint32_t offset;                   /**< Where the new image goes.                       */
    uint8_t  evict[KAPPS_MAX_ENTRIES]; /**< Directory indices to remove, in eviction order.  */
    uint32_t evict_count;
    uint8_t  slot_victim;              /**< The entry appdir_make_room() takes after the
                                        *   write when the directory would still be full -
                                        *   the oldest live entry not in evict[] - or
                                        *   KAPPS_NO_VICTIM. Predicted, not evicted: a
                                        *   namesake frees its own entry, and only the
                                        *   write tells.                                    */
    uint8_t  replaced;                 /**< The replaced app when evict[] holds it, else
                                        *   KAPPS_NO_VICTIM: it is not lost, the install
                                        *   brings it back.                                 */
} kapps_plan_t;

/**
 * @brief Plan where an image of @p img_size bytes would land.
 *
 * Pure function over the snapshot @p d: no I/O, no static state. That is
 * what makes it testable without a board - see `test apps`. It also names
 * `slot_victim`, for a caller that must act on that entry before the write
 * (AO_AppMgr copies it to the card).
 *
 * @param d           The directory to plan against.
 * @param region_size The region's size in bytes.
 * @param img_size    The image's size in bytes.
 * @param replacing   The entry of the app this image is a new version of, or
 *                    KAPPS_NO_VICTIM. When there is no room, it is evicted
 *                    before any other app: its old copy would otherwise cost
 *                    every older app its place, and the install replaces it
 *                    anyway.
 * @param out         The plan.
 * @return KODE_OK, or KODE_ERROR when the image cannot fit even in an empty
 *         region (nothing is planned then).
 */
kode_status_t kdrv_apps_plan(const kapps_dir_t *d, uint32_t region_size,
                             uint32_t img_size, uint8_t replacing,
                             kapps_plan_t *out);

/* ---- Writing an image into the region -------------------------------------- */

/** @brief Open a write session at a region-relative @p offset for @p size bytes. */
kode_status_t kdrv_apps_write_begin(uint32_t offset, uint32_t size);

/** @brief Append to the open session, erasing 64 KB blocks as it crosses them. */
kode_status_t kdrv_apps_write(const void *data, size_t len);

/** @brief Close the session and verify the image that was written. */
kode_status_t kdrv_apps_write_end(void);

/** @brief Abandon the session. Whatever was written stays, unverified. */
void kdrv_apps_write_abort(void);

/**
 * @brief Has the write erased any part of an extent?
 *
 * Answers for the write session in hand, or the last one that ran, and starts
 * over with every committed directory: a commit says which extents hold apps,
 * so an erase made before it speaks for nothing that comes after. An erase that
 * failed counts as having reached its block, because flash a failed erase
 * touched is not flash that can be trusted. Only blocks the write got to count:
 * the region erases 64 KB at a time, as the data arrives.
 *
 * @param offset Region-relative start of the extent.
 * @param size   Its length in bytes.
 * @return true when some of its bytes may no longer be what they were; false
 *         when every one of them is, or when @p size is 0.
 */
bool kdrv_apps_write_reached(uint32_t offset, uint32_t size);

/**
 * @brief Read @p len bytes of the region at region-relative @p offset.
 *
 * For reading an installed image back out - the copy AO_AppMgr makes on the
 * card before an eviction. Only the data area: the directory and the boot
 * request below it have their own calls.
 *
 * @param offset Region-relative, at or past KAPPS_DATA_OFF.
 * @param buf    Where the bytes go.
 * @param len    How many.
 * @return KODE_OK; KODE_DENIED with no region or a NULL @p buf; KODE_ERROR for
 *         an extent outside the data area or a failed read.
 */
kode_status_t kdrv_apps_read(uint32_t offset, void *buf, size_t len);

/**
 * @brief Fold the bytes of an extent of the region into a running CRC-32.
 *
 * Reads exactly @p size bytes from @p offset, in pieces, and never one more:
 * what lies beyond the extent is not this call's to touch. The CRC is the
 * one khal_crc32() gives, chainable, so an extent may be taken in parts: pass
 * KHAL_CRC32_INIT in @p crc for the first and the result for the next. It
 * does not parse the data. An image a reset half overwrote is another
 * image's header and segments, and what a parser makes of them is not bounded
 * by @p size; a digest is.
 *
 * @param offset Region-relative start, at or past KAPPS_DATA_OFF.
 * @param size   How many bytes; 0 reads nothing and changes nothing.
 * @param crc    The running CRC on entry and, on KODE_OK, the updated one;
 *               left alone on any other answer.
 * @return KODE_OK; KODE_DENIED with no region or a NULL @p crc; KODE_ERROR for
 *         an extent that leaves the data area or one that cannot be mapped.
 */
kode_status_t kdrv_apps_extent_crc(uint32_t offset, uint32_t size, uint32_t *crc);

/** @brief What kdrv_apps_sink() needs: where kdrv_apps_plan() said to write. */
typedef struct {
    uint32_t offset;   /**< Region-relative, 64 KB-aligned. */
} kdrv_apps_sink_ctx_t;

/**
 * @brief The region as a kfw_sink_t.
 * @return A singleton; never NULL. Pass a kdrv_apps_sink_ctx_t as context.
 */
const kfw_sink_t *kdrv_apps_sink(void);

/**
 * @brief Read an installed app's manifest out of its image header.
 * @param e        The directory entry naming the image.
 * @param out      IDF's own descriptor, translated; never NULL.
 * @param manifest Optional (may be NULL): the app's own kapp_desc_t, zeroed
 *                 when the image carries none - which is not an error.
 * @return KODE_OK when @p out was filled.
 */
kode_status_t kdrv_apps_desc(const kapps_entry_t *e, kdrv_slot_desc_t *out,
                             kapp_desc_t *manifest);

/**
 * @brief Verify the image found at @p offset within @p size bytes.
 * @param img_len Optional (may be NULL): the real image length taken from
 *                its header - the extent (or the mailbox) is usually larger
 *                than the image inside it.
 */
kode_status_t kdrv_apps_verify(uint32_t offset, uint32_t size, uint32_t *img_len);

/** @brief Erase the first 4 KB of an extent - enough to kill its header. */
kode_status_t kdrv_apps_erase_header(uint32_t offset);

/* ---- The boot request: which app the bootloader jumps to next ------------- */

/**
 * @brief Erase and write a fresh request: "boot this next reset".
 * @param offset Region-relative offset of the image (an MMU page).
 * @param size   Its exact length.
 * @param state  KAPPS_REQ_STATE_NEW (one boot; any reset comes home) or
 *               KAPPS_REQ_STATE_ALWAYS (the bootloader arms it on its first
 *               boot and then keeps it across USB-Serial/JTAG resets; only
 *               mailbox adoption asks for it). KAPPS_REQ_STATE_ARMED is the
 *               bootloader's to write, never kodeOS's, so it is refused here.
 * @return KODE_OK; KODE_DENIED before init; KODE_ERROR on a bad geometry, a
 *         state that is neither of the two, or a flash failure.
 */
kode_status_t kdrv_apps_req_set(uint32_t offset, uint32_t size, uint32_t state);

/** @brief True (with the extent's offset) if the bootloader left state=TRIED. */
bool kdrv_apps_req_tried(uint32_t *offset);

/**
 * @brief Is a sticky run standing launched but not retired (state=ARMED)?
 *
 * The bootloader writes ARMED when it jumps to a request that was ALWAYS, and
 * retires it to TRIED before it boots kodeOS. kodeOS therefore reads ARMED
 * only when that retire write did not stick; the app did run.
 *
 * @param offset Receives the extent's region-relative offset when true; NULL
 *               to skip.
 * @return true when a valid request stands in state ARMED.
 */
bool kdrv_apps_req_armed(uint32_t *offset);

/**
 * @brief Is any boot request record standing (not erased)?
 *
 * True when a valid record exists whose state is not EMPTY - NEW, ALWAYS,
 * ARMED or TRIED. kodeOS running proves the app is done, so a standing
 * request here means a bootloader write did not stick; AO_Boot clears it.
 *
 * @return true when a non-EMPTY request is present.
 */
bool kdrv_apps_req_present(void);

/** @brief Erase the request sector: no app is requested. */
void kdrv_apps_req_clear(void);

/* ---- app_pending: a fixed mailbox a host tool writes to -------------------- */

/**
 * @brief Does the mailbox hold something worth copying, and if so how long
 *        is it?
 *
 * A length check, not a cryptographic one - it walks the image's own header
 * chain by esp_partition_read, which reads any address; the walk is what
 * bounds the copy length (see the two-phase note in kdrv_apps.c). The real
 * verification is kdrv_apps_write_end(), once the bytes are copied into the
 * apps region.
 *
 * Two lengths come back because a Kode container is not what it carries: the
 * mailbox holds the header and the (possibly deflated) payload, and the
 * decoder places the inflated image. A copy reads the first, an eviction plan
 * must be made for the second - planned on the first, a deflated container
 * would name fewer victims than the decoder's own plan later does.
 *
 * @param[out] img_len     Bytes the mailbox holds: the walk's length for a
 *                         bare image, KFW_HDR_LEN plus the payload for a
 *                         container. May be NULL.
 * @param[out] decoded_len Bytes the image occupies once decoded: equal to
 *                         @p img_len for a bare image, the header's stated
 *                         image length for a container. May be NULL.
 * @return true when the mailbox holds a bare image or a container; false
 *         when it is empty or was broken (and so consumed).
 */
bool kdrv_apps_pending_present(uint32_t *img_len, uint32_t *decoded_len);

/**
 * @brief Crypto-verify the mailbox image in place (a bare image only).
 *
 * The real esp_image_verify(), run on app_pending at 0x1A00000 rather than on
 * the copy in the region. Its purpose is to judge a mailbox image BEFORE
 * adoption evicts a live app for it, so a torn or corrupt upload - one whose
 * headers walk but whose data is bad - is discarded without costing anybody
 * their installed app. A Kode container is not a bare image and cannot be
 * verified here; this returns KODE_OK for one, leaving the post-copy check to
 * judge it.
 *
 * @return KODE_OK when a bare image verifies (or the mailbox is not a bare
 *         image), KODE_ERROR when a bare image fails.
 */
kode_status_t kdrv_apps_pending_verify(void);

/**
 * @brief Open the mailbox for writing @p size bytes.
 *
 * What goes in is whatever the source delivered, container header included:
 * parking the decoded image would mean the copy is only ever checked once,
 * at the moment it was least likely to be wrong.
 */
kode_status_t kdrv_apps_pending_write_begin(uint32_t size);

/** @brief Append to the open mailbox session, erasing sectors as it goes. */
kode_status_t kdrv_apps_pending_write(const void *data, size_t len);

/** @brief Close it. Checks the byte count, not the content. */
kode_status_t kdrv_apps_pending_write_end(void);

/** @brief Abandon it. */
void kdrv_apps_pending_write_abort(void);

/** @brief The mailbox as a kfw_sink_t. Context unused. */
const kfw_sink_t *kdrv_apps_pending_sink(void);

/** @brief Read @p len bytes from the mailbox at @p offset. */
kode_status_t kdrv_apps_pending_read(uint32_t offset, void *buf, size_t len);

/**
 * @brief Does the mailbox's intent sector say "treat this as a kodeOS OTA
 *        candidate" rather than a plain app?
 *
 * A host tool signals this by writing KAPPS_OS_INTENT_MAGIC at the start of
 * the mailbox's last 4 KB sector (KAPPS_PENDING_INTENT_OFF on today's 6 MB
 * partition) - a separate address, written before the image - never
 * inferred from the image's own bytes. Anything else there, including
 * erased flash, means "no, adopt it as an app": today's behaviour.
 */
bool kdrv_apps_pending_os_intent(void);

/** @brief Erase the mailbox's first 4 KB and its intent sector: no more
 *         0xE9, no more OS-intent flag, nothing left to adopt either way. */
kode_status_t kdrv_apps_pending_consume(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_APPS_H_ */
