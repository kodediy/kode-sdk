/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_slot.c
 * @brief The kodeOS OTA slot: the half that is ours.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "Mockesp_ota_ops.h"

#include "esp_partition.h"

#include "kdrv_slot.h"
#include "kdrv_bounce.h"

/** @brief A handle value that is not zero; every use of it is mocked. */
static esp_ota_handle_t s_handle = (esp_ota_handle_t)0x1234U;

/**
 * @brief Queue an esp_ota_begin that actually hands a handle back.
 *
 * ExpectAnyArgsAndReturn alone is not enough and the difference is invisible:
 * the call returns ESP_OK, the driver reports KODE_OK, and `s_ota` is still
 * zero - so every later call in the case takes the "no session open" branch
 * and is refused for a reason that has nothing to do with what is being
 * tested. An out-parameter needs ReturnThruPtr.
 */
static void expect_begin_ok(void)
{
    /* kdrv_slot_begin() confirms a slot still pending: these are not. */
    esp_ota_get_state_partition_IgnoreAndReturn(ESP_ERR_NOT_FOUND);
    esp_ota_begin_ExpectAnyArgsAndReturn(ESP_OK);
    esp_ota_begin_ReturnThruPtr_out_handle(&s_handle);
}

/** @brief One of the board's real app partitions, by subtype. */
static const esp_partition_t *app_part(esp_partition_subtype_t sub)
{
    const esp_partition_t * const p =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, sub, NULL);
    TEST_ASSERT_NOT_NULL_MESSAGE(p, "the board's table must be in this project");
    return p;
}

/**
 * @brief Start a case with no session open and a stated running partition.
 * @param sub Which app partition the board is running from.
 */
static void slot_fresh(esp_partition_subtype_t sub)
{
    Mockesp_ota_ops_Init();
    esp_ota_get_running_partition_IgnoreAndReturn(app_part(sub));
    esp_ota_abort_IgnoreAndReturn(ESP_OK);
    kdrv_slot_abort();
}

/** @brief Finish a case, checking the mock saw what it was told to expect. */
static void slot_done(void)
{
    kdrv_slot_abort();
    Mockesp_ota_ops_Verify();
    Mockesp_ota_ops_Destroy();
}

/* ---- Which slot ------------------------------------------------------------ */

/* The ping-pong in one case. Running from ota_0 means the writable slot is
 * ota_1, and the other way round - anything else and an update would overwrite
 * the firmware performing it. */
TEST_CASE("the writable slot is always the one not running", "[kdrv_slot]")
{
    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);
    TEST_ASSERT_EQUAL_HEX32(0x820000U, kdrv_slot_address());
    slot_done();

    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_1);
    TEST_ASSERT_EQUAL_HEX32(0x420000U, kdrv_slot_address());
    slot_done();
}

/* Running from factory - the rescue image, reachable by cable only - the
 * writable slot is ota_0. Factory is never a target: the bootloader falls back
 * to it whenever otadata is blank, which is what makes it the rescue. */
TEST_CASE("running from factory, the writable slot is ota_0", "[kdrv_slot]")
{
    slot_fresh(ESP_PARTITION_SUBTYPE_APP_FACTORY);

    TEST_ASSERT_TRUE(kdrv_slot_present());
    TEST_ASSERT_EQUAL_HEX32(0x420000U, kdrv_slot_address());
    TEST_ASSERT_TRUE(kdrv_running_is_factory());

    slot_done();
}

TEST_CASE("the slot is 4MB, which is what the table says", "[kdrv_slot]")
{
    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);

    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0x400000U, kdrv_slot_size(),
        "the three slots grew from 2MB; an image had reached 84.6% of one");

    slot_done();
}

/* An image running from an OTA slot is not the rescue, and `boot_info` prints
 * the difference. */
TEST_CASE("only factory reads as factory", "[kdrv_slot]")
{
    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_1);
    TEST_ASSERT_FALSE(kdrv_running_is_factory());
    slot_done();
}

