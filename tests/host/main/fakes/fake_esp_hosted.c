/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_esp_hosted.c
 * @brief The coprocessor as esp-hosted's host API sees it, answering from an editable table.
 */
#include <stdio.h>
#include <string.h>

#include "fakes.h"
#include "esp_hosted.h"
#include "esp_hosted_transport_config.h"

#define HOSTED_PINS 64U

static struct {
    uint32_t chip_id;
    char     target[16];
    uint32_t fw_major;
    uint32_t fw_minor;
    uint32_t fw_patch;
    char     project[32];
    char     version[32];
    char     idf_ver[32];
    char     date[16];
    char     time[16];
    uint8_t  sha[32];
    bool     app_desc_unsupported;
    uint8_t  mac[6];
    uint32_t sdio_khz;
    uint8_t  sdio_width;
    int      level[HOSTED_PINS];
    uint32_t mode[HOSTED_PINS];
    bool     gpio_dead;
    uint32_t heap_free;
    uint32_t heap_largest;
    bool     link_down;
    unsigned gpio_configs;
    uint32_t last_gpio_mode;
    unsigned mem_queries;
} s_h;

static struct esp_hosted_sdio_config s_sdio;

void fake_hosted_reset(void)
{
    (void)memset(&s_h, 0, sizeof(s_h));
    s_h.chip_id  = 23U;   /* CHIP_ESP32C5 */
    (void)snprintf(s_h.target, sizeof(s_h.target), "%s", "esp32c5");
    s_h.fw_major = (uint32_t)ESP_HOSTED_VERSION_MAJOR_1;
    s_h.fw_minor = (uint32_t)ESP_HOSTED_VERSION_MINOR_1;
    s_h.fw_patch = (uint32_t)ESP_HOSTED_VERSION_PATCH_1;
    (void)snprintf(s_h.project, sizeof(s_h.project), "%s", "kode_c5");
    (void)snprintf(s_h.version, sizeof(s_h.version), "%s", "2.12.9");
    (void)snprintf(s_h.idf_ver, sizeof(s_h.idf_ver), "%s", "v5.5.1");
    (void)snprintf(s_h.date, sizeof(s_h.date), "%s", "Sep  5 2026");
    (void)snprintf(s_h.time, sizeof(s_h.time), "%s", "10:00:00");
    for ( unsigned i = 0U; sizeof(s_h.sha) > i; ++i )
    {
        s_h.sha[i] = (uint8_t)(0xA0U + i);
    }
    s_h.mac[0] = 0x38U; s_h.mac[1] = 0x44U; s_h.mac[2] = 0xBEU;
    s_h.mac[3] = 0x1CU; s_h.mac[4] = 0x1DU; s_h.mac[5] = 0x7EU;
    s_h.sdio_khz   = 40000U;
    s_h.sdio_width = 4U;
    /* IO26 low: the card powered. IO25 high: what a pull-up reads with
     * nothing pulling it, which is all that is known about that pin. */
    for ( unsigned i = 0U; HOSTED_PINS > i; ++i )
    {
        s_h.level[i] = 1;
    }
    s_h.level[26] = 0;
    s_h.heap_free    = 180U * 1024U;
    s_h.heap_largest = 96U * 1024U;
}

void fake_hosted_set_chip(uint32_t id, const char *target)
{
    s_h.chip_id = id;
    (void)snprintf(s_h.target, sizeof(s_h.target), "%s", target);
}

void fake_hosted_set_fw(uint32_t major, uint32_t minor, uint32_t patch)
{
    s_h.fw_major = major;
    s_h.fw_minor = minor;
    s_h.fw_patch = patch;
}

void fake_hosted_set_project(const char *name)
{
    (void)snprintf(s_h.project, sizeof(s_h.project), "%s", name);
}

void fake_hosted_set_app_strings(const char *version, const char *idf_ver,
                                 const char *date, const char *time)
{
    (void)snprintf(s_h.version, sizeof(s_h.version), "%s", version);
    (void)snprintf(s_h.idf_ver, sizeof(s_h.idf_ver), "%s", idf_ver);
    (void)snprintf(s_h.date, sizeof(s_h.date), "%s", date);
    (void)snprintf(s_h.time, sizeof(s_h.time), "%s", time);
}

