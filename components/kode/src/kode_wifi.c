/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_wifi.c
 * @brief The standard layer's Wi-Fi: join, ask, leave.
 */
#include "kode.h"

#include <inttypes.h>
#include <string.h>

#include "kdrv_wifi.h"
#include "khal_once.h"
#include "khal_sync.h"
#include "kode_entry.h"
#include "kode_standard_ao.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"

_Static_assert(KODE_WIFI_SSID_MAX == KODE_SYS_WIFI_SSID_MAX,
               "kode.h's network name and kode_sys.h's have drifted apart");
_Static_assert(sizeof(kode_wifi_network_t) == sizeof(kode_sys_wifi_ap_t),
               "a scanned network and kode_sys's have drifted apart");

/** @brief How long a request waits for AO_Radio's queue to have room, in ms. */
#define WIFI_DOOR_MS 2000U

/** @brief The lock every public Wi-Fi call that waits holds; made on the first call. */
static khal_mutex_t s_wifi_lock;

/** @brief Make the Wi-Fi lock. @return true when it exists. */
static bool wifi_lock_make(void)
{
    s_wifi_lock = khal_mutex_create();
    return NULL != s_wifi_lock;
}

/**
 * @brief kode_enter(), then the Wi-Fi turn; wifi_exit() gives it back.
 * @param name The public name of the call.
 * @return true when the call has its turn; false with the error set.
 */
static bool wifi_enter(const char *name)
{
    static khal_once_t lock_once;  /* the one-shot the lock is made by */
    bool               ok = kode_enter(name);

    if ( ok )
    {
        ok = khal_once_try(&lock_once, &wifi_lock_make);
        if ( ok )
        {
            khal_mutex_lock(s_wifi_lock);
        }
        else
        {
            kode_err_set("%s: no memory for the Wi-Fi lock", name);
        }
    }
    return ok;
}

/** @brief Give the turn back, at the end of a call wifi_enter() let in. */
static void wifi_exit(void)
{
    khal_mutex_unlock(s_wifi_lock);
}

/**
 * @brief Keep why the facade refused as the task's error.
 * @param rc The facade's answer.
 */
static void wifi_fail(kode_status_t rc)
{
    switch ( rc )
    {
    case KODE_DENIED:
        kode_err_set("this firmware has no Wi-Fi");
        break;
    case KODE_NO_RESOURCES:
        kode_err_set("Wi-Fi is busy");
        break;
    default:
        kode_err_set("Wi-Fi did not take the request");
        break;
    }
}

/**
 * @brief Check the arguments of a join before anything is posted.
 * @param ssid     The network; NULL for a remembered one.
 * @param password Its password; NULL for none.
 * @return true when both fit; false with the error set.
 */
static bool wifi_args_ok(const char *ssid, const char *password)
{
    bool ok = true;

    if ( NULL != ssid )
    {
        if ( ('\0' == ssid[0]) || (strlen(ssid) >= KODE_SYS_WIFI_SSID_MAX) )
        {
            kode_err_set("bad network name");
            ok = false;
        }
        else if ( (NULL != password) && (strlen(password) >= KODE_SYS_WIFI_PASS_MAX) )
        {
            kode_err_set("password too long");
            ok = false;
        }
        else
        {
            /* both fit */
        }
    }
    return ok;
}

/**
 * @brief Whether the board has an address on @p ssid.
 * @param st   A status just read.
 * @param ssid The network; NULL for any.
 * @return true when it has.
 */
static bool wifi_is_on(const kode_sys_wifi_status_t *st, const char *ssid)
{
    bool on = KODE_SYS_WIFI_STATE_GOT_IP == st->state;

    if ( on && (NULL != ssid) )
    {
        on = strcmp(st->ssid, ssid) == 0;
    }
    return on;
}

/** @brief A request to AO_Radio and its last answer. */
typedef struct {
    const char   *ssid;   /**< The network; NULL for a remembered one. */
    const char   *pass;   /**< Its password.                           */
    bool          leave;  /**< true to leave instead of join.          */
    kode_status_t rc;     /**< The last answer.                        */
} wifi_post_t;

/** @brief One try of a request: true unless AO_Radio's queue had no room. */
static bool wifi_post_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the wifi_post_t wifi_post() passed */
    wifi_post_t *const p = (wifi_post_t *)ctx;

    if ( p->leave )
    {
        p->rc = kode_sys_wifi_disconnect();
    }
    else
    {
        p->rc = kode_sys_wifi_connect(p->ssid, p->pass);
    }
    return KODE_NO_RESOURCES != p->rc;
}

/**
 * @brief Ask AO_Radio, waiting for room in its queue; the error is set on failure.
 * @param p The request.
 * @return true when it was posted.
 */
static bool wifi_post(wifi_post_t *p)
{
    (void)kode_retry(&wifi_post_attempt, p, WIFI_DOOR_MS);
    if ( KODE_OK != p->rc )
    {
        wifi_fail(p->rc);
    }
    return KODE_OK == p->rc;
}