/* ---- Where we are running -------------------------------------------------- */

TEST_CASE("the running partition names itself and its extent", "[kdrv_slot]")
{
    const char *label = NULL;
    uint32_t    at    = 0U;
    uint32_t    size  = 0U;

    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_1);
    kdrv_running_info(&label, &at, &size);

    TEST_ASSERT_EQUAL_STRING("ota_1", label);
    TEST_ASSERT_EQUAL_HEX32(0x820000U, at);
    TEST_ASSERT_EQUAL_HEX32(0x400000U, size);

    slot_done();
}

/* `boot_info` passes NULL for what it does not print, and a board with no
 * running partition at all is the state a very confused boot is in. Neither
 * may be a crash. */
TEST_CASE("asking for none of it, or asking with nowhere to answer from, is safe",
          "[kdrv_slot]")
{
    const char *label = NULL;
    uint32_t    at    = 0xFFFFFFFFU;
    uint32_t    size  = 0xFFFFFFFFU;

    Mockesp_ota_ops_Init();
    esp_ota_get_running_partition_IgnoreAndReturn(NULL);
    esp_ota_abort_IgnoreAndReturn(ESP_OK);

    kdrv_running_info(NULL, NULL, NULL);

    kdrv_running_info(&label, &at, &size);
    TEST_ASSERT_EQUAL_STRING("?", label);
    TEST_ASSERT_EQUAL_UINT32(0U, at);
    TEST_ASSERT_EQUAL_UINT32(0U, size);

    /* And with nowhere to run from there is no slot either. */
    TEST_ASSERT_FALSE(kdrv_running_is_factory());

    Mockesp_ota_ops_Verify();
    Mockesp_ota_ops_Destroy();
}

/* ---- The write session ----------------------------------------------------- */

TEST_CASE("a second session is refused while one is open", "[kdrv_slot]")
{
    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);

    expect_begin_ok();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_slot_begin());

    /* No second esp_ota_begin is expected: refusing here rather than at the
     * seam is what stops two writers sharing one handle. */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_slot_begin());

    slot_done();
}

TEST_CASE("writing or ending with no session is refused, and never reaches "
          "the seam", "[kdrv_slot]")
{
    static const uint8_t byte = 0x5AU;

    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);

    /* Neither esp_ota_write nor esp_ota_end is expected. The mock fails the
     * case if either is called, which is precisely the assertion. */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_slot_write(&byte, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_slot_end());

    slot_done();
}

/* Aborting what is not open must not reach the seam either: the abort path
 * runs from a failure handler that cannot know how far the transfer got, and
 * it is called twice as a matter of course. */
TEST_CASE("aborting what is not open touches nothing", "[kdrv_slot]")
{
    Mockesp_ota_ops_Init();
    esp_ota_get_running_partition_IgnoreAndReturn(
        app_part(ESP_PARTITION_SUBTYPE_APP_OTA_0));

    kdrv_slot_abort();
    kdrv_slot_abort();

    Mockesp_ota_ops_Verify();
    Mockesp_ota_ops_Destroy();
}

/* A failed begin must not leave a handle behind. If it did, every later
 * session would be refused with KODE_DENIED and the slot would be unusable
 * until the board reset - a refusal that looks exactly like the legitimate
 * one above. */
TEST_CASE("a failed begin leaves no session behind", "[kdrv_slot]")
{
    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);

    esp_ota_get_state_partition_IgnoreAndReturn(ESP_ERR_NOT_FOUND);
    esp_ota_begin_ExpectAnyArgsAndReturn(ESP_ERR_NO_MEM);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_slot_begin());

    expect_begin_ok();
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kdrv_slot_begin(),
        "a failed begin that kept the handle jams the slot until a reset");

    slot_done();
}

/* The one that is easy to get wrong. esp_ota_end consumes the handle whether
 * it succeeds or fails, so the driver must forget it either way - otherwise a
 * rejected image leaves a session nobody can close and no later update can
 * start. */
