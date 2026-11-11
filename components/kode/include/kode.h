/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode.h
 * @brief The standard layer: blocking calls for an app's own tasks.
 */
#ifndef KODE_H_
#define KODE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_colors.h"
#include "kode_defs.h"
#include "kode_init.h"
#include "kapp_desc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KODE_VERSION_MAJOR 4U /**< Major version of the kode API. */
#define KODE_VERSION_MINOR 0U /**< Minor version of the kode API. */

/** @brief A timeout with no limit, for the calls that wait for something. */
#define KODE_FOREVER UINT32_MAX

/** @brief Marks a printf-style call so the compiler checks its arguments. */
#if defined(__GNUC__)
#define KODE_PRINTF_FORMAT(fmt_, args_) __attribute__((format(printf, fmt_, args_)))
#else
#define KODE_PRINTF_FORMAT(fmt_, args_)
#endif

/**
 * @brief Why this task's last kode.h call failed.
 *
 * A failed call sets it, a successful one clears it. Valid until the task's
 * next kode.h call.
 *
 * kode.h is for tasks: a call from an ISR or an Active Object is refused, and this says why.
 *
 * @return Short English text; "" when there is none. Never NULL.
 */
const char *kode_error(void);

/* ---- System ---- */

/**
 * @brief Sleep this task.
 * @param ms Milliseconds; under one tick (1 ms) only yields. KODE_FOREVER is
 *           about 49.7 days, not forever.
 */
void kode_delay(uint32_t ms);

/**
 * @brief Milliseconds since boot.
 * @return The count; it wraps after about 49 days, so subtract readings as
 *         uint32_t. 0 when refused.
 */
uint32_t kode_uptime_ms(void);

/**
 * @brief The wall clock, in Unix seconds UTC.
 *
 * Set by SNTP, by `time set`, or restored after a power cut (then it may be
 * behind). kode_time_local() reads it in the board's zone.
 *
 * @return Seconds since 1970-01-01 UTC; -1 while the clock is not set or when
 *         refused.
 */
int64_t kode_time_unix(void);

/** @brief A moment of local time, broken down. */
typedef struct {
    uint16_t year;     /**< The year itself: 2026.                       */
    uint8_t  month;    /**< 1 (January) to 12.                           */
    uint8_t  day;      /**< The day of the month, 1 to 31.               */
    uint8_t  hour;     /**< 0 to 23.                                     */
    uint8_t  minute;   /**< 0 to 59.                                     */
    uint8_t  second;   /**< 0 to 59, 60 for a leap second.               */
    uint8_t  weekday;  /**< 0 = Sunday to 6 = Saturday.                  */
} kode_time_t;

/**
 * @brief The wall clock as local time, in the board's zone.
 *
 * The zone is the setting `config key="zone"`, a POSIX TZ string such as
 * "CET-1CEST,M3.5.0,M10.5.0/3", kept by the system for every app; UTC while
 * none is stored. A change of zone applies at once, and a reading is wholly
 * one zone's.
 *
 * @param[out] out Filled on true only.
 * @return true with @p out filled; false while the clock is not set, for a
 *         NULL @p out, or when refused.
 */
bool kode_time_local(kode_time_t *out);

/**
 * @brief This board's serial, such as "KODE78YR5EEV0XS".
 * @return Owned by kodeOS, valid for the firmware's life; "" when the MAC is
 *         unreadable or when refused. Never NULL.
 */
const char *kode_device_id(void);

/** @brief End the app and go back to kodeOS. Does not return, unless refused. */
void kode_exit(void);

#define KODE_PRINT_MAX 200U  /**< Bytes of one kode_print() text, NUL included. */

/**
 * @brief Print one line on the console, as printf() formats it.
 *
 * The line goes out with kodeOS's own log as `!N <ms> app <text>`, and it is
 * shown whatever `config key="log"` filters, unless the log is off. A text
 * longer than KODE_PRINT_MAX - 1 bytes is cut there; no newline is needed.
 *
 * @param fmt printf()'s format; NULL prints nothing and kode_error() says so.
 */
void kode_print(const char *fmt, ...) KODE_PRINTF_FORMAT(1, 2);

/**
 * @brief A random number from the chip's hardware random generator.
 *
 * Every number from @p min to @p max, both included, is equally likely.
 *
 * @param min The smallest number it may answer.
 * @param max The largest number it may answer; not below @p min.
 * @return The number; @p min when @p max is below it (kode_error() says so)
 *         or when refused.
 */
uint32_t kode_random(uint32_t min, uint32_t max);

/* ---- Buttons ---- */

/*
 * Names: "up", "down", "left", "right", "top", "bottom", any case, no spaces.
 * An unknown name or NULL fails and kode_error() names it.
 */

/** @brief Which buttons are down, from one reading. */
typedef struct {
    bool up;     /**< The pad's up button. */
    bool down;   /**< The pad's down button. */
    bool left;   /**< The pad's left button. */
    bool right;  /**< The pad's right button. */
    bool top;    /**< The side button at the top. */
    bool bottom; /**< The side button at the bottom. */
} kode_buttons_t;

/**
 * @brief Whether a button is down now.
 * @param name The button, such as "top".
 * @return true while held; false otherwise, on a bad name or when refused.
 */
bool kode_button_is_pressed(const char *name);

/**
 * @brief Whether a button was pressed since the last ask.
 *
 * Shares one count per button with kode_button_get_presses() and with every
 * task: asking either consumes the news. The first ask counts from boot.
 *
 * @param name The button, such as "top".
 * @return true after at least one press; false otherwise, on a bad name or
 *         when refused.
 */
bool kode_button_was_pressed(const char *name);

/**
 * @brief How many presses since the last ask; the count then restarts.
 *
 * The same shared count as kode_button_was_pressed().
 *
 * @param name The button, such as "top".
 * @return The presses; 0 on a bad name or when refused (kode_error() tells
 *         them from no press).
 */
uint32_t kode_button_get_presses(const char *name);

/**
 * @brief All six buttons from one reading.
 * @return Their states; all false when refused.
 */
kode_buttons_t kode_button_read_all(void);

/* ---- Events ---- */

/*
 * Events, any case: button_<name>_pressed, button_<name>_released, and
 * button_<name>_held for top and bottom; the source is the button's name.
 * Also wifi_connected and wifi_lost; the source is the network's name. A board
 * already online when the layer starts counts as connected: no event for that.
 * Also mqtt_message: the source is the topic, the text the payload, the value
 * its length in bytes.
 * Also battery_low: the value is the level, in percent, as it fell below
 * KODE_BATTERY_LOW_PCT.
 * ir_received: source "NEC", value the command, text the address "0x0004"; "unknown" for any other frame.
 * A listener or a waiter for it arms the receiver, once the C5 link is up.
 * nfc_tag: source the card, text the UID; also on the app's own NFC reads, with or without kode_nfc_scan_start().
 * touch_pressed, touch_moved and touch_released: the finger's point in x and
 * y (see the touch calls); every other event's point is 0, 0.
 * pin_<n>_changed, n = 1..14: the value is the pin's new level, 0 or 1. A
 * listener or a waiter for it makes pin n an input with no pull and attaches
 * its interrupt; each change is read KODE_PIN_SETTLE_MS after its edge, so a
 * bounce is one event at the level it settled on, and an edge that settles
 * back at the level last sent sends nothing. While anything listens, the pin
 * stays an input: kode_pin_write(), kode_pin_write_pwm(), kode_pin_read_analog()
 * and a bus's begin on it fail with "pin N is listened to". The last listener
 * removed, or the last wait over, lets the pin go to high impedance. A pin
 * lent to a bus cannot be listened to.
 * The first event call starts the layer's own Active Object at QP level
 * KODE_PRIO_USER_GUI (3), which an app's own AO then cannot take. Refused in
 * the kodeOS image.
 *
 * ble_connected and ble_disconnected: a phone connected or left; any phone,
 * the console's included. ble_written: a phone wrote to the app's own
 * service (KODE_BLE_SERVICE); the source is the characteristic's name, the
 * value how many bytes, and the text the bytes themselves, NULs included,
 * followed by one NUL. ble_taken: a phone's read took a value from a
 * KODE_BLE_QUEUED characteristic; the source is its name, the value how many
 * are still queued. A listener or a waiter for any of the four has the radio
 * send them.
 *
 * An event comes only from an owner the app links, and an app links an owner
 * by naming one of its calls. Ten events have an owner no other call need
 * link: ir_received (the infrared), wifi_connected and wifi_lost (the radio,
 * which joins a remembered network by itself), the three touch events (the
 * touch panel) and the four Bluetooth events (the radio). Name those with
 * KODE_EVENT_IR_RECEIVED, KODE_EVENT_WIFI_CONNECTED, KODE_EVENT_WIFI_LOST, the
 * KODE_EVENT_TOUCH_ constants and the KODE_EVENT_BLE_ constants, which link
 * it. Typed as a string in an app that links no such owner, the event is
 * refused and kode_error() names the constant.
 */

/* cppcheck-suppress misra-c2012-2.5 ; a name for the apps that listen, never used inside the SDK */
/** @brief "ir_received", naming the infrared so the app links it. */
#define KODE_EVENT_IR_RECEIVED    kode_event_ir_received()
/* cppcheck-suppress misra-c2012-2.5 ; a name for the apps that listen, never used inside the SDK */
/** @brief "wifi_connected", naming the radio so the app links it. */
#define KODE_EVENT_WIFI_CONNECTED kode_event_wifi_connected()
/* cppcheck-suppress misra-c2012-2.5 ; a name for the apps that listen, never used inside the SDK */
/** @brief "wifi_lost", naming the radio so the app links it. */
#define KODE_EVENT_WIFI_LOST      kode_event_wifi_lost()

/* cppcheck-suppress misra-c2012-2.5 ; a name for the apps that listen, never used inside the SDK */
/** @brief "ble_connected", naming the radio so the app links it. */
#define KODE_EVENT_BLE_CONNECTED    kode_event_ble_connected()
/* cppcheck-suppress misra-c2012-2.5 ; a name for the apps that listen, never used inside the SDK */
/** @brief "ble_disconnected", naming the radio so the app links it. */
#define KODE_EVENT_BLE_DISCONNECTED kode_event_ble_disconnected()
/* cppcheck-suppress misra-c2012-2.5 ; a name for the apps that listen, never used inside the SDK */
/** @brief "ble_written", naming the radio so the app links it. */
#define KODE_EVENT_BLE_WRITTEN      kode_event_ble_written()
/* cppcheck-suppress misra-c2012-2.5 ; a name for the apps that listen, never used inside the SDK */
/** @brief "ble_taken", naming the radio so the app links it. */
#define KODE_EVENT_BLE_TAKEN        kode_event_ble_taken()

/**
 * @brief The radio's event name for a phone that connected; what KODE_EVENT_BLE_CONNECTED calls.
 * @return "ble_connected".
 */
const char *kode_event_ble_connected(void);

/**
 * @brief The radio's event name for a phone that left; what KODE_EVENT_BLE_DISCONNECTED calls.
 * @return "ble_disconnected".
 */
const char *kode_event_ble_disconnected(void);

/**
 * @brief The radio's event name for a write to the app's service; what KODE_EVENT_BLE_WRITTEN calls.
 * @return "ble_written".
 */
const char *kode_event_ble_written(void);

/**
 * @brief The radio's event name for a read that took a queued value; what KODE_EVENT_BLE_TAKEN calls.
 * @return "ble_taken".
 */
const char *kode_event_ble_taken(void);

/**
 * @brief The infrared's event name; what KODE_EVENT_IR_RECEIVED calls.
 * @return "ir_received".
 */
const char *kode_event_ir_received(void);

/**
 * @brief The radio's event name for an address got; what KODE_EVENT_WIFI_CONNECTED calls.
 * @return "wifi_connected".
 */
const char *kode_event_wifi_connected(void);

/**
 * @brief The radio's event name for the address lost; what KODE_EVENT_WIFI_LOST calls.
 * @return "wifi_lost".
 */
const char *kode_event_wifi_lost(void);

#define KODE_LISTENERS_MAX     32U  /**< Listeners at once, across all events. */
#define KODE_EVENT_QUEUE_DEPTH 16U  /**< Events queued for the listeners; past it they drop. */
#define KODE_EVENT_NAME_MAX    32U  /**< Bytes of an event name, NUL included. */
#define KODE_EVENT_SOURCE_MAX  80U  /**< Bytes of a source, NUL included; longer is cut. */
#define KODE_EVENT_TEXT_MAX    256U /**< Bytes of a text, NUL included: one MQTT payload. */

/** @brief An event, as a listener or a waiter receives it. */
typedef struct {
    const char *name;   /**< Such as "button_top_pressed". */
    const char *source; /**< Who sent it; "" when nobody. */
    int32_t     value;  /**< Its number; 0 when it has none. */
    const char *text;   /**< Its payload; "" when it has none. */
    int16_t     x;      /**< A touch event's x, in glass pixels; 0 for every other event. */
    int16_t     y;      /**< A touch event's y, in glass pixels; 0 for every other event. */
} kode_event_t;

/**
 * @brief A listener.
 *
 * Runs in kodeOS's listener task, one at a time, in event order. Any kode.h
 * call but kode_event_wait() is allowed. The strings live until it returns.
 *
 * @param e The event.
 */
typedef void (*kode_listener_t)(const kode_event_t *e);

