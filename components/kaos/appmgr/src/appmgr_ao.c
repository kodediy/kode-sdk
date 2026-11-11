/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file appmgr_ao.c
 * @brief AO_AppMgr: owner of the apps region.
 */
#include "kaos.h"
#include "kaos_start.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "kqp.h"
#include "kaos_claim.h"
#include "kapp_cat.h"
#include "kdrv_apps.h"
#include "kdrv_kfw.h"
#include "kapp_data.h"
#include "sdkconfig.h"
#include "kode_log.h"
#include "khal_crc32.h"
#include "khal_system.h"
#include "kaos_claim.h"
#include "kverbs.h"

#define TAG               "kaos.appmgr"
#define APPMGR_PROGRESS   (10U)          /* log every N percent; see progress_report() */
/* Idle time between chunks. AppMgr outranks the GUI, so without a gap it
 * would hold the CPU for the whole write and the interface would crawl. 10 ms
 * costs ~1.6s over a 5MB image - cheap for a UI that stays alive. */
#define APPMGR_CHUNK_GAP_TICKS KQP_TICKS_PER_MS(10U)
/* No chunk for this long during an install and the host is presumed gone -
 * KODE_RECV_IDLE_MS (kqp_config.h) in ticks, this AO's own watchdog on the
 * command (README.md "The console": AO_Console disarms its 5 s one for the
 * length of this command and trusts this one instead - a 5 MB image over a
 * slow link runs for far longer than 5 s, and only the AO watching the
 * stream can tell "slow" from "dead"). */
#define APPMGR_RECV_IDLE_TICKS  KQP_TICKS_PER_MS(KODE_RECV_IDLE_MS)
/* Insurance on the arming handshake itself, armed at the moment this AO
 * enters AppMgr_receiving: if the console's yes never arrives (its answer
 * event could not be allocated - the one path with nobody left to speak),
 * this is what gets the AO back to idle instead of stranding it forever.
 * Generous on purpose: the region work that runs before the deadline is
 * re-tightened takes seconds at worst, never half a minute. */
#define APPMGR_RECV_LONGSTOP_TICKS KQP_TICKS_PER_MS(30000U)
/* The same long-stop for AppMgr_opening, which waits on AO_Storage or AO_Net:
 * a lost answer must not leave the AO busy until a reset. Generous, because an
 * HTTP open can spend real time in DNS and a TLS handshake. */
#define APPMGR_OPEN_TICKS          KQP_TICKS_PER_MS(KODE_APPMGR_OPEN_MS)
/* The same long-stop again, between two chunks of a write off a medium. A
 * chunk that never arrives - a request or a data event lost on a dry pool -
 * left AppMgr_flashing writing forever, with the reader still held. Above
 * the network stream's own 15 s, so a server that went quiet is reported
 * by AO_Net first, as what it is. */
#define APPMGR_CHUNK_TICKS         KQP_TICKS_PER_MS(30000U)
/* Between "the image is installed" and the reset that boots it: long enough
 * for AO_Log's lower-priority task to put `>ok` on the wire first. */
#define APPMGR_ANSWER_DRAIN_TICKS KQP_TICKS_PER_MS(50U)
/* Region bytes read per step while an evicted app is copied out of the
 * region: the same 32 KB the writes use, so a step stays short (rule 5). */
#define APPMGR_KEEP_CHUNK      (32U * 1024U)
/* Re-publish the percent this often while the card writes a copy. AO_Power
 * takes the LED's blue pulse back after three seconds without a report, and
 * a multi-MB write has none of its own. */
#define APPMGR_KEEP_BEAT_TICKS KQP_TICKS_PER_MS(1000U)
/* How long the card may take over one copy before it is given up on: a base
 * that covers mounting and a slow counterfeit card, plus a second for every
 * 128 KB, so a 13 MB extent gets nearly three minutes and a small one a
 * minute. The state has no other way out of a request AO_Storage dropped on
 * a dry pool, which it does without an answer. */
#define APPMGR_KEEP_WRITE_BASE_S   (60U)          /**< Seconds any copy gets.  */
#define APPMGR_KEEP_WRITE_PER_S_B  (128U * 1024U) /**< Bytes per extra second. */
#define APPMGR_KEEP_WRITE_TICKS(len_) \
    KQP_TICKS_PER_MS((APPMGR_KEEP_WRITE_BASE_S \
                      + ((len_) / APPMGR_KEEP_WRITE_PER_S_B)) * 1000U)

/* The events carry copies of strings whose arrays belong to kdrv, which kqp
 * sits below and cannot include: the sizes are twinned in kqp_config.h, and
 * this is the layer that sees both. */
_Static_assert(KODE_APP_PROJECT_MAX == KDRV_SLOT_PROJECT_MAX,
               "KODE_APP_PROJECT_MAX twins KDRV_SLOT_PROJECT_MAX");
_Static_assert(KODE_APP_DATE_MAX == KAPP_IDF_DATE_MAX,
               "KODE_APP_DATE_MAX twins KAPP_IDF_DATE_MAX");
_Static_assert(KODE_APP_TIME_MAX == KAPP_IDF_TIME_MAX,
               "KODE_APP_TIME_MAX twins KAPP_IDF_TIME_MAX");
/* The entry's date and time are copied out of the slot descriptor
 * kdrv_apps_desc() fills, and a descriptor field of another size would cut
 * them or leave room unused without a word. */
_Static_assert(KODE_APP_DATE_MAX == sizeof(((kdrv_slot_desc_t *)NULL)->date),
               "AppEntryEvt.build_date is the size of kdrv_slot_desc_t.date");
_Static_assert(KODE_APP_TIME_MAX == sizeof(((kdrv_slot_desc_t *)NULL)->time),
               "AppEntryEvt.build_time is the size of kdrv_slot_desc_t.time");
/* A fetch request carries a base URL: it has to stay out of the jumbo pool,
 * which a FlashReqEvt, the widest request of this AO, is asserted to (kqp.c). */
_Static_assert(sizeof(AppDataGetReqEvt) <= sizeof(FlashReqEvt),
               "AppDataGetReqEvt must not reach the jumbo pool");
_Static_assert(sizeof(AppDataGetRspEvt) <= sizeof(AppFindRspEvt),
               "AppDataGetRspEvt must fit the pool an AppFindRspEvt fits");
/* What a listing and an eviction answer hold at once, in medium-tier events:
 * AO_Gui can hold a page of apps and a page of files (KODE_STORAGE_LIST_MAX
 * entries and an end each) while an eviction answer (KODE_APP_EVICT_MAX and an
 * end) arrives. The pool must outlive the three together. */
_Static_assert(((2U * (KODE_STORAGE_LIST_MAX + 1U)) + (KODE_APP_EVICT_MAX + 1U))
               < KQP_MEDIUM_POOL_COUNT,
               "the medium pool must outlive two pages and an eviction answer");
/* AppEvictEndEvt.victims is a byte: every directory slot, and the one the
 * directory itself would take once it is full. */
_Static_assert((KAPPS_MAX_ENTRIES + 1U) <= UINT8_MAX,
               "AppEvictEndEvt.victims must hold every victim of a plan");

/* How long AO_Storage gets to answer one folder request of the tidy step
 * (APPMGR_TIDY_MS, kaos.h) before this AO gives up on it and goes on. The
 * install has already succeeded when the wait starts, so the budget only
 * bounds how long a silent AO_Storage can hold the apps region's owner and
 * the install's end. */
#define APPMGR_TIDY_TICKS   KQP_TICKS_PER_MS(APPMGR_TIDY_MS)   /**< APPMGR_TIDY_MS (kaos.h) in ticks. */
#define APPMGR_WIPE_TICKS   KQP_TICKS_PER_MS(APPMGR_WIPE_MS)   /**< APPMGR_WIPE_MS (kaos.h) in ticks. */

/* An app's data folder fetched from a URL (APPDATA_GET_REQ_SIG): its two
 * files, by the names the folder gives them. */
#define GET_JSON_NAME   KAPP_DATA_JSON_NAME   /**< The folder's first file.               */
#define GET_ICON_NAME   KAPP_DATA_ICON_NAME   /**< Its second, and what app.json calls it. */

/* Which part of a fetch is out. */
enum {
    GET_STEP_JSON = 0,   /**< app.json is being read from the URL.   */
    GET_STEP_ICON,       /**< icon.k64 is.                           */
    GET_STEP_PUT         /**< AO_Storage is writing the folder.      */
};

/* Immutable (statically allocated) outcome events, published on the QF bus.
 * Static events are never garbage-collected, so publishing them repeatedly is
 * safe. The done is the exception: it names what was installed, so
 * flash_done_publish() allocates one, and this unnamed instance goes out only
 * when the pool is dry - the end of a write still has to arrive. */
static const AppFlashDoneEvt s_flash_done_evt = {
    QEVT_INITIALIZER(APP_FLASH_DONE_SIG), ""
};
static const AppFlashFailEvt s_flash_fail_evt = {
    QEVT_INITIALIZER(APP_FLASH_FAIL_SIG), (uint8_t)KFAIL_WRITE
};
/* Posted to self, one per removed app's header (AppMgr_scrubbing): each erase
 * is a step of its own, so a request that arrives between two is answered. */
static const QEvt s_keep_only_step_evt = QEVT_INITIALIZER(APP_KEEP_ONLY_STEP_SIG);
static const QEvt s_recv_ready_evt = QEVT_INITIALIZER(RECV_READY_SIG);
static const QEvt s_recv_ack_evt   = QEVT_INITIALIZER(RECV_ACK_SIG);
static const QEvt s_recv_done_evt  = QEVT_INITIALIZER(RECV_DONE_SIG);
static const QEvt s_recv_fail_evt  = QEVT_INITIALIZER(RECV_FAIL_SIG);
static const QEvt s_recv_stall_evt = QEVT_INITIALIZER(RECV_STALL_SIG);
/* Posted to AO_Log, not published: only AppMgr_receiving's idle watchdog
 * sends this, to abandon raw mode when the reader task may still be mid-
 * transfer (a transfer that finished on the wire disarms raw mode itself). */
static const QEvt s_raw_end_evt    = QEVT_INITIALIZER(CONSOLE_RAW_END_SIG);

/* Where the install in hand came from (spec, "Sources"): what AppMgr.origin
 * holds. Constants and no type of their own - the field is a uint8_t, as the
 * events' enums are, and a type nothing names is MISRA 2.3. */
enum {
    APPMGR_ORIGIN_FILE = 0,  /**< A file on the internal FAT.                 */
    APPMGR_ORIGIN_CARD,      /**< A file on the card: a failed copy cancels.  */
    APPMGR_ORIGIN_NET,       /**< A URL.                                      */
    APPMGR_ORIGIN_MAILBOX,   /**< app_pending.                                */
    APPMGR_ORIGIN_CONSOLE    /**< A terminal install: cannot restart, copies
                              *   nothing.                                    */
};

/* How an install ends once its folders are in order (tidy_begin()). Constants
 * and no type, as the origins above. */
enum {
    TIDY_AFTER_FLASH = 0,    /**< From a medium: the done, then the launch.   */
    TIDY_AFTER_RECV,         /**< From the console: the done, the `>ok`, then
                              *   the launch.                                 */
    TIDY_AFTER_ADOPT,        /**< From the mailbox: the boot request.        */
    TIDY_AFTER_ERASE,        /**< An app erased: its files and its folder go
                              *   with it, on both media.                    */
    TIDY_AFTER_KEEP          /**< Every app but one erased: the folders go,
                              *   then the asker is answered.                 */
};

/* Which folder request is out, and so which answer settles it. */
enum {
    TIDY_STEP_NONE = 0,      /**< None: nothing is owed.                      */
    TIDY_STEP_PUT,           /**< The installed app's folder is being written. */
    TIDY_STEP_RM_OWN,        /**< The installed app's folder is being deleted. */
    TIDY_STEP_RM_VICTIM,     /**< An evicted app's folder is being deleted.   */
    TIDY_STEP_WIPE           /**< An erased app's files are being taken off
                              *   both media.                                 */
};

/** @brief What became of one victim's copy. */
typedef enum {
    KEEP_UNTRIED = 0,
    KEEP_SAVED,              /**< Written to the card now.                    */
    KEEP_THERE,              /**< Its file was already on the card.           */
    KEEP_FAILED              /**< No card, no memory, no name, or the write failed. */
} keep_res_t;

/** @brief The apps-region AO, plus the state of one in-flight copy. */
typedef struct {
    QActive  super;
    QTimeEvt pacer;          /**< Spaces the chunk requests; see above.      */
    char     file[KODE_APP_FILE_MAX];
    bool     writing;        /**< kdrv_apps holds an open write session.     */
    /* Which borrowed buffers are waiting to go back, and whether the pacer is
     * already counting. Two buffers can be in flight, so one index is not
     * enough: the pacer would be armed twice and one buffer refilled twice. */
    uint8_t  to_return;      /**< Bitmask of buffer indices to hand back.    */
    bool     pacing;         /**< The pacer is armed; do not arm it again.   */
    size_t   total;
    /* What the sink has produced: kfw_produced(), or the fed length in bypass
     * mode. region_install() records it as the extent's size, and every
     * progress percentage is taken from it. AppMgr_receiving counts the wire
     * apart, in recv_wire. */
    size_t   written;
    unsigned next_report;    /**< Next percentage worth logging.             */
    unsigned reported;       /**< Last percentage published on the bus.      */
    bool     launch;         /**< Boot the slot once written.                */
    uint32_t dst_offset;     /**< Region-relative offset this write targets. */
    /* Which of the two destinations this conversation with AO_Storage is
     * feeding: the apps region (false, the default) or the inactive kodeOS
     * OTA slot (true). One conversation, one struct, two
     * places the bytes can land - see the four branch points in
     * AppMgr_opening/consume_chunk/flash_finish/flash_cleanup. */
    bool     os_target;
    /* The destination the four write sites call. os_target still decides what
     * happens after the write (AO_Boot selects otadata and resets) and what the
     * progress event reports. */
    const kfw_sink_t     *sink;      /**< Never NULL while writing.          */
    kdrv_apps_sink_ctx_t  sink_ctx;  /**< Only the region sink reads it.     */
    /* dst=pending parks the container verbatim, so the decoder is out of the
     * path for that one destination - see kdrv_apps_pending_sink(). Reading
     * it back later is what verifies it, which is the point. */
    bool                  bypass;
    /* src=pending with an explicit dst: the intent sector is the host tool's
     * way of saying where an image goes, and an operator who typed dst=
     * has already said it more clearly. */
    bool                  dst_forced;
    /* What an adoption the console asked for does once the app is in:
     * `install src="pending" dst=app`'s `run`. Read only when keep_forced
     * says the console asked; a boot-time adoption is always sticky. */
    bool                  adopt_run;
    /* Which medium the chunks come from. A URL and a file are the same
     * conversation - open, response, data with a lent buffer, return - so
     * this only decides which AO to talk to, not how. */
    bool                  from_net;
    /* The mailbox copy reads by offset, so it needs to count what it has
     * CONSUMED from the source - which since the decoder arrived is no
     * longer the same number as what has been produced into the sink. One
     * field doing both jobs read the container back from the wrong offset
     * the first time a compressed one was parked. */
    uint32_t              src_len;    /**< Bytes the mailbox holds.        */
    uint32_t              consumed;   /**< Bytes read out of it so far.    */

    /* ---- An image arriving on the console (APP_RECV_REQ_SIG) ------------ */
    QTimeEvt idle;           /**< No chunk for this long: the host is gone.  */
    QTimeEvt answer;         /**< Lets the `>ok` reach the wire before the
                              *   board resets into the new app; see
                              *   AppMgr_launching.                          */
    uint32_t recv_crc;       /**< Running CRC over what has arrived.         */
    char     recv_name[KODE_APP_PROJECT_MAX]; /**< The project the host says
                              *   the image is, or "": the app it replaces. */
    char     project[KODE_APP_PROJECT_MAX];   /**< The app being written, by
                              *   its project name, for the reports; ""
                              *   until it is known (project_note()).       */
    uint32_t recv_want;      /**< What that CRC must equal at the end.       */
    /* The chunk buffer of the two transfers that bring their own bytes,
     * adoption and a console stream. A stream fills a whole KODE_RECV_CHUNK
     * before each flash write, because a write freezes the reader task. PSRAM,
     * taken once by AppMgr_initial; one buffer serves both, since neither
     * starts while the other runs. NULL if PSRAM had no 4 KB at boot: both
     * transfers are refused. */
    uint8_t *chunk;
    uint32_t recv_fill;      /**< Bytes staged in `chunk`, not yet written. */
    /* Bytes the host has put on the wire this transfer, counted as they are
     * copied into `chunk`. The completion check compares this, not `written`,
     * with the announced size: a failed flash write does not move `written`,
     * and the transfer would end as a stall instead of a write error. */
    uint32_t recv_wire;
    /* The size the console announced: what the wire carries, and the only
     * number recv_wire is counted against. `total` is the image's length once
     * the decoder has placed it (appmgr_place()), which is shorter than the
     * wire for a container (its header, its block) and longer for a deflated
     * one; the two were one field, and a container ended its transfer at the
     * wrong byte. */
    uint32_t recv_size;
    bool     recv_ok;        /**< false once a write fails mid-stream; the
                              *   remaining chunks are still drained (the
                              *   host's byte count is the wire's only
                              *   boundary) but no longer written.           */

    /* ---- Keeping victims on the card before an eviction ----------------
     * One round per install: the first plan that names victims abandons the
     * transfer, AppMgr_keeping copies them, and the install starts again
     * from the top with keep_round set, so the second plan only evicts.
     * Victims are known by seq: unique, and the directory cannot change
     * between the two plans - this AO is its only writer, and it is busy. */
    uint8_t     origin;                        /**< APPMGR_ORIGIN_*          */
    bool        keep_round;                    /**< The copies were made; the
                                                *   install running now is the
                                                *   restart.                  */
    bool        keep_cancel;                   /**< The person went back: the round ends the install. */
    bool        keep_pending;                  /**< appmgr_place() deferred:
                                                *   the feed failure that
                                                *   follows is not a failure. */
    bool        keep_forced;                   /**< Adoption had dst forced.   */
    bool        keep_writing;                  /**< AO_Storage holds keep_buf. */
    bool        keep_stat;                     /**< A STAT is out, unanswered. */
    /* Answers still owed for requests given up on at their deadline.
     * AO_Storage answers in order and its answers carry no path, so the
     * next that many answers of that kind are the abandoned requests', not
     * the victim's in hand. Kept across rounds: a late answer can outlive
     * the install it belonged to. */
    uint8_t     keep_stale_stat;               /**< STATs abandoned, owed.     */
    uint8_t     keep_stale_write;              /**< Writes abandoned, owed.    */
    uint8_t    *keep_orphan;                   /**< The buffer of the oldest
                                                *   abandoned write, freed when
                                                *   its answer comes.         */
    uint32_t    keep_seq[KAPPS_MAX_ENTRIES];   /**< Victims, eviction order.   */
    uint8_t     keep_res[KAPPS_MAX_ENTRIES];   /**< keep_res_t of each.        */
    uint32_t    keep_count;
    uint32_t    keep_idx;                      /**< The victim in hand.        */
    uint32_t    keep_total;                    /**< Bytes of every victim.     */
    uint32_t    keep_done;                     /**< Bytes copied or skipped.   */
    uint8_t    *keep_buf;                      /**< PSRAM: one victim's extent. */
    uint32_t    keep_len;                      /**< That extent's length.      */
    uint32_t    keep_off;                      /**< Its region offset.         */
    uint32_t    keep_fill;                     /**< Bytes of it read so far.   */
    char        keep_name[KDRV_SLOT_PROJECT_MAX]; /**< The victim's project name. */
    char        keep_path[KODE_APP_FILE_MAX];  /**< /sdcard/<project>.bin      */
    FlashReqEvt keep_req;                      /**< The request to restart.    */

    /* ---- A question about the next install (APP_EVICT_REQ_SIG) -----------
     * Asked and answered inside AppMgr_asking, without touching anything the
     * fields above hold: the victims are known by where they lie, and the
     * card is asked with the same STAT, so a late answer is owed through
     * keep_stale_stat like any other. */
    QActive    *ask_who;                       /**< Who asked; NULL between
                                                *   questions.                */
    uint32_t    ask_off[KODE_APP_EVICT_MAX];   /**< Where the victims to report
                                                *   lie, in the order they
                                                *   would go.                 */
    uint32_t    ask_count;                     /**< How many of them.          */
    uint32_t    ask_victims;                   /**< Every victim, reported or
                                                *   not: the end's count.     */
    uint32_t    ask_idx;                       /**< The victim in hand.        */
    bool        ask_stat;                      /**< A STAT is out, unanswered. */
    bool        ask_mute;                      /**< The card let a STAT go
                                                *   unanswered: the victims
                                                *   still to report are not
                                                *   asked about.              */
    char        ask_name[KDRV_SLOT_PROJECT_MAX]; /**< The victim in hand.      */
#if CONFIG_KODE_LOWPOWER
    uint8_t     lp_who;                        /**< The id lowpower_join() gave. */
#endif

    /* ---- A question about where an app is (APP_FIND_REQ_SIG) --------------
     * Asked and answered inside AppMgr_finding, which only an app that is not
     * installed enters: the card is asked with the same STAT, so a late answer
     * is owed through keep_stale_stat like any other. */
    QActive    *find_who;                      /**< Who asked; NULL between
                                                *   questions.                */
    bool        find_stat;                     /**< A STAT is out, unanswered. */
    char        find_name[KDRV_SLOT_PROJECT_MAX]; /**< The app asked after.    */

    /* ---- Keep only one app (APP_KEEP_ONLY_REQ_SIG) -----------------------
     * The directory is committed in one step, each victim's image header is
     * erased in a step of its own, and the victims' folders are what is left
     * for the tidy step, which deletes them one at a time. */
    QActive    *keep_only_who;                 /**< Who asked; answered when the
                                                *   last folder is dealt with. */
    uint8_t     keep_only_erased;              /**< Apps removed from the
                                                *   directory.                 */
    uint8_t     keep_only_next;                /**< The next removed app whose
                                                *   header is still to erase.  */
    uint32_t    keep_only_off[KAPPS_MAX_ENTRIES]; /**< Where the removed apps lay. */
    char        keep_only_name[KDRV_SLOT_PROJECT_MAX]; /**< The app that stays. */

    /* ---- Tidying: the folders, between the commit and the end -----------
     * The image is in the directory, but the install has not ended: the
     * app's metadata folder has to match what the container carried, and the
     * folders of the apps it evicted without a copy on the card have to go,
     * or the launcher would show the new app with the old one's icon (or an
     * orphan with no binary). One request is out at a time, under `tidy`. */
    QTimeEvt      tidy;            /**< Deadline of the request in flight.     */
    bool          tidy_on;         /**< AppMgr_tidying owns this install now.   */
    bool          tidy_launch;     /**< Finished: go to AppMgr_launching.       */
    bool          tidy_own_asked;  /**< The installed app's own request is made. */
    uint8_t       tidy_after;      /**< TIDY_AFTER_*: how the install ends.     */
    uint8_t       tidy_step;       /**< TIDY_STEP_*: which request is out.      */
    kfw_appdata_t tidy_ad;         /**< What the container said about the folder. */
    char          tidy_name[KDRV_SLOT_PROJECT_MAX]; /**< The project; "" when unreadable. */
    /* Nothing is held across a wait: a put hands its buffer to AO_Storage,
     * which frees it on every path, and an answer is taken only when it names
     * the request in flight (tidy_step and tidy_who()). */
    bool          tidy_swallow;    /**< A deadline already queued is stale: swallow it. */
    /* The evicted apps whose folders go, by project name: they are read off
     * the images before the install overwrites them. PSRAM, taken when the
     * first one is recorded (rule 8: KAPPS_MAX_ENTRIES names would be 2 KB of
     * internal RAM held for an event that rarely happens). */
    char        (*victims)[KDRV_SLOT_PROJECT_MAX];
    uint32_t      victim_count;    /**< Names recorded in victims[].            */
    uint32_t      victim_next;     /**< The next one to ask AO_Storage about.   */

    /* ---- The erase in hand (APP_ERASE_REQ_SIG) ----------------------------
     * What its end says (AppEraseDoneEvt), kept from the idle step that took
     * the image to the answer of AO_Storage's wipe; the name is tidy_name. */
    bool          erase_image;     /**< An installed image was removed.         */
    bool          erase_extra;     /**< Its override or a mailbox image went.   */
    uint8_t       erase_status;    /**< kode_status_t the end carries.          */
    AppWipeMedium erase_fs;        /**< What went on the internal partition.    */
    AppWipeMedium erase_sd;        /**< What went on the card.                  */

    /* ---- An app's data folder fetched from a URL (APPDATA_GET_REQ_SIG) ---
     * One buffer for both files, app.json then icon.k64, which is what
     * appdata_put_request() takes: PSRAM, this AO's until AO_Storage has it.
     * The base URL is kept in PSRAM too, for the icon's open (rule 8: 200
     * bytes of internal RAM held for a fetch that rarely happens). */
    QActive      *get_who;         /**< Who asked.                              */
    char          get_project[KODE_APP_PROJECT_MAX]; /**< Whose folder.         */
    char         *get_url;         /**< The base URL, KODE_APPDATA_URL_MAX of
                                    *   PSRAM; NULL outside a fetch.            */
    uint8_t      *get_buf;         /**< The two files; NULL when not this AO's. */
    uint16_t      get_json_len;    /**< Bytes of app.json in it.                */
    uint16_t      get_icon_len;    /**< Bytes of icon.k64 after them.           */
    uint32_t      get_want;        /**< What the file in hand said it brings.   */
    uint32_t      get_fill;        /**< How much of it has arrived.             */
    uint8_t       get_step;        /**< GET_STEP_*: which part is out.          */
    bool          get_open;        /**< A transfer is open with AO_Net.         */
} AppMgr;