TEST_CASE("a failed end still consumes the session", "[kdrv_slot]")
{
    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);

    expect_begin_ok();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_slot_begin());

    esp_ota_end_ExpectAnyArgsAndReturn(ESP_ERR_OTA_VALIDATE_FAILED);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_slot_end());

    /* Ending again finds nothing, and a new session opens. */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_slot_end());

    expect_begin_ok();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_slot_begin());

    slot_done();
}

/* A write that the seam refuses is reported rather than counted as progress. */
TEST_CASE("a refused write is an error, and the session stays open",
          "[kdrv_slot]")
{
    static const uint8_t data[8] = { 0 };

    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);

    expect_begin_ok();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_slot_begin());

    esp_ota_write_ExpectAnyArgsAndReturn(ESP_FAIL);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_slot_write(data, sizeof(data)));

    /* Still open: the caller decides whether to retry or abort, and a driver
     * that closed itself here would turn a retryable write into a lost one. */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_slot_begin());

    slot_done();
}

/* ---- Writing through internal RAM ------------------------------------------ */

/** @brief A write two bounces and a part long, so every boundary is crossed. */
#define BOUNCE_CASE_LEN  ((KDRV_BOUNCE_BYTES * 2U) + (KDRV_BOUNCE_BYTES / 2U) + 1U)

static uint8_t  s_seen[BOUNCE_CASE_LEN];   /**< What the flash was handed.  */
static size_t   s_seen_len;                /**< How much of it.             */
static uint32_t s_writes;                  /**< How many writes it took.    */
static size_t   s_largest;                 /**< The longest single write.   */
static uint32_t s_fail_at;                 /**< The write that fails, or 0. */

/** @brief esp_ota_write as the flash sees it: one piece at a time, recorded. */
static esp_err_t record_write(esp_ota_handle_t handle, const void *data,
                              size_t size, int calls)
{
    (void)handle;
    (void)calls;
    s_writes++;
    if ( s_writes == s_fail_at )
    {
        return ESP_FAIL;
    }
    TEST_ASSERT_TRUE((sizeof(s_seen) - s_seen_len) >= size);
    (void)memcpy(&s_seen[s_seen_len], data, size);
    s_seen_len += size;
    if ( size > s_largest )
    {
        s_largest = size;
    }
    return ESP_OK;
}

/** @brief Fill @p buf with a pattern no shifted copy of itself matches. */
static void fill_pattern(uint8_t *buf, size_t len)
{
    for ( size_t i = 0U; i < len; i++ )
    {
        buf[i] = (uint8_t)((i * 7U) + (i >> 8U) + 3U);
    }
}

/** @brief Open a session with esp_ota_write recorded, failing at @p fail_at. */
static void record_from(uint32_t fail_at)
{
    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);
    expect_begin_ok();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_slot_begin());
    s_seen_len = 0U;
    s_writes   = 0U;
    s_largest  = 0U;
    s_fail_at  = fail_at;
    esp_ota_write_Stub(&record_write);
}

/* An image arrives in PSRAM, and esp_flash_write() copies such a buffer to
 * its stack 32 bytes at a time. So the slot hands the flash one bounce of
 * internal RAM at a time instead, and the bytes must survive the trip in
 * order across every boundary. */
