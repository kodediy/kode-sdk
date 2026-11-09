/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_bus.c
 * @brief The standard layer's buses on J3: kode_i2c_*(), kode_spi_*() and kode_uart_*().
 */
#include "kode.h"

#include <inttypes.h>

#include "khal_once.h"
#include "khal_sync.h"
#include "kode_entry.h"
#include "kode_pin_lock.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"

_Static_assert(KODE_I2C_ADDR_MAX == KDRV_EXP_I2C_ADDR_MAX, "the highest address is the driver's");
_Static_assert(KODE_I2C_TIMEOUT_MS == KDRV_EXP_I2C_PROBE_MS, "a probe waits as long as a transfer");
_Static_assert(KODE_SPI_XFER_MAX == KDRV_EXP_SPI_XFER_MAX, "one transfer is the driver's");
_Static_assert(KODE_SPI_MODE_MAX == KDRV_EXP_SPI_MODE_MAX, "the modes are the driver's");
_Static_assert(KODE_SPI_HZ_MIN == KDRV_EXP_SPI_HZ_MIN, "the slowest clock is the driver's");
_Static_assert(KODE_UART_RX_BYTES == KDRV_EXP_UART_RX_BYTES, "the receive ring is the driver's");

/** @brief The pins an I2C bus takes: the data and the clock. */
#define BUS_I2C_PINS 2U

/** @brief Where the data pin sits in the pins a begin lends. */
#define BUS_I2C_SDA 0U

/** @brief Where the clock pin sits in the pins a begin lends. */
#define BUS_I2C_SCL 1U

/** @brief The pins an SPI bus takes: the clock, data out, data in and the chip select. */
#define BUS_SPI_PINS 4U

/** @brief Where the clock pin sits in the pins an SPI begin lends. */
#define BUS_SPI_SCK 0U

/** @brief Where the data-out pin sits. */
#define BUS_SPI_MOSI 1U

/** @brief Where the data-in pin sits. */
#define BUS_SPI_MISO 2U

/** @brief Where the chip-select pin sits. */
#define BUS_SPI_CS 3U

/** @brief The pins a UART takes: transmit and receive. */
#define BUS_UART_PINS 2U

/** @brief Where the transmit pin sits in the pins a UART begin lends. */
#define BUS_UART_TX 0U

/** @brief Where the receive pin sits. */
#define BUS_UART_RX 1U

/** @brief One kind of bus: its books and the words its refusals use. */
typedef struct {
    const char        *tag;               /**< Who holds its pins and controller, KODE_BUS_TAG_*. */
    kode_sys_exp_bus_t ctrl;              /**< The kind of controller it takes.                  */
    const char        *name;              /**< "the I2C bus", "the UART"...: what a refusal names. */
    const char        *chip;              /**< "I2C", "SPI", "UART": what its driver is called.    */
    const char        *held;              /**< The refusal when no controller of its kind is free. */
    kode_status_t (*drv_close)(void);     /**< Its driver's close.                               */
    bool              *begun;             /**< Whether it is begun; written under all its locks, read under one. */
    khal_mutex_t      *lock;              /**< Its own lock, taken before the pins lock, never after. */
    khal_mutex_t      *tx_lock;           /**< The UART's transmit lock, taken after @c lock; NULL for one direction at a time. */
} bus_kind_t;

/** @brief Whether the I2C bus is begun; read and written under s_i2c_bus_lock. */
static bool s_i2c_begun;

/** @brief Whether the SPI bus is begun; read and written under s_spi_bus_lock. */
static bool s_spi_begun;

/** @brief Whether the UART is begun; written under both its locks, read under either. */
static bool s_uart_begun;

/* The bus locks, made together on the first bus call. Each is held by every
 * call on its bus; begin and end then take the pins lock inside it. Order:
 * a bus lock first - the UART's receive lock, then its transmit lock - and
 * the pins lock last; nothing takes a bus lock while holding the pins lock,
 * and no call holds the locks of two buses. */

