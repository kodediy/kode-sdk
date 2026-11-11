/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_internal.h
 * @brief Not part of the API: the blocking calls kode.h is built from, for kode_sys and kode only.
 */
#ifndef KODE_SYS_INTERNAL_H_
#define KODE_SYS_INTERNAL_H_

#ifndef KODE_SYS_INTERNAL_ALLOWED
#error "kode_sys_internal.h is private to kode_sys and kode: an app uses kode.h from a task or kode_sys.h from an Active Object"
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "kode_sys.h"
#include "kdrv_exp_bus.h"   /* J3's buses: kode.h calls the driver itself, see below */
#include "kdrv_ir.h"        /* kdrv_ir_symbol_t, kdrv_ir_frame_t */
#include "kdrv_es8311.h"    /* kdrv_es8311_info_t */
#include "kdrv_qmc5883p.h"  /* kdrv_qmc5883p_cal_t */
#include "kdrv_qmi8658.h"   /* kdrv_qmi8658_sample_t */
#include "knfc_tag.h"       /* knfc_tag_t */
#include "knfc_emulate.h"   /* knfc_emu_card_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- J3's buses ----
 * kdrv_exp_bus.h's calls, included above, wait on the wire, so kode.h reaches
 * them here and kode_sys.h keeps the books alone (kode_sys_exp_lend(),
 * kode_sys_exp_ctrl()). They have no lock: the standard layer calls them
 * under a lock of its own for each bus.
 */

/* ---- Time ---- */

/**
 * @brief Sleep, yielding the CPU.
 * @note Never call this from inside an AO.
 * @param ms Milliseconds to sleep.
 */
void kode_block_delay_ms(uint32_t ms);

/* ---- Infrared: through AO_Ir; without KODEOS_RADIO (the block is on the C5) the calls refuse ---- */

/**
 * @brief Send any infrared frame of @p n {mark_us, space_us} symbols on @p carrier_hz.
 * @note Blocks until AO_Ir answers. One call at a time. AO_Ir copies @p sym as
 *       the call starts, so the caller may reuse it on return.
 * @param sym        The symbols, mark first.
 * @param n          How many, 1..KODE_SYS_IR_RAW_MAX_SYMBOLS.
 * @param carrier_hz The carrier; 38000 for most remotes and for this board's receiver.
 * @return KODE_OK when the frame went to the C5. KODE_DENIED for a bad argument,
 *         while another raw send is in flight, while a timed-out one is still
 *         unanswered, or when AO_Ir's queue of held requests is full.
 *         KODE_NO_RESOURCES when the event pool was dry, AO_Ir had no memory
 *         for its copy, or AO_Ir's worker had no room. KODE_TIMEOUT when
 *         nothing answered in time, or when the frame waited too long to start
 *         (it is then never sent). Otherwise the driver's status.
 */
kode_status_t kode_block_ir_send_raw(const kode_sys_ir_symbol_t *sym, size_t n,
                                     uint32_t carrier_hz);

/**
 * @brief Wait up to @p ms for one frame and copy its symbols into @p out.
 * @note Blocks (an AO uses kode_sys_ir_capture()). Not reentrant: a second
 *       caller gets KODE_DENIED.
 * @param out Out: kdrv_ir_symbol_t[@p cap]; at most KDRV_IR_RX_MAX_SYMBOLS are
 *            written. Written only on KODE_OK.
 * @param cap How many symbols @p out holds.
 * @param n   Out: how many were written; may be NULL. Written only on KODE_OK.
 * @param ms  How long to wait, capped at KODE_SYS_IR_TIMER_MS_MAX; ask again for longer.
 * @return KODE_OK when AO_Ir answered in time, even with zero symbols (@p n
 *         says). KODE_TIMEOUT when it did not (@p out and @p n untouched).
 *         KODE_ERROR for a NULL @p out or a zero @p cap. KODE_DENIED while
 *         another call waits.
 */
kode_status_t kode_block_ir_capture_symbols(kdrv_ir_symbol_t *out, size_t cap, size_t *n,
                                            uint32_t ms);

/**
 * @brief The last frame AO_Ir heard, whoever was listening.
 *
 * That includes a frame suppressed as this board's own echo or consumed by a test.
 * @note Blocks for one round trip to AO_Ir. Not reentrant: a second caller gets
 *       KODE_DENIED.
 * @param out Out: the frame. Written only on KODE_OK.
 * @return KODE_OK when a frame is written. KODE_ERROR for a NULL @p out.
 *         KODE_DENIED when AO_Ir has heard nothing yet or another call waits.
 *         KODE_TIMEOUT if AO_Ir does not answer.
 */
