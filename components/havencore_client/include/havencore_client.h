#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * HavenCore HTTP client — three OpenAI-compatible endpoints:
 *   /v1/audio/transcriptions, /v1/chat/completions, /v1/audio/speech.
 *
 * All calls are synchronous request/response over HTTP (no TLS, no auth —
 * assumed trusted LAN). base_url is e.g. "http://havencore.local" (no
 * trailing slash).
 */

/*
 * POST multipart/form-data {file=<wav>, model="whisper-1"} to
 * <base_url>/v1/audio/transcriptions. Writes the transcribed text (UTF-8,
 * NUL-terminated) to text_out. Returns ESP_OK on 2xx + parseable JSON with a
 * non-empty "text" field; text_out is left empty on ESP_OK + empty-speech.
 */
esp_err_t havencore_stt(const char *base_url,
                        const uint8_t *wav, size_t wav_len,
                        char *text_out, size_t text_out_sz);

/*
 * POST application/json with a single user-role message to
 * <base_url>/v1/chat/completions. Agent sessions are server-side (180s
 * idle), so callers MUST NOT rebuild history here. Writes
 * choices[0].message.content (NUL-terminated) to reply_out.
 */
esp_err_t havencore_chat(const char *base_url,
                         const char *user_text,
                         char *reply_out, size_t reply_out_sz);

/*
 * POST application/json {input, model:"tts-1", voice, response_format:"wav"}
 * to <base_url>/v1/audio/speech. The server ALWAYS returns WAV 16kHz/16-bit
 * mono regardless of response_format — do not trust response_format.
 *
 * Allocates *wav_out in PSRAM and writes the raw WAV body into it; caller
 * owns the buffer and must free() it. *wav_len_out receives the byte count.
 */
esp_err_t havencore_tts(const char *base_url,
                        const char *voice,
                        const char *input_text,
                        uint8_t **wav_out, size_t *wav_len_out);

/*
 * GET <base_url><path>. Returns ESP_OK if the server responds with any 2xx.
 * Used for boot-time health probes against /api/status, /api/tts/health,
 * /api/stt/health. Non-fatal: caller decides how to react.
 */
esp_err_t havencore_get_ok(const char *base_url, const char *path);

/*
 * Derive a stable per-device identity from the Wi-Fi STA MAC and cache it for
 * later use as the `X-Session-Id` header value (e.g. "selene-a4b2"). Must be
 * called once after NVS/network init and before any havencore_* request.
 * Safe to call multiple times; only the first call has effect.
 */
void havencore_client_init_session_id(void);

/*
 * Set the user-visible device name used as the `X-Device-Name` header value
 * on subsequent requests. Safe to call repeatedly (e.g. when the user edits
 * the name from Settings). Pass an empty string to suppress the header.
 */
void havencore_client_set_device_name(const char *name);

#ifdef __cplusplus
}
#endif
