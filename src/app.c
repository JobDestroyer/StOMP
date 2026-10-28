/* SPDX-License-Identifier: LGPL-2.1-only
 * Copyright (C) 2026 JobDestroyer
 */

#include "app.h"
#include "cdg.h"
#include "mpris.h"
#include "radio.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int s_page_dir;
static uint32_t s_page_next_ms;
static int s_seek_dir;
static uint32_t s_seek_hold_ms;
static int s_nudge_dir;
static uint32_t s_nudge_next_ms;
static int s_fail_skip;
static int s_suspend_paused;

static void karaoke_stop_playback(App *app);
static void karaoke_try_cdg(const LibTrack *t)
{
    char side[VIBE_PATH_MAX];
    int kind = KARA_SIDE_NONE;
    cdg_close();
    if (!t) {
        return;
    }
    if (karaoke_song_sidecar(t->id, side, (int)sizeof(side), &kind) == 0 &&
        kind == KARA_SIDE_CDG && side[0]) {
        cdg_open(side);
    }
}

static int karaoke_menu_up(void)
{
    return ui_view() != UI_VIEW_NOW_PLAYING;
}

static void karaoke_clear_gap(App *app)
{
    if (!app) {
        return;
    }
    app->kara_gap_until = 0;
    app->kara_gap_hold_ms = 0;
}

static int karaoke_gap_active(const App *app)
{
    return app && (app->kara_gap_until || app->kara_gap_hold_ms);
}

static void karaoke_freeze_gap(App *app)
{
    uint32_t now;
    if (!app || !app->kara_gap_until) {
        return;
    }
    now = SDL_GetTicks();
    if (app->kara_gap_until > now) {
        app->kara_gap_hold_ms = app->kara_gap_until - now;
    } else {
        app->kara_gap_hold_ms = 1;
    }
    app->kara_gap_until = 0;
}

static void karaoke_resume_gap(App *app)
{
    if (!app || !app->kara_gap_hold_ms) {
        return;
    }
    app->kara_gap_until = SDL_GetTicks() + app->kara_gap_hold_ms;
    app->kara_gap_hold_ms = 0;
}

static void karaoke_sync_menu_hold(App *app)
{
    if (!app || !app->cfg.karaoke_mode) {
        return;
    }
    if (karaoke_menu_up()) {
        karaoke_freeze_gap(app);
        if (app->playing_karaoke && decode_has_file() && !audio_paused() &&
            decode_play_position() < 2.0) {
            audio_pause(1);
            decode_pause(1);
            app->kara_menu_paused = 1;
            mpris_notify();
        }
        return;
    }
    karaoke_resume_gap(app);
    if (app->kara_menu_paused) {
        app->kara_menu_paused = 0;
        if (decode_has_file()) {
            decode_pause(0);
            audio_pause(0);
            mpris_notify();
        }
    }
}

int app_play_track(App *app, const LibTrack *t)
{
    const char *open_path;
    if (!app || !t || !t->path[0]) {
        return -1;
    }
    open_path = t->path;
    if (karaoke_path_is_zip(t->path)) {
        if (karaoke_zip_prepare(t->path) != 0 || !karaoke_zip_audio_path()[0]) {
            snprintf(app->status, sizeof(app->status), "Could not open zip");
            return -1;
        }
        open_path = karaoke_zip_audio_path();
    }
    cdg_close();
    app->now = *t;
    app->now_valid = 1;
    app->playing_radio = 0;
    app->playing_karaoke = 0;
    karaoke_clear_gap(app);
    app->kara_menu_paused = 0;
    app->radio_url[0] = '\0';
    decode_open(open_path);
    decode_pause(0);
    audio_pause(0);
    ui_show_overlay();
    if (app->cfg.karaoke_mode && ui_view() == UI_VIEW_NOW_PLAYING) {
        ui_hide_overlay();
    }
    mpris_notify();
    return 0;
}

void app_play_queue_index(App *app, int idx)
{
    LibTrack t;
    if (!app) {
        return;
    }
    if (app->cfg.karaoke_mode || app->playing_karaoke) {
        int tries = 0;
        karaoke_queue_set_index(idx);
        while (tries++ < 16) {
            if (karaoke_queue_get(karaoke_queue_index(), &t) != 0) {
                karaoke_stop_playback(app);
                return;
            }
            if (app_play_track(app, &t) == 0) {
                app->playing_karaoke = 1;
                karaoke_try_cdg(&t);
                return;
            }
            karaoke_queue_remove(karaoke_queue_index());
            ui_refresh_lists(app);
            if (karaoke_queue_len() <= 0) {
                karaoke_stop_playback(app);
                return;
            }
        }
        karaoke_stop_playback(app);
        return;
    }
    library_queue_set_index(idx);
    if (library_queue_get(idx, &t) != 0) {
        return;
    }
    app_play_track(app, &t);
}

