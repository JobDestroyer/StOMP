#ifndef VIBE_UI_H
#define VIBE_UI_H

#include "config.h"
#include "input.h"

struct App;

typedef enum {
    UI_VIEW_NOW_PLAYING = 0,
    UI_VIEW_LIBRARY,
    UI_VIEW_ALBUMS,
    UI_VIEW_ALBUM_TRACKS,
    UI_VIEW_ARTISTS,
    UI_VIEW_ARTIST_ALBUMS,
    UI_VIEW_FOLDERS,
    UI_VIEW_FOLDER_TRACKS,
    UI_VIEW_PLAYLISTS,
    UI_VIEW_PLAYLIST_TRACKS,
    UI_VIEW_QUEUE,
    UI_VIEW_SEARCH,
    UI_VIEW_SETTINGS,
    UI_VIEW_BINDING,
    UI_VIEW_PRESETS,
    UI_VIEW_MUSIC_DIRS,
    UI_VIEW_DIR_BROWSER,
    UI_VIEW_RADIO,
    UI_VIEW_RADIO_FAVORITES,
    UI_VIEW_RADIO_SOMA,
    UI_VIEW_RADIO_COUNTRIES,
    UI_VIEW_RADIO_STATIONS,
    UI_VIEW_RADIO_CUSTOM,
    UI_VIEW_RADIO_ADD
} UiView;

int ui_init(int win_w, int win_h, float veil);
void ui_shutdown(void);
void ui_resize(int win_w, int win_h);
void ui_set_veil(float veil);
void ui_set_view(UiView v);
UiView ui_view(void);
void ui_show_overlay(void);
void ui_hide_overlay(void);
int ui_overlay_visible(void);
void ui_idle_tick(uint32_t now_ms);
void ui_note_input(uint32_t now_ms);
int ui_handle(struct App *app, VibeCmd cmd, float seek_axis, int repeat);
void ui_draw(struct App *app);
void ui_refresh_lists(struct App *app);
void ui_text_input(const char *text);
int ui_focus_kind(void);
int64_t ui_focus_id(void);
const char *ui_search_query(void);
int ui_binding_mode(void);
void ui_binding_captured(int button);

enum {
    UI_KIND_NONE = 0,
    UI_KIND_ACTION,
    UI_KIND_TRACK,
    UI_KIND_ALBUM,
    UI_KIND_ARTIST,
    UI_KIND_FOLDER,
    UI_KIND_PLAYLIST,
    UI_KIND_SETTING,
    UI_KIND_PRESET,
    UI_KIND_RADIO
};

#endif
