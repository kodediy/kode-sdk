/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_net.c
 * @brief lwIP, ICMP, HTTP(S) and MQTT - the only file that speaks any of them.
 */
#include "kdrv_net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sdkconfig.h"
#include "khal_identity.h"
#include "khal_system.h"
#include "khal_time.h"
#include "kode_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* Nothing here touches internal flash (NVS, a partition, FAT): AO_Net runs it on PSRAM stacks. */
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "ping/ping_sock.h"

#if CONFIG_KODEOS_MQTT
#include "mqtt_client.h"
#include "mqtt5_client.h"

/* The channel's session is MQTT 5 (a persistent session with an expiry), and
 * esp-mqtt compiles that code only with this option: a project that does not
 * read the board's sdkconfig first would link without it and find out on a
 * bench. It finds out here instead. */
#if !defined(CONFIG_MQTT_PROTOCOL_5)
#error "the Kode channel speaks MQTT 5: read sdk/components/kbsp/sdkconfig.board before your own sdkconfig defaults (CONFIG_MQTT_PROTOCOL_5=y)"
#endif
#endif

#if !defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
#error "KODEOS_NET verifies https:// against the Mozilla bundle: enable CONFIG_MBEDTLS_CERTIFICATE_BUNDLE (kodeOS pins it in sdkconfig.defaults)"
#endif

/* mbedTLS allocates in PSRAM. Its default is the internal pool, and an MQTT
 * session that stays up holds about 30 KB of it while a renewal opens two more
 * handshakes: a project that does not read the board's sdkconfig first finds
 * out on a bench, at the pool's floor. It finds out here instead. */
#if defined(CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC)
#error "this project keeps mbedTLS's buffers in internal RAM: read sdk/components/kbsp/sdkconfig.board before your own sdkconfig defaults (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y)"
#endif

#define TAG "kdrv.net"

/** One echo per second, the interval every ping tool on earth uses - and the
 *  reason `net_ping` outlasts the console's 5 s deadline at four echoes. */
#define NET_PING_INTERVAL_MS   1000U
/** How long one echo waits for its reply before being called a loss. */
#define NET_PING_TIMEOUT_MS    1000U
/** Whole-session ceiling, so a session that never ends cannot hold the worker
 *  forever: every echo's interval and timeout, plus generous slack. The slack
 *  is deliberately large because expiring here is not a clean outcome - the
 *  ping task cannot be joined, so the session's context has to be abandoned
 *  (see kdrv_net_ping). A busy LAN can stretch a receive well past its own
 *  socket timeout: esp_ping_receive keeps reading while foreign ICMP traffic
 *  keeps arriving. */
#define NET_PING_SESSION_MS(count_) \
    (((uint32_t)(count_) * (NET_PING_INTERVAL_MS + NET_PING_TIMEOUT_MS)) + 5000U)

/** What one read() off the body pulls. Also the discard buffer for the part
 *  past the caller's cap: the bytes are counted, not kept. */
#define NET_HTTP_READ_CHUNK    256U
/* Generous, because the far end is a server and the near end is a board that
 * would rather wait than restart a 1.5 MB download. */
#define KDRV_NET_STREAM_TIMEOUT_MS  15000U

/* KDRV_NET_STREAM_OPEN_MAX_MS and KDRV_NET_STREAM_READ_MAX_MS, the most one
 * open or one read may spend in total, are in kdrv_net.h: AO_Net's guards on
 * the jobs that run them are built on them. Each I/O gets the smaller of
 * KDRV_NET_STREAM_TIMEOUT_MS and the time left, so the cap is a real budget. */

/** How long the worker's GET may take, headers and body together. Long
 *  enough for a TLS handshake over a tired access point, short enough that a
 *  server which accepted the connection and then went quiet still answers the
 *  console this side of anyone's patience. */
#define NET_HTTP_TIMEOUT_MS  15000U

/** The scheme a PUT's URL must start with: its bearer travels in the clear
 *  on anything else. */
#define NET_PUT_SCHEME        "https://"
/** What a log line names when the URL's host cannot be cut out of it. */
#define NET_HOST_UNKNOWN      "?"
/** The header value's scheme word, space included. */
#define NET_AUTH_SCHEME       "Bearer "
/** ESP-IDF's HTTP client's log tag. At DEBUG it writes a request's headers
 *  whole - the request line with the token in its path, and the bearer - so a
 *  PUT holds it at INFO (kode_log_hold()) for as long as its exchange runs. */
#define NET_HTTP_LOG_TAG      "HTTP_CLIENT"

/** The last job's results. Written by the job, read by AO_Net between the
 *  done event and the next job - a window the AO controls. */
typedef struct {
    uint8_t                 ping_n;
    kdrv_net_ping_reply_t   ping_rows[KDRV_NET_PING_MAX];
    kdrv_net_ping_stats_t   ping_stats;
    kdrv_net_http_result_t  http;
    uint8_t                 body[KDRV_NET_BODY_MAX];
    uint16_t                udp_sent;
} NetResults;

/** The results, in PSRAM: 1.6 KB that a board with no network job never
 *  needs. Taken by the first job that writes them, never freed; a reader
 *  that finds none reads "nothing was kept". */
static NetResults *s_net;

/**
 * @brief The results block, taken on first use.
 * @return The block, zeroed; NULL when PSRAM cannot give it.
 */
static NetResults *net_results(void)
{
    if ( NULL == s_net )
    {
        s_net = (NetResults *)khal_alloc_psram(1U, sizeof(*s_net));
    }
    return s_net;
}

/* ---- Names and addresses ------------------------------------------------ */

kode_status_t kdrv_net_lookup(const char *host, uint32_t *ip4)
{
    if ( (NULL == host) || ('\0' == host[0]) || (NULL == ip4) )
    {
        return KODE_ERROR;
    }

    struct addrinfo hints;
    (void)memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;      /* IPv4 only: see the spec's scope. */
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if ( (getaddrinfo(host, NULL, &hints, &res) != 0) || (NULL == res) )
    {
        KLOG_W(TAG, "'%s' does not resolve", host);
        return KODE_ERROR;
    }

    struct sockaddr_in const * const in = (struct sockaddr_in *)res->ai_addr;
    *ip4 = in->sin_addr.s_addr;
    freeaddrinfo(res);
    return KODE_OK;
}

kode_status_t kdrv_net_info(kdrv_net_info_t *out)
{
    if ( NULL == out )
    {
        return KODE_ERROR;
    }
    (void)memset(out, 0, sizeof(*out));

    /* By key rather than by a handle someone hands us: the station netif is
     * kdrv_wifi's to create (lazily, on the first join), and asking the
     * registry means this file does not have to be told when that happened. */
    esp_netif_t * const sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if ( NULL == sta )
    {
        return KODE_DENIED;   /* no station yet: "not connected" is an answer */
    }

    esp_netif_ip_info_t ip;
    if ( esp_netif_get_ip_info(sta, &ip) != ESP_OK )
    {
        return KODE_ERROR;
    }
    out->ip4   = ip.ip.addr;
    out->gw4   = ip.gw.addr;
    out->mask4 = ip.netmask.addr;

    esp_netif_dns_info_t dns;
    if ( esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK )
    {
        out->dns4 = dns.ip.u_addr.ip4.addr;
    }
    return KODE_OK;
}

/* ---- Ping ---------------------------------------------------------------
 * esp_ping runs its own task and calls back into ours, which makes the
 * lifetime of the context the whole problem here.
 *
 * esp_ping_stop() and esp_ping_delete_session() only clear flags - neither
 * joins the ping task (IDF's ping_sock.c). So a session that outlives our
 * wait still calls on_ping_timeout() and on_ping_end() afterwards, with the
 * cb_args we gave it. If that context were a local of the waiting function -
 * the obvious way to write this, and the way it was written first - those
 * late callbacks would write through a pointer into a stack frame that no
 * longer exists, and give a semaphore that had been deleted.
 *
 * So the context is heap-allocated and, when we give up on a session, it is
 * deliberately *abandoned* rather than freed: rows is cleared so nothing is
 * written through it, and the block plus its semaphore are left to the
 * session that still owns them. That leaks about fifty bytes on a path that
 * should never run, which is the right trade against a use-after-free that
 * would corrupt whichever task's memory happened to be there.
 *
 * Everything the callbacks touch lives in this struct, including the stats,
 * so the caller's own buffer is only ever written on the path where the
 * caller is still there to own it. Two sessions at once each get their own
 * block, so this stays reentrant.
 */

typedef struct {
    kdrv_net_ping_reply_t *rows;    /**< NULL once abandoned.               */
    uint8_t                cap;
    uint8_t                n;       /**< Rows the callbacks have written.   */
    uint32_t               sum_ms;
    kdrv_net_ping_stats_t  stats;   /**< By value: outlives the caller.     */
    SemaphoreHandle_t      done;
} ping_ctx_t;

/** @brief Record one row, if there is room for it. */
static void ping_row(ping_ctx_t *c, uint16_t seq, uint16_t ms, uint8_t ttl,
                     bool ok)
{
    if ( (NULL != c->rows) && (c->n < c->cap) )
    {
        c->rows[c->n].seq     = seq;
        c->rows[c->n].time_ms = ms;
        c->rows[c->n].ttl     = ttl;
        c->rows[c->n].ok      = ok;
    }
    if ( 0xFFU > c->n )
    {
        ++c->n;
    }
}

