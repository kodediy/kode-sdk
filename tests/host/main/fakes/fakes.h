/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fakes.h
 * @brief The control surface of every faked seam.
 */
#ifndef FAKES_H_
#define FAKES_H_

#include <setjmp.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/* The card the SDMMC fake hands out. Included rather than forward-declared:
 * it is a typedef of an anonymous struct, as every khal type is, and it costs
 * nothing - the header is deliberately parseable without ESP-IDF. */
#include "khal_sdmmc.h"
/* The C5 link's job and outcome types, for the same reason: kdrv_radio.h
 * includes nothing but kode_defs.h, and a case that finishes a job names
 * the outcome by its enum rather than by a number. */
#include "kdrv_radio.h"
/* The pairing outcome enum, for the same reason again: a case that ends a
 * pairing names how it ended rather than passing a number. This header is
 * also where CONFIG_KODEOS_BLE_PAIRING_PASSKEY arrives from, via sdkconfig.h. */
#include "kdrv_ble.h"
/* The semaphore handle type, for fake_sem_watch(). */
#include "khal_sync.h"
/* The low-power answer, for fake_lowpower_ready_take(). */
#include "kqp.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Put every fake back to the state a fresh boot would have. */
void fakes_reset(void);

/** @brief Forget every stored record and disarm any forced failure. */
void fake_nvs_reset(void);

/** @brief Make kaos_linked() say the owner whose start is @p start is not in the image. */
void fake_kaos_absent(void (*start)(void));

/** @brief Every owner linked again (kaos_linked()'s fake). */
void fake_kaos_linked_reset(void);

/** @brief No state to clear today; kept so the set stays symmetrical. */
void fake_sync_reset(void);

/** @brief Forget how many times a semaphore has been signalled. */
void fake_sem_reset(void);

/** @brief How many times khal_sem_signal() has been called since the reset. */
unsigned fake_sem_signals(void);

/** @brief How many semaphores khal_sem_create() has made since the binary
 *         started, fake or real; a case reads the difference across a call. */
unsigned fake_sem_creates(void);

/** @brief The @c ms of the most recent khal_sem_wait_timeout() call. */
uint32_t fake_sem_last_wait_ms(void);

/**
 * @brief Run @p hook once, from inside the next khal_sem_wait_timeout().
 *
 * The one thing a single-threaded host cannot otherwise express: what some
 * other task does while this one is asleep. A caller blocked on a semaphore
 * is exactly where a second caller of a non-reentrant function shows up, so
 * this is where a case puts one.
 *
 * Fires once and clears itself, so the hook may call back into the function
 * under test without recursing forever. Cleared by fake_sem_reset() too.
 *
 * Only a wait with a non-zero @c ms runs it: a zero-length poll never yields
 * the core on the target, so nothing else can run inside one.
 *
 * @param hook What to run, or NULL to arm nothing.
 */
void fake_sem_wait_hook(void (*hook)(void));

/**
 * @brief Watch one semaphore: count how many times it is deleted.
 *
 * Works for a real semaphore, whose handle is its own, and for the fake one,
 * whose handle is shared by all. NULL stops watching and forgets the count;
 * fake_sync_reset() does the same.
 *
 * @param s The semaphore to watch.
 */
void fake_sem_watch(khal_sem_t s);

/** @brief How many times khal_sem_delete() was called with the watched semaphore. */
unsigned fake_sem_watch_deletes(void);

/**
 * @brief Make the mutexes and semaphores created from now on real ones.
 *
 * For the code under test that is used by more than one task at once. What
 * was created while this was on stays real for the life of the binary; only
 * creation is affected, and fake_sync_reset() (every case's setUp) turns it
 * off again. A real wait runs fake_sem_wait_hook() for the task that armed
 * it and for no other, and does not record fake_sem_last_wait_ms().
 *
 * @param on true to create real ones.
 */
void fake_sync_real(bool on);

/**
 * @brief Make the mutexes created from now on real ones; semaphores stay fake.
 *
 * Like fake_sync_real(), for a lock that two tasks contend. Turned off again
 * by fake_sync_reset().
 *
 * @param on true to create real mutexes.
 */
void fake_mutex_real(bool on);

/**
 * @brief Run @p hook once, inside the next outermost khal_env_unlock().
 *
 * The task releasing the C library's environment lock runs it while it still
 * holds the lock: a hook that sleeps keeps the lock taken for as long, and
 * whatever another task does meanwhile shows whether that task waits for it.
 * fake_sync_reset() disarms it.
 *
 * @param hook What to run, or NULL to arm nothing.
 */
void fake_env_release_hook(void (*hook)(void));

/** @brief Forget every captured line and re-install the capture sink. */
void fake_klog_reset(void);

/** @brief Line @p idx as klog formatted it, or NULL. @param idx 0-based. */
const char *fake_klog_line(unsigned idx);

/**
 * @brief The tag line @p idx arrived with (KODE_CONSOLE_SRC_*), or
 *        KODE_LOG_SRC_NONE for a log line, a reply sent without one, or an
 *        index with no line.
 * @param idx 0-based.
 */
