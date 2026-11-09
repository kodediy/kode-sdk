/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_sync_esp.c
 * @brief khal_sync on ESP-IDF: mutexes, binary semaphores and the C library's environment lock.
 */
#include "khal_sync.h"

#include <envlock.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/* Newlib keeps the environment and the zone under two locks, and holding one around tzset()
 * would order them against every other localtime_r() caller. */
#if !CONFIG_LIBC_PICOLIBC
#error "khal_env_lock() is picolibc's environment lock: build with CONFIG_LIBC_PICOLIBC (ESP-IDF's default with GCC)"
#endif

khal_mutex_t khal_mutex_create(void)
{
    return (khal_mutex_t)xSemaphoreCreateMutex();
}

void khal_mutex_lock(khal_mutex_t m)
{
    if ( NULL != m )
    {
        (void)xSemaphoreTake((SemaphoreHandle_t)m, portMAX_DELAY);
    }
}

void khal_mutex_unlock(khal_mutex_t m)
{
    if ( NULL != m )
    {
        (void)xSemaphoreGive((SemaphoreHandle_t)m);
    }
}

khal_sem_t khal_sem_create(void)
{
    return (khal_sem_t)xSemaphoreCreateBinary();
}

bool khal_sem_wait_timeout(khal_sem_t s, uint32_t ms)
{
    if ( NULL == s )
    {
        return false;
    }

    return xSemaphoreTake((SemaphoreHandle_t)s, pdMS_TO_TICKS(ms)) == pdTRUE;
}

void khal_sem_delete(khal_sem_t s)
{
    if ( NULL != s )
    {
        vSemaphoreDelete((SemaphoreHandle_t)s);
    }
}

void IRAM_ATTR khal_sem_signal(khal_sem_t s)
{
    if ( NULL == s )
    {
        return;
    }
    /* Task context: the plain give. */
    if ( xPortCanYield() )
    {
        (void)xSemaphoreGive((SemaphoreHandle_t)s);
        return;
    }
    BaseType_t woken = pdFALSE;
    (void)xSemaphoreGiveFromISR((SemaphoreHandle_t)s, &woken);
    if ( pdFALSE != woken )
    {
        portYIELD_FROM_ISR();
    }
}

void khal_env_lock(void)
{
    ENV_LOCK;
}

void khal_env_unlock(void)
{
    ENV_UNLOCK;
}
