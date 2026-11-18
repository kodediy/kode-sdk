/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file appmgr_verbs.c
 * @brief AO_AppMgr's console verbs: `app_list`, `app_run`, `app_erase`, `app_cat` and `install_info`, registered by kaos_appmgr_start().
 */
#include <stdio.h>
#include <inttypes.h>
#include <string.h>

#include "kaos.h"
#include "kapp_cat.h"
#include "kdrv_kfw.h"
#include "kode_log.h"
#include "kqp.h"

#include "console_verbs.h"
#include "kverbs.h"

#define TAG "kcli"  /**< The console's tag, which `log_level tag="kcli"` governs. */

#define APPMGR_V_PROJECT_MAX  32U    /**< A project name, as kapp_cat_row_t keeps it. */
#define APPMGR_V_VERSION_MAX  16U    /**< A version, as AppEntryEvt carries it. */
#define APPMGR_V_BYTES_PER_KB 1024U  /**< `size_kb=`'s unit. */
#define APPMGR_V_ERASE_MARGIN_MS 5000U  /**< What `app_erase` waits past AO_AppMgr's own bound on AO_Storage. */
#define APPMGR_V_ERASE_MS     (APPMGR_WIPE_MS + APPMGR_V_ERASE_MARGIN_MS) /**< `app_erase`'s deadline. */
#define APPMGR_V_RUN_MS       (KODE_APPMGR_OPEN_MS + APPMGR_V_ERASE_MARGIN_MS) /**< `app_run`'s: AO_AppMgr's bound on a medium's open, and the margin. */
#define APPMGR_V_FILE_MS      1000U  /**< `app_list files=on`: what one question to AO_Storage, a page or a manifest, adds to the deadline. */
#define APPMGR_V_FS_PREFIX    "/storage/"  /**< The internal partition's root, before a file's name. */
#define APPMGR_V_AT_APP       "app"  /**< `at=`: installed, in the apps region. */
#define APPMGR_V_AT_SD        "sd"   /**< `at=`: a file on the card. */
#define APPMGR_V_AT_FS        "fs"   /**< `at=`: a file on the internal partition. */

/** @brief Which app verb holds the console. */
typedef enum {
    APPMGR_V_NONE = 0,  /**< None of them.                                  */
    APPMGR_V_LIST,      /**< app_list: APP_ENTRY_SIG pages until the end.   */
    APPMGR_V_CAT,       /**< app_cat set/clear: closed by APP_CAT_RSP_SIG.  */
    APPMGR_V_ERASE,     /**< app_erase: closed by APP_ERASE_DONE_SIG.       */
    APPMGR_V_RUN,       /**< app_run: closed by APP_REQ_RSP_SIG.            */
    APPMGR_V_INFO       /**< install_info: closed by APP_REQ_RSP_SIG.       */
} appmgr_verb_kind_t;

static appmgr_verb_kind_t s_appmgr_kind;

/** @brief APPMGR_V_ERASE: the app whose erase holds the console. The end of
 *         an erase is published, and the launcher's ends on the same signal. */
static char s_appmgr_erasing[APPMGR_V_PROJECT_MAX];

/** @brief APPMGR_V_LIST: the position the next entry must carry, and the skip
 *         of the next page. */
static uint16_t s_appmgr_next;

/** @brief APPMGR_V_LIST: the skip the last page was asked with; a page that
 *         ends with `more` and leaves s_appmgr_next here brought nothing. */
static uint16_t s_appmgr_from;

/** @brief One app file a page of AO_Storage's listing named. */
typedef struct {
    char     name[KODE_APP_FILE_MAX];  /**< Its file name, as the listing gave it. */
    uint32_t size;                     /**< Its size in bytes.                     */
    bool     from_sd;                  /**< On the card; false is the internal partition. */
} appmgr_file_t;

/** @brief APPMGR_V_LIST: `files=on` was asked, so every record says where its
 *         app is and the app files follow the installed apps. */
static bool s_appmgr_files;

/** @brief APPMGR_V_LIST: the installed apps have ended and the app files are
 *         being listed. s_appmgr_next and s_appmgr_from count files from here. */
static bool s_appmgr_on_files;

/** @brief APPMGR_V_LIST: the app files of the page in hand, kept while their
 *         manifests are read one at a time. One page, never the whole listing. */
