/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file audio_ao.h
 * @brief The example's own Active Object: a note and a recording.
 */
#ifndef AUDIO_AO_H_
#define AUDIO_AO_H_

#include "kode_sys.h"

/** @brief The singleton, as an opaque QActive. Named AudioDemo because
 *         AO_Audio and Audio_ctor are the SDK's own, and a second AO cannot
 *         answer to either. */
extern QActive * const AO_AudioDemo;

/**
 * @brief Construct it.
 *
 * Call after kode_init() and before AudioDemo_start(). It only initialises
 * memory: nothing is subscribed and nothing is requested until
 * AudioDemo_start() runs the initial transition, in the caller's thread,
 * before the AO's own task exists.
 */
void AudioDemo_ctor(void);

/**
 * @brief Give it a thread, a queue and a stack, and let it run.
 */
void AudioDemo_start(void);

#endif /* AUDIO_AO_H_ */
