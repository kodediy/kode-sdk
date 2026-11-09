/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_pin_bus.c
 * @brief kode_i2c_*(), kode_spi_*() and kode_uart_*() over khal_exp, with bus and GPIO mocked.
 */
/* Named to sort after test_kode_exp.c: the first call here claims J3 for the rest of the binary. */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "Mockkdrv_exp_bus.h"
#include "Mockkhal_exp_port.h"
#include "Mockkhal_gpio.h"
#include "Mockkhal_pwm.h"

#include "fakes.h"
#include "khal_exp.h"
#include "khal_once.h"
#include "khal_system.h"
#include "kode.h"
#include "kode_pin_lock.h"
#include "kode_sys.h"
#include "kode_sys_priv.h"

#define BUS_SDA        3U       /**< The data pin: EXP3.                          */
#define BUS_SDA_GPIO   10U      /**< The GPIO behind it.                          */
#define BUS_SCL        4U       /**< The clock pin: EXP4.                         */
#define BUS_SCL_GPIO   11U      /**< The GPIO behind it.                          */
#define BUS_OTHER      5U       /**< A third pin, for a bus begun elsewhere.      */
#define BUS_OTHER_GPIO 12U      /**< The GPIO behind it.                          */
#define BUS_HZ         100000U  /**< A clock inside the range.                    */
#define BUS_I2C_ID     1U       /**< I2C1, the controller an app may take.        */
#define BUS_ADDR       0x3CU    /**< A device's address.                          */
#define BUS_ADDR_OVER  0x80U    /**< Past the highest 7-bit address.              */
#define BUS_REG        0x0FU    /**< A register number, written before a read.    */
#define BUS_ANSWER     0xA5U    /**< What the device answers.                     */
#define BUS_READ_LEN   2U       /**< Bytes a read asks for.                       */
#define BUS_LENT_TAG   "mesh"   /**< A module that holds a pin by its own loan.   */
#define BUS_STACK      8192U    /**< Stack of the second task, in bytes.          */
#define BUS_PRIO       1U       /**< Priority of the second task.                 */
#define BUS_WAIT_MS    4000U    /**< Longest the case waits for the second task.  */
#define BUS_HOLD_MS    50U      /**< How long the case holds the lock against it. */
#define BUS_ORDER_MAX  4U       /**< Events the order log keeps.                  */
#define SPI_SCK        5U       /**< The SPI clock pin: EXP5.                     */
#define SPI_SCK_GPIO   12U      /**< The GPIO behind it.                          */
#define SPI_MOSI       6U       /**< The SPI data-out pin: EXP6.                  */
#define SPI_MOSI_GPIO  13U      /**< The GPIO behind it.                          */
#define SPI_MISO       7U       /**< The SPI data-in pin: EXP7.                   */
#define SPI_MISO_GPIO  14U      /**< The GPIO behind it.                          */
#define SPI_CS         8U       /**< The SPI chip-select pin: EXP8.               */
#define SPI_CS_GPIO    21U      /**< The GPIO behind it.                          */
#define SPI_HZ         1000000U /**< A clock inside the range.                    */
#define SPI_MODE       3U       /**< A mode inside the range.                     */
#define SPI_LEN        3U       /**< Bytes a transfer moves.                      */
#define SPI_ANSWER     0x5AU    /**< What the device answers.                     */
#define UART_TX        9U       /**< The UART transmit pin: EXP9.                 */
#define UART_TX_GPIO   19U      /**< The GPIO behind it.                          */
#define UART_RX        10U      /**< The UART receive pin: EXP10.                 */
#define UART_RX_GPIO   20U      /**< The GPIO behind it.                          */
#define UART_BAUD      115200U  /**< A rate inside the range.                     */
#define UART_MS        250U     /**< How long a read waits.                       */
#define UART_CAP       8U       /**< Room a read gives.                           */
#define UART_CAME      3U       /**< Bytes that come within the wait.             */
#define UART_BYTE      0x42U    /**< What comes.                                  */
#define ALL_PINS       8U       /**< Pins the three buses take together.          */

/** @brief What happened, in the order it happened, in the lock case. */
typedef enum {
    BUS_EV_GIVE = 1,  /**< The case gave the pins lock back. */
    BUS_EV_OPEN,      /**< The driver was asked to open.     */
} bus_ev_t;

static bus_ev_t    s_order[BUS_ORDER_MAX];  /**< The order log.                      */
static atomic_uint s_order_n;               /**< Events in it.                       */
static uint32_t    s_xfers;                 /**< kdrv_exp_i2c_xfer() calls.          */
static size_t      s_xfer_out_len;          /**< What the last transfer wrote, bytes. */
static size_t      s_xfer_in_len;           /**< What the last transfer read, bytes. */
static uint8_t     s_xfer_out0;             /**< The first byte it wrote.            */
static kode_sys_exp_mode_t s_mode_at_close; /**< The data pin's mode as the driver closed. */
static size_t      s_uart_came;             /**< What the UART read stand-in says came.  */
static uint32_t    s_uart_ms;               /**< The wait it was handed.                 */

/** @brief Note @p ev in the order log. */
static void order_note(bus_ev_t ev)
{
    unsigned const n = atomic_fetch_add(&s_order_n, 1U);

    if ( BUS_ORDER_MAX > n )
    {
        s_order[n] = ev;
    }
}

/**
 * @brief Stand-in for kdrv_exp_i2c_open(): note it in the order log.
 * @return KODE_OK.
 */
static kode_status_t open_noted(uint8_t ctrl, uint32_t sda, uint32_t scl, uint32_t hz, int calls)
{
    (void)ctrl;
    (void)sda;
    (void)scl;
    (void)hz;
    (void)calls;
    order_note(BUS_EV_OPEN);
    return KODE_OK;
}

/**
 * @brief Stand-in for kdrv_exp_i2c_xfer(): note what it was handed, answer BUS_ANSWER into @p in.
 * @return KODE_OK.
 */
static kode_status_t xfer_stub(uint8_t addr, const uint8_t *out, size_t out_len, uint8_t *in,
                               size_t in_len, uint32_t ms, int calls)
{
    (void)calls;
    TEST_ASSERT_EQUAL_HEX8(BUS_ADDR, addr);
    TEST_ASSERT_EQUAL_UINT32(KODE_I2C_TIMEOUT_MS, ms);
    s_xfers++;
    s_xfer_out_len = out_len;
    s_xfer_in_len  = in_len;
    s_xfer_out0    = 0U;
    if ( 0U < out_len )
    {
        s_xfer_out0 = out[0];
    }
    for ( size_t i = 0U; i < in_len; ++i )
    {
        in[i] = BUS_ANSWER;
    }
    return KODE_OK;
}

/** @brief Stand-in for kdrv_exp_i2c_close(): note what the data pin was as it ran. */
static kode_status_t close_noted(int calls)
{
    kode_sys_exp_info_t info;

    (void)calls;
    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_info(BUS_SDA, &info));
    s_mode_at_close = info.mode;
    return KODE_OK;
}

