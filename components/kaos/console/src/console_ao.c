/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file console_ao.c
 * @brief AO_Console: the command line, and nothing else.
 */
#include "kaos.h"
#include "kaos_start.h"
#include "kdrv_bq25896.h"
#include "kdrv_cw2217.h"
#include "kdrv_tca6408a.h"
#include "kes_fields.h"
#include "ktone.h"
#include "kdrv_sd.h"

#include <inttypes.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "sdkconfig.h"

#include "kaos_wire.h"      /* the base64 encoder shared with display_ao.c */
#include "console_fs.h"     /* the file verbs' bodies, and the shared cwd */
#include "kregistry.h"      /* the app's variables and functions */
#include "console_grammar.h" /* karg_spec_t, the tokeniser, and quoting */
#include "console_verbs.h"   /* what an owner's verbs ask of this AO */
#include "kverbs.h"         /* the owners' verbs, registered by their starts */
#include "kqp.h"
#include "kqp_worker.h"
#include "kbsp_owners.h"
#include "kbsp_board.h"     /* KBSP_LCD_H_RES / _V_RES bound `touch`/`tap` */
#include "kcli_proto.h"
#include "kapp_desc.h"
#include "kapp_cat.h"
#include "kapp_data.h"      /* the app.json status names `appdata_ls` prints */
#include "kdrv_buttons.h"
#include "kdrv_clocks.h"   /* the `clocks` verb and its test row */
#if CONFIG_KODEOS_NET
#include "kdrv_net.h"      /* time sync=: ask a server now */
#endif
#include "kdrv_display.h"   /* the capture self-test's body lives there */
#if CONFIG_KODEOS_MIRROR
#include "kdrv_jpeg.h"      /* test_jpeg judges the encoder the same way */
#endif
#include "kdrv_apps.h"
#include "kdrv_kfw.h"
#include "kdrv_storage.h"
#include "kode_log.h"
#include "kbsp_identity.h"
#include "kbsp_exp.h"       /* KBSP_EXP_COUNT, for the `ps` buffer */
#include "khal_exp.h"       /* the expansion header's table, for `exp` and `ps` */
#include "khal_i2c.h"       /* khal_i2c_probe(): `i2c_scan`, and nothing else */
#include "khal_identity.h"
#include "khal_nvs.h"
#include "khal_sync.h"      /* the semaphore rfid_read/rfid_write wait on */
#include "khal_system.h"
#include "khal_temp.h"      /* the die temperature, for `temp` and `info` */
#include "khal_time.h"      /* the time an injected touch is stamped with */

Q_DEFINE_THIS_MODULE("console_ao")

/* The optional owners the console's own verbs reach, named weakly (KAOS_WEAK,
 * kaos.h): the console comes with the first owner that has verbs, and naming
 * these strongly would bring the radio, the panel, the media and the apps
 * region into every app with it. Each verb that needs one asks
 * console_owner_absent() first; a door reached only after that is linked. */
KAOS_WEAK void kaos_storage_start(void);
KAOS_WEAK void kaos_appmgr_start(void);
KAOS_WEAK void kaos_radio_start(void);
KAOS_WEAK void kaos_display_start(void);
KAOS_WEAK void kaos_net_start(void);
KAOS_WEAK void appdata_request(const char *project, QActive *who);
KAOS_WEAK kode_status_t appdata_rm_request(const char *project, QActive *who);
KAOS_WEAK kode_status_t appmgr_appdata_get_request(const char *project,
                                                   const char *url, QActive *who);
KAOS_WEAK const uint8_t *storage_buf(uint8_t idx);
KAOS_WEAK void storage_close_request(void);
KAOS_WEAK void storage_desc_request(const char *name, QActive *who);
KAOS_WEAK void storage_list_page(uint16_t skip, QActive *who);
KAOS_WEAK void storage_list_request(QActive *who);
KAOS_WEAK void storage_next_request(uint8_t idx);
KAOS_WEAK void storage_open_request(const char *name, QActive *who);
KAOS_WEAK void storage_path_request(QActive *who, uint8_t op, const char *path);
KAOS_WEAK void storage_read_request(void);
KAOS_WEAK void storage_write_all_request(const char *name, const void *data,
                                         uint32_t len, QActive *who);
KAOS_WEAK bool appmgr_image_ask(uint8_t src_kind, const char *src,
                                uint8_t dst_kind, bool launch, QActive *who);
KAOS_WEAK void appmgr_list_page(uint16_t skip, QActive *who);
KAOS_WEAK void appmgr_list_request(QActive *who);
KAOS_WEAK bool appmgr_recv_request(uint8_t dst, uint32_t size, uint32_t crc32,
                                   bool launch, uint8_t src, const char *name);
KAOS_WEAK void c5_flash_request(uint8_t src_kind, const char *src);
KAOS_WEAK bool c5_recv_request(uint32_t size, uint32_t crc32, uint8_t src);
KAOS_WEAK const uint8_t *display_capture_buf(void);
KAOS_WEAK void display_capture_release(void);
KAOS_WEAK void display_capture_request(QActive *who);
#if CONFIG_KODEOS_SHOT_HTTP
KAOS_WEAK void net_shot_stop_request(QActive *who);
KAOS_WEAK void net_shot_request(QActive *who, const char *token, uint8_t fps,
                                uint16_t secs);
#endif
#if CONFIG_KODEOS_NET
KAOS_WEAK kode_status_t kdrv_net_sntp_sync_now(void);
#endif

#define TAG                "kcli"
#define KCLI_MAX_ARGS      8    /**< Tokens per line, verb included.        */
/* How many records leave in one pass, and the gap between passes.
 *
 * Every reply line is a LogEvt from the large pool and this AO outranks
 * AO_Log: a quarter of the pool per pass leaves the rest to other loggers, and
 * the gap is a timer, not a self-post, so AO_Log gets the CPU. `screenshot`
 * and `file_get` use the same pacer; `log_dropped` in `ps` shows a loss. */
#define KCLI_SLICE          KAOS_WIRE_SLICE
#define KCLI_SLICE_GAP_TICKS KQP_TICKS_PER_MS(10U)

/* How long an injected tap holds. Long enough that LVGL's input reader runs
 * at least once with the finger down - its tick is ~30 ms - and short enough
 * to read as a tap rather than a long press. */
#define KCLI_TAP_HOLD_MS    60U

/* How long a due release of an injected gesture waits to retry when the pool
 * was empty. Not KCLI_TAP_HOLD_MS: changing the hold must not change this. */
#define KCLI_INJ_RETRY_MS   60U

/* The limits of an injected swipe. A swipe is replayed at the touch sensor's
 * own rate, one sample a KODE_TOUCH_POLL_MS, so the shortest one is a single
 * sample; the longest two are far past any gesture a hand makes, and bound
 * how long a mistyped number keeps the finger down. */
#define KCLI_SWIPE_MIN_MS       KODE_TOUCH_POLL_MS /**< Shortest: one poll. */
#define KCLI_SWIPE_MAX_MS       5000U              /**< Longest trip.       */
#define KCLI_SWIPE_HOLD_MAX_MS  5000U              /**< Longest rest.       */

/* The payload record: 216 base64 characters, which is a multiple of 4 and so
 * a whole number of source bytes, and 220 with its prefix - inside the 240 a
 * protocol line is allowed. Defined once, in kaos_wire.h, alongside the
 * encoder that sizes records to it - display_ao.c's mirror frames use the
 * same figures for the same reason. */
#define KCLI_PAY_B64        KAOS_WIRE_PAY_B64
#define KCLI_PAY_BYTES      KAOS_WIRE_PAY_BYTES
/* Keys any one command may declare (six, for `exp`), and the length of
 * console_exec()'s value array, which cmd_validate() indexes by spec slot: a
 * longer spec array would write past the stack frame. Long spec arrays
 * _Static_assert against it, and console_specs_ok() refuses any CMDS[] row
 * over it (Console_ctor() asserts that). */
#define KCLI_MAX_KEYS      KARG_KEYS_MAX

/* One `help` record's `args=` value: every key of a command spelled out with
 * its type, its enum members or source prefixes and its default. `install`'s
 * five keys, its six source prefixes among them, take 135 characters; a
 * longer value is cut at the tail rather than overrunning. */
#define KCLI_ARGS_DESC_MAX    192U

/* The longest field name and field description the chip tables carry, as the
 * size of the buffers they are quoted into. A longer one is cut at the tail
 * rather than overrunning anything; the descriptions are the prose the
 * `*_help` verbs print, the longest of them `ir`'s at 168 characters. */
#define KCLI_FIELD_NAME_MAX   CONSOLE_FIELD_NAME_MAX
#define KCLI_FIELD_DESC_MAX   CONSOLE_FIELD_DESC_MAX

/* ---- In-flight commands --------------------------------------------------
 * Queries close with their data: the terminator follows the answer. Exactly
 * one such command may be in flight; its terminator frees the console.
 * Mutations (mount, load, erase...) answer `>ok` as an accept and are not
 * tracked. A watchdog closes an in-flight command with code 6 when its owner
 * does not answer in time, so the console is never stuck at `busy`.
 */
/* Spelled as a bare number, so the same token can also be an argument spec's
 * printed default - which is a string. Everything that wants it as a value
 * takes KCLI_CMD_TIMEOUT_MS and gets the unsigned type with it. */
#define KCLI_CMD_TIMEOUT_MS_N CONSOLE_CMD_TIMEOUT_MS_N
#define KCLI_CMD_TIMEOUT_MS   ((uint32_t)KCLI_CMD_TIMEOUT_MS_N)
/** @brief Room for the auto-lock word of the `>|gui` record: digits or `never`. */
#define KCLI_LOCK_WORD_MAX    12U

/* `screenshot dst=file` writes 402 KB to a medium that may be slow, and may
 * refuse every write before the next one is tried. See h_screenshot. */
#define KCLI_SHOT_FILE_TIMEOUT_MS 20000U
/* `screenshot dst=http` waits for AO_Net, which asks the display for a JPEG
 * and runs a PUT over TLS: a handshake and a frame on a slow link. A stream is
 * answered at once and never waits this long. */
#define KCLI_SHOT_HTTP_TIMEOUT_MS 20000U
/* `cat`'s budget: AO_Storage paces the line records (cat_step() in
 * storage_ao.c). Worst case: KODE_FS_CAT_MAX_CEIL = 32768 one-byte lines at
 * STORAGE_CAT_LINES_PER_STEP = 24 per 10 ms step is ~13.7 s, so 30 s is a
 * margin of about 2.2x. Recheck it if AO_Storage's pacing changes. */
#define KCLI_CAT_TIMEOUT_MS       30000U
/* `cp`'s budget. AO_Storage answers a copy once, at the END of a transfer it
 * paces 32 KB per 10 ms, so this covers a whole file. The slow card is what
 * binds: at 250 KB/s of writing, 60 s carries 15 MB, about 4x a 4 MB OS image.
 * A larger copy can outrun it: the console stops waiting, AO_Storage goes on,
 * and the late answer is dropped by the in-flight check. */
#define KCLI_CP_TIMEOUT_MS        60000U

/** @brief Which in-flight command is waiting for its closing answer. */
typedef enum {
    KCLI_IDLE = 0,  /**< Nothing in flight; any command is welcome.        */
    KCLI_HELP,      /**< help: emitted in slices, closed by this AO.       */
    KCLI_VARIABLE_LIST, /**< variable_list: the app's variables in slices,
                         *   closed by this AO - KCLI_HELP's shape over the
                         *   registry.                                     */
    KCLI_FUNCTION_LIST, /**< function_list: KCLI_VARIABLE_LIST's shape over
                         *   the functions.                                */
    KCLI_PS,        /**< ps: emitted in slices, closed by this AO. About
                     *   fifty records, more than the large pool holds.   */
    KCLI_I2C_SCAN,  /**< i2c_scan: a few addresses probed per step, closed by
                     *   this AO after the last one.                      */
    KCLI_FS_LIST_STAT, /**< ls, phase one: STAT first, as the LIST
                        *   terminator carries no status. Closed by
                        *   STORAGE_PATH_RSP_SIG, which refuses or promotes
                        *   to KCLI_FS_LIST.                             */
    KCLI_FS_LIST,   /**< ls, phase two: sd_ls's burst plus `type=`.       */
    KCLI_FS_STAT,   /**< stat: closed by STORAGE_PATH_RSP_SIG, one record. */
    KCLI_FS_CD,     /**< cd: closed by the same signal and no record.    */
    KCLI_FS_CAT_STAT, /**< cat, phase one: STAT first; a directory is
                       *   refused. Closed by STORAGE_PATH_RSP_SIG, which
                       *   refuses or promotes to KCLI_FS_CAT.             */
    KCLI_FS_CAT,    /**< cat, phase two: STORAGE_LINE_SIG records, posted
                     *   here and never published, until their terminator. */
    KCLI_FS_MKDIR,  /**< mkdir: one round trip, no STAT phase. Closed by
                     *   STORAGE_PATH_RSP_SIG.                              */
    KCLI_FS_RM_STAT,/**< rm, phase one: STAT first; a directory is refused.
                     *   Closed by STORAGE_PATH_RSP_SIG, which refuses or
                     *   promotes to KCLI_FS_RM.                            */
    KCLI_FS_RM,     /**< rm, phase two: the actual delete. Closed by
                     *   STORAGE_PATH_RSP_SIG; `rc` alone says how it went,
                     *   there is nothing left to check about the path.    */
    KCLI_FS_RMDIR_STAT, /**< rmdir, phase one: STAT first; a file is
                        *   refused. Closed by STORAGE_PATH_RSP_SIG, which
                        *   refuses or promotes to KCLI_FS_RMDIR.          */
    KCLI_FS_RMDIR,  /**< rmdir, phase two: the removal; a non-empty
                     *   directory is refused here. Closed by
                     *   STORAGE_PATH_RSP_SIG.                              */
    KCLI_FS_MV,     /**< mv: one round trip, no STAT; a move across volumes
                     *   comes back KODE_DENIED (code 5). Closed by
                     *   STORAGE_PATH_RSP_SIG.                              */
    KCLI_FS_CP,     /**< cp: one answer, at the END of the paced transfer,
                     *   so its watchdog is KCLI_CP_TIMEOUT_MS. No STAT
                     *   phase. Closed by STORAGE_PATH_RSP_SIG.              */
    KCLI_FS_WRITE,  /**< write: one round trip, no STAT phase. Closed by
                     *   STORAGE_PATH_RSP_SIG.                              */
    KCLI_TEST,      /**< test: the selected rows one at a time, each on
                     *   its own budget, until the last has answered or
                     *   timed out.                                      */
    KCLI_BTN,       /**< btn_read: closed by BTN_STATE_SIG.                */
    KCLI_BOOT,      /**< boot_info: closed by BOOT_STATE_SIG.              */
    KCLI_TEST_REPORT, /**< test_report: one AUTOTEST_REPORT_SIG a row,
                       *   closed by the one that is the end marker.      */
    KCLI_GUI,       /**< gui_info: closed by GUI_STATE_SIG.                */
    KCLI_BAT,       /**< bat_info: closed by the next POWER_EVT_SIG.       */
    KCLI_FIELD,     /**< pmic/gauge/ioexp/tp/sd/c5/audio/haptic: closed by
                     *   the answer of the chip in Console.field_chip,
                     *   through console_field_answer() for an owner's. */
    KCLI_FIELD_HELP,/**< pmic_help and every `<chip>_help`: emitted in
                     *   slices over Console.help_chip, closed by this AO. */
    KCLI_CTRL,      /**< boost/pmic_reset: closed by POWER_CTRL_SIG; the
                     *   charge settings are `config`'s.               */
#if CONFIG_KODEOS_BOOT_ROLE_OS
    KCLI_RESET,     /**< factory_reset: closed by FACTORY_RESET_RSP_SIG.   */
#endif
    KCLI_CONFIG,    /**< config: one CONFIG_ANS_SIG a setting, the whole
                     *   table one row at a time.                          */
    KCLI_FUNCTION,  /**< A function that answers (answer_ms set): its records
                     *   arrive as FUNCTION_ANS_SIG, the last one closes it. */
    KCLI_RECV,      /**< The receive protocol (`install src="terminal"`,
                     *   `file_put`): `>|ready` on RECV_READY, closed by
                     *   RECV_DONE / _FAIL / _STALL. The one kind that
                     *   disarms this AO's watchdog mid-flight: from
                     *   `>|ready` on, the receiver's idle watchdog owns it. */
    KCLI_SHOT,      /**< screenshot dst=console: base64 records out of the
                     *   capture, paced, closed by this AO.                */
    KCLI_SHOT_FILE, /**< screenshot dst=file: closed by
                     *   STORAGE_WRITE_RSP_SIG.                            */
    KCLI_SHOT_HTTP, /**< screenshot dst=http: closed by NET_SHOT_RSP_SIG
                     *   (a capture at the end of its upload, a stream
                     *   when AO_Net accepts it, a stop at once).          */
    KCLI_FILE,      /**< file_get: base64 records out of the borrowed
                     *   storage buffers, closed by this AO at EOF.        */
    KCLI_APPDATA_LS,  /**< appdata_ls: one record per project, closed by
                       *   this AO after the last folder. A multi-step
                       *   gather, tracked by Console.ad_phase.           */
    KCLI_APPDATA_INFO,/**< appdata_info: the same gather cut short at the
                       *   one project asked about, closed by
                       *   APPDATA_END_SIG.                               */
    KCLI_APPDATA_RM,  /**< appdata_rm: closed by APPDATA_RM_RSP_SIG.      */
    KCLI_APPDATA_GET, /**< appdata_get: closed by APPDATA_GET_RSP_SIG.    */
    KCLI_INSTALL,   /**< install off a medium or the mailbox: closed by
                     *   AO_AppMgr's verdict, APP_REQ_RSP_SIG.           */
    KCLI_OWNER,     /**< A registered verb: its owner's answer hook sees
                     *   the events, and console_end() closes it.        */
} kcli_inflight_t;

/* ---- The payload producer ------------------------------------------------
 * One producer, two sources: `screenshot dst=console` a frozen display
 * capture, `file_get` a borrowed AO_Storage buffer, re-pointed at the next
 * one as each runs out. `run` is the current source; `total` is everything
 * emitted since the command began.
 */
typedef struct {
    const uint8_t *run;      /**< The bytes being encoded right now.        */
    uint32_t       run_len;  /**< How many of them.                         */
    uint32_t       pos;      /**< How far into `run`.                       */
    uint32_t       total;    /**< Bytes encoded since the command began.    */
    uint32_t       expect;   /**< What the header record promised.          */
    uint8_t        idx;      /**< file_get: which storage buffer is on loan. */
    bool           borrowed; /**< file_get: whether one is.                 */

    /* Source bytes carried into the next run: a run need not end on a
     * group of three (32768 is not a multiple of 3), and '=' mid-stream would
     * corrupt a host that decodes the records concatenated. Only the last
     * record of the whole payload is padded. */
    uint8_t        carry[2];
    uint8_t        n_carry;
} kcli_payload_t;

/* ---- The app data gather -------------------------------------------------
 * `appdata_ls` lists every project the board knows about: AO_AppMgr's
 * installed list, AO_Storage's .bin files and manifests, and the two
 * `.kode/appsdata` trees. One question is in flight at a time; the phase says
 * which part a listing belongs to. `appdata_info` stops at its one project.
 */

/**
 * @brief The verb's own ceiling: projects it gathers in all, apps and files.
 *
 * Not a sum of the listing ceilings: every slot is a name in internal RAM.
 */
#define KCLI_AD_MAX     48U

/** @brief A row's share of the budget; a long listing buys it one row at a time. */
#define KCLI_AD_ROW_MS  1000U

/** @brief The internal partition's tree, and the card's. */
#define KCLI_AD_DIR_FS  "/storage/.kode/appsdata"
#define KCLI_AD_DIR_SD  "/sdcard/.kode/appsdata"

/** @brief Where the gather has got to (Console.ad_phase). */
typedef enum {
    KCLI_AD_INSTALLED = 0, /**< APP_ENTRY_SIG until APP_LIST_END_SIG.        */
    KCLI_AD_FILES,         /**< STORAGE_ENTRY_SIG until the list end.        */
    KCLI_AD_DESC,          /**< One manifest per .bin found, in turn.        */
    KCLI_AD_FOLDERS_FS,    /**< The internal tree's own listing.             */
    KCLI_AD_FOLDERS_SD,    /**< The card's.                                  */
    KCLI_AD_ASK            /**< One appdata_request() per row, in turn.      */
} kcli_ad_phase_t;

/** @brief What one gathered slot turned out to be (Console.ad_kind). */
typedef enum {
    KCLI_AD_INSTALLED_APP = 0, /**< The apps region carries it.              */
    KCLI_AD_FILE_PENDING,      /**< A .bin whose manifest is still to come.  */
    KCLI_AD_FILE,              /**< A .bin, its project name now known.      */
    KCLI_AD_ORPHAN,            /**< A folder with no binary anywhere.        */
    KCLI_AD_DROPPED            /**< Folded into an earlier slot, or unread.  */
} kcli_ad_kind_t;

/* ---- AO_Console --------------------------------------------------------- */

/** @brief Which injected gesture is in flight, if any (Console::inj). */
typedef enum {
    KCLI_INJ_NONE = 0,  /**< None: `tap`, `touch` and `swipe` are accepted.  */
    KCLI_INJ_TAP,       /**< A `tap`, whose release is owed when the hold
                         *   has run.                                       */
    KCLI_INJ_SWIPE      /**< A `swipe`: its moves and its release follow the
                         *   plan in Console::swipe.                        */
} kcli_inj_t;

/**
 * @brief The plan of the swipe in flight.
 *
 * Times are offsets from @c t0, the command's time. Samples are stamped with
 * their PLANNED time, so the replay speed is exact. The end point is
 * Console::inj_x / Console::inj_y.
 */
typedef struct {
    uint32_t t0;    /**< khal_time_ms() at the command: the press's stamp.  */
    uint32_t step;  /**< Timer steps since the press; one stands for
                     *   KODE_TOUCH_POLL_MS of the plan.                    */
    int16_t  x0;    /**< Where the finger goes down.                        */
    int16_t  y0;    /**< The row it goes down on; x0 is the column.         */
    uint16_t ms;    /**< How long the finger takes to get to the end.       */
    uint16_t hold;  /**< How long it rests there before it lifts.          */
    uint16_t moved; /**< Moves published so far.                            */
} kcli_swipe_t;

/** @brief One followed variable: its timer, its row, and how often. */
typedef struct {
    QTimeEvt               tev;     /**< CONSOLE_WATCH_SIG, periodic.   */
    const kode_variable_t *row;     /**< NULL: the slot is free.        */
    uint16_t               period_ms;
} kcli_watch_t;

/** @brief The console AO: parser state plus the one in-flight command. */
typedef struct {
    QActive  super;
    const console_chip_t *field_chip; /**< KCLI_FIELD: the chip asked. An app
                             *   may ask a chip while a field verb waits,
                             *   and only that chip's answer closes it. */
    const console_chip_t *help_chip;  /**< KCLI_FIELD_HELP: the table. */
    const kverbs_t *owner;  /**< The registered table whose verb is in
                             *   flight, or NULL for the console's own:
                             *   its answer hook sees the events. */
    uint8_t  inflight;      /**< kcli_inflight_t: the command awaiting its
                             *   terminator, or KCLI_IDLE.                  */
    bool     test_wait_begin;   /**< `test`: asked, and AO_Autotest has not
                                 *   yet said whether it took the run.    */
    uint8_t  test_rows;         /**< `test`: how many rows AO_Autotest said
                                 *   the run holds.                       */
    uint8_t  test_id[AUTOTEST_RUN_MAX];    /**< `test`, `test_report`: the
                                 *   rows that ended, AUTOTEST_ID_*, for the
                                 *   verdict block that closes them.     */
    uint8_t  test_state[AUTOTEST_RUN_MAX]; /**< How each of them ended.   */
    uint8_t  test_kept;         /**< Entries used in the two above.      */
    uint8_t  test_seen;         /**< `test`: how many of them have ended
                                 *   with a record so far.                */
    uint8_t  src;           /**< KODE_CONSOLE_SRC_* of the line in flight:
                             *   cable, BLE, Kode channel (NET) or
                             *   AO_Scheduler (SCHED). Verbs that need more
                             *   than a line ask console_src_allows().      */
    uint8_t  cmd_src;       /**< KODE_CONSOLE_SRC_* of the command whose
                             *   replies are being written, or
                             *   KODE_LOG_SRC_NONE. Set only when a line is
                             *   accepted; `src` is the newest line's.      */
    uint8_t  line_seq;      /**< The sequence the newest line from the Kode
                             *   channel (src NET) arrived with.             */
    uint8_t  net_seq;       /**< The same for the command whose replies are
                             *   being written, set when a line is accepted
                             *   like @c cmd_src; net_line_done() returns it. */
    uint16_t help_idx;      /**< Next command `help` has still to print.    */
    uint16_t scan_next;     /**< KCLI_I2C_SCAN: the next address to probe.  */
    uint16_t scan_found;    /**< KCLI_I2C_SCAN: the addresses that answered. */

    uint8_t  ctrl_op;       /**< KCLI_CTRL: the POWER_CTRL_* op in flight.
                             *   Every answer is published, so a verb has to
                             *   recognise its own.                         */
    uint32_t cat_max;       /**< KCLI_FS_CAT: the `max=` asked for, carried
                             *   across the STAT phase to
                             *   console_fs_cat_begin().                    */

    const char *verb;       /**< The in-flight command's verb, for errors.  */
    QTimeEvt    tmo;       /**< Watchdog: no closing answer -> err code 6. */
    QTimeEvt    pacer;      /**< Spaces every kind of slice; see h_help.    */
    kcli_payload_t pay;     /**< The base64 producer's place in its source. */
    char        file_name[KODE_APP_FILE_MAX];  /**< screenshot / file_get.  */

    /* appdata_ls / appdata_info: the gather. A slot holds a .bin's file name
     * until its manifest is read, then the project name, so it is as wide as
     * a file name. */
    uint8_t  ad_phase;      /**< kcli_ad_phase_t: where the gather is.     */
    uint16_t list_next;     /**< The position the next entry of a paged
                              *   listing this AO asked for must carry; it
                              *   is also the skip of the next page.       */
    uint16_t list_from;     /**< The skip the last page was asked with: a
                              *   page that ends with `more` and leaves
                              *   list_next here brought nothing.          */
    uint8_t  ad_n;          /**< Slots in use.                             */
    uint8_t  ad_at;         /**< The slot being asked about right now.     */
    uint8_t  ad_kind[KCLI_AD_MAX];                  /**< kcli_ad_kind_t.   */
    char     ad_name[KCLI_AD_MAX][KODE_APP_FILE_MAX];
    uint8_t  ad_medium;     /**< The last fields event's AppDataMedium.    */
    uint8_t  ad_json;       /**< ...its kapp_json_status_t.                */
    uint8_t  ad_icon;       /**< ...its AppDataIconStatus.                 */
    /* appdata_info only: the one project asked about, and what the binary
     * says about it - the second source of the spec's section C table, and
     * the only one this AO has to go and fetch for itself. */
    char     ad_target[32];
    char     ad_bin_name[KODE_APP_DISPLAY_MAX];
    char     ad_bin_version[16];
    char     ad_bin_category[KODE_APP_CATEGORY_MAX];
    bool     ad_bin;        /**< A binary for `ad_target` was found.       */

    QTimeEvt    inj_tmo;    /**< Drives what an injected gesture still owes:
                             *   a tap's release, a swipe's moves and its
                             *   release.                                  */
    int16_t     inj_x;      /**< Where the gesture in flight will release. */
    int16_t     inj_y;
    kcli_inj_t  inj;        /**< The gesture in flight. One at a time: a
                             *   second would not know whether the release
                             *   owed is its own.                          */
    bool        inj_stalled;/**< A sample has retried at least once; gates
                             *   the KLOG_W to once per stall.             */
    kcli_swipe_t swipe;     /**< KCLI_INJ_SWIPE: the plan.                 */

    /* config: the setting asked about, the request's number, and whether
     * the whole table is being listed. */
    const kode_config_t *cfg_row;
    uint32_t    cfg_seq;
    uint16_t    cfg_idx;    /**< With cfg_all: cfg_row's index.            */
    bool        cfg_all;

    /* The app's followed variables: one timer per slot, all on
     * CONSOLE_WATCH_SIG. A slot is free when its row is NULL. */
    kcli_watch_t watch[KODE_VARIABLE_WATCH_MAX];
#if CONFIG_KODE_LOWPOWER
    uint8_t     lp_who;     /**< The id lowpower_join() gave.               */
#endif
} Console;

static Console     l_console;
QActive * const    AO_Console = &l_console.super;

static QState Console_initial(Console * const me, void const * const par);
static QState Console_active(Console * const me, QEvt const * const e);

/* ---- Responses ----------------------------------------------------------
 * Every line goes out through kode_log_reply(), on the log path, so AO_Log
 * stays the console's only writer. A SCHED line is answered as a NOTICE
 * `sched: <reply>`, a NET line on the Kode channel; its terminator tells the
 * source the next may go (sched_line_done(), net_line_done() with the line's
 * sequence). A line the console is too busy for is handed back
 * (sched_line_busy(), net_line_busy()).
 */

/**
 * @brief Whether a reply is the one that ends its command.
 *
 * @param line The reply, without its newline.
 * @return true for `>ok`, `>ok <fields>` and `>err <fields>`.
 */
static bool reply_ends(const char *line)
{
    bool ends = false;

    if ( strncmp(line, ">ok", sizeof(">ok") - 1U) == 0 )
    {
        ends = ('\0' == line[sizeof(">ok") - 1U]) || (' ' == line[sizeof(">ok") - 1U]);
    }
    else
    {
        ends = (strncmp(line, ">err ", sizeof(">err ") - 1U) == 0);
    }
    return ends;
}

/** @brief Emit one response line (`>ok`, `>err`, or a `>|` record). */
void console_reply(const char *fmt, ...)
{
    char buf[KODE_LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if ( (uint8_t)KODE_CONSOLE_SRC_SCHED == l_console.cmd_src )
    {
        KLOG_N(TAG, "sched: %s", buf);
        if ( reply_ends(buf) )
        {
            sched_line_done();
        }
    }
    else
    {
        /* Tagged with the port of the command being answered: the Kode
         * channel publishes the replies of its own commands and of no
         * other. */
        kode_log_reply_src(buf, l_console.cmd_src);
#if CONFIG_KODEOS_CHANNEL
        /* A line off the Kode channel ends here too, and AO_Net, which holds
         * the lines behind it, gives the console the next. After the reply,
         * so the answers stay in order. */
        if ( ((uint8_t)KODE_CONSOLE_SRC_NET == l_console.cmd_src) && reply_ends(buf) )
        {
            net_line_done(l_console.net_seq);
        }
#endif
    }
}

/** @brief Terminate the command in flight with an error. @p msg is one word. */
void console_reply_err(kcli_err_t code, const char *msg)
{
    console_reply(">err code=%u msg=%s", (unsigned)code, msg);
}

/**
 * @brief A text value for a record: quoted, or a bare `-` when there is none.
 *
 * `-` is the grammar's "no value" and is never quoted: `"-"` would be a value
 * that happens to be one dash, which is a different statement.
 *
 * @param dst Where console_quote() writes; size it with CONSOLE_QUOTED_MAX().
 * @param cap Its size.
 * @param src The text. NULL or empty means there is none.
 * @return @p dst, or the literal `-`, for use inside a console_reply() argument list.
 */
const char *console_kv_text(char *dst, size_t cap, const char *src)
{
    const char *out = "-";
    if ( (NULL != src) && ('\0' != src[0]) )
    {
        out = console_quote(dst, cap, src);
    }
    return out;
}

/**
 * @brief console_quote() for a value that may not fit what is left of its
 *        line, saying so when it does not.
 *
 * A cut value is still well-formed, but a host cannot tell it from a whole
 * one, so the cut earns a `!W` line naming @p what, ahead of the record.
 *
 * @param dst  Where to write: what is left of the line, not more.
 * @param cap  Its size.
 * @param src  The value.
 * @param what The record's key, for the warning.
 * @return @p dst, for use inside a console_reply() argument list.
 */
const char *console_kv_fit(char *dst, size_t cap, const char *src,
                          const char *what)
{
    if ( console_quoted_size(src) > cap )
    {
        KLOG_W(TAG, "%s: cut to fit one line", what);
    }
    return console_quote(dst, cap, src);
}

/**
 * @brief One field-table answer: `>|chip=<chip> field="<name>" value=<v>`.
 *
 * The same record for every chip with a field table, the name quoted the way
 * `field=` takes it, so an answer can be typed straight back as a request.
 */
static void field_record(const char *chip, const char *name, int32_t value)
{
    char qn[CONSOLE_QUOTED_MAX(KCLI_FIELD_NAME_MAX)];
    console_reply(">|chip=%s field=%s value=%" PRId32, chip,
          console_quote(qn, sizeof(qn), name), value);
}

/**
 * @brief One word for a kode_status_t an owning AO answered with, for the
 *        `msg=` of a code 5 `>err` line.
 *
 * @param st The status, as the uint32_t an answer event carries it.
 * @return The word; `error` for a value outside the enum.
 */
const char *console_status_word(uint32_t st)
{
    static const char *const WORDS[] = {
        "ok", "error", "timeout", "denied", "noresources", "iofail",
        "exists", "notdir", "isdir"
    };
    /* The table is tied to KODE_IS_DIR + 1, so a member inserted before
     * KODE_IS_DIR stops the build; one appended after it prints error until
     * it is named here. */
    _Static_assert((sizeof(WORDS) / sizeof(WORDS[0]))
                   == ((size_t)KODE_IS_DIR + 1U),
                   "one status word per kode_status_t");
    const char *w = "error";

    if ( (uint32_t)(sizeof(WORDS) / sizeof(WORDS[0])) > st )
    {
        w = WORDS[st];
    }
    return w;
}

/**
 * @brief Terminate a fire-and-forget command: `>ok` if the event went out.
 *
 * @p delivered false means an event pool was empty, so the request never
 * reached its owner - saying `>ok` would be a lie, so it answers code 5
 * with the verb as the token.
 */
void console_reply_posted(const char *verb, bool delivered)
{
    if ( delivered )
    {
        console_reply(">ok");
    }
    else
    {
        console_reply_err(KCLI_ERR_REJECT, verb);
    }
}

/**
 * @brief Arm an in-flight command: records may now arrive, the console is
 *        busy to new commands, and the watchdog starts counting.
 */
static void cmd_begin(Console *me, kcli_inflight_t what, const char *verb)
{
    me->inflight = (uint8_t)what;
    me->verb     = verb;
    QTimeEvt_armX(&me->tmo, KQP_TICKS_PER_MS(KCLI_CMD_TIMEOUT_MS), 0U);
}

/**
 * @brief Give the command in flight a longer deadline than the default.
 *
 * For a bounded operation longer than the default, whose owner answers once,
 * at the end, so there is nothing to re-arm on.
 */
void console_extend(uint32_t ms)
{
    (void)QTimeEvt_rearm(&l_console.tmo, KQP_TICKS_PER_MS(ms));
}

/** @brief Close the in-flight command: terminator sent, console free again. */
void console_end(void)
{
    Console * const me = &l_console;

    console_unwatched();
    /* Whatever closed the command, no further slice is owed. The pacer's
     * own case checks the in-flight kind as well; this stops a tick that is
     * already on the way from arriving at all. */
    (void)QTimeEvt_disarm(&me->pacer);
    me->inflight = (uint8_t)KCLI_IDLE;
}

/* ---- What an owner's verbs ask of this AO (console_verbs.h) ---------------- */

void console_begin(const char *verb)
{
    cmd_begin(&l_console, KCLI_OWNER, verb);
}

void console_unwatched(void)
{
    (void)QTimeEvt_disarm(&l_console.tmo);
}

/* cppcheck-suppress misra-c2012-8.7 ; called by lowpower_verbs.c, built with CONFIG_KODE_LOWPOWER, which this check does not read */
void console_pace(void)
{
    Console * const me = &l_console;

    /* rearm, not armX: the watchdog cmd_begin() set is still armed, and QP
     * asserts on arming an armed time event. */
    (void)QTimeEvt_rearm(&me->tmo, KQP_TICKS_PER_MS(KCLI_CMD_TIMEOUT_MS));
    QTimeEvt_armX(&me->pacer, KCLI_SLICE_GAP_TICKS, 0U);
}

/* cppcheck-suppress misra-c2012-8.7 ; called by lowpower_verbs.c, built with CONFIG_KODE_LOWPOWER, which this check does not read */
uint8_t console_src(void)
{
    return l_console.cmd_src;
}

const char *console_verb(void)
{
    const char *verb = "?";

    if ( NULL != l_console.verb )
    {
        verb = l_console.verb;
    }
    return verb;
}

/* ---- The argument specs -------------------------------------------------
 * Each command declares its keys; one validator checks any line against the
 * declaration; handlers receive parsed values. `help` prints the same specs.
 * karg_type_t and karg_spec_t themselves live in console_grammar.h, beside
 * the rule that decides which of them is typed in quotes.
 */

struct kcmd;

/** @brief A handler: values are already validated when it runs. */
typedef void (*kcmd_fn)(Console *me, const struct kcmd *cmd,
                        const karg_val_t *v);

/** @brief One console command: the verb, its keys, and the handler. */
typedef struct kcmd {
    const char        *verb;    /**< First token of the line.              */
    const karg_spec_t *args;    /**< Key specs, NULL when none.            */
    uint8_t            n_args;  /**< How many key specs.                   */
    kcmd_fn            fn;      /**< Runs with validated values.           */
} kcmd_t;

static const kcmd_t *g_cmds;   /* set in ctor; forward-referenced by help */
static size_t        g_ncmds;

/* ---- Value parsers ------------------------------------------------------ */

/** @brief Strict unsigned decimal: digits only, bounded by [min, max]. */
static bool parse_u32(const char *s, uint32_t min, uint32_t max, uint32_t *out)
{
    uint64_t val = 0U;
    bool     ok  = ('\0' != s[0]);

    for ( size_t i = 0U; ok && ('\0' != s[i]); ++i )
    {
        if ( ('0' > s[i]) || ('9' < s[i]) )
        {
            ok = false;
        }
        else
        {
            val = (val * 10U) + (uint64_t)(s[i] - '0');
            if ( (uint64_t)max < val )
            {
                ok = false;
            }
        }
    }
    if ( ok && ((uint64_t)min <= val) )
    {
        *out = (uint32_t)val;
    }
    else
    {
        ok = false;
    }
    return ok;
}

/** @brief `on` -> true, `off` -> false, anything else refused. */
static bool parse_bool(const char *s, bool *out)
{
    bool ok = true;
    if ( strcmp(s, "on") == 0 )
    {
        *out = true;
    }
    else if ( strcmp(s, "off") == 0 )
    {
        *out = false;
    }
    else
    {
        ok = false;
    }
    return ok;
}

/** @brief Three comma-separated 0..255, no spaces: `0,0,200`. */
static bool parse_rgb(const char *s, uint8_t out[3])
{
    bool ok = true;
    for ( int i = 0; ok && (3 > i); ++i )
    {
        uint32_t val = 0U;
        bool     any = false;
        while ( ('0' <= *s) && ('9' >= *s) && ok )
        {
            val = (val * 10U) + (uint32_t)(*s - '0');
            any = true;
            ok  = (255U >= val);
            ++s;
        }
        ok = ok && any;
        if ( ok )
        {
            if ( 2 > i )
            {
                ok = (',' == *s);
                ++s;
            }
            else
            {
                ok = ('\0' == *s);
            }
            out[i] = (uint8_t)val;
        }
    }
    return ok;
}

/**
 * @brief Check one line against a command's key specs.
 *
 * On failure it answers the `>err` itself: code 2 for a token without `=` or
 * a missing required key; code 3, naming the key, for an unknown or repeated
 * key or a value that does not parse, including one in the wrong form (a
 * string typed bare, or a number, boolean, enum or colour typed in quotes).
 *
 * @param args   The command's key specs.
 * @param n_args How many.
 * @param argc Token count, verb included.
 * @param argv The tokens; key=value tokens are split and quoted values read
 *             in place.
 * @param vals Out: one slot per spec, .present false where a key was absent.
 * @return true when @p vals is fully valid and the handler may run.
 */
static bool cmd_validate(const karg_spec_t *args, uint8_t n_args, int argc,
                         char **argv, karg_val_t *vals)
{
    (void)memset(vals, 0, sizeof(karg_val_t) * KCLI_MAX_KEYS);

    for ( int i = 1; i < argc; ++i )
    {
        char * const eq = strchr(argv[i], '=');
        if ( NULL == eq )
        {
            console_reply_err(KCLI_ERR_MISSING, argv[i]);
            return false;
        }
        *eq = '\0';
        char const * const key = argv[i];
        char       * const val = eq + 1;

        int slot = -1;
        for ( int a = 0; (int)n_args > a; ++a )
        {
            if ( strcmp(key, args[a].key) == 0 )
            {
                slot = a;
                break;
            }
        }
        if ( (0 > slot) || vals[slot].present )
        {
            /* An unknown key, or one given a second time: which value wins
             * is not for the parser to guess. */
            console_reply_err(KCLI_ERR_VALUE, key);
            return false;
        }

        /* console_token() already refused a quote that does not close, so a
         * value that opens with one always reads back whole. */
        bool const quoted = console_unquote(val);

        karg_spec_t const * const spec = &args[slot];
        karg_val_t        * const out  = &vals[slot];
        bool ok = false;
        switch ( spec->type )
        {
            case KARG_UINT:
                ok = !quoted && parse_u32(val, spec->min, spec->max, &out->u);
                break;
            case KARG_BOOL:
                ok = !quoted && parse_bool(val, &out->b);
                break;
            case KARG_ENUM:
                for ( uint8_t m = 0U; (m < spec->n_enums) && !quoted; ++m )
                {
                    if ( strcmp(val, spec->enums[m]) == 0 )
                    {
                        out->u = (uint32_t)m;
                        ok     = true;
                        break;
                    }
                }
                break;
            case KARG_STRING:
                out->s = val;
                ok     = quoted && ('\0' != val[0]);
                break;
            case KARG_RGB:
                ok = !quoted && parse_rgb(val, out->rgb);
                break;
            case KARG_TEXT:
                /* An empty text is still a value - `text=""` is the operator
                 * asking for an empty file or an empty record on purpose -
                 * so there is no non-empty check the way KARG_STRING has. */
                out->s = val;
                ok     = quoted;
                break;
            case KARG_ROW:
                /* Either form: the row the handler looks up decides which
                 * one it wanted, so all the parser can do is say which came. */
                out->s      = val;
                out->quoted = quoted;
                ok          = ('\0' != val[0]);
                break;
            default:
                break;
        }
        if ( !ok )
        {
            console_reply_err(KCLI_ERR_VALUE, key);
            return false;
        }
        out->present = true;
    }

    for ( int a = 0; (int)n_args > a; ++a )
    {
        if ( args[a].required && !vals[a].present )
        {
            console_reply_err(KCLI_ERR_MISSING, args[a].key);
            return false;
        }
    }
    return true;
}

bool console_is_bin(const char *name)
{
    const char *dot = strrchr(name, '.');
    return (NULL != dot) && (strcasecmp(dot, ".bin") == 0);
}

const char *console_on_off(bool on)
{
    const char *word = "off";

    if ( on )
    {
        word = "on";
    }
    return word;
}

const char *console_word(const char *const *words, size_t n, uint32_t i)
{
    const char *word = "?";

    if ( (size_t)i < n )
    {
        word = words[i];
    }
    return word;
}

/** @brief The value of hex digit @p ch, either case; false for anything else. */
static bool hex_nibble(char ch, uint8_t *out)
{
    static const char LOWER[] = "0123456789abcdef";
    static const char UPPER[] = "0123456789ABCDEF";
    bool found = false;

    for ( uint8_t d = 0U; (CONSOLE_HEX_DIGITS > d) && !found; ++d )
    {
        if ( (ch == LOWER[d]) || (ch == UPPER[d]) )
        {
            *out  = d;
            found = true;
        }
    }
    return found;
}

/**
 * @brief Count the hex digits of @p s, or refuse it.
 * @return true when every character is a digit or `:` and there are at most
 *         @p most digits; @p n holds how many.
 */
static bool hex_digits(const char *s, size_t most, size_t *n)
{
    bool ok = (NULL != s);

    *n = 0U;
    for ( size_t i = 0U; ok && ('\0' != s[i]); ++i )
    {
        uint8_t val = 0U;

        if ( ':' == s[i] )
        {
            /* A separator between byte pairs, or anywhere: ignored. */
        }
        else if ( !hex_nibble(s[i], &val) || (*n >= most) )
        {
            ok = false;
        }
        else
        {
            *n += 1U;
        }
    }
    return ok;
}

uint8_t console_hex_bytes(const char *s, uint8_t *out, uint8_t max)
{
    size_t  n     = 0U;
    uint8_t bytes = 0U;

    /* The whole text is judged before a byte is written: a refused value
     * leaves @p out as it was. */
    if ( hex_digits(s, (size_t)max * CONSOLE_HEX_PER_BYTE, &n)
        && (0U < n) && ((n % CONSOLE_HEX_PER_BYTE) == 0U) )
    {
        size_t at = 0U;

        for ( size_t i = 0U; '\0' != s[i]; ++i )
        {
            uint8_t val = 0U;

            if ( hex_nibble(s[i], &val) )
            {
                size_t const byte = at / CONSOLE_HEX_PER_BYTE;

                if ( (at % CONSOLE_HEX_PER_BYTE) == 0U )
                {
                    out[byte] = (uint8_t)(val << CONSOLE_NIBBLE_BITS);
                }
                else
                {
                    out[byte] = (uint8_t)(out[byte] | val);
                }
                at += 1U;
            }
        }
        bytes = (uint8_t)(n / CONSOLE_HEX_PER_BYTE);
    }
    return bytes;
}

const char *console_hex_text(char *dst, size_t cap, const uint8_t *bytes, size_t n)
{
    size_t at = 0U;

    dst[0] = '\0';
    for ( size_t i = 0U; (i < n) && ((at + CONSOLE_HEX_PER_BYTE) < cap); ++i )
    {
        (void)snprintf(&dst[at], cap - at, "%02" PRIX32, (uint32_t)bytes[i]);
        at += CONSOLE_HEX_PER_BYTE;
    }
    return dst;
}

const char *console_ip4(char *dst, size_t cap, uint32_t ip4)
{
    uint32_t const b = CONSOLE_IP4_OCTET_BITS;

    (void)snprintf(dst, cap, "%" PRIu32 ".%" PRIu32 ".%" PRIu32 ".%" PRIu32,
                   ip4 & CONSOLE_IP4_OCTET_MASK,
                   (ip4 >> b) & CONSOLE_IP4_OCTET_MASK,
                   (ip4 >> (b + b)) & CONSOLE_IP4_OCTET_MASK,
                   (ip4 >> (b + b + b)) & CONSOLE_IP4_OCTET_MASK);
    return dst;
}

/* ---- Handlers: the file tree --------------------------------------------
 * The handlers live here because the Console struct is private; resolving the
 * path and asking AO_Storage are in console_fs.c, which the host suite drives.
 * A refusal from console_fs must close the command (fs_refuse()): nothing
 * will answer a request that was never posted.
 */

/**
 * @brief Refuse a verb whose owner the app did not link: `>err code=5 msg=absent`.
 *
 * A verb asks before cmd_begin(), so nothing is in flight to close.
 *
 * @param start The owner's start, named weakly at the top of this file.
 * @return true when the owner is absent and the refusal has been sent.
 */
static bool console_owner_absent(kaos_start_fn_t start)
{
    bool const absent = !kaos_linked(start);

    if ( absent )
    {
        console_reply_err(KCLI_ERR_REJECT, "absent");
    }
    return absent;
}

/** @brief Refuse the command just begun: the path never left this AO. */
static void fs_refuse(void)
{
    console_reply_err(KCLI_ERR_VALUE, "path");
    console_end();
}

/**
 * @brief ls [path=]: STATs the path first, then entries and `>ok`.
 *
 * KCLI_FS_LIST_STAT until path_answer() sees a directory, then KCLI_FS_LIST
 * for the burst STORAGE_LIST_END_SIG closes. A path `cd` refuses, `ls`
 * refuses the same way.
 */
static void h_ls(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    if ( console_owner_absent(&kaos_storage_start) )
    {
        return;
    }
    cmd_begin(me, KCLI_FS_LIST_STAT, c->verb);
    if ( console_fs_ls(&me->super, v[0].s) != KODE_OK )
    {
        fs_refuse();
    }
}

/** @brief cd path=: closed by STORAGE_PATH_RSP_SIG; the cwd moves there. */
static void h_cd(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    if ( console_owner_absent(&kaos_storage_start) )
    {
        return;
    }
    cmd_begin(me, KCLI_FS_CD, c->verb);
    if ( console_fs_chdir(&me->super, v[0].s) != KODE_OK )
    {
        fs_refuse();
    }
}

/** @brief stat path=: one record, closed by STORAGE_PATH_RSP_SIG. */
static void h_stat(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    if ( console_owner_absent(&kaos_storage_start) )
    {
        return;
    }
    cmd_begin(me, KCLI_FS_STAT, c->verb);
    if ( console_fs_stat(&me->super, v[0].s) != KODE_OK )
    {
        fs_refuse();
    }
}

/**
 * @brief cat path= [max=]: STATs the path first, then line records and `>ok`.
 *
 * `max` is kept now: v[] does not survive this call, and
 * console_fs_cat_begin() needs it after the STAT.
 */
static void h_cat(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    if ( console_owner_absent(&kaos_storage_start) )
    {
        return;
    }
    me->cat_max = KODE_FS_CAT_MAX_DEFAULT;
    if ( v[1].present )
    {
        me->cat_max = v[1].u;
    }
    cmd_begin(me, KCLI_FS_CAT_STAT, c->verb);
    if ( console_fs_cat(&me->super, v[0].s) != KODE_OK )
    {
        fs_refuse();
    }
}

/** @brief mkdir path=: closed by STORAGE_PATH_RSP_SIG. No STAT phase. */
static void h_mkdir(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    if ( console_owner_absent(&kaos_storage_start) )
    {
        return;
    }
    cmd_begin(me, KCLI_FS_MKDIR, c->verb);
    if ( console_fs_mkdir(&me->super, v[0].s) != KODE_OK )
    {
        fs_refuse();
    }
}

/**
 * @brief rm path=: STATs the path first, then deletes it. Files only.
 *
 * KCLI_FS_RM_STAT until path_answer() sees a file, then KCLI_FS_RM for the
 * delete. A directory is refused at the STAT.
 */
static void h_rm(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    if ( console_owner_absent(&kaos_storage_start) )
    {
        return;
    }
    cmd_begin(me, KCLI_FS_RM_STAT, c->verb);
    if ( console_fs_rm(&me->super, v[0].s) != KODE_OK )
    {
        fs_refuse();
    }
}

/**
 * @brief rmdir path=: STATs the path first, then removes it. Empty
 *        directories only; there is no recursive form.
 */
static void h_rmdir(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    if ( console_owner_absent(&kaos_storage_start) )
    {
        return;
    }
    cmd_begin(me, KCLI_FS_RMDIR_STAT, c->verb);
    if ( console_fs_rmdir(&me->super, v[0].s) != KODE_OK )
    {
        fs_refuse();
    }
}

/**
 * @brief mv src= dst=: renames src to dst. Same volume only.
 *
 * No STAT phase: one round trip, closed by STORAGE_PATH_RSP_SIG.
 */
static void h_mv(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    if ( console_owner_absent(&kaos_storage_start) )
    {
        return;
    }
    cmd_begin(me, KCLI_FS_MV, c->verb);
    if ( console_fs_pair(&me->super, (uint8_t)STORAGE_PAIR_MOVE,
                        v[0].s, v[1].s) != KODE_OK )
    {
        fs_refuse();
    }
}

/**
 * @brief cp src= dst=: copies src to dst, either direction across the two
 *        volumes.
 *
 * AO_Storage answers only when the copy is finished, a 32 KB chunk per step,
 * so the watchdog is extended to KCLI_CP_TIMEOUT_MS. Unlike `mv`, crossing
 * volumes is what this verb is for.
 */
static void h_cp(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    if ( console_owner_absent(&kaos_storage_start) )
    {
        return;
    }
    cmd_begin(me, KCLI_FS_CP, c->verb);
    console_extend(KCLI_CP_TIMEOUT_MS);
    if ( console_fs_pair(&me->super, (uint8_t)STORAGE_PAIR_COPY,
                        v[0].s, v[1].s) != KODE_OK )
    {
        fs_refuse();
    }
}

/**
 * @brief write path= [append=on] [line=<n>] text=: put a line of text in a
 *        file, or replace one existing line of it.
 *
 * `text` is quoted and may be empty (`text=""` writes an empty file); `\n`
 * inside it writes a newline. Without `append=on` the write truncates.
 * `line=` nonzero replaces that one line, through a temporary and a rename.
 * `append=on` with a nonzero `line=` is refused (`appendline`): one adds at
 * the end, the other replaces in the middle.
 */
static void h_write(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    if ( console_owner_absent(&kaos_storage_start) )
    {
        return;
    }
    bool     append = false;
    uint16_t line   = 0U;

    if ( v[1].present )
    {
        append = v[1].b;
    }
    if ( v[2].present )
    {
        line = (uint16_t)v[2].u;
    }
    if ( append && (0U != line) )
    {
        console_reply_err(KCLI_ERR_REJECT, "appendline");
        return;
    }
    cmd_begin(me, KCLI_FS_WRITE, c->verb);
    if ( console_fs_write(&me->super, v[0].s, v[3].s, append, line)
        != KODE_OK )
    {
        fs_refuse();
    }
}

/* ---- Which source may run what -------------------------------------------
 * A line comes from the cable, BLE, the Kode channel (NET) or AO_Scheduler
 * (SCHED). Most verbs never ask; the few that need something of their port
 * ask console_src_allows(), so the policy is one function. NET may edit the
 * scheduler's table; the table's own (SCHED) lines may not.
 */

/**
 * @brief Whether the line in hand may run, and the refusal when it may not.
 *
 * The refusal is `>err code=5 msg=cable` for BLE, `msg=net` for the channel
 * and `msg=sched` for the scheduler; no command is begun, so the caller just
 * returns.
 *
 * @param need What the verb asks of its port.
 * @return true when the verb may run.
 */
bool console_src_allows(console_need_t need)
{
    const Console * const me = &l_console;

    bool const ble   = ((uint8_t)KODE_CONSOLE_SRC_BLE == me->src);
    bool const net   = ((uint8_t)KODE_CONSOLE_SRC_NET == me->src);
    bool const sched = ((uint8_t)KODE_CONSOLE_SRC_SCHED == me->src);
    bool       ok    = true;

    switch ( need )
    {
        case CONSOLE_NEED_NOT_BLE:
            ok = !ble;
            break;
        case CONSOLE_NEED_WIRE:
            ok = !(net || sched);
            break;
        case CONSOLE_NEED_CABLE:
            ok = !(ble || net || sched);
            break;
        case CONSOLE_NEED_NOT_SCHED:
            ok = !sched;
            break;
        default:
            break;
    }
    if ( !ok )
    {
        if ( net )
        {
            console_reply_err(KCLI_ERR_REJECT, "net");
        }
        else if ( sched )
        {
            console_reply_err(KCLI_ERR_REJECT, "sched");
        }
        else
        {
            console_reply_err(KCLI_ERR_REJECT, "cable");
        }
    }
    return ok;
}

/**
 * @brief file_put path= size= crc32=: stream a file in over this very wire.
 *
 * The receive protocol (KCLI_RECV) `install src="terminal"` uses, aimed at a
 * path:
 *
 *     host:  file_put path="/storage/x.bin" size=N crc32=C
 *     board: >|ready chunk=4096     (destination open, wire switched to raw)
 *     host:  <4096 raw bytes>
 *     board: >|ack                  (chunk written; send the next)
 *     ...                           (last partial chunk earns no ack)
 *     board: >ok                    (or >err 5 / >err 6)
 *
 * `>ok` is a result, not an accept: transfer, CRC and close all happened.
 * Verified on the cable; on BLE untried, not refused. A bad path (code 3
 * `path`) or an empty event pool (code 5, the verb) closes the command here,
 * since nothing will answer.
 */
static void h_file_put(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    if ( console_owner_absent(&kaos_storage_start) )
    {
        return;
    }
    if ( !console_src_allows(CONSOLE_NEED_WIRE) )
    {
        return;
    }
    cmd_begin(me, KCLI_RECV, c->verb);

    /* The payload follows on the port the command arrived on. */
    kode_status_t const rc = console_fs_put(&me->super, v[0].s, v[1].u,
                                            v[2].u, me->src);
    if ( KODE_DENIED == rc )
    {
        fs_refuse();
    }
    else if ( KODE_OK != rc )
    {
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        console_end();
    }
    else
    {
        /* AO_Storage owns the round from here, including this AO's watchdog
         * from `>|ready` on. */
    }
}

/**
 * @brief pwd: where the working directory is.
 *
 * The one file verb that asks nobody: the cwd is this AO's own state, so
 * there is no owner to wait on and no in-flight kind to arm.
 */
static void h_pwd(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)me;
    (void)c;
    (void)v;
    char q[CONSOLE_QUOTED_MAX(KODE_PATH_MAX)];
    console_reply(">|path=%s", console_quote(q, sizeof(q), console_fs_cwd()));
    console_reply(">ok");
}

/**
 * @brief Turn AO_Storage's answer about one path into the wire's version.
 *
 * Each refusal is one word. KODE_DENIED is a bad value (`path`; `noline` for
 * a one-line write); any other failing `rc` is code 5, with the word for what
 * refused. On some branches AO_Storage repurposes `exists`, as the comments
 * below say. Mutations (`is_mutation`) never ask
 * what is at a path, so `is_dir`, and `exists` on success, mean nothing.
 */
static void path_answer(Console *me, const StoragePathRspEvt *r)
{
    bool const wants_dir  = ((uint8_t)KCLI_FS_CD == me->inflight)
                          || ((uint8_t)KCLI_FS_LIST_STAT == me->inflight)
                          || ((uint8_t)KCLI_FS_RMDIR_STAT == me->inflight);
    bool const wants_file = ((uint8_t)KCLI_FS_CAT_STAT == me->inflight)
                          || ((uint8_t)KCLI_FS_RM_STAT == me->inflight);
    bool const is_mutation = ((uint8_t)KCLI_FS_MKDIR == me->inflight)
                           || ((uint8_t)KCLI_FS_RM == me->inflight)
                           || ((uint8_t)KCLI_FS_RMDIR == me->inflight)
                           || ((uint8_t)KCLI_FS_MV == me->inflight)
                           || ((uint8_t)KCLI_FS_CP == me->inflight)
                           || ((uint8_t)KCLI_FS_WRITE == me->inflight);
    /* Set only for `write`'s one-line replace (STORAGE_PATH_WRITE_LINE). */
    bool const is_line_write = ((uint8_t)STORAGE_PATH_WRITE_LINE == r->op);

    if ( (uint8_t)KODE_DENIED == r->rc )
    {
        if ( is_line_write && r->exists )
        {
            /* `exists` repurposed: `line=` named no line the file has, a bad
             * value. `noline`, not `line`, which a refused `line=` key
             * already answers; the two must read differently. */
            console_reply_err(KCLI_ERR_VALUE, "noline");
        }
        else
        {
            /* A path on no volume, or an `mv` across two. */
            console_reply_err(KCLI_ERR_VALUE, "path");
        }
    }
    else if ( (uint8_t)KODE_OK != r->rc )
    {
        if ( ((uint8_t)KODE_IO_ERROR == r->rc)
            && (((uint8_t)KCLI_FS_MKDIR == me->inflight)
                || ((uint8_t)KCLI_FS_WRITE == me->inflight)
                || ((uint8_t)KCLI_FS_CP == me->inflight)) )
        {
            /* The volume mounted and the parent was there, but the medium
             * refused the write: not `volume`. */
            console_reply_err(KCLI_ERR_REJECT, "iofail");
        }
        else if ( (uint8_t)KODE_EXISTS == r->rc )
        {
            /* mkdir alone can reach this: the NAME is taken. */
            console_reply_err(KCLI_ERR_REJECT, "exists");
        }
        else if ( ((uint8_t)KODE_NOT_DIR == r->rc)
                 || ((uint8_t)KODE_IS_DIR == r->rc) )
        {
            /* A component of the path is a file (`mkdir /storage/n.txt/sub`),
             * or `write` met a directory. The medium did not fail: not
             * `iofail`. */
            const char *word = "notdir";
            if ( (uint8_t)KODE_IS_DIR == r->rc )
            {
                word = "isdir";
            }
            console_reply_err(KCLI_ERR_REJECT, word);
        }
        else if ( ((uint8_t)KCLI_FS_MKDIR == me->inflight)
            || ((uint8_t)KCLI_FS_RMDIR == me->inflight) )
        {
            /* KODE_ERROR is both a dead mount and a refused mutation. `exists`
             * (repurposed by answer_path_mkdir()/answer_path_rmdir()) is true
             * when the volume mounted and the mutation was refused. */
            if ( !r->exists )
            {
                console_reply_err(KCLI_ERR_REJECT, "volume");
            }
            else
            {
                const char *word = "noparent";
                if ( (uint8_t)KCLI_FS_RMDIR == me->inflight )
                {
                    word = "notempty";
                }
                console_reply_err(KCLI_ERR_REJECT, word);
            }
        }
        else if ( (((uint8_t)KCLI_FS_MV == me->inflight)
                 || ((uint8_t)KCLI_FS_CP == me->inflight)) && r->exists )
        {
            /* `exists` repurposed (storage_pair_request() in kaos.h):
             * AO_Storage found the SOURCE missing after the failure, which
             * is what a mistyped `src=` looks like. */
            console_reply_err(KCLI_ERR_REJECT, "src");
        }
        else if ( is_line_write && ((uint8_t)KODE_NO_RESOURCES == r->rc) )
        {
            /* The file, or the file with the new line in it, does not fit
             * the 32 KB rewrite buffer. The way round: `file_get`, edit on
             * the host, `file_put`. */
            console_reply_err(KCLI_ERR_REJECT, "toolarge");
        }
        else
        {
            console_reply_err(KCLI_ERR_REJECT, "volume");
        }
    }
    else if ( !is_mutation && !r->exists )
    {
        console_reply_err(KCLI_ERR_REJECT, "missing");
    }
    else if ( (wants_dir && !r->is_dir) || (wants_file && r->is_dir) )
    {
        /* cd and the ls and rmdir STATs want a directory; the cat and rm
         * STATs want a file. */
        const char *word = "notdir";
        if ( wants_file )
        {
            word = "isdir";
        }
        console_reply_err(KCLI_ERR_REJECT, word);
    }
    else if ( (uint8_t)KCLI_FS_CD == me->inflight )
    {
        console_fs_enter();
        console_reply(">ok");
    }
    else if ( (uint8_t)KCLI_FS_LIST_STAT == me->inflight )
    {
        /* The STAT that guards `ls` found a directory. The command stays in
         * flight under the one watchdog cmd_begin() already armed - only
         * what it is waiting for changes - so this returns instead of
         * falling through to console_end(). */
        me->inflight = (uint8_t)KCLI_FS_LIST;
        console_fs_list_begin(&me->super);
        return;
    }
    else if ( (uint8_t)KCLI_FS_CAT_STAT == me->inflight )
    {
        /* The STAT that guards `cat` found a file: on to the read phase, on
         * KCLI_CAT_TIMEOUT_MS since AO_Storage paces the lines. */
        me->inflight = (uint8_t)KCLI_FS_CAT;
        console_extend(KCLI_CAT_TIMEOUT_MS);
        console_fs_cat_begin(&me->super, me->cat_max);
        return;
    }
    else if ( (uint8_t)KCLI_FS_RM_STAT == me->inflight )
    {
        /* The STAT that guards `rm` found a file - the KCLI_FS_CAT_STAT
         * branch above with a different phase two. */
        me->inflight = (uint8_t)KCLI_FS_RM;
        console_fs_rm_begin(&me->super);
        return;
    }
    else if ( (uint8_t)KCLI_FS_RMDIR_STAT == me->inflight )
    {
        /* The STAT that guards `rmdir` found a directory. Whether it is
         * empty is rmdir()'s question, one round trip on. */
        me->inflight = (uint8_t)KCLI_FS_RMDIR;
        console_fs_rmdir_begin(&me->super);
        return;
    }
    else if ( is_mutation )
    {
        /* mkdir's only round trip, and the second round trip of rm and
         * rmdir: the path question is already settled (or, for mkdir and
         * mv, was never asked), so `rc` alone says whether it worked. */
        console_reply(">ok");
    }
    else
    {
        const char * const path = console_fs_target();
        const char        *type = "file";
        if ( r->is_dir )
        {
            type = "dir";
        }
        char q[CONSOLE_QUOTED_MAX(KODE_PATH_MAX)];
        console_reply(">|path=%s type=%s size=%lu medium=%s",
              console_quote(q, sizeof(q), path), type,
              (unsigned long)r->size, console_fs_medium_name(path));
        console_reply(">ok");
    }
    console_end();
}

/* ---- Handlers: apps ------------------------------------------------------ */

#if CONFIG_KODEOS_BOOT_ROLE_OS
/* ---- Handlers: the factory reset ----------------------------------------- */

/**
 * @brief factory_reset confirm=on: forget everything the board remembers,
 *        then restart into the welcome app. The cable alone.
 *
 * AO_Boot walks the steps and answers once; the console waits for the sum of
 * their budgets. On success the board restarts a moment after the answer is
 * printed, so the caller reads `>|factory_reset result=done` and `>ok` first.
 */
static void h_factory_reset(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    if ( !console_src_allows(CONSOLE_NEED_CABLE) )
    {
        return;
    }
    if ( !v[0].b )
    {
        console_reply_err(KCLI_ERR_VALUE, "confirm");
        return;
    }
#if CONFIG_KODE_TEST_DOT
    /* A reset under a run would erase what the run is judging. */
    if ( test_dot_busy() )
    {
        console_reply_err(KCLI_ERR_BUSY, "busy");
        return;
    }
#endif
    cmd_begin(me, KCLI_RESET, c->verb);
    console_extend(KODE_RESET_CONSOLE_MS);
    if ( !boot_factory_reset_request(KBOOT_RESET_RESTART, AO_Console) )
    {
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        console_end();
    }
}
#endif

/**
 * @brief The `src="terminal"` half of `install`: stream the image in over
 *        this very wire.
 *
 * The protocol `file_put` uses. `>ok` is a result (transfer, CRC and install
 * all happened), not an accept:
 *
 *     host:  install src="terminal" dst=app size=N crc32=C run=on name="x"
 *     board: >|evicted name="y" kept=off   (one per app it costs, if any)
 *     board: >|ready chunk=4096     (destination open, wire switched to raw)
 *     host:  <4096 raw bytes>
 *     board: >|ack                  (chunk written; send the next)
 *     ...                           (last partial chunk earns no ack)
 *     board: >ok                    (or >err 5 / >err 6)
 *
 * The stop-and-wait is the flow control; the wire has none. `dst=c5` goes to
 * AO_Radio and is cable only: activating it reboots the C5, the BLE radio, so
 * a `>ok` over BLE could never arrive. Other destinations are AO_AppMgr's and
 * take the cable or BLE; the Kode channel takes neither.
 *
 * @param me    The console; @c me->src is the wire the payload follows on.
 * @param c     The command, for the verb a refusal names.
 * @param dst   kfw_target_t, or KODE_FLASH_DST_PENDING.
 * @param size  The bytes the host promises.
 * @param crc32 Their CRC-32.
 * @param run   Boot the app once it is installed (`dst=app` only).
 * @param name  The image's project name as the host read it, or "" (`dst=app`
 *              only): the installed app of that name gives its place first.
 */
static void install_from_wire(Console *me, const kcmd_t *c, uint8_t dst,
                              uint32_t size, uint32_t crc32, bool run,
                              const char *name)
{
    console_need_t  need  = CONSOLE_NEED_WIRE;
    kaos_start_fn_t owner = &kaos_appmgr_start;
    if ( (uint8_t)KFW_TARGET_C5 == dst )
    {
        need  = CONSOLE_NEED_CABLE;
        owner = &kaos_radio_start;
    }
    if ( console_owner_absent(owner) )
    {
        return;
    }
    if ( !console_src_allows(need) )
    {
        return;
    }
    cmd_begin(me, KCLI_RECV, c->verb);

    /* The payload follows on the port the command arrived on. */
    bool ok = false;
    if ( (uint8_t)KFW_TARGET_C5 == dst )
    {
        ok = c5_recv_request(size, crc32, me->src);
    }
    else
    {
        ok = appmgr_recv_request(dst, size, crc32, run, me->src, name);
    }
    if ( !ok )
    {
        /* The event pool was dry and nothing was posted: close here, or
         * the watchdog would report a false timeout. */
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        console_end();
    }
}

/** How long `install` waits for AO_AppMgr's verdict: the deadline AO_AppMgr gives a
 *  medium to open a file, then the console's own. AO_AppMgr answers every
 *  request it takes, so this only bounds a lost answer. */
#define KCLI_INSTALL_MS  (KODE_APPMGR_OPEN_MS + KCLI_CMD_TIMEOUT_MS)

/**
 * @brief Close the command in flight with AO_AppMgr's verdict on its request.
 *
 * `>ok` for a request taken; code 4 `busy` when AO_AppMgr had work in hand;
 * code 5 `missing` for a file that is not on the medium or an app that is not
 * installed, `empty` for a mailbox with nothing in it, `refused` for anything
 * else AO_AppMgr turned down.
 *
 * @param verdict app_req_verdict_t.
 */
void console_reply_app_verdict(uint8_t verdict)
{
    switch ( (app_req_verdict_t)verdict )
    {
        case APP_REQ_TAKEN: {
            console_reply(">ok");
            break;
        }
        case APP_REQ_BUSY: {
            console_reply_err(KCLI_ERR_BUSY, "busy");
            break;
        }
        case APP_REQ_MISSING: {
            console_reply_err(KCLI_ERR_REJECT, "missing");
            break;
        }
        case APP_REQ_EMPTY: {
            console_reply_err(KCLI_ERR_REJECT, "empty");
            break;
        }
        default: {
            console_reply_err(KCLI_ERR_REJECT, "refused");
            break;
        }
    }
    console_end();
}

/**
 * @brief install src=<where from> dst=<where to> [run=on|off]
 *        [size=<n> crc32=<n>].
 *
 * `dst=` is `os`, `c5`, `pending` (the mailbox) or `app`, which boots once
 * only on `run=on`; `run` is refused for any other `dst`. `src=` is
 * `fs:name`, `sd:name`, `http(s)://...`, `"pending"` or `"terminal"`. Only
 * `"terminal"` takes `size` and `crc32`, and only there is `>ok` a result: off
 * a medium or the mailbox it says AO_AppMgr took the install, and the
 * verdict decides (console_reply_app_verdict()).
 */
static void h_install(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    const char   *spec = v[0].s;
    uint8_t const dst  = (uint8_t)v[1].u;
    uint8_t       kind;

    if ( v[2].present && ((uint8_t)KFW_TARGET_APP != dst) )
    {
        console_reply_err(KCLI_ERR_VALUE, "run");
        return;
    }
    bool const run = v[2].present && v[2].b;

    /* Only a stream into the region needs telling what it replaces: a medium's
     * image is read before the room is made. */
    if ( v[5].present
        && ((strcmp(spec, "terminal") != 0) || ((uint8_t)KFW_TARGET_APP != dst)) )
    {
        console_reply_err(KCLI_ERR_VALUE, "name");
        return;
    }
    if ( strcmp(spec, "terminal") == 0 )
    {
        if ( !v[3].present )
        {
            console_reply_err(KCLI_ERR_MISSING, "size");
            return;
        }
        if ( !v[4].present )
        {
            console_reply_err(KCLI_ERR_MISSING, "crc32");
            return;
        }
        const char *name = "";
        if ( v[5].present )
        {
            name = v[5].s;
        }
        install_from_wire(me, c, dst, v[3].u, v[4].u, run, name);
        return;
    }
    if ( v[3].present )
    {
        console_reply_err(KCLI_ERR_VALUE, "size");
        return;
    }
    if ( v[4].present )
    {
        console_reply_err(KCLI_ERR_VALUE, "crc32");
        return;
    }

    if ( strncmp(spec, "fs:", 3U) == 0 )
    {
        kind = (uint8_t)KFW_SRC_FS;   spec += 3;
    }
    else if ( strncmp(spec, "sd:", 3U) == 0 )
    {
        kind = (uint8_t)KFW_SRC_SD;   spec += 3;
    }
    else if ( (strncmp(spec, "http://", 7U) == 0)
               || (strncmp(spec, "https://", 8U) == 0) )
    {
        kind = (uint8_t)KFW_SRC_HTTP;
    }
    else if ( strcmp(spec, "pending") == 0 )
    {
        kind = (uint8_t)KFW_SRC_PENDING;
        if ( (uint8_t)KODE_FLASH_DST_PENDING == dst )
        {
            /* The mailbox into itself: nothing to move. */
            console_reply_err(KCLI_ERR_VALUE, "dst");
            return;
        }
    }
    else
    {
        /* A bare name means the media, searched in the order kdrv_storage
         * already searches them: the card first, because putting a card in
         * with the file on it says plainly what you meant. */
        kind = (uint8_t)KFW_SRC_FS;
    }

    if ( '\0' == *spec )
    {
        console_reply_err(KCLI_ERR_VALUE, "src");
        return;
    }
    if ( (uint8_t)KFW_TARGET_C5 == dst )
    {
        if ( console_owner_absent(&kaos_radio_start) )
        {
            return;
        }
    }
    else if ( console_owner_absent(&kaos_appmgr_start) )
    {
        return;
    }
    else
    {
        /* the apps region's owner is here */
    }
    /* The coprocessor is AO_Radio's, not AO_AppMgr's. */
    if ( (uint8_t)KFW_TARGET_C5 == dst )
    {
        /* As off the wire: the update resets the C5, which ends BLE and the
         * channel, and from the channel it would hand the coprocessor that
         * holds the unit's secrets to whatever image a URL names. */
        if ( !console_src_allows(CONSOLE_NEED_CABLE) )
        {
            return;
        }
        c5_flash_request(kind, spec);
        console_reply(">ok");
        return;
    }
    cmd_begin(me, KCLI_INSTALL, c->verb);
    console_extend(KCLI_INSTALL_MS);
    if ( !appmgr_image_ask(kind, spec, dst, run, &me->super) )
    {
        /* Nothing was posted, so nobody will answer. */
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        console_end();
    }
}

/* ---- Handlers: the app data folder ---------------------------------------
 * https://docs.kode.diy/sdk/kdrv/apps gives the wire format; the walk these three
 * verbs share is described at kcli_ad_phase_t.
 */

/** @brief The one-word name of a medium, for a record. */
static const char *ad_medium_name(uint8_t m)
{
    const char *name = "none";
    if ( (uint8_t)APPDATA_MEDIUM_FS == m )
    {
        name = "fs";
    }
    else if ( (uint8_t)APPDATA_MEDIUM_SD == m )
    {
        name = "sd";
    }
    else
    {
        /* none */
    }
    return name;
}

/**
 * @brief The one-word name of an icon's status.
 *
 * Here, not in kqp: AppDataIconStatus is kqp's, and kqp names no strings.
 */
static const char *ad_icon_name(uint8_t st)
{
    static const char *const NAMES[] = { "ok", "none", "size", "path", "busy" };
    return ((uint8_t)(sizeof(NAMES) / sizeof(NAMES[0])) > st) ? NAMES[st]
                                                              : "none";
}

/** @brief What a gathered slot is, for the record's `app=`. */
static const char *ad_kind_name(uint8_t kind)
{
    const char *name = "orphan";
    if ( (uint8_t)KCLI_AD_INSTALLED_APP == kind )
    {
        name = "installed";
    }
    else if ( (uint8_t)KCLI_AD_FILE == kind )
    {
        name = "file";
    }
    else
    {
        /* orphan */
    }
    return name;
}

/** @brief Whether the command in flight is the listing rather than the one. */
static bool ad_is_ls(const Console *me)
{
    return (uint8_t)KCLI_APPDATA_LS == me->inflight;
}

/**
 * @brief Whether @p name is already gathered, ignoring slot @p skip.
 * @param skip KCLI_AD_MAX to compare against every slot.
 */
static bool ad_has(const Console *me, const char *name, uint8_t skip)
{
    for ( uint8_t i = 0U; i < me->ad_n; ++i )
    {
        if ( (i != skip) && ((uint8_t)KCLI_AD_DROPPED != me->ad_kind[i])
            && (strcmp(me->ad_name[i], name) == 0) )
        {
            return true;
        }
    }
    return false;
}

/** @brief Take one more slot, or quietly stop at the ceiling. */
static void ad_add(Console *me, const char *name, kcli_ad_kind_t kind)
{
    if ( (uint8_t)KCLI_AD_MAX <= me->ad_n )
    {
        KLOG_W(TAG, "the listing stopped at %u projects (the ceiling)",
               (unsigned)me->ad_n);
        return;
    }
    (void)snprintf(me->ad_name[me->ad_n], sizeof(me->ad_name[0]), "%s", name);
    me->ad_kind[me->ad_n] = (uint8_t)kind;
    ++me->ad_n;
}

static void ad_phase_enter(Console *me, kcli_ad_phase_t phase);

/** @brief Close the verb: every row that was going to be printed has been. */
static void ad_finish(void)
{
    console_reply(">ok");
    console_end();
}

/** @brief Ask about the next .bin whose manifest is still unread. */
static void ad_desc_next(Console *me)
{
    /* `info` stops the moment it has its binary: the rest of the scan would
     * be manifest reads nobody is going to look at. */
    if ( !ad_is_ls(me) && me->ad_bin )
    {
        ad_phase_enter(me, KCLI_AD_ASK);
        return;
    }
    while ( (me->ad_at < me->ad_n)
           && ((uint8_t)KCLI_AD_FILE_PENDING != me->ad_kind[me->ad_at]) )
    {
        ++me->ad_at;
    }
    if ( me->ad_at >= me->ad_n )
    {
        ad_phase_enter(me, ad_is_ls(me) ? KCLI_AD_FOLDERS_FS : KCLI_AD_ASK);
        return;
    }
    console_extend(KCLI_AD_ROW_MS);
    storage_desc_request(me->ad_name[me->ad_at], &me->super);
}

/** @brief Ask the folder question about the next row, or close the verb. */
static void ad_ask_next(Console *me)
{
    if ( !ad_is_ls(me) )
    {
        /* One project, asked once: `ad_at` counts the asking, not a slot. */
        if ( 0U != me->ad_at )
        {
            ad_finish();
            return;
        }
        me->ad_at = 1U;
        console_extend(KCLI_AD_ROW_MS);
        appdata_request(me->ad_target, &me->super);
        return;
    }
    while ( (me->ad_at < me->ad_n)
           && ((uint8_t)KCLI_AD_DROPPED == me->ad_kind[me->ad_at]) )
    {
        ++me->ad_at;
    }
    if ( me->ad_at >= me->ad_n )
    {
        ad_finish();
        return;
    }
    console_extend(KCLI_AD_ROW_MS);
    appdata_request(me->ad_name[me->ad_at], &me->super);
}

/** @brief Move the gather to @p phase and ask whatever that phase asks. */
static void ad_phase_enter(Console *me, kcli_ad_phase_t phase)
{
    me->ad_phase = (uint8_t)phase;
    switch ( phase )
    {
        case KCLI_AD_FILES: {
            console_extend(KCLI_AD_ROW_MS);
            me->list_next = 0U;
            me->list_from = 0U;
            storage_list_request(&me->super);
            break;
        }
        case KCLI_AD_DESC: {
            me->ad_at = 0U;
            ad_desc_next(me);
            break;
        }
        case KCLI_AD_FOLDERS_FS: {
            console_extend(KCLI_AD_ROW_MS);
            storage_path_request(&me->super, (uint8_t)STORAGE_PATH_LIST,
                                 KCLI_AD_DIR_FS);
            break;
        }
        case KCLI_AD_FOLDERS_SD: {
            console_extend(KCLI_AD_ROW_MS);
            storage_path_request(&me->super, (uint8_t)STORAGE_PATH_LIST,
                                 KCLI_AD_DIR_SD);
            break;
        }
        case KCLI_AD_ASK: {
            me->ad_at = 0U;
            ad_ask_next(me);
            break;
        }
        default: {
            /* KCLI_AD_INSTALLED is entered by the handler itself. */
            break;
        }
    }
}

/**
 * @brief Start the walk both listing verbs share.
 *
 * @param target The one project `appdata_info` was asked about, or NULL for
 *        the listing. Taken before the first question goes out, never after:
 *        the answer decides what the record handlers do with it.
 */
static void ad_begin(Console *me, kcli_inflight_t what, const kcmd_t *c,
                     const char *target)
{
    cmd_begin(me, what, c->verb);
    (void)snprintf(me->ad_target, sizeof(me->ad_target), "%s",
                   (NULL != target) ? target : "");
    me->ad_n     = 0U;
    me->ad_at    = 0U;
    me->ad_phase = (uint8_t)KCLI_AD_INSTALLED;
    me->list_next = 0U;
    me->list_from = 0U;
    me->ad_bin   = false;
    me->ad_bin_name[0]     = '\0';
    me->ad_bin_version[0]  = '\0';
    me->ad_bin_category[0] = '\0';
    me->ad_medium = (uint8_t)APPDATA_MEDIUM_NONE;
    me->ad_json   = (uint8_t)KAPP_JSON_NONE;
    me->ad_icon   = (uint8_t)APPDATA_ICON_NONE;
    appmgr_list_request(&me->super);
}

/** @brief appdata_ls: one record per project that has a binary or a folder. */
static void h_appdata_ls(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    if ( console_owner_absent(&kaos_storage_start) )
    {
        return;
    }
    if ( console_owner_absent(&kaos_appmgr_start) )
    {
        return;
    }
    (void)v;
    ad_begin(me, KCLI_APPDATA_LS, c, NULL);
}

/** @brief appdata_info name=<project>: every field, and where it came from. */
static void h_appdata_info(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    if ( console_owner_absent(&kaos_storage_start) )
    {
        return;
    }
    if ( console_owner_absent(&kaos_appmgr_start) )
    {
        return;
    }
    ad_begin(me, KCLI_APPDATA_INFO, c, v[0].s);
}

/** @brief appdata_rm name=<project>: closed by APPDATA_RM_RSP_SIG, or at once
 *        when the request is refused. */
static void h_appdata_rm(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    if ( console_owner_absent(&kaos_storage_start) )
    {
        return;
    }
    cmd_begin(me, KCLI_APPDATA_RM, c->verb);
    kode_status_t const rc = appdata_rm_request(v[0].s, &me->super);
    if ( KODE_OK != rc )
    {
        /* Nothing was posted, so nobody will answer: close here rather than
         * let the watchdog turn the refusal into a fake timeout. */
        console_reply_err(KCLI_ERR_REJECT, console_status_word((uint32_t)rc));
        console_end();
    }
}

/** How long `appdata_get` may take: two files, each with the deadline
 *  AO_AppMgr gives a transfer's answer, then AO_Storage's write. AO_AppMgr
 *  answers every fetch, so this only bounds a lost answer. */
#define KCLI_APPDATA_GET_MS  ((2U * KODE_APPMGR_OPEN_MS) + APPMGR_TIDY_MS + KCLI_CMD_TIMEOUT_MS)

/** @brief appdata_get name=<project> url=<base>: fetch the app's data folder
 *        from `<base>app.json` and `<base>icon.k64`; closed by
 *        APPDATA_GET_RSP_SIG, or at once when the request is refused. */
static void h_appdata_get(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    if ( console_owner_absent(&kaos_storage_start) )
    {
        return;
    }
    if ( console_owner_absent(&kaos_appmgr_start) )
    {
        return;
    }
    if ( strlen(v[1].s) >= (size_t)KODE_APPDATA_URL_MAX )
    {
        console_reply_err(KCLI_ERR_VALUE, "url");
        return;
    }
    cmd_begin(me, KCLI_APPDATA_GET, c->verb);
    kode_status_t const rc = appmgr_appdata_get_request(v[0].s, v[1].s, &me->super);
    if ( KODE_OK != rc )
    {
        /* Nothing was posted, so nobody will answer. */
        console_reply_err(KCLI_ERR_REJECT, console_status_word((uint32_t)rc));
        console_end();
        return;
    }
    console_extend(KCLI_APPDATA_GET_MS);
}

/**
 * @brief The word an `appdata_get` that made no folder ends with.
 *
 * @param why How the fetch ended (appdata_get_why_t).
 * @return The word after `msg=`.
 */
static const char *ad_get_word(uint8_t why)
{
    /* In appdata_get_why_t's order. */
    static const char *const WORDS[] = { "ok", "busy", "name", "fetch",
                                         "json", "icon", "full", "store" };
    _Static_assert((sizeof(WORDS) / sizeof(WORDS[0]))
                       == ((size_t)APPDATA_GET_STORE + 1U),
                   "one word for each way a fetch ends");
    const char *word = "store";

    if ( (uint8_t)(sizeof(WORDS) / sizeof(WORDS[0])) > why )
    {
        word = WORDS[why];
    }
    return word;
}

/**
 * @brief `appdata_info`'s seven records, each saying which source won.
 *
 * Applies the spec's section C precedence itself, not the GUI's result.
 * AppDataFieldsEvt carries the icon's STATUS, not its path (no room in a
 * medium-pool event), so the icon's `value` says icon or monogram and
 * `status` says why.
 */
static void ad_info_records(Console *me, const AppDataFieldsEvt *f)
{
    /* Sized for the widest of the texts below, the category, every byte of it
     * escaped; the rest are shorter and fit the same buffer. */
    char q[CONSOLE_QUOTED_MAX(KODE_APP_CATEGORY_MAX)];
    char cats[KODE_APP_CATEGORY_MAX];

    /* The project asked for, not the one in the answer: a busy answer from
     * a dry pool is static and carries none. */
    console_reply(">|project=%s medium=%s", console_quote(q, sizeof(q), me->ad_target),
          ad_medium_name(f->medium));

    if ( '\0' != f->name[0] )
    {
        console_reply(">|field=name src=folder value=%s",
              console_quote(q, sizeof(q), f->name));
    }
    else if ( '\0' != me->ad_bin_name[0] )
    {
        console_reply(">|field=name src=binary value=%s",
              console_quote(q, sizeof(q), me->ad_bin_name));
    }
    else
    {
        console_reply(">|field=name src=default value=%s",
              console_quote(q, sizeof(q), me->ad_target));
    }

    if ( '\0' != f->category[0] )
    {
        console_reply(">|field=category src=folder value=%s",
              console_quote(q, sizeof(q), f->category));
    }
    else if ( kapp_cat_resolve(me->ad_target, cats, sizeof(cats)) )
    {
        console_reply(">|field=category src=table value=%s",
              console_quote(q, sizeof(q), cats));
    }
    else if ( '\0' != me->ad_bin_category[0] )
    {
        console_reply(">|field=category src=binary value=%s",
              console_quote(q, sizeof(q), me->ad_bin_category));
    }
    else
    {
        console_reply(">|field=category src=default value=\"other\"");
    }

    if ( '\0' != f->version[0] )
    {
        console_reply(">|field=version src=folder value=%s",
              console_quote(q, sizeof(q), f->version));
    }
    else if ( '\0' != me->ad_bin_version[0] )
    {
        console_reply(">|field=version src=binary value=%s",
              console_quote(q, sizeof(q), me->ad_bin_version));
    }
    else
    {
        console_reply(">|field=version src=default value=-");
    }

    if ( '\0' != f->description[0] )
    {
        console_reply(">|field=description src=folder value=%s",
              console_quote(q, sizeof(q), f->description));
    }
    else
    {
        console_reply(">|field=description src=default value=-");
    }

    if ( (uint8_t)APPDATA_ICON_OK == f->icon_status )
    {
        console_reply(">|field=icon src=folder value=icon status=ok");
    }
    else
    {
        console_reply(">|field=icon src=default value=monogram status=%s",
              ad_icon_name(f->icon_status));
    }

    if ( f->color_valid )
    {
        console_reply(">|field=color src=folder value=#%02X%02X%02X",
              (unsigned)f->color[0], (unsigned)f->color[1],
              (unsigned)f->color[2]);
    }
    else
    {
        /* The default is kgui's `color_pill_default`, a GUI fact this layer
         * has no business naming. `-` says "whatever the launcher's own is". */
        console_reply(">|field=color src=default value=-");
    }
}

/** @brief `appdata_ls`'s one record for the row just asked about. */
static void ad_ls_record(Console *me)
{
    if ( me->ad_at >= me->ad_n )
    {
        return;
    }
    char q[CONSOLE_QUOTED_MAX(KODE_APP_FILE_MAX)];
    console_reply(">|project=%s medium=%s json=%s icon=%s app=%s",
          console_quote(q, sizeof(q), me->ad_name[me->ad_at]),
          ad_medium_name(me->ad_medium),
          kapp_json_status_name((kapp_json_status_t)me->ad_json),
          ad_icon_name(me->ad_icon), ad_kind_name(me->ad_kind[me->ad_at]));
}

/**
 * @brief One entry of whichever listing the gather is waiting on.
 *
 * Listings are published, so this only ever runs with one of the two verbs
 * in flight - the caller has already checked that much - and the phase is
 * what says whether the entry is a .bin or a folder.
 */
static void ad_entry(Console *me, const StorageEntryEvt *en)
{
    if ( (uint8_t)KCLI_AD_FILES == me->ad_phase )
    {
        /* A page asked twice repeats its entries: only the one at the next
         * position is new. */
        if ( en->pos != me->list_next )
        {
            return;
        }
        ++me->list_next;
        /* Not deduplicated by file name: two media may carry the same name
         * and mean two different projects, and the manifest is what says so.
         * The DESC phase drops whichever of them turns out to be a repeat. */
        if ( !en->is_dir && console_is_bin(en->name) )
        {
            ad_add(me, en->name, KCLI_AD_FILE_PENDING);
        }
    }
    else if ( ((uint8_t)KCLI_AD_FOLDERS_FS == me->ad_phase)
             || ((uint8_t)KCLI_AD_FOLDERS_SD == me->ad_phase) )
    {
        /* A folder whose name is already a project is that project's folder;
         * only the ones left over are orphans. */
        if ( en->is_dir && !ad_has(me, en->name, (uint8_t)KCLI_AD_MAX) )
        {
            ad_add(me, en->name, KCLI_AD_ORPHAN);
        }
    }
    else
    {
        /* Somebody else's listing, or one this phase did not ask for. */
    }
}

/** @brief One manifest, for the .bin in slot `ad_at`. */
static void ad_desc(Console *me, const StorageDescRspEvt *r)
{
    if ( me->ad_at >= me->ad_n )
    {
        return;
    }
    if ( ((kode_status_t)r->status != KODE_OK) || ('\0' == r->project[0]) )
    {
        me->ad_kind[me->ad_at] = (uint8_t)KCLI_AD_DROPPED;
    }
    else if ( ad_is_ls(me) )
    {
        if ( ad_has(me, r->project, me->ad_at) )
        {
            me->ad_kind[me->ad_at] = (uint8_t)KCLI_AD_DROPPED;
        }
        else
        {
            (void)snprintf(me->ad_name[me->ad_at], sizeof(me->ad_name[0]),
                           "%s", r->project);
            me->ad_kind[me->ad_at] = (uint8_t)KCLI_AD_FILE;
        }
    }
    else
    {
        if ( strcmp(r->project, me->ad_target) == 0 )
        {
            (void)snprintf(me->ad_bin_name, sizeof(me->ad_bin_name), "%s",
                           r->display);
            (void)snprintf(me->ad_bin_category, sizeof(me->ad_bin_category),
                           "%s", r->category);
            me->ad_bin = true;   /* a file carries no version of its own */
        }
        me->ad_kind[me->ad_at] = (uint8_t)KCLI_AD_DROPPED;
    }
    ++me->ad_at;
    ad_desc_next(me);
}

/* ---- Handlers: one-shot asks -------------------------------------------- */

/** @brief ping: answered here, >|pong then >ok; no owner is asked. */
static void h_ping(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)me; (void)c; (void)v;
    console_reply(">|pong");
    console_reply(">ok");
}

/** @brief btn_read: one record per button when BTN_STATE_SIG answers. */
static void h_btn_read(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)v;
    cmd_begin(me, KCLI_BTN, c->verb);
    buttons_dump_request();
}

/* The word lists of the injected-finger and button verbs' specs (A_TOUCH,
 * A_TAP, A_BTN below), defined here because the handlers below log them. */
static const char *const A_TOUCH_STATES[] = { "down", "up", "move" };
/* In kdrv_button_t's order, so the index is the id. */
static const char *const A_BTN_IDS[] = {
    "up", "down", "left", "right", "top", "bot"
};
/* The two edges, then the two other things AO_Buttons says about a button:
 * that it has been held, and that the top one was pressed twice. h_btn tells
 * them apart by index. */
static const char *const A_BTN_STATES[] = { "press", "release", "hold", "double" };
/* The words' indices, named once: h_btn and the args spec below agree on them. */
#define A_BTN_PRESS     0U   /**< Index of "press" in A_BTN_STATES.   */
#define A_BTN_HOLD      2U   /**< Index of "hold" in A_BTN_STATES.    */
#define A_BTN_DOUBLE    3U   /**< Index of "double" in A_BTN_STATES.  */
#define A_BTN_N_STATES  4U   /**< How many words A_BTN_STATES holds.  */
_Static_assert(Q_DIM(A_BTN_STATES) == A_BTN_N_STATES,
               "A_BTN_N_STATES must count the words of A_BTN_STATES");

/* ---- Handlers: the injected finger ---------------------------------------
 * These publish the SAME events AO_Touch and AO_Buttons publish; no device is
 * touched, so no claim is needed. Nothing marks a sample as injected, on
 * purpose: do not add a flag a subscriber could start to depend on.
 */

/**
 * @brief Publish one touch sample, as AO_Touch would.
 *
 * @param x       Where the finger is.
 * @param y       Where the finger is.
 * @param pressed Whether it is down.
 * @param t_ms    The stamp the sample carries.
 * @return false when the pool had no block: nothing was published.
 */
static bool touch_publish(int16_t x, int16_t y, bool pressed, uint32_t t_ms)
{
    TouchEvt *e = Q_NEW_X(TouchEvt, 0U, TOUCH_EVT_SIG);

    if ( NULL == e )
    {
        return false;
    }
    e->x       = x;
    e->y       = y;
    e->pressed = pressed;
    e->t_ms    = t_ms;
    QACTIVE_PUBLISH(&e->super, &l_console.super);
    return true;
}

/**
 * @brief touch: one sample, exactly as the panel would have reported it.
 *
 * Refused while a `tap` or `swipe` is in flight: its release publishes at the
 * armed coordinates, and a `touch` in between would make it land elsewhere.
 */
static void h_touch(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    if ( KCLI_INJ_NONE != me->inj )
    {
        console_reply_err(KCLI_ERR_BUSY, "busy");
        return;
    }
    /* "move" is a sample with the finger still down: index 1 is "up" and
     * it is the only one of the three that is not pressed. */
    bool const sent = touch_publish((int16_t)v[0].u, (int16_t)v[1].u,
                                    (1U != v[2].u), khal_time_ms());
    /* down/up only: logging every "move" would flood the log. */
    if ( sent && (2U != v[2].u) )
    {
        KLOG_I(TAG, "console: injected touch x=%d y=%d state=%s",
               (int)v[0].u, (int)v[1].u, A_TOUCH_STATES[v[2].u]);
    }
    console_reply_posted(c->verb, sent);
}

/**
 * @brief Publish a sample an injected gesture owes after its press.
 *
 * LVGL is already holding a finger down, so a full pool is a stall and the
 * caller retries. The warning is logged once per stall: KLOG_W uses the large
 * pool, and a warning per retry would flood AO_Log's queue while the small
 * pool is starved.
 *
 * @param me      The console.
 * @param x       Where the finger is.
 * @param y       Where the finger is.
 * @param pressed Whether it is down.
 * @param t_ms    The stamp the sample carries.
 * @return true when the sample went out.
 */
static bool inj_send(Console *me, int16_t x, int16_t y, bool pressed,
                     uint32_t t_ms)
{
    bool const sent = touch_publish(x, y, pressed, t_ms);

    if ( sent )
    {
        me->inj_stalled = false;
    }
    else if ( !me->inj_stalled )
    {
        KLOG_W(TAG, "injected touch stalled (pool full); retrying");
        me->inj_stalled = true;
    }
    else
    {
        /* Already said. */
    }
    return sent;
}

/**
 * @brief Lift the finger an injected gesture has put down, or ask again later.
 *
 * The press already reached the GUI, so the release is never dropped: a full
 * pool re-arms @c inj_tmo (not from the pool) and tries again. @c inj stays
 * set, so a second gesture is refused until the release goes out.
 *
 * @param me   The console.
 * @param t_ms The stamp of the release: a tap's is the time it goes out, a
 *             swipe's is the time it was planned for.
 */
static void inj_release(Console *me, uint32_t t_ms)
{
    /* A swipe's timer is periodic and a tap's has already fired; either way
     * nothing more is due from it but this. */
    (void)QTimeEvt_disarm(&me->inj_tmo);
    if ( inj_send(me, me->inj_x, me->inj_y, false, t_ms) )
    {
        me->inj = KCLI_INJ_NONE;
    }
    else
    {
        QTimeEvt_armX(&me->inj_tmo, KQP_TICKS_PER_MS(KCLI_INJ_RETRY_MS), 0U);
    }
}

/**
 * @brief tap: press, hold, release - the sequence a finger actually makes.
 *
 * Publishes the press now and arms @c inj_tmo for the release once the hold
 * has run.
 */
static void h_tap(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    if ( KCLI_INJ_NONE != me->inj )
    {
        /* One gesture in flight at a time: a second `tap` would not know
         * whether its release belongs to it or to the first, and a `touch`
         * (see h_touch) would race the one already owed. */
        console_reply_err(KCLI_ERR_BUSY, "busy");
        return;
    }
    if ( !touch_publish((int16_t)v[0].u, (int16_t)v[1].u, true,
                       khal_time_ms()) )
    {
        console_reply_posted(c->verb, false);
        return;
    }
    KLOG_I(TAG, "console: injected tap x=%d y=%d", (int)v[0].u, (int)v[1].u);

    /* The release cannot go out in this same step: LVGL reads the last
     * sample once per pass, so a press and a release in one
     * run-to-completion is a press nobody ever saw. */
    me->inj_x       = (int16_t)v[0].u;
    me->inj_y       = (int16_t)v[1].u;
    me->inj_stalled = false;
    me->inj         = KCLI_INJ_TAP;
    QTimeEvt_armX(&me->inj_tmo, KQP_TICKS_PER_MS(KCLI_TAP_HOLD_MS), 0U);
    console_reply_posted(c->verb, true);
}

/**
 * @brief The coordinate a fraction of the way from one point to another.
 *
 * Rounded to the nearest whole pixel, a tie going away from @p from, so a
 * swipe and its mirror image land on mirrored pixels.
 *
 * @param from Where the fraction is 0.
 * @param to   Where the fraction is 1.
 * @param num  The numerator of the fraction.
 * @param den  Its denominator, above 0.
 * @return The coordinate.
 */
static int16_t swipe_lerp(int16_t from, int16_t to, int32_t num, int32_t den)
{
    int32_t const span   = (int32_t)to - (int32_t)from;
    int32_t const scaled = span * num;
    int32_t const half   = den / 2;
    int32_t       shift;

    if ( 0 <= scaled )
    {
        shift = (scaled + half) / den;
    }
    else
    {
        shift = -(((-scaled) + half) / den);
    }
    return (int16_t)((int32_t)from + shift);
}

/**
 * @brief One poll period of the swipe in flight.
 *
 * Publishes every move whose planned time has come (several if the pool was
 * dry), then, after the hold, the release. Move k is planned at
 * min(k * KODE_TOUCH_POLL_MS, ms) and stamped @c swipe.t0 plus that. The hold
 * publishes nothing, as AO_Touch does for a resting finger.
 *
 * @param me The console.
 */
static void inj_swipe_step(Console *me)
{
    kcli_swipe_t * const s = &me->swipe;
    uint32_t const ms    = (uint32_t)s->ms;
    uint32_t const moves = (ms + KODE_TOUCH_POLL_MS - 1U) / KODE_TOUCH_POLL_MS;

    ++s->step;
    uint32_t const due = s->step * KODE_TOUCH_POLL_MS;

    while ( (uint32_t)s->moved < moves )
    {
        uint32_t at = ((uint32_t)s->moved + 1U) * KODE_TOUCH_POLL_MS;

        if ( at > ms )
        {
            at = ms;
        }
        if ( at > due )
        {
            return;
        }
        int32_t const num = (int32_t)at;
        int32_t const den = (int32_t)ms;

        if ( !inj_send(me, swipe_lerp(s->x0, me->inj_x, num, den),
                      swipe_lerp(s->y0, me->inj_y, num, den), true,
                      s->t0 + at) )
        {
            return;
        }
        ++s->moved;
    }
    if ( (ms + (uint32_t)s->hold) <= due )
    {
        inj_release(me, s->t0 + ms + (uint32_t)s->hold);
    }
}

/**
 * @brief swipe: a finger that goes from one point to another, replayed at the
 *        touch sensor's rate.
 *
 * The press now, a move every KODE_TOUCH_POLL_MS from a timer, a rest of
 * @c hold ms, then the release, stamped with planned times (inj_swipe_step()).
 * Refused while any injected gesture is in flight.
 */
static void h_swipe(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    if ( KCLI_INJ_NONE != me->inj )
    {
        console_reply_err(KCLI_ERR_BUSY, "busy");
        return;
    }
    int16_t const x0 = (int16_t)v[0].u;
    int16_t const y0 = (int16_t)v[1].u;
    int16_t const x1 = (int16_t)v[2].u;
    int16_t const y1 = (int16_t)v[3].u;
    uint32_t hold = 0U;

    if ( v[5].present )
    {
        hold = v[5].u;
    }
    uint32_t const t0 = khal_time_ms();

    if ( !touch_publish(x0, y0, true, t0) )
    {
        console_reply_posted(c->verb, false);
        return;
    }
    KLOG_I(TAG, "console: injected swipe %d,%d -> %d,%d in %u ms, hold %u ms",
           (int)x0, (int)y0, (int)x1, (int)y1, (unsigned)v[4].u,
           (unsigned)hold);

    me->swipe.t0    = t0;
    me->swipe.step  = 0U;
    me->swipe.x0    = x0;
    me->swipe.y0    = y0;
    me->swipe.ms    = (uint16_t)v[4].u;
    me->swipe.hold  = (uint16_t)hold;
    me->swipe.moved = 0U;
    me->inj_x       = x1;
    me->inj_y       = y1;
    me->inj_stalled = false;
    me->inj         = KCLI_INJ_SWIPE;
    QTimeEvt_armX(&me->inj_tmo, KQP_TICKS_PER_MS(KODE_TOUCH_POLL_MS),
                  KQP_TICKS_PER_MS(KODE_TOUCH_POLL_MS));
    console_reply_posted(c->verb, true);
}

/**
 * @brief btn: one button changed, was held, or was pressed twice, as
 *        AO_Buttons would have said it.
 *
 * A press or a release goes through buttons_inject(), so a polling app sees
 * it too. A hold or a double is published here: an injected press never
 * starts AO_Buttons' hold clock or double detector, and neither changes a
 * button's state. Only the top and bottom buttons hold and only the top one
 * doubles; any other id is refused (`>err code=3 msg=id`).
 */
static void h_btn(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)me;
    /* The enum's order is kdrv_button_t's order, so the index IS the id. */
    uint8_t const id = (uint8_t)v[0].u;
    bool ok = false;

    if ( A_BTN_HOLD == v[1].u )
    {
        if ( ((uint8_t)KDRV_BTN_TOP != id) && ((uint8_t)KDRV_BTN_BOT != id) )
        {
            console_reply_err(KCLI_ERR_VALUE, "id");
            return;
        }
        ButtonHoldEvt *ev = Q_NEW_X(ButtonHoldEvt, 0U, BUTTON_HOLD_SIG);
        if ( NULL != ev )
        {
            ev->id = id;
            QACTIVE_PUBLISH(&ev->super, &me->super);
            ok = true;
        }
    }
    else if ( A_BTN_DOUBLE == v[1].u )
    {
        if ( (uint8_t)KDRV_BTN_TOP != id )
        {
            console_reply_err(KCLI_ERR_VALUE, "id");
            return;
        }
        ButtonDoubleEvt *ev = Q_NEW_X(ButtonDoubleEvt, 0U, BUTTON_DOUBLE_SIG);
        if ( NULL != ev )
        {
            ev->id   = id;
            ev->t_ms = khal_time_ms();
            QACTIVE_PUBLISH(&ev->super, &me->super);
            ok = true;
        }
    }
    else
    {
        ok = buttons_inject(id, A_BTN_PRESS == v[1].u);
    }
    if ( ok )
    {
        KLOG_I(TAG, "console: injected btn %s %s", A_BTN_IDS[v[0].u],
               A_BTN_STATES[v[1].u]);
    }
    console_reply_posted(c->verb, ok);
}

/**
 * @brief bat_info: closed by the next POWER_EVT_SIG - AO_Power publishes a
 *        fresh reading for the request (and one per poll anyway). No
 *        battery, no reading: the command times out, which is the honest
 *        answer. The register-level chip dump still arrives as '!' lines.
 */
static void h_bat_info(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)v;
    cmd_begin(me, KCLI_BAT, c->verb);
    power_dump_request();
}

/**
 * @brief ship_mode: open the battery path (BQ25896 ship mode).
 *
 * No terminator to wait for: AO_Power logs the outcome itself, and on battery
 * nothing would ever arrive here to answer anyway. The only door to ship
 * mode; the facade has none.
 */
static void h_ship_mode(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)me; (void)v;
    console_reply_posted(c->verb, power_ship_request());
}

#if (!CONFIG_KODE_LOWPOWER) || (!CONFIG_KODE_SCHEDULER)
/** @brief sleep, power_off and cron in a build without what they ask: refused, said so. */
static void h_not_built(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)me; (void)c; (void)v;
    console_reply_err(KCLI_ERR_BUSY, "not_built");
}
#endif

/* ---- pmic_help / gauge_help ----------------------------------------------
 * Everything but the description already comes from the driver's own
 * field_info() table; the one thing it does not carry is what a field is
 * *for*, so that lives here, one line per field, in enum order.
 */
static const char *const PMIC_FIELD_DESC[] = {
    [KDRV_BQ_EN_HIZ]        = "Input in high impedance (no VBUS draw).",
    [KDRV_BQ_EN_ILIM]       = "The ILIM pin clamps the input current.",
    [KDRV_BQ_IINLIM_MA]     = "Input current limit.",
    [KDRV_BQ_BHOT]          = "Boost hot threshold (3 = off).",
    [KDRV_BQ_BCOLD]         = "Boost cold threshold.",
    [KDRV_BQ_VINDPM_OS_MV]  = "Relative VINDPM offset.",
    [KDRV_BQ_CONV_START]    = "One-shot ADC conversion (self-clearing).",
    [KDRV_BQ_CONV_RATE]     = "Continuous 1 s ADC conversion.",
    [KDRV_BQ_BOOST_FREQ]    = "Boost frequency: 0 = 1.5 MHz, 1 = 500 kHz.",
    [KDRV_BQ_ICO_EN]        = "Input current optimiser.",
    [KDRV_BQ_FORCE_DPDM]    = "Force input detection (self-clearing).",
    [KDRV_BQ_AUTO_DPDM_EN]  = "Detect the input on plug-in.",
    [KDRV_BQ_BAT_LOADEN]    = "The 30 mA battery test load.",
    [KDRV_BQ_WD_RST]        = "Kick the I2C watchdog (self-clearing).",
    [KDRV_BQ_OTG_CONFIG]    = "Boost (OTG) enable.",
    [KDRV_BQ_CHG_CONFIG]    = "Charge enable.",
    [KDRV_BQ_SYS_MIN_MV]    = "Minimum system voltage.",
    [KDRV_BQ_MIN_VBAT_SEL]  = "Boost exit: 0 = 2.9 V, 1 = 2.5 V.",
    [KDRV_BQ_EN_PUMPX]      = "Current pulse control enable.",
    [KDRV_BQ_ICHG_MA]       = "Fast charge current.",
    [KDRV_BQ_IPRECHG_MA]    = "Precharge current.",
    [KDRV_BQ_ITERM_MA]      = "Termination current.",
    [KDRV_BQ_VREG_MV]       = "Charge voltage.",
    [KDRV_BQ_BATLOWV]       = "Pre to fast threshold: 0 = 2.8 V, 1 = 3.0 V.",
    [KDRV_BQ_VRECHG]        = "Recharge below VREG: 0 = 100 mV, 1 = 200 mV.",
    [KDRV_BQ_EN_TERM]       = "Charge termination.",
    [KDRV_BQ_STAT_DIS]      = "STAT pin off.",
    [KDRV_BQ_WATCHDOG]      = "I2C watchdog: 0 off, 1 40 s, 2 80 s, 3 160 s.",
    [KDRV_BQ_EN_TIMER]      = "Charge safety timer.",
    [KDRV_BQ_CHG_TIMER]     = "Safety timer: 0 5 h, 1 8 h, 2 12 h, 3 20 h.",
    [KDRV_BQ_JEITA_ISET]    = "Cool-range current: 0 = 50%, 1 = 20% of ICHG.",
    [KDRV_BQ_BAT_COMP_MOHM] = "IR compensation resistance.",
    [KDRV_BQ_VCLAMP_MV]     = "IR compensation clamp.",
    [KDRV_BQ_TREG_C]        = "Thermal regulation threshold.",
    [KDRV_BQ_FORCE_ICO]     = "Start ICO now (self-clearing).",
    [KDRV_BQ_TMR2X_EN]      = "Safety timer at half rate under DPM/thermal.",
    [KDRV_BQ_BATFET_DIS]    = "BATFET off: ship mode.",
    [KDRV_BQ_JEITA_VSET]    = "Warm-range voltage: 0 = VREG-200 mV, 1 = VREG.",
    [KDRV_BQ_BATFET_DLY]    = "Ship mode after ~10 s instead of at once.",
    [KDRV_BQ_BATFET_RST_EN] = "QON long press resets the system.",
    [KDRV_BQ_PUMPX_UP]      = "Pulse the adapter voltage up (self-clearing).",
    [KDRV_BQ_PUMPX_DN]      = "Pulse the adapter voltage down (self-clearing).",
    [KDRV_BQ_BOOSTV_MV]     = "Boost voltage.",
    [KDRV_BQ_PFM_OTG_DIS]   = "No PFM in boost mode.",
    [KDRV_BQ_BOOST_LIM]     = "Boost current limit.",
    [KDRV_BQ_VBUS_STAT]     = "What is on VBUS: none, USB host, adapter, or this chip's own boost.",
    [KDRV_BQ_CHRG_STAT]     = "Charge phase: none, pre-charge, fast charge, or done.",
    [KDRV_BQ_PG_STAT]       = "Input power good.",
    [KDRV_BQ_VSYS_STAT]     = "In minimum-system-voltage regulation.",
    [KDRV_BQ_FORCE_VINDPM]  = "Absolute VINDPM (else relative).",
    [KDRV_BQ_VINDPM_MV]     = "Input voltage limit.",
    [KDRV_BQ_THERM_STAT]    = "In thermal regulation.",
    [KDRV_BQ_BATV_MV]       = "Battery voltage, from the last ADC conversion.",
    [KDRV_BQ_SYSV_MV]       = "System rail voltage, from the last ADC conversion.",
    [KDRV_BQ_TS_PCT_X1000]  = "TS pin as a fraction of REGN.",
    [KDRV_BQ_VBUS_GD]       = "VBUS attached.",
    [KDRV_BQ_VBUSV_MV]      = "Input voltage, from the last ADC conversion.",
    [KDRV_BQ_ICHGR_MA]      = "Charge current, from the last ADC conversion.",
    [KDRV_BQ_VDPM_STAT]     = "In input voltage regulation.",
    [KDRV_BQ_IDPM_STAT]     = "In input current regulation.",
    [KDRV_BQ_IDPM_LIM_MA]   = "The input current optimiser's limit.",
    [KDRV_BQ_REG_RST]       = "Reset every register (self-clearing).",
    [KDRV_BQ_ICO_OPTIMIZED] = "ICO finished.",
    [KDRV_BQ_PN]            = "Part number code; 0 is the bq25896.",
    [KDRV_BQ_TS_PROFILE]    = "1 = JEITA.",
    [KDRV_BQ_DEV_REV]       = "Silicon revision.",
};

static const char *const GAUGE_FIELD_DESC[] = {
    [KDRV_CW_VERSION]        = "Chip identity; 0xA0 for any CW221x, asleep or awake.",
    [KDRV_CW_VCELL_MV]       = "The cell voltage.",
    [KDRV_CW_SOC_X256]       = "State of charge (above 100% means no profile loaded).",
    [KDRV_CW_TEMP_C2]        = "The TS pin's temperature.",
    [KDRV_CW_EN_SOC_INT]     = "SOC alarm enable.",
    [KDRV_CW_EN_TMX_INT]     = "Maximum temperature alarm enable.",
    [KDRV_CW_EN_TMN_INT]     = "Minimum temperature alarm enable.",
    [KDRV_CW_INT_FLAGS]      = "SOC, TMX, TMN pending; write 0 to clear.",
    [KDRV_CW_UPDATE_FLAG]    = "A profile was loaded since the last power-on.",
    [KDRV_CW_SOC_ALERT_PCT]  = "Alarm threshold; 101..127 means every 1% change.",
    [KDRV_CW_TEMP_MAX_C2]    = "Maximum temperature alarm threshold.",
    [KDRV_CW_TEMP_MIN_C2]    = "Minimum temperature alarm threshold.",
    [KDRV_CW_CURRENT_MA]     = "Current through the shunt, charging positive.",
    [KDRV_CW_T_HOST_C2]      = "Host-reported temperature, fed back into the algorithm.",
    [KDRV_CW_USER_CONF]      = "A scratch byte for whoever wants one.",
    [KDRV_CW_CYCLES_X16]     = "Charge cycles.",
    [KDRV_CW_SOH_PCT]        = "State of health.",
    [KDRV_CW_READY]          = "The algorithm is up and its registers mean something.",
    [KDRV_CW_AVE_CURRENT_MA] = "16 s mean of the current.",
    [KDRV_CW_FW_VERSION]     = "0x00 asleep; bits 7:6 = 01 for a CW2217.",
};

/* The expander's table says what each pin IS on this board, because the
 * chip's own names (i6, c7) say nothing: the datasheet has no idea a panel
 * hangs off P7. */
static const char *const IOEXP_FIELD_DESC[] = {
    [KDRV_TCA_INPUT]    = "All eight pins as they read; bit n is Pn.",
    [KDRV_TCA_OUTPUT]   = "What pins configured as outputs drive; reset 0xFF.",
    [KDRV_TCA_POLARITY] = "A 1 inverts that pin's reading; 0 on this board.",
    [KDRV_TCA_CONFIG]   = "Direction, 1 = input: 0x7F on this board (P7 out).",
    [KDRV_TCA_I0] = "P0 reads: BUTTON_BOT, low = pressed. Also the PMIC's QON.",
    [KDRV_TCA_I1] = "P1 reads: PAD_TOP, low = pressed.",
    [KDRV_TCA_I2] = "P2 reads: PAD_LEFT, low = pressed.",
    [KDRV_TCA_I3] = "P3 reads: PAD_BOTTOM, low = pressed.",
    [KDRV_TCA_I4] = "P4 reads: PAD_RIGHT, low = pressed.",
    [KDRV_TCA_I5] = "P5 reads: the IMU's INT1, driven by the QMI8658A.",
    [KDRV_TCA_I6] = "P6 reads: the touch controller's IRQ, low = report ready.",
    [KDRV_TCA_I7] = "P7 reads back VCI_EN, the panel's supply enable.",
    [KDRV_TCA_O0] = "What P0 would drive as an output; it is an input here.",
    [KDRV_TCA_O1] = "What P1 would drive as an output; it is an input here.",
    [KDRV_TCA_O2] = "What P2 would drive as an output; it is an input here.",
    [KDRV_TCA_O3] = "What P3 would drive as an output; it is an input here.",
    [KDRV_TCA_O4] = "What P4 would drive as an output; it is an input here.",
    [KDRV_TCA_O5] = "What P5 would drive as an output; it is an input here.",
    [KDRV_TCA_O6] = "What P6 would drive as an output; it is an input here.",
    [KDRV_TCA_O7] = "VCI_EN, the panel's supply: 1 = on. AO_Display's pin.",
    [KDRV_TCA_N0] = "Invert P0's reading.",
    [KDRV_TCA_N1] = "Invert P1's reading.",
    [KDRV_TCA_N2] = "Invert P2's reading.",
    [KDRV_TCA_N3] = "Invert P3's reading.",
    [KDRV_TCA_N4] = "Invert P4's reading.",
    [KDRV_TCA_N5] = "Invert P5's reading.",
    [KDRV_TCA_N6] = "Invert P6's reading.",
    [KDRV_TCA_N7] = "Invert P7's reading.",
    [KDRV_TCA_C0] = "P0 direction, 1 = input; a button, so it stays 1.",
    [KDRV_TCA_C1] = "P1 direction, 1 = input; a button, so it stays 1.",
    [KDRV_TCA_C2] = "P2 direction, 1 = input; a button, so it stays 1.",
    [KDRV_TCA_C3] = "P3 direction, 1 = input; a button, so it stays 1.",
    [KDRV_TCA_C4] = "P4 direction, 1 = input; a button, so it stays 1.",
    [KDRV_TCA_C5] = "P5 direction, 1 = input; an interrupt, so it stays 1.",
    [KDRV_TCA_C6] = "P6 direction, 1 = input; the touch IRQ, so it stays 1.",
    [KDRV_TCA_C7] = "P7 direction, 0 = output; the panel's supply needs 0.",
};

/*
 * The three tables above are unsized, so the compiler counts their entries. A
 * table short of its enum would leave a NULL that field_help_slice() passes to
 * %s; these make that a build failure. Keep the tables unsized.
 */
#define KCLI_DESC_COUNT(arr) (sizeof(arr) / sizeof((arr)[0]))

_Static_assert(KCLI_DESC_COUNT(PMIC_FIELD_DESC) == (size_t)KDRV_BQ_FIELD_MAX,
               "PMIC_FIELD_DESC is missing an entry for a bq25896 field");
_Static_assert(KCLI_DESC_COUNT(GAUGE_FIELD_DESC) == (size_t)KDRV_CW_FIELD_MAX,
               "GAUGE_FIELD_DESC is missing an entry for a cw2217 field");
_Static_assert(KCLI_DESC_COUNT(IOEXP_FIELD_DESC) == (size_t)KDRV_TCA_FIELD_MAX,
               "IOEXP_FIELD_DESC is missing an entry for a tca6408a field");

#undef KCLI_DESC_COUNT

/* ---- Field tables ----------------------------------------------------------
 * Every chip with a field table is a console_chip_t: one resolver, one row
 * describer and one request to its owner. The core's four are here; an
 * owner's verbs file carries its own and calls the same three doors.
 */

/** @brief Resolve a PMIC field name. */
static kode_status_t pmic_field_by_name(const char *name, uint16_t *field)
{
    kdrv_bq25896_field_t f = KDRV_BQ_FIELD_MAX;
    kode_status_t const rc = kdrv_bq25896_field_by_name(name, &f);

    *field = (uint16_t)f;
    return rc;
}

/** @brief Describe a PMIC field. */
static bool pmic_field_row(uint16_t field, console_field_t *out)
{
    const kdrv_bq25896_field_info_t * const i =
        kdrv_bq25896_field_info((kdrv_bq25896_field_t)field);
    bool const ok = (NULL != i);

    if ( ok )
    {
        out->name = i->name; out->unit = i->unit;
        out->min = i->min;   out->max = i->max;
        out->writable = i->writable;
        out->desc = PMIC_FIELD_DESC[field];
    }
    return ok;
}

/** @brief Ask AO_Power about a PMIC field. */
static void pmic_field_ask(uint8_t op, uint16_t field, int32_t value)
{
    power_field_request(POWER_CHIP_PMIC, op, field, value);
}

/** @brief Resolve a gauge field name. */
static kode_status_t gauge_field_by_name(const char *name, uint16_t *field)
{
    kdrv_cw2217_field_t f = KDRV_CW_FIELD_MAX;
    kode_status_t const rc = kdrv_cw2217_field_by_name(name, &f);

    *field = (uint16_t)f;
    return rc;
}

/** @brief Describe a gauge field. */
static bool gauge_field_row(uint16_t field, console_field_t *out)
{
    const kdrv_cw2217_field_info_t * const i =
        kdrv_cw2217_field_info((kdrv_cw2217_field_t)field);
    bool const ok = (NULL != i);

    if ( ok )
    {
        out->name = i->name; out->unit = i->unit;
        out->min = i->min;   out->max = i->max;
        out->writable = i->writable;
        out->desc = GAUGE_FIELD_DESC[field];
    }
    return ok;
}

/** @brief Ask AO_Power about a gauge field. */
static void gauge_field_ask(uint8_t op, uint16_t field, int32_t value)
{
    power_field_request(POWER_CHIP_GAUGE, op, field, value);
}

/** @brief Resolve an expander field name. */
static kode_status_t ioexp_field_by_name(const char *name, uint16_t *field)
{
    kdrv_tca6408a_field_t f = KDRV_TCA_FIELD_MAX;
    kode_status_t const rc = kdrv_tca6408a_field_by_name(name, &f);

    *field = (uint16_t)f;
    return rc;
}

/** @brief Describe an expander field. */
static bool ioexp_field_row(uint16_t field, console_field_t *out)
{
    const kdrv_tca6408a_field_info_t * const i =
        kdrv_tca6408a_field_info((kdrv_tca6408a_field_t)field);
    bool const ok = (NULL != i);

    if ( ok )
    {
        out->name = i->name; out->unit = i->unit;
        out->min = i->min;   out->max = i->max;
        out->writable = i->writable;
        out->desc = IOEXP_FIELD_DESC[field];
    }
    return ok;
}

/** @brief Ask AO_IoExp about a field; it answers this AO alone. */
static void ioexp_field_ask(uint8_t op, uint16_t field, int32_t value)
{
    ioexp_field_request(AO_Console, op, field, value);
}

static const console_chip_t PMIC_CHIP = {
    .chip = "pmic", .count = (uint16_t)KDRV_BQ_FIELD_MAX,
    .by_name = pmic_field_by_name, .row = pmic_field_row, .ask = pmic_field_ask,
};
static const console_chip_t GAUGE_CHIP = {
    .chip = "gauge", .count = (uint16_t)KDRV_CW_FIELD_MAX,
    .by_name = gauge_field_by_name, .row = gauge_field_row, .ask = gauge_field_ask,
};
static const console_chip_t IOEXP_CHIP = {
    .chip = "ioexp", .count = (uint16_t)KDRV_TCA_FIELD_MAX,
    .by_name = ioexp_field_by_name, .row = ioexp_field_row, .ask = ioexp_field_ask,
};

/**
 * @brief Emit one slice of a chip's whole field table: name, unit, range,
 *        whether it can be written, and what it is for.
 *
 * Same pacing as help_slice() and for the same reason: the PMIC alone is
 * KDRV_BQ_FIELD_MAX fields, well past one KCLI_SLICE.
 *
 * @param me The console; @c me->help_chip is the table, @c me->help_idx
 *           where the last slice stopped.
 * @return true when the table is exhausted.
 */
static bool field_help_slice(Console *me)
{
    const console_chip_t * const chip = me->help_chip;
    uint16_t const total = chip->count;
    uint16_t       end   = total;

    if ( (uint16_t)(me->help_idx + KCLI_SLICE) < total )
    {
        end = (uint16_t)(me->help_idx + KCLI_SLICE);
    }
    for ( uint16_t i = me->help_idx; i < end; ++i )
    {
        console_field_t f;

        if ( chip->row(i, &f) )
        {
            const char *rw = "ro";
            char qn[CONSOLE_QUOTED_MAX(KCLI_FIELD_NAME_MAX)];
            char qd[CONSOLE_QUOTED_MAX(KCLI_FIELD_DESC_MAX)];

            if ( f.writable )
            {
                rw = "rw";
            }
            console_reply(">|field=%s unit=%s min=%" PRId32 " max=%" PRId32 " rw=%s desc=%s",
                  console_quote(qn, sizeof(qn), f.name), f.unit, f.min,
                  f.max, rw, console_quote(qd, sizeof(qd), f.desc));
        }
    }
    me->help_idx = end;
    return (end >= total);
}

/** @brief `<chip>_help`: the table in slices; see field_help_slice(). */
static void field_help(Console *me, const console_chip_t *chip, const char *verb)
{
    me->help_idx  = 0U;
    me->help_chip = chip;
    cmd_begin(me, KCLI_FIELD_HELP, verb);
    if ( field_help_slice(me) )
    {
        console_reply(">ok");
        console_end();
    }
    else
    {
        QTimeEvt_armX(&me->pacer, KCLI_SLICE_GAP_TICKS, 0U);
    }
}

/**
 * @brief pmic, gauge, ioexp, sd and every owner's field verb: one field of
 *        one chip by its name, or the whole chip.
 *
 * No keys: the owner logs the whole chip. `field=` alone reads; with
 * `value=` it writes, and the record carries what the chip kept (500 mA of
 * charge current comes back as 512). The name is resolved here, so a
 * misspelling is refused before the owner is asked. `value` is unsigned.
 */
static void field_cmd(Console *me, const console_chip_t *chip, const char *verb,
                      const karg_val_t *v)
{
    uint16_t field = 0U;
    uint8_t  op    = FIELD_OP_DUMP;
    int32_t  value = 0;

    if ( v[0].present )
    {
        if ( chip->by_name(v[0].s, &field) != KODE_OK )
        {
            console_reply_err(KCLI_ERR_VALUE, "field");
            return;
        }
        op = FIELD_OP_GET;
        if ( v[1].present )
        {
            op    = FIELD_OP_SET;
            value = (int32_t)v[1].u;
        }
    }
    else if ( v[1].present )
    {
        console_reply_err(KCLI_ERR_MISSING, "field");
        return;
    }
    else
    {
        /* No keys at all: the whole chip. */
    }
    cmd_begin(me, KCLI_FIELD, verb);
    me->field_chip = chip;
    chip->ask(op, field, value);
}

/**
 * @brief Close a field verb with the owner's answer.
 *
 * KODE_DENIED on a read or a write is the driver's refusal - a value the
 * field cannot hold, or a field that is not written - and is named after the
 * field. Anything else is the chip not answering, not keeping the write or
 * not being on this board.
 */
static void field_close(const console_chip_t *chip, uint8_t op, uint16_t field,
                        uint8_t rc, int32_t value)
{
    console_field_t f;
    const char *name = "";

    if ( chip->row(field, &f) )
    {
        name = f.name;
    }
    if ( (uint8_t)KODE_OK == rc )
    {
        if ( FIELD_OP_DUMP != op )
        {
            field_record(chip->chip, name, value);
        }
        console_reply(">ok");
    }
    else if ( ((uint8_t)KODE_DENIED == rc) && (FIELD_OP_DUMP != op) )
    {
        console_reply_err(KCLI_ERR_VALUE, name);
    }
    else
    {
        console_reply_err(KCLI_ERR_REJECT, console_verb());
    }
    console_end();
}

void console_field_cmd(const console_chip_t *chip, const char *verb,
                       const karg_val_t *v)
{
    field_cmd(&l_console, chip, verb, v);
}

void console_field_help(const console_chip_t *chip, const char *verb)
{
    field_help(&l_console, chip, verb);
}

void console_field_answer(const console_chip_t *chip, uint8_t op, uint16_t field,
                          uint8_t rc, int32_t value)
{
    const Console * const me = &l_console;

    /* Gated: an app may ask a chip while another chip's verb waits, and
     * only the right chip's answer closes it. */
    if ( ((uint8_t)KCLI_FIELD == me->inflight) && (me->field_chip == chip) )
    {
        field_close(chip, op, field, rc, value);
    }
}

/** @brief pmic [field=] [value=]: one PMIC field, or the whole chip; see field_cmd(). */
static void h_pmic(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    field_cmd(me, &PMIC_CHIP, c->verb, v);
}

/** @brief gauge [field=] [value=]: one gauge field, or the whole chip; see field_cmd(). */
static void h_gauge(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    field_cmd(me, &GAUGE_CHIP, c->verb, v);
}

/** @brief ioexp [field=] [value=]: one expander field, or the whole chip; see field_cmd(). */
static void h_ioexp(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    field_cmd(me, &IOEXP_CHIP, c->verb, v);
}

/** @brief pmic_help: the PMIC's field table, in slices; see field_help_slice(). */
static void h_pmic_help(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)v;
    field_help(me, &PMIC_CHIP, c->verb);
}

/** @brief gauge_help: the gauge's field table, in slices; see field_help_slice(). */
static void h_gauge_help(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)v;
    field_help(me, &GAUGE_CHIP, c->verb);
}

/** @brief Steps in the IMU row. A constant rather than a field of the event:
 *         the row cannot end early, so every record knows how many there
 *         are, and `of=` on the wire says so. */
#define KCLI_IMU_STEPS            3U

/**
 * @brief What one MEASURED haptic record's flags amount to, as the wire
 *        spells it.
 *
 * AO_Haptic publishes a measurement it could not judge BEFORE the noverdict
 * record that says why, so only a record measured and found wanting is
 * `fail`. Order matters: HAPTIC_TF_LOUD first, as a bench already carrying
 * 235 Hz is the finding.
 *
 * @param flags HAPTIC_TF_*.
 * @return pass, loud, short, mute or fail.
 */
static const char *haptic_result(uint8_t flags)
{
    if ( (flags & HAPTIC_TF_PASS) != 0U )
    {
        return "pass";
    }
    if ( (flags & HAPTIC_TF_LOUD) != 0U )
    {
        return "loud";
    }
    if ( (flags & HAPTIC_TF_SHORT) != 0U )
    {
        return "short";
    }
    if ( (flags & HAPTIC_TF_MUTE) != 0U )
    {
        return "mute";
    }
    return "fail";
}

/**
 * @brief Which field a display record is about, as the wire spells it.
 *
 * @param field DISPLAY_FIELD_*.
 * @return white, red, green, blue or black.
 */
static const char *display_field_name(uint8_t field)
{
    if ( (uint8_t)DISPLAY_FIELD_RED == field )
    {
        return "red";
    }
    if ( (uint8_t)DISPLAY_FIELD_GREEN == field )
    {
        return "green";
    }
    if ( (uint8_t)DISPLAY_FIELD_BLUE == field )
    {
        return "blue";
    }
    if ( (uint8_t)DISPLAY_FIELD_BLACK == field )
    {
        return "black";
    }
    return "white";
}

/**
 * @brief Which colour an LED record is about, as the wire spells it.
 *
 * @param colour LED_COLOUR_*.
 * @return red, green or blue.
 */
static const char *led_colour_name(uint8_t colour)
{
    if ( (uint8_t)LED_COLOUR_GREEN == colour )
    {
        return "green";
    }
    if ( (uint8_t)LED_COLOUR_BLUE == colour )
    {
        return "blue";
    }
    return "red";
}

/** @brief ioexp_help: the expander's field table, in slices; see field_help_slice(). */
static void h_ioexp_help(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)v;
    field_help(me, &IOEXP_CHIP, c->verb);
}

/* ---- Handlers: the owner's operations on the PMIC ------------------------
 * Four verbs, one event pair. Each is a query: the owner answers when it
 * knows, which for `boost` and `pmic_reset` is after a wait on its timer.
 */

/** @brief One control request, closed by POWER_CTRL_SIG. */
static void ctrl_cmd(Console *me, const kcmd_t *c, uint8_t op, int32_t value)
{
    cmd_begin(me, KCLI_CTRL, c->verb);
    me->ctrl_op = op;
    power_ctrl_request(op, value);
}

/** @brief boost state=on|off: the PMIC's boost, closed by POWER_CTRL_SIG. */
static void h_boost(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    int32_t on = 0;
    if ( v[0].b )
    {
        on = 1;
    }
    ctrl_cmd(me, c, POWER_CTRL_BOOST, on);
}

/** @brief pmic_reset: REG_RST on the PMIC, closed by POWER_CTRL_SIG. */
static void h_pmic_reset(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)v;
    ctrl_cmd(me, c, POWER_CTRL_RESET, 0);
}

/**
 * @brief Which phase of a drag a `>|gesture` record is about, as the wire
 *        spells it.
 *
 * @param drag KODE_GUI_DRAG_*.
 * @return idle, pending, follow or settle; ? for any other value.
 */
static const char *gui_drag_name(uint8_t drag)
{
    const char *name = "?";
    if ( (uint8_t)KODE_GUI_DRAG_IDLE == drag )
    {
        name = "idle";
    }
    else if ( (uint8_t)KODE_GUI_DRAG_PENDING == drag )
    {
        name = "pending";
    }
    else if ( (uint8_t)KODE_GUI_DRAG_FOLLOW == drag )
    {
        name = "follow";
    }
    else if ( (uint8_t)KODE_GUI_DRAG_SETTLE == drag )
    {
        name = "settle";
    }
    else
    {
        /* Any other value keeps the ? */
    }
    return name;
}

/**
 * @brief gui_info: closed by GUI_STATE_SIG.
 *
 * An AO_Gui without LVGL still answers and says so; one blocked on the panel
 * does not, and the command times out with code 6. Published, not posted:
 * AO_Gui lives in kgui, a layer above. Without a GUI it times out too.
 */
static void h_gui_info(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    static const QEvt req = QEVT_INITIALIZER(GUI_DUMP_REQ_SIG);
    (void)v;
    cmd_begin(me, KCLI_GUI, c->verb);
    QACTIVE_PUBLISH(&req, &me->super);
}

/**
 * @brief restart: the deliberate restart every firmware leaves by.
 *
 * The same request an app's way home posts: AO_Boot leaves (the channel, the
 * C5 parked) and resets. Answered first: nothing comes after the reset. It is
 * how a bench sees a warm boot, which opening the port never is.
 */
static void h_restart(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)c; (void)v;
    if ( !console_src_allows(CONSOLE_NEED_CABLE) )
    {
        return;
    }
    console_reply(">ok");
    QACTIVE_POST(AO_Boot, boot_exit_evt(), &me->super);
}

/** @brief boot_info: closed by BOOT_STATE_SIG. */
static void h_boot_info(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)v;
    cmd_begin(me, KCLI_BOOT, c->verb);
    boot_info_request();
}

/* ---- Handlers: the store -------------------------------------------------
 * The provisioning store: an operator sets a key and a value; apps read them
 * with kode_sys_nvs_get() (KODE_NVS_NAMESPACE, kqp_config.h). Synchronous: one
 * key is one NVS record written whole, so no owner AO is needed. Values are
 * not secret: `nvs_get` prints them on any port; they sit in plain flash.
 */

/**
 * @brief Refuse a key the store cannot hold, naming it.
 *
 * The ceiling is NVS's own. Checked here as well as in the facade, so a
 * mistyped line gets `>err msg=key`.
 *
 * @param key The value of the line's `key=`.
 * @return true when the command may go on; it has already answered otherwise.
 */
static bool nvs_key_ok(const char *key)
{
    if ( strlen(key) >= KODE_SYS_NVS_KEY_MAX )
    {
        console_reply_err(KCLI_ERR_VALUE, "key");
        return false;
    }
    return true;
}

/** @brief nvs_set key=<k> value=<v>: remember one string under one name. */
static void h_nvs_set(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)me;

    if ( !nvs_key_ok(v[0].s) )
    {
        return;
    }
    /* A console line cannot carry a value this long today - KODE_CONSOLE_LINE_MAX
     * stops it first - and the check is here anyway, so the store's own
     * ceiling holds if the line ever grows. */
    if ( strlen(v[1].s) >= KODE_SYS_NVS_VALUE_MAX )
    {
        console_reply_err(KCLI_ERR_VALUE, "value");
        return;
    }
    if ( khal_nvs_set_str(KODE_NVS_NAMESPACE, v[0].s, v[1].s) != KODE_OK )
    {
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        return;
    }
    console_reply(">ok");
}

/**
 * @brief nvs_get key=<k>: what is stored, or that nothing is.
 *
 * An absent key answers `state=unset` and `>ok`, not an error. Key and value
 * are quoted, so they can be typed straight back into `nvs_set`.
 */
static void h_nvs_get(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    char val[KODE_SYS_NVS_VALUE_MAX];
    char qk[CONSOLE_QUOTED_MAX(KODE_SYS_NVS_KEY_MAX)];
    /* What is left of the line, not CONSOLE_QUOTED_MAX(KODE_SYS_NVS_VALUE_MAX),
     * which is wider than a line. console_kv_fit() says when it cuts. */
    char qv[KODE_LOG_LINE_MAX - sizeof(">|key= value=")
            - CONSOLE_QUOTED_MAX(KODE_SYS_NVS_KEY_MAX)];

    (void)me;

    if ( !nvs_key_ok(v[0].s) )
    {
        return;
    }

    kode_status_t const rc = khal_nvs_get_str(KODE_NVS_NAMESPACE, v[0].s,
                                              val, sizeof(val));
    if ( KODE_DENIED == rc )
    {
        console_reply(">|key=%s state=unset", console_quote(qk, sizeof(qk), v[0].s));
    }
    else if ( KODE_OK != rc )
    {
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        return;
    }
    else
    {
        console_reply(">|key=%s value=%s", console_quote(qk, sizeof(qk), v[0].s),
              console_kv_fit(qv, sizeof(qv), val, "value"));
    }
    console_reply(">ok");
}

/** @brief nvs_del key=<k>: forget it. Missing is success. */
static void h_nvs_del(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)me;

    if ( !nvs_key_ok(v[0].s) )
    {
        return;
    }
    if ( khal_nvs_erase(KODE_NVS_NAMESPACE, v[0].s) != KODE_OK )
    {
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        return;
    }
    console_reply(">ok");
}

/**
 * @brief nvs_list: the names in the store, one record each.
 *
 * Names only, and no order: khal_nvs_key() promises none, so a host that
 * wants them sorted sorts them.
 */
static void h_nvs_list(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    char     key[KODE_SYS_NVS_KEY_MAX];
    char     qk[CONSOLE_QUOTED_MAX(KODE_SYS_NVS_KEY_MAX)];
    uint16_t idx = 0U;

    (void)me; (void)c; (void)v;

    while ( (KODE_NVS_LIST_MAX > idx)
           && (khal_nvs_key(KODE_NVS_NAMESPACE, idx, key, sizeof(key))
               == KODE_OK) )
    {
        console_reply(">|key=%s", console_quote(qk, sizeof(qk), key));
        ++idx;
    }

    /* Probed rather than inferred from the count: stopping exactly at the
     * ceiling with nothing left is a complete listing, not a capped one. */
    if ( (KODE_NVS_LIST_MAX == idx)
        && (khal_nvs_key(KODE_NVS_NAMESPACE, idx, key, sizeof(key))
            == KODE_OK) )
    {
        KLOG_W(TAG, "store listing stopped at %u keys (the ceiling)",
               (unsigned)KODE_NVS_LIST_MAX);
    }
    console_reply(">ok");
}

/* ---- Handlers: log ------------------------------------------------------- */

/** @brief The `level=` enum, in kode_log_lvl_t order - the index IS the level. */
/* cppcheck-suppress misra-c2012-8.9 ; also the `level=` argument table's enum, a file-scope initializer, so one function is not its only user */
static const char *const EN_LEVEL[] = KODE_LOG_LEVEL_WORDS;

/**
 * @brief log_level tag=<tag> level=d|i|n|w|e|off: one tag's level, an accept.
 *
 * Not stored. The global level is a setting, `config key="log"`, so `*` and
 * `all` are refused. AO_Log owns the level, so the console asks it rather
 * than calling klog; it is in force before the next line AO_Log writes.
 */
static void h_log_level(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)me; (void)c;
    const char *tag = v[0].s;

    if ( (strcmp(tag, "*") == 0) || (strcmp(tag, "all") == 0) )
    {
        console_reply_err(KCLI_ERR_VALUE, "tag");
        return;
    }
    log_level_request(tag, (kode_log_lvl_t)v[1].u);
    console_reply(">ok");
}

/** @brief log_color state=on|off: ANSI colour on the '!' lines. */
static void h_log_color(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)me; (void)c;
    kode_log_set_color(v[0].b);
    console_reply(">ok");
}

/* ---- Handlers: ps / help (synchronous but multi-record) ------------------ */

/* The AO roster, in priority order, by hand: QP's registry holds no names
 * or ownership. A row prints only for an AO that started (kqp_prio_taken());
 * the ownership table below names an unlinked owner as parked. */
static const struct {
    const char *name;
    uint8_t     prio;
    const char *owns;
} AOS[] = {
    { "Net",     KODE_PRIO_NET,     "ping/HTTP/UDP/MQTT (no hardware)" },
#if CONFIG_KODEOS_GUI
    /* kgui's, not ours, but running in a build with a GUI. */
    { "Gui",     KODE_PRIO_USER_GUI, "LVGL (no hardware)"           },
#endif
    { "Radio",   KODE_PRIO_RADIO,   "the ESP32-C5 (esp-hosted link)" },
    { "Storage", KODE_PRIO_STORAGE, "the microSD and the FAT partition" },
    { "Log",     KODE_PRIO_LOG,     "the console device"            },
    { "LedMgr",  KODE_PRIO_LED,     "the KTD2026 RGB"               },
    { "Power",   KODE_PRIO_POWER,   "the BQ25896 and the CW2217"    },
    { "AppMgr",  KODE_PRIO_APPMGR,  "the apps region"               },
    { "Autotest", KODE_PRIO_AUTOTEST, "the self-test rows (no hardware)" },
    { "Console", KODE_PRIO_CONSOLE, "command parsing (no hardware)" },
    { "Boot",    KODE_PRIO_BOOT,    "otadata"                       },
    { "Buttons", KODE_PRIO_BUTTONS, "GPIO35, plus the expander's bits" },
    { "Touch",   KODE_PRIO_TOUCH,   "the CST820, polled while pressed" },
    { "Display", KODE_PRIO_DISPLAY, "the CO5300"                    },
    { "IoExp",   KODE_PRIO_IOEXP,   "the TCA6408A and its INT (GPIO6)" },
    { "Ir",      KODE_PRIO_IR,      "the TSOP75338 and both SFH4045N" },
    { "Nfc",     KODE_PRIO_NFC,     "the ST25R3916B and its SPI"    },
    { "Audio",   KODE_PRIO_AUDIO,   "the ES8311, the speaker and the microphone" },
    { "Imu",     KODE_PRIO_IMU,     "the QMI8658A, its FIFO and the QMC5883P" },
    { "Haptic",  KODE_PRIO_HAPTIC,  "the AW86233 and the LRA"       },
    { "Rfid",    KODE_PRIO_RFID,    "the 125 kHz LF front end"      },
};

/* The roster rows that are not Active Objects, counted so the pacing cursor
 * can index them: the console's blocking reader alone. The workers have rows
 * of their own (ps_worker_record()). Kept beside the row itself in
 * ps_task_record(), which is the only other place that knows the count. */
#define KCLI_PS_TASKS  1U

/** @brief Tenths of a degree in one degree, for printing a khal_temp reading. */
#define TEMP_DECI_PER_C  10U

/**
 * @brief The die temperature as one record: `>|temp=p4 c=41.5`, or `c=-`.
 * @return The status khal_temp_read() answered.
 */
static kode_status_t reply_temp(void)
{
    int32_t deci = 0;
    kode_status_t const st = khal_temp_read(&deci);

    if ( KODE_OK != st )
    {
        console_reply(">|temp=p4 c=-");
        return st;
    }
    const char *sign = "";
    uint32_t mag = (uint32_t)deci;
    if ( 0 > deci )
    {
        sign = "-";
        mag  = (uint32_t)(-deci);
    }
    console_reply(">|temp=p4 c=%s%lu.%lu", sign, (unsigned long)(mag / TEMP_DECI_PER_C),
          (unsigned long)(mag % TEMP_DECI_PER_C));
    return KODE_OK;
}

/** @brief Bytes of a MAC as `info` prints it, "xx:xx:xx:xx:xx:xx" and its NUL. */
#define INFO_MAC_TEXT  18U

/**
 * @brief Write a MAC as `info` prints it, or "-" for one that is all zero.
 * @param mac KHAL_ID_MAC_LEN bytes, most significant first.
 * @param out Where the text goes.
 * @param cap Bytes of @p out, at least INFO_MAC_TEXT.
 */
static void info_mac_text(const uint8_t mac[KHAL_ID_MAC_LEN], char *out, size_t cap)
{
    bool any = false;

    for ( size_t i = 0U; i < (size_t)KHAL_ID_MAC_LEN; i++ )
    {
        if ( 0U != mac[i] )
        {
            any = true;
        }
    }
    if ( any )
    {
        (void)snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    else
    {
        (void)snprintf(out, cap, "-");
    }
}

/**
 * @brief info: what this board is.
 *
 * Synchronous: the identity is decoded at startup, so no owner is asked.
 * `mac_p4` is the P4's own MAC, the one its USB port gives as its serial
 * number; `mac_c5` is the C5's, which the serial encodes. A board with no
 * identity is not a refusal: its record carries `-` placeholders and closes
 * with `>ok`.
 */
static void h_info(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)me; (void)c; (void)v;

    uint8_t          mac[KHAL_ID_MAC_LEN];
    khal_chip_info_t chip;
    char             p4[INFO_MAC_TEXT];
    char             c5[INFO_MAC_TEXT];

    /* A board with no identity leaves the C5's MAC at zero, which prints as "-". */
    (void)memset(mac, 0, sizeof(mac));
    bool const known = (khal_identity_mac(mac) == KODE_OK);

    /* Only the flash size can fail; khal_chip_info() fills the MAC first. */
    (void)memset(&chip, 0, sizeof(chip));
    (void)khal_chip_info(&chip);
    info_mac_text(chip.mac, p4, sizeof(p4));
    info_mac_text(mac, c5, sizeof(c5));

    /* The product is a firmware fact, so it answers even when the silicon
     * would not: a board that cannot read its own MAC still knows what it is. */
    console_reply(">|serial=%s product=\"%s\" board=\"%s\"",
          known ? khal_identity_serial() : "-", KBSP_ID_PRODUCT,
          KBSP_BOARD_REV);
    console_reply(">|mac_p4=%s mac_c5=%s", p4, c5);

    /* Then what is programmed where: a descriptor is one 256-byte flash read,
     * so this answers in every state. An empty partition is an answer. */
    static const char *const SLOT_NAMES[KDRV_SLOT_ID_COUNT] = {
        "factory", "ota_0", "ota_1",
    };
    for ( unsigned i = 0U; (unsigned)KDRV_SLOT_ID_COUNT > i; ++i )
    {
        kdrv_slot_info_t info;
        if ( kdrv_slot_info((kdrv_slot_id_t)i, &info) != KODE_OK )
        {
            console_reply(">|slot=%s addr=- size_kb=- state=absent project=- "
                  "version=- idf=-", SLOT_NAMES[i]);
            continue;
        }
        const char *const state = info.running ? "running"
                                : info.has_image ? "inactive" : "empty";
        char qp[CONSOLE_QUOTED_MAX(sizeof(info.desc.project))];
        char qv[CONSOLE_QUOTED_MAX(sizeof(info.desc.version))];
        char qi[CONSOLE_QUOTED_MAX(sizeof(info.desc.idf_ver))];
        const char *project = "-";
        const char *version = "-";
        const char *idf     = "-";
        if ( info.has_image )
        {
            project = console_kv_text(qp, sizeof(qp), info.desc.project);
            version = console_kv_text(qv, sizeof(qv), info.desc.version);
            idf     = console_kv_text(qi, sizeof(qi), info.desc.idf_ver);
        }
        console_reply(">|slot=%s addr=0x%06lx size_kb=%lu state=%s project=%s "
              "version=%s idf=%s",
              info.label, (unsigned long)info.address,
              (unsigned long)(info.size / 1024U), state,
              project, version, idf);
    }

    /* Whether a crash waits in the coredump partition, and whose. Omitted
     * for KODE_DENIED: the build has no coredump to flash (every SDK app). */
    kdrv_coredump_t cd;
    if ( kdrv_slot_coredump(&cd) == KODE_OK )
    {
        if ( cd.present )
        {
            const char *owner = "other";
            if ( cd.mine )
            {
                owner = "kodeos";
            }
            char qt[CONSOLE_QUOTED_MAX(sizeof(cd.task))];
            const char * const task = console_kv_text(qt, sizeof(qt), cd.task);
            if ( 0U == cd.pc )
            {
                console_reply(">|coredump=present owner=%s task=%s pc=-", owner, task);
            }
            else
            {
                console_reply(">|coredump=present owner=%s task=%s pc=0x%08lx",
                      owner, task, (unsigned long)cd.pc);
            }
        }
        else
        {
            console_reply(">|coredump=none");
        }
    }

    /* What the owners add, in registration order: the coprocessor's
     * snapshot, when AO_Radio is linked. */
    for ( size_t t = 0U; KVERBS_TABLES_MAX > t; ++t )
    {
        const kverbs_t * const owner = kverbs_table(t);

        if ( (NULL != owner) && (NULL != owner->info) )
        {
            owner->info();
        }
    }
    /* The console's own level, so a host that sees nothing can ask why.
     * `stored` is a level that differs from the build's: AO_Log keeps the
     * key only then, so the comparison is the whole answer. */
    {
        kode_log_lvl_t const lvl = kode_log_get_level();
        const char *source = "default";

        if ( kode_log_build_level() != lvl )
        {
            source = "stored";
        }
        console_reply(">|log=%s source=%s", EN_LEVEL[lvl], source);
    }
    /* The die temperature; `c=-` when the sensor does not answer. */
    (void)reply_temp();
    console_reply(">ok");
}

/* ---- The expansion header ------------------------------------------------
 * Shared by `exp`, `exp_help` and `ps`, which is why they sit here rather
 * than next to the one handler that reads them.
 */

/** @brief How many bytes exp_caps() needs: four letters and a terminator. */
#define KCLI_EXP_CAPS_LEN  5U

/**
 * @brief The capability letters `exp` prints: d igital, m atrix, a dc, l p.
 * @param info The position, as khal_exp_info() reported it.
 * @param out  At least KCLI_EXP_CAPS_LEN bytes; filled and terminated.
 */
static void exp_caps(const khal_exp_info_t *info, char *out)
{
    size_t n = 0U;

    /* Every position is digital and matrix-routed. */
    out[n] = 'd';
    ++n;
    out[n] = 'm';
    ++n;
    if ( info->adc )
    {
        out[n] = 'a';
        ++n;
    }
    if ( info->lp )
    {
        out[n] = 'l';
        ++n;
    }
    out[n] = '\0';
}

/**
 * @brief What to call a mode on the wire.
 * @param m The mode khal_exp_info() reported.
 * @return The token, or "?" for a mode this table does not name.
 */
static const char *exp_mode_name(khal_exp_mode_t m)
{
    static const char *const NAMES[] = {
        "free", "in", "out", "adc", "pwm", "lent"
    };

    if ( (unsigned)m >= (sizeof(NAMES) / sizeof(NAMES[0])) )
    {
        return "?";
    }
    return NAMES[m];
}

/** @brief How much of a loan tag a record carries. `ps`'s entries and
 *         `exp`'s lent row both clip to this. */
#define KCLI_EXP_TAG_MAX  8U
/** @brief The widest entry: `<pos>:<mode>:<tag>` plus its separator. */
#define KCLI_EXP_PS_ENTRY    (2U + 1U + 4U + 1U + KCLI_EXP_TAG_MAX + 1U)
/** @brief Every position busy and lent, which is the worst this can be. */
#define KCLI_EXP_PS_LEN      ((KBSP_EXP_COUNT * KCLI_EXP_PS_ENTRY) + 1U)
/* The tag cap bounds the worst case, which must fit one line: console_reply()
 * would cut a longer record mid-entry. */
_Static_assert((KCLI_EXP_PS_LEN + 2U + sizeof(">|exp="))
               <= (size_t)KODE_LOG_LINE_MAX,
               "one ps expansion record, quoted, must fit inside one log line");

/**
 * @brief `ps`: the header's busy positions, as ONE record or as none at all.
 *
 * A free position contributes nothing and an all-free header prints no
 * record: fourteen `free` entries would bury the two that matter.
 */
static void ps_exp_record(void)
{
    char   list[KCLI_EXP_PS_LEN];
    char   tag[KCLI_EXP_TAG_MAX + 1U];
    size_t used = 0U;

    list[0] = '\0';
    for ( uint8_t pos = (uint8_t)KHAL_EXP_POS_MIN;
         (uint8_t)KHAL_EXP_POS_MAX >= pos; ++pos )
    {
        khal_exp_info_t info;
        khal_exp_info(pos, &info);
        if ( KHAL_EXP_FREE != info.mode )
        {
            const char *sep = "";
            int         n;
            if ( 0U < used )
            {
                sep = ",";
            }
            if ( KHAL_EXP_LENT == info.mode )
            {
                /* Clipped to what this entry budgeted for it. The whole list
                 * is quoted once it is built, since a tenant's name is text
                 * and may carry a space. */
                (void)snprintf(tag, sizeof(tag), "%s", info.tag);
                n = snprintf(&list[used], sizeof(list) - used, "%s%u:lent:%s",
                             sep, (unsigned)pos, tag);
            }
            else
            {
                n = snprintf(&list[used], sizeof(list) - used, "%s%u:%s",
                             sep, (unsigned)pos, exp_mode_name(info.mode));
            }
            if ( (0 < n) && ((size_t)n < (sizeof(list) - used)) )
            {
                used += (size_t)n;
            }
        }
    }
    if ( 0U < used )
    {
        /* What is left of the line, which holds the widest list whole (the
         * assertion beside KCLI_EXP_PS_LEN): only a tag that escapes its way
         * past that budget costs the list its tail, and console_kv_fit() says so. */
        char q[KODE_LOG_LINE_MAX - (sizeof(">|exp=") - 1U)];
        console_reply(">|exp=%s", console_kv_fit(q, sizeof(q), list, "exp"));
    }
}

/**
 * @brief Whether @p task is the task of the roster row named @p row.
 *
 * Every AO's task is its row's name in lower case (AO_START's name, kgui's
 * "gui"), so a level held by somebody else's AO is told apart from the row's.
 * A holder whose task is not made yet ("?") cannot be told apart, and is
 * taken as the row's.
 *
 * @param row  The row's name.
 * @param task The task name the registry holds at the row's level.
 * @return true when they match, letter case aside.
 */
static bool ps_task_is(const char *row, const char *task)
{
    size_t i = 0U;

    if ( strcmp(task, "?") == 0 )
    {
        return true;
    }
    while ( ('\0' != row[i]) && ('\0' != task[i])
           && (tolower((unsigned char)row[i]) == tolower((unsigned char)task[i])) )
    {
        ++i;
    }
    return ('\0' == row[i]) && ('\0' == task[i]);
}

/**
 * @brief One roster row that is a task rather than an Active Object.
 *
 * `ps` describes the running system, tasks included. Keep this list and
 * KCLI_PS_TASKS in step.
 *
 * @param k 0..KCLI_PS_TASKS-1.
 */
static void ps_task_record(uint16_t k)
{
    if ( 0U == k )
    {
        console_reply(">|prio=%u ao=reader owns=\"the console's blocking read (a task)\"",
              (unsigned)KODE_PRIO_READER);
    }
    else
    {
        /* Unreachable while KCLI_PS_TASKS counts the branches above. */
    }
}

/**
 * @brief One record for a started worker: its task, its stack and how much
 *        of it has never been used.
 *
 * The workers are the registry's (kqp_worker_start()), so a worker an app
 * starts is listed without this file hearing of it. `free` is the lowest the
 * stack has been since boot: the number a stack size is judged by.
 *
 * @param k Which worker, from 0; below kqp_worker_count().
 */
static void ps_worker_record(uint8_t k)
{
    KqpWorkerInfo info;
    char          word[CONSOLE_QUOTED_MAX(KQP_WORKER_NAME_MAX)];

    if ( kqp_worker_info(k, &info) )
    {
        console_reply(">|worker name=%s stack=%u free=%u",
              console_quote(word, sizeof(word), info.name),
              (unsigned)info.stack_bytes, (unsigned)info.free_bytes);
    }
}

/**
 * @brief Emit one slice of `ps`, and say whether that was the last.
 *
 * Five groups over one cursor: the AO roster, the non-AO tasks, the started
 * workers, the ownership table, then the header's record and the heap's. A
 * slot that prints nothing still costs its step.
 *
 * @param me The console; @c me->help_idx is where the last slice stopped.
 * @return true when there is nothing left to print.
 */
static bool ps_slice(Console *me)
{
    char           word[CONSOLE_QUOTED_MAX(48U)];
    char           who[CONSOLE_QUOTED_MAX(48U)];
    uint16_t const n_ao   = (uint16_t)(sizeof(AOS) / sizeof(AOS[0]));
    uint16_t const n_task = (uint16_t)(n_ao + KCLI_PS_TASKS);
    uint16_t const n_wrk  = (uint16_t)(n_task + (uint16_t)kqp_worker_count());
    uint16_t const n_res  = (uint16_t)(n_wrk + (uint16_t)KBSP_RES_MAX);
    uint16_t const total  = (uint16_t)(n_res + 2U);
    uint16_t       end    = (uint16_t)(me->help_idx + KCLI_SLICE);

    if ( end > total )
    {
        end = total;
    }
    for ( uint16_t i = me->help_idx; i < end; ++i )
    {
        if ( i < n_ao )
        {
            /* A row that did not start still costs its step, as the header's
             * does; and a level some app AO took is not this row's AO. */
            const char *task = NULL;
            if ( kqp_prio_taken(AOS[i].prio, &task) && ps_task_is(AOS[i].name, task) )
            {
                console_reply(">|prio=%u ao=%s owns=%s", (unsigned)AOS[i].prio,
                      AOS[i].name, console_quote(word, sizeof(word), AOS[i].owns));
            }
        }
        else if ( i < n_task )
        {
            ps_task_record((uint16_t)(i - n_ao));
        }
        else if ( i < n_wrk )
        {
            ps_worker_record((uint8_t)(i - n_task));
        }
        else if ( i < n_res )
        {
            /* The claims themselves, not the roster above. */
            kbsp_res_t const r = (kbsp_res_t)(i - n_wrk);
            console_reply(">|res=%s owner=%s",
                  console_quote(word, sizeof(word), kbsp_res_name(r)),
                  console_kv_text(who, sizeof(who), kbsp_owner_name(r)));
        }
        else if ( i == n_res )
        {
            ps_exp_record();
        }
        else
        {
            /* `heap` is mostly PSRAM, so the internal pool DMA and the ISRs
             * share rides along: int_free and int_max now, int_low the lowest
             * since boot, alloc_fail the requests the heap refused. */
            console_reply(">|heap=%u int_free=%u int_max=%u int_low=%u "
                  "alloc_fail=%lu log_dropped=%lu rx_dropped=%lu",
                  (unsigned)khal_heap_free(),
                  (unsigned)khal_heap_internal_free(),
                  (unsigned)khal_heap_internal_largest(),
                  (unsigned)khal_heap_internal_low(),
                  (unsigned long)khal_heap_alloc_failures(),
                  (unsigned long)log_dropped(),
                  (unsigned long)console_rx_dropped());
        }
    }
    me->help_idx = end;
    return (end >= total);
}

/**
 * @brief ps: one record per AO, one per started worker, one per claimed
 *        resource, one for the heap. Roster and claims should agree; printing
 *        both is what shows it when they do not.
 *
 * Paced like `help`: about fifty records, more than the large pool holds in
 * one step.
 */
static void h_ps(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)v;

    me->help_idx = 0U;
    cmd_begin(me, KCLI_PS, c->verb);
    if ( ps_slice(me) )
    {
        console_reply(">ok");
        console_end();
    }
    else
    {
        QTimeEvt_armX(&me->pacer, KCLI_SLICE_GAP_TICKS, 0U);
    }
}

/** @brief `i2c_scan`: the 7-bit addresses outside the two reserved ranges. */
#define KCLI_I2C_SCAN_FIRST     0x08U
#define KCLI_I2C_SCAN_LAST      0x77U
/** @brief How long one address is given to acknowledge. */
#define KCLI_I2C_SCAN_PROBE_MS  10U
/** @brief Addresses probed in one step: 8 x 10 ms at the very most, so a
 *         sweep over an empty bus never holds the thread for a second. */
#define KCLI_I2C_SCAN_CHUNK     8U

/**
 * @brief Probe the next chunk of the `i2c_scan` sweep.
 *
 * @param me  The console; @c scan_next is where the last step stopped and
 *            @c scan_found counts the addresses that answered so far.
 * @return true when the last address has been probed and the records are
 *         complete, false when another step is owed.
 */
static bool i2c_scan_step(Console *me)
{
    uint16_t addr = me->scan_next;
    unsigned probed = 0U;

    while ( (KCLI_I2C_SCAN_CHUNK > probed) && (KCLI_I2C_SCAN_LAST >= addr) )
    {
        if ( khal_i2c_probe(addr, KCLI_I2C_SCAN_PROBE_MS) )
        {
            console_reply(">|addr=0x%02X", (unsigned)addr);
            me->scan_found += 1U;
        }
        addr += 1U;
        probed += 1U;
    }
    me->scan_next = addr;
    if ( KCLI_I2C_SCAN_LAST >= addr )
    {
        return false;
    }
    console_reply(">|found=%u", (unsigned)me->scan_found);
    return true;
}

/**
 * @brief i2c_scan: which addresses on the shared bus acknowledge.
 *
 * One address-only probe per 7-bit address outside the reserved ranges, a
 * record for each that answers. No owner is asked: a probe touches no
 * register. KCLI_I2C_SCAN_CHUNK addresses per step, paced like `*_help`, so
 * the thread is never held for more than a few tens of milliseconds.
 */
static void h_i2c_scan(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)v;

    if ( khal_i2c_init() != KODE_OK )
    {
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        return;
    }
    me->scan_next  = KCLI_I2C_SCAN_FIRST;
    me->scan_found = 0U;
    cmd_begin(me, KCLI_I2C_SCAN, c->verb);
    if ( i2c_scan_step(me) )
    {
        console_reply(">ok");
        console_end();
    }
    else
    {
        QTimeEvt_armX(&me->pacer, KCLI_SLICE_GAP_TICKS, 0U);
    }
}

/**
 * @brief clocks: the two crystals and the tree they carry.
 *
 * Synchronous: no AO owns the clock block. It busy-waits about 92 ms at
 * KDRV_CLOCKS_CYCLES_FINE, interrupts on, which is affordable because every
 * AO that must not be delayed outranks AO_Console. The fine count, not the
 * fast one the test row takes: this is the bench instrument.
 */
static void h_clocks(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    kdrv_clocks_t r;

    (void)me; (void)v;

    if ( kdrv_clocks_read(&r, KDRV_CLOCKS_CYCLES_FINE) != KODE_OK )
    {
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        return;
    }

    console_reply(">|clk=x1 nominal_hz=%u", (unsigned)r.main_hz);
    console_reply(">|clk=slow_mux src=%s", kdrv_clocks_slow_token(r.slow_src));

    if ( r.x32k_ok )
    {
        console_reply(">|clk=x3 state=ok millihz=%u ppm=%+d", (unsigned)r.x32k_mhz,
              (int)r.x32k_ppm);
        /* The same ratio read backwards, not a second instrument: X1 is
         * what everything is counted against. */
        console_reply(">|clk=x1_from_x3 hz=%u", (unsigned)r.main_from_x32k_hz);
    }
    else
    {
        console_reply(">|clk=x3 state=silent");
    }

    if ( r.rc_slow_ok )
    {
        console_reply(">|clk=rc_slow state=ok millihz=%u", (unsigned)r.rc_slow_mhz);
    }
    else
    {
        console_reply(">|clk=rc_slow state=silent");
    }

    console_reply(">|clk=cpu hz=%u", (unsigned)r.cpu_hz);
    console_reply(">|clk=apb hz=%u", (unsigned)r.apb_hz);
    console_reply(">|clk=rtc_slow hz=%u", (unsigned)r.rtc_slow_hz);
    console_reply(">|clk=rtc_fast hz=%u", (unsigned)r.rtc_fast_hz);
    console_reply(">ok");
}

/**
 * @brief temp: the P4's die temperature, synchronously; nothing owns the sensor.
 */
static void h_temp(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)me;
    (void)v;
    if ( reply_temp() != KODE_OK )
    {
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        return;
    }
    console_reply(">ok");
}

/** @brief What the console calls itself in the ownership table. */
#define KCLI_EXP_OWNER      "console (bench)"
/** @brief A `mode=pwm` with no `hz=`. A kilohertz: audible on a scope, slow
 *         enough for a meter, and nothing a bench pin cannot carry. */
#define KCLI_EXP_HZ_DEF_N   1000  /**< Bare, for `help`. */
#define KCLI_EXP_HZ_DEF     ((uint32_t)KCLI_EXP_HZ_DEF_N)
/** @brief A `mode=pwm` with no `duty=`. Half, in tenths of a percent. */
#define KCLI_EXP_DUTY_DEF_N 500   /**< Bare, for `help`. */
#define KCLI_EXP_DUTY_DEF   ((uint16_t)KCLI_EXP_DUTY_DEF_N)

/**
 * @brief Whether the table printer may run a live conversion on an ADC row.
 *
 * A conversion uses the calibration handle of whoever opened the channel, and
 * a tenant closing it mid-conversion is a use-after-free. So convert only when
 * nobody holds the header or this AO's bench claim does. kbsp_claim() is only
 * reached once the header is taken, so a read never claims a free header.
 *
 * @param me The console, whose address is the token its bench claim uses.
 * @return true when the conversion is this AO's to make.
 */
static bool exp_may_convert(Console *me)
{
    if ( kbsp_owner_name(KBSP_RES_EXP) == NULL )
    {
        return true;
    }
    return kbsp_claim(KBSP_RES_EXP, me, KCLI_EXP_OWNER);
}

/**
 * @brief The name of a controller kind as `exp` prints it.
 * @param bus The kind.
 * @return "i2c", "spi", "uart", or "?" for a kind this console has not heard of.
 */
static const char *exp_bus_name(kbsp_exp_bus_t bus)
{
    static const char *const NAMES[] = { "i2c", "spi", "uart" };

    if ( (unsigned)bus >= (sizeof(NAMES) / sizeof(NAMES[0])) )
    {
        return "?";
    }
    return NAMES[bus];
}

/**
 * @brief One `>|exp_ctrl` record per controller an app may take for J3, after
 *        the position rows: its kind, its number, and who holds it.
 */
static void exp_ctrl_records(void)
{
    for ( size_t i = 0U; KBSP_EXP_CTRL_COUNT > i; ++i )
    {
        khal_exp_ctrl_info_t info;

        khal_exp_ctrl_info(i, &info);
        if ( NULL == info.tag )
        {
            console_reply(">|exp_ctrl bus=%s id=%u state=free tag=-",
                  exp_bus_name(info.bus), (unsigned)info.id);
        }
        else
        {
            char tag[KCLI_EXP_TAG_MAX + 1U];
            char qt[CONSOLE_QUOTED_MAX(KCLI_EXP_TAG_MAX)];
            /* Clipped and quoted as the lent rows' tags are. */
            (void)snprintf(tag, sizeof(tag), "%s", info.tag);
            console_reply(">|exp_ctrl bus=%s id=%u state=taken tag=%s",
                  exp_bus_name(info.bus), (unsigned)info.id,
                  console_quote(qt, sizeof(qt), tag));
        }
    }
}

/**
 * @brief exp: the expansion header's fourteen positions, or one of them.
 *
 * Synchronous: nothing in khal_exp blocks. With KBSP_RES_EXP held by another
 * owner, every write is refused naming it; with none, the first write claims
 * the header for the bench for the rest of the boot (`ps` shows it).
 */
static void h_exp(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    char caps[KCLI_EXP_CAPS_LEN];

    if ( !v[0].present )
    {
        /* A write key with no position (`exp mode=out level=1`) is refused
         * naming `pos`, not answered with the table and `>ok`. */
        if ( v[1].present || v[2].present || v[3].present || v[4].present
            || v[5].present )
        {
            console_reply_err(KCLI_ERR_MISSING, "pos");
            return;
        }
        /* The table. */
        for ( uint8_t pos = (uint8_t)KHAL_EXP_POS_MIN;
             (uint8_t)KHAL_EXP_POS_MAX >= pos; ++pos )
        {
            khal_exp_info_t info;
            khal_exp_info(pos, &info);
            exp_caps(&info, caps);
            if ( KHAL_EXP_LENT == info.mode )
            {
                char tag[KCLI_EXP_TAG_MAX + 1U];
                char qt[CONSOLE_QUOTED_MAX(KCLI_EXP_TAG_MAX)];
                /* Clipped as ps_exp_record() clips it, and quoted: a
                 * tenant's tag is a string it chose. */
                (void)snprintf(tag, sizeof(tag), "%s", info.tag);
                console_reply(">|exp=%u gpio=%u caps=%s mode=lent tag=%s", (unsigned)pos,
                      (unsigned)info.gpio, caps,
                      console_quote(qt, sizeof(qt), tag));
            }
            else if ( KHAL_EXP_ADC == info.mode )
            {
                uint16_t raw  = 0U;
                int32_t  mv   = 0;
                bool     cal  = false;
                bool     read = false;
                /* A failed conversion leaves raw and mv untouched, and a row
                 * the console may not convert (exp_may_convert()) has no
                 * number either: both print `?`, never stale zeros. */
                if ( exp_may_convert(me) )
                {
                    read = (khal_exp_adc_read(pos, &raw, &mv, &cal) == KODE_OK);
                }
                if ( read )
                {
                    console_reply(">|exp=%u gpio=%u caps=%s mode=adc raw=%u mv=%ld cal=%u",
                          (unsigned)pos, (unsigned)info.gpio, caps,
                          (unsigned)raw, (long)mv, (unsigned)cal);
                }
                else
                {
                    console_reply(">|exp=%u gpio=%u caps=%s mode=adc raw=? mv=? cal=?",
                          (unsigned)pos, (unsigned)info.gpio, caps);
                }
            }
            else
            {
                unsigned level = 0U;
                if ( khal_exp_level(pos) )
                {
                    level = 1U;
                }
                console_reply(">|exp=%u gpio=%u caps=%s mode=%s level=%u", (unsigned)pos,
                      (unsigned)info.gpio, caps, exp_mode_name(info.mode),
                      level);
            }
        }
        exp_ctrl_records();
        console_reply(">ok");
        return;
    }
    if ( !v[1].present )
    {
        console_reply_err(KCLI_ERR_MISSING, "mode");
        return;
    }
    /* A write: kbsp_claim() grants a free resource or one this AO holds, and
     * refuses only another owner's. */
    if ( !kbsp_claim(KBSP_RES_EXP, me, KCLI_EXP_OWNER) )
    {
        const char *owner = kbsp_owner_name(KBSP_RES_EXP);
        if ( NULL == owner )
        {
            owner = "?";
        }
        /* The log line is what reaches the wire with the name in it;
         * console_reply_err()'s `msg=` is one token. */
        KLOG_W(TAG, "exp: owned by %s", owner);
        console_reply_err(KCLI_ERR_REJECT, "owned");
        return;
    }
    {
        uint8_t const pos = (uint8_t)v[0].u;
        /* The parser fills an absent key with zero rather than with its
         * printed default, so the two keys whose default is not zero are
         * applied here. `pull` and `level` want zero when they are absent -
         * KHAL_GPIO_PULL_NONE and low - so they are read straight. */
        uint32_t hz   = KCLI_EXP_HZ_DEF;
        uint16_t duty = KCLI_EXP_DUTY_DEF;
        kode_status_t rc = KODE_DENIED;

        if ( v[4].present )
        {
            hz = v[4].u;
        }
        if ( v[5].present )
        {
            duty = (uint16_t)v[5].u;
        }
        switch ( v[1].u )
        {
            case 0U: rc = khal_exp_release(pos); break;
            case 1U: rc = khal_exp_input(pos, (khal_gpio_pull_t)v[2].u); break;
            case 2U: rc = khal_exp_output(pos, 0U != v[3].u); break;
            case 3U: rc = khal_exp_adc(pos); break;
            case 4U: rc = khal_exp_pwm(pos, hz, duty); break;
            default: break;
        }
        if ( KODE_OK != rc )
        {
            console_reply_err(KCLI_ERR_REJECT, c->verb);
            return;
        }
        console_reply(">ok");
    }
}

/**
 * @brief exp_help: what each position may become, and the grammar of the keys.
 *
 * Read from kbsp's own table, so the listing follows the board. About
 * eighteen records, sent in one step.
 */
static void h_exp_help(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    char caps[KCLI_EXP_CAPS_LEN];

    (void)me; (void)c; (void)v;

    for ( uint8_t pos = (uint8_t)KHAL_EXP_POS_MIN;
         (uint8_t)KHAL_EXP_POS_MAX >= pos; ++pos )
    {
        khal_exp_info_t info;
        const char     *may  = "in,out,pwm";
        const char     *lent = "no";

        khal_exp_info(pos, &info);
        exp_caps(&info, caps);
        /* Every position does in, out and pwm; only the ADC channel varies. */
        if ( info.adc )
        {
            may = "in,out,adc,pwm";
        }
        if ( KHAL_EXP_LENT == info.mode )
        {
            lent = "yes";
        }
        console_reply(">|exp=%u gpio=%u caps=%s may=%s lent=%s", (unsigned)pos,
              (unsigned)info.gpio, caps, may, lent);
    }
    /* The grammar, one record per mode, so a host parses keys, not prose.
     * `syntax=` keeps them apart from the position rows. */
    console_reply(">|syntax=exp pos=%u..%u mode=free|in|out|adc|pwm",
          (unsigned)KHAL_EXP_POS_MIN, (unsigned)KHAL_EXP_POS_MAX);
    console_reply(">|syntax=in pull=none|up|down");
    console_reply(">|syntax=out level=0|1");
    console_reply(">|syntax=pwm hz=%u..%u duty=0..%u",
          (unsigned)KHAL_EXP_PWM_HZ_MIN, (unsigned)KHAL_EXP_PWM_HZ_MAX,
          (unsigned)KHAL_EXP_PWM_DUTY_MAX);
    /* The controllers kodeOS keeps (kbsp_board.h), which the exp_ctrl records
     * leave out: board bus, NFC, codec, the RFID demodulator's continuous ADC,
     * and J3's sampled ADC. The LEDC is shared with J3, so it is not here. */
    console_reply(">|kodeos_ctrl i2c=%u spi=%u i2s=%u adc=%u adc_dma=rfid exp_adc=%u",
          (unsigned)KBSP_I2C_BOARD, (unsigned)KBSP_SPI_NFC,
          (unsigned)KBSP_I2S_CODEC, (unsigned)KBSP_ADC_RFID,
          (unsigned)KBSP_ADC_EXP);
    console_reply(">ok");
}

/**
 * @brief time: read the wall clock, or set it.
 *
 * Synchronous: setting the clock is one whole write, so no owner is needed.
 * A clock never set says `state=unset`, not the epoch.
 */
static void h_time(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    char iso[32];

    (void)me;

    if ( v[0].present )
    {
        if ( kdrv_clocks_wall_set_iso(v[0].s) != KODE_OK )
        {
            console_reply_err(KCLI_ERR_VALUE, "set");
            return;
        }
        console_reply(">ok");
        return;
    }

    if ( v[1].present && v[1].b )
    {
        /* The client only exists once there is an address, so a refusal here
         * is what a board with no network looks like - said plainly rather
         * than left as a silence the caller has to time out on. */
#if CONFIG_KODEOS_NET
        /* The network is AO_Net's: with no AO_Net there is no client to ask. */
        if ( console_owner_absent(&kaos_net_start) )
        {
            return;
        }
        if ( kdrv_net_sntp_sync_now() != KODE_OK )
        {
            console_reply_err(KCLI_ERR_REJECT, "sync");
            return;
        }
        console_reply(">ok");
#else
        console_reply_err(KCLI_ERR_REJECT, "sync");
#endif
        return;
    }

    if ( kdrv_clocks_wall_iso(iso, sizeof(iso)) != KODE_OK )
    {
        console_reply(">|time state=unset");
    }
    else
    {
        /* Quoted the way `time set=` takes it, so the answer can be typed
         * straight back. The firmware formats it, so it never holds a byte
         * console_quote() would have to escape. */
        const char *state = "known";

        /* Put back after a power cut, and late by however long the board
         * was off: said until a time server or a person replaces it. */
        if ( kdrv_clocks_wall_approx() )
        {
            state = "approx";
        }
        console_reply(">|time state=%s utc=\"%s\" source=%s", state, iso,
              kdrv_clocks_wall_source_token(kdrv_clocks_wall_source()));
    }
    console_reply(">ok");
}

/** @brief The printable name of each karg_type_t, for help records. */
static const char *const TYPE_NAMES[] = {
    "uint", "bool", "enum", "string", "rgb", "text", "row",
};
_Static_assert((sizeof(TYPE_NAMES) / sizeof(TYPE_NAMES[0]))
               == ((size_t)KARG_ROW + 1U),
               "one help name per karg_type_t");

/**
 * @brief One command's key specs as a record value: no spaces, keys joined
 *        by commas, each `key:type:req|opt[=default]`, enums spelled out.
 *        `-` when the command takes no keys.
 */
static void args_desc(const karg_spec_t *args, uint8_t n_args, char *dst,
                      size_t cap)
{
    size_t used = 0U;
    if ( 0U == n_args )
    {
        (void)snprintf(dst, cap, "-");
        return;
    }
    dst[0] = '\0';
    for ( uint8_t a = 0U; a < n_args; ++a )
    {
        karg_spec_t const * const s = &args[a];
        int n = snprintf(&dst[used], cap - used, "%s%s:%s",
                         (0U < a) ? "," : "", s->key, TYPE_NAMES[s->type]);
        used += ((0 < n) && ((size_t)n < (cap - used))) ? (size_t)n : 0U;
        /* Not just for an enum: a string's members are the prefixes it
         * understands (`src`), where an enum's are its values (`dst`). */
        if ( NULL != s->enums )
        {
            for ( uint8_t m = 0U; m < s->n_enums; ++m )
            {
                n = snprintf(&dst[used], cap - used, "%c%s",
                             (0U == m) ? '(' : '|', s->enums[m]);
                used += ((0 < n) && ((size_t)n < (cap - used))) ? (size_t)n : 0U;
            }
            n = snprintf(&dst[used], cap - used, ")");
            used += ((0 < n) && ((size_t)n < (cap - used))) ? (size_t)n : 0U;
        }
        n = snprintf(&dst[used], cap - used, ":%s%s%s",
                     s->required ? "req" : "opt",
                     (NULL != s->def) ? "=" : "",
                     (NULL != s->def) ? s->def : "");
        used += ((0 < n) && ((size_t)n < (cap - used))) ? (size_t)n : 0U;
    }
}

/**
 * @brief tasks: every thread the scheduler is running, not just ours.
 *
 * Separate from `ps`, which is paced; this fits one burst. It shows what `ps`
 * cannot: the tasks of ESP-IDF and the vendor stacks under kdrv (SDIO
 * transport, RPC, NimBLE host, lwIP), with their own priorities.
 */
static void h_tasks(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)me; (void)c; (void)v;
    khal_task_info_t info[28];
    size_t total = 0U;
    size_t const shown = khal_task_list(info, sizeof(info) / sizeof(info[0]),
                                        &total);

    if ( 0U == total )
    {
        /* The scheduler was not built to be asked (FREERTOS_USE_TRACE_
         * FACILITY off). Say that rather than report an empty system. */
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        return;
    }
    char word[CONSOLE_QUOTED_MAX(24U)];
    for ( size_t i = 0U; i < shown; ++i )
    {
        console_reply(">|task=%s prio=%u stack_free=%lu",
              console_quote(word, sizeof(word), info[i].name),
              (unsigned)info[i].prio,
              (unsigned long)info[i].stack_free);
    }
    /* Last, and always: a board running more tasks than one snapshot holds
     * shows none, and this record tells that from a board running none. */
    console_reply(">|tasks=%u shown=%u", (unsigned)total, (unsigned)shown);
    console_reply(">ok");
}

/**
 * @brief Emit one slice of the listing, and say whether that was the last.
 *
 * @param me The console; @c me->help_idx is where the last slice stopped.
 * @return true when the table is exhausted.
 */
static bool help_slice(Console *me)
{
    char desc[KCLI_ARGS_DESC_MAX];
    uint16_t const total = (uint16_t)(g_ncmds + kverbs_count());
    uint16_t       end   = total;

    if ( (uint16_t)(me->help_idx + KCLI_SLICE) < total )
    {
        end = (uint16_t)(me->help_idx + KCLI_SLICE);
    }
    /* The console's own verbs, then the owners' in registration order. */
    for ( uint16_t i = me->help_idx; i < end; ++i )
    {
        if ( (size_t)i < g_ncmds )
        {
            args_desc(g_cmds[i].args, g_cmds[i].n_args, desc, sizeof(desc));
            console_reply(">|cmd=%s args=%s", g_cmds[i].verb, desc);
        }
        else
        {
            const kverb_t * const r = kverbs_at((size_t)i - g_ncmds);

            if ( NULL != r )
            {
                args_desc(r->args, r->n_args, desc, sizeof(desc));
                console_reply(">|cmd=%s args=%s", r->verb, desc);
            }
        }
    }
    me->help_idx = end;
    return (end >= total);
}

/**
 * @brief help: one record per command, straight from the table, in slices.
 *
 * Paced (KCLI_SLICE): in one step the listing would exhaust the large pool
 * before AO_Log writes a line, losing its tail and the `>ok`.
 */
static void h_help(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)v;
    me->help_idx = 0U;
    cmd_begin(me, KCLI_HELP, c->verb);
    if ( help_slice(me) )
    {
        console_reply(">ok");
        console_end();
    }
    else
    {
        QTimeEvt_armX(&me->pacer, KCLI_SLICE_GAP_TICKS, 0U);
    }
}

/* ---- The app's variables and functions --------------------------------------
 * `variable` reads or follows a row an app registered with kode_variables_add();
 * `function` runs one of kode_functions_add()'s; the `_list` verbs print them.
 * The registry (kregistry.c) parses and spells; this AO does the wire. Callbacks
 * run synchronously on this task: short, never blocking. `variable` never runs
 * anything, so a host may read and follow every variable safely.
 */

/** @brief The word for a function's argument type, its `arg=`. */
static const char *type_word(kode_type_t t)
{
    static const char *const WORDS[] = { "none", "int", "bool", "enum", "string" };
    /* The registry refuses a type outside the enum, so this index is in range -
     * but only while the list here still covers the enum, which is what fails
     * the build rather than the bench if a type is ever added. */
    _Static_assert((sizeof(WORDS) / sizeof(WORDS[0]))
                   == ((size_t)KODE_TYPE_STRING + 1U),
                   "one type word per kode_type_t");
    return WORDS[(unsigned)t];
}

/** @brief The slot following @p row, or NULL. */
static kcli_watch_t *watch_find(Console *me, const kode_variable_t *row)
{
    for ( size_t i = 0U; KODE_VARIABLE_WATCH_MAX > i; ++i )
    {
        if ( me->watch[i].row == row )
        {
            return &me->watch[i];
        }
    }
    return NULL;
}

/** @brief The period a variable is being followed at, or 0. */
static uint16_t variable_watch_ms(Console *me, const kode_variable_t *row)
{
    const kcli_watch_t * const w = watch_find(me, row);
    return (NULL != w) ? w->period_ms : 0U;
}

/** @brief Free a slot: disarm its timer and forget its row. */
static void watch_stop(kcli_watch_t *w)
{
    (void)QTimeEvt_disarm(&w->tev);
    w->row       = NULL;
    w->period_ms = 0U;
}

/**
 * @brief Follow @p row every @p ms, in its own slot or a free one.
 *
 * A row already followed keeps its slot and takes the new period: disarm
 * then arm, because QTimeEvt_rearm keeps the old interval and armX on an
 * armed timer is a QP assert.
 *
 * @return false when every slot is taken by another row.
 */
static bool watch_start(Console *me, const kode_variable_t *row, uint16_t ms)
{
    kcli_watch_t *w = watch_find(me, row);
    if ( NULL == w )
    {
        w = watch_find(me, NULL);
    }
    if ( NULL == w )
    {
        return false;
    }
    (void)QTimeEvt_disarm(&w->tev);
    w->row       = row;
    w->period_ms = ms;
    QTimeEvt_armX(&w->tev, KQP_TICKS_PER_MS((uint32_t)ms),
                  KQP_TICKS_PER_MS((uint32_t)ms));
    return true;
}

/**
 * @brief One period ran out: read the row and put its record on the wire.
 *
 * The same record a point read emits, so a host parses one shape. A
 * congested port costs the sample and nothing else - a late sample is not
 * the one that was asked for. A read that fails ends the watch, with one
 * line saying why: a stream that outlives its source would be a lie.
 */
static void watch_tick(kcli_watch_t *w)
{
    if ( console_congested() )
    {
        return;
    }
    char                word[KREGISTRY_WORD_MAX];
    kode_status_t const rc = kvariables_read(w->row, word, sizeof(word));
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "variable %s: watch stopped, get answered %u",
               w->row->name, (unsigned)rc);
        watch_stop(w);
        return;
    }
    char qn[CONSOLE_QUOTED_MAX(KODE_ROW_NAME_MAX)];
    console_reply(">|variable=%s value=%s",
          console_quote(qn, sizeof(qn), w->row->name), word);
}

/**
 * @brief Emit one slice of `variable_list`, and say whether it was the last.
 *
 * `desc=` is prose, quoted as field_help_slice() quotes it, and so is the
 * name, the way `variable name=` takes it. The value is the getter's bare
 * word. The caps on names and descriptions keep the widest record inside a
 * protocol line.
 */
static bool variable_slice(Console *me)
{
    size_t const total = kvariables_count();
    size_t const end   = ((me->help_idx + KCLI_SLICE) < total)
                       ? (me->help_idx + KCLI_SLICE) : total;
    for ( size_t i = me->help_idx; i < end; ++i )
    {
        const kode_variable_t * const r = kvariables_at(i);
        char qn[CONSOLE_QUOTED_MAX(KODE_ROW_NAME_MAX)];
        char qd[CONSOLE_QUOTED_MAX(KODE_ROW_DESC_MAX)];
        console_reply(">|variable=%s watch=%u desc=%s",
              console_quote(qn, sizeof(qn), r->name),
              (unsigned)variable_watch_ms(me, r),
              console_kv_text(qd, sizeof(qd), r->desc));
    }
    me->help_idx = (uint16_t)end;
    return (end >= total);
}

/** @brief function_slice()'s bounds buffer: the `enum=` key, every word of the
 *         longest possible list with its separator, and the terminator. */
#define KCLI_FUNCTION_EXTRA_LEN \
    (6U + (KODE_VALUE_ENUMS_MAX * (KODE_ROW_NAME_MAX + 1U)) + 1U)
/* Sized from the caps, so snprintf never cuts an enum word in half (a
 * different word); raising a cap fails the build here. */
_Static_assert(KCLI_FUNCTION_EXTRA_LEN
               >= (sizeof(" enum=")
                   + (KODE_VALUE_ENUMS_MAX * (KODE_ROW_NAME_MAX + 1U))),
               "extra[] must hold the widest enum= list whole");

/** @brief Emit one slice of `function_list`; variable_slice()'s shape, with the
 *         argument's type and what it may be. */
static bool function_slice(Console *me)
{
    size_t const total = kfunctions_count();
    size_t const end   = ((me->help_idx + KCLI_SLICE) < total)
                       ? (me->help_idx + KCLI_SLICE) : total;
    for ( size_t i = me->help_idx; i < end; ++i )
    {
        const kode_function_t * const r = kfunctions_at(i);
        char extra[KCLI_FUNCTION_EXTRA_LEN];
        extra[0] = '\0';
        if ( KODE_TYPE_INT == r->arg )
        {
            (void)snprintf(extra, sizeof(extra), " min=%ld max=%ld",
                           (long)r->min, (long)r->max);
        }
        else if ( KODE_TYPE_ENUM == r->arg )
        {
            size_t at = (size_t)snprintf(extra, sizeof(extra), " enum=");
            for ( uint8_t k = 0U; k < r->n_enums; ++k )
            {
                at += (size_t)snprintf(&extra[at], sizeof(extra) - at, "%s%s",
                                       (0U == k) ? "" : ",", r->enums[k]);
            }
        }
        else
        {
            /* bool, string and none carry no bounds. */
        }
        char qn[CONSOLE_QUOTED_MAX(KODE_ROW_NAME_MAX)];
        char qd[CONSOLE_QUOTED_MAX(KODE_ROW_DESC_MAX)];
        console_reply(">|function=%s arg=%s%s desc=%s",
              console_quote(qn, sizeof(qn), r->name), type_word(r->arg),
              extra, console_kv_text(qd, sizeof(qd), r->desc));
    }
    me->help_idx = (uint16_t)end;
    return (end >= total);
}

/** @brief The next slice of whichever list is in flight. */
static bool list_slice(Console *me)
{
    return ((uint8_t)KCLI_VARIABLE_LIST == me->inflight) ? variable_slice(me)
                                                         : function_slice(me);
}

/** @brief Start a list: the first slice now, the rest paced; see the slicers. */
static void list_begin(Console *me, const kcmd_t *c, kcli_inflight_t what)
{
    me->help_idx = 0U;
    cmd_begin(me, what, c->verb);
    if ( list_slice(me) )
    {
        console_reply(">ok");
        console_end();
    }
    else
    {
        QTimeEvt_armX(&me->pacer, KCLI_SLICE_GAP_TICKS, 0U);
    }
}

/** @brief variable_list: the app's variables, in slices. */
static void h_variable_list(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)v;
    list_begin(me, c, KCLI_VARIABLE_LIST);
}

/** @brief function_list: the app's functions, in slices. */
static void h_function_list(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)v;
    list_begin(me, c, KCLI_FUNCTION_LIST);
}

/** @brief variable: read one row, or follow it with `watch=`. */
static void h_variable(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)c;
    const kode_variable_t * const row = kvariables_find(v[0].s);
    if ( NULL == row )
    {
        console_reply_err(KCLI_ERR_VALUE, "name");
        return;
    }
    if ( v[1].present )
    {
        /* Stopping comes before every rule about arming: `watch=0` is the host
         * saying "send me nothing for this row", which is a true statement
         * about any row, followed or not. */
        if ( 0U == v[1].u )
        {
            kcli_watch_t * const w = watch_find(me, row);
            if ( NULL != w )
            {
                watch_stop(w);
            }
            console_reply(">ok");
            return;
        }
        /* A period under the floor is a port full of records. */
        if ( KODE_VARIABLE_WATCH_MIN_MS > v[1].u )
        {
            console_reply_err(KCLI_ERR_VALUE, "watch");
            return;
        }
        if ( !watch_start(me, row, (uint16_t)v[1].u) )
        {
            console_reply_err(KCLI_ERR_REJECT, "slots");
            return;
        }
        /* An accept, the way a mutation's >ok is: the records follow. */
        console_reply(">ok");
        return;
    }
    char                word[KREGISTRY_WORD_MAX];
    kode_status_t const rc = kvariables_read(row, word, sizeof(word));
    if ( KODE_OK != rc )
    {
        console_reply_err(KCLI_ERR_REJECT, row->name);
        return;
    }
    char qn[CONSOLE_QUOTED_MAX(KODE_ROW_NAME_MAX)];
    console_reply(">|variable=%s value=%s",
          console_quote(qn, sizeof(qn), row->name), word);
    console_reply(">ok");
}

/**
 * @brief Close a run by what the callback answered.
 *
 * KODE_DENIED is the row refusing its argument, named after the key it came
 * in; anything else that is not KODE_OK is the row's own failure, named after
 * the row.
 */
static void function_close(kode_status_t rc, const char *name)
{
    if ( KODE_OK == rc )
    {
        console_reply(">ok");
    }
    else if ( KODE_DENIED == rc )
    {
        console_reply_err(KCLI_ERR_VALUE, "arg");
    }
    else
    {
        console_reply_err(KCLI_ERR_REJECT, name);
    }
}

/* ---- Handlers: config ----------------------------------------------------
 * The console holds the rows, never the values: each one is asked of its
 * owner by CONFIG_REQ_SIG, one at a time, and printed from its answer.
 */

/**
 * @brief Ask @p row's owner for its value, or to set it to @p val.
 *
 * Posted to the row's owner with its registered margin (kode_config_post()),
 * and only while every queue is at least half free (kqp_queues_half_free()):
 * answers are published to subscribers below the console, which a burst of
 * requests would fill.
 *
 * @param me  The console.
 * @param row The setting.
 * @param val The value to set, or NULL to read it.
 * @return true when the request went; false for a full queue anywhere, an
 *         empty pool or an owner whose queue is too full, which the caller
 *         answers as busy.
 */
static bool config_ask(Console *me, const kode_config_t *row,
                       const kode_value_t *val)
{
    KodeConfigReqEvt *e = NULL;
    bool sent = false;

    if ( kqp_queues_half_free() )
    {
        e = Q_NEW_X(KodeConfigReqEvt, 1U, CONFIG_REQ_SIG);
    }
    if ( NULL != e )
    {
        me->cfg_seq += 1U;
        me->cfg_row  = row;
        e->seq = me->cfg_seq;
        e->set = (NULL != val);
        (void)snprintf(e->key, sizeof(e->key), "%s", row->name);
        e->i = 0;
        e->b = false;
        e->e = 0U;
        e->s[0] = '\0';
        if ( NULL != val )
        {
            e->i = val->i;
            e->b = val->b;
            e->e = val->e;
            if ( NULL != val->s )
            {
                (void)snprintf(e->s, sizeof(e->s), "%s", val->s);
            }
        }
        sent = kode_config_post(e, &me->super);
    }
    if ( sent )
    {
        /* A listing's next row is still the same command: its watchdog is
         * re-armed, never armed twice. */
        if ( (uint8_t)KCLI_CONFIG != me->inflight )
        {
            cmd_begin(me, KCLI_CONFIG, "config");
        }
        console_extend(KODE_CONFIG_ANSWER_MS);
    }
    return sent;
}

/** @brief One `>|config` record: @p value is NULL when the owner gave none. */
static void config_record(const kode_config_t *row, const char *value, bool stored)
{
    char qk[CONSOLE_QUOTED_MAX(KODE_CONFIG_KEY_MAX)];
    char qv[CONSOLE_QUOTED_MAX(KODE_CONFIG_VALUE_MAX)];
    const char *word = "-";
    const char *st   = "-";

    if ( NULL != value )
    {
        word = value;
        if ( KODE_TYPE_STRING == row->type )
        {
            word = console_quote(qv, sizeof(qv), value);
        }
        st = "no";
        if ( stored )
        {
            st = "yes";
        }
    }
    console_reply(">|config key=%s value=%s stored=%s",
          console_quote(qk, sizeof(qk), row->name), word, st);
}

/**
 * @brief The listing's next row, or its end: called once a row is printed.
 */
static void config_next(Console *me)
{
    const kode_config_t *row = NULL;
    bool asked = false;

    while ( !asked )
    {
        me->cfg_idx += 1U;
        row = kconfig_at(me->cfg_idx);
        if ( NULL == row )
        {
            break;
        }
        asked = config_ask(me, row, NULL);
        if ( !asked )
        {
            config_record(row, NULL, false);
        }
    }
    if ( !asked )
    {
        console_reply(">ok");
        console_end();
    }
}

/**
 * @brief config [key=] [value=]: list every setting, read one, or set one.
 */
static void h_config(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)c;
    const kode_config_t *row = NULL;
    kode_value_t val;

    me->cfg_all = false;
    if ( !v[0].present )
    {
        if ( v[1].present )
        {
            console_reply_err(KCLI_ERR_MISSING, "key");
            return;
        }
        me->cfg_idx = 0U;
        row = kconfig_at(0U);
        if ( NULL == row )
        {
            console_reply(">ok");
            return;
        }
        me->cfg_all = true;
        if ( !config_ask(me, row, NULL) )
        {
            config_record(row, NULL, false);
            config_next(me);
        }
        return;
    }
    row = kconfig_find(v[0].s);
    if ( NULL == row )
    {
        console_reply_err(KCLI_ERR_VALUE, "key");
        return;
    }
    if ( v[1].present )
    {
        /* A string value is quoted and every other kind is bare; only the
         * row knows which this one is. */
        bool const want_quoted = (KODE_TYPE_STRING == row->type);
        if ( (v[1].quoted != want_quoted)
            || (kconfig_parse(row, v[1].s, &val) != KODE_OK) )
        {
            console_reply_err(KCLI_ERR_VALUE, "value");
            return;
        }
        if ( !config_ask(me, row, &val) )
        {
            console_reply_err(KCLI_ERR_BUSY, "busy");
        }
        return;
    }
    if ( !config_ask(me, row, NULL) )
    {
        console_reply_err(KCLI_ERR_BUSY, "busy");
    }
}

/** @brief The ticket of the last function that answers in records that was run;
 *         function_ticket() hands it to the run, the answer echoes it. */
static uint32_t s_fn_ticket;

/**
 * @brief function: run one row, with its argument when the line carries one.
 *
 * Nothing is read back: a function's outcome is its >ok or its >err, and
 * anything it changed that a host wants to see is a variable.
 */
static void h_function(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)c;
    const kode_function_t * const row = kfunctions_find(v[0].s);
    kode_value_t arg;
    const kode_value_t *argp = NULL;
    kode_status_t rc;
    if ( NULL == row )
    {
        console_reply_err(KCLI_ERR_VALUE, "name");
        return;
    }
    if ( v[1].present )
    {
        /* A string argument is quoted and every other kind is bare, the one
         * rule of the grammar; only the row knows which this one is. */
        bool const want_quoted = (KODE_TYPE_STRING == row->arg);
        if ( (v[1].quoted != want_quoted)
            || (kfunctions_parse(row, v[1].s, &arg) != KODE_OK) )
        {
            console_reply_err(KCLI_ERR_VALUE, "arg");
            return;
        }
        argp = &arg;
    }
    if ( 0U == row->answer_ms )
    {
        function_close(row->run(argp), row->name);
        return;
    }
    /* The line stays open for the row's records. The watchdog is armed
     * before the run, which may be answered at once from another core: the
     * answer waits in the queue until this step is over. */
    cmd_begin(me, KCLI_FUNCTION, row->name);
    console_extend(row->answer_ms);
    s_fn_ticket += 1U;
    if ( 0U == s_fn_ticket )
    {
        s_fn_ticket = 1U;
    }
    rc = row->run(argp);
    if ( KODE_OK != rc )
    {
        function_close(rc, row->name);
        console_end();
    }
}

uint32_t function_ticket(void)
{
    return s_fn_ticket;
}

bool function_record(uint32_t ticket, const char *record)
{
    FunctionAnsEvt *e = NULL;
    bool sent = false;

    if ( (NULL != record) && (strlen(record) < (size_t)KODE_FUNCTION_REC_MAX) )
    {
        e = Q_NEW_X(FunctionAnsEvt, 0U, FUNCTION_ANS_SIG);
    }
    if ( NULL != e )
    {
        e->ticket = ticket;
        e->done   = false;
        e->status = (uint8_t)KODE_OK;
        (void)snprintf(e->rec, sizeof(e->rec), "%s", record);
        sent = QACTIVE_POST_X(AO_Console, &e->super, 1U, (void *)0);
    }
    return sent;
}

bool function_end(uint32_t ticket, kode_status_t status, const char *msg)
{
    FunctionAnsEvt * const e = Q_NEW_X(FunctionAnsEvt, 0U, FUNCTION_ANS_SIG);
    bool sent = false;

    if ( NULL != e )
    {
        e->ticket = ticket;
        e->done   = true;
        e->status = (uint8_t)status;
        e->rec[0] = '\0';
        e->msg[0] = '\0';
        if ( NULL != msg )
        {
            (void)snprintf(e->msg, sizeof(e->msg), "%s", msg);
        }
        sent = QACTIVE_POST_X(AO_Console, &e->super, 1U, (void *)0);
    }
    return sent;
}

const char *function_quote(char *dst, size_t cap, const char *src)
{
    return console_quote(dst, cap, src);
}

/* ---- The payload producer ------------------------------------------------
 * `screenshot dst=console` and `file_get` send bytes as base64 in ordinary
 * `>|` records, paced like `help`. Not raw bytes: klog's rescue path can
 * write into the stream bypassing AO_Log, and the BLE console splits on
 * newlines. A `!` line among the records is just another line. The encoder
 * is kaos_wire.h's.
 */

/** @brief `sd` or `fs`, for a record value. */
static const char *medium_name(uint8_t medium)
{
    return ((uint8_t)KDRV_MEDIUM_SD == medium) ? "sd" : "fs";
}

/**
 * @brief Let go of whatever the producer is holding.
 *
 * Called on every exit - the terminator, an error, and the watchdog - because
 * a capture left frozen would mirror one old frame for every capture after
 * it, and would do it without a word.
 */
static void payload_release(Console *me)
{
    if ( ((uint8_t)KCLI_SHOT == me->inflight)
        || ((uint8_t)KCLI_SHOT_FILE == me->inflight) )
    {
        display_capture_release();
    }
    else if ( (uint8_t)KCLI_FILE == me->inflight )
    {
        /* Closing drops every loan, so the borrowed buffer needs no separate
         * hand-back - and asking for one would start a read nobody is left to
         * collect. */
        storage_close_request();
    }
    else
    {
        /* Nothing was held. */
    }
    me->pay.borrowed = false;
    me->pay.run      = NULL;
    me->pay.run_len  = 0U;
    me->pay.pos      = 0U;
}

/**
 * @brief Emit up to KCLI_SLICE records from the current source.
 *
 * Every record it emits is a whole number of base64 groups, so none of them
 * carries padding; whatever does not divide by three waits in `carry` for the
 * next run. payload_flush() is what finally lets a record be padded, and it
 * runs once, at the end.
 *
 * @return true when this source has nothing left to give.
 */
static bool payload_slice(Console *me)
{
    char                   b64[KCLI_PAY_B64 + 1U];
    uint8_t                src[KCLI_PAY_BYTES];
    kcli_payload_t * const p = &me->pay;

    for ( uint8_t n = 0U; KCLI_SLICE > n; ++n )
    {
        /* Per record, not per slice: the BLE output ring (2048 bytes) is
         * smaller than a slice, so one check per slice could overfill it. */
        if ( console_congested() )
        {
            break;
        }
        size_t take = (size_t)p->n_carry;
        if ( 0U < take )
        {
            (void)memcpy(src, p->carry, take);
            p->n_carry = 0U;
        }
        uint32_t const left = p->run_len - p->pos;
        size_t const   room = (size_t)KCLI_PAY_BYTES - take;
        size_t const   more = ((uint32_t)room < left) ? room : (size_t)left;
        if ( 0U < more )
        {
            (void)memcpy(&src[take], &p->run[p->pos], more);
            take   += more;
            p->pos += (uint32_t)more;
        }

        /* Hold back a part-group so this record needs no padding. */
        size_t const keep = take % 3U;
        if ( 0U < keep )
        {
            take -= keep;
            (void)memcpy(p->carry, &src[take], keep);
            p->n_carry = (uint8_t)keep;
        }

        if ( 0U < take )
        {
            (void)kaos_wire_b64_encode(src, take, b64);
            console_reply(">|d=%s", b64);
            p->total += (uint32_t)take;
        }
        if ( p->pos >= p->run_len )
        {
            break;
        }
    }
    return (p->pos >= p->run_len);
}

/** @brief Emit the held-back tail. The one record allowed to be padded. */
static void payload_flush(Console *me)
{
    char b64[KCLI_PAY_B64 + 1U];
    if ( 0U < me->pay.n_carry )
    {
        (void)kaos_wire_b64_encode(me->pay.carry, (size_t)me->pay.n_carry, b64);
        console_reply(">|d=%s", b64);
        me->pay.total  += me->pay.n_carry;
        me->pay.n_carry = 0U;
    }
}

/** @brief Close a finished payload: tail, terminator, release, console free. */
static void payload_done(Console *me)
{
    payload_flush(me);
    console_reply(">ok");
    payload_release(me);
    console_end();
}

/** @brief Abandon a payload mid-flight, naming the verb that died. */
static void payload_fail(Console *me, kcli_err_t code)
{
    console_reply_err(code, console_verb());
    payload_release(me);
    console_end();
}

/**
 * @brief One pass of the producer: emit a slice, then decide what comes next.
 *
 * The watchdog measures progress, not total time: a slow transfer may take
 * as long as it needs, and one whose source died still closes with code 6.
 */
static void payload_pump(Console *me)
{
    uint32_t const before = me->pay.total;
    bool     const drained = payload_slice(me);

    if ( me->pay.total > before )
    {
        /* Re-armed on bytes moved, not passes: a peer that stopped listening
         * must still time out. */
        (void)QTimeEvt_rearm(&me->tmo, KQP_TICKS_PER_MS(KCLI_CMD_TIMEOUT_MS));
    }

    if ( !drained || console_congested() )
    {
        /* Congested and drained still waits: a terminator posted into a full
         * ring would leave the host without one. */
        QTimeEvt_armX(&me->pacer, KCLI_SLICE_GAP_TICKS, 0U);
    }
    else if ( (uint8_t)KCLI_FILE == me->inflight )
    {
        /* This chunk is spent. Handing the buffer back *is* the request for
         * the next one, so the read never runs ahead of the wire and the
         * back-pressure needs no mechanism of its own. */
        me->pay.borrowed = false;
        storage_next_request(me->pay.idx);
    }
    else
    {
        payload_done(me);
    }
}

/* ---- Handlers: screenshot, file_get --------------------------------------- */

/* The slots of `A_SHOT`, in the order its table declares the keys, and the
 * members of `EN_SHOT_DST`. They are the table's own positions: a key added
 * there goes at the end, or every slot below it moves. */
#define SHOT_ARG_DST    0U   /**< `dst`.                                    */
#define SHOT_ARG_NAME   1U   /**< `name`.                                   */
#define SHOT_ARG_TOKEN  2U   /**< `token`.                                  */
#define SHOT_ARG_FPS    3U   /**< `fps`.                                    */
#define SHOT_ARG_SECS   4U   /**< `secs`.                                   */
#define SHOT_ARG_STOP   5U   /**< `stop`.                                   */
#define SHOT_DST_FILE   1U   /**< `dst=file`.                               */
#define SHOT_DST_HTTP   2U   /**< `dst=http`; the enum has it only when the
                              *   build has the upload.                     */

#if CONFIG_KODEOS_SHOT_HTTP

/**
 * @brief screenshot dst=http stop=on: ask AO_Net to end the running stream.
 *
 * A stop names nothing else: `token`, `fps`, `secs` and `name` with it, or
 * `stop=off`, are a bad value for `stop`. AO_Net answers at once, `>ok` when a
 * stream ended and an error when none was running.
 */
static void shot_http_stop(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    if ( (!v[SHOT_ARG_STOP].b) || v[SHOT_ARG_TOKEN].present
        || v[SHOT_ARG_FPS].present || v[SHOT_ARG_SECS].present
        || v[SHOT_ARG_NAME].present )
    {
        console_reply_err(KCLI_ERR_VALUE, "stop");
    }
    else
    {
        cmd_begin(me, KCLI_SHOT_HTTP, c->verb);
        console_extend(KCLI_SHOT_HTTP_TIMEOUT_MS);
        net_shot_stop_request(&me->super);
    }
}

/**
 * @brief screenshot dst=http: check the order and hand it to AO_Net.
 *
 * Open to every source: the picture does not come down this wire. The token
 * is checked here too, so the caller gets a reason. `fps` or `secs` makes a
 * stream (the other takes its default); neither is one capture.
 */
static void shot_http_order(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    if ( !v[SHOT_ARG_TOKEN].present )
    {
        console_reply_err(KCLI_ERR_MISSING, "token");
    }
    else if ( v[SHOT_ARG_NAME].present )
    {
        console_reply_err(KCLI_ERR_VALUE, "name");
    }
    else if ( !kshot_token_valid(v[SHOT_ARG_TOKEN].s) )
    {
        console_reply_err(KCLI_ERR_VALUE, "token");
    }
    else
    {
        uint8_t  fps  = 0U;
        uint16_t secs = 0U;

        if ( v[SHOT_ARG_FPS].present || v[SHOT_ARG_SECS].present )
        {
            fps  = (uint8_t)KSHOT_FPS_DEFAULT;
            secs = (uint16_t)KSHOT_SECS_DEFAULT;
            if ( v[SHOT_ARG_FPS].present )
            {
                fps = (uint8_t)v[SHOT_ARG_FPS].u;
            }
            if ( v[SHOT_ARG_SECS].present )
            {
                secs = (uint16_t)v[SHOT_ARG_SECS].u;
            }
        }
        cmd_begin(me, KCLI_SHOT_HTTP, c->verb);
        console_extend(KCLI_SHOT_HTTP_TIMEOUT_MS);
        net_shot_request(&me->super, v[SHOT_ARG_TOKEN].s, fps, secs);
    }
}

/**
 * @brief Print how AO_Net ended an upload order, and close the command.
 *
 * One line a case, the spec's table: a capture taken is a record and `>ok`, a
 * refusal is a record carrying the server's code and then the error, and the
 * rest are errors on their own. A stream is accepted here, not finished: its
 * record is the rate and length AO_Net will use.
 *
 * @param r The answer. Its id is read as a text that may lack a terminator.
 */
static void shot_http_answer(NetShotRspEvt const *r)
{
    char id[KSHOT_ID_MAX + 1U];
    char qid[CONSOLE_QUOTED_MAX(KSHOT_ID_MAX)];

    (void)memcpy(id, r->id, KSHOT_ID_MAX);
    id[KSHOT_ID_MAX] = '\0';

    switch ( r->result )
    {
        case (uint8_t)KSHOT_R_OK:
            console_reply(">|screenshot dst=http bytes=%lu id=%s seq=%lu",
                  (unsigned long)r->bytes, console_kv_text(qid, sizeof(qid), id),
                  (unsigned long)r->seq);
            console_reply(">ok");
            break;
        case (uint8_t)KSHOT_R_REFUSED:
            console_reply(">|screenshot dst=http result=refused http=%u",
                  (unsigned)r->http);
            console_reply_err(KCLI_ERR_REJECT, "http");
            break;
        case (uint8_t)KSHOT_R_NO_ANSWER:
            console_reply_err(KCLI_ERR_TIMEOUT, "http");
            break;
        case (uint8_t)KSHOT_R_BUSY:
            console_reply_err(KCLI_ERR_BUSY, "busy");
            break;
        case (uint8_t)KSHOT_R_SIZE:
            console_reply_err(KCLI_ERR_REJECT, "size");
            break;
        case (uint8_t)KSHOT_R_CHANNEL:
            console_reply_err(KCLI_ERR_REJECT, "channel");
            break;
        case (uint8_t)KSHOT_R_STREAM:
            console_reply(">|screenshot dst=http mode=stream fps=%u secs=%u",
                  (unsigned)r->fps, (unsigned)r->secs);
            console_reply(">ok");
            break;
        case (uint8_t)KSHOT_R_STOPPED:
            console_reply(">ok");
            break;
        case (uint8_t)KSHOT_R_NO_STREAM:
            console_reply_err(KCLI_ERR_REJECT, "stream");
            break;
        default:
            console_reply_err(KCLI_ERR_REJECT, "http");
            break;
    }
}

#endif /* CONFIG_KODEOS_SHOT_HTTP */

/**
 * @brief screenshot: capture the panel, then send it, save it or upload it.
 *
 * `dst=console` and `dst=file` first freeze a copy of the panel; the in-flight
 * kind says which runs. `dst=http` asks AO_Net, which gets a JPEG itself.
 * `token`, `fps`, `secs` and `stop` belong to `dst=http` alone; `stop=on` ends
 * the running stream.
 */
static void h_screenshot(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    bool const to_file = v[SHOT_ARG_DST].present
                         && (SHOT_DST_FILE == v[SHOT_ARG_DST].u);
#if CONFIG_KODEOS_SHOT_HTTP
    bool const to_http = v[SHOT_ARG_DST].present
                         && (SHOT_DST_HTTP == v[SHOT_ARG_DST].u);
#else
    bool const to_http = false;
#endif

    if ( !to_http )
    {
        if ( v[SHOT_ARG_TOKEN].present )
        {
            console_reply_err(KCLI_ERR_VALUE, "token");
            return;
        }
        if ( v[SHOT_ARG_FPS].present )
        {
            console_reply_err(KCLI_ERR_VALUE, "fps");
            return;
        }
        if ( v[SHOT_ARG_SECS].present )
        {
            console_reply_err(KCLI_ERR_VALUE, "secs");
            return;
        }
        if ( v[SHOT_ARG_STOP].present )
        {
            console_reply_err(KCLI_ERR_VALUE, "stop");
            return;
        }
    }
#if CONFIG_KODEOS_SHOT_HTTP
    if ( to_http )
    {
        if ( console_owner_absent(&kaos_net_start) )
        {
            return;
        }
        if ( v[SHOT_ARG_STOP].present )
        {
            shot_http_stop(me, c, v);
        }
        else
        {
            shot_http_order(me, c, v);
        }
        return;
    }
#endif

    if ( console_owner_absent(&kaos_display_start) )
    {
        return;
    }
    if ( to_file )
    {
        if ( console_owner_absent(&kaos_storage_start) )
        {
            return;
        }
    }
    /* `dst` defaults to the console: the frame as records. A file answers in
     * a few lines and stays open to every source. */
    if ( (!to_file) && (!console_src_allows(CONSOLE_NEED_WIRE)) )
    {
        return;
    }

    const char *name = "shot.bmp";
    if ( v[SHOT_ARG_NAME].present )
    {
        name = v[SHOT_ARG_NAME].s;
    }
    (void)snprintf(me->file_name, sizeof(me->file_name), "%s",
                   name);
    me->pay.total    = 0U;
    me->pay.borrowed = false;
    me->pay.n_carry  = 0U;

    cmd_begin(me, to_file ? KCLI_SHOT_FILE : KCLI_SHOT, c->verb);
    if ( to_file )
    {
        /* AO_Storage answers once, and a bad card can spend ~3.5 s failing
         * its first write before the fallback to the internal partition. */
        console_extend(KCLI_SHOT_FILE_TIMEOUT_MS);
    }
    display_capture_request(&me->super);
}

/** @brief file_get: any file on either medium, as base64 records. */
static void h_file_get(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    if ( console_owner_absent(&kaos_storage_start) )
    {
        return;
    }
    if ( !console_src_allows(CONSOLE_NEED_WIRE) )
    {
        return;
    }
    (void)snprintf(me->file_name, sizeof(me->file_name), "%s", v[0].s);
    me->pay.total    = 0U;
    me->pay.borrowed = false;
    me->pay.n_carry  = 0U;

    cmd_begin(me, KCLI_FILE, c->verb);
    storage_open_request(v[0].s, &me->super);
}

/* ---- Handlers: halt and catch fire --------------------------------------- */

/**
 * @brief Fire a QP assertion on purpose. Resets the board. No terminator:
 *        the reset IS the answer, and any line posted here would die in the
 *        queue when Q_ERROR() kills the system.
 *
 * Normal use never reaches the assert path: Q_onError runs in a critical
 * section and prints through the lock-free ROM path, and a regression there
 * would be a silent reboot.
 */
static void h_halt_and_catch_fire(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)me; (void)c; (void)v;
    Q_ERROR();
}

/* ---- Handlers: self-tests ------------------------------------------------
 * AO_Autotest owns the rows and the run. This AO prints what it publishes: a
 * `>|test=` record as each row ends, a guided row's steps, and one summary
 * record. The `>ok` follows the last row.
 */

/** @brief `dst=`: where an image goes. Mirrors kfw_target_t's order. */
static const char *const EN_DST[] = { "os", "app", "c5", "pending" };

/**
 * @brief `src=`: the prefixes, not the values.
 *
 * A source is a kind plus a locator (`fs:kodeos.kfw`), so it is a string and
 * these are its prefixes; `pending` and `terminal` carry no locator. A bare
 * name means the media, card first.
 */
static const char *const EN_SRC[] = {
    "fs:", "sd:", "http://", "https://", "pending", "terminal"
};

/** @brief The `component=` enum: the names of the self-test rows, plus the
 *         group words `main` and `all`. The guards here are the ones the
 *         rows carry: a word with no row behind it - its owner not linked
 *         among them - is refused by AO_Autotest. */
static const char *const EN_COMPONENT[] = {
    "led", "screenshot",
#if CONFIG_KODEOS_MIRROR
    "jpeg",
#endif
    "fs", "sd",
#if CONFIG_KODEOS_RADIO
    "c5",
#endif
    "pmic", "gauge", "ioexp", "clocks", "imu", "mag", "ir", "haptic", "nfc",
    "rfid",
    "audio",
    "display",
    "buttons", "tp",
    "psram",
    "mem",
    "boot",
    "apps", "kfw", "id",
#if CONFIG_KODEOS_RADIO
#if CONFIG_KODEOS_BLE_CONSOLE
    "ble_console",
#endif
#if CONFIG_KODEOS_NET
    "net",
#endif
#endif
    "sd_user",
#if CONFIG_KODEOS_RADIO
    "c5_user",
#endif
    "nfc_field", "rfid_field", "power_plug", "nfc_card", "rfid_card",
    "main", "all", "user",
};

/** @brief The word each AUTOTEST_ROW_* state prints as; a row that is still
 *         running prints nothing. */
static const char *const TEST_STATE_WORD[AUTOTEST_ROW_STATES] = {
    [AUTOTEST_ROW_RUNNING] = "running",
    [AUTOTEST_ROW_PASS]    = "pass",
    [AUTOTEST_ROW_FAIL]    = "fail",
    [AUTOTEST_ROW_TIMEOUT] = "timeout",
    [AUTOTEST_ROW_SKIPPED] = "skipped",
};

/**
 * @brief Note one row that ended, for the verdict block.
 *
 * @param me    The console.
 * @param id    AUTOTEST_ID_*.
 * @param state AUTOTEST_ROW_*.
 */
static void test_keep(Console *me, uint8_t id, uint8_t state)
{
    if ( AUTOTEST_RUN_MAX > me->test_kept )
    {
        me->test_id[me->test_kept]    = id;
        me->test_state[me->test_kept] = state;
        me->test_kept += 1U;
    }
}

/**
 * @brief Print the rows of one state, as lines of the verdict block.
 *
 * @param me    The console.
 * @param state AUTOTEST_ROW_*: the rows to print.
 */
static void test_verdicts_of(const Console *me, uint8_t state)
{
    for ( uint8_t i = 0U; i < me->test_kept; ++i )
    {
        uint8_t const id = me->test_id[i];

        if ( me->test_state[i] != state )
        {
            continue;
        }
        if ( AUTOTEST_ROW_FAIL == state )
        {
            KLOG_E(TAG, "  FAIL       %-11s %s - %s", autotest_row_name(id),
                   autotest_row_label(id), autotest_row_problem(id));
        }
        else if ( AUTOTEST_ROW_TIMEOUT == state )
        {
            KLOG_W(TAG, "  NO ANSWER  %-11s %s", autotest_row_name(id),
                   autotest_row_label(id));
        }
        else if ( AUTOTEST_ROW_SKIPPED == state )
        {
            KLOG_N(TAG, "  skipped    %-11s %s", autotest_row_name(id),
                   autotest_row_label(id));
        }
        else
        {
            KLOG_N(TAG, "  pass       %-11s %s", autotest_row_name(id),
                   autotest_row_label(id));
        }
    }
}

/**
 * @brief The verdict block: what a run came to, read at a glance.
 *
 * Log lines for a person; scripts parse the records. A headline, then the
 * failed rows (errors), the unjudged (warnings), the skipped, the passed, and
 * a count. Printed after `test` and `test_report`.
 *
 * @param me The console, holding the rows that ended.
 */
static void test_verdicts_print(const Console *me)
{
    uint8_t count[AUTOTEST_ROW_STATES];

    (void)memset(count, 0, sizeof(count));
    for ( uint8_t i = 0U; i < me->test_kept; ++i )
    {
        if ( AUTOTEST_ROW_STATES > me->test_state[i] )
        {
            count[me->test_state[i]] += 1U;
        }
    }
    if ( 0U != count[AUTOTEST_ROW_FAIL] )
    {
        KLOG_E(TAG, "==================== TEST: %u FAILED ====================",
               (unsigned)count[AUTOTEST_ROW_FAIL]);
    }
    else if ( 0U != count[AUTOTEST_ROW_TIMEOUT] )
    {
        KLOG_W(TAG, "================ TEST: %u NOT JUDGED, none failed ================",
               (unsigned)count[AUTOTEST_ROW_TIMEOUT]);
    }
    else
    {
        KLOG_N(TAG, "==================== TEST: ALL PASSED ====================");
    }
    test_verdicts_of(me, AUTOTEST_ROW_FAIL);
    test_verdicts_of(me, AUTOTEST_ROW_TIMEOUT);
    test_verdicts_of(me, AUTOTEST_ROW_SKIPPED);
    test_verdicts_of(me, AUTOTEST_ROW_PASS);
    KLOG_N(TAG, "=========== %u pass, %u fail, %u no answer, %u skipped ===========",
           (unsigned)count[AUTOTEST_ROW_PASS], (unsigned)count[AUTOTEST_ROW_FAIL],
           (unsigned)count[AUTOTEST_ROW_TIMEOUT],
           (unsigned)count[AUTOTEST_ROW_SKIPPED]);
}

/**
 * @brief test_report: the report kept from the last whole `user` run.
 *
 * One `>|row=` record a row, then one record with when it ran and how many
 * rows were problems - or `>|report=none` when no `user` run has ever
 * finished on this unit. AO_Autotest keeps it and reads it back.
 */
static void h_test_report(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    (void)v;
    me->test_kept = 0U;
    cmd_begin(me, KCLI_TEST_REPORT, c->verb);
    autotest_report_request(AUTOTEST_CLIENT_CONSOLE);
}

/**
 * @brief test [component=<name>|main|all|user]: run the self-tests.
 *
 * `all` (the default) is AO_TestDot's E5 final test, cable only; its verbs own
 * the line from test_dot_run_request() on (`msg=not_built` without
 * CONFIG_KODE_TEST_DOT). `main` is what a bare main PCB answers, `user` what
 * the settings screen runs, and a row's name runs that row, on AO_Autotest.
 */
static void h_test(Console *me, const kcmd_t *c, const karg_val_t *v)
{
    uint8_t group = AUTOTEST_G_NONE;
    const char *sel = NULL;

    if ( !v[0].present || (strcmp(EN_COMPONENT[v[0].u], "all") == 0) )
    {
#if CONFIG_KODE_TEST_DOT
        if ( console_src_allows(CONSOLE_NEED_CABLE) )
        {
            me->owner = &test_dot_verbs;
            test_dot_run_request();
        }
#else
        console_reply_err(KCLI_ERR_REJECT, "not_built");
#endif
        return;
    }
    if ( strcmp(EN_COMPONENT[v[0].u], "main") == 0 )
    {
        group = AUTOTEST_G_MAIN;
    }
    else if ( strcmp(EN_COMPONENT[v[0].u], "user") == 0 )
    {
        group = AUTOTEST_G_USER;
    }
    else
    {
        sel = EN_COMPONENT[v[0].u];
    }
    me->test_wait_begin = true;
    me->test_rows = 0U;
    me->test_seen = 0U;
    me->test_kept = 0U;
    cmd_begin(me, KCLI_TEST, c->verb);
    autotest_run_request(AUTOTEST_CLIENT_CONSOLE, group, sel);
}

/* ---- Command table + dispatch -------------------------------------------
 * One verb per action. The order is the order `help` prints.
 */

const karg_spec_t console_state_args[CONSOLE_STATE_ARGS_N] = {
    { .key = "state", .required = true, .type = KARG_BOOL },
};

/** @brief `sd_verify`: destructive, so it is spelled out rather than assumed. */
const karg_spec_t console_confirm_args[CONSOLE_CONFIRM_ARGS_N] = {
    { .key = "confirm", .required = true, .type = KARG_BOOL },
};
/* The field verbs: both keys optional, and `value` only meaningful with
 * `field` - the handler says so. The ceiling is int32's: every field of every
 * chip fits, and the driver refuses anything outside the field's own range. */
const karg_spec_t console_field_args[CONSOLE_FIELD_ARGS_N] = {
    { .key = "field", .required = false, .type = KARG_STRING },
    { .key = "value", .required = false, .type = KARG_UINT,
      .min = 0U, .max = 0x7FFFFFFFU },
};
/* `variable name= [watch=]`. There is no `value`: a variable is read-only,
 * and changing what one shows is a function. `watch` is bounded here so the
 * handler only has the floor to refuse. */
static const karg_spec_t A_VARIABLE[] = {
    { .key = "name",  .required = true,  .type = KARG_STRING },
    { .key = "watch", .required = false, .type = KARG_UINT,
      .min = 0U, .max = KODE_VARIABLE_WATCH_MAX_MS },
};
_Static_assert(KODE_VARIABLE_WATCH_MAX_MS <= UINT16_MAX,
               "watch period fits kcli_watch_t.period_ms");
/* `function name= [arg=]`. `arg` is a KARG_ROW: only the row knows its type,
 * so h_function() checks the form. */
static const karg_spec_t A_FUNCTION[] = {
    { .key = "name", .required = true,  .type = KARG_STRING },
    { .key = "arg",  .required = false, .type = KARG_ROW    },
};
/* `config [key=] [value=]`. `value` is a KARG_ROW for the reason `arg` is:
 * only the setting's row knows its type. */
static const karg_spec_t A_CONFIG[] = {
    { .key = "key",   .required = false, .type = KARG_STRING },
    { .key = "value", .required = false, .type = KARG_ROW    },
};
/** @brief `exp mode=`: what a position becomes. Order is khal_exp_mode_t's
 *         minus LENT, which is not something a bench can ask for. */
static const char *const EN_EXP_MODE[] = { "free", "in", "out", "adc", "pwm" };
/** @brief `exp pull=`: khal_gpio_pull_t's own order, which is what lets the
 *         handler cast the parsed index straight to it. */
static const char *const EN_EXP_PULL[] = { "none", "up", "down" };
_Static_assert((int)KHAL_GPIO_PULL_NONE == 0, "EN_EXP_PULL[0] is none");
_Static_assert((int)KHAL_GPIO_PULL_UP   == 1, "EN_EXP_PULL[1] is up");
_Static_assert((int)KHAL_GPIO_PULL_DOWN == 2, "EN_EXP_PULL[2] is down");

/* `exp [pos= mode= pull= level= hz= duty=]`. No keys prints the table; a
 * `pos` with a `mode` acts on one position. `pull` goes with `mode=in`,
 * `level` with `mode=out`, `hz`/`duty` with `mode=pwm`. */
static const karg_spec_t A_EXP[] = {
    { .key = "pos",   .required = false, .type = KARG_UINT,
      .min = KHAL_EXP_POS_MIN, .max = KHAL_EXP_POS_MAX },
    { .key = "mode",  .required = false, .type = KARG_ENUM,
      .enums = EN_EXP_MODE,
      .n_enums = (uint8_t)(sizeof(EN_EXP_MODE) / sizeof(EN_EXP_MODE[0])) },
    { .key = "pull",  .required = false, .type = KARG_ENUM,
      .enums = EN_EXP_PULL,
      .n_enums = (uint8_t)(sizeof(EN_EXP_PULL) / sizeof(EN_EXP_PULL[0])),
      .def = "none" },
    { .key = "level", .required = false, .type = KARG_UINT, .min = 0U, .max = 1U,
      .def = "0" },
    { .key = "hz",    .required = false, .type = KARG_UINT,
      .min = KHAL_EXP_PWM_HZ_MIN, .max = KHAL_EXP_PWM_HZ_MAX,
      .def = CONSOLE_STRINGIFY(KCLI_EXP_HZ_DEF_N) },
    { .key = "duty",  .required = false, .type = KARG_UINT,
      .min = 0U, .max = KHAL_EXP_PWM_DUTY_MAX,
      .def = CONSOLE_STRINGIFY(KCLI_EXP_DUTY_DEF_N) },
};
/* The widest spec in the table, and cmd_validate() indexes its value array
 * by slot - so this is the one that would run off the end of console_exec()'s
 * stack frame if KCLI_MAX_KEYS ever fell behind it. */
_Static_assert((sizeof(A_EXP) / sizeof(A_EXP[0])) <= (size_t)KCLI_MAX_KEYS,
               "A_EXP declares more keys than cmd_validate() has slots for");

static const karg_spec_t A_NAME[] = {
    { .key = "name", .required = true, .type = KARG_STRING },
};
static const karg_spec_t A_APPDATA_GET[] = {
    { .key = "name", .required = true, .type = KARG_STRING },
    { .key = "url",  .required = true, .type = KARG_STRING },
};
/* `ls` may be typed bare (the cwd). `cd` and `stat` need a path, so a
 * mistyped key is not taken as the cwd. */
static const karg_spec_t A_PATH_OPT[] = {
    { .key = "path", .required = false, .type = KARG_STRING },
};
static const karg_spec_t A_PATH[] = {
    { .key = "path", .required = true, .type = KARG_STRING },
};
/* `cat`'s ceiling and default live in kqp_config.h, tied to AO_Storage's
 * read buffer. */
#define KCLI_CAT_MAX_DEF_N  4096  /**< KODE_FS_CAT_MAX_DEFAULT, bare for `help`. */
_Static_assert((uint32_t)KCLI_CAT_MAX_DEF_N == KODE_FS_CAT_MAX_DEFAULT, "cat's printed default is the applied one");
static const karg_spec_t A_CAT[] = {
    { .key = "path", .required = true,  .type = KARG_STRING },
    { .key = "max",  .required = false, .type = KARG_UINT,
      .min = 1U, .max = KODE_FS_CAT_MAX_CEIL,
      .def = CONSOLE_STRINGIFY(KCLI_CAT_MAX_DEF_N) },
};
/* `mv src= dst=` and `cp src= dst=` share one spec. */
static const karg_spec_t A_PAIR[] = {
    { .key = "src", .required = true, .type = KARG_STRING },
    { .key = "dst", .required = true, .type = KARG_STRING },
};
/* `text=""` (an empty file) is a value. `append`'s default is spelled out so
 * `help` prints it. `line` fits StorageTextEvt.line (uint16_t); 0 means the
 * whole file. */
static const karg_spec_t A_WRITE[] = {
    { .key = "path",   .required = true,  .type = KARG_STRING },
    { .key = "append", .required = false, .type = KARG_BOOL, .def = "off" },
    { .key = "line",   .required = false, .type = KARG_UINT,
      .min = 0U, .max = 65535U, .def = "0" },
    { .key = "text",   .required = true,  .type = KARG_TEXT   },
};
/* `file_put path= size= crc32=`. No `format=`: the bytes are stored as they
 * are. `size` starts at 1: an empty transfer would wait on a completion that
 * never fires. `write` is the verb for an empty file. */
static const karg_spec_t A_FILE_PUT[] = {
    { .key = "path",  .required = true, .type = KARG_STRING },
    { .key = "size",  .required = true, .type = KARG_UINT,
      .min = 1U, .max = 0xFFFFFFFFU },
    { .key = "crc32", .required = true, .type = KARG_UINT,
      .min = 0U, .max = 0xFFFFFFFFU },
};
/* `dst` follows `install dst=app|os|c5`: one verb, and the destination says
 * which half of it runs. */
static const char *const EN_SHOT_DST[] = {
    "console", "file",
#if CONFIG_KODEOS_SHOT_HTTP
    "http",
#endif
};
/* The positions are SHOT_ARG_*'s. `token`, `fps`, `secs` and `stop` are
 * `dst=http`'s: every other destination refuses them, and a build without the
 * upload has no `http` to give them a meaning. */
static const karg_spec_t A_SHOT[] = {
    { .key = "dst", .required = false, .type = KARG_ENUM,
      .enums = EN_SHOT_DST,
      .n_enums = (uint8_t)(sizeof(EN_SHOT_DST) / sizeof(EN_SHOT_DST[0])),
      .def = "console" },
    { .key = "name", .required = false, .type = KARG_STRING,
      .def = "shot.bmp" },
    { .key = "token", .required = false, .type = KARG_STRING },
    { .key = "fps", .required = false, .type = KARG_UINT,
      .min = 1U, .max = KSHOT_FPS_MAX },
    { .key = "secs", .required = false, .type = KARG_UINT,
      .min = 1U, .max = KSHOT_SECS_MAX },
    { .key = "stop", .required = false, .type = KARG_BOOL },
};
/* `install src= dst= [run=] [size= crc32=] [name=]`. `size`, `crc32` and
 * `name` are optional to the parser: h_install requires or refuses them by
 * `src`/`dst`. `run`'s default is spelled out so `help` prints it. */
static const karg_spec_t A_INSTALL[] = {
    { .key = "src", .required = true,  .type = KARG_STRING,
      .enums = EN_SRC,
      .n_enums = (uint8_t)(sizeof(EN_SRC) / sizeof(EN_SRC[0])) },
    { .key = "dst", .required = true,  .type = KARG_ENUM,
      .enums = EN_DST,
      .n_enums = (uint8_t)(sizeof(EN_DST) / sizeof(EN_DST[0])) },
    { .key = "run", .required = false, .type = KARG_BOOL, .def = "off" },
    { .key = "size",  .required = false, .type = KARG_UINT,
      .min = 1U, .max = 0xFFFFFFFFU },
    { .key = "crc32", .required = false, .type = KARG_UINT,
      .min = 0U, .max = 0xFFFFFFFFU },
    { .key = "name",  .required = false, .type = KARG_STRING },
};
static const karg_spec_t A_LOG_LEVEL[] = {
    { .key = "tag",   .required = true, .type = KARG_STRING },
    { .key = "level", .required = true, .type = KARG_ENUM,
    .enums = EN_LEVEL,
    .n_enums = (uint8_t)(sizeof(EN_LEVEL) / sizeof(EN_LEVEL[0])) },
};
static const karg_spec_t A_NVS_SET[] = {
    { .key = "key",   .required = true, .type = KARG_STRING },
    { .key = "value", .required = true, .type = KARG_STRING },
};
static const karg_spec_t A_NVS_KEY[] = {
    { .key = "key", .required = true, .type = KARG_STRING },
};
static const karg_spec_t A_TIME[] = {
    { .key = "set",  .required = false, .type = KARG_STRING },
    { .key = "sync", .required = false, .type = KARG_BOOL },
};
static const karg_spec_t A_TEST[] = {
    { .key = "component", .required = false, .type = KARG_ENUM,
      .enums = EN_COMPONENT,
      .n_enums = (uint8_t)(sizeof(EN_COMPONENT) / sizeof(EN_COMPONENT[0])),
      .def = "all" },
};

/* The panel's bounds (410x502): outside is refused, never clamped, which
 * would invent a tap. A_TOUCH_STATES is with the injected-finger handlers. */
static const karg_spec_t A_TOUCH[] = {
    { .key = "x", .required = true, .type = KARG_UINT,
      .min = 0U, .max = (uint32_t)(KBSP_LCD_H_RES - 1) },
    { .key = "y", .required = true, .type = KARG_UINT,
      .min = 0U, .max = (uint32_t)(KBSP_LCD_V_RES - 1) },
    { .key = "state", .required = true, .type = KARG_ENUM,
      .enums = A_TOUCH_STATES, .n_enums = 3U },
};
#if !CONFIG_KODE_SCHEDULER
/* `cron [name=] [when=] [cmd=]`, declared for the build without the scheduler,
 * so every form of the verb answers "not built" and none is refused by the
 * validator first. `when` is a KARG_ROW: quoted for an expression, bare for `off`. */
static const karg_spec_t A_CRON[] = {
    { .key = "name", .required = false, .type = KARG_STRING },
    { .key = "when", .required = false, .type = KARG_ROW    },
    { .key = "cmd",  .required = false, .type = KARG_STRING },
};
#endif

static const karg_spec_t A_TAP[] = {
    { .key = "x", .required = true, .type = KARG_UINT,
      .min = 0U, .max = (uint32_t)(KBSP_LCD_H_RES - 1) },
    { .key = "y", .required = true, .type = KARG_UINT,
      .min = 0U, .max = (uint32_t)(KBSP_LCD_V_RES - 1) },
};
/* `swipe x0= y0= x1= y1= ms= [hold=]`: down point, lift point, trip time and
 * rest at the end. Out of range is refused, never clamped. */
static const karg_spec_t A_SWIPE[] = {
    { .key = "x0", .required = true, .type = KARG_UINT,
      .min = 0U, .max = KBSP_LCD_H_RES - 1U },
    { .key = "y0", .required = true, .type = KARG_UINT,
      .min = 0U, .max = KBSP_LCD_V_RES - 1U },
    { .key = "x1", .required = true, .type = KARG_UINT,
      .min = 0U, .max = KBSP_LCD_H_RES - 1U },
    { .key = "y1", .required = true, .type = KARG_UINT,
      .min = 0U, .max = KBSP_LCD_V_RES - 1U },
    { .key = "ms", .required = true, .type = KARG_UINT,
      .min = KCLI_SWIPE_MIN_MS, .max = KCLI_SWIPE_MAX_MS },
    { .key = "hold", .required = false, .type = KARG_UINT,
      .min = 0U, .max = KCLI_SWIPE_HOLD_MAX_MS, .def = "0" },
};
_Static_assert((sizeof(A_SWIPE) / sizeof(A_SWIPE[0])) <= (size_t)KCLI_MAX_KEYS,
               "A_SWIPE declares more keys than cmd_validate() has slots for");
/* A_BTN_IDS/A_BTN_STATES are defined with the injected-finger handlers
 * above, which log the same words. */
static const karg_spec_t A_BTN[] = {
    { .key = "id", .required = true, .type = KARG_ENUM,
      .enums = A_BTN_IDS, .n_enums = 6U },
    { .key = "state", .required = true, .type = KARG_ENUM,
      .enums = A_BTN_STATES, .n_enums = A_BTN_N_STATES },
};

static const kcmd_t CMDS[] = {
    { "info",       NULL,        0U, h_info       },
    { "help",       NULL,        0U, h_help       },
    { "ping",       NULL,        0U, h_ping       },
    { "ps",         NULL,        0U, h_ps         },
    { "tasks",      NULL,        0U, h_tasks      },
    { "hcf",        NULL,        0U, h_halt_and_catch_fire },
    { "ls",         A_PATH_OPT,  1U, h_ls         },
    { "cd",         A_PATH,      1U, h_cd         },
    { "pwd",        NULL,        0U, h_pwd        },
    { "stat",       A_PATH,      1U, h_stat       },
    { "cat",        A_CAT,       2U, h_cat        },
    { "mkdir",      A_PATH,      1U, h_mkdir      },
    { "rm",         A_PATH,      1U, h_rm         },
    { "rmdir",      A_PATH,      1U, h_rmdir      },
    { "mv",         A_PAIR,      2U, h_mv         },
    { "cp",         A_PAIR,      2U, h_cp         },
    { "write",      A_WRITE,     4U, h_write      },
    { "file_put",   A_FILE_PUT,  3U, h_file_put   },
    { "file_get",   A_NAME,      1U, h_file_get   },
    { "screenshot", A_SHOT,      6U, h_screenshot },
    { "install",    A_INSTALL,   6U, h_install    },
    { "appdata_ls",   NULL,      0U, h_appdata_ls   },
    { "appdata_info", A_NAME,    1U, h_appdata_info },
    { "appdata_rm",   A_NAME,    1U, h_appdata_rm   },
    { "appdata_get",  A_APPDATA_GET, 2U, h_appdata_get },
    { "variable",      A_VARIABLE, 2U, h_variable      },
    { "variable_list", NULL,       0U, h_variable_list },
    { "function",        A_FUNCTION,   2U, h_function        },
    { "function_list",   NULL,       0U, h_function_list   },
    { "config",          A_CONFIG,     2U, h_config          },
    { "btn_read",   NULL,        0U, h_btn_read   },
    { "touch",      A_TOUCH,     3U, h_touch      },
    { "tap",        A_TAP,       2U, h_tap        },
    { "swipe",      A_SWIPE,     6U, h_swipe      },
    { "btn",        A_BTN,       2U, h_btn        },
    { "bat_info",   NULL,        0U, h_bat_info   },
#if !CONFIG_KODE_LOWPOWER
    /* AO_LowPower registers them when it is built (lowpower_verbs.c). */
    { "sleep",      NULL,        0U, h_not_built  },
    { "power_off",  NULL,        0U, h_not_built  },
#endif
#if !CONFIG_KODE_SCHEDULER
    /* AO_Scheduler registers `cron` when it is built (scheduler_verbs.c). */
    { "cron",       A_CRON,      3U, h_not_built  },
#endif
    { "ship_mode",  NULL,        0U, h_ship_mode  },
    { "pmic",       console_field_args, 2U, h_pmic       },
    { "gauge",      console_field_args, 2U, h_gauge      },
    { "pmic_help",  NULL,        0U, h_pmic_help  },
    { "gauge_help", NULL,        0U, h_gauge_help },
    { "ioexp",      console_field_args, 2U, h_ioexp      },
    { "ioexp_help", NULL,        0U, h_ioexp_help },
    { "boost",        console_state_args, 1U, h_boost        },
    { "pmic_reset",   NULL,    0U, h_pmic_reset   },
    { "gui_info",   NULL,        0U, h_gui_info   },
    { "clocks",     NULL,        0U, h_clocks     },
    { "temp",       NULL,        0U, h_temp       },
    { "i2c_scan",   NULL,        0U, h_i2c_scan   },
    { "exp",        A_EXP,       6U, h_exp        },
    { "exp_help",   NULL,        0U, h_exp_help   },
    { "time",       A_TIME,      2U, h_time       },
    { "boot_info",  NULL,        0U, h_boot_info  },
    { "restart",    NULL,        0U, h_restart    },
    { "nvs_set",    A_NVS_SET,   2U, h_nvs_set    },
    { "nvs_get",    A_NVS_KEY,   1U, h_nvs_get    },
    { "nvs_del",    A_NVS_KEY,   1U, h_nvs_del    },
    { "nvs_list",   NULL,        0U, h_nvs_list   },
#if CONFIG_KODEOS_BOOT_ROLE_OS
    { "factory_reset", console_confirm_args, CONSOLE_CONFIRM_ARGS_N, h_factory_reset },
#endif
    { "log_level",  A_LOG_LEVEL, 2U, h_log_level  },
    { "log_color",  console_state_args, 1U, h_log_color  },
    { "test",       A_TEST,      1U, h_test       },
    { "test_report", NULL,       0U, h_test_report },
};

bool console_system_verb(const char *verb)
{
    bool found = false;

    for ( size_t c = 0U; (NULL != verb) && ((sizeof(CMDS) / sizeof(CMDS[0])) > c); ++c )
    {
        if ( strcmp(CMDS[c].verb, verb) == 0 )
        {
            found = true;
        }
    }
    return found;
}

bool console_specs_ok(void)
{
    for ( size_t c = 0U; (sizeof(CMDS) / sizeof(CMDS[0])) > c; ++c )
    {
        /* A row with more keys than KCLI_MAX_KEYS would make cmd_validate()
         * write past console_exec()'s stack frame. This checks every row. */
        if ( (size_t)CMDS[c].n_args > (size_t)KCLI_MAX_KEYS )
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief Hand a line back to the sender that holds it, when the console is busy.
 *
 * The scheduler's lines and the Kode channel's have no port to be refused on:
 * AO_Scheduler and AO_Net give the line again shortly.
 *
 * @param src The line's source (a KodeConsoleSrc).
 * @return true when @p src is one that holds its line and was told to.
 */
static bool line_handed_back(uint8_t src)
{
    bool back = false;

    if ( (uint8_t)KODE_CONSOLE_SRC_SCHED == src )
    {
        sched_line_busy();
        back = true;
    }
#if CONFIG_KODEOS_CHANNEL
    if ( (uint8_t)KODE_CONSOLE_SRC_NET == src )
    {
        net_line_busy();
        back = true;
    }
#endif
    return back;
}

/**
 * @brief Run a line that arrived with a command in flight, when it is its
 *        owner's verb that does not wait (kverbs_t.nowait).
 *
 * Only the bare verb: with anything behind it the line is as busy as any
 * other. The handler answers its one line and begins no command, so the
 * command in flight keeps the console; the caller has pointed the replies at
 * the new line's port.
 *
 * @param verb The line's first token.
 * @param rest The line behind it.
 * @return true when the line was run, and has answered.
 */
static bool console_exec_nowait(const char *verb, char *rest)
{
    const kverbs_t *owner = NULL;
    const kverb_t * const row = kverbs_find(verb, &owner);
    char *cur = rest;
    char *tok = NULL;
    bool  ran = false;

    if ( (NULL != row) && (NULL != owner->nowait) && (strcmp(owner->nowait, row->verb) == 0)
        && (CONSOLE_TOK_END == console_token(&cur, &tok)) )
    {
        karg_val_t vals[KCLI_MAX_KEYS];

        (void)memset(vals, 0, sizeof(vals));
        row->fn(row, vals);
        ran = true;
    }
    return ran;
}

/**
 * @brief Parse and run one line: split it, find the verb, validate against
 *        its specs, hand the values to the handler.
 *
 * A quoted value is one token, spaces and all; a malformed one is refused
 * naming its key before the verb is looked up. Every path out has answered
 * (an `>err`, a terminator, or an armed @c me->inflight), except an empty
 * line, which is not a command and gets no response.
 */
static void console_exec(Console *me, char *line)
{
    char *argv[KCLI_MAX_ARGS];
    int   argc = 0;
    char *cur  = line;
    char *tok  = NULL;

    console_tok_t rc = console_token(&cur, &tok);
    if ( CONSOLE_TOK_END == rc )
    {
        /* Not a command, so it answers nothing; but AO_Scheduler counts the
         * lines it hands out, and a blank one is still one of them. */
        if ( (uint8_t)KODE_CONSOLE_SRC_SCHED == me->src )
        {
            sched_line_done();
        }
#if CONFIG_KODEOS_CHANNEL
        if ( (uint8_t)KODE_CONSOLE_SRC_NET == me->src )
        {
            net_line_done(me->line_seq);
        }
#endif
        return;
    }
    if ( ((uint8_t)KCLI_IDLE != me->inflight) && line_handed_back(me->src) )
    {
        /* A NET or SCHED line is not refused: its sender holds it and offers
         * it again. Nothing was accepted, so it neither wakes the board nor
         * resets the idle time. */
        return;
    }
    /* A line is the board being used, from any port, and wakes it when asleep:
     * before the busy check, because a refused line is a person at the other
     * end as much as an accepted one. */
    lowpower_input_note(me->src);
    if ( (uint8_t)KCLI_IDLE != me->inflight )
    {
        /* Busy, whatever else is wrong with the line, unless it is a verb
         * that does not wait. Either answer goes to the NEW line's port; the
         * running command keeps its own. */
        uint8_t const running = me->cmd_src;

        me->cmd_src = me->src;
        if ( !console_exec_nowait(tok, cur) )
        {
            console_reply_err(KCLI_ERR_BUSY, "busy");
        }
        me->cmd_src = running;
        return;
    }
    /* Accepted: from here every reply, errors of the line included, goes to
     * the port that sent it, until the command has closed. */
    me->cmd_src = me->src;
    me->net_seq = me->line_seq;
    while ( (CONSOLE_TOK_WORD == rc) && (KCLI_MAX_ARGS > argc) )
    {
        argv[argc] = tok;
        ++argc;
        rc = console_token(&cur, &tok);
    }
    if ( CONSOLE_TOK_BAD == rc )
    {
        /* console_token() has already cut the token at its `=`. */
        console_reply_err(KCLI_ERR_VALUE, tok);
        return;
    }
    if ( CONSOLE_TOK_WORD == rc )
    {
        /* A dropped token would be a protocol lie. No command reaches here
         * on real keys alone, so refuse it the same way an unknown key is
         * refused. */
        char * const eq = strchr(tok, '=');
        if ( NULL != eq )
        {
            *eq = '\0';
        }
        console_reply_err(KCLI_ERR_VALUE, tok);
        return;
    }

    /* The console's own verbs first, then the ones the owners registered:
     * a verb whose owner the app did not link is unknown. */
    for ( size_t i = 0U; (sizeof(CMDS) / sizeof(CMDS[0])) > i; ++i )
    {
        if ( strcmp(argv[0], CMDS[i].verb) == 0 )
        {
            karg_val_t vals[KCLI_MAX_KEYS];
            if ( cmd_validate(CMDS[i].args, CMDS[i].n_args, argc, argv, vals) )
            {
                me->owner = NULL;
                CMDS[i].fn(me, &CMDS[i], vals);
            }
            return;
        }
    }
    const kverbs_t *owner = NULL;
    const kverb_t * const row = kverbs_find(argv[0], &owner);
    if ( NULL != row )
    {
        karg_val_t vals[KCLI_MAX_KEYS];
        if ( cmd_validate(row->args, row->n_args, argc, argv, vals) )
        {
            me->owner = owner;
            row->fn(row, vals);
        }
        return;
    }
    /* Named back as far as its `=`: a first token that happens to carry a
     * quoted value would otherwise put the value's spaces into a `msg=` that
     * is one word by contract. */
    char * const eq = strchr(argv[0], '=');
    if ( NULL != eq )
    {
        *eq = '\0';
    }
    console_reply_err(KCLI_ERR_VERB, argv[0]);
}

/* ---- The AO -------------------------------------------------------------- */

void Console_ctor(void)
{
    /* A row with too many keys would write past a stack frame when used:
     * fail at boot instead. */
    Q_ASSERT(console_specs_ok());

    g_cmds  = CMDS;
    g_ncmds = sizeof(CMDS) / sizeof(CMDS[0]);
    QActive_ctor(&l_console.super, Q_STATE_CAST(&Console_initial));
    l_console.cmd_src = KODE_LOG_SRC_NONE;
#if CONFIG_KODE_LOWPOWER
    l_console.lp_who  = KODE_LOWPOWER_WHO_NONE;
#endif
    QTimeEvt_ctorX(&l_console.tmo, &l_console.super, CONSOLE_TIMEOUT_SIG, 0U);
    QTimeEvt_ctorX(&l_console.pacer, &l_console.super,
                   CONSOLE_SLICE_SIG, 0U);
    QTimeEvt_ctorX(&l_console.inj_tmo, &l_console.super,
                   KCLI_INJ_TMO_SIG, 0U);
    for ( size_t i = 0U; KODE_VARIABLE_WATCH_MAX > i; ++i )
    {
        QTimeEvt_ctorX(&l_console.watch[i].tev, &l_console.super,
                       CONSOLE_WATCH_SIG, 0U);
        l_console.watch[i].row = NULL;
    }
}

#define KODE_QUEUE_CONSOLE_LEN  64U  /**< A listing page and its end marker land at once. */

AO_STORAGE(console, KODE_QUEUE_CONSOLE_LEN, KQP_AO_STACK_BYTES);

void kaos_console_start(void)
{
    Console_ctor();
    AO_START(AO_Console, console, KODE_PRIO_CONSOLE);
}

/** @brief Initial transition: subscribe to what the console answers, then go active. */
static QState Console_initial(Console * const me, void const * const par)
{
    (void)par;
    QActive_subscribe(&me->super, CONSOLE_LINE_SIG);
#if CONFIG_KODE_LOWPOWER
    me->lp_who = lowpower_join(&me->super, "console");
    if ( KODE_LOWPOWER_WHO_NONE != me->lp_who )
    {
        QActive_subscribe(&me->super, LOWPOWER_ROUND_SIG);
    }
    QActive_subscribe(&me->super, LOWPOWER_STATE_SIG);
    QActive_subscribe(&me->super, LOWPOWER_REFUSED_SIG);
#endif
    QActive_subscribe(&me->super, STORAGE_ENTRY_SIG);
    QActive_subscribe(&me->super, STORAGE_LIST_END_SIG);
    /* APP_ENTRY_SIG and APP_LIST_END_SIG are not subscribed: AO_AppMgr
     * posts a page to its asker alone, and this AO hears only its own. */
    QActive_subscribe(&me->super, RECV_READY_SIG);
    QActive_subscribe(&me->super, RECV_ACK_SIG);
    QActive_subscribe(&me->super, RECV_DONE_SIG);
    QActive_subscribe(&me->super, APP_EVICTED_SIG);
    QActive_subscribe(&me->super, RECV_FAIL_SIG);
    QActive_subscribe(&me->super, RECV_STALL_SIG);
    QActive_subscribe(&me->super, APP_CAT_RSP_SIG);
    QActive_subscribe(&me->super, APP_ERASE_DONE_SIG);
    QActive_subscribe(&me->super, TEST_RESULT_SIG);
    QActive_subscribe(&me->super, AUTOTEST_BEGIN_SIG);
    QActive_subscribe(&me->super, AUTOTEST_ROW_SIG);
    QActive_subscribe(&me->super, AUTOTEST_END_SIG);
    QActive_subscribe(&me->super, AUTOTEST_HOLD_SIG);
    QActive_subscribe(&me->super, AUTOTEST_REPORT_SIG);
    QActive_subscribe(&me->super, POWER_TEST_SIG);
    QActive_subscribe(&me->super, POWER_EVT_SIG);
    QActive_subscribe(&me->super, POWER_FIELD_SIG);
    QActive_subscribe(&me->super, POWER_CTRL_SIG);
    QActive_subscribe(&me->super, BTN_STATE_SIG);
    QActive_subscribe(&me->super, BTN_TEST_SIG);
    QActive_subscribe(&me->super, TOUCH_TEST_SIG);
    QActive_subscribe(&me->super, STORAGE_TEST_SIG);
    QActive_subscribe(&me->super, PSRAM_TEST_SIG);
    QActive_subscribe(&me->super, AUDIO_TEST_SIG);
    QActive_subscribe(&me->super, AUDIO_FIELD_SIG);
    QActive_subscribe(&me->super, AUDIO_FILE_RSP_SIG);
    QActive_subscribe(&me->super, IMU_TEST_SIG);
    QActive_subscribe(&me->super, MAG_TEST_SIG);
    QActive_subscribe(&me->super, HAPTIC_TEST_SIG);
    QActive_subscribe(&me->super, HAPTIC_FIELD_SIG);
    QActive_subscribe(&me->super, DISPLAY_TEST_SIG);
    QActive_subscribe(&me->super, LED_TEST_SIG);
    QActive_subscribe(&me->super, BOOT_STATE_SIG);
    QActive_subscribe(&me->super, GUI_STATE_SIG);
    QActive_subscribe(&me->super, CONFIG_ANS_SIG);
    QActive_subscribe(&me->super, STORAGE_INFO_SIG);
    QActive_subscribe(&me->super, IR_RX_SIG);
#if CONFIG_KODEOS_MIRROR
    QActive_subscribe(&me->super, DISPLAY_MIRROR_INFO_SIG);
#endif
#if CONFIG_KODEOS_RADIO
    QActive_subscribe(&me->super, RADIO_STATE_SIG);
#if CONFIG_KODEOS_WIFI
    QActive_subscribe(&me->super, WIFI_STATE_SIG);
    QActive_subscribe(&me->super, WIFI_SCAN_ENTRY_SIG);
    QActive_subscribe(&me->super, WIFI_SCAN_END_SIG);
    QActive_subscribe(&me->super, WIFI_SAVED_ENTRY_SIG);
    QActive_subscribe(&me->super, WIFI_SAVED_END_SIG);
#endif
#if CONFIG_KODEOS_BLE
    QActive_subscribe(&me->super, BLE_STATE_SIG);
    QActive_subscribe(&me->super, BLE_SCAN_ENTRY_SIG);
    QActive_subscribe(&me->super, BLE_SCAN_END_SIG);
#endif
#if CONFIG_KODEOS_NET
    QActive_subscribe(&me->super, NET_PING_ENTRY_SIG);
    QActive_subscribe(&me->super, NET_PING_END_SIG);
    QActive_subscribe(&me->super, NET_HTTP_DATA_SIG);
    QActive_subscribe(&me->super, NET_HTTP_DONE_SIG);
    QActive_subscribe(&me->super, NET_UDP_DONE_SIG);
    QActive_subscribe(&me->super, NET_INFO_SIG);
#if CONFIG_KODEOS_MQTT
    QActive_subscribe(&me->super, MQTT_STATE_SIG);
    QActive_subscribe(&me->super, CHANNEL_STATE_SIG);
    /* Not MQTT_MSG_SIG: an arriving message is unsolicited, so it belongs in
     * a '!' line - which AO_Net already logs - and not in the response
     * stream, where it would appear in the middle of somebody's command. */
#endif
#endif
#endif
    KLOG_I(TAG, "console up ('help' for commands)");
    return Q_TRAN(&Console_active);
}

/**
 * @brief Every event the console handles itself, whatever holds it.
 * @param me The console.
 * @param e  The event.
 * @return The state handler's verdict.
 */
static QState console_own(Console * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case CONSOLE_LINE_SIG: {
            /* copy: the tokeniser cuts in place, and the pooled event is
             * shared */
            char line[KODE_CONSOLE_LINE_MAX];
            ConsoleLineEvt const * const cl = Q_EVT_CAST(ConsoleLineEvt);
            (void)snprintf(line, sizeof(line), "%s", cl->line);
            /* Before console_exec, not in cmd_begin: handlers ask
             * console_src_allows() before cmd_begin. */
            me->src = cl->src;
            if ( (uint8_t)KODE_CONSOLE_SRC_NET == me->src )
            {
                me->line_seq = Q_EVT_CAST(ConsoleLineEvt)->seq;
            }
            console_exec(me, line);
            status = Q_HANDLED();
            break;
        }

        /* `sd_ls`, `fs_ls` and `ls` entries are published, so they also
         * arrive when someone else asked: each record is gated on this AO's
         * own command being in flight. */
        case STORAGE_ENTRY_SIG: {
            StorageEntryEvt const * const en = Q_EVT_CAST(StorageEntryEvt);
            const char *medium = "fs";
            if ( en->from_sd )
            {
                medium = "sd";
            }
            char qn[CONSOLE_QUOTED_MAX(sizeof(en->name))];
            if ( (uint8_t)KCLI_FS_LIST == me->inflight )
            {
                /* sd_ls's record plus `type=`. Only a path listing knows
                 * directories: the medium walk sets is_dir false. */
                const char *type = "file";
                if ( en->is_dir )
                {
                    type = "dir";
                }
                console_reply(">|name=%s type=%s size=%lu medium=%s",
                      console_quote(qn, sizeof(qn), en->name), type,
                      (unsigned long)en->size, medium);
            }
            else if ( ((uint8_t)KCLI_APPDATA_LS == me->inflight)
                     || ((uint8_t)KCLI_APPDATA_INFO == me->inflight) )
            {
                ad_entry(me, en);
            }
            else
            {
                /* Somebody else's listing. */
            }
            status = Q_HANDLED();
            break;
        }
        case STORAGE_LIST_END_SIG: {
            if ( (uint8_t)KCLI_FS_LIST == me->inflight )
            {
                console_reply(">ok");
                console_end();
            }
            else if ( ((uint8_t)KCLI_APPDATA_LS == me->inflight)
                     || ((uint8_t)KCLI_APPDATA_INFO == me->inflight) )
            {
                bool const more = Q_EVT_CAST(ListEndEvt)->more;
                if ( ((uint8_t)KCLI_AD_FILES == me->ad_phase) && more
                    && (me->list_next == me->list_from) )
                {
                    /* A page that brought nothing: asking again would be a
                     * loop with the owner. The files found so far stand. */
                    KLOG_W(TAG, "appdata: a page of the files brought nothing; "
                                "the walk goes on with %u", (unsigned)me->list_next);
                    ad_phase_enter(me, KCLI_AD_DESC);
                }
                else if ( ((uint8_t)KCLI_AD_FILES == me->ad_phase) && more
                         && ((uint8_t)KCLI_AD_MAX > me->ad_n) )
                {
                    console_extend(KCLI_AD_ROW_MS);
                    me->list_from = me->list_next;
                    storage_list_page(me->list_next, &me->super);
                }
                else if ( (uint8_t)KCLI_AD_FILES == me->ad_phase )
                {
                    ad_phase_enter(me, KCLI_AD_DESC);
                }
                else if ( (uint8_t)KCLI_AD_FOLDERS_FS == me->ad_phase )
                {
                    ad_phase_enter(me, KCLI_AD_FOLDERS_SD);
                }
                else if ( (uint8_t)KCLI_AD_FOLDERS_SD == me->ad_phase )
                {
                    ad_phase_enter(me, KCLI_AD_ASK);
                }
                else
                {
                    /* A listing this phase did not ask for. */
                }
            }
            else
            {
                /* Somebody else's listing. */
            }
            status = Q_HANDLED();
            break;
        }
        /* Posted, not published - AO_Storage answers the AO that asked - so
         * this arrives whether or not anything is in flight, and a stale one
         * has to be ignored rather than closing whatever came after it. */
        case STORAGE_PATH_RSP_SIG: {
            if ( ((uint8_t)KCLI_FS_CD == me->inflight)
                || ((uint8_t)KCLI_FS_STAT == me->inflight)
                || ((uint8_t)KCLI_FS_LIST_STAT == me->inflight)
                || ((uint8_t)KCLI_FS_CAT_STAT == me->inflight)
                || ((uint8_t)KCLI_FS_MKDIR == me->inflight)
                || ((uint8_t)KCLI_FS_RM_STAT == me->inflight)
                || ((uint8_t)KCLI_FS_RM == me->inflight)
                || ((uint8_t)KCLI_FS_RMDIR_STAT == me->inflight)
                || ((uint8_t)KCLI_FS_RMDIR == me->inflight)
                || ((uint8_t)KCLI_FS_MV == me->inflight)
                || ((uint8_t)KCLI_FS_CP == me->inflight)
                || ((uint8_t)KCLI_FS_WRITE == me->inflight) )
            {
                path_answer(me, Q_EVT_CAST(StoragePathRspEvt));
            }
            status = Q_HANDLED();
            break;
        }
        /* Posted to this AO, never published: a file's text is between this
         * console and AO_Storage. */
        case STORAGE_LINE_SIG: {
            if ( (uint8_t)KCLI_FS_CAT == me->inflight )
            {
                StorageLineEvt const * const ln = Q_EVT_CAST(StorageLineEvt);
                if ( !ln->eof )
                {
                    /* What is left of the line; console_kv_fit() says when
                     * escaping cuts a line. */
                    char q[KODE_LOG_LINE_MAX - (sizeof(">|line=") - 1U)];
                    console_reply(">|line=%s", console_kv_fit(q, sizeof(q), ln->text, "line"));
                }
                else if ( (uint8_t)KODE_OK != ln->rc )
                {
                    console_reply_err(KCLI_ERR_REJECT, "volume");
                    console_end();
                }
                else
                {
                    console_reply(">ok");
                    console_end();
                }
            }
            status = Q_HANDLED();
            break;
        }
        case APP_ENTRY_SIG: {
            AppEntryEvt const * const en = Q_EVT_CAST(AppEntryEvt);
            bool const gather =
                (((uint8_t)KCLI_APPDATA_LS == me->inflight)
                 || ((uint8_t)KCLI_APPDATA_INFO == me->inflight))
                && ((uint8_t)KCLI_AD_INSTALLED == me->ad_phase);
            /* A page asked twice repeats its entries: only the one at the
             * next position is new. */
            bool const fresh = gather && (en->pos == me->list_next);
            if ( fresh )
            {
                ++me->list_next;
                if ( ad_is_ls(me) )
                {
                    ad_add(me, en->name, KCLI_AD_INSTALLED_APP);
                }
                else if ( strcmp(en->name, me->ad_target) == 0 )
                {
                    (void)snprintf(me->ad_bin_name, sizeof(me->ad_bin_name),
                                   "%s", en->display);
                    (void)snprintf(me->ad_bin_version,
                                   sizeof(me->ad_bin_version), "%s",
                                   en->version);
                    (void)snprintf(me->ad_bin_category,
                                   sizeof(me->ad_bin_category), "%s",
                                   en->category);
                    me->ad_bin = true;
                }
                else
                {
                    /* Another app; this verb was asked about one. */
                }
            }
            /* Else a repeat, or somebody else's listing. */
            status = Q_HANDLED();
            break;
        }
        case APP_LIST_END_SIG: {
            bool const more = Q_EVT_CAST(ListEndEvt)->more;
            bool const gather =
                (((uint8_t)KCLI_APPDATA_LS == me->inflight)
                 || ((uint8_t)KCLI_APPDATA_INFO == me->inflight))
                && ((uint8_t)KCLI_AD_INSTALLED == me->ad_phase);
            /* A page that ends with `more` but brought nothing would be
             * asked again for ever: an owner out of event blocks answers
             * every page that way. */
            bool const stuck = more && (me->list_next == me->list_from);
            if ( gather && more && !stuck
                && ((uint8_t)KCLI_AD_MAX > me->ad_n) )
            {
                me->list_from = me->list_next;
                appmgr_list_page(me->list_next, &me->super);
            }
            else if ( gather )
            {
                /* `info` whose project is installed already has its binary:
                 * scanning the media for a file of the same name would only
                 * find a copy the apps region has already outranked. */
                ad_phase_enter(me, (!ad_is_ls(me) && me->ad_bin)
                                   ? KCLI_AD_ASK : KCLI_AD_FILES);
            }
            else
            {
                /* Somebody else's listing. */
            }
            status = Q_HANDLED();
            break;
        }
        case BTN_TEST_SIG: {
            if ( ((uint8_t)KCLI_TEST == me->inflight)
                && (TEST_ORIGIN_AUTOTEST == Q_EVT_CAST(ButtonsTestEvt)->origin) )
            {
                /* The row runs on its own budget (KODE_BUTTONS_TEST_BUDGET_MS,
                 * the script's worst case), armed when the row started; a
                 * prompt is nothing to re-buy. BTN_TEST_DONE prints nothing:
                 * the verdict follows as TEST_RESULT_SIG. */
                char r[KODE_FUNCTION_REC_MAX];
                if ( buttons_test_record(Q_EVT_CAST(ButtonsTestEvt), r, sizeof(r)) > 0U )
                {
                    console_reply(">|%s", r);
                }
            }
            status = Q_HANDLED();
            break;
        }
        case AUDIO_TEST_SIG: {
            /* One line per record, and nothing is buffered: the row prints
             * as it measures, so an operator sees the floor before the tones
             * that are judged against it. */
            if ( (uint8_t)KCLI_TEST == me->inflight )
            {
                AudioTestEvt const * const t = Q_EVT_CAST(AudioTestEvt);
                const char *result = "fail";
                if ( (t->flags & AUDIO_TF_PASS) != 0U )
                {
                    result = "pass";
                }
                if ( AUDIO_TEST_MIC == t->phase )
                {
                    console_reply(">|step=%u of=%u what=mic rms=%d peak=%d max=%d "
                          "stuck=%u clip=%u loop=%u result=%s",
                          (unsigned)t->step, (unsigned)t->total,
                          (int)t->level_x10, (int)t->floor_x10,
                          (int)KTONE_FLOOR_MAX_DBFS_X10,
                          (unsigned)((t->flags & AUDIO_TF_STUCK) != 0U),
                          (unsigned)((t->flags & AUDIO_TF_CLIP) != 0U),
                          (unsigned)((t->flags & AUDIO_TF_LOOP) != 0U),
                          result);
                }
                else if ( AUDIO_TEST_BIN == t->phase )
                {
                    console_reply(">|step=%u bin=%u level=%d", (unsigned)t->step,
                          (unsigned)t->hz, (int)t->level_x10);
                }
                else if ( AUDIO_TEST_TONE == t->phase )
                {
                    console_reply(">|step=%u of=%u attempt=%u of_tries=%u tone=%u",
                          (unsigned)t->step, (unsigned)t->total,
                          (unsigned)t->attempt, (unsigned)t->tries,
                          (unsigned)t->hz);
                }
                else
                {
                    console_reply(">|step=%u attempt=%u tone=%u level=%d other=%d "
                          "floor=%d result=%s",
                          (unsigned)t->step, (unsigned)t->attempt,
                          (unsigned)t->hz, (int)t->level_x10,
                          (int)t->other_x10, (int)t->floor_x10, result);
                }
            }
            status = Q_HANDLED();
            break;
        }
        case IMU_TEST_SIG: {
            /* One line per record, and nothing is buffered: the row prints
             * as it measures. Three steps, and `of=` is a constant because
             * the event carries no total - the row cannot end early. */
            if ( (uint8_t)KCLI_TEST == me->inflight )
            {
                ImuTestEvt const * const t = Q_EVT_CAST(ImuTestEvt);
                const char *result = "fail";
                if ( (t->flags & IMU_TF_PASS) != 0U )
                {
                    result = "pass";
                }
                if ( IMU_TEST_WHOAMI == t->phase )
                {
                    console_reply(">|step=%u of=%u whoami=0x%02X result=%s",
                          (unsigned)t->step, (unsigned)KCLI_IMU_STEPS,
                          (unsigned)(uint32_t)t->a, result);
                }
                else if ( IMU_TEST_ST == t->phase )
                {
                    const char *sensor = "xl";
                    if ( 0 != t->d )
                    {
                        sensor = "gy";
                    }
                    console_reply(">|step=%u of=%u sensor=%s dx=%ld dy=%ld dz=%ld "
                          "result=%s",
                          (unsigned)t->step, (unsigned)KCLI_IMU_STEPS, sensor,
                          (long)t->a, (long)t->b, (long)t->c, result);
                }
                else
                {
                    console_reply(">|step=%u of=%u a_mg=%ld w_mdps=%ld temp_dc=%ld "
                          "result=%s",
                          (unsigned)t->step, (unsigned)KCLI_IMU_STEPS,
                          (long)t->a, (long)t->b, (long)t->c, result);
                }
            }
            status = Q_HANDLED();
            break;
        }
        case MAG_TEST_SIG: {
            /* One line per record, as the row measures, in the spec's own
             * shapes - no `of=`, because step 1 can end the row. Step 3 is a
             * reading and carries no `result=` at all; the end marker prints
             * nothing, since the verdict follows as TEST_RESULT_SIG. */
            if ( (uint8_t)KCLI_TEST == me->inflight )
            {
                MagTestEvt const * const t = Q_EVT_CAST(MagTestEvt);
                const char *result = "fail";
                if ( (t->flags & MAG_TF_PASS) != 0U )
                {
                    result = "pass";
                }
                if ( MAG_TEST_WHOAMI == t->phase )
                {
                    if ( (t->flags & MAG_TF_ABSENT) != 0U )
                    {
                        console_reply(">|step=%u mag=absent result=absent",
                              (unsigned)t->step);
                    }
                    else if ( (t->flags & MAG_TF_MOVED) != 0U )
                    {
                        console_reply(">|step=%u mag=moved addr=0x%02X result=fail",
                              (unsigned)t->step, (unsigned)(uint32_t)t->a);
                    }
                    else
                    {
                        console_reply(">|step=%u whoami=0x%02X result=%s",
                              (unsigned)t->step, (unsigned)(uint32_t)t->a,
                              result);
                    }
                }
                else if ( MAG_TEST_ST == t->phase )
                {
                    console_reply(">|step=%u st_x=%ld st_y=%ld st_z=%ld result=%s",
                          (unsigned)t->step, (long)t->a, (long)t->b,
                          (long)t->c, result);
                }
                else if ( MAG_TEST_FIELD == t->phase )
                {
                    console_reply(">|step=%u bx=%ld by=%ld bz=%ld babs=%ld",
                          (unsigned)t->step, (long)t->a, (long)t->b,
                          (long)t->c, (long)t->d);
                }
                else
                {
                    /* MAG_TEST_DONE: the verdict follows as TEST_RESULT_SIG. */
                }
            }
            status = Q_HANDLED();
            break;
        }
        case HAPTIC_TEST_SIG: {
            /* One record shape per phase. HAPTIC_TF_LOUD on a floor record
             * judges the bench; on a noverdict record it is why nothing was
             * judged. Both measured shapes go through haptic_result(). */
            if ( (uint8_t)KCLI_TEST == me->inflight )
            {
                HapticTestEvt const * const t = Q_EVT_CAST(HapticTestEvt);
                if ( HAPTIC_TEST_ABSENT == t->phase )
                {
                    console_reply(">|step=%u haptic=absent result=absent",
                          (unsigned)t->step);
                }
                else if ( HAPTIC_TEST_NOVERDICT == t->phase )
                {
                    const char *why = "loud";
                    /* The row could not judge the motor at all: the same
                     * outcome as a step nobody answered. */
                    if ( (t->flags & HAPTIC_TF_MUTE) != 0U )
                    {
                        why = "mute";
                    }
                    else if ( (t->flags & HAPTIC_TF_SHORT) != 0U )
                    {
                        why = "short";
                    }
                    else
                    {
                        /* HAPTIC_TF_LOUD, the only reason left. */
                    }
                    console_reply(">|step=%u result=noverdict why=%s",
                          (unsigned)t->step, why);
                }
                else if ( HAPTIC_TEST_FLOOR == t->phase )
                {
                    const char * const result = haptic_result(t->flags);
                    console_reply(">|step=%u floor_235=%ld n=%u result=%s",
                          (unsigned)t->step, (long)t->floor_x10,
                          (unsigned)t->n, result);
                }
                else
                {
                    const char * const result = haptic_result(t->flags);
                    console_reply(">|step=%u gain=0x%02X on_235=%ld ratio=%lu.%lu "
                          "n=%u result=%s",
                          (unsigned)t->step, (unsigned)t->gain,
                          (long)t->on_x10,
                          (unsigned long)(t->ratio_x10 / 10UL),
                          (unsigned long)(t->ratio_x10 % 10UL),
                          (unsigned)t->n, result);
                }
            }
            status = Q_HANDLED();
            break;
        }
        case DISPLAY_TEST_SIG: {
            /* The guided button test's shapes: a question, then how it was
             * answered, under one step number. The row runs on its own
             * budget, armed when it started; a question is nothing to
             * re-buy. */
            if ( (uint8_t)KCLI_TEST == me->inflight )
            {
                DisplayTestEvt const * const t = Q_EVT_CAST(DisplayTestEvt);
                const char * const name = display_field_name(t->field);
                if ( (uint8_t)DISPLAY_TEST_ABSENT == t->phase )
                {
                    console_reply(">|step=%u panel=absent result=absent",
                          (unsigned)t->step);
                }
                else if ( (uint8_t)DISPLAY_TEST_ASK == t->phase )
                {
                    console_reply(">|step=%u of=%u look=%s yes=top no=bot",
                          (unsigned)t->step, (unsigned)t->total, name);
                }
                else if ( (uint8_t)DISPLAY_TEST_PASS == t->phase )
                {
                    console_reply(">|step=%u field=%s result=pass", (unsigned)t->step, name);
                }
                else if ( (uint8_t)DISPLAY_TEST_FAIL == t->phase )
                {
                    console_reply(">|step=%u field=%s result=fail", (unsigned)t->step, name);
                }
                else if ( (uint8_t)DISPLAY_TEST_TIMEOUT == t->phase )
                {
                    console_reply(">|step=%u field=%s result=timeout", (unsigned)t->step, name);
                }
                else
                {
                    /* DISPLAY_TEST_INTRO: the glass's to say, and the log's.
                     * DISPLAY_TEST_DONE: the verdict follows as TEST_RESULT_SIG. */
                }
            }
            status = Q_HANDLED();
            break;
        }
        case LED_TEST_SIG: {
            /* The display row's contract, for the LED's three colours. */
            if ( (uint8_t)KCLI_TEST == me->inflight )
            {
                LedTestEvt const * const t = Q_EVT_CAST(LedTestEvt);
                const char * const name = led_colour_name(t->colour);
                if ( (uint8_t)LED_TEST_ASK == t->phase )
                {
                    console_reply(">|step=%u of=%u look=%s yes=top no=bot",
                          (unsigned)t->step, (unsigned)t->total, name);
                }
                else if ( (uint8_t)LED_TEST_PASS == t->phase )
                {
                    console_reply(">|step=%u colour=%s result=pass", (unsigned)t->step, name);
                }
                else if ( (uint8_t)LED_TEST_FAIL == t->phase )
                {
                    console_reply(">|step=%u colour=%s result=fail", (unsigned)t->step, name);
                }
                else if ( (uint8_t)LED_TEST_TIMEOUT == t->phase )
                {
                    console_reply(">|step=%u colour=%s result=timeout", (unsigned)t->step, name);
                }
                else
                {
                    /* LED_TEST_DONE: the verdict follows as TEST_RESULT_SIG. */
                }
            }
            status = Q_HANDLED();
            break;
        }
        case TOUCH_TEST_SIG: {
            if ( ((uint8_t)KCLI_TEST == me->inflight)
                && (TEST_ORIGIN_AUTOTEST == Q_EVT_CAST(TouchTestEvt)->origin) )
            {
                /* On the row's own budget, like the button test. */
                char r[KODE_FUNCTION_REC_MAX];
                if ( touch_test_record(Q_EVT_CAST(TouchTestEvt), r, sizeof(r)) > 0U )
                {
                    console_reply(">|%s", r);
                }
            }
            status = Q_HANDLED();
            break;
        }
        case AUTOTEST_BEGIN_SIG: {
            AutotestBeginEvt const * const b = Q_EVT_CAST(AutotestBeginEvt);

            /* Published, so the answer to somebody else's request arrives
             * here too: only the console's own is read. */
            if ( ((uint8_t)KCLI_TEST == me->inflight) && me->test_wait_begin
                && (AUTOTEST_CLIENT_CONSOLE == b->client) )
            {
                me->test_wait_begin = false;
                if ( AUTOTEST_BUSY == b->status )
                {
                    console_reply_err(KCLI_ERR_BUSY, "test");
                    console_end();
                }
                else if ( AUTOTEST_UNKNOWN == b->status )
                {
                    console_reply_err(KCLI_ERR_VALUE, "component");
                    console_end();
                }
                else
                {
                    /* Taken: the rows follow, and this many are owed. */
                    me->test_rows = b->rows;
                    me->test_seen = 0U;
                }
            }
            status = Q_HANDLED();
            break;
        }
        case AUTOTEST_ROW_SIG: {
            AutotestRowEvt const * const r = Q_EVT_CAST(AutotestRowEvt);

            if ( ((uint8_t)KCLI_TEST == me->inflight) && !me->test_wait_begin
                && (AUTOTEST_CLIENT_CONSOLE == r->client) )
            {
                if ( AUTOTEST_ROW_RUNNING == r->state )
                {
                    /* The row's budget is AO_Autotest's to enforce; this
                     * watchdog only has to outlast it. */
                    console_extend(r->budget_ms + KCLI_CMD_TIMEOUT_MS);
                }
                else if ( AUTOTEST_ROW_STATES > r->state )
                {
                    console_reply(">|test=%s result=%s", r->name,
                          TEST_STATE_WORD[r->state]);
                    if ( UINT8_MAX > me->test_seen )
                    {
                        me->test_seen += 1U;
                    }
                    test_keep(me, r->id, r->state);
                }
                else
                {
                    /* A state this build does not know: not a record. */
                }
            }
            status = Q_HANDLED();
            break;
        }
        case POWER_TEST_SIG: {
            /* The `power_plug` row's steps, as records: what is being asked
             * of whoever holds the charger, and how each ask ended. */
            if ( (uint8_t)KCLI_TEST == me->inflight )
            {
                PowerTestEvt const * const t = Q_EVT_CAST(PowerTestEvt);

                if ( POWER_TEST_ASK_PLUG == t->phase )
                {
                    console_reply(">|step=%u of=%u do=plug", (unsigned)t->step,
                          (unsigned)t->total);
                }
                else if ( POWER_TEST_ASK_UNPLUG == t->phase )
                {
                    console_reply(">|step=%u of=%u do=unplug", (unsigned)t->step,
                          (unsigned)t->total);
                }
                else if ( POWER_TEST_PLUGGED == t->phase )
                {
                    console_reply(">|step=%u charger=in", (unsigned)t->step);
                }
                else if ( POWER_TEST_PASS == t->phase )
                {
                    console_reply(">|step=%u result=pass", (unsigned)t->step);
                }
                else if ( POWER_TEST_FAIL == t->phase )
                {
                    console_reply(">|step=%u result=fail", (unsigned)t->step);
                }
                else
                {
                    /* POWER_TEST_TIMEOUT: nobody did what was asked. */
                    console_reply(">|step=%u result=timeout", (unsigned)t->step);
                }
            }
            status = Q_HANDLED();
            break;
        }
        case AUTOTEST_REPORT_SIG: {
            AutotestReportEvt const * const r = Q_EVT_CAST(AutotestReportEvt);

            if ( ((uint8_t)KCLI_TEST_REPORT == me->inflight)
                && (AUTOTEST_CLIENT_CONSOLE == r->client) )
            {
                if ( !r->last )
                {
                    char word[CONSOLE_QUOTED_MAX(KODE_TEST_NAME_LEN)];
                    const char *state = "-";

                    if ( AUTOTEST_ROW_STATES > r->state )
                    {
                        state = TEST_STATE_WORD[r->state];
                    }
                    console_reply(">|row=%s result=%s",
                          console_quote(word, sizeof(word),
                                        autotest_row_name(r->id)),
                          state);
                    test_keep(me, r->id, r->state);
                }
                else
                {
                    if ( 0U == r->rows )
                    {
                        console_reply(">|report=none");
                    }
                    else
                    {
                        test_verdicts_print(me);
                        console_reply(">|when=%lu rows=%u problems=%u",
                              (unsigned long)r->when, (unsigned)r->rows,
                              (unsigned)r->problems);
                    }
                    console_reply(">ok");
                    console_end();
                }
            }
            status = Q_HANDLED();
            break;
        }
        case AUTOTEST_HOLD_SIG: {
            /* The `user` profile waits here for whoever sits at the glass.
             * From the console that is the person typing, who reads the
             * prompts as records: the run goes straight on. */
            if ( ((uint8_t)KCLI_TEST == me->inflight) && !me->test_wait_begin
                && (Q_EVT_CAST(AutotestHoldEvt)->client
                    == AUTOTEST_CLIENT_CONSOLE) )
            {
                console_reply(">|hold=continue");
                console_extend(KCLI_CMD_TIMEOUT_MS);
                autotest_continue(AUTOTEST_CLIENT_CONSOLE);
            }
            status = Q_HANDLED();
            break;
        }
        case AUTOTEST_END_SIG: {
            AutotestEndEvt const * const d = Q_EVT_CAST(AutotestEndEvt);

            if ( ((uint8_t)KCLI_TEST == me->inflight) && !me->test_wait_begin
                && (AUTOTEST_CLIENT_CONSOLE == d->client) )
            {
                /* `fail=` is every row that was judged bad or not judged at
                 * all, so a bench ships a unit on `fail=0` alone; `timeout=`
                 * says how many of those nobody judged. A skipped row is
                 * neither, and is only named when there is one. */
                test_verdicts_print(me);
                unsigned const fail = (unsigned)d->tally[AUTOTEST_ROW_FAIL]
                                      + (unsigned)d->tally[AUTOTEST_ROW_TIMEOUT];

                if ( 0U != d->tally[AUTOTEST_ROW_SKIPPED] )
                {
                    console_reply(">|summary pass=%u fail=%u timeout=%u skipped=%u",
                          (unsigned)d->tally[AUTOTEST_ROW_PASS], fail,
                          (unsigned)d->tally[AUTOTEST_ROW_TIMEOUT],
                          (unsigned)d->tally[AUTOTEST_ROW_SKIPPED]);
                }
                else
                {
                    console_reply(">|summary pass=%u fail=%u timeout=%u",
                          (unsigned)d->tally[AUTOTEST_ROW_PASS], fail,
                          (unsigned)d->tally[AUTOTEST_ROW_TIMEOUT]);
                }
                /* `>ok` says every row of the run has its record above. A
                 * run that ended short of the rows it promised lost one on
                 * the way, and a summary over the rest is not a verdict. */
                if ( me->test_seen == me->test_rows )
                {
                    console_reply(">ok");
                }
                else
                {
                    KLOG_E(TAG, "test: %u rows promised, %u answered",
                           (unsigned)me->test_rows, (unsigned)me->test_seen);
                    console_reply_err(KCLI_ERR_REJECT, "test");
                }
                console_end();
            }
            status = Q_HANDLED();
            break;
        }
        case POWER_FIELD_SIG: {
            /* Published: either power chip's answer closes a pmic or gauge
             * verb, named after the chip that answered. */
            if ( ((uint8_t)KCLI_FIELD == me->inflight)
                && ((me->field_chip == &PMIC_CHIP)
                    || (me->field_chip == &GAUGE_CHIP)) )
            {
                PowerFieldEvt const * const a = Q_EVT_CAST(PowerFieldEvt);
                const console_chip_t *chip = &PMIC_CHIP;

                if ( POWER_CHIP_GAUGE == a->chip )
                {
                    chip = &GAUGE_CHIP;
                }
                field_close(chip, a->op, a->field, a->rc, a->value);
            }
            status = Q_HANDLED();
            break;
        }
        case IOEXP_FIELD_RSP_SIG: {
            /* Posted to us, not published: only ever our own question's
             * answer. The gate still stands, against a late one. */
            IoexpFieldEvt const * const a = Q_EVT_CAST(IoexpFieldEvt);

            console_field_answer(&IOEXP_CHIP, a->op, a->field, a->rc, a->value);
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODEOS_BOOT_ROLE_OS
        case FACTORY_RESET_RSP_SIG: {
            if ( (uint8_t)KCLI_RESET == me->inflight )
            {
                FactoryResetRspEvt const * const a =
                    Q_EVT_CAST(FactoryResetRspEvt);
                /* Success is the status, not the step: the closing request
                 * can fail (step `request`) after everything is erased, and
                 * the board then stays up. */
                if ( (uint8_t)KODE_OK == a->status )
                {
                    console_reply(">|factory_reset result=done");
                    console_reply(">ok");
                }
                else
                {
                    char const * const step = boot_reset_step_name(a->step);
                    console_reply(">|factory_reset step=%s status=%u", step,
                                  (unsigned)a->status);
                    console_reply_err(KCLI_ERR_REJECT, step);
                }
                console_end();
            }
            status = Q_HANDLED();
            break;
        }
#endif
        case PSRAM_TEST_SIG: {
            if ( (uint8_t)KCLI_TEST == me->inflight )
            {
                PsramTestEvt const * const t = Q_EVT_CAST(PsramTestEvt);

                /* With no fault there is no address to name: `-`, never a
                 * zero that would read as one. */
                if ( 0U == t->errors )
                {
                    console_reply(">|psram tested_kb=%lu free_kb=%lu errors=0 "
                          "first=- wrote=- read=-",
                          (unsigned long)t->tested_kb,
                          (unsigned long)t->free_kb);
                }
                else
                {
                    console_reply(">|psram tested_kb=%lu free_kb=%lu errors=%lu "
                          "first=0x%08lx wrote=0x%08lx read=0x%08lx",
                          (unsigned long)t->tested_kb,
                          (unsigned long)t->free_kb,
                          (unsigned long)t->errors,
                          (unsigned long)t->first,
                          (unsigned long)t->wrote,
                          (unsigned long)t->read);
                }
            }
            status = Q_HANDLED();
            break;
        }
        case STORAGE_TEST_SIG: {
            if ( (uint8_t)KCLI_TEST == me->inflight )
            {
                StorageTestEvt const * const t = Q_EVT_CAST(StorageTestEvt);
                static const char *const PHASE[] = {
                    "", "identify", "read4", "read1", "write", "verify"
                };
                const char *const phase =
                    ((sizeof(PHASE) / sizeof(PHASE[0])) > t->phase)
                        ? PHASE[t->phase] : "?";
                const char *const result = t->ok ? "pass" : "fail";
                char safe[CONSOLE_QUOTED_MAX(sizeof(t->name))];

                /* One record per phase, carrying only the numbers its own
                 * phase measured: printing a zero for the rest would read as
                 * a measurement rather than as an absence. */
                switch ( t->phase )
                {
                    case SD_TEST_IDENTIFY: {
                        console_reply(">|step=%u of=%u sd=%s result=%s name=%s "
                              "width=%u freq_khz=%lu capacity_mb=%lu",
                              (unsigned)t->phase, (unsigned)t->total, phase,
                              result, console_kv_text(safe, sizeof(safe), t->name),
                              (unsigned)t->width,
                              (unsigned long)t->freq_khz,
                              (unsigned long)t->capacity_mb);
                        break;
                    }
                    default: {
                        /* The four transfer phases, each with the rate it
                         * measured across the transfers alone - never across
                         * the pacing between them, which would be a measure
                         * of AO_Storage rather than of the card. */
                        console_reply(">|step=%u of=%u sd=%s result=%s width=%u "
                              "kb=%u kb_s=%lu",
                              (unsigned)t->phase, (unsigned)t->total, phase,
                              result, (unsigned)t->width,
                              (unsigned)KODE_SD_TEST_KB,
                              (unsigned long)t->kb_s);
                        break;
                    }
                }
            }
            status = Q_HANDLED();
            break;
        }
        case POWER_CTRL_SIG: {
            PowerCtrlEvt const * const a = Q_EVT_CAST(PowerCtrlEvt);
            /* Answers are published, so one may belong to an app that asked
             * while this verb was waiting: only its own op closes it. */
            if ( ((uint8_t)KCLI_CTRL == me->inflight) && (a->op == me->ctrl_op) )
            {
                if ( (uint8_t)KODE_OK == a->rc )
                {
                    switch ( a->op )
                    {
                        case POWER_CTRL_BOOST: {
                            console_reply(">|op=boost boost=%s", console_on_off(0 != a->value));
                            break;
                        }
                        default: {
                            /* A reset has nothing to report but that it is done. */
                            break;
                        }
                    }
                    console_reply(">ok");
                }
                else if ( (uint8_t)KODE_DENIED == a->rc )
                {
                    console_reply_err(KCLI_ERR_VALUE, me->verb);
                }
                else if ( (uint8_t)KODE_NO_RESOURCES == a->rc )
                {
                    console_reply_err(KCLI_ERR_BUSY, "busy");
                }
                else
                {
                    console_reply_err(KCLI_ERR_REJECT, me->verb);
                }
                console_end();
            }
            status = Q_HANDLED();
            break;
        }
        case POWER_EVT_SIG: {
            if ( (uint8_t)KCLI_BAT == me->inflight )
            {
                PowerEvt const * const p = Q_EVT_CAST(PowerEvt);
                console_reply(">|vbat_mv=%u vbus_mv=%u vbus=%s boost=%s current_ma=%d"
                      " soc=%u charging=%u fault=%u",
                      (unsigned)p->vbat_mv, (unsigned)p->vbus_mv,
                      console_on_off(p->vbus), console_on_off(p->boost), (int)p->current_ma,
                      (unsigned)p->soc_pct, (unsigned)p->charging,
                      (unsigned)p->fault);
                console_reply(">ok");
                console_end();
            }
            status = Q_HANDLED();
            break;
        }
        case BTN_STATE_SIG: {
            if ( (uint8_t)KCLI_BTN == me->inflight )
            {
                ButtonsStateEvt const * const b = Q_EVT_CAST(ButtonsStateEvt);
                if ( !b->ok )
                {
                    console_reply_err(KCLI_ERR_REJECT, me->verb);
                }
                else
                {
                    for ( uint8_t i = 0U; (uint8_t)KDRV_BTN_COUNT > i; ++i )
                    {
                        console_reply(">|btn=%s pressed=%s",
                              kdrv_button_name((kdrv_button_t)i),
                              ((b->pressed & (uint8_t)(1U << i)) != 0U)
                                  ? "on" : "off");
                    }
                    console_reply(">ok");
                }
                console_end();
            }
            status = Q_HANDLED();
            break;
        }
        case BOOT_STATE_SIG: {
            if ( (uint8_t)KCLI_BOOT == me->inflight )
            {
                BootStateEvt const * const b = Q_EVT_CAST(BootStateEvt);
                static const char *const REASONS[] = {
                    "cold", "app", "unexpected",   /* kboot_reason_t order */
                };
                console_reply(">|running=%s addr=0x%06lx size_kb=%lu otadata=%s"
                      " reason=%s app_end=%s",
                      b->running, (unsigned long)b->address,
                      (unsigned long)(b->size / 1024U),
                      b->otadata_blank ? "blank" : "slot",
                      (3U > b->reason) ? REASONS[b->reason] : "?",
                      boot_app_end_name((kboot_app_end_t)b->app_end));
                console_reply(">ok");
                console_end();
            }
            status = Q_HANDLED();
            break;
        }
        case GUI_STATE_SIG: {
            if ( (uint8_t)KCLI_GUI == me->inflight )
            {
                GuiStateEvt const * const g = Q_EVT_CAST(GuiStateEvt);
                char qg[CONSOLE_QUOTED_MAX(sizeof(g->group))];
                char qs[CONSOLE_QUOTED_MAX(sizeof(g->sel))];
                char lock[KCLI_LOCK_WORD_MAX];
                const char *sheet = "-";
                const char *level = "root";
                console_reply(">|state=%s lvgl=%s pills=%u installed=%u files=%u"
                      " writing=%s",
                      g->state, g->screen_up ? "on" : "off",
                      (unsigned)g->pills, (unsigned)g->installed,
                      (unsigned)g->files, g->writing ? "on" : "off");
                if ( 0U == g->lock_s )
                {
                    (void)snprintf(lock, sizeof(lock), "%s", "never");
                }
                else
                {
                    (void)snprintf(lock, sizeof(lock), "%lu",
                                   (unsigned long)g->lock_s);
                }
                if ( '\0' != g->sheet[0] )
                {
                    sheet = g->sheet;
                }
                if ( KODE_GUI_LEVEL_GROUP == g->level )
                {
                    level = "group";
                }
                /* The selection, the launcher's level and open group, the
                 * sheet, Kody, the auto-lock and the stack. */
                console_reply(">|gui state=%s sel=%s level=%s group=%s sheet=%s"
                      " kody=%s lock_s=%s stack_free=%lu",
                      g->state, console_kv_text(qs, sizeof(qs), g->sel),
                      level, console_kv_text(qg, sizeof(qg), g->group), sheet,
                      g->kody, lock, (unsigned long)g->stack_free);
                /* What the finger has done since boot: the drag now and the
                 * recognizer's tallies. */
                console_reply(">|gesture drag=%s taps=%u claims=%u commits=%u"
                      " shorts=%u diagonal=%u axis=%u zone=%u blocked=%u",
                      gui_drag_name(g->gesture.drag),
                      (unsigned)g->gesture.taps, (unsigned)g->gesture.claims,
                      (unsigned)g->gesture.commits,
                      (unsigned)g->gesture.shorts,
                      (unsigned)g->gesture.diagonal,
                      (unsigned)g->gesture.axis, (unsigned)g->gesture.zone,
                      (unsigned)g->gesture.blocked);
                console_reply(">ok");
                console_end();
            }
            status = Q_HANDLED();
            break;
        }
        case CONFIG_ANS_SIG: {
            KodeConfigAnsEvt const * const a = Q_EVT_CAST(KodeConfigAnsEvt);
            /* Only the answer to the request in flight: a late one belongs
             * to a row that already timed out. */
            if ( ((uint8_t)KCLI_CONFIG == me->inflight) && (a->seq == me->cfg_seq) )
            {
                if ( (kode_status_t)a->status == KODE_OK )
                {
                    config_record(me->cfg_row, a->value, a->stored);
                }
                else if ( me->cfg_all )
                {
                    config_record(me->cfg_row, NULL, false);
                }
                else
                {
                    /* The owner refused the value. */
                }
                if ( me->cfg_all )
                {
                    config_next(me);
                }
                else if ( (kode_status_t)a->status == KODE_OK )
                {
                    console_reply(">ok");
                    console_end();
                }
                else
                {
                    console_reply_err(KCLI_ERR_REJECT, "value");
                    console_end();
                }
            }
            status = Q_HANDLED();
            break;
        }
        case FUNCTION_ANS_SIG: {
            FunctionAnsEvt const * const a = Q_EVT_CAST(FunctionAnsEvt);
            /* Only the answer of the run in flight: a function that timed
             * out has an older ticket, and its late end must not close the
             * line of the next one. */
            if ( ((uint8_t)KCLI_FUNCTION == me->inflight) && (a->ticket == s_fn_ticket) )
            {
                const char *name = "?";
                if ( '\0' != a->msg[0] )
                {
                    name = a->msg;
                }
                else if ( NULL != me->verb )
                {
                    name = me->verb;
                }
                else
                {
                    /* The word above. */
                }
                if ( '\0' != a->rec[0] )
                {
                    console_reply(">|%s", a->rec);
                }
                if ( a->done )
                {
                    function_close((kode_status_t)a->status, name);
                    console_end();
                }
            }
            status = Q_HANDLED();
            break;
        }
        case RECV_READY_SIG: {
            if ( (uint8_t)KCLI_RECV == me->inflight )
            {
                /* From here the receiving AO's idle watchdog
                 * (RECV_STALL_SIG) tells a slow host from a dead one; this
                 * AO's deadline stops. */
                console_unwatched();
                /* chunk= is the transfer's window: the host sends that many
                 * bytes, then waits for one `>|ack` before the next. */
                console_reply(">|ready chunk=%u", (unsigned)KODE_RECV_CHUNK);
            }
            status = Q_HANDLED();
            break;
        }
        case RECV_ACK_SIG: {
            if ( (uint8_t)KCLI_RECV == me->inflight )
            {
                console_reply(">|ack");
            }
            status = Q_HANDLED();
            break;
        }
        case APP_EVICTED_SIG: {
            /* Only a stream's: an install from a medium has answered `>ok`
             * long before, and says what it evicted in the log. */
            if ( (uint8_t)KCLI_RECV == me->inflight )
            {
                AppEvictedEvt const * const ev = Q_EVT_CAST(AppEvictedEvt);
                char q[CONSOLE_QUOTED_MAX(KODE_APP_PROJECT_MAX)];
                const char *kept = "off";
                if ( ev->kept )
                {
                    kept = "on";
                }
                console_reply(">|evicted name=%s kept=%s",
                              console_quote(q, sizeof(q), ev->name), kept);
            }
            status = Q_HANDLED();
            break;
        }
        case RECV_DONE_SIG: {
            if ( (uint8_t)KCLI_RECV == me->inflight )
            {
                console_reply(">ok");
                console_end();
            }
            status = Q_HANDLED();
            break;
        }
        case APP_REQ_RSP_SIG: {
            /* Only the install in flight hears it: a verdict that arrives
             * after the watchdog closed the command belongs to nobody. */
            if ( (uint8_t)KCLI_INSTALL == me->inflight )
            {
                console_reply_app_verdict(Q_EVT_CAST(AppReqRspEvt)->verdict);
            }
            status = Q_HANDLED();
            break;
        }
        case RECV_FAIL_SIG: {
            if ( (uint8_t)KCLI_RECV == me->inflight )
            {
                console_reply_err(KCLI_ERR_REJECT, me->verb);
                console_end();
            }
            status = Q_HANDLED();
            break;
        }
        case RECV_STALL_SIG: {
            if ( (uint8_t)KCLI_RECV == me->inflight )
            {
                console_reply_err(KCLI_ERR_TIMEOUT, me->verb);
                console_end();
            }
            status = Q_HANDLED();
            break;
        }
        case RECV_BUSY_SIG: {
            /* AO_Storage's transfer in flight outlasted the wait of this
             * `file_put`: nothing was armed, the host sends no byte. */
            if ( (uint8_t)KCLI_RECV == me->inflight )
            {
                console_reply_err(KCLI_ERR_BUSY, "busy");
                console_end();
            }
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODEOS_SHOT_HTTP
        case NET_SHOT_RSP_SIG: {
            /* Only the order in flight hears it. AO_Net can answer a capture
             * after the 20 s this AO waited for it, when the console is idle
             * or busy with something else, and that answer belongs to nobody:
             * it is not printed and it closes nothing. */
            if ( (uint8_t)KCLI_SHOT_HTTP == me->inflight )
            {
                shot_http_answer(Q_EVT_CAST(NetShotRspEvt));
                console_end();
            }
            status = Q_HANDLED();
            break;
        }
#endif
        case DISPLAY_CAPTURE_RSP_SIG: {
            bool const to_file = ((uint8_t)KCLI_SHOT_FILE == me->inflight);
            if ( to_file || ((uint8_t)KCLI_SHOT == me->inflight) )
            {
                DisplayCaptureRspEvt const * const r =
                    Q_EVT_CAST(DisplayCaptureRspEvt);
                const uint8_t * const img = display_capture_buf();

                if ( ((uint8_t)KODE_OK != r->status) || (NULL == img) )
                {
                    /* Built without KODEOS_SCREENSHOT, no PSRAM for the
                     * buffer, or a capture already open. Nothing was frozen,
                     * so there is nothing to release. */
                    console_reply_err(KCLI_ERR_REJECT, me->verb);
                    console_end();
                }
                else
                {
                    if ( !r->painted )
                    {
                        /* Not an error (the glass is black too), but say
                         * why the picture is. */
                        KLOG_W(TAG, "nothing drawn yet; the capture is black");
                    }
                    if ( to_file )
                    {
                        storage_write_all_request(me->file_name, img, r->len,
                                                  &me->super);
                    }
                    else
                    {
                        console_reply(">|shot w=%u h=%u fmt=bmp16 len=%lu",
                              (unsigned)r->w, (unsigned)r->h,
                              (unsigned long)r->len);
                        me->pay.run     = img;
                        me->pay.run_len = r->len;
                        me->pay.pos     = 0U;
                        me->pay.expect  = r->len;
                        payload_pump(me);
                    }
                }
            }
            status = Q_HANDLED();
            break;
        }
        case STORAGE_WRITE_RSP_SIG: {
            if ( (uint8_t)KCLI_SHOT_FILE == me->inflight )
            {
                StorageWriteRspEvt const * const r =
                    Q_EVT_CAST(StorageWriteRspEvt);
                /* AO_Storage has finished reading the capture, so this is
                 * the first moment it can be let go. */
                payload_release(me);
                if ( (uint8_t)KODE_OK == r->status )
                {
                    char qn[CONSOLE_QUOTED_MAX(sizeof(me->file_name))];
                    console_reply(">|shot name=%s bytes=%lu medium=%s",
                          console_quote(qn, sizeof(qn), me->file_name),
                          (unsigned long)r->written,
                          medium_name(r->medium));
                    console_reply(">ok");
                }
                else
                {
                    console_reply_err(KCLI_ERR_REJECT, me->verb);
                }
                console_end();
            }
            status = Q_HANDLED();
            break;
        }
        case STORAGE_OPEN_RSP_SIG: {
            if ( (uint8_t)KCLI_FILE == me->inflight )
            {
                StorageOpenRspEvt const * const r =
                    Q_EVT_CAST(StorageOpenRspEvt);
                if ( (uint8_t)KODE_OK != r->status )
                {
                    /* Nowhere to be found, empty, or somebody else has the
                     * one handle. Nothing was opened, so nothing to close. */
                    console_reply_err(KCLI_ERR_REJECT, me->verb);
                    console_end();
                }
                else
                {
                    me->pay.expect = r->size;
                    char qn[CONSOLE_QUOTED_MAX(sizeof(me->file_name))];
                    console_reply(">|file name=%s size=%lu medium=%s",
                          console_quote(qn, sizeof(qn), me->file_name),
                          (unsigned long)r->size,
                          medium_name(r->medium));
                    (void)QTimeEvt_rearm(&me->tmo,
                                  KQP_TICKS_PER_MS(KCLI_CMD_TIMEOUT_MS));
                    storage_read_request();
                }
            }
            status = Q_HANDLED();
            break;
        }
        case STORAGE_DESC_RSP_SIG: {
            if ( (((uint8_t)KCLI_APPDATA_LS == me->inflight)
                 || ((uint8_t)KCLI_APPDATA_INFO == me->inflight))
                && ((uint8_t)KCLI_AD_DESC == me->ad_phase) )
            {
                ad_desc(me, Q_EVT_CAST(StorageDescRspEvt));
            }
            else
            {
                /* Stale: posted answers arrive whatever is in flight. */
            }
            status = Q_HANDLED();
            break;
        }
        /* The folder conversation, posted here by AO_Storage. `appdata_ls`
         * keeps the statuses and prints its one record on the end marker;
         * `appdata_info` prints its seven the moment the fields arrive,
         * because that event is the whole of what it was asked for. */
        case APPDATA_FIELDS_SIG: {
            AppDataFieldsEvt const * const f = Q_EVT_CAST(AppDataFieldsEvt);
            if ( (uint8_t)KCLI_APPDATA_LS == me->inflight )
            {
                me->ad_medium = f->medium;
                me->ad_json   = f->json_status;
                me->ad_icon   = f->icon_status;
            }
            else if ( (uint8_t)KCLI_APPDATA_INFO == me->inflight )
            {
                ad_info_records(me, f);
            }
            else
            {
                /* The launcher's conversation, or a stale one. */
            }
            status = Q_HANDLED();
            break;
        }
        case APPDATA_ICON_SIG: {
            /* Not wanted: the console reports what an icon IS, and the bytes
             * are the launcher's. QP frees the block on return. */
            status = Q_HANDLED();
            break;
        }
        case APPDATA_END_SIG: {
            if ( (uint8_t)KCLI_APPDATA_LS == me->inflight )
            {
                ad_ls_record(me);
                ++me->ad_at;
                ad_ask_next(me);
            }
            else if ( (uint8_t)KCLI_APPDATA_INFO == me->inflight )
            {
                ad_ask_next(me);
            }
            else
            {
                /* Not ours. */
            }
            status = Q_HANDLED();
            break;
        }
        case APPDATA_GET_RSP_SIG: {
            if ( (uint8_t)KCLI_APPDATA_GET == me->inflight )
            {
                AppDataGetRspEvt const * const g = Q_EVT_CAST(AppDataGetRspEvt);

                if ( (uint8_t)APPDATA_GET_OK == g->why )
                {
                    char        q[CONSOLE_QUOTED_MAX(sizeof(g->project))];
                    const char *icon = "none";

                    if ( g->icon )
                    {
                        icon = "ok";
                    }
                    console_reply(">|appdata project=%s json=ok icon=%s",
                                  console_quote(q, sizeof(q), g->project), icon);
                    console_reply(">ok");
                }
                else if ( (uint8_t)APPDATA_GET_BUSY == g->why )
                {
                    /* The one refusal that asking again cures: the code every
                     * other verb gives a busy answer. */
                    console_reply_err(KCLI_ERR_BUSY, ad_get_word(g->why));
                }
                else
                {
                    console_reply_err(KCLI_ERR_REJECT, ad_get_word(g->why));
                }
                console_end();
            }
            status = Q_HANDLED();
            break;
        }
        case APPDATA_RM_RSP_SIG: {
            if ( (uint8_t)KCLI_APPDATA_RM == me->inflight )
            {
                switch ( (kode_status_t)Q_EVT_CAST(AppDataRmRspEvt)->status )
                {
                    case KODE_OK: {
                        console_reply(">ok");
                        break;
                    }
                    case KODE_DENIED: {
                        console_reply_err(KCLI_ERR_REJECT, "busy");
                        break;
                    }
                    case KODE_IO_ERROR: {
                        console_reply_err(KCLI_ERR_REJECT, "notempty");
                        break;
                    }
                    default: {
                        console_reply_err(KCLI_ERR_REJECT, "missing");
                        break;
                    }
                }
                console_end();
            }
            status = Q_HANDLED();
            break;
        }
        case STORAGE_DATA_SIG: {
            if ( (uint8_t)KCLI_FILE == me->inflight )
            {
                StorageDataEvt const * const d = Q_EVT_CAST(StorageDataEvt);
                if ( (uint8_t)KODE_OK != d->status )
                {
                    payload_fail(me, KCLI_ERR_REJECT);
                }
                else if ( d->eof )
                {
                    payload_done(me);
                }
                else
                {
                    me->pay.idx      = d->idx;
                    me->pay.borrowed = true;
                    me->pay.run      = storage_buf(d->idx);
                    me->pay.run_len  = d->len;
                    me->pay.pos      = 0U;
                    payload_pump(me);
                }
            }
            status = Q_HANDLED();
            break;
        }
        case CONSOLE_SLICE_SIG: {
            /* Only ours if the producer that armed it is still in flight: a
             * timeout could have closed it while the tick was on its way. */
            if ( ((uint8_t)KCLI_SHOT == me->inflight)
                || ((uint8_t)KCLI_FILE == me->inflight) )
            {
                payload_pump(me);
            }
            else if ( (uint8_t)KCLI_HELP == me->inflight )
            {
                if ( console_congested() )
                {
                    /* The slow port has not caught up: wait rather than
                     * overfill its ring. The watchdog measures progress, so a
                     * slow link may take its time and a dead one still closes. */
                    (void)QTimeEvt_rearm(&me->tmo,
                                  KQP_TICKS_PER_MS(KCLI_CMD_TIMEOUT_MS));
                    QTimeEvt_armX(&me->pacer, KCLI_SLICE_GAP_TICKS, 0U);
                }
                else if ( help_slice(me) )
                {
                    console_reply(">ok");
                    console_end();
                }
                else
                {
                    /* rearm, not armX: the watchdog cmd_begin() set is still
                     * armed, and QP asserts on arming an armed time event. */
                    (void)QTimeEvt_rearm(&me->tmo,
                                  KQP_TICKS_PER_MS(KCLI_CMD_TIMEOUT_MS));
                    QTimeEvt_armX(&me->pacer, KCLI_SLICE_GAP_TICKS, 0U);
                }
            }
            else if ( (uint8_t)KCLI_I2C_SCAN == me->inflight )
            {
                /* KCLI_PS's shape over the bus: a chunk of addresses a tick. */
                if ( console_congested() )
                {
                    (void)QTimeEvt_rearm(&me->tmo,
                                  KQP_TICKS_PER_MS(KCLI_CMD_TIMEOUT_MS));
                    QTimeEvt_armX(&me->pacer, KCLI_SLICE_GAP_TICKS, 0U);
                }
                else if ( i2c_scan_step(me) )
                {
                    console_reply(">ok");
                    console_end();
                }
                else
                {
                    (void)QTimeEvt_rearm(&me->tmo,
                                  KQP_TICKS_PER_MS(KCLI_CMD_TIMEOUT_MS));
                    QTimeEvt_armX(&me->pacer, KCLI_SLICE_GAP_TICKS, 0U);
                }
            }
            else if ( (uint8_t)KCLI_PS == me->inflight )
            {
                /* Exactly KCLI_HELP's shape over a different listing: wait
                 * out a congested port, keep the watchdog measuring progress
                 * rather than total time, and close on the last slice. */
                if ( console_congested() )
                {
                    (void)QTimeEvt_rearm(&me->tmo,
                                  KQP_TICKS_PER_MS(KCLI_CMD_TIMEOUT_MS));
                    QTimeEvt_armX(&me->pacer, KCLI_SLICE_GAP_TICKS, 0U);
                }
                else if ( ps_slice(me) )
                {
                    console_reply(">ok");
                    console_end();
                }
                else
                {
                    (void)QTimeEvt_rearm(&me->tmo,
                                  KQP_TICKS_PER_MS(KCLI_CMD_TIMEOUT_MS));
                    QTimeEvt_armX(&me->pacer, KCLI_SLICE_GAP_TICKS, 0U);
                }
            }
            else if ( ((uint8_t)KCLI_VARIABLE_LIST == me->inflight)
                     || ((uint8_t)KCLI_FUNCTION_LIST == me->inflight) )
            {
                /* KCLI_HELP's shape over the app's tables. */
                if ( console_congested() )
                {
                    (void)QTimeEvt_rearm(&me->tmo,
                                  KQP_TICKS_PER_MS(KCLI_CMD_TIMEOUT_MS));
                    QTimeEvt_armX(&me->pacer, KCLI_SLICE_GAP_TICKS, 0U);
                }
                else if ( list_slice(me) )
                {
                    console_reply(">ok");
                    console_end();
                }
                else
                {
                    (void)QTimeEvt_rearm(&me->tmo,
                                  KQP_TICKS_PER_MS(KCLI_CMD_TIMEOUT_MS));
                    QTimeEvt_armX(&me->pacer, KCLI_SLICE_GAP_TICKS, 0U);
                }
            }
            else if ( (uint8_t)KCLI_FIELD_HELP == me->inflight )
            {
                /* Same slicing as KCLI_HELP above, over a chip's field table
                 * instead of the command table. */
                if ( console_congested() )
                {
                    (void)QTimeEvt_rearm(&me->tmo,
                                  KQP_TICKS_PER_MS(KCLI_CMD_TIMEOUT_MS));
                    QTimeEvt_armX(&me->pacer, KCLI_SLICE_GAP_TICKS, 0U);
                }
                else if ( field_help_slice(me) )
                {
                    console_reply(">ok");
                    console_end();
                }
                else
                {
                    (void)QTimeEvt_rearm(&me->tmo,
                                  KQP_TICKS_PER_MS(KCLI_CMD_TIMEOUT_MS));
                    QTimeEvt_armX(&me->pacer, KCLI_SLICE_GAP_TICKS, 0U);
                }
            }
            status = Q_HANDLED();
            break;
        }
        case CONSOLE_WATCH_SIG: {
            /* A QTimeEvt is delivered as itself, so the event's address says
             * which slot's period ran out. A slot freed between the tick
             * being posted and this step has no row and is skipped. */
            for ( size_t i = 0U; KODE_VARIABLE_WATCH_MAX > i; ++i )
            {
                if ( (e == &me->watch[i].tev.super) && (NULL != me->watch[i].row) )
                {
                    watch_tick(&me->watch[i]);
                }
            }
            status = Q_HANDLED();
            break;
        }
        case CONSOLE_TIMEOUT_SIG: {
            /* The watchdog fired: name the command and free the console; late
             * answers are ignored by the in-flight gates. A silent `test` row
             * is timed out first by AO_Autotest, inside its own budget. */
            if ( ((uint8_t)KCLI_CONFIG == me->inflight) && me->cfg_all )
            {
                /* One owner that did not answer does not end the listing:
                 * its row says so, and the next one is asked. */
                config_record(me->cfg_row, NULL, false);
                config_next(me);
            }
            else if ( ((uint8_t)KCLI_IDLE != me->inflight) && (NULL != me->owner)
                     && (NULL != me->owner->timeout) && me->owner->timeout() )
            {
                /* Running out the time was the verb's own end: the owner
                 * has closed it, `ir_rx`'s window the one today. */
                console_end();
            }
            else if ( (uint8_t)KCLI_IDLE != me->inflight )
            {
                console_reply_err(KCLI_ERR_TIMEOUT,
                          console_verb());
                /* Before the kind is cleared, because that is what it reads
                 * to know what it is holding. A capture abandoned frozen
                 * would mirror this frame for every capture after it. */
                payload_release(me);
                me->inflight = (uint8_t)KCLI_IDLE;
            }
            status = Q_HANDLED();
            break;
        }
        case KCLI_INJ_TMO_SIG: {
            /* A tap owes its release, or a swipe a poll period. A stale tick
             * finds nothing in flight. One queued behind a new gesture cannot
             * be told apart: it lifts the new tap early, or steps the new
             * swipe one poll period ahead. */
            if ( KCLI_INJ_TAP == me->inj )
            {
                inj_release(me, khal_time_ms());
            }
            else if ( KCLI_INJ_SWIPE == me->inj )
            {
                inj_swipe_step(me);
            }
            else
            {
                /* Stale. */
            }
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODEOS_WIFI
        case WIFI_FRAMES_TICK_SIG: {
            /* Ticks are posted to the asker alone, and no `wifi_frames` run
             * reads this one: a run ended while AO_Radio still held the
             * console as its asker (its stop never got in). The stream has
             * nobody now, so it is stopped; AO_Radio takes a stop that comes
             * again while one waits as the same one, and a refused post is
             * asked again by the next tick. Its answer comes here and is
             * nobody's. */
            (void)wifi_frames_request(&me->super, false, 0U);
            status = Q_HANDLED();
            break;
        }
#endif
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            bool const moving = ((uint8_t)KCLI_RECV == me->inflight)
                                || ((uint8_t)KCLI_SHOT == me->inflight)
                                || ((uint8_t)KCLI_FILE == me->inflight);

            lowpower_ready(me->lp_who, Q_EVT_CAST(LowPowerRoundEvt)->round,
                           moving, "console transfer");
            status = Q_HANDLED();
            break;
        }
#endif
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }
    return status;
}

/** @brief The console's only state: lines in, verbs run, replies out. */
static QState Console_active(Console * const me, QEvt const * const e)
{
    QState status;

    /* A registered verb's answer is its owner's to read, first: a signal the
     * console also reads (a listing, a test verdict) is the owner's while
     * the owner's verb holds the console, and the console's own otherwise. */
    if ( ((uint8_t)KCLI_IDLE != me->inflight) && (NULL != me->owner)
        && (NULL != me->owner->answer) && me->owner->answer(e) )
    {
        status = Q_HANDLED();
    }
    else
    {
        status = console_own(me, e);
    }
    /* Whatever this event answered, it answered before here: a command that
     * is still running keeps its port for the events to come, and one that
     * has closed leaves nothing behind for a record that belongs to nobody. */
    if ( (uint8_t)KCLI_IDLE == me->inflight )
    {
        me->cmd_src = KODE_LOG_SRC_NONE;
        me->owner   = NULL;
    }
    return status;
}
