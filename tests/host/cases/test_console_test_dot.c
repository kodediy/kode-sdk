/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_test_dot.c
 * @brief The station's console verbs: who may type them and what reaches AO_TestDot.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "esp_app_desc.h"
#include "esp_partition.h"
#include "psa/crypto.h"

#include "unity.h"

#include "ao_spy.h"
#include "autotest_support.h"
#include "fake_station.h"
#include "fakes.h"
#include "kaos.h"
#include "kapp_desc.h"
#include "kapps_fmt.h"
#include "kdrv_apps.h"
#include "kcli_proto.h"
#include "kqp.h"
#include "qf_support.h"
#include "test_dot_support.h"
#include "test_dot_priv.h"

#if !CONFIG_KODE_TEST_DOT
#error "test_console_test_dot.c needs CONFIG_KODE_TEST_DOT: main/CMakeLists.txt spells it out"
#endif

/** @brief Past a station verb's budget, the longest of any command: whatever is left open times out. */
#define TDC_CLOSE_MS          (KODE_TEST_DOT_LINE_MS + 1000U)
/** @brief Most rounds of the two queues after a typed line. */
#define TDC_ROUNDS            16U

/** @brief Empty an Active Object's queue, so a case sees only its own events. */
static void mailbox_empty(QActive *ao)
{
    const QEvt *e = NULL;

    qf_support_give_queue(ao);
    while ( xQueueReceive(ao->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief The console up, then AO_TestDot built afresh (stored lock or not). */
static void up(bool locked)
{
    qf_support_init();
    autotest_up();
    (void)ao_spy();
    ao_spy_drain();
    if ( qf_support_claim_ao(AO_Console) )
    {
        Console_ctor();
        qf_support_give_queue(AO_Console);
        AO_Console->prio  = KODE_PRIO_CONSOLE;
        AO_Console->pthre = KODE_PRIO_CONSOLE;
        QActive_register_(AO_Console);
        QASM_INIT(AO_Console, (void *)0, 0U);
        qf_support_background(AO_Console);
    }
    (void)qf_support_tick(AO_Console, QF_SUPPORT_STEPS(TDC_CLOSE_MS));
    if ( locked )
    {
        td_up_locked();
    }
    else
    {
        td_up();
    }
    mailbox_empty(AO_Boot);
    fake_klog_reset();
}

/**
 * @brief End a case: the console's command closed, then AO_TestDot quiet.
 *
 * A station verb's budget is longer than any other command's, so a command
 * left open here would outlast the time-out the console files after this one
 * tick through, and they would read busy.
 */
static void down(void)
{
    (void)qf_support_tick(AO_Console, QF_SUPPORT_STEPS(TDC_CLOSE_MS));
    td_down();
}

/** @brief Both queues dispatched until neither holds anything. */
static void exchange(void)
{
    for ( unsigned i = 0U; TDC_ROUNDS > i; i++ )
    {
        td_settle();
        if ( qf_support_settle(AO_Console) == 0U )
        {
            break;
        }
    }
}

/** @brief A line typed on port @p src, then the exchange it starts. */
static void type_from(uint8_t src, const char *text)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = src;
    (void)snprintf(e.line, sizeof(e.line), "%s", text);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
    autotest_typed(text);
}

/** @brief A line typed on the cable, and its exchange. */
static void type_line(const char *text)
{
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, text);
    exchange();
}

/** @brief How many of the log's lines hold @p needle. */
static unsigned replied_n(const char *needle)
{
    unsigned n = 0U;

    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            n++;
        }
    }
    return n;
}

/** @brief Whether a log line holds @p needle. */
static bool replied(const char *needle)
{
    return replied_n(needle) > 0U;
}

/** @brief The station requests waiting in AO_TestDot's queue; the queue is emptied. @p relays Out: the last one's byte. */
static unsigned requests(uint8_t *relays)
{
    unsigned n = 0U;
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_TestDot->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)TEST_DOT_REQ_SIG == e->sig )
        {
            n++;
            if ( NULL != relays )
            {
                *relays = ((const TestDotReqEvt *)e)->a.relays;
            }
        }
        QF_gc(e);
    }
    return n;
}

TEST_CASE("test_all_from_ble_or_channel_is_refused_cable", "[console_test_dot]")
{
    up(false);
    type_from((uint8_t)KODE_CONSOLE_SRC_BLE, "test component=all");
    TEST_ASSERT_TRUE(replied(">err code=5 msg=cable"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, requests(NULL), "nothing reaches AO_TestDot");

    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_NET, "test component=all");
    TEST_ASSERT_TRUE(replied(">err code=5 msg=net"));
    TEST_ASSERT_EQUAL_UINT(0U, requests(NULL));

    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_BLE, "test_dot_info");
    TEST_ASSERT_TRUE(replied(">err code=5 msg=cable"));
    TEST_ASSERT_EQUAL_UINT(0U, requests(NULL));

    /* The cable reaches it. */
    fake_klog_reset();
    type_line("test_dot_info");
    TEST_ASSERT_TRUE(replied(">|test_dot state=dormant"));
    TEST_ASSERT_TRUE(replied(">ok"));
    down();
}

