/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_khal_task.c
 * @brief khal_task: a task starts with its argument, and keeps one pointer of its own.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "fakes.h"
#include "khal_system.h"

/** @brief Stack of a test task, in bytes. */
#define TEST_STACK_BYTES 8192U

/** @brief Priority of a test task. */
#define TEST_PRIO 1U

/** @brief The longest a case waits for a task, in ms. */
#define TEST_WAIT_MS 2000U

/** @brief Number of tasks the slot case starts. */
#define TEST_TASKS 2U

/** @brief What one test task finds and leaves behind. */
typedef struct {
    SemaphoreHandle_t  set;        /**< Given once the task has set its slot. */
    SemaphoreHandle_t  go;         /**< Taken by the task before it reads back. */
    SemaphoreHandle_t  done;       /**< Given when the task has recorded its results. */
    int                mark;       /**< The task's own target for the slot pointer. */
    void              *before;     /**< The slot before the task set it. */
    void              *after;      /**< The slot after both tasks had set theirs. */
    void              *self;       /**< khal_task_self() as the task saw it. */
    void              *arg_seen;   /**< The argument the task was started with. */
} probe_t;

/** @brief Fill @p p with fresh semaphores. */
static void probe_init(probe_t *p)
{
    p->set    = xSemaphoreCreateBinary();
    p->go     = xSemaphoreCreateBinary();
    p->done   = xSemaphoreCreateBinary();
    p->mark   = 0;
    p->before = (void *)p;
    p->after  = NULL;
    p->self   = NULL;
    p->arg_seen = NULL;
}

/** @brief Release @p p's semaphores. */
static void probe_free(probe_t *p)
{
    vSemaphoreDelete(p->set);
    vSemaphoreDelete(p->go);
    vSemaphoreDelete(p->done);
}

/** @brief Wait for @p s; true when it was given in time. */
static bool wait_for(SemaphoreHandle_t s)
{
    return xSemaphoreTake(s, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE;
}

/** @brief A task that records its slot before and after the other task set its own. */
static void slot_task(void *arg)
{
    probe_t *const p = (probe_t *)arg;

    p->self   = khal_task_self();
    p->before = khal_task_slot_get();
    khal_task_slot_set(&p->mark);
    (void)xSemaphoreGive(p->set);
    (void)wait_for(p->go);
    p->after = khal_task_slot_get();
    (void)xSemaphoreGive(p->done);
}

/** @brief A task that only records the argument it was given. */
static void arg_task(void *arg)
{
    probe_t *const p = (probe_t *)arg;

    p->arg_seen = arg;
    (void)xSemaphoreGive(p->done);
}

TEST_CASE("a started task runs its function with the argument", "[khal_task]")
{
    probe_t p;

    probe_init(&p);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_task_start(&arg_task, "t_arg", TEST_STACK_BYTES, TEST_PRIO, &p));
    TEST_ASSERT_TRUE(wait_for(p.done));
    TEST_ASSERT_EQUAL_PTR(&p, p.arg_seen);
    probe_free(&p);
}

TEST_CASE("a start with no function is refused", "[khal_task]")
{
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        khal_task_start(NULL, "t_null", TEST_STACK_BYTES, TEST_PRIO, NULL));
}

TEST_CASE("each task reads back the pointer it set, not the other's",
          "[khal_task]")
{
    probe_t p[TEST_TASKS];

    for ( uint32_t i = 0U; TEST_TASKS > i; ++i )
    {
        probe_init(&p[i]);
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_task_start(&slot_task, "t_slot_a", TEST_STACK_BYTES, TEST_PRIO, &p[0]));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_task_start(&slot_task, "t_slot_b", TEST_STACK_BYTES, TEST_PRIO, &p[1]));
    /* Both have set their slot before either reads. */
    TEST_ASSERT_TRUE(wait_for(p[0].set));
    TEST_ASSERT_TRUE(wait_for(p[1].set));
    (void)xSemaphoreGive(p[0].go);
    (void)xSemaphoreGive(p[1].go);
    TEST_ASSERT_TRUE(wait_for(p[0].done));
    TEST_ASSERT_TRUE(wait_for(p[1].done));

    for ( uint32_t i = 0U; TEST_TASKS > i; ++i )
    {
        TEST_ASSERT_NULL(p[i].before);
        TEST_ASSERT_EQUAL_PTR(&p[i].mark, p[i].after);
        probe_free(&p[i]);
    }
    TEST_ASSERT_NOT_EQUAL(&p[0].mark, &p[1].mark);
}

