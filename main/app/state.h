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

typedef enum {
    SAT_STATE_IDLE = 0,
    SAT_STATE_LISTENING,
    SAT_STATE_UPLOADING,
    SAT_STATE_THINKING,
    SAT_STATE_SPEAKING,
    SAT_STATE_ERROR,
    SAT_STATE_UPDATING,
} sat_state_t;

void sat_state_init(void);
void sat_state_set(sat_state_t s);
sat_state_t sat_state_get(void);
const char *sat_state_name(sat_state_t s);

#ifdef __cplusplus
}
#endif