static appmgr_file_t s_appmgr_page[KODE_STORAGE_LIST_MAX];
static uint8_t       s_appmgr_page_n;     /**< Files the page in hand holds.       */
static uint8_t       s_appmgr_page_at;    /**< The one whose manifest is awaited.  */
static bool          s_appmgr_page_more;  /**< Another page follows this one.      */

/** @brief APPMGR_V_LIST: the path the manifest in flight was asked with; the
 *         answer echoes it, and any other answer is not this listing's. */
static char s_appmgr_asked[KODE_APP_FILE_MAX];

/**
 * @brief app_list [files=on|off]: records arrive as APP_ENTRY_SIG pages, closed by the end.
 *
 * With `files=on` every record ends with `at=`, and after the installed apps
 * come the app files on the card and on the internal partition, one record
 * each: the apps the board holds but has not installed, and the copies of the
 * ones it has. Without it, or with `off`, the answer is the installed apps
 * and nothing else, as it has always been.
 */
static void h_app_list(const kverb_t *c, const karg_val_t *v)
{
    console_begin(c->verb);
    s_appmgr_kind = APPMGR_V_LIST;
    s_appmgr_files    = v[0].present && v[0].b;
    s_appmgr_on_files = false;
    s_appmgr_next = 0U;
    s_appmgr_from = 0U;
    appmgr_list_request(AO_Console);
}

/**
 * @brief app_run name=<name|file>: a .bin is flashed then booted.
 *
 * The line stays open until AO_AppMgr says whether it took the request: `>ok`
 * when the app is on its way to boot, `missing` for a name nobody installed or
 * a file that is not on a medium, `busy` when AO_AppMgr has work in hand.
 */
static void h_app_run(const kverb_t *c, const karg_val_t *v)
{
    bool posted;

    console_begin(c->verb);
    console_extend(APPMGR_V_RUN_MS);
    s_appmgr_kind = APPMGR_V_RUN;
    if ( console_is_bin(v[0].s) )
    {
        /* Flash, then boot it. */
        posted = appmgr_image_ask_from((uint8_t)KFW_SRC_FS, v[0].s,
                                       (uint8_t)KFW_TARGET_APP, true,
                                       AO_Console, console_src());
    }
    else
    {
        posted = appmgr_run_ask(v[0].s, AO_Console);
    }
    if ( !posted )
    {
        /* Nothing was posted, so nobody will answer. */
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        console_end();
    }
}

/**
 * @brief install_info: the record of the last app install and of the last
 *        kodeOS update, one `>|install` each, then `>ok`.
 *
 * AO_AppMgr writes the records itself, the way it writes them when an install
 * is taken and when it ends, so the line stays open until it says it is done.
 */
static void h_install_info(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_appmgr_kind = APPMGR_V_INFO;
    if ( !appmgr_install_info_ask(AO_Console, console_src()) )
    {
        /* Nothing was posted, so nobody will answer. */
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        console_end();
    }
}

/**
 * @brief app_erase name=<name>: take everything of one app off the board.
 *
 * The line stays open until AO_AppMgr publishes the end of the erase, under
 * a deadline past the one AO_AppMgr gives AO_Storage.
 */
static void h_app_erase(const kverb_t *c, const karg_val_t *v)
{
    console_begin(c->verb);
    console_extend(APPMGR_V_ERASE_MS);
    s_appmgr_kind = APPMGR_V_ERASE;
    (void)snprintf(s_appmgr_erasing, sizeof(s_appmgr_erasing), "%s", v[0].s);
    appmgr_erase_request(v[0].s);
}

/** @brief app_cat with no name: the override table, read synchronously. */
static void appmgr_cat_list(void)
{
    char project[APPMGR_V_PROJECT_MAX];
    char cats[KODE_APP_CATEGORY_MAX];
    uint8_t const n = kapp_cat_count();   /* one lock for the count; each row
                                           * takes its own */

    for ( uint8_t i = 0U; i < n; ++i )
    {
        if ( kapp_cat_row(i, project, sizeof(project), cats, sizeof(cats)) )
        {
            char qp[CONSOLE_QUOTED_MAX(sizeof(project))];
            char qc[CONSOLE_QUOTED_MAX(sizeof(cats))];

            console_reply(">|cat project=%s cats=%s",
                          console_quote(qp, sizeof(qp), project),
                          console_quote(qc, sizeof(qc), cats));
        }
    }
    console_reply(">ok");
}