TEST_CASE("a write reaches the flash a bounce at a time, every byte in order",
          "[kdrv_slot]")
{
    static uint8_t data[BOUNCE_CASE_LEN];

    fill_pattern(data, sizeof(data));
    record_from(0U);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_slot_write(data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT32(3U, s_writes);
    TEST_ASSERT_EQUAL_size_t(KDRV_BOUNCE_BYTES, s_largest);
    TEST_ASSERT_EQUAL_size_t(sizeof(data), s_seen_len);
    TEST_ASSERT_EQUAL_MEMORY(data, s_seen, sizeof(data));

    slot_done();
}

/* A piece the flash refuses ends the write there: nothing after it is written
 * as if the hole were not there. */
TEST_CASE("a bounce the flash refuses stops the write at that bounce",
          "[kdrv_slot]")
{
    static uint8_t data[BOUNCE_CASE_LEN];

    fill_pattern(data, sizeof(data));
    record_from(2U);

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_slot_write(data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT32(2U, s_writes);

    slot_done();
}

/* ---- The slot as a kfw_sink_t ---------------------------------------------- */

/* The length check the bare call never had: an image too big is refused before
 * the first byte instead of after two minutes of writing over a cable. */
TEST_CASE("the sink refuses an image too big for the slot before opening it",
          "[kdrv_slot]")
{
    const kfw_sink_t * const s = kdrv_slot_sink();

    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);

    /* No esp_ota_begin is expected: refused before anything was opened. */
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, s->begin(NULL, 0x400001U));

    slot_done();
}

/* Exactly the slot's size fits. A `>=` here would make the largest legal image
 * the one thing that cannot be flashed. */
TEST_CASE("an image exactly the size of the slot is accepted", "[kdrv_slot]")
{
    const kfw_sink_t * const s = kdrv_slot_sink();

    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);

    expect_begin_ok();
    TEST_ASSERT_EQUAL_INT(KODE_OK, s->begin(NULL, 0x400000U));

    slot_done();
}

/* A source that cannot say how long the image is passes zero. Refusing that
 * would break every transfer whose length is not known up front. */
TEST_CASE("a sink told no length opens anyway", "[kdrv_slot]")
{
    const kfw_sink_t * const s = kdrv_slot_sink();

    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);

    expect_begin_ok();
    TEST_ASSERT_EQUAL_INT(KODE_OK, s->begin(NULL, 0U));

    slot_done();
}

/* ---- Confirming ourselves -------------------------------------------------- */

/* The branch every ordinary boot takes. Saying "confirmed" for a partition
 * with nothing to confirm would put "kodeOS update confirmed" in the log of
 * every cold boot the board ever performs. */
TEST_CASE("a partition that is not pending has nothing to confirm",
          "[kdrv_slot]")
{
    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);

    esp_ota_get_state_partition_ExpectAnyArgsAndReturn(ESP_OK);
    esp_ota_get_state_partition_ReturnThruPtr_ota_state(
        &(esp_ota_img_states_t){ ESP_OTA_IMG_VALID });

    /* esp_ota_mark_app_valid_cancel_rollback is not expected. */
    TEST_ASSERT_FALSE(kdrv_slot_confirm_self());

    slot_done();
}

/* The happy path, and the only one a board can show you. */
TEST_CASE("a pending partition is confirmed", "[kdrv_slot]")
{
    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_1);

    esp_ota_get_state_partition_ExpectAnyArgsAndReturn(ESP_OK);
    esp_ota_get_state_partition_ReturnThruPtr_ota_state(
        &(esp_ota_img_states_t){ ESP_OTA_IMG_PENDING_VERIFY });
    esp_ota_mark_app_valid_cancel_rollback_ExpectAndReturn(ESP_OK);

    TEST_ASSERT_TRUE(kdrv_slot_confirm_self());

    slot_done();
}

/* The branch that matters most and that no bench produces: pending, and the
 * mark itself failed. Reporting success here would leave a slot that stays
 * unconfirmed forever - and esp_ota_begin refuses to open a session from an
 * unconfirmed partition, so the next update would be refused with no clue
 * why. */
TEST_CASE("a confirmation that failed is not reported as confirmed",
          "[kdrv_slot]")
{
    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_1);

    esp_ota_get_state_partition_ExpectAnyArgsAndReturn(ESP_OK);
    esp_ota_get_state_partition_ReturnThruPtr_ota_state(
        &(esp_ota_img_states_t){ ESP_OTA_IMG_PENDING_VERIFY });
    esp_ota_mark_app_valid_cancel_rollback_ExpectAndReturn(ESP_ERR_NOT_FOUND);

    TEST_ASSERT_FALSE(kdrv_slot_confirm_self());

    slot_done();
}

