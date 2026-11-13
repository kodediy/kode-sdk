/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys.c
 * @brief kode_init(), the one place that knows the startup order, and the answer wait doors share.
 */
#include "kode_sys.h"
#include "kode_sys_priv.h"
#include "kode_banner.h"
#include "kode_gate.h"

#include "kaos.h"
#include "khal_exp.h"
#include "khal_pwm.h"
#include "khal_identity.h"
#include "khal_sync.h"
#include "khal_time.h"
#include "khal_keep.h"
#include "khal_system.h"
#include "sdkconfig.h"

#include <stdatomic.h>
#include <string.h>

khal_once_t kode_sys_init_once;

/** @brief Every row has been walked: see kode_up(). */
static atomic_bool s_kode_up;

/* The optional modules. An app names them weakly (KAOS_WEAK, kaos.h): one it
 * did not link resolves to NULL and its row is parked instead. kodeOS uses
 * every module, so in the OS role each start and door set-up is named as
 * kaos.h and kode_sys_priv.h declare it, and linked; only a module the build
 * compiled out stays weak there. The core - AO_Log, AO_Boot, AO_Power,
 * AO_IoExp and AO_Buttons - is linked by this file whatever the app does. */
#if !defined(CONFIG_KODEOS_BOOT_ROLE_OS)
KAOS_WEAK void kaos_storage_start(void);
KAOS_WEAK void kaos_ledmgr_start(void);
KAOS_WEAK void kaos_ir_start(void);
KAOS_WEAK void kaos_nfc_start(void);
KAOS_WEAK void kaos_rfid_start(void);
KAOS_WEAK void kaos_audio_start(void);
KAOS_WEAK void kaos_imu_start(void);
KAOS_WEAK void kaos_haptic_start(void);
KAOS_WEAK void kaos_touch_start(void);
KAOS_WEAK void kaos_display_start(void);
/* Called only once kaos_display_start() is linked: both live in one object. */
KAOS_WEAK bool kaos_display_settled(void);
KAOS_WEAK void kode_sys_ir_init(void);
KAOS_WEAK void kode_sys_nfc_init(void);
KAOS_WEAK void kode_sys_audio_init(void);
KAOS_WEAK void kode_sys_imu_init(void);
KAOS_WEAK void kode_sys_rfid_init(void);
KAOS_WEAK void kode_sys_files_init(void);
#endif
#if !(defined(CONFIG_KODEOS_BOOT_ROLE_OS) && defined(CONFIG_KODEOS_RADIO))
KAOS_WEAK void kaos_radio_start(void);
#endif
#if !(defined(CONFIG_KODEOS_BOOT_ROLE_OS) && defined(CONFIG_KODEOS_NET)) \
    && !defined(CONFIG_KODEOS_CHANNEL)
/* With the channel on, AO_Net is a use the build asked for, in either role. */
KAOS_WEAK void kaos_net_start(void);
#endif
#if !(defined(CONFIG_KODEOS_BOOT_ROLE_OS) && defined(CONFIG_KODEOS_APPMGR))
KAOS_WEAK void kaos_appmgr_start(void);
#endif
#if !(defined(CONFIG_KODEOS_BOOT_ROLE_OS) && defined(CONFIG_KODEOS_AUTOTEST))
KAOS_WEAK void kaos_autotest_start(void);
#endif
#if !defined(CONFIG_KODEOS_CONSOLE)
/* With the option on the console is a choice the build made, in either role:
 * it owns no device, and AO_Autotest comes with it. */
KAOS_WEAK void kaos_console_start(void);
#endif
#if !defined(CONFIG_KODE_LOWPOWER)
/* With the option on AO_LowPower is core, in either role: every owner joins
 * its rounds from its own initial transition, and the board sleeps on idle
 * time whether the app ever asks for a state or not. */
KAOS_WEAK void kaos_lowpower_start(void);
#endif
#if !defined(CONFIG_KODE_SCHEDULER)
/* With the option on AO_Scheduler is a choice the build made, in either role,
 * like the console: it owns no device, an app that turns it on gives up level
 * 16 for it, and its entries run by the minute with no call of the app's. */
KAOS_WEAK void kaos_scheduler_start(void);
#endif
#if !defined(CONFIG_KODE_TEST_DOT)
/* With the option on AO_TestDot is a choice the build made, in either role,
 * like the scheduler: it owns nothing until the cable wakes it, and an app
 * that turns it on gives up level 1 for it. */
KAOS_WEAK void kaos_testdot_start(void);
#endif

#define KODE_SYS_PANEL_WAIT_MS 800U   /**< The longest the rows wait for the panel's bring-up. */

