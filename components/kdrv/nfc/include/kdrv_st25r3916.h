/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_st25r3916.h
 * @brief The NFC reader on the Antenna PCB, and the port ST's RFAL runs on.
 */
#ifndef KDRV_ST25R3916_H_
#define KDRV_ST25R3916_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "knfc_emulate.h"
#include "knfc_field.h"
#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Amplitude measurements one kdrv_st25r3916_listen_step() takes. */
#define KNFC_LISTEN_BURST 16U

/** @brief What IC_IDENTITY reads on this board: ic_type 6, revision 1. */
#define KDRV_ST25R3916_IDENTITY   0x31U

/** @brief IC_IDENTITY's type field. */
#define KDRV_ST25R3916_IC_TYPE_MASK  0xF8U
/** @brief IC_IDENTITY's revision field. */
#define KDRV_ST25R3916_IC_REV_MASK   0x07U
/** @brief The ST25R3916B's type code (6 << 3): the part this driver is built
 *         for (kdrv's CMakeLists.txt defines ST25R3916B). The plain ST25R3916's
 *         code, 5 << 3, is not one: RFAL refuses it in this build. */
#define KDRV_ST25R3916_IC_TYPE_B     0x30U
/** @brief The oldest ST25R3916B revision RFAL accepts. */
#define KDRV_ST25R3916_IC_REV_MIN    1U

/** @brief The longest NFCID1 there is. */
#define KDRV_ST25R3916_UID_MAX    10U

/** @brief The largest APDU kdrv_st25r3916_apdu() will carry either way. */
#define KDRV_ST25R3916_APDU_MAX   512U

/**
 * @brief Bring the reader up: SPI, the interrupt line, and RFAL.
 *
 * Opens SPI2 at 1 MHz in mode 1 with CS driven by hand - RFAL chains several
 * transfers under one CS, so the peripheral cannot own it - attaches the
 * interrupt through khal_gpio, and calls rfalNfcInitialize(), which is what
 * reads IC_IDENTITY and refuses a chip that is not an ST25R3916B.
 *
 * Energises nothing. The field stays down until somebody asks for it.
 *
 * @return KODE_OK when the chip answered. KODE_ERROR when it did not, which
 *         on this board usually means the Antenna PCB is not fitted; the
 *         driver then reports absent rather than broken.
 */
kode_status_t kdrv_st25r3916_init(void);

/** @brief Did kdrv_st25r3916_init() find the chip? */
bool kdrv_st25r3916_present(void);

/**
 * @brief Power the reader down, for a board with no owner for it.
 *
 * No RFAL: SPI2 is opened for two raw transfers - IC_IDENTITY, then the Set
 * Default command, which leaves the chip as a power-on does (power-down, the
 * oscillator and the field off) - and left open, the reader's alone. For
 * kaos_park_nfc() alone; never with AO_Nfc running.
 *
 * @return KODE_OK with the chip powered down; KODE_ERROR when no ST25R3916
 *         answered or the bus refused.
 */
kode_status_t kdrv_st25r3916_park(void);

/**
 * @brief The IC_IDENTITY register as it was read at init.
 *
 * @param out Out: the register. Written only on KODE_OK.
 * @return KODE_OK, or KODE_DENIED for a NULL @p out or an absent chip.
 */
kode_status_t kdrv_st25r3916_identity(uint8_t *out);

/**
 * @brief Energise the field and start looking for an NFC-A tag.
 *
 * Arms RFAL's discovery state machine. It does not advance on its own:
 * kdrv_st25r3916_worker() is what turns it, and nothing is found until it
 * has been called enough times.
 *
 * @return KODE_OK when discovery is armed; KODE_DENIED with no chip or while a
 *         listen window is open; KODE_ERROR when RFAL refused.
 */
kode_status_t kdrv_st25r3916_discover_start(void);

/** @brief Advance RFAL's state machine one step. Cheap, and never blocks. */
void kdrv_st25r3916_worker(void);

/** @brief Has a tag been activated since discovery started? */
bool kdrv_st25r3916_activated(void);

