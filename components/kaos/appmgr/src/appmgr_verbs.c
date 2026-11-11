/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file appmgr_verbs.c
 * @brief AO_AppMgr's console verbs: `app_list`, `app_run`, `app_erase` and `app_cat`, registered by kaos_appmgr_start().
 */
#include <stdio.h>
#include <inttypes.h>
#include <string.h>

#include "kaos.h"
#include "kapp_cat.h"
#include "kode_log.h"
#include "kqp.h"

#include "console_verbs.h"
#include "kverbs.h"

#define TAG "kcli"  /**< The console's tag, which `log_level tag="kcli"` governs. */

#define APPMGR_V_PROJECT_MAX  32U    /**< A project name, as kapp_cat_row_t keeps it. */
#define APPMGR_V_BYTES_PER_KB 1024U  /**< `size_kb=`'s unit. */
#define APPMGR_V_ERASE_MARGIN_MS 5000U  /**< What `app_erase` waits past AO_AppMgr's own bound on AO_Storage. */
#define APPMGR_V_ERASE_MS     (APPMGR_WIPE_MS + APPMGR_V_ERASE_MARGIN_MS) /**< `app_erase`'s deadline. */

/** @brief Which app verb holds the console. */
typedef enum {
    APPMGR_V_NONE = 0,  /**< None of them.                                  */
    APPMGR_V_LIST,      /**< app_list: APP_ENTRY_SIG pages until the end.   */
    APPMGR_V_CAT,       /**< app_cat set/clear: closed by APP_CAT_RSP_SIG.  */
    APPMGR_V_ERASE      /**< app_erase: closed by APP_ERASE_DONE_SIG.       */
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

/** @brief app_list: records arrive as APP_ENTRY_SIG pages, closed by the end. */
static void h_app_list(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_appmgr_kind = APPMGR_V_LIST;
    s_appmgr_next = 0U;
    s_appmgr_from = 0U;
    appmgr_list_request(AO_Console);
}

/** @brief app_run name=<name|file>: a .bin is flashed then booted. */
static void h_app_run(const kverb_t *c, const karg_val_t *v)
{
    (void)c;
    if ( console_is_bin(v[0].s) )
    {
        appmgr_flash_request(v[0].s, true);   /* flash, then boot it */
    }
    else
    {
        appmgr_run_request(v[0].s);
    }
    console_reply(">ok");
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

/** @brief One app_list record. The fallbacks live here, in the one place that
 *         renders for a human, not in AO_AppMgr. */
static void appmgr_entry(AppEntryEvt const *en)
{
    char const *category = "other";
    char const *display  = en->name;
    char qn[CONSOLE_QUOTED_MAX(sizeof(en->name))];
    char qv[CONSOLE_QUOTED_MAX(sizeof(en->version))];
    char qc[CONSOLE_QUOTED_MAX(sizeof(en->category))];
    char qd[CONSOLE_QUOTED_MAX(sizeof(en->display))];

    if ( '\0' != en->category[0] )
    {
        category = en->category;
    }
    if ( '\0' != en->display[0] )
    {
        display = en->display;
    }
    console_reply(">|name=%s version=%s size_kb=%" PRIu32 " category=%s display=%s",
                  console_quote(qn, sizeof(qn), en->name),
                  console_kv_text(qv, sizeof(qv), en->version),
                  (uint32_t)(en->size / APPMGR_V_BYTES_PER_KB),
                  console_quote(qc, sizeof(qc), category),
                  console_quote(qd, sizeof(qd), display));
}

/** @brief app_list's pages: an entry at the expected position, and the end of a page. */
static bool appmgr_listing(QEvt const *e)
{
    bool handled = false;

    if ( (QSignal)APP_ENTRY_SIG == e->sig )
    {
        AppEntryEvt const * const en = Q_EVT_CAST(AppEntryEvt);

        /* A page asked twice repeats its entries: only the next position is new. */
        if ( en->pos == s_appmgr_next )
        {
            ++s_appmgr_next;
            appmgr_entry(en);
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
        else
        {
            console_reply(">ok");
            console_end();
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
    else
    {
        /* None of them. */
    }
    return handled;
}

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
    { "app_list",  NULL,       0U,                     h_app_list  },
    { "app_run",   A_APP_NAME, KVERB_KEYS(A_APP_NAME), h_app_run   },
    { "app_erase", A_APP_NAME, KVERB_KEYS(A_APP_NAME), h_app_erase },
    { "app_cat",   A_APP_CAT,  KVERB_KEYS(A_APP_CAT),  h_app_cat   },
};

const kverbs_t appmgr_verbs = {
    .verbs  = APPMGR_VERBS, .n = sizeof(APPMGR_VERBS) / sizeof(APPMGR_VERBS[0]),
    .answer = appmgr_answer,
};
