/* SPDX-License-Identifier: LGPL-2.1-only
 * Copyright (C) 2026 JobDestroyer
 */

#include "library.h"
#include "playlist.h"

#include "sqlite3.h"

#include <libavformat/avformat.h>
#include <libavutil/dict.h>

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <strings.h>

static sqlite3 *s_db;
static sqlite3 *s_viz;
static char s_roots[VIBE_MAX_MUSIC_DIRS][VIBE_PATH_MAX];
static int s_nroots;
static int s_cnt_albums = -1;
static int s_cnt_artists = -1;
static int s_cnt_folders = -1;
static int s_scan_writes;
static int s_scanning;

static void counts_invalidate(void);
static void scan_checkpoint(void);
static int album_name_cmp(const void *a, const void *b);
static int album_year_cmp(const void *a, const void *b);
static int artist_name_cmp(const void *a, const void *b);
static int playlist_name_cmp(const void *a, const void *b);
static int64_t s_queue[VIBE_QUEUE_MAX];
static int s_qlen;
static int s_qidx;
static int64_t s_qhas[VIBE_QUEUE_MAX];
static int s_qhas_n;
static int s_qhas_ready;

typedef struct {
    int64_t id;
    int queued;
    int total;
} QueueCover;
static QueueCover s_cover_alb[VIBE_QUEUE_MAX];
static int s_cover_nalb;
static QueueCover s_cover_art[VIBE_QUEUE_MAX];
static int s_cover_nart;
static QueueCover s_cover_pl[VIBE_QUEUE_MAX];
static int s_cover_npl;
static char s_cover_path[VIBE_QUEUE_MAX][VIBE_PATH_MAX];
static int s_cover_npath;
static QueueCover s_cover_fold[VIBE_QUEUE_MAX];
static int s_cover_nfold;
static int s_cover_ready;

static void queue_mutated(void)
{
    s_cover_ready = 0;
    s_qhas_ready = 0;
}

static const char *k_schema =
    "PRAGMA journal_mode=WAL;"
    "CREATE TABLE IF NOT EXISTS folders ("
    "  id INTEGER PRIMARY KEY,"
    "  path TEXT UNIQUE NOT NULL,"
    "  parent_id INTEGER"
    ");"
    "CREATE TABLE IF NOT EXISTS artists ("
    "  id INTEGER PRIMARY KEY,"
    "  name TEXT UNIQUE NOT NULL COLLATE NOCASE"
    ");"
    "CREATE TABLE IF NOT EXISTS albums ("
    "  id INTEGER PRIMARY KEY,"
    "  artist_id INTEGER,"
    "  name TEXT NOT NULL COLLATE NOCASE,"
    "  year INTEGER,"
    "  UNIQUE(name)"
    ");"
    "CREATE TABLE IF NOT EXISTS tracks ("
    "  id INTEGER PRIMARY KEY,"
    "  path TEXT UNIQUE NOT NULL,"
    "  title TEXT,"
    "  artist_id INTEGER,"
    "  album_id INTEGER,"
    "  folder_id INTEGER,"
    "  track_no INTEGER,"
    "  duration_ms INTEGER,"
    "  mtime INTEGER"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_tracks_album ON tracks(album_id);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_artist ON tracks(artist_id);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_folder ON tracks(folder_id);"
    "CREATE INDEX IF NOT EXISTS idx_folders_parent ON folders(parent_id);"
    "CREATE TABLE IF NOT EXISTS playlists ("
    "  id INTEGER PRIMARY KEY,"
    "  name TEXT NOT NULL,"
    "  path TEXT UNIQUE"
    ");"
    "CREATE TABLE IF NOT EXISTS playlist_items ("
    "  id INTEGER PRIMARY KEY,"
    "  playlist_id INTEGER,"
    "  track_id INTEGER,"
    "  pos INTEGER"
    ");"
    "CREATE TABLE IF NOT EXISTS last_queue ("
    "  pos INTEGER PRIMARY KEY,"
    "  track_id INTEGER"
    ");"
    "CREATE TABLE IF NOT EXISTS session ("
    "  key TEXT PRIMARY KEY,"
    "  value TEXT"
    ");"
    "CREATE TABLE IF NOT EXISTS viz_ratings ("
    "  key TEXT PRIMARY KEY,"
    "  rating INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS radio_custom ("
    "  id INTEGER PRIMARY KEY,"
    "  name TEXT NOT NULL,"
    "  url TEXT NOT NULL UNIQUE"
    ");"
    "CREATE TABLE IF NOT EXISTS radio_fav ("
    "  id INTEGER PRIMARY KEY,"
    "  name TEXT NOT NULL,"
    "  url TEXT NOT NULL UNIQUE,"
    "  sub TEXT"
    ");"
    "CREATE TABLE IF NOT EXISTS artist_fav ("
    "  artist_id INTEGER PRIMARY KEY"
    ");"
    "CREATE TABLE IF NOT EXISTS album_fav ("
    "  album_id INTEGER PRIMARY KEY"
    ");"
    "CREATE TABLE IF NOT EXISTS playlist_fav ("
    "  path TEXT PRIMARY KEY"
    ");";

static int is_audio_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) {
        return 0;
    }
    return strcasecmp(dot, ".flac") == 0 || strcasecmp(dot, ".mp3") == 0 ||
           strcasecmp(dot, ".ogg") == 0 || strcasecmp(dot, ".opus") == 0 ||
           strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".aac") == 0 ||
           strcasecmp(dot, ".m4a") == 0 || strcasecmp(dot, ".mka") == 0;
}

static int artist_split_len(const char *s)
{
    static const struct {
        const char *w;
        int n;
    } marks[] = {
        {"featuring", 9},
        {"versus", 7},
        {"feat.", 5},
        {"feat", 4},
        {"vs.", 3},
        {"ft.", 3},
        {"vs", 2},
        {"ft", 2},
    };
    for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); i++) {
        unsigned char next;
        if (strncasecmp(s, marks[i].w, (size_t)marks[i].n) != 0) {
            continue;
        }
        next = (unsigned char)s[marks[i].n];
        if (next == '\0' || !isalnum(next)) {
            return marks[i].n;
        }
    }
    return 0;
}

static void artist_normalize(char *name)
{
    unsigned char prev = 0;
    char *s;
    size_t len, i;

    if (!name) {
        return;
    }
    for (s = name; *s; s++) {
        if (*s == ',') {
            *s = '\0';
            break;
        }
        if ((prev == 0 || !isalnum(prev)) && artist_split_len(s) > 0) {
            *s = '\0';
            break;
        }
        prev = (unsigned char)*s;
    }
    len = strlen(name);
    for (;;) {
        while (len > 0 && isspace((unsigned char)name[len - 1])) {
            name[--len] = '\0';
        }
        if (len > 0 && (name[len - 1] == '-' || name[len - 1] == '/' ||
                        name[len - 1] == ',' || name[len - 1] == '&' ||
                        name[len - 1] == '(' || name[len - 1] == '[' ||
                        name[len - 1] == '{')) {
            name[--len] = '\0';
            continue;
        }
        break;
    }
    i = 0;
    while (name[i] && isspace((unsigned char)name[i])) {
        i++;
    }
    if (i > 0) {
        memmove(name, name + i, strlen(name + i) + 1);
    }
}

