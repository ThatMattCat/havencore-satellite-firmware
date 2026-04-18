/*
 * SPDX-FileCopyrightText: 2026 HavenCore
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "microwakeword_internal.h"

static const char *TAG = "mww_manifest";

static esp_err_t read_file_all(const char *path, char **out_buf, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "fopen(%s) failed", path);
        return ESP_ERR_NOT_FOUND;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024) {
        fclose(fp);
        return ESP_ERR_INVALID_SIZE;
    }
    char *buf = (char *)malloc(sz + 1);
    if (!buf) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }
    size_t n = fread(buf, 1, sz, fp);
    fclose(fp);
    if ((long)n != sz) {
        free(buf);
        return ESP_FAIL;
    }
    buf[sz] = '\0';
    *out_buf = buf;
    *out_len = (size_t)sz;
    return ESP_OK;
}

esp_err_t mww_manifest_load(const char *json_path, mww_manifest_t *out)
{
    if (!json_path || !out) return ESP_ERR_INVALID_ARG;

    char *json_text = NULL;
    size_t json_len = 0;
    esp_err_t ret = read_file_all(json_path, &json_text, &json_len);
    if (ret != ESP_OK) return ret;

    cJSON *root = cJSON_ParseWithLength(json_text, json_len);
    free(json_text);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->probability_cutoff   = 0.9f;
    out->sliding_window_size  = 5;
    out->tensor_arena_size    = 64 * 1024;
    out->feature_step_size_ms = 10;
    out->version              = 1;
    strncpy(out->wake_word, "unnamed", sizeof(out->wake_word) - 1);

    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (cJSON_IsNumber(version)) {
        out->version = (uint16_t)version->valueint;
    }

    cJSON *wake = cJSON_GetObjectItemCaseSensitive(root, "wake_word");
    if (cJSON_IsString(wake) && wake->valuestring) {
        strncpy(out->wake_word, wake->valuestring, sizeof(out->wake_word) - 1);
    }

    /* microWakeWord v2 manifests nest the runtime config under "micro"; v1
     * puts the fields at the root. Accept either. */
    cJSON *micro = cJSON_GetObjectItemCaseSensitive(root, "micro");
    cJSON *src   = cJSON_IsObject(micro) ? micro : root;

    cJSON *cutoff = cJSON_GetObjectItemCaseSensitive(src, "probability_cutoff");
    if (cJSON_IsNumber(cutoff)) {
        out->probability_cutoff = (float)cutoff->valuedouble;
    }
    cJSON *sws = cJSON_GetObjectItemCaseSensitive(src, "sliding_window_size");
    if (cJSON_IsNumber(sws)) {
        out->sliding_window_size = (uint16_t)sws->valueint;
    }
    cJSON *arena = cJSON_GetObjectItemCaseSensitive(src, "tensor_arena_size");
    if (cJSON_IsNumber(arena)) {
        out->tensor_arena_size = (uint32_t)arena->valueint;
    }
    cJSON *step = cJSON_GetObjectItemCaseSensitive(src, "feature_step_size");
    if (cJSON_IsNumber(step)) {
        out->feature_step_size_ms = (uint16_t)step->valueint;
    }

    cJSON_Delete(root);

    if (out->feature_step_size_ms != 10 && out->feature_step_size_ms != 20) {
        ESP_LOGW(TAG, "unusual feature_step_size=%u ms, proceeding anyway",
                 (unsigned)out->feature_step_size_ms);
    }

    ESP_LOGI(TAG,
             "loaded '%s' v%u: cutoff=%.2f, window=%u, arena=%u B, step=%u ms",
             out->wake_word, (unsigned)out->version,
             out->probability_cutoff,
             (unsigned)out->sliding_window_size,
             (unsigned)out->tensor_arena_size,
             (unsigned)out->feature_step_size_ms);
    return ESP_OK;
}
