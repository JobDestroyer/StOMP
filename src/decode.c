#include "decode.h"

#include "library.h"

#include <SDL.h>
#include <stdatomic.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CMD_NONE = 0,
    CMD_OPEN,
    CMD_SEEK,
    CMD_STOP,
    CMD_QUIT
};

static RingBuf *s_ring;
static SDL_Thread *s_thread;
static SDL_Thread *s_scan_thread;
static SDL_mutex *s_mtx;
static SDL_cond *s_cond;
static int s_cmd;
static char s_path[VIBE_PATH_MAX];
static double s_seek_sec;
static double s_pending_seek = -1.0;
static int s_paused;
static int s_have_file;
static int s_live;
static atomic_int s_ended;
static atomic_int s_failed;
static atomic_int s_scan_busy;
static atomic_int s_pos_ms;
static atomic_int s_dur_ms;
static char s_title[VIBE_NAME_MAX];
static char s_artist[VIBE_NAME_MAX];
static char s_album[VIBE_NAME_MAX];
static char s_icy[VIBE_NAME_MAX];
static float s_scratch[VIBE_DECODE_SCRATCH_FRAMES * 2];

static void copy_meta_dict(AVDictionary *d, const char *key, char *out, int n)
{
    const AVDictionaryEntry *e = NULL;
    if (!d || !out || n <= 0) {
        return;
    }
    e = av_dict_get(d, key, NULL, 0);
    if (e && e->value) {
        snprintf(out, (size_t)n, "%s", e->value);
    }
}

static void close_ctx(AVFormatContext **fmt, AVCodecContext **cc, SwrContext **swr,
                      AVPacket **pkt, AVFrame **fr)
{
    if (swr && *swr) {
        swr_free(swr);
    }
    if (cc && *cc) {
        avcodec_free_context(cc);
    }
    if (fmt && *fmt) {
        avformat_close_input(fmt);
    }
    if (pkt && *pkt) {
        av_packet_free(pkt);
    }
    if (fr && *fr) {
        av_frame_free(fr);
    }
}