static int64_t get_or_create_artist(const char *name)
{
    sqlite3_stmt *st = NULL;
    int64_t id = 0;
    const char *n = (name && name[0]) ? name : "Unknown Artist";

    if (sqlite3_prepare_v2(s_db, "SELECT id FROM artists WHERE name=?1 COLLATE NOCASE;",
                           -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, n, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        id = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        return id;
    }
    sqlite3_finalize(st);

    if (sqlite3_prepare_v2(s_db, "INSERT INTO artists(name) VALUES(?1);", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, n, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);

    if (sqlite3_prepare_v2(s_db, "SELECT id FROM artists WHERE name=?1 COLLATE NOCASE;",
                           -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, n, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        id = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return id;
}

static int64_t get_or_create_album(const char *name)
{
    sqlite3_stmt *st = NULL;
    int64_t id = 0;
    const char *n = (name && name[0]) ? name : "Unknown Album";
    if (sqlite3_prepare_v2(s_db, "INSERT OR IGNORE INTO albums(name) VALUES(?1);", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, n, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    if (sqlite3_prepare_v2(s_db, "SELECT id FROM albums WHERE name=?1;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, n, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        id = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return id;
}

static int64_t get_or_create_folder(const char *path, int64_t parent_id)
{
    sqlite3_stmt *st = NULL;
    int64_t id = 0;
    if (sqlite3_prepare_v2(s_db,
                           "INSERT INTO folders(path, parent_id) VALUES(?1,?2) "
                           "ON CONFLICT(path) DO UPDATE SET parent_id=excluded.parent_id;",
                           -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    if (parent_id > 0) {
        sqlite3_bind_int64(st, 2, parent_id);
    } else {
        sqlite3_bind_null(st, 2);
    }
    sqlite3_step(st);
    sqlite3_finalize(st);
    if (sqlite3_prepare_v2(s_db, "SELECT id FROM folders WHERE path=?1;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        id = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return id;
}

static void fill_track_from_row(sqlite3_stmt *st, LibTrack *t)
{
    const unsigned char *s;
    memset(t, 0, sizeof(*t));
    t->id = sqlite3_column_int64(st, 0);
    s = sqlite3_column_text(st, 1);
    if (s) {
        snprintf(t->path, sizeof(t->path), "%s", (const char *)s);
    }
    s = sqlite3_column_text(st, 2);
    if (s) {
        snprintf(t->title, sizeof(t->title), "%s", (const char *)s);
    }
    s = sqlite3_column_text(st, 3);
    if (s) {
        snprintf(t->artist, sizeof(t->artist), "%s", (const char *)s);
    }
    s = sqlite3_column_text(st, 4);
    if (s) {
        snprintf(t->album, sizeof(t->album), "%s", (const char *)s);
    }
    t->album_id = sqlite3_column_int64(st, 5);
    t->artist_id = sqlite3_column_int64(st, 6);
    t->folder_id = sqlite3_column_int64(st, 7);
    t->track_no = sqlite3_column_int(st, 8);
    t->duration_ms = sqlite3_column_int(st, 9);
}

static void fill_album_from_row(sqlite3_stmt *st, LibAlbum *a)
{
    const unsigned char *s;
    memset(a, 0, sizeof(*a));
    a->id = sqlite3_column_int64(st, 0);
    s = sqlite3_column_text(st, 1);
    if (s) {
        snprintf(a->name, sizeof(a->name), "%s", (const char *)s);
    }
    s = sqlite3_column_text(st, 2);
    if (s) {
        snprintf(a->artist, sizeof(a->artist), "%s", (const char *)s);
    }
    a->artist_id = sqlite3_column_int64(st, 3);
    a->track_count = sqlite3_column_int(st, 4);
    a->year = sqlite3_column_int(st, 5);
}

static const char *k_track_select =
    "SELECT t.id, t.path, t.title, a.name, b.name, t.album_id, t.artist_id, t.folder_id, t.track_no, t.duration_ms "
    "FROM tracks t "
    "LEFT JOIN artists a ON a.id=t.artist_id "
    "LEFT JOIN albums b ON b.id=t.album_id ";

/* Album rows: id, name, display artist, representative artist_id, track count. */
static const char *k_album_from =
    "SELECT b.id, b.name, "
    "CASE WHEN COUNT(DISTINCT t.artist_id) > 1 THEN 'Various Artists' "
    "ELSE IFNULL(MAX(a.name), '') END, "
    "IFNULL(MIN(t.artist_id), 0), "
    "COUNT(t.id), "
    "IFNULL(b.year, 0) "
    "FROM albums b "
    "LEFT JOIN tracks t ON t.album_id=b.id "
    "LEFT JOIN artists a ON a.id=t.artist_id ";

int library_read_tags(const char *path, LibTrack *out)
{
    AVFormatContext *fmt = NULL;
    const AVDictionaryEntry *e;
    int si;
    memset(out, 0, sizeof(*out));
    snprintf(out->path, sizeof(out->path), "%s", path);
    {
        AVDictionary *opts = NULL;
        av_dict_set(&opts, "protocol_whitelist", "file", 0);
        if (avformat_open_input(&fmt, path, NULL, &opts) < 0) {
            av_dict_free(&opts);
            return -1;
        }
        av_dict_free(&opts);
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        return -1;
    }
    e = av_dict_get(fmt->metadata, "title", NULL, 0);
    if (e && e->value) {
        snprintf(out->title, sizeof(out->title), "%s", e->value);
    }
    e = av_dict_get(fmt->metadata, "artist", NULL, 0);
    if (e && e->value) {
        snprintf(out->artist, sizeof(out->artist), "%s", e->value);
        artist_normalize(out->artist);
    }
    e = av_dict_get(fmt->metadata, "album", NULL, 0);
    if (e && e->value) {
        snprintf(out->album, sizeof(out->album), "%s", e->value);
    }
    e = av_dict_get(fmt->metadata, "track", NULL, 0);
    if (e && e->value) {
        out->track_no = atoi(e->value);
    }
    {
        static const char *keys[] = {"date", "year", "DATE", "YEAR", "TDRC", "TDRL", "TYER", NULL};
        int k;
        for (k = 0; keys[k] && out->year <= 0; k++) {
            int y = 0;
            const char *s;
            e = av_dict_get(fmt->metadata, keys[k], NULL, 0);
            if (!e || !e->value) {
                continue;
            }
            s = e->value;
            while (*s && (*s < '0' || *s > '9')) {
                s++;
            }
            if (sscanf(s, "%d", &y) == 1 && y >= 1000 && y <= 2100) {
                out->year = y;
            }
        }
    }
    if (fmt->duration > 0) {
        out->duration_ms = (int)(fmt->duration / 1000);
    }
    si = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (si >= 0 && fmt->streams[si]->duration > 0 && out->duration_ms <= 0) {
        double sec = fmt->streams[si]->duration * av_q2d(fmt->streams[si]->time_base);
        out->duration_ms = (int)(sec * 1000.0);
    }
    if (!out->title[0]) {
        const char *slash = strrchr(path, '/');
        snprintf(out->title, sizeof(out->title), "%s", slash ? slash + 1 : path);
    }
    if (!out->artist[0]) {
        snprintf(out->artist, sizeof(out->artist), "Unknown Artist");
    }
    if (!out->album[0]) {
        snprintf(out->album, sizeof(out->album), "Unknown Album");
    }
    avformat_close_input(&fmt);
    return 0;
}

static void upsert_track(const char *path, time_t mtime, int64_t folder_id)
{
    sqlite3_stmt *st = NULL;
    LibTrack t;
    int64_t artist_id, album_id;
    char dir[VIBE_PATH_MAX];
    const char *slash;
    time_t old_mtime = 0;
    int exists = 0;

    if (sqlite3_prepare_v2(s_db, "SELECT mtime FROM tracks WHERE path=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            exists = 1;
            old_mtime = (time_t)sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
    }
    if (exists && old_mtime == mtime) {
        return;
    }
    if (library_read_tags(path, &t) != 0) {
        return;
    }
    slash = strrchr(path, '/');
    if (slash) {
        size_t n = (size_t)(slash - path);
        if (n >= sizeof(dir)) {
            n = sizeof(dir) - 1;
        }
        memcpy(dir, path, n);
        dir[n] = '\0';
    } else {
        snprintf(dir, sizeof(dir), ".");
    }
    artist_id = get_or_create_artist(t.artist);
    album_id = get_or_create_album(t.album);
    if (t.year > 0 && album_id > 0) {
        sqlite3_stmt *yst = NULL;
        if (sqlite3_prepare_v2(s_db,
                               "UPDATE albums SET year=?1 WHERE id=?2 AND IFNULL(year,0)=0;",
                               -1, &yst, NULL) == SQLITE_OK) {
            sqlite3_bind_int(yst, 1, t.year);
            sqlite3_bind_int64(yst, 2, album_id);
            sqlite3_step(yst);
            sqlite3_finalize(yst);
        }
    }
    if (folder_id <= 0) {
        folder_id = get_or_create_folder(dir, 0);
    }

    if (sqlite3_prepare_v2(s_db,
                           "INSERT INTO tracks(path,title,artist_id,album_id,folder_id,track_no,duration_ms,mtime) "
                           "VALUES(?1,?2,?3,?4,?5,?6,?7,?8) "
                           "ON CONFLICT(path) DO UPDATE SET title=excluded.title, artist_id=excluded.artist_id, "
                           "album_id=excluded.album_id, folder_id=excluded.folder_id, track_no=excluded.track_no, "
                           "duration_ms=excluded.duration_ms, mtime=excluded.mtime;",
                           -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "StOMP: upsert track: %s\n", sqlite3_errmsg(s_db));
        return;
    }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, t.title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, artist_id);
    sqlite3_bind_int64(st, 4, album_id);
    sqlite3_bind_int64(st, 5, folder_id);
    sqlite3_bind_int(st, 6, t.track_no);
    sqlite3_bind_int(st, 7, t.duration_ms);
    sqlite3_bind_int64(st, 8, (sqlite3_int64)mtime);
    if (sqlite3_step(st) != SQLITE_DONE) {
        fprintf(stderr, "StOMP: upsert step: %s\n", sqlite3_errmsg(s_db));
    } else {
        scan_checkpoint();
    }
    sqlite3_finalize(st);
}

static void walk_dir(const char *path, int64_t parent_id, int depth)
{
    DIR *d;
    struct dirent *de;
    char child[VIBE_PATH_MAX];
    struct stat st;
    int64_t folder_id;

    if (depth > 16) {
        return;
    }
    folder_id = get_or_create_folder(path, parent_id);
    d = opendir(path);
    if (!d) {
        return;
    }
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        if (lstat(child, &st) != 0) {
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            walk_dir(child, folder_id, depth + 1);
        } else if (S_ISREG(st.st_mode) && is_audio_ext(de->d_name)) {
            upsert_track(child, st.st_mtime, folder_id);
        }
    }
    closedir(d);
}

static void path_dirname_copy(const char *path, char *out, int n)
{
    const char *slash;
    if (!out || n <= 0) {
        return;
    }
    out[0] = '\0';
    if (!path) {
        return;
    }
    slash = strrchr(path, '/');
    if (!slash) {
        snprintf(out, (size_t)n, ".");
        return;
    }
    if (slash == path) {
        snprintf(out, (size_t)n, "/");
        return;
    }
    {
        size_t L = (size_t)(slash - path);
        if (L >= (size_t)n) {
            L = (size_t)n - 1;
        }
        memcpy(out, path, L);
        out[L] = '\0';
    }
}

static void path_basename_copy(const char *path, char *out, int n)
{
    const char *slash;
    if (!out || n <= 0) {
        return;
    }
    out[0] = '\0';
    if (!path || !path[0]) {
        return;
    }
    slash = strrchr(path, '/');
    snprintf(out, (size_t)n, "%s", slash ? slash + 1 : path);
}

static void path_join_norm(const char *dir, const char *rel, char *out, int n)
{
    char tmp[VIBE_PATH_MAX];
    char *parts[128];
    char store[VIBE_PATH_MAX];
    int np = 0, abs = 0, i;
    size_t used = 0;
    if (!out || n <= 0) {
        return;
    }
    out[0] = '\0';
    if (!rel || !rel[0]) {
        return;
    }
    if (rel[0] == '/' || (isalpha((unsigned char)rel[0]) && rel[1] == ':')) {
        snprintf(tmp, sizeof(tmp), "%s", rel);
    } else if (dir && dir[0]) {
        snprintf(tmp, sizeof(tmp), "%s/%s", dir, rel);
    } else {
        snprintf(tmp, sizeof(tmp), "%s", rel);
    }
    abs = tmp[0] == '/';
    store[0] = '\0';
    {
        char *p = tmp;
        while (*p) {
            char *start;
            size_t L;
            while (*p == '/') {
                p++;
            }
            if (!*p) {
                break;
            }
            start = p;
            while (*p && *p != '/') {
                p++;
            }
            L = (size_t)(p - start);
            if (L == 1 && start[0] == '.') {
                continue;
            }
            if (L == 2 && start[0] == '.' && start[1] == '.') {
                if (np > 0) {
                    np--;
                }
                continue;
            }
            if (np < 128 && used + L + 1 < sizeof(store)) {
                parts[np] = store + used;
                memcpy(store + used, start, L);
                store[used + L] = '\0';
                used += L + 1;
                np++;
            }
        }
    }
    if (abs) {
        snprintf(out, (size_t)n, "/");
    } else {
        out[0] = '\0';
    }
    for (i = 0; i < np; i++) {
        size_t have = strlen(out);
        if (have && out[have - 1] != '/') {
            snprintf(out + have, (size_t)n - have, "/%s", parts[i]);
        } else {
            snprintf(out + have, (size_t)n - have, "%s", parts[i]);
        }
    }
}

static int64_t track_id_by_path(const char *path)
{
    sqlite3_stmt *st = NULL;
    int64_t id = 0;
    if (!s_db || !path || !path[0]) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_db, "SELECT id FROM tracks WHERE path=?1;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        id = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return id;
}

static void like_escape(const char *in, char *out, size_t cap)
{
    size_t j = 0;
    const char *p;
    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (!in) {
        return;
    }
    for (p = in; *p && j + 2 < cap; p++) {
        if (*p == '%' || *p == '_' || *p == '\\') {
            out[j++] = '\\';
        }
        out[j++] = *p;
    }
    out[j] = '\0';
}

static int64_t track_id_by_basename(const char *base, const char *pl_dir)
{
    sqlite3_stmt *st = NULL;
    int64_t best = 0, fallback = 0;
    size_t pld = pl_dir ? strlen(pl_dir) : 0;
    char pat[VIBE_PATH_MAX];
    if (!s_db || !base || !base[0]) {
        return 0;
    }
    like_escape(base, pat, sizeof(pat));
    if (sqlite3_prepare_v2(s_db,
                           "SELECT id, path FROM tracks WHERE lower(path) LIKE '%/' || lower(?1) ESCAPE '\\' "
                           "OR lower(path) = lower(?2);",
                           -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, pat, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, base, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(st, 0);
        const unsigned char *p = sqlite3_column_text(st, 1);
        const char *path = p ? (const char *)p : "";
        if (!fallback) {
            fallback = id;
        }
        if (pld > 0 && strncmp(path, pl_dir, pld) == 0 &&
            (path[pld] == '\0' || path[pld] == '/')) {
            best = id;
            break;
        }
    }
    sqlite3_finalize(st);
    return best ? best : fallback;
}

static int64_t track_id_by_title(const char *title, const char *artist)
{
    sqlite3_stmt *st = NULL;
    int64_t id = 0;
    if (!s_db || !title || !title[0]) {
        return 0;
    }
    if (artist && artist[0]) {
        if (sqlite3_prepare_v2(s_db,
                               "SELECT t.id FROM tracks t LEFT JOIN artists a ON a.id=t.artist_id "
                               "WHERE t.title=?1 COLLATE NOCASE AND a.name=?2 COLLATE NOCASE LIMIT 1;",
                               -1, &st, NULL) != SQLITE_OK) {
            return 0;
        }
        sqlite3_bind_text(st, 1, title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, artist, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            id = sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
        if (id) {
            return id;
        }
    }
    if (sqlite3_prepare_v2(s_db,
                           "SELECT id FROM tracks WHERE title=?1 COLLATE NOCASE LIMIT 1;",
                           -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, title, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        id = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return id;
}

static int64_t resolve_pl_entry(const char *pl_dir, const PlEntry *e)
{
    char full[VIBE_PATH_MAX];
    char base[VIBE_PATH_MAX];
    int64_t id;
    struct stat st;
    if (!e) {
        return 0;
    }
    full[0] = '\0';
    if (e->path[0] && !(isalpha((unsigned char)e->path[0]) && e->path[1] == ':')) {
        path_join_norm(pl_dir, e->path, full, (int)sizeof(full));
        id = track_id_by_path(full);
        if (id) {
            return id;
        }
        if (full[0] && stat(full, &st) == 0 && S_ISREG(st.st_mode) && is_audio_ext(full)) {
            upsert_track(full, st.st_mtime, 0);
            id = track_id_by_path(full);
            if (id) {
                return id;
            }
        }
        id = track_id_by_path(e->path);
        if (id) {
            return id;
        }
    }
    path_basename_copy(e->path[0] ? e->path : "", base, (int)sizeof(base));
    if (base[0]) {
        id = track_id_by_basename(base, pl_dir);
        if (id) {
            return id;
        }
    }
    return track_id_by_title(e->title, e->artist);
}

static void playlist_display_name(const char *path, char *out, int n)
{
    char base[VIBE_NAME_MAX];
    char *dot;
    path_basename_copy(path, base, (int)sizeof(base));
    dot = strrchr(base, '.');
    if (dot && dot != base) {
        *dot = '\0';
    }
    snprintf(out, (size_t)n, "%s", base[0] ? base : "Playlist");
}

static void import_playlist_file(const char *path)
{
    PlEntry *ents;
    int n, i, pos;
    int64_t pl_id = 0;
    char dir[VIBE_PATH_MAX];
    char name[VIBE_NAME_MAX];
    sqlite3_stmt *st = NULL;
    if (!path || !s_db) {
        return;
    }
    ents = calloc((size_t)VIBE_LIST_MAX, sizeof(*ents));
    if (!ents) {
        return;
    }
    n = playlist_parse(path, ents, VIBE_LIST_MAX);
    path_dirname_copy(path, dir, (int)sizeof(dir));
    playlist_display_name(path, name, (int)sizeof(name));
    if (sqlite3_prepare_v2(s_db,
                           "INSERT INTO playlists(name, path) VALUES(?1,?2) "
                           "ON CONFLICT(path) DO UPDATE SET name=excluded.name;",
                           -1, &st, NULL) != SQLITE_OK) {
        free(ents);
        return;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, path, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        free(ents);
        return;
    }
    sqlite3_finalize(st);
    st = NULL;
    if (sqlite3_prepare_v2(s_db, "SELECT id FROM playlists WHERE path=?1;", -1, &st, NULL) != SQLITE_OK) {
        free(ents);
        return;
    }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        pl_id = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    if (pl_id <= 0) {
        free(ents);
        return;
    }
    if (sqlite3_prepare_v2(s_db, "DELETE FROM playlist_items WHERE playlist_id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, pl_id);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    pos = 0;
    if (sqlite3_prepare_v2(s_db,
                           "INSERT INTO playlist_items(playlist_id, track_id, pos) VALUES(?1,?2,?3);",
                           -1, &st, NULL) != SQLITE_OK) {
        free(ents);
        return;
    }
    for (i = 0; i < n; i++) {
        int64_t tid = resolve_pl_entry(dir, &ents[i]);
        if (tid <= 0) {
            continue;
        }
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_int64(st, 1, pl_id);
        sqlite3_bind_int64(st, 2, tid);
        sqlite3_bind_int(st, 3, pos++);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    free(ents);
    scan_checkpoint();
}

static void walk_playlists(const char *path, int depth)
{
    DIR *d;
    struct dirent *de;
    char child[VIBE_PATH_MAX];
    struct stat st;
    if (depth > 16) {
        return;
    }
    d = opendir(path);
    if (!d) {
        return;
    }
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        if (lstat(child, &st) != 0) {
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            walk_playlists(child, depth + 1);
        } else if (S_ISREG(st.st_mode) && playlist_is_ext(de->d_name)) {
            import_playlist_file(child);
        }
    }
    closedir(d);
}

static void viz_path_from_library(const char *db_path, char *out, size_t n)
{
    const char *slash;
    if (!out || n == 0) {
        return;
    }
    out[0] = '\0';
    if (!db_path || !db_path[0]) {
        snprintf(out, n, "viz.db");
        return;
    }
    slash = strrchr(db_path, '/');
    if (slash) {
        snprintf(out, n, "%.*s/viz.db", (int)(slash - db_path), db_path);
    } else {
        snprintf(out, n, "viz.db");
    }
}

static int viz_row_count(sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!db) {
        return 0;
    }
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM viz_ratings;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

static void viz_migrate_from_library(void)
{
    sqlite3_stmt *sel = NULL;
    sqlite3_stmt *ins = NULL;
    int n = 0;
    if (!s_db || !s_viz) {
        return;
    }
    if (viz_row_count(s_viz) > 0 || viz_row_count(s_db) <= 0) {
        return;
    }
    if (sqlite3_prepare_v2(s_db, "SELECT key, rating FROM viz_ratings;", -1, &sel, NULL) != SQLITE_OK) {
        return;
    }
    if (sqlite3_prepare_v2(s_viz,
                           "INSERT OR REPLACE INTO viz_ratings(key,rating) VALUES(?1,?2);",
                           -1, &ins, NULL) != SQLITE_OK) {
        sqlite3_finalize(sel);
        return;
    }
    sqlite3_exec(s_viz, "BEGIN;", NULL, NULL, NULL);
    while (sqlite3_step(sel) == SQLITE_ROW) {
        const unsigned char *k = sqlite3_column_text(sel, 0);
        if (!k || !k[0]) {
            continue;
        }
        sqlite3_reset(ins);
        sqlite3_bind_text(ins, 1, (const char *)k, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(ins, 2, sqlite3_column_int(sel, 1));
        if (sqlite3_step(ins) == SQLITE_DONE) {
            n++;
        }
    }
    sqlite3_exec(s_viz, "COMMIT;", NULL, NULL, NULL);
    sqlite3_finalize(ins);
    sqlite3_finalize(sel);
    if (n > 0) {
        fprintf(stderr, "StOMP: copied %d visualization ratings into viz.db\n", n);
    }
}

static int viz_open(const char *db_path)
{
    char path[VIBE_PATH_MAX];
    char *err = NULL;
    static const char *schema =
        "PRAGMA journal_mode=WAL;"
        "CREATE TABLE IF NOT EXISTS viz_ratings ("
        "  key TEXT PRIMARY KEY,"
        "  rating INTEGER NOT NULL"
        ");";
    viz_path_from_library(db_path, path, sizeof(path));
    if (sqlite3_open(path, &s_viz) != SQLITE_OK) {
        fprintf(stderr, "StOMP: sqlite open %s: %s\n", path, sqlite3_errmsg(s_viz));
        sqlite3_close(s_viz);
        s_viz = NULL;
        return -1;
    }
    if (sqlite3_exec(s_viz, schema, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "StOMP: viz schema: %s\n", err ? err : "");
        sqlite3_free(err);
        sqlite3_close(s_viz);
        s_viz = NULL;
        return -1;
    }
    sqlite3_busy_timeout(s_viz, 5000);
    sqlite3_exec(s_viz, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_exec(s_viz, "PRAGMA temp_store=MEMORY;", NULL, NULL, NULL);
    viz_migrate_from_library();
    fprintf(stderr, "StOMP: visualization ratings %s (%d)\n", path, viz_row_count(s_viz));
    return 0;
}

int library_open(const char *db_path)
{
    char *err = NULL;
    if (sqlite3_open(db_path, &s_db) != SQLITE_OK) {
        fprintf(stderr, "StOMP: sqlite open %s: %s\n", db_path, sqlite3_errmsg(s_db));
        sqlite3_close(s_db);
        s_db = NULL;
        return -1;
    }
    if (sqlite3_exec(s_db, k_schema, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "StOMP: schema: %s\n", err ? err : "");
        sqlite3_free(err);
        sqlite3_close(s_db);
        s_db = NULL;
        return -1;
    }
    sqlite3_exec(s_db, "ALTER TABLE playlists ADD COLUMN path TEXT;", NULL, NULL, NULL);
    sqlite3_exec(s_db, "ALTER TABLE albums ADD COLUMN year INTEGER;", NULL, NULL, NULL);
    sqlite3_exec(s_db, "CREATE UNIQUE INDEX IF NOT EXISTS idx_playlists_path ON playlists(path);",
                 NULL, NULL, NULL);
    sqlite3_busy_timeout(s_db, 5000);
    sqlite3_exec(s_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_exec(s_db, "PRAGMA temp_store=MEMORY;", NULL, NULL, NULL);
    if (viz_open(db_path) != 0) {
        fprintf(stderr, "StOMP: visualization ratings will not persist this run\n");
    }
    s_qlen = 0;
    s_qidx = 0;
    s_cnt_albums = s_cnt_artists = s_cnt_folders = -1;
    return 0;
}

static void counts_invalidate(void)
{
    s_cnt_albums = s_cnt_artists = s_cnt_folders = -1;
}

static void scan_checkpoint(void)
{
    if (!s_scanning) {
        return;
    }
    if (++s_scan_writes < 256) {
        return;
    }
    s_scan_writes = 0;
    sqlite3_exec(s_db, "COMMIT;", NULL, NULL, NULL);
    sqlite3_exec(s_db, "BEGIN;", NULL, NULL, NULL);
}

void library_close(void)
{
    if (s_viz) {
        sqlite3_close(s_viz);
        s_viz = NULL;
    }
    if (s_db) {
        sqlite3_close(s_db);
        s_db = NULL;
    }
}

int library_ok(void)
{
    return s_db != NULL;
}

void library_set_scan_roots(const char dirs[][VIBE_PATH_MAX], int count)
{
    s_nroots = 0;
    if (!dirs) {
        return;
    }
    if (count > VIBE_MAX_MUSIC_DIRS) {
        count = VIBE_MAX_MUSIC_DIRS;
    }
    for (int i = 0; i < count; i++) {
        snprintf(s_roots[s_nroots], VIBE_PATH_MAX, "%s", dirs[i]);
        s_nroots++;
    }
}

static int path_in_roots(const char *path)
{
    int i;
    if (!path) {
        return 0;
    }
    for (i = 0; i < s_nroots; i++) {
        size_t n = strlen(s_roots[i]);
        if (n == 0) {
            continue;
        }
        if (strncmp(path, s_roots[i], n) == 0 && (path[n] == '\0' || path[n] == '/')) {
            return 1;
        }
    }
    return 0;
}

int library_scan(void)
{
    sqlite3_stmt *st = NULL;
    sqlite3_stmt *del = NULL;

    if (!s_db) {
        return -1;
    }
    counts_invalidate();
    s_scan_writes = 0;
    s_scanning = 1;
    sqlite3_exec(s_db, "BEGIN;", NULL, NULL, NULL);
    for (int i = 0; i < s_nroots; i++) {
        walk_dir(s_roots[i], 0, 0);
    }
    if (sqlite3_prepare_v2(s_db, "SELECT id, path FROM tracks;", -1, &st, NULL) == SQLITE_OK) {
        int64_t *ids = NULL;
        int nids = 0, cap = 0, k;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *path = (const char *)sqlite3_column_text(st, 1);
            struct stat pst;
            int gone = !path || !path_in_roots(path);
            if (!gone && (stat(path, &pst) != 0 || !S_ISREG(pst.st_mode))) {
                gone = 1;
            }
            if (gone) {
                if (nids >= cap) {
                    int ncap = cap ? cap * 2 : 64;
                    int64_t *grow = realloc(ids, (size_t)ncap * sizeof(*ids));
                    if (!grow) {
                        break;
                    }
                    ids = grow;
                    cap = ncap;
                }
                ids[nids++] = sqlite3_column_int64(st, 0);
            }
        }
        sqlite3_finalize(st);
        st = NULL;
        if (nids > 0 &&
            sqlite3_prepare_v2(s_db, "DELETE FROM tracks WHERE id=?1;", -1, &del, NULL) == SQLITE_OK) {
            for (k = 0; k < nids; k++) {
                sqlite3_reset(del);
                sqlite3_clear_bindings(del);
                sqlite3_bind_int64(del, 1, ids[k]);
                sqlite3_step(del);
            }
            sqlite3_finalize(del);
            del = NULL;
        }
        free(ids);
    }
    if (st) {
        sqlite3_finalize(st);
    }
    if (del) {
        sqlite3_finalize(del);
    }
    sqlite3_exec(s_db, "DELETE FROM playlist_items WHERE playlist_id IN "
                       "(SELECT id FROM playlists WHERE path IS NOT NULL AND path != '');",
                 NULL, NULL, NULL);
    sqlite3_exec(s_db, "DELETE FROM playlists WHERE path IS NOT NULL AND path != '';",
                 NULL, NULL, NULL);
    for (int i = 0; i < s_nroots; i++) {
        walk_playlists(s_roots[i], 0);
    }
    sqlite3_exec(s_db, "DELETE FROM playlist_fav WHERE path NOT IN "
                       "(SELECT path FROM playlists WHERE path IS NOT NULL AND path != '');",
                 NULL, NULL, NULL);
    sqlite3_exec(s_db, "DELETE FROM playlist_items WHERE track_id NOT IN (SELECT id FROM tracks);",
                 NULL, NULL, NULL);
    sqlite3_exec(s_db, "DELETE FROM last_queue WHERE track_id NOT IN (SELECT id FROM tracks);",
                 NULL, NULL, NULL);
    sqlite3_exec(s_db, "DELETE FROM albums WHERE NOT EXISTS (SELECT 1 FROM tracks WHERE tracks.album_id = albums.id);",
                 NULL, NULL, NULL);
    sqlite3_exec(s_db, "DELETE FROM artists WHERE NOT EXISTS (SELECT 1 FROM tracks WHERE tracks.artist_id = artists.id);",
                 NULL, NULL, NULL);
    if (sqlite3_prepare_v2(s_db, "SELECT id, path FROM folders;", -1, &st, NULL) == SQLITE_OK) {
        int64_t *ids = NULL;
        int nids = 0, cap = 0, k;
        sqlite3_stmt *fdel = NULL;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *path = (const char *)sqlite3_column_text(st, 1);
            struct stat pst;
            int gone = !path || !path_in_roots(path);
            if (!gone && (stat(path, &pst) != 0 || !S_ISDIR(pst.st_mode))) {
                gone = 1;
            }
            if (gone) {
                if (nids >= cap) {
                    int ncap = cap ? cap * 2 : 64;
                    int64_t *grow = realloc(ids, (size_t)ncap * sizeof(*ids));
                    if (!grow) {
                        break;
                    }
                    ids = grow;
                    cap = ncap;
                }
                ids[nids++] = sqlite3_column_int64(st, 0);
            }
        }
        sqlite3_finalize(st);
        st = NULL;
        if (nids > 0 &&
            sqlite3_prepare_v2(s_db, "DELETE FROM folders WHERE id=?1;", -1, &fdel, NULL) == SQLITE_OK) {
            for (k = 0; k < nids; k++) {
                sqlite3_reset(fdel);
                sqlite3_clear_bindings(fdel);
                sqlite3_bind_int64(fdel, 1, ids[k]);
                sqlite3_step(fdel);
            }
            sqlite3_finalize(fdel);
        }
        free(ids);
    }
    sqlite3_exec(s_db, "COMMIT;", NULL, NULL, NULL);
    s_scanning = 0;
    s_scan_writes = 0;
    counts_invalidate();
    fprintf(stderr, "StOMP: library has %d tracks in %d albums, %d playlists\n",
            library_track_count(), library_album_count(), library_playlist_count());
    return 0;
}

int library_track_count(void)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_db, "SELECT COUNT(*) FROM tracks;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

int library_album_count(void)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db) {
        return 0;
    }
    if (s_cnt_albums >= 0) {
        return s_cnt_albums;
    }
    if (sqlite3_prepare_v2(s_db, "SELECT COUNT(*) FROM albums;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    s_cnt_albums = n;
    return n;
}

int library_artist_count(void)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db) {
        return 0;
    }
    if (s_cnt_artists >= 0) {
        return s_cnt_artists;
    }
    if (sqlite3_prepare_v2(s_db, "SELECT COUNT(*) FROM artists;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    s_cnt_artists = n;
    return n;
}

int library_list_albums(LibAlbum *out, int cap, int offset)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    char sql[1024];
    if (!s_db || !out || cap <= 0) {
        return 0;
    }
    snprintf(sql, sizeof(sql),
             "%s GROUP BY b.id ORDER BY b.name COLLATE NOCASE LIMIT ?1 OFFSET ?2;",
             k_album_from);
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int(st, 1, cap);
    sqlite3_bind_int(st, 2, offset);
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        fill_album_from_row(st, &out[n]);
        n++;
    }
    sqlite3_finalize(st);
    if (n > 1) {
        qsort(out, (size_t)n, sizeof(out[0]), album_name_cmp);
    }
    return n;
}

int library_list_artists(LibArtist *out, int cap, int offset)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db || !out || cap <= 0) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_db,
                           "SELECT a.id, a.name, "
                           "(SELECT COUNT(DISTINCT t.album_id) FROM tracks t WHERE t.artist_id=a.id) "
                           "FROM artists a ORDER BY a.name COLLATE NOCASE LIMIT ?1 OFFSET ?2;",
                           -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int(st, 1, cap);
    sqlite3_bind_int(st, 2, offset);
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *s;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].id = sqlite3_column_int64(st, 0);
        s = sqlite3_column_text(st, 1);
        if (s) {
            snprintf(out[n].name, sizeof(out[n].name), "%s", (const char *)s);
        }
        out[n].album_count = sqlite3_column_int(st, 2);
        n++;
    }
    sqlite3_finalize(st);
    if (n > 1) {
        qsort(out, (size_t)n, sizeof(out[0]), artist_name_cmp);
    }
    return n;
}

static const char *skip_article(const char *s)
{
    if (!s) {
        return "";
    }
    while (*s == ' ') {
        s++;
    }
    if (strncasecmp(s, "the ", 4) == 0) {
        return s + 4;
    }
    if (strncasecmp(s, "a ", 2) == 0) {
        return s + 2;
    }
    return s;
}

static int name_sort_cmp(const char *a, const char *b)
{
    unsigned char ca, cb;
    int ba, bb;

    if (!a) {
        a = "";
    }
    if (!b) {
        b = "";
    }
    while (*a == ' ') {
        a++;
    }
    while (*b == ' ') {
        b++;
    }
    ca = (unsigned char)*a;
    cb = (unsigned char)*b;
    ba = ((ca >= 'A' && ca <= 'Z') || (ca >= 'a' && ca <= 'z') || ca >= 0x80) ? 0 : 1;
    bb = ((cb >= 'A' && cb <= 'Z') || (cb >= 'a' && cb <= 'z') || cb >= 0x80) ? 0 : 1;
    if (ba != bb) {
        return ba - bb;
    }
    return strcasecmp(a, b);
}

static int folder_name_cmp(const void *a, const void *b)
{
    const LibFolder *fa = a;
    const LibFolder *fb = b;
    return name_sort_cmp(fa->name, fb->name);
}

static int album_name_cmp(const void *a, const void *b)
{
    const LibAlbum *aa = a;
    const LibAlbum *ab = b;
    return name_sort_cmp(skip_article(aa->name), skip_article(ab->name));
}

static int album_year_cmp(const void *a, const void *b)
{
    const LibAlbum *aa = a;
    const LibAlbum *ab = b;
    int ya = aa->year;
    int yb = ab->year;
    if (ya > 0 && yb > 0 && ya != yb) {
        return ya - yb;
    }
    if (ya > 0 && yb <= 0) {
        return -1;
    }
    if (ya <= 0 && yb > 0) {
        return 1;
    }
    return name_sort_cmp(skip_article(aa->name), skip_article(ab->name));
}

static int artist_name_cmp(const void *a, const void *b)
{
    const LibArtist *aa = a;
    const LibArtist *ab = b;
    return name_sort_cmp(aa->name, ab->name);
}

static int playlist_name_cmp(const void *a, const void *b)
{
    const LibPlaylist *pa = a;
    const LibPlaylist *pb = b;
    return name_sort_cmp(pa->name, pb->name);
}

static int fill_folder_row(LibFolder *dst, sqlite3_stmt *st, int64_t parent_id)
{
    const unsigned char *s;
    const char *slash;
    memset(dst, 0, sizeof(*dst));
    dst->id = sqlite3_column_int64(st, 0);
    s = sqlite3_column_text(st, 1);
    if (s) {
        snprintf(dst->path, sizeof(dst->path), "%s", (const char *)s);
        slash = strrchr(dst->path, '/');
        snprintf(dst->name, sizeof(dst->name), "%s", slash && slash[1] ? slash + 1 : dst->path);
    }
    dst->parent_id = parent_id;
    dst->child_count = sqlite3_column_int(st, 2);
    return dst->path[0] ? 1 : 0;
}

static int list_folders_where(const char *where, int64_t bind, int has_bind,
                              LibFolder *out, int cap, int64_t parent_id)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT f.id, f.path, COUNT(t.id) "
             "FROM folders f LEFT JOIN tracks t ON t.folder_id=f.id "
             "%s GROUP BY f.id;",
             where ? where : "");
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    if (has_bind) {
        sqlite3_bind_int64(st, 1, bind);
    }
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        fill_folder_row(&out[n], st, parent_id);
        n++;
    }
    sqlite3_finalize(st);
    if (n > 1) {
        qsort(out, (size_t)n, sizeof(out[0]), folder_name_cmp);
    }
    return n;
}

int library_folder_count(void)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db) {
        return 0;
    }
    if (s_cnt_folders >= 0) {
        return s_cnt_folders;
    }
    if (sqlite3_prepare_v2(s_db,
                           "SELECT COUNT(*) FROM folders WHERE parent_id IN "
                           "(SELECT id FROM folders WHERE parent_id IS NULL);",
                           -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    if (n <= 0) {
        if (sqlite3_prepare_v2(s_db,
                               "SELECT COUNT(*) FROM folders WHERE parent_id IS NULL;",
                               -1, &st, NULL) != SQLITE_OK) {
            return 0;
        }
        if (sqlite3_step(st) == SQLITE_ROW) {
            n = sqlite3_column_int(st, 0);
        }
        sqlite3_finalize(st);
    }
    s_cnt_folders = n;
    return s_cnt_folders;
}

int library_list_folders(int64_t parent_id, LibFolder *out, int cap)
{
    int n;
    if (!s_db || !out || cap <= 0) {
        return 0;
    }
    if (parent_id > 0) {
        return list_folders_where("WHERE f.parent_id=?1", parent_id, 1, out, cap, parent_id);
    }
    n = list_folders_where(
        "WHERE f.parent_id IN (SELECT id FROM folders WHERE parent_id IS NULL)",
        0, 0, out, cap, 0);
    if (n == 0) {
        n = list_folders_where("WHERE f.parent_id IS NULL", 0, 0, out, cap, 0);
    }
    return n;
}

int library_playlist_count(void)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_db, "SELECT COUNT(*) FROM playlists;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

int library_list_playlists(LibPlaylist *out, int cap)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db || !out || cap <= 0) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_db,
                           "SELECT id, name, (SELECT COUNT(*) FROM playlist_items i WHERE i.playlist_id=playlists.id) "
                           "FROM playlists ORDER BY name LIMIT ?1;",
                           -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int(st, 1, cap);
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *s;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].id = sqlite3_column_int64(st, 0);
        s = sqlite3_column_text(st, 1);
        if (s) {
            snprintf(out[n].name, sizeof(out[n].name), "%s", (const char *)s);
        }
        out[n].item_count = sqlite3_column_int(st, 2);
        n++;
    }
    sqlite3_finalize(st);
    if (n > 1) {
        qsort(out, (size_t)n, sizeof(out[0]), playlist_name_cmp);
    }
    return n;
}

static int list_tracks_sql(const char *sql, int64_t id, LibTrack *out, int cap)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db || !out || cap <= 0) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int64(st, 1, id);
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        fill_track_from_row(st, &out[n]);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

int library_album_tracks(int64_t album_id, LibTrack *out, int cap)
{
    char sql[768];
    snprintf(sql, sizeof(sql),
             "%s WHERE t.album_id=?1 ORDER BY CASE WHEN IFNULL(t.track_no,0)=0 THEN 1 ELSE 0 END, "
             "IFNULL(t.track_no,0), t.title COLLATE NOCASE LIMIT %d;",
             k_track_select, cap);
    return list_tracks_sql(sql, album_id, out, cap);
}

int library_artist_albums(int64_t artist_id, LibAlbum *out, int cap)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    char sql[1024];
    if (!s_db || !out || cap <= 0) {
        return 0;
    }
    snprintf(sql, sizeof(sql),
             "%s WHERE b.id IN (SELECT album_id FROM tracks WHERE artist_id=?1) "
             "GROUP BY b.id ORDER BY CASE WHEN IFNULL(b.year,0)=0 THEN 1 ELSE 0 END, "
             "IFNULL(b.year,0), b.name COLLATE NOCASE LIMIT ?2;",
             k_album_from);
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int64(st, 1, artist_id);
    sqlite3_bind_int(st, 2, cap);
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        fill_album_from_row(st, &out[n]);
        n++;
    }
    sqlite3_finalize(st);
    if (n > 1) {
        qsort(out, (size_t)n, sizeof(out[0]), album_year_cmp);
    }
    return n;
}

int library_folder_tracks(int64_t folder_id, LibTrack *out, int cap)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    char sql[900];
    if (!s_db || !out || cap <= 0 || folder_id <= 0) {
        return 0;
    }
    snprintf(sql, sizeof(sql),
             "%s JOIN folders f ON f.id=?1 "
             "WHERE t.path = f.path OR substr(t.path, 1, length(f.path)+1) = f.path || '/' "
             "ORDER BY t.path COLLATE NOCASE LIMIT %d;",
             k_track_select, cap);
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int64(st, 1, folder_id);
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        fill_track_from_row(st, &out[n]);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

int library_folder_direct_tracks(int64_t folder_id, LibTrack *out, int cap)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    char sql[900];
    if (!s_db || !out || cap <= 0) {
        return 0;
    }
    if (folder_id > 0) {
        snprintf(sql, sizeof(sql),
                 "%s WHERE t.folder_id=?1 ORDER BY t.path COLLATE NOCASE LIMIT %d;",
                 k_track_select, cap);
        return list_tracks_sql(sql, folder_id, out, cap);
    }
    snprintf(sql, sizeof(sql),
             "%s JOIN folders f ON f.id=t.folder_id WHERE f.parent_id IS NULL "
             "ORDER BY t.path COLLATE NOCASE LIMIT %d;",
             k_track_select, cap);
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        fill_track_from_row(st, &out[n]);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

int library_get_folder(int64_t id, LibFolder *out)
{
    sqlite3_stmt *st = NULL;
    if (!s_db || !out || id <= 0) {
        return -1;
    }
    if (sqlite3_prepare_v2(s_db,
                           "SELECT f.id, f.path, COUNT(t.id) "
                           "FROM folders f LEFT JOIN tracks t ON t.folder_id=f.id "
                           "WHERE f.id=?1 GROUP BY f.id;",
                           -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return -1;
    }
    fill_folder_row(out, st, 0);
    sqlite3_finalize(st);
    return 0;
}

int library_playlist_tracks(int64_t playlist_id, LibTrack *out, int cap)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    char sql[768];
    snprintf(sql, sizeof(sql),
             "%s JOIN playlist_items i ON i.track_id=t.id WHERE i.playlist_id=?1 ORDER BY i.pos LIMIT %d;",
             k_track_select, cap);
    if (!s_db || !out || cap <= 0) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int64(st, 1, playlist_id);
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        fill_track_from_row(st, &out[n]);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

int library_search(const char *query, LibTrack *out, int cap)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    char like[VIBE_NAME_MAX * 2 + 8];
    char sql[768];
    size_t j;
    const char *q;
    if (!s_db || !out || cap <= 0 || !query) {
        return 0;
    }
    like[0] = '%';
    j = 1;
    for (q = query; *q && j + 3 < sizeof(like); q++) {
        if (*q == '%' || *q == '_' || *q == '\\') {
            like[j++] = '\\';
        }
        like[j++] = *q;
    }
    like[j++] = '%';
    like[j] = '\0';
    snprintf(sql, sizeof(sql),
             "%s WHERE t.title LIKE ?1 ESCAPE '\\' OR a.name LIKE ?1 ESCAPE '\\' "
             "OR b.name LIKE ?1 ESCAPE '\\' "
             "ORDER BY t.title LIMIT %d;",
             k_track_select, cap);
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, like, -1, SQLITE_TRANSIENT);
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        fill_track_from_row(st, &out[n]);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

int library_get_track(int64_t id, LibTrack *out)
{
    char sql[768];
    snprintf(sql, sizeof(sql), "%s WHERE t.id=?1;", k_track_select);
    return list_tracks_sql(sql, id, out, 1) == 1 ? 0 : -1;
}

int library_get_album(int64_t id, LibAlbum *out)
{
    sqlite3_stmt *st = NULL;
    char sql[1024];
    if (!s_db || !out) {
        return -1;
    }
    snprintf(sql, sizeof(sql), "%s WHERE b.id=?1 GROUP BY b.id;", k_album_from);
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return -1;
    }
    fill_album_from_row(st, out);
    sqlite3_finalize(st);
    return 0;
}

int library_resume_track(LibTrack *out, int *position_ms)
{
    char buf[32];
    int64_t id;
    if (library_session_get_int("playing_radio", 0)) {
        return -1;
    }
    if (position_ms) {
        *position_ms = library_session_get_int("position_ms", 0);
    }
    if (library_session_get("track_id", buf, (int)sizeof(buf)) != 0) {
        return -1;
    }
    id = (int64_t)atoll(buf);
    if (id <= 0) {
        return -1;
    }
    return library_get_track(id, out);
}

int library_resume_radio(char *title, int title_n, char *url, int url_n, char *sub, int sub_n)
{
    if (!title || title_n <= 0 || !url || url_n <= 0) {
        return -1;
    }
    title[0] = '\0';
    url[0] = '\0';
    if (sub && sub_n > 0) {
        sub[0] = '\0';
    }
    if (library_session_get_int("playing_radio", 0) == 0) {
        return -1;
    }
    if (library_session_get("radio_url", url, url_n) != 0 || !url[0]) {
        return -1;
    }
    library_session_get("radio_title", title, title_n);
    if (sub && sub_n > 0) {
        library_session_get("radio_sub", sub, sub_n);
    }
    return 0;
}

int library_queue_len(void)
{
    return s_qlen;
}

int library_queue_index(void)
{
    return s_qidx;
}

void library_queue_set_index(int i)
{
    if (s_qlen <= 0) {
        s_qidx = 0;
        return;
    }
    if (i < 0) {
        i = 0;
    }
    if (i >= s_qlen) {
        i = s_qlen - 1;
    }
    s_qidx = i;
}

int library_queue_get(int idx, LibTrack *out)
{
    if (idx < 0 || idx >= s_qlen || !out) {
        return -1;
    }
    return library_get_track(s_queue[idx], out);
}

int library_queue_add(int64_t track_id)
{
    if (s_qlen >= VIBE_QUEUE_MAX || track_id <= 0) {
        return -1;
    }
    s_queue[s_qlen++] = track_id;
    queue_mutated();
    return 0;
}

static int add_tracks_sql(const char *sql, int64_t id)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int64(st, 1, id);
    while (sqlite3_step(st) == SQLITE_ROW && s_qlen < VIBE_QUEUE_MAX) {
        s_queue[s_qlen++] = sqlite3_column_int64(st, 0);
        n++;
    }
    sqlite3_finalize(st);
    if (n > 0) {
        queue_mutated();
    }
    return n;
}

