/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file storage_ao.c
 * @brief AO_Storage: the Active Object that owns the two media.
 */
#include "sdkconfig.h"
#include "kaos.h"
#include "kaos_ask.h"
#include "kaos_start.h"

#include <string.h>

#include "kqp.h"
#include "kqp_worker.h"
#include "kaos_claim.h"
#include "kverbs.h"
#include "kapp_cat.h"
#include "kapp_data.h"
#include "kapp_desc.h"
#include "kbsp_board.h"
#include "kdrv_sd.h"
#include "kdrv_storage.h"
#include "khal_crc32.h"
#include "khal_system.h"
#include "khal_time.h"
#include "kode_log.h"
#include "krows.h"

#define TAG            "kaos.storage"

/* cppcheck-suppress misra-c2012-5.9 ; QP's module name for Q_ASSERT: one static of that name per file is QP's design */
Q_DEFINE_THIS_MODULE("storage_ao")

#define STORAGE_BUFS   2U
#define STORAGE_CHUNK  (32U * 1024U)

/* The app data folder's two path lengths, spelled out so the compiler can see
 * that neither snprintf below can truncate. The folder is
 * "/storage/.kode/appsdata/" (24) plus a project name (31) plus the terminator;
 * a path under it adds a separator and at most an `icon` value
 * (KAPP_DATA_ICON_MAX). The worst case is one byte past KODE_PATH_MAX, and the
 * driver refuses such a path anyway. */
#define APPDATA_FOLDER_MAX  56U
#define APPDATA_PATH_MAX    (APPDATA_FOLDER_MAX + KAPP_DATA_ICON_MAX)

#define APPDATA_JSON_NAME   "app.json"        /**< The folder's one required file. */
#define APPDATA_ICON_NAME   "icon.k64"        /**< The icon's default file name.   */
#define APPDATA_TOP_DIR     "/storage/.kode"  /**< The tree's first level, internal. */
#define APPDATA_TREE_DIR    APPDATA_TOP_DIR "/appsdata" /**< Holds one folder per app. */

#define WIPE_FILES_MAX      KODE_APP_LIST_MAX /**< Images the wipe keeps from one medium's root: as many as the launcher lists. */
#define WIPE_OS_IMAGE       "kodeos.bin"      /**< kodeOS's own image: never an app's file. */
#define WIPE_C5_IMAGE       "c5_slave.bin"    /**< The coprocessor's image: never an app's file. */
#define WIPE_FS_ROOT        "/storage/"       /**< Where the internal partition's images lie. */
#define WIPE_PATH_MAX       (sizeof(WIPE_FS_ROOT) + KODE_APP_FILE_MAX) /**< A root, a file name and the terminator. */

/** @brief What a wipe of one app's files holds in PSRAM while it runs. */
typedef struct {
    char    names[2U * WIPE_FILES_MAX][KODE_APP_FILE_MAX]; /**< The images to look at: the internal partition's, then the card's. */
    uint8_t prefix[KAPP_PREFIX_LEN];                       /**< The front of the image being looked at. */
} WipeScratch;

/* kqp.c cannot see these two (they live in kaos.h, beside the request that
 * carries them), so their fit in the medium pool, which is as wide as a
 * ConsoleLineEvt, is asserted here. */
_Static_assert(sizeof(AppDataPutEvt) <= sizeof(ConsoleLineEvt),
               "AppDataPutEvt must fit the medium pool");
_Static_assert(sizeof(AppDataPutRspEvt) <= sizeof(ConsoleLineEvt),
               "AppDataPutRspEvt must fit the medium pool");

/* A listing publishes at most one page plus its end marker at once, all
 * medium-tier events, so the pool must outlive that burst. */
_Static_assert((KODE_STORAGE_LIST_MAX + 1U) < KQP_MEDIUM_POOL_COUNT,
               "the medium pool must outlive a page of a listing");

/** @brief The worker's task name; `ps` prints it. */
#define STORAGE_SD_WORKER_NAME   "storage_sd"

/** @brief The worker's stack, in bytes, in PSRAM. A mount or a format never
 *         reaches the internal flash. An overflow corrupts a neighbour
 *         silently: 8 KB at least, whatever `ps` reads as free. */
#define STORAGE_SD_WORKER_STACK  8192U

/** @brief Jobs that may wait besides the one running: none is ever queued,
 *         because AO_Storage submits only while no call is out. */
#define STORAGE_SD_WORKER_DEPTH  1U

/** @brief The most the AO waits for a mount, a remount or an unmount: the
 *         driver's own bound and the AO's margin. */
#define STORAGE_JOB_MOUNT_MS   (KDRV_SD_MOUNT_MAX_MS + KODE_STORAGE_JOB_MARGIN_MS)
/** @brief The same for a transfer of raw sectors. */
#define STORAGE_JOB_SECTORS_MS (KDRV_SD_SECTORS_MAX_MS + KODE_STORAGE_JOB_MARGIN_MS)
/** @brief The same for a format. */
#define STORAGE_JOB_FORMAT_MS  (KDRV_SD_FORMAT_MAX_MS + KODE_STORAGE_JOB_MARGIN_MS)

/** @brief The calls the worker makes. */
typedef enum {
    SD_OP_MOUNT,      /**< Mount the volume.                                  */
    SD_OP_UNMOUNT,    /**< Unmount it.                                        */
    SD_OP_REMOUNT,    /**< Unmount, then mount: the next width, or a rescan.  */
    SD_OP_READ,       /**< Read raw sectors.                                  */
    SD_OP_WRITE,      /**< Write raw sectors.                                 */
    SD_OP_FORMAT      /**< Make a fresh filesystem and ask how big it is.     */
} SdOp;

/** @brief What a call was for: where the AO goes on when it returns. */
typedef enum {
    SD_THEN_MOUNT,         /**< A mount command: say where, answer.           */
    SD_THEN_UNMOUNT,       /**< An unmount command: answer.                   */
    SD_THEN_SCRIPT,        /**< The remount that starts a card script.        */
    SD_THEN_READ4,         /**< One chunk of the four-line read.              */
    SD_THEN_READ1_MOUNT,   /**< The remount onto one data line.               */
    SD_THEN_READ1,         /**< One chunk of the one-line read.               */
    SD_THEN_WRITE_MOUNT,   /**< The remount back onto the board's width.      */
    SD_THEN_VERDICT,       /**< The remount that hands the slot back.         */
    SD_THEN_PROBE_WRITE,   /**< One chunk written to the card's far end.      */
    SD_THEN_PROBE_READ,    /**< One chunk of the far end read back.           */
    SD_THEN_ALIAS,         /**< One sector of the wrap scan.                  */
    SD_THEN_FORMAT,        /**< The format.                                   */
    SD_THEN_NONE           /**< Nothing: the remount a script owed the slot.  */
} SdThen;

/**
 * @brief The one call to the card that may be out.
 *
 * From the submit to the done event the first group is the worker's: the AO
 * writes it before the submit and reads the answer after the event, and
 * touches none of it in between. The second group is the AO's alone.
 */
typedef struct {
    SdOp           op;      /**< What to call.                               */
    uint32_t       first;   /**< READ, WRITE: the first sector.              */
    uint32_t       count;   /**< READ, WRITE: how many.                      */
    uint8_t       *buf;     /**< READ, WRITE: the AO's PSRAM buffer.         */
    bool           sized;   /**< FORMAT: the new volume's size was read.     */

    bool           busy;    /**< A call is out; the guard.                   */
    bool           expired; /**< Its deadline passed and its requester has
                             *   been told; the guard stays until it returns. */
    uint16_t       seq;     /**< The call's sequence number.                 */
    SdThen         then;    /**< What it was for.                            */
    QActive       *who;     /**< A command's asker; NULL for none.           */
} SdJob;

typedef struct {
    QActive  super;
    KqpWorker worker;                   /**< storage_sd.                       */
    SdJob     sdj;                      /**< The call to the card, if one is out. */
    QTimeEvt  sdj_dl;                   /**< Its deadline.                     */
    uint8_t   sdj_stale;                /**< Expired deadlines still queued.   */
    bool      worker_up;                /**< The worker started.               */
    bool      width_owed;               /**< A script ended on a call that
                                         *   overran: the slot goes back onto
                                         *   the board's width when it returns. */
    QEQueue   deferq;                   /**< Requests held while a call is out. */
    QEvtPtr   defer_sto[KODE_STORAGE_DEFER_DEPTH - 1U]; /**< Its ring; the queue
                                         *   keeps the front event aside.     */
    QActive *reader;                    /**< Who has the file open, if anyone. */
    uint8_t *buf[STORAGE_BUFS];         /**< PSRAM, allocated once at startup. */
    bool     lent[STORAGE_BUFS];        /**< Out on loan; do not touch.        */

    /* The write in progress. The payload is somebody else's memory, held for
     * the duration by the promise storage_write_all_request() documents. */
    QActive       *writer;
    const uint8_t *w_data;
    uint32_t       w_len;
    uint32_t       w_done;
    uint8_t        w_medium;
    bool           w_retried;                  /**< One other medium, once.   */
    char           w_name[KODE_APP_FILE_MAX];  /**< Kept for the retry.       */
    QTimeEvt       w_pacer;             /**< 10 ms between chunks.             */

    /* The card test. One chunk per step, paced by its own timer for the same
     * reason the write above has one. */
    QTimeEvt t_pacer;
    uint8_t  t_phase;        /**< SD_TEST_* while running, 0 when idle.     */
    uint16_t t_step;         /**< Which chunk of this phase comes next.     */
    bool     t_ok;           /**< This phase, so far.                       */
    bool     t_verify;       /**< This run is `sd_verify`, not the row.     */
    bool     t_board_failed; /**< A phase that judges the SLOT has failed.  */
    bool     t_card_failed;  /**< A phase that only the card can veto has.  */
    uint32_t t_seed;         /**< This run's pattern, so a stale one from an
                              *   earlier run cannot pass.                  */
    uint32_t t_per_chunk;    /**< Sectors in one 32 KB chunk.               */
    uint32_t t_read_first;   /**< Where the raw reads start, mid-card.      */
    uint32_t t_tail;         /**< First sector of the last megabyte, which is
                              *   where a lied-about capacity shows.        */
    uint32_t t_alias;        /**< The real size the wrap scan looks at next,
                              *   in sectors; 0 once it has run out.        */
    uint32_t t_capacity_mb;
    uint32_t t_total_mb;     /**< The volume, for `sd_verify`.              */
    uint32_t t_read4_kb_s;   /**< Kept so one bit can be compared with four. */
    uint64_t t_busy_us;      /**< Time inside the transfers of this phase,
                              *   never the pacing between them.            */

    /* The `cat` in progress. The file is already closed: do_path_cat() reads it
     * once into a buffer, and these fields track the walk of that buffer across
     * dispatch steps. c_who is the tell: NULL means no cat is mid-walk. */
    QTimeEvt c_pacer;        /**< 10 ms between batches of lines.          */
    QActive *c_who;          /**< Where the lines and the terminator go.    */
    char     c_path[KODE_PATH_MAX]; /**< Kept for the log lines mid-walk.   */
    uint8_t  c_idx;          /**< Which buffer this cat is walking.         */
    size_t   c_got;          /**< Bytes the one read actually returned.     */
    size_t   c_pos;          /**< Where the next batch resumes.             */
    uint32_t c_size;         /**< The file's own size, for the truncation
                              *   log line.                                 */
    uint32_t c_want;         /**< Bytes actually asked of the read, clamped
                              *   to STORAGE_CHUNK - compared against c_got
                              *   to say whether `max` cut the file short.   */
    uint32_t c_max;          /**< What `max=` asked for, unclamped, for the
                              *   same log line.                            */

    /* The `cp` in progress: a reader and a writer open across dispatch steps, the
     * bytes going through one of the two buffers above, lent for the duration.
     * p_who is the tell: NULL means no copy is running. */
    QTimeEvt p_pacer;        /**< 10 ms between chunks.                     */
    QActive *p_who;          /**< Where the one answer goes.                */
    char     p_src[KODE_PATH_MAX]; /**< Kept for the log lines mid-copy.    */
    char     p_dst[KODE_PATH_MAX]; /**< Kept for the same reason.           */
    uint8_t  p_idx;          /**< Which buffer this copy is walking.        */
    uint32_t p_size;         /**< The source's size, for the log line.      */
    uint32_t p_done;         /**< Bytes carried across so far.              */

    /* The `file_put` in progress: a payload arriving one RECV_DATA_SIG at a time,
     * written straight into an open writer. Paced by the host: each chunk is
     * written and acked. r_who is the tell: NULL means nothing is arriving. */
    QTimeEvt r_idle;         /**< No chunk for this long: the host is gone.  */
    QActive *r_who;          /**< Where the whole RECV_* conversation goes.  */
    char     r_path[KODE_PATH_MAX]; /**< Kept for the log lines mid-transfer. */
    uint32_t r_total;        /**< What the host promised on the command line. */
    uint32_t r_wire;         /**< Bytes taken off the wire, and ONLY that:
                              *   a failed write must not hold back the
                              *   completion check, or it ends as a stall
                              *   instead of a write error.                  */
    uint32_t r_owed_ack;     /**< Bytes consumed since the last `>|ack`. One
                              *   ack per whole KODE_RECV_CHUNK; the trailing
                              *   remainder earns none, and the host knows
                              *   not to wait for one.                       */
    uint32_t r_crc;          /**< Running CRC over what has been accepted.   */
    uint32_t r_want;         /**< What that CRC must equal at the end.       */
    bool     r_open;         /**< The destination is open for writing.       */
    bool     r_ok;           /**< false once a write fails mid-stream; the
                              *   draining and the acks carry on, because
                              *   the host still owes the full count.        */

    /* The buffer one APPDATA_REQ_SIG stages an icon in, taken from PSRAM on the
     * first request and held for the life of the firmware, to keep 12 KB out of
     * internal RAM. One icon at a time: one request per dispatch step. */
    uint8_t *icon;           /**< KAPP_ICON_BYTES, NULL until first asked.   */
    bool     icon_warned;    /**< A refusal is logged once, not once per pill
                              *   of every launcher repaint.                 */
#if CONFIG_KODE_LOWPOWER
    uint8_t  lp_who;         /**< The id lowpower_join() gave.               */
#endif

    /* The `appdata_put` in progress, between its two steps (app.json, then the
     * icon): a copy of the request's fields, since QP recycles the event. g_buf
     * is this AO's from the post; appdata_put_finish() frees it. g_who is the
     * tell: NULL means no put is under way, and busy() reads it. */
    QActive       *g_who;          /**< Where the one answer goes.            */
    char           g_project[KDRV_SLOT_PROJECT_MAX]; /**< Whose folder.       */
    uint8_t       *g_buf;          /**< app.json then icon.k64; this AO's.    */
    uint16_t       g_json_len;     /**< Bytes of app.json at the head of g_buf. */
    uint16_t       g_icon_len;     /**< 0 when there is no icon step.         */

    /* The wipe of one app's files in progress: the images kept by the walk of
     * each root, read and deleted one a step, then the folders. x_who is the
     * tell: NULL means no wipe is under way, and busy() reads it. */
    QTimeEvt       x_pacer;        /**< 10 ms between files.                  */
    QActive       *x_who;          /**< Where the one answer goes.            */
    char           x_project[KODE_APP_PROJECT_MAX]; /**< Whose files.         */
    WipeScratch   *x;              /**< PSRAM, held for the wipe: the names
                                    *   and the descriptor being read.        */
    uint16_t       x_fs_count;     /**< Names from the internal partition: the
                                    *   first of x->names.                    */
    uint16_t       x_count;        /**< Names in all; the card's follow.      */
    uint16_t       x_next;         /**< The next name to look at.             */
    AppWipeMedium  x_fs;           /**< What went on the internal partition.  */
    AppWipeMedium  x_sd;           /**< What went on the card.                */
} Storage;

/* A timer, not a self-post, between chunks: this AO is priority 5 and AO_Gui,
 * AO_Radio and AO_Net are below it. A self-post keeps the queue non-empty and
 * starves them; going idle for 10 ms lets them run. */
#define STORAGE_WRITE_GAP_TICKS  KQP_TICKS_PER_MS(10U)

/* How many lines of a `cat` one dispatch step may post before the pacer
 * brings it back for the rest, so lower-priority AOs get the CPU. 24, like
 * KODE_STORAGE_LIST_MAX: under a console queue's depth (32), so one step can
 * never overflow the asker's queue on its own. */
#define STORAGE_CAT_LINES_PER_STEP  24U

/* The between-chunk deadline of a `file_put`, in QP ticks: no chunk for this
 * long and the host is presumed gone. The same KODE_RECV_IDLE_MS as
 * AO_AppMgr's install path. */
#define STORAGE_RECV_IDLE_TICKS  KQP_TICKS_PER_MS(KODE_RECV_IDLE_MS)

/* The long-stop, armed when a transfer is asked for and replaced by the
 * deadline above once the wire is raw. It covers AO_Log failing to allocate
 * its answer to the arming request, which would otherwise leave this AO
 * holding the console for good. Thirty seconds, as in AO_AppMgr. */
#define STORAGE_RECV_LONGSTOP_TICKS  KQP_TICKS_PER_MS(30000U)

static Storage l_storage;
QActive * const AO_Storage = &l_storage.super;

static QState Storage_initial(Storage * const me, void const * const par);
static QState Storage_active(Storage * const me, QEvt const * const e);

/* ---- Request helpers ----------------------------------------------------- */

/**
 * @brief Post a STORAGE_CMD_SIG to AO_Storage.
 *
 * @param medium      The medium it acts on.
 * @param op          What to do.
 * @param images_only LIST: only files that could be booted.
 * @param last        LIST: publish STORAGE_LIST_END_SIG afterwards.
 * @param who         Who is told the command has run; NULL for nobody.
 * @return true when it was posted; false when the event pool was empty.
 */
static bool storage_cmd(kdrv_medium_t medium, storage_op_t op,
                        bool images_only, bool last, QActive *who)
{
    StorageCmdEvt *e = Q_NEW_X(StorageCmdEvt, 0U, STORAGE_CMD_SIG);
    if ( NULL == e )
    {
        return false;
    }
    e->medium      = (uint8_t)medium;
    e->op          = (uint8_t)op;
    e->images_only = images_only;
    e->last        = last;
    e->who         = who;
    QACTIVE_POST(AO_Storage, &e->super, NULL);
    return true;
}

void storage_request(kdrv_medium_t medium, storage_op_t op)
{
    (void)storage_cmd(medium, op, false, false, NULL);
}

bool storage_request_answered(kdrv_medium_t medium, storage_op_t op,
                              QActive *who)
{
    return storage_cmd(medium, op, false, false, who);
}

void storage_field_request(QActive *who, uint8_t op, uint16_t field,
                           int32_t value)
{
    StorageFieldReqEvt *e = Q_NEW_X(StorageFieldReqEvt, 0U,
                                    STORAGE_FIELD_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; field request dropped");
        return;
    }
    e->who   = who;
    e->op    = op;
    e->field = field;
    e->value = value;
    QACTIVE_POST(AO_Storage, &e->super, NULL);
}

/* ---- The blocked task the request being answered right now belongs to -----
 * NULL for every request an Active Object made. One request is in flight at a
 * time, so file statics stand in for a parameter on every answer site; the
 * three handlers at the bottom set them and clear them as they return.
 * `cp` keeps its own copy across its steps (p_wait/p_gen); a task's `cat` is
 * answered whole inside do_path_cat().
 */
static StorageWaiter *s_waiter;
static uint32_t       s_waiter_gen;

/* ---- The asks: kode_sys's requests, answered to the asking AO --------------
 * The third kind of requester, beside an AO's `who` and a task's waiter: the
 * request carries its answer, taken with it, and the request in hand's is
 * named here the way s_waiter names a waiter's. The answer opens the asker's
 * door just before it is posted.
 */

/** @brief The kinds of ask, one door each per asker. */
enum {
    STORAGE_ASK_STAT = 0,  /**< storage_stat_ask()   */
    STORAGE_ASK_LIST,      /**< storage_list_ask()   */
    STORAGE_ASK_FOLDER,    /**< storage_mkdir_ask()  */
    STORAGE_ASK_DELETE,    /**< storage_delete_ask() */
    STORAGE_ASK_RENAME,    /**< storage_rename_ask() */
    STORAGE_ASK_READ,      /**< storage_read_ask()   */
    STORAGE_ASK_WRITE,     /**< storage_write_ask()  */
    STORAGE_ASK_SPACE,     /**< storage_space_ask()  */
    STORAGE_ASK_KINDS      /**< How many there are.  */
};

/** @brief The doors, one per kind of ask. */
static kaos_ask_door_t s_storage_doors[STORAGE_ASK_KINDS];

/** @brief The request in hand's ask answer, or NULL. */
static FileRspEvt *s_ask_rsp;
/** @brief Where it goes, for the whole step; NULL when no ask is in hand. */
static QActive    *s_ask_who;
/** @brief A read's lent buffer. */
static void       *s_ask_buf;

/**
 * @brief Name the request in hand's requester, whichever kind it is.
 *
 * An ask also fills the event's `who` with the asker: every answer must go
 * through the funnels that read s_ask_rsp first (post_path_rsp(),
 * post_cat_end()), and the `who` branches assert that no ask is in hand.
 *
 * @param wait A task's waiter, or NULL.
 * @param gen  Its tag.
 * @param who  The asking AO, read only with @p rsp.
 * @param rsp  An ask's answer, or NULL.
 * @param buf  A read's lent buffer, or NULL.
 */
static void reply_set(StorageWaiter *wait, uint32_t gen, QActive *who, FileRspEvt *rsp,
                      void *buf)
{
    s_waiter     = wait;
    s_waiter_gen = gen;
    s_ask_rsp    = rsp;
    s_ask_who    = NULL;
    s_ask_buf    = buf;
    if ( NULL != rsp )
    {
        s_ask_who = who;   /* kept until reply_clear(), after the answer too */
    }
}

/** @brief Forget the request in hand's requester. */
static void reply_clear(void)
{
    reply_set(NULL, 0U, NULL, NULL, NULL);
}

/**
 * @brief The door an answer belongs to, by its signal.
 * @param sig The answer's signal.
 * @return STORAGE_ASK_*.
 */
static uint8_t storage_ask_kind(QSignal sig)
{
    uint8_t kind = (uint8_t)STORAGE_ASK_STAT;

    if ( (QSignal)FILE_LIST_END_SIG == sig )
    {
        kind = (uint8_t)STORAGE_ASK_LIST;
    }
    else if ( (QSignal)FILE_FOLDER_RSP_SIG == sig )
    {
        kind = (uint8_t)STORAGE_ASK_FOLDER;
    }
    else if ( (QSignal)FILE_DELETE_RSP_SIG == sig )
    {
        kind = (uint8_t)STORAGE_ASK_DELETE;
    }
    else if ( (QSignal)FILE_RENAME_RSP_SIG == sig )
    {
        kind = (uint8_t)STORAGE_ASK_RENAME;
    }
    else if ( (QSignal)FILE_READ_RSP_SIG == sig )
    {
        kind = (uint8_t)STORAGE_ASK_READ;
    }
    else if ( (QSignal)FILE_WRITE_RSP_SIG == sig )
    {
        kind = (uint8_t)STORAGE_ASK_WRITE;
    }
    else if ( (QSignal)FILE_SPACE_RSP_SIG == sig )
    {
        kind = (uint8_t)STORAGE_ASK_SPACE;
    }
    else
    {
        /* FILE_STATUS_RSP_SIG */
    }
    return kind;
}

/**
 * @brief Answer the request in hand's ask, once: @p rc, and a status's @p st.
 * @param rc The outcome.
 * @param st What is at the path, read only by FILE_STATUS_RSP_SIG; NULL for none.
 */
static void file_ask_answer(kode_status_t rc, FileStat const *st)
{
    FileRspEvt * const a   = s_ask_rsp;
    QActive * const    who = s_ask_who;

    s_ask_rsp = NULL;
    a->rc = (uint8_t)rc;
    if ( (NULL != st) && ((QSignal)FILE_STATUS_RSP_SIG == a->super.sig) )
    {
        a->stat = *st;
    }
    kaos_ask_answer(&s_storage_doors[storage_ask_kind(a->super.sig)], who, &a->super,
                    AO_Storage);
}

/**
 * @brief Write one answer into a blocked task's own storage and wake it.
 *
 * The release store publishes everything written before it to a requester
 * that may read on the other core. The semaphore orders only its token, and a
 * requester may wake on another request's token, hence @c ack.
 *
 * @param wait   Where the answer goes. NULL is an Active Object's request and
 *               does nothing here.
 * @param gen    The answering request's own tag.
 * @param rc     kode_status_t for the round.
 * @param exists STAT only.
 * @param is_dir STAT only.
 * @param size   STAT only.
 */
static void wake_waiter(StorageWaiter *wait, uint32_t gen, kode_status_t rc,
                        bool exists, bool is_dir, uint32_t size)
{
    if ( NULL == wait )
    {
        return;
    }
    wait->rc     = (uint8_t)rc;
    wait->exists = exists;
    wait->is_dir = is_dir;
    wait->size   = size;
    __atomic_store_n(&wait->ack, gen, __ATOMIC_RELEASE);
    /* Not guarded against a NULL semaphore: kaos.h says a waiter names one,
     * and a loud fault is easier to find than a silent stall. */
    khal_sem_signal(wait->done);
}

/**
 * @brief Tell a command's asker the command has run.
 *
 * Whatever became of it - done, refused, dropped for the card test: the asker
 * is waiting on when, not on what.
 *
 * @param who The asker, or NULL for nobody.
 */
static void cmd_answer(QActive *who)
{
    static const QEvt rsp = QEVT_INITIALIZER(STORAGE_CMD_RSP_SIG);

    if ( NULL != who )
    {
        QACTIVE_POST(who, &rsp, &l_storage.super);
    }
}

void storage_path_request(QActive *who, uint8_t op, const char *path)
{
    StoragePathEvt *e = Q_NEW_X(StoragePathEvt, 0U, STORAGE_PATH_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; path request dropped");
        return;
    }
    e->op     = op;
    (void)snprintf(e->path, sizeof(e->path), "%s", path);
    e->who    = who;
    e->max    = 0U;
    e->offset = 0U;
    e->wait   = NULL;
    e->gen    = 0U;
    e->rsp    = NULL;
    e->buf    = NULL;
    QACTIVE_POST(AO_Storage, &e->super, NULL);
}

void storage_cat_request(QActive *who, const char *path, uint32_t max)
{
    StoragePathEvt *e = Q_NEW_X(StoragePathEvt, 0U, STORAGE_PATH_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; cat request dropped");
        return;
    }
    e->op     = (uint8_t)STORAGE_PATH_CAT;
    (void)snprintf(e->path, sizeof(e->path), "%s", path);
    e->who    = who;
    e->max    = max;
    e->offset = 0U;
    e->wait   = NULL;
    e->gen    = 0U;
    e->rsp    = NULL;
    e->buf    = NULL;
    QACTIVE_POST(AO_Storage, &e->super, NULL);
}

/**
 * @brief Build and post one StoragePathEvt on behalf of a blocked task.
 *
 * Shared by the two public wait variants, which differ in one field.
 *
 * @param wait Where the answer goes. Not NULL.
 * @param gen  This request's tag.
 * @param op   storage_path_op_t.
 * @param path Absolute, normalised, virtual.
 * @param offset CAT only; ignored otherwise.
 * @param max  CAT only; ignored otherwise.
 */
static void path_request_wait(StorageWaiter *wait, uint32_t gen, uint8_t op,
                              const char *path, uint32_t offset, uint32_t max)
{
    StoragePathEvt *e = Q_NEW_X(StoragePathEvt, 0U, STORAGE_PATH_REQ_SIG);
    if ( NULL == e )
    {
        /* Answered rather than dropped: the requester is asleep with a
         * deadline and a silence here costs it the whole of that deadline
         * for a failure already known. ir_capture_request() does the same. */
        KLOG_E(TAG, "out of events; path request dropped");
        wake_waiter(wait, gen, KODE_NO_RESOURCES, false, false, 0U);
        return;
    }
    e->op     = op;
    (void)snprintf(e->path, sizeof(e->path), "%s", path);
    e->who    = NULL;
    e->max    = max;
    e->offset = offset;
    e->wait   = wait;
    e->gen    = gen;
    e->rsp    = NULL;
    e->buf    = NULL;
    QACTIVE_POST(AO_Storage, &e->super, NULL);
}

void storage_path_request_wait(StorageWaiter *wait, uint32_t gen, uint8_t op,
                               const char *path)
{
    path_request_wait(wait, gen, op, path, 0U, 0U);
}

void storage_cat_request_wait(StorageWaiter *wait, uint32_t gen,
                              const char *path, uint32_t offset, uint32_t max)
{
    path_request_wait(wait, gen, (uint8_t)STORAGE_PATH_CAT, path, offset, max);
}

void storage_pair_request(QActive *who, uint8_t op, const char *src,
                          const char *dst)
{
    StoragePairEvt *e = Q_NEW_X(StoragePairEvt, 0U, STORAGE_PAIR_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; pair request dropped");
        return;
    }
    e->op   = op;
    (void)snprintf(e->src, sizeof(e->src), "%s", src);
    (void)snprintf(e->dst, sizeof(e->dst), "%s", dst);
    e->who  = who;
    e->wait = NULL;
    e->gen  = 0U;
    e->rsp  = NULL;
    QACTIVE_POST(AO_Storage, &e->super, NULL);
}

