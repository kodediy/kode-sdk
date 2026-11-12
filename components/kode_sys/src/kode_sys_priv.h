/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_priv.h
 * @brief The command post, the claim and wait machinery the door files share,
 *        and kode_init()'s rows and the set-up it asks of them; private to this component.
 */
#ifndef KODE_SYS_PRIV_H_
#define KODE_SYS_PRIV_H_

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "khal_once.h"
#include "sdkconfig.h"
#include "khal_sync.h"
#include "kode_sys.h"
#include "kqp.h"

/** @brief Milliseconds in a second. */
#define KODE_SYS_MS_PER_S 1000U

/**
 * @brief Post a command to its owner, leaving the owner's command margin free.
 * @param owner  The device's owner.
 * @param e      The command's event; NULL when the pool was dry.
 * @param margin The owner's KQP_<OWNER>_CMD_MARGIN (kqp_config.h).
 * @return KODE_OK when the owner's queue took it; KODE_NO_RESOURCES for a NULL
 *         @p e or a queue within the margin, and QP gives the event back to its pool.
 */
static inline kode_status_t kode_sys_cmd_post(QActive *owner, QEvt const *e,
                                              uint_fast16_t margin)
{
    kode_status_t rc = KODE_NO_RESOURCES;

    if ( NULL != e )
    {
        if ( QACTIVE_POST_X(owner, e, margin, NULL) )
        {
            rc = KODE_OK;
        }
    }
    return rc;
}

/** @brief The one-shot kode_init() runs its bring-up behind; a host case arms it with a body of its own. */
extern khal_once_t kode_sys_init_once;

/** @brief One step of the bring-up: a start, a doors' set-up or a park. */
typedef void (*kode_sys_step_t)(void);

/** @brief One module in kode_init()'s order. */
typedef struct {
    kode_sys_step_t start; /**< Its start or its doors' set-up; NULL when the app did not link it. */
    kode_sys_step_t park;  /**< What puts its part to sleep while it is absent; NULL for none. */
} kode_sys_row_t;

#if CONFIG_KODEOS_NFC
/** @brief Rows in kode_init()'s order, AO_Boot to AO_Display (AO_Log starts before the banner). */
#define KODE_SYS_ROWS 29U
#else
/** @brief Rows in kode_init()'s order, less the NFC doors' set-up and AO_Nfc, whose reader is the app's. */
#define KODE_SYS_ROWS 27U
#endif

/** @brief kode_init()'s order: the one place it is written. */
extern const kode_sys_row_t kode_sys_rows[KODE_SYS_ROWS];

/**
 * @brief Start every linked row in order, then park every absent one.
 *
 * A park waits for the last start, so the core it may ask (AO_IoExp) is up.
 * The calling task runs the walk at KODE_PRIO_BOOT_WALK and has the priority
 * it came with back when the walk ends.
 * @param rows The rows, in order.
 * @param n    How many.
 */
void kode_sys_rows_walk(const kode_sys_row_t *rows, size_t n);

/** @brief Create the infrared doors' semaphores and lock; kode_init() calls it before AO_Ir starts. */
void kode_sys_ir_init(void);
/** @brief Create the NFC doors' semaphore and lock; kode_init() calls it before AO_Nfc starts. */
void kode_sys_nfc_init(void);
/** @brief Create the audio doors' semaphores and lock; kode_init() calls it before AO_Audio starts. */
void kode_sys_audio_init(void);
/** @brief Create the motion reads' semaphore and lock; kode_init() calls it before AO_Imu starts. */
void kode_sys_imu_init(void);
/** @brief Create the RFID doors' semaphore and lock; kode_init() calls it before AO_Rfid starts. */
void kode_sys_rfid_init(void);
/** @brief Create the file doors' semaphore and lock, and point AO_Storage's answer at them; before AO_Storage is asked. */
void kode_sys_files_init(void);

/** @brief The semaphore AO_Nfc signals when a request ends. */
extern khal_sem_t kode_sys_nfc_sem;
/** @brief Whose answer the last NFC signal carried; never lent to a caller. */
extern uint32_t   kode_sys_nfc_ack;
/** @brief The kode_status_t of the NFC exchange or carrier in flight. */
extern uint32_t   kode_sys_nfc_rc;
/** @brief The semaphore AO_Rfid signals when a request ends. */
extern khal_sem_t kode_sys_rfid_sem;
/** @brief Whose answer the last RFID signal carried. */
extern uint32_t   kode_sys_rfid_ack;

/** @brief The highest certification carrier mode both fields take: 0 off, 1 bare, 2 modulated. */
#define KODE_CERT_MODE_MAX  2

/**
 * @brief Whether @p mode names a certification carrier mode.
 * @param mode The mode asked for.
 * @return true for 0..KODE_CERT_MODE_MAX.
 */
static inline bool kode_cert_mode_ok(int32_t mode)
{
    return (0 <= mode) && (KODE_CERT_MODE_MAX >= mode);
}

/**
 * @brief Whether @p text is a string a request carries whole.
 * @param text A string, or NULL.
 * @param cap  Bytes allowed, NUL included.
 * @return true when it is not NULL, not empty and shorter than @p cap.
 */
