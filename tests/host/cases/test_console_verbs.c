/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_verbs.c
 * @brief The owners' verbs registry: what an unlinked owner leaves, two owners side by side, and a full registry.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "qpc.h"

#include "Mockkdrv_slot.h"

#include "ao_spy.h"
#include "ask_support.h"
#include "autotest_support.h"
#include "fakes.h"
#include "kaos.h"
#include "kcli_proto.h"
#include "kqp.h"
#include "console_verbs.h"
#include "kverbs.h"
#include "owners_support.h"
#include "qf_support.h"

#define VERB_WORD_MAX   8U    /**< Room for a made-up verb's name. */
#define HELP_STEPS_MAX  100U  /**< Ticks enough for the whole `help` listing. */
/** @brief Past the console's watchdog and the longest one test row's budget
 *         (the C5's, KODE_C5_TEST_BUDGET_MS, under 90 s) with room to spare:
 *         whatever an earlier file left in flight has expired by then. */
#define ANY_BUDGET_MS     200000U
#define ANY_BUDGET_TICKS  QF_SUPPORT_STEPS(ANY_BUDGET_MS)  /**< ANY_BUDGET_MS in ticks. */
/** @brief One step past the console's watchdog. */
#define WATCHDOG_TICKS    (QF_SUPPORT_STEPS((uint32_t)CONSOLE_CMD_TIMEOUT_MS_N) + 1U)

static void console_up(void)
{
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
    (void)qf_support_tick(AO_Console, ANY_BUDGET_TICKS);
    fake_klog_reset();
    fake_neighbours_reset();
}

/** @brief Leave the registry as the owners' starts leave it, for the next case and file. */
static void owners_back(void)
{
    kverbs_reset();
    owners_up();
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
    bool found = false;

    for ( uint32_t i = 0U; (uint32_t)fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            found = true;
        }
    }
    return found;
}

/* ---- Two made-up owners ---------------------------------------------------- */

static const char *s_ran;  /**< The verb the last handler ran for. */

static void h_made_up(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    s_ran = c->verb;
    console_reply(">ok");
}

static const kverb_t A_VERBS[] = {
    { "zz_a1", NULL, 0U, h_made_up },
    { "zz_a2", NULL, 0U, h_made_up },
};
static const kverbs_t A_TABLE = { .verbs = A_VERBS, .n = 2U };

static const kverb_t B_VERBS[] = {
    { "zz_b1", NULL, 0U, h_made_up },
};
static const kverbs_t B_TABLE = { .verbs = B_VERBS, .n = 1U };

/** @brief A third owner that names one of A's verbs. */
static const kverb_t C_VERBS[] = {
    { "zz_c1", NULL, 0U, h_made_up },
    { "zz_a2", NULL, 0U, h_made_up },
};
static const kverbs_t C_TABLE = { .verbs = C_VERBS, .n = 2U };

/** @brief An owner that names one of the console's own verbs. */
static const kverb_t D_VERBS[] = {
    { "ping", NULL, 0U, h_made_up },
};
static const kverbs_t D_TABLE = { .verbs = D_VERBS, .n = 1U };

TEST_CASE("a verb whose owner was not started is answered as unknown",
          "[console_verbs]")
{
    console_up();
    kverbs_reset();

    type_line("tp");
    TEST_ASSERT_TRUE(replied(">err code=1 msg=tp"));
    fake_klog_reset();
    type_line("wifi_info");
    TEST_ASSERT_TRUE(replied(">err code=1 msg=wifi_info"));
    fake_klog_reset();
    type_line("no_such_verb");
    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=1 msg=no_such_verb"),
        "the same answer a verb nobody has gets");
    fake_klog_reset();
    type_line("ping");
    TEST_ASSERT_TRUE_MESSAGE(replied(">ok"), "the console's own verbs stay");

    /* Its owner's start registers it, and the verb answers. */
    kaos_touch_register();
    fake_klog_reset();
    type_line("tp_help");
    TEST_ASSERT_FALSE(replied(">err code=1"));
    TEST_ASSERT_TRUE(replied(">|field="));
    (void)qf_support_tick(AO_Console, HELP_STEPS_MAX);   /* the rest of its slices */
    owners_back();
}

