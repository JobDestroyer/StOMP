#include "audio.h"

#include <SDL.h>

#include <stdio.h>
#include <string.h>

static RingBuf *s_ring;
static VizSnap *s_snap;
static SDL_AudioDeviceID s_dev;
static atomic_int s_paused;
static atomic_uint s_vol_milli; /* 0..1000 */
static float s_cb_scratch[4096 * 2];

static void audio_cb(void *userdata, Uint8 *stream, int len)
{
    int bytes_per_frame = (int)(VIBE_CHANNELS * sizeof(float));
    uint32_t want = (uint32_t)(len / bytes_per_frame);
    uint32_t got;
    float vol;
    unsigned page;
    uint32_t viz_n;
    const float *viz_src;
    (void)userdata;

    if (len > 0) {
        memset(stream, 0, (size_t)len);
    }
    if (want * 2u > (uint32_t)(sizeof(s_cb_scratch) / sizeof(s_cb_scratch[0]))) {
        want = (uint32_t)(sizeof(s_cb_scratch) / sizeof(s_cb_scratch[0]) / 2u);
    }

    if (atomic_load_explicit(&s_paused, memory_order_relaxed) || !s_ring) {
        return;
    }

    got = ringbuf_read(s_ring, s_cb_scratch, want);
    if (got < want) {
        memset(s_cb_scratch + got * 2u, 0, (size_t)(want - got) * 2u * sizeof(float));
    }

    /* Viz snapshot: pre-fader copy of the same decoded block. */
    viz_n = got;
    viz_src = s_cb_scratch;
    if (viz_n > VIBE_VIZ_FRAMES) {
        viz_src = s_cb_scratch + (got - VIBE_VIZ_FRAMES) * 2u;
        viz_n = VIBE_VIZ_FRAMES;
    }
    if (s_snap && viz_n > 0) {
        page = 1u - atomic_load_explicit(&s_snap->page, memory_order_relaxed);
        memcpy(s_snap->pcm[page], viz_src, (size_t)viz_n * 2u * sizeof(float));
        s_snap->nframes[page] = viz_n;
        atomic_store_explicit(&s_snap->page, page, memory_order_release);
    }

    vol = (float)atomic_load_explicit(&s_vol_milli, memory_order_relaxed) / 1000.f;
    if (vol < 0.999f) {
        uint32_t n = want * 2u;
        for (uint32_t i = 0; i < n; i++) {
            s_cb_scratch[i] *= vol;
        }
    }
    memcpy(stream, s_cb_scratch, (size_t)want * 2u * sizeof(float));
}

static int open_device(const char *device_name)
{
    SDL_AudioSpec want, have;

    memset(&want, 0, sizeof(want));
    want.freq = VIBE_SAMPLE_RATE;
    want.format = AUDIO_F32SYS;
    want.channels = VIBE_CHANNELS;
    want.samples = 512;
    want.callback = audio_cb;

    s_dev = SDL_OpenAudioDevice(device_name && device_name[0] ? device_name : NULL,
                                0, &want, &have, 0);
    if (!s_dev) {
        fprintf(stderr, "StOMP: SDL_OpenAudioDevice: %s\n", SDL_GetError());
        return -1;
    }
    if (have.freq != VIBE_SAMPLE_RATE || have.channels != VIBE_CHANNELS ||
        have.format != AUDIO_F32SYS) {
        fprintf(stderr, "StOMP: audio device opened with unexpected spec (%d Hz, %d ch, fmt %d)\n",
                have.freq, have.channels, (int)have.format);
        SDL_CloseAudioDevice(s_dev);
        s_dev = 0;
        return -1;
    }
    SDL_PauseAudioDevice(s_dev, 0);
    return 0;
}

int audio_init(RingBuf *rb, VizSnap *snap, const char *device_name)
{
    s_ring = rb;
    s_snap = snap;
    atomic_store(&s_paused, 1);
    atomic_store(&s_vol_milli, 1000);
    if (snap) {
        memset(snap->pcm, 0, sizeof(snap->pcm));
        snap->nframes[0] = snap->nframes[1] = 0;
        atomic_store(&snap->page, 0);
    }
    return open_device(device_name);
}

void audio_shutdown(void)
{
    if (s_dev) {
        SDL_PauseAudioDevice(s_dev, 1);
        SDL_CloseAudioDevice(s_dev);
        s_dev = 0;
    }
    s_ring = NULL;
    s_snap = NULL;
}

int audio_reopen(const char *device_name)
{
    if (s_dev) {
        SDL_PauseAudioDevice(s_dev, 1);
        SDL_CloseAudioDevice(s_dev);
        s_dev = 0;
    }
    return open_device(device_name);
}

void audio_set_volume(float v)
{
    if (v < 0.f) {
        v = 0.f;
    }
    if (v > 1.f) {
        v = 1.f;
    }
    atomic_store(&s_vol_milli, (unsigned)(v * 1000.f + 0.5f));
}

float audio_volume(void)
{
    return (float)atomic_load(&s_vol_milli) / 1000.f;
}

void audio_pause(int paused)
{
    atomic_store(&s_paused, paused ? 1 : 0);
}

int audio_paused(void)
{
    return atomic_load(&s_paused) != 0;
}

void audio_list_devices(char names[][128], int *count, int cap)
{
    int n = SDL_GetNumAudioDevices(0);
    int out = 0;
    if (n < 0) {
        n = 0;
    }
    for (int i = 0; i < n && out < cap; i++) {
        const char *nm = SDL_GetAudioDeviceName(i, 0);
        if (!nm) {
            continue;
        }
        snprintf(names[out], 128, "%s", nm);
        out++;
    }
    if (count) {
        *count = out;
    }
}
