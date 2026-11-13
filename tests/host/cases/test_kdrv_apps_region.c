/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_apps_region.c
 * @brief The apps region against a real emulated flash.
 */
#include <stddef.h>
#include <string.h>

#include "unity.h"

#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_partition.h"
#include "esp_private/partition_linux.h"
#include "esp_rom_crc.h"

#include "fakes.h"
#include "kapp_desc.h"
#include "kapps_fmt.h"
#include "kdrv_apps.h"
#include "kdrv_bounce.h"
#include "kdrv_kfw.h"
#include "khal_crc32.h"

/** @brief The region, found the same way the driver finds it. */
static const esp_partition_t *region(void)
{
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)KAPPS_PART_SUBTYPE,
        "apps");
    TEST_ASSERT_NOT_NULL_MESSAGE(p, "the test project's partition table must "
                                    "carry the board's apps region");
    return p;
}

/* Every case leaves the region as it found it: the Active Objects in this binary share it. */
/**
 * @brief The driver, brought up if nobody has yet, with no session open.
 *
 * The abort is not decoration. Unity abandons a case at its first failed
 * assertion, so a case that fails between begin() and abort() leaves the write
 * session open - and every later case then gets KODE_DENIED from begin() for a
 * reason that has nothing to do with what it is testing. One failure became
 * two the first time this was written without it.
 */
static void apps_ready(void)
{
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_init());
    TEST_ASSERT_TRUE(kdrv_apps_present());
    kdrv_apps_write_abort();
}

/* ---- The region as it describes itself ------------------------------------- */

/* The table's own arithmetic, read back through the driver. The region takes
 * every byte between the last OS slot and the internal FAT. It is not bounded
 * by the 16MB mark: the cache maps 32-bit addresses
 * (CONFIG_BOOTLOADER_CACHE_32BIT_ADDR_QUAD_FLASH), so an image above that line
 * is as executable as one below it. The load-bearing property is the base: it
 * never moves, so installed apps keep their addresses across a table change
 * that moves only the top, and this table is the one that ships. */
TEST_CASE("the region is where the table says, and ends where the FAT begins",
          "[apps_region]")
{
    apps_ready();

    TEST_ASSERT_EQUAL_HEX32(0xC40000U, kdrv_apps_address());
    TEST_ASSERT_EQUAL_HEX32(0xBC0000U, kdrv_apps_region_size());
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(
        0x1800000U, kdrv_apps_address() + kdrv_apps_region_size(),
        "the region reaches the internal FAT and stops there");
}

/* ---- Power cuts ------------------------------------------------------------ */

/* ESP-IDF's emulated flash can fail on the n-th 4-byte write or sector erase,
 * and every one after it, leaving what was written before in place - which is
 * what a power cut does to NOR flash. With CONFIG_ESP_PARTITION_ENABLE_STATS
 * on, these cases cut each write the region makes at every step it has, and
 * read back what a boot would read. */

/** @brief More steps than a directory commit or a request write ever takes. */
#define CUT_STEPS_MAX 600U

/** @brief Stop cutting: every write and erase succeeds again. */
static void cut_off(void)
{
    esp_partition_fail_after(SIZE_MAX, ESP_PARTITION_FAIL_AFTER_MODE_BOTH);
}

/**
 * @brief Read one directory copy straight from flash and judge it as a boot
 *        does, with the driver's own classifier.
 *
 * @param off Offset of the copy in the region.
 * @param gen Receives its generation.
 * @return What kapps_dir_classify() makes of it.
 */
static kapps_dir_copy_t copy_kind(uint32_t off, uint32_t *gen)
{
    static kapps_dir_t d;

    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_partition_read(region(), off, &d, sizeof(d)));
    *gen = d.gen;
    return kapps_dir_classify(&d,
        esp_rom_crc32_le(UINT32_MAX, (const uint8_t *)&d, offsetof(kapps_dir_t, crc)),
        kdrv_apps_region_size());
}

/**
 * @brief The generation the next boot would believe, or 0 when it would
 *        format the region or shut it.
 *
 * A copy the driver calls FOREIGN shuts the region on every boot, so a torn
 * write must never produce one: that is asserted here, for both copies.
 */
static uint32_t believed_gen(void)
{
    uint32_t a_gen = 0U;
    uint32_t b_gen = 0U;
    kapps_dir_copy_t const a = copy_kind(KAPPS_DIR_A_OFF, &a_gen);
    kapps_dir_copy_t const b = copy_kind(KAPPS_DIR_B_OFF, &b_gen);
    uint32_t gen = 0U;

    TEST_ASSERT_NOT_EQUAL_MESSAGE(KAPPS_DIR_COPY_FOREIGN, a,
        "a torn directory copy reads as another format's, and the region would stay shut");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(KAPPS_DIR_COPY_FOREIGN, b,
        "a torn directory copy reads as another format's, and the region would stay shut");
    switch ( kapps_dir_pick(KAPPS_DIR_COPY_OK == a, a_gen, KAPPS_DIR_COPY_OK == b, b_gen) )
    {
    case KAPPS_DIR_PICK_A:
        gen = a_gen;
        break;
    case KAPPS_DIR_PICK_B:
        gen = b_gen;
        break;
    default:
        break;
    }
    return gen;
}

/* The whole reason for two copies. A commit erases and writes the copy that
 * is not believed in; a cut anywhere in that leaves the believed one alone,
 * so the next boot reads what this one had, or - when the cut came after the
 * last word landed - the new one. Never neither: that would format the
 * region, and every installed app would be gone. */
TEST_CASE("a power cut anywhere in a directory commit leaves a copy a boot believes",
          "[apps_region][power_cut]")
{
    static kapps_dir_t d;
    kode_status_t      st     = KODE_ERROR;
    uint32_t           cut    = 0U;

    apps_ready();
    d = *kdrv_apps_dir();
    uint32_t const before = kdrv_apps_dir()->gen;

    for ( cut = 0U; (CUT_STEPS_MAX > cut) && (KODE_OK != st); ++cut )
    {
        esp_partition_fail_after(cut, ESP_PARTITION_FAIL_AFTER_MODE_BOTH);
        st = kdrv_apps_dir_commit(&d);
        cut_off();

        uint32_t const gen = believed_gen();
        TEST_ASSERT_TRUE_MESSAGE((gen == before) || ((before + 1U) == gen),
            "after a cut, a boot would believe neither the old directory nor the new");
        if ( KODE_OK != st )
        {
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(before, kdrv_apps_dir()->gen,
                "a commit that failed was adopted anyway");
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, st, "no commit got through uncut");
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(1U, cut, "the cuts never landed");
    TEST_ASSERT_EQUAL_UINT32(before + 1U, believed_gen());
}

/* The bootloader jumps on a request whose magic and CRC hold and whose state
 * asks for a boot. A request half-written is none of those; a cut after its
 * last word is the request that was asked for. Either way a cut never leaves
 * a live request pointing anywhere but where the writer meant. */
TEST_CASE("a power cut anywhere in a boot request write never points a boot elsewhere",
          "[apps_region][power_cut]")
{
    kapps_req_t   req;
    kode_status_t st  = KODE_ERROR;
    uint32_t      cut = 0U;

    apps_ready();
    for ( cut = 0U; (CUT_STEPS_MAX > cut) && (KODE_OK != st); ++cut )
    {
        esp_partition_fail_after(cut, ESP_PARTITION_FAIL_AFTER_MODE_BOTH);
        st = kdrv_apps_req_set(KAPPS_DATA_OFF, 0x8000U, KAPPS_REQ_STATE_NEW);
        cut_off();

        TEST_ASSERT_EQUAL_INT(ESP_OK, esp_partition_read(region(), KAPPS_REQ_OFF,
                                                         &req, sizeof(req)));
        bool const live = kapps_req_live(&req,
            esp_rom_crc32_le(UINT32_MAX, (const uint8_t *)&req,
                             offsetof(kapps_req_t, crc)));
        if ( KODE_OK == st )
        {
            TEST_ASSERT_TRUE_MESSAGE(live, "a written request is not live");
        }
        if ( live )
        {
            TEST_ASSERT_EQUAL_HEX32(KAPPS_DATA_OFF, req.offset);
            TEST_ASSERT_EQUAL_HEX32(0x8000U, req.size);
            TEST_ASSERT_EQUAL_HEX32(KAPPS_REQ_STATE_NEW, req.state);
        }
    }
    kdrv_apps_req_clear();
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, st, "no request got through uncut");
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(1U, cut, "the cuts never landed");
}

/* ---- The directory, and its A/B alternation -------------------------------- */

TEST_CASE("a commit with nothing to commit is refused", "[apps_region]")
{
    apps_ready();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_apps_dir_commit(NULL));
}

/* The point of A/B: a commit never overwrites the copy currently believed in,
 * so a power cut mid-write leaves the old one intact and readable. Two commits
 * must therefore land on two different sectors, and come back again. */
