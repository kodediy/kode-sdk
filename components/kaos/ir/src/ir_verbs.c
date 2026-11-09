/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file ir_verbs.c
 * @brief AO_Ir's console verbs: `ir`, `ir_help`, `ir_send` and `ir_rx`, registered by kaos_ir_start().
 */
#include <inttypes.h>
#include <stdio.h>

#include "kaos.h"
#include "kdrv_ir.h"
#include "kqp.h"

#include "console_verbs.h"
#include "kverbs.h"

/** @brief Which `ir` verb holds the console. */
typedef enum {
    IR_V_NONE = 0,  /**< None of them.                                     */
    IR_V_INFO,      /**< ir: closed by IR_INFO_RSP_SIG.                    */
    IR_V_RX         /**< ir_rx: records off IR_RX_SIG until the window's
                     *   deadline, its normal end.                         */
} ir_verb_kind_t;

static ir_verb_kind_t s_ir_kind;

/**
 * @brief One row of `ir`'s status output, named for `ir_help`.
 *
 * Keys this console prints, not driver fields, so the table lives here and
 * carries only a name and a meaning: `ir` has no field=/value= keys.
 */
typedef enum {
    IR_FLD_TX = 0,
    IR_FLD_RX,
    IR_FLD_CARRIER,
    IR_FLD_LEVEL,
    IR_FLD_LAST,
    IR_FLD_MAX
} ir_field_t;

/** @brief One `ir_help` row: the key `ir` prints it under, and what it means. */
typedef struct {
    const char *name;
    const char *desc;
} ir_field_desc_t;

/**
 * @brief ir: the block's status - both channels, the carrier, the
 *        receiver's resting level, and the last frame heard with its age.
 *
 * Closed by IR_INFO_RSP_SIG: AO_Ir owns the block. The carrier is a board
 * fact, printed from KDRV_IR_CARRIER_HZ.
 */
static void h_ir(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_ir_kind = IR_V_INFO;
    ir_info_request(AO_Console);
}

/**
 * @brief ir_help: the field descriptions as `>|field=<name> desc=<text>` lines,
 *        in one pass (five rows fit in one KCLI_SLICE).
 */
static void h_ir_help(const kverb_t *c, const karg_val_t *v)
{
    static const ir_field_desc_t IR_FIELD_DESC[] = {
        [IR_FLD_TX] = { "tx",
            "Whether the transmit channel came up at boot." },
        [IR_FLD_RX] = { "rx",
            "Whether the receive channel is up, and whether the echo "
            "suppression window is open (deaf=yes) - open for the length of "
            "this board's own last transmission plus its tail." },
        [IR_FLD_CARRIER] = { "carrier",
            "The carrier the transmitter drives, in Hz - fixed at the only "
            "frequency this board's own receiver can hear." },
        [IR_FLD_LEVEL] = { "level",
            "The receiver's resting level: high proves the line is not held "
            "low, not that a receiver is fitted - test component=ir is the "
            "reading that does." },
        [IR_FLD_LAST] = { "last",
            "The last frame the receiver produced, decoded or not, and how "
            "long ago - state=none before anything has been heard." },
    };

    _Static_assert((sizeof(IR_FIELD_DESC) / sizeof(IR_FIELD_DESC[0])) == (size_t)IR_FLD_MAX,
                   "IR_FIELD_DESC is missing an entry for an `ir` status field");

    (void)c; (void)v;
    for ( uint8_t i = 0U; (uint8_t)IR_FLD_MAX > i; ++i )
    {
        char qn[CONSOLE_QUOTED_MAX(CONSOLE_FIELD_NAME_MAX)];
        char qd[CONSOLE_QUOTED_MAX(CONSOLE_FIELD_DESC_MAX)];
        console_reply(">|field=%s desc=%s",
              console_quote(qn, sizeof(qn), IR_FIELD_DESC[i].name),
              console_quote(qd, sizeof(qd), IR_FIELD_DESC[i].desc));
    }
    console_reply(">ok");
}

/**
 * @brief ir_send proto=nec addr= cmd=: transmit one frame.
 *
 * Posts and returns: `>ok` means handed to AO_Ir, not on the wire; a dropped
 * event is logged at its own end.
 */
static void h_ir_send(const kverb_t *c, const karg_val_t *v)
{
    (void)c;
    ir_send_request((uint16_t)v[1].u, (uint16_t)v[2].u);
    console_reply(">ok");
}

/**
 * @brief ir_rx ms=: listen for a window and report each frame as it arrives.
 *
 * Each IR_RX_SIG becomes one record as it arrives; nothing is buffered. The
 * window running out is the ordinary end: the watchdog closes it with `>ok`,
 * not `>err code=6`.
 */
static void h_ir_rx(const kverb_t *c, const karg_val_t *v)
{
    uint32_t ms = ((uint32_t)CONSOLE_CMD_TIMEOUT_MS_N);

    if ( v[0].present )
    {
        ms = v[0].u;
    }
    console_begin(c->verb);
    s_ir_kind = IR_V_RX;
    console_extend(ms);
}

