/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kapps_fmt.h
 * @brief The `apps` region's on-flash format, shared verbatim with the bootloader: no ESP-IDF here.
 */
#ifndef KAPPS_FMT_H_
#define KAPPS_FMT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KAPPS_DIR_MAGIC        0x5050414BU   /* "KAPP" */
#define KAPPS_REQ_MAGIC        0x4E55524BU   /* "KRUN" */
#define KAPPS_OS_INTENT_MAGIC  0x41544F4BU   /* "KOTA" - app_pending's intent flag */
/* "KFW1" - the Kode container's magic, twinned from KFW_MAGIC in kdrv_kfw.h
 * so the bootloader can recognise a container without pulling that header
 * (and everything kode_defs.h drags in). kdrv_kfw.c asserts the two are
 * the same number, so they cannot drift. */
#define KAPPS_KFW_MAGIC        0x3157464BU
#define KAPPS_FMT_VERSION      1U

#define KAPPS_DIR_A_OFF        0x0000U
#define KAPPS_DIR_B_OFF        0x1000U
#define KAPPS_REQ_OFF          0x2000U
#define KAPPS_DATA_OFF         0x10000U
#define KAPPS_ALIGN            0x10000U      /* the MMU page: images must start here */
#define KAPPS_MAX_ENTRIES      64U

/* app_pending is its own partition (base = the image at offset 0), and its
 * intent word lives in the partition's LAST 4 KB sector, whatever its size.
 * kdrv_apps finds that sector from the partition table at run time. This
 * number is what it comes to for today's 6 MB mailbox, and it is the
 * contract with the host tools that write the word, which do not read the
 * table: growing app_pending therefore means moving this constant AND those
 * tools in the same change. test_kdrv_apps_region.c pins the constant to
 * partitions.csv, so a table change that forgets fails a host case rather
 * than leaving the tools writing into the middle of an image. */
#define KAPPS_PENDING_INTENT_OFF  0x5FF000U

#define KAPPS_REQ_STATE_EMPTY  0xFFFFFFFFU   /* erased flash                       */
#define KAPPS_REQ_STATE_NEW    0xAAAAAAAAU   /* kodeOS wrote it; boot this once    */
#define KAPPS_REQ_STATE_ALWAYS 0x55555555U   /* boot this across USB resets; only
                                              * mailbox adoption writes it         */
#define KAPPS_REQ_STATE_ARMED  0x00005555U   /* the bootloader wrote it on the
                                              * first boot of an ALWAYS request;
                                              * from here on the USB rule applies   */
#define KAPPS_REQ_STATE_TRIED  0x00000000U   /* the bootloader retired it          */

#define KAPPS_ENTRY_VALID      (1U << 0)     /* bits 1..31 reserved */

#define KAPPS_PART_SUBTYPE     0x40U         /* the `apps` region              */
#define KAPPS_PENDING_SUBTYPE  0x41U         /* the `app_pending` mailbox      */

typedef struct {
    uint32_t offset;   /* from region start; multiple of KAPPS_ALIGN */
    uint32_t size;     /* exact image length in bytes                */
    uint32_t seq;      /* install order, monotonic: the FIFO         */
    uint32_t flags;
} kapps_entry_t;

typedef struct {
    uint32_t      magic;
    uint32_t      version;
    uint32_t      gen;
    uint32_t      count;
    kapps_entry_t e[KAPPS_MAX_ENTRIES];
    uint32_t      crc;    /* CRC32 of every byte above this field */
} kapps_dir_t;

/* kdrv_apps writes the magic last, so that only a whole copy carries it;
 * that order means "everything after the first word, then the first word". */
_Static_assert(offsetof(kapps_dir_t, magic) == 0U, "the directory's magic is its first word");

typedef struct {
    uint32_t magic;
    uint32_t offset;   /* region-relative */
    uint32_t size;
    uint32_t crc;      /* CRC32 of magic..size - state is NOT covered */
    uint32_t state;
} kapps_req_t;

#define KAPPS_REQ_LEN       20U  /**< On-flash size of a boot request; frozen. */
#define KAPPS_REQ_STATE_AT  16U  /**< Byte offset of `state`; the bootloader writes it there. */

/* kodeOS and the bootloader are updated apart: this layout must never move. */
_Static_assert(sizeof(kapps_req_t) == KAPPS_REQ_LEN, "the boot request is a frozen on-flash layout");
_Static_assert(offsetof(kapps_req_t, state) == KAPPS_REQ_STATE_AT, "the bootloader writes state at +16");