TEST_CASE("two owners registering do not collide", "[console_verbs]")
{
    console_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kverbs_add(&A_TABLE));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kverbs_add(&B_TABLE));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kverbs_add(&A_TABLE),
        "the same owner twice is one registration");

    const kverbs_t *owner = NULL;
    TEST_ASSERT_EQUAL_PTR(&A_VERBS[1], kverbs_find("zz_a2", &owner));
    TEST_ASSERT_EQUAL_PTR(&A_TABLE, owner);
    TEST_ASSERT_EQUAL_PTR(&B_VERBS[0], kverbs_find("zz_b1", &owner));
    TEST_ASSERT_EQUAL_PTR(&B_TABLE, owner);
    TEST_ASSERT_NOT_NULL_MESSAGE(kverbs_find("tp", NULL), "a real owner's verbs stay");

    s_ran = NULL;
    type_line("zz_a1");
    TEST_ASSERT_EQUAL_STRING("zz_a1", s_ran);
    type_line("zz_b1");
    TEST_ASSERT_EQUAL_STRING("zz_b1", s_ran);

    /* An owner that would take a verb another holds is refused whole. */
    size_t const before = kverbs_count();
    fake_klog_reset();
    TEST_ASSERT_EQUAL_INT(KODE_EXISTS, kverbs_add(&C_TABLE));
    TEST_ASSERT_NULL(kverbs_find("zz_c1", NULL));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)before, (uint32_t)kverbs_count());
    TEST_ASSERT_TRUE(replied("'zz_a2' is already a verb"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_EXISTS, kverbs_add(&D_TABLE),
        "nor may one take the console's own");
    owners_back();
}

TEST_CASE("a full registry refuses the next owner with a log line",
          "[console_verbs]")
{
    static char           names[KVERBS_TABLES_MAX + 1U][VERB_WORD_MAX];
    static kverb_t        rows[KVERBS_TABLES_MAX + 1U];
    static kverbs_t       tables[KVERBS_TABLES_MAX + 1U];

    console_up();
    kverbs_reset();
    for ( size_t i = 0U; KVERBS_TABLES_MAX >= i; ++i )
    {
        (void)snprintf(names[i], sizeof(names[i]), "ov%" PRIu32, (uint32_t)i);
        rows[i].verb   = names[i];
        rows[i].args   = NULL;
        rows[i].n_args = 0U;
        rows[i].fn     = h_made_up;
        tables[i].verbs = &rows[i];
        tables[i].n     = 1U;
    }
    for ( size_t i = 0U; KVERBS_TABLES_MAX > i; ++i )
    {
        TEST_ASSERT_EQUAL_INT(KODE_OK, kverbs_add(&tables[i]));
    }
    fake_klog_reset();
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES,
                          kverbs_add(&tables[KVERBS_TABLES_MAX]));
    TEST_ASSERT_TRUE(replied("no table slot left"));
    TEST_ASSERT_NULL(kverbs_find(names[KVERBS_TABLES_MAX], NULL));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KVERBS_TABLES_MAX, (uint32_t)kverbs_count());

    /* What was registered still answers. */
    s_ran = NULL;
    type_line("ov0");
    TEST_ASSERT_EQUAL_STRING("ov0", s_ran);
    owners_back();
}

/** @brief Where @p needle came in a listing read a window at a time; 0 when it never did. */
typedef struct {
    const char *needle;
    uint32_t    at;
} seen_t;

TEST_CASE("help lists the console's verbs, then the owners'", "[console_verbs]")
{
    seen_t want[] = {
        { ">|cmd=ping args=-", 0U },
        { ">|cmd=tp args=field:string:opt,value:uint:opt", 0U },
        { ">|cmd=zz_b1 args=-", 0U },
    };
    /* Every numeric default bare, as a host types it back: no `U`. */
    seen_t bare[] = {
        { ">|cmd=audio_rec args=ms:uint:opt=1000,", 0U },
        { "ms:uint:opt=300,amp:uint:opt=25", 0U },
        { ">|cmd=haptic_play args=ms:uint:opt=200,gain:uint:opt=50", 0U },
        { ">|cmd=rfid_read args=ms:uint:opt=2000", 0U },
        { ">|cmd=nfc_read args=ms:uint:opt=1500", 0U },
        { "max:uint:opt=4096", 0U },
        { "hz:uint:opt=1000,duty:uint:opt=500", 0U },
    };
    uint32_t line = 0U;
    bool     done = false;

    console_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kverbs_add(&B_TABLE));
    type_line("help");
    /* The window keeps 80 lines; the listing is longer, so it is read a
     * slice at a time. */
    for ( uint32_t step = 0U; (HELP_STEPS_MAX > step) && !done; ++step )
    {
        for ( uint32_t i = 0U; ((uint32_t)fake_klog_count() > i) && !done; ++i )
        {
            const char * const l = fake_klog_line(i);

            line += 1U;
            for ( size_t w = 0U; (sizeof(want) / sizeof(want[0])) > w; ++w )
            {
                if ( (NULL != l) && (strstr(l, want[w].needle) != NULL) )
                {
                    want[w].at = line;
                }
            }
            for ( size_t w = 0U; (sizeof(bare) / sizeof(bare[0])) > w; ++w )
            {
                if ( (NULL != l) && (strstr(l, bare[w].needle) != NULL) )
                {
                    bare[w].at = line;
                }
            }
            done = (NULL != l) && (strstr(l, ">ok") != NULL);
        }
        fake_klog_reset();
        (void)qf_support_tick(AO_Console, 1U);
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_NOT_EQUAL(0U, want[0].at);
    TEST_ASSERT_TRUE(want[1].at > want[0].at);
    TEST_ASSERT_TRUE(want[2].at > want[1].at);
    for ( size_t w = 0U; (sizeof(bare) / sizeof(bare[0])) > w; ++w )
    {
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0U, bare[w].at, bare[w].needle);
    }
    owners_back();
}

