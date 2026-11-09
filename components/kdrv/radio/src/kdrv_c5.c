/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_c5.c
 * @brief The ESP32-C5's table: one pass of RPCs, then a snapshot.
 */
#include "kdrv_c5.h"

#include <stdio.h>
#include <string.h>

#include "kbsp.h"
#include "kode_log.h"
#include "sdkconfig.h"

#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_hosted.h"
#include "esp_hosted_transport_config.h"

#define TAG "kdrv.c5"

/* The header spells the chip id out so its contract stays free of ESP-IDF;
 * this is where the spelling is held to the real enum. */
_Static_assert(KDRV_C5_CHIP_ESP32C5 == (int)CHIP_ESP32C5,
               "KDRV_C5_CHIP_ESP32C5 is not esp_chip_info.h's ESP32-C5");

/* The board's statement of the link and the transport's configuration must
 * be one fact. Guarded because the host suite compiles this file with no
 * esp-hosted Kconfig at all. */
#ifdef CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ
_Static_assert(CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ == KBSP_C5_SDIO_KHZ,
               "kbsp_board.h and sdkconfig.board disagree on the C5's SDIO clock");
#endif
#ifdef CONFIG_ESP_HOSTED_SDIO_4_BIT_BUS
_Static_assert(KBSP_C5_SDIO_WIDTH == 4U,
               "sdkconfig.board asks for four SDIO lines; kbsp_board.h says otherwise");
#endif

/* What the last successful refresh took, group by group. The two volatile
 * rows at the end are written from other threads - CP_INIT arrives on
 * esp-hosted's event task - and survive a forget. */
static struct {
    bool     valid;          /**< The identity group is filled.             */
    uint32_t chip_id;
    char     target[16];
    uint32_t fw_major;
    uint32_t fw_minor;
    uint32_t fw_patch;
    uint8_t  mac[6];
    uint32_t sdio_khz;
    uint8_t  sdio_width;

    bool     app_desc;       /**< The descriptor group is filled.           */
    bool     project_kode;
    char     project[32];
    char     version[32];
    char     idf_ver[32];
    char     date[16];
    char     time[16];
    uint8_t  sha[32];

    bool     heap_ok;        /**< The heap group is filled.                 */
    uint32_t heap_free;
    uint32_t heap_largest;

    bool     gpio_ok;        /**< The pin group is filled.                  */
    int      sd_cd;
    int      pwr_ext;

    volatile int32_t  reset_reason;
    volatile uint32_t link_ms;
} s_c5;

/* ---- The table ----------------------------------------------------------- */

/* Which group a row belongs to, which is also what decides its refusal. */
#define SRC_SELF 0U   /* this driver's own rows: always answer            */
#define SRC_ID   1U   /* the identity: refused until a refresh succeeded  */
#define SRC_APP  2U   /* the descriptor: refused when the slave has none  */
#define SRC_HEAP 3U   /* the memory monitor: refused when it did not answer */
#define SRC_PIN  4U   /* the RPC GPIO service: likewise                   */

/** @brief One field: what the outside sees, then where it comes from. */
typedef struct {
    kdrv_c5_field_info_t info;
    uint8_t              src;
} field_t;

#define ROW(nm, un, mn, mx, s) { { (nm), (un), (mn), (mx), 1U, false }, (s) }
#define SELF(nm, un, mn, mx)  ROW(nm, un, mn, mx, SRC_SELF)
#define IDENT(nm, un, mn, mx) ROW(nm, un, mn, mx, SRC_ID)
#define APP(nm, un, mn, mx)   ROW(nm, un, mn, mx, SRC_APP)
#define HEAP(nm)              ROW(nm, "KB", 0, 1048576, SRC_HEAP)
#define PIN(nm)               ROW(nm, "bool", 0, 1, SRC_PIN)

