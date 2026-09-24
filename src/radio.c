#include "radio.h"

#include <libavformat/avio.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define RADIO_SOMA_MAX 128
#define RADIO_COUNTRY_MAX 320
#define RADIO_STATION_MAX 400

static RadioItem s_soma[RADIO_SOMA_MAX];
static int s_nsoma;
static RadioCountry s_cty[RADIO_COUNTRY_MAX];
static int s_ncty;
static RadioItem s_st[RADIO_STATION_MAX];
static int s_nst;
static int s_net_inited;

static const char *k_rb_hosts[] = {
    "https://de1.api.radio-browser.info",
    "https://de2.api.radio-browser.info",
};

void radio_init(void)
{
    if (!s_net_inited) {
        avformat_network_init();
        s_net_inited = 1;
    }
}

static int http_get(const char *url, char **out)
{
    AVIOContext *io = NULL;
    AVDictionary *opts = NULL;
    char *buf;
    int cap = 256 * 1024;
    int n = 0, err, r;

    if (!url || !out) {
        return -1;
    }
    *out = NULL;
    radio_init();
    av_dict_set(&opts, "user_agent", "StOMP/1.0", 0);
    av_dict_set(&opts, "timeout", "15000000", 0);
    av_dict_set(&opts, "rw_timeout", "15000000", 0);
    av_dict_set(&opts, "reconnect", "1", 0);
    err = avio_open2(&io, url, AVIO_FLAG_READ, NULL, &opts);
    av_dict_free(&opts);
    if (err < 0) {
        return -1;
    }
    buf = (char *)malloc((size_t)cap);
    if (!buf) {
        avio_closep(&io);
        return -1;
    }
    for (;;) {
        if (cap - n < 8192) {
            char *nb;
            int ncap = cap * 2;
            if (ncap > 4 * 1024 * 1024) {
                break;
            }
            nb = (char *)realloc(buf, (size_t)ncap);
            if (!nb) {
                break;
            }
            buf = nb;
            cap = ncap;
        }
        r = avio_read(io, (unsigned char *)buf + n, cap - n - 1);
        if (r <= 0) {
            break;
        }
        n += r;
    }
    buf[n] = '\0';
    avio_closep(&io);
    *out = buf;
    return n;
}

static int js_unescape(const char *in, int inlen, char *out, int outn)
{
    int i = 0, o = 0;
    if (!in || !out || outn < 1) {
        return -1;
    }
    while (i < inlen && o < outn - 1) {
        if (in[i] == '\\' && i + 1 < inlen) {
            char c = in[i + 1];
            if (c == '"' || c == '\\' || c == '/') {
                out[o++] = c;
                i += 2;
            } else if (c == 'n' || c == 't' || c == 'r') {
                out[o++] = (c == 'n') ? '\n' : ((c == 't') ? '\t' : '\r');
                i += 2;
            } else if (c == 'u' && i + 5 < inlen) {
                i += 6;
            } else {
                i += 2;
            }
        } else {
            out[o++] = in[i++];
        }
    }
    out[o] = '\0';
    return 0;
}

static const char *js_skip_ws(const char *s)
{
    while (s && *s && isspace((unsigned char)*s)) {
        s++;
    }
    return s;
}

static const char *js_skip_string(const char *s)
{
    if (!s || *s != '"') {
        return s;
    }
    s++;
    while (*s) {
        if (*s == '\\' && s[1]) {
            s += 2;
            continue;
        }
        if (*s == '"') {
            return s + 1;
        }
        s++;
    }
    return s;
}

static const char *js_skip_value(const char *s);

static const char *js_skip_object(const char *s)
{
    int depth;
    s = js_skip_ws(s);
    if (!s || *s != '{') {
        return s;
    }
    depth = 1;
    s++;
    while (*s && depth > 0) {
        if (*s == '"') {
            s = js_skip_string(s);
            continue;
        }
        if (*s == '{') {
            depth++;
        } else if (*s == '}') {
            depth--;
        }
        s++;
    }
    return s;
}

static const char *js_skip_array(const char *s)
{
    int depth;
    s = js_skip_ws(s);
    if (!s || *s != '[') {
        return s;
    }
    depth = 1;
    s++;
    while (*s && depth > 0) {
        if (*s == '"') {
            s = js_skip_string(s);
            continue;
        }
        if (*s == '[') {
            depth++;
        } else if (*s == ']') {
            depth--;
        }
        s++;
    }
    return s;
}

