#ifndef VIBE_CONFIG_H
#define VIBE_CONFIG_H

#include <stddef.h>

#define VIBE_PATH_MAX 1024
#define VIBE_NAME_MAX 256
#define VIBE_MAX_MUSIC_DIRS 8
#define VIBE_SAMPLE_RATE 44100
#define VIBE_CHANNELS 2
#define VIBE_RING_FRAMES 65536
#define VIBE_VIZ_FRAMES 1024
#define VIBE_DECODE_SCRATCH_FRAMES 8192
#define VIBE_QUEUE_MAX 1024
#define VIBE_LIST_MAX 4096
#define VIBE_UI_ROWS_MAX 16384

typedef enum {
    VIBE_PROFILE_AUTO = 0,
    VIBE_PROFILE_CINEMA = 1,
    VIBE_PROFILE_HANDHELD = 2
} VibeProfileMode;

typedef struct {
    int viz_w;
    int viz_h;
    int fps;
    int mesh_w;
    int mesh_h;
    float veil;
} VibeProfileParams;

typedef enum {
    VIBE_REPEAT_OFF = 0,
    VIBE_REPEAT_ALL = 1,
    VIBE_REPEAT_ONE = 2
} VibeRepeat;

/* Controller actions the user may remap to any gamepad button. */
typedef enum {
    VIBE_BIND_CONFIRM = 0,
    VIBE_BIND_BACK,
    VIBE_BIND_ADD_TO_QUEUE,
    VIBE_BIND_SEARCH_REMOVE,
    VIBE_BIND_PREV_TRACK,
    VIBE_BIND_NEXT_TRACK,
    VIBE_BIND_QUEUE,
    VIBE_BIND_LIBRARY,
    VIBE_BIND_R3,
    VIBE_BIND_COUNT
} VibeBind;

typedef struct {
    char music_dirs[VIBE_MAX_MUSIC_DIRS][VIBE_PATH_MAX];
    int music_dir_count;
    int music_user; /* 1 = folders set in-app; skip auto-discovered roots */
    float volume;
    int profile_mode;
    int start_library;
    int viz_shuffle;
    char audio_device[128];
    char config_dir[VIBE_PATH_MAX];
    char data_dir[VIBE_PATH_MAX];
    char cache_dir[VIBE_PATH_MAX];
    char db_path[VIBE_PATH_MAX];
    char preset_dir[VIBE_PATH_MAX];
    char texture_dir[VIBE_PATH_MAX];
    int bindings[VIBE_BIND_COUNT];
} VibeConfig;

int config_init(VibeConfig *cfg, int argc, char **argv);
void config_save(const VibeConfig *cfg);
int config_add_music_dir(VibeConfig *cfg, const char *path); /* 1 added, 0 duplicate, -1 full/invalid */
void config_remove_music_dir(VibeConfig *cfg, int index);
void config_profile_params(int effective_profile, VibeProfileParams *out);
int config_resolve_effective_profile(int mode, int on_battery, int display_h);
void config_strlcpy(char *dst, size_t dst_sz, const char *src);
int config_find_file(char *out, size_t out_sz, const char *rel);
/* Directory of the running binary (packed cache or install dir). No trailing slash. */
int config_resource_dir(char *out, size_t n);

#endif
