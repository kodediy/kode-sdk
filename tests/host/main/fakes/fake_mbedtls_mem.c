/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_mbedtls_mem.c
 * @brief mbedTLS's allocator hooks, which IDF does not provide on the host.
 */
#include <stdlib.h>

void *esp_mbedtls_mem_calloc(size_t n, size_t size);
void  esp_mbedtls_mem_free(void *ptr);

void *esp_mbedtls_mem_calloc(size_t n, size_t size)
{
    return calloc(n, size);
}

void esp_mbedtls_mem_free(void *ptr)
{
    free(ptr);
}
