/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_widget.c
 * @brief kode_screen_new(), its widgets and its buttons: the app's side of the screen, no LVGL.
 */
#include "kode.h"

/* cppcheck-suppress misra-c2012-17.1 ; printf-style text for a widget needs a variable argument list, bounded by vsnprintf() */
#include <stdarg.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "khal_once.h"
#include "khal_sync.h"
#include "khal_time.h"
#include "kode_screen_priv.h"
#include "kqp.h"

_Static_assert(KODE_WIDGET_TEXT_MAX == KODE_SCREEN_TEXT_MAX,
               "a widget's text is what one request carries");

_Static_assert(KODE_WIDGETS_MAX <= KODE_WIDGET_PLACE_MASK, "a place fits its bits");

_Static_assert((KODE_VALUE_NUMBER_MAX + KODE_VALUE_UNIT_MAX) <= KODE_SCREEN_TEXT_MAX,
               "a value's number and unit are what one request carries");
_Static_assert(KODE_BUTTON_LABEL_MAX <= KODE_SCREEN_TEXT_MAX,
               "a button's label is what one request carries");

/** @brief What a place of the screen holds. */
#define KIND_TEXT   0U  /**< A line of text.    */
#define KIND_VALUE  1U  /**< A value.           */
#define KIND_BUTTON 2U  /**< A button.          */
#define KIND_CANVAS 3U  /**< A canvas.          */
#define KIND_IMAGE  4U  /**< An image.          */

/** @brief People waiting at once for a text to be drawn; each has a semaphore of its own. */
#define TEXT_SLOTS 4U

/** @brief A text answer: the ticket over one bit, set when the text was drawn and clear when it was dropped. */
#define TEXT_ANSWER_BITS  1U
#define TEXT_ANSWER_DRAWN 1U

/** @brief The text wait word: a ticket of 23 bits over the slot, so it stays positive. */
#define TEXT_SLOT_BITS   8U
#define TEXT_SLOT_MASK   0xFFU
#define TEXT_TICKET_MASK 0x007FFFFFU

/** @brief Bits under the generation in a tap word. */
#define TAP_GEN_SHIFT 8U

/** @brief The tap word's flag: tapped since the last ask. */
#define TAP_FLAG 1U

/** @brief The screen as the callers number it; guarded by s_screen_lock. */
typedef struct {
    uint32_t gen;                                           /**< The screen's generation.        */
    uint32_t count;                                         /**< Widgets on it.                 */
    bool     titled;                                        /**< It has a header.              */
    uint32_t used_h;                                        /**< Body taken by its widgets, px. */
    uint32_t text_ticket;                                   /**< The last text ticket given out. */
    uint8_t  kind[KODE_WIDGETS_MAX];                         /**< KIND_* of each.               */
    char     label[KODE_WIDGETS_MAX][KODE_BUTTON_LABEL_MAX]; /**< A button's label, lower case. */
} current_t;

/** @brief What one try posts. */
typedef struct {
    uint8_t     op;     /**< KODE_SCREEN_OP_*.            */
    uint32_t    widget;  /**< The widget's place.           */
    int32_t     num[KODE_SCREEN_REQ_NUMS];  /**< The request's numbers.  */
    const char *text;   /**< Its text.                    */
    const char *unit;   /**< Sent after the text's NUL, or NULL. */
} send_t;

/** @brief The screen; guarded by s_screen_lock. */
static current_t s_current;

/** @brief Each place's tap word: the generation of the button's screen over TAP_FLAG. */
static atomic_uint_least32_t s_taps[KODE_WIDGETS_MAX];

/** @brief The lock; made by the first call. */
static khal_mutex_t s_screen_lock;

/** @brief Each slot's semaphore, given when its text is drawn; made by the first call. */
static khal_sem_t s_text_sem[TEXT_SLOTS];

/** @brief The slot is some call's. */
static atomic_bool s_text_busy[TEXT_SLOTS];

/** @brief The screen's last answer for the slot: the ticket over a drawn bit. */
static atomic_uint_least32_t s_text_done[TEXT_SLOTS];

/**
 * @brief Make the lock, take the screen's memory and attach it: what the first call runs.
 * @return true when both are done; false with the task's error set.
 */
