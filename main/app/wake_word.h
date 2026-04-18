/*
 * SPDX-FileCopyrightText: 2025 HavenCore
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime gate for the microWakeWord ("Hey Selene") detector. When true,
 * audio_detect_task will emit a LISTENING trigger on mww_poll_detected().
 * Touch-to-talk is always available regardless of this flag. Stub starts
 * false at boot and is flipped by main.c from the NVS `wake_enabled` key;
 * that key defaults to 1 (armed) when absent, so the effective default
 * on a normally-provisioned device is on. */
bool wake_word_enabled(void);
void wake_word_set_enabled(bool en);

#ifdef __cplusplus
}
#endif