/** @brief Where a join stands. */
typedef enum {
    WIFI_WAITING = 0,  /**< Not there yet.                          */
    WIFI_JOINED,       /**< An address on the asked network.        */
    WIFI_REFUSED,      /**< The network was tried, then given up.   */
    WIFI_UNAVAILABLE   /**< The facade has no Wi-Fi to ask.         */
} wifi_outcome_t;

/** @brief The watch over one join. */
typedef struct {
    const char     *ssid;     /**< The asked network; NULL for any.                    */
    bool            settled;  /**< A read other than CONNECTING came after the post.   */
    bool            trying;   /**< CONNECTING on the asked network came once settled.  */
    wifi_outcome_t  outcome;  /**< Where the join stands.                              */
    kode_status_t   rc;       /**< The facade's answer if it refused.                  */
} wifi_watch_t;

/** @brief One look at the join: true once it has an answer. */
static bool wifi_watch_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the wifi_watch_t wifi_wait_for_ip() passed */
    wifi_watch_t *const    w  = (wifi_watch_t *)ctx;
    kode_sys_wifi_status_t st = { 0U, 0U, "" };

    w->rc = kode_sys_wifi_status(&st);
    if ( KODE_OK != w->rc )
    {
        w->outcome = WIFI_UNAVAILABLE;
    }
    else if ( wifi_is_on(&st, w->ssid) )
    {
        w->outcome = WIFI_JOINED;
    }
    else if ( NULL == w->ssid )
    {
        /* the radio tries its remembered networks in turn: only the bound decides */
    }
    else if ( KODE_SYS_WIFI_STATE_CONNECTING == st.state )
    {
        /* an attempt that was running before the post is not this join's */
        if ( w->settled && (strcmp(st.ssid, w->ssid) == 0) )
        {
            w->trying = true;
        }
    }
    else
    {
        w->settled = true;
        if ( w->trying && ((KODE_SYS_WIFI_STATE_DISCONNECTED == st.state)
                          || (KODE_SYS_WIFI_STATE_IDLE == st.state)) )
        {
            w->outcome = WIFI_REFUSED;
        }
    }
    return WIFI_WAITING != w->outcome;
}

/**
 * @brief Wait for an address on @p ssid; the error is set on failure.
 * @param ssid  The network; NULL for any.
 * @param state The driver's state when the join was posted.
 * @return true when the board has the address.
 */
static bool wifi_wait_for_ip(const char *ssid, uint8_t state)
{
    wifi_watch_t   w       = { ssid, KODE_SYS_WIFI_STATE_CONNECTING != state, false, WIFI_WAITING, KODE_OK };
    const uint32_t seconds = KODE_WIFI_CONNECT_MS / KODE_MS_PER_S;

    (void)kode_retry(&wifi_watch_attempt, &w, KODE_WIFI_CONNECT_MS);
    switch ( w.outcome )
    {
    case WIFI_JOINED:
        break;
    case WIFI_UNAVAILABLE:
        wifi_fail(w.rc);
        break;
    case WIFI_REFUSED:
        kode_err_set("could not join '%s': wrong password or out of range", ssid);
        break;
    default:
        if ( NULL != ssid )
        {
            kode_err_set("no address from '%s' within %" PRIu32 " s", ssid, seconds);
        }
        else
        {
            kode_err_set("no remembered network answered within %" PRIu32 " s", seconds);
        }
        break;
    }
    return WIFI_JOINED == w.outcome;
}

/**
 * @brief The join itself, under the Wi-Fi lock.
 * @param ssid     The network; NULL for a remembered one.
 * @param password Its password.
 * @return true when the board has an address on the network.
 */