/**
 * @brief What @p pos is doing.
 * @param pos The position.
 * @return Its mode.
 */
static kode_sys_exp_mode_t pin_mode(uint8_t pos)
{
    kode_sys_exp_info_t info;

    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_info(pos, &info));
    return info.mode;
}

/**
 * @brief Who holds controller @p id of kind @p bus in the books.
 * @param bus The kind.
 * @param id  The controller, in IDF's numbering.
 * @return The tag; NULL while nobody does.
 */
static const char *ctrl_holder(kbsp_exp_bus_t bus, uint8_t id)
{
    khal_exp_ctrl_info_t ci  = { KBSP_EXP_UART, 0U, NULL };
    const char          *tag = NULL;

    for ( size_t c = 0U; KBSP_EXP_CTRL_COUNT > c; ++c )
    {
        khal_exp_ctrl_info(c, &ci);
        if ( (ci.bus == bus) && (ci.id == id) )
        {
            tag = ci.tag;
        }
    }
    return tag;
}

/** @brief Who holds I2C1 in the books; NULL while nobody does. */
static const char *i2c_holder(void)
{
    return ctrl_holder(KBSP_EXP_I2C, (uint8_t)KBSP_EXP_I2C_ID);
}

/** @brief Mocks up, any bus a case left begun ended, the header's table FREE, locks real. */
static void bus_fresh(void)
{
    Mockkdrv_exp_bus_Init();
    Mockkhal_gpio_Init();
    Mockkhal_exp_port_Init();
    Mockkhal_pwm_Init();
    fake_mutex_real(true);
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    kdrv_exp_i2c_close_IgnoreAndReturn(KODE_OK);
    kdrv_exp_spi_close_IgnoreAndReturn(KODE_OK);
    kdrv_exp_uart_close_IgnoreAndReturn(KODE_OK);
    khal_gpio_release_Ignore();
    khal_exp_port_adc_close_Ignore();
    khal_pwm_close_Ignore();
    (void)kode_i2c_end();
    (void)kode_spi_end();
    (void)kode_uart_end();
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_init());
    kdrv_exp_i2c_close_StopIgnore();
    kdrv_exp_spi_close_StopIgnore();
    kdrv_exp_uart_close_StopIgnore();
    khal_gpio_release_StopIgnore();
    khal_exp_port_adc_close_StopIgnore();
    khal_pwm_close_StopIgnore();
    s_xfers         = 0U;
    s_mode_at_close = KODE_SYS_EXP_FREE;
    atomic_store(&s_order_n, 0U);
}

/** @brief Every call the case expected happened; the mocks go down. */
static void bus_done(void)
{
    Mockkdrv_exp_bus_Verify();
    Mockkhal_gpio_Verify();
    Mockkhal_exp_port_Verify();
    Mockkhal_pwm_Verify();
    Mockkdrv_exp_bus_Destroy();
    Mockkhal_gpio_Destroy();
    Mockkhal_exp_port_Destroy();
    Mockkhal_pwm_Destroy();
}

/** @brief Begin the bus on BUS_SDA and BUS_SCL, the driver opening on their GPIOs. */
static void bus_begin(void)
{
    kdrv_exp_i2c_open_ExpectAndReturn(BUS_I2C_ID, BUS_SDA_GPIO, BUS_SCL_GPIO, BUS_HZ, KODE_OK);
    TEST_ASSERT_TRUE(kode_i2c_begin(BUS_SDA, BUS_SCL, BUS_HZ));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

/** @brief End the bus: the driver closes, both pins go back to high-Z. */
static void bus_end(void)
{
    kdrv_exp_i2c_close_ExpectAndReturn(KODE_OK);
    khal_gpio_release_Expect(BUS_SDA_GPIO);
    khal_gpio_release_Expect(BUS_SCL_GPIO);
    TEST_ASSERT_TRUE(kode_i2c_end());
}

/** @brief Nothing is lent and no controller is taken: what a refused begin leaves. */
static void expect_nothing_held(void)
{
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(BUS_SDA));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(BUS_SCL));
    TEST_ASSERT_NULL(i2c_holder());
}

TEST_CASE("i2c begin lends both pins under i2c, takes I2C1 and opens the driver on their GPIOs", "[kode_bus]")
{
    kode_sys_exp_info_t info;

    bus_fresh();
    bus_begin();
    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_info(BUS_SDA, &info));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_LENT, info.mode);
    TEST_ASSERT_EQUAL_STRING(KODE_BUS_TAG_I2C, info.tag);
    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_info(BUS_SCL, &info));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_LENT, info.mode);
    TEST_ASSERT_EQUAL_STRING(KODE_BUS_TAG_I2C, info.tag);
    TEST_ASSERT_EQUAL_STRING(KODE_BUS_TAG_I2C, i2c_holder());

    bus_end();
    expect_nothing_held();
    bus_done();
}

TEST_CASE("a second i2c begin fails until the bus is ended", "[kode_bus]")
{
    bus_fresh();
    bus_begin();
    TEST_ASSERT_FALSE(kode_i2c_begin(BUS_OTHER, BUS_OTHER + 1U, BUS_HZ));
    TEST_ASSERT_EQUAL_STRING("the I2C bus is already begun", kode_error());
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(BUS_OTHER));

    bus_end();
    bus_begin();
    bus_end();
    bus_done();
}

TEST_CASE("i2c begin refuses a clock out of range, a pin off the header and one pin twice", "[kode_bus]")
{
    bus_fresh();
    TEST_ASSERT_FALSE(kode_i2c_begin(BUS_SDA, BUS_SCL, KODE_I2C_HZ_MIN - 1U));
    TEST_ASSERT_EQUAL_STRING("frequency 9999 Hz is out of range: use 10000 to 400000", kode_error());
    expect_nothing_held();

    TEST_ASSERT_FALSE(kode_i2c_begin(BUS_SDA, BUS_SCL, KODE_I2C_HZ_MAX + 1U));
    TEST_ASSERT_EQUAL_STRING("frequency 400001 Hz is out of range: use 10000 to 400000", kode_error());
    expect_nothing_held();

    TEST_ASSERT_FALSE(kode_i2c_begin(0U, BUS_SCL, BUS_HZ));
    TEST_ASSERT_EQUAL_STRING("pin 0 does not exist: use 1 to 14", kode_error());
    expect_nothing_held();

    TEST_ASSERT_FALSE(kode_i2c_begin(BUS_SDA, 15U, BUS_HZ));
    TEST_ASSERT_EQUAL_STRING("pin 15 does not exist: use 1 to 14", kode_error());
    expect_nothing_held();

    TEST_ASSERT_FALSE(kode_i2c_begin(BUS_SDA, BUS_SDA, BUS_HZ));
    TEST_ASSERT_EQUAL_STRING("pin 3 is named twice: each line needs a pin of its own", kode_error());
    expect_nothing_held();

    /* The edges of the range are taken. */
    kdrv_exp_i2c_open_ExpectAndReturn(BUS_I2C_ID, BUS_SDA_GPIO, BUS_SCL_GPIO, KODE_I2C_HZ_MAX, KODE_OK);
    TEST_ASSERT_TRUE(kode_i2c_begin(BUS_SDA, BUS_SCL, KODE_I2C_HZ_MAX));
    bus_end();
    kdrv_exp_i2c_open_ExpectAndReturn(BUS_I2C_ID, BUS_SDA_GPIO, BUS_SCL_GPIO, KODE_I2C_HZ_MIN, KODE_OK);
    TEST_ASSERT_TRUE(kode_i2c_begin(BUS_SDA, BUS_SCL, KODE_I2C_HZ_MIN));
    bus_end();
    bus_done();
}