static void app_radio_skip(App *app, int dir)
{
    LibRadioCustom next;
    const char *u;
    if (!app) {
        return;
    }
    u = app->radio_url[0] ? app->radio_url : app->now.path;
    if (library_radio_fav_step(u, dir, &next) != 0) {
        return;
    }
    app_play_radio(app, next.name, next.url, "Favorite");
}

static void karaoke_stop_playback(App *app)
{
    karaoke_clear_gap(app);
    app->kara_menu_paused = 0;
    if (app->playing_karaoke) {
        karaoke_session_set_int("position_ms", 0);
        karaoke_session_set_int("queue_index", karaoke_queue_index());
        if (app->now_valid && app->now.id > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)app->now.id);
            karaoke_session_set("track_id", buf);
        }
        karaoke_queue_save();
    }
    app->playing_karaoke = 0;
    app->now_valid = 0;
    memset(&app->now, 0, sizeof(app->now));
    cdg_close();
    decode_stop_file();
    audio_pause(1);
    mpris_notify();
}

void app_next_track(App *app, int from_end)
{
    int n, i;
    int kara;
    if (app && app->playing_radio) {
        app_radio_skip(app, 1);
        return;
    }
    if (!app) {
        return;
    }
    kara = app->playing_karaoke || app->cfg.karaoke_mode;
    if (kara) {
        if (!app->playing_karaoke && !karaoke_gap_active(app)) {
            if (karaoke_queue_len() > 0) {
                app_play_queue_index(app, karaoke_queue_index());
            }
            return;
        }
        if (karaoke_gap_active(app) && !from_end) {
            karaoke_clear_gap(app);
            if (karaoke_queue_len() > 0) {
                app_play_queue_index(app, karaoke_queue_index());
            } else {
                karaoke_stop_playback(app);
            }
            return;
        }
        i = karaoke_queue_index();
        n = karaoke_queue_len();
        if (n <= 0 || i < 0 || i >= n) {
            karaoke_stop_playback(app);
            return;
        }
        if (i + 1 >= n) {
            karaoke_queue_remove(i);
            ui_refresh_lists(app);
            karaoke_stop_playback(app);
            return;
        }
        karaoke_queue_remove(i);
        karaoke_queue_set_index(i);
        ui_refresh_lists(app);
        if (from_end) {
            karaoke_clear_gap(app);
            app->kara_gap_until = SDL_GetTicks() + 5000;
            cdg_close();
            decode_stop_file();
            audio_pause(1);
            if (!karaoke_menu_up()) {
                ui_set_view(UI_VIEW_NOW_PLAYING);
            }
            mpris_notify();
            return;
        }
        app_play_queue_index(app, karaoke_queue_index());
        return;
    }
    n = library_queue_len();
    i = library_queue_index();
    if (n <= 0) {
        return;
    }
    if (app->repeat == VIBE_REPEAT_ONE && from_end) {
        app_play_queue_index(app, i);
        return;
    }
    if (app->shuffle && n > 1) {
        int next = i;
        int guard = 0;
        while (next == i && guard++ < 16) {
            next = rand() % n;
        }
        app_play_queue_index(app, next);
        return;
    }
    i++;
    if (i >= n) {
        if (app->repeat == VIBE_REPEAT_ALL) {
            i = 0;
        } else {
            decode_stop_file();
            audio_pause(1);
            mpris_notify();
            return;
        }
    }
    app_play_queue_index(app, i);
}

void app_play_radio(App *app, const char *title, const char *url, const char *subtitle)
{
    char play[VIBE_PATH_MAX];
    if (!app || !url || !url[0]) {
        return;
    }
    play[0] = '\0';
    if (radio_resolve_play_url(url, play, (int)sizeof(play)) != 0 || !play[0] ||
        !radio_url_is_http(play)) {
        snprintf(app->status, sizeof(app->status), "No connection  ·  could not reach the station");
        return;
    }
    memset(&app->now, 0, sizeof(app->now));
    snprintf(app->radio_url, sizeof(app->radio_url), "%s", url);
    snprintf(app->now.path, sizeof(app->now.path), "%s", play);
    snprintf(app->now.title, sizeof(app->now.title), "%s", title && title[0] ? title : "Internet Radio");
    snprintf(app->now.artist, sizeof(app->now.artist), "%s",
             subtitle && subtitle[0] ? subtitle : "Internet Radio");
    snprintf(app->now.album, sizeof(app->now.album), "%s", "Radio");
    app->now_valid = 1;
    app->playing_radio = 1;
    app->playing_karaoke = 0;
    karaoke_clear_gap(app);
    app->kara_menu_paused = 0;
    cdg_close();
    decode_open(play);
    decode_pause(0);
    audio_pause(0);
    ui_show_overlay();
    mpris_notify();
}

