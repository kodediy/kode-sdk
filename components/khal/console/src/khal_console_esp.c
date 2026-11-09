/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_console_esp.c
 * @brief The console on ESP-IDF: writes the cable in chunks, reads from the driver, never stdin.
 */
#include "khal_console.h"

#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#elif CONFIG_ESP_CONSOLE_UART
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif

#if CONFIG_KHAL_CONSOLE_DOCK
#include "khal_identity.h"
#include "khal_sync.h"
#include "tusb.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "kode_log.h"
#include "esp_private/periph_ctrl.h"
#include "hal/usb_wrap_hal.h"
#include "soc/usb_wrap_struct.h"
#endif

#define CONSOLE_TX_BUF      2048U   /**< Driver TX ring, bytes: sixteen cable chunks. */
#define CONSOLE_RX_BUF      8192U   /**< Driver RX ring, bytes: ~16 ms of a payload at line rate. */
#define READER_STACK        4096U   /**< Reader task stack, bytes. */
#define READER_TIMEOUT_MS   50U     /**< Longest wait before the reader looks for a raw-mode request. */
#define READER_DRAIN_CHUNK  64U     /**< Bytes read per pass when discarding input before raw mode. */
#define READER_REST_MS      1000U   /**< The same wait while the board sleeps: khal_console_rest(). */

static volatile bool s_reader_rest;   /**< The board sleeps: the reader looks up once a second. */

/* ---- The dock port (CONFIG_KHAL_CONSOLE_DOCK): a TinyUSB CDC-ACM on GPIO26/27 ---- */
#if CONFIG_KHAL_CONSOLE_DOCK

#define TAG "khal.dock"

/** @brief Which port a read listens to. */
typedef enum {
    PORT_ANY = 0,   /**< Line mode: both. */
    PORT_CABLE,     /**< The USB-C (USB-Serial/JTAG). */
    PORT_DOCK,      /**< The magnetic pads (TinyUSB CDC-ACM). */
} khal_port_t;

#define DOCK_POLL_MS     5U   /**< Longest cable read before the dock is looked at again. */
#define DOCK_TX_STEP_MS  1U   /**< Wait between attempts to queue the rest of a write. */
#define DOCK_STR_SERIAL  3U   /**< Index of the serial number in s_dock_strings. */

static bool                 s_dock_up;        /**< Driver and CDC installed. */
static khal_mutex_t         s_dock_lock;      /**< Held around every call into the driver, so a lend never uninstalls it under a read or a write. */
static volatile bool        s_dock_stalled;   /**< The host stopped draining TX. */
static khal_sem_t           s_dock_rx_sem;    /**< Signalled by the RX callback. */
static volatile khal_port_t s_last_port = PORT_ANY;   /**< The port that spoke last. */
static volatile khal_port_t s_raw_port  = PORT_ANY;   /**< The port raw mode reads. */

/** @brief USB string descriptors; the serial is the board's printed one, set in dock_init(). */
static const char *s_dock_strings[] = {
    (const char[]){0x09, 0x04},          /* 0: English (0x0409) */
    "Kode",                              /* 1: manufacturer */
    "Dot",                               /* 2: product: macOS shows "Kode Dot" */
    "",                                  /* 3: serial */
    "Kode Dot console",                  /* 4: the CDC interface */
};

/** @brief TinyUSB task: bytes arrived. Wake the reader; never read here. */
static void dock_rx_cb(int itf, cdcacm_event_t *event)
{
    (void)itf;
    (void)event;
    khal_sem_signal(s_dock_rx_sem);
}

/**
 * @brief Install TinyUSB and the CDC port on the pads. The caller holds the lock.
 * @return KODE_OK, or KODE_ERROR with the cable console left intact.
 */
static kode_status_t dock_install(void)
{
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    /* The full-speed PHY on GPIO26/27, the magnetic pads. */
    cfg.port                    = TINYUSB_PORT_FULL_SPEED_0;
    cfg.descriptor.string       = s_dock_strings;
    cfg.descriptor.string_count = (int)(sizeof(s_dock_strings)
                                        / sizeof(s_dock_strings[0]));
    /* The device and configuration descriptors are TinyUSB's defaults on
     * purpose, and it warns that it uses them. */
    kode_log_set_level("tusb_desc", KODE_LOG_ERROR);
    esp_err_t err = tinyusb_driver_install(&cfg);
    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "dock port not available: USB driver: %s",
               esp_err_to_name(err));
        return KODE_ERROR;
    }

    tinyusb_config_cdcacm_t const acm = {
        .cdc_port    = TINYUSB_CDC_ACM_0,
        .callback_rx = &dock_rx_cb,
    };
    err = tinyusb_cdcacm_init(&acm);
    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "dock port not available: CDC: %s", esp_err_to_name(err));
        (void)tinyusb_driver_uninstall();
        return KODE_ERROR;
    }
    s_dock_up = true;
    return KODE_OK;
}

