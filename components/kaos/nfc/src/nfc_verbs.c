/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file nfc_verbs.c
 * @brief AO_Nfc's console verbs: `nfc`, `nfc_help`, `nfc_read`, `nfc_write`, `nfc_keys`, `nfc_dump`, `nfc_field` and `nfc_emulate*`, registered by kaos_nfc_start().
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "kaos.h"
#include "kdrv_st25r3916.h"
#include "knfc_tag.h"
#include "kode_log.h"
#include "kqp.h"

#include "console_verbs.h"
#include "kverbs.h"

/** @brief Which `nfc` verb holds the console. */
typedef enum {
    NFC_V_NONE = 0,  /**< None of them.                                    */
    NFC_V_INFO,      /**< nfc: closed by NFC_INFO_RSP_SIG.                 */
    NFC_V_NDEF,      /**< nfc_read and nfc_write: closed by NFC_NDEF_RSP_SIG,
                      *   whose own deadline is AO_Nfc's window.           */
    NFC_V_KEYS,      /**< nfc_keys: closed by NFC_KEYS_RSP_SIG, minutes
                      *   later - a dictionary sweep, not a watched one.   */
    NFC_V_DUMP,      /**< nfc_dump: closed by NFC_DUMP_RSP_SIG.            */
    NFC_V_FIELD,     /**< nfc_field: closed by NFC_FIELD_RSP_SIG when the
                      *   listen window ends.                              */
    NFC_V_EMU        /**< nfc_emulate and nfc_emulate_off: closed by
                      *   NFC_EMU_RSP_SIG.                                 */
} nfc_verb_kind_t;

static nfc_verb_kind_t s_nfc_kind;

#define NFC_V_READ_MS_DEF_N  1500  /**< KODE_NFC_TEST_FIELD_MS, bare for `help`. */
_Static_assert((uint32_t)NFC_V_READ_MS_DEF_N == KODE_NFC_TEST_FIELD_MS,
               "nfc_read's default window is the row's");
#define NFC_V_UID_SINGLE     4U    /**< A single-size UID, in bytes. */
#define NFC_V_UID_DOUBLE     7U    /**< A double-size UID. */
#define NFC_V_UID_TRIPLE     10U   /**< A triple-size UID. */
#define NFC_V_ATQA_BYTES     2U    /**< ATQA, most-significant byte first. */
#define NFC_V_SAK_BYTES      1U    /**< SAK. */
#define NFC_V_BYTE_BITS      8U    /**< Bits in a byte. */
#define NFC_V_KEY_B          1U    /**< NfcKeysRspEvt.type for key B. */
#define NFC_V_IC_TYPE_SHIFT  3U    /**< The identity register: type in bits 7..3. */
#define NFC_V_IC_TYPE_MASK   0x1FU /**< Its five type bits. */
#define NFC_V_IC_REV_MASK    0x07U /**< Its three revision bits. */

/* What `nfc_read` and `nfc_write` lend AO_Nfc. Static, not on a handler's
 * stack: AO_Nfc holds it for its whole window, long after the handler returns.
 * One buffer, because console_begin() runs one command at a time. */
static char s_nfc_text[KQP_NFC_TEXT_MAX];

/**
 * @brief nfc: the reader's own status. Answers in every state.
 *
 * The read-only snapshot, like `c5`: it energises nothing. The verbs that put
 * the field up are named apart.
 */
static void h_nfc(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_nfc_kind = NFC_V_INFO;
    nfc_info_request(AO_Console);
}

/**
 * @brief nfc_read [ms=]: energise for a window and read what turns up.
 */
static void h_nfc_read(const kverb_t *c, const karg_val_t *v)
{
    uint32_t ms = KODE_NFC_TEST_FIELD_MS;

    if ( v[0].present )
    {
        ms = (uint32_t)v[0].u;
    }
    console_begin(c->verb);
    s_nfc_kind = NFC_V_NDEF;
    s_nfc_text[0] = '\0';
    nfc_ndef_request(AO_Console, s_nfc_text, sizeof(s_nfc_text), false, false,
                     ms);
}

