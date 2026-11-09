/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file net_verbs.c
 * @brief AO_Net's console verbs: `net_*`, `http_get`, `udp_send`, `mqtt_*` and `channel_info`, registered by kaos_net_start().
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "kaos.h"
#include "kdrv_net.h"
#include "khal_identity.h"
#include "kode_log.h"
#include "kqp.h"

#include "console_verbs.h"
#include "kverbs.h"

/** @brief Which network verb holds the console. */
typedef enum {
    NET_V_NONE = 0,   /**< None of them.                                   */
    NET_V_PING,       /**< net_ping: entries until NET_PING_END_SIG; four
                       *   echoes a second apart outlast the watchdog.     */
    NET_V_HTTP,       /**< http_get: body records until NET_HTTP_DONE_SIG,
                       *   the watchdog handed over for the handshake.     */
    NET_V_UDP,        /**< udp_send: closed by NET_UDP_DONE_SIG.           */
    NET_V_INFO,       /**< net_info: closed by NET_INFO_SIG.               */
    NET_V_MQTT,       /**< mqtt_down/info: closed by MQTT_STATE_SIG.      */
    NET_V_MQTT_UP,    /**< mqtt_up: closed by MQTT_STATE_SIG, hands the
                       *   watchdog over, and is refused when the board
                       *   has no address.                                 */
    NET_V_MQTT_ACK,   /**< mqtt_pub/sub/unsub: closed by MQTT_STATE_SIG,
                       *   which says busy (the channel holds the client),
                       *   no network, or ok.                              */
    NET_V_CHANNEL     /**< channel_info: closed by CHANNEL_STATE_SIG.      */
} net_verb_kind_t;

static net_verb_kind_t s_net_kind;

#define NET_V_PING_COUNT_DEF_N  4                                /**< Echoes with no `count=`; bare, for `help`. */
#define NET_V_PING_COUNT_DEF    ((uint8_t)NET_V_PING_COUNT_DEF_N) /**< The same, as the request takes it. */
#define NET_V_BODY_PARTS        2U   /**< A body chunk too wide for one record goes out as this many. */
#define NET_V_MQTT_SCHEME       "mqtt://"   /**< The plain broker scheme `mqtt_up` takes.  */
#define NET_V_MQTTS_SCHEME      "mqtts://"  /**< The TLS broker scheme `mqtt_up` takes.    */

/* ---- Handlers: the network -----------------------------------------------
 * These run on the P4's own lwIP, so they do not queue behind the radio.
 */

/** @brief net_ping host=<h> [count=<n>]: entries, then the summary. */
static void h_net_ping(const kverb_t *c, const karg_val_t *v)
{
    /* Four echoes a second apart outlast the console's 5 s deadline, so the
     * watchdog stands down and AO_Net's answer, which always comes, closes
     * the command. */
    console_begin(c->verb);
    s_net_kind = NET_V_PING;
    console_unwatched();
    uint8_t count = NET_V_PING_COUNT_DEF;

    if ( v[1].present )
    {
        count = (uint8_t)v[1].u;
    }
    net_ping_request(v[0].s, count);
}

/** @brief net_info: closed by NET_INFO_SIG - answerable with no network. */
static void h_net_info(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_net_kind = NET_V_INFO;
    net_info_request();
}

/**
 * @brief http_get url=<u>: a printable preview of the body, then the status.
 *
 * The console's line is 80 bytes, so a URL typed here fits in about 70.
 */
static void h_http_get(const kverb_t *c, const karg_val_t *v)
{
    console_begin(c->verb);
    s_net_kind = NET_V_HTTP;
    console_unwatched();   /* a handshake plus a download */
    net_http_get_request(v[0].s);
}

/** @brief udp_send host=<h> port=<p> data=<d>: one datagram. */
static void h_udp_send(const kverb_t *c, const karg_val_t *v)
{
    console_begin(c->verb);
    s_net_kind = NET_V_UDP;
    /* A host that does not resolve costs the whole DNS retry budget, more
     * than the console's deadline. AO_Net answers either way. */
    console_unwatched();
    net_udp_send_request(v[0].s, (uint16_t)v[1].u, v[2].s,
                         (uint16_t)strlen(v[2].s));
}