TEST_CASE("i2c begin refuses a pin another module holds, naming it, and lends nothing", "[kode_bus]")
{
    uint8_t pos  = BUS_SCL;
    uint8_t gpio = 0U;

    bus_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_lend(&pos, 1U, BUS_LENT_TAG, &gpio));
    TEST_ASSERT_FALSE(kode_i2c_begin(BUS_SDA, BUS_SCL, BUS_HZ));
    TEST_ASSERT_EQUAL_STRING("pin 4 is lent to mesh", kode_error());
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(BUS_SDA));
    TEST_ASSERT_NULL(i2c_holder());

    khal_gpio_release_Expect(BUS_SCL_GPIO);
    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_return(BUS_LENT_TAG));
    bus_done();
}

TEST_CASE("a pin the app drove is released by i2c begin, then lent", "[kode_bus]")
{
    bus_fresh();
    khal_gpio_output_ExpectAndReturn(BUS_SDA_GPIO, true, KODE_OK);
    TEST_ASSERT_TRUE(kode_pin_write(BUS_SDA, true));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_OUT, pin_mode(BUS_SDA));

    khal_gpio_release_Expect(BUS_SDA_GPIO);
    bus_begin();
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_LENT, pin_mode(BUS_SDA));
    bus_end();
    bus_done();
}

TEST_CASE("a pin lent to the I2C bus refuses kode_pin, naming i2c", "[kode_bus]")
{
    bus_fresh();
    bus_begin();
    TEST_ASSERT_FALSE(kode_pin_read(BUS_SDA));
    TEST_ASSERT_EQUAL_STRING("pin 3 is lent to i2c", kode_error());
    TEST_ASSERT_FALSE(kode_pin_write(BUS_SCL, true));
    TEST_ASSERT_EQUAL_STRING("pin 4 is lent to i2c", kode_error());
    bus_end();
    bus_done();
}

TEST_CASE("i2c end closes the driver before the pins go back", "[kode_bus]")
{
    bus_fresh();
    bus_begin();
    kdrv_exp_i2c_close_Stub(&close_noted);
    khal_gpio_release_Expect(BUS_SDA_GPIO);
    khal_gpio_release_Expect(BUS_SCL_GPIO);
    TEST_ASSERT_TRUE(kode_i2c_end());
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_LENT, s_mode_at_close);
    expect_nothing_held();

    TEST_ASSERT_FALSE(kode_i2c_end());
    TEST_ASSERT_EQUAL_STRING("the I2C bus is not begun", kode_error());
    bus_done();
}

TEST_CASE("a driver that will not open makes i2c begin give the loan back", "[kode_bus]")
{
    bus_fresh();
    kdrv_exp_i2c_open_ExpectAndReturn(BUS_I2C_ID, BUS_SDA_GPIO, BUS_SCL_GPIO, BUS_HZ, KODE_ERROR);
    khal_gpio_release_Expect(BUS_SDA_GPIO);
    khal_gpio_release_Expect(BUS_SCL_GPIO);
    TEST_ASSERT_FALSE(kode_i2c_begin(BUS_SDA, BUS_SCL, BUS_HZ));
    TEST_ASSERT_EQUAL_STRING("the I2C bus could not be opened on pins 3 and 4", kode_error());
    expect_nothing_held();

    /* Nothing was begun, so the next begin goes ahead. */
    bus_begin();
    bus_end();
    bus_done();
}

TEST_CASE("i2c write_read passes both halves to the driver in one transfer", "[kode_bus]")
{
    uint8_t const reg = BUS_REG;
    uint8_t       in[BUS_READ_LEN] = { 0U, 0U };

    bus_fresh();
    bus_begin();
    kdrv_exp_i2c_xfer_Stub(&xfer_stub);
    TEST_ASSERT_TRUE(kode_i2c_write_read(BUS_ADDR, &reg, 1U, in, sizeof(in)));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_xfers);
    TEST_ASSERT_EQUAL_UINT32(1U, s_xfer_out_len);
    TEST_ASSERT_EQUAL_HEX8(BUS_REG, s_xfer_out0);
    TEST_ASSERT_EQUAL_UINT32(BUS_READ_LEN, s_xfer_in_len);
    TEST_ASSERT_EQUAL_HEX8(BUS_ANSWER, in[0]);
    TEST_ASSERT_EQUAL_HEX8(BUS_ANSWER, in[1]);

    /* A write moves only bytes out, a read only bytes in. */
    TEST_ASSERT_TRUE(kode_i2c_write(BUS_ADDR, &reg, 1U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_xfer_out_len);
    TEST_ASSERT_EQUAL_UINT32(0U, s_xfer_in_len);
    TEST_ASSERT_TRUE(kode_i2c_read(BUS_ADDR, in, sizeof(in)));
    TEST_ASSERT_EQUAL_UINT32(0U, s_xfer_out_len);
    TEST_ASSERT_EQUAL_UINT32(BUS_READ_LEN, s_xfer_in_len);
    TEST_ASSERT_EQUAL_UINT32(3U, s_xfers);
    kdrv_exp_i2c_xfer_Stub(NULL);
    bus_end();
    bus_done();
}

