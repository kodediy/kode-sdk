/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_init_rows.c
 * @brief kode_init()'s table: a linked module starts in its place, an absent one is parked.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "Mockkdrv_es8311.h"
#include "Mockkdrv_st25r3916.h"
#include "Mockkhal_gpio.h"
#include "Mockkhal_i2c.h"

#include "ao_spy.h"

#include "fakes.h"
#include "kaos.h"
#include "kaos_claim.h"
#include "kbsp.h"
#include "kbsp_owners.h"
#include "kdrv_cst820.h"
#include "kdrv_radio.h"
#include "kode_sys_priv.h"
#include "owners_support.h"
#include "qf_support.h"

/** @brief What a CST820 answers at its chip_id register. */
#define CST_CHIP_ID_CST820   0xB7U
/** @brief The CST820's sleep-mode register. */
#define CST_REG_SLEEP_MODE   0xE5U
/** @brief What puts it in deep sleep. */
#define CST_SLEEP_DEEP_SLEEP 0x03U

/** @brief Each I2C park's one probe, as its driver times it (KTD_PROBE_MS,
 *         QMI_PROBE_MS, QMC_PROBE_MS, AW_PROBE_MS - private to each driver,
 *         repeated as the drivers' own cases repeat them). */
#define PROBE_MS_LED    100U
#define PROBE_MS_IMU    20U
#define PROBE_MS_MAG    20U
#define PROBE_MS_HAPTIC 50U

/** @brief Steps one case can record. */
#define LOG_MAX 32U

static const char *s_log[LOG_MAX];  /**< What ran, in order. */
static uint32_t    s_n;             /**< Steps that ran. */

/** @brief Write a step down. */
static void note(const char *what)
{
    if ( LOG_MAX > s_n )
    {
        s_log[s_n] = what;
    }
    ++s_n;
}

/** @brief Forget every step. */
static void log_clear(void)
{
    (void)memset(s_log, 0, sizeof(s_log));
    s_n = 0U;
}

static void start_a(void) { note("start a"); }
static void start_b(void) { note("start b"); }
static void start_c(void) { note("start c"); }
static void park_a(void)  { note("park a"); }
static void park_b(void)  { note("park b"); }
static void park_c(void)  { note("park c"); }
static void start_storage(void) { note("start storage"); }
static void start_appmgr(void)  { note("start appmgr"); }

/* This binary has no interface of its own, and a weak reference needs a definition on the host. */
void kode_app_early(void);
void kode_app_early(void)
{
}

/* The production table is read, never run: its starts would take slots earlier files hold. */
/** @brief The index of the production row whose start is @p start, or KODE_SYS_ROWS. */
static size_t row_of(kode_sys_step_t start)
{
    size_t i = 0U;

    while ( (KODE_SYS_ROWS > i) && (kode_sys_rows[i].start != start) )
    {
        ++i;
    }
    return i;
}

/** @brief The production table with every module absent: no start, no park. */
static void rows_absent(kode_sys_row_t rows[KODE_SYS_ROWS])
{
    (void)memset(rows, 0, sizeof(kode_sys_row_t) * KODE_SYS_ROWS);
}

TEST_CASE("every row linked: each starts in order and nothing is parked",
          "[kode_init]")
{
    static const kode_sys_row_t rows[] = {
        { .start = &start_a, .park = &park_a },
        { .start = &start_b, .park = &park_b },
        { .start = &start_c, .park = &park_c },
    };

    log_clear();
    kode_sys_rows_walk(rows, sizeof(rows) / sizeof(rows[0]));

    TEST_ASSERT_EQUAL_UINT32(3U, s_n);
    TEST_ASSERT_EQUAL_STRING("start a", s_log[0]);
    TEST_ASSERT_EQUAL_STRING("start b", s_log[1]);
    TEST_ASSERT_EQUAL_STRING("start c", s_log[2]);
}

/** @brief The boot task's own priority before kode_init(): FreeRTOS's main task's. */
#define ROWS_MAIN_PRIO  1U

/** @brief The boot task's priority while a row started, and while one parked. */
static uint32_t s_prio_at_start;
static uint32_t s_prio_at_park;

static void start_notes_prio(void)
{
    s_prio_at_start = fake_task_priority();
}

static void park_notes_prio(void)
{
    s_prio_at_park = fake_task_priority();
}

