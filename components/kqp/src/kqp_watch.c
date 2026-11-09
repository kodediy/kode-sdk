/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kqp_watch.c
 * @brief The AO supervisor: samples each AO every period and panics naming one stuck or starved.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "qpc.h"
#include "kode_defs.h"
#include "kode_log.h"
#include "khal_system.h"
#include "kqp_bsp.h"
#include "kqp_config.h"
#include "kqp_watch_judge.h"

/** @brief Entries in the sample and slot arrays: one per QP priority. */
#define WATCH_SLOTS ((uint8_t)QF_MAX_ACTIVE)

/** @brief Bytes of the panic reason, name and numbers included. */
#define WATCH_REASON_LEN 80U

/** @brief Milliseconds in a second. */
#define WATCH_MS_PER_S 1000U

/** @brief Milliseconds in a tenth of a second. */
#define WATCH_MS_PER_TENTH 100U

/** @brief What the last pass read, one entry per priority (index = prio - 1). */
static KqpWatchSample s_samples[WATCH_SLOTS];

/**
 * @brief Read one Active Object into a judge sample.
 *
 * @param prio QP priority, 1..QF_MAX_ACTIVE.
 * @param out  Receives the reading; invalid when no AO holds the priority.
 */
static void read_sample(uint8_t prio, KqpWatchSample *out)
{
    QAoWatchSample raw;

    out->valid = QF_aoWatchSample(prio, &raw);
    out->name[0] = '\0';
    if ( out->valid )
    {
        uint8_t i = 0U;

        while ( ((KQP_WATCH_NAME_LEN - 1U) > i) && ('\0' != raw.name[i]) )
        {
            out->name[i] = raw.name[i];
            ++i;
        }
        out->name[i] = '\0';
        out->prio = raw.prio;
        out->count = raw.count;
        out->start_ms = raw.start_ms;
        out->sig = raw.sig;
        out->waiting = raw.waiting;
        out->slow_seq = raw.slow_seq;
        out->slow_ms = raw.slow_ms;
        out->slow_sig = raw.slow_sig;
    }
}

/**
 * @brief Warn of each Active Object's steps that ran over the limit.
 *
 * Printed at every log level except off (kode_log_always()). It runs in the
 * supervisor's pass, never on the panic path.
 */
static void slow_steps_log(void)
{
    /* The slow-step count each Active Object had at the last pass. */
    static uint32_t s_slow_seen[WATCH_SLOTS];
    uint8_t i;

    for ( i = 0U; WATCH_SLOTS > i; ++i )
    {
        KqpWatchSlow const r = kqp_watch_slow(&s_slow_seen[i], &s_samples[i],
                                              QF_AO_STEP_MAX_MS);

        if ( r.log && (1U < r.count) )
        {
            kode_log_always(KODE_LOG_WARN, "kqp",
                            "watch: %s step %u ms (signal 0x%x), %u slow steps since the last pass",
                            s_samples[i].name, (unsigned)r.ms, (unsigned)r.sig,
                            (unsigned)r.count);
        }
        else if ( r.log )
        {
            kode_log_always(KODE_LOG_WARN, "kqp", "watch: %s step %u ms (signal 0x%x)",
                            s_samples[i].name, (unsigned)r.ms, (unsigned)r.sig);
        }
        else
        {
            /* Nothing slow since the last pass. */
        }
    }
}

/**
 * @brief Say which Active Object hung, then panic. Does not return.
 *
 * @param v The judge's verdict, not OK.
 */
static void report(KqpWatchVerdict const *v) __attribute__((noreturn));
static void report(KqpWatchVerdict const *v)
{
    /* Handed to the panic handler, which keeps the pointer. */
    static char watch_reason[WATCH_REASON_LEN];
    KqpWatchSample const *const s = &s_samples[v->index];
    unsigned const secs = (unsigned)(v->over_ms / WATCH_MS_PER_S);
    unsigned const tenths = (unsigned)((v->over_ms % WATCH_MS_PER_S) / WATCH_MS_PER_TENTH);

    /* khal_panic_print(), never KLOG: AO_Log is an Active Object and may be the one stuck. */
    if ( KQP_WATCH_STUCK == v->kind )
    {
        khal_panic_print("\r\nkodeOS: AO '%s' (prio %u) stuck %u.%u s in signal 0x%x\r\n",
                         s->name, (unsigned)s->prio, secs, tenths, (unsigned)v->sig);
        (void)snprintf(watch_reason, sizeof(watch_reason),
                       "AO stuck: %s, %u s in one event", s->name, secs);
    }
    else
    {
        khal_panic_print("\r\nkodeOS: AO '%s' (prio %u) starved %u.%u s, %u events waiting\r\n",
                         s->name, (unsigned)s->prio, secs, tenths, (unsigned)v->waiting);
        (void)snprintf(watch_reason, sizeof(watch_reason),
                       "AO starved: %s, %u s, %u events waiting",
                       s->name, secs, (unsigned)v->waiting);
    }
    khal_panic(watch_reason);
}

/** @brief One supervisor pass: sample, judge, report or warn of slow steps. */
static void watch_pass(void)
{
    /* The judge's memory between passes, zeroed at start and after a rest. */
    static KqpWatchSlot watch_slots[WATCH_SLOTS];
    /* The rests the last pass knew of. */
    static uint32_t watch_rests;
    /* Two statements, not two arguments: the flag is read before the count. */
    bool const resting = kqp_tick_resting();
    uint32_t const rests = kqp_tick_rests();
    KqpWatchVerdict v;
    uint32_t now_ms;
    uint8_t i;

    /* A stopped tick is a board asleep on purpose, and the pass after it
     * starts the streaks over: the sleep is not time an AO stood still. */

    if ( kqp_watch_rested(resting, rests, &watch_rests, watch_slots, WATCH_SLOTS) )
    {
        return;
    }

    for ( i = 0U; WATCH_SLOTS > i; ++i )
    {
        read_sample((uint8_t)(i + 1U), &s_samples[i]);
    }
    /* After the walk, so no sample is newer than the clock it is judged by. */
    now_ms = QF_aoWatchNowMs();

    v = kqp_watch_judge(watch_slots, s_samples, WATCH_SLOTS, now_ms,
                        KQP_AO_WATCH_TIMEOUT_MS);
    if ( KQP_WATCH_OK != v.kind )
    {
        report(&v);
    }
    slow_steps_log();
}

void kqp_watch_start(void)
{
    kode_status_t const st = khal_watch_start(&watch_pass, KQP_AO_WATCH_PERIOD_MS);

    if ( KODE_OK == st )
    {
        KLOG_D("kqp", "watch: supervisor up, %u ms pass, %u ms to a hang",
               (unsigned)KQP_AO_WATCH_PERIOD_MS, (unsigned)KQP_AO_WATCH_TIMEOUT_MS);
    }
    else
    {
        KLOG_W("kqp", "watch: no supervisor, status %d", (int)st);
    }
}
