/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_pack.h
 * @brief Kody's pack: the manifest kode.json, and the checks a Lottie passes before it plays.
 */
#ifndef KUI_PACK_H
#define KUI_PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KUI_PACK_FORMAT         1U                /**< The one manifest format this firmware reads. */
#define KUI_PACK_MANIFEST_MAX   (256U * 1024U)    /**< Bytes in kode.json, at most. */
#define KUI_PACK_FILE_MAX       (64U * 1024U)     /**< Bytes in one file as stored, at most. */
#define KUI_PACK_JSON_MAX       (96U * 1024U)     /**< Bytes of a Lottie's JSON once inflated, at most. */
/* ThorVG rasterises on AO_Gui's stack and recurses as deep as a Lottie nests. */
#define KUI_PACK_MAX_DEPTH      32U               /**< Deepest JSON nesting a file may reach. */
#define KUI_PACK_MAX_PRECOMP    2U                /**< Longest chain of precomps a Lottie may hold. */
#define KUI_PACK_NAME_MAX       15U               /**< Characters in a scene's name. */
#define KUI_PACK_FILE_NAME_MAX  40U               /**< Characters in a file's name. */
#define KUI_PACK_TEXT_MAX       31U               /**< Characters in the pack's name and version. */
#define KUI_PACK_FRAMES_MAX     1800U             /**< Frames in one pass, at most. */
#define KUI_PACK_FPS_MAX        60U               /**< Frame rate, at most. */
#define KUI_PACK_SHA256_LEN     32U               /**< Bytes in a digest. */
#define KUI_PACK_NONE           0xFFFFU           /**< No index: no such scene, file or role. */
#define KUI_PACK_SCENE_W        410U              /**< The face's canvas width, px. */
#define KUI_PACK_SCENE_H        334U              /**< The face's canvas height, px: a 288 px scene at 1.16. */
#define KUI_PACK_SCENES_MAX     32U               /**< Scenes in one manifest, at most. */
#define KUI_PACK_REACT_MAX      128U              /**< Reactions of every scene together, at most. */
#define KUI_PACK_EMPTY_W        410U              /**< The empty launcher's canvas width, px. */
#define KUI_PACK_EMPTY_H        369U              /**< The empty launcher's canvas height, px. */
#define KUI_PACK_CARD_W         410U              /**< The Opening card's canvas width, px. */
#define KUI_PACK_CARD_H         160U              /**< The Opening card's canvas height, px. */
#define KUI_PACK_LOADING_SIDE   264U              /**< The loading screen's square canvas, px. */
#define KUI_PACK_HOUR_LAST      23U               /**< The last hour a window may start at. */
#define KUI_PACK_HOUR_END       24U               /**< The hour the last window may end at. */
#define KUI_PACK_ANCHOR_MAX     0xFFU             /**< An anchor spacing fits a byte (kui_kody_anim_t.anchor). */
#define KUI_PACK_LOG_TEXT_MAX   40U               /**< Characters of a manifest string a log line quotes. */

/** @brief Why a manifest or a file was refused; kui_pack_err_word() names each. */
typedef enum {
    KUI_PACK_OK = 0,      /**< Nothing refused. */
    KUI_PACK_E_SIZE,      /**< Empty, or larger than its limit. */
    KUI_PACK_E_DEPTH,     /**< Nested deeper than KUI_PACK_MAX_DEPTH. */
    KUI_PACK_E_JSON,      /**< Not JSON, or not the shape a Lottie has. */
    KUI_PACK_E_FORMAT,    /**< No `format`, or not KUI_PACK_FORMAT. */
    KUI_PACK_E_SCHEMA,    /**< A key of the wrong type or out of range. */
    KUI_PACK_E_NAME,      /**< A scene name that breaks the rule, or twice. */
    KUI_PACK_E_FILE,      /**< A file name that breaks the rule, or missing. */
    KUI_PACK_E_HASH,      /**< A `sha256` entry that is not a digest. */
    KUI_PACK_E_CANVAS,    /**< Not drawn for its role's canvas. */
    KUI_PACK_E_FRAMES,    /**< No frames, too many, or not a multiple of the anchor. */
    KUI_PACK_E_FPS,       /**< A rate that is not a whole 1-60. */
    KUI_PACK_E_PRECOMP,   /**< Precomps chained deeper than the bound, or in a cycle. */
    KUI_PACK_E_MEMORY     /**< PSRAM had no room. */
} kui_pack_err_t;

/** @brief The roles outside the face. */
typedef enum {
    KUI_PACK_ROLE_EMPTY = 0,  /**< The launcher with no apps: 410 x 369. */
    KUI_PACK_ROLE_WRITING,    /**< The Opening card while an image writes: 410 x 160. */
    KUI_PACK_ROLE_GOODBYE,    /**< The wave before a run: 410 x 160. */
    KUI_PACK_ROLE_LOADING,    /**< The loading screen: 264 x 264. */
    KUI_PACK_ROLE_COUNT
} kui_pack_role_t;