unsigned char fake_klog_src(unsigned idx);

/** @brief The most recent captured line, or NULL if there is none. */
const char *fake_klog_last(void);

/** @brief Remove the capture sink, arming klog's rescue path. */
void fake_klog_detach(void);

/** @brief Put the port shim back to "a task may yield". */
void fake_port_reset(void);

/**
 * @brief What xPortCanYield() should answer.
 * @param can false to put klog on its ISR-context branch.
 */
void fake_port_set_can_yield(bool can);

/**
 * @brief Forget any armed restart landing point and the restart count,
 *        reseed the entropy source, and disarm fake_psram_refuse().
 *
 * khal_random() is a fixed sequence here rather than a random one: a case
 * that asserts two draws differ has to be able to depend on it, and a suite
 * that fails one run in a few hundred is worse than one that never asks.
 */
void fake_system_reset(void);

/** @brief Put the die temperature back to 41.5 C and KODE_OK. */
void fake_temp_reset(void);

/**
 * @brief Make khal_temp_read() answer @p status, with @p deci_c on KODE_OK.
 * @param status What the read returns.
 * @param deci_c Tenths of a degree Celsius.
 */
void fake_temp_set(kode_status_t status, int32_t deci_c);

/** @brief Force esp_image_verify() to return @p r, an esp_err_t (0 = ESP_OK,
 *         the default). Passed as int to keep this header ESP-IDF-free; reset
 *         to ESP_OK by fake_system_reset(). */
void fake_image_verify_set_result(int r);

/** @brief How many times khal_restart() was reached. */
unsigned fake_system_restarts(void);

/** @brief How many times khal_panic() was reached. */
unsigned fake_system_panics(void);

/**
 * @brief Arm a landing point so a restart, or a panic, can be survived.
 *
 * Use as `if (setjmp(*fake_system_arm_restart()) == 0) { ...drive... }`; the
 * body runs once, and control returns here if khal_restart() OR khal_panic()
 * is reached - both are noreturn, a case only ever expects one of them from
 * a given drive, and fake_system_restarts()/fake_system_panics() say which.
 *
 * @return The buffer to hand to setjmp().
 */
jmp_buf *fake_system_arm_restart(void);

/** @brief Set what khal_heap_alloc_failures() answers; fake_system_reset()
 *         takes it back to 0. */
void fake_heap_set_alloc_failures(uint32_t n);

/** @brief Set what khal_heap_internal_low() answers, in bytes;
 *         fake_system_reset() takes it back to a healthy 96 KB. */
void fake_heap_set_internal_low(size_t bytes);

/** @brief Set what khal_psram_free() answers, in bytes; fake_system_reset()
 *         takes it back to the PSRAM row's reserve plus two blocks. */
void fake_psram_set_free(size_t bytes);

/** @brief Set the largest single block khal_psram_largest() answers, in
 *         bytes, never more than what is free; fake_system_reset() makes
 *         it unlimited again. */
void fake_psram_set_largest(size_t bytes);

/** @brief Forget every khal_alloc_psram()/khal_free() call counted so far. */
void fake_psram_reset(void);

/**
 * @brief Make the next khal_alloc_psram() calls answer NULL.
 *
 * PSRAM refusing is the one failure a host with gigabytes to give cannot
 * produce by itself. Disarmed by fake_system_reset(), so a case that arms
 * more refusals than it uses does not hand them to the next one.
 *
 * @param n How many calls to refuse, counting from the next one.
 */
void fake_psram_refuse(unsigned n);

/**
 * @brief Let @p ok more khal_alloc_psram() calls succeed, then refuse @p n.
 *
 * A refusal partway through a run of allocations, which is what PSRAM that
 * looked free and was not does. Disarmed by fake_system_reset().
 *
 * @param ok How many calls succeed first.
 * @param n  How many are refused after them.
 */
void fake_psram_refuse_after(unsigned ok, unsigned n);

/** @brief How many times khal_alloc_psram() has returned non-NULL. */
unsigned fake_psram_alloc_count(void);

/** @brief How many times khal_free() has been called with a non-NULL
 *         pointer - the mirror's frame buffer is the only PSRAM block a
 *         display_mirror case ever touches, so alloc_count - free_count is
 *         "is a frame buffer currently held". */
unsigned fake_psram_free_count(void);

/**
 * @brief Watch one pointer in the task slots.
 *
 * Counts how many times khal_free() is called with @p p, and lets
 * fake_task_slot_wait_ended() wait for the clean-up of the task whose slot
 * held it. One pointer at a time; NULL stops watching and forgets the count.
 * fake_system_reset() does the same.
 *
 * @param p The pointer a task keeps in its slot.
 */
void fake_task_slot_watch(void *p);

/** @brief How many times khal_free() was called with the watched pointer. */
unsigned fake_task_slot_watch_frees(void);