static AppMgr l_appMgr;
QActive * const AO_AppMgr = &l_appMgr.super;

static QState AppMgr_initial(AppMgr * const me, void const * const par);
static QState AppMgr_idle(AppMgr * const me, QEvt const * const e);
static QState AppMgr_opening(AppMgr * const me, QEvt const * const e);
static QState AppMgr_flashing(AppMgr * const me, QEvt const * const e);
static QState AppMgr_adopting(AppMgr * const me, QEvt const * const e);
static QState AppMgr_keeping(AppMgr * const me, QEvt const * const e);
static QState AppMgr_busy(AppMgr * const me, QEvt const * const e);
static QState AppMgr_asking(AppMgr * const me, QEvt const * const e);
static QState AppMgr_finding(AppMgr * const me, QEvt const * const e);
static QState AppMgr_scrubbing(AppMgr * const me, QEvt const * const e);
static QState AppMgr_tidying(AppMgr * const me, QEvt const * const e);
static QState AppMgr_fetching(AppMgr * const me, QEvt const * const e);

/* The four transfer wrappers are used by flash_cleanup(), which is declared
 * ahead of them because it is what every failure path calls. */
static void          medium_read(AppMgr * const me);
static void          medium_close(AppMgr * const me);
static void          project_note(AppMgr * const me, const char *name);
static QState        get_begin(AppMgr * const me, AppDataGetReqEvt const * const req);
static const uint8_t *medium_buf(AppMgr * const me, uint8_t idx);
static void          medium_return(AppMgr * const me, uint8_t idx);
static kode_status_t xfer_begin(AppMgr * const me, uint8_t want, uint32_t src_len);
static kode_status_t xfer_feed(AppMgr * const me, const void *d, size_t len);
static kode_status_t xfer_end(AppMgr * const me);
static void          xfer_abort(AppMgr * const me);
static void          keep_pause(AppMgr * const me);
static bool          keep_owed_answer(AppMgr * const me, QEvt const * const e);
static QState        evict_ask(AppMgr * const me, QEvt const * const e);
static QState        find_ask(AppMgr * const me, QEvt const * const e);
static QState        keep_only_ask(AppMgr * const me, QEvt const * const e);
static QState AppMgr_receiving(AppMgr * const me, QEvt const * const e);
static QState AppMgr_launching(AppMgr * const me, QEvt const * const e);

/* ---- Request helpers (what the console and the GUI call) ----------------- */

bool appmgr_recv_request(uint8_t dst, uint32_t size, uint32_t crc32,
                         bool launch, uint8_t src, const char *name)
{
    AppRecvReqEvt *e = Q_NEW_X(AppRecvReqEvt, 0U, APP_RECV_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; install request dropped");
        return false;
    }
    e->dst    = dst;
    e->size   = size;
    e->crc32  = crc32;
    e->launch = launch;
    e->src    = src;
    e->name[0] = '\0';
    if ( NULL != name )
    {
        (void)snprintf(e->name, sizeof(e->name), "%s", name);
    }
    QACTIVE_POST(AO_AppMgr, &e->super, NULL);
    return true;
}

void appmgr_flash_request(const char *file, bool launch)
{
    appmgr_image_request((uint8_t)KFW_SRC_FS, file,
                         (uint8_t)KFW_TARGET_APP, launch);
}

void appmgr_cancel_request(void)
{
    static const QEvt cancel = QEVT_INITIALIZER(APP_CANCEL_REQ_SIG);

    QACTIVE_POST(AO_AppMgr, &cancel, NULL);
}

void appmgr_run_request(const char *name)
{
    AppFlashEvt *e = Q_NEW_X(AppFlashEvt, 0U, APP_RUN_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; run request dropped");
        return;
    }
    (void)snprintf(e->file, sizeof(e->file), "%s", name);
    QACTIVE_POST(AO_AppMgr, &e->super, NULL);
}

void appmgr_erase_request(const char *name)
{
    AppFlashEvt *e = Q_NEW_X(AppFlashEvt, 0U, APP_ERASE_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; erase request dropped");
        return;
    }
    (void)snprintf(e->file, sizeof(e->file), "%s", name);
    QACTIVE_POST(AO_AppMgr, &e->super, NULL);
}

bool appmgr_find_request(const char *project, QActive *who)
{
    if ( (NULL == project) || (NULL == who) )
    {
        return false;
    }
    AppFindReqEvt *e = Q_NEW_X(AppFindReqEvt, 0U, APP_FIND_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_W(TAG, "out of events; find request dropped");
        return false;
    }
    (void)snprintf(e->project, sizeof(e->project), "%s", project);
    e->who = who;
    if ( !QACTIVE_POST_X(AO_AppMgr, &e->super, 0U, NULL) )
    {
        KLOG_W(TAG, "AO_AppMgr's queue is full; find request dropped");
        return false;
    }
    return true;
}

bool appmgr_keep_only_request(const char *project, QActive *who)
{
    if ( (NULL == project) || (NULL == who) )
    {
        return false;
    }
    AppKeepOnlyReqEvt *e = Q_NEW_X(AppKeepOnlyReqEvt, 0U, APP_KEEP_ONLY_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_W(TAG, "out of events; keep-only request dropped");
        return false;
    }
    (void)snprintf(e->project, sizeof(e->project), "%s", project);
    e->who = who;
    if ( !QACTIVE_POST_X(AO_AppMgr, &e->super, 0U, NULL) )
    {
        KLOG_W(TAG, "AO_AppMgr's queue is full; keep-only request dropped");
        return false;
    }
    return true;
}

kode_status_t appmgr_appdata_get_request(const char *project, const char *url,
                                         QActive *who)
{
    if ( (NULL == project) || (NULL == url) || (NULL == who)
        || (strlen(url) >= (size_t)KODE_APPDATA_URL_MAX) )
    {
        return KODE_ERROR;
    }
    AppDataGetReqEvt *e = Q_NEW_X(AppDataGetReqEvt, 0U, APPDATA_GET_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_W(TAG, "out of events; the fetch of a data folder is dropped");
        return KODE_NO_RESOURCES;
    }
    (void)snprintf(e->project, sizeof(e->project), "%s", project);
    (void)snprintf(e->url, sizeof(e->url), "%s", url);
    e->who = who;
    if ( !QACTIVE_POST_X(AO_AppMgr, &e->super, 0U, NULL) )
    {
        KLOG_W(TAG, "AO_AppMgr's queue is full; the fetch of a data folder "
                    "is dropped");
        return KODE_NO_RESOURCES;
    }
    return KODE_OK;
}

void appmgr_cat_request(const char *project, const char *cats, bool clear)
{
    AppCatReqEvt *e = Q_NEW_X(AppCatReqEvt, 0U, APP_CAT_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; category request dropped");
        return;
    }
    (void)snprintf(e->project, sizeof(e->project), "%s", project);
    (void)snprintf(e->cats, sizeof(e->cats), "%s", clear ? "" : cats);
    e->clear = clear;
    QACTIVE_POST(AO_AppMgr, &e->super, NULL);
}

/**
 * @brief Refuse an app_cat write while AO_AppMgr is busy with a transfer.
 *
 * The console waits on app_cat (KCLI_APP_CAT), so the refusal is answered
 * rather than dropped. Callers log their own busy line first.
 */
static void app_cat_deny(AppMgr * const me)
{
    AppCatRspEvt *rsp = Q_NEW_X(AppCatRspEvt, 0U, APP_CAT_RSP_SIG);
    if ( NULL != rsp )
    {
        rsp->status = (uint8_t)KODE_DENIED;
        QACTIVE_PUBLISH(&rsp->super, &me->super);
    }
}

/**
 * @brief One transfer: where the bytes come from, where they go.
 *
 * The only way to ask AO_AppMgr to write an image. appmgr_flash_request() is
 * the shorthand for "this file, as an app, and boot it".
 */
QEvt const *appmgr_image_evt(uint8_t src_kind, const char *src,
                             uint8_t dst_kind, bool launch)
{
    FlashReqEvt *e   = Q_NEW_X(FlashReqEvt, 0U, FLASH_REQ_SIG);
    QEvt const  *evt = NULL;

    if ( NULL != e )
    {
        e->src_kind = src_kind;
        e->dst_kind = dst_kind;
        e->launch   = launch;
        e->src[0]   = '\0';
        if ( NULL != src )
        {
            (void)snprintf(e->src, sizeof(e->src), "%s", src);
        }
        evt         = &e->super;
    }
    return evt;
}

void appmgr_image_request(uint8_t src_kind, const char *src,
                          uint8_t dst_kind, bool launch)
{
    QEvt const *e = appmgr_image_evt(src_kind, src, dst_kind, launch);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; flash request dropped");
        return;
    }
    QACTIVE_POST(AO_AppMgr, e, NULL);
}

/* A reader that keeps KODE_APP_LIST_MAX rows of the installed list holds
 * every app the region can: nothing installed is ever out of its reach. */
_Static_assert(KODE_APP_LIST_MAX >= KAPPS_MAX_ENTRIES,
               "a list reader must hold the whole apps region");

void appmgr_list_request(QActive *who)
{
    appmgr_list_page(0U, who);
}

void appmgr_list_page(uint16_t skip, QActive *who)
{
    AppListReqEvt * const e = Q_NEW_X(AppListReqEvt, 0U, APP_LIST_REQ_SIG);

    if ( NULL == e )
    {
        /* An empty pool: the asker's own round window closes the question. */
        return;
    }
    e->skip = skip;
    e->who  = who;
    QACTIVE_POST(AO_AppMgr, &e->super, NULL);
}

bool appmgr_evict_query(uint32_t size, const char *project, QActive *who)
{
    if ( NULL == who )
    {
        return false;
    }
    AppEvictReqEvt *e = Q_NEW_X(AppEvictReqEvt, 0U, APP_EVICT_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_W(TAG, "out of events; eviction question dropped");
        return false;
    }
    e->size = size;
    e->who  = who;
    e->project[0] = '\0';
    if ( NULL != project )
    {
        /* A name longer than the array is cut by the bound, and stays ended. */
        (void)snprintf(e->project, sizeof(e->project), "%s", project);
    }
    if ( !QACTIVE_POST_X(AO_AppMgr, &e->super, KQP_APPMGR_ASK_MARGIN, NULL) )
    {
        KLOG_W(TAG, "AO_AppMgr's queue is full; eviction question dropped");
        return false;
    }
    return true;
}

/**
 * @brief Post the end of a page of the installed apps to its asker.
 *
 * Two static events, for the reason storage_ao.c's list_end() gives: the end
 * of a page is what its asker is waiting on.
 *
 * @param who  The asker.
 * @param more Another page follows this one.
 */
static void app_list_end(QActive *who, bool more)
{
    static const ListEndEvt end_more = {
        .super = QEVT_INITIALIZER(APP_LIST_END_SIG), .more = true };
    static const ListEndEvt end_last = {
        .super = QEVT_INITIALIZER(APP_LIST_END_SIG), .more = false };

    if ( more )
    {
        QACTIVE_POST(who, &end_more.super, AO_AppMgr);
    }
    else
    {
        QACTIVE_POST(who, &end_last.super, AO_AppMgr);
    }
}


/* ---- Keeping the victims of an eviction -----------------------------------
 * What the round records and how a plan's victims are told apart; the copies
 * themselves are AppMgr_keeping's.
 */

/** @brief Forget the evicted apps whose folders were still to go. */
static void victims_clear(AppMgr * const me)
{
    if ( NULL != me->victims )
    {
        khal_free(me->victims);
        me->victims = NULL;
    }
    me->victim_count = 0U;
    me->victim_next  = 0U;
}

/** @brief Forget the round: the install it belonged to has ended. */
static void keep_clear(AppMgr * const me)
{
    if ( NULL != me->keep_buf )
    {
        khal_free(me->keep_buf);
        me->keep_buf = NULL;
    }
    victims_clear(me);      /* an install that ends owes no folder to anyone */
    me->keep_round   = false;
    me->keep_pending = false;
    me->keep_forced  = false;
    me->keep_writing = false;
    me->keep_stat    = false;
    me->keep_count   = 0U;
    me->keep_idx     = 0U;
    me->keep_total   = 0U;
    me->keep_done    = 0U;
}

/** @return The victim's index in keep_seq[], or -1. */
static int keep_find(const AppMgr * const me, uint32_t seq)
{
    for ( uint32_t i = 0U; i < me->keep_count; ++i )
    {
        if ( me->keep_seq[i] == seq )
        {
            return (int)i;
        }
    }
    return -1;
}

/**
 * @brief The directory index of the plan's @p n-th victim: evict[] in order,
 *        then the slot victim. KAPPS_NO_VICTIM past the end.
 */
static uint8_t plan_victim(const kapps_plan_t *plan, uint32_t n)
{
    if ( n < plan->evict_count )
    {
        return plan->evict[n];
    }
    if ( n == plan->evict_count )
    {
        return plan->slot_victim;
    }
    return KAPPS_NO_VICTIM;
}

/**
 * @brief The name an app goes by when its image cannot be read: where it is.
 * @param out    Receives the name.
 * @param cap    Its size in bytes.
 * @param offset The extent's region-relative offset.
 */
static void unnamed_label(char *out, size_t cap, uint32_t offset)
{
    (void)snprintf(out, cap, "@0x%06lx", (unsigned long)offset);
}

/**
 * @brief Post one page of the installed apps to the asker that named itself.
 *
 * Served in AppMgr_idle and in AppMgr_tidying alike: the directory is
 * committed before a tidy starts, so it already says what the listing must.
 *
 * @param req The request: who asks, and how many entries to skip.
 */
static void app_list_page(AppListReqEvt const * const req)
{
    kapps_dir_t const *d = kdrv_apps_dir();
    if ( NULL == req->who )
    {
        KLOG_W(TAG, "a page of the app listing named nobody to send it to");
        return;
    }
    /* One page, posted to the asker alone: a page bigger than its queue would
     * meet QP's guaranteed delivery and reset the board. KODE_STORAGE_LIST_MAX
     * is sized against that queue. */
    uint32_t end = (uint32_t)req->skip + KODE_STORAGE_LIST_MAX;
    if ( end > d->count )
    {
        end = d->count;
    }
    uint32_t i = (uint32_t)req->skip;
    for ( ; i < end; ++i )
    {
        AppEntryEvt *ev = Q_NEW_X(AppEntryEvt, 0U, APP_ENTRY_SIG);
        if ( NULL == ev ) { KLOG_W(TAG, "listing cut short"); break; }
        kdrv_slot_desc_t desc;
        kapp_desc_t      man;
        if ( kdrv_apps_desc(&d->e[i], &desc, &man) == KODE_OK )
        {
            (void)snprintf(ev->name, sizeof(ev->name), "%s", desc.project);
            /* desc.version is 32 bytes, ev->version only 16: an
             * explicit precision (rather than plain "%s") tells the
             * compiler this truncation is bounded and intentional. */
            (void)snprintf(ev->version, sizeof(ev->version), "%.15s", desc.version);
            /* man is zeroed by kdrv_apps_desc when the image carries
             * no manifest, so this copies empty strings and the
             * reader decides what an empty category means. This AO
             * does not: it publishes the directory as it stands and
             * groups nothing. */
            (void)snprintf(ev->display, sizeof(ev->display), "%s", man.name);
            (void)snprintf(ev->category, sizeof(ev->category), "%s", man.category);
            (void)snprintf(ev->build_date, sizeof(ev->build_date), "%s",
                           desc.date);
            (void)snprintf(ev->build_time, sizeof(ev->build_time), "%s",
                           desc.time);
        }
        else
        {
            unnamed_label(ev->name, sizeof(ev->name), d->e[i].offset);
            ev->version[0] = '\0';
            ev->display[0] = '\0';
            ev->category[0] = '\0';
            ev->build_date[0] = '\0';
            ev->build_time[0] = '\0';
        }
        ev->size   = d->e[i].size;
        ev->seq    = d->e[i].seq;
        ev->offset = d->e[i].offset;
        ev->pos    = (uint16_t)i;
        QACTIVE_POST(req->who, &ev->super, AO_AppMgr);
    }
    if ( i < d->count )
    {
        KLOG_D(TAG, "page from %u: %u entries, more",
               (unsigned)req->skip, (unsigned)(i - req->skip));
    }
    app_list_end(req->who, i < d->count);
}

/**
 * @brief Decide whether this plan's victims must be copied first.
 *
 * Records every victim not yet in the round. Answers true when there is at
 * least one and the install can restart; the caller then abandons the
 * transfer before anything is committed. The app the install replaces is no
 * victim: its new version is the install.
 *
 * @return true to defer the install.
 */
static bool keep_defer(AppMgr * const me, const kapps_dir_t *d,
                       const kapps_plan_t *plan)
{
    uint32_t fresh = 0U;

    if ( me->keep_round || ((uint8_t)APPMGR_ORIGIN_CONSOLE == me->origin) )
    {
        return false;    /* one round per install; the console cannot pause */
    }
    for ( uint32_t n = 0U; n <= plan->evict_count; ++n )
    {
        uint8_t const idx = plan_victim(plan, n);
        if ( (KAPPS_NO_VICTIM != idx) && (plan->replaced != idx)
            && (keep_find(me, d->e[idx].seq) < 0)
            && (KAPPS_MAX_ENTRIES > me->keep_count) )
        {
            me->keep_seq[me->keep_count] = d->e[idx].seq;
            me->keep_res[me->keep_count] = (uint8_t)KEEP_UNTRIED;
            me->keep_total += d->e[idx].size;
            ++me->keep_count;
            ++fresh;
        }
    }
    return 0U < fresh;
}

/**
 * @brief Remember an evicted app, by name, so the install's tidy step deletes
 *        its folder.
 *
 * Silent about a name already held. A dry PSRAM costs the folder, never the
 * install: the app's folder stays as an orphan the launcher ignores.
 *
 * @param me      The AO.
 * @param project The evicted app's project name.
 */
static void victim_add(AppMgr * const me, const char *project)
{
    for ( uint32_t i = 0U; i < me->victim_count; ++i )
    {
        if ( strcmp(me->victims[i], project) == 0 )
        {
            return;
        }
    }
    if ( NULL == me->victims )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; a table from a void * allocator */
        me->victims = (char (*)[KDRV_SLOT_PROJECT_MAX])
            khal_realloc_psram(NULL, (size_t)KAPPS_MAX_ENTRIES
                                     * (size_t)KDRV_SLOT_PROJECT_MAX);
    }
    if ( (NULL == me->victims) || (KAPPS_MAX_ENTRIES <= me->victim_count) )
    {
        KLOG_W(TAG, "no room to note '%s'; its data folder stays", project);
        return;
    }
    (void)snprintf(me->victims[me->victim_count], KDRV_SLOT_PROJECT_MAX, "%s",
                   project);
    ++me->victim_count;
}

/**
 * @brief Say that one app is evicted, in the log and to every subscriber.
 *
 * Called before the entry is zeroed. A victim kept on the card keeps its data
 * folder; any other loses it in the install's tidy step.
 *
 * @param me  The AO, whose round says whether the victim was kept.
 * @param d   The working directory copy.
 * @param idx The victim's entry in @p d.
 */
static void evict_one(AppMgr * const me, const kapps_dir_t *d, uint8_t idx)
{
    int const k = keep_find(me, d->e[idx].seq);
    bool kept = false;
    kdrv_slot_desc_t vd;
    AppEvictedEvt *ev = Q_NEW_X(AppEvictedEvt, 0U, APP_EVICTED_SIG);

    if ( 0 <= k )
    {
        kept = ((uint8_t)KEEP_SAVED == me->keep_res[k])
            || ((uint8_t)KEEP_THERE == me->keep_res[k]);
    }
    if ( kdrv_apps_desc(&d->e[idx], &vd, NULL) != KODE_OK )
    {
        unnamed_label(vd.project, sizeof(vd.project), d->e[idx].offset);
    }
    else if ( !kept )
    {
        victim_add(me, vd.project);
    }
    else
    {
        /* kept on the card: its folder stays with its copy */
    }
    if ( kept )
    {
        KLOG_N(TAG, "evicting '%s' to make room; its copy is on the card",
               vd.project);
    }
    else
    {
        KLOG_W(TAG, "evicting '%s' to make room; no copy kept", vd.project);
    }
    if ( NULL == ev )
    {
        KLOG_W(TAG, "out of events; the eviction of '%s' goes unpublished",
               vd.project);
        return;
    }
    (void)snprintf(ev->name, sizeof(ev->name), "%s", vd.project);
    ev->kept = kept;
    QACTIVE_PUBLISH(&ev->super, &me->super);
}

/* ---- Directory helpers ----------------------------------------------------
 * Shared by the write path (install is upsert-by-name) and by run/erase
 * (both look an app up by the name it prints, not by directory index - the
 * index moves under appdir_compact).
 */

/** Drop the entries whose flags were zeroed (the evicted ones). */
static void appdir_compact(kapps_dir_t *d)
{
    uint32_t w = 0U;
    for ( uint32_t r = 0U; r < d->count; ++r )
    {
        if ( 0U != d->e[r].flags )
        {
            d->e[w++] = d->e[r];
        }
    }
    d->count = w;
}

/**
 * @brief Take a plan's victims out of the working directory copy.
 *
 * Every victim is said (evict_one()) before its entry is zeroed, except the
 * app the install replaces, whose new version is the install itself; then the
 * copy is compacted. Nothing is committed here: the caller persists the copy
 * before any flash the victims held is erased.
 *
 * @param me   The AO.
 * @param d    The working directory copy the plan was made on.
 * @param plan The plan.
 */
static void plan_evict(AppMgr * const me, kapps_dir_t *d,
                       const kapps_plan_t *plan)
{
    for ( uint32_t i = 0U; i < plan->evict_count; ++i )
    {
        uint8_t const idx = plan->evict[i];
        if ( plan->replaced == idx )
        {
            KLOG_I(TAG, "the old copy of the app being replaced makes room "
                        "for its new version");
        }
        else
        {
            evict_one(me, d, idx);
        }
        d->e[idx].flags = 0U;                     /* dead entry */
    }
    appdir_compact(d);
}

/**
 * @brief Free one directory slot by evicting the oldest app (FIFO by seq).
 *
 * The same FIFO kdrv_apps_plan() uses when the region is full. The evicted
 * image's bytes stay where they are, for a later plan to reclaim.
 *
 * @param me The AO, whose round says whether the victim was kept on the card.
 * @param d  The working directory copy to edit.
 * @return true when a slot was freed, false when there was nothing to evict.
 */
static bool appdir_make_room(AppMgr * const me, kapps_dir_t *d)
{
    uint32_t oldest = KAPPS_MAX_ENTRIES;
    for ( uint32_t i = 0U; i < d->count; ++i )
    {
        if ( (d->e[i].flags & KAPPS_ENTRY_VALID) != 0U )
        {
            if ( (KAPPS_MAX_ENTRIES == oldest) || (d->e[i].seq < d->e[oldest].seq) )
            {
                oldest = i;
            }
        }
    }
    if ( KAPPS_MAX_ENTRIES == oldest )
    {
        return false;
    }
    KLOG_W(TAG, "the directory is full; its oldest app goes");
    evict_one(me, d, (uint8_t)oldest);
    d->e[oldest].flags = 0U;
    appdir_compact(d);
    return true;
}

/** The next install sequence number: one past the highest in use. */
static uint32_t next_seq(const kapps_dir_t *d)
{
    uint32_t hi = 0U;
    for ( uint32_t i = 0U; i < d->count; ++i )
    {
        if ( d->e[i].seq > hi )
        {
            hi = d->e[i].seq;
        }
    }
    return hi + 1U;
}

/** Directory index of the VALID entry whose project name is `name`, or -1. */
static int appmgr_find_idx(const kapps_dir_t *d, const char *name)
{
    for ( uint32_t i = 0U; i < d->count; ++i )
    {
        if ( (d->e[i].flags & KAPPS_ENTRY_VALID) == 0U )
        {
            continue;
        }
        kdrv_slot_desc_t desc;
        if ( (kdrv_apps_desc(&d->e[i], &desc, NULL) == KODE_OK)
            && (strcmp(desc.project, name) == 0) )
        {
            return (int)i;
        }
    }
    return -1;
}

