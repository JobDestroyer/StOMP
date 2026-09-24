#ifndef VIBE_DECODE_H
#define VIBE_DECODE_H

#include "config.h"
#include "ringbuf.h"

#include <stdint.h>

int decode_start(RingBuf *rb);
void decode_stop(void);
int decode_open(const char *path);
int decode_open_at(const char *path, int paused, double seek_sec);
void decode_pause(int paused);
void decode_seek(double seconds);
void decode_stop_file(void);
int decode_has_file(void);
int decode_is_live(void);
int decode_take_ended(void);
int decode_take_failed(void);
double decode_position(void);
double decode_duration(void);
void decode_meta(char *title, int tlen, char *artist, int alen, char *album, int blen);
void decode_icy(char *out, int n);

void decode_request_scan(void);
int decode_scan_busy(void);

#endif
