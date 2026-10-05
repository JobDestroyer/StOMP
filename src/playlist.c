/* SPDX-License-Identifier: LGPL-2.1-only
 * Copyright (C) 2026 JobDestroyer
 */

#include "playlist.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void trim(char *s)
{
    char *e, *p;
    if (!s) {
        return;
    }
    p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
        *--e = '\0';
    }
}

static void strip_quotes(char *s)
{
    size_t n;
    trim(s);
    n = strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        s[n - 1] = '\0';
        memmove(s, s + 1, n - 1);
    }
}

static int hexn(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void url_decode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && hexn(r[1]) >= 0 && hexn(r[2]) >= 0) {
            *w++ = (char)((hexn(r[1]) << 4) | hexn(r[2]));
            r += 3;
        } else if (*r == '+') {
            *w++ = ' ';
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static void xml_decode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '&') {
            if (strncmp(r, "&amp;", 5) == 0) {
                *w++ = '&';
                r += 5;
                continue;
            }
            if (strncmp(r, "&lt;", 4) == 0) {
                *w++ = '<';
                r += 4;
                continue;
            }
            if (strncmp(r, "&gt;", 4) == 0) {
                *w++ = '>';
                r += 4;
                continue;
            }
            if (strncmp(r, "&quot;", 6) == 0) {
                *w++ = '"';
                r += 6;
                continue;
            }
            if (strncmp(r, "&apos;", 6) == 0) {
                *w++ = '\'';
                r += 6;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

static void slash_fix(char *s)
{
    for (; s && *s; s++) {
        if (*s == '\\') {
            *s = '/';
        }
    }
}

static int is_url(const char *s)
{
    return strncasecmp(s, "http://", 7) == 0 || strncasecmp(s, "https://", 8) == 0 ||
           strncasecmp(s, "rtsp://", 7) == 0 || strncasecmp(s, "mms://", 6) == 0 ||
           strncasecmp(s, "rtp://", 6) == 0;
}

static void strip_file_url(char *s)
{
    if (strncasecmp(s, "file://localhost/", 17) == 0) {
        memmove(s, s + 16, strlen(s + 16) + 1);
    } else if (strncasecmp(s, "file:///", 8) == 0) {
        memmove(s, s + 7, strlen(s + 7) + 1);
    } else if (strncasecmp(s, "file://", 7) == 0) {
        memmove(s, s + 7, strlen(s + 7) + 1);
    }
    url_decode(s);
}

static void split_artist_title(const char *in, char *artist, int an, char *title, int tn)
{
    const char *p;
    if (!in) {
        return;
    }
    p = strstr(in, " - ");
    if (p && p != in) {
        size_t al = (size_t)(p - in);
        if (al >= (size_t)an) {
            al = (size_t)an - 1;
        }
        memcpy(artist, in, al);
        artist[al] = '\0';
        snprintf(title, (size_t)tn, "%s", p + 3);
        trim(artist);
        trim(title);
        return;
    }
    snprintf(title, (size_t)tn, "%s", in);
    trim(title);
}

static void add_entry(PlEntry *out, int cap, int *n, const char *path, const char *title, const char *artist)
{
    PlEntry *e;
    if (!out || !n || *n >= cap || !path || !path[0]) {
        return;
    }
    if (is_url(path)) {
        return;
    }
    e = &out[*n];
    memset(e, 0, sizeof(*e));
    snprintf(e->path, sizeof(e->path), "%s", path);
    strip_quotes(e->path);
    strip_file_url(e->path);
    slash_fix(e->path);
    trim(e->path);
    if (!e->path[0] || is_url(e->path)) {
        return;
    }
    if (title) {
        snprintf(e->title, sizeof(e->title), "%s", title);
        trim(e->title);
    }
    if (artist) {
        snprintf(e->artist, sizeof(e->artist), "%s", artist);
        trim(e->artist);
    }
    (*n)++;
}

static int parse_m3u(FILE *fp, PlEntry *out, int cap)
{
    char line[2048];
    char title[VIBE_NAME_MAX];
    char artist[VIBE_NAME_MAX];
    int n = 0;
    int first = 1;
    title[0] = '\0';
    artist[0] = '\0';
    while (fgets(line, (int)sizeof(line), fp)) {
        char *s = line;
        if (first) {
            first = 0;
            if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) {
                s += 3;
            }
        }
        trim(s);
        if (!s[0]) {
            continue;
        }
        if (s[0] == '#') {
            if (strncasecmp(s, "#EXTINF:", 8) == 0) {
                char *comma = strchr(s, ',');
                title[0] = '\0';
                artist[0] = '\0';
                if (comma) {
                    split_artist_title(comma + 1, artist, (int)sizeof(artist), title, (int)sizeof(title));
                }
            }
            continue;
        }
        add_entry(out, cap, &n, s, title, artist);
        title[0] = '\0';
        artist[0] = '\0';
    }
    return n;
}

static int parse_pls(FILE *fp, PlEntry *out, int cap)
{
    char line[2048];
    char (*files)[VIBE_PATH_MAX];
    char (*titles)[VIBE_NAME_MAX];
    unsigned char *used;
    int i, n = 0, maxn = 0;
    files = calloc(512, sizeof(*files));
    titles = calloc(512, sizeof(*titles));
    used = calloc(512, 1);
    if (!files || !titles || !used) {
        free(files);
        free(titles);
        free(used);
        return 0;
    }
    while (fgets(line, (int)sizeof(line), fp)) {
        char *eq;
        int idx = 0;
        trim(line);
        if (!line[0] || line[0] == '[') {
            continue;
        }
        eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq++ = '\0';
        trim(line);
        trim(eq);
        if (strncasecmp(line, "File", 4) == 0 && isdigit((unsigned char)line[4])) {
            idx = atoi(line + 4);
            if (idx >= 1 && idx <= 512) {
                snprintf(files[idx - 1], sizeof(files[0]), "%s", eq);
                used[idx - 1] = 1;
                if (idx > maxn) {
                    maxn = idx;
                }
            }
        } else if (strncasecmp(line, "Title", 5) == 0 && isdigit((unsigned char)line[5])) {
            idx = atoi(line + 5);
            if (idx >= 1 && idx <= 512) {
                snprintf(titles[idx - 1], sizeof(titles[0]), "%s", eq);
                if (idx > maxn) {
                    maxn = idx;
                }
            }
        }
    }
    for (i = 0; i < maxn; i++) {
        char artist[VIBE_NAME_MAX];
        char title[VIBE_NAME_MAX];
        if (!used[i] || !files[i][0]) {
            continue;
        }
        artist[0] = '\0';
        title[0] = '\0';
        if (titles[i][0]) {
            split_artist_title(titles[i], artist, (int)sizeof(artist), title, (int)sizeof(title));
        }
        add_entry(out, cap, &n, files[i], title, artist);
    }
    free(files);
    free(titles);
    free(used);
    return n;
}

static int parse_cue(FILE *fp, PlEntry *out, int cap)
{
    char line[2048];
    char file[VIBE_PATH_MAX];
    char title[VIBE_NAME_MAX];
    char artist[VIBE_NAME_MAX];
    int n = 0;
    file[0] = '\0';
    title[0] = '\0';
    artist[0] = '\0';
    while (fgets(line, (int)sizeof(line), fp)) {
        char *s = line;
        trim(s);
        if (strncasecmp(s, "FILE ", 5) == 0) {
            char tmp[VIBE_PATH_MAX];
            char *p, *q;
            if (file[0]) {
                add_entry(out, cap, &n, file, title, artist);
            }
            snprintf(tmp, sizeof(tmp), "%s", s + 5);
            trim(tmp);
            p = tmp;
            if (*p == '"') {
                q = strchr(p + 1, '"');
                if (q) {
                    *q = '\0';
                    snprintf(file, sizeof(file), "%s", p + 1);
                }
            } else {
                q = strrchr(p, ' ');
                if (q) {
                    *q = '\0';
                }
                snprintf(file, sizeof(file), "%s", p);
            }
            title[0] = '\0';
        } else if (strncasecmp(s, "TITLE ", 6) == 0) {
            snprintf(title, sizeof(title), "%s", s + 6);
            strip_quotes(title);
        } else if (strncasecmp(s, "PERFORMER ", 10) == 0) {
            snprintf(artist, sizeof(artist), "%s", s + 10);
            strip_quotes(artist);
        }
    }
    if (file[0]) {
        add_entry(out, cap, &n, file, title, artist);
    }
    return n;
}

static char *read_whole(const char *path, size_t *out_n)
{
    FILE *fp;
    char *buf;
    long sz;
    fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    sz = ftell(fp);
    if (sz < 0 || sz > 2 * 1024 * 1024) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    buf[sz] = '\0';
    fclose(fp);
    if (out_n) {
        *out_n = (size_t)sz;
    }
    return buf;
}

static int attr_value(const char *s, const char *end, const char *attr, char *out, int n)
{
    size_t al = strlen(attr);
    const char *p = s;
    while (p && p < end) {
        const char *hit = p;
        while (hit < end && strncasecmp(hit, attr, al) != 0) {
            hit++;
        }
        if (hit >= end) {
            return -1;
        }
        p = hit + al;
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }
        if (p < end && *p == '=') {
            char q;
            p++;
            while (p < end && (*p == ' ' || *p == '\t')) {
                p++;
            }
            if (p >= end) {
                return -1;
            }
            q = *p;
            if (q == '"' || q == '\'') {
                const char *e;
                size_t L;
                p++;
                e = p;
                while (e < end && *e != q) {
                    e++;
                }
                L = (size_t)(e - p);
                if (L >= (size_t)n) {
                    L = (size_t)n - 1;
                }
                memcpy(out, p, L);
                out[L] = '\0';
                xml_decode(out);
                return 0;
            }
        }
        p = hit + 1;
    }
    return -1;
}

