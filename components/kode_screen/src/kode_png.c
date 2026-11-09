/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_png.c
 * @brief A PNG file read into RGBA, in the caller's task, with no LVGL.
 */
#include "kode.h"

#include <inttypes.h>
#include <string.h>

#include "khal_system.h"
#include "khal_zlib.h"
#include "kode_screen_priv.h"

/** @brief Bytes of the PNG signature. */
#define PNG_SIG_BYTES 8U

/** @brief Bytes of a chunk's length and type. */
#define PNG_CHUNK_HEAD 8U

/** @brief Bytes of a chunk's checksum, which is not read. */
#define PNG_CHUNK_CRC 4U

/** @brief Bytes a chunk takes around its data. */
#define PNG_CHUNK_FRAME (PNG_CHUNK_HEAD + PNG_CHUNK_CRC)

/** @brief Bytes of the header chunk's data. */
#define PNG_IHDR_BYTES 13U

/** @brief Where the header's fields sit in its data. */
#define PNG_IHDR_WIDTH      0U
#define PNG_IHDR_HEIGHT     4U
#define PNG_IHDR_DEPTH      8U
#define PNG_IHDR_COLOUR     9U
#define PNG_IHDR_COMPRESS  10U
#define PNG_IHDR_FILTER    11U
#define PNG_IHDR_INTERLACE 12U

/** @brief Chunk types, as four bytes read most significant first. */
#define PNG_TYPE_IHDR 0x49484452U
#define PNG_TYPE_PLTE 0x504C5445U
#define PNG_TYPE_IDAT 0x49444154U
#define PNG_TYPE_IEND 0x49454E44U
#define PNG_TYPE_TRNS 0x74524E53U

/** @brief Colour types. */
#define PNG_GRAY       0U
#define PNG_RGB        2U
#define PNG_PALETTE    3U
#define PNG_GRAY_ALPHA 4U
#define PNG_RGBA       6U

/** @brief Row filters; 0 is none. */
#define PNG_FILTER_SUB   1U
#define PNG_FILTER_UP    2U
#define PNG_FILTER_AVG   3U
#define PNG_FILTER_PAETH 4U

/** @brief The IDAT cap: twice the inflated size, a flush allowance per row, some slack. */
#define PNG_STREAM_DOUBLE_SHIFT 1U
#define PNG_ROW_FLUSH_ALLOWANCE 16U
#define PNG_STREAM_SLACK        64U

/** @brief Entries a palette can hold. */
#define PNG_PAL_MAX 256U

/** @brief Bytes of a palette entry: red, green, blue, alpha. */
#define PNG_PAL_ENTRY 4U

/** @brief Bytes of a decoded pixel. */
#define PNG_RGBA_BYTES 4U

/** @brief Bits in a byte. */
#define PNG_BYTE_BITS 8U

/** @brief The largest value of a byte. */
#define PNG_BYTE_MAX 255U

/** @brief Bits of a sample that is two bytes. */
#define PNG_DEPTH_16 16U

/** @brief Alignment of a decoded picture's pixels: what LVGL asks of a draw buffer. */
#define PNG_PIXEL_ALIGN KODE_CANVAS_ALIGN

/** @brief Alignment of the work buffers. */
#define PNG_WORK_ALIGN 16U

/** @brief What a header says and what follows from it. */
typedef struct {
    uint32_t w;        /**< Width in pixels.                                      */
    uint32_t h;        /**< Height.                                               */
    uint32_t depth;    /**< Bits per sample.                                      */
    uint32_t colour;   /**< PNG_GRAY ... PNG_RGBA.                                */
    uint32_t spp;      /**< Samples per pixel.                                    */
    uint32_t stride;   /**< Bytes of a row, without its filter byte.              */
    uint32_t bpp;      /**< Bytes from a pixel to the next for the filters, at least 1. */
    uint32_t pal_n;    /**< Entries the palette holds.                            */
} png_info_t;

/** @brief A chunk: its type, where its data is and how long it is. */
typedef struct {
    uint32_t       type;   /**< PNG_TYPE_*, or anything else.   */
    const uint8_t *data;   /**< The chunk's data.               */
    uint32_t       len;    /**< Its length.                     */
} png_chunk_t;

