/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_boot_ao.c
 * @brief AO_Boot: which of three mechanisms owns the next reset.
 */
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "qpc.h"
#include "Mockkdrv_slot.h"

#include "ao_spy.h"
#include "config_support.h"
#include "fakes.h"
#include "kaos.h"
#include "kapps_fmt.h"
#include "kdrv_apps.h"
#include "kdrv_buttons.h"
#include "kdrv_clocks.h"
#include "khal_identity.h"
#include "khal_nvs.h"
#include "khal_time.h"
#include "kbsp_owners.h"
#include "kode_log.h"
#include "kode_sys.h"
#include "kode_wire.h"
#include "kqp.h"
#include "kset.h"
#include "lowpower_cases.h"
#include "qf_support.h"

/**
 * @brief Take the next event waiting in @p ao's queue and report its signal.
 *
 * AO_Net and AO_Radio are mailboxes here, never dispatched: what AO_Boot asked
 * of them is what their queues hold. They are not background AOs, so a tick
 * leaves them alone.
 *
 * @param ao     The mailbox.
 * @param target Where a park request's target goes, or NULL.
 * @return The signal, or -1 when the queue is empty.
 */
static int mailbox_take(QActive *ao, uint8_t *target)
{
    const QEvt *e = NULL;
    int         sig;

    if ( (NULL == ao->eQueue) || (xQueueReceive(ao->eQueue, &e, 0U) != pdTRUE) )
    {
        return -1;
    }
    sig = (int)e->sig;
    if ( (NULL != target) && ((QSignal)RADIO_PARK_REQ_SIG == e->sig) )
    {
        *target = Q_EVT_CAST(RadioParkEvt)->target;
    }
    QF_gc(e);
    return sig;
}

/** @brief Empty the mailboxes of AO_Net and AO_Radio. */
static void mailboxes_drain(void)
{
    while ( mailbox_take(AO_Net, NULL) >= 0 )
    {
    }
    while ( mailbox_take(AO_Radio, NULL) >= 0 )
    {
    }
}

/** @brief AO_Net answers the leave request, as boot_net_left() posts it. */
static void answer_net_left(void)
{
    static const QEvt e = QEVT_INITIALIZER(BOOT_NET_LEFT_SIG);
    QASM_DISPATCH(AO_Boot, &e, 0U);
}

/** @brief AO_Radio answers the park request, as boot_radio_parked() posts it. */
static void answer_radio_parked(void)
{
    static const QEvt e = QEVT_INITIALIZER(BOOT_RADIO_PARKED_SIG);
    QASM_DISPATCH(AO_Boot, &e, 0U);
}

/**
 * @brief Let the C5 answer, which ends the leave in the reset.
 *
 * A case that stops half way would leave the leave timer armed, and the next
 * case's constructor would cut QF's list at it. Every case therefore ends in
 * the reset, here or by the bound running out.
 */
static void finish_leave(void)
{
    if ( setjmp(*fake_system_arm_restart()) == 0 )
    {
        answer_radio_parked();
        TEST_FAIL_MESSAGE("the board should have reset");
    }
    mailboxes_drain();
}

/**
 * @brief Dispatch a restart request and run the leave sequence to its reset.
 *
 * Both neighbours answer at once, in order. Call from inside an armed landing
 * point: the reset leaves by longjmp.
 *
 * @param e The restart request.
 */
static void dispatch_through_leave(const QEvt *e)
{
    QASM_DISPATCH(AO_Boot, e, 0U);
    answer_net_left();
    answer_radio_parked();
}

/** @brief What kdrv_slot_confirm_self() answers: true on the first boot of
 *         an update the bootloader left pending. Only the update case sets it. */
static bool s_update_pending;

/* ---- The crash word, as the register would keep it ------------------------- */

static bool          s_crash_mine;      /**< The word is the running slot's.     */
static uint32_t      s_crash_count;     /**< What it holds.                      */
static unsigned      s_crash_writes;    /**< How many times it was written.      */
static unsigned      s_rejects;         /**< kdrv_slot_reject_self() calls.      */
static kode_status_t s_reject_answer;   /**< What they answer.                   */

static bool crashes_read(uint32_t *count, int calls)
{
    (void)calls;
    if ( s_crash_mine )
    {
        *count = s_crash_count;
    }
    return s_crash_mine;
}

static void crashes_write(uint32_t count, int calls)
{
    (void)calls;
    s_crash_mine  = true;
    s_crash_count = count;
    ++s_crash_writes;
}

static kode_status_t slot_reject(int calls)
{
    (void)calls;
    ++s_rejects;
    return s_reject_answer;
}

/** @brief The boot runs from factory, with otadata blank. */
static bool s_on_factory;

/** @brief How many times the running image was asked to confirm itself. */
static unsigned s_confirms;

static bool confirm_self(int calls)
{
    bool const was = s_update_pending;

    (void)calls;
    ++s_confirms;
    s_update_pending = false;   /* a slot confirms once */
    return was;
}

/** @brief The word as a board that has never crashed left it. */
static void crashes_none(void)
{
    s_crash_mine    = true;
    s_crash_count   = 0U;
    s_crash_writes  = 0U;
    s_rejects       = 0U;
    s_reject_answer = KODE_OK;
    s_confirms      = 0U;
}

/**
 * @brief Put AO_Boot through its initial transition, once per case.
 *
 * Re-running the constructor and the initial transition every time is not
 * tidiness, it is required. Half these cases end in a restart, the restart
 * fake leaves by longjmp, and that jump exits QP mid-dispatch - which leaves
 * the state machine's own bookkeeping half-updated. The next dispatch into it
 * then trips a QP assertion rather than doing anything useful. Reconstructing
 * is the only way back, and it is cheap.
 *
 * The registry entry is a different matter: QActive_register_() refuses a
 * priority slot that is already taken and QF has no unregister, so that part
 * happens exactly once.
 *
 * Must be called after Mockkdrv_slot_Init(), because the ignores below belong
 * to the mock session the case opened.
 *
 * @param reset_reason What kdrv_reset_reason() answers on this boot, one of
 *                     the KDRV_RESET_ values or a number that is none.
 * @param rescued      Whether kodeOS runs from the rescue image with a dirty
 *                     otadata, the state that reads as KBOOT_UNEXPECTED.
 */
static void boot_up(int reset_reason, bool rescued)
{
    static bool s_registered;

    /* The initial transition's own reads. A case that cares about them says
     * so with its own expectations; these keep the rest quiet. */
    kdrv_slot_confirm_self_StubWithCallback(&confirm_self);
    kdrv_slot_crashes_StubWithCallback(&crashes_read);
    kdrv_slot_crashes_set_StubWithCallback(&crashes_write);
    kdrv_slot_reject_self_StubWithCallback(&slot_reject);
    kdrv_reset_reason_IgnoreAndReturn(reset_reason);
    kdrv_running_is_factory_IgnoreAndReturn(rescued);
    if ( rescued )
    {
        kdrv_otadata_blank_IgnoreAndReturn(false);
        kdrv_otadata_clear_IgnoreAndReturn(KODE_OK);
    }
    else if ( s_on_factory )
    {
        /* Factory with nothing in otadata: no rescue, just the image. */
        kdrv_running_is_factory_IgnoreAndReturn(true);
        kdrv_otadata_blank_IgnoreAndReturn(true);
    }

    qf_support_init();
    qf_support_give_queue(AO_Net);
    qf_support_give_queue(AO_Radio);
    mailboxes_drain();

    /* One tick before the constructor, and it is not tidiness either. QP
     * unlinks a spent one-shot from its list only on the NEXT tick, so the
     * home grace of the case that just ended is still linked here - and
     * QTimeEvt_ctorX() zeroes the link field of whatever it is handed, which
     * cuts QF's list at that node and hands the following arm a loop back to
     * itself. The symptom is not an assertion but a tick that never returns.
     * A pump with a ceiling of zero is exactly one tick and no dispatch. */
    (void)qf_support_pump(AO_Boot, 0U);

    Boot_ctor();
    AO_Boot->prio  = KODE_PRIO_BOOT;
    AO_Boot->pthre = KODE_PRIO_BOOT;

    if ( !s_registered )
    {
        QActive_register_(AO_Boot);
        /* Registered here, but subscribed to a signal other suites publish -
         * BUTTON_HOLD_SIG - and dispatched only by the cases below. Without a
         * queue to receive into and a drain to empty it, every hold published
         * anywhere in this binary would keep its reference for ever and the
         * pool, thirty-two blocks shared by every suite, would run down until
         * somebody else's bounded Q_NEW_X quietly returned NULL. */
        qf_support_give_queue(AO_Boot);
        qf_support_background(AO_Boot);
        s_registered = true;
    }

    QASM_INIT(AO_Boot, (void *)0, 0U);
}

/** @brief A plain power-on: the boot almost every case wants. */
static void boot_fresh(void)
{
    boot_up((int)KDRV_RESET_POWERON, false);
}

/* ---- The guard ------------------------------------------------------------ */

/* Without the role flag, the whole OS branch of the initial transition -
 * confirm-self, the boot reason, the rescue check - compiles out, and the
 * cases below would pass while asserting about absent code. */
TEST_CASE("the build really has the OS boot role compiled in", "[boot_ao]")
{
#if !CONFIG_KODEOS_BOOT_ROLE_OS
    TEST_FAIL_MESSAGE("CONFIG_KODEOS_BOOT_ROLE_OS is not set: the boot-reason "
                      "logic compiled out and these cases prove nothing");
#endif
    TEST_PASS();
}

/* ---- The initial transition: why did we boot? ------------------------------ */

/* This is the first case that touches the AO, so it is the one that gets the
 * real initial transition. A cold boot is the ordinary path: no update to
 * confirm, no app coming back, not running from the rescue image. */
TEST_CASE("a plain power-on reads as a cold boot", "[boot_ao]")
{
    Mockkdrv_slot_Init();

    /* The apps region is not reachable in this binary, so no boot request can
     * be read: the reason falls through to cold, which is exactly what the
     * code says a failure there costs. */
    boot_fresh();

    TEST_ASSERT_EQUAL_INT(KBOOT_COLD, boot_reason());
    TEST_ASSERT_EQUAL_INT(KBOOT_APP_END_NONE, boot_app_end());
    TEST_ASSERT_EQUAL_HEX32(KODE_APP_OFFSET_NONE, boot_app_offset());

    /* It claimed both of the resources it owns, in the real table. */
    TEST_ASSERT_EQUAL_STRING("AO_Boot", kbsp_owner_name(KBSP_RES_OTADATA));
    TEST_ASSERT_EQUAL_STRING("AO_Boot",
                             kbsp_owner_name(KBSP_RES_APPS_BOOTREQ));

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* ---- How the app before this boot ended ------------------------------------ */

/** @brief How many KDRV_RESET_ values there are: UNKNOWN is 0, CPU_LOCKUP last. */
#define RESET_MEMBERS       ((size_t)KDRV_RESET_CPU_LOCKUP + 1U)

/** @brief The first number past the last KDRV_RESET_ member. */
#define RESET_PAST_LAST     ((int)KDRV_RESET_CPU_LOCKUP + 1)

/** @brief A reset reason nowhere near any member. */
#define RESET_FAR_OUT       1000

/** @brief A reset reason below the first member. */
#define RESET_BELOW_FIRST   (-1)

/** @brief Room for the message that names the reset reason a row failed on. */
#define RESET_MSG_MAX       48U

/** @brief An app end no kboot_app_end_t class has. */
#define APP_END_NO_CLASS    99

/** @brief The size of the app the TRIED request below points at, in bytes. */
#define TRIED_APP_SIZE      0x8000U

/** @brief Where the apps the request cases below name sit, past the first. */
#define NAMED_APP_OFFSET    (KAPPS_DATA_OFF + (2U * KAPPS_ALIGN))

/** @brief Another app, so a case can tell one boot's answer from the last's. */
#define OTHER_APP_OFFSET    (KAPPS_DATA_OFF + (5U * KAPPS_ALIGN))

/* One row for every KDRV_RESET_ value, in the table's own index. The table
 * and the walks below run from UNKNOWN to CPU_LOCKUP, the enum's last member,
 * and that is all they check:
 *  - a member inserted before CPU_LOCKUP leaves a slot with no row, which
 *    reads NONE (a boot back from an app never answers it), so the walk
 *    fails until the row is written;
 *  - a member added after CPU_LOCKUP lies outside the table and the walks,
 *    and reads CLEAN through the default of boot_app_end_of() with no case
 *    failing. The enum has no count sentinel and no compile-time guard that
 *    CPU_LOCKUP stays last exists: a literal count here would be a second
 *    number to keep in step with the enum.
 * A panic, a watchdog and a brownout have classes of their own; everything
 * else that brings a person home from an app (BOT+LEFT, the external reset
 * pin, the USB port opening) is a way of leaving, CLEAN. */
static const kboot_app_end_t k_end_of_reason[RESET_MEMBERS] = {
    [KDRV_RESET_UNKNOWN]    = KBOOT_APP_END_CLEAN,
    [KDRV_RESET_POWERON]    = KBOOT_APP_END_CLEAN,
    [KDRV_RESET_EXTERNAL]   = KBOOT_APP_END_CLEAN,
    [KDRV_RESET_SOFTWARE]   = KBOOT_APP_END_CLEAN,
    [KDRV_RESET_PANIC]      = KBOOT_APP_END_PANIC,
    [KDRV_RESET_INT_WDT]    = KBOOT_APP_END_WATCHDOG,
    [KDRV_RESET_TASK_WDT]   = KBOOT_APP_END_WATCHDOG,
    [KDRV_RESET_WDT]        = KBOOT_APP_END_WATCHDOG,
    [KDRV_RESET_DEEPSLEEP]  = KBOOT_APP_END_CLEAN,
    [KDRV_RESET_BROWNOUT]   = KBOOT_APP_END_BROWNOUT,
    [KDRV_RESET_SDIO]       = KBOOT_APP_END_CLEAN,
    [KDRV_RESET_USB]        = KBOOT_APP_END_CLEAN,
    [KDRV_RESET_JTAG]       = KBOOT_APP_END_CLEAN,
    [KDRV_RESET_EFUSE]      = KBOOT_APP_END_CLEAN,
    [KDRV_RESET_PWR_GLITCH] = KBOOT_APP_END_BROWNOUT,
    [KDRV_RESET_CPU_LOCKUP] = KBOOT_APP_END_PANIC,
};

TEST_CASE("how an app ended follows the reset reason", "[boot_ao]")
{
    char msg[RESET_MSG_MAX];

    for ( int r = (int)KDRV_RESET_UNKNOWN; (int)KDRV_RESET_CPU_LOCKUP >= r; ++r )
    {
        (void)snprintf(msg, sizeof(msg), "back from an app, reset reason %d", r);
        TEST_ASSERT_EQUAL_INT_MESSAGE(k_end_of_reason[r],
                                      boot_app_end_of(KBOOT_FROM_APP, r), msg);
    }
}

/* A boot that is not back from an app has no app end, whatever reset it: a
 * panic of kodeOS itself is not an app that stopped. */
TEST_CASE("a boot that is not back from an app has no app end", "[boot_ao]")
{
    char msg[RESET_MSG_MAX];

    for ( int r = (int)KDRV_RESET_UNKNOWN; (int)KDRV_RESET_CPU_LOCKUP >= r; ++r )
    {
        (void)snprintf(msg, sizeof(msg), "cold boot, reset reason %d", r);
        TEST_ASSERT_EQUAL_INT_MESSAGE(KBOOT_APP_END_NONE,
                                      boot_app_end_of(KBOOT_COLD, r), msg);
        (void)snprintf(msg, sizeof(msg), "unexpected boot, reset reason %d", r);
        TEST_ASSERT_EQUAL_INT_MESSAGE(KBOOT_APP_END_NONE,
                                      boot_app_end_of(KBOOT_UNEXPECTED, r), msg);
    }
    TEST_ASSERT_EQUAL_INT(KBOOT_APP_END_NONE,
                          boot_app_end_of(KBOOT_COLD, RESET_PAST_LAST));
    TEST_ASSERT_EQUAL_INT(KBOOT_APP_END_NONE,
                          boot_app_end_of(KBOOT_UNEXPECTED, RESET_FAR_OUT));
}

/* A reason this list has never heard of must not read as a panic, a watchdog
 * or a brownout: a false crash class on a healthy app is worse than a missed
 * one. */
TEST_CASE("a reset reason no member names reads as clean", "[boot_ao]")
{
    TEST_ASSERT_EQUAL_INT(KBOOT_APP_END_CLEAN,
                          boot_app_end_of(KBOOT_FROM_APP, RESET_PAST_LAST));
    TEST_ASSERT_EQUAL_INT(KBOOT_APP_END_CLEAN,
                          boot_app_end_of(KBOOT_FROM_APP, RESET_FAR_OUT));
    TEST_ASSERT_EQUAL_INT(KBOOT_APP_END_CLEAN,
                          boot_app_end_of(KBOOT_FROM_APP, RESET_BELOW_FIRST));
}

/* The console and the log say the class by name, and a byte that is not one
 * says so rather than naming a neighbour. */
TEST_CASE("the app end has a name for each class and a mark for none", "[boot_ao]")
{
    TEST_ASSERT_EQUAL_STRING("none",     boot_app_end_name(KBOOT_APP_END_NONE));
    TEST_ASSERT_EQUAL_STRING("clean",    boot_app_end_name(KBOOT_APP_END_CLEAN));
    TEST_ASSERT_EQUAL_STRING("panic",    boot_app_end_name(KBOOT_APP_END_PANIC));
    TEST_ASSERT_EQUAL_STRING("watchdog", boot_app_end_name(KBOOT_APP_END_WATCHDOG));
    TEST_ASSERT_EQUAL_STRING("brownout", boot_app_end_name(KBOOT_APP_END_BROWNOUT));
    TEST_ASSERT_EQUAL_STRING("?",
        boot_app_end_name((kboot_app_end_t)APP_END_NO_CLASS));
}

/**
 * @brief Write a boot request the way kodeOS does.
 *
 * @param offset Where the app sits in the region.
 * @param state  KAPPS_REQ_STATE_NEW or KAPPS_REQ_STATE_ALWAYS.
 */
static void app_request_write(uint32_t offset, uint32_t state)
{
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_init());
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_req_set(offset, TRIED_APP_SIZE, state));
}

