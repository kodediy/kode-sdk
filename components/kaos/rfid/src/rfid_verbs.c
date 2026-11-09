/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file rfid_verbs.c
 * @brief AO_Rfid's console verbs: `rfid`, `rfid_help`, `rfid_read`, `rfid_write` and `rfid_field`, registered by kaos_rfid_start().
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "kaos.h"
#include "kdrv_rfid_lf.h"
#include "kqp.h"

#include "console_verbs.h"
#include "kverbs.h"

/** @brief Which `rfid` verb holds the console. */
typedef enum {
    RFID_V_NONE = 0,  /**< None of them.                                   */
    RFID_V_INFO,      /**< rfid: closed by RFID_INFO_RSP_SIG.              */
    RFID_V_OP,        /**< rfid_read and rfid_write: closed by
                       *   RFID_OP_RSP_SIG when the field window ends.     */
    RFID_V_FIELD      /**< rfid_field: closed by RFID_FIELD_RSP_SIG when the
                       *   capture window ends.                            */
} rfid_verb_kind_t;

static rfid_verb_kind_t s_rfid_kind;

/*
 * The `rfid` status fields. The 125 kHz front end has no reader IC and so no
 * kdrv field_info(): `rfid` reports the AO's own state, from this table.
 */
typedef enum {
    RFID_FLD_FIELD = 0,
    RFID_FLD_LAST,
    RFID_FLD_COUNTS,
    RFID_FLD_MAX
} rfid_field_t;

/** @brief One `rfid_help` row: the key `rfid` prints it under, and what it means. */
typedef struct {
    const char *name;
    const char *desc;
} rfid_field_desc_t;

/* ---- rfid ----------------------------------------------------------------
 * `rfid` is the read-only snapshot, answered in every state. `rfid_read` and
 * `rfid_write` energise the field for a whole window, so they are async: the
 * handler posts the request and AO_Rfid answers a RfidOpRspEvt on
 * RFID_OP_RSP_SIG when the window closes. */

/** @brief The window rfid_read/rfid_write keep the field up for by default. */
#define RFID_VERB_WINDOW_MS_N  2000                              /**< Bare, for `help`. */
#define RFID_VERB_WINDOW_MS    ((uint32_t)RFID_VERB_WINDOW_MS_N)

/* The ID rfid_write hands AO_Rfid. Static: the request is handled after
 * h_rfid_write returns. One buffer, as the console runs one command at a time. */
#define RFID_V_ID_BYTES  5U  /**< An EM4100 ID: forty bits. */

static uint8_t s_rfid_id[RFID_V_ID_BYTES];

/**
 * @brief rfid: the LF front end's status. Answers in every state.
 *
 * The read-only snapshot: it energises nothing. Closed by RFID_INFO_RSP_SIG.
 */
static void h_rfid(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_rfid_kind = RFID_V_INFO;
    rfid_info_request(AO_Console);
}

/**
 * @brief rfid_read [ms=]: energise for a window and report the first EM4100 ID.
 *
 * A read never boosts VO4. Async: this goes in flight and
 * AO_Rfid answers with RFID_OP_RSP_SIG when the window closes.
 */
static void h_rfid_read(const kverb_t *c, const karg_val_t *v)
{
    uint32_t ms = RFID_VERB_WINDOW_MS;

    if ( v[0].present )
    {
        ms = (uint32_t)v[0].u;
    }
    console_begin(c->verb);
    s_rfid_kind = RFID_V_OP;
    rfid_read_console_request(AO_Console, ms);
}

/**
 * @brief rfid_write id=<10 hex>: clone that ID onto a T5577, then read it back.
 *
 * The id is five bytes, ten hex digits, colons between the pairs optional.
 * The clone raises VO4 for the write and verifies by read-back. Async:
 * AO_Rfid answers with RFID_OP_RSP_SIG.
 */
static void h_rfid_write(const kverb_t *c, const karg_val_t *v)
{
    /* Exactly ten hex digits, colons between the pairs optional. */
    if ( console_hex_bytes(v[0].s, s_rfid_id, RFID_V_ID_BYTES) != RFID_V_ID_BYTES )
    {
        console_reply_err(KCLI_ERR_VALUE, "id");
        return;
    }
    console_begin(c->verb);
    s_rfid_kind = RFID_V_OP;
    rfid_clone_console_request(AO_Console, s_rfid_id, RFID_VERB_WINDOW_MS);
}

