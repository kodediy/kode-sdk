/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file runner.c
 * @brief Entry point of the host suite: run every registered case, then go.
 */
#include <stdlib.h>

#include "unity.h"

#include "Mockkhal_gpio.h"

#include "fakes.h"
#include "fake_khal_worker.h"

/**
 * @brief Put every fake back to a fresh state.
 *
 * Sequencing the fakes is the runner's job rather than any one fake's: none
 * of them owns the others, and a reset that lived inside one would make that
 * one the odd file out.
 *
 * The ownership table is not in this list, and that is the point: the real
 * one is linked rather than faked, and it needs no reset hook because it can
 * be given its resources back. kbsp_release() is what a fake could not offer.
 */
void fakes_reset(void)
{
    fake_nvs_reset();
    fake_kaos_linked_reset();
    fake_sync_reset();
    /* The card's supply is a GPIO the driver drives low before every mount,
     * and khal_gpio is a CMock mock rather than a fake - so the mount below
     * would be an unexpected call in EVERY case in the binary, including the
     * ones that have never heard of a card. Ignored here, at the one place
     * that knows a mount is about to happen, and WITHOUT Mockkhal_gpio_Init():
     * a CMock Init frees the one memory pool every mock in this binary
     * chains its expectations from, and a case that sets a mock up without
     * initialising it first - test_net_ao.c does - then chains onto freed
     * memory and CMock_Guts_MemChain() walks a cycle for ever. A case that
     * drives GPIOs of its own calls Mockkhal_gpio_Init() in its own body,
     * which is after this and replaces it: the seven files that do are
     * unaffected. */
    khal_gpio_output_IgnoreAndReturn(KODE_OK);
    khal_gpio_driven_low_IgnoreAndReturn(false);
    /* Before the log's, and that ordering is load-bearing: kdrv_sd is real in
     * this binary, so putting the card back means mounting it, and a mount
     * logs a line. Reset the log afterwards and every case starts with a
     * capture that holds only its own lines. */
    fake_media_reset();
    fake_klog_reset();
    fake_time_reset();
    fake_panic_reset();
    fake_port_reset();
    fake_system_reset();
    fake_temp_reset();
    fake_radio_reset();
    fake_peer_reset();
    fake_hosted_reset();
    fake_neighbours_reset();
    fake_tinfl_reset();
    fake_ble_mirror_reset();
    fake_ble_reset();
    fake_khal_worker_reset();
#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
    fake_ble_pair_reset();
#endif
}

/** @brief Runs before every case. IDF's default is weak; this replaces it. */
void setUp(void)
{
    fakes_reset();
}

/** @brief Runs after every case. */
void tearDown(void)
{
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    /* exit(), not return: the host port's scheduler never stops, so a return hangs. */
    exit(UNITY_END());
}
