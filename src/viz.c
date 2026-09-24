#include "viz.h"

#include "config.h"
#include "library.h"
#include "platform_sdl.h"

#include <projectM-4/projectM.h>
#include <projectM-4/playlist.h>
#include <projectM-4/playlist_callbacks.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static projectm_handle s_pm;
static projectm_playlist_handle s_pl;
static int s_w, s_h;
static int s_ready;
static char (*s_pname)[VIBE_NAME_MAX];
static char (*s_pcat)[VIBE_NAME_MAX];
static int *s_order;
static int *s_rate;
static int s_ncache;
static int s_enabled = 1;
static int s_pool = VIBE_VIZ_POOL_NOT_BAD;
static int s_pending_skip;
static int s_skip_guard;

static void viz_free_index(void)
{
    free(s_pname);
    free(s_pcat);
    free(s_order);
    free(s_rate);
    s_pname = NULL;
    s_pcat = NULL;
    s_order = NULL;
    s_rate = NULL;
    s_ncache = 0;
}

static void split_preset_path(const char *path, char *name, size_t name_n, char *cat, size_t cat_n)
{
    const char *slash;
    const char *base;
    const char *marker;
    const char *start;
    const char *end;
    size_t i, o, L;

    if (name && name_n) {
        name[0] = '\0';
    }
    if (cat && cat_n) {
        cat[0] = '\0';
    }
    if (!path || !path[0]) {
        return;
    }

    slash = strrchr(path, '/');
    base = slash ? slash + 1 : path;
    if (name && name_n) {
        snprintf(name, name_n, "%s", base);
        L = strlen(name);
        if (L > 5) {
            char *ext = name + L - 5;
            if (ext[0] == '.' &&
                (ext[1] == 'm' || ext[1] == 'M') &&
                (ext[2] == 'i' || ext[2] == 'I') &&
                (ext[3] == 'l' || ext[3] == 'L') &&
                (ext[4] == 'k' || ext[4] == 'K')) {
                ext[0] = '\0';
            }
        }
    }

    if (!cat || cat_n == 0) {
        return;
    }
    marker = strstr(path, "cream-of-the-crop/");
    if (!marker) {
        snprintf(cat, cat_n, "StOMP");
        return;
    }
    start = marker + strlen("cream-of-the-crop/");
    end = slash ? slash : path + strlen(path);
    if (start >= end) {
        snprintf(cat, cat_n, "Cream of the Crop");
        return;
    }
    o = 0;
    for (i = 0; start + i < end && o + 1 < cat_n; i++) {
        if (start[i] == '/') {
            if (o + 3 < cat_n) {
                cat[o++] = ' ';
                cat[o++] = '/';
                cat[o++] = ' ';
            }
        } else {
            cat[o++] = start[i];
        }
    }
    cat[o] = '\0';
}

static int name_bucket(const char *s)
{
    unsigned char c;
    if (!s) {
        return 1;
    }
    while (*s == ' ') {
        s++;
    }
    c = (unsigned char)*s;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0x80) {
        return 0;
    }
    return 1;
}

static int viz_cmp(const void *a, const void *b)
{
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    int ba = name_bucket(s_pcat[ia]);
    int bb = name_bucket(s_pcat[ib]);
    int c;
    if (ba != bb) {
        return ba - bb;
    }
    c = strcasecmp(s_pcat[ia], s_pcat[ib]);
    if (c != 0) {
        return c;
    }
    ba = name_bucket(s_pname[ia]);
    bb = name_bucket(s_pname[ib]);
    if (ba != bb) {
        return ba - bb;
    }
    return strcasecmp(s_pname[ia], s_pname[ib]);
}

static void viz_build_index(void)
{
    uint32_t n, i;
    char **items;

    viz_free_index();
    if (!s_pl) {
        return;
    }
    n = projectm_playlist_size(s_pl);
    if (n == 0) {
        return;
    }
    items = projectm_playlist_items(s_pl, 0, n);
    if (!items) {
        return;
    }
    s_pname = calloc((size_t)n, sizeof(*s_pname));
    s_pcat = calloc((size_t)n, sizeof(*s_pcat));
    s_order = malloc((size_t)n * sizeof(int));
    s_rate = calloc((size_t)n, sizeof(int));
    if (!s_pname || !s_pcat || !s_order || !s_rate) {
        viz_free_index();
        projectm_playlist_free_string_array(items);
        return;
    }
    for (i = 0; i < n; i++) {
        split_preset_path(items[i], s_pname[i], sizeof(s_pname[i]),
                          s_pcat[i], sizeof(s_pcat[i]));
        s_order[i] = (int)i;
        s_rate[i] = library_viz_get_rating(s_pname[i]);
    }
    projectm_playlist_free_string_array(items);
    s_ncache = (int)n;
    qsort(s_order, (size_t)n, sizeof(int), viz_cmp);
}

static int viz_allowed_index(int idx)
{
    int r;
    if (idx < 0 || idx >= s_ncache || !s_rate) {
        return 1;
    }
    r = s_rate[idx];
    if (s_pool == VIBE_VIZ_POOL_UNREVIEWED) {
        return r == 0;
    }
    if (s_pool == VIBE_VIZ_POOL_GOOD_ONLY) {
        return r > 0;
    }
    return r >= 0;
}