/** @brief What a scene reacts to. */
typedef enum {
    KUI_PACK_TRIGGER_LOOK = 0,  /**< He looks at you: back on the face, once a minute at most. */
    KUI_PACK_TRIGGER_HOUR,      /**< He looks at the time: on the hour, or when it becomes known. */
    KUI_PACK_TRIGGER_NOTICE,    /**< He reads a notice. */
    KUI_PACK_TRIGGER_PLUG,      /**< The cable. */
    KUI_PACK_TRIGGER_JOKE,      /**< The scene's joke: on waking, back on the face, every 2 to 3 minutes. */
    KUI_PACK_TRIGGER_COUNT
} kui_pack_trigger_t;

/** @brief One file the manifest names. */
typedef struct {
    char    name[KUI_PACK_FILE_NAME_MAX + 1U];  /**< Its name in the pack's folder. */
    uint8_t sha256[KUI_PACK_SHA256_LEN];       /**< Its digest, when `hashed`. */
    bool    hashed;                            /**< The manifest gave a digest. */
} kui_pack_file_t;

/** @brief One scene of the face. */
typedef struct {
    char     name[KUI_PACK_NAME_MAX + 1U];       /**< Its name. */
    uint16_t wake;                               /**< Index into files: the wake-up. */
    uint16_t loop;                               /**< Index into files: the loop. */
    uint16_t anchor;                             /**< The loop's anchor spacing; 0 = none. */
    uint16_t on_first[KUI_PACK_TRIGGER_COUNT];   /**< Per trigger, the first index into react. */
    uint16_t on_count[KUI_PACK_TRIGGER_COUNT];   /**< Per trigger, how many. */
} kui_pack_scene_t;

/** @brief One window of the hour's pick: from `from` to `to`, `to` excluded. */
typedef struct {
    uint8_t  from;   /**< First hour, 0-23. */
    uint8_t  to;     /**< The hour it ends at, 1-24, after `from`. */
    uint16_t scene;  /**< Index into scenes. */
} kui_pack_window_t;

/** @brief A parsed manifest. Arrays in PSRAM; free with kui_pack_free(). */
typedef struct {
    char               name[KUI_PACK_TEXT_MAX + 1U];     /**< `name`; empty when absent. */
    char               version[KUI_PACK_TEXT_MAX + 1U];  /**< `version`; empty when absent. */
    kui_pack_file_t   *files;                            /**< Every file named, once. */
    uint16_t           n_files;                          /**< How many. */
    kui_pack_scene_t  *scenes;                           /**< The scenes, in order. */
    uint16_t           n_scenes;                         /**< How many; at least one. */
    uint16_t          *react;                            /**< Reaction file indices, by scene and trigger. */
    uint16_t           n_react;                          /**< How many. */
    kui_pack_window_t *windows;                          /**< The hour's windows. */
    uint16_t           n_windows;                        /**< How many. */
    uint16_t           default_scene;                    /**< The scene outside every window. */
    uint16_t           role[KUI_PACK_ROLE_COUNT];        /**< Per role, an index into files, or KUI_PACK_NONE. */
    uint32_t           crc;                              /**< CRC-32 of the manifest's bytes. */
} kui_pack_t;

/**
 * @brief The word a refusal is logged with.
 * @param e A refusal.
 * @return "ok", "size", "depth", "json", "format", "schema", "name", "file",
 *         "hash", "canvas", "frames", "fps", "precomp" or "memory"; "error"
 *         for any other value.
 */
const char *kui_pack_err_word(kui_pack_err_t e);

/**
 * @brief How deep JSON text nests, counted without recursion.
 *
 * Brackets and braces outside strings; an escaped quote does not end a
 * string. The text is not validated: this is the bound cJSON's recursive
 * parser is held to before it sees the text.
 *
 * @param text The text; NULL counts as empty.
 * @param len  Bytes in it.
 * @return The deepest nesting: 0 for a scalar, 1 for a flat object.
 */
uint16_t kui_pack_json_depth(const char *text, size_t len);

/**
 * @brief Whether a name may be a file of a pack.
 * @param name The name.
 * @return true for 1-40 characters of [A-Za-z0-9_.-], not starting with
 *         '.', ending ".json" or ".json.z"; false otherwise and for NULL.
 */
bool kui_pack_file_name_ok(const char *name);

