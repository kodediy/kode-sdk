/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_net.c
 * @brief The network: the requests through AO_Net, and the blocking one-shots on
 *        kdrv_net in the caller's task; without CONFIG_KODEOS_NET both link and refuse.
 */
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_sys_priv.h"

#include "kaos.h"
#include "kdrv_net.h"   /* a socket has no owner to reach past */
#include "sdkconfig.h"

#include <stddef.h>
#include <string.h>

#if CONFIG_KODEOS_NET

kode_status_t kode_block_net_lookup(const char *host, uint32_t *ip4)
{
    return kdrv_net_lookup(host, ip4);
}

kode_status_t kode_block_net_ping(const char *host, uint8_t count,
                            kode_sys_ping_stats_t *stats)
{
    if ( NULL == stats )
    {
        return KODE_ERROR;
    }

    /* Cleared first: the driver writes it only once its own checks pass. */
    kdrv_net_ping_stats_t s;
    (void)memset(&s, 0, sizeof(s));

    /* No rows: a caller that wants every echo uses an AO and NET_PING_ENTRY_SIG. */
    kode_status_t const rc = kdrv_net_ping(host, count, NULL, 0U, NULL, &s);

    stats->ip4    = s.ip4;
    stats->tx     = s.tx;
    stats->rx     = s.rx;
    stats->min_ms = s.min_ms;
    stats->avg_ms = s.avg_ms;
    stats->max_ms = s.max_ms;
    return rc;
}

kode_status_t kode_block_http_get_buffer(const char *url, void *buf, uint32_t cap,
                            kode_sys_http_result_t *out)
{
    if ( NULL == out )
    {
        return KODE_ERROR;
    }

    /* Cleared first, as in kode_block_net_ping(). */
    kdrv_net_http_result_t r;
    (void)memset(&r, 0, sizeof(r));
    r.content_length = -1;

    kode_status_t const rc = kdrv_net_http_get(url, (uint8_t *)buf, cap, &r,
                                               KODE_HTTP_TIMEOUT_MS);

    out->status         = r.status;
    out->content_length = r.content_length;
    out->body_len       = r.body_len;
    out->kept           = r.kept;
    out->truncated      = r.truncated;
    return rc;
}

kode_status_t kode_block_http_post_buffer(const char *url, const void *body,
                            uint32_t len, const char *ctype, void *buf,
                            uint32_t cap, kode_sys_http_result_t *out)
{
    if ( NULL == out )
    {
        return KODE_ERROR;
    }

    kdrv_net_http_result_t r;
    (void)memset(&r, 0, sizeof(r));
    r.content_length = -1;

    const char *const send_body = body;
    uint8_t *const    recv_buf  = buf;

    kode_status_t rc = KODE_ERROR;
    if ( NULL != url )
    {
        rc = kdrv_net_http_post(url, send_body, len, ctype, recv_buf, cap, &r,
                                KODE_HTTP_TIMEOUT_MS);
    }

    out->status         = r.status;
    out->content_length = r.content_length;
    out->body_len       = r.body_len;
    out->kept           = r.kept;
    out->truncated      = r.truncated;
    return rc;
}

kode_status_t kode_block_udp_send(const char *host, uint16_t port,
                            const void *data, uint32_t len)
{
    if ( (uint32_t)UINT16_MAX < len )
    {
        return KODE_ERROR;   /* one datagram, and this is its ceiling */
    }
    return kdrv_net_udp_send(host, port, data, (uint16_t)len);
}

/* ---- kode_sys's requests: AO_Net answers the asking AO ---- */

_Static_assert(KODE_SYS_NET_HOST_MAX == KODE_NET_HOST_MAX, "a host the facade takes is one a request carries");
_Static_assert(KODE_SYS_NET_URL_MAX == KODE_NET_URL_MAX, "a URL the facade takes is one a request carries");
_Static_assert(KODE_SYS_NET_CTYPE_MAX == KODE_NET_CTYPE_MAX, "a type the facade takes is one a request carries");
_Static_assert(KODE_SYS_NET_PING_MAX == KODE_NET_PING_MAX, "a ping the facade takes is one AO_Net runs");

