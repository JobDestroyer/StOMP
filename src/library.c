#include "library.h"

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
static char s_roots[VIBE_MAX_MUSIC_DIRS][VIBE_PATH_MAX];
static int s_nroots;
static int s_cnt_albums = -1;
static int s_cnt_artists = -1;
static int s_cnt_folders = -1;
static int s_scan_writes;

static void counts_invalidate(void);
static void scan_checkpoint(void);
static int album_name_cmp(const void *a, const void *b);
static int artist_name_cmp(const void *a, const void *b);
static int playlist_name_cmp(const void *a, const void *b);
static int64_t s_queue[VIBE_QUEUE_MAX];
static int s_qlen;
static int s_qidx;

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
    "  name TEXT NOT NULL"
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
                           "INSERT OR IGNORE INTO folders(path, parent_id) VALUES(?1,?2);",
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
    "COUNT(t.id) "
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
    sqlite3_busy_timeout(s_db, 5000);
    sqlite3_exec(s_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_exec(s_db, "PRAGMA temp_store=MEMORY;", NULL, NULL, NULL);
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
    if (++s_scan_writes < 256) {
        return;
    }
    s_scan_writes = 0;
    sqlite3_exec(s_db, "COMMIT;", NULL, NULL, NULL);
    sqlite3_exec(s_db, "BEGIN;", NULL, NULL, NULL);
}