kode_status_t kode_block_ir_last(kdrv_ir_frame_t *out);

/* ---- Audio: through AO_Audio. The speaker amplifier is the C5's: with another C5 image every call still returns KODE_OK and plays silence ---- */

/**
 * @brief What the audio path is and what it is set to.
 * @note Blocks for one round trip to AO_Audio. Only another info call refuses
 *       it; a sound or a stream does not.
 * @param out Out: written only on KODE_OK.
 * @return KODE_OK when the answer arrived; KODE_DENIED for a NULL @p out or
 *         while another info call is in flight; KODE_TIMEOUT when AO_Audio did
 *         not answer.
 */
kode_status_t kode_block_audio_info(kdrv_es8311_info_t *out);

/**
 * @brief Record for @p ms and wait until it has finished.
 * @note Blocks. One audio call at a time.
 * @param out Where the frames go.
 * @param cap How many @p out holds.
 * @param n   How many arrived; may be NULL.
 * @param ms  How long to record, up to KODE_SYS_AUDIO_BUF_SECONDS of it.
 * @return KODE_OK when the whole recording arrived. KODE_DENIED for a bad
 *         argument, a length over the ceiling, or a second caller.
 *         KODE_NO_RESOURCES when the buffer could not be allocated. KODE_TIMEOUT
 *         when AO_Audio never answered.
 */
kode_status_t kode_block_audio_record_samples(int16_t *out, size_t cap, size_t *n,
                                              uint32_t ms);

/**
 * @brief Record for @p ms straight into a 48 kHz 16-bit mono WAV file and wait until it is written.
 * @note Blocks. One audio call at a time. The frames never pass through the
 *       caller, so KODE_SYS_FILE_READ_MAX does not apply.
 * @param path  Absolute, virtual; the parent directory must exist.
 * @param ms    How long to record, up to KODE_SYS_AUDIO_BUF_SECONDS of it.
 * @param bytes Out: the file's length, header included; may be NULL.
 * @return KODE_OK. KODE_DENIED for a bad argument, a second caller or a codec in
 *         use. KODE_NO_RESOURCES when the buffer could not be allocated.
 *         KODE_TIMEOUT when nothing answered. Otherwise AO_Storage's status.
 */
kode_status_t kode_block_audio_record_file(const char *path, uint32_t ms,
                                           uint32_t *bytes);

/* ---- The stream: sound written as it is made; it holds the codec from open to close ---- */

/**
 * @brief Open the stream and wait until it is playing.
 * @note Blocks. Every other sound and a second stream are refused meanwhile.
 * @note One task at a time; the opener may hand the stream over before it writes.
 * @param rate_hz  KODE_SYS_AUDIO_RATE_HZ; any other rate is refused.
 * @param channels KODE_SYS_AUDIO_CHANNELS_MONO, or _STEREO for left-right pairs played as their mean.
 * @return KODE_OK once it plays silence and waits for frames. KODE_DENIED for
 *         another rate or channel count, a stream already open, or a codec in
 *         use. KODE_NO_RESOURCES when there is no PSRAM for its ring.
 *         KODE_TIMEOUT when AO_Audio never answered.
 */
kode_status_t kode_block_audio_stream_open(uint32_t rate_hz, uint8_t channels);

/**
 * @brief Write frames to the stream.
 * @note Blocks only while it is full. One task at a time; the opener may hand the stream over before it writes.
 * @param frames @p n frames of the kind the stream was opened with.
 * @param n      How many frames.
 * @param ms     The longest to wait for room in all; 0 takes what fits and returns.
 * @param taken  Out: how many frames went in; may be NULL.
 * @return KODE_OK when all @p n went in. KODE_TIMEOUT when @p ms ran out first
 *         (@p taken says how far). KODE_DENIED when no stream is open.
 */
kode_status_t kode_block_audio_stream_write(const int16_t *frames, size_t n,
                                            uint32_t ms, size_t *taken);

/**
 * @brief Close the stream and wait until what it held has been heard.
 * @note Blocks. One task at a time; the opener may hand the stream over before it writes.
 * @return KODE_OK once the codec is free again; KODE_DENIED when no stream is
 *         open; KODE_TIMEOUT when AO_Audio never answered.
 */
kode_status_t kode_block_audio_stream_close(void);

/* ---- Motion: AO_Haptic drives the LRA, AO_Imu reads the QMI8658A and QMC5883P ---- */