/**
 * @brief Call @p fn every time the event happens.
 *
 * Listeners run in the order they were added; adding the same pair twice
 * registers it once.
 *
 * @param event The event's name.
 * @param fn    The listener.
 * @return true when registered; false on a bad name, a NULL @p fn, a full
 *         table, level 3 taken, or when refused.
 */
bool kode_event_add_listener(const char *event, kode_listener_t fn);

/**
 * @brief Stop calling @p fn for the event; a call already running finishes.
 * @param event The event's name, as added.
 * @param fn    The listener, as added.
 * @return true when it was removed; false when it was not registered, on a
 *         NULL argument or when refused.
 */
bool kode_event_remove_listener(const char *event, kode_listener_t fn);

/**
 * @brief Wait for the event.
 *
 * Only an event pushed after the wait began counts. Every waiting task gets
 * it. Refused inside a listener. Never delete a task while it waits.
 *
 * @param event The event's name.
 * @param ms    Milliseconds, or KODE_FOREVER; 0 returns false at once.
 * @param out   Receives the event, its strings valid until this task's next
 *              wait; may be NULL.
 * @return true when it happened; false on timeout, a bad name, level 3 taken,
 *         or when refused.
 */
bool kode_event_wait(const char *event, uint32_t ms, kode_event_t *out);

/* ---- Colours and the LED ---- */

/**
 * @brief A colour from red, green and blue.
 * @param r Red, 0 to 255.
 * @param g Green, 0 to 255.
 * @param b Blue, 0 to 255.
 * @return 0xRRGGBB.
 */
