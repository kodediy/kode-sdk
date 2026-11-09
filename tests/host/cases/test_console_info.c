/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_info.c
 * @brief `info`: the machine itself, and what is programmed where.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "qpc.h"
#include "Mockkdrv_slot.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kcli_proto.h"
#include "kdrv_c5.h"
#include "kdrv_c5svc.h"
#include "kode_wire.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

static void console_up(void)
{
    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();

    owners_up();
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
    fake_klog_reset();
    fake_neighbours_reset();
}

static void type_line(const char *text)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = (uint8_t)KODE_CONSOLE_SRC_SERIAL;
    (void)snprintf(e.line, sizeof(e.line), "%s", text);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

static bool replied(const char *needle)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            return true;
        }
    }
    return false;
}

/** @brief The index of the first reply containing @p needle, or -1. */
static int reply_index(const char *needle)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            return (int)i;
        }
    }
    return -1;
}

/** @brief What kdrv_slot answers for one partition on the next ask. */
static void expect_slot(kdrv_slot_info_t *info, const char *label,
                        uint32_t address, bool running, const char *project,
                        const char *version, const char *idf)
{
    (void)memset(info, 0, sizeof(*info));
    info->label   = label;
    info->address = address;
    info->size    = 0x400000U;
    info->running = running;
    if ( NULL != project )
    {
        info->has_image = true;
        (void)snprintf(info->desc.project, sizeof(info->desc.project), "%s", project);
        (void)snprintf(info->desc.version, sizeof(info->desc.version), "%s", version);
        (void)snprintf(info->desc.idf_ver, sizeof(info->desc.idf_ver), "%s", idf);
    }
    kdrv_slot_info_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_slot_info_ReturnThruPtr_out(info);
}

/** @brief What kdrv_slot_coredump answers on the next ask, for a case that
 *         cares about the row it produces. */
static void expect_coredump(kdrv_coredump_t *cd, bool present, bool mine,
                            const char *task, uint32_t pc)
{
    (void)memset(cd, 0, sizeof(*cd));
    cd->present = present;
    cd->mine    = mine;
    if ( NULL != task )
    {
        (void)snprintf(cd->task, sizeof(cd->task), "%s", task);
    }
    cd->pc = pc;
    kdrv_slot_coredump_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_slot_coredump_ReturnThruPtr_out(cd);
}