/** Runs on esp_ping's task: a reply came back. */
static void ping_on_success(esp_ping_handle_t hdl, void *args)
{
    ping_ctx_t * const c = (ping_ctx_t *)args;
    uint16_t seq = 0U;
    uint32_t ms  = 0U;
    uint8_t  ttl = 0U;

    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));

    c->sum_ms += ms;
    if ( (0U == c->stats.min_ms) || ((uint16_t)ms < c->stats.min_ms) )
    {
        c->stats.min_ms = (uint16_t)ms;
    }
    if ( (uint16_t)ms > c->stats.max_ms )
    {
        c->stats.max_ms = (uint16_t)ms;
    }
    ping_row(c, seq, (uint16_t)ms, ttl, true);
}

/** Runs on esp_ping's task: one echo went unanswered. */
static void ping_on_timeout(esp_ping_handle_t hdl, void *args)
{
    ping_ctx_t * const c = (ping_ctx_t *)args;
    uint16_t seq = 0U;

    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    /* Recorded rather than skipped: a session of four with one row missing
     * makes the reader count, and counting is how a loss goes unnoticed. */
    ping_row(c, seq, 0U, 0U, false);
}

/** Runs on esp_ping's task: the session is over. Release the caller. */
static void ping_on_end(esp_ping_handle_t hdl, void *args)
{
    ping_ctx_t * const c = (ping_ctx_t *)args;
    uint32_t tx = 0U;
    uint32_t rx = 0U;

    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &tx, sizeof(tx));
    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &rx, sizeof(rx));
    c->stats.tx = (uint16_t)tx;
    c->stats.rx = (uint16_t)rx;
    c->stats.avg_ms = (0U < rx) ? (uint16_t)(c->sum_ms / rx) : 0U;
    (void)xSemaphoreGive(c->done);
}

kode_status_t kdrv_net_ping(const char *host, uint8_t count,
                            kdrv_net_ping_reply_t *rows, uint8_t cap,
                            uint8_t *rows_written,
                            kdrv_net_ping_stats_t *stats)
{
    if ( (NULL == stats) || (0U == count) )
    {
        return KODE_ERROR;
    }
    (void)memset(stats, 0, sizeof(*stats));
    if ( NULL != rows_written )
    {
        *rows_written = 0U;
    }

    uint32_t ip4 = 0U;
    if ( kdrv_net_lookup(host, &ip4) != KODE_OK )
    {
        return KODE_ERROR;
    }

    ping_ctx_t * const ctx = calloc(1U, sizeof(ping_ctx_t));
    if ( NULL == ctx )
    {
        return KODE_NO_RESOURCES;
    }
    ctx->rows      = rows;
    ctx->cap       = cap;
    ctx->stats.ip4 = ip4;
    ctx->done      = xSemaphoreCreateBinary();
    if ( NULL == ctx->done )
    {
        free(ctx);
        return KODE_NO_RESOURCES;
    }

    ip_addr_t target;
    (void)memset(&target, 0, sizeof(target));
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = ip4;

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count       = count;
    cfg.interval_ms = NET_PING_INTERVAL_MS;
    cfg.timeout_ms  = NET_PING_TIMEOUT_MS;

    esp_ping_callbacks_t cbs = {
        .cb_args         = ctx,
        .on_ping_success = &ping_on_success,
        .on_ping_timeout = &ping_on_timeout,
        .on_ping_end     = &ping_on_end,
    };

    esp_ping_handle_t hdl = NULL;
    kode_status_t     rc  = KODE_OK;
    bool              abandoned = false;

    if ( esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK )
    {
        rc = KODE_ERROR;
    }
    else if ( esp_ping_start(hdl) != ESP_OK )
    {
        rc = KODE_ERROR;
    }
    else if ( xSemaphoreTake(ctx->done,
                            pdMS_TO_TICKS(NET_PING_SESSION_MS(count)))
             != pdTRUE )
    {
        /* The session outlived its own arithmetic, which means the ping task
         * is still in a receive we cannot interrupt. Stopping and deleting
         * only ask it to wind down; they do not wait for it. So the context
         * goes with it: rows first, so nothing is written through a pointer
         * the caller is about to take back. */
        ctx->rows = NULL;
        abandoned = true;
        (void)esp_ping_stop(hdl);
        rc = KODE_TIMEOUT;
        KLOG_E(TAG, "a ping session to %s outstayed its welcome; abandoning "
                    "its context rather than freeing memory it still uses",
               host);
    }
    else
    {
        /* on_ping_end filled the summary, and it did so before giving the
         * semaphore - so this read cannot race it. */
        *stats = ctx->stats;
        if ( NULL != rows_written )
        {
            *rows_written = (ctx->n > cap) ? cap : ctx->n;
        }
    }

    if ( NULL != hdl )
    {
        (void)esp_ping_delete_session(hdl);
    }
    if ( !abandoned )
    {
        vSemaphoreDelete(ctx->done);
        free(ctx);
    }
    return rc;
}

/* ---- A URL as a medium --------------------------------------------------- */

static esp_http_client_handle_t s_stream_h;

/**
 * @brief The timeout the next I/O of a capped call may have.
 *
 * The smaller of the normal per-I/O timeout and what is left of the call's
 * total budget, so the budget is a real one: no single wait can carry the
 * call past it.
 *
 * @param t0     khal_time_ms() when the call began.
 * @param cap_ms The call's total budget.
 * @return Milliseconds for the next I/O; 0 when the budget is spent.
 */
static uint32_t stream_time_left(uint32_t t0, uint32_t cap_ms)
{
    uint32_t const waited = khal_time_ms() - t0;
    uint32_t       left   = 0U;

    if ( waited < cap_ms )
    {
        left = cap_ms - waited;
        if ( KDRV_NET_STREAM_TIMEOUT_MS < left )
        {
            left = KDRV_NET_STREAM_TIMEOUT_MS;
        }
    }
    return left;
}

kode_status_t kdrv_net_stream_open(const char *url, uint32_t *total,
                                   uint16_t *status)
{
    if ( (NULL == url) || ('\0' == url[0]) )
    {
        return KODE_ERROR;
    }
    if ( NULL != s_stream_h )
    {
        KLOG_E(TAG, "a transfer is already open");
        return KODE_DENIED;
    }
    /* What a log line shows of the URL: everything before the query or the
     * fragment, where a token usually rides. */
    int const shown = (int)strcspn(url, "?#");

    /* The open as a whole spends at most KDRV_NET_STREAM_OPEN_MAX_MS: the
     * connection and the headers each get the time that is left, the way the
     * read gives each of its rounds. A server that accepts the connection and
     * then says nothing used to hold a full per-I/O timeout for each step. */
    uint32_t const t0 = khal_time_ms();

    esp_http_client_config_t cfg;
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.url                   = url;
    cfg.method                = HTTP_METHOD_GET;
    cfg.timeout_ms            = (int)stream_time_left(t0,
                                    KDRV_NET_STREAM_OPEN_MAX_MS);
    /* The same bundle http_get attaches, so https:// behaves identically to
     * the run net.md recorded rather than merely similarly. */
    cfg.crt_bundle_attach     = esp_crt_bundle_attach;

    esp_http_client_handle_t const h = esp_http_client_init(&cfg);
    if ( NULL == h )
    {
        return KODE_ERROR;
    }
    if ( esp_http_client_open(h, 0) != ESP_OK )
    {
        KLOG_W(TAG, "GET %.*s: could not open", shown, url);
        esp_http_client_cleanup(h);
        return KODE_ERROR;
    }

    uint32_t const left = stream_time_left(t0, KDRV_NET_STREAM_OPEN_MAX_MS);
    if ( 0U == left )
    {
        KLOG_W(TAG, "GET %.*s: the open outlasted its %lu ms", shown, url,
               (unsigned long)KDRV_NET_STREAM_OPEN_MAX_MS);
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
        return KODE_TIMEOUT;
    }
    (void)esp_http_client_set_timeout_ms(h, (int)left);

    int64_t  const cl = esp_http_client_fetch_headers(h);
    uint16_t const st = (uint16_t)esp_http_client_get_status_code(h);
    if ( NULL != status )
    {
        *status = st;
    }
    if ( 200U != st )
    {
        KLOG_E(TAG, "GET %.*s: HTTP %u", shown, url, (unsigned)st);
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
        return KODE_ERROR;
    }
    if ( NULL != total )
    {
        *total = (0 < cl) ? (uint32_t)cl : 0U;
    }
    /* The body is read with the normal timeout (each read sets its own). */
    (void)esp_http_client_set_timeout_ms(h, (int)KDRV_NET_STREAM_TIMEOUT_MS);
    s_stream_h = h;
    KLOG_N(TAG, "GET %.*s: %ld bytes", shown, url, (long)cl);
    return KODE_OK;
}

