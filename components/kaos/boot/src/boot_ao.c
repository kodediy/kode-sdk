/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file boot_ao.c
 * @brief AO_Boot: the Active Object that owns otadata and the apps region's boot request.
 */
#include "kaos.h"
#include "kaos_start.h"

#include <stdio.h>
#include <string.h>

#include "kqp.h"
#include "kaos_claim.h"
#include "kdrv_clocks.h"
#include "kdrv_slot.h"
#include "kdrv_apps.h"
#include "kdrv_buttons.h"
#include "khal_nvs.h"
#include "khal_time.h"
#include "khal_system.h"
#if CONFIG_KODEOS_BOOT_ROLE_OS
#include "khal_identity.h"
#include "khal_nvs.h"
#include "kode_wire.h"
#endif
#include "kode_log.h"
#include "boot_zone.h"
#include "kset.h"
#include "sdkconfig.h"

#define TAG "kaos.boot"

/** @brief The tag of a zone being set: the zone's own record, not a line of the boot. */
#define ZONE_TAG "kaos.zone"

/** @brief The leave step that waits for AO_Net's answer. */
#define BOOT_LEAVE_STEP_NET     0U
/** @brief The leave step that waits for AO_Radio's answer. */
#define BOOT_LEAVE_STEP_RADIO   1U

_Static_assert(KSET_ZONE_MAX == KODE_CONFIG_VALUE_MAX,
               "a zone is one setting's value, the longest one");
_Static_assert(sizeof(KSET_ZONE_DEF) <= (KSET_ZONE_MAX + 1U),
               "the default zone fits where a zone is kept");

typedef struct {
    QActive  super;
    QTimeEvt leave;         /**< The bound of the leave step that is running. */
    uint8_t  leave_kind;    /**< kdrv_radio_note_t: the role the jump goes to. */
    uint8_t  leave_step;    /**< BOOT_LEAVE_STEP_NET or BOOT_LEAVE_STEP_RADIO. */
#if CONFIG_KODE_LOWPOWER
    uint8_t  lp_who;        /**< The id lowpower_join() gave.                 */
#endif
#if CONFIG_KODEOS_BOOT_ROLE_OS
    QTimeEvt reset;         /**< The budget of the reset step that is running. */
    QTimeEvt again;         /**< The pause before a refused wipe is asked anew. */
    QTimeEvt healthy;       /**< KODE_BOOT_CONFIRM_MS, then the rest of
                             *   KODE_BOOT_STABLE_MS: one timer, one after the
                             *   other.                                        */
    bool     stable_next;   /**< The healthy timer is on its second count.      */
    QActive *reset_who;     /**< Where the reset's answer goes.                 */
    uint32_t welcome_off;   /**< The welcome app's extent in the apps region.   */
    uint32_t welcome_size;  /**< Its length.                                    */
    uint8_t  reset_end;     /**< kboot_reset_end_t: how the reset ends.         */
    uint8_t  reset_step;    /**< kboot_reset_step_t: the step that is running.  */
    uint8_t  reset_passes;  /**< NVS namespaces erased so far.                  */
    bool     welcome_card;  /**< The find found the welcome app on the card
                             *   only: the install step comes after the link. */
    bool     welcome_written; /**< The install step has written the app; the
                               *   find that follows learns its extent.       */
    bool     link_up;       /**< The last RADIO_STATE_SIG said the link is up.  */
#endif
#if CONFIG_KODEOS_HOME_HOLD
    QTimeEvt home;          /**< The grace first, then the cap: one timer,
                             *   because only one of them is ever running. */
    bool     home_deferred; /**< An app asked for a moment (kode_sys_home_defer). */
    bool     home_capped;   /**< The extension is running; no second one.   */
#endif
    char     zone[KSET_ZONE_MAX + 1U]; /**< The zone in force.               */
    bool     zone_stored;   /**< `kset/zone` holds the zone in force.       */
} Boot;

static Boot l_boot;
QActive * const AO_Boot = &l_boot.super;

static kboot_reason_t  s_reason  = KBOOT_COLD;
static kboot_app_end_t s_app_end = KBOOT_APP_END_NONE;
static uint32_t        s_app_off = KODE_APP_OFFSET_NONE;

static QState Boot_initial(Boot * const me, void const * const par);
static QState Boot_active(Boot * const me, QEvt const * const e);
static QState Boot_leaving(Boot * const me, QEvt const * const e);
#if CONFIG_KODEOS_BOOT_ROLE_OS
static QState Boot_resetting(Boot * const me, QEvt const * const e);
static QState Boot_shipping(Boot * const me, QEvt const * const e);
static void boot_restart(void);
#endif

/* ---- Request helpers ----------------------------------------------------- */

kboot_reason_t boot_reason(void)
{
    return s_reason;
}

kboot_app_end_t boot_app_end(void)
{
    return s_app_end;
}

uint32_t boot_app_offset(void)
{
    return s_app_off;
}

kboot_app_end_t boot_app_end_of(kboot_reason_t r, int reason)
{
    kboot_app_end_t end;

    if ( KBOOT_FROM_APP != r )
    {
        end = KBOOT_APP_END_NONE;
    }
    else
    {
        switch ( reason )
        {
        case KDRV_RESET_PANIC:
        case KDRV_RESET_CPU_LOCKUP:
            end = KBOOT_APP_END_PANIC;
            break;
        case KDRV_RESET_INT_WDT:
        case KDRV_RESET_TASK_WDT:
        case KDRV_RESET_WDT:
            end = KBOOT_APP_END_WATCHDOG;
            break;
        case KDRV_RESET_BROWNOUT:
        case KDRV_RESET_PWR_GLITCH:
            end = KBOOT_APP_END_BROWNOUT;
            break;
        default:
            /* Every way of leaving (a power-on, a USB reset, a software
             * restart) and every unnamed reason, any number included: an
             * unknown reason must never read as a crash. */
            end = KBOOT_APP_END_CLEAN;
            break;
        }
    }
    return end;
}

uint32_t boot_crashes_next(uint32_t crashes, int reason, bool after_app)
{
    uint32_t next = crashes;

    if ( !after_app )
    {
        switch ( boot_app_end_of(KBOOT_FROM_APP, reason) )
        {
        case KBOOT_APP_END_PANIC:
        case KBOOT_APP_END_WATCHDOG:
            if ( KODE_BOOT_CRASHES_MAX > next )
            {
                ++next;
            }
            break;
        case KBOOT_APP_END_BROWNOUT:
            /* The cell's doing, not the image's: neither one more nor a way of
             * leaving. */
            break;
        default:
            next = 0U;
            break;
        }
    }
    return next;
}

const char *boot_app_end_name(kboot_app_end_t e)
{
    const char *name;

    switch ( e )
    {
    case KBOOT_APP_END_NONE:
        name = "none";
        break;
    case KBOOT_APP_END_CLEAN:
        name = "clean";
        break;
    case KBOOT_APP_END_PANIC:
        name = "panic";
        break;
    case KBOOT_APP_END_WATCHDOG:
        name = "watchdog";
        break;
    case KBOOT_APP_END_BROWNOUT:
        name = "brownout";
        break;
    default:
        name = "?";
        break;
    }
    return name;
}

void boot_os_switch_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(BOOT_OS_SWITCH_SIG);
    QACTIVE_POST(AO_Boot, &evt, NULL);
}

void boot_app_run_request(uint32_t offset, uint32_t size,
                          kboot_run_mode_t mode)
{
    BootRunEvt *e = Q_NEW_X(BootRunEvt, 0U, BOOT_APP_RUN_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; app run request dropped");
        return;
    }
    e->offset = offset;
    e->size   = size;
    e->mode   = mode;
    QACTIVE_POST(AO_Boot, &e->super, NULL);
}

void boot_info_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(BOOT_INFO_REQ_SIG);
    QACTIVE_POST(AO_Boot, &evt, NULL);
}

QEvt const *boot_exit_evt(void)
{
    static const QEvt evt = QEVT_INITIALIZER(BOOT_EXIT_REQ_SIG);
    return &evt;
}

void boot_radio_parked(void)
{
    static const QEvt evt = QEVT_INITIALIZER(BOOT_RADIO_PARKED_SIG);
    QACTIVE_POST(AO_Boot, &evt, NULL);
}

void boot_net_left(void)
{
    static const QEvt evt = QEVT_INITIALIZER(BOOT_NET_LEFT_SIG);
    QACTIVE_POST(AO_Boot, &evt, NULL);
}

bool boot_home_pending(void)
{
#if CONFIG_KODEOS_HOME_HOLD
    return (!l_boot.home_capped)
           && (QTimeEvt_getCtr(&l_boot.home) != 0U);
#else
    return false;
#endif
}

