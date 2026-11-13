/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_system.h
 * @brief Restart, panic, random numbers, memory and tasks.
 */
#ifndef KHAL_SYSTEM_H_
#define KHAL_SYSTEM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Reset the chip. Does not return. */
void khal_restart(void) __attribute__((noreturn));

/**
 * @brief Stop the chip through the panic handler, leaving a core dump.
 * @param why One line, into the panic log and the core dump.
 */
void khal_panic(const char *why) __attribute__((noreturn));

/**
 * @brief Thirty-two bits from the hardware random number generator.
 * @return A fresh value, different across boards and across resets.
 */
uint32_t khal_random(void);

/**
 * @brief Print through the ROM's printf, taking no lock: for a QP assert and an ISR.
 * @param fmt Only %s, %d, %u, %x and %c; no floats, no widths.
 */
void khal_panic_print(const char *fmt, ...);

/**
 * @brief Free bytes in the general heap, mostly PSRAM.
 * @return Bytes.
 */
size_t khal_heap_free(void);

/**
 * @brief Free bytes in PSRAM alone; khal_heap_free() also counts internal RAM.
 * @return Bytes.
 */
size_t khal_psram_free(void);

/**
 * @brief The largest single block PSRAM can still give.
 *
 * Ask before a big allocation that may not fit: a refusal is counted as a
 * failure (`alloc_fail`), and this is not.
 *
 * @return Bytes.
 */
size_t khal_psram_largest(void);

/**
 * @brief Free bytes in the internal, DMA-capable pool.
 * @return Bytes.
 */
size_t khal_heap_internal_free(void);

/**
 * @brief Largest single block the internal pool can still give.
 * @return Bytes.
 */
size_t khal_heap_internal_largest(void);

/**
 * @brief The lowest the internal pool has been since boot.
 * @return Bytes, the sum of each internal region's own low mark.
 */
size_t khal_heap_internal_low(void);

/** @brief Count every allocation the heap refuses, and print the first few. Call once, early. */
void khal_heap_watch(void);

/**
 * @brief Allocations the heap has refused since khal_heap_watch().
 * @return The count; 0 on a healthy board.
 */
uint32_t khal_heap_alloc_failures(void);

#define KHAL_TASK_NAME_LEN  16U   /**< Bytes of a task name, NUL included. */

/** @brief One live task, as the scheduler knows it. */
typedef struct {
    char     name[KHAL_TASK_NAME_LEN];  /**< As it was created with. */
    uint8_t  prio;                      /**< Current priority. */
    uint32_t stack_free;                /**< Bytes of its stack never used. */
} khal_task_info_t;

/**
 * @brief Snapshot the tasks the scheduler runs.
 * @param out   Filled with at most @p max entries, in no particular order.
 * @param max   Room in @p out.
 * @param total If not NULL, receives how many tasks the scheduler runs;
 *              0 when the scheduler was not built to be asked.
 * @return How many entries were written to @p out.
 */
size_t khal_task_list(khal_task_info_t *out, size_t max, size_t *total);

/**
 * @brief The least stack the calling task has had free since it started.
 * @return Bytes; 0 before the scheduler runs.
 */
uint32_t khal_task_stack_free(void);

/**
 * @brief Change the calling task's priority.
 * @param priority The FreeRTOS priority to run at from here on.
 * @return The priority the task had, to give it back with.
 */
uint32_t khal_task_priority_set(uint32_t priority);

/**
 * @brief The name of the running task. Safe from a critical section.
 * @return The name, or NULL before the scheduler runs.
 */
const char *khal_task_name(void);

/**
 * @brief Whether the calling task's stack is in PSRAM.
 * @return true when it is; false on the host unless a test says otherwise.
 */
bool khal_task_stack_in_psram(void);

/** @brief What a task started by khal_task_start() runs; it may return, and the task then ends. */
typedef void (*khal_task_fn_t)(void *arg);

/**
 * @brief Start a task whose stack is in PSRAM, on either core.
 * @param fn          Runs in the new task with @p arg; when it returns the task is deleted.
 * @param name        The task's name, as `tasks` prints it; copied.
 * @param stack_bytes Bytes of stack, taken from PSRAM.
 * @param prio        FreeRTOS priority.
 * @param arg         Handed to @p fn.
 * @return KODE_OK once the task exists; KODE_DENIED for a NULL @p fn or
 *         @p name, or a zero stack; KODE_NO_RESOURCES when no stack or
 *         control block could be had.
 */
kode_status_t khal_task_start(khal_task_fn_t fn, const char *name,
                              uint32_t stack_bytes, uint32_t prio, void *arg);

/**
 * @brief An opaque handle of the calling task; two tasks never share one.
 * @return The handle, or NULL before the scheduler runs.
 */
void *khal_task_self(void);

#define KHAL_TASK_SLOT_INDEX 1U   /**< FreeRTOS TLS index of the per-task pointer; index 0 is the pthread layer's. */