TEST_CASE("consecutive commits alternate between the two copies",
          "[apps_region]")
{
    const esp_partition_t * const p = region();
    kapps_dir_t                   d;
    kapps_dir_t                   a;
    kapps_dir_t                   b;

    apps_ready();

    d = *kdrv_apps_dir();
    uint32_t const gen0 = d.gen;

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&d));
    TEST_ASSERT_EQUAL_UINT32(gen0 + 1U, kdrv_apps_dir()->gen);

    d = *kdrv_apps_dir();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&d));
    TEST_ASSERT_EQUAL_UINT32(gen0 + 2U, kdrv_apps_dir()->gen);

    /* Both copies now hold a real directory, and they differ by exactly one
     * generation - which is the state that makes the boot-time choice
     * meaningful in the first place. */
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_read(p, KAPPS_DIR_A_OFF, &a, sizeof(a)));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_read(p, KAPPS_DIR_B_OFF, &b, sizeof(b)));

    TEST_ASSERT_EQUAL_HEX32(KAPPS_DIR_MAGIC, a.magic);
    TEST_ASSERT_EQUAL_HEX32(KAPPS_DIR_MAGIC, b.magic);

    uint32_t const hi = (a.gen > b.gen) ? a.gen : b.gen;
    uint32_t const lo = (a.gen > b.gen) ? b.gen : a.gen;
    TEST_ASSERT_EQUAL_UINT32(gen0 + 2U, hi);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, hi - lo,
        "the older copy is the previous generation, not a stale ancient one");
}

/* A commit seals what it writes: magic, version and CRC are the driver's, not
 * the caller's, so a caller that forgot them cannot write an unreadable
 * directory. */
TEST_CASE("a commit seals the record whatever the caller passed",
          "[apps_region]")
{
    kapps_dir_t d;

    apps_ready();

    d = *kdrv_apps_dir();
    d.magic   = 0xDEADBEEFU;
    d.version = 0xFFU;
    d.crc     = 0U;

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&d));

    const kapps_dir_t * const now = kdrv_apps_dir();
    TEST_ASSERT_EQUAL_HEX32(KAPPS_DIR_MAGIC, now->magic);
    TEST_ASSERT_EQUAL_UINT32(KAPPS_FMT_VERSION, now->version);
    TEST_ASSERT_EQUAL_HEX32(
        esp_rom_crc32_le(UINT32_MAX, (const uint8_t *)now,
                         offsetof(kapps_dir_t, crc)),
        now->crc);
}

/* What the caller put in the entries survives the round trip through flash.
 * The directory is the only record of where anything is; if it did not, the
 * region would be a set of images nobody can find. Note what an entry does
 * NOT carry: a name. The name lives in the image's own app descriptor, which
 * is why a listing costs a read per entry rather than one read of this. */
TEST_CASE("entries survive the trip through flash", "[apps_region]")
{
    kapps_dir_t d;

    apps_ready();

    d = *kdrv_apps_dir();
    kapps_dir_t const saved = d;

    d.count       = 1U;
    d.e[0].offset = KAPPS_DATA_OFF;
    d.e[0].size   = 0x20000U;
    d.e[0].seq    = 7U;
    d.e[0].flags  = 0x3U;

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&d));

    const kapps_dir_t * const now = kdrv_apps_dir();
    TEST_ASSERT_EQUAL_UINT(1U, now->count);
    TEST_ASSERT_EQUAL_HEX32(KAPPS_DATA_OFF, now->e[0].offset);
    TEST_ASSERT_EQUAL_HEX32(0x20000U, now->e[0].size);
    TEST_ASSERT_EQUAL_UINT32(7U, now->e[0].seq);
    TEST_ASSERT_EQUAL_HEX32(0x3U, now->e[0].flags);

    /* Put the region back the way the rest of the binary expects it. */
    kapps_dir_t restore = saved;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&restore));
    TEST_ASSERT_EQUAL_UINT(saved.count, kdrv_apps_dir()->count);
}

/* The copy to the card reads an installed image back out of the region, so
 * what comes out must be what went in, at the region-relative offset the
 * directory records. */
TEST_CASE("an extent written to the region reads back byte for byte",
          "[apps_region]")
{
    static uint8_t out[300];
    static uint8_t in[300];

    apps_ready();

    for ( size_t i = 0U; sizeof(out) > i; ++i )
    {
        out[i] = (uint8_t)(i * 7U);
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_write_begin(KAPPS_DATA_OFF + KAPPS_ALIGN, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write(out, sizeof(out)));
    kdrv_apps_write_abort();

    (void)memset(in, 0, sizeof(in));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_read(KAPPS_DATA_OFF + KAPPS_ALIGN, in, sizeof(in)));
    TEST_ASSERT_EQUAL_MEMORY(out, in, sizeof(out));
}

/* Below the data area is the directory and the boot request; past the end is
 * the FAT. Neither is an app, and a read there is a caller's bug. */
TEST_CASE("a read outside the data area is refused", "[apps_region]")
{
    uint8_t b[16];

    apps_ready();

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_apps_read(0U, b, sizeof(b)));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_apps_read(region()->size - 8U, b, sizeof(b)));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kdrv_apps_read(KAPPS_DATA_OFF, NULL, sizeof(b)));
}

/* ---- The boot request ------------------------------------------------------ */

/* The protocol in one case. kodeOS writes NEW; only the bootloader turns it
 * into TRIED, with a four-byte write, and only TRIED means "we really jumped".
 * A NEW record read back as TRIED would make every refused boot look like a
 * completed one. */
TEST_CASE("a fresh request is NEW, and NEW is not TRIED", "[apps_region]")
{
    const esp_partition_t * const p = region();
    kapps_req_t                   req;
    uint32_t                      off = 0xFFFFFFFFU;

    apps_ready();

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_req_set(KAPPS_DATA_OFF, 0x8000U, KAPPS_REQ_STATE_NEW));

    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_read(p, KAPPS_REQ_OFF, &req, sizeof(req)));
    TEST_ASSERT_EQUAL_HEX32(KAPPS_REQ_MAGIC, req.magic);
    TEST_ASSERT_EQUAL_HEX32(KAPPS_REQ_STATE_NEW, req.state);
    TEST_ASSERT_EQUAL_HEX32(KAPPS_DATA_OFF, req.offset);

    TEST_ASSERT_FALSE_MESSAGE(kdrv_apps_req_tried(&off),
        "a request nobody jumped to must not read as a completed run");

    kdrv_apps_req_clear();
}

/* The bootloader's half, done here the way the bootloader does it: NEW is all
 * ones over TRIED's zeros, so flipping the state needs no erase - which is
 * exactly why it can be done from a bootloader with the cache off. */
TEST_CASE("a request the bootloader marked TRIED is reported with its offset",
          "[apps_region]")
{
    const esp_partition_t * const p = region();
    uint32_t                      off = 0U;
    static const uint32_t         tried = KAPPS_REQ_STATE_TRIED;

    apps_ready();

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_req_set(KAPPS_DATA_OFF + KAPPS_ALIGN, 0x8000U,
                          KAPPS_REQ_STATE_NEW));

    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_write(p, KAPPS_REQ_OFF + offsetof(kapps_req_t, state),
                            &tried, sizeof(tried)));

    TEST_ASSERT_TRUE(kdrv_apps_req_tried(&off));
    TEST_ASSERT_EQUAL_HEX32(KAPPS_DATA_OFF + KAPPS_ALIGN, off);

    /* And clearing it is what stops the next boot reading as another return
     * from the same app. */
    kdrv_apps_req_clear();
    TEST_ASSERT_FALSE(kdrv_apps_req_tried(&off));
}

/* The sticky request is written with the same geometry checks and the same
 * CRC; only the state word differs. And it is retired the same way: the
 * bootloader's bit-clearing write takes it to TRIED, which kodeOS then reads
 * as "back from an app" exactly as it does for NEW. */
TEST_CASE("an ALWAYS request is written, and TRIED is recognised from it",
          "[apps_region]")
{
    const esp_partition_t * const p = region();
    kapps_req_t                   req;
    uint32_t                      off = 0U;
    static const uint32_t         tried = KAPPS_REQ_STATE_TRIED;

    apps_ready();

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_req_set(KAPPS_DATA_OFF, 0x8000U, KAPPS_REQ_STATE_ALWAYS));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_read(p, KAPPS_REQ_OFF, &req, sizeof(req)));
    TEST_ASSERT_EQUAL_HEX32(KAPPS_REQ_STATE_ALWAYS, req.state);
    TEST_ASSERT_FALSE(kdrv_apps_req_tried(&off));

    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_write(p, KAPPS_REQ_OFF + offsetof(kapps_req_t, state),
                            &tried, sizeof(tried)));
    TEST_ASSERT_TRUE(kdrv_apps_req_tried(&off));
    TEST_ASSERT_EQUAL_HEX32(KAPPS_DATA_OFF, off);

    kdrv_apps_req_clear();
}

