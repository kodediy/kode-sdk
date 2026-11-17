/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_nvs_esp.c
 * @brief khal_nvs on ESP-IDF's NVS: the `nvs` partition.
 */
#include "khal_nvs.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "khal_system.h"
#include "kode_log.h"

#define TAG "khal.nvs"

/** @brief Bytes of one entry of the store: a key's header, or that much of its string. */
#define NVS_ENTRY_BYTES  32U

static bool s_ready;
static bool s_erased;   /* khal_nvs_init() erased the partition in this boot */

/**
 * @brief Whether the caller runs on a stack in PSRAM, which NVS must not see.
 *
 * Every NVS call reaches the internal flash through SPI1, which switches the
 * cache off, and a task whose stack is in PSRAM cannot run with it off. The
 * refusal is logged here so each entry point only returns it.
 *
 * @param name Name of the public call, for the log.
 * @return true when the call must be refused (KODE_DENIED).
 */
static bool stack_refused(char const *name)
{
    if ( !khal_task_stack_in_psram() )
    {
        return false;
    }
    KLOG_E(TAG, "%s refused: task '%s' has its stack in PSRAM and NVS "
                "reaches the flash",
           name, pcTaskGetName(NULL));
    return true;
}

kode_status_t khal_nvs_init(void)
{
    if ( stack_refused("khal_nvs_init") )
    {
        return KODE_DENIED;
    }

    if ( s_ready )
    {
        return KODE_OK;
    }

    esp_err_t err = nvs_flash_init();
    if ( (ESP_ERR_NVS_NO_FREE_PAGES == err)
        || (ESP_ERR_NVS_NEW_VERSION_FOUND == err) )
    {
        /* Erased whole: settings and app data go back to their defaults. */
        KLOG_W(TAG, "the settings store cannot be read as it is (%s): erasing "
                    "it, every setting goes back to its default",
               esp_err_to_name(err));
        s_erased = (nvs_flash_erase() == ESP_OK);
        err      = nvs_flash_init();
    }
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "nvs unavailable: %s", esp_err_to_name(err));
        return KODE_ERROR;
    }
    s_ready = true;
    return KODE_OK;
}

kode_status_t khal_nvs_get_str(const char *ns, const char *key,
                               char *out, size_t cap)
{
    if ( stack_refused("khal_nvs_get_str") )
    {
        return KODE_DENIED;
    }

    if ( (khal_nvs_init() != KODE_OK) || (NULL == out) || (0U == cap) )
    {
        return KODE_ERROR;
    }

    nvs_handle_t h;
    if ( nvs_open(ns, NVS_READONLY, &h) != ESP_OK )
    {
        /* No namespace yet: nothing was stored. */
        return KODE_DENIED;
    }

    size_t          len = cap;
    esp_err_t const err = nvs_get_str(h, key, out, &len);
    nvs_close(h);

    if ( ESP_ERR_NVS_NOT_FOUND == err )
    {
        return KODE_DENIED;
    }
    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "get %s/%s: %s", ns, key, esp_err_to_name(err));
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t khal_nvs_set_str(const char *ns, const char *key,
                               const char *value)
{
    if ( stack_refused("khal_nvs_set_str") )
    {
        return KODE_DENIED;
    }

    if ( (khal_nvs_init() != KODE_OK) || (NULL == value) )
    {
        return KODE_ERROR;
    }

    nvs_handle_t h;
    if ( nvs_open(ns, NVS_READWRITE, &h) != ESP_OK )
    {
        return KODE_ERROR;
    }

    esp_err_t err = nvs_set_str(h, key, value);
    if ( ESP_OK == err )
    {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "set %s/%s: %s", ns, key, esp_err_to_name(err));
        return KODE_ERROR;
    }
    return KODE_OK;
}

bool khal_nvs_erased(void)
{
    return s_erased;
}

/**
 * @brief Entries a string takes in the store: its header, then its bytes in
 *        whole entries.
 *
 * @param size The string's length, NUL included.
 * @return How many entries.
 */
