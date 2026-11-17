/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_radio_knock.c
 * @brief A knock through every order of its steps: posts from two tasks, a full queue, and the Active Object.
 *
 * The cases build a small world - one knock, a queue of a few slots, two
 * tasks that post the knock, other traffic that fills the queue, and an
 * Active Object that takes one event at a time - and walk every order in
 * which their steps can run. A step is one read or write of the knock's word
 * or of the queue, so the orders walked are the orders a board can produce.
 * Each world is walked once: a world reached twice is not walked again.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "radio_knock.h"

/* ---- The world --------------------------------------------------------------- */

/** @brief The longest queue a case builds. */
#define WORLD_QUEUE_MAX   3U

/** @brief The tasks that post the knock. */
#define WORLD_POSTERS     2U

/** @brief What a slot of the queue holds: another task's event. */
#define SLOT_OTHER        1U

/** @brief What a slot of the queue holds: the knock's event. */
#define SLOT_KNOCK        2U

/** @brief The Active Object waits for an event. */
#define AO_WAITS          0U

/** @brief The Active Object has an event in hand and pays what is owed. */
#define AO_PAYS           1U

/** @brief The Active Object handles the knock's event: it takes the knock. */
#define AO_TAKES          2U

/** @brief The tries of the queue a post makes by itself: the first, and one after a look that found room. */
#define POST_OWN_TRIES    2U

/** @brief What the first task's posts carry. */
#define ARG_FIRST         0x11U

/** @brief What the second task's posts carry. */
#define ARG_SECOND        0x22U

/** @brief The actors a step can belong to. */
typedef enum {
    ACT_POSTER_0 = 0,   /**< The first task that posts the knock. */
    ACT_POSTER_1,       /**< The second. */
    ACT_OTHER,          /**< The traffic that fills the queue. */
    ACT_AO,             /**< The Active Object. */
    ACT_TAKER,          /**< A take that comes from anywhere, with no event in hand. */
    ACT_COUNT
} actor_t;

/** @brief One task that posts the knock. */
typedef struct {
    radio_knock_run_t run;      /**< The post it is inside. */
    bool              inside;   /**< A post has begun and has steps left. */
    uint8_t           left;     /**< Posts it has yet to begin. */
    uint8_t           arg;      /**< What its posts carry. */
    uint8_t           sends;    /**< Times the post it is inside has tried the queue. */
    uint8_t           others;   /**< Events the other traffic had queued when that post began. */
} poster_t;

/** @brief Everything a case's world holds. */
typedef struct {
    radio_knock_t     knock;                    /**< The knock. */
    uint8_t           slot[WORLD_QUEUE_MAX];    /**< The queue, oldest first. */
    uint8_t           used;                     /**< Slots taken. */
    uint8_t           len;                      /**< Slots in all. */
    uint8_t           margin;                   /**< Slots the knock's post leaves free. */
    poster_t          poster[WORLD_POSTERS];    /**< The tasks that post. */
    uint8_t           other_left;               /**< Events the other traffic has yet to post. */
    uint8_t           other_in;                 /**< Events of the other traffic the queue has taken. */
    uint8_t           taker_left;               /**< Takes from anywhere yet to come. */
    uint8_t           ao_at;                    /**< AO_WAITS, AO_PAYS or AO_TAKES. */
    uint8_t           ao_slot;                  /**< What the event in hand is. */
    radio_knock_run_t ao_run;                   /**< The payment it is inside. */
    uint8_t           claims;                   /**< Posts that made the knock pending. */
    uint8_t           heard;                    /**< Takes that found it pending. */
    uint8_t           claim_arg;                /**< What the last such post carried. */
} world_t;

/** @brief The world the door's two calls act on. */
static world_t *s_world;

/** @brief Why the walk stopped; NULL while every world held. */
static const char *s_broken;

/** @brief The longest name of an actor in a trace, with the blank after it. */
#define TRACE_NAME_MAX    8U