/**
 * @brief Wait until the slot's owner has been run for the watched pointer.
 *
 * The task's deletion runs it, in whichever task deletes it, some time after
 * the task's function returned: this is how a case knows it is done without
 * guessing at a time.
 *
 * @param ms The longest to wait.
 * @return true when the owner ran in time.
 */
bool fake_task_slot_wait_ended(uint32_t ms);

/** @brief How many times a task gave its slot an owner since the reset. */
uint32_t fake_task_slot_owned(void);

/**
 * @brief Make khal_task_stack_in_psram() answer @p on, for every task, until
 *        the next fakes_reset().
 * @param on true for a stack in PSRAM.
 */
void fake_task_stack_in_psram_set(bool on);

/** @brief Empty the stand-in neighbours' queues. */
void fake_neighbours_reset(void);

/** @brief How many events AO_AppMgr sent AO_Display. */
unsigned fake_neighbour_display_posts(void);

/** @brief How many events reached the stand-in AO_Log. */
unsigned fake_neighbour_log_posts(void);

/** @brief How many events reached AO_IoExp's queue without being dispatched -
 *  the read requests AO_Buttons makes when its debounce wants a second look. */
unsigned fake_neighbour_ioexp_posts(void);

#if CONFIG_KODE_LOWPOWER
/**
 * @brief Take the next answer a participant posted to AO_LowPower.
 *
 * AO_LowPower is not dispatched in a participant's cases; its queue is the
 * record of what the participant said. Anything else queued there is
 * discarded on the way; a wake time is kept for fake_lowpower_wake_take().
 *
 * @param[out] out The answer, copied.
 * @return true when one was waiting.
 */
bool fake_lowpower_ready_take(LowPowerReadyEvt *out);

/**
 * @brief Take the newest wake time a participant gave AO_LowPower.
 *
 * Reading it does not cost the case the answers that came with it, nor the
 * other way round: both kinds are kept until their reader asks.
 *
 * @param[out] out The note, copied.
 * @return true when one was waiting.
 */
bool fake_lowpower_wake_take(LowPowerWakeAtEvt *out);

/**
 * @brief How many radio-done notes the owner under test gave AO_LowPower since the last ask.
 *
 * Reading it clears the count.
 *
 * @return The count.
 */
unsigned fake_lowpower_radio_done_count(void);

/**
 * @brief How many requests to rest (lowpower_request_rest()) reached AO_LowPower since the last ask.
 *
 * Reading it clears the count.
 *
 * @param[out] src The source of the newest, when there was one; may be NULL.
 * @return The count.
 */
unsigned fake_lowpower_rest_count(uint8_t *src);
#endif

/** @brief Put the stand-in inflate back to "one call, all done, no output". */
void fake_tinfl_reset(void);

/**
 * @brief Say what the next inflate calls should do.
 * @param status      What the decoder reports.
 * @param in_consumed Input bytes it swallows per call, capped by what it gets.
 * @param out_written Output bytes it produces per call, capped by the room.
 */
void fake_tinfl_set(int status, size_t in_consumed, size_t out_written);

/** @brief The byte the stand-in inflate writes. @param byte The filler. */
void fake_tinfl_set_fill(uint8_t byte);

/** @brief Bytes of the input the stand-in keeps a copy of. */
#define FAKE_TINFL_IN_KEEP 256U

/**
 * @brief Make the stand-in inflate write exactly these bytes, whatever the input.
 *
 * What a real stream would inflate to, so a caller of the one-call wrapper
 * can be fed a payload it can check exactly. The bytes must outlive the
 * calls; NULL goes back to the filler. It consumes the whole input.
 *
 * @param bytes The payload.
 * @param len   Its size.
 */
void fake_tinfl_set_payload(const uint8_t *bytes, size_t len);

/** @brief Bytes of input the last inflate call was offered. */
size_t fake_tinfl_in_len(void);

/** @brief The first FAKE_TINFL_IN_KEEP bytes of that input. */
const uint8_t *fake_tinfl_in(void);

/** @brief How many times the decoder was asked to inflate. */
unsigned fake_tinfl_calls(void);

/**
 * @brief The flags of the most recent inflate call, 0 when there was none.
 *
 * The flags are how a caller tells the ROM what kind of buffers it holds, and
 * the stand-in obeys none of them, so a wrong one would pass every other
 * assertion here and fail only on a board.
 */
uint32_t fake_tinfl_last_flags(void);

/** @brief Zero the panic counter. */
void fake_panic_reset(void);

/** @brief How many times the fatal printer was reached. */
unsigned fake_panic_count(void);

/** @brief Empty both fake media directories and re-seat the card. */
void fake_media_reset(void);

/** @brief Whether a card is in the slot. @param present false to eject it. */
void fake_media_set_sd_present(bool present);

/** @brief Make kdrv_fs_mount() fail (NULL, the state untouched) or work again.
 *         fake_media_reset() puts it back to working.
 *  @param fails true to refuse every mount of the internal FAT. */
void fake_fs_mount_set_fails(bool fails);

/** @brief How many times kdrv_fs_mount() was called since the last reset. */
unsigned fake_fs_mount_calls(void);