void app_prev_track(App *app)
{
    int i;
    if (!app) {
        return;
    }
    if (app->playing_radio) {
        app_radio_skip(app, -1);
        return;
    }
    if (decode_play_position() > 2.0) {
        decode_seek(0);
        return;
    }
    if (app->playing_karaoke || app->cfg.karaoke_mode) {
        i = karaoke_queue_index() - 1;
        if (i < 0) {
            decode_seek(0);
            return;
        }
        app_play_queue_index(app, i);
        return;
    }
    i = library_queue_index() - 1;
    if (i < 0) {
        if (app->repeat == VIBE_REPEAT_ALL && library_queue_len() > 0) {
            i = library_queue_len() - 1;
        } else {
            decode_seek(0);
            return;
        }
    }
    app_play_queue_index(app, i);
}

void app_resume_playback(App *app)
{
    LibTrack t;
    int pos = 0;
    char title[VIBE_NAME_MAX];
    char url[VIBE_PATH_MAX];
    char sub[VIBE_NAME_MAX];
    if (!app) {
        return;
    }
    if (app->cfg.karaoke_mode) {
        if (karaoke_resume_song(&t, &pos) != 0 || !t.path[0]) {
            return;
        }
        if (app_play_track(app, &t) != 0) {
            return;
        }
        app->playing_karaoke = 1;
        karaoke_try_cdg(&t);
        if (pos > 0) {
            decode_seek((double)pos / 1000.0);
        }
        ui_set_view(UI_VIEW_NOW_PLAYING);
        return;
    }
    if (library_resume_radio(title, (int)sizeof(title), url, (int)sizeof(url),
                             sub, (int)sizeof(sub)) == 0 && url[0]) {
        app_play_radio(app, title[0] ? title : "Internet Radio", url, sub[0] ? sub : "Internet Radio");
        ui_set_view(UI_VIEW_NOW_PLAYING);
        return;
    }
    if (library_resume_track(&t, &pos) != 0 || !t.path[0]) {
        return;
    }
    library_queue_load();
    app_play_track(app, &t);
    if (pos > 0) {
        decode_seek((double)pos / 1000.0);
    }
    ui_set_view(UI_VIEW_NOW_PLAYING);
}

void app_toggle_pause(App *app)
{
    int p;
    if (app && app->cfg.karaoke_mode && !app->playing_karaoke) {
        return;
    }
    if (!decode_has_file()) {
        if (app && karaoke_gap_active(app)) {
            return;
        }
        if (app && app->now_valid && (app->playing_karaoke || app->playing_radio)) {
            p = !audio_paused();
            audio_pause(p);
            decode_pause(p);
            mpris_notify();
            return;
        }
        app_resume_playback(app);
        return;
    }
    p = !audio_paused();
    audio_pause(p);
    decode_pause(p);
    mpris_notify();
}

void app_stop(App *app)
{
    if (app) {
        karaoke_clear_gap(app);
        app->kara_menu_paused = 0;
    }
    audio_pause(1);
    decode_pause(1);
    decode_seek(0.0);
    mpris_notify();
}

void app_halt(App *app)
{
    cdg_close();
    decode_stop_file();
    audio_pause(1);
    if (app) {
        if (app->playing_karaoke) {
            karaoke_session_set("track_id", "");
            karaoke_session_set_int("position_ms", 0);
            karaoke_queue_save();
        }
        app->now_valid = 0;
        app->playing_radio = 0;
        app->playing_karaoke = 0;
        karaoke_clear_gap(app);
        app->kara_menu_paused = 0;
        app->radio_url[0] = '\0';
        memset(&app->now, 0, sizeof(app->now));
        if (!app->cfg.karaoke_mode) {
            app_persist(app);
        }
    }
    mpris_notify();
}

static void stop_now(App *app)
{
    cdg_close();
    decode_stop_file();
    audio_pause(1);
    app->now_valid = 0;
    app->playing_radio = 0;
    app->playing_karaoke = 0;
    karaoke_clear_gap(app);
    app->kara_menu_paused = 0;
    app->radio_url[0] = '\0';
    memset(&app->now, 0, sizeof(app->now));
    mpris_notify();
}

void app_set_karaoke_mode(App *app, int on)
{
    if (!app) {
        return;
    }
    on = on ? 1 : 0;
    if (on) {
        if (app->playing_radio || (app->now_valid && !app->playing_karaoke)) {
            app_persist(app);
            stop_now(app);
        }
        app->cfg.karaoke_mode = 1;
        karaoke_queue_clear();
        karaoke_queue_save();
        karaoke_session_set_int("queue_index", 0);
    } else {
        if (app->playing_karaoke) {
            app_persist(app);
            stop_now(app);
        }
        app->cfg.karaoke_mode = 0;
    }
    config_save(&app->cfg);
}

void app_seek_delta(App *app, double seconds)
{
    double p = decode_play_position() + seconds;
    double d = decode_duration();
    (void)app;
    if (app && (app->playing_radio || decode_is_live())) {
        return;
    }
    if (p < 0) {
        p = 0;
    }
    if (d > 0 && p > d) {
        p = d;
    }
    decode_seek(p);
    mpris_notify_seeked();
}