void library_close(void)
{
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
    sqlite3_exec(s_db, "BEGIN;", NULL, NULL, NULL);
    for (int i = 0; i < s_nroots; i++) {
        walk_dir(s_roots[i], 0, 0);
    }
    if (sqlite3_prepare_v2(s_db, "SELECT id, path FROM tracks;", -1, &st, NULL) == SQLITE_OK) {
        int64_t *ids = NULL;
        int nids = 0, cap = 0, k;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *path = (const char *)sqlite3_column_text(st, 1);
            if (!path_in_roots(path)) {
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
    sqlite3_exec(s_db, "DELETE FROM playlist_items WHERE track_id NOT IN (SELECT id FROM tracks);",
                 NULL, NULL, NULL);
    sqlite3_exec(s_db, "DELETE FROM last_queue WHERE track_id NOT IN (SELECT id FROM tracks);",
                 NULL, NULL, NULL);
    sqlite3_exec(s_db, "DELETE FROM albums WHERE NOT EXISTS (SELECT 1 FROM tracks WHERE tracks.album_id = albums.id);",
                 NULL, NULL, NULL);
    sqlite3_exec(s_db, "DELETE FROM artists WHERE NOT EXISTS (SELECT 1 FROM tracks WHERE tracks.artist_id = artists.id);",
                 NULL, NULL, NULL);
    sqlite3_exec(s_db, "COMMIT;", NULL, NULL, NULL);
    counts_invalidate();
    fprintf(stderr, "StOMP: library has %d tracks in %d albums\n",
            library_track_count(), library_album_count());
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
    snprintf(sql, sizeof(sql), "%s WHERE t.album_id=?1 ORDER BY t.track_no, t.title LIMIT %d;", k_track_select, cap);
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
             "GROUP BY b.id ORDER BY b.name COLLATE NOCASE LIMIT ?2;",
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
        qsort(out, (size_t)n, sizeof(out[0]), album_name_cmp);
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
             "ORDER BY t.title COLLATE NOCASE LIMIT %d;",
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
                 "%s WHERE t.folder_id=?1 ORDER BY t.title COLLATE NOCASE LIMIT %d;",
                 k_track_select, cap);
        return list_tracks_sql(sql, folder_id, out, cap);
    }
    snprintf(sql, sizeof(sql),
             "%s JOIN folders f ON f.id=t.folder_id WHERE f.parent_id IS NULL "
             "ORDER BY t.title COLLATE NOCASE LIMIT %d;",
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
    char like[VIBE_NAME_MAX + 8];
    char sql[768];
    if (!s_db || !out || cap <= 0 || !query) {
        return 0;
    }
    snprintf(like, sizeof(like), "%%%s%%", query);
    snprintf(sql, sizeof(sql),
             "%s WHERE t.title LIKE ?1 OR a.name LIKE ?1 OR b.name LIKE ?1 "
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
    if (position_ms) {
        *position_ms = library_session_get_int("position_ms", 0);
    }
    if (library_session_get("track_id", buf, (int)sizeof(buf)) != 0) {
        return -1;
    }
    id = (int64_t)atoll(buf);
    return library_get_track(id, out);
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
    return n;
}

int library_queue_add_album(int64_t album_id)
{
    return add_tracks_sql("SELECT id FROM tracks WHERE album_id=?1 ORDER BY track_no, title;", album_id);
}

int library_queue_add_artist(int64_t artist_id)
{
    return add_tracks_sql("SELECT t.id FROM tracks t JOIN albums b ON b.id=t.album_id "
                          "WHERE t.artist_id=?1 ORDER BY b.name, t.track_no, t.title;",
                          artist_id);
}

int library_queue_add_folder(int64_t folder_id)
{
    return add_tracks_sql(
        "SELECT t.id FROM tracks t JOIN folders f ON f.id=?1 "
        "WHERE t.path = f.path OR substr(t.path, 1, length(f.path)+1) = f.path || '/' "
        "ORDER BY t.title COLLATE NOCASE;",
        folder_id);
}

int library_queue_play_album_from(int64_t album_id, int64_t start_track_id)
{
    sqlite3_stmt *st = NULL;
    int started = 0;
    s_qlen = 0;
    s_qidx = 0;
    if (sqlite3_prepare_v2(s_db,
                           "SELECT id FROM tracks WHERE album_id=?1 ORDER BY track_no, title;",
                           -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, album_id);
    while (sqlite3_step(st) == SQLITE_ROW && s_qlen < VIBE_QUEUE_MAX) {
        int64_t id = sqlite3_column_int64(st, 0);
        if (!started && start_track_id != 0 && id != start_track_id) {
            continue;
        }
        started = 1;
        s_queue[s_qlen++] = id;
    }
    sqlite3_finalize(st);
    return s_qlen > 0 ? 0 : -1;
}

int library_queue_play_folder_from(int64_t folder_id, int64_t start_track_id)
{
    sqlite3_stmt *st = NULL;
    int started = 0;
    s_qlen = 0;
    s_qidx = 0;
    if (sqlite3_prepare_v2(s_db,
                           "SELECT id FROM tracks WHERE folder_id=?1 ORDER BY title COLLATE NOCASE;",
                           -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, folder_id);
    while (sqlite3_step(st) == SQLITE_ROW && s_qlen < VIBE_QUEUE_MAX) {
        int64_t id = sqlite3_column_int64(st, 0);
        if (!started && start_track_id != 0 && id != start_track_id) {
            continue;
        }
        started = 1;
        s_queue[s_qlen++] = id;
    }
    sqlite3_finalize(st);
    return s_qlen > 0 ? 0 : -1;
}

int library_queue_play_track(int64_t track_id)
{
    s_qlen = 0;
    s_qidx = 0;
    return library_queue_add(track_id);
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
}

int library_queue_save(void)
{
    sqlite3_stmt *st = NULL;
    if (!s_db) {
        return -1;
    }
    sqlite3_exec(s_db, "DELETE FROM last_queue;", NULL, NULL, NULL);
    if (sqlite3_prepare_v2(s_db, "INSERT INTO last_queue(pos, track_id) VALUES(?1,?2);", -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    for (int i = 0; i < s_qlen; i++) {
        sqlite3_reset(st);
        sqlite3_bind_int(st, 1, i);
        sqlite3_bind_int64(st, 2, s_queue[i]);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    return 0;
}

int library_queue_load(void)
{
    sqlite3_stmt *st = NULL;
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
    if (!s_db || !key || !key[0]) {
        return;
    }
    if (sqlite3_prepare_v2(s_db,
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
    if (!s_db || !key || !key[0]) {
        return 0;
    }
    if (sqlite3_prepare_v2(s_db, "SELECT rating FROM viz_ratings WHERE key=?1;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        r = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return r;
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