/**
 * @brief rfid_field ms=: read the demod line with the carrier OFF, and
 *        describe what somebody else's 125 kHz field put on it.
 *
 * Async: AO_Rfid is asked with every pointer NULL (no waveform goes to the
 * console) and answers a RfidFieldRspEvt carrying the window by value.
 */
static void h_rfid_field(const kverb_t *c, const karg_val_t *v)
{
    console_begin(c->verb);
    s_rfid_kind = RFID_V_FIELD;
    rfid_capture_request(NULL, NULL, 0U, NULL, NULL, NULL, v[0].u, 0U, NULL,
                         AO_Console);
}

/**
 * @brief rfid_help: the field descriptions, same shape as `nfc_help`'s.
 */
static void h_rfid_help(const kverb_t *c, const karg_val_t *v)
{
    static const rfid_field_desc_t RFID_FIELD_DESC[] = {
        [RFID_FLD_FIELD] = { "field",
            "Whether the 125 kHz carrier is up right now - only ever true while "
            "a read or a clone is in flight; the field is down at rest." },
        [RFID_FLD_LAST] = { "last",
            "The last EM4100 ID decoded, its bit-rate divisor (16/32/64) and how "
            "long ago - state=none before any tag has been read." },
        [RFID_FLD_COUNTS] = { "counts",
            "Since boot: reads that decoded a tag, clones whose read-back "
            "matched, and windows that closed empty." },
    };

    _Static_assert((sizeof(RFID_FIELD_DESC) / sizeof(RFID_FIELD_DESC[0])) == (size_t)RFID_FLD_MAX,
                   "RFID_FIELD_DESC is missing an entry for an `rfid` status field");

    (void)c;
    (void)v;

    for ( uint8_t i = 0U; (uint8_t)RFID_FLD_MAX > i; ++i )
    {
        char qn[CONSOLE_QUOTED_MAX(CONSOLE_FIELD_NAME_MAX)];
        char qd[CONSOLE_QUOTED_MAX(CONSOLE_FIELD_DESC_MAX)];
        console_reply(">|field=%s desc=%s",
              console_quote(qn, sizeof(qn), RFID_FIELD_DESC[i].name),
              console_quote(qd, sizeof(qd), RFID_FIELD_DESC[i].desc));
    }
    console_reply(">ok");
}