/**
 * @brief One IMU sample.
 * @note A resting IMU wakes first, about 200 ms.
 * @note Blocks up to KODE_SYS_IMU_READ_WAIT_MS. Shares one claim with
 *       kode_block_mag_read_raw().
 * @param out Out: written only on KODE_OK.
 * @return KODE_OK with @p out filled. KODE_TIMEOUT when AO_Imu did not answer.
 *         KODE_IO_ERROR when the IMU is absent. KODE_NO_RESOURCES when the event
 *         pool was dry. KODE_DENIED for NULL, a second concurrent caller, or a
 *         production row or witness window holding the part.
 */
kode_status_t kode_block_imu_read_sample(kdrv_qmi8658_sample_t *out);

/**
 * @brief One raw magnetometer sample, milligauss per axis on the QMC5883P's own axes.
 *
 * The board's own field is in every reading (several gauss on the bench, Earth's
 * is 250 to 650 mG), so it is no compass by itself.
 * @note Blocks up to KODE_SYS_MAG_READ_WAIT_MS. Shares one claim with
 *       kode_block_imu_read_sample().
 * @param mg Out: written only on KODE_OK.
 * @return KODE_OK with @p mg filled. KODE_TIMEOUT when AO_Imu did not answer.
 *         KODE_IO_ERROR when the magnetometer is absent. KODE_ERROR when no
 *         conversion was ready or one overflowed (it converts at 50 Hz: ask
 *         again). KODE_NO_RESOURCES when the request could not be made.
 *         KODE_DENIED for NULL, a concurrent caller of either motion read, or
 *         AO_Imu's queue having no room (ask again).
 */
kode_status_t kode_block_mag_read_raw(int32_t mg[KODE_SYS_MAG_AXES]);

/**
 * @brief The magnetometer's calibration, which AO_Imu keeps as the `mag_cal` setting.
 * @note Blocks up to KODE_SYS_MAG_CAL_WAIT_MS. Shares one claim with the two
 *       motion reads.
 * @param corr    Out: written only on KODE_OK with @p present true.
 * @param present Out: whether one is stored.
 * @return KODE_OK. KODE_TIMEOUT when AO_Imu did not answer. KODE_NO_RESOURCES
 *         when the request could not be made. KODE_DENIED for NULL, a concurrent
 *         caller or AO_Imu's queue having no room.
 */
kode_status_t kode_block_mag_cal_get(kdrv_qmc5883p_cal_t *corr, bool *present);

/**
 * @brief Store the magnetometer's calibration.
 * @note As kode_block_mag_cal_get().
 * @param corr Within the KDRV_QMC5883P_CAL limits.
 * @return KODE_OK. KODE_DENIED for NULL, numbers past their limits or a
 *         concurrent caller. KODE_ERROR when the store refused (the old one
 *         stays). Otherwise as kode_block_mag_cal_get().
 */
kode_status_t kode_block_mag_cal_set(const kdrv_qmc5883p_cal_t *corr);

/**
 * @brief Forget the magnetometer's calibration.
 * @note As kode_block_mag_cal_get().
 * @return As kode_block_mag_cal_set(), minus the limits.
 */
kode_status_t kode_block_mag_cal_clear(void);

/* ---- NFC: the ST25R3916B on the Antenna PCB; the field is up only while a call waits ---- */

/**
 * @brief Wait up to @p ms for a tag and copy its identity into @p out.
 * @note Blocks (an AO uses kode_sys_nfc_read_tag()). One NFC call at a time.
 * @param out Out: the tag. Written only on KODE_OK.
 * @param ms  How long to keep the field up looking for one.
 * @return KODE_OK when a tag was found. KODE_TIMEOUT when the window closed empty.
 *         KODE_DENIED when another call waits or no reader is fitted (the field
 *         never came up). KODE_ERROR for a NULL @p out or a zero @p ms.
 */
kode_status_t kode_block_nfc_read_tag(knfc_tag_t *out, uint32_t ms);

/**
 * @brief Wait for a tag and copy its NDEF message into @p out as text (a URI record as its full URI).
 * @note Blocks (an AO uses kode_sys_nfc_ndef_read()). One NFC call at a time.
 * @param out Out: the message, NUL-terminated. Written only on KODE_OK.
 * @param cap How many bytes @p out holds, terminator included.
 * @param n   Out: bytes written, terminator excluded; may be NULL.
 * @param uri Out: true for a URI record, false for Text; may be NULL. Written only on KODE_OK.
 * @param ms  How long to keep the field up.
 * @return KODE_OK. KODE_TIMEOUT when no tag turned up. KODE_DENIED for a busy
 *         facade, a missing reader, or a tag type this cannot read (a Mifare
 *         Classic). KODE_NO_RESOURCES when the message does not fit @p cap, or
 *         is longer than KODE_SYS_NFC_TEXT_MAX - 1 bytes.
 *         KODE_ERROR for a NULL @p out or a zero @p cap or @p ms.
 */
