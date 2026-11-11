/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file autotest_ao.c
 * @brief AO_Autotest: the Active Object that holds the self-test rows and walks them.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "kaos.h"
#include "kaos_start.h"
#include "kapp_cat.h"
#include "kbsp_identity.h"
#include "kdrv_apps.h"
#include "kdrv_clocks.h"    /* the crystals' row */
#include "kdrv_kfw.h"
#include "kdrv_storage.h"
#include "khal_identity.h"
#include "khal_nvs.h"
#include "khal_system.h"
#include "khal_time.h"
#include "kode_log.h"
#include "kqp.h"

#include "autotest_psram.h"
#include "krows.h"
#include "kaos_claim.h"

#define TAG "kaos.autotest"

static uint8_t power_depth(void);

/* The chips are AO_Power's: it runs the test and answers TEST_RESULT_SIG. On the
 * production line the test proves the chip works with a battery and a USB adapter
 * attached; the assembled Kode's final test asks only that it answers and measures
 * (power_depth()). */
/**
 * @brief The PMIC row: ask AO_Power to test the PMIC at this client's depth.
 *
 * @return false: AO_Power answers TEST_RESULT_SIG.
 */
static bool row_pmic(void)
{
    power_test_request(POWER_CHIP_PMIC, power_depth());
    return false;
}

/**
 * @brief The gauge row: ask AO_Power to test the fuel gauge at this client's depth.
 *
 * @return false: AO_Power answers TEST_RESULT_SIG.
 */
static bool row_gauge(void)
{
    power_test_request(POWER_CHIP_GAUGE, power_depth());
    return false;
}

/**
 * @brief The expander row: ask AO_IoExp to run its test.
 *
 * The expander is AO_IoExp's, and its test is the third hardware row: the
 * chip answers, its inputs face in, and reading them releases INT.
 *
 * @return false: AO_IoExp answers TEST_RESULT_SIG.
 */
static bool test_ioexp(void)
{
    ioexp_test_request();
    return false;
}

/**
 * @brief The buttons row: ask AO_Buttons to run its guided test.
 *
 * The operator's row: AO_Buttons asks for one button at a time and judges
 * that exactly that one was pressed; the steps arrive as BTN_TEST_SIG.
 *
 * @return false: AO_Buttons answers TEST_RESULT_SIG.
 */
static bool test_buttons(void)
{
    buttons_test_request();
    return false;
}

/** @brief The crystals' row, judged here and now: no AO owns the clock block. It
 *         proves this unit has a 32 kHz crystal and is running on it. */
static bool test_clocks(void)
{
    return kdrv_clocks_test();
}

/* The PSRAM row is this AO's own: nobody else owns the PSRAM, and the work
 * is paced by this AO's timer. It starts here and answers itself. */
static void psram_begin(void);

/** @brief The PSRAM row: four passes over the free PSRAM, a step at a time.
 *  @return false: the row answers itself when its last step is done. */
static bool test_psram(void)
{
    psram_begin();
    return false;
}

/** @brief The internal pool's row, judged here and now: no AO owns the pool. It
 *         judges the lowest the pool has been since boot and whether the heap ever
 *         refused a request, so it is the last row of `all`. */
static bool test_mem(void)
{
    size_t   const low     = khal_heap_internal_low();
    uint32_t const refused = khal_heap_alloc_failures();
    bool           ok      = true;

    if ( 0U != refused )
    {
        KLOG_E(TAG, "test mem: FAIL - the heap refused %lu allocation(s) since boot",
               (unsigned long)refused);
        ok = false;
    }
    if ( (size_t)KODE_MEM_TEST_FLOOR_BYTES > low )
    {
        KLOG_E(TAG, "test mem: FAIL - the internal pool went down to %u B, "
               "under its floor of %u B",
               (unsigned)low, (unsigned)KODE_MEM_TEST_FLOOR_BYTES);
        ok = false;
    }
    if ( ok )
    {
        KLOG_N(TAG, "test mem: the internal pool never went under %u B "
               "(floor %u B), and the heap refused nothing",
               (unsigned)low, (unsigned)KODE_MEM_TEST_FLOOR_BYTES);
    }
    return ok;
}

/**
 * @brief The boot row: kodeOS runs from a partition ESP-IDF names, and otadata agrees.
 *
 * Judged here and now, and it boots nothing. It also asks AO_Boot to
 * log the boot state (boot_info_request()).
 *
 * @return true when both hold.
 */
static bool test_boot(void)
{
    /* Non-destructive: kodeOS runs from a partition ESP-IDF can name, and
     * otadata is blank (factory) or points at the OTA slot we run from (a
     * confirmed kodeOS update). */
    boot_info_request();
    uint32_t address = 0U;
    uint32_t size    = 0U;
    kdrv_running_info(NULL, &address, &size);
    bool const running_ok = (0U != address) && (0U != size);
    bool const ota_ok     = kdrv_otadata_blank() || !kdrv_running_is_factory();
    return running_ok && ota_ok;
}

/**
 * @brief The live override table, against the real NVS.
 *
 * It writes flash, two small commits, to prove what the console verb rests on:
 * a set survives a read, and a clear really removes.
 */
static bool test_apps_catlive(void)
{
    char cats[KAPP_CATEGORY_MAX];
    bool ok = true;

    /* A name no real app will carry, so a failed run cannot leave a row
     * that shadows something the user installed. */
    static const char *const who = "__kodeos_selftest";

    ok &= (kapp_cat_set(who, "Game, demo") == KODE_OK);
    ok &= kapp_cat_resolve(who, cats, sizeof(cats))
          && (strcmp(cats, "game,demo") == 0);

    /* Setting again replaces rather than appending. */
    ok &= (kapp_cat_set(who, "tool") == KODE_OK);
    ok &= kapp_cat_resolve(who, cats, sizeof(cats))
          && (strcmp(cats, "tool") == 0);

    /* A name with no row resolves to nothing and says so. */
    ok &= !kapp_cat_resolve("__no_such_app", cats, sizeof(cats));

    /* Clearing removes the row; resolve stops answering for it. */
    ok &= (kapp_cat_clear(who) == KODE_OK);
    ok &= !kapp_cat_resolve(who, cats, sizeof(cats));

    /* Clearing what is not there is success: the postcondition holds. */
    ok &= (kapp_cat_clear(who) == KODE_OK);

    /* A list that normalises to nothing is refused, not stored empty. */
    ok &= (kapp_cat_set(who, " , , ") == KODE_ERROR);
    ok &= !kapp_cat_resolve(who, cats, sizeof(cats));

    return ok;
}

/**
 * @brief What `test apps` proves that a host cannot.
 *
 * The only check that drives the real store on real flash: the host suites
 * reach kapp_cat through a faked khal_nvs.
 */
static bool test_apps(void)
{
    return test_apps_catlive();
}

/**
 * @brief The one half of the container format a host cannot check.
 *
 * A real deflate stream inflated back to its image, which needs the P4's ROM
 * tinfl_decompress. The body is in kdrv_kfw.c: esp_rom_crc.h stops at kdrv.
 */
static bool test_kfw(void)
{
    return (kfw_selftest() == KODE_OK);
}

/** @brief One encoder vector: a MAC, and the string it has to produce. */
typedef struct {
    uint8_t     mac[6];
    const char *want;
} id_vec_t;