QEvt const *boot_home_defer_evt(void)
{
    static const QEvt evt = QEVT_INITIALIZER(BOOT_HOME_DEFER_SIG);
    return &evt;
}

QEvt const *boot_home_now_evt(void)
{
    static const QEvt evt = QEVT_INITIALIZER(BOOT_HOME_NOW_SIG);
    return &evt;
}

/* ---- What `boot info` prints --------------------------------------------- */

/** @brief Publish the boot state for `boot info`, and log the inactive kodeOS slot's image. */
static void boot_dump(void)
{
    const char *label   = "?";
    uint32_t    address = 0U;
    uint32_t    size    = 0U;
    kdrv_running_info(&label, &address, &size);

    /* The essentials go out as one event - the console closes `boot_info`
     * with them - so they are not also logged here. What stays in the log
     * is the detail the event does not carry: the inactive slot's image. */
    BootStateEvt *ev = Q_NEW_X(BootStateEvt, 0U, BOOT_STATE_SIG);
    if ( NULL != ev )
    {
        (void)snprintf(ev->running, sizeof(ev->running), "%s",
                       (NULL != label) ? label : "?");
        ev->address       = address;
        ev->size          = size;
        ev->reason        = (uint8_t)s_reason;
        ev->otadata_blank = kdrv_otadata_blank();
        ev->app_end       = (uint8_t)s_app_end;
        QACTIVE_PUBLISH(&ev->super, &l_boot.super);
    }

    if ( !kdrv_slot_present() )
    {
        KLOG_N(TAG, "kodeOS slot (inactive): absent from the partition table");
    }
    else
    {
        kdrv_slot_desc_t desc;
        if ( kdrv_slot_desc(&desc) == KODE_OK )
        {
            KLOG_N(TAG, "kodeOS slot (inactive): '%s' v%s (%s %s, IDF %s)",
                   desc.project, desc.version, desc.date, desc.time,
                   desc.idf_ver);
        }
        else
        {
            KLOG_N(TAG, "kodeOS slot (inactive): empty");
        }
        KLOG_N(TAG, "kodeOS slot (inactive): @0x%06lx (%luKB)",
               (unsigned long)kdrv_slot_address(),
               (unsigned long)(kdrv_slot_size() / 1024U));
    }

}

/* ---- The time zone ------------------------------------------------------- */

/**
 * @brief Put the clock in the zone the store keeps, or in UTC.
 *
 * Once, in the initial transition. A stored zone that does not read, or that
 * is no zone, leaves the default; a zone the C library has no memory for
 * leaves UTC too, and the line logged says which zone came of it.
 *
 * @param me AO_Boot.
 */
static void zone_load(Boot * const me)
{
    char stored[KSET_ZONE_MAX + 1U];

    me->zone_stored = false;
    (void)snprintf(me->zone, sizeof(me->zone), "%s", KSET_ZONE_DEF);
    if ( (khal_nvs_get_str(KSET_NAMESPACE, KSET_KEY_ZONE, stored, sizeof(stored))
         == KODE_OK)
        && boot_zone_valid(stored, sizeof(stored)) )
    {
        (void)snprintf(me->zone, sizeof(me->zone), "%s", stored);
        me->zone_stored = true;
    }
    if ( !khal_time_zone_set(me->zone) )
    {
        KLOG_W(TAG, "zone: no memory for %s; the clock runs in UTC", me->zone);
        (void)snprintf(me->zone, sizeof(me->zone), "%s", KSET_ZONE_DEF);
        me->zone_stored = false;
    }
    else if ( me->zone_stored )
    {
        KLOG_N(TAG, "zone: %s (stored)", me->zone);
    }
    else
    {
        KLOG_N(TAG, "zone: %s (none stored)", me->zone);
    }
}

/**
 * @brief Take @p zone: apply it, then store it, or change nothing.
 *
 * Applied first, so a zone the C library has no memory for is never stored;
 * stored second, so a zone the store refuses does not outlive this boot. On
 * either refusal the zone before it is put back.
 *
 * @param me   AO_Boot.
 * @param zone A zone boot_zone_valid() allowed.
 * @return KODE_OK when it is in force and stored; KODE_DENIED otherwise.
 */
static kode_status_t zone_take(Boot * const me, const char *zone)
{
    kode_status_t st = KODE_DENIED;

    if ( !khal_time_zone_set(zone) )
    {
        KLOG_W(ZONE_TAG, "zone: no memory for %s; staying in %s", zone, me->zone);
        (void)khal_time_zone_set(me->zone);
    }
    else if ( khal_nvs_set_str(KSET_NAMESPACE, KSET_KEY_ZONE, zone) != KODE_OK )
    {
        KLOG_W(ZONE_TAG, "zone: the store refused %s; staying in %s", zone, me->zone);
        (void)khal_time_zone_set(me->zone);
    }
    else
    {
        (void)snprintf(me->zone, sizeof(me->zone), "%s", zone);
        me->zone_stored = true;
        KLOG_N(ZONE_TAG, "zone: %s (stored)", me->zone);
        st = KODE_OK;
    }
    return st;
}

/**
 * @brief Answer `config key="zone"`, taking the zone first when asked.
 *
 * The answer is the zone in force, after a set as after a refusal.
 *
 * @param me AO_Boot.
 * @param r  The request.
 */
static void zone_config(Boot * const me, KodeConfigReqEvt const * const r)
{
    kode_status_t st = KODE_OK;

    if ( strcmp(r->key, KSET_KEY_ZONE) != 0 )
    {
        return;   /* another owner's key */
    }
    if ( r->set )
    {
        if ( boot_zone_valid(r->s, sizeof(r->s)) )
        {
            st = zone_take(me, r->s);
        }
        else
        {
            KLOG_D(TAG, "zone: refused - a zone is a POSIX TZ string of 1 to "
                        "%u characters; staying in %s",
                   (unsigned)KSET_ZONE_MAX, me->zone);
            st = KODE_DENIED;
        }
    }
    (void)kode_config_answer(&me->super, r, st, me->zone_stored, me->zone);
}

/* ---- The AO -------------------------------------------------------------- */

void Boot_ctor(void)
{
    QActive_ctor(&l_boot.super, Q_STATE_CAST(&Boot_initial));
    QTimeEvt_ctorX(&l_boot.leave, &l_boot.super, BOOT_LEAVE_TMO_SIG, 0U);
#if CONFIG_KODE_LOWPOWER
    l_boot.lp_who = KODE_LOWPOWER_WHO_NONE;
#endif
#if CONFIG_KODEOS_BOOT_ROLE_OS
    QTimeEvt_ctorX(&l_boot.reset, &l_boot.super, BOOT_RESET_TMO_SIG, 0U);
    QTimeEvt_ctorX(&l_boot.again, &l_boot.super, BOOT_RESET_AGAIN_SIG, 0U);
    QTimeEvt_ctorX(&l_boot.healthy, &l_boot.super, BOOT_HEALTHY_TMO_SIG, 0U);
    l_boot.stable_next = false;
#endif
#if CONFIG_KODEOS_HOME_HOLD
    QTimeEvt_ctorX(&l_boot.home, &l_boot.super, BOOT_HOME_GRACE_SIG, 0U);
#endif
}

AO_STORAGE(boot, KQP_BOOT_QUEUE_LEN, KQP_AO_STACK_MEDIUM);

void kaos_boot_start(void)
{
    Boot_ctor();
    AO_START(AO_Boot, boot, KODE_PRIO_BOOT);
#if CONFIG_KODEOS_CONSOLE
    /* `config key="zone"`, in both roles, registered once this AO has the
     * queue its requests are posted to. */
    static const kode_config_t BOOT_CONFIG[] = {
        { .name = KSET_KEY_ZONE, .type = KODE_TYPE_STRING },
    };

    kaos_require(KODE_CONFIG_ADD(AO_Boot, KQP_BOOT_CMD_MARGIN, BOOT_CONFIG), "boot settings");
#endif
}

void boot_first_frame(void)
{
#if CONFIG_KODEOS_BOOT_ROLE_OS
    static const QEvt frame = QEVT_INITIALIZER(BOOT_FIRST_FRAME_SIG);

    /* No room is no loss: the healthy timer confirms all the same. */
    (void)QACTIVE_POST_X(AO_Boot, &frame, 1U, NULL);
#endif
}

void boot_system_up(void)
{
#if CONFIG_KODEOS_BOOT_ROLE_OS
    /* Armed from here rather than posted: a post can find no room, and an update
     * that never counted would go back a slot at the next power-on. */
    QTimeEvt_armX(&l_boot.healthy, KQP_TICKS_PER_MS(KODE_BOOT_CONFIRM_MS), 0U);
#endif
}