static const char *js_skip_value(const char *s)
{
    s = js_skip_ws(s);
    if (!s || !*s) {
        return s;
    }
    if (*s == '"') {
        return js_skip_string(s);
    }
    if (*s == '{') {
        return js_skip_object(s);
    }
    if (*s == '[') {
        return js_skip_array(s);
    }
    while (*s && *s != ',' && *s != '}' && *s != ']' && !isspace((unsigned char)*s)) {
        s++;
    }
    return s;
}

static int js_streq_key(const char *s, const char *key)
{
    size_t n;
    if (!s || *s != '"') {
        return 0;
    }
    s++;
    n = strlen(key);
    if (strncmp(s, key, n) != 0) {
        return 0;
    }
    return s[n] == '"';
}

static int js_obj_str(const char *obj, const char *end, const char *key, char *out, int outn)
{
    const char *s = js_skip_ws(obj);
    if (!s || *s != '{') {
        return -1;
    }
    s++;
    while (s && s < end && *s && *s != '}') {
        s = js_skip_ws(s);
        if (*s != '"') {
            break;
        }
        if (js_streq_key(s, key)) {
            s = js_skip_string(s);
            s = js_skip_ws(s);
            if (*s != ':') {
                return -1;
            }
            s = js_skip_ws(s + 1);
            if (*s == '"') {
                const char *a = s + 1;
                const char *b = js_skip_string(s);
                int len = (int)((b - 1) - a);
                if (len < 0) {
                    len = 0;
                }
                return js_unescape(a, len, out, outn);
            }
            if (*s == '-' || (*s >= '0' && *s <= '9')) {
                int i = 0;
                while (s[i] && (isdigit((unsigned char)s[i]) || s[i] == '-' || s[i] == '.')) {
                    if (i < outn - 1) {
                        out[i] = s[i];
                    }
                    i++;
                }
                if (i >= outn) {
                    i = outn - 1;
                }
                out[i] = '\0';
                return 0;
            }
            return -1;
        }
        s = js_skip_string(s);
        s = js_skip_ws(s);
        if (*s == ':') {
            s = js_skip_value(s + 1);
        }
        s = js_skip_ws(s);
        if (*s == ',') {
            s++;
        }
    }
    return -1;
}

static const char *js_obj_array(const char *obj, const char *end, const char *key)
{
    const char *s = js_skip_ws(obj);
    if (!s || *s != '{') {
        return NULL;
    }
    s++;
    while (s && s < end && *s && *s != '}') {
        s = js_skip_ws(s);
        if (*s != '"') {
            break;
        }
        if (js_streq_key(s, key)) {
            s = js_skip_string(s);
            s = js_skip_ws(s);
            if (*s != ':') {
                return NULL;
            }
            s = js_skip_ws(s + 1);
            if (*s == '[') {
                return s;
            }
            return NULL;
        }
        s = js_skip_string(s);
        s = js_skip_ws(s);
        if (*s == ':') {
            s = js_skip_value(s + 1);
        }
        s = js_skip_ws(s);
        if (*s == ',') {
            s++;
        }
    }
    return NULL;
}

static int pls_score(const char *fmt, const char *qual, const char *url)
{
    int q = 0, f = 0;
    if (qual && strcasecmp(qual, "highest") == 0) {
        q = 30;
    } else if (qual && strcasecmp(qual, "high") == 0) {
        q = 20;
    } else if (qual && strcasecmp(qual, "low") == 0) {
        q = 5;
    }
    if (fmt && strcasecmp(fmt, "mp3") == 0) {
        f = 8; /* widest decoder support */
    } else if (fmt && strcasecmp(fmt, "aac") == 0) {
        f = 6;
    } else if (fmt && strcasecmp(fmt, "aacp") == 0) {
        f = 3;
    }
    if (url && strstr(url, "320")) {
        q += 5;
    }
    return q * 10 + f;
}

