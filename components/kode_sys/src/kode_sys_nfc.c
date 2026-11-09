/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_nfc.c
 * @brief NFC through AO_Nfc: the requests, and the blocking calls behind one
 *        claim, each waiting the caller's window plus a margin.
 */
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_sys_priv.h"

#include "kaos.h"
#include "khal_sync.h"

#include <stddef.h>
#include <string.h>

/* ---- The answers' payloads against the drivers' types they carry ---- */
_Static_assert(KQP_NFC_UID_MAX == KNFC_UID_MAX, "the payload's identity is the driver's size");
_Static_assert(KODE_SYS_NFC_UID_MAX == KNFC_UID_MAX, "the facade's identity ceiling is the driver's");

/* ---- The facade's names against the driver's ---- */
_Static_assert(KODE_SYS_NFC_TYPE_UNKNOWN == (uint32_t)KNFC_TAG_UNKNOWN, "tag types follow the driver's");
_Static_assert(KODE_SYS_NFC_TYPE_T2T == (uint32_t)KNFC_TAG_T2T, "tag types follow the driver's");
_Static_assert(KODE_SYS_NFC_TYPE_T4T == (uint32_t)KNFC_TAG_T4T, "tag types follow the driver's");
_Static_assert(KODE_SYS_NFC_TYPE_MIFARE == (uint32_t)KNFC_TAG_MIFARE, "tag types follow the driver's");
_Static_assert(KODE_SYS_NFC_NDEF_NO_MESSAGE == KNFC_NDEF_NO_MESSAGE, "NDEF statuses follow the driver's");
_Static_assert(KODE_SYS_NFC_NDEF_LOCKED == KNFC_NDEF_LOCKED, "NDEF statuses follow the driver's");
_Static_assert(KODE_SYS_NFC_NDEF_TOO_LONG == KNFC_NDEF_TOO_LONG, "NDEF statuses follow the driver's");
_Static_assert(KODE_SYS_NFC_NDEF_LOST == KODE_TIMEOUT, "a lost tag is the transport's timeout");
_Static_assert(KQP_NFC_EMU_NDEF_MAX == KNFC_EMU_NDEF_MAX, "the payload's message is the driver's size");
_Static_assert(KQP_NFC_EMU_RAW_MAX == KNFC_EMU_RAW_MAX, "the payload's raw message is the driver's size");
_Static_assert(offsetof(kode_sys_nfc_tag_t, uid_len) == offsetof(knfc_tag_t, uid_len),
               "the tag's identity length where the driver's is");
_Static_assert(offsetof(kode_sys_nfc_tag_t, atqa) == offsetof(knfc_tag_t, atqa),
               "the tag's ATQA where the driver's is");
_Static_assert(offsetof(kode_sys_nfc_tag_t, sak) == offsetof(knfc_tag_t, sak),
               "the tag's SAK where the driver's is");
_Static_assert(sizeof(kode_sys_nfc_card_t) == sizeof(knfc_emu_card_t), "the card is the driver's");
_Static_assert(offsetof(kode_sys_nfc_card_t, uid_len) == offsetof(knfc_emu_card_t, uid_len),
               "the card's identity length where the driver's is");
_Static_assert(offsetof(kode_sys_nfc_card_t, atqa) == offsetof(knfc_emu_card_t, atqa),
               "the card's ATQA where the driver's is");
_Static_assert(offsetof(kode_sys_nfc_card_t, uri) == offsetof(knfc_emu_card_t, uri),
               "the card's URI flag where the driver's is");
_Static_assert(offsetof(kode_sys_nfc_card_t, ndef) == offsetof(knfc_emu_card_t, ndef),
               "the card's message where the driver's is");
_Static_assert(offsetof(kode_sys_nfc_card_t, raw_len) == offsetof(knfc_emu_card_t, raw_len),
               "the card's raw length where the driver's is");