#if CONFIG_KODEOS_MQTT
/** The highest TCP port. */
#define NET_V_PORT_MAX      65535U
/** The most digits a port has. */
#define NET_V_PORT_DIGITS   5U
/** Decimal: each digit is worth this many of the one after it. */
#define NET_V_DECIMAL       10U

/**
 * @brief Is @p port a TCP port: 1 to 5 digits, 1 to 65535?
 * @param port The text after the host's colon, up to the end of the authority.
 * @param len  Its length.
 * @return true when it is.
 */
static bool net_mqtt_port_ok(const char *port, size_t len)
{
    uint32_t value = 0U;
    bool     ok    = (0U < len) && (NET_V_PORT_DIGITS >= len);

    for ( size_t i = 0U; ok && (len > i); i++ )
    {
        if ( ('0' > port[i]) || ('9' < port[i]) )
        {
            ok = false;
        }
        else
        {
            value = (value * NET_V_DECIMAL) + ((uint32_t)(uint8_t)port[i] - (uint32_t)'0');
        }
    }
    return ok && (0U < value) && (NET_V_PORT_MAX >= value);
}

/**
 * @brief Where @p ch first stands in @p text between two places.
 * @param text The text.
 * @param from The first place looked at.
 * @param to   One past the last place looked at.
 * @param ch   The character.
 * @return Its place, or @p to when it is not there.
 */
static size_t net_find(const char *text, size_t from, size_t to, char ch)
{
    size_t at = to;

    for ( size_t i = from; (to > i) && (to == at); i++ )
    {
        if ( ch == text[i] )
        {
            at = i;
        }
    }
    return at;
}

/**
 * @brief Whether a character may stand in a host's name or address.
 *
 * A name: letters, digits, '-', '_' and '.'. Inside the brackets of an IPv6
 * address: hex digits, ':' and '.'. Spelled out rather than isalnum(), which
 * follows the locale.
 *
 * @param ch        The character.
 * @param bracketed true inside the brackets of an IPv6 address.
 * @return true when it may.
 */
static bool net_host_char_ok(char ch, bool bracketed)
{
    bool const digit = (ch >= '0') && (ch <= '9');
    bool       ok;

    if ( bracketed )
    {
        ok = digit || ((ch >= 'a') && (ch <= 'f')) || ((ch >= 'A') && (ch <= 'F'))
             || (':' == ch) || ('.' == ch);
    }
    else
    {
        ok = digit || ((ch >= 'a') && (ch <= 'z')) || ((ch >= 'A') && (ch <= 'Z'))
             || ('-' == ch) || ('_' == ch) || ('.' == ch);
    }
    return ok;
}

/**
 * @brief Whether text[from..to) is a host: one character at least, each one
 *        net_host_char_ok(), and a plain name neither starts nor ends with
 *        '.' or '-' (so '.', '..' and '-' are not hosts).
 *
 * @param text      The authority.
 * @param from      Where the host starts.
 * @param to        One past where it ends.
 * @param bracketed true for the inside of an address in brackets.
 * @return true when it is.
 */
static bool net_host_ok(const char *text, size_t from, size_t to, bool bracketed)
{
    bool ok = (from < to);

    for ( size_t i = from; ok && (to > i); i++ )
    {
        ok = net_host_char_ok(text[i], bracketed);
    }
    if ( ok && !bracketed )
    {
        ok = ('.' != text[from]) && ('-' != text[from])
             && ('.' != text[to - 1U]) && ('-' != text[to - 1U]);
    }
    return ok;
}

/**
 * @brief Is @p uri a broker address: `mqtt://` or `mqtts://`, a host and,
 *        when there is one, a port?
 *
 * The authority runs to the first `/`, `?` or `#`. A user and password end
 * at its last `@`, and what follows is the host: a name, or an address in
 * brackets, then an optional `:port`.
 *
 * @param uri The text given to `mqtt_up`.
 * @return true when the scheme, a host and any port are well formed.
 */
