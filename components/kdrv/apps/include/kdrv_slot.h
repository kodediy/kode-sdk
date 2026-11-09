/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_slot.h
 * @brief The inactive kodeOS OTA slot and otadata, as two devices you can write to.
 */
#ifndef KDRV_SLOT_H_
#define KDRV_SLOT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"
#include "kdrv_kfw.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief What an image says about itself.
 *
 * Our own struct rather than esp_app_desc_t: it is the manifest kodeOS shows
 * for `app info`, and letting an ESP-IDF type reach the console would drag
 * esp_app_desc.h up through every layer between.
 */
#define KDRV_SLOT_PROJECT_MAX 32U /**< Bytes of a project name, with its NUL, as
                                    *   esp_app_desc_t holds it.             */

typedef struct {
    char project[KDRV_SLOT_PROJECT_MAX];
    char version[32];
    char date[16];
    char time[16];
    char idf_ver[32];
} kdrv_slot_desc_t;

/* ---- The slot ------------------------------------------------------------ */

/** @brief Is the inactive kodeOS OTA slot (ota_0 or ota_1, whichever kodeOS
 *         is NOT currently running) present at all? */
bool kdrv_slot_present(void);

/** @brief The slot's size in bytes, 0 when it is absent. */
uint32_t kdrv_slot_size(void);

/** @brief The slot's offset in flash - for `boot info`. */
uint32_t kdrv_slot_address(void);

/** @brief Read the manifest of whatever is in the slot. */
kode_status_t kdrv_slot_desc(kdrv_slot_desc_t *out);

/** @brief Whether the slot holds an image that verifies. */
bool kdrv_slot_has_image(void);

/**
 * @brief Open the slot for writing.
 *
 * Asks for sequential writes so the erase is spread across them instead of
 * blocking for seconds up front - erasing 7.5 MB in one go is exactly the
 * kind of thing that makes the prompt freeze.
 */
kode_status_t kdrv_slot_begin(void);

/** @brief Append to the open session. */
kode_status_t kdrv_slot_write(const void *data, size_t len);

/** @brief Close the session and verify what was written. */
kode_status_t kdrv_slot_end(void);

/** @brief Abandon the session, leaving the slot invalid. */
void kdrv_slot_abort(void);

/**
 * @brief The slot as a kfw_sink_t, for the one pipeline that writes images.
 *
 * Its begin() additionally refuses an image larger than the slot, which the
 * bare kdrv_slot_begin() has no length to check.
 *
 * @return A singleton; never NULL. The context argument is unused.
 */
const kfw_sink_t *kdrv_slot_sink(void);

/** @brief Invalidate the slot by erasing enough of it to kill the header. */
kode_status_t kdrv_slot_erase(void);

/**
 * @brief If the running kodeOS image is still PENDING_VERIFY, confirm it.
 *
 * AO_Boot calls it once the board has shown it works, and before every way out
 * of kodeOS it takes on purpose: at the next reset the bootloader turns a
 * PENDING_VERIFY slot into ABORTED and boots the one before it, whatever the
 * reset was. Call it before kdrv_otadata_select_slot(), never after: the mark
 * goes on whichever slot otadata names. kdrv_slot_begin() calls it too, because
 * esp_ota_begin() refuses a session from an unconfirmed slot.
 *
 * @return Whether this call was the one that confirmed it.
 */
bool kdrv_slot_confirm_self(void);

/**
 * @brief Give up on the running kodeOS image: mark it invalid in otadata.
 *
 * The next boot is the slot before it or, with none good, factory. The caller
 * restarts. Refused on a board that has nowhere to go, factory included.
 *
 * @return KODE_OK when the next boot leaves this image, else KODE_ERROR and
 *         otadata is as it was.
 */
kode_status_t kdrv_slot_reject_self(void);

/**
 * @brief The crashes in a row kept for the running slot, across every reset
 *        but a power-on.
 *
 * @param count Receives them when the answer is true.
 * @return false when what is kept is no slot's, or another slot's.
 */
bool kdrv_slot_crashes(uint32_t *count);

/**
 * @brief Keep @p count for the running slot.
 *
 * @param count The crashes in a row; at most 255.
 */
void kdrv_slot_crashes_set(uint32_t count);