static bool wifi_join(const char *ssid, const char *password)
{
    kode_sys_wifi_status_t st = { 0U, 0U, "" };
    bool                   ok = wifi_args_ok(ssid, password);

    if ( ok )
    {
        const kode_status_t rc = kode_sys_wifi_status(&st);

        if ( KODE_OK != rc )
        {
            wifi_fail(rc);
            ok = false;
        }
        else if ( !wifi_is_on(&st, ssid) )
        {
            wifi_post_t p = { ssid, password, false, KODE_ERROR };

            ok = wifi_post(&p) && wifi_wait_for_ip(ssid, st.state);
        }
        else
        {
            /* already there */
        }
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
const char *kode_event_wifi_connected(void)
{
    return KODE_EVENT_WIFI_UP_NAME;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
const char *kode_event_wifi_lost(void)
{
    return KODE_EVENT_WIFI_LOST_NAME;
}

bool kode_wifi_connect(const char *ssid, const char *password)
{
    bool ok = wifi_enter("kode_wifi_connect");

    if ( ok )
    {
        ok = wifi_join(ssid, password);
        wifi_exit();
    }
    return ok;
}

bool kode_wifi_is_connected(void)
{
    kode_sys_wifi_status_t st = { 0U, 0U, "" };
    bool                   up = false;

    if ( kode_enter("kode_wifi_is_connected") )
    {
        const kode_status_t rc = kode_sys_wifi_status(&st);

        if ( KODE_OK == rc )
        {
            up = wifi_is_on(&st, NULL);
        }
        else
        {
            wifi_fail(rc);
        }
    }
    return up;
}

/** @brief One look at the link: true once the board has no address. */
static bool wifi_gone_attempt(void *ctx)
{
    kode_sys_wifi_status_t st = { 0U, 0U, "" };

    (void)ctx;
    return (kode_sys_wifi_status(&st) != KODE_OK) || !wifi_is_on(&st, NULL);
}

/**
 * @brief The leave itself, under the Wi-Fi lock.
 * @return true when the board has no address any more.
 */
static bool wifi_drop(void)
{
    kode_sys_wifi_status_t st = { 0U, 0U, "" };
    const kode_status_t    rc = kode_sys_wifi_status(&st);
    bool                   ok = KODE_OK == rc;

    if ( !ok )
    {
        wifi_fail(rc);
    }
    else if ( wifi_is_on(&st, NULL) )
    {
        wifi_post_t p = { NULL, NULL, true, KODE_ERROR };

        const uint32_t seconds = KODE_WIFI_LEAVE_MS / KODE_MS_PER_S;

        ok = wifi_post(&p);
        if ( ok && !kode_retry(&wifi_gone_attempt, NULL, KODE_WIFI_LEAVE_MS) )
        {
            kode_err_set("could not leave '%s' within %" PRIu32 " s", st.ssid, seconds);
            ok = false;
        }
    }
    else
    {
        /* not connected: nothing to leave */
    }
    return ok;
}

bool kode_wifi_disconnect(void)
{
    bool ok = wifi_enter("kode_wifi_disconnect");

    if ( ok )
    {
        ok = wifi_drop();
        wifi_exit();
    }
    return ok;
}

/** @brief Where kode_wifi_scan() keeps the strongest networks while they arrive. */
typedef struct {
    kode_wifi_network_t *out;  /**< The caller's array.         */
    size_t               cap;  /**< How many it holds.          */
    size_t               n;    /**< How many it holds so far.   */
} wifi_scan_keep_t;

/**
 * @brief Keep @p ap if it is among the strongest @c cap so far, in order.
 *
 * Strongest first; a network as strong as one kept goes after it, so the
 * sweep's order decides a tie.
 *
 * @param ap  One network.
 * @param ctx The wifi_scan_keep_t.
 * @return true: every network is looked at.
 */
static bool wifi_scan_keep(const kode_sys_wifi_ap_t *ap, void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the wifi_scan_keep_t kode_wifi_scan() passed */
    wifi_scan_keep_t *const k = (wifi_scan_keep_t *)ctx;
    size_t                  at = k->n;

    while ( (0U < at) && (k->out[at - 1U].rssi < ap->rssi) )
    {
        at--;
    }
    if ( at < k->cap )
    {
        size_t last = k->n;

        if ( last == k->cap )
        {
            last--;   /* the weakest kept falls off the end */
        }
        else
        {
            k->n++;
        }
        while ( last > at )
        {
            k->out[last] = k->out[last - 1U];
            last--;
        }
        (void)memcpy(k->out[at].ssid, ap->ssid, sizeof(k->out[at].ssid));
        k->out[at].rssi    = ap->rssi;
        k->out[at].channel = ap->channel;
        k->out[at].open    = ap->open;
    }
    return true;
}

/**
 * @brief Keep why the scan failed as the task's error.
 * @param rc What the facade answered.
 */
static void wifi_scan_fail(kode_status_t rc)
{
    const uint32_t seconds = KODE_WIFI_SCAN_MS / KODE_MS_PER_S;

    switch ( rc )
    {
    case KODE_DENIED:
        wifi_fail(rc);
        break;
    case KODE_NO_RESOURCES:
        kode_err_set("the radio is busy with a scan, a self-test or an install; try again");
        break;
    case KODE_TIMEOUT:
        kode_err_set("no answer to the scan within %" PRIu32 " s", seconds);
        break;
    default:
        kode_err_set("the Wi-Fi scan failed");
        break;
    }
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
int kode_wifi_scan(kode_wifi_network_t *out, size_t cap)
{
    int found = -1;

    if ( !wifi_enter("kode_wifi_scan") )
    {
        return found;
    }
    if ( (NULL == out) || (0U == cap) )
    {
        kode_err_set("kode_wifi_scan: no room for the networks");
    }
    else
    {
        wifi_scan_keep_t    keep = { out, cap, 0U };
        kode_status_t const rc   = kode_block_wifi_scan(&wifi_scan_keep, &keep, KODE_WIFI_SCAN_MS);

        if ( KODE_OK == rc )
        {
            found = (int)keep.n;
        }
        else
        {
            wifi_scan_fail(rc);
        }
    }
    wifi_exit();
    return found;
}