void app_seek_frac(App *app, float t)
{
    double d;
    if (app && (app->playing_radio || decode_is_live())) {
        return;
    }
    d = decode_duration();
    if (d <= 0.1) {
        return;
    }
    if (t < 0.f) {
        t = 0.f;
    }
    if (t > 1.f) {
        t = 1.f;
    }
    decode_seek((double)t * d);
    mpris_notify_seeked();
}

void app_set_volume_delta(App *app, float d)
{
    float v = audio_volume() + d;
    audio_set_volume(v);
    if (app) {
        app->cfg.volume = audio_volume();
    }
    mpris_notify();
}

void app_apply_profile(App *app, int force)
{
    int next = config_resolve_effective_profile(app->cfg.profile_mode,
                                                plat_on_battery(),
                                                plat_display_h());
    VibeProfileParams p;
    if (!force && next == app->effective_profile) {
        return;
    }
    app->effective_profile = next;
    config_profile_params(next, &p);
    app->profile = p;
    plat_drawable_size(&p.viz_w, &p.viz_h);
    if (p.viz_w < 64) {
        p.viz_w = 64;
    }
    if (p.viz_h < 64) {
        p.viz_h = 64;
    }
    app->profile.viz_w = p.viz_w;
    app->profile.viz_h = p.viz_h;
    app->mesh_w = p.mesh_w;
    app->mesh_h = p.mesh_h;
    app->frame_n = 0;
    app->frame_miss = 0;
    if (viz_ready()) {
        viz_set_internal_size(p.viz_w, p.viz_h);
        viz_set_mesh(p.mesh_w, p.mesh_h);
        viz_set_fps(p.fps);
    }
    ui_set_veil(p.veil);
    fprintf(stderr, "StOMP: profile %s (drawable %dx%d @ %d fps, mesh %dx%d)\n",
            next == VIBE_PROFILE_HANDHELD ? "handheld" : "cinema",
            p.viz_w, p.viz_h, p.fps, p.mesh_w, p.mesh_h);
}

void app_persist(App *app)
{
    char buf[32];
    if (!app) {
        return;
    }
    library_session_set_int("volume_milli", (int)(audio_volume() * 1000.f));
    library_session_set_int("shuffle", app->shuffle);
    library_session_set_int("repeat", (int)app->repeat);
    library_session_set_int("preset_lock", viz_locked());
    library_session_set_int("viz_enabled", viz_enabled());
    library_session_set_int("viz_pool", (int)viz_pool());
    library_session_set_int("last_view", (int)ui_view());
    library_session_set_int("position_ms", (int)(decode_play_position() * 1000.0));
    if (app->playing_karaoke) {
        karaoke_session_set_int("position_ms", (int)(decode_play_position() * 1000.0));
        karaoke_session_set_int("queue_index", karaoke_queue_index());
        if (app->now_valid && app->now.id > 0) {
            snprintf(buf, sizeof(buf), "%lld", (long long)app->now.id);
            karaoke_session_set("track_id", buf);
        } else {
            karaoke_session_set("track_id", "");
            karaoke_session_set_int("position_ms", 0);
        }
        karaoke_queue_save();
        return;
    }
    library_session_set_int("queue_index", library_queue_index());
    if (app->playing_radio && app->radio_url[0]) {
        library_session_set_int("playing_radio", 1);
        library_session_set("radio_url", app->radio_url);
        library_session_set("radio_title", app->now.title);
        library_session_set("radio_sub", app->now.artist);
        library_session_set("track_id", "");
    } else {
        library_session_set_int("playing_radio", 0);
        library_session_set("radio_url", "");
        library_session_set("radio_title", "");
        library_session_set("radio_sub", "");
        if (app->now_valid && app->now.id > 0) {
            snprintf(buf, sizeof(buf), "%lld", (long long)app->now.id);
            library_session_set("track_id", buf);
        } else {
            library_session_set("track_id", "");
            library_session_set_int("position_ms", 0);
        }
    }
    library_queue_save();
}

void app_restore_session(App *app)
{
    int vol_m;
    app->shuffle = library_session_get_int("shuffle", 0);
    app->repeat = (VibeRepeat)library_session_get_int("repeat", VIBE_REPEAT_OFF);
    viz_set_lock(library_session_get_int("preset_lock", 0));
    viz_set_enabled(library_session_get_int("viz_enabled", 1));
    viz_set_pool((VibeVizPool)library_session_get_int("viz_pool", VIBE_VIZ_POOL_NOT_BAD));
    app->cfg.profile_mode = VIBE_PROFILE_AUTO;
    vol_m = library_session_get_int("volume_milli", (int)(app->cfg.volume * 1000.f));
    audio_set_volume((float)vol_m / 1000.f);
    library_queue_load();
    if (app->cfg.karaoke_mode) {
        karaoke_queue_clear();
        karaoke_queue_save();
        karaoke_session_set_int("queue_index", 0);
    } else {
        karaoke_queue_load();
    }
}