void storage_pair_request_wait(StorageWaiter *wait, uint32_t gen, uint8_t op,
                               const char *src, const char *dst)
{
    StoragePairEvt *e = Q_NEW_X(StoragePairEvt, 0U, STORAGE_PAIR_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; pair request dropped");
        wake_waiter(wait, gen, KODE_NO_RESOURCES, false, false, 0U);
        return;
    }
    e->op   = op;
    (void)snprintf(e->src, sizeof(e->src), "%s", src);
    (void)snprintf(e->dst, sizeof(e->dst), "%s", dst);
    e->who  = NULL;
    e->wait = wait;
    e->gen  = gen;
    e->rsp  = NULL;
    QACTIVE_POST(AO_Storage, &e->super, NULL);
}

void storage_text_request(QActive *who, const char *path, const char *text,
                          bool append, uint16_t line)
{
    StorageTextEvt *e = Q_NEW_X(StorageTextEvt, 0U, STORAGE_TEXT_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; write request dropped");
        return;
    }
    e->append = append;
    e->line   = line;
    (void)snprintf(e->path, sizeof(e->path), "%s", path);

    /* snprintf(), not memcpy(): @p text is a C string, cut here to
     * KODE_FS_TEXT_MAX. The clipped return value is the only length
     * AO_Storage may trust afterwards. */
    int const wrote = snprintf(e->text, sizeof(e->text), "%s", text);
    if ( 0 > wrote )
    {
        e->len = 0U;
    }
    else if ( (size_t)wrote >= sizeof(e->text) )
    {
        e->len = (uint16_t)(sizeof(e->text) - 1U);
    }
    else
    {
        e->len = (uint16_t)wrote;
    }

    e->who  = who;
    e->data = NULL;
    e->wait = NULL;
    e->gen  = 0U;
    e->rsp  = NULL;
    QACTIVE_POST(AO_Storage, &e->super, NULL);
}

void storage_text_request_wait(StorageWaiter *wait, uint32_t gen,
                               const char *path, const void *data,
                               uint32_t len, bool append)
{
    if ( (uint32_t)UINT16_MAX < len )
    {
        /* StorageTextEvt counts its bytes in a uint16_t: refused whole
         * rather than truncated, which would write a fraction and answer
         * KODE_OK. */
        KLOG_E(TAG, "write of %lu bytes is past what one request carries",
               (unsigned long)len);
        wake_waiter(wait, gen, KODE_NO_RESOURCES, false, false, 0U);
        return;
    }

    StorageTextEvt *e = Q_NEW_X(StorageTextEvt, 0U, STORAGE_TEXT_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; write request dropped");
        wake_waiter(wait, gen, KODE_NO_RESOURCES, false, false, 0U);
        return;
    }
    e->append = append;
    e->line   = 0U;
    (void)snprintf(e->path, sizeof(e->path), "%s", path);

    /* Nothing is copied into `text`: the bytes travel by pointer (see
     * StorageTextEvt.data for what its owner owes). */
    e->text[0] = '\0';
    e->len     = (uint16_t)len;
    e->data    = data;
    e->who     = NULL;
    e->wait    = wait;
    e->gen     = gen;
    e->rsp     = NULL;
    QACTIVE_POST(AO_Storage, &e->super, NULL);
}

/**
 * @brief Take an ask's door and its answer from the pool.
 * @param who  The asking AO.
 * @param kind STORAGE_ASK_*.
 * @param sig  The answer's signal.
 * @param out  The answer, cleared; NULL unless KODE_OK.
 * @return KODE_OK; as kaos_ask_take(), or KODE_NO_RESOURCES for a dry pool.
 */
static kode_status_t file_ask_open(QActive const *who, uint8_t kind, enum_t sig,
                                   FileRspEvt **out)
{
    kode_status_t const rc = kaos_ask_take(&s_storage_doors[kind], who);
    FileRspEvt         *a;

    *out = NULL;
    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(FileRspEvt, 0U, sig);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_storage_doors[kind], who, NULL);
    }
    a->rc          = (uint8_t)KODE_OK;
    a->more        = false;
    a->count       = 0U;
    a->got         = 0U;
    a->stat.exists = false;
    a->stat.is_dir = false;
    a->stat.size   = 0U;
    /* After the stat, so every byte the two views share is zero. */
    a->space.total_kb = 0U;
    a->space.free_kb  = 0U;
    *out = a;
    return KODE_OK;
}

/**
 * @brief Post one path-shaped ask.
 * @param who    The asking AO.
 * @param kind   STORAGE_ASK_*.
 * @param sig    The answer's signal.
 * @param op     storage_path_op_t.
 * @param path   Absolute, virtual; copied.
 * @param offset A read's first byte, or a listing's entries to skip.
 * @param buf    A read's lent buffer, or NULL.
 * @param max    A read's room.
 * @return As the asks in kaos.h.
 */
static kode_status_t path_ask(QActive *who, uint8_t kind, enum_t sig, uint8_t op,
                              const char *path, uint32_t offset, void *buf, uint32_t max)
{
    FileRspEvt     *a;
    StoragePathEvt *e;
    kode_status_t   rc = file_ask_open(who, kind, sig, &a);

    if ( KODE_OK != rc )
    {
        return rc;
    }
    e = Q_NEW_X(StoragePathEvt, 0U, STORAGE_PATH_REQ_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_storage_doors[kind], who, &a->super);
    }
    e->op     = op;
    (void)snprintf(e->path, sizeof(e->path), "%s", path);
    e->who    = who;
    e->max    = max;
    e->offset = offset;
    e->wait   = NULL;
    e->gen    = 0U;
    e->rsp    = a;
    e->buf    = buf;
    return kaos_ask_post(&s_storage_doors[kind], who, AO_Storage, &e->super, &a->super,
                         KQP_STORAGE_CMD_MARGIN);
}

kode_status_t storage_stat_ask(QActive *who, const char *path)
{
    return path_ask(who, (uint8_t)STORAGE_ASK_STAT, FILE_STATUS_RSP_SIG,
                    (uint8_t)STORAGE_PATH_STAT, path, 0U, NULL, 0U);
}

kode_status_t storage_list_ask(QActive *who, const char *path, uint16_t skip)
{
    return path_ask(who, (uint8_t)STORAGE_ASK_LIST, FILE_LIST_END_SIG,
                    (uint8_t)STORAGE_PATH_LIST, path, (uint32_t)skip, NULL, 0U);
}

kode_status_t storage_mkdir_ask(QActive *who, const char *path)
{
    return path_ask(who, (uint8_t)STORAGE_ASK_FOLDER, FILE_FOLDER_RSP_SIG,
                    (uint8_t)STORAGE_PATH_MKDIR, path, 0U, NULL, 0U);
}

kode_status_t storage_delete_ask(QActive *who, const char *path)
{
    return path_ask(who, (uint8_t)STORAGE_ASK_DELETE, FILE_DELETE_RSP_SIG,
                    (uint8_t)STORAGE_PATH_DELETE, path, 0U, NULL, 0U);
}

kode_status_t storage_read_ask(QActive *who, const char *path, uint32_t offset, void *buf,
                               uint32_t cap)
{
    return path_ask(who, (uint8_t)STORAGE_ASK_READ, FILE_READ_RSP_SIG,
                    (uint8_t)STORAGE_PATH_CAT, path, offset, buf, cap);
}

kode_status_t storage_rename_ask(QActive *who, const char *from, const char *to)
{
    FileRspEvt     *a;
    StoragePairEvt *e;
    kode_status_t   rc = file_ask_open(who, (uint8_t)STORAGE_ASK_RENAME, FILE_RENAME_RSP_SIG, &a);

    if ( KODE_OK != rc )
    {
        return rc;
    }
    e = Q_NEW_X(StoragePairEvt, 0U, STORAGE_PAIR_REQ_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_storage_doors[STORAGE_ASK_RENAME], who, &a->super);
    }
    e->op   = (uint8_t)STORAGE_PAIR_MOVE;
    (void)snprintf(e->src, sizeof(e->src), "%s", from);
    (void)snprintf(e->dst, sizeof(e->dst), "%s", to);
    e->who  = who;
    e->wait = NULL;
    e->gen  = 0U;
    e->rsp  = a;
    return kaos_ask_post(&s_storage_doors[STORAGE_ASK_RENAME], who, AO_Storage, &e->super,
                         &a->super, KQP_STORAGE_CMD_MARGIN);
}

kode_status_t storage_space_ask(QActive *who, const char *path)
{
    return path_ask(who, (uint8_t)STORAGE_ASK_SPACE, FILE_SPACE_RSP_SIG,
                    (uint8_t)STORAGE_PATH_SPACE, path, 0U, NULL, 0U);
}

kode_status_t storage_write_ask(QActive *who, const char *path, const void *data, uint32_t len,
                                bool append)
{
    FileRspEvt     *a;
    StorageTextEvt *e;
    kode_status_t   rc;

    if ( (uint32_t)UINT16_MAX < len )
    {
        return KODE_DENIED;   /* StorageTextEvt counts its bytes in a uint16_t */
    }
    rc = file_ask_open(who, (uint8_t)STORAGE_ASK_WRITE, FILE_WRITE_RSP_SIG, &a);
    if ( KODE_OK != rc )
    {
        return rc;
    }
    e = Q_NEW_X(StorageTextEvt, 0U, STORAGE_TEXT_REQ_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_storage_doors[STORAGE_ASK_WRITE], who, &a->super);
    }
    e->append  = append;
    e->line    = 0U;
    (void)snprintf(e->path, sizeof(e->path), "%s", path);
    e->text[0] = '\0';
    e->len     = (uint16_t)len;
    e->data    = data;
    e->who     = who;
    e->wait    = NULL;
    e->gen     = 0U;
    e->rsp     = a;
    return kaos_ask_post(&s_storage_doors[STORAGE_ASK_WRITE], who, AO_Storage, &e->super,
                         &a->super, KQP_STORAGE_CMD_MARGIN);
}

bool storage_recv_request(QActive *who, const char *path, uint32_t size,
                          uint32_t crc32, uint8_t src)
{
    StorageRecvEvt *e = Q_NEW_X(StorageRecvEvt, 0U, STORAGE_RECV_REQ_SIG);
    if ( NULL == e )
    {
        /* Reported false, unlike the requests above: a console command
         * waits on this, and its watchdog would blame the host. */
        KLOG_E(TAG, "out of events; file_put request dropped");
        return false;
    }
    e->size  = size;
    e->crc32 = crc32;
    (void)snprintf(e->path, sizeof(e->path), "%s", path);
    e->who   = who;
    e->src   = src;
    QACTIVE_POST(AO_Storage, &e->super, NULL);
    return true;
}

void storage_list_one(kdrv_medium_t medium, bool images_only, bool last)
{
    (void)storage_cmd(medium, STORAGE_OP_LIST, images_only, last, NULL);
}

void storage_list_request(QActive *who)
{
    /* Remount the card before looking at it: FATFS answers from what it read
     * at mount, so a swapped or PC-written card would list stale. A mount, not
     * the probe, which gives up after one failure per boot. Posted, so these
     * run before the page asked below; nothing has a file open here. The
     * internal partition cannot change behind our back and is left alone. */
    static bool s_listed;   /* one caller, AO_Gui's step */

    /* The first listing of a boot is not a rescan: the mount of a moment ago
     * is as honest as another, and 50 ms sooner. */
    if ( s_listed )
    {
        storage_request(KDRV_MEDIUM_SD, STORAGE_OP_UNMOUNT);
    }
    s_listed = true;
    storage_request(KDRV_MEDIUM_SD, STORAGE_OP_MOUNT);

    storage_list_page(0U, who);
}

void storage_list_page(uint16_t skip, QActive *who)
{
    StorageListPageEvt *e =
        Q_NEW_X(StorageListPageEvt, 0U, STORAGE_LIST_PAGE_SIG);
    if ( NULL == e )
    {
        /* An empty pool: the asker's own round window closes the question. */
        return;
    }
    e->skip = skip;
    e->who  = who;
    QACTIVE_POST(AO_Storage, &e->super, NULL);
}

void storage_open_request(const char *name, QActive *who)
{
    StorageOpenEvt *e = Q_NEW_X(StorageOpenEvt, 0U, STORAGE_OPEN_REQ_SIG);
    if ( NULL == e )
    {
        return;
    }
    (void)snprintf(e->name, sizeof(e->name), "%s", name);
    e->who = who;
    QACTIVE_POST(AO_Storage, &e->super, NULL);
}

void storage_desc_request(const char *name, QActive *who)
{
    StorageDescReqEvt *e = Q_NEW_X(StorageDescReqEvt, 0U, STORAGE_DESC_REQ_SIG);
    if ( NULL != e )
    {
        (void)snprintf(e->name, sizeof(e->name), "%s", name);
        e->who = who;
        QACTIVE_POST(AO_Storage, &e->super, (void *)0);
    }
}

/**
 * @brief Answer a folder question that cannot be answered for real: busy,
 *        nothing read, then the end marker.
 *
 * Every folder question gets its fields and its end: the launcher starts the
 * next folder on the end marker. Static events, because this is what is said
 * when the pool is dry; the project is left empty.
 *
 * @param who Where the answer goes; NULL asks for none.
 */
static void appdata_answer_busy(QActive *who)
{
    static const AppDataFieldsEvt fields = {
        .super       = QEVT_INITIALIZER(APPDATA_FIELDS_SIG),
        .medium      = (uint8_t)APPDATA_MEDIUM_NONE,
        .json_status = (uint8_t)KAPP_JSON_NONE,
        .icon_status = (uint8_t)APPDATA_ICON_BUSY,
    };
    static const QEvt end = QEVT_INITIALIZER(APPDATA_END_SIG);

    if ( NULL != who )
    {
        QACTIVE_POST(who, &fields.super, (void *)0);
        QACTIVE_POST(who, &end, (void *)0);
    }
}

void appdata_request(const char *project, QActive *who)
{
    AppDataReqEvt *e = Q_NEW_X(AppDataReqEvt, 0U, APPDATA_REQ_SIG);
    if ( NULL != e )
    {
        (void)snprintf(e->project, sizeof(e->project), "%s", project);
        e->who = who;
        QACTIVE_POST(AO_Storage, &e->super, (void *)0);
    }
    else
    {
        KLOG_E(TAG, "out of events; the folder of '%s' is answered busy",
               project);
        appdata_answer_busy(who);
    }
}

kode_status_t appdata_rm_request(const char *project, QActive *who)
{
    AppDataReqEvt *e = Q_NEW_X(AppDataReqEvt, 0U, APPDATA_RM_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; the data folder of '%s' is not deleted",
               project);
        return KODE_NO_RESOURCES;
    }
    (void)snprintf(e->project, sizeof(e->project), "%s", project);
    e->who = who;
    QACTIVE_POST(AO_Storage, &e->super, (void *)0);
    return KODE_OK;
}

kode_status_t storage_app_wipe_request(const char *project, QActive *who)
{
    AppDataReqEvt *e = Q_NEW_X(AppDataReqEvt, 0U, STORAGE_APP_WIPE_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; the files of '%s' are not looked at",
               project);
        return KODE_NO_RESOURCES;
    }
    (void)snprintf(e->project, sizeof(e->project), "%s", project);
    e->who = who;
    /* A card put in since the last listing is mounted before it is looked
     * at: posted first, so the wipe waits behind the call to the card. */
    storage_request(KDRV_MEDIUM_SD, STORAGE_OP_MOUNT);
    QACTIVE_POST(AO_Storage, &e->super, (void *)0);
    return KODE_OK;
}

kode_status_t appdata_put_request(const char *project, uint8_t *buf,
                                  uint16_t json_len, uint16_t icon_len,
                                  QActive *who)
{
    AppDataPutEvt *e = Q_NEW_X(AppDataPutEvt, 0U, APPDATA_PUT_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; the data folder of '%s' goes unwritten",
               project);
        return KODE_NO_RESOURCES;
    }
    (void)snprintf(e->project, sizeof(e->project), "%s", project);
    e->buf      = buf;
    e->json_len = json_len;
    e->icon_len = icon_len;
    e->who      = who;
    QACTIVE_POST(AO_Storage, &e->super, (void *)0);
    return KODE_OK;
}

bool appdata_folder_path(char *out, size_t cap, AppDataMedium m,
                         const char *project)
{
    static const char *const ROOT[] = { "", "/storage", "/sdcard" };

    if ( ('\0' == project[0]) || (strlen(project) > 31U) )
    {
        return false;
    }
    for ( const char *q = project; '\0' != *q; ++q )
    {
        bool const ok = (('A' <= *q) && ('Z' >= *q)) || (('a' <= *q) && ('z' >= *q))
                     || (('0' <= *q) && ('9' >= *q)) || ('_' == *q) || ('-' == *q)
                     || ('.' == *q);
        if ( !ok )
        {
            return false;   /* FAT cannot hold it, so no folder can exist */
        }
    }
    return snprintf(out, cap, "%s/.kode/appsdata/%s", ROOT[m], project) < (int)cap;
}

void storage_read_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(STORAGE_READ_REQ_SIG);
    QACTIVE_POST(AO_Storage, &evt, NULL);
}

void storage_next_request(uint8_t idx)
{
    StorageBufEvt *e = Q_NEW_X(StorageBufEvt, 0U, STORAGE_NEXT_SIG);
    if ( NULL != e )
    {
        e->idx = idx;
        QACTIVE_POST(AO_Storage, &e->super, NULL);
    }
}

void storage_close_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(STORAGE_CLOSE_REQ_SIG);
    QACTIVE_POST(AO_Storage, &evt, NULL);
}

const uint8_t *storage_buf(uint8_t idx)
{
    return (STORAGE_BUFS > idx) ? l_storage.buf[idx] : NULL;
}

void storage_write_all_request(const char *name, const void *data,
                               uint32_t len, QActive *who)
{
    StorageWriteEvt *e = Q_NEW_X(StorageWriteEvt, 0U, STORAGE_WRITE_REQ_SIG);
    if ( NULL == e )
    {
        return;
    }
    (void)snprintf(e->name, sizeof(e->name), "%s", name);
    e->data = data;
    e->len  = len;
    e->who  = who;
    QACTIVE_POST(AO_Storage, &e->super, NULL);
}

/* ---- Listing ------------------------------------------------------------- */

/**
 * @brief Send one event of a listing: to its asker, or to every subscriber.
 *
 * A page of bootable images goes to the AO that asked, so it sets the pace.
 * The one-medium and path listings (`sd_ls`, `fs_ls`, `ls`) are published.
 *
 * @param who The asker, or NULL to publish.
 * @param e   The event.
 */
static void list_send(QActive *who, QEvt const *e)
{
    if ( NULL != who )
    {
        QACTIVE_POST(who, e, AO_Storage);
    }
    else
    {
        QACTIVE_PUBLISH(e, AO_Storage);
    }
}

/**
 * @brief Send the end of a page of a listing.
 *
 * Two static events rather than one taken from a pool: the end of a page is
 * what its asker is waiting on, and an empty pool must not be able to lose it.
 *
 * @param who  The asker, or NULL to publish.
 * @param more Another page follows this one.
 */
static void list_end(QActive *who, bool more)
{
    static const ListEndEvt end_more = {
        .super = QEVT_INITIALIZER(STORAGE_LIST_END_SIG), .more = true };
    static const ListEndEvt end_last = {
        .super = QEVT_INITIALIZER(STORAGE_LIST_END_SIG), .more = false };

    if ( more )
    {
        list_send(who, &end_more.super);
    }
    else
    {
        list_send(who, &end_last.super);
    }
}

typedef struct {
    kdrv_medium_t medium;
    bool          images_only;
    QActive      *who;         /**< The asker, or NULL to publish.      */
    uint32_t      base;        /**< Position of the page's first entry. */
    uint32_t      skip;        /**< Entries still to leave out: what
                                 *   earlier pages sent.                 */
    uint32_t      published;
    bool          truncated;   /**< Ran out of events part-way through. */
    bool          capped;      /**< Hit KODE_STORAGE_LIST_MAX.           */
} ListCtx;

/**
 * @brief Whether two names are the same to FAT, which keeps no case.
 * @param a One name.
 * @param b The other.
 * @return true when they differ in case at most.
 */
static bool name_is(const char *a, const char *b)
{
    return strcasecmp(a, b) == 0;
}

/** Anything not ending in .bin cannot possibly be booted. */
static bool is_app_image(const char *name)
{
    const char *dot = strrchr(name, '.');
    return (NULL != dot) && name_is(dot, ".bin");
}

/** Publish one entry. Runs inside kdrv_storage_list, on this AO's thread. */
static bool publish_entry(const char *name, uint32_t size, void *ctx)
{
    ListCtx *c = (ListCtx *)ctx;

    if ( c->images_only && !is_app_image(name) )
    {
        return true;
    }

    /* Skip rather than truncate: a name that does not fit the event would come
     * back as a request nobody could resolve to a file. */
    size_t const len = strlen(name);
    if ( KODE_APP_FILE_MAX <= len )
    {
        KLOG_W(TAG, "ignoring '%s': name longer than %u chars",
               name, (unsigned)(KODE_APP_FILE_MAX - 1U));
        return true;
    }

    /* Counted after both filters, so a skip lands on the file it landed on
     * when the earlier page was walked. */
    if ( 0U < c->skip )
    {
        --c->skip;
        return true;
    }

    if ( KODE_STORAGE_LIST_MAX <= c->published )
    {
        c->capped = true;
        return false;
    }

    StorageEntryEvt *ev = Q_NEW_X(StorageEntryEvt, 0U, STORAGE_ENTRY_SIG);
    if ( NULL == ev )
    {
        /* Out of pool blocks. End the walk rather than assert - but say so,
         * because a listing that quietly stops short is a listing that lies.
         * The usual cause is AO_Gui: it subscribes to these and is their
         * lowest-priority subscriber, so it can be holding a whole
         * listing's worth of events while everything above it runs. */
        c->truncated = true;
        return false;
    }
    (void)memcpy(ev->name, name, len + 1U);
    ev->size    = size;
    ev->from_sd = (KDRV_MEDIUM_SD == c->medium);
    ev->pos     = (uint16_t)(c->base + c->published);
    /* kdrv_storage_entry_cb above carries no is_dir argument - this walk
     * genuinely does not know, and false is said outright rather than left
     * to whatever the pool block last held. */
    ev->is_dir  = false;
    list_send(c->who, &ev->super);
    ++c->published;
    return true;
}

/** @brief Walk one medium, publishing an entry per file, and say so when the listing was cut. */
static void do_list(kdrv_medium_t medium, bool images_only)
{
    ListCtx ctx = { .medium = medium, .images_only = images_only,
                    .who = NULL, .base = 0U, .skip = 0U, .published = 0U,
                    .truncated = false, .capped = false };
    (void)kdrv_storage_list(medium, &publish_entry, &ctx);
    if ( ctx.capped )
    {
        KLOG_W(TAG, "listing stopped at %u entries (the ceiling)",
               (unsigned)KODE_STORAGE_LIST_MAX);
    }
    else if ( ctx.truncated )
    {
        KLOG_W(TAG, "listing cut short: no event blocks left");
    }
}

/**
 * @brief Publish one page of the bootable images on every medium.
 *
 * The card, then the internal partition, as one listing: `published` carries
 * on from one medium to the next. A page cut by an empty pool also says more
 * follows, so the asker asks again from what it holds.
 *
 * @param skip Images to leave out: the ones earlier pages sent.
 * @param who  The asker; the page is posted to it alone.
 */
static void do_list_apps(uint32_t skip, QActive *who)
{
    static const kdrv_medium_t order[] = { KDRV_MEDIUM_SD, KDRV_MEDIUM_FS };
    ListCtx ctx = { .medium = KDRV_MEDIUM_SD, .images_only = true,
                    .who = who, .base = skip, .skip = skip, .published = 0U,
                    .truncated = false, .capped = false };

    for ( size_t i = 0U; (sizeof(order) / sizeof(order[0])) > i; ++i )
    {
        ctx.medium = order[i];
        (void)kdrv_storage_list(order[i], &publish_entry, &ctx);
        if ( ctx.capped || ctx.truncated )
        {
            break;
        }
    }
    if ( ctx.truncated )
    {
        KLOG_W(TAG, "listing cut short: no event blocks left");
    }
    else if ( ctx.capped )
    {
        KLOG_D(TAG, "page from %u: %u entries, more",
               (unsigned)skip, (unsigned)ctx.published);
    }
    else
    {
        /* The last page: nothing to say. */
    }
    list_end(who, ctx.capped || ctx.truncated);
}

/**
 * @brief Which medium a STORAGE_ENTRY_SIG born of a path walk should claim.
 *
 * kdrv_storage_medium_of() answers for the walked path. "/" names no volume,
 * so its children ("sdcard", "storage") are asked about instead.
 */
static bool path_entry_from_sd(const char *dir_path, const char *name)
{
    if ( strcmp(dir_path, "/") != 0 )
    {
        return (kdrv_storage_medium_of(dir_path) == KDRV_MEDIUM_SD);
    }

    char root_child[KODE_PATH_MAX];
    (void)snprintf(root_child, sizeof(root_child), "/%s", name);
    return (kdrv_storage_medium_of(root_child) == KDRV_MEDIUM_SD);
}

typedef struct {
    const char *dir_path;      /**< What was asked for; feeds from_sd above. */
    uint32_t    published;
    bool        truncated;     /**< Ran out of events part-way through.     */
    bool        capped;        /**< Hit KODE_STORAGE_LIST_MAX, or the
                                 *   requester's own array filled up.       */
    StorageWaiter *wait;       /**< A blocked task's rows, or NULL to
                                 *   publish the walk as events.            */
    QActive    *ask;           /**< An asking AO the page is posted to, or
                                 *   NULL.                                  */
    uint32_t    skip;          /**< An ask's entries still to leave out.    */
    uint32_t    base;          /**< An ask's first entry's position.        */
} PathListCtx;

/**
 * @brief Post one entry of an ask's page, leaving KODE_STORAGE_LIST_FREE_SLOTS
 *        free in the asker's queue; an entry that does not fit ends the page.
 * @param c      The walk.
 * @param name   The entry's name, shorter than KODE_APP_FILE_MAX.
 * @param len    Its length.
 * @param is_dir A directory.
 * @param size   Its bytes.
 * @return false to end the walk: the page is full, or the pool or the asker's queue is.
 */
static bool ask_path_entry(PathListCtx *c, const char *name, size_t len, bool is_dir,
                           uint32_t size)
{
    StorageEntryEvt *ev;

    if ( 0U < c->skip )
    {
        --c->skip;
        return true;
    }
    if ( KODE_STORAGE_LIST_MAX <= c->published )
    {
        c->capped = true;
        return false;
    }
    ev = Q_NEW_X(StorageEntryEvt, 0U, STORAGE_ENTRY_SIG);
    if ( NULL == ev )
    {
        c->truncated = true;
        return false;
    }
    (void)memcpy(ev->name, name, len + 1U);
    ev->size    = size;
    ev->from_sd = path_entry_from_sd(c->dir_path, name);
    ev->is_dir  = is_dir;
    ev->pos     = (uint16_t)(c->base + c->published);
    if ( !QACTIVE_POST_X(c->ask, &ev->super, KODE_STORAGE_LIST_FREE_SLOTS, AO_Storage) )
    {
        c->truncated = true;   /* QP took the entry back; the page ends here */
        return false;
    }
    ++c->published;
    return true;
}

/**
 * @brief Publish one entry of a path walk. Runs inside
 *        kdrv_storage_list_path(), on this AO's thread.
 *
 * Only the path walk knows `is_dir`, which `ls` prints as `type=file|dir`.
 */
static bool publish_path_entry(const char *name, bool is_dir, uint32_t size,
                               void *ctx)
{
    PathListCtx *c = (PathListCtx *)ctx;

    /* Skip rather than truncate: a name that does not fit the event would come
     * back as a request nobody could resolve to a file. Same rule
     * publish_entry() applies above. */
    size_t const len = strlen(name);
    if ( KODE_APP_FILE_MAX <= len )
    {
        KLOG_W(TAG, "ignoring '%s': name longer than %u chars",
               name, (unsigned)(KODE_APP_FILE_MAX - 1U));
        return true;
    }

    if ( NULL != c->ask )
    {
        return ask_path_entry(c, name, len, is_dir, size);
    }
    if ( NULL != c->wait )
    {
        /* A blocked task asked: the row goes straight into its own array and
         * nothing is published. Its array's size is the ceiling, not
         * KODE_STORAGE_LIST_MAX, and it is told when there was more. */
        if ( c->published >= c->wait->cap )
        {
            c->capped = true;
            return false;
        }
        StorageRow * const row = &c->wait->rows[c->published];
        (void)memcpy(row->name, name, len + 1U);
        row->is_dir = is_dir;
        row->size   = size;
        ++c->published;
        return true;
    }

    if ( KODE_STORAGE_LIST_MAX <= c->published )
    {
        c->capped = true;
        return false;
    }

    StorageEntryEvt *ev = Q_NEW_X(StorageEntryEvt, 0U, STORAGE_ENTRY_SIG);
    if ( NULL == ev )
    {
        /* Out of pool blocks - end the walk and say so, exactly as
         * publish_entry() does and for the same reason: AO_Gui subscribes to
         * this signal too, its lowest-priority subscriber. */
        c->truncated = true;
        return false;
    }
    (void)memcpy(ev->name, name, len + 1U);
    ev->size    = size;
    ev->from_sd = path_entry_from_sd(c->dir_path, name);
    ev->is_dir  = is_dir;
    ev->pos     = (uint16_t)c->published;
    QACTIVE_PUBLISH(&ev->super, &l_storage.super);
    ++c->published;
    return true;
}

