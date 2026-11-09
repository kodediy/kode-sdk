//============================================================================
// QP/C Real-Time Event Framework (RTEF)
//
// Copyright (C) 2005 Quantum Leaps, LLC. All rights reserved.
//
//                    Q u a n t u m  L e a P s
//                    ------------------------
//                    Modern Embedded Software
//
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-QL-commercial
//
// This software is dual-licensed under the terms of the open-source GNU
// General Public License (GPL) or under the terms of one of the closed-
// source Quantum Leaps commercial licenses.
//
// Redistributions in source code must retain this top-level comment block.
// Plagiarizing this software to sidestep the license obligations is illegal.
//
// NOTE:
// The GPL does NOT permit the incorporation of this code into proprietary
// programs. Please contact Quantum Leaps for commercial licensing options,
// which expressly supersede the GPL and are designed explicitly for
// closed-source distribution.
//
// Quantum Leaps contact information:
// <www.state-machine.com/licensing>
// <info@state-machine.com>
//============================================================================
#ifndef QP_PORT_H_
#define QP_PORT_H_

#include <stdint.h>     // Exact-width types. WG14/N843 C99 Standard
#include <stdbool.h>    // Boolean type.      WG14/N843 C99 Standard
#include <stddef.h>     // size_t type        ISO/IEC 9899:1990 Standard
#include "qp_config.h"  // QP configuration from the application

#ifdef __cplusplus
    // no-return function specifier (C++11 Standard)
    #define Q_NORETURN  [[ noreturn ]] void

    // static assertion (C++11 Standard)
    #define Q_ASSERT_STATIC(expr_)  static_assert((expr_), "QP static assert")
#else
    // no-return function specifier (C11 Standard)
    #define Q_NORETURN   _Noreturn void

    // static assertion (C11 Standard)
    #define Q_ASSERT_STATIC(expr_)  _Static_assert((expr_), "QP static assert")
#endif

// QActive event queue and thread types for FreeRTOS
#define QACTIVE_EQUEUE_TYPE     QueueHandle_t
#define QACTIVE_OS_OBJ_TYPE     OsObject
#define QACTIVE_THREAD_TYPE     TaskHandle_t

// FreeRTOS requires the "FromISR" API in QP/C
#define QF_ISR_API              1

// QF interrupt disabling/enabling (task level)
#define QF_INT_DISABLE()        taskDISABLE_INTERRUPTS()
#define QF_INT_ENABLE()         taskENABLE_INTERRUPTS()

// QF critical section for FreeRTOS (task level), see NOTE2
// kodeOS/ESP-IDF adaptation: ESP-IDF FreeRTOS is SMP and its critical
// sections take a spinlock argument (defined in qf_port.c). Plain interrupt
// disabling would not protect against the other core.
#define QF_CRIT_STAT
#define QF_CRIT_ENTRY()         taskENTER_CRITICAL(&QF_esp_crit_mux_)
#define QF_CRIT_EXIT()          taskEXIT_CRITICAL(&QF_esp_crit_mux_)
#define QF_CRIT_EST()           taskENTER_CRITICAL(&QF_esp_crit_mux_)

// include files -------------------------------------------------------------
// kodeOS/ESP-IDF adaptation: ESP-IDF namespaces the FreeRTOS headers
#include "freertos/FreeRTOS.h"  // FreeRTOS master include file, see NOTE3
#include "freertos/task.h"      // FreeRTOS task management
#include "freertos/queue.h"     // FreeRTOS queue management

// kodeOS/ESP-IDF adaptation: what the AO supervisor in kqp reads, see
// QF_aoWatchSample(). One writer per record (the AO's own task, in
// task_main()); `count` is odd while the AO is inside an event and even while
// it waits, and its release stores order the other two fields before it.
// `slow_seq` counts the steps that ended over QF_AO_STEP_MAX_MS, and its
// release store orders `slow_ms` and `slow_sig` (the latest such step's) before
// it.
#ifdef QF_AO_WATCH
typedef struct {
    uint32_t count;       //!< bumped before and after each dispatch
    uint32_t start_tick;  //!< FreeRTOS tick at which the current dispatch began
    uint16_t sig;         //!< signal of the event being dispatched
    uint32_t slow_seq;    //!< steps that ended over QF_AO_STEP_MAX_MS, wraps
    uint32_t slow_ms;     //!< length of the latest of them, in ms
    uint16_t slow_sig;    //!< signal of that step
} QAoWatchRec;
#endif // def QF_AO_WATCH

typedef struct {
    StaticQueue_t queue;
    StaticTask_t  task;
#ifdef QF_AO_WATCH
    QAoWatchRec   watch;
#endif
} OsObject;

// kodeOS/ESP-IDF adaptation: SMP spinlock backing all QP critical sections
// (defined in qf_port.c)
extern portMUX_TYPE QF_esp_crit_mux_;

#include "qequeue.h"   // QP event queue (for deferring events)
#include "qmpool.h"    // QP memory pool (for event pools)
#include "qp.h"        // QP platform-independent public interface

// the "FromISR" versions of the QF APIs, see NOTE3
#ifdef Q_SPY