static int tag_text(const char *s, const char *end, const char *tag, char *out, int n)
{
    char open[64], close[64];
    const char *a, *b;
    size_t L;
    snprintf(open, sizeof(open), "<%s", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    a = s;
    while (a < end) {
        const char *t = a;
        while (t < end && strncasecmp(t, open, strlen(open)) != 0) {
            t++;
        }
        if (t >= end) {
            return -1;
        }
        a = strchr(t, '>');
        if (!a || a >= end) {
            return -1;
        }
        a++;
        b = a;
        while (b < end && strncasecmp(b, close, strlen(close)) != 0) {
            b++;
        }
        if (b >= end) {
            return -1;
        }
        L = (size_t)(b - a);
        if (L >= (size_t)n) {
            L = (size_t)n - 1;
        }
        memcpy(out, a, L);
        out[L] = '\0';
        xml_decode(out);
        trim(out);
        return 0;
    }
    return -1;
}

static int xml_tag_open(const char *p, const char *end, const char *name)
{
    size_t n;
    unsigned char c;
    if (!p || !name || p >= end || *p != '<') {
        return 0;
    }
    n = strlen(name);
    if ((size_t)(end - p) < n + 2) {
        return 0;
    }
    if (strncasecmp(p + 1, name, n) != 0) {
        return 0;
    }
    c = (unsigned char)p[1 + n];
    return c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int parse_xspf(const char *buf, size_t sz, PlEntry *out, int cap)
{
    const char *s = buf, *end = buf + sz;
    int n = 0;
    while (s < end) {
        const char *tr, *te;
        char loc[VIBE_PATH_MAX], title[VIBE_NAME_MAX], artist[VIBE_NAME_MAX];
        tr = s;
        while (tr < end && !xml_tag_open(tr, end, "track")) {
            tr++;
        }
        if (tr >= end) {
            break;
        }
        te = tr + 6;
        while (te < end && strncasecmp(te, "</track>", 8) != 0) {
            te++;
        }
        if (te >= end) {
            break;
        }
        loc[0] = title[0] = artist[0] = '\0';
        tag_text(tr, te, "location", loc, (int)sizeof(loc));
        tag_text(tr, te, "title", title, (int)sizeof(title));
        tag_text(tr, te, "creator", artist, (int)sizeof(artist));
        if (loc[0]) {
            add_entry(out, cap, &n, loc, title, artist);
        }
        s = te + 8;
    }
    return n;
}

static int parse_wpl(const char *buf, size_t sz, PlEntry *out, int cap)
{
    const char *s = buf, *end = buf + sz;
    int n = 0;
    while (s < end) {
        const char *m = s;
        char src[VIBE_PATH_MAX];
        while (m < end && strncasecmp(m, "<media", 6) != 0) {
            m++;
        }
        if (m >= end) {
            break;
        }
        s = m + 6;
        src[0] = '\0';
        {
            const char *gt = m;
            while (gt < end && *gt != '>') {
                gt++;
            }
            if (attr_value(m, gt, "src", src, (int)sizeof(src)) == 0 && src[0]) {
                add_entry(out, cap, &n, src, NULL, NULL);
            }
        }
    }
    return n;
}

static int parse_asx(const char *buf, size_t sz, PlEntry *out, int cap)
{
    const char *s = buf, *end = buf + sz;
    int n = 0;
    while (s < end) {
        const char *e0, *e1, *r;
        char href[VIBE_PATH_MAX], title[VIBE_NAME_MAX];
        e0 = s;
        while (e0 < end && strncasecmp(e0, "<entry", 6) != 0) {
            e0++;
        }
        if (e0 >= end) {
            /* refs without entry wrappers */
            char href2[VIBE_PATH_MAX];
            const char *m = s;
            while (m < end) {
                while (m < end && strncasecmp(m, "<ref", 4) != 0) {
                    m++;
                }
                if (m >= end) {
                    break;
                }
                href2[0] = '\0';
                if (attr_value(m, end, "href", href2, (int)sizeof(href2)) == 0 && href2[0]) {
                    add_entry(out, cap, &n, href2, NULL, NULL);
                }
                m += 4;
            }
            break;
        }
        e1 = e0 + 6;
        while (e1 < end && strncasecmp(e1, "</entry>", 8) != 0) {
            e1++;
        }
        href[0] = title[0] = '\0';
        tag_text(e0, e1 < end ? e1 : end, "title", title, (int)sizeof(title));
        r = e0;
        while (r < e1) {
            while (r < e1 && strncasecmp(r, "<ref", 4) != 0) {
                r++;
            }
            if (r >= e1) {
                break;
            }
            href[0] = '\0';
            if (attr_value(r, e1, "href", href, (int)sizeof(href)) == 0 && href[0]) {
                add_entry(out, cap, &n, href, title, NULL);
            }
            r += 4;
        }
        s = (e1 < end) ? e1 + 8 : end;
    }
    return n;
}

int playlist_is_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) {
        return 0;
    }
    return strcasecmp(dot, ".m3u") == 0 || strcasecmp(dot, ".m3u8") == 0 ||
           strcasecmp(dot, ".pls") == 0 || strcasecmp(dot, ".pl") == 0 ||
           strcasecmp(dot, ".xspf") == 0 || strcasecmp(dot, ".wpl") == 0 ||
           strcasecmp(dot, ".zpl") == 0 || strcasecmp(dot, ".asx") == 0 ||
           strcasecmp(dot, ".wax") == 0 || strcasecmp(dot, ".wmx") == 0 ||
           strcasecmp(dot, ".cue") == 0;
}

