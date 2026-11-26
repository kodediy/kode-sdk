/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file app_desc_stub.c
 * @brief The running image's description, for a binary the linux target
 *        links none into.
 */
#include <string.h>

#include "esp_app_desc.h"

/** @brief What the running image says it is: the project an update must name,
 *         and the head of its ELF SHA-256 (in hex, 5e1f0a7c93). */
static const esp_app_desc_t s_desc = {
    .project_name   = "kodeos",
    .version        = "0.0.0",
    .app_elf_sha256 = { 0x5EU, 0x1FU, 0x0AU, 0x7CU, 0x93U },
};

const esp_app_desc_t *esp_app_get_description(void)
{
    return &s_desc;
}

/**
 * @brief The running image's ELF SHA-256 in hex, as esp_app_format gives it:
 *        CONFIG_APP_RETRIEVE_LEN_ELF_SHA lowercase digits at most.
 */
int esp_app_get_elf_sha256(char *dst, size_t size)
{
    static const char HEX[] = "0123456789abcdef";
    char   sha[CONFIG_APP_RETRIEVE_LEN_ELF_SHA + 1];
    size_t n = sizeof(sha);

    if ( (NULL == dst) || (size < 2U) )
    {
        return 0;
    }
    for ( size_t i = 0U; i < (sizeof(sha) - 1U); ++i )
    {
        uint8_t const b = s_desc.app_elf_sha256[i / 2U];
        if ( 0U == (i % 2U) )
        {
            sha[i] = HEX[b >> 4];
        }
        else
        {
            sha[i] = HEX[b & 0x0FU];
        }
    }
    sha[sizeof(sha) - 1U] = '\0';
    if ( size < n )
    {
        n = size;
    }
    (void)memcpy(dst, sha, n);
    dst[n - 1U] = '\0';
    return (int)n;
}
