/* SPDX-License-Identifier: LGPL-2.1-only
 * Copyright (C) 2026 JobDestroyer
 */

#ifndef STOMP_CDG_H
#define STOMP_CDG_H

int cdg_open(const char *path);
void cdg_close(void);
void cdg_shutdown(void);
void cdg_lost_gl(void);
int cdg_active(void);
void cdg_update(double seconds);
void cdg_present(int win_w, int win_h);

#endif