/**
 * @brief test id: the encoder against fixed vectors, then this board.
 *
 * The vectors mean the same thing on every unit. `00:00:00:00:00:00` is left
 * out: an unburned eFuse reads like that, and khal_identity_init() refuses it.
 */
static bool test_id(void)
{
    static const id_vec_t VECS[] = {
        { { 0xE8U, 0xF6U, 0x0AU, 0xE7U, 0x6CU, 0x1DU }, "KODE78YR5EEV0XS" },
        { { 0xE8U, 0xF6U, 0x0AU, 0xE7U, 0x6CU, 0x1EU }, "KODE78YR5EEV0YQ" },
        { { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U }, "KODE0000000001Y" },
        { { 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU }, "KODE7ZZZZZZZZZ2" },
    };
    bool ok = true;
    char got[KHAL_ID_LEN];

    for ( size_t i = 0U; (sizeof(VECS) / sizeof(VECS[0])) > i; ++i )
    {
        if ( khal_identity_encode(VECS[i].mac, got, sizeof(got)) != KODE_OK )
        {
            KLOG_E(TAG, "test id: vector %u refused", (unsigned)i);
            ok = false;
        }
        else if ( strcmp(got, VECS[i].want) != 0 )
        {
            KLOG_E(TAG, "test id: vector %u gave %s, wanted %s",
                   (unsigned)i, got, VECS[i].want);
            ok = false;
        }
        else
        {
            /* that vector passed */
        }
    }

    /* This board's own name: present, the right length, and starting inside
     * 0-7 because 48 bits left-padded to 50 leave the first symbol three. */
    const char *const self = khal_identity_serial();
    size_t const want_len = KHAL_ID_LEN - 1U;
    if ( strlen(self) != want_len )
    {
        KLOG_E(TAG, "test id: '%s' is not %u characters",
               self, (unsigned)want_len);
        ok = false;
    }
    else if ( ('0' > self[sizeof(KBSP_ID_PREFIX) - 1U])
             || ('7' < self[sizeof(KBSP_ID_PREFIX) - 1U]) )
    {
        KLOG_E(TAG, "test id: '%s' starts outside 0-7", self);
        ok = false;
    }
    else
    {
        KLOG_N(TAG, "test id: this board is %s", khal_identity_pretty());
    }

    return ok;
}

/** @brief The charger plugged in, the two power chips judged with it there,
 *         and the charger taken out again. */
static bool test_power_plug(void)
{
    power_plug_test_request();
    return false;
}


void kaos_autotest_register(void)
{
#if CONFIG_KODEOS_AUTOTEST
    static const krow_t AUTOTEST_ROWS[] = {
        { "pmic", AUTOTEST_ID_PMIC, row_pmic, KROW_ASYNC, AUTOTEST_G_MAIN, KODE_TEST_ROW_BUDGET_MS,
          NULL, 0U, "Charger",
          "Charger not right",
          AUTOTEST_ORDER_PMIC },
        { "gauge", AUTOTEST_ID_GAUGE, row_gauge, KROW_ASYNC, AUTOTEST_G_MAIN, KODE_GAUGE_TEST_BUDGET_MS,
          NULL, 0U, "Battery gauge",
          "Gauge not right",
          AUTOTEST_ORDER_GAUGE },
        { "ioexp", AUTOTEST_ID_IOEXP, test_ioexp, KROW_ASYNC, AUTOTEST_G_MAIN, KODE_TEST_ROW_BUDGET_MS,
          NULL, 0U, "Button lines",
          "Lines not answering",
          AUTOTEST_ORDER_IOEXP },
        { "clocks", AUTOTEST_ID_CLOCKS, test_clocks, KROW_SYNC, AUTOTEST_G_MAIN, 0U,
          NULL, 0U, "Clock",
          "Crystal not running",
          AUTOTEST_ORDER_CLOCKS },
        { "buttons", AUTOTEST_ID_BUTTONS, test_buttons, KROW_ASYNC, AUTOTEST_G_NONE, KODE_BUTTONS_TEST_BUDGET_MS,
          NULL, 0U, "Buttons",
          "Button not working",
          AUTOTEST_ORDER_BUTTONS },
        { "psram", AUTOTEST_ID_PSRAM, test_psram, KROW_ASYNC, AUTOTEST_G_NONE,
          KODE_PSRAM_TEST_BUDGET_MS, NULL, 0U, "PSRAM",
          "Memory fault",
          AUTOTEST_ORDER_PSRAM },
        { "mem", AUTOTEST_ID_MEM, test_mem, KROW_SYNC, AUTOTEST_G_NONE, 0U,
          NULL, 0U, "Memory",
          "Ran short of memory",
          AUTOTEST_ORDER_MEM },
        { "boot", AUTOTEST_ID_BOOT, test_boot, KROW_SYNC, AUTOTEST_G_NONE, 0U,
          NULL, 0U, "Start-up record",
          "Record not right",
          AUTOTEST_ORDER_BOOT },
        { "apps", AUTOTEST_ID_APPS, test_apps, KROW_SYNC, AUTOTEST_G_NONE, 0U,
          NULL, 0U, "App settings",
          "Settings not kept",
          AUTOTEST_ORDER_APPS },
        { "kfw", AUTOTEST_ID_KFW, test_kfw, KROW_SYNC, AUTOTEST_G_NONE, 0U,
          NULL, 0U, "Update unpacker",
          "Unpacker not working",
          AUTOTEST_ORDER_KFW },
        { "id", AUTOTEST_ID_ID, test_id, KROW_SYNC, AUTOTEST_G_NONE, 0U,
          NULL, 0U, "Serial number",
          "Serial not right",
          AUTOTEST_ORDER_ID },
        { "power_plug", AUTOTEST_ID_POWER_PLUG, test_power_plug, KROW_ASYNC,
          AUTOTEST_G_NONE, KODE_POWER_PLUG_BUDGET_MS,
          "power", 0U, "Charger & battery",
          "Charger problem",
          AUTOTEST_ORDER_POWER_PLUG },
    };

    kaos_require(krows_add(AUTOTEST_ROWS, sizeof(AUTOTEST_ROWS) / sizeof(AUTOTEST_ROWS[0])),
                 "autotest rows");
#endif
}

/* ---- The `user` profile ---------------------------------------------------
 * An order of its own: first what needs nobody, then a hold, then what needs
 * hands. The table's orders hold too: card before coprocessor, infrared right
 * behind it, audio after it, buttons before the pad's rows, the pool last.
 */

/** @brief Not a row: the run waits here for whoever asked for it. */
#define USER_HOLD 0xFEU
_Static_assert(KROWS_ROWS_MAX < USER_HOLD, "a row's index is never the hold");

/* ---- The kept report -------------------------------------------------------
 * One record in NVS, written when a `user` run ends uncancelled and replaced
 * by the next: when it ran, and how each row ended. NVS rather than a file
 * because the internal volume is the owner's and ships empty.
 */
#define REPORT_NS       "ktest"
#define REPORT_KEY      "last"
#define REPORT_VERSION  1U

