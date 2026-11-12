/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_kdrv_ble.c
 * @brief The BLE half of the C5, as a state a case can arrange.
 */
#include <stdio.h>
#include <string.h>

#include "fakes.h"
#include "kdrv_ble.h"

static struct {
    bool             up;
    bool             synced;
    bool             fail_start;
    bool             fail_scan;
    kdrv_ble_state_t state;
    uint8_t          addr[6];
    kdrv_ble_cb_t    cb;
    unsigned         starts;
    unsigned         scans;
    uint32_t         last_scan_ms;
    uint8_t          air_n;
    int8_t           air_top;
    uint8_t          count;
    kdrv_ble_dev_t   dev[KDRV_BLE_SCAN_MAX];
    uint8_t          bonds;
    uint32_t         links;
    uint16_t         adv_itvl;
    uint32_t         adv_ms;
    bool             adv_hidden;
    bool             adv_pulsed;
    bool             calm_pulsed; /**< What the last kdrv_ble_adv_calm() was asked for. */
    bool             secure;
    char             name[KDRV_BLE_ADV_MAX];  /**< What set_scan_rsp() named. */
    unsigned         conn_rest;   /**< kdrv_ble_conn_calm(true) calls.  */
    unsigned         conn_awake;  /**< kdrv_ble_conn_calm(false) calls. */
} s_b;

/** @brief The app's service a case declared; not cleared by fake_ble_reset(). */
static const kdrv_ble_app_service_t *s_app_svc;

void fake_ble_app_service_set(const kdrv_ble_app_service_t *svc)
{
    s_app_svc = svc;
}

const kdrv_ble_app_service_t *kdrv_ble_app_declared(void)
{
    return s_app_svc;
}

uint32_t kdrv_ble_links(void)
{
    return s_b.links;
}

/** @brief Wake-ups asked of the drain; not cleared by fake_ble_reset(). */
static unsigned s_app_kicks;

size_t kdrv_ble_app_host_bytes(void)
{
    return 0U;
}

void kdrv_ble_app_kick(void)
{
    s_app_kicks++;
}

unsigned fake_ble_app_kicks(void)
{
    return s_app_kicks;
}

/** @brief Whether the app declared the Battery service; not cleared by fake_ble_reset(). */
static bool s_batt_declared;

/** @brief The last level handed down; not cleared by fake_ble_reset(). */
static int16_t s_batt_pct = -1;

bool kdrv_ble_battery_declared(void)
{
    return s_batt_declared;
}

void kdrv_ble_battery_set(uint8_t pct)
{
    s_batt_pct = (int16_t)pct;
}

void fake_ble_battery_declare(bool on)
{
    s_batt_declared = on;
    s_batt_pct      = -1;
}

int16_t fake_ble_battery_pct(void)
{
    return s_batt_pct;
}

/* ---- The mirror half, unchanged in name and behaviour ----------------------- */

static kode_status_t s_write_result = KODE_OK;
static bool           s_attached;
static unsigned       s_write_calls;

void fake_ble_mirror_reset(void)
{
    s_attached     = false;
    s_write_result = KODE_OK;
    s_write_calls  = 0U;
}

void fake_ble_mirror_set_attached(bool attached)
{
    s_attached = attached;
}

void fake_ble_mirror_deny_writes(void)
{
    s_write_result = KODE_DENIED;
}

void fake_ble_mirror_congest_writes(void)
{
    s_write_result = KODE_NO_RESOURCES;
}

unsigned fake_ble_mirror_write_calls(void)
{
    return s_write_calls;
}

bool kdrv_ble_mirror_attached(void)
{
    return s_attached;
}

kode_status_t kdrv_ble_mirror_write(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    ++s_write_calls;
    return s_write_result;
}

/* ---- The stack ------------------------------------------------------------- */

void fake_ble_reset(void)
{
    kdrv_ble_cb_t const cb = s_b.cb;

    (void)memset(&s_b, 0, sizeof(s_b));
    s_b.cb     = cb;   /* installed once per binary, by AO_Radio's initial */
    s_b.synced = true;
    s_b.state  = KDRV_BLE_OFF;
    s_b.addr[0] = 0x38U; s_b.addr[1] = 0x44U; s_b.addr[2] = 0xBEU;
    s_b.addr[3] = 0x1CU; s_b.addr[4] = 0x1DU; s_b.addr[5] = 0x06U;
}