static const field_t FIELDS[KDRV_C5_FIELD_MAX] = {
    [KDRV_C5_VALID]           = SELF("valid",           "bool", 0, 1),

    [KDRV_C5_CHIP_ID]         = IDENT("chip_id",        "code", 0, 65535),
    [KDRV_C5_FW_MAJOR]        = IDENT("fw_major",       "code", 0, 255),
    [KDRV_C5_FW_MINOR]        = IDENT("fw_minor",       "code", 0, 255),
    [KDRV_C5_FW_PATCH]        = IDENT("fw_patch",       "code", 0, 255),

    [KDRV_C5_HOST_MAJOR]      = SELF("host_major",      "code", 0, 255),
    [KDRV_C5_HOST_MINOR]      = SELF("host_minor",      "code", 0, 255),
    [KDRV_C5_HOST_PATCH]      = SELF("host_patch",      "code", 0, 255),

    [KDRV_C5_APP_DESC]        = SELF("app_desc",        "bool", 0, 1),
    [KDRV_C5_PROJECT_KODE]    = APP("project_kode",     "bool", 0, 1),

    [KDRV_C5_MAC_HI]          = IDENT("mac_hi",         "code", 0, 65535),
    [KDRV_C5_MAC_MID]         = IDENT("mac_mid",        "code", 0, 65535),
    [KDRV_C5_MAC_LO]          = IDENT("mac_lo",         "code", 0, 65535),

    [KDRV_C5_RESET_REASON]    = SELF("reset_reason",    "code", 0, 255),
    [KDRV_C5_LINK_MS]         = SELF("link_ms",         "ms",   0, 60000),

    [KDRV_C5_HEAP_FREE_KB]    = HEAP("heap_free_kb"),
    [KDRV_C5_HEAP_LARGEST_KB] = HEAP("heap_largest_kb"),

    [KDRV_C5_SDIO_KHZ]        = IDENT("sdio_khz",       "kHz",  0, 1000000),
    [KDRV_C5_SDIO_WIDTH]      = IDENT("sdio_width",     "bits", 0, 8),

    [KDRV_C5_SD_CD]           = PIN("sd_cd"),
    [KDRV_C5_PWR_EXT]         = PIN("pwr_ext"),
};

/** @brief Whether the group @p src belongs to answered at the last refresh. */
static bool group_filled(uint8_t src)
{
    bool filled;

    switch ( src )
    {
        case SRC_ID:   filled = s_c5.valid;    break;
        case SRC_APP:  filled = s_c5.app_desc; break;
        case SRC_HEAP: filled = s_c5.heap_ok;  break;
        case SRC_PIN:  filled = s_c5.gpio_ok;  break;
        default:       filled = true;          break;
    }
    return filled;
}

/** @brief A bool as the table's 0 or 1. */
static int32_t flag(bool on)
{
    if ( on )
    {
        return 1;
    }
    return 0;
}

/** @brief Two MAC bytes as one code, the way the table carries them. */
static int32_t mac_pair(uint8_t first)
{
    return (int32_t)(((uint32_t)s_c5.mac[first] << 8)
                     | (uint32_t)s_c5.mac[first + 1U]);
}

/**
 * @brief Read one row of the snapshot.
 * @param[in]  f     Which field; already known to be in the table and filled.
 * @param[out] value Its value.
 */