/** @brief The palette, red, green, blue and alpha of each entry. */
typedef struct {
    uint8_t rgba[PNG_PAL_MAX][PNG_PAL_ENTRY];  /**< The entries.  */
} png_colors_t;

/** @brief A 32-bit number stored most significant byte first. */
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24U) | ((uint32_t)p[1] << 16U) | ((uint32_t)p[2] << 8U)
           | (uint32_t)p[3];
}

/** @brief Whether the file starts with the PNG signature. */
static bool has_signature(const uint8_t *data, uint32_t len)
{
    static const uint8_t sig[PNG_SIG_BYTES] = { 0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU };

    return (NULL != data) && (PNG_SIG_BYTES <= len) && (memcmp(data, sig, PNG_SIG_BYTES) == 0);
}

/**
 * @brief The next chunk of the file, if all of it is there.
 * @param d     The file.
 * @param total Its size.
 * @param pos   In: where the chunk starts; out: where the next one does.
 * @param c     Out: the chunk.
 * @return true when the chunk, with its checksum, lies inside the file.
 */
static bool chunk_next(const uint8_t *d, uint32_t total, uint32_t *pos, png_chunk_t *c)
{
    bool ok = false;

    if ( (*pos <= total) && ((total - *pos) >= PNG_CHUNK_FRAME) )
    {
        const uint32_t len = be32(&d[*pos]);

        if ( (total - *pos - PNG_CHUNK_FRAME) >= len )
        {
            c->type = be32(&d[*pos + 4U]);
            c->data = &d[*pos + PNG_CHUNK_HEAD];
            c->len  = len;
            *pos    = *pos + PNG_CHUNK_FRAME + len;
            ok      = true;
        }
    }
    return ok;
}

/** @brief Whether a depth is one a colour type allows. */
static bool depth_allowed(uint32_t colour, uint32_t depth)
{
    bool ok = false;

    if ( PNG_GRAY == colour )
    {
        ok = (1U == depth) || (2U == depth) || (4U == depth) || (8U == depth) || (PNG_DEPTH_16 == depth);
    }
    else if ( PNG_PALETTE == colour )
    {
        ok = (1U == depth) || (2U == depth) || (4U == depth) || (8U == depth);
    }
    else if ( (PNG_RGB == colour) || (PNG_GRAY_ALPHA == colour) || (PNG_RGBA == colour) )
    {
        ok = (8U == depth) || (PNG_DEPTH_16 == depth);
    }
    else
    {
        ok = false;
    }
    return ok;
}

/** @brief Samples a pixel of a colour type holds. */
static uint32_t samples_of(uint32_t colour)
{
    uint32_t n = 1U;

    if ( PNG_RGB == colour )
    {
        n = 3U;
    }
    else if ( PNG_GRAY_ALPHA == colour )
    {
        n = 2U;
    }
    else if ( PNG_RGBA == colour )
    {
        n = 4U;
    }
    else
    {
        n = 1U;
    }
    return n;
}

kode_png_result_t kode_png_size(const uint8_t *data, uint32_t len, uint32_t *w, uint32_t *h)
{
    kode_png_result_t r = KODE_PNG_OK;

    *w = 0U;
    *h = 0U;
    if ( !has_signature(data, len) )
    {
        r = KODE_PNG_NOT_PNG;
    }
    else if ( ((PNG_SIG_BYTES + PNG_CHUNK_HEAD + PNG_IHDR_BYTES) > len)
             || (be32(&data[PNG_SIG_BYTES]) != PNG_IHDR_BYTES)
             || (be32(&data[PNG_SIG_BYTES + 4U]) != PNG_TYPE_IHDR) )
    {
        r = KODE_PNG_DAMAGED;
    }
    else
    {
        const uint8_t *const ihdr = &data[PNG_SIG_BYTES + PNG_CHUNK_HEAD];

        *w = be32(&ihdr[PNG_IHDR_WIDTH]);
        *h = be32(&ihdr[PNG_IHDR_HEIGHT]);
        if ( (KODE_CANVAS_WIDTH_MAX < *w) || (KODE_CANVAS_HEIGHT_MAX < *h) )
        {
            r = KODE_PNG_TOO_BIG;
        }
        else if ( (0U == *w) || (0U == *h) )
        {
            r = KODE_PNG_DAMAGED;
        }
        else
        {
            /* a PNG no bigger than the glass */
        }
    }
    return r;
}