int library_queue_add_album(int64_t album_id)
{
    return add_tracks_sql(
        "SELECT id FROM tracks WHERE album_id=?1 "
        "ORDER BY CASE WHEN IFNULL(track_no,0)=0 THEN 1 ELSE 0 END, IFNULL(track_no,0), title COLLATE NOCASE;",
        album_id);
}

int library_queue_add_artist(int64_t artist_id)
{
    return add_tracks_sql(
        "SELECT t.id FROM tracks t JOIN albums b ON b.id=t.album_id "
        "WHERE t.artist_id=?1 "
        "ORDER BY CASE WHEN IFNULL(b.year,0)=0 THEN 1 ELSE 0 END, IFNULL(b.year,0), "
        "b.name COLLATE NOCASE, "
        "CASE WHEN IFNULL(t.track_no,0)=0 THEN 1 ELSE 0 END, IFNULL(t.track_no,0), "
        "t.title COLLATE NOCASE;",
        artist_id);
}

int library_queue_add_folder(int64_t folder_id)
{
    return add_tracks_sql(
        "SELECT t.id FROM tracks t JOIN folders f ON f.id=?1 "
        "WHERE t.path = f.path OR substr(t.path, 1, length(f.path)+1) = f.path || '/' "
        "ORDER BY t.path COLLATE NOCASE;",
        folder_id);
}