/**
 * @brief app_cat: read the override table, or ask AO_AppMgr to change it.
 *
 * Reading is synchronous: the table is guarded. Only the write, which touches
 * flash, goes through AO_AppMgr, the table's one writer.
 */
static void h_app_cat(const kverb_t *c, const karg_val_t *v)
{
    bool const has_set   = v[1].present;
    bool const has_clear = v[2].present && v[2].b;

    if ( !v[0].present )
    {
        /* `set`/`clear` with no `name` is a write with nowhere to write to,
         * not "list the table" wearing a typo. */
        if ( v[1].present || v[2].present )
        {
            console_reply_err(KCLI_ERR_MISSING, "name");
        }
        else
        {
            appmgr_cat_list();
        }
    }
    else if ( !has_set && !has_clear )
    {
        /* Exactly one of set and clear: neither is a typo not to guess at. */
        console_reply_err(KCLI_ERR_MISSING, c->verb);
    }
    else if ( has_set && has_clear )
    {
        console_reply_err(KCLI_ERR_VALUE, c->verb);
    }
    else
    {
        /* Only the write is asynchronous: the one path that reaches AO_AppMgr
         * and touches flash, closed by APP_CAT_RSP_SIG. */
        const char *cats = "";

        if ( has_set )
        {
            cats = v[1].s;
        }
        console_begin(c->verb);
        s_appmgr_kind = APPMGR_V_CAT;
        appmgr_cat_request(v[0].s, cats, has_clear);
    }
}

/**
 * @brief One app_list record. The fallbacks live here, in the one place that
 *        renders for a human, not in AO_AppMgr.
 *
 * @param name     The project name.
 * @param version  Its version; empty prints as `-`.
 * @param size     The image's size in bytes.
 * @param category Its category; empty prints as `other`.
 * @param display  Its display name; empty prints as @p name.
 * @param at       `files=on` only: where the app is. NULL leaves the record
 *                 as `app_list` has always printed it.
 * @param file     With @p at: the file's name on its medium, or NULL for an
 *                 installed app, which has none.
 */
static void appmgr_record(const char *name, const char *version, uint32_t size,
                          const char *category, const char *display,
                          const char *at, const char *file)
{
    char qn[CONSOLE_QUOTED_MAX(APPMGR_V_PROJECT_MAX)];
    char qv[CONSOLE_QUOTED_MAX(APPMGR_V_VERSION_MAX)];
    char qc[CONSOLE_QUOTED_MAX(KODE_APP_CATEGORY_MAX)];
    char qd[CONSOLE_QUOTED_MAX(KODE_APP_DISPLAY_MAX)];
    char qf[CONSOLE_QUOTED_MAX(KODE_APP_FILE_MAX)];
    /* What `files=on` adds after the record `app_list` has always printed. */
    char tail[sizeof(" at= file=") + sizeof(APPMGR_V_AT_APP) + sizeof(qf)];

    const char *shown_category = "other";
    const char *shown_display  = name;

    if ( '\0' != category[0] )
    {
        shown_category = category;
    }
    if ( '\0' != display[0] )
    {
        shown_display = display;
    }
    tail[0] = '\0';
    if ( (NULL != at) && (NULL != file) )
    {
        (void)snprintf(tail, sizeof(tail), " at=%s file=%s", at,
                       console_quote(qf, sizeof(qf), file));
    }
    else if ( NULL != at )
    {
        (void)snprintf(tail, sizeof(tail), " at=%s", at);
    }
    else
    {
        /* The record as it has always been. */
    }
    console_reply(">|name=%s version=%s size_kb=%" PRIu32 " category=%s display=%s%s",
                  console_quote(qn, sizeof(qn), name),
                  console_kv_text(qv, sizeof(qv), version),
                  (uint32_t)(size / APPMGR_V_BYTES_PER_KB),
                  console_quote(qc, sizeof(qc), shown_category),
                  console_quote(qd, sizeof(qd), shown_display), tail);
}

/** @brief Close app_list: every record that was going to be printed has been. */
static void appmgr_list_done(void)
{
    console_reply(">ok");
    console_end();
}

/**
 * @brief Ask for the manifest of the next app file of the page in hand, for
 *        the next page once the page is through, or close the listing.
 */