/**
 * @brief What the activated tag said about itself.
 *
 * Reports the anticollision result and nothing derived from it: which NFC
 * Forum tag type this is, is knfc_tag.c's reading of @p sak, because that is
 * protocol knowledge and this file holds chip knowledge.
 *
 * @param uid     Out: NFCID1, at least KDRV_ST25R3916_UID_MAX bytes.
 * @param uid_len Out: how many of them are real - 4, 7 or 10.
 * @param atqa    Out: SENS_RES, as two bytes in host order.
 * @param sak     Out: SEL_RES.
 * @return KODE_OK, KODE_DENIED for a NULL argument or an absent chip, and
 *         KODE_ERROR when nothing is activated.
 */
kode_status_t kdrv_st25r3916_active_tag(uint8_t *uid, uint8_t *uid_len,
                                        uint16_t *atqa, uint8_t *sak);

/**
 * @brief Drop the tag, stop discovery, and take the field down.
 *
 * An open listen window is closed first, as kdrv_st25r3916_listen_end()
 * closes it, and its stats are discarded: off leaves nothing open behind it.
 */
void kdrv_st25r3916_field_off(void);

/**
 * @brief Raise a continuous 13.56 MHz field at maximum drive, for FCC bench use.
 *
 * The certification path (FCC 15.225): the reader wakes from rest, its Tx
 * driver is set to the least RFO resistance - the most drive the part can
 * make - and the field is raised and held, with no discovery, no timeout and
 * no tag ever activated, until kdrv_st25r3916_field_off() takes it down. That
 * call also restores the drive strength to the value discovery uses, so an
 * ordinary read still works afterwards.
 *
 * With @p modulated false the field is a bare unmodulated carrier, for the
 * fundamental field-strength and spurious measurements. With it true the
 * reader's ASK modulation is applied continuously - a fixed pattern
 * retransmitted from a driver-internal task - for the occupied / 20 dB
 * bandwidth measurement.
 *
 * Only compiled when CONFIG_KODEOS_NFC_CERT is set; the symbol is absent in a
 * production build, which is why nothing outside the certification app links
 * a maximum-field capability.
 *
 * @param modulated false for a bare carrier, true for a continuously
 *        ASK-modulated one.
 * @return KODE_OK when the field is up; KODE_DENIED with no chip fitted or
 *         while a listen window is open, which is left open; KODE_ERROR when
 *         the reader refused to energise.
 */
kode_status_t kdrv_st25r3916_cert_carrier(bool modulated);

/**
 * @brief Open a listen window: the transmitter held off, the external field
 *        detector on, the receive chain set for amplitude measurement, the
 *        stats cleared.
 *
 * Wakes the reader from rest, which waits for the oscillator to settle
 * (700 us in the datasheet, bounded by RFAL at 10 ms), and turns its field
 * off, then reads the Operation control register back: a transmitter still
 * enabled there refuses the window, which must never measure the Dot's own
 * drive. It switches the external field detector to manual peer detection -
 * the threshold the datasheet gives for sensing another device's field, with
 * nothing automatic behind it - and sets the receive chain once for the whole
 * window: both receive channels (rx_chn 0), the peak detector rather than the
 * mixer (RX_CONF2, DS13541 section 4.5.15, p. 87) and regulator-shaped AM off
 * (rgs_am 0, which section 4.5.49, p. 113, asks for when amplitude is
 * measured from tx_en=0). So a reading does not depend on which technology
 * the reader last ran. The operation mode is left alone: the window is a
 * listener, not an initiator. Every bit it changes is remembered and put back
 * when the window closes.
 *
 * @return KODE_OK with the window open; KODE_DENIED with no chip, while a field
 *         is up (a read, a write or a certification carrier holds it), while
 *         a window is already open, or when the transmitter would not go off;
 *         KODE_ERROR when the reader would not wake or refused a register
 *         write. Every refusal after the wake leaves the reader at rest.
 */
kode_status_t kdrv_st25r3916_listen_begin(void);