static bool screen_start(void)
{
    bool ok = true;

    if ( NULL == s_screen_lock )
    {
        s_screen_lock = khal_mutex_create();
        if ( NULL == s_screen_lock )
        {
            kode_err_set("no memory for the screen lock");
            ok = false;
        }
    }
    for ( uint32_t i = 0U; ok && (TEXT_SLOTS > i); i++ )
    {
        if ( NULL == s_text_sem[i] )
        {
            s_text_sem[i] = khal_sem_create();
        }
        if ( NULL == s_text_sem[i] )
        {
            kode_err_set("no memory for the screen lock");
            ok = false;
        }
    }
    if ( ok && !kode_screen_ops.prepare() )
    {
        kode_err_set("out of memory");
        ok = false;
    }
    if ( ok )
    {
        ok = kode_standard_ao_screen_attach(&kode_screen_ops);
    }
    return ok;
}

/**
 * @brief The prologue of a public call: kode_enter(), then the screen attached.
 * @param name The public name of the call.
 * @return true when the call may go on; false with the error set.
 */
static bool screen_enter(const char *name)
{
    static khal_once_t once;  /* the one-shot the screen is attached by */
    bool               ok = kode_enter(name);

    if ( ok )
    {
        ok = khal_once_try(&once, &screen_start);
    }
    return ok;
}

/**
 * @brief Say that a text is longer than a widget takes.
 * @param what "text", "title", "number", "unit" or "label".
 * @param max  The bytes it may take, NUL included.
 */
static void too_long(const char *what, uint32_t max)
{
    const uint32_t most = max - 1U;

    kode_err_set("%s is too long (%" PRIu32 " bytes at most)", what, most);
}

/** @brief One try: true when the request was posted. */
/* cppcheck-suppress constParameterCallback ; the signature is the one kode_retry() takes */
static bool screen_send_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the send_t screen_send() passed */
    const send_t *const s = (const send_t *)ctx;
    KodeScreenReqEvt *const e = Q_NEW_X(KodeScreenReqEvt, KODE_SCREEN_POOL_MARGIN,
                                        KODE_SCREEN_REQ_SIG);
    bool posted = false;

    if ( NULL != e )
    {
        /* Whole arrays: a pool block holds whatever the last event left. */
        (void)memset(e->text, 0, sizeof(e->text));
        (void)memset(e->num, 0, sizeof(e->num));
        const size_t n = strlen(s->text);

        (void)memcpy(e->text, s->text, n);
        if ( NULL != s->unit )
        {
            (void)memcpy(&e->text[n + 1U], s->unit, strlen(s->unit));
        }
        for ( uint32_t i = 0U; KODE_SCREEN_REQ_NUMS > i; i++ )
        {
            e->num[i] = s->num[i];
        }
        e->op     = s->op;
        e->widget  = s->widget;
        posted   = kode_standard_ao_screen_post(&e->super);
    }
    return posted;
}

/**
 * @brief Post a request, waiting its turn; the caller holds s_screen_lock.
 * @param s What to post, already checked to fit.
 * @return true when posted; false with the error set.
 */
static bool screen_send(send_t *s)
{
    const bool posted = kode_retry(&screen_send_attempt, s, KODE_SCREEN_BUSY_MS);

    if ( !posted )
    {
        kode_err_set("the screen is busy");
    }
    return posted;
}

bool kode_screen_new(const char *title)
{
    bool        ok = screen_enter("kode_screen_new");
    const char *t  = "";

    if ( NULL != title )
    {
        t = title;
    }
    if ( ok && (strlen(t) >= KODE_WIDGET_TEXT_MAX) )
    {
        too_long("title", KODE_WIDGET_TEXT_MAX);
        ok = false;
    }
    if ( ok )
    {
        khal_mutex_lock(s_screen_lock);
        const uint32_t next = (s_current.gen + 1U) & KODE_WIDGET_GEN_MASK;
        send_t         s    = { KODE_SCREEN_OP_NEW, 0U, { (int32_t)next, 0, 0, 0 }, t, NULL };

        ok = screen_send(&s);
        if ( ok )
        {
            s_current.gen    = next;
            s_current.count  = 0U;
            s_current.used_h = 0U;
            s_current.titled = ('\0' != t[0]);
            kode_canvasbuf_screen_changed(next);
        }
        khal_mutex_unlock(s_screen_lock);
    }
    return ok;
}

