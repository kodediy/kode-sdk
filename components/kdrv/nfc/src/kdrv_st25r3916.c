/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_st25r3916.c
 * @brief The NFC reader, and the whole of ST's RFAL platform.
 */
#include "kdrv_st25r3916.h"

#include "sdkconfig.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kbsp_board.h"
#include "kbsp_pins.h"
#include "khal_gpio.h"
#include "khal_time.h"
#include "knfc_emu_img.h"
#include "knfc_ident.h"
#include "knfc_power.h"
#include "kode_log.h"

#include "rfal_nfc.h"
#include "rfal_nfca.h"
#include "rfal_rf.h"
#include "rfal_t2t.h"
#include "rfal_utils.h"
#include "soc/soc_caps.h"
#include "st25r3916.h"
#include "st25r3916_com.h"
#include "st25r3916_irq.h"

#define TAG "kdrv.nfc"

/** @brief The host these five pins are on (kbsp's KBSP_SPI_NFC, SPI2), and
 *         the reader is alone on it. */
#define KDRV_ST25_SPI_HOST      ((spi_host_device_t)KBSP_SPI_NFC)
_Static_assert((KBSP_SPI_NFC > (uint32_t)SPI1_HOST)
               && (KBSP_SPI_NFC < (uint32_t)SOC_SPI_PERIPH_NUM),
               "KBSP_SPI_NFC must name a general SPI host, not the flash's");
/** @brief What validation ran at. The part allows more; nothing here waits. */
#define KDRV_ST25_SPI_HZ        (1 * 1000 * 1000)
/* It outranks AO_Nfc: RFAL's interrupt wait spins, never yielding, on a word this task writes. */
/** @brief Above AO_Nfc and every Active Object but AO_Rfid, whose level this
 *         is too; 24 is left to ESP-IDF's own. */
#define KDRV_ST25_IRQ_PRIO      (configMAX_PRIORITIES - 2)
/** @brief The servicer does one thing and needs no depth to do it. */
#define KDRV_ST25_IRQ_STACK     4096U
/** @brief A Type-2 READ answers with four blocks whatever it was asked. */
#define KDRV_ST25_T2T_READ_LEN  16U
/** @brief A Type-2 block. */
#define KDRV_ST25_T2T_BLOCK_LEN 4U
/** @brief The most the SPI peripheral carries in one transaction without
 *         DMA: its own buffer, 64 bytes. A longer transfer goes out in pieces
 *         of this, under the one chip-select RFAL already holds. */
#define KDRV_ST25_SPI_CHUNK   64U
_Static_assert(KDRV_ST25_SPI_CHUNK <= (uint32_t)SOC_SPI_MAXIMUM_BUFFER_SIZE,
               "a piece must fit the SPI peripheral's own buffer");
/* The identity the park accepts is the one RFAL's own check accepts. */
_Static_assert(KDRV_ST25R3916_IC_TYPE_MASK == ST25R3916_REG_IC_IDENTITY_ic_type_mask,
               "the type field is RFAL's");
_Static_assert(KDRV_ST25R3916_IC_TYPE_B == ST25R3916_REG_IC_IDENTITY_ic_type_st25r3916B,
               "the ST25R3916B's code is RFAL's");
_Static_assert(KDRV_ST25R3916_IC_REV_MASK == ST25R3916_REG_IC_IDENTITY_ic_rev_mask,
               "the revision field is RFAL's");

/** @brief The park's IC_IDENTITY read: the address byte and the one answered, in bits. */
#define KDRV_ST25_PARK_READ_BITS 16U
/** @brief The park's Set Default: one command byte, in bits. */
#define KDRV_ST25_PARK_CMD_BITS  8U
/** @brief The SPI mode bits of a register read (st25r3916_com.c's READ_MODE). */
#define ST25_SPI_READ            0x40U
/** @brief How long a reselect waits for the card to answer again. */
#define KDRV_ST25_RESELECT_MS  50U

/* The chip driver keeps this visible; the recursive lock below is what
 * actually excludes. Declared extern by rfal_platform.h. */
uint8_t globalCommProtectCnt;

static spi_device_handle_t  s_spi;
static SemaphoreHandle_t    s_spi_lock;  /**< One SPI transaction at a time. */
static SemaphoreHandle_t    s_comm;      /**< Recursive: RFAL nests it.     */
static SemaphoreHandle_t    s_worker;    /**< Recursive, and independent.   */
static SemaphoreHandle_t    s_irq_sem;   /**< The ISR wakes the servicer.   */
static TaskHandle_t         s_irq_task;
static void               (*s_chip_isr)(void);
static bool                 s_present;
static bool                 s_field;
static bool                 s_listening; /**< A listen window is open.      */
static bool                 s_emulating; /**< An emulation window is open.   */
static knfc_power_state_t   s_power;     /**< Rest/wake state; see nfc_power_set. */
static uint8_t              s_identity;
static uint32_t             s_tags;
static uint32_t             s_errors;
static rfalNfcDiscoverParam s_disc;
static rfalNfcDevice       *s_dev;

/* ==========================================================================
 * The platform RFAL runs on
 * ========================================================================== */

void rfal_port_spi_select(void)
{
    (void)gpio_set_level((gpio_num_t)KBSP_PIN_NFC_CS, 0);
}

void rfal_port_spi_deselect(void)
{
    (void)gpio_set_level((gpio_num_t)KBSP_PIN_NFC_CS, 1);
}

void rfal_port_spi_txrx(const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    /* No DMA on this bus, and that is not thrift: it is what makes a read
     * mean what the chip said.
     *
     * This port used to hand ESP-IDF buffers in PSRAM with
     * SPI_TRANS_DMA_USE_PSRAM, because RFAL asks for odd lengths (a byte of
     * address, a byte of answer, a UID's worth) that the P4's cache-line rule
     * makes ESP-IDF bounce, and the internal DMA pool they would be bounced
     * from runs at nearly zero free bytes on this board. That path shares the
     * PSRAM's own bus, and now and then a read came back holding something
     * the chip never sent: a register that read 0x00 or 0x67 where it held
     * 0x07, an identity of 0x00, an oscillator "not stable" that was, a FIFO
     * with no tag in it. ESP-IDF raised neither of its flags for it
     * (SPI_TRANS_DMA_RX_FAIL, SPI_TRANS_DMA_TX_FAIL). At the end-of-line
     * station it showed as a reader that stopped seeing its tag for the rest
     * of a boot, about once in ten boots, with nothing in the log.
     *
     * Without DMA the peripheral takes a transaction from its own 64-byte
     * buffer, filled and emptied by the CPU: no DMA memory of either kind, no
     * bounce, no cache to keep in step. A transfer longer than that goes out
     * in pieces. That is safe here and only here: the chip-select is a pin
     * this port drives by hand (rfal_port_spi_select()), held across every
     * call RFAL makes for one access, so the chip sees one unbroken access
     * however many transactions carry it.
     *
     * ST calls this in three shapes - rx NULL for a write, tx NULL for a
     * read, and tx == rx for a transceive (st25r3916_com.c:282, without
     * ST25R_COM_SINGLETXRX ever being defined). A read clocks zeros out.
     */
    static uint8_t s_tx_buf[KDRV_ST25_SPI_CHUNK];   /* One piece out. */
    static uint8_t s_rx_buf[KDRV_ST25_SPI_CHUNK];   /* One piece in.  */
    uint16_t       done = 0U;
    bool           failed = false;

    if ( (NULL == s_spi) || (NULL == s_spi_lock) || (0U == len) )
    {
        return;
    }

    /* One transaction at a time, and this lock is not ST's.
     *
     * ESP-IDF keeps the transaction in flight in the HOST's own state
     * (host->cur_trans_buf holds a pointer to the caller's
     * spi_transaction_t), so two tasks inside spi_device_polling_transmit()
     * at once leave the second's descriptor there and the first tears down
     * against it. Two tasks is exactly what this port has: AO_Nfc pumping
     * RFAL's worker, and the interrupt servicer above it reading the chip's
     * status registers. The pieces of one transfer and the two buffers they
     * share are under it as well.
     */
    (void)xSemaphoreTakeRecursive(s_spi_lock, portMAX_DELAY);
    while ( (done < len) && !failed )
    {
        uint16_t piece = (uint16_t)(len - done);

        if ( (uint16_t)KDRV_ST25_SPI_CHUNK < piece )
        {
            piece = (uint16_t)KDRV_ST25_SPI_CHUNK;
        }
        if ( NULL != tx )
        {
            (void)memcpy(s_tx_buf, &tx[done], piece);
        }
        else
        {
            (void)memset(s_tx_buf, 0, piece);
        }

        spi_transaction_t t;
        (void)memset(&t, 0, sizeof(t));
        t.length    = (size_t)piece * 8U;
        t.rxlength  = (size_t)piece * 8U;   /* Said, not left to be inferred. */
        t.tx_buffer = s_tx_buf;
        t.rx_buffer = s_rx_buf;

        if ( spi_device_polling_transmit(s_spi, &t) != ESP_OK )
        {
            KLOG_E(TAG, "SPI transfer of %u bytes failed at byte %u",
                   (unsigned)len, (unsigned)done);
            failed = true;
        }
        else
        {
            if ( NULL != rx )
            {
                (void)memcpy(&rx[done], s_rx_buf, piece);
            }
            done = (uint16_t)(done + piece);
        }
    }
    (void)xSemaphoreGiveRecursive(s_spi_lock);
}