static int viz_skip_to_allowed(int dir, int hard)
{
    int n = s_ncache;
    int cur, i, k;
    if (!s_pl || n <= 0) {
        return 0;
    }
    cur = (int)projectm_playlist_get_position(s_pl);
    if (viz_shuffle()) {
        int tries;
        for (tries = 0; tries < n; tries++) {
            int pick = (int)((unsigned)rand() % (unsigned)n);
            if (pick != cur && viz_allowed_index(pick)) {
                projectm_playlist_set_position(s_pl, (uint32_t)pick, hard ? true : false);
                return 1;
            }
        }
        for (k = 0; k < n; k++) {
            if (viz_allowed_index(k)) {
                projectm_playlist_set_position(s_pl, (uint32_t)k, hard ? true : false);
                return 1;
            }
        }
        return 0;
    }
    for (k = 1; k <= n; k++) {
        i = cur + dir * k;
        while (i < 0) {
            i += n;
        }
        i %= n;
        if (viz_allowed_index(i)) {
            projectm_playlist_set_position(s_pl, (uint32_t)i, hard ? true : false);
            return 1;
        }
    }
    return 0;
}

static void viz_on_switched(bool is_hard_cut, unsigned int index, void *user_data)
{
    (void)is_hard_cut;
    (void)user_data;
    if (!viz_allowed_index((int)index)) {
        s_pending_skip = 1;
    }
}

int viz_init(int w, int h, int mesh_w, int mesh_h, int fps,
             const char *preset_dir, const char *texture_dir)
{
    uint32_t added = 0;
    const char *tex_paths[1];

    viz_shutdown();
    srand((unsigned)time(NULL));
    s_w = w > 64 ? w : 64;
    s_h = h > 64 ? h : 64;

    s_pm = projectm_create();
    if (!s_pm) {
        fprintf(stderr, "StOMP: projectm_create failed (need GL 3.3 core context)\n");
        return -1;
    }

    projectm_set_window_size(s_pm, (size_t)s_w, (size_t)s_h);
    projectm_set_mesh_size(s_pm, (size_t)mesh_w, (size_t)mesh_h);
    projectm_set_fps(s_pm, fps);
    projectm_set_preset_duration(s_pm, 30.0);
    projectm_set_soft_cut_duration(s_pm, 3.0);
    projectm_set_hard_cut_enabled(s_pm, false);
    projectm_set_aspect_correction(s_pm, true);

    if (texture_dir && texture_dir[0]) {
        tex_paths[0] = texture_dir;
        projectm_set_texture_search_paths(s_pm, tex_paths, 1);
    }

    s_pl = projectm_playlist_create(s_pm);
    if (!s_pl) {
        fprintf(stderr, "StOMP: projectm_playlist_create failed\n");
        projectm_destroy(s_pm);
        s_pm = NULL;
        return -1;
    }
    if (preset_dir && preset_dir[0]) {
        added = projectm_playlist_add_path(s_pl, preset_dir, true, false);
    }
    fprintf(stderr, "StOMP: loaded %u ProjectM presets from %s\n",
            (unsigned)added, preset_dir ? preset_dir : "(none)");
    projectm_playlist_set_shuffle(s_pl, false);
    viz_build_index();
    projectm_playlist_set_preset_switched_event_callback(s_pl, viz_on_switched, NULL);
    if (added > 0) {
        projectm_playlist_play_next(s_pl, true);
        viz_filter_tick();
    } else {
        projectm_load_preset_file(s_pm, "idle://", false);
    }
    s_ready = 1;
    return 0;
}

void viz_shutdown(void)
{
    viz_free_index();
    if (s_pl) {
        projectm_playlist_destroy(s_pl);
        s_pl = NULL;
    }
    if (s_pm) {
        projectm_destroy(s_pm);
        s_pm = NULL;
    }
    s_ready = 0;
}

int viz_ready(void)
{
    return s_ready;
}

void viz_set_internal_size(int w, int h)
{
    if (!s_pm) {
        return;
    }
    if (w < 64) {
        w = 64;
    }
    if (h < 64) {
        h = 64;
    }
    if (w == s_w && h == s_h) {
        return;
    }
    s_w = w;
    s_h = h;
    projectm_set_window_size(s_pm, (size_t)s_w, (size_t)s_h);
}

void viz_set_mesh(int w, int h)
{
    if (s_pm) {
        projectm_set_mesh_size(s_pm, (size_t)w, (size_t)h);
    }
}

void viz_set_fps(int fps)
{
    if (s_pm) {
        projectm_set_fps(s_pm, fps);
    }
}

void viz_feed_pcm(const float *interleaved, unsigned frames)
{
    if (!s_pm || !interleaved || frames == 0) {
        return;
    }
    /* 4.x C API: interleaved LRLR float, count is samples per channel. */
    projectm_pcm_add_float(s_pm, interleaved, frames, PROJECTM_STEREO);
}