/** @brief Bring the dock port up. A failure leaves the cable console intact. */
static void dock_init(void)
{
    s_dock_rx_sem = khal_sem_create();
    s_dock_lock   = khal_mutex_create();
    if ( (NULL == s_dock_rx_sem) || (NULL == s_dock_lock) )
    {
        return;
    }

    /* Idempotent; an unknown identity leaves the serial empty. */
    (void)khal_identity_init();
    s_dock_strings[DOCK_STR_SERIAL] = khal_identity_pretty();

    khal_mutex_lock(s_dock_lock);
    (void)dock_install();
    khal_mutex_unlock(s_dock_lock);
}

/**
 * @brief Take what the dock's RX FIFO holds, without waiting.
 * @return Bytes read, 0 when there were none.
 */
static int32_t dock_take(void *buf, size_t want)
{
    size_t  got = 0U;
    int32_t n   = 0;

    khal_mutex_lock(s_dock_lock);
    if ( s_dock_up
        && (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf, want, &got) == ESP_OK)
        && (0U < got) )
    {
        n = (int32_t)got;
    }
    khal_mutex_unlock(s_dock_lock);
    return n;
}

/**
 * @brief Read from the dock, waiting at most @p timeout_ms for the first byte.
 * @return Bytes read, 0 when nothing arrived or the port is down.
 */
static int32_t dock_read(void *buf, size_t want, uint32_t timeout_ms)
{
    if ( !s_dock_up )
    {
        if ( 0U != timeout_ms )
        {
            vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        }
        return 0;
    }
    int32_t const n = dock_take(buf, want);
    if ( (0 < n) || (0U == timeout_ms) )
    {
        return n;
    }
    /* A signal left over from bytes already read only costs one extra pass. */
    (void)khal_sem_wait_timeout(s_dock_rx_sem, timeout_ms);
    return dock_take(buf, want);
}

/** @brief Put console output on the dock without ever blocking for long. */
static void dock_write(const char *buf, size_t len)
{
    /* No base, or no computer behind it. */
    if ( !s_dock_up )
    {
        return;
    }
    size_t   off    = 0U;
    uint32_t waited = 0U;
    while ( off < len )
    {
        bool sent = false;

        khal_mutex_lock(s_dock_lock);
        if ( s_dock_up && tud_mounted() )
        {
            off += tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0,
                                              (const uint8_t *)buf + off, len - off);
            (void)tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
            sent = true;
        }
        khal_mutex_unlock(s_dock_lock);
        if ( !sent || (off >= len) )
        {
            break;
        }
        if ( s_dock_stalled || (CONFIG_KHAL_CONSOLE_DOCK_TX_WAIT_MS <= waited) )
        {
            /* Nobody is reading: drop the rest until a write fits again. */
            s_dock_stalled = true;
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(DOCK_TX_STEP_MS));
        waited += DOCK_TX_STEP_MS;
    }
    s_dock_stalled = false;
}

/*
 * The pads are the OTG full-speed PHY's pins. tinyusb_driver_uninstall()
 * stops the PHY but leaves its pad function on (USB_WRAP's usb_pad_enable),
 * the pad pulls under software control and the USB_WRAP block without a
 * clock, so writes to it are lost: the pins read 1 whatever the GPIO matrix
 * says and no UART can drive them. The block is clocked again, as IDF's PHY
 * driver does, and the pad function and its pulls are switched off, so the
 * GPIO matrix owns the pins. The PHY driver sets them again on the next
 * install.
 */