void fake_ble_set_sync(bool synced)
{
    s_b.synced = synced;
}

void fake_ble_fail_start(bool fail)
{
    s_b.fail_start = fail;
}

void fake_ble_fail_scan(bool fail)
{
    s_b.fail_scan = fail;
}

void fake_ble_seed_air(uint8_t n, int8_t top_rssi)
{
    s_b.air_n   = n;
    s_b.air_top = top_rssi;
}

void fake_ble_central(bool connected)
{
    s_b.state = KDRV_BLE_IDLE;
    if ( connected )
    {
        s_b.state = KDRV_BLE_CONNECTED;
        s_b.links++;
    }
    if ( NULL != s_b.cb )
    {
        s_b.cb(s_b.state);
    }
}

unsigned fake_ble_starts(void)
{
    return s_b.starts;
}

unsigned fake_ble_scans(void)
{
    return s_b.scans;
}

uint32_t fake_ble_last_scan_ms(void)
{
    return s_b.last_scan_ms;
}

kode_status_t kdrv_ble_set_cb(kdrv_ble_cb_t cb)
{
    if ( NULL == cb )
    {
        return KODE_DENIED;
    }
    s_b.cb = cb;
    return KODE_OK;
}

kode_status_t kdrv_ble_start(void)
{
    ++s_b.starts;
    if ( s_b.up )
    {
        return KODE_OK;
    }
    if ( s_b.fail_start )
    {
        return KODE_ERROR;
    }
    s_b.up    = true;
    s_b.state = KDRV_BLE_IDLE;
    return KODE_OK;
}

kode_status_t kdrv_ble_quiet(void)
{
    if ( s_b.up )
    {
        s_b.state = KDRV_BLE_IDLE;
    }
    return KODE_OK;
}

kode_status_t kdrv_ble_stop(void)
{
    s_b.up    = false;
    s_b.state = KDRV_BLE_OFF;
    return KODE_OK;
}

void kdrv_ble_adv_plan(uint16_t itvl, uint32_t ms, bool hidden, bool pulsed)
{
    s_b.adv_itvl    = itvl;
    s_b.adv_ms      = ms;
    s_b.adv_hidden  = hidden;
    s_b.adv_pulsed  = pulsed;
}

bool fake_ble_adv_hidden(void)
{
    return s_b.adv_hidden;
}

bool fake_ble_adv_pulsed(void)
{
    return s_b.adv_pulsed;
}

bool fake_ble_calm_pulsed(void)
{
    return s_b.calm_pulsed;
}

void fake_ble_set_bonds(uint8_t n)
{
    s_b.bonds = n;
}

uint16_t fake_ble_adv_itvl(void)
{
    return s_b.adv_itvl;
}

uint32_t fake_ble_adv_ms(void)
{
    return s_b.adv_ms;
}

kode_status_t kdrv_ble_adv_calm(uint32_t ms, bool pulsed)
{
    s_b.calm_pulsed = pulsed;
    if ( (KDRV_BLE_ADVERTISING == s_b.state) && (0U != s_b.adv_itvl)
        && ((uint16_t)KDRV_BLE_ITVL_SLOW > s_b.adv_itvl) )
    {
        kdrv_ble_adv_plan((uint16_t)KDRV_BLE_ITVL_SLOW, ms, s_b.adv_hidden, pulsed);
    }
    return KODE_OK;
}

kode_status_t kdrv_ble_conn_calm(bool rest)
{
    if ( rest )
    {
        s_b.conn_rest++;
    }
    else
    {
        s_b.conn_awake++;
    }
    return KODE_OK;
}

unsigned fake_ble_conn_calls(bool rest)
{
    return rest ? s_b.conn_rest : s_b.conn_awake;
}

bool kdrv_ble_link_secure(void)
{
    return s_b.secure;
}

void fake_ble_secure(bool secure)
{
    s_b.secure = secure;
}