/**
 * @brief Whether a button of the screen is called @p label, ignoring ASCII case.
 * @param label What the caller typed.
 * @return Its place, or KODE_WIDGETS_MAX for none; the caller holds s_screen_lock.
 */
static uint32_t screen_button_find(const char *label)
{
    uint32_t found = KODE_WIDGETS_MAX;

    for ( uint32_t i = 0U; (i < s_current.count) && (KODE_WIDGETS_MAX == found); i++ )
    {
        if ( ((uint8_t)KIND_BUTTON == s_current.kind[i]) && kode_name_is(label, s_current.label[i]) )
        {
            found = i;
        }
    }
    return found;
}

/**
 * @brief Keep a widget the Active Object has been sent as the screen's next one.
 * @param kind  KIND_*.
 * @param text  A button's label, else "".
 * @param h     What it takes in the body, px; 0 for one on the glass.
 * @return The widget; the caller holds s_screen_lock and has checked there is room.
 */
static kode_widget_t screen_widget_keep(uint8_t kind, const char *text, uint32_t h)
{
    const uint32_t place = s_current.count;

    if ( 0U < h )
    {
        if ( 0U < s_current.used_h )
        {
            s_current.used_h += KODE_SCREEN_GAP_H;
        }
        s_current.used_h += h;
    }

    s_current.kind[place] = kind;
    (void)memset(s_current.label[place], 0, sizeof(s_current.label[place]));
    if ( (uint8_t)KIND_BUTTON == kind )
    {
        for ( size_t i = 0U; '\0' != text[i]; i++ )
        {
            s_current.label[place][i] = kode_ascii_lower(text[i]);
        }
    }
    s_current.count++;
    return kode_widget_make(s_current.gen, place);
}

/**
 * @brief Whether the screen has room for a widget; the caller holds s_screen_lock.
 * @return true when it has; false with the error set.
 */
static bool screen_has_room(void)
{
    const bool room = (KODE_WIDGETS_MAX > s_current.count);

    if ( !room )
    {
        kode_err_set("a screen holds at most %" PRIu32 " widgets", (uint32_t)KODE_WIDGETS_MAX);
    }
    return room;
}

/**
 * @brief Add a widget under the others: the one path text, value and button take.
 * @param kind  KIND_*.
 * @param text  The widget's text, already checked to fit: a line, a number or a label.
 * @param unit  A value's unit, else NULL.
 * @param h     What it takes in the body, px; a text counts one line.
 * @return The widget; 0 with the error set.
 */
static kode_widget_t screen_widget_add(uint8_t kind, const char *text, const char *unit, uint32_t h)
{
    static const uint8_t ops[] = { KODE_SCREEN_OP_TEXT, KODE_SCREEN_OP_VALUE, KODE_SCREEN_OP_BUTTON };
    kode_widget_t         widget = 0U;

    khal_mutex_lock(s_screen_lock);
    if ( !screen_has_room() )
    {
        /* screen_has_room() said why */
    }
    else if ( ((uint8_t)KIND_BUTTON == kind) && (screen_button_find(text) != KODE_WIDGETS_MAX) )
    {
        kode_err_set("a button called '%s' is already on the screen", text);
    }
    else
    {
        const uint32_t place = s_current.count;
        send_t         s     = { ops[kind], place, { 0, 0, 0, 0 }, text, unit };

        /* The button's word is ready before the Active Object can hear of it. */
        atomic_store(&s_taps[place], (uint_least32_t)(s_current.gen << TAP_GEN_SHIFT));
        if ( screen_send(&s) )
        {
            widget = screen_widget_keep(kind, text, h);
        }
        else
        {
            /* screen_send() said why */
        }
    }
    khal_mutex_unlock(s_screen_lock);
    return widget;
}

kode_widget_t kode_screen_add_text(const char *text)
{
    kode_widget_t widget = 0U;
    bool       ok    = screen_enter("kode_screen_add_text");

    if ( ok && (NULL == text) )
    {
        kode_err_set("text is NULL");
        ok = false;
    }
    if ( ok && (strlen(text) >= KODE_WIDGET_TEXT_MAX) )
    {
        too_long("text", KODE_WIDGET_TEXT_MAX);
        ok = false;
    }
    if ( ok )
    {
        widget = screen_widget_add((uint8_t)KIND_TEXT, text, NULL, KODE_SCREEN_LINE_H);
    }
    return widget;
}