/**
 * @brief The entry of the app an install replaces, for kdrv_apps_plan().
 * @param d    The directory the plan is made on.
 * @param name The project name the image goes by; "" or NULL when unknown.
 * @return Its index, or KAPPS_NO_VICTIM when no installed app has that name.
 */
static uint8_t replacing_idx(const kapps_dir_t *d, const char *name)
{
    uint8_t idx = KAPPS_NO_VICTIM;

    if ( (NULL != name) && ('\0' != name[0]) )
    {
        int const i = appmgr_find_idx(d, name);
        if ( 0 <= i )
        {
            idx = (uint8_t)i;
        }
    }
    return idx;
}

/**
 * @brief The project name the mailbox's image goes by, read before its plan.
 *
 * A bare image says it in IDF's descriptor; a container only in its header's
 * name, which mkkfw.py leaves empty unless asked. Read into the chunk buffer,
 * which no transfer holds while an adoption is being planned.
 *
 * @param me  The AO; its chunk buffer is not NULL.
 * @param out Receives the name, or "" when the image does not say.
 * @param max Its size in bytes.
 */
static void pending_project(AppMgr * const me, char *out, size_t max)
{
    kfw_hdr_t hdr;

    out[0] = '\0';
    if ( kdrv_apps_pending_read(0U, me->chunk, KAPP_PREFIX_LEN) != KODE_OK )
    {
        return;
    }
    if ( kfw_hdr_parse(me->chunk, KAPP_PREFIX_LEN, &hdr) == KODE_OK )
    {
        (void)snprintf(out, max, "%.*s", (int)KFW_NAME_MAX, hdr.name);
    }
    else
    {
        (void)kapp_project_name(me->chunk, KAPP_PREFIX_LEN, out, max);
    }
}

/**
 * @brief Post the answer to a find request to its asker.
 *
 * @param who       The asker. Not NULL.
 * @param project   The name that was asked.
 * @param ent       The app's directory entry, or NULL when it is not installed.
 * @param on_card   Its copy is on the card; only meaningful when not installed.
 * @param busy      The question could not be looked at now.
 */
static void find_reply(QActive * const who, const char *project,
                       const kapps_entry_t *ent, bool on_card, bool busy)
{
    AppFindRspEvt * const ev = Q_NEW_X(AppFindRspEvt, 0U, APP_FIND_RSP_SIG);

    if ( NULL == ev )
    {
        KLOG_W(TAG, "out of events; the answer to a find request is lost");
        return;
    }
    (void)snprintf(ev->project, sizeof(ev->project), "%s", project);
    ev->installed = (NULL != ent);
    ev->offset    = 0U;
    ev->size      = 0U;
    if ( NULL != ent )
    {
        ev->offset = ent->offset;
        ev->size   = ent->size;
    }
    ev->on_card = on_card;
    ev->busy    = busy;
    if ( !QACTIVE_POST_X(who, &ev->super, 0U, AO_AppMgr) )
    {
        KLOG_W(TAG, "the asker's queue is full; the answer to a find request "
                    "is lost");
    }
}

/**
 * @brief Post the answer to a keep-only request to its asker.
 *
 * @param who    The asker. Not NULL.
 * @param st     KODE_OK, KODE_DENIED or KODE_ERROR.
 * @param erased How many apps were removed.
 */
static void keep_only_reply(QActive * const who, kode_status_t st,
                            uint8_t erased)
{
    AppKeepOnlyRspEvt * const ev = Q_NEW_X(AppKeepOnlyRspEvt, 0U,
                                           APP_KEEP_ONLY_RSP_SIG);

    if ( NULL == ev )
    {
        KLOG_W(TAG, "out of events; the answer to a keep-only request is lost");
        return;
    }
    ev->status = (uint8_t)st;
    ev->erased = erased;
    if ( !QACTIVE_POST_X(who, &ev->super, 0U, AO_AppMgr) )
    {
        KLOG_W(TAG, "the asker's queue is full; the answer to a keep-only "
                    "request is lost");
    }
}

/* ---- Copy machinery ------------------------------------------------------ */

/**
 * @brief Reset every flag that selects how the next transfer runs.
 *
 * Called on cleanup and at the top of every request that begins a transfer,
 * so a transfer that fails early cannot leave its bypass, destination, medium
 * or launch flag to the next one.
 *
 * @param me The active object.
 * @return Nothing.
 */
static void xfer_reset(AppMgr * const me)
{
    me->bypass     = false;
    me->dst_forced = false;
    me->from_net   = false;
    me->launch     = false;
}

/** Release everything an in-flight copy owns. Safe to call twice. */
static void flash_cleanup(AppMgr * const me)
{
    (void)QTimeEvt_disarm(&me->pacer);
    me->pacing    = false;
    me->to_return = 0U;
    if ( me->writing )
    {
        xfer_abort(me);
        me->writing = false;
    }
    me->os_target = false;   /* back to idle: next request picks its own target */
    me->sink      = NULL;
    /* The medium first: from_net says which one is open, and xfer_reset()
     * clears it. Closing storage's reader instead leaves the download open,
     * and the next https install is told a transfer is already open. */
    medium_close(me);
    xfer_reset(me);          /* clear bypass/dst_forced/from_net/launch too */
}

/**
 * @brief Unwind the transfer and say it failed, and why.
 * @param me     The AO.
 * @param why    The log's phrase.
 * @param reason What the glass is told (kfail_t).
 */
static void flash_fail(AppMgr * const me, const char *why, kfail_t reason)
{
    /* The second reason's event, static and immutable like its sibling above;
     * here because this is the one place that publishes it. */
    static const AppFlashFailEvt s_flash_keep_fail_evt = {
        QEVT_INITIALIZER(APP_FLASH_FAIL_SIG), (uint8_t)KFAIL_KEEP
    };
    KLOG_E(TAG, "install failed: %s", why);
    flash_cleanup(me);
    keep_clear(me);          /* the install ends here; so does its round */
    if ( KFAIL_KEEP == reason )
    {
        QACTIVE_PUBLISH(&s_flash_keep_fail_evt.super, &me->super);
    }
    else
    {
        QACTIVE_PUBLISH(&s_flash_fail_evt.super, &me->super);
    }
}

/**
 * @brief Publish the end of a write that installed @p name (APP_FLASH_DONE_SIG).
 *
 * A pool event, because it carries the name. On a dry pool it publishes
 * s_flash_done_evt, the same event with an empty name, so the end is not lost.
 *
 * @param me   The AO.
 * @param name The installed app's project name; "" when the written image's
 *             descriptor could not be read.
 */
static void flash_done_publish(AppMgr * const me, const char *name)
{
    keep_clear(me);          /* the install ends here; so does its round */
    AppFlashDoneEvt *ev = Q_NEW_X(AppFlashDoneEvt, 0U, APP_FLASH_DONE_SIG);
    if ( NULL != ev )
    {
        (void)snprintf(ev->name, sizeof(ev->name), "%s", name);
        QACTIVE_PUBLISH(&ev->super, &me->super);
    }
    else
    {
        KLOG_W(TAG, "out of events; '%s' is installed, but its done goes out "
                    "unnamed", name);
        QACTIVE_PUBLISH(&s_flash_done_evt.super, &me->super);
    }
}

/**
 * @brief Put a written, verified extent in the directory, replacing a namesake.
 *
 * A name already installed has its old entry zeroed in the same commit, so
 * reflashing one app never evicts the others. Shared by flash_finish() and
 * recv_close().
 *
 * @param len  The extent's length.
 * @param name Filled with the installed app's own name.
 * @param cap  sizeof(@p name).
 * @return NULL on success, with the directory committed and nothing yet
 *         announced (the install ends in tidy_begin()), else one phrase
 *         saying what went wrong.
 */
static const char *region_install(AppMgr * const me, uint32_t len,
                                  char *name, size_t cap)
{
    kapps_entry_t const probe = {
        .offset = me->dst_offset,
        .size   = len,
        .seq    = 0U,               /* placeholder; kdrv_apps_desc only reads .offset */
        .flags  = KAPPS_ENTRY_VALID,
    };
    kdrv_slot_desc_t desc;
    kode_status_t const dst = kdrv_apps_desc(&probe, &desc, NULL);

    kapps_dir_t dir = *kdrv_apps_dir();     /* work on a copy */
    if ( KODE_OK == dst )
    {
        int const old = appmgr_find_idx(&dir, desc.project);
        if ( 0 <= old )
        {
            dir.e[old].flags = 0U;          /* dead entry: superseded */
        }
    }
    /* Right after zeroing, before appdir_compact removes it: the superseded
     * entry's seq is still sitting in the array here (only its flags
     * changed), so it still counts toward the max and is not handed to the
     * new entry as a "free" reuse - same reasoning adopt_finish uses. */
    uint32_t const seq = next_seq(&dir);
    appdir_compact(&dir);
    if ( KAPPS_MAX_ENTRIES <= dir.count )
    {
        /* Every slot holds a live app of a different name (an upsert would
         * have freed one above). Evict the oldest so the install the user
         * asked for is not silently thrown away - the byte planner already
         * evicts by the same FIFO when the region runs out of space. */
        (void)appdir_make_room(me, &dir);
    }
    if ( KAPPS_MAX_ENTRIES <= dir.count )
    {
        /* Cannot happen after make_room freed a slot; kept as the guard that
         * dir.e[dir.count++] below never writes into .crc and beyond. */
        KLOG_E(TAG, "directory full (%u entries); not installing", (unsigned)dir.count);
        return "directory full";
    }
    kapps_entry_t const written = {
        .offset = me->dst_offset,
        .size   = len,
        .seq    = seq,
        .flags  = KAPPS_ENTRY_VALID,
    };
    dir.e[dir.count++] = written;

    if ( kdrv_apps_dir_commit(&dir) != KODE_OK )
    {
        return "cannot update the directory";
    }

    name[0] = '\0';
    if ( KODE_OK == dst )
    {
        (void)snprintf(name, cap, "%s", desc.project);
        KLOG_N(TAG, "installed '%s' v%s (%s %s) @0x%06lx",
               desc.project, desc.version, desc.date, desc.time,
               (unsigned long)me->dst_offset);
    }
    KLOG_N(TAG, "install complete (%u bytes)", (unsigned)len);
    return NULL;
}

/**
 * @brief Boot the app just installed under @p name.
 *
 * Resolves the extent from the directory, the same path APP_RUN_REQ_SIG takes.
 */
static void region_launch(const char *name)
{
    int const idx = ('\0' != name[0])
                    ? appmgr_find_idx(kdrv_apps_dir(), name) : -1;
    if ( 0 > idx )
    {
        KLOG_E(TAG, "installed but could not resolve its name; not booting");
        return;
    }
    kapps_dir_t const *d = kdrv_apps_dir();
    boot_app_run_request(d->e[idx].offset, d->e[idx].size, KBOOT_RUN_ONCE);
}

/* ---- The erase: everything of one app, and an end that says so --------------
 * The image and its directory entry go in the idle step that takes the
 * request, with the app's category override and a mailbox image of the same
 * project; the files and the folders are AO_Storage's, asked in the tidy step.
 * Every erase ends with one published AppEraseDoneEvt, a refusal included:
 * the console and the launcher both wait on it.
 */

/**
 * @brief Publish the end of an erase.
 *
 * @param name   The project the erase was about.
 * @param st     How it ended; see AppEraseDoneEvt.
 * @param image  An installed image was removed.
 * @param fs     What went on the internal partition; NULL when it was not
 *               looked at.
 * @param sd     What went on the card; NULL when it was not looked at.
 */
static void erase_publish(const char *name, kode_status_t st, bool image,
                          const AppWipeMedium *fs, const AppWipeMedium *sd)
{
    AppEraseDoneEvt *e = Q_NEW_X(AppEraseDoneEvt, 0U, APP_ERASE_DONE_SIG);
    /* A request carries a file's name, longer than a project's: what does
     * not fit is cut, and said to the format so the compiler knows. */
    size_t const room = sizeof(e->name) - 1U;

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; the end of the erase of '%s' goes unsaid",
               name);
        return;
    }
    (void)snprintf(e->name, sizeof(e->name), "%.*s", (int)room, name);
    e->status = (uint8_t)st;
    e->image  = image;
    (void)memset(&e->fs, 0, sizeof(e->fs));
    (void)memset(&e->sd, 0, sizeof(e->sd));
    if ( NULL != fs )
    {
        e->fs = *fs;
    }
    if ( NULL != sd )
    {
        e->sd = *sd;
    }
    QACTIVE_PUBLISH(&e->super, AO_AppMgr);
}

/**
 * @brief Answer an erase that arrives while the AO is busy: KODE_DENIED,
 *        published, so nobody waits on an erase that will not run.
 *
 * @param e  The request refused; any other signal is left alone.
 */
static void erase_deny(QEvt const * const e)
{
    if ( (QSignal)APP_ERASE_REQ_SIG == e->sig )
    {
        erase_publish(Q_EVT_CAST(AppFlashEvt)->file, KODE_DENIED, false,
                      NULL, NULL);
    }
}

/**
 * @brief Whether a medium's share of a wipe left something of the app behind.
 * @param m The medium's result.
 * @return true for an image or a folder that would not go.
 */
static bool erase_left(const AppWipeMedium *m)
{
    return (0U != m->left) || ((uint8_t)APP_WIPE_FOLDER_LEFT == m->folder);
}

/**
 * @brief Whether a medium's share of a wipe found anything of the app.
 * @param m The medium's result.
 * @return true for an image or a folder, gone or not.
 */
static bool erase_found(const AppWipeMedium *m)
{
    return (0U != m->files) || (0U != m->left)
        || ((uint8_t)APP_WIPE_FOLDER_NONE != m->folder);
}

/**
 * @brief Take AO_Storage's answer into what the erase's end will say.
 *
 * @param me  The AO, with an erase in hand.
 * @param rsp The wipe's answer.
 */
static void erase_take_wipe(AppMgr * const me, StorageAppWipeRspEvt const * const rsp)
{
    if ( (uint8_t)KODE_OK != rsp->status )
    {
        /* Refused: the files were not looked at. */
        me->erase_status = rsp->status;
        return;
    }
    me->erase_fs = rsp->fs;
    me->erase_sd = rsp->sd;
    if ( erase_left(&rsp->fs) || erase_left(&rsp->sd) )
    {
        me->erase_status = (uint8_t)KODE_IO_ERROR;
    }
    else if ( me->erase_image || me->erase_extra || erase_found(&rsp->fs)
             || erase_found(&rsp->sd) )
    {
        me->erase_status = (uint8_t)KODE_OK;
    }
    else
    {
        me->erase_status = (uint8_t)KODE_ERROR;   /* nothing of that name */
    }
}

/* ---- Tidying: the folders, then the end ------------------------------------
 * After region_install() commits, the app's metadata folder is made to match
 * the container, and the folders of evicted apps with no copy on the card are
 * deleted, one request at a time. The install ends (done, launch, `>ok`, boot)
 * only when AO_Storage has answered each one or its deadline ran out.
 */

/** @brief The project the request in flight is about, for the log. */
static const char *tidy_who(const AppMgr * const me)
{
    if ( ((uint8_t)TIDY_STEP_RM_VICTIM == me->tidy_step) && (0U < me->victim_next) )
    {
        return me->victims[me->victim_next - 1U];
    }
    return me->tidy_name;
}

/**
 * @brief Ask AO_Storage to make the installed app's own folder match the
 *        container: the put of its block, or the delete when it carried none.
 *
 * Nothing is asked of a bare image (its folder stays) or of a name that
 * cannot be a folder, which is one warning and no request.
 *
 * @param me The AO.
 * @return true when a request is out.
 */
static bool tidy_ask_own(AppMgr * const me)
{
    char path[KODE_APP_FILE_MAX];

    if ( KFW_AD_RAW == me->tidy_ad.kind )
    {
        return false;
    }
    if ( '\0' == me->tidy_name[0] )
    {
        KLOG_W(TAG, "the installed app's name could not be read; its data "
                    "folder is left as it is");
        return false;
    }
    if ( !appdata_folder_path(path, sizeof(path), APPDATA_MEDIUM_FS,
                             me->tidy_name) )
    {
        KLOG_W(TAG, "'%s' cannot be the name of a data folder; the folder is "
                    "left as it is", me->tidy_name);
        return false;
    }
    if ( (KFW_AD_BLOCK == me->tidy_ad.kind) && (NULL != me->tidy_ad.buf) )
    {
        if ( appdata_put_request(me->tidy_name, me->tidy_ad.buf,
                                me->tidy_ad.json_len, me->tidy_ad.icon_len,
                                &me->super) != KODE_OK )
        {
            KLOG_W(TAG, "the data folder of '%s' could not be asked for; the "
                        "rest goes on", me->tidy_name);
            return false;       /* not posted: the buffer is still ours */
        }
        me->tidy_ad.buf = NULL;  /* posted: AO_Storage owns it now */
        me->tidy_step = (uint8_t)TIDY_STEP_PUT;
    }
    else
    {
        if ( appdata_rm_request(me->tidy_name, &me->super) != KODE_OK )
        {
            KLOG_W(TAG, "the data folder of '%s' could not be asked for; the "
                        "rest goes on", me->tidy_name);
            return false;
        }
        me->tidy_step = (uint8_t)TIDY_STEP_RM_OWN;
    }
    return true;
}

/**
 * @brief Send the next folder request there is, under its deadline.
 *
 * The installed app's own first, then one delete per evicted app. A victim
 * with the installed app's own name is skipped: it is the app being replaced,
 * and its folder is the one just written. An erase has one request, the wipe
 * of the app's files.
 *
 * @param me The AO.
 * @return true when a request is out and its answer is awaited.
 */
static bool tidy_advance(AppMgr * const me)
{
    me->tidy_step = (uint8_t)TIDY_STEP_NONE;
    if ( !me->tidy_own_asked )
    {
        me->tidy_own_asked = true;
        if ( (uint8_t)TIDY_AFTER_ERASE == me->tidy_after )
        {
            /* An erase asks for more than the folder: every file of the app,
             * on both media, under a deadline of its own. */
            if ( storage_app_wipe_request(me->tidy_name, &me->super) == KODE_OK )
            {
                me->tidy_step = (uint8_t)TIDY_STEP_WIPE;
                (void)QTimeEvt_rearm(&me->tidy, APPMGR_WIPE_TICKS);
                return true;
            }
            me->erase_status = (uint8_t)KODE_NO_RESOURCES;
        }
        else if ( tidy_ask_own(me) )
        {
            (void)QTimeEvt_rearm(&me->tidy, APPMGR_TIDY_TICKS);
            return true;
        }
        else
        {
            /* Nothing to ask for the installed app's own folder. */
        }
    }
    while ( me->victim_next < me->victim_count )
    {
        const char * const victim = me->victims[me->victim_next];

        ++me->victim_next;
        if ( strcmp(victim, me->tidy_name) == 0 )
        {
            continue;
        }
        if ( appdata_rm_request(victim, &me->super) == KODE_OK )
        {
            me->tidy_step = (uint8_t)TIDY_STEP_RM_VICTIM;
            (void)QTimeEvt_rearm(&me->tidy, APPMGR_TIDY_TICKS);
            return true;
        }
        KLOG_W(TAG, "the data folder of '%s' could not be asked for; the "
                    "rest goes on", victim);
    }
    return false;
}

/**
 * @brief Every folder is settled: release what the tidy step held and end the
 *        install the way @c tidy_after says.
 *
 * Runs in whichever state the last answer found this AO in; a launch that
 * must wait for the console's answer to drain says so in @c tidy_launch and
 * the caller moves to AppMgr_launching.
 *
 * @param me The AO.
 */
static void tidy_finish(AppMgr * const me)
{
    (void)QTimeEvt_disarm(&me->tidy);
    me->tidy_on      = false;
    me->tidy_swallow = false;
    me->tidy_step    = (uint8_t)TIDY_STEP_NONE;
    kfw_appdata_release(&me->tidy_ad);

    switch ( me->tidy_after )
    {
        case TIDY_AFTER_FLASH: {
            flash_done_publish(me, me->tidy_name);
            if ( me->launch )
            {
                region_launch(me->tidy_name);
            }
            break;
        }
        case TIDY_AFTER_RECV: {
            flash_done_publish(me, me->tidy_name);
            QACTIVE_PUBLISH(&s_recv_done_evt, &me->super);
            if ( me->launch )
            {
                (void)snprintf(me->file, sizeof(me->file), "%s", me->tidy_name);
                me->tidy_launch = true;
            }
            break;
        }
        case TIDY_AFTER_ADOPT: {
            /* At the console, `install src="pending"` boots the app once. At
             * boot it is sticky: the app keeps running across the resets a
             * working app or its host make, until power-on, a hardware
             * watchdog or a brownout. The mailbox is already consumed. */
            if ( me->keep_forced )
            {
                if ( me->adopt_run )
                {
                    boot_app_run_request(me->dst_offset, me->total,
                                         KBOOT_RUN_ONCE);
                }
            }
            else
            {
                boot_app_run_request(me->dst_offset, me->total,
                                     KBOOT_RUN_STICKY);
            }
            keep_clear(me);        /* every way adoption ends ends its round */
            break;
        }
        case TIDY_AFTER_KEEP: {
            KLOG_N(TAG, "only '%s' is left; %u app(s) removed", me->tidy_name,  /* '' for none */
                   (unsigned)me->keep_only_erased);
            victims_clear(me);
            keep_only_reply(me->keep_only_who, KODE_OK, me->keep_only_erased);
            me->keep_only_who = NULL;
            break;
        }
        default: {
            /* TIDY_AFTER_ERASE: said in the log, then to whoever waits. */
            if ( (uint8_t)KODE_OK == me->erase_status )
            {
                KLOG_N(TAG, "'%s' removed", me->tidy_name);
            }
            else
            {
                KLOG_W(TAG, "the erase of '%s' ended with code %u", me->tidy_name,
                       (unsigned)me->erase_status);
            }
            erase_publish(me->tidy_name, (kode_status_t)me->erase_status,
                          me->erase_image, &me->erase_fs, &me->erase_sd);
            break;
        }
    }
}

/**
 * @brief Start the tidy step of an install that has just committed.
 *
 * Takes the block the decoder kept, and sends the first request or ends the
 * install at once when there is none. @c tidy_on then says whether
 * AppMgr_tidying owns the install; the caller transitions with tidy_state().
 *
 * @param me    The AO.
 * @param name  The project installed or erased; "" when it could not be read.
 * @param after TIDY_AFTER_*: how the install ends.
 */
static void tidy_begin(AppMgr * const me, const char *name, uint8_t after)
{
    me->tidy_after     = after;
    me->tidy_launch    = false;
    me->tidy_own_asked = false;
    me->tidy_swallow   = false;
    me->victim_next    = 0U;
    (void)snprintf(me->tidy_name, sizeof(me->tidy_name), "%.*s", (int)(sizeof(me->tidy_name) - 1U), name);
    if ( (uint8_t)TIDY_AFTER_ERASE == after )
    {
        me->tidy_ad.kind     = KFW_AD_NONE;
        me->tidy_ad.buf      = NULL;
        me->tidy_ad.json_len = 0U;
        me->tidy_ad.icon_len = 0U;
    }
    else if ( (uint8_t)TIDY_AFTER_KEEP == after )
    {
        /* The name is the app that stays: its own folder is left as it is,
         * which is what a bare image's block says. */
        me->tidy_ad.kind     = KFW_AD_RAW;
        me->tidy_ad.buf      = NULL;
        me->tidy_ad.json_len = 0U;
        me->tidy_ad.icon_len = 0U;
    }
    else
    {
        kfw_appdata_take(&me->tidy_ad);
    }
    if ( tidy_advance(me) )
    {
        me->tidy_on = true;
    }
    else
    {
        tidy_finish(me);
    }
}

/**
 * @brief Give back the block the decoder kept, for an install that ends
 *        between kfw_end() and tidy_begin().
 *
 * A block nobody takes stays in the decoder until the next kfw_begin() or
 * kfw_abort(), so an install that fails after kfw_end() answered KODE_OK
 * takes it and frees it on the spot.
 */
static void block_discard(void)
{
    kfw_appdata_t ad;

    kfw_appdata_take(&ad);
    kfw_appdata_release(&ad);
}

/** @brief Where the AO goes once tidy_begin() or an answer has been dealt with. */
static QState tidy_state(AppMgr * const me)
{
    if ( me->tidy_on )
    {
        return Q_TRAN(&AppMgr_tidying);
    }
    if ( me->tidy_launch )
    {
        me->tidy_launch = false;
        return Q_TRAN(&AppMgr_launching);
    }
    return Q_TRAN(&AppMgr_idle);
}

/**
 * @brief Whether @p e answers the request in flight: its signal, and the
 *        project it names.
 *
 * A put or a delete is answered once, and the answer names the project, so an
 * answer that matches neither the step nor the project is not this install's
 * and is not handled.
 *
 * @param me The AO.
 * @param e  APPDATA_PUT_RSP_SIG, APPDATA_RM_RSP_SIG or STORAGE_APP_WIPE_RSP_SIG.
 * @return true when it is the answer awaited.
 */
