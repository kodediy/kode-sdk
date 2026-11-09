/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_haptic.c
 * @brief The standard layer's vibration: the hold, the built-in profiles and the app's own.
 */
#include "kode.h"

#include <inttypes.h>
#include <string.h>

#include "kdrv_aw86233.h"
#include "khal_once.h"
#include "khal_sync.h"
#include "kode_entry.h"
#include "kode_sys.h"

/** @brief The intensity of kode_haptic_vibrate(true), percent: the strongest. */
#define VIBE_HOLD_PCT 100U

/** @brief The most intensity a profile takes, percent. */
#define VIBE_INTENSITY_MAX 100U

/** @brief One vibration: on first, then off, on, ... */
typedef struct {
    char     name[KODE_HAPTIC_NAME_MAX];     /**< Lower case, NUL-terminated.  */
    uint16_t seg[KODE_HAPTIC_PATTERN_MAX];   /**< Segment lengths, ms.         */
    uint8_t  n;                              /**< Segments; 0 is a free slot.  */
    uint8_t  intensity;                      /**< Percent.                     */
} profile_t;

/** @brief What one try carries. */
typedef struct {
    profile_t     profile;  /**< The profile to play (unused by a hold).           */
    bool          held;     /**< A hold is on, whoever started it.                 */
    kode_status_t rc;       /**< The system call's last answer.                    */
} haptic_try_t;

/** @brief A built-in vibration. */
typedef struct {
    const char     *name;       /**< Lower case.                  */
    const uint16_t *seg;        /**< Segment lengths, ms.         */
    size_t          n;          /**< Segments.                    */
    uint8_t         intensity;  /**< Percent.                     */
} builtin_t;

_Static_assert(KODE_HAPTIC_PATTERN_MAX == KODE_SYS_HAPTIC_SEGMENTS_MAX,
               "the standard layer's segment ceiling is the facade's");

/** @brief "click": one short pulse. */
static const uint16_t s_click_seg[]   = { 12U };
/** @brief "tick": the lightest pulse. */
static const uint16_t s_tick_seg[]    = { 6U };
/** @brief "double": two pulses. */
static const uint16_t s_double_seg[]  = { 12U, 80U, 12U };
/** @brief "success": a short pulse, then a longer one. */
static const uint16_t s_success_seg[] = { 20U, 60U, 40U };
/** @brief "warning": three pulses. */
static const uint16_t s_warning_seg[] = { 40U, 80U, 40U, 80U, 40U };
/** @brief "error": two short pulses, then a long one. */
static const uint16_t s_error_seg[]   = { 60U, 60U, 60U, 60U, 120U };

/** @brief The built-ins. */
static const builtin_t s_builtin[] = {
    { "click",   s_click_seg,   sizeof(s_click_seg) / sizeof(s_click_seg[0]),     60U },
    { "tick",    s_tick_seg,    sizeof(s_tick_seg) / sizeof(s_tick_seg[0]),       40U },
    { "double",  s_double_seg,  sizeof(s_double_seg) / sizeof(s_double_seg[0]),   60U },
    { "success", s_success_seg, sizeof(s_success_seg) / sizeof(s_success_seg[0]), 60U },
    { "warning", s_warning_seg, sizeof(s_warning_seg) / sizeof(s_warning_seg[0]), 70U },
    { "error",   s_error_seg,   sizeof(s_error_seg) / sizeof(s_error_seg[0]),     80U }
};

/** @brief How many built-ins there are. */
#define VIBE_BUILTIN_COUNT (sizeof(s_builtin) / sizeof(s_builtin[0]))

/** @brief The app's profiles; guarded by s_haptic_lock. */
static profile_t s_app[KODE_HAPTIC_PROFILES_MAX];

/** @brief The lock; made on the first call. */
static khal_mutex_t s_haptic_lock;

/** @brief Make the lock. @return true when it exists. */
static bool haptic_lock_make(void)
{
    s_haptic_lock = khal_mutex_create();
    return NULL != s_haptic_lock;
}

/**
 * @brief The prologue of a public call: kode_enter(), then the lock exists.
 * @param name The public name of the call.
 * @return true when the call may go on; false with the error set.
 */
static bool haptic_enter(const char *name)
{
    static khal_once_t lock_once;  /* the one-shot the lock is made by */
    bool               ok = kode_enter(name);

    if ( ok )
    {
        ok = khal_once_try(&lock_once, &haptic_lock_make);
        if ( !ok )
        {
            kode_err_set("%s: no memory for the vibration lock", name);
        }
    }
    return ok;
}