void fake_hosted_app_desc_unsupported(bool unsupported)
{
    s_h.app_desc_unsupported = unsupported;
}

void fake_hosted_set_mac(const uint8_t mac[6])
{
    (void)memcpy(s_h.mac, mac, sizeof(s_h.mac));
}

void fake_hosted_set_sdio(uint32_t khz, uint8_t width)
{
    s_h.sdio_khz   = khz;
    s_h.sdio_width = width;
}

void fake_hosted_set_level(uint32_t pin, int level)
{
    if ( HOSTED_PINS > pin )
    {
        s_h.level[pin] = level;
    }
}

void fake_hosted_gpio_dead(bool dead)
{
    s_h.gpio_dead = dead;
}

void fake_hosted_set_heap(uint32_t free_bytes, uint32_t largest_bytes)
{
    s_h.heap_free    = free_bytes;
    s_h.heap_largest = largest_bytes;
}

void fake_hosted_link_down(bool down)
{
    s_h.link_down = down;
}

unsigned fake_hosted_gpio_configs(void)
{
    return s_h.gpio_configs;
}

uint32_t fake_hosted_last_gpio_mode(void)
{
    return s_h.last_gpio_mode;
}

unsigned fake_hosted_mem_queries(void)
{
    return s_h.mem_queries;
}

int fake_hosted_level(uint32_t pin)
{
    if ( HOSTED_PINS <= pin )
    {
        return -1;
    }
    return s_h.level[pin];
}

/* ---- The API ------------------------------------------------------------- */

int esp_hosted_get_coprocessor_fwversion(esp_hosted_coprocessor_fwver_t *ver_info)
{
    if ( s_h.link_down || (NULL == ver_info) )
    {
        return ESP_ERR_INVALID_STATE;
    }
    ver_info->major1     = s_h.fw_major;
    ver_info->minor1     = s_h.fw_minor;
    ver_info->patch1     = s_h.fw_patch;
    ver_info->revision   = -1;
    ver_info->prerelease = -1;
    ver_info->build      = -1;
    return ESP_OK;
}

int esp_hosted_get_cp_info(uint32_t *cp_chip_id, char *cp_target_name,
                           size_t cp_target_name_len)
{
    if ( s_h.link_down || (NULL == cp_chip_id) || (NULL == cp_target_name) )
    {
        return ESP_ERR_INVALID_STATE;
    }
    *cp_chip_id = s_h.chip_id;
    (void)snprintf(cp_target_name, cp_target_name_len, "%s", s_h.target);
    return ESP_OK;
}

esp_err_t esp_hosted_iface_mac_addr_get(uint8_t *mac, size_t mac_len,
                                        esp_mac_type_t type)
{
    (void)type;
    if ( s_h.link_down || (NULL == mac) || (sizeof(s_h.mac) > mac_len) )
    {
        return ESP_ERR_INVALID_STATE;
    }
    (void)memcpy(mac, s_h.mac, sizeof(s_h.mac));
    return ESP_OK;
}

esp_err_t esp_hosted_get_coprocessor_app_desc(esp_hosted_app_desc_t *app_desc)
{
    if ( s_h.link_down || (NULL == app_desc) )
    {
        return ESP_ERR_INVALID_STATE;
    }
    if ( s_h.app_desc_unsupported )
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    (void)memset(app_desc, 0, sizeof(*app_desc));
    (void)snprintf(app_desc->project_name, sizeof(app_desc->project_name),
                   "%s", s_h.project);
    (void)snprintf(app_desc->version, sizeof(app_desc->version),
                   "%s", s_h.version);
    (void)snprintf(app_desc->idf_ver, sizeof(app_desc->idf_ver),
                   "%s", s_h.idf_ver);
    (void)snprintf(app_desc->date, sizeof(app_desc->date), "%s", s_h.date);
    (void)snprintf(app_desc->time, sizeof(app_desc->time), "%s", s_h.time);
    (void)memcpy(app_desc->app_elf_sha256, s_h.sha,
                 sizeof(app_desc->app_elf_sha256));
    return ESP_OK;
}

