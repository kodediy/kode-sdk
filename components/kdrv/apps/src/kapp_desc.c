/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kapp_desc.c
 * @brief Reading a manifest out of an image prefix.
 */
#include "kapp_desc.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "kapp_cat.h"

/* The arithmetic the whole layout rests on: the manifest sits immediately
 * behind IDF's own descriptor, with no gap. kapp_desc.h's KAPP_DESC_OFF is
 * declared as exactly this sum in prose; this checks it in bytes, at compile
 * time, so a change to either offset - or to esp_app_desc_t's own size -
 * cannot silently point kapp_desc_parse() at the wrong place. */
_Static_assert(KAPP_DESC_OFF == (KAPP_IDF_DESC_OFF + sizeof(esp_app_desc_t)),
               "the manifest sits immediately behind IDF's descriptor");

/* The two sizes kapp_desc.h names for the build date and time are IDF's own
 * array sizes; this keeps them so. */
_Static_assert(KAPP_IDF_NAME_MAX
               == sizeof(((esp_app_desc_t *)NULL)->project_name),
               "KAPP_IDF_NAME_MAX is the size of esp_app_desc_t.project_name");
_Static_assert(KAPP_IDF_DATE_MAX == sizeof(((esp_app_desc_t *)NULL)->date),
               "KAPP_IDF_DATE_MAX is the size of esp_app_desc_t.date");
_Static_assert(KAPP_IDF_TIME_MAX == sizeof(((esp_app_desc_t *)NULL)->time),
               "KAPP_IDF_TIME_MAX is the size of esp_app_desc_t.time");

/**
 * @brief True when @p s holds a terminator inside its @p max bytes.
 * @param s   The array to inspect; never NULL.
 * @param max Its length in bytes.
 * @return true when a NUL exists within the array.
 */
static bool str_bounded(const char *s, size_t max)
{
    return (memchr(s, '\0', max) != NULL);
}

kode_status_t kapp_desc_parse(const uint8_t *prefix, size_t len,
                              kapp_desc_t *out)
{
    if ( (NULL == prefix) || (NULL == out) || (KAPP_PREFIX_LEN > len) )
    {
        return KODE_ERROR;
    }

    kapp_desc_t d;
    (void)memcpy(&d, &prefix[KAPP_DESC_OFF], sizeof(d));

    if ( KAPP_DESC_MAGIC != d.magic )
    {
        return KODE_ERROR;   /* absent: no manifest was ever written */
    }
    if ( !str_bounded(d.name, sizeof(d.name)) )
    {
        return KODE_ERROR;   /* truncated or hostile: never read past the array */
    }

    char cat[KAPP_CATEGORY_MAX];

    /* Both versions put magic and version in the same first eight bytes, so
     * the version can be read before deciding how much to trust. */
    if ( 1U == d.version )
    {
        /* A v1 category is one item in a 16-byte field. Copy it into the
         * wide field and let the normaliser do the rest; a v1 list is a
         * list of one. The struct is 88 bytes, so bound this read by that -
         * never by sizeof(kapp_desc_t) - or it walks into whatever follows. */
        char one[KAPP_CATEGORY_ITEM_MAX];
        (void)memcpy(one, &prefix[KAPP_DESC_OFF + 8U + KAPP_NAME_MAX],
                     sizeof(one));
        if ( memchr(one, '\0', sizeof(one)) == NULL )
        {
            return KODE_ERROR;          /* unterminated: refuse the image */
        }
        kapp_cat_normalise(one, cat, sizeof(cat));
    }
    else if ( KAPP_DESC_VERSION == d.version )
    {
        if ( memchr(d.category, '\0', sizeof(d.category)) == NULL )
        {
            return KODE_ERROR;   /* truncated or hostile: never read past the array */
        }
        kapp_cat_normalise(d.category, cat, sizeof(cat));
    }
    else
    {
        return KODE_ERROR;              /* from the future: do not guess */
    }

    /* An empty cat[] is a manifest that names no category: the name still
     * holds, and the reader files the app as it files an unmanifested one. */
    out->magic   = d.magic;
    out->version = d.version;
    (void)memcpy(out->name, d.name, sizeof(out->name));
    /* snprintf, not memcpy: kapp_cat_normalise() only wrote cat[] up to its
     * terminator, so a raw memcpy of the whole array would carry
     * indeterminate stack bytes past it into out->category (MISRA 9.1). */
    (void)snprintf(out->category, sizeof(out->category), "%s", cat);
    (void)memset(out->reserved, 0, sizeof(out->reserved));

    return KODE_OK;
}

