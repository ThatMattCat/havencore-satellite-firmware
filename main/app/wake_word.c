/*
 * SPDX-FileCopyrightText: 2025 HavenCore
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "wake_word.h"

static bool g_enabled = true;

bool wake_word_enabled(void)
{
    return g_enabled;
}

void wake_word_set_enabled(bool en)
{
    g_enabled = en;
}