/**
 * @brief Take one burst: one read of the external field detector, then
 *        KNFC_LISTEN_BURST amplitude measurements, folded into the window's
 *        stats.
 *
 * Each measurement is one Measure amplitude direct command, whose conversion
 * takes at most 25 us (DS13541 section 4.4.8, p. 65), then a wait for the
 * chip's end-of-command interrupt and a read of the A/D output register. The
 * transmitter stays off throughout, so each byte is the outside field's
 * amplitude at the antenna, 13.02 mVpp a step. The SPI transfers around the
 * conversion are most of what a step costs. The detector is read before the
 * burst's first conversion. Called once per AO_Nfc tick, and returns when the
 * burst is folded.
 *
 * The wait for each conversion's end spins without yielding, so the caller
 * holds its CPU for the whole step: at most KNFC_LISTEN_BURST waits of 10 ms
 * when the chip stops answering, and in practice what the SPI transfers
 * cost. Every step is timed and the window keeps its longest, which
 * kdrv_st25r3916_listen_end() logs; a step that fails logs its own duration.
 *
 * @return KODE_OK with the burst folded; KODE_DENIED when no window is open;
 *         KODE_ERROR when a conversion did not signal its end within 10 ms or
 *         the reader refused a transfer, which is a front end that stopped
 *         answering. Nothing of that burst is folded, and the window ends:
 *         the receive chain and the detector are put back, the reader rests,
 *         and a later kdrv_st25r3916_listen_end() answers KODE_DENIED.
 */
kode_status_t kdrv_st25r3916_listen_step(void);

/**
 * @brief Close the window: copy its stats out, put the receive chain and the
 *        external field detector back as the window found them, and let the
 *        reader rest.
 *
 * Logs at debug how many bursts the window folded and its longest step, as
 * `listen: <n> bursts, step max <us> us`, under the `kdrv.nfc` tag - the
 * wall time one step holds AO_Nfc for.
 *
 * @param out Out: the window's stats, as knfc_field_close() gives them.
 *            Written whenever a window was open. NULL discards them.
 * @return KODE_OK with the window closed; KODE_ERROR when a register could not
 *         be put back, with @p out written and the window closed all the
 *         same; KODE_DENIED when no window is open, with @p out untouched.
 */
kode_status_t kdrv_st25r3916_listen_end(knfc_field_t *out);

/* ---- Emulation: the transmitter off, the chip a passive target ------------
 *
 * The mirror of discovery. rfalListenStart() loads the card's identity into
 * the ST25R3916B's passive-target memory, and the chip answers REQA,
 * anticollision and SELECT in hardware, with the Dot's own field never raised.
 * A Type-2 READ has to be answered inside about 87 microseconds, which no
 * Active Object tick can promise, so a driver-owned worker task pumps RFAL's
 * listen worker every millisecond and serves the READs, above every AO's
 * reach the same way the interrupt servicer is. AO_Nfc arms and disarms it and
 * samples its progress; it does not pump it.
 *
 * Emulation is exclusive with every field the reader raises: it refuses to
 * start while a field or a listen window is up, and discovery, a listen window
 * and the certification carrier refuse to start while it runs.
 */

/**
 * @brief Start presenting @p card as a passive NFC-A target.
 *
 * Builds the identity and Type-2 image (knfc_emu_img.c), arms RFAL listen
 * mode, and spawns the worker that pumps it and answers Type-2 READs. The
 * field stays down: a passive target never raises one.
 *
 * @param card What to present. Copied here; the pointer need not outlive the
 *             call.
 * @return KODE_OK with emulation running; KODE_DENIED with no chip fitted or
 *         while a field or listen window holds the reader; KODE_ERROR when
 *         RFAL refused listen mode or the worker would not start;
 *         KODE_NO_RESOURCES when the card's message does not fit the image.
 */
kode_status_t kdrv_st25r3916_emulate_start(const knfc_emu_card_t *card);

/**
 * @brief Stop emulating, without waiting.
 *
 * Signals the worker to end; it stops RFAL listen mode, rests the reader and
 * clears the window's active flag on its own, within a session's end. This
 * returns at once so an Active Object may call it, and
 * kdrv_st25r3916_emulate_active() reports when the reader is actually free
 * again.
 *
 * @return KODE_OK, or KODE_DENIED with no chip fitted or no emulation running.
 */
kode_status_t kdrv_st25r3916_emulate_stop(void);

/** @brief Whether an emulation window still holds the reader. */
bool kdrv_st25r3916_emulate_active(void);