/**
 * @brief nfc_write url=<u> | text=<t>: write one record to the next tag.
 *
 * Exactly one of the two: the record type decides whether a phone opens a link
 * or shows a string, and the operator has said which. `text` is a KARG_TEXT:
 * `text="hello there"` is one record, and `text=""` a deliberate empty one.
 */
static void h_nfc_write(const kverb_t *c, const karg_val_t *v)
{
    bool has_url  = v[0].present;
    bool has_text = v[1].present;

    if ( has_url == has_text )
    {
        console_reply(">err code=2 msg=\"say url= or text=, and only one\"");
        return;
    }

    const char *what = v[1].s;
    if ( has_url )
    {
        what = v[0].s;
    }
    (void)snprintf(s_nfc_text, sizeof(s_nfc_text), "%s", what);

    console_begin(c->verb);
    s_nfc_kind = NFC_V_NDEF;
    nfc_ndef_request(AO_Console, s_nfc_text, sizeof(s_nfc_text), true,
                     has_url, KODE_NFC_TEST_FIELD_MS);
}

/**
 * @brief nfc_keys [block=]: sweep the public Mifare dictionary on one block.
 *
 * Tries every key kodeOS carries and reports the one that opens the block, or
 * that none do. Minutes long, with progress in the log.
 */
static void h_nfc_keys(const kverb_t *c, const karg_val_t *v)
{
    uint8_t block = 0U;

    if ( v[0].present )
    {
        block = (uint8_t)v[0].u;
    }
    console_begin(c->verb);
    s_nfc_kind = NFC_V_KEYS;
    /* Thousands of authentications, each a frame timeout when the key is
     * wrong: minutes, so the watchdog is stretched to cover the worst case. */
    console_extend(KODE_NFC_KEYS_BUDGET_MS);
    nfc_keys_request(AO_Console, block);
}

/**
 * @brief nfc_dump: read every sector a Mifare Classic opens, stream it.
 *
 * The full-card read. Each sector's key and blocks go to the log as they are
 * read; the command closes when the last sector is done. Sectors on keys
 * kodeOS does not carry are reported locked and skipped.
 */
static void h_nfc_dump(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_nfc_kind = NFC_V_DUMP;
    /* A dump is bounded but not by five seconds: sixteen sectors, each up to
     * a fast-dictionary auth plus its block reads. */
    console_extend(KODE_NFC_KEYS_BUDGET_MS);
    nfc_dump_request(AO_Console);
}

/**
 * @brief nfc_field ms=: listen with the reader's own field off, and describe
 *        what reached the antenna.
 *
 * Async: AO_Nfc answers an NfcFieldRspEvt carrying the window by value. The
 * window is at most KODE_NFC_FIELD_MS_MAX, inside the console's watchdog. A
 * busy reader answers code 4 `busy`; no reader, code 5 `absent`; any other
 * failure, code 5 with the status's own word.
 */
static void h_nfc_field(const kverb_t *c, const karg_val_t *v)
{
    console_begin(c->verb);
    s_nfc_kind = NFC_V_FIELD;
    nfc_field_request(NULL, NULL, NULL, v[0].u, 0U, NULL, AO_Console);
}

/* The card lent to AO_Nfc to present. Static so it outlives the post: the
 * driver copies it during the AO step that answers `nfc_emulate`. */
static knfc_emu_card_t s_nfc_emu;

/**
 * @brief nfc_emulate [uid=] [atqa=] [sak=] [uri= | text=]: present a card.
 *
 * A uid/atqa/sak is cloned verbatim; a uri or text presents a Type-2 tag a
 * phone reads. At least one of uid, uri or text. Held until `nfc_emulate_off`.
 * Async: closed by NFC_EMU_RSP_SIG.
 */
