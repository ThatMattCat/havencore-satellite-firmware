/*
 * SPDX-FileCopyrightText: 2025 HavenCore
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "wake_word.h"

/* Forced ON regardless of NVS. NVS on deployed devices still reflects the
 * old default (0), and the UF2 factory-reset flow isn't currently working,
 * so there's no practical way to flip the key. Revert this once the UF2
 * path is fixed and devices can be re-provisioned. */
bool wake_word_enabled(void)
{
    return true;
}

void wake_word_set_enabled(bool en)
{
    (void)en;
}
