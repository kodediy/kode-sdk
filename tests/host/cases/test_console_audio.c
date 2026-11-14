/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_audio.c
 * @brief `audio`, `audio_help`, `audio_tone` and the row, from the wire.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "autotest_support.h"
#include "fakes.h"
#include "kaos.h"
#include "kaos_wire.h"
#include "kcli_proto.h"
#include "kdrv_es8311.h"
#include "kes_fields.h"
#include "ktone.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

/* The command watchdog, in QP ticks - repeated rather than imported for the
 * same reason the other console files repeat it. */
#define CMD_TIMEOUT_TICKS    ((5000U * 100U) / 1000U)
/** @brief The audio row's own budget, in whole QP ticks. */
#define AUDIO_BUDGET_TICKS   ((KODE_AUDIO_TEST_BUDGET_MS * 100U) / 1000U)

static void console_up(void)
{
    const QEvt *e;

    qf_support_init();
    autotest_up();
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
    (void)qf_support_tick(AO_Console, AUDIO_BUDGET_TICKS + 1U);

    /* AO_Audio's queue is the mailbox the verbs post into; emptied here so a
     * request left by an earlier case cannot be mistaken for this one's. */
    qf_support_give_queue(AO_Audio);
    while ( xQueueReceive(AO_Audio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
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
    autotest_typed(text);
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

/**
 * @brief Take the field request the console posted to AO_Audio, if any.
 *
 * @param out A copy of it.
 * @return false when nothing was posted.
 */
static bool posted_field(AudioFieldReqEvt *out)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Audio->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(AUDIO_FIELD_REQ_SIG, e->sig);
    *out = *(const AudioFieldReqEvt *)e;
    QF_gc(e);
    return true;
}

/** @brief Was the row asked of AO_Audio? Consumes the request. */
static bool posted_row(void)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Audio->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(AUDIO_TEST_REQ_SIG, e->sig);
    QF_gc(e);
    return true;
}

/** @brief Was a tone asked of AO_Audio? Consumes the request. */
static bool posted_tone(AudioToneEvt *out)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Audio->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(AUDIO_TONE_REQ_SIG, e->sig);
    *out = *(const AudioToneEvt *)e;
    QF_gc(e);
    return true;
}

/** @brief Publish AO_Audio's answer to a field request. */
static void answer_field(uint8_t op, uint16_t field, int32_t value,
                         kode_status_t rc)
{
    AudioFieldEvt a;

    (void)memset(&a, 0, sizeof(a));
    a.super = (QEvt)QEVT_INITIALIZER(AUDIO_FIELD_SIG);
    a.op    = op;
    a.field = field;
    a.value = value;
    a.rc    = (uint8_t)rc;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
}

/** @brief Publish the row's verdict, the way AO_Audio ends it. */
static void verdict(bool ok)
{
    TestResultEvt r;

    (void)memset(&r, 0, sizeof(r));
    r.super = (QEvt)QEVT_INITIALIZER(TEST_RESULT_SIG);
    (void)snprintf(r.name, sizeof(r.name), "%s", "audio");
    r.ok = ok;
    r.origin = TEST_ORIGIN_AUTOTEST;
    QASM_DISPATCH(AO_Autotest, &r.super, 0U);
    QASM_DISPATCH(AO_Console, &r.super, 0U);
    autotest_settle();
}

/** @brief Publish one of the row's step records. */
static void record(uint8_t phase, uint8_t attempt, uint16_t hz, int16_t level,
                   int16_t other, int16_t floor, uint8_t flags)
{
    AudioTestEvt t;

    (void)memset(&t, 0, sizeof(t));
    t.super     = (QEvt)QEVT_INITIALIZER(AUDIO_TEST_SIG);
    t.phase     = phase;
    t.total     = 2U;
    t.tries     = (uint8_t)KODE_AUDIO_TEST_TRIES;
    t.attempt   = attempt;
    t.flags     = flags;
    t.hz        = hz;
    t.level_x10 = level;
    t.other_x10 = other;
    t.floor_x10 = floor;
    t.step      = 2U;
    if ( (AUDIO_TEST_MIC == phase) || (AUDIO_TEST_BIN == phase) )
    {
        t.step = 1U;
    }
    QASM_DISPATCH(AO_Console, &t.super, 0U);
}

