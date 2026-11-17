/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_http_off64.c
 * @brief The ranged GET built with CONFIG_KODE_HTTP_OFFSET_64: an offset past
 *        4 GB goes through both doors with every bit.
 *
 * The project builds the 32-bit kode_http_off_t, which is the default. This
 * file compiles the two sources that name the type again, with the option on
 * and under names of their own, as test_kode_event_os.c does for the standard
 * layer. Only the doors differ between the two builds: the request and its
 * answer carry 64 bits either way.
 */
#define CONFIG_KODE_HTTP_OFFSET_64 1

#define kode_block_net_lookup       kode_block_net_lookup_off64
#define kode_block_net_ping         kode_block_net_ping_off64
#define kode_block_http_get_buffer  kode_block_http_get_buffer_off64
#define kode_block_http_get_range   kode_block_http_get_range_off64
#define kode_block_http_post_buffer kode_block_http_post_buffer_off64
#define kode_block_udp_send         kode_block_udp_send_off64
#define kode_sys_net_lookup         kode_sys_net_lookup_off64
#define kode_sys_net_ping           kode_sys_net_ping_off64
#define kode_sys_http_get           kode_sys_http_get_off64
#define kode_sys_http_get_range     kode_sys_http_get_range_off64
#define kode_sys_http_post          kode_sys_http_post_off64
#define kode_sys_udp_send           kode_sys_udp_send_off64
#define kode_http_get               kode_http_get_off64
#define kode_http_post              kode_http_post_off64
#define kode_http_get_range         kode_http_get_range_off64
#define kode_http_status            kode_http_status_off64
#define kode_http_state_free        kode_http_state_free_off64
/* The door's one call into AO_Net, taken here so the case reads what it was
 * handed and no ask is left waiting on an AO_Net that is not running. */
#define net_http_range_ask          off64_range_ask
#include "kode_sys_net.c"
#include "kode_http.c"

#include <stdatomic.h>

#include "unity.h"

#include "ao_spy.h"
#include "fakes.h"
#include "khal_once.h"
#include "kode_sys_priv.h"
#include "qf_support.h"

/** @brief An offset no 32-bit type holds. */
#define OFF64_FAR   6000000000ULL
/** @brief Room of the buffer the cases lend. */
#define OFF64_CAP   64U
/** @brief An address the cases name. */
#define OFF64_URL   "https://example.test/big.bin"

_Static_assert(sizeof(kode_http_off_t) == sizeof(uint64_t),
               "with the option the offset is 64 bits wide");
_Static_assert(KODE_HTTP_OFF_MAX == UINT64_MAX, "and its last value is the type's");

/** @brief What the door handed AO_Net's ask, and how often. */
static uint64_t s_ask_offset;
static uint32_t s_ask_cap;
static unsigned s_asks;

kode_status_t off64_range_ask(QActive *who, const char *url, uint64_t offset, void *buf,
                              uint32_t cap)
{
    (void)who; (void)url; (void)buf;
    s_ask_offset = offset;
    s_ask_cap    = cap;
    s_asks++;
    return KODE_OK;
}

TEST_CASE("with 64-bit offsets the request door carries one past 4 GB whole",
          "[kode_http][off64]")
{
    static uint8_t buf[OFF64_CAP];

    qf_support_init();
    s_asks = 0U;

    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kode_sys_http_get_range(ao_spy(), OFF64_URL, UINT64_MAX, buf, OFF64_CAP));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_asks,
        "a range that runs past the last offset is refused before it is asked");

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kode_sys_http_get_range(ao_spy(), OFF64_URL, OFF64_FAR, buf, OFF64_CAP));
    TEST_ASSERT_EQUAL_UINT(1U, s_asks);
    TEST_ASSERT_EQUAL_UINT64(OFF64_FAR, s_ask_offset);
    TEST_ASSERT_EQUAL_UINT32(OFF64_CAP, s_ask_cap);
}

/* The task's call checks the range against the type's own last value, so an
 * offset past 4 GB is not refused as one that runs off the end: it gets as
 * far as asking whether the board has a network. */
TEST_CASE("with 64-bit offsets the task's call takes one past 4 GB",
          "[kode_http][off64]")
{
    static uint8_t  buf[OFF64_CAP];
    uint32_t        got   = 99U;
    kode_http_off_t total = 99U;

    fake_mutex_real(true);
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);

    TEST_ASSERT_FALSE(kode_http_get_range(OFF64_URL, UINT64_MAX, buf, OFF64_CAP, &got, &total));
    TEST_ASSERT_EQUAL_STRING("the range runs past the last offset", kode_error());

    TEST_ASSERT_FALSE(kode_http_get_range(OFF64_URL, OFF64_FAR, buf, OFF64_CAP, &got, &total));
    TEST_ASSERT_EQUAL_STRING("no network: call kode_wifi_connect() first", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, got);

    TEST_ASSERT_TRUE_MESSAGE(9000000000ULL == total_of(9000000000LL),
                             "a total past 4 GB is reported whole");
}
