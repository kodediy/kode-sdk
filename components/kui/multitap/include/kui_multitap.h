/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_multitap.h
 * @brief Multitap keypad text rules: key characters per mode, pending character, cap, plain view.
 */
#ifndef KUI_MULTITAP_H
#define KUI_MULTITAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KUI_MULTITAP_MS       800U  /**< A press of the same key less than this many milliseconds after the last press cycles the pending character. */
#define KUI_MULTITAP_SHOW_MS  1000U /**< The last character shows plain this many milliseconds after the press that typed or cycled it. */
#define KUI_MULTITAP_TEXT_MAX 63U   /**< The longest text, and the longest Wi-Fi passphrase (802.11). */
#define KUI_MULTITAP_SSID_MAX 32U   /**< The longest hidden network name (802.11). */

#define KUI_MULTITAP_CHAR_KEYS 10U /**< The keys that type characters: 0 to 9. */
#define KUI_MULTITAP_KEYS      12U /**< Every key of the keypad: the character keys, the mode key and Delete. */
#define KUI_MULTITAP_MODES     4U  /**< How many modes there are. */

/** @brief The keys. The character keys have the value of their digit. */
typedef enum {
    KUI_MULTITAP_KEY_0 = 0, /**< The key 0. */
    KUI_MULTITAP_KEY_1,     /**< The key 1. */
    KUI_MULTITAP_KEY_2,     /**< The key 2. */
    KUI_MULTITAP_KEY_3,     /**< The key 3. */
    KUI_MULTITAP_KEY_4,     /**< The key 4. */
    KUI_MULTITAP_KEY_5,     /**< The key 5. */
    KUI_MULTITAP_KEY_6,     /**< The key 6. */
    KUI_MULTITAP_KEY_7,     /**< The key 7. */
    KUI_MULTITAP_KEY_8,     /**< The key 8. */
    KUI_MULTITAP_KEY_9,     /**< The key 9. */
    KUI_MULTITAP_KEY_MODE,  /**< The mode key: steps the mode. */
    KUI_MULTITAP_KEY_DELETE /**< Delete: removes the pending or the last character. */
} kui_multitap_key_t;

/** @brief The modes, in the order the mode key steps them. */
typedef enum {
    KUI_MULTITAP_MODE_LOWER = 0, /**< "abc": a letter key cycles its lower-case letters, then its digit. */
    KUI_MULTITAP_MODE_UPPER,     /**< "ABC": the same in capitals. */
    KUI_MULTITAP_MODE_DIGITS,    /**< "123": a key types its digit and nothing else. */
    KUI_MULTITAP_MODE_SYMBOLS    /**< Symbols: a key cycles four symbols, then its digit; the key 0 its space; the key 9 only its digit. */
} kui_multitap_mode_t;

/** @brief What one press did. */
typedef enum {
    KUI_MULTITAP_TYPED = 0, /**< A character was added, pending or committed at once. */
    KUI_MULTITAP_CYCLED,    /**< The pending character was replaced by the key's next one. */
    KUI_MULTITAP_DELETED,   /**< A character was removed. */
    KUI_MULTITAP_STEPPED,   /**< The mode key committed the pending character and stepped the mode. */
    KUI_MULTITAP_FULL,      /**< The press would add a character and the text is at its cap: nothing was added. */
    KUI_MULTITAP_EMPTY,     /**< Delete found nothing to delete. */
    KUI_MULTITAP_IGNORED    /**< Nothing was done: the multitap is NULL or the key is outside the keypad. */
} kui_multitap_result_t;

/** @brief The state of one text being typed. Read it through the getters, never write it. */
typedef struct {
    char text[KUI_MULTITAP_TEXT_MAX + 1U]; /**< The text, always NUL-terminated, every byte past its length 0. */
    uint8_t len;                           /**< How many characters the text holds, the pending one included. */
    uint8_t cap;                           /**< The most characters the text may hold. */
    bool used;                             /**< A key was typed in a mode other than lower case. */
    uint8_t mode;                          /**< The current kui_multitap_mode_t. */
    uint8_t key;                           /**< The key of the pending character; meaningful while `pending`. */
    uint8_t idx;                           /**< Where the pending character sits in its key's characters; meaningful while `pending`. */
    bool pending;                          /**< The last character is pending: the same key still cycles it. */
    bool fresh;                            /**< The last character was typed or cycled, and no delete has come since. */
    uint32_t press_ms;                     /**< The time of the last press that typed or cycled a character. */
} kui_multitap_t;

_Static_assert(((unsigned int)KUI_MULTITAP_KEY_9 + 1U) == KUI_MULTITAP_CHAR_KEYS, "the character keys are the digits");
_Static_assert(((unsigned int)KUI_MULTITAP_KEY_DELETE + 1U) == KUI_MULTITAP_KEYS, "the keys are the enumerators");
_Static_assert(((unsigned int)KUI_MULTITAP_MODE_SYMBOLS + 1U) == KUI_MULTITAP_MODES, "the modes are the enumerators");
_Static_assert(KUI_MULTITAP_TEXT_MAX <= UINT8_MAX, "the length is a byte");
_Static_assert(KUI_MULTITAP_SSID_MAX <= KUI_MULTITAP_TEXT_MAX, "a name is never longer than the longest text");
_Static_assert(KUI_MULTITAP_SHOW_MS >= KUI_MULTITAP_MS, "a pending character is still plain");