kode_status_t kode_block_nfc_ndef_read(char *out, size_t cap, size_t *n,
                                       bool *uri, uint32_t ms);

/**
 * @brief Wait for a tag and write @p uri to it as an NDEF URI record.
 * @note Blocks. One NFC call at a time.
 * @param uri The link, at most KODE_SYS_NFC_TEXT_MAX - 1 bytes; longer is refused, never cut.
 * @param ms  How long to keep the field up.
 * @return KODE_OK. KODE_TIMEOUT when no tag turned up. KODE_DENIED for a busy
 *         facade, a missing reader, a tag type this cannot write, or a read-only
 *         tag. KODE_NO_RESOURCES when it does not fit the tag (checked before
 *         the first byte goes out). KODE_ERROR for a NULL or too long @p uri, or
 *         a zero @p ms.
 */
kode_status_t kode_block_nfc_ndef_write_uri(const char *uri, uint32_t ms);

/**
 * @brief Wait for a tag and write @p text to it as an NDEF Text record.
 * @note Blocks. One NFC call at a time.
 * @param text The text, at most KODE_SYS_NFC_TEXT_MAX - 1 bytes; longer is refused, never cut.
 * @param ms   How long to keep the field up.
 * @return As kode_block_nfc_ndef_write_uri().
 */
kode_status_t kode_block_nfc_ndef_write_text(const char *text, uint32_t ms);

/**
 * @brief Present the Dot as a passive NFC-A card until kode_block_nfc_emulate_stop().
 *
 * Returns once the window is armed; emulation then runs in the background and
 * every other NFC call is refused KODE_DENIED. Fill @p card as knfc_emulate.h says.
 * @note Blocks for one round trip to AO_Nfc (an AO uses
 *       kode_sys_nfc_emulate_start()).
 * @param card What to present; copied.
 * @return KODE_OK with the window up. KODE_ERROR for a NULL @p card, no reader
 *         fitted, or a window that would not open. KODE_DENIED while another NFC
 *         call holds the reader. KODE_NO_RESOURCES when the message does not
 *         fit. KODE_TIMEOUT when AO_Nfc did not answer.
 */
kode_status_t kode_block_nfc_emulate_start(const knfc_emu_card_t *card);

/**
 * @brief Stop presenting the emulated card.
 * @note Blocks for one round trip to AO_Nfc.
 * @param reads Out: the Type-2 READs served while the window was up (0 means a
 *              reader may have seen the card but never read it); may be NULL.
 * @return KODE_OK when a window was stopped. KODE_DENIED when nothing was being
 *         emulated, or an AO's request started it. KODE_TIMEOUT when AO_Nfc did
 *         not answer.
 */
kode_status_t kode_block_nfc_emulate_stop(uint32_t *reads);

/* ---- RFID (125 kHz LF): the front end on the Antenna PCB; the field is up only while a call waits ---- */

/**
 * @brief Energise for up to @p ms and copy the first EM4100 ID decoded.
 * @note A read never boosts VO4.
 * @note Blocks (an AO uses kode_sys_rfid_read()). One RFID call at a time. A
 *       missing front end is not detected: the read times out.
 * @param id     Out: the 40-bit ID, most-significant nibble first. Written only on KODE_OK.
 * @param rf_div Out: 16, 32 or 64, the bit rate it was read at. Written only on KODE_OK.
 * @param ms     How long to keep the field up looking for a tag.
 * @return KODE_OK with @p id and @p rf_div set. KODE_TIMEOUT when nothing was
 *         decoded, and also when AO_Rfid turned the read away because its front
 *         end was held by a request of its own. KODE_DENIED when another call
 *         waits (nothing posted). KODE_ERROR for a NULL @p id or @p rf_div, or a
 *         zero @p ms.
 */
kode_status_t kode_block_rfid_read(uint8_t id[KODE_SYS_RFID_ID_BYTES], uint8_t *rf_div,
                                   uint32_t ms);