kode_status_t khal_console_dock_release(void)
{
    if ( NULL == s_dock_lock )
    {
        return KODE_OK;   /* dock_init() never ran: nothing holds the pins */
    }
    khal_mutex_lock(s_dock_lock);
    if ( s_dock_up )
    {
        s_dock_up = false;
        (void)tinyusb_driver_uninstall();
    }
    {
        usb_wrap_hal_context_t ctx;
        PERIPH_RCC_ATOMIC()
        {
            usb_wrap_hal_init(&ctx);
        }
    }
    USB_WRAP.otg_conf.usb_pad_enable    = 0U;
    USB_WRAP.otg_conf.pad_pull_override = 0U;
    USB_WRAP.otg_conf.dp_pullup         = 0U;
    USB_WRAP.otg_conf.dp_pulldown       = 0U;
    USB_WRAP.otg_conf.dm_pullup         = 0U;
    USB_WRAP.otg_conf.dm_pulldown       = 0U;
    khal_mutex_unlock(s_dock_lock);
    KLOG_N(TAG, "dock port let go: the pads are the GPIO matrix's");
    return KODE_OK;
}

kode_status_t khal_console_dock_take(void)
{
    kode_status_t rc = KODE_OK;

    if ( NULL == s_dock_lock )
    {
        return KODE_OK;
    }
    khal_mutex_lock(s_dock_lock);
    if ( !s_dock_up )
    {
        rc = dock_install();
    }
    khal_mutex_unlock(s_dock_lock);
    if ( KODE_OK == rc )
    {
        KLOG_N(TAG, "dock port back on the pads");
    }
    return rc;
}
#else
kode_status_t khal_console_dock_release(void)
{
    return KODE_OK;   /* no dock built: the pads are free already */
}

kode_status_t khal_console_dock_take(void)
{
    return KODE_OK;
}
#endif /* CONFIG_KHAL_CONSOLE_DOCK */

/* ---- The cable's output (USB-Serial/JTAG) --------------------------------- */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG

#define CABLE_TX_CHUNK   128U   /**< Bytes per driver call, line endings included. */
#define CABLE_TX_WAIT_MS 500U   /**< Longest wait for the driver's lock and then for room in its TX ring, each: past it nobody is reading. */
#define CABLE_BYTE_MAX   2U     /**< Most one byte becomes on the wire: a newline and its '\r'. */

static char s_cable_tx[CABLE_TX_CHUNK];   /**< One chunk, its line endings already turned. */
static bool s_cable_stalled;              /**< The host stopped draining TX. */

/**
 * @brief Hand one chunk to the cable's driver.
 *
 * The driver's ring takes a chunk whole or not at all: usb_serial_jtag_write_bytes()
 * returns 0 or @p len, never a part, so a result is judged as zero or not.
 * A full ring makes the writer wait for the host, up to CABLE_TX_WAIT_MS. After
 * a timeout every later chunk is tried without waiting, and dropped, until one
 * fits: nobody is reading, and a writer that waited each time would stall AO_Log.
 * @param buf The bytes.
 * @param len How many, at most CABLE_TX_CHUNK.
 */
static void cable_put(const char *buf, size_t len)
{
    TickType_t wait = pdMS_TO_TICKS(CABLE_TX_WAIT_MS);

    if ( s_cable_stalled )
    {
        wait = 0U;
    }
    s_cable_stalled = (0 == usb_serial_jtag_write_bytes(buf, len, wait));
}

/**
 * @brief Put console output on the cable, a chunk per driver call.
 *
 * Through stdout the IDF's VFS hands the driver one byte per call and, after
 * the first byte that finds the ring full, stops waiting and drops every byte
 * that does not fit: a host that reads late loses bytes in the middle of a
 * record, and a task writing a long block spins through the dropped bytes
 * until the idle task's watchdog fires. These are the same bytes with the same
 * line endings, in chunks, each waiting for the host.
 *
 * The caller holds stdout's lock, after flushing it: that is the one lock a
 * log line, a record and an app's own printf share. klog does not write
 * through stdout. Its lines reach the console as AO_Log's sink, which ends in
 * khal_console_write() and so in here, and only its rescue path (an ISR, or
 * before the scheduler runs) bypasses this, through esp_rom_printf().
 * @param buf The bytes.
 * @param len How many.
 */
static void cable_write(const char *buf, size_t len)
{
    /* usb_serial_jtag_is_connected() is not asked: it reads false for a
     * tick now and then while the host is reading, and each time a whole
     * write was dropped. With no host, cable_put's stall rule drops instead,
     * after one wait. */
    size_t fill = 0U;
    for ( size_t i = 0U; i < len; ++i )
    {
        /* Room for the byte and the '\r' a newline may bring. */
        if ( sizeof(s_cable_tx) < (fill + CABLE_BYTE_MAX) )
        {
            cable_put(s_cable_tx, fill);
            fill = 0U;
        }
#if CONFIG_LIBC_STDOUT_LINE_ENDING_CRLF
        if ( '\n' == buf[i] )
        {
            s_cable_tx[fill] = '\r';
            ++fill;
        }
        s_cable_tx[fill] = buf[i];
#elif CONFIG_LIBC_STDOUT_LINE_ENDING_CR
        s_cable_tx[fill] = buf[i];
        if ( '\n' == buf[i] )
        {
            s_cable_tx[fill] = '\r';
        }
#else
        s_cable_tx[fill] = buf[i];
#endif
        ++fill;
    }
    if ( 0U < fill )
    {
        cable_put(s_cable_tx, fill);
    }
}
#endif /* CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG */

void khal_console_init(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = CONSOLE_RX_BUF,
        .tx_buffer_size = CONSOLE_TX_BUF,
    };
    (void)usb_serial_jtag_driver_install(&cfg);
    /* Without the driver, stdin is the ROM's non-blocking path. */
    usb_serial_jtag_vfs_use_driver();
    /* Binary input arrives verbatim: the VFS default turns CR into LF. */
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
#elif CONFIG_ESP_CONSOLE_UART
    uart_port_t const port = (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM;
    if ( !uart_is_driver_installed(port) )
    {
        (void)uart_driver_install(port, (int)CONSOLE_RX_BUF, 0, 0, NULL, 0);
    }
    uart_vfs_dev_use_driver(port);
    (void)uart_vfs_dev_port_set_rx_line_endings(port, ESP_LINE_ENDINGS_LF);
#endif
#if CONFIG_KHAL_CONSOLE_DOCK
    dock_init();
#endif
}

/**
 * @brief Take what the cable's driver has, waiting at most @p timeout_ms for it.
 * @param[out] buf        Where the bytes go.
 * @param[in]  want       At most this many.
 * @param[in]  timeout_ms How long to wait for the first of them.
 * @return Bytes read, 0 on a timeout, negative when there is no transport.
 */
static int32_t cable_read(void *buf, size_t want, uint32_t timeout_ms)
{
    TickType_t const ticks = pdMS_TO_TICKS(timeout_ms);

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    return (int32_t)usb_serial_jtag_read_bytes(buf, (uint32_t)want, ticks);
#elif CONFIG_ESP_CONSOLE_UART
    return (int32_t)uart_read_bytes((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM, buf,
                                    (uint32_t)want, ticks);
#else
    (void)buf;
    (void)want;
    (void)ticks;
    return -1;
#endif
}

/**
 * @brief The console's one read: the cable, and the dock when it is built in.
 *
 * Raw mode reads only its own port. Line mode checks the dock, then the cable
 * for a short slice, until something arrives, and remembers which port spoke.
 */
static int32_t console_read(void *buf, size_t want, uint32_t timeout_ms)
{
#if CONFIG_KHAL_CONSOLE_DOCK
    khal_port_t const raw = s_raw_port;
    if ( PORT_DOCK == raw )
    {
        return dock_read(buf, want, timeout_ms);
    }
    if ( PORT_CABLE == raw )
    {
        return cable_read(buf, want, timeout_ms);
    }

    TickType_t const start = xTaskGetTickCount();
    TickType_t const total = pdMS_TO_TICKS(timeout_ms);
    for ( ;; )
    {
        int32_t n = dock_read(buf, want, 0U);
        if ( 0 < n )
        {
            s_last_port = PORT_DOCK;
            return n;
        }
        TickType_t const spent = xTaskGetTickCount() - start;
        TickType_t       slice = pdMS_TO_TICKS(DOCK_POLL_MS);
        if ( s_reader_rest )
        {
            /* Nothing on the dock to look at: the whole wait, in one go. */
            slice = total;
        }
        if ( spent >= total )
        {
            slice = 0;
        }
        else if ( (total - spent) < slice )
        {
            slice = total - spent;
        }
        else
        {
            /* A whole slice fits in what is left. */
        }
        n = cable_read(buf, want, (uint32_t)pdTICKS_TO_MS(slice));
        if ( 0 < n )
        {
            s_last_port = PORT_CABLE;
            return n;
        }
        if ( (xTaskGetTickCount() - start) >= total )
        {
            return 0;
        }
        if ( (0 > n) && (0 == slice) )
        {
            /* No cable transport: do not spin. */
            vTaskDelay(1);
        }
    }
#else
    return cable_read(buf, want, timeout_ms);
#endif
}

/**
 * @brief Read one byte, blocking up to @p timeout_ms.
 * @return The byte (0..255), or -1 on a timeout or with no transport.
 */
static int32_t console_getc(uint32_t timeout_ms)
{
    uint8_t byte = 0U;
    int32_t const n = console_read(&byte, 1U, timeout_ms);

    if ( 1 != n )
    {
        return -1;
    }
    return (int32_t)byte;
}

int32_t khal_console_read_raw(void *buf, size_t want, uint32_t timeout_ms)
{
    if ( (NULL == buf) || (0U == want) )
    {
        return -1;
    }
    int32_t const n = console_read(buf, want, timeout_ms);

    if ( 0 > n )
    {
        return -1;
    }
    return n;
}

void khal_console_write(const char *buf, size_t len)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    /* Under stdout's lock, after what it holds: an app's own printf and this
     * still come out whole and in order. */
    flockfile(stdout);
    (void)fflush(stdout);
    if ( usb_serial_jtag_is_driver_installed() )
    {
        cable_write(buf, len);
    }
    else
    {
        (void)fwrite(buf, 1U, len, stdout);
        (void)fflush(stdout);
    }
    funlockfile(stdout);
#else
    (void)fwrite(buf, 1U, len, stdout);
    (void)fflush(stdout);
#endif
#if CONFIG_KHAL_CONSOLE_DOCK
    dock_write(buf, len);
#endif
}