TEST_CASE("i2c transfers say why they fail", "[kode_bus]")
{
    uint8_t const reg = BUS_REG;
    uint8_t       in  = 0U;

    bus_fresh();
    TEST_ASSERT_FALSE(kode_i2c_write(BUS_ADDR, &reg, 1U));
    TEST_ASSERT_EQUAL_STRING("the I2C bus is not begun: call kode_i2c_begin() first", kode_error());

    bus_begin();
    TEST_ASSERT_FALSE(kode_i2c_read(BUS_ADDR_OVER, &in, 1U));
    TEST_ASSERT_EQUAL_STRING("address 0x80 is out of range: use 0x00 to 0x7F", kode_error());
    TEST_ASSERT_FALSE(kode_i2c_write(BUS_ADDR, NULL, 1U));
    TEST_ASSERT_EQUAL_STRING("nothing to write: give a buffer and a length of at least 1", kode_error());
    TEST_ASSERT_FALSE(kode_i2c_write_read(BUS_ADDR, &reg, 1U, &in, 0U));
    TEST_ASSERT_EQUAL_STRING("nothing to read: give a buffer and a length of at least 1", kode_error());

    kdrv_exp_i2c_xfer_ExpectAndReturn(BUS_ADDR, NULL, 0U, &in, 1U, KODE_I2C_TIMEOUT_MS, KODE_IO_ERROR);
    TEST_ASSERT_FALSE(kode_i2c_read(BUS_ADDR, &in, 1U));
    TEST_ASSERT_EQUAL_STRING("the device at 0x3C did not answer, or the bus is stuck: check the pull-ups",
                             kode_error());
    bus_end();
    bus_done();
}

TEST_CASE("i2c probe answers true for a device, false and no error for nobody, a stuck bus named", "[kode_bus]")
{
    bus_fresh();
    TEST_ASSERT_FALSE(kode_i2c_probe(BUS_ADDR));
    TEST_ASSERT_EQUAL_STRING("the I2C bus is not begun: call kode_i2c_begin() first", kode_error());

    bus_begin();
    kdrv_exp_i2c_probe_ExpectAndReturn(BUS_ADDR, KODE_OK);
    TEST_ASSERT_TRUE(kode_i2c_probe(BUS_ADDR));
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    kdrv_exp_i2c_probe_ExpectAndReturn(BUS_ADDR, KODE_IO_ERROR);
    TEST_ASSERT_FALSE(kode_i2c_probe(BUS_ADDR));
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    kdrv_exp_i2c_probe_ExpectAndReturn(BUS_ADDR, KODE_TIMEOUT);
    TEST_ASSERT_FALSE(kode_i2c_probe(BUS_ADDR));
    TEST_ASSERT_EQUAL_STRING("the probe of 0x3C timed out, the bus is stuck: check the pull-ups", kode_error());

    kdrv_exp_i2c_probe_ExpectAndReturn(BUS_ADDR, KODE_ERROR);
    TEST_ASSERT_FALSE(kode_i2c_probe(BUS_ADDR));
    TEST_ASSERT_EQUAL_STRING("the I2C bus could not probe 0x3C", kode_error());
    bus_end();
    bus_done();
}

/* ---- SPI and the UART ---------------------------------------------------- */

/**
 * @brief The tag @p pos is lent under.
 * @param pos The position.
 * @return The tag; NULL when it is not lent.
 */
static const char *pin_tag(uint8_t pos)
{
    kode_sys_exp_info_t info;

    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_info(pos, &info));
    return info.tag;
}

/** @brief Begin the SPI bus on SPI_SCK..SPI_CS, the driver opening on SPI3 and their GPIOs. */
static void spi_begin(void)
{
    kdrv_exp_spi_open_ExpectAndReturn(KBSP_EXP_SPI_ID, SPI_SCK_GPIO, SPI_MOSI_GPIO, SPI_MISO_GPIO,
                                      SPI_CS_GPIO, SPI_HZ, SPI_MODE, KODE_OK);
    TEST_ASSERT_TRUE(kode_spi_begin(SPI_SCK, SPI_MISO, SPI_MOSI, SPI_CS, SPI_HZ, SPI_MODE));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

/** @brief End the SPI bus: the driver closes, its four pins go back to high-Z. */
static void spi_end(void)
{
    kdrv_exp_spi_close_ExpectAndReturn(KODE_OK);
    khal_gpio_release_Expect(SPI_SCK_GPIO);
    khal_gpio_release_Expect(SPI_MOSI_GPIO);
    khal_gpio_release_Expect(SPI_MISO_GPIO);
    khal_gpio_release_Expect(SPI_CS_GPIO);
    TEST_ASSERT_TRUE(kode_spi_end());
}

/**
 * @brief Begin the UART on UART_TX and UART_RX, the driver opening on @p port.
 * @param port The UART the books should hand out.
 */
static void uart_begin(uint8_t port)
{
    kdrv_exp_uart_open_ExpectAndReturn(port, UART_TX_GPIO, UART_RX_GPIO, UART_BAUD, KODE_OK);
    TEST_ASSERT_TRUE(kode_uart_begin(UART_TX, UART_RX, UART_BAUD));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

/** @brief End the UART: the driver closes, both pins go back to high-Z. */
static void uart_end(void)
{
    kdrv_exp_uart_close_ExpectAndReturn(KODE_OK);
    khal_gpio_release_Expect(UART_TX_GPIO);
    khal_gpio_release_Expect(UART_RX_GPIO);
    TEST_ASSERT_TRUE(kode_uart_end());
}

/** @brief No SPI pin is lent and SPI3 is not taken: what a refused SPI begin leaves. */
static void expect_no_spi(void)
{
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(SPI_SCK));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(SPI_MOSI));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(SPI_MISO));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(SPI_CS));
    TEST_ASSERT_NULL(ctrl_holder(KBSP_EXP_SPI, (uint8_t)KBSP_EXP_SPI_ID));
}

/** @brief No UART pin is lent and no UART is taken: what a refused UART begin leaves. */
static void expect_no_uart(void)
{
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(UART_TX));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(UART_RX));
    for ( uint8_t id = (uint8_t)KBSP_EXP_UART_FIRST; (uint8_t)KBSP_EXP_UART_LAST >= id; ++id )
    {
        TEST_ASSERT_NULL(ctrl_holder(KBSP_EXP_UART, id));
    }
}

/**
 * @brief Stand-in for kdrv_exp_spi_xfer(): a send-only transfer of SPI_LEN, its bytes checked.
 * @return KODE_OK.
 */
static kode_status_t spi_send_only(const uint8_t *out, uint8_t *in, size_t len, int calls)
{
    (void)calls;
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_NULL(in);
    TEST_ASSERT_EQUAL_UINT32(SPI_LEN, len);
    TEST_ASSERT_EQUAL_HEX8(BUS_REG, out[0]);
    return KODE_OK;
}

/**
 * @brief Stand-in for kdrv_exp_spi_xfer(): a full-duplex transfer, SPI_ANSWER into every byte of @p in.
 * @return KODE_OK.
 */
static kode_status_t spi_duplex(const uint8_t *out, uint8_t *in, size_t len, int calls)
{
    (void)calls;
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_NOT_NULL(in);
    for ( size_t i = 0U; i < len; ++i )
    {
        in[i] = SPI_ANSWER;
    }
    return KODE_OK;
}

/**
 * @brief Stand-in for kdrv_exp_uart_read(): s_uart_came bytes of UART_BYTE came; note the wait.
 * @return KODE_OK.
 */
