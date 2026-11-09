/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_aw86233.c
 * @brief The haptic driver, checked by what it says on the bus.
 */
#include <string.h>
#include "unity.h"
#include "Mockkhal_i2c.h"
#include "fakes.h"
#include "kbsp.h"
#include "kdrv_aw86233.h"

#define REG_RSTCFG   0x00U
#define REG_SYSST    0x01U
#define REG_PLAYCFG2 0x07U
#define REG_PLAYCFG3 0x08U
#define REG_PLAYCFG4 0x09U
#define REG_RTPDATA  0x32U
#define REG_GLBRD5   0x3FU
#define REG_SYSCTRL2 0x46U
#define REG_CHIPID_H 0x57U
#define REG_CHIPID_L 0x69U

#define GLB_STANDBY  0x00U
#define GLB_RTP      0x08U

/**
 * @brief Was @p needle in any line klog captured since the last reset?
 *
 * The refusals below are the whole of what the driver says about a part it
 * will not drive, so the line is the assertion: a create that returned
 * KODE_IO_ERROR having said nothing leaves an operator with a silent motor
 * and no reason for it.
 *
 * @param needle A fragment of the line.
 * @return true when some captured line carries it.
 */
static bool logged(const char *needle)
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

static uint8_t s_regs[256];
static bool    s_acks = true;
static bool    s_go_takes = true;    /* does GO move GLB_STATE to RTP?   */
static size_t  s_fifo_bytes;         /* what landed on RTPDATA */
static uint8_t s_fifo_last[64];
static unsigned s_resets;

static kode_status_t on_read(khal_i2c_dev_t dev, uint8_t reg, uint8_t *buf, size_t len, int calls)
{
    (void)dev; (void)calls;
    if ( !s_acks ) { return KODE_ERROR; }
    (void)memcpy(buf, &s_regs[reg], len);
    return KODE_OK;
}
static kode_status_t on_write(khal_i2c_dev_t dev, uint8_t reg, uint8_t val, int calls)
{
    (void)dev; (void)calls;
    if ( !s_acks ) { return KODE_ERROR; }
    if ( (REG_RSTCFG == reg) && (0xAAU == val) ) { s_resets++; return KODE_OK; }
    s_regs[reg] = val;
    /* The chip's own state machine, as far as the driver reads it: GO puts
     * it in RTP, STOP puts it back. */
    if ( REG_PLAYCFG4 == reg )
    {
        if ( ((val & 0x01U) != 0U) && s_go_takes ) { s_regs[REG_GLBRD5] = GLB_RTP; }
        if ( (val & 0x02U) != 0U ) { s_regs[REG_GLBRD5] = GLB_STANDBY; }
    }
    return KODE_OK;
}
static kode_status_t on_write_regs(khal_i2c_dev_t dev, uint8_t reg, const uint8_t *buf, size_t len, int calls)
{
    (void)dev; (void)calls;
    if ( !s_acks ) { return KODE_ERROR; }
    TEST_ASSERT_EQUAL_HEX8(REG_RTPDATA, reg);
    TEST_ASSERT_TRUE(64U >= len);
    (void)memcpy(s_fifo_last, buf, len);
    s_fifo_bytes += len;
    return KODE_OK;
}
static void bus_up(bool present)
{
    Mockkhal_i2c_Init();
    khal_i2c_init_IgnoreAndReturn(KODE_OK);
    khal_i2c_probe_IgnoreAndReturn(present);
    khal_i2c_add_IgnoreAndReturn(KODE_OK);
    khal_i2c_read_reg_StubWithCallback(&on_read);
    khal_i2c_write_reg_StubWithCallback(&on_write);
    khal_i2c_write_regs_StubWithCallback(&on_write_regs);
    (void)memset(s_regs, 0, sizeof(s_regs));
    s_regs[REG_CHIPID_H] = 0x23U;    /* an AW86233: 0x2330 */
    s_regs[REG_CHIPID_L] = 0x30U;
    s_regs[REG_PLAYCFG2] = 0x80U;    /* the reset defaults that matter */
    s_regs[REG_SYSST]    = 0x10U;    /* FF_AES: the FIFO starts empty   */
    s_acks = present; s_go_takes = true; s_fifo_bytes = 0U; s_resets = 0U;
}

TEST_CASE("no ACK at 0x5A is absent, not an error worth a boot", "[kdrv_aw86233]")
{
    bus_up(false);
    TEST_ASSERT_EQUAL_INT(KODE_IO_ERROR, kdrv_aw86233_create());
    TEST_ASSERT_FALSE(kdrv_aw86233_present());
}

/* 0x5A is also where supplier B's touch controller answers, so an ACK there
 * proves nothing: CHIPID does. The family byte decides, because the three
 * AW8623X members share the map; anything else is refused and never driven. */