static size_t str_entries(size_t size)
{
    return 1U + ((size + NVS_ENTRY_BYTES - 1U) / NVS_ENTRY_BYTES);
}

/**
 * @brief How many bytes @p ns would take once @p value is under @p key.
 *
 * @param ns    Namespace.
 * @param key   Key.
 * @param value The string to be written.
 * @param after Where the bytes go on KODE_OK.
 * @return KODE_OK; KODE_ERROR when the store could not count @p ns.
 */
static kode_status_t str_bytes_after(const char *ns, const char *key,
                                     const char *value, size_t *after)
{
    nvs_handle_t h;
    size_t       used = 0U;   /* entries @p ns takes now */
    esp_err_t    err  = ESP_OK;

    /* No namespace yet: it takes nothing. */
    if ( nvs_open(ns, NVS_READONLY, &h) == ESP_OK )
    {
        size_t held = 0U;     /* the string @p key holds now, NUL included */

        err = nvs_get_used_entry_count(h, &used);
        if ( (ESP_OK == err) && (nvs_get_str(h, key, NULL, &held) == ESP_OK)
            && (str_entries(held) <= used) )
        {
            /* The write frees what the key held. */
            used -= str_entries(held);
        }
        nvs_close(h);
    }
    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "count %s: %s", ns, esp_err_to_name(err));
        return KODE_ERROR;
    }
    *after = (used + str_entries(strlen(value) + 1U)) * NVS_ENTRY_BYTES;
    return KODE_OK;
}

kode_status_t khal_nvs_set_str_within(const char *ns, const char *key,
                                      const char *value, size_t share)
{
    size_t after = 0U;

    if ( stack_refused("khal_nvs_set_str_within") )
    {
        return KODE_DENIED;
    }

    if ( (khal_nvs_init() != KODE_OK) || (NULL == value) )
    {
        return KODE_ERROR;
    }

    if ( str_bytes_after(ns, key, value, &after) != KODE_OK )
    {
        return KODE_ERROR;
    }
    if ( after > share )
    {
        return KODE_NO_RESOURCES;
    }
    return khal_nvs_set_str(ns, key, value);
}