/**
 * @brief Move the standing request's state the way the bootloader does.
 *
 * Each state is the previous one with bits cleared, so the bootloader changes
 * it with a four-byte write and no erase; this does the same.
 *
 * @param state ARMED or TRIED.
 */
static void app_request_advance(uint32_t state)
{
    const esp_partition_t * const p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)KAPPS_PART_SUBTYPE,
        "apps");

    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_write(p, KAPPS_REQ_OFF + offsetof(kapps_req_t, state),
                            &state, sizeof(state)));
}

/**
 * @brief Leave the request the bootloader leaves when it has jumped to an app.
 *
 * kdrv_apps_req_tried() reads true on the next AO_Boot initial transition.
 * The AO clears it again.
 */
static void app_request_tried(void)
{
    app_request_write(KAPPS_DATA_OFF, KAPPS_REQ_STATE_NEW);
    app_request_advance(KAPPS_REQ_STATE_TRIED);
}

/**
 * @brief Ask AO_Boot for its state and hand back the class it published.
 *
 * @return BootStateEvt.app_end, as the console would read it.
 */
static uint8_t boot_info_app_end(void)
{
    static const QEvt e = QEVT_INITIALIZER(BOOT_INFO_REQ_SIG);
    uint8_t           app_end;

    kdrv_running_info_ExpectAnyArgs();
    kdrv_slot_present_IgnoreAndReturn(false);
    kdrv_otadata_blank_IgnoreAndReturn(true);

    ao_spy_drain();
    ao_spy_subscribe(BOOT_STATE_SIG);
    QASM_DISPATCH(AO_Boot, &e, 0U);

    const BootStateEvt * const b = (const BootStateEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_INT(BOOT_STATE_SIG, b->super.sig);
    app_end = b->app_end;

    QActive_unsubscribe(ao_spy(), BOOT_STATE_SIG);
    ao_spy_drain();
    return app_end;
}

/** @brief How many lines the log holds that contain @p needle. */
static unsigned log_lines_with(const char *needle)
{
    unsigned n = 0U;

    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            ++n;
        }
    }
    return n;
}

/* A boot from OFF is a boot, and the log says which: the wake that got past
 * kode_init()'s gate is BOT's. Any other reset reason says nothing of it. */
TEST_CASE("a wake from deep sleep says it came from OFF", "[boot_ao]")
{
    Mockkdrv_slot_Init();
    fake_klog_reset();

    boot_up((int)KDRV_RESET_DEEPSLEEP, false);

    TEST_ASSERT_EQUAL_UINT(1U, log_lines_with("woke from OFF (BOT)"));
    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("a plain power-on does not say it came from OFF", "[boot_ao]")
{
    Mockkdrv_slot_Init();
    fake_klog_reset();

    boot_fresh();

    TEST_ASSERT_EQUAL_UINT(0U, log_lines_with("woke from OFF"));
    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* The seam end to end, with the flash a real emulation: the bootloader's
 * TRIED mark, a panic as the chip's reset reason, and what AO_Boot then
 * publishes and says. */
TEST_CASE("coming back from an app that panicked says panic", "[boot_ao]")
{
    Mockkdrv_slot_Init();
    app_request_tried();
    boot_up((int)KDRV_RESET_PANIC, false);

    TEST_ASSERT_EQUAL_INT(KBOOT_FROM_APP, boot_reason());
    TEST_ASSERT_EQUAL_INT(KBOOT_APP_END_PANIC, boot_app_end());
    TEST_ASSERT_EQUAL_UINT(1U, log_lines_with("how the user app ended: panic"));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KBOOT_APP_END_PANIC, boot_info_app_end());

    Mockkdrv_slot_Destroy();
}

/* BOT+LEFT is the P4's CHIP_PU, which the chip calls a power-on: the person
 * left the app by the hardware way, which is no crash. */
TEST_CASE("coming back from an app by BOT+LEFT says clean", "[boot_ao]")
{
    Mockkdrv_slot_Init();
    app_request_tried();
    boot_up((int)KDRV_RESET_POWERON, false);

    TEST_ASSERT_EQUAL_INT(KBOOT_FROM_APP, boot_reason());
    TEST_ASSERT_EQUAL_INT(KBOOT_APP_END_CLEAN, boot_app_end());
    TEST_ASSERT_EQUAL_UINT(1U, log_lines_with("how the user app ended: clean"));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KBOOT_APP_END_CLEAN, boot_info_app_end());

    Mockkdrv_slot_Destroy();
}

/* A cold boot publishes none: the byte is set on every boot, never left from
 * the last one's value (the case before this one ends in CLEAN). */
TEST_CASE("a cold boot publishes no app end", "[boot_ao]")
{
    Mockkdrv_slot_Init();
    boot_up((int)KDRV_RESET_PANIC, false);

    TEST_ASSERT_EQUAL_INT(KBOOT_COLD, boot_reason());
    TEST_ASSERT_EQUAL_UINT(0U, log_lines_with("how the user app ended"));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KBOOT_APP_END_NONE, boot_info_app_end());

    Mockkdrv_slot_Destroy();
}

/* ---- Confirming an update ---------------------------------------------------
 *
 * The first boot of a kodeOS update runs from a slot the bootloader left pending:
 * nothing confirms it but AO_Boot, and an update nobody confirms rolls back at
 * the next reset. Confirming at once would rule that out for everything past
 * this AO's first step, the panel, the radio and the interface among them. */

/** @brief The next crank of AO_Boot's own queue, its timers' events included. */
static void boot_drain(void)
{
    const QEvt *e;

    while ( xQueueReceive(AO_Boot->eQueue, &e, 0U) == pdTRUE )
    {
        QASM_DISPATCH(AO_Boot, e, 0U);
        QF_gc(e);
    }
}

/** @brief Let @p ms go by, AO_Boot's own events answered. */
static void boot_wait(uint32_t ms)
{
    (void)qf_support_tick(AO_Boot, QF_SUPPORT_STEPS(ms));
    boot_drain();
}

/** @brief An update on its first boot, with nothing asked of it yet. */
static void update_boots(void)
{
    Mockkdrv_slot_Init();
    crashes_none();
    s_update_pending = true;
    boot_fresh();
}

/** @brief Run the healthy timer out, so the next case's constructor finds it spent. */
static void update_done(void)
{
    s_update_pending = false;
    boot_wait(KODE_BOOT_STABLE_MS);
    Mockkdrv_slot_Destroy();
}

TEST_CASE("the first boot of a kodeOS update does not confirm itself at once", "[boot_ao]")
{
    update_boots();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_confirms,
        "confirmed before the panel, the radio or the interface proved anything");
    TEST_ASSERT_EQUAL_INT(KBOOT_COLD, boot_reason());
    s_update_pending = false;
    Mockkdrv_slot_Destroy();
}

TEST_CASE("the first frame on the glass confirms an update", "[boot_ao]")
{
    update_boots();
    boot_system_up();

    boot_first_frame();
    boot_drain();

    TEST_ASSERT_EQUAL_UINT(1U, s_confirms);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, log_lines_with("kodeOS update confirmed"),
        "a pending update was not confirmed");
    update_done();
}

/* A board on the line has no glass and must not go back a slot for it. */
TEST_CASE("a board with no first frame confirms once the rows have had their time",
          "[boot_ao]")
{
    update_boots();
    boot_system_up();

    boot_wait(KODE_BOOT_CONFIRM_MS - 10U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_confirms, "not yet");
    boot_wait(10U);
    TEST_ASSERT_EQUAL_UINT(1U, s_confirms);
    TEST_ASSERT_EQUAL_UINT(1U, log_lines_with("kodeOS update confirmed"));
    update_done();
}

TEST_CASE("the time runs from the rows, not from this AO's start", "[boot_ao]")
{
    update_boots();

    boot_wait(KODE_BOOT_STABLE_MS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_confirms,
        "a board stuck in kode_init() answered for its image");
    s_update_pending = false;
    Mockkdrv_slot_Destroy();
}

TEST_CASE("an ordinary boot confirms nothing", "[boot_ao]")
{
    Mockkdrv_slot_Init();
    crashes_none();
    boot_fresh();
    boot_system_up();
    boot_first_frame();
    boot_drain();

    TEST_ASSERT_EQUAL_UINT(0U, log_lines_with("kodeOS update confirmed"));
    update_done();
}

/* At the next reset the bootloader turns a pending slot into an aborted one, and
 * it cannot tell a restart on purpose from a crash. */
TEST_CASE("a restart on purpose confirms the update first", "[boot_ao]")
{
    static const QEvt e = QEVT_INITIALIZER(BOOT_EXIT_REQ_SIG);

    update_boots();
    if ( setjmp(*fake_system_arm_restart()) == 0 )
    {
        dispatch_through_leave(&e);
        TEST_FAIL_MESSAGE("the board should have reset");
    }
    TEST_ASSERT_EQUAL_UINT(1U, s_confirms);
    TEST_ASSERT_EQUAL_UINT(1U, log_lines_with("kodeOS update confirmed"));
    mailboxes_drain();
    s_update_pending = false;
    Mockkdrv_slot_Destroy();
}

/** @brief kdrv_otadata_select_slot(), asserting the running image confirmed first. */
static kode_status_t select_after_confirm(int calls)
{
    (void)calls;
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_confirms,
        "a mark set after this write lands on the slot it selects");
    return KODE_OK;
}

TEST_CASE("the next update confirms this one before otadata moves", "[boot_ao]")
{
    static const QEvt e = QEVT_INITIALIZER(BOOT_OS_SWITCH_SIG);

    update_boots();
    kdrv_slot_present_ExpectAndReturn(true);
    kdrv_slot_has_image_ExpectAndReturn(true);
    kdrv_otadata_select_slot_StubWithCallback(&select_after_confirm);

    if ( setjmp(*fake_system_arm_restart()) == 0 )
    {
        dispatch_through_leave(&e);
        TEST_FAIL_MESSAGE("the board should have reset");
    }
    mailboxes_drain();
    s_update_pending = false;
    Mockkdrv_slot_Destroy();
}

/* The wake from OFF is a reset; the wake from a rest is not. */
TEST_CASE("a round for OFF confirms the update, a round for ASLEEP does not",
          "[boot_ao][lowpower]")
{
    update_boots();

    lp_round(AO_Boot, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();
    TEST_ASSERT_EQUAL_UINT(0U, s_confirms);

    lp_round(AO_Boot, KODE_LP_OFF);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();
    TEST_ASSERT_EQUAL_UINT(1U, s_confirms);

    s_update_pending = false;
    Mockkdrv_slot_Destroy();
}

/* ---- Crashes in a row ------------------------------------------------------- */

TEST_CASE("a panic or a watchdog is one more crash", "[boot_ao][crashes]")
{
    TEST_ASSERT_EQUAL_UINT32(1U, boot_crashes_next(0U, (int)KDRV_RESET_PANIC, false));
    TEST_ASSERT_EQUAL_UINT32(2U, boot_crashes_next(1U, (int)KDRV_RESET_CPU_LOCKUP, false));
    TEST_ASSERT_EQUAL_UINT32(1U, boot_crashes_next(0U, (int)KDRV_RESET_INT_WDT, false));
    TEST_ASSERT_EQUAL_UINT32(1U, boot_crashes_next(0U, (int)KDRV_RESET_TASK_WDT, false));
    TEST_ASSERT_EQUAL_UINT32(1U, boot_crashes_next(0U, (int)KDRV_RESET_WDT, false));
}

TEST_CASE("the count stops at the limit", "[boot_ao][crashes]")
{
    TEST_ASSERT_EQUAL_UINT32(KODE_BOOT_CRASHES_MAX,
                             boot_crashes_next(KODE_BOOT_CRASHES_MAX, (int)KDRV_RESET_PANIC, false));
}

TEST_CASE("a brownout neither counts nor forgets", "[boot_ao][crashes]")
{
    TEST_ASSERT_EQUAL_UINT32(2U, boot_crashes_next(2U, (int)KDRV_RESET_BROWNOUT, false));
    TEST_ASSERT_EQUAL_UINT32(2U, boot_crashes_next(2U, (int)KDRV_RESET_PWR_GLITCH, false));
}

TEST_CASE("every way of leaving forgets, a reason nobody named included", "[boot_ao][crashes]")
{
    static const int leaving[] = {
        (int)KDRV_RESET_POWERON, (int)KDRV_RESET_SOFTWARE, (int)KDRV_RESET_DEEPSLEEP,
        (int)KDRV_RESET_USB, (int)KDRV_RESET_JTAG, (int)KDRV_RESET_EXTERNAL, 1234,
    };

    for ( size_t i = 0U; (sizeof(leaving) / sizeof(leaving[0])) > i; ++i )
    {
        TEST_ASSERT_EQUAL_UINT32(0U, boot_crashes_next(2U, leaving[i], false));
    }
}

TEST_CASE("what ended an app says nothing about kodeOS", "[boot_ao][crashes]")
{
    TEST_ASSERT_EQUAL_UINT32(2U, boot_crashes_next(2U, (int)KDRV_RESET_PANIC, true));
    TEST_ASSERT_EQUAL_UINT32(2U, boot_crashes_next(2U, (int)KDRV_RESET_POWERON, true));
}

/** @brief A boot with this much kept for its slot, after this reset. */
static void boot_after_crashes(uint32_t kept, int reason, bool factory)
{
    Mockkdrv_slot_Init();
    crashes_none();
    s_crash_count = kept;
    s_on_factory  = factory;
    boot_up(reason, false);
    s_on_factory  = false;
}

TEST_CASE("a crash short of the limit is kept and the boot goes on", "[boot_ao][crashes]")
{
    boot_after_crashes(1U, (int)KDRV_RESET_PANIC, false);

    TEST_ASSERT_EQUAL_UINT32(2U, s_crash_count);
    TEST_ASSERT_EQUAL_UINT(0U, s_rejects);
    TEST_ASSERT_EQUAL_UINT(0U, fake_system_restarts());
    Mockkdrv_slot_Destroy();
}

/* What no bootloader can do: this image confirmed itself and then fell over, boot
 * after boot. */
TEST_CASE("the third crash in a row gives the slot up and restarts", "[boot_ao][crashes]")
{
    if ( setjmp(*fake_system_arm_restart()) == 0 )
    {
        boot_after_crashes(KODE_BOOT_CRASHES_MAX - 1U, (int)KDRV_RESET_TASK_WDT, false);
        TEST_FAIL_MESSAGE("the board should have restarted");
    }

    TEST_ASSERT_EQUAL_UINT(1U, s_rejects);
    TEST_ASSERT_EQUAL_UINT(1U, fake_system_restarts());
    TEST_ASSERT_EQUAL_UINT32(KODE_BOOT_CRASHES_MAX, s_crash_count);
    Mockkdrv_slot_Destroy();
}

TEST_CASE("factory has nothing to go back to and goes on", "[boot_ao][crashes]")
{
    boot_after_crashes(KODE_BOOT_CRASHES_MAX - 1U, (int)KDRV_RESET_PANIC, true);

    TEST_ASSERT_EQUAL_UINT(0U, s_rejects);
    TEST_ASSERT_EQUAL_UINT(0U, fake_system_restarts());
    TEST_ASSERT_EQUAL_UINT(1U, log_lines_with("factory has nothing to go back to"));
    Mockkdrv_slot_Destroy();
}

TEST_CASE("a slot that cannot be given up goes on and says so", "[boot_ao][crashes]")
{
    Mockkdrv_slot_Init();
    crashes_none();
    s_crash_count   = KODE_BOOT_CRASHES_MAX - 1U;
    s_reject_answer = KODE_ERROR;
    boot_up((int)KDRV_RESET_PANIC, false);

    TEST_ASSERT_EQUAL_UINT(1U, s_rejects);
    TEST_ASSERT_EQUAL_UINT(0U, fake_system_restarts());
    TEST_ASSERT_EQUAL_UINT(1U, log_lines_with("could not give this slot up"));
    Mockkdrv_slot_Destroy();
}

/* Three apps that panic are three apps, and kodeOS is not to blame. */
TEST_CASE("an app that panics is not counted against kodeOS", "[boot_ao][crashes]")
{
    Mockkdrv_slot_Init();
    crashes_none();
    s_crash_count = KODE_BOOT_CRASHES_MAX - 1U;
    app_request_tried();
    boot_up((int)KDRV_RESET_PANIC, false);

    TEST_ASSERT_EQUAL_INT(KBOOT_FROM_APP, boot_reason());
    TEST_ASSERT_EQUAL_UINT32(KODE_BOOT_CRASHES_MAX - 1U, s_crash_count);
    TEST_ASSERT_EQUAL_UINT(0U, s_rejects);
    Mockkdrv_slot_Destroy();
}

/* The update panicked and the bootloader went back: the reason is the update's,
 * and the slot that reads it has done nothing. */
TEST_CASE("a word that is another slot's counts nothing and becomes this one's",
          "[boot_ao][crashes]")
{
    Mockkdrv_slot_Init();
    crashes_none();
    s_crash_mine  = false;
    s_crash_count = KODE_BOOT_CRASHES_MAX - 1U;
    boot_up((int)KDRV_RESET_PANIC, false);

    TEST_ASSERT_TRUE(s_crash_mine);
    TEST_ASSERT_EQUAL_UINT32(0U, s_crash_count);
    TEST_ASSERT_EQUAL_UINT(0U, s_rejects);
    Mockkdrv_slot_Destroy();
}

/* The first frame comes early and confirming is for ever: an image that panics
 * moments after it must still count, so the frame forgets nothing. */
TEST_CASE("the crashes are forgotten only after a long time up", "[boot_ao][crashes]")
{
    update_boots();
    s_crash_count = 2U;
    boot_system_up();

    boot_first_frame();
    boot_drain();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2U, s_crash_count, "a frame is not a long time");

    boot_wait(KODE_BOOT_STABLE_MS - 10U);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2U, s_crash_count, "not yet");
    boot_wait(10U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_crash_count);
    s_update_pending = false;
    Mockkdrv_slot_Destroy();
}