/**
 * @brief Could an image at @p offset of @p size be booted from the region?
 *
 * Geometry only: it says nothing about magic, CRC or state, because the two
 * callers check those with different tools - the bootloader has its own flash
 * reader and its own CRC entry point.
 *
 * It lives in this header because it has two readers and they must not drift.
 * The bootloader used to carry these five conditions alone, which made it the
 * only thing between a wrong boot request and a jump into the directory
 * sector - in code that cannot be updated over the air. Now the writer refuses
 * to record a request that this rejects, and the bootloader still checks,
 * because flash is not to be trusted just because something trustworthy wrote
 * it once.
 *
 * @p offset is bounded against @p region_size on its own line, before the
 * subtraction on the last one. That is defensive rather than load-bearing -
 * unsigned wrap is well defined, and the standalone bound refuses an offset
 * past the end whatever the subtraction produced - but it keeps the last line
 * meaning what it reads as meaning.
 *
 * @param offset      Region-relative byte offset of the image.
 * @param size        Its exact length in bytes.
 * @param region_size Size of the whole `apps` region.
 * @return true when the image lies wholly inside the data area on an MMU page.
 */
static inline bool kapps_req_bounds_ok(uint32_t offset, uint32_t size,
                                       uint32_t region_size)
{
    return (KAPPS_DATA_OFF <= offset)
        && ((offset % KAPPS_ALIGN) == 0U)
        && (0U != size)
        && (offset <= region_size)
        && ((region_size - offset) >= size);
}

/* The resets that keep an ARMED request standing, as the P4's ROM numbers
 * them (soc/esp32p4/include/soc/reset_reasons.h). Restated here because this
 * header includes nothing from ESP-IDF; the bootloader asserts at compile time
 * that each equals the RESET_REASON_* it stands for. */
#define KAPPS_RESET_CORE_SW     0x03U  /**< RESET_REASON_CORE_SW: OpenOCD's first reset.       */
#define KAPPS_RESET_DEEP_SLEEP  0x05U  /**< RESET_REASON_CORE_DEEP_SLEEP: a deep-sleep wake.   */
#define KAPPS_RESET_CPU_SW      0x0CU  /**< RESET_REASON_CPU_SW: esp_restart(), a panic's reboot. */
#define KAPPS_RESET_USB_JTAG    0x16U  /**< RESET_REASON_CORE_USB_JTAG: OpenOCD over USB.       */
#define KAPPS_RESET_USB_UART    0x17U  /**< RESET_REASON_CORE_USB_UART: the host opened the port. */
#define KAPPS_RESET_CPU_JTAG    0x18U  /**< RESET_REASON_CPU_JTAG: OpenOCD's reset halt.        */

/**
 * @brief Does this reset keep an ARMED request standing? A closed list.
 *
 * The resets a working sketch or its host produce keep it, so an uploaded
 * sketch survives the Serial Monitor and the debugger. Everything else -
 * power-on, which BOT+LEFT is, a watchdog, a brownout, a number nobody named -
 * retires it and kodeOS boots.
 *
 * @param cause The ROM's reset reason for CPU 0.
 * @return true when the cause is one of the six KAPPS_RESET_* above.
 */
static inline bool kapps_req_cause_keeps(uint32_t cause)
{
    bool keep;

    switch ( cause )
    {
    case KAPPS_RESET_CORE_SW:
    case KAPPS_RESET_DEEP_SLEEP:
    case KAPPS_RESET_CPU_SW:
    case KAPPS_RESET_USB_JTAG:
    case KAPPS_RESET_USB_UART:
    case KAPPS_RESET_CPU_JTAG:
        keep = true;
        break;
    default:
        keep = false;
        break;
    }
    return keep;
}

/**
 * @brief Does an ARMED request still stand after this reset?
 *
 * Only on a kept reset, and only while the mailbox is empty: an image waiting
 * there is an Upload kodeOS has to adopt, and the armed sketch gives way.
 *
 * @param cause        The ROM's reset reason for CPU 0.
 * @param mailbox_full Whether an image (or an unreadable mailbox) is waiting.
 * @return true when the bootloader boots the armed app again.
 */
static inline bool kapps_req_armed_stands(uint32_t cause, bool mailbox_full)
{
    return kapps_req_cause_keeps(cause) && !mailbox_full;
}

/**
 * @brief Is this boot request whole and asking for a boot?
 *
 * The CRC is the caller's, because the bootloader and kodeOS compute it with
 * different entry points; it covers magic..size and never the state word.
 *
 * @param req The request as read from flash.
 * @param crc The CRC32 of its bytes up to `crc`, computed by the caller.
 * @return true for the right magic, a matching CRC and a NEW, ALWAYS or ARMED state.
 */
static inline bool kapps_req_live(const kapps_req_t *req, uint32_t crc)
{
    return (KAPPS_REQ_MAGIC == req->magic)
        && (req->crc == crc)
        && ((KAPPS_REQ_STATE_NEW == req->state)
            || (KAPPS_REQ_STATE_ALWAYS == req->state)
            || (KAPPS_REQ_STATE_ARMED == req->state));
}

/**
 * @brief The state the bootloader writes before it jumps.
 *
 * NEW becomes TRIED (one run), ALWAYS becomes ARMED (the sticky run has
 * started); anything else is returned as it is, and for ARMED that means the
 * bootloader writes nothing. Each step only clears bits.
 *
 * @param state The live state read from flash.
 * @return The state to leave behind.
 */