/**
 * @brief Answer an ask's STORAGE_PATH_LIST: a page of entries posted to the
 *        asker, then its end, FILE_LIST_END_SIG with the count and whether more
 *        follow. A path that is not a directory is KODE_DENIED, not an empty page;
 *        a page that finds no room for its first entry is KODE_NO_RESOURCES.
 * @param path The directory.
 * @param skip Entries earlier pages sent.
 */
static void list_ask(const char *path, uint32_t skip)
{
    kdrv_stat_t   st  = { .exists = false, .is_dir = false, .size = 0U };
    kode_status_t rc  = kdrv_storage_stat(path, &st);
    PathListCtx   ctx = { .dir_path = path, .published = 0U,
                          .truncated = false, .capped = false, .wait = NULL,
                          .ask = s_ask_who, .skip = skip, .base = skip };

    if ( (KODE_OK == rc) && (!st.exists || !st.is_dir) )
    {
        rc = KODE_DENIED;
    }
    if ( KODE_OK == rc )
    {
        rc = kdrv_storage_list_path(path, &publish_path_entry, &ctx);
    }
    if ( (KODE_OK == rc) && ctx.truncated && (0U == ctx.published) )
    {
        /* Not one entry found room: the asker asking again would get the same. */
        rc = KODE_NO_RESOURCES;
    }
    s_ask_rsp->count = (uint16_t)ctx.published;
    s_ask_rsp->more  = ctx.capped || ctx.truncated;
    file_ask_answer(rc, NULL);
}

/**
 * @brief Answer a STORAGE_PATH_LIST request.
 *
 * Same ceiling (KODE_STORAGE_LIST_MAX), "!" line and event shape as sd_ls.
 * The terminator is unconditional, even on a refusal from
 * kdrv_storage_list_path(): the requester waits on STORAGE_LIST_END_SIG.
 * An ask's listing is list_ask()'s.
 *
 * @param path The directory.
 * @param skip An ask's entries to skip; nobody else's.
 */
static void do_path_list(const char *path, uint32_t skip)
{
    PathListCtx ctx = { .dir_path = path, .published = 0U,
                        .truncated = false, .capped = false,
                        .wait = s_waiter, .ask = NULL, .skip = 0U, .base = 0U };

    if ( NULL != s_ask_rsp )
    {
        list_ask(path, skip);
        return;
    }
    kode_status_t const rc =
        kdrv_storage_list_path(path, &publish_path_entry, &ctx);
    if ( ctx.capped )
    {
        KLOG_W(TAG, "listing stopped at %u entries (the ceiling)",
               (unsigned)ctx.published);
    }
    else if ( ctx.truncated )
    {
        KLOG_W(TAG, "listing cut short: no event blocks left");
    }
    else if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "cannot list '%s'", path);
    }

    if ( NULL != s_waiter )
    {
        /* Written before the wake, never after: wake_waiter()'s release store
         * is what publishes these two to the other core along with the rows
         * themselves. `cut` is the whole reason a caller can tell a directory
         * of exactly `cap` entries from one that had more - the count alone
         * cannot say it. */
        s_waiter->got = (uint32_t)ctx.published;
        s_waiter->cut = ctx.capped || ctx.truncated;
        wake_waiter(s_waiter, s_waiter_gen, rc, false, false, 0U);
        return;
    }
    list_end(NULL, false);
}

/* ---- Reading ------------------------------------------------------------- */

/** Find a buffer nobody is holding. @return STORAGE_BUFS when they are all out. */
static uint8_t free_buf(Storage * const me)
{
    for ( uint8_t i = 0U; STORAGE_BUFS > i; ++i )
    {
        if ( !me->lent[i] )
        {
            return i;
        }
    }
    return STORAGE_BUFS;
}

/** Fill a free buffer and send it to the reader. */
static void serve_read(Storage * const me)
{
    if ( NULL == me->reader )
    {
        return;                       /* nothing open; a stale request */
    }
    uint8_t const idx = free_buf(me);
    if ( STORAGE_BUFS <= idx )
    {
        KLOG_W(TAG, "both buffers are out on loan; read request ignored");
        return;
    }

    size_t got = 0U;
    kode_status_t const st =
        kdrv_storage_read(me->buf[idx], STORAGE_CHUNK, &got);

    StorageDataEvt *ev = Q_NEW_X(StorageDataEvt, 0U, STORAGE_DATA_SIG);
    if ( NULL == ev )
    {
        /* Bounded: without an event the reader would wait forever, so say so
         * loudly rather than stalling in silence. */
        KLOG_E(TAG, "out of events mid-read; the transfer will stall");
        return;
    }
    me->lent[idx] = (KODE_OK == st) && (0U < got);

    ev->idx    = idx;
    ev->len    = (uint16_t)got;
    ev->eof    = (0U == got);
    ev->status = (uint8_t)st;
    QACTIVE_POST(me->reader, &ev->super, &me->super);
}

/** @brief End the read conversation: close the reader and take back every lent buffer. */
static void close_file(Storage * const me)
{
    /* Always a read (the close handler refuses during a write), so
     * the close status, about a writer's buffered bytes, is ignored. */
    (void)kdrv_storage_close();
    me->reader = NULL;
    for ( uint8_t i = 0U; STORAGE_BUFS > i; ++i )
    {
        me->lent[i] = false;
    }
}

/**
 * @brief Is anything already using a file handle, or one of the two
 *        streaming buffers behind them?
 *
 * kdrv_storage has one reader and one writer for the whole system, and every
 * path that opens anything, or uses buf[0]/buf[1], asks this first. The card
 * test, a mid-walk `cat`, a `cp`, a `file_put`, an `appdata_put`, the wipe of
 * an app's files and a card call past its deadline (sdj.expired) all hold a
 * handle, a buffer or the volume.
 */
static bool busy(const Storage * const me)
{
    return (NULL != me->reader) || (NULL != me->writer) || (0U != me->t_phase)
        || (NULL != me->c_who) || (NULL != me->p_who) || (NULL != me->r_who)
        || (NULL != me->g_who) || (NULL != me->x_who) || me->sdj.expired;
}

/**
 * @brief Read up to @p max bytes of @p path in one step, reader slot untouched.
 *
 * Open, read and close in one dispatch step, so the one handle is never left held.
 *
 * @param path Absolute, virtual: "/storage/..." or "/sdcard/...".
 * @param buf  Where the bytes go.
 * @param max  How many at most.
 * @param[out] got   How many arrived.
 * @param[out] size  The file's whole length, which may exceed @p max.
 * @return false when the file is not there or cannot be opened.
 */
static bool appdata_read_file(const char *path, void *buf, size_t max,
                              size_t *got, uint32_t *size)
{
    if ( kdrv_storage_open_path(path, size) != KODE_OK )
    {
        return false;
    }
    kode_status_t const st = kdrv_storage_read(buf, max, got);
    (void)kdrv_storage_close();
    return KODE_OK == st;
}

/* ---- Deleting one app's data folder ---------------------------------------
 * kdrv_storage has no recursive delete; this is a bounded one, two levels deep,
 * for the app data folder layout (https://docs.kode.diy/sdk/kdrv/apps).
 * Names are collected first and deleted afterwards, never inside the walk:
 * FAT makes no promise about readdir() after the current entry is removed.
 */

#define APPDATA_RM_MAX  8U   /**< Entries one pass collects; see `cut`. */

/** @brief One directory level, read into names before anything is removed. */
typedef struct {
    char    name[APPDATA_RM_MAX][KAPP_DATA_ICON_MAX];
    bool    is_dir[APPDATA_RM_MAX];
    uint8_t n;
    bool    cut;   /**< More entries than a pass holds, or a name too long. */
} AppDataRmList;

/** @brief kdrv_path_entry_cb: remember one entry, delete nothing. */
static bool appdata_rm_entry(const char *name, bool is_dir, uint32_t size,
                             void *ctx)
{
    AppDataRmList * const l = (AppDataRmList *)ctx;
    (void)size;

    if ( (APPDATA_RM_MAX <= l->n) || (strlen(name) >= sizeof(l->name[0])) )
    {
        l->cut = true;
        return true;   /* keep walking: `cut` is what the caller acts on */
    }
    (void)snprintf(l->name[l->n], sizeof(l->name[0]), "%s", name);
    l->is_dir[l->n] = is_dir;
    ++l->n;
    return true;
}

/**
 * @brief Empty one directory of its files.
 *
 * @param dir   Absolute path of the directory to empty.
 * @param l     Scratch, filled by the walk; caller's, so the two levels
 *              below can share one.
 * @param[out] sub  The first subdirectory found, or "" - only the top level
 *              wants it. NULL when subdirectories are of no interest, which
 *              is the case one level down, where they are simply left.
 * @return true when everything the directory held is gone.
 */
static bool appdata_rm_files(const char *dir, AppDataRmList *l, char *sub,
                             size_t sub_cap)
{
    char path[APPDATA_PATH_MAX];
    bool all = true;

    (void)memset(l, 0, sizeof(*l));
    if ( NULL != sub )
    {
        sub[0] = '\0';
    }
    if ( kdrv_storage_list_path(dir, &appdata_rm_entry, l) != KODE_OK )
    {
        return false;
    }
    for ( uint8_t i = 0U; i < l->n; ++i )
    {
        if ( snprintf(path, sizeof(path), "%s/%s", dir, l->name[i])
            >= (int)sizeof(path) )
        {
            all = false;
            continue;
        }
        if ( l->is_dir[i] )
        {
            if ( (NULL != sub) && ('\0' == sub[0]) )
            {
                (void)snprintf(sub, sub_cap, "%s", l->name[i]);
            }
            else
            {
                all = false;   /* deeper than the format goes */
            }
        }
        else if ( kdrv_storage_unlink(path) != KODE_OK )
        {
            KLOG_W(TAG, "'%s' would not go", path);
            all = false;
        }
        else
        {
            /* Gone. */
        }
    }
    return all && !l->cut;
}

/**
 * @brief Delete one data folder, whatever medium it is on.
 *
 * @param folder Absolute path of the folder: what appdata_folder_path() wrote.
 * @return KODE_OK the folder is gone, KODE_ERROR there was none,
 *         KODE_IO_ERROR it is there and something in it would not go.
 */
static kode_status_t appdata_rm_tree(const char *folder)
{
    static AppDataRmList list;   /* one delete at a time, by design */
    char        sub[KAPP_DATA_ICON_MAX];
    char        path[APPDATA_PATH_MAX];
    kdrv_stat_t st;

    if ( (kdrv_storage_stat(folder, &st) != KODE_OK) || !st.exists || !st.is_dir )
    {
        return KODE_ERROR;
    }

    bool all = appdata_rm_files(folder, &list, sub, sizeof(sub));
    if ( '\0' != sub[0] )
    {
        if ( snprintf(path, sizeof(path), "%s/%s", folder, sub)
            < (int)sizeof(path) )
        {
            all = appdata_rm_files(path, &list, NULL, 0U) && all;
            all = (kdrv_storage_rmdir(path) == KODE_OK) && all;
        }
        else
        {
            all = false;
        }
    }
    if ( !all || (kdrv_storage_rmdir(folder) != KODE_OK) )
    {
        KLOG_W(TAG, "'%s' did not empty", folder);
        return KODE_IO_ERROR;
    }
    return KODE_OK;
}

/**
 * @brief Delete @p project's folder on the internal partition, and say so.
 *
 * @param me      The AO, for busy().
 * @param project The app's project name.
 * @return What AppDataRmRspEvt.status carries; see its own documentation.
 */
static kode_status_t appdata_rm(const Storage * const me, const char *project)
{
    char        folder[APPDATA_FOLDER_MAX];
    char        card[APPDATA_FOLDER_MAX];
    kdrv_stat_t st;

    /* Said before anything is removed and whatever the outcome: an operator
     * who copied a folder onto a card and then deletes "it" has to be told
     * which one this verb can reach. */
    if ( kdrv_storage_mounted(KDRV_MEDIUM_SD)
        && appdata_folder_path(card, sizeof(card), APPDATA_MEDIUM_SD, project)
        && (kdrv_storage_stat(card, &st) == KODE_OK) && st.exists && st.is_dir )
    {
        KLOG_N(TAG, "card folder for '%s' left alone", project);
    }

    if ( busy(me) )
    {
        KLOG_W(TAG, "a transfer is already in flight; the folder of '%s' "
                    "stays", project);
        return KODE_DENIED;
    }
    /* A name that cannot be a folder is refused before anything is mounted:
     * the mount below formats a partition that will not mount. */
    if ( !appdata_folder_path(folder, sizeof(folder), APPDATA_MEDIUM_FS,
                             project) )
    {
        return KODE_ERROR;
    }
    /* Nothing may have mounted the internal FAT yet (no launcher scan has
     * run): mount it here; a volume that will not mount is formatted first,
     * as the boot scan does, and refused only when that fails too. */
    if ( kdrv_storage_mount(KDRV_MEDIUM_FS) == NULL )
    {
        return KODE_ERROR;
    }

    kode_status_t const rc = appdata_rm_tree(folder);
    if ( KODE_OK == rc )
    {
        KLOG_N(TAG, "the data folder of '%s' is gone", project);
    }
    return rc;
}

/* ---- Taking every file of one app ------------------------------------------
 * An erase's share of this AO: the images at a medium's root whose descriptor
 * names the project, whatever the files are called, and the app's data folder,
 * on the internal partition and on the card. The request's step walks each
 * root once and keeps the names, since FAT makes no promise about readdir()
 * across an unlink; each file then has a step of its own, paced as a copy's
 * chunks are; the last step takes the folders and answers.
 */

/** @brief One root's walk: where the names go and how many it has kept. */
typedef struct {
    Storage *me;     /**< The AO: its scratch takes the names.               */
    uint16_t kept;   /**< Names kept from this medium.                       */
    bool     cut;    /**< The medium holds more images than a walk keeps.    */
} WipeWalk;

/** @brief kdrv_storage_entry_cb: keep the name of an image an app may own. */
static bool wipe_collect(const char *name, uint32_t size, void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the walk wipe_walk() handed the listing */
    WipeWalk * const w  = (WipeWalk *)ctx;
    Storage * const  me = w->me;
    (void)size;

    /* The OS image and the coprocessor's are left out by name, before any
     * descriptor is read: what they say they are never puts them in reach. */
    if ( !is_app_image(name) || name_is(name, WIPE_OS_IMAGE)
        || name_is(name, WIPE_C5_IMAGE)
        || (strlen(name) >= (size_t)KODE_APP_FILE_MAX) )
    {
        return true;
    }
    if ( WIPE_FILES_MAX <= w->kept )
    {
        w->cut = true;
        return false;
    }
    (void)snprintf(me->x->names[me->x_count], sizeof(me->x->names[0]), "%s",
                   name);
    ++me->x_count;
    ++w->kept;
    return true;
}

/**
 * @brief Keep the names of the images at one medium's root.
 *
 * @param me     The AO, with its scratch taken.
 * @param medium Which medium.
 * @return true when the medium was there and its root was read.
 */
static bool wipe_walk(Storage * const me, kdrv_medium_t medium)
{
    WipeWalk            walk = { .me = me, .kept = 0U, .cut = false };
    kode_status_t const rc   = kdrv_storage_list(medium, &wipe_collect, &walk);

    if ( walk.cut )
    {
        KLOG_W(TAG, "more than %u images at one root; the rest are not "
                    "looked at for '%s'", (unsigned)WIPE_FILES_MAX,
               me->x_project);
    }
    return KODE_OK == rc;
}

/**
 * @brief Answer a wipe that did not run. Bounded, never asserts.
 * @param who     The asker.
 * @param project The project the wipe was for.
 * @param rc      Why it did not run.
 */
static void wipe_refuse(QActive *who, const char *project, kode_status_t rc)
{
    StorageAppWipeRspEvt *rsp = Q_NEW_X(StorageAppWipeRspEvt, 0U,
                                        STORAGE_APP_WIPE_RSP_SIG);

    if ( NULL == rsp )
    {
        KLOG_E(TAG, "out of events; the wipe of '%s' goes unanswered", project);
        return;
    }
    (void)memset(&rsp->fs, 0, sizeof(rsp->fs));
    (void)memset(&rsp->sd, 0, sizeof(rsp->sd));
    (void)snprintf(rsp->project, sizeof(rsp->project), "%s", project);
    rsp->status = (uint8_t)rc;
    QACTIVE_POST(who, &rsp->super, AO_Storage);
}

/**
 * @brief Start the wipe of one app's files: keep the names, arm the first step.
 *
 * @param me  The AO.
 * @param req The request: the project and the asker.
 */
static void wipe_begin(Storage * const me, AppDataReqEvt const * const req)
{
    if ( NULL == req->who )
    {
        KLOG_W(TAG, "the wipe of '%s' names nobody to answer; refused",
               req->project);
        return;
    }
    if ( busy(me) )
    {
        KLOG_W(TAG, "a transfer is already in flight; the files of '%s' stay",
               req->project);
        wipe_refuse(req->who, req->project, KODE_DENIED);
        return;
    }
    /* cppcheck-suppress misra-c2012-11.5 ; a struct from a void * allocator */
    me->x = (WipeScratch *)khal_alloc_psram(64U, sizeof(WipeScratch));
    if ( NULL == me->x )
    {
        KLOG_W(TAG, "no memory for the list of images; the files of '%s' stay",
               req->project);
        wipe_refuse(req->who, req->project, KODE_NO_RESOURCES);
        return;
    }
    (void)memset(&me->x_fs, 0, sizeof(me->x_fs));
    (void)memset(&me->x_sd, 0, sizeof(me->x_sd));
    (void)snprintf(me->x_project, sizeof(me->x_project), "%s", req->project);
    me->x_who   = req->who;
    me->x_count = 0U;
    me->x_next  = 0U;

    /* Nothing may have mounted the internal FAT yet: mount it here, as
     * appdata_rm() does. */
    if ( kdrv_storage_mount(KDRV_MEDIUM_FS) != NULL )
    {
        me->x_fs.present = wipe_walk(me, KDRV_MEDIUM_FS);
    }
    me->x_fs_count   = me->x_count;
    me->x_sd.present = wipe_walk(me, KDRV_MEDIUM_SD);

    QTimeEvt_armX(&me->x_pacer, STORAGE_WRITE_GAP_TICKS, 0U);
}

/**
 * @brief Whether the image at @p path says it is the project being wiped.
 *
 * Open, read the front and close within this step: the one reader is the
 * wipe's while it runs, and is never left held.
 *
 * @param me   The AO, with a wipe under way.
 * @param path Absolute, virtual: "/storage/..." or "/sdcard/...".
 * @return true when its descriptor names the project.
 */
static bool wipe_is_of(Storage * const me, const char *path)
{
    char          project[KODE_APP_PROJECT_MAX];
    uint32_t      size = 0U;
    size_t        got  = 0U;
    kode_status_t st;

    if ( kdrv_storage_open_path(path, &size) != KODE_OK )
    {
        return false;
    }
    st = kdrv_storage_read(me->x->prefix, sizeof(me->x->prefix), &got);
    (void)kdrv_storage_close();
    if ( (KODE_OK != st) || (sizeof(me->x->prefix) > got) )
    {
        return false;
    }
    if ( kapp_project_name(me->x->prefix, got, project, sizeof(project))
        != KODE_OK )
    {
        return false;
    }
    /* An image that names no project is nobody's, whatever was asked. */
    return ('\0' != project[0]) && (strcmp(project, me->x_project) == 0);
}

/**
 * @brief Take @p project's data folder off one medium.
 *
 * @param m       Which medium's tree.
 * @param project The app's project name.
 * @return What became of it.
 */
static AppWipeFolder wipe_folder(AppDataMedium m, const char *project)
{
    char          folder[APPDATA_FOLDER_MAX];
    AppWipeFolder end = APP_WIPE_FOLDER_NONE;

    /* A name FAT cannot hold has no folder to take. */
    if ( appdata_folder_path(folder, sizeof(folder), m, project) )
    {
        kode_status_t const rc = appdata_rm_tree(folder);

        if ( KODE_OK == rc )
        {
            end = APP_WIPE_FOLDER_REMOVED;
        }
        else if ( KODE_ERROR != rc )
        {
            end = APP_WIPE_FOLDER_LEFT;
        }
        else
        {
            /* There was none. */
        }
    }
    return end;
}

/**
 * @brief End the wipe: the folders, the answer, and the scratch given back.
 * @param me The AO, with every kept name looked at.
 */
static void wipe_finish(Storage * const me)
{
    StorageAppWipeRspEvt *rsp = Q_NEW_X(StorageAppWipeRspEvt, 0U,
                                        STORAGE_APP_WIPE_RSP_SIG);

    if ( me->x_fs.present )
    {
        me->x_fs.folder = (uint8_t)wipe_folder(APPDATA_MEDIUM_FS, me->x_project);
    }
    if ( me->x_sd.present )
    {
        me->x_sd.folder = (uint8_t)wipe_folder(APPDATA_MEDIUM_SD, me->x_project);
    }
    KLOG_N(TAG, "the files of '%s': %u gone and %u left on the partition, "
                "%u gone and %u left on the card", me->x_project,
           (unsigned)me->x_fs.files, (unsigned)me->x_fs.left,
           (unsigned)me->x_sd.files, (unsigned)me->x_sd.left);

    if ( NULL == rsp )
    {
        KLOG_E(TAG, "out of events; the wipe of '%s' goes unanswered",
               me->x_project);
    }
    else
    {
        (void)snprintf(rsp->project, sizeof(rsp->project), "%s", me->x_project);
        rsp->status = (uint8_t)KODE_OK;
        rsp->fs     = me->x_fs;
        rsp->sd     = me->x_sd;
        QACTIVE_POST(me->x_who, &rsp->super, &me->super);
    }
    khal_free(me->x);
    me->x     = NULL;
    me->x_who = NULL;
}

/**
 * @brief Look at one kept image and delete it when it is the project's, then
 *        arm the next step or finish.
 * @param me The AO.
 */
static void wipe_step(Storage * const me)
{
    char           path[WIPE_PATH_MAX];
    const char    *root = KODE_CARD_APP_PREFIX;
    AppWipeMedium *m    = &me->x_sd;

    if ( NULL == me->x_who )
    {
        return;                       /* nothing being wiped; a stale tick */
    }
    if ( me->sdj.busy )
    {
        /* A call to the card is out on the worker: the volume is its own
         * until it returns. */
        QTimeEvt_armX(&me->x_pacer, STORAGE_WRITE_GAP_TICKS, 0U);
        return;
    }
    if ( me->x_next >= me->x_count )
    {
        wipe_finish(me);
        return;
    }
    if ( me->x_next < me->x_fs_count )
    {
        root = WIPE_FS_ROOT;
        m    = &me->x_fs;
    }
    (void)snprintf(path, sizeof(path), "%s%s", root, me->x->names[me->x_next]);
    ++me->x_next;

    if ( wipe_is_of(me, path) )
    {
        if ( kdrv_storage_unlink(path) == KODE_OK )
        {
            KLOG_N(TAG, "'%s' removed: an image of '%s'", path, me->x_project);
            ++m->files;
        }
        else
        {
            KLOG_W(TAG, "'%s' would not go", path);
            ++m->left;
        }
    }
    QTimeEvt_armX(&me->x_pacer, STORAGE_WRITE_GAP_TICKS, 0U);
}

/* ---- Writing one app's data folder -----------------------------------------
 * The install of a container that carries metadata. Two steps of this AO, one
 * file each, with a self-post between so 13 KB of FAT never holds the AO in
 * one go; the request's fields and bytes are kept in g_* for the second step,
 * and appdata_put_finish() frees g_buf. Internal partition only.
 */

/**
 * @brief Answer a put to its asker. Bounded, never asserts.
 * @param who     The asker.
 * @param project The project the put was for.
 * @param rc      The outcome.
 */
static void appdata_put_answer(QActive *who, const char *project,
                               kode_status_t rc)
{
    AppDataPutRspEvt *rsp = Q_NEW_X(AppDataPutRspEvt, 0U, APPDATA_PUT_RSP_SIG);
    if ( NULL == rsp )
    {
        KLOG_E(TAG, "out of events; the put of '%s' goes unanswered", project);
        return;
    }
    (void)snprintf(rsp->project, sizeof(rsp->project), "%s", project);
    rsp->rc = rc;
    QACTIVE_POST(who, &rsp->super, NULL);
}

/**
 * @brief Give back the buffer a put was handed, and forget it.
 * @param me The AO.
 */
static void appdata_put_free(Storage * const me)
{
    if ( NULL != me->g_buf )
    {
        khal_free(me->g_buf);
    }
    me->g_buf      = NULL;
    me->g_json_len = 0U;
    me->g_icon_len = 0U;
}

/**
 * @brief Write one file of a folder: create, write, close, all in this step.
 *
 * @param path Absolute virtual path on the internal partition.
 * @param data The bytes.
 * @param len  How many.
 * @return KODE_OK, or what the medium said; the stub is discarded on failure.
 */
static kode_status_t appdata_put_file(const char *path, const uint8_t *data,
                                      uint16_t len)
{
    kode_status_t st = kdrv_storage_create_path(path, false);
    if ( KODE_OK != st )
    {
        return st;
    }
    st = kdrv_storage_write(data, (size_t)len);
    if ( KODE_OK != st )
    {
        kdrv_storage_discard();
        return st;
    }
    return kdrv_storage_close();
}

/** @brief Make one directory; one that already exists is as good as new. */
static kode_status_t appdata_put_mkdir(const char *path)
{
    kode_status_t const st = kdrv_storage_mkdir(path);
    if ( KODE_EXISTS == st )
    {
        return KODE_OK;
    }
    return st;
}

/**
 * @brief End a put: forget it, remove what a failure left, answer.
 *
 * The state goes first because appdata_rm() refuses while busy() is true and
 * g_who is part of busy().
 *
 * @param me The AO.
 * @param rc How it ended; anything but KODE_OK takes the folder away again.
 */
static void appdata_put_finish(Storage * const me, kode_status_t rc)
{
    QActive * const who = me->g_who;
    char            project[KDRV_SLOT_PROJECT_MAX];

    (void)snprintf(project, sizeof(project), "%s", me->g_project);
    me->g_who      = NULL;
    appdata_put_free(me);
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "the data folder of '%s' was not written (%d)", project,
               (int)rc);
        (void)appdata_rm(me, project);
    }
    else
    {
        KLOG_N(TAG, "the data folder of '%s' is written", project);
    }
    if ( NULL != who )
    {
        appdata_put_answer(who, project, rc);
    }
}

/**
 * @brief Step one of a put: refuse, clear, make the path, write app.json.
 *
 * The request's buffer is this AO's from the post: a refusal frees it here,
 * and a put that begins keeps it in g_buf for appdata_put_finish().
 *
 * @param me  The AO.
 * @param req The request; its fields are copied before this returns.
 */
static void appdata_put_begin(Storage * const me, AppDataPutEvt const * const req)
{
    char          folder[APPDATA_FOLDER_MAX];
    kode_status_t rc = KODE_DENIED;

    if ( busy(me) )
    {
        KLOG_W(TAG, "a transfer is already in flight; the folder of '%s' is "
                    "not written", req->project);
    }
    else if ( !appdata_folder_path(folder, sizeof(folder), APPDATA_MEDIUM_FS,
                                  req->project)
             || (NULL == req->buf) || (0U == req->json_len)
             || (KAPP_DATA_JSON_MAX < req->json_len)
             || ((0U != req->icon_len) && (KAPP_ICON_BYTES != req->icon_len)) )
    {
        KLOG_W(TAG, "'%s' cannot be a data folder as asked", req->project);
    }
    else if ( kdrv_storage_mount(KDRV_MEDIUM_FS) == NULL )
    {
        rc = KODE_ERROR;
    }
    else
    {
        rc = KODE_OK;
    }
    if ( KODE_OK != rc )
    {
        if ( NULL != req->buf )
        {
            khal_free(req->buf);
        }
        appdata_put_answer(req->who, req->project, rc);
        return;
    }

    /* From here the put owns the AO. The old folder goes first and "there was
     * none" is no failure; appdata_rm() reads busy(), so g_who is set after. */
    rc = appdata_rm(me, req->project);
    if ( KODE_ERROR == rc )
    {
        rc = KODE_OK;
    }
    me->g_who      = req->who;
    (void)snprintf(me->g_project, sizeof(me->g_project), "%s", req->project);
    me->g_buf      = req->buf;
    me->g_json_len = req->json_len;
    me->g_icon_len = req->icon_len;

    if ( KODE_OK == rc )
    {
        rc = appdata_put_mkdir(APPDATA_TOP_DIR);
    }
    if ( KODE_OK == rc )
    {
        rc = appdata_put_mkdir(APPDATA_TREE_DIR);
    }
    if ( KODE_OK == rc )
    {
        rc = appdata_put_mkdir(folder);
    }
    if ( KODE_OK == rc )
    {
        char path[APPDATA_PATH_MAX];

        if ( snprintf(path, sizeof(path), "%s/" APPDATA_JSON_NAME, folder)
            >= (int)sizeof(path) )
        {
            rc = KODE_ERROR;
        }
        else
        {
            rc = appdata_put_file(path, req->buf, req->json_len);
        }
    }
    if ( KODE_OK != rc )
    {
        appdata_put_finish(me, rc);
    }
    else
    {
        static const QEvt next = QEVT_INITIALIZER(APPDATA_PUT_NEXT_SIG);
        QACTIVE_POST(&me->super, &next, &me->super);
    }
}

