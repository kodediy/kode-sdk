/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_boot_ao_nohold.c
 * @brief AO_Boot built without the home hold, as kodeOS builds it: the bottom
 *        button's ordinary hold announces home.
 *
 * The project builds AO_Boot with CONFIG_KODEOS_HOME_HOLD, which is the app's
 * half (main/CMakeLists.txt). This file compiles the same source again
 * without the symbol and under names of its own, as test_kode_event_os.c does
 * for the standard layer. The copy is never started: the cases hand an event
 * to its state handler and read what it published.
 */
#undef CONFIG_KODEOS_HOME_HOLD

#define AO_Boot                    AO_Boot_nohold
#define Boot_ctor                  Boot_ctor_nohold
#define kaos_boot_start            kaos_boot_start_nohold
#define boot_reason                boot_reason_nohold
#define boot_app_end               boot_app_end_nohold
#define boot_app_offset            boot_app_offset_nohold
#define boot_app_end_of            boot_app_end_of_nohold
#define boot_crashes_next          boot_crashes_next_nohold
#define boot_app_end_name          boot_app_end_name_nohold
#define boot_os_switch_request     boot_os_switch_request_nohold
#define boot_app_run_request       boot_app_run_request_nohold
#define boot_info_request          boot_info_request_nohold
#define boot_exit_evt              boot_exit_evt_nohold
#define boot_radio_parked          boot_radio_parked_nohold
#define boot_net_left              boot_net_left_nohold
#define boot_log_flushed           boot_log_flushed_nohold
#define boot_home_pending          boot_home_pending_nohold
#define boot_home_defer_evt        boot_home_defer_evt_nohold
#define boot_home_now_evt          boot_home_now_evt_nohold
#define boot_first_frame           boot_first_frame_nohold
#define boot_system_up             boot_system_up_nohold
#define boot_reset_step_name       boot_reset_step_name_nohold
#define boot_factory_reset_request boot_factory_reset_request_nohold
#include "../../../components/kaos/boot/src/boot_ao.c"

#include "unity.h"

#include "ao_spy.h"
#include "qf_support.h"

/** @brief A hold of @p id under @p sig, as AO_Buttons publishes one. */
static ButtonHoldEvt hold_of(enum_t sig, kdrv_button_t id)
{
    ButtonHoldEvt h;

    h.super = (QEvt)QEVT_INITIALIZER(sig);
    h.id    = (uint8_t)id;
    return h;
}

/** @brief How many HOME_SIG the spy holds; the spy is left empty. */
static uint32_t homes_taken(void)
{
    uint32_t n = 0U;

    for ( const QEvt *e = ao_spy_take(); NULL != e; e = ao_spy_take() )
    {
        if ( (QSignal)HOME_SIG == e->sig )
        {
            ++n;
        }
    }
    ao_spy_drain();
    return n;
}

TEST_CASE("without the home hold the bottom button's ordinary hold announces home",
          "[boot_ao]")
{
    ButtonHoldEvt const bot = hold_of(BUTTON_HOLD_SIG, KDRV_BTN_BOT);
    ButtonHoldEvt const top = hold_of(BUTTON_HOLD_SIG, KDRV_BTN_TOP);

    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();
    ao_spy_subscribe(HOME_SIG);

    TEST_ASSERT_EQUAL_INT((int)Q_RET_HANDLED, (int)Boot_active(&l_boot, &bot.super));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, homes_taken(),
        "the interface goes home at the ordinary hold, as kodeOS has it");

    TEST_ASSERT_EQUAL_INT((int)Q_RET_HANDLED, (int)Boot_active(&l_boot, &top.super));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, homes_taken(), "the top button is not home");
}

/* The longer hold is the app build's gesture. This build does not subscribe
 * to it, and one handed to it anyway is nobody's. */
TEST_CASE("without the home hold the longer hold announces nothing", "[boot_ao]")
{
    ButtonHoldEvt const far = hold_of(BUTTON_HOME_HOLD_SIG, KDRV_BTN_BOT);

    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();
    ao_spy_subscribe(HOME_SIG);

    TEST_ASSERT_EQUAL_INT((int)Q_RET_SUPER, (int)Boot_active(&l_boot, &far.super));
    TEST_ASSERT_EQUAL_UINT32(0U, homes_taken());
}