/** @brief The bytes a trace is written into. */
#define TRACE_MAX         2048U

/** @brief The steps that led to the world that broke, oldest first. */
static char s_trace[TRACE_MAX];

/** @brief radio_knock_door_t.post: the queue takes the knock's event past its margin. */
static bool world_post(const void *ctx)
{
    (void)ctx;
    bool const taken = (uint8_t)(s_world->len - s_world->used) > s_world->margin;

    if ( taken )
    {
        s_world->slot[s_world->used] = SLOT_KNOCK;
        s_world->used += 1U;
    }
    return taken;
}

/** @brief radio_knock_door_t.room: whether the queue would take the knock's event. */
static bool world_room(const void *ctx)
{
    (void)ctx;
    return (uint8_t)(s_world->len - s_world->used) > s_world->margin;
}

/** @brief The queue of every world. */
static const radio_knock_door_t s_door = { &world_post, &world_room, NULL };

/** @brief How many events of the knock wait in the queue. */
static uint8_t knock_events(const world_t *w)
{
    uint8_t n = 0U;

    for ( uint8_t i = 0U; i < w->used; ++i )
    {
        if ( SLOT_KNOCK == w->slot[i] )
        {
            n += 1U;
        }
    }
    return n;
}

/** @brief Whether @p who has a step to take in @p w. */
static bool can_step(const world_t *w, actor_t who)
{
    bool can = false;

    switch ( who )
    {
        case ACT_POSTER_0:
        case ACT_POSTER_1: {
            poster_t const * const p = &w->poster[(unsigned)who];

            can = p->inside || (0U < p->left);
            break;
        }
        case ACT_OTHER: {
            can = 0U < w->other_left;
            break;
        }
        case ACT_AO: {
            can = (AO_WAITS != w->ao_at) || (0U < w->used);
            break;
        }
        case ACT_TAKER: {
            can = 0U < w->taker_left;
            break;
        }
        default: {
            can = false;
            break;
        }
    }
    return can;
}

/** @brief Count a take that found the knock pending, and judge what it carried. */
static void heard_one(world_t *w, uint8_t arg)
{
    w->heard += 1U;
    if ( w->heard > w->claims )
    {
        s_broken = "a knock was heard more times than it was made";
    }
    else if ( arg != w->claim_arg )
    {
        s_broken = "the knock did not carry what its first post did";
    }
    else
    {
        /* heard, once, with what its first post carried */
    }
}

/**
 * @brief One step of a task that posts the knock.
 * @param w    The world.
 * @param p    The task.
 * @param lone Whether nothing but the Active Object's own takes clears the knock.
 */
static void poster_step(world_t *w, poster_t *p, bool lone)
{
    if ( !p->inside )
    {
        radio_knock_post_begin(&p->run, p->arg);
        p->inside = true;
        p->left  -= 1U;
        p->sends  = 0U;
        p->others = w->other_in;
    }
    bool const claiming = (uint8_t)RADIO_KNOCK_AT_CLAIM == p->run.at;

    if ( (uint8_t)RADIO_KNOCK_AT_SEND == p->run.at )
    {
        /* The first try and the one a look with room allows are the post's
         * own. Every try past those follows a refusal after such a look,
         * which takes an event another task queued in between. Judged where
         * the knock's own events are one at a time: a take from anywhere
         * leaves stray ones, which fill the queue as other traffic does. */
        p->sends += 1U;
        if ( lone && (p->sends > (uint8_t)(POST_OWN_TRIES + (uint8_t)(w->other_in - p->others))) )
        {
            s_broken = "a post went round again with no event of another task in between";
        }
    }

    p->inside = radio_knock_step(&w->knock, &p->run);
    if ( claiming && ((uint8_t)RADIO_KNOCK_AT_SEND == p->run.at) )
    {
        w->claims   += 1U;
        w->claim_arg = p->arg;
    }
    if ( claiming && ((uint8_t)RADIO_KNOCK_AT_DONE == p->run.at) && (w->heard == w->claims) )
    {
        /* Every knock made was heard already, so this post folded into
         * nothing: no event will ever tell of it. */
        s_broken = "a post was folded into a knock that nobody will hear";
    }
    if ( lone && (1U < knock_events(w)) )
    {
        s_broken = "two events of one knock wait in the queue";
    }
}