/* kdrv_apps_req_present() answers "is any record standing", not "is it TRIED".
 * If the bootloader's retire write ever failed, an ALWAYS or ARMED word would
 * be left in the sector while kodeOS runs; AO_Boot uses this to clear it, so a
 * later keep-list reset cannot jump into that stale sketch. */
TEST_CASE("a standing request that is not TRIED reads present, so it can be cleared",
          "[apps_region]")
{
    const esp_partition_t * const p = region();
    uint32_t                      off = 0U;
    static const uint32_t         armed = KAPPS_REQ_STATE_ARMED;
    static const uint32_t         tried = KAPPS_REQ_STATE_TRIED;

    apps_ready();

    /* Erased flash: nothing standing. */
    TEST_ASSERT_FALSE(kdrv_apps_req_present());

    /* A fresh sticky launch: present, but not yet TRIED. */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_req_set(KAPPS_DATA_OFF, 0x8000U, KAPPS_REQ_STATE_ALWAYS));
    TEST_ASSERT_TRUE(kdrv_apps_req_present());
    TEST_ASSERT_FALSE(kdrv_apps_req_tried(&off));

    /* The bootloader arms it (a bit-clearing write to ARMED). Still present,
     * still not the TRIED that means "back from an app". This is the state a
     * failed retire write would strand. */
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_write(p, KAPPS_REQ_OFF + offsetof(kapps_req_t, state),
                            &armed, sizeof(armed)));
    TEST_ASSERT_TRUE(kdrv_apps_req_present());
    TEST_ASSERT_FALSE(kdrv_apps_req_tried(&off));

    /* A TRIED record is present too - present means simply "not erased". */
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_write(p, KAPPS_REQ_OFF + offsetof(kapps_req_t, state),
                            &tried, sizeof(tried)));
    TEST_ASSERT_TRUE(kdrv_apps_req_present());
    TEST_ASSERT_TRUE(kdrv_apps_req_tried(&off));

    /* Clearing erases the sector: nothing standing again. */
    kdrv_apps_req_clear();
    TEST_ASSERT_FALSE(kdrv_apps_req_present());
    TEST_ASSERT_FALSE(kdrv_apps_req_tried(&off));
}

/* ARMED is a sticky run the bootloader launched and did not retire. It is not
 * TRIED, and only it answers kdrv_apps_req_armed(), with the extent's offset:
 * a request that was never launched (NEW, ALWAYS) or one already spent (TRIED)
 * does not. */
TEST_CASE("an ARMED request reports its offset, and no other state does",
          "[apps_region]")
{
    const esp_partition_t * const p = region();
    uint32_t                      off = 0U;
    static const uint32_t         armed = KAPPS_REQ_STATE_ARMED;
    static const uint32_t         tried = KAPPS_REQ_STATE_TRIED;

    apps_ready();

    TEST_ASSERT_FALSE_MESSAGE(kdrv_apps_req_armed(&off), "erased flash is no request");

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_req_set(KAPPS_DATA_OFF + KAPPS_ALIGN, 0x8000U,
                          KAPPS_REQ_STATE_NEW));
    TEST_ASSERT_FALSE(kdrv_apps_req_armed(&off));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_req_set(KAPPS_DATA_OFF + KAPPS_ALIGN, 0x8000U,
                          KAPPS_REQ_STATE_ALWAYS));
    TEST_ASSERT_FALSE(kdrv_apps_req_armed(&off));

    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_write(p, KAPPS_REQ_OFF + offsetof(kapps_req_t, state),
                            &armed, sizeof(armed)));
    TEST_ASSERT_TRUE(kdrv_apps_req_armed(&off));
    TEST_ASSERT_EQUAL_HEX32(KAPPS_DATA_OFF + KAPPS_ALIGN, off);
    TEST_ASSERT_TRUE_MESSAGE(kdrv_apps_req_armed(NULL), "the offset is optional");

    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_write(p, KAPPS_REQ_OFF + offsetof(kapps_req_t, state),
                            &tried, sizeof(tried)));
    TEST_ASSERT_FALSE(kdrv_apps_req_armed(&off));

    kdrv_apps_req_clear();
}

/* Only two words exist. A caller passing TRIED would be writing "already
 * retired" and a caller passing EMPTY would be writing erased flash with a
 * magic in front of it; both are refused before the sector is touched. */
TEST_CASE("a request with a state that is not NEW or ALWAYS is refused",
          "[apps_region]")
{
    uint32_t off = 0U;

    apps_ready();
    kdrv_apps_req_clear();

    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_apps_req_set(KAPPS_DATA_OFF, 0x8000U, KAPPS_REQ_STATE_TRIED));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_apps_req_set(KAPPS_DATA_OFF, 0x8000U, KAPPS_REQ_STATE_EMPTY));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_apps_req_set(KAPPS_DATA_OFF, 0x8000U, 0x12345678U));
    TEST_ASSERT_FALSE(kdrv_apps_req_tried(&off));
}

/* A record whose CRC does not match is not half-believed. It carries an offset
 * the bootloader would jump to, so believing a corrupt one is the worst
 * available outcome. */
TEST_CASE("a request with a broken CRC is ignored, TRIED or not",
          "[apps_region]")
{
    const esp_partition_t * const p = region();
    kapps_req_t                   req;
    uint32_t                      off = 0U;

    apps_ready();

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_req_set(KAPPS_DATA_OFF, 0x8000U, KAPPS_REQ_STATE_NEW));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_read(p, KAPPS_REQ_OFF, &req, sizeof(req)));

    /* Rewrite the whole sector with a TRIED record whose CRC is wrong. Only
     * bits going 1->0 can be written without an erase, so erase first. */
    req.state = KAPPS_REQ_STATE_TRIED;
    req.crc  ^= 0x1U;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_erase_range(p, KAPPS_REQ_OFF, 0x1000U));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_write(p, KAPPS_REQ_OFF, &req, sizeof(req)));

    TEST_ASSERT_FALSE(kdrv_apps_req_tried(&off));

    kdrv_apps_req_clear();
}

/* Erased flash is all ones, and all ones is not a request. This is the state
 * of a board that has never run an app, so getting it wrong would mean every
 * cold boot claimed to be a return from one. */
TEST_CASE("an erased request sector is not a request", "[apps_region]")
{
    uint32_t off = 0U;

    apps_ready();
    kdrv_apps_req_clear();

    TEST_ASSERT_FALSE(kdrv_apps_req_tried(&off));
}

/* The bounds check, which exists because the bootloader used to be the only
 * thing standing between a bad offset and a jump into the directory sector.
 * Both still check; only this side can say why while a console is listening. */
TEST_CASE("an out-of-bounds request is refused and writes nothing",
          "[apps_region]")
{
    const esp_partition_t * const p = region();
    kapps_req_t                   before;
    kapps_req_t                   after;

    apps_ready();
    kdrv_apps_req_clear();
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_read(p, KAPPS_REQ_OFF, &before, sizeof(before)));

    /* Into the directory area. */
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_apps_req_set(0U, 0x8000U, KAPPS_REQ_STATE_NEW));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_apps_req_set(KAPPS_DATA_OFF - KAPPS_ALIGN, 0x8000U,
                          KAPPS_REQ_STATE_NEW));
    /* Not on an MMU page. */
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_apps_req_set(KAPPS_DATA_OFF + 4U, 0x8000U, KAPPS_REQ_STATE_NEW));
    /* Past the end, and past it by overflowing rather than by exceeding. */
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_apps_req_set(KAPPS_DATA_OFF, kdrv_apps_region_size(),
                          KAPPS_REQ_STATE_NEW));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_apps_req_set(0xFFFF0000U, 0x20000U, KAPPS_REQ_STATE_NEW));
    /* Nothing at all. */
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_apps_req_set(KAPPS_DATA_OFF, 0U, KAPPS_REQ_STATE_NEW));

    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_read(p, KAPPS_REQ_OFF, &after, sizeof(after)));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&before, &after, sizeof(after),
        "a refused request must not have erased the sector on its way out");
}

/* ---- The write session ----------------------------------------------------- */

TEST_CASE("a session below the data offset, or off a page, is refused",
          "[apps_region]")
{
    apps_ready();

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_apps_write_begin(0U, 0x1000U));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
                          kdrv_apps_write_begin(KAPPS_REQ_OFF, 0x1000U));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
                          kdrv_apps_write_begin(KAPPS_DATA_OFF + 0x100U, 0x1000U));
}

