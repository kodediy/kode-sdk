/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_kdrv_rfid_lf.h
 * @brief The control surface of the faked LF front end (fake_kdrv_rfid_lf.c).
 */
#ifndef FAKE_KDRV_RFID_LF_H_
#define FAKE_KDRV_RFID_LF_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Forget the worker check, the counters and the staged window. */
void fake_rfid_reset(void);

/**
 * @brief Say what the next READ job decodes.
 * @param rc     The window's result as an int (0 = KODE_OK, else KODE_TIMEOUT).
 * @param id     The 5-byte ID it hands back on a decode. May be NULL.
 * @param rf_div The bit rate it reports.
 */
void fake_rfid_set_window(int rc, const uint8_t id[5], uint8_t rf_div);

/** @brief How many READ jobs have run since the reset. */
int fake_rfid_read_calls(void);

/** @brief How many CLONE jobs have run since the reset. */
int fake_rfid_clone_calls(void);

/**
 * @brief Make the next CLONE jobs read back an ID other than the one written.
 * @param mismatch true for a different read-back, false for a matching one.
 */
void fake_rfid_set_clone_mismatch(bool mismatch);

/**
 * @brief The ID the last CLONE job was asked to write.
 * @param out Out: the 5 bytes.
 */
void fake_rfid_last_clone_id(uint8_t out[5]);

/**
 * @brief The boost of the last READ or CLONE's field: false for a READ, true
 *        for a CLONE. A CAPTURE raises no field and leaves it as it was.
 */
bool fake_rfid_last_boost(void);

/**
 * @brief Say what the next CAPTURE job answers, and what its window holds.
 *
 * On KODE_OK the job's statistics and waveform are what the real
 * krfid_wave_stats() and krfid_wave_decimate() make of @p samples, written
 * through the job's pointers as the driver writes them - a NULL one skipped,
 * and the waveform filled even when the count's pointer is NULL; on any other
 * @p rc the pointers are left alone. The window is taken as it is, whatever
 * the job's @c ms: it is what the ADC delivered. Until a case stages one, a
 * capture answers KODE_ERROR over an empty window, as an ADC that delivers
 * nothing does.
 *
 * @param rc      The job's result as an int (a kode_status_t).
 * @param samples The window, copied; at most KRFID_EM4100_MAX_SAMPLES are
 *                kept. May be NULL, which stages an empty window.
 * @param n       How many samples @p samples holds.
 */
void fake_rfid_set_capture(int rc, const uint16_t *samples, size_t n);

/**
 * @brief How many captures the worker check let through since the reset - a
 *        refused one is not counted.
 */
int fake_rfid_capture_calls(void);

/**
 * @brief How many times kdrv_rfid_lf_field_off() took the front end down since
 *        the reset. The fake's worker never calls it, so every call is the
 *        AO's own.
 */
int fake_rfid_field_off_calls(void);

/** @brief How many times kdrv_rfid_lf_rest() was called since the reset. */
int fake_rfid_rest_calls(void);

/** @brief What the next certification carriers answer; fake_rfid_reset()
 *         puts it back to KODE_OK. */
void fake_rfid_set_cert_rc(int rc);

/**
 * @brief What the next EMULATE jobs answer when nothing is emulating;
 *        fake_rfid_reset() puts it back to KODE_OK. Anything but KODE_OK
 *        leaves the loop not running, as the driver does.
 */
void fake_rfid_set_emulate_rc(uint32_t rc);

/** @brief How many EMULATE jobs reached the driver since the reset. */
uint32_t fake_rfid_emulate_calls(void);

/** @brief How many EMU_STOP jobs reached the driver since the reset. */
uint32_t fake_rfid_emu_stop_calls(void);

/** @brief Whether the faked loop is running: an EMULATE answered KODE_OK and
 *         no EMU_STOP has run since. */
bool fake_rfid_emulating(void);

/**
 * @brief The ID the last EMULATE job carried.
 * @param out Out: the 5 bytes.
 */
void fake_rfid_last_emulate_id(uint8_t out[5]);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_KDRV_RFID_LF_H_ */