static bool tidy_awaited(const AppMgr * const me, QEvt const * const e)
{
    bool awaited = false;

    if ( (QSignal)APPDATA_PUT_RSP_SIG == e->sig )
    {
        awaited = ((uint8_t)TIDY_STEP_PUT == me->tidy_step)
               && (strcmp(Q_EVT_CAST(AppDataPutRspEvt)->project,
                          tidy_who(me)) == 0);
    }
    else if ( (QSignal)APPDATA_RM_RSP_SIG == e->sig )
    {
        awaited = (((uint8_t)TIDY_STEP_RM_OWN == me->tidy_step)
                   || ((uint8_t)TIDY_STEP_RM_VICTIM == me->tidy_step))
               && (strcmp(Q_EVT_CAST(AppDataRmRspEvt)->project,
                          tidy_who(me)) == 0);
    }
    else if ( (QSignal)STORAGE_APP_WIPE_RSP_SIG == e->sig )
    {
        awaited = ((uint8_t)TIDY_STEP_WIPE == me->tidy_step)
               && (strcmp(Q_EVT_CAST(StorageAppWipeRspEvt)->project,
                          tidy_who(me)) == 0);
    }
    else
    {
        /* not a folder answer */
    }
    return awaited;
}

/**
 * @brief An answer was taken: stop the deadline, and remember if it had
 *        already fired.
 *
 * A deadline that expired while the answer was queued behind it is in the
 * queue now; the flag makes the AO swallow exactly that one.
 *
 * @param me The AO.
 */
static void tidy_answered(AppMgr * const me)
{
    if ( !QTimeEvt_disarm(&me->tidy) )
    {
        me->tidy_swallow = true;
    }
}

/**
 * @brief Place the image now that its real length is known (kfw_hdr_cb_t).
 *
 * The decoder calls it before it opens the sink. A compressed container's file
 * size is not the image's, so the region cannot be planned any earlier.
 */
static kode_status_t appmgr_place(void *ctx, uint32_t img_len, const char *name)
{
    AppMgr * const me = (AppMgr *)ctx;

    if ( 0U == img_len )
    {
        KLOG_E(TAG, "the source could not say how long the image is");
        return KODE_ERROR;
    }
    /* The true total, so the progress percentages count the image rather
     * than the bytes that happened to carry it. */
    me->total = img_len;

    if ( me->os_target )
    {
        if ( kdrv_slot_size() < img_len )
        {
            KLOG_E(TAG, "%lu bytes will not fit the kodeOS slot (%lu bytes)",
                   (unsigned long)img_len, (unsigned long)kdrv_slot_size());
            return KODE_ERROR;
        }
        me->dst_offset = kdrv_slot_address();
        KLOG_I(TAG, "a %lu byte image for the kodeOS slot at 0x%06lx",
               (unsigned long)img_len, (unsigned long)me->dst_offset);
        return KODE_OK;   /* one image, one slot: nothing to place */
    }
    project_note(me, name);   /* the image's own word for what it is */

    kapps_plan_t plan;
    kapps_dir_t  dir = *kdrv_apps_dir();          /* work on a copy */
    if ( kdrv_apps_plan(&dir, kdrv_apps_region_size(), img_len,
                        replacing_idx(&dir, name), &plan) != KODE_OK )
    {
        KLOG_E(TAG, "%lu bytes will not fit the region even after evicting "
                    "everything", (unsigned long)img_len);
        return KODE_ERROR;
    }
    if ( keep_defer(me, &dir, &plan) )
    {
        me->keep_pending = true;
        KLOG_I(TAG, "%lu app(s) to keep on the card before this install",
               (unsigned long)me->keep_count);
        return KODE_ERROR;       /* the decoder stops before the sink opens */
    }
    /* Evictions, loudly and before any flash is touched; the copy is
     * persisted BEFORE anything the victims occupied is erased. */
    plan_evict(me, &dir, &plan);
    if ( (0U < plan.evict_count) && (kdrv_apps_dir_commit(&dir) != KODE_OK) )
    {
        KLOG_E(TAG, "cannot update the directory");
        return KODE_ERROR;
    }
    me->sink_ctx.offset = plan.offset;
    me->dst_offset      = plan.offset;
    KLOG_I(TAG, "placing a %lu byte image at 0x%06lx in the region",
           (unsigned long)img_len, (unsigned long)plan.offset);
    return KODE_OK;
}

/* ---- One transfer, with or without the decoder ---------------------------
 * Everything goes through the decoder except a park into the mailbox, which
 * must keep the container intact. Four wrappers rather than four conditionals
 * spread across the state machine.
 */

/* ---- The medium, whichever it is ------------------------------------------
 * A file and a URL are the same conversation; only the AO on the other end
 * differs. Three wrappers keep that fact in one place instead of at every
 * call site.
 */

/**
 * @brief Ask the medium in use, AO_Net's stream or AO_Storage's file, for a chunk.
 *
 * @param me The AO.
 */
static void medium_read(AppMgr * const me)
{
#if CONFIG_KODEOS_NET
    if ( me->from_net )
    {
        net_stream_read_request();
        return;
    }
#endif
    storage_read_request();
}

/**
 * @brief Close the medium in use: AO_Net's stream or AO_Storage's file.
 *
 * @param me The AO.
 */
static void medium_close(AppMgr * const me)
{
#if CONFIG_KODEOS_NET
    if ( me->from_net )
    {
        net_stream_close_request();
        return;
    }
#endif
    storage_close_request();
}

/**
 * @brief The bytes of lent buffer @p idx, from the medium in use.
 *
 * @param me  The AO.
 * @param idx The buffer's index, from the data event.
 * @return Its bytes; read them, do not keep them past medium_return().
 */
static const uint8_t *medium_buf(AppMgr * const me, uint8_t idx)
{
#if CONFIG_KODEOS_NET
    if ( me->from_net )
    {
        return net_stream_buf(idx);
    }
#endif
    return storage_buf(idx);
}

/**
 * @brief Hand lent buffer @p idx back to the medium in use, to be refilled.
 *
 * @param me  The AO.
 * @param idx The buffer's index, from the data event.
 */
static void medium_return(AppMgr * const me, uint8_t idx)
{
#if CONFIG_KODEOS_NET
    if ( me->from_net )
    {
        net_stream_return(idx);
        return;
    }
#endif
    storage_next_request(idx);
}

/**
 * @brief Open the transfer: the sink itself for a park, otherwise a decode session.
 *
 * @param me      The AO, whose sink and sink context are set.
 * @param want    The destination asked for, as a kfw_target_t; the decoder's only.
 * @param src_len Total bytes the source will deliver, or 0 when unknown.
 * @return KODE_OK, or the sink's or the decoder's error.
 */
static kode_status_t xfer_begin(AppMgr * const me, uint8_t want, uint32_t src_len)
{
    if ( me->bypass )
    {
        return me->sink->begin(&me->sink_ctx, src_len);
    }
    return kfw_begin(me->sink, &me->sink_ctx, (kfw_target_t)want, src_len,
                     appmgr_place, me);
}

/**
 * @brief Push one chunk into the transfer and count what it produced in written.
 *
 * @param me  The AO.
 * @param d   The chunk.
 * @param len Its length in bytes.
 * @return KODE_OK, or the sink's or the decoder's error.
 */
static kode_status_t xfer_feed(AppMgr * const me, const void *d, size_t len)
{
    if ( me->bypass )
    {
        kode_status_t const st = me->sink->write(&me->sink_ctx, d, len);
        if ( KODE_OK == st )
        {
            me->written += (uint32_t)len;   /* parked: produced == fed */
        }
        return st;
    }
    kode_status_t const st = kfw_feed(d, len);
    /* What the decoder produced, not what it was fed: a compressed container
     * produces more than it consumes, and every progress report and
     * end-of-stream check counts the image rather than what carried it. */
    me->written = kfw_produced();
    return st;
}

/**
 * @brief Close the transfer: the sink itself for a park, otherwise the decoder.
 *
 * @param me The AO.
 * @return KODE_OK, or the sink's or the decoder's error.
 */
static kode_status_t xfer_end(AppMgr * const me)
{
    return me->bypass ? me->sink->end(&me->sink_ctx) : kfw_end();
}

/**
 * @brief Abandon the transfer: the sink for a park, else only this AO's decode session.
 *
 * @param me The AO.
 */
static void xfer_abort(AppMgr * const me)
{
    if ( me->bypass )
    {
        if ( NULL != me->sink )
        {
            me->sink->abort(&me->sink_ctx);
        }
    }
    else
    {
        kfw_abort_owned(&me->sink_ctx);
    }
}

/**
 * @brief Close the write session, verify it, and install what it wrote.
 */
static void flash_finish(AppMgr * const me)
{
    kode_status_t const st = xfer_end(me);
    me->writing = false;
    medium_close(me);

    if ( KODE_OK != st )
    {
        flash_fail(me, "the written image did not verify", KFAIL_WRITE);
        return;
    }

    if ( me->os_target )
    {
        /* No directory: the slot holds one image, and the bootloader's
         * PENDING_VERIFY and confirm protect it. */
        kdrv_slot_desc_t desc;
        if ( kdrv_slot_desc(&desc) == KODE_OK )
        {
            KLOG_N(TAG, "kodeOS slot now holds '%s' v%s (%s %s, IDF %s)",
                   desc.project, desc.version, desc.date, desc.time,
                   desc.idf_ver);
        }
        KLOG_N(TAG, "install complete (%u bytes)", (unsigned)me->written);
        me->os_target = false;      /* back to idle before AO_Boot resets us */
        boot_os_switch_request();   /* AO_Boot selects otadata and resets */
        return;
    }

    if ( me->bypass )
    {
        /* Parked, not installed. The mailbox has no directory and what is in
         * it is a container, not an image - asking region_install() to
         * describe it produced an entry named after an offset that was never
         * allocated, which is how this branch came to exist. */
        KLOG_N(TAG, "parked %u bytes in the mailbox; `install src=\"pending\" "
                    "dst=...` installs it", (unsigned)me->written);
        me->bypass = false;
        /* The card that rose for the write ends on the done, which names no
         * app: none was installed. */
        QACTIVE_PUBLISH(&s_flash_done_evt.super, &me->super);
        return;
    }

    char name[32];   /* esp_app_desc_t.project_name is 32 bytes */
    const char *const why = region_install(me, (uint32_t)me->written,
                                           name, sizeof(name));
    if ( NULL != why )
    {
        block_discard();
        flash_fail(me, why, KFAIL_WRITE);
        return;
    }
    /* The done and the launch come after the folders (tidy_finish()). */
    tidy_begin(me, name, (uint8_t)TIDY_AFTER_FLASH);
}

/**
 * @brief Publish how far the write has got (APP_FLASH_PROGRESS_SIG).
 *
 * @param me  The AO.
 * @param pct The percentage of the image written, 0..100.
 */
static void progress_publish(AppMgr * const me, unsigned pct)
{
    if ( (uint8_t)APPMGR_ORIGIN_MAILBOX == me->origin )
    {
        /* Adoption has no Opening card and no end event to close one: it
         * stays silent, as it always was. */
        me->reported = pct;
        return;
    }
    AppProgressEvt *ev = Q_NEW_X(AppProgressEvt, 0U, APP_FLASH_PROGRESS_SIG);
    if ( NULL != ev )
    {
        /* What it is and how it ends: the slot always ends in AO_Boot's
         * switch, an app only when a launch was asked for. */
        ev->what = (uint8_t)KWRITE_APP;
        if ( me->os_target )
        {
            ev->what = (uint8_t)KWRITE_OS;
        }
        else if ( me->launch )
        {
            ev->what = (uint8_t)KWRITE_APP_RUN;
        }
        else
        {
            /* An app that stays installed, or a park in the mailbox. */
        }
        ev->percent = (uint8_t)pct;
        (void)snprintf(ev->file, sizeof(ev->file), "%s", me->file);
        ev->project[0] = '\0';
        if ( !me->os_target )
        {
            (void)snprintf(ev->project, sizeof(ev->project), "%s", me->project);
        }
        QACTIVE_PUBLISH(&ev->super, &me->super);
    }
    me->reported = pct;
}

/**
 * @brief Note which project is being written and, when that is news, say it.
 *
 * The reports name the project so the Opening sheet can show the app's own
 * data folder. A write already announced learns the name later than its 0 %
 * (a file's or a URL's first chunk carries it), and the same percent is then
 * reported again at once: a big image would otherwise keep the sheet
 * nameless for a whole percent of its bytes.
 *
 * @param me   The AO.
 * @param name The project name; NULL or the one already noted changes nothing.
 */
static void project_note(AppMgr * const me, const char *name)
{
    if ( (NULL != name) && (strcmp(me->project, name) != 0) )
    {
        (void)snprintf(me->project, sizeof(me->project), "%s", name);
        if ( me->writing && !me->os_target )
        {
            progress_publish(me, me->reported);
        }
    }
}

/** @brief Percent of the whole install: the copies, then the image. */
static unsigned progress_pct(const AppMgr * const me)
{
    uint64_t const whole = (uint64_t)me->keep_total + (uint64_t)me->total;
    uint64_t const done  = (uint64_t)me->keep_done + (uint64_t)me->written;
    unsigned pct = 0U;

    if ( 0U < whole )
    {
        pct = (unsigned)((done * 100U) / whole);
    }
    if ( 100U < pct )
    {
        pct = 100U;
    }
    return pct;
}

/**
 * @brief Report a write's progress after a chunk: on the bus at every whole
 *        percent, in the log every APPMGR_PROGRESS.
 *
 * A medium's chunks and the console's both come through here. The bus needs
 * the percent step: the Opening pill moves by it, and AO_Power drops its blue
 * pulse after POWER_LED_WRITE_MS without a report.
 *
 * @param me The AO.
 */
static void progress_report(AppMgr * const me)
{
    unsigned const pct = progress_pct(me);

    if ( pct > me->reported )
    {
        progress_publish(me, pct);
    }
    if ( pct >= me->next_report )
    {
        KLOG_N(TAG, "  %u%% (%u/%u bytes)", pct,
               (unsigned)me->written, (unsigned)me->total);
        me->next_report = pct + APPMGR_PROGRESS;
    }
}

/**
 * @brief Write one delivered chunk into the region.
 * @return true while more chunks are wanted.
 */
static bool consume_chunk(AppMgr * const me, const StorageDataEvt *d)
{
    if ( (uint8_t)KODE_OK != d->status )
    {
        flash_fail(me, "read from storage failed", KFAIL_WRITE);
        return false;
    }
    if ( d->eof )
    {
        if ( me->written < me->total )
        {
            flash_fail(me, "the file ended early", KFAIL_WRITE);
            return false;
        }
        flash_finish(me);
        return false;
    }

    const uint8_t *buf = medium_buf(me, d->idx);
    if ( NULL == buf )
    {
        flash_fail(me, "storage handed back a buffer that does not exist",
                   KFAIL_WRITE);
        return false;
    }

    /* The cheapest possible sanity gate, and it belongs on the first chunk:
     * every ESP-IDF image starts with 0xE9, and a container with "KFW1".
     * Deciding which - or that it is neither - is the decoder's job now, in
     * kfw_feed(): it is the only place that can tell the two formats apart,
     * and doing it here as well would be a second mechanism for one job. */

    kode_status_t const wst = xfer_feed(me, buf, d->len);
    if ( KODE_OK != wst )
    {
        if ( me->keep_pending )
        {
            return false;        /* deferred, not failed: the caller moves on */
        }
        flash_fail(me, me->os_target ? "write to the kodeOS slot failed"
                                      : "write to the region failed",
                   KFAIL_WRITE);
        return false;
    }

    progress_report(me);
    return true;
}

/* ---- The mailbox: app_pending -> the region -------------------------------
 * AppMgr_adopting's copy is fed by kdrv_apps_pending_read rather than by
 * AO_Storage - the mailbox is already a partition, so there is no medium to
 * open and no reason to make Storage a party to it.
 */

/**
 * @brief Close the adoption write session, verify it, and upsert the directory.
 *
 * An image that fails to verify consumes the mailbox: the same bytes fail the
 * same way at every boot. A failed directory write leaves the mailbox, so the
 * next boot tries again.
 */
static void adopt_finish(AppMgr * const me)
{
    kode_status_t const st = xfer_end(me);
    if ( KODE_OK != st )
    {          /* verify in place */
        /* idempotent: end() already closed the session it belongs to. */
        xfer_abort(me);
        KLOG_E(TAG, "adopted image does not verify; discarding the mailbox");
        (void)kdrv_apps_pending_consume();
        me->os_target = false;
        return;
    }

    if ( me->os_target )
    {
        /* No directory, no upsert: the slot holds exactly one image, and
         * the bootloader's own PENDING_VERIFY/confirm dance
         * (kdrv_slot_confirm_self(), AO_Boot's) is what protects
         * it - not this function's commit protocol. Mirrors flash_finish()'s
         * own os_target branch exactly; only the trigger differs (the
         * mailbox instead of the console's `os update`). */
        kdrv_slot_desc_t desc;
        if ( kdrv_slot_desc(&desc) == KODE_OK )
        {
            KLOG_N(TAG, "kodeOS slot now holds '%s' v%s (%s %s, IDF %s) - "
                        "adopted from the mailbox",
                   desc.project, desc.version, desc.date, desc.time,
                   desc.idf_ver);
        }
        /* Before the reset, not after: otherwise the kodeOS now running
         * from the new slot finds the same mailbox at its next boot and
         * tries to adopt it all over again. */
        (void)kdrv_apps_pending_consume();
        me->os_target = false;
        boot_os_switch_request();   /* AO_Boot selects otadata and resets */
        return;
    }

    /* upsert by name, ONE commit - same block flash_finish uses */
    kapps_entry_t const probe = {
        .offset = me->dst_offset,
        .size   = (uint32_t)me->total,
        .seq    = 0U,               /* placeholder; kdrv_apps_desc only reads .offset */
        .flags  = KAPPS_ENTRY_VALID,
    };
    kdrv_slot_desc_t desc;
    kode_status_t const dst = kdrv_apps_desc(&probe, &desc, NULL);

    kapps_dir_t dir = *kdrv_apps_dir();
    if ( KODE_OK == dst )
    {
        int const old = appmgr_find_idx(&dir, desc.project);
        if ( 0 <= old )
        {
            dir.e[old].flags = 0U;          /* dead entry: superseded */
        }
    }
    /* Right after zeroing, before appdir_compact removes it: see
     * flash_finish - same reasoning, same order, so the two read alike. */
    uint32_t const seq = next_seq(&dir);
    appdir_compact(&dir);
    if ( KAPPS_MAX_ENTRIES <= dir.count )
    {
        /* Every slot holds a live app of a different name: evict the oldest
         * rather than discard an upload the host already believes succeeded
         * (esptool printed "Hash of data verified"). Same FIFO as the byte
         * planner. */
        (void)appdir_make_room(me, &dir);
    }
    if ( KAPPS_MAX_ENTRIES <= dir.count )
    {
        /* Cannot happen after make_room; the guard against dir.e[dir.count++]
         * corrupting the record stays. */
        KLOG_E(TAG, "directory full (%u entries); not installing", (unsigned)dir.count);
        block_discard();
        (void)kdrv_apps_pending_consume();
        return;
    }
    kapps_entry_t const fresh = {
        .offset = me->dst_offset,
        .size   = (uint32_t)me->total,
        .seq    = seq,
        .flags  = KAPPS_ENTRY_VALID,
    };
    dir.e[dir.count++] = fresh;

    if ( kdrv_apps_dir_commit(&dir) != KODE_OK )
    {
        KLOG_E(TAG, "cannot commit the directory; mailbox left intact");
        block_discard();
        return;
    }
    (void)kdrv_apps_pending_consume();               /* only after the commit */
    KLOG_N(TAG, "dev app '%s' v%s adopted from the mailbox",
           desc.project, desc.version);

    /* The boot request - once for a console's `install src="pending"`,
     * sticky at boot - is tidy_finish()'s, after the app's folder. */
    const char *name = "";
    if ( KODE_OK == dst )
    {
        name = desc.project;
    }
    tidy_begin(me, name, (uint8_t)TIDY_AFTER_ADOPT);
}

/* ---- The AO -------------------------------------------------------------- */

void AppMgr_ctor(void)
{
    AppMgr * const me = &l_appMgr;
    QActive_ctor(&me->super, Q_STATE_CAST(&AppMgr_initial));
    QTimeEvt_ctorX(&me->pacer, &me->super, APP_FLASH_CHUNK_SIG, 0U);
    QTimeEvt_ctorX(&me->idle, &me->super, APP_RECV_IDLE_SIG, 0U);
    QTimeEvt_ctorX(&me->answer, &me->super, APP_RECV_LAUNCH_SIG, 0U);
    QTimeEvt_ctorX(&me->tidy, &me->super, APP_TIDY_TIMEOUT_SIG, 0U);
    me->writing   = false;
    me->pacing    = false;
    me->to_return = 0U;
    me->os_target = false;
    me->sink      = NULL;
    me->chunk     = NULL;
    me->keep_stale_stat  = 0U;
    me->keep_stale_write = 0U;
    me->keep_orphan      = NULL;
    me->ask_who          = NULL;
    me->ask_count        = 0U;
    me->ask_victims      = 0U;
    me->ask_idx          = 0U;
    me->ask_stat         = false;
    me->ask_mute         = false;
    me->find_who         = NULL;
    me->find_stat        = false;
    me->keep_only_who    = NULL;
    me->keep_only_erased = 0U;
    me->keep_only_next   = 0U;
    me->tidy_on          = false;
    me->tidy_launch      = false;
    me->tidy_step        = (uint8_t)TIDY_STEP_NONE;
    me->tidy_ad.kind     = KFW_AD_RAW;
    me->tidy_ad.buf      = NULL;
    me->tidy_ad.json_len = 0U;
    me->tidy_ad.icon_len = 0U;
    me->tidy_swallow     = false;
    me->victims          = NULL;     /* a used AO's table is the heap's now */
    keep_clear(me);   /* no round: a constructor over a used AO frees its buffer */
    me->origin    = (uint8_t)APPMGR_ORIGIN_FILE;
#if CONFIG_KODE_LOWPOWER
    me->lp_who    = KODE_LOWPOWER_WHO_NONE;
#endif
}

AO_STORAGE(appmgr, KQP_AO_QUEUE_LEN, KQP_AO_STACK_BYTES);   /* the install path needs the default stack */

void kaos_appmgr_register(void)
{
#if CONFIG_KODEOS_CONSOLE
    kaos_require(kverbs_add(&appmgr_verbs), "appmgr verbs");
#endif
}

void kaos_appmgr_start(void)
{
    AppMgr_ctor();
    kaos_appmgr_register();
    AO_START(AO_AppMgr, appmgr, KODE_PRIO_APPMGR);
}

/**
 * @brief Claim the region, the mailbox and the OTA slot, take the chunk buffer, go idle.
 *
 * An image waiting in the mailbox is not adopted here: APP_ADOPT_SIG is posted
 * to self, and idle takes it.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to AppMgr_idle.
 */
static QState AppMgr_initial(AppMgr * const me, void const * const par)
{
    (void)par;
    /* All three are flash: the apps region, the mailbox and the OTA slot. The
     * category table is not claimed: kapp_cat.c's own lock protects it. */
    kaos_claim(KBSP_RES_APPS_REGION, me, "AO_AppMgr");
    kaos_claim(KBSP_RES_APP_PENDING, me, "AO_AppMgr");
    kaos_claim(KBSP_RES_OTA_SLOT,    me, "AO_AppMgr");
#if CONFIG_KODE_LOWPOWER
    me->lp_who = lowpower_join(&me->super, "appmgr");
    if ( KODE_LOWPOWER_WHO_NONE != me->lp_who )
    {
        QActive_subscribe(&me->super, LOWPOWER_ROUND_SIG);
    }
#endif
    (void)kdrv_apps_init();
    /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() hands back an untyped block aligned for any type */
    me->chunk = (uint8_t *)khal_alloc_psram(sizeof(uint32_t), (size_t)KODE_RECV_CHUNK);
    if ( NULL == me->chunk )
    {
        KLOG_E(TAG, "no PSRAM for the chunk buffer; installs over the wire "
                    "and the mailbox are off");
    }

    /* The mailbox: if the host left an image at the fixed address, adopt it.
     * Posted to self rather than done here - a multi-MB flash copy has no
     * business inside an initial transition. */
    uint32_t img_len = 0U;
    if ( kdrv_apps_pending_present(&img_len, NULL) )
    {
        static const QEvt adopt_evt = QEVT_INITIALIZER(APP_ADOPT_SIG);
        QACTIVE_POST(&me->super, &adopt_evt, me);
    }

    KLOG_D(TAG, "FSM start: initial -> idle");
    return Q_TRAN(&AppMgr_idle);
}

/**
 * @brief Name a download by its URL's last path segment.
 *
 * The label is what the log and the progress events show. The query and the
 * fragment are left out, because that is where a token usually rides, and
 * the segment is cut to fit @p out.
 *
 * @param out  Where the label goes.
 * @param size Its size, terminator included.
 * @param url  The URL.
 */