TEST_CASE("a session running past the end of the region is refused",
          "[apps_region]")
{
    uint32_t const size = kdrv_apps_region_size();

    apps_ready();

    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
                          kdrv_apps_write_begin(size - KAPPS_ALIGN,
                                                KAPPS_ALIGN + 1U));
    /* And the last page exactly filled is fine, which is what makes the one
     * above an off-by-one rather than a rounding preference. */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_apps_write_begin(size - KAPPS_ALIGN, KAPPS_ALIGN));
    kdrv_apps_write_abort();
}

/* Two writers at once would interleave into one image. The second is refused
 * rather than allowed to reset the first one's cursor. */
TEST_CASE("a second session is refused while one is open", "[apps_region]")
{
    apps_ready();

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_apps_write_begin(KAPPS_DATA_OFF, 0x10000U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kdrv_apps_write_begin(KAPPS_DATA_OFF, 0x10000U));

    kdrv_apps_write_abort();

    /* Aborting really closes it: the next writer gets in. */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_apps_write_begin(KAPPS_DATA_OFF, 0x10000U));
    kdrv_apps_write_abort();
}

TEST_CASE("writing with no session open is refused", "[apps_region]")
{
    static const uint8_t byte = 0x5AU;

    apps_ready();
    kdrv_apps_write_abort();   /* whatever came before, there is none now */

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_apps_write(&byte, 1U));
}

/* The declared size is a promise, not a hint: an image that grew past what
 * the allocator reserved would run into whatever the directory placed next. */
TEST_CASE("a write past what the session declared is refused", "[apps_region]")
{
    static uint8_t buf[64];

    (void)memset(buf, 0xA5, sizeof(buf));
    apps_ready();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write_begin(KAPPS_DATA_OFF, 32U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write(buf, 32U));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_apps_write(buf, 1U));

    kdrv_apps_write_abort();
}

/* The real thing: bytes go to flash and come back. The chunk size is
 * deliberately not a divisor of the erase block, so the session's own cursor
 * arithmetic - not a tidy multiple - is what has to be right. */
TEST_CASE("what a session writes is what the region holds", "[apps_region]")
{
    static uint8_t out[0x300];
    static uint8_t in[0x300];
    uint32_t const at = KAPPS_DATA_OFF;

    apps_ready();

    for ( unsigned i = 0U; sizeof(out) > i; ++i )
    {
        out[i] = (uint8_t)(i * 7U);
    }

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write_begin(at, sizeof(out)));
    for ( unsigned i = 0U; sizeof(out) > i; i += 0x37U )
    {
        size_t const n = ((sizeof(out) - i) < 0x37U) ? (sizeof(out) - i) : 0x37U;
        TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write(&out[i], n));
    }
    kdrv_apps_write_abort();   /* end() would verify an image, and this is not one */

    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_read(region(), at, in, sizeof(in)));
    TEST_ASSERT_EQUAL_MEMORY(out, in, sizeof(out));
}

/* Erasing happens block by block as the write reaches into each one, which is
 * what leaves the GUI room to breathe during an install. The property that
 * matters is that it happens *before* the bytes and *only* ahead of them: a
 * block erased late would take the data back out again. */
TEST_CASE("erasing follows the write across a block boundary, never behind it",
          "[apps_region]")
{
    static uint8_t out[0x40];
    static uint8_t in[0x40];
    uint32_t const at = KAPPS_DATA_OFF;

    apps_ready();
    (void)memset(out, 0x3CU, sizeof(out));

    /* A session spanning two erase blocks, written at the very start of the
     * first and again at the very start of the second. */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_write_begin(at, KAPPS_ALIGN + sizeof(out)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write(out, sizeof(out)));

    /* Fill the rest of the first block so the cursor lands exactly on the
     * boundary - the session's declared size is a hard limit, so overshooting
     * here is refused rather than tolerated. */
    static uint8_t gap[0x1000];
    (void)memset(gap, 0x00U, sizeof(gap));
    uint32_t left = KAPPS_ALIGN - (uint32_t)sizeof(out);
    while ( 0U < left )
    {
        size_t const n = (sizeof(gap) > left) ? (size_t)left : sizeof(gap);
        TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write(gap, n));
        left -= (uint32_t)n;
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write(out, sizeof(out)));
    kdrv_apps_write_abort();

    /* The first block's bytes survived the second block's erase. */
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_read(region(), at, in, sizeof(in)));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(out, in, sizeof(out),
        "erasing ahead of the cursor must never reach back behind it");

    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_read(region(), at + KAPPS_ALIGN, in, sizeof(in)));
    TEST_ASSERT_EQUAL_MEMORY(out, in, sizeof(out));
}

/* What an install may take back after it fails is decided by which blocks the
 * write got to erase, so the driver says it: an extent is reached when any
 * block of the session's run has been erased over it, and never before the
 * data gets there. */
TEST_CASE("an extent is reached once the write erases a block of it, and not before",
          "[apps_region]")
{
    static uint8_t out[0x40];
    uint32_t const at = KAPPS_DATA_OFF + (2U * KAPPS_ALIGN);

    apps_ready();
    kapps_dir_t same = *kdrv_apps_dir();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&same));   /* starts the record over */
    (void)memset(out, 0x5AU, sizeof(out));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write_begin(at, 3U * KAPPS_ALIGN));
    TEST_ASSERT_FALSE_MESSAGE(kdrv_apps_write_reached(at, KAPPS_ALIGN),
        "an extent under a session that has written nothing is reached");

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write(out, sizeof(out)));
    TEST_ASSERT_TRUE_MESSAGE(kdrv_apps_write_reached(at, KAPPS_ALIGN),
        "the block the first bytes landed in is not reached");
    TEST_ASSERT_TRUE_MESSAGE(kdrv_apps_write_reached(at + 0x10U, 1U),
        "a one-byte extent inside the erased block is not reached");
    TEST_ASSERT_TRUE_MESSAGE(kdrv_apps_write_reached(at - KAPPS_ALIGN, 2U * KAPPS_ALIGN),
        "an extent that runs into the erased block is not reached");
    TEST_ASSERT_FALSE_MESSAGE(kdrv_apps_write_reached(at - KAPPS_ALIGN, KAPPS_ALIGN),
        "the extent just below the session is reached");
    TEST_ASSERT_FALSE_MESSAGE(kdrv_apps_write_reached(at + KAPPS_ALIGN, KAPPS_ALIGN),
        "a block the write has not got to is reached");
    TEST_ASSERT_FALSE_MESSAGE(kdrv_apps_write_reached(at + (2U * KAPPS_ALIGN), 4U),
        "the last block of the session is reached before any byte lands in it");
    TEST_ASSERT_FALSE_MESSAGE(kdrv_apps_write_reached(at, 0U),
        "an extent of no bytes is reached");
    kdrv_apps_write_abort();

    TEST_ASSERT_TRUE_MESSAGE(kdrv_apps_write_reached(at, KAPPS_ALIGN),
        "an abandoned session forgot what it had erased");
}

TEST_CASE("the erases of the last session are not the next directory's",
          "[apps_region]")
{
    static uint8_t out[0x40];
    uint32_t const at = KAPPS_DATA_OFF;

    apps_ready();
    (void)memset(out, 0x5AU, sizeof(out));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write_begin(at, KAPPS_ALIGN));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write(out, sizeof(out)));
    kdrv_apps_write_abort();
    TEST_ASSERT_TRUE(kdrv_apps_write_reached(at, 1U));

    kapps_dir_t same = *kdrv_apps_dir();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&same));
    TEST_ASSERT_FALSE_MESSAGE(kdrv_apps_write_reached(at, 1U),
        "a committed directory kept the erases made before it");

    /* A commit that fails changes nothing: the record stands. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write_begin(at, KAPPS_ALIGN));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write(out, sizeof(out)));
    kdrv_apps_write_abort();
    esp_partition_fail_after(0U, ESP_PARTITION_FAIL_AFTER_MODE_BOTH);
    TEST_ASSERT_NOT_EQUAL(KODE_OK, kdrv_apps_dir_commit(&same));
    cut_off();
    TEST_ASSERT_TRUE_MESSAGE(kdrv_apps_write_reached(at, 1U),
        "a commit that failed wiped the record");

    /* A new session starts its own record, and says nothing of the old one. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write_begin(at + KAPPS_ALIGN, KAPPS_ALIGN));
    TEST_ASSERT_FALSE_MESSAGE(kdrv_apps_write_reached(at, 1U),
        "a session that has erased nothing inherited the last one's");
    kdrv_apps_write_abort();
}

TEST_CASE("an erase that fails counts as reaching its block",
          "[apps_region]")
{
    static uint8_t out[0x40];
    uint32_t const at = KAPPS_DATA_OFF + KAPPS_ALIGN;

    apps_ready();
    kapps_dir_t same = *kdrv_apps_dir();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_dir_commit(&same));
    (void)memset(out, 0x5AU, sizeof(out));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write_begin(at, 2U * KAPPS_ALIGN));
    esp_partition_fail_after(0U, ESP_PARTITION_FAIL_AFTER_MODE_BOTH);
    TEST_ASSERT_NOT_EQUAL(KODE_OK, kdrv_apps_write(out, sizeof(out)));
    cut_off();
    kdrv_apps_write_abort();

    TEST_ASSERT_TRUE_MESSAGE(kdrv_apps_write_reached(at, KAPPS_ALIGN),
        "a block a failed erase touched is called untouched");
    TEST_ASSERT_FALSE(kdrv_apps_write_reached(at + KAPPS_ALIGN, KAPPS_ALIGN));
}

/** @brief A lead-in that leaves the next write starting mid-session. */
#define LEAD_LEN         0x37U
/** @brief One write two bounces and a part long, crossing every boundary. */
#define LONG_WRITE_LEN   ((KDRV_BOUNCE_BYTES * 2U) + (KDRV_BOUNCE_BYTES / 2U) + 1U)