static void snap_field(kdrv_c5_field_t f, int32_t *value)
{
    switch ( f )
    {
        case KDRV_C5_VALID:        *value = flag(s_c5.valid);          break;
        case KDRV_C5_CHIP_ID:      *value = (int32_t)s_c5.chip_id;     break;
        case KDRV_C5_FW_MAJOR:     *value = (int32_t)s_c5.fw_major;    break;
        case KDRV_C5_FW_MINOR:     *value = (int32_t)s_c5.fw_minor;    break;
        case KDRV_C5_FW_PATCH:     *value = (int32_t)s_c5.fw_patch;    break;
        case KDRV_C5_HOST_MAJOR:   *value = ESP_HOSTED_VERSION_MAJOR_1; break;
        case KDRV_C5_HOST_MINOR:   *value = ESP_HOSTED_VERSION_MINOR_1; break;
        case KDRV_C5_HOST_PATCH:   *value = ESP_HOSTED_VERSION_PATCH_1; break;
        case KDRV_C5_APP_DESC:     *value = flag(s_c5.app_desc);       break;
        case KDRV_C5_PROJECT_KODE: *value = flag(s_c5.project_kode);   break;
        case KDRV_C5_MAC_HI:       *value = mac_pair(0U);              break;
        case KDRV_C5_MAC_MID:      *value = mac_pair(2U);              break;
        case KDRV_C5_MAC_LO:       *value = mac_pair(4U);              break;
        case KDRV_C5_RESET_REASON: *value = s_c5.reset_reason;         break;
        case KDRV_C5_LINK_MS:      *value = (int32_t)s_c5.link_ms;     break;
        case KDRV_C5_HEAP_FREE_KB:
            *value = (int32_t)(s_c5.heap_free / 1024U);
            break;
        case KDRV_C5_HEAP_LARGEST_KB:
            *value = (int32_t)(s_c5.heap_largest / 1024U);
            break;
        case KDRV_C5_SDIO_KHZ:     *value = (int32_t)s_c5.sdio_khz;    break;
        case KDRV_C5_SDIO_WIDTH:   *value = (int32_t)s_c5.sdio_width;  break;
        case KDRV_C5_SD_CD:        *value = s_c5.sd_cd;                break;
        case KDRV_C5_PWR_EXT:      *value = s_c5.pwr_ext;              break;
        default:                   *value = 0;                         break;
    }
}

kode_status_t kdrv_c5_get(kdrv_c5_field_t f, int32_t *value)
{
    if ( (NULL == value) || (KDRV_C5_FIELD_MAX <= f) )
    {
        return KODE_DENIED;
    }
    if ( !group_filled(FIELDS[f].src) )
    {
        return KODE_DENIED;
    }
    snap_field(f, value);
    return KODE_OK;
}

kode_status_t kdrv_c5_set(kdrv_c5_field_t f, int32_t value)
{
    (void)f;
    (void)value;
    return KODE_DENIED;
}

const kdrv_c5_field_info_t *kdrv_c5_field_info(kdrv_c5_field_t f)
{
    if ( KDRV_C5_FIELD_MAX <= f )
    {
        return NULL;
    }
    return &FIELDS[f].info;
}

kode_status_t kdrv_c5_field_by_name(const char *name, kdrv_c5_field_t *f)
{
    if ( (NULL == name) || (NULL == f) )
    {
        return KODE_DENIED;
    }
    for ( uint16_t i = 0U; (uint16_t)KDRV_C5_FIELD_MAX > i; ++i )
    {
        if ( strcmp(FIELDS[i].info.name, name) == 0 )
        {
            *f = (kdrv_c5_field_t)i;
            return KODE_OK;
        }
    }
    return KODE_DENIED;
}

/* ---- What the table cannot say ------------------------------------------- */

/**
 * @brief Hand out one of the snapshot's strings.
 * @param have Whether the group it belongs to was filled.
 * @param src  The string; empty counts as not sent.
 * @param out  Where it goes, NUL-terminated.
 * @param len  Room there.
 * @return KODE_OK, or KODE_DENIED when there is nothing honest to give.
 */
static kode_status_t give_str(bool have, const char *src, char *out, size_t len)
{
    if ( (NULL == out) || (0U == len) || !have || ('\0' == src[0]) )
    {
        return KODE_DENIED;
    }
    (void)snprintf(out, len, "%s", src);
    return KODE_OK;
}

kode_status_t kdrv_c5_target(char *out, size_t len)
{
    return give_str(s_c5.valid, s_c5.target, out, len);
}

kode_status_t kdrv_c5_project_name(char *out, size_t len)
{
    return give_str(s_c5.app_desc, s_c5.project, out, len);
}

kode_status_t kdrv_c5_app_version(char *out, size_t len)
{
    return give_str(s_c5.app_desc, s_c5.version, out, len);
}

kode_status_t kdrv_c5_idf_ver(char *out, size_t len)
{
    return give_str(s_c5.app_desc, s_c5.idf_ver, out, len);
}

kode_status_t kdrv_c5_build_date(char *out, size_t len)
{
    return give_str(s_c5.app_desc, s_c5.date, out, len);
}

kode_status_t kdrv_c5_build_time(char *out, size_t len)
{
    return give_str(s_c5.app_desc, s_c5.time, out, len);
}

