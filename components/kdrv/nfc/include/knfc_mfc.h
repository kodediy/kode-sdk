/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file knfc_mfc.h
 * @brief Mifare Classic: authenticate with a key you have, read, write.
 */
#ifndef KNFC_MFC_H_
#define KNFC_MFC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A sector key is six bytes. */
#define KNFC_MFC_KEY_LEN     6U
/** @brief A block is sixteen bytes, always. */
#define KNFC_MFC_BLOCK_LEN  16U
/** @brief The first four sectors of any card, which is all kodeOS needs. */
#define KNFC_MFC_SECTOR_LEN  4U

/** @brief Which of a sector's two keys to authenticate with. */
typedef enum {
    KNFC_MFC_KEY_A = 0,
    KNFC_MFC_KEY_B,
} knfc_mfc_key_t;

/**
 * @brief Authenticate to the sector @p block belongs to.
 *
 * Must be repeated for every sector, and again after any failure: a card
 * that refuses stops answering entirely, and only a reselect recovers it -
 * which this does for the caller.
 *
 * @param block The block whose sector to authenticate to.
 * @param type  Key A or key B.
 * @param key   The six key bytes, as they are stored in the trailer.
 * @param uid   The card's four-byte identifier - the last four bytes of a
 *              longer UID.
 * @return KODE_OK when the card proved it holds the same key; KODE_DENIED
 *         for a bad argument or no activated card; KODE_TIMEOUT when the
 *         card went silent, which is how it refuses; KODE_ERROR when it
 *         answered something that was not the expected proof.
 */
kode_status_t knfc_mfc_auth(uint8_t block, knfc_mfc_key_t type,
                            const uint8_t *key, const uint8_t *uid);

/**
 * @brief Try every key kodeOS knows against @p block, and keep the one that
 *        works.
 *
 * The dictionary is short on purpose: the factory default and the keys the
 * NFC Forum's own mapping puts on a blank tag. A card outside it is a card
 * with keys somebody chose, and kodeOS does not go looking for those.
 *
 * @param block The block whose sector to open.
 * @param uid   The card's four-byte identifier.
 * @param type  Out: which key type worked. May be NULL.
 * @param key   Out: the six bytes that worked, at least KNFC_MFC_KEY_LEN.
 *              May be NULL.
 * @return KODE_OK when one worked, KODE_DENIED when none did.
 */
kode_status_t knfc_mfc_auth_any(uint8_t block, const uint8_t *uid,
                                knfc_mfc_key_t *type, uint8_t *key);

/**
 * @brief Read one block. The sector must be authenticated already.
 *
 * @param block The block number.
 * @param out   Out: KNFC_MFC_BLOCK_LEN bytes.
 * @return KODE_OK; KODE_DENIED for a NULL @p out or no authentication;
 *         KODE_TIMEOUT when the card did not answer; KODE_ERROR when what
 *         came back was the wrong length or failed its checksum.
 */
kode_status_t knfc_mfc_read(uint8_t block, uint8_t *out);

/**
 * @brief Write one block. The sector must be authenticated already.
 *
 * Refuses a sector trailer outright: those hold the keys, and a wrong write
 * there loses the sector permanently. Refuses block 0 too, which is the
 * manufacturer's.
 *
 * @param block The block number.
 * @param data  KNFC_MFC_BLOCK_LEN bytes.
 * @return KODE_OK; KODE_DENIED for a NULL @p data, a trailer, block 0, or no
 *         authentication; KODE_TIMEOUT when the card did not acknowledge;
 *         KODE_ERROR when it refused the write.
 */
kode_status_t knfc_mfc_write(uint8_t block, const uint8_t *data);

/**
 * @brief Try a batch of the full public dictionary against @p block.
 *
 * Chunked on purpose: a full sweep is thousands of keys and a card that
 * refuses each one costs a frame timeout, so it cannot run in one step. A
 * caller runs it a batch at a time - this returns after @p batch keys whether
 * or not one worked - and paces the batches itself.
 *
 * @param block      The block whose sector to open.
 * @param uid        The card's four-byte identifier.
 * @param from       The dictionary index to start this batch at.
 * @param batch      How many keys to try before returning.
 * @param found_idx  Out: the dictionary index that worked. May be NULL.
 * @param found_type Out: which key type worked. May be NULL.
 * @param tried      Out: how many keys this batch actually tried, so the
 *                   caller knows where the next batch begins and when the
 *                   dictionary is exhausted. May be NULL.
 * @return KODE_OK when a key in this batch worked, KODE_DENIED when none in
 *         this batch did (there may be more batches), KODE_TIMEOUT when the
 *         card could not be reselected at all.
 */
kode_status_t knfc_mfc_sweep(uint8_t block, const uint8_t *uid, size_t from,
                             size_t batch, size_t *found_idx,
                             knfc_mfc_key_t *found_type, size_t *tried);

/** @brief How many keys the public dictionary holds. */
size_t knfc_mfc_dict_size(void);

/**
 * @brief How many sectors a card of this SAK has.
 *
 * @param sak The SEL_RES.
 * @return 5 for a Mini, 16 for a 1K, 40 for a 4K; 16 as a safe default.
 */
uint8_t knfc_mfc_sectors(uint8_t sak);

/**
 * @brief The first block number of sector @p sector.
 *
 * The low 32 sectors hold four blocks each; a 4K's top eight hold sixteen,
 * which is why this is not simply @p sector times four.
 *
 * @param sector The sector index.
 * @return Its first block.
 */
uint8_t knfc_mfc_sector_first(uint8_t sector);

/**
 * @brief How many blocks sector @p sector holds.
 *
 * @param sector The sector index.
 * @return 4 for the low sectors, 16 for a 4K's top eight.
 */
uint8_t knfc_mfc_sector_blocks(uint8_t sector);

/** @brief Forget the authentication; the next read must open a sector again. */
void knfc_mfc_reset(void);

/**
 * @brief Is @p block a sector trailer?
 *
 * @param block The block number.
 * @return true when writing it would change the sector's keys.
 */
bool knfc_mfc_is_trailer(uint8_t block);

#ifdef __cplusplus
}
#endif

#endif /* KNFC_MFC_H_ */
