#include "havencore_client.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "cJSON.h"

static const char *TAG = "havencore";

#define MULTIPART_BOUNDARY "----hc-satellite-0XZ7mK"
#define STT_PATH           "/v1/audio/transcriptions"
#define CHAT_PATH          "/v1/chat/completions"
#define TTS_PATH           "/v1/audio/speech"
#define HTTP_TIMEOUT_MS    30000

static esp_err_t build_url(const char *base_url, const char *path,
                           char *out, size_t out_sz)
{
    int n = snprintf(out, out_sz, "%s%s", base_url, path);
    if (n < 0 || (size_t)n >= out_sz) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* Drain the response body into a fresh PSRAM buffer. *body_out is allocated
 * with capability MALLOC_CAP_SPIRAM; caller must free. NUL-terminates so the
 * buffer is also safe to read as a C string for JSON. */
static esp_err_t read_body(esp_http_client_handle_t client,
                           uint8_t **body_out, size_t *body_len_out)
{
    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP status %d", status);
        return ESP_FAIL;
    }

    /* content_length is -1 for chunked; grow on demand in that case. */
    size_t cap = (content_length > 0) ? (size_t)content_length + 1 : 8192;
    uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    while (1) {
        if (total + 1 >= cap) {
            size_t new_cap = cap * 2;
            uint8_t *bigger = heap_caps_realloc(buf, new_cap, MALLOC_CAP_SPIRAM);
            if (!bigger) {
                free(buf);
                return ESP_ERR_NO_MEM;
            }
            buf = bigger;
            cap = new_cap;
        }
        int r = esp_http_client_read(client, (char *)(buf + total), cap - total - 1);
        if (r < 0) {
            free(buf);
            return ESP_FAIL;
        }
        if (r == 0) {
            break;
        }
        total += r;
    }
    buf[total] = '\0';
    *body_out = buf;
    *body_len_out = total;
    return ESP_OK;
}

esp_err_t havencore_stt(const char *base_url,
                        const uint8_t *wav, size_t wav_len,
                        char *text_out, size_t text_out_sz)
{
    if (!base_url || !wav || !text_out || text_out_sz == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    text_out[0] = '\0';

    char url[192];
    esp_err_t ret = build_url(base_url, STT_PATH, url, sizeof(url));
    if (ret != ESP_OK) return ret;

    /* Assemble multipart body: file part header + WAV + model part. */
    static const char file_part_fmt[] =
        "--" MULTIPART_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n"
        "\r\n";
    static const char model_part[] =
        "\r\n--" MULTIPART_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n"
        "\r\n"
        "whisper-1"
        "\r\n--" MULTIPART_BOUNDARY "--\r\n";

    size_t head_len = strlen(file_part_fmt);
    size_t tail_len = strlen(model_part);
    size_t body_len = head_len + wav_len + tail_len;

    uint8_t *body = heap_caps_malloc(body_len, MALLOC_CAP_SPIRAM);
    if (!body) return ESP_ERR_NO_MEM;
    memcpy(body, file_part_fmt, head_len);
    memcpy(body + head_len, wav, wav_len);
    memcpy(body + head_len + wav_len, model_part, tail_len);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(body); return ESP_FAIL; }

    esp_http_client_set_header(client, "Content-Type",
        "multipart/form-data; boundary=" MULTIPART_BOUNDARY);

    ret = esp_http_client_open(client, body_len);
    if (ret != ESP_OK) goto cleanup;

    int written = esp_http_client_write(client, (const char *)body, body_len);
    if (written != (int)body_len) { ret = ESP_FAIL; goto cleanup; }

    uint8_t *resp = NULL;
    size_t resp_len = 0;
    ret = read_body(client, &resp, &resp_len);
    if (ret != ESP_OK) goto cleanup;

    cJSON *root = cJSON_ParseWithLength((const char *)resp, resp_len);
    free(resp);
    if (!root) { ret = ESP_ERR_INVALID_RESPONSE; goto cleanup; }

    cJSON *text = cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(text) && text->valuestring) {
        strlcpy(text_out, text->valuestring, text_out_sz);
    }
    cJSON_Delete(root);

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(body);
    return ret;
}

esp_err_t havencore_chat(const char *base_url,
                         const char *user_text,
                         char *reply_out, size_t reply_out_sz)
{
    if (!base_url || !user_text || !reply_out || reply_out_sz == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    reply_out[0] = '\0';

    char url[192];
    esp_err_t ret = build_url(base_url, CHAT_PATH, url, sizeof(url));
    if (ret != ESP_OK) return ret;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "selene");
    cJSON_AddBoolToObject(root, "stream", false);
    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", user_text);
    cJSON_AddItemToArray(messages, msg);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(body); return ESP_FAIL; }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    size_t body_len = strlen(body);

    ret = esp_http_client_open(client, body_len);
    if (ret != ESP_OK) goto cleanup;

    int written = esp_http_client_write(client, body, body_len);
    if (written != (int)body_len) { ret = ESP_FAIL; goto cleanup; }

    uint8_t *resp = NULL;
    size_t resp_len = 0;
    ret = read_body(client, &resp, &resp_len);
    if (ret != ESP_OK) goto cleanup;

    cJSON *r = cJSON_ParseWithLength((const char *)resp, resp_len);
    free(resp);
    if (!r) { ret = ESP_ERR_INVALID_RESPONSE; goto cleanup; }

    cJSON *choices = cJSON_GetObjectItem(r, "choices");
    cJSON *first = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(first, "message");
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (cJSON_IsString(content) && content->valuestring) {
        strlcpy(reply_out, content->valuestring, reply_out_sz);
    } else {
        ret = ESP_ERR_INVALID_RESPONSE;
    }
    cJSON_Delete(r);

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(body);
    return ret;
}

esp_err_t havencore_tts(const char *base_url,
                        const char *voice,
                        const char *input_text,
                        uint8_t **wav_out, size_t *wav_len_out)
{
    if (!base_url || !voice || !input_text || !wav_out || !wav_len_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *wav_out = NULL;
    *wav_len_out = 0;

    char url[192];
    esp_err_t ret = build_url(base_url, TTS_PATH, url, sizeof(url));
    if (ret != ESP_OK) return ret;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "input", input_text);
    cJSON_AddStringToObject(root, "model", "tts-1");
    cJSON_AddStringToObject(root, "voice", voice);
    cJSON_AddStringToObject(root, "response_format", "wav");
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(body); return ESP_FAIL; }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    size_t body_len = strlen(body);

    ret = esp_http_client_open(client, body_len);
    if (ret != ESP_OK) goto cleanup;

    int written = esp_http_client_write(client, body, body_len);
    if (written != (int)body_len) { ret = ESP_FAIL; goto cleanup; }

    ret = read_body(client, wav_out, wav_len_out);

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(body);
    return ret;
}