static void appmgr_file_next(void)
{
    if ( s_appmgr_page_at < s_appmgr_page_n )
    {
        appmgr_file_t const * const f = &s_appmgr_page[s_appmgr_page_at];
        const char *prefix = APPMGR_V_FS_PREFIX;
        int         len;

        if ( f->from_sd )
        {
            prefix = KODE_CARD_APP_PREFIX;
        }
        /* By path, so the file read is the one the listing named: a bare
         * name is searched for card first, and the two media may each hold a
         * file of that name. A name too long to carry its volume is asked
         * bare, which is all the request has room for. */
        len = snprintf(s_appmgr_asked, sizeof(s_appmgr_asked), "%s%s", prefix, f->name);
        if ( (0 > len) || ((size_t)len >= sizeof(s_appmgr_asked)) )
        {
            (void)snprintf(s_appmgr_asked, sizeof(s_appmgr_asked), "%s", f->name);
        }
        console_extend(APPMGR_V_FILE_MS);
        storage_desc_request(s_appmgr_asked, AO_Console);
    }
    else if ( s_appmgr_page_more )
    {
        s_appmgr_page_n  = 0U;
        s_appmgr_page_at = 0U;
        s_appmgr_from    = s_appmgr_next;
        console_extend(APPMGR_V_FILE_MS);
        storage_list_page(s_appmgr_next, AO_Console);
    }
    else
    {
        appmgr_list_done();
    }
}

/** @brief The installed apps have ended: go on to the app files. */
static void appmgr_files_begin(void)
{
    s_appmgr_on_files  = true;
    s_appmgr_next      = 0U;
    s_appmgr_from      = 0U;
    s_appmgr_page_n    = 0U;
    s_appmgr_page_at   = 0U;
    s_appmgr_page_more = false;
    s_appmgr_asked[0]  = '\0';
    console_extend(APPMGR_V_FILE_MS);
    /* The card is mounted again first, so one put in or written on a
     * computer since the last look is listed as it is now. */
    storage_list_request(AO_Console);
}

/** @brief `files=on`: a page of the app files, and the manifest of each. */
static bool appmgr_files_listing(QEvt const *e)
{
    bool handled = false;

    if ( (QSignal)STORAGE_ENTRY_SIG == e->sig )
    {
        StorageEntryEvt const * const en = Q_EVT_CAST(StorageEntryEvt);

        /* A page asked twice repeats its entries: only the next position is new. */
        if ( en->pos == s_appmgr_next )
        {
            ++s_appmgr_next;
            if ( !en->is_dir && console_is_bin(en->name)
                && (s_appmgr_page_n < (uint8_t)KODE_STORAGE_LIST_MAX) )
            {
                appmgr_file_t * const f = &s_appmgr_page[s_appmgr_page_n];

                (void)snprintf(f->name, sizeof(f->name), "%s", en->name);
                f->size    = en->size;
                f->from_sd = en->from_sd;
                ++s_appmgr_page_n;
            }
        }
        handled = true;
    }
    else if ( (QSignal)STORAGE_LIST_END_SIG == e->sig )
    {
        bool const more = Q_EVT_CAST(ListEndEvt)->more;

        if ( more && (s_appmgr_next == s_appmgr_from) )
        {
            /* A page that ends with `more` but brought nothing would be asked
             * again for ever, as with the installed apps. */
            console_reply_err(KCLI_ERR_REJECT, "app_list");
            console_end();
        }
        else
        {
            s_appmgr_page_more = more;
            s_appmgr_page_at   = 0U;
            appmgr_file_next();
        }
        handled = true;
    }
    else if ( (QSignal)STORAGE_DESC_RSP_SIG == e->sig )
    {
        StorageDescRspEvt const * const r = Q_EVT_CAST(StorageDescRspEvt);

        /* Answers can cross: only the one that echoes the path asked is the
         * manifest of the file in hand. */
        if ( (s_appmgr_page_at < s_appmgr_page_n)
            && (strcmp(r->name, s_appmgr_asked) == 0) )
        {
            appmgr_file_t const * const f = &s_appmgr_page[s_appmgr_page_at];

            if ( ((kode_status_t)r->status == KODE_OK) && ('\0' != r->project[0]) )
            {
                const char *at = APPMGR_V_AT_FS;

                if ( f->from_sd )
                {
                    at = APPMGR_V_AT_SD;
                }
                appmgr_record(r->project, r->version, f->size, r->category,
                              r->display, at, f->name);
            }
            else
            {
                /* Not an app image, or one that would not be read: it is no
                 * app, and the listing goes on without it. */
                KLOG_D(TAG, "app_list: '%s' says nothing of an app (status %" PRIu32 ")",
                       f->name, (uint32_t)r->status);
            }
            ++s_appmgr_page_at;
            appmgr_file_next();
        }
        handled = true;
    }
    else
    {
        /* Not the listing's. */
    }
    return handled;
}

