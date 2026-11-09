/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_kody.c
 * @brief The Kody player, on lv_lottie. See kui_kody.h.
 *
 * Seven facts about lv_lottie and LVGL 9.6 decide the design.
 *
 * 1. A widget takes one source. ThorVG refuses a second load into a picture
 *    that has drawn, and lv_lottie_set_src_data() does not look at the
 *    answer, so every animation gets a widget of its own. The widgets take
 *    turns on the player's one buffer: the old one is deleted and the new one
 *    created on the same buffer in the same call, so the glass sees no gap.
 *    Deleting first keeps one ThorVG model alive at a time; its nodes are
 *    small allocations, and on this board those land in internal RAM unless
 *    the firmware routes C++'s new elsewhere (see the README).
 * 2. A widget keeps a raw pointer to its anim and is never told that the
 *    anim ended, so a finite repeat count would free the anim under it. Every
 *    anim here repeats forever, and a one-shot is stopped by pausing it on
 *    its last frame.
 * 3. lv_lottie spreads the file's duration over frames 0 .. total, one more
 *    than there are, so the last pose shows twice. The player owns that
 *    arithmetic instead: the anim is only a clock, and kody_path() turns its
 *    time into a frame at the animation's own rate, never past the frame where
 *    something is due. kody_exec(), which the anim calls once the widget has
 *    drawn that frame, does what is due there. Both ask kui_kody_anchor.h.
 * 4. A widget's destructor reads its draw buffer's header, so a widget always
 *    goes before the buffer.
 * 5. lv_lottie draws only a visible widget. Meanwhile it sets the clock back
 *    to the time of its own last draw, which is 0 for a widget that has not
 *    drawn since it was started, whatever frame the player started it on,
 *    and it leaves the anim's value where the path put it, ahead of that
 *    clock. kody_path() would read the step back as a wrap through the end
 *    of the pass and hold the frame where a switch is due, and the widget
 *    would draw that frame the moment it is seen. So kody_exec() sets the
 *    clock and the value back to the last frame drawn: a player that is not
 *    seen stands on that frame, and switches only on frames that were drawn.
 * 6. kody_arrive() switches from inside kody_exec(), the custom exec of the
 *    anim it ends, and the switch deletes that anim's widget, and the anim
 *    with it. That is safe because LVGL 9.6's anim timer notes a delete made
 *    during its walk (anim_list_changed): it touches the freed anim no
 *    further and starts its walk again from the list's head. Every LVGL bump
 *    checks it still does (src/misc/lv_anim.c, anim_timer()).
 * 7. lv_lottie_set_src_data() keeps nothing of the JSON it is handed. It
 *    loads with ThorVG's copy flag set, so the Lottie loader copies the bytes
 *    into a block of its own (lv_malloc(), one byte longer for the NUL its
 *    parser needs), parses that copy in place, and frees it once the model
 *    is built, all before the call returns: this ThorVG runs no worker
 *    thread. So the JSON an animation is inflated to is needed for that one
 *    call. kody_start() inflates it into a PSRAM block, hands it over and
 *    frees it, and no JSON is held while an animation plays. Every LVGL bump
 *    checks the flag is still set (src/widgets/lottie/lv_lottie.c,
 *    lv_lottie_set_src_data(); src/libs/thorvg/tvgLottieLoader.cpp,
 *    LottieLoader::open()): without it ThorVG parses the caller's block in
 *    place, and its parser reads on to a NUL that the player's block does
 *    not end in.
 */
#include "lvgl.h"

#if LV_USE_LOTTIE

#include <stddef.h>

#include "khal_zlib.h"
#include "khal_system.h"
#include "khal_time.h"
#include "kode_log.h"
#include "kui_kody.h"
#include "kui_kody_anchor.h"

/** The log tag. */
#define TAG "kui.kody"

/** Milliseconds in a second: the anim's clock counts the one, an animation's rate the other. */
#define MS_PER_S 1000U

/** Microseconds in a millisecond: khal's clock counts the one, the log line of an inflate the other. */
#define US_PER_MS 1000U

