/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kbsp_owners.c
 * @brief The ownership table: one atomic token per resource, no lock.
 */
#include "kbsp_owners.h"

#include <stdatomic.h>
#include <stddef.h>

/* A slot is free when its token is NULL; it changes only by compare-and-swap.
 * The AOs claim from their own tasks, on either core: a plain read-then-write
 * would let two of them see the slot free and both take it, while one swap
 * lets exactly one win and hands the loser the winner. The token is a
 * pointer, not a flag, because a second claim by the holder, a release and
 * the double-claim message all need to know WHO holds it. The name is atomic
 * too, since `ps` reads it while a claim may be writing it. */
typedef struct {
    _Atomic(const void *) token;  /* the owner (the AO); NULL = free */
    _Atomic(const char *) name;   /* what `ps` prints */
} holder_t;

static holder_t s_holders[KBSP_RES_MAX];

/* Designated initialisers: a missing name is a NULL the host suite catches. */
static const char *const RES_NAMES[KBSP_RES_MAX] = {
    [KBSP_RES_LED]          = "LED (KTD2026)",
    [KBSP_RES_PANEL]        = "panel (CO5300)",
    [KBSP_RES_TOUCH]        = "touch",
    [KBSP_RES_IOEXP]        = "expander (TCA6408A) + INT",
    [KBSP_RES_BTN_TOP]      = "GPIO35 (button top)",
    [KBSP_RES_SDMMC]        = "microSD",
    [KBSP_RES_FATFS]        = "storage partition",
    [KBSP_RES_OTA_SLOT]     = "kodeOS slot",
    [KBSP_RES_OTADATA]      = "otadata",
    [KBSP_RES_APPS_REGION]  = "apps region",
    [KBSP_RES_APPS_BOOTREQ] = "apps boot request",
    [KBSP_RES_APP_PENDING]  = "app_pending mailbox",
    [KBSP_RES_PMIC]         = "PMIC (BQ25896)",
    [KBSP_RES_GAUGE]        = "gauge (CW2217)",
    [KBSP_RES_CONSOLE]      = "console transport",
    [KBSP_RES_C5]           = "ESP32-C5 (radio)",
    [KBSP_RES_JPEG]         = "JPEG codec",
    [KBSP_RES_PPA]          = "PPA (pixel accelerator)",
    [KBSP_RES_IR]           = "infrared (TSOP75338 + 2x SFH4045N)",
    [KBSP_RES_NFC]          = "NFC reader (ST25R3916B)",
    [KBSP_RES_AUDIO]        = "audio (ES8311 + NS4150B)",
    [KBSP_RES_IMU]          = "IMU (QMI8658A) + magnetometer (QMC5883P)",
    [KBSP_RES_HAPTIC]       = "haptic (AW86233)",
    [KBSP_RES_RFID]         = "rfid-lf",
    [KBSP_RES_EXP]          = "expansion header (J3)",
};

bool kbsp_claim(kbsp_res_t res, const void *owner, const char *name)
{
    holder_t   *slot;
    const void *holder = NULL;

    if ( (KBSP_RES_MAX <= res) || (NULL == owner) )
    {
        return false;
    }
    slot = &s_holders[res];

    /* One swap decides it; only the new holder writes the name. */
    if ( atomic_compare_exchange_strong(&slot->token, &holder, owner) )
    {
        atomic_store(&slot->name, name);
        return true;
    }
    /* Already held: a second claim by the same owner keeps the first name. */
    return holder == owner;
}

void kbsp_release(kbsp_res_t res, const void *owner)
{
    /* Only the host suite releases. The name goes first, while the slot is
     * still this owner's. */
    const void *holder = owner;

    if ( (KBSP_RES_MAX > res) && (atomic_load(&s_holders[res].token) == owner) )
    {
        atomic_store(&s_holders[res].name, NULL);
        (void)atomic_compare_exchange_strong(&s_holders[res].token, &holder, NULL);
    }
}

const char *kbsp_owner_name(kbsp_res_t res)
{
    const char *name;

    if ( KBSP_RES_MAX <= res )
    {
        return NULL;
    }
    name = atomic_load(&s_holders[res].name);
    if ( (NULL == name) && (NULL != atomic_load(&s_holders[res].token)) )
    {
        /* Held but not named yet. */
        name = "?";
    }
    return name;
}

const char *kbsp_res_name(kbsp_res_t res)
{
    /* Never NULL: the double-claim message prints it on the fatal path. */
    if ( (KBSP_RES_MAX <= res) || (NULL == RES_NAMES[res]) )
    {
        return "?";
    }
    return RES_NAMES[res];
}
