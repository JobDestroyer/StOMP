#include "app.h"
#include "mpris.h"
#include "radio.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int s_page_dir;
static uint32_t s_page_next_ms;
static int s_fail_skip;

void app_play_track(App *app, const LibTrack *t)
{
    if (!app || !t || !t->path[0]) {
        return;
    }
    app->now = *t;
    app->now_valid = 1;
    app->playing_radio = 0;
    app->radio_url[0] = '\0';
    decode_open(t->path);
    decode_pause(0);
    audio_pause(0);
    ui_show_overlay();
    mpris_notify();
}

void app_play_queue_index(App *app, int idx)
{
    LibTrack t;
    library_queue_set_index(idx);
    if (library_queue_get(idx, &t) != 0) {
        return;
    }
    app_play_track(app, &t);
}

static void app_radio_skip(App *app, int dir)
{
    LibRadioCustom fav[512];
    int n, i, cur = -1;
    const char *u;
    if (!app) {
        return;
    }
    n = library_radio_fav_list(fav, 512);
    if (n <= 0) {
        return;
    }
    u = app->radio_url[0] ? app->radio_url : app->now.path;
    for (i = 0; i < n; i++) {
        if (strcmp(fav[i].url, u) == 0) {
            cur = i;
            break;
        }
    }
    if (cur < 0) {
        app_play_radio(app, fav[0].name, fav[0].url, "Favorite");
        return;
    }
    i = cur + dir;
    if (i >= n) {
        i = 0;
    }
    if (i < 0) {
        i = n - 1;
    }
    app_play_radio(app, fav[i].name, fav[i].url, "Favorite");
}

void app_next_track(App *app, int from_end)
{
    int n = library_queue_len();
    int i;
    if (app && app->playing_radio) {
        app_radio_skip(app, 1);
        return;
    }
    if (!app || n <= 0) {
        return;
    }
    i = library_queue_index();
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
    if (radio_resolve_play_url(url, play, (int)sizeof(play)) != 0 || !play[0]) {
        snprintf(play, sizeof(play), "%s", url);
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
    if (decode_position() > 2.0) {
        decode_seek(0);
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
    if (!app) {
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
    if (!decode_has_file()) {
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
    (void)app;
    audio_pause(1);
    decode_pause(1);
    decode_seek(0.0);
    mpris_notify();
}

void app_seek_delta(App *app, double seconds)
{
    double p = decode_position() + seconds;
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
}

void app_set_volume_delta(App *app, float d)
{
    float v = audio_volume() + d;
    audio_set_volume(v);
    app->cfg.volume = audio_volume();
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
    library_session_set_int("volume_milli", (int)(audio_volume() * 1000.f));
    library_session_set_int("shuffle", app->shuffle);
    library_session_set_int("repeat", (int)app->repeat);
    library_session_set_int("preset_lock", viz_locked());
    library_session_set_int("viz_enabled", viz_enabled());
    library_session_set_int("viz_pool", (int)viz_pool());
    library_session_set_int("last_view", (int)ui_view());
    library_session_set_int("position_ms", (int)(decode_position() * 1000.0));
    library_session_set_int("queue_index", library_queue_index());
    if (app->now_valid) {
        snprintf(buf, sizeof(buf), "%lld", (long long)app->now.id);
        library_session_set("track_id", buf);
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
    app_persist(app);
    audio_pause(1);
    decode_pause(1);
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
    decode_pause(0);
    audio_pause(0);
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
        app->viz_retry = 0;
    }
    if (ui_init(app->win_w, app->win_h, app->profile.veil) != 0) {
        fprintf(stderr, "StOMP: HUD init failed\n");
    }
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

    if (audio_init(&app.ring, &app.viz, app.cfg.audio_device) != 0) {
        library_close();
        plat_shutdown();
        return 1;
    }
    audio_set_volume(app.cfg.volume);

    if (decode_start(&app.ring) != 0) {
        audio_shutdown();
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
        library_close();
        plat_shutdown();
        return 1;
    }
    viz_set_shuffle(app.cfg.viz_shuffle);

    if (ui_init(app.win_w, app.win_h, app.profile.veil) != 0) {
        viz_shutdown();
        decode_stop();
        audio_shutdown();
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
            if (ev.type == SDL_TEXTINPUT) {
                ui_text_input(ev.text.text);
                ui_refresh_lists(&app);
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
                ui_note_input(SDL_GetTicks());
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
        if (ui_view() == UI_VIEW_NOW_PLAYING && seek_x != 0.f) {
            app_seek_delta(&app, (double)seek_x * 1.6);
            ui_note_input(SDL_GetTicks());
        } else {
            uint32_t now = SDL_GetTicks();
            int dir = 0;
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

        if (decode_take_failed()) {
            if (app.playing_radio) {
                s_fail_skip = 0;
                snprintf(app.status, sizeof(app.status), "Station failed");
            } else if (audio_paused()) {
                s_fail_skip = 0;
            } else if (++s_fail_skip > 16) {
                fprintf(stderr, "StOMP: stopping after repeated unreadable files\n");
                app_stop(&app);
                s_fail_skip = 0;
            } else {
                app_next_track(&app, 1);
            }
        } else if (decode_take_ended()) {
            s_fail_skip = 0;
            if (!app.playing_radio) {
                app_next_track(&app, 1);
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
            ui_idle_tick(SDL_GetTicks());
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
            if (elapsed < frame_ms) {
                SDL_Delay(frame_ms - elapsed);
            }
        }
    }

    app_persist(&app);
    mpris_shutdown();
    ui_shutdown();
    viz_shutdown();
    decode_stop();
    audio_shutdown();
    library_close();
    plat_shutdown();
    free(app.ring_storage);
    return 0;
}
