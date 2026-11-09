/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_standard_ao.h
 * @brief The standard layer's one Active Object and its event ring. Private, never installed.
 */
#ifndef KODE_STANDARD_AO_H_
#define KODE_STANDARD_AO_H_

#include <stdbool.h>
#include <stdint.h>

#include "khal_sync.h"
#include "kode_internal.h"   /* the screen's table and its post */
#include "kqp.h"

/** @brief The event for a frame the infrared receiver heard: kode.h's KODE_EVENT_IR_RECEIVED. */
#define KODE_EVENT_IR_NAME "ir_received"

/** @brief The event for an address the radio got: kode.h's KODE_EVENT_WIFI_CONNECTED. */
#define KODE_EVENT_WIFI_UP_NAME "wifi_connected"

/** @brief The event for the address the radio lost: kode.h's KODE_EVENT_WIFI_LOST. */
#define KODE_EVENT_WIFI_LOST_NAME "wifi_lost"

/** @brief The event for a tag the NFC poller activated. */
#define KODE_EVENT_NFC_TAG "nfc_tag"

/** @brief The event for a finger that lands: kode.h's KODE_EVENT_TOUCH_PRESSED. */
#define KODE_EVENT_TOUCH_PRESSED_NAME "touch_pressed"

/** @brief The event for a finger that slides: kode.h's KODE_EVENT_TOUCH_MOVED. */
#define KODE_EVENT_TOUCH_MOVED_NAME "touch_moved"

/** @brief The event for a finger that lifts: kode.h's KODE_EVENT_TOUCH_RELEASED. */
#define KODE_EVENT_TOUCH_RELEASED_NAME "touch_released"

/** @brief The event for a phone that connected: kode.h's KODE_EVENT_BLE_CONNECTED. */
#define KODE_EVENT_BLE_CONNECTED_NAME "ble_connected"

/** @brief The event for a phone that left: kode.h's KODE_EVENT_BLE_DISCONNECTED. */
#define KODE_EVENT_BLE_DISCONNECTED_NAME "ble_disconnected"

/** @brief The event for a write to the app's service: kode.h's KODE_EVENT_BLE_WRITTEN. */
#define KODE_EVENT_BLE_WRITTEN_NAME "ble_written"

/* cppcheck-suppress misra-c2012-2.5 ; the app build of kode_standard_ao.c uses it, and the kodeOS image builds only its refusal */
/** @brief How long asking for the finger keeps trying while the Active Object's queue is full, in ms. */
#define KODE_TOUCH_LISTEN_BOUND_MS 100U

/** @brief What a J3 pin's event starts with: "pin_<n>_changed". */
#define KODE_EVENT_PIN_PREFIX "pin_"

/** @brief What a J3 pin's event ends with, after the pin's number. */
#define KODE_EVENT_PIN_SUFFIX "_changed"

/* cppcheck-suppress misra-c2012-2.5 ; the app build of kode_standard_ao.c uses it, and the kodeOS image builds only its refusal */
/** @brief How long arming a pin keeps trying while the Active Object's queue is full, in ms. */
#define KODE_PIN_LISTEN_BOUND_MS 100U

/** @brief What subscribes an Active Object to an owner's signals: AO_Touch's touch_subscribe(), ble_app_subscribe(). */
typedef void (*kode_owner_subscribe_t)(QActive const *who);

/** @brief Bytes of the dispatch task's stack (taken from PSRAM). */
#define KODE_EVENT_STACK_BYTES 8192U

/** @brief Queue slots a screen request leaves free for what the owners
 *         publish while a render pass runs (a touch every KODE_TOUCH_POLL_MS). */
#define KODE_SCREEN_POST_MARGIN KQP_AO_QUEUE_LEN

/**
 * @brief The standard layer's Active Object, running once
 *        kode_standard_ao_ensure() has answered true.
 *
 * Only built in an app image: the kodeOS image refuses the layer.
 */
extern QActive * const AO_KodeStandard;

/**
 * @brief Bring up the standard layer's Active Object and its dispatch task, once.
 *
 * Safe from any number of tasks at once (khal_once_try()): one of them does
 * the work, the others wait until it is done, and none of them returns before
 * the layer is ready. Starts the dispatch task at KODE_PRIO_STANDARD_EVENTS and
 * the Active Object at KODE_PRIO_USER_GUI, after checking that nobody else
 * holds that level. A start that fails leaves the layer as it was, and the
 * next call tries again.
 *
 * In the kodeOS image (CONFIG_KODEOS_BOOT_ROLE_OS) it starts nothing, because
 * kgui holds level 3 there.
 *
 * @return true when the layer is up. false otherwise, with the task's error
 *         set: "the standard layer is for apps, not kodeOS" in the kodeOS
 *         image, "QP level 3 is taken by another Active Object (<name>)", or
 *         what could not be had.
 */