int library_queue_add_playlist(int64_t playlist_id)
{
    return add_tracks_sql(
        "SELECT track_id FROM playlist_items WHERE playlist_id=?1 ORDER BY pos;",
        playlist_id);
}

static int play_from_sql(const char *sql, int64_t id, int64_t start_track_id)
{
    sqlite3_stmt *st = NULL;
    int64_t tmp[VIBE_QUEUE_MAX];
    int n = 0, started = 0;
    if (!s_db || !sql) {
        return -1;
    }
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, id);
    while (sqlite3_step(st) == SQLITE_ROW && n < VIBE_QUEUE_MAX) {
        int64_t tid = sqlite3_column_int64(st, 0);
        if (!started && start_track_id != 0 && tid != start_track_id) {
            continue;
        }
        started = 1;
        tmp[n++] = tid;
    }
    sqlite3_finalize(st);
    if (n <= 0) {
        return -1;
    }
    memcpy(s_queue, tmp, (size_t)n * sizeof(tmp[0]));
    s_qlen = n;
    s_qidx = 0;
    queue_mutated();
    return 0;
}

int library_queue_play_album_from(int64_t album_id, int64_t start_track_id)
{
    return play_from_sql(
        "SELECT id FROM tracks WHERE album_id=?1 "
        "ORDER BY CASE WHEN IFNULL(track_no,0)=0 THEN 1 ELSE 0 END, "
        "IFNULL(track_no,0), title COLLATE NOCASE;",
        album_id, start_track_id);
}

