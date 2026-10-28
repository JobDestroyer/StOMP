/* SPDX-License-Identifier: LGPL-2.1-only
 * Copyright (C) 2026 JobDestroyer
 */

#include "karaoke.h"
#include "zipread.h"

#include "sqlite3.h"

#include <libavformat/avformat.h>
#include <libavutil/dict.h>

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

static sqlite3 *s_db;
static sqlite3 *s_scan;
static _Thread_local sqlite3 *s_tls;

static sqlite3 *kdb(void)
{
    return s_tls ? s_tls : s_db;
}

static char s_roots[VIBE_MAX_MUSIC_DIRS][VIBE_PATH_MAX];
static int s_nroots;
static int s_cnt_songs = -1;
static int s_cnt_artists = -1;
static int s_cnt_genres = -1;
static int s_cnt_folders = -1;
static int s_scan_writes;
static int s_scanning;

static int64_t *s_queue;
static int64_t *s_qhas;
static int s_qcap;
static int s_qlen;
static int s_qidx;
static int s_qhas_n;
static int s_qhas_ready;

static void counts_invalidate(void)
{
    s_cnt_songs = s_cnt_artists = s_cnt_genres = s_cnt_folders = -1;
}

static void queue_mutated(void)
{
    s_qhas_ready = 0;
}

static int queue_ensure(int need)
{
    int cap;
    int64_t *q, *h;
    if (need < 0) {
        return -1;
    }
    if (need <= s_qcap) {
        return 0;
    }
    cap = s_qcap > 0 ? s_qcap : 256;
    while (cap < need) {
        if (cap > INT_MAX / 16) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    q = (int64_t *)malloc((size_t)cap * sizeof(int64_t));
    h = (int64_t *)malloc((size_t)cap * sizeof(int64_t));
    if (!q || !h) {
        free(q);
        free(h);
        return -1;
    }
    if (s_queue && s_qlen > 0) {
        memcpy(q, s_queue, (size_t)s_qlen * sizeof(int64_t));
    }
    free(s_queue);
    free(s_qhas);
    s_queue = q;
    s_qhas = h;
    s_qcap = cap;
    s_qhas_ready = 0;
    return 0;
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
    "CREATE TABLE IF NOT EXISTS songs ("
    "  id INTEGER PRIMARY KEY,"
    "  path TEXT UNIQUE NOT NULL,"
    "  title TEXT,"
    "  artist_id INTEGER,"
    "  genre TEXT NOT NULL DEFAULT 'Uncategorized' COLLATE NOCASE,"
    "  folder_id INTEGER,"
    "  sidecar TEXT,"
    "  sidecar_kind INTEGER DEFAULT 0,"
    "  duration_ms INTEGER,"
    "  mtime INTEGER"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_kara_title ON songs(title COLLATE NOCASE, id);"
    "CREATE INDEX IF NOT EXISTS idx_kara_artist ON songs(artist_id);"
    "CREATE INDEX IF NOT EXISTS idx_kara_genre ON songs(genre COLLATE NOCASE);"
    "CREATE INDEX IF NOT EXISTS idx_kara_folder ON songs(folder_id);"
    "CREATE INDEX IF NOT EXISTS idx_kara_folders_parent ON folders(parent_id);"
    "CREATE TABLE IF NOT EXISTS last_queue ("
    "  pos INTEGER PRIMARY KEY,"
    "  song_id INTEGER"
    ");"
    "CREATE TABLE IF NOT EXISTS session ("
    "  key TEXT PRIMARY KEY,"
    "  value TEXT"
    ");"
    "CREATE TABLE IF NOT EXISTS song_fav ("
    "  song_id INTEGER PRIMARY KEY"
    ");";

const char *karaoke_side_label(int kind)
{
    switch (kind) {
    case KARA_SIDE_CDG:
        return "CDG";
    case KARA_SIDE_LRC:
        return "LRC";
    case KARA_SIDE_ASS:
        return "ASS";
    case KARA_SIDE_KAR:
        return "KAR";
    case KARA_SIDE_ULTRASTAR:
        return "UltraStar";
    case KARA_SIDE_KFN:
        return "KFN";
    case KARA_SIDE_KOK:
        return "KOK";
    default:
        return "";
    }
}

static int i64_cmp(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a;
    int64_t y = *(const int64_t *)b;
    if (x < y) {
        return -1;
    }
    if (x > y) {
        return 1;
    }
    return 0;
}

static void qhas_rebuild(void)
{
    int i, w = 0;
    s_qhas_n = 0;
    s_qhas_ready = 1;
    if (!s_qhas || s_qlen <= 0) {
        return;
    }
    memcpy(s_qhas, s_queue, (size_t)s_qlen * sizeof(int64_t));
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
}

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

static int is_kara_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) {
        return 0;
    }
    return strcasecmp(dot, ".kar") == 0 || strcasecmp(dot, ".kfn") == 0;
}

static int is_zip_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ".zip") == 0;
}

int karaoke_path_is_zip(const char *path)
{
    return path && path[0] && is_zip_name(path);
}

static const char *zip_base(const char *name)
{
    const char *s = strrchr(name, '/');
    const char *b = strrchr(name, '\\');
    if (b && (!s || b > s)) {
        s = b;
    }
    return s ? s + 1 : name;
}

static int zip_member_is_cdg(const char *name)
{
    size_t n;
    name = zip_base(name);
    n = strlen(name);
    if (n >= 7 && strcasecmp(name + n - 7, ".cdg.gz") == 0) {
        return 1;
    }
    if (n >= 4 && strcasecmp(name + n - 4, ".cdg") == 0) {
        return 1;
    }
    return 0;
}

static int zip_member_skip(const char *name)
{
    const char *base = zip_base(name);
    if (!base || !base[0] || base[0] == '.') {
        return 1;
    }
    if (strstr(name, "__MACOSX")) {
        return 1;
    }
    return 0;
}

static int zip_pick_members(const ZipEnt *ents, int n, const ZipEnt **audio, const ZipEnt **cdg)
{
    int i;
    const ZipEnt *a = NULL, *c = NULL, *mp3 = NULL;
    *audio = NULL;
    *cdg = NULL;
    for (i = 0; i < n; i++) {
        const char *base;
        if (zip_member_skip(ents[i].name)) {
            continue;
        }
        base = zip_base(ents[i].name);
        if (is_audio_ext(base)) {
            if (!a) {
                a = &ents[i];
            }
            if (!mp3 && strcasecmp(strrchr(base, '.'), ".mp3") == 0) {
                mp3 = &ents[i];
            }
        } else if (zip_member_is_cdg(ents[i].name) && !c) {
            c = &ents[i];
        }
    }
    if (mp3) {
        a = mp3;
    }
    if (!a) {
        return -1;
    }
    if (c && a) {
        /* Prefer a CDG whose stem matches the audio stem. */
        char astem[ZIPREAD_NAME_MAX], cstem[ZIPREAD_NAME_MAX];
        const char *ab = zip_base(a->name);
        char *dot;
        snprintf(astem, sizeof(astem), "%s", ab);
        dot = strrchr(astem, '.');
        if (dot) {
            *dot = '\0';
        }
        for (i = 0; i < n; i++) {
            const char *cb, *end;
            char *cdot;
            if (zip_member_skip(ents[i].name) || !zip_member_is_cdg(ents[i].name)) {
                continue;
            }
            cb = zip_base(ents[i].name);
            snprintf(cstem, sizeof(cstem), "%s", cb);
            cdot = strrchr(cstem, '.');
            if (cdot && strcasecmp(cdot, ".gz") == 0) {
                *cdot = '\0';
                cdot = strrchr(cstem, '.');
            }
            if (cdot) {
                *cdot = '\0';
            }
            end = cstem;
            if (strcasecmp(astem, end) == 0) {
                c = &ents[i];
                break;
            }
        }
    }
    *audio = a;
    *cdg = c;
    return 0;
}