TEST_CASE("audio with no keys asks for the whole codec and closes on the answer",
          "[console_audio]")
{
    AudioFieldReqEvt q;

    console_up();
    type_line("audio");
    TEST_ASSERT_TRUE(posted_field(&q));
    TEST_ASSERT_EQUAL_UINT(FIELD_OP_DUMP, q.op);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before AO_Audio answered");

    answer_field(FIELD_OP_DUMP, 0U, 0, KODE_OK);
    TEST_ASSERT_FALSE_MESSAGE(replied(">|chip=audio"),
                              "a dump prints through the log, not as a record");
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("audio field= asks for that field and answers a record",
          "[console_audio]")
{
    AudioFieldReqEvt q;

    console_up();
    type_line("audio field=\"dac_vol_db\"");
    TEST_ASSERT_TRUE(posted_field(&q));
    TEST_ASSERT_EQUAL_UINT(FIELD_OP_GET, q.op);
    TEST_ASSERT_EQUAL_UINT(KDRV_ES_DAC_VOL_DB, q.field);

    answer_field(FIELD_OP_GET, (uint16_t)KDRV_ES_DAC_VOL_DB,
                 KDRV_ES8311_OUT_DB_X10_MAX, KODE_OK);
    TEST_ASSERT_TRUE_MESSAGE(
        replied(">|chip=audio field=\"dac_vol_db\" value=0"),
        "volume 100 reads full scale");
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("audio field= value= asks for a write and answers the readback",
          "[console_audio]")
{
    AudioFieldReqEvt q;

    console_up();
    type_line("audio field=\"mic_db\" value=26");
    TEST_ASSERT_TRUE(posted_field(&q));
    TEST_ASSERT_EQUAL_UINT(FIELD_OP_SET, q.op);
    TEST_ASSERT_EQUAL_UINT(KDRV_ES_MIC_DB, q.field);
    TEST_ASSERT_EQUAL_INT(26, q.value);

    /* The readback, not the echo: this field rounds to a step the chip has. */
    answer_field(FIELD_OP_SET, (uint16_t)KDRV_ES_MIC_DB, 24, KODE_OK);
    TEST_ASSERT_TRUE(replied(">|chip=audio field=\"mic_db\" value=24"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("a field nobody printed is refused before AO_Audio is asked",
          "[console_audio]")
{
    AudioFieldReqEvt q;

    console_up();
    type_line("audio field=\"nope\"");
    TEST_ASSERT_TRUE(replied(">err"));
    TEST_ASSERT_FALSE_MESSAGE(posted_field(&q), "nothing was asked of the owner");
}

TEST_CASE("value without field is a missing key", "[console_audio]")
{
    AudioFieldReqEvt q;

    console_up();
    type_line("audio value=5");
    TEST_ASSERT_TRUE(replied(">err"));
    TEST_ASSERT_FALSE(posted_field(&q));
}

TEST_CASE("a refusal from the owner names the field, a failure names the verb",
          "[console_audio]")
{
    AudioFieldReqEvt q;

    console_up();
    type_line("audio field=\"chd1\" value=1");
    TEST_ASSERT_TRUE(posted_field(&q));
    answer_field(FIELD_OP_SET, (uint16_t)KDRV_ES_CHD1, 0, KODE_DENIED);
    TEST_ASSERT_TRUE_MESSAGE(replied("chd1"),
                             "an identity register is not written");

    fake_klog_reset();
    type_line("audio field=\"reg01\"");
    TEST_ASSERT_TRUE(posted_field(&q));
    answer_field(FIELD_OP_GET, (uint16_t)KDRV_ES_REG01, 0, KODE_IO_ERROR);
    TEST_ASSERT_TRUE_MESSAGE(replied("audio"),
                             "a bus that said no names the verb");
}

TEST_CASE("audio_help lists every field, paced, and closes with ok",
          "[console_audio]")
{
    unsigned remaining;
    unsigned ticks_to_last_slice;

    /* Counted rather than searched: the log fake holds far fewer lines than
     * this table has fields, so the tp_help arrangement is the only honest
     * one - a count over the whole listing, and the text of the first and
     * last fields with the capture window placed over each. */
    console_up();
    type_line("audio_help");
    TEST_ASSERT_TRUE(replied(">|field=\"present\" unit=bool min=0 max=1 rw=ro"));
    TEST_ASSERT_TRUE(replied("desc="));
    TEST_ASSERT_EQUAL_UINT((unsigned)KAOS_WIRE_SLICE, fake_klog_count());

    for ( unsigned i = 0U; (unsigned)KDRV_ES_FIELD_MAX > i; ++i )
    {
        (void)qf_support_tick(AO_Console, 1U);
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)KDRV_ES_FIELD_MAX + 1U,
                                   fake_klog_count(),
                                   "one line per field, and then >ok");

    console_up();
    type_line("audio_help");
    remaining = (unsigned)KDRV_ES_FIELD_MAX - (unsigned)KAOS_WIRE_SLICE;
    ticks_to_last_slice =
        (remaining + (unsigned)KAOS_WIRE_SLICE - 1U) / (unsigned)KAOS_WIRE_SLICE;
    for ( unsigned i = 0U; (ticks_to_last_slice - 1U) > i; ++i )
    {
        (void)qf_support_tick(AO_Console, 1U);
    }
    fake_klog_reset();
    (void)qf_support_tick(AO_Console, 1U);
    TEST_ASSERT_TRUE(replied(">|field=\"chver\""));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("every field has a name and a description", "[console_audio]")
{
    /* The count is a _Static_assert in console_ao.c; this is the other half -
     * a hole left as a NULL by a designated initialiser would pass the count
     * and print nothing on the wire. */
    for ( uint32_t f = 0U; (uint32_t)KDRV_ES_FIELD_MAX > f; ++f )
    {
        const kdrv_es8311_field_info_t * const info =
            kdrv_es8311_field_info((kdrv_es8311_field_t)f);
        kdrv_es8311_field_t back = KDRV_ES_FIELD_MAX;

        TEST_ASSERT_NOT_NULL(info);
        TEST_ASSERT_NOT_NULL(info->name);
        TEST_ASSERT_NOT_NULL(info->unit);
        TEST_ASSERT_TRUE_MESSAGE('\0' != info->name[0], "a field has no name");
        TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK,
            kdrv_es8311_field_by_name(info->name, &back),
            "a field the table prints cannot be typed back at it");
        TEST_ASSERT_EQUAL_UINT(f, (uint32_t)back);
    }
}

TEST_CASE("a tone below the board's own floor is refused", "[console_audio]")
{
    AudioToneEvt t;

    console_up();
    type_line("audio_tone freq=399");
    TEST_ASSERT_TRUE(replied(">err"));
    TEST_ASSERT_FALSE_MESSAGE(posted_tone(&t), "nothing was asked of the owner");
}

TEST_CASE("a tone reports the frequency it will actually emit",
          "[console_audio]")
{
    /* Snapped, and the SNAPPED value comes back: a bench that asks for 1500
     * and measures 1535 on a scope must not think the firmware lied. */
    AudioToneEvt t;
    char         want[32];

    console_up();
    type_line("audio_tone freq=1500 ms=300 amp=25");
    TEST_ASSERT_TRUE(posted_tone(&t));
    /* Snapped before it is posted, so the owner and the record cannot end up
     * carrying two different frequencies. */
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(ktone_bin_hz(ktone_bin_nearest(1500U)),
                                     t.hz, "the owner is asked for the snapped one");
    TEST_ASSERT_EQUAL_UINT16(300U, t.ms);
    TEST_ASSERT_EQUAL_UINT8(25U, t.amp_pct);

    (void)snprintf(want, sizeof(want), ">|tone=%u", (unsigned)t.hz);
    TEST_ASSERT_TRUE(replied(want));
    TEST_ASSERT_FALSE_MESSAGE(replied(">|tone=1500"),
                              "what was typed is not what is emitted");
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("a bare tone takes the board's validated reference frequency",
          "[console_audio]")
{
    AudioToneEvt t;

    console_up();
    type_line("audio_tone");
    TEST_ASSERT_TRUE(posted_tone(&t));
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(ktone_bin_hz(0U), t.hz,
                                     "the default is the lowest candidate");
    TEST_ASSERT_EQUAL_UINT16(300U, t.ms);
    TEST_ASSERT_EQUAL_UINT8(25U, t.amp_pct);
    TEST_ASSERT_TRUE(replied(">|tone=1020 ms=300 amp=25"));

    /* A_AUDIO_TONE prints "1020" as the default `freq=`, and this is what
     * stops that literal drifting away from the frequency the handler
     * actually applies. */
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(1020U, ktone_bin_hz(0U),
        "A_AUDIO_TONE's printed default is no longer the lowest candidate");
}

TEST_CASE("test component=audio asks AO_Audio and closes on its verdict",
          "[console_audio]")
{
    console_up();
    type_line("test component=audio");
    TEST_ASSERT_TRUE(posted_row());
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before AO_Audio answered");

    verdict(true);
    TEST_ASSERT_TRUE(replied(">|test=audio result=pass"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("the audio row waits its own budget before it is a timeout",
          "[console_audio]")
{
    console_up();
    type_line("test component=audio");
    TEST_ASSERT_TRUE(posted_row());

    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS + 1U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">|test=audio"),
                              "the row has longer than a chip's five seconds");
    TEST_ASSERT_FALSE(replied(">err code=6"));

    (void)qf_support_tick(AO_Console, AUDIO_BUDGET_TICKS);
    TEST_ASSERT_TRUE(replied(">|test=audio result=timeout"));
    TEST_ASSERT_TRUE_MESSAGE(replied(">ok"),
                             "a row's silence is a record, not the end of it");
}

TEST_CASE("every record shape the row prints is the documented one",
          "[console_audio]")
{
    console_up();
    type_line("test component=audio");
    TEST_ASSERT_TRUE(posted_row());

    record(AUDIO_TEST_MIC, 0U, 0U, -624, 0, -401, AUDIO_TF_PASS);
    TEST_ASSERT_TRUE(replied(">|step=1 of=2 what=mic rms=-624 peak=-401"));
    TEST_ASSERT_TRUE(replied("stuck=0 clip=0 loop=0 result=pass"));

    record(AUDIO_TEST_BIN, 0U, 1020U, -812, 0, 0, 0U);
    TEST_ASSERT_TRUE(replied(">|step=1 bin=1020 level=-812"));

    record(AUDIO_TEST_TONE, 1U, 1535U, 0, 0, 0, 0U);
    TEST_ASSERT_TRUE(replied(">|step=2 of=2 attempt=1 of_tries=3 tone=1535"));

    record(AUDIO_TEST_PROBE, 1U, 1535U, -311, -702, -798, AUDIO_TF_PASS);
    TEST_ASSERT_TRUE(replied(
        ">|step=2 attempt=1 tone=1535 level=-311 other=-702 floor=-798 result=pass"));

    /* And a failure says so on the record, not only in the verdict. */
    record(AUDIO_TEST_PROBE, 2U, 1020U, -790, -800, -812, 0U);
    TEST_ASSERT_TRUE(replied("attempt=2 tone=1020 level=-790"));
    TEST_ASSERT_TRUE(replied("result=fail"));

    verdict(false);
    TEST_ASSERT_TRUE(replied(">|test=audio result=fail"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("a record arriving with no row in flight prints nothing",
          "[console_audio]")
{
    console_up();
    record(AUDIO_TEST_MIC, 0U, 0U, -624, 0, -401, AUDIO_TF_PASS);
    TEST_ASSERT_FALSE_MESSAGE(replied(">|step="),
                              "nobody asked for that row");
}

/* ---- audio_rec: the verb the bench calibrates with -------------------------
 * `test component=audio` prints the floor as numbers, and numbers are enough
 * to judge a board. Setting the thresholds those numbers are judged against
 * needs the waveform itself, on a host - which is what this verb is for.
 */

/** @brief Take the recording request the console posted to AO_Audio. */
static bool posted_rec(AudioFileReqEvt *out)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Audio->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(AUDIO_REC_FILE_REQ_SIG, e->sig);
    *out = *(const AudioFileReqEvt *)e;
    QF_gc(e);
    return true;
}

/** @brief Publish the outcome, the way AO_Audio ends a recording. */
static void rec_done(const char *path, kode_status_t rc, uint32_t bytes,
                     uint32_t ms)
{
    AudioFileRspEvt a;

    (void)memset(&a, 0, sizeof(a));
    a.super = (QEvt)QEVT_INITIALIZER(AUDIO_FILE_RSP_SIG);
    (void)snprintf(a.path, sizeof(a.path), "%s", path);
    a.rc    = (uint8_t)rc;
    a.bytes = bytes;
    a.ms    = ms;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
}

TEST_CASE("audio_rec resolves the path here and hands it over resolved",
          "[console_audio]")
{
    /* AO_Audio owns the codec, not the current directory: a relative path
     * reaching it would be a path it has no way to resolve. */
    AudioFileReqEvt q;

    console_up();
    type_line("audio_rec ms=3000 file=\"/storage/mic.wav\"");
    TEST_ASSERT_TRUE(posted_rec(&q));
    TEST_ASSERT_EQUAL_STRING("/storage/mic.wav", q.path);
    TEST_ASSERT_EQUAL_UINT32(3000U, q.ms);
    TEST_ASSERT_NULL_MESSAGE(q.done,
        "the console does not sleep on anything; it closes on the answer");
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before AO_Audio answered");

    rec_done("/storage/mic.wav", KODE_OK, 288044U, 3000U);
    TEST_ASSERT_TRUE(replied(">|rec=\"/storage/mic.wav\" ms=3000 bytes=288044"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("audio_rec without a file is refused: it would duplicate the row",
          "[console_audio]")
{
    AudioFileReqEvt q;

    console_up();
    type_line("audio_rec ms=1000");
    TEST_ASSERT_TRUE(replied(">err"));
    TEST_ASSERT_FALSE(posted_rec(&q));
}

TEST_CASE("a recording longer than the buffer is refused, not cut",
          "[console_audio]")
{
    AudioFileReqEvt q;
    char            line[64];

    console_up();
    (void)snprintf(line, sizeof(line), "audio_rec ms=%u file=\"/storage/x.wav\"",
                   (unsigned)(KDRV_ES8311_BUF_SECONDS * 1000U) + 1U);
    type_line(line);
    TEST_ASSERT_TRUE(replied(">err"));
    TEST_ASSERT_FALSE(posted_rec(&q));
}

TEST_CASE("a medium that would not take the recording names the verb",
          "[console_audio]")
{
    AudioFileReqEvt q;

    console_up();
    type_line("audio_rec file=\"/storage/mic.wav\"");
    TEST_ASSERT_TRUE(posted_rec(&q));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1000U, q.ms, "the default recording");

    rec_done("/storage/mic.wav", KODE_IO_ERROR, 0U, 0U);
    TEST_ASSERT_TRUE(replied("audio_rec"));
    TEST_ASSERT_FALSE_MESSAGE(replied(">|rec="),
                              "nothing landed, so nothing is reported as landed");
}