static void url_label(char *out, size_t size, const char *url)
{
    size_t start = 0U;
    size_t end   = 0U;

    while ( ('\0' != url[end]) && ('?' != url[end]) && ('#' != url[end]) )
    {
        if ( '/' == url[end] )
        {
            start = end + 1U;
        }
        ++end;
    }
    size_t len = end - start;
    if ( len >= size )
    {
        len = size - 1U;
    }
    (void)memcpy(out, &url[start], len);
    out[len] = '\0';
}

/**
 * @brief Take the erase in hand: the image, the override, the mailbox, then
 *        AO_Storage for the files.
 *
 * An app that is not installed is not the end of it: a file or a folder of
 * that name may still be on a medium, and AO_Storage is asked all the same. A
 * directory that will not commit ends the erase with nothing removed, and so
 * does an empty name.
 *
 * @param me  The AO, idle.
 * @param req The request: the project's name.
 * @return The QP status: tidying while AO_Storage works, idle otherwise.
 */
static QState erase_begin(AppMgr * const me, AppFlashEvt const * const req)
{
    kapps_dir_t dir = *kdrv_apps_dir();
    int const   idx = appmgr_find_idx(&dir, req->file);  /* -1 if absent */
    char        cats[KODE_APP_CATEGORY_MAX];
    uint32_t    img_len     = 0U;
    uint32_t    decoded_len = 0U;

    me->erase_image  = false;
    me->erase_extra  = false;
    me->erase_status = (uint8_t)KODE_TIMEOUT;   /* until AO_Storage answers */
    (void)memset(&me->erase_fs, 0, sizeof(me->erase_fs));
    (void)memset(&me->erase_sd, 0, sizeof(me->erase_sd));

    if ( '\0' == req->file[0] )
    {
        /* No name is no app: nothing is looked for. */
        erase_publish(req->file, KODE_ERROR, false, NULL, NULL);
        return Q_HANDLED();
    }

    if ( 0 <= idx )
    {
        uint32_t const off = dir.e[idx].offset;

        dir.e[idx].flags = 0U;
        appdir_compact(&dir);
        if ( kdrv_apps_dir_commit(&dir) != KODE_OK )
        {
            KLOG_E(TAG, "cannot update the directory; nothing erased");
            erase_publish(req->file, KODE_IO_ERROR, false, NULL, NULL);
            return Q_HANDLED();
        }
        /* Erase the image header too, so no stale 0xE9 is left at a
         * bootable offset. The directory is what counts. */
        (void)kdrv_apps_erase_header(off);   /* kdrv: erase first 4 KB */
        me->erase_image = true;
    }
    else
    {
        KLOG_N(TAG, "'%s' is not installed; its files are looked for",
               req->file);
    }

    /* The person's categories for the app go with it: one NVS write, made
     * only when there is a row to drop. */
    if ( kapp_cat_resolve(req->file, cats, sizeof(cats)) )
    {
        (void)kapp_cat_clear(req->file);
        me->erase_extra = true;
    }
    /* A mailbox that still holds the app would bring it back at the next
     * boot. */
    if ( (NULL != me->chunk) && kdrv_apps_pending_present(&img_len, &decoded_len) )
    {
        char held[KDRV_SLOT_PROJECT_MAX];

        pending_project(me, held, sizeof(held));
        if ( strcmp(held, req->file) == 0 )
        {
            KLOG_N(TAG, "the mailbox held '%s'; emptied", req->file);
            (void)kdrv_apps_pending_consume();
            me->erase_extra = true;
        }
    }

    victims_clear(me);
    tidy_begin(me, req->file, (uint8_t)TIDY_AFTER_ERASE);
    return tidy_state(me);
}

/**
 * @brief Nothing in flight: installs, runs, erases, listings and the other requests.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState AppMgr_idle(AppMgr * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case APP_RECV_REQ_SIG: {
            AppRecvReqEvt const * const req = Q_EVT_CAST(AppRecvReqEvt);
            bool const to_os      = ((uint8_t)KFW_TARGET_OS == req->dst);
            bool const to_pending =
                ((uint8_t)KODE_FLASH_DST_PENDING == req->dst);

            if ( NULL == me->chunk )
            {
                KLOG_E(TAG, "install failed: no chunk buffer (see the boot)");
                QACTIVE_PUBLISH(&s_recv_fail_evt, &me->super);
                status = Q_HANDLED();
                break;
            }
            /* The sanity checks are the destination's: the region has to
             * exist and fit the image, and so does the slot. Refused here,
             * before the console is armed, so a transfer that cannot end
             * well never takes the wire. The mailbox's own size is its
             * sink's to check, when it opens. */
            if ( (!to_os) && (!to_pending)
                && ((uint8_t)KFW_TARGET_APP != req->dst) )
            {
                KLOG_E(TAG, "install failed: the coprocessor is AO_Radio's, "
                            "not this AO's");
                QACTIVE_PUBLISH(&s_recv_fail_evt, &me->super);
                status = Q_HANDLED();
                break;
            }
            if ( to_os )
            {
                if ( !kdrv_slot_present() )
                {
                    KLOG_E(TAG, "install dst=os failed: no kodeOS OTA slot "
                                "in the partition table");
                    QACTIVE_PUBLISH(&s_recv_fail_evt, &me->super);
                    status = Q_HANDLED();
                    break;
                }
                if ( (0U == req->size) || (kdrv_slot_size() < req->size) )
                {
                    KLOG_E(TAG, "install dst=os failed: %lu bytes will not "
                                "fit the slot", (unsigned long)req->size);
                    QACTIVE_PUBLISH(&s_recv_fail_evt, &me->super);
                    status = Q_HANDLED();
                    break;
                }
            }
            else if ( to_pending )
            {
                /* Nothing to check before the wire: see above. */
            }
            else
            {
                if ( !kdrv_apps_present() )
                {
                    KLOG_E(TAG, "install failed: no apps region in the "
                                "partition table");
                    QACTIVE_PUBLISH(&s_recv_fail_evt, &me->super);
                    status = Q_HANDLED();
                    break;
                }
                if ( (0U == req->size) || (kdrv_apps_region_size() < req->size) )
                {
                    KLOG_E(TAG, "install failed: %lu bytes will not fit the "
                                "region", (unsigned long)req->size);
                    QACTIVE_PUBLISH(&s_recv_fail_evt, &me->super);
                    status = Q_HANDLED();
                    break;
                }
            }

            xfer_reset(me);   /* a leaked bypass from a failed park must not
                               * make this install skip the decoder */
            me->origin = (uint8_t)APPMGR_ORIGIN_CONSOLE;
            keep_clear(me);
            me->os_target  = to_os;
            me->total      = req->size;
            me->recv_size  = req->size;
            me->written    = 0U;
            (void)snprintf(me->recv_name, sizeof(me->recv_name), "%s",
                           req->name);
            /* What the reports call it until the image says so itself. */
            (void)snprintf(me->project, sizeof(me->project), "%s", req->name);
            /* The ready publishes 0 % and the reports count on from there.
             * A stream has no file name, and one left over from the last
             * write would name a stranger on the card. */
            me->next_report = APPMGR_PROGRESS;
            me->reported    = 0U;
            me->file[0]     = '\0';
            /* The slot has no launch of its own: AO_Boot's switch is it.
             * The mailbox has none at all: what is parked there is still a
             * container, installed only by `install src="pending"`. */
            me->launch     = false;
            if ( (!to_os) && (!to_pending) )
            {
                me->launch = req->launch;
            }
            if ( to_pending )
            {
                me->bypass = true;
                me->sink   = kdrv_apps_pending_sink();
            }
            me->writing    = false;
            me->recv_ok    = true;
            me->recv_crc   = KHAL_CRC32_INIT;
            me->recv_want  = req->crc32;
            me->recv_fill  = 0U;
            me->recv_wire  = 0U;

            /* Arm the console first, before making room in the region: an
             * erase freezes the reader task, and an arming inside that freeze
             * cannot tell the command's newline from the payload's first byte.
             * AO_Log owns the console device; ask it. */
            ConsoleRawReqEvt *raw = Q_NEW_X(ConsoleRawReqEvt, 0U, CONSOLE_RAW_REQ_SIG);
            if ( NULL == raw )
            {
                KLOG_E(TAG, "install failed: out of events arming the console");
                QACTIVE_PUBLISH(&s_recv_fail_evt, &me->super);
                status = Q_HANDLED();
                break;
            }
            raw->size = req->size;
            raw->who  = &me->super;
            raw->src  = req->src;
            QACTIVE_POST(AO_Log, &raw->super, &me->super);
            status = Q_TRAN(&AppMgr_receiving);
            break;
        }
        case FLASH_REQ_SIG: {
            /* The general form the two verbs above are special cases of.
             * Both axes arrive in the event instead of being implied by the
             * signal, which is the whole point - but the conversation with
             * AO_Storage from here on is the same one, because it always
             * was: only the destination ever differed. */
            FlashReqEvt const * const req = Q_EVT_CAST(FlashReqEvt);

            xfer_reset(me);
            if ( !me->keep_round )
            {
                keep_clear(me);
                me->keep_req = *req;   /* what a restart would send again */
            }
            me->origin = (uint8_t)APPMGR_ORIGIN_FILE;
            if ( (uint8_t)KFW_SRC_HTTP == req->src_kind )
            {
                me->origin = (uint8_t)APPMGR_ORIGIN_NET;
            }

            /* src=pending is the boot-time adoption with the destination
             * typed out instead of read from the intent sector. Same copy,
             * same verification, same consume-on-success; only the boot
             * after it is the request's `run` (adopt_finish()). */
            if ( (uint8_t)KFW_SRC_PENDING == req->src_kind )
            {
                if ( (uint8_t)KFW_TARGET_C5 == req->dst_kind )
                {
                    KLOG_E(TAG, "dst=c5 belongs to AO_Radio; the console routes it there");
                    status = Q_HANDLED();
                    break;
                }
                me->os_target  = ((uint8_t)KFW_TARGET_OS == req->dst_kind);
                me->dst_forced = true;
                me->adopt_run  = req->launch;
                static const QEvt adopt = QEVT_INITIALIZER(APP_ADOPT_SIG);
                QACTIVE_POST(&me->super, &adopt, &me->super);
                status = Q_HANDLED();
                break;
            }

            me->from_net = ((uint8_t)KFW_SRC_HTTP == req->src_kind);
            if ( !me->from_net
                && ((uint8_t)KFW_SRC_FS != req->src_kind)
                && ((uint8_t)KFW_SRC_SD != req->src_kind) )
            {
                KLOG_E(TAG, "that source is not wired up yet");
                status = Q_HANDLED();
                break;
            }
#if !CONFIG_KODEOS_NET
            if ( me->from_net )
            {
                KLOG_E(TAG, "this firmware has no network services");
                status = Q_HANDLED();
                break;
            }
#endif

            /* dst=pending: park what arrives, container and all, without
             * decoding it. Reading it back is what checks it. */
            if ( (uint8_t)KODE_FLASH_DST_PENDING == req->dst_kind )
            {
                me->os_target = false;
                me->bypass    = true;
                me->launch    = false;
                me->sink      = kdrv_apps_pending_sink();
            }
            else if ( (uint8_t)KFW_TARGET_OS == req->dst_kind )
            {
                if ( !kdrv_slot_present() )
                {
                    flash_fail(me, "no kodeOS OTA slot in the partition table",
                               KFAIL_WRITE);
                    status = Q_HANDLED();
                    break;
                }
                me->os_target = true;
                me->launch    = false;  /* boot_os_switch_request() launches */
            }
            else if ( (uint8_t)KFW_TARGET_APP == req->dst_kind )
            {
                if ( !kdrv_apps_present() )
                {
                    flash_fail(me, "no apps region in the partition table",
                               KFAIL_WRITE);
                    status = Q_HANDLED();
                    break;
                }
                me->os_target = false;
                me->launch    = req->launch;
            }
            else
            {
                /* Only reachable from kode_sys_flash() or a stale caller: the
                 * console routes dst=c5 straight to AO_Radio. */
                KLOG_E(TAG, "the coprocessor is AO_Radio's, not this AO's");
                status = Q_HANDLED();
                break;
            }
            /* A file's name is not its project's, nor is a URL's: the
             * reports name none until the first chunk is read. */
            me->project[0] = '\0';
            /* A URL fits KODE_FLASH_SRC_MAX and travels whole; me->file
             * only names it. A file name has to fit me->file, which is the
             * shorter of the two. Refuse rather than truncate: a silently
             * shortened name would look for the wrong file and report it
             * missing. */
            if ( me->from_net )
            {
                url_label(me->file, sizeof(me->file), req->src);
            }
            else if ( strlen(req->src) >= sizeof(me->file) )
            {
                KLOG_E(TAG, "'%s' is longer than a file name may be (%u)",
                       req->src, (unsigned)(sizeof(me->file) - 1U));
                status = Q_HANDLED();
                break;
            }
            else
            {
                /* The precision is what tells the compiler the bound the
                 * check above already guarantees; without it
                 * -Wformat-truncation cannot see that these two buffers
                 * differ in size on purpose. */
                (void)snprintf(me->file, sizeof(me->file), "%.*s",
                               (int)(sizeof(me->file) - 1U), req->src);
            }
            /* Never true without CONFIG_KODEOS_NET (refused above), but the
             * guard stays: without it the call does not link. */
#if CONFIG_KODEOS_NET
            if ( me->from_net )
            {
                net_stream_open_request(req->src, &me->super);
            }
            else
            {
                storage_open_request(me->file, &me->super);
            }
#else
            storage_open_request(me->file, &me->super);
#endif
            status = Q_TRAN(&AppMgr_opening);
            break;
        }
        case STORAGE_PATH_RSP_SIG:
        case STORAGE_WRITE_RSP_SIG: {
            /* Only a late answer to a copy given up on reaches this state:
             * settle what it owes, or the next round would take it. */
            (void)keep_owed_answer(me, e);
            status = Q_HANDLED();
            break;
        }
        case APP_RUN_REQ_SIG: {
            AppFlashEvt const * const req = Q_EVT_CAST(AppFlashEvt);
            kapps_dir_t const *d = kdrv_apps_dir();
            int const idx = appmgr_find_idx(d, req->file);
            if ( 0 > idx )
            {
                KLOG_E(TAG, "'%s' is not installed - `app list` knows what is",
                       req->file);
            }
            else
            {
                boot_app_run_request(d->e[idx].offset, d->e[idx].size,
                                     KBOOT_RUN_ONCE);
            }                                          /* AO_Boot resets */
            status = Q_HANDLED();
            break;
        }
        case APP_ERASE_REQ_SIG: {
            status = erase_begin(me, Q_EVT_CAST(AppFlashEvt));
            break;
        }
        case APPDATA_GET_REQ_SIG: {
            status = get_begin(me, Q_EVT_CAST(AppDataGetReqEvt));
            break;
        }
        case APP_FIND_REQ_SIG: {
            status = find_ask(me, e);
            break;
        }
        case APP_KEEP_ONLY_REQ_SIG: {
            status = keep_only_ask(me, e);
            break;
        }
        case APP_CAT_REQ_SIG: {
            AppCatReqEvt const * const r = Q_EVT_CAST(AppCatReqEvt);
            /* One synchronous NVS write of 12 + 96 bytes per row, made here
             * because the table has one writer. Measured at a few ms for 1-3
             * rows; a full table and NVS page compaction are not measured. If
             * it ever stalls, pace it as a self-posted step. */
            kode_status_t const st = r->clear
                                     ? kapp_cat_clear(r->project)
                                     : kapp_cat_set(r->project, r->cats);
            AppCatRspEvt *rsp = Q_NEW_X(AppCatRspEvt, 0U, APP_CAT_RSP_SIG);
            if ( NULL != rsp )
            {
                rsp->status = (uint8_t)st;
                QACTIVE_PUBLISH(&rsp->super, &me->super);
            }
            else
            {
                KLOG_W(TAG, "out of events; app_cat answer dropped "
                            "(the console's watchdog will time it out)");
            }
            status = Q_HANDLED();
            break;
        }
        case APP_LIST_REQ_SIG: {
            app_list_page(Q_EVT_CAST(AppListReqEvt));
            status = Q_HANDLED();
            break;
        }
        case APP_EVICT_REQ_SIG: {
            status = evict_ask(me, e);
            break;
        }
        case APP_ADOPT_SIG: {
            if ( NULL == me->chunk )
            {
                KLOG_E(TAG, "no chunk buffer (see the boot); mailbox left intact");
                keep_clear(me);
                status = Q_HANDLED();
                break;
            }
            /* Ask again rather than trust the img_len AppMgr_initial saw -
             * the truth may have changed (nothing else writes the mailbox
             * between boot and here, but "trust the driver, not a stale
             * local" costs nothing). */
            uint32_t img_len     = 0U;
            uint32_t decoded_len = 0U;
            if ( !kdrv_apps_pending_present(&img_len, &decoded_len) )
            {
                /* Gone, or the driver just found it broken and consumed it -
                 * either way it already said why. */
                keep_clear(me);
                status = Q_HANDLED();
                break;
            }
            /* A round that is running was started by this adoption's first
             * pass, which asked for it again; any other round is somebody
             * else's and over. */
            if ( !me->keep_round )
            {
                keep_clear(me);
            }
            me->origin = (uint8_t)APPMGR_ORIGIN_MAILBOX;

            /* The host tool's own signal, not a guess from the image's
             * bytes: KAPPS_PENDING_INTENT_OFF, written separately, before
             * the image (see kapps_fmt.h and the design doc). Absent or
             * anything else there: adopt as an app, today's default. */
            if ( !me->dst_forced )
            {
                me->os_target = kdrv_apps_pending_os_intent();
            }
            me->keep_forced = me->dst_forced;   /* a restart asks the same again */
            me->dst_forced  = false;

            if ( me->os_target )
            {
                /* No plan, no evictions, no directory: the slot holds
                 * exactly one image, same as the slot's own path -
                 * mirrors AppMgr_opening's STORAGE_OPEN_RSP_SIG branch. */
                if ( !kdrv_slot_present() )
                {
                    KLOG_E(TAG, "mailbox flagged as a kodeOS OTA candidate "
                                "but no inactive slot exists in the "
                                "partition table; discarding");
                    (void)kdrv_apps_pending_consume();
                    me->os_target = false;
                    keep_clear(me);
                    status = Q_HANDLED();
                    break;
                }
                if ( kdrv_slot_size() < img_len )
                {
                    KLOG_E(TAG, "mailbox image (%lu bytes) will not fit the "
                                "kodeOS slot (%lu bytes); discarding",
                                (unsigned long)img_len,
                                (unsigned long)kdrv_slot_size());
                    (void)kdrv_apps_pending_consume();
                    me->os_target = false;
                    keep_clear(me);
                    status = Q_HANDLED();
                    break;
                }
                me->sink = kdrv_slot_sink();
                if ( xfer_begin(me, (uint8_t)KFW_TARGET_OS, img_len) != KODE_OK )
                {
                    KLOG_E(TAG, "cannot open the kodeOS slot for writing; "
                                "mailbox left intact");
                    me->os_target = false;
                    keep_clear(me);
                    status = Q_HANDLED();
                    break;
                }
            }
            else
            {
                /* Same plan/evict/compact/commit block STORAGE_OPEN_RSP_SIG
                 * runs in AppMgr_opening - the mailbox needs the identical
                 * room to be made for it. */
                kapps_plan_t plan;
                kapps_dir_t  dir = *kdrv_apps_dir();      /* work on a copy */
                /* Planned for the decoded image, the length the decoder's
                 * own appmgr_place() plans with on the first chunk: a
                 * deflated container holds fewer bytes than it places, and a
                 * plan made for the smaller number would leave victims for
                 * the decoder's plan to name after the round was over. The
                 * copy loop below still runs over what the mailbox holds. */
                char project[KDRV_SLOT_PROJECT_MAX];
                pending_project(me, project, sizeof(project));
                if ( kdrv_apps_plan(&dir, kdrv_apps_region_size(), decoded_len,
                                   replacing_idx(&dir, project), &plan)
                    != KODE_OK )
                {
                    KLOG_E(TAG, "mailbox image (%lu bytes) will not fit the "
                                "region even after evicting everything",
                                (unsigned long)decoded_len);
                    keep_clear(me);
                    status = Q_HANDLED();
                    break;
                }
                if ( 0U < plan.evict_count )
                {
                    /* Eviction is about to delete a live app, and a torn
                     * upload only fails the full verify, which runs after the
                     * copy. Verify the mailbox in place first. Without
                     * eviction the post-copy verify is enough. */
                    if ( kdrv_apps_pending_verify() != KODE_OK )
                    {
                        KLOG_E(TAG, "mailbox image does not verify; discarding "
                                    "it (no app evicted)");
                        (void)kdrv_apps_pending_consume();
                        keep_clear(me);
                        status = Q_HANDLED();
                        break;
                    }
                }
                /* The image is known good (or nothing is evicted): copy the
                 * victims to the card, then come back here from the top. The
                 * mailbox is a partition, so there is no medium to close. */
                if ( keep_defer(me, &dir, &plan) )
                {
                    KLOG_I(TAG, "%lu app(s) to keep on the card before "
                                "adopting the mailbox",
                           (unsigned long)me->keep_count);
                    keep_pause(me);
                    status = Q_TRAN(&AppMgr_keeping);
                    break;
                }
                plan_evict(me, &dir, &plan);
                if ( (0U < plan.evict_count)
                    && (kdrv_apps_dir_commit(&dir) != KODE_OK) )
                {
                    KLOG_E(TAG, "cannot update the directory; mailbox left intact");
                    keep_clear(me);
                    status = Q_HANDLED();
                    break;
                }
                me->sink            = kdrv_apps_sink();
                me->sink_ctx.offset = plan.offset;
                if ( xfer_begin(me, (uint8_t)KFW_TARGET_APP, img_len) != KODE_OK )
                {
                    KLOG_E(TAG, "cannot open the region for writing; mailbox left intact");
                    keep_clear(me);
                    status = Q_HANDLED();
                    break;
                }
            }
            me->total      = img_len;   /* replaced by appmgr_place() for a
                                         * container, which knows better */
            me->written    = 0U;
            me->src_len    = img_len;
            me->consumed   = 0U;
            /* rearm, not armX: a heartbeat queued behind the last copy's
             * answer can reach the adopting state after this armed the
             * pacer, and armX on an armed timer asserts. */
            (void)QTimeEvt_rearm(&me->pacer, APPMGR_CHUNK_GAP_TICKS);
            status = Q_TRAN(&AppMgr_adopting);
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            /* Idle: no image is being written, kept, received or launched. */
            lowpower_ready(me->lp_who, Q_EVT_CAST(LowPowerRoundEvt)->round,
                           false, NULL);
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

/** Waiting for AO_Storage to say whether the file is there. */
static QState AppMgr_opening(AppMgr * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            QTimeEvt_armX(&me->idle, APPMGR_OPEN_TICKS, 0U);
            status = Q_HANDLED();
            break;
        }
        case APP_CANCEL_REQ_SIG: {
            flash_fail(me, "the person went back", KFAIL_WRITE);
            status = Q_TRAN(&AppMgr_idle);
            break;
        }
        case Q_EXIT_SIG: {
            QTimeEvt_disarm(&me->idle);
            status = Q_HANDLED();
            break;
        }
        case APP_RECV_IDLE_SIG: {
            /* Shares the deadline timer with AppMgr_receiving, AppMgr_flashing
             * and the keep round: the states are mutually exclusive and each
             * disarms it on the way out. */
            KLOG_E(TAG, "'%s' was never answered; giving up on it", me->file);
            flash_fail(me, "the medium did not answer", KFAIL_WRITE);
            status = Q_TRAN(&AppMgr_idle);
            break;
        }
        case STORAGE_OPEN_RSP_SIG:
        case NET_STREAM_OPEN_RSP_SIG: {
            StorageOpenRspEvt const * const rsp = Q_EVT_CAST(StorageOpenRspEvt);
            if ( (uint8_t)KODE_OK != rsp->status )
            {
                if ( (uint8_t)KODE_ERROR == rsp->status )
                {
                    KLOG_E(TAG, me->from_net
                                ? "'%s' did not come back from the server"
                                : "'%s' is not on the SD card nor in the "
                                  "internal storage", me->file);
                    flash_fail(me, me->from_net ? "the server said no"
                                                : "file not found",
                               KFAIL_WRITE);
                }
                else
                {
                    /* Found but unusable - AO_Storage already logged what was
                     * wrong with it, and repeating "not found" would
                     * contradict it. */
                    flash_fail(me, "unusable file", KFAIL_WRITE);
                }
                status = Q_TRAN(&AppMgr_idle);
                break;
            }
            /* Placement, eviction and the sink all happen inside the
             * decoder's header callback now: only there is the image's real
             * length known, and for a compressed container it is not the
             * file's. */
            /* Not in bypass: there the destination was chosen when the
             * request arrived and must not be second-guessed here. */
            if ( !me->bypass )
            {
                me->sink = me->os_target ? kdrv_slot_sink() : kdrv_apps_sink();
            }
            if ( (!me->from_net) && ((uint8_t)KDRV_MEDIUM_SD == rsp->medium) )
            {
                me->origin = (uint8_t)APPMGR_ORIGIN_CARD;
            }
            if ( xfer_begin(me, me->os_target ? (uint8_t)KFW_TARGET_OS
                                             : (uint8_t)KFW_TARGET_APP,
                           rsp->size) != KODE_OK )
            {
                flash_fail(me, "cannot start a transfer", KFAIL_WRITE);
                status = Q_TRAN(&AppMgr_idle);
                break;
            }

            me->writing     = true;
            me->pacing      = false;
            me->to_return   = 0U;

            /* Announce the start, not just the first percent. Subscribers use
             * this to know a write is happening at all - it is what raises
             * the Opening sheet and the LED's pulse - and the medium has not
             * delivered a byte yet. The console's road announces at its
             * ready (AppMgr_receiving). */
            me->total       = (size_t)rsp->size;
            me->written     = 0U;
            me->next_report = APPMGR_PROGRESS;
            progress_publish(me, progress_pct(me));   /* a restart resumes where
                                                       * the copies left the bar */
            /* The file's size and not the image's, and no offset: with a
             * container neither is known until the header is parsed, and
             * appmgr_place() logs the real ones the moment they are. Naming
             * a number here that the next line might contradict is worse
             * than naming none. */
            KLOG_N(TAG, "writing %s from %s (%u bytes on the medium) into %s",
                   me->file,
                   me->from_net ? "the network"
                                : kdrv_storage_mount_point((kdrv_medium_t)rsp->medium),
                   (unsigned)rsp->size,
                   me->os_target ? "the kodeOS slot" : "the apps region");

            /* Prime both buffers: from here on one is being written to flash
             * while the other is being filled from the medium. */
            medium_read(me);
            medium_read(me);
            status = Q_TRAN(&AppMgr_flashing);
            break;
        }
        /* A request arriving now would interleave with the one in flight. */
        case FLASH_REQ_SIG:
        case APP_RUN_REQ_SIG:
        case APP_ERASE_REQ_SIG:
        case APP_LIST_REQ_SIG: {
            KLOG_W(TAG, "busy opening '%s'; request ignored", me->file);
            erase_deny(e);
            status = Q_HANDLED();
            break;
        }
        case APP_CAT_REQ_SIG: {
            KLOG_W(TAG, "busy opening '%s'; app_cat request denied", me->file);
            app_cat_deny(me);
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&AppMgr_busy);
            break;
        }
    }
    return status;
}