/* esp_ota_begin() refuses a session from a pending slot, so the next update
 * confirms this one rather than fail for a reason nobody could guess. */
TEST_CASE("opening a session from a pending slot confirms it first", "[kdrv_slot]")
{
    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);

    esp_ota_get_state_partition_ExpectAnyArgsAndReturn(ESP_OK);
    esp_ota_get_state_partition_ReturnThruPtr_ota_state(
        &(esp_ota_img_states_t){ ESP_OTA_IMG_PENDING_VERIFY });
    esp_ota_mark_app_valid_cancel_rollback_ExpectAndReturn(ESP_OK);
    esp_ota_begin_ExpectAnyArgsAndReturn(ESP_OK);
    esp_ota_begin_ReturnThruPtr_out_handle(&s_handle);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_slot_begin());

    kdrv_slot_abort();
    slot_done();
}

TEST_CASE("giving a slot up marks it invalid", "[kdrv_slot]")
{
    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_1);

    esp_ota_mark_app_invalid_rollback_ExpectAndReturn(ESP_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_slot_reject_self());

    slot_done();
}

/* Nowhere to go: no slot before it and no factory, or factory itself. */
TEST_CASE("a slot with nowhere to go back to is not given up", "[kdrv_slot]")
{
    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_1);

    esp_ota_mark_app_invalid_rollback_ExpectAndReturn(ESP_ERR_OTA_ROLLBACK_FAILED);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_slot_reject_self());

    slot_done();
}

/* A state that could not be read at all is not a confirmation either. */
TEST_CASE("a state that cannot be read confirms nothing", "[kdrv_slot]")
{
    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);

    esp_ota_get_state_partition_ExpectAnyArgsAndReturn(ESP_ERR_NOT_FOUND);
    TEST_ASSERT_FALSE(kdrv_slot_confirm_self());

    slot_done();
}

/* ---- The state the console prints ------------------------------------------ */

/* ABORTED is the interesting one: it means the slot was launched and never
 * confirmed, which is exactly "it came home". Everything that is neither that
 * nor UNDEFINED is folded into OTHER, because the console says it out loud
 * and a number nobody can interpret is worse than a word. */
TEST_CASE("the slot's state is mapped, not passed through", "[kdrv_slot]")
{
    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);

    esp_ota_get_state_partition_ExpectAnyArgsAndReturn(ESP_OK);
    esp_ota_get_state_partition_ReturnThruPtr_ota_state(
        &(esp_ota_img_states_t){ ESP_OTA_IMG_ABORTED });
    TEST_ASSERT_EQUAL_INT(KDRV_SLOT_STATE_ABORTED, kdrv_slot_state());

    esp_ota_get_state_partition_ExpectAnyArgsAndReturn(ESP_OK);
    esp_ota_get_state_partition_ReturnThruPtr_ota_state(
        &(esp_ota_img_states_t){ ESP_OTA_IMG_UNDEFINED });
    TEST_ASSERT_EQUAL_INT(KDRV_SLOT_STATE_UNDEFINED, kdrv_slot_state());

    esp_ota_get_state_partition_ExpectAnyArgsAndReturn(ESP_OK);
    esp_ota_get_state_partition_ReturnThruPtr_ota_state(
        &(esp_ota_img_states_t){ ESP_OTA_IMG_VALID });
    TEST_ASSERT_EQUAL_INT(KDRV_SLOT_STATE_OTHER, kdrv_slot_state());

    /* And a read that failed is undefined rather than a stale answer. */
    esp_ota_get_state_partition_ExpectAnyArgsAndReturn(ESP_FAIL);
    TEST_ASSERT_EQUAL_INT(KDRV_SLOT_STATE_UNDEFINED, kdrv_slot_state());

    slot_done();
}