/** @brief The I2C bus's lock. */
/* cppcheck-suppress misra-c2012-8.9 ; also taken by address in BUS_I2C, a file-scope initializer, so one function is not its only user */
static khal_mutex_t s_i2c_bus_lock;

/** @brief The SPI bus's lock. */
/* cppcheck-suppress misra-c2012-8.9 ; also taken by address in BUS_SPI, a file-scope initializer, so one function is not its only user */
static khal_mutex_t s_spi_bus_lock;

/** @brief The UART's receive lock: kode_uart_read(), and begin and end. */
/* cppcheck-suppress misra-c2012-8.9 ; also taken by address in BUS_UART, a file-scope initializer, so one function is not its only user */
static khal_mutex_t s_uart_rx_lock;

/** @brief The UART's transmit lock: kode_uart_write(), and begin and end. */
/* cppcheck-suppress misra-c2012-8.9 ; also taken by address in BUS_UART, a file-scope initializer, so one function is not its only user */
static khal_mutex_t s_uart_tx_lock;

/** @brief The I2C bus. */
static const bus_kind_t BUS_I2C = {
    KODE_BUS_TAG_I2C, KODE_SYS_EXP_I2C, "the I2C bus", "I2C",
    "the I2C controller is held by something else in this app", &kdrv_exp_i2c_close, &s_i2c_begun,
    &s_i2c_bus_lock, NULL,
};

/** @brief The SPI bus. */
static const bus_kind_t BUS_SPI = {
    KODE_BUS_TAG_SPI, KODE_SYS_EXP_SPI, "the SPI bus", "SPI",
    "the SPI controller is held by something else in this app", &kdrv_exp_spi_close, &s_spi_begun,
    &s_spi_bus_lock, NULL,
};

/** @brief The UART. */
static const bus_kind_t BUS_UART = {
    KODE_BUS_TAG_UART, KODE_SYS_EXP_UART, "the UART", "UART",
    "every UART is held by something else in this app", &kdrv_exp_uart_close, &s_uart_begun,
    &s_uart_rx_lock, &s_uart_tx_lock,
};

/**
 * @brief Make the four bus locks; a run that fails leaves the ones made for the next try.
 * @return true when all four exist.
 */
static bool bus_locks_make(void)
{
    if ( NULL == s_i2c_bus_lock )
    {
        s_i2c_bus_lock = khal_mutex_create();
    }
    if ( NULL == s_spi_bus_lock )
    {
        s_spi_bus_lock = khal_mutex_create();
    }
    if ( NULL == s_uart_rx_lock )
    {
        s_uart_rx_lock = khal_mutex_create();
    }
    if ( NULL == s_uart_tx_lock )
    {
        s_uart_tx_lock = khal_mutex_create();
    }
    return (NULL != s_i2c_bus_lock) && (NULL != s_spi_bus_lock) && (NULL != s_uart_rx_lock)
           && (NULL != s_uart_tx_lock);
}

/**
 * @brief The prologue of every bus call: kode_enter(), then one lock.
 *
 * The locks are made on the first bus call; blocks while another task holds
 * @p lock.
 * @param lock The lock to take: a bus's own, or one of the UART's two.
 * @param name The public name of the call: the name a refusal carries.
 * @return true when the caller holds @p lock; false with the error set when
 *         kode_enter() refused or there is no memory for the locks.
 */
static bool bus_side_take(khal_mutex_t *lock, const char *name)
{
    static khal_once_t locks_once;  /* the one-shot the bus locks are made by */
    bool               ok = kode_enter(name);

    if ( ok )
    {
        ok = khal_once_try(&locks_once, &bus_locks_make);
        if ( ok )
        {
            khal_mutex_lock(*lock);
        }
        else
        {
            kode_err_set("%s: no memory for the bus locks", name);
        }
    }
    return ok;
}

/**
 * @brief The prologue of every call on @p k but a UART read or write: all of its locks, in order.
 * @param k    The bus.
 * @param name The public name of the call: the name a refusal carries.
 * @return true when the caller holds the bus's locks; false with the error set
 *         when kode_enter() refused or there is no memory for the locks.
 */