kode_status_t khal_nvs_get_u8(const char *ns, const char *key, uint8_t *out)
{
    if ( stack_refused("khal_nvs_get_u8") )
    {
        return KODE_DENIED;
    }

    if ( (khal_nvs_init() != KODE_OK) || (NULL == out) )
    {
        return KODE_ERROR;
    }

    nvs_handle_t h;
    if ( nvs_open(ns, NVS_READONLY, &h) != ESP_OK )
    {
        /* No namespace yet: nothing was stored. */
        return KODE_DENIED;
    }

    esp_err_t const err = nvs_get_u8(h, key, out);
    nvs_close(h);

    if ( ESP_ERR_NVS_NOT_FOUND == err )
    {
        return KODE_DENIED;
    }
    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "get %s/%s: %s", ns, key, esp_err_to_name(err));
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t khal_nvs_set_u8(const char *ns, const char *key, uint8_t value)
{
    if ( stack_refused("khal_nvs_set_u8") )
    {
        return KODE_DENIED;
    }

    if ( khal_nvs_init() != KODE_OK )
    {
        return KODE_ERROR;
    }

    nvs_handle_t h;
    if ( nvs_open(ns, NVS_READWRITE, &h) != ESP_OK )
    {
        return KODE_ERROR;
    }

    esp_err_t err = nvs_set_u8(h, key, value);
    if ( ESP_OK == err )
    {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "set %s/%s: %s", ns, key, esp_err_to_name(err));
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t khal_nvs_get_u64(const char *ns, const char *key, uint64_t *out)
{
    if ( (khal_nvs_init() != KODE_OK) || (NULL == out) )
    {
        return KODE_ERROR;
    }

    nvs_handle_t h;
    if ( nvs_open(ns, NVS_READONLY, &h) != ESP_OK )
    {
        /* No namespace yet: nothing was stored. */
        return KODE_DENIED;
    }

    esp_err_t const err = nvs_get_u64(h, key, out);
    nvs_close(h);

    if ( ESP_ERR_NVS_NOT_FOUND == err )
    {
        return KODE_DENIED;
    }
    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "get %s/%s: %s", ns, key, esp_err_to_name(err));
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t khal_nvs_set_u64(const char *ns, const char *key, uint64_t value)
{
    if ( khal_nvs_init() != KODE_OK )
    {
        return KODE_ERROR;
    }

    nvs_handle_t h;
    if ( nvs_open(ns, NVS_READWRITE, &h) != ESP_OK )
    {
        return KODE_ERROR;
    }

    esp_err_t err = nvs_set_u64(h, key, value);
    if ( ESP_OK == err )
    {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "set %s/%s: %s", ns, key, esp_err_to_name(err));
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t khal_nvs_get_blob(const char *ns, const char *key,
                                void *out, size_t *len)
{
    if ( stack_refused("khal_nvs_get_blob") )
    {
        return KODE_DENIED;
    }

    if ( (khal_nvs_init() != KODE_OK) || (NULL == out) || (NULL == len) )
    {
        return KODE_ERROR;
    }

    nvs_handle_t h;
    if ( nvs_open(ns, NVS_READONLY, &h) != ESP_OK )
    {
        return KODE_DENIED;
    }

    esp_err_t const err = nvs_get_blob(h, key, out, len);
    nvs_close(h);

    if ( ESP_ERR_NVS_NOT_FOUND == err )
    {
        return KODE_DENIED;
    }
    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "get %s/%s: %s", ns, key, esp_err_to_name(err));
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t khal_nvs_set_blob(const char *ns, const char *key,
                                const void *value, size_t len)
{
    if ( stack_refused("khal_nvs_set_blob") )
    {
        return KODE_DENIED;
    }

    if ( (khal_nvs_init() != KODE_OK) || (NULL == value) || (0U == len) )
    {
        return KODE_ERROR;
    }

    nvs_handle_t h;
    if ( nvs_open(ns, NVS_READWRITE, &h) != ESP_OK )
    {
        return KODE_ERROR;
    }

    esp_err_t err = nvs_set_blob(h, key, value, len);
    if ( ESP_OK == err )
    {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "set %s/%s: %s", ns, key, esp_err_to_name(err));
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t khal_nvs_erase(const char *ns, const char *key)
{
    if ( stack_refused("khal_nvs_erase") )
    {
        return KODE_DENIED;
    }

    if ( khal_nvs_init() != KODE_OK )
    {
        return KODE_ERROR;
    }

    nvs_handle_t h;
    if ( nvs_open(ns, NVS_READWRITE, &h) != ESP_OK )
    {
        /* Nothing there: the postcondition holds. */
        return KODE_OK;
    }

    esp_err_t err = nvs_erase_key(h, key);
    if ( ESP_ERR_NVS_NOT_FOUND == err )
    {
        err = ESP_OK;
    }
    if ( ESP_OK == err )
    {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if ( ESP_OK != err )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t khal_nvs_key(const char *ns, uint16_t idx, char *out, size_t cap)
{
    if ( stack_refused("khal_nvs_key") )
    {
        return KODE_DENIED;
    }

    if ( (khal_nvs_init() != KODE_OK) || (NULL == out) || (0U == cap) )
    {
        return KODE_ERROR;
    }

    nvs_iterator_t it  = NULL;
    esp_err_t      err = nvs_entry_find(NVS_DEFAULT_PART_NAME, ns,
                                        NVS_TYPE_ANY, &it);

    /* From the start every time: no iterator for the caller to leak. */
    for ( uint16_t i = 0U; (ESP_OK == err) && (i < idx); ++i )
    {
        err = nvs_entry_next(&it);
    }

    if ( ESP_OK != err )
    {
        /* IDF clears the iterator on any error but ESP_ERR_INVALID_ARG, and
         * releasing NULL is allowed, so this covers both exits. */
        nvs_release_iterator(it);
        if ( ESP_ERR_NVS_NOT_FOUND == err )
        {
            return KODE_DENIED;
        }
        return KODE_ERROR;
    }

    nvs_entry_info_t info;
    err = nvs_entry_info(it, &info);
    nvs_release_iterator(it);

    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "list %s[%u]: %s", ns, (unsigned)idx, esp_err_to_name(err));
        return KODE_ERROR;
    }

    size_t const n = strlen(info.key);
    if ( n >= cap )
    {
        /* A truncated key opens nothing. */
        return KODE_ERROR;
    }
    (void)memcpy(out, info.key, n + 1U);
    return KODE_OK;
}

/**
 * @brief Whether an entry's namespace was already met earlier in the walk.
 *
 * @param ns    The namespace to look for.
 * @param upto  How many entries of the walk, from the start, to look at.
 * @return true when one of them is in @p ns.
 */
static bool namespace_met_before(const char *ns, uint16_t upto)
{
    nvs_iterator_t it  = NULL;
    esp_err_t      err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NULL,
                                        NVS_TYPE_ANY, &it);
    bool           met = false;

    for ( uint16_t i = 0U; (ESP_OK == err) && (i < upto) && !met; ++i )
    {
        nvs_entry_info_t info;

        if ( nvs_entry_info(it, &info) == ESP_OK )
        {
            met = (strcmp(info.namespace_name, ns) == 0);
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return met;
}

kode_status_t khal_nvs_namespace(uint16_t idx, char *out, size_t cap)
{
    if ( stack_refused("khal_nvs_namespace") )
    {
        return KODE_DENIED;
    }

    if ( (khal_nvs_init() != KODE_OK) || (NULL == out) || (0U == cap) )
    {
        return KODE_ERROR;
    }

    nvs_iterator_t it  = NULL;
    esp_err_t      err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NULL,
                                        NVS_TYPE_ANY, &it);
    uint16_t       pos = 0U;   /* entries walked so far */
    uint16_t       n   = 0U;   /* distinct namespaces met so far */

    /* No table: the idx-th distinct name is the entry whose namespace no
     * earlier entry of the walk carries. */
    while ( ESP_OK == err )
    {
        nvs_entry_info_t info;

        err = nvs_entry_info(it, &info);
        if ( ESP_OK != err )
        {
            break;
        }
        if ( !namespace_met_before(info.namespace_name, pos) )
        {
            if ( n == idx )
            {
                nvs_release_iterator(it);
                size_t const len = strlen(info.namespace_name);
                if ( len >= cap )
                {
                    /* A truncated name opens nothing. */
                    return KODE_ERROR;
                }
                (void)memcpy(out, info.namespace_name, len + 1U);
                return KODE_OK;
            }
            ++n;
        }
        ++pos;
        err = nvs_entry_next(&it);
    }

    nvs_release_iterator(it);
    if ( ESP_ERR_NVS_NOT_FOUND == err )
    {
        return KODE_DENIED;
    }
    KLOG_W(TAG, "list namespace %u: %s", (unsigned)idx, esp_err_to_name(err));
    return KODE_ERROR;
}

kode_status_t khal_nvs_erase_namespace(const char *ns)
{
    if ( stack_refused("khal_nvs_erase_namespace") )
    {
        return KODE_DENIED;
    }

    if ( (khal_nvs_init() != KODE_OK) || (NULL == ns) )
    {
        return KODE_ERROR;
    }

    nvs_handle_t h;
    if ( nvs_open(ns, NVS_READWRITE, &h) != ESP_OK )
    {
        return KODE_ERROR;
    }

    esp_err_t err = nvs_erase_all(h);
    if ( ESP_OK == err )
    {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "erase %s: %s", ns, esp_err_to_name(err));
        return KODE_ERROR;
    }
    return KODE_OK;
}
