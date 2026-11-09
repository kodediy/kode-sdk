/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_tag.h
 * @brief Shared by kode.h's infrared, NFC and RFID calls. Private to this component.
 */
#ifndef KODE_TAG_H_
#define KODE_TAG_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"
#include "kode.h"

/** @brief What a task keeps for its infrared and tag calls; one PSRAM block. */
struct kode_tag_state {
    kode_ir_raw_t ir_raw;                       /**< The last kode_ir_receive_raw() result.  */
    char          nfc_text[KODE_NFC_TEXT_MAX];  /**< The last NFC read's text.            */
};

/**
 * @brief Write bytes as upper-case hex joined by colons, "04:A2:3B".
 * @param bytes The bytes; may be NULL when @p n is 0.
 * @param n     How many.
 * @param out   Receives the text and its NUL.
 * @param cap   Bytes of @p out.
 * @return true when it fits; false for a NULL pointer or too little room.
 */
bool kode_tag_hex_format(const uint8_t *bytes, uint32_t n, char *out, uint32_t cap);

/**
 * @brief Read hex bytes, with or without ':', '-' or ' ' between bytes, in any case.
 * @param text The text.
 * @param out  Receives the bytes.
 * @param cap  Bytes of @p out.
 * @param n    Out: how many; written only on success.
 * @return true on success; false for a NULL pointer, an odd digit count, any
 *         other character, or more than @p cap bytes.
 */
bool kode_tag_hex_parse(const char *text, uint8_t *out, uint32_t cap, uint32_t *n);

/**
 * @brief The calling task's tag state, taken from PSRAM on first use.
 * @return The state; NULL if PSRAM has none to give.
 */
struct kode_tag_state *kode_tag_state_ensure(void);

/**
 * @brief Free a tag state.
 * @param s The state; NULL does nothing.
 */
void kode_tag_state_free(struct kode_tag_state *s);

/**
 * @brief The time a call has left, at most @p cap.
 * @param start When the call began, from kode_sys_time_ms().
 * @param ms    The time it was given, or KODE_FOREVER.
 * @param cap   The most to answer; KODE_FOREVER for no cap.
 * @return Milliseconds; 0 when the time is up; @p cap when @p ms is KODE_FOREVER.
 */
uint32_t kode_tag_window(uint32_t start, uint32_t ms, uint32_t cap);

/** @brief What a module hands kode_tag_emu_release() to let go of its emulation. */
typedef struct {
    kode_status_t (*stop)(void);       /**< Asks the owner to stop; DENIED while another holder has the claim.  */
    bool          (*playing)(void);    /**< Whether the emulation is still up.                                  */
    void          (*fail)(kode_status_t rc);  /**< Sets the error for a stop that failed.                       */
    const char     *releasing;         /**< The error text of an emulation that is slow to end.                 */
    uint32_t        bound_ms;          /**< The total time the stop and the wait have.                          */
} kode_tag_emu_t;

/**
 * @brief Stop an emulation and wait for it to end, in @c bound_ms in all; the lock is held.
 * @param e What to ask.
 * @return true when no emulation is up; false with the error set.
 */
bool kode_tag_emu_release(const kode_tag_emu_t *e);

#endif /* KODE_TAG_H_ */
