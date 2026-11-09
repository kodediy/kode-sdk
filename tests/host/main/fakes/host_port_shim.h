/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file host_port_shim.h
 * @brief Declares the one FreeRTOS port symbol the linux port is missing.
 */
#ifndef HOST_PORT_SHIM_H_
#define HOST_PORT_SHIM_H_

/* Force-included by main/CMakeLists.txt, because klog.c needs it and must not be edited. */
#ifndef xPortCanYield
int xPortCanYield(void);
#endif

#endif /* HOST_PORT_SHIM_H_ */