kode_widget_t kode_screen_add_value(const char *number, const char *unit)
{
    kode_widget_t  widget = 0U;
    bool        ok    = screen_enter("kode_screen_add_value");
    const char *u     = "";

    if ( NULL != unit )
    {
        u = unit;
    }
    if ( ok && (NULL == number) )
    {
        kode_err_set("number is NULL");
        ok = false;
    }
    if ( ok && (strlen(number) >= KODE_VALUE_NUMBER_MAX) )
    {
        too_long("number", KODE_VALUE_NUMBER_MAX);
        ok = false;
    }
    if ( ok && (strlen(u) >= KODE_VALUE_UNIT_MAX) )
    {
        too_long("unit", KODE_VALUE_UNIT_MAX);
        ok = false;
    }
    if ( ok )
    {
        uint32_t h = KODE_SCREEN_NUMBER_H;

        if ( '\0' != u[0] )
        {
            h += KODE_SCREEN_LINE_H;
        }
        widget = screen_widget_add((uint8_t)KIND_VALUE, number, u, h);
    }
    return widget;
}

kode_widget_t kode_screen_add_button(const char *label)
{
    kode_widget_t widget = 0U;
    bool       ok    = screen_enter("kode_screen_add_button");

    if ( ok && (NULL == label) )
    {
        kode_err_set("label is NULL");
        ok = false;
    }
    if ( ok && (strlen(label) >= KODE_BUTTON_LABEL_MAX) )
    {
        too_long("label", KODE_BUTTON_LABEL_MAX);
        ok = false;
    }
    if ( ok )
    {
        widget = screen_widget_add((uint8_t)KIND_BUTTON, label, NULL, KODE_SCREEN_BUTTON_H);
    }
    return widget;
}

/**
 * @brief Whether the screen holds room for one more widget of a kind; the caller holds s_screen_lock.
 * @param kind  KIND_CANVAS or KIND_IMAGE.
 * @param max   The most of them a screen holds.
 * @param plural What they are called in the error, such as "canvases".
 * @return true when it does; false with the error set.
 */
static bool screen_has_kind_room(uint8_t kind, uint32_t max, const char *plural)
{
    uint32_t n = 0U;

    for ( uint32_t i = 0U; i < s_current.count; i++ )
    {
        if ( s_current.kind[i] == kind )
        {
            n++;
        }
    }
    if ( n >= max )
    {
        kode_err_set("a screen holds at most %" PRIu32 " %s", max, plural);
    }
    return n < max;
}

/**
 * @brief Whether a picture of @p w by @p h is the glass or fits the body under the widgets.
 * @param w      Width.
 * @param h      Height.
 * @param body_h Out: the tallest widget the body still holds.
 * @return true when it does; the caller holds s_screen_lock.
 */
static bool size_fits(uint32_t w, uint32_t h, uint32_t *body_h)
{
    uint32_t whole = KODE_CANVAS_BODY_H_BARE;
    uint32_t taken = s_current.used_h;

    if ( s_current.titled )
    {
        whole = KODE_CANVAS_BODY_H_TITLE;
    }
    if ( 0U < taken )
    {
        taken += KODE_SCREEN_GAP_H;
    }
    *body_h = 0U;
    if ( taken < whole )
    {
        *body_h = whole - taken;
    }
    const bool glass = (KODE_CANVAS_WIDTH_MAX == w) && (KODE_CANVAS_HEIGHT_MAX == h);
    const bool fits  = (1U <= w) && (KODE_CANVAS_BODY_W >= w) && (1U <= h) && (h <= *body_h);

    return glass || fits;
}

/**
 * @brief What a picture that passed size_fits() takes in the body.
 * @param w Width.
 * @param h Height.
 * @return @p h; 0 for the glass, which floats over the card.
 */
static uint32_t size_taken(uint32_t w, uint32_t h)
{
    uint32_t taken = h;

    if ( (KODE_CANVAS_WIDTH_MAX == w) && (KODE_CANVAS_HEIGHT_MAX == h) )
    {
        taken = 0U;
    }
    return taken;
}