/* ---- The SDMMC seam under kdrv_sd ----------------------------------------
 * kdrv_sd.c is compiled for real, so a case arranges the CARD rather than the
 * driver: what it says it is, how many data lines it will come up on, and
 * whether the far end of it is where the capacity claims. See
 * fake_khal_sdmmc.c; fake_media_reset() calls the reset below for you.
 */

/** @brief Eject the card, empty its sectors and put a healthy one back. */
void fake_sdmmc_reset(void);

/** @brief The directory standing in for the card's mount point. */
const char *fake_sdmmc_dir(void);

/** @brief Whether a card is in the slot. @param present false to eject it. */
void fake_sdmmc_set_present(bool present);

/** @brief A card that answers with no FAT volume. @param no_volume true for one. */
void fake_sdmmc_set_no_volume(bool no_volume);

/** @brief What the next mount reports. @param card The registers to answer. */
void fake_sdmmc_set_card(const khal_sdmmc_card_t *card);

/** @brief The healthy card a reset installs, to be edited and set back. */
void fake_sdmmc_default_card(khal_sdmmc_card_t *out);

/**
 * @brief The widest mount that succeeds.
 * @param width 1 makes a 4-bit mount fail, which is the counterfeit card's
 *        signature and what the driver's fallback exists for.
 */
void fake_sdmmc_set_max_width(uint8_t width);

/**
 * @brief Make the card lie about its capacity.
 * @param real_sectors How big it really is; every access wraps modulo this.
 *        0 turns the lie off.
 */
void fake_sdmmc_set_alias(uint32_t real_sectors);

/**
 * @brief Force the size the filesystem reports.
 * @param total_mb 0 derives it from the card's capacity, as a format would.
 */
void fake_sdmmc_set_total_mb(uint32_t total_mb);

/** @brief Make every raw sector write fail. */
void fake_sdmmc_fail_writes(bool fail);

/**
 * @brief Make every raw sector read fail.
 *
 * What a data line that is not carrying looks like from above: the peripheral
 * checks a CRC per line, so the transfer errors out rather than returning
 * something wrong.
 */
void fake_sdmmc_fail_reads(bool fail);

/**
 * @brief Make writing a FILE to the card fail, while raw access still works.
 *
 * The counterfeit card, exactly: it reads perfectly and refuses to be written
 * to. This is what tells a bad card from a bad slot.
 */
void fake_sdmmc_fail_file_writes(bool fail);

/**
 * @brief Charge @p ms to every raw read, so a rate can be asserted.
 * @param ms Milliseconds one read is made to take. 0 makes reads free.
 */
void fake_sdmmc_set_read_ms(uint32_t ms);

/** @brief Make the format fail, which leaves nothing mounted. */
void fake_sdmmc_fail_format(bool fail);

/**
 * @brief Take the controller away, as a hosted mount finds it while the C5's
 *        link is down: khal_sdmmc_ready() says false and a mount is refused
 *        with KODE_DENIED, trying nothing.
 * @param refuse true to take it away; false to answer as the card does.
 */
void fake_sdmmc_set_refuse(bool refuse);

/**
 * @brief Refuse every mount with KODE_DENIED while khal_sdmmc_ready() still
 *        says true: the controller went away between the two calls.
 * @param deny true to refuse; false to answer as the card does.
 */
void fake_sdmmc_set_deny(bool deny);

#define FAKE_SLOT_TAG_PROTOCOL "sdmmc_common"   /**< ESP-IDF's tag for the card's protocol. */
#define FAKE_SLOT_TAG_MOUNT    "vfs_fat_sdmmc"  /**< ESP-IDF's tag for the FAT mount. */
#define FAKE_SLOT_TAG_PINS     "gpio"           /**< ESP-IDF's tag for the pins the slot takes. */
#define FAKE_SLOT_LINE         "the slot answers nothing"  /**< What an empty slot's lines say. */

/** @brief How many mounts have been attempted since the reset. */
unsigned fake_sdmmc_mounts(void);

/** @brief The data lines the last mount attempt asked for. */
uint8_t fake_sdmmc_last_width(void);

/** @brief The clock ceiling the last mount attempt asked for, in kHz. */
uint32_t fake_sdmmc_last_khz(void);

/** @brief The directory standing in for the microSD mount point. */
const char *fake_media_sd_dir(void);

/** @brief The directory standing in for the internal FAT mount point. */
const char *fake_media_fs_dir(void);

/** @brief What the internal partition reports as free from now on;
 *         fake_media_reset() puts back its 8 MB. */
void fake_media_set_fs_free(uint64_t free_bytes);

/**
 * @brief Does @p name exist under @p dir?
 * @param dir  One of the two directories above.
 * @param name File name within it.
 */
bool fake_media_file_exists(const char *dir, const char *name);

/**
 * @brief Write a file straight into the internal FAT fake's directory.
 * @param name File name, no path components.
 * @param body Contents, written verbatim.
 */
void fake_media_put_fs_file(const char *name, const char *body);

