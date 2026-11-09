/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_pin.c
 * @brief The standard layer's expansion pins: write, read, analog and PWM by number.
 */
#include "kode.h"

#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>

#include "khal_once.h"
#include "khal_sync.h"
#include "kode_entry.h"
#include "kode_pin_lock.h"
#include "kode_sys.h"

/** @brief The highest duty a caller may ask for, in percent. */
#define PIN_DUTY_PCT_MAX 100U

/** @brief Tenths of a percent in one percent: the header's duty unit over the caller's. */
#define PIN_DUTY_X10_PER_PCT 10U

/** @brief The most listeners and waits one pin counts (s_listen_n's cell). */
#define PIN_LISTEN_N_MAX UINT8_MAX

_Static_assert(KODE_PIN_FIRST == KODE_SYS_EXP_POS_MIN, "the first pin is the header's");
_Static_assert(KODE_PIN_LAST == KODE_SYS_EXP_POS_MAX, "the last pin is the header's");
_Static_assert(KODE_PIN_PWM_HZ_MIN == KODE_SYS_EXP_PWM_HZ_MIN, "the lowest frequency is the header's");
_Static_assert(KODE_PIN_PWM_HZ_MAX == KODE_SYS_EXP_PWM_HZ_MAX, "the highest frequency is the header's");
_Static_assert((PIN_DUTY_PCT_MAX * PIN_DUTY_X10_PER_PCT) == KODE_SYS_EXP_PWM_DUTY_MAX,
               "100 percent is the header's full duty");

_Static_assert(KODE_PIN_COUNT <= (sizeof(unsigned) * (size_t)CHAR_BIT), "one bit per pin fits s_listened");

/** @brief The books' lock: every pin call, and a bus's begin and end inside its own lock; made on the first call. */
static khal_mutex_t s_pin_lock;

_Static_assert(KODE_LISTENERS_MAX < PIN_LISTEN_N_MAX, "every listener of a pin fits its count");

/** @brief Listeners and waits holding each pin, by pin less KODE_PIN_FIRST; under the lock. */
static uint8_t s_listen_n[KODE_PIN_COUNT];

/**
 * @brief One bit per listened pin (pin less KODE_PIN_FIRST): what
 *        kode_pin_is_listened() reads without the lock. Written under it.
 */
static atomic_uint s_listened;

/** @brief Make the pins lock. @return true when it exists. */
static bool pin_lock_make(void)
{
    s_pin_lock = khal_mutex_create();
    return NULL != s_pin_lock;
}

/**
 * @brief Take the pins lock, made on the first call.
 * @return true when the caller holds it; false when there is no memory to make it.
 */
static bool pin_lock_get(void)
{
    static khal_once_t lock_once;  /* the one-shot the lock is made by */
    const bool         ok = khal_once_try(&lock_once, &pin_lock_make);

    if ( ok )
    {
        khal_mutex_lock(s_pin_lock);
    }
    return ok;
}

bool kode_pin_lock_take(const char *name)
{
    bool ok = kode_enter(name);

    if ( ok )
    {
        ok = pin_lock_get();
        if ( !ok )
        {
            kode_err_set("%s: no memory for the pins lock", name);
        }
    }
    return ok;
}

void kode_pin_lock_give(void)
{
    khal_mutex_unlock(s_pin_lock);
}

/**
 * @brief Whether @p pin is on the connector; the error is set when it is not.
 * @param pin The pin as the caller typed it.
 * @return true when it is.
 */
static bool pin_valid(uint8_t pin)
{
    bool ok = (KODE_PIN_FIRST <= pin) && (KODE_PIN_LAST >= pin);

    if ( !ok )
    {
        kode_err_set("pin %" PRIu32 " does not exist: use %" PRIu32 " to %" PRIu32, (uint32_t)pin,
                     (uint32_t)KODE_PIN_FIRST, (uint32_t)KODE_PIN_LAST);
    }
    return ok;
}

/**
 * @brief Look at @p pin: it exists, the header is the app's and no module holds it.
 * @param pin  The pin as the caller typed it.
 * @param info Filled with what the pin is doing.
 * @return true when the pin may be used; false with the error set.
 */
