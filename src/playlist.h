/* SPDX-License-Identifier: LGPL-2.1-only
 * Copyright (C) 2026 JobDestroyer
 */

#ifndef VIBE_PLAYLIST_H
#define VIBE_PLAYLIST_H

#include "config.h"

typedef struct {
    char path[VIBE_PATH_MAX];
    char title[VIBE_NAME_MAX];
    char artist[VIBE_NAME_MAX];
} PlEntry;

int playlist_is_ext(const char *name);
/* Parse a playlist file into out[0..cap). Returns entry count, 0 on none/fail. */
int playlist_parse(const char *path, PlEntry *out, int cap);

#endif
