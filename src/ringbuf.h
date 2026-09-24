#ifndef VIBE_RINGBUF_H
#define VIBE_RINGBUF_H

#include <stdatomic.h>
#include <stdint.h>

typedef struct RingBuf {
    float *samples;          /* interleaved stereo, cap_frames * 2 */
    uint32_t cap_frames;     /* power of two */
    uint32_t mask;
    atomic_uint w;
    atomic_uint r;
} RingBuf;

int ringbuf_init(RingBuf *rb, uint32_t cap_frames, float *storage);
void ringbuf_clear(RingBuf *rb);
uint32_t ringbuf_write(RingBuf *rb, const float *interleaved, uint32_t frames);
uint32_t ringbuf_read(RingBuf *rb, float *interleaved, uint32_t frames);
uint32_t ringbuf_fill(const RingBuf *rb);
uint32_t ringbuf_space(const RingBuf *rb);

#endif
