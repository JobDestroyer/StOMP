#include "config.h"

#include "input.h"

#include <SDL.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void config_strlcpy(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_sz, "%s", src);
}

static int dir_exists(const char *p)
{
    struct stat st;
    return p && p[0] && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int file_exists(const char *p)
{
    struct stat st;
    return p && p[0] && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static void mkdir_p(const char *path)
{
    char buf[VIBE_PATH_MAX];
    config_strlcpy(buf, sizeof(buf), path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    mkdir(buf, 0755);
}

int config_add_music_dir(VibeConfig *cfg, const char *path)
{
    char real[VIBE_PATH_MAX];
    char *rp;
    if (!cfg || !path || !path[0]) {
        return -1;
    }
    rp = realpath(path, NULL);
    if (rp) {
        config_strlcpy(real, sizeof(real), rp);
        free(rp);
    } else {
        config_strlcpy(real, sizeof(real), path);
    }
    for (int i = 0; i < cfg->music_dir_count; i++) {
        if (strcmp(cfg->music_dirs[i], real) == 0) {
            return 0;
        }
    }
    if (cfg->music_dir_count >= VIBE_MAX_MUSIC_DIRS) {
        return -1;
    }
    config_strlcpy(cfg->music_dirs[cfg->music_dir_count], VIBE_PATH_MAX, real);
    cfg->music_dir_count++;
    return 1;
}

void config_remove_music_dir(VibeConfig *cfg, int index)
{
    int i;
    if (!cfg || index < 0 || index >= cfg->music_dir_count) {
        return;
    }
    for (i = index; i < cfg->music_dir_count - 1; i++) {
        memcpy(cfg->music_dirs[i], cfg->music_dirs[i + 1], VIBE_PATH_MAX);
    }
    cfg->music_dir_count--;
    cfg->music_dirs[cfg->music_dir_count][0] = '\0';
}

void config_profile_params(int effective_profile, VibeProfileParams *out)
{
    if (!out) {
        return;
    }
    if (effective_profile == VIBE_PROFILE_HANDHELD) {
        out->viz_w = 960;
        out->viz_h = 600;
        out->fps = 40;
        out->mesh_w = 32;
        out->mesh_h = 24;
        out->veil = 0.38f;
    } else {
        out->viz_w = 1920;
        out->viz_h = 1080;
        out->fps = 60;
        out->mesh_w = 96;
        out->mesh_h = 72;
        out->veil = 0.28f;
    }
}

int config_resolve_effective_profile(int mode, int on_battery, int display_h)
{
    if (on_battery) {
        return VIBE_PROFILE_HANDHELD;
    }
    if (mode == VIBE_PROFILE_CINEMA) {
        return VIBE_PROFILE_CINEMA;
    }
    if (mode == VIBE_PROFILE_HANDHELD) {
        return VIBE_PROFILE_HANDHELD;
    }
    if (display_h > 0 && display_h <= 800) {
        return VIBE_PROFILE_HANDHELD;
    }
    return VIBE_PROFILE_CINEMA;
}

static void xdg_join(char *out, size_t n, const char *env, const char *fallback_home_sub, const char *app)
{
    const char *e = getenv(env);
    if (e && e[0]) {
        snprintf(out, n, "%s/%s", e, app);
        return;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        home = ".";
    }
    snprintf(out, n, "%s/%s/%s", home, fallback_home_sub, app);
}

static void try_add_media_root(VibeConfig *cfg, const char *root)
{
    DIR *d;
    struct dirent *de;
    char path[VIBE_PATH_MAX];

    if (!dir_exists(root)) {
        return;
    }
    d = opendir(root);
    if (!d) {
        return;
    }
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", root, de->d_name);
        if (dir_exists(path)) {
            config_add_music_dir(cfg, path);
            char music[VIBE_PATH_MAX];
            snprintf(music, sizeof(music), "%s/Music", path);
            if (dir_exists(music)) {
                config_add_music_dir(cfg, music);
            }
        }
    }
    closedir(d);
}

static void load_conf_file(VibeConfig *cfg, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[VIBE_PATH_MAX + 64];
    if (!f) {
        return;
    }
    while (fgets(line, (int)sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }
        if (line[0] == '#' || line[0] == '\0') {
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;
        int bind_done = 0;
        for (int i = 0; i < VIBE_BIND_COUNT; i++) {
            if (strcmp(key, input_bind_key((VibeBind)i)) == 0) {
                int v = atoi(val);
                if (v >= 0 && v <= SDL_CONTROLLER_BUTTON_MAX) {
                    cfg->bindings[i] = v;
                }
                bind_done = 1;
                break;
            }
        }
        if (bind_done) {
            continue;
        }
        if (strcmp(key, "music_dir") == 0) {
            config_add_music_dir(cfg, val);
        } else if (strcmp(key, "music_user") == 0) {
            cfg->music_user = atoi(val) != 0;
        } else if (strcmp(key, "volume") == 0) {
            cfg->volume = (float)atof(val);
            if (cfg->volume < 0.f) {
                cfg->volume = 0.f;
            }
            if (cfg->volume > 1.f) {
                cfg->volume = 1.f;
            }
        } else if (strcmp(key, "profile") == 0) {
            if (strcmp(val, "cinema") == 0) {
                cfg->profile_mode = VIBE_PROFILE_CINEMA;
            } else if (strcmp(val, "handheld") == 0) {
                cfg->profile_mode = VIBE_PROFILE_HANDHELD;
            } else {
                cfg->profile_mode = VIBE_PROFILE_AUTO;
            }
        } else if (strcmp(key, "start_library") == 0) {
            cfg->start_library = atoi(val) != 0;
        } else if (strcmp(key, "viz_shuffle") == 0) {
            cfg->viz_shuffle = atoi(val) != 0;
        } else if (strcmp(key, "audio_device") == 0) {
            config_strlcpy(cfg->audio_device, sizeof(cfg->audio_device), val);
        }
    }
    fclose(f);
}

void config_save(const VibeConfig *cfg)
{
    char path[VIBE_PATH_MAX];
    FILE *f;
    if (!cfg) {
        return;
    }
    mkdir_p(cfg->config_dir);
    snprintf(path, sizeof(path), "%s/vibe.conf", cfg->config_dir);
    f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "StOMP: cannot write %s\n", path);
        return;
    }
    fprintf(f, "music_user=%d\n", cfg->music_user ? 1 : 0);
    for (int i = 0; i < cfg->music_dir_count; i++) {
        fprintf(f, "music_dir=%s\n", cfg->music_dirs[i]);
    }
    fprintf(f, "volume=%.3f\n", (double)cfg->volume);
    fprintf(f, "profile=auto\n");
    fprintf(f, "start_library=%d\n", cfg->start_library ? 1 : 0);
    fprintf(f, "viz_shuffle=%d\n", cfg->viz_shuffle ? 1 : 0);
    if (cfg->audio_device[0]) {
        fprintf(f, "audio_device=%s\n", cfg->audio_device);
    }
    for (int i = 0; i < VIBE_BIND_COUNT; i++) {
        fprintf(f, "%s=%d\n", input_bind_key((VibeBind)i), cfg->bindings[i]);
    }
    fclose(f);
}

int config_resource_dir(char *out, size_t n)
{
    char buf[VIBE_PATH_MAX];
    ssize_t r;
    char *slash;
    if (!out || n == 0) {
        return -1;
    }
    r = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (r <= 0) {
        return -1;
    }
    buf[r] = '\0';
    slash = strrchr(buf, '/');
    if (!slash || slash == buf) {
        return -1;
    }
    *slash = '\0';
    config_strlcpy(out, n, buf);
    return 0;
}

static int find_dir(char *out, size_t n, const char *name)
{
    char cand[VIBE_PATH_MAX];
    char root[VIBE_PATH_MAX];
    const char *base;
    if (config_resource_dir(root, sizeof(root)) == 0) {
        snprintf(cand, sizeof(cand), "%s/%s", root, name);
        if (dir_exists(cand)) {
            config_strlcpy(out, n, cand);
            return 0;
        }
    }
    base = SDL_GetBasePath();
    if (base) {
        snprintf(cand, sizeof(cand), "%s%s", base, name);
        if (dir_exists(cand)) {
            config_strlcpy(out, n, cand);
            SDL_free((void *)base);
            return 0;
        }
        SDL_free((void *)base);
    }
#ifdef VIBE_SOURCE_DIR
    snprintf(cand, sizeof(cand), "%s/%s", VIBE_SOURCE_DIR, name);
    if (dir_exists(cand)) {
        config_strlcpy(out, n, cand);
        return 0;
    }
#endif
    snprintf(cand, sizeof(cand), "./%s", name);
    if (dir_exists(cand)) {
        config_strlcpy(out, n, cand);
        return 0;
    }
    return -1;
}

int config_find_file(char *out, size_t out_sz, const char *rel)
{
    char cand[VIBE_PATH_MAX];
    const char *base;
    if (!out || !rel) {
        return -1;
    }
    base = SDL_GetBasePath();
    if (base) {
        snprintf(cand, sizeof(cand), "%s%s", base, rel);
        SDL_free((void *)base);
        if (file_exists(cand)) {
            config_strlcpy(out, out_sz, cand);
            return 0;
        }
    }
#ifdef VIBE_SOURCE_DIR
    snprintf(cand, sizeof(cand), "%s/%s", VIBE_SOURCE_DIR, rel);
    if (file_exists(cand)) {
        config_strlcpy(out, out_sz, cand);
        return 0;
    }
#endif
    if (file_exists(rel)) {
        config_strlcpy(out, out_sz, rel);
        return 0;
    }
    return -1;
}

int config_init(VibeConfig *cfg, int argc, char **argv)
{
    char home_music[VIBE_PATH_MAX];
    char conf_path[VIBE_PATH_MAX];
    const char *home;

    if (!cfg) {
        return -1;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->volume = 1.0f;
    cfg->profile_mode = VIBE_PROFILE_AUTO;
    cfg->viz_shuffle = 0;
    memcpy(cfg->bindings, input_default_bindings(), sizeof(cfg->bindings));

    xdg_join(cfg->config_dir, sizeof(cfg->config_dir), "XDG_CONFIG_HOME", ".config", "vibe");
    xdg_join(cfg->data_dir, sizeof(cfg->data_dir), "XDG_DATA_HOME", ".local/share", "vibe");
    xdg_join(cfg->cache_dir, sizeof(cfg->cache_dir), "XDG_CACHE_HOME", ".cache", "vibe");
    mkdir_p(cfg->config_dir);
    mkdir_p(cfg->data_dir);
    mkdir_p(cfg->cache_dir);
    snprintf(cfg->db_path, sizeof(cfg->db_path), "%s/library.db", cfg->data_dir);

    if (find_dir(cfg->preset_dir, sizeof(cfg->preset_dir), "presets") != 0) {
        fprintf(stderr, "StOMP: presets/ not found next to binary or source tree\n");
        config_strlcpy(cfg->preset_dir, sizeof(cfg->preset_dir), "presets");
    } else {
        fprintf(stderr, "StOMP: presets %s\n", cfg->preset_dir);
    }
    if (find_dir(cfg->texture_dir, sizeof(cfg->texture_dir), "textures") != 0) {
        config_strlcpy(cfg->texture_dir, sizeof(cfg->texture_dir), "textures");
    }

    snprintf(conf_path, sizeof(conf_path), "%s/vibe.conf", cfg->config_dir);
    load_conf_file(cfg, conf_path);
    cfg->profile_mode = VIBE_PROFILE_AUTO;

    if (!cfg->music_user) {
        home = getenv("HOME");
        if (home && home[0]) {
            snprintf(home_music, sizeof(home_music), "%s/Music", home);
            if (dir_exists(home_music)) {
                config_add_music_dir(cfg, home_music);
            }
        }
        try_add_media_root(cfg, "/run/media");
        try_add_media_root(cfg, "/media");

#ifdef VIBE_SOURCE_DIR
        {
            char demo[VIBE_PATH_MAX];
            snprintf(demo, sizeof(demo), "%s/demo", VIBE_SOURCE_DIR);
            if (dir_exists(demo)) {
                config_add_music_dir(cfg, demo);
            }
        }
#endif
        {
            char demo[VIBE_PATH_MAX];
            const char *base = SDL_GetBasePath();
            if (base) {
                snprintf(demo, sizeof(demo), "%sdemo", base);
                SDL_free((void *)base);
                if (dir_exists(demo)) {
                    config_add_music_dir(cfg, demo);
                }
            }
        }
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--music-dir") == 0 && i + 1 < argc) {
            config_add_music_dir(cfg, argv[++i]);
        } else if (strncmp(argv[i], "--music-dir=", 12) == 0) {
            config_add_music_dir(cfg, argv[i] + 12);
        }
    }

    return 0;
}