/**
 * @brief Copy the running window's statistics.
 *
 * Safe from any task: the worker keeps them under a spinlock and this copies
 * them under the same one. All zero, with @c active false, when nothing is
 * emulating.
 *
 * @param out Out: the statistics. Written whenever it is non-NULL.
 */
void kdrv_st25r3916_emulate_snapshot(knfc_emu_stat_t *out);

/**
 * @brief One Type-2 read: four blocks from @p block, sixteen bytes.
 *
 * A Type-2 READ always returns four blocks whatever was asked for, which is
 * why @p cap is sixteen and not four.
 *
 * @param block The first block number.
 * @param buf   Out: at least @p cap bytes.
 * @param cap   How many bytes @p buf holds; 16 gets the whole answer.
 * @param got   Out: how many bytes arrived. May be NULL.
 * @return KODE_OK, KODE_DENIED for a bad argument or an absent chip,
 *         KODE_TIMEOUT when the tag did not answer, KODE_ERROR otherwise.
 */
kode_status_t kdrv_st25r3916_t2t_read(uint8_t block, uint8_t *buf,
                                      uint16_t cap, uint16_t *got);

/**
 * @brief One Type-2 write: exactly four bytes into @p block.
 *
 * @param block The block number.
 * @param data  The four bytes.
 * @return KODE_OK, KODE_DENIED for a NULL @p data or an absent chip,
 *         KODE_TIMEOUT when the tag did not acknowledge, KODE_ERROR
 *         when it answered something other than an ACK.
 */
kode_status_t kdrv_st25r3916_t2t_write(uint8_t block, const uint8_t *data);

/**
 * @brief One ISO-DEP exchange: send an APDU, get the response.
 *
 * The tag must be activated and must have come up as ISO-DEP; a Type-2 tag
 * refuses here rather than answering nonsense.
 *
 * @param tx     The command APDU.
 * @param tx_len Its length, at most KDRV_ST25R3916_APDU_MAX.
 * @param rx     Out: the response APDU, status word included.
 * @param rx_cap How many bytes @p rx holds.
 * @param rx_len Out: how many arrived. May be NULL.
 * @return KODE_OK, KODE_DENIED for a bad argument, an absent chip or a tag
 *         that is not ISO-DEP, KODE_TIMEOUT when it did not answer,
 *         KODE_ERROR otherwise.
 */
kode_status_t kdrv_st25r3916_apdu(const uint8_t *tx, uint16_t tx_len,
                                  uint8_t *rx, uint16_t rx_cap,
                                  uint16_t *rx_len);

/** @brief Is the activated tag an ISO-DEP one - a Type-4, in NFC Forum terms? */
bool kdrv_st25r3916_is_iso_dep(void);

/**
 * @brief One raw NFC-A exchange, bit-counted, with parity left alone.
 *
 * The escape hatch a Mifare Classic needs and nothing else does. After that
 * card authenticates, its parity bits are encrypted with the keystream and
 * its frames carry no CRC the reader may add, so every layer that would
 * normally help has to be told not to: parity is taken from the buffer as
 * given and kept as received, and the CRC is the caller's business.
 *
 * Lengths are in BITS, in both directions, and that is load-bearing. A
 * Mifare authentication sends four bytes plus four parity bits - thirty-six
 * bits, which is not a whole number of bytes. Rounding it up to forty sends
 * four trailing bits the card rejects.
 *
 * It runs the exchange to completion rather than returning to be pumped,
 * bounded by @p fwt_ms. A Mifare frame is under a millisecond in practice.
 *
 * @param tx      The bits to send, packed least significant bit first within
 *                each byte, parity bits included where the caller put them.
 * @param tx_bits How many bits of @p tx to send.
 * @param rx      Out: what came back, packed the same way.
 * @param rx_cap  How many bytes @p rx holds.
 * @param rx_bits Out: how many bits arrived. May be NULL.
 * @param fwt_ms  How long to wait for the answer.
 * @return KODE_OK; KODE_DENIED for a bad argument, an absent chip or no
 *         activated tag; KODE_TIMEOUT when nothing answered - which is also
 *         how a Mifare card refuses an authentication; KODE_ERROR otherwise.
 */
