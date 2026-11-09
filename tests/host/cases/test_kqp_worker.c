/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kqp_worker.c
 * @brief kqp_worker: a job runs off the step and its answer comes back as one event.
 */
#include <stddef.h>
#include <string.h>

#include "unity.h"

#include "ao_spy.h"
#include "fake_khal_worker.h"
#include "fakes.h"
#include "kode_worker.h"
#include "khal_time.h"
#include "kqp_worker.h"
#include "qf_support.h"

/** @brief The done signal every case uses; the spy takes a post of anything. */
#define DONE_SIG IOEXP_INT_SIG

/** @brief What the job function writes as its value. */
#define JOB_VALUE 7U

/** @brief Stack of the faked worker; the fake ignores it. */
#define STACK_BYTES 4096U

/** @brief What the faked stack reads as never used, in a registry case. */
#define STACK_FREE 1234U

/** @brief Priority of the faked worker; the fake ignores it. */
#define WORKER_PRIO 4U

/** @brief Most pool blocks the exhaust helper will hold. */
#define HOLD_MAX 64U

/** @brief The first sequence number after the wrap. */
#define SEQ_LAST 0xFFFFU

/** @brief A job that counts its runs and answers with JOB_VALUE. */
static kode_status_t job_ok(void *arg, uint32_t *value)
{
    unsigned *const runs = (unsigned *)arg;

    ++(*runs);
    *value = JOB_VALUE;
    return KODE_OK;
}

/** @brief The pool blocks a case holds so the runner finds none. */
static KqpWorkerDoneEvt *s_held[HOLD_MAX];

/** @brief How many of s_held are in hand. */
static unsigned s_held_n;

/** @brief Take every pool block there is. */
static void pool_exhaust(void)
{
    KqpWorkerDoneEvt *e = Q_NEW_X(KqpWorkerDoneEvt, 0U, DONE_SIG);

    s_held_n = 0U;
    while ( (NULL != e) && (HOLD_MAX > s_held_n) )
    {
        s_held[s_held_n] = e;
        ++s_held_n;
        e = Q_NEW_X(KqpWorkerDoneEvt, 0U, DONE_SIG);
    }
}

/** @brief Give one block back: the AO ran and recycled something. */
static void pool_free_one(void)
{
    if ( 0U < s_held_n )
    {
        --s_held_n;
        QF_gc(&s_held[s_held_n]->super);
    }
}

/** @brief Give every block still in hand back. */
static void pool_free_all(void)
{
    while ( 0U < s_held_n )
    {
        pool_free_one();
    }
}

/** @brief A worker over the spy, depth @p depth, ready for jobs. */
static void worker_up(KqpWorker *w, uint8_t depth)
{
    qf_support_init();
    ao_spy_drain();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kqp_worker_start(w, ao_spy(), "test_worker", STACK_BYTES, WORKER_PRIO,
                         depth, false));
}

TEST_CASE("a job runs and its done event reaches the AO", "[kqp_worker]")
{
    KqpWorker w;
    unsigned runs = 0U;
    uint16_t seq = 0xAAAAU;

    worker_up(&w, 2U);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kqp_worker_submit(&w, &job_ok, &runs, DONE_SIG, &seq));
    TEST_ASSERT_EQUAL_UINT(0U, runs);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    TEST_ASSERT_TRUE(fake_khal_worker_run_next());
    TEST_ASSERT_EQUAL_UINT(1U, runs);
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());

    const KqpWorkerDoneEvt *e = (const KqpWorkerDoneEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(DONE_SIG, e->super.sig);
    TEST_ASSERT_EQUAL_UINT16(seq, e->seq);
    TEST_ASSERT_EQUAL_INT(KODE_OK, e->status);
    TEST_ASSERT_EQUAL_UINT32(JOB_VALUE, e->value);
    ao_spy_drain();
}