int playlist_parse(const char *path, PlEntry *out, int cap)
{
    const char *dot;
    FILE *fp;
    int n = 0;
    if (!path || !out || cap <= 0) {
        return 0;
    }
    dot = strrchr(path, '.');
    if (!dot) {
        return 0;
    }
    if (strcasecmp(dot, ".xspf") == 0) {
        size_t sz = 0;
        char *buf = read_whole(path, &sz);
        if (!buf) {
            return 0;
        }
        n = parse_xspf(buf, sz, out, cap);
        free(buf);
        return n;
    }
    if (strcasecmp(dot, ".wpl") == 0 || strcasecmp(dot, ".zpl") == 0) {
        size_t sz = 0;
        char *buf = read_whole(path, &sz);
        if (!buf) {
            return 0;
        }
        n = parse_wpl(buf, sz, out, cap);
        free(buf);
        return n;
    }
    if (strcasecmp(dot, ".asx") == 0 || strcasecmp(dot, ".wax") == 0 || strcasecmp(dot, ".wmx") == 0) {
        size_t sz = 0;
        char *buf = read_whole(path, &sz);
        if (!buf) {
            return 0;
        }
        n = parse_asx(buf, sz, out, cap);
        free(buf);
        return n;
    }
    fp = fopen(path, "r");
    if (!fp) {
        return 0;
    }
    if (strcasecmp(dot, ".pls") == 0) {
        n = parse_pls(fp, out, cap);
    } else if (strcasecmp(dot, ".cue") == 0) {
        n = parse_cue(fp, out, cap);
    } else if (strcasecmp(dot, ".pl") == 0) {
        n = parse_pls(fp, out, cap);
        if (n == 0) {
            rewind(fp);
            n = parse_m3u(fp, out, cap);
        }
    } else {
        n = parse_m3u(fp, out, cap);
    }
    fclose(fp);
    return n;
}
