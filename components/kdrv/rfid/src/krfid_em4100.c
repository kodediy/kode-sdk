/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file krfid_em4100.c
 * @brief The pure EM4100 codec (see krfid_em4100.h). Host-testable, no IDF deps.
 */
#include "krfid_em4100.h"
#include <string.h>

#define MAXN   KRFID_EM4100_MAX_SAMPLES /* max samples per decode window           */
#define GLITCH 5           /* runs shorter than this are noise, ignored for timing */

/**
 * @brief Resolve a decoded bit, applying polarity.
 * @param v   A decoded half-bit pair result (0, 1, or 2 for a violation).
 * @param inv Non-zero to invert the polarity.
 * @return The bit (0/1), or -1 if @p v was a Manchester violation.
 */
static inline int getbit(uint8_t v, int inv)
{
    if ( 1 < v )
    {
        return -1;
    }
    if ( inv )
    {
        return v ^ 1;
    }
    return v;
}

/**
 * @brief Try to parse a valid EM4100 frame starting at bit index @p s.
 * @param bit    The Manchester-decoded bit stream.
 * @param nb     Number of bits in @p bit.
 * @param s      Candidate frame start.
 * @param inv    Polarity to resolve the bits with.
 * @param[out] id 5-byte ID on success.
 * @return 0 and @p id on success; -1 otherwise.
 */
static int parse_frame(const uint8_t *bit, int nb, int s, int inv, uint8_t id[5])
{
    if ( s + 64 > nb ) return -1;
    for ( int k = 0; 9 > k; k++ )                 /* 9-bit header, all 1 */
        if ( getbit(bit[s + k], inv) != 1 ) return -1;

    uint8_t nib[10];
    int p = s + 9;
    for ( int r = 0; 10 > r; r++, p += 5 )
    {      /* 10 rows: 4 data + 1 even parity */
        int d0 = getbit(bit[p], inv),   d1 = getbit(bit[p + 1], inv);
        int d2 = getbit(bit[p + 2], inv), d3 = getbit(bit[p + 3], inv);
        int pp = getbit(bit[p + 4], inv);
        if ( (d0 | d1 | d2 | d3 | pp) < 0 ) return -1;
        if ( (d0 ^ d1 ^ d2 ^ d3) != pp ) return -1;
        nib[r] = (d0 << 3) | (d1 << 2) | (d2 << 1) | d3;
    }
    for ( int c = 0; 4 > c; c++ )
    {               /* 4 column parities (even) */
        int par = 0;
        for ( int r = 0; 10 > r; r++ ) par ^= (nib[r] >> (3 - c)) & 1;
        int cb = getbit(bit[p + c], inv);
        if ( 0 > cb || cb != par ) return -1;
    }
    if ( getbit(bit[p + 4], inv) != 0 ) return -1; /* stop bit = 0 */

    for ( int i = 0; 5 > i; i++ ) id[i] = (nib[2 * i] << 4) | nib[2 * i + 1];
    if ( !(id[0] | id[1] | id[2] | id[3] | id[4]) ) return -1;  /* reject all-zero = false frame */
    return 0;
}