esp_err_t esp_hosted_set_mem_monitor(esp_hosted_config_mem_monitor_t *config,
                                     esp_hosted_curr_mem_info_t *curr_mem_info)
{
    if ( s_h.link_down || (NULL == config) || (NULL == curr_mem_info) )
    {
        return ESP_ERR_INVALID_STATE;
    }
    ++s_h.mem_queries;
    (void)memset(curr_mem_info, 0, sizeof(*curr_mem_info));
    curr_mem_info->config = config->config;
    curr_mem_info->curr_internal.cap_8bit.free_size          = s_h.heap_free;
    curr_mem_info->curr_internal.cap_8bit.largest_free_block = s_h.heap_largest;
    curr_mem_info->curr_internal.cap_dma.free_size           = s_h.heap_free;
    curr_mem_info->curr_internal.cap_dma.largest_free_block  = s_h.heap_largest;
    curr_mem_info->curr_total_heap_size = s_h.heap_free + (128U * 1024U);
    return ESP_OK;
}

esp_hosted_transport_err_t esp_hosted_sdio_get_config(struct esp_hosted_sdio_config **config)
{
    if ( s_h.link_down || (NULL == config) )
    {
        return ESP_TRANSPORT_ERR_INVALID_STATE;
    }
    (void)memset(&s_sdio, 0, sizeof(s_sdio));
    s_sdio.clock_freq_khz = s_h.sdio_khz;
    s_sdio.bus_width      = s_h.sdio_width;
    *config = &s_sdio;
    return ESP_TRANSPORT_OK;
}

esp_err_t esp_hosted_cp_gpio_config(const esp_hosted_cp_gpio_config_t *pGPIOConfig)
{
    if ( s_h.link_down || s_h.gpio_dead || (NULL == pGPIOConfig) )
    {
        return ESP_ERR_INVALID_STATE;
    }
    ++s_h.gpio_configs;
    s_h.last_gpio_mode = pGPIOConfig->mode;
    for ( unsigned i = 0U; HOSTED_PINS > i; ++i )
    {
        if ( (pGPIOConfig->pin_bit_mask & (1ULL << i)) != 0ULL )
        {
            s_h.mode[i] = pGPIOConfig->mode;
        }
    }
    return ESP_OK;
}

esp_err_t esp_hosted_cp_gpio_set_level(uint32_t gpio_num, uint32_t level)
{
    if ( s_h.link_down || s_h.gpio_dead || (HOSTED_PINS <= gpio_num) )
    {
        return ESP_ERR_INVALID_STATE;
    }
    if ( 0U != level )
    {
        s_h.level[gpio_num] = 1;
    }
    else
    {
        s_h.level[gpio_num] = 0;
    }
    return ESP_OK;
}

esp_err_t esp_hosted_cp_gpio_get_level(uint32_t gpio_num, int *level)
{
    if ( s_h.link_down || s_h.gpio_dead || (HOSTED_PINS <= gpio_num)
        || (NULL == level) )
    {
        return ESP_ERR_INVALID_STATE;
    }
    /* The C5's own rule, reproduced because the driver's comment leans on
     * it: a pin configured OUTPUT only has its input buffer off and reads 0
     * whatever it drives. A pin nobody configured through this API reads
     * what the table says - that is the slave's own boot-time configuration
     * standing in. */
    if ( (uint32_t)H_CP_GPIO_MODE_OUTPUT == s_h.mode[gpio_num] )
    {
        *level = 0;
    }
    else
    {
        *level = s_h.level[gpio_num];
    }
    return ESP_OK;
}

esp_err_t esp_hosted_cp_gpio_input_enable(uint32_t gpio_num)
{
    if ( s_h.link_down || s_h.gpio_dead || (HOSTED_PINS <= gpio_num) )
    {
        return ESP_ERR_INVALID_STATE;
    }
    /* An output with its input switched on reads what it drives. */
    if ( (uint32_t)H_CP_GPIO_MODE_OUTPUT == s_h.mode[gpio_num] )
    {
        s_h.mode[gpio_num] = (uint32_t)H_CP_GPIO_MODE_INPUT_OUTPUT;
    }
    return ESP_OK;
}
