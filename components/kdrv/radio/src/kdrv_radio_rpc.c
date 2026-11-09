/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_radio_rpc.c
 * @brief One synchronous RPC to the C5 at a time, whoever asks.
 */
#include <stdbool.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kdrv_radio.h"
#include "kode_log.h"

#include "rpc_slave_if.h"

#define TAG "kdrv.radio"

/** @brief How long a request waits its turn: more than the two longest answers
 *         esp-hosted will wait for in a row, RPC timeouts being seconds. A turn
 *         that never comes goes ahead unlocked, as every request did before
 *         this file: a stuck peer must not wedge the radio for good. */
#define RADIO_RPC_TURN_MS  65000U

int         __real_rpc_send_req(ctrl_cmd_t *app_req);
ctrl_cmd_t *__real_rpc_wait_and_parse_sync_resp(ctrl_cmd_t *app_req);
int         __wrap_rpc_send_req(ctrl_cmd_t *app_req);
ctrl_cmd_t *__wrap_rpc_wait_and_parse_sync_resp(ctrl_cmd_t *app_req);

/* esp-hosted's request path holds no lock: two threads sending at once share a uid and a slot. */
static SemaphoreHandle_t s_turn;

/* cppcheck-suppress misra-c2012-8.7 ; the link's bring-up in kdrv_radio.c calls it */
void kdrv_radio_rpc_init(void)
{
    static StaticSemaphore_t s_turn_mem;

    if ( NULL == s_turn )
    {
        s_turn = xSemaphoreCreateMutexStatic(&s_turn_mem);
    }
}

/** @brief Take the turn. A request with no lock yet goes unlocked: nothing can
 *         be running beside it before the link's bring-up made the lock. */
static void rpc_turn_take(void)
{
    if ( (NULL != s_turn)
        && (xSemaphoreTake(s_turn, pdMS_TO_TICKS(RADIO_RPC_TURN_MS)) != pdTRUE) )
    {
        KLOG_E(TAG, "an RPC waited %u ms for its turn; it goes anyway",
               (unsigned)RADIO_RPC_TURN_MS);
    }
}

/** @brief Give the turn back, when this task has it. */
static void rpc_turn_give(void)
{
    if ( (NULL != s_turn)
        && (xSemaphoreGetMutexHolder(s_turn) == xTaskGetCurrentTaskHandle()) )
    {
        (void)xSemaphoreGive(s_turn);
    }
}

/**
 * @brief Send a request in its turn; a synchronous one that went out keeps the turn.
 *
 * The turn is given back by __wrap_rpc_wait_and_parse_sync_resp() once its answer is in.
 *
 * @param app_req The request; a failed send has freed it.
 * @return What esp-hosted's rpc_send_req() returned; 0 when the request went out.
 */
/* cppcheck-suppress misra-c2012-8.7 ; the linker's --wrap routes esp-hosted's own calls here */
int __wrap_rpc_send_req(ctrl_cmd_t *app_req)
{
    /* Read now: a send that fails has freed the request. */
    bool const is_sync = (NULL != app_req) && (NULL == app_req->rpc_rsp_cb);

    rpc_turn_take();
    int const rc = __real_rpc_send_req(app_req);

    /* A synchronous request that went out keeps the turn for its wait. */
    if ( (!is_sync) || (0 != rc) )
    {
        rpc_turn_give();
    }
    return rc;
}

/**
 * @brief Wait for a synchronous answer, then give the turn back.
 * @param app_req The request that was sent.
 * @return What esp-hosted's rpc_wait_and_parse_sync_resp() returned.
 */
/* cppcheck-suppress misra-c2012-8.7 ; the linker's --wrap routes esp-hosted's own calls here */
ctrl_cmd_t *__wrap_rpc_wait_and_parse_sync_resp(ctrl_cmd_t *app_req)
{
    ctrl_cmd_t * const rsp = __real_rpc_wait_and_parse_sync_resp(app_req);

    rpc_turn_give();
    return rsp;
}
