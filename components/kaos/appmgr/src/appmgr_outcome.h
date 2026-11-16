/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file appmgr_outcome.h
 * @brief The record of AO_AppMgr's last app install and last kodeOS update.
 *
 * One record per kind, kept in NVS, numbered by AO_AppMgr when it takes the
 * install. The record goes to the port of the command that asked, as a
 * `>|install` line: when the install is taken, before the verdict, and again
 * when it ends. A kodeOS update ends `installed` before the switch, like an
 * app; its verdict follows on the next boot, once AO_Boot says the kodeOS
 * running has stayed up a minute. A port that is down then (the Kode channel, the BLE console) gets
 * the record when it comes up. Every call here runs in AO_AppMgr's thread.
 */
#ifndef APPMGR_OUTCOME_H
#define APPMGR_OUTCOME_H

#include <stdbool.h>
#include <stdint.h>

#include "kqp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APPMGR_INST_LAYOUT    1U          /**< The record's layout; any other is ignored. */
#define APPMGR_INST_KEY_APP   "inst_app"  /**< NVS key of the last app install.           */
#define APPMGR_INST_KEY_OS    "inst_os"   /**< NVS key of the last kodeOS update.         */
#define APPMGR_INST_NAME_MAX  32U         /**< A project name, terminator included.       */
#define APPMGR_INST_VER_MAX   32U         /**< A version, terminator included.            */
#define APPMGR_INST_LEN       80U         /**< Bytes of appmgr_inst_rec_t, as stored.     */
#define APPMGR_INST_PCT_DONE  100U        /**< The percent of a written image.            */

/* The two kinds of install a record is kept for, an index into the records. */
#define APPMGR_INST_APP       0U          /**< An app, into the apps region.              */
#define APPMGR_INST_OS        1U          /**< kodeOS, into the inactive OTA slot.        */
#define APPMGR_INST_KINDS     2U          /**< How many; also "none" where one is kept.   */

/** @brief Where an install stands (`state=`). */
typedef enum {
    APPMGR_INST_NONE = 0,     /**< No install of this kind was ever taken. */
    APPMGR_INST_RUNNING,      /**< Taken, being written.                   */
    APPMGR_INST_INSTALLED,    /**< Written and verified: an app in the
                               *   directory, or kodeOS in its slot with the
                               *   switch asked.                            */
    APPMGR_INST_FAILED,       /**< Ended without installing; `why` says.   */
    APPMGR_INST_CONFIRMED,    /**< The new kodeOS stayed up a minute.      */
    APPMGR_INST_ROLLED_BACK,  /**< The board came back to the old kodeOS.  */
    APPMGR_INST_STATES        /**< How many.                               */
} appmgr_inst_state_t;

/** @brief Why an install did not end well (`why=`). */
typedef enum {
    APPMGR_WHY_NONE = 0,     /**< Nothing went wrong.                          */
    APPMGR_WHY_MISSING,      /**< No such file, or the server said no.         */
    APPMGR_WHY_CUT,          /**< The source stopped before the image ended.   */
    APPMGR_WHY_CORRUPT,      /**< What arrived did not verify.                 */
    APPMGR_WHY_WRITE,        /**< The image could not be written.               */
    APPMGR_WHY_NO_ROOM,      /**< It does not fit, even with every app gone.   */
    APPMGR_WHY_WRONG_BOARD,  /**< It was built for another chip or target.     */
    APPMGR_WHY_BUSY,         /**< The network's one download was someone
                              *   else's: an app's file or GET.              */
    APPMGR_WHY_KEEP,         /**< An app it would remove could not be kept.    */
    APPMGR_WHY_CANCELLED,    /**< The person went back.                        */
    APPMGR_WHY_STALLED,      /**< The terminal stopped sending.                */
    APPMGR_WHY_RESET,        /**< A reset cut the install.                     */
    APPMGR_WHY_SWITCH,       /**< The board never ran the new slot: otadata could
                              *   not be pointed at it, or the switch never took. */
    APPMGR_WHY_UNCONFIRMED,  /**< The new kodeOS never confirmed itself.       */
    APPMGR_WHY_CRASHED,      /**< The new kodeOS crashed until it gave up.     */
    APPMGR_WHY_OTHER,        /**< Anything else; the log says.                 */
    APPMGR_WHY_COUNT         /**< How many.                                    */
} appmgr_inst_why_t;