/* ---- What the slot holds --------------------------------------------------- */

TEST_CASE("a slot with no readable descriptor holds no image", "[kdrv_slot]")
{
    kdrv_slot_desc_t desc;

    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);

    esp_ota_get_partition_description_ExpectAnyArgsAndReturn(ESP_ERR_NOT_FOUND);
    TEST_ASSERT_FALSE(kdrv_slot_has_image());

    esp_ota_get_partition_description_ExpectAnyArgsAndReturn(ESP_ERR_NOT_FOUND);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_slot_desc(&desc));

    slot_done();
}

/* What `boot_info` prints about the inactive slot, copied out of the image's
 * own descriptor. The buffers are the driver's, not the caller's, so a
 * descriptor with an unterminated field must not run off the end. */
TEST_CASE("a readable descriptor is copied into the caller's buffers",
          "[kdrv_slot]")
{
    kdrv_slot_desc_t out;
    esp_app_desc_t   desc;

    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);

    (void)memset(&desc, 0, sizeof(desc));
    (void)snprintf(desc.project_name, sizeof(desc.project_name), "%s", "kodeos");
    (void)snprintf(desc.version, sizeof(desc.version), "%s", "00.00.09");
    (void)snprintf(desc.date, sizeof(desc.date), "%s", "Aug 27 2026");
    (void)snprintf(desc.time, sizeof(desc.time), "%s", "10:11:12");
    (void)snprintf(desc.idf_ver, sizeof(desc.idf_ver), "%s", "v9.9.9");

    esp_ota_get_partition_description_ExpectAnyArgsAndReturn(ESP_OK);
    esp_ota_get_partition_description_ReturnThruPtr_app_desc(&desc);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_slot_desc(&out));
    TEST_ASSERT_EQUAL_STRING("kodeos", out.project);
    TEST_ASSERT_EQUAL_STRING("00.00.09", out.version);
    TEST_ASSERT_EQUAL_STRING("v9.9.9", out.idf_ver);

    slot_done();
}

TEST_CASE("describing nothing is refused rather than crashed", "[kdrv_slot]")
{
    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_slot_desc(NULL));
    slot_done();
}

/* ---- Erasing, on real flash ------------------------------------------------ */

/* Only the header is erased, and that is a deliberate trade: killing the first
 * 64KB is enough to make an image unbootable, and erasing 4MB to say so
 * would stall the board for seconds with the GUI frozen.
 *
 * This runs against the real emulated flash, so it can assert both halves -
 * that the header is gone AND that the megabytes behind it were not touched. */
TEST_CASE("erasing kills the header and leaves the rest alone", "[kdrv_slot]")
{
    static uint8_t pattern[256];
    uint8_t        back[256];

    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);

    const esp_partition_t * const p =
        app_part(ESP_PARTITION_SUBTYPE_APP_OTA_1);   /* the writable one */

    (void)memset(pattern, 0x5A, sizeof(pattern));

    /* One block inside the header, one just past it. */
    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_partition_erase_range(p, 0U, 0x20000U));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_write(p, 0U, pattern, sizeof(pattern)));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_write(p, 0x10000U, pattern, sizeof(pattern)));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_slot_erase());

    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_partition_read(p, 0U, back, sizeof(back)));
    for ( unsigned i = 0U; sizeof(back) > i; ++i )
    {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFFU, back[i],
            "the header must be gone, or the bootloader still jumps to it");
    }

    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_read(p, 0x10000U, back, sizeof(back)));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(pattern, back, sizeof(back),
        "erasing the whole 4MB would freeze the GUI for seconds");

    slot_done();
}

/* ---- otadata, on real flash ------------------------------------------------ */

/** @brief The board's otadata partition. */
static const esp_partition_t *otadata(void)
{
    const esp_partition_t * const p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    TEST_ASSERT_NOT_NULL(p);
    return p;
}

/* Blank is otadata's default state, and the whole rescue model rests on it:
 * the ROM bootloader falls back to factory whenever otadata is blank or
 * invalid. */
