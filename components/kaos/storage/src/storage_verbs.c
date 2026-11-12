/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file storage_verbs.c
 * @brief AO_Storage's console verbs: `sd_*`, `fs_*`, the card's field table and `file_desc`, registered by kaos_storage_start().
 */
#include <inttypes.h>
#include <string.h>

#include "kaos.h"
#include "kdrv_sd.h"
#include "kqp.h"

#include "console_verbs.h"
#include "kverbs.h"

/** @brief Which storage verb holds the console. */
typedef enum {
    STORAGE_V_NONE = 0,  /**< None of them, or a field verb.               */
    STORAGE_V_LS,        /**< sd_ls/fs_ls: STORAGE_ENTRY_SIG until the end. */
    STORAGE_V_INFO,      /**< sd_info/fs_info: closed by STORAGE_INFO_SIG.  */
    STORAGE_V_VERIFY,    /**< sd_verify: phase records until the
                          *   TEST_RESULT_SIG named `sd_verify`.            */
    STORAGE_V_DESC       /**< file_desc: closed by STORAGE_DESC_RSP_SIG.    */
} storage_verb_kind_t;

static storage_verb_kind_t s_storage_kind;

/** @brief STORAGE_V_INFO: the medium asked about; another asker's answer is not this one's. */
static uint8_t s_storage_medium;

/* ---- The card's field table -------------------------------------------------- */

/** @brief Resolve a card field name. */
static kode_status_t sd_field_by_name(const char *name, uint16_t *field)
{
    kdrv_sd_field_t f = KDRV_SD_FIELD_MAX;
    kode_status_t const rc = kdrv_sd_field_by_name(name, &f);

    *field = (uint16_t)f;
    return rc;
}

