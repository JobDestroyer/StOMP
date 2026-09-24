#ifndef VIBE_APP_H
#define VIBE_APP_H

#include "audio.h"
#include "config.h"
#include "decode.h"
#include "input.h"
#include "library.h"
#include "platform_sdl.h"
#include "ringbuf.h"
#include "ui.h"
#include "viz.h"

#include <stdint.h>

typedef struct App {
    int running;
    int windowed;
    int win_w, win_h;
    int effective_profile;
    int shuffle;
    VibeRepeat repeat;
    int suspended;
    uint32_t last_persist_ms;
    uint32_t last_profile_check_ms;
    uint32_t resize_stamp_ms;
    int pending_resize;
    int mesh_w, mesh_h;
    int frame_n, frame_miss;
    int viz_retry;
    VibeConfig cfg;
    VibeProfileParams profile;
    RingBuf ring;
    VizSnap viz;
    LibTrack now;
    int now_valid;
    int playing_radio;
    char radio_url[VIBE_PATH_MAX];
    char status[128];
    float *ring_storage;
    float *decode_scratch; /* owned by decode via static; kept for documentation */
    char play_file[VIBE_PATH_MAX];
} App;

int app_run(int argc, char **argv);
void app_play_track(App *app, const LibTrack *t);
void app_play_radio(App *app, const char *title, const char *url, const char *subtitle);
void app_play_queue_index(App *app, int idx);
void app_next_track(App *app, int from_end);
void app_prev_track(App *app);
void app_toggle_pause(App *app);
void app_resume_playback(App *app);
void app_stop(App *app);
void app_seek_delta(App *app, double seconds);
void app_set_volume_delta(App *app, float d);
void app_apply_profile(App *app, int force);
void app_persist(App *app);
void app_restore_session(App *app);

#endif
