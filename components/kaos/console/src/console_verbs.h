/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file console_verbs.h
 * @brief What an owner's verbs ask of AO_Console: the replies, the command in flight and the field tables.
 */
#ifndef CONSOLE_VERBS_H_
#define CONSOLE_VERBS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console_grammar.h"
#include "kcli_proto.h"
#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Stringify after expansion, so a default a handler applies and the default
 * `help` prints for it are one token rather than two spellings of one value.
 * MISRA C rule 20.10 (advisory) discourages the # operator; the deviation is
 * deliberate and confined to these two lines, and it buys the only thing that
 * stops the printed default drifting away from the applied one. */
#define CONSOLE_STRINGIFY_(x_) #x_
#define CONSOLE_STRINGIFY(x_)  CONSOLE_STRINGIFY_(x_)

#define CONSOLE_CMD_TIMEOUT_MS_N  5000  /**< A command's watchdog, in ms; bare, so `help` can print it as a default. */
#define CONSOLE_FIELD_NAME_MAX    32U   /**< The longest field name a table carries. */
#define CONSOLE_FIELD_DESC_MAX    176U  /**< The longest field description a table carries. */

/** @brief What a verb needs of the port its line arrived on. */
typedef enum {
    CONSOLE_NEED_NOT_BLE,   /**< The cable, the channel or the scheduler. */
    CONSOLE_NEED_WIRE,      /**< The cable or BLE: a payload rides the wire. */
    CONSOLE_NEED_CABLE,     /**< The cable alone. */
    CONSOLE_NEED_NOT_SCHED  /**< Anything but the scheduler's own lines: an entry does not edit the table it runs from. */
} console_need_t;

/* ---- Replies ---- */