static bool net_mqtt_uri_ok(const char *uri)
{
    size_t skip = 0U;
    bool   ok   = false;

    if ( 0 == strncmp(uri, NET_V_MQTT_SCHEME, sizeof(NET_V_MQTT_SCHEME) - 1U) )
    {
        skip = sizeof(NET_V_MQTT_SCHEME) - 1U;
    }
    if ( 0 == strncmp(uri, NET_V_MQTTS_SCHEME, sizeof(NET_V_MQTTS_SCHEME) - 1U) )
    {
        skip = sizeof(NET_V_MQTTS_SCHEME) - 1U;
    }
    if ( 0U < skip )
    {
        const char * const auth = &uri[skip];
        size_t const       end  = strcspn(auth, "/?#");
        size_t             host = 0U;
        size_t             host_end;
        size_t             colon;

        for ( size_t i = 0U; end > i; i++ )
        {
            if ( '@' == auth[i] )
            {
                host = i + 1U;
            }
        }
        if ( (end > host) && ('[' == auth[host]) )
        {
            /* An address in brackets: the port's colon comes after them. */
            size_t const close = net_find(auth, host, end, ']');

            host_end = close + 1U;
            colon    = end;
            if ( (end > host_end) && (':' == auth[host_end]) )
            {
                colon = host_end;
            }
            ok = (end > close) && ((host + 2U) < host_end)
                 && ((end == host_end) || (colon == host_end))
                 && net_host_ok(auth, host + 1U, close, true);
        }
        else
        {
            colon    = net_find(auth, host, end, ':');
            host_end = colon;
            ok = net_host_ok(auth, host, host_end, false);
        }
        if ( ok && (end > colon) )
        {
            ok = net_mqtt_port_ok(&auth[colon + 1U], end - colon - 1U);
        }
    }
    return ok;
}

/** @brief mqtt_up uri="<u>" [user="<u>"] [pass="<p>"]: closed by MQTT_STATE_SIG. */
static void h_mqtt_up(const kverb_t *c, const karg_val_t *v)
{
    const char *user = "";
    const char *pass = "";

    if ( !net_mqtt_uri_ok(v[0].s) )
    {
        console_reply_err(KCLI_ERR_VALUE, "uri");
        return;
    }
    console_begin(c->verb);
    s_net_kind = NET_V_MQTT_UP;
    /* Reaching a broker outlasts any console deadline; AO_Net carries its own
     * (KODE_MQTT_CONNECT_MS) and answers with it. */
    console_unwatched();
    if ( v[1].present )
    {
        user = v[1].s;
    }
    if ( v[2].present )
    {
        pass = v[2].s;
    }
    /* With the watchdog disarmed, only MQTT_STATE_SIG closes the command,
     * and a request the queue or the pool refused will never have one. */
    if ( !mqtt_up_request(v[0].s, user, pass) )
    {
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        console_end();
    }
}

/** @brief mqtt_down: stop the client. Closed by MQTT_STATE_SIG. */
static void h_mqtt_down(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_net_kind = NET_V_MQTT;
    /* Destroying the client joins esp-mqtt's own task, which may be inside a
     * transport connect to a broker that is not answering - so stopping can
     * take longer than starting did. */
    console_unwatched();
    if ( !mqtt_down_request() )
    {
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        console_end();
    }
}

/** @brief mqtt_info: closed by MQTT_STATE_SIG. */
static void h_mqtt_info(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_net_kind = NET_V_MQTT;
    mqtt_state_request();
}

/**
 * @brief mqtt_pub topic=<t> data=<d>: an accept, like `install` off a medium.
 *
 * `>ok` says the request was taken; a missing broker says so in a '!' line.
 * The other answers are `>err code=4 msg=busy` when the Kode channel holds the
 * client, and a REJECT when the request cannot be posted. `mqtt_sub` and
 * `mqtt_unsub` answer the same way.
 */
static void h_mqtt_pub(const kverb_t *c, const karg_val_t *v)
{
    console_begin(c->verb);
    s_net_kind = NET_V_MQTT_ACK;
    if ( !mqtt_pub_request(v[0].s, v[1].s, (uint16_t)strlen(v[1].s)) )
    {
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        console_end();
    }
}

/** @brief mqtt_sub topic=<t>: an accept; messages arrive as '!' lines. */
static void h_mqtt_sub(const kverb_t *c, const karg_val_t *v)
{
    console_begin(c->verb);
    s_net_kind = NET_V_MQTT_ACK;
    if ( !mqtt_sub_request(v[0].s) )
    {
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        console_end();
    }
}