static kode_status_t uart_read_stub(uint8_t *d, size_t cap, uint32_t ms, size_t *got, int calls)
{
    (void)calls;
    TEST_ASSERT_EQUAL_UINT32(UART_CAP, cap);
    s_uart_ms = ms;
    for ( size_t i = 0U; i < s_uart_came; ++i )
    {
        d[i] = UART_BYTE;
    }
    *got = s_uart_came;
    return KODE_OK;
}

/**
 * @brief Stand-in for kdrv_exp_uart_write(): every byte is queued.
 * @return KODE_OK.
 */
static kode_status_t uart_write_stub(const uint8_t *d, size_t len, size_t *sent, int calls)
{
    (void)calls;
    TEST_ASSERT_NOT_NULL(d);
    *sent = len;
    return KODE_OK;
}

TEST_CASE("spi begin lends four pins under spi, takes SPI3 and opens the driver on their GPIOs", "[kode_bus]")
{
    bus_fresh();
    spi_begin();
    TEST_ASSERT_EQUAL_STRING(KODE_BUS_TAG_SPI, pin_tag(SPI_SCK));
    TEST_ASSERT_EQUAL_STRING(KODE_BUS_TAG_SPI, pin_tag(SPI_MOSI));
    TEST_ASSERT_EQUAL_STRING(KODE_BUS_TAG_SPI, pin_tag(SPI_MISO));
    TEST_ASSERT_EQUAL_STRING(KODE_BUS_TAG_SPI, pin_tag(SPI_CS));
    TEST_ASSERT_EQUAL_STRING(KODE_BUS_TAG_SPI, ctrl_holder(KBSP_EXP_SPI, (uint8_t)KBSP_EXP_SPI_ID));

    TEST_ASSERT_FALSE(kode_spi_begin(SPI_SCK, SPI_MISO, SPI_MOSI, SPI_CS, SPI_HZ, SPI_MODE));
    TEST_ASSERT_EQUAL_STRING("the SPI bus is already begun", kode_error());

    spi_end();
    expect_no_spi();
    TEST_ASSERT_FALSE(kode_spi_end());
    TEST_ASSERT_EQUAL_STRING("the SPI bus is not begun", kode_error());
    bus_done();
}

TEST_CASE("spi begin refuses mode 4, a clock under 100 kHz or over 10 MHz and one pin twice, and lends nothing", "[kode_bus]")
{
    bus_fresh();
    TEST_ASSERT_FALSE(kode_spi_begin(SPI_SCK, SPI_MISO, SPI_MOSI, SPI_CS, SPI_HZ, KODE_SPI_MODE_MAX + 1U));
    TEST_ASSERT_EQUAL_STRING("mode 4 is out of range: use 0 to 3", kode_error());
    expect_no_spi();

    TEST_ASSERT_FALSE(kode_spi_begin(SPI_SCK, SPI_MISO, SPI_MOSI, SPI_CS, KODE_SPI_HZ_MIN - 1U, SPI_MODE));
    TEST_ASSERT_EQUAL_STRING("frequency 99999 Hz is out of range: use 100000 to 10000000", kode_error());
    expect_no_spi();

    TEST_ASSERT_FALSE(kode_spi_begin(SPI_SCK, SPI_MISO, SPI_MOSI, SPI_CS, KODE_SPI_HZ_MAX + 1U, SPI_MODE));
    TEST_ASSERT_EQUAL_STRING("frequency 10000001 Hz is out of range: use 100000 to 10000000", kode_error());
    expect_no_spi();

    TEST_ASSERT_FALSE(kode_spi_begin(SPI_SCK, SPI_MISO, SPI_MOSI, SPI_MOSI, SPI_HZ, SPI_MODE));
    TEST_ASSERT_EQUAL_STRING("pin 6 is named twice: each line needs a pin of its own", kode_error());
    expect_no_spi();

    /* The edges are taken: mode 0 at the fastest clock, mode 3 at the slowest. */
    kdrv_exp_spi_open_ExpectAndReturn(KBSP_EXP_SPI_ID, SPI_SCK_GPIO, SPI_MOSI_GPIO, SPI_MISO_GPIO,
                                      SPI_CS_GPIO, KODE_SPI_HZ_MAX, 0U, KODE_OK);
    TEST_ASSERT_TRUE(kode_spi_begin(SPI_SCK, SPI_MISO, SPI_MOSI, SPI_CS, KODE_SPI_HZ_MAX, 0U));
    spi_end();
    kdrv_exp_spi_open_ExpectAndReturn(KBSP_EXP_SPI_ID, SPI_SCK_GPIO, SPI_MOSI_GPIO, SPI_MISO_GPIO,
                                      SPI_CS_GPIO, KODE_SPI_HZ_MIN, KODE_SPI_MODE_MAX, KODE_OK);
    TEST_ASSERT_TRUE(kode_spi_begin(SPI_SCK, SPI_MISO, SPI_MOSI, SPI_CS, KODE_SPI_HZ_MIN,
                                    KODE_SPI_MODE_MAX));
    spi_end();
    bus_done();
}

TEST_CASE("spi and uart begin give the loan back when the driver will not open or the controller is held", "[kode_bus]")
{
    uint8_t id = 0U;

    bus_fresh();
    kdrv_exp_spi_open_ExpectAndReturn(KBSP_EXP_SPI_ID, SPI_SCK_GPIO, SPI_MOSI_GPIO, SPI_MISO_GPIO,
                                      SPI_CS_GPIO, SPI_HZ, SPI_MODE, KODE_ERROR);
    khal_gpio_release_Expect(SPI_SCK_GPIO);
    khal_gpio_release_Expect(SPI_MOSI_GPIO);
    khal_gpio_release_Expect(SPI_MISO_GPIO);
    khal_gpio_release_Expect(SPI_CS_GPIO);
    TEST_ASSERT_FALSE(kode_spi_begin(SPI_SCK, SPI_MISO, SPI_MOSI, SPI_CS, SPI_HZ, SPI_MODE));
    TEST_ASSERT_EQUAL_STRING("the SPI bus could not be opened on pins 5, 6, 7 and 8", kode_error());
    expect_no_spi();

    kdrv_exp_uart_open_ExpectAndReturn(KBSP_EXP_UART_FIRST, UART_TX_GPIO, UART_RX_GPIO, UART_BAUD,
                                       KODE_ERROR);
    khal_gpio_release_Expect(UART_TX_GPIO);
    khal_gpio_release_Expect(UART_RX_GPIO);
    TEST_ASSERT_FALSE(kode_uart_begin(UART_TX, UART_RX, UART_BAUD));
    TEST_ASSERT_EQUAL_STRING("the UART could not be opened on pins 9 and 10", kode_error());
    expect_no_uart();

    /* SPI3 held under another tag: the four pins are lent, then given back. */
    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_ctrl(KODE_SYS_EXP_SPI, BUS_LENT_TAG, &id));
    khal_gpio_release_Expect(SPI_SCK_GPIO);
    khal_gpio_release_Expect(SPI_MOSI_GPIO);
    khal_gpio_release_Expect(SPI_MISO_GPIO);
    khal_gpio_release_Expect(SPI_CS_GPIO);
    TEST_ASSERT_FALSE(kode_spi_begin(SPI_SCK, SPI_MISO, SPI_MOSI, SPI_CS, SPI_HZ, SPI_MODE));
    TEST_ASSERT_EQUAL_STRING("the SPI controller is held by something else in this app", kode_error());
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(SPI_SCK));
    TEST_ASSERT_EQUAL_STRING(BUS_LENT_TAG, ctrl_holder(KBSP_EXP_SPI, (uint8_t)KBSP_EXP_SPI_ID));
    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_return(BUS_LENT_TAG));

    /* Nothing was begun, so both begin now. */
    spi_begin();
    uart_begin((uint8_t)KBSP_EXP_UART_FIRST);
    spi_end();
    uart_end();
    bus_done();
}