/* The interface starts in the early row and draws without a pause from then
 * on. On the boot task's core it holds every row below it back, so the walk
 * outranks it and hands the boot task its own priority back at the end. */
TEST_CASE("the rows start and park above the renderer, and the boot task gets "
          "its priority back", "[kode_init]")
{
    static const kode_sys_row_t rows[] = {
        { .start = &start_notes_prio, .park = NULL },
        { .start = NULL,              .park = &park_notes_prio },
    };

    fake_task_priority_set(ROWS_MAIN_PRIO);
    s_prio_at_start = 0U;
    s_prio_at_park  = 0U;

    kode_sys_rows_walk(rows, sizeof(rows) / sizeof(rows[0]));

    TEST_ASSERT_EQUAL_UINT32(KODE_PRIO_BOOT_WALK, s_prio_at_start);
    TEST_ASSERT_EQUAL_UINT32(KODE_PRIO_BOOT_WALK, s_prio_at_park);
    TEST_ASSERT_TRUE(KODE_PRIO_BOOT_WALK > KODE_PRIO_USER_GUI);
    TEST_ASSERT_EQUAL_UINT32(ROWS_MAIN_PRIO, fake_task_priority());
}

TEST_CASE("an absent row is not started, and its park runs after every start",
          "[kode_init]")
{
    static const kode_sys_row_t rows[] = {
        { .start = &start_a, .park = &park_a },
        { .start = NULL,     .park = &park_b },
        { .start = &start_c, .park = &park_c },
    };

    log_clear();
    kode_sys_rows_walk(rows, sizeof(rows) / sizeof(rows[0]));

    TEST_ASSERT_EQUAL_UINT32(3U, s_n);
    TEST_ASSERT_EQUAL_STRING("start a", s_log[0]);
    TEST_ASSERT_EQUAL_STRING("start c", s_log[1]);
    TEST_ASSERT_EQUAL_STRING("park b", s_log[2]);
}

TEST_CASE("an absent row with nothing to park is passed over", "[kode_init]")
{
    static const kode_sys_row_t rows[] = {
        { .start = NULL,     .park = NULL },
        { .start = &start_b, .park = NULL },
    };

    log_clear();
    kode_sys_rows_walk(rows, sizeof(rows) / sizeof(rows[0]));

    TEST_ASSERT_EQUAL_UINT32(1U, s_n);
    TEST_ASSERT_EQUAL_STRING("start b", s_log[0]);
}

TEST_CASE("the startup order: AO_Boot first, AO_Storage before AO_AppMgr, "
          "AO_IoExp before its users, the panel before the radio", "[kode_init]")
{
    size_t const boot    = row_of(&kaos_boot_start);
    size_t const storage = row_of(&kaos_storage_start);
    size_t const radio   = row_of(&kaos_radio_start);
    size_t const net     = row_of(&kaos_net_start);
    size_t const appmgr  = row_of(&kaos_appmgr_start);
    size_t const ioexp   = row_of(&kaos_ioexp_start);
    size_t const buttons = row_of(&kaos_buttons_start);
    size_t const touch   = row_of(&kaos_touch_start);
    size_t const display = row_of(&kaos_display_start);

    TEST_ASSERT_EQUAL_size_t(0U, boot);
    TEST_ASSERT_LESS_THAN_size_t(radio, storage);
    TEST_ASSERT_LESS_THAN_size_t(net, radio);
    TEST_ASSERT_LESS_THAN_size_t(appmgr, storage);
    TEST_ASSERT_LESS_THAN_size_t(buttons, ioexp);
    TEST_ASSERT_LESS_THAN_size_t(touch, ioexp);
    TEST_ASSERT_LESS_THAN_size_t(display, ioexp);
    TEST_ASSERT_LESS_THAN_size_t(radio, display);
}

TEST_CASE("this binary links every module, so kode_init() would park nothing",
          "[kode_init]")
{
    for ( size_t i = 0U; KODE_SYS_ROWS > i; ++i )
    {
        TEST_ASSERT_NOT_NULL(kode_sys_rows[i].start);
    }
}

TEST_CASE("with KODEOS_NFC on, AO_Nfc has its row and parks the reader", "[kode_init]")
{
#if !CONFIG_KODEOS_NFC
    TEST_FAIL_MESSAGE("CONFIG_KODEOS_NFC is not set: kode_init() would have "
                      "no NFC row to start or park");
#endif
    size_t const nfc = row_of(&kaos_nfc_start);

    TEST_ASSERT_LESS_THAN_size_t(KODE_SYS_ROWS, nfc);
    TEST_ASSERT_TRUE(kode_sys_rows[nfc].park == &kaos_park_nfc);
    TEST_ASSERT_LESS_THAN_size_t(KODE_SYS_ROWS, row_of(&kode_sys_nfc_init));
}