/** @brief mqtt_unsub topic=<t>: an accept. */
static void h_mqtt_unsub(const kverb_t *c, const karg_val_t *v)
{
    console_begin(c->verb);
    s_net_kind = NET_V_MQTT_ACK;
    if ( !mqtt_unsub_request(v[0].s) )
    {
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        console_end();
    }
}

/* `channel_info` is one record on one line. The broker varies most, so it is
 * quoted last into what the other fields leave: at their widest, at least
 * CHAN_BROKER_FLOOR. A URI that does not fit is cut by console_kv_fit(), which
 * says so on the log. */
/** Tenths in one frame a second: `fps_got` is kept times ten. */
#define SHOT_FPS_TENTHS    10U
/** The longest `why` the record carries: ChannelStateEvt.why less its NUL. */
#define CHAN_WHY_CHARS     23U
/** The least a quoted broker gets at the record's widest, quotes included
 *  (the line's newline takes the byte after it, kode_log_reply_src()). */
#define CHAN_BROKER_FLOOR  18U
/* Two widest records, because `keep=` and `session=` say something only while
 * the channel is up and `next_s=` only while it waits. Every counter is at
 * 4294967295, every word its longest and `serial`, `why` and the broker's
 * value are left out. */
/** Not up: `why` as long as it goes and a next try owed, `keep=-
 *  session=-`; the longest state that waits is `waiting`. */
#define CHAN_WIDEST_DOWN ">|channel=waiting serial= broker= " \
                    "since_s=4294967295 why= next_s=4294967295 " \
                    "lost=4294967295 fast=off shot=stream " \
                    "sent=4294967295 fps_got=6553.5 left_s=4294967295 " \
                    "keep=- session=-"
/** Up: no `why` and no next try; the longest age is `604800` (seven days), as
 *  AO_Net asks for no other number but `never`. */
#define CHAN_WIDEST_UP ">|channel=up serial= broker= " \
                    "since_s=4294967295 why=- next_s=- " \
                    "lost=4294967295 fast=off shot=stream " \
                    "sent=4294967295 fps_got=6553.5 left_s=4294967295 " \
                    "keep=604800 session=resumed"
_Static_assert((sizeof(CHAN_WIDEST_DOWN) + CHAN_WHY_CHARS
                + CONSOLE_QUOTED_MAX(KHAL_ID_LEN) + CHAN_BROKER_FLOOR)
               <= (KODE_LOG_LINE_MAX - 1U),   /* the line's newline */
               "channel_info: the widest record leaves a short URI room");
_Static_assert((sizeof(CHAN_WIDEST_UP) + CONSOLE_QUOTED_MAX(KHAL_ID_LEN)
                + CHAN_BROKER_FLOOR) <= (KODE_LOG_LINE_MAX - 1U),
               "channel_info: the widest record, up, leaves a short URI room");
/** The start of the record, through `broker=`, with the longest serial. */
#define CHAN_HEAD_MAX  (sizeof(">|channel=no_identity serial= broker=") \
                        + CONSOLE_QUOTED_MAX(KHAL_ID_LEN))

/** @brief channel_info: where the Kode channel stands. Closed by CHANNEL_STATE_SIG. */
static void h_channel_info(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_net_kind = NET_V_CHANNEL;
    (void)channel_state_request();
}
#endif /* CONFIG_KODEOS_MQTT */