static inline uint32_t kapps_req_next_state(uint32_t state)
{
    uint32_t next = state;

    if ( KAPPS_REQ_STATE_NEW == state )
    {
        next = KAPPS_REQ_STATE_TRIED;
    }
    else if ( KAPPS_REQ_STATE_ALWAYS == state )
    {
        next = KAPPS_REQ_STATE_ARMED;
    }
    else
    {
        /* ARMED, or a state the caller already refused: unchanged. */
    }
    return next;
}

/**
 * @brief Can a directory whose CRC holds be walked and trusted?
 *
 * The CRC says a copy was written whole, not that what was written makes
 * sense: a writer's bug would seal a bad directory as faithfully as a good
 * one. Every reader walks e[] up to `count` and takes each live extent at its
 * word, so the count has to fit the array and every live entry has to be an
 * image the region could boot. kdrv_apps reads a copy through this and
 * refuses to commit one that fails it; the bootloader never walks the
 * directory.
 *
 * @param d           The directory.
 * @param region_size Size of the whole `apps` region.
 * @return true when `count` fits and every live entry lies inside the data area.
 */
static inline bool kapps_dir_sane(const kapps_dir_t *d, uint32_t region_size)
{
    bool sane = (KAPPS_MAX_ENTRIES >= d->count);

    for ( uint32_t i = 0U; sane && (i < d->count); ++i )
    {
        if ( (d->e[i].flags & KAPPS_ENTRY_VALID) != 0U )
        {
            sane = kapps_req_bounds_ok(d->e[i].offset, d->e[i].size, region_size);
        }
    }
    return sane;
}

#define KAPPS_DIR_ERASED  0xFFFFFFFFU  /**< The magic word of an erased directory sector. */

/** @brief What a directory copy read from flash is. */
typedef enum {
    KAPPS_DIR_COPY_OK,       /**< Ours, whole and sane: it may be used.               */
    KAPPS_DIR_COPY_BLANK,    /**< Erased flash: nothing was ever written there.       */
    KAPPS_DIR_COPY_CORRUPT,  /**< Ours, but torn or not sane: worth nothing.          */
    KAPPS_DIR_COPY_FOREIGN   /**< Another format's directory: not ours to touch.      */
} kapps_dir_copy_t;

/**
 * @brief Judge a directory copy as read from flash.
 *
 * A copy with this format's magic and another version is FOREIGN: a newer
 * kodeOS wrote it, and this one must not format over it. Everything else that
 * is not whole is BLANK or CORRUPT, and loses to the other copy. The CRC is
 * the caller's, for the same reason as in kapps_req_live().
 *
 * @param d           The copy.
 * @param crc         The CRC32 of its bytes up to `crc`, computed by the caller.
 * @param region_size Size of the whole `apps` region.
 * @return What the copy is.
 */
static inline kapps_dir_copy_t kapps_dir_classify(const kapps_dir_t *d, uint32_t crc,
                                                  uint32_t region_size)
{
    kapps_dir_copy_t kind = KAPPS_DIR_COPY_OK;

    if ( KAPPS_DIR_ERASED == d->magic )
    {
        kind = KAPPS_DIR_COPY_BLANK;
    }
    else if ( KAPPS_DIR_MAGIC != d->magic )
    {
        kind = KAPPS_DIR_COPY_CORRUPT;
    }
    else if ( KAPPS_FMT_VERSION != d->version )
    {
        kind = KAPPS_DIR_COPY_FOREIGN;
    }
    else if ( (d->crc != crc) || !kapps_dir_sane(d, region_size) )
    {
        kind = KAPPS_DIR_COPY_CORRUPT;
    }
    else
    {
        /* whole, sane, ours */
    }
    return kind;
}

/** @brief Which directory copy a boot uses. */
typedef enum {
    KAPPS_DIR_PICK_A,      /**< Copy A is used.                         */
    KAPPS_DIR_PICK_B,      /**< Copy B is used.                         */
    KAPPS_DIR_PICK_FORMAT  /**< Neither is whole: the region is formatted. */
} kapps_dir_pick_t;

/**
 * @brief Choose between the two directory copies, as otadata does.
 *
 * The whole copy with the higher generation wins, and A wins a tie. A copy
 * that is not whole loses whatever generation it claims, because a torn write
 * can leave any number there.
 *
 * @param a_ok  Whether copy A is whole and sane.
 * @param a_gen Copy A's generation.
 * @param b_ok  Whether copy B is whole and sane.
 * @param b_gen Copy B's generation.
 * @return The copy to use, or KAPPS_DIR_PICK_FORMAT when neither is whole.
 */
static inline kapps_dir_pick_t kapps_dir_pick(bool a_ok, uint32_t a_gen,
                                              bool b_ok, uint32_t b_gen)
{
    kapps_dir_pick_t pick = KAPPS_DIR_PICK_FORMAT;

    if ( a_ok && (!b_ok || (a_gen >= b_gen)) )
    {
        pick = KAPPS_DIR_PICK_A;
    }
    else if ( b_ok )
    {
        pick = KAPPS_DIR_PICK_B;
    }
    else
    {
        /* Neither: format. */
    }
    return pick;
}

#endif /* KAPPS_FMT_H_ */
