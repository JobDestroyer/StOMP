#ifndef VIBE_LIBRARY_H
#define VIBE_LIBRARY_H

#include "config.h"

#include <stdint.h>

typedef struct {
    int64_t id;
    char path[VIBE_PATH_MAX];
    char title[VIBE_NAME_MAX];
    char artist[VIBE_NAME_MAX];
    char album[VIBE_NAME_MAX];
    int64_t album_id;
    int64_t artist_id;
    int64_t folder_id;
    int track_no;
    int duration_ms;
} LibTrack;

typedef struct {
    int64_t id;
    char name[VIBE_NAME_MAX];
    char artist[VIBE_NAME_MAX];
    int64_t artist_id;
    int track_count;
} LibAlbum;

typedef struct {
    int64_t id;
    char name[VIBE_NAME_MAX];
    int album_count;
} LibArtist;

typedef struct {
    int64_t id;
    char path[VIBE_PATH_MAX];
    char name[VIBE_NAME_MAX];
    int64_t parent_id;
    int child_count;
} LibFolder;

typedef struct {
    int64_t id;
    char name[VIBE_NAME_MAX];
    int item_count;
} LibPlaylist;

int library_open(const char *db_path);
void library_close(void);
int library_ok(void);

void library_set_scan_roots(const char dirs[][VIBE_PATH_MAX], int count);
int library_scan(void); /* call from decode thread only */
int library_track_count(void);
int library_album_count(void);
int library_artist_count(void);
int library_folder_count(void);

int library_list_albums(LibAlbum *out, int cap, int offset);
int library_list_artists(LibArtist *out, int cap, int offset);
int library_list_folders(int64_t parent_id, LibFolder *out, int cap);
int library_list_playlists(LibPlaylist *out, int cap);
int library_album_tracks(int64_t album_id, LibTrack *out, int cap);
int library_artist_albums(int64_t artist_id, LibAlbum *out, int cap);
int library_folder_tracks(int64_t folder_id, LibTrack *out, int cap);
int library_folder_direct_tracks(int64_t folder_id, LibTrack *out, int cap);
int library_get_folder(int64_t id, LibFolder *out);
int library_playlist_tracks(int64_t playlist_id, LibTrack *out, int cap);
int library_search(const char *query, LibTrack *out, int cap);
int library_get_track(int64_t id, LibTrack *out);
int library_get_album(int64_t id, LibAlbum *out);
int library_resume_track(LibTrack *out, int *position_ms);

int library_queue_len(void);
int library_queue_index(void);
void library_queue_set_index(int i);
int library_queue_get(int idx, LibTrack *out);
int library_queue_add(int64_t track_id);
int library_queue_add_album(int64_t album_id);
int library_queue_add_artist(int64_t artist_id);
int library_queue_add_folder(int64_t folder_id);
int library_queue_play_album_from(int64_t album_id, int64_t start_track_id);
int library_queue_play_folder_from(int64_t folder_id, int64_t start_track_id);
int library_queue_play_track(int64_t track_id);
int library_queue_remove(int idx);
int library_queue_move(int idx, int delta);
void library_queue_clear(void);
int library_queue_save(void);
int library_queue_load(void);

void library_session_set(const char *key, const char *value);
int library_session_get(const char *key, char *out, int cap);
void library_session_set_int(const char *key, int v);
int library_session_get_int(const char *key, int fallback);

void library_viz_set_rating(const char *key, int rating);
int library_viz_get_rating(const char *key);

int library_read_tags(const char *path, LibTrack *out);

typedef struct {
    int64_t id;
    char name[VIBE_NAME_MAX];
    char url[VIBE_PATH_MAX];
} LibRadioCustom;

int library_radio_custom_add(const char *name, const char *url);
int library_radio_custom_count(void);
int library_radio_custom_list(LibRadioCustom *out, int cap);
int library_radio_custom_remove(int64_t id);

int library_radio_fav_add(const char *name, const char *url, const char *sub);
int library_radio_fav_has(const char *url);
int library_radio_fav_toggle(const char *name, const char *url, const char *sub);
int library_radio_fav_count(void);
int library_radio_fav_list(LibRadioCustom *out, int cap);
int library_radio_fav_remove(int64_t id);

#endif