static void handle_cmd(App *app, VibeCmd cmd, int repeat)
{
    switch (cmd) {
    case VIBE_CMD_QUIT:
        app->running = 0;
        break;
    case VIBE_CMD_PREV_TRACK:
        app_prev_track(app);
        break;
    case VIBE_CMD_NEXT_TRACK:
        app_next_track(app, 0);
        break;
    case VIBE_CMD_VOL_DOWN:
        app_set_volume_delta(app, -0.04f);
        break;
    case VIBE_CMD_VOL_UP:
        app_set_volume_delta(app, 0.04f);
        break;
    case VIBE_CMD_STOP:
        app_stop(app);
        break;
    case VIBE_CMD_PRESET_NEXT:
        viz_next_preset(0);
        break;
    case VIBE_CMD_PRESET_PREV:
        viz_prev_preset(0);
        break;
    case VIBE_CMD_SEEK_BACK:
        app_seek_delta(app, -5.0);
        break;
    case VIBE_CMD_SEEK_FWD:
        app_seek_delta(app, 5.0);
        break;
    default:
        ui_handle(app, cmd, 0.f, repeat);
        break;
    }
}

static void suspend(App *app)
{
    if (app->suspended) {
        return;
    }
    fprintf(stderr, "StOMP: suspend\n");
    s_suspend_paused = audio_paused();
    app_persist(app);
    audio_pause(1);
    decode_pause(1);
    cdg_lost_gl();
    viz_shutdown();
    ui_shutdown();
    plat_destroy_gl();
    app->suspended = 1;
}

static int resume_app(App *app)
{
    if (!app->suspended) {
        return 0;
    }
    fprintf(stderr, "StOMP: resume\n");
    decode_pause(s_suspend_paused);
    audio_pause(s_suspend_paused);
    if (plat_recreate_gl() != 0) {
        fprintf(stderr, "StOMP: GL recreate failed; audio continues\n");
        app->suspended = 0;
        app->viz_retry = 1;
        return 0;
    }
    if (viz_init(app->profile.viz_w, app->profile.viz_h,
                 app->mesh_w, app->mesh_h, app->profile.fps,
                 app->cfg.preset_dir, app->cfg.texture_dir) != 0) {
        fprintf(stderr, "StOMP: vis init failed; will retry\n");
        app->viz_retry = 1;
    } else {
        viz_set_shuffle(app->cfg.viz_shuffle);
        viz_set_lock(library_session_get_int("preset_lock", 0));
        viz_set_enabled(library_session_get_int("viz_enabled", 1));
        viz_set_pool((VibeVizPool)library_session_get_int("viz_pool", VIBE_VIZ_POOL_NOT_BAD));
        app->viz_retry = 0;
    }
    if (ui_init(app->win_w, app->win_h, app->profile.veil) != 0) {
        fprintf(stderr, "StOMP: HUD init failed\n");
    }
    input_init();
    ui_refresh_lists(app);
    app->suspended = 0;
    return 0;
}

static void print_help(void)
{
    fprintf(stdout,
            "StOMP — ProjectM cinema music player\n"
            "Usage: StOMP [options] [audio-file]\n"
            "  --music-dir PATH   Add a library root (repeatable)\n"
            "  --windowed         Windowed 1280x720 (dev)\n"
            "  --help             This text\n");
}