static bool pin_look(uint8_t pin, kode_sys_exp_info_t *info)
{
    bool ok = pin_valid(pin);

    if ( ok )
    {
        kode_status_t const rc = kode_sys_exp_info(pin, info);

        if ( KODE_EXISTS == rc )
        {
            kode_err_set("the expansion header is held by something else in this image");
            ok = false;
        }
        else if ( KODE_OK != rc )
        {
            kode_err_set("pin %" PRIu32 " could not be looked up", (uint32_t)pin);
            ok = false;
        }
        else if ( KODE_SYS_EXP_LENT == info->mode )
        {
            const char *who = "a module";

            if ( NULL != info->tag )
            {
                who = info->tag;
            }
            kode_err_set("pin %" PRIu32 " is lent to %s", (uint32_t)pin, who);
            ok = false;
        }
        else
        {
            ok = true;
        }
    }
    return ok;
}

/** @brief The bit of @p pin in s_listened; @p pin is on the connector. */
static unsigned pin_bit(uint8_t pin)
{
    return 1U << (uint8_t)(pin - KODE_PIN_FIRST);
}

bool kode_pin_is_listened(uint8_t pin)
{
    bool listened = false;

    if ( (KODE_PIN_FIRST <= pin) && (KODE_PIN_LAST >= pin) )
    {
        listened = (atomic_load(&s_listened) & pin_bit(pin)) != 0U;
    }
    return listened;
}

/**
 * @brief pin_look(), and nothing listens to @p pin: the look of a call that
 *        would make the pin something other than an input.
 * @param pin  The pin as the caller typed it.
 * @param info Filled with what the pin is doing.
 * @return true when the pin may be changed; false with the error set.
 */
static bool pin_look_unlistened(uint8_t pin, kode_sys_exp_info_t *info)
{
    bool ok = pin_look(pin, info);

    if ( ok && kode_pin_is_listened(pin) )
    {
        kode_err_set("pin %" PRIu32 " is listened to", (uint32_t)pin);
        ok = false;
    }
    return ok;
}

bool kode_pin_ready_for_bus(const uint8_t *pins, size_t n)
{
    kode_sys_exp_info_t info;
    bool            ok = true;

    for ( size_t i = 0U; ok && (i < n); ++i )
    {
        ok = pin_valid(pins[i]);
        for ( size_t j = 0U; ok && (j < i); ++j )
        {
            if ( pins[j] == pins[i] )
            {
                kode_err_set("pin %" PRIu32 " is named twice: each line needs a pin of its own",
                             (uint32_t)pins[i]);
                ok = false;
            }
        }
    }
    /* Every pin is looked at before any is released: a refusal leaves them all as they were. */
    for ( size_t i = 0U; ok && (i < n); ++i )
    {
        ok = pin_look_unlistened(pins[i], &info);
    }
    for ( size_t i = 0U; ok && (i < n); ++i )
    {
        ok = pin_look(pins[i], &info);
        if ( ok && (KODE_SYS_EXP_FREE != info.mode) )
        {
            ok = kode_sys_exp_release(pins[i]) == KODE_OK;
            if ( !ok )
            {
                kode_err_set("pin %" PRIu32 " could not be released", (uint32_t)pins[i]);
            }
        }
    }
    return ok;
}

/**
 * @brief Drive @p pin, making it an output first when it is not one.
 * @param pin   The pin.
 * @param level The level.
 * @return true when it is driven; false with the error set.
 */
static bool pin_write_locked(uint8_t pin, bool level)
{
    kode_sys_exp_info_t info;
    bool            ok = pin_look_unlistened(pin, &info);

    if ( ok )
    {
        if ( KODE_SYS_EXP_OUT == info.mode )
        {
            ok = kode_sys_exp_write(pin, level) == KODE_OK;
            if ( !ok )
            {
                kode_err_set("pin %" PRIu32 " could not be driven", (uint32_t)pin);
            }
        }
        else
        {
            ok = kode_sys_exp_output(pin, level) == KODE_OK;
            if ( !ok )
            {
                kode_err_set("pin %" PRIu32 " could not be made an output", (uint32_t)pin);
            }
        }
    }
    return ok;
}

