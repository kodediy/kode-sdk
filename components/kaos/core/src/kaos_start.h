/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kaos_start.h
 * @brief An Active Object's own queue and stack, and its start on them; every kaos AO's file uses both.
 */
#ifndef KAOS_START_H_
#define KAOS_START_H_

#include "kqp.h"

/** @brief Static storage for an AO: its queue and its stack. */
#define AO_STORAGE(name_, qlen_, stack_bytes_)                     \
    static QEvtPtr     name_##_queue[qlen_];                       \
    static StackType_t name_##_stack[(stack_bytes_) / sizeof(StackType_t)]

/** @brief Name the FreeRTOS task after the AO and start it on the storage AO_STORAGE() gave it. */
#define AO_START(ao_, name_, prio_)                                \
    do {                                                           \
        QActive_setAttr((ao_), TASK_NAME_ATTR, #name_);            \
        QACTIVE_START((ao_), (prio_),                              \
                      name_##_queue, Q_DIM(name_##_queue),         \
                      name_##_stack, sizeof(name_##_stack),        \
                      (void *)0);                                  \
    } while (0)

#endif /* KAOS_START_H_ */
