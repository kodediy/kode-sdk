/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file radio_knocks.c
 * @brief AO_Radio's knocks: their events, the queue they go to and the line a refusal logs.
 */
#include "radio_knocks.h"

#include <stddef.h>

#include "kaos.h"
#include "kode_log.h"
#include "radio_knock.h"

#define TAG "kaos.radio"

/* What a knock does to AO_Radio's queue: it stops KQP_RADIO_KNOCK_MARGIN
 * slots short of the end, and one event of each knock waits at a time, so
 * together they never fill the queue and never assert on a full one. That is
 * all the margin says. The slots it leaves are not kept for anybody: the
 * requests other Active Objects post with no margin (the Wi-Fi doors and the
 * scan requests keep KQP_RADIO_POST_MARGIN of their own), the AO's posts to
 * itself, a worker's done event and the AO's time events all go past it, and
 * a full queue is still QP's assertion for every one of those but the done
 * event, which waits. The port also reads the free slots and sends in two
 * steps, so tasks that look at the same moment can each find room and
 * together go one or two slots past their margin. */

/** @brief radio_knock_door_t.post: the knock's event to AO_Radio, short of the last slots. */
static bool knocks_post(const void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the door's own is the knock's static event, which radio_knocks_reset() gives it */
    const QEvt * const evt = (const QEvt *)ctx;

    return QACTIVE_POST_X(AO_Radio, evt, KQP_RADIO_KNOCK_MARGIN, NULL);
}

/** @brief radio_knock_door_t.room: whether AO_Radio's queue would take a knock's event now. */
static bool knocks_room(const void *ctx)
{
    (void)ctx;
    return (uint16_t)KQP_RADIO_KNOCK_MARGIN < QActive_getQueueFree(AO_Radio->prio);
}

/** @brief The knocks, by radio_knock_id_t; radio_knocks_reset() gives each its door. */
static radio_knock_t s_knock[RADIO_KNOCK_COUNT];

void radio_knocks_reset(void)
{
    /* Each knock's event, and its way into AO_Radio's queue by radio_knock_id_t. */
    static const QEvt lost_evt  = QEVT_INITIALIZER(RADIO_LINK_LOST_SIG);
    static const QEvt ble_evt   = QEVT_INITIALIZER(BLE_LINK_SIG);
    static const QEvt pair_evt  = QEVT_INITIALIZER(BLE_PAIR_TAKE_SIG);
    static const QEvt heard_evt = QEVT_INITIALIZER(RADIO_HEARD_KNOCK_SIG);
    /* A report's event of no bytes: radio_hid_event() reads its length. */
    static const BleHidReportEvt hid_evt = { .super = QEVT_INITIALIZER(BLE_HID_REPORT_REQ_SIG) };
    static const radio_knock_door_t door[RADIO_KNOCK_COUNT] = {
        [RADIO_KNOCK_LOST]  = { &knocks_post, &knocks_room, &lost_evt  },
        [RADIO_KNOCK_BLE]   = { &knocks_post, &knocks_room, &ble_evt   },
        [RADIO_KNOCK_PAIR]  = { &knocks_post, &knocks_room, &pair_evt  },
        [RADIO_KNOCK_HEARD] = { &knocks_post, &knocks_room, &heard_evt },
        [RADIO_KNOCK_HID]   = { &knocks_post, &knocks_room, &hid_evt.super },
    };

    for ( uint32_t i = 0U; (uint32_t)RADIO_KNOCK_COUNT > i; ++i )
    {
        radio_knock_init(&s_knock[i], &door[i]);
    }
}

void radio_knocks_post(radio_knock_id_t id, uint8_t arg)
{
    /* What each knock is called in the log, by radio_knock_id_t. */
    static const char * const name[RADIO_KNOCK_COUNT] = {
        [RADIO_KNOCK_LOST]  = "the lost link",
        [RADIO_KNOCK_BLE]   = "the BLE state",
        [RADIO_KNOCK_PAIR]  = "the pairing note",
        [RADIO_KNOCK_HEARD] = "the BLE advertisements",
        [RADIO_KNOCK_HID]   = "the HID wake-up",
    };

    if ( !radio_knock_post(&s_knock[id], arg) )
    {
        KLOG_W(TAG, "no room for %s in AO_Radio's queue; it is posted again "
                    "once there is", name[id]);
    }
}

void radio_knocks_pay(const QEvt *e)
{
    if ( (enum_t)Q_USER_SIG <= (enum_t)e->sig )
    {
        for ( uint32_t i = 0U; (uint32_t)RADIO_KNOCK_COUNT > i; ++i )
        {
            radio_knock_pay(&s_knock[i]);
        }
    }
}

bool radio_knocks_take(radio_knock_id_t id, uint8_t *arg)
{
    return radio_knock_take(&s_knock[id], arg);
}

bool radio_knocks_pending(radio_knock_id_t id)
{
    return radio_knock_pending(&s_knock[id]);
}
