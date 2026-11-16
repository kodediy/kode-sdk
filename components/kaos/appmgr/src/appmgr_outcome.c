/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file appmgr_outcome.c
 * @brief The record of the last app install and the last kodeOS update: numbered and kept.
 *
 * The block lives in PSRAM, not in AO_AppMgr, whose static object is held
 * under 2 KB of internal RAM. It is AO_AppMgr's alone: every function runs in
 * its thread.
 */
#include <stdio.h>
#include <string.h>

#include "kaos.h"
#include "kdrv_kfw.h"
#include "kdrv_slot.h"
#include "khal_nvs.h"
#include "khal_system.h"
#include "kode_log.h"

#include "appmgr_outcome.h"

#define TAG "kaos.appmgr"  /**< AO_AppMgr's tag. */

/** @brief The block both records and the install in hand live in. */
typedef struct {
    appmgr_inst_rec_t rec[APPMGR_INST_KINDS];  /**< The last of each kind.            */
    uint8_t           pending;                 /**< Kind a fresh request noted, or
                                                *   APPMGR_INST_KINDS for none.      */
    uint8_t           pending_src;             /**< Its asker's port.                 */
    uint8_t           open;                    /**< Kind taken and not ended, or
                                                *   APPMGR_INST_KINDS for none.      */
    uint8_t           why;                     /**< The cause noted for the open one. */
} appmgr_inst_t;

static appmgr_inst_t *s_inst;   /**< The block; NULL when PSRAM refused it. */

/** @brief The NVS key of each kind. */
static const char * const INST_KEY[APPMGR_INST_KINDS] = {
    APPMGR_INST_KEY_APP, APPMGR_INST_KEY_OS,
};

/**
 * @brief Keep one record in NVS. A refusal is logged; the record in RAM stands.
 * @param kind APPMGR_INST_APP or APPMGR_INST_OS.
 */
static void inst_store(uint8_t kind)
{
    if ( khal_nvs_set_blob(APPMGR_UNDO_NS, INST_KEY[kind], &s_inst->rec[kind],
                           sizeof(appmgr_inst_rec_t)) != KODE_OK )
    {
        KLOG_W(TAG, "cannot keep the record of install %lu",
               (unsigned long)s_inst->rec[kind].id);
    }
}

/**
 * @brief Close the install in hand with @p state and keep it.
 * @param state appmgr_inst_state_t.
 */
static void inst_finish(appmgr_inst_state_t state)
{
    uint8_t const kind = s_inst->open;

    s_inst->open = (uint8_t)APPMGR_INST_KINDS;
    s_inst->rec[kind].state = (uint8_t)state;
    inst_store(kind);
}

void appmgr_inst_init(void)
{
    if ( NULL == s_inst )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() hands back an untyped block aligned for any type */
        s_inst = (appmgr_inst_t *)khal_alloc_psram(sizeof(uint32_t),
                                                   sizeof(appmgr_inst_t));
    }
    if ( NULL == s_inst )
    {
        KLOG_W(TAG, "no PSRAM for the install records; installs are not "
                    "numbered");
        return;
    }
    (void)memset(s_inst, 0, sizeof(appmgr_inst_t));
    s_inst->pending = (uint8_t)APPMGR_INST_KINDS;
    s_inst->open    = (uint8_t)APPMGR_INST_KINDS;

    for ( uint8_t k = 0U; (uint8_t)APPMGR_INST_KINDS > k; ++k )
    {
        appmgr_inst_rec_t * const r = &s_inst->rec[k];
        size_t len = sizeof(appmgr_inst_rec_t);

        if ( (khal_nvs_get_blob(APPMGR_UNDO_NS, INST_KEY[k], r, &len) != KODE_OK)
            || (sizeof(appmgr_inst_rec_t) != len)
            || ((uint8_t)APPMGR_INST_LAYOUT != r->layout) )
        {
            (void)memset(r, 0, sizeof(appmgr_inst_rec_t));
        }
        r->name[APPMGR_INST_NAME_MAX - 1U]   = '\0';
        r->version[APPMGR_INST_VER_MAX - 1U] = '\0';
        if ( (uint8_t)APPMGR_INST_RUNNING == r->state )
        {
            r->state = (uint8_t)APPMGR_INST_FAILED;
            r->why   = (uint8_t)APPMGR_WHY_RESET;
            inst_store(k);
        }
    }
}