/** @brief Fill @p buf with a pattern no shifted copy of itself matches. */
static void fill_pattern(uint8_t *buf, size_t len)
{
    for ( size_t i = 0U; i < len; i++ )
    {
        buf[i] = (uint8_t)((i * 7U) + (i >> 8U) + 3U);
    }
}

/* ---- The CRC of an extent -------------------------------------------------- */

/** @brief The extent the CRC cases fold: odd on purpose, so a read cannot end on a chunk. */
#define EXTENT_LEN   ((3U * 4096U) + 7U)

/** @brief What lies right after the extent: a pattern no extent byte matches. */
#define GUARD_LEN    64U

/* A digest is what can be trusted over data that is not known to be an image:
 * it reads the bytes it is given and nothing else. The guard after the
 * extent is the proof: a read that ran on would fold it in. */
TEST_CASE("an extent's CRC folds exactly its bytes and never the ones after it",
          "[apps_region]")
{
    static uint8_t data[EXTENT_LEN + GUARD_LEN];
    uint32_t       crc = KHAL_CRC32_INIT;

    apps_ready();
    fill_pattern(data, EXTENT_LEN);
    (void)memset(&data[EXTENT_LEN], 0xEEU, GUARD_LEN);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write_begin(KAPPS_DATA_OFF, sizeof(data)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write(data, sizeof(data)));
    kdrv_apps_write_abort();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_extent_crc(KAPPS_DATA_OFF, EXTENT_LEN, &crc));
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(khal_crc32(KHAL_CRC32_INIT, data, EXTENT_LEN), crc,
        "the CRC is not that of the extent's bytes");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(khal_crc32(KHAL_CRC32_INIT, data, sizeof(data)), crc,
        "the guard after the extent was folded in");

    /* In parts it is the same CRC, and a seed continues one. */
    uint32_t part = KHAL_CRC32_INIT;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_extent_crc(KAPPS_DATA_OFF, 5000U, &part));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_extent_crc(KAPPS_DATA_OFF + 5000U,
                                                        EXTENT_LEN - 5000U, &part));
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(crc, part, "an extent taken in two parts differs");
}

TEST_CASE("an extent that leaves the data area is refused and leaves the CRC alone",
          "[apps_region]")
{
    uint32_t crc = 0xDEADBEEFU;

    apps_ready();
    uint32_t const end = kdrv_apps_region_size();

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_ERROR,
        kdrv_apps_extent_crc(KAPPS_DATA_OFF - 1U, 16U, &crc), "below the data area");
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_ERROR,
        kdrv_apps_extent_crc(0U, 16U, &crc), "the directory is not an extent");
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_ERROR,
        kdrv_apps_extent_crc(end - 15U, 16U, &crc), "one byte past the region");
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_ERROR,
        kdrv_apps_extent_crc(KAPPS_DATA_OFF, end, &crc), "longer than the region");
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_ERROR,
        kdrv_apps_extent_crc(UINT32_MAX - 7U, 16U, &crc), "an extent that wraps");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0xDEADBEEFU, crc, "a refusal changed the CRC");

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_apps_extent_crc(KAPPS_DATA_OFF, 16U, NULL));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kdrv_apps_extent_crc(KAPPS_DATA_OFF, 0U, &crc),
        "an extent of no bytes is refused");
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFU, crc);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kdrv_apps_extent_crc(end - 16U, 16U, &crc),
        "the last 16 bytes of the region are an extent");
}

/* ---- An image that claims more than it holds ------------------------------- */

/** @brief Bytes the image of these cases is given in the region. */
#define CLAIM_IMG_LEN  4096U

/**
 * @brief Write the head of an image at the first data block and close the session unverified.
 * @param segments What the image header claims.
 * @param data_len What its first segment header claims.
 */
static void claim_image(uint8_t segments, uint32_t data_len)
{
    static uint8_t             img[CLAIM_IMG_LEN];
    esp_image_header_t         h;
    esp_image_segment_header_t seg;

    (void)memset(img, 0, sizeof(img));
    (void)memset(&h, 0, sizeof(h));
    h.magic         = ESP_IMAGE_HEADER_MAGIC;
    h.segment_count = segments;
    h.hash_appended = 0U;
    (void)memcpy(&img[0], &h, sizeof(h));
    (void)memset(&seg, 0, sizeof(seg));
    seg.load_addr = 0x40000000U;
    seg.data_len  = data_len;
    (void)memcpy(&img[sizeof(h)], &seg, sizeof(seg));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write_begin(KAPPS_DATA_OFF, sizeof(img)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write(img, sizeof(img)));
    kdrv_apps_write_abort();
}

/* The verifier hashes every segment by the length its header declares and
 * compares the total with the partition only afterwards, so a header that
 * declares more than the image holds keeps it reading flash for as long as the
 * lengths say. The segment chain is walked first, by its own bytes alone, and
 * an image whose chain leaves them never reaches the verifier: the fake one
 * here says yes to everything, so a refusal is the walk's. */
TEST_CASE("an image whose segments leave its own bytes is refused unhashed",
          "[apps_region]")
{
    apps_ready();

    claim_image(1U, 0x7FFFFFF0U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_ERROR,
        kdrv_apps_verify(KAPPS_DATA_OFF, CLAIM_IMG_LEN, NULL), "a segment of 2 GB");

    claim_image(1U, 0xFFFFFFFCU);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_ERROR,
        kdrv_apps_verify(KAPPS_DATA_OFF, CLAIM_IMG_LEN, NULL), "a length that wraps");

    claim_image(1U, CLAIM_IMG_LEN);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_ERROR,
        kdrv_apps_verify(KAPPS_DATA_OFF, CLAIM_IMG_LEN, NULL),
        "a segment as long as the whole image, after its headers");

    claim_image(0U, 16U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_ERROR,
        kdrv_apps_verify(KAPPS_DATA_OFF, CLAIM_IMG_LEN, NULL), "no segments");

    claim_image((uint8_t)(ESP_IMAGE_MAX_SEGMENTS + 1U), 16U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_ERROR,
        kdrv_apps_verify(KAPPS_DATA_OFF, CLAIM_IMG_LEN, NULL), "more segments than an image has");
}

/* The walk is a length check and nothing more: an image whose chain holds is
 * still the verifier's to judge. */
TEST_CASE("an image whose segments fit is the verifier's to judge", "[apps_region]")
{
    uint32_t len = 0U;

    apps_ready();
    claim_image(1U, 256U);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_verify(KAPPS_DATA_OFF, CLAIM_IMG_LEN, &len));

    fake_image_verify_set_result(ESP_FAIL);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_apps_verify(KAPPS_DATA_OFF, CLAIM_IMG_LEN, NULL));
    fake_image_verify_set_result(ESP_OK);
}

/* An install ends with the same check: the session's own close refuses what
 * the walk refuses. */
TEST_CASE("a write session does not close on an image that claims more than it holds",
          "[apps_region]")
{
    static uint8_t             img[CLAIM_IMG_LEN];
    esp_image_header_t         h;
    esp_image_segment_header_t seg;

    apps_ready();
    (void)memset(img, 0, sizeof(img));
    (void)memset(&h, 0, sizeof(h));
    h.magic         = ESP_IMAGE_HEADER_MAGIC;
    h.segment_count = 1U;
    (void)memcpy(&img[0], &h, sizeof(h));
    (void)memset(&seg, 0, sizeof(seg));
    seg.load_addr = 0x40000000U;
    seg.data_len  = 0x00800000U;
    (void)memcpy(&img[sizeof(h)], &seg, sizeof(seg));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write_begin(KAPPS_DATA_OFF, sizeof(img)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write(img, sizeof(img)));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_apps_write_end());
}

/* A write longer than a bounce reaches the flash in pieces, and each piece
 * must land where it belongs: after whatever the session already wrote, and
 * after the pieces before it in the same write. */