/** @brief One step of the Active Object: an event out of the queue, a step of the payment, or the take. */
static void ao_step(world_t *w, bool lone)
{
    if ( AO_WAITS == w->ao_at )
    {
        w->ao_slot = w->slot[0];
        for ( uint8_t i = 1U; i < w->used; ++i )
        {
            w->slot[i - 1U] = w->slot[i];
        }
        w->used -= 1U;
        w->slot[w->used] = 0U;
        radio_knock_pay_begin(&w->ao_run);
        w->ao_at = AO_PAYS;
    }
    else if ( AO_PAYS == w->ao_at )
    {
        if ( !radio_knock_step(&w->knock, &w->ao_run) )
        {
            w->ao_at = AO_WAITS;
            if ( SLOT_KNOCK == w->ao_slot )
            {
                w->ao_at = AO_TAKES;
            }
        }
        if ( lone && (1U < knock_events(w)) )
        {
            s_broken = "two events of one knock wait in the queue";
        }
    }
    else
    {
        uint8_t arg = 0U;

        if ( radio_knock_take(&w->knock, &arg) )
        {
            heard_one(w, arg);
        }
        else if ( lone )
        {
            s_broken = "the knock's event found nothing pending";
        }
        else
        {
            /* taken from elsewhere already: an event with nothing to say */
        }
        w->ao_at = AO_WAITS;
    }
}

/**
 * @brief Take one step of @p who in the world @p w.
 * @param w    The world.
 * @param who  The actor; it has a step to take.
 * @param lone Whether nothing but the Active Object's own takes clears the knock.
 */
static void step(world_t *w, actor_t who, bool lone)
{
    s_world = w;
    switch ( who )
    {
        case ACT_POSTER_0:
        case ACT_POSTER_1: {
            poster_step(w, &w->poster[(unsigned)who], lone);
            break;
        }
        case ACT_OTHER: {
            /* Posted with no margin, as a time event is: it takes the last slot. */
            w->other_left -= 1U;
            if ( w->used < w->len )
            {
                w->slot[w->used] = SLOT_OTHER;
                w->used     += 1U;
                w->other_in += 1U;
            }
            break;
        }
        case ACT_AO: {
            ao_step(w, lone);
            break;
        }
        default: {
            uint8_t arg = 0U;

            w->taker_left -= 1U;
            if ( radio_knock_take(&w->knock, &arg) )
            {
                heard_one(w, arg);
            }
            break;
        }
    }
}

/** @brief Judge a world in which nobody has a step left. */
static void judge_end(const world_t *w)
{
    if ( 0U != atomic_load(&w->knock.word) )
    {
        s_broken = "a knock is left standing with no event in the queue and nobody to post it";
    }
    else if ( w->heard != w->claims )
    {
        s_broken = "a knock that was made was never heard";
    }
    else
    {
        /* every knock made was heard, and nothing is left standing */
    }
}

/* ---- The walk ------------------------------------------------------------------ */

/** @brief The bytes that tell one world from another. */
#define KEY_LEN      32U

/** @brief The bit of a task's byte of the key that says it is inside a post. */
#define KEY_INSIDE   0x80U

/** @brief The bytes a run takes in a key. */
#define KEY_RUN_LEN  4U

/** @brief The bytes a post's tries and the traffic since it began take in a key. */
#define KEY_POST_LEN 2U

/** @brief The low byte of a word. */
#define BYTE_MASK    0xFFU