/* What a firmware whose interface may start with the first rows defines. */
KAOS_WEAK void kode_app_early(void);

/** @brief The early row: the firmware's interface, when it has one that may start here. */
static void kode_sys_app_early(void)
{
    if ( kaos_linked(&kode_app_early) )
    {
        kode_app_early();
    }
    /* The rows below talk to the I2C bus. Not while the panel's reset line, which
     * is the touch controller's too, is moving. */
    if ( kaos_linked(&kaos_display_start) )
    {
        for ( uint32_t waited = 0U; (!kaos_display_settled()) && (KODE_SYS_PANEL_WAIT_MS > waited); waited += 5U )
        {
            khal_delay_ms(5U);
        }
    }
}

const kode_sys_row_t kode_sys_rows[KODE_SYS_ROWS] = {
    /* Second, after AO_Log: from here on the next boot is kodeOS again. */
    { .start = &kaos_boot_start,     .park = NULL },
    /* Third, so every owner below can lowpower_join() in its own initial
     * transition: a join before the first round is all a round asks of it. */
    { .start = &kaos_lowpower_start, .park = NULL },
    /* After AO_LowPower, which it joins, and before the console, so that the
     * first `cron` line finds a queue. It posts to nobody until
     * sched_system_up() says every queue exists. */
    { .start = &kaos_scheduler_start, .park = NULL },
    /* Before AO_AppMgr, which streams an image out of it, and before the
     * interface, which reads through it from its first step. */
    { .start = &kaos_storage_start,  .park = &kaos_park_storage },
    /* The first thing an interface asks for is the list. */
    { .start = &kaos_appmgr_start,   .park = &kaos_park_appmgr },
    /* The glass comes first: everything below these four happens with the
     * panel already on its way up, and none of it is waited for.
     * AO_IoExp before the buttons, the touch and the display, which use it
     * from their initial transitions, and before the parks, which ask it for
     * pins. */
    { .start = &kaos_ioexp_start,    .park = NULL },
    { .start = &kaos_buttons_start,  .park = NULL },
    { .start = &kaos_touch_start,    .park = &kaos_park_touch },
    { .start = &kaos_display_start,  .park = &kaos_park_display },
    /* The firmware's interface, if it asks to start here (kode_app_early()):
     * its first frame does not wait for the rows below. */
    { .start = &kode_sys_app_early,  .park = NULL },
    /* After AO_Storage: the link bring-up walks the card by posting to it.
     * AO_Audio names it (the amplifier is the C5's IO4) and so does AO_Ir (the
     * infrared block is behind the link), so sound and infrared keep it. */
    { .start = &kaos_radio_start,    .park = &kaos_park_c5 },
    /* After AO_Radio: its initial transition asks AO_Radio for the Wi-Fi state. */
    { .start = &kaos_net_start,      .park = NULL },
    { .start = &kaos_ledmgr_start,   .park = &kaos_park_led },
    /* The doors' semaphores and mutexes, created once (khal_sync has no
     * destructor) and before AO_Ir, which outranks every other AO. */
    { .start = &kode_sys_ir_init,    .park = NULL },
#if CONFIG_KODEOS_NFC
    { .start = &kode_sys_nfc_init,   .park = NULL },
#endif
    { .start = &kode_sys_audio_init, .park = NULL },
    { .start = &kode_sys_imu_init,   .park = NULL },
    { .start = &kode_sys_rfid_init,  .park = NULL },
    { .start = &kode_sys_files_init, .park = NULL },
    /* The IR, NFC, RFID and audio owners depend on nothing and nothing depends on them. */
    { .start = &kaos_ir_start,       .park = &kaos_park_ir },
#if CONFIG_KODEOS_NFC
    /* Off, the reader is the app's: neither started nor parked. */
    { .start = &kaos_nfc_start,      .park = &kaos_park_nfc },
#endif
    { .start = &kaos_rfid_start,     .park = &kaos_park_rfid },
    { .start = &kaos_audio_start,    .park = &kaos_park_audio },
    /* AO_Imu before AO_Haptic: the haptic row posts to it from its first step. */
    { .start = &kaos_imu_start,      .park = &kaos_park_imu },
    { .start = &kaos_haptic_start,   .park = &kaos_park_haptic },
    /* Before the console, its first client. */
    { .start = &kaos_autotest_start, .park = NULL },
    { .start = &kaos_console_start,  .park = NULL },
    /* After the console, whose verbs it adds to, and before AO_Power, which
     * it asks nothing of until the cable wakes it. */
    { .start = &kaos_testdot_start,  .park = NULL },
    { .start = &kaos_power_start,    .park = NULL },
};