/* ---- Keeping the victims on the card ---------------------------------------
 * The install is stopped at its plan, the extents it would evict are copied
 * to the card, and it starts again from the top: why, and for which sources,
 * is the block comment on AppMgr's keep fields.
 */

/** @brief Abandon the transfer in hand, before anything was committed. */
static void keep_pause(AppMgr * const me)
{
    (void)QTimeEvt_disarm(&me->pacer);
    me->pacing    = false;
    me->to_return = 0U;
    if ( me->writing )
    {
        xfer_abort(me);
        me->writing = false;
    }
    me->written = 0U;
    if ( (uint8_t)APPMGR_ORIGIN_MAILBOX != me->origin )
    {
        medium_close(me);        /* from_net still says which medium */
    }
    me->keep_pending = false;
    me->keep_idx     = 0U;
}

/** @brief Start the same install again from the top; the round is done. */
static void keep_restart(AppMgr * const me)
{
    me->keep_round = true;
    if ( (uint8_t)APPMGR_ORIGIN_MAILBOX == me->origin )
    {
        static const QEvt adopt = QEVT_INITIALIZER(APP_ADOPT_SIG);
        me->dst_forced = me->keep_forced;
        me->os_target  = false;
        QACTIVE_POST_LIFO(&me->super, &adopt);
        return;
    }
    FlashReqEvt * const r = Q_NEW_X(FlashReqEvt, 0U, FLASH_REQ_SIG);
    if ( NULL == r )
    {
        flash_fail(me, "out of events to restart the install", KFAIL_WRITE);
        return;
    }
    r->src_kind = me->keep_req.src_kind;
    r->dst_kind = me->keep_req.dst_kind;
    r->launch   = me->keep_req.launch;
    (void)snprintf(r->src, sizeof(r->src), "%s", me->keep_req.src);
    /* LIFO: a request already queued must not run between the copies and
     * the install they were made for. */
    QACTIVE_POST_LIFO(&me->super, &r->super);
}

/** @brief The victim's directory entry, found again by seq; NULL if gone. */
static const kapps_entry_t *keep_entry(uint32_t seq)
{
    kapps_dir_t const * const d = kdrv_apps_dir();

    for ( uint32_t i = 0U; i < d->count; ++i )
    {
        if ( ((d->e[i].flags & KAPPS_ENTRY_VALID) != 0U) && (d->e[i].seq == seq) )
        {
            return &d->e[i];
        }
    }
    return NULL;
}

/**
 * @brief Settle an answer owed to a STAT or a write given up on, if @p e is
 *        one; the caller has checked it is a path or write answer.
 * @return true when @p e was such a late answer and is now spent.
 */
static bool keep_owed_answer(AppMgr * const me, QEvt const * const e)
{
    bool spent = false;

    if ( ((QSignal)STORAGE_PATH_RSP_SIG == e->sig)
        && ((uint8_t)STORAGE_PATH_STAT == Q_EVT_CAST(StoragePathRspEvt)->op)
        && (0U < me->keep_stale_stat) )
    {
        --me->keep_stale_stat;
        spent = true;
    }
    else if ( ((QSignal)STORAGE_WRITE_RSP_SIG == e->sig)
             && (0U < me->keep_stale_write) )
    {
        /* AO_Storage is done with the abandoned buffer now. */
        --me->keep_stale_write;
        khal_free(me->keep_orphan);
        me->keep_orphan = NULL;
        spent = true;
    }
    else
    {
        /* not a late answer */
    }
    if ( spent )
    {
        KLOG_D(TAG, "a late answer to an abandoned copy");
    }
    return spent;
}

/** @brief Record @p res for the victim in hand, count its bytes, publish. */
static void keep_settle(AppMgr * const me, keep_res_t res)
{
    me->keep_res[me->keep_idx] = (uint8_t)res;
    me->keep_done += me->keep_len;
    if ( NULL != me->keep_buf )
    {
        khal_free(me->keep_buf);
        me->keep_buf = NULL;
    }
    me->keep_writing = false;
    me->keep_stat    = false;
    (void)QTimeEvt_disarm(&me->idle);    /* the victim's answer deadline */
    ++me->keep_idx;
    if ( progress_pct(me) > me->reported )
    {
        progress_publish(me, progress_pct(me));
    }
}

/**
 * @brief Write the path of @p project's copy on the card into @p out.
 * @param out     Receives `/sdcard/<project>.bin`.
 * @param cap     Its size in bytes.
 * @param project The app's project name.
 */
static void card_copy_path(char *out, size_t cap, const char *project)
{
    (void)snprintf(out, cap, KODE_CARD_APP_PREFIX "%s" KODE_CARD_APP_SUFFIX,
                   project);
}

/**
 * @brief Ask AO_Storage whether @p path is on the card, and start the clock
 *        on its answer.
 *
 * The answer is STORAGE_PATH_RSP_SIG, in order and without the path. The
 * caller owns the flag that says a STAT is out; the deadline runs on the
 * timer both AppMgr_keeping and AppMgr_asking use for it.
 *
 * @param me   The AO.
 * @param path Absolute and virtual: what card_copy_path() wrote.
 */
static void card_stat(AppMgr * const me, const char *path)
{
    storage_path_request(&me->super, (uint8_t)STORAGE_PATH_STAT, path);
    (void)QTimeEvt_rearm(&me->idle, APPMGR_OPEN_TICKS);
}

/**
 * @brief Does a STAT's answer say a copy is on the card?
 * @param r The answer.
 * @return true for a file that is there; false for no card, a failed STAT,
 *         nothing at the path, or a folder where the file would be.
 */
static bool card_has_copy(const StoragePathRspEvt *r)
{
    return ((uint8_t)KODE_OK == r->rc) && r->exists && !r->is_dir;
}

/**
 * @brief Start on the next untried victim.
 * @return true when one was started (a STAT is out), false when none is left.
 */
static bool keep_next(AppMgr * const me)
{
    while ( me->keep_idx < me->keep_count )
    {
        kapps_entry_t const * const ent = keep_entry(me->keep_seq[me->keep_idx]);
        kdrv_slot_desc_t vd;

        me->keep_len = 0U;
        if ( NULL != ent )
        {
            me->keep_len = ent->size;
            me->keep_off = ent->offset;
        }
        if ( (NULL == ent) || (kdrv_apps_desc(ent, &vd, NULL) != KODE_OK) )
        {
            KLOG_W(TAG, "an app with no readable name cannot be kept on the card");
            keep_settle(me, KEEP_FAILED);
        }
        else
        {
            (void)snprintf(me->keep_name, sizeof(me->keep_name), "%s",
                           vd.project);
            card_copy_path(me->keep_path, sizeof(me->keep_path), vd.project);
            card_stat(me, me->keep_path);
            me->keep_stat = true;
            return true;
        }
    }
    return false;
}

/** @brief Every victim tried: cancel (rule 5) or restart. */
static QState keep_finish(AppMgr * const me)
{
    bool failed = false;

    if ( me->keep_cancel )
    {
        me->keep_cancel = false;
        flash_fail(me, "the person went back", KFAIL_WRITE);
        return Q_TRAN(&AppMgr_idle);
    }
    for ( uint32_t i = 0U; i < me->keep_count; ++i )
    {
        if ( (uint8_t)KEEP_FAILED == me->keep_res[i] )
        {
            failed = true;
        }
    }
    if ( failed && ((uint8_t)APPMGR_ORIGIN_CARD == me->origin) )
    {
        flash_fail(me, "the card could not keep an app this install would "
                       "remove; nothing evicted", KFAIL_KEEP);
        return Q_TRAN(&AppMgr_idle);
    }
    keep_restart(me);     /* a failed restart calls flash_fail() itself */
    return Q_TRAN(&AppMgr_idle);
}

/**
 * @brief Copy each victim to the card, one at a time, then restart.
 *
 * Per victim: STAT its path, and when it is not there, read its extent in
 * APPMGR_KEEP_CHUNK steps and hand them to AO_Storage. A failure is
 * KEEP_FAILED, and the round goes on.
 */
static QState AppMgr_keeping(AppMgr * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            if ( !keep_next(me) )
            {
                /* Nothing reachable to copy: decide at once. A transition
                 * cannot be taken from an entry action, so post to self. */
                static const QEvt done = QEVT_INITIALIZER(APP_FLASH_CHUNK_SIG);
                QACTIVE_POST(&me->super, &done, &me->super);
            }
            status = Q_HANDLED();
            break;
        }
        case APP_CANCEL_REQ_SIG: {
            /* The copy in hand ends first, and the app stays safe on the card. */
            me->keep_cancel = true;
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            (void)QTimeEvt_disarm(&me->pacer);
            (void)QTimeEvt_disarm(&me->idle);
            status = Q_HANDLED();
            break;
        }
        case APP_RECV_IDLE_SIG: {
            /* AO_Storage dropped a request on a dry pool, or the card never
             * answered: the victim is a failed copy and the round goes on. */
            status = Q_HANDLED();
            if ( me->keep_writing )
            {
                KLOG_E(TAG, "the card never answered for '%s'; its buffer is "
                            "abandoned", me->keep_name);
                (void)QTimeEvt_disarm(&me->pacer);   /* the heartbeat too */
                /* Not freed: AO_Storage may still hold it, and the write's
                 * data must stay valid until its answer. The oldest one is
                 * freed when that answer comes; a second one at once is
                 * left to the heap. */
                if ( NULL == me->keep_orphan )
                {
                    me->keep_orphan = me->keep_buf;
                }
                me->keep_buf = NULL;
                if ( UINT8_MAX > me->keep_stale_write )
                {
                    ++me->keep_stale_write;
                }
            }
            else if ( me->keep_stat )
            {
                KLOG_W(TAG, "nothing answered about '%s' on the card",
                       me->keep_name);
                if ( UINT8_MAX > me->keep_stale_stat )
                {
                    ++me->keep_stale_stat;
                }
            }
            else
            {
                break;                               /* a stale expiry */
            }
            keep_settle(me, KEEP_FAILED);
            if ( !keep_next(me) )
            {
                status = keep_finish(me);
            }
            break;
        }
        case STORAGE_PATH_RSP_SIG: {
            StoragePathRspEvt const * const r = Q_EVT_CAST(StoragePathRspEvt);
            status = Q_HANDLED();
            if ( (uint8_t)STORAGE_PATH_STAT != r->op )
            {
                /* The answer to the unlink a failed copy asked for: it
                 * settles nothing, least of all the next victim's STAT. */
                KLOG_D(TAG, "a path answer that is not a stat");
                break;
            }
            if ( keep_owed_answer(me, e) )
            {
                break;   /* a STAT given up on, not this victim's */
            }
            if ( !me->keep_stat )
            {
                KLOG_D(TAG, "an answer about a path nobody is waiting for");
                break;
            }
            me->keep_stat = false;
            (void)QTimeEvt_disarm(&me->idle);
            if ( card_has_copy(r) )
            {
                KLOG_I(TAG, "'%s' is already on the card", me->keep_name);
                keep_settle(me, KEEP_THERE);
            }
            else if ( (uint8_t)KODE_OK != r->rc )
            {
                keep_settle(me, KEEP_FAILED);   /* no card, or it will not mount */
            }
            else if ( r->exists )
            {
                /* A folder of that name is no copy, and a file cannot be
                 * written over it. */
                KLOG_W(TAG, "'%s' on the card is a folder; the app cannot be "
                            "kept there", me->keep_path);
                keep_settle(me, KEEP_FAILED);
            }
            else
            {
                /* Not zeroed: every byte is read from the region before it
                 * is written, and clearing up to 13 MB is no step (rule 5). */
                /* cppcheck-suppress misra-c2012-11.5 ; a byte buffer from a void * allocator */
                me->keep_buf = (uint8_t *)khal_realloc_psram(NULL, me->keep_len);
                if ( NULL == me->keep_buf )
                {
                    keep_settle(me, KEEP_FAILED);
                }
                else
                {
                    KLOG_N(TAG, "keeping '%s' on the card before it is evicted",
                           me->keep_name);
                    me->keep_fill = 0U;
                    /* The pacer in this state is always rearmed, never
                     * armX'd: a tick or heartbeat queued before it was
                     * disarmed may find it armed again. */
                    (void)QTimeEvt_rearm(&me->pacer, APPMGR_CHUNK_GAP_TICKS);
                    break;
                }
            }
            if ( !keep_next(me) )
            {
                status = keep_finish(me);
            }
            break;
        }
        case APP_FLASH_CHUNK_SIG: {
            status = Q_HANDLED();
            if ( me->keep_writing )
            {
                /* The heartbeat while AO_Storage writes: the same percent
                 * again keeps the LED pulse and the card alive. */
                progress_publish(me, me->reported);
                (void)QTimeEvt_rearm(&me->pacer, APPMGR_KEEP_BEAT_TICKS);
                break;
            }
            if ( NULL == me->keep_buf )
            {
                /* The entry's own "nothing to do" post, which comes with
                 * every victim settled - or a tick of the pacer that fired
                 * just before it was disarmed, which finds the next victim's
                 * STAT still out and has nothing to say. */
                if ( me->keep_idx >= me->keep_count )
                {
                    status = keep_finish(me);
                }
                break;
            }
            uint32_t n = me->keep_len - me->keep_fill;
            if ( APPMGR_KEEP_CHUNK < n )
            {
                n = APPMGR_KEEP_CHUNK;
            }
            if ( kdrv_apps_read(me->keep_off + me->keep_fill,
                               &me->keep_buf[me->keep_fill], n) != KODE_OK )
            {
                keep_settle(me, KEEP_FAILED);
                if ( !keep_next(me) )
                {
                    status = keep_finish(me);
                }
                break;
            }
            me->keep_fill += n;
            if ( me->keep_fill < me->keep_len )
            {
                (void)QTimeEvt_rearm(&me->pacer, APPMGR_CHUNK_GAP_TICKS);
                break;
            }
            me->keep_writing = true;
            storage_write_all_request(me->keep_path, me->keep_buf,
                                      me->keep_len, &me->super);
            (void)QTimeEvt_rearm(&me->idle,
                                 APPMGR_KEEP_WRITE_TICKS(me->keep_len));
            (void)QTimeEvt_rearm(&me->pacer, APPMGR_KEEP_BEAT_TICKS);
            break;
        }
        case STORAGE_WRITE_RSP_SIG: {
            StorageWriteRspEvt const * const w = Q_EVT_CAST(StorageWriteRspEvt);
            if ( keep_owed_answer(me, e) )
            {
                status = Q_HANDLED();   /* a write given up on, not this one */
                break;
            }
            if ( !me->keep_writing )
            {
                /* The answer to a write already given up on. */
                KLOG_D(TAG, "an answer to a write nobody is waiting for");
                status = Q_HANDLED();
                break;
            }
            (void)QTimeEvt_disarm(&me->pacer);
            if ( (uint8_t)KODE_OK == w->status )
            {
                KLOG_N(TAG, "'%s' kept as %s", me->keep_name, me->keep_path);
                keep_settle(me, KEEP_SAVED);
            }
            else
            {
                /* The path was absent when asked about, so whatever is
                 * there now is this copy's stub (a close that failed leaves
                 * the file): remove it, or the next round would find it and
                 * call the app kept. Its answer is not waited for. */
                storage_path_request(&me->super, (uint8_t)STORAGE_PATH_UNLINK,
                                     me->keep_path);
                keep_settle(me, KEEP_FAILED);
            }
            status = Q_HANDLED();
            if ( !keep_next(me) )
            {
                status = keep_finish(me);
            }
            break;
        }
        case FLASH_REQ_SIG:
        case APP_RUN_REQ_SIG:
        case APP_ERASE_REQ_SIG:
        case APP_LIST_REQ_SIG:
        case APP_RECV_REQ_SIG: {
            KLOG_W(TAG, "busy keeping apps on the card; request ignored");
            erase_deny(e);
            status = Q_HANDLED();
            break;
        }
        case APP_CAT_REQ_SIG: {
            KLOG_W(TAG, "busy keeping apps on the card; app_cat request denied");
            app_cat_deny(me);
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&AppMgr_busy);
            break;
        }
    }
    return status;
}

/* ---- Asking what the next install would evict --------------------------------
 * Answered without doing any of it: the plan is made on a copy of the
 * directory, and the card is asked whether each victim has its copy. Only idle
 * answers; a busy state answers at once with `busy` set.
 */

/**
 * @brief Post an eviction answer's end to its asker.
 *
 * @param who     The asker. Not NULL.
 * @param victims Every app the install would evict.
 * @param fits    The image fits the region once they are gone.
 * @param busy    The question could not be looked at now.
 */
static void evict_end(QActive * const who, uint32_t victims, bool fits,
                      bool busy)
{
    AppEvictEndEvt * const ev = Q_NEW_X(AppEvictEndEvt, 0U, APP_EVICT_END_SIG);

    if ( NULL == ev )
    {
        KLOG_W(TAG, "out of events; the end of an eviction answer is lost");
        return;
    }
    ev->victims = (uint8_t)victims;
    ev->fits    = fits;
    ev->busy    = busy;
    if ( !QACTIVE_POST_X(who, &ev->super, 0U, AO_AppMgr) )
    {
        KLOG_W(TAG, "the asker's queue is full; the end of an eviction answer "
                    "is lost");
    }
}

/**
 * @brief Post one app the install would evict to its asker.
 *
 * @param who     The asker. Not NULL.
 * @param name    The app's project name.
 * @param on_card Its copy is already on the card.
 */
static void evict_entry(QActive * const who, const char *name, bool on_card)
{
    AppEvictEntryEvt * const ev = Q_NEW_X(AppEvictEntryEvt, 0U,
                                          APP_EVICT_ENTRY_SIG);

    if ( NULL == ev )
    {
        KLOG_W(TAG, "out of events; an app of an eviction answer is lost");
        return;
    }
    (void)snprintf(ev->name, sizeof(ev->name), "%s", name);
    ev->on_card = on_card;
    if ( !QACTIVE_POST_X(who, &ev->super, 0U, AO_AppMgr) )
    {
        KLOG_W(TAG, "the asker's queue is full; an app of an eviction answer "
                    "is lost");
    }
}

/**
 * @brief Report the victims from ask_idx on, until one needs the card's word.
 *
 * A victim with no readable name is reported with no copy. Once the card has
 * let one STAT go unanswered (ask_mute), the rest are reported at once with no
 * copy. With every victim reported the end is posted.
 *
 * @param me The AO.
 * @return true when a STAT is out; false when the end was posted.
 */
static bool evict_next(AppMgr * const me)
{
    while ( me->ask_idx < me->ask_count )
    {
        /* kdrv_apps_desc() reads only the offset of its entry, as
         * region_install()'s probe relies on. */
        kapps_entry_t const probe = {
            .offset = me->ask_off[me->ask_idx],
            .size   = 0U,
            .seq    = 0U,
            .flags  = KAPPS_ENTRY_VALID,
        };
        kdrv_slot_desc_t vd;

        if ( kdrv_apps_desc(&probe, &vd, NULL) == KODE_OK )
        {
            (void)snprintf(me->ask_name, sizeof(me->ask_name), "%s",
                           vd.project);
            if ( !me->ask_mute )
            {
                char path[KODE_APP_FILE_MAX];

                card_copy_path(path, sizeof(path), vd.project);
                card_stat(me, path);
                me->ask_stat = true;
                return true;
            }
        }
        else
        {
            unnamed_label(me->ask_name, sizeof(me->ask_name), probe.offset);
        }
        evict_entry(me->ask_who, me->ask_name, false);
        ++me->ask_idx;
    }
    evict_end(me->ask_who, me->ask_victims, true, false);
    me->ask_who = NULL;
    return false;
}

/**
 * @brief Take a question put to an idle AO_AppMgr.
 *
 * Plans an install of the request's size on a copy of the directory, as
 * appmgr_place() will, and notes up to KODE_APP_EVICT_MAX victims. An app with
 * the install's own name is no victim: the install replaces it.
 *
 * @param me The AO.
 * @param e  The request (AppEvictReqEvt).
 * @return AppMgr_asking while a STAT is out; otherwise the question is
 *         already answered and the AO stays idle.
 */
static QState evict_ask(AppMgr * const me, QEvt const * const e)
{
    AppEvictReqEvt const * const req = Q_EVT_CAST(AppEvictReqEvt);
    kapps_plan_t plan;
    kapps_dir_t  dir = *kdrv_apps_dir();     /* a copy: planning writes nothing */
    uint32_t     victims = 0U;
    int          same = -1;                  /* the install's namesake, if any */

    if ( NULL == req->who )
    {
        KLOG_W(TAG, "an eviction question with no asker; ignored");
        return Q_HANDLED();
    }
    if ( kdrv_apps_plan(&dir, kdrv_apps_region_size(), req->size,
                        replacing_idx(&dir, req->project), &plan) != KODE_OK )
    {
        evict_end(req->who, 0U, false, false);
        return Q_HANDLED();
    }
    if ( '\0' != req->project[0] )
    {
        same = appmgr_find_idx(&dir, req->project);
    }

    me->ask_who   = req->who;
    me->ask_mute  = false;
    me->ask_count = 0U;
    me->ask_idx   = 0U;
    for ( uint32_t n = 0U; n <= plan.evict_count; ++n )
    {
        uint8_t const idx = plan_victim(&plan, n);
        bool const    slot = (n == plan.evict_count);   /* the directory's own */

        if ( KAPPS_NO_VICTIM == idx )
        {
            continue;
        }
        if ( (0 <= same) && (slot || ((int)idx == same)) )
        {
            continue;        /* replaced by the install, not lost to it */
        }
        ++victims;
        if ( KODE_APP_EVICT_MAX > me->ask_count )
        {
            me->ask_off[me->ask_count] = dir.e[idx].offset;
            ++me->ask_count;
        }
    }
    me->ask_victims = victims;

    if ( evict_next(me) )
    {
        return Q_TRAN(&AppMgr_asking);
    }
    return Q_HANDLED();
}

/**
 * @brief Refuse a request that arrives while a state with an answer to give is
 *        busy with it.
 *
 * What AppMgr_asking, AppMgr_finding and AppMgr_scrubbing all do: an install,
 * run, listing or receive request is logged and dropped, and an erase or an
 * app_cat request is answered KODE_DENIED, so nobody waits on it.
 *
 * @param me    The AO.
 * @param e     The event.
 * @param doing What the AO is busy with, for the log.
 * @return true when @p e was such a request and is dealt with.
 */