TEST_CASE("a submit to a full queue is refused and takes no sequence number",
          "[kqp_worker]")
{
    KqpWorker w;
    unsigned runs = 0U;
    uint16_t seq = 0U;

    worker_up(&w, 1U);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kqp_worker_submit(&w, &job_ok, &runs, DONE_SIG, &seq));
    TEST_ASSERT_EQUAL_UINT16(0U, seq);

    seq = 0x1234U;
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES,
        kqp_worker_submit(&w, &job_ok, &runs, DONE_SIG, &seq));
    TEST_ASSERT_EQUAL_UINT16(0x1234U, seq);
    TEST_ASSERT_EQUAL_UINT(1U, fake_khal_worker_pending());

    TEST_ASSERT_TRUE(fake_khal_worker_run_next());
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kqp_worker_submit(&w, &job_ok, &runs, DONE_SIG, &seq));
    TEST_ASSERT_EQUAL_UINT16(1U, seq);
    TEST_ASSERT_TRUE(fake_khal_worker_run_next());
    ao_spy_drain();
}

TEST_CASE("the sequence number increments and wraps", "[kqp_worker]")
{
    KqpWorker w;
    unsigned runs = 0U;
    uint16_t first = 0U;
    uint16_t second = 0U;

    worker_up(&w, 2U);
    w.next_seq = SEQ_LAST;
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kqp_worker_submit(&w, &job_ok, &runs, DONE_SIG, &first));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kqp_worker_submit(&w, &job_ok, &runs, DONE_SIG, &second));
    TEST_ASSERT_EQUAL_UINT16(SEQ_LAST, first);
    TEST_ASSERT_EQUAL_UINT16(0U, second);

    TEST_ASSERT_TRUE(fake_khal_worker_run_next());
    TEST_ASSERT_TRUE(fake_khal_worker_run_next());
    TEST_ASSERT_EQUAL_UINT16(SEQ_LAST,
        ((const KqpWorkerDoneEvt *)ao_spy_take())->seq);
    TEST_ASSERT_EQUAL_UINT16(0U,
        ((const KqpWorkerDoneEvt *)ao_spy_take())->seq);
    ao_spy_drain();
}

/** @brief Frees one held block, as the AO does while the runner sleeps. */
static void free_one_hook(void)
{
    pool_free_one();
}

TEST_CASE("the runner waits and posts again when the pool is empty",
          "[kqp_worker]")
{
    KqpWorker w;
    unsigned runs = 0U;
    uint16_t seq = 0U;

    worker_up(&w, 1U);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kqp_worker_submit(&w, &job_ok, &runs, DONE_SIG, &seq));

    pool_exhaust();
    TEST_ASSERT_GREATER_THAN_UINT(0U, s_held_n);
    fake_time_delay_hook(&free_one_hook);
    uint32_t const before = khal_time_ms();

    TEST_ASSERT_TRUE(fake_khal_worker_run_next());

    TEST_ASSERT_EQUAL_UINT32(before + 1U, khal_time_ms());
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    TEST_ASSERT_EQUAL_UINT16(seq,
        ((const KqpWorkerDoneEvt *)ao_spy_take())->seq);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    ao_spy_drain();
    pool_free_all();
}

TEST_CASE("two answers carry distinct sequence numbers the AO can tell apart",
          "[kqp_worker]")
{
    KqpWorker w;
    unsigned runs = 0U;
    uint16_t old_seq = 0U;
    uint16_t new_seq = 0U;
    unsigned accepted = 0U;

    worker_up(&w, 2U);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kqp_worker_submit(&w, &job_ok, &runs, DONE_SIG, &old_seq));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kqp_worker_submit(&w, &job_ok, &runs, DONE_SIG, &new_seq));
    TEST_ASSERT_NOT_EQUAL_UINT16(old_seq, new_seq);

    TEST_ASSERT_TRUE(fake_khal_worker_run_next());
    TEST_ASSERT_TRUE(fake_khal_worker_run_next());
    TEST_ASSERT_EQUAL_UINT(2U, ao_spy_count());

    /* The AO waits for the newer job only, as it does after a timeout. */
    for ( unsigned i = 0U; 2U > i; ++i )
    {
        const KqpWorkerDoneEvt *e = (const KqpWorkerDoneEvt *)ao_spy_take();

        if ( e->seq == new_seq )
        {
            ++accepted;
        }
    }
    TEST_ASSERT_EQUAL_UINT(1U, accepted);
    ao_spy_drain();
}

