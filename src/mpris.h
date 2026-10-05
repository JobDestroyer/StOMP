/* SPDX-License-Identifier: LGPL-2.1-only
 * Copyright (C) 2026 JobDestroyer
 */

#ifndef VIBE_MPRIS_H
#define VIBE_MPRIS_H

struct App;

int mpris_init(struct App *app);
void mpris_shutdown(void);
void mpris_poll(void);
void mpris_notify(void);
void mpris_notify_seeked(void);

#endif