#if CONFIG_KODEOS_BOOT_ROLE_OS
/* Every deliberate restart or power-off confirms first: any reset undoes an unconfirmed slot. */
/** @brief A kodeOS update that has not confirmed itself does so now. */
static void boot_confirm(void)
{
    if ( kdrv_slot_confirm_self() )
    {
        KLOG_N(TAG, "kodeOS update confirmed - this slot is now the one");
    }
}

/**
 * @brief Count this boot, and give the slot up at the limit.
 *
 * Runs with the boot's reason settled: an app's crash is not kodeOS's. A word
 * that is not this slot's belongs to a slot that died and left this one running,
 * so there is nothing here to count. Does not return when it gives up.
 *
 * @param after_app An app ran before this boot.
 */
static void boot_crashes_count(bool after_app)
{
    uint32_t crashes = 0U;

    if ( kdrv_slot_crashes(&crashes) )
    {
        crashes = boot_crashes_next(crashes, kdrv_reset_reason(), after_app);
    }
    else
    {
        crashes = 0U;
    }
    kdrv_slot_crashes_set(crashes);
    if ( 0U < crashes )
    {
        KLOG_W(TAG, "kodeOS crashed %lu time(s) in a row before it stayed up",
               (unsigned long)crashes);
    }
    if ( KODE_BOOT_CRASHES_MAX <= crashes )
    {
        if ( kdrv_running_is_factory() )
        {
            KLOG_E(TAG, "kodeOS keeps crashing and factory has nothing to go back to");
        }
        else if ( kdrv_slot_reject_self() == KODE_OK )
        {
            KLOG_E(TAG, "kodeOS keeps crashing; this slot is given up");
            boot_restart();
        }
        else
        {
            KLOG_E(TAG, "kodeOS keeps crashing and could not give this slot up");
        }
    }
}
#endif

/**
 * @brief Claim otadata, restore the clock and the zone; as kodeOS, learn why it booted.
 *
 * In an app it never writes otadata, so any reset returns to kodeOS.
 *
 * @param me  AO_Boot.
 * @param par Unused.
 * @return The transition to Boot_active.
 */
static QState Boot_initial(Boot * const me, void const * const par)
{
    (void)par;

    kaos_claim(KBSP_RES_OTADATA, me, "AO_Boot");
    kaos_claim(KBSP_RES_APPS_BOOTREQ, me, "AO_Boot");
#if CONFIG_KODE_LOWPOWER
    me->lp_who = lowpower_join(&me->super, "boot");
    if ( KODE_LOWPOWER_WHO_NONE != me->lp_who )
    {
        QActive_subscribe(&me->super, LOWPOWER_ROUND_SIG);
    }
#endif

    /* The hold has ONE owner in both roles: this AO hears it and publishes
     * HOME_SIG, and whoever draws the screen answers that. What differs is
     * only what AO_Boot does afterwards - in an app, reset. */
    QActive_subscribe(&me->super, BUTTON_HOLD_SIG);

#if CONFIG_KODEOS_HOME_HOLD
    me->home_deferred = false;
    me->home_capped   = false;
#endif

    /* The wall clock, before anything reads the hour or stamps a line: the
     * RTC's, or after a lost supply the last value kept, marked approximate
     * (kdrv_clocks). In both roles: an app reads the mark too. */
    if ( kdrv_clocks_wall_restore() != KODE_OK )
    {
        KLOG_I(TAG, "the clock is not set and nothing was kept");
    }

    /* The zone, with the clock: in both roles, before anything reads the
     * local hour or stamps a file. */
    zone_load(me);

#if !CONFIG_KODEOS_BOOT_ROLE_OS
    /* An app in the slot must NEVER touch otadata: the bootloader left this
     * slot PENDING_VERIFY and nothing confirms it, so the next boot is kodeOS
     * whatever this firmware does, a crash on its first line included. */
    KLOG_N(TAG, "running as an app; otadata is left alone, so any reset "
                "returns to kodeOS");
    return Q_TRAN(&Boot_active);
#else
#if CONFIG_KODE_LOWPOWER
    /* The only deep sleep kodeOS enters is OFF, and kode_init()'s gate lets a
     * wake from it go on only when BOT was down: this boot is that wake. */
    if ( kdrv_reset_reason() == (int)KDRV_RESET_DEEPSLEEP )
    {
        KLOG_N(TAG, "woke from OFF (BOT)");
    }
#endif
    /* The factory reset acts on the welcome app's write ending only while it
     * runs; the link's state is kept all the time, so a later reset does not
     * ask for it again. AO_Boot starts before AO_Radio, so no state is missed;
     * until one arrives the link reads as down. */
    me->reset_who       = NULL;
    me->reset_step      = (uint8_t)KBOOT_RESET_STEP_DONE;
    me->welcome_written = false;
    me->link_up         = false;
    bool after_app      = false;
    QActive_subscribe(&me->super, APP_FLASH_DONE_SIG);
    QActive_subscribe(&me->super, APP_FLASH_FAIL_SIG);
    QActive_subscribe(&me->super, RADIO_STATE_SIG);

    /* 1. Why did we boot? The bootloader marks the app request TRIED before
     *    jumping, so TRIED means "back from an app". The region is read here
     *    first: AO_Boot starts before AO_AppMgr, and kdrv_apps_req_tried()
     *    answers false without it. A failure must not block boot; the reason
     *    may then read cold. */
    if ( kdrv_apps_init() != KODE_OK )
    {
        KLOG_W(TAG, "apps region unreadable; boot reason may read cold");
    }
    s_app_off = KODE_APP_OFFSET_NONE;
    if ( kdrv_apps_req_tried(&s_app_off) )
    {
        s_reason = KBOOT_FROM_APP;
        after_app = true;
        kdrv_apps_req_clear();
        KLOG_N(TAG, "back from the user app @0x%06lx",
               (unsigned long)s_app_off);
    }
    else
    {
        s_reason = KBOOT_COLD;
        /* Present but not TRIED: the bootloader's retire write did not stick.
         * Cleared, or a later reset could jump into that stale app. An ARMED
         * one was launched, so its app ran; a NEW or ALWAYS one names nobody. */
        if ( kdrv_apps_req_present() )
        {
            after_app = kdrv_apps_req_armed(&s_app_off);
            kdrv_apps_req_clear();
            KLOG_W(TAG, "cleared a stale boot request (a retire write did not stick)");
        }
    }

    /* 2. Running from factory with otadata non-blank means the rescue took
     *    over: a kodeOS update died before confirming. Make it sticky. */
    if ( kdrv_running_is_factory() && !kdrv_otadata_blank() )
    {
        s_reason = KBOOT_UNEXPECTED;
        (void)kdrv_otadata_clear();
        KLOG_W(TAG, "a kodeOS update never confirmed; staying on factory");
    }

    /* 3. How the app before this boot ended, from the reason as it finally
     *    stands: a boot turned UNEXPECTED above is not back from an app. */
    s_app_end = boot_app_end_of(s_reason, kdrv_reset_reason());
    if ( KBOOT_APP_END_NONE != s_app_end )
    {
        KLOG_N(TAG, "how the user app ended: %s", boot_app_end_name(s_app_end));
    }

    /* 4. The crashes in a row, with who ran before this boot known. A slot that
     *    has crashed too often does not get past here. Confirming waits for the
     *    board to show it works. */
    boot_crashes_count(after_app);
    return Q_TRAN(&Boot_active);
#endif
}

/* ---- The leave ------------------------------------------------------------ */

/* The two neighbours the leave asks, named weakly (KAOS_WEAK, kaos.h): AO_Boot
 * is in every app, and naming them strongly would link AO_Net and AO_Radio
 * into every app with it. An app that did not link one has nothing to ask. */
KAOS_WEAK void kaos_net_start(void);
KAOS_WEAK void net_leave_request(void);
KAOS_WEAK void kaos_radio_start(void);
KAOS_WEAK void radio_park_request(kdrv_radio_note_t target);

/** @brief Let the console drain, then reset. Does not return on the board. */
static void boot_restart(void)
{
#if CONFIG_KODEOS_BOOT_ROLE_OS
    /* A restart on purpose has answered for this image, and the bootloader
     * cannot tell it from a crash. */
    boot_confirm();
#endif
    /* Whatever the reset turns out to be, the clock is on the store. */
    (void)kdrv_clocks_wall_keep(true);
    khal_delay_ms(KODE_RESTART_DRAIN_MS);
    khal_restart();
}

/**
 * @brief The leave is over: reset.
 *
 * @param me AO_Boot.
 */
static void leave_finish(Boot * const me)
{
    (void)QTimeEvt_disarm(&me->leave);
    if ( (uint8_t)KDRV_RADIO_NOTE_OS == me->leave_kind )
    {
        /* The next firmware is kodeOS, which takes the glass as it finds it. An
         * app built on another SDK might not: that road keeps the reset. */
        static const QEvt keep = QEVT_INITIALIZER(BOOT_KEEP_SIG);

        QACTIVE_PUBLISH(&keep, &me->super);
    }
    boot_restart();
}