/** @brief One install's record, as NVS keeps it. */
typedef struct {
    uint32_t id;                              /**< AO_AppMgr's number; 0 for none.     */
    uint32_t slot;                            /**< kodeOS: the written slot's address. */
    uint8_t  layout;                          /**< APPMGR_INST_LAYOUT.                 */
    uint8_t  state;                           /**< appmgr_inst_state_t.                */
    uint8_t  why;                             /**< appmgr_inst_why_t.                  */
    uint8_t  src;                             /**< KODE_CONSOLE_SRC_* of the asker.    */
    uint8_t  told;                            /**< 1 once this state reached its port. */
    uint8_t  pct;                             /**< The percent written at this state.  */
    uint8_t  pad[2];                          /**< Zero.                               */
    char     name[APPMGR_INST_NAME_MAX];      /**< The image's project, or "".         */
    char     version[APPMGR_INST_VER_MAX];    /**< Its version, or "".                 */
} appmgr_inst_rec_t;

_Static_assert(sizeof(appmgr_inst_rec_t) == APPMGR_INST_LEN,
               "the install record's stored layout moved");

/**
 * @brief Take the block, read both records back, subscribe.
 *
 * A record a reset left running becomes a failure for `reset`, to be told
 * when its port comes up. Called again, as the host suite does, it starts over.
 *
 * @param ao AO_AppMgr.
 */
void appmgr_inst_init(QActive *ao);

/**
 * @brief A fresh install request arrived: note its kind and its asker.
 *
 * Nothing is numbered or stored until appmgr_inst_taken(). An image from the
 * mailbox, or for anything but the apps region or the kodeOS slot, keeps no
 * record.
 *
 * @param src_kind kfw_src_t of the request.
 * @param dst_kind kfw_target_t of the request.
 * @param asker    KODE_CONSOLE_SRC_* of the command, or KODE_LOG_SRC_NONE.
 */
void appmgr_inst_prepare(uint8_t src_kind, uint8_t dst_kind, uint8_t asker);

/** @brief The request noted is taken: number it, store it running, tell its port. */
void appmgr_inst_taken(void);

/**
 * @brief The install in hand's image named itself: keep the name in its record.
 * @param name Its project name; NULL or "" changes nothing.
 */
void appmgr_inst_named(const char *name);

/**
 * @brief Note why the install in hand is failing; the first cause noted wins.
 * @param why appmgr_inst_why_t.
 */
void appmgr_inst_why(appmgr_inst_why_t why);

/**
 * @brief The install in hand failed: store it and tell its port.
 *
 * A request refused before it was taken keeps no record.
 *
 * @param pct The percent written when it failed.
 */
void appmgr_inst_failed(uint32_t pct);

/**
 * @brief The app install in hand is in the directory: store it and tell its port.
 *
 * With @p jumps the board resets into the app at once: the line is handed to
 * the port, and on the Kode channel or BLE it stays owed, to be told again
 * once the board is back in kodeOS and the port is up.
 *
 * @param name    Its project name.
 * @param version Its version.
 * @param jumps   The app runs now (`run=on`).
 */
void appmgr_inst_installed(const char *name, const char *version, bool jumps);

/**
 * @brief The kodeOS update in hand is written and verified, and the switch is
 *        about to be asked: store it installed and hand its line to its port.
 *
 * The switch follows at once, as an app's jump does: on the Kode channel or
 * BLE the line stays owed and is told again once the port is up after the
 * reset. The verdict (`confirmed`, `rolled_back`, `failed why=switch`) follows
 * at AO_Boot's minute.
 */
void appmgr_inst_os_installed(void);

/**
 * @brief The events the records wait on: AO_Boot's minute and its failed
 *        switch, the Kode channel and the BLE console coming up.
 * @param e The event.
 * @return true when @p e was one of them.
 */
bool appmgr_inst_on(QEvt const *e);

/**
 * @brief `install_info`: write both records to @p src.
 * @param src      KODE_CONSOLE_SRC_* of the command.
 * @param live_pct The percent of the install in hand, for a running record.
 */
void appmgr_inst_info(uint8_t src, uint32_t live_pct);

#ifdef __cplusplus
}
#endif

#endif /* APPMGR_OUTCOME_H */