int library_queue_play_playlist_from(int64_t playlist_id, int64_t start_track_id)
{
    return play_from_sql(
        "SELECT track_id FROM playlist_items WHERE playlist_id=?1 ORDER BY pos;",
        playlist_id, start_track_id);
}

int library_queue_play_folder_from(int64_t folder_id, int64_t start_track_id)
{
    return play_from_sql(
        "SELECT id FROM tracks WHERE folder_id=?1 ORDER BY path COLLATE NOCASE;",
        folder_id, start_track_id);
}

int library_queue_play_track(int64_t track_id)
{
    if (track_id <= 0) {
        return -1;
    }
    s_queue[0] = track_id;
    s_qlen = 1;
    s_qidx = 0;
    queue_mutated();
    return 0;
}

static int i64_cmp(const void *a, const void *b)
{
    int64_t da = *(const int64_t *)a;
    int64_t db = *(const int64_t *)b;
    if (da < db) {
        return -1;
    }
    if (da > db) {
        return 1;
    }
    return 0;
}

static void qhas_rebuild(void)
{
    int i, w = 0;
    memcpy(s_qhas, s_queue, (size_t)s_qlen * sizeof(s_qhas[0]));
    if (s_qlen > 1) {
        qsort(s_qhas, (size_t)s_qlen, sizeof(s_qhas[0]), i64_cmp);
    }
    for (i = 0; i < s_qlen; i++) {
        if (s_qhas[i] <= 0) {
            continue;
        }
        if (w == 0 || s_qhas[w - 1] != s_qhas[i]) {
            s_qhas[w++] = s_qhas[i];
        }
    }
    s_qhas_n = w;
    s_qhas_ready = 1;
}