static void h_nfc_emulate(const kverb_t *c, const karg_val_t *v)
{
    (void)memset(&s_nfc_emu, 0, sizeof(s_nfc_emu));

    if ( v[0].present )
    {
        uint8_t n = console_hex_bytes(v[0].s, s_nfc_emu.uid, KNFC_UID_MAX);
        if ( (NFC_V_UID_SINGLE != n) && (NFC_V_UID_DOUBLE != n)
            && (NFC_V_UID_TRIPLE != n) )
        {
            console_reply(">err code=2 msg=\"uid= must be 4, 7 or 10 hex bytes\"");
            return;
        }
        s_nfc_emu.uid_len = n;
    }
    if ( v[1].present )
    {
        uint8_t a[NFC_V_ATQA_BYTES];
        if ( console_hex_bytes(v[1].s, a, NFC_V_ATQA_BYTES) != NFC_V_ATQA_BYTES )
        {
            console_reply(">err code=2 msg=\"atqa= must be two hex bytes\"");
            return;
        }
        s_nfc_emu.atqa = (uint16_t)(((uint16_t)a[0] << NFC_V_BYTE_BITS) | (uint16_t)a[1]);
    }
    if ( v[2].present )
    {
        uint8_t sk;
        if ( console_hex_bytes(v[2].s, &sk, NFC_V_SAK_BYTES) != NFC_V_SAK_BYTES )
        {
            console_reply(">err code=2 msg=\"sak= must be one hex byte\"");
            return;
        }
        s_nfc_emu.sak = sk;
    }

    bool has_uri  = v[3].present;
    bool has_text = v[4].present;
    if ( has_uri && has_text )
    {
        console_reply(">err code=2 msg=\"say uri= or text=, not both\"");
        return;
    }
    if ( has_uri )
    {
        s_nfc_emu.uri = true;
        (void)snprintf(s_nfc_emu.ndef, sizeof(s_nfc_emu.ndef), "%s", v[3].s);
    }
    else if ( has_text )
    {
        (void)snprintf(s_nfc_emu.ndef, sizeof(s_nfc_emu.ndef), "%s", v[4].s);
    }
    else
    {
        s_nfc_emu.ndef[0] = '\0';
    }

    if ( !v[0].present && !has_uri && !has_text )
    {
        console_reply(">err code=2 msg=\"give uid=, uri= or text=\"");
        return;
    }

    console_begin(c->verb);
    s_nfc_kind = NFC_V_EMU;
    nfc_emulate_request(&s_nfc_emu, NULL, NULL, 0U, NULL, AO_Console);
}

/**
 * @brief nfc_emulate_off: stop presenting the emulated card.
 */
static void h_nfc_emulate_off(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_nfc_kind = NFC_V_EMU;
    nfc_emulate_stop_request(NULL, NULL, NULL, 0U, NULL, AO_Console);
}

/**
 * @brief nfc_help: the field descriptions, same shape as `c5_help`'s.
 */
static void h_nfc_help(const kverb_t *c, const karg_val_t *v)
{
    (void)c;
    (void)v;

    for ( uint32_t f = 0U; (uint32_t)KDRV_ST25_FIELD_MAX > f; ++f )
    {
        const kdrv_st25r3916_field_info_t *i =
            kdrv_st25r3916_field_info((kdrv_st25r3916_field_t)f);
        if ( NULL == i )
        {
            continue;
        }
        char qn[CONSOLE_QUOTED_MAX(CONSOLE_FIELD_NAME_MAX)];
        console_reply(">|field=%s unit=%s min=%" PRId32 " max=%" PRId32 " step=%" PRIu32 " rw=%s",
              console_quote(qn, sizeof(qn), i->name), i->unit, (int32_t)i->min,
              (int32_t)i->max, (uint32_t)i->step, "r");
    }
    console_reply(">ok");
}