/**
 * @brief The rounds of kdrv_net_stream_read(), each given only the time left.
 *
 * Leaves the client's timeout at whatever the last round needed; the caller
 * puts the configured one back.
 *
 * @param buf Where the bytes go.
 * @param max Room in @p buf.
 * @param got Out: bytes read; 0 with KODE_OK is the real end of the body.
 * @return KODE_OK, or KODE_TIMEOUT when the body stopped or the budget ran out.
 */
static kode_status_t stream_read_rounds(void *buf, size_t max, size_t *got)
{
    /* A read of 0 is NOT end-of-body. esp_http_client_read() returns 0
     * whenever the socket has nothing ready, and with a flash write between
     * one chunk and the next it very often does. kdrv_net_http_get() never
     * notices because it reads in a tight loop with nothing in between; this
     * one paid for the difference by reporting a 232 KB download as finished
     * after zero bytes. The library's own completion check is the only
     * honest end-of-body test.
     *
     * So: keep asking until there are bytes, until the client says the body
     * is complete, or until the budget runs out. The call as a whole spends
     * at most KDRV_NET_STREAM_READ_MAX_MS, which AO_Net's guard on the read
     * job exceeds, however many quick empty rounds it takes: before each
     * round the client is given the smaller of its normal timeout and what is
     * left. */
    uint32_t const t0 = khal_time_ms();
    for ( ;; )
    {
        uint32_t const waited = khal_time_ms() - t0;
        if ( KDRV_NET_STREAM_READ_MAX_MS <= waited )
        {
            KLOG_W(TAG, "the body stalled for %lu ms", (unsigned long)waited);
            return KODE_TIMEOUT;
        }
        uint32_t round_ms = KDRV_NET_STREAM_READ_MAX_MS - waited;
        if ( KDRV_NET_STREAM_TIMEOUT_MS < round_ms )
        {
            round_ms = KDRV_NET_STREAM_TIMEOUT_MS;
        }
        (void)esp_http_client_set_timeout_ms(s_stream_h, (int)round_ms);

        int const n = esp_http_client_read(s_stream_h, (char *)buf, (int)max);
        if ( 0 > n )
        {
            /* Not the end of the body - the end of our patience with it.
             * Calling this EOF is how a download cut short by the network
             * gets reported as a complete one, and then written to a slot. */
            KLOG_W(TAG, "the body stopped arriving");
            return KODE_TIMEOUT;
        }
        if ( 0 < n )
        {
            *got = (size_t)n;
            return KODE_OK;
        }
        if ( esp_http_client_is_complete_data_received(s_stream_h) )
        {
            return KODE_OK;      /* *got stays 0: this is the real EOF */
        }
        /* Nothing ready yet: the server is slow rather than gone. Go round
         * again; the budget at the top of the loop is what gives up. */
    }
}

kode_status_t kdrv_net_stream_read(void *buf, size_t max, size_t *got)
{
    if ( (NULL == s_stream_h) || (NULL == buf) || (NULL == got) )
    {
        return KODE_DENIED;
    }
    *got = 0U;

    kode_status_t const st = stream_read_rounds(buf, max, got);
    /* Whatever the outcome, the next call and the rest of the stream start
     * from the normal timeout. */
    (void)esp_http_client_set_timeout_ms(s_stream_h,
                                         (int)KDRV_NET_STREAM_TIMEOUT_MS);
    return st;
}

void kdrv_net_stream_close(void)
{
    if ( NULL != s_stream_h )
    {
        (void)esp_http_client_close(s_stream_h);
        (void)esp_http_client_cleanup(s_stream_h);
        s_stream_h = NULL;
    }
}

/* ---- HTTP --------------------------------------------------------------- */

/** The PUT's buffers and the connection a kept PUT leaves open. One block in
 *  PSRAM, taken by the first kdrv_net_http_put_prepare() and never freed: the
 *  body alone is 64 KB, which internal RAM does not have to give. The setter
 *  writes it, the worker reads it, and the setter refuses while the worker is
 *  busy. */
typedef struct {
    uint8_t body[KDRV_NET_PUT_MAX];
    size_t  len;
    char    url[KDRV_NET_PUT_URL_MAX];
    char    kept_url[KDRV_NET_PUT_URL_MAX];   /**< What the kept handle is open
                                               *   for; "" while none is.      */
    char    type[KDRV_NET_PUT_TYPE_MAX];
    char    auth[KDRV_NET_AUTH_MAX];          /**< "Bearer <jwt>", wiped by the
                                               *   job that used it.           */
} net_put_t;

static net_put_t *s_put;
/** The connection the last kept PUT left open, or NULL. */
static esp_http_client_handle_t s_kept;

kode_status_t kdrv_net_url_host(const char *url, char *out, size_t cap)
{
    if ( (NULL == out) || (0U == cap) )
    {
        return KODE_ERROR;
    }
    out[0] = '\0';
    if ( NULL == url )
    {
        return KODE_ERROR;
    }
    const char *const sep = strstr(url, "://");
    if ( NULL == sep )
    {
        return KODE_ERROR;
    }
    const char *const host = &sep[3];
    size_t n = 0U;
    while ( ('\0' != host[n]) && ('/' != host[n]) && ('?' != host[n])
           && ('#' != host[n]) )
    {
        ++n;
    }
    if ( (0U == n) || (n >= cap) )
    {
        return KODE_ERROR;
    }
    (void)memcpy(out, host, n);
    out[n] = '\0';
    return KODE_OK;
}

/** @brief Let go of the connection a kept PUT left open. Safe when none is. */
static void put_drop_kept(void)
{
    if ( NULL != s_kept )
    {
        (void)esp_http_client_close(s_kept);
        (void)esp_http_client_cleanup(s_kept);
        s_kept = NULL;
    }
    if ( NULL != s_put )
    {
        (void)memset(s_put->kept_url, 0, sizeof(s_put->kept_url));
    }
}

/**
 * @brief The kept connection when it is open for @p url, else none.
 *
 * Another URL means the kept one is of no use to this request, and it is let
 * go of before a new one is made: one TLS session at a time is what the
 * internal pool can afford.
 *
 * @param url Where the PUT is going.
 * @return The handle to reuse, taken out of the keeper's hands; or NULL.
 */
static esp_http_client_handle_t put_take_kept(const char *url)
{
    if ( (NULL == s_kept) || (NULL == s_put) )
    {
        return NULL;
    }
    if ( strcmp(s_put->kept_url, url) != 0 )
    {
        put_drop_kept();
        return NULL;
    }
    esp_http_client_handle_t const h = s_kept;
    s_kept = NULL;
    (void)memset(s_put->kept_url, 0, sizeof(s_put->kept_url));
    return h;
}

/**
 * @brief One request, one response: what GET, POST and PUT have in common.
 *
 * @param url      Target.
 * @param method   HTTP_METHOD_GET, _POST or _PUT.
 * @param body     POST and PUT: what to send. GET: NULL.
 * @param body_len Bytes of @p body.
 * @param ctype    Content-Type to send; NULL sends none.
 * @param auth     Authorization value to send; NULL sends none.
 * @param keep     PUT only: leave the connection open when the exchange
 *                 succeeded, for the next PUT to the same URL to reuse.
 * @param buf      Where the response body goes; NULL keeps nothing.
 * @param cap      Room in @p buf.
 * @param out      Out: status and lengths.
 * @param timeout_ms Whole-exchange network timeout.
 * @return KODE_OK once the response was read to its end, KODE_ERROR when the
 *         request could not be sent, KODE_TIMEOUT when the body stopped.
 */