TEST_CASE("with AO_AppMgr and AO_Storage both linked, AO_Storage starts first",
          "[kode_init]")
{
    kode_sys_row_t rows[KODE_SYS_ROWS];

    rows_absent(rows);
    rows[row_of(&kaos_storage_start)].start = &start_storage;
    rows[row_of(&kaos_appmgr_start)].start  = &start_appmgr;

    log_clear();
    kode_sys_rows_walk(rows, KODE_SYS_ROWS);

    TEST_ASSERT_EQUAL_UINT32(2U, s_n);
    TEST_ASSERT_EQUAL_STRING("start storage", s_log[0]);
    TEST_ASSERT_EQUAL_STRING("start appmgr", s_log[1]);
}

TEST_CASE("the radio's row parks the C5", "[kode_init]")
{
    TEST_ASSERT_TRUE(kode_sys_rows[row_of(&kaos_radio_start)].park == &kaos_park_c5);
}

TEST_CASE("an app with neither AO_Radio nor AO_Audio: the C5 is held in reset "
          "and claimed as parked", "[kode_init]")
{
    kode_sys_row_t rows[KODE_SYS_ROWS];
    size_t const   radio = row_of(&kaos_radio_start);
    bool const     held  = owners_lend(KBSP_RES_C5, AO_Radio);

    fake_radio_reset();
    fake_kaos_absent(&kaos_audio_start);
    rows_absent(rows);
    rows[radio].park = kode_sys_rows[radio].park;

    kode_sys_rows_walk(rows, KODE_SYS_ROWS);

    TEST_ASSERT_TRUE(kdrv_radio_c5_held());
    TEST_ASSERT_EQUAL_STRING("parked(AO_Radio)", kbsp_owner_name(KBSP_RES_C5));
    owners_return(KBSP_RES_C5, AO_Radio, "AO_Radio", held);
    fake_radio_reset();
}

TEST_CASE("an app with AO_Audio and no AO_Radio: the C5 stays running, claimed "
          "for the amplifier", "[kode_init]")
{
    kode_sys_row_t rows[KODE_SYS_ROWS];
    size_t const   radio = row_of(&kaos_radio_start);
    bool const     held  = owners_lend(KBSP_RES_C5, AO_Radio);

    fake_radio_reset();
    rows_absent(rows);
    rows[radio].park = kode_sys_rows[radio].park;

    kode_sys_rows_walk(rows, KODE_SYS_ROWS);

    TEST_ASSERT_FALSE(kdrv_radio_c5_held());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_radio_c5_holds(),
        "the C5 was not reset, so its firmware never raised the amplifier");
    TEST_ASSERT_EQUAL_STRING("running for the amp", kbsp_owner_name(KBSP_RES_C5));
    owners_return(KBSP_RES_C5, AO_Radio, "AO_Radio", held);
}

/** @brief Whether a captured log line contains @p needle. */
static bool log_has(const char *needle)
{
    bool has = false;

    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        if ( strstr(fake_klog_line(i), needle) != NULL )
        {
            has = true;
        }
    }
    return has;
}

TEST_CASE("an app with AO_Audio and no AO_Radio whose C5 will not reset: the "
          "log says so, and never that it reset", "[kode_init]")
{
    kode_sys_row_t   rows[KODE_SYS_ROWS];
    kdrv_radio_req_t connect;
    size_t const     radio = row_of(&kaos_radio_start);
    bool const       held  = owners_lend(KBSP_RES_C5, AO_Radio);

    fake_radio_reset();
    fake_klog_reset();
    /* A connect has run: the reset line is esp-hosted's, and a hold is refused. */
    (void)memset(&connect, 0, sizeof(connect));
    connect.type = KDRV_RADIO_JOB_CONNECT;
    (void)kdrv_radio_run(&connect);
    rows_absent(rows);
    rows[radio].park = kode_sys_rows[radio].park;

    kode_sys_rows_walk(rows, KODE_SYS_ROWS);

    TEST_ASSERT_EQUAL_UINT(0U, fake_radio_c5_holds());
    TEST_ASSERT_FALSE_MESSAGE(log_has("the C5 reset"), "a reset that never happened was logged");
    TEST_ASSERT_TRUE(log_has("would not reset"));
    TEST_ASSERT_EQUAL_STRING("running for the amp", kbsp_owner_name(KBSP_RES_C5));
    owners_return(KBSP_RES_C5, AO_Radio, "AO_Radio", held);
    fake_radio_reset();
}

