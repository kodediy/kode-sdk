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
#define QP_IMPL             // this is QP implementation
#include "qp_port.h"        // QP port
#include "qp_pkg.h"         // QP package-scope interface
#include "qsafe.h"          // QP Functional Safety (FuSa) Subsystem
#ifdef Q_SPY                // QS software tracing enabled?
    #include "qs_port.h"    // QS port
    #include "qs_pkg.h"     // QS package-scope internal interface
#else
    #include "qs_dummy.h"   // disable the QS software tracing
#endif // Q_SPY

Q_DEFINE_THIS_MODULE("qf_port")

// kodeOS/ESP-IDF adaptation: the FreeRTOS kernel in ESP-IDF is SMP and its
// critical sections take a spinlock argument. This spinlock backs ALL QP
// critical sections (task and ISR level) on both cores.
portMUX_TYPE QF_esp_crit_mux_ = portMUX_INITIALIZER_UNLOCKED;

#if (configSUPPORT_STATIC_ALLOCATION == 0)
    #error This QP/C port to FreeRTOS requires configSUPPORT_STATIC_ALLOCATION
#endif

#if (configMAX_PRIORITIES < QF_MAX_ACTIVE)
    #error FreeRTOS configMAX_PRIORITIES must not be less than QF_MAX_ACTIVE
#endif