TEST_CASE("spi transfer with in NULL only sends, and refuses a length past KODE_SPI_XFER_MAX", "[kode_bus]")
{
    uint8_t out[KODE_SPI_XFER_MAX + 1U];
    uint8_t in[SPI_LEN] = { 0U, 0U, 0U };

    (void)memset(out, 0, sizeof(out));
    out[0] = BUS_REG;
    bus_fresh();
    TEST_ASSERT_FALSE(kode_spi_transfer(out, in, SPI_LEN));
    TEST_ASSERT_EQUAL_STRING("the SPI bus is not begun: call kode_spi_begin() first", kode_error());

    spi_begin();
    kdrv_exp_spi_xfer_Stub(&spi_send_only);
    TEST_ASSERT_TRUE(kode_spi_transfer(out, NULL, SPI_LEN));
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    kdrv_exp_spi_xfer_Stub(&spi_duplex);
    TEST_ASSERT_TRUE(kode_spi_transfer(out, in, SPI_LEN));
    TEST_ASSERT_EQUAL_HEX8(SPI_ANSWER, in[0]);
    TEST_ASSERT_EQUAL_HEX8(SPI_ANSWER, in[SPI_LEN - 1U]);
    kdrv_exp_spi_xfer_Stub(NULL);

    /* Refused before the driver is asked. */
    TEST_ASSERT_FALSE(kode_spi_transfer(out, in, KODE_SPI_XFER_MAX + 1U));
    TEST_ASSERT_EQUAL_STRING("length 65 is out of range: use 1 to 64", kode_error());
    TEST_ASSERT_FALSE(kode_spi_transfer(out, in, 0U));
    TEST_ASSERT_EQUAL_STRING("length 0 is out of range: use 1 to 64", kode_error());
    TEST_ASSERT_FALSE(kode_spi_transfer(NULL, in, SPI_LEN));
    TEST_ASSERT_EQUAL_STRING("nothing to send: give the bytes out, even to read", kode_error());

    /* The longest transfer goes, and a driver refusal is named. */
    kdrv_exp_spi_xfer_ExpectAndReturn(out, NULL, KODE_SPI_XFER_MAX, KODE_ERROR);
    TEST_ASSERT_FALSE(kode_spi_transfer(out, NULL, KODE_SPI_XFER_MAX));
    TEST_ASSERT_EQUAL_STRING("the SPI transfer failed", kode_error());
    spi_end();
    bus_done();
}

TEST_CASE("uart begin takes the lowest free UART", "[kode_bus]")
{
    uint8_t id = 0U;

    bus_fresh();
    uart_begin((uint8_t)KBSP_EXP_UART_FIRST);
    TEST_ASSERT_EQUAL_STRING(KODE_BUS_TAG_UART, pin_tag(UART_TX));
    TEST_ASSERT_EQUAL_STRING(KODE_BUS_TAG_UART, pin_tag(UART_RX));
    TEST_ASSERT_EQUAL_STRING(KODE_BUS_TAG_UART, ctrl_holder(KBSP_EXP_UART, (uint8_t)KBSP_EXP_UART_FIRST));
    TEST_ASSERT_FALSE(kode_uart_begin(UART_TX, UART_RX, UART_BAUD));
    TEST_ASSERT_EQUAL_STRING("the UART is already begun", kode_error());
    uart_end();
    expect_no_uart();

    /* With the lowest held elsewhere, the next one up. */
    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_ctrl(KODE_SYS_EXP_UART, BUS_LENT_TAG, &id));
    TEST_ASSERT_EQUAL_UINT8(KBSP_EXP_UART_FIRST, id);
    uart_begin((uint8_t)(KBSP_EXP_UART_FIRST + 1U));
    uart_end();
    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_return(BUS_LENT_TAG));
    bus_done();
}

TEST_CASE("uart begin refuses a rate out of range and one pin twice, and lends nothing", "[kode_bus]")
{
    bus_fresh();
    TEST_ASSERT_FALSE(kode_uart_begin(UART_TX, UART_RX, KODE_UART_BAUD_MIN - 1U));
    TEST_ASSERT_EQUAL_STRING("baud 1199 is out of range: use 1200 to 921600", kode_error());
    expect_no_uart();

    TEST_ASSERT_FALSE(kode_uart_begin(UART_TX, UART_RX, KODE_UART_BAUD_MAX + 1U));
    TEST_ASSERT_EQUAL_STRING("baud 921601 is out of range: use 1200 to 921600", kode_error());
    expect_no_uart();

    TEST_ASSERT_FALSE(kode_uart_begin(UART_TX, UART_TX, UART_BAUD));
    TEST_ASSERT_EQUAL_STRING("pin 9 is named twice: each line needs a pin of its own", kode_error());
    expect_no_uart();

    kdrv_exp_uart_open_ExpectAndReturn(KBSP_EXP_UART_FIRST, UART_TX_GPIO, UART_RX_GPIO,
                                       KODE_UART_BAUD_MIN, KODE_OK);
    TEST_ASSERT_TRUE(kode_uart_begin(UART_TX, UART_RX, KODE_UART_BAUD_MIN));
    uart_end();
    kdrv_exp_uart_open_ExpectAndReturn(KBSP_EXP_UART_FIRST, UART_TX_GPIO, UART_RX_GPIO,
                                       KODE_UART_BAUD_MAX, KODE_OK);
    TEST_ASSERT_TRUE(kode_uart_begin(UART_TX, UART_RX, KODE_UART_BAUD_MAX));
    uart_end();
    bus_done();
}