TEST_CASE("something else answering at 0x5A is refused by its CHIPID", "[kdrv_aw86233]")
{
    bus_up(true);
    s_regs[REG_CHIPID_H] = 0x00U;
    s_regs[REG_CHIPID_L] = 0x00U;
    TEST_ASSERT_EQUAL_INT(KODE_IO_ERROR, kdrv_aw86233_create());
    TEST_ASSERT_FALSE(kdrv_aw86233_present());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_resets, "a part that is not ours is not reset");
    TEST_ASSERT_TRUE(logged("not an AW8623X"));

    bus_up(true);
    s_regs[REG_CHIPID_L] = 0x50U;    /* an AW86235: same family, same map */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_aw86233_create());
    TEST_ASSERT_TRUE(kdrv_aw86233_present());
}

/* The run write is not skipped and then undone for nothing: it is what proves
 * the chip leaves standby at all, and bringing a part up only to find at the
 * first buzz that it never drives is a fault reported in the wrong place. So
 * create() runs it, and then parks the driver stage - which is where a board
 * nobody has asked to buzz belongs. The address is the AW8623X's, 0x46: the
 * AW8622X's 0x44 is a register this part does not have, and a driver writing
 * there is acknowledged and ignored. */
TEST_CASE("create proves the chip can run at 12 kHz and then rests it in standby", "[kdrv_aw86233]")
{
    int32_t v = 0;

    bus_up(true);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_aw86233_create());
    TEST_ASSERT_TRUE(kdrv_aw86233_present());
    TEST_ASSERT_EQUAL_UINT(1U, s_resets);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x42U, s_regs[REG_SYSCTRL2],
                                   "standby set, WAVDAT_MODE 10 (12 kHz), at 0x46");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, s_regs[0x44U],
                                   "nothing was written where the AW8622X keeps SYSCTRL2");
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_aw86233_get(KDRV_AW_CHIPID_FIELD, &v));
    TEST_ASSERT_EQUAL_HEX16(0x2330U, v);
}

/* Standby is the driver stage sleeping, not the chip: I2C stays alive and
 * every register keeps its value. Bit 6 is the owner's; the rate under it is
 * whoever last wrote it, and a rest must not put create()'s own choice back. */
TEST_CASE("standby moves bit 6 and leaves the rate a user chose", "[kdrv_aw86233]")
{
    int32_t v = 0;

    bus_up(true); (void)kdrv_aw86233_create();

    /* A rate that is not the one create() wrote, so a standby that rewrote the
     * whole byte would be visible rather than a coincidence. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_aw86233_set(KDRV_AW_SYSCTRL2, 0x43));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_aw86233_standby(false));
    TEST_ASSERT_EQUAL_HEX8(0x03U, s_regs[REG_SYSCTRL2]);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_aw86233_standby(true));
    TEST_ASSERT_EQUAL_HEX8(0x43U, s_regs[REG_SYSCTRL2]);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_aw86233_get(KDRV_AW_STANDBY, &v));
    TEST_ASSERT_EQUAL_INT(1, v);
}

TEST_CASE("standby is refused when there is no chip", "[kdrv_aw86233]")
{
    bus_up(false);
    TEST_ASSERT_EQUAL_INT(KODE_IO_ERROR, kdrv_aw86233_create());
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_aw86233_standby(true));
}

/* The register byte is reported rather than recomputed by the caller: the
 * percentage a caller asks for does not survive the round trip through
 * PLAYCFG2 - 100 per cent is clamped and every percentage truncates - so a
 * log line or a production record that quotes the request is quoting a number
 * the chip never saw. */
TEST_CASE("rtp_begin clamps the gain at the LRA's rating, reports the byte it wrote "
          "and issues GO in RTP mode", "[kdrv_aw86233]")
{
    uint8_t reg = 0U;
    int32_t v   = 0;

    bus_up(true); (void)kdrv_aw86233_create();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_aw86233_rtp_begin(100U, &reg));
    TEST_ASSERT_EQUAL_HEX8((uint8_t)((80U * 255U) / 100U), s_regs[REG_PLAYCFG2]);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE((uint8_t)((80U * 255U) / 100U), reg,
                                   "it reported the gain that was asked for, not "
                                   "the one the motor got");
    TEST_ASSERT_EQUAL_HEX8(0x01U, s_regs[REG_PLAYCFG3] & 0x03U);
    TEST_ASSERT_EQUAL_HEX8(0x01U, s_regs[REG_PLAYCFG4]);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_aw86233_get(KDRV_AW_GLB_STATE, &v));
    TEST_ASSERT_EQUAL_INT(GLB_RTP, v);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_aw86233_rtp_begin(50U, &reg));
    TEST_ASSERT_EQUAL_HEX8((uint8_t)((50U * 255U) / 100U), s_regs[REG_PLAYCFG2]);
    TEST_ASSERT_EQUAL_HEX8(0x7FU, reg);

    /* The out-parameter is an offer, not an obligation. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_aw86233_rtp_begin(25U, NULL));
    TEST_ASSERT_EQUAL_HEX8(0x3FU, s_regs[REG_PLAYCFG2]);
}

/* The datasheet's playback sequence checks GLB_STATE after GO, and so does the
 * driver: a chip that acknowledges GO and stays in standby is exactly how a
 * parked driver stage - or the wrong register map - fails, and a buzz nobody
 * can feel is worth a reported error. */