#define QACTIVE_POST_FROM_ISR(me_, e_, pxHigherPrioTaskWoken_, sender_) \
    ((void)QActive_postFromISR_((me_), (e_), QF_NO_MARGIN,              \
                                (pxHigherPrioTaskWoken_), (sender_)))

#define QACTIVE_POST_X_FROM_ISR(me_, e_, margin_,                \
                                pxHigherPrioTaskWoken_, sender_) \
    (QActive_postFromISR_((me_), (e_), (margin_),                \
                          (pxHigherPrioTaskWoken_), (sender_)))

#define QACTIVE_PUBLISH_FROM_ISR(e_, pxHigherPrioTaskWoken_, sender_) \
    (QActive_publishFromISR_((e_), (pxHigherPrioTaskWoken_),          \
                        (void const *)(sender_)))

#define QTIMEEVT_TICK_FROM_ISR(tickRate_, pxHigherPrioTaskWoken_, sender_) \
    (QTimeEvt_tickFromISR_((tickRate_), (pxHigherPrioTaskWoken_), (sender_)))

#else // ndef Q_SPY

#define QACTIVE_POST_FROM_ISR(me_, e_, pxHigherPrioTaskWoken_, dummy) \
    ((void)QActive_postFromISR_((me_), (e_), QF_NO_MARGIN,              \
                                (pxHigherPrioTaskWoken_), (void *)0))

#define QACTIVE_POST_X_FROM_ISR(me_, e_, margin_,                \
                                pxHigherPrioTaskWoken_, dummy) \
    (QActive_postFromISR_((me_), (e_), (margin_),                \
                          (pxHigherPrioTaskWoken_), (void *)0))

#define QACTIVE_PUBLISH_FROM_ISR(e_, pxHigherPrioTaskWoken_, dummy) \
    (QActive_publishFromISR_((e_), (pxHigherPrioTaskWoken_),          \
                        (void *)0))

#define QTIMEEVT_TICK_FROM_ISR(tickRate_, pxHigherPrioTaskWoken_, dummy) \
    (QTimeEvt_tickFromISR_((tickRate_), (pxHigherPrioTaskWoken_), (void *)0))

#endif // Q_SPY

bool QActive_postFromISR_(QActive * const me, QEvt const * const e,
                          uint_fast16_t const margin,
                          BaseType_t * const pxHigherPriorityTaskWoken,
                          void const * const sender);

void QActive_publishFromISR_(QEvt const * const e,
                          BaseType_t * const pxHigherPriorityTaskWoken,
                          void const * const sender);

void QTimeEvt_tickFromISR_(uint_fast8_t const tickRate,
                          BaseType_t * const pxHigherPriorityTaskWoken,
                          void const * const sender);

#define QF_TICK_FROM_ISR(pxHigherPrioTaskWoken_, sender_) \
    QTIMEEVT_TICK_FROM_ISR(0U, pxHigherPrioTaskWoken_, sender_)

#ifdef QEVT_DYN_CTOR // Shall the ctor for the ::QEvt class be provided?

    #define Q_NEW_FROM_ISR(evtT_, sig_, ...)                  \
        (evtT_##_ctor((evtT_ *)QF_newXFromISR_(sizeof(evtT_), \
                      QF_NO_MARGIN, (sig_)), __VA_ARGS__))

    #define Q_NEW_X_FROM_ISR(e_, evtT_, margin_, sig_, ...) do { \
        (e_) = (evtT_ *)QF_newXFromISR_(sizeof(evtT_),           \
                                 (margin_), (sig_));             \
        if ((e_) != (evtT_ *)0) {                                \
            evtT_##_ctor((e_), __VA_ARGS__);                     \
        }                                                        \
     } while (false)

#else // no ::QEvt ctor

    #define Q_NEW_FROM_ISR(evtT_, sig_)                         \
        ((evtT_ *)QF_newXFromISR_((uint_fast16_t)sizeof(evtT_), \
                                  QF_NO_MARGIN, (sig_)))

    #define Q_NEW_X_FROM_ISR(e_, evtT_, margin_, sig_) ((e_) = \
        (evtT_ *)QF_newXFromISR_((uint_fast16_t)sizeof(evtT_), \
                                 (margin_), (sig_)))

#endif // QEVT_DYN_CTOR

void QF_gcFromISR(QEvt const * const e);

#ifdef QF_AO_WATCH
//! One reading of an Active Object's dispatch record, for the supervisor.
typedef struct {
    char const *name;    //!< the AO's task name
    uint8_t     prio;    //!< the AO's QP priority
    uint32_t    count;   //!< dispatch counter: odd while inside an event
    uint32_t    start_ms; //!< tick at which the current dispatch began, in ms
    uint16_t    sig;     //!< signal of the event being dispatched
    uint32_t    waiting; //!< events in the AO's queue right now
    uint32_t    slow_seq; //!< steps that ended over QF_AO_STEP_MAX_MS, wraps
    uint32_t    slow_ms; //!< length of the latest of them, in ms
    uint16_t    slow_sig; //!< signal of that step
} QAoWatchSample;

