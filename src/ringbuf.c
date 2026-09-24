#include "ringbuf.h"

#include <string.h>

int ringbuf_init(RingBuf *rb, uint32_t cap_frames, float *storage)
{
    if (!rb || !storage || cap_frames == 0 || (cap_frames & (cap_frames - 1u)) != 0) {
        return -1;
    }
    rb->samples = storage;
    rb->cap_frames = cap_frames;
    rb->mask = cap_frames - 1u;
    atomic_store(&rb->w, 0);
    atomic_store(&rb->r, 0);
    memset(storage, 0, (size_t)cap_frames * 2u * sizeof(float));
    return 0;
}

void ringbuf_clear(RingBuf *rb)
{
    atomic_store(&rb->w, 0);
    atomic_store(&rb->r, 0);
}

uint32_t ringbuf_fill(const RingBuf *rb)
{
    uint32_t w = atomic_load_explicit(&rb->w, memory_order_acquire);
    uint32_t r = atomic_load_explicit(&rb->r, memory_order_relaxed);
    return w - r;
}

uint32_t ringbuf_space(const RingBuf *rb)
{
    return rb->cap_frames - ringbuf_fill(rb);
}

uint32_t ringbuf_write(RingBuf *rb, const float *interleaved, uint32_t frames)
{
    uint32_t w = atomic_load_explicit(&rb->w, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&rb->r, memory_order_acquire);
    uint32_t fill = w - r;
    uint32_t space = rb->cap_frames - fill;
    if (frames > space) {
        frames = space;
    }
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t idx = (w + i) & rb->mask;
        rb->samples[idx * 2u] = interleaved[i * 2u];
        rb->samples[idx * 2u + 1u] = interleaved[i * 2u + 1u];
    }
    atomic_store_explicit(&rb->w, w + frames, memory_order_release);
    return frames;
}

uint32_t ringbuf_read(RingBuf *rb, float *interleaved, uint32_t frames)
{
    uint32_t r = atomic_load_explicit(&rb->r, memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&rb->w, memory_order_acquire);
    uint32_t fill = w - r;
    if (frames > fill) {
        frames = fill;
    }
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t idx = (r + i) & rb->mask;
        interleaved[i * 2u] = rb->samples[idx * 2u];
        interleaved[i * 2u + 1u] = rb->samples[idx * 2u + 1u];
    }
    atomic_store_explicit(&rb->r, r + frames, memory_order_release);
    return frames;
}