TEST_CASE("cleared otadata reads as blank", "[kdrv_slot]")
{
    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_otadata_clear());
    TEST_ASSERT_TRUE(kdrv_otadata_blank());

    slot_done();
}

/* One byte anywhere is enough. The walk covers the whole partition rather than
 * sampling its head, because a partially written otadata is exactly what a
 * power cut mid-update leaves behind - and the interesting bytes are not
 * necessarily at the front. */
TEST_CASE("a single byte at the very end makes otadata not blank",
          "[kdrv_slot]")
{
    const esp_partition_t * const ota = otadata();
    static const uint8_t          mark = 0x00U;

    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_otadata_clear());

    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_write(ota, ota->size - 1U, &mark, sizeof(mark)));

    TEST_ASSERT_FALSE_MESSAGE(kdrv_otadata_blank(),
        "a head-only check would call this board's otadata blank");

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_otadata_clear());
    TEST_ASSERT_TRUE(kdrv_otadata_blank());

    slot_done();
}

/* ---- Choosing the other slot ----------------------------------------------- */

/* esp_ota_set_boot_partition verifies the image before it writes otadata,
 * which is what turns "a corrupt update bricks the next boot" into "a corrupt
 * update is refused now, with a console still listening". The refusal has to
 * be passed on rather than swallowed - AO_Boot's final `else` prints it. */
TEST_CASE("a slot that cannot be selected is reported, not swallowed",
          "[kdrv_slot]")
{
    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);

    esp_ota_set_boot_partition_ExpectAnyArgsAndReturn(ESP_ERR_OTA_VALIDATE_FAILED);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_otadata_select_slot());

    slot_done();
}

TEST_CASE("a slot that can be selected reports success", "[kdrv_slot]")
{
    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_1);

    esp_ota_set_boot_partition_ExpectAnyArgsAndReturn(ESP_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_otadata_select_slot());

    slot_done();
}

/* ---- Every app partition, by id ----------------------------------------------
 * `info` prints what sits in each of the three kodeOS partitions, running or
 * not. The reads are the same descriptor read kdrv_slot_desc() makes for the
 * inactive slot; what is new is being asked by name, and being told which of
 * the three the board is running from.
 */

/** @brief Lay a descriptor naming @p project for the next description read. */
static void expect_desc(esp_app_desc_t *desc, const char *project)
{
    (void)memset(desc, 0, sizeof(*desc));
    (void)snprintf(desc->project_name, sizeof(desc->project_name), "%s", project);
    (void)snprintf(desc->version, sizeof(desc->version), "%s", "00.00.18");
    esp_ota_get_partition_description_ExpectAnyArgsAndReturn(ESP_OK);
    esp_ota_get_partition_description_ReturnThruPtr_app_desc(desc);
}

/* The slot is where this firmware's own update goes, and the next boot hands
 * the board to whatever it holds. What esp_ota_end() accepts is any image
 * built for the chip, an app's included, so the project's name is what tells
 * an update from something else. */
TEST_CASE("an image of the running project ends well", "[kdrv_slot]")
{
    esp_app_desc_t   desc;
    kdrv_slot_desc_t running;

    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_running_desc(&running));

    expect_begin_ok();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_slot_begin());

    esp_ota_end_ExpectAnyArgsAndReturn(ESP_OK);
    expect_desc(&desc, running.project);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_slot_end());

    slot_done();
}

TEST_CASE("an image of another project is refused at the end", "[kdrv_slot]")
{
    esp_app_desc_t desc;

    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);

    expect_begin_ok();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_slot_begin());

    esp_ota_end_ExpectAnyArgsAndReturn(ESP_OK);
    expect_desc(&desc, "buttons_led");
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_ERROR, kdrv_slot_end(),
        "an app's image in the slot would boot in the OS's place");

    /* The session is over all the same, and a new one opens. */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_slot_end());
    expect_begin_ok();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_slot_begin());

    slot_done();
}

