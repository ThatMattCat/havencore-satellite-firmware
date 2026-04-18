/*
 * SPDX-FileCopyrightText: 2026 HavenCore
 * SPDX-License-Identifier: CC0-1.0
 */

#include "simple_vad.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "simple_vad";

#define FRAME_SAMPLES   320   /* 20 ms @ 16 kHz */
#define NOISE_INIT      60.0f
#define NOISE_ALPHA_UP  0.02f /* how fast noise floor rises when silent */
#define NOISE_ALPHA_DN  0.005f/* how fast it relaxes back if over-estimated */
#define SPEECH_MARGIN   4.0f  /* speech if rms > noise_floor * margin */
#define RMS_FLOOR       150.0f/* below this, always treat as silence */

static struct {
    int16_t  frame_buf[FRAME_SAMPLES];
    size_t   frame_fill;
    float    noise_floor;
    simple_vad_state_t state;
    bool     floor_locked;
} s;

void simple_vad_reset(void)
{
    memset(&s, 0, sizeof(s));
    s.noise_floor = NOISE_INIT;
    s.state       = SIMPLE_VAD_SILENCE;
}

static float frame_rms(const int16_t *p, size_t n)
{
    uint64_t acc = 0;
    for (size_t i = 0; i < n; ++i) {
        int32_t v = (int32_t)p[i];
        acc += (uint64_t)(v * v);
    }
    return sqrtf((float)acc / (float)n);
}

static void process_frame(void)
{
    float rms = frame_rms(s.frame_buf, FRAME_SAMPLES);

    bool is_speech = (rms > RMS_FLOOR) && (rms > s.noise_floor * SPEECH_MARGIN);
    simple_vad_state_t prev = s.state;

    if (is_speech) {
        s.state = SIMPLE_VAD_SPEECH;
        /* Don't update the noise floor during speech. */
    } else {
        s.state = SIMPLE_VAD_SILENCE;
        /* Update noise floor: fast rise toward current rms (catches loud
         * rooms), slow decay if rms dropped below it. Skipped while the
         * floor is locked (during a listen window) so inter-word pauses
         * can't inflate the threshold. */
        if (!s.floor_locked) {
            if (rms > s.noise_floor) {
                s.noise_floor += NOISE_ALPHA_UP * (rms - s.noise_floor);
            } else {
                s.noise_floor += NOISE_ALPHA_DN * (rms - s.noise_floor);
            }
            if (s.noise_floor < 1.0f) s.noise_floor = 1.0f;
        }
    }

    /* Log state transitions at INFO so wake/endpoint behaviour is
     * observable without flooding — no periodic dump. */
    if (prev != s.state) {
        ESP_LOGI(TAG, "%s  rms=%.0f floor=%.0f thresh=%.0f",
                 s.state == SIMPLE_VAD_SPEECH ? "-> SPEECH" : "-> silence",
                 rms, s.noise_floor, s.noise_floor * SPEECH_MARGIN);
    }
}

void simple_vad_feed(const int16_t *samples, size_t n_samples)
{
    if (!samples || n_samples == 0) return;
    while (n_samples > 0) {
        size_t room = FRAME_SAMPLES - s.frame_fill;
        size_t take = (n_samples < room) ? n_samples : room;
        memcpy(&s.frame_buf[s.frame_fill], samples, take * sizeof(int16_t));
        s.frame_fill += take;
        samples      += take;
        n_samples    -= take;
        if (s.frame_fill == FRAME_SAMPLES) {
            process_frame();
            s.frame_fill = 0;
        }
    }
}

simple_vad_state_t simple_vad_state(void)
{
    return s.state;
}

void simple_vad_set_floor_locked(bool locked)
{
    s.floor_locked = locked;
}