bool kode_standard_ao_ensure(void);

/**
 * @brief Have the Active Object subscribe to the finger, once per boot.
 *
 * Until a request has been posted, each call posts one, which the Active
 * Object runs on its own thread: it calls @p subscribe for itself and from
 * then on turns every TOUCH_EVT_SIG into an event (kode_event_push_touch())
 * before the screen gets it. Once one is posted, later calls do nothing. No
 * call answers true before a request is in the queue, its own or an earlier
 * one; two at once may both post, which is harmless. Naming touch_subscribe() is what links
 * AO_Touch, so @p subscribe is passed in rather than named here: this file is
 * in every app that uses events. Call it once kode_standard_ao_ensure() has
 * answered true.
 *
 * @param subscribe touch_subscribe(); never NULL.
 * @return true when a request is posted, now or by an earlier call; false
 *         when the pool or the queue stayed full for
 *         KODE_TOUCH_LISTEN_BOUND_MS, and the next call tries again. Sets no
 *         error: the caller names what could not be heard. In the kodeOS
 *         image false, with kode_standard_ao_ensure()'s error.
 */
bool kode_standard_ao_touch_listen(kode_owner_subscribe_t subscribe);

/**
 * @brief Hand a listened J3 pin's interrupt to the Active Object: the
 *        kode_pin_arm_fn_t that kode_pin_listen() calls for the first
 *        listener of a pin.
 *
 * Runs in the caller's task, with the pins lock held and the pin already an
 * input. The first call of the image takes the fourteen pins' settle timers
 * from PSRAM and constructs them. Attaches the pin's interrupt (any edge) to
 * a static event of that pin, which the interrupt posts to the Active Object
 * and nothing else (a GPIO interrupt is an event), enables it, reads the
 * level, and posts it as the level the pin starts at, so the first change
 * sent is a change from it. The Active Object then waits KODE_PIN_SETTLE_MS
 * after each edge, re-arms the interrupt, reads the level and pushes
 * pin_<n>_changed when it differs from the last one it pushed. Call it once
 * kode_standard_ao_ensure() has answered true.
 *
 * @param pin The pin, KODE_PIN_FIRST to KODE_PIN_LAST.
 * @return true when the interrupt is armed and the Active Object told; false
 *         with the task's error set ("no memory for the pins' settle
 *         timers", "pin 5's interrupt could not be attached", or the layer
 *         busy for KODE_PIN_LISTEN_BOUND_MS). The
 *         caller lets the pin go. In the kodeOS image false, with
 *         kode_standard_ao_ensure()'s error.
 */
bool kode_standard_ao_pin_arm(uint8_t pin);

/**
 * @brief Arm the one wait for the end of a switch of J3's supply, before the
 *        switch is asked for.
 *
 * The Active Object subscribes to EXP_POWER_SIG from its start; once armed,
 * the first one that names @p on ends the wait: it keeps the outcome and
 * signals @p done. One wait at a time: kode_pin_power_3v3() calls take turns
 * before they arm. Call it once kode_standard_ao_ensure() has answered true,
 * with @p done holding no signal.
 *
 * @param on   The switch waited for: true for on.
 * @param done The waiting task's semaphore.
 */
void kode_standard_ao_power_expect(bool on, khal_sem_t done);

/**
 * @brief Wait for the end kode_standard_ao_power_expect() armed, then disarm.
 *
 * Waits up to @p ms on the semaphore it was given. When the time runs out at
 * the moment the Active Object takes the end, the end is waited for still:
 * the Active Object signals right after, so no signal is left for a later
 * wait and no end is lost.
 *
 * @param ms How long to wait; 0 disarms a wait whose request never went out.
 * @return The end's code (EXP_POWER_SIG's rc: KODE_OK, KODE_IO_ERROR or
 *         KODE_DENIED), or KODE_TIMEOUT when nothing said it in time or no
 *         wait was armed. In the kodeOS image KODE_TIMEOUT.
 */
kode_status_t kode_standard_ao_power_await(uint32_t ms);