/** @brief AO_Rfid's answers to the verb in flight. */
static bool rfid_answer(QEvt const *e)
{
    bool handled = true;

    switch ( e->sig )
    {
        case RFID_FIELD_RSP_SIG: {
            /* The same for `rfid_field`. */
            if ( RFID_V_FIELD == s_rfid_kind )
            {
                RfidFieldRspEvt const * const a = Q_EVT_CAST(RfidFieldRspEvt);

                if ( (uint32_t)KODE_OK == a->st )
                {
                    console_reply(">|rfid min=%" PRIu32 " mean=%" PRIu32 " peak=%" PRIu32 " ac=%" PRIu32 " n=%" PRIu32,
                          (uint32_t)a->min, (uint32_t)a->mean,
                          (uint32_t)a->peak, (uint32_t)a->ac,
                          (uint32_t)a->n);
                    console_reply(">ok");
                }
                else if ( (uint32_t)KODE_DENIED == a->st )
                {
                    console_reply_err(KCLI_ERR_BUSY, "busy");
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
        case RFID_INFO_RSP_SIG: {
            /* Posted to us: only ever our own `rfid` question's answer, gated
             * all the same against a late one. */
            if ( RFID_V_INFO == s_rfid_kind )
            {
                RfidInfoEvt const * const a = Q_EVT_CAST(RfidInfoEvt);
                uint32_t up = 0U;

                if ( a->field )
                {
                    up = 1U;
                }
                console_reply(">|rfid=field up=%" PRIu32, up);
                if ( a->have )
                {
                    console_reply(">|rfid=last state=seen id=\"%02" PRIX32 ":%02" PRIX32 ":%02" PRIX32 ":%02" PRIX32 ":%02" PRIX32 "\" "
                          "rf=%" PRIu32 " age_ms=%" PRIu32,
                          (uint32_t)a->id[0], (uint32_t)a->id[1],
                          (uint32_t)a->id[2], (uint32_t)a->id[3],
                          (uint32_t)a->id[4], (uint32_t)a->rf_div,
                          (uint32_t)a->age_ms);
                }
                else
                {
                    console_reply(">|rfid=last state=none");
                }
                console_reply(">|rfid=counts reads=%" PRIu32 " writes=%" PRIu32 " windows=%" PRIu32,
                      (uint32_t)a->reads, (uint32_t)a->writes,
                      (uint32_t)a->windows);
                console_reply(">ok");
                console_end();
            }
            else
            {
                handled = false;
            }
            break;
        }
        case RFID_OP_RSP_SIG: {
            /* Our own rfid_read / rfid_write answer, gated against a late one. */
            if ( RFID_V_OP == s_rfid_kind )
            {
                RfidOpRspEvt const * const a = Q_EVT_CAST(RfidOpRspEvt);

                if ( (uint32_t)KODE_DENIED == a->rc )
                {
                    console_reply(">err code=3 msg=\"rfid busy or not fitted\"");
                }
                else if ( !a->write )
                {
                    if ( (uint32_t)KODE_OK == a->rc )
                    {
                        console_reply(">|rfid id=\"%02" PRIX32 ":%02" PRIX32 ":%02" PRIX32 ":%02" PRIX32 ":%02" PRIX32 "\" rf=%" PRIu32,
                              (uint32_t)a->id[0], (uint32_t)a->id[1],
                              (uint32_t)a->id[2], (uint32_t)a->id[3],
                              (uint32_t)a->id[4], (uint32_t)a->rf_div);
                        console_reply(">ok");
                    }
                    else
                    {
                        console_reply(">|rfid=tag state=none");
                        console_reply(">err code=4 msg=\"no tag in the window\"");
                    }
                }
                else
                {
                    if ( (uint32_t)KODE_OK == a->rc )
                    {
                        console_reply(">|rfid=clone id=\"%02" PRIX32 ":%02" PRIX32 ":%02" PRIX32 ":%02" PRIX32 ":%02" PRIX32 "\" "
                              "match=yes",
                              (uint32_t)a->id[0], (uint32_t)a->id[1],
                              (uint32_t)a->id[2], (uint32_t)a->id[3],
                              (uint32_t)a->id[4]);
                        console_reply(">ok");
                    }
                    else
                    {
                        console_reply(">|rfid=clone match=no");
                        console_reply(">err code=4 msg=\"clone not verified\"");
                    }
                }
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

/* `rfid_read ms=`: the same bounds as `nfc_read`; a window too short to catch
 * a frame, or absurdly long, is refused before the field goes up. */
static const karg_spec_t A_RFID_READ[] = {
    { .key = "ms", .required = false, .type = KARG_UINT,
      .min = 100U, .max = 10000U,
      .def = CONSOLE_STRINGIFY(RFID_VERB_WINDOW_MS_N) },
};

/* `rfid_write id=`: the five-byte EM4100 ID, as ten hex digits (colons
 * optional). Required, and parsed by the handler: KARG_STRING checks presence only. */
static const karg_spec_t A_RFID_WRITE[] = {
    { .key = "id", .required = true, .type = KARG_STRING },
};

static const karg_spec_t A_RFID_FIELD[] = {
    { .key = "ms", .required = true, .type = KARG_UINT,
      .min = 1U, .max = KDRV_RFID_LF_CAPTURE_MS_MAX },
};

static const kverb_t RFID_VERBS[] = {
    { "rfid",       NULL,         0U,                       h_rfid       },
    { "rfid_help",  NULL,         0U,                       h_rfid_help  },
    { "rfid_read",  A_RFID_READ,  KVERB_KEYS(A_RFID_READ),  h_rfid_read  },
    { "rfid_write", A_RFID_WRITE, KVERB_KEYS(A_RFID_WRITE), h_rfid_write },
    { "rfid_field", A_RFID_FIELD, KVERB_KEYS(A_RFID_FIELD), h_rfid_field },
};

const kverbs_t rfid_verbs = {
    .verbs  = RFID_VERBS, .n = sizeof(RFID_VERBS) / sizeof(RFID_VERBS[0]),
    .answer = rfid_answer,
};