/**
 * @brief Make a directory straight inside the internal FAT fake's directory.
 * @param name Directory name, no path components.
 */
void fake_media_put_fs_dir(const char *name);

/**
 * @brief Write @p body to @p rel under the internal FAT fake, creating each
 *        directory on the way.
 *
 * @p rel may carry path components ("dir/sub/file"); the other put helpers
 * deliberately may not.
 *
 * @param rel  Path within the fake's directory, forward slashes.
 * @param body Contents, written verbatim.
 * @param len  How many bytes of @p body.
 */
void fake_media_put_fs_path(const char *rel, const void *body, size_t len);

/**
 * @brief The same, under the card fake.
 * @param rel  Path within the fake's directory, forward slashes.
 * @param body Contents, written verbatim.
 * @param len  How many bytes of @p body.
 */
void fake_media_put_sd_path(const char *rel, const void *body, size_t len);

/** @brief Put the clock back to zero. */
void fake_time_reset(void);

/**
 * @brief Run @p hook once, from inside the next khal_delay_ms().
 *
 * Stands for whatever another task does while the caller sleeps - a pool
 * block freed, a queue drained. Cleared by fake_time_reset() too.
 *
 * @param hook What to run, or NULL to cancel.
 */
void fake_time_delay_hook(void (*hook)(void));

/** @brief Move the clock forward without sleeping. @param ms Milliseconds. */
void fake_time_advance_ms(uint32_t ms);

/** @brief Forget the stops and starts khal_timer_periodic_run() has counted. */
void fake_time_periodic_forget(void);

/** @brief How many times the periodic timer was told to stop since the last forget. */
unsigned fake_time_periodic_stops(void);

/** @brief How many times the periodic timer was told to run since the last forget. */
unsigned fake_time_periodic_starts(void);

/** @brief Whether the fake's periodic timer is running now. */
bool fake_time_periodic_running(void);

/** @brief How many khal_timer_once() one-shots are armed and not yet fired. */
unsigned fake_time_once_pending(void);

/** @brief The delay, microseconds, of the one-shot armed last. */
uint64_t fake_time_once_last_delay_us(void);

/**
 * @brief Fire the one-shot that is due first, as the timer task would.
 *
 * The clock moves to the moment it was due when that is ahead. The slot is
 * free before the callback runs.
 *
 * @return false when none was armed.
 */
bool fake_time_once_fire(void);

/** @brief Drop every armed one-shot without running it. */
void fake_time_once_forget(void);

/**
 * @brief Set the wall clock the fake reports.
 *
 * @param unix_us Microseconds since the Unix epoch. Zero means "never set",
 *                which is where every case starts.
 */
void fake_time_wall_set(uint64_t unix_us);

/**
 * @brief Pre-load a record so a get finds it, as if it had been written.
 *
 * Seeding is not a write under test - it arranges the world before the case
 * starts - so an armed failure does not swallow it.
 *
 * @param ns   Namespace, as the caller under test will ask for it.
 * @param key  Key, likewise.
 * @param blob Bytes to store; copied.
 * @param len  How many.
 */
void fake_nvs_seed(const char *ns, const char *key,
                   const void *blob, size_t len);

/** @brief Make the next khal_nvs_set_blob() fail once, then behave again. */
void fake_nvs_fail_next_set(void);

/** @brief How many records the fake currently holds. */
unsigned fake_nvs_count(void);

/** @brief How many writes were asked of the fake since fake_nvs_reset(),
 *         refused ones included. */
unsigned fake_nvs_set_count(void);

/** @brief Run @p hook inside every write the fake is asked for (NULL: none);
 *         what another task would see while the store is being written. */
void fake_nvs_set_hook(void (*hook)(void));

/** @brief How many lines reached the capture sink. */
unsigned fake_klog_count(void);

#ifdef __cplusplus
}
#endif


/* ---- The console transport (fake_khal_console.c) --------------------------
 * Captured, not mocked: AO_Log writes one call per echoed keystroke, so a
 * case asserts on the bytes a terminal would have seen instead of on a list
 * of expected calls.
 */

/** @brief Forget everything written, read and armed. */
void fake_console_reset(void);

/** @brief Forget what was written, keeping counters and scripted input. */
void fake_console_clear_out(void);

/** @brief Everything written since the reset, NUL-terminated. */
const char *fake_console_out(void);

/** @brief How many bytes that is. */
size_t fake_console_out_len(void);

/** @brief How many times the transport was opened. */
unsigned fake_console_inits(void);

/** @brief Load bytes for khal_console_read_raw() to hand out. */
void fake_console_feed_raw(const void *data, size_t len);

/** @brief Make the next khal_console_raw_begin() fail. */
void fake_console_raw_refuse(bool refuse);

/** @brief Is raw mode currently open? */
bool fake_console_raw_open(void);

/** @brief Call the armed hook and then the pump, as the reader task would. */
void fake_console_raw_run_pump(void);