/** @brief What a player knows. Lives in the object's user data from create to delete. */
typedef struct {
    lv_obj_t *self;                  /**< The player object. */
    lv_draw_buf_t *buf;              /**< w x h ARGB8888, premultiplied; NULL until the first play. */
    lv_obj_t *lottie;                /**< The widget that plays cur; NULL when there is none. */
    const kui_kody_anim_t *cur;      /**< What lottie plays. */
    const kui_kody_anim_t *next;     /**< Queued by kui_kody_then() for the end of the pass; NULL for none. */
    const kui_kody_anim_t *reaction; /**< Waiting for the loop's next anchor; NULL for none. */
    const kui_kody_anim_t *ret;      /**< The loop the reaction that plays returns to; NULL for none. */
    uint16_t w;                      /**< Canvas width, px. */
    uint16_t h;                      /**< Canvas height, px. */
    uint16_t frame;                  /**< The last frame lottie drew. */
    uint16_t ret_frame;              /**< The anchor the reaction that plays started on. */
    bool loop;                       /**< cur loops. */
    bool next_loop;                  /**< next loops. */
    bool freeze;                     /**< Stop on the next anchor the loop shows. */
    bool held;                       /**< cur was played once and holds its last frame. */
} kody_t;

/**
 * @brief The player behind an object.
 * @param k The object, from kui_kody_create().
 * @return Its state, or NULL for NULL.
 */
static kody_t *kody_of(lv_obj_t *k)
{
    if ( NULL == k )
    {
        return NULL;
    }
    return (kody_t *)lv_obj_get_user_data(k);
}

/**
 * @brief The clock of the widget that plays now.
 * @param st The player.
 * @return Its anim, or NULL when nothing plays.
 */
static lv_anim_t *kody_anim(const kody_t *st)
{
    if ( NULL == st->lottie )
    {
        return NULL;
    }
    return lv_lottie_get_anim(st->lottie);
}

/**
 * @brief Where a frame starts on the anim's clock.
 *
 * Rounded up, so kody_path() reads exactly that frame there.
 *
 * @param a The animation.
 * @param f The frame.
 * @return Milliseconds from the start of the pass.
 */
static int32_t frame_ms(const kui_kody_anim_t *a, uint16_t f)
{
    uint32_t const fps = (uint32_t)a->fps;
    return (int32_t)((((uint32_t)f * MS_PER_S) + fps - 1U) / fps);
}

/**
 * @brief Whether an animation can play on this player.
 *
 * Logs a descriptor that cannot: the canvas is the player's for good, and
 * playing another size would stretch the picture over the buffer.
 *
 * @param st The player.
 * @param a  The animation.
 * @return true when @p a is drawn for the player's canvas and has frames to play.
 */
static bool kody_fits(const kody_t *st, const kui_kody_anim_t *a)
{
    if ( NULL == a )
    {
        return false;
    }
    if ( (a->w != st->w) || (a->h != st->h) )
    {
        kode_log(KODE_LOG_WARN, TAG, "refused a %ux%u animation on a %ux%u player",
                 (unsigned)a->w, (unsigned)a->h, (unsigned)st->w, (unsigned)st->h);
        return false;
    }
    if ( (NULL == a->zlib) || (0U == a->zlib_len) || (0U == a->json_len) || (0U == a->frames) ||
        (0U == a->fps) )
    {
        kode_log(KODE_LOG_WARN, TAG, "refused an animation with no data, no frames or no rate");
        return false;
    }
    if ( (!a->deflated) && (a->zlib_len != a->json_len) )
    {
        kode_log(KODE_LOG_WARN, TAG, "refused an animation whose plain JSON is not json_len bytes");
        return false;
    }
    return true;
}

/**
 * @brief Whether the frame on the glass is one a reaction may start on.
 *
 * An anchor of the loop; for a loop without anchors, its last frame, where
 * the pass closes on its first.
 *
 * @param st The player; it plays a loop.
 * @return true on such a frame.
 */
static bool kody_on_seam(const kody_t *st)
{
    uint16_t const anchor = (uint16_t)st->cur->anchor;
    if ( 0U == anchor )
    {
        return (uint16_t)(st->cur->frames - 1U) == st->frame;
    }
    return (st->frame % anchor) == 0U;
}