/** @brief AO_Net's answers to the verb in flight. */
static bool net_answer(QEvt const *e)
{
    bool handled = true;

    switch ( e->sig )
    {
        case NET_PING_ENTRY_SIG: {
            if ( NET_V_PING == s_net_kind )
            {
                NetPingEntryEvt const * const p = Q_EVT_CAST(NetPingEntryEvt);
                if ( p->ok )
                {
                    console_reply(">|seq=%" PRIu32 " time_ms=%" PRIu32 " ttl=%" PRIu32, (uint32_t)p->seq,
                          (uint32_t)p->time_ms, (uint32_t)p->ttl);
                }
                else
                {
                    /* Printed rather than skipped: a gap in the sequence
                     * numbers would make the reader count losses itself. */
                    console_reply(">|seq=%" PRIu32 " timeout=on", (uint32_t)p->seq);
                }
            }
            else
            {
                handled = false;
            }
            break;
        }
        case NET_PING_END_SIG: {
            if ( NET_V_PING == s_net_kind )
            {
                NetPingEndEvt const * const p = Q_EVT_CAST(NetPingEndEvt);
                if ( !p->ok )
                {
                    console_reply_err(KCLI_ERR_REJECT, console_verb());
                }
                else
                {
                    char ip[CONSOLE_IP4_TEXT_MAX];

                    console_reply(">|ip=%s tx=%" PRIu32 " rx=%" PRIu32
                          " min_ms=%" PRIu32 " avg_ms=%" PRIu32 " max_ms=%" PRIu32,
                          console_ip4(ip, sizeof(ip), p->ip4),
                          (uint32_t)p->tx, (uint32_t)p->rx,
                          (uint32_t)p->min_ms, (uint32_t)p->avg_ms,
                          (uint32_t)p->max_ms);
                    console_reply(">ok");
                }
                console_end();
            }
            else
            {
                handled = false;
            }
            break;
        }
        case NET_HTTP_DATA_SIG: {
            if ( NET_V_HTTP == s_net_kind )
            {
                NetHttpDataEvt const * const d = Q_EVT_CAST(NetHttpDataEvt);
                /* Already printable (AO_Net rewrote the rest). A chunk that
                 * outgrows a line once escaped goes out as two records, which
                 * a host joins in order; a cut would make `shown=` wrong. */
                char q[CONSOLE_QUOTED_MAX(KODE_NET_HTTP_CHUNK)];
                if ( (console_quoted_size(d->text) + (sizeof(">|body=") - 1U))
                    <= (size_t)KODE_LOG_LINE_MAX )
                {
                    console_reply(">|body=%s", console_quote(q, sizeof(q), d->text));
                }
                else
                {
                    char   half[(KODE_NET_HTTP_CHUNK / NET_V_BODY_PARTS) + 1U];
                    size_t const n = strlen(d->text) / NET_V_BODY_PARTS;
                    (void)memcpy(half, d->text, n);
                    half[n] = '\0';
                    console_reply(">|body=%s", console_quote(q, sizeof(q), half));
                    console_reply(">|body=%s", console_quote(q, sizeof(q), &d->text[n]));
                }
            }
            else
            {
                handled = false;
            }
            break;
        }
        case NET_HTTP_DONE_SIG: {
            if ( NET_V_HTTP == s_net_kind )
            {
                NetHttpDoneEvt const * const h = Q_EVT_CAST(NetHttpDoneEvt);
                if ( (uint8_t)KODE_OK != h->rc )
                {
                    console_reply_err(KCLI_ERR_REJECT, console_verb());
                }
                else
                {
                    /* The status is the server's answer and rc was the
                     * transport's: a 404 closes with `>ok` because the
                     * request worked - what failed was the URL. */
                    console_reply(">|status=%" PRIu32 " content_len=%" PRId32 " body_len=%" PRIu32
                          " shown=%" PRIu32 " truncated=%s",
                          (uint32_t)h->status, (int32_t)h->content_length,
                          (uint32_t)h->body_len, (uint32_t)h->kept,
                          console_on_off(h->truncated));
                    console_reply(">ok");
                }
                console_end();
            }
            else
            {
                handled = false;
            }
            break;
        }
        case NET_UDP_DONE_SIG: {
            if ( NET_V_UDP == s_net_kind )
            {
                NetUdpDoneEvt const * const u = Q_EVT_CAST(NetUdpDoneEvt);
                if ( (uint8_t)KODE_OK != u->rc )
                {
                    console_reply_err(KCLI_ERR_REJECT, console_verb());
                }
                else
                {
                    console_reply(">|sent=%" PRIu32, (uint32_t)u->sent);
                    console_reply(">ok");
                }
                console_end();
            }
            else
            {
                handled = false;
            }
            break;
        }
        case NET_INFO_SIG: {
            if ( NET_V_INFO == s_net_kind )
            {
                NetInfoEvt const * const n = Q_EVT_CAST(NetInfoEvt);
                static const char *const MQTTS[] = {
                    "down", "connecting", "up",
                };                                 /* kdrv_mqtt_state_t order */
                char ip[CONSOLE_IP4_TEXT_MAX];
                char gw[CONSOLE_IP4_TEXT_MAX];
                char mask[CONSOLE_IP4_TEXT_MAX];
                char dns[CONSOLE_IP4_TEXT_MAX];

                console_reply(">|ip=%s gw=%s mask=%s dns=%s mqtt=%s rx_dropped=%" PRIu32,
                      console_ip4(ip, sizeof(ip), n->ip4),
                      console_ip4(gw, sizeof(gw), n->gw4),
                      console_ip4(mask, sizeof(mask), n->mask4),
                      console_ip4(dns, sizeof(dns), n->dns4),
                      console_word(MQTTS, sizeof(MQTTS) / sizeof(MQTTS[0]), n->mqtt),
                      (uint32_t)n->rx_dropped);
                console_reply(">ok");
                console_end();
            }
            else
            {
                handled = false;
            }
            break;
        }
#if CONFIG_KODEOS_MQTT
        case MQTT_STATE_SIG: {
            if ( NET_V_MQTT_ACK == s_net_kind )
            {
                MqttStateEvt const * const m = Q_EVT_CAST(MqttStateEvt);
                if ( (uint8_t)KDRV_MQTT_ERR_BUSY == m->err )
                {
                    /* The channel's client is not the bench's to publish on,
                     * subscribe or unsubscribe. */
                    console_reply_err(KCLI_ERR_BUSY, "busy");
                }
                else if ( (uint8_t)KDRV_MQTT_ERR_NO_NET == m->err )
                {
                    console_reply_err(KCLI_ERR_REJECT, "no_net");
                }
                else
                {
                    console_reply(">ok");
                }
                console_end();
                break;
            }
            if ( (NET_V_MQTT == s_net_kind) || (NET_V_MQTT_UP == s_net_kind) )
            {
                MqttStateEvt const * const m = Q_EVT_CAST(MqttStateEvt);
                if ( (uint8_t)KDRV_MQTT_ERR_BUSY == m->err )
                {
                    /* The Kode channel holds the one client: a request for it
                     * is refused, not answered with a state. */
                    console_reply_err(KCLI_ERR_BUSY, "busy");
                    console_end();
                    break;
                }
                if ( (NET_V_MQTT_UP == s_net_kind)
                    && ((uint8_t)KDRV_MQTT_ERR_NO_NET == m->err) )
                {
                    /* A client that was asked for and cannot start is a
                     * failed command; `mqtt_info` and `mqtt_down` may still
                     * report "down, no_net" as the state they found. */
                    console_reply_err(KCLI_ERR_REJECT, "no_net");
                    console_end();
                    break;
                }
                static const char *const MQTTS[] = {
                    "down", "connecting", "up",
                };                                 /* kdrv_mqtt_state_t order */
                static const char *const WHY[] = {
                    "none", "no_net", "refused", "timeout",
                };                                 /* kdrv_mqtt_err_t order   */
                /* A URI at its cap fits whole; one that escapes its way past
                 * the rest of the line is cut by console_kv_fit(), which says so. */
                char safe[KODE_LOG_LINE_MAX
                          - sizeof(">|mqtt=connecting uri= why=refused")];
                const char *uri = "-";
                if ( '\0' != m->uri[0] )
                {
                    uri = console_kv_fit(safe, sizeof(safe), m->uri, "uri");
                }
                /* Closed with `>ok` whatever the state: "down, timeout" is
                 * an answer, not a failed command. */
                console_reply(">|mqtt=%s uri=%s why=%s",
                      console_word(MQTTS, sizeof(MQTTS) / sizeof(MQTTS[0]), m->state),
                      uri,
                      console_word(WHY, sizeof(WHY) / sizeof(WHY[0]), m->err));
                console_reply(">ok");
                console_end();
            }
            else
            {
                handled = false;
            }
            break;
        }
        case CHANNEL_STATE_SIG: {
            if ( NET_V_CHANNEL == s_net_kind )
            {
                ChannelStateEvt const * const m = Q_EVT_CAST(ChannelStateEvt);
                static const char *const STATES[] = {
                    "off", "no_net", "no_identity", "joining", "up",
                    "waiting", "refused",
                };                                 /* chan_state_t order */
                static const char *const SHOTS[] = {
                    "idle", "one", "stream",
                };                                 /* kshot_mode_t order */
                char        why[sizeof(m->why)];
                char        next[sizeof("4294967295")];
                char        qs[CONSOLE_QUOTED_MAX(KHAL_ID_LEN)];
                char        keep[sizeof("4294967295")];
                const char *session = "-";
                char        head[CHAN_HEAD_MAX];
                char        tail[KODE_LOG_LINE_MAX];
                char        safe[CONSOLE_QUOTED_MAX(KODE_MQTT_URI_MAX)];
                const char *state  = "?";
                const char *serial = NULL;
                const char *broker = "-";
                const char *wordy  = "-";
                const char *fast   = "off";
                const char *shot   = "?";

                if ( (uint8_t)(sizeof(SHOTS) / sizeof(SHOTS[0])) > m->shot )
                {
                    shot = SHOTS[m->shot];
                }
                if ( m->fast )
                {
                    fast = "on";
                }
                if ( (uint8_t)(sizeof(STATES) / sizeof(STATES[0])) > m->state )
                {
                    state = STATES[m->state];
                }
                /* A word, whatever the server sent: anything else on the line
                 * would end the record early or start another. */
                (void)memset(why, 0, sizeof(why));
                for ( size_t i = 0U; (i + 1U) < sizeof(why); ++i )
                {
                    char const ch = m->why[i];
                    if ( '\0' == ch )
                    {
                        break;
                    }
                    why[i] = '_';
                    if ( (('a' <= ch) && ('z' >= ch))
                        || (('A' <= ch) && ('Z' >= ch))
                        || (('0' <= ch) && ('9' >= ch))
                        || ('_' == ch) || ('.' == ch) || ('-' == ch) )
                    {
                        why[i] = ch;
                    }
                }
                if ( '\0' != why[0] )
                {
                    wordy = why;
                }
                /* The next try means something only while one is owed. */
                (void)snprintf(next, sizeof(next), "-");
                if ( (uint8_t)CHAN_WAITING == m->state )
                {
                    (void)snprintf(next, sizeof(next), "%" PRIu32,
                                   (uint32_t)m->next_s);
                }
                if ( (uint8_t)CHAN_SESSION_RESUMED == m->session )
                {
                    session = "resumed";
                }
                else if ( (uint8_t)CHAN_SESSION_NEW == m->session )
                {
                    session = "new";
                }
                else
                {
                    /* Not up: the broker has said nothing. */
                }
                /* The age asked for means something only while the session is up. */
                (void)snprintf(keep, sizeof(keep), "-");
                if ( KDRV_MQTT_EXPIRY_NEVER == m->keep_s )
                {
                    (void)snprintf(keep, sizeof(keep), "never");
                }
                else if ( 0U != m->keep_s )
                {
                    (void)snprintf(keep, sizeof(keep), "%" PRIu32,
                                   (uint32_t)m->keep_s);
                }
                else
                {
                    /* Not up: no age was asked of the broker. */
                }
                if ( khal_identity_valid() )
                {
                    serial = khal_identity_serial();
                }
                (void)snprintf(head, sizeof(head), ">|channel=%s serial=%s broker=",
                               state, console_kv_text(qs, sizeof(qs), serial));
                (void)snprintf(tail, sizeof(tail),
                               " since_s=%" PRIu32 " why=%s next_s=%s lost=%" PRIu32 " "
                               "fast=%s shot=%s sent=%" PRIu32 " fps_got=%" PRIu32 ".%" PRIu32 " "
                               "left_s=%" PRIu32 " keep=%s session=%s",
                               (uint32_t)m->since_s, wordy, next,
                               (uint32_t)m->lost, fast, shot,
                               (uint32_t)m->shot_sent,
                               (uint32_t)(m->shot_fps_x10 / SHOT_FPS_TENTHS),
                               (uint32_t)(m->shot_fps_x10 % SHOT_FPS_TENTHS),
                               (uint32_t)m->shot_left_s, keep, session);
                if ( '\0' != m->broker[0] )
                {
                    /* What the rest of the line leaves, NUL and newline apart;
                     * the static assert above keeps it at CHAN_BROKER_FLOOR or
                     * more. */
                    size_t room = (KODE_LOG_LINE_MAX - 1U) - strlen(head)
                                  - strlen(tail);
                    if ( sizeof(safe) < room )
                    {
                        room = sizeof(safe);
                    }
                    broker = console_kv_fit(safe, room, m->broker, "broker");
                }
                /* Closed with `>ok` whatever the state, like `mqtt_info`:
                 * waiting or refused is an answer, not a failed command. */
                console_reply("%s%s%s", head, broker, tail);
                console_reply(">ok");
                console_end();
            }
            else
            {
                handled = false;
            }
            break;
        }
#endif
        default: {
            handled = false;
            break;
        }
    }
    return handled;
}