TEST_CASE("an absent AO_Rfid's park says once that it cannot tell a fitted "
          "front end", "[kode_init]")
{
    bool const held = owners_lend(KBSP_RES_RFID, AO_Rfid);

    fake_klog_reset();
    kaos_park_rfid();

    TEST_ASSERT_EQUAL_STRING("parked(AO_Rfid)", kbsp_owner_name(KBSP_RES_RFID));
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());
    TEST_ASSERT_NOT_NULL(strstr(fake_klog_line(0U), "parked (fitted or not)"));
    TEST_ASSERT_EQUAL_INT(0, strncmp(fake_klog_line(0U), "!I", 2U));
    owners_return(KBSP_RES_RFID, AO_Rfid, "AO_Rfid", held);
}

TEST_CASE("an app with AO_Radio: the C5 is left to it, unparked and unclaimed "
          "by the core", "[kode_init]")
{
    kode_sys_row_t rows[KODE_SYS_ROWS];
    size_t const   radio = row_of(&kaos_radio_start);
    bool const     held  = owners_lend(KBSP_RES_C5, AO_Radio);

    fake_radio_reset();
    log_clear();
    rows_absent(rows);
    rows[radio].start = &start_a;
    rows[radio].park  = kode_sys_rows[radio].park;

    kode_sys_rows_walk(rows, KODE_SYS_ROWS);

    TEST_ASSERT_EQUAL_UINT32(1U, s_n);
    TEST_ASSERT_FALSE(kdrv_radio_c5_held());
    TEST_ASSERT_NULL(kbsp_owner_name(KBSP_RES_C5));
    owners_return(KBSP_RES_C5, AO_Radio, "AO_Radio", held);
}

/** @brief One resource a park claims, and the owner that holds it when linked. */
typedef struct {
    kbsp_res_t        res;     /**< The resource.                  */
    QActive *const   *holder;  /**< Its owner's pointer.           */
    const char       *owner;   /**< Its owner's name.              */
    const char       *parked;  /**< What `ps` says once it is parked. */
} parked_res_t;