/**
 * @brief Whether a switch is due after a frame, and on which frame.
 *
 * A one-shot is due on its last frame. A loop is due on its next anchor
 * while a reaction or a freeze waits for one, and on its last frame while
 * something is queued for the end of its pass.
 *
 * @param st   The player; something plays.
 * @param from The frame the playhead leaves.
 * @param at   Where to write the frame the switch is due on.
 * @return true when a switch is due.
 */
static bool kody_due(const kody_t *st, uint16_t from, uint16_t *at)
{
    uint16_t const frames = st->cur->frames;
    if ( !st->loop )
    {
        *at = (uint16_t)(frames - 1U);
        return true;
    }
    if ( (NULL != st->reaction) || st->freeze )
    {
        *at = kui_kody_next_anchor(from, (uint16_t)st->cur->anchor, frames);
        return true;
    }
    if ( NULL != st->next )
    {
        *at = (uint16_t)(frames - 1U);
        return true;
    }
    return false;
}

/**
 * @brief Stop on the frame on the glass.
 *
 * The clock goes back to where that frame starts, so whatever comes next
 * goes on from it.
 *
 * @param st The player.
 * @param a  The clock of the widget that plays.
 */
static void kody_stop(const kody_t *st, lv_anim_t *a)
{
    a->act_time = frame_ms(st->cur, st->frame);
    lv_anim_pause(a);
}

/**
 * @brief Forget what plays and everything that waits: the player is empty.
 * @param st The player; its widget is already gone.
 */
static void kody_empty(kody_t *st)
{
    st->lottie = NULL;
    st->cur = NULL;
    st->next = NULL;
    st->reaction = NULL;
    st->ret = NULL;
    st->freeze = false;
    st->held = false;
}

static int32_t kody_path(const lv_anim_t *a);
static void kody_exec(lv_anim_t *a, int32_t value);

/**
 * @brief An animation's JSON, inflated into a PSRAM block of its own.
 *
 * The block is exactly the JSON's size, and it is the caller's to free once
 * lv_lottie has been handed it (fact 7). The decoder's state is PSRAM too,
 * taken and given back inside khal_zlib_decompress(), so the inflate takes no
 * internal RAM and puts none of its state on the stack. The time it takes is
 * logged at debug level.
 *
 * Never inlined: kody_start() is the frame under the parse and the first
 * raster, the deepest the stack of LVGL's thread goes, and what this function
 * keeps on the stack is gone again before that starts. Inlined, its locals
 * are part of that frame for as long as the parse runs (kody_start() gives
 * the two figures).
 *
 * @param a The animation, checked by kody_fits().
 * @return The block, a->json_len bytes, for khal_free(); NULL when PSRAM has
 *         no room for it or for the decoder, or the data does not inflate to
 *         the length its descriptor gives (logged).
 */
static __attribute__((noinline)) uint8_t *kody_json(const kui_kody_anim_t *a)
{
    uint8_t *const json = (uint8_t *)khal_alloc_psram(sizeof(uint32_t), (size_t)a->json_len);
    if ( NULL == json )
    {
        kode_log(KODE_LOG_WARN, TAG, "no memory for %u bytes of JSON: Kody is not drawn",
                 (unsigned)a->json_len);
        return NULL;
    }
    uint64_t const t0 = khal_time_us();
    size_t len = 0U;
    kode_status_t const rc =
        khal_zlib_decompress(a->zlib, (size_t)a->zlib_len, json, (size_t)a->json_len, &len);
    if ( (KODE_OK != rc) || ((size_t)a->json_len != len) )
    {
        khal_free(json);
        if ( KODE_NO_RESOURCES == rc )
        {
            kode_log(KODE_LOG_WARN, TAG, "no memory to inflate an animation: Kody is not drawn");
        }
        else
        {
            kode_log(KODE_LOG_WARN, TAG,
                     "an animation's %u bytes do not inflate to its %u (status %d, %u bytes): "
                     "Kody is not drawn",
                     (unsigned)a->zlib_len, (unsigned)a->json_len, (int)rc, (unsigned)len);
        }
        return NULL;
    }
    uint32_t const us = (uint32_t)(khal_time_us() - t0);
    kode_log(KODE_LOG_DEBUG, TAG, "inflated %u bytes to %u in %u.%03u ms", (unsigned)a->zlib_len,
             (unsigned)len, (unsigned)(us / US_PER_MS), (unsigned)(us % US_PER_MS));
    return json;
}

