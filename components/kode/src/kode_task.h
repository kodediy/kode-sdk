/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_task.h
 * @brief What the standard layer keeps for each task that uses it. Private, never installed.
 */
#ifndef KODE_TASK_H_
#define KODE_TASK_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode.h"
#include "kode_entry.h"
#include "khal_sync.h"

struct kode_file_state;
struct kode_http_state;
struct kode_setting_state;
struct kode_tag_state;

/** @brief One event with its strings: a ring entry, and a waiter's copy. */
typedef struct {
    uint32_t seq;                           /**< Its number: the push that made it. */
    char    name[KODE_EVENT_NAME_MAX];      /**< The event's name.   */
    char    source[KODE_EVENT_SOURCE_MAX];  /**< Who sent it.        */
    char    text[KODE_EVENT_TEXT_MAX];      /**< Its payload.        */
    int32_t value;                          /**< Its number.         */
    int16_t x;                              /**< Its point's x; 0 for an event that is not the finger. */
    int16_t y;                              /**< Its point's y; 0 for an event that is not the finger. */
} kode_event_copy_t;

/* Its task's alone, except while it waits: the dispatch task then fills hit and got under
 * the events lock, and the task reads them after. */
/** @brief The standard layer's per-task state. */
typedef struct kode_task {
    char               error[KODE_ERROR_MAX];   /**< Text of the last failure; "" if none.      */
    khal_sem_t         wait_sem;                /**< Signalled when the awaited event arrives;
                                                 *   created on the first wait.                */
    bool               waiting;                 /**< On the list of waiting tasks.              */
    bool               hit;                     /**< The event arrived; @c got is whole.        */
    char               want[KODE_EVENT_NAME_MAX];   /**< The event it waits for, in lower case. */
    uint32_t           start;                   /**< The events' counter when it began; only later entries serve it. */
    kode_event_copy_t  got;                     /**< The last event it waited for.              */
    struct kode_task  *next;                    /**< The next waiting task; events lock.        */
    struct kode_file_state *file;               /**< The files state (kode_file.h); NULL until the
                                                 *   task's first file call.                    */
    struct kode_http_state *http;               /**< The HTTP state (kode_http.h); NULL until the
                                                 *   task's first request.                      */
    struct kode_setting_state *setting;         /**< The settings state; NULL until the task's
                                                 *   first kode_setting_get().                  */
    struct kode_tag_state  *tag;                /**< The infrared and tag state (kode_tag.h); NULL
                                                 *   until the task's first raw infrared read.  */
} kode_task_t;

/**
 * @brief The calling task's state, if it has one.
 *
 * @return The struct; NULL if the task never needed one.
 */
kode_task_t *kode_task_get(void);

/**
 * @brief The calling task's state, taken from PSRAM on the first need.
 *
 * @return The struct, zeroed on its first call; NULL if PSRAM has none to give.
 */
kode_task_t *kode_task_ensure(void);

#endif /* KODE_TASK_H_ */