void kode_sys_rows_walk(const kode_sys_row_t *rows, size_t n)
{
    /* The walk outranks the renderer the early row starts: every row comes
     * up at the pace of its own bring-up, on whichever core the renderer
     * landed. The boot task has its own priority back when it ends. */
    uint32_t const was = khal_task_priority_set(KODE_PRIO_BOOT_WALK);

    for ( size_t i = 0U; i < n; ++i )
    {
        if ( NULL != rows[i].start )
        {
            rows[i].start();
        }
    }
    for ( size_t i = 0U; i < n; ++i )
    {
        if ( (NULL == rows[i].start) && (NULL != rows[i].park) )
        {
            rows[i].park();
        }
    }
    (void)khal_task_priority_set(was);
}

/** @brief The bring-up itself; kode_init() runs it behind the one-shot. */
static void kode_bringup(void)
{
    kode_log_init();

#if CONFIG_KODE_LOWPOWER
    /* Before anything else is started or touched: a board that was woken
     * from OFF by anything but BOT goes back to sleep here, with its pins
     * still held and no Active Object running. A boot that goes on gets the
     * holds released. */
    kode_boot_gate();
#endif

    /* Before anything allocates: a refusal during bring-up is the one most worth a line. */
    khal_heap_watch();

    /* Out of the heap before the first big allocation, in every image. */
    khal_keep_init();

    /* The board's name is the C5's, from the cached MAC; a failure costs a name, not a boot. */
    (void)khal_identity_init();

    /* The LEDC's table, before the header's: khal_exp hands its channels back through it. */
    (void)khal_pwm_init();

    /* The expansion header's table, before anything can ask it anything. */
    (void)khal_exp_init();

    /* QF, the pools, the pub/sub bus and the tick that drives them. */
    kqp_init();

    /* AO_Log first: until it is klog's sink every line takes the rescue path. */
    kaos_log_start();

    /* Through AO_Log, so after it starts. */
    kode_banner_print();

    /* Every other module the app linked, in its place; then the parks. */
    kode_sys_rows_walk(kode_sys_rows, KODE_SYS_ROWS);

    /* Only now is there a queue for a keystroke to land in. */
    log_start_reader();

#if defined(CONFIG_KODE_SCHEDULER)
    /* Every AO above has its queue: the scheduler may post to them now. */
    sched_system_up();
#endif

    /* Returns: the scheduler was already running, so the AOs are away. */
    (void)QF_run();

    atomic_store(&s_kode_up, true);

    /* A kodeOS update counts how long it has been up from here. */
    boot_system_up();
}

bool kode_up(void)
{
    return atomic_load(&s_kode_up);
}

void kode_init(void)
{
    khal_once_run(&kode_sys_init_once, &kode_bringup);
}

bool kode_sys_text_fits(const char *text, size_t cap)
{
    if ( (NULL == text) || ('\0' == text[0]) )
    {
        return false;
    }
    return memchr(text, '\0', cap) != NULL;
}

/**
 * @brief Whether the generation an AO last stamped into @p ack is @p gen (an acquire load).
 * @param ack Where the answering Active Object stamps its generation.
 * @param gen The generation asked about.
 * @return true when @p ack holds @p gen.
 */
bool kode_sys_answer_is(const uint32_t *ack, uint32_t gen)
{
    /* cppcheck-suppress misra-c2012-17.3 ; GCC's __atomic builtins are the compiler's own, never declared in a header */
    return __atomic_load_n(ack, __ATOMIC_ACQUIRE) == gen;
}

/**
 * @brief Wait for the answer to generation @p gen, and for no other.
 *
 * An owner signals once per request, so a caller that gave up leaves an answer
 * owed that wakes a later caller on the same semaphore. Only the generation
 * stamped into @p ack decides; a foreign token resumes the wait for what is left.
 * @param sem The semaphore to sleep on.
 * @param ack Where the answering Active Object stamps the generation it answered.
 * @param gen This caller's own generation.
 * @param ms  How long to wait, in total, across every token discarded.
 * @return true when this caller's own answer arrived, and only then.
 */
bool kode_sys_answer_await(khal_sem_t sem, const uint32_t *ack, uint32_t gen,
                          uint32_t ms)
{
    uint32_t const start = khal_time_ms();
    uint32_t       left  = ms;
    bool           mine  = false;

    for ( ;; )
    {
        if ( !khal_sem_wait_timeout(sem, left) )
        {
            break;
        }
        if ( kode_sys_answer_is(ack, gen) )
        {
            mine = true;
            break;
        }
        /* Somebody else's. Unsigned, so the subtraction is right across the wrap. */
        uint32_t const spent = khal_time_ms() - start;
        if ( spent >= ms )
        {
            break;
        }
        left = ms - spent;
    }
    return mine;
}