/** @brief A restart request arrived while one is leaving: the first wins. */
static void leave_drop(void)
{
    KLOG_N(TAG, "already leaving; this restart request is dropped");
}

/**
 * @brief Second step of the leave: AO_Radio hangs BLE up and parks the C5.
 *
 * An app without AO_Radio has nothing to wait for.
 *
 * @param me AO_Boot.
 */
static void leave_radio_step(Boot * const me)
{
    me->leave_step = BOOT_LEAVE_STEP_RADIO;
    if ( kaos_linked(&kaos_radio_start) )
    {
        radio_park_request((kdrv_radio_note_t)me->leave_kind);
        QTimeEvt_armX(&me->leave, KQP_TICKS_PER_MS(KODE_LEAVE_RADIO_MS), 0U);
    }
    else
    {
        leave_finish(me);
    }
}

/**
 * @brief Start the leave that ends in a reset.
 *
 * The caller has done whatever must be on flash first. The first step is
 * AO_Net's clean stop of the Kode channel; an app without AO_Net goes on to
 * the second at once. The caller then transitions to Boot_leaving.
 *
 * @param me   AO_Boot.
 * @param kind The role of the firmware the jump goes to.
 */
static void leave_begin(Boot * const me, kdrv_radio_note_t kind)
{
    me->leave_kind = (uint8_t)kind;
    me->leave_step = BOOT_LEAVE_STEP_NET;
    if ( kaos_linked(&kaos_net_start) )
    {
        net_leave_request();
        QTimeEvt_armX(&me->leave, KQP_TICKS_PER_MS(KODE_LEAVE_NET_MS), 0U);
    }
    else
    {
        leave_radio_step(me);
    }
}

#if CONFIG_KODEOS_BOOT_ROLE_OS
/* ---- The factory reset ----------------------------------------------------- */

/** @brief The file the install step writes: the welcome app's copy on the card. */
#define BOOT_WELCOME_FILE  KODE_CARD_APP_PREFIX CONFIG_KODEOS_WELCOME_APP \
                           KODE_CARD_APP_SUFFIX

_Static_assert(sizeof(CONFIG_KODEOS_WELCOME_APP) <= KODE_APP_PROJECT_MAX,
               "the welcome app's name must fit a project name");
_Static_assert(sizeof(BOOT_WELCOME_FILE) <= KODE_FLASH_SRC_MAX,
               "the welcome app's file must fit a flash request");

const char *boot_reset_step_name(uint8_t step)
{
    const char *name;

    switch ( step )
    {
    case (uint8_t)KBOOT_RESET_STEP_FIND:
        name = "find";
        break;
    case (uint8_t)KBOOT_RESET_STEP_INSTALL:
        name = "install";
        break;
    case (uint8_t)KBOOT_RESET_STEP_APPS:
        name = "apps";
        break;
    case (uint8_t)KBOOT_RESET_STEP_LINK:
        name = "link";
        break;
    case (uint8_t)KBOOT_RESET_STEP_WIPE:
        name = "wipe";
        break;
    case (uint8_t)KBOOT_RESET_STEP_NVS:
        name = "nvs";
        break;
    case (uint8_t)KBOOT_RESET_STEP_REQUEST:
        name = "request";
        break;
    case (uint8_t)KBOOT_RESET_STEP_DONE:
        name = "done";
        break;
    default:
        name = "?";
        break;
    }
    return name;
}

bool boot_factory_reset_request(kboot_reset_end_t end, QActive *who)
{
    FactoryResetReqEvt *e;

    if ( (NULL == who) || ((KBOOT_RESET_RESTART != end)
                          && (KBOOT_RESET_SHIP != end)) )
    {
        return false;
    }
    e = Q_NEW_X(FactoryResetReqEvt, 0U, FACTORY_RESET_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_W(TAG, "out of events; factory reset request dropped");
        return false;
    }
    e->end = (uint8_t)end;
    e->who = who;
    if ( !QACTIVE_POST_X(AO_Boot, &e->super, 0U, NULL) )
    {
        KLOG_W(TAG, "AO_Boot's queue is full; factory reset request dropped");
        return false;
    }
    return true;
}

/**
 * @brief Answer a factory reset.
 *
 * @param who    Where the answer goes.
 * @param step   The step that stopped the reset, or KBOOT_RESET_STEP_DONE.
 * @param status A kode_status_t.
 */
static void reset_reply(QActive *who, uint8_t step, kode_status_t status)
{
    FactoryResetRspEvt *e = Q_NEW_X(FactoryResetRspEvt, 0U,
                                    FACTORY_RESET_RSP_SIG);

    if ( (NULL == who) || (NULL == e) )
    {
        if ( NULL != e )
        {
            QF_gc(&e->super);
        }
        KLOG_E(TAG, "a factory reset's answer could not be sent");
        return;
    }
    e->step   = step;
    e->status = (uint8_t)status;
    if ( !QACTIVE_POST_X(who, &e->super, 0U, AO_Boot) )
    {
        KLOG_E(TAG, "the factory reset's asker's queue is full; its answer "
                    "is lost");
    }
}

/**
 * @brief Refuse what a running reset (or its wait) may not be interrupted by.
 *
 * A half-erased board must not restart, switch kodeOS slot or start a second
 * reset. A second reset is answered KODE_NO_RESOURCES; everything else is
 * dropped. The bottom button's hold is dropped too (HOME must not leave the
 * reset's screen), logged at debug level as it is an ordinary press.
 *
 * @param e The event.
 * @return true when @p e was one of those and has been dealt with.
 */
static bool reset_refuses(QEvt const * const e)
{
    bool refused = true;

    switch ( e->sig )
    {
    case FACTORY_RESET_REQ_SIG:
        KLOG_N(TAG, "busy with a factory reset; this one is refused");
        reset_reply(Q_EVT_CAST(FactoryResetReqEvt)->who,
                    (uint8_t)KBOOT_RESET_STEP_FIND, KODE_NO_RESOURCES);
        break;
    case BOOT_OS_SWITCH_SIG:
    case BOOT_APP_RUN_SIG:
    case BOOT_EXIT_REQ_SIG:
    case BOOT_HOME_NOW_SIG:
    case BOOT_HOME_DEFER_SIG:
    case BOOT_HOME_GRACE_SIG:
        KLOG_N(TAG, "busy with a factory reset; this request is dropped");
        break;
    case BUTTON_HOLD_SIG:
        KLOG_D(TAG, "busy with a factory reset; this button hold is dropped");
        break;
    default:
        refused = false;
        break;
    }
    return refused;
}

/**
 * @brief Arm the budget of the step that is running, in place of the last.
 *
 * @param me AO_Boot.
 * @param ms The budget, in milliseconds.
 */
static void reset_arm(Boot * const me, uint32_t ms)
{
    (void)QTimeEvt_disarm(&me->reset);
    QTimeEvt_armX(&me->reset, KQP_TICKS_PER_MS(ms), 0U);
}

/**
 * @brief Start @p step and arm its budget.
 *
 * The step is set before anything is asked of its owner, so a request that
 * cannot be posted is reported at the step it belongs to.
 *
 * @param me   AO_Boot.
 * @param step A kboot_reset_step_t.
 * @param ms   The step's budget, in milliseconds.
 */
static void reset_enter(Boot * const me, kboot_reset_step_t step, uint32_t ms)
{
    me->reset_step = (uint8_t)step;
    reset_arm(me, ms);
    KLOG_N(TAG, "factory reset: %s", boot_reset_step_name((uint8_t)step));
}

/**
 * @brief Whether the factory reset keeps @p ns.
 *
 * @param ns An NVS namespace name.
 * @return true for the identity cache and the station lock.
 */
static bool reset_ns_kept(const char *ns)
{
    return (strcmp(ns, KHAL_IDENTITY_NVS_NS) == 0)
           || (strcmp(ns, KODE_STATION_NVS_NS) == 0);
}

/**
 * @brief The NVS step: erase one namespace, or finish.
 *
 * Walks the store from index 0 every time: an erase renumbers what follows.
 * Only kept namespaces left ends the step; one still there after
 * KODE_RESET_NS_MAX erases will not go. KODE_DENIED from the listing is the
 * end of the store (AO_Boot's stack is internal, not PSRAM).
 *
 * @param me AO_Boot.
 * @return KODE_OK to go on (the step is KBOOT_RESET_STEP_DONE when nothing
 *         is left), else the status that stops the reset.
 */