/* kodeOS running proves the app is over, so a request still asking for a
 * boot is one whose retire write did not stick. Left there, a reset on the
 * bootloader's keep list would jump into that sketch instead of home. */
TEST_CASE("a boot request still standing at boot is cleared", "[boot_ao]")
{
    Mockkdrv_slot_Init();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_init());
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_req_set(KAPPS_DATA_OFF, TRIED_APP_SIZE, KAPPS_REQ_STATE_ALWAYS));
    TEST_ASSERT_TRUE(kdrv_apps_req_present());

    boot_fresh();

    TEST_ASSERT_EQUAL_INT(KBOOT_COLD, boot_reason());
    TEST_ASSERT_FALSE_MESSAGE(kdrv_apps_req_present(),
        "a standing request survived a boot of kodeOS");
    TEST_ASSERT_EQUAL_UINT(1U, log_lines_with("cleared a stale boot request"));

    Mockkdrv_slot_Destroy();
}

/* The rescue check runs after the request is read and can turn the boot into
 * UNEXPECTED. That boot is not back from an app any more, so it says none,
 * and says nothing in the log, even though a request was tried. */
TEST_CASE("an unexpected boot says no app ended even with a tried request",
          "[boot_ao]")
{
    Mockkdrv_slot_Init();
    app_request_tried();
    boot_up((int)KDRV_RESET_PANIC, true);

    TEST_ASSERT_EQUAL_INT(KBOOT_UNEXPECTED, boot_reason());
    TEST_ASSERT_EQUAL_INT(KBOOT_APP_END_NONE, boot_app_end());
    TEST_ASSERT_EQUAL_UINT(0U, log_lines_with("how the user app ended"));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KBOOT_APP_END_NONE, boot_info_app_end());
}

/* ---- Which app ran --------------------------------------------------------- */

/* The offset is the request's own: the app's place in the apps region, the
 * number its directory entry carries, so a launcher can find its row. */
TEST_CASE("coming back from an app names the app that ran", "[boot_ao]")
{
    Mockkdrv_slot_Init();
    app_request_write(NAMED_APP_OFFSET, KAPPS_REQ_STATE_NEW);
    app_request_advance(KAPPS_REQ_STATE_TRIED);
    boot_fresh();

    TEST_ASSERT_EQUAL_INT(KBOOT_FROM_APP, boot_reason());
    TEST_ASSERT_EQUAL_HEX32(NAMED_APP_OFFSET, boot_app_offset());

    Mockkdrv_slot_Destroy();
}

/* Set on every boot, never left from the last one: the second boot below
 * finds the request the first one cleared. */
TEST_CASE("a cold boot names no app, whatever the boot before named",
          "[boot_ao]")
{
    Mockkdrv_slot_Init();
    app_request_write(OTHER_APP_OFFSET, KAPPS_REQ_STATE_NEW);
    app_request_advance(KAPPS_REQ_STATE_TRIED);
    boot_fresh();
    TEST_ASSERT_EQUAL_HEX32(OTHER_APP_OFFSET, boot_app_offset());

    boot_fresh();
    TEST_ASSERT_EQUAL_INT(KBOOT_COLD, boot_reason());
    TEST_ASSERT_EQUAL_HEX32(KODE_APP_OFFSET_NONE, boot_app_offset());

    Mockkdrv_slot_Destroy();
}

/* The bootloader retires an ARMED request before it boots kodeOS, so kodeOS
 * reads ARMED only when that write did not stick. The app did run, so it is
 * named; the request is still cleared, and the reason stays what it was for
 * every stale request. */
TEST_CASE("a sticky run left ARMED names its app and the request is cleared",
          "[boot_ao]")
{
    Mockkdrv_slot_Init();
    app_request_write(NAMED_APP_OFFSET, KAPPS_REQ_STATE_ALWAYS);
    app_request_advance(KAPPS_REQ_STATE_ARMED);
    boot_fresh();

    TEST_ASSERT_EQUAL_INT(KBOOT_COLD, boot_reason());
    TEST_ASSERT_EQUAL_HEX32(NAMED_APP_OFFSET, boot_app_offset());
    TEST_ASSERT_FALSE(kdrv_apps_req_present());

    Mockkdrv_slot_Destroy();
}

/* NEW and ALWAYS are the states before the bootloader launches anything: a
 * request that stands in one of them was refused, and no app ran. */
TEST_CASE("a request the bootloader never launched names nobody", "[boot_ao]")
{
    Mockkdrv_slot_Init();
    app_request_write(NAMED_APP_OFFSET, KAPPS_REQ_STATE_NEW);
    boot_fresh();
    TEST_ASSERT_EQUAL_HEX32(KODE_APP_OFFSET_NONE, boot_app_offset());
    TEST_ASSERT_FALSE(kdrv_apps_req_present());

    app_request_write(NAMED_APP_OFFSET, KAPPS_REQ_STATE_ALWAYS);
    boot_fresh();
    TEST_ASSERT_EQUAL_HEX32(KODE_APP_OFFSET_NONE, boot_app_offset());
    TEST_ASSERT_FALSE(kdrv_apps_req_present());

    Mockkdrv_slot_Destroy();
}

/* ---- The wall clock -------------------------------------------------------- */

/** @brief Whether any captured log line holds @p needle. */
static bool klog_has(const char *needle)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            return true;
        }
    }
    return false;
}

/** @brief Where the first line holding @p needle was captured, or a value
 *         past every line when there is none. */
static unsigned klog_at(const char *needle)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            return i;
        }
    }
    return UINT32_MAX;
}

/* The chip lost its supply, so its clock is gone, and the last value kept
 * comes back before anything reads the hour - the face, a log stamp. */
TEST_CASE("the boot puts a kept clock back before anything reads it",
          "[boot_ao]")
{
    Mockkdrv_slot_Init();
    fake_nvs_reset();
    fake_klog_reset();
    fake_time_wall_set(0U);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          khal_nvs_set_u64("kclock", "wall", 1790000000ULL));

    boot_fresh();

    TEST_ASSERT_TRUE(kdrv_clocks_wall_known());
    TEST_ASSERT_TRUE(kdrv_clocks_wall_approx());
    TEST_ASSERT_TRUE(klog_has("the clock lost its power; restored to "
                              "2026-09-21T14:13:20Z"));
    /* Before the boot's own lines, so they carry the date; and before the
     * role's branch, so an app reads the approximate mark too. */
    TEST_ASSERT_TRUE_MESSAGE(klog_at("the clock lost its power") < klog_at("cold boot"),
                             "the clock is put back before the boot says anything");

    kdrv_clocks_wall_note_sntp();
    fake_time_wall_set(0U);
    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* ---- Switching kodeOS to the other slot ------------------------------------ */

/* No second slot in the partition table at all. The refusal has to say so and
 * must not reset: a reset here would boot the same image again and look like
 * the update silently did nothing. */
