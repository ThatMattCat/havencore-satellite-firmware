/*
 * SPDX-FileCopyrightText: 2026 HavenCore
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "havencore_ota.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "cJSON.h"

static const char *TAG = "havencore_ota";

/* Receive buffer size for streaming the POST body into flash. 4 KiB
 * matches the flash sector size and keeps us within stack budgets. */
#define OTA_RECV_CHUNK 4096

static httpd_handle_t s_server = NULL;
static havencore_ota_state_iface_t s_iface = {0};

void havencore_ota_set_state_iface(const havencore_ota_state_iface_t *iface)
{
    if (iface) {
        s_iface = *iface;
    } else {
        memset(&s_iface, 0, sizeof(s_iface));
    }
}

static bool can_start_update(void)
{
    return s_iface.is_idle ? s_iface.is_idle() : true;
}

static void mark_updating(void)
{
    if (s_iface.set_updating) s_iface.set_updating();
}

static void mark_failed(const char *msg)
{
    if (s_iface.set_error) s_iface.set_error(msg);
    if (s_iface.set_idle) s_iface.set_idle();
}

/* POST /dev/ota — body = raw firmware image. Streams chunks straight
 * into the next OTA partition. On success: set boot, send 200, reboot. */
static esp_err_t dev_ota_post_handler(httpd_req_t *req)
{
    if (!can_start_update()) {
        ESP_LOGW(TAG, "POST /dev/ota refused: device not idle");
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "device busy\n");
        return ESP_OK;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        ESP_LOGE(TAG, "no next OTA partition");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "no next OTA partition");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA push -> %s @ 0x%08" PRIx32 " (%" PRIu32 " bytes)",
             target->label, target->address, target->size);

    mark_updating();

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        mark_failed("ota: begin");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "ota begin failed");
        return ESP_FAIL;
    }

    char *buf = malloc(OTA_RECV_CHUNK);
    if (!buf) {
        esp_ota_abort(handle);
        mark_failed("ota: oom");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    size_t total = 0;
    while (true) {
        int got = httpd_req_recv(req, buf, OTA_RECV_CHUNK);
        if (got == 0) {
            break;  /* end of body */
        }
        if (got < 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;  /* retry */
            }
            ESP_LOGE(TAG, "httpd_req_recv: %d", got);
            esp_ota_abort(handle);
            free(buf);
            mark_failed("ota: recv");
            return ESP_FAIL;
        }
        err = esp_ota_write(handle, buf, got);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            free(buf);
            mark_failed("ota: write");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "ota write failed");
            return ESP_FAIL;
        }
        total += got;
    }
    free(buf);

    if (total == 0) {
        ESP_LOGE(TAG, "empty body");
        esp_ota_abort(handle);
        mark_failed("ota: empty body");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        mark_failed("ota: end");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "ota end failed (image invalid?)");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        mark_failed("ota: set_boot");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "set_boot_partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA OK (%u bytes) -> rebooting into %s",
             (unsigned)total, target->label);

    httpd_resp_set_type(req, "text/plain");
    char ok[64];
    snprintf(ok, sizeof(ok), "ok %u bytes -> %s, rebooting\n",
             (unsigned)total, target->label);
    httpd_resp_sendstr(req, ok);

    /* Give the response time to flush before the network stack dies. */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;  /* unreachable */
}

/* GET /dev/version — JSON: {project, version, idf, partition} */
static esp_err_t dev_version_get_handler(httpd_req_t *req)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    char body[256];
    int n = snprintf(body, sizeof(body),
        "{\"project\":\"%s\",\"version\":\"%s\",\"idf\":\"%s\","
        "\"partition\":\"%s\",\"offset\":%" PRIu32 "}\n",
        desc ? desc->project_name : "?",
        desc ? desc->version : "?",
        desc ? desc->idf_ver : "?",
        running ? running->label : "?",
        running ? running->address : 0);
    if (n < 0 || n >= (int)sizeof(body)) {
        body[sizeof(body) - 1] = '\0';
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

esp_err_t havencore_ota_dev_server_start(void)
{
    if (s_server) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    /* OTA push body can run for tens of seconds on slow Wi-Fi; bump the
     * idle/recv timeouts so the server doesn't drop us mid-stream. */
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 4;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    static const httpd_uri_t ota_uri = {
        .uri      = "/dev/ota",
        .method   = HTTP_POST,
        .handler  = dev_ota_post_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t ver_uri = {
        .uri      = "/dev/version",
        .method   = HTTP_GET,
        .handler  = dev_version_get_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &ota_uri);
    httpd_register_uri_handler(s_server, &ver_uri);

    ESP_LOGI(TAG, "dev OTA server up: POST /dev/ota, GET /dev/version");
    return ESP_OK;
}

const char *havencore_ota_running_version(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    return (desc && desc->version[0]) ? desc->version : "?";
}

/* Sidecar JSON is tiny (~100 bytes); cap at 1 KiB and refuse anything
 * larger to keep the path bounded even if the server misbehaves. */
#define SIDECAR_MAX_BYTES 1024

esp_err_t havencore_ota_fetch_remote_version(const char *json_url,
                                              char *out, size_t out_len)
{
    if (!json_url || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = json_url,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_FAIL;
    char *body = NULL;
    int body_len = 0;

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sidecar open failed: %s", esp_err_to_name(err));
        ret = err;
        goto cleanup;
    }

    int content_len = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    if (status != 200) {
        ESP_LOGI(TAG, "sidecar HTTP %d (no version pin)", status);
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    /* content_len < 0 means chunked / unknown — read until EOF up to cap. */
    int cap = (content_len > 0 && content_len < SIDECAR_MAX_BYTES)
              ? content_len + 1
              : SIDECAR_MAX_BYTES + 1;
    body = malloc(cap);
    if (!body) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    while (body_len < cap - 1) {
        int got = esp_http_client_read(cli, body + body_len,
                                       cap - 1 - body_len);
        if (got < 0) {
            ret = ESP_FAIL;
            goto cleanup;
        }
        if (got == 0) break;
        body_len += got;
    }
    body[body_len] = '\0';

    cJSON *root = cJSON_ParseWithLength(body, body_len);
    if (!root) {
        ESP_LOGW(TAG, "sidecar JSON parse failed");
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (!cJSON_IsString(ver) || !ver->valuestring) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "sidecar missing 'version' string");
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    strlcpy(out, ver->valuestring, out_len);
    cJSON_Delete(root);
    ret = ESP_OK;

cleanup:
    if (body) free(body);
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return ret;
}

esp_err_t havencore_ota_pull(const char *url)
{
    if (!url || !*url) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!can_start_update()) {
        ESP_LOGW(TAG, "pull refused: device not idle");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "OTA pull from %s", url);
    mark_updating();

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 60000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota: %s", esp_err_to_name(err));
        mark_failed("ota: pull failed");
        return err;
    }

    ESP_LOGI(TAG, "OTA pull OK -> rebooting");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;  /* unreachable */
}