/** @brief Describe a card field. */
static bool sd_field_row(uint16_t field, console_field_t *out)
{
    /** @brief What each field of the card is, for `sd_help`. */
    static const char *const SD_FIELD_DESC[] = {
        [KDRV_SD_MFG_ID]          = "Manufacturer ID from the CID. 0x03 is SanDisk, 0x1B Samsung; the counterfeits use 0x05.",
        [KDRV_SD_OEM_ID]          = "OEM or application ID from the CID, two ASCII characters as one number.",
        [KDRV_SD_REVISION]        = "Product revision from the CID, BCD: 0x30 is 3.0.",
        [KDRV_SD_SERIAL_HI]       = "Top half of the card's 32-bit serial number. Split in two because a record's value is signed.",
        [KDRV_SD_SERIAL_LO]       = "Bottom half of the card's serial number. Consecutive serials across a batch are a counterfeit's signature.",
        [KDRV_SD_MFG_YEAR]        = "Year the card says it was made.",
        [KDRV_SD_MFG_MONTH]       = "Month the card says it was made, 1..12.",
        [KDRV_SD_CSD_VER]         = "CSD structure version: 0 is an SDSC card, 1 an SDHC or SDXC.",
        [KDRV_SD_CAPACITY_MB]     = "What the card says it holds. Exactly 30000 is the counterfeits' number.",
        [KDRV_SD_SECTORS]         = "The same capacity in sectors, which is where the card ends and what the production test writes to.",
        [KDRV_SD_SECTOR_SIZE]     = "Bytes in a sector. 512 on every SDHC card.",
        [KDRV_SD_READ_BLOCK_LEN]  = "Read block length the CSD declares, in bytes. The CSD holds it as an exponent; this is the shift already done.",
        [KDRV_SD_TR_SPEED_KHZ]    = "Transfer rate the CSD declares. What the bus actually runs at is freq_khz.",
        [KDRV_SD_CCC]             = "Card command classes, as a bitmap: which command groups the card implements.",
        [KDRV_SD_SD_SPEC]         = "Physical layer specification version from the SCR.",
        [KDRV_SD_BUS_WIDTHS]      = "Widths the card claims in its SCR: bit 0 is 1-bit, bit 2 is 4-bit. Claiming 4 and failing it is what the counterfeits do.",
        [KDRV_SD_ERASE_STATE]     = "What an erased block reads back as on this card.",
        [KDRV_SD_AU_KB]           = "Allocation unit, the granularity the card erases in.",
        [KDRV_SD_ERASE_SIZE_AU]   = "How many allocation units one erase covers.",
        [KDRV_SD_ERASE_TIMEOUT_S] = "How long the card asks for to erase one allocation unit.",
        [KDRV_SD_ERASE_OFFSET_S]  = "Constant the card adds to any erase timeout.",
        [KDRV_SD_DISCARD]         = "The card supports DISCARD, the cheap half of an erase.",
        [KDRV_SD_FULE]            = "The card supports Full User area Logical Erase.",
        [KDRV_SD_HIGH_CAPACITY]   = "SDHC or SDXC: addressed by block rather than by byte.",
        [KDRV_SD_BUS_WIDTH]       = "Data lines actually in use, 1 or 4. Four is what a healthy slot negotiates; one means three lines are not reaching it.",
        [KDRV_SD_FREQ_KHZ]        = "What the host clock ended up at, in kHz.",
        [KDRV_SD_CARD_MAX_KHZ]    = "The fastest clock the card said it would take.",
        [KDRV_SD_RCA]             = "Relative card address the card was given during identification.",
        [KDRV_SD_MMC]             = "The part is an MMC rather than an SD card.",
        [KDRV_SD_UHS1]            = "The card is UHS-I capable. This board cannot use it: UHS-I needs the bus switched to 1.8 V.",
        [KDRV_SD_MOUNTED]         = "Whether a card is mounted right now. Answered with or without one.",
        [KDRV_SD_HOSTED]          = "Whether the next mount reuses the SDMMC controller esp-hosted owns. AO_Radio sets this around the C5 link.",
        [KDRV_SD_TOTAL_MB]        = "How big the filesystem on the card is. Asked afresh every time.",
        [KDRV_SD_FREE_MB]         = "How much of that filesystem is unused.",
        [KDRV_SD_WIDTH_REQ]       = "Data lines the NEXT mount asks for: 1 or 4, nothing between. Does not touch the card, and lasts until the board resets.",
        [KDRV_SD_FREQ_REQ_KHZ]    = "Clock ceiling the NEXT mount asks for. 40000 is what this board is validated at; higher is a bench experiment.",
        [KDRV_SD_CHECK_REQ]       = "Whether the NEXT mount on four lines proves the card takes a write before keeping it: 1, the default, or 0 for none. Does not touch the card, and lasts until the board resets.",
    };
    _Static_assert((sizeof(SD_FIELD_DESC) / sizeof(SD_FIELD_DESC[0]))
                   == (size_t)KDRV_SD_FIELD_MAX,
                   "SD_FIELD_DESC is missing an entry for a card field");

    const kdrv_sd_field_info_t * const i = kdrv_sd_field_info((kdrv_sd_field_t)field);
    bool const ok = (NULL != i);

    if ( ok )
    {
        out->name = i->name; out->unit = i->unit;
        out->min = i->min;   out->max = i->max;
        out->writable = i->writable;
        out->desc = SD_FIELD_DESC[field];
    }
    return ok;
}

/** @brief Ask AO_Storage about a card field; it answers the console alone. */
static void sd_field_ask(uint8_t op, uint16_t field, int32_t value)
{
    storage_field_request(AO_Console, op, field, value);
}

static const console_chip_t SD_CHIP = {
    .chip = "sd", .count = (uint16_t)KDRV_SD_FIELD_MAX,
    .by_name = sd_field_by_name, .row = sd_field_row, .ask = sd_field_ask,
};

/** @brief sd: the card's field table. */
static void h_sd(const kverb_t *c, const karg_val_t *v)
{
    s_storage_kind = STORAGE_V_NONE;
    console_field_cmd(&SD_CHIP, c->verb, v);
}

/** @brief sd_help: the card's field table, in slices. */
static void h_sd_help(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    s_storage_kind = STORAGE_V_NONE;
    console_field_help(&SD_CHIP, c->verb);
}