TEST_CASE("switching with no inactive slot refuses without resetting",
          "[boot_ao]")
{
    static const QEvt e = QEVT_INITIALIZER(BOOT_OS_SWITCH_SIG);

    Mockkdrv_slot_Init();
    boot_fresh();

    kdrv_slot_present_ExpectAndReturn(false);
    /* No otadata write is expected: reaching one fails this case. */

    QASM_DISPATCH(AO_Boot, &e, 0U);

    TEST_ASSERT_EQUAL_UINT(0U, fake_system_restarts());
    TEST_ASSERT_GREATER_THAN_UINT(0U, fake_klog_count());

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* A slot that exists but holds nothing bootable. Selecting it would hand the
 * next boot to an empty partition and rely on the rescue to come back. */
TEST_CASE("switching to a slot with no image refuses without resetting",
          "[boot_ao]")
{
    static const QEvt e = QEVT_INITIALIZER(BOOT_OS_SWITCH_SIG);

    Mockkdrv_slot_Init();
    boot_fresh();

    kdrv_slot_present_ExpectAndReturn(true);
    kdrv_slot_has_image_ExpectAndReturn(false);

    QASM_DISPATCH(AO_Boot, &e, 0U);

    TEST_ASSERT_EQUAL_UINT(0U, fake_system_restarts());

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* Everything in order: otadata is written and the board goes. The reset is
 * the assertion - it is the whole point of the signal. */
TEST_CASE("a good switch writes otadata and resets", "[boot_ao]")
{
    static const QEvt e = QEVT_INITIALIZER(BOOT_OS_SWITCH_SIG);

    Mockkdrv_slot_Init();
    boot_fresh();

    kdrv_slot_present_ExpectAndReturn(true);
    kdrv_slot_has_image_ExpectAndReturn(true);
    kdrv_otadata_select_slot_ExpectAndReturn(KODE_OK);

    if ( setjmp(*fake_system_arm_restart()) == 0 )
    {
        dispatch_through_leave(&e);
        TEST_FAIL_MESSAGE("the board should have reset");
    }

    TEST_ASSERT_EQUAL_UINT(1U, fake_system_restarts());
    mailboxes_drain();

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* The case the audit found. The chain used to end without a final `else`, so
 * an otadata write that failed produced nothing at all: no line, no event, no
 * reset. An `os update` in that state was indistinguishable from one still
 * running, and the two branches above it both said why they refused. */
TEST_CASE("an otadata write that fails says so instead of going quiet",
          "[boot_ao]")
{
    static const QEvt e = QEVT_INITIALIZER(BOOT_OS_SWITCH_SIG);
    unsigned before;

    Mockkdrv_slot_Init();
    boot_fresh();

    kdrv_slot_present_ExpectAndReturn(true);
    kdrv_slot_has_image_ExpectAndReturn(true);
    kdrv_otadata_select_slot_ExpectAndReturn(KODE_ERROR);

    before = fake_klog_count();
    QASM_DISPATCH(AO_Boot, &e, 0U);

    /* It did not reset - correct, there is nothing to boot into. */
    TEST_ASSERT_EQUAL_UINT(0U, fake_system_restarts());

    /* And it did not go quiet, which is the part that was missing. */
    TEST_ASSERT_GREATER_THAN_UINT(before, fake_klog_count());

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* ---- Running an app -------------------------------------------------------- */

/* The boot request could not be written - there is no apps region in this
 * binary - so the board must stay where it is. Resetting on a request that
 * was never recorded would land back in kodeOS with no explanation, which
 * reads to an operator as "app run did nothing". */
/* The offset is deliberately past the end of the region. This binary now
 * carries the board's real partition table, so the apps region genuinely
 * exists and an in-bounds request would be written and the board reset - the
 * refusal has to come from the bounds check rather than from an absent
 * partition, which is what the earlier version of this case was quietly
 * relying on. */
TEST_CASE("an app run whose request cannot be written does not reset",
          "[boot_ao]")
{
    BootRunEvt e = { .super = QEVT_INITIALIZER(BOOT_APP_RUN_SIG),
                     .offset = 0xFF000000U, .size = 0x8000U };

    Mockkdrv_slot_Init();
    boot_fresh();

    QASM_DISPATCH(AO_Boot, &e.super, 0U);

    TEST_ASSERT_EQUAL_UINT(0U, fake_system_restarts());
    TEST_ASSERT_GREATER_THAN_UINT(0U, fake_klog_count());

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/**
 * @brief The apps region as the driver finds it.
 *
 * The request the AO just wrote is read back straight from flash rather than
 * from the driver: what the bootloader will find is the only thing that
 * decides whether the app boots once or keeps booting.
 *
 * @return The region's partition, or NULL if this binary has no table.
 */
static const esp_partition_t *boot_region(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    (esp_partition_subtype_t)KAPPS_PART_SUBTYPE,
                                    NULL);
}

/* The launcher's request. One boot, then home: NEW on flash, and a reset. */
TEST_CASE("a once-only app run writes NEW and resets", "[boot_ao]")
{
    BootRunEvt e = { .super = QEVT_INITIALIZER(BOOT_APP_RUN_SIG),
                     .offset = KAPPS_DATA_OFF, .size = 0x8000U,
                     .mode = KBOOT_RUN_ONCE };
    kapps_req_t req;

    Mockkdrv_slot_Init();
    boot_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_init());

    if ( setjmp(*fake_system_arm_restart()) == 0 )
    {
        dispatch_through_leave(&e.super);
        TEST_FAIL_MESSAGE("the board should have reset");
    }

    TEST_ASSERT_EQUAL_UINT(1U, fake_system_restarts());
    mailboxes_drain();
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_read(boot_region(), KAPPS_REQ_OFF, &req, sizeof(req)));
    TEST_ASSERT_EQUAL_HEX32(KAPPS_REQ_STATE_NEW, req.state);
    TEST_ASSERT_EQUAL_HEX32(KAPPS_DATA_OFF, req.offset);

    kdrv_apps_req_clear();
    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* The mailbox's request. The word on flash is the only difference: the
 * bootloader, not this AO, decides what a sticky request survives. */
TEST_CASE("a sticky app run writes ALWAYS and resets", "[boot_ao]")
{
    BootRunEvt e = { .super = QEVT_INITIALIZER(BOOT_APP_RUN_SIG),
                     .offset = KAPPS_DATA_OFF, .size = 0x8000U,
                     .mode = KBOOT_RUN_STICKY };
    kapps_req_t req;

    Mockkdrv_slot_Init();
    boot_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_init());

    if ( setjmp(*fake_system_arm_restart()) == 0 )
    {
        dispatch_through_leave(&e.super);
        TEST_FAIL_MESSAGE("the board should have reset");
    }

    TEST_ASSERT_EQUAL_UINT(1U, fake_system_restarts());
    mailboxes_drain();
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_read(boot_region(), KAPPS_REQ_OFF, &req, sizeof(req)));
    TEST_ASSERT_EQUAL_HEX32(KAPPS_REQ_STATE_ALWAYS, req.state);

    kdrv_apps_req_clear();
    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* A mailbox adoption (an IDE upload) asks for a sticky run of the entry it
 * wrote. The request goes down as ALWAYS, the bootloader arms it on its first
 * boot, and the reset it does not keep takes it to TRIED before kodeOS runs:
 * the walk ends where a run once does, with the offset the run asked for. */
TEST_CASE("an adopted app's sticky run names it once the bootloader retired it",
          "[boot_ao]")
{
    BootRunEvt e = { .super = QEVT_INITIALIZER(BOOT_APP_RUN_SIG),
                     .offset = NAMED_APP_OFFSET, .size = TRIED_APP_SIZE,
                     .mode = KBOOT_RUN_STICKY };
    kapps_req_t req;

    Mockkdrv_slot_Init();
    boot_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_init());

    if ( setjmp(*fake_system_arm_restart()) == 0 )
    {
        dispatch_through_leave(&e.super);
        TEST_FAIL_MESSAGE("the board should have reset");
    }
    mailboxes_drain();
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_read(boot_region(), KAPPS_REQ_OFF, &req, sizeof(req)));
    TEST_ASSERT_EQUAL_HEX32(KAPPS_REQ_STATE_ALWAYS, req.state);
    TEST_ASSERT_EQUAL_HEX32(NAMED_APP_OFFSET, req.offset);

    app_request_advance(KAPPS_REQ_STATE_ARMED);
    app_request_advance(KAPPS_REQ_STATE_TRIED);
    boot_fresh();

    TEST_ASSERT_EQUAL_INT(KBOOT_FROM_APP, boot_reason());
    TEST_ASSERT_EQUAL_HEX32(NAMED_APP_OFFSET, boot_app_offset());

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* ---- Leaving, without writing anything ------------------------------------- */

/* The whole safety property of the app model in one case: an app asking to
 * come home resets and touches no flash at all. Any write here - otadata, a
 * boot request, a slot - would mean an app could change what boots next, and
 * the model rests on it being unable to. */
TEST_CASE("exiting to the OS resets and writes nothing", "[boot_ao]")
{
    static const QEvt e = QEVT_INITIALIZER(BOOT_EXIT_REQ_SIG);

    Mockkdrv_slot_Init();
    boot_fresh();

    /* No slot or otadata expectation is queued. The mock fails the case if
     * any of them is called, which is precisely the assertion. */
    if ( setjmp(*fake_system_arm_restart()) == 0 )
    {
        dispatch_through_leave(&e);
        TEST_FAIL_MESSAGE("the board should have reset");
    }

    TEST_ASSERT_EQUAL_UINT(1U, fake_system_restarts());
    mailboxes_drain();

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* ---- Answering the console ------------------------------------------------- */

/* `boot_info` is answered from whatever the drivers say, and the AO must not
 * fall over when a slot is absent - which is the state of a board that has
 * never been updated. */
TEST_CASE("boot info is answered even with no inactive slot", "[boot_ao]")
{
    static const QEvt e = QEVT_INITIALIZER(BOOT_INFO_REQ_SIG);

    Mockkdrv_slot_Init();
    boot_fresh();

    kdrv_running_info_ExpectAnyArgs();
    kdrv_slot_present_IgnoreAndReturn(false);
    kdrv_slot_address_IgnoreAndReturn(0U);
    kdrv_slot_size_IgnoreAndReturn(0U);
    kdrv_slot_desc_IgnoreAndReturn(KODE_ERROR);
    kdrv_otadata_blank_IgnoreAndReturn(true);

    QASM_DISPATCH(AO_Boot, &e, 0U);

    TEST_ASSERT_EQUAL_UINT(0U, fake_system_restarts());

    Mockkdrv_slot_Destroy();
}

/* An unknown signal costs nothing: no driver call, no reset. */
TEST_CASE("a signal AO_Boot does not handle changes nothing", "[boot_ao]")
{
    static const QEvt e = QEVT_INITIALIZER(LED_SET_SIG);

    Mockkdrv_slot_Init();
    boot_fresh();

    QASM_DISPATCH(AO_Boot, &e, 0U);

    TEST_ASSERT_EQUAL_UINT(0U, fake_system_restarts());

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* ---- The hold: home, with a grace ---------------------------------------- */

/* The gate is Kconfig, and on the board it depends on the APP role - kodeOS
 * itself is home already. This suite spells CONFIG_KODEOS_HOME_HOLD out
 * beside CONFIG_KODEOS_BOOT_ROLE_OS in its own CMakeLists precisely so the
 * app's half can be driven here, and without the symbol the whole block
 * compiles out SILENTLY: the three cases below would still run, still pass,
 * and assert about absent code. Hence a case rather than a comment. */
TEST_CASE("the build really has the home hold compiled in", "[boot_ao]")
{
#if !CONFIG_KODEOS_HOME_HOLD
    TEST_FAIL_MESSAGE("CONFIG_KODEOS_HOME_HOLD is not set: the grace, the two "
                      "facade calls and the restart compiled out, and the "
                      "cases below prove nothing");
#endif
    TEST_PASS();
}

/** @brief The bottom button held, exactly as AO_Buttons publishes it. */
static void hold_bot(void)
{
    ButtonHoldEvt h;
    h.super = (QEvt)QEVT_INITIALIZER(BUTTON_HOLD_SIG);
    h.id    = (uint8_t)KDRV_BTN_BOT;
    QASM_DISPATCH(AO_Boot, &h.super, 0U);
}

/**
 * @brief Tick until the restart lands, from inside an armed landing point.
 *
 * The restart fake leaves by longjmp and these restarts happen on a TIMER, so
 * the jump crosses qf_support_tick() - which holds a critical section for the
 * length of its loop. qf_support_tick_escaped() is what balances it; without
 * that call the host port's signals stay blocked for the rest of the binary.
 *
 * @param ticks How many QP ticks to advance before giving up.
 */
static void tick_to_restart(unsigned ticks)
{
    if ( setjmp(*fake_system_arm_restart()) == 0 )
    {
        (void)qf_support_tick(AO_Boot, ticks);
        TEST_FAIL_MESSAGE("the board should have reset");
    }
    qf_support_tick_escaped();
}

/* The gesture every app inherits without cooperating. What is new beside
 * Phase 1 is the announcement and the pause: the hold goes out on the bus
 * first, so an app's own AO can answer it, and only then does the grace run
 * down to the leave that ends in the reset. */
TEST_CASE("a held bottom button announces home and leaves after the grace",
          "[boot_ao]")
{
    Mockkdrv_slot_Init();
    boot_fresh();
    ao_spy_drain();
    ao_spy_subscribe(HOME_SIG);

    hold_bot();

    TEST_ASSERT_EQUAL_MESSAGE(HOME_SIG, ao_spy_next_sig(),
        "the hold is announced before the board goes, not after");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_system_restarts(),
        "the grace is the whole point: nothing resets in the same tick");
    TEST_ASSERT_TRUE_MESSAGE(boot_home_pending(),
        "and an app asking now would be heard");

    TEST_ASSERT_EQUAL_UINT(0U,
        qf_support_tick(AO_Boot, QF_SUPPORT_STEPS(KODE_SYS_HOME_GRACE_MS) - 1U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_system_restarts(),
        "one tick short of the grace is still short of it");

    (void)qf_support_tick(AO_Boot, 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_system_restarts(),
        "the expiry begins the leave; the reset is the leave's end");
    TEST_ASSERT_EQUAL_INT((int)NET_LEAVE_REQ_SIG, mailbox_take(AO_Net, NULL));
    answer_net_left();
    finish_leave();
    TEST_ASSERT_EQUAL_UINT(1U, fake_system_restarts());

    ao_spy_drain();
    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* The deal the grace exists for: an app that has state to save says so, and
 * decides itself when it is ready. */
TEST_CASE("a deferral holds the reset until the app says go", "[boot_ao]")
{
    Mockkdrv_slot_Init();
    boot_fresh();

    hold_bot();
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_home_defer(),
        "the facade takes the deferral while a hold is pending");

    (void)qf_support_tick(AO_Boot,
                          QF_SUPPORT_STEPS(KODE_SYS_HOME_GRACE_MS) + 10U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_system_restarts(),
        "the grace expired and the board waited, because it was asked to");

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_home_now());
    (void)qf_support_tick(AO_Boot, 1U);   /* HOME_NOW arrives: the leave begins */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_system_restarts(),
        "going home now leaves first, it does not reset in the same step");
    if ( setjmp(*fake_system_arm_restart()) == 0 )
    {
        answer_net_left();
        answer_radio_parked();
        TEST_FAIL_MESSAGE("the board should have reset");
    }
    TEST_ASSERT_EQUAL_UINT(1U, fake_system_restarts());
    mailboxes_drain();

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* The cap, which is what keeps the deferral from being a way out. The grace
 * is for saving state, not for thinking: an app that asks for a moment and
 * never comes back is reset anyway, on AO_Boot's clock rather than its own. */
TEST_CASE("a deferral that never resolves still comes home at the cap",
          "[boot_ao]")
{
    Mockkdrv_slot_Init();
    boot_fresh();

    hold_bot();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_home_defer());

    (void)qf_support_tick(AO_Boot,
                          QF_SUPPORT_STEPS(KODE_SYS_HOME_DEFER_MAX_MS) - 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_system_restarts(),
        "one tick short of the cap, the app still has the board");
    TEST_ASSERT_FALSE_MESSAGE(boot_home_pending(),
        "and a second deferral cannot extend what is already extended");

    (void)qf_support_tick(AO_Boot, 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_system_restarts(),
        "the cap begins the leave; the reset is the leave's end");
    TEST_ASSERT_EQUAL_INT((int)NET_LEAVE_REQ_SIG, mailbox_take(AO_Net, NULL));
    answer_net_left();
    finish_leave();
    TEST_ASSERT_EQUAL_UINT(1U, fake_system_restarts());

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* ---- Leaving: the network and the C5 first, then the reset ------------------ */

/** @brief The Open button's request, as the launcher posts it. */
static BootRunEvt run_once(void)
{
    BootRunEvt e = { .super = QEVT_INITIALIZER(BOOT_APP_RUN_SIG),
                     .offset = KAPPS_DATA_OFF, .size = 0x8000U,
                     .mode = KBOOT_RUN_ONCE };
    return e;
}

/** @brief Whether the apps region holds a boot request in the state NEW. */
static bool new_request_on_flash(void)
{
    kapps_req_t req;

    if ( esp_partition_read(boot_region(), KAPPS_REQ_OFF, &req, sizeof(req))
        != ESP_OK )
    {
        return false;
    }
    return KAPPS_REQ_STATE_NEW == req.state;
}

TEST_CASE("app_run writes the request, leaves, then resets", "[boot_ao]")
{
    BootRunEvt e = run_once();
    uint8_t    target = 0xFFU;

    Mockkdrv_slot_Init();
    boot_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_init());

    QASM_DISPATCH(AO_Boot, &e.super, 0U);

    TEST_ASSERT_TRUE_MESSAGE(new_request_on_flash(),
        "the request is written before anything is asked of the neighbours");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_system_restarts(),
        "the leave comes first: no reset yet");
    TEST_ASSERT_EQUAL_INT((int)NET_LEAVE_REQ_SIG, mailbox_take(AO_Net, NULL));
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_take(AO_Radio, NULL),
        "the radio is asked after the network, not with it");

    answer_net_left();
    TEST_ASSERT_EQUAL_INT((int)RADIO_PARK_REQ_SIG,
                          mailbox_take(AO_Radio, &target));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_RADIO_NOTE_APP, target);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_system_restarts(),
        "the C5 has not answered yet");

    finish_leave();
    TEST_ASSERT_EQUAL_UINT(1U, fake_system_restarts());

    kdrv_apps_req_clear();
    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("kode_sys_exit_to_os posts its request and returns, then parks for the OS",
          "[boot_ao]")
{
    static const QEvt exit_evt = QEVT_INITIALIZER(BOOT_EXIT_REQ_SIG);
    uint8_t target = 0xFFU;

    Mockkdrv_slot_Init();
    boot_fresh();

    qf_support_margin_queue(AO_Boot);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_exit_to_os());
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)BOOT_EXIT_REQ_SIG, mailbox_take(AO_Boot, NULL),
        "the command is in AO_Boot's queue once it returns");
    TEST_ASSERT_EQUAL_INT(-1, mailbox_take(AO_Boot, NULL));
    TEST_ASSERT_EQUAL_UINT(0U, fake_system_restarts());

    QASM_DISPATCH(AO_Boot, &exit_evt, 0U);
    TEST_ASSERT_EQUAL_INT((int)NET_LEAVE_REQ_SIG, mailbox_take(AO_Net, NULL));
    answer_net_left();
    TEST_ASSERT_EQUAL_INT((int)RADIO_PARK_REQ_SIG,
                          mailbox_take(AO_Radio, &target));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_RADIO_NOTE_OS, target);
    TEST_ASSERT_EQUAL_UINT(0U, fake_system_restarts());

    finish_leave();
    TEST_ASSERT_EQUAL_UINT(1U, fake_system_restarts());
    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("HOME now parks for the OS", "[boot_ao]")
{
    static const QEvt now_evt = QEVT_INITIALIZER(BOOT_HOME_NOW_SIG);
    uint8_t target = 0xFFU;

    Mockkdrv_slot_Init();
    boot_fresh();

    hold_bot();
    QASM_DISPATCH(AO_Boot, &now_evt, 0U);
    TEST_ASSERT_EQUAL_INT((int)NET_LEAVE_REQ_SIG, mailbox_take(AO_Net, NULL));
    answer_net_left();
    TEST_ASSERT_EQUAL_INT((int)RADIO_PARK_REQ_SIG,
                          mailbox_take(AO_Radio, &target));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_RADIO_NOTE_OS, target);
    TEST_ASSERT_EQUAL_UINT(0U, fake_system_restarts());

    finish_leave();
    TEST_ASSERT_EQUAL_UINT(1U, fake_system_restarts());
    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("install dst=os parks for the OS after the slot is selected",
          "[boot_ao]")
{
    static const QEvt e = QEVT_INITIALIZER(BOOT_OS_SWITCH_SIG);
    uint8_t target = 0xFFU;

    Mockkdrv_slot_Init();
    boot_fresh();

    kdrv_slot_present_ExpectAndReturn(true);
    kdrv_slot_has_image_ExpectAndReturn(true);
    kdrv_otadata_select_slot_ExpectAndReturn(KODE_OK);
    QASM_DISPATCH(AO_Boot, &e, 0U);
    TEST_ASSERT_EQUAL_UINT(0U, fake_system_restarts());
    TEST_ASSERT_EQUAL_INT((int)NET_LEAVE_REQ_SIG, mailbox_take(AO_Net, NULL));
    answer_net_left();
    TEST_ASSERT_EQUAL_INT((int)RADIO_PARK_REQ_SIG,
                          mailbox_take(AO_Radio, &target));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_RADIO_NOTE_OS, target);

    finish_leave();
    TEST_ASSERT_EQUAL_UINT(1U, fake_system_restarts());
    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("a refused slot switch starts no leave", "[boot_ao]")
{
    static const QEvt e = QEVT_INITIALIZER(BOOT_OS_SWITCH_SIG);

    Mockkdrv_slot_Init();
    boot_fresh();

    kdrv_slot_present_ExpectAndReturn(true);
    kdrv_slot_has_image_ExpectAndReturn(true);
    kdrv_otadata_select_slot_ExpectAndReturn(KODE_ERROR);
    QASM_DISPATCH(AO_Boot, &e, 0U);

    TEST_ASSERT_EQUAL_UINT(0U, fake_system_restarts());
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_take(AO_Net, NULL),
        "a refused switch has nothing to leave for");
    TEST_ASSERT_EQUAL_INT(-1, mailbox_take(AO_Radio, NULL));

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("a silent AO_Net does not hold the jump", "[boot_ao]")
{
    static const QEvt e = QEVT_INITIALIZER(BOOT_EXIT_REQ_SIG);

    Mockkdrv_slot_Init();
    boot_fresh();

    QASM_DISPATCH(AO_Boot, &e, 0U);
    TEST_ASSERT_EQUAL_INT((int)NET_LEAVE_REQ_SIG, mailbox_take(AO_Net, NULL));

    (void)qf_support_tick(AO_Boot, QF_SUPPORT_STEPS(KODE_LEAVE_NET_MS) - 1U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_take(AO_Radio, NULL),
        "one tick short of the bound, the network still has the floor");

    (void)qf_support_tick(AO_Boot, 1U);
    TEST_ASSERT_EQUAL_INT((int)RADIO_PARK_REQ_SIG,
                          mailbox_take(AO_Radio, NULL));
    TEST_ASSERT_EQUAL_UINT(0U, fake_system_restarts());

    finish_leave();
    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("a silent AO_Radio does not hold the jump", "[boot_ao]")
{
    static const QEvt e = QEVT_INITIALIZER(BOOT_EXIT_REQ_SIG);

    Mockkdrv_slot_Init();
    boot_fresh();

    QASM_DISPATCH(AO_Boot, &e, 0U);
    answer_net_left();
    mailboxes_drain();

    TEST_ASSERT_EQUAL_UINT(0U,
        qf_support_tick(AO_Boot, QF_SUPPORT_STEPS(KODE_LEAVE_RADIO_MS) - 1U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_system_restarts(),
        "one tick short of the bound, the C5 still has the floor");

    tick_to_restart(1U);
    TEST_ASSERT_EQUAL_UINT(1U, fake_system_restarts());

    mailboxes_drain();
    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("a second restart request while leaving is dropped", "[boot_ao]")
{
    BootRunEvt e = run_once();
    static const QEvt sw   = QEVT_INITIALIZER(BOOT_OS_SWITCH_SIG);
    static const QEvt exit_evt = QEVT_INITIALIZER(BOOT_EXIT_REQ_SIG);
    unsigned before;

    Mockkdrv_slot_Init();
    boot_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_init());

    QASM_DISPATCH(AO_Boot, &e.super, 0U);
    TEST_ASSERT_TRUE(new_request_on_flash());
    TEST_ASSERT_EQUAL_INT((int)NET_LEAVE_REQ_SIG, mailbox_take(AO_Net, NULL));

    /* The first request wins: take its record away, so a second write shows. */
    kdrv_apps_req_clear();
    before = fake_klog_count();

    QASM_DISPATCH(AO_Boot, &e.super, 0U);       /* a double tap on Open */
    QASM_DISPATCH(AO_Boot, &exit_evt, 0U);      /* BOT held meanwhile */
    QASM_DISPATCH(AO_Boot, &sw, 0U);            /* no slot call is expected */

    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(before, fake_klog_count(),
        "a dropped request says so");
    TEST_ASSERT_FALSE_MESSAGE(kdrv_apps_req_present(),
        "the second app run wrote nothing");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_take(AO_Net, NULL),
        "and asked the network for nothing");
    TEST_ASSERT_EQUAL_UINT(0U, fake_system_restarts());

    answer_net_left();
    TEST_ASSERT_EQUAL_INT((int)RADIO_PARK_REQ_SIG, mailbox_take(AO_Radio, NULL));
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_take(AO_Radio, NULL),
        "one park request, not three");

    finish_leave();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_system_restarts(),
        "exactly one reset");

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("the HOME grace expiry leaves, then resets", "[boot_ao]")
{
    uint8_t target = 0xFFU;

    Mockkdrv_slot_Init();
    boot_fresh();

    hold_bot();
    TEST_ASSERT_EQUAL_UINT(0U,
        qf_support_tick(AO_Boot, QF_SUPPORT_STEPS(KODE_SYS_HOME_GRACE_MS) - 1U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_take(AO_Net, NULL),
        "one tick short of the grace nothing is asked of the network");
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_Boot, 1U));

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_system_restarts(),
        "the grace expiry begins the leave, it does not reset in the same step");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)NET_LEAVE_REQ_SIG,
                                  mailbox_take(AO_Net, NULL),
        "the network is asked to leave cleanly");
    answer_net_left();
    TEST_ASSERT_EQUAL_INT((int)RADIO_PARK_REQ_SIG,
                          mailbox_take(AO_Radio, &target));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KDRV_RADIO_NOTE_OS, target,
        "BOT held goes home to kodeOS");

    finish_leave();
    TEST_ASSERT_EQUAL_UINT(1U, fake_system_restarts());

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("a silent neighbour does not hold the grace expiry's leave",
          "[boot_ao]")
{
    Mockkdrv_slot_Init();
    boot_fresh();

    hold_bot();
    (void)qf_support_tick(AO_Boot, QF_SUPPORT_STEPS(KODE_SYS_HOME_GRACE_MS));
    TEST_ASSERT_EQUAL_INT((int)NET_LEAVE_REQ_SIG, mailbox_take(AO_Net, NULL));

    /* Neither AO answers: the two bounds run out and the board resets. */
    (void)qf_support_tick(AO_Boot, QF_SUPPORT_STEPS(KODE_LEAVE_NET_MS));
    TEST_ASSERT_EQUAL_INT((int)RADIO_PARK_REQ_SIG, mailbox_take(AO_Radio, NULL));
    tick_to_restart(QF_SUPPORT_STEPS(KODE_LEAVE_RADIO_MS));
    TEST_ASSERT_EQUAL_UINT(1U, fake_system_restarts());

    mailboxes_drain();
    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("a grace or a hold during a leave starts nothing new", "[boot_ao]")
{
    static const QEvt exit_evt = QEVT_INITIALIZER(BOOT_EXIT_REQ_SIG);
    static const QEvt grace    = QEVT_INITIALIZER(BOOT_HOME_GRACE_SIG);
    static const QEvt defer    = QEVT_INITIALIZER(BOOT_HOME_DEFER_SIG);
    unsigned          before;

    Mockkdrv_slot_Init();
    boot_fresh();

    QASM_DISPATCH(AO_Boot, &exit_evt, 0U);
    TEST_ASSERT_EQUAL_INT((int)NET_LEAVE_REQ_SIG, mailbox_take(AO_Net, NULL));
    before = fake_klog_count();

    QASM_DISPATCH(AO_Boot, &grace, 0U);     /* the timer ran out meanwhile */
    QASM_DISPATCH(AO_Boot, &defer, 0U);     /* an app asking for a moment */
    hold_bot();                             /* BOT held again */

    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(before, fake_klog_count(),
        "a dropped event says so");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_system_restarts(),
        "the grace does not reset a board that is already leaving");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_take(AO_Net, NULL),
        "and asks the network for nothing");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_take(AO_Radio, NULL),
        "nor the radio, which is asked after the network");
    TEST_ASSERT_FALSE_MESSAGE(boot_home_pending(),
        "a hold during the leave arms no new grace");

    answer_net_left();
    TEST_ASSERT_EQUAL_INT((int)RADIO_PARK_REQ_SIG, mailbox_take(AO_Radio, NULL));
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_take(AO_Radio, NULL),
        "one park request");

    finish_leave();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_system_restarts(),
        "exactly one reset");

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("a late answer after the reset step is ignored", "[boot_ao]")
{
    static const QEvt e   = QEVT_INITIALIZER(BOOT_EXIT_REQ_SIG);
    static const QEvt tmo = QEVT_INITIALIZER(BOOT_LEAVE_TMO_SIG);

    Mockkdrv_slot_Init();
    boot_fresh();

    QASM_DISPATCH(AO_Boot, &e, 0U);
    answer_net_left();
    TEST_ASSERT_EQUAL_INT((int)NET_LEAVE_REQ_SIG, mailbox_take(AO_Net, NULL));
    TEST_ASSERT_EQUAL_INT((int)RADIO_PARK_REQ_SIG, mailbox_take(AO_Radio, NULL));

    answer_net_left();                       /* a second, late answer */
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_take(AO_Radio, NULL),
        "the radio is not asked twice");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_system_restarts(),
        "a network answer does not end the radio's step");

    QASM_DISPATCH(AO_Boot, &tmo, 0U);        /* the network step's timeout,
                                              * posted before its answer came */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_system_restarts(),
        "nor does the timeout of the step before");

    finish_leave();
    TEST_ASSERT_EQUAL_UINT(1U, fake_system_restarts());

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* An app that never linked AO_Net or AO_Radio has nobody to ask on the way
 * out: AO_Boot names both weakly, and the leave resets at once, with no
 * answer to wait for and nothing posted. */