/**
 * @brief Parse and check a manifest.
 *
 * Unknown keys are ignored at every level (logged at debug level), so a
 * pack made for a newer firmware reads here. A window naming no scene is
 * dropped, and a `default` naming none leaves the first scene the default,
 * each with a warning.
 *
 * An optional `canvas` ({"w":410,"h":334}) says what the pack's scenes are drawn
 * for. One that is not KUI_PACK_SCENE_W x KUI_PACK_SCENE_H refuses the whole pack
 * with KUI_PACK_E_CANVAS, without reading a file of it. A pack without the key is
 * judged file by file, as ever.
 *
 * @param text The manifest's bytes.
 * @param len  How many.
 * @param out  Filled on success; on failure it holds no arrays (files NULL).
 * @return KUI_PACK_OK, or why it was refused.
 */
kui_pack_err_t kui_pack_parse(const char *text, size_t len, kui_pack_t *out);

/**
 * @brief Free a parsed manifest and zero it. NULL-safe; idempotent.
 * @param p The pack.
 */
void kui_pack_free(kui_pack_t *p);

/**
 * @brief A scene's index by name.
 * @param p    The pack.
 * @param name The name.
 * @return The index, or KUI_PACK_NONE.
 */
uint16_t kui_pack_scene_find(const kui_pack_t *p, const char *name);

/**
 * @brief A file's index by name.
 * @param p    The pack.
 * @param name The name.
 * @return The index, or KUI_PACK_NONE.
 */
uint16_t kui_pack_file_find(const kui_pack_t *p, const char *name);

/**
 * @brief The scene the hour picks.
 * @param p    The pack.
 * @param hour The local hour, 0-23; negative while the time is unknown.
 * @return The scene of the first window holding @p hour; otherwise, and
 *         for a negative hour, the default scene.
 */
uint16_t kui_pack_scene_for_hour(const kui_pack_t *p, int8_t hour);

/**
 * @brief Whether every file the manifest names carries a digest.
 * @param p The pack.
 * @return true when all do (an installer's precondition).
 */
bool kui_pack_hashed(const kui_pack_t *p);

/**
 * @brief A role's canvas.
 * @param role The role.
 * @param w    Receives the width, px; 0 for a role out of range.
 * @param h    Receives the height, px; 0 for a role out of range.
 */
void kui_pack_role_canvas(kui_pack_role_t role, uint16_t *w, uint16_t *h);

/**
 * @brief The canvas and anchor a file plays with.
 *
 * A scene's loop plays with the scene's anchor; a wake-up and a reaction
 * with none; a role's file on the role's canvas with none.
 *
 * @param p      The pack.
 * @param file   An index into files.
 * @param w      Receives the canvas width.
 * @param h      Receives the canvas height.
 * @param anchor Receives the anchor.
 * @return false when the file serves two different canvases or anchors, is
 *         used by nothing, or the index is out of range.
 */
bool kui_pack_file_canvas(const kui_pack_t *p, uint16_t file, uint16_t *w, uint16_t *h,
                          uint16_t *anchor);

/** @brief What a Lottie that passed the checks is. */
typedef struct {
    uint16_t w;        /**< Canvas width, px. */
    uint16_t h;        /**< Canvas height, px. */
    uint16_t frames;   /**< Frames in one pass (op - ip). */
    uint8_t  fps;      /**< Frame rate. */
    uint16_t depth;    /**< Its JSON's deepest nesting. */
    uint16_t precomp;  /**< Its longest chain of precomps (the deepest level reached). */
} kui_pack_anim_t;

/** @brief Where the active pack comes from. */
typedef enum {
    KUI_PACK_SRC_BUILTIN = 0,  /**< No pack: the scene compiled in. */
    KUI_PACK_SRC_SD,           /**< /sdcard/kode/. */
    KUI_PACK_SRC_FS            /**< /storage/kode/, the internal FAT. */
} kui_pack_src_t;

/**
 * @brief Check one Lottie, given as plain JSON, against its role.
 *
 * Refusals in this order: size (empty, or over KUI_PACK_JSON_MAX), depth
 * (out->depth set), json, canvas (`w` and `h` not exactly @p w x @p h), fps
 * (not a whole 1-60), frames (`op - ip` not whole, not 1-1800, or not a
 * multiple of a non-zero @p anchor), precomp (out->precomp is the deepest
 * level reached). A deflated file is inflated by the caller first.
 *
 * @param json   The JSON.
 * @param len    Bytes in it.
 * @param w      The role's canvas width.
 * @param h      The role's canvas height.
 * @param anchor The anchor it plays with, or 0.
 * @param out    Zeroed, then filled as far as the checks get.
 * @return KUI_PACK_OK, or why it was refused.
 */
kui_pack_err_t kui_pack_check_anim(const char *json, size_t len, uint16_t w, uint16_t h,
                                   uint16_t anchor, kui_pack_anim_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KUI_PACK_H */