static int kara_cache_dir(char *out, int n)
{
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    char parent[VIBE_PATH_MAX];
    if (xdg && xdg[0]) {
        snprintf(out, (size_t)n, "%s/stomp", xdg);
    } else if (home && home[0]) {
        snprintf(parent, sizeof(parent), "%s/.cache", home);
        mkdir(parent, 0700);
        snprintf(out, (size_t)n, "%s/stomp", parent);
    } else {
        snprintf(out, (size_t)n, "/tmp/stomp-cache");
    }
    if (mkdir(out, 0700) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static char s_zip_src[VIBE_PATH_MAX];
static char s_zip_audio[VIBE_PATH_MAX];
static char s_zip_cdg[VIBE_PATH_MAX];

static uint32_t zip_path_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; s && *s; s++) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

int karaoke_zip_prepare(const char *zip_path)
{
    ZipEnt *ents = NULL;
    int n = 0;
    const ZipEnt *audio = NULL, *cdg = NULL;
    char dir[VIBE_PATH_MAX], ext[16];
    char new_audio[VIBE_PATH_MAX], new_cdg[VIBE_PATH_MAX];
    char old_audio[VIBE_PATH_MAX], old_cdg[VIBE_PATH_MAX];
    const char *dot;
    struct stat st, zs;
    uint32_t h;

    if (!zip_path || !zip_path[0] || !is_zip_name(zip_path)) {
        return -1;
    }
    if (s_zip_src[0] && strcmp(s_zip_src, zip_path) == 0 && s_zip_audio[0] &&
        stat(s_zip_audio, &st) == 0 && S_ISREG(st.st_mode)) {
        int cdg_ok = !s_zip_cdg[0] ||
                     (stat(s_zip_cdg, &zs) == 0 && S_ISREG(zs.st_mode));
        if (cdg_ok && (stat(zip_path, &zs) != 0 || zs.st_mtime <= st.st_mtime)) {
            return 0;
        }
    }
    if (zip_list(zip_path, &ents, &n) != 0 || zip_pick_members(ents, n, &audio, &cdg) != 0) {
        zip_list_free(ents);
        return -1;
    }
    if (kara_cache_dir(dir, (int)sizeof(dir)) != 0) {
        zip_list_free(ents);
        return -1;
    }
    ext[0] = '\0';
    dot = strrchr(zip_base(audio->name), '.');
    if (dot && strlen(dot) < sizeof(ext)) {
        snprintf(ext, sizeof(ext), "%s", dot);
    } else {
        snprintf(ext, sizeof(ext), ".mp3");
    }
    h = zip_path_hash(zip_path);
    snprintf(new_audio, sizeof(new_audio), "%s/kara-%08x-audio%s", dir, h, ext);
    new_cdg[0] = '\0';
    if (zip_extract_file(zip_path, audio, new_audio) != 0) {
        zip_list_free(ents);
        return -1;
    }
    if (cdg) {
        const char *cb = zip_base(cdg->name);
        size_t cn = strlen(cb);
        const char *cext = (cn >= 7 && strcasecmp(cb + cn - 7, ".cdg.gz") == 0) ? ".cdg.gz" : ".cdg";
        snprintf(new_cdg, sizeof(new_cdg), "%s/kara-%08x-lyrics%s", dir, h, cext);
        if (zip_extract_file(zip_path, cdg, new_cdg) != 0) {
            new_cdg[0] = '\0';
        }
    }
    snprintf(old_audio, sizeof(old_audio), "%s", s_zip_audio);
    snprintf(old_cdg, sizeof(old_cdg), "%s", s_zip_cdg);
    snprintf(s_zip_audio, sizeof(s_zip_audio), "%s", new_audio);
    snprintf(s_zip_cdg, sizeof(s_zip_cdg), "%s", new_cdg);
    snprintf(s_zip_src, sizeof(s_zip_src), "%s", zip_path);
    zip_list_free(ents);
    if (old_audio[0] && strcmp(old_audio, s_zip_audio) != 0) {
        remove(old_audio);
    }
    if (old_cdg[0] && strcmp(old_cdg, s_zip_cdg) != 0) {
        remove(old_cdg);
    }
    return 0;
}

const char *karaoke_zip_audio_path(void)
{
    return s_zip_audio;
}

const char *karaoke_zip_cdg_path(void)
{
    return s_zip_cdg;
}

static void trim_ws(char *s)
{
    char *e;
    if (!s) {
        return;
    }
    while (*s == ' ' || *s == '\t' || *s == '_') {
        memmove(s, s + 1, strlen(s));
    }
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '_' || e[-1] == '.')) {
        *--e = '\0';
    }
}

static int is_publisher(const char *s)
{
    static const char *names[] = {
        "karaoke", "karaoke version", "instrumental", "party tyme",
        "party tyme karaoke", "zoom karaoke", "zoom", "sunfly",
        "chartbusters", "sbi", "sbi karaoke", "stingray",
        "singing machine", "karaoke cdg", "dj karaoke", "karaoke hits",
        "backing track", "tribute", "the hit crew", "karaoke maker",
        "scoti", "dkkaraoke", "sound choice", "sweet georgia brown",
        NULL
    };
    int i;
    if (!s || !s[0]) {
        return 1;
    }
    for (i = 0; names[i]; i++) {
        if (strcasecmp(s, names[i]) == 0) {
            return 1;
        }
    }
    if (strncasecmp(s, "karaoke", 7) == 0) {
        return 1;
    }
    return 0;
}

static int is_junk_genre(const char *s)
{
    return !s || !s[0] || strcasecmp(s, "karaoke") == 0 ||
           strcasecmp(s, "other") == 0 || strcasecmp(s, "unknown") == 0 ||
           strcasecmp(s, "general") == 0 || strcasecmp(s, "uncategorized") == 0;
}

static int contains_ci(const char *hay, const char *needle)
{
    size_t n, i;
    if (!hay || !needle || !needle[0]) {
        return 0;
    }
    n = strlen(needle);
    for (i = 0; hay[i]; i++) {
        size_t k;
        for (k = 0; k < n; k++) {
            unsigned char a = (unsigned char)hay[i + k];
            unsigned char b = (unsigned char)needle[k];
            if (!a) {
                return 0;
            }
            if (tolower(a) != tolower(b)) {
                break;
            }
        }
        if (k == n) {
            return 1;
        }
    }
    return 0;
}

static const char *match_genre_token(const char *s)
{
    static const char *g[] = {
        "Christmas", "Holiday", "Disney", "Duets", "Soundtrack", "Showtunes",
        "Musical", "Hip-Hop", "Hip Hop", "R&B", "K-Pop", "J-Pop",
        "Children's", "Kids", "Christian", "Gospel", "Country", "Metal",
        "Reggae", "Disco", "Dance", "Punk", "Indie", "Alternative",
        "Classical", "Latin", "Salsa", "Motown", "Funk", "Electronic",
        "House", "Techno", "Blues", "Jazz", "Folk", "Soul", "Rock",
        "Pop", "Rap", "World", "Irish", "Comedy", "Wedding", "Patriotic",
        "2020s", "2010s", "2000s", "90s", "80s", "70s", "60s", "50s",
        NULL
    };
    int i;
    if (!s || !s[0]) {
        return NULL;
    }
    for (i = 0; g[i]; i++) {
        if (contains_ci(s, g[i])) {
            return g[i];
        }
    }
    return NULL;
}

static void strip_markup(char *s)
{
    char *a, *b;
    if (!s) {
        return;
    }
    for (;;) {
        a = strchr(s, '(');
        b = a ? strchr(a, ')') : NULL;
        if (!a || !b) {
            break;
        }
        memmove(a, b + 1, strlen(b + 1) + 1);
    }
    for (;;) {
        a = strchr(s, '[');
        b = a ? strchr(a, ']') : NULL;
        if (!a || !b) {
            break;
        }
        memmove(a, b + 1, strlen(b + 1) + 1);
    }
    trim_ws(s);
}

static void basename_no_ext(const char *path, char *out, int n)
{
    const char *slash, *src;
    char *dot;
    slash = strrchr(path, '/');
    src = slash ? slash + 1 : path;
    snprintf(out, (size_t)n, "%s", src);
    dot = strrchr(out, '.');
    if (dot && dot != out) {
        *dot = '\0';
    }
    strip_markup(out);
}

static void parse_filename(const char *path, char *artist, int an, char *title, int tn)
{
    char base[VIBE_NAME_MAX];
    char *sep;
    basename_no_ext(path, base, (int)sizeof(base));
    sep = strstr(base, " - ");
    if (!sep) {
        sep = strstr(base, " _ ");
    }
    if (sep && sep != base) {
        *sep = '\0';
        snprintf(artist, (size_t)an, "%s", base);
        snprintf(title, (size_t)tn, "%s", sep + 3);
        trim_ws(artist);
        trim_ws(title);
        if (is_publisher(artist)) {
            artist[0] = '\0';
        }
    } else {
        artist[0] = '\0';
        snprintf(title, (size_t)tn, "%s", base);
        trim_ws(title);
    }
    if (!title[0]) {
        snprintf(title, (size_t)tn, "Unknown");
    }
}

static int is_ultrastar_txt(const char *path)
{
    FILE *f;
    char buf[256];
    size_t n;
    f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, "#TITLE") != NULL || strstr(buf, "#ARTIST") != NULL;
}