/** @brief The record as it is stored. */
typedef struct {
    uint8_t  version;                    /**< REPORT_VERSION.                */
    uint8_t  rows;                       /**< Entries used below.            */
    uint8_t  id[AUTOTEST_RUN_MAX];       /**< AUTOTEST_ID_*.                 */
    uint8_t  state[AUTOTEST_RUN_MAX];    /**< AUTOTEST_ROW_*.                */
    uint32_t when;                       /**< UTC seconds; 0: clock not set. */
} report_t;

/** @brief The kept report as it is read back: one copy, used from this AO's
 *         thread alone, by one request at a time. */
static report_t s_kept;

/**
 * @brief Read the kept report into s_kept.
 *
 * @return true when a report of this format is kept.
 */
static bool report_load(void)
{
    size_t len = sizeof(s_kept);

    return (khal_nvs_get_blob(REPORT_NS, REPORT_KEY, &s_kept, &len) == KODE_OK)
           && (sizeof(s_kept) == len) && (REPORT_VERSION == s_kept.version)
           && (AUTOTEST_RUN_MAX >= s_kept.rows);
}

/**
 * @brief A row run again by name: its new verdict replaces its old one in
 *        the kept report, so the page that asked for it shows it.
 *
 * Only the state of a row the report already holds; the run's time stays. A
 * row the report does not hold changes nothing.
 *
 * @param id    AUTOTEST_ID_*.
 * @param state How it ended this time.
 */
static void report_patch(uint8_t id, uint8_t state)
{
    bool found = false;

    if ( report_load() )
    {
        for ( uint8_t i = 0U; i < s_kept.rows; ++i )
        {
            if ( s_kept.id[i] == id )
            {
                s_kept.state[i] = state;
                found = true;
            }
        }
    }
    if ( found && (khal_nvs_set_blob(REPORT_NS, REPORT_KEY, &s_kept,
                                    sizeof(s_kept)) != KODE_OK) )
    {
        KLOG_W(TAG, "the report could not take this row's new verdict");
    }
}

/* ---- The Active Object --------------------------------------------------- */

/** @brief How long a hold waits for its client before the run is given up. */
#define HOLD_MS 300000U

/** @brief QP ticks between two steps of the PSRAM row. A step is about 3.5 ms
 *         on the PSRAM bus the GUI renders from; three ticks keep Kody's
 *         longest pass near 270 ms, for a row of about 3 s. */
#define PSRAM_STEP_TICKS  3U

/** @brief Bytes in a KB, for the PSRAM row's record. */
#define PSRAM_KB  1024U

/** @brief 32-bit words in one block, and in one step. */
#define PSRAM_BLOCK_WORDS  (KODE_PSRAM_TEST_BLOCK_BYTES / sizeof(uint32_t))
#define PSRAM_STEP_WORDS   (KODE_PSRAM_TEST_STEP_BYTES / sizeof(uint32_t))

/** @brief AO_Autotest. */
typedef struct {
    QActive  super;
    QTimeEvt budget;        /**< The deadline of the row in flight, or of a
                             *   hold.                                        */
    bool     budget_live;   /**< @c budget is armed and has not expired.     */
    uint8_t  budget_stale;  /**< Expired AUTOTEST_BUDGET_SIG still queued.   */
    bool     running;       /**< A run is in flight.                         */
    bool     holding;       /**< It is waiting for its client.               */
    bool     cancel;        /**< It ends after the row in flight.            */
    bool     user;          /**< It is the `user` profile: its report is
                             *   kept.                                        */
    bool     single;        /**< It is one row asked for by name: its verdict
                             *   replaces that row's in the kept report.      */
    uint8_t  client;        /**< AUTOTEST_CLIENT_*: who asked for it.        */
    uint8_t  order[AUTOTEST_RUN_MAX]; /**< krows_at() indices in run order,
                                       *   USER_HOLD where the run waits.    */
    uint8_t  n;             /**< Entries used in @c order.                   */
    uint8_t  next;          /**< The entry the walk takes next.              */
    const krow_t *row;      /**< The row waiting for its verdict, or NULL.   */
    uint8_t  pos;           /**< The place of that row in the run, from 1.   */
    bool     step_fail;     /**< A step of the row in flight was answered,
                             *   and answered no.                             */
    bool     step_timeout;  /**< A step of it was never answered.            */
    bool     step_absent;   /**< The card's first step found no card.        */
    uint8_t  tally[AUTOTEST_ROW_STATES]; /**< Rows ended so far, by state.   */
    report_t report;        /**< This run's rows, kept when it ends.         */
    QTimeEvt psram_step;    /**< The PSRAM row's next step.                  */
    uint32_t *psram_block[KODE_PSRAM_TEST_BLOCKS_MAX]; /**< Its blocks.      */
    uint8_t  psram_blocks;  /**< How many it holds.                          */
    bool     psram_filling; /**< It is still taking blocks.                  */
    bool     psram_live;    /**< A PSRAM walk is in flight.                  */
    uint8_t  psram_pass;    /**< PSRAM_PASS_* in progress.                   */
    uint8_t  psram_bad_pass;/**< The pass that found the first fault.        */
    uint8_t  psram_at;      /**< The block that pass is in.                  */
    size_t   psram_word;    /**< Words into that block.                      */
    size_t   psram_free;    /**< PSRAM free when the row started, in bytes:
                             *   the record's, not the reserve's.            */
    psram_fault_t psram_fault; /**< What the check passes found.             */
#if CONFIG_KODE_LOWPOWER
    uint8_t  lp_who;        /**< The id lowpower_join() gave.                 */
#endif
} Autotest;

static Autotest l_autotest;
QActive * const AO_Autotest = &l_autotest.super;

/**
 * @brief How much the power rows judge in the run in flight.
 *
 * The E5 final test runs on an assembled Kode with its cell at any charge: it
 * asks only that the chips answer and measure. Every other client gets the
 * whole row.
 *
 * @return POWER_DEPTH_ANSWER for AO_TestDot's run, else POWER_DEPTH_FULL.
 */
static uint8_t power_depth(void)
{
    uint8_t depth = POWER_DEPTH_FULL;

    if ( AUTOTEST_CLIENT_TEST_DOT == l_autotest.client )
    {
        depth = POWER_DEPTH_ANSWER;
    }
    return depth;
}

static QState Autotest_initial(Autotest * const me, void const * const par);
static QState Autotest_active(Autotest * const me, QEvt const * const e);

void autotest_run_request(uint8_t client, uint8_t group, const char *name)
{
    AutotestRunReqEvt *e = Q_NEW_X(AutotestRunReqEvt, 0U, AUTOTEST_RUN_REQ_SIG);

    if ( NULL != e )
    {
        e->client  = client;
        e->group   = group;
        e->name[0] = '\0';
        if ( NULL != name )
        {
            (void)snprintf(e->name, sizeof(e->name), "%s", name);
        }
        QACTIVE_POST(AO_Autotest, &e->super, NULL);
    }
}

/**
 * @brief Post a request that carries only its sender.
 *
 * @param sig    AUTOTEST_CANCEL_REQ_SIG, AUTOTEST_CONTINUE_SIG or
 *               AUTOTEST_REPORT_REQ_SIG.
 * @param client AUTOTEST_CLIENT_*.
 */
static void client_request(enum_t sig, uint8_t client)
{
    AutotestClientEvt *e = Q_NEW_X(AutotestClientEvt, 0U, sig);

    if ( NULL != e )
    {
        e->client = client;
        QACTIVE_POST(AO_Autotest, &e->super, NULL);
    }
}