bool kode_sys_text_fits(const char *text, size_t cap);

/**
 * @brief Whether the generation an AO last stamped into @p ack is @p gen (an acquire load).
 * @param ack Where the answering Active Object stamps its generation.
 * @param gen The generation asked about.
 * @return true when @p ack holds @p gen.
 */
bool kode_sys_answer_is(const uint32_t *ack, uint32_t gen);

/**
 * @brief Sleep until the answer stamped @p gen arrives, discarding others.
 * @param sem The semaphore to sleep on.
 * @param ack Where the answering Active Object stamps the generation it answered.
 * @param gen This caller's own generation.
 * @param ms  How long to wait, in total, across every token discarded.
 * @return true when this caller's own answer arrived, and only then.
 */
bool kode_sys_answer_await(khal_sem_t sem, const uint32_t *ack, uint32_t gen,
                           uint32_t ms);

/**
 * @brief One blocking scan's claim and storage, AO_Radio's waiter included.
 *
 * The generation guards the answer; @c stale guards the storage. A call that
 * times out leaves AO_Radio holding pointers into @c wait and its rows, so
 * kode_sys_scan_await() latches the generation and kode_sys_scan_enter()
 * makes the next caller wait that generation out before lending them again.
 * The semaphore, the lock and the rows are made on the first call, so a
 * firmware that never scans pays nothing. Set @c row_size and @c cap, zero
 * the rest (a static does).
 */
typedef struct {
    khal_once_t  made;      /**< The one-shot the semaphore and the lock are made by. */
    khal_sem_t   sem;       /**< The blocking scan sleeps here.                       */
    khal_mutex_t lock;      /**< Guards @c busy, @c gen and @c stale.                 */
    bool         busy;      /**< A call holds the claim.                              */
    uint32_t     gen;       /**< The last generation drawn; tags requests apart.      */
    uint32_t     stale;     /**< The generation of a request left unanswered, or 0.   */
    ScanWaiter   wait;      /**< Where AO_Radio writes its answers.                   */
    size_t       row_size;  /**< Bytes of one row the driver keeps.                   */
    uint8_t      cap;       /**< Rows the waiter holds.                               */
} kode_sys_scan_t;

/**
 * @brief Make @p s's semaphore and lock; the body a module hands
 *        kode_sys_scan_enter() calls it.
 * @param s The scan.
 * @return true when both exist.
 */
bool kode_sys_scan_make(kode_sys_scan_t *s);

/**
 * @brief Take the claim, and wait out any request an earlier caller abandoned.
 * @param s    The scan.
 * @param make The module's one-shot body: kode_sys_scan_make() on @p s.
 * @param ms   The longest the abandoned answer is waited for.
 * @return KODE_OK when the claim is held and kode_sys_scan_release() is owed;
 *         KODE_NO_RESOURCES when another call holds it, the abandoned answer
 *         is still owed, or nothing could be made.
 */
kode_status_t kode_sys_scan_enter(kode_sys_scan_t *s, khal_once_try_body_t make, uint32_t ms);

/**
 * @brief Draw the tag for one request.
 * @param s The scan, its claim held.
 * @return The next generation, never zero.
 */
uint32_t kode_sys_scan_gen(kode_sys_scan_t *s);

/**
 * @brief Wait for the answer tagged @p gen.
 * @param s   The scan, its claim held.
 * @param gen The request's tag.
 * @param ms  How long to wait.
 * @return The answer's rc (ScanRspEvt.rc's values); KODE_TIMEOUT when it did
 *         not come, the generation then latched as stale.
 */
kode_status_t kode_sys_scan_await(kode_sys_scan_t *s, uint32_t gen, uint32_t ms);

/**
 * @brief Give the claim back.
 * @param s The scan.
 */
void kode_sys_scan_release(kode_sys_scan_t *s);

/**
 * @brief Take the NFC facade, and stamp this call's generation.
 * @param gen Out: the generation, never zero.
 * @return true when this caller has it.
 */
bool kode_sys_nfc_claim(uint32_t *gen);

/** @brief Give the NFC facade back. */
void kode_sys_nfc_release(void);

/**
 * @brief Wait for this call's own NFC answer, discarding an abandoned one.
 * @param gen This call's generation.
 * @param ms  How long AO_Nfc was asked to keep the field up.
 * @return true when the answer that arrived was this call's.
 */
bool kode_sys_nfc_await(uint32_t gen, uint32_t ms);

/**
 * @brief Take the RFID facade, and stamp this call's generation.
 * @param gen Out: the generation, never zero.
 * @return true when this caller has it.
 */
bool kode_sys_rfid_claim(uint32_t *gen);

/** @brief Give the RFID facade back. */
void kode_sys_rfid_release(void);

/**
 * @brief Wait for this call's own RFID answer, discarding an abandoned one.
 * @param gen This call's generation.
 * @param ms  How long AO_Rfid was asked to keep the field up.
 * @return true when the answer that arrived was this call's.
 */
bool kode_sys_rfid_await(uint32_t gen, uint32_t ms);

#endif /* KODE_SYS_PRIV_H_ */