/** @brief AO_Nfc's answers to the verb in flight. */
static bool nfc_answer(QEvt const *e)
{
    bool handled = true;

    switch ( e->sig )
    {
        case NFC_INFO_RSP_SIG: {
            if ( NFC_V_INFO == s_nfc_kind )
            {
                NfcInfoEvt const * const a = Q_EVT_CAST(NfcInfoEvt);

                if ( !a->present )
                {
                    /* A board whose Antenna PCB is not fitted. Said plainly,
                     * because it is a normal state and not a fault. */
                    console_reply(">|nfc=chip state=absent");
                    console_reply(">ok");
                    console_end();
                    break;
                }

                uint32_t field = 0U;
                uint32_t busy  = 0U;
                if ( a->field )
                {
                    field = 1U;
                }
                if ( a->busy )
                {
                    busy = 1U;
                }
                console_reply(">|nfc=chip state=present identity=0x%02" PRIX32 " ic_type=%" PRIu32 " "
                      "ic_rev=%" PRIu32, (uint32_t)a->identity,
                      (uint32_t)((a->identity >> NFC_V_IC_TYPE_SHIFT) & NFC_V_IC_TYPE_MASK),
                      (uint32_t)(a->identity & NFC_V_IC_REV_MASK));
                console_reply(">|nfc=field up=%" PRIu32 " busy=%" PRIu32, field, busy);
                uint32_t emu = 0U;
                if ( a->emulating )
                {
                    emu = 1U;
                }
                console_reply(">|nfc=emulate on=%" PRIu32 " reads=%" PRIu32, emu,
                      (uint32_t)a->emu_reads);
                console_reply(">|nfc=counts tags=%" PRIu32 " windows=%" PRIu32 " errors=%" PRIu32,
                      (uint32_t)a->tags, (uint32_t)a->windows,
                      (uint32_t)a->errors);
                if ( a->have_last )
                {
                    char uid[CONSOLE_HEX_TEXT(KQP_NFC_UID_MAX)];

                    (void)console_hex_text(uid, sizeof(uid), a->uid, a->uid_len);
                    console_reply(">|nfc=last state=seen uid=%s atqa=0x%04" PRIX32 " sak=0x%02" PRIX32 " "
                          "type=%" PRIu32 " model=\"%s\" age_ms=%" PRIu32, uid,
                          (uint32_t)a->atqa, (uint32_t)a->sak,
                          (uint32_t)a->type,
                          knfc_tag_model(a->atqa, a->sak),
                          (uint32_t)a->age_ms);
                }
                else
                {
                    console_reply(">|nfc=last state=none");
                }
                console_reply(">ok");
                console_end();
            }
            else
            {
                handled = false;
            }
            break;
        }
        case NFC_FIELD_RSP_SIG: {
            /* Posted to us: only ever our own `nfc_field` window's answer,
             * gated all the same against a late one. */
            if ( NFC_V_FIELD == s_nfc_kind )
            {
                NfcFieldRspEvt const * const a = Q_EVT_CAST(NfcFieldRspEvt);

                if ( (uint32_t)KODE_OK == a->st )
                {
                    uint32_t ext = 0U;

                    if ( a->ext )
                    {
                        ext = 1U;
                    }
                    console_reply(">|nfc min=%" PRIu32 " mean=%" PRIu32 " peak=%" PRIu32 " mod=%" PRIu32 " ext=%" PRIu32 " n=%" PRIu32,
                          (uint32_t)a->min, (uint32_t)a->mean,
                          (uint32_t)a->peak, (uint32_t)a->mod, ext,
                          (uint32_t)a->n);
                    console_reply(">ok");
                }
                else if ( (uint32_t)KODE_DENIED == a->st )
                {
                    console_reply_err(KCLI_ERR_BUSY, "busy");
                }
                else if ( (uint32_t)KODE_IO_ERROR == a->st )
                {
                    /* NfcFieldRspEvt.st's meaning of it, not the medium's
                     * `iofail`: no reader is fitted, and asking again will
                     * not change that. */
                    console_reply_err(KCLI_ERR_REJECT, "absent");
                }
                else
                {
                    console_reply_err(KCLI_ERR_REJECT, console_status_word(a->st));
                }
                console_end();
            }
            else
            {
                handled = false;
            }
            break;
        }
        case NFC_EMU_RSP_SIG: {
            /* Posted to us: the answer to our own `nfc_emulate` or
             * `nfc_emulate_off`, gated against a late one. */
            if ( NFC_V_EMU == s_nfc_kind )
            {
                NfcEmuRspEvt const * const a = Q_EVT_CAST(NfcEmuRspEvt);

                if ( (uint32_t)KODE_OK == a->rc )
                {
                    console_reply(">ok reads=%" PRIu32, (uint32_t)a->reads);
                }
                else if ( (uint32_t)KODE_DENIED == a->rc )
                {
                    console_reply_err(KCLI_ERR_BUSY, "busy");
                }
                else
                {
                    console_reply_err(KCLI_ERR_REJECT, console_status_word(a->rc));
                }
                console_end();
            }
            else
            {
                handled = false;
            }
            break;
        }

        case NFC_NDEF_RSP_SIG: {
            if ( NFC_V_NDEF == s_nfc_kind )
            {
                NfcNdefRspEvt const * const a = Q_EVT_CAST(NfcNdefRspEvt);

                if ( !a->have_tag )
                {
                    console_reply(">|nfc=tag state=none");
                    console_reply(">err code=4 msg=\"no tag in the window\"");
                    console_end();
                    break;
                }

                char uid[CONSOLE_HEX_TEXT(KQP_NFC_UID_MAX)];

                (void)console_hex_text(uid, sizeof(uid), a->uid, a->uid_len);
                console_reply(">|nfc=tag uid=%s atqa=0x%04" PRIX32 " sak=0x%02" PRIX32 " type=%" PRIu32 " "
                      "model=\"%s\"", uid, (uint32_t)a->atqa,
                      (uint32_t)a->sak, (uint32_t)a->type,
                      knfc_tag_model(a->atqa, a->sak));

                if ( (uint32_t)KODE_OK != a->rc )
                {
                    /* DENIED: no key opened it (nfc_keys sweeps the full list).
                     * ERROR: it opened, but holds no NDEF message.
                     * TIMEOUT: it stopped answering mid-exchange. */
                    const char *why = "the exchange failed";
                    if ( (uint32_t)KODE_DENIED == a->rc )
                    {
                        why = "no key kodeOS knows will open it - "
                              "nfc_keys sweeps the full list";
                    }
                    else if ( (uint32_t)KODE_ERROR == a->rc )
                    {
                        why = "opened, but carries no NDEF message - "
                              "write one with nfc_write first";
                    }
                    else if ( (uint32_t)KODE_TIMEOUT == a->rc )
                    {
                        why = "the card stopped answering - "
                              "marginal link or it was moved";
                    }
                    console_reply(">err code=%" PRIu32 " msg=\"%s\"", (uint32_t)a->rc,
                          why);
                    console_end();
                    break;
                }
                if ( 0U < a->len )
                {
                    /* What is left of the line; a tag's text is whatever
                     * somebody wrote on it, so it may escape its way past
                     * that, and console_kv_fit() says so when it does. */
                    char q[KODE_LOG_LINE_MAX
                           - sizeof(">|nfc=ndef len=65535 text=")];
                    console_reply(">|nfc=ndef len=%" PRIu32 " text=%s", (uint32_t)a->len,
                          console_kv_fit(q, sizeof(q), a->text, "text"));
                }
                console_reply(">ok");
                console_end();
            }
            else
            {
                handled = false;
            }
            break;
        }
        case NFC_KEYS_RSP_SIG: {
            if ( NFC_V_KEYS == s_nfc_kind )
            {
                NfcKeysRspEvt const * const a = Q_EVT_CAST(NfcKeysRspEvt);

                if ( !a->have_tag )
                {
                    console_reply(">|nfc=keys state=no_tag block=%" PRIu32,
                          (uint32_t)a->block);
                    console_reply(">err code=4 msg=\"no card in the window\"");
                    console_end();
                    break;
                }
                if ( a->found )
                {
                    char kt = 'A';
                    char key[CONSOLE_HEX_TEXT(sizeof(a->key))];
                    if ( NFC_V_KEY_B == a->type )
                    {
                        kt = 'B';
                    }
                    console_reply(">|nfc=keys block=%" PRIu32 " result=found "
                          "key=%s type=%c tried=%" PRIu32,
                          (uint32_t)a->block,
                          console_hex_text(key, sizeof(key), a->key, sizeof(a->key)),
                          kt, (uint32_t)a->tried);
                    console_reply(">ok");
                }
                else
                {
                    console_reply(">|nfc=keys block=%" PRIu32 " result=none tried=%" PRIu32,
                          (uint32_t)a->block, (uint32_t)a->tried);
                    console_reply(">|nfc=keys msg=\"this card's key is on no public "
                          "list kodeOS carries\"");
                    console_reply(">ok");
                }
                console_end();
            }
            else
            {
                handled = false;
            }
            break;
        }
        case NFC_DUMP_RSP_SIG: {
            if ( NFC_V_DUMP == s_nfc_kind )
            {
                NfcDumpRspEvt const * const a = Q_EVT_CAST(NfcDumpRspEvt);

                if ( !a->have_tag )
                {
                    console_reply(">|nfc=dump state=no_tag");
                    console_reply(">err code=4 msg=\"no card in the window\"");
                    console_end();
                    break;
                }
                console_reply(">|nfc=dump sectors=%" PRIu32 "/%" PRIu32 " blocks=%" PRIu32,
                      (uint32_t)a->sectors_read, (uint32_t)a->sectors_total,
                      (uint32_t)a->blocks_read);
                console_reply(">ok");
                console_end();
            }
            else
            {
                handled = false;
            }
            break;
        }
        default: {
            handled = false;
            break;
        }
    }
    return handled;
}