/**
 * @brief The pointer the calling task keeps in its own slot.
 * @return What this task last passed to khal_task_slot_set(); NULL if none.
 */
void *khal_task_slot_get(void);

/**
 * @brief Keep one pointer in the calling task, where no other task can see it.
 * @param p The pointer; the slot does not own what it points at.
 */
void khal_task_slot_set(void *p);

/** @brief What runs when a task that owns its slot ends. */
typedef void (*khal_task_slot_free_t)(void *p);

/**
 * @brief Keep one pointer in the calling task, and free it when the task ends.
 *
 * As khal_task_slot_set(), and @p free_fn is called with @p p once the task is
 * deleted, whether its function returned or another task deleted it. It runs
 * in the task that deletes this one or in the idle task, never in this one:
 * it must not block, take a lock or wait for anything. It is not called when
 * @p p is NULL.
 *
 * One function serves every task: the slot has a single owner, and the
 * function of the latest call is the one that runs.
 *
 * @param p       The pointer; the slot owns what it points at.
 * @param free_fn Frees @p p and whatever hangs off it; NULL keeps the slot
 *                without an owner.
 */
void khal_task_slot_set_owned(void *p, khal_task_slot_free_t free_fn);

/**
 * @brief Start the one supervisor task, watched by the task watchdog.
 *
 * Creates a task named "kqp.watch" at the highest application priority, with
 * a static stack in internal RAM, and subscribes it to the task watchdog,
 * which is reconfigured to keep its configured timeout and idle-task checks
 * and to panic when it fires. The task sleeps @p period_ms, runs @p pass,
 * feeds the watchdog, and repeats: if it stops running for the watchdog's
 * timeout, the board panics naming it.
 *
 * @param pass      Called in the task once a period; must return.
 * @param period_ms Sleep between passes, in ms; at least one scheduler tick.
 * @return KODE_OK once the task runs; KODE_DENIED on a second call, a NULL
 *         pass, a period shorter than a tick, or a build with no task
 *         watchdog; KODE_ERROR when the watchdog or the task refuses.
 */
kode_status_t khal_watch_start(void (*pass)(void), uint32_t period_ms);

/**
 * @brief Tell the supervisor task the board is asleep, or that it is not any more.
 *
 * Asleep the pass has nothing to judge (the QP tick is stopped and every Active
 * Object stands still on purpose), but the task still woke the P4 once a period
 * to find that out. At its next wake after @p rest is true it leaves the task
 * watchdog and waits, with no timeout, until it is told false; then it joins the
 * watchdog again and goes back to a pass a period. While it waits nothing
 * watches the Active Objects, which the pass would not do anyway.
 *
 * @param rest true while the board sleeps. Safe from any task; does nothing
 *             before khal_watch_start() or in a build with no task watchdog.
 */
void khal_watch_rest(bool rest);

/** @brief What the chip says about itself, for the boot banner. */
typedef struct {
    char const *model;       /**< The chip's name, such as "ESP32-P4"; static. */
    uint8_t     rev_major;   /**< Silicon revision, major.                     */
    uint8_t     rev_minor;   /**< Silicon revision, minor.                     */
    uint32_t    flash_bytes; /**< The flash chip's size, as the chip reports it. */
    uint32_t    psram_bytes; /**< The PSRAM's size; 0 when there is none.      */
} khal_chip_info_t;

/**
 * @brief Read the chip's model, revision, flash size and PSRAM size.
 * @param out Filled on KODE_OK.
 * @return KODE_OK; KODE_DENIED for a NULL @p out; KODE_ERROR when the flash
 *         size cannot be read (the other fields are still filled).
 */
kode_status_t khal_chip_info(khal_chip_info_t *out);

/**
 * @brief Allocate from PSRAM, zeroed and aligned. Never falls back to internal RAM.
 * @param align Alignment in bytes, a power of two.
 * @param size  Bytes wanted.
 * @return The block, or NULL if PSRAM cannot satisfy it.
 */
void *khal_alloc_psram(size_t align, size_t size);

/**
 * @brief Resize a block, keeping it in PSRAM, with realloc()'s semantics.
 *
 * NULL @p p allocates, @p size 0 frees, a failed grow leaves @p p untouched,
 * and the new bytes are not zeroed.
 *
 * @param p    Block from khal_alloc_psram() or khal_realloc_psram(), or NULL.
 * @param size Bytes wanted.
 * @return The block, or NULL if PSRAM cannot satisfy it.
 */
void *khal_realloc_psram(void *p, size_t size);

/**
 * @brief Release a block from khal_alloc_psram() or khal_realloc_psram().
 * @param p The block; NULL does nothing.
 */
void khal_free(void *p);

#ifdef __cplusplus
}
#endif

#endif /* KHAL_SYSTEM_H_ */