/**
 * @brief Starts an empty text in the lower-case mode.
 *
 * It wipes the whole state, the text buffer included, with the wipe
 * kui_multitap_clear() uses.
 *
 * @param m The multitap; NULL is ignored.
 * @param cap The most characters the text may hold, from 1 to KUI_MULTITAP_TEXT_MAX (KUI_MULTITAP_SSID_MAX for a network name); a value outside that range is KUI_MULTITAP_TEXT_MAX.
 */
void kui_multitap_init(kui_multitap_t *m, uint8_t cap);

/**
 * @brief Presses a key.
 *
 * A pending character whose KUI_MULTITAP_MS has passed is committed first,
 * so a press never cycles a character the tick has not yet committed. A
 * press of a character key other than the pending one commits the pending
 * one even when the new character is then refused at the cap. A refused
 * press changes nothing but those commits: the time the last character shows
 * plain from is not moved. An ignored press changes nothing.
 *
 * @param m The multitap; NULL answers KUI_MULTITAP_IGNORED.
 * @param key The key, a kui_multitap_key_t; a value outside the keypad answers KUI_MULTITAP_IGNORED.
 * @param t_ms The clock now, in milliseconds.
 * @return What the press did.
 */
kui_multitap_result_t kui_multitap_press(kui_multitap_t *m, kui_multitap_key_t key, uint32_t t_ms);

/**
 * @brief Commits the pending character once KUI_MULTITAP_MS has passed since the press that typed or cycled it.
 *
 * Less than that is still the pending character's window; exactly that is not.
 *
 * The card must call it periodically, at any interval well under
 * KUI_MULTITAP_MS (100 ms is plenty): kui_multitap_pending() and
 * kui_multitap_show_plain() are const and expire nothing, so without ticks a
 * pending character stays pending, and shows plain, until the next tick or
 * press.
 *
 * @param m The multitap; NULL is ignored.
 * @param t_ms The clock now, in milliseconds, from the counter every press uses.
 */
void kui_multitap_tick(kui_multitap_t *m, uint32_t t_ms);

/**
 * @brief The text.
 *
 * @param m The multitap; NULL answers "".
 * @return The text, NUL-terminated, valid until the next press, init or clear. The caller must not write it and must not log it.
 */
const char *kui_multitap_text(const kui_multitap_t *m);

/**
 * @brief The number of characters in the text, the pending one included.
 *
 * @param m The multitap; NULL answers 0.
 * @return The length.
 */
size_t kui_multitap_len(const kui_multitap_t *m);

/**
 * @brief Whether the last character is pending, still open to the same key's cycle.
 *
 * It reads the state as the last press or tick left it and expires nothing.
 *
 * @param m The multitap; NULL answers false.
 * @return true while a character is pending.
 */
bool kui_multitap_pending(const kui_multitap_t *m);

/**
 * @brief Whether the last character shows plain.
 *
 * The card shows every other character as a dot.
 *
 * @param m The multitap; NULL answers false.
 * @param t_ms The clock now, in milliseconds.
 * @return true while the last character is pending, and for KUI_MULTITAP_SHOW_MS after the press that typed or cycled it; false when it was deleted, when nothing is typed and after that time.
 */
bool kui_multitap_show_plain(const kui_multitap_t *m, uint32_t t_ms);

/**
 * @brief The current mode.
 *
 * @param m The multitap; NULL answers KUI_MULTITAP_MODE_LOWER.
 * @return The mode.
 */
kui_multitap_mode_t kui_multitap_mode(const kui_multitap_t *m);

/**
 * @brief The characters a key types in a mode, in the order its presses cycle them.
 *
 * It is the table the typing uses, so a card that draws its labels from it
 * cannot disagree with what the key types. The first character is what one
 * press types.
 *
 * @param key The key.
 * @param mode The mode.
 * @return A NUL-terminated string of static storage, or NULL for the mode key, Delete, or a key or mode outside the keypad.
 */
const char *kui_multitap_chars(kui_multitap_key_t key, kui_multitap_mode_t mode);

/**
 * @brief Forgets the text.
 *
 * Zeroes every byte of the text buffer, not only up to its length, in a way
 * the compiler may not drop, and resets the state to an empty text in the
 * lower-case mode. The cap stays, so the multitap can take the next text
 * without an init. The card calls it when it closes and after the join.
 *
 * @param m The multitap; NULL is ignored.
 */
void kui_multitap_clear(kui_multitap_t *m);

#ifdef __cplusplus
}
#endif

#endif /* KUI_MULTITAP_H */