/* ---- The listing whole ------------------------------------------------------ */

#define LISTING_MAX  1024U  /**< Lines one `help` listing may run to here.      */
#define LISTING_LEN  288U   /**< Room for a line, past the console's own cap.    */
#define LONG_DEF     "0123456789012345678901234567890123456789"  /**< A long default. */

static char     s_listing[LISTING_MAX][LISTING_LEN];  /**< `help`, line by line. */
static uint32_t s_listed;                             /**< Lines in it.          */

/** @brief Run `help` to its `>ok`, keeping every line without its line end. */
static bool help_listing(void)
{
    bool done = false;

    s_listed = 0U;
    type_line("help");
    for ( uint32_t step = 0U; (HELP_STEPS_MAX > step) && !done; ++step )
    {
        for ( uint32_t i = 0U; ((uint32_t)fake_klog_count() > i) && !done; ++i )
        {
            const char * const l = fake_klog_line(i);

            if ( (NULL != l) && (LISTING_MAX > s_listed) )
            {
                (void)snprintf(s_listing[s_listed], LISTING_LEN, "%s", l);
                s_listing[s_listed][strcspn(s_listing[s_listed], "\r\n")] = '\0';
                s_listed += 1U;
            }
            done = (NULL != l) && (strstr(l, ">ok") != NULL);
        }
        fake_klog_reset();
        (void)qf_support_tick(AO_Console, 1U);
    }
    return done;
}

/** @brief The line of the listing that is exactly @p want, or -1. */
static int32_t listed_at(const char *want)
{
    int32_t at = -1;

    for ( uint32_t i = 0U; (s_listed > i) && (0 > at); ++i )
    {
        if ( strcmp(s_listing[i], want) == 0 )
        {
            at = (int32_t)i;
        }
    }
    return at;
}

/* `test` takes one key, and its members run past what one record holds: on
 * the board they cut the record mid-member. A key's members are records of
 * their own, under their verb's, and every record is whole. */
TEST_CASE("help gives every member of a key a record of its own, after its verb's",
          "[console_verbs]")
{
    char verb[LISTING_LEN] = "";

    console_up();
    TEST_ASSERT_TRUE(help_listing());

    TEST_ASSERT_TRUE(0 <= listed_at(">|cmd=test args=component:enum:opt=all"));
    TEST_ASSERT_TRUE(listed_at(">|cmd=test args=component:enum:opt=all")
                     < listed_at(">|member=led verb=test key=component"));
    TEST_ASSERT_TRUE(listed_at(">|member=led verb=test key=component")
                     < listed_at(">|member=all verb=test key=component"));
    TEST_ASSERT_TRUE(0 <= listed_at(">|cmd=install args=src:string:req,dst:enum:req,"
                                    "run:bool:opt=off,size:uint:opt,crc32:uint:opt,"
                                    "name:string:opt"));
    TEST_ASSERT_TRUE(0 <= listed_at(">|member=https:// verb=install key=src"));
    TEST_ASSERT_TRUE(0 <= listed_at(">|member=pending verb=install key=dst"));

    for ( uint32_t i = 0U; s_listed > i; ++i )
    {
        const char * const l = s_listing[i];

        TEST_ASSERT_TRUE_MESSAGE(strlen(l) < (size_t)(KODE_LOG_LINE_MAX - 1U), l);
        TEST_ASSERT_NULL_MESSAGE(strstr(l, "cut=on"), l);
        TEST_ASSERT_NULL_MESSAGE(strchr(l, '('), l);
        if ( strncmp(l, ">|cmd=", sizeof(">|cmd=") - 1U) == 0 )
        {
            (void)snprintf(verb, sizeof(verb), "%s", &l[sizeof(">|cmd=") - 1U]);
            verb[strcspn(verb, " ")] = '\0';
        }
        else if ( strncmp(l, ">|member=", sizeof(">|member=") - 1U) == 0 )
        {
            char under[LISTING_LEN + sizeof(" verb= key=")];

            (void)snprintf(under, sizeof(under), " verb=%s key=", verb);
            TEST_ASSERT_NOT_NULL_MESSAGE(strstr(l, under), l);
        }
        else
        {
            /* The `>ok` that ends it. */
        }
    }
    owners_back();
}