static kode_status_t reset_nvs_pass(Boot * const me)
{
    static const QEvt next = QEVT_INITIALIZER(BOOT_RESET_STEP_SIG);
    char          ns[KHAL_NVS_NS_SIZE];
    kode_status_t st    = KODE_OK;
    bool          found = false;
    bool          more  = true;
    uint16_t      idx   = 0U;

    while ( more )
    {
        kode_status_t const q = khal_nvs_namespace(idx, ns, sizeof(ns));
        if ( KODE_DENIED == q )
        {
            more = false;
        }
        else if ( KODE_OK != q )
        {
            st   = q;
            more = false;
        }
        else if ( reset_ns_kept(ns) )
        {
            ++idx;
        }
        else
        {
            found = true;
            more  = false;
        }
    }
    if ( KODE_OK != st )
    {
        return st;
    }
    if ( !found )
    {
        me->reset_step = (uint8_t)KBOOT_RESET_STEP_DONE;
        return KODE_OK;
    }
    if ( KODE_RESET_NS_MAX <= me->reset_passes )
    {
        KLOG_W(TAG, "factory reset: '%s' is still there after %u erases", ns,
               (unsigned)me->reset_passes);
        return KODE_ERROR;
    }
    st = khal_nvs_erase_namespace(ns);
    if ( KODE_OK != st )
    {
        KLOG_W(TAG, "factory reset: '%s' would not erase", ns);
        return st;
    }
    ++me->reset_passes;
    KLOG_I(TAG, "factory reset: erased '%s'", ns);
    QACTIVE_POST(&me->super, &next, me);
    return KODE_OK;
}

/** @brief The NVS step starts: nothing but AO_Boot is waited for, so no budget. */
static kode_status_t reset_start_nvs(Boot * const me)
{
    (void)QTimeEvt_disarm(&me->reset);
    me->reset_step   = (uint8_t)KBOOT_RESET_STEP_NVS;
    me->reset_passes = 0U;
    KLOG_N(TAG, "factory reset: %s", boot_reset_step_name(me->reset_step));
    return reset_nvs_pass(me);
}

/** @brief The apps step starts: AppMgr removes every app but the welcome app. */
static kode_status_t reset_start_apps(Boot * const me)
{
    /* No extent: there is no welcome app to keep, and nothing stays. */
    const char * const keep = (0U != me->welcome_size) ? CONFIG_KODEOS_WELCOME_APP : "";

    reset_enter(me, KBOOT_RESET_STEP_APPS, KODE_RESET_APPS_MS);
    if ( !appmgr_keep_only_request(keep, &me->super) )
    {
        KLOG_W(TAG, "factory reset: AO_AppMgr was not asked to remove the "
                    "other apps");
        return KODE_ERROR;
    }
    return KODE_OK;
}

/** @brief The wipe step starts: AO_Radio forgets the secrets. */
static kode_status_t reset_start_wipe(Boot * const me)
{
#if CONFIG_KODEOS_RADIO
    reset_enter(me, KBOOT_RESET_STEP_WIPE, KODE_RESET_WIPE_MS);
    if ( !c5_wipe_request(&me->super) )
    {
        KLOG_W(TAG, "factory reset: out of events; the wipe was not asked");
        return KODE_ERROR;
    }
    return KODE_OK;
#else
    return reset_start_apps(me);  /* no C5 to ask, no secrets of its own */
#endif
}

/** @brief The link is still off during the link step: the request is made again. */
static void reset_link_ask_again(void)
{
#if CONFIG_KODEOS_RADIO
    KLOG_N(TAG, "factory reset: the link is off; asking for it again");
    radio_up_request();
#endif
}

/** @brief Ask where the welcome app is (the first find, or the one after the install). */
static kode_status_t reset_ask_find(Boot * const me)
{
    if ( !appmgr_find_request(CONFIG_KODEOS_WELCOME_APP, &me->super) )
    {
        KLOG_W(TAG, "factory reset: AO_AppMgr was not asked for the welcome "
                    "app");
        return KODE_ERROR;
    }
    return KODE_OK;
}

/** @brief The install step starts: the welcome app is written from the card. */
static kode_status_t reset_start_install(Boot * const me)
{
    reset_enter(me, KBOOT_RESET_STEP_INSTALL, KODE_RESET_INSTALL_MS);
    me->welcome_written = false;
    appmgr_flash_request(BOOT_WELCOME_FILE, false);
    return KODE_OK;
}

/**
 * @brief The link is up: the install, when the card has the app, then the wipe.
 *
 * A write that makes room for the welcome app can cost an older app, so it
 * comes after the one check that stops most resets.
 *
 * @param me AO_Boot.
 * @return KODE_OK to go on, else the status that stops the reset.
 */
static kode_status_t reset_after_link(Boot * const me)
{
    return me->welcome_card ? reset_start_install(me) : reset_start_wipe(me);
}

/** @brief The link step starts: the wipe needs it, so it is brought up if down. */
static kode_status_t reset_start_link(Boot * const me)
{
#if CONFIG_KODEOS_RADIO
    if ( me->link_up )
    {
        return reset_after_link(me);
    }
    reset_enter(me, KBOOT_RESET_STEP_LINK, KODE_RESET_LINK_MS);
    radio_up_request();
    return KODE_OK;
#else
    return reset_after_link(me);
#endif
}

/**
 * @brief The install step did not leave a welcome app.
 *
 * A person's reset goes on with no app to keep; the ship end does not leave
 * the factory without its first-run app, and nothing is erased yet.
 *
 * @param me AO_Boot.
 * @return KODE_OK to go on, else the status that stops the reset.
 */
static kode_status_t reset_install_failed(Boot * const me)
{
    kode_status_t st = KODE_ERROR;

    if ( (uint8_t)KBOOT_RESET_RESTART == me->reset_end )
    {
        KLOG_W(TAG, "factory reset: going on with no welcome app");
        me->welcome_size = 0U;
        st = reset_start_wipe(me);
    }
    return st;
}

/**
 * @brief The answer to a find.
 *
 * @param me AO_Boot.
 * @param r  The answer.
 * @return KODE_OK to go on, else the status that stops the reset.
 */
static kode_status_t reset_on_find(Boot * const me, AppFindRspEvt const * const r)
{
    kode_status_t st = KODE_OK;

    if ( r->busy )
    {
        KLOG_W(TAG, "factory reset: AO_AppMgr is busy; try again");
        st = KODE_NO_RESOURCES;
    }
    else if ( (uint8_t)KBOOT_RESET_STEP_FIND == me->reset_step )
    {
        if ( r->installed )
        {
            me->welcome_off  = r->offset;
            me->welcome_size = r->size;
            st = reset_start_link(me);
        }
        else if ( r->on_card )
        {
            me->welcome_card = true;
            st = reset_start_link(me);
        }
        else if ( (uint8_t)KBOOT_RESET_RESTART == me->reset_end )
        {
            KLOG_N(TAG, "factory reset: the welcome app '%s' is neither "
                        "installed nor on the card; it ends in kodeOS",
                   CONFIG_KODEOS_WELCOME_APP);
            st = reset_start_link(me);
        }
        else
        {
            KLOG_W(TAG, "factory reset: the welcome app '%s' is neither "
                        "installed nor on the card; nothing was erased",
                   CONFIG_KODEOS_WELCOME_APP);
            st = KODE_DENIED;
        }
    }
    else if ( (uint8_t)KBOOT_RESET_STEP_INSTALL == me->reset_step )
    {
        if ( me->welcome_written && r->installed )
        {
            me->welcome_off  = r->offset;
            me->welcome_size = r->size;
            st = reset_start_wipe(me);
        }
        else if ( me->welcome_written )
        {
            KLOG_W(TAG, "factory reset: the welcome app was written and is "
                        "not in the directory");
            st = reset_install_failed(me);
        }
        else
        {
            KLOG_D(TAG, "factory reset: a find answer out of turn is ignored");
        }
    }
    else
    {
        KLOG_D(TAG, "factory reset: a late find answer is ignored");
    }
    return st;
}

/**
 * @brief Stop the reset at the step that is running.
 *
 * The caller transitions out of Boot_resetting, whose exit disarms the timer.
 *
 * @param me     AO_Boot.
 * @param status Why, a kode_status_t that is not KODE_OK.
 */
static void reset_stop(Boot * const me, kode_status_t status)
{
    KLOG_W(TAG, "factory reset stopped at %s (status %d)",
           boot_reset_step_name(me->reset_step), (int)status);
    reset_reply(me->reset_who, me->reset_step, status);
}

/**
 * @brief Everything is erased: answer, then restart or wait for the cable.
 *
 * The restart skips leave_begin() on purpose: a C5 parked now would carry the
 * association the wipe just erased into the welcome app (the broker sees an
 * unclean drop). The ship end asks AO_Power to watch the cable before it
 * answers, so a request that was not posted is reported.
 *
 * @param me AO_Boot.
 * @return The next state, or Q_HANDLED() on the restart that does not return.
 */
