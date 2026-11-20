/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_http_header.h
 * @brief kode_http_header_t, declared once for kode.h and kode_sys.h.
 */
#ifndef KODE_HTTP_HEADER_H_
#define KODE_HTTP_HEADER_H_

/**
 * @brief One request header of the app's own, for the HTTP calls' @c headers.
 *
 * A list ends at a header whose @c name is NULL:
 * @code
 * static const kode_http_header_t hdrs[] = {
 *     { "Authorization", "Bearer abc123" },
 *     { "X-Api-Key",     "k-42" },
 *     { NULL, NULL },
 * };
 * @endcode
 */
typedef struct {
    const char *name;   /**< "Authorization", "X-Api-Key"...; NULL ends the list. */
    const char *value;  /**< Sent as written; "" sends an empty one.            */
} kode_http_header_t;

/** @brief Headers of the app's own one request sends at most. */
#define KODE_HTTP_HEADERS_MAX  8U
/** @brief Bytes of one such header's name and value together, at most. */
#define KODE_HTTP_HEADER_MAX   4000U

#endif /* KODE_HTTP_HEADER_H_ */