/**
 * @brief Read and check the header's remaining fields.
 * @param data The file, whose size is already checked.
 * @param w    Width.
 * @param h    Height.
 * @param in   Out: what the header says.
 * @return KODE_PNG_OK, KODE_PNG_DAMAGED, or KODE_PNG_UNSUPPORTED for interlacing.
 */
static kode_png_result_t info_read(const uint8_t *data, uint32_t w, uint32_t h, png_info_t *in)
{
    const uint8_t *const ihdr = &data[PNG_SIG_BYTES + PNG_CHUNK_HEAD];
    kode_png_result_t    r    = KODE_PNG_OK;

    in->w      = w;
    in->h      = h;
    in->depth  = ihdr[PNG_IHDR_DEPTH];
    in->colour = ihdr[PNG_IHDR_COLOUR];
    in->pal_n  = 0U;
    if ( !depth_allowed(in->colour, in->depth) || (0U != ihdr[PNG_IHDR_COMPRESS])
        || (0U != ihdr[PNG_IHDR_FILTER]) || (1U < ihdr[PNG_IHDR_INTERLACE]) )
    {
        r = KODE_PNG_DAMAGED;
    }
    else if ( 1U == ihdr[PNG_IHDR_INTERLACE] )
    {
        r = KODE_PNG_UNSUPPORTED;
    }
    else
    {
        in->spp    = samples_of(in->colour);
        in->stride = ((w * in->spp * in->depth) + (PNG_BYTE_BITS - 1U)) / PNG_BYTE_BITS;
        in->bpp    = (in->spp * in->depth) / PNG_BYTE_BITS;
        if ( 0U == in->bpp )
        {
            in->bpp = 1U;
        }
    }
    return r;
}

/**
 * @brief Walk the chunks after the header: take the palette, count the data.
 * @param d     The file.
 * @param len   Its size.
 * @param in    The header's fields; pal_n is set.
 * @param colors   Out: the palette, for a palette picture.
 * @param idat  Out: bytes the data chunks add up to.
 * @return KODE_PNG_OK when the file ends with its end chunk after at least one
 *         data chunk (and a palette, if it needs one); KODE_PNG_DAMAGED otherwise.
 */
static kode_png_result_t chunks_scan(const uint8_t *d, uint32_t len, png_info_t *in,
                                     png_colors_t *colors, uint32_t *idat)
{
    uint32_t          pos   = PNG_SIG_BYTES + PNG_CHUNK_FRAME + PNG_IHDR_BYTES;
    bool              ended = false;
    bool              bad   = false;
    png_chunk_t       c     = { 0U, NULL, 0U };
    kode_png_result_t r     = KODE_PNG_DAMAGED;

    *idat = 0U;
    while ( !ended && !bad )
    {
        if ( !chunk_next(d, len, &pos, &c) )
        {
            bad = true;
        }
        else if ( PNG_TYPE_IEND == c.type )
        {
            ended = true;
        }
        else if ( PNG_TYPE_IDAT == c.type )
        {
            *idat += c.len;
        }
        else if ( (PNG_TYPE_PLTE == c.type) && (PNG_PALETTE == in->colour) )
        {
            bad = ((c.len % 3U) != 0U) || ((c.len / 3U) > PNG_PAL_MAX);
            for ( uint32_t i = 0U; !bad && ((c.len / 3U) > i); i++ )
            {
                (void)memcpy(colors->rgba[i], &c.data[i * 3U], 3U);
            }
            in->pal_n = c.len / 3U;
        }
        else if ( (PNG_TYPE_TRNS == c.type) && (PNG_PALETTE == in->colour) )
        {
            bad = (PNG_PAL_MAX < c.len);
            for ( uint32_t i = 0U; !bad && (i < c.len); i++ )
            {
                colors->rgba[i][3] = c.data[i];
            }
        }
        else
        {
            /* a chunk the picture does not need */
        }
    }
    if ( bad || (0U == *idat) || ((PNG_PALETTE == in->colour) && (0U == in->pal_n)) )
    {
        ended = false;
    }
    if ( ended )
    {
        r = KODE_PNG_OK;
    }
    return r;
}

/**
 * @brief Copy the data chunks, one after the other, into @p z.
 * @param d   The file, already walked once.
 * @param len Its size.
 * @param z   Room for the sum of the data chunks.
 */