void viz_render_to_fbo(void)
{
    int dw = 0, dh = 0;
    if (!s_pm) {
        return;
    }
    /* libprojectM 4.1 blits to FBO 0 using its window size as the viewport. */
    plat_drawable_size(&dw, &dh);
    if (dw != s_w || dh != s_h) {
        viz_set_internal_size(dw, dh);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, dw > 0 ? dw : s_w, dh > 0 ? dh : s_h);
    projectm_opengl_render_frame(s_pm);
}

unsigned viz_texture(void)
{
    return 0;
}

int viz_tex_w(void)
{
    return s_w;
}

int viz_tex_h(void)
{
    return s_h;
}

void viz_next_preset(int hard)
{
    if (!s_pl) {
        return;
    }
    if (!viz_skip_to_allowed(1, hard)) {
        projectm_playlist_play_next(s_pl, hard ? true : false);
    }
}

void viz_prev_preset(int hard)
{
    if (!s_pl) {
        return;
    }
    if (!viz_skip_to_allowed(-1, hard)) {
        projectm_playlist_play_previous(s_pl, hard ? true : false);
    }
}

void viz_set_lock(int lock)
{
    if (s_pm) {
        projectm_set_preset_locked(s_pm, lock ? true : false);
    }
}

int viz_locked(void)
{
    return s_pm ? (projectm_get_preset_locked(s_pm) ? 1 : 0) : 0;
}

void viz_set_shuffle(int on)
{
    if (s_pl) {
        projectm_playlist_set_shuffle(s_pl, on ? true : false);
    }
}

int viz_shuffle(void)
{
    return s_pl ? (projectm_playlist_get_shuffle(s_pl) ? 1 : 0) : 0;
}

int viz_preset_count(void)
{
    return s_ncache > 0 ? s_ncache : (s_pl ? (int)projectm_playlist_size(s_pl) : 0);
}

int viz_current_index(void)
{
    return s_pl ? (int)projectm_playlist_get_position(s_pl) : 0;
}

int viz_play_index(int index, int hard)
{
    if (!s_pl || index < 0) {
        return -1;
    }
    projectm_playlist_set_position(s_pl, (uint32_t)index, hard ? true : false);
    return 0;
}

int viz_preset_name(int index, char *buf, size_t n)
{
    if (!buf || n == 0) {
        return -1;
    }
    buf[0] = '\0';
    if (index < 0 || index >= s_ncache || !s_pname) {
        return -1;
    }
    snprintf(buf, n, "%s", s_pname[index]);
    return 0;
}

int viz_preset_cat(int index, char *buf, size_t n)
{
    if (!buf || n == 0) {
        return -1;
    }
    buf[0] = '\0';
    if (index < 0 || index >= s_ncache || !s_pcat) {
        return -1;
    }
    snprintf(buf, n, "%s", s_pcat[index]);
    return 0;
}

int viz_current_label(char *buf, size_t n)
{
    return viz_preset_name(viz_current_index(), buf, n);
}

int viz_sorted_at(int rank)
{
    if (rank < 0 || rank >= s_ncache || !s_order) {
        return -1;
    }
    return s_order[rank];
}

int viz_rank_of(int playlist_index)
{
    int i;
    if (!s_order) {
        return 0;
    }
    for (i = 0; i < s_ncache; i++) {
        if (s_order[i] == playlist_index) {
            return i;
        }
    }
    return 0;
}

void viz_set_enabled(int on)
{
    s_enabled = on ? 1 : 0;
}

int viz_enabled(void)
{
    return s_enabled;
}

void viz_set_pool(VibeVizPool pool)
{
    if ((int)pool < 0 || pool > VIBE_VIZ_POOL_GOOD_ONLY) {
        pool = VIBE_VIZ_POOL_NOT_BAD;
    }
    s_pool = (int)pool;
    if (s_pl && !viz_allowed_index(viz_current_index())) {
        s_pending_skip = 1;
    }
}

VibeVizPool viz_pool(void)
{
    return (VibeVizPool)s_pool;
}

const char *viz_pool_label(VibeVizPool pool)
{
    switch (pool) {
    case VIBE_VIZ_POOL_UNREVIEWED:
        return "Unreviewed";
    case VIBE_VIZ_POOL_GOOD_ONLY:
        return "Good Only";
    case VIBE_VIZ_POOL_NOT_BAD:
    default:
        return "Not bad";
    }
}

int viz_preset_rating(int index)
{
    if (index < 0 || index >= s_ncache || !s_rate) {
        return 0;
    }
    return s_rate[index];
}

int viz_current_rating(void)
{
    return viz_preset_rating(viz_current_index());
}

int viz_rate_current(int good)
{
    int idx = viz_current_index();
    int r = good ? 1 : -1;
    if (idx < 0 || idx >= s_ncache || !s_pname) {
        return 0;
    }
    s_rate[idx] = r;
    library_viz_set_rating(s_pname[idx], r);
    return r;
}

void viz_filter_tick(void)
{
    if (!s_pending_skip || s_skip_guard) {
        return;
    }
    s_pending_skip = 0;
    s_skip_guard = 1;
    viz_next_preset(0);
    s_skip_guard = 0;
}