int app_run(int argc, char **argv)
{
    App app;
    int windowed = 0;
    char cli_file[VIBE_PATH_MAX];
    uint32_t last_seq_feed = 0;
    int scan_started = 0;

    memset(&app, 0, sizeof(app));
    cli_file[0] = '\0';
    srand((unsigned)time(NULL));

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help();
            return 0;
        }
        if (strcmp(argv[i], "--windowed") == 0) {
            windowed = 1;
        } else if (strcmp(argv[i], "--music-dir") == 0 && i + 1 < argc) {
            i++;
        } else if (argv[i][0] != '-') {
            snprintf(cli_file, sizeof(cli_file), "%s", argv[i]);
        }
    }

    if (config_init(&app.cfg, argc, argv) != 0) {
        return 1;
    }
    app.cfg.profile_mode = VIBE_PROFILE_AUTO;

    app.ring_storage = (float *)calloc((size_t)VIBE_RING_FRAMES * 2u, sizeof(float));
    if (!app.ring_storage) {
        fprintf(stderr, "StOMP: ring alloc failed\n");
        return 1;
    }
    if (ringbuf_init(&app.ring, VIBE_RING_FRAMES, app.ring_storage) != 0) {
        fprintf(stderr, "StOMP: ringbuf_init failed\n");
        return 1;
    }

    plat_set_vsync(app.cfg.vsync);
    if (plat_init(windowed, windowed ? 1280 : 0, windowed ? 720 : 0) != 0) {
        return 1;
    }
    plat_size(&app.win_w, &app.win_h);
    app.windowed = windowed;
    app_apply_profile(&app, 1);

    if (library_open(app.cfg.db_path) != 0) {
        plat_shutdown();
        return 1;
    }
    library_set_scan_roots(app.cfg.music_dirs, app.cfg.music_dir_count);
    if (karaoke_open(app.cfg.karaoke_db_path) != 0) {
        fprintf(stderr, "StOMP: karaoke catalog unavailable\n");
    }
    karaoke_set_scan_roots(app.cfg.karaoke_dirs, app.cfg.karaoke_dir_count);

    if (audio_init(&app.ring, &app.viz, app.cfg.audio_device) != 0) {
        karaoke_close();
        library_close();
        plat_shutdown();
        return 1;
    }
    audio_set_volume(app.cfg.volume);

    if (decode_start(&app.ring) != 0) {
        audio_shutdown();
        karaoke_close();
        library_close();
        plat_shutdown();
        return 1;
    }
    radio_init();

    plat_make_current();
    if (viz_init(app.profile.viz_w, app.profile.viz_h,
                 app.profile.mesh_w, app.profile.mesh_h, app.profile.fps,
                 app.cfg.preset_dir, app.cfg.texture_dir) != 0) {
        fprintf(stderr, "StOMP: visualization init failed\n");
        decode_stop();
        audio_shutdown();
        karaoke_close();
        library_close();
        plat_shutdown();
        return 1;
    }
    viz_set_shuffle(app.cfg.viz_shuffle);

    if (ui_init(app.win_w, app.win_h, app.profile.veil) != 0) {
        viz_shutdown();
        decode_stop();
        audio_shutdown();
        karaoke_close();
        library_close();
        plat_shutdown();
        return 1;
    }
    input_init();
    input_apply_bindings(app.cfg.bindings);

    decode_request_scan();
    scan_started = 1;
    ui_set_view(UI_VIEW_NOW_PLAYING);
    ui_show_overlay();
    if (!cli_file[0]) {
        app_restore_session(&app);
    }
    mpris_init(&app);

    if (cli_file[0]) {
        LibTrack t;
        memset(&t, 0, sizeof(t));
        snprintf(t.path, sizeof(t.path), "%s", cli_file);
        library_read_tags(cli_file, &t);
        library_queue_play_track(0);
        app_play_track(&app, &t);
    }

    app.running = 1;
    fprintf(stderr, "StOMP: running %dx%d, viz %dx%d\n",
            app.win_w, app.win_h, app.profile.viz_w, app.profile.viz_h);

    while (app.running) {
        SDL_Event ev;
        VibeCmd cmd;
        float seek_x = 0.f, page_y = 0.f;
        uint32_t frame_start = SDL_GetTicks();
        uint32_t frame_ms = (uint32_t)(1000 / (app.profile.fps > 0 ? app.profile.fps : 60));
        unsigned page;
        uint32_t nframes;

        while (plat_poll(&ev)) {
            if (ev.type == SDL_CONTROLLERBUTTONDOWN && ui_binding_mode()) {
                ui_binding_captured((int)ev.cbutton.button);
                memcpy(app.cfg.bindings, input_binds(), sizeof(app.cfg.bindings));
                config_save(&app.cfg);
                ui_refresh_lists(&app);
                ui_note_input(SDL_GetTicks());
                continue;
            }
            if (ev.type == SDL_APP_WILLENTERBACKGROUND ||
                (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_MINIMIZED)) {
                suspend(&app);
                continue;
            }
            if (ev.type == SDL_APP_DIDENTERFOREGROUND ||
                (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_RESTORED)) {
                if (resume_app(&app) != 0) {
                    app.running = 0;
                }
                continue;
            }
            if (ev.type == SDL_WINDOWEVENT &&
                (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                 ev.window.event == SDL_WINDOWEVENT_RESIZED)) {
                app.pending_resize = 1;
                app.resize_stamp_ms = SDL_GetTicks();
                plat_size(&app.win_w, &app.win_h);
                ui_resize(app.win_w, app.win_h);
                continue;
            }
            if (ev.type == SDL_MOUSEMOTION ||
                ev.type == SDL_MOUSEBUTTONDOWN ||
                ev.type == SDL_MOUSEBUTTONUP ||
                ev.type == SDL_MOUSEWHEEL) {
                ui_mouse(&app, &ev);
                continue;
            }
            if (ev.type == SDL_TEXTINPUT) {
                ui_text_input(ev.text.text);
                if (ui_view() == UI_VIEW_SEARCH) {
                    ui_refresh_lists(&app);
                }
                ui_note_input(SDL_GetTicks());
                continue;
            }
            if (ev.type == SDL_CONTROLLERDEVICEADDED) {
                plat_pad_added(ev.cdevice.which);
                continue;
            }
            if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
                plat_pad_removed(ev.cdevice.which);
                continue;
            }
            if (ev.type == SDL_AUDIODEVICEREMOVED) {
                fprintf(stderr, "StOMP: audio device lost, reopening\n");
                audio_reopen(app.cfg.audio_device[0] ? app.cfg.audio_device : NULL);
                continue;
            }
            if (input_process_event(&ev, &cmd) && cmd != VIBE_CMD_NONE) {
                if (cmd != VIBE_CMD_BACK) {
                    ui_note_input(SDL_GetTicks());
                }
                handle_cmd(&app, cmd, 0);
            }
        }
        {
            int held = 0;
            if (input_poll_repeat(&cmd, &held) && cmd != VIBE_CMD_NONE) {
                ui_note_input(SDL_GetTicks());
                handle_cmd(&app, cmd, held);
            }
        }
        input_axes(&seek_x, &page_y);
        {
            uint32_t now = SDL_GetTicks();
            if (ui_view() == UI_VIEW_NOW_PLAYING) {
                float ax = seek_x < 0.f ? -seek_x : seek_x;
                float ay = page_y < 0.f ? -page_y : page_y;
                int vertical = ay >= 0.65f && ay + 0.08f >= ax;
                int horizontal = ax >= 0.55f && ax > ay + 0.12f;
                if (app.cfg.karaoke_mode && !ui_overlay_visible()) {
                    s_seek_dir = 0;
                    if (vertical && page_y < 0.f) {
                        if (s_nudge_dir != 1) {
                            s_nudge_dir = 1;
                            ui_handle(&app, VIBE_CMD_UP, 0.f, 0);
                        }
                    } else {
                        s_nudge_dir = 0;
                    }
                } else if (app.cfg.karaoke_mode && s_nudge_dir == 1 && page_y < -0.5f) {
                    s_seek_dir = 0;
                } else if (vertical) {
                    int ydir = page_y < 0.f ? 1 : -1;
                    s_seek_dir = 0;
                    if (ydir != s_nudge_dir) {
                        s_nudge_dir = ydir;
                        s_nudge_next_ms = now + 500;
                        app_seek_delta(&app, ydir > 0 ? 30.0 : -10.0);
                        ui_note_input(now);
                    } else if (now >= s_nudge_next_ms) {
                        s_nudge_next_ms = now + 500;
                        app_seek_delta(&app, ydir > 0 ? 30.0 : -10.0);
                        ui_note_input(now);
                    }
                } else if (horizontal) {
                    int dir = seek_x > 0.f ? 1 : -1;
                    double rate = 1.6;
                    uint32_t held;
                    s_nudge_dir = 0;
                    if (dir != s_seek_dir) {
                        s_seek_dir = dir;
                        s_seek_hold_ms = now;
                    }
                    held = now - s_seek_hold_ms;
                    if (held >= 12000) {
                        rate = 1.6 * 8.0;
                    } else if (held >= 3000) {
                        rate = 1.6 * 3.0;
                    }
                    app_seek_delta(&app, (double)seek_x * rate);
                    ui_note_input(now);
                } else {
                    s_seek_dir = 0;
                    s_nudge_dir = 0;
                }
            } else {
                int dir = 0;
                s_seek_dir = 0;
                s_nudge_dir = 0;
                if (page_y < -0.7f) {
                    dir = -1;
                } else if (page_y > 0.7f) {
                    dir = 1;
                }
                if (dir == 0) {
                    s_page_dir = 0;
                } else if (dir != s_page_dir) {
                    s_page_dir = dir;
                    s_page_next_ms = now + 280;
                    ui_handle(&app, dir < 0 ? VIBE_CMD_PAGE_UP : VIBE_CMD_PAGE_DOWN, 0.f, 0);
                    ui_note_input(now);
                } else if (now >= s_page_next_ms) {
                    s_page_next_ms = now + 70;
                    ui_handle(&app, dir < 0 ? VIBE_CMD_PAGE_UP : VIBE_CMD_PAGE_DOWN, 0.f, 1);
                    ui_note_input(now);
                }
            }
        }

        if (app.pending_resize && SDL_GetTicks() - app.resize_stamp_ms > 250) {
            app.pending_resize = 0;
            app_apply_profile(&app, 0);
        }

        if (SDL_GetTicks() - app.last_profile_check_ms > 2000) {
            app.last_profile_check_ms = SDL_GetTicks();
            app_apply_profile(&app, 0);
            if (app.viz_retry && !app.suspended && !viz_ready()) {
                if (viz_init(app.profile.viz_w, app.profile.viz_h,
                             app.mesh_w, app.mesh_h, app.profile.fps,
                             app.cfg.preset_dir, app.cfg.texture_dir) == 0) {
                    viz_set_shuffle(app.cfg.viz_shuffle);
                    app.viz_retry = 0;
                    fprintf(stderr, "StOMP: vis recovered\n");
                }
            }
        }

        if (scan_started && !decode_scan_busy()) {
            scan_started = 0;
            if (app.cfg.start_library && ui_view() == UI_VIEW_NOW_PLAYING && !app.now_valid) {
                ui_set_view(UI_VIEW_LIBRARY);
            }
            ui_refresh_lists(&app);
        }

        if (app.playing_radio && decode_live_net() == 0 &&
            (strcmp(app.status, "No connection") == 0 ||
             strcmp(app.status, "No connection  ·  could not reach the station") == 0)) {
            app.status[0] = '\0';
        }

        if (decode_take_failed()) {
            if (app.playing_radio) {
                s_fail_skip = 0;
                snprintf(app.status, sizeof(app.status), "No connection");
            } else if (++s_fail_skip > 16) {
                fprintf(stderr, "StOMP: stopping after repeated unreadable files\n");
                app_stop(&app);
                s_fail_skip = 0;
            } else {
                int from_end = !app.playing_karaoke || karaoke_menu_up();
                app_next_track(&app, from_end);
            }
        } else if (decode_take_ended()) {
            s_fail_skip = 0;
            if (!app.playing_radio && !karaoke_gap_active(&app)) {
                app_next_track(&app, 1);
            }
        }

        karaoke_sync_menu_hold(&app);

        if (app.kara_gap_until) {
            if (karaoke_queue_len() <= 0) {
                karaoke_stop_playback(&app);
            } else if (SDL_GetTicks() >= app.kara_gap_until) {
                karaoke_clear_gap(&app);
                app_play_queue_index(&app, karaoke_queue_index());
            }
        }

        if (!app.suspended) {
            page = atomic_load_explicit(&app.viz.page, memory_order_acquire);
            nframes = app.viz.nframes[page];
            if (nframes > 0 && page != last_seq_feed) {
                viz_feed_pcm(app.viz.pcm[page], nframes);
                last_seq_feed = page;
            }
            viz_filter_tick();
            if (viz_enabled()) {
                viz_render_to_fbo();
            } else {
                int dw = 0, dh = 0;
                plat_drawable_size(&dw, &dh);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glViewport(0, 0, dw > 0 ? dw : app.win_w, dh > 0 ? dh : app.win_h);
                glClearColor(0.f, 0.f, 0.f, 1.f);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            if (cdg_active()) {
                int dw = 0, dh = 0;
                plat_drawable_size(&dw, &dh);
                cdg_update(decode_play_position());
                cdg_present(dw > 0 ? dw : app.win_w, dh > 0 ? dh : app.win_h);
            }
            ui_idle_tick(&app, SDL_GetTicks());
            ui_draw(&app);
            plat_swap();
            mpris_poll();
            {
                uint32_t drew = SDL_GetTicks() - frame_start;
                if (drew > frame_ms + 6) {
                    app.frame_miss++;
                }
                app.frame_n++;
                if (app.frame_n >= 90 && viz_ready()) {
                    if (app.frame_miss > 20 && (app.mesh_w > 32 || app.mesh_h > 24)) {
                        app.mesh_w = app.mesh_w * 3 / 4;
                        app.mesh_h = app.mesh_h * 3 / 4;
                        if (app.mesh_w < 32) {
                            app.mesh_w = 32;
                        }
                        if (app.mesh_h < 24) {
                            app.mesh_h = 24;
                        }
                        viz_set_mesh(app.mesh_w, app.mesh_h);
                        fprintf(stderr, "StOMP: mesh %dx%d (dropping frames)\n",
                                app.mesh_w, app.mesh_h);
                    } else if (app.frame_miss < 4 &&
                               (app.mesh_w < app.profile.mesh_w ||
                                app.mesh_h < app.profile.mesh_h)) {
                        app.mesh_w += 8;
                        app.mesh_h += 6;
                        if (app.mesh_w > app.profile.mesh_w) {
                            app.mesh_w = app.profile.mesh_w;
                        }
                        if (app.mesh_h > app.profile.mesh_h) {
                            app.mesh_h = app.profile.mesh_h;
                        }
                        viz_set_mesh(app.mesh_w, app.mesh_h);
                    }
                    app.frame_n = 0;
                    app.frame_miss = 0;
                }
            }
        }

        if (SDL_GetTicks() - app.last_persist_ms > 5000) {
            app.last_persist_ms = SDL_GetTicks();
            app_persist(&app);
        }

        {
            uint32_t elapsed = SDL_GetTicks() - frame_start;
            if (!plat_vsync() && elapsed < frame_ms) {
                SDL_Delay(frame_ms - elapsed);
            }
        }
    }

    app_persist(&app);
    mpris_shutdown();
    cdg_shutdown();
    ui_shutdown();
    viz_shutdown();
    decode_stop();
    audio_shutdown();
    karaoke_close();
    library_close();
    plat_shutdown();
    free(app.ring_storage);
    return 0;
}