TEST_CASE("an app with neither AO_Net nor AO_Radio leaves at once", "[boot_ao]")
{
    static const QEvt e = QEVT_INITIALIZER(BOOT_EXIT_REQ_SIG);

    Mockkdrv_slot_Init();
    boot_fresh();
    mailboxes_drain();
    fake_kaos_absent(&kaos_net_start);
    fake_kaos_absent(&kaos_radio_start);

    if ( setjmp(*fake_system_arm_restart()) == 0 )
    {
        QASM_DISPATCH(AO_Boot, &e, 0U);
        TEST_FAIL_MESSAGE("the board should have reset");
    }

    TEST_ASSERT_EQUAL_UINT(1U, fake_system_restarts());
    TEST_ASSERT_EQUAL_INT(-1, mailbox_take(AO_Net, NULL));
    TEST_ASSERT_EQUAL_INT(-1, mailbox_take(AO_Radio, NULL));

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* ---- The time zone ----------------------------------------------------------
 * The zone is the system's, AO_Boot's in both roles: it applies the stored
 * one in its initial transition, before any app code runs and before it
 * publishes anything, and it answers `config key="zone"`. What each case
 * asserts is the local time of one moment, 2026-07-01T10:00:00Z, a
 * Wednesday: 10:00 in UTC, 12:00 in Madrid's summer time. */

/** @brief 2026-07-01T10:00:00Z, in seconds. */
#define ZONE_TEST_S        1782900000LL
/** @brief Madrid and Paris, daylight saving included. */
#define ZONE_TEST_MADRID   "CET-1CEST,M3.5.0,M10.5.0/3"
/** @brief The local hour of ZONE_TEST_S in UTC. */
#define ZONE_TEST_HOUR_UTC     10
/** @brief The local hour of ZONE_TEST_S in ZONE_TEST_MADRID. */
#define ZONE_TEST_HOUR_MADRID  12

/** @brief The local hour of ZONE_TEST_S in the zone the C library runs in now. */
static int zone_hour_now(void)
{
    struct tm t;

    TEST_ASSERT_TRUE(khal_time_local(ZONE_TEST_S, &t));
    return t.tm_hour;
}

/** @brief Put the process back in UTC, for the cases after these. */
static void zone_back_to_utc(void)
{
    TEST_ASSERT_TRUE(khal_time_zone_set(KSET_ZONE_DEF));
    QActive_unsubscribe(ao_spy(), CONFIG_ANS_SIG);
    ao_spy_drain();
}

TEST_CASE("a stored zone is applied in the initial transition, before AO_Boot "
          "publishes anything", "[boot_ao]")
{
    const KodeConfigAnsEvt *a;

    Mockkdrv_slot_Init();
    TEST_ASSERT_TRUE(khal_time_zone_set(KSET_ZONE_DEF));
    fake_nvs_seed(KSET_NAMESPACE, KSET_KEY_ZONE, ZONE_TEST_MADRID,
                  sizeof(ZONE_TEST_MADRID));
    ao_spy_drain();
    ao_spy_subscribe(BOOT_STATE_SIG);
    ao_spy_subscribe(HOME_SIG);
    ao_spy_subscribe(CONFIG_ANS_SIG);

    boot_fresh();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "the initial transition published something");
    TEST_ASSERT_EQUAL_INT(ZONE_TEST_HOUR_MADRID, zone_hour_now());

    a = config_ask(AO_Boot, &(config_ask_t){ .key = KSET_KEY_ZONE });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, a->status);
    TEST_ASSERT_EQUAL_STRING(ZONE_TEST_MADRID, a->value);
    TEST_ASSERT_TRUE(a->stored);

    QActive_unsubscribe(ao_spy(), BOOT_STATE_SIG);
    QActive_unsubscribe(ao_spy(), HOME_SIG);
    zone_back_to_utc();
    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("with no zone stored AO_Boot applies UTC0 and says so", "[boot_ao]")
{
    const KodeConfigAnsEvt *a;

    Mockkdrv_slot_Init();
    TEST_ASSERT_TRUE(khal_time_zone_set(ZONE_TEST_MADRID));

    boot_fresh();

    TEST_ASSERT_EQUAL_INT(ZONE_TEST_HOUR_UTC, zone_hour_now());
    a = config_ask(AO_Boot, &(config_ask_t){ .key = KSET_KEY_ZONE });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, a->status);
    TEST_ASSERT_EQUAL_STRING("UTC0", a->value);
    TEST_ASSERT_FALSE(a->stored);

    zone_back_to_utc();
    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("config key=zone value= stores the zone and applies it at once",
          "[boot_ao]")
{
    const KodeConfigAnsEvt *a;
    char kept[KSET_ZONE_MAX + 1U];

    Mockkdrv_slot_Init();
    boot_fresh();

    a = config_ask(AO_Boot, &(config_ask_t){ .key = KSET_KEY_ZONE, .set = true,
                                             .s = ZONE_TEST_MADRID });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, a->status);
    TEST_ASSERT_EQUAL_STRING(ZONE_TEST_MADRID, a->value);
    TEST_ASSERT_TRUE(a->stored);
    TEST_ASSERT_EQUAL_INT(ZONE_TEST_HOUR_MADRID, zone_hour_now());
    TEST_ASSERT_EQUAL(KODE_OK, khal_nvs_get_str(KSET_NAMESPACE, KSET_KEY_ZONE,
                                                kept, sizeof(kept)));
    TEST_ASSERT_EQUAL_STRING(ZONE_TEST_MADRID, kept);

    /* A read answers the zone in force, stored. */
    a = config_ask(AO_Boot, &(config_ask_t){ .key = KSET_KEY_ZONE });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING(ZONE_TEST_MADRID, a->value);
    TEST_ASSERT_TRUE(a->stored);

    zone_back_to_utc();
    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* The console holds a string to 32 characters before it asks anyone, so a
 * 33-character zone reaches AO_Boot only in a request that fills the whole
 * value with no terminator: AO_Boot judges the length itself rather than
 * trusting the sender. A line break (the console's `\n` escape) or a byte
 * beyond ASCII is no zone either. Every refusal is KODE_DENIED, which the
 * console prints as `>err code=5 msg=value`, and leaves the zone in force,
 * the store and the clock as they were. */
TEST_CASE("a zone of 33 characters, an empty one or a non-printable one is "
          "refused and changes nothing", "[boot_ao]")
{
    static const char *const BAD[] = { "", "UTC\n0", "CET-1\x7f", "CET\xc3\xa9-1" };
    KodeConfigReqEvt         r;
    const KodeConfigAnsEvt  *a;
    unsigned                 writes;

    Mockkdrv_slot_Init();
    boot_fresh();
    writes = fake_nvs_set_count();
    ao_spy_drain();
    ao_spy_subscribe(CONFIG_ANS_SIG);

    (void)memset(&r, 0, sizeof(r));
    r.super = (QEvt)QEVT_INITIALIZER(CONFIG_REQ_SIG);
    r.set   = true;
    (void)snprintf(r.key, sizeof(r.key), "%s", KSET_KEY_ZONE);
    (void)memset(r.s, 'A', sizeof(r.s));
    TEST_ASSERT_EQUAL_size_t(KSET_ZONE_MAX + 1U, sizeof(r.s));
    QASM_DISPATCH(AO_Boot, &r.super, 0U);
    a = (const KodeConfigAnsEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)CONFIG_ANS_SIG, (uint32_t)a->super.sig);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KODE_DENIED, a->status,
        "a zone of 33 characters was taken");
    TEST_ASSERT_EQUAL_STRING("UTC0", a->value);

    for ( size_t i = 0U; (sizeof(BAD) / sizeof(BAD[0])) > i; ++i )
    {
        a = config_ask(AO_Boot, &(config_ask_t){ .key = KSET_KEY_ZONE,
                                                 .set = true, .s = BAD[i] });
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KODE_DENIED, a->status, BAD[i]);
        TEST_ASSERT_EQUAL_STRING("UTC0", a->value);
        TEST_ASSERT_FALSE(a->stored);
    }

    TEST_ASSERT_EQUAL_UINT_MESSAGE(writes, fake_nvs_set_count(),
        "a refused zone was written to the store");
    TEST_ASSERT_EQUAL_INT(ZONE_TEST_HOUR_UTC, zone_hour_now());

    zone_back_to_utc();
    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* A zone is a POSIX TZ string, not any printable text. The first table is
 * every row of the Time zone page and the forms the grammar allows besides;
 * the second is text that is no zone and the ways a string falls short of the
 * grammar. */
TEST_CASE("every zone of the Time zone page is taken, a string that is no "
          "POSIX TZ is refused", "[boot_ao]")
{
    static const char *const GOOD[] = {
        "HST10", "PST8PDT,M3.2.0,M11.1.0", "MST7MDT,M3.2.0,M11.1.0",
        "CST6CDT,M3.2.0,M11.1.0", "CST6", "EST5EDT,M3.2.0,M11.1.0", "<-05>5",
        "<-03>3", "UTC0", "GMT0BST,M3.5.0/1,M10.5.0",
        "CET-1CEST,M3.5.0,M10.5.0/3", "EET-2EEST,M3.5.0/3,M10.5.0/4", "SAST-2",
        "<+03>-3", "<+04>-4", "IST-5:30", "<+07>-7", "<+08>-8", "JST-9",
        "AEST-10AEDT,M10.1.0,M4.1.0/3", "NZST-12NZDT,M9.5.0,M4.1.0/3",
        /* The forms the page does not use. */
        "EST+5", "XXX24", "XXX-24:59:59", "<+0530>-5:30:15",
        "CET-1CEST", "CET-1CEST-2", "CET-1CEST0,J1,J365",
        "CET-1CEST,0,365/-1", "CET-1CEST,M12.5.6/167,M1.1.0/0",
        "CET-1CEST,J1/0:59:59,J365",
    };
    static const char *const BAD[] = {
        /* Text that is no zone. */
        "xyz", "A", "12345", "<<<", "\\q", "ABC-100", "UTC+99",
        /* The name. */
        "AB1", "UT", "<>5", "<ab>5", "<+03-3", "<+0 3>-3", " UTC0", "UTC 0",
        /* The offset. */
        "UTC", "UTC+", "UTC25", "UTC-24:60", "UTC0:", "UTC0:00:60", "UTC0:0:0:0",
        "UTC1000", "UTC.5",
        /* The second name and the rules. */
        "CET-1C", "CET-1CEST,", "CET-1CEST,M3.5.0", "CET-1CEST,M3.5.0,",
        "CET-1CEST,M3.5.0,M10.5.0,M1.1.1", "CET-1CEST,M3.5.0,M10.5.0x",
        "CET-1,M3.5.0,M10.5.0", "CET-1CEST!", "CET-1CEST,J0,J1",
        "CET-1CEST,J366,J1", "CET-1CEST,366,1", "CET-1CEST,M0.1.0,M1.1.0",
        "CET-1CEST,M13.1.0,M1.1.0", "CET-1CEST,M3.0.0,M10.5.0",
        "CET-1CEST,M3.6.0,M10.5.0", "CET-1CEST,M3.5.7,M10.5.0",
        "CET-1CEST,M3.5,M10.5.0", "CET-1CEST,M3.5.0/,M10.5.0",
        "CET-1CEST,M3.5.0/168,M10.5.0", "CET-1CEST,M3.5.0/1:60,M10.5.0",
    };
    const KodeConfigAnsEvt *a;

    Mockkdrv_slot_Init();
    boot_fresh();

    for ( size_t i = 0U; (sizeof(GOOD) / sizeof(GOOD[0])) > i; ++i )
    {
        a = config_ask(AO_Boot, &(config_ask_t){ .key = KSET_KEY_ZONE,
                                                 .set = true, .s = GOOD[i] });
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KODE_OK, a->status, GOOD[i]);
        TEST_ASSERT_EQUAL_STRING(GOOD[i], a->value);
        ao_spy_drain();   /* an answer is an event of the pool */
    }

    a = config_ask(AO_Boot, &(config_ask_t){ .key = KSET_KEY_ZONE,
                                             .set = true, .s = ZONE_TEST_MADRID });
    TEST_ASSERT_NOT_NULL(a);
    ao_spy_drain();
    for ( size_t i = 0U; (sizeof(BAD) / sizeof(BAD[0])) > i; ++i )
    {
        a = config_ask(AO_Boot, &(config_ask_t){ .key = KSET_KEY_ZONE,
                                                 .set = true, .s = BAD[i] });
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KODE_DENIED, a->status, BAD[i]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(ZONE_TEST_MADRID, a->value, BAD[i]);
        TEST_ASSERT_TRUE(a->stored);
        ao_spy_drain();
    }
    TEST_ASSERT_EQUAL_INT(ZONE_TEST_HOUR_MADRID, zone_hour_now());

    zone_back_to_utc();
    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* The longest zone is 32 characters, the whole of a setting's value. */
TEST_CASE("a zone of exactly 32 characters is taken whole", "[boot_ao]")
{
    /* Auckland, its change of hour spelt out to the minute. */
    static const char zone[] = "NZST-12NZDT,M9.5.0/2,M4.1.0/3:00";
    const KodeConfigAnsEvt *a;

    TEST_ASSERT_EQUAL_size_t(KSET_ZONE_MAX, strlen(zone));

    Mockkdrv_slot_Init();
    boot_fresh();
    a = config_ask(AO_Boot, &(config_ask_t){ .key = KSET_KEY_ZONE, .set = true,
                                             .s = zone });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, a->status);
    TEST_ASSERT_EQUAL_STRING(zone, a->value);

    zone_back_to_utc();
    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* A zone the store refuses is not half taken: the clock stays where it was,
 * so the next boot and this one agree. */
TEST_CASE("a zone the store refuses is not applied", "[boot_ao]")
{
    const KodeConfigAnsEvt *a;

    Mockkdrv_slot_Init();
    boot_fresh();
    fake_nvs_fail_next_set();
    a = config_ask(AO_Boot, &(config_ask_t){ .key = KSET_KEY_ZONE, .set = true,
                                             .s = ZONE_TEST_MADRID });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_DENIED, a->status);
    TEST_ASSERT_EQUAL_STRING("UTC0", a->value);
    TEST_ASSERT_FALSE(a->stored);
    TEST_ASSERT_EQUAL_INT(ZONE_TEST_HOUR_UTC, zone_hour_now());

    zone_back_to_utc();
    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* Another owner's key is another owner's to answer. */
