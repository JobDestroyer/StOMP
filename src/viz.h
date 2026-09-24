#ifndef VIBE_VIZ_H
#define VIBE_VIZ_H

#include <stddef.h>

int viz_init(int w, int h, int mesh_w, int mesh_h, int fps,
             const char *preset_dir, const char *texture_dir);
void viz_shutdown(void);
int viz_ready(void);
void viz_set_internal_size(int w, int h);
void viz_set_mesh(int w, int h);
void viz_set_fps(int fps);
void viz_feed_pcm(const float *interleaved, unsigned frames);
void viz_render_to_fbo(void);
unsigned viz_texture(void);
int viz_tex_w(void);
int viz_tex_h(void);
void viz_next_preset(int hard);
void viz_prev_preset(int hard);
void viz_set_lock(int lock);
int viz_locked(void);
void viz_set_shuffle(int on);
int viz_shuffle(void);
int viz_preset_count(void);
int viz_current_index(void);
int viz_play_index(int index, int hard);
int viz_preset_name(int index, char *buf, size_t n);
int viz_preset_cat(int index, char *buf, size_t n);
int viz_current_label(char *buf, size_t n);
int viz_sorted_at(int rank);
int viz_rank_of(int playlist_index);

typedef enum {
    VIBE_VIZ_POOL_NOT_BAD = 0,
    VIBE_VIZ_POOL_UNREVIEWED = 1,
    VIBE_VIZ_POOL_GOOD_ONLY = 2
} VibeVizPool;

void viz_set_enabled(int on);
int viz_enabled(void);
void viz_set_pool(VibeVizPool pool);
VibeVizPool viz_pool(void);
int viz_rate_current(int good);
int viz_current_rating(void);
int viz_preset_rating(int index);
void viz_filter_tick(void);
const char *viz_pool_label(VibeVizPool pool);

#endif