static QState reset_finish(Boot * const me)
{
    QState status;

    (void)QTimeEvt_disarm(&me->reset);
    KLOG_N(TAG, "factory reset: %s", boot_reset_step_name(me->reset_step));
    if ( (uint8_t)KBOOT_RESET_SHIP == me->reset_end )
    {
        if ( power_ship_on_unplug_request(&me->super) )
        {
            reset_reply(me->reset_who, me->reset_step, KODE_OK);
            KLOG_N(TAG, "factory reset: waiting for the cable to leave for "
                        "%u s, then the board switches off",
                   KODE_SHIP_UNPLUG_S);
            status = Q_TRAN(&Boot_shipping);
        }
        else
        {
            KLOG_E(TAG, "factory reset: AO_Power was not asked to watch the "
                        "cable - not waiting for it");
            reset_reply(me->reset_who, (uint8_t)KBOOT_RESET_STEP_REQUEST,
                        KODE_ERROR);
            status = Q_TRAN(&Boot_active);
        }
    }
    else if ( 0U == me->welcome_size )
    {
        reset_reply(me->reset_who, me->reset_step, KODE_OK);
        KLOG_N(TAG, "factory reset: restarting into kodeOS; no welcome app");
        boot_restart();
        status = Q_TRAN(&Boot_active);
    }
    else if ( kdrv_apps_req_set(me->welcome_off, me->welcome_size,
                                 KAPPS_REQ_STATE_NEW) == KODE_OK )
    {
        reset_reply(me->reset_who, me->reset_step, KODE_OK);
        KLOG_N(TAG, "factory reset: restarting into the welcome app @0x%06lx",
               (unsigned long)me->welcome_off);
        boot_restart();
        status = Q_TRAN(&Boot_active);
    }
    else
    {
        KLOG_E(TAG, "factory reset: could not write the welcome app's boot "
                    "request - not restarting");
        reset_reply(me->reset_who, (uint8_t)KBOOT_RESET_STEP_REQUEST,
                    KODE_ERROR);
        status = Q_TRAN(&Boot_active);
    }
    return status;
}

/**
 * @brief Act on what a step handler decided.
 *
 * @param me AO_Boot.
 * @param st KODE_OK to go on, else the status that stops the reset.
 * @return The next state.
 */
static QState reset_settle(Boot * const me, kode_status_t st)
{
    QState status = Q_HANDLED();

    if ( KODE_OK != st )
    {
        reset_stop(me, st);
        status = Q_TRAN(&Boot_active);
    }
    else if ( (uint8_t)KBOOT_RESET_STEP_DONE == me->reset_step )
    {
        status = reset_finish(me);
    }
    else
    {
        /* The reset goes on in the step the handler started. */
    }
    return status;
}

/**
 * @brief FACTORY_RESET_REQ_SIG in Boot_active: begin, or answer why not.
 *
 * @param me AO_Boot.
 * @param r  The request.
 * @return true when the reset runs and the caller enters Boot_resetting.
 */
static bool reset_begin(Boot * const me, FactoryResetReqEvt const * const r)
{
    kode_status_t st;

    me->reset_who       = r->who;
    me->reset_end       = r->end;
    me->welcome_written = false;
    me->welcome_card    = false;
    me->welcome_off     = 0U;
    me->welcome_size    = 0U;
    if ( (uint8_t)KBOOT_RESET_SHIP == r->end )
    {
        KLOG_N(TAG, "factory reset: asked, ends with the board switched off");
    }
    else
    {
        KLOG_N(TAG, "factory reset: asked, ends with a restart");
    }
    reset_enter(me, KBOOT_RESET_STEP_FIND, KODE_RESET_FIND_MS);
    st = reset_ask_find(me);
    if ( KODE_OK != st )
    {
        (void)QTimeEvt_disarm(&me->reset);
        reset_reply(r->who, (uint8_t)KBOOT_RESET_STEP_FIND, st);
        me->reset_who = NULL;
    }
    return (KODE_OK == st);
}
#endif /* CONFIG_KODEOS_BOOT_ROLE_OS */

/**
 * @brief Running: switches, app runs, `boot info`, the factory reset and the home hold.
 *
 * @param me AO_Boot.
 * @param e  The event.
 * @return The QP status.
 */
static QState Boot_active(Boot * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case BOOT_OS_SWITCH_SIG: {
#if CONFIG_KODEOS_BOOT_ROLE_OS
            /* Before otadata moves: confirming marks the slot otadata names,
             * which from then on is the new one. */
            boot_confirm();
#endif
            if ( !kdrv_slot_present() )
            {
                KLOG_E(TAG, "no inactive kodeOS slot in the partition table");
            }
            else if ( !kdrv_slot_has_image() )
            {
                KLOG_E(TAG, "the inactive kodeOS slot holds no valid image - "
                            "nothing to switch to");
            }
            /* kdrv_otadata_select_slot verifies the image before writing, so
             * a corrupt slot is refused here rather than at the next boot. */
            else if ( kdrv_otadata_select_slot() == KODE_OK )
            {
                KLOG_N(TAG, "switching kodeOS to the other slot; it must "
                            "confirm itself or the rescue brings us back to "
                            "factory");
                leave_begin(me, KDRV_RADIO_NOTE_OS);
                status = Q_TRAN(&Boot_leaving);
                break;
            }
            else
            {
                /* Said, or a failed write would look like an update in progress. */
                KLOG_E(TAG, "could not write otadata - staying on this slot");
            }
            status = Q_HANDLED();
            break;
        }
        case BOOT_APP_RUN_SIG: {
            BootRunEvt const * const ev = Q_EVT_CAST(BootRunEvt);
            /* The mode is the whole of the difference: one word on flash, and
             * the bootloader reads it, not us. */
            uint32_t state = KAPPS_REQ_STATE_NEW;
            if ( KBOOT_RUN_STICKY == ev->mode )
            {
                state = KAPPS_REQ_STATE_ALWAYS;
            }
            if ( kdrv_apps_req_set(ev->offset, ev->size, state) == KODE_OK )
            {
                if ( KBOOT_RUN_STICKY == ev->mode )
                {
                    KLOG_N(TAG, "booting the app @0x%06lx; it stays until "
                                "power-on, a hardware fault or a new upload",
                           (unsigned long)ev->offset);
                }
                else
                {
                    KLOG_N(TAG, "booting the app @0x%06lx; the bootloader "
                                "returns us here", (unsigned long)ev->offset);
                }
                leave_begin(me, KDRV_RADIO_NOTE_APP);
                status = Q_TRAN(&Boot_leaving);
                break;
            }
            else
            {
                KLOG_E(TAG, "could not write the app boot request - not resetting");
            }
            status = Q_HANDLED();
            break;
        }
        case BOOT_INFO_REQ_SIG: {
            boot_dump();
            status = Q_HANDLED();
            break;
        }
        case CONFIG_REQ_SIG: {
            zone_config(me, Q_EVT_CAST(KodeConfigReqEvt));
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODEOS_BOOT_ROLE_OS
        case FACTORY_RESET_REQ_SIG: {
            if ( reset_begin(me, Q_EVT_CAST(FactoryResetReqEvt)) )
            {
                status = Q_TRAN(&Boot_resetting);
            }
            else
            {
                status = Q_HANDLED();
            }
            break;
        }
        case BOOT_FIRST_FRAME_SIG: {
            boot_confirm();
            status = Q_HANDLED();
            break;
        }
        case BOOT_HEALTHY_TMO_SIG: {
            if ( !me->stable_next )
            {
                boot_confirm();
                me->stable_next = true;
                QTimeEvt_armX(&me->healthy,
                              KQP_TICKS_PER_MS(KODE_BOOT_STABLE_MS - KODE_BOOT_CONFIRM_MS),
                              0U);
            }
            else
            {
                kdrv_slot_crashes_set(0U);
            }
            status = Q_HANDLED();
            break;
        }
        case RADIO_STATE_SIG: {
            /* Kept in every state, so a reset asked later knows whether the
             * link is up without asking AO_Radio. */
            me->link_up = (Q_EVT_CAST(RadioStateEvt)->link
                           == (uint8_t)KRADIO_UP);
            status = Q_HANDLED();
            break;
        }
#endif
        case BOOT_EXIT_REQ_SIG: {
            /* A plain reset. Nothing is written: see the initial transition. */
            KLOG_N(TAG, "resetting");
            leave_begin(me, KDRV_RADIO_NOTE_OS);
            status = Q_TRAN(&Boot_leaving);
            break;
        }
        case BUTTON_HOLD_SIG: {
            ButtonHoldEvt const * const h = Q_EVT_CAST(ButtonHoldEvt);
            if ( (uint8_t)KDRV_BTN_BOT == h->id )
            {
                /* Published in both roles: the launcher goes home on it, and
                 * an app's AO learns the board is about to go. */
                static const QEvt home = QEVT_INITIALIZER(HOME_SIG);
                QACTIVE_PUBLISH(&home, &me->super);
#if CONFIG_KODEOS_HOME_HOLD
                /* Home is a plain reset (an app's slot is never confirmed),
                 * and reaches a hung app too: AO_Buttons outranks it. A
                 * second hold inside the grace must not re-arm the timer. */
                if ( QTimeEvt_getCtr(&me->home) == 0U )
                {
                    KLOG_N(TAG, "bottom button held: going home to kodeOS");
                    me->home_deferred = false;
                    me->home_capped   = false;
                    QTimeEvt_armX(&me->home,
                                  KQP_TICKS_PER_MS(KODE_SYS_HOME_GRACE_MS), 0U);
                }
#endif
            }
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODEOS_HOME_HOLD
        case BOOT_HOME_DEFER_SIG: {
            /* Honoured only during the grace: once the cap counts down, the
             * app has had its moment. kode_sys_home_defer() answers which. */
            if ( boot_home_pending() )
            {
                me->home_deferred = true;
            }
            status = Q_HANDLED();
            break;
        }
        case BOOT_HOME_NOW_SIG: {
            /* The app saved what it had to. Disarm first: the timer must
             * not outlive the decision it was counting down to. */
            QTimeEvt_disarm(&me->home);
            KLOG_N(TAG, "the app is ready; going home to kodeOS");
            leave_begin(me, KDRV_RADIO_NOTE_OS);
            status = Q_TRAN(&Boot_leaving);
            break;
        }
        case BOOT_HOME_GRACE_SIG: {
            if ( me->home_deferred && !me->home_capped )
            {
                /* Extend, once, to KODE_SYS_HOME_DEFER_MAX_MS counted from the
                 * hold: the app asked for a moment, and this is the whole
                 * of what a moment may be. */
                me->home_deferred = false;
                me->home_capped   = true;
                QTimeEvt_armX(&me->home,
                              KQP_TICKS_PER_MS(KODE_SYS_HOME_DEFER_MAX_MS
                                               - KODE_SYS_HOME_GRACE_MS), 0U);
                KLOG_I(TAG, "the app asked for a moment before going home");
            }
            else
            {
                if ( me->home_capped )
                {
                    KLOG_W(TAG, "the app never came back; going home anyway");
                }
                /* The bounded leave: it ends KODE_LEAVE_BUDGET_MS after this
                 * expiry at the latest, so a hung app is still reset. */
                leave_begin(me, KDRV_RADIO_NOTE_OS);
                status = Q_TRAN(&Boot_leaving);
                break;
            }
            status = Q_HANDLED();
            break;
        }
#endif
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            /* A deferral an app asked for is a restart a moment away. */
            bool const homing = boot_home_pending();

#if CONFIG_KODEOS_BOOT_ROLE_OS
            /* The wake from OFF is a reset. */
            if ( (uint8_t)KODE_LP_OFF == Q_EVT_CAST(LowPowerRoundEvt)->target )
            {
                boot_confirm();
            }
#endif
            lowpower_ready(me->lp_who, Q_EVT_CAST(LowPowerRoundEvt)->round,
                           homing, "home pending");
            status = Q_HANDLED();
            break;
        }
#endif
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }
    return status;
}