TEST_CASE("AO_Boot answers no setting but the zone", "[boot_ao]")
{
    Mockkdrv_slot_Init();
    boot_fresh();
    TEST_ASSERT_NULL(config_ask(AO_Boot, &(config_ask_t){ .key = KSET_KEY_LOG }));

    zone_back_to_utc();
    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/* ---- The board's rest ------------------------------------------------------- */

TEST_CASE("a restart on its way refuses the round; a settled boot is ready",
          "[boot_ao][lowpower]")
{
    static const QEvt sw = QEVT_INITIALIZER(BOOT_OS_SWITCH_SIG);

    Mockkdrv_slot_Init();
    boot_fresh();

    lp_round(AO_Boot, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();

    kdrv_slot_present_ExpectAndReturn(true);
    kdrv_slot_has_image_ExpectAndReturn(true);
    kdrv_otadata_select_slot_ExpectAndReturn(KODE_OK);
    QASM_DISPATCH(AO_Boot, &sw, 0U);          /* the leave begins: AO_Net first */

    lp_round(AO_Boot, KODE_LP_OFF);
    lp_expect_answer(true, "restart pending");
    lp_expect_no_more();

    if ( setjmp(*fake_system_arm_restart()) == 0 )
    {
        answer_net_left();
        answer_radio_parked();
        TEST_FAIL_MESSAGE("the board should have reset");
    }
    mailboxes_drain();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("a home grace an app asked for refuses the round until it runs out",
          "[boot_ao][lowpower]")
{
    Mockkdrv_slot_Init();
    boot_fresh();
    hold_bot();                               /* the grace starts counting */

    lp_round(AO_Boot, KODE_LP_ASLEEP);
    lp_expect_answer(true, "home pending");

    (void)qf_support_tick(AO_Boot, QF_SUPPORT_STEPS(KODE_SYS_HOME_GRACE_MS));
    answer_net_left();
    finish_leave();
    mailboxes_drain();
    Mockkdrv_slot_Destroy();
}

/* ---- The factory reset ------------------------------------------------------ */

/* AO_AppMgr, AO_Radio and AO_Power are mailboxes here, never dispatched: what
 * AO_Boot asked of them is what their queues hold, and the case posts their
 * answers into AO_Boot by hand. The asker is the spy. Every case runs the
 * reset to its end, an answer or the restart, because a step's timer left
 * armed would be cut out of QF's list by the next case's constructor. */

/** @brief The welcome app's extent as the first find answers it. */
#define WELCOME_OFF   KAPPS_DATA_OFF
/** @brief The welcome app's length: the same as the other boot cases use. */
#define WELCOME_SIZE  0x8000U
/** @brief Where the find after an install finds it: a different extent. */
#define WELCOME_OFF2  (KAPPS_DATA_OFF + KAPPS_ALIGN)
/** @brief An offset past the region: the boot request cannot be written. */
#define OFF_PAST_REGION  0xFF000000U
/** @brief Namespaces the keep case seeds and the walk must leave. */
#define KEPT_LEFT     2U

/** @brief The next event in @p ao's queue, or NULL. The caller QF_gc()s it. */
static const QEvt *mailbox_event(QActive *ao)
{
    const QEvt *e = NULL;

    if ( (NULL == ao->eQueue) || (xQueueReceive(ao->eQueue, &e, 0U) != pdTRUE) )
    {
        return NULL;
    }
    return e;
}

/** @brief Empty a queue, releasing what it holds. */
static void queue_drained(QActive *ao)
{
    const QEvt *e;

    while ( (e = mailbox_event(ao)) != NULL )
    {
        QF_gc(e);
    }
}

/** @brief Empty the mailboxes of the four neighbours and the spy. */
static void reset_mailboxes_drained(void)
{
    queue_drained(AO_AppMgr);
    queue_drained(AO_Power);
    queue_drained(AO_Radio);
    queue_drained(AO_Net);
    ao_spy_drain();
}

/**
 * @brief Give @p ao the queue QP can read the free room of.
 *
 * A post with a margin - the ship arm's - is answered from the queue QP lays
 * out in the AO's own osObject. Another file's fake of a neighbour may have
 * given the AO a queue of the heap's, which QP reads as always full.
 *
 * @param ao The mailbox.
 */
static void queue_made_proper(QActive *ao)
{
    if ( (NULL != ao->eQueue)
        && ((void *)ao->eQueue != (void *)&ao->osObject.queue) )
    {
        queue_drained(ao);
        ao->eQueue = NULL;
    }
    qf_support_give_queue(ao);
}

/**
 * @brief AO_Boot up, the neighbours' queues in place and empty, no request
 *        on flash.
 */
static void reset_up(void)
{
    Mockkdrv_slot_Init();
    boot_fresh();
    qf_support_give_queue(AO_AppMgr);
    queue_made_proper(AO_Power);
    (void)ao_spy();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_init());
    kdrv_apps_req_clear();
    reset_mailboxes_drained();
}

/**
 * @brief Leave nothing behind for the files that sort after this one.
 *
 * AO_Boot subscribes to three published signals in the OS role, and a
 * subscription cannot be undone by constructing the AO again. Whatever other
 * files publish would then fill a queue nobody drains.
 */
static void reset_down(void)
{
    QActive_unsubscribe(AO_Boot, APP_FLASH_DONE_SIG);
    QActive_unsubscribe(AO_Boot, APP_FLASH_FAIL_SIG);
    QActive_unsubscribe(AO_Boot, RADIO_STATE_SIG);
    queue_drained(AO_Boot);
    reset_mailboxes_drained();
    kdrv_apps_req_clear();
    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

/** @brief Dispatch what waits in AO_Boot's own queue, until it is empty. */
static void reset_pump(void)
{
    const QEvt *e;

    while ( (e = mailbox_event(AO_Boot)) != NULL )
    {
        QASM_DISPATCH(AO_Boot, e, 0U);
        QF_gc(e);
    }
}

/** @brief Ask for a reset the way the console does, and let AO_Boot read it. */
static void reset_ask(kboot_reset_end_t end)
{
    TEST_ASSERT_TRUE(boot_factory_reset_request(end, ao_spy()));
    reset_pump();
}

/** @brief AO_Radio's state, as it publishes it. */
static void radio_state(kradio_link_t link)
{
    RadioStateEvt e = { .super = QEVT_INITIALIZER(RADIO_STATE_SIG),
                        .link  = (uint8_t)link };

    QASM_DISPATCH(AO_Boot, &e.super, 0U);
    reset_pump();
}

/** @brief AO_AppMgr's answer to the find. */
static void answer_find(bool installed, uint32_t off, uint32_t size,
                        bool on_card, bool busy)
{
    AppFindRspEvt e = { .super = QEVT_INITIALIZER(APP_FIND_RSP_SIG),
                        .installed = installed, .offset = off, .size = size,
                        .on_card = on_card, .busy = busy };

    (void)snprintf(e.project, sizeof(e.project), "%s",
                   CONFIG_KODEOS_WELCOME_APP);
    QASM_DISPATCH(AO_Boot, &e.super, 0U);
    reset_pump();
}

/** @brief AO_AppMgr's answer to the keep-only. */
static void answer_keep(kode_status_t status, uint8_t erased)
{
    AppKeepOnlyRspEvt e = { .super = QEVT_INITIALIZER(APP_KEEP_ONLY_RSP_SIG),
                            .status = (uint8_t)status, .erased = erased };

    QASM_DISPATCH(AO_Boot, &e.super, 0U);
    reset_pump();
}

/** @brief AO_Radio's answer to the wipe. */
static void answer_wipe(C5WipeStep step, uint8_t status)
{
    C5WipeRspEvt e = { .super = QEVT_INITIALIZER(C5_WIPE_RSP_SIG),
                       .step = (uint8_t)step, .status = status };

    QASM_DISPATCH(AO_Boot, &e.super, 0U);
    reset_pump();
}

/** @brief AO_AppMgr's published end of a write that installed @p name. */
static void flash_done(const char *name)
{
    AppFlashDoneEvt e = { .super = QEVT_INITIALIZER(APP_FLASH_DONE_SIG) };

    (void)snprintf(e.name, sizeof(e.name), "%s", name);
    QASM_DISPATCH(AO_Boot, &e.super, 0U);
    reset_pump();
}

/** @brief AO_AppMgr's published failure of a write. */
static void flash_fail(void)
{
    AppFlashFailEvt e = { .super = QEVT_INITIALIZER(APP_FLASH_FAIL_SIG),
                          .why = (uint8_t)KFAIL_WRITE };

    QASM_DISPATCH(AO_Boot, &e.super, 0U);
    reset_pump();
}

/** @brief AO_Power's word that the cable has been out long enough. */
static void ship_due(void)
{
    static const QEvt e = QEVT_INITIALIZER(POWER_SHIP_DUE_SIG);

    QASM_DISPATCH(AO_Boot, &e, 0U);
    reset_pump();
}

/** @brief What AO_AppMgr's next event asks, and for whom. */
static void expect_find_asked(void)
{
    const QEvt *e = mailbox_event(AO_AppMgr);

    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT((int)APP_FIND_REQ_SIG, (int)e->sig);
    TEST_ASSERT_EQUAL_STRING(CONFIG_KODEOS_WELCOME_APP,
                             Q_EVT_CAST(AppFindReqEvt)->project);
    TEST_ASSERT_TRUE(Q_EVT_CAST(AppFindReqEvt)->who == AO_Boot);
    QF_gc(e);
}

/** @brief AO_AppMgr was asked to keep only the welcome app. */
static void expect_keep_asked(void)
{
    const QEvt *e = mailbox_event(AO_AppMgr);

    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT((int)APP_KEEP_ONLY_REQ_SIG, (int)e->sig);
    TEST_ASSERT_EQUAL_STRING(CONFIG_KODEOS_WELCOME_APP,
                             Q_EVT_CAST(AppKeepOnlyReqEvt)->project);
    TEST_ASSERT_TRUE(Q_EVT_CAST(AppKeepOnlyReqEvt)->who == AO_Boot);
    QF_gc(e);
}

/** @brief The next signal in @p ao's queue, or -1. */
static int mailbox_sig(QActive *ao)
{
    return mailbox_take(ao, NULL);
}

/**
 * @brief Read the answer the spy holds.
 *
 * @param step   Receives FactoryResetRspEvt.step.
 * @param status Receives FactoryResetRspEvt.status.
 * @return false when the spy holds nothing.
 */
static bool reply_take(uint8_t *step, uint8_t *status)
{
    const QEvt *e = ao_spy_take();

    if ( NULL == e )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT((int)FACTORY_RESET_RSP_SIG, (int)e->sig);
    *step   = Q_EVT_CAST(FactoryResetRspEvt)->step;
    *status = Q_EVT_CAST(FactoryResetRspEvt)->status;
    ao_spy_release();
    return true;
}

/** @brief The one answer the spy holds must be (@p step, @p status). */
static void expect_reply(kboot_reset_step_t step, kode_status_t status)
{
    uint8_t got_step   = 0U;
    uint8_t got_status = 0U;

    TEST_ASSERT_TRUE_MESSAGE(reply_take(&got_step, &got_status),
                             "the reset should have answered");
    TEST_ASSERT_EQUAL_INT((int)step, (int)got_step);
    TEST_ASSERT_EQUAL_INT((int)status, (int)got_status);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)ao_spy_count(), "one answer only");
}

/** @brief The boot request as the bootloader will find it on flash. */
static void request_read(kapps_req_t *req)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_read(boot_region(), KAPPS_REQ_OFF, req, sizeof(*req)));
}

/**
 * @brief The namespaces the store still holds.
 *
 * @param names Receives them.
 * @param max   Room in @p names.
 * @return How many.
 */
static unsigned nvs_left(char names[][KHAL_NVS_NS_SIZE], unsigned max)
{
    unsigned n = 0U;

    while ( (n < max)
           && (khal_nvs_namespace((uint16_t)n, names[n], KHAL_NVS_NS_SIZE)
               == KODE_OK) )
    {
        ++n;
    }
    return n;
}

/** @brief Whether @p ns is among the first @p n of @p names. */
static bool nvs_has(char names[][KHAL_NVS_NS_SIZE], unsigned n, const char *ns)
{
    for ( unsigned i = 0U; i < n; ++i )
    {
        if ( strcmp(names[i], ns) == 0 )
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Walk a reset that finds the welcome app installed, up to the wipe.
 *
 * The link is cached up, so the wipe is asked at once: AO_Radio's mailbox
 * then holds it.
 *
 * @param end KBOOT_RESET_RESTART or KBOOT_RESET_SHIP.
 */
static void reset_to_wipe(kboot_reset_end_t end)
{
    radio_state(KRADIO_UP);
    reset_ask(end);
    expect_find_asked();
    answer_find(true, WELCOME_OFF, WELCOME_SIZE, false, false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_sig(AO_AppMgr),
        "no app goes before the C5 has answered");
    TEST_ASSERT_EQUAL_INT((int)C5_WIPE_REQ_SIG, mailbox_sig(AO_Radio));
    TEST_ASSERT_EQUAL_INT(-1, mailbox_sig(AO_Radio));
}

/** @brief The wipe says it is done, and the other apps go. Leaves the NVS step
 *         to run, and with it the end of the reset. */
static void wipe_then_apps(void)
{
    answer_wipe(C5_WIPE_DONE, KODE_ST_OK);
    expect_keep_asked();
    answer_keep(KODE_OK, 2U);
}

/** @brief Put one key in each of @p ns's names so the store holds them. */
static void nvs_seed_all(const char *const *ns, unsigned n)
{
    static const uint8_t one = 1U;

    for ( unsigned i = 0U; i < n; ++i )
    {
        fake_nvs_seed(ns[i], "k", &one, sizeof(one));
    }
}

/** @brief The step names the console prints. */
TEST_CASE("the reset's steps have the words the console prints", "[boot_ao][reset]")
{
    TEST_ASSERT_EQUAL_STRING("find",    boot_reset_step_name(KBOOT_RESET_STEP_FIND));
    TEST_ASSERT_EQUAL_STRING("install", boot_reset_step_name(KBOOT_RESET_STEP_INSTALL));
    TEST_ASSERT_EQUAL_STRING("apps",    boot_reset_step_name(KBOOT_RESET_STEP_APPS));
    TEST_ASSERT_EQUAL_STRING("link",    boot_reset_step_name(KBOOT_RESET_STEP_LINK));
    TEST_ASSERT_EQUAL_STRING("wipe",    boot_reset_step_name(KBOOT_RESET_STEP_WIPE));
    TEST_ASSERT_EQUAL_STRING("nvs",     boot_reset_step_name(KBOOT_RESET_STEP_NVS));
    TEST_ASSERT_EQUAL_STRING("request", boot_reset_step_name(KBOOT_RESET_STEP_REQUEST));
    TEST_ASSERT_EQUAL_STRING("done",    boot_reset_step_name(KBOOT_RESET_STEP_DONE));
    TEST_ASSERT_EQUAL_STRING("?",       boot_reset_step_name(0xFFU));
}

/* A board does not leave the factory without its first-run app. */
TEST_CASE("a ship reset without the welcome app erases nothing", "[boot_ao][reset]")
{
    static const char *const seeded[] = { "kset", "kapp", "kident" };
    unsigned                 before;

    reset_up();
    nvs_seed_all(seeded, 3U);
    before = fake_nvs_count();
    radio_state(KRADIO_UP);

    reset_ask(KBOOT_RESET_SHIP);
    expect_find_asked();
    answer_find(false, 0U, 0U, false, false);

    expect_reply(KBOOT_RESET_STEP_FIND, KODE_DENIED);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_sig(AO_AppMgr),
        "no keep-only, no install");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_sig(AO_Radio),
        "no link, no wipe");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before, fake_nvs_count(), "nothing erased");
    TEST_ASSERT_EQUAL_UINT(0U, fake_system_restarts());
    reset_down();
}

/* A person who removed the welcome app can still forget everything: the reset
 * keeps no app, writes no request and ends in kodeOS. */
TEST_CASE("a person's reset without the welcome app erases all and restarts plain",
          "[boot_ao][reset]")
{
    static const char *const seeded[] = { "kset", "kgui", "kapp" };
    const QEvt              *e;
    uint8_t                  step;
    uint8_t                  status;

    reset_up();
    nvs_seed_all(seeded, 3U);
    radio_state(KRADIO_UP);

    reset_ask(KBOOT_RESET_RESTART);
    expect_find_asked();
    answer_find(false, 0U, 0U, false, false);
    TEST_ASSERT_EQUAL_INT((int)C5_WIPE_REQ_SIG, mailbox_sig(AO_Radio));

    answer_wipe(C5_WIPE_DONE, KODE_ST_OK);
    e = mailbox_event(AO_AppMgr);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT((int)APP_KEEP_ONLY_REQ_SIG, (int)e->sig);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", Q_EVT_CAST(AppKeepOnlyReqEvt)->project,
                                     "there is no app to keep");
    QF_gc(e);

    if ( setjmp(*fake_system_arm_restart()) == 0 )
    {
        answer_keep(KODE_OK, 3U);
        TEST_FAIL_MESSAGE("the board should have restarted");
    }

    TEST_ASSERT_EQUAL_UINT(1U, fake_system_restarts());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_nvs_count(), "everything erased");
    TEST_ASSERT_FALSE_MESSAGE(kdrv_apps_req_present(), "no app to boot");
    TEST_ASSERT_TRUE(reply_take(&step, &status));
    TEST_ASSERT_EQUAL_INT((int)KBOOT_RESET_STEP_DONE, (int)step);
    TEST_ASSERT_EQUAL_INT((int)KODE_OK, (int)status);
    reset_down();
}

/* A board that rested in the middle of the reset would wake with half of it
 * done: every step refuses a round. */
TEST_CASE("a running reset refuses the round", "[boot_ao][reset][lowpower]")
{
    reset_up();
    reset_ask(KBOOT_RESET_RESTART);
    expect_find_asked();

    lp_round(AO_Boot, KODE_LP_ASLEEP);
    lp_expect_answer(true, "factory reset");
    lp_expect_no_more();

    answer_find(false, 0U, 0U, false, true);
    expect_reply(KBOOT_RESET_STEP_FIND, KODE_NO_RESOURCES);
    lp_round(AO_Boot, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();
    reset_down();
}

/* A busy AppMgr could not look; the reset says so and goes no further. It is
 * no "the welcome app is absent" (KODE_DENIED): trying again can work. */
TEST_CASE("a busy AppMgr stops the reset at find", "[boot_ao][reset]")
{
    reset_up();
    reset_ask(KBOOT_RESET_RESTART);
    expect_find_asked();
    answer_find(false, 0U, 0U, false, true);

    expect_reply(KBOOT_RESET_STEP_FIND, KODE_NO_RESOURCES);
    TEST_ASSERT_EQUAL_INT(-1, mailbox_sig(AO_AppMgr));
    reset_down();
}

/** @brief A reset that finds the app on the card only, with the link up, up to
 *         the write: AO_AppMgr's mailbox has had it taken out. */
static void reset_to_install(kboot_reset_end_t end)
{
    radio_state(KRADIO_UP);
    reset_ask(end);
    expect_find_asked();
    answer_find(false, 0U, 0U, true, false);
    TEST_ASSERT_EQUAL_INT((int)FLASH_REQ_SIG, mailbox_sig(AO_AppMgr));
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_sig(AO_Radio), "no wipe yet");
}