TEST_CASE("a long write lands in the region where the session has reached",
          "[apps_region]")
{
    static uint8_t out[LEAD_LEN + LONG_WRITE_LEN];
    static uint8_t in[LEAD_LEN + LONG_WRITE_LEN];
    uint32_t const at = KAPPS_DATA_OFF;

    apps_ready();
    fill_pattern(out, sizeof(out));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write_begin(at, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write(out, LEAD_LEN));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write(&out[LEAD_LEN], LONG_WRITE_LEN));
    kdrv_apps_write_abort();

    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_read(region(), at, in, sizeof(in)));
    TEST_ASSERT_EQUAL_MEMORY(out, in, sizeof(out));
}

/* The same for the mailbox, which has its own cursor. */
TEST_CASE("a long write lands in the mailbox where the session has reached",
          "[apps_region]")
{
    static uint8_t out[LEAD_LEN + LONG_WRITE_LEN];
    static uint8_t in[LEAD_LEN + LONG_WRITE_LEN];
    const esp_partition_t * const box = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)KAPPS_PENDING_SUBTYPE,
        "app_pending");

    TEST_ASSERT_NOT_NULL(box);
    apps_ready();
    kdrv_apps_pending_write_abort();
    fill_pattern(out, sizeof(out));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_write_begin(sizeof(out)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_write(out, LEAD_LEN));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_pending_write(&out[LEAD_LEN], LONG_WRITE_LEN));
    kdrv_apps_pending_write_abort();

    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_partition_read(box, 0U, in, sizeof(in)));
    TEST_ASSERT_EQUAL_MEMORY(out, in, sizeof(out));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_consume());
}

/* ---- Taking an image out of circulation ------------------------------------ */

/* Eviction erases the header sector rather than the whole image: an image
 * whose magic is gone is one the bootloader will not jump to, and erasing 
 * megabytes to say so would stall everything for seconds. */
TEST_CASE("erasing a header leaves erased flash where the magic was",
          "[apps_region]")
{
    static const uint8_t magic[4] = { 0xE9U, 0x04U, 0x02U, 0x2FU };
    uint8_t              in[4];
    uint32_t const       at = KAPPS_DATA_OFF;

    apps_ready();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write_begin(at, sizeof(magic)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write(magic, sizeof(magic)));
    kdrv_apps_write_abort();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_erase_header(at));

    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_partition_read(region(), at, in, sizeof(in)));
    for ( unsigned i = 0U; sizeof(in) > i; ++i )
    {
        TEST_ASSERT_EQUAL_HEX8(0xFFU, in[i]);
    }
}

/* ---- Naming what is installed ---------------------------------------------
 *
 * A directory entry says where an image is and how long it is, and nothing at
 * all about what it is called. The name comes out of the image's own IDF
 * descriptor, which is why every listing costs a flash read per entry - and
 * why an entry pointing at erased flash has no name and cannot be found.
 */

/**
 * @brief Write an image prefix carrying an IDF descriptor at @p offset.
 * @param offset Region-relative, a multiple of KAPPS_ALIGN.
 * @param project The project name to plant.
 * @param magic_ok Write the descriptor's magic word, or leave it wrong.
 */
static void plant_desc(uint32_t offset, const char *project, bool magic_ok)
{
    static uint8_t prefix[KAPP_PREFIX_LEN];
    esp_app_desc_t desc;

    (void)memset(prefix, 0xFF, sizeof(prefix));
    (void)memset(&desc, 0, sizeof(desc));
    desc.magic_word = magic_ok ? ESP_APP_DESC_MAGIC_WORD : 0xDEADBEEFU;
    (void)snprintf(desc.project_name, sizeof(desc.project_name), "%s", project);
    (void)snprintf(desc.version, sizeof(desc.version), "%s", "1.2.3");
    (void)snprintf(desc.date, sizeof(desc.date), "%s", "Jan  1 2026");
    (void)snprintf(desc.time, sizeof(desc.time), "%s", "12:00:00");
    (void)snprintf(desc.idf_ver, sizeof(desc.idf_ver), "%s", "v9.9.9");
    (void)memcpy(&prefix[KAPP_IDF_DESC_OFF], &desc, sizeof(desc));

    kdrv_apps_write_abort();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_apps_write_begin(offset, sizeof(prefix)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write(prefix, sizeof(prefix)));
    kdrv_apps_write_abort();
}

/* The writer refuses what a reader would: a directory sealed with a good CRC
 * and a count past the array, or an extent outside the data area, would be
 * read back whole and walked. The directory in force stays as it was. */
TEST_CASE("a directory that does not fit the region is not committed",
          "[apps_region]")
{
    apps_ready();
    kapps_dir_t const before = *kdrv_apps_dir();

    kapps_dir_t bad = before;
    bad.count = KAPPS_MAX_ENTRIES + 1U;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_apps_dir_commit(&bad));

    bad             = before;
    bad.count       = 1U;
    bad.e[0].offset = 0U;
    bad.e[0].size   = 0x1000U;
    bad.e[0].flags  = KAPPS_ENTRY_VALID;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_apps_dir_commit(&bad));

    TEST_ASSERT_EQUAL_UINT32(before.gen, kdrv_apps_dir()->gen);
    TEST_ASSERT_EQUAL_UINT32(before.count, kdrv_apps_dir()->count);
}

TEST_CASE("an installed image is named by its own descriptor", "[apps_region]")
{
    kapps_entry_t    e = { .offset = KAPPS_DATA_OFF, .size = 0x1000U,
                           .seq = 1U, .flags = KAPPS_ENTRY_VALID };
    kdrv_slot_desc_t out;

    apps_ready();
    plant_desc(e.offset, "metronome", true);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_desc(&e, &out, NULL));
    TEST_ASSERT_EQUAL_STRING("metronome", out.project);
    TEST_ASSERT_EQUAL_STRING("1.2.3", out.version);
    TEST_ASSERT_EQUAL_STRING("v9.9.9", out.idf_ver);
}

/* esp_app_desc_t's strings are fixed-width arrays a NUL need not end. Each
 * field is read with its own width: a full one is cut to fit ours, and never
 * runs on into the field after it. */
TEST_CASE("a descriptor field with no NUL is read with its width", "[apps_region]")
{
    static uint8_t   prefix[KAPP_PREFIX_LEN];
    esp_app_desc_t   desc;
    kapps_entry_t    e = { .offset = KAPPS_DATA_OFF, .size = 0x1000U,
                           .seq = 1U, .flags = KAPPS_ENTRY_VALID };
    kdrv_slot_desc_t out;

    apps_ready();
    (void)memset(prefix, 0xFF, sizeof(prefix));
    (void)memset(&desc, 0, sizeof(desc));
    desc.magic_word = ESP_APP_DESC_MAGIC_WORD;
    (void)memset(desc.project_name, 'p', sizeof(desc.project_name));
    (void)memset(desc.version, 'v', sizeof(desc.version));
    (void)memcpy(&prefix[KAPP_IDF_DESC_OFF], &desc, sizeof(desc));
    kdrv_apps_write_abort();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write_begin(e.offset, sizeof(prefix)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_write(prefix, sizeof(prefix)));
    kdrv_apps_write_abort();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_desc(&e, &out, NULL));
    TEST_ASSERT_EQUAL_size_t(sizeof(out.project) - 1U, strlen(out.project));
    TEST_ASSERT_NULL_MESSAGE(strchr(out.project, 'v'),
                             "the project name ran on into the version");
    TEST_ASSERT_EQUAL_size_t(sizeof(out.version) - 1U, strlen(out.version));
    TEST_ASSERT_EQUAL_STRING("", out.date);
}

/* Erased flash is not an image, and neither is one whose descriptor magic is
 * wrong. Both must be errors rather than a struct full of whatever was there:
 * the strings go straight into a listing an operator reads. */
TEST_CASE("an entry pointing at nothing has no name", "[apps_region]")
{
    kapps_entry_t    e = { .offset = KAPPS_DATA_OFF, .size = 0x1000U,
                           .seq = 1U, .flags = KAPPS_ENTRY_VALID };
    kdrv_slot_desc_t out;

    apps_ready();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_erase_header(e.offset));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_apps_desc(&e, &out, NULL));

    plant_desc(e.offset, "wrong-magic", false);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_apps_desc(&e, &out, NULL));
}

/* Unmanifested is the common case, not a failure: a plain .bin nobody wrote a
 * manifest for still has a project name and still has to be filed somewhere.
 * The manifest comes back zeroed rather than the call coming back an error. */
TEST_CASE("an image with no manifest is described anyway", "[apps_region]")
{
    kapps_entry_t    e = { .offset = KAPPS_DATA_OFF, .size = 0x1000U,
                           .seq = 1U, .flags = KAPPS_ENTRY_VALID };
    kdrv_slot_desc_t out;
    kapp_desc_t      manifest;

    apps_ready();
    plant_desc(e.offset, "plainbin", true);

    (void)memset(&manifest, 0xAB, sizeof(manifest));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_desc(&e, &out, &manifest));
    TEST_ASSERT_EQUAL_STRING("plainbin", out.project);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", manifest.name,
        "an absent manifest must come back empty, not full of old bytes");
}

