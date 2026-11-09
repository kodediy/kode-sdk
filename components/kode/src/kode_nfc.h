/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_nfc.h
 * @brief kode.h's NFC reader timing. Private to this component.
 */
#ifndef KODE_NFC_H_
#define KODE_NFC_H_

#include "kode.h"

/** @brief How long one window of the NFC reader listens, ms: the poll task's and a long call's. */
#define KODE_NFC_POLL_MS 500U

/** @brief How long the poll task rests after a tag, so one left on the antenna is heard once a second, ms. */
#define KODE_NFC_REPEAT_MS 1000U

/** @brief The longest kode_nfc_scan_stop() waits for the poll task to end, ms: a window and a wait for the reader. */
#define KODE_NFC_STOP_MS (KODE_NFC_POLL_MS + KODE_NFC_BUSY_MS)

#endif /* KODE_NFC_H_ */
