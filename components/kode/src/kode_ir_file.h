/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_ir_file.h
 * @brief The text file a capture is kept in. Pure: no lock, no system call. Private to this component.
 */
#ifndef KODE_IR_FILE_H_
#define KODE_IR_FILE_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode.h"

/** @brief The highest carrier a file may name, Hz. */
#define KODE_IR_FILE_CARRIER_MAX 1000000U

/** @brief The highest NEC address, NEC command or raw duration a file may carry. */
#define KODE_IR_FILE_VALUE_MAX 65535U

/** @brief One signal of a capture file: a decoded NEC frame or raw durations. */
typedef struct {
    bool     raw;                    /**< Raw durations; false: NEC.                 */
    uint32_t address;                /**< NEC address, 0 to 65535.                   */
    uint32_t command;                /**< NEC command, 0 to 65535.                   */
    uint32_t carrier_hz;             /**< Raw carrier, 1 to KODE_IR_FILE_CARRIER_MAX. */
    uint32_t count;                  /**< Raw durations, 1 to KODE_IR_RAW_MAX.       */
    uint16_t us[KODE_IR_RAW_MAX];    /**< Raw durations, mark first, each 1 or more. */
} kode_ir_signal_t;

/**
 * @brief Write signals as the capture file's text.
 * @param sig The signals.
 * @param n   How many, 0 to KODE_IR_FILE_SIGNALS_MAX.
 * @param out Receives the text, NUL-terminated when room is left.
 * @param cap Bytes of @p out.
 * @return Bytes written, the NUL not counted; 0 for a NULL pointer, a signal out of range or too little room.
 */
uint32_t kode_ir_file_format(const kode_ir_signal_t *sig, uint32_t n, char *out, uint32_t cap);

/**
 * @brief Read the capture file's text.
 * @param text     The text; not NUL-terminated.
 * @param len      Its bytes.
 * @param sig      Receives the signals.
 * @param cap      How many @p sig holds.
 * @param n        Out: how many were read; written on success only.
 * @param bad_line Out: the line at fault, from 1; written on failure only.
 * @param why      Out: what is wrong, a static text; written on failure only.
 * @return true when every line is right (an empty text reads 0 signals); false with @p bad_line and @p why set.
 */
bool kode_ir_file_parse(const char *text, uint32_t len, kode_ir_signal_t *sig, uint32_t cap,
                        uint32_t *n, uint32_t *bad_line, const char **why);

/**
 * @brief The longest text KODE_IR_FILE_SIGNALS_MAX raw signals of KODE_IR_RAW_MAX durations format to.
 * @return Bytes, without a NUL.
 */
uint32_t kode_ir_file_size_max(void);

#endif /* KODE_IR_FILE_H_ */