TEST_CASE("uart read returns what came within ms, 0 and no error when nothing did", "[kode_bus]")
{
    uint8_t buf[UART_CAP];

    (void)memset(buf, 0, sizeof(buf));
    bus_fresh();
    TEST_ASSERT_EQUAL_UINT32(0U, kode_uart_read(buf, sizeof(buf), UART_MS));
    TEST_ASSERT_EQUAL_STRING("the UART is not begun: call kode_uart_begin() first", kode_error());

    uart_begin((uint8_t)KBSP_EXP_UART_FIRST);
    kdrv_exp_uart_read_Stub(&uart_read_stub);
    s_uart_came = UART_CAME;
    TEST_ASSERT_EQUAL_UINT32(UART_CAME, kode_uart_read(buf, sizeof(buf), UART_MS));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(UART_MS, s_uart_ms);
    TEST_ASSERT_EQUAL_HEX8(UART_BYTE, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(UART_BYTE, buf[UART_CAME - 1U]);

    s_uart_came = 0U;
    TEST_ASSERT_EQUAL_UINT32(0U, kode_uart_read(buf, sizeof(buf), 0U));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_uart_ms);
    kdrv_exp_uart_read_Stub(NULL);

    TEST_ASSERT_EQUAL_UINT32(0U, kode_uart_read(NULL, sizeof(buf), UART_MS));
    TEST_ASSERT_EQUAL_STRING("nothing to read: give a buffer and a length of at least 1", kode_error());

    kdrv_exp_uart_read_ExpectAnyArgsAndReturn(KODE_ERROR);
    TEST_ASSERT_EQUAL_UINT32(0U, kode_uart_read(buf, sizeof(buf), UART_MS));
    TEST_ASSERT_EQUAL_STRING("the UART could not be read", kode_error());
    uart_end();
    bus_done();
}

TEST_CASE("uart write returns the bytes queued, 0 when it cannot send", "[kode_bus]")
{
    uint8_t const data[UART_CAME] = { UART_BYTE, UART_BYTE, UART_BYTE };

    bus_fresh();
    TEST_ASSERT_EQUAL_UINT32(0U, kode_uart_write(data, sizeof(data)));
    TEST_ASSERT_EQUAL_STRING("the UART is not begun: call kode_uart_begin() first", kode_error());

    uart_begin((uint8_t)KBSP_EXP_UART_FIRST);
    kdrv_exp_uart_write_Stub(&uart_write_stub);
    TEST_ASSERT_EQUAL_UINT32(sizeof(data), kode_uart_write(data, sizeof(data)));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    kdrv_exp_uart_write_Stub(NULL);

    TEST_ASSERT_EQUAL_UINT32(0U, kode_uart_write(data, 0U));
    TEST_ASSERT_EQUAL_STRING("nothing to write: give a buffer and a length of at least 1", kode_error());

    kdrv_exp_uart_write_ExpectAnyArgsAndReturn(KODE_ERROR);
    TEST_ASSERT_EQUAL_UINT32(0U, kode_uart_write(data, sizeof(data)));
    TEST_ASSERT_EQUAL_STRING("the UART could not send", kode_error());
    uart_end();
    bus_done();
}

TEST_CASE("the three buses begun together lend eight distinct pins, each under its own tag", "[kode_bus]")
{
    static const uint8_t pins[ALL_PINS] = {
        BUS_SDA, BUS_SCL, SPI_SCK, SPI_MOSI, SPI_MISO, SPI_CS, UART_TX, UART_RX,
    };
    static const char *const tags[ALL_PINS] = {
        KODE_BUS_TAG_I2C, KODE_BUS_TAG_I2C, KODE_BUS_TAG_SPI, KODE_BUS_TAG_SPI,
        KODE_BUS_TAG_SPI, KODE_BUS_TAG_SPI, KODE_BUS_TAG_UART, KODE_BUS_TAG_UART,
    };
    kode_sys_exp_info_t info;

    bus_fresh();
    bus_begin();
    spi_begin();
    uart_begin((uint8_t)KBSP_EXP_UART_FIRST);
    for ( size_t i = 0U; ALL_PINS > i; ++i )
    {
        TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_info(pins[i], &info));
        TEST_ASSERT_EQUAL(KODE_SYS_EXP_LENT, info.mode);
        TEST_ASSERT_EQUAL_STRING(tags[i], info.tag);
        for ( size_t j = 0U; j < i; ++j )
        {
            kode_sys_exp_info_t other;

            TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_info(pins[j], &other));
            TEST_ASSERT_NOT_EQUAL(other.gpio, info.gpio);
        }
    }
    TEST_ASSERT_EQUAL_STRING(KODE_BUS_TAG_I2C, i2c_holder());
    TEST_ASSERT_EQUAL_STRING(KODE_BUS_TAG_SPI, ctrl_holder(KBSP_EXP_SPI, (uint8_t)KBSP_EXP_SPI_ID));
    TEST_ASSERT_EQUAL_STRING(KODE_BUS_TAG_UART, ctrl_holder(KBSP_EXP_UART, (uint8_t)KBSP_EXP_UART_FIRST));

    /* Ending one leaves the others as they were. */
    spi_end();
    TEST_ASSERT_EQUAL_STRING(KODE_BUS_TAG_I2C, pin_tag(BUS_SDA));
    TEST_ASSERT_EQUAL_STRING(KODE_BUS_TAG_UART, pin_tag(UART_RX));
    uart_end();
    bus_end();
    bus_done();
}

TEST_CASE("a pin one bus holds fails the second bus's begin, naming the first", "[kode_bus]")
{
    bus_fresh();
    bus_begin();
    TEST_ASSERT_FALSE(kode_spi_begin(SPI_SCK, SPI_MISO, BUS_SCL, SPI_CS, SPI_HZ, SPI_MODE));
    TEST_ASSERT_EQUAL_STRING("pin 4 is lent to i2c", kode_error());
    expect_no_spi();
    TEST_ASSERT_EQUAL_STRING(KODE_BUS_TAG_I2C, pin_tag(BUS_SCL));

    TEST_ASSERT_FALSE(kode_uart_begin(UART_TX, BUS_SDA, UART_BAUD));
    TEST_ASSERT_EQUAL_STRING("pin 3 is lent to i2c", kode_error());
    expect_no_uart();

    spi_begin();
    TEST_ASSERT_FALSE(kode_i2c_begin(BUS_OTHER + 1U, SPI_CS, BUS_HZ));
    TEST_ASSERT_EQUAL_STRING("the I2C bus is already begun", kode_error());
    TEST_ASSERT_FALSE(kode_uart_begin(SPI_CS, UART_RX, UART_BAUD));
    TEST_ASSERT_EQUAL_STRING("pin 8 is lent to spi", kode_error());
    expect_no_uart();
    spi_end();
    bus_end();
    bus_done();
}

/* ---- Another task ----------------------------------------------------------- */

/** @brief What the second task leaves for the case. */
typedef struct {
    SemaphoreHandle_t done;   /**< Given when the task has noted its result. */
    bool              begun;  /**< What kode_i2c_begin() answered.           */
} other_t;

/** @brief A task that begins the bus and notes what came of it. */
static void other_begins(void *arg)
{
    other_t *const o = (other_t *)arg;

    o->begun = kode_i2c_begin(BUS_SDA, BUS_SCL, BUS_HZ);
    (void)xSemaphoreGive(o->done);
}