static const char *const LONG_K1_MEMBERS[] = { "m1" };  /**< In the record.   */
static const char *const LONG_K4_MEMBERS[] = { "m4" };  /**< Cut out of it.   */
static const karg_spec_t LONG_KEYS[] = {
    { .key = "k1", .type = KARG_STRING, .def = LONG_DEF, .enums = LONG_K1_MEMBERS, .n_enums = 1U },
    { .key = "k2", .type = KARG_STRING, .def = LONG_DEF },
    { .key = "k3", .type = KARG_STRING, .def = LONG_DEF },
    { .key = "k4", .type = KARG_STRING, .def = LONG_DEF, .enums = LONG_K4_MEMBERS, .n_enums = 1U },
    { .key = "k5", .type = KARG_STRING, .def = LONG_DEF },
    { .key = "k6", .type = KARG_STRING, .def = LONG_DEF },
};
static const kverb_t LONG_VERBS[] = {
    { "zz_long", LONG_KEYS, (uint8_t)(sizeof(LONG_KEYS) / sizeof(LONG_KEYS[0])), h_made_up },
};
static const kverbs_t LONG_TABLE = { .verbs = LONG_VERBS, .n = 1U };

/* An owner's keys can outrun the record; it is cut after a whole key and
 * says so, never mid-key with the rest written over it. */
TEST_CASE("a verb whose keys outrun the record is cut at a whole key, and says so",
          "[console_verbs]")
{
    const char *l = NULL;
    size_t      n;

    console_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kverbs_add(&LONG_TABLE));
    TEST_ASSERT_TRUE(help_listing());

    for ( uint32_t i = 0U; s_listed > i; ++i )
    {
        if ( strncmp(s_listing[i], ">|cmd=zz_long ", sizeof(">|cmd=zz_long ") - 1U) == 0 )
        {
            l = s_listing[i];
        }
    }
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(l, " args=k1:string:opt=" LONG_DEF ",k2:"), l);
    TEST_ASSERT_NULL_MESSAGE(strstr(l, "k6:"), l);
    n = strlen(l);
    TEST_ASSERT_TRUE_MESSAGE(n > (sizeof(LONG_DEF " cut=on") - 1U), l);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(LONG_DEF " cut=on",
                                     &l[n - (sizeof(LONG_DEF " cut=on") - 1U)], l);

    /* A member belongs to a key the record holds, or it is not listed. */
    TEST_ASSERT_TRUE(0 <= listed_at(">|member=m1 verb=zz_long key=k1"));
    TEST_ASSERT_TRUE_MESSAGE(0 > listed_at(">|member=m4 verb=zz_long key=k4"),
                             "a member of a key the record left out");
    owners_back();
}

/** @brief A default longer than a whole record. */
#define HUGE_DEF  LONG_DEF LONG_DEF LONG_DEF LONG_DEF LONG_DEF

static const char *const HUGE_MEMBERS[] = { "mh" };
static const karg_spec_t HUGE_KEYS[] = {
    { .key = "kh", .type = KARG_STRING, .def = HUGE_DEF, .enums = HUGE_MEMBERS, .n_enums = 1U },
};
static const kverb_t HUGE_VERBS[] = { { "zz_huge", HUGE_KEYS, 1U, h_made_up } };
static const kverbs_t HUGE_TABLE = { .verbs = HUGE_VERBS, .n = 1U };

/* Not even the first key fits: the record holds no key, which the grammar
 * writes `-`, and says it was cut; and no member of the key is listed. */
TEST_CASE("a verb whose first key alone outruns the record lists none, cut",
          "[console_verbs]")
{
    console_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kverbs_add(&HUGE_TABLE));
    TEST_ASSERT_TRUE(help_listing());

    TEST_ASSERT_TRUE(0 <= listed_at(">|cmd=zz_huge args=- cut=on"));
    TEST_ASSERT_TRUE(0 > listed_at(">|member=mh verb=zz_huge key=kh"));
    owners_back();
}

