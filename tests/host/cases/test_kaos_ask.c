/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kaos_ask.c
 * @brief The shared door: one per asker and kind, shut by an ask, opened by its answer.
 */
#include <stdbool.h>
#include <string.h>

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "kaos_ask.h"
#include "kqp.h"
#include "qf_support.h"

/** @brief A static event, to fill a queue with. */
static const QEvt s_filler = QEVT_INITIALIZER(IMU_TEST_REQ_SIG);

/** @brief Fresh doors for each case. */
static kaos_ask_door_t s_door;

/**
 * @brief Whether @p who's door is open: a take that succeeds, given back at once.
 * @param who The asking AO.
 * @return true when it could ask.
 */
static bool door_open(QActive const *who)
{
    bool const open = kaos_ask_take(&s_door, who) == KODE_OK;

    if ( open )
    {
        kaos_ask_release(&s_door, who);
    }
    return open;
}

TEST_CASE("an ask shuts its asker's door and nobody else's", "[kaos_ask]")
{
    (void)memset(&s_door, 0, sizeof(s_door));
    ao_spy_drain();
    ao_spy_other_drain();

    TEST_ASSERT_TRUE(door_open(ao_spy()));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kaos_ask_take(&s_door, ao_spy()));
    TEST_ASSERT_FALSE(door_open(ao_spy()));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kaos_ask_take(&s_door, ao_spy()),
                                  "one ask of a kind in flight per asker");
    TEST_ASSERT_TRUE(door_open(ao_spy_other()));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kaos_ask_take(&s_door, ao_spy_other()));

    kaos_ask_release(&s_door, ao_spy());
    TEST_ASSERT_TRUE(door_open(ao_spy()));
    TEST_ASSERT_FALSE(door_open(ao_spy_other()));
    kaos_ask_release(&s_door, ao_spy_other());
}

TEST_CASE("a plain task or an unstarted AO has no door", "[kaos_ask]")
{
    QActive unstarted;

    (void)memset(&s_door, 0, sizeof(s_door));
    (void)memset(&unstarted, 0, sizeof(unstarted));

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kaos_ask_take(&s_door, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kaos_ask_take(&s_door, &unstarted));
}

TEST_CASE("a cancel gives the answer back and opens the door", "[kaos_ask]")
{
    StatusRspEvt *a;

    (void)memset(&s_door, 0, sizeof(s_door));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kaos_ask_take(&s_door, ao_spy()));
    a = Q_NEW_X(StatusRspEvt, 0U, IR_SEND_RAW_RSP_SIG);
    TEST_ASSERT_NOT_NULL(a);

    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kaos_ask_cancel(&s_door, ao_spy(), &a->super));
    TEST_ASSERT_TRUE(door_open(ao_spy()));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kaos_ask_cancel(&s_door, ao_spy(), NULL));
}

TEST_CASE("a post into a full owner is refused and opens the door", "[kaos_ask]")
{
    StatusRspEvt *req;
    StatusRspEvt *a;

    (void)memset(&s_door, 0, sizeof(s_door));
    ao_spy_drain();
    ao_spy_other_drain();
    TEST_ASSERT_TRUE(qf_support_queue_stuff(ao_spy_other(), &s_filler));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kaos_ask_take(&s_door, ao_spy()));
    req = Q_NEW_X(StatusRspEvt, 0U, IR_SEND_RAW_REQ_SIG);
    a   = Q_NEW_X(StatusRspEvt, 0U, IR_SEND_RAW_RSP_SIG);
    TEST_ASSERT_NOT_NULL(req);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES,
                          kaos_ask_post(&s_door, ao_spy(), ao_spy_other(), &req->super,
                                        &a->super, 1U));
    TEST_ASSERT_TRUE(door_open(ao_spy()));
    ao_spy_other_drain();
}

TEST_CASE("an answer opens the door and reaches the asker", "[kaos_ask]")
{
    StatusRspEvt *a;
    const QEvt   *e;

    (void)memset(&s_door, 0, sizeof(s_door));
    ao_spy_drain();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kaos_ask_take(&s_door, ao_spy()));
    a = Q_NEW_X(StatusRspEvt, 0U, IR_SEND_RAW_RSP_SIG);
    TEST_ASSERT_NOT_NULL(a);
    a->rc = (uint8_t)KODE_OK;

    kaos_ask_answer(&s_door, ao_spy(), &a->super, ao_spy_other());
    TEST_ASSERT_TRUE(door_open(ao_spy()));
    e = ao_spy_take();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)IR_SEND_RAW_RSP_SIG, (uint32_t)e->sig);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    ao_spy_drain();
}