/** @brief app_list's pages: an entry at the expected position, and the end of a page. */
static bool appmgr_listing(QEvt const *e)
{
    bool handled = false;

    if ( s_appmgr_on_files )
    {
        handled = appmgr_files_listing(e);
    }
    else if ( (QSignal)APP_ENTRY_SIG == e->sig )
    {
        AppEntryEvt const * const en = Q_EVT_CAST(AppEntryEvt);

        /* A page asked twice repeats its entries: only the next position is new. */
        if ( en->pos == s_appmgr_next )
        {
            const char *at = NULL;

            if ( s_appmgr_files )
            {
                at = APPMGR_V_AT_APP;
            }
            ++s_appmgr_next;
            appmgr_record(en->name, en->version, en->size, en->category,
                          en->display, at, NULL);
        }
        handled = true;
    }
    else if ( (QSignal)APP_LIST_END_SIG == e->sig )
    {
        bool const more  = Q_EVT_CAST(ListEndEvt)->more;
        /* A page that ends with `more` but brought nothing would be asked
         * again for ever: an owner out of event blocks answers that way. */
        bool const stuck = more && (s_appmgr_next == s_appmgr_from);

        if ( stuck )
        {
            console_reply_err(KCLI_ERR_REJECT, "app_list");
            console_end();
        }
        else if ( more )
        {
            s_appmgr_from = s_appmgr_next;
            appmgr_list_page(s_appmgr_next, AO_Console);
        }
        else if ( s_appmgr_files )
        {
            appmgr_files_begin();
        }
        else
        {
            appmgr_list_done();
        }
        handled = true;
    }
    else
    {
        /* Not the listing's. */
    }
    return handled;
}

/** @brief app_cat's answer. */
static bool appmgr_cat_answer(QEvt const *e)
{
    bool handled = false;

    if ( (QSignal)APP_CAT_RSP_SIG == e->sig )
    {
        AppCatRspEvt const * const r = Q_EVT_CAST(AppCatRspEvt);

        if ( (kode_status_t)r->status == KODE_OK )
        {
            console_reply(">ok");
        }
        else
        {
            /* kapp_cat_set() answers KODE_ERROR for a list that normalises
             * to nothing AND for an NVS write that failed, so the real status
             * goes out as a record ahead of the error. */
            kcli_err_t code = KCLI_ERR_VALUE;

            if ( (uint8_t)KODE_DENIED == r->status )
            {
                code = KCLI_ERR_REJECT;
            }
            KLOG_W(TAG, "app_cat refused: kode_status_t=%" PRIu32, (uint32_t)r->status);
            console_reply(">|app_cat result=refused status=%s",
                          console_status_word((uint32_t)r->status));
            console_reply_err(code, "app_cat");
        }
        console_end();
        handled = true;
    }
    return handled;
}

/**
 * @brief One medium's record of an erase: what went, what stayed, or absent.
 * @param where "fs" or "sd".
 * @param m     The medium's result.
 */
static void appmgr_erase_medium(const char *where, const AppWipeMedium *m)
{
    const char *folder = "none";

    if ( !m->present )
    {
        console_reply(">|erased where=%s state=absent", where);
        return;
    }
    if ( (uint8_t)APP_WIPE_FOLDER_REMOVED == m->folder )
    {
        folder = "removed";
    }
    else if ( (uint8_t)APP_WIPE_FOLDER_LEFT == m->folder )
    {
        folder = "left";
    }
    else
    {
        /* There was none. */
    }
    console_reply(">|erased where=%s files=%u left=%u folder=%s", where,
                  (unsigned)m->files, (unsigned)m->left, folder);
}