/* ---- What a table's keys may be called -------------------------------------- */

#define KEY_AT_MAX         "k234567890123456"           /**< KARG_KEY_NAME_MAX long.    */
#define KEY_PAST_MAX       KEY_AT_MAX "x"               /**< One past it.               */
#define MEMBER_AT_MAX      "m23456789012345678901234"   /**< KARG_MEMBER_NAME_MAX long. */
#define MEMBER_PAST_MAX    MEMBER_AT_MAX "x"            /**< One past it.               */
_Static_assert((sizeof(KEY_AT_MAX) - 1U) == KARG_KEY_NAME_MAX, "a key at the limit");
_Static_assert((sizeof(MEMBER_AT_MAX) - 1U) == KARG_MEMBER_NAME_MAX, "a member at the limit");

static const char *const FINE_MEMBERS[]  = { "fine" };
static const char *const BAD_MEMBERS[]   = { "fine", "not-a-name" };
static const char *const LONG_MEMBERS[]  = { MEMBER_PAST_MAX };
static const char *const EDGE_MEMBERS[]  = { MEMBER_AT_MAX };

static const karg_spec_t KEY_NOT_NAME[] = { { .key = "Key", .type = KARG_UINT, .max = 9U } };
static const karg_spec_t KEY_TOO_LONG[] = { { .key = KEY_PAST_MAX, .type = KARG_UINT, .max = 9U } };
static const karg_spec_t MEMBER_NOT_NAME[] = {
    { .key = "a", .type = KARG_ENUM, .enums = FINE_MEMBERS, .n_enums = 1U },
    { .key = "b", .type = KARG_ENUM, .enums = BAD_MEMBERS,  .n_enums = 2U },
};
static const karg_spec_t MEMBER_TOO_LONG[] = {
    { .key = "c", .type = KARG_ENUM, .enums = LONG_MEMBERS, .n_enums = 1U },
};
static const karg_spec_t NAMES_AT_MAX[] = {
    { .key = KEY_AT_MAX, .type = KARG_ENUM, .enums = EDGE_MEMBERS, .n_enums = 1U },
};

/** @brief Its second row's key is not a name. */
static const kverb_t L_VERBS[] = {
    { "zz_l0", NULL, 0U, h_made_up },
    { "zz_l1", KEY_NOT_NAME, 1U, h_made_up },
};
static const kverbs_t L_TABLE = { .verbs = L_VERBS, .n = 2U };
static const kverb_t M_VERBS[] = { { "zz_m0", KEY_TOO_LONG, 1U, h_made_up } };
static const kverbs_t M_TABLE = { .verbs = M_VERBS, .n = 1U };
static const kverb_t N_VERBS[] = { { "zz_n0", MEMBER_NOT_NAME, 2U, h_made_up } };
static const kverbs_t N_TABLE = { .verbs = N_VERBS, .n = 1U };
static const kverb_t O_VERBS[] = { { "zz_o0", MEMBER_TOO_LONG, 1U, h_made_up } };
static const kverbs_t O_TABLE = { .verbs = O_VERBS, .n = 1U };
static const kverb_t P_VERBS[] = { { "zz_p0", NAMES_AT_MAX, 1U, h_made_up } };
static const kverbs_t P_TABLE = { .verbs = P_VERBS, .n = 1U };

/* A key or a member the console could not print in a record of its own is
 * refused with its table, and the log says which row and what is wrong. */
TEST_CASE("an owner's key or member that is no name, or too long, refuses its table",
          "[console_verbs]")
{
    struct {
        const kverbs_t *table;
        const char     *verb;
        const char     *why;
    } const bad[] = {
        { &L_TABLE, "zz_l1", "verb 1 'zz_l1', key 0: not a name" },
        { &M_TABLE, "zz_m0", "verb 0 'zz_m0', key 0: not a name" },
        { &N_TABLE, "zz_n0", "verb 0 'zz_n0', key 'b', member 1: not a name" },
        { &O_TABLE, "zz_o0", "verb 0 'zz_o0', key 'c', member 0: not a name" },
    };

    console_up();
    size_t const before = kverbs_count();

    for ( size_t i = 0U; (sizeof(bad) / sizeof(bad[0])) > i; ++i )
    {
        fake_klog_reset();
        TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kverbs_add(bad[i].table), bad[i].verb);
        TEST_ASSERT_TRUE_MESSAGE(replied(bad[i].why), bad[i].why);
        TEST_ASSERT_NULL_MESSAGE(kverbs_find(bad[i].verb, NULL), bad[i].verb);
    }
    TEST_ASSERT_NULL_MESSAGE(kverbs_find("zz_l0", NULL), "a refused table registers no row");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)before, (uint32_t)kverbs_count());

    /* At the limits, it goes in, and its member's record is one line. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kverbs_add(&P_TABLE));
    TEST_ASSERT_TRUE(help_listing());
    TEST_ASSERT_TRUE(0 <= listed_at(">|member=" MEMBER_AT_MAX " verb=zz_p0 key=" KEY_AT_MAX));
    owners_back();
}

/* ---- The hooks an owner's table carries ------------------------------------ */