/* ---- otadata: who boots next --------------------------------------------- */

/** @brief Is otadata all 0xFF, i.e. "the bootloader picks factory"? */
bool kdrv_otadata_blank(void);

/** @brief Erase otadata, so the next boot is the factory image. */
kode_status_t kdrv_otadata_clear(void);

/** @brief Point otadata at the slot. Verifies the image before writing. */
kode_status_t kdrv_otadata_select_slot(void);

/** @brief What the bootloader recorded about the slot's last run. */
typedef enum {
    KDRV_SLOT_STATE_UNDEFINED = 0, /**< Never launched, or freshly written. */
    KDRV_SLOT_STATE_ABORTED,       /**< Launched and never confirmed: it came home. */
    KDRV_SLOT_STATE_OTHER          /**< Anything else - worth saying out loud. */
} kdrv_slot_state_t;

/** @brief Read the slot's recorded state. Call before clearing otadata. */
kdrv_slot_state_t kdrv_slot_state(void);

/* ---- Every app partition, by id ------------------------------------------ */

/** @brief The three partitions kodeOS can run from, in table order. */
typedef enum {
    KDRV_SLOT_ID_FACTORY = 0,  /**< The rescue image, reachable by cable only. */
    KDRV_SLOT_ID_OTA_0,        /**< One half of the update ping-pong.          */
    KDRV_SLOT_ID_OTA_1,        /**< The other half.                            */
    KDRV_SLOT_ID_COUNT
} kdrv_slot_id_t;

/**
 * @brief One app partition as `info` describes it: where it is, whether the
 *        board is running from it, and what image it holds.
 */
typedef struct {
    const char       *label;     /**< The partition table's name.            */
    uint32_t          address;   /**< Flash offset.                          */
    uint32_t          size;      /**< In bytes.                              */
    bool              running;   /**< This boot came from it.                */
    bool              has_image; /**< A descriptor could be read from it.    */
    kdrv_slot_desc_t  desc;      /**< That descriptor; all "" when there is
                                  *   none.                                  */
} kdrv_slot_info_t;

/**
 * @brief Describe one of the three app partitions, running or not.
 *
 * The same descriptor read kdrv_slot_desc() makes for the inactive slot,
 * asked by name: `info` prints all three so an operator can see what every
 * slot holds without working out which one is "the slot" today. An empty
 * partition is an answer, not a failure - `has_image` says so and the
 * descriptor's strings are left empty.
 *
 * @param id  Which partition.
 * @param out Filled on KODE_OK.
 * @return KODE_DENIED for an id off the end, a NULL @p out, or a partition
 *         the table does not carry; KODE_OK otherwise.
 */
kode_status_t kdrv_slot_info(kdrv_slot_id_t id, kdrv_slot_info_t *out);

/* ---- The crash the board kept --------------------------------------------
 * Not one of the three app partitions above - its own region entirely - but
 * asked about the same way `info` asks about them: present or not, and whose.
 */

/**
 * @brief What `info` needs to know about a coredump waiting in flash.
 */
typedef struct {
    bool     present; /**< A crash left a dump behind.                      */
    bool     mine;    /**< Its ELF SHA-256 matches the image running now -
                        *   false means the dump is not the running image's:
                        *   a sketch, another kodeOS build, or a dump this
                        *   build cannot decode (a sketch's dump is CRC32-
                        *   checksummed, from the Arduino core's IDF v5.5.5
                        *   libraries; this build verifies SHA-256 only, so
                        *   it cannot even open such a dump to read who or
                        *   where - task and pc are then left empty).       */
    char     task[16];/**< The task that crashed, as the dump recorded it;
                        *   empty when the dump could not be read.          */
    uint32_t pc;       /**< Its program counter at the crash; 0 when the
                        *   dump could not be read.                        */
} kdrv_coredump_t;