static int pick_soma_playlist(const char *ch, const char *ch_end, char *url, int urln)
{
    const char *arr = js_obj_array(ch, ch_end, "playlists");
    const char *s;
    int best = -1;
    char best_url[VIBE_PATH_MAX];
    best_url[0] = '\0';
    if (!arr) {
        return -1;
    }
    s = js_skip_ws(arr + 1);
    while (s && *s && *s != ']') {
        const char *obj = js_skip_ws(s);
        const char *obj_end;
        char fmt[16], qual[16], u[VIBE_PATH_MAX];
        int sc;
        if (*obj != '{') {
            break;
        }
        obj_end = js_skip_object(obj);
        fmt[0] = qual[0] = u[0] = '\0';
        js_obj_str(obj, obj_end, "format", fmt, (int)sizeof(fmt));
        js_obj_str(obj, obj_end, "quality", qual, (int)sizeof(qual));
        js_obj_str(obj, obj_end, "url", u, (int)sizeof(u));
        sc = pls_score(fmt, qual, u);
        if (u[0] && sc > best) {
            best = sc;
            snprintf(best_url, sizeof(best_url), "%s", u);
        }
        s = js_skip_ws(obj_end);
        if (*s == ',') {
            s++;
        }
    }
    if (!best_url[0]) {
        return -1;
    }
    snprintf(url, urln, "%s", best_url);
    return 0;
}

int radio_resolve_play_url(const char *in, char *out, int n)
{
    char *body = NULL;
    const char *p;
    int is_pls = 0, is_m3u = 0;

    if (!in || !out || n < 8) {
        return -1;
    }
    out[0] = '\0';
    p = strrchr(in, '.');
    if (p && strcasecmp(p, ".pls") == 0) {
        is_pls = 1;
    }
    if (p && (strcasecmp(p, ".m3u") == 0 || strcasecmp(p, ".m3u8") == 0)) {
        is_m3u = 1;
    }
    if (!is_pls && !is_m3u) {
        snprintf(out, n, "%s", in);
        return 0;
    }
    if (http_get(in, &body) < 0 || !body) {
        /* local file */
        FILE *fp = fopen(in, "rb");
        long sz;
        if (!fp) {
            snprintf(out, n, "%s", in);
            return 0;
        }
        fseek(fp, 0, SEEK_END);
        sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz < 0 || sz > 1024 * 1024) {
            fclose(fp);
            snprintf(out, n, "%s", in);
            return 0;
        }
        body = (char *)malloc((size_t)sz + 1);
        if (!body) {
            fclose(fp);
            return -1;
        }
        if (fread(body, 1, (size_t)sz, fp) != (size_t)sz) {
            fclose(fp);
            free(body);
            return -1;
        }
        body[sz] = '\0';
        fclose(fp);
    }
    {
        char *line = body;
        while (line && *line) {
            char *nl = strchr(line, '\n');
            char *cr;
            if (nl) {
                *nl = '\0';
            }
            cr = strchr(line, '\r');
            if (cr) {
                *cr = '\0';
            }
            while (*line == ' ' || *line == '\t') {
                line++;
            }
            if (is_pls && strncasecmp(line, "File", 4) == 0) {
                char *eq = strchr(line, '=');
                if (eq && eq[1]) {
                    snprintf(out, n, "%s", eq + 1);
                    free(body);
                    return 0;
                }
            }
            if (is_m3u && line[0] && line[0] != '#') {
                snprintf(out, n, "%s", line);
                free(body);
                return 0;
            }
            line = nl ? nl + 1 : NULL;
        }
    }
    free(body);
    snprintf(out, n, "%s", in);
    return 0;
}

void radio_name_from_url(const char *url, char *out, int n)
{
    const char *s, *e, *slash;
    if (!url || !out || n < 2) {
        return;
    }
    s = url;
    if (strncmp(s, "http://", 7) == 0) {
        s += 7;
    } else if (strncmp(s, "https://", 8) == 0) {
        s += 8;
    }
    slash = strrchr(s, '/');
    e = slash && slash[1] ? slash + 1 : s;
    snprintf(out, n, "%s", e);
    {
        char *q = strchr(out, '?');
        char *dot;
        if (q) {
            *q = '\0';
        }
        dot = strrchr(out, '.');
        if (dot && (strcasecmp(dot, ".pls") == 0 || strcasecmp(dot, ".m3u") == 0 ||
                    strcasecmp(dot, ".m3u8") == 0)) {
            *dot = '\0';
        }
    }
    if (!out[0]) {
        snprintf(out, n, "Station");
    }
}

