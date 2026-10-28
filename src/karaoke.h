/* SPDX-License-Identifier: LGPL-2.1-only
 * Copyright (C) 2026 JobDestroyer
 */

#ifndef STOMP_KARAOKE_H
#define STOMP_KARAOKE_H

#include "config.h"
#include "library.h"

#include <stdint.h>

enum {
    KARA_SIDE_NONE = 0,
    KARA_SIDE_CDG,
    KARA_SIDE_LRC,
    KARA_SIDE_ASS,
    KARA_SIDE_KAR,
    KARA_SIDE_ULTRASTAR,
    KARA_SIDE_KFN,
    KARA_SIDE_KOK
};

int karaoke_open(const char *db_path);
void karaoke_close(void);
int karaoke_ok(void);

void karaoke_set_scan_roots(const char dirs[][VIBE_PATH_MAX], int count);
int karaoke_scan(void);

int karaoke_song_count(void);
int karaoke_artist_count(void);
int karaoke_genre_count(void);
int karaoke_folder_count(void);

int karaoke_list_artists(LibArtist *out, int cap);
int karaoke_list_genres(LibArtist *out, int cap);
int karaoke_list_folders(int64_t parent_id, LibFolder *out, int cap);
int karaoke_folder_direct_tracks(int64_t folder_id, LibTrack *out, int cap);
int karaoke_get_folder(int64_t id, LibFolder *out);
int karaoke_artist_songs(int64_t artist_id, LibTrack *out, int cap);
int karaoke_genre_songs(const char *genre, LibTrack *out, int cap);
int karaoke_list_songs_after(const char *title, int64_t id, LibTrack *out, int cap);
int karaoke_list_songs_before(const char *title, int64_t id, LibTrack *out, int cap);
int karaoke_list_songs_from_letter(uint32_t letter, LibTrack *out, int cap);
int karaoke_search(const char *query, LibTrack *out, int cap);
int karaoke_get_song(int64_t id, LibTrack *out);
int karaoke_song_sidecar(int64_t id, char *out, int cap, int *kind);
int karaoke_path_is_zip(const char *path);
int karaoke_zip_prepare(const char *zip_path);
const char *karaoke_zip_audio_path(void);
const char *karaoke_zip_cdg_path(void);
int karaoke_get_artist(int64_t id, LibArtist *out);
int karaoke_resume_song(LibTrack *out, int *position_ms);

int karaoke_queue_len(void);
int karaoke_queue_index(void);
void karaoke_queue_set_index(int i);
int karaoke_queue_get(int idx, LibTrack *out);
int karaoke_queue_add(int64_t song_id);
int karaoke_queue_insert_next(int64_t song_id);
int karaoke_queue_add_artist(int64_t artist_id);
int karaoke_queue_add_genre(const char *genre);
int karaoke_queue_add_folder(int64_t folder_id);
int karaoke_queue_add_all(void);
int karaoke_queue_has(int64_t song_id);
int karaoke_queue_remove_song(int64_t song_id);
int karaoke_queue_play_artist_from(int64_t artist_id, int64_t start_id);
int karaoke_queue_play_genre_from(const char *genre, int64_t start_id);
int karaoke_queue_play_folder_from(int64_t folder_id, int64_t start_id);
int karaoke_queue_play_song(int64_t song_id);
int karaoke_queue_remove(int idx);
void karaoke_queue_clear(void);
int karaoke_queue_save(void);
int karaoke_queue_load(void);

int karaoke_song_fav_has(int64_t song_id);
int karaoke_song_fav_toggle(int64_t song_id);
int karaoke_song_fav_count(void);
int karaoke_song_fav_list(LibTrack *out, int cap);

void karaoke_session_set(const char *key, const char *value);
int karaoke_session_get(const char *key, char *out, int cap);
void karaoke_session_set_int(const char *key, int v);
int karaoke_session_get_int(const char *key, int fallback);

const char *karaoke_side_label(int kind);

#endif