kode_status_t kdrv_c5_elf_sha(uint8_t out[32])
{
    if ( (NULL == out) || !s_c5.app_desc )
    {
        return KODE_DENIED;
    }
    /* A descriptor without the full descriptor carries an all-zero digest,
     * which is not a digest. */
    bool any = false;
    for ( size_t i = 0U; sizeof(s_c5.sha) > i; ++i )
    {
        if ( 0U != s_c5.sha[i] )
        {
            any = true;
        }
    }
    if ( !any )
    {
        return KODE_DENIED;
    }
    (void)memcpy(out, s_c5.sha, sizeof(s_c5.sha));
    return KODE_OK;
}

kode_status_t kdrv_c5_mac(uint8_t out[6])
{
    if ( (NULL == out) || !s_c5.valid )
    {
        return KODE_DENIED;
    }
    (void)memcpy(out, s_c5.mac, sizeof(s_c5.mac));
    return KODE_OK;
}

/* ---- The dump -------------------------------------------------------------- */

#define C5_DUMP_LINE 160U

/**
 * @brief Append @p text to @p line, flushing the line first if it would not fit.
 * @param line The line being built.
 * @param n    Characters in it so far; updated.
 * @param text What to add.
 */
static void dump_append(char *line, size_t *n, const char *text)
{
    size_t const len = strlen(text);

    if ( (*n + len) >= C5_DUMP_LINE )
    {
        KLOG_N(TAG, "%s", line);
        *n = 0U;
        line[0] = '\0';
    }
    (void)memcpy(&line[*n], text, len + 1U);
    *n += len;
}

/** @brief @p s when @p have, a dash otherwise: what a log prints for an
 *         absent string, so a reader never mistakes empty for present. */
static const char *or_dash(bool have, const char *s)
{
    if ( !have || ('\0' == s[0]) )
    {
        return "-";
    }
    return s;
}

void kdrv_c5_dump(void)
{
    char   line[C5_DUMP_LINE + 32U];
    size_t n = 0U;

    line[0] = '\0';
    for ( uint16_t i = 0U; (uint16_t)KDRV_C5_FIELD_MAX > i; ++i )
    {
        char    pair[40];
        int32_t value = 0;

        if ( kdrv_c5_get((kdrv_c5_field_t)i, &value) == KODE_OK )
        {
            (void)snprintf(pair, sizeof(pair), "%s=%ld ",
                           FIELDS[i].info.name, (long)value);
        }
        else
        {
            (void)snprintf(pair, sizeof(pair), "%s=? ", FIELDS[i].info.name);
        }
        dump_append(line, &n, pair);
    }
    if ( 0U < n )
    {
        KLOG_N(TAG, "%s", line);
    }

    if ( !s_c5.valid )
    {
        KLOG_N(TAG, "no snapshot: the link has not been up");
        return;
    }
    KLOG_N(TAG, "target=%s mac=%02x:%02x:%02x:%02x:%02x:%02x",
           s_c5.target,
           (unsigned)s_c5.mac[0], (unsigned)s_c5.mac[1],
           (unsigned)s_c5.mac[2], (unsigned)s_c5.mac[3],
           (unsigned)s_c5.mac[4], (unsigned)s_c5.mac[5]);
    {
        /* The digest through its own getter, so a descriptor built without
         * the full one prints a dash here as it does in the row. */
        uint8_t sha[32];
        char    shahex[9];
        if ( kdrv_c5_elf_sha(sha) == KODE_OK )
        {
            (void)snprintf(shahex, sizeof(shahex), "%02x%02x%02x%02x",
                           (unsigned)sha[0], (unsigned)sha[1],
                           (unsigned)sha[2], (unsigned)sha[3]);
        }
        else
        {
            (void)snprintf(shahex, sizeof(shahex), "%s", "-");
        }
        KLOG_N(TAG, "project=%s version=%s idf=%s date=%s time=%s sha=%s",
               or_dash(s_c5.app_desc, s_c5.project),
               or_dash(s_c5.app_desc, s_c5.version),
               or_dash(s_c5.app_desc, s_c5.idf_ver),
               or_dash(s_c5.app_desc, s_c5.date),
               or_dash(s_c5.app_desc, s_c5.time),
               shahex);
    }
}