static kode_status_t http_exchange(const char *url,
                                   esp_http_client_method_t method,
                                   const char *body, size_t body_len,
                                   const char *ctype, const char *auth,
                                   bool keep,
                                   uint8_t *buf, uint32_t cap,
                                   kdrv_net_http_result_t *out,
                                   uint32_t timeout_ms)
{
    (void)memset(out, 0, sizeof(*out));
    out->content_length = -1;

    /* What the log may say about the target. A PUT's URL carries a token in
     * its path, so its lines name the host and nothing after it. */
    char        host[KDRV_NET_HOST_MAX];
    const char *shown = url;
    if ( HTTP_METHOD_PUT == method )
    {
        shown = host;
        if ( kdrv_net_url_host(url, host, sizeof(host)) != KODE_OK )
        {
            (void)snprintf(host, sizeof(host), "%s", NET_HOST_UNKNOWN);
        }
    }

    esp_http_client_handle_t h = NULL;
    if ( HTTP_METHOD_PUT == method )
    {
        h = put_take_kept(url);
    }
    if ( NULL == h )
    {
        esp_http_client_config_t cfg;
        (void)memset(&cfg, 0, sizeof(cfg));
        cfg.url                   = url;
        cfg.method                = method;
        cfg.timeout_ms            = (int)timeout_ms;
        if ( HTTP_METHOD_PUT == method )
        {
            /* A redirect would carry the bearer to wherever it points, and
             * max_redirection_count = 0 does not mean none: esp_http_client
             * reads it as its default of ten. */
            cfg.disable_auto_redirect = true;
        }
        /* The Mozilla bundle, so https:// against a public host works with no
         * certificate to ship. Harmless on plain http, which never asks for
         * it. */
        cfg.crt_bundle_attach     = esp_crt_bundle_attach;

        h = esp_http_client_init(&cfg);
        if ( NULL == h )
        {
            return KODE_ERROR;
        }
    }

    kode_status_t rc = KODE_OK;
    if ( NULL != ctype )
    {
        (void)esp_http_client_set_header(h, "Content-Type", ctype);
    }
    if ( NULL != auth )
    {
        /* Every request, kept handle or not: the token may have been renewed
         * since the last one. */
        (void)esp_http_client_set_header(h, "Authorization", auth);
    }
    esp_err_t err = esp_http_client_open(h, (int)body_len);
    if ( (ESP_OK == err) && (NULL != body) )
    {
        /* open() sent the headers with the length; the body is ours to write,
         * and a short write is a request the server will never answer. */
        if ( esp_http_client_write(h, body, (int)body_len)
            != (int)body_len )
        {
            err = ESP_FAIL;
        }
    }
    const char *verb = "GET";
    if ( HTTP_METHOD_POST == method )
    {
        verb = "POST";
    }
    if ( HTTP_METHOD_PUT == method )
    {
        verb = "PUT";
    }
    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "%s %s: %s", verb, shown, esp_err_to_name(err));
        rc = KODE_ERROR;
    }
    else
    {
        int64_t const cl = esp_http_client_fetch_headers(h);
        if ( 0 > cl )
        {
            /* No answer came: the server ended the connection before a
             * status line, or a kept connection it had closed meanwhile took
             * the request in its socket buffer and fails here. Reading on
             * would report a status nobody sent, or the old reply's end as
             * this one's. */
            KLOG_W(TAG, "%s %s: no answer", verb, shown);
            rc = KODE_ERROR;
        }
        else
        {
            out->status = (uint16_t)esp_http_client_get_status_code(h);
        }
        /* A chunked response has no declared length and fetch_headers reports
         * that as 0, which is also what an empty body reports. Asking the
         * client which one it is keeps -1 meaning "undeclared" rather than
         * quietly turning every chunked page into content_len=0. */
        if ( (0 < cl) || ((0 == cl) && !esp_http_client_is_chunked_response(h)) )
        {
            out->content_length = (int32_t)cl;
        }

        /* Read the whole body even past the caller's room for it: the bytes
         * beyond cap are counted and dropped, so body_len is the truth and
         * `truncated` is not a guess. */
        uint8_t scratch[NET_HTTP_READ_CHUNK];
        while ( KODE_OK == rc )
        {
            int const n = esp_http_client_read(h, (char *)scratch,
                                               (int)sizeof(scratch));
            if ( 0 > n )
            {
                /* Not the end of the body - the end of our patience with it.
                 * Treating this as EOF is how a download cut short by the
                 * network gets reported as a complete one. */
                KLOG_W(TAG, "%s: the body stopped arriving", shown);
                rc = KODE_TIMEOUT;
                break;
            }
            if ( 0 == n )
            {
                /* Not end-of-body on its own. The stream reader beside this
                 * one carries the same lesson at length: treating a
                 * zero-length read as the end reported a 232 KB download as
                 * finished after zero bytes, and the library's own completion
                 * check is the only honest test. This loop was excused on the
                 * grounds that it is tight enough never to see one, which is
                 * a statement about timing rather than about correctness. */
                if ( esp_http_client_is_complete_data_received(h) )
                {
                    break;
                }
                continue;
            }
            uint32_t const got = (uint32_t)n;
            if ( (NULL != buf) && (out->kept < cap) )
            {
                uint32_t room = cap - out->kept;
                if ( room > got )
                {
                    room = got;
                }
                (void)memcpy(&buf[out->kept], scratch, room);
                out->kept += room;
            }
            out->body_len += got;
        }
        out->truncated = (out->body_len > out->kept);
        if ( keep && (KODE_OK == rc) && (HTTP_METHOD_PUT == method)
            && (NULL != s_put) && esp_http_client_is_persistent_connection(h) )
        {
            /* Answered to its end, and the server said the connection stays
             * open (a protocol fact, not a policy): the next PUT to this URL
             * picks it up where it is. Anything else is closed here. */
            (void)snprintf(s_put->kept_url, sizeof(s_put->kept_url), "%s", url);
            s_kept = h;
            return rc;
        }
        (void)esp_http_client_close(h);
    }

    (void)esp_http_client_cleanup(h);
    return rc;
}

kode_status_t kdrv_net_http_get(const char *url, uint8_t *buf, uint32_t cap,
                                kdrv_net_http_result_t *out,
                                uint32_t timeout_ms)
{
    if ( (NULL == url) || ('\0' == url[0]) || (NULL == out) )
    {
        return KODE_ERROR;
    }
    return http_exchange(url, HTTP_METHOD_GET, NULL, 0U, NULL, NULL, false,
                         buf, cap, out, timeout_ms);
}

kode_status_t kdrv_net_http_post(const char *url, const char *body,
                                 uint32_t len, const char *ctype,
                                 uint8_t *buf, uint32_t cap,
                                 kdrv_net_http_result_t *out,
                                 uint32_t timeout_ms)
{
    if ( (NULL == url) || ('\0' == url[0]) || (NULL == out) )
    {
        return KODE_ERROR;
    }
    if ( (NULL == body) && (0U < len) )
    {
        return KODE_ERROR;
    }
    if ( 0U == len )
    {
        body = NULL;   /* nothing to write after the headers */
    }
    return http_exchange(url, HTTP_METHOD_POST, body, (size_t)len, ctype,
                         NULL, false, buf, cap, out, timeout_ms);
}

/** The next POST's body. Allocated on first use; a job reads it, the setter
 *  writes it, and AO_Net calls the setter only while its worker is idle. */
static struct {
    char   *body;
    size_t  len;
} s_post;

kode_status_t kdrv_net_http_post_body(const char *json, size_t len)
{
    if ( (NULL == json) || (0U == len) || (KDRV_NET_POST_MAX < len) )
    {
        return KODE_ERROR;
    }
    if ( NULL == s_post.body )
    {
        s_post.body = (char *)khal_alloc_psram(1U, KDRV_NET_POST_MAX);
        if ( NULL == s_post.body )
        {
            return KODE_NO_RESOURCES;
        }
    }
    (void)memcpy(s_post.body, json, len);
    s_post.len = len;
    return KODE_OK;
}

kode_status_t kdrv_net_http_put_prepare(const char *url, const uint8_t *body,
                                        size_t len, const char *content_type,
                                        const char *bearer)
{
    if ( (NULL == url) || (NULL == body) || (NULL == content_type)
        || (NULL == bearer) || (0U == len) || (KDRV_NET_PUT_MAX < len) )
    {
        return KODE_ERROR;
    }
    size_t const url_len    = strlen(url);
    size_t const type_len   = strlen(content_type);
    size_t const bearer_len = strlen(bearer);
    size_t const scheme_len = strlen(NET_PUT_SCHEME);
    if ( (KDRV_NET_PUT_URL_MAX <= url_len)
        || (strncmp(url, NET_PUT_SCHEME, scheme_len) != 0)
        || (0U == type_len) || (KDRV_NET_PUT_TYPE_MAX <= type_len)
        || (0U == bearer_len)
        || ((strlen(NET_AUTH_SCHEME) + bearer_len) >= KDRV_NET_AUTH_MAX) )
    {
        return KODE_ERROR;
    }
    if ( NULL == s_put )
    {
        s_put = (net_put_t *)khal_alloc_psram(1U, sizeof(*s_put));
        if ( NULL == s_put )
        {
            return KODE_NO_RESOURCES;
        }
        (void)memset(s_put, 0, sizeof(*s_put));
    }
    (void)memcpy(s_put->body, body, len);
    s_put->len = len;
    (void)memcpy(s_put->url, url, url_len + 1U);
    (void)memcpy(s_put->type, content_type, type_len + 1U);
    (void)snprintf(s_put->auth, sizeof(s_put->auth), "%s%s", NET_AUTH_SCHEME,
                   bearer);
    return KODE_OK;
}

/* ---- UDP ---------------------------------------------------------------- */

kode_status_t kdrv_net_udp_send(const char *host, uint16_t port,
                                const void *data, uint16_t len)
{
    if ( (NULL == host) || (NULL == data) || (0U == len) || (0U == port) )
    {
        return KODE_ERROR;
    }

    char portstr[6];
    (void)snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);

    struct addrinfo hints;
    (void)memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo *res = NULL;
    if ( (getaddrinfo(host, portstr, &hints, &res) != 0) || (NULL == res) )
    {
        KLOG_W(TAG, "'%s' does not resolve", host);
        return KODE_ERROR;
    }

    kode_status_t rc = KODE_OK;
    int const     s  = socket(res->ai_family, res->ai_socktype, 0);
    if ( 0 > s )
    {
        rc = KODE_NO_RESOURCES;
    }
    else
    {
        int const sent = sendto(s, data, len, 0, res->ai_addr,
                                (socklen_t)res->ai_addrlen);
        if ( (int)len != sent )
        {
            KLOG_W(TAG, "datagram to %s:%u went nowhere (errno %d)",
                   host, (unsigned)port, errno);
            rc = KODE_ERROR;
        }
        (void)close(s);
    }
    freeaddrinfo(res);
    return rc;
}

