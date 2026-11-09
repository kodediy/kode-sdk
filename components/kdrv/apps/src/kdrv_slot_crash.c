/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_slot_crash.c
 * @brief The register that keeps kdrv_slot_crashes() across a panic.
 */
#include "kdrv_slot.h"
#include "kdrv_slot_crash.h"

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "soc/lp_system_reg.h"
#include "soc/soc.h"

/* A register, not RTC memory: every slot finds it at the same address. STORE12..15 are taken,
 * and STORE11 is IDF's only for an LP core's wakeup cause, which this build never runs. */
#define SLOT_CRASH_REG  LP_SYSTEM_REG_LP_STORE11_REG

/** @brief The subtype of the slot running: what the word is kept for. */
static uint32_t crash_slot(void)
{
    const esp_partition_t * const run = esp_ota_get_running_partition();

    return (NULL != run) ? (uint32_t)run->subtype : (uint32_t)KSLOT_CRASH_FIELD;
}

bool kdrv_slot_crashes(uint32_t *count)
{
    return (NULL != count) && kslot_crash_read(REG_READ(SLOT_CRASH_REG), crash_slot(), count);
}

void kdrv_slot_crashes_set(uint32_t count)
{
    REG_WRITE(SLOT_CRASH_REG, kslot_crash_pack(crash_slot(), count));
}