/**
 * @brief Whether a canvas of @p w by @p h is the glass or fits the screen's body.
 * @param w Width.
 * @param h Height.
 * @return true when it does; false with the error set. The caller holds s_screen_lock.
 */
static bool canvas_size_ok(uint32_t w, uint32_t h)
{
    uint32_t       body_h;
    const bool     ok = size_fits(w, h, &body_h);

    if ( !ok )
    {
        kode_err_set("a canvas is %" PRIu32 " x %" PRIu32 " (the glass) or up to %" PRIu32
                     " x %" PRIu32 " on this screen", (uint32_t)KODE_CANVAS_WIDTH_MAX,
                     (uint32_t)KODE_CANVAS_HEIGHT_MAX, (uint32_t)KODE_CANVAS_BODY_W, body_h);
    }
    return ok;
}

/**
 * @brief Open a canvas, post it and add it to the screen.
 *
 * The pixels are the screen's once the post goes; one whose post was refused
 * never reached it and goes back here.
 *
 * @param w Width, already checked.
 * @param h Height, already checked.
 * @return The canvas; 0 with the error set.
 */
static kode_canvas_t screen_canvas_add(uint32_t w, uint32_t h)
{
    kode_canvas_t canvas = 0U;

    khal_mutex_lock(s_screen_lock);
    if ( !canvas_size_ok(w, h) )
    {
        /* canvas_size_ok() said why */
    }
    else if ( screen_has_room()
             && screen_has_kind_room((uint8_t)KIND_CANVAS, KODE_CANVAS_MAX, "canvases") )
    {
        const uint32_t          place = s_current.count;
        kode_canvasbuf_t *const buf   = kode_canvasbuf_open(kode_widget_make(s_current.gen, place), w, h);

        if ( NULL == buf )
        {
            kode_err_set("no memory for a canvas of %" PRIu32 " x %" PRIu32 " pixels", w, h);
        }
        else
        {
            send_t s = { KODE_SCREEN_OP_CANVAS, place, { (int32_t)s_current.gen, 0, 0, 0 }, "", NULL };

            if ( screen_send(&s) )
            {
                canvas = screen_widget_keep((uint8_t)KIND_CANVAS, "", size_taken(w, h));
            }
            else
            {
                kode_canvasbuf_drop(buf);
            }
        }
    }
    else
    {
        /* screen_has_room() or screen_has_kind_room() said why */
    }
    khal_mutex_unlock(s_screen_lock);
    return canvas;
}

kode_canvas_t kode_screen_add_canvas(uint32_t width, uint32_t height)
{
    kode_canvas_t canvas = 0U;

    if ( screen_enter("kode_screen_add_canvas") )
    {
        canvas = screen_canvas_add(width, height);
    }
    return canvas;
}

kode_widget_t kode_screen_image_add(const char *path, kode_png_t *png)
{
    kode_widget_t image = 0U;
    uint32_t   body_h;

    khal_mutex_lock(s_screen_lock);
    if ( !size_fits(png->w, png->h, &body_h) )
    {
        kode_err_set("%s is %" PRIu32 " x %" PRIu32 ": an image is %" PRIu32 " x %" PRIu32
                     " (the glass) or up to %" PRIu32 " x %" PRIu32 " on this screen", path, png->w,
                     png->h, (uint32_t)KODE_CANVAS_WIDTH_MAX, (uint32_t)KODE_CANVAS_HEIGHT_MAX,
                     (uint32_t)KODE_CANVAS_BODY_W, body_h);
    }
    else if ( screen_has_room() && screen_has_kind_room((uint8_t)KIND_IMAGE, KODE_IMAGE_MAX, "images") )
    {
        const uint32_t place = s_current.count;
        const uint32_t taken = size_taken(png->w, png->h);

        kode_png_swap_rb(png);
        kode_canvasbuf_t *const buf = kode_canvasbuf_adopt(kode_widget_make(s_current.gen, place),
                                                           png->rgba, png->w, png->h);

        if ( NULL == buf )
        {
            kode_err_set("%s: out of memory", path);
        }
        else
        {
            send_t s = { KODE_SCREEN_OP_IMAGE, place, { (int32_t)s_current.gen, 0, 0, 0 }, "", NULL };

            /* The pixels are the screen's from here: a refused post gives them back with the buffer. */
            png->rgba = NULL;
            if ( screen_send(&s) )
            {
                image = screen_widget_keep((uint8_t)KIND_IMAGE, "", taken);
            }
            else
            {
                kode_canvasbuf_drop(buf);
            }
        }
    }
    else
    {
        /* screen_has_room() or screen_has_kind_room() said why */
    }
    khal_mutex_unlock(s_screen_lock);
    kode_png_free(png);
    return image;
}