/* The welcome app on the card only: a write can cost an older app its room, so
 * it waits for the link; its extent is learned after, and only then does the
 * C5 forget anything. */
TEST_CASE("test_reset_installs_from_the_card_once_the_link_is_up", "[boot_ao][reset]")
{
    const QEvt *e;

    reset_up();
    radio_state(KRADIO_OFF);
    reset_ask(KBOOT_RESET_RESTART);
    expect_find_asked();
    answer_find(false, 0U, 0U, true, false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_sig(AO_AppMgr), "the link comes first");
    TEST_ASSERT_EQUAL_INT((int)RADIO_UP_REQ_SIG, mailbox_sig(AO_Radio));

    radio_state(KRADIO_UP);
    e = mailbox_event(AO_AppMgr);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT((int)FLASH_REQ_SIG, (int)e->sig);
    TEST_ASSERT_EQUAL_STRING("/sdcard/" CONFIG_KODEOS_WELCOME_APP ".bin",
                             Q_EVT_CAST(FlashReqEvt)->src);
    TEST_ASSERT_FALSE_MESSAGE(Q_EVT_CAST(FlashReqEvt)->launch,
        "the reset runs the app itself");
    QF_gc(e);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_sig(AO_Radio), "no wipe before the write");

    flash_done(CONFIG_KODEOS_WELCOME_APP);
    expect_find_asked();                     /* its extent, now it is in */
    answer_find(true, WELCOME_OFF2, WELCOME_SIZE, false, false);
    TEST_ASSERT_EQUAL_INT((int)C5_WIPE_REQ_SIG, mailbox_sig(AO_Radio));

    answer_wipe(C5_WIPE_DONE, KODE_ST_OK);
    expect_keep_asked();                     /* only then the others go */

    answer_keep(KODE_DENIED, 0U);            /* ends the case */
    expect_reply(KBOOT_RESET_STEP_APPS, KODE_DENIED);
    reset_down();
}

/* The name a write's end carries is the container's own, not the card file's:
 * a file that holds another project, or a pool too dry to name it, ends a write
 * all the same. Any end of the write moves on to the find, which decides. */
TEST_CASE("a write's end that names another project moves on to the find",
          "[boot_ao][reset]")
{
    reset_up();
    reset_to_install(KBOOT_RESET_SHIP);

    flash_done("some_other_app");

    expect_find_asked();                     /* at once, not at the budget */
    answer_find(false, 0U, 0U, false, false);
    expect_reply(KBOOT_RESET_STEP_INSTALL, KODE_ERROR);
    TEST_ASSERT_EQUAL_INT(-1, mailbox_sig(AO_AppMgr));
    reset_down();
}

TEST_CASE("a write's end with no name moves on to the find", "[boot_ao][reset]")
{
    reset_up();
    reset_to_install(KBOOT_RESET_RESTART);

    flash_done("");

    expect_find_asked();
    answer_find(true, WELCOME_OFF2, WELCOME_SIZE, false, false);
    TEST_ASSERT_EQUAL_INT((int)C5_WIPE_REQ_SIG, mailbox_sig(AO_Radio));

    answer_wipe(C5_WIPE_DONE, KODE_ST_OK);
    expect_keep_asked();                     /* the find decided: it is in */

    answer_keep(KODE_DENIED, 0U);            /* ends the case */
    expect_reply(KBOOT_RESET_STEP_APPS, KODE_DENIED);
    reset_down();
}

/* The write failed, or the app did not land: a board for the factory stops at
 * install, with nothing erased. */
TEST_CASE("a failed install stops a ship reset at install", "[boot_ao][reset]")
{
    reset_up();
    reset_to_install(KBOOT_RESET_SHIP);

    flash_fail();

    expect_reply(KBOOT_RESET_STEP_INSTALL, KODE_ERROR);
    TEST_ASSERT_EQUAL_INT(-1, mailbox_sig(AO_AppMgr));
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_sig(AO_Radio), "no wipe");
    reset_down();
}

/* The write ended well but the directory does not show the app. */
TEST_CASE("an install that leaves no app stops a ship reset at install",
          "[boot_ao][reset]")
{
    reset_up();
    reset_to_install(KBOOT_RESET_SHIP);
    flash_done(CONFIG_KODEOS_WELCOME_APP);
    expect_find_asked();

    answer_find(false, 0U, 0U, false, false);

    expect_reply(KBOOT_RESET_STEP_INSTALL, KODE_ERROR);
    TEST_ASSERT_EQUAL_INT(-1, mailbox_sig(AO_AppMgr));
    TEST_ASSERT_EQUAL_INT(-1, mailbox_sig(AO_Radio));
    reset_down();
}

/* A bad file on a card does not stand between a person and their reset. */
TEST_CASE("a failed install does not stop a person's reset", "[boot_ao][reset]")
{
    const QEvt *e;

    reset_up();
    reset_to_install(KBOOT_RESET_RESTART);

    flash_fail();

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)ao_spy_count(), "the reset goes on");
    TEST_ASSERT_EQUAL_INT((int)C5_WIPE_REQ_SIG, mailbox_sig(AO_Radio));
    answer_wipe(C5_WIPE_DONE, KODE_ST_OK);
    e = mailbox_event(AO_AppMgr);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT((int)APP_KEEP_ONLY_REQ_SIG, (int)e->sig);
    TEST_ASSERT_EQUAL_STRING("", Q_EVT_CAST(AppKeepOnlyReqEvt)->project);
    QF_gc(e);
    answer_keep(KODE_DENIED, 0U);            /* ends the case */
    expect_reply(KBOOT_RESET_STEP_APPS, KODE_DENIED);
    reset_down();
}

TEST_CASE("an install that leaves no app does not stop a person's reset",
          "[boot_ao][reset]")
{
    reset_up();
    reset_to_install(KBOOT_RESET_RESTART);
    flash_done(CONFIG_KODEOS_WELCOME_APP);
    expect_find_asked();

    answer_find(false, 0U, 0U, false, false);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)ao_spy_count(), "the reset goes on");
    TEST_ASSERT_EQUAL_INT((int)C5_WIPE_REQ_SIG, mailbox_sig(AO_Radio));
    answer_wipe(C5_WIPE_BTKEYS, KODE_ST_REFUSED);  /* ends the case */
    expect_reply(KBOOT_RESET_STEP_WIPE, KODE_ERROR);
    reset_down();
}

/* The wipe talks to the C5 over the link, so a board that booted without the
 * radio gets it first, and the wipe waits for the link to say it is up. */
TEST_CASE("test_reset_brings_the_link_up_before_the_wipe", "[boot_ao][reset]")
{
    reset_up();
    radio_state(KRADIO_OFF);

    reset_ask(KBOOT_RESET_RESTART);
    expect_find_asked();
    answer_find(true, WELCOME_OFF, WELCOME_SIZE, false, false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_sig(AO_AppMgr),
        "no app goes before the link is up");

    TEST_ASSERT_EQUAL_INT((int)RADIO_UP_REQ_SIG, mailbox_sig(AO_Radio));
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_sig(AO_Radio),
        "no wipe while the link is down");

    radio_state(KRADIO_STARTING);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_sig(AO_Radio),
        "no wipe while the link is coming up");

    radio_state(KRADIO_UP);
    TEST_ASSERT_EQUAL_INT((int)C5_WIPE_REQ_SIG, mailbox_sig(AO_Radio));

    answer_wipe(C5_WIPE_BTKEYS, KODE_ST_REFUSED);  /* ends the case */
    expect_reply(KBOOT_RESET_STEP_WIPE, KODE_ERROR);
    reset_down();
}

/* AO_Radio drops a request that reaches it while it is going down, and says
 * OFF when it has: the reset asks again, as many times as the budget lets it. */
TEST_CASE("a link that is still off is asked for again", "[boot_ao][reset]")
{
    reset_up();
    radio_state(KRADIO_OFF);

    reset_ask(KBOOT_RESET_RESTART);
    expect_find_asked();
    answer_find(true, WELCOME_OFF, WELCOME_SIZE, false, false);
    TEST_ASSERT_EQUAL_INT((int)RADIO_UP_REQ_SIG, mailbox_sig(AO_Radio));
    TEST_ASSERT_EQUAL_INT(-1, mailbox_sig(AO_Radio));

    radio_state(KRADIO_STOPPING);            /* still on its way down */
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_sig(AO_Radio),
        "nothing to add while it stops");

    radio_state(KRADIO_OFF);                 /* the first request was lost */
    TEST_ASSERT_EQUAL_INT((int)RADIO_UP_REQ_SIG, mailbox_sig(AO_Radio));
    radio_state(KRADIO_OFF);                 /* and so was the second */
    TEST_ASSERT_EQUAL_INT((int)RADIO_UP_REQ_SIG, mailbox_sig(AO_Radio));
    TEST_ASSERT_EQUAL_INT(-1, mailbox_sig(AO_Radio));

    radio_state(KRADIO_UP);
    TEST_ASSERT_EQUAL_INT((int)C5_WIPE_REQ_SIG, mailbox_sig(AO_Radio));

    answer_wipe(C5_WIPE_BTKEYS, KODE_ST_REFUSED);  /* ends the case */
    expect_reply(KBOOT_RESET_STEP_WIPE, KODE_ERROR);
    reset_down();
}

/* An OFF that is nobody's step is news to the cache alone. */
TEST_CASE("an off link outside the link step asks for nothing", "[boot_ao][reset]")
{
    reset_up();
    reset_ask(KBOOT_RESET_SHIP);
    expect_find_asked();

    radio_state(KRADIO_OFF);

    TEST_ASSERT_EQUAL_INT(-1, mailbox_sig(AO_Radio));
    answer_find(false, 0U, 0U, false, false);      /* ends the case */
    expect_reply(KBOOT_RESET_STEP_FIND, KODE_DENIED);
    reset_down();
}

/* HOME must not navigate away mid-reset, so the hold is refused; it is the
 * ordinary thumb on the ordinary button, so the refusal is said at debug. */
TEST_CASE("a button hold during a reset is dropped at debug level",
          "[boot_ao][reset]")
{
    reset_up();
    reset_ask(KBOOT_RESET_RESTART);
    expect_find_asked();
    fake_klog_reset();
    kode_log_set_level(NULL, KODE_LOG_NOTICE);

    hold_bot();

    TEST_ASSERT_FALSE_MESSAGE(klog_has("this button hold is dropped"),
        "not above debug");
    kode_log_set_level(NULL, KODE_LOG_DEBUG);
    hold_bot();
    TEST_ASSERT_TRUE_MESSAGE(klog_has("this button hold is dropped"),
        "at debug");
    TEST_ASSERT_EQUAL_INT(-1, mailbox_sig(AO_Radio));

    answer_find(false, 0U, 0U, false, true);      /* ends the case */
    expect_reply(KBOOT_RESET_STEP_FIND, KODE_NO_RESOURCES);
    reset_down();
}

/* A link that is up already costs no request. */
TEST_CASE("a link that is up needs no request", "[boot_ao][reset]")
{
    reset_up();
    reset_to_wipe(KBOOT_RESET_RESTART);   /* asserts no up request */

    answer_wipe(C5_WIPE_BTKEYS, KODE_ST_REFUSED);
    expect_reply(KBOOT_RESET_STEP_WIPE, KODE_ERROR);
    reset_down();
}

/* AO_Radio refuses a wipe while a request of its own is with the vault - the
 * list it asks for as the link comes up, or an autojoin round. The reset asks
 * again a moment later, within the step's budget. */
TEST_CASE("a busy radio is asked for the wipe again", "[boot_ao][reset]")
{
    reset_up();
    reset_to_wipe(KBOOT_RESET_RESTART);

    answer_wipe(C5_WIPE_VAULT, KODE_ST_REFUSED);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)ao_spy_count(),
        "a refusal at the vault is no answer yet");
    TEST_ASSERT_EQUAL_INT(-1, mailbox_sig(AO_Radio));

    (void)qf_support_tick(AO_Boot,
                          QF_SUPPORT_STEPS(KODE_RESET_WIPE_AGAIN_MS) - 1U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_sig(AO_Radio), "not yet");
    (void)qf_support_tick(AO_Boot, 1U);
    reset_pump();
    TEST_ASSERT_EQUAL_INT((int)C5_WIPE_REQ_SIG, mailbox_sig(AO_Radio));
    TEST_ASSERT_EQUAL_INT(-1, mailbox_sig(AO_Radio));

    answer_wipe(C5_WIPE_BTKEYS, KODE_ST_REFUSED);  /* ends the case */
    expect_reply(KBOOT_RESET_STEP_WIPE, KODE_ERROR);
    reset_down();
}

/* With the link down, a refusal is no busy radio: nothing can be wiped, so
 * the reset stops at once instead of asking for the whole budget. */
TEST_CASE("a refused wipe with the link down stops the reset", "[boot_ao][reset]")
{
    reset_up();
    reset_to_wipe(KBOOT_RESET_RESTART);

    radio_state(KRADIO_OFF);
    answer_wipe(C5_WIPE_VAULT, KODE_ST_REFUSED);
    expect_reply(KBOOT_RESET_STEP_WIPE, KODE_ERROR);

    (void)qf_support_tick(AO_Boot, QF_SUPPORT_STEPS(KODE_RESET_WIPE_AGAIN_MS));
    reset_pump();
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_sig(AO_Radio),
        "no request after a stop");
    reset_down();
}

/* Any other answer at the vault, a storage failure on the C5 for one, stops
 * the reset there as before. */
TEST_CASE("a vault that fails the wipe stops the reset", "[boot_ao][reset]")
{
    reset_up();
    reset_to_wipe(KBOOT_RESET_RESTART);

    answer_wipe(C5_WIPE_VAULT, KODE_ST_STORAGE);
    expect_reply(KBOOT_RESET_STEP_WIPE, KODE_ERROR);
    TEST_ASSERT_EQUAL_INT(-1, mailbox_sig(AO_Radio));
    reset_down();
}

/* A radio still busy when the step's budget runs out stops the reset at the
 * wipe, and nothing more is asked of it afterwards. */
TEST_CASE("a radio busy for the whole budget stops the reset at wipe", "[boot_ao][reset]")
{
    uint32_t const half = QF_SUPPORT_STEPS(KODE_RESET_WIPE_AGAIN_MS) / 2U;

    reset_up();
    reset_to_wipe(KBOOT_RESET_RESTART);

    (void)qf_support_tick(AO_Boot, QF_SUPPORT_STEPS(KODE_RESET_WIPE_MS) - half);
    answer_wipe(C5_WIPE_VAULT, KODE_ST_REFUSED);
    (void)qf_support_tick(AO_Boot, half - 1U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)ao_spy_count(), "not yet");
    (void)qf_support_tick(AO_Boot, 1U);
    expect_reply(KBOOT_RESET_STEP_WIPE, KODE_ERROR);

    (void)qf_support_tick(AO_Boot, QF_SUPPORT_STEPS(KODE_RESET_WIPE_AGAIN_MS));
    reset_pump();
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_sig(AO_Radio),
        "nothing is asked once the reset has stopped");
    reset_down();
}

/* The link never comes: the reset stops at link, with the wipe never asked and
 * the apps and the settings where they were. A C5 that does not answer used to
 * take the apps with it and call that "not done". */
TEST_CASE("a link that never comes up times out, with nothing erased", "[boot_ao][reset]")
{
    static const char *const seeded[] = { "kset", "kgui", "kapp" };

    reset_up();
    nvs_seed_all(seeded, 3U);
    radio_state(KRADIO_OFF);
    reset_ask(KBOOT_RESET_RESTART);
    expect_find_asked();
    answer_find(true, WELCOME_OFF, WELCOME_SIZE, false, false);
    TEST_ASSERT_EQUAL_INT((int)RADIO_UP_REQ_SIG, mailbox_sig(AO_Radio));

    (void)qf_support_tick(AO_Boot, QF_SUPPORT_STEPS(KODE_RESET_LINK_MS) - 1U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)ao_spy_count(), "not yet");
    (void)qf_support_tick(AO_Boot, 1U);

    expect_reply(KBOOT_RESET_STEP_LINK, KODE_ERROR);
    TEST_ASSERT_EQUAL_INT(-1, mailbox_sig(AO_Radio));
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_sig(AO_AppMgr), "no app went");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, fake_nvs_count(), "no setting went");
    reset_down();
}

/* The keep list: the identity cache and the station lock survive, every
 * other namespace goes, an app's and the bond store's included. */
TEST_CASE("test_nvs_keeps_only_the_keep_list", "[boot_ao][reset]")
{
    static const char *const seeded[] = {
        "kset", "kgui", "kapp", KHAL_IDENTITY_NVS_NS, KODE_STATION_NVS_NS,
        "nimble_bond"
    };
    char     left[8][KHAL_NVS_NS_SIZE];
    unsigned n;

    reset_up();
    nvs_seed_all(seeded, 6U);
    reset_to_wipe(KBOOT_RESET_SHIP);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(6U, fake_nvs_count(),
        "nothing erased before the wipe has answered");

    wipe_then_apps();

    expect_reply(KBOOT_RESET_STEP_DONE, KODE_OK);
    n = nvs_left(left, 8U);
    TEST_ASSERT_EQUAL_UINT(KEPT_LEFT, n);
    TEST_ASSERT_TRUE(nvs_has(left, n, KHAL_IDENTITY_NVS_NS));
    TEST_ASSERT_TRUE(nvs_has(left, n, KODE_STATION_NVS_NS));
    TEST_ASSERT_EQUAL_UINT(KEPT_LEFT, fake_nvs_count());
    reset_down();
}