_Static_assert(offsetof(kode_sys_nfc_card_t, raw) == offsetof(knfc_emu_card_t, raw),
               "the card's raw message where the driver's is");

/* ---- NFC state: one claim flag, as AO_Nfc takes one request at a time ---- */
static khal_mutex_t s_nfc_lock;               /**< Guards s_nfc_busy and s_nfc_gen.   */
khal_sem_t   kode_sys_nfc_sem;                /**< AO_Nfc signals a request's end.    */
static bool         s_nfc_busy;               /**< An NFC call holds the claim.       */
static uint32_t     s_nfc_gen;                /**< Never 0.                           */
static knfc_tag_t   s_nfc_tag;                /**< What AO_Nfc writes a tag into.     */
static uint32_t     s_nfc_read_rc;            /**< kode_block_nfc_read_tag()'s status.  */
static char         s_nfc_text[KODE_SYS_NFC_TEXT_MAX];   /**< An NDEF message in or out, and its NUL. */
static size_t       s_nfc_len;                /**< Bytes of s_nfc_text.               */
uint32_t     kode_sys_nfc_rc;                 /**< The exchange's kode_status_t.      */
static knfc_emu_card_t s_nfc_emu_card;        /**< The card lent to AO_Nfc to present. */
static uint32_t     s_nfc_emu_reads;          /**< READs served, on a stop.           */
uint32_t     kode_sys_nfc_ack;                /**< Whose NFC answer that is.          */

void kode_sys_nfc_init(void)
{
    kode_sys_nfc_sem = khal_sem_create();
    s_nfc_lock       = khal_mutex_create();
}

/**
 * @brief Take the NFC facade and stamp this call's generation.
 * @param[out] gen The generation, never zero.
 * @return true when this caller has it.
 */
bool kode_sys_nfc_claim(uint32_t *gen)
{
    bool took = false;

    khal_mutex_lock(s_nfc_lock);
    if ( !s_nfc_busy )
    {
        s_nfc_busy = true;
        took       = true;
        s_nfc_gen += 1U;
        if ( 0U == s_nfc_gen )
        {
            s_nfc_gen = 1U;
        }
        *gen = s_nfc_gen;
    }
    khal_mutex_unlock(s_nfc_lock);
    return took;
}

/** @brief Give the NFC facade back. */
void kode_sys_nfc_release(void)
{
    khal_mutex_lock(s_nfc_lock);
    s_nfc_busy = false;
    khal_mutex_unlock(s_nfc_lock);
}

/**
 * @brief Wait for this call's own NFC answer, discarding an abandoned one.
 * @param gen This call's generation.
 * @param ms  How long AO_Nfc was asked to keep the field up.
 * @return true when the answer that arrived was this call's.
 */
bool kode_sys_nfc_await(uint32_t gen, uint32_t ms)
{
    return kode_sys_answer_await(kode_sys_nfc_sem, &kode_sys_nfc_ack, gen,
                                 ms + KODE_NFC_WAIT_MARGIN_MS);
}

kode_status_t kode_block_nfc_read_tag(knfc_tag_t *out, uint32_t ms)
{
    uint32_t gen = 0U;

    if ( (NULL == out) || (0U == ms) )
    {
        return KODE_ERROR;
    }
    if ( !kode_sys_nfc_claim(&gen) )
    {
        return KODE_DENIED;
    }

    s_nfc_read_rc = (uint32_t)KODE_TIMEOUT;
    nfc_read_request((void *)&s_nfc_tag, &s_nfc_read_rc, kode_sys_nfc_sem, ms, gen,
                     &kode_sys_nfc_ack);

    kode_status_t rc = KODE_TIMEOUT;
    if ( kode_sys_nfc_await(gen, ms) )
    {
        rc = (kode_status_t)s_nfc_read_rc;
        if ( KODE_OK == rc )
        {
            *out = s_nfc_tag;
        }
    }
    kode_sys_nfc_release();
    return rc;
}