/* ---- The answers' payloads against the driver's types they copy ---- */
_Static_assert(sizeof(kode_sys_http_result_t) == sizeof(kdrv_net_http_result_t), "the result is the driver's");
_Static_assert(offsetof(kode_sys_http_result_t, truncated) == offsetof(kdrv_net_http_result_t, truncated),
               "the result's flag where the driver's is");

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_net_lookup(QActive *me, const char *host)
{
    if ( !kode_sys_text_fits(host, KODE_SYS_NET_HOST_MAX) )
    {
        return KODE_DENIED;
    }
    return net_lookup_ask(me, host);
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_net_ping(QActive *me, const char *host, uint8_t count)
{
    if ( !kode_sys_text_fits(host, KODE_SYS_NET_HOST_MAX) || (0U == count)
        || (KODE_SYS_NET_PING_MAX < count) )
    {
        return KODE_DENIED;
    }
    return net_ping_ask(me, host, count);
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_http_get(QActive *me, const char *url, void *buf, uint32_t cap)
{
    if ( !kode_sys_text_fits(url, KODE_SYS_NET_URL_MAX) )
    {
        return KODE_DENIED;
    }
    return net_http_ask(me, false, url, NULL, 0U, NULL, buf, cap);
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_http_post(QActive *me, const char *url, const void *body, uint32_t len,
                                 const char *ctype, void *buf, uint32_t cap)
{
    if ( !kode_sys_text_fits(url, KODE_SYS_NET_URL_MAX) || ((NULL == body) && (0U < len)) )
    {
        return KODE_DENIED;
    }
    if ( (NULL != ctype) && !kode_sys_text_fits(ctype, KODE_SYS_NET_CTYPE_MAX) )
    {
        return KODE_DENIED;
    }
    return net_http_ask(me, true, url, body, len, ctype, buf, cap);
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_udp_send(QActive *me, const char *host, uint16_t port, const void *data,
                                uint16_t len)
{
    if ( !kode_sys_text_fits(host, KODE_SYS_NET_HOST_MAX) || ((NULL == data) && (0U < len)) )
    {
        return KODE_DENIED;
    }
    return net_udp_ask(me, host, port, data, len);
}

#else  /* !CONFIG_KODEOS_NET: the surface links and refuses */

kode_status_t kode_block_net_lookup(const char *host, uint32_t *ip4)
{
    (void)host; (void)ip4;
    return KODE_DENIED;
}

kode_status_t kode_block_net_ping(const char *host, uint8_t count,
                            kode_sys_ping_stats_t *stats)
{
    (void)host; (void)count; (void)stats;
    return KODE_DENIED;
}

kode_status_t kode_block_http_get_buffer(const char *url, void *buf, uint32_t cap,
                            kode_sys_http_result_t *out)
{
    (void)url; (void)buf; (void)cap; (void)out;
    return KODE_DENIED;
}

kode_status_t kode_block_http_post_buffer(const char *url, const void *body,
                            uint32_t len, const char *ctype, void *buf,
                            uint32_t cap, kode_sys_http_result_t *out)
{
    (void)url; (void)body; (void)len; (void)ctype; (void)buf; (void)cap;
    (void)out;
    return KODE_DENIED;
}

kode_status_t kode_block_udp_send(const char *host, uint16_t port,
                            const void *data, uint32_t len)
{
    (void)host; (void)port; (void)data; (void)len;
    return KODE_DENIED;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_net_lookup(QActive *me, const char *host)
{
    (void)me; (void)host;
    return KODE_DENIED;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_net_ping(QActive *me, const char *host, uint8_t count)
{
    (void)me; (void)host; (void)count;
    return KODE_DENIED;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_http_get(QActive *me, const char *url, void *buf, uint32_t cap)
{
    (void)me; (void)url; (void)buf; (void)cap;
    return KODE_DENIED;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_http_post(QActive *me, const char *url, const void *body, uint32_t len,
                                 const char *ctype, void *buf, uint32_t cap)
{
    (void)me; (void)url; (void)body; (void)len; (void)ctype; (void)buf; (void)cap;
    return KODE_DENIED;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_udp_send(QActive *me, const char *host, uint16_t port, const void *data,
                                uint16_t len)
{
    (void)me; (void)host; (void)port; (void)data; (void)len;
    return KODE_DENIED;
}

#endif /* CONFIG_KODEOS_NET */