static bool bus_lock_take(const bus_kind_t *k, const char *name)
{
    bool const ok = bus_side_take(k->lock, name);

    if ( ok && (NULL != k->tx_lock) )
    {
        khal_mutex_lock(*k->tx_lock);
    }
    return ok;
}

/**
 * @brief Give @p k's locks back, at the end of a call bus_lock_take() let in.
 * @param k The bus.
 */
static void bus_lock_give(const bus_kind_t *k)
{
    if ( NULL != k->tx_lock )
    {
        khal_mutex_unlock(*k->tx_lock);
    }
    khal_mutex_unlock(*k->lock);
}

/**
 * @brief The prologue of a begin or an end: the bus's locks, then the pins lock for the books.
 * @param k    The bus.
 * @param name The public name of the call.
 * @return true when the caller holds both; false with the error set and neither held.
 */
static bool bus_books_take(const bus_kind_t *k, const char *name)
{
    bool ok = bus_lock_take(k, name);

    if ( ok )
    {
        ok = kode_pin_lock_take(name);
        if ( !ok )
        {
            bus_lock_give(k);
        }
    }
    return ok;
}

/**
 * @brief Give back what bus_books_take() took: the pins lock, then the bus's locks.
 * @param k The bus.
 */
static void bus_books_give(const bus_kind_t *k)
{
    kode_pin_lock_give();
    bus_lock_give(k);
}

/**
 * @brief Whether @p k may begin: it is not begun already, the error set when it is.
 * @param k The bus.
 * @return true when it is not begun.
 */
static bool bus_unbegun_locked(const bus_kind_t *k)
{
    bool const ok = !*k->begun;

    if ( !ok )
    {
        kode_err_set("%s is already begun", k->name);
    }
    return ok;
}

/**
 * @brief Whether @p k is begun, the error set when it is not.
 * @param k The bus.
 * @return true when it is begun.
 */
static bool bus_begun_locked(const bus_kind_t *k)
{
    bool const ok = *k->begun;

    if ( !ok )
    {
        kode_err_set("%s is not begun: call kode_%s_begin() first", k->name, k->tag);
    }
    return ok;
}

/**
 * @brief Lend @p n readied pins and take a controller for @p k; both or neither.
 * @param k    The bus.
 * @param pins The pins, already readied.
 * @param n    How many.
 * @param gpio Out: the GPIO behind each pin, in the same order.
 * @param id   Out: the controller, in IDF's numbering.
 * @return true when both are held; false with the error set and nothing held.
 */
static bool bus_take_locked(const bus_kind_t *k, const uint8_t *pins, size_t n, uint8_t *gpio,
                            uint8_t *id)
{
    kode_status_t rc = kode_sys_exp_lend(pins, n, k->tag, gpio);
    bool          ok = KODE_OK == rc;

    if ( !ok )
    {
        kode_err_set("the pins could not be lent to %s", k->name);
    }
    else
    {
        rc = kode_sys_exp_ctrl(k->ctrl, k->tag, id);
        if ( KODE_NO_RESOURCES == rc )
        {
            kode_err_set("%s", k->held);
            ok = false;
        }
        else if ( KODE_OK != rc )
        {
            kode_err_set("no controller could be taken for %s", k->name);
            ok = false;
        }
        else
        {
            ok = true;
        }
        if ( !ok )
        {
            (void)kode_sys_exp_return(k->tag);
        }
    }
    return ok;
}

/**
 * @brief What @p k's driver answered to its open: begun, or the loan given back.
 * @param k  The bus.
 * @param rc What the open answered; the caller has set the error when it is not KODE_OK.
 * @return true when the bus is begun; false with nothing held.
 */
static bool bus_opened_locked(const bus_kind_t *k, kode_status_t rc)
{
    bool const ok = KODE_OK == rc;

    if ( !ok )
    {
        (void)kode_sys_exp_return(k->tag);
    }
    *k->begun = ok;
    return ok;
}