/**
 * @brief Have the Active Object subscribe to an owner's signals, from the caller's task.
 *
 * QP's subscribe is safe from any task, so @p subscribe runs here, for the
 * Active Object, and nothing is posted. Asking again is harmless. Naming
 * ble_app_subscribe() is what links AO_Radio, so @p subscribe is passed in
 * rather than named here. Call it once kode_standard_ao_ensure() has answered
 * true. The Active Object turns BLE_APP_LINK_SIG into ble_connected and
 * ble_disconnected, and BLE_APP_WRITE_SIG into ble_written
 * (kode_event_push_bytes()).
 *
 * @param subscribe The owner's subscribe; never NULL.
 * @return true; in the kodeOS image false, with kode_standard_ao_ensure()'s error.
 */
bool kode_standard_ao_subscribe(kode_owner_subscribe_t subscribe);

/**
 * @brief Make the event ring, the listener table and the dispatch task.
 *        Called by kode_standard_ao_ensure() alone, and again after a failure.
 *
 * What an earlier try made (the block, its mutex and semaphore) is kept and
 * reused, so a try that fails leaks nothing. The layer's events go live
 * (kode_event_push() takes events) only once the dispatch task runs.
 *
 * @return true when the dispatch task is running; false with the task's
 *         error set.
 */
bool kode_event_start(void);

/**
 * @brief Queue an event for the listeners; callable from any task.
 *
 * Copies the strings into a ring entry, so the caller keeps its own and the
 * listener never points into anyone's table. The name is kept in lower case
 * and cut to KODE_EVENT_NAME_MAX, the source to KODE_EVENT_SOURCE_MAX and the
 * text to KODE_EVENT_TEXT_MAX (NUL included). Names are not checked here:
 * only the calls that take a name from the user do that. When
 * KODE_EVENT_QUEUE_DEPTH events are queued, the one under delivery included,
 * the new one is dropped and counted, and one warning names the first drop of
 * each burst (a burst ends when the ring has emptied). Does nothing before
 * the layer is up.
 *
 * @param name   The event: "button_top_pressed".
 * @param source Who: the button's name; NULL for none.
 * @param value  A number the event carries; 0 for none.
 * @param text   A payload the event carries; NULL for none.
 */
void kode_event_push(const char *name, const char *source, int32_t value, const char *text);

/**
 * @brief Queue an event whose text is @p len bytes, NULs included, as
 *        kode_event_push() queues one; its value is @p len.
 *
 * The bytes are copied whole and followed by one NUL, so a listener reads
 * them by the value and a text listener reads them up to the first NUL.
 *
 * @param name   The event: "ble_written".
 * @param source Who: the characteristic's name; NULL for none.
 * @param data   The bytes; NULL when @p len is 0.
 * @param len    Their count, at most KODE_EVENT_TEXT_MAX - 1; longer is cut.
 */
void kode_event_push_bytes(const char *name, const char *source, const uint8_t *data, size_t len);

/**
 * @brief Turn one touch sample into an event, and keep it for kode_touch_read().
 *
 * A finger that lands is touch_pressed and one that lifts touch_released,
 * each pushed as kode_event_push() does, with the point. A finger that slides
 * is touch_moved, coalesced (kode_event_ring_replace_move() in kode_event.c):
 * when the newest touch event waiting in the ring is a move, it takes the new
 * point in place, so a drag never fills the ring. A lift with no finger down
 * pushes nothing. Called by the standard layer's Active Object alone. Does
 * nothing before the layer is up.
 *
 * @param x       The point's x, in glass pixels.
 * @param y       The point's y, in glass pixels.
 * @param pressed Whether a finger is down.
 */
void kode_event_push_touch(int16_t x, int16_t y, bool pressed);

/**
 * @brief The last touch sample kode_event_push_touch() kept.
 *
 * @param x Receives the finger's x while one is down; untouched otherwise. May be NULL.
 * @param y Receives the finger's y while one is down; untouched otherwise. May be NULL.
 * @return true while a finger is down; false when none is, before any sample
 *         and before the layer is up.
 */
bool kode_event_touch_read(int16_t *x, int16_t *y);

/**
 * @brief How many events were dropped because the ring was full, since boot.
 *
 * @return The count; 0 before the layer is up.
 */
uint32_t kode_event_dropped(void);

/**
 * @brief Whether a ring entry was pushed after a wait began.
 *
 * Every entry carries the number of its push, from a counter that counts up
 * and wraps; a wait keeps the counter as it stood when it began. The
 * difference is read as signed, so the order holds across the wrap as long
 * as the two are less than 2^31 pushes apart.
 *
 * @param entry_seq The entry's number.
 * @param start     The counter when the wait began.
 * @return true when the entry came later than the start; false for the same
 *         number or an earlier one.
 */
bool kode_event_is_after(uint32_t entry_seq, uint32_t start);

#endif /* KODE_STANDARD_AO_H_ */