/** @brief One response line, tagged with the port of the command it answers. @param fmt printf format. */
void console_reply(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/**
 * @brief Close the command with `>err code=<code> msg=<msg>`.
 * @param code The error class.
 * @param msg  One word.
 */
void console_reply_err(kcli_err_t code, const char *msg);

/**
 * @brief `>ok` when the request reached its owner, else code 5 naming the verb.
 * @param verb      The verb.
 * @param delivered Whether the event went out.
 */
void console_reply_posted(const char *verb, bool delivered);

/**
 * @brief A text value for a record: quoted, or `-` when empty.
 * @param dst Room sized with CONSOLE_QUOTED_MAX().
 * @param cap Its size.
 * @param src The text; NULL or empty is `-`.
 * @return @p dst or `-`.
 */
const char *console_kv_text(char *dst, size_t cap, const char *src);

/**
 * @brief console_quote() into what is left of a line, with a `!W` when it cuts.
 * @param dst  Where to write.
 * @param cap  Its size.
 * @param src  The value.
 * @param what The record's key, for the warning.
 * @return @p dst.
 */
const char *console_kv_fit(char *dst, size_t cap, const char *src, const char *what);

/**
 * @brief One word for a kode_status_t, for a code 5 `msg=`.
 * @param st The status.
 * @return The word.
 */
const char *console_status_word(uint32_t st);

/**
 * @brief Whether @p name ends in `.bin`, any case: a file to flash, not an installed app.
 * @param name The name.
 * @return true for a file name.
 */
bool console_is_bin(const char *name);

/** @brief `on` or `off`, for a record. @param on The flag. @return The word. */
const char *console_on_off(bool on);

/**
 * @brief Word @p i of a table, or `?` past its end.
 * @param words The table.
 * @param n     Its length.
 * @param i     The index, usually an enum from an event.
 * @return The word.
 */
const char *console_word(const char *const *words, size_t n, uint32_t i);

#define CONSOLE_HEX_PER_BYTE    2U     /**< Hex digits a byte takes. */
#define CONSOLE_HEX_DIGITS      16U    /**< Values one hex digit holds. */
#define CONSOLE_NIBBLE_BITS     4U     /**< Bits one hex digit holds. */
/** @brief Room for @p n bytes as hex text, and its terminator. */
#define CONSOLE_HEX_TEXT(n)     (((size_t)(n) * CONSOLE_HEX_PER_BYTE) + 1U)

/**
 * @brief Hex text into bytes: pairs of digits, either case, `:` anywhere ignored.
 * @param s   The text; NULL is refused.
 * @param out At least @p max bytes.
 * @param max How many bytes @p out holds.
 * @return The byte count; 0 for an empty, odd, over-long or non-hex text,
 *         which leaves @p out untouched.
 */
uint8_t console_hex_bytes(const char *s, uint8_t *out, uint8_t max);

/**
 * @brief Bytes as upper-case hex text, two digits each, no separator.
 * @param dst   Room sized with CONSOLE_HEX_TEXT().
 * @param cap   Its size.
 * @param bytes The bytes.
 * @param n     How many.
 * @return @p dst.
 */
const char *console_hex_text(char *dst, size_t cap, const uint8_t *bytes, size_t n);

#define CONSOLE_IP4_TEXT_MAX    16U    /**< `255.255.255.255` and its terminator. */
#define CONSOLE_IP4_OCTET_BITS  8U     /**< Bits in one octet of an address. */
#define CONSOLE_IP4_OCTET_MASK  0xFFU  /**< One octet. */

/**
 * @brief An IPv4 address as a record prints it, first octet in the low byte.
 * @param dst At least CONSOLE_IP4_TEXT_MAX bytes.
 * @param cap Its size.
 * @param ip4 The address, as lwIP keeps it.
 * @return @p dst.
 */
const char *console_ip4(char *dst, size_t cap, uint32_t ip4);

/**
 * @brief Whether the line in hand may run from its port; answers the refusal when not.
 * @param need What the verb asks of its port.
 * @return true when it may run.
 */
bool console_src_allows(console_need_t need);

/* ---- The command in flight ---- */

/** @brief Hold the console for the verb in hand until console_end(), under the watchdog. @param verb The verb. */
void console_begin(const char *verb);

/** @brief Disarm the watchdog of the command in flight: its owner always answers. */
void console_unwatched(void);

/** @brief Give the command in flight a deadline of @p ms from now. @param ms The deadline. */
void console_extend(uint32_t ms);

/** @brief Close the command in flight: the console is free again. */
void console_end(void);

/**
 * @brief Pace the owner's listing: one CONSOLE_SLICE_SIG reaches its answer
 *        hook a slice gap from now, and the watchdog starts over, because it
 *        measures progress, not total time.
 */
void console_pace(void);

/** @brief The port of the command in flight. @return KODE_CONSOLE_SRC_*, or KODE_LOG_SRC_NONE between commands. */
uint8_t console_src(void);

/** @brief The verb of the command in flight. @return Its name, or `?`. */
const char *console_verb(void);

/**
 * @brief Whether @p verb is one of the console's own.
 * @param verb The word.
 * @return true when the system table carries it.
 */
bool console_system_verb(const char *verb);

/* ---- Field tables ---- */

/** @brief One field of a chip's table, as `<chip>_help` prints it. */
typedef struct {
    const char *name;      /**< As `field=` takes it. */
    const char *unit;      /**< Bare word. */
    int32_t     min;       /**< Lowest value. */
    int32_t     max;       /**< Highest value. */
    bool        writable;  /**< `value=` may set it. */
    const char *desc;      /**< What it is for. */
} console_field_t;

/** @brief A chip with a field table, and the owner that answers for it. */
typedef struct {
    const char *chip;   /**< The records' `chip=` word. */
    uint16_t    count;  /**< Fields in its table. */
    kode_status_t (*by_name)(const char *name, uint16_t *field);   /**< A name to its field. */
    bool (*row)(uint16_t field, console_field_t *out);             /**< A field described; false past the end. */
    void (*ask)(uint8_t op, uint16_t field, int32_t value);        /**< FIELD_OP_* to its owner. */
} console_chip_t;

#define CONSOLE_FIELD_ARGS_N    2U  /**< Keys in console_field_args. */
#define CONSOLE_STATE_ARGS_N    1U  /**< Keys in console_state_args. */
#define CONSOLE_CONFIRM_ARGS_N  1U  /**< Keys in console_confirm_args. */

/** @brief `field=` and `value=`, the keys every field verb takes. */
extern const karg_spec_t console_field_args[CONSOLE_FIELD_ARGS_N];

/** @brief `state=on|off`. */
extern const karg_spec_t console_state_args[CONSOLE_STATE_ARGS_N];

/** @brief `confirm=on|off`, for what is destructive. */
extern const karg_spec_t console_confirm_args[CONSOLE_CONFIRM_ARGS_N];

/**
 * @brief `<chip> [field=] [value=]`: resolve the name and ask the chip's owner.
 * @param chip The chip.
 * @param verb The verb.
 * @param v    Its two values.
 */
void console_field_cmd(const console_chip_t *chip, const char *verb, const karg_val_t *v);

/**
 * @brief `<chip>_help`: the chip's whole table, in slices.
 * @param chip The chip.
 * @param verb The verb.
 */
void console_field_help(const console_chip_t *chip, const char *verb);

/**
 * @brief The owner's answer to a field verb; ignored unless that chip's verb is in flight.
 * @param chip  The chip.
 * @param op    FIELD_OP_*.
 * @param field The field.
 * @param rc    The kode_status_t it answered.
 * @param value What the chip holds.
 */
void console_field_answer(const console_chip_t *chip, uint8_t op, uint16_t field,
                          uint8_t rc, int32_t value);

#ifdef __cplusplus
}
#endif

#endif /* CONSOLE_VERBS_H_ */