/* ---- The reader task ----------------------------------------------------- */

static void (*s_on_byte)(uint8_t byte);   /**< Where line-mode bytes go. */

/* Raw mode's handshake: the requester writes s_pump_req and s_on_armed; only
 * the reader touches the transport, drains it and promotes the request. */
static void (*volatile s_pump)(void);       /**< Raw mode is live while set. */
static void (*volatile s_pump_req)(void);   /**< A request the reader has not taken yet. */
static void (*s_on_armed)(void);            /**< Called once raw mode is live. */

/** @brief Report bytes one at a time, or run the pump while raw mode is live. */
static void khal_reader_task(void *pv)
{
    (void)pv;
    for ( ;; )
    {
        if ( NULL != s_pump_req )
        {
#if CONFIG_KHAL_CONSOLE_DOCK
            /* The request came from the port that spoke last; take it before the drain. */
            khal_port_t const from = s_last_port;
#endif
            uint8_t junk[READER_DRAIN_CHUNK];
            while ( khal_console_read_raw(junk, sizeof(junk), 0U) > 0 )
            {
            }
#if CONFIG_KHAL_CONSOLE_DOCK
            if ( PORT_DOCK == from )
            {
                s_raw_port = PORT_DOCK;
            }
            else
            {
                s_raw_port = PORT_CABLE;
            }
#endif
            s_pump     = s_pump_req;
            s_pump_req = NULL;
            s_on_armed();
            continue;
        }
        void (*const pump)(void) = s_pump;
        if ( NULL != pump )
        {
            pump();
            continue;
        }
        uint32_t wait_ms = READER_TIMEOUT_MS;
        if ( s_reader_rest )
        {
            wait_ms = READER_REST_MS;
        }
        int32_t const c = console_getc(wait_ms);
        if ( 0 > c )
        {
            continue;
        }
        /* A byte that raced a raw-mode request is residue: drop it. */
        if ( NULL == s_pump_req )
        {
            s_on_byte((uint8_t)c);
        }
    }
}

kode_status_t khal_console_raw_begin(void (*pump)(void), void (*on_armed)(void))
{
    if ( (NULL == pump) || (NULL == on_armed)
        || (NULL != s_pump) || (NULL != s_pump_req) )
    {
        return KODE_DENIED;
    }
    s_on_armed = on_armed;
    /* Last: this is what the reader acts on. */
    s_pump_req = pump;
    return KODE_OK;
}

void khal_console_raw_end(void)
{
    s_pump     = NULL;
    s_pump_req = NULL;
#if CONFIG_KHAL_CONSOLE_DOCK
    s_raw_port = PORT_ANY;
#endif
}

void khal_console_rest(bool rest)
{
    s_reader_rest = rest;
}

kode_status_t khal_console_start_reader(void (*on_byte)(uint8_t byte), uint32_t prio)
{
    if ( NULL == on_byte )
    {
        return KODE_ERROR;
    }
    s_on_byte = on_byte;
    BaseType_t const ok = xTaskCreate(&khal_reader_task, "khal.rd",
                                      READER_STACK, NULL,
                                      (UBaseType_t)prio, NULL);
    if ( pdPASS != ok )
    {
        return KODE_NO_RESOURCES;
    }
    return KODE_OK;
}