int library_queue_has(int64_t track_id)
{
    if (track_id <= 0 || s_qlen <= 0) {
        return 0;
    }
    if (!s_qhas_ready) {
        qhas_rebuild();
    }
    return bsearch(&track_id, s_qhas, (size_t)s_qhas_n, sizeof(s_qhas[0]), i64_cmp) != NULL;
}

static int cover_id_cmp(const void *a, const void *b)
{
    int64_t da = ((const QueueCover *)a)->id;
    int64_t db = ((const QueueCover *)b)->id;
    if (da < db) {
        return -1;
    }
    if (da > db) {
        return 1;
    }
    return 0;
}

static int cover_lookup(const QueueCover *arr, int n, int64_t id)
{
    QueueCover key;
    const QueueCover *hit;
    if (id <= 0 || n <= 0) {
        return 0;
    }
    memset(&key, 0, sizeof(key));
    key.id = id;
    hit = bsearch(&key, arr, (size_t)n, sizeof(*arr), cover_id_cmp);
    if (!hit || hit->total <= 0 || hit->queued <= 0) {
        return 0;
    }
    return hit->queued >= hit->total ? 2 : 1;
}

static int qset_fill(void)
{
    sqlite3_stmt *st = NULL;
    int i;
    sqlite3_exec(s_db, "CREATE TEMP TABLE IF NOT EXISTS qset (id INTEGER PRIMARY KEY);",
                 NULL, NULL, NULL);
    sqlite3_exec(s_db, "DELETE FROM qset;", NULL, NULL, NULL);
    if (sqlite3_prepare_v2(s_db, "INSERT OR IGNORE INTO qset(id) VALUES(?1);", -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    for (i = 0; i < s_qlen; i++) {
        if (s_queue[i] <= 0) {
            continue;
        }
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, s_queue[i]);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    return 0;
}

static int cover_load_group(const char *sql, QueueCover *out, int cap)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        out[n].id = sqlite3_column_int64(st, 0);
        out[n].queued = sqlite3_column_int(st, 1);
        out[n].total = sqlite3_column_int(st, 2);
        n++;
    }
    sqlite3_finalize(st);
    if (n > 1) {
        qsort(out, (size_t)n, sizeof(out[0]), cover_id_cmp);
    }
    return n;
}

void library_queue_cover_refresh(void)
{
    sqlite3_stmt *st = NULL;
    if (s_cover_ready) {
        return;
    }
    s_cover_nalb = 0;
    s_cover_nart = 0;
    s_cover_npl = 0;
    s_cover_npath = 0;
    s_cover_nfold = 0;
    if (!s_db || s_qlen <= 0) {
        s_cover_ready = 1;
        return;
    }
    if (qset_fill() != 0) {
        return;
    }
    s_cover_nalb = cover_load_group(
        "SELECT t.album_id, "
        "SUM(CASE WHEN q.id IS NOT NULL THEN 1 ELSE 0 END), "
        "COUNT(*) "
        "FROM tracks t LEFT JOIN qset q ON q.id=t.id "
        "WHERE t.album_id IN (SELECT DISTINCT album_id FROM tracks WHERE id IN (SELECT id FROM qset)) "
        "GROUP BY t.album_id;",
        s_cover_alb, VIBE_QUEUE_MAX);
    s_cover_nart = cover_load_group(
        "SELECT t.artist_id, "
        "SUM(CASE WHEN q.id IS NOT NULL THEN 1 ELSE 0 END), "
        "COUNT(*) "
        "FROM tracks t LEFT JOIN qset q ON q.id=t.id "
        "WHERE t.artist_id IN (SELECT DISTINCT artist_id FROM tracks WHERE id IN (SELECT id FROM qset)) "
        "GROUP BY t.artist_id;",
        s_cover_art, VIBE_QUEUE_MAX);
    s_cover_npl = cover_load_group(
        "SELECT i.playlist_id, "
        "SUM(CASE WHEN q.id IS NOT NULL THEN 1 ELSE 0 END), "
        "COUNT(*) "
        "FROM playlist_items i LEFT JOIN qset q ON q.id=i.track_id "
        "WHERE i.playlist_id IN (SELECT DISTINCT playlist_id FROM playlist_items WHERE track_id IN (SELECT id FROM qset)) "
        "GROUP BY i.playlist_id;",
        s_cover_pl, VIBE_QUEUE_MAX);
    if (sqlite3_prepare_v2(s_db,
                           "SELECT t.path FROM tracks t JOIN qset q ON q.id=t.id;",
                           -1, &st, NULL) == SQLITE_OK) {
        while (s_cover_npath < VIBE_QUEUE_MAX && sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *s = sqlite3_column_text(st, 0);
            if (s && s[0]) {
                snprintf(s_cover_path[s_cover_npath], sizeof(s_cover_path[0]), "%s", (const char *)s);
                s_cover_npath++;
            }
        }
        sqlite3_finalize(st);
    }
    s_cover_ready = 1;
}

