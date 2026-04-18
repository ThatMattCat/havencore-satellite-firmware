#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * HavenCore HTTP client — three endpoints:
 *   /v1/audio/transcriptions (OpenAI-compatible STT shim),
 *   /api/chat (first-party chat with server-owned session history),
 *   /v1/audio/speech (OpenAI-compatible TTS shim).
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
 * POST application/json {"message": "<user text>"} to <base_url>/api/chat.
 * /api/chat owns per-session history (180s rolling idle timeout, LRU-pooled);
 * callers MUST NOT rebuild history here. Session identity is carried in the
 * X-Session-Id header (see havencore_client_set_session_id); the server may
 * rotate the id on eviction by echoing a new value in the response header,
 * which the client captures and surfaces via the session-changed callback.
 * Writes top-level response.response (NUL-terminated) to reply_out.
 */
esp_err_t havencore_chat(const char *base_url,
                         const char *user_text,
                         char *reply_out, size_t reply_out_sz);

/*
 * POST application/json {input, model:"tts-1", voice, response_format:"wav"}
 * to <base_url>/v1/audio/speech. The server honors response_format and
 * advertises the encoding via Content-Type; the playback path inspects
 * Content-Type to route the body.
 *
 * Allocates *wav_out in PSRAM and writes the raw response body into it;
 * caller owns the buffer and must free() it. *wav_len_out receives the byte
 * count.
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
 * Set the session id used as the `X-Session-Id` header value on subsequent
 * requests. The id is an NVS-persisted random hex blob minted at first boot;
 * the server may rotate it (see havencore_client_set_session_changed_cb).
 * Safe to call repeatedly. Pass NULL or "" to suppress the header.
 */
void havencore_client_set_session_id(const char *id);

/*
 * Register a callback invoked when the server returns a new X-Session-Id in
 * the /api/chat response header. Fires on the HTTP task context; keep the
 * handler cheap (a single NVS write is fine). Overwrites any prior callback.
 */
void havencore_client_set_session_changed_cb(void (*cb)(const char *new_id));

/*
 * Set the user-visible device name used as the `X-Device-Name` header value
 * on subsequent requests. Safe to call repeatedly (e.g. when the user edits
 * the name from Settings). Pass an empty string to suppress the header.
 */
void havencore_client_set_device_name(const char *name);

#ifdef __cplusplus
}
#endif