TEST_CASE("two tasks see different self handles", "[khal_task]")
{
    probe_t p[TEST_TASKS];

    for ( uint32_t i = 0U; TEST_TASKS > i; ++i )
    {
        probe_init(&p[i]);
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_task_start(&slot_task, "t_self_a", TEST_STACK_BYTES, TEST_PRIO, &p[0]));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_task_start(&slot_task, "t_self_b", TEST_STACK_BYTES, TEST_PRIO, &p[1]));
    TEST_ASSERT_TRUE(wait_for(p[0].set));
    TEST_ASSERT_TRUE(wait_for(p[1].set));
    (void)xSemaphoreGive(p[0].go);
    (void)xSemaphoreGive(p[1].go);
    TEST_ASSERT_TRUE(wait_for(p[0].done));
    TEST_ASSERT_TRUE(wait_for(p[1].done));

    TEST_ASSERT_NOT_NULL(p[0].self);
    TEST_ASSERT_NOT_NULL(p[1].self);
    TEST_ASSERT_NOT_EQUAL(p[0].self, p[1].self);
    TEST_ASSERT_NOT_EQUAL(khal_task_self(), p[0].self);
    for ( uint32_t i = 0U; TEST_TASKS > i; ++i )
    {
        probe_free(&p[i]);
    }
}

/** @brief What a slot's free function leaves for the case. */
typedef struct {
    SemaphoreHandle_t  set;        /**< Given once the task has set its owned slot. */
    SemaphoreHandle_t  go;         /**< Taken by the task before it ends. */
    volatile uint32_t  freed;      /**< Times the free function ran with this struct. */
    uint32_t           freed_early;/**< What @c freed read while the task was still alive. */
} owned_t;

/** @brief The free function: count the call on the struct the slot held. */
static void owned_free(void *p)
{
    owned_t *const o = (owned_t *)p;

    o->freed = o->freed + 1U;
}

/** @brief A task that gives its slot an owner and ends. */
static void owned_task(void *arg)
{
    owned_t *const o = (owned_t *)arg;

    fake_task_slot_watch(o);
    khal_task_slot_set_owned(o, &owned_free);
    (void)xSemaphoreGive(o->set);
    (void)wait_for(o->go);
    o->freed_early = o->freed;
}

/* The linux port runs no TLS deletion callback, so the fake runs the owner at
 * the end of the task itself (fake_khal_system.c); the board's callback is
 * the one FreeRTOS calls from prvDeleteTCB(). */
TEST_CASE("a slot with an owner is freed when its task ends, and not before",
          "[khal_task]")
{
    owned_t o = { NULL, NULL, 0U, 0xFFU };

    o.set = xSemaphoreCreateBinary();
    o.go  = xSemaphoreCreateBinary();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_task_start(&owned_task, "t_owned", TEST_STACK_BYTES, TEST_PRIO, &o));
    TEST_ASSERT_TRUE(wait_for(o.set));
    TEST_ASSERT_EQUAL_UINT32(0U, o.freed);

    (void)xSemaphoreGive(o.go);
    TEST_ASSERT_TRUE(fake_task_slot_wait_ended(TEST_WAIT_MS));
    TEST_ASSERT_EQUAL_UINT32(0U, o.freed_early);
    TEST_ASSERT_EQUAL_UINT32(1U, o.freed);
    vSemaphoreDelete(o.set);
    vSemaphoreDelete(o.go);
}