static void idat_gather(const uint8_t *d, uint32_t len, uint8_t *z)
{
    uint32_t    pos  = PNG_SIG_BYTES + PNG_CHUNK_FRAME + PNG_IHDR_BYTES;
    uint32_t    at   = 0U;
    png_chunk_t c    = { 0U, NULL, 0U };

    while ( chunk_next(d, len, &pos, &c) && (PNG_TYPE_IEND != c.type) )
    {
        if ( PNG_TYPE_IDAT == c.type )
        {
            (void)memcpy(&z[at], c.data, c.len);
            at += c.len;
        }
    }
}

/** @brief The Paeth predictor of the PNG specification. */
static uint32_t paeth(int32_t a, int32_t b, int32_t c)
{
    const int32_t p  = (a + b) - c;
    int32_t       pa = p - a;
    int32_t       pb = p - b;
    int32_t       pc = p - c;
    int32_t       pick;

    if ( 0 > pa )
    {
        pa = -pa;
    }
    if ( 0 > pb )
    {
        pb = -pb;
    }
    if ( 0 > pc )
    {
        pc = -pc;
    }
    if ( (pa <= pb) && (pa <= pc) )
    {
        pick = a;
    }
    else if ( pb <= pc )
    {
        pick = b;
    }
    else
    {
        pick = c;
    }
    return (uint32_t)pick;
}

/**
 * @brief Undo a row's filter in place.
 * @param row    The row's bytes, filtered.
 * @param prev   The row above, unfiltered; NULL for the first row.
 * @param in     The header's fields.
 * @param filter The row's filter byte.
 * @return false when the filter is not one of the five.
 */
static bool row_unfilter(uint8_t *row, const uint8_t *prev, const png_info_t *in, uint8_t filter)
{
    bool ok = (PNG_FILTER_PAETH >= filter);

    for ( uint32_t i = 0U; ok && (i < in->stride); i++ )
    {
        uint32_t a = 0U;
        uint32_t b = 0U;
        uint32_t c = 0U;
        uint32_t pred = 0U;

        if ( i >= in->bpp )
        {
            a = row[i - in->bpp];
        }
        if ( NULL != prev )
        {
            b = prev[i];
            if ( i >= in->bpp )
            {
                c = prev[i - in->bpp];
            }
        }
        if ( PNG_FILTER_SUB == filter )
        {
            pred = a;
        }
        else if ( PNG_FILTER_UP == filter )
        {
            pred = b;
        }
        else if ( PNG_FILTER_AVG == filter )
        {
            pred = (a + b) / 2U;
        }
        else if ( PNG_FILTER_PAETH == filter )
        {
            pred = paeth((int32_t)a, (int32_t)b, (int32_t)c);
        }
        else
        {
            pred = 0U;
        }
        row[i] = (uint8_t)(row[i] + pred);
    }
    return ok;
}

/**
 * @brief Sample @p idx of an unfiltered row, as the value it holds.
 *
 * A 16-bit sample keeps its high byte.
 *
 * @param row   The row.
 * @param idx   Which sample, counting across the pixels.
 * @param depth Bits per sample.
 * @return The value, 0 to 255 (up to 2^depth - 1 below eight bits).
 */
static uint32_t sample_get(const uint8_t *row, uint32_t idx, uint32_t depth)
{
    uint32_t v;

    if ( PNG_DEPTH_16 == depth )
    {
        v = row[idx * 2U];
    }
    else if ( PNG_BYTE_BITS == depth )
    {
        v = row[idx];
    }
    else
    {
        const uint32_t bit  = idx * depth;
        const uint32_t from = PNG_BYTE_BITS - depth - (bit % PNG_BYTE_BITS);

        v = ((uint32_t)row[bit / PNG_BYTE_BITS] >> from) & ((1U << depth) - 1U);
    }
    return v;
}

/** @brief A gray sample as eight bits: 1, 2 and 4-bit values spread over 0 to 255. */
static uint8_t gray_scale(uint32_t v, uint32_t depth)
{
    uint32_t out = v;

    if ( PNG_BYTE_BITS > depth )
    {
        out = (v * PNG_BYTE_MAX) / ((1U << depth) - 1U);
    }
    return (uint8_t)out;
}