/**
 * @brief The body of both NDEF calls: one request, one wait, one answer.
 * @param text   What to write, or NULL to read.
 * @param out    Where to put what was read; NULL when writing.
 * @param cap    How many bytes @p out holds.
 * @param n      Out: how many were written; may be NULL.
 * @param is_uri Out: reading, whether the record was a URI; may be NULL. Written only on KODE_OK.
 * @param uri    Writing: a URI record rather than a Text one.
 * @param ms     The window.
 * @return What the exchange came to.
 */
static kode_status_t nfc_ndef(const char *text, char *out, size_t cap,
                              size_t *n, bool *is_uri, bool uri, uint32_t ms)
{
    static bool s_nfc_uri;   /* The record AO_Nfc read was a URI. */
    uint32_t    gen = 0U;

    if ( 0U == ms )
    {
        return KODE_ERROR;
    }
    if ( !kode_sys_nfc_claim(&gen) )
    {
        return KODE_DENIED;
    }

    bool write = NULL != text;
    if ( write )
    {
        (void)memcpy(s_nfc_text, text, strlen(text) + 1U);
    }
    else
    {
        s_nfc_text[0] = '\0';
    }
    s_nfc_len = 0U;
    s_nfc_uri = false;
    kode_sys_nfc_rc  = (uint32_t)KODE_TIMEOUT;

    nfc_ndef_task_request(s_nfc_text, sizeof(s_nfc_text), &s_nfc_len, &s_nfc_uri,
                          write, uri, &kode_sys_nfc_rc, kode_sys_nfc_sem, ms, gen, &kode_sys_nfc_ack);

    kode_status_t rc = KODE_TIMEOUT;
    if ( kode_sys_nfc_await(gen, ms) )
    {
        rc = (kode_status_t)kode_sys_nfc_rc;
        if ( !write && (KODE_OK == rc) )
        {
            if ( (s_nfc_len + 1U) > cap )
            {
                rc = KODE_NO_RESOURCES;
            }
            else
            {
                (void)memcpy(out, s_nfc_text, s_nfc_len + 1U);
                if ( NULL != n )
                {
                    *n = s_nfc_len;
                }
                if ( NULL != is_uri )
                {
                    *is_uri = s_nfc_uri;
                }
            }
        }
    }
    kode_sys_nfc_release();
    return rc;
}

kode_status_t kode_block_nfc_ndef_read(char *out, size_t cap, size_t *n,
                                       bool *uri, uint32_t ms)
{
    if ( (NULL == out) || (0U == cap) )
    {
        return KODE_ERROR;
    }
    return nfc_ndef(NULL, out, cap, n, uri, false, ms);
}

/**
 * @brief Whether @p text fits an NDEF message: a terminator within KODE_SYS_NFC_TEXT_MAX.
 * @param text The message; not NULL.
 * @return true when it is at most KODE_SYS_NFC_TEXT_MAX - 1 characters.
 */
static bool nfc_text_fits(const char *text)
{
    return memchr(text, '\0', KODE_SYS_NFC_TEXT_MAX) != NULL;
}

kode_status_t kode_block_nfc_ndef_write_uri(const char *uri, uint32_t ms)
{
    if ( (NULL == uri) || !nfc_text_fits(uri) )
    {
        return KODE_ERROR;
    }
    return nfc_ndef(uri, NULL, 0U, NULL, NULL, true, ms);
}

kode_status_t kode_block_nfc_ndef_write_text(const char *text, uint32_t ms)
{
    if ( (NULL == text) || !nfc_text_fits(text) )
    {
        return KODE_ERROR;
    }
    return nfc_ndef(text, NULL, 0U, NULL, NULL, false, ms);
}

/** @brief How long the emulation start and stop wait for AO_Nfc: a liveness bound. */
#define KODE_NFC_EMU_WAIT_MS  1000U