TEST_CASE("an image with no description to read is refused at the end",
          "[kdrv_slot]")
{
    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);

    expect_begin_ok();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_slot_begin());

    esp_ota_end_ExpectAnyArgsAndReturn(ESP_OK);
    esp_ota_get_partition_description_ExpectAnyArgsAndReturn(ESP_ERR_NOT_FOUND);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_slot_end());

    slot_done();
}

TEST_CASE("every app partition can be described by id, and only the running "
          "one says so", "[kdrv_slot]")
{
    static esp_app_desc_t descs[3];
    kdrv_slot_info_t      info;

    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);

    expect_desc(&descs[0], "rescue");
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_slot_info(KDRV_SLOT_ID_FACTORY, &info));
    TEST_ASSERT_EQUAL_STRING("factory", info.label);
    TEST_ASSERT_EQUAL_HEX32(0x20000U, info.address);
    TEST_ASSERT_EQUAL_HEX32(0x400000U, info.size);
    TEST_ASSERT_FALSE(info.running);
    TEST_ASSERT_TRUE(info.has_image);
    TEST_ASSERT_EQUAL_STRING("rescue", info.desc.project);
    TEST_ASSERT_EQUAL_STRING("00.00.18", info.desc.version);

    expect_desc(&descs[1], "kodeos");
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_slot_info(KDRV_SLOT_ID_OTA_0, &info));
    TEST_ASSERT_EQUAL_STRING("ota_0", info.label);
    TEST_ASSERT_EQUAL_HEX32(0x420000U, info.address);
    TEST_ASSERT_TRUE_MESSAGE(info.running, "the board runs from ota_0 here");
    TEST_ASSERT_EQUAL_STRING("kodeos", info.desc.project);

    expect_desc(&descs[2], "kodeos");
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_slot_info(KDRV_SLOT_ID_OTA_1, &info));
    TEST_ASSERT_EQUAL_STRING("ota_1", info.label);
    TEST_ASSERT_EQUAL_HEX32(0x820000U, info.address);
    TEST_ASSERT_FALSE(info.running);

    slot_done();
}

TEST_CASE("a partition with no readable image is reported empty, with its "
          "geometry intact", "[kdrv_slot]")
{
    kdrv_slot_info_t info;

    slot_fresh(ESP_PARTITION_SUBTYPE_APP_FACTORY);

    esp_ota_get_partition_description_ExpectAnyArgsAndReturn(ESP_ERR_NOT_FOUND);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_slot_info(KDRV_SLOT_ID_OTA_1, &info));
    TEST_ASSERT_FALSE(info.has_image);
    TEST_ASSERT_EQUAL_STRING("", info.desc.project);
    TEST_ASSERT_EQUAL_STRING("", info.desc.version);
    TEST_ASSERT_EQUAL_HEX32(0x820000U, info.address);
    TEST_ASSERT_EQUAL_HEX32(0x400000U, info.size);
    TEST_ASSERT_FALSE(info.running);

    slot_done();
}

TEST_CASE("an id off the end, or nowhere to answer to, is refused rather "
          "than read", "[kdrv_slot]")
{
    kdrv_slot_info_t info;

    slot_fresh(ESP_PARTITION_SUBTYPE_APP_OTA_0);

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_slot_info(KDRV_SLOT_ID_COUNT, &info));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_slot_info(KDRV_SLOT_ID_OTA_0, NULL));

    slot_done();
}

TEST_CASE("the dump's reason is refused without room to put it, and cleared "
          "when there is none to give", "[kdrv_slot]")
{
    char buf[8] = { 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x' };

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_slot_coredump_reason(NULL, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_slot_coredump_reason(buf, 0U));

    /* The host build keeps no core dump, so there is never a reason to give:
     * the buffer comes back an empty string, not whatever it held. */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_slot_coredump_reason(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_CHAR('\0', buf[0]);
}