/**
 * @brief Put an animation on the player's buffer at a frame, in place of what plays.
 *
 * The widget that plays is deleted first, so one ThorVG model is alive at a
 * time, and the new one is created on the same buffer in the same call, so
 * the glass sees no gap. The animation's JSON is inflated for
 * lv_lottie_set_src_data(), which parses it and draws frame 0 at once, and
 * freed as soon as that returns; a loop picked up at an anchor shows that
 * pose there, which is frame 0's. Then the widget's anim becomes the player's
 * clock, set to @p frame and repeating forever, with kody_path() and
 * kody_exec().
 *
 * This frame lies under the parse and the first raster, so the inflate adds
 * nothing to it. The rule is in the code: kody_json() has returned, and its
 * frame and khal_zlib_decompress()'s are gone, before lv_lottie_set_src_data() is
 * called. The frame's own size is the compiler's choice of registers, and no
 * rule of this file's: 32 bytes with ESP-IDF v6.1's GCC 15.2 at -Os, and 80
 * with kody_json() inlined into it. Every toolchain bump reads it again, one
 * of two ways: as the `addi sp,sp,-<bytes>` of kody_start's prologue in the
 * image's disassembly (riscv32-esp-elf-objdump -d --disassemble=kody_start
 * on the ELF), or as kody_start's line in the .su file the compiler writes
 * beside the object when it is given -fstack-usage.
 *
 * @param st    The player.
 * @param a     The animation, checked by kody_fits().
 * @param loop  Whether it loops.
 * @param frame The frame it starts on: 0, or an anchor of a loop.
 * @return true when it plays; false when the buffer, the widget, its anim or
 *         the JSON could not be allocated, or the data does not inflate
 *         (logged), which leaves the player empty.
 */
