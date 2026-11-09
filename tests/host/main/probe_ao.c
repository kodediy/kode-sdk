/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file probe_ao.c
 * @brief The probe Active Object: one job at a time, run inside its step.
 */
#include "probe_ao.h"

#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "kqp.h"
#include "qf_support.h"

/** @brief Stack of the probe's task, in bytes. */
#define PROBE_STACK_BYTES 8192U

/** @brief Slots in the probe's queue. */
#define PROBE_QUEUE_LEN 4U

/** @brief The FreeRTOS priority of the probe's task: the highest there is. */
#define PROBE_TASK_PRIO ((UBaseType_t)(configMAX_PRIORITIES - 1U))

/** @brief Where the FreeRTOS priority sits in a QPrioSpec (qf_port.c). */
#define PROBE_SPEC_TASK_SHIFT 8U

/** @brief The signal that asks the probe to run its job. */
#define PROBE_RUN_SIG IOEXP_INT_SIG

/** @brief The probe and the job it runs next. */
typedef struct {
    QActive            super;  /**< The Active Object.         */
    SemaphoreHandle_t  done;   /**< Given once the job ended.  */
    probe_ao_job_t     job;    /**< The job, set by the asker. */
    void              *ctx;    /**< Its context.               */
} Probe;

static Probe        l_probe;
static QEvtPtr      s_queue[PROBE_QUEUE_LEN];
static StackType_t  s_stack[PROBE_STACK_BYTES / sizeof(StackType_t)];

static const QEvt RUN = QEVT_INITIALIZER(PROBE_RUN_SIG);

/** @brief The probe's only state: on the run event, run the job. */
static QState Probe_idle(Probe * const me, QEvt const * const e)
{
    QState status = Q_SUPER(&QHsm_top);

    if ( (QSignal)PROBE_RUN_SIG == e->sig )
    {
        if ( NULL != me->job )
        {
            me->job(me->ctx);
        }
        (void)xSemaphoreGive(me->done);
        status = Q_HANDLED();
    }
    return status;
}

static QState Probe_initial(Probe * const me, void const * const par)
{
    (void)me;
    (void)par;
    return Q_TRAN(&Probe_idle);
}

/** @brief Start the probe once for the whole binary. */
static void probe_up_once(void)
{
    static bool s_up;

    if ( !s_up )
    {
        qf_support_init();
        l_probe.done = xSemaphoreCreateBinary();
        QActive_ctor(&l_probe.super, Q_STATE_CAST(&Probe_initial));
        QActive_setAttr(&l_probe.super, TASK_NAME_ATTR, PROBE_AO_NAME);
        QACTIVE_START(&l_probe.super,
                      (QPrioSpec)((uint32_t)PROBE_AO_PRIO
                                  | ((uint32_t)PROBE_TASK_PRIO << PROBE_SPEC_TASK_SHIFT)),
                      s_queue, PROBE_QUEUE_LEN, s_stack, sizeof(s_stack), NULL);
        s_up = true;
    }
}

bool probe_ao_run(probe_ao_job_t job, void *ctx, uint32_t wait_ms)
{
    probe_up_once();
    l_probe.job = job;
    l_probe.ctx = ctx;
    QACTIVE_POST(&l_probe.super, &RUN, NULL);
    return xSemaphoreTake(l_probe.done, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}