TEST_CASE("test_locked_refuses_every_station_verb", "[console_test_dot]")
{
    static const char *const LINES[] = {
        "test component=all", "test_dot_place", "test_dot_poll", "test_dot_screen phase=connecting",
        "test_dot_photos", "test_dot_blob name=\"location\"", "test_dot_relays state=0xFF", "test_dot_adc ch=0",
        "test_dot_loop", "test_dot_boost state=on", "test_dot_result verdict=fail", "test_dot_lock confirm=on",
        "test_dot_end", "test_dot_info", "test_dot_camera", "test_dot_bright pct=50",
    };
    unsigned const n = (unsigned)(sizeof(LINES) / sizeof(LINES[0]));

    up(true);
    for ( unsigned i = 0U; i < n; i++ )
    {
        type_line(LINES[i]);
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(n, replied_n(">err code=5 msg=locked"), "every verb refused");
    TEST_ASSERT_EQUAL_UINT(0U, replied_n(">ok"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_station_probes(), "nothing woke");

    /* The factory reset is not a station verb: it still runs. */
    fake_klog_reset();
    type_line("factory_reset confirm=on");
    TEST_ASSERT_FALSE(replied("msg=locked"));
    TEST_ASSERT_EQUAL_UINT(1U, td_posted(AO_Boot, FACTORY_RESET_REQ_SIG));
    down();
}

TEST_CASE("test_factory_reset_refused_while_a_run_goes", "[console_test_dot]")
{
    up(false);
    /* A run the station started; its line is not the console's here. */
    td_verb_plain((uint8_t)TEST_DOT_VERB_RUN);
    td_wake_ok();
    TEST_ASSERT_TRUE(test_dot_busy());

    type_line("factory_reset confirm=on");
    TEST_ASSERT_TRUE(replied(">err code=4 msg=busy"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, td_posted(AO_Boot, FACTORY_RESET_REQ_SIG), "nothing erased");

    td_end();
    TEST_ASSERT_FALSE(test_dot_busy());
    fake_klog_reset();
    type_line("factory_reset confirm=on");
    TEST_ASSERT_FALSE(replied("msg=busy"));
    TEST_ASSERT_EQUAL_UINT(1U, td_posted(AO_Boot, FACTORY_RESET_REQ_SIG));
    down();
}

TEST_CASE("test_relays_take_hex_or_decimal_and_refuse_the_forbidden_pair", "[console_test_dot]")
{
    uint8_t b = 0U;

    up(false);
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_relays state=0xFE");
    TEST_ASSERT_EQUAL_UINT(1U, requests(&b));
    TEST_ASSERT_EQUAL_HEX8(0xFEU, b);

    up(false);
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_relays state=254");
    TEST_ASSERT_EQUAL_UINT(1U, requests(&b));
    TEST_ASSERT_EQUAL_HEX8(0xFEU, b);

    up(false);
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_relays state=\"0xFE\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=state"));
    TEST_ASSERT_EQUAL_UINT(0U, requests(NULL));

    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_relays state=0x1FF");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=state"));

    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_relays state=0xFD");
    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=5 msg=forbidden"), "R2 on through R1 off");
    TEST_ASSERT_EQUAL_UINT(0U, requests(NULL));
    down();
}

TEST_CASE("test_welcome_and_storage_are_armed_by_the_file_name", "[console_test_dot]")
{
    up(false);
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_welcome name=\"welcome.kfw\"");
    TEST_ASSERT_TRUE(replied(">|welcome armed=welcome.kfw"));
    TEST_ASSERT_TRUE(replied(">ok"));

    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_storage name=\"storage.bin\"");
    TEST_ASSERT_TRUE(replied(">|storage armed=storage.bin"));
    TEST_ASSERT_TRUE(replied(">ok"));

    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_welcome");
    TEST_ASSERT_TRUE_MESSAGE(replied(">err"), "a verb with no name arms nothing");
    test_dot_welcome_arm("");
    test_dot_storage_arm("");
    down();
}

/** @brief Where the cases below install "onboarding": a block no other case of this file writes. */
#define TDC_APP_OFF   (KAPPS_DATA_OFF + (30U * KAPPS_ALIGN))
/** @brief Its length: odd, so no read ends on a sector. */
#define TDC_APP_LEN   (0x3000U + 5U)