/** @brief Step two of a put: the icon when there is one, then the answer. */
static void appdata_put_icon(Storage * const me)
{
    kode_status_t rc = KODE_OK;

    if ( NULL == me->g_who )
    {
        return;   /* a stale tick: no put is under way */
    }
    if ( 0U != me->g_icon_len )
    {
        char folder[APPDATA_FOLDER_MAX];
        char path[APPDATA_PATH_MAX];

        if ( !appdata_folder_path(folder, sizeof(folder), APPDATA_MEDIUM_FS,
                                 me->g_project)
            || (snprintf(path, sizeof(path), "%s/" APPDATA_ICON_NAME, folder)
                >= (int)sizeof(path)) )
        {
            rc = KODE_ERROR;
        }
        else
        {
            rc = appdata_put_file(path, &me->g_buf[me->g_json_len],
                                  me->g_icon_len);
        }
    }
    appdata_put_finish(me, rc);
}

/* ---- The card's field table ----------------------------------------------- */

/** @brief Answer a field request to whoever asked. Bounded, never asserts. */
static void field_answer(StorageFieldReqEvt const *q, kode_status_t rc,
                         int32_t value)
{
    if ( NULL == q->who )
    {
        return;
    }
    StorageFieldEvt *ev = Q_NEW_X(StorageFieldEvt, 0U, STORAGE_FIELD_RSP_SIG);
    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; a field answer is lost");
        return;
    }
    ev->op    = q->op;
    ev->field = q->field;
    ev->rc    = (uint8_t)rc;
    ev->value = value;
    QACTIVE_POST(q->who, &ev->super, NULL);
}

/**
 * @brief One field of the card, as asked.
 *
 * SET is followed by a GET so the answer carries what the driver kept: a
 * width the slot cannot do is refused, so this is what the next mount uses.
 */
static kode_status_t field_do(StorageFieldReqEvt const *q, int32_t *value)
{
    kdrv_sd_field_t const f = (kdrv_sd_field_t)q->field;
    kode_status_t rc = KODE_DENIED;

    switch ( q->op )
    {
        case FIELD_OP_GET: {
            rc = kdrv_sd_get(f, value);
            break;
        }
        case FIELD_OP_SET: {
            rc = kdrv_sd_set(f, *value);
            if ( KODE_OK == rc )
            {
                rc = kdrv_sd_get(f, value);
            }
            break;
        }
        case FIELD_OP_DUMP: {
            kdrv_sd_dump();
            rc = KODE_OK;
            break;
        }
        default: {
            break;
        }
    }
    return rc;
}

/* ---- Path requests --------------------------------------------------------
 * AO_Storage is the only code that opens a file, so the console's stat,
 * listing and `cat` are requests. STAT answers once; LIST answers as the
 * burst do_path_list() drives; CAT answers one line at a time, straight
 * back to the asker (do_path_cat()).
 */

/**
 * @brief Answer one path-shaped request: a StoragePathRspEvt posted to @p who,
 *        or a blocked task's own storage written and its semaphore signalled.
 *        Bounded, never asserts.
 *
 * The request said which; the blocked task is s_waiter, not a parameter.
 */
static void post_path_rsp(QActive *who, uint8_t op, kode_status_t rc,
                          bool exists, bool is_dir, uint32_t size)
{
    if ( NULL != s_ask_rsp )
    {
        FileStat const st = { .exists = exists, .is_dir = is_dir, .size = size };

        file_ask_answer(rc, &st);
        return;
    }
    if ( NULL != s_waiter )
    {
        wake_waiter(s_waiter, s_waiter_gen, rc, exists, is_dir, size);
        return;
    }
    if ( NULL == who )
    {
        return;
    }
    Q_ASSERT(NULL == s_ask_who);   /* an ask in hand is answered by its funnel, once */
    StoragePathRspEvt *ev = Q_NEW_X(StoragePathRspEvt, 0U, STORAGE_PATH_RSP_SIG);
    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; a path answer is lost");
        return;
    }
    ev->op     = op;
    ev->rc     = (uint8_t)rc;
    ev->exists = exists;
    ev->is_dir = is_dir;
    ev->size   = size;
    QACTIVE_POST(who, &ev->super, NULL);
}

/** @brief Bytes in the kilobyte StorageInfoEvt and FileSpace count in. */
#define STORAGE_KB_BYTES  1024ULL

/**
 * @brief A byte count in whole kilobytes, cut down: what `sd_info` and `fs_info` print.
 * @param bytes The count.
 * @return Its kilobytes.
 */
static uint32_t kb_of(uint64_t bytes)
{
    return (uint32_t)(bytes / STORAGE_KB_BYTES);
}

/**
 * @brief Answer a STORAGE_PATH_SPACE request: the ask's @c space, or a blocked
 *        task's waiter.
 *
 * Nothing else has a shape of answer for it, so a request that names neither
 * is refused as every path op is.
 *
 * @param who      The requester, read only when it is neither.
 * @param op       storage_path_op_t, for that refusal.
 * @param rc       kode_status_t for the round.
 * @param total_kb The volume's kilobytes; 0 unless @p rc is KODE_OK.
 * @param free_kb  What is left of them; 0 unless @p rc is KODE_OK.
 */
static void post_space_rsp(QActive *who, uint8_t op, kode_status_t rc, uint32_t total_kb,
                           uint32_t free_kb)
{
    if ( NULL != s_ask_rsp )
    {
        s_ask_rsp->space.total_kb = total_kb;
        s_ask_rsp->space.free_kb  = free_kb;
        file_ask_answer(rc, NULL);
        return;
    }
    if ( NULL != s_waiter )
    {
        s_waiter->total_kb = total_kb;
        s_waiter->free_kb  = free_kb;
        wake_waiter(s_waiter, s_waiter_gen, rc, false, false, 0U);
        return;
    }
    post_path_rsp(who, op, KODE_DENIED, false, false, 0U);
}

/**
 * @brief Whether @p path is a volume's own root: "/sdcard" or "/storage", and nothing after it.
 *
 * Both roots are one path part, so a further '/' makes a path inside a volume.
 *
 * @param path A virtual path, not NULL.
 * @return true for a root.
 */
static bool path_is_volume_root(const char *path)
{
    return (KDRV_MEDIUM_COUNT != kdrv_storage_medium_of(path)) && (NULL == strchr(&path[1], '/'));
}

/**
 * @brief Answer a STORAGE_PATH_SPACE request.
 *
 * The count is the one `sd_info` and `fs_info` print, taken in this step; the
 * volume is probed as a path call probes it. KODE_DENIED is a @p path that is
 * not a volume's root, KODE_ERROR a volume that will not mount or be counted.
 *
 * @param who  The requester, for the refusal of a request with no asker.
 * @param op   storage_path_op_t, for that refusal.
 * @param path The virtual path asked about.
 */
static void answer_path_space(QActive *who, uint8_t op, const char *path)
{
    kdrv_medium_t const medium = kdrv_storage_medium_of(path);
    uint64_t            total  = 0U;
    uint64_t            freeb  = 0U;
    kode_status_t       rc     = KODE_DENIED;

    if ( path_is_volume_root(path) )
    {
        rc = KODE_ERROR;
        if ( (NULL != kdrv_storage_probe(medium))
            && (KODE_OK == kdrv_storage_usage(medium, &total, &freeb)) )
        {
            rc = KODE_OK;
        }
        else
        {
            total = 0U;
            freeb = 0U;
        }
    }
    post_space_rsp(who, op, rc, kb_of(total), kb_of(freeb));
}

/**
 * @brief Answer a STORAGE_PATH_STAT request.
 *
 * "Not there" is KODE_OK with `exists` false; KODE_ERROR is a volume that
 * will not mount, and KODE_DENIED a malformed path or one naming no volume.
 */
static void answer_path_stat(QActive *who, uint8_t op, const char *path)
{
    kdrv_stat_t   st = { .exists = false, .is_dir = false, .size = 0U };
    kode_status_t rc = kdrv_storage_stat(path, &st);

    post_path_rsp(who, op, rc, st.exists, st.is_dir, st.size);
}

/**
 * @brief Warn that the card is about to be changed, naming the verb and the
 *        path.
 *
 * Called before every kdrv call that might change the card, even if it then
 * refuses: on counterfeit cards one failed write leaves every later open on
 * /sdcard reading length 0, and the log must say a write was tried. A spare
 * line is cheap. Keep it here, in the card's owner, not in the driver or the
 * console: writes nobody typed (`install dst=app`) pass through here too.
 *
 * @param verb What is about to be attempted, as an operator would name it -
 *             "mkdir", "rm", "mv", "write". It goes on the wire verbatim, so
 *             it is a literal at every call site and never a path.
 * @param path The path about to be changed. Warned about only when it names
 *             the card.
 */
static void warn_if_card(const char *verb, const char *path)
{
    if ( kdrv_storage_medium_of(path) == KDRV_MEDIUM_SD )
    {
        KLOG_W(TAG, "%s on the card: %s", verb, path);
    }
}

/**
 * @brief Answer a STORAGE_PATH_MKDIR request.
 *
 * One level: kdrv_storage_mkdir() itself has no `-p`, and neither does this -
 * a missing parent comes back as KODE_ERROR unchanged, not silently repaired.
 */
static void answer_path_mkdir(QActive *who, uint8_t op, const char *path)
{
    warn_if_card("mkdir", path);
    kode_status_t const rc = kdrv_storage_mkdir(path);
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "mkdir '%s' failed", path);
    }
    /* `exists` here says whether the volume mounted: kdrv_storage_mkdir()
     * gives the same KODE_ERROR for a dead mount and a missing parent, and
     * path_answer() needs it to answer `volume` or `noparent`. */
    bool const mounted = kdrv_storage_mounted(kdrv_storage_medium_of(path));
    post_path_rsp(who, op, rc, mounted, false, 0U);
}

/**
 * @brief Answer a STORAGE_PATH_UNLINK request.
 *
 * The console's STAT phase has already said @p path is a file, but
 * kdrv_storage_unlink() checks again for callers that skip it.
 * warn_if_card() fires first, whether or not the delete reaches remove().
 */
static void answer_path_unlink(QActive *who, uint8_t op, const char *path)
{
    warn_if_card("rm", path);
    kode_status_t const rc = kdrv_storage_unlink(path);
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "rm '%s' failed", path);
    }
    post_path_rsp(who, op, rc, false, false, 0U);
}

/**
 * @brief Answer a STORAGE_PATH_RMDIR request.
 *
 * The console's STAT phase has said @p path is a directory; what can still
 * fail is rmdir() refusing a non-empty one.
 */
static void answer_path_rmdir(QActive *who, uint8_t op, const char *path)
{
    warn_if_card("rmdir", path);
    kode_status_t const rc = kdrv_storage_rmdir(path);
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "rmdir '%s' failed", path);
    }
    /* `exists` says whether the volume mounted, as in answer_path_mkdir():
     * path_answer() answers `volume` or `notempty` from it. */
    bool const mounted = kdrv_storage_mounted(kdrv_storage_medium_of(path));
    post_path_rsp(who, op, rc, mounted, false, 0U);
}

/**
 * @brief Answer a STORAGE_PATH_DELETE request: whichever of a file or an empty
 *        directory is at @p path goes, in one step.
 */
static void answer_path_delete(Storage const * const me, QActive *who, uint8_t op,
                               const char *path)
{
    kdrv_stat_t   st = { .exists = false, .is_dir = false, .size = 0U };
    kode_status_t rc;

    if ( busy(me) )
    {
        KLOG_W(TAG, "a transfer is already in flight; delete of '%s' refused",
               path);
        post_path_rsp(who, op, KODE_DENIED, false, false, 0U);
        return;
    }
    rc = kdrv_storage_stat(path, &st);
    if ( KODE_OK != rc )
    {
        post_path_rsp(who, op, rc, false, false, 0U);
    }
    else if ( st.exists && st.is_dir )
    {
        answer_path_rmdir(who, op, path);
    }
    else
    {
        answer_path_unlink(who, op, path);
    }
}

/**
 * @brief Whether @p src is the reason a pair op's KODE_ERROR just fired.
 *
 * kdrv_storage_rename() and kdrv_storage_copy_open() give one KODE_ERROR for
 * a dead source volume, a missing source and a destination that will not
 * open. A stat of @p src returning KODE_OK with `exists` false singles out the
 * missing source, the usual typo. One synchronous call in the same step.
 */
static bool pair_src_missing(const char *src)
{
    kdrv_stat_t st = { .exists = false, .is_dir = false, .size = 0U };
    return (kdrv_storage_stat(src, &st) == KODE_OK) && !st.exists;
}

/**
 * @brief Answer a STORAGE_PAIR_REQ_SIG move.
 *
 * warn_if_card() fires on @p dst before the rename, even one about to be
 * refused for crossing volumes. On a KODE_ERROR, `exists` in the response says
 * whether pair_src_missing() found the source missing.
 */
static void answer_pair_move(QActive *who, uint8_t op, const char *src,
                             const char *dst)
{
    warn_if_card("mv", dst);
    kode_status_t const rc = kdrv_storage_rename(src, dst);
    bool                src_missing = false;
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "mv '%s' -> '%s' failed", src, dst);
        if ( KODE_ERROR == rc )
        {
            src_missing = pair_src_missing(src);
        }
    }
    post_path_rsp(who, op, rc, src_missing, false, 0U);
}

/* ---- cp -------------------------------------------------------------------
 * The only thing in kodeOS that holds a reader and a writer at once. One
 * 32 KB chunk per dispatch step with a QTimeEvt between, so AO_Gui, AO_Radio
 * and AO_Net below this AO keep the CPU. One answer, at the end: a caller's
 * deadline must cover the whole transfer (KCLI_CP_TIMEOUT_MS in console_ao.c).
 */

/**
 * @brief End the copy: hand the buffer back, keep or remove the destination,
 *        and tell whoever asked.
 *
 * @param st KODE_OK when every step succeeded and the source ran out.
 *           Anything else removes the destination.
 *
 * A failed close (what fclose() said about the destination) overrides
 * KODE_OK: bytes the card refused at the flush were never copied.
 */
static void copy_finish(Storage * const me, kode_status_t st)
{
    (void)QTimeEvt_disarm(&me->p_pacer);

    kode_status_t const closed = kdrv_storage_copy_end(KODE_OK == st);
    if ( (KODE_OK == st) && (KODE_OK != closed) )
    {
        st = closed;
    }

    if ( KODE_OK != st )
    {
        KLOG_W(TAG, "cp '%s' -> '%s' failed at %lu of %lu bytes",
               me->p_src, me->p_dst, (unsigned long)me->p_done,
               (unsigned long)me->p_size);
    }
    else
    {
        KLOG_D(TAG, "cp '%s' -> '%s': %lu bytes",
               me->p_src, me->p_dst, (unsigned long)me->p_done);
    }

    me->lent[me->p_idx] = false;
    QActive * const who = me->p_who;
    me->p_who = NULL;
    post_path_rsp(who, (uint8_t)STORAGE_PAIR_COPY, st, false, false, 0U);
}

/** @brief Carry one chunk across and arm the next, or finish. */
static void copy_step(Storage * const me)
{
    if ( NULL == me->p_who )
    {
        return;                       /* nothing copying; a stale tick */
    }

    size_t              done = 0U;
    kode_status_t const rc   =
        kdrv_storage_copy_step(me->buf[me->p_idx], (size_t)STORAGE_CHUNK,
                               &done);
    if ( KODE_OK != rc )
    {
        copy_finish(me, rc);
        return;
    }
    if ( 0U == done )
    {
        /* The source ran out - kdrv_storage_read()'s own end-of-file signal,
         * a successful step that moved nothing, carried up unchanged. */
        copy_finish(me, KODE_OK);
        return;
    }

    me->p_done += (uint32_t)done;
    QTimeEvt_armX(&me->p_pacer, STORAGE_WRITE_GAP_TICKS, 0U);
}

/**
 * @brief Answer a STORAGE_PAIR_REQ_SIG copy.
 *
 * No STAT phase: kdrv_storage_copy_open() refuses a missing source, a
 * destination that will not open and a copy onto itself. warn_if_card() fires
 * on @p dst only. A NULL @p who is refused first: p_who is also the flag that
 * a copy is running, and a copy without it would hold both handles for good.
 * On a KODE_ERROR, `exists` says whether pair_src_missing() found the source missing.
 */
static void answer_pair_copy(Storage * const me, QActive *who, uint8_t op,
                             const char *src, const char *dst)
{
    if ( NULL == who )
    {
        KLOG_W(TAG, "cp '%s' -> '%s' has nobody to answer; refused",
              src, dst);
        /* A blocked task asleep on this is told, which post_path_rsp() does;
         * with no task it does nothing. `cp` is never served to a task, for
         * the reason above. The facade's rename asks for MOVE instead. */
        post_path_rsp(NULL, op, KODE_DENIED, false, false, 0U);
        return;
    }

    warn_if_card("cp", dst);

    if ( busy(me) )
    {
        KLOG_W(TAG, "a transfer is already in flight; cp of '%s' refused", src);
        post_path_rsp(who, op, KODE_DENIED, false, false, 0U);
        return;
    }

    uint8_t const idx = free_buf(me);
    if ( STORAGE_BUFS <= idx )
    {
        /* Unreachable while busy() is false, but said, in case a future
         * buffer holder forgets to join busy(). */
        KLOG_W(TAG, "both buffers are out on loan; cp of '%s' refused", src);
        post_path_rsp(who, op, KODE_DENIED, false, false, 0U);
        return;
    }

    uint32_t            size = 0U;
    kode_status_t const rc   = kdrv_storage_copy_open(src, dst, &size);
    if ( KODE_OK != rc )
    {
        bool src_missing = false;
        if ( KODE_ERROR == rc )
        {
            src_missing = pair_src_missing(src);
        }
        KLOG_W(TAG, "cp '%s' -> '%s' could not start", src, dst);
        post_path_rsp(who, op, rc, src_missing, false, 0U);
        return;
    }

    me->lent[idx] = true;
    me->p_who     = who;
    me->p_idx     = idx;
    me->p_size    = size;
    me->p_done    = 0U;
    (void)snprintf(me->p_src, sizeof(me->p_src), "%s", src);
    (void)snprintf(me->p_dst, sizeof(me->p_dst), "%s", dst);

    /* The first chunk runs in this step, the rest on the pacer. */
    copy_step(me);
}

/**
 * @brief Answer a STORAGE_TEXT_REQ_SIG.
 *
 * One round trip: KODE_FS_TEXT_MAX bounds a `write` to about a hundred bytes.
 * A failed write calls kdrv_storage_discard(): a truncating create leaves no
 * file, an append keeps what it managed to add. busy() comes first: closing
 * here closes both handles and would end another transfer's read.
 */
static void answer_text_write(Storage * const me, QActive *who, uint8_t op,
                              const char *path, const char *text,
                              uint16_t len, bool append)
{
    if ( busy(me) )
    {
        KLOG_W(TAG, "a transfer is already in flight; write '%s' refused",
              path);
        post_path_rsp(who, op, KODE_DENIED, false, false, 0U);
        return;
    }

    warn_if_card("write", path);
    kode_status_t rc = kdrv_storage_create_path(path, append);

    if ( KODE_OK == rc )
    {
        rc = kdrv_storage_write(text, (size_t)len);
        if ( KODE_OK != rc )
        {
            KLOG_W(TAG, "write '%s' failed", path);
            kdrv_storage_discard();
        }
        else
        {
            /* The close is checked: fclose() is where buffered bytes meet the
             * card, and an unlanded `write` must not answer `>ok`. */
            rc = kdrv_storage_close();
            if ( KODE_OK != rc )
            {
                /* Said, not undone: an append may sit on a file that held good
                 * data, so the file is not removed. */
                KLOG_W(TAG, "write '%s' did not land", path);
            }
        }
    }
    else
    {
        KLOG_W(TAG, "write '%s' could not open", path);
    }
    post_path_rsp(who, op, rc, false, false, 0U);
}

/**
 * @brief Answer a STORAGE_TEXT_REQ_SIG whose `line` names one line to
 *        replace, rather than the whole file.
 *
 * Read the file into buf[0], rebuild it with one line swapped into buf[1],
 * write it to `<path>.tmp`, then rename <path> -> .bak, .tmp -> <path>, and
 * unlink .bak. FatFs f_rename() will not overwrite, and a complete file exists
 * at every instant; on failure the original is put back. A leftover .tmp or
 * .bak from a reset is not swept. One dispatch step, at most 32 KB each way.
 *
 * @param line 1-based. Zero never reaches here - handle_text_req() below is
 *             what tells the two shapes of a STORAGE_TEXT_REQ_SIG apart.
 */
static void answer_text_line_write(Storage * const me, QActive *who,
                                   const char *path, const char *text,
                                   uint16_t len, uint16_t line)
{
    if ( busy(me) )
    {
        KLOG_W(TAG, "a transfer is already in flight; write '%s' refused",
              path);
        post_path_rsp(who, (uint8_t)STORAGE_PATH_WRITE_LINE, KODE_DENIED,
                      false, false, 0U);
        return;
    }

    warn_if_card("write", path);

    uint32_t      size = 0U;
    kode_status_t rc   = kdrv_storage_open_path(path, &size);
    if ( KODE_OK != rc )
    {
        /* KODE_DENIED with size 0 on a path naming a real volume is an
         * empty file: answered as `noline`, like a line past the end.
         * A path naming no volume stays a bad path. */
        if ( (KODE_DENIED == rc) && (0U == size)
            && (kdrv_storage_medium_of(path) != KDRV_MEDIUM_COUNT) )
        {
            KLOG_W(TAG, "write '%s' line=%u: file is empty, no line to "
                        "replace", path, (unsigned)line);
            post_path_rsp(who, (uint8_t)STORAGE_PATH_WRITE_LINE, KODE_DENIED,
                          true, false, 0U);
            return;
        }
        KLOG_W(TAG, "write '%s' line=%u: cannot open", path, (unsigned)line);
        post_path_rsp(who, (uint8_t)STORAGE_PATH_WRITE_LINE, rc, false,
                      false, 0U);
        return;
    }

    if ( (uint32_t)STORAGE_CHUNK < size )
    {
        (void)kdrv_storage_close();
        KLOG_W(TAG, "write '%s' line=%u: %lu bytes is over the %u KB line "
                    "edit ceiling; use file_get, edit, file_put",
              path, (unsigned)line, (unsigned long)size,
              (unsigned)((uint32_t)STORAGE_CHUNK / 1024U));
        post_path_rsp(who, (uint8_t)STORAGE_PATH_WRITE_LINE,
                      KODE_NO_RESOURCES, false, false, size);
        return;
    }

    size_t got = 0U;
    rc = kdrv_storage_read(me->buf[0], (size_t)STORAGE_CHUNK, &got);
    (void)kdrv_storage_close();
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "write '%s' line=%u: read failed", path, (unsigned)line);
        post_path_rsp(who, (uint8_t)STORAGE_PATH_WRITE_LINE, rc, false,
                      false, 0U);
        return;
    }

    /* Rebuild into buf[1], swapping @p text for line @p line. Split on
     * '\n' alone, as cat_step() does, and keep whether the last line
     * had a trailing '\n': no other byte of the file may change. */
    const char *const src      = (const char *)me->buf[0];
    char       *const dst      = (char *)me->buf[1];
    size_t            dst_used = 0U;
    size_t            src_pos  = 0U;
    uint16_t          cur_line = 0U;
    bool              found    = false;

    while ( src_pos < got )
    {
        size_t const       left = got - src_pos;
        const void *const  nl   = memchr(&src[src_pos], '\n', left);
        size_t             seg_len;
        bool               has_nl;

        if ( NULL != nl )
        {
            seg_len = (size_t)((const char *)nl - &src[src_pos]);
            has_nl  = true;
        }
        else
        {
            seg_len = left;
            has_nl  = false;
        }
        ++cur_line;

        const char *put     = &src[src_pos];
        size_t      put_len = seg_len;
        if ( cur_line == line )
        {
            put     = text;
            put_len = (size_t)len;
            found   = true;
        }

        /* A longer replacement can push the rebuilt file past buf[1]
         * (STORAGE_CHUNK): rewrite what fits whole, or refuse, never half. */
        size_t need = put_len;
        if ( has_nl )
        {
            need += 1U;
        }
        if ( (dst_used + need) > (size_t)STORAGE_CHUNK )
        {
            KLOG_W(TAG, "write '%s' line=%u: the replacement makes '%s' too "
                        "large to rewrite in place",
                  path, (unsigned)line, path);
            post_path_rsp(who, (uint8_t)STORAGE_PATH_WRITE_LINE,
                          KODE_NO_RESOURCES, false, false, 0U);
            return;
        }
        (void)memcpy(&dst[dst_used], put, put_len);
        dst_used += put_len;
        if ( has_nl )
        {
            dst[dst_used] = '\n';
            ++dst_used;
        }

        src_pos += seg_len;
        if ( has_nl )
        {
            ++src_pos;
        }
    }

    if ( !found )
    {
        /* `exists` true says @p line named no line in the file, unlike the
         * busy refusal above; `size` carries the line count found. */
        const char *plural = "s";
        if ( 1U == cur_line )
        {
            plural = "";
        }
        KLOG_W(TAG, "write '%s': line=%u is past the end (%u line%s)",
              path, (unsigned)line, (unsigned)cur_line, plural);
        post_path_rsp(who, (uint8_t)STORAGE_PATH_WRITE_LINE, KODE_DENIED,
                      true, false, (uint32_t)cur_line);
        return;
    }

    /* The temporary is @p path with ".tmp" appended: same directory, same
     * volume, since kdrv_storage_rename() refuses a crossing. */
    char      tmp_path[KODE_PATH_MAX];
    int const wrote = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if ( (0 > wrote) || ((size_t)wrote >= sizeof(tmp_path)) )
    {
        KLOG_W(TAG, "write '%s' line=%u: no room for a temporary name",
              path, (unsigned)line);
        post_path_rsp(who, (uint8_t)STORAGE_PATH_WRITE_LINE, KODE_DENIED,
                      false, false, 0U);
        return;
    }

    rc = kdrv_storage_create_path(tmp_path, false);
    if ( KODE_OK == rc )
    {
        rc = kdrv_storage_write(dst, dst_used);
        if ( KODE_OK == rc )
        {
            /* Checked: fclose() is where buffered bytes meet the card, and the
             * original must not be replaced on a write that did not land. */
            rc = kdrv_storage_close();
        }
        if ( KODE_OK != rc )
        {
            /* The temporary was a truncating create, so discard() removes it
             * whole. */
            kdrv_storage_discard();
        }
    }

    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "write '%s' line=%u: could not stage the rewrite",
              path, (unsigned)line);
        post_path_rsp(who, (uint8_t)STORAGE_PATH_WRITE_LINE, rc, false,
                      false, 0U);
        return;
    }

    /* The backup name for step 1 of the dance, same construction as the
     * temporary above and on the same volume for the same reason. */
    char      bak_path[KODE_PATH_MAX];
    int const bwrote = snprintf(bak_path, sizeof(bak_path), "%s.bak", path);
    if ( (0 > bwrote) || ((size_t)bwrote >= sizeof(bak_path)) )
    {
        KLOG_W(TAG, "write '%s' line=%u: no room for a backup name",
              path, (unsigned)line);
        (void)kdrv_storage_unlink(tmp_path);
        post_path_rsp(who, (uint8_t)STORAGE_PATH_WRITE_LINE, KODE_DENIED,
                      false, false, 0U);
        return;
    }

    /* Step 1: move the original aside; FatFs accepts it, the target is
     * absent. On failure the original is untouched; drop the temporary. */
    rc = kdrv_storage_rename(path, bak_path);
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "write '%s' line=%u: could not move the original aside",
              path, (unsigned)line);
        (void)kdrv_storage_unlink(tmp_path);
        post_path_rsp(who, (uint8_t)STORAGE_PATH_WRITE_LINE, KODE_IO_ERROR,
                      false, false, 0U);
        return;
    }

    /* Step 2: put the rewrite in place. On failure roll `<path>.bak` back
     * to @p path and drop the temporary. */
    rc = kdrv_storage_rename(tmp_path, path);
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "write '%s' line=%u: could not put the rewrite in place",
              path, (unsigned)line);
        (void)kdrv_storage_rename(bak_path, path);
        (void)kdrv_storage_unlink(tmp_path);
        post_path_rsp(who, (uint8_t)STORAGE_PATH_WRITE_LINE, KODE_IO_ERROR,
                      false, false, 0U);
        return;
    }

    /* Step 3: the rewrite is in place. A backup that will not go is
     * logged, not reported as a failure. */
    if ( kdrv_storage_unlink(bak_path) != KODE_OK )
    {
        KLOG_W(TAG, "write '%s' line=%u: rewrite done, backup '%s' left behind",
              path, (unsigned)line, bak_path);
    }

    post_path_rsp(who, (uint8_t)STORAGE_PATH_WRITE_LINE, KODE_OK, false,
                  false, 0U);
}

/**
 * @brief Dispatch one StorageTextEvt: a whole-file write when `line` is
 *        zero, a one-line replace otherwise.
 */
static void handle_text_req(Storage * const me, const StorageTextEvt *q)
{
    reply_set(q->wait, q->gen, q->who, q->rsp, NULL);

    if ( 0U == q->line )
    {
        /* `data` when the requester had more bytes than the event carries,
         * `text` otherwise (see StorageTextEvt.data); `len` counts both. */
        const char *bytes = q->text;
        if ( NULL != q->data )
        {
            bytes = (const char *)q->data;
        }
        answer_text_write(me, q->who, (uint8_t)STORAGE_PATH_WRITE, q->path,
                          bytes, q->len, q->append);
    }
    else
    {
        answer_text_line_write(me, q->who, q->path, q->text, q->len,
                               q->line);
    }

    reply_clear();
}

