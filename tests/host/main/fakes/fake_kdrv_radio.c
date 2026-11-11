/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_kdrv_radio.c
 * @brief The C5 link's driver without the link: jobs are recorded and a case sets their outcome.
 */
#include <string.h>

#include "fakes.h"
#include "kapp_desc.h"
#include "kdrv_radio.h"

static struct {
    kdrv_radio_lost_cb_t lost_cb;
    kdrv_radio_req_t   ran;         /**< The job whose body ran last.      */
    unsigned           runs;        /**< Jobs whose body ran since reset.  */
    bool               link_up;
    bool               c5_held;     /**< kdrv_radio_c5_hold() holds the C5. */
    unsigned           c5_holds;    /**< Calls that held it since reset.   */
    bool               connected;   /**< A CONNECT ran: the pin is
                                     *   esp-hosted's from then on.         */
    bool               fw_known;
    kdrv_radio_fwver_t fw;
    uint32_t           note;        /**< The last target left; 0 = none.   */
    bool               adopted;     /**< What kdrv_radio_adopted() answers. */
    bool               exp_fails;   /**< A CONNECT does not switch J3's supply. */
    bool               exp_switched; /**< What kdrv_radio_exp_switched() answers. */
    bool               exp_pin;     /**< The level J3's supply enable holds.    */
    unsigned           exp_offs;    /**< Times the pin was driven low.          */
    bool               read_done;   /**< The CONNECT worker has read the wish.  */
    bool               read_on;     /**< What it read.                          */
    bool               exp_level;   /**< What kdrv_radio_exp_level() answers.   */
    kdrv_radio_probe_result_t probe;  /**< What kdrv_radio_probe_result() answers. */
#if CONFIG_KODE_LOWPOWER
    kdrv_radio_sleep_note_cb_t sleep_note; /**< What the last start was given. */
    void              *sleep_arg;   /**< ...and its argument.               */
    unsigned           sleep_starts;/**< Starts asked for since the reset.  */
    unsigned           sleep_stops; /**< Stops asked for since the reset.   */
    bool               sleep_armed; /**< The last call was a start.         */
    kdrv_radio_sleep_stats_t stats; /**< What the sentinel "did".           */
    kode_status_t      sleep_rc;    /**< What the next start answers.       */
#endif
} s_r;

/** The wish AO_Radio last set: kept across fake_radio_reset(), as the AO's own is. */
static bool s_exp_wish;

void kdrv_radio_exp_wish(bool on)
{
    s_exp_wish = on;
}

bool kdrv_radio_exp_level(void)
{
    return s_r.exp_level;
}

void fake_radio_reset(void)
{
    kdrv_radio_lost_cb_t const cb = s_r.lost_cb;

    (void)memset(&s_r, 0, sizeof(s_r));
    /* The notification is installed once per binary by AO_Radio's initial
     * transition, and a reset between cases must not forget that: the AO
     * never installs it again. */
    s_r.lost_cb = cb;
}

void fake_radio_power_cycle(void)
{
    /* Everything, the notification included: the next initial transition
     * installs it again, and the reset line is nobody's until then. */
    (void)memset(&s_r, 0, sizeof(s_r));
}

const kdrv_radio_req_t *fake_radio_ran(void)
{
    return &s_r.ran;
}

unsigned fake_radio_runs(void)
{
    return s_r.runs;
}

void fake_radio_set_link(bool up)
{
    s_r.link_up = up;
}

#if CONFIG_KODE_LOWPOWER
unsigned fake_radio_sleep_starts(void)
{
    return s_r.sleep_starts;
}

unsigned fake_radio_sleep_stops(void)
{
    return s_r.sleep_stops;
}

bool fake_radio_sleep_armed(void)
{
    return s_r.sleep_armed;
}

void fake_radio_sleep_note(uint8_t kind, uint16_t knocks)
{
    kdrv_radio_sleep_note_t n;

    (void)memset(&n, 0, sizeof(n));
    n.kind   = kind;
    n.knocks = knocks;
    n.tx_sta = 3U;
    if ( KDRV_RADIO_WOKE_C5 == kind )
    {
        s_r.stats.wakes_c5 += 1U;
    }
    else if ( KDRV_RADIO_WOKE_TX == kind )
    {
        s_r.stats.wakes_tx += 1U;
    }
    else if ( KDRV_RADIO_WOKE_DEAF == kind )
    {
        s_r.stats.deaf += 1U;
    }
    else
    {
        s_r.stats.stood_down += 1U;
    }
    if ( NULL != s_r.sleep_note )
    {
        s_r.sleep_note(&n, s_r.sleep_arg);
    }
}
#endif

void fake_radio_set_c5_held(bool held)
{
    s_r.c5_held = held;
}

uint32_t fake_radio_note_last(void)
{
    return s_r.note;
}

void fake_radio_set_adopted(bool adopted)
{
    s_r.adopted = adopted;
}

void fake_radio_fail_exp_switch(bool fails)
{
    s_r.exp_fails = fails;
}