/**
 * @brief Write an ID onto a T5577 and read it back.
 * @note Blocks like kode_block_rfid_read(), with its claim and refusals. @p id
 *       is copied before the post. The write raises VO4 to 3.3 V.
 * @param id The 40-bit ID, most-significant nibble first.
 * @param ms How long to keep the field up.
 * @return KODE_OK when the read-back matched. KODE_TIMEOUT when it did not, or
 *         nothing answered in time. KODE_DENIED when another RFID call waits.
 *         KODE_ERROR for a NULL @p id or a zero @p ms.
 */
kode_status_t kode_block_rfid_write(const uint8_t id[KODE_SYS_RFID_ID_BYTES], uint32_t ms);

/**
 * @brief Play an EM4100 ID as a tag until kode_block_rfid_emulate_stop().
 * @note Blocks for one round trip to AO_Rfid.
 * @param id The ID; copied.
 * @return KODE_OK with the tag playing. KODE_DENIED while the front end is held.
 *         KODE_NO_RESOURCES or KODE_ERROR from the driver; KODE_ERROR for a NULL
 *         @p id. KODE_TIMEOUT when AO_Rfid did not answer.
 */
kode_status_t kode_block_rfid_emulate_start(const uint8_t id[KODE_SYS_RFID_ID_BYTES]);

/**
 * @brief Stop the emulated tag.
 * @note Blocks for one round trip to AO_Rfid.
 * @return KODE_OK when one was stopped. KODE_DENIED when none plays, or an
 *         AO's request started it. KODE_TIMEOUT when AO_Rfid did not answer.
 */
kode_status_t kode_block_rfid_emulate_stop(void);

/* ---- The store: strings kept across resets under keys you choose; plain flash, readable by any console and app, not a vault ---- */

/**
 * @brief Remember @p value under @p key, replacing whatever was there.
 * @note Waits for the flash write; not from a kode_worker job (its stack is in PSRAM).
 * @param key   At most KODE_SYS_NVS_KEY_MAX - 1 characters, not empty.
 * @param value At most KODE_SYS_NVS_VALUE_MAX - 1 characters; the empty string is a value.
 * @return KODE_ERROR when either is NULL or over its ceiling (refused whole, never
 *         shortened). KODE_DENIED from a kode_worker job, with an error line in
 *         the log. Otherwise the store's status.
 */
kode_status_t kode_block_nvs_set(const char *key, const char *value);


/* ---- Files: through AO_Storage; paths are absolute and virtual ("/sdcard/...", "/storage/...")
 * KODE_OK means the question was answered. KODE_DENIED mostly means ask again: a concurrent call, a timed-out
 * request still owed, or AO_Storage busy (`cat`, `app install`, `cp`). A wait is 5 s per round trip: 10 s for
 * the two-round-trip calls, 15 s after a timed-out predecessor. ---- */

/**
 * @brief What @p path is.
 * @note Blocks (an AO uses the kode_sys_file_* requests). One file call at a
 *       time. Nothing is truncated in silence. Never delete a task blocked in a
 *       blocking call here: its claim is lost for the boot.
 * @param path Absolute, virtual: "/sdcard/...", "/storage/..." or "/".
 * @param out  Out: written only on KODE_OK.
 * @return KODE_OK when answered (`out->exists` says whether there was anything).
 *         KODE_DENIED for a malformed path, one naming no volume, or another file
 *         call waiting. KODE_ERROR when the volume will not mount, or for a NULL
 *         @p path or @p out. KODE_NO_RESOURCES when no PSRAM was left for the
 *         staging buffer (first call). KODE_TIMEOUT when AO_Storage did not answer.
 */
kode_status_t kode_block_file_status(const char *path, kode_sys_file_status_t *out);

/**
 * @brief How big the volume at @p path is and how much of it is free.
 * @note Blocks (an AO uses kode_sys_file_space()). One file call at a time.
 *       On a large card the first count can take hundreds of milliseconds.
 * @param path A volume's root, absolute and virtual: "/sdcard" or "/storage".
 * @param out  Out: written only on KODE_OK.
 * @return KODE_OK when counted. KODE_DENIED for a @p path that is not a
 *         volume's own root, or another file call waiting. KODE_ERROR when the
 *         volume will not mount or cannot be counted, or for a NULL @p path or
 *         @p out. KODE_NO_RESOURCES when no PSRAM was left for the staging
 *         buffer (first call). KODE_TIMEOUT when AO_Storage did not answer.
 */
kode_status_t kode_block_file_space(const char *path, kode_sys_file_space_t *out);

/**
 * @brief Called once per entry of a directory listing.
 * @param name   The entry's name, one path segment.
 * @param is_dir Whether it is a directory.
 * @param size   Its size in bytes; meaningless for a directory.
 * @param ctx    As given to the listing.
 * @return false to stop the walk early.
 */