TEST_CASE("test_kode_bus_and_pin_share_the_lock", "[kode_bus]")
{
    other_t o;

    bus_fresh();
    (void)memset(&o, 0, sizeof(o));
    o.done = xSemaphoreCreateBinary();
    kdrv_exp_i2c_open_Stub(&open_noted);

    /* The case holds the lock as a pin call would, and the begin waits for it. */
    TEST_ASSERT_TRUE(kode_pin_lock_take("test_kode_bus"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&other_begins, "t_bus", BUS_STACK, BUS_PRIO, &o));
    TEST_ASSERT_EQUAL_INT(pdFALSE, xSemaphoreTake(o.done, pdMS_TO_TICKS(BUS_HOLD_MS)));
    TEST_ASSERT_EQUAL_UINT32(0U, atomic_load(&s_order_n));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(BUS_SDA));

    order_note(BUS_EV_GIVE);
    kode_pin_lock_give();
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(o.done, pdMS_TO_TICKS(BUS_WAIT_MS)));
    vSemaphoreDelete(o.done);

    TEST_ASSERT_TRUE(o.begun);
    TEST_ASSERT_EQUAL_UINT32(2U, atomic_load(&s_order_n));
    TEST_ASSERT_EQUAL(BUS_EV_GIVE, s_order[0]);
    TEST_ASSERT_EQUAL(BUS_EV_OPEN, s_order[1]);
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_LENT, pin_mode(BUS_SDA));
    kdrv_exp_i2c_open_Stub(NULL);
    bus_end();
    bus_done();
}


static SemaphoreHandle_t s_read_in;  /**< Given by the waiting read once it is inside the driver. */
static SemaphoreHandle_t s_read_go;  /**< Given by the case to let that read finish.            */

/**
 * @brief Stand-in for kdrv_exp_uart_read() that waits on the wire: it says it
 *        is in, waits for the case's word (up to BUS_WAIT_MS), then one byte came.
 * @return KODE_OK.
 */
static kode_status_t uart_read_waits(uint8_t *d, size_t cap, uint32_t ms, size_t *got, int calls)
{
    (void)cap;
    (void)ms;
    (void)calls;
    (void)xSemaphoreGive(s_read_in);
    (void)xSemaphoreTake(s_read_go, pdMS_TO_TICKS(BUS_WAIT_MS));
    d[0] = UART_BYTE;
    *got = 1U;
    return KODE_OK;
}

/** @brief What the reading task leaves for the case. */
typedef struct {
    SemaphoreHandle_t done;  /**< Given when the read has returned. */
    size_t            got;   /**< What kode_uart_read() answered.  */
} reader_t;

/** @brief A task that reads the UART and notes what came of it. */
static void other_reads(void *arg)
{
    reader_t *const r = (reader_t *)arg;
    uint8_t         buf[UART_CAP];

    r->got = kode_uart_read(buf, sizeof(buf), UART_MS);
    (void)xSemaphoreGive(r->done);
}

TEST_CASE("a uart read waiting in one task holds up neither kode_pin_read nor an i2c transfer in another", "[kode_bus]")
{
    uint8_t const reg = BUS_REG;
    reader_t      r;

    bus_fresh();
    bus_begin();
    uart_begin((uint8_t)KBSP_EXP_UART_FIRST);
    (void)memset(&r, 0, sizeof(r));
    r.done    = xSemaphoreCreateBinary();
    s_read_in = xSemaphoreCreateBinary();
    s_read_go = xSemaphoreCreateBinary();
    kdrv_exp_uart_read_Stub(&uart_read_waits);
    kdrv_exp_i2c_xfer_Stub(&xfer_stub);
    khal_gpio_input_ExpectAndReturn(BUS_OTHER_GPIO, KHAL_GPIO_PULL_NONE, KODE_OK);
    khal_gpio_level_ExpectAndReturn(BUS_OTHER_GPIO, true);

    /* The read is inside the driver, holding the UART's lock. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&other_reads, "t_read", BUS_STACK, BUS_PRIO, &r));
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_read_in, pdMS_TO_TICKS(BUS_WAIT_MS)));

    /* While it waits, a pin read and an I2C transfer go ahead and finish. */
    TEST_ASSERT_TRUE(kode_pin_read(BUS_OTHER));
    TEST_ASSERT_TRUE(kode_i2c_write(BUS_ADDR, &reg, 1U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_xfers);
    TEST_ASSERT_EQUAL_INT(pdFALSE, xSemaphoreTake(r.done, 0U));

    (void)xSemaphoreGive(s_read_go);
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(r.done, pdMS_TO_TICKS(BUS_WAIT_MS)));
    TEST_ASSERT_EQUAL_UINT32(1U, r.got);

    kdrv_exp_uart_read_Stub(NULL);
    kdrv_exp_i2c_xfer_Stub(NULL);
    vSemaphoreDelete(r.done);
    vSemaphoreDelete(s_read_in);
    vSemaphoreDelete(s_read_go);
    khal_gpio_release_Expect(BUS_OTHER_GPIO);
    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_release(BUS_OTHER));
    uart_end();
    bus_end();
    bus_done();
}

TEST_CASE("a uart write in one task goes ahead while a read waits in another", "[kode_bus]")
{
    uint8_t const data[UART_CAME] = { UART_BYTE, UART_BYTE, UART_BYTE };
    reader_t      r;

    bus_fresh();
    uart_begin((uint8_t)KBSP_EXP_UART_FIRST);
    (void)memset(&r, 0, sizeof(r));
    r.done    = xSemaphoreCreateBinary();
    s_read_in = xSemaphoreCreateBinary();
    s_read_go = xSemaphoreCreateBinary();
    kdrv_exp_uart_read_Stub(&uart_read_waits);
    kdrv_exp_uart_write_Stub(&uart_write_stub);

    /* The read is inside the driver, holding the UART's receive side. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&other_reads, "t_read", BUS_STACK, BUS_PRIO, &r));
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_read_in, pdMS_TO_TICKS(BUS_WAIT_MS)));

    /* While it waits, a write sends and returns. */
    TEST_ASSERT_EQUAL_UINT32(sizeof(data), kode_uart_write(data, sizeof(data)));
    TEST_ASSERT_EQUAL_INT_MESSAGE(pdFALSE, xSemaphoreTake(r.done, 0U),
                                  "the write waited for the read to end");

    (void)xSemaphoreGive(s_read_go);
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(r.done, pdMS_TO_TICKS(BUS_WAIT_MS)));
    TEST_ASSERT_EQUAL_UINT32(1U, r.got);

    kdrv_exp_uart_read_Stub(NULL);
    kdrv_exp_uart_write_Stub(NULL);
    vSemaphoreDelete(r.done);
    vSemaphoreDelete(s_read_in);
    vSemaphoreDelete(s_read_go);
    uart_end();
    bus_done();
}
