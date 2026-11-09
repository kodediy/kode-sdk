/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_net.c
 * @brief The standard layer's ping, name lookup and UDP send, blocking in the
 *        calling task on kode_sys's one-shots, and the address check every
 *        network call opens with.
 */
#include "kode.h"

#include "kode_entry.h"
#include "kode_net.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"

/** @brief What the error text shows for a host that is NULL. */
#define KODE_NET_NULL_SHOWN "(null)"

_Static_assert((KODE_NET_HOST_LIMIT + 1U) == KODE_SYS_NET_HOST_MAX,
               "a host the standard layer takes is one the facade takes");
_Static_assert(KODE_PING_COUNT_MAX == KODE_SYS_NET_PING_MAX,
               "a ping the standard layer asks is one the facade runs");
_Static_assert(KODE_UDP_DATA_MAX == UINT16_MAX, "a datagram's length fits the driver's");

bool kode_net_ready(void)
{
    kode_sys_wifi_status_t st = { 0U, 0U, "" };
    const kode_status_t    rc = kode_sys_wifi_status(&st);
    bool                   ok = false;

    if ( KODE_DENIED == rc )
    {
        kode_err_set("this firmware has no network");
    }
    else if ( (KODE_OK != rc) || (KODE_SYS_WIFI_STATE_GOT_IP != st.state) )
    {
        kode_err_set("no network: call kode_wifi_connect() first");
    }
    else
    {
        ok = true;
    }
    return ok;
}

/**
 * @brief Whether @p host is a name a call may take; the error says why when not.
 * @param host What the caller typed; may be NULL.
 * @return true for 1 to KODE_NET_HOST_LIMIT bytes.
 */
static bool host_ok(const char *host)
{
    bool ok = false;

    if ( NULL == host )
    {
        kode_err_set("bad host '%s'", KODE_NET_NULL_SHOWN);
    }
    else
    {
        uint32_t n = 0U;

        while ( (KODE_NET_HOST_LIMIT >= n) && ('\0' != host[n]) )
        {
            n++;
        }
        if ( (0U == n) || (KODE_NET_HOST_LIMIT < n) )
        {
            kode_err_set("bad host: 1 to %u bytes", (unsigned)KODE_NET_HOST_LIMIT);
        }
        else
        {
            ok = true;
        }
    }
    return ok;
}

/**
 * @brief Keep why a one-shot failed as the task's error.
 * @param host The host asked.
 * @param rc   The facade's answer.
 */
static void net_fail(const char *host, kode_status_t rc)
{
    switch ( rc )
    {
    case KODE_DENIED:
        kode_err_set("this firmware has no network");
        break;
    case KODE_TIMEOUT:
        kode_err_set("%s: no answer in time", host);
        break;
    default:
        kode_err_set("%s: could not reach it", host);
        break;
    }
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_net_ping_t kode_net_ping(const char *host, uint32_t count)
{
    kode_net_ping_t out = { false, 0U, 0U, 0U, 0U, 0U };

    if ( !kode_enter("kode_net_ping") )
    {
        /* kode_enter() said why */
    }
    else if ( !host_ok(host) )
    {
        /* host_ok() said why */
    }
    else if ( (0U == count) || (KODE_PING_COUNT_MAX < count) )
    {
        kode_err_set("count %u: 1 to %u", (unsigned)count, (unsigned)KODE_PING_COUNT_MAX);
    }
    else if ( kode_net_ready() )
    {
        kode_sys_ping_stats_t st = { 0U, 0U, 0U, 0U, 0U, 0U };
        const kode_status_t   rc = kode_block_net_ping(host, (uint8_t)count, &st);

        if ( KODE_OK == rc )
        {
            out.ok       = true;
            out.sent     = st.tx;
            out.received = st.rx;
            out.min_ms   = st.min_ms;
            out.avg_ms   = st.avg_ms;
            out.max_ms   = st.max_ms;
        }
        else
        {
            net_fail(host, rc);
        }
    }
    else
    {
        /* kode_net_ready() said why */
    }
    return out;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_net_resolve(const char *host, uint32_t *ip4)
{
    bool ok = false;

    if ( !kode_enter("kode_net_resolve") )
    {
        /* kode_enter() said why */
    }
    else if ( !host_ok(host) )
    {
        /* host_ok() said why */
    }
    else if ( NULL == ip4 )
    {
        kode_err_set("no place for the address");
    }
    else if ( kode_net_ready() )
    {
        uint32_t            got = 0U;
        const kode_status_t rc  = kode_block_net_lookup(host, &got);

        if ( KODE_OK == rc )
        {
            *ip4 = got;
            ok   = true;
        }
        else if ( KODE_ERROR == rc )
        {
            kode_err_set("%s does not resolve", host);
        }
        else
        {
            net_fail(host, rc);
        }
    }
    else
    {
        /* kode_net_ready() said why */
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_udp_send(const char *host, uint16_t port, const void *data, uint32_t len)
{
    bool ok = false;

    if ( !kode_enter("kode_udp_send") )
    {
        /* kode_enter() said why */
    }
    else if ( !host_ok(host) )
    {
        /* host_ok() said why */
    }
    else if ( 0U == port )
    {
        kode_err_set("port 0: 1 to %u", (unsigned)UINT16_MAX);
    }
    else if ( ((NULL == data) && (0U < len)) || (KODE_UDP_DATA_MAX < len) )
    {
        kode_err_set("bad datagram: 0 to %u bytes, not NULL", (unsigned)KODE_UDP_DATA_MAX);
    }
    else if ( kode_net_ready() )
    {
        const kode_status_t rc = kode_block_udp_send(host, port, data, len);

        if ( KODE_OK == rc )
        {
            ok = true;
        }
        else
        {
            net_fail(host, rc);
        }
    }
    else
    {
        /* kode_net_ready() said why */
    }
    return ok;
}