/**
 * @brief The leave: AO_Net, then AO_Radio, then the reset.
 *
 * The substate of Boot_active that a restart request enters once its
 * pre-reset write is done. Every restart request and home gesture is dropped
 * here, so the first request wins and the board resets once; anything else is
 * Boot_active's.
 *
 * @param me AO_Boot.
 * @param e  The event.
 * @return The QP status.
 */
static QState Boot_leaving(Boot * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case BOOT_NET_LEFT_SIG: {
            if ( BOOT_LEAVE_STEP_NET == me->leave_step )
            {
                (void)QTimeEvt_disarm(&me->leave);
                leave_radio_step(me);
            }
            else
            {
                KLOG_D(TAG, "a late answer from AO_Net is ignored");
            }
            status = Q_HANDLED();
            break;
        }
        case BOOT_RADIO_PARKED_SIG: {
            if ( BOOT_LEAVE_STEP_RADIO == me->leave_step )
            {
                leave_finish(me);
            }
            else
            {
                KLOG_D(TAG, "an answer from AO_Radio out of turn is ignored");
            }
            status = Q_HANDLED();
            break;
        }
        case BOOT_LEAVE_TMO_SIG: {
            if ( QTimeEvt_getCtr(&me->leave) != 0U )
            {
                /* The step before ran out as its answer arrived; the timer
                 * counting now is the next step's, not this one. */
                KLOG_D(TAG, "a stale leave timeout is ignored");
            }
            else if ( BOOT_LEAVE_STEP_NET == me->leave_step )
            {
                KLOG_W(TAG, "AO_Net did not answer in time; going on");
                leave_radio_step(me);
            }
            else
            {
                KLOG_W(TAG, "AO_Radio did not answer in time; resetting");
                leave_finish(me);
            }
            status = Q_HANDLED();
            break;
        }
        case BOOT_OS_SWITCH_SIG: {
            leave_drop();
            status = Q_HANDLED();
            break;
        }
        case BOOT_APP_RUN_SIG: {
            leave_drop();
            status = Q_HANDLED();
            break;
        }
        case BOOT_EXIT_REQ_SIG: {
            leave_drop();
            status = Q_HANDLED();
            break;
        }
        case BOOT_HOME_NOW_SIG: {
            leave_drop();
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            /* A board that rested between the boot request's write and the reset
             * would wake into the old firmware. */
            lowpower_ready(me->lp_who, Q_EVT_CAST(LowPowerRoundEvt)->round,
                           true, "restart pending");
            status = Q_HANDLED();
            break;
        }
#endif
#if CONFIG_KODEOS_BOOT_ROLE_OS
        case FACTORY_RESET_REQ_SIG: {
            /* The board is on its way somewhere else; a reset would erase
             * it from under a jump already under way. */
            leave_drop();
            reset_reply(Q_EVT_CAST(FactoryResetReqEvt)->who,
                        (uint8_t)KBOOT_RESET_STEP_FIND, KODE_NO_RESOURCES);
            status = Q_HANDLED();
            break;
        }
#endif
        case BUTTON_HOLD_SIG:
        case BOOT_HOME_DEFER_SIG:
        case BOOT_HOME_GRACE_SIG: {
            /* The board is already on its way: a second hold must not arm a
             * grace of its own, a deferral has nothing left to defer and a
             * grace that ran out meanwhile must not start a second leave. */
            KLOG_D(TAG, "already leaving; this home gesture is dropped");
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&Boot_active);
            break;
        }
    }
    return status;
}

#if CONFIG_KODEOS_BOOT_ROLE_OS
/**
 * @brief The factory reset: one step at a time, each with a budget.
 *
 * The substate of Boot_active that FACTORY_RESET_REQ_SIG enters. Every step
 * ends on its owner's answer, on its budget, or on a failure that stops the
 * reset there; the answer to the asker is posted once either way. Everything
 * that could restart the board or start a second reset is refused.
 *
 * @param me AO_Boot.
 * @param e  The event.
 * @return The QP status.
 */