static uint32_t s_hook_a;   /**< Events table E's answer hook was shown. */
static uint32_t s_hook_b;   /**< Events table F's answer hook was shown. */

/** @brief A verb that holds the console until its owner's answer. */
static void h_holds(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
}

/** @brief E's hook: every event is E's, and it closes the verb. */
static bool hook_a(QEvt const *e)
{
    (void)e;
    s_hook_a += 1U;
    console_reply(">ok");
    console_end();
    return true;
}

/** @brief F's hook: the same, counted apart. */
static bool hook_b(QEvt const *e)
{
    (void)e;
    s_hook_b += 1U;
    console_reply(">ok");
    console_end();
    return true;
}

/** @brief G's timeout hook: running out the time is its verb's own end. */
static bool timeout_ok(void)
{
    console_reply(">|window=over");
    return true;
}

static const kverb_t E_VERBS[] = { { "zz_e1", NULL, 0U, h_holds } };
static const kverbs_t E_TABLE = { .verbs = E_VERBS, .n = 1U, .answer = hook_a };
static const kverb_t F_VERBS[] = { { "zz_f1", NULL, 0U, h_holds } };
static const kverbs_t F_TABLE = { .verbs = F_VERBS, .n = 1U, .answer = hook_b };
static const kverb_t G_VERBS[] = { { "zz_g1", NULL, 0U, h_holds } };
static const kverbs_t G_TABLE = { .verbs = G_VERBS, .n = 1U, .timeout = timeout_ok };
static const kverb_t H_VERBS[] = { { "zz_h1", NULL, 0U, h_holds } };
static const kverbs_t H_TABLE = { .verbs = H_VERBS, .n = 1U };

/** @brief An event no console case reads, as an owner's answer would arrive. */
static void deliver(void)
{
    static const QEvt answer = QEVT_INITIALIZER(NFC_INFO_RSP_SIG);

    QASM_DISPATCH(AO_Console, &answer, 0U);
}

TEST_CASE("only the owner whose verb holds the console sees an answer",
          "[console_verbs]")
{
    console_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kverbs_add(&E_TABLE));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kverbs_add(&F_TABLE));
    s_hook_a = 0U;
    s_hook_b = 0U;

    deliver();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, s_hook_a + s_hook_b,
        "nothing in flight: no owner is shown the event");

    type_line("zz_e1");
    deliver();
    TEST_ASSERT_EQUAL_UINT32(1U, s_hook_a);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, s_hook_b, "F's verb was not in flight");
    TEST_ASSERT_TRUE(replied(">ok"));

    deliver();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, s_hook_a, "closed: a late answer is nobody's");

    type_line("zz_f1");
    deliver();
    TEST_ASSERT_EQUAL_UINT32(1U, s_hook_a);
    TEST_ASSERT_EQUAL_UINT32(1U, s_hook_b);

    owners_back();
}

/** @brief Empty AO_Storage's queue of what a case's verbs posted to it. */
static void drain_storage(void)
{
    const QEvt *e = NULL;

    while ( (NULL != AO_Storage->eQueue)
           && (xQueueReceive(AO_Storage->eQueue, &e, 0U) == pdTRUE) )
    {
        QF_gc(e);
    }
}

/**
 * @brief The `ls` half of the case below, answered by hand, in a child.
 *
 * `ls` asks its page through AO_Storage's list door, and only AO_Storage's
 * own answer opens that door again. Answered by hand in this process, the
 * console's door would stay shut for every `ls` after this file; in a child
 * it ends with the child.
 *
 * @param arg Unused.
 */