void fake_radio_set_fw(uint32_t major, uint32_t minor, uint32_t patch,
                       bool known)
{
    s_r.fw.major  = major;
    s_r.fw.minor  = minor;
    s_r.fw.patch  = patch;
    s_r.fw_known  = known;
}

/** @brief The C5 drives J3's supply enable to @p on. */
static void exp_drive(bool on)
{
    s_r.exp_pin = on;
    if ( !on )
    {
        s_r.exp_offs += 1U;
    }
}

bool fake_radio_exp_pin(void)
{
    return s_r.exp_pin;
}

unsigned fake_radio_exp_off_drives(void)
{
    return s_r.exp_offs;
}

void fake_radio_connect_read(void)
{
    s_r.read_done = true;
    s_r.read_on   = s_exp_wish;
}

void fake_radio_effects(kdrv_radio_job_t job, kode_status_t rc)
{
    /* What the real driver does on the way out of the two link jobs, so a
     * case that connects and then asks kdrv_radio_link_up() sees the truth
     * without arranging it by hand. */
    if ( (KDRV_RADIO_JOB_CONNECT == job) && (KODE_OK == rc) )
    {
        s_r.link_up      = true;
        s_r.exp_switched = !s_r.exp_fails;
        if ( s_r.read_done )
        {
            s_r.exp_level = s_r.read_on;
        }
        else
        {
            s_r.exp_level = s_exp_wish;
        }
        if ( !s_r.exp_fails )
        {
            exp_drive(s_r.exp_level);
        }
    }
    if ( (KDRV_RADIO_JOB_EXP_POWER == job) && (KODE_OK == rc) )
    {
        exp_drive(s_r.ran.on);
    }
    if ( (KDRV_RADIO_JOB_CONNECT == job) && (KODE_OK != rc) )
    {
        /* Every failure path lets go of esp-hosted, the reset line with it. */
        s_r.connected = false;
    }
    if ( KDRV_RADIO_JOB_DISCONNECT == job )
    {
        s_r.link_up   = false;
        s_r.fw_known  = false;
        /* esp-hosted has let go of the reset line with its deinit. */
        s_r.connected = false;
    }
}

void fake_radio_lose_link(kode_status_t why)
{
    s_r.link_up = false;
    if ( NULL != s_r.lost_cb )
    {
        s_r.lost_cb(why);
    }
}

void fake_radio_drop_link(void)
{
    fake_radio_lose_link(KODE_ERROR);
}

/* ---- The driver's header --------------------------------------------------- */

#if CONFIG_KODE_LOWPOWER
kode_status_t kdrv_radio_sleep(bool sleep, kdrv_radio_sleep_note_cb_t note, void *arg)
{
    /* Recorded as a job, so the harness sees what the worker ran. */
    (void)memset(&s_r.ran, 0, sizeof(s_r.ran));
    s_r.ran.type = KDRV_RADIO_JOB_SLEEP;
    s_r.ran.on   = sleep;
    ++s_r.runs;
    s_r.sleep_armed = sleep;
    if ( sleep )
    {
        ++s_r.sleep_starts;
        s_r.sleep_note = note;
        s_r.sleep_arg  = arg;
        return s_r.sleep_rc;
    }
    ++s_r.sleep_stops;
    return KODE_OK;
}

void fake_radio_sleep_fail(kode_status_t rc)
{
    s_r.sleep_rc = rc;
}

void fake_radio_set_sleep_stats(const kdrv_radio_sleep_stats_t *stats)
{
    if ( NULL != stats )
    {
        s_r.stats = *stats;
    }
    else
    {
        (void)memset(&s_r.stats, 0, sizeof(s_r.stats));
    }
}

void kdrv_radio_sleep_stats(kdrv_radio_sleep_stats_t *out)
{
    if ( NULL != out )
    {
        *out = s_r.stats;
    }
}
#endif

/* Unlike the real link, no once-per-boot limit: one binary may bring the link up many times. */
kode_status_t kdrv_radio_run(const kdrv_radio_req_t *req)
{
    if ( NULL == req )
    {
        return KODE_DENIED;
    }
    s_r.ran = *req;
    ++s_r.runs;
    if ( KDRV_RADIO_JOB_CONNECT == req->type )
    {
        /* What the real driver does first: esp-hosted takes the reset line
         * and pulses it, which is what lets a held C5 go. */
        s_r.connected    = true;
        s_r.c5_held      = false;
        s_r.exp_switched = false;
        s_r.read_done    = false;
        s_r.exp_pin      = true;   /* the reset lets go of the enable */
    }
    return KODE_OK;
}

void fake_radio_set_probe_result(const kdrv_radio_probe_result_t *res)
{
    s_r.probe = *res;
}

void kdrv_radio_probe_result(kdrv_radio_probe_result_t *out)
{
    if ( NULL != out )
    {
        *out = s_r.probe;
    }
}

void kdrv_radio_on_link_lost(kdrv_radio_lost_cb_t cb)
{
    s_r.lost_cb = cb;
}

bool kdrv_radio_link_up(void)
{
    return s_r.link_up;
}