/**
 * @brief One pixel of a row as RGBA.
 * @param in  The header's fields.
 * @param colors The palette.
 * @param row The unfiltered row.
 * @param x   Which pixel.
 * @param out Four bytes.
 * @return false when a palette index is past the palette.
 */
static bool pixel_get(const png_info_t *in, const png_colors_t *colors, const uint8_t *row,
                      uint32_t x, uint8_t *out)
{
    const uint32_t at = x * in->spp;
    bool           ok = true;

    out[3] = (uint8_t)PNG_BYTE_MAX;
    if ( PNG_PALETTE == in->colour )
    {
        const uint32_t idx = sample_get(row, at, in->depth);

        ok = (idx < in->pal_n);
        if ( ok )
        {
            (void)memcpy(out, colors->rgba[idx], PNG_PAL_ENTRY);
        }
    }
    else if ( (PNG_GRAY == in->colour) || (PNG_GRAY_ALPHA == in->colour) )
    {
        out[0] = gray_scale(sample_get(row, at, in->depth), in->depth);
        out[1] = out[0];
        out[2] = out[0];
        if ( PNG_GRAY_ALPHA == in->colour )
        {
            out[3] = (uint8_t)sample_get(row, at + 1U, in->depth);
        }
    }
    else
    {
        out[0] = (uint8_t)sample_get(row, at, in->depth);
        out[1] = (uint8_t)sample_get(row, at + 1U, in->depth);
        out[2] = (uint8_t)sample_get(row, at + 2U, in->depth);
        if ( PNG_RGBA == in->colour )
        {
            out[3] = (uint8_t)sample_get(row, at + 3U, in->depth);
        }
    }
    return ok;
}

/**
 * @brief Unfilter every row of the inflated data and turn it into RGBA.
 * @param in  The header's fields.
 * @param colors The palette.
 * @param raw The inflated rows, each a filter byte and its bytes; changed in place.
 * @param out Room for w * h pixels of four bytes.
 * @return true when every row's filter and every palette index was good.
 */
static bool rows_convert(const png_info_t *in, const png_colors_t *colors, uint8_t *raw, uint8_t *out)
{
    const uint32_t row_bytes = in->stride + 1U;
    bool           ok        = true;

    for ( uint32_t y = 0U; ok && (y < in->h); y++ )
    {
        uint8_t       *row  = &raw[y * row_bytes];
        const uint8_t *prev = NULL;

        if ( 0U < y )
        {
            prev = &raw[((y - 1U) * row_bytes) + 1U];
        }
        ok = row_unfilter(&row[1], prev, in, row[0]);
        for ( uint32_t x = 0U; ok && (x < in->w); x++ )
        {
            ok = pixel_get(in, colors, &row[1], x, &out[((y * in->w) + x) * PNG_RGBA_BYTES]);
        }
    }
    return ok;
}

/**
 * @brief Inflate the data chunks and convert the rows.
 * @param in   The header's fields.
 * @param colors  The palette.
 * @param z    The data chunks, joined.
 * @param zlen Their size.
 * @param out  Out: the picture on KODE_PNG_OK.
 * @return The result.
 */
static kode_png_result_t pixels_make(const png_info_t *in, const png_colors_t *colors, const uint8_t *z,
                                     uint32_t zlen, kode_png_t *out)
{
    const uint32_t raw_n = in->h * (in->stride + 1U);
    kode_png_result_t r  = KODE_PNG_NO_MEMORY;
    /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block is the scanlines */
    uint8_t *const raw = (uint8_t *)khal_alloc_psram(PNG_WORK_ALIGN, raw_n);

    if ( NULL != raw )
    {
        size_t        got = 0U;
        const kode_status_t st = khal_zlib_decompress(z, zlen, raw, raw_n, &got);

        if ( KODE_NO_RESOURCES == st )
        {
            r = KODE_PNG_NO_MEMORY;
        }
        else if ( (KODE_OK != st) || (got != raw_n) )
        {
            r = KODE_PNG_DAMAGED;
        }
        else
        {
            /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block is the pixels */
            uint8_t *const rgba = (uint8_t *)khal_alloc_psram(PNG_PIXEL_ALIGN,
                                                              (size_t)in->w * in->h * PNG_RGBA_BYTES);

            if ( NULL == rgba )
            {
                r = KODE_PNG_NO_MEMORY;
            }
            else if ( rows_convert(in, colors, raw, rgba) )
            {
                out->w    = in->w;
                out->h    = in->h;
                out->rgba = rgba;
                r         = KODE_PNG_OK;
            }
            else
            {
                khal_free(rgba);
                r = KODE_PNG_DAMAGED;
            }
        }
        khal_free(raw);
    }
    return r;
}