void autotest_cancel_request(uint8_t client)
{
    client_request((enum_t)AUTOTEST_CANCEL_REQ_SIG, client);
}

void autotest_continue(uint8_t client)
{
    client_request((enum_t)AUTOTEST_CONTINUE_SIG, client);
}

void autotest_report_request(uint8_t client)
{
    client_request((enum_t)AUTOTEST_REPORT_REQ_SIG, client);
}

const char *autotest_row_label(uint8_t id)
{
    const krow_t * const t = krows_by_id(id);
    const char *label = "";

    if ( NULL != t )
    {
        label = t->label;
    }
    return label;
}

const char *autotest_row_problem(uint8_t id)
{
    const krow_t * const t = krows_by_id(id);
    const char *problem = "";

    if ( NULL != t )
    {
        problem = t->problem;
    }
    return problem;
}

const char *autotest_row_name(uint8_t id)
{
    const krow_t * const t = krows_by_id(id);
    const char *name = "";

    if ( NULL != t )
    {
        name = t->name;
    }
    return name;
}

/**
 * @brief Lay out the run a request asks for.
 *
 * @param me    The AO.
 * @param group AUTOTEST_G_*.
 * @param name  The one row, when @p group is AUTOTEST_G_NONE.
 * @return How many rows it holds; 0 when nothing answers to what was asked.
 */
static uint8_t order_build(Autotest *me, uint8_t group, const char *name)
{
    /* The `user` profile's order: the comment above says why this one. */
    static const uint8_t USER_ORDER[] = {
        AUTOTEST_ID_IOEXP,
        AUTOTEST_ID_CLOCKS,
        AUTOTEST_ID_IMU,
        AUTOTEST_ID_MAG,
        AUTOTEST_ID_HAPTIC,
        AUTOTEST_ID_SD_USER,
        AUTOTEST_ID_C5_USER,
        AUTOTEST_ID_IR,
        AUTOTEST_ID_NFC_FIELD,
        AUTOTEST_ID_RFID_FIELD,
        AUTOTEST_ID_AUDIO,
        USER_HOLD,
        AUTOTEST_ID_POWER_PLUG,
        AUTOTEST_ID_BUTTONS,
        AUTOTEST_ID_LED,
        AUTOTEST_ID_DISPLAY,
        AUTOTEST_ID_TP,
        AUTOTEST_ID_NFC_CARD,
        AUTOTEST_ID_RFID_CARD,
        AUTOTEST_ID_PSRAM,
        AUTOTEST_ID_MEM,
    };

    _Static_assert((sizeof(USER_ORDER) / sizeof(USER_ORDER[0])) <= AUTOTEST_RUN_MAX,
                   "the user profile does not fit a run");
    uint8_t rows = 0U;

    me->n = 0U;
    if ( AUTOTEST_G_USER == group )
    {
        for ( size_t k = 0U; (sizeof(USER_ORDER) / sizeof(USER_ORDER[0])) > k; ++k )
        {
            if ( USER_HOLD == USER_ORDER[k] )
            {
                me->order[me->n] = USER_HOLD;
                me->n += 1U;
            }
            else
            {
                /* A row no owner registered - the coprocessor's, with no
                 * radio linked - is simply not in the run. */
                for ( size_t i = 0U; krows_count() > i; ++i )
                {
                    if ( krows_at(i)->id == USER_ORDER[k] )
                    {
                        me->order[me->n] = (uint8_t)i;
                        me->n += 1U;
                        rows += 1U;
                    }
                }
            }
        }
    }
    else
    {
        for ( size_t i = 0U; (krows_count() > i) && (AUTOTEST_RUN_MAX > me->n); ++i )
        {
            const krow_t * const t = krows_at(i);
            bool selected;

            if ( AUTOTEST_G_NONE != group )
            {
                selected = ((t->groups & group) != 0U);
            }
            else
            {
                selected = (strcmp(name, t->name) == 0);
            }
            if ( selected )
            {
                /* In by its order key, not by when its owner registered. */
                uint8_t at = me->n;

                while ( (0U < at) && (krows_at(me->order[at - 1U])->order > t->order) )
                {
                    me->order[at] = me->order[at - 1U];
                    at -= 1U;
                }
                me->order[at] = (uint8_t)i;
                me->n += 1U;
                rows += 1U;
            }
        }
    }
    return rows;
}

/**
 * @brief Publish the answer to a run request.
 *
 * @param client Whose request it answers.
 * @param status AUTOTEST_TAKEN, AUTOTEST_BUSY or AUTOTEST_UNKNOWN.
 * @param rows   How many rows the run holds.
 * @return false when the pool had no event for it, and nothing was said.
 */
static bool publish_begin(uint8_t client, uint8_t status, uint8_t rows)
{
    AutotestBeginEvt *e = Q_NEW_X(AutotestBeginEvt, 0U, AUTOTEST_BEGIN_SIG);
    bool const said = (NULL != e);

    if ( said )
    {
        e->client = client;
        e->status = status;
        e->rows   = rows;
        QACTIVE_PUBLISH(&e->super, AO_Autotest);
    }
    return said;
}

/**
 * @brief Publish one row starting or ending.
 *
 * @param me        The AO, for whose run and which place.
 * @param t         The row.
 * @param state     AUTOTEST_ROW_*.
 * @param budget_ms The most its owner may take, for a row that is waited for.
 */
static void publish_row(const Autotest *me, const krow_t *t, uint8_t state,
                        uint32_t budget_ms)
{
    AutotestRowEvt *e = Q_NEW_X(AutotestRowEvt, 0U, AUTOTEST_ROW_SIG);

    if ( NULL != e )
    {
        e->client    = me->client;
        e->id        = t->id;
        e->pos       = me->pos;
        e->state     = state;
        (void)snprintf(e->name, sizeof(e->name), "%s", t->name);
        e->budget_ms = budget_ms;
        QACTIVE_PUBLISH(&e->super, AO_Autotest);
    }
}

/**
 * @brief Close a row: counted, kept for the report, and published.
 *
 * @param me    The AO.
 * @param t     The row.
 * @param state How it ended.
 */
static void row_close(Autotest *me, const krow_t *t, uint8_t state)
{
    if ( UINT8_MAX > me->tally[state] )
    {
        me->tally[state] += 1U;
    }
    if ( AUTOTEST_RUN_MAX > me->report.rows )
    {
        me->report.id[me->report.rows]    = t->id;
        me->report.state[me->report.rows] = state;
        me->report.rows += 1U;
    }
    publish_row(me, t, state, 0U);
}

/**
 * @brief How an owner's verdict reads, given what the row's steps said.
 *
 * An owner answers pass or fail. A fail with a step unanswered and none
 * answered no was never judged (nobody pressed TOP): a timeout. A card row
 * that found no card is skipped, where the row says an empty slot is no fault.
 *
 * @param me The AO, holding what the row's steps said.
 * @param t  The row.
 * @param ok The owner's verdict.
 * @return AUTOTEST_ROW_PASS, _FAIL, _TIMEOUT or _SKIPPED.
 */