static void replace_ext(const char *path, const char *ext, char *out, int n)
{
    const char *dot, *slash;
    size_t stem;
    slash = strrchr(path, '/');
    dot = strrchr(path, '.');
    if (!dot || (slash && dot < slash)) {
        snprintf(out, (size_t)n, "%s%s", path, ext);
        return;
    }
    stem = (size_t)(dot - path);
    if (stem + strlen(ext) + 1 > (size_t)n) {
        snprintf(out, (size_t)n, "%s", path);
        return;
    }
    memcpy(out, path, stem);
    snprintf(out + stem, (size_t)n - stem, "%s", ext);
}

static int find_sidecar(const char *audio, char *out, int n, int *kind)
{
    static const struct {
        const char *ext;
        int kind;
    } try[] = {
        {".cdg", KARA_SIDE_CDG},
        {".CDG", KARA_SIDE_CDG},
        {".cdg.gz", KARA_SIDE_CDG},
        {".CDG.gz", KARA_SIDE_CDG},
        {".CDG.GZ", KARA_SIDE_CDG},
        {".lrc", KARA_SIDE_LRC},
        {".ass", KARA_SIDE_ASS},
        {".kok", KARA_SIDE_KOK},
        {".txt", KARA_SIDE_ULTRASTAR},
        {NULL, 0}
    };
    struct stat st;
    int i;
    out[0] = '\0';
    *kind = KARA_SIDE_NONE;
    for (i = 0; try[i].ext; i++) {
        replace_ext(audio, try[i].ext, out, n);
        if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) {
            if (try[i].kind == KARA_SIDE_ULTRASTAR && !is_ultrastar_txt(out)) {
                continue;
            }
            *kind = try[i].kind;
            return 0;
        }
    }
    snprintf(out, (size_t)n, "%s.cdg", audio);
    if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) {
        *kind = KARA_SIDE_CDG;
        return 0;
    }
    snprintf(out, (size_t)n, "%s.cdg.gz", audio);
    if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) {
        *kind = KARA_SIDE_CDG;
        return 0;
    }
    out[0] = '\0';
    return -1;
}

static void genre_from_path(const char *path, char *out, int n)
{
    const char *hit = NULL;
    char tmp[VIBE_PATH_MAX];
    char *part;
    snprintf(tmp, sizeof(tmp), "%s", path ? path : "");
    part = tmp;
    while (*part) {
        char *slash = strchr(part, '/');
        if (slash) {
            *slash = '\0';
        }
        {
            const char *g = match_genre_token(part);
            if (g) {
                hit = g;
            }
        }
        if (!slash) {
            break;
        }
        part = slash + 1;
    }
    if (hit) {
        snprintf(out, (size_t)n, "%s", hit);
    } else {
        snprintf(out, (size_t)n, "Uncategorized");
    }
}

static int read_ultrastar(const char *txt, char *title, int tn, char *artist, int an,
                          char *genre, int gn, char *mp3, int mn)
{
    FILE *f;
    char line[512];
    title[0] = artist[0] = genre[0] = mp3[0] = '\0';
    f = fopen(txt, "r");
    if (!f) {
        return -1;
    }
    while (fgets(line, (int)sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        char *cr;
        if (nl) {
            *nl = '\0';
        }
        cr = strchr(line, '\r');
        if (cr) {
            *cr = '\0';
        }
        if (strncmp(line, "#TITLE:", 7) == 0) {
            snprintf(title, (size_t)tn, "%s", line + 7);
        } else if (strncmp(line, "#ARTIST:", 8) == 0) {
            snprintf(artist, (size_t)an, "%s", line + 8);
        } else if (strncmp(line, "#GENRE:", 7) == 0) {
            snprintf(genre, (size_t)gn, "%s", line + 7);
        } else if (strncmp(line, "#MP3:", 5) == 0) {
            snprintf(mp3, (size_t)mn, "%s", line + 5);
        }
        if (line[0] && line[0] != '#') {
            break;
        }
    }
    fclose(f);
    trim_ws(title);
    trim_ws(artist);
    trim_ws(genre);
    trim_ws(mp3);
    return title[0] ? 0 : -1;
}

static int read_tags(const char *path, char *title, int tn, char *artist, int an,
                     char *genre, int gn, int *duration_ms)
{
    AVFormatContext *fmt = NULL;
    AVDictionary *opts = NULL;
    const AVDictionaryEntry *e;
    *duration_ms = 0;
    title[0] = artist[0] = genre[0] = '\0';
    av_dict_set(&opts, "protocol_whitelist", "file", 0);
    if (avformat_open_input(&fmt, path, NULL, &opts) < 0) {
        av_dict_free(&opts);
        return -1;
    }
    av_dict_free(&opts);
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        return -1;
    }
    e = av_dict_get(fmt->metadata, "title", NULL, 0);
    if (e && e->value) {
        snprintf(title, (size_t)tn, "%s", e->value);
    }
    e = av_dict_get(fmt->metadata, "artist", NULL, 0);
    if (e && e->value) {
        snprintf(artist, (size_t)an, "%s", e->value);
    }
    e = av_dict_get(fmt->metadata, "genre", NULL, 0);
    if (e && e->value) {
        snprintf(genre, (size_t)gn, "%s", e->value);
    }
    if (fmt->duration > 0) {
        *duration_ms = (int)(fmt->duration / 1000);
    }
    avformat_close_input(&fmt);
    strip_markup(title);
    strip_markup(artist);
    trim_ws(title);
    trim_ws(artist);
    trim_ws(genre);
    if (is_publisher(artist)) {
        artist[0] = '\0';
    }
    if (is_junk_genre(genre)) {
        genre[0] = '\0';
    }
    return 0;
}