/** @brief app_erase's answer: the end of the erase of the app it named. */
static bool appmgr_erase_answer(QEvt const *e)
{
    AppEraseDoneEvt const *d;
    char                   qn[CONSOLE_QUOTED_MAX(KODE_APP_PROJECT_MAX)];
    const char            *image = "none";
    kode_status_t          st;

    if ( (QSignal)APP_ERASE_DONE_SIG != e->sig )
    {
        return false;
    }
    d = Q_EVT_CAST(AppEraseDoneEvt);
    if ( strcmp(d->name, s_appmgr_erasing) != 0 )
    {
        return false;   /* another app's erase: the launcher's */
    }
    st = (kode_status_t)d->status;
    if ( d->image )
    {
        image = "removed";
    }

    /* The records say what was found, so an erase that found nothing and one
     * refused outright have none; an image that went is said whatever came
     * after it. */
    if ( (KODE_OK == st) || (KODE_IO_ERROR == st) || d->image )
    {
        console_reply(">|erased name=%s image=%s",
                      console_quote(qn, sizeof(qn), d->name), image);
    }
    if ( (KODE_OK == st) || (KODE_IO_ERROR == st) )
    {
        appmgr_erase_medium("fs", &d->fs);
        appmgr_erase_medium("sd", &d->sd);
    }

    switch ( st )
    {
        case KODE_OK: {
            console_reply(">ok");
            break;
        }
        case KODE_ERROR: {
            console_reply_err(KCLI_ERR_REJECT, "missing");
            break;
        }
        case KODE_IO_ERROR: {
            console_reply_err(KCLI_ERR_REJECT, "left");
            break;
        }
        case KODE_DENIED: {
            console_reply_err(KCLI_ERR_BUSY, "busy");
            break;
        }
        default: {
            /* AO_Storage kept silent or had no room: the files were not
             * looked at. */
            console_reply_err(KCLI_ERR_TIMEOUT, "storage");
            break;
        }
    }
    console_end();
    return true;
}

/** @brief app_run's and install_info's answer: AO_AppMgr's verdict on the request. */
static bool appmgr_run_answer(QEvt const *e)
{
    bool handled = false;

    if ( (QSignal)APP_REQ_RSP_SIG == e->sig )
    {
        console_reply_app_verdict(Q_EVT_CAST(AppReqRspEvt)->verdict);
        handled = true;
    }
    return handled;
}

/** @brief AO_AppMgr's answers to the verb in flight. */
static bool appmgr_answer(QEvt const *e)
{
    bool handled = false;

    if ( APPMGR_V_LIST == s_appmgr_kind )
    {
        handled = appmgr_listing(e);
    }
    else if ( APPMGR_V_CAT == s_appmgr_kind )
    {
        handled = appmgr_cat_answer(e);
    }
    else if ( APPMGR_V_ERASE == s_appmgr_kind )
    {
        handled = appmgr_erase_answer(e);
    }
    else if ( (APPMGR_V_RUN == s_appmgr_kind) || (APPMGR_V_INFO == s_appmgr_kind) )
    {
        handled = appmgr_run_answer(e);
    }
    else
    {
        /* None of them. */
    }
    return handled;
}

/* `files` is optional: without it `app_list` answers the installed apps alone. */
static const karg_spec_t A_APP_LIST[] = {
    { .key = "files", .required = false, .type = KARG_BOOL },
};
static const karg_spec_t A_APP_NAME[] = {
    { .key = "name", .required = true, .type = KARG_STRING },
};
/* `name` is optional here: `app_cat` with no arguments lists the table. `set`
 * and `clear` are optional too - h_app_cat enforces exactly one of them. */
static const karg_spec_t A_APP_CAT[] = {
    { .key = "name",  .required = false, .type = KARG_STRING },
    { .key = "set",   .required = false, .type = KARG_STRING },
    { .key = "clear", .required = false, .type = KARG_BOOL   },
};

static const kverb_t APPMGR_VERBS[] = {
    { "app_list",  A_APP_LIST, KVERB_KEYS(A_APP_LIST), h_app_list  },
    { "app_run",   A_APP_NAME, KVERB_KEYS(A_APP_NAME), h_app_run   },
    { "app_erase", A_APP_NAME, KVERB_KEYS(A_APP_NAME), h_app_erase },
    { "app_cat",   A_APP_CAT,  KVERB_KEYS(A_APP_CAT),  h_app_cat   },
    { "install_info", NULL,    0U,                     h_install_info },
};

const kverbs_t appmgr_verbs = {
    .verbs  = APPMGR_VERBS, .n = sizeof(APPMGR_VERBS) / sizeof(APPMGR_VERBS[0]),
    .answer = appmgr_answer,
};