static void ls_in_flight_body(void *arg)
{
    FileRspEvt        end;
    StoragePathRspEvt stat;
    StorageEntryEvt   entry;

    (void)arg;
    (void)memset(&end, 0, sizeof(end));
    end.super = (QEvt)QEVT_INITIALIZER(FILE_LIST_END_SIG);
    end.rc    = (uint8_t)KODE_OK;
    end.more  = false;
    end.count = 1U;

    /* `ls` stays in flight across its answers, the entry among them the same
     * STORAGE_ENTRY_SIG that hook reads. */
    fake_klog_reset();
    type_line("ls path=\"/storage\"");
    (void)memset(&stat, 0, sizeof(stat));
    stat.super  = (QEvt)QEVT_INITIALIZER(STORAGE_PATH_RSP_SIG);
    stat.rc     = (uint8_t)KODE_OK;
    stat.exists = true;
    stat.is_dir = true;
    QASM_DISPATCH(AO_Console, &stat.super, 0U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "the listing is still to come");

    (void)memset(&entry, 0, sizeof(entry));
    entry.super = (QEvt)QEVT_INITIALIZER(STORAGE_ENTRY_SIG);
    (void)snprintf(entry.name, sizeof(entry.name), "%s", "notes.txt");
    entry.size = 5U;
    QASM_DISPATCH(AO_Console, &entry.super, 0U);
    TEST_ASSERT_TRUE_MESSAGE(replied(">|name=\"notes.txt\" type=file size=5 medium=fs"),
        "the console's own record, with the type only `ls` prints");
    TEST_ASSERT_FALSE_MESSAGE(replied(">|name=\"notes.txt\" size=5 medium=fs"),
        "not the owner's sd_ls record");
    QASM_DISPATCH(AO_Console, &end.super, 0U);
    TEST_ASSERT_TRUE(replied(">ok"));

    /* And while it was in flight no owner's hook was shown anything. */
    type_line("ls path=\"/storage\"");
    deliver();
    TEST_ASSERT_EQUAL_UINT32(0U, s_hook_a);
}

TEST_CASE("a console verb in flight reads a signal an owner's hook also reads",
          "[console_verbs]")
{
    static const ListEndEvt end = { .super = QEVT_INITIALIZER(STORAGE_LIST_END_SIG), .more = false };

    console_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kverbs_add(&E_TABLE));
    s_hook_a = 0U;

    /* An owner's listing first, so AO_Storage's hook is left in its listing
     * kind: under the old order it would take the next listing too. */
    type_line("sd_ls");
    QASM_DISPATCH(AO_Console, &end.super, 0U);
    TEST_ASSERT_TRUE(replied(">ok"));

    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)ASK_CHILD_RETURNED,
        (uint32_t)ask_support_fork_run(&ls_in_flight_body, NULL),
        "the `ls` half failed in its child");
    drain_storage();
    owners_back();
}

TEST_CASE("hex text is judged whole before a byte is written", "[console_verbs]")
{
    static const uint8_t KEEP[] = { 0x11U, 0x22U, 0x33U, 0x44U, 0x55U };
    uint8_t out[sizeof(KEEP)];

    (void)memcpy(out, KEEP, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)console_hex_bytes("AB12ZZ0123", out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(KEEP, out, sizeof(out), "a bad digit late in the text");
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)console_hex_bytes("AB12C", out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(KEEP, out, sizeof(out), "an odd digit count");
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)console_hex_bytes("AB12CD3456EF", out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(KEEP, out, sizeof(out), "too many digits");

    static const uint8_t ID[] = { 0xABU, 0xCDU, 0xEFU, 0x01U, 0x23U };
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(ID),
                             (uint32_t)console_hex_bytes("ab:CD:ef:01:23", out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ID, out, sizeof(ID));
}

TEST_CASE("info prints the coprocessor's record only with AO_Radio registered",
          "[console_verbs]")
{
    console_up();
    Mockkdrv_slot_Init();
    kdrv_slot_info_IgnoreAndReturn(KODE_DENIED);
    kdrv_slot_coredump_IgnoreAndReturn(KODE_DENIED);
    type_line("info");
    TEST_ASSERT_TRUE(replied(">|c5="));
    TEST_ASSERT_TRUE(replied(">|log="));
    TEST_ASSERT_TRUE(replied(">ok"));

    kverbs_reset();
    fake_klog_reset();
    type_line("info");
    TEST_ASSERT_FALSE_MESSAGE(replied(">|c5="), "no AO_Radio, no record of its own");
    TEST_ASSERT_TRUE(replied(">|serial="));
    TEST_ASSERT_TRUE(replied(">|log="));
    TEST_ASSERT_TRUE(replied(">ok"));
    Mockkdrv_slot_Destroy();
    owners_back();
}

TEST_CASE("an owner's timeout hook closes its verb, and without one the watchdog errs",
          "[console_verbs]")
{
    console_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kverbs_add(&G_TABLE));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kverbs_add(&H_TABLE));

    type_line("zz_g1");
    (void)qf_support_tick(AO_Console, WATCHDOG_TICKS);
    TEST_ASSERT_TRUE(replied(">|window=over"));
    TEST_ASSERT_FALSE(replied(">err code=6"));
    fake_klog_reset();
    type_line("ping");
    TEST_ASSERT_TRUE_MESSAGE(replied(">ok"), "the hook's close freed the console");

    fake_klog_reset();
    type_line("zz_h1");
    (void)qf_support_tick(AO_Console, WATCHDOG_TICKS);
    TEST_ASSERT_TRUE(replied(">err code=6 msg=zz_h1"));
    fake_klog_reset();
    type_line("ping");
    TEST_ASSERT_TRUE(replied(">ok"));
    owners_back();
}