/**
 * @brief The most IDAT bytes worth copying: a memory cap, not a zlib bound.
 * @param raw_n The inflated size.
 * @param rows  The picture's rows, each of which an encoder may flush.
 * @return The cap.
 */
static uint32_t zlib_cap(uint32_t raw_n, uint32_t rows)
{
    return (raw_n << PNG_STREAM_DOUBLE_SHIFT) + (rows * PNG_ROW_FLUSH_ALLOWANCE) + PNG_STREAM_SLACK;
}

kode_png_result_t kode_png_decode(const uint8_t *data, uint32_t len, kode_png_t *out)
{
    uint32_t          w = 0U;
    uint32_t          h = 0U;
    kode_png_result_t r = kode_png_size(data, len, &w, &h);
    png_info_t        in;
    png_colors_t      colors;
    uint32_t          zlen = 0U;

    out->w    = 0U;
    out->h    = 0U;
    out->rgba = NULL;
    if ( KODE_PNG_TOO_BIG == r )
    {
        out->w = w;
        out->h = h;
    }
    if ( KODE_PNG_OK == r )
    {
        r = info_read(data, w, h, &in);
    }
    if ( KODE_PNG_OK == r )
    {
        (void)memset(&colors, 0, sizeof(colors));
        for ( uint32_t i = 0U; PNG_PAL_MAX > i; i++ )
        {
            colors.rgba[i][3] = (uint8_t)PNG_BYTE_MAX;
        }
        r = chunks_scan(data, len, &in, &colors, &zlen);
    }
    if ( (KODE_PNG_OK == r) && (zlib_cap(in.h * (in.stride + 1U), in.h) < zlen) )
    {
        /* more than a picture of this size can need: not worth a copy */
        r = KODE_PNG_DAMAGED;
    }
    if ( KODE_PNG_OK == r )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block is the joined data */
        uint8_t *const z = (uint8_t *)khal_alloc_psram(PNG_WORK_ALIGN, zlen);

        if ( NULL == z )
        {
            r = KODE_PNG_NO_MEMORY;
        }
        else
        {
            idat_gather(data, len, z);
            r = pixels_make(&in, &colors, z, zlen, out);
            khal_free(z);
        }
    }
    return r;
}

void kode_png_say(const char *path, kode_png_result_t r, uint32_t w, uint32_t h)
{
    if ( KODE_PNG_NOT_PNG == r )
    {
        kode_err_set("%s: not a PNG", path);
    }
    else if ( KODE_PNG_TOO_BIG == r )
    {
        kode_err_set("%s: too big for the glass (%" PRIu32 "x%" PRIu32 ")", path, w, h);
    }
    else if ( KODE_PNG_DAMAGED == r )
    {
        kode_err_set("%s: damaged PNG", path);
    }
    else if ( KODE_PNG_UNSUPPORTED == r )
    {
        kode_err_set("%s: interlaced PNGs are not supported", path);
    }
    else
    {
        kode_err_set("%s: out of memory", path);
    }
}

bool kode_png_load(const char *name, const char *path, kode_png_t *out)
{
    uint8_t *file = NULL;
    uint32_t len  = 0U;
    bool     ok   = kode_file_read_owned(name, path, &file, &len);

    out->w    = 0U;
    out->h    = 0U;
    out->rgba = NULL;
    if ( ok )
    {
        const kode_png_result_t r = kode_png_decode(file, len, out);

        ok = (KODE_PNG_OK == r);
        if ( !ok )
        {
            kode_png_say(path, r, out->w, out->h);
        }
        khal_free(file);
    }
    return ok;
}

void kode_png_free(kode_png_t *p)
{
    khal_free(p->rgba);
    p->rgba = NULL;
}

void kode_png_swap_rb(kode_png_t *p)
{
    const size_t n = (size_t)p->w * p->h;

    for ( size_t i = 0U; i < n; i++ )
    {
        uint8_t *const px = &p->rgba[i * PNG_RGBA_BYTES];
        const uint8_t  r  = px[0];

        px[0] = px[2];
        px[2] = r;
    }
}