int radio_fetch_soma(char *err, int errn)
{
    char *body = NULL;
    const char *root, *arr, *s;
    s_nsoma = 0;
    if (http_get("https://somafm.com/channels.json", &body) < 0 || !body) {
        if (err && errn) {
            snprintf(err, errn, "Could not reach Soma.fm");
        }
        return -1;
    }
    root = js_skip_ws(body);
    arr = NULL;
    if (*root == '{') {
        arr = js_obj_array(root, root + strlen(root), "channels");
    } else if (*root == '[') {
        arr = root;
    }
    if (!arr) {
        free(body);
        if (err && errn) {
            snprintf(err, errn, "Bad Soma.fm list");
        }
        return -1;
    }
    s = js_skip_ws(arr + 1);
    while (s && *s && *s != ']' && s_nsoma < RADIO_SOMA_MAX) {
        const char *obj = js_skip_ws(s);
        const char *obj_end;
        RadioItem *it;
        char pls[VIBE_PATH_MAX];
        if (*obj != '{') {
            break;
        }
        obj_end = js_skip_object(obj);
        it = &s_soma[s_nsoma];
        memset(it, 0, sizeof(*it));
        js_obj_str(obj, obj_end, "title", it->name, (int)sizeof(it->name));
        js_obj_str(obj, obj_end, "genre", it->sub, (int)sizeof(it->sub));
        if (!it->sub[0]) {
            js_obj_str(obj, obj_end, "description", it->sub, (int)sizeof(it->sub));
        }
        pls[0] = '\0';
        if (pick_soma_playlist(obj, obj_end, pls, (int)sizeof(pls)) == 0) {
            snprintf(it->url, sizeof(it->url), "%s", pls);
        }
        if (it->name[0] && it->url[0]) {
            s_nsoma++;
        }
        s = js_skip_ws(obj_end);
        if (*s == ',') {
            s++;
        }
    }
    free(body);
    if (s_nsoma <= 0) {
        if (err && errn) {
            snprintf(err, errn, "No Soma.fm channels");
        }
        return -1;
    }
    return s_nsoma;
}

int radio_soma_count(void)
{
    return s_nsoma;
}

int radio_soma_at(int i, RadioItem *out)
{
    if (!out || i < 0 || i >= s_nsoma) {
        return -1;
    }
    *out = s_soma[i];
    return 0;
}

static int country_rank(const RadioCountry *c)
{
    if (strcasecmp(c->name, "United States") == 0 || strcasecmp(c->code, "US") == 0) {
        return 0;
    }
    if (strcasecmp(c->name, "Japan") == 0 || strcasecmp(c->code, "JP") == 0) {
        return 1;
    }
    return 2;
}

static int country_cmp(const void *a, const void *b)
{
    const RadioCountry *ca = a, *cb = b;
    int ra = country_rank(ca), rb = country_rank(cb);
    if (ra != rb) {
        return ra - rb;
    }
    return strcasecmp(ca->name, cb->name);
}

int radio_fetch_countries(char *err, int errn)
{
    char *body = NULL;
    const char *s;
    int hi, n = 0;
    s_ncty = 0;
    for (hi = 0; hi < (int)(sizeof(k_rb_hosts) / sizeof(k_rb_hosts[0])); hi++) {
        char url[256];
        snprintf(url, sizeof(url), "%s/json/countries?order=name", k_rb_hosts[hi]);
        if (http_get(url, &body) >= 0 && body) {
            break;
        }
        free(body);
        body = NULL;
    }
    if (!body) {
        if (err && errn) {
            snprintf(err, errn, "Could not reach Radio Browser");
        }
        return -1;
    }
    s = js_skip_ws(body);
    if (*s != '[') {
        free(body);
        if (err && errn) {
            snprintf(err, errn, "Bad country list");
        }
        return -1;
    }
    s = js_skip_ws(s + 1);
    while (s && *s && *s != ']' && n < RADIO_COUNTRY_MAX) {
        const char *obj = js_skip_ws(s);
        const char *obj_end;
        char cnt[16];
        RadioCountry *c;
        if (*obj != '{') {
            break;
        }
        obj_end = js_skip_object(obj);
        c = &s_cty[n];
        memset(c, 0, sizeof(*c));
        js_obj_str(obj, obj_end, "name", c->name, (int)sizeof(c->name));
        js_obj_str(obj, obj_end, "iso_3166_1", c->code, (int)sizeof(c->code));
        cnt[0] = '\0';
        js_obj_str(obj, obj_end, "stationcount", cnt, (int)sizeof(cnt));
        c->stationcount = atoi(cnt);
        if (c->name[0] && c->code[0] && c->stationcount > 0) {
            n++;
        }
        s = js_skip_ws(obj_end);
        if (*s == ',') {
            s++;
        }
    }
    free(body);
    s_ncty = n;
    if (s_ncty > 1) {
        qsort(s_cty, (size_t)s_ncty, sizeof(s_cty[0]), country_cmp);
    }
    if (s_ncty <= 0) {
        if (err && errn) {
            snprintf(err, errn, "No countries");
        }
        return -1;
    }
    return s_ncty;
}