TEST_CASE("info prints one record per kodeOS partition after the identity, "
          "and says which one is running", "[console_info]")
{
    static kdrv_slot_info_t slots[3];

    console_up();
    Mockkdrv_slot_Init();
    expect_slot(&slots[0], "factory", 0x20000U, false, "kodeos", "00.00.17", "v6.1");
    expect_slot(&slots[1], "ota_0", 0x420000U, true, "kodeos", "00.00.18", "v6.1");
    expect_slot(&slots[2], "ota_1", 0x820000U, false, NULL, NULL, NULL);
    /* Not this case's concern - see the coredump-specific cases below. */
    kdrv_slot_coredump_IgnoreAndReturn(KODE_DENIED);
    kdrv_c5_forget();

    type_line("info");

    TEST_ASSERT_TRUE(replied(">|slot=factory addr=0x020000 size_kb=4096 "
                             "state=inactive project=\"kodeos\" "
                             "version=\"00.00.17\" idf=\"v6.1\""));
    TEST_ASSERT_TRUE(replied(">|slot=ota_0 addr=0x420000 size_kb=4096 "
                             "state=running project=\"kodeos\" "
                             "version=\"00.00.18\" idf=\"v6.1\""));
    TEST_ASSERT_TRUE_MESSAGE(replied(">|slot=ota_1 addr=0x820000 size_kb=4096 "
                                     "state=empty project=- version=- idf=-"),
        "an empty slot is an answer, with dashes where the image would be");
    TEST_ASSERT_TRUE_MESSAGE(reply_index(">|serial=") < reply_index(">|slot=factory"),
        "the machine first, then what it holds");
    TEST_ASSERT_TRUE(reply_index(">|slot=ota_1") < reply_index(">ok"));

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("a partition the table does not carry is reported absent",
          "[console_info]")
{
    console_up();
    Mockkdrv_slot_Init();
    kdrv_slot_info_IgnoreAndReturn(KODE_DENIED);
    kdrv_slot_coredump_IgnoreAndReturn(KODE_DENIED);
    kdrv_c5_forget();

    type_line("info");

    TEST_ASSERT_TRUE(replied(">|slot=factory addr=- size_kb=- state=absent "
                             "project=- version=- idf=-"));
    TEST_ASSERT_TRUE(replied(">|slot=ota_1 addr=- size_kb=- state=absent"));
    TEST_ASSERT_TRUE(replied(">ok"));

    Mockkdrv_slot_Destroy();
}

/* The console's own level, so a host that sees nothing can ask why. Stored
 * is whatever differs from the build's level: AO_Log keeps the key only
 * then, so the comparison is the whole answer. */
TEST_CASE("info says the console's log level and whether it was stored",
          "[console_info]")
{
    console_up();
    Mockkdrv_slot_Init();
    kdrv_slot_info_IgnoreAndReturn(KODE_DENIED);
    kdrv_slot_coredump_IgnoreAndReturn(KODE_DENIED);
    kdrv_c5_forget();

    kode_log_set_level(NULL, kode_log_build_level());
    type_line("info");
    TEST_ASSERT_TRUE(replied(">|log=i source=default"));

    fake_klog_reset();
    kode_log_set_level(NULL, KODE_LOG_NOTICE);
    type_line("info");
    TEST_ASSERT_TRUE(replied(">|log=n source=stored"));

    fake_klog_reset();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("info says nothing is waiting when the coredump partition is empty",
          "[console_info]")
{
    kdrv_coredump_t cd;

    console_up();
    Mockkdrv_slot_Init();
    kdrv_slot_info_IgnoreAndReturn(KODE_DENIED);
    expect_coredump(&cd, false, false, NULL, 0U);
    kdrv_c5_forget();

    type_line("info");

    TEST_ASSERT_TRUE(replied(">|coredump=none"));

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("info names the crashed task and whether the dump is kodeOS's own",
          "[console_info]")
{
    kdrv_coredump_t cd;

    console_up();
    Mockkdrv_slot_Init();
    kdrv_slot_info_IgnoreAndReturn(KODE_DENIED);
    expect_coredump(&cd, true, true, "AO_Console", 0x40001234U);
    kdrv_c5_forget();

    type_line("info");

    TEST_ASSERT_TRUE(replied(">|coredump=present owner=kodeos "
                             "task=\"AO_Console\" pc=0x40001234"));

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("info reports an undecodable dump as present but unclaimed, with "
          "nothing else known", "[console_info]")
{
    kdrv_coredump_t cd;

    console_up();
    Mockkdrv_slot_Init();
    kdrv_slot_info_IgnoreAndReturn(KODE_DENIED);
    expect_coredump(&cd, true, false, NULL, 0U);
    kdrv_c5_forget();

    type_line("info");

    TEST_ASSERT_TRUE(replied(">|coredump=present owner=other task=- pc=-"));

    Mockkdrv_slot_Verify();
    Mockkdrv_slot_Destroy();
}

TEST_CASE("info prints what the coprocessor's snapshot says, and how the P4 "
          "recognises it", "[console_info]")
{
    console_up();
    Mockkdrv_slot_Init();
    kdrv_slot_info_IgnoreAndReturn(KODE_DENIED);
    kdrv_slot_coredump_IgnoreAndReturn(KODE_DENIED);

    /* A Kode build of the slave, as the last bring-up saw it. */
    fake_hosted_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());
    type_line("info");
    TEST_ASSERT_TRUE(replied(">|c5=running project=\"kode_c5\" "
                             "version=\"2.12.9\" idf=\"v5.5.1\" recog=kode"));
    TEST_ASSERT_TRUE(reply_index(">|slot=ota_1") < reply_index(">|c5=running"));
    TEST_ASSERT_TRUE(reply_index(">|c5=running") < reply_index(">ok"));

    /* Stock esp-hosted: describes itself, but not as ours. */
    fake_klog_reset();
    fake_hosted_set_project("network_adapter");
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());
    type_line("info");
    TEST_ASSERT_TRUE(replied(">|c5=running project=\"network_adapter\" "
                             "version=\"2.12.9\" idf=\"v5.5.1\" recog=stock"));

    /* An older slave: no descriptor RPC at all. */
    fake_klog_reset();
    fake_hosted_app_desc_unsupported(true);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());
    type_line("info");
    TEST_ASSERT_TRUE(replied(">|c5=running project=- version=- idf=- recog=old"));

    /* No link this boot: nothing to report, and said so. */
    fake_klog_reset();
    kdrv_c5_forget();
    type_line("info");
    TEST_ASSERT_TRUE(replied(">|c5=running project=- version=- idf=- "
                             "recog=nolink power=running"));
    TEST_ASSERT_TRUE(replied(">ok"));

    /* And the C5's own reset line, which is not the link: a C5 the board is
     * holding in reset is off, and the record says so. */
    fake_klog_reset();
    fake_radio_set_c5_held(true);
    type_line("info");
    TEST_ASSERT_TRUE(replied(">|c5=running project=- version=- idf=- "
                             "recog=nolink power=held"));
    fake_radio_set_c5_held(false);

    Mockkdrv_slot_Destroy();
}

/** @brief The C5 answers an INFO with these counts. */
static void c5_info(uint8_t vault, uint8_t btkeys, uint8_t flags)
{
    fake_radio_set_link(true);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5svc_init());
    kdrv_c5svc_forget();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_c5svc_send(KODE_SVC_SELF, KODE_SELF_INFO, NULL, 0U));
    const kode_hdr_t *const req = (const kode_hdr_t *)fake_peer_last_sent(NULL);
    uint8_t m[sizeof(kode_hdr_t) + sizeof(kode_info_t)];
    kode_hdr_t const h = { KODE_WIRE_VERSION, KODE_SVC_SELF, KODE_SELF_INFO,
                           req->seq, KODE_ST_OK, 0U,
                           (uint16_t)sizeof(kode_info_t) };
    kode_info_t i;
    (void)memset(&i, 0, sizeof(i));
    i.wire_version = KODE_WIRE_VERSION;
    i.vault_count  = vault;
    i.btkeys_count = btkeys;
    i.id_flags     = flags;
    (void)memcpy(m, &h, sizeof(h));
    (void)memcpy(&m[sizeof(h)], &i, sizeof(i));
    fake_peer_deliver(KODE_MSG_RSP, m, sizeof(m));
}