TEST_CASE("every park claims its owner's parts as parked and carries on past "
          "a part that is not fitted", "[kode_init]")
{
    static const parked_res_t RES[] = {
        { KBSP_RES_SDMMC,       &AO_Storage, "AO_Storage", "parked(AO_Storage)" },
        { KBSP_RES_FATFS,       &AO_Storage, "AO_Storage", "parked(AO_Storage)" },
        { KBSP_RES_LED,         &AO_LedMgr,  "AO_LedMgr",  "parked(AO_LedMgr)"  },
        { KBSP_RES_IR,          &AO_Ir,      "AO_Ir",      "parked(AO_Ir)"      },
        { KBSP_RES_NFC,         &AO_Nfc,     "AO_Nfc",     "parked(AO_Nfc)"     },
        { KBSP_RES_RFID,        &AO_Rfid,    "AO_Rfid",    "parked(AO_Rfid)"    },
        { KBSP_RES_AUDIO,       &AO_Audio,   "AO_Audio",   "parked(AO_Audio)"   },
        { KBSP_RES_IMU,         &AO_Imu,     "AO_Imu",     "parked(AO_Imu)"     },
        { KBSP_RES_HAPTIC,      &AO_Haptic,  "AO_Haptic",  "parked(AO_Haptic)"  },
        { KBSP_RES_APPS_REGION, &AO_AppMgr,  "AO_AppMgr",  "parked(AO_AppMgr)"  },
        { KBSP_RES_APP_PENDING, &AO_AppMgr,  "AO_AppMgr",  "parked(AO_AppMgr)"  },
        { KBSP_RES_OTA_SLOT,    &AO_AppMgr,  "AO_AppMgr",  "parked(AO_AppMgr)"  },
    };
    /* The amplifier's ask and the touch park are cases of their own below. */
    fake_kaos_absent(&kaos_radio_start);
    static const kode_sys_step_t PARKS[] = {
        &kaos_park_storage, &kaos_park_led, &kaos_park_ir, &kaos_park_nfc,
        &kaos_park_rfid, &kaos_park_audio, &kaos_park_imu, &kaos_park_haptic,
        &kaos_park_appmgr,
    };
    size_t const n = sizeof(RES) / sizeof(RES[0]);
    bool         held[sizeof(RES) / sizeof(RES[0])];

    /* The bus is up and no part answers on it; the codec and the reader are
     * missing too. Each I2C park probes once, in park order, and stops: the
     * probe is expected and nothing after it is, so a park that went straight
     * to its bring-up fails here whatever state the driver is in. */
    static const uint16_t PROBED[]    = {
        KBSP_I2C_ADDR_LED, KBSP_I2C_ADDR_IMU, KBSP_I2C_ADDR_MAG, KBSP_I2C_ADDR_HAPTIC,
    };
    static const uint32_t PROBED_MS[] = {
        PROBE_MS_LED, PROBE_MS_IMU, PROBE_MS_MAG, PROBE_MS_HAPTIC,
    };

    /* Every Init first: one frees the pool every mock chains from. */
    fake_klog_reset();
    Mockkhal_i2c_Init();
    Mockkdrv_es8311_Init();
    Mockkdrv_st25r3916_Init();
    for ( size_t i = 0U; (sizeof(PROBED) / sizeof(PROBED[0])) > i; ++i )
    {
        khal_i2c_init_ExpectAndReturn(KODE_OK);
        khal_i2c_probe_ExpectAndReturn(PROBED[i], PROBED_MS[i], false);
    }
    kdrv_es8311_park_ExpectAndReturn(KODE_ERROR);
    kdrv_st25r3916_park_ExpectAndReturn(KODE_ERROR);

    for ( size_t i = 0U; i < n; ++i )
    {
        held[i] = owners_lend(RES[i].res, *RES[i].holder);
    }
    for ( size_t i = 0U; (sizeof(PARKS) / sizeof(PARKS[0])) > i; ++i )
    {
        PARKS[i]();
    }
    for ( size_t i = 0U; i < n; ++i )
    {
        TEST_ASSERT_EQUAL_STRING(RES[i].parked, kbsp_owner_name(RES[i].res));
    }
    /* A part that is not fitted is one line, and not a warning or an error. */
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        TEST_ASSERT_FALSE_MESSAGE((strncmp(l, "!E", 2U) == 0) || (strncmp(l, "!W", 2U) == 0), l);
    }
    for ( size_t i = 0U; i < n; ++i )
    {
        owners_return(RES[i].res, *RES[i].holder, RES[i].owner, held[i]);
    }

    Mockkdrv_st25r3916_Verify();
    Mockkdrv_st25r3916_Destroy();
    Mockkdrv_es8311_Verify();
    Mockkdrv_es8311_Destroy();
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* A subscription and a publish name nobody, so the two owners that are only
 * ever heard on the bus are reached through a door of their own; the door is
 * what links them, and it must behave as the bare call did. */
TEST_CASE("touch_subscribe() makes its caller hear AO_Touch's reports",
          "[kode_init]")
{
    static TouchEvt touch;

    qf_support_init();
    QActive * const spy = ao_spy();
    ao_spy_drain();
    QActive_unsubscribe(spy, TOUCH_EVT_SIG);

    touch_subscribe(spy);
    touch.super   = (QEvt)QEVT_INITIALIZER(TOUCH_EVT_SIG);
    touch.pressed = true;
    QACTIVE_PUBLISH(&touch.super, NULL);

    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    ao_spy_drain();
    QActive_unsubscribe(spy, TOUCH_EVT_SIG);
}

/* The parks that do more than a register write of a mocked driver: each one's
 * call, and what it does around the owner it may ask. */

/** @brief The CST820's register file as the touch park reaches it. */
static uint8_t s_cst_sleep_reg;
static uint8_t s_cst_sleep_val;

static kode_status_t cst_read(khal_i2c_dev_t dev, uint8_t reg, uint8_t *buf,
                              size_t len, int ncalls)
{
    (void)dev; (void)reg; (void)ncalls;
    (void)memset(buf, 0, len);
    buf[0] = CST_CHIP_ID_CST820;
    return KODE_OK;
}

/** @brief Something for the driver to hold as its device. */
static uint8_t s_cst_dev;

