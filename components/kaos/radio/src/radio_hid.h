/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file radio_hid.h
 * @brief Package-private: AO_Radio's HID door, the reports it queues and kodeOS's stored choice of kinds.
 *
 * The HID service is the driver's (kdrv_ble_hid.h) and is served only when
 * asked. This module is the part of AO_Radio that takes the reports the
 * door posts (ble_hid_report_post(), kaos.h) and hands them to the driver,
 * and, in kodeOS alone, the setting `ble_hid`: the kinds kodeOS asks for
 * before Bluetooth registers its table. An app asks by code and reads no
 * setting.
 */
#ifndef RADIO_HID_H_
#define RADIO_HID_H_

#include <stdbool.h>
#include <stdint.h>

#include "kqp.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The words of the setting `ble_hid`: each word's place is its kinds. */
#define RADIO_HID_WORDS_N  4U

/** @brief The setting's words: "off", "mouse", "keyboard", "both". */
extern const char *const RADIO_HID_WORDS[RADIO_HID_WORDS_N];

/**
 * @brief BLE_HID_REPORT_REQ_SIG: queue the report for the host and wake the drain.
 *
 * A report waits for no job of the worker: a Wi-Fi scan lasts half a minute
 * and a mouse cannot stop for it. The drain is left alone only while the job
 * in flight starts or stops the Bluetooth host, which the wake-up would
 * race; that job's end wakes it (radio_hid_job_end()). An event of no bytes
 * is the wake-up's knock (RADIO_KNOCK_HID), posted for a report of zeroes
 * that is owed: it is taken, queues nothing and only wakes the drain.
 *
 * @param e      Any event.
 * @param moving Whether the job in flight starts or stops the Bluetooth host.
 * @return true when @p e was a report and has been handled.
 */
bool radio_hid_event(QEvt const * const e, bool moving);

/**
 * @brief A job of the worker ended: wake the drain for the reports queued
 *        meanwhile, and, once the table serves HID, hear the battery it
 *        reports and hand its last reading down.
 *
 * The HID service carries the Battery service, whose level is AO_Power's.
 * The table is built by a job, so its end is where AO_Radio, on its own
 * thread, first knows that HID is served.
 *
 * @param me AO_Radio.
 */
void radio_hid_job_end(QActive const * const me);

/**
 * @brief Ask the driver for the kinds the setting `ble_hid` stores, at AO_Radio's start.
 *
 * kodeOS alone; in an app it does nothing.
 */
void radio_hid_boot(void);

/**
 * @brief CONFIG_REQ_SIG for `ble_hid`: read the stored kinds, or store them
 *        and ask the driver for them.
 *
 * The kinds hold from the next boot once Bluetooth has registered its table,
 * and the log says which.
 *
 * @param me AO_Radio, the answer's sender.
 * @param r  The request.
 * @return true when @p r named `ble_hid` and has been answered; always false in an app.
 */
bool radio_hid_config(QActive * const me, KodeConfigReqEvt const * const r);

#ifdef __cplusplus
}
#endif

#endif /* RADIO_HID_H_ */