//============================================================================
static void task_main(void *pvParameters);  // prototype
static void task_main(void *pvParameters) { // FreeRTOS task signature
    QActive * const act = (QActive *)pvParameters;

    // the event-loop...
#ifdef QACTIVE_CAN_STOP
    while (act->eQueue != (QueueHandle_t)0) {
#else
    for (;;) { // for-ever
#endif
        QEvt const * const e = QActive_get_(act); // BLOCK for event
#ifdef QF_AO_WATCH
        // kodeOS adaptation: tell the AO supervisor an event is in progress.
        // The count turns odd after start_tick and sig are in place.
        uint32_t const step_start = (uint32_t)xTaskGetTickCount();
        __atomic_store_n(&act->osObject.watch.start_tick,
                         step_start, __ATOMIC_RELAXED);
        __atomic_store_n(&act->osObject.watch.sig,
                         (uint16_t)e->sig, __ATOMIC_RELAXED);
        __atomic_store_n(&act->osObject.watch.count,
                         act->osObject.watch.count + 1U, __ATOMIC_RELEASE);
#endif
        QASM_DISPATCH(act, e, act->prio); // dispatch event (virtual call)
#if (QF_MAX_EPOOL > 0U)
        QF_gc(e); // check if the event is garbage, and collect it if so
#endif
#ifdef QF_AO_WATCH
        // kodeOS adaptation: the event is done. A step over the limit is
        // recorded here, where it ends, so the supervisor reads its exact
        // length however its passes fall; slow_seq goes last and orders
        // slow_ms and slow_sig before it. The signal is the record's own,
        // because the event may be gone by now. Then the count turns even.
        uint32_t const step_ms = (uint32_t)pdTICKS_TO_MS(
            (uint32_t)xTaskGetTickCount() - step_start);
        if (step_ms > QF_AO_STEP_MAX_MS) {
            __atomic_store_n(&act->osObject.watch.slow_ms,
                             step_ms, __ATOMIC_RELAXED);
            __atomic_store_n(&act->osObject.watch.slow_sig,
                             __atomic_load_n(&act->osObject.watch.sig,
                                             __ATOMIC_RELAXED),
                             __ATOMIC_RELAXED);
            __atomic_store_n(&act->osObject.watch.slow_seq,
                             act->osObject.watch.slow_seq + 1U,
                             __ATOMIC_RELEASE);
        }
        __atomic_store_n(&act->osObject.watch.count,
                         act->osObject.watch.count + 1U, __ATOMIC_RELEASE);
#endif
    }
#ifdef QACTIVE_CAN_STOP
    QActive_unregister_(act); // remove this object from the framewrok
    vTaskDelete((TaskHandle_t)0); // delete this FreeRTOS task
#endif
}

//............................................................................
// kodeOS adaptation: which AO, if any, runs in the given task. The registry is
// walked without a QP critical section: the loop only reads pointers, an AO is
// registered before its task is created and the list never shrinks.
QActive const *QF_aoFromThread(void const * const thread) {
    QActive const *found = (QActive const *)0;
    if (thread != (void const *)0) {
        for (uint_fast8_t prio = 1U; prio <= QF_MAX_ACTIVE; ++prio) {
            QActive const * const act = QActive_registry_[prio];
            if ((act != (QActive const *)0)
                && ((void const *)act->thread == thread))
            {
                found = act;
                break;
            }
        }
    }
    return found;
}

//............................................................................
// kodeOS adaptation: which AO, if any, holds the given QP priority. Reads one
// registry slot, as QF_aoFromThread() does.
QActive const *QF_aoFromPrio(uint_fast8_t const prio) {
    QActive const *found = (QActive const *)0;
    if ((prio >= 1U) && (prio <= QF_MAX_ACTIVE)) {
        found = QActive_registry_[prio];
    }
    return found;
}

#ifdef QF_AO_WATCH
//............................................................................
// kodeOS adaptation: the supervisor's view of the dispatch record. It runs
// in another task, possibly on the other core, and takes no QP critical
// section: the record has one writer and the reads below are acquire loads.
bool QF_aoWatchSample(uint_fast8_t const prio, QAoWatchSample * const out) {
    bool found = false;
    if ((prio >= 1U) && (prio <= QF_MAX_ACTIVE)
        && (out != (QAoWatchSample *)0))
    {
        QActive const * const act = QActive_registry_[prio];
        // an AO is registered before its task exists
        if ((act != (QActive const *)0) && (act->thread != (TaskHandle_t)0)) {
            // slow_seq first, then count: the fields each orders are read after it
            out->slow_seq = __atomic_load_n(&act->osObject.watch.slow_seq,
                                            __ATOMIC_ACQUIRE);
            out->slow_ms = __atomic_load_n(&act->osObject.watch.slow_ms,
                                           __ATOMIC_RELAXED);
            out->slow_sig = __atomic_load_n(&act->osObject.watch.slow_sig,
                                            __ATOMIC_RELAXED);
            out->count = __atomic_load_n(&act->osObject.watch.count,
                                         __ATOMIC_ACQUIRE);
            out->start_ms = (uint32_t)pdTICKS_TO_MS(__atomic_load_n(
                &act->osObject.watch.start_tick, __ATOMIC_RELAXED));
            out->sig = __atomic_load_n(&act->osObject.watch.sig,
                                       __ATOMIC_RELAXED);
            out->name = pcTaskGetName(act->thread);
            out->prio = (uint8_t)prio;
            out->waiting = (uint32_t)uxQueueMessagesWaiting(act->eQueue);
            found = true;
        }
    }
    return found;
}
//............................................................................
uint32_t QF_aoWatchNowMs(void) {
    return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
}
#endif // def QF_AO_WATCH

// The following macro provides the number of free slots in the FreeRTOS
// queue, see NOTE1 and NOTE2
#define FREERTOS_QUEUE_GET_FREE(me_) \
    ((me_)->osObject.queue.uxDummy4[1] - (me_)->osObject.queue.uxDummy4[0])

//============================================================================
// Active Object customization...

//............................................................................
//! @private @memberof QActive
bool QActive_post_(QActive * const me,
    QEvt const * const e,
    uint_fast16_t const margin,
    void const * const sender)
{
#ifndef Q_SPY
    Q_UNUSED_PAR(sender);
#endif

    QF_CRIT_STAT
    QF_CRIT_ENTRY();

    // the event to post must not be NULL
    Q_REQUIRE_INCRIT(100, e != (QEvt *)0);

    // the number of free slots available in the FreeRTOS queue
    QEQueueCtr const nFree =
        (QEQueueCtr)FREERTOS_QUEUE_GET_FREE(me);

    bool status = ((margin == QF_NO_MARGIN)
        || (nFree > (QEQueueCtr)margin));
    if (status) { // should try to post the event?
#if (QF_MAX_EPOOL > 0U)
        if (e->poolNum_ != 0U) { // is it a mutable event?
            QEvt_refCtr_inc_(e); // increment the reference counter
        }
#endif // (QF_MAX_EPOOL > 0U)

        // assume that event posting will be successful, see NOTE3
        QS_BEGIN_PRE(QS_QF_ACTIVE_POST, me->prio)
            QS_TIME_PRE();      // timestamp
            QS_OBJ_PRE(sender); // the sender object
            QS_SIG_PRE(e->sig); // the signal of the event
            QS_OBJ_PRE(me);     // this active object (recipient)
            QS_2U8_PRE(e->poolNum_, e->refCtr_); // pool-Num & ref-Count
            QS_EQC_PRE(nFree);  // # free entries available
            QS_EQC_PRE(0U);     // min # free entries (unknown)
        QS_END_PRE()

        QF_CRIT_EXIT(); // exit crit.sect. before calling RTOS API

        // post the following evtPtr to the RTOS event queue, see NOTE3
        QEvtPtr const evtPtr = {
            e
        };
        status = (xQueueSendToBack(
            me->eQueue, (void const *)&evtPtr, 0U) == pdPASS);

        QF_CRIT_ENTRY(); // re-enter crit.sec.
    }

    if (!status) { // event NOT posted?
        // posting is allowed to fail only when margin != QF_NO_MARGIN
        Q_ASSERT_INCRIT(130, margin != QF_NO_MARGIN);

        QS_BEGIN_PRE(QS_QF_ACTIVE_POST_ATTEMPT, me->prio)
            QS_TIME_PRE();      // timestamp
            QS_OBJ_PRE(sender); // the sender object
            QS_SIG_PRE(e->sig); // the signal of the event
            QS_OBJ_PRE(me);     // this active object (recipient)
            QS_2U8_PRE(e->poolNum_, e->refCtr_); // pool-Num & ref-Count
            QS_EQC_PRE(nFree);  // # free entries
            QS_EQC_PRE(margin); // margin requested
        QS_END_PRE()

        QF_CRIT_EXIT();

#if (QF_MAX_EPOOL > 0U)
        QF_gc(e); // recycle the event to avoid a leak
#endif
    }
    else {
        QF_CRIT_EXIT();
    }

    return status;
}
//............................................................................
//! @private @memberof QActive
void QActive_postLIFO_(QActive * const me, QEvt const * const e) {
    QF_CRIT_STAT
    QF_CRIT_ENTRY();

    // the posted event must be be valid (which includes not NULL)
    Q_REQUIRE_INCRIT(200, e != (QEvt *)0);

#if (QF_MAX_EPOOL > 0U)
    if (e->poolNum_ != 0U) { // is it a mutable event?
        QEvt_refCtr_inc_(e); // increment the reference counter
    }
#endif // (QF_MAX_EPOOL > 0U)

    QS_BEGIN_PRE(QS_QF_ACTIVE_POST_LIFO, me->prio)
        QS_TIME_PRE();       // timestamp
        QS_SIG_PRE(e->sig);  // the signal of this event
        QS_OBJ_PRE(me);      // this active object
        QS_2U8_PRE(e->poolNum_, e->refCtr_); // pool-Num & ref-Count
        QS_EQC_PRE(FREERTOS_QUEUE_GET_FREE(me)); // # free entries
        QS_EQC_PRE(0U);      // min # free entries (unknown)
    QS_END_PRE()

    QF_CRIT_EXIT(); // exit crit.sect. before calling RTOS API

    // post the following evtPtr to the RTOS event queue, see NOTE3
    QEvtPtr const evtPtr = {
        e
    };
    BaseType_t const err =
        xQueueSendToFront(me->eQueue, (void const *)&evtPtr, 0U);

#ifndef Q_UNSAFE
    QF_CRIT_ENTRY();
    // LIFO posting to FreeRTOS message queue must succeed, see NOTE3
    Q_ASSERT_INCRIT(230, err == pdPASS);
    QF_CRIT_EXIT();
#else
    Q_UNUSED_PAR(err);
#endif
}
//............................................................................
//! @private @memberof QActive
QEvt const *QActive_get_(QActive * const me) {
    // wait for an event (forever)
    QEvtPtr evtPtr;
    BaseType_t const err = xQueueReceive(me->eQueue,
        (void *)&evtPtr, portMAX_DELAY);

    QF_CRIT_STAT
    QF_CRIT_ENTRY();

#ifndef Q_UNSAFE
    Q_ASSERT_INCRIT(310, err == pdPASS); // queue-get must succeed
#else
    Q_UNUSED_PAR(err);
#endif

    QS_BEGIN_PRE(QS_QF_ACTIVE_GET, me->prio)
        QS_TIME_PRE();       // timestamp
        QS_SIG_PRE(evtPtr.e->sig); // the signal of this event
        QS_OBJ_PRE(me);      // this active object
        QS_2U8_PRE(evtPtr.e->poolNum_, evtPtr.e->refCtr_);
        QS_EQC_PRE(FREERTOS_QUEUE_GET_FREE(me)); // # free entries available
    QS_END_PRE()

    QF_CRIT_EXIT();

    return evtPtr.e;
}
//............................................................................
//! @static @public @memberof QActive
uint16_t QActive_getQueueUse(uint_fast8_t const prio) {
    Q_UNUSED_PAR(prio);
    return 0U; // current use level in a queue not supported in this RTOS
}
//............................................................................
//! @static @public @memberof QActive
uint16_t QActive_getQueueFree(uint_fast8_t const prio) {
    // kodeOS adaptation: the free slots read as a post reads them (NOTE2),
    // in the same critical section; 0 for a priority no started AO holds.
    uint16_t nFree = 0U;
    QActive const * const act = QF_aoFromPrio(prio);
    if ((act != (QActive const *)0) && (act->eQueue != (QueueHandle_t)0)) {
        QF_CRIT_STAT
        QF_CRIT_ENTRY();
        nFree = (uint16_t)FREERTOS_QUEUE_GET_FREE(act);
        QF_CRIT_EXIT();
    }
    return nFree;
}
//............................................................................
// kodeOS adaptation: the length the queue was started with (NOTE2). It never
// changes after the start, so it is read with no critical section.
uint16_t QF_queueLen(uint_fast8_t const prio) {
    uint16_t len = 0U;
    QActive const * const act = QF_aoFromPrio(prio);
    if ((act != (QActive const *)0) && (act->eQueue != (QueueHandle_t)0)) {
        len = (uint16_t)act->osObject.queue.uxDummy4[1];
    }
    return len;
}
//............................................................................
//! @static @public @memberof QActive
uint16_t QActive_getQueueMin(uint_fast8_t const prio) {
    Q_UNUSED_PAR(prio);
    return 0U; // minimum free entries in a queue not supported in this RTOS
}

//............................................................................
//! @public @memberof QActive
void QActive_start(QActive * const me,
    QPrioSpec const prioSpec,
    QEvtPtr * const qSto, uint_fast16_t const qLen,
    void * const stkSto, uint_fast16_t const stkSize,
    void const * const par)
{
    // create FreeRTOS message queue
    me->eQueue = xQueueCreateStatic(
        (UBaseType_t)qLen,            // length of the queue
        (UBaseType_t)sizeof(QEvtPtr), // item size
        (uint8_t *)qSto,              // storage buffer
        &me->osObject.queue);         // static queue buffer

    QF_CRIT_STAT
    QF_CRIT_ENTRY();
    // the FreeRTOS queue must be created correctly
    Q_ASSERT_INCRIT(410, me->eQueue != (QueueHandle_t)0);
    QF_CRIT_EXIT();

    me->prio  = (uint8_t)(prioSpec & 0xFFU); // QP-priority
    me->pthre = 0U; // preemption-threshold (not used for AO registration)
    QActive_register_(me); // make QF aware of this AO

    // top-most initial tran. (virtual call)
    QASM_INIT(me, par, me->prio);
    QS_FLUSH(); // flush the trace buffer to the host

    // task name provided by the user in QActive_setAttr() or default name
    char const * const taskName = (me->osObject.task.pxDummy1 != (void *)0)
        ? (char const *)me->osObject.task.pxDummy1
        : (char const *)"AO";

    // FreeRTOS priority, see NOTE1
    UBaseType_t freertos_prio = (prioSpec >> 8U);
    if (freertos_prio == 0U) {
        freertos_prio = FREERTOS_TASK_PRIO(me->prio);
    }

    // create the FreeRTOS task for the AO
    me->thread = xTaskCreateStatic(
        &task_main,     // the task function
        taskName,       // the name of the task
        stkSize/sizeof(portSTACK_TYPE), // stack length
        (void *)me,     // the 'pvParameters' parameter
        freertos_prio,  // FreeRTOS priority
        (StackType_t *)stkSto, // stack storage
        &me->osObject.task);   // task buffer

    QF_CRIT_ENTRY();
    // FreeRTOS task must be created correctly
    Q_ASSERT_INCRIT(490, me->thread != (TaskHandle_t)0);
    QF_CRIT_EXIT();
}
//............................................................................
#ifdef QACTIVE_CAN_STOP
//! @public @memberof QActive
void QActive_stop(QActive * const me) {
    if (QActive_subscrList_ != nullptr) {
        QActive_unsubscribeAll(me); // unsubscribe from all events
    }
    me->eQueue = (QueueHandle_t)0; // stop thread (see task_main())
}
#endif
//............................................................................
//! @public @memberof QActive
void QActive_setAttr(QActive *const me, uint32_t attr1, void const *attr2) {
    // NOTE: this function must be called *before* QActive_start(),
    // which implies that me->thread.tx_thread_name must not be used yet;
    QF_CRIT_STAT
    QF_CRIT_ENTRY();
    switch (attr1) {
        case TASK_NAME_ATTR:
            // me->osObject must not be used yet
            Q_ASSERT_INCRIT(510, me->osObject.task.pxDummy1 == (void *)0);
            // temporarily store the name, cast 'const' away
            me->osObject.task.pxDummy1 = (void *)attr2;
            break;
        // ...
        default:
            break;
    }
    QF_CRIT_EXIT();
}

//============================================================================
// The "FromISR" QP APIs for the FreeRTOS port...

//............................................................................
//! @private @memberof QActive
bool QActive_postFromISR_(QActive * const me,
    QEvt const * const e,
    uint_fast16_t const margin,
    BaseType_t * const pxHigherPriorityTaskWoken,
    void const * const sender)
{
#ifndef Q_SPY
    Q_UNUSED_PAR(sender);
#endif

    taskENTER_CRITICAL_ISR(&QF_esp_crit_mux_);

    // the event to post must not be NULL
    Q_REQUIRE_INCRIT(600, e != (QEvt *)0);

    // the number of free slots available in the FreeRTOS queue
    QEQueueCtr const nFree =
        (QEQueueCtr)FREERTOS_QUEUE_GET_FREE(me);

    // should we try to post the event?
    bool status = ((margin == QF_NO_MARGIN)
        || (nFree > (QEQueueCtr)margin));
    if (status) { // should try to post the event?
#if (QF_MAX_EPOOL > 0U)
        if (e->poolNum_ != 0U) { // is it a mutable event?
            QEvt_refCtr_inc_(e); // increment the reference counter
        }
#endif // (QF_MAX_EPOOL > 0U)

        QS_BEGIN_PRE(QS_QF_ACTIVE_POST, me->prio)
            QS_TIME_PRE();      // timestamp
            QS_OBJ_PRE(sender); // the sender object
            QS_SIG_PRE(e->sig); // the signal of the event
            QS_OBJ_PRE(me);     // this active object (recipient)
            QS_2U8_PRE(e->poolNum_, e->refCtr_); // pool-Num & ref-Count
            QS_EQC_PRE(nFree);  // # free entries available
            QS_EQC_PRE(0U);     // min # free entries (unknown)
        QS_END_PRE()

        // exit crit.sect. before calling RTOS API
        taskEXIT_CRITICAL_ISR(&QF_esp_crit_mux_);

        // post the following evtPtr to the RTOS event queue, see NOTE3
        QEvtPtr const evtPtr = {
            e
        };
        status = (xQueueSendToBackFromISR(me->eQueue,
            (void const *)&evtPtr, pxHigherPriorityTaskWoken) == pdPASS);

        taskENTER_CRITICAL_ISR(&QF_esp_crit_mux_);
    }

    if (!status) { // event NOT posted?
        // posting is allowed to fail only when margin != QF_NO_MARGIN
        Q_ASSERT_INCRIT(630, margin != QF_NO_MARGIN);

        QS_BEGIN_PRE(QS_QF_ACTIVE_POST_ATTEMPT, me->prio)
            QS_TIME_PRE();      // timestamp
            QS_OBJ_PRE(sender); // the sender object
            QS_SIG_PRE(e->sig); // the signal of the event
            QS_OBJ_PRE(me);     // this active object (recipient)
            QS_2U8_PRE(e->poolNum_, e->refCtr_); // pool-Num & ref-Count
            QS_EQC_PRE(nFree);  // # free entries available
            QS_EQC_PRE(margin); // margin requested
        QS_END_PRE()

        taskEXIT_CRITICAL_ISR(&QF_esp_crit_mux_);

#if (QF_MAX_EPOOL > 0U)
        QF_gcFromISR(e); // recycle the event to avoid a leak
#endif
    }
    else {
        taskEXIT_CRITICAL_ISR(&QF_esp_crit_mux_);
    }

    return status;
}
//............................................................................
//! @private @static @memberof QActive
void QActive_publishFromISR_(
    QEvt const * const e,
    BaseType_t * const pxHigherPriorityTaskWoken,
    void const * const sender)
{
    taskENTER_CRITICAL_ISR(&QF_esp_crit_mux_);

    // the event must be valid
    Q_REQUIRE_INCRIT(700, e != (QEvt *)0);
    QSignal const sig = e->sig;
    // the published signal must be within the configured range
    Q_REQUIRE_INCRIT(710, sig < (QSignal)QActive_maxPubSignal_);

    QS_BEGIN_PRE(QS_QF_PUBLISH, 0U)
        QS_TIME_PRE();          // the timestamp
        QS_OBJ_PRE(sender);     // the sender object
        QS_SIG_PRE(sig);        // the signal of the event
        QS_2U8_PRE(e->poolNum_, e->refCtr_);
    QS_END_PRE()

#if (QF_MAX_EPOOL > 0U)
    // is it a mutable event?
    if (e->poolNum_ != 0U) {
        QEvt_refCtr_inc_(e);
    }
#endif // (QF_MAX_EPOOL > 0U)

    // make a local, modifiable copy of the subscriber set
    QPSet subscrSet = QActive_subscrList_[sig].set;

    taskEXIT_CRITICAL_ISR(&QF_esp_crit_mux_);

    if (QPSet_notEmpty(&subscrSet)) { // any subscribers?
        // the highest-prio subscriber
        uint_fast8_t p = QPSet_findMax(&subscrSet);

        taskENTER_CRITICAL_ISR(&QF_esp_crit_mux_);

        // no need to lock the scheduler in the ISR context
        QActive *a = QActive_registry_[p];
        // the AO must be registered with the framework
        Q_ASSERT_INCRIT(720, a != (QActive *)0);

        taskEXIT_CRITICAL_ISR(&QF_esp_crit_mux_);

        //QF_SCHED_LOCK_(p); // no scheduler locking in FreeRTOS
        do { // loop over all subscribers
            // QACTIVE_POST() asserts internally if the queue overflows
            QACTIVE_POST_FROM_ISR(a, e, pxHigherPriorityTaskWoken, sender);

            QPSet_remove(&subscrSet, p); // remove the handled subscriber
            if (QPSet_notEmpty(&subscrSet)) {  // still more subscribers?
                p = QPSet_findMax(&subscrSet); // highest-prio subscriber

                taskENTER_CRITICAL_ISR(&QF_esp_crit_mux_);

                a = QActive_registry_[p];
                // the AO must be registered with the framework
                Q_ASSERT_INCRIT(730, a != (QActive *)0);

                taskEXIT_CRITICAL_ISR(&QF_esp_crit_mux_);
            }
            else {
                p = 0U; // no more subscribers
            }
        } while (p != 0U);

        taskENTER_CRITICAL_ISR(&QF_esp_crit_mux_);
        Q_ASSERT_INCRIT(590, p == 0U); // all subscribers processed
        taskEXIT_CRITICAL_ISR(&QF_esp_crit_mux_);

        //QF_SCHED_UNLOCK_(); // no scheduler locking in FreeRTOS
    }

#if (QF_MAX_EPOOL > 0U)
    // The following garbage collection step decrements the reference counter
    // and recycles the event if the counter drops to zero. This covers both
    // cases when the event was published with or without any subscribers.
    QF_gcFromISR(e);
#endif // (QF_MAX_EPOOL > 0U)
}

//----------------------------------------------------------------------------
// QTimeEvt "fromISR"

//............................................................................
//! @private @static @memberof QTimeEvt
void QTimeEvt_tickFromISR_(
    uint_fast8_t const tickRate,
    BaseType_t * const pxHigherPriorityTaskWoken,
    void const * const sender)
{
#ifndef Q_SPY
    Q_UNUSED_PAR(sender);
#endif

    taskENTER_CRITICAL_ISR(&QF_esp_crit_mux_);

    Q_REQUIRE_INCRIT(800, tickRate < Q_DIM(QTimeEvt_timeEvtHead_));

    QTimeEvt *prev = &QTimeEvt_timeEvtHead_[tickRate];

#ifdef Q_SPY
    QS_BEGIN_PRE(QS_QF_TICK, 0U)
        ++prev->ctr;
        QS_TEC_PRE(prev->ctr);   // tick ctr
        QS_U8_PRE(tickRate);     // tick rate
    QS_END_PRE()
#endif // def Q_SPY

    // scan the linked-list of time events at this rate...
    while (true) {
        Q_ASSERT_INCRIT(810, prev != (QTimeEvt *)0); // sanity check

        QTimeEvt *te = prev->next; // advance down the time evt. list

        if (te == (QTimeEvt *)0) { // end of the list?
            // NO any new time events armed since the last QTimeEvt_tick_()?
            if (QTimeEvt_timeEvtHead_[tickRate].act == (void *)0) {
                break; // terminate the for-loop
            }

            prev->next = (QTimeEvt*)QTimeEvt_timeEvtHead_[tickRate].act;
            QTimeEvt_timeEvtHead_[tickRate].act = (void *)0;

            te = prev->next; // switch to the new list
        }

        // the time event 'te' must be valid
        Q_ASSERT_INCRIT(640, te != (QTimeEvt *)0);

        QTimeEvtCtr ctr = te->ctr; // move volatile into temporary

        if (ctr == 0U) { // time event scheduled for removal?
            prev->next = te->next;

            // mark time event 'te' as NOT linked
            te->flags &= (uint8_t)(~QTE_FLAG_IS_LINKED & 0xFFU);
            // do NOT advance the prev pointer

            // exit crit. section to reduce latency
            taskEXIT_CRITICAL_ISR(&QF_esp_crit_mux_);
        }
        else if (ctr == 1U) { // is time event about to expire?
            QActive * const act = (QActive *)te->act;
            prev = QTimeEvt_expire_(te, prev, act, tickRate);

            taskEXIT_CRITICAL_ISR(&QF_esp_crit_mux_);

            // QACTIVE_POST_FROM_ISR() asserts if the queue overflows
            QACTIVE_POST_FROM_ISR(act, &te->super,
                                  pxHigherPriorityTaskWoken,
                                  sender);
        }
        else { // time event keeps timing out
            --ctr; // decrement the tick counter
            te->ctr = ctr; // update the original
            prev = te; // advance to this time event

            taskEXIT_CRITICAL_ISR(&QF_esp_crit_mux_);
        }
        // re-enter crit. section to continue the loop
        taskENTER_CRITICAL_ISR(&QF_esp_crit_mux_);
    }

    taskEXIT_CRITICAL_ISR(&QF_esp_crit_mux_);
}

//----------------------------------------------------------------------------
#if (QF_MAX_EPOOL > 0U)

QEvt *QF_newXFromISR_(
    uint_fast16_t const evtSize,
    uint_fast16_t const margin,
    enum_t const sig)
{
    // find the pool number that fits the requested event size...
    uint8_t poolNum = 0U; // zero-based poolNum initially
    for (; poolNum < QF_priv_.maxPool_; ++poolNum) {
        if (evtSize <= QF_EPOOL_EVENT_SIZE_(QF_priv_.ePool_[poolNum])) {
            break;
        }
    }

    taskENTER_CRITICAL_ISR(&QF_esp_crit_mux_);

    // precondition:
    // - cannot run out of registered pools
    Q_REQUIRE_INCRIT(700, poolNum < QF_priv_.maxPool_);

    ++poolNum; // convert to 1-based poolNum

    taskEXIT_CRITICAL_ISR(&QF_esp_crit_mux_);

    // get event e (port-dependent)...
    QEvt *e;
#ifdef Q_SPY
    e = QMPool_getFromISR(&QF_priv_.ePool_[poolNum - 1U],
                  ((margin != QF_NO_MARGIN) ? margin : 0U),
                  (uint_fast8_t)QS_ID_EP + poolNum);
#else
    e = QMPool_getFromISR(&QF_priv_.ePool_[poolNum - 1U],
                      ((margin != QF_NO_MARGIN) ? margin : 0U), 0U);
#endif

    if (e != (QEvt *)0) { // was e allocated correctly?
        e->sig      = (QSignal)sig; // set the signal
        e->poolNum_ = poolNum;
        e->refCtr_  = 0U;

#ifdef Q_SPY
        taskENTER_CRITICAL_ISR(&QF_esp_crit_mux_);
        QS_BEGIN_PRE(QS_QF_NEW, (uint_fast8_t)QS_ID_EP + poolNum)
            QS_TIME_PRE();        // timestamp
            QS_EVS_PRE(evtSize);  // the size of the event
            QS_SIG_PRE(sig);      // the signal of the event
        QS_END_PRE()
        taskEXIT_CRITICAL_ISR(&QF_esp_crit_mux_);
#endif // def Q_SPY
    }
    else { // event was not allocated
        taskENTER_CRITICAL_ISR(&QF_esp_crit_mux_);

        // This assertion means that the event allocation failed,
        // and this failure cannot be tolerated. The most frequent
        // reason is an event leak in the application.
        Q_ASSERT_INCRIT(720, margin != QF_NO_MARGIN);

        QS_BEGIN_PRE(QS_QF_NEW_ATTEMPT,
                (uint_fast8_t)QS_ID_EP + poolNum)
            QS_TIME_PRE();        // timestamp
            QS_EVS_PRE(evtSize);  // the size of the event
            QS_SIG_PRE(sig);      // the signal of the event
        QS_END_PRE()

        taskEXIT_CRITICAL_ISR(&QF_esp_crit_mux_);
    }

    // the returned event e is guaranteed to be valid (not NULL)
    // if we can't tolerate failed allocation
    return e;
}
//............................................................................
void QF_gcFromISR(QEvt const * const e) {
    taskENTER_CRITICAL_ISR(&QF_esp_crit_mux_);

    Q_REQUIRE_INCRIT(900, e != (QEvt *)0);

    uint8_t const poolNum = e->poolNum_;

    if (poolNum != 0U) { // is it a pool event (mutable)?

        if (e->refCtr_ > 1U) { // isn't this the last reference?

            QS_BEGIN_PRE(QS_QF_GC_ATTEMPT,
                    (uint_fast8_t)QS_ID_EP + poolNum)
                QS_TIME_PRE();       // timestamp
                QS_SIG_PRE(e->sig);  // the signal of the event
                QS_2U8_PRE(poolNum, e->refCtr_);
            QS_END_PRE()

            Q_ASSERT_INCRIT(805, e->refCtr_ > 0U);
            QEvt_refCtr_dec_(e); // decrement the ref counter

            taskEXIT_CRITICAL_ISR(&QF_esp_crit_mux_);
        }
        else { // this is the last reference to this event, recycle it

            QS_BEGIN_PRE(QS_QF_GC,
                    (uint_fast8_t)QS_ID_EP + poolNum)
                QS_TIME_PRE();       // timestamp
                QS_SIG_PRE(e->sig);  // the signal of the event
                QS_2U8_PRE(poolNum, e->refCtr_);
            QS_END_PRE()

            // pool number must be in range
            Q_ASSERT_INCRIT(910, (poolNum <= QF_priv_.maxPool_)
                                  && (poolNum <= QF_MAX_EPOOL));
            taskEXIT_CRITICAL_ISR(&QF_esp_crit_mux_);

            // NOTE: casting 'const' away is legit because 'e' is a pool event
#ifdef Q_SPY
            // cast 'const' away in (QEvt *)e is OK because it's a pool event
            QMPool_putFromISR(&QF_priv_.ePool_[poolNum - 1U], (QEvt *)e,
                              (uint_fast8_t)QS_ID_EP + e->poolNum_);
#else
            QMPool_putFromISR(&QF_priv_.ePool_[poolNum - 1U], (QEvt *)e, 0U);
#endif
        }
    }
    else {
        taskEXIT_CRITICAL_ISR(&QF_esp_crit_mux_);
    }
}
//............................................................................
void *QMPool_getFromISR(QMPool * const me,
    uint_fast16_t const margin,
    uint_fast8_t const qsId)
{
#ifndef Q_SPY
    Q_UNUSED_PAR(qsId);
#endif

    taskENTER_CRITICAL_ISR(&QF_esp_crit_mux_);

    // get volatile into temporaries
    void * *pfb = me->freeHead; // pointer to free block
    QMPoolCtr nFree = me->nFree;

    // have more free blocks than the requested margin?
    if (nFree > (QMPoolCtr)margin) {
        Q_ASSERT_INCRIT(1010, pfb != (void**)0);

        void ** const pfb_next = pfb[0]; // fast temporary

        --nFree; // one less free block
        if (nFree == 0U) { // is the pool becoming empty?
            // pool is becoming empty, so the next free block must be NULL
            Q_ASSERT_INCRIT(920, pfb_next == (void**)0);

            me->nFree = 0U; // no more free blocks
            me->nMin  = 0U; // remember that the pool got empty
        }
        else { // the pool is NOT empty

            // the next free-block pointer must be in range
            Q_ASSERT_INCRIT(930,
                (me->start <= pfb_next) && (pfb_next <= me->end));

            me->nFree = nFree; // update the original
            if (me->nMin > nFree) { // is this the new minimum?
                me->nMin = nFree; // remember the minimum so far
            }
        }

        me->freeHead = pfb_next; // set the head to the next free block

        // change the allocated block contents so that it is different
        // than a free block inside the pool.
        pfb[0] = &me->end[1]; // invalid location beyond the end

        QS_BEGIN_PRE(QS_QF_MPOOL_GET, qsId)
            QS_TIME_PRE();         // timestamp
            QS_OBJ_PRE(me);        // this memory pool
            QS_MPC_PRE(nFree);     // # free blocks in the pool
            QS_MPC_PRE(me->nMin);  // min # free blocks ever in the pool
        QS_END_PRE()
    }
    else { // don't have enough free blocks at this point
        pfb = (void**)0;

        QS_BEGIN_PRE(QS_QF_MPOOL_GET_ATTEMPT, qsId)
            QS_TIME_PRE();         // timestamp
            QS_OBJ_PRE(me);        // this memory pool
            QS_MPC_PRE(nFree);     // # free blocks in the pool
            QS_MPC_PRE(margin);    // the requested margin
        QS_END_PRE()
    }

    taskEXIT_CRITICAL_ISR(&QF_esp_crit_mux_);

    return (void *)pfb; // return the block or NULL pointer to the caller
}
//............................................................................
void QMPool_putFromISR(
    QMPool * const me, void *block,
    uint_fast8_t const qsId)
{
#ifndef Q_SPY
    Q_UNUSED_PAR(qsId);
#endif

    void * * const pfb = (void * *)block; // pointer to free block

    taskENTER_CRITICAL_ISR(&QF_esp_crit_mux_);

    // get volatile into temporaries
    void * * const freeHead = me->freeHead;
    QMPoolCtr nFree = me->nFree;

    Q_REQUIRE_INCRIT(1000, nFree < me->nTot);
    Q_REQUIRE_INCRIT(1010, (me->start <= pfb) && (pfb <= me->end));

    ++nFree; // one more free block in this pool

    me->freeHead = pfb; // set as new head of the free list
    me->nFree = nFree;
    pfb[0] = freeHead; // link into the list

    QS_BEGIN_PRE(QS_QF_MPOOL_PUT, qsId)
        QS_TIME_PRE();         // timestamp
        QS_OBJ_PRE(me);        // this memory pool
        QS_MPC_PRE(nFree);     // the # free blocks in the pool
    QS_END_PRE()

    taskEXIT_CRITICAL_ISR(&QF_esp_crit_mux_);
}
#endif // (QF_MAX_EPOOL > 0U)

//============================================================================
void QF_init(void) {
    QTimeEvt_init(); // initialize QTimeEvts
}
//............................................................................
int_t QF_run(void) {
    QF_onStartup(); // the startup callback

    // produce the QS_QF_RUN trace record
#ifdef Q_SPY
    QS_CRIT_STAT
    QS_CRIT_ENTRY();
    QS_BEGIN_PRE(QS_QF_RUN, 0U)
    QS_END_PRE()
    QS_CRIT_EXIT();
#endif // Q_SPY

    // kodeOS/ESP-IDF adaptation: in ESP-IDF the FreeRTOS scheduler is already
    // running when app_main() executes, so vTaskStartScheduler() must NOT be
    // called. The AO tasks created by QACTIVE_START() are already runnable;
    // QF_run() returns to app_main(), whose task FreeRTOS deletes on return.

    return 0;
}
//............................................................................
void QF_stop(void) {
    QF_onCleanup(); // cleanup callback
}

//============================================================================
// NOTE1:
// The FreeRTOS priority of the AO thread can be specified in two ways:
//
// A. Implicitly based on the AO's priority (by the formula specified
//    in the macro FREERTOS_TASK_PRIO(), see qp_port.h). This option
//    is chosen, when the higher-byte of the prioSpec parameter is set
//    to zero.
//
// B. Explicitly as the higher-byte of the prioSpec parameter.
//    This option is chosen when the prioSpec parameter is not-zero.
//    For example, Q_PRIO(10U, 5U) will explicitly specify AO priority
//    as 10 and FreeRTOS priority as 5.
//
// CAUTION: The explicit FreeRTOS priority is NOT sanity-checked, so it is the
// responsibility of the application to ensure that it is consistent with the
// QP priority. An example of inconsistent setting would be assigning FreeRTOS
// priorities that would result in a different relative prioritization of AOs
// than indicated by the QP priorities assigned to the AOs.
//
// NOTE2:
// The official FreeRTOS API uxQueueSpacesAvailable() is not used because
// that API uses task-level critical section internally. Instead, the free
// slots calculation happens here in already established critical section.
// Unfortunately, the bizarre "information obfuscating" policy of FreeRTOS
// (incorrectly called "information hiding") forces the use of the StaticQueue_t
// with "dummy" members. This could potentially break in the future releases
// of FreeRTOS.
//
// Currently, the correspondence between xQUEUE and StaticQueue_t is as follows
// (see queue.c and FreeRTOS.h, respectively):
//
// xQUEUE.uxMessagesWaiting == StaticQueue_t.uxDummy4[0];
// xQUEUE.uxLength          == StaticQueue_t.uxDummy4[1];
//
// NOTE3:
// The event posting to FreeRTOS message queue occurs OUTSIDE critical section,
// which means that the remaining margin of available slots in the queue
// cannot be guaranteed. The problem is that interrupts and other tasks can
// preempt the event posting after checking the margin, but before actually
// posting the event to the queue.
//