/** @brief The bits of a byte. */
#define BYTE_BITS    8U

/** @brief Where the hash of a key starts: FNV-1a's offset. */
#define HASH_START   2166136261UL

/** @brief What the hash of a key is multiplied by at each byte: FNV-1a's prime. */
#define HASH_STEP    16777619UL

/** @brief The fewest worlds a case's walk judges; fewer is a walk that went nowhere. */
#define WORLDS_MIN   1000UL

/** @brief Slots of the table of worlds already walked; a power of two. */
#define SEEN_SLOTS   (1UL << 21)

/** @brief The deepest a walk goes: every actor's steps, end to end. */
#define DEPTH_MAX    256U

/** @brief The worlds already walked, by key; a first byte of 0 is an empty slot. */
static uint8_t (*s_seen)[KEY_LEN];

/** @brief How many worlds the walk has judged. */
static unsigned long s_worlds;

/** @brief The actor of each step taken so far. */
static uint8_t s_path[DEPTH_MAX];

/** @brief Write one run into a key. */
static size_t key_run(uint8_t *key, size_t at, const radio_knock_run_t *run)
{
    size_t n = at;

    key[n] = run->at;
    n++;
    if ( run->payer )
    {
        key[n] = 1U;
    }
    n++;
    key[n] = (uint8_t)(run->held & BYTE_MASK);
    n++;
    key[n] = (uint8_t)((run->held >> BYTE_BITS) & BYTE_MASK);
    n++;
    return n;
}

/** @brief Write the bytes that tell @p w from every other world; @p lone as step() takes it. */
static void key_of(const world_t *w, bool lone, uint8_t key[KEY_LEN])
{
    unsigned int const word = atomic_load(&w->knock.word);
    size_t n = 0U;

    (void)memset(key, 0, KEY_LEN);
    key[n] = 1U;   /* never an empty slot */
    n++;
    key[n] = (uint8_t)(word & BYTE_MASK);
    n++;
    key[n] = (uint8_t)((word >> BYTE_BITS) & BYTE_MASK);
    n++;
    key[n] = w->used;
    n++;
    for ( uint8_t i = 0U; WORLD_QUEUE_MAX > i; ++i )
    {
        key[n] = w->slot[i];
        n++;
    }
    for ( uint8_t i = 0U; WORLD_POSTERS > i; ++i )
    {
        poster_t const * const p = &w->poster[i];

        if ( p->inside )
        {
            n = key_run(key, n, &p->run);
            if ( lone )
            {
                /* What the bound on a post's tries is judged by; not judged otherwise. */
                key[n] = p->sends;
                key[n + 1U] = (uint8_t)(w->other_in - p->others);
            }
            n += KEY_POST_LEN;
        }
        else
        {
            n += KEY_RUN_LEN + KEY_POST_LEN;
        }
        key[n] = p->left;
        if ( p->inside )
        {
            key[n] |= KEY_INSIDE;
        }
        n++;
    }
    key[n] = w->other_left;
    n++;
    key[n] = w->taker_left;
    n++;
    key[n] = w->ao_at;
    n++;
    if ( AO_WAITS != w->ao_at )
    {
        key[n] = w->ao_slot;
    }
    n++;
    if ( AO_PAYS == w->ao_at )
    {
        n = key_run(key, n, &w->ao_run);
    }
    else
    {
        n += KEY_RUN_LEN;
    }
    key[n] = w->claims;
    n++;
    key[n] = w->heard;
    n++;
    if ( w->heard < w->claims )
    {
        key[n] = w->claim_arg;
    }
    n++;
    TEST_ASSERT_TRUE(KEY_LEN >= n);
}

/**
 * @brief Note a world as walked.
 * @return true when it was walked already.
 */