static uint8_t verdict_state(const Autotest *me, const krow_t *t, bool ok)
{
    uint8_t state = AUTOTEST_ROW_FAIL;

    if ( ok )
    {
        state = AUTOTEST_ROW_PASS;
    }
    else if ( me->step_absent && ((t->flags & KROW_F_SKIP_NO_CARD) != 0U) )
    {
        state = AUTOTEST_ROW_SKIPPED;
    }
    else if ( me->step_timeout && !me->step_fail )
    {
        state = AUTOTEST_ROW_TIMEOUT;
    }
    else
    {
        /* A step answered no, or the row failed before asking anything. */
    }
    return state;
}

/**
 * @brief Stop the budget of the row that just answered.
 *
 * QTimeEvt_disarm() answers false both for a timer never armed and for one
 * that has expired and already posted its event into this queue. The second
 * is the one that matters: that event arrives behind the verdict, and read
 * as a deadline it would time out the row the walk has moved on to.
 *
 * @param me The AO.
 */
static void budget_stop(Autotest *me)
{
    if ( !QTimeEvt_disarm(&me->budget) && me->budget_live )
    {
        me->budget_stale += 1U;
    }
    me->budget_live = false;
}

/**
 * @brief Arm the one deadline this AO has.
 *
 * @param me The AO.
 * @param ms How long from now.
 */
static void budget_start(Autotest *me, uint32_t ms)
{
    (void)QTimeEvt_rearm(&me->budget, KQP_TICKS_PER_MS(ms));
    me->budget_live = true;
}

/**
 * @brief Stop listening for what only a run needs.
 *
 * @param me The AO.
 */
static void run_unsubscribe(Autotest *me)
{
    QActive_unsubscribe(&me->super, TEST_RESULT_SIG);
    QActive_unsubscribe(&me->super, TEST_SKIP_SIG);
    QActive_unsubscribe(&me->super, BTN_TEST_SIG);
    QActive_unsubscribe(&me->super, TOUCH_TEST_SIG);
    QActive_unsubscribe(&me->super, HAPTIC_TEST_SIG);
    QActive_unsubscribe(&me->super, DISPLAY_TEST_SIG);
    QActive_unsubscribe(&me->super, LED_TEST_SIG);
    QActive_unsubscribe(&me->super, POWER_TEST_SIG);
    QActive_unsubscribe(&me->super, STORAGE_TEST_SIG);
}

/**
 * @brief Listen for the verdicts and the steps of the rows about to run.
 *
 * Only while a run is in flight, so an idle sequencer is never woken by a
 * result that is somebody else's - `sd_verify` publishes one too.
 *
 * @param me The AO.
 */
static void run_subscribe(Autotest *me)
{
    QActive_subscribe(&me->super, TEST_RESULT_SIG);
    QActive_subscribe(&me->super, TEST_SKIP_SIG);
    QActive_subscribe(&me->super, BTN_TEST_SIG);
    QActive_subscribe(&me->super, TOUCH_TEST_SIG);
    QActive_subscribe(&me->super, HAPTIC_TEST_SIG);
    QActive_subscribe(&me->super, DISPLAY_TEST_SIG);
    QActive_subscribe(&me->super, LED_TEST_SIG);
    QActive_subscribe(&me->super, POWER_TEST_SIG);
    QActive_subscribe(&me->super, STORAGE_TEST_SIG);
}

/**
 * @brief End the run: its report kept when it is a whole `user` run, and
 *        the end published.
 *
 * @param me        The AO.
 * @param cancelled It ended before its last row.
 */
static void run_end(Autotest *me, bool cancelled)
{
    AutotestEndEvt *e;

    me->row     = NULL;
    me->running = false;
    me->holding = false;
    run_unsubscribe(me);

    if ( me->user && !cancelled )
    {
        me->report.version = REPORT_VERSION;
        me->report.when    = 0U;
        if ( khal_time_wall_known() )
        {
            me->report.when = (uint32_t)(khal_time_wall_us() / 1000000ULL);
        }
        if ( khal_nvs_set_blob(REPORT_NS, REPORT_KEY, &me->report,
                              sizeof(me->report)) != KODE_OK )
        {
            KLOG_W(TAG, "the report of this run could not be kept");
        }
    }
    else if ( me->single && !cancelled && (1U == me->report.rows) )
    {
        report_patch(me->report.id[0], me->report.state[0]);
    }
    else
    {
        /* A group run other than `user`, or a cancelled one: kept nowhere. */
    }
    /* The rows judged on the spot ran on this stack, so the end of a run is
     * where its low-water mark means something. */
    KLOG_D(TAG, "run over; %lu B of this stack never used",
           (unsigned long)khal_task_stack_free());

    e = Q_NEW_X(AutotestEndEvt, 0U, AUTOTEST_END_SIG);
    if ( NULL != e )
    {
        e->client = me->client;
        (void)memcpy(e->tally, me->tally, sizeof(e->tally));
        e->cancelled = cancelled;
        QACTIVE_PUBLISH(&e->super, AO_Autotest);
    }
    else
    {
        KLOG_E(TAG, "no event to say the run is over");
    }
}

/**
 * @brief Run the rows after the one that just ended, up to and including
 *        the next one that has to be waited for; end the run when none is
 *        left.
 *
 * A row judged on the spot is published and the walk goes on; an owner's row
 * is asked, its budget armed, and the walk stops until its verdict or the
 * budget. One row at a time: two operator scripts on one glass would judge
 * each other's presses. A hold stops the walk until the client answers it.
 *
 * @param me The AO.
 */
static void autotest_walk(Autotest *me)
{
    bool waiting = false;

    me->row = NULL;
    while ( (me->next < me->n) && !waiting && !me->cancel )
    {
        uint8_t const entry = me->order[me->next];

        me->next += 1U;
        if ( USER_HOLD == entry )
        {
            AutotestHoldEvt *h = Q_NEW_X(AutotestHoldEvt, 0U, AUTOTEST_HOLD_SIG);

            if ( NULL != h )
            {
                h->client = me->client;
                QACTIVE_PUBLISH(&h->super, AO_Autotest);
                me->holding = true;
                budget_start(me, HOLD_MS);
                waiting = true;
            }
            /* With no event to ask with, nobody would ever answer: the run
             * goes on rather than wait for good. */
        }
        else
        {
            const krow_t * const t = krows_at(entry);

            me->pos += 1U;
            me->step_fail    = false;
            me->step_timeout = false;
            me->step_absent  = false;
            if ( KROW_ASYNC == t->kind )
            {
                me->row = t;
                publish_row(me, t, AUTOTEST_ROW_RUNNING, t->budget_ms);
                budget_start(me, t->budget_ms);
                (void)t->fn();
                waiting = true;
            }
            else
            {
                publish_row(me, t, AUTOTEST_ROW_RUNNING, 0U);
                if ( t->fn() )
                {
                    row_close(me, t, AUTOTEST_ROW_PASS);
                }
                else
                {
                    row_close(me, t, AUTOTEST_ROW_FAIL);
                }
            }
        }
    }
    if ( !waiting )
    {
        run_end(me, me->cancel);   /* asked to end: so it says, last row or not */
    }
}

/**
 * @brief Publish the kept report: one event a row, then the marker.
 *
 * @param client Who asked.
 */