/** @brief Post one line of a `cat`. Bounded, never asserts. */
static void post_cat_line(QActive *who, const char *text, size_t len)
{
    if ( NULL == who )
    {
        return;
    }
    Q_ASSERT(NULL == s_ask_who);   /* an ask in hand is answered by its funnel, once */
    StorageLineEvt *ev = Q_NEW_X(StorageLineEvt, 0U, STORAGE_LINE_SIG);
    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; a cat line is lost");
        return;
    }
    (void)memcpy(ev->text, text, len);
    ev->text[len] = '\0';
    ev->rc  = (uint8_t)KODE_OK;
    ev->eof = false;
    QACTIVE_POST(who, &ev->super, NULL);
}

/**
 * @brief Close a `cat`: the terminator posted to @p who, or the blocked task
 *        that asked woken with @p rc. Bounded, never asserts.
 *
 * Every early refusal in do_path_cat() comes through here; a blocked task's
 * successful `cat` is answered in do_path_cat() itself.
 */
static void post_cat_end(QActive *who, kode_status_t rc)
{
    if ( NULL != s_ask_rsp )
    {
        file_ask_answer(rc, NULL);   /* a refused read writes nothing */
        return;
    }
    if ( NULL != s_waiter )
    {
        wake_waiter(s_waiter, s_waiter_gen, rc, false, false, 0U);
        return;
    }
    if ( NULL == who )
    {
        return;
    }
    Q_ASSERT(NULL == s_ask_who);   /* an ask in hand is answered by its funnel, once */
    StorageLineEvt *ev = Q_NEW_X(StorageLineEvt, 0U, STORAGE_LINE_SIG);
    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; a cat terminator is lost");
        return;
    }
    ev->text[0] = '\0';
    ev->rc  = (uint8_t)rc;
    ev->eof = true;
    QACTIVE_POST(who, &ev->super, NULL);
}

/**
 * @brief Post one line of a `cat`, cut to KODE_FS_CAT_LINE_MAX-1 bytes and
 *        said so in the log when the file's own line was longer.
 *
 * @param text The line as do_path_cat() found it, `\n` not included - this
 *             only ever cuts on byte count, never looks for one itself.
 */
static void emit_cat_line(QActive *who, const char *path, const char *text,
                          size_t len)
{
    size_t use = len;
    bool   cut = false;

    if ( (size_t)KODE_FS_CAT_LINE_MAX <= use )
    {
        use = (size_t)KODE_FS_CAT_LINE_MAX - 1U;
        cut = true;
    }
    post_cat_line(who, text, use);
    if ( cut )
    {
        KLOG_W(TAG, "cat: '%s' has a line longer than %u chars; cut",
              path, (unsigned)((size_t)KODE_FS_CAT_LINE_MAX - 1U));
    }
}

/**
 * @brief Walk the buffer one STORAGE_PATH_CAT already read, one batch of
 *        lines at a time, and either re-arm c_pacer for the rest or close
 *        the round.
 *
 * Runs inline from do_path_cat() for the first batch, then once per
 * STORAGE_CAT_NEXT_SIG, so AO_Gui, AO_Radio and AO_Net below get the CPU.
 * Splits on '\n' alone: CRLF is not translated, a '\r' stays in the record.
 */
static void cat_step(Storage * const me)
{
    const char *const buf     = (const char *)me->buf[me->c_idx];
    uint32_t          emitted = 0U;

    while ( (me->c_pos < me->c_got)
           && ((uint32_t)STORAGE_CAT_LINES_PER_STEP > emitted) )
    {
        size_t const start = me->c_pos;
        size_t const left  = me->c_got - start;
        const void  *nl    = memchr(&buf[start], '\n', left);
        size_t       len;

        if ( NULL != nl )
        {
            len       = (size_t)((const char *)nl - &buf[start]);
            me->c_pos = start + len + 1U;
        }
        else
        {
            /* The buffer stopped mid-line - either the file itself ends
             * there with no trailing newline, or `max` did. Either way
             * there is text to show, and it goes out like any other line. */
            len       = left;
            me->c_pos = me->c_got;
        }
        emit_cat_line(me->c_who, me->c_path, &buf[start], len);
        ++emitted;
    }

    if ( me->c_pos < me->c_got )
    {
        QTimeEvt_armX(&me->c_pacer, STORAGE_WRITE_GAP_TICKS, 0U);
        return;
    }

    if ( ((size_t)me->c_want <= me->c_got) && ((size_t)me->c_size > me->c_got) )
    {
        KLOG_W(TAG, "cat: '%s' stopped at %lu of %lu bytes (max=%lu)",
              me->c_path, (unsigned long)me->c_got, (unsigned long)me->c_size,
              (unsigned long)me->c_max);
    }

    me->lent[me->c_idx] = false;
    QActive * const who = me->c_who;
    me->c_who = NULL;
    post_cat_end(who, KODE_OK);
}

/**
 * @brief Whether @p path is a file that is there and zero bytes long.
 *
 * kdrv_storage_open_path() answers such a file KODE_DENIED, as it answers a
 * bad path or a handle already open; asking the volume tells them apart.
 *
 * @param path Absolute, virtual: "/storage/..." or "/sdcard/...".
 * @return true for an empty file; false for anything else or no answer.
 */
static bool path_is_empty_file(const char *path)
{
    kdrv_stat_t st    = { 0 };
    bool        empty = false;

    if ( KODE_OK == kdrv_storage_stat(path, &st) )
    {
        empty = st.exists && !st.is_dir && (0U == st.size);
    }
    return empty;
}

/**
 * @brief Answer a STORAGE_PATH_CAT request.
 *
 * Opens, reads once (at most KODE_FS_CAT_MAX_CEIL, one buffer) and closes in
 * this dispatch step; cat_step() then paces the lines out. @p offset moves the
 * reader before the read, so a blocked task can walk a file in slices; the
 * console's CAT passes 0. Failures here are logged and told to the asker.
 */
static void do_path_cat(Storage * const me, QActive *who, const char *path,
                        uint32_t offset, uint32_t max)
{
    if ( busy(me) )
    {
        /* Any transfer: the read below ends with kdrv_storage_close(), which
         * closes the writer too. A second cat is covered by c_who. */
        KLOG_W(TAG, "a transfer is already in flight; cat of '%s' refused",
               path);
        post_cat_end(who, KODE_DENIED);
        return;
    }

    uint32_t      size = 0U;
    kode_status_t rc   = kdrv_storage_open_path(path, &size);
    bool          open = (KODE_OK == rc);

    if ( (KODE_DENIED == rc) && path_is_empty_file(path) )
    {
        /* kdrv_storage_open_path() refuses an empty file with the status a
         * busy store answers too. kode_sys.h promises `got` below `cap` at
         * the end of the file, so an empty one reads as zero bytes. */
        rc = KODE_OK;
    }
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "cat: cannot open '%s'", path);
        post_cat_end(who, rc);
        return;
    }

    uint8_t const idx = free_buf(me);
    if ( STORAGE_BUFS <= idx )
    {
        KLOG_W(TAG, "both buffers are out on loan; cat of '%s' refused", path);
        if ( open )
        {
            (void)kdrv_storage_close();
        }
        post_cat_end(who, KODE_DENIED);
        return;
    }

    uint32_t want = max;
    if ( (uint32_t)STORAGE_CHUNK < want )
    {
        want = (uint32_t)STORAGE_CHUNK;
    }

    size_t got = 0U;
    rc = KODE_OK;
    if ( open )
    {
        if ( 0U < offset )
        {
            rc = kdrv_storage_seek(offset);
        }
        if ( KODE_OK == rc )
        {
            rc = kdrv_storage_read(me->buf[idx], (size_t)want, &got);
        }
        (void)kdrv_storage_close();
    }

    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "cat: read of '%s' failed", path);
        post_cat_end(who, rc);
        return;
    }

    if ( NULL != s_ask_rsp )
    {
        /* The asker's lent buffer, written here, in this step, before the
         * answer and never after: `want` is at most its room. */
        if ( 0U < got )
        {
            (void)memcpy(s_ask_buf, me->buf[idx], got);
        }
        s_ask_rsp->got = (uint32_t)got;
        file_ask_answer(KODE_OK, NULL);
        return;
    }
    if ( NULL != s_waiter )
    {
        /* A blocked task asked: the bytes go across raw, not split, cut or
         * NUL-terminated, up to its own ceiling, and the round ends here. */
        size_t take = got;
        if ( (size_t)s_waiter->cap < take )
        {
            take = (size_t)s_waiter->cap;
        }
        if ( 0U < take )
        {
            (void)memcpy(s_waiter->text, me->buf[idx], take);
        }
        s_waiter->got = (uint32_t)take;
        s_waiter->cut = (((uint64_t)offset + (uint64_t)take) < (uint64_t)size);
        if ( s_waiter->cut )
        {
            KLOG_D(TAG, "cat: '%s' handed back %lu of %lu bytes",
                   path, (unsigned long)take, (unsigned long)size);
        }
        wake_waiter(s_waiter, s_waiter_gen, KODE_OK, false, false, 0U);
        return;
    }

    /* Lent for the whole walk, so no read or card test takes the slot
     * while cat walks it; busy() covers the rest via c_who. */
    me->lent[idx] = true;
    me->c_who     = who;
    (void)snprintf(me->c_path, sizeof(me->c_path), "%s", path);
    me->c_idx  = idx;
    me->c_got  = got;
    me->c_pos  = 0U;
    me->c_size = size;
    me->c_want = want;
    me->c_max  = max;

    cat_step(me);
}

/**
 * @brief Dispatch one StoragePathEvt by storage_path_op_t.
 *
 * The default case is unreachable today, but every request is answered, so a
 * corrupted or future op still gets a refusal.
 */
static void handle_path_req(Storage * const me, const StoragePathEvt *q)
{
    reply_set(q->wait, q->gen, q->who, q->rsp, q->buf);

    switch ( (storage_path_op_t)q->op )
    {
        case STORAGE_PATH_STAT: {
            answer_path_stat(q->who, q->op, q->path);
            break;
        }
        case STORAGE_PATH_LIST: {
            do_path_list(q->path, q->offset);
            break;
        }
        case STORAGE_PATH_CAT: {
            do_path_cat(me, q->who, q->path, q->offset, q->max);
            break;
        }
        case STORAGE_PATH_MKDIR: {
            answer_path_mkdir(q->who, q->op, q->path);
            break;
        }
        case STORAGE_PATH_UNLINK: {
            if ( busy(me) )
            {
                KLOG_W(TAG, "a transfer is already in flight; rm of '%s' "
                            "refused", q->path);
                post_path_rsp(q->who, q->op, KODE_DENIED, false, false, 0U);
                break;
            }
            answer_path_unlink(q->who, q->op, q->path);
            break;
        }
        case STORAGE_PATH_RMDIR: {
            if ( busy(me) )
            {
                KLOG_W(TAG, "a transfer is already in flight; rmdir of '%s' "
                            "refused", q->path);
                post_path_rsp(q->who, q->op, KODE_DENIED, false, false, 0U);
                break;
            }
            answer_path_rmdir(q->who, q->op, q->path);
            break;
        }
        case STORAGE_PATH_DELETE: {
            answer_path_delete(me, q->who, q->op, q->path);
            break;
        }
        case STORAGE_PATH_SPACE: {
            answer_path_space(q->who, q->op, q->path);
            break;
        }
        default: {
            KLOG_W(TAG, "unknown path op %u", (unsigned)q->op);
            post_path_rsp(q->who, q->op, KODE_DENIED, false, false, 0U);
            break;
        }
    }

    reply_clear();
}

/**
 * @brief Dispatch one StoragePairEvt by storage_pair_op_t.
 *
 * MOVE is one rename() answered in this step; COPY holds two handles and
 * answers many steps later. The default refuses, as in handle_path_req().
 */
static void handle_pair_req(Storage * const me, const StoragePairEvt *q)
{
    reply_set(q->wait, q->gen, q->who, q->rsp, NULL);

    switch ( (storage_pair_op_t)q->op )
    {
        case STORAGE_PAIR_MOVE: {
            if ( busy(me) )
            {
                KLOG_W(TAG, "a transfer is already in flight; mv of '%s' "
                            "refused", q->src);
                post_path_rsp(q->who, q->op, KODE_DENIED, false, false, 0U);
                break;
            }
            answer_pair_move(q->who, q->op, q->src, q->dst);
            break;
        }
        case STORAGE_PAIR_COPY: {
            answer_pair_copy(me, q->who, q->op, q->src, q->dst);
            break;
        }
        default: {
            KLOG_W(TAG, "unknown pair op %u", (unsigned)q->op);
            post_path_rsp(q->who, q->op, KODE_DENIED, false, false, 0U);
            break;
        }
    }

    reply_clear();
}

/* ---- Receiving a file on the console --------------------------------------
 * `file_put`, on the console's receive protocol shared with AO_AppMgr. This
 * AO POSTS the five control signals to the console instead of publishing
 * them, so AO_Gui does not take the screen over with "Installing...".
 * Bytes arrive as RECV_DATA_SIG from AO_Log's reader task, at most
 * KODE_RECV_CHUNK each, and go straight to fwrite(); only the ack counts to a
 * chunk boundary. Built and verified for the cable; never tried over BLE.
 */

/* Posted, not published. Static, so they cannot fail to allocate:
 * AO_Console has no watchdog during a transfer, so a verdict must ALWAYS
 * be able to arrive. */
static const QEvt s_recv_ready_evt = QEVT_INITIALIZER(RECV_READY_SIG);
static const QEvt s_recv_ack_evt   = QEVT_INITIALIZER(RECV_ACK_SIG);
static const QEvt s_recv_done_evt  = QEVT_INITIALIZER(RECV_DONE_SIG);
static const QEvt s_recv_fail_evt  = QEVT_INITIALIZER(RECV_FAIL_SIG);
static const QEvt s_recv_stall_evt = QEVT_INITIALIZER(RECV_STALL_SIG);

/* Posted to AO_Log, and only ever an abort: a transfer that reached its
 * byte count has left raw mode already. */
static const QEvt s_raw_end_evt    = QEVT_INITIALIZER(CONSOLE_RAW_END_SIG);

/**
 * @brief Put the receive state back to "nothing is arriving".
 *
 * Disarms the deadline and clears @c r_who, which busy(), the stale-chunk
 * check and the idle timer's handler all read.
 */
static void recv_reset(Storage * const me)
{
    (void)QTimeEvt_disarm(&me->r_idle);
    me->r_who  = NULL;
    me->r_open = false;
}

/**
 * @brief Close a transfer: finish or discard the destination, then answer.
 *
 * @param ok      Every byte the host promised arrived, was written, and
 *                checksummed. False if anything at all refused along the way.
 * @param stalled Only meaningful when @p ok is false: distinguishes kcli
 *                error 6 (the host went quiet) from error 5 (everything else).
 *
 * A failed close turns success into failure. A failure removes the
 * destination (a truncating create): kdrv_storage_discard() while the file is
 * open, kdrv_storage_unlink() after a failed close, which forgets the name.
 */
static void recv_close(Storage * const me, bool ok, bool stalled)
{
    QActive * const who = me->r_who;

    if ( me->r_open )
    {
        if ( ok )
        {
            kode_status_t const closed = kdrv_storage_close();
            if ( KODE_OK != closed )
            {
                ok = false;
                KLOG_E(TAG, "file_put '%s' did not land: %lu bytes were "
                            "written and the close failed",
                       me->r_path, (unsigned long)me->r_wire);
                /* Do not claim the stub is removed: the unlink can fail on the
                 * same card. If it does, name the file for a delete by hand. */
                if ( kdrv_storage_unlink(me->r_path) != KODE_OK )
                {
                    KLOG_E(TAG, "file_put '%s': the stub could not be removed "
                                "either - delete it by hand", me->r_path);
                }
            }
        }
        else
        {
            kdrv_storage_discard();
        }
    }

    recv_reset(me);

    if ( NULL == who )
    {
        return;
    }
    if ( ok )
    {
        /* recv_reset() leaves the path and the count to report. */
        KLOG_D(TAG, "file_put '%s': %lu bytes", me->r_path,
               (unsigned long)me->r_wire);
        QACTIVE_POST(who, &s_recv_done_evt, &me->super);
    }
    else if ( stalled )
    {
        QACTIVE_POST(who, &s_recv_stall_evt, &me->super);
    }
    else
    {
        QACTIVE_POST(who, &s_recv_fail_evt, &me->super);
    }
}

/**
 * @brief Refuse a transfer that never started, answering @p who anyway.
 *
 * Every request is answered: the console hands its own watchdog over on
 * `>|ready`, so a silent refusal could leave the command in flight for good.
 */
static void recv_deny(QActive *who)
{
    if ( NULL != who )
    {
        QACTIVE_POST(who, &s_recv_fail_evt, NULL);
    }
}

/**
 * @brief Start a transfer: arm the wire, then wait to be told it is armed.
 *
 * Nothing is opened before the arming: an arming that lands after slow work
 * cannot tell the command line's trailing newline from the payload's first byte.
 */
static void recv_begin(Storage * const me, const StorageRecvEvt *q)
{
    if ( NULL == q->who )
    {
        /* Refused, as in answer_pair_copy(): `r_who` is also the flag that a
         * transfer is running, and without it the writer and the console
         * would stay held until a reset. */
        KLOG_W(TAG, "file_put '%s' has nobody to answer; refused", q->path);
        return;
    }
    if ( busy(me) )
    {
        KLOG_W(TAG, "a transfer is already in flight; file_put of '%s' refused",
               q->path);
        recv_deny(q->who);
        return;
    }
    if ( 0U == q->size )
    {
        /* Unreachable from a prompt (the key spec refuses `size=0`), but a
         * zero-byte transfer would wait for a completion that never fires. */
        KLOG_W(TAG, "file_put '%s' of nothing; refused", q->path);
        recv_deny(q->who);
        return;
    }

    me->r_who      = q->who;
    me->r_total    = q->size;
    me->r_want     = q->crc32;
    me->r_crc      = KHAL_CRC32_INIT;
    me->r_wire     = 0U;
    me->r_owed_ack = 0U;
    me->r_ok       = true;
    me->r_open     = false;
    (void)snprintf(me->r_path, sizeof(me->r_path), "%s", q->path);

    ConsoleRawReqEvt *raw = Q_NEW_X(ConsoleRawReqEvt, 0U, CONSOLE_RAW_REQ_SIG);
    if ( NULL == raw )
    {
        KLOG_E(TAG, "file_put '%s' failed: out of events arming the console",
               me->r_path);
        recv_reset(me);
        recv_deny(q->who);
        return;
    }
    raw->size = q->size;
    raw->who  = &me->super;
    raw->src  = q->src;
    QACTIVE_POST(AO_Log, &raw->super, &me->super);

    QTimeEvt_armX(&me->r_idle, STORAGE_RECV_LONGSTOP_TICKS, 0U);
}

/**
 * @brief AO_Log has answered whether the wire is ours; open and say ready.
 *
 * The host waits for `>|ready`, so the destination is opened here, and one
 * that will not open is refused before the host hears anything; raw mode goes
 * straight back.
 */
static void recv_armed(Storage * const me, uint8_t status)
{
    if ( NULL == me->r_who )
    {
        /* A stale answer to a transfer that already ended - its own long-stop
         * fired, or a NULL-`who` request was refused before this arrived. */
        return;
    }
    if ( (uint8_t)KODE_OK != status )
    {
        /* AO_Log refused to arm, which today means raw mode was somehow
         * already on. Nothing is open and nothing was streamed. */
        KLOG_E(TAG, "file_put '%s' failed: the console would not go raw",
               me->r_path);
        recv_close(me, false, false);
        return;
    }

    warn_if_card("file_put", me->r_path);

    /* Truncating, never appending: `file_put` replaces what is at the path,
     * and recv_close()'s discard then removes the file whole. */
    if ( kdrv_storage_create_path(me->r_path, false) != KODE_OK )
    {
        KLOG_E(TAG, "file_put '%s' failed: the destination would not open",
               me->r_path);
        QACTIVE_POST(AO_Log, &s_raw_end_evt, &me->super);
        recv_close(me, false, false);
        return;
    }
    me->r_open = true;

    /* Tighten the long-stop to the real between-chunk deadline. rearm, not
     * armX - armX asserts on a timer that is already ticking (Q_REQUIRE 450
     * in qf_time.c), and this one always is by now. */
    (void)QTimeEvt_rearm(&me->r_idle, STORAGE_RECV_IDLE_TICKS);

    /* The host may start streaming. This is `>|ready`. */
    QACTIVE_POST(me->r_who, &s_recv_ready_evt, &me->super);
}

/**
 * @brief One piece of the arriving payload: check it, write it, ack it.
 *
 * A failed write stops the WRITING and nothing else: the host still owes the
 * full count, so draining and acks carry on. @c r_wire counts what left the
 * wire, so the completion check still fires and reports the write error.
 */
static void recv_data(Storage * const me, const RecvDataEvt *d)
{
    if ( NULL == me->r_who )
    {
        return;                       /* nothing arriving; a stale chunk */
    }

    /* Still hearing from the host: push the deadline out. */
    (void)QTimeEvt_rearm(&me->r_idle, STORAGE_RECV_IDLE_TICKS);

    uint16_t off = 0U;
    while ( off < d->len )
    {
        uint32_t const owed = me->r_total - me->r_wire;
        if ( 0U == owed )
        {
            /* The host talked past its own `size`: unreachable, since AO_Log stops
             * at the count, but the CRC covers only the promised bytes. */
            KLOG_W(TAG, "file_put '%s': %u bytes past the promised %lu; "
                        "ignored", me->r_path, (unsigned)(d->len - off),
                   (unsigned long)me->r_total);
            break;
        }

        uint32_t const room = (uint32_t)KODE_RECV_CHUNK - me->r_owed_ack;
        uint32_t       take = (uint32_t)(d->len - off);
        if ( room < take )
        {
            take = room;
        }
        if ( owed < take )
        {
            take = owed;
        }

        /* The CRC covers exactly what was accepted, the prefix the host
         * described. */
        me->r_crc = khal_crc32(me->r_crc, &d->data[off], (size_t)take);

        if ( me->r_ok
            && (kdrv_storage_write(&d->data[off], (size_t)take) != KODE_OK) )
        {
            me->r_ok = false;
            KLOG_E(TAG, "file_put '%s': write failed at %lu/%lu bytes; "
                        "draining the rest of the transfer",
                   me->r_path, (unsigned long)me->r_wire,
                   (unsigned long)me->r_total);
        }

        me->r_owed_ack += take;
        me->r_wire     += take;
        off            += (uint16_t)take;

        if ( (uint32_t)KODE_RECV_CHUNK <= me->r_owed_ack )
        {
            /* The transfer's whole flow control: the host waits for this before
             * the next chunk. A final partial chunk earns none. */
            me->r_owed_ack = 0U;
            QACTIVE_POST(me->r_who, &s_recv_ack_evt, &me->super);
        }
    }

    if ( me->r_wire < me->r_total )
    {
        return;
    }

    bool const crc_ok = (me->r_crc == me->r_want);
    if ( me->r_ok && !crc_ok )
    {
        KLOG_E(TAG, "file_put '%s' failed: CRC mismatch (got 0x%08lx, "
                    "wanted 0x%08lx)", me->r_path,
               (unsigned long)me->r_crc, (unsigned long)me->r_want);
    }
    recv_close(me, me->r_ok && crc_ok, false);
}

/** @brief The between-chunk deadline expired: the host is presumed gone. */
static void recv_stall(Storage * const me)
{
    if ( NULL == me->r_who )
    {
        return;                       /* a stale tick; nothing is arriving */
    }
    KLOG_E(TAG, "file_put '%s' failed: no data for %u ms at %lu/%lu bytes; "
                "the host is presumed gone",
           me->r_path, (unsigned)KODE_RECV_IDLE_MS,
           (unsigned long)me->r_wire, (unsigned long)me->r_total);

    /* The reader task may still be sitting in raw mode waiting for bytes that
     * are never coming; a transfer that died is the one that has to say so. */
    QACTIVE_POST(AO_Log, &s_raw_end_evt, &me->super);
    recv_close(me, false, true);
}

/* ---- Writing ------------------------------------------------------------- */

/**
 * @brief Close the write, tell the writer how it went, and free the handle.
 *
 * A failed close demotes the whole write: fclose() is the first flush of
 * hundreds of kilobytes (`screenshot dst=file`), and KODE_OK over bytes that
 * never reached the card is the counterfeit-card failure. The stub is left
 * where it fell; the status tells the writer.
 */
static void write_finish(Storage * const me, kode_status_t st)
{
    (void)QTimeEvt_disarm(&me->w_pacer);
    if ( KODE_OK == st )
    {
        st = kdrv_storage_close();
        if ( KODE_OK != st )
        {
            KLOG_E(TAG, "'%s' did not land: %lu bytes were written and the "
                        "close failed", me->w_name, (unsigned long)me->w_done);
        }
    }
    else
    {
        /* Already discarded on the retry path; harmless a second time, and
         * it is what closes the handle when the failure came from elsewhere. */
        kdrv_storage_discard();
    }

    StorageWriteRspEvt *rsp = Q_NEW_X(StorageWriteRspEvt, 0U,
                                      STORAGE_WRITE_RSP_SIG);
    if ( NULL != rsp )
    {
        rsp->written = me->w_done;
        rsp->status  = (uint8_t)st;
        rsp->medium  = me->w_medium;
        QACTIVE_POST(me->writer, &rsp->super, &me->super);
    }
    else
    {
        /* The file is written either way; what is lost is the news of it.
         * Say so here, because the writer is about to time out instead. */
        KLOG_E(TAG, "out of events; the write result was lost");
    }

    me->writer = NULL;
    me->w_data = NULL;
    me->w_len  = 0U;
    me->w_done = 0U;
}

/**
 * @brief Open the file a whole-buffer write goes to, in either vocabulary.
 *
 * A name starting with '/' is a virtual path, opened only on the volume it
 * names, which @p first must be. A bare name goes to kdrv_storage_create(),
 * which places it on @p first and falls over to the other medium.
 *
 * @param name   The bare name or the absolute virtual path.
 * @param first  The medium to try first (the only one, for a path).
 * @param medium Receives the medium the file was opened on.
 * @return KODE_OK with the write handle open, or the refusal.
 */
static kode_status_t write_open(const char *name, kdrv_medium_t first,
                                kdrv_medium_t *medium)
{
    if ( '/' != name[0] )
    {
        return kdrv_storage_create(name, first, medium);
    }
    kdrv_medium_t const m = kdrv_storage_medium_of(name);
    if ( (KDRV_MEDIUM_COUNT <= m) || (m != first) )
    {
        return KODE_ERROR;
    }
    *medium = m;
    return kdrv_storage_create_path(name, false);
}

/** @brief Put one chunk on the medium and arm the next, or finish. */
static void write_chunk(Storage * const me)
{
    if ( NULL == me->writer )
    {
        return;                       /* nothing open; a stale tick */
    }
    uint32_t const left = me->w_len - me->w_done;
    uint32_t const now  = ((uint32_t)STORAGE_CHUNK < left)
                        ? (uint32_t)STORAGE_CHUNK : left;

    kode_status_t const st = kdrv_storage_write(&me->w_data[me->w_done],
                                                (size_t)now);
    if ( KODE_OK != st )
    {
        KLOG_W(TAG, "%s failed at %lu/%lu bytes",
               ((uint8_t)KDRV_MEDIUM_SD == me->w_medium) ? "sd write"
                                                         : "fs write",
               (unsigned long)me->w_done, (unsigned long)me->w_len);
        /* Undo the stub before anything else: a name in the listing that
         * promises bytes it does not have is worse than no file at all. */
        kdrv_storage_discard();

        /* One failure earns one retry on the other medium (a full internal
         * partition is the case); the payload is still whole in memory. */
        kdrv_medium_t next  = KDRV_MEDIUM_FS;
        kdrv_medium_t other = KDRV_MEDIUM_FS;
        if ( (uint8_t)KDRV_MEDIUM_FS == me->w_medium )
        {
            other = KDRV_MEDIUM_SD;
        }
        /* Not onto the card while a call to it is out: opening a file there
         * may mount it, and the worker is doing that. */
        if ( !me->w_retried
            && !((KDRV_MEDIUM_SD == other) && me->sdj.busy)
            && (write_open(me->w_name, other, &next) == KODE_OK) )
        {
            KLOG_W(TAG, "retrying on the %s",
                   (KDRV_MEDIUM_SD == next) ? "card" : "internal partition");
            me->w_retried = true;
            me->w_medium  = (uint8_t)next;
            me->w_done    = 0U;
            QTimeEvt_armX(&me->w_pacer, STORAGE_WRITE_GAP_TICKS, 0U);
        }
        else
        {
            KLOG_E(TAG, "nowhere left to write '%s'", me->w_name);
            write_finish(me, st);
        }
        return;
    }
    me->w_done += now;

    if ( me->w_done >= me->w_len )
    {
        write_finish(me, KODE_OK);
    }
    else
    {
        QTimeEvt_armX(&me->w_pacer, STORAGE_WRITE_GAP_TICKS, 0U);
    }
}