void appmgr_inst_prepare(uint8_t src_kind, uint8_t dst_kind, uint8_t asker)
{
    if ( NULL != s_inst )
    {
        s_inst->pending = (uint8_t)APPMGR_INST_KINDS;
        if ( (uint8_t)KFW_SRC_PENDING == src_kind )
        {
            /* The mailbox's install is the host's bench path: no record. */
        }
        else if ( (uint8_t)KFW_TARGET_APP == dst_kind )
        {
            s_inst->pending = (uint8_t)APPMGR_INST_APP;
        }
        else if ( (uint8_t)KFW_TARGET_OS == dst_kind )
        {
            s_inst->pending = (uint8_t)APPMGR_INST_OS;
        }
        else
        {
            /* The mailbox as a destination, the coprocessor: no record. */
        }
        s_inst->pending_src = asker;
    }
}

void appmgr_inst_taken(void)
{
    if ( (NULL == s_inst) || ((uint8_t)APPMGR_INST_KINDS == s_inst->pending) )
    {
        return;
    }
    uint8_t const kind = s_inst->pending;
    uint32_t      id   = s_inst->rec[APPMGR_INST_APP].id;

    if ( s_inst->rec[APPMGR_INST_OS].id > id )
    {
        id = s_inst->rec[APPMGR_INST_OS].id;
    }
    ++id;
    if ( 0U == id )
    {
        id = 1U;   /* 0 is "none" */
    }
    appmgr_inst_rec_t * const r = &s_inst->rec[kind];
    (void)memset(r, 0, sizeof(appmgr_inst_rec_t));
    r->id     = id;
    r->layout = (uint8_t)APPMGR_INST_LAYOUT;
    r->state  = (uint8_t)APPMGR_INST_RUNNING;
    r->src    = s_inst->pending_src;
    s_inst->pending = (uint8_t)APPMGR_INST_KINDS;
    s_inst->open    = kind;
    s_inst->why     = (uint8_t)APPMGR_WHY_NONE;
    inst_store(kind);
}

void appmgr_inst_named(const char *name)
{
    if ( (NULL != s_inst) && ((uint8_t)APPMGR_INST_KINDS != s_inst->open)
        && (NULL != name) && ('\0' != name[0]) )
    {
        appmgr_inst_rec_t * const r = &s_inst->rec[s_inst->open];

        (void)snprintf(r->name, sizeof(r->name), "%s", name);
    }
}

void appmgr_inst_why(appmgr_inst_why_t why)
{
    if ( (NULL != s_inst) && ((uint8_t)APPMGR_INST_KINDS != s_inst->open)
        && ((uint8_t)APPMGR_WHY_NONE == s_inst->why) )
    {
        s_inst->why = (uint8_t)why;
    }
}

void appmgr_inst_failed(uint32_t pct)
{
    if ( NULL == s_inst )
    {
        return;
    }
    s_inst->pending = (uint8_t)APPMGR_INST_KINDS;
    if ( (uint8_t)APPMGR_INST_KINDS != s_inst->open )
    {
        appmgr_inst_rec_t * const r = &s_inst->rec[s_inst->open];

        r->why = s_inst->why;
        if ( (uint8_t)APPMGR_WHY_NONE == r->why )
        {
            r->why = (uint8_t)APPMGR_WHY_OTHER;
        }
        r->pct = (uint8_t)pct;
        inst_finish(APPMGR_INST_FAILED);
    }
}

void appmgr_inst_installed(const char *name, const char *version)
{
    if ( (NULL != s_inst) && ((uint8_t)APPMGR_INST_APP == s_inst->open) )
    {
        appmgr_inst_rec_t * const r = &s_inst->rec[APPMGR_INST_APP];

        (void)snprintf(r->name, sizeof(r->name), "%s", name);
        (void)snprintf(r->version, sizeof(r->version), "%s", version);
        r->pct = (uint8_t)APPMGR_INST_PCT_DONE;
        inst_finish(APPMGR_INST_INSTALLED);
    }
}

void appmgr_inst_rebooting(void)
{
    if ( (NULL != s_inst) && ((uint8_t)APPMGR_INST_OS == s_inst->open) )
    {
        appmgr_inst_rec_t * const r = &s_inst->rec[APPMGR_INST_OS];
        kdrv_slot_desc_t desc;

        if ( kdrv_slot_desc(&desc) == KODE_OK )
        {
            (void)snprintf(r->name, sizeof(r->name), "%s", desc.project);
            (void)snprintf(r->version, sizeof(r->version), "%s", desc.version);
        }
        r->slot = kdrv_slot_address();
        r->pct  = (uint8_t)APPMGR_INST_PCT_DONE;
        inst_finish(APPMGR_INST_REBOOTING);
    }
}
