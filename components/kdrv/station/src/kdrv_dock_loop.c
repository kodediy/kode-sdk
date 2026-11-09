/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_dock_loop.c
 * @brief The E5 station's pads loop over a free UART, as the charging base's jig does it.
 */
#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"

#include "kbsp_pins.h"
#include "kdrv_dock_loop.h"
#include "khal_time.h"

/** @brief The lowest UART the loop may take: 0 is the ROM's console, 1 the C5's. */
#define LOOP_UART_FIRST      2
/** @brief RX buffer the UART driver keeps, bytes (its floor is the FIFO's size). */
#define LOOP_RX_BUF          256
/** @brief Bits on the wire per byte: start, eight data, stop. */
#define LOOP_BITS_PER_BYTE   10U
/** @brief Milliseconds in a second. */
#define LOOP_MS_PER_S        1000U
/** @brief What a frame's wait adds to its own air time, ms. */
#define LOOP_MARGIN_MS       30U
/** @brief The frame's two first bytes, so a frame is recognisable on a scope. */
#define LOOP_MAGIC_0         ((uint8_t)'E')
#define LOOP_MAGIC_1         ((uint8_t)'5')
/** @brief Where a frame's random part starts. */
#define LOOP_HEAD_BYTES      4U
/** @brief The seeds of the frames' content (Knuth's multiplicative hash, then an LCG). */
#define LOOP_HASH_MUL        2654435761U
#define LOOP_LCG_MUL         1664525U
#define LOOP_LCG_ADD         1013904223U
#define LOOP_TOP_BYTE_SHIFT  24U
/** @brief Two bytes a stuck pin could produce, never in a frame. */
#define LOOP_STUCK_LOW       0x00U
#define LOOP_STUCK_HIGH      0xFFU
/** @brief What replaces them. */
#define LOOP_FILL            0x5AU

/** @brief The first UART with no driver installed, or -1. */
static int loop_pick_uart(void)
{
    int u;
    int found = -1;

    for ( u = (int)SOC_UART_HP_NUM - 1; LOOP_UART_FIRST <= u; u-- )
    {
        if ( (0 > found) && !uart_is_driver_installed((uart_port_t)u) )
        {
            found = u;
        }
    }
    return found;
}

/** @brief Frame @p idx of a pass at @p baud: changing content, never a stuck level's byte. */
static void loop_frame(uint8_t *f, uint32_t idx, uint32_t baud)
{
    uint32_t x = ((idx + 1U) * LOOP_HASH_MUL) ^ baud;
    uint32_t i;

    f[0] = LOOP_MAGIC_0;
    f[1] = LOOP_MAGIC_1;
    f[2] = (uint8_t)idx;
    f[3] = (uint8_t)~idx;
    for ( i = LOOP_HEAD_BYTES; KDRV_DOCK_LOOP_FRAME_BYTES > i; i++ )
    {
        uint8_t b;

        x = (x * LOOP_LCG_MUL) + LOOP_LCG_ADD;
        b = (uint8_t)(x >> LOOP_TOP_BYTE_SHIFT);
        if ( (LOOP_STUCK_LOW == b) || (LOOP_STUCK_HIGH == b) )
        {
            b = LOOP_FILL;
        }
        f[i] = b;
    }
}

/**
 * @brief One pass: @p n frames at @p baud from @p tx to @p rx.
 * @param seen Adds the bytes that came back at all.
 * @return Frames that came back identical.
 */