/* ---- MQTT ---------------------------------------------------------------- */

#if CONFIG_KODEOS_MQTT

/** @brief The error cause of a transport failure; a refusal is its return code. */
#define MQTT_ERR_KEY_TRANSPORT  (-1)

/** @brief The highest QoS a subscribe asks for. */
#define KDRV_MQTT_SUB_QOS_MAX   2U

static struct {
    esp_mqtt_client_handle_t client;
    kdrv_net_mqtt_cb_t       cb;
    volatile kdrv_mqtt_state_t state;
    char                     uri[KDRV_MQTT_URI_MAX];
    kdrv_mqtt_session_t     *session;      /**< PSRAM, taken on first use.   */
    bool                     session_set;  /**< Whether START uses it.       */
    int                      sub_qos;      /**< The QoS of a subscribe: the
                                            *   live client's session set it,
                                            *   0 for a bench client.        */
    bool                     channel;      /**< The live client was started with a
                                            *   session: its messages are tagged
                                            *   as the channel's.             */
    bool                     redials;      /**< esp-mqtt reconnects by itself;
                                            *   false when the owner of the
                                            *   session paces the retries.   */
    int                      err_key;      /**< The cause last logged as an
                                            *   error; 0 when none since the
                                            *   last CONNECTED.              */
    kdrv_mqtt_msg_t         *rx;           /**< The receive ring, PSRAM,
                                            *   KDRV_MQTT_RX_RING slots;
                                            *   taken by the first START.   */
    uint8_t                  rx_head;      /**< Oldest message (s_rx_lock).  */
    uint8_t                  rx_len;       /**< Messages held (s_rx_lock).   */
    bool                     rx_owed;      /**< A notice is owed (one was refused,
                                            *   or the owner asked for the next
                                            *   message to send one): the next
                                            *   push sends it (s_rx_lock).   */
    uint32_t                 rx_dropped;   /**< Messages dropped because the
                                            *   ring was full, since boot.   */
} s_mqtt;

/**
 * @brief The most unacknowledged QoS 1 and 2 messages the broker may have in
 *        flight to the channel's client (MQTT 5 `receive_maximum`).
 *
 * esp-mqtt acknowledges a message as the data callback returns, and the
 * callback never waits (it copies into the ring and returns), so the window
 * reopens at once and this cannot hold the broker back from a slow consumer;
 * it bounds what is on the wire unacknowledged, and nothing more.
 */
#define KDRV_MQTT_RECEIVE_MAX  4U

/** @brief The drop of a message is logged when the count's low bits equal 1: the first, then every 64th. */
#define KDRV_MQTT_DROP_LOG_MASK  0x3FU

/* The ring's indices are bytes. */
_Static_assert(KDRV_MQTT_RX_RING <= UINT8_MAX,
               "the receive ring's head and length are bytes");

/** @brief Guards the receive ring and nothing else: held for a copy of one message, never across a publish
 *         or a post. Two tasks meet here: esp-mqtt's, which pushes, and AO_Net's, which takes. */
static portMUX_TYPE s_rx_lock = portMUX_INITIALIZER_UNLOCKED;

void kdrv_net_mqtt_set_cb(kdrv_net_mqtt_cb_t cb)
{
    s_mqtt.cb = cb;
}

bool kdrv_net_mqtt_rx_take(kdrv_mqtt_msg_t *out)
{
    bool taken = false;

    portENTER_CRITICAL(&s_rx_lock);
    if ( (NULL != s_mqtt.rx) && (0U != s_mqtt.rx_len) )
    {
        *out = s_mqtt.rx[s_mqtt.rx_head];
        s_mqtt.rx_head = (uint8_t)(((uint32_t)s_mqtt.rx_head + 1U) % (uint32_t)KDRV_MQTT_RX_RING);
        s_mqtt.rx_len--;
        taken = true;
    }
    portEXIT_CRITICAL(&s_rx_lock);
    return taken;
}

void kdrv_net_mqtt_rx_owe(void)
{
    portENTER_CRITICAL(&s_rx_lock);
    s_mqtt.rx_owed = true;
    portEXIT_CRITICAL(&s_rx_lock);
}

uint32_t kdrv_net_mqtt_rx_dropped(void)
{
    return s_mqtt.rx_dropped;
}

kdrv_mqtt_state_t kdrv_net_mqtt_state(void)
{
    return s_mqtt.state;
}

/** @brief Guards the broker URI: written by the net worker, read from any core. */
/* cppcheck-suppress misra-c2012-7.2 ; IDF's initialiser macro, no suffix to give it */
static portMUX_TYPE s_mqtt_uri_mux = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief Replace the broker URI, whole, under the lock.
 * @param uri The new URI, cut to the field.
 */
static void mqtt_uri_set(const char *uri)
{
    char fresh[KDRV_MQTT_URI_MAX];

    (void)memset(fresh, 0, sizeof(fresh));
    (void)snprintf(fresh, sizeof(fresh), "%s", uri);
    taskENTER_CRITICAL(&s_mqtt_uri_mux);
    (void)memcpy(s_mqtt.uri, fresh, sizeof(s_mqtt.uri));
    taskEXIT_CRITICAL(&s_mqtt_uri_mux);
}

void kdrv_net_mqtt_uri_copy(char *out, size_t cap)
{
    if ( (NULL == out) || (0U == cap) )
    {
        return;
    }
    size_t n = 0U;

    taskENTER_CRITICAL(&s_mqtt_uri_mux);
    while ( ((cap - 1U) > n) && (sizeof(s_mqtt.uri) > n) && ('\0' != s_mqtt.uri[n]) )
    {
        out[n] = s_mqtt.uri[n];
        ++n;
    }
    taskEXIT_CRITICAL(&s_mqtt_uri_mux);
    out[n] = '\0';
}

/** @brief Forget the session and wipe the token: a session serves one START. */
static void mqtt_session_spent(void)
{
    if ( NULL != s_mqtt.session )
    {
        (void)memset(s_mqtt.session, 0, sizeof(*s_mqtt.session));
    }
    s_mqtt.session_set = false;
}

kode_status_t kdrv_net_mqtt_session(const kdrv_mqtt_session_t *s)
{
    if ( NULL == s )
    {
        mqtt_session_spent();   /* the token does not linger in a free buffer */
        return KODE_OK;
    }
    if ( '\0' == s->client_id[0] )
    {
        return KODE_ERROR;    /* esp-mqtt would invent one: not what was asked */
    }
    if ( NULL == s_mqtt.session )
    {
        s_mqtt.session = (kdrv_mqtt_session_t *)
            khal_alloc_psram(sizeof(void *), sizeof(*s_mqtt.session));
        if ( NULL == s_mqtt.session )
        {
            return KODE_NO_RESOURCES;
        }
    }
    *s_mqtt.session = *s;
    /* A caller's unterminated array must not become esp-mqtt's overrun. */
    s_mqtt.session->client_id[sizeof(s_mqtt.session->client_id) - 1U]   = '\0';
    s_mqtt.session->user[sizeof(s_mqtt.session->user) - 1U]             = '\0';
    s_mqtt.session->pass[sizeof(s_mqtt.session->pass) - 1U]             = '\0';
    s_mqtt.session->will_topic[sizeof(s_mqtt.session->will_topic) - 1U] = '\0';
    s_mqtt.session->will_msg[sizeof(s_mqtt.session->will_msg) - 1U]     = '\0';
    s_mqtt.session_set = true;
    return KODE_OK;
}

/** @brief Tell AO_Net, if it is listening. */
static void mqtt_report(kdrv_net_mqtt_evt_t evt, const kdrv_mqtt_msg_t *msg,
                        bool session_present)
{
    if ( NULL != s_mqtt.cb )
    {
        (void)s_mqtt.cb(evt, msg, session_present);
    }
}

/**
 * @brief Keep one arriving message for the owner and tell it, without waiting.
 *
 * Runs on esp-mqtt's task, inside its API lock, where the owner may itself be
 * blocked in a publish: nothing here may wait for the owner, so the message is
 * copied into the ring and the callback returns at once, which is also what
 * sends the PUBACK. The owner is told by an event (the callback is a post)
 * when the ring goes from empty to holding something, or when a notice is
 * owed: one it refused, or one it asked for with kdrv_net_mqtt_rx_owe(). It
 * empties the ring with kdrv_net_mqtt_rx_take() and does not need one notice
 * a message. A full ring drops the message, with a WARN and a count
 * (kdrv_net_mqtt_rx_dropped()); a notice that is owed still goes then. The
 * ring is shared by the channel's client and the bench client, which are never
 * alive together (AO_Net answers a bench request busy while the channel holds
 * the client), so there is one stream and the owner tells it apart by topic.
 */
