/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_flash.c
 * @brief The firmware image command: posted to AO_AppMgr.
 */
#include "kode_sys.h"
#include "kode_sys_priv.h"

#include "kaos.h"
#include "sdkconfig.h"

#include <string.h>

#if CONFIG_KODEOS_APPMGR
#define KODE_SYS_FLASH_PREFIX_LEN  3U          /**< The length of the "fs:" and "sd:" prefixes. */
#define KODE_SYS_FLASH_HTTP        "http://"   /**< The scheme of a plain download. */
#define KODE_SYS_FLASH_HTTPS       "https://"  /**< The scheme of a TLS download. */
#endif

kode_status_t kode_sys_flash(const char *src, const char *dst, bool run)
{
#if CONFIG_KODEOS_APPMGR
    if ( (NULL == src) || (NULL == dst) )
    {
        return KODE_DENIED;
    }
    if ( strlen(src) >= KODE_FLASH_SRC_MAX )
    {
        return KODE_DENIED;   /* refused rather than cut: a cut URL is another URL */
    }

    uint8_t     src_kind;
    const char *spec = src;
    if ( strncmp(spec, "fs:", KODE_SYS_FLASH_PREFIX_LEN) == 0 )
    {
        src_kind = (uint8_t)KFW_SRC_FS;  spec += KODE_SYS_FLASH_PREFIX_LEN;
    }
    else if ( strncmp(spec, "sd:", KODE_SYS_FLASH_PREFIX_LEN) == 0 )
    {
        src_kind = (uint8_t)KFW_SRC_SD;  spec += KODE_SYS_FLASH_PREFIX_LEN;
    }
    else if ( (strncmp(spec, KODE_SYS_FLASH_HTTP, sizeof(KODE_SYS_FLASH_HTTP) - 1U) == 0)
               || (strncmp(spec, KODE_SYS_FLASH_HTTPS, sizeof(KODE_SYS_FLASH_HTTPS) - 1U) == 0) )
    {
        src_kind = (uint8_t)KFW_SRC_HTTP;   /* the URL travels whole */
    }
    else
    {
        src_kind = (uint8_t)KFW_SRC_FS;  /* a bare name: the media, in order */
    }
    if ( '\0' == *spec )
    {
        return KODE_DENIED;
    }

    uint8_t dst_kind;
    if ( strcmp(dst, "os") == 0 )
    {
        dst_kind = (uint8_t)KFW_TARGET_OS;
    }
    else if ( strcmp(dst, "app") == 0 )
    {
        dst_kind = (uint8_t)KFW_TARGET_APP;
    }
    else
    {
        return KODE_DENIED;   /* "c5" belongs to AO_Radio, not here */
    }

    return kode_sys_cmd_post(AO_AppMgr, appmgr_image_evt(src_kind, spec, dst_kind, run),
                             KQP_APPMGR_CMD_MARGIN);
#else
    (void)src; (void)dst; (void)run;
    return KODE_DENIED;
#endif
}