/**
 * @brief Call the reader task's captured on_byte() directly, synchronously.
 *
 * Every other case in test_log_ao.c dispatches a ConsoleByteEvt straight
 * into AO_Log, which tests handle_byte() but skips log_on_byte() - the
 * allocation and its retry - entirely. This is the one seam that reaches it,
 * without starting a task: log_start_reader() -> khal_console_start_reader()
 * captures the callback here instead of launching anything, and this
 * function is what a case calls it through.
 */
void fake_console_feed_byte(int c);

/** @brief What khal_console_rest() was told last. */
bool fake_console_resting(void);

/** @brief What khal_watch_rest() was told last. */
bool fake_watch_resting(void);

/** @brief How many times khal_watch_rest() was called. */
unsigned fake_watch_rest_calls(void);

/** @brief Whether the dock's pins are lent (released and not taken back). */
bool fake_console_dock_lent(void);

/** @brief How many times khal_console_dock_release() was called. */
unsigned fake_console_dock_releases(void);

/** @brief How many times khal_console_dock_take() was called. */
unsigned fake_console_dock_takes(void);

/** @brief What the next khal_console_dock_take() answers. */
void fake_console_dock_take_rc(kode_status_t rc);

/* ---- The BLE half of the C5 (fake_kdrv_ble.c) ------------------------------
 * The whole of kdrv_ble.h, because the real file needs NimBLE and esp-hosted
 * and neither builds for `linux`. Two halves. The stack: off until started,
 * synchronised with the controller unless a case says otherwise, a scan
 * table filled from what the case put in the air - what AO_Radio's BLE arms
 * and the production row's BLE step read. The mirror: display_ao.c's two
 * calls, under the control names test_display_mirror.c has always used
 * (display_ao.c alone is compiled with CONFIG_KODEOS_BLE_CONSOLE, scoped in
 * the project's CMakeLists.txt). Both default to the real board's idle
 * answers, so a case that never touches BLE is unaffected.
 */

/** @brief Stack off, synchronised, nothing in the air, no failures armed. */
void fake_ble_reset(void);

/** @brief Whether NimBLE has synchronised with the controller: false makes
 *         kdrv_ble_addr() refuse, the way a controller that never answered
 *         the host would. */
void fake_ble_set_sync(bool synced);

/** @brief Make kdrv_ble_start() fail - a controller the C5 would not bring up. */
void fake_ble_fail_start(bool fail);

/** @brief Make kdrv_ble_scan() fail. */
void fake_ble_fail_scan(bool fail);

/** @brief Put @p n advertisers in the air, the strongest at @p top_rssi and
 *         the rest 6 dB apart below it; the next scan reports them. The
 *         first carries a random address, FAKE_BLE_MFG_* as its maker data
 *         and FAKE_BLE_SVC16 as its one service; the rest carry none. */
void fake_ble_seed_air(uint8_t n, int8_t top_rssi);

#define FAKE_BLE_MFG_LEN    3U      /**< The first advertiser's maker bytes. */
#define FAKE_BLE_MFG_ID_LO  0x4CU   /**< Apple's company id, low byte.      */
#define FAKE_BLE_MFG_ID_HI  0x00U   /**< And its high byte.                  */
#define FAKE_BLE_MFG_TYPE   0x12U   /**< Find My's advert type.              */
#define FAKE_BLE_SVC16      0xFD5AU /**< Samsung's SmartTag service.         */

/** @brief A central arrives (@p connected) or leaves, the way NimBLE's host
 *         task reports it: the state changes, then the installed callback
 *         hears it. */
void fake_ble_central(bool connected);

/** @brief The interval the last kdrv_ble_adv_plan() gave. */
uint16_t fake_ble_adv_itvl(void);

/** @brief The time on the air the last kdrv_ble_adv_plan() gave, ms. */
uint32_t fake_ble_adv_ms(void);

/** @brief Whether the last advert planned hides the board. */
bool fake_ble_adv_hidden(void);

/** @brief Set the count of bonded peers kdrv_ble_bonds() reports. */
void fake_ble_set_bonds(uint8_t n);

/** @brief How many times kdrv_ble_conn_calm() was asked for the rest set
 *         (@p rest) or the awake one since the reset. */
unsigned fake_ble_conn_calls(bool rest);

/** @brief What kdrv_ble_link_secure() answers. */
void fake_ble_secure(bool secure);

/** @brief The advert's time ran out: IDLE, told as the real one tells it. */
void fake_ble_adv_end(void);

/** @brief How many times kdrv_ble_start() has been called since the reset. */
unsigned fake_ble_starts(void);

/** @brief Declare the app's service kdrv_ble_app_service() finds, NULL for
 *         none; it stays across fake_ble_reset(), so a case that sets one
 *         puts NULL back. fake_ble_central(true) counts a link
 *         (kdrv_ble_links()). */
void fake_ble_app_service_set(const kdrv_ble_app_service_t *svc);

/** @brief How many times kdrv_ble_app_kick() was called since boot. */
unsigned fake_ble_app_kicks(void);