kode_status_t kdrv_radio_c5_hold(bool hold)
{
    if ( s_r.connected )
    {
        return KODE_DENIED;
    }
    s_r.c5_held = hold;
    if ( hold )
    {
        ++s_r.c5_holds;
    }
    return KODE_OK;
}

unsigned fake_radio_c5_holds(void)
{
    return s_r.c5_holds;
}

bool kdrv_radio_c5_held(void)
{
    return s_r.c5_held;
}

void kdrv_radio_gate(bool closed)
{
    (void)closed;
}

void kdrv_radio_note_leave(kdrv_radio_note_t target)
{
    s_r.note = (uint32_t)target;
}

bool kdrv_radio_adopted(void)
{
    return s_r.adopted;
}

bool kdrv_radio_exp_switched(void)
{
    return s_r.exp_switched;
}

kode_status_t kdrv_radio_fwversion(kdrv_radio_fwver_t *ver)
{
    if ( !s_r.fw_known || (NULL == ver) )
    {
        return KODE_DENIED;
    }
    *ver = s_r.fw;
    return KODE_OK;
}

bool kdrv_radio_image_header_ok(const uint8_t *head, uint32_t len)
{
    /* The real one, minus the log line: the same pure reader, and the same
     * chip id (ESP_CHIP_ID_ESP32C5, 0x0017, spelled out because the IDF
     * header that names it is not in this binary). */
    uint16_t chip = 0U;

    return (kapp_image_chip(head, (size_t)len, &chip) == KODE_OK)
           && (0x0017U == chip);
}

/* ---- Peer data ------------------------------------------------------------
 * The listeners live outside the reset on purpose: on the board a listener
 * registers once per boot and stays, and kdrv_c5svc_init() is idempotent the
 * same way. What a case resets is the wire: the last message sent. */

#define FAKE_PEER_SLOTS  3U
#define FAKE_PEER_MAX    2056U

static struct {
    uint32_t             msg_id;
    kdrv_radio_peer_cb_t cb;
    void                *ctx;
} s_listen[FAKE_PEER_SLOTS];

static uint8_t  s_sent[FAKE_PEER_MAX];
static size_t   s_sent_len;
static uint32_t s_sent_id;

/* The headers of the last few messages, newest first: a link that comes up
 * sends more than one, and a case may want an earlier one. */
#define FAKE_PEER_HIST  4U
static uint8_t  s_hist[FAKE_PEER_HIST][16];
static unsigned s_hist_n;

void fake_peer_reset(void)
{
    s_sent_len = 0U;
    s_sent_id  = 0U;
    s_hist_n   = 0U;
    (void)memset(s_sent, 0, sizeof(s_sent));
    (void)memset(s_hist, 0, sizeof(s_hist));
}

const uint8_t *fake_peer_sent_back(unsigned back)
{
    if ( back >= s_hist_n )
    {
        return NULL;
    }
    return s_hist[back];
}

const uint8_t *fake_peer_last_sent(size_t *len)
{
    if ( NULL != len )
    {
        *len = s_sent_len;
    }
    return s_sent;
}

uint32_t fake_peer_last_msg_id(void)
{
    return s_sent_id;
}

void fake_peer_deliver(uint32_t msg_id, const uint8_t *data, size_t len)
{
    for ( size_t i = 0U; FAKE_PEER_SLOTS > i; i++ )
    {
        if ( (NULL != s_listen[i].cb) && (s_listen[i].msg_id == msg_id) )
        {
            s_listen[i].cb(data, len, s_listen[i].ctx);
        }
    }
}

kode_status_t kdrv_radio_peer_listen(uint32_t msg_id, kdrv_radio_peer_cb_t cb,
                                     kdrv_radio_peer_up_t up, void *ctx)
{
    (void)up;
    if ( NULL == cb )
    {
        return KODE_DENIED;
    }
    for ( size_t i = 0U; FAKE_PEER_SLOTS > i; i++ )
    {
        if ( (NULL == s_listen[i].cb) || (s_listen[i].msg_id == msg_id) )
        {
            s_listen[i].msg_id = msg_id;
            s_listen[i].cb     = cb;
            s_listen[i].ctx    = ctx;
            return KODE_OK;
        }
    }
    return KODE_NO_RESOURCES;
}

kode_status_t kdrv_radio_peer_send(uint32_t msg_id, const uint8_t *data,
                                   size_t len)
{
    if ( (NULL == data) || (0U == len) || (FAKE_PEER_MAX < len)
        || (!s_r.link_up) )
    {
        return KODE_DENIED;
    }
    (void)memcpy(s_sent, data, len);
    s_sent_len = len;
    s_sent_id  = msg_id;
    for ( unsigned i = FAKE_PEER_HIST - 1U; 0U < i; i-- )
    {
        (void)memcpy(s_hist[i], s_hist[i - 1U], sizeof(s_hist[i]));
    }
    (void)memset(s_hist[0], 0, sizeof(s_hist[0]));
    size_t keep = sizeof(s_hist[0]);
    if ( len < keep )
    {
        keep = len;
    }
    (void)memcpy(s_hist[0], data, keep);
    if ( FAKE_PEER_HIST > s_hist_n )
    {
        s_hist_n++;
    }
    return KODE_OK;
}