/** @brief Start a write, or refuse it because the one handle is taken. */
static void write_begin(Storage * const me, StorageWriteEvt const * const req)
{
    kdrv_medium_t medium = KDRV_MEDIUM_FS;
    kdrv_medium_t first  = KDRV_MEDIUM_FS;
    kode_status_t st     = KODE_DENIED;

    if ( busy(me) )
    {
        KLOG_W(TAG, "a file is already open; '%s' refused", req->name);
    }
    else if ( (NULL == req->data) || (0U == req->len) )
    {
        KLOG_W(TAG, "nothing to write to '%s'", req->name);
    }
    else
    {
        /* A bare name goes to the internal partition first: see
         * kdrv_storage_create(). A path goes where it says. */
        if ( '/' == req->name[0] )
        {
            first = kdrv_storage_medium_of(req->name);
        }
        st = write_open(req->name, first, &medium);
    }

    if ( KODE_OK != st )
    {
        StorageWriteRspEvt *rsp = Q_NEW_X(StorageWriteRspEvt, 0U,
                                          STORAGE_WRITE_RSP_SIG);
        if ( NULL != rsp )
        {
            rsp->written = 0U;
            rsp->status  = (uint8_t)st;
            rsp->medium  = (uint8_t)medium;
            QACTIVE_POST(req->who, &rsp->super, &me->super);
        }
        return;
    }

    me->writer   = req->who;
    (void)snprintf(me->w_name, sizeof(me->w_name), "%s", req->name);
    me->w_data   = (const uint8_t *)req->data;
    me->w_len    = req->len;
    me->w_done   = 0U;
    me->w_medium  = (uint8_t)medium;
    me->w_retried = false;
    write_chunk(me);
}
/* ---- The card's worker ----------------------------------------------------
 * One call to the card is out at a time: the AO fills `sdj` and submits, the
 * worker makes the kdrv call, and the done event brings the AO to
 * sd_job_complete(). Past the deadline the waiter is told it failed, but the
 * guard stays up until the call returns: the worker still holds `sdj` and
 * the PSRAM buffer.
 */

static void sd_job_complete(Storage * const me, kode_status_t rc,
                            uint32_t value);
static void sd_script_overrun(Storage * const me);
static void sd_test_finish(Storage * const me);
static bool sd_test_restore_width(Storage * const me, SdThen then);

/**
 * @brief Whether a call was made for the card script.
 * @param then What the call is for.
 * @return true for the script's calls; false for a command's.
 */
static bool sd_then_is_script(SdThen then)
{
    return (SD_THEN_SCRIPT <= then) && (SD_THEN_NONE != then);
}

/**
 * @brief The call as the worker makes it.
 *
 * Runs on the worker's task and touches only its SdJob. Each case is one call
 * into the SD driver or the card's FAT volume, never the internal flash (the
 * stack is in PSRAM). The word handed back is the microseconds a sector
 * transfer took, or the megabytes of a new volume.
 *
 * @param arg   The SdJob.
 * @param value Receives that word.
 * @return What the driver returned.
 */
static kode_status_t sd_job_run(void *arg, uint32_t *value)
{
    /* cppcheck-suppress misra-c2012-11.5 ; arg is the SdJob that was submitted */
    SdJob * const j = (SdJob *)arg;
    kode_status_t rc = KODE_OK;
    uint32_t      v  = 0U;

    switch ( j->op )
    {
        case SD_OP_MOUNT: {
            if ( kdrv_storage_mount(KDRV_MEDIUM_SD) == NULL )
            {
                rc = KODE_ERROR;
            }
            break;
        }
        case SD_OP_UNMOUNT: {
            kdrv_storage_unmount(KDRV_MEDIUM_SD);
            break;
        }
        case SD_OP_REMOUNT: {
            kdrv_storage_unmount(KDRV_MEDIUM_SD);
            if ( kdrv_storage_mount(KDRV_MEDIUM_SD) == NULL )
            {
                rc = KODE_ERROR;
            }
            break;
        }
        case SD_OP_READ: {
            uint64_t const t0 = khal_time_us();

            rc = kdrv_sd_read_sectors(j->first, j->count, j->buf);
            v  = (uint32_t)(khal_time_us() - t0);
            break;
        }
        case SD_OP_WRITE: {
            uint64_t const t0 = khal_time_us();

            rc = kdrv_sd_write_sectors(j->first, j->count, j->buf);
            v  = (uint32_t)(khal_time_us() - t0);
            break;
        }
        case SD_OP_FORMAT: {
            int32_t total = 0;

            j->sized = false;
            rc = kdrv_sd_format();
            if ( (KODE_OK == rc)
                && (kdrv_sd_get(KDRV_SD_TOTAL_MB, &total) == KODE_OK) )
            {
                j->sized = true;
                v        = (uint32_t)total;
            }
            break;
        }
        default: {
            rc = KODE_DENIED;
            break;
        }
    }
    *value = v;
    return rc;
}

/**
 * @brief How long the AO waits for @p op.
 * @param op The call.
 * @return Milliseconds.
 */
static uint32_t sd_job_deadline_ms(SdOp op)
{
    uint32_t ms = STORAGE_JOB_MOUNT_MS;

    switch ( op )
    {
        case SD_OP_READ:
        case SD_OP_WRITE: {
            ms = STORAGE_JOB_SECTORS_MS;
            break;
        }
        case SD_OP_FORMAT: {
            ms = STORAGE_JOB_FORMAT_MS;
            break;
        }
        default: {
            break;
        }
    }
    return ms;
}

/**
 * @brief Hand the worker a call, with its deadline, and raise the guard.
 *
 * Only while no call is out. The caller has written what the call carries
 * into @c sdj already (and checked the guard first, because those fields are
 * the worker's while a call is out).
 *
 * @param me   The AO.
 * @param op   The call.
 * @param then What it is for.
 * @return true when the worker has it; false when a call is out, the worker
 *         did not start or it has no room, and nothing changed.
 */
static bool sd_job_submit(Storage * const me, SdOp op, SdThen then)
{
    uint16_t seq = 0U;

    if ( (!me->worker_up) || me->sdj.busy )
    {
        return false;
    }
    me->sdj.op   = op;
    me->sdj.then = then;
    if ( kqp_worker_submit(&me->worker, &sd_job_run, &me->sdj,
                          STORAGE_SDJOB_DONE_SIG, &seq) != KODE_OK )
    {
        return false;
    }
    me->sdj.seq     = seq;
    me->sdj.busy    = true;
    me->sdj.expired = false;
    QTimeEvt_armX(&me->sdj_dl,
                  (QTimeEvtCtr)KQP_TICKS_PER_MS(sd_job_deadline_ms(op)), 0U);
    return true;
}

/**
 * @brief Submit a raw sector transfer.
 *
 * @param me    The AO.
 * @param op    SD_OP_READ or SD_OP_WRITE.
 * @param first First sector.
 * @param count How many.
 * @param buf   The AO's buffer.
 * @param then  What it is for.
 * @return As sd_job_submit().
 */
static bool sd_job_sectors(Storage * const me, SdOp op, uint32_t first,
                           uint32_t count, uint8_t *buf, SdThen then)
{
    if ( me->sdj.busy )
    {
        return false;
    }
    me->sdj.first = first;
    me->sdj.count = count;
    me->sdj.buf   = buf;
    return sd_job_submit(me, op, then);
}

/**
 * @brief The worker's call returned.
 *
 * A done event with the wrong sequence number, or with no call out, is
 * dropped. A call that returns after its deadline only lowers the guard: its
 * waiter was told already.
 *
 * @param me The AO.
 * @param e  The done event.
 */
static void sd_job_done(Storage * const me, KqpWorkerDoneEvt const * const e)
{
    kode_status_t const rc = (kode_status_t)e->status;
    bool                late;

    if ( (!me->sdj.busy) || (e->seq != me->sdj.seq) )
    {
        KLOG_D(TAG, "worker answer seq %u is not the call's (%u); dropped",
               (unsigned)e->seq, (unsigned)me->sdj.seq);
        return;
    }
    late = me->sdj.expired;
    if ( (!late) && kqp_timer_stale(&me->sdj_dl, true) )
    {
        me->sdj_stale += 1U;
    }
    me->sdj.busy    = false;
    me->sdj.expired = false;
    if ( late )
    {
        KLOG_W(TAG, "the call to the card returned after its deadline "
                    "(rc=%d); its answer is dropped", (int)rc);
        if ( me->width_owed )
        {
            /* The script that was running when it overran could not put the
             * slot back on the board's width with the call still out; it can
             * now. Asked directly, so a later script starts from it. */
            me->width_owed = false;
            if ( !sd_test_restore_width(me, SD_THEN_NONE) )
            {
                KLOG_D(TAG, "the slot is on the board's width already");
            }
        }
        return;
    }
    sd_job_complete(me, rc, e->value);
}

/**
 * @brief The call has been out for its deadline.
 *
 * Whoever waits for it is told it failed, and the guard stays up: the worker
 * still holds @c sdj, and the next call must not be written over it.
 *
 * @param me The AO.
 */
static void sd_job_deadline(Storage * const me)
{
    if ( 0U < me->sdj_stale )
    {
        me->sdj_stale -= 1U;
        return;
    }
    if ( me->sdj.busy && (!me->sdj.expired) )
    {
        me->sdj.expired = true;
        KLOG_E(TAG, "a call to the card did not return in time; its "
                    "requester is told it failed");
        if ( (0U != me->t_phase) && (SD_THEN_VERDICT == me->sdj.then) )
        {
            /* The remount that hands the width back overran: every phase has
             * been judged already, so the verdict stands as it is and no
             * record is added for a phase that passed. The width is owed. */
            me->width_owed = true;
            sd_test_finish(me);
        }
        else if ( (0U != me->t_phase) && sd_then_is_script(me->sdj.then) )
        {
            sd_script_overrun(me);
        }
        else if ( SD_THEN_NONE == me->sdj.then )
        {
            /* The width a script owed was being put back and that overran
             * too: still owed, and asked for again when this one returns. */
            me->width_owed = true;
        }
        else
        {
            sd_job_complete(me, KODE_TIMEOUT, 0U);
        }
    }
}

/* ---- The card test --------------------------------------------------------
 * This row judges THE BOARD (card supply, CLK, CMD, four data lines at
 * 40 MHz), not the card: some units shipped with counterfeit microSDs. The
 * data lines are proved by READING a megabyte (SDMMC checks a CRC per line
 * per block), then again on one line: a healthy board is markedly faster on
 * four. A file is then written and read back for the write speed, the one
 * part the card can veto: a failed write after good reads is the card's fault.
 * One 32 KB chunk per step on its own pacer; rates time the transfers only.
 */

#define SD_TEST_FILE     ".kodeos_sdtest"
#define SD_TEST_BYTES    (KODE_SD_TEST_KB * 1024U)
#define SD_TEST_CHUNKS   (SD_TEST_BYTES / STORAGE_CHUNK)

/** @brief How much of the card the filesystem must reach after a format. */
#define SD_TEST_MIN_PCT   95U
/** @brief Smallest real size the alias scan looks for: one gigabyte. */
#define SD_TEST_ALIAS_MIN_SECTORS (2U * 1024U * 1024U)

/**
 * @brief Fill @p buf with chunk @p idx of the run seeded @p seed.
 *
 * Regenerable from the seed and the index alone, so the read side keeps no copy.
 */
static void fill_pattern(uint8_t *buf, size_t len, uint32_t seed, uint32_t idx)
{
    uint32_t x = seed ^ (idx * 0x9E3779B9U);

    if ( 0U == x )
    {
        x = 1U;                      /* xorshift has one fixed point */
    }
    for ( size_t i = 0U; (i + 4U) <= len; i += 4U )
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        (void)memcpy(&buf[i], &x, sizeof(x));
    }
}

/** @brief Publish this phase's record. @param kb_s 0 when nothing was timed. */
static void sd_test_publish(Storage * const me, uint32_t kb_s)
{
    StorageTestEvt *ev = Q_NEW_X(StorageTestEvt, 0U, STORAGE_TEST_SIG);

    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; a card test record is lost");
        return;
    }
    (void)memset(ev->name, 0, sizeof(ev->name));

    int32_t width = 0;
    int32_t freq  = 0;
    if ( !me->sdj.expired )
    {
        /* Not while the worker is still inside the driver: the identity it
         * would read is the one that call may be rewriting. */
        (void)kdrv_sd_name(ev->name, sizeof(ev->name));
        (void)kdrv_sd_get(KDRV_SD_BUS_WIDTH, &width);
        (void)kdrv_sd_get(KDRV_SD_FREQ_KHZ, &freq);
    }

    ev->phase = me->t_phase;
    if ( me->t_verify )
    {
        ev->total = SD_VERIFY_STEPS;
    }
    else
    {
        ev->total = SD_TEST_STEPS;
    }
    ev->ok          = me->t_ok;
    ev->width       = (uint8_t)width;
    ev->freq_khz    = (uint32_t)freq;
    ev->capacity_mb = me->t_capacity_mb;
    ev->total_mb    = me->t_total_mb;
    ev->kb_s        = kb_s;
    QACTIVE_PUBLISH(&ev->super, &me->super);
}

/**
 * @brief Put the slot back on the board's own width, whatever the run did.
 *
 * Asking costs nothing. A card that is on another width is remounted, and
 * that is a call to the card.
 *
 * @param me   The AO.
 * @param then What to go on with when the remount returns.
 * @return true when a remount is out and @p then runs when it returns; false
 *         when the slot was already on the board's width or the call could not
 *         be made, and the caller goes on at once.
 */
static bool sd_test_restore_width(Storage * const me, SdThen then)
{
    int32_t width = 0;

    (void)kdrv_sd_set(KDRV_SD_WIDTH_REQ, (int32_t)KBSP_SD_WIDTH);
    (void)kdrv_sd_get(KDRV_SD_BUS_WIDTH, &width);
    if ( (int32_t)KBSP_SD_WIDTH == width )
    {
        return false;
    }
    if ( sd_job_submit(me, SD_OP_REMOUNT, then) )
    {
        return true;
    }
    KLOG_W(TAG, "test sd: the slot could not be put back on the board's "
                "width (no call to the card could be made)");
    if ( me->sdj.busy )
    {
        /* A call that overran is still out: the remount waits for it. */
        me->width_owed = true;
    }
    return false;
}

/** @brief Publish the row's verdict. The console closes `test` on this. */
static void sd_test_result(Storage * const me, bool ok)
{
    TestResultEvt *ev = Q_NEW_X(TestResultEvt, 0U, TEST_RESULT_SIG);

    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; the card test's verdict was lost");
        return;
    }
    (void)snprintf(ev->name, sizeof(ev->name), "%s", "sd");
    ev->ok = ok;
    ev->origin = TEST_ORIGIN_AUTOTEST;
    QACTIVE_PUBLISH(&ev->super, &me->super);
}

/** @brief Publish `sd_verify`'s verdict. Named apart so `test` ignores it. */
static void sd_verify_result(Storage * const me, bool ok)
{
    TestResultEvt *ev = Q_NEW_X(TestResultEvt, 0U, TEST_RESULT_SIG);

    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; sd_verify's verdict was lost");
        return;
    }
    (void)snprintf(ev->name, sizeof(ev->name), "%s", "sd_verify");
    ev->ok = ok;
    ev->origin = TEST_ORIGIN_AUTOTEST;
    QACTIVE_PUBLISH(&ev->super, &me->super);
}

/**
 * @brief Let go of the run's state and say what the SLOT is worth.
 *
 * The verdict is the two board phases and nothing else. A card that reads
 * perfectly and then refuses to be written to is a bad card in a good slot,
 * and saying so is the whole point of separating them.
 */
static void sd_test_finish(Storage * const me)
{
    bool const verify = me->t_verify;
    bool       ok     = !me->t_board_failed;

    if ( verify )
    {
        /* Nothing in `sd_verify` is the board's, so its verdict is simply
         * whether every phase passed. */
        ok = !me->t_card_failed;
    }
    me->t_phase  = 0U;
    me->t_verify = false;

    if ( !verify && ok && me->t_card_failed )
    {
        KLOG_W(TAG, "test sd: the slot is good; this CARD refused to be "
                    "written to, which is what the counterfeit ones do");
    }
    if ( verify )
    {
        sd_verify_result(me, ok);
        return;
    }
    sd_test_result(me, ok);
}

/**
 * @brief A call to the card overran its deadline in the middle of a script:
 *        end the run here with a failed verdict.
 *
 * Not fail-and-advance: the worker is still inside this call, so every later
 * call would be refused. The phase is published as failed, the verdict follows,
 * and the width is put back when the call returns (@c width_owed).
 *
 * @param me The AO.
 */
static void sd_script_overrun(Storage * const me)
{
    KLOG_E(TAG, "test sd: FAIL - a call to the card did not return in time; "
                "the run ends here");
    (void)QTimeEvt_disarm(&me->t_pacer);
    me->t_ok = false;
    if ( me->t_verify )
    {
        me->t_card_failed = true;
    }
    else
    {
        me->t_board_failed = true;
    }
    sd_test_publish(me, 0U);
    me->width_owed = true;
    sd_test_finish(me);
}

/**
 * @brief End the run: hand the slot back on the board's own width, then say
 *        what it is worth.
 *
 * The run stays open - and so does everything that checks it - until a remount
 * that puts the width back has returned.
 */
static void sd_test_verdict(Storage * const me)
{
    (void)QTimeEvt_disarm(&me->t_pacer);
    if ( sd_test_restore_width(me, SD_THEN_VERDICT) )
    {
        return;
    }
    sd_test_finish(me);
}

/** @brief Close this phase and start the next, or finish the run. */
static void sd_test_advance(Storage * const me, uint32_t kb_s)
{
    sd_test_publish(me, kb_s);
    if ( !me->t_ok )
    {
        /* Which tally a failure lands in is what separates a bad slot from a
         * bad card: in the row, the first two phases are the board's and the
         * rest are not; in `sd_verify` nothing is. */
        if ( !me->t_verify && (SD_TEST_READ4 >= me->t_phase) )
        {
            me->t_board_failed = true;
        }
        else
        {
            me->t_card_failed = true;
        }
    }
    uint8_t last = SD_TEST_VERIFY;
    if ( me->t_verify )
    {
        last = SD_VERIFY_FORMAT;
    }
    if ( me->t_phase >= last )
    {
        sd_test_verdict(me);
        return;
    }
    ++me->t_phase;
    me->t_step    = 0U;
    me->t_ok      = true;
    me->t_busy_us = 0U;
    QTimeEvt_armX(&me->t_pacer, STORAGE_WRITE_GAP_TICKS, 0U);
}

/** @brief Fail this phase, naming what failed, and go on to the next. */
static void sd_test_fail(Storage * const me, const char *why)
{
    KLOG_E(TAG, "test sd: FAIL - %s", why);
    /* The card is somebody's: the megabyte the row put on it does not stay
     * there because the row failed. Every failure of the two file phases
     * has closed or discarded the file by now; a file that never came to be
     * is not there to remove, and that refusal is nothing to report. */
    if ( (SD_TEST_WRITE <= me->t_phase) && (!me->sdj.busy) )
    {
        (void)kdrv_storage_remove(SD_TEST_FILE, KDRV_MEDIUM_SD);
    }
    me->t_ok = false;
    sd_test_advance(me, 0U);
}

/**
 * @brief Fail this phase and end the run.
 *
 * Only the identify uses this: without a card the later phases are impossible.
 * Every other phase runs whatever the one before did.
 */
static void sd_test_abort(Storage * const me, const char *why)
{
    KLOG_E(TAG, "sd: FAIL - %s", why);
    me->t_ok           = false;
    me->t_board_failed = true;
    me->t_card_failed  = true;
    sd_test_publish(me, 0U);
    sd_test_verdict(me);
}

/** @brief The rate a phase moved SD_TEST_BYTES at, in KB per second. */
static uint32_t sd_test_rate(uint64_t busy_us)
{
    if ( 0U == busy_us )
    {
        return 0U;      /* a clock that did not move says nothing about speed */
    }
    return (uint32_t)(((uint64_t)KODE_SD_TEST_KB * 1000000ULL) / busy_us);
}

/* ---- Phases 2 and 3: the data lines, at four bits and at one ------------- */

/**
 * @brief Hand the worker this step's chunk of raw sectors to read.
 *
 * The content is never looked at: SDMMC checks a CRC per data line, so a
 * chunk without an error is one every line carried. The worker times it.
 *
 * @param me   The AO.
 * @param then What to go on with when the chunk is in.
 * @return true when the call is out.
 */
static bool sd_test_read_chunk(Storage * const me, SdThen then)
{
    uint32_t const first = me->t_read_first + (me->t_step * me->t_per_chunk);

    return sd_job_sectors(me, SD_OP_READ, first, me->t_per_chunk, me->buf[1],
                          then);
}

/** @brief One step of the 4-bit read: the phase the four data lines live in. */
static void sd_test_read4_step(Storage * const me)
{
    if ( !sd_test_read_chunk(me, SD_THEN_READ4) )
    {
        sd_test_fail(me, "the card could not be asked for the four-line read");
    }
}

/** @brief The chunk of the 4-bit read is in. */
static void sd_test_read4_done(Storage * const me, kode_status_t rc,
                               uint32_t us)
{
    me->t_busy_us += us;
    if ( KODE_OK != rc )
    {
        sd_test_fail(me, "the card would not read at four data lines; one of "
                         "D0..D3 is not carrying");
        return;
    }
    ++me->t_step;
    if ( SD_TEST_CHUNKS <= me->t_step )
    {
        uint32_t const kb_s = sd_test_rate(me->t_busy_us);
        me->t_read4_kb_s = kb_s;
        KLOG_N(TAG, "test sd: %uKB read on four data lines at %luKB/s",
               (unsigned)KODE_SD_TEST_KB, (unsigned long)kb_s);
        sd_test_advance(me, kb_s);
        return;
    }
    QTimeEvt_armX(&me->t_pacer, STORAGE_WRITE_GAP_TICKS, 0U);
}

/** @brief Ask for the next chunk of the 1-bit read. */
static void sd_test_read1_chunk(Storage * const me)
{
    if ( !sd_test_read_chunk(me, SD_THEN_READ1) )
    {
        sd_test_fail(me, "the card could not be asked for the one-line read");
    }
}

/**
 * @brief One step of the 1-bit read, which exists to be compared with the one
 *        above.
 *
 * A slot with dead extra lines still negotiates four and passes the CRC by
 * retrying, but it cannot be faster than one line: the comparison sees it.
 */
static void sd_test_read1_step(Storage * const me)
{
    if ( 0U == me->t_step )
    {
        /* The knob only names the next mount's width; the remount is the
         * call. */
        (void)kdrv_sd_set(KDRV_SD_WIDTH_REQ, 1);
        if ( !sd_job_submit(me, SD_OP_REMOUNT, SD_THEN_READ1_MOUNT) )
        {
            sd_test_fail(me, "the card could not be asked to come back on "
                             "one data line");
        }
        return;
    }
    sd_test_read1_chunk(me);
}

/** @brief The card is back, on the one line it was asked for (or not). */
static void sd_test_read1_mounted(Storage * const me, kode_status_t rc)
{
    int32_t width = 0;

    if ( KODE_OK != rc )
    {
        sd_test_fail(me, "the card would not come back on one data line");
        return;
    }
    (void)kdrv_sd_get(KDRV_SD_BUS_WIDTH, &width);
    if ( 1 != width )
    {
        sd_test_fail(me, "the slot would not be held to one data line");
        return;
    }
    sd_test_read1_chunk(me);
}

/** @brief The chunk of the 1-bit read is in. */
static void sd_test_read1_done(Storage * const me, kode_status_t rc,
                               uint32_t us)
{
    me->t_busy_us += us;
    if ( KODE_OK != rc )
    {
        sd_test_fail(me, "the card would not read on one data line");
        return;
    }
    ++me->t_step;
    if ( SD_TEST_CHUNKS <= me->t_step )
    {
        uint32_t const kb_s = sd_test_rate(me->t_busy_us);

        KLOG_N(TAG, "test sd: %uKB read on one data line at %luKB/s",
               (unsigned)KODE_SD_TEST_KB, (unsigned long)kb_s);
        if ( (0U != kb_s) && (0U != me->t_read4_kb_s) )
        {
            KLOG_N(TAG, "test sd: four lines are %lu%% of one - a healthy slot "
                        "is well past 200",
                   (unsigned long)(((uint64_t)me->t_read4_kb_s * 100ULL)
                                   / (uint64_t)kb_s));
        }
        sd_test_advance(me, kb_s);
        return;
    }
    QTimeEvt_armX(&me->t_pacer, STORAGE_WRITE_GAP_TICKS, 0U);
}

/* ---- Phases 4 and 5: a file there and back, for the write speed ---------- */

/*
 * The file phases stay in the AO: kdrv_storage_create() can fall back to the
 * internal flash, which the worker's PSRAM stack must not touch.
 */

/**
 * @brief Open the test file on the card.
 * @param me The AO.
 * @return true with the file open; false after the phase has been failed.
 */
static bool sd_test_write_open(Storage * const me)
{
    kdrv_medium_t medium = KDRV_MEDIUM_FS;

    if ( kdrv_storage_create(SD_TEST_FILE, KDRV_MEDIUM_SD, &medium)
        != KODE_OK )
    {
        sd_test_fail(me, "no medium would take the test file");
        return false;
    }
    if ( KDRV_MEDIUM_SD != medium )
    {
        /* create() falls back to the other medium, and a megabyte written
         * to the internal partition would time the wrong thing entirely. */
        kdrv_storage_discard();
        sd_test_fail(me, "the card refused the file and it went to the "
                         "internal partition");
        return false;
    }
    return true;
}

/** @brief One chunk of the test file, and the close after the last. */
static void sd_test_write_chunk(Storage * const me)
{
    fill_pattern(me->buf[0], STORAGE_CHUNK, me->t_seed, me->t_step);

    uint64_t const t0 = khal_time_us();
    kode_status_t const st = kdrv_storage_write(me->buf[0], STORAGE_CHUNK);
    me->t_busy_us += (khal_time_us() - t0);

    if ( KODE_OK != st )
    {
        kdrv_storage_discard();
        sd_test_fail(me, "the card stopped taking the file");
        return;
    }
    ++me->t_step;
    if ( SD_TEST_CHUNKS <= me->t_step )
    {
        /* The close is part of the write and is timed: FATFS holds the last
         * sectors until then, and it is where a card that cannot take the
         * megabyte finally refuses. */
        uint64_t const t1 = khal_time_us();
        kode_status_t const closed = kdrv_storage_close();
        me->t_busy_us += (khal_time_us() - t1);

        if ( KODE_OK != closed )
        {
            sd_test_fail(me, "the card would not flush the file it had taken");
            return;
        }

        uint32_t const kb_s = sd_test_rate(me->t_busy_us);
        KLOG_N(TAG, "test sd: %uKB written at %luKB/s",
               (unsigned)KODE_SD_TEST_KB, (unsigned long)kb_s);
        sd_test_advance(me, kb_s);
        return;
    }
    QTimeEvt_armX(&me->t_pacer, STORAGE_WRITE_GAP_TICKS, 0U);
}

/** @brief One step of the write phase. */
static void sd_test_write_step(Storage * const me)
{
    if ( 0U == me->t_step )
    {
        /* Back on the board's own width first: the file is timed, and timing
         * it on one data line would measure the previous phase's setting. */
        if ( sd_test_restore_width(me, SD_THEN_WRITE_MOUNT) )
        {
            return;
        }
        if ( !sd_test_write_open(me) )
        {
            return;
        }
    }
    sd_test_write_chunk(me);
}

/** @brief The card is back on the board's width; the file can be made. */
static void sd_test_write_mounted(Storage * const me, kode_status_t rc)
{
    if ( KODE_OK != rc )
    {
        sd_test_fail(me, "the card would not come back on the board's width");
        return;
    }
    if ( sd_test_write_open(me) )
    {
        sd_test_write_chunk(me);
    }
}

/** @brief One step of the verify phase: read one chunk back and compare it with the pattern. */
static void sd_test_verify_step(Storage * const me)
{
    if ( 0U == me->t_step )
    {
        uint32_t      size   = 0U;
        kdrv_medium_t medium = KDRV_MEDIUM_FS;

        if ( kdrv_storage_open(SD_TEST_FILE, &size, &medium) != KODE_OK )
        {
            sd_test_fail(me, "the file that was just written cannot be opened");
            return;
        }
        if ( (KDRV_MEDIUM_SD != medium) || (SD_TEST_BYTES != size) )
        {
            (void)kdrv_storage_close();
            sd_test_fail(me, "the file came back the wrong size or from the "
                             "wrong medium");
            return;
        }
    }
    size_t got = 0U;

    uint64_t const t0 = khal_time_us();
    kode_status_t const st = kdrv_storage_read(me->buf[1], STORAGE_CHUNK, &got);
    me->t_busy_us += (khal_time_us() - t0);

    if ( (KODE_OK != st) || ((size_t)STORAGE_CHUNK != got) )
    {
        (void)kdrv_storage_close();
        sd_test_fail(me, "the file stopped reading back");
        return;
    }
    fill_pattern(me->buf[0], STORAGE_CHUNK, me->t_seed, me->t_step);
    if ( memcmp(me->buf[0], me->buf[1], STORAGE_CHUNK) != 0 )
    {
        (void)kdrv_storage_close();
        sd_test_fail(me, "the file read back as something else");
        return;
    }
    ++me->t_step;
    if ( SD_TEST_CHUNKS <= me->t_step )
    {
        uint32_t const kb_s = sd_test_rate(me->t_busy_us);

        (void)kdrv_storage_close();
        (void)kdrv_storage_remove(SD_TEST_FILE, KDRV_MEDIUM_SD);
        KLOG_N(TAG, "test sd: %uKB read back through the filesystem at "
                    "%luKB/s", (unsigned)KODE_SD_TEST_KB,
               (unsigned long)kb_s);
        sd_test_advance(me, kb_s);
        return;
    }
    QTimeEvt_armX(&me->t_pacer, STORAGE_WRITE_GAP_TICKS, 0U);
}