/**
 * @brief Whether a core dump is waiting in the coredump partition, and whose.
 *
 * Reads what ESP-IDF already wrote there: esp_core_dump_image_check() proves
 * the checksum first. ESP_ERR_NOT_FOUND means the partition truly holds no
 * dump, so it answers "none"; any other error (ESP_ERR_INVALID_CRC,
 * ESP_ERR_INVALID_SIZE, ...) means a dump is present but this build cannot
 * decode it - most often a sketch's CRC32-checksummed dump, since this
 * build's espcoredump verifies SHA-256 only - and it answers "present" with
 * @p out->mine false and @p out->task / @p out->pc left at their zeroed
 * defaults, never attempting esp_core_dump_get_summary() on bytes
 * image_check() has already refused. When the checksum does verify but
 * esp_core_dump_get_summary() still fails (no crashed TCB found), the same
 * "present, not mine, nothing else known" answer is given. Only when both
 * calls succeed are @p out->task, @p out->pc and @p out->mine filled from
 * the summary - @p out->mine by comparing its ELF SHA-256 against this
 * image's own.
 *
 * @param out Filled on KODE_OK; @p out->present is false when flash holds
 *            no dump.
 * @return KODE_DENIED when @p out is NULL or this firmware was not built
 *         with CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH - every app built from the
 *         SDK, since whether a firmware keeps its crashes is not the app's
 *         call to make; KODE_OK otherwise.
 */
kode_status_t kdrv_slot_coredump(kdrv_coredump_t *out);

/**
 * @brief The dump's panic reason, when the dump is this image's own.
 *
 * Asks kdrv_slot_coredump() whether a dump is waiting and whose it is, then
 * reads the reason the panic recorded (a khal_panic() string such as
 * "AO stuck: Net, 30 s in one event"). A dump that is another image's says
 * nothing about this boot, so it is not read.
 *
 * @param buf Receives the reason, always NUL-terminated on KODE_OK.
 * @param len Room in @p buf.
 * @return KODE_OK only when a dump is present, is this image's and carries a
 *         non-empty reason; KODE_DENIED otherwise (no dump, not this image's,
 *         no reason, a NULL or empty @p buf, or a build with no core dump).
 */
kode_status_t kdrv_slot_coredump_reason(char *buf, size_t len);

/* ---- Where we are running ------------------------------------------------ */

/** @brief The running partition's label, address and size - for `boot info`. */
void kdrv_running_info(const char **label, uint32_t *address, uint32_t *size);

/** @brief What the image currently running (this boot) says about itself -
 *         same manifest kdrv_slot_desc() reads for the inactive slot, here
 *         for "myself" instead. Used for the boot banner and the GUI's
 *         version label. */
kode_status_t kdrv_running_desc(kdrv_slot_desc_t *out);

/** @brief Whether kodeOS is running from the factory partition, as designed. */
bool kdrv_running_is_factory(void);

/**
 * @brief The reset reasons kdrv_reset_reason() can answer, numbered as
 *        ESP-IDF's esp_reset_reason_t numbers them (kdrv_slot.c asserts it).
 */
enum {
    KDRV_RESET_UNKNOWN = 0, /**< Could not be determined.                   */
    KDRV_RESET_POWERON,     /**< Power applied, or BOT+LEFT (CHIP_PU).      */
    KDRV_RESET_EXTERNAL,    /**< Reset by an external pin.                  */
    KDRV_RESET_SOFTWARE,    /**< esp_restart().                             */
    KDRV_RESET_PANIC,       /**< An exception or a panic.                   */
    KDRV_RESET_INT_WDT,     /**< The interrupt watchdog.                    */
    KDRV_RESET_TASK_WDT,    /**< The task watchdog.                         */
    KDRV_RESET_WDT,         /**< Any other watchdog.                        */
    KDRV_RESET_DEEPSLEEP,   /**< Wake from deep sleep.                      */
    KDRV_RESET_BROWNOUT,    /**< The supply dipped.                         */
    KDRV_RESET_SDIO,        /**< Over SDIO.                                 */
    KDRV_RESET_USB,         /**< The USB peripheral: the port opening.      */
    KDRV_RESET_JTAG,        /**< The debugger.                              */
    KDRV_RESET_EFUSE,       /**< An eFuse error.                            */
    KDRV_RESET_PWR_GLITCH,  /**< A power glitch.                            */
    KDRV_RESET_CPU_LOCKUP   /**< A double exception.                        */
};

/** @brief The chip's reset reason, as ESP-IDF numbers it (one of the KDRV_RESET_ values). */
int kdrv_reset_reason(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_SLOT_H_ */