typedef bool (*kode_sys_file_entry_cb)(const char *name, bool is_dir, uint32_t size,
                                       void *ctx);

/**
 * @brief List one directory's own entries, one level.
 *
 * More than KODE_SYS_FILE_LIST_MAX entries: the first ones are all offered to
 * @p cb, then KODE_NO_RESOURCES says that was a page, not the whole.
 * @note Blocks. Two round trips (a STAT, then the list); the claim is held
 *       across the callbacks.
 * @param path Absolute, virtual, and a directory.
 * @param cb   Called once per entry, in AO_Storage's order.
 * @param ctx  Passed to every call of @p cb.
 * @return KODE_OK once every entry has been offered and there were no more.
 *         KODE_NO_RESOURCES when there were more than fitted. KODE_DENIED for a
 *         malformed path, a path that is not there or is a file, or another file
 *         call waiting. KODE_ERROR when the volume will not mount, or for a NULL
 *         @p path or @p cb. KODE_TIMEOUT when AO_Storage did not answer.
 */
kode_status_t kode_block_file_list_each(const char *path, kode_sys_file_entry_cb cb, void *ctx);

/**
 * @brief Make one directory at @p path.
 * @note The parent must already exist.
 * @note Blocks.
 * @param path Absolute, virtual.
 * @return KODE_OK. KODE_DENIED for a malformed path, one naming no volume, or
 *         another file call waiting. KODE_ERROR when the volume will not mount,
 *         the parent is missing, or @p path is NULL. KODE_EXISTS when the name
 *         is taken. KODE_NOT_DIR when a component of the path is a file.
 *         KODE_IO_ERROR when the medium refused the write. KODE_TIMEOUT when
 *         AO_Storage did not answer.
 */
kode_status_t kode_block_file_create_folder(const char *path);

/**
 * @brief Delete a file or an EMPTY directory.
 * @note A STAT picks the verb; a tree is never deleted.
 * @note Blocks. Two round trips.
 * @param path Absolute, virtual.
 * @return KODE_OK. KODE_DENIED for a malformed path, one naming no volume, or
 *         another file call waiting. KODE_ERROR when the volume will not mount,
 *         there is nothing at @p path, it is a directory that is not empty, it
 *         will not go, or @p path is NULL. KODE_TIMEOUT when AO_Storage did not
 *         answer.
 */
kode_status_t kode_block_file_delete(const char *path);

/**
 * @brief Rename or move @p from to @p to, on the same volume only (no copy across volumes).
 * @note Blocks.
 * @param from Absolute, virtual, and something that is there.
 * @param to   Absolute, virtual, on the same volume; its parent must exist.
 * @return KODE_OK. KODE_DENIED for a malformed path, a path naming no volume,
 *         two volumes, or another file call waiting. KODE_ERROR when the volume
 *         will not mount, the rename fails, or @p from or @p to is NULL.
 *         KODE_TIMEOUT when AO_Storage did not answer.
 */
kode_status_t kode_block_file_rename_path(const char *from, const char *to);

/**
 * @brief Read up to @p cap bytes of @p path from @p offset in one round trip, no STAT.
 *
 * @p got below @p cap means the end of the file; an empty file, or an @p offset at or past
 * the end, is KODE_OK with @p got 0.
 * @note Blocks.
 * @param path   Absolute, virtual.
 * @param offset Where in the file the window starts.
 * @param buf    Out: the bytes, written only on KODE_OK.
 * @param cap    How many bytes @p buf holds, at most KODE_SYS_FILE_READ_MAX.
 * @param got    Out: how many were written; may be NULL.
 * @return KODE_OK. KODE_DENIED for a malformed path, another file call
 *         waiting or AO_Storage busy
 *         with a transfer. KODE_ERROR when the
 *         volume will not mount or the read fails, for a missing file, a NULL
 *         @p path or @p buf, a zero @p cap, or a @p cap over the ceiling.
 *         KODE_TIMEOUT when AO_Storage did not answer.
 */
kode_status_t kode_block_file_read_part(const char *path, uint32_t offset, void *buf,
                                        uint32_t cap, uint32_t *got);