/** @brief AO_Ir's answers to the verb in flight. */
static bool ir_answer(QEvt const *e)
{
    bool handled = true;

    switch ( e->sig )
    {
        case IR_INFO_RSP_SIG: {
            /* Posted to us: only ever our own question's answer, gated all
             * the same against a late one. */
            if ( IR_V_INFO == s_ir_kind )
            {
                IrInfoEvt const * const a = Q_EVT_CAST(IrInfoEvt);
                const char *tx   = "down";
                const char *rx   = "down";
                const char *deaf = "no";

                if ( a->up )
                {
                    tx = "up";
                    rx = "up";
                }
                if ( a->deaf )
                {
                    deaf = "yes";
                }

                console_reply(">|ir=tx state=%s", tx);
                console_reply(">|ir=rx state=%s deaf=%s", rx, deaf);
                console_reply(">|ir=carrier hz=%" PRIu32, (uint32_t)KDRV_IR_CARRIER_HZ);
                if ( a->up )
                {
                    uint32_t high = 0U;
                    if ( a->rx_high )
                    {
                        high = 1U;
                    }
                    console_reply(">|ir=level high=%" PRIu32, high);
                }
                else
                {
                    /* Not asked of the driver at all when the block never
                     * came up - kdrv_ir_rx_idle() would only refuse it. */
                    console_reply(">|ir=level state=unknown");
                }
                if ( a->have_last )
                {
                    if ( a->nec )
                    {
                        console_reply(">|ir=last state=heard nec=1 addr=0x%04" PRIX32 " "
                              "cmd=0x%04" PRIX32 " symbols=%" PRIu32 " age_ms=%" PRIu32,
                              (uint32_t)a->addr, (uint32_t)a->cmd,
                              (uint32_t)a->symbols, (uint32_t)a->age_ms);
                    }
                    else
                    {
                        console_reply(">|ir=last state=heard nec=0 symbols=%" PRIu32 " "
                              "age_ms=%" PRIu32,
                              (uint32_t)a->symbols, (uint32_t)a->age_ms);
                    }
                }
                else
                {
                    console_reply(">|ir=last state=none");
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
        case IR_RX_SIG: {
            /* Published to the whole bus; only this verb cares, behind the
             * inflight gate. */
            if ( IR_V_RX == s_ir_kind )
            {
                IrRxEvt const * const a = Q_EVT_CAST(IrRxEvt);
                if ( a->nec )
                {
                    console_reply(">|ir=rx nec=1 addr=0x%04" PRIX32 " cmd=0x%04" PRIX32 " symbols=%" PRIu32,
                          (uint32_t)a->addr, (uint32_t)a->cmd,
                          (uint32_t)a->symbols);
                }
                else
                {
                    console_reply(">|ir=rx nec=0 symbols=%" PRIu32, (uint32_t)a->symbols);
                }
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

/** @brief The watchdog ran out: for `ir_rx`, the window's ordinary end. */
static bool ir_timeout(void)
{
    bool closed = false;

    if ( IR_V_RX == s_ir_kind )
    {
        /* Every frame it heard already left its own record the moment it
         * arrived, so `>ok` is the honest close. */
        console_reply(">ok");
        closed = true;
    }
    return closed;
}

/* `ir_send`: an enum, so a second protocol is a table edit. `addr` and `cmd`
 * are NEC's width; `cmd` is required, while address 0 is an ordinary one. */
static const char *const EN_IR_PROTO[] = { "nec" };
static const karg_spec_t A_IR_SEND[] = {
    { .key = "proto", .required = false, .type = KARG_ENUM,
      .enums = EN_IR_PROTO,
      .n_enums = (uint8_t)(sizeof(EN_IR_PROTO) / sizeof(EN_IR_PROTO[0])),
      .def = "nec" },
    { .key = "addr", .required = false, .type = KARG_UINT,
      .min = 0U, .max = 0xFFFFU, .def = "0" },
    { .key = "cmd", .required = true, .type = KARG_UINT,
      .min = 0U, .max = 0xFFFFU },
};
/* `ir_rx ms=`: 100 ms to 30 s. The default is CONSOLE_CMD_TIMEOUT_MS_N, the
 * same as any command's watchdog. */

static const karg_spec_t A_IR_RX[] = {
    { .key = "ms", .required = false, .type = KARG_UINT,
      .min = 100U, .max = 30000U,
      .def = CONSOLE_STRINGIFY(CONSOLE_CMD_TIMEOUT_MS_N) },
};

static const kverb_t IR_VERBS[] = {
    { "ir",      NULL,      0U,                    h_ir      },
    { "ir_help", NULL,      0U,                    h_ir_help },
    { "ir_send", A_IR_SEND, KVERB_KEYS(A_IR_SEND), h_ir_send },
    { "ir_rx",   A_IR_RX,   KVERB_KEYS(A_IR_RX),   h_ir_rx   },
};

const kverbs_t ir_verbs = {
    .verbs   = IR_VERBS, .n = sizeof(IR_VERBS) / sizeof(IR_VERBS[0]),
    .answer  = ir_answer,
    .timeout = ir_timeout,
};