static const karg_spec_t A_NET_PING[] = {
    { .key = "host",  .required = true,  .type = KARG_STRING },
    /* Capped at the listing ceiling rather than at anything ICMP says: every
     * echo is an event every subscriber receives (KODE_NET_PING_MAX). */
    { .key = "count", .required = false, .type = KARG_UINT,
      .min = 1U, .max = KODE_NET_PING_MAX,
      .def = CONSOLE_STRINGIFY(NET_V_PING_COUNT_DEF_N) },
};
static const karg_spec_t A_URL[] = {
    { .key = "url", .required = true, .type = KARG_STRING },
};
static const karg_spec_t A_UDP_SEND[] = {
    { .key = "host", .required = true, .type = KARG_STRING },
    { .key = "port", .required = true, .type = KARG_UINT,
      .min = 1U, .max = 65535U },
    { .key = "data", .required = true, .type = KARG_STRING },
};
#if CONFIG_KODEOS_MQTT
static const karg_spec_t A_MQTT_UP[] = {
    { .key = "uri",  .required = true,  .type = KARG_STRING },
    /* Optional: anonymous brokers are the common case on a bench. */
    { .key = "user", .required = false, .type = KARG_STRING },
    { .key = "pass", .required = false, .type = KARG_STRING },
};
static const karg_spec_t A_MQTT_PUB[] = {
    { .key = "topic", .required = true, .type = KARG_STRING },
    { .key = "data",  .required = true, .type = KARG_STRING },
};
static const karg_spec_t A_TOPIC[] = {
    { .key = "topic", .required = true, .type = KARG_STRING },
};
#endif