static void report_publish(uint8_t client)
{
    uint8_t         rows     = 0U;
    uint8_t         problems = 0U;
    uint32_t        when     = 0U;
    AutotestReportEvt *e;

    if ( report_load() )
    {
        rows = s_kept.rows;
        when = s_kept.when;
    }
    for ( uint8_t i = 0U; i < rows; ++i )
    {
        if ( (AUTOTEST_ROW_FAIL == s_kept.state[i])
            || (AUTOTEST_ROW_TIMEOUT == s_kept.state[i]) )
        {
            problems += 1U;
        }
        e = Q_NEW_X(AutotestReportEvt, 0U, AUTOTEST_REPORT_SIG);
        if ( NULL != e )
        {
            e->client   = client;
            e->last     = false;
            e->id       = s_kept.id[i];
            e->state    = s_kept.state[i];
            e->rows     = 0U;
            e->problems = 0U;
            e->when     = 0U;
            QACTIVE_PUBLISH(&e->super, AO_Autotest);
        }
    }
    e = Q_NEW_X(AutotestReportEvt, 0U, AUTOTEST_REPORT_SIG);
    if ( NULL != e )
    {
        e->client   = client;
        e->last     = true;
        e->id       = AUTOTEST_ID_NONE;
        e->state    = 0U;
        e->rows     = rows;
        e->problems = problems;
        e->when     = when;
        QACTIVE_PUBLISH(&e->super, AO_Autotest);
    }
}

void Autotest_ctor(void)
{
    Autotest * const me = &l_autotest;

    QActive_ctor(&me->super, Q_STATE_CAST(&Autotest_initial));
    QTimeEvt_ctorX(&me->budget, &me->super, AUTOTEST_BUDGET_SIG, 0U);
    QTimeEvt_ctorX(&me->psram_step, &me->super, AUTOTEST_PSRAM_STEP_SIG, 0U);
    me->psram_blocks  = 0U;
    me->psram_filling = false;
    me->psram_live    = false;
    me->budget_live  = false;
    me->budget_stale = 0U;
    me->running      = false;
    me->holding      = false;
    me->cancel       = false;
    me->user         = false;
    me->single       = false;
    me->client       = 0U;
    me->n            = 0U;
    me->next         = 0U;
    me->row          = NULL;
    me->pos          = 0U;
    me->step_fail    = false;
    me->step_timeout = false;
    me->step_absent  = false;
#if CONFIG_KODE_LOWPOWER
    me->lp_who       = KODE_LOWPOWER_WHO_NONE;
#endif
    (void)memset(me->tally, 0, sizeof(me->tally));
    (void)memset(&me->report, 0, sizeof(me->report));
}

#define KODE_QUEUE_AUTOTEST_LEN  16U  /**< AO_Autotest's queue: a request, a deadline and a row's verdicts. */

AO_STORAGE(autotest, KODE_QUEUE_AUTOTEST_LEN, KQP_AO_STACK_MEDIUM);

void kaos_autotest_start(void)
{
    Autotest_ctor();
    kaos_autotest_register();
    AO_START(AO_Autotest, autotest, KODE_PRIO_AUTOTEST);
}

/**
 * @brief Join the low-power rounds, then go active.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to Autotest_active.
 */
static QState Autotest_initial(Autotest * const me, void const * const par)
{
    (void)par;
    /* Verdicts and steps are listened for only while a run is in flight; the
     * rounds of the board's rest are heard always. */
#if CONFIG_KODE_LOWPOWER
    me->lp_who = lowpower_join(&me->super, "autotest");
    if ( KODE_LOWPOWER_WHO_NONE != me->lp_who )
    {
        QActive_subscribe(&me->super, LOWPOWER_ROUND_SIG);
    }
#endif
    return Q_TRAN(&Autotest_active);
}

/**
 * @brief Whether a published step belongs to a run this autotest asked for.
 *
 * Only the two operator scripts carry an origin: another client can ask for
 * them too, and a step of that run must not mark the row now running.
 *
 * @param e A step event of one of the guided rows.
 * @return true when the step is the autotest's own.
 */
static bool step_is_ours(QEvt const * const e)
{
    bool ours = true;

    if ( (enum_t)BTN_TEST_SIG == e->sig )
    {
        ours = (TEST_ORIGIN_AUTOTEST == Q_EVT_CAST(ButtonsTestEvt)->origin);
    }
    else if ( (enum_t)TOUCH_TEST_SIG == e->sig )
    {
        ours = (TEST_ORIGIN_AUTOTEST == Q_EVT_CAST(TouchTestEvt)->origin);
    }
    else
    {
        /* The other owners' steps have no origin: only the autotest asks for them. */
    }
    return ours;
}

/**
 * @brief Note what a guided row's step said, for the verdict that follows.
 *
 * @param me The AO.
 * @param e  A step event of one of the guided rows.
 */
static void step_note(Autotest *me, QEvt const * const e)
{
    switch ( e->sig )
    {
        case BTN_TEST_SIG: {
            uint8_t const phase = Q_EVT_CAST(ButtonsTestEvt)->phase;

            if ( BTN_TEST_TIMEOUT == phase )
            {
                me->step_timeout = true;
            }
            else if ( (BTN_TEST_STUCK == phase) || (BTN_TEST_DOUBLEFAIL == phase)
                     || (BTN_TEST_HELD == phase) || (BTN_TEST_GIVEUP == phase) )
            {
                me->step_fail = true;
            }
            else
            {
                /* A prompt, a pass, a forgiven slip (wrong, double) or the end of the script. */
            }
            break;
        }
        case TOUCH_TEST_SIG: {
            uint8_t const phase = Q_EVT_CAST(TouchTestEvt)->phase;

            if ( TP_TEST_TIMEOUT == phase )
            {
                me->step_timeout = true;
            }
            else if ( TP_TEST_FAIL == phase )
            {
                me->step_fail = true;
            }
            else
            {
                /* A target, a pass, a forgiven miss or the end of the script. */
            }
            break;
        }
        case HAPTIC_TEST_SIG: {
            HapticTestEvt const * const t = Q_EVT_CAST(HapticTestEvt);

            if ( HAPTIC_TEST_NOVERDICT == t->phase )
            {
                /* The row could not judge the motor at all: the same
                 * outcome as a step nobody answered. */
                me->step_timeout = true;
            }
            else if ( (HAPTIC_TEST_ABSENT != t->phase)
                     && ((t->flags & (HAPTIC_TF_PASS | HAPTIC_TF_LOUD
                                      | HAPTIC_TF_SHORT | HAPTIC_TF_MUTE))
                         == 0U) )
            {
                me->step_fail = true;
            }
            else
            {
                /* Absent, passed, or a reading the row could not use. */
            }
            break;
        }
        case DISPLAY_TEST_SIG: {
            uint8_t const phase = Q_EVT_CAST(DisplayTestEvt)->phase;

            if ( (uint8_t)DISPLAY_TEST_TIMEOUT == phase )
            {
                me->step_timeout = true;
            }
            else if ( (uint8_t)DISPLAY_TEST_FAIL == phase )
            {
                me->step_fail = true;
            }
            else
            {
                /* A question, a yes, or the end of the row. */
            }
            break;
        }
        case LED_TEST_SIG: {
            uint8_t const phase = Q_EVT_CAST(LedTestEvt)->phase;

            if ( (uint8_t)LED_TEST_TIMEOUT == phase )
            {
                me->step_timeout = true;
            }
            else if ( (uint8_t)LED_TEST_FAIL == phase )
            {
                me->step_fail = true;
            }
            else
            {
                /* A question, a yes, or the end of the row. */
            }
            break;
        }
        case POWER_TEST_SIG: {
            uint8_t const phase = Q_EVT_CAST(PowerTestEvt)->phase;

            if ( POWER_TEST_TIMEOUT == phase )
            {
                me->step_timeout = true;
            }
            else if ( POWER_TEST_FAIL == phase )
            {
                me->step_fail = true;
            }
            else
            {
                /* A question or a step that was answered. */
            }
            break;
        }
        case STORAGE_TEST_SIG: {
            StorageTestEvt const * const t = Q_EVT_CAST(StorageTestEvt);

            /* The card row's first step failing with no capacity read: the
             * slot is empty. With a capacity, the card or slot is at fault.
             * `sd_verify` numbers its phases from 1 too; its step count differs. */
            if ( (SD_TEST_IDENTIFY == t->phase) && !t->ok
                && (SD_TEST_STEPS == t->total) && (0U == t->capacity_mb) )
            {
                me->step_absent = true;
            }
            break;
        }
        default:
            /* Not a step this AO reads. */
            break;
    }
}

