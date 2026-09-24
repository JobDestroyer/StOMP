#ifndef VIBE_AUDIO_H
#define VIBE_AUDIO_H

#include "config.h"
#include "ringbuf.h"

#include <stdatomic.h>
#include <stdint.h>

typedef struct {
    float pcm[2][VIBE_VIZ_FRAMES * 2];
    uint32_t nframes[2];
    atomic_uint page;
} VizSnap;

int audio_init(RingBuf *rb, VizSnap *snap, const char *device_name);
void audio_shutdown(void);
int audio_reopen(const char *device_name);
void audio_set_volume(float v);
float audio_volume(void);
void audio_pause(int paused);
int audio_paused(void);
void audio_list_devices(char names[][128], int *count, int cap);

#endif
