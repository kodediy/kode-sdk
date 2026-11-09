/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file lwip/ip_addr.h
 * @brief The address shape ping_sock.h needs, for a host build. See sockets.h.
 */
#ifndef KODE_HOST_SHIM_LWIP_IP_ADDR_H_
#define KODE_HOST_SHIM_LWIP_IP_ADDR_H_

#include <stdint.h>

#define IPADDR_TYPE_V4   0U
#define IPADDR_TYPE_V6   6U
#define IPADDR_TYPE_ANY  46U

#define IP_DEFAULT_TTL   64

typedef struct { uint32_t addr; } ip4_addr_t;
typedef struct { uint32_t addr[4]; uint8_t zone; } ip6_addr_t;

typedef struct {
    union {
        ip6_addr_t ip6;
        ip4_addr_t ip4;
    } u_addr;
    uint8_t type;
} ip_addr_t;

extern const ip_addr_t kode_shim_ip_any;
#define IP_ANY_TYPE  (&kode_shim_ip_any)

#endif /* KODE_HOST_SHIM_LWIP_IP_ADDR_H_ */