/* ---- sd_verify: is the CARD what it says it is? -------------------------- */

/**
 * @brief Look for the tail's pattern at every power-of-two size below the
 *        capacity the card claims.
 *
 * A card that lies about its size wraps a write past its real end to near the
 * beginning. One single-sector read per power-of-two size catches it: buf[0]
 * keeps the pattern, buf[1] takes each sector, and sd_verify_alias_done()
 * judges one and asks for the next.
 */
static void sd_verify_alias_next(Storage * const me)
{
    if ( (0U == me->t_alias) || (me->t_alias > me->t_tail) )
    {
        KLOG_N(TAG, "sd_verify: the last megabyte of %luMB is real",
               (unsigned long)me->t_capacity_mb);
        sd_test_advance(me, 0U);
        return;
    }
    if ( !sd_job_sectors(me, SD_OP_READ, me->t_tail % me->t_alias, 1U,
                        me->buf[1], SD_THEN_ALIAS) )
    {
        sd_test_fail(me, "the card could not be asked for the wrap scan");
    }
}

/** @brief Put the pattern in place and start the wrap scan. */
static void sd_verify_alias_begin(Storage * const me)
{
    uint32_t const ssz = STORAGE_CHUNK / me->t_per_chunk;

    fill_pattern(me->buf[0], (size_t)ssz, me->t_seed, 0U);
    me->t_alias = SD_TEST_ALIAS_MIN_SECTORS;
    sd_verify_alias_next(me);
}

/** @brief One sector of the wrap scan is in. */
static void sd_verify_alias_done(Storage * const me, kode_status_t rc)
{
    uint32_t const real  = me->t_alias;
    uint32_t const where = me->t_tail % real;
    uint32_t const ssz   = STORAGE_CHUNK / me->t_per_chunk;

    me->t_alias = real << 1;
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "sd_verify: sector %lu would not read",
               (unsigned long)where);
    }
    else if ( memcmp(me->buf[1], me->buf[0], (size_t)ssz) == 0 )
    {
        KLOG_E(TAG, "sd_verify: the tail also appears at sector %lu - the "
                    "card really holds %luMB, not %luMB",
               (unsigned long)where,
               (unsigned long)(((uint64_t)real * ssz) / (1024ULL * 1024ULL)),
               (unsigned long)me->t_capacity_mb);
        sd_test_fail(me, "the card is smaller than it says it is");
        return;
    }
    else
    {
        /* the tail is not here; the next size */
    }
    sd_verify_alias_next(me);
}

/** @brief One step of the probe: a chunk written, a chunk checked, or the scan. */
static void sd_verify_probe_step(Storage * const me)
{
    bool out = true;

    if ( SD_TEST_CHUNKS > me->t_step )
    {
        uint32_t const first = me->t_tail + (me->t_step * me->t_per_chunk);

        fill_pattern(me->buf[0], STORAGE_CHUNK, me->t_seed, me->t_step);
        out = sd_job_sectors(me, SD_OP_WRITE, first, me->t_per_chunk,
                             me->buf[0], SD_THEN_PROBE_WRITE);
    }
    else if ( (SD_TEST_CHUNKS * 2U) > me->t_step )
    {
        uint32_t const idx   = me->t_step - SD_TEST_CHUNKS;
        uint32_t const first = me->t_tail + (idx * me->t_per_chunk);

        out = sd_job_sectors(me, SD_OP_READ, first, me->t_per_chunk,
                             me->buf[1], SD_THEN_PROBE_READ);
    }
    else
    {
        sd_verify_alias_begin(me);
    }
    if ( !out )
    {
        sd_test_fail(me, "the card could not be asked for the last megabyte");
    }
}

/** @brief A chunk of the far end was written (or was not). */
static void sd_verify_probe_written(Storage * const me, kode_status_t rc)
{
    if ( KODE_OK != rc )
    {
        sd_test_fail(me, "the last megabyte of the card would not take a "
                         "write");
        return;
    }
    ++me->t_step;
    QTimeEvt_armX(&me->t_pacer, STORAGE_WRITE_GAP_TICKS, 0U);
}

/** @brief A chunk of the far end was read back. */
static void sd_verify_probe_read(Storage * const me, kode_status_t rc)
{
    uint32_t const idx = me->t_step - SD_TEST_CHUNKS;

    if ( KODE_OK != rc )
    {
        sd_test_fail(me, "the last megabyte of the card would not read "
                         "back");
        return;
    }
    fill_pattern(me->buf[0], STORAGE_CHUNK, me->t_seed, idx);
    if ( memcmp(me->buf[0], me->buf[1], STORAGE_CHUNK) != 0 )
    {
        sd_test_fail(me, "the last megabyte read back as something else");
        return;
    }
    ++me->t_step;
    QTimeEvt_armX(&me->t_pacer, STORAGE_WRITE_GAP_TICKS, 0U);
}

/** @brief Ask for the format. */
static void sd_verify_format_step(Storage * const me)
{
    if ( !sd_job_submit(me, SD_OP_FORMAT, SD_THEN_FORMAT) )
    {
        sd_test_fail(me, "the card could not be asked for a filesystem");
    }
}

/** @brief The format is done: did the volume it made cover the card? */
static void sd_verify_format_done(Storage * const me, kode_status_t rc,
                                  uint32_t total_mb)
{
    if ( KODE_OK != rc )
    {
        sd_test_fail(me, "the card would not take a filesystem");
        return;
    }
    if ( !me->sdj.sized )
    {
        sd_test_fail(me, "the new filesystem would not say how big it is");
        return;
    }
    me->t_total_mb = total_mb;
    KLOG_N(TAG, "sd_verify: formatted; %luMB of filesystem on a %luMB card",
           (unsigned long)me->t_total_mb, (unsigned long)me->t_capacity_mb);

    if ( ((uint64_t)me->t_total_mb * 100ULL)
        < ((uint64_t)me->t_capacity_mb * SD_TEST_MIN_PCT) )
    {
        sd_test_fail(me, "the filesystem covers less of the card than it "
                         "should");
        return;
    }
    sd_test_advance(me, 0U);
}

/* ---- Phase 1, and the way in --------------------------------------------- */

/** @brief One step of whichever phase is running. */
static void sd_test_step(Storage * const me)
{
    if ( me->sdj.busy )
    {
        /* A tick with a call out cannot be a live run's: a script arms its
         * pacer only between calls, and one that overran ended where it
         * stood (sd_script_overrun()). A run still open here would wait for
         * a tick that this return never re-arms, so it is ended. */
        if ( 0U != me->t_phase )
        {
            sd_script_overrun(me);
        }
        return;
    }
    if ( me->t_verify )
    {
        if ( SD_VERIFY_PROBE == me->t_phase )
        {
            sd_verify_probe_step(me);
        }
        else
        {
            sd_verify_format_step(me);
        }
        return;
    }
    switch ( me->t_phase )
    {
        case SD_TEST_READ4:  sd_test_read4_step(me);  break;
        case SD_TEST_READ1:  sd_test_read1_step(me);  break;
        case SD_TEST_WRITE:  sd_test_write_step(me);  break;
        case SD_TEST_VERIFY: sd_test_verify_step(me); break;
        default:                                      break;
    }
}

/**
 * @brief The remount that starts a script has returned: identify the card and
 *        set the geometry both scripts need.
 *
 * @param me The AO.
 * @param rc What the remount returned.
 */
static void sd_script_identify(Storage * const me, kode_status_t rc)
{
    int32_t width    = 0;
    int32_t freq     = 0;
    int32_t sectors  = 0;
    int32_t ssz      = 0;
    int32_t capacity = 0;
    char    name[9]  = { 0 };

    if ( KODE_OK != rc )
    {
        sd_test_abort(me, "there is no card in the slot; on this board that "
                          "is usually the ESP32-C5 not switching its supply");
        return;
    }
    kdrv_sd_dump();

    (void)kdrv_sd_name(name, sizeof(name));
    (void)kdrv_sd_get(KDRV_SD_BUS_WIDTH, &width);
    (void)kdrv_sd_get(KDRV_SD_FREQ_KHZ, &freq);
    (void)kdrv_sd_get(KDRV_SD_SECTORS, &sectors);
    (void)kdrv_sd_get(KDRV_SD_SECTOR_SIZE, &ssz);
    (void)kdrv_sd_get(KDRV_SD_CAPACITY_MB, &capacity);

    me->t_capacity_mb = (uint32_t)capacity;
    KLOG_N(TAG, "test sd: '%s', %luMB, %d-bit at %lukHz",
           name, (unsigned long)me->t_capacity_mb, (int)width,
           (unsigned long)freq);

    if ( (0 >= ssz) || ((STORAGE_CHUNK % (uint32_t)ssz) != 0U) )
    {
        sd_test_abort(me, "the card's sector size is not one this test can "
                          "chunk");
        return;
    }
    me->t_per_chunk = STORAGE_CHUNK / (uint32_t)ssz;

    if ( (uint32_t)sectors <= (2U * (SD_TEST_BYTES / (uint32_t)ssz)) )
    {
        sd_test_abort(me, "the card is smaller than the test");
        return;
    }
    /* Read from the middle of the card rather than the front: the front is
     * the filesystem's own metadata, which the card may well be holding in a
     * cache of its own, and a cached read measures nothing. */
    me->t_read_first = ((uint32_t)sectors / 2U) & ~(me->t_per_chunk - 1U);
    me->t_tail = (uint32_t)sectors - (SD_TEST_BYTES / (uint32_t)ssz);

    if ( me->t_verify )
    {
        /* Straight into the probe: the identify is bring-up here, not a
         * phase, because nothing sd_verify asks is about the board. */
        me->t_phase = SD_VERIFY_PROBE;
        QTimeEvt_armX(&me->t_pacer, STORAGE_WRITE_GAP_TICKS, 0U);
        return;
    }
    if ( (int32_t)KBSP_SD_WIDTH != width )
    {
        sd_test_abort(me, "the card came up on one data line; three of the "
                          "four are not reaching the slot");
        return;
    }
    sd_test_advance(me, 0U);
}

/**
 * @brief Start one of the two scripts: remount, then identify (the rest is
 *        sd_script_identify(), when the remount returns).
 *
 * @param verify false for `test component=sd`, which judges the slot and ends
 *        at the identify unless the card came up on four data lines. true for
 *        `sd_verify`, which judges the card on however many lines it got.
 */
static void sd_script_start(Storage * const me, bool verify)
{
    if ( 0U != me->t_phase )
    {
        /* Nothing typed can arrive here - the console is busy for the whole
         * run - so this is somebody asking twice from inside the firmware. */
        KLOG_W(TAG, "a card script is already running");
        return;
    }
    if ( busy(me) )
    {
        /* Answered rather than dropped: whoever asked is waiting on a
         * verdict, and both scripts walk the card, so neither is something to
         * start while anything at all has a file open. */
        KLOG_E(TAG, "sd: FAIL - a file is open");
        if ( verify )
        {
            sd_verify_result(me, false);
        }
        else
        {
            sd_test_result(me, false);
        }
        return;
    }

    me->t_verify       = verify;
    me->t_phase        = SD_TEST_IDENTIFY;
    me->t_step         = 0U;
    me->t_ok           = true;
    me->t_board_failed = false;
    me->t_card_failed  = false;
    me->t_busy_us      = 0U;
    me->t_read4_kb_s   = 0U;
    me->t_total_mb     = 0U;
    me->t_capacity_mb  = 0U;
    me->t_seed         = (uint32_t)khal_time_us();

    /* On the board's own width, whatever an earlier run or a bench session
     * left in the knob: this row judges the slot as the product uses it. */
    (void)kdrv_sd_set(KDRV_SD_WIDTH_REQ, (int32_t)KBSP_SD_WIDTH);

    /* Mount semantics rather than the probe's: somebody just asked for this
     * test, and on this board asking usually means a card has just gone in. */
    if ( !sd_job_submit(me, SD_OP_REMOUNT, SD_THEN_SCRIPT) )
    {
        /* No room to ask: the same word a file left open gets, since the
         * asker is waiting on a verdict either way. */
        KLOG_E(TAG, "sd: FAIL - the card could not be asked for");
        me->t_phase  = 0U;
        me->t_verify = false;
        if ( verify )
        {
            sd_verify_result(me, false);
        }
        else
        {
            sd_test_result(me, false);
        }
    }
}

/**
 * @brief The call to the card has returned: go on with what it was for.
 *
 * Also the deadline's way in, with KODE_TIMEOUT: the caller of the call is
 * told it failed, and the guard is still up.
 *
 * @param me    The AO.
 * @param rc    The status the call returned.
 * @param value What it measured or found; see sd_job_run().
 */
static void sd_job_complete(Storage * const me, kode_status_t rc,
                            uint32_t value)
{
    switch ( me->sdj.then )
    {
        case SD_THEN_MOUNT: {
            if ( KODE_OK == rc )
            {
                KLOG_N(TAG, "sd mounted at %s",
                       kdrv_storage_mount_point(KDRV_MEDIUM_SD));
            }
            /* else: the driver already said why, and "no card" and "the
             * partition will not mount" are very different problems. */
            cmd_answer(me->sdj.who);
            break;
        }
        case SD_THEN_UNMOUNT: {
            cmd_answer(me->sdj.who);
            break;
        }
        case SD_THEN_SCRIPT: {
            sd_script_identify(me, rc);
            break;
        }
        case SD_THEN_READ4: {
            sd_test_read4_done(me, rc, value);
            break;
        }
        case SD_THEN_READ1_MOUNT: {
            sd_test_read1_mounted(me, rc);
            break;
        }
        case SD_THEN_READ1: {
            sd_test_read1_done(me, rc, value);
            break;
        }
        case SD_THEN_WRITE_MOUNT: {
            sd_test_write_mounted(me, rc);
            break;
        }
        case SD_THEN_VERDICT: {
            sd_test_finish(me);
            break;
        }
        case SD_THEN_PROBE_WRITE: {
            sd_verify_probe_written(me, rc);
            break;
        }
        case SD_THEN_PROBE_READ: {
            sd_verify_probe_read(me, rc);
            break;
        }
        case SD_THEN_ALIAS: {
            sd_verify_alias_done(me, rc);
            break;
        }
        case SD_THEN_FORMAT: {
            sd_verify_format_done(me, rc, value);
            break;
        }
        case SD_THEN_NONE: {
            break;
        }
        default: {
            break;
        }
    }
}

/* ---- The AO -------------------------------------------------------------- */

void Storage_ctor(void)
{
    Storage * const me = &l_storage;
    QActive_ctor(&me->super, Q_STATE_CAST(&Storage_initial));
    me->reader = NULL;
    me->writer = NULL;
    me->t_phase = 0U;
    me->c_who  = NULL;
    me->p_who  = NULL;
    me->r_who  = NULL;
    me->r_open = false;
    me->g_who      = NULL;
    me->g_buf      = NULL;     /* a constructor over a used AO: the heap's now */
    me->g_json_len = 0U;
    me->g_icon_len = 0U;
    me->x_who      = NULL;
    me->x          = NULL;     /* as g_buf: the heap's now */
    me->worker_up  = false;
    me->width_owed = false;
    me->sdj_stale  = 0U;
    (void)memset(&me->worker, 0, sizeof(me->worker));
    (void)memset(&me->sdj, 0, sizeof(me->sdj));
    QEQueue_init(&me->deferq, me->defer_sto, Q_DIM(me->defer_sto));
    QTimeEvt_ctorX(&me->sdj_dl, &me->super, STORAGE_SDJOB_TO_SIG, 0U);
    QTimeEvt_ctorX(&me->w_pacer, &me->super, STORAGE_WRITE_NEXT_SIG, 0U);
    QTimeEvt_ctorX(&me->t_pacer, &me->super, STORAGE_TEST_NEXT_SIG, 0U);
    QTimeEvt_ctorX(&me->c_pacer, &me->super, STORAGE_CAT_NEXT_SIG, 0U);
    QTimeEvt_ctorX(&me->p_pacer, &me->super, STORAGE_COPY_NEXT_SIG, 0U);
    QTimeEvt_ctorX(&me->r_idle,  &me->super, STORAGE_RECV_IDLE_SIG, 0U);
    QTimeEvt_ctorX(&me->x_pacer, &me->super, STORAGE_APP_WIPE_NEXT_SIG, 0U);
#if CONFIG_KODE_LOWPOWER
    me->lp_who = KODE_LOWPOWER_WHO_NONE;
#endif
}

AO_STORAGE(storage, KQP_AO_QUEUE_LEN, KQP_AO_STACK_LARGE);

#if CONFIG_KODEOS_AUTOTEST
/* ---- The self-test rows: registered by kaos_storage_register() ---------------- */

/** @brief The internal volume's row: AO_Storage judges it and answers TEST_RESULT_SIG. */
static bool storage_row_fs(void)
{
    storage_request(KDRV_MEDIUM_FS, STORAGE_OP_SELFTEST);
    return false;   /* the answer comes back as TEST_RESULT_SIG */
}

/**
 * @brief The card's row: AO_Storage judges it and answers TEST_RESULT_SIG.
 *
 * The card read on four data lines and again on one, then a
 * megabyte written to a file, read back and removed. Each phase is a record
 * of its own, and nothing already on the card is touched.
 *
 * @return false: the answer comes back as TEST_RESULT_SIG.
 */
static bool storage_row_sd(void)
{
    storage_request(KDRV_MEDIUM_SD, STORAGE_OP_SELFTEST);
    return false;
}
#endif

void kaos_storage_register(void)
{
#if CONFIG_KODEOS_AUTOTEST
    static const krow_t STORAGE_ROWS[] = {
        { "fs", AUTOTEST_ID_FS, storage_row_fs, KROW_ASYNC, AUTOTEST_G_NONE, KODE_TEST_ROW_BUDGET_MS,
          NULL, 0U, "Internal storage",
          "Storage not working",
          AUTOTEST_ORDER_FS },
        { "sd", AUTOTEST_ID_SD, storage_row_sd, KROW_ASYNC, AUTOTEST_G_NONE, KODE_SD_TEST_BUDGET_MS,
          NULL, 0U, "Card slot",
          "Slot not working",
          AUTOTEST_ORDER_SD },
        { "sd_user", AUTOTEST_ID_SD_USER, storage_row_sd, KROW_ASYNC, AUTOTEST_G_NONE,
          KODE_SD_TEST_BUDGET_MS,
          "sd", KROW_F_SKIP_NO_CARD, "Card slot",
          "Restart, then retry",
          AUTOTEST_ORDER_SD_USER },
    };

    kaos_require(krows_add(STORAGE_ROWS, sizeof(STORAGE_ROWS) / sizeof(STORAGE_ROWS[0])),
                 "storage rows");
#endif
#if CONFIG_KODEOS_CONSOLE
    kaos_require(kverbs_add(&storage_verbs), "storage verbs");
#endif
}

void kaos_storage_start(void)
{
    Storage_ctor();
    kaos_storage_register();
    AO_START(AO_Storage, storage, KODE_PRIO_STORAGE);
}

/**
 * @brief Claim the card and the filesystem, take the PSRAM buffers, start the card worker.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to Storage_active.
 */
static QState Storage_initial(Storage * const me, void const * const par)
{
    (void)par;

    kaos_claim(KBSP_RES_SDMMC, me, "AO_Storage");
    kaos_claim(KBSP_RES_FATFS, me, "AO_Storage");
#if CONFIG_KODE_LOWPOWER
    me->lp_who = lowpower_join(&me->super, "storage");
    if ( KODE_LOWPOWER_WHO_NONE != me->lp_who )
    {
        QActive_subscribe(&me->super, LOWPOWER_ROUND_SIG);
    }
#endif

    /* PSRAM, and aligned: these are the source of DMA'd flash writes, and 64 KB
     * of internal RAM is not something to spend on a buffer that is idle most
     * of the time. */
    for ( uint8_t i = 0U; STORAGE_BUFS > i; ++i )
    {
        me->buf[i]  = khal_alloc_psram(64U, STORAGE_CHUNK);
        me->lent[i] = false;
        if ( NULL == me->buf[i] )
        {
            KLOG_E(TAG, "no PSRAM for the streaming buffers; reads will fail");
        }
    }
    if ( kqp_worker_start(&me->worker, &me->super, STORAGE_SD_WORKER_NAME,
                         STORAGE_SD_WORKER_STACK, KODE_PRIO_STORAGE,
                         STORAGE_SD_WORKER_DEPTH, true) == KODE_OK )
    {
        me->worker_up = true;
    }
    else
    {
        KLOG_E(TAG, "the card worker would not start; the card cannot be "
                    "mounted or tested");
    }
    KLOG_I(TAG, "up; %ux%uKB streaming buffers in PSRAM",
           (unsigned)STORAGE_BUFS, (unsigned)(STORAGE_CHUNK / 1024U));
    return Q_TRAN(&Storage_active);
}

/**
 * @brief Hand the worker a mount or an unmount that a command asked for.
 *
 * @param me   The AO.
 * @param c    The command.
 * @param op   SD_OP_MOUNT or SD_OP_UNMOUNT.
 * @param then The matching SdThen.
 * @return true when the call is out and its return answers the asker; false
 *         when it could not be made and the asker is answered at once.
 */
static bool cmd_job(Storage * const me, const StorageCmdEvt *c, SdOp op,
                    SdThen then)
{
    me->sdj.who = c->who;
    if ( sd_job_submit(me, op, then) )
    {
        return true;
    }
    KLOG_W(TAG, "the card could not be asked for; the request is refused");
    return false;
}

/**
 * @brief Do one command.
 *
 * @param me The AO.
 * @param c  The command.
 * @return true when the command is a call to the card that is now out, whose
 *         return answers the asker; false when it is done and the asker is
 *         answered at once.
 */
static bool handle_cmd(Storage * const me, const StorageCmdEvt *c)
{
    kdrv_medium_t const medium = (kdrv_medium_t)c->medium;
    const char *const   what   = (KDRV_MEDIUM_SD == medium) ? "sd" : "fs";
    bool                job    = false;

    /* The card test walks the card between phases, and a rescan from the
     * launcher or a link coming up on the radio both post a mount or an
     * unmount without knowing that. Dropping those for the length of the run
     * is what stops the card being pulled out from under it; the console is
     * busy for the same duration, so nothing typed can arrive here either. */
    if ( (0U != me->t_phase) && (KDRV_MEDIUM_SD == medium)
        && ((storage_op_t)c->op != STORAGE_OP_SELFTEST)
        && ((storage_op_t)c->op != STORAGE_OP_VERIFY) )
    {
        KLOG_W(TAG, "the card test is running; the card is not free");
        return false;
    }

    switch ( (storage_op_t)c->op )
    {
        case STORAGE_OP_MOUNT: {
            if ( KDRV_MEDIUM_SD == medium )
            {
                job = cmd_job(me, c, SD_OP_MOUNT, SD_THEN_MOUNT);
                break;
            }
            const char *mount = kdrv_storage_mount(medium);
            if ( NULL != mount )
            {
                KLOG_N(TAG, "%s mounted at %s", what, mount);
            }
            /* else: the driver already said why, and "no card" and "the
             * partition will not mount" are very different problems. */
            break;
        }
        case STORAGE_OP_UNMOUNT: {
            if ( busy(me) )
            {
                /* Usually a rescan's unmount+mount, which is fire-and-forget:
                 * refusing it stalls nothing, and keeps the card mounted under
                 * the transfer already touching it. */
                KLOG_W(TAG, "a transfer is in flight; %s unmount is refused",
                       what);
                break;
            }
            if ( KDRV_MEDIUM_SD == medium )
            {
                job = cmd_job(me, c, SD_OP_UNMOUNT, SD_THEN_UNMOUNT);
                break;
            }
            kdrv_storage_unmount(medium);
            break;
        }
        case STORAGE_OP_INFO: {
            /* The card/partition detail stays in the log (kdrv dumps it);
             * the mount state and usage go out as the event the console
             * closes `sd_info`/`fs_info` with. */
            kdrv_storage_dump(medium);
            StorageInfoEvt *ev = Q_NEW_X(StorageInfoEvt, 0U, STORAGE_INFO_SIG);
            if ( NULL != ev )
            {
                uint64_t total = 0U;
                uint64_t freeb = 0U;
                int32_t  mb    = 0;
                ev->medium      = (uint8_t)medium;
                ev->mounted     = kdrv_storage_mounted(medium);
                ev->usage_known = (kdrv_storage_usage(medium, &total, &freeb)
                                   == KODE_OK);
                ev->total_kb    = kb_of(total);
                ev->free_kb     = kb_of(freeb);
                /* THE CARD, not the volume on it: the two match only when the
                 * card is formatted end to end. */
                ev->card_mb     = 0U;
                if ( (KDRV_MEDIUM_SD == medium)
                    && (kdrv_sd_get(KDRV_SD_CAPACITY_MB, &mb) == KODE_OK)
                    && (0 < mb) )
                {
                    ev->card_mb = (uint32_t)mb;
                }
                QACTIVE_PUBLISH(&ev->super, &me->super);
            }
            break;
        }
        case STORAGE_OP_LIST: {
            do_list(medium, c->images_only);
            if ( c->last )
            {
                list_end(NULL, false);
            }
            break;
        }
        case STORAGE_OP_VERIFY: {
            if ( KDRV_MEDIUM_SD != medium )
            {
                break;      /* the internal partition has no capacity to lie about */
            }
            sd_script_start(me, true);
            break;
        }
        case STORAGE_OP_SELFTEST: {
            if ( KDRV_MEDIUM_SD == medium )
            {
                /* The card's is the production row: five phases, its own
                 * records, and its own verdict when it is done. */
                sd_script_start(me, false);
                break;
            }
            if ( busy(me) )
            {
                /* Silent, like every other row this AO cannot start: the
                 * console's own budget turns an unanswered `test` into
                 * `result=timeout`, which is the honest word for "the
                 * partition is in use, ask again" here too. */
                KLOG_W(TAG, "a transfer is in flight; fs selftest is refused");
                break;
            }
            bool const ok = (kdrv_storage_selftest(medium) == KODE_OK);
            TestResultEvt *ev = Q_NEW_X(TestResultEvt, 0U, TEST_RESULT_SIG);
            if ( NULL != ev )
            {
                (void)snprintf(ev->name, sizeof(ev->name), "%s", what);
                ev->ok = ok;
                ev->origin = TEST_ORIGIN_AUTOTEST;
                QACTIVE_PUBLISH(&ev->super, &me->super);
            }
            break;
        }
        default: {
            break;
        }
    }
    return job;
}

#if CONFIG_KODE_LOWPOWER
/**
 * @brief LOWPOWER_ROUND_SIG: files closed and the card idle, or busy; answer once.
 *
 * @param me The AO.
 * @param r  The round.
 */
static void storage_round(Storage const * const me, const LowPowerRoundEvt * const r)
{
    if ( (NULL != me->writer) || me->r_open )
    {
        lowpower_ready(me->lp_who, r->round, true, "file write");
    }
    else if ( busy(me) || me->sdj.busy )
    {
        lowpower_ready(me->lp_who, r->round, true, "storage busy");
    }
    else
    {
        lowpower_ready(me->lp_who, r->round, false, NULL);
    }
}
#endif /* CONFIG_KODE_LOWPOWER */

