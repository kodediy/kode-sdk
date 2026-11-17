/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_http_file.c
 * @brief The download into a file: its two doors.
 */
#include "kode_sys.h"

#include "kode_sys_priv.h"

#include "kaos.h"
#include "sdkconfig.h"

/* In a file of its own so that only an app that downloads links AO_Storage's
 * half of it, and AO_Net with it. */

#if CONFIG_KODEOS_NET

_Static_assert(KODE_SYS_HTTP_FILE_URL_MAX == KODE_NET_FETCH_URL_MAX,
               "a URL the facade takes is one the download carries");

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_http_get_file(QActive *me, const char *url, const char *path)
{
    if ( (NULL == me) || !kode_sys_text_fits(url, KODE_SYS_HTTP_FILE_URL_MAX)
        || !kode_sys_text_fits(path, KODE_SYS_FILE_PATH_MAX) )
    {
        return KODE_DENIED;
    }
    return storage_fetch_ask(me, url, path);
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_http_file_stop(QActive *me)
{
    if ( NULL == me )
    {
        return KODE_DENIED;
    }
    return storage_fetch_stop(me);
}

#else /* !CONFIG_KODEOS_NET */

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_http_get_file(QActive *me, const char *url, const char *path)
{
    (void)me; (void)url; (void)path;
    return KODE_DENIED;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_http_file_stop(QActive *me)
{
    (void)me;
    return KODE_DENIED;
}

#endif /* CONFIG_KODEOS_NET */