static bool busy_refused(AppMgr * const me, QEvt const * const e,
                         const char *doing)
{
    bool refused = true;

    switch ( e->sig )
    {
        case FLASH_REQ_SIG:
        case APP_RUN_REQ_SIG:
        case APP_ERASE_REQ_SIG:
        case APP_LIST_REQ_SIG:
        case APP_RECV_REQ_SIG: {
            KLOG_W(TAG, "busy %s; request ignored", doing);
            erase_deny(e);
            break;
        }
        case APP_CAT_REQ_SIG: {
            KLOG_W(TAG, "busy %s; app_cat request denied", doing);
            app_cat_deny(me);
            break;
        }
        default: {
            refused = false;
            break;
        }
    }
    return refused;
}

/**
 * @brief Take the answer to the STAT a question has out, if @p e is it.
 *
 * Settles an answer owed to a STAT given up on first, and an unlink's answer
 * is no STAT's. When it is the awaited one the flag is cleared and the
 * deadline stopped.
 *
 * @param me   The AO.
 * @param e    STORAGE_PATH_RSP_SIG.
 * @param stat The question's "a STAT is out" flag.
 * @return true when @p e answers the question's STAT.
 */
static bool card_answer_taken(AppMgr * const me, QEvt const * const e,
                              bool * const stat)
{
    StoragePathRspEvt const * const r = Q_EVT_CAST(StoragePathRspEvt);

    if ( (uint8_t)STORAGE_PATH_STAT != r->op )
    {
        return false;
    }
    if ( keep_owed_answer(me, e) || (!*stat) )
    {
        return false;            /* a STAT given up on, not this question's */
    }
    *stat = false;
    (void)QTimeEvt_disarm(&me->idle);
    return true;
}

/**
 * @brief The card let the question's STAT go unanswered: give up on it.
 *
 * The late answer that may still come is owed, as keeping owes it.
 *
 * @param me   The AO.
 * @param stat The question's "a STAT is out" flag.
 * @param name The app asked about, for the log.
 * @return true when a STAT was out and is now given up on; false for a stale
 *         expiry.
 */
static bool card_answer_lapsed(AppMgr * const me, bool * const stat,
                               const char *name)
{
    if ( !*stat )
    {
        return false;
    }
    KLOG_W(TAG, "nothing answered about '%s' on the card", name);
    if ( UINT8_MAX > me->keep_stale_stat )
    {
        ++me->keep_stale_stat;
    }
    *stat = false;
    return true;
}

/**
 * @brief Take a find request put to an idle AO_AppMgr.
 *
 * An installed app is answered from the directory at once. Any other is
 * looked for on the card: a STAT goes out, and AppMgr_finding waits for it.
 *
 * @param me The AO.
 * @param e  The request (AppFindReqEvt).
 * @return AppMgr_finding while a STAT is out; otherwise the question is
 *         already answered and the AO stays idle.
 */
static QState find_ask(AppMgr * const me, QEvt const * const e)
{
    AppFindReqEvt const * const req = Q_EVT_CAST(AppFindReqEvt);
    kapps_dir_t const * const   dir = kdrv_apps_dir();
    int const                   idx = appmgr_find_idx(dir, req->project);

    if ( NULL == req->who )
    {
        KLOG_W(TAG, "a find request with no asker; ignored");
        return Q_HANDLED();
    }
    if ( 0 <= idx )
    {
        find_reply(req->who, req->project, &dir->e[idx], false, false);
        return Q_HANDLED();
    }
    char path[KODE_APP_FILE_MAX];

    me->find_who = req->who;
    (void)snprintf(me->find_name, sizeof(me->find_name), "%s", req->project);
    card_copy_path(path, sizeof(path), me->find_name);
    card_stat(me, path);
    me->find_stat = true;
    return Q_TRAN(&AppMgr_finding);
}

/**
 * @brief Take a keep-only request put to an idle AO_AppMgr.
 *
 * Clears every other app from the directory in one commit, erases each one's
 * image header (AppMgr_scrubbing), then deletes each one's data folder.
 * Nothing changes when the app named is not installed.
 *
 * @param me The AO.
 * @param e  The request (AppKeepOnlyReqEvt).
 * @return AppMgr_scrubbing while headers are being erased, AppMgr_tidying if
 *         only folders are left; otherwise the question is already answered
 *         and the AO stays idle.
 */
static QState keep_only_ask(AppMgr * const me, QEvt const * const e)
{
    AppKeepOnlyReqEvt const * const req = Q_EVT_CAST(AppKeepOnlyReqEvt);
    kapps_dir_t  dir = *kdrv_apps_dir();
    uint32_t     erased = 0U;
    bool const   none = ('\0' == req->project[0]);   /* no app stays */
    int const    keep = none ? -1 : appmgr_find_idx(&dir, req->project);

    if ( NULL == req->who )
    {
        KLOG_W(TAG, "a keep-only request with no asker; ignored");
        return Q_HANDLED();
    }
    if ( (0 > keep) && !none )
    {
        KLOG_W(TAG, "'%s' is not installed; nothing erased", req->project);
        keep_only_reply(req->who, KODE_DENIED, 0U);
        return Q_HANDLED();
    }
    victims_clear(me);
    for ( uint32_t i = 0U; i < dir.count; ++i )
    {
        kdrv_slot_desc_t vd;

        if ( ((dir.e[i].flags & KAPPS_ENTRY_VALID) == 0U) || ((uint32_t)keep == i) )
        {
            continue;
        }
        if ( kdrv_apps_desc(&dir.e[i], &vd, NULL) == KODE_OK )
        {
            victim_add(me, vd.project);
        }
        me->keep_only_off[erased] = dir.e[i].offset;
        ++erased;
        dir.e[i].flags = 0U;
    }
    me->keep_only_who    = req->who;
    me->keep_only_erased = (uint8_t)erased;
    me->keep_only_next   = 0U;
    (void)snprintf(me->keep_only_name, sizeof(me->keep_only_name), "%s",
                   req->project);
    if ( 0U == erased )
    {
        tidy_begin(me, me->keep_only_name, (uint8_t)TIDY_AFTER_KEEP);
        return tidy_state(me);          /* nothing to remove: the answer is OK */
    }
    appdir_compact(&dir);
    if ( kdrv_apps_dir_commit(&dir) != KODE_OK )
    {
        KLOG_E(TAG, "cannot update the directory; nothing erased");
        victims_clear(me);
        me->keep_only_who = NULL;
        keep_only_reply(req->who, KODE_ERROR, 0U);
        return Q_HANDLED();
    }
    /* The headers one step at a time (rule 5): up to 63 NOR erases are not
     * one dispatch's work. */
    QACTIVE_POST(&me->super, &s_keep_only_step_evt, me);
    return Q_TRAN(&AppMgr_scrubbing);
}

/* ---- An app's data folder fetched from a URL -------------------------------
 * An app's data travels apart from its image and may come first. This AO reads
 * the folder's two files from a base URL into one buffer and hands it to
 * AO_Storage, which is the folder's only writer. One fetch at a time, and none
 * while an image is written: the transfer is AO_Net's one.
 */

/**
 * @brief Answer a fetch.
 *
 * @param who     The asker.
 * @param project Whose folder.
 * @param why     How it ended.
 * @param icon    Whether the folder written holds an icon.
 */
static void get_reply(QActive * const who, const char *project,
                      appdata_get_why_t why, bool icon)
{
    AppDataGetRspEvt * const ev = Q_NEW_X(AppDataGetRspEvt, 0U,
                                          APPDATA_GET_RSP_SIG);

    if ( NULL == ev )
    {
        KLOG_W(TAG, "out of events; the answer to a data folder's fetch is lost");
        return;
    }
    (void)snprintf(ev->project, sizeof(ev->project), "%s", project);
    ev->why  = (uint8_t)why;
    ev->icon = icon;
    if ( !QACTIVE_POST_X(who, &ev->super, 0U, AO_AppMgr) )
    {
        KLOG_W(TAG, "the asker's queue is full; the answer to a data folder's "
                    "fetch is lost");
    }
}

#if CONFIG_KODEOS_NET
/**
 * @brief Give back what a fetch still holds: the files' buffer, unless
 *        AO_Storage has it, and the base URL.
 *
 * @param me The AO.
 */
static void get_release(AppMgr * const me)
{
    if ( NULL != me->get_buf )
    {
        khal_free(me->get_buf);
        me->get_buf = NULL;
    }
    if ( NULL != me->get_url )
    {
        khal_free(me->get_url);
        me->get_url = NULL;
    }
}

/* A base URL with either file name after it is a URL a transfer takes whole. */
_Static_assert((KODE_APPDATA_URL_MAX + sizeof(GET_JSON_NAME)) <= KODE_FLASH_SRC_MAX,
               "a data folder's base URL and a file name fit a transfer's URL");
_Static_assert(sizeof(GET_ICON_NAME) == sizeof(GET_JSON_NAME),
               "the folder's two file names are the same length");

/**
 * @brief Open one of the folder's two files at the base URL.
 *
 * The answer's deadline is the caller's to arm: the first file is opened on
 * the way into the state, whose entry arms it.
 *
 * @param me   The AO.
 * @param file The file's name.
 */
static void get_open_file(AppMgr * const me, const char *file)
{
    char url[KODE_FLASH_SRC_MAX];

    (void)snprintf(url, sizeof(url), "%s%s", me->get_url, file);
    me->get_want = 0U;
    me->get_fill = 0U;
    me->get_open = true;
    net_stream_open_request(url, &me->super);
}

/**
 * @brief End a fetch that did not make a folder.
 *
 * @param me  The AO.
 * @param why What went wrong.
 * @return The transition to idle.
 */
static QState get_fail(AppMgr * const me, appdata_get_why_t why)
{
    if ( me->get_open )
    {
        net_stream_close_request();
        me->get_open = false;
    }
    get_reply(me->get_who, me->get_project, why, false);
    return Q_TRAN(&AppMgr_idle);
}

/**
 * @brief Hand the fetched folder to AO_Storage.
 *
 * @param me The AO.
 * @return The state's answer: it stays for AO_Storage's, or ends busy.
 */
static QState get_put(AppMgr * const me)
{
    if ( appdata_put_request(me->get_project, me->get_buf, me->get_json_len,
                             me->get_icon_len, &me->super) != KODE_OK )
    {
        return get_fail(me, APPDATA_GET_BUSY);   /* the buffer is still ours */
    }
    me->get_buf  = NULL;                         /* AO_Storage's from here */
    me->get_step = (uint8_t)GET_STEP_PUT;
    (void)QTimeEvt_rearm(&me->idle, APPMGR_TIDY_TICKS);
    return Q_HANDLED();
}

/**
 * @brief The file in hand has arrived whole: check it and go on.
 *
 * app.json is read with the folder's own parser, so a file AO_Storage would
 * refuse to show is never written. It says whether there is an icon: one it
 * names must be icon.k64, the name the folder gives it, and is fetched next.
 *
 * @param me The AO.
 * @return The state's answer.
 */
static QState get_file_done(AppMgr * const me)
{
    net_stream_close_request();
    me->get_open = false;

    if ( (uint8_t)GET_STEP_ICON == me->get_step )
    {
        me->get_icon_len = (uint16_t)me->get_fill;
        return get_put(me);
    }

    kapp_json_status_t why = KAPP_JSON_NONE;
    kapp_fetch_t const ask = kapp_data_fetched((const char *)me->get_buf,
                                               me->get_fill, &why);
    if ( KAPP_FETCH_REFUSED == ask )
    {
        KLOG_W(TAG, "the app.json fetched for '%s' is refused (%s); it must be "
                    "one the board reads, and name no icon but "
                    GET_ICON_NAME, me->get_project, kapp_json_status_name(why));
        return get_fail(me, APPDATA_GET_JSON);
    }
    me->get_json_len = (uint16_t)me->get_fill;
    if ( KAPP_FETCH_NO_ICON == ask )
    {
        return get_put(me);                      /* an app with no icon */
    }
    me->get_step = (uint8_t)GET_STEP_ICON;
    get_open_file(me, GET_ICON_NAME);
    (void)QTimeEvt_rearm(&me->idle, APPMGR_OPEN_TICKS);
    return Q_HANDLED();
}

/**
 * @brief The open of the file in hand was answered.
 *
 * The length is judged before a byte is read: app.json may be anything up to
 * the reader's limit, an icon is one size.
 *
 * @param me  The AO.
 * @param rsp The answer.
 * @return The state's answer.
 */
static QState get_opened(AppMgr * const me, StorageOpenRspEvt const * const rsp)
{
    if ( (uint8_t)KODE_OK != rsp->status )
    {
        me->get_open = false;                    /* nothing was opened */
        return get_fail(me, APPDATA_GET_FETCH);
    }
    if ( 0U == rsp->size )
    {
        KLOG_W(TAG, "the server gave no length for a file of '%s'",
               me->get_project);
        return get_fail(me, APPDATA_GET_FETCH);
    }
    if ( (uint8_t)GET_STEP_JSON == me->get_step )
    {
        if ( KAPP_DATA_JSON_MAX < rsp->size )
        {
            return get_fail(me, APPDATA_GET_JSON);
        }
    }
    else if ( KAPP_ICON_BYTES != rsp->size )
    {
        KLOG_W(TAG, "the icon fetched for '%s' is %lu bytes, not %lu",
               me->get_project, (unsigned long)rsp->size,
               (unsigned long)KAPP_ICON_BYTES);
        return get_fail(me, APPDATA_GET_ICON);
    }
    else
    {
        /* An icon of the one size there is. */
    }
    me->get_want = rsp->size;
    (void)QTimeEvt_rearm(&me->idle, APPMGR_OPEN_TICKS);
    net_stream_read_request();
    return Q_HANDLED();
}

/**
 * @brief A chunk of the file in hand arrived.
 *
 * get_want is the file's length from its open's answer, and 0 between a
 * file's end and the next one's open.
 *
 * @param me The AO.
 * @param d  The chunk.
 * @return The state's answer.
 */
static QState get_chunk(AppMgr * const me, StorageDataEvt const * const d)
{
    if ( 0U == me->get_want )
    {
        /* No file is open: the open in flight has not been answered. This is
         * a chunk of the file before it, sent before its close was read, and
         * taking it for the new file's would end that file empty. */
        return Q_HANDLED();
    }
    if ( (uint8_t)KODE_OK != d->status )
    {
        return get_fail(me, APPDATA_GET_FETCH);
    }
    if ( (uint32_t)d->len > (me->get_want - me->get_fill) )
    {
        KLOG_W(TAG, "a file of '%s' brought more than it said", me->get_project);
        return get_fail(me, APPDATA_GET_FETCH);
    }
    /* app.json fills the buffer's start; the icon goes right after it. */
    size_t at = (size_t)me->get_fill;
    if ( (uint8_t)GET_STEP_ICON == me->get_step )
    {
        at += (size_t)me->get_json_len;
    }
    const uint8_t * const src = net_stream_buf(d->idx);
    if ( NULL == src )
    {
        return get_fail(me, APPDATA_GET_FETCH);  /* a buffer AO_Net has not got */
    }
    (void)memcpy(&me->get_buf[at], src, d->len);
    me->get_fill += (uint32_t)d->len;

    if ( me->get_fill == me->get_want )
    {
        return get_file_done(me);
    }
    if ( d->eof )
    {
        KLOG_W(TAG, "a file of '%s' ended %lu bytes short", me->get_project,
               (unsigned long)(me->get_want - me->get_fill));
        return get_fail(me, APPDATA_GET_FETCH);
    }
    (void)QTimeEvt_rearm(&me->idle, APPMGR_OPEN_TICKS);
    net_stream_return(d->idx);                   /* and asks for the next */
    return Q_HANDLED();
}
#endif /* CONFIG_KODEOS_NET */

/**
 * @brief Begin a fetch: refuse what cannot be one, take the buffer, open app.json.
 *
 * @param me  The AO, idle.
 * @param req The request.
 * @return The transition to AppMgr_fetching, or handled when it was refused.
 */
static QState get_begin(AppMgr * const me, AppDataGetReqEvt const * const req)
{
    char folder[KODE_PATH_MAX];

    if ( NULL == req->who )
    {
        return Q_HANDLED();
    }
    if ( !appdata_folder_path(folder, sizeof(folder), APPDATA_MEDIUM_FS,
                              req->project) )
    {
        KLOG_W(TAG, "'%s' cannot name a data folder", req->project);
        get_reply(req->who, req->project, APPDATA_GET_NAME, false);
        return Q_HANDLED();
    }
#if CONFIG_KODEOS_NET
    /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() answers void *: PSRAM is taken by name (rule 8) */
    me->get_buf = (uint8_t *)khal_alloc_psram(1U, (size_t)KAPP_DATA_JSON_MAX
                                                  + (size_t)KAPP_ICON_BYTES);
    /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() answers void *: PSRAM is taken by name (rule 8) */
    me->get_url = (char *)khal_alloc_psram(1U, (size_t)KODE_APPDATA_URL_MAX);
    if ( (NULL == me->get_buf) || (NULL == me->get_url) )
    {
        KLOG_W(TAG, "no PSRAM to fetch the data folder of '%s'", req->project);
        get_release(me);
        get_reply(req->who, req->project, APPDATA_GET_BUSY, false);
        return Q_HANDLED();
    }
    me->get_who      = req->who;
    me->get_step     = (uint8_t)GET_STEP_JSON;
    me->get_json_len = 0U;
    me->get_icon_len = 0U;
    me->get_open     = false;
    (void)snprintf(me->get_project, sizeof(me->get_project), "%s", req->project);
    (void)snprintf(me->get_url, (size_t)KODE_APPDATA_URL_MAX, "%s", req->url);
    KLOG_N(TAG, "fetching the data folder of '%s'", me->get_project);
    get_open_file(me, GET_JSON_NAME);
    return Q_TRAN(&AppMgr_fetching);
#else
    KLOG_E(TAG, "this firmware has no network services");
    get_reply(req->who, req->project, APPDATA_GET_FETCH, false);
    return Q_HANDLED();
#endif
}

/**
 * @brief Reading an app's data folder from a URL, then waiting for AO_Storage
 *        to write it.
 *
 * Every wait has the deadline timer the other transfers use; the states are
 * mutually exclusive and each disarms it on the way out. The buffer is this
 * AO's until AO_Storage takes it, and whatever is still held is freed on the
 * way out.
 */
static QState AppMgr_fetching(AppMgr * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
#if CONFIG_KODEOS_NET
        case Q_ENTRY_SIG: {
            QTimeEvt_armX(&me->idle, APPMGR_OPEN_TICKS, 0U);
            status = Q_HANDLED();
            break;
        }
        case NET_STREAM_OPEN_RSP_SIG: {
            status = get_opened(me, Q_EVT_CAST(StorageOpenRspEvt));
            break;
        }
        case NET_STREAM_DATA_SIG: {
            status = get_chunk(me, Q_EVT_CAST(StorageDataEvt));
            break;
        }
        case APPDATA_PUT_RSP_SIG: {
            AppDataPutRspEvt const * const rsp = Q_EVT_CAST(AppDataPutRspEvt);

            status = Q_HANDLED();
            if ( ((uint8_t)GET_STEP_PUT == me->get_step)
                && (strcmp(rsp->project, me->get_project) == 0) )
            {
                appdata_get_why_t why = APPDATA_GET_STORE;

                if ( KODE_OK == rsp->rc )
                {
                    why = APPDATA_GET_OK;
                }
                else if ( KODE_NO_RESOURCES == rsp->rc )
                {
                    why = APPDATA_GET_FULL;
                }
                else if ( KODE_DENIED == rsp->rc )
                {
                    why = APPDATA_GET_BUSY;
                }
                else
                {
                    /* The folder could not be written. */
                }
                get_reply(me->get_who, me->get_project, why,
                          (APPDATA_GET_OK == why) && (0U != me->get_icon_len));
                status = Q_TRAN(&AppMgr_idle);
            }
            break;
        }
        case APP_RECV_IDLE_SIG: {
            if ( (uint8_t)GET_STEP_PUT == me->get_step )
            {
                KLOG_W(TAG, "AO_Storage never said how the data folder of '%s' "
                            "went", me->get_project);
                status = get_fail(me, APPDATA_GET_STORE);
            }
            else
            {
                KLOG_W(TAG, "the data folder of '%s' was never answered",
                       me->get_project);
                status = get_fail(me, APPDATA_GET_FETCH);
            }
            break;
        }
        case Q_EXIT_SIG: {
            (void)QTimeEvt_disarm(&me->idle);
            get_release(me);
            status = Q_HANDLED();
            break;
        }
#endif /* CONFIG_KODEOS_NET */
        default: {
            if ( busy_refused(me, e, "fetching an app's data folder") )
            {
                status = Q_HANDLED();
            }
            else
            {
                status = Q_SUPER(&AppMgr_busy);
            }
            break;
        }
    }
    return status;
}

/**
 * @brief The parent of every state with work in hand.
 *
 * Answers a question about the next install with `busy`, and settles the late
 * AO_Storage answers owed to a STAT or a write given up on, for every child
 * that does not take them itself.
 */
static QState AppMgr_busy(AppMgr * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case STORAGE_PATH_RSP_SIG:
        case STORAGE_WRITE_RSP_SIG: {
            (void)keep_owed_answer(me, e);
            status = Q_HANDLED();
            break;
        }
        case APP_ERASE_REQ_SIG: {
            /* A state under this one that names no refusal of its own. */
            KLOG_W(TAG, "busy; the erase of '%s' is refused",
                   Q_EVT_CAST(AppFlashEvt)->file);
            erase_deny(e);
            status = Q_HANDLED();
            break;
        }
        case APP_EVICT_REQ_SIG: {
            AppEvictReqEvt const * const req = Q_EVT_CAST(AppEvictReqEvt);

            if ( NULL != req->who )
            {
                evict_end(req->who, 0U, false, true);
            }
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            /* Every state under this one holds work that must not be cut. */
            lowpower_ready(me->lp_who, Q_EVT_CAST(LowPowerRoundEvt)->round,
                           true, "app write");
            status = Q_HANDLED();
            break;
        }
#endif
        case APP_FIND_REQ_SIG: {
            AppFindReqEvt const * const req = Q_EVT_CAST(AppFindReqEvt);

            if ( NULL != req->who )
            {
                find_reply(req->who, req->project, NULL, false, true);
            }
            status = Q_HANDLED();
            break;
        }
        case APP_KEEP_ONLY_REQ_SIG: {
            AppKeepOnlyReqEvt const * const req = Q_EVT_CAST(AppKeepOnlyReqEvt);

            if ( NULL != req->who )
            {
                KLOG_W(TAG, "busy; keep-only request for '%s' denied",
                       req->project);
                keep_only_reply(req->who, KODE_DENIED, 0U);
            }
            status = Q_HANDLED();
            break;
        }
        case APPDATA_GET_REQ_SIG: {
            AppDataGetReqEvt const * const req = Q_EVT_CAST(AppDataGetReqEvt);

            if ( NULL != req->who )
            {
                KLOG_W(TAG, "busy; the fetch of the data folder of '%s' is "
                            "refused", req->project);
                get_reply(req->who, req->project, APPDATA_GET_BUSY, false);
            }
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }
    return status;
}

/**
 * @brief Waiting for AO_Storage to say whether the victim in hand has its copy
 *        on the card.
 *
 * Each answer is reported and the next victim asked about, until the end. A
 * STAT nobody answers within the deadline reads as no copy and is owed, as
 * keeping owes it, to the late answer that may still come.
 */
static QState AppMgr_asking(AppMgr * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case Q_EXIT_SIG: {
            (void)QTimeEvt_disarm(&me->idle);
            status = Q_HANDLED();
            break;
        }
        case STORAGE_PATH_RSP_SIG: {
            status = Q_HANDLED();
            if ( card_answer_taken(me, e, &me->ask_stat) )
            {
                evict_entry(me->ask_who, me->ask_name,
                            card_has_copy(Q_EVT_CAST(StoragePathRspEvt)));
                ++me->ask_idx;
                if ( !evict_next(me) )
                {
                    status = Q_TRAN(&AppMgr_idle);
                }
            }
            break;
        }
        case APP_RECV_IDLE_SIG: {
            status = Q_HANDLED();
            if ( card_answer_lapsed(me, &me->ask_stat, me->ask_name) )
            {
                me->ask_mute = true; /* the rest are not asked: one debt, not many */
                evict_entry(me->ask_who, me->ask_name, false);
                ++me->ask_idx;
                if ( !evict_next(me) )
                {
                    status = Q_TRAN(&AppMgr_idle);
                }
            }
            break;
        }
        default: {
            if ( busy_refused(me, e, "answering which apps an install would "
                                    "evict") )
            {
                status = Q_HANDLED();
            }
            else
            {
                status = Q_SUPER(&AppMgr_busy);
            }
            break;
        }
    }
    return status;
}

/**
 * @brief Waiting for AO_Storage to say whether an app that is not installed
 *        has its copy on the card.
 *
 * The answer is posted to the asker and the AO goes back to idle. A STAT
 * nobody answers within the deadline reads as no copy and is owed, as keeping
 * owes it, to the late answer that may still come.
 */