TEST_CASE("describing nothing is refused rather than crashed", "[apps_region]")
{
    kapps_entry_t    e = { .offset = KAPPS_DATA_OFF, .size = 0x1000U,
                           .seq = 1U, .flags = KAPPS_ENTRY_VALID };
    kdrv_slot_desc_t out;

    apps_ready();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_apps_desc(NULL, &out, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_apps_desc(&e, NULL, NULL));
}

/* ---- The development mailbox ----------------------------------------------
 *
 * app_pending sits at 0x1A00000 - fixed, and marked non-executable so the
 * bootloader will never jump to it. The driver judges what is parked there by
 * walking the image's own header chain with plain reads: a length check that
 * bounds the copy and catches a torn upload, never a hash. The real
 * verification runs after the copy, where the image will execute.
 */

/** @brief The mailbox partition, found the way the driver finds it. */
static const esp_partition_t *mailbox(void)
{
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)KAPPS_PENDING_SUBTYPE,
        "app_pending");
    TEST_ASSERT_NOT_NULL(p);
    return p;
}

/**
 * @brief Park a synthetic ESP image in the mailbox.
 * @param segments What the header should claim.
 * @param data_len What the one segment header should claim.
 * @return The partition, for the caller's own reads.
 */
static const esp_partition_t *park_image(uint8_t segments, uint32_t data_len)
{
    const esp_partition_t * const p = mailbox();
    esp_image_header_t            h;
    esp_image_segment_header_t    seg;

    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_partition_erase_range(p, 0, 0x1000U));

    (void)memset(&h, 0, sizeof(h));
    h.magic         = ESP_IMAGE_HEADER_MAGIC;
    h.segment_count = segments;
    h.hash_appended = 0U;
    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_partition_write(p, 0, &h, sizeof(h)));

    (void)memset(&seg, 0, sizeof(seg));
    seg.load_addr = 0x40000000U;
    seg.data_len  = data_len;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_write(p, sizeof(h), &seg, sizeof(seg)));

    return p;
}

TEST_CASE("an erased mailbox holds nothing", "[apps_region]")
{
    uint32_t len = 0xFFFFFFFFU;

    apps_ready();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_consume());

    TEST_ASSERT_FALSE(kdrv_apps_pending_present(&len, NULL));
}

/* A whole image, walked by hand: header, one segment, the checksum pad rounded
 * to 16, and no appended hash. The length is what AppMgr copies, so it has to
 * be exact rather than merely plausible. */
TEST_CASE("a parked image is found and its length walked exactly",
          "[apps_region]")
{
    uint32_t len = 0U;
    uint32_t const data = 0x40U;

    apps_ready();
    (void)park_image(1U, data);

    TEST_ASSERT_TRUE(kdrv_apps_pending_present(&len, NULL));

    uint32_t expect = (uint32_t)sizeof(esp_image_header_t)
                    + (uint32_t)sizeof(esp_image_segment_header_t) + data;
    expect = (expect | 15U) + 1U;
    TEST_ASSERT_EQUAL_UINT32(expect, len);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_consume());
}

/* The overflow the walk is written to survive. A data_len near UINT32_MAX does
 * not run off the end of the partition - it runs *around* it, wrapping the
 * cursor back to a small number, and a bounds check made after the addition
 * waves that straight through. Bounding the addition instead is the fix, and
 * this is what proves it holds. */
TEST_CASE("a segment length that would wrap the cursor is refused",
          "[apps_region]")
{
    uint32_t len = 0xFFFFFFFFU;

    apps_ready();
    (void)park_image(1U, 0xFFFFFFF0U);

    TEST_ASSERT_FALSE_MESSAGE(kdrv_apps_pending_present(&len, NULL),
        "an image claiming a segment near UINT32_MAX must not be accepted");
}

/** @brief The payload the containers parked below claim to carry. */
#define PARK_PAYLOAD_LEN  304U

/** @brief The app.json length the parked blocks claim. */
#define PARK_JSON_LEN     12U

/**
 * @brief Park the headers of a container for an app: its own 72 bytes and,
 *        with @p with_block, the block's 12 right after.
 *
 * Only the headers are written. The mailbox's length is a statement of those
 * two headers, which is all kdrv_apps_pending_present() reads.
 *
 * @param with_block Set KFW_FLAG_APPDATA and write the block's header.
 * @param magic      The magic the block's header carries.
 * @param icon_len   The icon length the block's header claims.
 */
static void park_container_headers(bool with_block, uint32_t magic,
                                   uint16_t icon_len)
{
    const esp_partition_t * const p = mailbox();
    kfw_hdr_t    h;
    kfw_ad_hdr_t ad;

    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_partition_erase_range(p, 0, 0x1000U));

    (void)memset(&h, 0, sizeof(h));
    h.magic       = KFW_MAGIC;
    h.hdr_version = (uint16_t)KFW_HDR_VERSION;
    h.hdr_len     = (uint16_t)KFW_HDR_LEN;
    h.target      = (uint8_t)KFW_TARGET_APP;
    h.comp        = (uint8_t)KFW_COMP_NONE;
    if ( with_block )
    {
        h.flags = KFW_FLAG_APPDATA;
    }
    h.payload_len = PARK_PAYLOAD_LEN;
    h.image_len   = PARK_PAYLOAD_LEN;
    h.crc = kfw_crc32(&h, offsetof(kfw_hdr_t, crc));
    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_partition_write(p, 0, &h, sizeof(h)));

    if ( with_block )
    {
        (void)memset(&ad, 0, sizeof(ad));
        ad.magic    = magic;
        ad.json_len = PARK_JSON_LEN;
        ad.icon_len = icon_len;
        TEST_ASSERT_EQUAL_INT(ESP_OK,
            esp_partition_write(p, KFW_HDR_LEN, &ad, sizeof(ad)));
    }
}

/* The header's payload_len leaves the block out - the block states its own
 * length - so a parked container with one is longer than its header says. A
 * length that missed it fed the decoder short, and the adoption failed. */
TEST_CASE("a parked container is sized with its app-data block",
          "[apps_region]")
{
    uint32_t len = 0U;

    apps_ready();
    park_container_headers(false, KFW_AD_MAGIC, 0U);
    TEST_ASSERT_TRUE(kdrv_apps_pending_present(&len, NULL));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(KFW_HDR_LEN + PARK_PAYLOAD_LEN, len,
        "a container with no block is its header and its payload");

    park_container_headers(true, KFW_AD_MAGIC, 0U);
    TEST_ASSERT_TRUE(kdrv_apps_pending_present(&len, NULL));
    TEST_ASSERT_EQUAL_UINT32(KFW_HDR_LEN + KFW_AD_HDR_LEN + PARK_JSON_LEN
                             + PARK_PAYLOAD_LEN, len);

    park_container_headers(true, KFW_AD_MAGIC, (uint16_t)KFW_AD_ICON_LEN);
    TEST_ASSERT_TRUE(kdrv_apps_pending_present(&len, NULL));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(KFW_HDR_LEN + KFW_AD_HDR_LEN
                                     + PARK_JSON_LEN + KFW_AD_ICON_LEN
                                     + PARK_PAYLOAD_LEN, len,
        "the icon is part of the block");

    /* Not a block at all: counted as none, and the decoder refuses it when
     * the mailbox is adopted. */
    park_container_headers(true, 0U, 0U);
    TEST_ASSERT_TRUE(kdrv_apps_pending_present(&len, NULL));
    TEST_ASSERT_EQUAL_UINT32(KFW_HDR_LEN + PARK_PAYLOAD_LEN, len);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_consume());
}

/* A segment that merely exceeds the partition, without wrapping. */
TEST_CASE("a segment running past the end of the mailbox is refused",
          "[apps_region]")
{
    uint32_t len = 0xFFFFFFFFU;

    apps_ready();
    (void)park_image(1U, (uint32_t)mailbox()->size);

    TEST_ASSERT_FALSE(kdrv_apps_pending_present(&len, NULL));
}

/* The walk and write_begin() used to disagree by one sector: write_begin()
 * stopped an upload at the intent sector, the walk only at the end of the
 * partition, so an esptool push could land image bytes in the sector that
 * consume() erases. An image whose walk ends inside the intent sector fits
 * the partition and is still refused. */