/** @brief Declare the Battery service kdrv_ble_battery_declared() reports, and
 *         forget the level; it stays across fake_ble_reset(), so a case that
 *         declares it puts false back. */
void fake_ble_battery_declare(bool on);

/** @brief The last level kdrv_ble_battery_set() was given; -1 for none since the declare. */
int fake_ble_battery_pct(void);

/** @brief How many scans have been run since the reset. */
unsigned fake_ble_scans(void);

/** @brief The listen window the last scan asked for, in ms. */
uint32_t fake_ble_last_scan_ms(void);


/** @brief Detach, and let every write succeed again. */
void fake_ble_mirror_reset(void);

/** @brief Whether kdrv_ble_mirror_attached() reports a central subscribed. */
void fake_ble_mirror_set_attached(bool attached);

/** @brief Every kdrv_ble_mirror_write() from here answers KODE_DENIED - the
 *         unretryable "nobody is subscribed any more" a dropped central
 *         leaves behind (kdrv_ble.h's own contract). */
void fake_ble_mirror_deny_writes(void);

/** @brief Every kdrv_ble_mirror_write() from here answers KODE_NO_RESOURCES -
 *         the retryable "ring is full", as a permanently congested link
 *         would look forever. */
void fake_ble_mirror_congest_writes(void);

/** @brief How many times kdrv_ble_mirror_write() has been called since the
 *         reset - proof a "the session already ended" assertion is not
 *         passing for having never been exercised. */
unsigned fake_ble_mirror_write_calls(void);

#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
/** @brief Forget any pending pairing record and the rejection count. */
void fake_ble_pair_reset(void);

/** @brief Play the host task's part: a code is on offer. Nudges the callback,
 *         exactly as the GAP handler's PASSKEY_ACTION case does. */
void fake_ble_pair_show(uint32_t passkey);

/** @brief Play the host task's part: the pairing ended this way. */
void fake_ble_pair_end(kdrv_ble_pair_outcome_t how);

/** @brief How many times kdrv_ble_pair_reject() was called and had something
 *         to refuse - a refusal with nothing in flight does not count, which
 *         is what makes "the request reached the driver" assertable. */
unsigned fake_ble_pair_rejects(void);
#endif

/* ---- The C5 link's driver (fake_kdrv_radio.c) -----------------------------
 * radio_ao.c is compiled for real; kdrv_radio.c is not, because it is the
 * SDIO transport. The fake driver records the job whose body ran
 * (kdrv_radio_run() is called by the fake worker when a case pulls the job)
 * and answers KODE_OK; the case holds the done event back and delivers it
 * with the status it wants, so that the outcome arrives as RADIO_JOB_SIG the
 * way it does on the board.
 */

/** @brief Forget the last job and every forced answer; keep the link
 *         notification. */
void fake_radio_reset(void);

/** @brief How many kdrv_radio_c5_hold(true) calls since fake_radio_reset(). */
unsigned fake_radio_c5_holds(void);

/** @brief Forget the notification too, as a power cycle does: what a case
 *         that runs AO_Radio's initial transition again needs first, because
 *         the notification is installed there. */
void fake_radio_power_cycle(void);

/** @brief The job whose body ran last, arguments included. */
const kdrv_radio_req_t *fake_radio_ran(void);

/** @brief How many jobs' bodies have run since the reset. */
unsigned fake_radio_runs(void);

/** @brief What kdrv_radio_link_up() answers. Ending a CONNECT sets it. */
void fake_radio_set_link(bool up);

#if CONFIG_KODE_LOWPOWER
/** @brief How many times the sleep sentinel was started since the reset. */
unsigned fake_radio_sleep_starts(void);

/** @brief What the next sentinel start answers; KODE_OK (0) after the reset. */
void fake_radio_sleep_fail(kode_status_t rc);

/** @brief How many times it was stopped. */
unsigned fake_radio_sleep_stops(void);

/** @brief Whether the last call to kdrv_radio_sleep() was a start. */
bool fake_radio_sleep_armed(void);

/**
 * @brief Play the sentinel's task: report a wake to the note the AO gave.
 *
 * @param kind   KDRV_RADIO_WOKE_*.
 * @param knocks Attempts the wake "needed".
 */
void fake_radio_sleep_note(uint8_t kind, uint16_t knocks);

/**
 * @brief Set what kdrv_radio_sleep_stats() answers, whatever the sentinel did.
 *
 * @param stats The counters to report; NULL puts them back to zero.
 */
void fake_radio_set_sleep_stats(const kdrv_radio_sleep_stats_t *stats);
#endif

/** @brief Whether the C5 is being held in reset, set without AO_Radio: what
 *         a console case stages before it asks the board what it is. */
void fake_radio_set_c5_held(bool held);

/** @brief The target of the last kdrv_radio_note_leave(), 0 when none was
 *         left since the reset. */
uint32_t fake_radio_note_last(void);

/** @brief What kdrv_radio_adopted() answers. */
void fake_radio_set_adopted(bool adopted);