/**
 * @brief End @p k: its driver closes first, then the loan goes back, whatever the driver answered.
 * @param k The bus.
 * @return true when it ended cleanly; false with the error set.
 */
static bool bus_end_locked(const bus_kind_t *k)
{
    bool ok = *k->begun;

    if ( !ok )
    {
        kode_err_set("%s is not begun", k->name);
    }
    else
    {
        /* The driver lets go of the pins before the books free them, whatever it answers. */
        ok = k->drv_close() == KODE_OK;
        if ( !ok )
        {
            kode_err_set("the %s driver did not close cleanly; the pins are given back", k->chip);
        }
        (void)kode_sys_exp_return(k->tag);
        *k->begun = false;
    }
    return ok;
}

/**
 * @brief Lend the pins, take a controller and open the driver; all or nothing.
 * @param pins The data and the clock pin, already readied.
 * @param hz   The clock, already judged.
 * @return true when the bus is up; false with the error set and nothing held.
 */
static bool i2c_open_locked(const uint8_t pins[BUS_I2C_PINS], uint32_t hz)
{
    uint8_t gpio[BUS_I2C_PINS] = { 0U, 0U };
    uint8_t ctrl               = 0U;
    bool    ok                 = bus_take_locked(&BUS_I2C, pins, BUS_I2C_PINS, gpio, &ctrl);

    if ( ok )
    {
        kode_status_t const rc = kdrv_exp_i2c_open(ctrl, gpio[BUS_I2C_SDA], gpio[BUS_I2C_SCL], hz);

        if ( KODE_OK != rc )
        {
            kode_err_set("the I2C bus could not be opened on pins %" PRIu32 " and %" PRIu32,
                         (uint32_t)pins[BUS_I2C_SDA], (uint32_t)pins[BUS_I2C_SCL]);
        }
        ok = bus_opened_locked(&BUS_I2C, rc);
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_i2c_begin(uint8_t sda, uint8_t scl, uint32_t hz)
{
    bool ok = bus_books_take(&BUS_I2C, "kode_i2c_begin");

    if ( ok )
    {
        uint8_t const pins[BUS_I2C_PINS] = { sda, scl };

        if ( !bus_unbegun_locked(&BUS_I2C) )
        {
            ok = false;
        }
        else if ( (KODE_I2C_HZ_MIN > hz) || (KODE_I2C_HZ_MAX < hz) )
        {
            kode_err_set("frequency %" PRIu32 " Hz is out of range: use %" PRIu32 " to %" PRIu32, hz,
                         (uint32_t)KODE_I2C_HZ_MIN, (uint32_t)KODE_I2C_HZ_MAX);
            ok = false;
        }
        else if ( !kode_pin_ready_for_bus(pins, BUS_I2C_PINS) )
        {
            ok = false;
        }
        else
        {
            ok = i2c_open_locked(pins, hz);
        }
        bus_books_give(&BUS_I2C);
    }
    return ok;
}

/**
 * @brief Whether a transfer to @p addr may go ahead: the bus is begun and the address is 7-bit.
 * @param addr The address as the caller typed it.
 * @return true when it may; false with the error set.
 */
static bool i2c_ready_locked(uint8_t addr)
{
    bool ok = bus_begun_locked(&BUS_I2C);

    if ( !ok )
    {
        /* The error is set. */
    }
    else if ( KODE_I2C_ADDR_MAX < addr )
    {
        kode_err_set("address 0x%02" PRIX32 " is out of range: use 0x00 to 0x%02" PRIX32,
                     (uint32_t)addr, (uint32_t)KODE_I2C_ADDR_MAX);
        ok = false;
    }
    else
    {
        ok = true;
    }
    return ok;
}

/**
 * @brief One transfer with the device at @p addr, the error set when it fails.
 * @param addr    The address, already judged.
 * @param out     The bytes to write, or NULL.
 * @param out_len How many.
 * @param in      Where the bytes read go, or NULL.
 * @param in_len  How many.
 * @return true when the device answered the whole transfer.
 */
static bool i2c_xfer_locked(uint8_t addr, const uint8_t *out, size_t out_len, uint8_t *in,
                            size_t in_len)
{
    kode_status_t const rc = kdrv_exp_i2c_xfer(addr, out, out_len, in, in_len, KODE_I2C_TIMEOUT_MS);

    if ( KODE_IO_ERROR == rc )
    {
        kode_err_set("the device at 0x%02" PRIX32 " did not answer, or the bus is stuck: check the pull-ups",
                     (uint32_t)addr);
    }
    else if ( KODE_OK != rc )
    {
        kode_err_set("the I2C transfer with 0x%02" PRIX32 " failed", (uint32_t)addr);
    }
    else
    {
        /* Answered. */
    }
    return KODE_OK == rc;
}

/**
 * @brief Whether @p len bytes at @p data are something to move; the error set when not.
 * @param data The caller's buffer.
 * @param len  Its length.
 * @param what "write" or "read", for the error.
 * @return true when @p data is not NULL and @p len is at least one.
 */
static bool bus_buffer_ok(const void *data, size_t len, const char *what)
{
    bool const ok = (NULL != data) && (0U < len);

    if ( !ok )
    {
        kode_err_set("nothing to %s: give a buffer and a length of at least 1", what);
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_i2c_probe(uint8_t addr)
{
    bool found = false;

    if ( bus_lock_take(&BUS_I2C, "kode_i2c_probe") )
    {
        if ( i2c_ready_locked(addr) )
        {
            kode_status_t const rc = kdrv_exp_i2c_probe(addr);

            if ( KODE_OK == rc )
            {
                found = true;
            }
            else if ( KODE_TIMEOUT == rc )
            {
                kode_err_set("the probe of 0x%02" PRIX32 " timed out, the bus is stuck: check the pull-ups",
                             (uint32_t)addr);
            }
            else if ( KODE_IO_ERROR != rc )
            {
                kode_err_set("the I2C bus could not probe 0x%02" PRIX32, (uint32_t)addr);
            }
            else
            {
                /* Nobody there: false, and not an error. */
            }
        }
        bus_lock_give(&BUS_I2C);
    }
    return found;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_i2c_write(uint8_t addr, const uint8_t *data, size_t len)
{
    bool ok = bus_lock_take(&BUS_I2C, "kode_i2c_write");

    if ( ok )
    {
        ok = i2c_ready_locked(addr);
        if ( ok )
        {
            ok = bus_buffer_ok(data, len, "write");
        }
        if ( ok )
        {
            ok = i2c_xfer_locked(addr, data, len, NULL, 0U);
        }
        bus_lock_give(&BUS_I2C);
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_i2c_read(uint8_t addr, uint8_t *data, size_t len)
{
    bool ok = bus_lock_take(&BUS_I2C, "kode_i2c_read");

    if ( ok )
    {
        ok = i2c_ready_locked(addr);
        if ( ok )
        {
            ok = bus_buffer_ok(data, len, "read");
        }
        if ( ok )
        {
            ok = i2c_xfer_locked(addr, NULL, 0U, data, len);
        }
        bus_lock_give(&BUS_I2C);
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_i2c_write_read(uint8_t addr, const uint8_t *out, size_t out_len, uint8_t *in,
                         size_t in_len)
{
    bool ok = bus_lock_take(&BUS_I2C, "kode_i2c_write_read");

    if ( ok )
    {
        ok = i2c_ready_locked(addr);
        if ( ok )
        {
            ok = bus_buffer_ok(out, out_len, "write");
        }
        if ( ok )
        {
            ok = bus_buffer_ok(in, in_len, "read");
        }
        if ( ok )
        {
            ok = i2c_xfer_locked(addr, out, out_len, in, in_len);
        }
        bus_lock_give(&BUS_I2C);
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_i2c_end(void)
{
    bool ok = bus_books_take(&BUS_I2C, "kode_i2c_end");

    if ( ok )
    {
        ok = bus_end_locked(&BUS_I2C);
        bus_books_give(&BUS_I2C);
    }
    return ok;
}

/* ---- SPI ---------------------------------------------------------------- */

/**
 * @brief Lend the four pins, take the SPI host and open the driver; all or nothing.
 * @param pins The clock, data-out, data-in and chip-select pins, already readied.
 * @param hz   The clock, already judged.
 * @param mode The mode, already judged.
 * @return true when the bus is up; false with the error set and nothing held.
 */
static bool spi_open_locked(const uint8_t pins[BUS_SPI_PINS], uint32_t hz, uint8_t mode)
{
    uint8_t gpio[BUS_SPI_PINS] = { 0U, 0U, 0U, 0U };
    uint8_t host               = 0U;
    bool    ok                 = bus_take_locked(&BUS_SPI, pins, BUS_SPI_PINS, gpio, &host);

    if ( ok )
    {
        kode_status_t const rc = kdrv_exp_spi_open(host, gpio[BUS_SPI_SCK], gpio[BUS_SPI_MOSI],
                                                   gpio[BUS_SPI_MISO], gpio[BUS_SPI_CS], hz, mode);

        if ( KODE_OK != rc )
        {
            kode_err_set("the SPI bus could not be opened on pins %" PRIu32 ", %" PRIu32 ", %" PRIu32
                         " and %" PRIu32,
                         (uint32_t)pins[BUS_SPI_SCK], (uint32_t)pins[BUS_SPI_MOSI],
                         (uint32_t)pins[BUS_SPI_MISO], (uint32_t)pins[BUS_SPI_CS]);
        }
        ok = bus_opened_locked(&BUS_SPI, rc);
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_spi_begin(uint8_t sck, uint8_t miso, uint8_t mosi, uint8_t cs, uint32_t hz, uint8_t mode)
{
    bool ok = bus_books_take(&BUS_SPI, "kode_spi_begin");

    if ( ok )
    {
        uint8_t const pins[BUS_SPI_PINS] = { sck, mosi, miso, cs };

        if ( !bus_unbegun_locked(&BUS_SPI) )
        {
            ok = false;
        }
        else if ( (KODE_SPI_HZ_MIN > hz) || (KODE_SPI_HZ_MAX < hz) )
        {
            kode_err_set("frequency %" PRIu32 " Hz is out of range: use %" PRIu32 " to %" PRIu32, hz,
                         (uint32_t)KODE_SPI_HZ_MIN, (uint32_t)KODE_SPI_HZ_MAX);
            ok = false;
        }
        else if ( KODE_SPI_MODE_MAX < mode )
        {
            kode_err_set("mode %" PRIu32 " is out of range: use 0 to %" PRIu32, (uint32_t)mode,
                         (uint32_t)KODE_SPI_MODE_MAX);
            ok = false;
        }
        else if ( !kode_pin_ready_for_bus(pins, BUS_SPI_PINS) )
        {
            ok = false;
        }
        else
        {
            ok = spi_open_locked(pins, hz, mode);
        }
        bus_books_give(&BUS_SPI);
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_spi_transfer(const uint8_t *out, uint8_t *in, size_t len)
{
    bool ok = bus_lock_take(&BUS_SPI, "kode_spi_transfer");

    if ( ok )
    {
        if ( !bus_begun_locked(&BUS_SPI) )
        {
            ok = false;
        }
        else if ( NULL == out )
        {
            kode_err_set("nothing to send: give the bytes out, even to read");
            ok = false;
        }
        else if ( (0U == len) || (KODE_SPI_XFER_MAX < len) )
        {
            kode_err_set("length %zu is out of range: use 1 to %" PRIu32, len,
                         (uint32_t)KODE_SPI_XFER_MAX);
            ok = false;
        }
        else if ( kdrv_exp_spi_xfer(out, in, len) != KODE_OK )
        {
            kode_err_set("the SPI transfer failed");
            ok = false;
        }
        else
        {
            ok = true;
        }
        bus_lock_give(&BUS_SPI);
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_spi_end(void)
{
    bool ok = bus_books_take(&BUS_SPI, "kode_spi_end");

    if ( ok )
    {
        ok = bus_end_locked(&BUS_SPI);
        bus_books_give(&BUS_SPI);
    }
    return ok;
}

/* ---- UART --------------------------------------------------------------- */

/**
 * @brief Lend the two pins, take the lowest free UART and open the driver; all or nothing.
 * @param pins The transmit and receive pins, already readied.
 * @param baud The rate, already judged.
 * @return true when the UART is up; false with the error set and nothing held.
 */
static bool uart_open_locked(const uint8_t pins[BUS_UART_PINS], uint32_t baud)
{
    uint8_t gpio[BUS_UART_PINS] = { 0U, 0U };
    uint8_t port                = 0U;
    bool    ok                  = bus_take_locked(&BUS_UART, pins, BUS_UART_PINS, gpio, &port);

    if ( ok )
    {
        kode_status_t const rc =
            kdrv_exp_uart_open(port, gpio[BUS_UART_TX], gpio[BUS_UART_RX], baud);

        if ( KODE_OK != rc )
        {
            kode_err_set("the UART could not be opened on pins %" PRIu32 " and %" PRIu32,
                         (uint32_t)pins[BUS_UART_TX], (uint32_t)pins[BUS_UART_RX]);
        }
        ok = bus_opened_locked(&BUS_UART, rc);
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_uart_begin(uint8_t tx, uint8_t rx, uint32_t baud)
{
    bool ok = bus_books_take(&BUS_UART, "kode_uart_begin");

    if ( ok )
    {
        uint8_t const pins[BUS_UART_PINS] = { tx, rx };

        if ( !bus_unbegun_locked(&BUS_UART) )
        {
            ok = false;
        }
        else if ( (KODE_UART_BAUD_MIN > baud) || (KODE_UART_BAUD_MAX < baud) )
        {
            kode_err_set("baud %" PRIu32 " is out of range: use %" PRIu32 " to %" PRIu32, baud,
                         (uint32_t)KODE_UART_BAUD_MIN, (uint32_t)KODE_UART_BAUD_MAX);
            ok = false;
        }
        else if ( !kode_pin_ready_for_bus(pins, BUS_UART_PINS) )
        {
            ok = false;
        }
        else
        {
            ok = uart_open_locked(pins, baud);
        }
        bus_books_give(&BUS_UART);
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
size_t kode_uart_write(const uint8_t *data, size_t len)
{
    size_t sent = 0U;

    if ( bus_side_take(BUS_UART.tx_lock, "kode_uart_write") )
    {
        bool ok = bus_begun_locked(&BUS_UART);

        if ( ok )
        {
            ok = bus_buffer_ok(data, len, "write");
        }
        if ( ok )
        {
            if ( kdrv_exp_uart_write(data, len, &sent) != KODE_OK )
            {
                kode_err_set("the UART could not send");
                sent = 0U;
            }
        }
        khal_mutex_unlock(*BUS_UART.tx_lock);
    }
    return sent;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
size_t kode_uart_read(uint8_t *data, size_t cap, uint32_t ms)
{
    size_t got = 0U;

    if ( bus_side_take(BUS_UART.lock, "kode_uart_read") )
    {
        bool ok = bus_begun_locked(&BUS_UART);

        if ( ok )
        {
            ok = bus_buffer_ok(data, cap, "read");
        }
        if ( ok )
        {
            if ( kdrv_exp_uart_read(data, cap, ms, &got) != KODE_OK )
            {
                kode_err_set("the UART could not be read");
                got = 0U;
            }
        }
        khal_mutex_unlock(*BUS_UART.lock);
    }
    return got;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_uart_end(void)
{
    bool ok = bus_books_take(&BUS_UART, "kode_uart_end");

    if ( ok )
    {
        ok = bus_end_locked(&BUS_UART);
        bus_books_give(&BUS_UART);
    }
    return ok;
}