kode_widget_t kode_screen_add_image(const char *path)
{
    kode_widget_t image = 0U;
    kode_png_t png   = { 0U, 0U, NULL };
    bool       ok    = screen_enter("kode_screen_add_image");

    if ( ok && (NULL == path) )
    {
        kode_err_set("path is NULL");
        ok = false;
    }
    /* The file is read and decoded before the screen's lock is taken. */
    ok = ok && kode_png_load("kode_screen_add_image", path, &png);
    if ( ok )
    {
        image = kode_screen_image_add(path, &png);
    }
    return image;
}

void kode_widget_text_done(uint32_t wait, bool drawn)
{
    const uint32_t slot = wait & TEXT_SLOT_MASK;

    if ( TEXT_SLOTS > slot )
    {
        uint32_t answer = (wait >> TEXT_SLOT_BITS) << TEXT_ANSWER_BITS;

        if ( drawn )
        {
            answer |= TEXT_ANSWER_DRAWN;
        }
        atomic_store(&s_text_done[slot], answer);
        khal_sem_signal(s_text_sem[slot]);
    }
}

/**
 * @brief One try to take a slot.
 * @param ctx A uint32_t that receives the slot.
 * @return true with the slot taken.
 */
static bool slot_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the uint32_t kode_draw_text() passed */
    uint32_t *const slot = (uint32_t *)ctx;
    bool            got  = false;

    for ( uint32_t i = 0U; (TEXT_SLOTS > i) && !got; i++ )
    {
        bool idle = false;

        got = atomic_compare_exchange_strong(&s_text_busy[i], &idle, true);
        if ( got )
        {
            *slot = i;
        }
    }
    return got;
}

/**
 * @brief Wait for the screen to answer a slot's text, up to KODE_SCREEN_BUSY_MS.
 *
 * A give left by an earlier text of the slot, one that timed out and was
 * answered late, wakes this wait too; the ticket tells it from this text's.
 *
 * @param slot   The slot.
 * @param ticket The text's ticket.
 * @param drawn  Out: the screen drew the text, when it answered.
 * @return true when the screen answered (drawn or dropped).
 */
static bool text_wait(uint32_t slot, uint32_t ticket, bool *drawn)
{
    const uint32_t start    = khal_time_ms();
    bool           answered = false;
    bool           alive    = true;

    while ( !answered && alive )
    {
        const uint32_t spent = khal_time_ms() - start;

        alive = (KODE_SCREEN_BUSY_MS > spent);
        if ( alive )
        {
            alive = khal_sem_wait_timeout(s_text_sem[slot], KODE_SCREEN_BUSY_MS - spent);
            if ( alive )
            {
                const uint32_t answer = atomic_load(&s_text_done[slot]);

                answered = ((answer >> TEXT_ANSWER_BITS) == ticket);
                *drawn   = ((answer & TEXT_ANSWER_DRAWN) != 0U);
            }
        }
    }
    return answered;
}

/**
 * @brief Post a line of text for a canvas, if the canvas is one of the screen.
 *
 * The screen's lock is held for the post alone, not for the wait.
 *
 * @param c     The caller's canvas.
 * @param x     Column of the text.
 * @param y     Row of its top.
 * @param text  The text, already checked to fit.
 * @param color The colour.
 * @param align Where x sits.
 * @param slot  The caller's slot.
 * @param ticket Out: the text's ticket, which the screen hands back with the slot.
 * @return true when posted; false with the error set.
 */