static bool seen_before(const uint8_t key[KEY_LEN])
{
    unsigned long h = HASH_START;

    for ( size_t i = 0U; KEY_LEN > i; ++i )
    {
        h = (h ^ key[i]) * HASH_STEP;
    }
    unsigned long at = h & (SEEN_SLOTS - 1UL);

    for ( unsigned long tries = 0UL; SEEN_SLOTS > tries; ++tries )
    {
        if ( 0U == s_seen[at][0] )
        {
            (void)memcpy(s_seen[at], key, KEY_LEN);
            return false;
        }
        if ( 0 == memcmp(s_seen[at], key, KEY_LEN) )
        {
            return true;
        }
        at = (at + 1UL) & (SEEN_SLOTS - 1UL);
    }
    TEST_FAIL_MESSAGE("the table of worlds walked is full");
    return true;
}

/** @brief Write the steps that led here into s_trace. */
static void trace_write(unsigned depth)
{
    static const char * const name[ACT_COUNT] = { "A", "B", "other", "AO", "take" };
    size_t n = 0U;

    s_trace[0] = '\0';
    for ( unsigned i = 0U; (i < depth) && ((n + TRACE_NAME_MAX) < sizeof(s_trace)); ++i )
    {
        n += (size_t)snprintf(&s_trace[n], sizeof(s_trace) - n, "%s ", name[s_path[i]]);
    }
}

/**
 * @brief Walk every order of steps from @p w on.
 * @param w     The world, left as it was found.
 * @param lone  Whether nothing but the Active Object's own takes clears the knock.
 * @param depth Steps taken to reach it.
 */
static void walk(const world_t *w, bool lone, unsigned depth)
{
    uint8_t key[KEY_LEN];
    bool    any = false;

    key_of(w, lone, key);
    if ( (NULL != s_broken) || seen_before(key) )
    {
        return;
    }
    s_worlds += 1UL;
    TEST_ASSERT_TRUE_MESSAGE(DEPTH_MAX > depth, "a walk went deeper than every step there is");
    for ( unsigned who = 0U; ((unsigned)ACT_COUNT > who) && (NULL == s_broken); ++who )
    {
        if ( can_step(w, (actor_t)who) )
        {
            world_t next;

            (void)memcpy(&next, w, sizeof(next));
            any = true;
            s_path[depth] = (uint8_t)who;
            step(&next, (actor_t)who, lone);
            if ( NULL != s_broken )
            {
                trace_write(depth + 1U);
            }
            else
            {
                walk(&next, lone, depth + 1U);
            }
        }
    }
    if ( (!any) && (NULL == s_broken) )
    {
        judge_end(w);
        if ( NULL != s_broken )
        {
            trace_write(depth);
        }
    }
}

/**
 * @brief Build a world and walk it whole.
 *
 * @param len    Slots of the queue.
 * @param margin Slots the knock's post leaves free.
 * @param posts  Posts each of the two tasks makes.
 * @param others Events the other traffic posts.
 * @param takes  Takes that come from anywhere, with no event in hand.
 * @return How many worlds were judged.
 */
static unsigned long walk_world(uint8_t len, uint8_t margin, uint8_t posts,
                                uint8_t others, uint8_t takes)
{
    world_t w;

    (void)memset(&w, 0, sizeof(w));
    radio_knock_init(&w.knock, &s_door);
    w.len            = len;
    w.margin         = margin;
    w.poster[0].left = posts;
    w.poster[0].arg  = ARG_FIRST;
    w.poster[1].left = posts;
    w.poster[1].arg  = ARG_SECOND;
    w.other_left     = others;
    w.taker_left     = takes;

    s_seen = calloc(SEEN_SLOTS, sizeof(*s_seen));
    TEST_ASSERT_NOT_NULL(s_seen);
    s_broken = NULL;
    s_worlds = 0UL;
    walk(&w, 0U == takes, 0U);
    free(s_seen);
    s_seen = NULL;
    if ( NULL != s_broken )
    {
        (void)printf("  queue %u, margin %u, %u posts a task, %u other events, %u takes from anywhere\n"
                     "  steps: %s\n",
                     (unsigned)len, (unsigned)margin, (unsigned)posts,
                     (unsigned)others, (unsigned)takes, s_trace);
        TEST_FAIL_MESSAGE(s_broken);
    }
    return s_worlds;
}