/**
 * @brief The level of @p pin, making it an input with no pull first when it is neither one.
 * @param pin The pin.
 * @return The level; false with the error set when the pin cannot be read.
 */
static bool pin_read_locked(uint8_t pin)
{
    kode_sys_exp_info_t info;
    bool            ok    = pin_look(pin, &info);
    bool            level = false;

    if ( ok && (KODE_SYS_EXP_IN != info.mode) && (KODE_SYS_EXP_OUT != info.mode) )
    {
        ok = kode_sys_exp_input(pin, KODE_SYS_GPIO_PULL_NONE) == KODE_OK;
        if ( !ok )
        {
            kode_err_set("pin %" PRIu32 " could not be made an input", (uint32_t)pin);
        }
    }
    if ( ok )
    {
        level = kode_sys_exp_level(pin);
    }
    return level;
}

/**
 * @brief The voltage on @p pin, making it an analog input first when it is not one.
 * @param pin The pin.
 * @return Millivolts; 0 with the error set when the pin cannot be read.
 */
static uint32_t pin_analog_locked(uint8_t pin)
{
    kode_sys_exp_info_t info;
    uint16_t        raw = 0U;
    int32_t         mv  = 0;
    bool            calibrated = false;
    uint32_t        out = 0U;

    if ( pin_look_unlistened(pin, &info) )
    {
        if ( !info.adc )
        {
            kode_err_set("pin %" PRIu32 " has no analog input", (uint32_t)pin);
        }
        else if ( (kode_sys_exp_adc(pin, &raw, &mv, &calibrated) != KODE_OK) || (0 > mv) )
        {
            kode_err_set("pin %" PRIu32 " could not be read", (uint32_t)pin);
        }
        else
        {
            out = (uint32_t)mv;
        }
    }
    return out;
}

/**
 * @brief Run @p pin as PWM at @p hz and @p duty_pct, opening a channel when it has none.
 * @param pin      The pin.
 * @param hz       The frequency, already judged.
 * @param duty_pct The duty, already judged.
 * @return true when it runs; false with the error set.
 */
static bool pin_pwm_locked(uint8_t pin, uint32_t hz, uint8_t duty_pct)
{
    kode_sys_exp_info_t info;
    bool            ok = pin_look_unlistened(pin, &info);

    if ( ok )
    {
        uint16_t const        duty_x10 = (uint16_t)((uint32_t)duty_pct * PIN_DUTY_X10_PER_PCT);
        kode_status_t const   rc       = kode_sys_exp_pwm(pin, hz, duty_x10);
        if ( KODE_NO_RESOURCES == rc )
        {
            kode_err_set("pin %" PRIu32 " has no PWM channel or timer free", (uint32_t)pin);
            ok = false;
        }
        else if ( KODE_OK != rc )
        {
            kode_err_set("pin %" PRIu32 " could not be made a PWM output", (uint32_t)pin);
            ok = false;
        }
        else
        {
            ok = true;
        }
    }
    return ok;
}

bool kode_pin_write(uint8_t pin, bool level)
{
    bool ok = kode_pin_lock_take("kode_pin_write");

    if ( ok )
    {
        ok = pin_write_locked(pin, level);
        kode_pin_lock_give();
    }
    return ok;
}

bool kode_pin_read(uint8_t pin)
{
    bool level = false;

    if ( kode_pin_lock_take("kode_pin_read") )
    {
        level = pin_read_locked(pin);
        kode_pin_lock_give();
    }
    return level;
}

uint32_t kode_pin_read_analog(uint8_t pin)
{
    uint32_t mv = 0U;

    if ( kode_pin_lock_take("kode_pin_read_analog") )
    {
        mv = pin_analog_locked(pin);
        kode_pin_lock_give();
    }
    return mv;
}