/**
 * @brief An owner answered: close the row in flight if the answer is its
 *        own, and go on. An answer of another origin is ignored.
 *
 * @param me      The AO.
 * @param r       The answer.
 * @param skipped It is TEST_SKIP_SIG: what the row needs is not there.
 */
static void owner_answered(Autotest *me, TestResultEvt const * const r,
                           bool skipped)
{
    /* The answer to another client's run is not this row's, whatever its name. */
    if ( TEST_ORIGIN_AUTOTEST != r->origin )
    {
        KLOG_D(TAG, "verdict of '%s' from origin %u is not the row's; ignored",
               r->name, (unsigned)r->origin);
    }
    else if ( NULL != me->row )
    {
        const krow_t * const t = me->row;
        const char *answer = t->name;

        if ( NULL != t->answer )
        {
            answer = t->answer;
        }
        /* Only the row in flight may close itself: a verdict from a row
         * given up on, arriving late, is nobody's. */
        if ( strcmp(r->name, answer) == 0 )
        {
            budget_stop(me);
            if ( skipped )
            {
                row_close(me, t, AUTOTEST_ROW_SKIPPED);
            }
            else
            {
                row_close(me, t, verdict_state(me, t, r->ok));
            }
            autotest_walk(me);
        }
        else
        {
            KLOG_W(TAG, "a verdict for %s while %s is running; ignored",
                   r->name, t->name);
        }
    }
}

/* ---- The PSRAM row ----------------------------------------------------------- */

/* The row takes the free PSRAM a block per step, then walks the four passes
 * of autotest_psram.c over all of it, KODE_PSRAM_TEST_STEP_BYTES a step.
 * Every step re-arms a timer of PSRAM_STEP_TICKS, so the GUI and everything
 * below this AO run between them. The blocks go back on every way out: the last step,
 * and the budget running out. */

/** @brief Start the PSRAM row: clear its state and arm the first step. */
static void psram_begin(void)
{
    Autotest * const me = &l_autotest;

    me->psram_blocks   = 0U;
    me->psram_filling  = true;
    me->psram_live     = true;
    me->psram_pass     = (uint8_t)PSRAM_PASS_WRITE_ADDR;
    me->psram_bad_pass = (uint8_t)PSRAM_PASS_WRITE_ADDR;
    me->psram_at       = 0U;
    me->psram_word     = 0U;
    me->psram_free     = khal_psram_free();
    (void)memset(&me->psram_fault, 0, sizeof(me->psram_fault));
    (void)QTimeEvt_rearm(&me->psram_step, PSRAM_STEP_TICKS);
}

/**
 * @brief Take one more block, if the reserve allows it.
 *
 * Judged on what is free now, not at the start: the system goes on
 * allocating while the row fills, and the reserve is what it keeps. And a
 * block is asked for only when the heap can give one whole, because a
 * refusal is counted against the boot (`alloc_fail`) and fails `mem`.
 *
 * @param me The AO.
 * @return true when a block was taken; false when the row has all it gets.
 */
static bool psram_take_block(Autotest *me)
{
    size_t const want = (size_t)KODE_PSRAM_TEST_BLOCK_BYTES
                      + (size_t)KODE_PSRAM_TEST_RESERVE_BYTES;
    bool took = false;

    if ( ((uint8_t)KODE_PSRAM_TEST_BLOCKS_MAX > me->psram_blocks)
        && (khal_psram_free() >= want)
        && (khal_psram_largest() >= (size_t)KODE_PSRAM_TEST_BLOCK_BYTES) )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() answers void *: PSRAM is taken by name (rule 8) */
        uint32_t * const b = (uint32_t *)khal_alloc_psram(
            sizeof(uint32_t), (size_t)KODE_PSRAM_TEST_BLOCK_BYTES);

        if ( NULL != b )
        {
            me->psram_block[me->psram_blocks] = b;
            me->psram_blocks += 1U;
            took = true;
        }
    }
    return took;
}

/**
 * @brief Give every block back and stop the walk; nothing when none runs.
 *
 * A step already queued finds the walk stopped and is dropped.
 *
 * @param me The AO.
 */
static void psram_release(Autotest *me)
{
    if ( me->psram_live )
    {
        (void)QTimeEvt_disarm(&me->psram_step);
        for ( uint8_t i = 0U; i < me->psram_blocks; i++ )
        {
            khal_free(me->psram_block[i]);
            me->psram_block[i] = NULL;
        }
        me->psram_blocks  = 0U;
        me->psram_filling = false;
        me->psram_live    = false;
    }
}

/**
 * @brief Say what the row found, give the blocks back, and close the row.
 *
 * @param me The AO.
 */
static void psram_finish(Autotest *me)
{
    uint32_t const tested_kb = (uint32_t)(((size_t)me->psram_blocks
                                           * (size_t)KODE_PSRAM_TEST_BLOCK_BYTES)
                                          / (size_t)PSRAM_KB);
    uint32_t const free_kb = (uint32_t)(me->psram_free / (size_t)PSRAM_KB);
    uint32_t const kept_kb = (uint32_t)KODE_PSRAM_TEST_RESERVE_BYTES / PSRAM_KB;
    psram_fault_t const f = me->psram_fault;
    PsramTestEvt * const e = Q_NEW_X(PsramTestEvt, 0U, PSRAM_TEST_SIG);
    uint8_t state = AUTOTEST_ROW_PASS;

    if ( NULL != e )
    {
        e->tested_kb = tested_kb;
        e->free_kb   = free_kb;
        e->errors    = f.errors;
        e->first     = (uint32_t)f.first;
        e->wrote     = f.wrote;
        e->read      = f.read;
        QACTIVE_PUBLISH(&e->super, AO_Autotest);
    }
    if ( 0U == me->psram_blocks )
    {
        KLOG_E(TAG, "test psram: FAIL - nothing to test: %lu KB free, %lu KB "
                    "kept for the system", (unsigned long)free_kb,
               (unsigned long)kept_kb);
        state = AUTOTEST_ROW_FAIL;
    }
    else if ( 0U != f.errors )
    {
        KLOG_E(TAG, "test psram: FAIL - %lu word(s) read wrong in %lu KB; the "
                    "first, in pass %u, at 0x%08lx held 0x%08lx, not 0x%08lx",
               (unsigned long)f.errors, (unsigned long)tested_kb,
               (unsigned)me->psram_bad_pass, (unsigned long)f.first,
               (unsigned long)f.read, (unsigned long)f.wrote);
        state = AUTOTEST_ROW_FAIL;
    }
    else
    {
        KLOG_N(TAG, "test psram: %lu KB of the %lu KB free read back right "
                    "in four passes", (unsigned long)tested_kb,
               (unsigned long)free_kb);
    }
    psram_release(me);
    budget_stop(me);
    if ( NULL != me->row )
    {
        row_close(me, me->row, state);
    }
    autotest_walk(me);
}