TEST_CASE("kode_worker_start puts the stack in PSRAM", "[kqp_worker]")
{
    kode_worker_t w;

    qf_support_init();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kode_worker_start(&w, ao_spy(), "test_worker", STACK_BYTES,
                          WORKER_PRIO, 1U));
    TEST_ASSERT_TRUE(fake_khal_worker_last_psram());
}

TEST_CASE("kqp_worker_start keeps the stack where it is told", "[kqp_worker]")
{
    KqpWorker w;

    worker_up(&w, 1U);
    TEST_ASSERT_FALSE(fake_khal_worker_last_psram());
    TEST_ASSERT_FALSE(kqp_worker_is_self(&w));
}

TEST_CASE("a worker refuses NULL arguments", "[kqp_worker]")
{
    KqpWorker w;
    unsigned runs = 0U;

    worker_up(&w, 1U);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kqp_worker_start(NULL, ao_spy(), "x", STACK_BYTES, WORKER_PRIO, 1U,
                         false));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kqp_worker_start(&w, NULL, "x", STACK_BYTES, WORKER_PRIO, 1U, false));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kqp_worker_submit(&w, NULL, &runs, DONE_SIG, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kqp_worker_submit(NULL, &job_ok, &runs, DONE_SIG, NULL));
}

/** @brief The registry entry called @p name, or false. */
static bool listed(char const *name, KqpWorkerInfo *out)
{
    for ( uint8_t i = 0U; kqp_worker_count() > i; ++i )
    {
        if ( kqp_worker_info(i, out) && (strcmp(out->name, name) == 0) )
        {
            return true;
        }
    }
    return false;
}

TEST_CASE("a started worker is listed with its stack and what is left of it",
          "[kqp_worker]")
{
    KqpWorker w;
    KqpWorkerInfo info;

    qf_support_init();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kqp_worker_start(&w, ao_spy(), "listed_worker", STACK_BYTES,
                         WORKER_PRIO, 1U, true));
    fake_khal_worker_set_stack_free_for("listed_worker", STACK_FREE);

    TEST_ASSERT_TRUE(listed("listed_worker", &info));
    TEST_ASSERT_EQUAL_UINT32(STACK_BYTES, info.stack_bytes);
    TEST_ASSERT_EQUAL_UINT32(STACK_FREE, info.free_bytes);

    TEST_ASSERT_FALSE(kqp_worker_info(kqp_worker_count(), &info));
    TEST_ASSERT_FALSE(kqp_worker_info(0U, NULL));
}

TEST_CASE("starting a worker again under its name takes its entry over",
          "[kqp_worker]")
{
    KqpWorker w;
    KqpWorkerInfo info;

    qf_support_init();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kqp_worker_start(&w, ao_spy(), "again_worker", STACK_BYTES,
                         WORKER_PRIO, 1U, true));
    uint8_t const before = kqp_worker_count();

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kqp_worker_start(&w, ao_spy(), "again_worker", STACK_BYTES * 2U,
                         WORKER_PRIO, 1U, true));
    TEST_ASSERT_EQUAL_UINT8(before, kqp_worker_count());
    TEST_ASSERT_TRUE(listed("again_worker", &info));
    TEST_ASSERT_EQUAL_UINT32(STACK_BYTES * 2U, info.stack_bytes);
}

TEST_CASE("a start that fails leaves no worker to submit to and lists none",
          "[kqp_worker]")
{
    KqpWorker w;
    KqpWorkerInfo info;
    unsigned runs = 0U;

    qf_support_init();
    w.w = (khal_worker_t)&w;   /* stale: a previous start's */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kqp_worker_start(&w, ao_spy(), "failed_worker", STACK_BYTES,
                         WORKER_PRIO, 0U, true));
    TEST_ASSERT_NULL(w.w);
    TEST_ASSERT_FALSE(listed("failed_worker", &info));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kqp_worker_submit(&w, &job_ok, &runs, DONE_SIG, NULL));
}