TEST_CASE("info's c5 row says what the KODE services hold, or - when unknown",
          "[console_info]")
{
    console_up();
    Mockkdrv_slot_Init();
    kdrv_slot_info_IgnoreAndReturn(KODE_DENIED);
    kdrv_slot_coredump_IgnoreAndReturn(KODE_DENIED);
    fake_hosted_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());

    c5_info(2U, 1U, (uint8_t)(KODE_ID_F_KEY | KODE_ID_F_CERT));
    fake_klog_reset();
    type_line("info");
    char want[64];
    (void)snprintf(want, sizeof(want), "power=running wire=%u vault=2 btkeys=1 id=cert",
                   (unsigned)KODE_WIRE_VERSION);
    TEST_ASSERT_TRUE(replied(want));

    c5_info(0U, 0U, (uint8_t)(KODE_ID_F_KEY | KODE_ID_F_CERT | KODE_ID_F_LOCKED));
    fake_klog_reset();
    type_line("info");
    (void)snprintf(want, sizeof(want), "wire=%u vault=0 btkeys=0 id=locked",
                   (unsigned)KODE_WIRE_VERSION);
    TEST_ASSERT_TRUE(replied(want));

    c5_info(0U, 0U, 0U);
    fake_klog_reset();
    type_line("info");
    TEST_ASSERT_TRUE(replied("id=none"));

    kdrv_c5svc_forget();
    fake_klog_reset();
    type_line("info");
    TEST_ASSERT_TRUE(replied("power=running wire=- vault=- btkeys=- id=-"));

    Mockkdrv_slot_Destroy();
}

TEST_CASE("info ends with the die temperature, and a dash when the sensor "
          "does not answer", "[console_info]")
{
    console_up();
    Mockkdrv_slot_Init();
    kdrv_slot_info_IgnoreAndReturn(KODE_DENIED);
    kdrv_slot_coredump_IgnoreAndReturn(KODE_DENIED);
    kdrv_c5_forget();

    type_line("info");
    TEST_ASSERT_TRUE(replied(">|temp=p4 c=41.5"));
    TEST_ASSERT_TRUE(reply_index(">|log=") < reply_index(">|temp=p4"));
    TEST_ASSERT_TRUE(reply_index(">|temp=p4") < reply_index(">ok"));

    fake_temp_set(KODE_ERROR, 0);
    fake_klog_reset();
    type_line("info");
    TEST_ASSERT_TRUE_MESSAGE(replied(">|temp=p4 c=-"),
        "a sensor that does not answer is a dash, not a failed info");
    TEST_ASSERT_TRUE(replied(">ok"));

    Mockkdrv_slot_Destroy();
}

/* ---- boot_info ---------------------------------------------------------------- */

/** @brief Where the running partition starts in the answers below. */
#define BOOT_ANSWER_ADDRESS     0x10000U

/** @brief How big that partition is in the answers below, bytes. */
#define BOOT_ANSWER_SIZE        0x100000U

/** @brief An app_end byte no kboot_app_end_t has. */
#define BOOT_ANSWER_APP_END_BAD 99U