bool rfal_port_irq_pin_high(void)
{
    return khal_gpio_level(KBSP_PIN_NFC_IRQ);
}

void rfal_port_irq_set_callback(void (*cb)(void))
{
    s_chip_isr = cb;
}

void rfal_port_protect_comm(void)
{
    if ( NULL != s_comm )
    {
        (void)xSemaphoreTakeRecursive(s_comm, portMAX_DELAY);
    }
}

void rfal_port_unprotect_comm(void)
{
    if ( NULL != s_comm )
    {
        (void)xSemaphoreGiveRecursive(s_comm);
    }
}

void rfal_port_protect_worker(void)
{
    if ( NULL != s_worker )
    {
        (void)xSemaphoreTakeRecursive(s_worker, portMAX_DELAY);
    }
}

void rfal_port_unprotect_worker(void)
{
    if ( NULL != s_worker )
    {
        (void)xSemaphoreGiveRecursive(s_worker);
    }
}

uint32_t rfal_port_tick_ms(void)
{
    return khal_time_ms();
}

void rfal_port_delay_ms(uint32_t ms)
{
    khal_delay_ms(ms);
}

void rfal_port_log(const char *fmt, ...)
{
    /* RFAL logs a handful of lines at initialisation and nothing in a steady
     * state, so one line's worth of stack is enough and a truncated line is
     * better than a heap allocation on this path. They go out at debug,
     * where `log_level tag="kdrv.nfc" level=d` reaches them like everything
     * else. */
    char    line[128];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    KLOG_D(TAG, "%s", line);
}

void rfal_port_error(const char *file, int line)
{
    KLOG_E(TAG, "RFAL called its own error handler at %s:%d", file, line);
}

/* ==========================================================================
 * The interrupt: an ISR that only wakes, and a servicer that outranks
 * ========================================================================== */

/** @brief Runs in interrupt context. Wakes the servicer and nothing else. */
static void irq_isr(void *arg)
{
    (void)arg;
    if ( NULL != s_irq_sem )
    {
        BaseType_t woken = pdFALSE;
        (void)xSemaphoreGiveFromISR(s_irq_sem, &woken);
        if ( woken == pdTRUE )
        {
            portYIELD_FROM_ISR();
        }
    }
}

/**
 * @brief Read the chip's interrupt registers, then let the line fire again.
 *
 * @param arg Unused.
 */
static void irq_service(void *arg)
{
    (void)arg;
    for ( ;; )
    {
        if ( xSemaphoreTake(s_irq_sem, portMAX_DELAY) != pdTRUE )
        {
            continue;
        }
        if ( NULL != s_chip_isr )
        {
            s_chip_isr();
        }
        khal_gpio_irq_enable(KBSP_PIN_NFC_IRQ);
    }
}

/* ==========================================================================
 * Bring-up
 * ========================================================================== */

/**
 * @brief Bring the reader to @p want, resting or waking the chip as needed.
 *
 * The reader rests with its oscillator powered down between uses and wakes
 * only for one - the field, a read, a write. Resting is RFAL's low-power mode
 * (RFAL_LP_MODE_PD: STOP, then oscillator, transmitter, receiver and field
 * detector off); waking is rfalLowPowerModeStop(), which re-enables the
 * oscillator and blocks on its stable interrupt. Nothing configured at
 * bring-up is lost across a rest - the registers keep their values through a
 * powered-down oscillator, and the next rfalNfcDiscover re-applies the
 * technology and analog settings on top - so a wake restores no state of its
 * own.
 *
 * The cost of the rest is paid on the next wake: the oscillator's start-up,
 * ST25R3916_TOUT_OSC_STABLE (700 us typical, 10 ms ceiling) plus the B
 * variant's RC calibration, added once to the first field-on of each request
 * and to nothing after it within the same field-up.
 *
 * @param want KNFC_POWER_AWAKE before a use, KNFC_POWER_RESTING after it.
 * @return KODE_OK when the reader reached @p want, KODE_ERROR when a wake or
 *         rest was refused (the believed state is left unchanged).
 */
static kode_status_t nfc_power_set(knfc_power_state_t want)
{
    switch ( knfc_power_action(s_power, want) )
    {
    case KNFC_POWER_WAKE:
        if ( rfalLowPowerModeStop() != RFAL_ERR_NONE )
        {
            KLOG_E(TAG, "the reader would not wake from rest");
            return KODE_ERROR;
        }
        s_power = KNFC_POWER_AWAKE;
        break;
    case KNFC_POWER_REST:
        if ( rfalLowPowerModeStart(RFAL_LP_MODE_PD) != RFAL_ERR_NONE )
        {
            KLOG_W(TAG, "the reader would not rest; leaving it awake");
            return KODE_ERROR;
        }
        s_power = KNFC_POWER_RESTING;
        break;
    case KNFC_POWER_HOLD:
    default:
        break;
    }
    return KODE_OK;
}

/**
 * @brief Open SPI2 with CS as a plain output and the reader as its only device.
 *
 * @return KODE_OK, or KODE_ERROR when the bus or the device refused.
 */