static int64_t get_or_create_artist(const char *name)
{
    sqlite3_stmt *st = NULL;
    int64_t id = 0;
    const char *n = (name && name[0]) ? name : "Unknown Artist";
    if (sqlite3_prepare_v2(kdb(),
                           "INSERT INTO artists(name) VALUES(?1) "
                           "ON CONFLICT(name) DO NOTHING;",
                           -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, n, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    if (sqlite3_prepare_v2(kdb(), "SELECT id FROM artists WHERE name=?1 COLLATE NOCASE;",
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

static int64_t get_or_create_folder(const char *path, int64_t parent_id)
{
    sqlite3_stmt *st = NULL;
    int64_t id = 0;
    if (sqlite3_prepare_v2(kdb(),
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
    if (sqlite3_prepare_v2(kdb(), "SELECT id FROM folders WHERE path=?1;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        id = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return id;
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
    sqlite3_exec(kdb(), "COMMIT;", NULL, NULL, NULL);
    sqlite3_exec(kdb(), "BEGIN;", NULL, NULL, NULL);
}

static void upsert_song(const char *path, time_t mtime, int64_t folder_id, int force_kind,
                        const char *force_side)
{
    sqlite3_stmt *st = NULL;
    char title[VIBE_NAME_MAX], artist[VIBE_NAME_MAX], genre[VIBE_NAME_MAX];
    char fn_artist[VIBE_NAME_MAX], fn_title[VIBE_NAME_MAX];
    char tag_title[VIBE_NAME_MAX], tag_artist[VIBE_NAME_MAX], tag_genre[VIBE_NAME_MAX];
    char side[VIBE_PATH_MAX];
    int duration_ms = 0, kind = KARA_SIDE_NONE;
    int64_t artist_id;
    time_t old_mtime = 0;
    int exists = 0;

    if (sqlite3_prepare_v2(kdb(), "SELECT mtime FROM songs WHERE path=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            exists = 1;
            old_mtime = (time_t)sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
        st = NULL;
    }
    parse_filename(path, fn_artist, (int)sizeof(fn_artist), fn_title, (int)sizeof(fn_title));
    if (exists && old_mtime == mtime) {
        if (fn_artist[0] && !is_publisher(fn_artist)) {
            sqlite3_stmt *cur = NULL;
            const unsigned char *have_artist = NULL;
            const unsigned char *have_title = NULL;
            int64_t aid;
            if (sqlite3_prepare_v2(kdb(),
                                   "SELECT s.title, IFNULL(a.name,'') FROM songs s "
                                   "LEFT JOIN artists a ON a.id=s.artist_id WHERE s.path=?1;",
                                   -1, &cur, NULL) == SQLITE_OK) {
                sqlite3_bind_text(cur, 1, path, -1, SQLITE_TRANSIENT);
                if (sqlite3_step(cur) == SQLITE_ROW) {
                    have_title = sqlite3_column_text(cur, 0);
                    have_artist = sqlite3_column_text(cur, 1);
                }
                if ((!have_artist || strcasecmp((const char *)have_artist, fn_artist) != 0) ||
                    (fn_title[0] && (!have_title || strcasecmp((const char *)have_title, fn_title) != 0))) {
                    sqlite3_finalize(cur);
                    cur = NULL;
                    aid = get_or_create_artist(fn_artist);
                    if (sqlite3_prepare_v2(kdb(),
                                           "UPDATE songs SET title=?1, artist_id=?2 WHERE path=?3;",
                                           -1, &cur, NULL) == SQLITE_OK) {
                        sqlite3_bind_text(cur, 1, fn_title[0] ? fn_title : "Unknown",
                                          -1, SQLITE_TRANSIENT);
                        sqlite3_bind_int64(cur, 2, aid);
                        sqlite3_bind_text(cur, 3, path, -1, SQLITE_TRANSIENT);
                        sqlite3_step(cur);
                    }
                    scan_checkpoint();
                }
                if (cur) {
                    sqlite3_finalize(cur);
                }
            }
        }
        return;
    }
    read_tags(path, tag_title, (int)sizeof(tag_title), tag_artist, (int)sizeof(tag_artist),
              tag_genre, (int)sizeof(tag_genre), &duration_ms);
    if (fn_artist[0]) {
        snprintf(artist, sizeof(artist), "%s", fn_artist);
    } else if (tag_artist[0]) {
        snprintf(artist, sizeof(artist), "%s", tag_artist);
    } else {
        snprintf(artist, sizeof(artist), "Unknown Artist");
    }
    if (fn_title[0] && (is_publisher(tag_title) || !tag_title[0] ||
                        strcasecmp(tag_title, fn_title) == 0)) {
        snprintf(title, sizeof(title), "%s", fn_title);
    } else if (tag_title[0] && !is_publisher(tag_title)) {
        snprintf(title, sizeof(title), "%s", tag_title);
    } else {
        snprintf(title, sizeof(title), "%s", fn_title[0] ? fn_title : "Unknown");
    }
    if (tag_genre[0]) {
        snprintf(genre, sizeof(genre), "%s", tag_genre);
    } else {
        genre_from_path(path, genre, (int)sizeof(genre));
    }
    side[0] = '\0';
    if (force_kind) {
        kind = force_kind;
        if (force_side) {
            snprintf(side, sizeof(side), "%s", force_side);
        }
    } else if (is_kara_ext(path)) {
        const char *dot = strrchr(path, '.');
        kind = (dot && strcasecmp(dot, ".kfn") == 0) ? KARA_SIDE_KFN : KARA_SIDE_KAR;
    } else {
        find_sidecar(path, side, (int)sizeof(side), &kind);
        if (kind == KARA_SIDE_ULTRASTAR) {
            char ut[VIBE_NAME_MAX], ua[VIBE_NAME_MAX], ug[VIBE_NAME_MAX], um[VIBE_PATH_MAX];
            if (read_ultrastar(side, ut, (int)sizeof(ut), ua, (int)sizeof(ua),
                               ug, (int)sizeof(ug), um, (int)sizeof(um)) == 0) {
                if (ut[0]) {
                    snprintf(title, sizeof(title), "%s", ut);
                }
                if (ua[0] && !is_publisher(ua)) {
                    snprintf(artist, sizeof(artist), "%s", ua);
                }
                if (ug[0] && !is_junk_genre(ug)) {
                    snprintf(genre, sizeof(genre), "%s", ug);
                }
            }
        }
    }
    artist_id = get_or_create_artist(artist);
    if (folder_id <= 0) {
        char dir[VIBE_PATH_MAX];
        const char *slash = strrchr(path, '/');
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
        folder_id = get_or_create_folder(dir, 0);
    }
    if (sqlite3_prepare_v2(kdb(),
                           "INSERT INTO songs(path,title,artist_id,genre,folder_id,sidecar,sidecar_kind,duration_ms,mtime) "
                           "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9) "
                           "ON CONFLICT(path) DO UPDATE SET title=excluded.title, artist_id=excluded.artist_id, "
                           "genre=excluded.genre, folder_id=excluded.folder_id, sidecar=excluded.sidecar, "
                           "sidecar_kind=excluded.sidecar_kind, duration_ms=excluded.duration_ms, mtime=excluded.mtime;",
                           -1, &st, NULL) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, artist_id);
    sqlite3_bind_text(st, 4, genre, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, folder_id);
    if (side[0]) {
        sqlite3_bind_text(st, 6, side, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(st, 6);
    }
    sqlite3_bind_int(st, 7, kind);
    sqlite3_bind_int(st, 8, duration_ms);
    sqlite3_bind_int64(st, 9, (sqlite3_int64)mtime);
    sqlite3_step(st);
    sqlite3_finalize(st);
    scan_checkpoint();
}

static void upsert_zip_song(const char *path, time_t mtime, int64_t folder_id)
{
    ZipEnt *ents = NULL;
    int n = 0, duration_ms = 0, kind = KARA_SIDE_NONE;
    const ZipEnt *audio = NULL, *cdg = NULL;
    sqlite3_stmt *st = NULL;
    char title[VIBE_NAME_MAX], artist[VIBE_NAME_MAX], genre[VIBE_NAME_MAX];
    time_t old_mtime = 0;
    int exists = 0;
    int64_t artist_id;

    if (sqlite3_prepare_v2(kdb(), "SELECT mtime FROM songs WHERE path=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            exists = 1;
            old_mtime = (time_t)sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
        st = NULL;
    }
    if (exists && old_mtime == mtime) {
        return;
    }
    if (zip_list(path, &ents, &n) != 0 || zip_pick_members(ents, n, &audio, &cdg) != 0) {
        zip_list_free(ents);
        return;
    }
    if (cdg) {
        kind = KARA_SIDE_CDG;
        if (cdg->uncomp_size >= 24) {
            duration_ms = (int)((cdg->uncomp_size / 24u) * 1000u / 300u);
        }
    }
    zip_list_free(ents);
    parse_filename(path, artist, (int)sizeof(artist), title, (int)sizeof(title));
    if (!artist[0]) {
        snprintf(artist, sizeof(artist), "Unknown Artist");
    }
    genre_from_path(path, genre, (int)sizeof(genre));
    artist_id = get_or_create_artist(artist);
    if (folder_id <= 0) {
        char dir[VIBE_PATH_MAX];
        const char *slash = strrchr(path, '/');
        if (slash) {
            size_t nlen = (size_t)(slash - path);
            if (nlen >= sizeof(dir)) {
                nlen = sizeof(dir) - 1;
            }
            memcpy(dir, path, nlen);
            dir[nlen] = '\0';
        } else {
            snprintf(dir, sizeof(dir), ".");
        }
        folder_id = get_or_create_folder(dir, 0);
    }
    if (sqlite3_prepare_v2(kdb(),
                           "INSERT INTO songs(path,title,artist_id,genre,folder_id,sidecar,sidecar_kind,duration_ms,mtime) "
                           "VALUES(?1,?2,?3,?4,?5,NULL,?6,?7,?8) "
                           "ON CONFLICT(path) DO UPDATE SET title=excluded.title, artist_id=excluded.artist_id, "
                           "genre=excluded.genre, folder_id=excluded.folder_id, sidecar=NULL, "
                           "sidecar_kind=excluded.sidecar_kind, duration_ms=excluded.duration_ms, mtime=excluded.mtime;",
                           -1, &st, NULL) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, artist_id);
    sqlite3_bind_text(st, 4, genre, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, folder_id);
    sqlite3_bind_int(st, 6, kind);
    sqlite3_bind_int(st, 7, duration_ms);
    sqlite3_bind_int64(st, 8, (sqlite3_int64)mtime);
    sqlite3_step(st);
    sqlite3_finalize(st);
    scan_checkpoint();
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
        } else if (S_ISREG(st.st_mode) && is_zip_name(de->d_name)) {
            upsert_zip_song(child, st.st_mtime, folder_id);
        } else if (S_ISREG(st.st_mode) && (is_audio_ext(de->d_name) || is_kara_ext(de->d_name))) {
            upsert_song(child, st.st_mtime, folder_id, 0, NULL);
        } else if (S_ISREG(st.st_mode)) {
            const char *dot = strrchr(de->d_name, '.');
            if (dot && strcasecmp(dot, ".txt") == 0 && is_ultrastar_txt(child)) {
                char ut[VIBE_NAME_MAX], ua[VIBE_NAME_MAX], ug[VIBE_NAME_MAX], um[VIBE_PATH_MAX];
                char audio[VIBE_PATH_MAX];
                struct stat ast;
                if (read_ultrastar(child, ut, (int)sizeof(ut), ua, (int)sizeof(ua),
                                   ug, (int)sizeof(ug), um, (int)sizeof(um)) == 0 && um[0]) {
                    if (um[0] == '/') {
                        snprintf(audio, sizeof(audio), "%s", um);
                    } else {
                        snprintf(audio, sizeof(audio), "%s/%s", path, um);
                    }
                    if (stat(audio, &ast) == 0 && S_ISREG(ast.st_mode)) {
                        upsert_song(audio, ast.st_mtime, folder_id, KARA_SIDE_ULTRASTAR, child);
                    }
                }
            }
        }
    }
    closedir(d);
}

static int path_in_root_list(const char *path, const char roots[][VIBE_PATH_MAX], int nroots)
{
    int i;
    if (!path) {
        return 0;
    }
    for (i = 0; i < nroots; i++) {
        size_t n = strlen(roots[i]);
        if (n == 0) {
            continue;
        }
        if (strncmp(path, roots[i], n) == 0 && (path[n] == '\0' || path[n] == '/')) {
            return 1;
        }
    }
    return 0;
}

static void kara_cache_gc(void);

int karaoke_open(const char *db_path)
{
    char *err = NULL;
    if (!db_path || !db_path[0]) {
        return -1;
    }
    if (sqlite3_open(db_path, &s_db) != SQLITE_OK) {
        fprintf(stderr, "StOMP: karaoke sqlite open %s: %s\n", db_path, sqlite3_errmsg(kdb()));
        sqlite3_close(s_db);
        s_db = NULL;
        return -1;
    }
    if (sqlite3_exec(kdb(), k_schema, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "StOMP: karaoke schema: %s\n", err ? err : "");
        sqlite3_free(err);
        sqlite3_close(s_db);
        s_db = NULL;
        return -1;
    }
    sqlite3_busy_timeout(s_db, 8000);
    chmod(db_path, 0600);
    sqlite3_exec(kdb(), "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_exec(kdb(), "PRAGMA temp_store=MEMORY;", NULL, NULL, NULL);
    s_scan = NULL;
    if (sqlite3_open(db_path, &s_scan) != SQLITE_OK) {
        sqlite3_close(s_scan);
        s_scan = NULL;
    } else {
        sqlite3_busy_timeout(s_scan, 8000);
        sqlite3_exec(s_scan, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
        sqlite3_exec(s_scan, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
        sqlite3_exec(s_scan, "PRAGMA temp_store=MEMORY;", NULL, NULL, NULL);
    }
    s_qlen = 0;
    s_qidx = 0;
    counts_invalidate();
    kara_cache_gc();
    return 0;
}

static void kara_cache_gc(void)
{
    char dir[VIBE_PATH_MAX], p[VIBE_PATH_MAX];
    DIR *d;
    struct dirent *de;
    if (kara_cache_dir(dir, (int)sizeof(dir)) != 0) {
        return;
    }
    d = opendir(dir);
    if (!d) {
        return;
    }
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "kara-", 5) != 0) {
            continue;
        }
        snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
        remove(p);
    }
    closedir(d);
}

void karaoke_close(void)
{
    s_tls = NULL;
    if (s_scan) {
        sqlite3_close(s_scan);
        s_scan = NULL;
    }
    if (s_db) {
        sqlite3_close(s_db);
        s_db = NULL;
    }
    free(s_queue);
    free(s_qhas);
    s_queue = NULL;
    s_qhas = NULL;
    s_qcap = s_qlen = s_qidx = s_qhas_n = 0;
    s_qhas_ready = 0;
}

int karaoke_ok(void)
{
    return s_db != NULL;
}

void karaoke_set_scan_roots(const char dirs[][VIBE_PATH_MAX], int count)
{
    char tmp[VIBE_MAX_MUSIC_DIRS][VIBE_PATH_MAX];
    int n = 0, i;
    memset(tmp, 0, sizeof(tmp));
    if (dirs) {
        if (count > VIBE_MAX_MUSIC_DIRS) {
            count = VIBE_MAX_MUSIC_DIRS;
        }
        for (i = 0; i < count; i++) {
            snprintf(tmp[n], VIBE_PATH_MAX, "%s", dirs[i]);
            n++;
        }
    }
    memcpy(s_roots, tmp, sizeof(s_roots));
    s_nroots = n;
}

int karaoke_scan(void)
{
    sqlite3_stmt *st = NULL;
    sqlite3_stmt *del = NULL;
    int64_t *ids = NULL;
    int nids = 0, cap = 0, k;

    if (!s_db) {
        return -1;
    }
    s_tls = s_scan ? s_scan : s_db;
    counts_invalidate();
    s_scan_writes = 0;
    s_scanning = 1;
    {
        char roots[VIBE_MAX_MUSIC_DIRS][VIBE_PATH_MAX];
        int nroots = s_nroots;
        int ri;
        if (nroots > VIBE_MAX_MUSIC_DIRS) {
            nroots = VIBE_MAX_MUSIC_DIRS;
        }
        memcpy(roots, s_roots, sizeof(roots));
        sqlite3_exec(kdb(), "BEGIN;", NULL, NULL, NULL);
        for (ri = 0; ri < nroots; ri++) {
            walk_dir(roots[ri], 0, 0);
        }
    if (sqlite3_prepare_v2(kdb(), "SELECT id, path FROM songs;", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *path = (const char *)sqlite3_column_text(st, 1);
            struct stat pst;
            int gone = !path || !path_in_root_list(path, roots, nroots) ||
                       stat(path, &pst) != 0 || !S_ISREG(pst.st_mode);
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
    }
    if (nids > 0 &&
        sqlite3_prepare_v2(kdb(), "DELETE FROM songs WHERE id=?1;", -1, &del, NULL) == SQLITE_OK) {
        for (k = 0; k < nids; k++) {
            sqlite3_reset(del);
            sqlite3_clear_bindings(del);
            sqlite3_bind_int64(del, 1, ids[k]);
            sqlite3_step(del);
        }
        sqlite3_finalize(del);
    }
    free(ids);
    }
    sqlite3_exec(kdb(), "DELETE FROM last_queue WHERE song_id NOT IN (SELECT id FROM songs);",
                 NULL, NULL, NULL);
    sqlite3_exec(kdb(), "DELETE FROM song_fav WHERE song_id NOT IN (SELECT id FROM songs);",
                 NULL, NULL, NULL);
    sqlite3_exec(kdb(), "DELETE FROM artists WHERE NOT EXISTS (SELECT 1 FROM songs WHERE songs.artist_id = artists.id);",
                 NULL, NULL, NULL);
    sqlite3_exec(kdb(), "DELETE FROM folders WHERE NOT EXISTS (SELECT 1 FROM songs WHERE songs.folder_id = folders.id) "
                       "AND NOT EXISTS (SELECT 1 FROM folders c WHERE c.parent_id = folders.id);",
                 NULL, NULL, NULL);
    sqlite3_exec(kdb(), "COMMIT;", NULL, NULL, NULL);
    s_scanning = 0;
    s_scan_writes = 0;
    counts_invalidate();
    fprintf(stderr, "StOMP: karaoke has %d songs, %d artists, %d genres\n",
            karaoke_song_count(), karaoke_artist_count(), karaoke_genre_count());
    s_tls = NULL;
    return 0;
}

static int count_sql(const char *sql, int *cache)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db) {
        return 0;
    }
    if (*cache >= 0) {
        return *cache;
    }
    if (sqlite3_prepare_v2(kdb(), sql, -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    *cache = n;
    return n;
}

int karaoke_song_count(void)
{
    return count_sql("SELECT COUNT(*) FROM songs;", &s_cnt_songs);
}

int karaoke_artist_count(void)
{
    return count_sql("SELECT COUNT(*) FROM artists;", &s_cnt_artists);
}

int karaoke_genre_count(void)
{
    return count_sql("SELECT COUNT(DISTINCT genre) FROM songs;", &s_cnt_genres);
}

int karaoke_folder_count(void)
{
    return count_sql("SELECT COUNT(*) FROM folders;", &s_cnt_folders);
}

static void fill_song(sqlite3_stmt *st, LibTrack *t)
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
    t->artist_id = sqlite3_column_int64(st, 5);
    t->folder_id = sqlite3_column_int64(st, 6);
    t->duration_ms = sqlite3_column_int(st, 7);
    t->track_no = sqlite3_column_int(st, 8);
}

static const char *k_song_sel =
    "SELECT s.id, s.path, s.title, IFNULL(a.name,''), s.genre, s.artist_id, s.folder_id, "
    "s.duration_ms, s.sidecar_kind "
    "FROM songs s LEFT JOIN artists a ON a.id=s.artist_id ";

int karaoke_get_song(int64_t id, LibTrack *out)
{
    sqlite3_stmt *st = NULL;
    char sql[512];
    int r = -1;
    if (!s_db || !out || id <= 0) {
        return -1;
    }
    snprintf(sql, sizeof(sql), "%s WHERE s.id=?1;", k_song_sel);
    if (sqlite3_prepare_v2(kdb(), sql, -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        fill_song(st, out);
        r = 0;
    }
    sqlite3_finalize(st);
    return r;
}

int karaoke_song_sidecar(int64_t id, char *out, int cap, int *kind)
{
    sqlite3_stmt *st = NULL;
    char audio[VIBE_PATH_MAX];
    char side[VIBE_PATH_MAX];
    int k = KARA_SIDE_NONE;
    struct stat pst;
    audio[0] = '\0';
    side[0] = '\0';
    if (out && cap > 0) {
        out[0] = '\0';
    }
    if (kind) {
        *kind = KARA_SIDE_NONE;
    }
    if (!s_db || id <= 0) {
        return -1;
    }
    if (sqlite3_prepare_v2(kdb(), "SELECT path, sidecar, sidecar_kind FROM songs WHERE id=?1;",
                           -1, &st, NULL) == SQLITE_OK) {
        const unsigned char *s;
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            s = sqlite3_column_text(st, 0);
            if (s) {
                snprintf(audio, sizeof(audio), "%s", (const char *)s);
            }
            s = sqlite3_column_text(st, 1);
            if (s) {
                snprintf(side, sizeof(side), "%s", (const char *)s);
            }
            k = sqlite3_column_int(st, 2);
        }
        sqlite3_finalize(st);
    }
    if (audio[0] && is_zip_name(audio)) {
        if (karaoke_zip_prepare(audio) == 0 && s_zip_cdg[0]) {
            snprintf(side, sizeof(side), "%s", s_zip_cdg);
            k = KARA_SIDE_CDG;
        } else {
            side[0] = '\0';
            k = KARA_SIDE_NONE;
        }
    } else if (!side[0] || stat(side, &pst) != 0 || !S_ISREG(pst.st_mode)) {
        int fk = KARA_SIDE_NONE;
        side[0] = '\0';
        if (audio[0] && find_sidecar(audio, side, (int)sizeof(side), &fk) == 0) {
            k = fk;
        } else {
            k = KARA_SIDE_NONE;
        }
    }
    if (kind) {
        *kind = k;
    }
    if (out && cap > 0 && side[0]) {
        snprintf(out, (size_t)cap, "%s", side);
    }
    return side[0] ? 0 : -1;
}

int karaoke_get_artist(int64_t id, LibArtist *out)
{
    sqlite3_stmt *st = NULL;
    if (!s_db || !out || id <= 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (sqlite3_prepare_v2(kdb(),
                           "SELECT id, name, (SELECT COUNT(*) FROM songs WHERE artist_id=artists.id) "
                           "FROM artists WHERE id=?1;",
                           -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return -1;
    }
    out->id = sqlite3_column_int64(st, 0);
    {
        const unsigned char *s = sqlite3_column_text(st, 1);
        if (s) {
            snprintf(out->name, sizeof(out->name), "%s", (const char *)s);
        }
    }
    out->album_count = sqlite3_column_int(st, 2);
    sqlite3_finalize(st);
    return 0;
}

int karaoke_list_artists(LibArtist *out, int cap)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db || !out || cap <= 0) {
        return 0;
    }
    if (sqlite3_prepare_v2(kdb(),
                           "SELECT id, name, (SELECT COUNT(*) FROM songs WHERE artist_id=artists.id) "
                           "FROM artists ORDER BY name COLLATE NOCASE LIMIT ?1;",
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
        out[n].album_count = sqlite3_column_int(st, 2);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

int karaoke_list_genres(LibArtist *out, int cap)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db || !out || cap <= 0) {
        return 0;
    }
    if (sqlite3_prepare_v2(kdb(),
                           "SELECT genre, COUNT(*) FROM songs GROUP BY genre COLLATE NOCASE "
                           "ORDER BY genre COLLATE NOCASE LIMIT ?1;",
                           -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int(st, 1, cap);
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *s;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].id = n + 1;
        s = sqlite3_column_text(st, 0);
        if (s) {
            snprintf(out[n].name, sizeof(out[n].name), "%s", (const char *)s);
        }
        out[n].album_count = sqlite3_column_int(st, 1);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

static int list_folders_where(const char *where, int64_t parent, int bind, LibFolder *out, int cap)
{
    sqlite3_stmt *st = NULL;
    char sql[640];
    int n = 0;
    snprintf(sql, sizeof(sql),
             "SELECT f.id, f.path, IFNULL((SELECT COUNT(*) FROM songs s WHERE s.folder_id=f.id),0), "
             "IFNULL(f.parent_id,0) FROM folders f %s ORDER BY f.path COLLATE NOCASE LIMIT ?%d;",
             where, bind ? 2 : 1);
    if (sqlite3_prepare_v2(kdb(), sql, -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    if (bind) {
        sqlite3_bind_int64(st, 1, parent);
        sqlite3_bind_int(st, 2, cap);
    } else {
        sqlite3_bind_int(st, 1, cap);
    }
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *s;
        const char *slash;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].id = sqlite3_column_int64(st, 0);
        s = sqlite3_column_text(st, 1);
        if (s) {
            snprintf(out[n].path, sizeof(out[n].path), "%s", (const char *)s);
            slash = strrchr(out[n].path, '/');
            snprintf(out[n].name, sizeof(out[n].name), "%s",
                     (slash && slash[1]) ? slash + 1 : out[n].path);
        }
        out[n].child_count = sqlite3_column_int(st, 2);
        out[n].parent_id = sqlite3_column_int64(st, 3);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

int karaoke_list_folders(int64_t parent_id, LibFolder *out, int cap)
{
    int n;
    if (!s_db || !out || cap <= 0) {
        return 0;
    }
    if (parent_id > 0) {
        return list_folders_where("WHERE f.parent_id=?1", parent_id, 1, out, cap);
    }
    n = list_folders_where(
        "WHERE f.parent_id IN (SELECT id FROM folders WHERE parent_id IS NULL)",
        0, 0, out, cap);
    if (n == 0) {
        n = list_folders_where("WHERE f.parent_id IS NULL", 0, 0, out, cap);
    }
    return n;
}

int karaoke_get_folder(int64_t id, LibFolder *out)
{
    sqlite3_stmt *st = NULL;
    if (!s_db || !out || id <= 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (sqlite3_prepare_v2(kdb(),
                           "SELECT id, path, IFNULL(parent_id,0) FROM folders WHERE id=?1;",
                           -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return -1;
    }
    out->id = sqlite3_column_int64(st, 0);
    {
        const unsigned char *s = sqlite3_column_text(st, 1);
        const char *slash;
        if (s) {
            snprintf(out->path, sizeof(out->path), "%s", (const char *)s);
            slash = strrchr(out->path, '/');
            snprintf(out->name, sizeof(out->name), "%s",
                     (slash && slash[1]) ? slash + 1 : out->path);
        }
    }
    out->parent_id = sqlite3_column_int64(st, 2);
    sqlite3_finalize(st);
    return 0;
}

static int list_songs_sql(const char *sql, int64_t id, const char *text, int has_id, int has_text,
                          LibTrack *out, int cap)
{
    sqlite3_stmt *st = NULL;
    int n = 0, p = 1;
    if (!s_db || !out || cap <= 0) {
        return 0;
    }
    if (sqlite3_prepare_v2(kdb(), sql, -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    if (has_text) {
        sqlite3_bind_text(st, p++, text ? text : "", -1, SQLITE_TRANSIENT);
    }
    if (has_id) {
        sqlite3_bind_int64(st, p++, id);
    }
    sqlite3_bind_int(st, p, cap);
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        fill_song(st, &out[n]);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

int karaoke_folder_direct_tracks(int64_t folder_id, LibTrack *out, int cap)
{
    char sql[640];
    snprintf(sql, sizeof(sql), "%s WHERE s.folder_id=?1 ORDER BY s.title COLLATE NOCASE, s.id LIMIT ?2;",
             k_song_sel);
    return list_songs_sql(sql, folder_id, NULL, 1, 0, out, cap);
}

int karaoke_artist_songs(int64_t artist_id, LibTrack *out, int cap)
{
    char sql[640];
    snprintf(sql, sizeof(sql), "%s WHERE s.artist_id=?1 ORDER BY s.title COLLATE NOCASE, s.id LIMIT ?2;",
             k_song_sel);
    return list_songs_sql(sql, artist_id, NULL, 1, 0, out, cap);
}

int karaoke_genre_songs(const char *genre, LibTrack *out, int cap)
{
    char sql[640];
    snprintf(sql, sizeof(sql),
             "%s WHERE s.genre=?1 COLLATE NOCASE ORDER BY s.title COLLATE NOCASE, s.id LIMIT ?2;",
             k_song_sel);
    return list_songs_sql(sql, 0, genre, 0, 1, out, cap);
}

int karaoke_list_songs_after(const char *title, int64_t id, LibTrack *out, int cap)
{
    sqlite3_stmt *st = NULL;
    const char *t = title ? title : "";
    char sql[800];
    int n = 0;
    if (!s_db || !out || cap <= 0) {
        return 0;
    }
    if (!t[0] && id == 0) {
        snprintf(sql, sizeof(sql), "%s ORDER BY s.title COLLATE NOCASE, s.id LIMIT ?1;", k_song_sel);
        if (sqlite3_prepare_v2(kdb(), sql, -1, &st, NULL) != SQLITE_OK) {
            return 0;
        }
        sqlite3_bind_int(st, 1, cap);
    } else {
        snprintf(sql, sizeof(sql),
                 "%s WHERE s.title > ?1 COLLATE NOCASE "
                 "   OR (s.title = ?1 COLLATE NOCASE AND s.id > ?2) "
                 "ORDER BY s.title COLLATE NOCASE, s.id LIMIT ?3;",
                 k_song_sel);
        if (sqlite3_prepare_v2(kdb(), sql, -1, &st, NULL) != SQLITE_OK) {
            return 0;
        }
        sqlite3_bind_text(st, 1, t, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, id);
        sqlite3_bind_int(st, 3, cap);
    }
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        fill_song(st, &out[n]);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

int karaoke_list_songs_before(const char *title, int64_t id, LibTrack *out, int cap)
{
    sqlite3_stmt *st = NULL;
    const char *t = title ? title : "";
    char sql[800];
    int n = 0, i;
    if (!s_db || !out || cap <= 0) {
        return 0;
    }
    if (!t[0] && id == 0) {
        snprintf(sql, sizeof(sql),
                 "%s ORDER BY s.title COLLATE NOCASE DESC, s.id DESC LIMIT ?1;", k_song_sel);
        if (sqlite3_prepare_v2(kdb(), sql, -1, &st, NULL) != SQLITE_OK) {
            return 0;
        }
        sqlite3_bind_int(st, 1, cap);
    } else {
        snprintf(sql, sizeof(sql),
                 "%s WHERE s.title < ?1 COLLATE NOCASE "
                 "   OR (s.title = ?1 COLLATE NOCASE AND s.id < ?2) "
                 "ORDER BY s.title COLLATE NOCASE DESC, s.id DESC LIMIT ?3;",
                 k_song_sel);
        if (sqlite3_prepare_v2(kdb(), sql, -1, &st, NULL) != SQLITE_OK) {
            return 0;
        }
        sqlite3_bind_text(st, 1, t, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, id);
        sqlite3_bind_int(st, 3, cap);
    }
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        fill_song(st, &out[n]);
        n++;
    }
    sqlite3_finalize(st);
    for (i = 0; i < n / 2; i++) {
        LibTrack tmp = out[i];
        out[i] = out[n - 1 - i];
        out[n - 1 - i] = tmp;
    }
    return n;
}

int karaoke_list_songs_from_letter(uint32_t letter, LibTrack *out, int cap)
{
    sqlite3_stmt *st = NULL;
    char prefix[8];
    char sql[800];
    int n = 0;
    if (!s_db || !out || cap <= 0) {
        return 0;
    }
    if (letter == 0 || letter == '#') {
        return karaoke_list_songs_after("", 0, out, cap);
    }
    if (letter >= 'a' && letter <= 'z') {
        letter = letter - 'a' + 'A';
    }
    prefix[0] = (char)letter;
    prefix[1] = '\0';
    snprintf(sql, sizeof(sql),
             "%s WHERE s.title >= ?1 COLLATE NOCASE "
             "ORDER BY s.title COLLATE NOCASE, s.id LIMIT ?2;",
             k_song_sel);
    if (sqlite3_prepare_v2(kdb(), sql, -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, prefix, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, cap);
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        fill_song(st, &out[n]);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

int karaoke_search(const char *query, LibTrack *out, int cap)
{
    sqlite3_stmt *st = NULL;
    char like[VIBE_NAME_MAX * 2 + 8];
    char sql[800];
    size_t j;
    const char *q;
    int n = 0;
    if (!s_db || !out || cap <= 0 || !query || !query[0]) {
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
             "%s WHERE s.title LIKE ?1 ESCAPE '\\' OR IFNULL(a.name,'') LIKE ?1 ESCAPE '\\' "
             "OR s.genre LIKE ?1 ESCAPE '\\' "
             "ORDER BY s.title COLLATE NOCASE LIMIT ?2;",
             k_song_sel);
    if (sqlite3_prepare_v2(kdb(), sql, -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, like, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, cap);
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        fill_song(st, &out[n]);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

int karaoke_resume_song(LibTrack *out, int *position_ms)
{
    char buf[32];
    int64_t id;
    if (!out) {
        return -1;
    }
    if (karaoke_session_get("track_id", buf, (int)sizeof(buf)) != 0 || !buf[0]) {
        return -1;
    }
    id = (int64_t)atoll(buf);
    if (karaoke_get_song(id, out) != 0) {
        return -1;
    }
    if (position_ms) {
        *position_ms = karaoke_session_get_int("position_ms", 0);
    }
    return 0;
}

int karaoke_queue_len(void)
{
    return s_qlen;
}

int karaoke_queue_index(void)
{
    return s_qidx;
}

void karaoke_queue_set_index(int i)
{
    if (i < 0) {
        i = 0;
    }
    if (s_qlen > 0 && i >= s_qlen) {
        i = s_qlen - 1;
    }
    s_qidx = i;
}

int karaoke_queue_get(int idx, LibTrack *out)
{
    if (idx < 0 || idx >= s_qlen || !out) {
        return -1;
    }
    return karaoke_get_song(s_queue[idx], out);
}

int karaoke_queue_add(int64_t song_id)
{
    if (song_id <= 0) {
        return -1;
    }
    if (queue_ensure(s_qlen + 1) != 0) {
        return -1;
    }
    s_queue[s_qlen++] = song_id;
    queue_mutated();
    return 0;
}

int karaoke_queue_insert_next(int64_t song_id)
{
    int at;
    if (song_id <= 0) {
        return -1;
    }
    if (queue_ensure(s_qlen + 1) != 0) {
        return -1;
    }
    at = s_qidx + 1;
    if (at < 0) {
        at = 0;
    }
    if (at > s_qlen) {
        at = s_qlen;
    }
    memmove(&s_queue[at + 1], &s_queue[at], (size_t)(s_qlen - at) * sizeof(int64_t));
    s_queue[at] = song_id;
    s_qlen++;
    queue_mutated();
    return 0;
}

static int add_sql(const char *sql, int64_t id, const char *text)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(kdb(), sql, -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    if (text) {
        sqlite3_bind_text(st, 1, text, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_int64(st, 1, id);
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (queue_ensure(s_qlen + 1) != 0) {
            break;
        }
        s_queue[s_qlen++] = sqlite3_column_int64(st, 0);
        n++;
    }
    sqlite3_finalize(st);
    if (n > 0) {
        queue_mutated();
    }
    return n;
}

int karaoke_queue_add_artist(int64_t artist_id)
{
    return add_sql("SELECT id FROM songs WHERE artist_id=?1 ORDER BY title COLLATE NOCASE, id;",
                   artist_id, NULL);
}

int karaoke_queue_add_genre(const char *genre)
{
    return add_sql("SELECT id FROM songs WHERE genre=?1 COLLATE NOCASE ORDER BY title COLLATE NOCASE, id;",
                   0, genre ? genre : "");
}

int karaoke_queue_add_folder(int64_t folder_id)
{
    return add_sql(
        "SELECT s.id FROM songs s JOIN folders f ON f.id=?1 "
        "WHERE s.folder_id=f.id OR s.path = f.path "
        "OR substr(s.path, 1, length(f.path)+1) = f.path || '/' "
        "ORDER BY s.path COLLATE NOCASE;",
        folder_id, NULL);
}

int karaoke_queue_add_all(void)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db) {
        return 0;
    }
    if (sqlite3_prepare_v2(kdb(), "SELECT id FROM songs ORDER BY title COLLATE NOCASE, id;",
                           -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (queue_ensure(s_qlen + 1) != 0) {
            break;
        }
        s_queue[s_qlen++] = sqlite3_column_int64(st, 0);
        n++;
    }
    sqlite3_finalize(st);
    if (n > 0) {
        queue_mutated();
    }
    return n;
}

int karaoke_queue_has(int64_t song_id)
{
    if (song_id <= 0 || s_qlen <= 0) {
        return 0;
    }
    if (!s_qhas_ready) {
        qhas_rebuild();
    }
    return bsearch(&song_id, s_qhas, (size_t)s_qhas_n, sizeof(s_qhas[0]), i64_cmp) != NULL;
}

int karaoke_queue_remove_song(int64_t song_id)
{
    int i, w = 0, n = 0;
    for (i = 0; i < s_qlen; i++) {
        if (s_queue[i] == song_id) {
            n++;
            if (s_qidx > w) {
                s_qidx--;
            }
            continue;
        }
        s_queue[w++] = s_queue[i];
    }
    s_qlen = w;
    if (s_qidx >= s_qlen) {
        s_qidx = s_qlen > 0 ? s_qlen - 1 : 0;
    }
    if (n) {
        queue_mutated();
    }
    return n;
}

static int play_from_sql(const char *sql, int64_t id, const char *text, int64_t start_id)
{
    sqlite3_stmt *st = NULL;
    int n = 0, started = 0;
    karaoke_queue_clear();
    if (sqlite3_prepare_v2(kdb(), sql, -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    if (text) {
        sqlite3_bind_text(st, 1, text, -1, SQLITE_TRANSIENT);
    } else if (id > 0) {
        sqlite3_bind_int64(st, 1, id);
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        int64_t sid = sqlite3_column_int64(st, 0);
        if (queue_ensure(s_qlen + 1) != 0) {
            break;
        }
        if (!started && start_id > 0 && sid == start_id) {
            s_qidx = s_qlen;
            started = 1;
        }
        s_queue[s_qlen++] = sid;
        n++;
    }
    sqlite3_finalize(st);
    if (n <= 0) {
        return -1;
    }
    if (!started) {
        s_qidx = 0;
    }
    queue_mutated();
    return 0;
}

int karaoke_queue_play_artist_from(int64_t artist_id, int64_t start_id)
{
    return play_from_sql("SELECT id FROM songs WHERE artist_id=?1 ORDER BY title COLLATE NOCASE, id;",
                         artist_id, NULL, start_id);
}

int karaoke_queue_play_genre_from(const char *genre, int64_t start_id)
{
    return play_from_sql(
        "SELECT id FROM songs WHERE genre=?1 COLLATE NOCASE ORDER BY title COLLATE NOCASE, id;",
        0, genre ? genre : "", start_id);
}

int karaoke_queue_play_folder_from(int64_t folder_id, int64_t start_id)
{
    return play_from_sql(
        "SELECT s.id FROM songs s JOIN folders f ON f.id=?1 "
        "WHERE s.folder_id=f.id OR s.path = f.path "
        "OR substr(s.path, 1, length(f.path)+1) = f.path || '/' "
        "ORDER BY s.path COLLATE NOCASE;",
        folder_id, NULL, start_id);
}

int karaoke_queue_play_song(int64_t song_id)
{
    karaoke_queue_clear();
    if (karaoke_queue_add(song_id) != 0) {
        return -1;
    }
    s_qidx = 0;
    return 0;
}

int karaoke_queue_remove(int idx)
{
    int i;
    if (idx < 0 || idx >= s_qlen) {
        return -1;
    }
    for (i = idx; i < s_qlen - 1; i++) {
        s_queue[i] = s_queue[i + 1];
    }
    s_qlen--;
    if (s_qidx > idx) {
        s_qidx--;
    }
    if (s_qidx >= s_qlen) {
        s_qidx = s_qlen > 0 ? s_qlen - 1 : 0;
    }
    queue_mutated();
    return 0;
}

void karaoke_queue_clear(void)
{
    s_qlen = 0;
    s_qidx = 0;
    queue_mutated();
}

int karaoke_queue_save(void)
{
    sqlite3_stmt *st = NULL;
    int i;
    if (!s_db) {
        return -1;
    }
    sqlite3_exec(kdb(), "BEGIN;", NULL, NULL, NULL);
    sqlite3_exec(kdb(), "DELETE FROM last_queue;", NULL, NULL, NULL);
    if (sqlite3_prepare_v2(kdb(), "INSERT INTO last_queue(pos, song_id) VALUES(?1,?2);",
                           -1, &st, NULL) != SQLITE_OK) {
        sqlite3_exec(kdb(), "COMMIT;", NULL, NULL, NULL);
        return -1;
    }
    for (i = 0; i < s_qlen; i++) {
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_int(st, 1, i);
        sqlite3_bind_int64(st, 2, s_queue[i]);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    sqlite3_exec(kdb(), "COMMIT;", NULL, NULL, NULL);
    return 0;
}

int karaoke_song_fav_has(int64_t song_id)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db || song_id <= 0) {
        return 0;
    }
    if (sqlite3_prepare_v2(kdb(), "SELECT 1 FROM song_fav WHERE song_id=?1;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int64(st, 1, song_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = 1;
    }
    sqlite3_finalize(st);
    return n;
}

int karaoke_song_fav_toggle(int64_t song_id)
{
    sqlite3_stmt *st = NULL;
    if (!s_db || song_id <= 0) {
        return -1;
    }
    if (karaoke_song_fav_has(song_id)) {
        if (sqlite3_prepare_v2(kdb(), "DELETE FROM song_fav WHERE song_id=?1;", -1, &st, NULL) != SQLITE_OK) {
            return -1;
        }
        sqlite3_bind_int64(st, 1, song_id);
        sqlite3_step(st);
        sqlite3_finalize(st);
        return 0;
    }
    if (sqlite3_prepare_v2(kdb(), "INSERT INTO song_fav(song_id) VALUES(?1);", -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, song_id);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    return 1;
}

int karaoke_song_fav_count(void)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!s_db) {
        return 0;
    }
    if (sqlite3_prepare_v2(kdb(), "SELECT COUNT(*) FROM song_fav;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

int karaoke_song_fav_list(LibTrack *out, int cap)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    char sql[768];
    if (!s_db || !out || cap <= 0) {
        return 0;
    }
    snprintf(sql, sizeof(sql),
             "%s JOIN song_fav f ON f.song_id=s.id "
             "ORDER BY IFNULL(a.name,'') COLLATE NOCASE, s.title COLLATE NOCASE, s.id "
             "LIMIT ?1;",
             k_song_sel);
    if (sqlite3_prepare_v2(kdb(), sql, -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int(st, 1, cap);
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        fill_song(st, &out[n]);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

int karaoke_queue_load(void)
{
    sqlite3_stmt *st = NULL;
    karaoke_queue_clear();
    if (!s_db) {
        return -1;
    }
    if (sqlite3_prepare_v2(kdb(), "SELECT song_id FROM last_queue ORDER BY pos;",
                           -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (queue_ensure(s_qlen + 1) != 0) {
            break;
        }
        s_queue[s_qlen++] = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    queue_mutated();
    return 0;
}

void karaoke_session_set(const char *key, const char *value)
{
    sqlite3_stmt *st = NULL;
    if (!s_db || !key) {
        return;
    }
    if (sqlite3_prepare_v2(kdb(),
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

int karaoke_session_get(const char *key, char *out, int cap)
{
    sqlite3_stmt *st = NULL;
    int r = -1;
    if (!s_db || !key || !out || cap <= 0) {
        return -1;
    }
    out[0] = '\0';
    if (sqlite3_prepare_v2(kdb(), "SELECT value FROM session WHERE key=?1;", -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *s = sqlite3_column_text(st, 0);
        if (s) {
            snprintf(out, (size_t)cap, "%s", (const char *)s);
            r = 0;
        }
    }
    sqlite3_finalize(st);
    return r;
}

void karaoke_session_set_int(const char *key, int v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", v);
    karaoke_session_set(key, buf);
}

int karaoke_session_get_int(const char *key, int fallback)
{
    char buf[32];
    if (karaoke_session_get(key, buf, (int)sizeof(buf)) != 0 || !buf[0]) {
        return fallback;
    }
    return atoi(buf);
}