/**
 * @brief Copy the profile called @p name into @p out.
 * @param name What the caller typed.
 * @param out  Out: the profile.
 * @return true when there is one; built-ins are looked at first.
 */
static bool profile_find(const char *name, profile_t *out)
{
    bool found = false;

    for ( size_t i = 0U; (VIBE_BUILTIN_COUNT > i) && !found; i++ )
    {
        if ( kode_name_is(name, s_builtin[i].name) )
        {
            (void)memset(out, 0, sizeof(*out));
            for ( size_t k = 0U; (k < s_builtin[i].n) && (KODE_HAPTIC_PATTERN_MAX > k); k++ )
            {
                out->seg[k] = s_builtin[i].seg[k];
                out->n++;
            }
            out->intensity = s_builtin[i].intensity;
            found = true;
        }
    }
    khal_mutex_lock(s_haptic_lock);
    for ( uint32_t i = 0U; (KODE_HAPTIC_PROFILES_MAX > i) && !found; i++ )
    {
        if ( (0U != s_app[i].n) && kode_name_is(name, s_app[i].name) )
        {
            *out  = s_app[i];
            found = true;
        }
    }
    khal_mutex_unlock(s_haptic_lock);
    return found;
}

/**
 * @brief The sys gain for an intensity: 0 to 100 spread over 0 to the LRA's rating.
 * @param intensity Percent, 0 to VIBE_INTENSITY_MAX.
 * @return Percent of full scale; the driver clamps at KDRV_AW_GAIN_MAX_PCT, so
 *         nothing above this is felt.
 */
static uint8_t drive_pct(uint8_t intensity)
{
    return (uint8_t)(((uint32_t)intensity * KDRV_AW_GAIN_MAX_PCT) / VIBE_INTENSITY_MAX);
}

/** @brief One try of a profile: true when posted or when the motor is held on. */
static bool play_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the haptic_try_t kode_haptic_play() passed */
    haptic_try_t *const t = (haptic_try_t *)ctx;

    t->held = kode_sys_haptic_is_holding();
    if ( !t->held )
    {
        t->rc = kode_sys_haptic_play_pattern(t->profile.seg, t->profile.n,
                                              drive_pct(t->profile.intensity));
    }
    return t->held || (KODE_OK == t->rc);
}

/** @brief One try of a hold: true when posted or when already held. */
static bool hold_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the haptic_try_t kode_haptic_vibrate() passed */
    haptic_try_t *const t = (haptic_try_t *)ctx;

    t->held = kode_sys_haptic_is_holding();
    if ( !t->held )
    {
        t->rc = kode_sys_haptic_hold(drive_pct(VIBE_HOLD_PCT));
    }
    return t->held || (KODE_OK == t->rc);
}

bool kode_haptic_vibrate(bool on)
{
    bool done = false;

    if ( haptic_enter("kode_haptic_vibrate") )
    {
        if ( on )
        {
            haptic_try_t t;

            (void)memset(&t, 0, sizeof(t));
            t.rc = KODE_ERROR;
            done = kode_retry(&hold_attempt, &t, KODE_HAPTIC_WAIT_MS);
            if ( !done )
            {
                kode_err_set("the motor is busy");
            }
        }
        else
        {
            kode_sys_haptic_stop();
            done = true;
        }
    }
    return done;
}

bool kode_haptic_play(const char *profile)
{
    bool played = false;

    if ( haptic_enter("kode_haptic_play") )
    {
        haptic_try_t t;

        (void)memset(&t, 0, sizeof(t));
        t.rc = KODE_ERROR;
        if ( NULL == profile )
        {
            kode_err_set("vibration name is NULL");
        }
        else if ( !profile_find(profile, &t.profile) )
        {
            kode_err_set("unknown vibration '%s'", profile);
        }
        else
        {
            played = kode_retry(&play_attempt, &t, KODE_HAPTIC_WAIT_MS);
            if ( t.held )
            {
                played = false;
                kode_err_set("the motor is held on; call kode_haptic_vibrate(false) first");
            }
            else if ( !played )
            {
                kode_err_set("the motor is busy");
            }
            else
            {
                /* Started. */
            }
        }
    }
    return played;
}

/**
 * @brief Whether @p name is fit to name a profile; the error is set if not.
 * @param name What the caller typed.
 * @return true when it is not NULL, empty, too long or a built-in's.
 */