static QState AppMgr_finding(AppMgr * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case Q_EXIT_SIG: {
            (void)QTimeEvt_disarm(&me->idle);
            status = Q_HANDLED();
            break;
        }
        case STORAGE_PATH_RSP_SIG: {
            status = Q_HANDLED();
            if ( card_answer_taken(me, e, &me->find_stat) )
            {
                find_reply(me->find_who, me->find_name, NULL,
                           card_has_copy(Q_EVT_CAST(StoragePathRspEvt)), false);
                me->find_who = NULL;
                status = Q_TRAN(&AppMgr_idle);
            }
            break;
        }
        case APP_RECV_IDLE_SIG: {
            status = Q_HANDLED();
            if ( card_answer_lapsed(me, &me->find_stat, me->find_name) )
            {
                find_reply(me->find_who, me->find_name, NULL, false, false);
                me->find_who = NULL;
                status = Q_TRAN(&AppMgr_idle);
            }
            break;
        }
        default: {
            if ( busy_refused(me, e, "looking for an app") )
            {
                status = Q_HANDLED();
            }
            else
            {
                status = Q_SUPER(&AppMgr_busy);
            }
            break;
        }
    }
    return status;
}

/**
 * @brief Erasing the image headers of the apps a keep-only request removed.
 *
 * One header a step, each step a self-posted event, so the AO answers what
 * arrives in between (a refusal, as the other states give). The directory is
 * already committed. With the last header gone the data folders are next.
 */
static QState AppMgr_scrubbing(AppMgr * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case APP_KEEP_ONLY_STEP_SIG: {
            /* kdrv logs a failed erase; the directory no longer names the app. */
            (void)kdrv_apps_erase_header(me->keep_only_off[me->keep_only_next]);
            ++me->keep_only_next;
            if ( me->keep_only_next < me->keep_only_erased )
            {
                QACTIVE_POST(&me->super, &s_keep_only_step_evt, me);
                status = Q_HANDLED();
            }
            else
            {
                tidy_begin(me, me->keep_only_name, (uint8_t)TIDY_AFTER_KEEP);
                status = tidy_state(me);
            }
            break;
        }
        default: {
            if ( busy_refused(me, e, "erasing apps") )
            {
                status = Q_HANDLED();
            }
            else
            {
                status = Q_SUPER(&AppMgr_busy);
            }
            break;
        }
    }
    return status;
}

/**
 * @brief Waiting for AO_Storage to put the installed app's folders in order.
 *
 * One request at a time, each under APPMGR_TIDY_MS; a refused or timed-out
 * folder is a warning, never a failure. Other requests are refused, except a
 * listing, which the committed directory answers.
 */
static QState AppMgr_tidying(AppMgr * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case Q_EXIT_SIG: {
            (void)QTimeEvt_disarm(&me->tidy);
            status = Q_HANDLED();
            break;
        }
        case APPDATA_PUT_RSP_SIG: {
            AppDataPutRspEvt const * const rsp = Q_EVT_CAST(AppDataPutRspEvt);
            status = Q_HANDLED();
            if ( !tidy_awaited(me, e) )
            {
                KLOG_D(TAG, "an answer to a put nobody is waiting for");
                break;
            }
            tidy_answered(me);
            if ( KODE_OK != rsp->rc )
            {
                KLOG_W(TAG, "the data folder of '%s' was not written (code "
                            "%d); the rest goes on", me->tidy_name,
                       (int)rsp->rc);
            }
            if ( !tidy_advance(me) )
            {
                tidy_finish(me);
                status = tidy_state(me);
            }
            break;
        }
        case APPDATA_RM_RSP_SIG: {
            AppDataRmRspEvt const * const rsp = Q_EVT_CAST(AppDataRmRspEvt);
            status = Q_HANDLED();
            if ( !tidy_awaited(me, e) )
            {
                KLOG_D(TAG, "an answer to a delete nobody is waiting for");
                break;
            }
            tidy_answered(me);
            /* KODE_ERROR says there was no folder: what a delete is for. */
            if ( ((uint8_t)KODE_OK != rsp->status)
                && ((uint8_t)KODE_ERROR != rsp->status) )
            {
                KLOG_W(TAG, "the data folder of '%s' was not deleted (code "
                            "%u); the rest goes on", tidy_who(me),
                       (unsigned)rsp->status);
            }
            if ( !tidy_advance(me) )
            {
                tidy_finish(me);
                status = tidy_state(me);
            }
            break;
        }
        case STORAGE_APP_WIPE_RSP_SIG: {
            status = Q_HANDLED();
            if ( !tidy_awaited(me, e) )
            {
                KLOG_D(TAG, "an answer to a wipe nobody is waiting for");
                break;
            }
            tidy_answered(me);
            erase_take_wipe(me, Q_EVT_CAST(StorageAppWipeRspEvt));
            if ( !tidy_advance(me) )
            {
                tidy_finish(me);
                status = tidy_state(me);
            }
            break;
        }
        case APP_TIDY_TIMEOUT_SIG: {
            status = Q_HANDLED();
            if ( me->tidy_swallow )
            {
                me->tidy_swallow = false;   /* the deadline of an answered request */
                break;
            }
            KLOG_W(TAG, "AO_Storage did not answer for the data folder of "
                        "'%s'; the rest goes on", tidy_who(me));
            /* Nothing to free: a put's buffer is AO_Storage's. */
            if ( !tidy_advance(me) )
            {
                tidy_finish(me);
                status = tidy_state(me);
            }
            break;
        }
        case FLASH_REQ_SIG:
        case APP_RUN_REQ_SIG:
        case APP_ERASE_REQ_SIG:
        case APP_RECV_REQ_SIG: {
            KLOG_W(TAG, "busy tidying the data folder of '%s'; request ignored",
                   me->tidy_name);
            erase_deny(e);
            status = Q_HANDLED();
            break;
        }
        case APP_LIST_REQ_SIG: {
            /* The directory is committed before the tidy starts. */
            app_list_page(Q_EVT_CAST(AppListReqEvt));
            status = Q_HANDLED();
            break;
        }
        case APP_CAT_REQ_SIG: {
            KLOG_W(TAG, "busy tidying the data folder of '%s'; app_cat request "
                        "denied", me->tidy_name);
            app_cat_deny(me);
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&AppMgr_busy);
            break;
        }
    }
    return status;
}

/**
 * @brief Writing a file's or a URL's chunks, each buffer handed back after a gap.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState AppMgr_flashing(AppMgr * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            QTimeEvt_armX(&me->idle, APPMGR_CHUNK_TICKS, 0U);
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            (void)QTimeEvt_disarm(&me->idle);
            status = Q_HANDLED();
            break;
        }
        case APP_CANCEL_REQ_SIG: {
            flash_fail(me, "the person went back", KFAIL_WRITE);
            status = Q_TRAN(&AppMgr_idle);
            break;
        }
        case APP_RECV_IDLE_SIG: {
            /* The deadline timer AppMgr_opening shares; see there. */
            flash_fail(me, "the medium stopped sending", KFAIL_WRITE);
            status = Q_TRAN(&AppMgr_idle);
            break;
        }
        case STORAGE_DATA_SIG:
        case NET_STREAM_DATA_SIG: {
            StorageDataEvt const * const d = Q_EVT_CAST(StorageDataEvt);
            uint8_t const idx = d->idx;
            if ( !consume_chunk(me, d) )
            {
                if ( me->keep_pending )
                {
                    keep_pause(me);
                    status = Q_TRAN(&AppMgr_keeping);
                    break;
                }
                status = tidy_state(me);   /* tidying after a commit, else idle */
                break;
            }
            (void)QTimeEvt_rearm(&me->idle, APPMGR_CHUNK_TICKS);
            /* Remember the buffer rather than returning it now: the gap is
             * what keeps the GUI alive across the erase. One pacer for however
             * many buffers are waiting - arming it per buffer is what produced
             * two "next" requests for one loan. */
            me->to_return |= (uint8_t)(1U << idx);
            if ( !me->pacing )
            {
                me->pacing = true;
                QTimeEvt_armX(&me->pacer, APPMGR_CHUNK_GAP_TICKS, 0U);
            }
            status = Q_HANDLED();
            break;
        }
        case APP_FLASH_CHUNK_SIG: {
            me->pacing = false;
            uint8_t const mask = me->to_return;
            me->to_return = 0U;
            for ( uint8_t i = 0U; 8U > i; ++i )
            {
                if ( (mask & (uint8_t)(1U << i)) != 0U )
                {
                    medium_return(me, i);
                }
            }
            status = Q_HANDLED();
            break;
        }
        /* Serialising for free: a second request while a copy is in flight is
         * refused rather than queued, so two images can never interleave in
         * the same slot. */
        case FLASH_REQ_SIG:
        case APP_RUN_REQ_SIG:
        case APP_ERASE_REQ_SIG:
        case APP_LIST_REQ_SIG: {
            KLOG_W(TAG, "busy writing %s (%u%%); request ignored",
                   me->os_target ? "the kodeOS slot" : "the region",
                   (unsigned)((me->written * 100U) / me->total));
            erase_deny(e);
            status = Q_HANDLED();
            break;
        }
        case APP_CAT_REQ_SIG: {
            KLOG_W(TAG, "busy writing %s (%u%%); app_cat request denied",
                   me->os_target ? "the kodeOS slot" : "the region",
                   (unsigned)((me->written * 100U) / me->total));
            app_cat_deny(me);
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&AppMgr_busy);
            break;
        }
    }
    return status;
}

/** One chunk per APP_FLASH_CHUNK_SIG, paced like the file path. Sibling of
 *  AppMgr_flashing, but its source is kdrv_apps_pending_read - it never
 *  talks to AO_Storage. */
static QState AppMgr_adopting(AppMgr * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case APP_FLASH_CHUNK_SIG: {
            size_t const left = me->src_len - me->consumed;
            size_t       n    = (size_t)KODE_RECV_CHUNK;
            if ( left < n )
            {
                n = left;
            }
            /* Read into `chunk` FIRST, then feed only what the read
             * just filled: the write must never run on a stale buffer, so a
             * failed read leaves wst at KODE_ERROR and skips the feed. */
            kode_status_t const rst = kdrv_apps_pending_read(me->consumed, me->chunk, n);
            kode_status_t wst = KODE_ERROR;
            if ( KODE_OK == rst )
            {
                wst = xfer_feed(me, me->chunk, n);
            }
            if ( (KODE_OK == rst) && (KODE_OK != wst) && me->keep_pending )
            {
                /* The decoder's own plan, made with the container's real
                 * length, found victims the walk of the mailbox did not:
                 * copy them, then adopt again. Nothing was committed. The
                 * session is closed here because the mailbox never sets
                 * `writing`, which is what keep_pause() closes it by. */
                xfer_abort(me);
                KLOG_I(TAG, "adoption paused to keep apps on the card");
                keep_pause(me);
                status = Q_TRAN(&AppMgr_keeping);
                break;
            }
            if ( (KODE_OK != rst) || (KODE_OK != wst) )
            {
                /* An I/O failure mid-copy says nothing of the image: the
                 * mailbox stays, and the next boot copies it again. */
                xfer_abort(me);
                me->os_target = false;
                keep_clear(me);
                KLOG_E(TAG, "adoption failed mid-copy; mailbox left intact");
                status = Q_TRAN(&AppMgr_idle);
                break;
            }
            me->consumed += (uint32_t)n;
            if ( me->consumed < me->src_len )
            {
                /* rearm: a stale chunk may have found the pacer armed. */
                (void)QTimeEvt_rearm(&me->pacer, APPMGR_CHUNK_GAP_TICKS);
                status = Q_HANDLED();
                break;
            }
            adopt_finish(me);
            if ( !me->tidy_on )
            {
                keep_clear(me);    /* every way adoption ends ends its round;
                                    * a tidying one ends it in tidy_finish() */
            }
            status = tidy_state(me);
            break;
        }
        /* Anything else waits: adoption runs once, at boot, briefly. */
        case FLASH_REQ_SIG:
        case APP_RUN_REQ_SIG:
        case APP_ERASE_REQ_SIG:
        case APP_LIST_REQ_SIG:
        case APP_RECV_REQ_SIG: {
            KLOG_W(TAG, "adopting the mailbox app; request ignored");
            erase_deny(e);
            status = Q_HANDLED();
            break;
        }
        case APP_CAT_REQ_SIG: {
            KLOG_W(TAG, "adopting the mailbox app; app_cat request denied");
            app_cat_deny(me);
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&AppMgr_busy);
            break;
        }
    }
    return status;
}

/**
 * @brief Close an install: verify or abort the write session, publish the
 *        outcome, and say what to do next.
 *
 * @param me      The AO.
 * @param ok      Every byte the host promised arrived and checksummed.
 * @param stalled When @p ok is false: the host went quiet (kcli error 6), not
 *                a refused or damaged transfer (error 5).
 * @return AppMgr_launching when the install succeeded and a boot was asked
 *         for, AppMgr_idle otherwise.
 */
static QState recv_close(AppMgr * const me, bool ok, bool stalled)
{
    /* Open means announced: the session opens in the same step that
     * publishes the ready and its 0 %, so only an install the glass has
     * already seen start owes it a failure. */
    bool const announced = me->writing;

    QTimeEvt_disarm(&me->idle);

    if ( me->writing )
    {
        if ( ok )
        {
            kode_status_t const st = xfer_end(me);
            me->writing = false;
            if ( KODE_OK != st )
            {
                ok = false;
                KLOG_E(TAG, "install failed: the written image did not verify");
            }
        }
        else
        {
            xfer_abort(me);
            me->writing = false;
        }
    }

    if ( ok && me->os_target )
    {
        /* No directory, no upsert: the slot holds exactly one image, and
         * the bootloader's own PENDING_VERIFY/confirm dance protects it.
         * The `>ok` goes out first and the switch waits for the drain, the
         * way an app's launch does - AO_Boot resets the board, and an answer
         * still in AO_Log's queue would die with it. */
        kdrv_slot_desc_t desc;
        if ( kdrv_slot_desc(&desc) == KODE_OK )
        {
            KLOG_N(TAG, "kodeOS slot now holds '%s' v%s (%s %s, IDF %s)",
                   desc.project, desc.version, desc.date, desc.time,
                   desc.idf_ver);
        }
        KLOG_N(TAG, "install dst=os complete (%u bytes); switching to the "
                    "new kodeOS", (unsigned)me->written);
        QACTIVE_PUBLISH(&s_recv_done_evt, &me->super);
        return Q_TRAN(&AppMgr_launching);
    }
    if ( ok && me->bypass )
    {
        /* Parked, not installed, as flash_finish() says for a medium: the
         * mailbox has no directory and holds a container, not an image. */
        KLOG_N(TAG, "parked %u bytes in the mailbox; `install src=\"pending\" "
                    "dst=...` installs it", (unsigned)me->written);
        me->bypass = false;
        QACTIVE_PUBLISH(&s_flash_done_evt.super, &me->super);   /* names no app */
        QACTIVE_PUBLISH(&s_recv_done_evt, &me->super);
        return Q_TRAN(&AppMgr_idle);
    }
    if ( ok )
    {
        char name[32];   /* esp_app_desc_t.project_name is 32 bytes */
        const char *const why = region_install(me, (uint32_t)me->written,
                                               name, sizeof(name));
        if ( NULL != why )
        {
            KLOG_E(TAG, "install failed: %s", why);
            block_discard();
            ok = false;
        }
        else
        {
            /* The done, the `>ok` and the launch wait for the folders. */
            tidy_begin(me, name, (uint8_t)TIDY_AFTER_RECV);
            return tidy_state(me);
        }
    }

    me->os_target = false;
    me->bypass    = false;
    keep_clear(me);     /* a failed install owes no folder to the apps it evicted */
    QACTIVE_PUBLISH(stalled ? &s_recv_stall_evt : &s_recv_fail_evt, &me->super);
    if ( announced )
    {
        QACTIVE_PUBLISH(&s_flash_fail_evt.super, &me->super);
    }
    return Q_TRAN(&AppMgr_idle);
}

/**
 * @brief Receiving an image the console streams in (`install src="terminal"`).
 *
 * RECV_DATA_SIG comes from the console reader task, one KODE_RECV_CHUNK at a
 * time. Each is CRC'd and written; after a failed write the rest is still
 * drained, because the host sends @c me->recv_size bytes whatever happens.
 */
static QState AppMgr_receiving(AppMgr * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            QTimeEvt_armX(&me->idle, APPMGR_RECV_LONGSTOP_TICKS, 0U);
            status = Q_HANDLED();
            break;
        }
        case CONSOLE_RAW_RSP_SIG: {
            ConsoleRawRspEvt const * const rsp = Q_EVT_CAST(ConsoleRawRspEvt);
            if ( (uint8_t)KODE_OK != rsp->status )
            {
                /* AO_Log refused to arm (raw mode was somehow already on).
                 * Nothing is open and nothing was streamed; just close. */
                status = recv_close(me, false, false);
                break;
            }

            /* The console is raw and the host waits for `>|ready`: now make
             * room in the region and open the write session, the same way the
             * mailbox path does. The slot and the mailbox need no room: each
             * holds one image. */
            if ( me->os_target || me->bypass )
            {
                const char *what = "mailbox";
                if ( me->os_target )
                {
                    me->sink = kdrv_slot_sink();
                    what     = "slot";
                }
                if ( xfer_begin(me, (uint8_t)KFW_TARGET_OS,
                               (uint32_t)me->total) != KODE_OK )
                {
                    KLOG_E(TAG, "install failed: cannot open the %s for "
                                "writing", what);
                    QACTIVE_POST(AO_Log, &s_raw_end_evt, &me->super);
                    status = recv_close(me, false, false);
                    break;
                }
                me->writing = true;
                (void)QTimeEvt_rearm(&me->idle, APPMGR_RECV_IDLE_TICKS);
                QACTIVE_PUBLISH(&s_recv_ready_evt, &me->super);
                progress_publish(me, 0U);
                status = Q_HANDLED();
                break;
            }
            kapps_plan_t plan;
            kapps_dir_t  dir = *kdrv_apps_dir();
            if ( kdrv_apps_plan(&dir, kdrv_apps_region_size(),
                               (uint32_t)me->total,
                               replacing_idx(&dir, me->recv_name), &plan)
                != KODE_OK )
            {
                KLOG_E(TAG, "install failed: %lu bytes will not fit even "
                            "after evicting everything",
                            (unsigned long)me->total);
                QACTIVE_POST(AO_Log, &s_raw_end_evt, &me->super);
                status = recv_close(me, false, false);
                break;
            }
            plan_evict(me, &dir, &plan);   /* the stream cannot wait: no copies */
            if ( (0U < plan.evict_count)
                && (kdrv_apps_dir_commit(&dir) != KODE_OK) )
            {
                KLOG_E(TAG, "install failed: cannot update the directory");
                QACTIVE_POST(AO_Log, &s_raw_end_evt, &me->super);
                status = recv_close(me, false, false);
                break;
            }
            me->sink            = kdrv_apps_sink();
            me->sink_ctx.offset = plan.offset;
            if ( xfer_begin(me, (uint8_t)KFW_TARGET_APP,
                           (uint32_t)me->total) != KODE_OK )
            {
                KLOG_E(TAG, "install failed: cannot open the region for writing");
                QACTIVE_POST(AO_Log, &s_raw_end_evt, &me->super);
                status = recv_close(me, false, false);
                break;
            }
            me->dst_offset = plan.offset;
            me->writing    = true;

            /* Tighten the watchdog from the long-stop insurance to the
             * real between-chunk deadline - only now, because armed at 5 s
             * from the start it would have been counting through the erases
             * above, and a big extent erases for longer than the whole
             * deadline. */
            (void)QTimeEvt_rearm(&me->idle, APPMGR_RECV_IDLE_TICKS);
            /* The host may start streaming. This is `>|ready`, and the 0 %
             * that raises the Opening sheet and the LED's pulse - the write
             * is announced with the wire, never before it. */
            QACTIVE_PUBLISH(&s_recv_ready_evt, &me->super);
            progress_publish(me, 0U);
            status = Q_HANDLED();
            break;
        }
        case RECV_DATA_SIG: {
            RecvDataEvt const * const d = Q_EVT_CAST(RecvDataEvt);
            /* Still hearing from the host: push the deadline out. rearm, not
             * armX - armX asserts on a timer that is already ticking
             * (Q_REQUIRE 450 in qf_time.c), and this one always is here. */
            (void)QTimeEvt_rearm(&me->idle, APPMGR_RECV_IDLE_TICKS);

            me->recv_crc = khal_crc32(me->recv_crc, d->data, d->len);

            /* Stage; write only whole chunks (see AppMgr.chunk). The write
             * grants the `>|ack`, so it falls while the host is silent. A
             * failed write stops the writing, never the draining or the acks:
             * the host still owes the full count. */
            uint16_t off = 0U;
            while ( off < d->len )
            {
                uint32_t const room = (uint32_t)KODE_RECV_CHUNK - me->recv_fill;
                uint32_t const left = (uint32_t)(d->len - off);
                uint32_t const take = (left < room) ? left : room;
                (void)memcpy(&me->chunk[me->recv_fill], &d->data[off], take);
                me->recv_fill += take;
                off           += (uint16_t)take;
                me->recv_wire += take;

                if ( ((uint32_t)KODE_RECV_CHUNK > me->recv_fill)
                    && (me->recv_wire < me->recv_size) )
                {
                    continue;              /* chunk still filling */
                }
                bool const whole = ((uint32_t)KODE_RECV_CHUNK == me->recv_fill);
                if ( me->recv_ok
                    && (xfer_feed(me, me->chunk, me->recv_fill) != KODE_OK) )
                {
                    me->recv_ok = false;
                    KLOG_E(TAG, "install: write failed at %u/%u bytes; "
                                "draining the rest of the transfer",
                           (unsigned)me->recv_wire, (unsigned)me->recv_size);
                }
                /* A failed write stops `written`, and the reports with it. */
                progress_report(me);
                me->recv_fill = 0U;
                if ( whole )
                {
                    /* Also for the final chunk when it lands exactly full:
                     * the host waits for this ack before it starts listening
                     * for the terminator. A final *partial* chunk earns none,
                     * and the host knows not to expect one. */
                    QACTIVE_PUBLISH(&s_recv_ack_evt, &me->super);
                }
            }

            if ( me->recv_wire < me->recv_size )
            {
                status = Q_HANDLED();
                break;
            }

            bool const crc_ok = (me->recv_crc == me->recv_want);
            if ( me->recv_ok && !crc_ok )
            {
                KLOG_E(TAG, "install failed: CRC mismatch (got 0x%08lx, "
                            "wanted 0x%08lx)",
                       (unsigned long)me->recv_crc, (unsigned long)me->recv_want);
            }
            status = recv_close(me, me->recv_ok && crc_ok, false);
            break;
        }
        case APP_RECV_IDLE_SIG: {
            KLOG_E(TAG, "install failed: no data for %u ms at %u/%u bytes; "
                        "the host is presumed gone",
                   (unsigned)KODE_RECV_IDLE_MS,
                   (unsigned)me->recv_wire, (unsigned)me->recv_size);
            /* The reader task may still be mid-transfer; tell AO_Log to
             * abandon raw mode rather than leave it waiting for bytes that
             * are never coming. A transfer that finished on the wire never
             * reaches here - the reader already disarmed itself. */
            QACTIVE_POST(AO_Log, &s_raw_end_evt, &me->super);
            status = recv_close(me, false, true);
            break;
        }
        /* Unlike AppMgr_opening/flashing/adopting, this state does not
         * enumerate the other fire-and-forget requests (FLASH_REQ_SIG and
         * friends already fall silently to Q_SUPER here, pre-existing and
         * out of scope for this fix) - but app_cat is the one request a
         * console command waits synchronously on, so it still must not be
         * dropped without an answer while an image is streaming in. */
        case APP_CAT_REQ_SIG: {
            KLOG_W(TAG, "busy receiving an installed image; "
                        "app_cat request denied");
            app_cat_deny(me);
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&AppMgr_busy);
            break;
        }
    }
    return status;
}

/**
 * @brief The pause between "installed" and "booting", after a terminal install
 *        that boots.
 *
 * Gives AO_Log's lower-priority task time to put `>ok` on the wire before
 * AO_Boot resets the board.
 */
static QState AppMgr_launching(AppMgr * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            QTimeEvt_armX(&me->answer, APPMGR_ANSWER_DRAIN_TICKS, 0U);
            status = Q_HANDLED();
            break;
        }
        case APP_RECV_LAUNCH_SIG: {
            if ( me->os_target )
            {
                /* `dst=os`: the launch is AO_Boot's switch. Back to
                 * idle first, exactly as flash_finish() does, so a refused
                 * switch leaves this AO ready rather than half-way. */
                me->os_target = false;
                boot_os_switch_request();   /* AO_Boot selects otadata and resets */
            }
            else
            {
                region_launch(me->file);   /* resets on success */
            }
            status = Q_TRAN(&AppMgr_idle); /* only reached if it could not */
            break;
        }
        default: {
            status = Q_SUPER(&AppMgr_busy);
            break;
        }
    }
    return status;
}