static bool kody_start(kody_t *st, const kui_kody_anim_t *a, bool loop, uint16_t frame)
{
    if ( NULL == st->buf )
    {
        st->buf = lv_draw_buf_create((uint32_t)st->w, (uint32_t)st->h,
                                     LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED, LV_STRIDE_AUTO);
        if ( NULL == st->buf )
        {
            kode_log(KODE_LOG_WARN, TAG, "no memory for a %ux%u buffer: Kody is not drawn",
                     (unsigned)st->w, (unsigned)st->h);
            return false;
        }
    }
    if ( NULL != st->lottie )
    {
        lv_obj_delete(st->lottie);
        st->lottie = NULL;
    }
    st->held = false;

    lv_obj_t *const w = lv_lottie_create(st->self);
    if ( NULL == w )
    {
        kody_empty(st);
        kode_log(KODE_LOG_WARN, TAG, "no memory for a widget: Kody is not drawn");
        return false;
    }
    lv_anim_t *const clock = lv_lottie_get_anim(w);
    if ( NULL == clock )
    {
        lv_obj_delete(w);
        kody_empty(st);
        kode_log(KODE_LOG_WARN, TAG, "no memory for a widget's anim: Kody is not drawn");
        return false;
    }
    lv_obj_set_name_static(w, "lottie");
    lv_obj_set_clickable(w, false);
    lv_obj_set_scrollable(w, false);
    lv_obj_set_pos(w, 0, 0);
    lv_lottie_set_draw_buf(w, st->buf);
    const uint8_t *src = a->zlib;
    uint8_t *json = NULL;
    if ( a->deflated )
    {
        json = kody_json(a);
        if ( NULL == json )
        {
            lv_obj_delete(w);
            kody_empty(st);
            return false;
        }
        src = json;
    }
    lv_lottie_set_src_data(w, src, (size_t)a->json_len);
    /* ThorVG has parsed a copy of its own and let it go (fact 7). */
    khal_free(json);

    lv_anim_set_user_data(clock, st);
    lv_anim_set_path_cb(clock, kody_path);
    lv_anim_set_custom_exec_cb(clock, kody_exec);
    lv_anim_set_values(clock, 0, (int32_t)a->frames - 1);
    lv_anim_set_duration(clock, ((uint32_t)a->frames * MS_PER_S) / (uint32_t)a->fps);
    lv_anim_set_repeat_count(clock, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_repeat_delay(clock, 0U);
    clock->act_time = frame_ms(a, frame);
    clock->current_value = (int32_t)frame;
    /* The parse and the first raster took a while: none of it is play time. */
    clock->last_timer_run = lv_tick_get();

    st->lottie = w;
    st->cur = a;
    st->loop = loop;
    st->frame = frame;
    return true;
}

/**
 * @brief Start a loop at a frame, and stop it there when a freeze waits for it.
 * @param st    The player.
 * @param a     The loop.
 * @param frame An anchor of it, or 0.
 */
static void kody_enter_loop(kody_t *st, const kui_kody_anim_t *a, uint16_t frame)
{
    if ( kody_start(st, a, true, frame) && st->freeze )
    {
        st->freeze = false;
        kody_stop(st, kody_anim(st));
    }
}

/**
 * @brief Start what was queued for the end of the pass.
 * @param st The player; st->next is set.
 */
static void kody_go_next(kody_t *st)
{
    const kui_kody_anim_t *const n = st->next;
    bool const l = st->next_loop;
    st->next = NULL;
    if ( l )
    {
        kody_enter_loop(st, n, 0U);
    }
    else
    {
        (void)kody_start(st, n, false, 0U);
    }
}

/**
 * @brief Do what is due on the frame just drawn.
 *
 * Starting another animation deletes the widget @p a belongs to, and @p a
 * with it: nothing here touches @p a after a start.
 *
 * @param st The player.
 * @param a  The clock of the widget that drew the frame.
 * @param at The frame the switch was due on: the frame just drawn.
 */
static void kody_arrive(kody_t *st, lv_anim_t *a, uint16_t at)
{
    if ( !st->loop )
    {
        if ( NULL != st->ret )
        {
            /* A reaction ends on the anchor pose: the loop comes back at the
             * anchor it left. */
            const kui_kody_anim_t *const back = st->ret;
            st->ret = NULL;
            kody_enter_loop(st, back, st->ret_frame);
        }
        else if ( NULL != st->next )
        {
            kody_go_next(st);
        }
        else
        {
            kody_stop(st, a);
            st->held = true;
        }
    }
    else if ( NULL != st->reaction )
    {
        const kui_kody_anim_t *const r = st->reaction;
        st->reaction = NULL;
        st->ret = st->cur;
        st->ret_frame = at;
        if ( !kody_start(st, r, false, 0U) )
        {
            st->ret = NULL;
        }
    }
    else if ( st->freeze )
    {
        st->freeze = false;
        kody_stop(st, a);
    }
    else if ( NULL != st->next )
    {
        kody_go_next(st);
    }
    else
    {
        /* Nothing was due: kody_due() said otherwise a moment ago. */
    }
}

/**
 * @brief The anim's path: the frame its clock is on, held at the frame where a switch is due.
 *
 * Holding the clock there is what makes a switch exact: the frame the widget
 * draws is the anchor, never the one after it, however late the pass.
 *
 * @param a The widget's anim; its user data is the player.
 * @return The frame, 0 .. frames - 1.
 */
static int32_t kody_path(const lv_anim_t *a)
{
    const kody_t *const st = (const kody_t *)lv_anim_get_user_data(a);
    if ( (NULL == st) || (NULL == st->cur) )
    {
        return 0;
    }
    uint16_t const frames = st->cur->frames;
    uint32_t f = 0U;
    if ( 0 < a->act_time )
    {
        f = ((uint32_t)a->act_time * (uint32_t)st->cur->fps) / MS_PER_S;
    }
    if ( (uint32_t)frames <= f )
    {
        f = (uint32_t)frames - 1U;
    }
    uint16_t from = 0U;
    if ( (0 < a->current_value) && ((int32_t)frames > a->current_value) )
    {
        from = (uint16_t)a->current_value;
    }
    uint16_t at = 0U;
    if ( kody_due(st, from, &at) && kui_kody_crossed(from, (uint16_t)f, at, frames) )
    {
        f = (uint32_t)at;
    }
    return (int32_t)f;
}

/**
 * @brief The anim's custom exec: do what is due on the frame the widget drew.
 *
 * A widget that is not visible draws nothing; the playhead is held on the
 * last frame drawn instead (fact 5).
 *
 * @param a     The widget's anim; its user data is the player.
 * @param value The frame, from kody_path().
 */
static void kody_exec(lv_anim_t *a, int32_t value)
{
    kody_t *const st = (kody_t *)lv_anim_get_user_data(a);
    if ( (NULL == st) || (NULL == st->lottie) || ((void *)st->lottie != a->var) )
    {
        return;
    }
    /* A frame lv_lottie did not draw is not on the glass, and nothing
     * switches on it. lv_lottie has put the clock back to its own last draw,
     * which knows nothing of the frame the player started it on (fact 5):
     * the clock and the path's value go back to the start of the last frame
     * drawn instead, where the playhead stands until the widget is seen. */
    if ( !lv_obj_is_visible(st->lottie) )
    {
        a->act_time = frame_ms(st->cur, st->frame);
        a->current_value = (int32_t)st->frame;
        return;
    }
    uint16_t const frames = st->cur->frames;
    uint16_t now = 0U;
    if ( (int32_t)frames <= value )
    {
        now = (uint16_t)(frames - 1U);
    }
    else if ( 0 < value )
    {
        now = (uint16_t)value;
    }
    else
    {
        now = 0U;
    }
    uint16_t const from = st->frame;
    st->frame = now;
    uint16_t at = 0U;
    if ( kody_due(st, from, &at) && kui_kody_crossed(from, now, at, frames) )
    {
        kody_arrive(st, a, at);
    }
}

/**
 * @brief LV_EVENT_DELETE: the widget, then the buffer, then the state.
 *
 * The widget's destructor reads the buffer's header, so it is deleted here,
 * first, rather than left to the object's own teardown, which reaches the
 * children only after this handler has run.
 *
 * @param e The event.
 */
static void kody_on_delete(lv_event_t *e)
{
    lv_obj_t *const k = lv_event_get_current_target_obj(e);
    if ( lv_event_get_target_obj(e) != k )
    {
        return;
    }
    kody_t *const st = kody_of(k);
    if ( NULL == st )
    {
        return;
    }
    if ( NULL != st->lottie )
    {
        lv_obj_delete(st->lottie);
        st->lottie = NULL;
    }
    if ( NULL != st->buf )
    {
        lv_draw_buf_destroy(st->buf);
        st->buf = NULL;
    }
    lv_obj_set_user_data(k, NULL);
    lv_free(st);
}

lv_obj_t *kui_kody_create(lv_obj_t *parent, uint16_t w, uint16_t h)
{
    if ( (0U == w) || (0U == h) )
    {
        kode_log(KODE_LOG_WARN, TAG, "refused a %ux%u player", (unsigned)w, (unsigned)h);
        return NULL;
    }
    kody_t *const st = (kody_t *)lv_malloc_zeroed(sizeof(kody_t));
    if ( NULL == st )
    {
        kode_log(KODE_LOG_WARN, TAG, "no memory for a player");
        return NULL;
    }
    lv_obj_t *const k = lv_obj_create(parent);
    if ( NULL == k )
    {
        lv_free(st);
        kode_log(KODE_LOG_WARN, TAG, "no memory for a player");
        return NULL;
    }
    lv_obj_remove_style_all(k);
    lv_obj_set_name_static(k, "kody_#");
    lv_obj_set_size(k, (int32_t)w, (int32_t)h);
    /* A tap or a swipe on Kody belongs to whatever holds him until the caller
     * says otherwise; and if it makes him clickable, a tap must not scroll
     * the parent to show him. */
    lv_obj_set_clickable(k, false);
    lv_obj_set_click_focusable(k, false);
    lv_obj_set_scrollable(k, false);
    lv_obj_set_scroll_on_focus(k, false);
    st->self = k;
    st->w = w;
    st->h = h;
    lv_obj_set_user_data(k, st);
    lv_obj_add_event_cb(k, kody_on_delete, LV_EVENT_DELETE, NULL);
    return k;
}

bool kui_kody_play(lv_obj_t *k, const kui_kody_anim_t *a, bool loop)
{
    kody_t *const st = kody_of(k);
    if ( (NULL == st) || !kody_fits(st, a) )
    {
        return false;
    }
    st->next = NULL;
    st->reaction = NULL;
    st->ret = NULL;
    st->freeze = false;
    return kody_start(st, a, loop, 0U);
}

void kui_kody_then(lv_obj_t *k, const kui_kody_anim_t *next, bool loop)
{
    kody_t *const st = kody_of(k);
    if ( (NULL == st) || !kody_fits(st, next) )
    {
        return;
    }
    if ( (NULL == st->lottie) || st->held )
    {
        (void)kody_start(st, next, loop, 0U);
    }
    else
    {
        st->next = next;
        st->next_loop = loop;
    }
}

bool kui_kody_react(lv_obj_t *k, const kui_kody_anim_t *r)
{
    kody_t *const st = kody_of(k);
    if ( (NULL == st) || !kody_fits(st, r) )
    {
        return false;
    }
    lv_anim_t *const a = kody_anim(st);
    if ( (NULL == a) || !st->loop || (NULL != st->reaction) )
    {
        return false;
    }
    st->freeze = false;
    if ( kody_on_seam(st) )
    {
        st->ret = st->cur;
        st->ret_frame = st->frame;
        if ( !kody_start(st, r, false, 0U) )
        {
            st->ret = NULL;
            return false;
        }
        return true;
    }
    st->reaction = r;
    if ( lv_anim_is_paused(a) )
    {
        lv_anim_resume(a);
    }
    return true;
}

void kui_kody_freeze(lv_obj_t *k)
{
    kody_t *const st = kody_of(k);
    if ( NULL == st )
    {
        return;
    }
    lv_anim_t *const a = kody_anim(st);
    if ( (NULL == a) || lv_anim_is_paused(a) )
    {
        return;
    }
    if ( st->loop )
    {
        if ( (0U == st->cur->anchor) || kody_on_seam(st) )
        {
            kody_stop(st, a);
        }
        else
        {
            st->freeze = true;
        }
    }
    else if ( (NULL != st->ret) || ((NULL != st->next) && st->next_loop) )
    {
        st->freeze = true;
    }
    else
    {
        kody_stop(st, a);
    }
}

void kui_kody_pause(lv_obj_t *k)
{
    kody_t *const st = kody_of(k);
    if ( NULL == st )
    {
        return;
    }
    lv_anim_t *const a = kody_anim(st);
    if ( (NULL != a) && !lv_anim_is_paused(a) )
    {
        kody_stop(st, a);
    }
}

void kui_kody_resume(lv_obj_t *k)
{
    kody_t *const st = kody_of(k);
    if ( NULL == st )
    {
        return;
    }
    st->freeze = false;
    lv_anim_t *const a = kody_anim(st);
    if ( (NULL != a) && !st->held && lv_anim_is_paused(a) )
    {
        lv_anim_resume(a);
    }
}

void kui_kody_stop(lv_obj_t *k)
{
    kody_t *const st = kody_of(k);
    if ( NULL == st )
    {
        return;
    }
    if ( NULL != st->lottie )
    {
        lv_obj_delete(st->lottie);
    }
    kody_empty(st);
}

bool kui_kody_is_still(lv_obj_t *k)
{
    kody_t *const st = kody_of(k);
    if ( NULL == st )
    {
        return true;
    }
    lv_anim_t *const a = kody_anim(st);
    if ( NULL == a )
    {
        return true;
    }
    return lv_anim_is_paused(a);
}

bool kui_kody_is_reacting(lv_obj_t *k)
{
    const kody_t *const st = kody_of(k);
    if ( NULL == st )
    {
        return false;
    }
    /* One waits for the loop's anchor, or one plays and the loop it returns
     * to is kept. */
    return (NULL != st->reaction) || (NULL != st->ret);
}

bool kui_kody_reaction_plays(lv_obj_t *k)
{
    const kody_t *const st = kody_of(k);

    return (NULL != st) && (NULL != st->ret);
}

#endif /* LV_USE_LOTTIE */