static const karg_spec_t A_NFC_READ[] = {
    { .key = "ms", .required = false, .type = KARG_UINT,
      .min = 100U, .max = 10000U,
      .def = CONSOLE_STRINGIFY(NFC_V_READ_MS_DEF_N) },
};

static const karg_spec_t A_NFC_WRITE[] = {
    { .key = "url",  .required = false, .type = KARG_STRING },
    /* KARG_TEXT rather than KARG_STRING: an empty text record is one an
     * operator can mean, and a KARG_STRING would refuse it. */
    { .key = "text", .required = false, .type = KARG_TEXT   },
};

static const karg_spec_t A_NFC_KEYS[] = {
    { .key = "block", .required = false, .type = KARG_UINT,
      .min = 0U, .max = 255U, .def = "0" },
};

/* `nfc_field ms=`: required, and bounded by the owner's own ceiling, so a
 * window it would silently shorten, or a zero, is refused here. */
static const karg_spec_t A_NFC_FIELD[] = {
    { .key = "ms", .required = true, .type = KARG_UINT,
      .min = 1U, .max = KODE_NFC_FIELD_MS_MAX },
};

/* `nfc_emulate`: all optional; the handler wants one of uid/uri/text. uid,
 * atqa and sak are hex strings the handler parses (a UID is bytes). `text` is
 * a KARG_TEXT: it may hold spaces, and `text=""` is a value, not a missing key. */
