/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_setting.c
 * @brief kode_setting_set() and kode_setting_get(): the settings lock, the task's copy and the checks.
 */
#include "kode.h"

#include <inttypes.h>
#include <string.h>

#include "khal_once.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "kode_entry.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_task.h"

/** @brief What the error text shows for a name that is NULL. */
#define KODE_SETTING_NULL_SHOWN "(null)"

_Static_assert(KODE_SETTING_KEY_MAX == KODE_SYS_NVS_KEY_MAX,
               "kode.h's name limit and kode_sys.h's have drifted apart");

_Static_assert(KODE_SETTING_VALUE_MAX == KODE_SYS_NVS_VALUE_MAX,
               "kode.h's value limit and kode_sys.h's have drifted apart");

/** @brief What a task keeps for its settings. */
struct kode_setting_state {
    char value[KODE_SETTING_VALUE_MAX];  /**< The last kode_setting_get() text. */
};

/** @brief The lock every public settings call holds; made on the first call. */
static khal_mutex_t s_setting_lock;

/** @brief Make the settings lock. @return true when it exists. */
static bool setting_lock_make(void)
{
    s_setting_lock = khal_mutex_create();
    return NULL != s_setting_lock;
}

/**
 * @brief kode_enter(), then the settings turn; setting_leave() gives it back.
 * @param name The public name of the call.
 * @return true when the call has its turn; false with the error set.
 */
static bool setting_enter(const char *name)
{
    static khal_once_t lock_once;  /* the one-shot the lock is made by */
    bool               ok = kode_enter(name);

    if ( ok )
    {
        ok = khal_once_try(&lock_once, &setting_lock_make);
        if ( ok )
        {
            khal_mutex_lock(s_setting_lock);
        }
        else
        {
            kode_err_set("%s: no memory for the settings lock", name);
        }
    }
    return ok;
}

/** @brief Give the turn back, at the end of a call setting_enter() let in. */
static void setting_leave(void)
{
    khal_mutex_unlock(s_setting_lock);
}

/**
 * @brief The calling task's settings state, taken from PSRAM on first use.
 * @return The state; NULL if PSRAM has none to give.
 */
static struct kode_setting_state *setting_state_ensure(void)
{
    kode_task_t *const         task = kode_task_ensure();
    struct kode_setting_state *s    = NULL;

    if ( NULL != task )
    {
        if ( NULL == task->setting )
        {
            /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block is a kode_setting_state */
            s = (struct kode_setting_state *)khal_alloc_psram(sizeof(void *), sizeof(*s));
            if ( NULL != s )
            {
                (void)memset(s, 0, sizeof(*s));
                task->setting = s;
            }
        }
        s = task->setting;
    }
    return s;
}

/**
 * @brief Length of @p text, counted up to @p limit.
 * @param text  A NUL-terminated string.
 * @param limit The most to count.
 * @return Its length; @p limit if that long or longer.
 */
static uint32_t text_length(const char *text, uint32_t limit)
{
    uint32_t n = 0U;

    while ( (n < limit) && ('\0' != text[n]) )
    {
        n++;
    }
    return n;
}

/**
 * @brief Whether @p key is a name a setting can have; the error says why not.
 * @param key What the caller typed; may be NULL.
 * @return true for 1 to KODE_SETTING_KEY_MAX - 1 bytes.
 */
static bool key_ok(const char *key)
{
    bool ok = false;

    if ( NULL != key )
    {
        const uint32_t n = text_length(key, KODE_SETTING_KEY_MAX);

        ok = (0U < n) && (KODE_SETTING_KEY_MAX > n);
        if ( !ok )
        {
            kode_err_set("bad setting name '%s'", key);
        }
    }
    else
    {
        kode_err_set("bad setting name '%s'", KODE_SETTING_NULL_SHOWN);
    }
    return ok;
}

/**
 * @brief Whether this task may reach the store; the error says why not.
 * @return false on a task whose stack is in PSRAM.
 */
static bool stack_ok(void)
{
    const bool ok = !khal_task_stack_in_psram();

    if ( !ok )
    {
        kode_err_set("settings cannot be used from this task: its stack is in PSRAM; "
                     "use app_main or a task of your own");
    }
    return ok;
}

/**
 * @brief Whether @p value is a value a setting can hold; the error says why not.
 * @param key   The name, for the text.
 * @param value What the caller typed; may be NULL.
 * @return true for up to KODE_SETTING_VALUE_MAX - 1 bytes.
 */
static bool value_ok(const char *key, const char *value)
{
    bool ok = false;

    if ( NULL == value )
    {
        kode_err_set("setting '%s' has no value: use \"\" for an empty one", key);
    }
    else if ( text_length(value, KODE_SETTING_VALUE_MAX) >= KODE_SETTING_VALUE_MAX )
    {
        const uint32_t most = KODE_SETTING_VALUE_MAX - 1U;

        kode_err_set("setting '%s': a value is at most %" PRIu32 " characters", key, most);
    }
    else
    {
        ok = true;
    }
    return ok;
}

bool kode_setting_set(const char *key, const char *value)
{
    bool kept = false;

    if ( setting_enter("kode_setting_set") )
    {
        if ( key_ok(key) && value_ok(key, value) && stack_ok() )
        {
            kept = (kode_block_nvs_set(key, value) == KODE_OK);
            if ( !kept )
            {
                kode_err_set("setting '%s' could not be kept", key);
            }
        }
        setting_leave();
    }
    return kept;
}

const char *kode_setting_get(const char *key)
{
    const char *text = "";

    if ( setting_enter("kode_setting_get") )
    {
        if ( key_ok(key) && stack_ok() )
        {
            struct kode_setting_state *const s = setting_state_ensure();

            if ( NULL == s )
            {
                kode_err_set("setting '%s': no memory to read it", key);
            }
            else
            {
                const kode_status_t rc = kode_sys_nvs_get(key, s->value, sizeof(s->value));

                if ( KODE_OK == rc )
                {
                    text = s->value;
                }
                else if ( KODE_DENIED == rc )
                {
                    kode_err_set("setting '%s' is not set", key);
                }
                else
                {
                    kode_err_set("setting '%s' could not be read", key);
                }
            }
        }
        setting_leave();
    }
    return text;
}
