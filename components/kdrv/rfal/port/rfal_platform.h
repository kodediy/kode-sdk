/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file rfal_platform.h
 * @brief What ST's RFAL calls its platform, pointed at kodeOS.
 *
 * RFAL and the ST25R3916 chip driver are written against a set of
 * `platformXxx()` macros and nothing else, which is what makes them portable
 * and what makes this file the whole of the port. Every macro here lands on
 * rfal_port.h, implemented by kdrv_st25r3916.c.
 *
 * The order below matters:
 *   1. Our own platform macros.
 *   2. The RFAL_FEATURE_* choices - BEFORE rfal_defConfig.h, whose #ifndefs
 *      then leave them alone and fill in the rest.
 *   3. rfal_defConfig.h.
 *   4. timer.h, which declares timerCalculateTimer() and friends and needs
 *      platformGetSysTick() to be defined already.
 */
#ifndef RFAL_PLATFORM_H
#define RFAL_PLATFORM_H

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rfal_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The chip driver keeps this visible and expects somebody to own it. The
 * real mutual exclusion is the recursive lock behind
 * platformProtectST25RComm(); this counter only has to exist. Defined in
 * kdrv_st25r3916.c. */
extern uint8_t globalCommProtectCnt;

/* ---- SPI: CS by hand, because RFAL chains transfers under one --------- */
#define platformSpiSelect()                  rfal_port_spi_select()
#define platformSpiDeselect()                rfal_port_spi_deselect()
#define platformSpiTxRx(tx, rx, len)         rfal_port_spi_txrx((tx), (rx), (len))

/* ---- GPIO: there are no STM32 ports here ------------------------------
 * port and pin are tokens this target ignores; the only line the chip
 * driver ever reads is its own interrupt. */
#define platformGpioSet(port, pin)           ((void)(port), (void)(pin))
#define platformGpioClear(port, pin)         ((void)(port), (void)(pin))
#define platformGpioToogle(port, pin)        ((void)(port), (void)(pin))
#define platformGpioIsHigh(port, pin)        rfal_port_irq_pin_high()
#define platformGpioIsLow(port, pin)         (!platformGpioIsHigh(port, pin))

/* ---- The interrupt ----------------------------------------------------
 * The line is configured by kdrv_st25r3916_init() through khal_gpio, so
 * there is nothing for the chip driver to initialise. */
#define platformIrqST25RPinInitialize()
#define platformIrqST25RSetCallback(cb)      rfal_port_irq_set_callback(cb)

/* ---- Mutual exclusion: a recursive lock, not NVIC_DisableIRQ ---------- */
#define platformProtectST25RComm()           rfal_port_protect_comm()
#define platformUnprotectST25RComm()         rfal_port_unprotect_comm()
#define platformProtectST25RIrqStatus()      platformProtectST25RComm()
#define platformUnprotectST25RIrqStatus()    platformUnprotectST25RComm()
#define platformProtectWorker()              rfal_port_protect_worker()
#define platformUnprotectWorker()            rfal_port_unprotect_worker()

/* ---- Time: ST's own timer.c, over our millisecond tick ---------------- */
#define platformTimerCreate(t)               timerCalculateTimer(t)
#define platformTimerIsExpired(timer)        timerIsExpired(timer)
#define platformTimerDestroy(timer)
#define platformTimerGetRemaining(t)         timerGetRemaining(t)
#define platformDelay(t)                     rfal_port_delay_ms(t)
#define platformGetSysTick()                 rfal_port_tick_ms()

#define platformErrorHandle()                rfal_port_error(__FILE__, __LINE__)

/* ---- LEDs: none are wired to this chip -------------------------------- */
#define platformLedsInitialize()
#define platformLedOff(port, pin)
#define platformLedOn(port, pin)
#define platformLedToogle(port, pin)

#define platformLog(...)                     rfal_port_log(__VA_ARGS__)

/* ---- What of RFAL is compiled ----------------------------------------
 * Chosen here rather than defaulted. rfal_defConfig.h turns on nearly
 * everything when it is left to itself, and most of that is code for
 * technologies this board has no caller for - NFC-DEP alone is the largest
 * translation unit in the middleware.
 *
 * On:  NFC-A, because it is what every tag in this slice speaks; T2T, the
 *      common blank tag; the ISO-DEP poller, which is what carries a Type-4
 *      tag and what a phone presents; and the low-power mode, which powers the
 *      chip's oscillator down between uses (kdrv_st25r3916 rests the reader at
 *      boot and after every request, waking it only when the field is asked
 *      for) - the wake-up mode's opposite, a rest the host ends, not a listen
 *      the chip ends; and the listen half, because kodeOS now also emulates an
 *      NFC-A card, the reader's mirror - the chip a passive target and a
 *      worker answering the Type-2 READs (kdrv_st25r3916's emulation section).
 * Off: NFC-B, NFC-F, NFC-V, T1T and ST25TB, technologies nothing here reads;
 *      NFC-DEP, peer-to-peer, abandoned by the phones that had it; and the
 *      wake-up mode, the chip waking itself on a field it senses, because the
 *      field only lives while somebody is asking.
 */
#define RFAL_FEATURE_LISTEN_MODE            true
#define RFAL_FEATURE_WAKEUP_MODE            false
#define RFAL_FEATURE_LOWPOWER_MODE          true
#define RFAL_FEATURE_NFCA                   true
#define RFAL_FEATURE_NFCB                   false
#define RFAL_FEATURE_NFCF                   false
#define RFAL_FEATURE_NFCV                   false
#define RFAL_FEATURE_T1T                    false
#define RFAL_FEATURE_T2T                    true
/* T4T is OFF although Type-4 tags are read and written here. What that
 * feature compiles is ST's APDU composer, and using it would put
 * rfalIsoDepApduBufFormat into knfc_tag.h - a header the host suite has to
 * compile without a radio anywhere. A SELECT and a READ BINARY are six bytes
 * each; knfc_tag.c builds them itself and the whole layer stays testable.
 * The ISO-DEP poller below is the part that actually matters, and it stays. */
#define RFAL_FEATURE_T4T                    false
#define RFAL_FEATURE_ST25TB                 false
#define RFAL_FEATURE_ST25xV                 false
#define RFAL_FEATURE_DYNAMIC_ANALOG_CONFIG  false
#define RFAL_FEATURE_DPO                    false
#define RFAL_FEATURE_DLMA                   false
#define RFAL_FEATURE_ISO_DEP                true
#define RFAL_FEATURE_ISO_DEP_POLL           true
#define RFAL_FEATURE_ISO_DEP_LISTEN         false
#define RFAL_FEATURE_NFC_DEP                false

/* The buffers those features imply. 256-byte I-blocks are FSD 256, the
 * largest a T4T tag negotiates in practice, and a 512-byte APDU is two of
 * them - enough for the NDEF file reads knfc_tag.c makes without paying for
 * a chained transfer it would never use. */
#define RFAL_FEATURE_ISO_DEP_IBLOCK_MAX_LEN 256U
#define RFAL_FEATURE_ISO_DEP_APDU_MAX_LEN   512U
#define RFAL_FEATURE_NFC_RF_BUF_LEN         258U

#include "rfal_defConfig.h"

/* Declares timerCalculateTimer()/timerIsExpired(); needs the macros above. */
#include "timer.h"

#ifdef __cplusplus
}
#endif

#endif /* RFAL_PLATFORM_H */