static QState Boot_resetting(Boot * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case Q_EXIT_SIG: {
            (void)QTimeEvt_disarm(&me->reset);
            (void)QTimeEvt_disarm(&me->again);
            me->reset_who = NULL;
            status = Q_HANDLED();
            break;
        }
        case APP_FIND_RSP_SIG: {
            status = reset_settle(me, reset_on_find(me, Q_EVT_CAST(AppFindRspEvt)));
            break;
        }
        case APP_FLASH_DONE_SIG: {
            kode_status_t st = KODE_OK;

            if ( ((uint8_t)KBOOT_RESET_STEP_INSTALL == me->reset_step)
                && !me->welcome_written )
            {
                /* This reset's write has ended. The name it publishes is the
                 * container's own and proves nothing: the find that follows
                 * says whether the welcome app is in. */
                me->welcome_written = true;
                reset_arm(me, KODE_RESET_FIND_MS);
                st = reset_ask_find(me);
            }
            else
            {
                KLOG_D(TAG, "factory reset: another write's end is ignored");
            }
            status = reset_settle(me, st);
            break;
        }
        case APP_FLASH_FAIL_SIG: {
            kode_status_t st = KODE_OK;

            if ( (uint8_t)KBOOT_RESET_STEP_INSTALL == me->reset_step )
            {
                KLOG_W(TAG, "factory reset: the welcome app could not be "
                            "written");
                st = reset_install_failed(me);
            }
            else
            {
                KLOG_D(TAG, "factory reset: another write's failure is "
                            "ignored");
            }
            status = reset_settle(me, st);
            break;
        }
        case APP_KEEP_ONLY_RSP_SIG: {
            kode_status_t st = KODE_OK;

            if ( (uint8_t)KBOOT_RESET_STEP_APPS == me->reset_step )
            {
                AppKeepOnlyRspEvt const * const r = Q_EVT_CAST(AppKeepOnlyRspEvt);
                if ( (uint8_t)KODE_OK == r->status )
                {
                    KLOG_N(TAG, "factory reset: %u other app(s) removed",
                           (unsigned)r->erased);
                    st = reset_start_nvs(me);
                }
                else
                {
                    KLOG_W(TAG, "factory reset: AO_AppMgr refused to remove "
                                "the other apps");
                    st = (kode_status_t)r->status;
                }
            }
            else
            {
                KLOG_D(TAG, "factory reset: a late keep-only answer is "
                            "ignored");
            }
            status = reset_settle(me, st);
            break;
        }
        case RADIO_STATE_SIG: {
            kode_status_t st = KODE_OK;

            uint8_t const link = Q_EVT_CAST(RadioStateEvt)->link;

            me->link_up = ((uint8_t)KRADIO_UP == link);
            if ( (uint8_t)KBOOT_RESET_STEP_LINK == me->reset_step )
            {
                if ( me->link_up )
                {
                    st = reset_after_link(me);
                }
                else if ( (uint8_t)KRADIO_OFF == link )
                {
                    /* AO_Radio drops a request that arrives while it goes
                     * down, so the first may be lost: ask again, within the
                     * step's budget. */
                    reset_link_ask_again();
                }
                else
                {
                    /* Starting, stopping or failed: nothing to add. */
                }
            }
            status = reset_settle(me, st);
            break;
        }
        case C5_WIPE_RSP_SIG: {
            kode_status_t st = KODE_OK;

            if ( (uint8_t)KBOOT_RESET_STEP_WIPE == me->reset_step )
            {
                C5WipeRspEvt const * const r = Q_EVT_CAST(C5WipeRspEvt);
                if ( ((uint8_t)C5_WIPE_DONE == r->step)
                    && ((uint8_t)KODE_ST_OK == r->status) )
                {
                    st = reset_start_apps(me);
                }
                else if ( ((uint8_t)C5_WIPE_VAULT == r->step)
                           && ((uint8_t)KODE_ST_REFUSED == r->status)
                           && me->link_up )
                {
                    /* With the link up, AO_Radio refuses while a vault
                     * request of its own is in flight. A wipe repeats safely,
                     * within the step's budget. */
                    KLOG_I(TAG, "factory reset: the wipe was refused; asked "
                                "again in %u ms",
                           KODE_RESET_WIPE_AGAIN_MS);
                    (void)QTimeEvt_disarm(&me->again);
                    QTimeEvt_armX(&me->again,
                                  KQP_TICKS_PER_MS(KODE_RESET_WIPE_AGAIN_MS),
                                  0U);
                }
                else
                {
                    KLOG_W(TAG, "factory reset: the wipe stopped at its step "
                                "%u (status %u)", (unsigned)r->step,
                           (unsigned)r->status);
                    st = KODE_ERROR;
                }
            }
            else
            {
                KLOG_D(TAG, "factory reset: a late wipe answer is ignored");
            }
            status = reset_settle(me, st);
            break;
        }
        case BOOT_RESET_STEP_SIG: {
            kode_status_t st = KODE_OK;

            if ( (uint8_t)KBOOT_RESET_STEP_NVS == me->reset_step )
            {
                st = reset_nvs_pass(me);
            }
            status = reset_settle(me, st);
            break;
        }
        case BOOT_RESET_AGAIN_SIG: {
            kode_status_t st = KODE_OK;

#if CONFIG_KODEOS_RADIO
            if ( (uint8_t)KBOOT_RESET_STEP_WIPE == me->reset_step )
            {
                if ( !c5_wipe_request(&me->super) )
                {
                    KLOG_W(TAG, "factory reset: out of events; the wipe was "
                                "not asked again");
                    st = KODE_ERROR;
                }
            }
#endif
            status = reset_settle(me, st);
            break;
        }
        case BOOT_RESET_TMO_SIG: {
            kode_status_t st = KODE_OK;

            if ( (QTimeEvt_getCtr(&me->reset) != 0U)
                || ((uint8_t)KBOOT_RESET_STEP_NVS <= me->reset_step) )
            {
                /* The step before ran out as its answer arrived; what counts
                 * now is the next step's budget, or no step has one. */
                KLOG_D(TAG, "a stale reset timeout is ignored");
            }
            else
            {
                KLOG_W(TAG, "factory reset: %s did not answer in time",
                       boot_reset_step_name(me->reset_step));
                st = KODE_ERROR;
            }
            status = reset_settle(me, st);
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            /* A board that rested in the middle of the reset would wake with
             * half of it done. */
            lowpower_ready(me->lp_who, Q_EVT_CAST(LowPowerRoundEvt)->round,
                           true, "factory reset");
            status = Q_HANDLED();
            break;
        }
#endif
        default: {
            if ( reset_refuses(e) )
            {
                status = Q_HANDLED();
            }
            else
            {
                status = Q_SUPER(&Boot_active);
            }
            break;
        }
    }
    return status;
}

/**
 * @brief The cable has been out long enough: write the request and switch off.
 *
 * A request that cannot be written is logged and the board switches off all
 * the same (nobody reads a refusal; plain kodeOS boots). A switch-off that
 * found no room is logged; the board then stays on, erased.
 *
 * @param me AO_Boot.
 */
static void ship_off(Boot const * const me)
{
    /* The next power-on must not go back a slot. */
    boot_confirm();
    if ( kdrv_apps_req_set(me->welcome_off, me->welcome_size,
                          KAPPS_REQ_STATE_NEW) == KODE_OK )
    {
        KLOG_N(TAG, "factory reset: the cable is out; switching off, "
                    "the welcome app runs at the next power-on");
    }
    else
    {
        KLOG_E(TAG, "factory reset: could not write the welcome "
                    "app's boot request - switching off anyway");
    }
    if ( !power_ship_request() )
    {
        KLOG_E(TAG, "factory reset: AO_Power's queue had no room; the board "
                    "was not switched off");
    }
}

/**
 * @brief One pass of the NVS walk that follows the cable leaving.
 *
 * The NVS step's pass, keep list and bound. A walk that fails is logged and
 * the board switches off anyway: nobody reads a failure after the unplug.
 *
 * @param me AO_Boot.
 */
static void ship_walk(Boot * const me)
{
    kode_status_t const st = reset_nvs_pass(me);

    if ( KODE_OK != st )
    {
        KLOG_E(TAG, "factory reset: the last NVS walk stopped (status %d) - "
                    "switching off anyway", (int)st);
        me->reset_step = (uint8_t)KBOOT_RESET_STEP_DONE;
        ship_off(me);
    }
    else if ( (uint8_t)KBOOT_RESET_STEP_DONE == me->reset_step )
    {
        ship_off(me);
    }
    else
    {
        /* A namespace went; the next pass is posted. */
    }
}

/**
 * @brief The ship end of the reset: everything is erased, the cable is in.
 *
 * Waits for AO_Power's word that the cable has been out KODE_SHIP_UNPLUG_MS.
 * The NVS walk runs again first (settings were written meanwhile), then the
 * welcome app's boot request, last, and the board switches off. Everything
 * that could restart the board is refused. A cable back only cancels the
 * count; the next unplug starts it again.
 *
 * @param me AO_Boot.
 * @param e  The event.
 * @return The QP status.
 */
static QState Boot_shipping(Boot * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case POWER_SHIP_DUE_SIG: {
            if ( (uint8_t)KBOOT_RESET_STEP_DONE == me->reset_step )
            {
                KLOG_N(TAG, "factory reset: the cable is out; erasing what "
                            "was written meanwhile");
                me->reset_step   = (uint8_t)KBOOT_RESET_STEP_NVS;
                me->reset_passes = 0U;
                ship_walk(me);
            }
            else
            {
                KLOG_D(TAG, "factory reset: a second word that the cable is "
                            "out is ignored");
            }
            status = Q_HANDLED();
            break;
        }
        case BOOT_RESET_STEP_SIG: {
            if ( (uint8_t)KBOOT_RESET_STEP_NVS == me->reset_step )
            {
                ship_walk(me);
            }
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            /* A board that rested in the middle of the reset would wake with
             * half of it done. */
            lowpower_ready(me->lp_who, Q_EVT_CAST(LowPowerRoundEvt)->round,
                           true, "factory reset");
            status = Q_HANDLED();
            break;
        }
#endif
        default: {
            if ( reset_refuses(e) )
            {
                status = Q_HANDLED();
            }
            else
            {
                status = Q_SUPER(&Boot_active);
            }
            break;
        }
    }
    return status;
}
#endif /* CONFIG_KODEOS_BOOT_ROLE_OS */