/* ---- The media ------------------------------------------------------------
 * `sd_*` and `fs_*` differ only in which medium they name, so they share a
 * body. Every one of them is a request: AO_Storage owns the media, so a
 * listing arrives afterwards as STORAGE_ENTRY_SIG events rather than being
 * read from here on the wrong thread.
 */

/** @brief Fire one storage mutation and ack; the outcome arrives as logs. */
static void storage_standard(kdrv_medium_t medium, storage_op_t op)
{
    storage_request(medium, op);
    console_reply(">ok");
}

/** @brief Ask for a medium's info: closed by STORAGE_INFO_SIG. */
static void storage_info(const kverb_t *c, kdrv_medium_t medium)
{
    console_begin(c->verb);
    s_storage_kind   = STORAGE_V_INFO;
    s_storage_medium = (uint8_t)medium;
    storage_request(medium, STORAGE_OP_INFO);
}

/**
 * @brief Start a listing: records arrive as STORAGE_ENTRY_SIG, the
 *        terminator on STORAGE_LIST_END_SIG. Everything, not just images:
 *        a directory listing that hides files is lying about the directory.
 */
static void storage_ls(const kverb_t *c, kdrv_medium_t medium)
{
    console_begin(c->verb);
    s_storage_kind = STORAGE_V_LS;
    storage_list_one(medium, false, true);
}

/** @brief sd_mount: mount the card and ack; the outcome arrives as logs. */
static void h_sd_mount(const kverb_t *c, const karg_val_t *v)
{
    (void)c; (void)v;
    storage_standard(KDRV_MEDIUM_SD, STORAGE_OP_MOUNT);
}

/** @brief sd_unmount: unmount the card and ack; the outcome arrives as logs. */
static void h_sd_unmount(const kverb_t *c, const karg_val_t *v)
{
    (void)c; (void)v;
    storage_standard(KDRV_MEDIUM_SD, STORAGE_OP_UNMOUNT);
}

/** @brief sd_info: the card's info, closed by STORAGE_INFO_SIG. */
static void h_sd_info(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    storage_info(c, KDRV_MEDIUM_SD);
}

/** @brief sd_ls: everything on the card, closed by STORAGE_LIST_END_SIG. */
static void h_sd_ls(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    storage_ls(c, KDRV_MEDIUM_SD);
}

/** @brief fs_mount: mount the internal partition and ack; the outcome arrives as logs. */
static void h_fs_mount(const kverb_t *c, const karg_val_t *v)
{
    (void)c; (void)v;
    storage_standard(KDRV_MEDIUM_FS, STORAGE_OP_MOUNT);
}

/** @brief fs_unmount: unmount the internal partition and ack; the outcome arrives as logs. */
static void h_fs_unmount(const kverb_t *c, const karg_val_t *v)
{
    (void)c; (void)v;
    storage_standard(KDRV_MEDIUM_FS, STORAGE_OP_UNMOUNT);
}

/** @brief fs_info: the internal partition's info, closed by STORAGE_INFO_SIG. */
static void h_fs_info(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    storage_info(c, KDRV_MEDIUM_FS);
}

/** @brief fs_ls: everything on the internal partition, closed by STORAGE_LIST_END_SIG. */
static void h_fs_ls(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    storage_ls(c, KDRV_MEDIUM_FS);
}

/**
 * @brief sd_verify confirm=on: is the CARD what it says it is?
 *
 * Writes to the far end of the card to find a fake capacity, then formats it.
 * Everything on the card is lost, so `confirm=on` is required.
 */
static void h_sd_verify(const kverb_t *c, const karg_val_t *v)
{
    if ( !v[0].b )
    {
        console_reply_err(KCLI_ERR_VALUE, "confirm");
    }
    else
    {
        console_begin(c->verb);
        s_storage_kind = STORAGE_V_VERIFY;
        console_extend(KODE_SD_VERIFY_BUDGET_MS);
        storage_request(KDRV_MEDIUM_SD, STORAGE_OP_VERIFY);
    }
}