int radio_country_count(void)
{
    return s_ncty;
}

int radio_country_at(int i, RadioCountry *out)
{
    if (!out || i < 0 || i >= s_ncty) {
        return -1;
    }
    *out = s_cty[i];
    return 0;
}

int radio_fetch_stations(const char *country_code, char *err, int errn)
{
    char *body = NULL;
    const char *s;
    int hi, n = 0;
    s_nst = 0;
    if (!country_code || !country_code[0]) {
        return -1;
    }
    for (hi = 0; hi < (int)(sizeof(k_rb_hosts) / sizeof(k_rb_hosts[0])); hi++) {
        char url[512];
        snprintf(url, sizeof(url),
                 "%s/json/stations/search?countrycode=%s&hidebroken=true&order=clickcount&reverse=true&limit=%d",
                 k_rb_hosts[hi], country_code, RADIO_STATION_MAX);
        if (http_get(url, &body) >= 0 && body) {
            break;
        }
        free(body);
        body = NULL;
    }
    if (!body) {
        if (err && errn) {
            snprintf(err, errn, "Could not load stations");
        }
        return -1;
    }
    s = js_skip_ws(body);
    if (*s != '[') {
        free(body);
        if (err && errn) {
            snprintf(err, errn, "Bad station list");
        }
        return -1;
    }
    s = js_skip_ws(s + 1);
    while (s && *s && *s != ']' && n < RADIO_STATION_MAX) {
        const char *obj = js_skip_ws(s);
        const char *obj_end;
        RadioItem *it;
        char clicks[32], codec[32], br[16];
        if (*obj != '{') {
            break;
        }
        obj_end = js_skip_object(obj);
        it = &s_st[n];
        memset(it, 0, sizeof(*it));
        js_obj_str(obj, obj_end, "name", it->name, (int)sizeof(it->name));
        js_obj_str(obj, obj_end, "url_resolved", it->url, (int)sizeof(it->url));
        if (!it->url[0]) {
            js_obj_str(obj, obj_end, "url", it->url, (int)sizeof(it->url));
        }
        clicks[0] = codec[0] = br[0] = '\0';
        js_obj_str(obj, obj_end, "clickcount", clicks, (int)sizeof(clicks));
        js_obj_str(obj, obj_end, "codec", codec, (int)sizeof(codec));
        js_obj_str(obj, obj_end, "bitrate", br, (int)sizeof(br));
        if (codec[0] && br[0] && strcmp(br, "0") != 0) {
            snprintf(it->sub, sizeof(it->sub), "%s  ·  %s kbps  ·  %s plays", codec, br, clicks[0] ? clicks : "0");
        } else if (codec[0]) {
            snprintf(it->sub, sizeof(it->sub), "%s  ·  %s plays", codec, clicks[0] ? clicks : "0");
        } else {
            snprintf(it->sub, sizeof(it->sub), "%s plays", clicks[0] ? clicks : "0");
        }
        if (it->name[0] && it->url[0]) {
            n++;
        }
        s = js_skip_ws(obj_end);
        if (*s == ',') {
            s++;
        }
    }
    free(body);
    s_nst = n;
    if (s_nst <= 0) {
        if (err && errn) {
            snprintf(err, errn, "No stations for this country");
        }
        return -1;
    }
    return s_nst;
}

int radio_station_count(void)
{
    return s_nst;
}

int radio_station_at(int i, RadioItem *out)
{
    if (!out || i < 0 || i >= s_nst) {
        return -1;
    }
    *out = s_st[i];
    return 0;
}