/** @brief Whether a CONNECT that succeeds leaves J3's supply unswitched
 *         (kdrv_radio_exp_switched() false); fake_radio_reset() clears it. */
void fake_radio_fail_exp_switch(bool fails);

/** @brief The level the C5 holds J3's supply enable at: a CONNECT resets the
 *         C5, which lets go of it (on), and a switch drives it. */
bool fake_radio_exp_pin(void);

/** @brief How many times the enable was driven low since fake_radio_reset(). */
unsigned fake_radio_exp_off_drives(void);

/** @brief The running CONNECT reads the wish now, as its worker does just
 *         before it drives the pin: a wish asked after this is not the one
 *         the connect drives. */
void fake_radio_connect_read(void);

/** @brief What kdrv_radio_fwversion() answers, and whether it answers. */
void fake_radio_set_fw(uint32_t major, uint32_t minor, uint32_t patch,
                       bool known);

/**
 * @brief What the real driver does to the link on its way out of @p job.
 *
 * A CONNECT that succeeds also raises the link, and a DISCONNECT lowers it
 * and forgets the version.
 *
 * @param job The job that ended.
 * @param rc  What it came to.
 */
void fake_radio_effects(kdrv_radio_job_t job, kode_status_t rc);

/** @brief The link is gone: the notification AO_Radio installed is told
 *         @p why (KODE_ERROR for a dropped transport, KODE_TIMEOUT for a C5
 *         that stopped answering), and the link reads down. */
void fake_radio_lose_link(kode_status_t why);

/** @brief What kdrv_radio_probe_result() answers; fake_radio_reset() clears it. */
void fake_radio_set_probe_result(const kdrv_radio_probe_result_t *res);

/** @brief The transport died on its own: fake_radio_lose_link(KODE_ERROR). */
void fake_radio_drop_link(void);

/* ---- Peer data (kdrv_radio_peer_*) --------------------------------------- */

/** @brief Forget the last message sent. Listeners stay, as on the board. */
void fake_peer_reset(void);

/** @brief The last message sent, and its length when len is not NULL. */
const uint8_t *fake_peer_last_sent(size_t *len);

/** @brief The first 16 bytes of an earlier message: 0 = the last one sent,
 *         1 = the one before it, up to 3. NULL when there was none. */
const uint8_t *fake_peer_sent_back(unsigned back);

/** @brief The msg id the last message went out under. */
uint32_t fake_peer_last_msg_id(void);

/** @brief The C5 sends a message: every listener for msg_id hears it. */
void fake_peer_deliver(uint32_t msg_id, const uint8_t *data, size_t len);

/* ---- esp-hosted's host API (fake_esp_hosted.c) -----------------------------
 * kdrv_c5.c is compiled for real over these. The reset is a healthy Kode
 * slave: an ESP32-C5 on the host's own esp-hosted version, project kode_c5,
 * four SDIO lines at 40 MHz, the card powered. Every setter moves one fact.
 */

/** @brief Put the healthy Kode slave back. */
void fake_hosted_reset(void);

/** @brief What the chip-info RPC answers. @param id esp_chip_model_t. */
void fake_hosted_set_chip(uint32_t id, const char *target);

/** @brief The esp-hosted version the slave reports. */
void fake_hosted_set_fw(uint32_t major, uint32_t minor, uint32_t patch);

/** @brief The project name in the slave's app descriptor. */
void fake_hosted_set_project(const char *name);

/** @brief The other strings of the descriptor. */
void fake_hosted_set_app_strings(const char *version, const char *idf_ver,
                                 const char *date, const char *time);

/** @brief Make the app-descriptor RPC answer ESP_ERR_NOT_SUPPORTED - an
 *         older slave that does not have it. */
void fake_hosted_app_desc_unsupported(bool unsupported);

/** @brief The slave's station MAC. */
void fake_hosted_set_mac(const uint8_t mac[6]);

/** @brief What the SDIO transport reports it negotiated. */
void fake_hosted_set_sdio(uint32_t khz, uint8_t width);

/** @brief The level a C5 pin reads. @param pin The C5's GPIO number. */
void fake_hosted_set_level(uint32_t pin, int level);

/** @brief The level a C5 pin holds now, or -1 for a pin that is not one. */
int fake_hosted_level(uint32_t pin);

/** @brief Make every RPC GPIO call fail - the expander service absent. */
void fake_hosted_gpio_dead(bool dead);

/** @brief The slave's heap, as the one-shot memory monitor reports it. */
void fake_hosted_set_heap(uint32_t free_bytes, uint32_t largest_bytes);

/** @brief Make every call fail with ESP_ERR_INVALID_STATE - no link. */
void fake_hosted_link_down(bool down);

/** @brief How many times a pin was configured since the reset. */
unsigned fake_hosted_gpio_configs(void);

/** @brief The mode the last configuration asked for (H_CP_GPIO_MODE_*). */
uint32_t fake_hosted_last_gpio_mode(void);

/** @brief How many times the memory monitor was queried. */
unsigned fake_hosted_mem_queries(void);

#endif /* FAKES_H_ */