/**
 * @brief file_desc name=<file>: what one .bin on either medium says it is.
 *
 * Closed by STORAGE_DESC_RSP_SIG, posted straight back to the console.
 */
static void h_file_desc(const kverb_t *c, const karg_val_t *v)
{
    console_begin(c->verb);
    s_storage_kind = STORAGE_V_DESC;
    storage_desc_request(v[0].s, AO_Console);
}

/** @brief The medium's name, as the records print it. */
static const char *storage_medium_word(bool sd)
{
    const char *word = "fs";

    if ( sd )
    {
        word = "sd";
    }
    return word;
}

/** @brief A listing's entry and its end, for sd_ls and fs_ls. */
static bool storage_listing(QEvt const *e)
{
    bool handled = false;

    if ( (QSignal)STORAGE_ENTRY_SIG == e->sig )
    {
        StorageEntryEvt const * const en = Q_EVT_CAST(StorageEntryEvt);
        char qn[CONSOLE_QUOTED_MAX(sizeof(en->name))];

        console_reply(">|name=%s size=%" PRIu32 " medium=%s",
                      console_quote(qn, sizeof(qn), en->name),
                      (uint32_t)en->size, storage_medium_word(en->from_sd));
        handled = true;
    }
    else if ( (QSignal)STORAGE_LIST_END_SIG == e->sig )
    {
        console_reply(">ok");
        console_end();
        handled = true;
    }
    else
    {
        /* Not a listing's. */
    }
    return handled;
}

/** @brief A medium's info, when it is the medium asked about. */
static bool storage_info_answer(QEvt const *e)
{
    bool handled = false;

    if ( (QSignal)STORAGE_INFO_SIG == e->sig )
    {
        StorageInfoEvt const * const i = Q_EVT_CAST(StorageInfoEvt);

        /* Another asker's medium - the launcher asks about both - is not
         * this command's answer. */
        if ( i->medium == s_storage_medium )
        {
            const char * const med =
                storage_medium_word((uint8_t)KDRV_MEDIUM_SD == i->medium);

            if ( i->usage_known )
            {
                console_reply(">|medium=%s mounted=%s total_kb=%" PRIu32
                              " free_kb=%" PRIu32,
                              med, console_on_off(i->mounted),
                              (uint32_t)i->total_kb, (uint32_t)i->free_kb);
            }
            else
            {
                console_reply(">|medium=%s mounted=%s", med,
                              console_on_off(i->mounted));
            }
            console_reply(">ok");
            console_end();
            handled = true;
        }
    }
    return handled;
}

/** @brief `pass` or `fail`, as the verify's records spell it. */
static const char *storage_result(bool ok)
{
    const char *word = "fail";

    if ( ok )
    {
        word = "pass";
    }
    return word;
}

/** @brief sd_verify's phase records and its verdict. */
static bool storage_verify_answer(QEvt const *e)
{
    bool handled = false;

    if ( (QSignal)STORAGE_TEST_SIG == e->sig )
    {
        StorageTestEvt const * const t = Q_EVT_CAST(StorageTestEvt);
        static const char *const VPHASE[] = { "", "probe", "format" };

        console_reply(">|step=%" PRIu32 " of=%" PRIu32 " sd=%s result=%s"
                      " capacity_mb=%" PRIu32 " total_mb=%" PRIu32,
                      (uint32_t)t->phase, (uint32_t)t->total,
                      console_word(VPHASE, sizeof(VPHASE) / sizeof(VPHASE[0]), t->phase),
                      storage_result(t->ok),
                      (uint32_t)t->capacity_mb, (uint32_t)t->total_mb);
        handled = true;
    }
    else if ( (QSignal)TEST_RESULT_SIG == e->sig )
    {
        TestResultEvt const * const v = Q_EVT_CAST(TestResultEvt);

        if ( (TEST_ORIGIN_AUTOTEST == v->origin)
            && (strcmp(v->name, "sd_verify") == 0) )
        {
            console_reply(">|verify=sd result=%s",
                          storage_result(v->ok));
            console_reply(">ok");
            console_end();
            handled = true;
        }
    }
    else
    {
        /* Not the verify's. */
    }
    return handled;
}

