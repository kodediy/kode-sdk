/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file frame_png.c
 * @brief A PNG reader for the suite's frames: chunks, a small inflater and the row filters.
 */
#include "frame_png.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BITS   15     /**< Longest Huffman code. */
#define MAX_LCODES 288    /**< Literal/length codes. */
#define MAX_DCODES 30     /**< Distance codes. */
#define ZLIB_HEAD  2U     /**< The zlib header before the deflate stream. */
#define PNG_SIG    8U     /**< The PNG signature before the first chunk. */
#define CHUNK_HEAD 8U     /**< A chunk's length and type, before its data. */
#define CHUNK_MORE 12U    /**< A chunk's bytes besides its data: length, type, CRC. */
#define IHDR_DEPTH 8U     /**< IHDR's byte with the bit depth. */
#define IHDR_TYPE  9U     /**< IHDR's byte with the colour type. */
#define IHDR_LACE  12U    /**< IHDR's byte with the interlace method. */
#define DEPTH_8    8U     /**< 8 bits a sample. */
#define TYPE_RGB   2U     /**< Colour type 2: RGB. */
#define RGB_BYTES  3U     /**< Bytes of one pixel. */

/** @brief The inflater's state. */
typedef struct {
    const uint8_t *in;      /**< Compressed bytes. */
    size_t         inlen;   /**< How many. */
    size_t         inpos;   /**< Next one. */
    uint32_t       bitbuf;  /**< Bits not yet used. */
    int32_t        bitcnt;  /**< How many. */
    uint8_t       *out;     /**< Output. */
    size_t         outlen;  /**< Its room. */
    size_t         outpos;  /**< Next byte. */
    bool           err;     /**< Ran off the input or the output. */
} inf_t;

/** @brief A canonical Huffman code. */
typedef struct {
    int16_t count[MAX_BITS + 1];   /**< Codes of each length. */
    int16_t symbol[MAX_LCODES];    /**< Symbols in code order. */
} huff_t;

static int32_t bits(inf_t *s, int32_t need)
{
    uint32_t val = s->bitbuf;
    while ( s->bitcnt < need )
    {
        if ( s->inpos >= s->inlen )
        {
            s->err = true;
            return 0;
        }
        val |= (uint32_t)s->in[s->inpos] << s->bitcnt;
        s->inpos++;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int32_t)(val & ((1U << need) - 1U));
}

static void put(inf_t *s, uint8_t b)
{
    if ( s->outpos >= s->outlen )
    {
        s->err = true;
        return;
    }
    s->out[s->outpos] = b;
    s->outpos++;
}

static void build(huff_t *h, const int16_t *length, int32_t n)
{
    int16_t offs[MAX_BITS + 1];
    (void)memset(h->count, 0, sizeof(h->count));
    for ( int32_t i = 0; i < n; i++ )
    {
        h->count[length[i]]++;
    }
    offs[1] = 0;
    for ( int32_t len = 1; MAX_BITS > len; len++ )
    {
        offs[len + 1] = (int16_t)(offs[len] + h->count[len]);
    }
    for ( int32_t i = 0; i < n; i++ )
    {
        if ( 0 != length[i] )
        {
            h->symbol[offs[length[i]]] = (int16_t)i;
            offs[length[i]]++;
        }
    }
}