/** @brief The lower-case hex of the SHA-256 of @p n bytes at @p d, worked out here. */
static void tdc_sha_hex(const uint8_t *d, size_t n, char *hex)
{
    uint8_t dg[32];
    size_t  len = 0U;

    TEST_ASSERT_EQUAL_INT(PSA_SUCCESS, psa_crypto_init());
    TEST_ASSERT_EQUAL_INT(PSA_SUCCESS, psa_hash_compute(PSA_ALG_SHA_256, d, n, dg, sizeof(dg), &len));
    for ( size_t i = 0U; sizeof(dg) > i; i++ )
    {
        (void)snprintf(&hex[2U * i], 3U, "%02x", (unsigned)dg[i]);
    }
}

/** @brief "onboarding" written at TDC_APP_OFF and listed alone; @p hex its SHA-256. Returns the directory it replaced. */
static kapps_dir_t tdc_install_onboarding(char *hex)
{
    static uint8_t img[TDC_APP_LEN];
    esp_app_desc_t desc;

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_init());
    for ( size_t i = 0U; sizeof(img) > i; i++ )
    {
        img[i] = (uint8_t)((i * 13U) + 1U);
    }
    (void)memset(&desc, 0, sizeof(desc));
    desc.magic_word = ESP_APP_DESC_MAGIC_WORD;
    (void)snprintf(desc.project_name, sizeof(desc.project_name), "%s", "onboarding");
    (void)memcpy(&img[KAPP_IDF_DESC_OFF], &desc, sizeof(desc));
    kdrv_apps_write_abort();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write_begin(TDC_APP_OFF, sizeof(img)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write(img, sizeof(img)));
    kdrv_apps_write_abort();

    kapps_dir_t const saved = *kdrv_apps_dir();
    kapps_dir_t       d     = saved;
    d.count = 1U;
    d.e[0]  = (kapps_entry_t){ .offset = TDC_APP_OFF, .size = sizeof(img), .seq = 1U,
                               .flags = KAPPS_ENTRY_VALID };
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&d));
    tdc_sha_hex(img, sizeof(img), hex);
    return saved;
}

/* The line the station sends, at its full length: a reply of present=yes
 * means the console took the 64 digits and the project whole. The digest is
 * a string like the name, so it travels quoted. */
TEST_CASE("test_welcome_already_on_the_board_is_not_armed_and_any_other_digest_arms_it",
          "[console_test_dot]")
{
    char hex[65];
    char line[KODE_CONSOLE_LINE_MAX];
    kapps_dir_t saved = tdc_install_onboarding(hex);

    up(false);
    (void)snprintf(line, sizeof(line), "test_dot_welcome name=\"onboarding.bin\" sha=\"%s\" project=\"onboarding\"", hex);
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, line);
    TEST_ASSERT_TRUE_MESSAGE(replied(">|welcome armed=none present=yes ms="), "the same bytes were armed again");
    TEST_ASSERT_TRUE(replied(">ok"));

    /* Upper case is the same digest. */
    fake_klog_reset();
    char upper[65];
    for ( size_t i = 0U; sizeof(upper) > i; i++ )
    {
        upper[i] = hex[i];
        if ( ('a' <= hex[i]) && ('f' >= hex[i]) )
        {
            upper[i] = (char)(hex[i] - 'a' + 'A');
        }
    }
    (void)snprintf(line, sizeof(line), "test_dot_welcome name=\"onboarding.bin\" sha=\"%s\" project=\"onboarding\"", upper);
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, line);
    TEST_ASSERT_TRUE(replied(">|welcome armed=none present=yes"));

    /* One digit off, one digit short, another project, no project: written. */
    char off[65];
    (void)memcpy(off, hex, sizeof(off));
    off[63] = '0';
    if ( '0' == hex[63] )
    {
        off[63] = '1';
    }
    const char * const others[] = { off, NULL, hex, hex };
    const char * const projects[] = { "onboarding", "onboarding", "metronome", NULL };
    for ( size_t k = 0U; (sizeof(others) / sizeof(others[0])) > k; k++ )
    {
        fake_klog_reset();
        if ( NULL == others[k] )
        {
            (void)snprintf(line, sizeof(line), "test_dot_welcome name=\"onboarding.bin\" sha=\"%.63s\" project=\"onboarding\"", hex);
        }
        else if ( NULL == projects[k] )
        {
            (void)snprintf(line, sizeof(line), "test_dot_welcome name=\"onboarding.bin\" sha=\"%s\"", others[k]);
        }
        else
        {
            (void)snprintf(line, sizeof(line), "test_dot_welcome name=\"onboarding.bin\" sha=\"%s\" project=\"%s\"",
                           others[k], projects[k]);
        }
        type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, line);
        TEST_ASSERT_TRUE_MESSAGE(replied(">|welcome armed=onboarding.bin present=no"), line);
        TEST_ASSERT_TRUE(replied(">ok"));
    }

    /* The storage partition blank: armed, at once. */
    const esp_partition_t * const sto = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                                 ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    TEST_ASSERT_NOT_NULL(sto);
    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_partition_erase_range(sto, 0U, KAPPS_ALIGN));
    fake_klog_reset();
    (void)snprintf(line, sizeof(line), "test_dot_storage name=\"storage.bin\" sha=\"%s\"", hex);
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, line);
    TEST_ASSERT_TRUE(replied(">|storage armed=storage.bin present=no"));

    test_dot_welcome_arm("");
    test_dot_storage_arm("");
    down();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&saved));
}