kode_status_t kdrv_st25r3916_transceive_bits(const uint8_t *tx,
                                             uint16_t tx_bits, uint8_t *rx,
                                             uint16_t rx_cap,
                                             uint16_t *rx_bits,
                                             uint16_t fwt_ms);

/**
 * @brief Drop the activated tag but keep the field and the discovery up.
 *
 * A failed Mifare authentication leaves the card silent and the reader
 * believing it still has one. Selecting it again is what recovers, and that
 * needs the tag released without taking the field down and starting over.
 *
 * @return KODE_OK when a tag is activated again, KODE_TIMEOUT when none
 *         answered, KODE_DENIED with no chip.
 */
kode_status_t kdrv_st25r3916_reselect(void);

/**
 * @brief Every field the console can print.
 *
 * The interrupt status registers are deliberately absent. Reading one clears
 * it, so printing them would consume an interrupt RFAL is waiting for and
 * turn `nfc` from a question into a change. What is here is safe to read at
 * any time and in any state.
 */
typedef enum {
    KDRV_ST25_IDENTITY = 0,   /**< 0x3F: the whole register.                */
    KDRV_ST25_IC_TYPE,        /**< 0x3F[7:3]: 6 is an ST25R3916B.           */
    KDRV_ST25_IC_REV,         /**< 0x3F[2:0]: silicon revision.             */
    KDRV_ST25_OP_CONTROL,     /**< 0x02: the whole register.                */
    KDRV_ST25_EN,             /**< 0x02[7]: the oscillator and regulator.   */
    KDRV_ST25_RX_EN,          /**< 0x02[6]: the receiver.                   */
    KDRV_ST25_RX_CHN,         /**< 0x02[5]: AM channel rather than PM.      */
    KDRV_ST25_RX_MAN,         /**< 0x02[4]: the channel is chosen by hand.  */
    KDRV_ST25_TX_EN,          /**< 0x02[3]: the field.                      */
    KDRV_ST25_WU,             /**< 0x02[2]: wake-up mode.                   */
    KDRV_ST25_MODE,           /**< 0x03: the whole register.                */
    KDRV_ST25_OM,             /**< 0x03[6:3]: the operation mode.           */
    KDRV_ST25_TR_AM,          /**< 0x03[7]: AM modulation rather than OOK.  */
    KDRV_ST25_BIT_RATE,       /**< 0x04: the whole register.                */
    KDRV_ST25_TXRATE,         /**< 0x04[7:4]: transmit rate, 0 is 106 kb/s. */
    KDRV_ST25_RXRATE,         /**< 0x04[3:0]: receive rate.                 */
    KDRV_ST25_REGULATOR,      /**< 0x2B: the regulator display, in mV.      */
    KDRV_ST25_FIELD,          /**< The driver's own: is the field up.       */
    KDRV_ST25_TAGS,           /**< The driver's own: tags activated.        */
    KDRV_ST25_ERRORS,         /**< The driver's own: exchanges that failed. */
    KDRV_ST25_FIELD_MAX
} kdrv_st25r3916_field_t;

/** @brief What a field is, for whoever prints or validates one. */
typedef struct {
    const char *name;     /**< The field's name, lower case.               */
    const char *unit;     /**< "bits", "bool", "code", "mV", "count".      */
    int32_t     min;      /**< Smallest value the field can hold.          */
    int32_t     max;      /**< Largest.                                    */
    uint16_t    step;     /**< Resolution; always 1 here.                  */
    bool        writable; /**< false for every field: `nfc` reports.        */
} kdrv_st25r3916_field_info_t;

/**
 * @brief Read one field.
 *
 * @param f     Which.
 * @param value Out: the value. Written only on KODE_OK.
 * @return KODE_OK, KODE_DENIED for a bad field, a NULL @p value or an absent
 *         chip, KODE_ERROR when the register could not be read.
 */
kode_status_t kdrv_st25r3916_get(kdrv_st25r3916_field_t f, int32_t *value);

/** @brief What @p f is, or NULL if there is no such field. */
const kdrv_st25r3916_field_info_t *kdrv_st25r3916_field_info(
        kdrv_st25r3916_field_t f);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_ST25R3916_H_ */