static kode_status_t spi_open(void)
{
    gpio_config_t cs = {
        .pin_bit_mask = 1ULL << KBSP_PIN_NFC_CS,
        .mode         = GPIO_MODE_OUTPUT,
        .intr_type    = GPIO_INTR_DISABLE,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    if ( gpio_config(&cs) != ESP_OK )
    {
        return KODE_ERROR;
    }
    (void)gpio_set_level((gpio_num_t)KBSP_PIN_NFC_CS, 1);

    spi_bus_config_t bus = {
        .mosi_io_num     = KBSP_PIN_SPI_MOSI,
        .miso_io_num     = KBSP_PIN_SPI_MISO,
        .sclk_io_num     = KBSP_PIN_SPI_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
    };
    /* No DMA: a transaction is then 64 bytes at most, and rfal_port_spi_txrx()
     * sends a longer access in pieces and says why. */
    esp_err_t err = spi_bus_initialize(KDRV_ST25_SPI_HOST, &bus,
                                       SPI_DMA_DISABLED);
    if ( (ESP_OK != err) && (ESP_ERR_INVALID_STATE != err) )
    {
        KLOG_E(TAG, "SPI2 would not initialise");
        return KODE_ERROR;
    }

    spi_device_interface_config_t dev = {
        .mode           = 1,        /* CPOL 0, CPHA 1: the datasheet's. */
        .clock_speed_hz = KDRV_ST25_SPI_HZ,
        .spics_io_num   = -1,       /* CS by hand; RFAL chains transfers. */
        .queue_size     = 1,
    };
    if ( spi_bus_add_device(KDRV_ST25_SPI_HOST, &dev, &s_spi) != ESP_OK )
    {
        KLOG_E(TAG, "the reader would not join SPI2");
        return KODE_ERROR;
    }
    return KODE_OK;
}

/**
 * @brief Hand back what serving the chip took: the interrupt is detached, the
 *        servicer deleted and the SPI scratch freed.
 *
 * The line first, so the ISR never gives to a task that is gone.
 */
static void servicer_release(void)
{
    khal_gpio_release(KBSP_PIN_NFC_IRQ);
    if ( NULL != s_irq_task )
    {
        vTaskDelete(s_irq_task);
        s_irq_task = NULL;
    }
}

kode_status_t kdrv_st25r3916_init(void)
{
    if ( s_present )
    {
        return KODE_OK;
    }

    s_spi_lock = xSemaphoreCreateRecursiveMutex();
    s_comm    = xSemaphoreCreateRecursiveMutex();
    s_worker  = xSemaphoreCreateRecursiveMutex();
    s_irq_sem = xSemaphoreCreateBinary();
    if ( (NULL == s_comm) || (NULL == s_worker) || (NULL == s_irq_sem)
        || (NULL == s_spi_lock) )
    {
        KLOG_E(TAG, "no memory for the reader's locks");
        return KODE_ERROR;
    }

    if ( spi_open() != KODE_OK )
    {
        return KODE_ERROR;
    }

    /* Pulled DOWN, not left floating: the reader drives this line push-pull
     * when it is fitted, and when it is not - a main PCB tested before its
     * antenna PCB - a floating input on a high-level interrupt fires without
     * end, and the servicer below eats CPU 0 whole before the console ever
     * runs. */
    if ( khal_gpio_input(KBSP_PIN_NFC_IRQ, KHAL_GPIO_PULL_DOWN) != KODE_OK )
    {
        return KODE_ERROR;
    }

    /* The servicer exists before the chip can raise a line. */
    if ( xTaskCreate(&irq_service, "nfc_irq", KDRV_ST25_IRQ_STACK, NULL,
                    KDRV_ST25_IRQ_PRIO, &s_irq_task) != pdPASS )
    {
        KLOG_E(TAG, "no task for the reader's interrupt");
        return KODE_ERROR;
    }

    /* A level, not an edge: the chip holds the line until its status is read, so none is lost. */
    if ( khal_gpio_irq_attach(KBSP_PIN_NFC_IRQ, KHAL_GPIO_IRQ_HIGH_LEVEL,
                             &irq_isr, NULL) != KODE_OK )
    {
        return KODE_ERROR;
    }
    khal_gpio_irq_enable(KBSP_PIN_NFC_IRQ);

    /* This is what reads IC_IDENTITY and refuses anything else. */
    ReturnCode rc = rfalNfcInitialize();
    if ( RFAL_ERR_NONE != rc )
    {
        (void)st25r3916ReadRegister(ST25R3916_REG_IC_IDENTITY, &s_identity);
        KLOG_W(TAG, "no reader: RFAL says %d, identity reads 0x%02X",
               (int)rc, (unsigned)s_identity);
        /* No reader, no line to service: a board without the chip runs like
         * one rather than around a task waiting on a pin nothing drives. */
        servicer_release();
        return KODE_ERROR;
    }

    (void)st25r3916ReadRegister(ST25R3916_REG_IC_IDENTITY, &s_identity);
    s_present = true;

    /* rfalNfcInitialize left the oscillator running, so that is where the
     * power state starts; drop it to rest at once. From here the chip sleeps
     * until a request wakes it, and identity reads (which do not need the
     * oscillator) keep working through the rest. */
    s_power = KNFC_POWER_AWAKE;
    (void)nfc_power_set(KNFC_POWER_RESTING);

    KLOG_D(TAG, "ST25R3916B up, identity 0x%02X, resting with the field down",
           (unsigned)s_identity);
    return KODE_OK;
}

bool kdrv_st25r3916_present(void)
{
    return s_present;
}

kode_status_t kdrv_st25r3916_park(void)
{
    spi_transaction_t t;
    uint8_t           identity = 0U;
    kode_status_t     rc       = KODE_ERROR;

    if ( spi_open() != KODE_OK )
    {
        return KODE_ERROR;
    }
    /* Who answers, from IC_IDENTITY, without RFAL: one read, CS by hand. */
    (void)memset(&t, 0, sizeof(t));
    /* cppcheck-suppress misra-c2012-10.1 ; SPI_TRANS_USE_* are ESP-IDF's own shifted flags, as spi_transaction_t takes them */
    t.flags      = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    t.length     = KDRV_ST25_PARK_READ_BITS;
    t.tx_data[0] = (uint8_t)(ST25_SPI_READ | ST25R3916_REG_IC_IDENTITY);
    rfal_port_spi_select();
    if ( spi_device_polling_transmit(s_spi, &t) == ESP_OK )
    {
        identity = t.rx_data[1];
    }
    rfal_port_spi_deselect();

    if ( knfc_ident_supported(identity) )
    {
        /* Set Default leaves the chip as a power-on does: power-down mode,
         * oscillator and field off. Its reset state is already its lowest;
         * this is what makes sure nothing since powered it up. */
        (void)memset(&t, 0, sizeof(t));
        /* cppcheck-suppress misra-c2012-10.1 ; SPI_TRANS_USE_* are ESP-IDF's own shifted flags, as spi_transaction_t takes them */
        t.flags      = SPI_TRANS_USE_TXDATA;
        t.length     = KDRV_ST25_PARK_CMD_BITS;
        t.tx_data[0] = (uint8_t)ST25R3916_CMD_SET_DEFAULT;
        rfal_port_spi_select();
        if ( spi_device_polling_transmit(s_spi, &t) == ESP_OK )
        {
            rc = KODE_OK;
        }
        rfal_port_spi_deselect();
    }
    /* SPI2 is left as it was opened: the bus is the reader's and nobody
     * else's, and taking the device off it would link the bus's acquire and
     * release into IRAM for one call at boot. */
    return rc;
}

kode_status_t kdrv_st25r3916_identity(uint8_t *out)
{
    if ( (NULL == out) || !s_present )
    {
        return KODE_DENIED;
    }
    *out = s_identity;
    return KODE_OK;
}

/* ==========================================================================
 * Discovery: armed here, advanced one step at a time
 * ========================================================================== */

kode_status_t kdrv_st25r3916_discover_start(void)
{
    /* A listen window measures with the transmitter off; a field raised under
     * it would be the Dot measuring itself. Its owner ends it first. An
     * emulation window is the same argument inverted - the chip is a target,
     * not a poller - so a field refuses under it too. */
    if ( !s_present || s_listening || s_emulating )
    {
        return KODE_DENIED;
    }

    /* Wake the chip before the field: every read, write and sweep reaches the
     * radio through a discovery that starts here, so this one wake covers the
     * whole request, and field_off() puts it back to rest at the end. */
    if ( nfc_power_set(KNFC_POWER_AWAKE) != KODE_OK )
    {
        return KODE_ERROR;
    }

    rfalNfcDeactivate(RFAL_NFC_DEACTIVATE_IDLE);
    s_dev = NULL;

    rfalNfcDefaultDiscParams(&s_disc);
    s_disc.compMode      = RFAL_COMPLIANCE_MODE_NFC;
    s_disc.techs2Find    = RFAL_NFC_POLL_TECH_A;
    s_disc.devLimit      = 1U;
    s_disc.totalDuration = 1000U;

    ReturnCode rc = rfalNfcDiscover(&s_disc);
    if ( RFAL_ERR_NONE != rc )
    {
        KLOG_W(TAG, "discovery refused: %d", (int)rc);
        return KODE_ERROR;
    }
    s_field = true;
    return KODE_OK;
}

void kdrv_st25r3916_worker(void)
{
    if ( !s_present )
    {
        return;
    }
    rfalNfcWorker();

    if ( NULL == s_dev )
    {
        if ( rfalNfcIsDevActivated(rfalNfcGetState()) )
        {
            if ( rfalNfcGetActiveDevice(&s_dev) == RFAL_ERR_NONE )
            {
                s_tags++;
            }
            else
            {
                s_dev = NULL;
            }
        }
    }
}

bool kdrv_st25r3916_activated(void)
{
    return NULL != s_dev;
}

bool kdrv_st25r3916_is_iso_dep(void)
{
    if ( NULL == s_dev )
    {
        return false;
    }
    return RFAL_NFC_INTERFACE_ISODEP == s_dev->rfInterface;
}

kode_status_t kdrv_st25r3916_active_tag(uint8_t *uid, uint8_t *uid_len,
                                        uint16_t *atqa, uint8_t *sak)
{
    if ( (NULL == uid) || (NULL == uid_len) || (NULL == atqa)
        || (NULL == sak) )
    {
        return KODE_DENIED;
    }
    if ( !s_present )
    {
        return KODE_DENIED;
    }
    if ( NULL == s_dev )
    {
        return KODE_ERROR;
    }

    uint8_t len = s_dev->nfcidLen;
    if ( KDRV_ST25R3916_UID_MAX < len )
    {
        len = KDRV_ST25R3916_UID_MAX;
    }
    (void)memset(uid, 0, KDRV_ST25R3916_UID_MAX);
    if ( (NULL != s_dev->nfcid) && (0U < len) )
    {
        (void)memcpy(uid, s_dev->nfcid, len);
    }
    *uid_len = len;
    *atqa    = (uint16_t)s_dev->dev.nfca.sensRes.anticollisionInfo
             | (uint16_t)((uint16_t)s_dev->dev.nfca.sensRes.platformInfo << 8);
    *sak     = s_dev->dev.nfca.selRes.sak;
    return KODE_OK;
}

/* ==========================================================================
 * The certification carrier (FCC 15.225), compiled only for the cert app
 * ========================================================================== */

#if CONFIG_KODEOS_NFC_CERT

/** @brief TX_DRIVER d_res = 0: the least RFO resistance, so the most drive. */
#define KDRV_ST25_DRES_MAX_DRIVE  0x00U
/** @brief The modulation task runs below the servicer, so its transceives can
 *         be preempted by the interrupt path they wait on. */
#define KDRV_ST25_CERT_MOD_PRIO   5U
/** @brief The task only loops one transceive; it needs no depth. */
#define KDRV_ST25_CERT_MOD_STACK  4096U
/** @brief How long one modulation frame waits for an answer that never comes. */
#define KDRV_ST25_CERT_MOD_FWT_MS 1U
/** @brief Steps of 2 ms field_off() waits for the modulation task to exit. */
#define KDRV_ST25_CERT_JOIN_STEPS 100U

/** @brief The bytes the modulated carrier retransmits at 106 kb/s NFC-A. The
 *         alternating pattern packs the ASK sidebands the occupied-bandwidth
 *         measurement needs onto the spectrum; nothing ever answers it. */
static const uint8_t s_cert_mod_frame[] = {
    0x55U, 0x55U, 0x55U, 0x55U, 0x55U, 0x55U, 0x55U, 0x55U
};

static volatile bool s_cert_mod_run;   /**< The modulation loop's on switch.   */
static TaskHandle_t  s_cert_mod_task;  /**< NULL when no loop is running.      */
static uint8_t       s_cert_dres_def;  /**< The d_res discovery uses, to restore. */
static bool          s_cert_drive;     /**< Cert pushed the drive to maximum.  */

/**
 * @brief Force the Tx driver to its maximum drive (minimum RFO resistance).
 *
 * A transceive re-applies the analog Tx configuration, so this is re-asserted
 * each loop rather than set once.
 */
static void cert_drive_max(void)
{
    (void)st25r3916ChangeRegisterBits(ST25R3916_REG_TX_DRIVER,
                                      ST25R3916_REG_TX_DRIVER_d_res_mask,
                                      KDRV_ST25_DRES_MAX_DRIVE);
}

/**
 * @brief Retransmit the fixed pattern until told to stop.
 *
 * Each pass is a full transceive whose reception times out with nothing on the
 * antenna, which leaves the RF layer idle for the next pass; the carrier stays
 * up at maximum drive between them. This is the fixed-pattern fallback the spec
 * allows where the driver refuses transparent/stream mode.
 *
 * @param arg Unused.
 */
static void cert_mod_task(void *arg)
{
    (void)arg;

    uint8_t  rx[32];
    uint16_t rcvd = 0U;

    while ( s_cert_mod_run )
    {
        cert_drive_max();
        (void)rfalTransceiveBlockingTxRx((uint8_t *)s_cert_mod_frame,
                                         (uint16_t)sizeof(s_cert_mod_frame),
                                         rx, (uint16_t)sizeof(rx), &rcvd,
                                         (uint32_t)RFAL_TXRX_FLAGS_DEFAULT,
                                         rfalConvMsTo1fc(
                                             KDRV_ST25_CERT_MOD_FWT_MS));
        /* RFAL's transceive spins without yielding, so the loop would busy the
         * core whole. Yield one tick between bursts: the carrier stays up at
         * maximum drive across the gap, and the ASK sidebands the measurement
         * wants are already on the spectrum from the burst that just went out. */
        khal_delay_ms(1U);
    }

    s_cert_mod_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Stop the modulation loop and give the drive strength back.
 *
 * Called from kdrv_st25r3916_field_off(). A no-op when no certification
 * carrier was raised, which is every ordinary field.
 */
static void cert_carrier_stop(void)
{
    if ( NULL != s_cert_mod_task )
    {
        s_cert_mod_run = false;
        uint32_t step = 0U;
        while ( (NULL != s_cert_mod_task) && (KDRV_ST25_CERT_JOIN_STEPS > step) )
        {
            khal_delay_ms(2U);
            step++;
        }
    }
    if ( s_cert_drive )
    {
        (void)st25r3916ChangeRegisterBits(ST25R3916_REG_TX_DRIVER,
                                          ST25R3916_REG_TX_DRIVER_d_res_mask,
                                          s_cert_dres_def);
        s_cert_drive = false;
    }
}

kode_status_t kdrv_st25r3916_cert_carrier(bool modulated)
{
    /* Refused before the field_off() below, which would close an open listen
     * or emulation window behind its owner's back: the owner ends it, not
     * this. */
    if ( !s_present || s_listening || s_emulating )
    {
        return KODE_DENIED;
    }

    /* Start from a clean field: a prior carrier's modulation task is joined
     * and the drive strength restored, so switching carrier<->modulated does
     * not leave a task or a half-applied register behind. */
    kdrv_st25r3916_field_off();

    if ( nfc_power_set(KNFC_POWER_AWAKE) != KODE_OK )
    {
        return KODE_ERROR;
    }

    /* Bring the NFC-A poller up, then raise the field with its guard time -
     * the same two steps a reselect uses. Both are needed before a modulated
     * frame can be sent, and harmless for a bare carrier. */
    if ( rfalNfcaPollerInitialize() != RFAL_ERR_NONE )
    {
        return KODE_ERROR;
    }
    if ( rfalFieldOnAndStartGT() != RFAL_ERR_NONE )
    {
        return KODE_ERROR;
    }

    /* Field-on has just applied the analog Tx configuration, so what the
     * driver reads now is the resistance discovery would use: remember it to
     * restore, then push to maximum drive. */
    uint8_t tx_driver = 0U;
    (void)st25r3916ReadRegister(ST25R3916_REG_TX_DRIVER, &tx_driver);
    s_cert_dres_def = tx_driver & ST25R3916_REG_TX_DRIVER_d_res_mask;
    s_cert_drive    = true;
    cert_drive_max();

    s_field = true;

    if ( modulated )
    {
        s_cert_mod_run = true;
        if ( xTaskCreate(&cert_mod_task, "nfc_cert", KDRV_ST25_CERT_MOD_STACK,
                        NULL, KDRV_ST25_CERT_MOD_PRIO, &s_cert_mod_task)
            != pdPASS )
        {
            KLOG_E(TAG, "no task for the certification modulation loop");
            s_cert_mod_run = false;
            kdrv_st25r3916_field_off();
            return KODE_ERROR;
        }
        KLOG_N(TAG, "certification carrier up: 13.56 MHz, max drive, ASK "
                    "modulated");
    }
    else
    {
        KLOG_N(TAG, "certification carrier up: 13.56 MHz, max drive, "
                    "unmodulated");
    }
    return KODE_OK;
}

#endif /* CONFIG_KODEOS_NFC_CERT */

/* ==========================================================================
 * Listening: the transmitter held off, the outside field measured
 * ========================================================================== */

/**
 * @brief The external field detector mode a listen window runs in: 10b,
 *        manual, with the peer detection threshold.
 *
 * DS13541 (ST25R3916B, Rev 11) gives en_fd_c<1:0> four values in the
 * Operation control register (Table 22, p. 74). 00 is off. 01 is the
 * collision avoidance threshold, which section 4.4.5 (p. 63) ties to the NFC
 * field ON commands a reader sends before raising its own field. 11 is
 * automatic: the chip swaps between the two thresholds itself, for NFCIP-1
 * active communication and passive target modes. 10 is the one left, and the
 * one section 4.2.9 (pp. 36-37) describes as detecting the field emitted by a
 * peer device, which is exactly the question a listen window asks. It is
 * manual, so nothing switches thresholds or answers the field behind the
 * driver's back. It is also the more sensitive of the two as RFAL's analog
 * configuration sets them at init: 105 mVpp to trip and 75 mVpp to release,
 * against 205 and 150 mVpp for collision avoidance.
 */
#define KDRV_ST25_LISTEN_FD  ST25R3916_REG_OP_CONTROL_en_fd_manual_efd_pdt

/**
 * @brief The Operation control bits a window owns: en_fd, set to
 *        KDRV_ST25_LISTEN_FD, and rx_chn, cleared so both receive channels
 *        are on (Table 22, p. 74).
 */
#define KDRV_ST25_LISTEN_OP_BITS \
    ((uint8_t)(ST25R3916_REG_OP_CONTROL_en_fd_mask \
               | ST25R3916_REG_OP_CONTROL_rx_chn))

/**
 * @brief The Receiver configuration register 2 bits a window owns, both
 *        cleared: demod_mode for AM/PM rather than I/Q demodulation, amd_sel
 *        for the peak detector rather than the mixer (Table 41, p. 87). An
 *        NFC-A read leaves I/Q through the mixer behind (RFAL's analog table
 *        writes 0xED), and the same field would read differently through it.
 */
#define KDRV_ST25_LISTEN_CONF2_BITS \
    ((uint8_t)(ST25R3916_REG_RX_CONF2_demod_mode \
               | ST25R3916_REG_RX_CONF2_amd_sel))

/**
 * @brief The Auxiliary modulation setting bit a window owns: rgs_am, cleared,
 *        as the datasheet asks for a measurement from tx_en=0 (Table 79,
 *        p. 113). RFAL's initialisation sets it for active wave shaping.
 */
#define KDRV_ST25_LISTEN_AUX_BITS  ((uint8_t)ST25R3916_REG_AUX_MOD_rgs_am)

/**
 * @brief How long one amplitude conversion may take to signal its end, in
 *        milliseconds.
 *
 * The conversion is 25 us at most (section 4.4.8, p. 65). This is the ceiling
 * RFAL's own measurement waits and keeps private. A burst stops at the first
 * conversion that misses it, so a front end that stopped answering costs one
 * wait, not one per measurement.
 */
#define KDRV_ST25_MEASURE_TOUT_MS  10U

static knfc_field_acc_t s_listen;       /**< The open window's running stats. */
static uint8_t          s_listen_op;    /**< OP_CONTROL bits it found.        */
static uint8_t          s_listen_conf2; /**< RX_CONF2 bits it found.          */
static uint8_t          s_listen_aux;   /**< AUX_MOD bits it found.           */
/* What the open window's steps cost. A step busy-waits on its conversions
 * inside AO_Nfc's step, so its duration is how long every lower priority on
 * that core waits for it once per tick. It is wall time: a higher priority
 * that preempts the step, the reader's own interrupt servicer included, is
 * in it. The window's longest is logged when it closes. */
static uint16_t         s_listen_bursts;  /**< Bursts folded, to UINT16_MAX. */
static uint32_t         s_listen_step_us; /**< Longest step so far, in us.   */

/**
 * @brief Write the bits a listen window owns and leave every other bit of the
 *        three registers alone.
 *
 * All three writes are attempted whatever the earlier ones answer, so a
 * restore puts back everything it can.
 *
 * @param op    OP_CONTROL's en_fd and rx_chn bits.
 * @param conf2 RX_CONF2's demod_mode and amd_sel bits.
 * @param aux   AUX_MOD's rgs_am bit.
 * @return KODE_OK, or KODE_ERROR when the reader refused any of the writes.
 */
static kode_status_t listen_regs_set(uint8_t op, uint8_t conf2, uint8_t aux)
{
    kode_status_t st = KODE_OK;

    if ( st25r3916ChangeRegisterBits(ST25R3916_REG_OP_CONTROL,
                                    KDRV_ST25_LISTEN_OP_BITS, op)
        != RFAL_ERR_NONE )
    {
        st = KODE_ERROR;
    }
    if ( st25r3916ChangeRegisterBits(ST25R3916_REG_RX_CONF2,
                                    KDRV_ST25_LISTEN_CONF2_BITS, conf2)
        != RFAL_ERR_NONE )
    {
        st = KODE_ERROR;
    }
    if ( st25r3916ChangeRegisterBits(ST25R3916_REG_AUX_MOD,
                                    KDRV_ST25_LISTEN_AUX_BITS, aux)
        != RFAL_ERR_NONE )
    {
        st = KODE_ERROR;
    }
    return st;
}

/**
 * @brief End the open window: its bits back as the window found them, then
 *        the reader at rest.
 *
 * The restore comes first, while the oscillator still runs, so that a rest
 * the chip refuses leaves it awake with the detector and the receive chain
 * RFAL expects. The rest is what field_off() does at the end of every use,
 * and like there a refusal is logged by nfc_power_set() and not returned: the
 * window is over either way.
 *
 * @return KODE_OK, or KODE_ERROR when a register could not be put back.
 */
static kode_status_t listen_close(void)
{
    kode_status_t const st =
        listen_regs_set(s_listen_op, s_listen_conf2, s_listen_aux);

    if ( KODE_OK != st )
    {
        KLOG_W(TAG, "the listen registers could not all be put back");
    }
    s_listening = false;
    (void)nfc_power_set(KNFC_POWER_RESTING);
    return st;
}

kode_status_t kdrv_st25r3916_listen_begin(void)
{
    uint8_t       op    = 0U;
    uint8_t       conf2 = 0U;
    uint8_t       aux   = 0U;
    kode_status_t st    = KODE_OK;

    if ( !s_present || s_field || s_listening || s_emulating )
    {
        return KODE_DENIED;
    }

    if ( nfc_power_set(KNFC_POWER_AWAKE) != KODE_OK )
    {
        return KODE_ERROR;
    }

    /* With s_field false the transmitter is already off: field_off() took it
     * down and a rest keeps it down. Turning it off once more is the one call
     * that makes that true whatever the chip was left in, and it keeps RFAL's
     * own idea of the field in step. The register is then read back, and a
     * transmitter that is still enabled refuses the window: every byte it
     * measured would be the Dot's own drive. Nothing here raises it again. */
    if ( rfalFieldOff() != RFAL_ERR_NONE )
    {
        st = KODE_ERROR;
    }
    else if ( st25r3916ReadRegister(ST25R3916_REG_OP_CONTROL, &op)
             != RFAL_ERR_NONE )
    {
        st = KODE_ERROR;
    }
    else if ( st25r3916ReadRegister(ST25R3916_REG_RX_CONF2, &conf2)
             != RFAL_ERR_NONE )
    {
        st = KODE_ERROR;
    }
    else if ( st25r3916ReadRegister(ST25R3916_REG_AUX_MOD, &aux)
             != RFAL_ERR_NONE )
    {
        st = KODE_ERROR;
    }
    else if ( (op & ST25R3916_REG_OP_CONTROL_tx_en) != 0U )
    {
        KLOG_W(TAG, "the transmitter is still enabled after field-off; "
                    "no listen window");
        st = KODE_DENIED;
    }
    else
    {
        /* What the window found is what goes back when it ends: the detector
         * in the mode the awake reader keeps (RFAL's wake sets it to
         * automatic), and the receive chain as the last technology left it. */
        s_listen_op    = (uint8_t)(op & KDRV_ST25_LISTEN_OP_BITS);
        s_listen_conf2 = (uint8_t)(conf2 & KDRV_ST25_LISTEN_CONF2_BITS);
        s_listen_aux   = (uint8_t)(aux & KDRV_ST25_LISTEN_AUX_BITS);

        /* The detector to manual peer detection with rx_chn clear; the
         * RX_CONF2 and AUX_MOD bits all clear. */
        st = listen_regs_set((uint8_t)KDRV_ST25_LISTEN_FD, 0U, 0U);
        if ( KODE_OK != st )
        {
            (void)listen_regs_set(s_listen_op, s_listen_conf2, s_listen_aux);
        }
    }

    if ( KODE_OK != st )
    {
        (void)nfc_power_set(KNFC_POWER_RESTING);
        return st;
    }

    knfc_field_reset(&s_listen);
    s_listen_bursts  = 0U;
    s_listen_step_us = 0U;
    s_listening      = true;
    return KODE_OK;
}

kode_status_t kdrv_st25r3916_listen_step(void)
{
    uint8_t amp[KNFC_LISTEN_BURST] = { 0U };
    bool    ok = true;

    if ( !s_listening )
    {
        return KODE_DENIED;
    }

    /* The step is timed whole, the detector read and every transfer
     * included, because the whole of it is what AO_Nfc's step waits on. */
    uint64_t const t0 = khal_time_us();

    /* The detector is read before any conversion of this burst. */
    bool const ext = st25r3916IsExtFieldOn();

    /* The measurement is issued by hand rather than through
     * st25r3916MeasureAmplitude(), whose wrapper drops the interrupt wait's
     * timeout and the register read's result and answers RFAL_ERR_NONE
     * whatever happened: a conversion that never ends would fold a stale A/D
     * byte in as a reading. The end-of-command interrupt is cleared and
     * enabled once for the burst, waited on after each command, and disabled
     * when the burst is over. */
    (void)st25r3916GetInterrupt(ST25R3916_IRQ_MASK_DCT);
    st25r3916EnableInterrupts(ST25R3916_IRQ_MASK_DCT);
    for ( size_t i = 0U; ((size_t)KNFC_LISTEN_BURST > i) && ok; i++ )
    {
        if ( st25r3916ExecuteCommand(ST25R3916_CMD_MEASURE_AMPLITUDE)
            != RFAL_ERR_NONE )
        {
            ok = false;
        }
        else if ( st25r3916WaitForInterruptsTimed(
                     ST25R3916_IRQ_MASK_DCT,
                     (uint16_t)KDRV_ST25_MEASURE_TOUT_MS) == 0U )
        {
            ok = false;
        }
        else if ( st25r3916ReadRegister(ST25R3916_REG_AD_RESULT, &amp[i])
                 != RFAL_ERR_NONE )
        {
            ok = false;
        }
        else
        {
            /* The byte is in; the next conversion may start. */
        }
    }
    st25r3916DisableInterrupts(ST25R3916_IRQ_MASK_DCT);

    /* A step is bounded by KNFC_LISTEN_BURST conversion timeouts, well
     * inside 32 bits of microseconds. */
    uint32_t const us = (uint32_t)(khal_time_us() - t0);

    if ( !ok )
    {
        KLOG_W(TAG, "an amplitude conversion did not complete in a %u us "
                    "step; the listen window ends", (unsigned)us);
        (void)listen_close();
        return KODE_ERROR;
    }

    knfc_field_fold(&s_listen, amp, (size_t)KNFC_LISTEN_BURST, ext);
    if ( UINT16_MAX > s_listen_bursts )
    {
        s_listen_bursts = (uint16_t)(s_listen_bursts + 1U);
    }
    if ( us > s_listen_step_us )
    {
        s_listen_step_us = us;
    }
    return KODE_OK;
}

kode_status_t kdrv_st25r3916_listen_end(knfc_field_t *out)
{
    if ( !s_listening )
    {
        return KODE_DENIED;
    }
    knfc_field_close(&s_listen, out);
    KLOG_D(TAG, "listen: %u bursts, step max %u us",
           (unsigned)s_listen_bursts, (unsigned)s_listen_step_us);
    return listen_close();
}

void kdrv_st25r3916_field_off(void)
{
    if ( !s_present )
    {
        return;
    }

    /* Off closes an open listen window before anything else, so a window
     * its owner never ended cannot keep the detector and the receive chain
     * in its mode, or refuse every window after it. Its stats go nowhere. */
    if ( s_listening )
    {
        (void)listen_close();
    }

#if CONFIG_KODEOS_NFC_CERT
    /* A certification carrier is taken down here like any other field, but it
     * leaves two things behind that an ordinary window does not: a modulation
     * task that has to be stopped and joined before the RF layer is touched,
     * and a maximum-drive Tx setting that has to be handed back to the value
     * discovery uses. */
    cert_carrier_stop();
#endif

    rfalNfcDeactivate(RFAL_NFC_DEACTIVATE_IDLE);
    s_dev   = NULL;
    s_field = false;

    /* The request is over: drop the oscillator too, back to the resting state
     * the reader holds between uses. Harmless when already resting. */
    (void)nfc_power_set(KNFC_POWER_RESTING);
}

/* ==========================================================================
 * Exchanges
 * ========================================================================== */

kode_status_t kdrv_st25r3916_t2t_read(uint8_t block, uint8_t *buf,
                                      uint16_t cap, uint16_t *got)
{
    if ( (NULL == buf) || (KDRV_ST25_T2T_READ_LEN > cap) )
    {
        return KODE_DENIED;
    }
    if ( !s_present || (NULL == s_dev) )
    {
        return KODE_DENIED;
    }

    uint16_t   rcvd = 0U;
    ReturnCode rc   = rfalT2TPollerRead(block, buf, cap, &rcvd);
    if ( NULL != got )
    {
        *got = rcvd;
    }
    if ( RFAL_ERR_TIMEOUT == rc )
    {
        s_errors++;
        return KODE_TIMEOUT;
    }
    if ( RFAL_ERR_NONE != rc )
    {
        s_errors++;
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t kdrv_st25r3916_t2t_write(uint8_t block, const uint8_t *data)
{
    if ( NULL == data )
    {
        return KODE_DENIED;
    }
    if ( !s_present || (NULL == s_dev) )
    {
        return KODE_DENIED;
    }

    ReturnCode rc = rfalT2TPollerWrite(block, data);
    if ( RFAL_ERR_TIMEOUT == rc )
    {
        s_errors++;
        return KODE_TIMEOUT;
    }
    if ( RFAL_ERR_NONE != rc )
    {
        s_errors++;
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t kdrv_st25r3916_apdu(const uint8_t *tx, uint16_t tx_len,
                                  uint8_t *rx, uint16_t rx_cap,
                                  uint16_t *rx_len)
{
    if ( (NULL == tx) || (NULL == rx) || (0U == tx_len)
        || (KDRV_ST25R3916_APDU_MAX < tx_len) || (0U == rx_cap) )
    {
        return KODE_DENIED;
    }
    if ( !s_present || (NULL == s_dev) )
    {
        return KODE_DENIED;
    }
    if ( !kdrv_st25r3916_is_iso_dep() )
    {
        return KODE_DENIED;
    }

    uint8_t  *rsp     = NULL;
    uint16_t *rsp_len = NULL;
    ReturnCode rc = rfalNfcDataExchangeStart((uint8_t *)tx, tx_len, &rsp,
                                             &rsp_len, RFAL_FWT_NONE);
    if ( RFAL_ERR_NONE != rc )
    {
        s_errors++;
        return KODE_ERROR;
    }

    do
    {
        rfalNfcWorker();
        rc = rfalNfcDataExchangeGetStatus();
    } while ( RFAL_ERR_BUSY == rc );

    if ( RFAL_ERR_TIMEOUT == rc )
    {
        s_errors++;
        return KODE_TIMEOUT;
    }
    if ( RFAL_ERR_NONE != rc )
    {
        s_errors++;
        return KODE_ERROR;
    }

    uint16_t len = 0U;
    if ( NULL != rsp_len )
    {
        len = *rsp_len;
    }
    if ( len > rx_cap )
    {
        len = rx_cap;
    }
    if ( (NULL != rsp) && (0U < len) )
    {
        (void)memcpy(rx, rsp, len);
    }
    if ( NULL != rx_len )
    {
        *rx_len = len;
    }
    return KODE_OK;
}

kode_status_t kdrv_st25r3916_transceive_bits(const uint8_t *tx,
                                             uint16_t tx_bits, uint8_t *rx,
                                             uint16_t rx_cap,
                                             uint16_t *rx_bits,
                                             uint16_t fwt_ms)
{
    if ( (NULL == tx) || (NULL == rx) || (0U == tx_bits) || (0U == rx_cap) )
    {
        return KODE_DENIED;
    }
    if ( !s_present || (NULL == s_dev) )
    {
        return KODE_DENIED;
    }

    uint16_t rcvd = 0U;

    /* Every helper the RF layer normally provides is turned off here:
     * parity is in the buffer already (encrypted, so nothing may recompute
     * it) and stays in the answer, and the CRC belongs to the caller. */
    rfalTransceiveContext ctx;
    ctx.txBuf     = (uint8_t *)tx;
    ctx.txBufLen  = tx_bits;
    ctx.rxBuf     = rx;
    ctx.rxBufLen  = (uint16_t)(rx_cap * 8U);
    ctx.rxRcvdLen = &rcvd;
    ctx.flags     = (uint32_t)RFAL_TXRX_FLAGS_CRC_TX_MANUAL
                  | (uint32_t)RFAL_TXRX_FLAGS_CRC_RX_KEEP
                  | (uint32_t)RFAL_TXRX_FLAGS_CRC_RX_MANUAL
                  | (uint32_t)RFAL_TXRX_FLAGS_NFCIP1_OFF
                  | (uint32_t)RFAL_TXRX_FLAGS_AGC_ON
                  | (uint32_t)RFAL_TXRX_FLAGS_PAR_RX_KEEP
                  | (uint32_t)RFAL_TXRX_FLAGS_PAR_TX_NONE
                  | (uint32_t)RFAL_TXRX_FLAGS_NFCV_FLAG_AUTO;
    ctx.fwt       = rfalConvMsTo1fc(fwt_ms);

    ReturnCode rc = rfalStartTransceive(&ctx);
    if ( RFAL_ERR_NONE != rc )
    {
        s_errors++;
        return KODE_ERROR;
    }

    do
    {
        rfalWorker();
        rc = rfalGetTransceiveStatus();
    } while ( RFAL_ERR_BUSY == rc );

    if ( NULL != rx_bits )
    {
        *rx_bits = rcvd;
    }

    /* A card that will not authenticate answers nothing at all, so a
     * timeout here is an ordinary outcome and not a fault. */
    if ( (RFAL_ERR_TIMEOUT == rc) || (RFAL_ERR_NOMSG == rc) )
    {
        return KODE_TIMEOUT;
    }
    /* CRC and parity errors are expected by construction: this call asked
     * for both to be left alone, so the layer's opinion of them is noise. */
    if ( (RFAL_ERR_NONE != rc) && (RFAL_ERR_CRC != rc)
        && (RFAL_ERR_PAR != rc) && (RFAL_ERR_INCOMPLETE_BYTE != rc) )
    {
        s_errors++;
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t kdrv_st25r3916_reselect(void)
{
    if ( !s_present )
    {
        return KODE_DENIED;
    }

    /* A full field cycle, then a fresh detect and select. This is the one
     * recovery that works from every state a Mifare card can be in - HALTed
     * after a failed authentication, ACTIVE after a successful one,
     * authenticated to a sector you now want to leave. A plaintext AUTH is
     * only valid on a freshly selected card, and reading a second sector
     * means authenticating again, so this runs before every authentication
     * rather than only after a failure. Powering the field down resets the
     * card entirely, which is what makes it state-independent - and it is
     * exactly what the validation firmware does between sectors.
     */
    (void)rfalFieldOff();
    khal_delay_ms(2U);

    if ( rfalNfcaPollerInitialize() != RFAL_ERR_NONE )
    {
        return KODE_ERROR;
    }
    if ( rfalFieldOnAndStartGT() != RFAL_ERR_NONE )
    {
        return KODE_ERROR;
    }

    rfalNfcaSensRes sens;
    if ( rfalNfcaPollerCheckPresence(RFAL_14443A_SHORTFRAME_CMD_REQA, &sens)
        != RFAL_ERR_NONE )
    {
        return KODE_TIMEOUT;
    }

    rfalNfcaSelRes sel;
    uint8_t        uid[RFAL_NFCA_CASCADE_3_UID_LEN];
    uint8_t        uid_len = 0U;
    bool           coll    = false;
    if ( rfalNfcaPollerSingleCollisionResolution(1U, &coll, &sel, uid, &uid_len)
        != RFAL_ERR_NONE )
    {
        return KODE_TIMEOUT;
    }

    /* s_dev still points at the device the discovery layer activated; the
     * raw transceive only needs it to be non-NULL, and a re-select of the
     * same card has not changed its UID or SAK. */
    return KODE_OK;
}

/* ==========================================================================
 * Emulation: a passive NFC-A target, pumped by a worker task
 * ==========================================================================
 *
 * The chip answers REQA / anticollision / SELECT in hardware from the
 * passive-target memory rfalListenStart() loads; the worker below pumps RFAL's
 * listen worker and answers the Type-2 READs that follow. It is a separate
 * task, not AO_Nfc's tick, for the reason the reader's header gives inverted: a
 * reader outside sets the tempo now, and a READ wants an answer inside about
 * 87 microseconds. It runs below the interrupt servicer, as the certification
 * task does, so the servicer preempts it to feed the interrupts its
 * transceives wait on.
 *
 * Stopping is cooperative: kdrv_st25r3916_emulate_stop() clears s_emu_run and
 * returns at once, so an Active Object may call it, and the worker tears the
 * window down in its own context - where nothing else is touching RFAL - and
 * clears s_emulating. It checks the flag between sessions and between the
 * frames of one, so a stop takes effect as soon as the reader in front of it
 * pauses; a reader held on without pause holds the session, exactly as it
 * holds a real card.
 */

/** @brief The worker pumps RFAL's listen worker this often. */
#define KDRV_ST25_EMU_PERIOD_MS  1U
/** @brief Below the interrupt servicer, like the certification task: the
 *         servicer must preempt it to feed the interrupts it waits on. */
#define KDRV_ST25_EMU_PRIO       5U
/** @brief The worker answers short frames and needs no depth. */
#define KDRV_ST25_EMU_STACK      4096U
/** @brief PT_MOD load modulation: ptm_res 0, the most depth, for the small
 *         ANT2 antenna. RFAL's card-mode default (0x2E) is weaker. */
#define KDRV_ST25_EMU_PT_MOD     0x0EU
/** @brief The listen rx buffer; NFC-A frames here are short. */
#define KDRV_ST25_EMU_RX         256U
/** @brief Type-2 READ command. */
#define KDRV_ST25_T2T_CMD_READ   0x30U
/** @brief Type-2 FAST_READ command. */
#define KDRV_ST25_T2T_CMD_FREAD  0x3AU
/** @brief Type-2 HALT command, first byte. */
#define KDRV_ST25_T2T_CMD_HALT   0x50U

static volatile bool     s_emu_run;    /**< The worker loop's switch.         */
static TaskHandle_t      s_emu_task;
static knfc_emu_image_t  s_emu_img;    /**< Built at start; served to READs.  */
static rfalLmConfPA      s_emu_conf;   /**< The presented identity.           */
static uint8_t           s_emu_rx[KDRV_ST25_EMU_RX];
static uint16_t          s_emu_rx_len; /**< RFAL fills this in BITS.          */
static bool              s_emu_field_prev;
static portMUX_TYPE      s_emu_mux = portMUX_INITIALIZER_UNLOCKED;
static knfc_emu_stat_t   s_emu_stat;   /**< The window's stats; guarded.      */

/**
 * @brief Force internal load modulation and the driver's modulation depth.
 *
 * Re-asserted on every field-detect because the listen worker may put the
 * analog-config default back when a reader's field appears.
 */
static void emu_apply_lma(void)
{
    (void)st25r3916ChangeRegisterBits(ST25R3916_REG_AUX_MOD,
        (uint8_t)(ST25R3916_REG_AUX_MOD_lm_ext | ST25R3916_REG_AUX_MOD_lm_dri),
        ST25R3916_REG_AUX_MOD_lm_dri);
    (void)st25r3916WriteRegister(ST25R3916_REG_PT_MOD, KDRV_ST25_EMU_PT_MOD);
}

/** @brief Arm NFC-A listen with the presented identity. */
static ReturnCode emu_listen_arm(void)
{
    ReturnCode rc = rfalListenStart(RFAL_LM_MASK_NFCA, &s_emu_conf, NULL, NULL,
                                    s_emu_rx,
                                    (uint16_t)rfalConvBytesToBits(
                                        sizeof(s_emu_rx)),
                                    &s_emu_rx_len);
    s_emu_field_prev = false;
    emu_apply_lma();
    return rc;
}

/** @brief Re-arm listen after a transceive ended or an unsupported command. */
static void emu_relisten(void)
{
    (void)rfalListenStop();
    (void)emu_listen_arm();
}

/**
 * @brief Answer the reader after it selected us, until it leaves or halts.
 *
 * The first command is already in @c s_emu_rx (@c s_emu_rx_len in bits). Only
 * READ, FAST_READ and HALT are served; anything else drops back to listen,
 * which is enough for a phone reading a Type-2 tag. FWT_NONE waits for the next
 * command indefinitely so the reader's presence-check gap does not tear the
 * session down; the worker's stop flag is checked between frames so a stop
 * still lands when the reader pauses.
 */
static void emu_service(void)
{
    uint16_t n = (uint16_t)rfalConvBitsToBytes(s_emu_rx_len);

    taskENTER_CRITICAL(&s_emu_mux);
    s_emu_stat.sessions += 1U;
    taskEXIT_CRITICAL(&s_emu_mux);

    for ( ;; )
    {
        if ( !s_emu_run )
        {
            return;
        }
        if ( 0U == n )
        {
            emu_relisten();
            return;
        }

        uint8_t  resp[KNFC_EMU_T2T_BYTES];
        uint16_t rlen = 0U;
        uint8_t  cmd  = s_emu_rx[0];

        if ( KDRV_ST25_T2T_CMD_READ == cmd )
        {
            if ( 2U > n )
            {
                emu_relisten();
                return;
            }
            taskENTER_CRITICAL(&s_emu_mux);
            s_emu_stat.reads += 1U;
            taskEXIT_CRITICAL(&s_emu_mux);
            for ( uint8_t i = 0U; 4U > i; i++ )
            {
                uint16_t pg = (uint16_t)(((uint16_t)s_emu_rx[1] + i)
                                         % KNFC_EMU_T2T_PAGES);
                (void)memcpy(&resp[i * 4U], &s_emu_img.t2t[pg * 4U], 4U);
            }
            rlen = 16U;
        }
        else if ( KDRV_ST25_T2T_CMD_FREAD == cmd )
        {
            if ( 3U > n )
            {
                emu_relisten();
                return;
            }
            uint8_t st = s_emu_rx[1];
            uint8_t en = s_emu_rx[2];
            if ( (uint8_t)KNFC_EMU_T2T_PAGES <= en )
            {
                en = (uint8_t)(KNFC_EMU_T2T_PAGES - 1U);
            }
            if ( en < st )
            {
                emu_relisten();
                return;
            }
            taskENTER_CRITICAL(&s_emu_mux);
            s_emu_stat.reads += 1U;
            taskEXIT_CRITICAL(&s_emu_mux);
            for ( uint8_t p = st; p <= en; p++ )
            {
                (void)memcpy(&resp[(p - st) * 4U], &s_emu_img.t2t[p * 4U], 4U);
            }
            rlen = (uint16_t)(((uint16_t)(en - st) + 1U) * 4U);
        }
        else if ( KDRV_ST25_T2T_CMD_HALT == cmd )
        {
            (void)rfalListenSleepStart(RFAL_LM_STATE_SLEEP_A, s_emu_rx,
                     (uint16_t)rfalConvBytesToBits(sizeof(s_emu_rx)),
                     &s_emu_rx_len);
            return;
        }
        else
        {
            emu_relisten();
            return;
        }

        uint16_t   got = 0U;
        ReturnCode rc  = rfalTransceiveBlockingTxRx(resp, rlen, s_emu_rx,
                             (uint16_t)sizeof(s_emu_rx), &got,
                             (uint32_t)RFAL_TXRX_FLAGS_DEFAULT, RFAL_FWT_NONE);
        if ( RFAL_ERR_NONE != rc )
        {
            emu_relisten();   /* the reader's field went away */
            return;
        }
        n = got;
    }
}

/**
 * @brief The emulation worker: pump the listen state machine, serve READs.
 *
 * @param arg Unused.
 */
static void emu_task(void *arg)
{
    (void)arg;

    while ( s_emu_run )
    {
        rfalWorker();

        bool        data_flag = false;
        rfalBitRate br;
        rfalLmState st = rfalListenGetState(&data_flag, &br);
        (void)br;

        bool field    = false;
        bool selected = false;
        switch ( st )
        {
        case RFAL_LM_STATE_ACTIVE_A:
        case RFAL_LM_STATE_ACTIVE_Ax:
            field    = true;
            selected = true;
            break;
        case RFAL_LM_STATE_IDLE:
        case RFAL_LM_STATE_READY_A:
        case RFAL_LM_STATE_READY_Ax:
            field = true;
            break;
        default:
            break;
        }

        if ( field && !s_emu_field_prev )
        {
            emu_apply_lma();
        }
        s_emu_field_prev = field;

        /* The levels are published before emu_service() blocks on the session,
         * so a reader's select is seen before the READs it makes; the READ
         * count emu_service() raises during the session updates under the same
         * lock. */
        taskENTER_CRITICAL(&s_emu_mux);
        s_emu_stat.field    = field;
        s_emu_stat.selected = selected;
        taskEXIT_CRITICAL(&s_emu_mux);

        if ( selected && data_flag )
        {
            emu_service();
        }
        else if ( RFAL_LM_STATE_NOT_INIT == st )
        {
            /* A session that ended without landing back in POWER_OFF - a
             * transceive the reader abandoned - leaves the listen worker
             * uninitialised. Re-arm it so the next reader is answered. */
            emu_relisten();
        }

        khal_delay_ms(KDRV_ST25_EMU_PERIOD_MS);
    }

    /* Teardown in the worker's own context, so nothing else touches RFAL
     * while it stops. The counts are left standing - only the levels drop and
     * the window closes - so a reader of the snapshot still learns the final
     * READ total; the next start clears them. */
    (void)rfalListenStop();
    (void)nfc_power_set(KNFC_POWER_RESTING);
    taskENTER_CRITICAL(&s_emu_mux);
    s_emu_stat.active   = false;
    s_emu_stat.field    = false;
    s_emu_stat.selected = false;
    taskEXIT_CRITICAL(&s_emu_mux);
    s_emulating = false;
    s_emu_task  = NULL;
    vTaskDelete(NULL);
}

kode_status_t kdrv_st25r3916_emulate_start(const knfc_emu_card_t *card)
{
    if ( NULL == card )
    {
        return KODE_DENIED;
    }
    if ( !s_present || s_field || s_listening || s_emulating )
    {
        return KODE_DENIED;
    }

    knfc_emu_image_t img;
    kode_status_t    brc = knfc_emu_image_build(card, &img);
    if ( KODE_OK != brc )
    {
        return brc;
    }

    /* Fully re-initialise the RF layer before listening. The reader rests with
     * its oscillator and field detector powered down between uses, and a bare
     * low-power wake does not recalibrate the front end, so the listen-mode
     * field detector never trips on an external field. A full init restores it
     * the way the reader path's discovery does; the oscillator is left running,
     * so the power state is awake from here. */
    if ( rfalNfcInitialize() != RFAL_ERR_NONE )
    {
        return KODE_ERROR;
    }
    s_power = KNFC_POWER_AWAKE;

    /* A prior poller path leaves the chip in reader mode with the field on; a
     * passive target must not emit one, so stop any listen and drop the
     * field before arming. */
    (void)rfalListenStop();
    (void)rfalFieldOff();

    s_emu_img = img;
    (void)memset(&s_emu_conf, 0, sizeof(s_emu_conf));
    if ( 4U == img.nfcid_len )
    {
        s_emu_conf.nfcidLen = RFAL_LM_NFCID_LEN_04;
    }
    else if ( 7U == img.nfcid_len )
    {
        s_emu_conf.nfcidLen = RFAL_LM_NFCID_LEN_07;
    }
    else
    {
        s_emu_conf.nfcidLen = RFAL_LM_NFCID_LEN_10;
    }
    (void)memcpy(s_emu_conf.nfcid, img.nfcid, img.nfcid_len);
    s_emu_conf.SENS_RES[0] = img.sens_res[0];
    s_emu_conf.SENS_RES[1] = img.sens_res[1];
    s_emu_conf.SEL_RES     = img.sak;

    taskENTER_CRITICAL(&s_emu_mux);
    (void)memset(&s_emu_stat, 0, sizeof(s_emu_stat));
    s_emu_stat.active = true;
    taskEXIT_CRITICAL(&s_emu_mux);

    if ( emu_listen_arm() != RFAL_ERR_NONE )
    {
        (void)rfalListenStop();
        (void)nfc_power_set(KNFC_POWER_RESTING);
        taskENTER_CRITICAL(&s_emu_mux);
        (void)memset(&s_emu_stat, 0, sizeof(s_emu_stat));
        taskEXIT_CRITICAL(&s_emu_mux);
        KLOG_E(TAG, "listen mode would not start");
        return KODE_ERROR;
    }

    s_emulating = true;
    s_emu_run   = true;
    if ( xTaskCreate(&emu_task, "nfc_emu", KDRV_ST25_EMU_STACK, NULL,
                    KDRV_ST25_EMU_PRIO, &s_emu_task) != pdPASS )
    {
        s_emu_run = false;
        (void)rfalListenStop();
        (void)nfc_power_set(KNFC_POWER_RESTING);
        s_emulating = false;
        taskENTER_CRITICAL(&s_emu_mux);
        (void)memset(&s_emu_stat, 0, sizeof(s_emu_stat));
        taskEXIT_CRITICAL(&s_emu_mux);
        KLOG_E(TAG, "no task for the emulation worker");
        return KODE_ERROR;
    }

    KLOG_N(TAG, "emulating: nfcid %u B, ATQA 0x%02X%02X, SAK 0x%02X",
           (unsigned)img.nfcid_len, (unsigned)img.sens_res[1],
           (unsigned)img.sens_res[0], (unsigned)img.sak);
    return KODE_OK;
}

kode_status_t kdrv_st25r3916_emulate_stop(void)
{
    if ( !s_present || !s_emulating )
    {
        return KODE_DENIED;
    }
    /* The worker sees this, ends its session, tears the window down and
     * clears s_emulating. Non-blocking, so an Active Object may call it. */
    s_emu_run = false;
    return KODE_OK;
}

bool kdrv_st25r3916_emulate_active(void)
{
    return s_emulating;
}

void kdrv_st25r3916_emulate_snapshot(knfc_emu_stat_t *out)
{
    if ( NULL == out )
    {
        return;
    }
    taskENTER_CRITICAL(&s_emu_mux);
    *out = s_emu_stat;
    taskEXIT_CRITICAL(&s_emu_mux);
}

/* ==========================================================================
 * The field table
 * ========================================================================== */

/** @brief One row of the table: which register, and which bits of it. */
typedef struct {
    kdrv_st25r3916_field_info_t info;
    uint8_t                     reg;    /**< 0xFF for a field of our own.  */
    uint8_t                     shift;
    uint8_t                     mask;   /**< After the shift.              */
} row_t;

/** @brief A field this driver keeps rather than reads. */
#define REG_OURS 0xFFU

static const row_t ROWS[KDRV_ST25_FIELD_MAX] = {
    [KDRV_ST25_IDENTITY]   = { { "identity",   "bits",  0, 255, 1, false },
                               ST25R3916_REG_IC_IDENTITY, 0U, 0xFFU },
    [KDRV_ST25_IC_TYPE]    = { { "ic_type",    "code",  0,  31, 1, false },
                               ST25R3916_REG_IC_IDENTITY, 3U, 0x1FU },
    [KDRV_ST25_IC_REV]     = { { "ic_rev",     "code",  0,   7, 1, false },
                               ST25R3916_REG_IC_IDENTITY, 0U, 0x07U },
    [KDRV_ST25_OP_CONTROL] = { { "op_control", "bits",  0, 255, 1, false },
                               ST25R3916_REG_OP_CONTROL, 0U, 0xFFU },
    [KDRV_ST25_EN]         = { { "en",         "bool",  0,   1, 1, false },
                               ST25R3916_REG_OP_CONTROL, 7U, 0x01U },
    [KDRV_ST25_RX_EN]      = { { "rx_en",      "bool",  0,   1, 1, false },
                               ST25R3916_REG_OP_CONTROL, 6U, 0x01U },
    [KDRV_ST25_RX_CHN]     = { { "rx_chn",     "bool",  0,   1, 1, false },
                               ST25R3916_REG_OP_CONTROL, 5U, 0x01U },
    [KDRV_ST25_RX_MAN]     = { { "rx_man",     "bool",  0,   1, 1, false },
                               ST25R3916_REG_OP_CONTROL, 4U, 0x01U },
    [KDRV_ST25_TX_EN]      = { { "tx_en",      "bool",  0,   1, 1, false },
                               ST25R3916_REG_OP_CONTROL, 3U, 0x01U },
    [KDRV_ST25_WU]         = { { "wu",         "bool",  0,   1, 1, false },
                               ST25R3916_REG_OP_CONTROL, 2U, 0x01U },
    [KDRV_ST25_MODE]       = { { "mode",       "bits",  0, 255, 1, false },
                               ST25R3916_REG_MODE, 0U, 0xFFU },
    [KDRV_ST25_OM]         = { { "om",         "code",  0,  15, 1, false },
                               ST25R3916_REG_MODE, 3U, 0x0FU },
    [KDRV_ST25_TR_AM]      = { { "tr_am",      "bool",  0,   1, 1, false },
                               ST25R3916_REG_MODE, 7U, 0x01U },
    [KDRV_ST25_BIT_RATE]   = { { "bit_rate",   "bits",  0, 255, 1, false },
                               ST25R3916_REG_BIT_RATE, 0U, 0xFFU },
    [KDRV_ST25_TXRATE]     = { { "txrate",     "code",  0,  15, 1, false },
                               ST25R3916_REG_BIT_RATE, 4U, 0x0FU },
    [KDRV_ST25_RXRATE]     = { { "rxrate",     "code",  0,  15, 1, false },
                               ST25R3916_REG_BIT_RATE, 0U, 0x0FU },
    [KDRV_ST25_REGULATOR]  = { { "regulator",  "bits",  0, 255, 1, false },
                               ST25R3916_REG_REGULATOR_RESULT, 0U, 0xFFU },
    [KDRV_ST25_FIELD]      = { { "field",      "bool",  0,   1, 1, false },
                               REG_OURS, 0U, 0U },
    [KDRV_ST25_TAGS]       = { { "tags",       "count", 0, INT32_MAX, 1, false },
                               REG_OURS, 0U, 0U },
    [KDRV_ST25_ERRORS]     = { { "errors",     "count", 0, INT32_MAX, 1, false },
                               REG_OURS, 0U, 0U },
};

kode_status_t kdrv_st25r3916_get(kdrv_st25r3916_field_t f, int32_t *value)
{
    if ( (NULL == value) || (KDRV_ST25_FIELD_MAX <= f) )
    {
        return KODE_DENIED;
    }
    if ( !s_present )
    {
        return KODE_DENIED;
    }

    if ( REG_OURS == ROWS[f].reg )
    {
        if ( KDRV_ST25_FIELD == f )
        {
            *value = 0;
            if ( s_field )
            {
                *value = 1;
            }
            return KODE_OK;
        }
        if ( KDRV_ST25_TAGS == f )
        {
            *value = (int32_t)s_tags;
            return KODE_OK;
        }
        *value = (int32_t)s_errors;
        return KODE_OK;
    }

    uint8_t reg = 0U;
    if ( st25r3916ReadRegister(ROWS[f].reg, &reg) != RFAL_ERR_NONE )
    {
        return KODE_ERROR;
    }
    *value = (int32_t)((reg >> ROWS[f].shift) & ROWS[f].mask);
    return KODE_OK;
}

const kdrv_st25r3916_field_info_t *kdrv_st25r3916_field_info(
        kdrv_st25r3916_field_t f)
{
    if ( KDRV_ST25_FIELD_MAX <= f )
    {
        return NULL;
    }
    return &ROWS[f].info;
}
