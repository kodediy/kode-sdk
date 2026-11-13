/*
 * kodeOS - QP/C compile-time configuration
 *
 * Included by qp_port.h. Keep this minimal: kodeOS-level settings (priority
 * bands, pool sizes) live in kqp/include/kqp_config.h.
 */
#ifndef QP_CONFIG_H_
#define QP_CONFIG_H_

#include "sdkconfig.h"  // CONFIG_KQP_AO_WATCH

// The port records every dispatch of every Active Object for the supervisor
// in kqp (see QAoWatchRec in qp_port.h). Set by kqp's Kconfig; off removes the
// record, its two stores per event and the sampling functions.
#ifdef CONFIG_KQP_AO_WATCH
#define QF_AO_WATCH

// The longest step an Active Object may take, in ms. The port times every
// step where it ends and records the ones over this, so none is missed. One
// source: kqp's Kconfig, which the supervisor reads too.
#define QF_AO_STEP_MAX_MS ((uint32_t)CONFIG_KQP_AO_STEP_MAX_MS)
#endif

// Maximum number of active objects in the system (QP priority levels).
// Must not exceed FreeRTOS configMAX_PRIORITIES: qf_port.c turns a breach of
// that into a hard #error rather than a runtime surprise. In this ESP-IDF
// version configMAX_PRIORITIES is not a Kconfig knob at all - FreeRTOSConfig.h
// spells it "#define configMAX_PRIORITIES ( 25 )" outright - so 25 is not a
// suggestion here, it is the ceiling this port can reach without patching
// vendored ESP-IDF, which is out of scope for an AO's priority.
//
// kodeOS's owners had filled 4..15 and 17, with 1, 3 and 16 reserved for
// whoever builds on the SDK - so an owner added after AO_IoExp (17) had
// nowhere to go. Raised from 16 to 25, the full room FreeRTOS leaves, to make
// room for KODE_PRIO_IR at 18 without spending this same fix again for the
// next owner; see kqp_config.h for why 18, of all places, is where AO_Ir
// landed.
//
// This is a change to vendored code's configuration, not just a number: QP's
// own qp.h picks QPSetBits' width from this value (qp.h:398) - uint8_t up to
// 8, uint16_t from 9 to 16, uint32_t above 16 - so crossing 16 widens every
// ready-set bitmask in the framework from 16 bits to 32, here as it would at
// any value above 16. QPSet itself only grows a second word past 32
// (qp.h:415), which 25 does not reach, so this stays a single-word set; the
// width change is the only effect measured here.
//
// One asymmetry the #error above does not catch: QP allows a priority up to
// QF_MAX_ACTIVE inclusive, and qp_port.h maps it straight through
// (FREERTOS_TASK_PRIO adds tskIDLE_PRIORITY, which is 0), while FreeRTOS
// accepts 0..configMAX_PRIORITIES-1. So 25 is the highest this constant may
// take, but 24 is the highest priority an AO may actually ask for: tasks.c
// asserts on 25 and then clamps it to 24, which in a build with NDEBUG means
// two owners sharing one level in silence. Nothing here is near it - AO_Rfid at
// 23 is the top - and the reserve for whoever builds on the SDK stays where
// kqp_config.h puts it.
#define QF_MAX_ACTIVE       25U

// Event pools (small/medium/large/jumbo, the icon tier above jumbo, and wide
// between large and jumbo in an app with long Bluetooth values)
#define QF_MAX_EPOOL        6U

// Single QP tick rate (driven from kbsp via esp_timer)
#define QF_MAX_TICK_RATE    1U

#endif // QP_CONFIG_H_