static const karg_spec_t A_NFC_EMULATE[] = {
    { .key = "uid",  .required = false, .type = KARG_STRING },
    { .key = "atqa", .required = false, .type = KARG_STRING },
    { .key = "sak",  .required = false, .type = KARG_STRING },
    { .key = "uri",  .required = false, .type = KARG_STRING },
    { .key = "text", .required = false, .type = KARG_TEXT   },
};

static const kverb_t NFC_VERBS[] = {
    { "nfc",             NULL,          0U,                        h_nfc             },
    { "nfc_help",        NULL,          0U,                        h_nfc_help        },
    { "nfc_read",        A_NFC_READ,    KVERB_KEYS(A_NFC_READ),    h_nfc_read        },
    { "nfc_write",       A_NFC_WRITE,   KVERB_KEYS(A_NFC_WRITE),   h_nfc_write       },
    { "nfc_keys",        A_NFC_KEYS,    KVERB_KEYS(A_NFC_KEYS),    h_nfc_keys        },
    { "nfc_dump",        NULL,          0U,                        h_nfc_dump        },
    { "nfc_field",       A_NFC_FIELD,   KVERB_KEYS(A_NFC_FIELD),   h_nfc_field       },
    { "nfc_emulate",     A_NFC_EMULATE, KVERB_KEYS(A_NFC_EMULATE), h_nfc_emulate     },
    { "nfc_emulate_off", NULL,          0U,                        h_nfc_emulate_off },
};

const kverbs_t nfc_verbs = {
    .verbs  = NFC_VERBS, .n = sizeof(NFC_VERBS) / sizeof(NFC_VERBS[0]),
    .answer = nfc_answer,
};
