/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kqp_bsp.c
 * @brief What QP needs from the machine: an assert hook and lifecycle.
 */
#include "kqp_bsp.h"

#include "qpc.h"
#include "khal_system.h"

/**
 * @brief QP assert hook. Current policy: report, then panic the whole chip.
 *
 * Panicking is a safe policy here in a way it was not under the ELF model:
 * the bootloader always brings kodeOS back (otadata is blank while kodeOS
 * runs), so an assert can never strand the device in someone else's
 * firmware - and unlike a plain reset, IDF's panic handler leaves a coredump
 * behind for `info`'s `coredump=` row and `idf.py coredump-info` to read.
 *
 * khal_panic_print, NOT klog. QP raises its asserts from inside a critical
 * section, and anything that goes through newlib's stdio takes a recursive
 * lock there - which ESP-IDF answers with a bare abort() whose message says
 * nothing about the actual failure. The lock-free path is the difference
 * between reading "QP ASSERT in qf_actq:110" and staring at "abort() was
 * called at PC ...".
 *
 * The offending task's name is part of the message because the two asserts
 * this system actually hits - a full queue (qf_port:130) and a full pool -
 * both name the *victim* and not the culprit: "a queue was full" is only
 * half a bug report until you know who was posting.
 */
Q_NORETURN Q_onError(char const * const module, int_t const id)
{
    const char *who = khal_task_name();
    khal_panic_print("\nQP ASSERT in %s:%d (task '%s') - resetting\n",
                     ((char const *)0 != module) ? module : "?", (int)id,
                     ((const char *)0 != who) ? who : "?");
    khal_panic("QP assert");
}

/* QF lifecycle callbacks. On ESP-IDF the scheduler and the tick are already
 * running when QF_run() is reached, so nothing to do here. */
void QF_onStartup(void)
{
}

void QF_onCleanup(void)
{
}