/** @brief file_desc's answer: the manifest, or the status it failed with. */
static bool storage_desc_answer(QEvt const *e)
{
    bool handled = false;

    if ( (QSignal)STORAGE_DESC_RSP_SIG == e->sig )
    {
        StorageDescRspEvt const * const r = Q_EVT_CAST(StorageDescRspEvt);

        if ( (kode_status_t)r->status != KODE_OK )
        {
            console_reply(">err code=%" PRIu32, (uint32_t)r->status);
        }
        else
        {
            const char *category = "other";
            char qn[CONSOLE_QUOTED_MAX(sizeof(r->name))];
            char qp[CONSOLE_QUOTED_MAX(sizeof(r->project))];
            char qc[CONSOLE_QUOTED_MAX(sizeof(r->category))];
            char qd[CONSOLE_QUOTED_MAX(sizeof(r->display))];

            if ( '\0' != r->category[0] )
            {
                category = r->category;
            }
            console_reply(">|name=%s project=%s category=%s display=%s",
                          console_quote(qn, sizeof(qn), r->name),
                          console_kv_text(qp, sizeof(qp), r->project),
                          console_quote(qc, sizeof(qc), category),
                          console_kv_text(qd, sizeof(qd), r->display));
            console_reply(">ok");
        }
        console_end();
        handled = true;
    }
    return handled;
}

/** @brief AO_Storage's answers to the verb in flight. */
static bool storage_answer(QEvt const *e)
{
    bool handled = false;

    if ( (QSignal)STORAGE_FIELD_RSP_SIG == e->sig )
    {
        /* Posted to the console, like the expander's. */
        StorageFieldEvt const * const a = Q_EVT_CAST(StorageFieldEvt);

        console_field_answer(&SD_CHIP, a->op, a->field, a->rc, a->value);
        handled = true;
    }
    else if ( STORAGE_V_LS == s_storage_kind )
    {
        handled = storage_listing(e);
    }
    else if ( STORAGE_V_INFO == s_storage_kind )
    {
        handled = storage_info_answer(e);
    }
    else if ( STORAGE_V_VERIFY == s_storage_kind )
    {
        handled = storage_verify_answer(e);
    }
    else if ( STORAGE_V_DESC == s_storage_kind )
    {
        handled = storage_desc_answer(e);
    }
    else
    {
        /* A field verb's, or none. */
    }
    return handled;
}

static const karg_spec_t A_STORAGE_NAME[] = {
    { .key = "name", .required = true, .type = KARG_STRING },
};

static const kverb_t STORAGE_VERBS[] = {
    { "sd_mount",   NULL,                 0U,                         h_sd_mount   },
    { "sd_unmount", NULL,                 0U,                         h_sd_unmount },
    { "sd_ls",      NULL,                 0U,                         h_sd_ls      },
    { "sd_info",    NULL,                 0U,                         h_sd_info    },
    { "fs_mount",   NULL,                 0U,                         h_fs_mount   },
    { "fs_unmount", NULL,                 0U,                         h_fs_unmount },
    { "fs_ls",      NULL,                 0U,                         h_fs_ls      },
    { "fs_info",    NULL,                 0U,                         h_fs_info    },
    { "sd",         console_field_args,   CONSOLE_FIELD_ARGS_N,       h_sd         },
    { "sd_help",    NULL,                 0U,                         h_sd_help    },
    { "sd_verify",  console_confirm_args, CONSOLE_CONFIRM_ARGS_N,     h_sd_verify  },
    { "file_desc",  A_STORAGE_NAME,       KVERB_KEYS(A_STORAGE_NAME), h_file_desc  },
};

const kverbs_t storage_verbs = {
    .verbs  = STORAGE_VERBS, .n = sizeof(STORAGE_VERBS) / sizeof(STORAGE_VERBS[0]),
    .answer = storage_answer,
};
