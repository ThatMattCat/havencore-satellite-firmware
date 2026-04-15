/*
 * SPDX-FileCopyrightText: 2025 HavenCore
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Long-press (2s) anywhere on the screen toggles a translucent overlay
 * with live diagnostics: base URL, RSSI, free PSRAM, state, last error.
 * Plan.md §UI: debug overlay on 2s long-press.
 */
void debug_overlay_init(void);
void debug_overlay_set_last_error(const char *msg);

#ifdef __cplusplus
}
#endif