static kode_status_t cst_add(uint16_t addr, khal_i2c_dev_t *out, int ncalls)
{
    (void)addr; (void)ncalls;
    *out = (khal_i2c_dev_t)(void *)&s_cst_dev;
    return KODE_OK;
}

static kode_status_t cst_write(khal_i2c_dev_t dev, uint8_t reg, uint8_t val,
                               int ncalls)
{
    (void)dev; (void)ncalls;
    s_cst_sleep_reg = reg;
    s_cst_sleep_val = val;
    return KODE_OK;
}

TEST_CASE("an absent AO_Touch's park, with no AO_Display, sends the CST820 to "
          "deep sleep", "[kode_init]")
{
    bool const held = owners_lend(KBSP_RES_TOUCH, AO_Touch);

    fake_kaos_absent(&kaos_display_start);
    /* The cases after this one find the driver as the ones before left it. */
    bool const was_up = kdrv_cst820_present();

    s_cst_sleep_reg = 0U;
    s_cst_sleep_val = 0U;
    Mockkhal_i2c_Init();
    khal_i2c_init_IgnoreAndReturn(KODE_OK);
    khal_i2c_probe_IgnoreAndReturn(true);
    khal_i2c_add_StubWithCallback(&cst_add);
    khal_i2c_remove_IgnoreAndReturn(KODE_OK);
    khal_i2c_read_reg_StubWithCallback(&cst_read);
    khal_i2c_write_reg_StubWithCallback(&cst_write);

    kaos_park_touch();

    TEST_ASSERT_EQUAL_STRING("parked(AO_Touch)", kbsp_owner_name(KBSP_RES_TOUCH));
    TEST_ASSERT_EQUAL_HEX8(CST_REG_SLEEP_MODE, s_cst_sleep_reg);
    TEST_ASSERT_EQUAL_HEX8(CST_SLEEP_DEEP_SLEEP, s_cst_sleep_val);
    TEST_ASSERT_FALSE_MESSAGE(kdrv_cst820_present(), "the driver kept a chip that no longer answers");
    if ( was_up )
    {
        TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cst820_init());
    }
    owners_return(KBSP_RES_TOUCH, AO_Touch, "AO_Touch", held);
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("an absent AO_Touch's park, with AO_Display linked, only claims", "[kode_init]")
{
    bool const held = owners_lend(KBSP_RES_TOUCH, AO_Touch);

    /* No expectation: any call on the bus fails the case. */
    Mockkhal_i2c_Init();
    kaos_park_touch();
    TEST_ASSERT_EQUAL_STRING("parked(AO_Touch)", kbsp_owner_name(KBSP_RES_TOUCH));
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
    owners_return(KBSP_RES_TOUCH, AO_Touch, "AO_Touch", held);
}

TEST_CASE("an absent AO_Storage's park switches the card's supply off", "[kode_init]")
{
    bool const sdmmc = owners_lend(KBSP_RES_SDMMC, AO_Storage);
    bool const fatfs = owners_lend(KBSP_RES_FATFS, AO_Storage);

    Mockkhal_gpio_Init();
    khal_gpio_output_ExpectAndReturn(KBSP_PIN_SD_PWR, true, KODE_OK);

    kaos_park_storage();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
    owners_return(KBSP_RES_SDMMC, AO_Storage, "AO_Storage", sdmmc);
    owners_return(KBSP_RES_FATFS, AO_Storage, "AO_Storage", fatfs);
}

TEST_CASE("an absent AO_Audio's park asks AO_Radio for the amplifier off, and "
          "nobody with no AO_Radio", "[kode_init]")
{
    bool const held = owners_lend(KBSP_RES_AUDIO, AO_Audio);

    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();
    ao_spy_subscribe(C5_AMP_OFF_SIG);
    Mockkdrv_es8311_Init();
    kdrv_es8311_park_IgnoreAndReturn(KODE_OK);

    kaos_park_audio();
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    TEST_ASSERT_EQUAL_INT(C5_AMP_OFF_SIG, ao_spy_take()->sig);
    ao_spy_drain();

    fake_kaos_absent(&kaos_radio_start);
    kaos_park_audio();
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    QActive_unsubscribe(ao_spy(), C5_AMP_OFF_SIG);
    Mockkdrv_es8311_Verify();
    Mockkdrv_es8311_Destroy();
    owners_return(KBSP_RES_AUDIO, AO_Audio, "AO_Audio", held);
}