//! Reads the dispatch record of the AO that holds `prio`.
//! @param[in]  prio QP priority, 1..QF_MAX_ACTIVE
//! @param[out] out  the reading
//! @return false when no AO holds `prio` or its task is not running yet
bool QF_aoWatchSample(uint_fast8_t const prio, QAoWatchSample * const out);

//! The FreeRTOS tick count in milliseconds, the clock of `start_ms`.
//! @return the tick count in ms; wraps at 2^32
uint32_t QF_aoWatchNowMs(void);
#endif // def QF_AO_WATCH

//! Finds the Active Object whose FreeRTOS task is `thread`.
//! kodeOS adaptation: the registry is QP-private, so the question is asked
//! from inside the port, which is allowed to read it. It takes no QP critical
//! section: an AO is registered before its task exists and never leaves.
//! @param[in] thread a FreeRTOS task handle (TaskHandle_t), as an opaque pointer
//! @return the AO that runs in that task, or NULL when no started AO does
QActive const *QF_aoFromThread(void const *thread);

//! Finds the Active Object registered at QP priority `prio`.
//! kodeOS adaptation, like QF_aoFromThread(): the registry is QP-private, so
//! the question is asked from inside the port. No QP critical section: an AO
//! is registered once and never leaves.
//! @param[in] prio a QP priority, 1 to QF_MAX_ACTIVE
//! @return the AO registered at that priority, or NULL when none is (or the
//!         priority is out of range)
QActive const *QF_aoFromPrio(uint_fast8_t const prio);

//! The length of the queue of the Active Object at QP priority `prio`.
//! kodeOS adaptation: with QActive_getQueueFree(), which this port implements
//! too, it says how full a queue is.
//! @param[in] prio a QP priority, 1 to QF_MAX_ACTIVE
//! @return the queue's length, or 0 when no started AO holds the priority
uint16_t QF_queueLen(uint_fast8_t const prio);

QEvt *QF_newXFromISR_(uint_fast16_t const evtSize,
                      uint_fast16_t const margin, enum_t const sig);

void *QMPool_getFromISR(QMPool * const me, uint_fast16_t const margin,
                        uint_fast8_t const qsId);
void QMPool_putFromISR(QMPool * const me, void *block,
                        uint_fast8_t const qsId);

enum FreeRTOS_TaskAttrs {
    TASK_NAME_ATTR
};

// FreeRTOS hooks prototypes (not provided by FreeRTOS)
#if (configUSE_IDLE_HOOK > 0)
    void vApplicationIdleHook(void);
#endif
#if (configUSE_TICK_HOOK > 0)
    void vApplicationTickHook(void);
#endif
#if (configCHECK_FOR_STACK_OVERFLOW > 0)
    void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName);
#endif
#if (configSUPPORT_STATIC_ALLOCATION > 0)
    void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                       StackType_t **ppxIdleTaskStackBuffer,
                                       uint32_t *pulIdleTaskStackSize);
#endif

//============================================================================
// interface used only inside QF, but not in applications

#ifdef QP_IMPL

#define FREERTOS_TASK_PRIO(qp_prio_) \
    ((UBaseType_t)((qp_prio_) + tskIDLE_PRIORITY))

// FreeRTOS scheduler locking for QF_publish_() (task context only)
#define QF_SCHED_STAT_
#define QF_SCHED_LOCK_(prio_) (vTaskSuspendAll())
#define QF_SCHED_UNLOCK_()    ((void)xTaskResumeAll())

// QMPool operations
#define QF_EPOOL_TYPE_  QMPool
#define QF_EPOOL_INIT_(p_, poolSto_, poolSize_, evtSize_) \
            (QMPool_init(&(p_), (poolSto_), (poolSize_), (evtSize_)))
#define QF_EPOOL_EVENT_SIZE_(p_)  ((uint16_t)(p_).blockSize)
#define QF_EPOOL_GET_(p_, e_, m_, qsId_) \
            ((e_) = (QEvt *)QMPool_get(&(p_), (m_), (qsId_)))
#define QF_EPOOL_PUT_(p_, e_, qsId_) (QMPool_put(&(p_), (e_), (qsId_)))
#define QF_EPOOL_USE_(ePool_)   (QMPool_getUse(ePool_))
#define QF_EPOOL_FREE_(ePool_)  ((uint16_t)(ePool_)->nFree)
#define QF_EPOOL_MIN_(ePool_)   ((uint16_t)(ePool_)->nMin)

#endif // def QP_IMPL

//============================================================================
// NOTE2:
// The critical section definition applies only to the FreeRTOS "task level"
// APIs. The "FromISR" APIs are defined separately.
//
// NOTE3:
// The design of FreeRTOS requires using different APIs inside the ISRs
// (the "FromISR" variant) than at the task level. Accordingly, this port
// provides the "FromISR" variants for QP functions and "FROM_ISR" variants
// for QP macros to be used inside ISRs. ONLY THESE "FROM_ISR" VARIANTS
// ARE ALLOWED INSIDE ISRs AND CALLING THE TASK-LEVEL APIs IS AN ERROR.

#endif // QP_PORT_H_