int library_queue_cover_album(int64_t album_id)
{
    if (!s_cover_ready) {
        library_queue_cover_refresh();
    }
    return cover_lookup(s_cover_alb, s_cover_nalb, album_id);
}

int library_queue_cover_artist(int64_t artist_id)
{
    if (!s_cover_ready) {
        library_queue_cover_refresh();
    }
    return cover_lookup(s_cover_art, s_cover_nart, artist_id);
}

int library_queue_cover_playlist(int64_t playlist_id)
{
    if (!s_cover_ready) {
        library_queue_cover_refresh();
    }
    return cover_lookup(s_cover_pl, s_cover_npl, playlist_id);
}

static int path_under(const char *root, const char *path)
{
    size_t n;
    if (!root || !root[0] || !path || !path[0]) {
        return 0;
    }
    n = strlen(root);
    if (strncmp(path, root, n) != 0) {
        return 0;
    }
    return path[n] == '\0' || path[n] == '/';
}

int library_queue_cover_folder(int64_t folder_id)
{
    sqlite3_stmt *st = NULL;
    char root[VIBE_PATH_MAX];
    int total = 0, queued = 0, i;
    if (!s_cover_ready) {
        library_queue_cover_refresh();
    }
    if (!s_db || folder_id <= 0) {
        return 0;
    }
    for (i = 0; i < s_cover_nfold; i++) {
        if (s_cover_fold[i].id == folder_id) {
            queued = s_cover_fold[i].queued;
            total = s_cover_fold[i].total;
            if (total <= 0 || queued <= 0) {
                return 0;
            }
            return queued >= total ? 2 : 1;
        }
    }
    root[0] = '\0';
    if (sqlite3_prepare_v2(s_db, "SELECT path FROM folders WHERE id=?1;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int64(st, 1, folder_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *s = sqlite3_column_text(st, 0);
        if (s) {
            snprintf(root, sizeof(root), "%s", (const char *)s);
        }
    }
    sqlite3_finalize(st);
    if (!root[0]) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_db,
                           "SELECT COUNT(*) FROM tracks t JOIN folders f ON f.id=?1 "
                           "WHERE t.path = f.path OR substr(t.path, 1, length(f.path)+1) = f.path || '/';",
                           -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int64(st, 1, folder_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        total = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    if (total > 0) {
        for (i = 0; i < s_cover_npath; i++) {
            if (path_under(root, s_cover_path[i])) {
                queued++;
            }
        }
    }
    if (s_cover_nfold < VIBE_QUEUE_MAX) {
        s_cover_fold[s_cover_nfold].id = folder_id;
        s_cover_fold[s_cover_nfold].queued = queued;
        s_cover_fold[s_cover_nfold].total = total > 0 ? total : 1;
        s_cover_nfold++;
    }
    if (total <= 0 || queued <= 0) {
        return 0;
    }
    return queued >= total ? 2 : 1;
}

int library_queue_remove(int idx)
{
    if (idx < 0 || idx >= s_qlen) {
        return -1;
    }
    memmove(&s_queue[idx], &s_queue[idx + 1], (size_t)(s_qlen - idx - 1) * sizeof(s_queue[0]));
    s_qlen--;
    if (s_qidx >= s_qlen) {
        s_qidx = s_qlen > 0 ? s_qlen - 1 : 0;
    } else if (idx < s_qidx) {
        s_qidx--;
    }
    queue_mutated();
    return 0;
}

int library_queue_remove_track(int64_t track_id)
{
    int n = 0;
    int i;
    if (track_id <= 0) {
        return 0;
    }
    for (i = s_qlen - 1; i >= 0; i--) {
        if (s_queue[i] == track_id) {
            library_queue_remove(i);
            n++;
        }
    }
    return n;
}

int library_queue_export_m3u(const char *path, const char *title)
{
    FILE *f;
    int i;
    LibTrack t;
    if (!path || !path[0] || s_qlen <= 0) {
        return -1;
    }
    f = fopen(path, "w");
    if (!f) {
        return -1;
    }
    fputs("#EXTM3U\n", f);
    if (title && title[0]) {
        fprintf(f, "#PLAYLIST:%s\n", title);
    }
    for (i = 0; i < s_qlen; i++) {
        int sec;
        if (library_queue_get(i, &t) != 0 || !t.path[0]) {
            continue;
        }
        sec = t.duration_ms > 0 ? t.duration_ms / 1000 : -1;
        fprintf(f, "#EXTINF:%d,%s - %s\n", sec, t.artist[0] ? t.artist : "Unknown",
                t.title[0] ? t.title : t.path);
        fprintf(f, "%s\n", t.path);
    }
    if (fclose(f) != 0) {
        return -1;
    }
    import_playlist_file(path);
    return 0;
}

int library_queue_move(int idx, int delta)
{
    int dst = idx + delta;
    int64_t tmp;
    if (idx < 0 || idx >= s_qlen || dst < 0 || dst >= s_qlen) {
        return -1;
    }
    tmp = s_queue[idx];
    s_queue[idx] = s_queue[dst];
    s_queue[dst] = tmp;
    if (s_qidx == idx) {
        s_qidx = dst;
    } else if (s_qidx == dst) {
        s_qidx = idx;
    }
    return dst;
}

void library_queue_clear(void)
{
    s_qlen = 0;
    s_qidx = 0;
    queue_mutated();
}

int library_queue_save(void)
{
    sqlite3_stmt *st = NULL;
    int i;
    if (!s_db) {
        return -1;
    }
    sqlite3_exec(s_db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_exec(s_db, "DELETE FROM last_queue;", NULL, NULL, NULL);
    if (sqlite3_prepare_v2(s_db, "INSERT INTO last_queue(pos, track_id) VALUES(?1,?2);", -1, &st, NULL) != SQLITE_OK) {
        sqlite3_exec(s_db, "ROLLBACK;", NULL, NULL, NULL);
        return -1;
    }
    for (i = 0; i < s_qlen; i++) {
        sqlite3_reset(st);
        sqlite3_bind_int(st, 1, i);
        sqlite3_bind_int64(st, 2, s_queue[i]);
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            sqlite3_exec(s_db, "ROLLBACK;", NULL, NULL, NULL);
            return -1;
        }
    }
    sqlite3_finalize(st);
    sqlite3_exec(s_db, "COMMIT;", NULL, NULL, NULL);
    return 0;
}

int library_queue_load(void)
{
    sqlite3_stmt *st = NULL;
    char buf[32];
    int64_t id = 0;
    int i, qi;
    s_qlen = 0;
    s_qidx = 0;
    if (!s_db) {
        return -1;
    }
    if (sqlite3_prepare_v2(s_db, "SELECT track_id FROM last_queue ORDER BY pos;", -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    while (sqlite3_step(st) == SQLITE_ROW && s_qlen < VIBE_QUEUE_MAX) {
        s_queue[s_qlen++] = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    queue_mutated();
    if (library_session_get("track_id", buf, (int)sizeof(buf)) == 0) {
        id = (int64_t)atoll(buf);
    }
    if (id > 0) {
        for (i = 0; i < s_qlen; i++) {
            if (s_queue[i] == id) {
                s_qidx = i;
                return 0;
            }
        }
    }
    qi = library_session_get_int("queue_index", 0);
    library_queue_set_index(qi);
    return 0;
}

void library_session_set(const char *key, const char *value)
{
    sqlite3_stmt *st = NULL;
    if (!s_db || !key) {
        return;
    }
    if (sqlite3_prepare_v2(s_db,
                           "INSERT INTO session(key,value) VALUES(?1,?2) "
                           "ON CONFLICT(key) DO UPDATE SET value=excluded.value;",
                           -1, &st, NULL) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, value ? value : "", -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

int library_session_get(const char *key, char *out, int cap)
{
    sqlite3_stmt *st = NULL;
    int rc = -1;
    if (!s_db || !key || !out || cap <= 0) {
        return -1;
    }
    out[0] = '\0';
    if (sqlite3_prepare_v2(s_db, "SELECT value FROM session WHERE key=?1;", -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *s = sqlite3_column_text(st, 0);
        if (s) {
            snprintf(out, (size_t)cap, "%s", (const char *)s);
            rc = 0;
        }
    }
    sqlite3_finalize(st);
    return rc;
}

void library_session_set_int(const char *key, int v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", v);
    library_session_set(key, buf);
}

int library_session_get_int(const char *key, int fallback)
{
    char buf[32];
    if (library_session_get(key, buf, (int)sizeof(buf)) != 0) {
        return fallback;
    }
    return atoi(buf);
}

void library_viz_set_rating(const char *key, int rating)
{
    sqlite3_stmt *st = NULL;
    if (!s_viz || !key || !key[0]) {
        return;
    }
    if (sqlite3_prepare_v2(s_viz,
                           "INSERT INTO viz_ratings(key,rating) VALUES(?1,?2) "
                           "ON CONFLICT(key) DO UPDATE SET rating=excluded.rating;",
                           -1, &st, NULL) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, rating);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

int library_viz_get_rating(const char *key)
{
    sqlite3_stmt *st = NULL;
    int r = 0;
    if (!s_viz || !key || !key[0]) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_viz, "SELECT rating FROM viz_ratings WHERE key=?1;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        r = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return r;
}

int library_viz_each_rating(int (*fn)(const char *key, int rating, void *ud), void *ud)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_viz || !fn) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_viz, "SELECT key, rating FROM viz_ratings;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *k = sqlite3_column_text(st, 0);
        int r = sqlite3_column_int(st, 1);
        if (k && k[0]) {
            if (fn((const char *)k, r, ud) != 0) {
                break;
            }
            n++;
        }
    }
    sqlite3_finalize(st);
    return n;
}

int library_radio_custom_add(const char *name, const char *url)
{
    sqlite3_stmt *st = NULL;
    if (!s_db || !url || !url[0]) {
        return -1;
    }
    if (!name || !name[0]) {
        name = "Station";
    }
    if (sqlite3_prepare_v2(s_db,
                           "INSERT INTO radio_custom(name, url) VALUES(?1,?2) "
                           "ON CONFLICT(url) DO UPDATE SET name=excluded.name;",
                           -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, url, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    return 0;
}

int library_radio_custom_count(void)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_db, "SELECT COUNT(*) FROM radio_custom;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

int library_radio_custom_list(LibRadioCustom *out, int cap)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db || !out || cap <= 0) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_db,
                           "SELECT id, name, url FROM radio_custom ORDER BY name COLLATE NOCASE;",
                           -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *s;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].id = sqlite3_column_int64(st, 0);
        s = sqlite3_column_text(st, 1);
        if (s) {
            snprintf(out[n].name, sizeof(out[n].name), "%s", (const char *)s);
        }
        s = sqlite3_column_text(st, 2);
        if (s) {
            snprintf(out[n].url, sizeof(out[n].url), "%s", (const char *)s);
        }
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

int library_radio_custom_remove(int64_t id)
{
    sqlite3_stmt *st = NULL;
    if (!s_db || id <= 0) {
        return -1;
    }
    if (sqlite3_prepare_v2(s_db, "DELETE FROM radio_custom WHERE id=?1;", -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return 0;
}

int library_radio_fav_add(const char *name, const char *url, const char *sub)
{
    sqlite3_stmt *st = NULL;
    if (!s_db || !url || !url[0]) {
        return -1;
    }
    if (!name || !name[0]) {
        name = "Station";
    }
    if (sqlite3_prepare_v2(s_db,
                           "INSERT INTO radio_fav(name, url, sub) VALUES(?1,?2,?3) "
                           "ON CONFLICT(url) DO UPDATE SET name=excluded.name, sub=excluded.sub;",
                           -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, sub ? sub : "", -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    return 0;
}

int library_radio_fav_has(const char *url)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db || !url || !url[0]) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_db, "SELECT 1 FROM radio_fav WHERE url=?1;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, url, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = 1;
    }
    sqlite3_finalize(st);
    return n;
}

int library_radio_fav_toggle(const char *name, const char *url, const char *sub)
{
    if (library_radio_fav_has(url)) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(s_db, "DELETE FROM radio_fav WHERE url=?1;", -1, &st, NULL) != SQLITE_OK) {
            return -1;
        }
        sqlite3_bind_text(st, 1, url, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
        return 0;
    }
    if (library_radio_fav_add(name, url, sub) != 0) {
        return -1;
    }
    return 1;
}

int library_radio_fav_count(void)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_db, "SELECT COUNT(*) FROM radio_fav;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

int library_radio_fav_list(LibRadioCustom *out, int cap)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db || !out || cap <= 0) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_db,
                           "SELECT id, name, url FROM radio_fav ORDER BY name COLLATE NOCASE;",
                           -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *s;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].id = sqlite3_column_int64(st, 0);
        s = sqlite3_column_text(st, 1);
        if (s) {
            snprintf(out[n].name, sizeof(out[n].name), "%s", (const char *)s);
        }
        s = sqlite3_column_text(st, 2);
        if (s) {
            snprintf(out[n].url, sizeof(out[n].url), "%s", (const char *)s);
        }
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

int library_radio_fav_remove(int64_t id)
{
    sqlite3_stmt *st = NULL;
    if (!s_db || id <= 0) {
        return -1;
    }
    if (sqlite3_prepare_v2(s_db, "DELETE FROM radio_fav WHERE id=?1;", -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return 0;
}

int library_artist_fav_has(int64_t artist_id)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db || artist_id <= 0) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_db, "SELECT 1 FROM artist_fav WHERE artist_id=?1;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int64(st, 1, artist_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = 1;
    }
    sqlite3_finalize(st);
    return n;
}

int library_artist_fav_toggle(int64_t artist_id)
{
    sqlite3_stmt *st = NULL;
    if (!s_db || artist_id <= 0) {
        return -1;
    }
    if (library_artist_fav_has(artist_id)) {
        if (sqlite3_prepare_v2(s_db, "DELETE FROM artist_fav WHERE artist_id=?1;", -1, &st, NULL) != SQLITE_OK) {
            return -1;
        }
        sqlite3_bind_int64(st, 1, artist_id);
        sqlite3_step(st);
        sqlite3_finalize(st);
        return 0;
    }
    if (sqlite3_prepare_v2(s_db, "INSERT INTO artist_fav(artist_id) VALUES(?1);", -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, artist_id);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    return 1;
}

int library_artist_fav_count(void)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_db, "SELECT COUNT(*) FROM artist_fav;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

int library_artist_fav_list(LibArtist *out, int cap)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db || !out || cap <= 0) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_db,
                           "SELECT a.id, a.name, "
                           "(SELECT COUNT(DISTINCT t.album_id) FROM tracks t WHERE t.artist_id=a.id) "
                           "FROM artist_fav f JOIN artists a ON a.id=f.artist_id "
                           "ORDER BY a.name COLLATE NOCASE;",
                           -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *s;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].id = sqlite3_column_int64(st, 0);
        s = sqlite3_column_text(st, 1);
        if (s) {
            snprintf(out[n].name, sizeof(out[n].name), "%s", (const char *)s);
        }
        out[n].album_count = sqlite3_column_int(st, 2);
        n++;
    }
    sqlite3_finalize(st);
    if (n > 1) {
        qsort(out, (size_t)n, sizeof(out[0]), artist_name_cmp);
    }
    return n;
}