/**
 * @brief Put @p data in @p path.
 * @note All @p len bytes land or the call fails.
 *
 * A NUL in @p data is written like any byte. A NULL @p data with @p len 0 is
 * legal: with @p append false it truncates @p path, and creates it if absent.
 * @note Blocks.
 * @param path   Absolute, virtual; the parent directory must exist.
 * @param data   The bytes; may be NULL only when @p len is zero.
 * @param len    How many, at most KODE_SYS_FILE_READ_MAX; a bigger file is written in parts, each with @p append.
 * @param append Keep what is there and write after it; otherwise truncate.
 * @return KODE_OK. KODE_NO_RESOURCES when @p len is over the ceiling. KODE_DENIED
 *         for a malformed path, one naming no volume, another file call waiting,
 *         or AO_Storage busy with a transfer (a file open for reading anywhere
 *         refuses a write). KODE_ERROR when the volume will not mount, the write
 *         does not land, @p path is NULL, or @p data is NULL with a nonzero
 *         @p len. KODE_NOT_DIR when a component of the path is a file. KODE_IS_DIR
 *         when @p path names a directory. KODE_IO_ERROR when the medium refused
 *         the open. KODE_TIMEOUT when AO_Storage did not answer.
 */
kode_status_t kode_block_file_write_buffer(const char *path, const void *data, uint32_t len,
                                           bool append);

/* ---- The network: needs Wi-Fi joined; without CONFIG_KODEOS_NET every call links and answers KODE_DENIED ---- */

/**
 * @brief Resolve @p host to an IPv4 address.
 * @note Blocks.
 * @param host The name.
 * @param ip4  Out: the address in network byte order.
 * @return KODE_OK, or KODE_ERROR when the name does not resolve.
 */
kode_status_t kode_block_net_lookup(const char *host, uint32_t *ip4);

/**
 * @brief Ping @p host @p count times, one echo per second.
 * @note Blocks throughout.
 * @param host  The name or address.
 * @param count 1..10.
 * @param stats Out: the summary; required.
 * @return KODE_OK when the session ran, even if nothing answered (`stats->rx`
 *         says). KODE_ERROR when the name did not resolve or @p stats is NULL.
 */
kode_status_t kode_block_net_ping(const char *host, uint8_t count,
                                  kode_sys_ping_stats_t *stats);

/**
 * @brief GET @p url into @p buf.
 * @note https:// verifies against the Mozilla certificate bundle.
 *
 * A 404 is KODE_OK: the server answered and `out->status` says what. KODE_OK
 * means the transport worked, so "no such page" differs from "no network".
 * @note Blocks.
 * @param url The address.
 * @param buf Out: the first @p cap bytes of the body, raw; may be NULL when only the status is wanted.
 * @param cap How many bytes @p buf holds.
 * @param out Out: status, lengths, and whether the body was longer than @p cap; required.
 * @return KODE_OK when the transport worked; KODE_ERROR for a NULL @p out;
 *         otherwise the transport's status.
 */
kode_status_t kode_block_http_get_buffer(const char *url, void *buf, uint32_t cap,
                                         kode_sys_http_result_t *out);

/**
 * @brief POST @p body to @p url and read the answer into @p buf.
 * @note Blocks.
 * @param url   The target; https:// verifies against the Mozilla bundle.
 * @param body  The request body; NULL only when @p len is 0.
 * @param len   Bytes of @p body.
 * @param ctype The Content-Type header, or NULL for none.
 * @param buf   Out: the first @p cap bytes of the answer; may be NULL.
 * @param cap   How many bytes @p buf holds.
 * @param out   Out: status, lengths, and whether the answer was cut; required.
 * @return KODE_OK once the server answered, whatever its status; KODE_TIMEOUT;
 *         KODE_ERROR for a NULL @p url or @p out, or a failed request;
 *         KODE_DENIED without a network.
 */
kode_status_t kode_block_http_post_buffer(const char *url, const void *body,
                                          uint32_t len, const char *ctype, void *buf,
                                          uint32_t cap, kode_sys_http_result_t *out);

/**
 * @brief Send one UDP datagram to @p host : @p port.
 * @note Blocks on the lookup.
 * @param host The name or address.
 * @param port The destination port.
 * @param data The payload.
 * @param len  Its length, at most 65535.
 * @return KODE_OK once it is on the wire (not a promise it arrived); KODE_ERROR
 *         for a @p len over 65535; otherwise the transport's status.
 */
kode_status_t kode_block_udp_send(const char *host, uint16_t port,
                                  const void *data, uint32_t len);

/* ---- Wi-Fi: a scan, AO_Radio's ---- */

/**
 * @brief One network of a blocking scan.
 * @param ap  The network; valid for the call only.
 * @param ctx The caller's context.
 * @return true for the next one; false to stop.
 */
typedef bool (*kode_sys_wifi_ap_cb)(const kode_sys_wifi_ap_t *ap, void *ctx);