static const kverb_t NET_VERBS[] = {
    /* `net_ping`, not `ping`: this protocol's `ping` is the liveness check
     * every host tool already speaks. */
    { "net_ping",     A_NET_PING, KVERB_KEYS(A_NET_PING), h_net_ping     },
    { "net_info",     NULL,       0U,                     h_net_info     },
    { "http_get",     A_URL,      KVERB_KEYS(A_URL),      h_http_get     },
    { "udp_send",     A_UDP_SEND, KVERB_KEYS(A_UDP_SEND), h_udp_send     },
#if CONFIG_KODEOS_MQTT
    { "mqtt_up",      A_MQTT_UP,  KVERB_KEYS(A_MQTT_UP),  h_mqtt_up      },
    { "mqtt_down",    NULL,       0U,                     h_mqtt_down    },
    { "mqtt_pub",     A_MQTT_PUB, KVERB_KEYS(A_MQTT_PUB), h_mqtt_pub     },
    { "mqtt_sub",     A_TOPIC,    KVERB_KEYS(A_TOPIC),    h_mqtt_sub     },
    { "mqtt_unsub",   A_TOPIC,    KVERB_KEYS(A_TOPIC),    h_mqtt_unsub   },
    { "mqtt_info",    NULL,       0U,                     h_mqtt_info    },
    { "channel_info", NULL,       0U,                     h_channel_info },
#endif
};

const kverbs_t net_verbs = {
    .verbs  = NET_VERBS, .n = sizeof(NET_VERBS) / sizeof(NET_VERBS[0]),
    .answer = net_answer,
};