static int32_t decode(inf_t *s, const huff_t *h)
{
    int32_t code = 0;
    int32_t first = 0;
    int32_t index = 0;
    for ( int32_t len = 1; MAX_BITS >= len; len++ )
    {
        code |= bits(s, 1);
        const int32_t count = h->count[len];
        if ( (code - count) < first )
        {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = true;
    return -1;
}

static void codes(inf_t *s, const huff_t *lencode, const huff_t *distcode)
{
    static const int16_t LBASE[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                       35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const int16_t LEXT[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                      3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const int16_t DBASE[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
                                       513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const int16_t DEXT[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9,
                                      10, 10, 11, 11, 12, 12, 13, 13 };
    for ( ;; )
    {
        int32_t sym = decode(s, lencode);
        if ( s->err || (256 == sym) )
        {
            return;
        }
        if ( 256 > sym )
        {
            put(s, (uint8_t)sym);
            continue;
        }
        sym -= 257;
        if ( 29 <= sym )
        {
            s->err = true;
            return;
        }
        const int32_t len = LBASE[sym] + bits(s, LEXT[sym]);
        const int32_t dsym = decode(s, distcode);
        if ( s->err || (0 > dsym) || (30 <= dsym) )
        {
            s->err = true;
            return;
        }
        const size_t dist = (size_t)(DBASE[dsym] + bits(s, DEXT[dsym]));
        if ( dist > s->outpos )
        {
            s->err = true;
            return;
        }
        for ( int32_t i = 0; i < len; i++ )
        {
            put(s, s->out[s->outpos - dist]);
        }
    }
}

static void fixed(inf_t *s)
{
    int16_t lengths[MAX_LCODES];
    huff_t lencode;
    huff_t distcode;
    int32_t i = 0;
    for ( ; 144 > i; i++ )
    {
        lengths[i] = 8;
    }
    for ( ; 256 > i; i++ )
    {
        lengths[i] = 9;
    }
    for ( ; 280 > i; i++ )
    {
        lengths[i] = 7;
    }
    for ( ; MAX_LCODES > i; i++ )
    {
        lengths[i] = 8;
    }
    build(&lencode, lengths, MAX_LCODES);
    for ( i = 0; MAX_DCODES > i; i++ )
    {
        lengths[i] = 5;
    }
    build(&distcode, lengths, MAX_DCODES);
    codes(s, &lencode, &distcode);
}

static void dynamic(inf_t *s)
{
    static const int16_t ORDER[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int16_t lengths[MAX_LCODES + MAX_DCODES];
    huff_t lencode;
    huff_t distcode;
    const int32_t nlen = bits(s, 5) + 257;
    const int32_t ndist = bits(s, 5) + 1;
    const int32_t ncode = bits(s, 4) + 4;
    int32_t i = 0;
    for ( ; i < ncode; i++ )
    {
        lengths[ORDER[i]] = (int16_t)bits(s, 3);
    }
    for ( ; 19 > i; i++ )
    {
        lengths[ORDER[i]] = 0;
    }
    build(&lencode, lengths, 19);
    i = 0;
    while ( ((nlen + ndist) > i) && !s->err )
    {
        int32_t sym = decode(s, &lencode);
        if ( 16 > sym )
        {
            lengths[i] = (int16_t)sym;
            i++;
            continue;
        }
        int16_t len = 0;
        int32_t rep = 0;
        if ( 16 == sym )
        {
            if ( 0 == i )
            {
                s->err = true;
                return;
            }
            len = lengths[i - 1];
            rep = 3 + bits(s, 2);
        }
        else if ( 17 == sym )
        {
            rep = 3 + bits(s, 3);
        }
        else
        {
            rep = 11 + bits(s, 7);
        }
        if ( (i + rep) > (nlen + ndist) )
        {
            s->err = true;
            return;
        }
        while ( 0 < rep )
        {
            lengths[i] = len;
            i++;
            rep--;
        }
    }
    build(&lencode, lengths, nlen);
    build(&distcode, &lengths[nlen], ndist);
    codes(s, &lencode, &distcode);
}

/** @brief Inflate a zlib stream (its 2-byte header skipped) into @p out. */
static bool inflate(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen)
{
    inf_t s = { .in = in, .inlen = inlen, .inpos = ZLIB_HEAD, .out = out, .outlen = outlen };
    int32_t last = 0;
    while ( (0 == last) && !s.err )
    {
        last = bits(&s, 1);
        const int32_t type = bits(&s, 2);
        if ( 0 == type )
        {
            s.bitbuf = 0U;
            s.bitcnt = 0;
            if ( (s.inpos + 4U) > s.inlen )
            {
                return false;
            }
            const size_t len = (size_t)s.in[s.inpos] | ((size_t)s.in[s.inpos + 1U] << 8);
            s.inpos += 4U;
            for ( size_t i = 0U; (i < len) && !s.err; i++ )
            {
                if ( s.inpos >= s.inlen )
                {
                    return false;
                }
                put(&s, s.in[s.inpos]);
                s.inpos++;
            }
        }
        else if ( 1 == type )
        {
            fixed(&s);
        }
        else if ( 2 == type )
        {
            dynamic(&s);
        }
        else
        {
            return false;
        }
    }
    return !s.err && (s.outpos == outlen);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c)
{
    const int32_t p = (int32_t)a + (int32_t)b - (int32_t)c;
    const int32_t pa = abs(p - (int32_t)a);
    const int32_t pb = abs(p - (int32_t)b);
    const int32_t pc = abs(p - (int32_t)c);
    if ( (pa <= pb) && (pa <= pc) )
    {
        return a;
    }
    if ( pb <= pc )
    {
        return b;
    }
    return c;
}

uint16_t *frame_png_load(const char *path, int32_t *w, int32_t *h)
{
    FILE *f = fopen(path, "rb");
    if ( NULL == f )
    {
        return NULL;
    }
    (void)fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc((size_t)size);
    const bool read = (NULL != file) && (fread(file, 1, (size_t)size, f) == (size_t)size);
    (void)fclose(f);
    uint8_t *z = malloc((size_t)size);
    uint8_t *raw = NULL;
    uint16_t *px = NULL;
    size_t zlen = 0U;
    uint32_t width = 0U;
    uint32_t height = 0U;
    bool ok = read && (NULL != z) && ((long)PNG_SIG < size);
    /* Chunks after the 8-byte signature: IHDR first, the IDATs joined, IEND last. */
    for ( size_t at = PNG_SIG; ok && ((at + CHUNK_MORE) <= (size_t)size); )
    {
        const uint32_t len = be32(&file[at]);
        const uint8_t *type = &file[at + 4U];
        const uint8_t *data = &file[at + CHUNK_HEAD];
        if ( 0 == memcmp(type, "IHDR", 4) )
        {
            width = be32(data);
            height = be32(&data[4]);
            /* 8 bits, RGB, no interlace: what the generator writes. */
            ok = (DEPTH_8 == data[IHDR_DEPTH]) && (TYPE_RGB == data[IHDR_TYPE]) && (0U == data[IHDR_LACE]);
        }
        else if ( 0 == memcmp(type, "IDAT", 4) )
        {
            (void)memcpy(&z[zlen], data, len);
            zlen += len;
        }
        else if ( 0 == memcmp(type, "IEND", 4) )
        {
            break;
        }
        at += (size_t)len + CHUNK_MORE;
    }
    const size_t stride = (size_t)width * RGB_BYTES;
    if ( ok && (0U != width) )
    {
        raw = malloc((stride + 1U) * height);
        ok = (NULL != raw) && inflate(z, zlen, raw, (stride + 1U) * height);
    }
    if ( ok )
    {
        /* Undo the row filters in place: each row's previous row is already plain. */
        for ( uint32_t y = 0U; ok && (y < height); y++ )
        {
            uint8_t *row = &raw[(y * (stride + 1U)) + 1U];
            const uint8_t *up = NULL;
            if ( 0U < y )
            {
                up = &raw[((y - 1U) * (stride + 1U)) + 1U];
            }
            const uint8_t filter = row[-1];
            for ( size_t i = 0U; i < stride; i++ )
            {
                uint8_t a = 0U;
                uint8_t b = 0U;
                uint8_t c = 0U;
                if ( RGB_BYTES <= i )
                {
                    a = row[i - RGB_BYTES];
                }
                if ( NULL != up )
                {
                    b = up[i];
                    if ( RGB_BYTES <= i )
                    {
                        c = up[i - RGB_BYTES];
                    }
                }
                switch ( filter )
                {
                    case 0U:
                        break;
                    case 1U:
                        row[i] = (uint8_t)(row[i] + a);
                        break;
                    case 2U:
                        row[i] = (uint8_t)(row[i] + b);
                        break;
                    case 3U:
                        row[i] = (uint8_t)(row[i] + (uint8_t)(((uint32_t)a + (uint32_t)b) / 2U));
                        break;
                    case 4U:
                        row[i] = (uint8_t)(row[i] + paeth(a, b, c));
                        break;
                    default:
                        ok = false;
                        break;
                }
            }
        }
    }
    if ( ok )
    {
        px = malloc((size_t)width * height * sizeof(uint16_t));
        for ( size_t i = 0U; (NULL != px) && (((size_t)width * height) > i); i++ )
        {
            const uint8_t *p = &raw[((i / width) * (stride + 1U)) + 1U + ((i % width) * RGB_BYTES)];
            px[i] = (uint16_t)((((uint32_t)p[0] >> 3) << 11) | (((uint32_t)p[1] >> 2) << 5) | ((uint32_t)p[2] >> 3));
        }
        *w = (int32_t)width;
        *h = (int32_t)height;
    }
    free(file);
    free(z);
    free(raw);
    return px;
}