static void mqtt_rx_push(const kdrv_mqtt_msg_t *msg)
{
    bool stored = false;
    bool notify = false;

    portENTER_CRITICAL(&s_rx_lock);
    if ( (NULL != s_mqtt.rx) && ((uint8_t)KDRV_MQTT_RX_RING > s_mqtt.rx_len) )
    {
        uint8_t const at = (uint8_t)(((uint32_t)s_mqtt.rx_head + (uint32_t)s_mqtt.rx_len)
                                     % (uint32_t)KDRV_MQTT_RX_RING);

        s_mqtt.rx[at] = *msg;
        notify        = (0U == s_mqtt.rx_len);
        s_mqtt.rx_len++;
        stored        = true;
    }
    else
    {
        s_mqtt.rx_dropped++;
    }
    if ( s_mqtt.rx_owed )
    {
        notify         = true;
        s_mqtt.rx_owed = false;
    }
    portEXIT_CRITICAL(&s_rx_lock);

    if ( !stored )
    {
        /* Once, then every 64th: a flood must not become a log flood. */
        if ( (s_mqtt.rx_dropped & KDRV_MQTT_DROP_LOG_MASK) == 1U )
        {
            KLOG_W(TAG, "mqtt: the receive ring is full; a message was dropped (%lu so far)",
                   (unsigned long)s_mqtt.rx_dropped);
        }
    }
    if ( notify )
    {
        bool posted = false;

        if ( NULL != s_mqtt.cb )
        {
            posted = s_mqtt.cb(KDRV_NET_MQTT_MSG, NULL, false);
        }
        if ( !posted )
        {
            kdrv_net_mqtt_rx_owe();
        }
    }
}

/**
 * @brief Copy one arriving message into a bounded struct.
 *
 * esp-mqtt splits a payload larger than its buffer across several DATA
 * events; only the first piece is carried and @c data_cut says so. The
 * alternative - reassembling into a growing buffer - would put an unbounded
 * allocation on the path a stranger controls.
 */
static void mqtt_copy_msg(const esp_mqtt_event_t *ev, kdrv_mqtt_msg_t *msg)
{
    (void)memset(msg, 0, sizeof(*msg));

    uint32_t tlen = (0 < ev->topic_len) ? (uint32_t)ev->topic_len : 0U;
    if ( sizeof(msg->topic) <= tlen )
    {
        tlen           = sizeof(msg->topic) - 1U;
        msg->topic_cut = true;
    }
    if ( (NULL != ev->topic) && (0U < tlen) )
    {
        (void)memcpy(msg->topic, ev->topic, tlen);
    }
    msg->topic[tlen] = '\0';

    uint32_t dlen = (0 < ev->data_len) ? (uint32_t)ev->data_len : 0U;
    if ( sizeof(msg->data) < dlen )
    {
        dlen = sizeof(msg->data);
    }
    if ( (NULL != ev->data) && (0U < dlen) )
    {
        (void)memcpy(msg->data, ev->data, dlen);
    }
    msg->len      = (uint16_t)dlen;
    msg->data_cut = ((uint32_t)ev->total_data_len > dlen);
    msg->retained = (0 != ev->retain);
}

/** Runs on esp-mqtt's own task: record, report, return. */
static void mqtt_event(void *args, esp_event_base_t base, int32_t id,
                       void *data)
{
    (void)args; (void)base;
    esp_mqtt_event_t const * const ev = (esp_mqtt_event_t *)data;

    switch ( (esp_mqtt_event_id_t)id )
    {
        case MQTT_EVENT_CONNECTED:
            s_mqtt.state   = KDRV_MQTT_UP;
            s_mqtt.err_key = 0;
            KLOG_N(TAG, "mqtt connected to %s", s_mqtt.uri);
            mqtt_report(KDRV_NET_MQTT_CONNECTED, NULL, 0 != ev->session_present);
            break;
        case MQTT_EVENT_DISCONNECTED:
            /* Saying it out loud is what keeps `mqtt_info` from claiming a
             * broker that is not there. A client that redials by itself is
             * still connecting; one whose owner paces the retries is down
             * until the owner starts another. */
            if ( s_mqtt.redials )
            {
                s_mqtt.state = KDRV_MQTT_CONNECTING;
                KLOG_W(TAG, "mqtt disconnected (retrying)");
            }
            else
            {
                s_mqtt.state = KDRV_MQTT_DOWN;
                KLOG_W(TAG, "mqtt disconnected");
            }
            mqtt_report(KDRV_NET_MQTT_DISCONNECTED, NULL, false);
            break;
        case MQTT_EVENT_ERROR: {
            /* A refusal (bad or expired token) carries the broker's own
             * code; saying it is what tells "not authorized" from a TLS or
             * socket failure when the channel goes to waiting. */
            int refused = 0;
            int key     = MQTT_ERR_KEY_TRANSPORT;
            if ( (NULL != ev->error_handle) &&
                (ev->error_handle->error_type ==
                 MQTT_ERROR_TYPE_CONNECTION_REFUSED) )
            {
                refused = (int)ev->error_handle->connect_return_code;
                key     = refused;
            }
            /* Once per cause: a client that redials every few seconds
             * against a dead broker would otherwise write the same line for
             * as long as the outage lasts. The next CONNECTED, or a new
             * client, starts the count again. */
            if ( key != s_mqtt.err_key )
            {
                s_mqtt.err_key = key;
                KLOG_E(TAG, "mqtt error from %s (refused code %d)", s_mqtt.uri,
                       refused);
            }
            if ( 0 != refused )
            {
                mqtt_report(KDRV_NET_MQTT_REFUSED, NULL, false);
            }
            else
            {
                mqtt_report(KDRV_NET_MQTT_ERROR, NULL, false);
            }
            break;
        }
        case MQTT_EVENT_DATA: {
            /* One message may arrive as several events; the later pieces
             * carry an offset and are dropped rather than reported as new
             * messages of their own. */
            if ( 0 == ev->current_data_offset )
            {
                kdrv_mqtt_msg_t msg;
                mqtt_copy_msg(ev, &msg);
                msg.channel = s_mqtt.channel;
                mqtt_rx_push(&msg);
            }
            break;
        }
        default:
            break;
    }
}

/** The MQTT_START job: create the client and let it dial. */
static kode_status_t mqtt_do_start(const kdrv_net_req_t *req)
{
    if ( NULL != s_mqtt.client )
    {
        mqtt_session_spent();
        return KODE_DENIED;   /* one client; AO_Net stops it before restarting */
    }

    if ( NULL == s_mqtt.rx )
    {
        /* PSRAM, rule 8, and only once: the ring outlives every client, so
         * what one received and AO_Net has not taken is still there. */
        /* cppcheck-suppress misra-c2012-11.5 ; PSRAM storage aligned for any object, and sized for this one */
        s_mqtt.rx = (kdrv_mqtt_msg_t *)khal_alloc_psram(sizeof(void *),
                                                        sizeof(kdrv_mqtt_msg_t) * KDRV_MQTT_RX_RING);
        if ( NULL == s_mqtt.rx )
        {
            mqtt_session_spent();
            return KODE_NO_RESOURCES;
        }
    }

    esp_mqtt_client_config_t cfg;
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.broker.address.uri = req->uri;

    bool     want_mqtt5 = false;
    uint32_t expiry_s   = 0U;

    kdrv_mqtt_session_t const * const sess = s_mqtt.session;
    if ( s_mqtt.session_set && (NULL != sess) )
    {
        /* The channel's session: the caller names the client and holds the
         * token, and the broker is told when we vanish. esp-mqtt copies every
         * string at init, so the buffer is free to be replaced afterwards. */
        cfg.credentials.client_id = sess->client_id;
        if ( '\0' != sess->user[0] )
        {
            cfg.credentials.username = sess->user;
        }
        if ( '\0' != sess->pass[0] )
        {
            cfg.credentials.authentication.password = sess->pass;
        }
        if ( '\0' != sess->will_topic[0] )
        {
            cfg.session.last_will.topic  = sess->will_topic;
            cfg.session.last_will.msg    = sess->will_msg;
            cfg.session.last_will.msg_len = (int)strlen(sess->will_msg);
            cfg.session.last_will.qos    = 0;
            if ( sess->will_retain )
            {
                cfg.session.last_will.retain = 1;
            }
        }
        cfg.session.keepalive = (int)sess->keepalive_s;
        /* Whether the broker keeps what was queued while the Kode was away:
         * a stable client id, no clean start and, in MQTT 5, an expiry. */
        cfg.session.disable_clean_session = sess->keep_session;
        if ( sess->mqtt5 )
        {
            cfg.session.protocol_ver = MQTT_PROTOCOL_V_5;
        }
        want_mqtt5 = sess->mqtt5;
        expiry_s   = sess->session_expiry_s;
        s_mqtt.sub_qos = (int)sess->sub_qos;
        if ( (uint8_t)KDRV_MQTT_SUB_QOS_MAX < sess->sub_qos )
        {
            s_mqtt.sub_qos = (int)KDRV_MQTT_SUB_QOS_MAX;
        }
        /* A dead broker is retried on the owner's back-off - doubling, with
         * jitter - not on esp-mqtt's flat ten seconds, which every Kode
         * would keep up in step for as long as the outage lasts. A drop
         * is reported and the client waits to be stopped. */
        cfg.network.disable_auto_reconnect = true;
        s_mqtt.redials = false;
        s_mqtt.channel = true;
    }
    else
    {
        s_mqtt.redials = true;
        s_mqtt.channel = false;
        s_mqtt.sub_qos = 0;
        /* The broker files sessions under this. Left unset, esp-mqtt invents
         * one from the chip id and two Kodes on one broker become
         * indistinguishable to anybody reading its logs. The canonical form,
         * not the printed one: this is a wire identifier, and nothing here is
         * read by a person. */
        if ( khal_identity_valid() )
        {
            cfg.credentials.client_id = khal_identity_serial();
        }
        if ( '\0' != req->user[0] )
        {
            cfg.credentials.username = req->user;
        }
        if ( '\0' != req->pass[0] )
        {
            cfg.credentials.authentication.password = req->pass;
        }
    }
    /* mqtts:// verifies against the same bundle HTTPS uses; plain mqtt://
     * never reaches for it. */
    cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;

    mqtt_uri_set(req->uri);
    s_mqtt.err_key = 0;

    s_mqtt.client = esp_mqtt_client_init(&cfg);
    /* esp-mqtt has copied every string by now (or has refused): the session
     * has done its one job, and a bench `mqtt_up` after it must not inherit
     * the channel's identity or find its token lying in memory. */
    mqtt_session_spent();
    if ( NULL == s_mqtt.client )
    {
        KLOG_E(TAG, "mqtt client refused '%s'", req->uri);
        return KODE_ERROR;
    }
    if ( want_mqtt5 )
    {
        /* The age the broker may keep the session for. Zero is not sent, and
         * means the session ends with the connection. */
        esp_mqtt5_connection_property_config_t prop;
        (void)memset(&prop, 0, sizeof(prop));
        prop.session_expiry_interval = expiry_s;
        prop.receive_maximum         = (uint16_t)KDRV_MQTT_RECEIVE_MAX;
        if ( esp_mqtt5_client_set_connect_property(s_mqtt.client, &prop) != ESP_OK )
        {
            KLOG_E(TAG, "mqtt5 connect property refused");
            (void)esp_mqtt_client_destroy(s_mqtt.client);
            s_mqtt.client = NULL;
            return KODE_ERROR;
        }
    }
    if ( esp_mqtt_client_register_event(s_mqtt.client, ESP_EVENT_ANY_ID,
                                       &mqtt_event, NULL) != ESP_OK )
    {
        (void)esp_mqtt_client_destroy(s_mqtt.client);
        s_mqtt.client = NULL;
        return KODE_ERROR;
    }

    s_mqtt.state = KDRV_MQTT_CONNECTING;
    if ( esp_mqtt_client_start(s_mqtt.client) != ESP_OK )
    {
        (void)esp_mqtt_client_destroy(s_mqtt.client);
        s_mqtt.client = NULL;
        s_mqtt.state  = KDRV_MQTT_DOWN;
        return KODE_ERROR;
    }
    /* Started, not connected: MQTT_EVENT_CONNECTED is the answer, and
     * AO_Net's deadline is what guarantees one either way. */
    return KODE_OK;
}