/** @brief Serve one event. */
static QState Storage_serve(Storage * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case STORAGE_LIST_PAGE_SIG: {
            StorageListPageEvt const * const pg = Q_EVT_CAST(StorageListPageEvt);
            if ( NULL == pg->who )
            {
                KLOG_W(TAG, "a page of the image listing named nobody to send it to");
            }
            else
            {
                do_list_apps(pg->skip, pg->who);
            }
            status = Q_HANDLED();
            break;
        }
        case STORAGE_CMD_SIG: {
            StorageCmdEvt const * const c = Q_EVT_CAST(StorageCmdEvt);

            /* Whatever became of it - done, refused, dropped for the card
             * test: the asker is waiting on when, not on what. A call to the
             * card answers when it returns. */
            if ( !handle_cmd(me, c) )
            {
                cmd_answer(c->who);
            }
            status = Q_HANDLED();
            break;
        }
        case STORAGE_SDJOB_DONE_SIG: {
            sd_job_done(me, Q_EVT_CAST(KqpWorkerDoneEvt));
            status = Q_HANDLED();
            break;
        }
        case STORAGE_SDJOB_TO_SIG: {
            sd_job_deadline(me);
            status = Q_HANDLED();
            break;
        }
        case STORAGE_OPEN_REQ_SIG: {
            StorageOpenEvt const * const req = Q_EVT_CAST(StorageOpenEvt);
            uint32_t      size   = 0U;
            kdrv_medium_t medium = KDRV_MEDIUM_FS;
            kode_status_t st     = KODE_DENIED;

            if ( busy(me) )
            {
                KLOG_W(TAG, "a file is already open; '%s' refused", req->name);
            }
            else if ( '/' == req->name[0] )
            {
                /* An absolute path names its volume: open exactly that file,
                 * as write_begin() writes exactly where a path says. */
                st = kdrv_storage_open_path(req->name, &size);
                medium = kdrv_storage_medium_of(req->name);
                if ( KODE_OK == st )
                {
                    me->reader = req->who;
                }
            }
            else
            {
                st = kdrv_storage_open(req->name, &size, &medium);
                if ( KODE_OK == st )
                {
                    me->reader = req->who;
                }
            }

            StorageOpenRspEvt *rsp = Q_NEW_X(StorageOpenRspEvt, 0U,
                                             STORAGE_OPEN_RSP_SIG);
            if ( NULL != rsp )
            {
                rsp->size   = size;
                rsp->status = (uint8_t)st;
                rsp->medium = (uint8_t)medium;
                QACTIVE_POST(req->who, &rsp->super, &me->super);
            }
            else
            {
                /* Answered all the same, from a static event: an open nobody
                 * heard would keep the reader for a client that never knew it
                 * had it, and AO_Radio has no deadline of its own to give up
                 * by. So the open is undone and refused. */
                static const StorageOpenRspEvt s_open_dry_evt = {
                    QEVT_INITIALIZER(STORAGE_OPEN_RSP_SIG), 0U,
                    (uint8_t)KODE_NO_RESOURCES, (uint8_t)KDRV_MEDIUM_FS
                };
                KLOG_E(TAG, "out of events; the open of '%s' is undone",
                       req->name);
                if ( KODE_OK == st )
                {
                    close_file(me);
                }
                QACTIVE_POST(req->who, &s_open_dry_evt.super, &me->super);
            }
            status = Q_HANDLED();
            break;
        }
        case STORAGE_DESC_REQ_SIG: {
            StorageDescReqEvt const * const req = Q_EVT_CAST(StorageDescReqEvt);
            static uint8_t prefix[KAPP_PREFIX_LEN];
            kode_status_t  st = KODE_DENIED;
            uint32_t       size = 0U;
            kdrv_medium_t  medium = KDRV_MEDIUM_FS;
            size_t         got = 0U;

            /* A manifest is never worth displacing a transfer in flight,
             * in either direction. */
            if ( busy(me) )
            {
                KLOG_D(TAG, "busy; manifest of '%s' refused", req->name);
            }
            else if ( kdrv_storage_open(req->name, &size, &medium) == KODE_OK )
            {
                /* Read and close inside this one step. The reader slot is
                 * deliberately not taken: nothing outside this case can leave
                 * the file open, so nothing outside it can strand the system. */
                st = kdrv_storage_read(prefix, sizeof(prefix), &got);
                (void)kdrv_storage_close();
            }

            StorageDescRspEvt *rsp = Q_NEW_X(StorageDescRspEvt, 0U,
                                             STORAGE_DESC_RSP_SIG);
            if ( NULL != rsp )
            {
                (void)memset(rsp->project, 0, sizeof(rsp->project));
                (void)memset(rsp->display, 0, sizeof(rsp->display));
                (void)memset(rsp->category, 0, sizeof(rsp->category));
                (void)snprintf(rsp->name, sizeof(rsp->name), "%s", req->name);
                rsp->status = (uint8_t)st;

                if ( (KODE_OK == st) && (KAPP_PREFIX_LEN <= got) )
                {
                    (void)kapp_project_name(prefix, got, rsp->project,
                                            sizeof(rsp->project));
                    kapp_desc_t man;
                    if ( kapp_desc_parse(prefix, got, &man) == KODE_OK )
                    {
                        (void)snprintf(rsp->display, sizeof(rsp->display), "%s",
                                       man.name);
                        (void)snprintf(rsp->category, sizeof(rsp->category), "%s",
                                       man.category);
                    }
                    /* As in kdrv_apps.c: the override wins over what the image
                     * says. rsp->project comes from IDF's own descriptor, so this
                     * runs even when the parse above failed. */
                    (void)kapp_cat_resolve(rsp->project, rsp->category,
                                           sizeof(rsp->category));
                }
                QACTIVE_POST(req->who, &rsp->super, &me->super);
            }
            status = Q_HANDLED();
            break;
        }
        case APPDATA_REQ_SIG: {
            AppDataReqEvt const * const req = Q_EVT_CAST(AppDataReqEvt);
            AppDataFieldsEvt *f = Q_NEW_X(AppDataFieldsEvt, 0U,
                                          APPDATA_FIELDS_SIG);
            /* A static: 1 KB the parser walks wants internal RAM. The
             * 12 KB icon is only copied out of, so it is in PSRAM. */
            static char     json[KAPP_DATA_JSON_MAX + 1U];
            AppDataIconEvt *ic[3] = { NULL, NULL, NULL };
            char            folder[APPDATA_FOLDER_MAX];
            char            path[APPDATA_PATH_MAX];
            kapp_data_t     d;

            if ( NULL == f )
            {
                KLOG_E(TAG, "out of events; the folder of '%s' is answered busy",
                       req->project);
                appdata_answer_busy(req->who);
                status = Q_HANDLED();
                break;
            }
            /* From the first payload field, never from the top: the header
             * Q_NEW_X just wrote is what makes this event recyclable. */
            (void)memset(f->project, 0,
                         sizeof(*f) - offsetof(AppDataFieldsEvt, project));
            (void)snprintf(f->project, sizeof(f->project), "%s", req->project);
            f->medium      = (uint8_t)APPDATA_MEDIUM_NONE;
            f->json_status = (uint8_t)KAPP_JSON_NONE;
            f->icon_status = (uint8_t)APPDATA_ICON_NONE;
            (void)memset(&d, 0, sizeof(d));
            folder[0] = '\0';

            /* First ask, and only then: a firmware whose launcher never
             * opens an app folder never pays the 12 KB. Never released -
             * the next request would only take it again, and a buffer that
             * comes and goes is a fragmentation source for no gain. */
            if ( NULL == me->icon )
            {
                me->icon = (uint8_t *)khal_alloc_psram(64U, KAPP_ICON_BYTES);
                if ( (NULL == me->icon) && !me->icon_warned )
                {
                    me->icon_warned = true;
                    KLOG_W(TAG, "no PSRAM for the icon buffer; "
                                "icons go unanswered");
                }
            }

            if ( busy(me) )
            {
                /* A folder is never worth displacing a transfer; the asker
                 * retries on its next scan. */
                KLOG_D(TAG, "busy; the folder of '%s' is refused", req->project);
                f->icon_status = (uint8_t)APPDATA_ICON_BUSY;
            }
            else
            {
                /* Internal first, card second, the first folder found used
                 * whole (https://docs.kode.diy/sdk/kdrv/apps). */
                static const AppDataMedium ORDER[2] = { APPDATA_MEDIUM_FS,
                                                        APPDATA_MEDIUM_SD };
                for ( uint8_t i = 0U;
                     (2U > i) && ((uint8_t)APPDATA_MEDIUM_NONE == f->medium);
                     ++i )
                {
                    kdrv_stat_t         st;
                    kdrv_medium_t const km = (APPDATA_MEDIUM_FS == ORDER[i])
                                           ? KDRV_MEDIUM_FS : KDRV_MEDIUM_SD;
                    if ( !kdrv_storage_mounted(km)
                        || !appdata_folder_path(folder, sizeof(folder), ORDER[i],
                                                req->project) )
                    {
                        continue;
                    }
                    if ( (kdrv_storage_stat(folder, &st) == KODE_OK) && st.exists
                        && st.is_dir )
                    {
                        f->medium = (uint8_t)ORDER[i];
                    }
                }
            }

            if ( (uint8_t)APPDATA_MEDIUM_NONE != f->medium )
            {
                size_t   got  = 0U;
                uint32_t size = 0U;

                (void)snprintf(path, sizeof(path), "%s/" APPDATA_JSON_NAME, folder);
                if ( appdata_read_file(path, json, sizeof(json), &got, &size) )
                {
                    f->json_status = (KAPP_DATA_JSON_MAX < size)
                                   ? (uint8_t)KAPP_JSON_SIZE
                                   : (uint8_t)kapp_data_parse(json, got, &d);
                }
                if ( (uint8_t)KAPP_JSON_OK == f->json_status )
                {
                    (void)snprintf(f->name, sizeof(f->name), "%s", d.name);
                    (void)snprintf(f->category, sizeof(f->category), "%s",
                                   d.category);
                    (void)snprintf(f->version, sizeof(f->version), "%s",
                                   d.version);
                    (void)snprintf(f->description, sizeof(f->description), "%s",
                                   d.description);
                    f->color_valid = d.color_valid;
                    (void)memcpy(f->color, d.color, sizeof(f->color));
                }

                /* The icon: the path the file names, else icon.k64 beside it. */
                (void)snprintf(path, sizeof(path), "%s/%s", folder,
                               ('\0' != d.icon[0]) ? d.icon : APPDATA_ICON_NAME);
                if ( NULL == me->icon )
                {
                    /* The same answer a transfer gets: nothing is wrong with
                     * the folder, this AO just cannot read it right now. */
                    f->icon_status = (uint8_t)APPDATA_ICON_BUSY;
                }
                else if ( !appdata_read_file(path, me->icon, KAPP_ICON_BYTES,
                                            &got, &size) )
                {
                    f->icon_status = ('\0' != d.icon[0])
                                   ? (uint8_t)APPDATA_ICON_PATH
                                   : (uint8_t)APPDATA_ICON_NONE;
                }
                else if ( (KAPP_ICON_BYTES != size) || (KAPP_ICON_BYTES != got) )
                {
                    f->icon_status = (uint8_t)APPDATA_ICON_SIZE;
                }
                else
                {
                    f->icon_status = (uint8_t)APPDATA_ICON_OK;
                }
            }

            /* All three blocks or none, and taken BEFORE the fields event
             * goes out: half an icon would be drawn wrong rather than not
             * drawn, and the status has to be true when the asker reads it. */
            if ( (uint8_t)APPDATA_ICON_OK == f->icon_status )
            {
                bool all = true;
                for ( uint8_t s = 0U; 3U > s; ++s )
                {
                    ic[s] = Q_NEW_X(AppDataIconEvt, 0U, APPDATA_ICON_SIG);
                    all   = all && (NULL != ic[s]);
                }
                if ( !all )
                {
                    for ( uint8_t s = 0U; 3U > s; ++s )
                    {
                        if ( NULL != ic[s] )
                        {
                            QF_gc(&ic[s]->super);
                            ic[s] = NULL;
                        }
                    }
                    f->icon_status = (uint8_t)APPDATA_ICON_BUSY;
                }
            }

            QACTIVE_POST(req->who, &f->super, &me->super);

            for ( uint8_t s = 0U; (3U > s) && (NULL != ic[s]); ++s )
            {
                (void)memset(ic[s]->project, 0, sizeof(ic[s]->project));
                (void)snprintf(ic[s]->project, sizeof(ic[s]->project), "%s",
                               req->project);
                ic[s]->seq = s;
                (void)memcpy(ic[s]->data,
                             &me->icon[(size_t)s * KODE_RECV_CHUNK],
                             KODE_RECV_CHUNK);
                QACTIVE_POST(req->who, &ic[s]->super, &me->super);
            }
            {
                static const QEvt end = QEVT_INITIALIZER(APPDATA_END_SIG);
                QACTIVE_POST(req->who, &end, &me->super);
            }
            status = Q_HANDLED();
            break;
        }
        case APPDATA_RM_REQ_SIG: {
            AppDataReqEvt const * const req = Q_EVT_CAST(AppDataReqEvt);
            kode_status_t const rc = appdata_rm(me, req->project);
            AppDataRmRspEvt *rsp = Q_NEW_X(AppDataRmRspEvt, 0U,
                                           APPDATA_RM_RSP_SIG);
            if ( NULL == rsp )
            {
                KLOG_E(TAG, "out of events; the delete of '%s' goes "
                            "unanswered", req->project);
            }
            else
            {
                (void)snprintf(rsp->project, sizeof(rsp->project), "%s",
                               req->project);
                rsp->status = (uint8_t)rc;
                QACTIVE_POST(req->who, &rsp->super, &me->super);
            }
            status = Q_HANDLED();
            break;
        }
        case STORAGE_APP_WIPE_REQ_SIG: {
            wipe_begin(me, Q_EVT_CAST(AppDataReqEvt));
            status = Q_HANDLED();
            break;
        }
        case STORAGE_APP_WIPE_NEXT_SIG: {
            wipe_step(me);
            status = Q_HANDLED();
            break;
        }
        case APPDATA_PUT_REQ_SIG: {
            appdata_put_begin(me, Q_EVT_CAST(AppDataPutEvt));
            status = Q_HANDLED();
            break;
        }
        case APPDATA_PUT_NEXT_SIG: {
            appdata_put_icon(me);
            status = Q_HANDLED();
            break;
        }
        case STORAGE_READ_REQ_SIG: {
            serve_read(me);
            status = Q_HANDLED();
            break;
        }
        case STORAGE_NEXT_SIG: {
            uint8_t const idx = Q_EVT_CAST(StorageBufEvt)->idx;
            if ( STORAGE_BUFS > idx )
            {
                me->lent[idx] = false;
            }
            serve_read(me);
            status = Q_HANDLED();
            break;
        }
        case STORAGE_CLOSE_REQ_SIG: {
            /* This ends a read, and only a read. Any AO can post it, so the
             * guard is all that stops one AO ending another's transfer: closing
             * under a write or a copy would leave the asker, the pacer and the
             * lent buffer dangling, which only write_finish()/copy_finish()
             * clean up. */
            if ( NULL != me->writer )
            {
                KLOG_W(TAG, "close ignored: a write to '%s' is in flight",
                       me->w_name);
            }
            else if ( NULL != me->p_who )
            {
                KLOG_W(TAG, "close ignored: a copy to '%s' is in flight",
                       me->p_dst);
            }
            else if ( NULL != me->r_who )
            {
                /* Closing under a `file_put` would write every chunk still
                 * to arrive into a closed handle, and end with `>ok`. */
                KLOG_W(TAG, "close ignored: a file is arriving at '%s'",
                       me->r_path);
            }
            else
            {
                close_file(me);
            }
            status = Q_HANDLED();
            break;
        }
        case STORAGE_FIELD_REQ_SIG: {
            StorageFieldReqEvt const * const q = Q_EVT_CAST(StorageFieldReqEvt);
            int32_t value = q->value;

            /* Two statements: field_do() writes `value`, and C leaves the order
             * of argument evaluation unspecified. Do not merge them. */
            kode_status_t const rc = field_do(q, &value);
            field_answer(q, rc, value);
            status = Q_HANDLED();
            break;
        }
        case STORAGE_PATH_REQ_SIG: {
            handle_path_req(me, Q_EVT_CAST(StoragePathEvt));
            status = Q_HANDLED();
            break;
        }
        case STORAGE_PAIR_REQ_SIG: {
            handle_pair_req(me, Q_EVT_CAST(StoragePairEvt));
            status = Q_HANDLED();
            break;
        }
        case STORAGE_TEXT_REQ_SIG: {
            handle_text_req(me, Q_EVT_CAST(StorageTextEvt));
            status = Q_HANDLED();
            break;
        }
        case STORAGE_WRITE_REQ_SIG: {
            write_begin(me, Q_EVT_CAST(StorageWriteEvt));
            status = Q_HANDLED();
            break;
        }
        case STORAGE_WRITE_NEXT_SIG: {
            write_chunk(me);
            status = Q_HANDLED();
            break;
        }
        case STORAGE_TEST_NEXT_SIG: {
            sd_test_step(me);
            status = Q_HANDLED();
            break;
        }
        case STORAGE_CAT_NEXT_SIG: {
            cat_step(me);
            status = Q_HANDLED();
            break;
        }
        case STORAGE_COPY_NEXT_SIG: {
            copy_step(me);
            status = Q_HANDLED();
            break;
        }
        case STORAGE_RECV_REQ_SIG: {
            recv_begin(me, Q_EVT_CAST(StorageRecvEvt));
            status = Q_HANDLED();
            break;
        }
        case CONSOLE_RAW_RSP_SIG: {
            recv_armed(me, Q_EVT_CAST(ConsoleRawRspEvt)->status);
            status = Q_HANDLED();
            break;
        }
        case RECV_DATA_SIG: {
            /* Posted by AO_Log's reader task, not by an Active Object - the
             * one signal in this file that does not come from a peer. */
            recv_data(me, Q_EVT_CAST(RecvDataEvt));
            status = Q_HANDLED();
            break;
        }
        case STORAGE_RECV_IDLE_SIG: {
            recv_stall(me);
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            storage_round(me, Q_EVT_CAST(LowPowerRoundEvt));
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

/* ---- Requests held while a call to the card is out ------------------------
 * A request that could touch the card waits for the call to return, so the
 * card is never mounted, unmounted or read raw while something else walks its
 * volume, and the VFS never sees a task inside it when the card goes. A
 * request for the internal partition is served at once.
 */

/**
 * @brief Whether @p path is on the internal partition and nowhere else.
 * @param path A virtual path.
 * @return true for a path under the partition's prefix; false for the card's,
 *         for the root (which lists both) and for anything that names neither.
 */
static bool path_is_fs(const char *path)
{
    return kdrv_storage_medium_of(path) == KDRV_MEDIUM_FS;
}

/**
 * @brief Whether serving @p e could touch the card.
 *
 * A bare file name is the flat vocabulary, which tries the card first and
 * falls over to the partition, so it is the card's. A listing of the partition
 * is the card's too while one of the card's is held: a rescan posts the two in
 * order and the second ends the burst, so it must not overtake the first.
 *
 * @param me The AO.
 * @param e  The event.
 * @return true when it must wait for a call to the card to return.
 */
static bool needs_card(const Storage * const me, QEvt const * const e)
{
    bool card = false;

    switch ( e->sig )
    {
        case STORAGE_CMD_SIG: {
            StorageCmdEvt const * const c = Q_EVT_CAST(StorageCmdEvt);

            card = ((uint8_t)KDRV_MEDIUM_SD == c->medium)
                || (((storage_op_t)c->op == STORAGE_OP_LIST)
                    && !QEQueue_isEmpty(&me->deferq));
            break;
        }
        case STORAGE_OPEN_REQ_SIG: {
            const char *const name = Q_EVT_CAST(StorageOpenEvt)->name;

            card = ('/' != name[0]) || !path_is_fs(name);
            break;
        }
        case STORAGE_WRITE_REQ_SIG: {
            const char *const name = Q_EVT_CAST(StorageWriteEvt)->name;

            card = ('/' != name[0]) || !path_is_fs(name);
            break;
        }
        case STORAGE_DESC_REQ_SIG:
        case APPDATA_REQ_SIG:
        case APPDATA_RM_REQ_SIG:
        case STORAGE_APP_WIPE_REQ_SIG:
        case STORAGE_FIELD_REQ_SIG:
        case STORAGE_LIST_PAGE_SIG: {
            /* A page walks the card first, and the first one comes right
             * behind the remount storage_list_request() asked for. */
            card = true;
            break;
        }
        case STORAGE_PATH_REQ_SIG: {
            card = !path_is_fs(Q_EVT_CAST(StoragePathEvt)->path);
            break;
        }
        case STORAGE_PAIR_REQ_SIG: {
            StoragePairEvt const * const q = Q_EVT_CAST(StoragePairEvt);

            card = !path_is_fs(q->src) || !path_is_fs(q->dst);
            break;
        }
        case STORAGE_TEXT_REQ_SIG: {
            card = !path_is_fs(Q_EVT_CAST(StorageTextEvt)->path);
            break;
        }
        case STORAGE_RECV_REQ_SIG: {
            card = !path_is_fs(Q_EVT_CAST(StorageRecvEvt)->path);
            break;
        }
        case CONSOLE_RAW_RSP_SIG: {
            /* The console has armed the wire for a file_put: the destination
             * is opened now, and on the card that may mount it. */
            card = (NULL != me->r_who) && !path_is_fs(me->r_path);
            break;
        }
        default: {
            break;
        }
    }
    return card;
}

/**
 * @brief Refuse a request the hold queue has no room for, the way this AO
 *        refuses what it cannot serve now: an answer that says no, never a
 *        silence.
 *
 * @param me The AO.
 * @param e  The request.
 * @return true when it was refused and answered; false for an event that has
 *         no refusal (the console's arming answer), which is then served.
 */
static bool card_refuse(Storage * const me, QEvt const * const e)
{
    bool refused = true;

    switch ( e->sig )
    {
        case STORAGE_CMD_SIG: {
            StorageCmdEvt const * const c = Q_EVT_CAST(StorageCmdEvt);

            KLOG_W(TAG, "the card is busy; the request is refused");
            if ( ((storage_op_t)c->op == STORAGE_OP_SELFTEST)
                && ((uint8_t)KDRV_MEDIUM_SD == c->medium) )
            {
                sd_test_result(me, false);
            }
            else if ( (storage_op_t)c->op == STORAGE_OP_VERIFY )
            {
                sd_verify_result(me, false);
            }
            else if ( (storage_op_t)c->op == STORAGE_OP_INFO )
            {
                /* `sd_info` closes on this event: the card's state as far as
                 * it can be known without asking the volume, and no usage. */
                StorageInfoEvt *ev = Q_NEW_X(StorageInfoEvt, 0U,
                                             STORAGE_INFO_SIG);

                if ( NULL != ev )
                {
                    ev->medium      = c->medium;
                    ev->mounted     = kdrv_storage_mounted(
                                          (kdrv_medium_t)c->medium);
                    ev->usage_known = false;
                    ev->total_kb    = 0U;
                    ev->free_kb     = 0U;
                    ev->card_mb     = 0U;
                    QACTIVE_PUBLISH(&ev->super, &me->super);
                }
            }
            else if ( ((storage_op_t)c->op == STORAGE_OP_LIST) && c->last )
            {
                static const QEvt end_evt = QEVT_INITIALIZER(STORAGE_LIST_END_SIG);

                QACTIVE_PUBLISH(&end_evt, &me->super);
            }
            else
            {
                /* nothing but the answer */
            }
            cmd_answer(c->who);
            break;
        }
        case STORAGE_OPEN_REQ_SIG: {
            StorageOpenEvt const * const req = Q_EVT_CAST(StorageOpenEvt);
            StorageOpenRspEvt *rsp = Q_NEW_X(StorageOpenRspEvt, 0U,
                                             STORAGE_OPEN_RSP_SIG);

            KLOG_W(TAG, "the card is busy; '%s' refused", req->name);
            if ( NULL != rsp )
            {
                rsp->size   = 0U;
                rsp->status = (uint8_t)KODE_DENIED;
                rsp->medium = (uint8_t)KDRV_MEDIUM_FS;
                QACTIVE_POST(req->who, &rsp->super, &me->super);
            }
            break;
        }
        case STORAGE_LIST_PAGE_SIG: {
            /* The asker is waiting on the page's end: the end of an empty
             * page is the answer, and the listing stops there. A page that
             * names nobody has nobody to answer, and list_end() would
             * publish it to every subscriber instead. */
            QActive * const who = Q_EVT_CAST(StorageListPageEvt)->who;

            KLOG_W(TAG, "the card is busy; a page of the image listing is refused");
            if ( NULL != who )
            {
                list_end(who, false);
            }
            break;
        }
        case STORAGE_DESC_REQ_SIG: {
            StorageDescReqEvt const * const req = Q_EVT_CAST(StorageDescReqEvt);
            StorageDescRspEvt *rsp = Q_NEW_X(StorageDescRspEvt, 0U,
                                             STORAGE_DESC_RSP_SIG);

            KLOG_D(TAG, "the card is busy; manifest of '%s' refused", req->name);
            if ( NULL != rsp )
            {
                (void)memset(rsp->project, 0, sizeof(rsp->project));
                (void)memset(rsp->display, 0, sizeof(rsp->display));
                (void)memset(rsp->category, 0, sizeof(rsp->category));
                (void)snprintf(rsp->name, sizeof(rsp->name), "%s", req->name);
                rsp->status = (uint8_t)KODE_DENIED;
                QACTIVE_POST(req->who, &rsp->super, &me->super);
            }
            break;
        }
        case APPDATA_REQ_SIG: {
            AppDataReqEvt const * const req = Q_EVT_CAST(AppDataReqEvt);
            AppDataFieldsEvt *f = Q_NEW_X(AppDataFieldsEvt, 0U,
                                          APPDATA_FIELDS_SIG);

            KLOG_D(TAG, "the card is busy; the folder of '%s' is refused",
                   req->project);
            if ( NULL != f )
            {
                static const QEvt end = QEVT_INITIALIZER(APPDATA_END_SIG);

                (void)memset(f->project, 0,
                             sizeof(*f) - offsetof(AppDataFieldsEvt, project));
                (void)snprintf(f->project, sizeof(f->project), "%s",
                               req->project);
                f->medium      = (uint8_t)APPDATA_MEDIUM_NONE;
                f->json_status = (uint8_t)KAPP_JSON_NONE;
                f->icon_status = (uint8_t)APPDATA_ICON_BUSY;
                QACTIVE_POST(req->who, &f->super, &me->super);
                QACTIVE_POST(req->who, &end, &me->super);
            }
            break;
        }
        case APPDATA_RM_REQ_SIG: {
            AppDataReqEvt const * const req = Q_EVT_CAST(AppDataReqEvt);
            AppDataRmRspEvt *rsp = Q_NEW_X(AppDataRmRspEvt, 0U,
                                           APPDATA_RM_RSP_SIG);

            KLOG_W(TAG, "the card is busy; the folder of '%s' stays",
                   req->project);
            if ( NULL != rsp )
            {
                rsp->status = (uint8_t)KODE_DENIED;
                QACTIVE_POST(req->who, &rsp->super, &me->super);
            }
            break;
        }
        case STORAGE_APP_WIPE_REQ_SIG: {
            AppDataReqEvt const * const req = Q_EVT_CAST(AppDataReqEvt);

            KLOG_W(TAG, "the card is busy; the files of '%s' stay",
                   req->project);
            if ( NULL != req->who )
            {
                wipe_refuse(req->who, req->project, KODE_DENIED);
            }
            break;
        }
        case STORAGE_WRITE_REQ_SIG: {
            StorageWriteEvt const * const req = Q_EVT_CAST(StorageWriteEvt);
            StorageWriteRspEvt *rsp = Q_NEW_X(StorageWriteRspEvt, 0U,
                                              STORAGE_WRITE_RSP_SIG);

            KLOG_W(TAG, "the card is busy; the write of '%s' is refused",
                   req->name);
            if ( NULL != rsp )
            {
                rsp->written = 0U;
                rsp->status  = (uint8_t)KODE_DENIED;
                rsp->medium  = (uint8_t)KDRV_MEDIUM_FS;
                QACTIVE_POST(req->who, &rsp->super, &me->super);
            }
            break;
        }
        case STORAGE_FIELD_REQ_SIG: {
            field_answer(Q_EVT_CAST(StorageFieldReqEvt), KODE_DENIED, 0);
            break;
        }
        case STORAGE_PATH_REQ_SIG: {
            StoragePathEvt const * const q = Q_EVT_CAST(StoragePathEvt);

            reply_set(q->wait, q->gen, q->who, q->rsp, NULL);
            KLOG_W(TAG, "the card is busy; '%s' refused", q->path);
            post_path_rsp(q->who, q->op, KODE_DENIED, false, false, 0U);
            reply_clear();
            break;
        }
        case STORAGE_PAIR_REQ_SIG: {
            StoragePairEvt const * const q = Q_EVT_CAST(StoragePairEvt);

            reply_set(q->wait, q->gen, q->who, q->rsp, NULL);
            KLOG_W(TAG, "the card is busy; '%s' refused", q->src);
            post_path_rsp(q->who, q->op, KODE_DENIED, false, false, 0U);
            reply_clear();
            break;
        }
        case STORAGE_TEXT_REQ_SIG: {
            StorageTextEvt const * const q = Q_EVT_CAST(StorageTextEvt);

            reply_set(q->wait, q->gen, q->who, q->rsp, NULL);
            KLOG_W(TAG, "the card is busy; '%s' refused", q->path);
            post_path_rsp(q->who, (uint8_t)STORAGE_PATH_WRITE, KODE_DENIED,
                          false, false, 0U);
            reply_clear();
            break;
        }
        case STORAGE_RECV_REQ_SIG: {
            StorageRecvEvt const * const q = Q_EVT_CAST(StorageRecvEvt);

            KLOG_W(TAG, "the card is busy; file_put of '%s' refused", q->path);
            recv_deny(q->who);
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
 * @brief Keep a request for later, or refuse it for want of room.
 *
 * @param me The AO.
 * @param e  The request.
 * @return true when it was held or refused; false when it could be neither
 *         and has to be served now.
 */
static bool card_hold(Storage * const me, QEvt const * const e)
{
    if ( QActive_defer(&me->super, &me->deferq, e) )
    {
        return true;
    }
    return card_refuse(me, e);
}

/**
 * @brief Take up what was held, once no call to the card is out.
 *
 * Run at the end of every event. ONE held request is recalled to the front of
 * the queue, so held requests keep their order ahead of later arrivals; the
 * next waits for the end of that event, which may put a call out.
 *
 * @param me The AO.
 */
static void card_pump(Storage * const me)
{
    if ( !me->sdj.busy )
    {
        (void)QActive_recall(&me->super, &me->deferq);
    }
}

/**
 * @brief The only state: hold what needs the card while a call to it is out, serve the rest.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Storage_active(Storage * const me, QEvt const * const e)
{
    QState status;

    if ( me->sdj.busy && needs_card(me, e) && card_hold(me, e) )
    {
        status = Q_HANDLED();
    }
    else
    {
        status = Storage_serve(me, e);
        if ( Q_HANDLED() == status )
        {
            card_pump(me);
        }
    }
    return status;
}