int krfid_em4100_decode(const uint16_t *samples, int n, uint32_t sample_hz,
                        krfid_em4100_work_t *work,
                        uint8_t id[5], krfid_em4100_diag_t *diag)
{
    /* The four stages' arrays, by the names the pipeline below knows them by.
     * They are the caller's: 84 KB is too much to hold as statics, which land
     * in internal RAM whether or not a tag is ever read. */
    uint8_t *const s_dig = work->dig;
    int     *const s_run = work->run;
    uint8_t *const s_hb  = work->hb;
    uint8_t *const s_bit = work->bit;

    krfid_em4100_diag_t d;
    memset(&d, 0, sizeof d);
    if ( MAXN < n ) n = MAXN;

    /* --- 1. threshold at the mid-level, gate on amplitude ---
     * NOTE: do NOT low-pass / smooth the samples first. With good tag coupling the RX
     * delivers clean Manchester levels (mean ~ pk-pk/2), but as coupling weakens the
     * AC-coupled front-end differentiates the data into NARROW SPIKES (~3-4 samples,
     * mean << pk-pk/2). A moving average widens those spikes past GLITCH, and the
     * half-bit estimator then locks onto the spike width (~4) instead of the gap
     * between spikes (~20) -> wrong bit rate, no frame. Threshold the raw samples;
     * sub-GLITCH spikes are correctly ignored by the half-bit estimator below. */
    int mn = 65535, mx = 0;
    for ( int i = 0; i < n; i++ )
    {
        if ( samples[i] < mn ) mn = samples[i];
        if ( samples[i] > mx ) mx = samples[i];
    }
    d.pkpk_raw = mx - mn;
    if ( diag ) *diag = d;
    if ( 120 > d.pkpk_raw ) return -1;            /* too flat -> no tag (~100 mV) */
    int thr = (mn + mx) / 2;
    for ( int i = 0; i < n; i++ ) s_dig[i] = samples[i] > thr;

    /* --- 2. run-length encode --- */
    int nr = 0, cur = s_dig[0], len = 1, first = s_dig[0];
    for ( int i = 1; i < n; i++ )
    {
        if ( s_dig[i] == cur ) { len++; }
        else { s_run[nr++] = len; cur = s_dig[i]; len = 1; }
    }
    s_run[nr++] = len;

    /* --- 3. estimate the half-bit width: the smallest run length that is
     *        corroborated by many others. First/last runs are excluded because
     *        they are partial (window edges) and would otherwise fool it. --- */
    int lo_i = 1, hi_i = nr - 1;
    if ( hi_i - lo_i < 4 ) { lo_i = 0; hi_i = nr; }
    int hb = 0, best = 1 << 30;
    int need = (hi_i - lo_i) / 8; if ( 3 > need ) need = 3;
    for ( int i = lo_i; i < hi_i; i++ )
    {
        int cand = s_run[i];
        if ( GLITCH > cand || cand >= best ) continue;
        int c2 = 0; long sum = 0;
        for ( int j = lo_i; j < hi_i; j++ )
            if ( s_run[j] * 10 >= cand * 7 && s_run[j] * 10 <= cand * 13 ) { c2++; sum += s_run[j]; }
        if ( c2 >= need ) { best = cand; hb = (int)(sum / c2); }
    }
    if ( 0 == hb ) return -2;
    d.halfbit_samples = hb;
    /* bit period = 2*half-bit; RF/div = 125kHz * bit_period. Snap to 16/32/64. */
    if ( sample_hz )
    {
        int div = (int)((250000.0 * hb) / (double)sample_hz + 0.5);
        if ( 24 > div )
        {
            d.bitrate_div = 16;
        }
        else if ( 48 > div )
        {
            d.bitrate_div = 32;
        }
        else
        {
            d.bitrate_div = 64;
        }
    }
    if ( diag ) *diag = d;
    if ( 5 > hb || 40 < hb ) return -2;           /* implausible timing -> not a tag */

    /* --- 4. rebuild the half-bit level stream --- */
    int nh = 0, lvl = first;
    for ( int i = 0; i < nr && MAXN > nh; i++ )
    {
        int c = (s_run[i] * 2 + hb) / (2 * hb);  /* round(run / hb) */
        if ( 1 > c ) c = 1;
        if ( 8 < c ) c = 8;                        /* cap gaps so a glitch can't run away */
        for ( int k = 0; k < c && MAXN > nh; k++ ) s_hb[nh++] = lvl;
        lvl ^= 1;
    }

    /* --- 5. Manchester decode + frame search over both phases and polarities --- */
    for ( int phase = 0; 2 > phase; phase++ )
    {
        int nb = 0;
        for ( int i = phase; i + 1 < nh; i += 2 )
        {
            int a = s_hb[i], b = s_hb[i + 1];
            uint8_t decoded;
            if ( 1 == a && 0 == b )
            {
                decoded = 1;
            }
            else if ( 0 == a && 1 == b )
            {
                decoded = 0;
            }
            else
            {
                decoded = 2;
            }
            s_bit[nb++] = decoded;
        }
        for ( int inv = 0; 2 > inv; inv++ )
        {
            for ( int s = 0; s + 64 <= nb; s++ )
            {
                if ( parse_frame(s_bit, nb, s, inv, id) == 0 )
                {
                    d.phase = phase; d.inverted = inv; d.frame_start_bit = s;
                    if ( diag ) *diag = d;
                    return 0;
                }
            }
        }
    }
    return -2;                                   /* tag present, but no clean frame */
}

uint64_t krfid_em4100_frame(const uint8_t id[5])
{
    uint8_t nib[10];
    for ( int i = 0; 5 > i; i++ ) { nib[2 * i] = id[i] >> 4; nib[2 * i + 1] = id[i] & 0xF; }

    uint64_t f = 0;
    int pos = 63;
    for ( int i = 0; 9 > i; i++ ) f |= (uint64_t)1 << pos--;   /* 9-bit header, all 1 */

    uint8_t col[4] = {0};
    for ( int r = 0; 10 > r; r++ )
    {                           /* 10 rows: 4 data + row parity */
        uint8_t p = 0;
        for ( int b = 0; 4 > b; b++ )
        {
            int bit = (nib[r] >> (3 - b)) & 1;
            f |= (uint64_t)bit << pos--;
            p ^= bit; col[b] ^= bit;
        }
        f |= (uint64_t)p << pos--;
    }
    for ( int b = 0; 4 > b; b++ ) f |= (uint64_t)col[b] << pos--;  /* 4 column parities */
    /* pos is now 0: the stop bit stays 0. */
    return f;
}
