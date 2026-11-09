/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_http.h
 * @brief Shared by kode.h's HTTP calls and the task's end. Private to this component.
 */
#ifndef KODE_HTTP_H_
#define KODE_HTTP_H_

/** @brief A task's last answer's text and status. */
struct kode_http_state;

/**
 * @brief Free an HTTP state and its body block.
 * @param s The state; NULL does nothing.
 */
void kode_http_state_free(struct kode_http_state *s);

#endif /* KODE_HTTP_H_ */