/* ---- The worker's half ---------------------------------------------------- */

void kdrv_c5_forget(void)
{
    int32_t const  reason = s_c5.reset_reason;
    uint32_t const ms     = s_c5.link_ms;

    (void)memset(&s_c5, 0, sizeof(s_c5));
    s_c5.reset_reason = reason;
    s_c5.link_ms      = ms;
}

void kdrv_c5_note_reset(int32_t reason)
{
    s_c5.reset_reason = reason;
}

void kdrv_c5_note_link_ms(uint32_t ms)
{
    s_c5.link_ms = ms;
}

/**
 * @brief The image descriptor group.
 *
 * An esp-hosted from before this RPC answers with an error; that is an
 * older slave, not a broken one, and the row above says which it saw.
 */
static void refresh_app_desc(void)
{
    esp_hosted_app_desc_t d;
    esp_err_t const err = esp_hosted_get_coprocessor_app_desc(&d);

    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "the slave did not answer the app-descriptor RPC (%s): "
                    "a stock or older esp-hosted build", esp_err_to_name(err));
        return;
    }
    (void)snprintf(s_c5.project, sizeof(s_c5.project), "%.*s",
                   (int)sizeof(d.project_name), d.project_name);
    (void)snprintf(s_c5.version, sizeof(s_c5.version), "%.*s",
                   (int)sizeof(d.version), d.version);
    (void)snprintf(s_c5.idf_ver, sizeof(s_c5.idf_ver), "%.*s",
                   (int)sizeof(d.idf_ver), d.idf_ver);
    (void)snprintf(s_c5.date, sizeof(s_c5.date), "%.*s",
                   (int)sizeof(d.date), d.date);
    (void)snprintf(s_c5.time, sizeof(s_c5.time), "%.*s",
                   (int)sizeof(d.time), d.time);
    (void)memcpy(s_c5.sha, d.app_elf_sha256, sizeof(s_c5.sha));
    s_c5.project_kode = (strcmp(s_c5.project, KDRV_C5_PROJECT_NAME) == 0);
    s_c5.app_desc     = true;
}

/** @brief The heap group: one query, no monitor left running. */
static void refresh_heap(void)
{
    esp_hosted_config_mem_monitor_t cfg;
    esp_hosted_curr_mem_info_t      info;

    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.config = ESP_HOSTED_MEMMONITOR_NO_CHANGE;
    if ( esp_hosted_set_mem_monitor(&cfg, &info) != ESP_OK )
    {
        KLOG_W(TAG, "the slave did not answer the memory monitor");
        return;
    }
    s_c5.heap_free    = info.curr_internal.cap_8bit.free_size;
    s_c5.heap_largest = info.curr_internal.cap_8bit.largest_free_block;
    s_c5.heap_ok      = true;
}

/** @brief The pin group: two reads through the RPC GPIO service, no writes. */
static void refresh_pins(void)
{
    int cd  = -1;
    int ext = -1;

    /* The card's supply is the P4's (KBSP_PIN_SD_PWR, kdrv_sd's to drive),
     * so the one card pin the C5 has to report is card-detect. The header's
     * supply enable is read with its input switched on: the C5's firmware
     * never configures that pin, and a pin with no input reads 0 whatever
     * R72 holds it at. Switching an input on drives nothing. */
    if ( (esp_hosted_cp_gpio_get_level(KBSP_C5_IO_SD_CD, &cd) != ESP_OK)
        || (esp_hosted_cp_gpio_input_enable(KBSP_C5_IO_PWR_EXT) != ESP_OK)
        || (esp_hosted_cp_gpio_get_level(KBSP_C5_IO_PWR_EXT, &ext) != ESP_OK) )
    {
        KLOG_W(TAG, "the RPC GPIO service did not answer; the pins are "
                    "unknown");
        return;
    }
    s_c5.sd_cd   = cd;
    s_c5.pwr_ext = ext;
    s_c5.gpio_ok = true;
}