void fake_ble_adv_end(void)
{
    s_b.state = KDRV_BLE_IDLE;
    if ( NULL != s_b.cb )
    {
        s_b.cb(s_b.state);
    }
}

kode_status_t kdrv_ble_advertise(bool on)
{
    if ( !s_b.up )
    {
        kode_status_t const rc = kdrv_ble_start();
        if ( KODE_OK != rc )
        {
            return rc;
        }
    }
    if ( KDRV_BLE_CONNECTED == s_b.state )
    {
        return KODE_DENIED;
    }
    if ( on )
    {
        s_b.state = KDRV_BLE_ADVERTISING;
    }
    else
    {
        s_b.state = KDRV_BLE_IDLE;
    }
    return KODE_OK;
}

kode_status_t kdrv_ble_scan(uint32_t ms)
{
    kode_status_t const rc = kdrv_ble_start();

    ++s_b.scans;
    s_b.last_scan_ms = ms;
    s_b.count = 0U;
    if ( KODE_OK != rc )
    {
        return rc;
    }
    if ( s_b.fail_scan )
    {
        return KODE_ERROR;
    }
    for ( uint8_t i = 0U; (i < s_b.air_n) && (KDRV_BLE_SCAN_MAX > i); ++i )
    {
        kdrv_ble_dev_t * const d = &s_b.dev[i];
        (void)memset(d, 0, sizeof(*d));
        d->addr[0] = 0xC0U;
        d->addr[5] = i;
        d->rssi    = (int8_t)(s_b.air_top - (int)(6U * i));
        (void)snprintf(d->name, sizeof(d->name), "beacon-%u", (unsigned)i);
        if ( 0U == i )
        {
            /* The first one says what a tracker says: a random address,
             * Apple's company id and one 16-bit service. The rest say
             * nothing past the name. */
            d->addr_type = (uint8_t)KDRV_BLE_ADDR_RANDOM;
            d->mfg_len   = FAKE_BLE_MFG_LEN;
            d->mfg[0]    = FAKE_BLE_MFG_ID_LO;
            d->mfg[1]    = FAKE_BLE_MFG_ID_HI;
            d->mfg[2]    = FAKE_BLE_MFG_TYPE;
            d->svc_n     = 1U;
            d->svc16[0]  = FAKE_BLE_SVC16;
        }
        ++s_b.count;
    }
    return KODE_OK;
}

uint8_t kdrv_ble_scan_count(void)
{
    return s_b.count;
}

bool kdrv_ble_scan_get(uint8_t idx, kdrv_ble_dev_t *out)
{
    if ( (idx >= s_b.count) || (NULL == out) )
    {
        return false;
    }
    *out = s_b.dev[idx];
    return true;
}

kode_status_t kdrv_ble_set_scan_rsp(const char *name, const uint8_t *data, size_t len)
{
    uint8_t     buf[KDRV_BLE_ADV_MAX];
    size_t      n   = 0U;
    const char *who = name;

    /* The real packing, so a refusal is the driver's own. */
    if ( NULL == who )
    {
        who = kdrv_ble_serial_name();
    }
    kode_status_t const rc = kdrv_ble_adv_pack(who, data, len, buf, sizeof(buf), &n);

    if ( KODE_OK == rc )
    {
        s_b.name[0] = '\0';
        if ( NULL != name )
        {
            (void)snprintf(s_b.name, sizeof(s_b.name), "%s", name);
        }
    }
    return rc;
}

const char *kdrv_ble_name(void)
{
    const char *name = s_b.name;

    if ( '\0' == s_b.name[0] )
    {
        name = kdrv_ble_serial_name();
    }
    return name;
}

kdrv_ble_state_t kdrv_ble_state(void)
{
    return s_b.state;
}

bool kdrv_ble_addr(uint8_t out[6])
{
    if ( !s_b.up || !s_b.synced || (NULL == out) )
    {
        return false;
    }
    (void)memcpy(out, s_b.addr, sizeof(s_b.addr));
    return true;
}

kode_status_t kdrv_ble_console_set_rx_cb(kdrv_ble_console_rx_t cb)
{
    (void)cb;
    return KODE_OK;
}