TEST_CASE("a GO the chip does not act on is reported, not fed", "[kdrv_aw86233]")
{
    uint8_t reg = 0xEEU;

    bus_up(true); (void)kdrv_aw86233_create();
    s_go_takes = false;
    TEST_ASSERT_EQUAL_INT(KODE_IO_ERROR, kdrv_aw86233_rtp_begin(50U, &reg));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xEEU, reg, "nothing is reported for a buzz that did not start");
    TEST_ASSERT_TRUE(logged("did not enter RTP"));
}

TEST_CASE("feed writes at most 64 bytes and nothing while the FIFO is almost full", "[kdrv_aw86233]")
{
    int8_t pcm[100]; size_t acc = 99U;
    bus_up(true); (void)kdrv_aw86233_create();
    (void)memset(pcm, 7, sizeof(pcm));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_aw86233_rtp_feed(pcm, 100U, &acc));
    TEST_ASSERT_EQUAL_UINT(64U, acc);
    TEST_ASSERT_EQUAL_UINT(64U, s_fifo_bytes);
    s_regs[REG_SYSST] = 0x08U;   /* FF_AFS */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_aw86233_rtp_feed(pcm, 100U, &acc));
    TEST_ASSERT_EQUAL_UINT(0U, acc);
    TEST_ASSERT_EQUAL_UINT(64U, s_fifo_bytes);
    s_regs[REG_SYSST] = 0x00U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_aw86233_rtp_feed(pcm, 10U, &acc));
    TEST_ASSERT_EQUAL_UINT(10U, acc);
}

TEST_CASE("rtp_end is STOP", "[kdrv_aw86233]")
{
    bus_up(true); (void)kdrv_aw86233_create();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_aw86233_rtp_end());
    TEST_ASSERT_EQUAL_HEX8(0x02U, s_regs[REG_PLAYCFG4]);
}

TEST_CASE("the sine is continuous across bursts and peaks at 80 percent", "[kdrv_aw86233]")
{
    int8_t a[64], b[64], whole[128]; uint32_t ph = 0U, ph2 = 0U; int8_t peak = 0;
    kdrv_aw86233_sine_fill(a, 64U, &ph);
    kdrv_aw86233_sine_fill(b, 64U, &ph);
    kdrv_aw86233_sine_fill(whole, 128U, &ph2);
    TEST_ASSERT_EQUAL_INT8_ARRAY(whole, a, 64);
    TEST_ASSERT_EQUAL_INT8_ARRAY(&whole[64], b, 64);
    for ( unsigned i = 0U; 128U > i; ++i ) { if ( whole[i] > peak ) { peak = whole[i]; } }
    TEST_ASSERT_INT_WITHIN(2, 101, peak);
}

TEST_CASE("playing_ms is the chip's clock, not the bus's", "[kdrv_aw86233]")
{
    TEST_ASSERT_EQUAL_UINT32(200U, kdrv_aw86233_playing_ms(2400U));
    TEST_ASSERT_EQUAL_UINT32(1U, kdrv_aw86233_playing_ms(1U));
    TEST_ASSERT_EQUAL_UINT32(0U, kdrv_aw86233_playing_ms(0U));
}

TEST_CASE("every field has a name and the identity is read-only", "[kdrv_aw86233]")
{
    kdrv_aw86233_field_t f;
    for ( uint32_t i = 0U; (uint32_t)KDRV_AW_FIELD_MAX > i; ++i )
    {
        const kdrv_aw86233_field_info_t *info = kdrv_aw86233_field_info((kdrv_aw86233_field_t)i);
        TEST_ASSERT_NOT_NULL(info);
        TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_aw86233_field_by_name(info->name, &f));
        TEST_ASSERT_EQUAL_UINT(i, (uint32_t)f);
    }
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_aw86233_set(KDRV_AW_CHIPID_FIELD, 1));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_aw86233_set(KDRV_AW_GLB_STATE, 8));
}