/* ---- The cases ----------------------------------------------------------------- */

TEST_CASE("a knock posted into a queue with room is heard once, with what it carried",
          "[radio_knock]")
{
    radio_knock_t k;
    world_t       w;
    uint8_t       arg = 0U;

    (void)memset(&w, 0, sizeof(w));
    w.len   = 2U;
    s_world = &w;
    radio_knock_init(&k, &s_door);

    TEST_ASSERT_TRUE(radio_knock_post(&k, ARG_FIRST));
    TEST_ASSERT_EQUAL_UINT8(1U, knock_events(&w));
    TEST_ASSERT_TRUE(radio_knock_take(&k, &arg));
    TEST_ASSERT_EQUAL_HEX8(ARG_FIRST, arg);
    TEST_ASSERT_FALSE_MESSAGE(radio_knock_take(&k, &arg), "a knock was taken twice");
}

TEST_CASE("posts that find a knock waiting fold into it and the first one's word stands",
          "[radio_knock]")
{
    radio_knock_t k;
    world_t       w;
    uint8_t       arg = 0U;

    (void)memset(&w, 0, sizeof(w));
    w.len   = 3U;
    s_world = &w;
    radio_knock_init(&k, &s_door);

    TEST_ASSERT_TRUE(radio_knock_post(&k, ARG_FIRST));
    TEST_ASSERT_TRUE(radio_knock_post(&k, ARG_SECOND));
    TEST_ASSERT_TRUE(radio_knock_post(&k, ARG_SECOND));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1U, knock_events(&w), "a folded post took a slot");
    TEST_ASSERT_TRUE(radio_knock_take(&k, &arg));
    TEST_ASSERT_EQUAL_HEX8(ARG_FIRST, arg);

    /* Taken: the next post is a knock of its own. */
    TEST_ASSERT_TRUE(radio_knock_post(&k, ARG_SECOND));
    TEST_ASSERT_EQUAL_UINT8(2U, knock_events(&w));
    TEST_ASSERT_TRUE(radio_knock_take(&k, &arg));
    TEST_ASSERT_EQUAL_HEX8(ARG_SECOND, arg);
}

TEST_CASE("a knock the queue refuses is said refused, takes no slot and is posted by the next payment",
          "[radio_knock]")
{
    radio_knock_t k;
    world_t       w;
    uint8_t       arg = 0U;

    (void)memset(&w, 0, sizeof(w));
    w.len     = 2U;
    w.margin  = 1U;
    w.slot[0] = SLOT_OTHER;
    w.used    = 1U;
    s_world   = &w;
    radio_knock_init(&k, &s_door);

    TEST_ASSERT_FALSE_MESSAGE(radio_knock_post(&k, ARG_FIRST), "the refusal was not told");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1U, w.used, "the knock went into its own margin");

    /* A payment that meets the same queue leaves it owed. */
    radio_knock_pay(&k);
    TEST_ASSERT_EQUAL_UINT8(1U, w.used);
    TEST_ASSERT_TRUE_MESSAGE(radio_knock_post(&k, ARG_SECOND),
                             "a post behind an owed knock was not folded");
    TEST_ASSERT_EQUAL_UINT8(1U, w.used);

    /* The Active Object takes the other event: the payment has room. */
    w.used = 0U;
    radio_knock_pay(&k);
    TEST_ASSERT_EQUAL_UINT8(1U, knock_events(&w));
    radio_knock_pay(&k);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1U, knock_events(&w), "a paid knock was posted twice");
    TEST_ASSERT_TRUE(radio_knock_take(&k, &arg));
    TEST_ASSERT_EQUAL_HEX8(ARG_FIRST, arg);
}