/** The MQTT_STOP job. Runs as a job because destroy joins esp-mqtt's
 *  task, which is not something an AO step may wait for. */
static kode_status_t mqtt_do_stop(void)
{
    if ( NULL == s_mqtt.client )
    {
        s_mqtt.state = KDRV_MQTT_DOWN;
        return KODE_OK;
    }
    (void)esp_mqtt_client_stop(s_mqtt.client);
    (void)esp_mqtt_client_destroy(s_mqtt.client);
    s_mqtt.client = NULL;
    s_mqtt.state  = KDRV_MQTT_DOWN;
    KLOG_N(TAG, "mqtt client stopped");
    return KODE_OK;
}

kode_status_t kdrv_net_mqtt_publish(const char *topic, const void *data,
                                    uint16_t len, bool retain)
{
    if ( (NULL == s_mqtt.client) || (KDRV_MQTT_UP != s_mqtt.state) )
    {
        return KODE_DENIED;
    }
    /* esp-mqtt reads len==0 as "this payload is a C string, measure it" - so
     * passing a caller's zero length straight through would make it strlen()
     * a buffer that is not a string. In this system that buffer is a pooled
     * event whose tail is whatever event used the block last, so an empty
     * publish would put a stranger's leftovers on the wire. An empty payload
     * is spelled out instead, and it is a legitimate MQTT message. */
    const char *payload = "";
    if ( 0U < len )
    {
        payload = (const char *)data;
    }
    int retain_flag = 0;
    if ( retain )
    {
        retain_flag = 1;
    }

    /* Sent inline, not stored: esp_mqtt_client_enqueue() hands the message to
     * esp-mqtt's task, which takes ONE outbox item per pass and ends each pass
     * in a one-second poll - a burst of reply lines would leave a line a
     * second, and the outbox silently drops what is older than 30 s or still
     * there when the client stops. A QoS 0 publish is written to the socket
     * before it returns and says -1 when it could not be, so a failure is
     * known now and counted by the caller. It may wait on the socket, which
     * AO_Net, the lowest-priority AO, can afford. */
    int const id = esp_mqtt_client_publish(s_mqtt.client, topic, payload,
                                           (int)len, 0, retain_flag);
    return (0 <= id) ? KODE_OK : KODE_NO_RESOURCES;
}

kode_status_t kdrv_net_mqtt_subscribe(const char *topic)
{
    kode_status_t st = KODE_DENIED;

    if ( (NULL != s_mqtt.client) && (KDRV_MQTT_UP == s_mqtt.state) )
    {
        st = KODE_OK;
        if ( esp_mqtt_client_subscribe(s_mqtt.client, topic, s_mqtt.sub_qos) < 0 )
        {
            st = KODE_ERROR;
        }
    }
    return st;
}

kode_status_t kdrv_net_mqtt_unsubscribe(const char *topic)
{
    if ( (NULL == s_mqtt.client) || (KDRV_MQTT_UP != s_mqtt.state) )
    {
        return KODE_DENIED;
    }
    return (esp_mqtt_client_unsubscribe(s_mqtt.client, topic) >= 0)
           ? KODE_OK : KODE_ERROR;
}

#else  /* !CONFIG_KODEOS_MQTT - the surface still links, and refuses */

/* The two jobs keep their shape so kdrv_net_run() below needs no #if: a build
 * without MQTT still answers a START it should never have been sent, and an
 * unanswered job is an AO waiting forever. */
/** @brief The MQTT_START job without MQTT: refused. */
static kode_status_t mqtt_do_start(const kdrv_net_req_t *req)
{
    (void)req;
    return KODE_DENIED;
}

/** @brief The MQTT_STOP job without MQTT: there is no client, so it is done. */
static kode_status_t mqtt_do_stop(void)
{
    return KODE_OK;
}

kode_status_t kdrv_net_mqtt_session(const kdrv_mqtt_session_t *s)
{
    (void)s;
    return KODE_DENIED;
}

void kdrv_net_mqtt_set_cb(kdrv_net_mqtt_cb_t cb)                { (void)cb; }
bool kdrv_net_mqtt_rx_take(kdrv_mqtt_msg_t *out)               { (void)out; return false; }
void kdrv_net_mqtt_rx_owe(void)                                 { }
uint32_t kdrv_net_mqtt_rx_dropped(void)                         { return 0U; }
kdrv_mqtt_state_t kdrv_net_mqtt_state(void)      { return KDRV_MQTT_DOWN; }

void kdrv_net_mqtt_uri_copy(char *out, size_t cap)
{
    if ( (NULL != out) && (0U < cap) )
    {
        out[0] = '\0';
    }
}

kode_status_t kdrv_net_mqtt_publish(const char *topic, const void *data,
                                    uint16_t len, bool retain)
{
    (void)topic; (void)data; (void)len; (void)retain;
    return KODE_DENIED;
}

kode_status_t kdrv_net_mqtt_subscribe(const char *topic)
{
    (void)topic;
    return KODE_DENIED;
}

kode_status_t kdrv_net_mqtt_unsubscribe(const char *topic)
{
    (void)topic;
    return KODE_DENIED;
}
#endif /* CONFIG_KODEOS_MQTT */

/* ---- The jobs ------------------------------------------------------------ */

uint8_t kdrv_net_ping_count(void)
{
    if ( NULL == s_net )
    {
        return 0U;
    }
    return s_net->ping_n;
}

bool kdrv_net_ping_get(uint8_t idx, kdrv_net_ping_reply_t *out)
{
    if ( (NULL == s_net) || (idx >= s_net->ping_n) || (NULL == out) )
    {
        return false;
    }
    *out = s_net->ping_rows[idx];
    return true;
}

void kdrv_net_ping_stats(kdrv_net_ping_stats_t *out)
{
    if ( NULL == out )
    {
        return;
    }
    if ( NULL == s_net )
    {
        (void)memset(out, 0, sizeof(*out));
        return;
    }
    *out = s_net->ping_stats;
}

void kdrv_net_http_result(kdrv_net_http_result_t *out)
{
    if ( NULL == out )
    {
        return;
    }
    if ( NULL == s_net )
    {
        (void)memset(out, 0, sizeof(*out));
        return;
    }
    *out = s_net->http;
}