/** @brief Empty AO_Boot's mailbox: boot_info posts its request there. */
static void boot_mailbox_drain(void)
{
    const QEvt *e = NULL;

    qf_support_give_queue(AO_Boot);
    while ( xQueueReceive(AO_Boot->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/**
 * @brief Hand the console the event AO_Boot answers boot_info with.
 *
 * @param reason  A kboot_reason_t.
 * @param app_end The byte BootStateEvt.app_end carries.
 */
static void boot_answers(kboot_reason_t reason, uint8_t app_end)
{
    BootStateEvt b;

    (void)memset(&b, 0, sizeof(b));
    b.super = (QEvt)QEVT_INITIALIZER(BOOT_STATE_SIG);
    (void)snprintf(b.running, sizeof(b.running), "%s", "factory");
    b.address       = BOOT_ANSWER_ADDRESS;
    b.size          = BOOT_ANSWER_SIZE;
    b.reason        = (uint8_t)reason;
    b.otadata_blank = true;
    b.app_end       = app_end;
    QASM_DISPATCH(AO_Console, &b.super, 0U);
}

/**
 * @brief Whether the first reply containing @p needle ends with @p tail.
 *
 * The capture keeps the newline the console's lines end with; it is not part
 * of what the line says, so it is not part of what it ends with.
 *
 * @param needle What picks the line.
 * @param tail   How the line must end.
 * @return true when the line exists and ends that way.
 */
static bool reply_ends_with(const char *needle, const char *tail)
{
    int const i = reply_index(needle);

    if ( 0 > i )
    {
        return false;
    }
    const char * const line = fake_klog_line((unsigned)i);
    size_t             n    = strlen(line);
    size_t const       t    = strlen(tail);

    if ( (0U < n) && ('\n' == line[n - 1U]) )
    {
        --n;
    }
    return (n >= t) && (strncmp(&line[n - t], tail, t) == 0);
}

/**
 * @brief Whether a reply containing @p first comes before one containing
 *        @p second.
 *
 * A reply that is missing is no order at all: both must be there.
 *
 * @param first  What picks the earlier line.
 * @param second What picks the later line.
 * @return true when both exist and @p first is the earlier.
 */
static bool reply_before(const char *first, const char *second)
{
    int const a = reply_index(first);
    int const b = reply_index(second);

    return (0 <= a) && (0 <= b) && (a < b);
}

TEST_CASE("boot_info ends its record with how the app before this boot ended",
          "[console_info]")
{
    console_up();
    boot_mailbox_drain();

    /* The whole line: every field it had before is still there, in its
     * place, and the new one is the last. */
    type_line("boot_info");
    boot_answers(KBOOT_COLD, (uint8_t)KBOOT_APP_END_NONE);
    TEST_ASSERT_TRUE(replied(">|running=factory addr=0x010000 size_kb=1024 "
                             "otadata=blank reason=cold app_end=none"));
    TEST_ASSERT_TRUE(reply_ends_with(">|running=", " app_end=none"));
    TEST_ASSERT_TRUE_MESSAGE(reply_before(">|running=", ">ok"),
                             "the record comes before the >ok, and is there");

    fake_klog_reset();
    type_line("boot_info");
    boot_answers(KBOOT_FROM_APP, (uint8_t)KBOOT_APP_END_PANIC);
    TEST_ASSERT_TRUE(reply_ends_with(">|running=", "reason=app app_end=panic"));

    fake_klog_reset();
    type_line("boot_info");
    boot_answers(KBOOT_FROM_APP, (uint8_t)KBOOT_APP_END_CLEAN);
    TEST_ASSERT_TRUE(reply_ends_with(">|running=", "reason=app app_end=clean"));

    fake_klog_reset();
    type_line("boot_info");
    boot_answers(KBOOT_FROM_APP, (uint8_t)KBOOT_APP_END_WATCHDOG);
    TEST_ASSERT_TRUE(reply_ends_with(">|running=", " app_end=watchdog"));

    fake_klog_reset();
    type_line("boot_info");
    boot_answers(KBOOT_FROM_APP, (uint8_t)KBOOT_APP_END_BROWNOUT);
    TEST_ASSERT_TRUE(reply_ends_with(">|running=", " app_end=brownout"));

    boot_mailbox_drain();
}

TEST_CASE("boot_info says ? for an app_end that is no class", "[console_info]")
{
    console_up();
    boot_mailbox_drain();

    type_line("boot_info");
    boot_answers(KBOOT_FROM_APP, (uint8_t)BOOT_ANSWER_APP_END_BAD);
    TEST_ASSERT_TRUE(reply_ends_with(">|running=", " app_end=?"));
    TEST_ASSERT_TRUE(reply_before(">|running=", ">ok"));

    boot_mailbox_drain();
}