static int open_file(const char *path, AVFormatContext **fmt, AVCodecContext **cc,
                     SwrContext **swr, AVPacket **pkt, AVFrame **fr, int *stream_i)
{
    const AVCodec *dec = NULL;
    int si, err;
    AVChannelLayout out_ch = AV_CHANNEL_LAYOUT_STEREO;

    s_title[0] = s_artist[0] = s_album[0] = s_icy[0] = '\0';
    atomic_store(&s_pos_ms, 0);
    atomic_store(&s_dur_ms, 0);
    *fmt = NULL;
    *cc = NULL;
    *swr = NULL;
    *pkt = NULL;
    *fr = NULL;
    *stream_i = -1;

    {
        AVDictionary *opts = NULL;
        int net = path && (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0);
        if (net) {
            av_dict_set(&opts, "protocol_whitelist", "file,http,https,tcp,tls,crypto,rtp,udp", 0);
            av_dict_set(&opts, "user_agent", "StOMP/1.0", 0);
            av_dict_set(&opts, "icy", "1", 0);
            av_dict_set(&opts, "reconnect", "1", 0);
            av_dict_set(&opts, "reconnect_streamed", "1", 0);
            av_dict_set(&opts, "reconnect_delay_max", "5", 0);
            av_dict_set(&opts, "timeout", "15000000", 0);
            av_dict_set(&opts, "rw_timeout", "15000000", 0);
        } else {
            av_dict_set(&opts, "protocol_whitelist", "file", 0);
        }
        err = avformat_open_input(fmt, path, NULL, &opts);
        av_dict_free(&opts);
    }
    if (err < 0) {
        char buf[128];
        av_strerror(err, buf, sizeof(buf));
        fprintf(stderr, "StOMP: open %s: %s\n", path, buf);
        return -1;
    }
    err = avformat_find_stream_info(*fmt, NULL);
    if (err < 0) {
        fprintf(stderr, "StOMP: stream info %s failed\n", path);
        avformat_close_input(fmt);
        return -1;
    }
    si = av_find_best_stream(*fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
    if (si < 0 || !dec) {
        fprintf(stderr, "StOMP: no audio stream in %s\n", path);
        avformat_close_input(fmt);
        return -1;
    }
    *cc = avcodec_alloc_context3(dec);
    if (!*cc) {
        avformat_close_input(fmt);
        return -1;
    }
    if (avcodec_parameters_to_context(*cc, (*fmt)->streams[si]->codecpar) < 0) {
        close_ctx(fmt, cc, swr, pkt, fr);
        return -1;
    }
    if (avcodec_open2(*cc, dec, NULL) < 0) {
        fprintf(stderr, "StOMP: avcodec_open2 failed for %s\n", path);
        close_ctx(fmt, cc, swr, pkt, fr);
        return -1;
    }
    if (swr_alloc_set_opts2(swr, &out_ch, AV_SAMPLE_FMT_FLT, VIBE_SAMPLE_RATE,
                            &(*cc)->ch_layout, (*cc)->sample_fmt, (*cc)->sample_rate,
                            0, NULL) < 0 || !*swr) {
        fprintf(stderr, "StOMP: swr_alloc_set_opts2 failed\n");
        close_ctx(fmt, cc, swr, pkt, fr);
        return -1;
    }
    if (swr_init(*swr) < 0) {
        fprintf(stderr, "StOMP: swr_init failed\n");
        close_ctx(fmt, cc, swr, pkt, fr);
        return -1;
    }
    *pkt = av_packet_alloc();
    *fr = av_frame_alloc();
    if (!*pkt || !*fr) {
        close_ctx(fmt, cc, swr, pkt, fr);
        return -1;
    }
    *stream_i = si;
    {
        double dur = 0;
        if ((*fmt)->duration > 0) {
            dur = (double)(*fmt)->duration / (double)AV_TIME_BASE;
        } else if ((*fmt)->streams[si]->duration > 0) {
            dur = (*fmt)->streams[si]->duration * av_q2d((*fmt)->streams[si]->time_base);
        }
        if (path && (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0)) {
            dur = 0;
        }
        atomic_store(&s_dur_ms, dur > 0 ? (int)(dur * 1000.0 + 0.5) : 0);
    }
    if (s_mtx) {
        SDL_LockMutex(s_mtx);
    }
    copy_meta_dict((*fmt)->metadata, "title", s_title, sizeof(s_title));
    copy_meta_dict((*fmt)->metadata, "artist", s_artist, sizeof(s_artist));
    copy_meta_dict((*fmt)->metadata, "album", s_album, sizeof(s_album));
    copy_meta_dict((*fmt)->streams[si]->metadata, "title", s_title, sizeof(s_title));
    if (!s_title[0]) {
        const char *slash = strrchr(path, '/');
        snprintf(s_title, sizeof(s_title), "%s", slash ? slash + 1 : path);
    }
    {
        AVDictionaryEntry *e = av_dict_get((*fmt)->metadata, "StreamTitle", NULL, 0);
        if (!e) {
            e = av_dict_get((*fmt)->metadata, "icy_title", NULL, 0);
        }
        if (e && e->value) {
            snprintf(s_icy, sizeof(s_icy), "%s", e->value);
        }
    }
    if (s_mtx) {
        SDL_UnlockMutex(s_mtx);
    }
    return 0;
}

static int write_pcm(const float *src, int frames)
{
    int off = 0;
    while (off < frames) {
        uint32_t space = ringbuf_space(s_ring);
        uint32_t chunk;
        if (space == 0) {
            SDL_Delay(4);
            SDL_LockMutex(s_mtx);
            if (s_cmd != CMD_NONE) {
                SDL_UnlockMutex(s_mtx);
                return -1;
            }
            SDL_UnlockMutex(s_mtx);
            continue;
        }
        chunk = (uint32_t)(frames - off);
        if (chunk > space) {
            chunk = space;
        }
        ringbuf_write(s_ring, src + off * 2, chunk);
        off += (int)chunk;
    }
    return 0;
}

static void set_pos_sec(double sec)
{
    if (sec < 0) {
        sec = 0;
    }
    atomic_store(&s_pos_ms, (int)(sec * 1000.0 + 0.5));
}

static int convert_and_write(SwrContext *swr, AVFrame *fr, AVFormatContext *fmt, int stream_i)
{
    const uint8_t **in = NULL;
    int in_nb = 0;
    int fed = 0;
    if (fr) {
        in = (const uint8_t **)fr->extended_data;
        in_nb = fr->nb_samples;
    }
    for (;;) {
        uint8_t *outs[1] = {(uint8_t *)s_scratch};
        int out_frames = swr_convert(swr, outs, VIBE_DECODE_SCRATCH_FRAMES, in, in_nb);
        in = NULL;
        in_nb = 0;
        if (out_frames < 0) {
            return -1;
        }
        if (out_frames == 0) {
            break;
        }
        if (write_pcm(s_scratch, out_frames) != 0) {
            return -1;
        }
        if (!fed && fr && fmt && stream_i >= 0 &&
            fr->best_effort_timestamp != AV_NOPTS_VALUE) {
            set_pos_sec(fr->best_effort_timestamp * av_q2d(fmt->streams[stream_i]->time_base));
            fed = 1;
        } else if (!fed) {
            int ms = atomic_load(&s_pos_ms);
            ms += (int)((out_frames * 1000) / VIBE_SAMPLE_RATE);
            atomic_store(&s_pos_ms, ms);
        }
    }
    return 0;
}

static int scan_thread_main(void *ud)
{
    (void)ud;
    if (library_scan() != 0) {
        fprintf(stderr, "StOMP: library scan failed\n");
    }
    atomic_store(&s_scan_busy, 0);
    return 0;
}

static int thread_main(void *ud)
{
    AVFormatContext *fmt = NULL;
    AVCodecContext *cc = NULL;
    SwrContext *swr = NULL;
    AVPacket *pkt = NULL;
    AVFrame *fr = NULL;
    int stream_i = -1;
    int pkt_errs = 0;
    char cur_path[VIBE_PATH_MAX];
    (void)ud;
    cur_path[0] = '\0';

    av_log_set_level(AV_LOG_ERROR);

    for (;;) {
        int cmd;
        SDL_LockMutex(s_mtx);
        while (s_cmd == CMD_NONE && (s_paused || !s_have_file)) {
            SDL_CondWait(s_cond, s_mtx);
        }
        cmd = s_cmd;
        if (cmd == CMD_QUIT) {
            SDL_UnlockMutex(s_mtx);
            break;
        }
        if (cmd == CMD_OPEN) {
            char path[VIBE_PATH_MAX];
            double pending;
            snprintf(path, sizeof(path), "%s", s_path);
            snprintf(cur_path, sizeof(cur_path), "%s", path);
            pending = s_pending_seek;
            s_pending_seek = -1.0;
            s_cmd = CMD_NONE;
            s_have_file = 0;
            SDL_UnlockMutex(s_mtx);
            pkt_errs = 0;
            close_ctx(&fmt, &cc, &swr, &pkt, &fr);
            ringbuf_clear(s_ring);
            atomic_store(&s_ended, 0);
            atomic_store(&s_failed, 0);
            s_live = (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0);
            if (open_file(path, &fmt, &cc, &swr, &pkt, &fr, &stream_i) == 0) {
                if (!s_live && pending >= 0.0 && stream_i >= 0) {
                    int64_t ts = (int64_t)(pending / av_q2d(fmt->streams[stream_i]->time_base));
                    av_seek_frame(fmt, stream_i, ts, AVSEEK_FLAG_BACKWARD);
                    avcodec_flush_buffers(cc);
                    if (swr) {
                        swr_init(swr);
                    }
                    set_pos_sec(pending);
                }
                SDL_LockMutex(s_mtx);
                s_have_file = 1;
                SDL_UnlockMutex(s_mtx);
            } else {
                fprintf(stderr, "StOMP: skip unreadable file\n");
                atomic_store(&s_failed, 1);
            }
            continue;
        }
        if (cmd == CMD_STOP) {
            s_cmd = CMD_NONE;
            s_have_file = 0;
            s_live = 0;
            SDL_UnlockMutex(s_mtx);
            close_ctx(&fmt, &cc, &swr, &pkt, &fr);
            ringbuf_clear(s_ring);
            continue;
        }
        if (cmd == CMD_SEEK) {
            double sec = s_seek_sec;
            s_cmd = CMD_NONE;
            SDL_UnlockMutex(s_mtx);
            if (s_live) {
                continue;
            }
            if (fmt && stream_i >= 0) {
                int64_t ts = (int64_t)(sec / av_q2d(fmt->streams[stream_i]->time_base));
                av_seek_frame(fmt, stream_i, ts, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(cc);
                if (swr) {
                    swr_init(swr);
                }
                ringbuf_clear(s_ring);
                set_pos_sec(sec);
            }
            continue;
        }
        SDL_UnlockMutex(s_mtx);

        if (!fmt || !cc || !swr) {
            SDL_Delay(10);
            continue;
        }

        if (av_read_frame(fmt, pkt) < 0) {
            if (s_live) {
                close_ctx(&fmt, &cc, &swr, &pkt, &fr);
                SDL_Delay(400);
                SDL_LockMutex(s_mtx);
                if (s_cmd != CMD_NONE) {
                    SDL_UnlockMutex(s_mtx);
                    continue;
                }
                SDL_UnlockMutex(s_mtx);
                if (open_file(cur_path, &fmt, &cc, &swr, &pkt, &fr, &stream_i) != 0) {
                    atomic_store(&s_failed, 1);
                    SDL_LockMutex(s_mtx);
                    s_have_file = 0;
                    SDL_UnlockMutex(s_mtx);
                }
                continue;
            }
            avcodec_send_packet(cc, NULL);
            while (avcodec_receive_frame(cc, fr) >= 0) {
                convert_and_write(swr, fr, fmt, stream_i);
                av_frame_unref(fr);
            }
            convert_and_write(swr, NULL, fmt, stream_i);
            atomic_store(&s_ended, 1);
            SDL_LockMutex(s_mtx);
            s_have_file = 0;
            SDL_UnlockMutex(s_mtx);
            close_ctx(&fmt, &cc, &swr, &pkt, &fr);
            continue;
        }
        if (s_live && fmt) {
            AVDictionaryEntry *e = av_dict_get(fmt->metadata, "StreamTitle", NULL, 0);
            if (!e) {
                e = av_dict_get(fmt->metadata, "icy_title", NULL, 0);
            }
            if (e && e->value && e->value[0]) {
                SDL_LockMutex(s_mtx);
                if (strcmp(s_icy, e->value) != 0) {
                    snprintf(s_icy, sizeof(s_icy), "%s", e->value);
                }
                SDL_UnlockMutex(s_mtx);
            }
        }
        if (pkt->stream_index != stream_i) {
            av_packet_unref(pkt);
            continue;
        }
        if (avcodec_send_packet(cc, pkt) < 0) {
            av_packet_unref(pkt);
            pkt_errs++;
            if (pkt_errs > 64) {
                fprintf(stderr, "StOMP: too many decode errors, skipping file\n");
                atomic_store(&s_failed, 1);
                SDL_LockMutex(s_mtx);
                s_have_file = 0;
                SDL_UnlockMutex(s_mtx);
                close_ctx(&fmt, &cc, &swr, &pkt, &fr);
            }
            continue;
        }
        av_packet_unref(pkt);
        pkt_errs = 0;
        while (avcodec_receive_frame(cc, fr) >= 0) {
            if (convert_and_write(swr, fr, fmt, stream_i) != 0) {
                av_frame_unref(fr);
                break;
            }
            av_frame_unref(fr);
        }
    }
    close_ctx(&fmt, &cc, &swr, &pkt, &fr);
    return 0;
}

int decode_start(RingBuf *rb)
{
    s_ring = rb;
    s_mtx = SDL_CreateMutex();
    s_cond = SDL_CreateCond();
    if (!s_mtx || !s_cond) {
        fprintf(stderr, "StOMP: decode sync objects failed: %s\n", SDL_GetError());
        return -1;
    }
    s_thread = SDL_CreateThread(thread_main, "vibe-decode", NULL);
    if (!s_thread) {
        fprintf(stderr, "StOMP: decode thread: %s\n", SDL_GetError());
        return -1;
    }
    return 0;
}

void decode_stop(void)
{
    if (!s_mtx) {
        return;
    }
    SDL_LockMutex(s_mtx);
    s_cmd = CMD_QUIT;
    SDL_CondSignal(s_cond);
    SDL_UnlockMutex(s_mtx);
    if (s_thread) {
        SDL_WaitThread(s_thread, NULL);
        s_thread = NULL;
    }
    if (s_scan_thread) {
        SDL_WaitThread(s_scan_thread, NULL);
        s_scan_thread = NULL;
    }
    SDL_DestroyCond(s_cond);
    SDL_DestroyMutex(s_mtx);
    s_cond = NULL;
    s_mtx = NULL;
}

int decode_open_at(const char *path, int paused, double seek_sec)
{
    if (!path || !s_mtx) {
        return -1;
    }
    SDL_LockMutex(s_mtx);
    snprintf(s_path, sizeof(s_path), "%s", path);
    s_pending_seek = seek_sec;
    s_paused = paused ? 1 : 0;
    s_cmd = CMD_OPEN;
    SDL_CondSignal(s_cond);
    SDL_UnlockMutex(s_mtx);
    return 0;
}

int decode_open(const char *path)
{
    return decode_open_at(path, 0, -1.0);
}

void decode_pause(int paused)
{
    if (!s_mtx) {
        return;
    }
    SDL_LockMutex(s_mtx);
    s_paused = paused ? 1 : 0;
    if (!s_paused) {
        SDL_CondSignal(s_cond);
    }
    SDL_UnlockMutex(s_mtx);
}

void decode_seek(double seconds)
{
    if (!s_mtx) {
        return;
    }
    if (s_live) {
        return;
    }
    if (seconds < 0) {
        seconds = 0;
    }
    SDL_LockMutex(s_mtx);
    s_seek_sec = seconds;
    s_cmd = CMD_SEEK;
    SDL_CondSignal(s_cond);
    SDL_UnlockMutex(s_mtx);
}

void decode_stop_file(void)
{
    if (!s_mtx) {
        return;
    }
    SDL_LockMutex(s_mtx);
    s_cmd = CMD_STOP;
    SDL_CondSignal(s_cond);
    SDL_UnlockMutex(s_mtx);
}

int decode_has_file(void)
{
    int v;
    if (!s_mtx) {
        return 0;
    }
    SDL_LockMutex(s_mtx);
    v = s_have_file;
    SDL_UnlockMutex(s_mtx);
    return v;
}

int decode_is_live(void)
{
    int v;
    if (!s_mtx) {
        return 0;
    }
    SDL_LockMutex(s_mtx);
    v = s_live && s_have_file;
    SDL_UnlockMutex(s_mtx);
    return v;
}

int decode_take_ended(void)
{
    return atomic_exchange(&s_ended, 0);
}

int decode_take_failed(void)
{
    return atomic_exchange(&s_failed, 0);
}

double decode_position(void)
{
    return (double)atomic_load(&s_pos_ms) / 1000.0;
}

double decode_duration(void)
{
    return (double)atomic_load(&s_dur_ms) / 1000.0;
}

void decode_meta(char *title, int tlen, char *artist, int alen, char *album, int blen)
{
    if (!s_mtx) {
        return;
    }
    SDL_LockMutex(s_mtx);
    if (title && tlen > 0) {
        snprintf(title, (size_t)tlen, "%s", s_title);
    }
    if (artist && alen > 0) {
        snprintf(artist, (size_t)alen, "%s", s_artist);
    }
    if (album && blen > 0) {
        snprintf(album, (size_t)blen, "%s", s_album);
    }
    SDL_UnlockMutex(s_mtx);
}

void decode_icy(char *out, int n)
{
    if (!out || n <= 0) {
        return;
    }
    out[0] = '\0';
    if (!s_mtx) {
        return;
    }
    SDL_LockMutex(s_mtx);
    snprintf(out, (size_t)n, "%s", s_icy);
    SDL_UnlockMutex(s_mtx);
}

void decode_request_scan(void)
{
    if (atomic_load(&s_scan_busy)) {
        return;
    }
    if (s_scan_thread) {
        SDL_WaitThread(s_scan_thread, NULL);
        s_scan_thread = NULL;
    }
    atomic_store(&s_scan_busy, 1);
    s_scan_thread = SDL_CreateThread(scan_thread_main, "vibe-scan", NULL);
    if (!s_scan_thread) {
        atomic_store(&s_scan_busy, 0);
        fprintf(stderr, "StOMP: scan thread failed: %s\n", SDL_GetError());
    }
}

int decode_scan_busy(void)
{
    return atomic_load(&s_scan_busy) != 0;
}