int library_artist_fav_remove(int64_t artist_id)
{
    sqlite3_stmt *st = NULL;
    if (!s_db || artist_id <= 0) {
        return -1;
    }
    if (sqlite3_prepare_v2(s_db, "DELETE FROM artist_fav WHERE artist_id=?1;", -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, artist_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return 0;
}

int library_album_fav_has(int64_t album_id)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db || album_id <= 0) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_db, "SELECT 1 FROM album_fav WHERE album_id=?1;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int64(st, 1, album_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = 1;
    }
    sqlite3_finalize(st);
    return n;
}

int library_album_fav_toggle(int64_t album_id)
{
    sqlite3_stmt *st = NULL;
    if (!s_db || album_id <= 0) {
        return -1;
    }
    if (library_album_fav_has(album_id)) {
        if (sqlite3_prepare_v2(s_db, "DELETE FROM album_fav WHERE album_id=?1;", -1, &st, NULL) != SQLITE_OK) {
            return -1;
        }
        sqlite3_bind_int64(st, 1, album_id);
        sqlite3_step(st);
        sqlite3_finalize(st);
        return 0;
    }
    if (sqlite3_prepare_v2(s_db, "INSERT INTO album_fav(album_id) VALUES(?1);", -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, album_id);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    return 1;
}

int library_album_fav_count(void)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_db, "SELECT COUNT(*) FROM album_fav;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

int library_album_fav_list(LibAlbum *out, int cap)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    char sql[1200];
    if (!s_db || !out || cap <= 0) {
        return 0;
    }
    snprintf(sql, sizeof(sql),
             "%s JOIN album_fav f ON f.album_id=b.id GROUP BY b.id;",
             k_album_from);
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        fill_album_from_row(st, &out[n]);
        n++;
    }
    sqlite3_finalize(st);
    if (n > 1) {
        qsort(out, (size_t)n, sizeof(out[0]), album_name_cmp);
    }
    return n;
}

int library_album_fav_remove(int64_t album_id)
{
    sqlite3_stmt *st = NULL;
    if (!s_db || album_id <= 0) {
        return -1;
    }
    if (sqlite3_prepare_v2(s_db, "DELETE FROM album_fav WHERE album_id=?1;", -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, album_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return 0;
}

static int playlist_path_by_id(int64_t playlist_id, char *out, int n)
{
    sqlite3_stmt *st = NULL;
    int rc = -1;
    if (!s_db || playlist_id <= 0 || !out || n <= 0) {
        return -1;
    }
    out[0] = '\0';
    if (sqlite3_prepare_v2(s_db, "SELECT path FROM playlists WHERE id=?1;", -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, playlist_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *s = sqlite3_column_text(st, 0);
        if (s && s[0]) {
            snprintf(out, (size_t)n, "%s", (const char *)s);
            rc = 0;
        }
    }
    sqlite3_finalize(st);
    return rc;
}

int library_playlist_fav_has(int64_t playlist_id)
{
    sqlite3_stmt *st = NULL;
    char path[VIBE_PATH_MAX];
    int n = 0;
    if (playlist_path_by_id(playlist_id, path, (int)sizeof(path)) != 0) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_db, "SELECT 1 FROM playlist_fav WHERE path=?1;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = 1;
    }
    sqlite3_finalize(st);
    return n;
}

int library_playlist_fav_toggle(int64_t playlist_id)
{
    sqlite3_stmt *st = NULL;
    char path[VIBE_PATH_MAX];
    if (playlist_path_by_id(playlist_id, path, (int)sizeof(path)) != 0) {
        return -1;
    }
    if (library_playlist_fav_has(playlist_id)) {
        if (sqlite3_prepare_v2(s_db, "DELETE FROM playlist_fav WHERE path=?1;", -1, &st, NULL) != SQLITE_OK) {
            return -1;
        }
        sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
        return 0;
    }
    if (sqlite3_prepare_v2(s_db, "INSERT INTO playlist_fav(path) VALUES(?1);", -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    return 1;
}

int library_playlist_fav_count(void)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_db,
                           "SELECT COUNT(*) FROM playlist_fav f "
                           "JOIN playlists p ON p.path=f.path;",
                           -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

int library_playlist_fav_list(LibPlaylist *out, int cap)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db || !out || cap <= 0) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_db,
                           "SELECT p.id, p.name, "
                           "(SELECT COUNT(*) FROM playlist_items i WHERE i.playlist_id=p.id) "
                           "FROM playlist_fav f JOIN playlists p ON p.path=f.path "
                           "ORDER BY p.name COLLATE NOCASE;",
                           -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *s;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].id = sqlite3_column_int64(st, 0);
        s = sqlite3_column_text(st, 1);
        if (s) {
            snprintf(out[n].name, sizeof(out[n].name), "%s", (const char *)s);
        }
        out[n].item_count = sqlite3_column_int(st, 2);
        n++;
    }
    sqlite3_finalize(st);
    if (n > 1) {
        qsort(out, (size_t)n, sizeof(out[0]), playlist_name_cmp);
    }
    return n;
}

int library_playlist_fav_remove(int64_t playlist_id)
{
    sqlite3_stmt *st = NULL;
    char path[VIBE_PATH_MAX];
    if (playlist_path_by_id(playlist_id, path, (int)sizeof(path)) != 0) {
        return -1;
    }
    if (sqlite3_prepare_v2(s_db, "DELETE FROM playlist_fav WHERE path=?1;", -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return 0;
}