/* A store with nothing but the kept namespaces is done at once. */
TEST_CASE("a store holding only the kept namespaces is done", "[boot_ao][reset]")
{
    static const char *const seeded[] = { KHAL_IDENTITY_NVS_NS,
                                          KODE_STATION_NVS_NS };

    reset_up();
    nvs_seed_all(seeded, 2U);
    reset_to_wipe(KBOOT_RESET_SHIP);

    wipe_then_apps();

    expect_reply(KBOOT_RESET_STEP_DONE, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(2U, fake_nvs_count());
    reset_down();
}

/* The request that boots the welcome app is the last thing written: nothing
 * stands on flash until everything is erased, and then the board restarts
 * without parking the radio. */
TEST_CASE("test_new_request_is_written_last", "[boot_ao][reset]")
{
    static const char *const seeded[] = { "kset", "kgui", "kapp" };
    kapps_req_t              req;
    uint8_t                  step;
    uint8_t                  status;

    reset_up();
    nvs_seed_all(seeded, 3U);
    reset_to_wipe(KBOOT_RESET_RESTART);
    TEST_ASSERT_FALSE_MESSAGE(kdrv_apps_req_present(),
        "no request while the reset still has work");

    if ( setjmp(*fake_system_arm_restart()) == 0 )
    {
        wipe_then_apps();
        TEST_FAIL_MESSAGE("the board should have restarted");
    }

    TEST_ASSERT_EQUAL_UINT(1U, fake_system_restarts());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_nvs_count(), "everything erased");
    TEST_ASSERT_TRUE(kdrv_apps_req_present());
    request_read(&req);
    TEST_ASSERT_EQUAL_HEX32(KAPPS_REQ_STATE_NEW, req.state);
    TEST_ASSERT_EQUAL_HEX32(WELCOME_OFF, req.offset);
    TEST_ASSERT_EQUAL_HEX32(WELCOME_SIZE, req.size);

    TEST_ASSERT_TRUE(reply_take(&step, &status));
    TEST_ASSERT_EQUAL_INT((int)KBOOT_RESET_STEP_DONE, (int)step);
    TEST_ASSERT_EQUAL_INT((int)KODE_OK, (int)status);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_sig(AO_Radio),
        "the radio is not parked: a parked C5 keeps what the wipe erased");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_sig(AO_Net),
        "no clean leave of the channel either");
    reset_down();
}

/* The extent that is written is the one the last find reported. */
TEST_CASE("the request names the extent found after an install", "[boot_ao][reset]")
{
    kapps_req_t req;

    reset_up();
    radio_state(KRADIO_UP);
    reset_ask(KBOOT_RESET_RESTART);
    expect_find_asked();
    answer_find(false, 0U, 0U, true, false);
    TEST_ASSERT_EQUAL_INT((int)FLASH_REQ_SIG, mailbox_sig(AO_AppMgr));
    flash_done(CONFIG_KODEOS_WELCOME_APP);
    expect_find_asked();
    answer_find(true, WELCOME_OFF2, WELCOME_SIZE, false, false);
    TEST_ASSERT_EQUAL_INT((int)C5_WIPE_REQ_SIG, mailbox_sig(AO_Radio));

    if ( setjmp(*fake_system_arm_restart()) == 0 )
    {
        wipe_then_apps();
        TEST_FAIL_MESSAGE("the board should have restarted");
    }

    request_read(&req);
    TEST_ASSERT_EQUAL_HEX32(KAPPS_REQ_STATE_NEW, req.state);
    TEST_ASSERT_EQUAL_HEX32(WELCOME_OFF2, req.offset);
    reset_down();
}

/* A request that cannot be written is told to the asker, and the board stays:
 * everything is erased, the welcome app is still installed, and the person
 * who asked can try again. */
TEST_CASE("a boot request that cannot be written is answered, not restarted",
          "[boot_ao][reset]")
{
    reset_up();
    radio_state(KRADIO_UP);
    reset_ask(KBOOT_RESET_RESTART);
    expect_find_asked();
    answer_find(true, OFF_PAST_REGION, WELCOME_SIZE, false, false);
    TEST_ASSERT_EQUAL_INT((int)C5_WIPE_REQ_SIG, mailbox_sig(AO_Radio));

    wipe_then_apps();

    expect_reply(KBOOT_RESET_STEP_REQUEST, KODE_ERROR);
    TEST_ASSERT_EQUAL_UINT(0U, fake_system_restarts());
    TEST_ASSERT_FALSE(kdrv_apps_req_present());
    reset_down();
}

/* The ship end waits for the cable to leave, and writes the request only
 * then: a board that sits switched off must find the welcome app, and a board
 * that is bumped off the cable must not. */
TEST_CASE("test_ship_end_writes_on_due_then_sleeps", "[boot_ao][reset]")
{
    kapps_req_t  req;
    const QEvt  *e;

    reset_up();
    reset_to_wipe(KBOOT_RESET_SHIP);

    wipe_then_apps();

    expect_reply(KBOOT_RESET_STEP_DONE, KODE_OK);
    TEST_ASSERT_FALSE_MESSAGE(kdrv_apps_req_present(),
        "no request while the cable is in");
    TEST_ASSERT_EQUAL_UINT(0U, fake_system_restarts());
    e = mailbox_event(AO_Power);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT((int)POWER_SHIP_ARM_SIG, (int)e->sig);
    TEST_ASSERT_TRUE(Q_EVT_CAST(PowerShipArmEvt)->who == AO_Boot);
    QF_gc(e);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_sig(AO_Power),
        "no sleep before the cable is out");

    s_confirms = 0U;
    ship_due();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_confirms, "the next power-on would go back a slot");
    TEST_ASSERT_TRUE(kdrv_apps_req_present());
    request_read(&req);
    TEST_ASSERT_EQUAL_HEX32(KAPPS_REQ_STATE_NEW, req.state);
    TEST_ASSERT_EQUAL_HEX32(WELCOME_OFF, req.offset);
    TEST_ASSERT_EQUAL_INT((int)POWER_SHIP_REQ_SIG, mailbox_sig(AO_Power));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_system_restarts(), "no restart");
    TEST_ASSERT_EQUAL_INT(-1, mailbox_sig(AO_Radio));
    reset_down();
}

/* Nobody is there to read a refusal: the board is switched off all the same
 * and the customer gets plain kodeOS. */
TEST_CASE("a late write that fails still ships", "[boot_ao][reset]")
{
    unsigned logged;

    reset_up();
    radio_state(KRADIO_UP);
    reset_ask(KBOOT_RESET_SHIP);
    expect_find_asked();
    answer_find(true, OFF_PAST_REGION, WELCOME_SIZE, false, false);
    TEST_ASSERT_EQUAL_INT((int)C5_WIPE_REQ_SIG, mailbox_sig(AO_Radio));
    wipe_then_apps();
    expect_reply(KBOOT_RESET_STEP_DONE, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)POWER_SHIP_ARM_SIG, mailbox_sig(AO_Power));
    logged = fake_klog_count();

    ship_due();

    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(logged, fake_klog_count(),
        "the failed write is logged");
    TEST_ASSERT_FALSE(kdrv_apps_req_present());
    TEST_ASSERT_EQUAL_INT((int)POWER_SHIP_REQ_SIG, mailbox_sig(AO_Power));
    reset_down();
}

/* The cable was in all through the wait, and the settings of every AO that
 * outlives the NVS step went on being written: the walk runs again once the
 * cable is out, before the request is written and the board is switched off. */
TEST_CASE("the ship end walks the NVS again once the cable is out",
          "[boot_ao][reset]")
{
    static const char *const seeded[] = { "kapp", KHAL_IDENTITY_NVS_NS,
                                          KODE_STATION_NVS_NS };
    static const char *const late[]   = { "kset", "kgui" };
    char     left[8][KHAL_NVS_NS_SIZE];
    unsigned n;

    reset_up();
    nvs_seed_all(seeded, 3U);
    reset_to_wipe(KBOOT_RESET_SHIP);
    wipe_then_apps();
    expect_reply(KBOOT_RESET_STEP_DONE, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)POWER_SHIP_ARM_SIG, mailbox_sig(AO_Power));
    TEST_ASSERT_EQUAL_UINT(KEPT_LEFT, fake_nvs_count());

    nvs_seed_all(late, 2U);                  /* an AO wrote during the wait */
    TEST_ASSERT_EQUAL_UINT(KEPT_LEFT + 2U, fake_nvs_count());
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_sig(AO_Power),
        "no sleep before the cable is out");

    ship_due();

    n = nvs_left(left, 8U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(KEPT_LEFT, n, "what was written meanwhile is gone");
    TEST_ASSERT_TRUE(nvs_has(left, n, KHAL_IDENTITY_NVS_NS));
    TEST_ASSERT_TRUE(nvs_has(left, n, KODE_STATION_NVS_NS));
    TEST_ASSERT_TRUE_MESSAGE(kdrv_apps_req_present(), "the request is written");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)POWER_SHIP_REQ_SIG, mailbox_sig(AO_Power),
        "and the board is switched off, after the walk");
    TEST_ASSERT_EQUAL_INT(-1, mailbox_sig(AO_Power));
    TEST_ASSERT_EQUAL_UINT(0U, fake_system_restarts());
    reset_down();
}

/* The arm is made before the answer says the reset is done: one that was not
 * posted is an answer, the board stays up and nothing waits for a word that
 * is not coming. */
TEST_CASE("a ship arm that cannot be posted is answered, not waited for",
          "[boot_ao][reset]")
{
    static const QEvt filler = QEVT_INITIALIZER(POWER_DUMP_REQ_SIG);

    reset_up();
    reset_to_wipe(KBOOT_RESET_SHIP);
    while ( QACTIVE_POST_X(AO_Power, &filler, 0U, NULL) )
    {
        /* until AO_Power's queue has no room */
    }

    wipe_then_apps();

    expect_reply(KBOOT_RESET_STEP_REQUEST, KODE_ERROR);
    queue_drained(AO_Power);
    ship_due();                              /* nobody is waiting for it */
    TEST_ASSERT_EQUAL_INT(-1, mailbox_sig(AO_Power));
    TEST_ASSERT_FALSE(kdrv_apps_req_present());

    /* Back in Boot_active, not Boot_shipping: a new reset is taken, not
     * refused as busy. */
    reset_ask(KBOOT_RESET_RESTART);
    expect_find_asked();
    answer_find(false, 0U, 0U, false, true);
    expect_reply(KBOOT_RESET_STEP_FIND, KODE_NO_RESOURCES);
    reset_down();
}

/* A step that fails stops the reset there, names the step, and the NVS is
 * left exactly as it was. */
TEST_CASE("test_a_failed_step_stops_and_names_it", "[boot_ao][reset]")
{
    static const char *const seeded[] = { "kset", "kapp", KHAL_IDENTITY_NVS_NS };
    unsigned                 before;

    reset_up();
    nvs_seed_all(seeded, 3U);
    before = fake_nvs_count();
    reset_to_wipe(KBOOT_RESET_RESTART);

    answer_wipe(C5_WIPE_BTKEYS, KODE_ST_REFUSED);

    expect_reply(KBOOT_RESET_STEP_WIPE, KODE_ERROR);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before, fake_nvs_count(), "no NVS erased");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_sig(AO_AppMgr), "no app went");
    TEST_ASSERT_FALSE(kdrv_apps_req_present());
    TEST_ASSERT_EQUAL_UINT(0U, fake_system_restarts());
    reset_down();
}

/* An owner that never answers ends the reset at its step, at its budget. */
TEST_CASE("test_a_silent_owner_times_out", "[boot_ao][reset]")
{
    reset_up();
    reset_ask(KBOOT_RESET_RESTART);
    expect_find_asked();

    (void)qf_support_tick(AO_Boot, QF_SUPPORT_STEPS(KODE_RESET_FIND_MS) - 1U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)ao_spy_count(), "not yet");
    (void)qf_support_tick(AO_Boot, 1U);

    expect_reply(KBOOT_RESET_STEP_FIND, KODE_ERROR);
    reset_down();
}

/* Each step has a budget of its own: the one before it must not leak into the
 * next. The find answers just in time and the keep-only is then silent. */
TEST_CASE("each step is armed with its own budget", "[boot_ao][reset]")
{
    reset_up();
    radio_state(KRADIO_UP);
    reset_ask(KBOOT_RESET_RESTART);
    expect_find_asked();
    (void)qf_support_tick(AO_Boot, QF_SUPPORT_STEPS(KODE_RESET_FIND_MS) - 1U);
    answer_find(true, WELCOME_OFF, WELCOME_SIZE, false, false);
    TEST_ASSERT_EQUAL_INT((int)C5_WIPE_REQ_SIG, mailbox_sig(AO_Radio));
    (void)qf_support_tick(AO_Boot, QF_SUPPORT_STEPS(KODE_RESET_WIPE_MS) - 1U);
    answer_wipe(C5_WIPE_DONE, KODE_ST_OK);
    expect_keep_asked();

    (void)qf_support_tick(AO_Boot, QF_SUPPORT_STEPS(KODE_RESET_APPS_MS) - 1U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)ao_spy_count(),
        "the keep-only has its own minute");
    (void)qf_support_tick(AO_Boot, 1U);

    expect_reply(KBOOT_RESET_STEP_APPS, KODE_ERROR);
    reset_down();
}

/* While a reset runs, nothing else may restart the board or start a second
 * reset: the half-erased board would come back to nothing. */
TEST_CASE("test_requests_during_a_reset_are_refused", "[boot_ao][reset]")
{
    BootRunEvt run = { .super = QEVT_INITIALIZER(BOOT_APP_RUN_SIG),
                       .offset = KAPPS_DATA_OFF, .size = WELCOME_SIZE,
                       .mode = KBOOT_RUN_ONCE };
    static const QEvt exit_req = QEVT_INITIALIZER(BOOT_EXIT_REQ_SIG);
    static const QEvt os_req   = QEVT_INITIALIZER(BOOT_OS_SWITCH_SIG);

    reset_up();
    reset_ask(KBOOT_RESET_RESTART);
    expect_find_asked();

    reset_ask(KBOOT_RESET_SHIP);            /* a second reset */
    expect_reply(KBOOT_RESET_STEP_FIND, KODE_NO_RESOURCES);

    QASM_DISPATCH(AO_Boot, &run.super, 0U);
    QASM_DISPATCH(AO_Boot, &exit_req, 0U);
    QASM_DISPATCH(AO_Boot, &os_req, 0U);
    reset_pump();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_system_restarts(), "no restart");
    TEST_ASSERT_FALSE_MESSAGE(kdrv_apps_req_present(), "no request written");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_sig(AO_Radio), "no leave");
    TEST_ASSERT_EQUAL_INT(-1, mailbox_sig(AO_Net));
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_sig(AO_AppMgr),
        "the first reset asked nothing twice");

    answer_find(false, 0U, 0U, false, true);        /* the first is alive */
    expect_reply(KBOOT_RESET_STEP_FIND, KODE_NO_RESOURCES);
    reset_down();
}

/* A ship end that waits for the cable refuses the same things. */
TEST_CASE("a waiting ship end refuses a second reset and a restart",
          "[boot_ao][reset]")
{
    static const QEvt exit_req = QEVT_INITIALIZER(BOOT_EXIT_REQ_SIG);

    reset_up();
    reset_to_wipe(KBOOT_RESET_SHIP);
    wipe_then_apps();
    expect_reply(KBOOT_RESET_STEP_DONE, KODE_OK);

    reset_ask(KBOOT_RESET_RESTART);
    expect_reply(KBOOT_RESET_STEP_FIND, KODE_NO_RESOURCES);
    QASM_DISPATCH(AO_Boot, &exit_req, 0U);
    reset_pump();
    TEST_ASSERT_EQUAL_UINT(0U, fake_system_restarts());
    reset_down();
}

/* A restart that is already leaving is not interrupted by a reset. */
TEST_CASE("a reset asked during a leave is refused", "[boot_ao][reset]")
{
    static const QEvt exit_req = QEVT_INITIALIZER(BOOT_EXIT_REQ_SIG);

    reset_up();
    QASM_DISPATCH(AO_Boot, &exit_req, 0U);
    reset_ask(KBOOT_RESET_RESTART);
    expect_reply(KBOOT_RESET_STEP_FIND, KODE_NO_RESOURCES);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, mailbox_sig(AO_AppMgr), "nothing asked");

    answer_net_left();
    finish_leave();                                  /* ends the leave */
    reset_down();
}

/* Outside a reset the three published signals are news to nobody but the
 * link cache: a write that ends, or fails, must start nothing. */
TEST_CASE("a write's end outside a reset starts nothing", "[boot_ao][reset]")
{
    reset_up();

    flash_done(CONFIG_KODEOS_WELCOME_APP);
    flash_fail();
    radio_state(KRADIO_UP);

    TEST_ASSERT_EQUAL_INT(-1, mailbox_sig(AO_AppMgr));
    TEST_ASSERT_EQUAL_INT(-1, mailbox_sig(AO_Radio));
    TEST_ASSERT_EQUAL_INT(-1, mailbox_sig(AO_Power));
    TEST_ASSERT_EQUAL_INT(0, (int)ao_spy_count());
    reset_down();
}

/* The link state is learned between resets too: a link that came up before
 * the reset is not asked for again. */
TEST_CASE("the link cache is kept between resets", "[boot_ao][reset]")
{
    reset_up();
    radio_state(KRADIO_UP);
    radio_state(KRADIO_STOPPING);
    radio_state(KRADIO_OFF);
    radio_state(KRADIO_UP);                          /* last word: up */

    reset_to_wipe(KBOOT_RESET_RESTART);     /* no up request */

    answer_wipe(C5_WIPE_BTKEYS, KODE_ST_REFUSED);
    expect_reply(KBOOT_RESET_STEP_WIPE, KODE_ERROR);
    reset_down();
}

/* A request that names no end, or no asker, is refused where it is made. */
TEST_CASE("a reset with no asker or no end is not posted", "[boot_ao][reset]")
{
    reset_up();

    TEST_ASSERT_FALSE(boot_factory_reset_request(KBOOT_RESET_RESTART,
                                                 NULL));
    TEST_ASSERT_FALSE(boot_factory_reset_request((kboot_reset_end_t)0xFFU, ao_spy()));
    reset_pump();
    TEST_ASSERT_EQUAL_INT(0, (int)ao_spy_count());
    TEST_ASSERT_EQUAL_INT(-1, mailbox_sig(AO_AppMgr));
    reset_down();
}