/**
 * @brief One step of the PSRAM row: one block taken, or one span of a pass.
 *
 * @param me The AO.
 */
static void psram_on_step(Autotest *me)
{
    bool done = false;

    if ( !me->psram_live )
    {
        /* A step queued before the walk was stopped: nothing to do. */
    }
    else if ( me->psram_filling )
    {
        me->psram_filling = psram_take_block(me);
        done = (!me->psram_filling) && (0U == me->psram_blocks);
    }
    else
    {
        uint32_t const errors_before = me->psram_fault.errors;
        size_t n = (size_t)PSRAM_BLOCK_WORDS - me->psram_word;

        if ( (size_t)PSRAM_STEP_WORDS < n )
        {
            n = (size_t)PSRAM_STEP_WORDS;
        }
        psram_pass_span(me->psram_pass,
                        &me->psram_block[me->psram_at][me->psram_word], n,
                        &me->psram_fault);
        if ( (0U == errors_before) && (0U != me->psram_fault.errors) )
        {
            me->psram_bad_pass = me->psram_pass;
        }
        me->psram_word += n;
        if ( (size_t)PSRAM_BLOCK_WORDS <= me->psram_word )
        {
            me->psram_word = 0U;
            me->psram_at  += 1U;
            if ( me->psram_at >= me->psram_blocks )
            {
                me->psram_at    = 0U;
                me->psram_pass += 1U;
            }
        }
        done = ((uint8_t)PSRAM_PASSES <= me->psram_pass);
    }
    if ( done )
    {
        psram_finish(me);
    }
    else if ( me->psram_live )
    {
        (void)QTimeEvt_rearm(&me->psram_step, PSRAM_STEP_TICKS);
    }
    else
    {
        /* Stopped: nothing to re-arm. */
    }
}

/**
 * @brief The only state: takes run requests, walks the rows, hears their answers.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Autotest_active(Autotest * const me, QEvt const * const e)
{
    QState status = Q_SUPER(&QHsm_top);

    switch ( e->sig )
    {
        case AUTOTEST_RUN_REQ_SIG: {
            AutotestRunReqEvt const * const r = Q_EVT_CAST(AutotestRunReqEvt);

            if ( me->running )
            {
                (void)publish_begin(r->client, AUTOTEST_BUSY, 0U);
            }
            else
            {
                uint8_t const rows = order_build(me, r->group, r->name);

                if ( 0U == rows )
                {
                    (void)publish_begin(r->client, AUTOTEST_UNKNOWN, 0U);
                }
                else if ( !publish_begin(r->client, AUTOTEST_TAKEN, rows) )
                {
                    /* Whoever asked was told nothing, so nothing starts: a
                     * run nobody knows about would take the glass, ask the
                     * operator, and refuse every request until it ended. */
                    KLOG_E(TAG, "no event to answer the request with; "
                                "the run is not started");
                }
                else
                {
                    me->running = true;
                    me->holding = false;
                    me->cancel  = false;
                    me->user    = (AUTOTEST_G_USER == r->group);
                    me->single  = (AUTOTEST_G_NONE == r->group);
                    me->client  = r->client;
                    me->next    = 0U;
                    me->pos     = 0U;
                    (void)memset(me->tally, 0, sizeof(me->tally));
                    (void)memset(&me->report, 0, sizeof(me->report));
                    run_subscribe(me);
                    autotest_walk(me);
                }
            }
            status = Q_HANDLED();
            break;
        }
        case AUTOTEST_CANCEL_REQ_SIG: {
            /* Only the client whose run it is may end it. */
            if ( me->running
                && (Q_EVT_CAST(AutotestClientEvt)->client == me->client) )
            {
                me->cancel = true;
                if ( me->holding )
                {
                    budget_stop(me);
                    autotest_walk(me);
                }
                /* A row in flight is never torn down halfway: the run ends
                 * when that row answers or runs out of budget. */
            }
            status = Q_HANDLED();
            break;
        }
        case AUTOTEST_CONTINUE_SIG: {
            if ( me->running && me->holding
                && (Q_EVT_CAST(AutotestClientEvt)->client == me->client) )
            {
                me->holding = false;
                budget_stop(me);
                autotest_walk(me);
            }
            status = Q_HANDLED();
            break;
        }
        case AUTOTEST_REPORT_REQ_SIG: {
            report_publish(Q_EVT_CAST(AutotestClientEvt)->client);
            status = Q_HANDLED();
            break;
        }
        case TEST_RESULT_SIG: {
            owner_answered(me, Q_EVT_CAST(TestResultEvt), false);
            status = Q_HANDLED();
            break;
        }
        case TEST_SKIP_SIG: {
            owner_answered(me, Q_EVT_CAST(TestResultEvt), true);
            status = Q_HANDLED();
            break;
        }
        case BTN_TEST_SIG:
        case TOUCH_TEST_SIG:
        case HAPTIC_TEST_SIG:
        case DISPLAY_TEST_SIG:
        case LED_TEST_SIG:
        case POWER_TEST_SIG:
        case STORAGE_TEST_SIG: {
            if ( (NULL != me->row) && step_is_ours(e) )
            {
                step_note(me, e);
            }
            status = Q_HANDLED();
            break;
        }
        case AUTOTEST_PSRAM_STEP_SIG: {
            psram_on_step(me);
            status = Q_HANDLED();
            break;
        }
        case AUTOTEST_BUDGET_SIG: {
            if ( 0U < me->budget_stale )
            {
                /* The deadline of a row that answered first. */
                me->budget_stale -= 1U;
            }
            else if ( me->holding )
            {
                me->budget_live = false;   /* this event is its expiry */
                KLOG_W(TAG, "nobody came back to the hold; the run ends here");
                me->cancel = true;
                autotest_walk(me);
            }
            else if ( NULL != me->row )
            {
                const krow_t * const t = me->row;

                me->budget_live = false;   /* this event is its expiry */
                psram_release(me);         /* the PSRAM row's blocks, if any */
                KLOG_W(TAG, "%s said nothing in %lu ms", t->name,
                       (unsigned long)t->budget_ms);
                row_close(me, t, AUTOTEST_ROW_TIMEOUT);
                autotest_walk(me);
            }
            else
            {
                /* No row in flight: nothing to time out. */
            }
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            /* A run is in flight until it ends, a held one included: the
             * operator at the pad would find the board gone mid-row. */
            lowpower_ready(me->lp_who, Q_EVT_CAST(LowPowerRoundEvt)->round,
                           me->running, "test row");
            status = Q_HANDLED();
            break;
        }
#endif
        default:
            /* Everything else is the top state's. */
            break;
    }
    return status;
}