static bool name_check(const char *name)
{
    bool ok = false;

    if ( NULL == name )
    {
        kode_err_set("profile name is NULL");
    }
    else
    {
        uint32_t len = 0U;

        while ( (KODE_HAPTIC_NAME_MAX > len) && ('\0' != name[len]) )
        {
            len++;
        }
        if ( 0U == len )
        {
            kode_err_set("profile name is empty");
        }
        else if ( KODE_HAPTIC_NAME_MAX <= len )
        {
            const uint32_t chars = KODE_HAPTIC_NAME_MAX - 1U;

            kode_err_set("profile name '%s' is too long (%" PRIu32 " characters at most)", name, chars);
        }
        else
        {
            ok = true;
            for ( size_t i = 0U; VIBE_BUILTIN_COUNT > i; i++ )
            {
                if ( kode_name_is(name, s_builtin[i].name) )
                {
                    kode_err_set("'%s' is a built-in vibration", name);
                    ok = false;
                }
            }
        }
    }
    return ok;
}

/**
 * @brief Whether the pattern is fit to store; the error is set if not.
 * @param pattern_ms The segments.
 * @param n          How many.
 * @param intensity  Percent.
 * @return true when the pattern is sound.
 */
static bool pattern_check(const uint16_t *pattern_ms, uint32_t n, uint8_t intensity)
{
    bool ok = false;

    if ( NULL == pattern_ms )
    {
        kode_err_set("pattern is NULL");
    }
    else if ( 0U == n )
    {
        kode_err_set("pattern has no segment");
    }
    else if ( KODE_HAPTIC_PATTERN_MAX < n )
    {
        const uint32_t limit = KODE_HAPTIC_PATTERN_MAX;

        kode_err_set("pattern has more than %" PRIu32 " segments", limit);
    }
    else if ( VIBE_INTENSITY_MAX < intensity )
    {
        const uint32_t limit = VIBE_INTENSITY_MAX;

        kode_err_set("intensity is over %" PRIu32, limit);
    }
    else
    {
        ok = true;
        for ( uint32_t i = 0U; i < n; i++ )
        {
            if ( 0U == pattern_ms[i] )
            {
                kode_err_set("a segment of 0 ms");
                ok = false;
            }
        }
    }
    return ok;
}

/**
 * @brief The slot for @p name: its own if it has one, else the first free; NULL if none.
 * @param name What the caller typed. The caller holds s_haptic_lock.
 * @return The slot, or NULL when the table is full.
 */
static profile_t *profile_slot_for(const char *name)
{
    profile_t *slot = NULL;
    profile_t *free_slot = NULL;

    for ( uint32_t i = 0U; (KODE_HAPTIC_PROFILES_MAX > i) && (NULL == slot); i++ )
    {
        if ( 0U == s_app[i].n )
        {
            if ( NULL == free_slot )
            {
                free_slot = &s_app[i];
            }
        }
        else if ( kode_name_is(name, s_app[i].name) )
        {
            slot = &s_app[i];
        }
        else
        {
            /* Another profile's. */
        }
    }
    if ( NULL == slot )
    {
        slot = free_slot;
    }
    return slot;
}

bool kode_haptic_add_profile(const char *name, const uint16_t *pattern_ms, uint32_t n, uint8_t intensity)
{
    bool added = false;

    if ( haptic_enter("kode_haptic_add_profile") && name_check(name)
        && pattern_check(pattern_ms, n, intensity) )
    {
        profile_t p;

        (void)memset(&p, 0, sizeof(p));
        p.n         = (uint8_t)n;
        p.intensity = intensity;
        for ( uint32_t i = 0U; '\0' != name[i]; i++ )
        {
            p.name[i] = kode_ascii_lower(name[i]);
        }
        for ( uint32_t s = 0U; s < n; s++ )
        {
            p.seg[s] = pattern_ms[s];
        }
        khal_mutex_lock(s_haptic_lock);
        profile_t *const slot = profile_slot_for(p.name);

        if ( NULL != slot )
        {
            *slot = p;
            added = true;
        }
        khal_mutex_unlock(s_haptic_lock);
        if ( !added )
        {
            const uint32_t limit = KODE_HAPTIC_PROFILES_MAX;

            kode_err_set("no room for another profile (%" PRIu32 " at most)", limit);
        }
    }
    return added;
}