kode_status_t kdrv_ble_console_set_raw_cb(kdrv_ble_console_raw_t cb)
{
    (void)cb;
    return KODE_OK;
}

void kdrv_ble_console_raw(bool on)
{
    (void)on;
}

bool kdrv_ble_console_attached(void)
{
    return s_attached;
}

uint16_t kdrv_ble_console_backlog(void)
{
    return 0U;
}

uint16_t kdrv_ble_console_retry(void)
{
    return 0U;
}

uint16_t kdrv_ble_console_tx_drop(void)
{
    return 0U;
}

uint8_t kdrv_ble_bonds(void)
{
    return s_b.bonds;
}

kode_status_t kdrv_ble_unpair(void)
{
    s_b.bonds = 0U;
    return KODE_OK;
}

kode_status_t kdrv_ble_console_write(const char *buf, size_t len)
{
    (void)buf;
    (void)len;
    return KODE_OK;
}

kode_status_t kdrv_ble_console_selftest(void)
{
    return KODE_OK;
}

void kdrv_ble_forget(void)
{
    s_b.up    = false;
    s_b.state = KDRV_BLE_OFF;
    s_b.count = 0U;
}

#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
/* ---- The pairing half --------------------------------------------------------
 *
 * The record and the nudge, with the driver's own rule kept: one take per
 * note, and a second take of the same note finds nothing. A case plays the
 * host task's part with fake_ble_pair_show() and fake_ble_pair_end(), which
 * is the whole of what the real GAP handler does to this state.
 */

static struct {
    kdrv_ble_pair_t    rec;
    kdrv_ble_pair_cb_t cb;
    bool               live;
    unsigned           rejects;
} s_p;

void fake_ble_pair_reset(void)
{
    kdrv_ble_pair_cb_t const cb = s_p.cb;

    (void)memset(&s_p, 0, sizeof(s_p));
    s_p.cb        = cb;   /* installed once per binary, like the state one */
    s_p.rec.phase = KDRV_BLE_PAIR_NONE;
}

void fake_ble_pair_show(uint32_t passkey)
{
    s_p.live          = true;
    s_p.rec.phase     = KDRV_BLE_PAIR_SHOW;
    s_p.rec.passkey   = passkey;
    s_p.rec.outcome   = KDRV_BLE_PAIR_OK;
    if ( NULL != s_p.cb )
    {
        s_p.cb();
    }
}

void fake_ble_pair_end(kdrv_ble_pair_outcome_t how)
{
    s_p.live        = false;
    s_p.rec.phase   = KDRV_BLE_PAIR_DONE;
    s_p.rec.passkey = 0U;
    s_p.rec.outcome = how;
    if ( NULL != s_p.cb )
    {
        s_p.cb();
    }
}

unsigned fake_ble_pair_rejects(void)
{
    return s_p.rejects;
}

kode_status_t kdrv_ble_set_pair_cb(kdrv_ble_pair_cb_t cb)
{
    if ( NULL == cb )
    {
        return KODE_DENIED;
    }
    s_p.cb = cb;
    return KODE_OK;
}

bool kdrv_ble_pair_take(kdrv_ble_pair_t *out)
{
    if ( (NULL == out) || (KDRV_BLE_PAIR_NONE == s_p.rec.phase) )
    {
        return false;
    }
    *out            = s_p.rec;
    s_p.rec.phase   = KDRV_BLE_PAIR_NONE;
    return true;
}

kode_status_t kdrv_ble_pair_reject(void)
{
    if ( !s_p.live )
    {
        return KODE_DENIED;
    }
    ++s_p.rejects;
    /* The real driver clears the flag, records REJECTED and then hangs up,
     * and the hang-up's own disconnect must NOT report GONE on top of it.
     * fake_ble_pair_end() reproduces the first two; nothing here fabricates
     * the third, which is what a case is for. */
    fake_ble_pair_end(KDRV_BLE_PAIR_REJECTED);
    return KODE_OK;
}
#endif /* CONFIG_KODEOS_BLE_PAIRING_PASSKEY */