/* ---- A verb that does not wait --------------------------------------------- */

static const karg_spec_t A_ONE_KEY[] = {
    { .key = "n", .required = false, .type = KARG_UINT, .min = 0U, .max = 9U },
};

static uint32_t s_hook_i;   /**< Answers table I's hook closed its verb on. */

/** @brief I's hook: the answer deliver() brings closes its verb; a line typed meanwhile is not its. */
static bool hook_i(QEvt const *e)
{
    bool const mine = ((QSignal)NFC_INFO_RSP_SIG == e->sig);

    if ( mine )
    {
        s_hook_i += 1U;
        console_reply(">ok");
        console_end();
    }
    return mine;
}

/** @brief An owner with a verb that holds the console and one that does not wait for it. */
static const kverb_t I_VERBS[] = {
    { "zz_i1", NULL, 0U, h_holds },
    { "zz_i2", NULL, 0U, h_made_up },
};
static const kverbs_t I_TABLE = { .verbs = I_VERBS, .n = 2U, .answer = hook_i, .nowait = "zz_i2" };

/** @brief An owner that names a verb it does not have. */
static const kverb_t J_VERBS[] = { { "zz_j1", NULL, 0U, h_made_up } };
static const kverbs_t J_TABLE = { .verbs = J_VERBS, .n = 1U, .nowait = "zz_i2" };

/** @brief An owner whose verb that does not wait takes a key. */
static const kverb_t K_VERBS[] = { { "zz_k1", A_ONE_KEY, 1U, h_made_up } };
static const kverbs_t K_TABLE = { .verbs = K_VERBS, .n = 1U, .nowait = "zz_k1" };

TEST_CASE("a verb that does not wait runs while another holds the console, which keeps its command",
          "[console_verbs]")
{
    console_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kverbs_add(&I_TABLE));
    s_hook_i = 0U;

    /* Idle, it is a verb like any other. */
    s_ran = NULL;
    type_line("zz_i2");
    TEST_ASSERT_EQUAL_STRING("zz_i2", s_ran);
    TEST_ASSERT_TRUE(replied(">ok"));

    type_line("zz_i1");
    fake_klog_reset();
    s_ran = NULL;
    type_line("zz_i2");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("zz_i2", s_ran, "it waited behind the command in flight");
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_FALSE(replied(">err code=4 msg=busy"));

    /* Its answer closed nothing: every other line is still turned away, the
     * bare verb with something behind it included, and the owner still hears
     * the answer to the command in flight. */
    fake_klog_reset();
    s_ran = NULL;
    type_line("zz_i2 n=1");
    type_line("ping");
    TEST_ASSERT_NULL(s_ran);
    TEST_ASSERT_TRUE(replied(">err code=4 msg=busy"));
    TEST_ASSERT_FALSE(replied(">ok"));
    deliver();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, s_hook_i, "the command in flight lost the console");
    TEST_ASSERT_TRUE(replied(">ok"));

    fake_klog_reset();
    type_line("ping");
    TEST_ASSERT_TRUE(replied(">ok"));
    owners_back();
}

TEST_CASE("a table's verb that does not wait must be one of its rows, with no keys",
          "[console_verbs]")
{
    console_up();
    size_t const before = kverbs_count();

    fake_klog_reset();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kverbs_add(&J_TABLE));
    TEST_ASSERT_TRUE(replied("'zz_i2' does not wait, and is not a row with no keys"));
    TEST_ASSERT_NULL(kverbs_find("zz_j1", NULL));

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kverbs_add(&K_TABLE));
    TEST_ASSERT_NULL(kverbs_find("zz_k1", NULL));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)before, (uint32_t)kverbs_count());
    owners_back();
}