static bool text_send(kode_canvas_t c, int32_t x, int32_t y, const char *text, kode_color_t color,
                      kode_align_t align, uint32_t slot, uint32_t *ticket)
{
    const uint32_t place = c & KODE_WIDGET_PLACE_MASK;
    const uint32_t shade = (color & KODE_TEXT_RGB_MASK) | ((uint32_t)align << KODE_TEXT_ALIGN_SHIFT);
    bool           ok    = false;

    khal_mutex_lock(s_screen_lock);
    if ( (0U != c) && ((c >> KODE_WIDGET_PLACE_BITS) != s_current.gen) )
    {
        kode_err_set("this canvas belongs to an old screen");
    }
    else if ( (1U > place) || (place > s_current.count)
             || ((uint8_t)KIND_CANVAS != s_current.kind[place - 1U]) )
    {
        kode_err_set("no such canvas");
    }
    else
    {
        const uint32_t next = (s_current.text_ticket + 1U) & TEXT_TICKET_MASK;
        const uint32_t wait = (next << TEXT_SLOT_BITS) | slot;
        send_t         s    = { KODE_SCREEN_OP_DRAW_TEXT, c, { x, y, (int32_t)shade, (int32_t)wait }, text, NULL };

        ok = screen_send(&s);
        if ( ok )
        {
            s_current.text_ticket = next;
            *ticket            = next;
        }
    }
    khal_mutex_unlock(s_screen_lock);
    return ok;
}

bool kode_draw_text(kode_canvas_t c, int32_t x, int32_t y, const char *text, kode_color_t color,
                    kode_align_t align)
{
    uint32_t slot = 0U;
    bool     ok   = screen_enter("kode_draw_text");

    if ( ok && (NULL == text) )
    {
        kode_err_set("text is NULL");
        ok = false;
    }
    if ( ok && (strlen(text) >= KODE_WIDGET_TEXT_MAX) )
    {
        too_long("text", KODE_WIDGET_TEXT_MAX);
        ok = false;
    }
    ok = ok && kode_draw_coords_ok(x, y);
    if ( ok && (((int32_t)align < (int32_t)KODE_ALIGN_LEFT) || ((int32_t)align > (int32_t)KODE_ALIGN_RIGHT)) )
    {
        kode_err_set("unknown alignment");
        ok = false;
    }
    if ( ok && !kode_retry(&slot_attempt, &slot, KODE_SCREEN_BUSY_MS) )
    {
        kode_err_set("the screen is busy");
        ok = false;
    }
    if ( ok )
    {
        /* No lock is held from here to the end of the wait. */
        uint32_t ticket = 0U;
        bool     drawn  = false;

        ok = text_send(c, x, y, text, color, align, slot, &ticket);
        if ( ok && !text_wait(slot, ticket, &drawn) )
        {
            kode_err_set("the screen is busy");
            ok = false;
        }
        else if ( ok && !drawn )
        {
            kode_err_set("the text could not be drawn");
            ok = false;
        }
        else
        {
            /* drawn, or not posted: the error is set */
        }
        atomic_store(&s_text_busy[slot], false);
    }
    return ok;
}

/**
 * @brief Where widget @p p sits on the current screen; the caller holds s_screen_lock.
 * @param p The widget.
 * @return Its place, counted from 1; 0 with the error set when the screen has no such widget.
 */
static uint32_t widget_place(kode_widget_t p)
{
    uint32_t place = p & KODE_WIDGET_PLACE_MASK;

    if ( ((p >> KODE_WIDGET_PLACE_BITS) != s_current.gen) || (1U > place) || (place > s_current.count) )
    {
        kode_err_set("no such widget");
        place = 0U;
    }
    return place;
}

/**
 * @brief Change a widget's text if the screen has it and it can change.
 * @param p    The widget.
 * @param text The new text, already checked against a text's limit.
 * @return true when posted; false with the error set.
 */
static bool widget_set(kode_widget_t p, const char *text)
{
    bool ok;

    khal_mutex_lock(s_screen_lock);
    const uint32_t place = widget_place(p);

    ok = (0U != place);
    if ( !ok )
    {
        /* the error is set */
    }
    else if ( (uint8_t)KIND_BUTTON == s_current.kind[place - 1U] )
    {
        kode_err_set("a button's label cannot change");
        ok = false;
    }
    else if ( (uint8_t)KIND_CANVAS == s_current.kind[place - 1U] )
    {
        kode_err_set("a canvas has no text");
        ok = false;
    }
    else if ( (uint8_t)KIND_IMAGE == s_current.kind[place - 1U] )
    {
        kode_err_set("an image has no text");
        ok = false;
    }
    else if ( ((uint8_t)KIND_VALUE == s_current.kind[place - 1U])
             && (strlen(text) >= KODE_VALUE_NUMBER_MAX) )
    {
        too_long("number", KODE_VALUE_NUMBER_MAX);
        ok = false;
    }
    else
    {
        send_t s = { KODE_SCREEN_OP_SET, place - 1U, { 0, 0, 0, 0 }, text, NULL };

        ok = screen_send(&s);
    }
    khal_mutex_unlock(s_screen_lock);
    return ok;
}