TEST_CASE("an image that runs into the intent sector is refused, though it fits the partition",
          "[apps_region]")
{
    uint32_t len = 0xFFFFFFFFU;
    uint32_t const hdrs = (uint32_t)sizeof(esp_image_header_t)
                        + (uint32_t)sizeof(esp_image_segment_header_t);
    /* Ends 0x800 into the last sector, padded to 16: past the intent
     * offset, well short of the partition's end. */
    uint32_t const data = (uint32_t)KAPPS_PENDING_INTENT_OFF + 0x800U - hdrs;

    apps_ready();
    (void)park_image(1U, data);

    TEST_ASSERT_FALSE_MESSAGE(kdrv_apps_pending_present(&len, NULL),
        "an image reaching into the intent sector must not be adopted");
}

/* ...and one that stops exactly where the intent sector starts is the
 * largest the mailbox holds, by either path. */
TEST_CASE("an image that ends exactly at the intent sector is the largest the mailbox takes",
          "[apps_region]")
{
    uint32_t len = 0U;
    uint32_t const hdrs = (uint32_t)sizeof(esp_image_header_t)
                        + (uint32_t)sizeof(esp_image_segment_header_t);
    /* The checksum pad rounds the end up to 16, and the intent offset is a
     * multiple of 16, so ending one byte short of it pads to exactly it. */
    uint32_t const data = (uint32_t)KAPPS_PENDING_INTENT_OFF - hdrs - 1U;

    apps_ready();
    (void)park_image(1U, data);

    TEST_ASSERT_TRUE(kdrv_apps_pending_present(&len, NULL));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KAPPS_PENDING_INTENT_OFF, len);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_consume());
}

/* A container states its own length in its header, and that claim meets the
 * same ceiling as a bare image's walk: a payload that would run into the
 * intent sector is refused, and the mailbox cleared so it is not re-read on
 * every boot. The largest payload that fits is still taken. */
TEST_CASE("a container claiming more than the mailbox holds is refused",
          "[apps_region]")
{
    const esp_partition_t * const p = mailbox();
    uint32_t const room = (uint32_t)KAPPS_PENDING_INTENT_OFF - (uint32_t)KFW_HDR_LEN;
    uint32_t len = 0U;
    uint8_t  first = 0U;
    kfw_hdr_t h;

    apps_ready();
    (void)memset(&h, 0, sizeof(h));
    h.magic       = KFW_MAGIC;
    h.hdr_version = (uint16_t)KFW_HDR_VERSION;
    h.hdr_len     = (uint16_t)KFW_HDR_LEN;
    h.target      = (uint8_t)KFW_TARGET_APP;
    h.comp        = (uint8_t)KFW_COMP_NONE;
    (void)snprintf(h.name, sizeof(h.name), "%s", "demo");

    /* Exactly the room there is: taken, and the length is exact. */
    h.payload_len = room;
    h.image_len   = room;
    h.crc = kfw_crc32(&h, offsetof(kfw_hdr_t, crc));
    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_partition_erase_range(p, 0, 0x1000U));
    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_partition_write(p, 0, &h, sizeof(h)));
    TEST_ASSERT_TRUE(kdrv_apps_pending_present(&len, NULL));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KAPPS_PENDING_INTENT_OFF, len);

    /* One byte more reaches the intent sector: refused, and cleared. */
    h.payload_len = room + 1U;
    h.image_len   = room + 1U;
    h.crc = kfw_crc32(&h, offsetof(kfw_hdr_t, crc));
    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_partition_erase_range(p, 0, 0x1000U));
    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_partition_write(p, 0, &h, sizeof(h)));
    TEST_ASSERT_FALSE(kdrv_apps_pending_present(&len, NULL));
    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_partition_read(p, 0, &first, sizeof(first)));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFFU, first,
        "the mailbox should have been cleared, not re-read every boot");

    /* A length that would wrap 32 bits once the header is added. */
    h.payload_len = 0xFFFFFFFFU;
    h.image_len   = 0xFFFFFFFFU;
    h.crc = kfw_crc32(&h, offsetof(kfw_hdr_t, crc));
    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_partition_erase_range(p, 0, 0x1000U));
    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_partition_write(p, 0, &h, sizeof(h)));
    TEST_ASSERT_FALSE(kdrv_apps_pending_present(&len, NULL));
}

/* The documented constant is what the code derives from the table: the
 * mailbox's last 4 KB. A partitions.csv that grows app_pending fails here,
 * which is the moment to move the constant and the host tool with it. */
TEST_CASE("the intent sector is the mailbox's last 4 KB", "[apps_region]")
{
    TEST_ASSERT_EQUAL_HEX32((uint32_t)mailbox()->size - 0x1000U,
                            (uint32_t)KAPPS_PENDING_INTENT_OFF);
}

/* Something that starts like an image and is not one is thrown away on the
 * spot, so a broken upload is not re-walked and re-logged on every boot for
 * the rest of the board's life. */
TEST_CASE("a broken image is discarded rather than reported every boot",
          "[apps_region]")
{
    const esp_partition_t * const p = mailbox();
    uint32_t                      len = 0U;
    uint8_t                       first = 0U;

    apps_ready();
    (void)park_image(0U, 0U);   /* magic right, segment count impossible */

    TEST_ASSERT_FALSE(kdrv_apps_pending_present(&len, NULL));

    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_partition_read(p, 0, &first, sizeof(first)));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFFU, first,
        "the mailbox should have been erased, not left to be re-read forever");
}

/* ---- Writing to the mailbox ------------------------------------------------ */

/* The intent sector is the mailbox's last 4KB and is not image space. A write
 * allowed to reach it would overwrite the flag describing itself. */
TEST_CASE("a mailbox write may not reach into the intent sector",
          "[apps_region]")
{
    apps_ready();
    kdrv_apps_pending_write_abort();

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_apps_pending_write_begin(0U));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_apps_pending_write_begin((uint32_t)KAPPS_PENDING_INTENT_OFF + 1U));

    /* Exactly up to it is fine, which makes the one above an off-by-one. */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_pending_write_begin((uint32_t)KAPPS_PENDING_INTENT_OFF));
    kdrv_apps_pending_write_abort();
}

/* Closing short of what was promised is an error. The mailbox holds a
 * container that nothing verifies until it is read back out, so a truncated
 * one that closed cleanly would only be discovered much later. */
TEST_CASE("a mailbox write that closes short says so", "[apps_region]")
{
    static const uint8_t buf[16] = { 0 };

    apps_ready();
    kdrv_apps_pending_write_abort();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_write_begin(64U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_write(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_apps_pending_write_end());

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_consume());
}

TEST_CASE("what the mailbox is given is what it reads back", "[apps_region]")
{
    static uint8_t out[128];
    static uint8_t in[128];

    apps_ready();
    kdrv_apps_pending_write_abort();

    for ( unsigned i = 0U; sizeof(out) > i; ++i )
    {
        out[i] = (uint8_t)(0xC0U + i);
    }

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_write_begin(sizeof(out)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_write(out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_write_end());

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_read(0U, in, sizeof(in)));
    TEST_ASSERT_EQUAL_MEMORY(out, in, sizeof(out));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_consume());
}

TEST_CASE("a second mailbox writer is refused while one is open",
          "[apps_region]")
{
    apps_ready();
    kdrv_apps_pending_write_abort();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_write_begin(64U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_apps_pending_write_begin(64U));
    kdrv_apps_pending_write_abort();
}

/* ---- The intent flag ------------------------------------------------------- */

/* The flag says the parked image is a kodeOS update rather than an app. It
 * lives in its own sector so that writing an image never touches it - and
 * consuming clears BOTH, so an intent can never outlive the image it
 * described and mislabel the next one. */
TEST_CASE("consuming clears the intent as well as the image", "[apps_region]")
{
    const esp_partition_t * const p = mailbox();
    static const uint32_t         magic = KAPPS_OS_INTENT_MAGIC;

    apps_ready();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_consume());
    TEST_ASSERT_FALSE(kdrv_apps_pending_os_intent());

    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_write(p, KAPPS_PENDING_INTENT_OFF, &magic, sizeof(magic)));
    TEST_ASSERT_TRUE(kdrv_apps_pending_os_intent());

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_consume());
    TEST_ASSERT_FALSE_MESSAGE(kdrv_apps_pending_os_intent(),
        "an intent outliving its image would mislabel the next upload");
}

/* Any other word there is not the flag. Erased flash is all ones and reads as
 * no intent, which is the state of every board that has never had one set. */
TEST_CASE("a word that is not the magic is not an intent", "[apps_region]")
{
    const esp_partition_t * const p = mailbox();
    static const uint32_t         nearly = KAPPS_OS_INTENT_MAGIC ^ 1U;

    apps_ready();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_consume());

    TEST_ASSERT_EQUAL_INT(ESP_OK,
        esp_partition_write(p, KAPPS_PENDING_INTENT_OFF, &nearly, sizeof(nearly)));
    TEST_ASSERT_FALSE(kdrv_apps_pending_os_intent());

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_apps_pending_consume());
}