void kdrv_net_http_forget(void)
{
    if ( NULL == s_net )
    {
        return;
    }
    (void)memset(s_net->body, 0, sizeof(s_net->body));
    s_net->http.kept = 0U;
}

uint32_t kdrv_net_http_body(uint32_t off, uint8_t *dst, uint32_t cap)
{
    if ( (NULL == s_net) || (NULL == dst) || (off >= s_net->http.kept) )
    {
        return 0U;
    }
    uint32_t n = s_net->http.kept - off;
    if ( n > cap )
    {
        n = cap;
    }
    (void)memcpy(dst, &s_net->body[off], n);
    return n;
}

uint16_t kdrv_net_udp_sent(void)
{
    if ( NULL == s_net )
    {
        return 0U;
    }
    return s_net->udp_sent;
}

/** The PING job: run the session into this driver's table. */
static kode_status_t net_do_ping(const kdrv_net_req_t *req)
{
    uint8_t count = req->count;
    NetResults * const r = net_results();

    if ( NULL == r )
    {
        return KODE_NO_RESOURCES;
    }
    if ( KDRV_NET_PING_MAX < count )
    {
        count = KDRV_NET_PING_MAX;
    }
    r->ping_n = 0U;
    /* The row count comes from the session itself, not from stats.tx: an
     * echo whose send failed - no route, the AP gone a moment ago - is a row
     * that tx never counted, and reporting tx rows would drop precisely the
     * losses somebody ran this command to see. */
    return kdrv_net_ping(req->host, count, r->ping_rows,
                         (uint8_t)KDRV_NET_PING_MAX, &r->ping_n,
                         &r->ping_stats);
}

/** The HTTP_GET job. */
static kode_status_t net_do_http(const kdrv_net_req_t *req)
{
    NetResults * const r = net_results();

    if ( NULL == r )
    {
        return KODE_NO_RESOURCES;
    }
    return kdrv_net_http_get(req->url, r->body, (uint32_t)KDRV_NET_BODY_MAX,
                             &r->http, NET_HTTP_TIMEOUT_MS);
}

/** The HTTP_POST job. A POST nobody gave a body to is refused here, with the
 *  result table cleared so a stale GET's answer is not read as its own. */
static kode_status_t net_do_http_post(const kdrv_net_req_t *req)
{
    NetResults * const r = net_results();

    if ( NULL == r )
    {
        return KODE_NO_RESOURCES;
    }
    if ( (NULL == s_post.body) || (0U == s_post.len) )
    {
        (void)memset(&r->http, 0, sizeof(r->http));
        r->http.content_length = -1;
        KLOG_W(TAG, "POST %s: no body was set", req->url);
        return KODE_ERROR;
    }
    return http_exchange(req->url, HTTP_METHOD_POST, s_post.body, s_post.len,
                         "application/json", NULL, false,
                         r->body, (uint32_t)KDRV_NET_BODY_MAX, &r->http,
                         NET_HTTP_TIMEOUT_MS);
}

/** The HTTP_PUT job. A PUT with nothing prepared - or whose bearer the last
 *  job already wiped - is refused here, with the result table cleared so a
 *  stale answer is not read as its own. The bearer and the URL (the token is
 *  in its path) are wiped as soon as the job is over, whatever it came to; a
 *  kept handle's own copy of the URL, kept_url, goes with the handle. The
 *  body stays: it is the glass, which any local console can read anyway.
 *  ESP-IDF's HTTP client is held at INFO for the exchange, because at DEBUG
 *  it logs the headers whole. */
static kode_status_t net_do_http_put(const kdrv_net_req_t *req)
{
    kode_status_t rc = KODE_ERROR;
    NetResults * const r = net_results();

    if ( NULL == r )
    {
        return KODE_NO_RESOURCES;
    }
    if ( (NULL == s_put) || (0U == s_put->len) || ('\0' == s_put->auth[0]) )
    {
        (void)memset(&r->http, 0, sizeof(r->http));
        r->http.content_length = -1;
        KLOG_W(TAG, "PUT: nothing was prepared");
    }
    else if ( !kode_log_hold(NET_HTTP_LOG_TAG, KODE_LOG_INFO) )
    {
        /* Sent unheld, the headers would reach the log at DEBUG. */
        (void)memset(&r->http, 0, sizeof(r->http));
        r->http.content_length = -1;
        KLOG_W(TAG, "PUT: the HTTP client's log could not be held quiet");
    }
    else
    {
        rc = http_exchange(
            s_put->url, HTTP_METHOD_PUT, (const char *)s_put->body, s_put->len,
            s_put->type, s_put->auth, req->keep,
            r->body, (uint32_t)KDRV_NET_BODY_MAX, &r->http,
            NET_HTTP_TIMEOUT_MS);
        kode_log_release(NET_HTTP_LOG_TAG);
    }
    if ( NULL != s_put )
    {
        (void)memset(s_put->auth, 0, sizeof(s_put->auth));
        (void)memset(s_put->url, 0, sizeof(s_put->url));
    }
    return rc;
}

/** The HTTP_CLOSE job: let go of what a kept PUT left open. The result table
 *  reads status 0, so the answer cannot be mistaken for a PUT's. */
static kode_status_t net_do_http_close(void)
{
    NetResults * const r = net_results();

    if ( NULL == r )
    {
        return KODE_NO_RESOURCES;
    }
    (void)memset(&r->http, 0, sizeof(r->http));
    r->http.content_length = -1;
    put_drop_kept();
    return KODE_OK;
}

/** The UDP_SEND job. */
static kode_status_t net_do_udp(const kdrv_net_req_t *req)
{
    NetResults * const r = net_results();

    if ( NULL == r )
    {
        return KODE_NO_RESOURCES;
    }
    r->udp_sent = 0U;
    kode_status_t const rc = kdrv_net_udp_send(req->host, req->port,
                                               req->data, req->len);
    if ( KODE_OK == rc )
    {
        r->udp_sent = req->len;
    }
    return rc;
}

kode_status_t kdrv_net_run(const kdrv_net_req_t *req)
{
    kode_status_t rc = KODE_DENIED;

    if ( NULL == req )
    {
        return KODE_DENIED;
    }
    switch ( req->type )
    {
        case KDRV_NET_JOB_PING:
            rc = net_do_ping(req);
            break;
        case KDRV_NET_JOB_HTTP_GET:
            rc = net_do_http(req);
            break;
        case KDRV_NET_JOB_HTTP_POST:
            rc = net_do_http_post(req);
            break;
        case KDRV_NET_JOB_HTTP_PUT:
            rc = net_do_http_put(req);
            break;
        case KDRV_NET_JOB_HTTP_CLOSE:
            rc = net_do_http_close();
            break;
        case KDRV_NET_JOB_UDP_SEND:
            rc = net_do_udp(req);
            break;
        case KDRV_NET_JOB_MQTT_START:
            rc = mqtt_do_start(req);
            break;
        case KDRV_NET_JOB_MQTT_STOP:
            rc = mqtt_do_stop();
            break;
        default:
            /* Refused, not skipped: a job that answers nobody leaves the
             * Active Object that ran it waiting out its own deadline. */
            KLOG_E(TAG, "job %d is not one of ours", (int)req->type);
            break;
    }
    return rc;
}

/* ---- The time -----------------------------------------------------------
 * A client that runs itself once started: it asks, it retries, and it calls
 * back on its own thread whenever a server answers. What this file adds is
 * the guard that makes starting it twice harmless, because AO_Net calls in
 * on every address it is given and does not count them.
 */

/** @brief Whether the client has been started. */
static bool s_sntp_up;

/** @brief What to call when a server answers. */
static void (*s_sntp_cb)(uint64_t unix_s);

/**
 * @brief The time client's own callback, on the client's thread.
 *
 * @param[in] tv What the server said.
 */
static void sntp_synced(struct timeval *tv)
{
    if ( (NULL != tv) && (NULL != s_sntp_cb) )
    {
        s_sntp_cb((uint64_t)tv->tv_sec);
    }
}

kode_status_t kdrv_net_sntp_start(void (*on_sync)(uint64_t unix_s))
{
    /* Idempotent by an explicit flag rather than by asking the library: its
     * state is not a question you can ask cheaply, and a second init is not
     * a no-op. */
    if ( s_sntp_up )
    {
        return KODE_OK;
    }

    s_sntp_cb = on_sync;

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = sntp_synced;

    if ( esp_netif_sntp_init(&cfg) != ESP_OK )
    {
        s_sntp_cb = NULL;
        return KODE_ERROR;
    }
    s_sntp_up = true;

    KLOG_I(TAG, "asking pool.ntp.org what time it is");

    return KODE_OK;
}

kode_status_t kdrv_net_sntp_sync_now(void)
{
    if ( !s_sntp_up )
    {
        return KODE_DENIED;
    }

    /* esp_netif_sntp_start() is a stop followed by an init, which is exactly
     * "ask again now" rather than "start", and is why the flag above and not
     * this call is what says whether the client exists. */
    if ( esp_netif_sntp_start() != ESP_OK )
    {
        return KODE_ERROR;
    }

    return KODE_OK;
}