TEST_CASE("test_storage_already_on_the_board_is_not_armed", "[console_test_dot]")
{
    static uint8_t img[0x200000];
    char hex[65];
    char line[KODE_CONSOLE_LINE_MAX];
    const esp_partition_t * const sto = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                                 ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");

    TEST_ASSERT_NOT_NULL(sto);
    TEST_ASSERT_EQUAL_UINT32(sizeof(img), sto->size);
    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_partition_erase_range(sto, 0U, sto->size));
    (void)memset(img, 0xFF, sizeof(img));
    for ( size_t i = 0U; 0x2000U > i; i++ )
    {
        img[i] = (uint8_t)((i * 3U) + 7U);
    }
    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_partition_write(sto, 0U, img, 0x2000U));
    tdc_sha_hex(img, sizeof(img), hex);

    up(false);
    (void)snprintf(line, sizeof(line), "test_dot_storage name=\"storage.bin\" sha=\"%s\"", hex);
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, line);
    TEST_ASSERT_TRUE(replied(">|storage armed=none present=yes ms="));

    /* One bit cleared in the partition's last byte: written again. */
    uint8_t const z = 0xFEU;
    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_partition_write(sto, sto->size - 1U, &z, 1U));
    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, line);
    TEST_ASSERT_TRUE(replied(">|storage armed=storage.bin present=no"));

    test_dot_storage_arm("");
    down();
    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_partition_erase_range(sto, 0U, sto->size));
}

/* A station that stops before the run takes back what it armed with an
 * empty name; a name left out is still refused. */
TEST_CASE("test_an_empty_name_disarms_welcome_and_storage", "[console_test_dot]")
{
    up(false);
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_welcome name=\"onboarding.bin\"");
    TEST_ASSERT_TRUE(replied(">|welcome armed=onboarding.bin present=no"));
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_storage name=\"storage.bin\"");
    TEST_ASSERT_TRUE(replied(">|storage armed=storage.bin present=no"));
    TEST_ASSERT_EQUAL_STRING("onboarding.bin", test_dot_welcome_armed());
    TEST_ASSERT_EQUAL_STRING("storage.bin", test_dot_storage_armed());

    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_welcome name=\"\"");
    TEST_ASSERT_TRUE(replied(">|welcome armed=none"));
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_EQUAL_STRING("", test_dot_welcome_armed());
    TEST_ASSERT_EQUAL_STRING("storage.bin", test_dot_storage_armed());

    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_storage name=\"\"");
    TEST_ASSERT_TRUE(replied(">|storage armed=none"));
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_EQUAL_STRING("", test_dot_storage_armed());

    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_welcome");
    TEST_ASSERT_TRUE(replied(">err code="));
    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_welcome name=onboarding.bin");
    TEST_ASSERT_TRUE(replied(">err code="));
    TEST_ASSERT_EQUAL_STRING("", test_dot_welcome_armed());
    down();
}

/* The bench's dim panel takes 10 to 100 percent and nothing else. */
TEST_CASE("test_bright_takes_ten_to_a_hundred_percent", "[console_test_dot]")
{
    up(false);
    type_line("test_dot_bright pct=40");
    TEST_ASSERT_TRUE(replied(">|bright pct=40"));
    TEST_ASSERT_TRUE(replied(">ok"));

    fake_klog_reset();
    type_line("test_dot_bright pct=10");
    TEST_ASSERT_TRUE(replied(">|bright pct=10"));
    fake_klog_reset();
    type_line("test_dot_bright pct=100");
    TEST_ASSERT_TRUE(replied(">|bright pct=100"));

    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_bright pct=9");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=pct"));
    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_bright pct=101");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=pct"));
    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_bright");
    TEST_ASSERT_TRUE(replied(">err code="));
    TEST_ASSERT_FALSE(replied(">|bright"));
    down();
}

/* Photos are numbered from one on the wire, as the station asks for
 * them again: there is no photo zero to ask for. */
TEST_CASE("test_photos_only_counts_from_one", "[console_test_dot]")
{
    up(false);
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_photos only=0");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=only"));
    TEST_ASSERT_EQUAL_UINT(0U, requests(NULL));

    up(false);
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_photos only=1");
    TEST_ASSERT_FALSE(replied(">err"));
    TEST_ASSERT_EQUAL_UINT(1U, requests(NULL));
    down();
}
