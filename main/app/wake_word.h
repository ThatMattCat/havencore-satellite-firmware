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

/* Stub wake-word gate. Touch-to-talk is the only trigger in the MVP;
 * Porcupine integration lands in a separate commit once the Picovoice
 * ESP32-S3 export is in hand. Default: disabled. */
bool wake_word_enabled(void);
void wake_word_set_enabled(bool en);

#ifdef __cplusplus
}
#endif