kode_status_t kode_block_nfc_emulate_start(const knfc_emu_card_t *card)
{
    uint32_t gen = 0U;

    if ( NULL == card )
    {
        return KODE_ERROR;
    }
    if ( !kode_sys_nfc_claim(&gen) )
    {
        return KODE_DENIED;
    }

    /* The driver copies the card during the step that answers; a static outlives the round trip. */
    s_nfc_emu_card = *card;
    kode_sys_nfc_rc       = (uint32_t)KODE_TIMEOUT;
    nfc_emulate_request(&s_nfc_emu_card, &kode_sys_nfc_rc, kode_sys_nfc_sem, gen, &kode_sys_nfc_ack,
                        NULL);

    kode_status_t rc = KODE_TIMEOUT;
    if ( kode_sys_answer_await(kode_sys_nfc_sem, &kode_sys_nfc_ack, gen, KODE_NFC_EMU_WAIT_MS) )
    {
        rc = (kode_status_t)kode_sys_nfc_rc;
    }
    kode_sys_nfc_release();
    return rc;
}

kode_status_t kode_block_nfc_emulate_stop(uint32_t *reads)
{
    uint32_t gen = 0U;

    if ( !kode_sys_nfc_claim(&gen) )
    {
        return KODE_DENIED;
    }

    kode_sys_nfc_rc        = (uint32_t)KODE_TIMEOUT;
    s_nfc_emu_reads = 0U;
    nfc_emulate_stop_request(&kode_sys_nfc_rc, &s_nfc_emu_reads, kode_sys_nfc_sem, gen,
                             &kode_sys_nfc_ack, NULL);

    kode_status_t rc = KODE_TIMEOUT;
    if ( kode_sys_answer_await(kode_sys_nfc_sem, &kode_sys_nfc_ack, gen, KODE_NFC_EMU_WAIT_MS) )
    {
        rc = (kode_status_t)kode_sys_nfc_rc;
        if ( NULL != reads )
        {
            *reads = s_nfc_emu_reads;
        }
    }
    kode_sys_nfc_release();
    return rc;
}

bool kode_sys_nfc_is_emulating(void)
{
    return nfc_is_emulating();
}

/* ---- kode_sys's requests: AO_Nfc answers the asking AO ---- */

kode_status_t kode_sys_nfc_read_tag(QActive *me, uint32_t ms)
{
    if ( 0U == ms )
    {
        return KODE_DENIED;
    }
    return nfc_read_ask(me, ms);
}

kode_status_t kode_sys_nfc_ndef_read(QActive *me, uint32_t ms)
{
    if ( 0U == ms )
    {
        return KODE_DENIED;
    }
    return nfc_ndef_ask(me, NULL, false, ms);
}

/**
 * @brief The body of both NDEF writes.
 * @param me   The asking AO.
 * @param text What to write; shorter than KODE_SYS_NFC_TEXT_MAX.
 * @param uri  A URI record rather than a Text one.
 * @param ms   The window.
 * @return As kode_sys_nfc_ndef_write_uri().
 */
static kode_status_t nfc_write_ask(QActive *me, const char *text, bool uri, uint32_t ms)
{
    /* A message too long is the owner's refusal, at its own door. */
    if ( (NULL == text) || (0U == ms) )
    {
        return KODE_DENIED;
    }
    return nfc_ndef_ask(me, text, uri, ms);
}

kode_status_t kode_sys_nfc_ndef_write_uri(QActive *me, const char *uri, uint32_t ms)
{
    return nfc_write_ask(me, uri, true, ms);
}

kode_status_t kode_sys_nfc_ndef_write_text(QActive *me, const char *text, uint32_t ms)
{
    return nfc_write_ask(me, text, false, ms);
}

kode_status_t kode_sys_nfc_emulate_start(QActive *me, const kode_sys_nfc_card_t *card)
{
    return nfc_emulate_ask(me, card);
}

kode_status_t kode_sys_nfc_emulate_stop(QActive *me)
{
    return nfc_emulate_stop_ask(me);
}