bool kode_widget_set_text(kode_widget_t p, const char *fmt, ...)
{
    bool ok = screen_enter("kode_widget_set_text");
    char text[KODE_WIDGET_TEXT_MAX];

    if ( ok && (NULL == fmt) )
    {
        kode_err_set("format is NULL");
        ok = false;
    }
    if ( ok )
    {
        /* cppcheck-suppress misra-c2012-17.1 ; the list is handed straight to vsnprintf() */
        va_list ap;
        /* cppcheck-suppress misra-c2012-17.1 ; the same list */
        va_start(ap, fmt);
        const int32_t n = (int32_t)vsnprintf(text, sizeof(text), fmt, ap);
        /* cppcheck-suppress misra-c2012-17.1 ; the same list */
        va_end(ap);

        if ( 0 > n )
        {
            kode_err_set("the format could not be printed");
            ok = false;
        }
        else if ( (uint32_t)n >= KODE_WIDGET_TEXT_MAX )
        {
            too_long("text", KODE_WIDGET_TEXT_MAX);
            ok = false;
        }
        else
        {
            /* It fit whole. */
        }
    }
    if ( ok )
    {
        ok = widget_set(p, text);
    }
    return ok;
}

bool kode_widget_set_color(kode_widget_t p, kode_color_t color)
{
    bool ok = screen_enter("kode_widget_set_color");

    if ( ok )
    {
        khal_mutex_lock(s_screen_lock);
        const uint32_t place = widget_place(p);

        ok = (0U != place);
        if ( !ok )
        {
            /* the error is set */
        }
        else if ( (uint8_t)KIND_CANVAS == s_current.kind[place - 1U] )
        {
            kode_err_set("a canvas has no colour of its own");
            ok = false;
        }
        else if ( (uint8_t)KIND_IMAGE == s_current.kind[place - 1U] )
        {
            kode_err_set("an image has no colour of its own");
            ok = false;
        }
        else
        {
            send_t s = { KODE_SCREEN_OP_COLOR, place - 1U,
                         { (int32_t)(color & KODE_TEXT_RGB_MASK), 0, 0, 0 }, "", NULL };

            ok = screen_send(&s);
        }
        khal_mutex_unlock(s_screen_lock);
    }
    return ok;
}

void kode_widget_tapped(uint32_t gen, uint32_t place)
{
    if ( KODE_WIDGETS_MAX > place )
    {
        uint_least32_t word = atomic_load(&s_taps[place]);
        bool           done = false;

        while ( !done )
        {
            if ( (uint32_t)(word >> TAP_GEN_SHIFT) != (gen & KODE_WIDGET_GEN_MASK) )
            {
                done = true;    /* a tap for another screen */
            }
            else
            {
                done = atomic_compare_exchange_weak(&s_taps[place], &word, word | TAP_FLAG);
            }
        }
    }
}

bool kode_widget_was_pressed(kode_widget_t w)
{
    bool pressed = false;
    bool ok      = screen_enter("kode_widget_was_pressed");

    if ( ok )
    {
        khal_mutex_lock(s_screen_lock);
        const uint32_t place = widget_place(w);

        if ( 0U == place )
        {
            /* the error is set */
        }
        else if ( (uint8_t)KIND_BUTTON != s_current.kind[place - 1U] )
        {
            kode_err_set("this widget is not a button");
        }
        else
        {
            uint_least32_t word = atomic_load(&s_taps[place - 1U]);

            pressed = ((word & TAP_FLAG) != 0U);
            while ( pressed && !atomic_compare_exchange_weak(&s_taps[place - 1U], &word, word & ~(uint_least32_t)TAP_FLAG) )
            {
                pressed = ((word & TAP_FLAG) != 0U);
            }
        }
        khal_mutex_unlock(s_screen_lock);
    }
    return pressed;
}