bool kode_pin_write_pwm(uint8_t pin, uint32_t hz, uint8_t duty_pct)
{
    bool ok = kode_pin_lock_take("kode_pin_write_pwm");

    if ( ok )
    {
        ok = pin_valid(pin);
        if ( ok && ((KODE_PIN_PWM_HZ_MIN > hz) || (KODE_PIN_PWM_HZ_MAX < hz)) )
        {
            kode_err_set("frequency %" PRIu32 " Hz is out of range: use %" PRIu32 " to %" PRIu32, hz,
                         (uint32_t)KODE_PIN_PWM_HZ_MIN, (uint32_t)KODE_PIN_PWM_HZ_MAX);
            ok = false;
        }
        if ( ok && (PIN_DUTY_PCT_MAX < duty_pct) )
        {
            kode_err_set("duty %" PRIu32 " %% is out of range: use 0 to %" PRIu32, (uint32_t)duty_pct,
                         (uint32_t)PIN_DUTY_PCT_MAX);
            ok = false;
        }
        if ( ok )
        {
            ok = pin_pwm_locked(pin, hz, duty_pct);
        }
        kode_pin_lock_give();
    }
    return ok;
}

/**
 * @brief The first listener of @p pin: judge it, make it an input with no
 *        pull, mark it listened and have @p arm attach its interrupt. Lock held.
 * @param pin The pin, on the connector.
 * @param arm What attaches the interrupt.
 * @return true when the pin is armed; false with the error set and the pin
 *         let go when it had been made an input.
 */
static bool pin_listen_first(uint8_t pin, kode_pin_arm_fn_t arm)
{
    kode_sys_exp_info_t info;
    bool                ok = pin_look(pin, &info);

    if ( ok && (kode_sys_exp_input(pin, KODE_SYS_GPIO_PULL_NONE) != KODE_OK) )
    {
        kode_err_set("pin %" PRIu32 " could not be made an input", (uint32_t)pin);
        ok = false;
    }
    else if ( ok )
    {
        /* Marked before the interrupt can fire: the Active Object takes an
         * edge only from a pin marked listened. */
        /* cppcheck-suppress misra-c2012-17.3 ; stdatomic.h's macro for GCC's __atomic builtin, the compiler's own, never declared in a header */
        (void)atomic_fetch_or(&s_listened, pin_bit(pin));
        ok = arm(pin);
        if ( !ok )
        {
            /* cppcheck-suppress misra-c2012-17.3 ; stdatomic.h's macro for GCC's __atomic builtin, the compiler's own, never declared in a header */
            (void)atomic_fetch_and(&s_listened, ~pin_bit(pin));
            (void)kode_sys_exp_release(pin);
        }
    }
    else
    {
        /* pin_look() said why */
    }
    return ok;
}

bool kode_pin_listen(uint8_t pin, kode_pin_arm_fn_t arm)
{
    bool ok = pin_valid(pin);

    if ( ok )
    {
        ok = pin_lock_get();
        if ( !ok )
        {
            kode_err_set("no memory for the pins lock");
        }
    }
    if ( ok )
    {
        uint8_t *const n = &s_listen_n[pin - KODE_PIN_FIRST];

        if ( PIN_LISTEN_N_MAX == *n )
        {
            kode_err_set("pin %" PRIu32 " is listened to by %" PRIu32 " already, the most it counts",
                         (uint32_t)pin, (uint32_t)PIN_LISTEN_N_MAX);
            ok = false;
        }
        else if ( 0U == *n )
        {
            ok = pin_listen_first(pin, arm);
        }
        else
        {
            /* listened to already: counted */
        }
        if ( ok )
        {
            (*n)++;
        }
        kode_pin_lock_give();
    }
    return ok;
}

void kode_pin_unlisten(uint8_t pin)
{
    bool held = false;

    if ( (KODE_PIN_FIRST <= pin) && (KODE_PIN_LAST >= pin) )
    {
        held = pin_lock_get();   /* it exists: a listen made it */
    }
    if ( held )
    {
        uint8_t *const n = &s_listen_n[pin - KODE_PIN_FIRST];

        if ( 0U < *n )
        {
            (*n)--;
            if ( 0U == *n )
            {
                /* Unmarked first: an edge the Active Object takes from now on
                 * arms nothing and re-arms nothing. */
                /* cppcheck-suppress misra-c2012-17.3 ; stdatomic.h's macro for GCC's __atomic builtin, the compiler's own, never declared in a header */
                (void)atomic_fetch_and(&s_listened, ~pin_bit(pin));
                (void)kode_sys_exp_release(pin);
            }
        }
        kode_pin_lock_give();
    }
}
