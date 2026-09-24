#ifndef VIBE_MPRIS_H
#define VIBE_MPRIS_H

struct App;

int mpris_init(struct App *app);
void mpris_shutdown(void);
void mpris_poll(void);
void mpris_notify(void);

#endif