/**
 * @brief Drive one of the C5's IOs through the RPC GPIO service.
 *
 * The level first and the output after it, so the pin comes up where it is
 * wanted instead of passing through the other level on the way. With the
 * input on as well, a read that follows is what the pin holds.
 *
 * @param io The C5's IO.
 * @param on true drives it high.
 * @return true when every call was taken.
 */
static bool c5_io_drive(uint32_t io, bool on)
{
    esp_hosted_cp_gpio_config_t cfg;
    uint32_t                    level = 0U;

    if ( on )
    {
        level = 1U;
    }
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.pin_bit_mask = 1ULL << io;
    cfg.mode         = H_CP_GPIO_MODE_INPUT_OUTPUT;
    return (esp_hosted_cp_gpio_set_level(io, level) == ESP_OK)
           && (esp_hosted_cp_gpio_config(&cfg) == ESP_OK)
           && (esp_hosted_cp_gpio_set_level(io, level) == ESP_OK);
}

kode_status_t kdrv_c5_amp(bool on)
{
    if ( !c5_io_drive((uint32_t)KBSP_C5_IO_PA_CTRL, on) )
    {
        KLOG_E(TAG, "the C5 would not drive the amplifier's enable");
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t kdrv_c5_exp_power(bool on)
{
    int read = -1;

    if ( !c5_io_drive((uint32_t)KBSP_C5_IO_PWR_EXT, on) )
    {
        KLOG_E(TAG, "the C5 would not drive the header's supply enable");
        return KODE_ERROR;
    }
    if ( esp_hosted_cp_gpio_get_level(KBSP_C5_IO_PWR_EXT, &read) == ESP_OK )
    {
        s_c5.pwr_ext = read;
    }
    else
    {
        KLOG_W(TAG, "the header's supply enable would not read back");
    }
    return KODE_OK;
}

kode_status_t kdrv_c5_refresh(void)
{
    uint32_t                       chip = 0U;
    char                           target[sizeof(s_c5.target)];
    esp_hosted_coprocessor_fwver_t fw;
    uint8_t                        mac[6];
    struct esp_hosted_sdio_config *sdio = NULL;

    kdrv_c5_forget();

    if ( esp_hosted_get_cp_info(&chip, target, sizeof(target)) != ESP_OK )
    {
        KLOG_E(TAG, "the slave did not say which chip it is");
        return KODE_ERROR;
    }
    if ( esp_hosted_get_coprocessor_fwversion(&fw) != ESP_OK )
    {
        KLOG_E(TAG, "the slave did not answer the version RPC");
        return KODE_ERROR;
    }
    if ( esp_hosted_iface_mac_addr_get(mac, sizeof(mac), ESP_MAC_WIFI_STA)
        != ESP_OK )
    {
        KLOG_E(TAG, "the slave did not answer its MAC");
        return KODE_ERROR;
    }
    if ( (esp_hosted_sdio_get_config(&sdio) != ESP_TRANSPORT_OK)
        || (NULL == sdio) )
    {
        KLOG_E(TAG, "the transport would not say what it negotiated");
        return KODE_ERROR;
    }

    s_c5.chip_id = chip;
    (void)snprintf(s_c5.target, sizeof(s_c5.target), "%s", target);
    s_c5.fw_major   = fw.major1;
    s_c5.fw_minor   = fw.minor1;
    s_c5.fw_patch   = fw.patch1;
    (void)memcpy(s_c5.mac, mac, sizeof(s_c5.mac));
    s_c5.sdio_khz   = sdio->clock_freq_khz;
    s_c5.sdio_width = sdio->bus_width;
    s_c5.valid      = true;

    refresh_app_desc();
    refresh_heap();
    refresh_pins();

    KLOG_I(TAG, "%s, esp-hosted %lu.%lu.%lu, project %s, SDIO %u-bit @%lukHz",
           s_c5.target,
           (unsigned long)s_c5.fw_major, (unsigned long)s_c5.fw_minor,
           (unsigned long)s_c5.fw_patch,
           or_dash(s_c5.app_desc, s_c5.project),
           (unsigned)s_c5.sdio_width, (unsigned long)s_c5.sdio_khz);
    return KODE_OK;
}