TEST_CASE("a knock says it is pending from its post to its take, queued or owed",
          "[radio_knock]")
{
    radio_knock_t k;
    world_t       w;

    (void)memset(&w, 0, sizeof(w));
    w.len   = 1U;
    w.used  = 1U;
    s_world = &w;
    radio_knock_init(&k, &s_door);

    TEST_ASSERT_FALSE(radio_knock_pending(&k));
    TEST_ASSERT_FALSE(radio_knock_post(&k, ARG_FIRST));
    TEST_ASSERT_TRUE_MESSAGE(radio_knock_pending(&k), "an owed knock is not pending");
    w.used = 0U;
    radio_knock_pay(&k);
    TEST_ASSERT_TRUE_MESSAGE(radio_knock_pending(&k), "a queued knock is not pending");
    TEST_ASSERT_TRUE(radio_knock_take(&k, NULL));
    TEST_ASSERT_FALSE_MESSAGE(radio_knock_pending(&k), "a taken knock is still pending");
}

TEST_CASE("a knock made again holds nothing: no payment posts it and no take finds it",
          "[radio_knock]")
{
    radio_knock_t k;
    world_t       w;

    (void)memset(&w, 0, sizeof(w));
    w.len   = 1U;
    w.used  = 1U;
    s_world = &w;
    radio_knock_init(&k, &s_door);

    TEST_ASSERT_FALSE(radio_knock_post(&k, ARG_FIRST));
    radio_knock_init(&k, &s_door);
    w.used = 0U;
    radio_knock_pay(&k);
    TEST_ASSERT_EQUAL_UINT8(0U, w.used);
    TEST_ASSERT_FALSE(radio_knock_take(&k, NULL));
    TEST_ASSERT_TRUE_MESSAGE(radio_knock_post(&k, ARG_SECOND), "a post to a knock made again was folded");
    TEST_ASSERT_EQUAL_UINT8(1U, knock_events(&w));
}

TEST_CASE("in every order of steps a knock is heard once and none is left standing",
          "[radio_knock][orders]")
{
    /* Two tasks post twice each while other traffic fills the queue as fast
     * as the Active Object empties it; the knock is taken only by the Active
     * Object, with the knock's event in hand. Each queue: its length, and the
     * margin the knock leaves. */
    static const uint8_t queue[][2] = { { 1U, 0U }, { 2U, 0U }, { 2U, 1U }, { 3U, 1U }, { 3U, 2U } };
    unsigned long worlds = 0UL;

    for ( size_t i = 0U; (sizeof(queue) / sizeof(queue[0])) > i; ++i )
    {
        worlds += walk_world(queue[i][0], queue[i][1], 2U, 3U, 0U);
    }
    (void)printf("  %lu worlds walked\n", worlds);
    TEST_ASSERT_TRUE_MESSAGE(WORLDS_MIN < worlds, "the walk did not go anywhere");
}

TEST_CASE("in every order of steps a take from anywhere strands no knock and none is heard twice",
          "[radio_knock][orders]")
{
    /* The same, with takes that come at any step and with no event in hand:
     * a knock taken early leaves an event with nothing to say, and a refusal
     * marked after the take owes nothing. */
    static const uint8_t queue[][2] = { { 1U, 0U }, { 2U, 0U }, { 2U, 1U }, { 3U, 1U } };
    unsigned long worlds = 0UL;

    for ( size_t i = 0U; (sizeof(queue) / sizeof(queue[0])) > i; ++i )
    {
        worlds += walk_world(queue[i][0], queue[i][1], 2U, 2U, 2U);
    }
    (void)printf("  %lu worlds walked\n", worlds);
    TEST_ASSERT_TRUE_MESSAGE(WORLDS_MIN < worlds, "the walk did not go anywhere");
}