kode_color_t kode_rgb(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Light the LED; waits up to 100 ms for the owner to take it.
 * @param c A colour such as KODE_BLUE; the top byte is ignored.
 * @return true when taken; false when not or when refused.
 */
bool kode_led_set_color(kode_color_t c);

/**
 * @brief kode_led_set_color(kode_rgb(r, g, b)).
 * @param r Red, 0 to 255.
 * @param g Green, 0 to 255.
 * @param b Blue, 0 to 255.
 * @return As kode_led_set_color().
 */
bool kode_led_set_rgb(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Switch the LED off; waits like kode_led_set_color().
 * @return As kode_led_set_color().
 */
bool kode_led_off(void);

/**
 * @brief Flip the LED: on if it is off, off if it is on; waits like kode_led_set_color().
 * @return As kode_led_set_color().
 */
bool kode_led_toggle(void);

/* ---- Files ---- */

/*
 * Paths: "sd" or "flash", then nothing or "/" and the rest: "sd/log.txt",
 * "flash/data/a.csv". Any case for the volume; no leading "/", no empty, "."
 * or ".." part. A bad path fails and kode_error() names it. A file call waits
 * for another task's file call to finish.
 */

#define KODE_FILE_PATH_MAX 91U      /**< Bytes of a path, NUL included. */
#define KODE_FILE_NAME_MAX 64U      /**< Bytes of one name in a listing, NUL included. */
#define KODE_FILE_LIST_MAX 24U      /**< Entries one listing carries. */
#define KODE_FILE_READ_MAX 4194304U /**< Largest file a read returns: 4 MiB. */
#define KODE_FILE_BUSY_MS  5000U    /**< How long each storage request waits if storage is busy. */

/** @brief What one path is. */
typedef struct {
    bool     exists;     /**< Something is at the path. */
    bool     is_folder;  /**< It is a folder; false for a file or for nothing. */
    uint32_t size;       /**< Bytes of a file; 0 for a folder or for nothing. */
} kode_file_status_t;

/** @brief One entry of a listing. */
typedef struct {
    char     name[KODE_FILE_NAME_MAX];  /**< Its name, without the folder. */
    bool     is_folder;                 /**< A folder rather than a file. */
    uint32_t size;                      /**< Bytes of a file; 0 for a folder. */
} kode_file_entry_t;

/** @brief What a listing carries. */
typedef struct {
    uint32_t          count;                       /**< Entries in use. */
    bool              more;                        /**< The folder holds more than were listed. */
    kode_file_entry_t entries[KODE_FILE_LIST_MAX]; /**< The entries, in folder order. */
} kode_file_list_t;

/**
 * @brief Read a whole file as text.
 *
 * Blocks. A file over KODE_FILE_READ_MAX fails. A NUL is added after the
 * last byte; a NUL inside the file ends the text early (kode_file_read_bytes()
 * has every byte).
 *
 * @param path "sd/..." or "flash/...".
 * @return The text, valid until this task's next kode_file_read(); "" on
 *         failure or for an empty file (kode_error() tells them apart).
 */
const char *kode_file_read(const char *path);

/**
 * @brief Read a whole file as bytes.
 *
 * Blocks. A file over KODE_FILE_READ_MAX fails.
 *
 * @param path "sd/..." or "flash/...".
 * @param len  Out: the bytes read; 0 on failure. May be NULL.
 * @return The bytes, valid until this task's next kode_file_read_bytes();
 *         NULL on failure.
 */
const uint8_t *kode_file_read_bytes(const char *path, uint32_t *len);

/**
 * @brief Read up to @p cap bytes of a file, starting at @p offset.
 *
 * Blocks. The file can be of any size: KODE_FILE_READ_MAX does not limit it.
 * The bytes land in @p buf; the task keeps nothing. A read that ends past the
 * end of the file gives the bytes there are. A read at or past the end, of an
 * empty file, or of 0 bytes gives none and is not an error. A file that is
 * shorter than it was when the read began fails. An Active Object reads with
 * kode_sys_file_read().
 *
 * @param path   "sd/..." or "flash/...".
 * @param offset The first byte to read.
 * @param buf    Where the bytes go: @p cap bytes of room. NULL is refused.
 * @param cap    The most bytes to read.
 * @param got    Out: the bytes read, fewer than @p cap only at the end of the
 *               file; 0 on failure. NULL is refused.
 * @return true when the read was done, even if it gave nothing; false
 *         otherwise, with kode_error() saying why (no such file, a folder, a
 *         bad path, a refused @p buf or @p got).
 */
bool kode_file_read_at(const char *path, uint32_t offset, void *buf, uint32_t cap, uint32_t *got);

/**
 * @brief Create a file with @p text, or replace what it held.
 *
 * Blocks. "" empties the file. The folder must already exist. The text goes
 * in parts: a failure after the first leaves the file cut, not as it was.
 *
 * @param path "sd/..." or "flash/...".
 * @param text The text to write.
 * @return true when all of it is written; false otherwise, with
 *         kode_error() saying how far it got.
 */
bool kode_file_write(const char *path, const char *text);

/**
 * @brief Add @p text at the end of a file, creating it if it is not there.
 *
 * Blocks.
 *
 * @param path "sd/..." or "flash/...".
 * @param text The text to add.
 * @return As kode_file_write().
 */
bool kode_file_append(const char *path, const char *text);

/**
 * @brief Create a file with @p len bytes, or replace what it held.
 *
 * Blocks. A 0 inside @p data is written like any byte. The folder must
 * already exist. @p len is any size the volume can hold; the bytes go in
 * parts, so a failure after the first leaves the file cut, not as it was. A
 * @p len of 0 with a @p data that is not NULL empties the file. An Active
 * Object writes with kode_sys_file_write().
 *
 * @param path "sd/..." or "flash/...".
 * @param data The bytes. NULL is refused, whatever @p len is.
 * @param len  How many.
 * @return true when all of it is written; false otherwise, with
 *         kode_error() saying how far it got.
 */
bool kode_file_write_bytes(const char *path, const void *data, uint32_t len);

/**
 * @brief Add @p len bytes at the end of a file, creating it if it is not there.
 *
 * Blocks. As kode_file_write_bytes(), but what the file held stays.
 *
 * @param path "sd/..." or "flash/...".
 * @param data The bytes. NULL is refused, whatever @p len is.
 * @param len  How many.
 * @return As kode_file_write_bytes().
 */
bool kode_file_append_bytes(const char *path, const void *data, uint32_t len);

/**
 * @brief What a path is.
 *
 * Blocks. A path with nothing at it is an answer, not an error.
 *
 * @param path "sd/..." or "flash/...".
 * @return The status; all false and 0 on failure (kode_error() says why).
 */
kode_file_status_t kode_file_status(const char *path);

/**
 * @brief How big a volume is and how much of it is free.
 *
 * Blocks. On a large card the first count can take hundreds of milliseconds.
 * An Active Object asks with kode_sys_file_space().
 *
 * @param volume       "sd" or "flash", with or without a "/" after it; a path
 *                     inside a volume is refused.
 * @param total_bytes  Out: what the volume's filesystem spans; 0 on failure.
 *                     NULL is refused. A whole number of kilobytes.
 * @param free_bytes   Out: what is left of it; 0 on failure. NULL is refused.
 *                     A whole number of kilobytes.
 * @return true when the volume was counted; false otherwise, with
 *         kode_error() saying why (a bad volume, no card, storage busy or
 *         silent).
 */
bool kode_file_space(const char *volume, uint64_t *total_bytes, uint64_t *free_bytes);

/**
 * @brief Whether anything is at a path.
 *
 * Blocks.
 *
 * @param path "sd/..." or "flash/...".
 * @return true for a file or a folder; false for nothing, with kode_error()
 *         "" for nothing and the reason for a failure.
 */
bool kode_file_exists(const char *path);

/**
 * @brief List one folder: its own entries, one level.
 *
 * Blocks. KODE_FILE_LIST_MAX entries at most; `more` says there were others.
 *
 * @param folder "sd", "flash" or "sd/...", "flash/..." naming a folder.
 * @return The listing, valid until this task's next kode_file_list(); never
 *         NULL: empty on failure, with kode_error() saying why.
 */
const kode_file_list_t *kode_file_list(const char *folder);

/**
 * @brief Delete a file, or a folder with nothing in it.
 *
 * Blocks. "sd" and "flash" themselves are refused.
 *
 * @param path "sd/..." or "flash/...".
 * @return true when it is gone; false otherwise, with kode_error() saying why.
 */
bool kode_file_delete(const char *path);

/**
 * @brief Make one folder; the folder above must already exist.
 *
 * Blocks. A folder already there counts as made.
 *
 * @param path "sd/..." or "flash/...".
 * @return true when the folder is there; false otherwise, with kode_error()
 *         saying why.
 */
bool kode_file_create_folder(const char *path);

/**
 * @brief Rename a file or folder where it is.
 *
 * Blocks. A volume itself is refused.
 *
 * @param path     "sd/..." or "flash/...".
 * @param new_name One name: not empty, not "." or "..", no '/'.
 * @return true when renamed; false otherwise, with kode_error() saying why.
 */
bool kode_file_rename(const char *path, const char *new_name);

/**
 * @brief Move a file or folder to a new full path.
 *
 * Blocks. Inside a volume a folder moves too; between sd/ and flash/ only a
 * file does, copied and then deleted, and a failed copy leaves the original.
 * The folder above @p to must exist; a path already taken is refused.
 *
 * @param from "sd/..." or "flash/...".
 * @param to   The new full path.
 * @return true when moved; false otherwise, with kode_error() saying why.
 */
bool kode_file_move(const char *from, const char *to);

/* ---- IMU and magnetometer ---- */

/* Axes are the parts' own: QMI8658A for accel and gyro, QMC5883P for the field.
 * A failed read answers NAN and kode_error() says why. All four block: a read
 * takes its turn behind other tasks' reads, keeps trying for KODE_MOTION_BUSY_MS,
 * and its last try can take 600 ms more. */

#define KODE_MOTION_BUSY_MS 1000U  /**< How long a read keeps trying once its turn comes. */

/** @brief The IMU's seven readings. */
typedef struct {
    float accel_x;  /**< Acceleration on x, g.               */
    float accel_y;  /**< Acceleration on y, g.               */
    float accel_z;  /**< Acceleration on z, g.               */
    float gyro_x;   /**< Rotation about x, degrees a second. */
    float gyro_y;   /**< Rotation about y, degrees a second. */
    float gyro_z;   /**< Rotation about z, degrees a second. */
    float temp;     /**< Die temperature, degrees C.         */
} kode_imu_t;

/** @brief The magnetic field on the part's three axes. */
typedef struct {
    float x;  /**< Field on x, microtesla. */
    float y;  /**< Field on y, microtesla. */
    float z;  /**< Field on z, microtesla. */
} kode_mag_t;

/**
 * @brief One IMU reading by name.
 * @param axis "accel_x", "accel_y", "accel_z", "gyro_x", "gyro_y", "gyro_z" or "temp", any case.
 * @return The reading; NAN on a bad name, a failure or a refusal.
 */
float kode_imu_read_axis(const char *axis);

/**
 * @brief All seven IMU readings from one sample.
 * @return The readings; every field NAN on failure.
 */
kode_imu_t kode_imu_read(void);

/**
 * @brief One magnetometer axis by name.
 * @param axis "x", "y" or "z", any case.
 * @return The field, microtesla; NAN on a bad name, a failure or a refusal.
 */
float kode_mag_read_axis(const char *axis);

/**
 * @brief The three field axes from one reading.
 * @return The field; every field NAN on failure.
 */
kode_mag_t kode_mag_read(void);

/* ---- Compass ---- */

/* The part's own field and the board's are in every reading, so a raw one is
 * no compass. kode_mag_calibrate() measures the correction by having the Dot
 * turned in every direction and stores it, and kode_mag_read_axis() and
 * kode_mag_read() then answer the corrected field. The correction is
 * AO_Imu's `mag_cal` setting, so `config` shows and clears it. The heading
 * follows the Dot's top; hold the Dot roughly flat for the best one, though
 * its tilt is corrected with the accelerometer. All four block, and the
 * first three wait as the reads above do; kode_mag_calibrate() is in the
 * kode_screen component, as it needs the glass. */

#define KODE_MAG_CALIBRATE_MS  30000U  /**< The longest kode_mag_calibrate() asks the user to turn the Dot. */

/**
 * @brief The direction the Dot's top points, from the field and the accelerometer.
 * @return Degrees clockwise from north, 0 up to 360; NAN with kode_error() saying why:
 *         "call kode_mag_calibrate() first" without a calibration;
 *         "the Dot is moving too much to tell which way is up" when it is shaken;
 *         "the field is too weak to give a heading" near no horizontal field.
 */
float kode_mag_heading(void);

/**
 * @brief Whether a magnetometer calibration is stored.
 * @return true when one is; false when not, or when the answer failed
 *         (kode_error() says so).
 */
bool kode_mag_is_calibrated(void);

/**
 * @brief Forget the stored calibration; the field reads raw again.
 * @return true when none is stored any more; false with kode_error() saying why.
 */
bool kode_mag_clear_calibration(void);

/**
 * @brief Guide the user to calibrate the compass, and store the result.
 *
 * Takes the glass: a screen asks the user to turn the Dot in every direction
 * and shows how far along they are. Blocks until the sweep is complete or
 * KODE_MAG_CALIBRATE_MS have passed. One calibration runs at a time; if two
 * overlap, the last one stored wins. Needs the kode_screen component, and is
 * not available in the kodeOS image.
 * Its screen replaces the app's: call kode_screen_new() again after it, as the old
 * widgets and canvases are gone.
 *
 * @return true when the calibration is stored; false with kode_error()
 *         saying why, such as "not enough directions were covered".
 */
bool kode_mag_calibrate(void);

/* ---- Vibration ---- */

/* One motor, one thing at a time: a call whose motor is in use waits up to
 * KODE_HAPTIC_WAIT_MS, but a hold on the motor, whoever started it, fails a
 * play at once. A hold has no time limit and any reset stops the motor.
 * The stored `haptic` setting scales every strength below. kode_haptic_vibrate() and
 * kode_haptic_play() block; kode_haptic_add_profile() never waits for the motor. */

#define KODE_HAPTIC_PROFILES_MAX 8U     /**< Profiles an app can add.                          */
#define KODE_HAPTIC_PATTERN_MAX  16U    /**< Segments in one vibration.                        */
#define KODE_HAPTIC_NAME_MAX     16U    /**< Bytes of a profile name, NUL included.            */
#define KODE_HAPTIC_WAIT_MS      5000U  /**< How long a call waits for a playing profile.      */

/**
 * @brief Vibrate until kode_haptic_vibrate(false).
 * @param on true to start at the strongest the motor allows, false to stop whatever the app plays.
 * @return true when done (already on counts); false if the motor stayed busy
 *         or the call could not start, with kode_error() saying why.
 */
bool kode_haptic_vibrate(bool on);

/**
 * @brief Play a named vibration: "click", "tick", "double", "success", "warning", "error" or an added one.
 *
 * Returns once the vibration is started, not when it ends.
 *
 * @param profile The name, any case.
 * @return true when started; false for an unknown name, while kode_haptic_vibrate(true)
 *         holds the motor, or if the motor stayed busy.
 */
bool kode_haptic_play(const char *profile);

/**
 * @brief Add or replace a vibration of the app's own.
 * @param name        1 to KODE_HAPTIC_NAME_MAX - 1 bytes; not a built-in name; any case.
 * @param pattern_ms  Segment lengths in ms, on first, each above 0; copied.
 * @param n           1 to KODE_HAPTIC_PATTERN_MAX segments.
 * @param intensity   0 to 100, spread over the motor's whole range.
 * @return true when stored; false with kode_error() saying why, or when
 *         KODE_HAPTIC_PROFILES_MAX are taken.
 */
bool kode_haptic_add_profile(const char *name, const uint16_t *pattern_ms, uint32_t n, uint8_t intensity);

/* ---- Audio ---- */

/* 48 kHz, 16-bit, mono WAV or samples only. Audio calls take turns, and a play or a
 * recording to a file or a buffer keeps its turn to the end. A call that finds the speaker
 * or microphone held waits up to KODE_AUDIO_BUSY_MS, then fails with "audio is
 * busy"; a background recording waits the same, then ends with no frames.
 * kode_audio_set(), kode_audio_get(), kode_audio_play_stop(), kode_audio_is_playing()
 * and kode_audio_record_status() take no turn. A setting lasts this boot. A
 * sound is refused while a recording runs, and a recording while a sound
 * plays in the background. */

#define KODE_AUDIO_BUSY_MS       5000U   /**< How long a call waits for the audio path.  */
#define KODE_AUDIO_TONE_MAX_MS   65535U  /**< The longest tone, ms.                      */
#define KODE_AUDIO_TONE_HZ_MIN   400U    /**< The lowest tone, Hz.                       */
#define KODE_AUDIO_TONE_HZ_MAX   3400U   /**< The highest tone, Hz.                      */
#define KODE_AUDIO_RECORD_MAX_MS 30000U  /**< The longest recording, ms.                 */

/** @brief What a recording in the background has done. */
typedef struct {
    bool     recording;  /**< It still records.                                        */
    uint32_t ms;         /**< While it records: time since it started, at most the length asked.
                          *   Once it ended: the length recorded.                      */
    uint32_t frames;     /**< Frames recorded; 0 while it records.                     */
} kode_audio_record_status_t;

/**
 * @brief Play a WAV file and wait until it has been heard.
 *
 * Blocks. A file of another rate, width or channel count fails and says which;
 * a .mp3 fails too.
 *
 * @param path "sd/..." or "flash/...", a 48 kHz mono 16-bit WAV.
 * @return true when it played to its end or kode_audio_play_stop() ended it; false with kode_error() saying why.
 */
bool kode_audio_play(const char *path);

/**
 * @brief Play a WAV file in the background; a sound already playing there is stopped first.
 *
 * Returns once the sound has started. The file is checked before it starts,
 * so a bad one fails here. A sound that fails after it started ends, a warning
 * is logged, and kode_audio_is_playing() turns false.
 *
 * @param path As for kode_audio_play().
 * @return true when it started; false with kode_error() saying why.
 */
bool kode_audio_play_start(const char *path);

/**
 * @brief Whether a sound started by kode_audio_play_start() is still playing.
 * @return true while it plays; false once it ended or was stopped.
 */
bool kode_audio_is_playing(void);

/**
 * @brief Stop the sound started by kode_audio_play_start(), or a play or tone another task waits for.
 *
 * Takes no turn, so it works while another task's call blocks.
 *
 * @return true when nothing plays any more; false if it did not stop in time.
 */
bool kode_audio_play_stop(void);

/**
 * @brief Play samples from memory and wait until they have been heard.
 *
 * Blocks. Waits its turn for the speaker like kode_audio_play(). Refused at
 * once while a sound plays in the background or a recording runs.
 *
 * @param samples 48 kHz mono 16-bit samples; read in place until the call returns.
 * @param count   How many, above 0.
 * @return true when they were heard or kode_audio_play_stop() ended them; false with kode_error() saying why.
 */
bool kode_audio_play_samples(const int16_t *samples, uint32_t count);

/**
 * @brief Play a tone and wait until it has been heard.
 *
 * Blocks. Waits its turn for the speaker like kode_audio_play(), so tones in
 * a loop each play in full. Refused at once while a sound plays in the
 * background or a recording runs.
 *
 * @param hz KODE_AUDIO_TONE_HZ_MIN to KODE_AUDIO_TONE_HZ_MAX; the board snaps it to a nearby frequency.
 * @param ms 1 to KODE_AUDIO_TONE_MAX_MS.
 * @return true when it was heard or kode_audio_play_stop() ended it; false with kode_error() saying why.
 */
bool kode_audio_tone(uint16_t hz, uint32_t ms);

/**
 * @brief Change an audio setting for this boot.
 * @param setting "volume" (0 to 100), "mic_gain" (0 to 42 dB, in steps of 6) or "mute" (0 or 1), any case.
 * @param value   The new value.
 * @return true when sent; false for an unknown name, a value out of range, or no
 *         room for the request (no event block free), with kode_error() saying which.
 */
bool kode_audio_set(const char *setting, uint32_t value);

/**
 * @brief Read an audio setting. Blocks briefly.
 * @param setting As for kode_audio_set().
 * @return The value; 0 on failure, so read kode_error() to tell it from a real 0.
 */
uint32_t kode_audio_get(const char *setting);

/**
 * @brief Record the microphone into a WAV file and wait until it is written.
 *
 * Blocks. Refused while a recording or a sound runs in the background. AO_Storage
 * writes the file outside the file calls' turn. A medium that refuses the file
 * as denied reads "audio is busy", like a held microphone.
 *
 * @param path "sd/..." or "flash/...", whose folder exists.
 * @param ms   1 to KODE_AUDIO_RECORD_MAX_MS.
 * @return true when the file is written; false with kode_error() saying why.
 */
bool kode_audio_record_file(const char *path, uint32_t ms);

/**
 * @brief Record the microphone into a buffer of the caller's own and wait until it is full or the time is up.
 *
 * Blocks. Waits its turn for the microphone like kode_audio_record_file():
 * a microphone another task holds fails with "audio is busy" after
 * KODE_AUDIO_BUSY_MS. Refused while a recording or a sound runs in the
 * background. Like every call of this layer it is for tasks, never for an
 * Active Object.
 *
 * The frames are 48 kHz mono 16-bit samples. The recording lasts @p ms or
 * until @p cap frames are in, whichever is first, so a buffer smaller than the
 * time asked ends it early. @p frames can also be below that: the codec hands
 * over what it has when the recording ends, down to none. Frames past
 * @p frames are not written. The SDK records into memory of its own that it
 * keeps after the first call, and the frames are copied out at the end.
 *
 * @param buf    Where the frames go; what it holds after a failure is unspecified.
 * @param cap    Its room, in frames, above 0.
 * @param ms     1 to KODE_AUDIO_RECORD_MAX_MS: how long to record.
 * @param frames Out, may be NULL: the frames written; 0 on failure.
 * @return true when the recording arrived; false with kode_error() saying why.
 */
bool kode_audio_record_samples(int16_t *buf, uint32_t cap, uint32_t ms, uint32_t *frames);

/**
 * @brief Record the microphone into memory in the background; the previous recording is dropped.
 *
 * Returns once the recording has started. A recording that fails to run ends
 * with no frames and a warning is logged.
 *
 * @param max_ms 1 to KODE_AUDIO_RECORD_MAX_MS: the longest it records.
 * @return true when it started; false with kode_error() saying why.
 */
bool kode_audio_record_start(uint32_t max_ms);

/**
 * @brief What the recording started by kode_audio_record_start() has done.
 * @return The status; all zero before the first recording.
 */
kode_audio_record_status_t kode_audio_record_status(void);

/**
 * @brief End the recording early and keep what it has.
 * @return true when no recording runs any more; false when none was ever
 *         started or it did not end in time, with kode_error() saying why.
 */
bool kode_audio_record_stop(void);

/**
 * @brief Write the recording to a WAV file.
 *
 * Blocks. The recording stays in PSRAM until the next kode_audio_record_start(),
 * so it can be saved again.
 *
 * @param path As for kode_audio_record_file().
 * @return true when the file is written; false with kode_error() saying why.
 */
bool kode_audio_record_save(const char *path);

/* ---- Wi-Fi ---- */

/* The calls block, one task at a time. Joins and drops are also events. */

#define KODE_WIFI_CONNECT_MS 30000U  /**< How long kode_wifi_connect() waits for an address. */
#define KODE_WIFI_LEAVE_MS   5000U   /**< How long kode_wifi_disconnect() waits. */

/**
 * @brief Join a network and wait until the board has an address.
 *
 * Returns at once when the board already has an address on that network.
 * A network that joined is remembered.
 *
 * @param ssid     The network's name, 1 to 32 bytes, case matters; NULL for a
 *                 network the board remembers.
 * @param password Its password, up to 64 bytes; NULL or "" for none.
 * @return true when the board has an address; false with kode_error() saying why.
 */
bool kode_wifi_connect(const char *ssid, const char *password);

/**
 * @brief Whether the board has an address on a network now. Never waits.
 * @return true when it has.
 */
bool kode_wifi_is_connected(void);

/**
 * @brief Leave the network; its password stays remembered.
 * @return true when the board has no address any more; false with kode_error() saying why.
 */
bool kode_wifi_disconnect(void);

#define KODE_WIFI_SCAN_MS  110000U  /**< How long kode_wifi_scan() waits for the sweep: its 90 s ceiling, a step's 12 s and room. */
#define KODE_WIFI_SSID_MAX 33U      /**< Bytes of a network's name, NUL included. */

/** @brief One network kode_wifi_scan() found. */
typedef struct {
    char    ssid[KODE_WIFI_SSID_MAX];  /**< Its name; "" for a hidden one. */
    int8_t  rssi;                      /**< Strength, dBm.                 */
    uint8_t channel;                   /**< Its channel.                   */
    bool    open;                      /**< No password.                   */
} kode_wifi_network_t;

/**
 * @brief Scan the air for networks, both bands, and wait for the sweep (about 38 s).
 *
 * Brings the radio's link up first when it is down, and switches Wi-Fi on for
 * nobody; the call can wait for that bring-up plus one sweep. It does not wait
 * for anything longer: while the radio is already sweeping - the boot's own
 * search for a known network, another scan - or listens for Bluetooth
 * devices (kode_ble_scan(), or the console's `ble_scan`), or runs a self-test
 * or an install, it fails at once and kode_error() says the radio is busy;
 * ask again later. Joins and leaves take turns with it.
 *
 * @param out Where the networks go, the strongest first.
 * @param cap How many @p out holds; the strongest @p cap are kept.
 * @return How many were written, 0 when the air is empty; -1 with
 *         kode_error() saying why.
 */
int kode_wifi_scan(kode_wifi_network_t *out, size_t cap);

/* ---- Bluetooth ---- */

/* The board stays on the air for the console as it always is; an app adds a
 * name and maker data to what a phone sees when it looks, and listens for the
 * devices nearby. Bluetooth on and off is the user's, in Settings: an app
 * never switches it, and while it is off every call here fails with
 * kode_error() saying so. */

#define KODE_BLE_NAME_MAX     30U     /**< Bytes of a name, NUL included.                       */
#define KODE_BLE_ADDR_MAX     18U     /**< Bytes of an address as text, NUL included.           */
#define KODE_BLE_SCAN_RSP_MAX 31U     /**< Bytes the name and the data share, two of them a
                                       *   length and a type per part.                          */
#define KODE_BLE_DATA_MIN     2U      /**< The least maker data: its company id.                */
#define KODE_BLE_SCAN_MS_MIN  500U    /**< The shortest listen, ms.                             */
#define KODE_BLE_SCAN_MS_MAX  30000U  /**< The longest listen, ms.                              */

/**
 * @brief Name the board for the phones that look: the name in the scan response.
 *
 * Not stored: every boot starts from the board's serial. The console's own
 * service stays in the advert whatever the name. The device's GAP name (the
 * Device Name characteristic, 0x2A00) stays the serial, so a phone that has
 * connected shows the serial, not this name.
 *
 * @param name Up to KODE_BLE_NAME_MAX - 1 bytes; NULL or "" for the serial.
 * @return true when the radio has the name; false with kode_error() saying why:
 *         Bluetooth off in Settings, or a name that does not fit beside the data
 *         (KODE_BLE_SCAN_RSP_MAX bytes for both).
 */
bool kode_ble_set_name(const char *name);

/**
 * @brief Maker data for the phones that look, beside the name.
 *
 * Not stored. The data starts with the company id, two bytes little-endian, as
 * Bluetooth defines it: the call adds none.
 *
 * @param data The bytes; NULL when @p len is 0.
 * @param len  0 for none, or KODE_BLE_DATA_MIN up to what fits beside the name.
 * @return true when the radio has the data; false with kode_error() saying why.
 */
bool kode_ble_set_manufacturer_data(const uint8_t *data, size_t len);

/** @brief One device kode_ble_scan() heard. */
typedef struct {
    char   name[KODE_BLE_NAME_MAX];     /**< Its name, cut to fit; "" when it gave none. */
    char   address[KODE_BLE_ADDR_MAX];  /**< "c0:00:00:00:00:01".                        */
    int8_t rssi;                        /**< Strength, dBm.                              */
} kode_ble_device_t;

/**
 * @brief Listen for Bluetooth devices for @p ms and wait for the end.
 *
 * Brings the radio's link up first when it is down. Like kode_wifi_scan() it
 * waits for nothing longer: while the radio is already scanning - a Wi-Fi
 * scan, another listen, or its own search for a known network - or runs a
 * self-test or an install, it fails at once and kode_error() says the radio
 * is busy. That search runs each time the link comes up with Wi-Fi on, so
 * the first listen on a link that was down nearly always fails busy; ask
 * again once the search is over. One task at a time.
 *
 * @param out Where the devices go, the strongest first.
 * @param cap How many @p out holds; the strongest @p cap are kept.
 * @param ms  How long to listen, KODE_BLE_SCAN_MS_MIN to KODE_BLE_SCAN_MS_MAX.
 * @return How many were written, 0 when nothing was heard; -1 with kode_error()
 *         saying why.
 */
int kode_ble_scan(kode_ble_device_t *out, size_t cap, uint32_t ms);

/* One GATT service of the app's own, beside the console's, declared at file
 * scope in the file that holds app_main() (or in one the app otherwise
 * links):
 *
 *     static const kode_ble_characteristic_t s_chars[] = {
 *         { "temp", "6e400002-b5a3-f393-e0a9-e50e24dcca9e", KODE_BLE_READ | KODE_BLE_NOTIFY },
 *         { "cmd",  "6e400003-b5a3-f393-e0a9-e50e24dcca9e", KODE_BLE_WRITE },
 *     };
 *     KODE_BLE_SERVICE("6e400001-b5a3-f393-e0a9-e50e24dcca9e", s_chars);
 *
 * The radio finds it when its Bluetooth starts; an app without it pays
 * nothing. Security is the console's, whatever the firmware makes it: with
 * console pairing and the passkey on, the default, every characteristic
 * needs an encrypted, authenticated, bonded link, so only a phone bonded in
 * kodeOS with the code on the glass reads, writes or subscribes; with pairing
 * but no passkey (Just Works) the link is encrypted and bonded, not
 * authenticated; with console pairing off the service is as open as the
 * console. A table the radio cannot serve (a UUID that is not 128 bits as
 * 8-4-4-4-12 hex, no characteristics or more than KODE_BLE_CHARACTERISTICS_MAX, a name
 * missing, too long or used twice, no property) is left out with one log
 * line, and
 * kode_ble_set_value() and kode_ble_notify() fail saying why. A write arrives
 * as ble_written, and a central that connects or leaves as ble_connected and
 * ble_disconnected (see the events). Throughput is not promised: the
 * notifications share the link with the console.
 *
 * A KODE_BLE_QUEUED characteristic gives each read the next value instead of
 * the same one, for a phone that drains it by reading until a read comes
 * back empty:
 *
 *     { "from_radio", "2c55e69e-4993-11ed-b878-0242ac120002", KODE_BLE_QUEUED },
 *
 * kode_ble_queue_push() puts values behind the others, each read takes the
 * oldest, and ble_taken says how many are left, so the app refills without
 * looking. A value longer than one read is read in parts and taken with its
 * last part. A phone that connects or leaves empties the queue. */

#define KODE_BLE_CHARACTERISTICS_MAX      4U     /**< Characteristics one service may have.            */
#define KODE_BLE_VALUE_MAX      244U   /**< Bytes of a value: one notification at an MTU of 247. */
#define KODE_BLE_CHARACTERISTIC_NAME_MAX  16U    /**< Bytes of a characteristic's name, NUL included.  */
#define KODE_BLE_READ           0x01U  /**< A phone may read the value.                      */
#define KODE_BLE_WRITE          0x02U  /**< A phone may write it: ble_written.               */
#define KODE_BLE_NOTIFY         0x04U  /**< A phone may subscribe: kode_ble_notify().        */
#define KODE_BLE_QUEUED         0x08U  /**< Reads take the values kode_ble_queue_push() queued,
                                        *   oldest first; a read with none left gets 0 bytes.
                                        *   A phone may read it.                              */
#define KODE_BLE_QUEUE_SLOTS    8U     /**< Values one queued characteristic holds.          */

/** @brief One characteristic of the app's service. */
typedef struct {
    const char *name;   /**< 1 to KODE_BLE_CHARACTERISTIC_NAME_MAX - 1 bytes, unique in the service.  */
    const char *uuid;   /**< 128 bits as text, "6e400002-b5a3-f393-e0a9-e50e24dcca9e".       */
    uint8_t     props;  /**< KODE_BLE_READ, _WRITE, _NOTIFY and _QUEUED, at least one. */
} kode_ble_characteristic_t;

/** @brief The app's service: what KODE_BLE_SERVICE() defines. */
typedef struct {
    const char            *uuid;   /**< 128 bits as text, as a characteristic's. */
    const kode_ble_characteristic_t *chars;  /**< Its characteristics, in table order.     */
    size_t                 count;  /**< How many; 1 to KODE_BLE_CHARACTERISTICS_MAX.        */
} kode_ble_service_t;

/* The constant's linkage: C's, in C++ too, which the radio's reference has. */
#ifdef __cplusplus
#define KODE_BLE_LINKAGE_ extern "C"   /**< The constant's linkage in C++. */
#else
/* cppcheck-suppress misra-c2012-2.5 ; an app expands it, through KODE_BLE_SERVICE() */
#define KODE_BLE_LINKAGE_ extern       /**< The constant's linkage in C.   */
#endif

/* cppcheck-suppress misra-c2012-2.5 ; a name for the apps that declare a service, never used inside the SDK */
/**
 * @brief Declare the app's service: @p uuid_ and the array @p chars_ of
 *        kode_ble_characteristic_t, at file scope, followed by a semicolon.
 *
 * Defines the constant kode_ble_app_service, the one name the radio looks
 * for; a second service in the same app is a link error.
 */
#define KODE_BLE_SERVICE(uuid_, chars_) \
    KODE_BLE_LINKAGE_ const kode_ble_service_t kode_ble_app_service; \
    const kode_ble_service_t kode_ble_app_service = { \
        (uuid_), (chars_), sizeof(chars_) / sizeof((chars_)[0]) }

/**
 * @brief Set what a phone that reads characteristic @p chr gets. Nothing is sent.
 *
 * Works before Bluetooth has started: the value waits for the first read.
 * Waits for the radio to take it: a step of the radio's, and a second at
 * most with its queue full or other tasks' values ahead of this one.
 *
 * @param chr  The characteristic's name.
 * @param data The value; NULL when @p len is 0.
 * @param len  0 to KODE_BLE_VALUE_MAX bytes.
 * @return true when it is set; false with kode_error() saying why: Bluetooth
 *         off in Settings, the service left out or not declared, no
 *         characteristic of that name, a value too long, or the radio not
 *         taking it in time.
 */
bool kode_ble_set_value(const char *chr, const void *data, size_t len);

/**
 * @brief Set characteristic @p chr's value and notify the phone subscribed to it.
 *
 * With nobody subscribed the value is set, nothing is sent, and the call
 * answers true. A notification longer than the link takes in one packet is
 * not sent; the value stays for a read. Waits for the radio's answer, as
 * kode_ble_set_value() does.
 *
 * @param chr  The characteristic's name; it has KODE_BLE_NOTIFY.
 * @param data The bytes; NULL when @p len is 0.
 * @param len  0 to KODE_BLE_VALUE_MAX bytes.
 * @return true when it is set and, with a phone subscribed, queued; false
 *         with kode_error() saying why: as kode_ble_set_value(), a
 *         characteristic without KODE_BLE_NOTIFY, or the notifications
 *         waiting for the link already filling their ring (the value is set;
 *         try again), or the radio not taking it in time.
 */
bool kode_ble_notify(const char *chr, const void *data, size_t len);

/**
 * @brief Queue one value for the reads of characteristic @p chr, behind the others.
 *
 * Each read a phone makes takes the oldest value queued, and ble_taken says
 * how many are left. Waits for the radio to take it, as kode_ble_set_value()
 * does.
 *
 * @param chr  The characteristic's name; it has KODE_BLE_QUEUED.
 * @param data The value.
 * @param len  1 to KODE_BLE_VALUE_MAX bytes.
 * @return true when it is queued; false with kode_error() saying why: as
 *         kode_ble_set_value(), a characteristic without KODE_BLE_QUEUED, no
 *         bytes, or the queue full (KODE_BLE_QUEUE_SLOTS values).
 */
bool kode_ble_queue_push(const char *chr, const void *data, size_t len);

/**
 * @brief How many more values characteristic @p chr's queue takes now.
 *
 * Does not wait: a read the phone makes may change it the next moment.
 *
 * @param chr The characteristic's name; it has KODE_BLE_QUEUED.
 * @return 0 to KODE_BLE_QUEUE_SLOTS; 0 with kode_error() saying why when it
 *         cannot be told: Bluetooth off in Settings, the service left out or
 *         not declared, no characteristic of that name, or one without
 *         KODE_BLE_QUEUED.
 */
size_t kode_ble_queue_room(const char *chr);

/**
 * @brief Empty characteristic @p chr's queue. Waits for the radio, as kode_ble_set_value() does.
 * @param chr The characteristic's name; it has KODE_BLE_QUEUED.
 * @return true when it is empty; false with kode_error() saying why: as
 *         kode_ble_queue_room(), or the radio not taking it in time.
 */
bool kode_ble_queue_clear(const char *chr);

/* ---- HTTP ---- */

/* The calls block, one task at a time. A redirect is not followed: it comes back
 * as the answer, and kode_http_status() says 301 or 302. */

#define KODE_HTTP_BODY_MAX 65536U  /**< Largest answer a request returns, bytes. */
#define KODE_HTTP_URL_MAX  256U    /**< Bytes of a URL, NUL included.            */

/**
 * @brief GET a page and return the answer's body as text.
 *
 * A page that answers 404 is not a failure: its body comes back and
 * kode_http_status() says 404.
 *
 * @param url An http:// or https:// address, under KODE_HTTP_URL_MAX bytes.
 * @return The body, valid until this task's next kode_http_get() or
 *         kode_http_post(); "" with kode_error() saying why when there is no
 *         answer, or one over KODE_HTTP_BODY_MAX.
 */
const char *kode_http_get(const char *url);

/**
 * @brief POST @p body to a page and return the answer's body as text.
 *
 * The body is sent as application/json when its first character that is not
 * a space starts a JSON object or array, as text/plain otherwise.
 *
 * @param url  As for kode_http_get().
 * @param body The text to send; "" sends an empty body.
 * @return As for kode_http_get().
 */
const char *kode_http_post(const char *url, const char *body);

/**
 * @brief The status this task's last request was answered with.
 * @return The HTTP status, such as 200 or 404; 0 when the task has made no
 *         request, or the last one failed before an answer.
 */
uint16_t kode_http_status(void);

/* ---- Ping, name lookup and UDP ---- */

/* The calls block the calling task, need an address (kode_wifi_connect()) and
 * fail with kode_error() saying why. Tasks may call them at once. */

#define KODE_NET_HOST_LIMIT  63U     /**< Longest host name or address, bytes. */
#define KODE_PING_COUNT_MAX  10U     /**< The most echoes one ping sends.     */
#define KODE_UDP_DATA_MAX    65535U  /**< Longest datagram, bytes.            */

/** @brief What a ping added up to. */
typedef struct {
    bool     ok;        /**< The ping ran; false with kode_error() saying why. */
    uint32_t sent;      /**< Echoes sent.                                       */
    uint32_t received;  /**< Echoes answered; 0 when nothing answered.          */
    uint32_t min_ms;    /**< Shortest round trip.                               */
    uint32_t avg_ms;    /**< Mean round trip.                                   */
    uint32_t max_ms;    /**< Longest round trip.                                */
} kode_net_ping_t;

/**
 * @brief Ping a host, one echo a second.
 *
 * Blocks for about @p count seconds. A host that never answers is not a
 * failure: ok is true and received 0.
 *
 * @param host  A name or a dotted address, up to KODE_NET_HOST_LIMIT bytes.
 * @param count 1 to KODE_PING_COUNT_MAX.
 * @return The counts; ok false with kode_error() saying why.
 */
kode_net_ping_t kode_net_ping(const char *host, uint32_t count);

/**
 * @brief Resolve a host name to its IPv4 address.
 * @param host A name or a dotted address, up to KODE_NET_HOST_LIMIT bytes.
 * @param ip4  Receives the address in network byte order; untouched on failure.
 * @return true when it resolved; false with kode_error() saying why.
 */
bool kode_net_resolve(const char *host, uint32_t *ip4);

/**
 * @brief Send one UDP datagram; it is on its way, not known to have arrived.
 * @param host A name or a dotted address, up to KODE_NET_HOST_LIMIT bytes.
 * @param port The destination port, above 0.
 * @param data The bytes; NULL only when @p len is 0.
 * @param len  0 to KODE_UDP_DATA_MAX.
 * @return true when it left; false with kode_error() saying why.
 */
bool kode_udp_send(const char *host, uint16_t port, const void *data, uint32_t len);

/* ---- MQTT ---- */

/* The calls block, one task at a time. Messages arrive as "mqtt_message" events. */

#define KODE_MQTT_WAIT_MS     20000U /**< How long kode_mqtt_connect() and kode_mqtt_disconnect() wait. */
#define KODE_MQTT_BUSY_MS     5000U  /**< How long a call waits for room in the network's queue. */
#define KODE_MQTT_URI_LIMIT   95U    /**< Longest broker address, bytes. */
#define KODE_MQTT_TOPIC_LIMIT 79U    /**< Longest topic, bytes. */
#define KODE_MQTT_TEXT_LIMIT  240U   /**< Longest message, bytes. */

/**
 * @brief Connect to a broker and wait until it accepts.
 * @param uri mqtt://host[:port] or mqtts://host[:port], up to KODE_MQTT_URI_LIMIT bytes.
 * @return true when connected, or already connected to @p uri; false with kode_error() saying why.
 */
bool kode_mqtt_connect(const char *uri);

/**
 * @brief Queue a message for a topic; true does not mean the broker has it.
 * @param topic 1 to KODE_MQTT_TOPIC_LIMIT bytes.
 * @param text  Up to KODE_MQTT_TEXT_LIMIT bytes; "" sends none.
 * @return true when queued; false with kode_error() saying why.
 */
bool kode_mqtt_publish(const char *topic, const char *text);

/**
 * @brief Ask the broker for a topic; its messages arrive as "mqtt_message" events.
 * @param topic 1 to KODE_MQTT_TOPIC_LIMIT bytes.
 * @return true when queued; false with kode_error() saying why.
 */
bool kode_mqtt_subscribe(const char *topic);

/**
 * @brief Stop receiving a topic; the broker stops sending it.
 * @param topic The filter given to kode_mqtt_subscribe(), 1 to KODE_MQTT_TOPIC_LIMIT bytes.
 * @return true when queued; false with kode_error() saying why. Waits for
 *         room in the network's queue like kode_mqtt_subscribe().
 */
bool kode_mqtt_unsubscribe(const char *topic);

/**
 * @brief Whether the app's client is connected to a broker now. Never waits.
 *
 * False while the Kode channel holds the client: the app cannot use it.
 * @return true when it is.
 */
bool kode_mqtt_is_connected(void);

/**
 * @brief Leave the broker.
 * @return true when the board is no longer connected; false with kode_error() saying why.
 */
bool kode_mqtt_disconnect(void);

/* ---- Battery and power ---- */

/* The calls block, one task at a time. A fall below the low mark is also an event. */

#define KODE_BATTERY_LOW_PCT   15U   /**< battery_low fires when the level drops below this. */
#define KODE_BATTERY_REARM_PCT 5U    /**< It fires again once the level climbed this far above it. */
#define KODE_BATTERY_WAIT_MS   3000U /**< How long a call waits for the first reading. */
#define KODE_CHARGE_CURRENT_MIN_MA 64U  /**< The smallest fast-charge current kode_power_set_charge_current() takes, mA. */
#define KODE_CHARGE_CURRENT_MAX_MA 896U /**< The largest, the pack's ceiling, mA. */

/**
 * @brief The charge left in the cell, in percent.
 *
 * Needs the fuel gauge: without it the number would be a guess.
 *
 * @return 0 to 100; 0 with kode_error() saying why when the battery has not
 *         been read within KODE_BATTERY_WAIT_MS or the gauge does not answer.
 */
uint8_t kode_battery_level(void);

/**
 * @brief Whether the charger is charging the cell, in pre-charge or fast charge.
 * @return true when it is; false when it is not, or is done, or when the
 *         battery has not been read (kode_error() says so).
 */
bool kode_battery_is_charging(void);

/**
 * @brief The cell's voltage in millivolts; without the gauge, the charger's, in 20 mV steps.
 * @return The voltage; 0 with kode_error() saying why when the battery has
 *         not been read within KODE_BATTERY_WAIT_MS.
 */
uint32_t kode_battery_voltage_mv(void);

/**
 * @brief Whether USB power is plugged in, judged from the last battery reading.
 * @return true when it is; false when it is not, or when the battery has not
 *         been read within KODE_BATTERY_WAIT_MS (kode_error() says so).
 */
bool kode_power_is_usb_connected(void);

/**
 * @brief Switch the board off: deep sleep, and only the bottom button wakes it.
 *
 * Every owner puts its device away first. It is an order, not a request: an
 * owner with work in hand is given up to KODE_LOWPOWER_ORDER_MS to finish it,
 * and the board goes off when that runs out, done or not. USB is judged from the last
 * battery reading, and nothing is asked while it is plugged in: on a cable
 * the board does not switch off. The battery's ship mode is not reached
 * from here; it is the console's `ship_mode`.
 *
 * Needs CONFIG_KODE_LOWPOWER: without it the call is refused and says so.
 *
 * @return false with kode_error() saying why; on battery it does not return.
 */
bool kode_power_off(void);

/**
 * @brief Allow or forbid charging the cell. Not stored: the next boot
 *        goes back to the stored `charge` setting (on unless it was set off).
 *
 * The charger still holds charging outside the pack's 10 to 45 C window; a
 * request to charge while held takes effect when the window returns.
 * @param on true to allow charging.
 * @return true when the power manager took the request; false with
 *         kode_error() saying why. Waits up to KODE_BATTERY_WAIT_MS for room.
 */
bool kode_power_set_charging(bool on);

/**
 * @brief Set the fast-charge current. Not stored: the next boot goes
 *        back to the stored `charge_ma` setting.
 *
 * The charger keeps the nearest 64 mA step. The board's input limit may hold
 * the real current lower.
 * @param ma KODE_CHARGE_CURRENT_MIN_MA to KODE_CHARGE_CURRENT_MAX_MA; 0 is not
 *           "off": use kode_power_set_charging().
 * @return true when the power manager took the request; false with
 *         kode_error() saying why, the range included.
 */
bool kode_power_set_charge_current(uint16_t ma);

/* ---- Settings ---- */

/* A few strings the board keeps by name across resets and visits to kodeOS.
 * Names are exact: "Name" and "name" are two settings. The calls block, one
 * task at a time, and fail on a task whose stack is in PSRAM: use app_main or
 * a task of your own. Anyone who can read this board can read them: keep no
 * secrets here. */

#define KODE_SETTING_KEY_MAX   16U   /**< Bytes of a name, NUL included.  */
#define KODE_SETTING_VALUE_MAX 192U  /**< Bytes of a value, NUL included. */

/**
 * @brief Keep @p value under @p key, replacing what was there.
 *
 * @param key   1 to KODE_SETTING_KEY_MAX - 1 bytes.
 * @param value Up to KODE_SETTING_VALUE_MAX - 1 bytes; "" is a value.
 * @return true when it is kept; false with kode_error() saying why.
 */
bool kode_setting_set(const char *key, const char *value);

/**
 * @brief The value kept under @p key.
 *
 * @param key As for kode_setting_set().
 * @return The value, valid until this task's next kode_setting_get(); ""
 *         with kode_error() saying why when nothing is kept under @p key or
 *         it cannot be read. A kept "" is "" with no error.
 */
const char *kode_setting_get(const char *key);

/* ---- Expansion pins ---- */

/* The fourteen positions of the expansion header J3, by the number printed on
 * the connector. A pin is configured by the call that uses it, and switched to
 * the mode the next call needs. The calls block, one task at a time. They fail
 * with kode_error() saying why when the pin does not exist, a module holds it
 * or something else in this image owns the header. A pin something listens
 * to (pin_<n>_changed, see the events) is read, never driven. */

#define KODE_PIN_FIRST      1U      /**< First pin, as printed on the connector. */
#define KODE_PIN_LAST       14U     /**< Last pin. */
#define KODE_PIN_PWM_HZ_MIN 1U      /**< Lowest PWM frequency, Hz. */
#define KODE_PIN_PWM_HZ_MAX 40000U  /**< Highest PWM frequency, Hz. */
/* cppcheck-suppress misra-c2012-2.5 ; the app build of kode_standard_ao.c uses it, and the kodeOS image builds only its refusal */
#define KODE_PIN_SETTLE_MS  30U     /**< From a pin's edge to the read of its level, ms (pin_<n>_changed). */
#define KODE_PIN_POWER_MS   300000U /**< How long kode_pin_power_3v3() waits for J3's supply to switch, ms: a sweep at its 90 s ceiling, an autojoin round's joins and room. */

/**
 * @brief Make @p pin an output and drive it high or low.
 * @param pin   KODE_PIN_FIRST to KODE_PIN_LAST.
 * @param level true for high.
 * @return true when it is driven; false with kode_error() saying why.
 */
bool kode_pin_write(uint8_t pin, bool level);

/**
 * @brief The level of @p pin; a pin that is neither an input nor an output becomes an input with no pull.
 * @param pin KODE_PIN_FIRST to KODE_PIN_LAST.
 * @return true for high; false for low, or with kode_error() saying why.
 */
bool kode_pin_read(uint8_t pin);

/**
 * @brief The voltage on @p pin, which becomes an analog input.
 * @param pin Pin 8 to 12 or 14: the ones with an analog input.
 * @return Millivolts; 0 with kode_error() saying why when the pin cannot be read.
 */
uint32_t kode_pin_read_analog(uint8_t pin);

/**
 * @brief Run @p pin as a PWM output, or retune it.
 * @param pin      KODE_PIN_FIRST to KODE_PIN_LAST.
 * @param hz       KODE_PIN_PWM_HZ_MIN to KODE_PIN_PWM_HZ_MAX.
 * @param duty_pct 0 to 100, the share of each period the pin is high.
 * @return true when it runs; false with kode_error() saying why.
 */
bool kode_pin_write_pwm(uint8_t pin, uint32_t hz, uint8_t duty_pct);

/**
 * @brief Switch the 3.3 V on J3's pins 2 and 19 on or off, and wait until it is.
 *
 * Its state when the app starts is not promised: a C5 that kodeOS parked for
 * the jump, held in reset or never linked lets go of the pin, and the board's
 * pull-up then holds the supply on; one whose link drove it off leaves it
 * off. It is unknown until the C5 reports it, so an app asks for the state it
 * needs, and a first call always switches it, never takes it as already
 * done. Its switch is a pin of the radio chip, the C5, so the call asks the
 * radio. With the link up and the radio idle it returns within
 * milliseconds; with no link, an "off" waits a few seconds for the C5's boot
 * and its link. It waits up to KODE_PIN_POWER_MS while the radio finishes the
 * job it is on, never one asked after it: a Wi-Fi sweep holds the C5 for
 * about 40 s and up to 90 s, and the boot's search for a known network holds
 * it for that sweep and then up to 20 s for each remembered network that will
 * not have the board. With no
 * link to the C5, "on" resets the C5 for about 10 ms, and the speaker is off
 * meanwhile and while the C5 boots; "off" brings the link up first, as
 * kode_wifi_scan() does, with nothing switched on in Settings, and the C5
 * then stays up for the rest of the boot, drawing about 85 mA. Two calls take
 * turns. The first call starts the standard layer's Active Object, as the
 * first event call does (level 3, KODE_PRIO_USER_GUI), and fails while an
 * Active Object of the app's own holds that level. J3's 5 V is not switched:
 * that is kode_pin_power_5v(). An app that calls it links the radio.
 * @param on true for on.
 * @return true when the supply is as asked; false with kode_error() saying
 *         why: "the C5 would not switch J3's supply", "the C5's link would
 *         not come up or went down: J3's supply switches at its next
 *         link-up", "J3's
 *         supply did not switch within 300 s", the radio not there or busy,
 *         or the level taken.
 */
bool kode_pin_power_3v3(bool on);

/**
 * @brief Put 5 V from the cell on J3, up to 0.5 A, or take it off. Not stored.
 *
 * Switching it on is refused while USB is plugged in and while the cell is
 * under the voltage the pack needs; the power manager also stops it on a low
 * cell or a fault. Switching it off is always taken.
 * @param on true to switch it on.
 * @return true when the power manager took the request; false with
 *         kode_error() saying why. Waits up to KODE_BATTERY_WAIT_MS for the
 *         first battery reading and for room.
 */
bool kode_pin_power_5v(bool on);

/* ---- Buses ---- */

/* A bus on any of J3's pins, by the numbers kode_pin_* uses: every position is
 * routed through the chip's matrix. One bus of each kind for the whole app,
 * held from its begin to its end and shared by every task. begin lends the
 * pins and takes the controller under the kind's tag (KODE_BUS_TAG_I2C...); a
 * pin the app drove with kode_pin_* is released first, and while the bus
 * holds a pin, kode_pin_* on it fails with "pin N is lent to i2c". end gives
 * the pins back, at high impedance. The calls block. Calls on one bus take
 * turns: a transfer waits while another task's transfer on the same bus moves
 * its bytes (an I2C transfer its bytes' time, the wire silent at most
 * KODE_I2C_TIMEOUT_MS within each part; an SPI transfer its bytes' time).
 * The UART's two directions take turns apart: a read waits only for another
 * task's read and its wait, a write only for another write, so a reader and
 * a writer go at once. A transfer never holds up kode_pin_* or another bus;
 * a begin or an end takes turns with kode_pin_* too, and with both of the
 * UART's directions. They fail with kode_error() saying why. */

#define KODE_BUS_TAG_I2C    "i2c"    /**< Who holds the I2C bus's pins, as kode_sys_exp_info() names it. */
#define KODE_BUS_TAG_SPI    "spi"    /**< Who holds the SPI bus's pins.                                  */
#define KODE_BUS_TAG_UART   "uart"   /**< Who holds the UART's pins.                                     */

#define KODE_I2C_HZ_MIN      10000U  /**< Slowest I2C clock, Hz.                                   */
#define KODE_I2C_HZ_MAX      400000U /**< Fastest I2C clock, Hz.                                   */
#define KODE_I2C_TIMEOUT_MS  100U    /**< Longest the wire may stay silent within each command segment of an I2C transfer, ms; a probe tries twice. */
#define KODE_I2C_ADDR_MAX    0x7FU   /**< The highest 7-bit address.                               */

/**
 * @brief Begin the I2C bus with its data on @p sda and its clock on @p scl.
 *
 * The internal pull-ups are on; a module with its own pull-ups needs nothing
 * else. All or nothing: a refusal leaves no pin lent and no controller taken.
 * @param sda The data pin, KODE_PIN_FIRST to KODE_PIN_LAST.
 * @param scl The clock pin, another one.
 * @param hz  KODE_I2C_HZ_MIN to KODE_I2C_HZ_MAX.
 * @return true when the bus is up; false with kode_error() saying why (also
 *         when it is already begun).
 */
bool kode_i2c_begin(uint8_t sda, uint8_t scl, uint32_t hz);

/**
 * @brief Whether a device answers at @p addr.
 * @param addr 0 to KODE_I2C_ADDR_MAX.
 * @return true when one answers. false with kode_error() empty when none
 *         does, or saying why when the bus could not be asked: a line held
 *         low (no pull-ups, a device holding SDA) says the bus is stuck.
 */
bool kode_i2c_probe(uint8_t addr);

/**
 * @brief Write @p len bytes to the device at @p addr.
 * @param addr 0 to KODE_I2C_ADDR_MAX.
 * @param data The bytes.
 * @param len  How many; at least one.
 * @return true when the device took them; false with kode_error() saying why.
 */
bool kode_i2c_write(uint8_t addr, const uint8_t *data, size_t len);

/**
 * @brief Read @p len bytes from the device at @p addr.
 * @param addr 0 to KODE_I2C_ADDR_MAX.
 * @param data Where they go.
 * @param len  How many; at least one.
 * @return true when @p data holds them; false with kode_error() saying why.
 */
bool kode_i2c_read(uint8_t addr, uint8_t *data, size_t len);

/**
 * @brief Write @p out, then read @p in_len bytes back, in one transfer with no stop between.
 *
 * What a register read is: the register's number out, its value in.
 * @param addr    0 to KODE_I2C_ADDR_MAX.
 * @param out     The bytes to write.
 * @param out_len How many; at least one.
 * @param in      Where the bytes read go.
 * @param in_len  How many; at least one.
 * @return true when @p in holds them; false with kode_error() saying why.
 */
bool kode_i2c_write_read(uint8_t addr, const uint8_t *out, size_t out_len, uint8_t *in,
                         size_t in_len);

/**
 * @brief End the I2C bus: the driver closes, then the pins and the controller go back.
 * @return true when it is ended; false with kode_error() saying why (also
 *         when it was not begun).
 */
bool kode_i2c_end(void);

#define KODE_SPI_HZ_MIN      100000U   /**< Slowest SPI clock, Hz: from here up the bus runs at the nearest divider value. */
#define KODE_SPI_HZ_MAX      10000000U /**< Fastest SPI clock, Hz.                                   */
#define KODE_SPI_MODE_MAX    3U        /**< The highest SPI mode: CPOL 1, CPHA 1.                    */
#define KODE_SPI_XFER_MAX    64U       /**< Bytes one SPI transfer moves: the controller's FIFO, no DMA. */

/**
 * @brief Begin the SPI bus with one device on @p cs.
 *
 * The bus is full duplex: every byte out on @p mosi brings one in on @p miso.
 * @p cs goes low for each transfer and high between them. All or nothing: a
 * refusal leaves no pin lent and no controller taken.
 * @param sck  The clock pin, KODE_PIN_FIRST to KODE_PIN_LAST.
 * @param miso The data-in pin, another one.
 * @param mosi The data-out pin, another one.
 * @param cs   The chip-select pin, another one.
 * @param hz   KODE_SPI_HZ_MIN to KODE_SPI_HZ_MAX; the bus runs at the
 *             nearest clock the chip's dividers make, which they can do from
 *             KODE_SPI_HZ_MIN up.
 * @param mode 0 to KODE_SPI_MODE_MAX, the device's datasheet says which: bit 1
 *             the clock's idle level (CPOL), bit 0 the edge data is read on
 *             (CPHA).
 * @return true when the bus is up; false with kode_error() saying why (also
 *         when it is already begun).
 */
bool kode_spi_begin(uint8_t sck, uint8_t miso, uint8_t mosi, uint8_t cs, uint32_t hz,
                    uint8_t mode);

/**
 * @brief Send @p len bytes from @p out while @p len bytes come in to @p in, under one chip select.
 *
 * To read, send what the device wants meanwhile, often 0x00 or 0xFF. The bus
 * clocks whether or not a device is there, so a transfer never times out:
 * with nothing on the bus, @p in holds whatever the data-in pin floated at.
 * @param out The bytes to send; never NULL, even to read (the call refuses it).
 * @param in  Where the bytes received go; NULL only sends.
 * @param len 1 to KODE_SPI_XFER_MAX. A longer exchange is several transfers,
 *            each under its own chip select.
 * @return true when the transfer ran; false with kode_error() saying why.
 */
bool kode_spi_transfer(const uint8_t *out, uint8_t *in, size_t len);

/**
 * @brief End the SPI bus: the driver closes, then the pins and the controller go back.
 * @return true when it is ended; false with kode_error() saying why (also
 *         when it was not begun).
 */
bool kode_spi_end(void);

#define KODE_UART_BAUD_MIN   1200U   /**< Slowest UART rate, baud.                       */
#define KODE_UART_BAUD_MAX   921600U /**< Fastest UART rate, baud.                       */
#define KODE_UART_RX_BYTES   1024U   /**< The UART's receive ring: what waits for a read. */

/**
 * @brief Begin a UART sending on @p tx and receiving on @p rx: 8 data bits, no parity, 1 stop bit.
 *
 * Takes the lowest UART the app may have. From here on, what arrives waits in
 * a ring of KODE_UART_RX_BYTES for kode_uart_read(); bytes past a full ring
 * are lost. During a flash write (an install, a setting stored, a file
 * written to the internal storage) the UART's interrupt cannot run unless the
 * image sets CONFIG_UART_ISR_IN_IRAM, and bytes past the 128 its FIFO holds
 * are lost then; with the option set, the UART's driver and its ring sit in
 * internal RAM. All or nothing: a refusal leaves no pin lent and no
 * controller taken.
 * @param tx   The transmit pin, KODE_PIN_FIRST to KODE_PIN_LAST: the other
 *             side's receive.
 * @param rx   The receive pin, another one: the other side's transmit.
 * @param baud KODE_UART_BAUD_MIN to KODE_UART_BAUD_MAX.
 * @return true when the UART is up; false with kode_error() saying why (also
 *         when it is already begun).
 */
bool kode_uart_begin(uint8_t tx, uint8_t rx, uint32_t baud);

/**
 * @brief Send @p len bytes; returns once the last of them is queued in the UART.
 *
 * Blocks for about the time the bytes take on the line, less the last 128.
 * @param data The bytes.
 * @param len  How many; at least one.
 * @return How many were queued: @p len; 0 with kode_error() saying why.
 */
size_t kode_uart_write(const uint8_t *data, size_t len);

/**
 * @brief What came in: up to @p cap bytes, waiting up to @p ms for them.
 *
 * Returns as soon as @p cap bytes are in @p data, or when @p ms have gone by
 * since the call, with what came. 0 takes only what is already in the ring.
 * @param data Where the bytes go.
 * @param cap  Room in @p data; at least one.
 * @param ms   The longest wait, ms.
 * @return How many bytes are in @p data; 0 with kode_error() empty when
 *         nothing came, or saying why when the UART could not be read.
 */
size_t kode_uart_read(uint8_t *data, size_t cap, uint32_t ms);

/**
 * @brief End the UART: the driver closes and its ring goes, then the pins and the controller go back.
 * @return true when it is ended; false with kode_error() saying why (also
 *         when it was not begun).
 */
bool kode_uart_end(void);

/* ---- Infrared ---- */

/* 38 kHz infrared. Calls take turns; a path held from outside the standard layer is
 * waited for up to KODE_IR_BUSY_MS (or the read's own time), then the call fails.
 * The block is behind the C5 link: the first call of a boot that needs it (any
 * but kode_ir_last()) waits for the link to come up, and a read or a raw send
 * that cannot finish in its own time fails. */

#define KODE_IR_PROTOCOL_MAX 8U     /**< Bytes of a protocol name, NUL included.       */
#define KODE_IR_RAW_MAX      512U   /**< Durations in one raw frame: 256 marks and spaces. */
#define KODE_IR_BUSY_MS      5000U  /**< How long a call waits for the infrared path.  */

/** @brief One frame that was heard and read as a protocol. */
typedef struct {
    bool     ok;                              /**< A frame was heard.                          */
    char     protocol[KODE_IR_PROTOCOL_MAX];  /**< "NEC", or "unknown" for any other frame.     */
    uint32_t address;                         /**< The address; 0 for "unknown".               */
    uint32_t command;                         /**< The command; 0 for "unknown".               */
} kode_ir_code_t;

/** @brief One frame that was heard, as the lengths of its marks and spaces. */
typedef struct {
    uint32_t count;                 /**< Durations in @c us; 0 when nothing was heard.        */
    uint32_t carrier_hz;            /**< 38000: the receiver cannot measure a carrier; 0 when nothing was heard. */
    uint16_t us[KODE_IR_RAW_MAX];   /**< Microseconds: mark, space, mark, space, ...          */
} kode_ir_raw_t;

/**
 * @brief Send a frame of a protocol the board encodes, and wait until it is on its way.
 *
 * Blocks. Only "nec" (any case) is encoded. For anything else encode the frame
 * yourself and use kode_ir_send_raw().
 *
 * @param protocol "nec".
 * @param address  0 to 65535.
 * @param command  0 to 65535.
 * @return true when the frame went out; false with kode_error() saying why.
 */
bool kode_ir_send(const char *protocol, uint32_t address, uint32_t command);

/**
 * @brief Wait for one frame and read it as a protocol.
 *
 * Blocks for up to @p ms. A frame that is not NEC reads ok with the protocol
 * "unknown"; kode_ir_receive_raw() has its lengths.
 *
 * @param ms Milliseconds to wait, or KODE_FOREVER.
 * @return The frame; ok false with kode_error() saying why when none was heard.
 */
kode_ir_code_t kode_ir_receive(uint32_t ms);

/**
 * @brief The last frame the receiver heard, read as a protocol, without waiting for one.
 *
 * Blocks only while it asks. The frame may be one another task, an Active
 * Object or the console listened for.
 *
 * @return The frame; ok false with kode_error() saying why when none was heard yet.
 */
kode_ir_code_t kode_ir_last(void);

/**
 * @brief Wait for one frame and give the lengths of its marks and spaces.
 *
 * Blocks for up to @p ms. The result belongs to the calling task and is valid
 * until its next raw read; it is never NULL.
 *
 * @param ms Milliseconds to wait, or KODE_FOREVER.
 * @return The durations; count 0 with kode_error() saying why when none were heard.
 */
const kode_ir_raw_t *kode_ir_receive_raw(uint32_t ms);

/**
 * @brief Send a frame as the lengths of its marks and spaces, and wait until it is on its way.
 *
 * Blocks. The durations alternate mark, space, mark first; an odd count ends
 * on a mark.
 *
 * @param us         Microseconds, mark first.
 * @param n          1 to KODE_IR_RAW_MAX.
 * @param carrier_hz The carrier, above 0; 38000 for most remotes.
 * @return true when the frame went out; false with kode_error() saying why.
 */
bool kode_ir_send_raw(const uint16_t *us, uint32_t n, uint32_t carrier_hz);

/* Capture to a file and play it back. The file is text: "# kode ir capture v1", then a
 * blank line and one block per signal, "nec 0x0012 0x0034" or "raw 38000" and its durations. */

#define KODE_IR_FILE_SIGNALS_MAX 32U   /**< Signals one capture keeps and one file carries. */
#define KODE_IR_FILE_GAP_MS      100U  /**< Pause between two signals of a file, ms.        */

/**
 * @brief Start listening in the background and keep the first KODE_IR_FILE_SIGNALS_MAX signals.
 *
 * Blocks only for its turn. Until kode_ir_capture_stop(), reads fail at once
 * and sends still work.
 *
 * @param path "sd/..." or "flash/..." the capture is written to by the stop; the folder must exist.
 * @return true when the capture runs; false with kode_error() saying why.
 */
bool kode_ir_capture_start(const char *path);

/**
 * @brief End the capture and write the file.
 *
 * Blocks until the listening ends, about half a second, longer behind a send
 * in progress. Signals kept before a receiver error are still saved. If the
 * file cannot be written the capture is kept: fix the cause and stop again.
 *
 * @return true when the file is written; false with kode_error() saying why (no file when nothing was heard).
 */
bool kode_ir_capture_stop(void);

/**
 * @brief Send every signal of a capture file in order, KODE_IR_FILE_GAP_MS apart.
 *
 * Blocks. The whole file is read first: if any line is wrong nothing is sent.
 *
 * @param path "sd/..." or "flash/...".
 * @return true when all went out; false with kode_error() saying why.
 */
bool kode_ir_send_file(const char *path);

/* ---- NFC and RFID ---- */

/* 13.56 MHz tags. Calls take turns; a reader held from outside the standard layer is
 * waited for up to KODE_NFC_BUSY_MS, then the call fails. A call with a long wait holds
 * the reader, so a start, stop, emulation or profile call waits behind it. */

#define KODE_TAG_ID_MAX            32U   /**< Bytes of a tag's ID text, NUL included.          */
#define KODE_TAG_NAME_MAX          32U   /**< Bytes of a tag's model or protocol name, NUL included. */
#define KODE_NFC_TEXT_MAX          97U   /**< Bytes of an NFC text or link, NUL included.       */
#define KODE_NFC_TAG_DATA_MAX      95U   /**< Bytes of a text or link read from or written to a tag, NUL excluded. */
#define KODE_NFC_PROFILES_MAX      8U    /**< Emulation profiles an app can add.               */
#define KODE_NFC_PROFILE_NAME_MAX  16U   /**< Bytes of a profile name, NUL included.           */
#define KODE_NFC_BUSY_MS           5000U /**< How long a call waits for the reader.            */

/** @brief One tag that was found. */
typedef struct {
    bool ok;                       /**< A tag was found.                                    */
    char id[KODE_TAG_ID_MAX];      /**< Its ID as hex bytes, "04:A2:3B:91".                  */
    char name[KODE_TAG_NAME_MAX];  /**< Its model, such as "Ultralight / NTAG (Type-2)".     */
} kode_tag_t;

/**
 * @brief Read a tag in the background: a task keeps looking, and every tag it finds is an "nfc_tag" event.
 *
 * A tag left on the antenna is heard once a second. Calls that read or write
 * still work meanwhile and wait up to half a second for their turn. A second
 * start does nothing and answers true.
 *
 * @return true when it runs; false with kode_error() saying why (the Dot is emulating a card).
 */
bool kode_nfc_scan_start(void);

/**
 * @brief End the background reading.
 *
 * Blocks until the task ends, up to about 5 seconds. A second stop made
 * while one waits fails.
 *
 * @return true when nothing runs any more; false with kode_error() saying why.
 */
bool kode_nfc_scan_stop(void);

/**
 * @brief Wait for a tag and say which one it is.
 *
 * Blocks for up to @p ms. Fails at once while the Dot emulates a card.
 *
 * @param ms Milliseconds to wait, or KODE_FOREVER; 0 looks for none.
 * @return The tag; ok false with kode_error() saying why when none was found.
 */
kode_tag_t kode_nfc_read_id(uint32_t ms);

/**
 * @brief Wait for a tag and read the text or the link it carries.
 *
 * Blocks for up to @p ms. Fails at once while the Dot emulates a card. The text
 * belongs to the calling task and is valid until its next kode_nfc_read(); it is never NULL.
 *
 * @param type "text" or "uri" (any case): what the tag must carry.
 * @param ms   Milliseconds to wait, or KODE_FOREVER.
 * @return The text or link, at most KODE_NFC_TAG_DATA_MAX bytes; "" with
 *         kode_error() saying why on failure, also when the tag carries the
 *         other type, a longer message, or a card that cannot be read (a
 *         Mifare Classic).
 */
const char *kode_nfc_read(const char *type, uint32_t ms);

/**
 * @brief Wait for a tag and write a text or a link to it.
 *
 * Blocks for up to @p ms. Fails at once while the Dot emulates a card. Nothing is
 * written if it does not fit; a write that times out may have written part of the tag.
 *
 * @param type "text" or "uri" (any case).
 * @param data At most KODE_NFC_TAG_DATA_MAX (95) bytes; a longer one is refused, never cut.
 * @param ms   Milliseconds to wait, or KODE_FOREVER.
 * @return true when written; false with kode_error() saying why.
 */
bool kode_nfc_write(const char *type, const char *data, uint32_t ms);

/**
 * @brief Present the Dot as the card of a profile until kode_nfc_emulate_stop().
 *
 * Returns once the card is up. A card already presented is replaced. Refused
 * while kode_nfc_scan_start() reads.
 *
 * @param profile A name given to kode_nfc_add_profile().
 * @return true when the card is up; false with kode_error() saying why.
 */
bool kode_nfc_emulate_start(const char *profile);

/**
 * @brief Stop presenting the card; returns once the reader is free again.
 * @return true when nothing is presented any more (also when none was); false with kode_error() saying why.
 */
bool kode_nfc_emulate_stop(void);

/**
 * @brief Name a card the Dot can present.
 *
 * A profile with the same name, in any case, is replaced. There are no profiles to begin with.
 *
 * @param name KODE_NFC_PROFILE_NAME_MAX - 1 bytes at most; any case.
 * @param type "text" or "uri": a tag a phone reads; "uid": a card clone for an access reader.
 * @param data The text or link, 1 to KODE_NFC_TEXT_MAX - 1 bytes; or for "uid" the ID as 4, 7 or 10 hex bytes, "04:A2:3B:91".
 * @return true when added; false with kode_error() saying why.
 */
bool kode_nfc_add_profile(const char *name, const char *type, const char *data);

/* 125 kHz tags (EM4100). Calls take turns; a front end held from outside the standard layer
 * is waited for up to KODE_RFID_BUSY_MS, then the call fails. A call with a long wait holds
 * the front end, so an emulation call waits behind it. */

#define KODE_RFID_BUSY_MS  5000U  /**< How long a call waits for the LF front end.  */

/**
 * @brief Wait for an EM4100 tag and say which one it is.
 *
 * Blocks for up to @p ms. Fails at once while the Dot emulates a tag.
 *
 * @param ms Milliseconds to wait, or KODE_FOREVER; 0 looks for none.
 * @return The tag, its id as five hex bytes "01:02:03:04:05" and its name "EM4100";
 *         ok false with kode_error() saying why when none was found.
 */
kode_tag_t kode_rfid_read_id(uint32_t ms);

/**
 * @brief Write an ID onto a T5577 tag and read it back.
 *
 * Blocks for up to @p ms. Fails at once while the Dot emulates a tag.
 *
 * @param id Five hex bytes, "01:02:03:04:05"; ':', '-' and ' ' between them are optional.
 * @param ms Milliseconds the field stays up, one minute at most for any value; 0 writes none.
 * @return true when the tag read back the ID; false with kode_error() saying why.
 */
bool kode_rfid_write(const char *id, uint32_t ms);

/**
 * @brief Play an ID as an EM4100 tag until kode_rfid_emulate_stop().
 *
 * Returns once the tag plays. A tag already played is replaced.
 *
 * @param id Five hex bytes, "01:02:03:04:05".
 * @return true when the tag plays; false with kode_error() saying why.
 */
bool kode_rfid_emulate_start(const char *id);

/**
 * @brief Stop playing the tag; returns once the front end is free again.
 * @return true when nothing is played any more (also when none was); false with kode_error() saying why.
 */
bool kode_rfid_emulate_stop(void);

/* ---- Display brightness ---- */

/* The panel's level for as long as the app runs. It lives in the kode
 * component, so an app with no screen can set it and links no LVGL. */

#define KODE_DISPLAY_BRIGHTNESS_MIN 5U    /**< The lowest lit brightness, per cent; 0 is dark. */
#define KODE_DISPLAY_BRIGHTNESS_MAX 100U  /**< The highest brightness, per cent.               */

/**
 * @brief Set the display's brightness.
 *
 * Never stored: the brightness the user chose comes back when the app ends.
 * 0 darkens the glass; the panel and the touch keep running, so a touch still
 * reads. Waits its turn when the system is busy, up to 100 ms.
 *
 * @param pct 0, or KODE_DISPLAY_BRIGHTNESS_MIN to KODE_DISPLAY_BRIGHTNESS_MAX per cent.
 * @return true when the level is on its way; false with kode_error() saying why.
 */
bool kode_display_set_brightness(uint8_t pct);

/* ---- Touch ---- */

/*
 * The finger on the glass, in glass pixels: 410 x 502, origin top-left, as
 * the touch panel reports it. Three events, each with the point in x and y:
 * touch_pressed when a finger lands, touch_moved while it slides and
 * touch_released when it lifts. A drag reports up to about 100 points a
 * second, so the moves are coalesced: at most one touch_moved of a stroke
 * waits for the listeners, and a newer point takes its place: a drag never
 * fills the queue, and the other events still arrive.
 *
 * Nothing is asked of the touch panel until the first touch listener, wait or
 * kode_touch_read(); the points start then. With a screen on the glass a finger
 * gives both the screen's tap (kode_widget_was_pressed()) and these
 * events, which say nothing about what is under it.
 */

/* cppcheck-suppress misra-c2012-2.5 ; a name for the apps that listen, never used inside the SDK */
/** @brief "touch_pressed", naming the touch panel so the app links it. */
#define KODE_EVENT_TOUCH_PRESSED  kode_event_touch_pressed()
/* cppcheck-suppress misra-c2012-2.5 ; a name for the apps that listen, never used inside the SDK */
/** @brief "touch_moved", naming the touch panel so the app links it. */
#define KODE_EVENT_TOUCH_MOVED    kode_event_touch_moved()
/* cppcheck-suppress misra-c2012-2.5 ; a name for the apps that listen, never used inside the SDK */
/** @brief "touch_released", naming the touch panel so the app links it. */
#define KODE_EVENT_TOUCH_RELEASED kode_event_touch_released()

/**
 * @brief The touch panel's event name for a finger that lands; what KODE_EVENT_TOUCH_PRESSED calls.
 * @return "touch_pressed".
 */
const char *kode_event_touch_pressed(void);

/**
 * @brief The touch panel's event name for a finger that slides; what KODE_EVENT_TOUCH_MOVED calls.
 * @return "touch_moved".
 */
const char *kode_event_touch_moved(void);

/**
 * @brief The touch panel's event name for a finger that lifts; what KODE_EVENT_TOUCH_RELEASED calls.
 * @return "touch_released".
 */
const char *kode_event_touch_released(void);

/**
 * @brief Whether a finger is on the glass, and where.
 *
 * The first call asks for the touch panel's reports, so it reads no finger
 * until the panel reports one after that. Starts the layer's Active Object,
 * as the first event call does.
 *
 * @param x Receives the finger's x while one is down; untouched otherwise. May be NULL.
 * @param y Receives the finger's y while one is down; untouched otherwise. May be NULL.
 * @return true while a finger is down; false when none is, and when refused
 *         (kode_error() tells the two apart).
 */
bool kode_touch_read(int16_t *x, int16_t *y);

/* ---- Screen ---- */

/* One screen on the glass at a time: a header with a title, or none, and its
 * widgets under it, top to bottom and centred, in the order they are added.
 * Widgets added before any kode_screen_new() go on a screen with no header. A call
 * checks what it is given, hands the screen a copy and returns; while the
 * screen is behind it waits up to KODE_SCREEN_BUSY_MS, after any screen calls
 * of other tasks ahead of it, so a wait can be longer. The calls live in the
 * kode_screen component: an app that draws adds it to its REQUIRES, and one
 * that does not links no LVGL. Not available in the kodeOS image. */

#define KODE_WIDGETS_MAX      32U    /**< Widgets on one screen.                              */
#define KODE_WIDGET_TEXT_MAX  128U   /**< Bytes of a title or a widget's text, NUL included.  */
#define KODE_SCREEN_BUSY_MS   2000U  /**< How long a call waits while the screen is behind.   */
#define KODE_VALUE_NUMBER_MAX 10U    /**< Bytes of a value's number, NUL included.            */
#define KODE_VALUE_UNIT_MAX   16U    /**< Bytes of a value's unit, NUL included.              */
#define KODE_BUTTON_LABEL_MAX 32U    /**< Bytes of a button's label, NUL included.            */

/** @brief A widget of the screen, 0 for none; valid until the next kode_screen_new(). */
typedef uint32_t kode_widget_t;

/**
 * @brief Put a new screen on the glass in place of the last one.
 * @param title The header's title, at most KODE_WIDGET_TEXT_MAX - 1 bytes;
 *              NULL or "" for a screen with no header.
 * @return true when the screen is on its way; false with kode_error() saying why.
 */
bool kode_screen_new(const char *title);

/**
 * @brief Add a line of text under the screen's last widget; a long one wraps.
 * @param text At most KODE_WIDGET_TEXT_MAX - 1 bytes; copied.
 * @return The widget; 0 with kode_error() saying why, also when the screen
 *         already holds KODE_WIDGETS_MAX.
 */
kode_widget_t kode_screen_add_text(const char *text);

/**
 * @brief Add a value: a number in the kit's big face, its unit under it.
 *
 * The big face draws digits and a space; a sign or a point comes out in the
 * kit's title face, smaller. A number wider than the glass is clipped.
 *
 * @param number At most KODE_VALUE_NUMBER_MAX - 1 bytes; copied.
 * @param unit   At most KODE_VALUE_UNIT_MAX - 1 bytes; copied. NULL for none.
 * @return The widget; 0 with kode_error() saying why, also when the screen
 *         already holds KODE_WIDGETS_MAX.
 */
kode_widget_t kode_screen_add_value(const char *number, const char *unit);

/**
 * @brief Add a button: a pill with the label on it.
 * @param label At most KODE_BUTTON_LABEL_MAX - 1 bytes; copied. Another
 *              button of the screen may not spell it the same apart from ASCII
 *              case.
 * @return The widget; 0 with kode_error() saying why, also when the screen
 *         already holds KODE_WIDGETS_MAX.
 */
kode_widget_t kode_screen_add_button(const char *label);

/**
 * @brief Change what a text widget says, or a value's number, printf style.
 *
 * A button's label does not change.
 *
 * @param p   A widget of the current screen.
 * @param fmt The format; the text it makes is at most KODE_WIDGET_TEXT_MAX - 1
 *            bytes, and a value's number at most KODE_VALUE_NUMBER_MAX - 1.
 * @return true when the change is on its way; false with kode_error() saying
 *         why ("no such widget" for a widget of an older screen).
 */
bool kode_widget_set_text(kode_widget_t p, const char *fmt, ...) KODE_PRINTF_FORMAT(2, 3);

/**
 * @brief Paint a widget: a button's fill, a text's letters or a value's number.
 *
 * A button keeps its label readable: dark letters on a light fill, light ones
 * on a dark fill, and it still greys while a finger holds it. The colour stays
 * until the next kode_screen_new(). A canvas or an image has no colour of its own.
 *
 * @param p     A widget of the current screen.
 * @param color The colour, such as KODE_BLUE or kode_rgb(r, g, b); the top byte is ignored.
 * @return true when the change is on its way; false with kode_error() saying
 *         why ("no such widget" for a widget of an older screen).
 */
bool kode_widget_set_color(kode_widget_t p, kode_color_t color);

/**
 * @brief Whether a button was tapped since the last ask.
 *
 * Like kode_button_was_pressed(): taps between two asks are one answer, and
 * the first ask counts from the moment the button was added. A press that
 * travelled more than a tap's radius is no tap.
 *
 * @param w A button of the current screen.
 * @return true when it was tapped; false when not, or when @p w is not a
 *         button of the current screen (kode_error() says so: "no such
 *         widget", or "this widget is not a button").
 */
bool kode_widget_was_pressed(kode_widget_t w);

/* ---- Canvas ---- */

/* A canvas is a widget with pixels of its own: (0, 0) is its top-left corner.
 * It is the whole glass (header and all) or it fits the screen's body. The draw
 * calls run in the caller's task, clip to the canvas and show what was drawn
 * within a frame; draw over the old frame, as clearing first can show a blank
 * one. They fail, with kode_error() saying why, for a canvas of an older screen.
 * A canvas starts black and lasts until the next kode_screen_new(). */

#define KODE_CANVAS_WIDTH_MAX  410U  /**< Width of the glass: the full-screen canvas.          */
#define KODE_CANVAS_HEIGHT_MAX 502U  /**< Height of the glass.                                 */
#define KODE_CANVAS_BODY_W     346U  /**< Widest canvas among the widgets.                     */
#define KODE_CANVAS_BODY_H_TITLE 264U /**< Tallest among the widgets of a screen with a title.  */
#define KODE_CANVAS_BODY_H_BARE  422U /**< Tallest among the widgets of a screen with no header. */
#define KODE_CANVAS_MAX        4U    /**< Canvases on one screen.                              */
#define KODE_IMAGE_MAX         4U    /**< Images on one screen.                                */
#define KODE_DRAW_RANGE        ((int32_t)32767)  /**< Largest coordinate, size or radius, either way. */

/** @brief A canvas of the current screen, 0 for none. */
typedef uint32_t kode_canvas_t;

/** @brief Where text sits on its x. */
typedef enum {
    KODE_ALIGN_LEFT = 0,  /**< x is the text's left edge.  */
    KODE_ALIGN_CENTER,    /**< x is the text's middle.     */
    KODE_ALIGN_RIGHT      /**< x is the text's right edge. */
} kode_align_t;

/**
 * @brief Add a canvas under the screen's last widget.
 * @param width  KODE_CANVAS_WIDTH_MAX for the glass, else 1 to KODE_CANVAS_BODY_W.
 * @param height KODE_CANVAS_HEIGHT_MAX for the glass, else 1 to
 *               KODE_CANVAS_BODY_H_TITLE (a screen with a title) or
 *               KODE_CANVAS_BODY_H_BARE (none), less what the widgets above
 *               take (a text counts one line).
 * @return The canvas; 0 with kode_error() saying why, also when the screen
 *         already holds KODE_WIDGETS_MAX widgets or KODE_CANVAS_MAX canvases.
 */
kode_canvas_t kode_screen_add_canvas(uint32_t width, uint32_t height);

/**
 * @brief Fill the whole canvas.
 * @param c     A canvas of the current screen.
 * @param color The colour, such as KODE_BLUE; the top byte is ignored.
 * @return true when drawn; false with kode_error() saying why.
 */
bool kode_canvas_fill(kode_canvas_t c, kode_color_t color);

/**
 * @brief Set one pixel; one off the canvas is skipped.
 * @param c     A canvas of the current screen.
 * @param x     Column.
 * @param y     Row.
 * @param color The colour.
 * @return true when drawn; false with kode_error() saying why, also when a
 *         coordinate is beyond KODE_DRAW_RANGE.
 */
bool kode_draw_pixel(kode_canvas_t c, int32_t x, int32_t y, kode_color_t color);

/**
 * @brief Draw a line from (x0, y0) to (x1, y1), one pixel wide.
 *
 * Only the part on the canvas shows. Where the line is half way between two
 * pixels it takes the one further along from (x0, y0).
 *
 * @param c     A canvas of the current screen.
 * @param x0    First point's column.
 * @param y0    First point's row.
 * @param x1    Last point's column.
 * @param y1    Last point's row.
 * @param color The colour.
 * @return true when drawn; false as kode_draw_pixel().
 */
bool kode_draw_line(kode_canvas_t c, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                    kode_color_t color);

/**
 * @brief Draw a rectangle's one-pixel outline, its top-left corner at (x, y).
 * @param c     A canvas of the current screen.
 * @param x     Left column.
 * @param y     Top row.
 * @param w     Width in pixels; 0 draws nothing.
 * @param h     Height in pixels; 0 draws nothing.
 * @param color The colour.
 * @return true when drawn; false as kode_draw_pixel(), also for a negative
 *         width or height.
 */
bool kode_draw_rect(kode_canvas_t c, int32_t x, int32_t y, int32_t w, int32_t h,
                    kode_color_t color);

/**
 * @brief Fill a rectangle, its top-left corner at (x, y).
 * @param c     A canvas of the current screen.
 * @param x     Left column.
 * @param y     Top row.
 * @param w     Width in pixels; 0 draws nothing.
 * @param h     Height in pixels; 0 draws nothing.
 * @param color The colour.
 * @return true when drawn; false as kode_draw_rect().
 */
bool kode_fill_rect(kode_canvas_t c, int32_t x, int32_t y, int32_t w, int32_t h,
                    kode_color_t color);

/**
 * @brief Draw a circle's one-pixel outline around (x, y).
 * @param c     A canvas of the current screen.
 * @param x     Centre column.
 * @param y     Centre row.
 * @param r     Radius in pixels; 0 is one pixel.
 * @param color The colour.
 * @return true when drawn; false as kode_draw_pixel(), also for a negative
 *         radius.
 */
bool kode_draw_circle(kode_canvas_t c, int32_t x, int32_t y, int32_t r, kode_color_t color);

/**
 * @brief Fill the disc of radius @p r around (x, y).
 * @param c     A canvas of the current screen.
 * @param x     Centre column.
 * @param y     Centre row.
 * @param r     Radius in pixels; 0 is one pixel.
 * @param color The colour.
 * @return true when drawn; false as kode_draw_circle().
 */
bool kode_fill_circle(kode_canvas_t c, int32_t x, int32_t y, int32_t r, kode_color_t color);

/**
 * @brief Write a line of text on a canvas, in the kit's body font.
 *
 * The screen draws the text and the call returns once it has, so a shape
 * drawn after the call lands on top of the text. A new line in @p text ('\n')
 * starts the next line under the last, lined up as the first is. Only the
 * part on the canvas shows. The font has ASCII, the middle dot and the degree
 * sign. Waits for a free slot, the screen's queue and the drawing in turn, each
 * up to KODE_SCREEN_BUSY_MS: at worst about three times that.
 *
 * @param c     A canvas of the current screen.
 * @param x     Column of the text's left edge, middle or right edge, by @p align.
 * @param y     Row of the top of the first line.
 * @param text  At most KODE_WIDGET_TEXT_MAX - 1 bytes; copied.
 * @param color The colour, such as KODE_WHITE; the top byte is ignored.
 * @param align Where @p x sits on each line.
 * @return true when the text is drawn; false with kode_error() saying why,
 *         also when a coordinate is beyond KODE_DRAW_RANGE, "the text could not
 *         be drawn" when the screen dropped it, and "the screen is busy" when
 *         it was not drawn in time (it may still appear).
 */
bool kode_draw_text(kode_canvas_t c, int32_t x, int32_t y, const char *text, kode_color_t color,
                    kode_align_t align);

/**
 * @brief Draw a PNG file on a canvas, blending its transparency.
 *
 * Reads and decodes the file in the caller's task, which waits for it; then
 * draws it with its top-left corner at (x, y), only the part on the canvas
 * showing. Handles gray, gray and alpha, RGB, palette and RGBA PNGs of 1 to 16
 * bits that are not interlaced, no wider than KODE_CANVAS_WIDTH_MAX or taller
 * than KODE_CANVAS_HEIGHT_MAX.
 *
 * @param c    A canvas of the current screen.
 * @param x    Column of the picture's left edge.
 * @param y    Row of its top edge.
 * @param path "sd/..." or "flash/...".
 * @return true when drawn; false with kode_error() saying why: the file's own
 *         error, "<path>: not a PNG", "<path>: too big for the glass (WxH)"
 *         with the picture's size, "<path>: damaged PNG", "<path>: interlaced
 *         PNGs are not supported", "<path>: out of memory", or a stale canvas.
 */
bool kode_draw_png(kode_canvas_t c, int32_t x, int32_t y, const char *path);

/* ---- Image ---- */

/**
 * @brief Add a PNG file to the screen as a picture under the last widget.
 *
 * Reads and decodes the file in the caller's task, which waits for it. It
 * is the glass's size (KODE_CANVAS_WIDTH_MAX by KODE_CANVAS_HEIGHT_MAX) or fits
 * the screen's body like a canvas; transparency shows the screen. The same PNGs as
 * kode_draw_png().
 *
 * @param path "sd/..." or "flash/...".
 * @return The widget; 0 with kode_error() saying why, as kode_draw_png() does
 *         for the file, also when the screen already holds KODE_WIDGETS_MAX
 *         widgets or KODE_IMAGE_MAX images, or the picture is the wrong size.
 */
kode_widget_t kode_screen_add_image(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* KODE_H_ */
