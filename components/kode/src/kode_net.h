/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_net.h
 * @brief Shared by kode.h's network calls: HTTP, ping, lookup and UDP. Private to this component.
 */
#ifndef KODE_NET_H_
#define KODE_NET_H_

#include <stdbool.h>

/**
 * @brief Whether the board has an address; the task's error says why when it has not.
 * @return true when it has.
 */
bool kode_net_ready(void);

#endif /* KODE_NET_H_ */