/* Deliberately its own read rather than a helper shared with
 * kdrv_apps_desc(): that one fills five fields of kdrv_slot_desc_t from an
 * already-open partition, this one fills the caller's buffers from a
 * caller-owned prefix, and the only way to share the extraction would be a
 * function that takes or returns esp_app_desc_t. Putting that signature
 * anywhere kapp_desc.h can see it would pull esp_app_desc.h into every app
 * that includes kode_sys.h, to save a few lines neither caller repeats often. */
kode_status_t kapp_project_name(const uint8_t *prefix, size_t len,
                                char *out, size_t max)
{
    return kapp_idf_desc(prefix, len, out, max, NULL, 0U, NULL, 0U, NULL, 0U);
}

kode_status_t kapp_idf_desc(const uint8_t *prefix, size_t len,
                            char *project, size_t project_max,
                            char *version, size_t version_max,
                            char *build_date, size_t build_date_max,
                            char *build_time, size_t build_time_max)
{
    if ( (NULL != project) && (0U < project_max) )
    {
        project[0] = '\0';
    }
    if ( (NULL != version) && (0U < version_max) )
    {
        version[0] = '\0';
    }
    if ( (NULL != build_date) && (0U < build_date_max) )
    {
        build_date[0] = '\0';
    }
    if ( (NULL != build_time) && (0U < build_time_max) )
    {
        build_time[0] = '\0';
    }
    if ( (NULL == prefix) || ((KAPP_IDF_DESC_OFF + sizeof(esp_app_desc_t)) > len) )
    {
        return KODE_ERROR;
    }

    esp_app_desc_t desc;
    (void)memcpy(&desc, &prefix[KAPP_IDF_DESC_OFF], sizeof(desc));
    if ( ESP_APP_DESC_MAGIC_WORD != desc.magic_word )
    {
        return KODE_ERROR;   /* no IDF descriptor at this offset */
    }

    if ( (NULL != project) && (0U < project_max) )
    {
        (void)snprintf(project, project_max, "%.*s",
                       (int)sizeof(desc.project_name), desc.project_name);
    }
    if ( (NULL != version) && (0U < version_max) )
    {
        (void)snprintf(version, version_max, "%.*s",
                       (int)sizeof(desc.version), desc.version);
    }
    /* The precision is the field's own size, so a date or time that fills
     * its array with no terminator is cut there and not read past. */
    if ( (NULL != build_date) && (0U < build_date_max) )
    {
        (void)snprintf(build_date, build_date_max, "%.*s",
                       (int)sizeof(desc.date), desc.date);
    }
    if ( (NULL != build_time) && (0U < build_time_max) )
    {
        (void)snprintf(build_time, build_time_max, "%.*s",
                       (int)sizeof(desc.time), desc.time);
    }
    return KODE_OK;
}

kode_status_t kapp_image_chip(const uint8_t *prefix, size_t len,
                              uint16_t *chip_id)
{
    if ( (NULL == prefix) || (NULL == chip_id) || (KAPP_IMAGE_HDR_LEN > len)
        || (KAPP_IMAGE_MAGIC != prefix[0]) )
    {
        return KODE_ERROR;
    }
    /* Little-endian, two bytes, at the offset the header puts it. */
    *chip_id = (uint16_t)((uint16_t)prefix[KAPP_IMAGE_CHIP_OFF]
                          | ((uint16_t)prefix[KAPP_IMAGE_CHIP_OFF + 1U] << 8));
    return KODE_OK;
}