static uint8_t loop_pass(int u, int tx, int rx, uint32_t baud, uint8_t n, uint32_t *seen)
{
    const uart_config_t cfg = {
        .baud_rate  = (int)baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uint32_t const wait_ms = ((KDRV_DOCK_LOOP_FRAME_BYTES * LOOP_BITS_PER_BYTE * LOOP_MS_PER_S) / baud)
                             + LOOP_MARGIN_MS;
    uint8_t good = 0U;
    uint8_t i;

    if ( (uart_param_config((uart_port_t)u, &cfg) != ESP_OK)
        || (uart_set_pin((uart_port_t)u, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) )
    {
        return 0U;
    }
    for ( i = 0U; i < n; i++ )
    {
        uint8_t f[KDRV_DOCK_LOOP_FRAME_BYTES];
        uint8_t r[KDRV_DOCK_LOOP_FRAME_BYTES];
        int got;

        loop_frame(f, i, baud);
        (void)uart_flush_input((uart_port_t)u);
        (void)uart_write_bytes((uart_port_t)u, f, KDRV_DOCK_LOOP_FRAME_BYTES);
        (void)uart_wait_tx_done((uart_port_t)u, pdMS_TO_TICKS(wait_ms));
        got = uart_read_bytes((uart_port_t)u, r, KDRV_DOCK_LOOP_FRAME_BYTES, pdMS_TO_TICKS(wait_ms));
        if ( 0 < got )
        {
            *seen += (uint32_t)got;
        }
        if ( ((int)KDRV_DOCK_LOOP_FRAME_BYTES == got) && (memcmp(f, r, KDRV_DOCK_LOOP_FRAME_BYTES) == 0) )
        {
            good++;
        }
    }
    return good;
}

/** @brief Both pads as inputs without pull: how the loop leaves them. */
static void loop_float_pads(void)
{
    static const gpio_num_t PADS[] = { (gpio_num_t)KBSP_PIN_DOCK_TX, (gpio_num_t)KBSP_PIN_DOCK_RX };
    size_t k;

    for ( k = 0U; (sizeof(PADS) / sizeof(PADS[0])) > k; k++ )
    {
        (void)gpio_reset_pin(PADS[k]);
        (void)gpio_set_direction(PADS[k], GPIO_MODE_INPUT);
        (void)gpio_set_pull_mode(PADS[k], GPIO_FLOATING);
    }
}

kode_status_t kdrv_dock_loop_run(uint8_t count, kdrv_dock_loop_t *out)
{
    uint32_t const t0 = khal_time_ms();
    uint32_t seen = 0U;
    int u;

    if ( (NULL == out) || (0U == count) )
    {
        return KODE_DENIED;
    }
    (void)memset(out, 0, sizeof(*out));
    u = loop_pick_uart();
    if ( (0 > u) || (uart_driver_install((uart_port_t)u, LOOP_RX_BUF, 0, 0, NULL, 0) != ESP_OK) )
    {
        out->why = (uint8_t)KDRV_DOCK_LOOP_UART_BUSY;
        out->ms  = khal_time_ms() - t0;
        return KODE_NO_RESOURCES;
    }
    (void)gpio_reset_pin((gpio_num_t)KBSP_PIN_DOCK_TX);
    (void)gpio_reset_pin((gpio_num_t)KBSP_PIN_DOCK_RX);
    out->slow = loop_pass(u, (int)KBSP_PIN_DOCK_TX, (int)KBSP_PIN_DOCK_RX, KDRV_DOCK_LOOP_BAUD_SLOW, count, &seen);
    out->fast = loop_pass(u, (int)KBSP_PIN_DOCK_TX, (int)KBSP_PIN_DOCK_RX, KDRV_DOCK_LOOP_BAUD_FAST, count, &seen);
    out->rev  = loop_pass(u, (int)KBSP_PIN_DOCK_RX, (int)KBSP_PIN_DOCK_TX, KDRV_DOCK_LOOP_BAUD_FAST, count, &seen);
    (void)uart_driver_delete((uart_port_t)u);
    loop_float_pads();
    out->ms = khal_time_ms() - t0;
    if ( (out->slow == count) && (out->fast == count) && (out->rev == count) )
    {
        out->why = (uint8_t)KDRV_DOCK_LOOP_OK;
        return KODE_OK;
    }
    if ( 0U == seen )
    {
        out->why = (uint8_t)KDRV_DOCK_LOOP_NO_ECHO;
    }
    else
    {
        out->why = (uint8_t)KDRV_DOCK_LOOP_CORRUPT;
    }
    return KODE_ERROR;
}