/**
 * @brief Scan for Wi-Fi networks, waiting for the sweep's end.
 *
 * The rows are offered once the sweep is over, from storage of this file's:
 * an abandoned scan's answer still lands there, and the next call waits that
 * answer out (at most @p ms) before it lends the storage again.
 *
 * @note Blocks for the sweep, about 38 s; never from an AO.
 * @param cb  Offered each network the sweep kept, in the driver's order.
 * @param ctx Handed to @p cb.
 * @param ms  How long to wait for the end.
 * @return KODE_OK once every row was offered (none is no failure); KODE_ERROR
 *         for a NULL @p cb, a sweep that failed or a link that went;
 *         KODE_NO_RESOURCES when another scan holds the radio or this file's
 *         storage, an abandoned answer is still owed, AO_Radio had no room or
 *         no PSRAM was left; KODE_TIMEOUT when @p ms ran out; KODE_DENIED
 *         without CONFIG_KODEOS_WIFI.
 */
kode_status_t kode_block_wifi_scan(kode_sys_wifi_ap_cb cb, void *ctx, uint32_t ms);

/* ---- Bluetooth: a scan, AO_Radio's ---- */

/**
 * @brief One device of a blocking listen.
 * @param dev The device; valid for the call only.
 * @param ctx The caller's context.
 * @return true for the next one; false to stop.
 */
typedef bool (*kode_sys_ble_device_cb)(const kode_sys_ble_device_t *dev, void *ctx);

/**
 * @brief Listen for @p ms and offer each device AO_Radio kept.
 *
 * One call at a time, as kode_block_wifi_scan(), with the same claim: an
 * abandoned listen's answer still lands in this file's storage, and the next
 * call waits it out (at most @p wait_ms) before lending the storage again.
 *
 * @note Blocks for the listen; never from an AO.
 * @param cb      Offered each device the listen kept, in the driver's order.
 * @param ctx     Handed to @p cb.
 * @param ms      How long to listen, KODE_SYS_BLE_SCAN_MS_MIN..KODE_SYS_BLE_SCAN_MS_MAX.
 * @param wait_ms How long to wait for the end.
 * @return KODE_OK once every device was offered (none is no failure);
 *         KODE_ERROR for a NULL @p cb, a listen that failed or a link that
 *         went; KODE_DENIED for @p ms out of range, Bluetooth off in Settings
 *         or a build without CONFIG_KODEOS_BLE; KODE_NO_RESOURCES when a scan
 *         holds the radio or this file's storage, an abandoned answer is still
 *         owed, AO_Radio had no room or no PSRAM was left; KODE_TIMEOUT when
 *         @p wait_ms ran out.
 */
kode_status_t kode_block_ble_scan(kode_sys_ble_device_cb cb, void *ctx, uint32_t ms,
                                  uint32_t wait_ms);

/**
 * @brief Set, or set and notify, one value of the app's service, or push a
 *        value on a queued characteristic or empty it, and wait for
 *        AO_Radio's answer.
 *
 * kode_sys_ble_set_value()'s, kode_sys_ble_notify()'s,
 * kode_sys_ble_queue_push()'s or kode_sys_ble_queue_clear()'s request, one
 * call at a time: a call from another task waits for the claim, and every request
 * carries a generation, so the late answer to a call that gave up is dropped
 * by kode_sys_answer_await(). The claim, a full queue and the answer share
 * KODE_SYS_BLE_VALUE_WAIT_MS in all. Nothing is allocated past the first
 * call's semaphore and lock.
 *
 * @note Blocks; never from an AO.
 * @param chr  The characteristic's name.
 * @param op   KQP_BLE_APP_OP_SET, _NOTIFY, _PUSH or _CLEAR.
 * @param data The bytes; NULL when @p len is 0.
 * @param len  0 to KODE_SYS_BLE_VALUE_MAX; 1 or more for a push, 0 for a clear.
 * @return KODE_OK, set and, for a notify, queued or nobody subscribed, or
 *         pushed, or emptied; KODE_DENIED as the commands refuse;
 *         KODE_NO_RESOURCES when the ring is full (the value is set, the
 *         notification is not queued) or, for a push, the queue is;
 *         KODE_ERROR when PSRAM refused the values, or the first call could
 *         not make the door's semaphore and lock; KODE_TIMEOUT when the
 *         claim, room in AO_Radio's queue or the answer did not come in time.
 */
kode_status_t kode_block_ble_value(const char *chr, uint8_t op, const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* KODE_SYS_INTERNAL_H_ */
