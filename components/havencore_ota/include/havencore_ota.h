/*
 * SPDX-FileCopyrightText: 2026 HavenCore
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Lightweight state-machine binding so havencore_ota stays decoupled from
 * main. Caller registers these once at boot. All callbacks may be NULL,
 * but is_idle is required for the push path's IDLE-only guard. */
typedef struct {
    bool (*is_idle)(void);              /* true if it's safe to start an OTA */
    void (*set_updating)(void);         /* called just before write begins */
    void (*set_idle)(void);             /* called on OTA failure */
    void (*set_error)(const char *msg); /* called on OTA failure (debug overlay) */
} havencore_ota_state_iface_t;

/* Register the state interface. Safe to call once at boot. */
void havencore_ota_set_state_iface(const havencore_ota_state_iface_t *iface);

/* Start a tiny HTTP server (port 80) exposing dev OTA endpoints:
 *   POST /dev/ota    — body is the raw firmware binary; reboots on success
 *   GET  /dev/version — JSON describing the running app
 *
 * The server runs for the lifetime of the app. Designed for a trusted LAN
 * dev loop; no auth. */
esp_err_t havencore_ota_dev_server_start(void);

/* Pull firmware from `url` (plain HTTP allowed) using esp_https_ota.
 * Returns ESP_OK on success and immediately reboots; returns an error
 * if the download or write failed. The caller is responsible for state
 * transitions; this function calls the state iface itself if registered. */
esp_err_t havencore_ota_pull(const char *url);

/* Returns the running app's version string (esp_app_desc_t::version,
 * which IDF derives from `git describe --always --tags --dirty` by
 * default). Never NULL — falls back to "?" if the descriptor is missing. */
const char *havencore_ota_running_version(void);

/* Fetch the version sidecar at `json_url` and extract the `version`
 * field into `out` (NUL-terminated, truncated if `out_len` is too
 * small). Returns ESP_OK on success, ESP_ERR_NOT_FOUND if the URL 404s
 * or the JSON has no `version` field, other esp_err_t on transport
 * failure. Used to skip a redundant OTA pull when the server's bin
 * already matches the running version. */
esp_err_t havencore_ota_fetch_remote_version(const char *json_url,
                                              char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
