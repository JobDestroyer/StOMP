/* SPDX-License-Identifier: LGPL-2.1-only
 * Copyright (C) 2026 JobDestroyer
 */

#include "ui.h"

#include "app.h"
#include "radio.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define UI_MAX_VTX 16384
#define ATLAS_W 2048
#define ATLAS_H 2048
#define FONT_FACES 4
#define GLYPH_CACHE 2048
#define UI_STACK 8
#define FOLDER_STACK 16

typedef struct {
    float x, y, u, v, r, g, b, a;
} Vtx;

typedef struct {
    float ax, ay, bw, bh, adv;
    int atlas_x, atlas_y;
} Glyph;

typedef struct {
    char line[128];
    char sub[192];
    int64_t id;
    int kind;
} Row;

static Vtx s_vtx[UI_MAX_VTX];
static int s_nv;
static unsigned s_prog, s_vao, s_vbo, s_atlas, s_white;
static int s_u_res, s_u_tex, s_u_mode;
static Glyph s_gcache[GLYPH_CACHE];
static uint32_t s_gcp[GLYPH_CACHE];
static int s_font_ok;
static float s_size_px = 36.f;
static FT_Library s_ft;
static FT_Face s_faces[FONT_FACES];
static int s_nfaces;
static int s_pen_x, s_pen_y, s_row_h;
static int s_win_w = 1920, s_win_h = 1080;
static float s_veil = 0.28f;
static float s_scale = 1.f;

static UiView s_view = UI_VIEW_NOW_PLAYING;
static UiView s_stack[UI_STACK];
static int s_stack_focus[UI_STACK];
static int s_stack_scroll[UI_STACK];
static int s_sp;
static int s_overlay = 1;
static uint32_t s_last_input;
static int s_idle_on;
static Row s_rows[VIBE_UI_ROWS_MAX];
static int s_nrows;
static int s_focus;
static int s_scroll;
static char s_search[64];
static int s_kb_i;
static int s_show_kb;
static int s_kb_shift;
static int s_kb_sym;
static int s_kb_zone;
static size_t s_kb_caret;
static int s_np_opt; /* 0 = seek strip (A play/pause); 1+ = extras row */
#define NP_OPT_PREV 1
#define NP_OPT_PLAY 2
#define NP_OPT_STOP 3
#define NP_OPT_NEXT 4
#define NP_OPT_SHUFFLE 5
#define NP_OPT_REPEAT 6
#define NP_OPT_VOLUME 7
#define NP_OPT_PRESET 8
#define NP_OPT_EYE 9
#define NP_OPT_LOCK 10
#define NP_OPT_MAX 10
static int s_kb_active = 1;
static int64_t s_ctx_album, s_ctx_artist, s_ctx_folder, s_ctx_playlist;
static int64_t s_folder_stack[FOLDER_STACK];
static int s_folder_focus[FOLDER_STACK];
static int s_folder_scroll[FOLDER_STACK];
static int s_folder_sp;
static int s_vol_open;
static float s_vol_draft;
static float s_vol_saved;
static uint32_t s_rate_flash_ms;
static int s_rate_flash_good;
static char s_radio_cc[8];
static char s_radio_url[VIBE_PATH_MAX];
static char s_radio_err[160];
#define RADIO_URL_SLOTS 1024
static char s_radio_play_url[RADIO_URL_SLOTS][VIBE_PATH_MAX];
static int s_hold_kind;
static int s_hold_did;
static int s_quit_prompt;
static int s_quit_choice;
static int s_bind_capture;
static int s_bind_target;
static char s_folder_title[VIBE_NAME_MAX];
static int s_settings_audio_i;
static char s_pl_name[VIBE_NAME_MAX];
static char s_browse[VIBE_PATH_MAX];
static char s_dirents[VIBE_LIST_MAX][VIBE_NAME_MAX];
/* Query scratch lives in BSS: LibTrack[VIBE_LIST_MAX] is ~7MB and must not sit on the stack. */
static union {
    LibTrack tracks[VIBE_LIST_MAX];
    LibAlbum albums[VIBE_LIST_MAX];
    LibArtist artists[VIBE_LIST_MAX];
    LibFolder folders[VIBE_LIST_MAX];
    LibPlaylist playlists[VIBE_LIST_MAX];
} s_qbuf;
static int s_ndirents;
static uint32_t s_jump_cp;
static uint32_t s_jump_ms;

#define KB_SPEC_BKSP  1
#define KB_SPEC_CLEAR 2
#define KB_SPEC_DONE  3
#define KB_SPEC_SHIFT 4
#define KB_SPEC_SYMS  5
#define KB_SPEC_STEAM 6
#define KB_ZONE_FIELD 0
#define KB_ZONE_KEYS  1
#define KB_COLS 10
#define KB_ROWS 6

typedef struct {
    int row;
    int col;
    int span;
    char ch;  /* 0 = special */
    unsigned special;
} KbKey;

static const KbKey k_letter_keys[] = {
    {0, 0, 1, '1', 0}, {0, 1, 1, '2', 0}, {0, 2, 1, '3', 0},
    {0, 3, 1, '4', 0}, {0, 4, 1, '5', 0}, {0, 5, 1, '6', 0},
    {0, 6, 1, '7', 0}, {0, 7, 1, '8', 0}, {0, 8, 1, '9', 0},
    {0, 9, 1, '0', 0},
    {1, 0, 1, 'q', 0}, {1, 1, 1, 'w', 0}, {1, 2, 1, 'e', 0},
    {1, 3, 1, 'r', 0}, {1, 4, 1, 't', 0}, {1, 5, 1, 'y', 0},
    {1, 6, 1, 'u', 0}, {1, 7, 1, 'i', 0}, {1, 8, 1, 'o', 0},
    {1, 9, 1, 'p', 0},
    {2, 0, 1, 'a', 0}, {2, 1, 1, 's', 0}, {2, 2, 1, 'd', 0},
    {2, 3, 1, 'f', 0}, {2, 4, 1, 'g', 0}, {2, 5, 1, 'h', 0},
    {2, 6, 1, 'j', 0}, {2, 7, 1, 'k', 0}, {2, 8, 1, 'l', 0},
    {2, 9, 1, '\'', 0},
    {3, 0, 1, 'z', 0}, {3, 1, 1, 'x', 0}, {3, 2, 1, 'c', 0},
    {3, 3, 1, 'v', 0}, {3, 4, 1, 'b', 0}, {3, 5, 1, 'n', 0},
    {3, 6, 1, 'm', 0}, {3, 7, 1, '-', 0}, {3, 8, 1, '_', 0},
    {3, 9, 1, '.', 0},
    {4, 0, 2, 0, KB_SPEC_SHIFT}, {4, 2, 2, 0, KB_SPEC_SYMS},
    {4, 4, 4, ' ', 0}, {4, 8, 2, 0, KB_SPEC_BKSP},
    {5, 0, 4, 0, KB_SPEC_STEAM}, {5, 4, 3, 0, KB_SPEC_CLEAR},
    {5, 7, 3, 0, KB_SPEC_DONE}
};

static const KbKey k_symbol_keys[] = {
    {0, 0, 1, '1', 0}, {0, 1, 1, '2', 0}, {0, 2, 1, '3', 0},
    {0, 3, 1, '4', 0}, {0, 4, 1, '5', 0}, {0, 5, 1, '6', 0},
    {0, 6, 1, '7', 0}, {0, 7, 1, '8', 0}, {0, 8, 1, '9', 0},
    {0, 9, 1, '0', 0},
    {1, 0, 1, '-', 0}, {1, 1, 1, '_', 0}, {1, 2, 1, '=', 0},
    {1, 3, 1, '+', 0}, {1, 4, 1, '/', 0}, {1, 5, 1, '\\', 0},
    {1, 6, 1, ':', 0}, {1, 7, 1, ';', 0}, {1, 8, 1, '@', 0},
    {1, 9, 1, '#', 0},
    {2, 0, 1, '.', 0}, {2, 1, 1, ',', 0}, {2, 2, 1, '?', 0},
    {2, 3, 1, '!', 0}, {2, 4, 1, '&', 0}, {2, 5, 1, '%', 0},
    {2, 6, 1, '~', 0}, {2, 7, 1, '*', 0}, {2, 8, 1, '"', 0},
    {2, 9, 1, '\'', 0},
    {3, 0, 1, '(', 0}, {3, 1, 1, ')', 0}, {3, 2, 1, '[', 0},
    {3, 3, 1, ']', 0}, {3, 4, 1, '{', 0}, {3, 5, 1, '}', 0},
    {3, 6, 1, '<', 0}, {3, 7, 1, '>', 0}, {3, 8, 1, '|', 0},
    {3, 9, 1, '$', 0},
    {4, 0, 2, 0, KB_SPEC_SHIFT}, {4, 2, 2, 0, KB_SPEC_SYMS},
    {4, 4, 4, ' ', 0}, {4, 8, 2, 0, KB_SPEC_BKSP},
    {5, 0, 4, 0, KB_SPEC_STEAM}, {5, 4, 3, 0, KB_SPEC_CLEAR},
    {5, 7, 3, 0, KB_SPEC_DONE}
};

static const KbKey *kb_cur(void)
{
    return s_kb_sym ? k_symbol_keys : k_letter_keys;
}

static int kb_count(void)
{
    return s_kb_sym ? (int)(sizeof(k_symbol_keys) / sizeof(k_symbol_keys[0]))
                    : (int)(sizeof(k_letter_keys) / sizeof(k_letter_keys[0]));
}

static int kb_find(int row, int col)
{
    const KbKey *keys = kb_cur();
    int n = kb_count();
    int best = -1, bestd = 99, i;
    for (i = 0; i < n; i++) {
        int d;
        if (keys[i].row != row) {
            continue;
        }
        d = keys[i].col - col;
        if (d < 0) {
            d = -d;
        }
        if (d < bestd) {
            bestd = d;
            best = i;
        }
    }
    return best;
}

static void kb_move(int dr, int dc)
{
    const KbKey *keys = kb_cur();
    int n = kb_count();
    int row, col, next;
    if (s_kb_i < 0 || s_kb_i >= n) {
        s_kb_i = 0;
    }
    row = keys[s_kb_i].row;
    col = keys[s_kb_i].col;
    if (dc != 0) {
        next = s_kb_i + dc;
        if (next >= 0 && next < n && keys[next].row == row) {
            s_kb_i = next;
        }
        return;
    }
    row += dr;
    if (row < 0) {
        s_kb_zone = KB_ZONE_FIELD;
        return;
    }
    if (row >= KB_ROWS) {
        return;
    }
    next = kb_find(row, col);
    if (next >= 0) {
        s_kb_i = next;
    }
}

static const char *k_vs =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "layout(location=2) in vec4 a_col;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_col;\n"
    "uniform vec2 u_res;\n"
    "void main(){\n"
    "  vec2 p = a_pos / u_res * 2.0 - 1.0;\n"
    "  p.y = -p.y;\n"
    "  gl_Position = vec4(p, 0.0, 1.0);\n"
    "  v_uv = a_uv;\n"
    "  v_col = a_col;\n"
    "}\n";

static const char *k_fs =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "in vec4 v_col;\n"
    "out vec4 frag;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_mode;\n"
    "void main(){\n"
    "  if (u_mode==0) frag = v_col;\n"
    "  else frag = texture(u_tex, v_uv) * v_col;\n"
    "}\n";

static int compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, (GLsizei)sizeof(log), NULL, log);
        fprintf(stderr, "StOMP: shader: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return (int)s;
}

static int font_readable(const char *p)
{
    FILE *f;
    if (!p || !p[0]) {
        return 0;
    }
    f = fopen(p, "rb");
    if (!f) {
        return 0;
    }
    fclose(f);
    return 1;
}

static uint32_t utf8_next(const char **ps)
{
    const unsigned char *s;
    uint32_t cp;
    if (!ps || !*ps) {
        return 0;
    }
    s = (const unsigned char *)*ps;
    if (!s[0]) {
        return 0;
    }
    if (s[0] < 0x80) {
        *ps += 1;
        return s[0];
    }
    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        *ps += 2;
        return cp < 0x80 ? 0xFFFD : cp;
    }
    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
        *ps += 3;
        return cp < 0x800 ? 0xFFFD : cp;
    }
    if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
        (s[3] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
             ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
        *ps += 4;
        return (cp < 0x10000 || cp > 0x10FFFF) ? 0xFFFD : cp;
    }
    *ps += 1;
    return 0xFFFD;
}

static void utf8_put(char *dst, size_t n, uint32_t cp)
{
    if (!dst || n == 0) {
        return;
    }
    if (cp < 0x80 && n > 1) {
        dst[0] = (char)cp;
        dst[1] = '\0';
    } else if (cp < 0x800 && n > 2) {
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3F));
        dst[2] = '\0';
    } else if (cp < 0x10000 && n > 3) {
        dst[0] = (char)(0xE0 | (cp >> 12));
        dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (cp & 0x3F));
        dst[3] = '\0';
    } else if (n > 4) {
        dst[0] = (char)(0xF0 | (cp >> 18));
        dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[3] = (char)(0x80 | (cp & 0x3F));
        dst[4] = '\0';
    } else {
        dst[0] = '?';
        dst[1] = '\0';
    }
}

static int add_face(const char *path)
{
    if (!path || s_nfaces >= FONT_FACES || !font_readable(path)) {
        return -1;
    }
    if (FT_New_Face(s_ft, path, 0, &s_faces[s_nfaces]) != 0) {
        return -1;
    }
    FT_Set_Pixel_Sizes(s_faces[s_nfaces], 0, 36);
    fprintf(stderr, "StOMP: font %s\n", path);
    s_nfaces++;
    return 0;
}

static void try_add_rel_font(const char *rel)
{
    char bundled[1024];
    char root[1024];
    if (config_resource_dir(root, sizeof(root)) == 0) {
        snprintf(bundled, sizeof(bundled), "%s/%s", root, rel);
        if (add_face(bundled) == 0) {
            return;
        }
    }
    {
        char *base = SDL_GetBasePath();
        if (base) {
            snprintf(bundled, sizeof(bundled), "%s%s", base, rel);
            SDL_free(base);
            if (add_face(bundled) == 0) {
                return;
            }
        }
    }
#ifdef VIBE_SOURCE_DIR
    snprintf(bundled, sizeof(bundled), "%s/%s", VIBE_SOURCE_DIR, rel);
    add_face(bundled);
#endif
}

static Glyph *cache_lookup(uint32_t cp)
{
    uint32_t h = cp * 2654435761u;
    unsigned i;
    for (i = 0; i < GLYPH_CACHE; i++) {
        uint32_t j = (h + i) % (uint32_t)GLYPH_CACHE;
        if (s_gcp[j] == 0) {
            return NULL;
        }
        if (s_gcp[j] == cp) {
            return &s_gcache[j];
        }
    }
    return NULL;
}

static Glyph *cache_insert(uint32_t cp, const Glyph *g)
{
    uint32_t h = cp * 2654435761u;
    unsigned i;
    for (i = 0; i < GLYPH_CACHE; i++) {
        uint32_t j = (h + i) % (uint32_t)GLYPH_CACHE;
        if (s_gcp[j] == 0 || s_gcp[j] == cp) {
            s_gcp[j] = cp;
            s_gcache[j] = *g;
            return &s_gcache[j];
        }
    }
    return NULL;
}

static int atlas_pack(int bw, int bh, int *ox, int *oy)
{
    if (bw < 0) {
        bw = 0;
    }
    if (bh < 0) {
        bh = 0;
    }
    if (s_pen_x + bw + 2 >= ATLAS_W) {
        s_pen_x = 2;
        s_pen_y += s_row_h + 2;
        s_row_h = 0;
    }
    if (s_pen_y + bh + 2 >= ATLAS_H) {
        return -1;
    }
    *ox = s_pen_x;
    *oy = s_pen_y;
    s_pen_x += bw + 2;
    if (bh + 2 > s_row_h) {
        s_row_h = bh + 2;
    }
    return 0;
}

static void atlas_blit(int x, int y, int bw, int bh, const unsigned char *src, int pitch)
{
    static unsigned char scratch[96 * 96 * 4];
    unsigned char *rgba;
    int row, col;
    size_t need;
    int heap = 0;
    if (bw <= 0 || bh <= 0 || !src) {
        return;
    }
    need = (size_t)bw * (size_t)bh * 4u;
    if (need <= sizeof(scratch)) {
        rgba = scratch;
    } else {
        rgba = (unsigned char *)malloc(need);
        if (!rgba) {
            return;
        }
        heap = 1;
    }
    for (row = 0; row < bh; row++) {
        const unsigned char *srow = src + row * pitch;
        unsigned char *drow = rgba + (size_t)row * (size_t)bw * 4u;
        for (col = 0; col < bw; col++) {
            unsigned char a = srow[col];
            drow[col * 4 + 0] = 255;
            drow[col * 4 + 1] = 255;
            drow[col * 4 + 2] = 255;
            drow[col * 4 + 3] = a;
        }
    }
    glBindTexture(GL_TEXTURE_2D, s_atlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, bw, bh, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    if (heap) {
        free(rgba);
    }
}

static void font_reset_atlas(void);

static int rasterize_cp(uint32_t cp, Glyph *out)
{
    FT_Face face = NULL;
    FT_UInt idx = 0;
    int i, bw, bh, ox, oy, pitch;
    const unsigned char *buf;

    memset(out, 0, sizeof(*out));
    if (cp == 0) {
        return -1;
    }
    for (i = 0; i < s_nfaces; i++) {
        idx = FT_Get_Char_Index(s_faces[i], (FT_ULong)cp);
        if (idx != 0) {
            face = s_faces[i];
            break;
        }
    }
    if (!face) {
        if (cp != '?' && cp != 0xFFFD) {
            return rasterize_cp('?', out);
        }
        if (s_nfaces > 0) {
            face = s_faces[0];
            idx = FT_Get_Char_Index(face, (FT_ULong)'?');
        }
        if (!face) {
            return -1;
        }
    }
    if (FT_Load_Glyph(face, idx, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) {
        return -1;
    }
    if (face->glyph->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) {
        if (FT_Load_Glyph(face, idx, FT_LOAD_RENDER | FT_LOAD_TARGET_MONO) != 0) {
            return -1;
        }
    }
    bw = (int)face->glyph->bitmap.width;
    bh = (int)face->glyph->bitmap.rows;
    pitch = (int)face->glyph->bitmap.pitch;
    buf = face->glyph->bitmap.buffer;
    if (atlas_pack(bw, bh, &ox, &oy) != 0) {
        font_reset_atlas();
        if (atlas_pack(bw, bh, &ox, &oy) != 0) {
            return -1;
        }
    }
    if (bw > 0 && bh > 0 && buf && pitch > 0) {
        atlas_blit(ox, oy, bw, bh, buf, pitch);
    }
    out->atlas_x = ox;
    out->atlas_y = oy;
    out->bw = (float)bw;
    out->bh = (float)bh;
    out->ax = (float)face->glyph->bitmap_left;
    out->ay = (float)face->glyph->bitmap_top;
    out->adv = (float)(face->glyph->advance.x >> 6);
    if (out->adv <= 0.f && cp == ' ') {
        out->adv = s_size_px * 0.33f;
    }
    return 0;
}

static void preload_ascii(void)
{
    uint32_t c;
    for (c = 32; c <= 126; c++) {
        Glyph g;
        if (rasterize_cp(c, &g) == 0) {
            cache_insert(c, &g);
        }
    }
}

static void font_reset_atlas(void)
{
    unsigned char *rgba;
    memset(s_gcp, 0, sizeof(s_gcp));
    s_pen_x = 2;
    s_pen_y = 2;
    s_row_h = 0;
    rgba = (unsigned char *)calloc((size_t)ATLAS_W * (size_t)ATLAS_H * 4u, 1);
    if (rgba && s_atlas) {
        glBindTexture(GL_TEXTURE_2D, s_atlas);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ATLAS_W, ATLAS_H, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    free(rgba);
    preload_ascii();
}

static Glyph *glyph_get(uint32_t cp)
{
    Glyph *hit;
    Glyph g;
    if (cp == 0) {
        return NULL;
    }
    hit = cache_lookup(cp);
    if (hit) {
        return hit;
    }
    if (rasterize_cp(cp, &g) != 0) {
        return cache_lookup('?');
    }
    hit = cache_insert(cp, &g);
    if (!hit) {
        font_reset_atlas();
        if (rasterize_cp(cp, &g) != 0) {
            return cache_lookup('?');
        }
        hit = cache_insert(cp, &g);
    }
    return hit;
}

static void font_shutdown(void)
{
    int i;
    for (i = 0; i < s_nfaces; i++) {
        if (s_faces[i]) {
            FT_Done_Face(s_faces[i]);
            s_faces[i] = NULL;
        }
    }
    s_nfaces = 0;
    if (s_ft) {
        FT_Done_FreeType(s_ft);
        s_ft = NULL;
    }
    s_font_ok = 0;
}

static int load_font(void)
{
    unsigned char white[4] = {255, 255, 255, 255};
    unsigned char *rgba;
    const char *cjk_sys[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"
    };
    size_t i;

    font_shutdown();
    if (FT_Init_FreeType(&s_ft) != 0) {
        fprintf(stderr, "StOMP: FT_Init_FreeType failed\n");
        return -1;
    }
    s_size_px = 36.f;
    try_add_rel_font("fonts/DejaVuSans.ttf");
    if (s_nfaces == 0) {
        add_face("/usr/share/fonts/TTF/DejaVuSans.ttf");
        add_face("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
        add_face("/usr/share/fonts/TTF/LiberationSans-Regular.ttf");
        add_face("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf");
    }
    try_add_rel_font("fonts/DroidSansFallback.ttf");
    for (i = 0; i < sizeof(cjk_sys) / sizeof(cjk_sys[0]); i++) {
        add_face(cjk_sys[i]);
    }
    if (s_nfaces == 0) {
        fprintf(stderr, "StOMP: no TTF font found\n");
        font_shutdown();
        return -1;
    }

    glGenTextures(1, &s_atlas);
    glBindTexture(GL_TEXTURE_2D, s_atlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    rgba = (unsigned char *)calloc((size_t)ATLAS_W * (size_t)ATLAS_H * 4u, 1);
    if (!rgba) {
        font_shutdown();
        return -1;
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ATLAS_W, ATLAS_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);
    s_pen_x = 2;
    s_pen_y = 2;
    s_row_h = 0;
    memset(s_gcp, 0, sizeof(s_gcp));
    preload_ascii();

    glGenTextures(1, &s_white);
    glBindTexture(GL_TEXTURE_2D, s_white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    s_font_ok = 1;
    return 0;
}

static int build_prog(void)
{
    GLuint vs, fs, p;
    GLint ok = 0;
    vs = (GLuint)compile_shader(GL_VERTEX_SHADER, k_vs);
    fs = (GLuint)compile_shader(GL_FRAGMENT_SHADER, k_fs);
    if (!vs || !fs) {
        return -1;
    }
    p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_uv");
    glBindAttribLocation(p, 2, "a_col");
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, (GLsizei)sizeof(log), NULL, log);
        fprintf(stderr, "StOMP: program: %s\n", log);
        return -1;
    }
    s_prog = p;
    s_u_res = glGetUniformLocation(p, "u_res");
    s_u_tex = glGetUniformLocation(p, "u_tex");
    s_u_mode = glGetUniformLocation(p, "u_mode");
    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(s_vtx), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)(4 * sizeof(float)));
    glBindVertexArray(0);
    return 0;
}

static int kb_on(void);
static void sync_text_input(void);

int ui_init(int win_w, int win_h, float veil)
{
    s_win_w = win_w > 0 ? win_w : 1920;
    s_win_h = win_h > 0 ? win_h : 1080;
    s_veil = veil;
    s_scale = (float)s_win_h / 1080.f;
    if (s_scale < 0.55f) {
        s_scale = 0.55f;
    }
    s_last_input = SDL_GetTicks();
    s_overlay = 1;
    if (build_prog() != 0) {
        return -1;
    }
    if (load_font() != 0) {
        fprintf(stderr, "StOMP: continuing without a font atlas\n");
    }
    sync_text_input();
    return 0;
}

void ui_shutdown(void)
{
    font_shutdown();
    if (s_atlas) {
        glDeleteTextures(1, &s_atlas);
    }
    if (s_white) {
        glDeleteTextures(1, &s_white);
    }
    if (s_vbo) {
        glDeleteBuffers(1, &s_vbo);
    }
    if (s_vao) {
        glDeleteVertexArrays(1, &s_vao);
    }
    if (s_prog) {
        glDeleteProgram(s_prog);
    }
    s_atlas = s_white = s_vbo = s_vao = s_prog = 0;
}

void ui_resize(int win_w, int win_h)
{
    s_win_w = win_w;
    s_win_h = win_h;
    s_scale = (float)s_win_h / 1080.f;
    if (s_scale < 0.55f) {
        s_scale = 0.55f;
    }
}

void ui_set_veil(float veil)
{
    s_veil = veil;
}

static void kb_field_geom(float *x, float *y, float *w, float *h)
{
    *x = 64.f * s_scale;
    *y = 110.f * s_scale;
    *w = (float)s_win_w - 128.f * s_scale;
    *h = 76.f * s_scale;
    if (*w < 64.f) {
        *w = 64.f;
    }
}

static void kb_set_text_rect(void)
{
    SDL_Rect r;
    float x, y, w, h;
    kb_field_geom(&x, &y, &w, &h);
    r.x = (int)x;
    r.y = (int)y;
    r.w = (int)w;
    r.h = (int)h;
    SDL_SetTextInputRect(&r);
}

static void open_steam_keyboard(void)
{
    kb_set_text_rect();
    SDL_StartTextInput();
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (SDL_OpenURL("steam://open/keyboard") != 0) {
        fprintf(stderr, "StOMP: steam keyboard: %s\n", SDL_GetError());
    }
#endif
}

static void sync_text_input(void)
{
    if (kb_on()) {
        kb_set_text_rect();
        SDL_StartTextInput();
    } else {
        SDL_StopTextInput();
    }
}

void ui_set_view(UiView v)
{
    s_view = v;
    s_overlay = 1;
    s_idle_on = 0;
    if (v == UI_VIEW_NOW_PLAYING) {
        s_np_opt = 0;
    }
    sync_text_input();
}

UiView ui_view(void)
{
    return s_view;
}

void ui_show_overlay(void)
{
    s_overlay = 1;
    s_idle_on = 0;
}

void ui_hide_overlay(void)
{
    if (s_view == UI_VIEW_NOW_PLAYING) {
        s_overlay = 0;
        s_idle_on = 1;
    }
}

int ui_overlay_visible(void)
{
    return s_overlay;
}

int ui_binding_mode(void)
{
    return s_bind_capture;
}

void ui_binding_captured(int button)
{
    if (!s_bind_capture) {
        return;
    }
    if (button == SDL_CONTROLLER_BUTTON_B) {
        s_bind_capture = 0;
        return;
    }
    if (button == SDL_CONTROLLER_BUTTON_DPAD_UP || button == SDL_CONTROLLER_BUTTON_DPAD_DOWN ||
        button == SDL_CONTROLLER_BUTTON_DPAD_LEFT || button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT ||
        button == SDL_CONTROLLER_BUTTON_GUIDE) {
        return;
    }
    for (int i = 0; i < VIBE_BIND_COUNT; i++) {
        if (i != s_bind_target && input_binds()[i] == button) {
            input_set_binding((VibeBind)i, SDL_CONTROLLER_BUTTON_INVALID);
        }
    }
    input_set_binding((VibeBind)s_bind_target, (SDL_GameControllerButton)button);
    s_bind_capture = 0;
}

void ui_note_input(uint32_t now_ms)
{
    s_last_input = now_ms;
    s_overlay = 1;
    s_idle_on = 0;
}

void ui_idle_tick(uint32_t now_ms)
{
    if (s_vol_open) {
        return;
    }
    if (s_view == UI_VIEW_NOW_PLAYING && s_overlay && (now_ms - s_last_input) > 4000) {
        s_overlay = 0;
        s_idle_on = 1;
    }
}

int ui_focus_kind(void)
{
    if (s_focus < 0 || s_focus >= s_nrows) {
        return UI_KIND_NONE;
    }
    return s_rows[s_focus].kind;
}

int64_t ui_focus_id(void)
{
    if (s_focus < 0 || s_focus >= s_nrows) {
        return 0;
    }
    return s_rows[s_focus].id;
}

const char *ui_search_query(void)
{
    return s_search;
}

static void push_view(UiView v)
{
    if (s_sp < UI_STACK) {
        s_stack[s_sp] = s_view;
        s_stack_focus[s_sp] = s_focus;
        s_stack_scroll[s_sp] = s_scroll;
        s_sp++;
    }
    s_view = v;
    s_focus = 0;
    s_scroll = 0;
    s_overlay = 1;
    sync_text_input();
}

static void pop_view(void)
{
    if (s_sp > 0) {
        s_sp--;
        s_view = s_stack[s_sp];
        s_focus = s_stack_focus[s_sp];
        s_scroll = s_stack_scroll[s_sp];
    } else {
        s_view = UI_VIEW_NOW_PLAYING;
        s_focus = 0;
        s_scroll = 0;
    }
    s_overlay = 1;
    sync_text_input();
}

static void add_row(const char *line, const char *sub, int64_t id, int kind)
{
    Row *r;
    if (s_nrows >= VIBE_UI_ROWS_MAX) {
        return;
    }
    r = &s_rows[s_nrows++];
    snprintf(r->line, sizeof(r->line), "%s", line ? line : "");
    snprintf(r->sub, sizeof(r->sub), "%s", sub ? sub : "");
    r->id = id;
    r->kind = kind;
}

static void add_track_row(const char *title, const char *base, int64_t id)
{
    char marked[160];
    const char *use = base ? base : "";
    if (library_queue_has(id)) {
        if (use[0]) {
            snprintf(marked, sizeof(marked), "%s  ·  On Current Playlist", use);
        } else {
            snprintf(marked, sizeof(marked), "On Current Playlist");
        }
        use = marked;
    }
    add_row(title, use, id, UI_KIND_TRACK);
}

static void fmt_queue_cover(char *out, size_t n, const char *base, int cover)
{
    char tmp[160];
    const char *tag = NULL;
    if (!out || n == 0) {
        return;
    }
    if (cover >= 2) {
        tag = "On Current Playlist";
    } else if (cover == 1) {
        tag = "Partially On Playlist";
    }
    if (tag && base && base[0]) {
        snprintf(tmp, sizeof(tmp), "%s  ·  %s", base, tag);
    } else if (tag) {
        snprintf(tmp, sizeof(tmp), "%s", tag);
    } else {
        snprintf(tmp, sizeof(tmp), "%s", base ? base : "");
    }
    snprintf(out, n, "%s", tmp);
}

static int kb_on(void)
{
    return s_view == UI_VIEW_SEARCH || s_view == UI_VIEW_RADIO_ADD ||
           s_view == UI_VIEW_QUEUE_SAVE;
}

static char *kb_buf(void)
{
    if (s_view == UI_VIEW_RADIO_ADD) {
        return s_radio_url;
    }
    if (s_view == UI_VIEW_QUEUE_SAVE) {
        return s_pl_name;
    }
    return s_search;
}

static size_t kb_cap(void)
{
    if (s_view == UI_VIEW_RADIO_ADD) {
        return sizeof(s_radio_url);
    }
    if (s_view == UI_VIEW_QUEUE_SAVE) {
        return sizeof(s_pl_name);
    }
    return sizeof(s_search);
}

static const char *kb_placeholder(void)
{
    if (s_view == UI_VIEW_RADIO_ADD) {
        return "Stream URL";
    }
    if (s_view == UI_VIEW_QUEUE_SAVE) {
        return "Playlist name";
    }
    return "Search library";
}

static void kb_clamp_caret(void)
{
    size_t n = strlen(kb_buf());
    if (s_kb_caret > n) {
        s_kb_caret = n;
    }
}

static size_t kb_utf8_prev(const char *s, size_t i)
{
    if (i == 0) {
        return 0;
    }
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) {
        i--;
    }
    return i;
}

static size_t kb_utf8_nexti(const char *s, size_t i)
{
    unsigned char c;
    size_t n;
    if (!s[i]) {
        return i;
    }
    c = (unsigned char)s[i];
    if ((c & 0x80) == 0) {
        i += 1;
    } else if ((c & 0xE0) == 0xC0) {
        i += 2;
    } else if ((c & 0xF0) == 0xE0) {
        i += 3;
    } else if ((c & 0xF8) == 0xF0) {
        i += 4;
    } else {
        i += 1;
    }
    n = strlen(s);
    if (i > n) {
        i = n;
    }
    return i;
}

static void kb_insert(const char *text)
{
    char *buf = kb_buf();
    size_t cap = kb_cap();
    size_t n, m, c;
    if (!text || !text[0]) {
        return;
    }
    n = strlen(buf);
    m = strlen(text);
    kb_clamp_caret();
    c = s_kb_caret;
    if (n + m >= cap) {
        size_t room = (cap > n + 1) ? cap - 1 - n : 0;
        if (room == 0) {
            return;
        }
        while (room > 0 && ((unsigned char)text[room] & 0xC0) == 0x80) {
            room--;
        }
        if (room == 0 || ((unsigned char)text[room] & 0xC0) == 0x80) {
            return;
        }
        m = room;
    }
    memmove(buf + c + m, buf + c, n - c + 1);
    memcpy(buf + c, text, m);
    s_kb_caret = c + m;
}

static void kb_backspace(void)
{
    char *buf = kb_buf();
    size_t n, c, p;
    kb_clamp_caret();
    c = s_kb_caret;
    if (c == 0) {
        return;
    }
    p = kb_utf8_prev(buf, c);
    n = strlen(buf);
    memmove(buf + p, buf + c, n - c + 1);
    s_kb_caret = p;
}

static void kb_clear(void)
{
    kb_buf()[0] = '\0';
    s_kb_caret = 0;
}

static void kb_reset(void)
{
    s_show_kb = 1;
    s_kb_active = 1;
    s_kb_i = 0;
    s_kb_shift = 0;
    s_kb_sym = 0;
    s_kb_zone = KB_ZONE_KEYS;
    s_kb_caret = strlen(kb_buf());
}

static int kb_nav(int dr, int dc)
{
    const KbKey *keys;
    int n;
    if (!kb_on() || !s_kb_active) {
        return 0;
    }
    if (s_kb_zone == KB_ZONE_FIELD) {
        kb_clamp_caret();
        if (dc < 0) {
            s_kb_caret = kb_utf8_prev(kb_buf(), s_kb_caret);
        } else if (dc > 0) {
            s_kb_caret = kb_utf8_nexti(kb_buf(), s_kb_caret);
        } else if (dr > 0) {
            if (s_nrows > 0) {
                s_kb_active = 0;
                s_focus = 0;
            } else {
                s_kb_zone = KB_ZONE_KEYS;
            }
        }
        return 1;
    }
    n = kb_count();
    if (s_kb_i < 0 || s_kb_i >= n) {
        s_kb_i = 0;
    }
    keys = kb_cur();
    if (dr < 0 && keys[s_kb_i].row == 0) {
        if (s_nrows > 0) {
            s_kb_active = 0;
            s_focus = s_nrows - 1;
        } else {
            s_kb_zone = KB_ZONE_FIELD;
        }
        return 1;
    }
    kb_move(dr, dc);
    return 1;
}

static void kb_key_label(const KbKey *k, char *out, size_t n)
{
    char c;
    if (!k || !out || n == 0) {
        return;
    }
    if (k->special == KB_SPEC_BKSP) {
        snprintf(out, n, "Bksp");
    } else if (k->special == KB_SPEC_CLEAR) {
        snprintf(out, n, "Clear");
    } else if (k->special == KB_SPEC_DONE) {
        snprintf(out, n, s_view == UI_VIEW_SEARCH ? "Done" : "Save");
    } else if (k->special == KB_SPEC_SHIFT) {
        snprintf(out, n, s_kb_shift ? "abc" : "ABC");
    } else if (k->special == KB_SPEC_SYMS) {
        snprintf(out, n, s_kb_sym ? "abc" : "123");
    } else if (k->special == KB_SPEC_STEAM) {
        snprintf(out, n, "Steam Keyboard");
    } else if (k->ch == ' ') {
        snprintf(out, n, "Space");
    } else {
        c = k->ch;
        if (!s_kb_sym && c >= 'a' && c <= 'z' && s_kb_shift) {
            c = (char)(c - 'a' + 'A');
        }
        out[0] = c;
        out[1] = '\0';
    }
}

static int list_vis_rows(void)
{
    float row_h = 72.f * s_scale;
    float top = 110.f * s_scale;
    float kb_reserve = 0.f;
    int vis;
    if (kb_on()) {
        float fx, fy, fw, fh;
        kb_field_geom(&fx, &fy, &fw, &fh);
        top = fy + fh + 16.f * s_scale;
        kb_reserve = 330.f * s_scale;
    }
    vis = (int)(((float)s_win_h - top - 24.f * s_scale - kb_reserve) / row_h);
    if (kb_on()) {
        if (vis < 2) {
            vis = 2;
        }
    } else if (vis < 3) {
        vis = 3;
    }
    return vis;
}

static void add_radio_row(const char *line, const char *sub, const char *url, int64_t id)
{
    char marked[VIBE_NAME_MAX];
    const char *use = sub ? sub : "";
    if (s_nrows >= 0 && s_nrows < RADIO_URL_SLOTS) {
        snprintf(s_radio_play_url[s_nrows], sizeof(s_radio_play_url[0]), "%s", url ? url : "");
    }
    if (url && url[0] && library_radio_fav_has(url)) {
        if (use[0]) {
            snprintf(marked, sizeof(marked), "%s  ·  Favorite", use);
        } else {
            snprintf(marked, sizeof(marked), "Favorite");
        }
        use = marked;
    }
    add_row(line, use, id, UI_KIND_RADIO);
}

static void clamp_focus(void)
{
    int vis;
    if (s_nrows <= 0) {
        s_focus = 0;
        s_scroll = 0;
        return;
    }
    if (s_focus < 0) {
        s_focus = 0;
    }
    if (s_focus >= s_nrows) {
        s_focus = s_nrows - 1;
    }
    vis = list_vis_rows();
    if (s_focus < s_scroll) {
        s_scroll = s_focus;
    }
    if (s_focus >= s_scroll + vis) {
        s_scroll = s_focus - vis + 1;
    }
}

static void path_basename(const char *path, char *out, size_t n)
{
    const char *slash;
    if (!out || n == 0) {
        return;
    }
    if (!path || !path[0] || (path[0] == '/' && path[1] == '\0')) {
        snprintf(out, n, "/");
        return;
    }
    slash = strrchr(path, '/');
    snprintf(out, n, "%s", (slash && slash[1]) ? slash + 1 : path);
}

static void music_folder_sub(const struct App *app, char *out, size_t n)
{
    if (!out || n == 0) {
        return;
    }
    if (!app || app->cfg.music_dir_count <= 0) {
        snprintf(out, n, "Not set");
        return;
    }
    if (app->cfg.music_dir_count == 1) {
        snprintf(out, n, "%s", app->cfg.music_dirs[0]);
        return;
    }
    snprintf(out, n, "%d folders", app->cfg.music_dir_count);
}

static void apply_music_dirs(struct App *app)
{
    if (!app) {
        return;
    }
    app->cfg.music_user = 1;
    library_set_scan_roots(app->cfg.music_dirs, app->cfg.music_dir_count);
    decode_request_scan();
    config_save(&app->cfg);
    snprintf(app->status, sizeof(app->status), "Scanning…");
}

static int dir_name_cmp(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

static void fill_dirents(void)
{
    DIR *d;
    struct dirent *de;
    s_ndirents = 0;
    d = opendir(s_browse[0] ? s_browse : "/");
    if (!d) {
        return;
    }
    while ((de = readdir(d)) != NULL && s_ndirents < VIBE_LIST_MAX) {
        char full[VIBE_PATH_MAX];
        struct stat st;
        if (de->d_name[0] == '.') {
            continue;
        }
        if (strcmp(s_browse, "/") == 0) {
            snprintf(full, sizeof(full), "/%s", de->d_name);
        } else {
            snprintf(full, sizeof(full), "%s/%s", s_browse, de->d_name);
        }
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        snprintf(s_dirents[s_ndirents], VIBE_NAME_MAX, "%s", de->d_name);
        s_ndirents++;
    }
    closedir(d);
    if (s_ndirents > 1) {
        qsort(s_dirents, (size_t)s_ndirents, VIBE_NAME_MAX, dir_name_cmp);
    }
}

static void browse_parent(void)
{
    char *slash;
    if (strcmp(s_browse, "/") == 0) {
        return;
    }
    slash = strrchr(s_browse, '/');
    if (!slash) {
        snprintf(s_browse, sizeof(s_browse), "/");
        return;
    }
    if (slash == s_browse) {
        s_browse[1] = '\0';
        return;
    }
    *slash = '\0';
}

static int browse_enter(const char *name)
{
    char next[VIBE_PATH_MAX];
    struct stat st;
    if (!name || !name[0]) {
        return -1;
    }
    if (strcmp(s_browse, "/") == 0) {
        snprintf(next, sizeof(next), "/%s", name);
    } else {
        snprintf(next, sizeof(next), "%s/%s", s_browse, name);
    }
    if (stat(next, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return -1;
    }
    snprintf(s_browse, sizeof(s_browse), "%s", next);
    return 0;
}

static void open_dir_browser(const char *start)
{
    struct stat st;
    const char *home;
    s_browse[0] = '\0';
    if (start && start[0] && stat(start, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(s_browse, sizeof(s_browse), "%s", start);
    } else {
        home = getenv("HOME");
        if (home && home[0] && stat(home, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(s_browse, sizeof(s_browse), "%s", home);
        } else {
            snprintf(s_browse, sizeof(s_browse), "/");
        }
    }
    push_view(UI_VIEW_DIR_BROWSER);
}

void ui_refresh_lists(struct App *app)
{
    char sub[160];
    s_nrows = 0;
    (void)app;
    library_queue_cover_refresh();
    switch (s_view) {
    case UI_VIEW_LIBRARY: {
        LibTrack resume;
        int pos = 0;
        char rtitle[VIBE_NAME_MAX], rurl[VIBE_PATH_MAX], rsub[VIBE_NAME_MAX];
        if (library_resume_radio(rtitle, (int)sizeof(rtitle), rurl, (int)sizeof(rurl),
                                 rsub, (int)sizeof(rsub)) == 0 && rurl[0]) {
            add_row("Resume", rtitle[0] ? rtitle : "Internet Radio", 1, UI_KIND_ACTION);
        } else {
            add_row("Resume",
                    library_resume_track(&resume, &pos) == 0 ? resume.title : "Nothing to resume",
                    1, UI_KIND_ACTION);
        }
        snprintf(sub, sizeof(sub), "%d artists", library_artist_count());
        add_row("Artists", sub, 4, UI_KIND_ACTION);
        snprintf(sub, sizeof(sub), "%d albums", library_album_count());
        add_row("Albums", sub, 3, UI_KIND_ACTION);
        snprintf(sub, sizeof(sub), "%d folders", library_folder_count());
        add_row("Folders", sub, 5, UI_KIND_ACTION);
        add_row("Internet Radio", "Soma.fm, Radio Browser, Custom", 10, UI_KIND_ACTION);
        snprintf(sub, sizeof(sub), "%d playlists", library_playlist_count());
        add_row("Playlists", sub, 11, UI_KIND_ACTION);
        snprintf(sub, sizeof(sub), "%d tracks", library_queue_len());
        add_row("Current Queue", sub, 8, UI_KIND_ACTION);
        add_row("Search", "", 6, UI_KIND_ACTION);
        add_row("Settings", "", 7, UI_KIND_ACTION);
        add_row("Exit", "Leave StOMP", 9, UI_KIND_ACTION);
        break;
    }
    case UI_VIEW_ALBUMS: {
        int n;
        snprintf(sub, sizeof(sub), "%d albums", library_album_fav_count());
        add_row("Favorites", sub, 1, UI_KIND_ACTION);
        n = library_list_albums(s_qbuf.albums, VIBE_LIST_MAX, 0);
        for (int i = 0; i < n; i++) {
            if (library_album_fav_has(s_qbuf.albums[i].id)) {
                snprintf(sub, sizeof(sub), "%s  ·  %d tracks  ·  Favorite",
                         s_qbuf.albums[i].artist, s_qbuf.albums[i].track_count);
            } else {
                snprintf(sub, sizeof(sub), "%s  ·  %d tracks",
                         s_qbuf.albums[i].artist, s_qbuf.albums[i].track_count);
            }
            fmt_queue_cover(sub, sizeof(sub), sub, library_queue_cover_album(s_qbuf.albums[i].id));
            add_row(s_qbuf.albums[i].name, sub, s_qbuf.albums[i].id, UI_KIND_ALBUM);
        }
        break;
    }
    case UI_VIEW_ALBUM_FAVORITES: {
        int n = library_album_fav_list(s_qbuf.albums, VIBE_LIST_MAX);
        for (int i = 0; i < n; i++) {
            snprintf(sub, sizeof(sub), "%s  ·  %d tracks",
                     s_qbuf.albums[i].artist, s_qbuf.albums[i].track_count);
            fmt_queue_cover(sub, sizeof(sub), sub, library_queue_cover_album(s_qbuf.albums[i].id));
            add_row(s_qbuf.albums[i].name, sub, s_qbuf.albums[i].id, UI_KIND_ALBUM);
        }
        if (n == 0) {
            add_row("No favorites yet", "Hold A on an album to save it", 0, UI_KIND_ACTION);
        }
        break;
    }
    case UI_VIEW_ALBUM_TRACKS: {
        int n = library_album_tracks(s_ctx_album, s_qbuf.tracks, VIBE_LIST_MAX);
        for (int i = 0; i < n; i++) {
            add_track_row(s_qbuf.tracks[i].title, s_qbuf.tracks[i].artist, s_qbuf.tracks[i].id);
        }
        break;
    }
    case UI_VIEW_ARTISTS: {
        int n;
        snprintf(sub, sizeof(sub), "%d artists", library_artist_fav_count());
        add_row("Favorites", sub, 1, UI_KIND_ACTION);
        n = library_list_artists(s_qbuf.artists, VIBE_LIST_MAX, 0);
        for (int i = 0; i < n; i++) {
            if (library_artist_fav_has(s_qbuf.artists[i].id)) {
                snprintf(sub, sizeof(sub), "%d albums  ·  Favorite", s_qbuf.artists[i].album_count);
            } else {
                snprintf(sub, sizeof(sub), "%d albums", s_qbuf.artists[i].album_count);
            }
            fmt_queue_cover(sub, sizeof(sub), sub, library_queue_cover_artist(s_qbuf.artists[i].id));
            add_row(s_qbuf.artists[i].name, sub, s_qbuf.artists[i].id, UI_KIND_ARTIST);
        }
        break;
    }
    case UI_VIEW_ARTIST_FAVORITES: {
        int n = library_artist_fav_list(s_qbuf.artists, VIBE_LIST_MAX);
        for (int i = 0; i < n; i++) {
            snprintf(sub, sizeof(sub), "%d albums", s_qbuf.artists[i].album_count);
            fmt_queue_cover(sub, sizeof(sub), sub, library_queue_cover_artist(s_qbuf.artists[i].id));
            add_row(s_qbuf.artists[i].name, sub, s_qbuf.artists[i].id, UI_KIND_ARTIST);
        }
        if (n == 0) {
            add_row("No favorites yet", "Hold A on an artist to save it", 0, UI_KIND_ACTION);
        }
        break;
    }
    case UI_VIEW_ARTIST_ALBUMS: {
        int n = library_artist_albums(s_ctx_artist, s_qbuf.albums, VIBE_LIST_MAX);
        for (int i = 0; i < n; i++) {
            if (library_album_fav_has(s_qbuf.albums[i].id)) {
                snprintf(sub, sizeof(sub), "%d tracks  ·  Favorite", s_qbuf.albums[i].track_count);
            } else {
                snprintf(sub, sizeof(sub), "%d tracks", s_qbuf.albums[i].track_count);
            }
            fmt_queue_cover(sub, sizeof(sub), sub, library_queue_cover_album(s_qbuf.albums[i].id));
            add_row(s_qbuf.albums[i].name, sub, s_qbuf.albums[i].id, UI_KIND_ALBUM);
        }
        break;
    }
    case UI_VIEW_FOLDER_TRACKS:
    case UI_VIEW_FOLDERS: {
        int n, i;
        n = library_list_folders(s_ctx_folder, s_qbuf.folders, VIBE_LIST_MAX);
        for (i = 0; i < n; i++) {
            if (s_qbuf.folders[i].child_count > 0) {
                snprintf(sub, sizeof(sub), "%d tracks", s_qbuf.folders[i].child_count);
            } else {
                snprintf(sub, sizeof(sub), "Folder");
            }
            fmt_queue_cover(sub, sizeof(sub), sub, library_queue_cover_folder(s_qbuf.folders[i].id));
            add_row(s_qbuf.folders[i].name, sub, s_qbuf.folders[i].id, UI_KIND_FOLDER);
        }
        n = library_folder_direct_tracks(s_ctx_folder, s_qbuf.tracks, VIBE_LIST_MAX);
        for (i = 0; i < n; i++) {
            add_track_row(s_qbuf.tracks[i].title, s_qbuf.tracks[i].artist, s_qbuf.tracks[i].id);
        }
        break;
    }
    case UI_VIEW_PLAYLISTS: {
        int n;
        snprintf(sub, sizeof(sub), "%d playlists", library_playlist_fav_count());
        add_row("Favorites", sub, 1, UI_KIND_ACTION);
        n = library_list_playlists(s_qbuf.playlists, VIBE_LIST_MAX);
        for (int i = 0; i < n; i++) {
            if (library_playlist_fav_has(s_qbuf.playlists[i].id)) {
                snprintf(sub, sizeof(sub), "%d tracks  ·  Favorite", s_qbuf.playlists[i].item_count);
            } else {
                snprintf(sub, sizeof(sub), "%d tracks", s_qbuf.playlists[i].item_count);
            }
            fmt_queue_cover(sub, sizeof(sub), sub, library_queue_cover_playlist(s_qbuf.playlists[i].id));
            add_row(s_qbuf.playlists[i].name, sub, s_qbuf.playlists[i].id, UI_KIND_PLAYLIST);
        }
        if (n == 0) {
            add_row("No playlist files", "Put .m3u, .pls, .xspf, .wpl, .asx, or .cue in your music folders", 0, UI_KIND_ACTION);
        }
        break;
    }
    case UI_VIEW_PLAYLIST_FAVORITES: {
        int n = library_playlist_fav_list(s_qbuf.playlists, VIBE_LIST_MAX);
        for (int i = 0; i < n; i++) {
            snprintf(sub, sizeof(sub), "%d tracks", s_qbuf.playlists[i].item_count);
            fmt_queue_cover(sub, sizeof(sub), sub, library_queue_cover_playlist(s_qbuf.playlists[i].id));
            add_row(s_qbuf.playlists[i].name, sub, s_qbuf.playlists[i].id, UI_KIND_PLAYLIST);
        }
        if (n == 0) {
            add_row("No favorites yet", "Hold A on a playlist to save it", 0, UI_KIND_ACTION);
        }
        break;
    }
    case UI_VIEW_PLAYLIST_TRACKS: {
        int n = library_playlist_tracks(s_ctx_playlist, s_qbuf.tracks, VIBE_LIST_MAX);
        if (n == 0) {
            add_row("No matching files", "Paths, filenames, and titles were checked against the library", 0, UI_KIND_ACTION);
        }
        for (int i = 0; i < n; i++) {
            add_track_row(s_qbuf.tracks[i].title, s_qbuf.tracks[i].artist, s_qbuf.tracks[i].id);
        }
        break;
    }
    case UI_VIEW_QUEUE: {
        int n = library_queue_len();
        add_row("Queue Options", "Clear, save as playlist", 1, UI_KIND_ACTION);
        if (n == 0) {
            add_row("Queue is empty", "Hold Y on a song, album, artist, or folder to add it", 0, UI_KIND_ACTION);
        }
        for (int i = 0; i < n; i++) {
            LibTrack t;
            if (library_queue_get(i, &t) != 0) {
                continue;
            }
            snprintf(sub, sizeof(sub), "%s%s", t.artist, i == library_queue_index() ? "  ·  now" : "");
            add_row(t.title, sub, t.id, UI_KIND_TRACK);
        }
        break;
    }
    case UI_VIEW_QUEUE_OPTIONS:
        add_row("Clear Queue", "Remove every song and stop playback", 1, UI_KIND_ACTION);
        add_row("Save Queue to Playlist", "Write an .m3u in your music folder", 2, UI_KIND_ACTION);
        break;
    case UI_VIEW_QUEUE_SAVE:
        s_show_kb = 1;
        break;
    case UI_VIEW_SEARCH: {
        int n = 0;
        if (s_search[0]) {
            n = library_search(s_search, s_qbuf.tracks, 200);
        }
        for (int i = 0; i < n; i++) {
            snprintf(sub, sizeof(sub), "%s  ·  %s", s_qbuf.tracks[i].artist, s_qbuf.tracks[i].album);
            add_track_row(s_qbuf.tracks[i].title, sub, s_qbuf.tracks[i].id);
        }
        s_show_kb = 1;
        break;
    }
    case UI_VIEW_SETTINGS: {
        music_folder_sub(app, sub, sizeof(sub));
        add_row("Music folder", sub, 8, UI_KIND_SETTING);
        add_row("Rescan library", "Walk selected folders", 1, UI_KIND_SETTING);
        snprintf(sub, sizeof(sub), "%s", app->cfg.audio_device[0] ? app->cfg.audio_device : "Default");
        add_row("Audio device", sub, 2, UI_KIND_SETTING);
        add_row("Preset shuffle", viz_shuffle() ? "On" : "Off", 4, UI_KIND_SETTING);
        add_row("Visualization lock", viz_locked() ? "Locked" : "Unlocked", 5, UI_KIND_SETTING);
        add_row("Visualization pool", viz_pool_label(viz_pool()), 9, UI_KIND_SETTING);
        add_row("Start view", app->cfg.start_library ? "Library" : "Now playing", 6, UI_KIND_SETTING);
        add_row("Controller layout", "Customize or reset", 10, UI_KIND_SETTING);
        break;
    }
    case UI_VIEW_BINDING: {
        const int *binds = input_binds();
        add_row("Reset layout", "Restore default buttons", 1000, UI_KIND_ACTION);
        for (int i = 0; i < VIBE_BIND_COUNT; i++) {
            if (s_bind_capture && s_bind_target == i) {
                add_row(input_binding_name((VibeBind)i), "Press a button…", i, UI_KIND_ACTION);
            } else {
                add_row(input_binding_name((VibeBind)i), input_button_name(binds[i]), i, UI_KIND_ACTION);
            }
        }
        break;
    }
    case UI_VIEW_MUSIC_DIRS: {
        int i;
        add_row("Add folder", "Browse the filesystem", 1, UI_KIND_ACTION);
        if (!app || app->cfg.music_dir_count == 0) {
            add_row("No music folder yet", "A on Add folder", 0, UI_KIND_ACTION);
        }
        for (i = 0; app && i < app->cfg.music_dir_count; i++) {
            char name[VIBE_NAME_MAX];
            path_basename(app->cfg.music_dirs[i], name, sizeof(name));
            snprintf(sub, sizeof(sub), "Y  remove  ·  %s", app->cfg.music_dirs[i]);
            add_row(name, sub, 100 + i, UI_KIND_ACTION);
        }
        break;
    }
    case UI_VIEW_DIR_BROWSER: {
        int i;
        fill_dirents();
        add_row("Use this folder", s_browse[0] ? s_browse : "/", 1, UI_KIND_ACTION);
        if (strcmp(s_browse, "/") != 0) {
            add_row("..", "Parent folder", 2, UI_KIND_ACTION);
        }
        for (i = 0; i < s_ndirents; i++) {
            add_row(s_dirents[i], "", 10 + i, UI_KIND_ACTION);
        }
        if (s_ndirents == 0) {
            add_row("No subfolders", "", 0, UI_KIND_ACTION);
        }
        break;
    }
    case UI_VIEW_PRESETS: {
        int n = viz_preset_count();
        int cur = viz_current_index();
        char name[VIBE_NAME_MAX];
        char cat[VIBE_NAME_MAX];
        if (n <= 0) {
            add_row("No presets", "", 0, UI_KIND_ACTION);
            break;
        }
        for (int i = 0; i < n; i++) {
            int pi = viz_sorted_at(i);
            viz_preset_name(pi, name, sizeof(name));
            viz_preset_cat(pi, cat, sizeof(cat));
            if (pi == cur) {
                char nowsub[192];
                snprintf(nowsub, sizeof(nowsub), "%s  ·  now", cat);
                add_row(name, nowsub, pi, UI_KIND_PRESET);
            } else {
                add_row(name, cat, pi, UI_KIND_PRESET);
            }
        }
        break;
    }
    case UI_VIEW_RADIO:
        snprintf(sub, sizeof(sub), "%d stations", library_radio_fav_count());
        add_row("Favorites", sub, 4, UI_KIND_ACTION);
        add_row("Soma.fm Channel", "Independent listener-supported radio", 1, UI_KIND_ACTION);
        add_row("Radio Browser", "Stations by country, most popular first", 2, UI_KIND_ACTION);
        add_row("Custom", "Your saved streams", 3, UI_KIND_ACTION);
        break;
    case UI_VIEW_RADIO_FAVORITES: {
        LibRadioCustom fv[512];
        int n, i;
        n = library_radio_fav_list(fv, 512);
        for (i = 0; i < n; i++) {
            add_radio_row(fv[i].name, "Favorite", fv[i].url, fv[i].id);
        }
        if (n == 0) {
            add_row("No favorites yet", "Hold A on a station to save it", 0, UI_KIND_ACTION);
        }
        break;
    }
    case UI_VIEW_RADIO_SOMA: {
        int n, i;
        if (s_radio_err[0] && radio_soma_count() == 0) {
            add_row(s_radio_err[0] ? s_radio_err : "Failed", "Back to retry", 0, UI_KIND_ACTION);
            break;
        }
        n = radio_soma_count();
        for (i = 0; i < n; i++) {
            RadioItem it;
            if (radio_soma_at(i, &it) == 0) {
                add_radio_row(it.name, it.sub, it.url, i);
            }
        }
        break;
    }
    case UI_VIEW_RADIO_COUNTRIES: {
        int n, i;
        if (s_radio_err[0] && radio_country_count() == 0) {
            add_row(s_radio_err, "Back to retry", 0, UI_KIND_ACTION);
            break;
        }
        n = radio_country_count();
        for (i = 0; i < n; i++) {
            RadioCountry c;
            if (radio_country_at(i, &c) == 0) {
                snprintf(sub, sizeof(sub), "%d stations", c.stationcount);
                add_row(c.name, sub, i, UI_KIND_ACTION);
            }
        }
        break;
    }
    case UI_VIEW_RADIO_STATIONS: {
        int n, i;
        if (s_radio_err[0] && radio_station_count() == 0) {
            add_row(s_radio_err, "Back to retry", 0, UI_KIND_ACTION);
            break;
        }
        n = radio_station_count();
        for (i = 0; i < n; i++) {
            RadioItem it;
            if (radio_station_at(i, &it) == 0) {
                add_radio_row(it.name, it.sub, it.url, i);
            }
        }
        break;
    }
    case UI_VIEW_RADIO_CUSTOM: {
        LibRadioCustom cu[512];
        int n, i;
        add_row("Add station", "Type a stream URL or playlist file", 1, UI_KIND_ACTION);
        n = library_radio_custom_list(cu, 512);
        for (i = 0; i < n; i++) {
            add_radio_row(cu[i].name, cu[i].url, cu[i].url, cu[i].id);
        }
        if (n == 0) {
            add_row("No saved stations", "Add a URL to keep it here, A–Z", 0, UI_KIND_ACTION);
        }
        break;
    }
    case UI_VIEW_RADIO_ADD:
        s_show_kb = 1;
        break;
    default:
        break;
    }
    clamp_focus();
}

static void emit(float x, float y, float u, float v, float r, float g, float b, float a)
{
    Vtx *p;
    if (s_nv >= UI_MAX_VTX) {
        return;
    }
    p = &s_vtx[s_nv++];
    p->x = x;
    p->y = y;
    p->u = u;
    p->v = v;
    p->r = r;
    p->g = g;
    p->b = b;
    p->a = a;
}

static void quad(float x, float y, float w, float h,
                 float u0, float v0, float u1, float v1,
                 float r, float g, float b, float a)
{
    emit(x, y, u0, v0, r, g, b, a);
    emit(x + w, y, u1, v0, r, g, b, a);
    emit(x, y + h, u0, v1, r, g, b, a);
    emit(x + w, y, u1, v0, r, g, b, a);
    emit(x + w, y + h, u1, v1, r, g, b, a);
    emit(x, y + h, u0, v1, r, g, b, a);
}

static void quad_shear(float x, float y, float w, float h, float baseline, float shear,
                       float u0, float v0, float u1, float v1,
                       float r, float g, float b, float a)
{
    float x0 = x + (baseline - y) * shear;
    float x1 = x + w + (baseline - y) * shear;
    float x2 = x + (baseline - (y + h)) * shear;
    float x3 = x + w + (baseline - (y + h)) * shear;
    emit(x0, y, u0, v0, r, g, b, a);
    emit(x1, y, u1, v0, r, g, b, a);
    emit(x2, y + h, u0, v1, r, g, b, a);
    emit(x1, y, u1, v0, r, g, b, a);
    emit(x3, y + h, u1, v1, r, g, b, a);
    emit(x2, y + h, u0, v1, r, g, b, a);
}

static void flush(int mode, unsigned tex)
{
    if (s_nv <= 0) {
        return;
    }
    glUseProgram(s_prog);
    glUniform2f(s_u_res, (float)s_win_w, (float)s_win_h);
    glUniform1i(s_u_mode, mode);
    glUniform1i(s_u_tex, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex ? tex : s_white);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)((size_t)s_nv * sizeof(Vtx)), s_vtx);
    glDrawArrays(GL_TRIANGLES, 0, s_nv);
    s_nv = 0;
}

static void draw_rect(float x, float y, float w, float h, float r, float g, float b, float a)
{
    quad(x, y, w, h, 0, 0, 1, 1, r, g, b, a);
}

static void draw_tri(float x0, float y0, float x1, float y1, float x2, float y2,
                     float r, float g, float b, float a)
{
    emit(x0, y0, 0, 0, r, g, b, a);
    emit(x1, y1, 0, 0, r, g, b, a);
    emit(x2, y2, 0, 0, r, g, b, a);
}

static float measure_text(const char *s, float scale)
{
    float pen = 0.f;
    float sc = scale * s_scale;
    const char *p = s;
    if (!s || !s_font_ok) {
        return 0.f;
    }
    while (*p) {
        uint32_t cp = utf8_next(&p);
        Glyph *gl = glyph_get(cp);
        if (gl) {
            pen += gl->adv * sc;
        }
    }
    return pen;
}

static void fit_text(char *dst, size_t n, const char *src, float max_w, float scale)
{
    const char *p;
    size_t used = 0;
    if (!dst || n == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }
    if (measure_text(src, scale) <= max_w) {
        snprintf(dst, n, "%s", src);
        return;
    }
    p = src;
    while (*p && used + 4 < n) {
        const char *prev = p;
        char trial[512];
        size_t glen;
        utf8_next(&p);
        glen = (size_t)(p - prev);
        if (used + glen + 4 >= n || used + glen + 4 >= sizeof(trial)) {
            break;
        }
        memcpy(trial, src, used + glen);
        trial[used + glen] = '.';
        trial[used + glen + 1] = '.';
        trial[used + glen + 2] = '.';
        trial[used + glen + 3] = '\0';
        if (measure_text(trial, scale) > max_w) {
            break;
        }
        used += glen;
    }
    if (used == 0) {
        snprintf(dst, n, "...");
        return;
    }
    memcpy(dst, src, used);
    if (used + 4 <= n) {
        memcpy(dst + used, "...", 4);
    } else {
        dst[used] = '\0';
    }
}

static void draw_text_ex(float x, float y, const char *s, float scale,
                         float r, float g, float b, float a, int italic)
{
    float pen = x;
    float sc = scale * s_scale;
    float shear = italic ? 0.22f : 0.f;
    const char *p = s;
    if (!s || !s_font_ok) {
        return;
    }
    while (*p) {
        uint32_t cp = utf8_next(&p);
        Glyph *gl = glyph_get(cp);
        float gx, gy, u0, v0, u1, v1;
        if (!gl) {
            continue;
        }
        if (gl->bw >= 1.f && gl->bh >= 1.f) {
            gx = pen + gl->ax * sc;
            gy = y - gl->ay * sc;
            u0 = (float)gl->atlas_x / (float)ATLAS_W;
            v0 = (float)gl->atlas_y / (float)ATLAS_H;
            u1 = (float)(gl->atlas_x + (int)gl->bw) / (float)ATLAS_W;
            v1 = (float)(gl->atlas_y + (int)gl->bh) / (float)ATLAS_H;
            if (shear != 0.f) {
                quad_shear(gx, gy, gl->bw * sc, gl->bh * sc, y, shear,
                           u0, v0, u1, v1, r, g, b, a);
            } else {
                quad(gx, gy, gl->bw * sc, gl->bh * sc, u0, v0, u1, v1, r, g, b, a);
            }
        }
        pen += gl->adv * sc;
    }
}

static void draw_text(float x, float y, const char *s, float scale, float r, float g, float b, float a)
{
    draw_text_ex(x, y, s, scale, r, g, b, a, 0);
}

static void flush_color(void)
{
    flush(0, s_white);
}

static void flush_font(void)
{
    flush(1, s_atlas);
}

static void fmt_time(char *buf, size_t n, double sec)
{
    int s, m;
    if (sec < 0) {
        sec = 0;
    }
    s = (int)sec;
    m = s / 60;
    s = s % 60;
    snprintf(buf, n, "%d:%02d", m, s);
}

static void draw_np_chip(float x, float y, float w, float h, int focused, const char *label)
{
    float sc = 0.6f;
    float tw;
    float tx;
    float ty;
    if (focused) {
        draw_rect(x, y, w, h, 1, 1, 1, 0.18f);
        draw_rect(x, y, 5.f * s_scale, h, 1, 1, 1, 0.95f);
    } else {
        draw_rect(x, y, w, h, 0, 0, 0, 0.22f);
    }
    flush_color();
    tw = measure_text(label ? label : "", sc);
    tx = x + (w - tw) * 0.5f;
    ty = y + h * 0.5f + 8.f * s_scale;
    draw_text(tx, ty, label ? label : "", sc, 0.92f, 0.92f, 0.92f, 1.f);
    flush_font();
}

static void draw_np_square(float x, float y, float s, int focused)
{
    if (focused) {
        draw_rect(x, y, s, s, 1, 1, 1, 0.28f);
    } else {
        draw_rect(x, y, s, s, 0, 0, 0, 0.22f);
    }
}

static void icon_prev(float x, float y, float s)
{
    float p = 9.f * s_scale;
    float bar = 3.5f * s_scale;
    float ir = 0.92f, ig = 0.92f, ib = 0.92f;
    float left = x + p;
    float top = y + p;
    float bot = y + s - p;
    float mid = y + s * 0.5f;
    float tip = x + s - p;
    draw_rect(left, top, bar, bot - top, ir, ig, ib, 1.f);
    draw_tri(tip, top, left + bar + 1.5f * s_scale, mid, tip, bot, ir, ig, ib, 1.f);
}

static void icon_next(float x, float y, float s)
{
    float p = 9.f * s_scale;
    float bar = 3.5f * s_scale;
    float ir = 0.92f, ig = 0.92f, ib = 0.92f;
    float top = y + p;
    float bot = y + s - p;
    float mid = y + s * 0.5f;
    float right = x + s - p;
    draw_rect(right - bar, top, bar, bot - top, ir, ig, ib, 1.f);
    draw_tri(x + p, top, right - bar - 1.5f * s_scale, mid, x + p, bot, ir, ig, ib, 1.f);
}

static void icon_play(float x, float y, float s)
{
    float p = 10.f * s_scale;
    float ir = 0.92f, ig = 0.92f, ib = 0.92f;
    draw_tri(x + p + 2.f * s_scale, y + p,
             x + s - p, y + s * 0.5f,
             x + p + 2.f * s_scale, y + s - p,
             ir, ig, ib, 1.f);
}

static void icon_pause(float x, float y, float s)
{
    float p = 10.f * s_scale;
    float bw = 5.f * s_scale;
    float gap = 5.f * s_scale;
    float x0 = x + (s - (bw * 2.f + gap)) * 0.5f;
    float ir = 0.92f, ig = 0.92f, ib = 0.92f;
    draw_rect(x0, y + p, bw, s - 2.f * p, ir, ig, ib, 1.f);
    draw_rect(x0 + bw + gap, y + p, bw, s - 2.f * p, ir, ig, ib, 1.f);
}

static void icon_stop(float x, float y, float s)
{
    float inner = s * 0.38f;
    float ir = 0.92f, ig = 0.92f, ib = 0.92f;
    draw_rect(x + (s - inner) * 0.5f, y + (s - inner) * 0.5f, inner, inner, ir, ig, ib, 1.f);
}

static void icon_speaker_wave(float cx, float cy, float r)
{
    float ir = 0.92f, ig = 0.92f, ib = 0.92f;
    float a0 = -0.60f, a1 = 0.60f;
    float t = 4.5f * s_scale;
    const int N = 4;
    for (int i = 0; i < N; i++) {
        float pa = a0 + (a1 - a0) * (float)i / (float)N;
        float pb = a0 + (a1 - a0) * (float)(i + 1) / (float)N;
        float c0 = cosf(pa), s0 = sinf(pa);
        float c1 = cosf(pb), s1 = sinf(pb);
        float x0 = cx + r * c0, y0 = cy + r * s0;
        float x1 = cx + r * c1, y1 = cy + r * s1;
        float x2 = cx + (r + t) * c1, y2 = cy + (r + t) * s1;
        float x3 = cx + (r + t) * c0, y3 = cy + (r + t) * s0;
        draw_tri(x0, y0, x1, y1, x2, y2, ir, ig, ib, 1.f);
        draw_tri(x0, y0, x2, y2, x3, y3, ir, ig, ib, 1.f);
    }
}

static void icon_speaker(float x, float y, float s)
{
    float p = 9.f * s_scale;
    float ir = 0.92f, ig = 0.92f, ib = 0.92f;
    float mid = y + s * 0.5f;
    float top = mid - s * 0.26f;
    float bot = mid + s * 0.26f;
    float bx = x + p;
    float bw = s * 0.24f;
    float txt = bx + bw;
    float tip = txt + s * 0.10f;
    draw_rect(bx, top, bw, bot - top, ir, ig, ib, 1.f);
    draw_tri(txt, mid, tip, top, tip, bot, ir, ig, ib, 1.f);
    float cx = tip + s * 0.02f;
    icon_speaker_wave(cx, mid, s * 0.13f);
    icon_speaker_wave(cx, mid, s * 0.22f);
}

static void icon_eye(float x, float y, float s, int on)
{
    float ir = 0.92f, ig = 0.92f, ib = 0.92f;
    float cx = x + s * 0.5f;
    float cy = y + s * 0.5f;
    float w = s * 0.38f;
    float h = s * 0.18f;
    if (on) {
        float pr = s * 0.10f;
        draw_tri(cx - w, cy, cx, cy - h, cx + w, cy, ir, ig, ib, 1.f);
        draw_tri(cx - w, cy, cx + w, cy, cx, cy + h, ir, ig, ib, 1.f);
        draw_rect(cx - pr * 0.5f, cy - pr * 0.5f, pr, pr, 0.08f, 0.10f, 0.16f, 1.f);
    } else {
        float lh = 3.f * s_scale;
        draw_rect(cx - w, cy - lh * 0.5f, w * 2.f, lh, ir, ig, ib, 1.f);
    }
}

static void draw_now_playing(struct App *app)
{
    float strip_h = 228.f * s_scale;
    float y = (float)s_win_h - strip_h;
    float bar_x = 80.f * s_scale;
    float bar_w = (float)s_win_w - 160.f * s_scale;
    float opt_y, bar_y;
    double dur = decode_duration();
    double pos = decode_position();
    float t = (dur > 0.1) ? (float)(pos / dur) : 0.f;
    char a[32], b[32];
    LibTrack next;
    int qn = library_queue_len();
    int qi = library_queue_index();

    if (t < 0.f) {
        t = 0.f;
    }
    if (t > 1.f) {
        t = 1.f;
    }
    draw_rect(0, y, (float)s_win_w, strip_h, 0.f, 0.f, 0.f, s_veil);
    flush_color();

    draw_text(bar_x, y + 44.f * s_scale,
              app->now_valid ? app->now.title : "StOMP",
              1.1f, 0.96f, 0.96f, 0.96f, 1.f);
    {
        const char *subln = "Open the library from the Start menu";
        char icy[VIBE_NAME_MAX];
        icy[0] = '\0';
        if (app->now_valid && (app->playing_radio || decode_is_live())) {
            decode_icy(icy, (int)sizeof(icy));
            subln = icy[0] ? icy : "Live";
        } else if (app->now_valid) {
            subln = app->now.artist;
        }
        draw_text(bar_x, y + 80.f * s_scale, subln, 0.7f, 0.75f, 0.75f, 0.75f, 1.f);
    }
    flush_font();

    opt_y = y + 98.f * s_scale;
    {
        char pname[VIBE_NAME_MAX];
        char shown[128];
        float chip_h = 36.f * s_scale;
        float sq = chip_h;
        float lock_w = 300.f * s_scale;
        float shuf_w = 150.f * s_scale;
        float rep_w = 150.f * s_scale;
        float gap = 12.f * s_scale;
        float sq_gap = 8.f * s_scale;
        float x_prev = bar_x;
        float x_play = x_prev + sq + sq_gap;
        float x_stop = x_play + sq + sq_gap;
        float x_next = x_stop + sq + sq_gap;
        float x_shuf = x_next + sq + gap;
        float x_rep = x_shuf + shuf_w + gap;
        float x_vol = x_rep + rep_w + gap;
        float left_end = x_vol + sq;
        float x_lock = bar_x + bar_w - lock_w;
        float x_eye = x_lock - gap - sq;
        float preset_w = x_eye - gap - left_end;
        float preset_x;
        if (preset_w < 140.f * s_scale) {
            preset_w = 140.f * s_scale;
        }
        preset_x = x_eye - gap - preset_w;
        viz_current_label(pname, sizeof(pname));
        if (!pname[0]) {
            snprintf(pname, sizeof(pname), "Preset");
        }
        fit_text(shown, sizeof(shown), pname, preset_w - 32.f * s_scale, 0.6f);
        draw_np_square(x_prev, opt_y, sq, s_np_opt == NP_OPT_PREV);
        icon_prev(x_prev, opt_y, sq);
        draw_np_square(x_play, opt_y, sq, s_np_opt == NP_OPT_PLAY);
        if (audio_paused()) {
            icon_play(x_play, opt_y, sq);
        } else {
            icon_pause(x_play, opt_y, sq);
        }
        draw_np_square(x_stop, opt_y, sq, s_np_opt == NP_OPT_STOP);
        icon_stop(x_stop, opt_y, sq);
        draw_np_square(x_next, opt_y, sq, s_np_opt == NP_OPT_NEXT);
        icon_next(x_next, opt_y, sq);
        flush_color();
        draw_np_chip(x_shuf, opt_y, shuf_w, chip_h, s_np_opt == NP_OPT_SHUFFLE,
                     app->shuffle ? "Shuffle  On" : "Shuffle  Off");
        draw_np_chip(x_rep, opt_y, rep_w, chip_h, s_np_opt == NP_OPT_REPEAT,
                     app->repeat == VIBE_REPEAT_ONE ? "Repeat  One" :
                     app->repeat == VIBE_REPEAT_ALL ? "Repeat  All" : "Repeat  Off");
        draw_np_square(x_vol, opt_y, sq, s_np_opt == NP_OPT_VOLUME);
        icon_speaker(x_vol, opt_y, sq);
        flush_color();
        draw_np_chip(preset_x, opt_y, preset_w, chip_h, s_np_opt == NP_OPT_PRESET, shown);
        draw_np_square(x_eye, opt_y, sq, s_np_opt == NP_OPT_EYE);
        icon_eye(x_eye, opt_y, sq, viz_enabled());
        flush_color();
        draw_np_chip(x_lock, opt_y, lock_w, chip_h, s_np_opt == NP_OPT_LOCK,
                     viz_locked() ? "Visualization Locked" : "Visualization Unlocked");
        if (s_vol_open) {
            float pw = 170.f * s_scale;
            float ph = 300.f * s_scale;
            float px = x_vol + (sq - pw) * 0.5f;
            float py = opt_y - ph - 18.f * s_scale;
            float tw = 12.f * s_scale;
            float tx = px + (pw - tw) * 0.5f;
            float ty = py + 48.f * s_scale;
            float th = ph - 62.f * s_scale;
            char vl[32];
            if (s_vol_draft < 0.f) {
                s_vol_draft = 0.f;
            }
            if (s_vol_draft > 1.f) {
                s_vol_draft = 1.f;
            }
            draw_rect(px, py, pw, ph, 0.f, 0.f, 0.f, 0.82f);
            draw_rect(tx, ty, tw, th, 1, 1, 1, 0.18f);
            draw_rect(tx, ty + th - th * s_vol_draft, tw, th * s_vol_draft, 0.95f, 0.95f, 0.95f, 0.95f);
            snprintf(vl, sizeof(vl), "Volume  %d%%", (int)(s_vol_draft * 100.f + 0.5f));
            flush_color();
            draw_text(px + (pw - measure_text(vl, 0.6f)) * 0.5f, py + 12.f * s_scale, vl, 0.6f, 0.95f, 0.95f, 0.95f, 1.f);
            flush_font();
        }
    }

    bar_y = y + 150.f * s_scale;
    draw_rect(bar_x, bar_y, bar_w, 8.f * s_scale, 1, 1, 1, 0.18f);
    draw_rect(bar_x, bar_y, bar_w * t, 8.f * s_scale, 0.95f, 0.95f, 0.95f, 0.95f);
    fmt_time(a, sizeof(a), pos);
    fmt_time(b, sizeof(b), dur);
    flush_color();
    {
        char tb[64];
        if (app->playing_radio || decode_is_live() || dur < 0.5) {
            snprintf(tb, sizeof(tb), "LIVE");
        } else {
            snprintf(tb, sizeof(tb), "%s  /  %s", a, b);
        }
        draw_text(bar_x, bar_y + 32.f * s_scale, tb, 0.55f, 0.8f, 0.8f, 0.8f, 1.f);
        if (app->status[0]) {
            draw_text(bar_x + bar_w * 0.55f, bar_y + 32.f * s_scale, app->status, 0.55f, 0.85f, 0.85f, 0.7f, 1.f);
        }
        if (qn > qi + 1 && library_queue_get(qi + 1, &next) == 0) {
            char peek[160];
            snprintf(peek, sizeof(peek), "Up next  %s", next.title);
            draw_text(bar_x + 180.f * s_scale, bar_y + 32.f * s_scale, peek, 0.55f, 0.7f, 0.7f, 0.7f, 1.f);
        }
        flush_font();
    }
}

static const char *heading(void)
{
    switch (s_view) {
    case UI_VIEW_LIBRARY:
        return "Library";
    case UI_VIEW_ALBUMS:
        return "Albums";
    case UI_VIEW_ALBUM_TRACKS:
        return "Album";
    case UI_VIEW_ARTISTS:
        return "Artists";
    case UI_VIEW_ARTIST_FAVORITES:
        return "Favorite artists";
    case UI_VIEW_ARTIST_ALBUMS:
        return "Artist";
    case UI_VIEW_ALBUM_FAVORITES:
        return "Favorite albums";
    case UI_VIEW_FOLDERS:
    case UI_VIEW_FOLDER_TRACKS:
        return s_folder_title[0] ? s_folder_title : "Folders";
    case UI_VIEW_PLAYLISTS:
        return "Playlists";
    case UI_VIEW_PLAYLIST_FAVORITES:
        return "Favorite playlists";
    case UI_VIEW_PLAYLIST_TRACKS:
        return "Playlist";
    case UI_VIEW_QUEUE:
        return "Queue";
    case UI_VIEW_QUEUE_OPTIONS:
        return "Queue Options";
    case UI_VIEW_QUEUE_SAVE:
        return "Save Playlist";
    case UI_VIEW_SEARCH:
        return "Search";
    case UI_VIEW_SETTINGS:
        return "Settings";
    case UI_VIEW_BINDING:
        return "Controller layout";
    case UI_VIEW_PRESETS: {
        static char h[64];
        snprintf(h, sizeof(h), "Presets  %d", viz_preset_count());
        return h;
    }
    case UI_VIEW_MUSIC_DIRS:
        return "Music folder";
    case UI_VIEW_DIR_BROWSER: {
        static char h[128];
        snprintf(h, sizeof(h), "%s", s_browse[0] ? s_browse : "/");
        return h;
    }
    case UI_VIEW_RADIO:
        return "Internet Radio";
    case UI_VIEW_RADIO_FAVORITES:
        return "Favorites";
    case UI_VIEW_RADIO_SOMA:
        return "Soma.fm";
    case UI_VIEW_RADIO_COUNTRIES:
        return "Radio Browser";
    case UI_VIEW_RADIO_STATIONS: {
        static char h[128];
        RadioCountry c;
        int i = 0;
        for (i = 0; i < radio_country_count(); i++) {
            if (radio_country_at(i, &c) == 0 && strcmp(c.code, s_radio_cc) == 0) {
                snprintf(h, sizeof(h), "%s", c.name);
                return h;
            }
        }
        return "Stations";
    }
    case UI_VIEW_RADIO_CUSTOM:
        return "Custom";
    case UI_VIEW_RADIO_ADD:
        return "Add station";
    default:
        return "";
    }
}

static int uses_letter_jump(void);
static uint32_t row_code(int i);

static void draw_kb_field(void)
{
    float x, y, w, h, pad, text_sc, tw, caret_x, scroll;
    float br, bg, bb;
    const char *buf = kb_buf();
    const char *shown;
    char prefix[VIBE_PATH_MAX];
    int field_hot = s_kb_active && s_kb_zone == KB_ZONE_FIELD;
    int blink = ((SDL_GetTicks() / 530) & 1) == 0;
    int dw = 0, dh = 0;
    float sx, sy;
    int gx, gy, gw, gh;
    size_t n, caret;
    kb_field_geom(&x, &y, &w, &h);
    kb_clamp_caret();
    caret = s_kb_caret;
    n = strlen(buf);
    if (caret > n) {
        caret = n;
    }
    if (caret >= sizeof(prefix)) {
        caret = sizeof(prefix) - 1;
    }
    memcpy(prefix, buf, caret);
    prefix[caret] = '\0';
    pad = 18.f * s_scale;
    text_sc = 0.95f;
    br = field_hot ? 0.10f : 0.55f;
    bg = field_hot ? 0.10f : 0.55f;
    bb = field_hot ? 0.10f : 0.55f;
    {
        float b = field_hot ? 4.f * s_scale : 2.f * s_scale;
        draw_rect(x - b, y - b, w + 2.f * b, h + 2.f * b, br, bg, bb, 1.f);
        draw_rect(x, y, w, h, 1.f, 1.f, 1.f, 1.f);
        flush_color();
    }
    shown = (buf[0] != '\0') ? buf : kb_placeholder();
    tw = measure_text(prefix, text_sc);
    scroll = 0.f;
    if (tw > w - pad * 2.f - 8.f * s_scale) {
        scroll = tw - (w - pad * 2.f - 8.f * s_scale);
    }
    plat_drawable_size(&dw, &dh);
    if (dw <= 0) {
        dw = s_win_w;
    }
    if (dh <= 0) {
        dh = s_win_h;
    }
    sx = (float)dw / (float)s_win_w;
    sy = (float)dh / (float)s_win_h;
    gx = (int)(x * sx);
    gy = (int)(((float)s_win_h - (y + h)) * sy);
    gw = (int)(w * sx);
    gh = (int)(h * sy);
    if (gw < 1) {
        gw = 1;
    }
    if (gh < 1) {
        gh = 1;
    }
    glEnable(GL_SCISSOR_TEST);
    glScissor(gx, gy, gw, gh);
    if (buf[0]) {
        draw_text(x + pad - scroll, y + h * 0.66f, shown, text_sc, 0.10f, 0.10f, 0.10f, 1.f);
    } else {
        draw_text(x + pad, y + h * 0.66f, shown, text_sc, 0.45f, 0.45f, 0.48f, 1.f);
    }
    flush_font();
    caret_x = x + pad - scroll + tw;
    if (s_kb_active && (field_hot || blink)) {
        draw_rect(caret_x, y + 14.f * s_scale, 3.f * s_scale, h - 28.f * s_scale,
                  0.10f, 0.10f, 0.12f, field_hot ? 1.f : 0.85f);
        flush_color();
    }
    glDisable(GL_SCISSOR_TEST);
}

static void draw_keyboard(void)
{
    const KbKey *keys = kb_cur();
    int n = kb_count();
    float cell = 56.f * s_scale;
    float kb_w = (float)KB_COLS * cell;
    float kx = ((float)s_win_w - kb_w) * 0.5f;
    float ky = (float)s_win_h - 318.f * s_scale;
    float row_h = 48.f * s_scale;
    int i, keys_hot;
    if (kx < 24.f * s_scale) {
        kx = 24.f * s_scale;
    }
    keys_hot = s_kb_active && s_kb_zone == KB_ZONE_KEYS;
    if (s_kb_i < 0 || s_kb_i >= n) {
        s_kb_i = 0;
    }
    for (i = 0; i < n; i++) {
        float x = kx + (float)keys[i].col * cell;
        float y = ky + (float)keys[i].row * row_h;
        float w = (float)keys[i].span * cell - 8.f * s_scale;
        float h = 42.f * s_scale;
        float kr = 0.f, kg = 0.f, kb = 0.f, ka = 0.38f;
        char lab[32];
        float tsc, tw, tx;
        int engaged = 0;
        if (keys[i].special == KB_SPEC_SHIFT && s_kb_shift) {
            engaged = 1;
        }
        if (keys[i].special == KB_SPEC_SYMS && s_kb_sym) {
            engaged = 1;
        }
        if (keys_hot && i == s_kb_i) {
            kr = 1.f;
            kg = 1.f;
            kb = 1.f;
            ka = 0.30f;
        } else if (engaged) {
            kr = 1.f;
            kg = 1.f;
            kb = 1.f;
            ka = 0.16f;
        }
        draw_rect(x, y, w, h, kr, kg, kb, ka);
        flush_color();
        kb_key_label(&keys[i], lab, sizeof(lab));
        tsc = (keys[i].special == KB_SPEC_STEAM) ? 0.55f : 0.62f;
        tw = measure_text(lab, tsc);
        while (tsc > 0.40f && tw > w - 10.f * s_scale) {
            tsc -= 0.04f;
            tw = measure_text(lab, tsc);
        }
        tx = x + (w - tw) * 0.5f;
        if (tx < x + 6.f * s_scale) {
            tx = x + 6.f * s_scale;
        }
        draw_text(tx, y + 28.f * s_scale, lab, tsc, 1.f, 1.f, 1.f, 1.f);
        flush_font();
    }
}

static void draw_list(struct App *app)
{
    float top = 110.f * s_scale;
    float row_h = 72.f * s_scale;
    float left = 64.f * s_scale;
    float width = (float)s_win_w - 128.f * s_scale;
    int vis;
    (void)app;
    if (kb_on()) {
        float fx, fy, fw, fh;
        kb_field_geom(&fx, &fy, &fw, &fh);
        top = fy + fh + 16.f * s_scale;
    }
    vis = list_vis_rows();

    draw_rect(32.f * s_scale, 24.f * s_scale, (float)s_win_w - 64.f * s_scale,
              (float)s_win_h - 48.f * s_scale, 0.f, 0.f, 0.f, s_veil);
    flush_color();
    draw_text(left, 80.f * s_scale, heading(), 1.35f, 0.96f, 0.96f, 0.96f, 1.f);
    flush_font();

    if (kb_on()) {
        draw_kb_field();
    }

    for (int i = 0; i < vis; i++) {
        int idx = s_scroll + i;
        float y;
        if (idx >= s_nrows) {
            break;
        }
        y = top + (float)i * row_h;
        if (idx == s_focus && !(kb_on() && s_kb_active)) {
            draw_rect(left - 12.f * s_scale, y, width + 24.f * s_scale, row_h - 8.f * s_scale,
                      1, 1, 1, 0.10f);
            draw_rect(left - 12.f * s_scale, y, 6.f * s_scale, row_h - 8.f * s_scale,
                      1, 1, 1, 0.95f);
            flush_color();
        }
        draw_text_ex(left + 16.f * s_scale, y + 34.f * s_scale, s_rows[idx].line, 0.95f,
                     0.95f, 0.95f, 0.95f, 1.f,
                     (s_view == UI_VIEW_FOLDERS || s_view == UI_VIEW_FOLDER_TRACKS) &&
                         s_rows[idx].kind == UI_KIND_TRACK);
        if (s_rows[idx].sub[0]) {
            draw_text(left + 16.f * s_scale, y + 58.f * s_scale, s_rows[idx].sub, 0.55f, 0.7f, 0.7f, 0.7f, 1.f);
        }
        if (s_view == UI_VIEW_PRESETS && s_rows[idx].kind == UI_KIND_PRESET) {
            int rt = viz_preset_rating((int)s_rows[idx].id);
            float mx = left + width - 36.f * s_scale;
            float my = y + 42.f * s_scale;
            if (rt > 0) {
                draw_text(mx, my, "\xE2\x9C\x94", 0.95f, 0.25f, 0.86f, 0.40f, 1.f);
            } else if (rt < 0) {
                draw_text(mx, my, "\xE2\x9C\x96", 0.95f, 0.92f, 0.28f, 0.28f, 1.f);
            }
        }
        flush_font();
    }

    if (kb_on()) {
        draw_keyboard();
    }

    if (uses_letter_jump() && s_nrows > 0 && s_focus >= 0) {
        char t[8];
        uint32_t now = SDL_GetTicks();
        uint32_t dt = now - s_jump_ms;
        int hot = s_jump_cp && dt < 1200;
        float a = hot ? 0.95f : 0.22f;
        utf8_put(t, sizeof(t), row_code(s_focus));
        draw_text((float)s_win_w - 140.f * s_scale, (float)s_win_h * 0.48f,
                  t, 2.8f, 0.96f, 0.96f, 0.96f, a);
        flush_font();
    }
}

static int uses_letter_jump(void)
{
    switch (s_view) {
    case UI_VIEW_ALBUMS:
    case UI_VIEW_ALBUM_FAVORITES:
    case UI_VIEW_ARTISTS:
    case UI_VIEW_ARTIST_FAVORITES:
    case UI_VIEW_FOLDERS:
    case UI_VIEW_PLAYLISTS:
    case UI_VIEW_PLAYLIST_FAVORITES:
    case UI_VIEW_ALBUM_TRACKS:
    case UI_VIEW_ARTIST_ALBUMS:
    case UI_VIEW_FOLDER_TRACKS:
    case UI_VIEW_PLAYLIST_TRACKS:
    case UI_VIEW_PRESETS:
    case UI_VIEW_DIR_BROWSER:
    case UI_VIEW_QUEUE:
    case UI_VIEW_SEARCH:
    case UI_VIEW_RADIO_FAVORITES:
    case UI_VIEW_RADIO_SOMA:
    case UI_VIEW_RADIO_COUNTRIES:
    case UI_VIEW_RADIO_STATIONS:
    case UI_VIEW_RADIO_CUSTOM:
        return 1;
    default:
        return 0;
    }
}

static uint32_t row_code(int i)
{
    const char *s;
    uint32_t cp;
    if (i < 0 || i >= s_nrows) {
        return '#';
    }
    s = s_rows[i].line;
    while (*s == ' ') {
        s++;
    }
    if (s_view == UI_VIEW_ALBUMS || s_view == UI_VIEW_ARTIST_ALBUMS ||
        s_view == UI_VIEW_ALBUM_FAVORITES) {
        if (strncasecmp(s, "the ", 4) == 0) {
            s += 4;
        } else if (strncasecmp(s, "a ", 2) == 0) {
            s += 2;
        }
    }
    if (!*s) {
        return '#';
    }
    cp = utf8_next(&s);
    if (cp >= 'a' && cp <= 'z') {
        return cp - 'a' + 'A';
    }
    if (cp >= 'A' && cp <= 'Z') {
        return cp;
    }
    if (cp < 0x80) {
        return '#';
    }
    return cp;
}

static int group_start(int i)
{
    uint32_t b;
    if (i < 0) {
        return 0;
    }
    if (i >= s_nrows) {
        return s_nrows > 0 ? s_nrows - 1 : 0;
    }
    b = row_code(i);
    while (i > 0 && row_code(i - 1) == b) {
        i--;
    }
    return i;
}

static void letter_jump(int dir)
{
    uint32_t cur;
    int i;
    if (!uses_letter_jump() || s_nrows <= 1 || s_focus < 0) {
        return;
    }
    cur = row_code(s_focus);
    if (dir > 0) {
        for (i = s_focus + 1; i < s_nrows; i++) {
            if (row_code(i) != cur) {
                s_focus = i;
                clamp_focus();
                s_jump_cp = row_code(s_focus);
                s_jump_ms = SDL_GetTicks();
                return;
            }
        }
        s_focus = 0;
    } else {
        for (i = s_focus - 1; i >= 0; i--) {
            if (row_code(i) != cur) {
                s_focus = group_start(i);
                clamp_focus();
                s_jump_cp = row_code(s_focus);
                s_jump_ms = SDL_GetTicks();
                return;
            }
        }
        s_focus = group_start(s_nrows - 1);
    }
    clamp_focus();
    s_jump_cp = row_code(s_focus);
    s_jump_ms = SDL_GetTicks();
}

static int play_focused_track(struct App *app, int64_t id)
{
    LibTrack t;
    if (library_get_track(id, &t) != 0) {
        return -1;
    }
    if (s_view == UI_VIEW_ALBUM_TRACKS || s_view == UI_VIEW_ALBUMS) {
        library_queue_play_album_from(t.album_id, t.id);
    } else if (s_view == UI_VIEW_PLAYLIST_TRACKS || s_view == UI_VIEW_PLAYLISTS) {
        library_queue_play_playlist_from(s_ctx_playlist, t.id);
    } else if (s_view == UI_VIEW_FOLDER_TRACKS || s_view == UI_VIEW_FOLDERS) {
        library_queue_play_folder_from(t.folder_id, t.id);
    } else if (s_view == UI_VIEW_QUEUE) {
        int n = library_queue_len();
        for (int i = 0; i < n; i++) {
            LibTrack q;
            if (library_queue_get(i, &q) == 0 && q.id == t.id) {
                library_queue_set_index(i);
                break;
            }
        }
    } else {
        library_queue_play_track(t.id);
        if (t.album_id) {
            /* keep playing through the album after this track */
            int n = library_album_tracks(t.album_id, s_qbuf.tracks, VIBE_LIST_MAX);
            int seen = 0;
            for (int i = 0; i < n; i++) {
                if (!seen) {
                    if (s_qbuf.tracks[i].id == t.id) {
                        seen = 1;
                    }
                    continue;
                }
                library_queue_add(s_qbuf.tracks[i].id);
            }
        }
    }
    app_play_queue_index(app, library_queue_index());
    s_view = UI_VIEW_NOW_PLAYING;
    s_sp = 0;
    return 0;
}

static void draw_quit_prompt(void)
{
    const char *title = "Quit StOMP?";
    const char *hint = "B again to quit.";
    const char *stay = "No! I want to keep StOMPing!";
    const char *quit = "Quit";
    float pad = 48.f * s_scale;
    float gap = 16.f * s_scale;
    float bh = 56.f * s_scale;
    float sc = 0.6f;
    float stay_w = measure_text(stay, sc) + 48.f * s_scale;
    float quit_w = measure_text(quit, sc) + 48.f * s_scale;
    float w;
    float h = 280.f * s_scale;
    float x, y, by, stay_x, quit_x;
    if (stay_w < 280.f * s_scale) {
        stay_w = 280.f * s_scale;
    }
    if (quit_w < 140.f * s_scale) {
        quit_w = 140.f * s_scale;
    }
    w = pad * 2.f + stay_w + gap + quit_w;
    if (w < 640.f * s_scale) {
        w = 640.f * s_scale;
    }
    x = ((float)s_win_w - w) * 0.5f;
    y = ((float)s_win_h - h) * 0.5f;
    by = y + h - 88.f * s_scale;
    stay_x = x + pad;
    quit_x = x + w - pad - quit_w;
    draw_rect(0, 0, (float)s_win_w, (float)s_win_h, 0.f, 0.f, 0.f, 0.55f);
    draw_rect(x, y, w, h, 0.04f, 0.05f, 0.07f, 0.94f);
    flush_color();
    draw_text(x + (w - measure_text(title, 1.05f)) * 0.5f, y + 72.f * s_scale,
              title, 1.05f, 0.96f, 0.96f, 0.96f, 1.f);
    draw_text(x + (w - measure_text(hint, 0.55f)) * 0.5f, y + 118.f * s_scale,
              hint, 0.55f, 0.7f, 0.7f, 0.7f, 1.f);
    flush_font();
    draw_np_chip(stay_x, by, stay_w, bh, s_quit_choice == 0, stay);
    draw_np_chip(quit_x, by, quit_w, bh, s_quit_choice == 1, quit);
}

static void open_quit_prompt(void)
{
    s_quit_prompt = 1;
    s_quit_choice = 0;
    s_overlay = 1;
    s_idle_on = 0;
}

static int queue_focus_index(void)
{
    int i, qi = 0;
    if (s_view != UI_VIEW_QUEUE || s_focus < 0 || s_focus >= s_nrows) {
        return -1;
    }
    if (s_rows[s_focus].kind != UI_KIND_TRACK) {
        return -1;
    }
    for (i = 0; i < s_focus; i++) {
        if (s_rows[i].kind == UI_KIND_TRACK) {
            qi++;
        }
    }
    return qi;
}

static void after_queue_remove(struct App *app, int removed_current)
{
    if (library_queue_len() <= 0) {
        app_halt(app);
        snprintf(app->status, sizeof(app->status), "Queue cleared");
        return;
    }
    if (removed_current && !app->playing_radio) {
        app_play_queue_index(app, library_queue_index());
    }
}

static void queue_remove_index(struct App *app, int qi)
{
    int cur = library_queue_index();
    int playing = qi == cur && app->now_valid && !app->playing_radio;
    if (library_queue_remove(qi) != 0) {
        return;
    }
    snprintf(app->status, sizeof(app->status), "Removed from queue");
    after_queue_remove(app, playing);
    ui_refresh_lists(app);
}

static void queue_remove_track_id(struct App *app, int64_t track_id)
{
    int playing = app->now_valid && !app->playing_radio && app->now.id == track_id;
    int n = library_queue_remove_track(track_id);
    if (n <= 0) {
        snprintf(app->status, sizeof(app->status), "Not in queue");
        return;
    }
    snprintf(app->status, sizeof(app->status), n == 1 ? "Removed from queue" : "Removed %d from queue", n);
    after_queue_remove(app, playing);
    ui_refresh_lists(app);
}

static void queue_append_focus(struct App *app)
{
    int n = 0;
    if (s_focus < 0 || s_focus >= s_nrows) {
        return;
    }
    switch (s_rows[s_focus].kind) {
    case UI_KIND_TRACK:
        n = library_queue_add(s_rows[s_focus].id) == 0 ? 1 : 0;
        break;
    case UI_KIND_ALBUM:
        n = library_queue_add_album(s_rows[s_focus].id);
        break;
    case UI_KIND_ARTIST:
        n = library_queue_add_artist(s_rows[s_focus].id);
        break;
    case UI_KIND_FOLDER:
        n = library_queue_add_folder(s_rows[s_focus].id);
        break;
    case UI_KIND_PLAYLIST:
        n = library_queue_add_playlist(s_rows[s_focus].id);
        break;
    default:
        return;
    }
    if (n <= 0) {
        snprintf(app->status, sizeof(app->status),
                 library_queue_len() >= VIBE_QUEUE_MAX ? "Queue is full" : "Nothing to add");
        return;
    }
    snprintf(app->status, sizeof(app->status), n == 1 ? "Added to queue" : "Added %d to queue", n);
    ui_refresh_lists(app);
}

static void playlist_filename(const char *name, char *out, size_t n)
{
    size_t j = 0;
    const char *p;
    if (!out || n == 0) {
        return;
    }
    out[0] = '\0';
    for (p = name ? name : ""; *p && j + 1 < n; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32) {
            continue;
        }
        if (strchr("/\\:*?\"<>|", (int)c)) {
            out[j++] = '_';
        } else {
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
    while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '.' || out[j - 1] == '_')) {
        out[--j] = '\0';
    }
    if (!out[0]) {
        snprintf(out, n, "Queue");
    }
}

static int save_queue_playlist(struct App *app)
{
    char file[VIBE_NAME_MAX];
    char path[VIBE_PATH_MAX];
    const char *dir;
    size_t n;
    if (!app) {
        return -1;
    }
    if (library_queue_len() <= 0) {
        snprintf(app->status, sizeof(app->status), "Queue is empty");
        return -1;
    }
    if (app->cfg.music_dir_count <= 0 || !app->cfg.music_dirs[0][0]) {
        snprintf(app->status, sizeof(app->status), "Set a music folder first");
        return -1;
    }
    playlist_filename(s_pl_name, file, sizeof(file));
    n = strlen(file);
    if (n < 4 || strcasecmp(file + n - 4, ".m3u") != 0) {
        if (n + 4 >= sizeof(file)) {
            file[sizeof(file) - 5] = '\0';
            n = strlen(file);
        }
        snprintf(file + n, sizeof(file) - n, ".m3u");
    }
    dir = app->cfg.music_dirs[0];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    if (library_queue_export_m3u(path, s_pl_name[0] ? s_pl_name : file) != 0) {
        snprintf(app->status, sizeof(app->status), "Could not save playlist");
        return -1;
    }
    snprintf(app->status, sizeof(app->status), "Saved %s", file);
    return 0;
}

static void list_step(int dir, int repeat)
{
    if (s_nrows <= 0) {
        s_focus = 0;
        return;
    }
    if (dir < 0) {
        if (s_focus > 0) {
            s_focus--;
        } else if (!repeat) {
            s_focus = s_nrows - 1;
        }
    } else if (s_focus < s_nrows - 1) {
        s_focus++;
    } else if (!repeat) {
        s_focus = 0;
    }
    clamp_focus();
}

int ui_handle(struct App *app, VibeCmd cmd, float seek_axis, int repeat)
{
    (void)seek_axis;
    if (s_quit_prompt) {
        switch (cmd) {
        case VIBE_CMD_LEFT:
            s_quit_choice = 0;
            return 1;
        case VIBE_CMD_RIGHT:
            s_quit_choice = 1;
            return 1;
        case VIBE_CMD_UP:
        case VIBE_CMD_DOWN:
            s_quit_choice = !s_quit_choice;
            return 1;
        case VIBE_CMD_CONFIRM:
            if (s_quit_choice) {
                app_persist(app);
                app->running = 0;
            }
            s_quit_prompt = 0;
            return 1;
        case VIBE_CMD_BACK:
            app_persist(app);
            app->running = 0;
            s_quit_prompt = 0;
            return 1;
        case VIBE_CMD_LIBRARY:
            s_quit_prompt = 0;
            return 1;
        default:
            return 1;
        }
    }
    switch (cmd) {
    case VIBE_CMD_CONFIRM_HOLD:
        if (s_hold_kind && s_focus >= 0 && s_focus < s_nrows) {
            int on = -1;
            if (s_rows[s_focus].kind == UI_KIND_RADIO) {
                const char *url = (s_focus >= 0 && s_focus < RADIO_URL_SLOTS) ? s_radio_play_url[s_focus] : "";
                on = library_radio_fav_toggle(s_rows[s_focus].line, url, s_rows[s_focus].sub);
            } else if (s_rows[s_focus].kind == UI_KIND_ARTIST) {
                on = library_artist_fav_toggle(s_rows[s_focus].id);
            } else if (s_rows[s_focus].kind == UI_KIND_ALBUM) {
                on = library_album_fav_toggle(s_rows[s_focus].id);
            } else if (s_rows[s_focus].kind == UI_KIND_PLAYLIST) {
                on = library_playlist_fav_toggle(s_rows[s_focus].id);
            }
            if (on >= 0) {
                s_hold_did = 1;
                snprintf(app->status, sizeof(app->status),
                         on > 0 ? "Favorite" : "Removed from favorites");
                ui_refresh_lists(app);
            }
        }
        return 1;
    case VIBE_CMD_CONFIRM_UP:
        if (s_hold_kind && !s_hold_did &&
            s_focus >= 0 && s_focus < s_nrows) {
            if (s_rows[s_focus].kind == UI_KIND_RADIO) {
                const char *url = (s_focus >= 0 && s_focus < RADIO_URL_SLOTS) ? s_radio_play_url[s_focus] : "";
                app_play_radio(app, s_rows[s_focus].line, url, s_rows[s_focus].sub);
            } else if (s_rows[s_focus].kind == UI_KIND_ARTIST) {
                s_ctx_artist = s_rows[s_focus].id;
                push_view(UI_VIEW_ARTIST_ALBUMS);
                ui_refresh_lists(app);
            } else if (s_rows[s_focus].kind == UI_KIND_ALBUM) {
                s_ctx_album = s_rows[s_focus].id;
                push_view(UI_VIEW_ALBUM_TRACKS);
                ui_refresh_lists(app);
            } else if (s_rows[s_focus].kind == UI_KIND_PLAYLIST) {
                s_ctx_playlist = s_rows[s_focus].id;
                push_view(UI_VIEW_PLAYLIST_TRACKS);
                ui_refresh_lists(app);
            }
        }
        s_hold_kind = 0;
        s_hold_did = 0;
        return 1;
    case VIBE_CMD_LIBRARY:
        s_folder_sp = 0;
        s_ctx_folder = 0;
        s_folder_title[0] = '\0';
        if (s_view == UI_VIEW_LIBRARY) {
            s_view = UI_VIEW_NOW_PLAYING;
            s_sp = 0;
        } else {
            s_sp = 0;
            s_view = UI_VIEW_LIBRARY;
            s_focus = 0;
            ui_refresh_lists(app);
        }
        return 1;
    case VIBE_CMD_QUEUE:
        if (s_view == UI_VIEW_QUEUE) {
            pop_view();
        } else {
            push_view(UI_VIEW_QUEUE);
        }
        ui_refresh_lists(app);
        return 1;
    case VIBE_CMD_BACK:
        if (s_view == UI_VIEW_NOW_PLAYING) {
            if (s_vol_open) {
                audio_set_volume(s_vol_saved);
                app->cfg.volume = audio_volume();
                s_vol_open = 0;
                return 1;
            }
            if (s_np_opt > 0) {
                s_np_opt = 0;
                return 1;
            }
            if (s_overlay) {
                ui_hide_overlay();
                return 1;
            }
            open_quit_prompt();
            return 1;
        }
        if ((s_view == UI_VIEW_FOLDERS || s_view == UI_VIEW_FOLDER_TRACKS) && s_folder_sp > 0) {
            s_folder_sp--;
            s_ctx_folder = s_folder_stack[s_folder_sp];
            s_focus = s_folder_focus[s_folder_sp];
            s_scroll = s_folder_scroll[s_folder_sp];
            if (s_ctx_folder <= 0) {
                s_folder_title[0] = '\0';
            } else {
                LibFolder f;
                if (library_get_folder(s_ctx_folder, &f) == 0) {
                    snprintf(s_folder_title, sizeof(s_folder_title), "%s", f.name);
                }
            }
            ui_refresh_lists(app);
            clamp_focus();
            return 1;
        }
        if (s_view == UI_VIEW_BINDING && s_bind_capture) {
            s_bind_capture = 0;
            ui_refresh_lists(app);
            snprintf(app->status, sizeof(app->status), "Binding cancelled");
            return 1;
        }
        pop_view();
        ui_refresh_lists(app);
        clamp_focus();
        return 1;
    case VIBE_CMD_UP:
        if (s_view == UI_VIEW_NOW_PLAYING) {
            if (s_vol_open) {
                s_vol_draft += 0.04f;
                if (s_vol_draft > 1.f) {
                    s_vol_draft = 1.f;
                }
                audio_set_volume(s_vol_draft);
                return 1;
            }
            s_np_opt = 1;
            return 1;
        }
        if (kb_nav(-1, 0)) {
            return 1;
        }
        if (kb_on() && !s_kb_active && s_focus <= 0) {
            s_kb_active = 1;
            s_kb_zone = KB_ZONE_FIELD;
            return 1;
        }
        list_step(-1, repeat);
        return 1;
    case VIBE_CMD_DOWN:
        if (s_view == UI_VIEW_NOW_PLAYING) {
            if (s_vol_open) {
                s_vol_draft -= 0.04f;
                if (s_vol_draft < 0.f) {
                    s_vol_draft = 0.f;
                }
                audio_set_volume(s_vol_draft);
                return 1;
            }
            s_np_opt = 0;
            return 1;
        }
        if (kb_nav(1, 0)) {
            return 1;
        }
        if (kb_on() && !s_kb_active && s_nrows > 0 && s_focus >= s_nrows - 1 && !repeat) {
            s_kb_active = 1;
            s_kb_zone = KB_ZONE_KEYS;
            return 1;
        }
        list_step(1, repeat);
        return 1;
    case VIBE_CMD_LEFT:
        if (s_view == UI_VIEW_NOW_PLAYING) {
            if (s_np_opt > 1) {
                s_np_opt--;
            }
            return 1;
        }
        if (kb_nav(0, -1)) {
            return 1;
        }
        letter_jump(-1);
        return 1;
    case VIBE_CMD_RIGHT:
        if (s_view == UI_VIEW_NOW_PLAYING) {
            if (s_np_opt >= 1 && s_np_opt < NP_OPT_MAX) {
                s_np_opt++;
            }
            return 1;
        }
        if (kb_nav(0, 1)) {
            return 1;
        }
        letter_jump(1);
        return 1;
    case VIBE_CMD_PAGE_UP:
        if (kb_on() && s_kb_active) {
            s_kb_caret = 0;
            return 1;
        }
        s_focus -= 8;
        clamp_focus();
        return 1;
    case VIBE_CMD_PAGE_DOWN:
        if (kb_on() && s_kb_active) {
            s_kb_caret = strlen(kb_buf());
            return 1;
        }
        s_focus += 8;
        clamp_focus();
        return 1;
    case VIBE_CMD_CONFIRM:
        if (s_view == UI_VIEW_NOW_PLAYING) {
            if (s_vol_open) {
                app->cfg.volume = audio_volume();
                s_vol_open = 0;
                config_save(&app->cfg);
                return 1;
            }
            if (s_np_opt == NP_OPT_LOCK) {
                viz_set_lock(!viz_locked());
            } else if (s_np_opt == NP_OPT_PREV) {
                app_prev_track(app);
            } else if (s_np_opt == NP_OPT_PLAY) {
                app_toggle_pause(app);
            } else if (s_np_opt == NP_OPT_STOP) {
                app_stop(app);
            } else if (s_np_opt == NP_OPT_NEXT) {
                app_next_track(app, 0);
            } else if (s_np_opt == NP_OPT_SHUFFLE) {
                app->shuffle = !app->shuffle;
            } else if (s_np_opt == NP_OPT_REPEAT) {
                app->repeat = (VibeRepeat)(((int)app->repeat + 1) % 3);
            } else if (s_np_opt == NP_OPT_VOLUME) {
                s_vol_saved = audio_volume();
                s_vol_draft = s_vol_saved;
                s_vol_open = 1;
            } else if (s_np_opt == NP_OPT_PRESET) {
                push_view(UI_VIEW_PRESETS);
                ui_refresh_lists(app);
                s_focus = viz_rank_of(viz_current_index());
                clamp_focus();
            } else if (s_np_opt == NP_OPT_EYE) {
                viz_set_enabled(!viz_enabled());
                library_session_set_int("viz_enabled", viz_enabled());
            } else {
                app_toggle_pause(app);
            }
            return 1;
        }
        if (kb_on() && s_kb_active) {
            const KbKey *keys = kb_cur();
            const KbKey *k;
            int n = kb_count();
            if (s_kb_zone == KB_ZONE_FIELD) {
                open_steam_keyboard();
                snprintf(app->status, sizeof(app->status), "Steam keyboard");
                return 1;
            }
            if (s_kb_i < 0 || s_kb_i >= n) {
                s_kb_i = 0;
            }
            k = &keys[s_kb_i];
            if (k->special == KB_SPEC_BKSP) {
                kb_backspace();
            } else if (k->special == KB_SPEC_CLEAR) {
                kb_clear();
            } else if (k->special == KB_SPEC_DONE) {
                if (s_view == UI_VIEW_RADIO_ADD) {
                    char name[VIBE_NAME_MAX];
                    if (!s_radio_url[0]) {
                        return 1;
                    }
                    radio_name_from_url(s_radio_url, name, (int)sizeof(name));
                    if (library_radio_custom_add(name, s_radio_url) == 0) {
                        snprintf(app->status, sizeof(app->status), "Saved");
                    } else {
                        snprintf(app->status, sizeof(app->status), "Could not save");
                    }
                    pop_view();
                    ui_refresh_lists(app);
                    return 1;
                }
                if (s_view == UI_VIEW_QUEUE_SAVE) {
                    if (save_queue_playlist(app) == 0) {
                        pop_view();
                        pop_view();
                        ui_refresh_lists(app);
                    }
                    return 1;
                }
                if (s_nrows > 0) {
                    s_kb_active = 0;
                    s_focus = 0;
                    clamp_focus();
                }
                return 1;
            } else if (k->special == KB_SPEC_SHIFT) {
                s_kb_shift = !s_kb_shift;
                return 1;
            } else if (k->special == KB_SPEC_SYMS) {
                s_kb_sym = !s_kb_sym;
                return 1;
            } else if (k->special == KB_SPEC_STEAM) {
                open_steam_keyboard();
                snprintf(app->status, sizeof(app->status), "Steam keyboard");
                return 1;
            } else {
                char tmp[2];
                char c = k->ch;
                if (!s_kb_sym && c >= 'a' && c <= 'z' && s_kb_shift) {
                    c = (char)(c - 'a' + 'A');
                }
                tmp[0] = c;
                tmp[1] = '\0';
                kb_insert(tmp);
            }
            if (s_view == UI_VIEW_SEARCH) {
                ui_refresh_lists(app);
            }
            return 1;
        }
        if (s_focus < 0 || s_focus >= s_nrows) {
            return 1;
        }
        if (s_view == UI_VIEW_LIBRARY) {
            switch ((int)s_rows[s_focus].id) {
            case 1:
                app_resume_playback(app);
                break;
            case 3:
                push_view(UI_VIEW_ALBUMS);
                break;
            case 4:
                push_view(UI_VIEW_ARTISTS);
                break;
            case 5:
                s_ctx_folder = 0;
                s_folder_sp = 0;
                s_folder_title[0] = '\0';
                push_view(UI_VIEW_FOLDERS);
                break;
            case 10:
                push_view(UI_VIEW_RADIO);
                break;
            case 11:
                push_view(UI_VIEW_PLAYLISTS);
                break;
            case 8:
                push_view(UI_VIEW_QUEUE);
                break;
            case 6:
                push_view(UI_VIEW_SEARCH);
                kb_reset();
                break;
            case 7:
                push_view(UI_VIEW_SETTINGS);
                break;
            case 9:
                app_persist(app);
                app->running = 0;
                return 1;
            default:
                break;
            }
            ui_refresh_lists(app);
            return 1;
        }
        if (s_view == UI_VIEW_RADIO) {
            int id = (int)s_rows[s_focus].id;
            s_radio_err[0] = '\0';
            if (id == 1) {
                if (radio_soma_count() <= 0) {
                    snprintf(app->status, sizeof(app->status), "Loading Soma.fm…");
                    radio_fetch_soma(s_radio_err, (int)sizeof(s_radio_err));
                }
                push_view(UI_VIEW_RADIO_SOMA);
            } else if (id == 2) {
                if (radio_country_count() <= 0) {
                    snprintf(app->status, sizeof(app->status), "Loading countries…");
                    radio_fetch_countries(s_radio_err, (int)sizeof(s_radio_err));
                }
                push_view(UI_VIEW_RADIO_COUNTRIES);
            } else if (id == 3) {
                push_view(UI_VIEW_RADIO_CUSTOM);
            } else if (id == 4) {
                push_view(UI_VIEW_RADIO_FAVORITES);
            }
            ui_refresh_lists(app);
            return 1;
        }
        if (s_view == UI_VIEW_RADIO_COUNTRIES) {
            RadioCountry c;
            if (radio_country_at((int)s_rows[s_focus].id, &c) == 0) {
                snprintf(s_radio_cc, sizeof(s_radio_cc), "%s", c.code);
                s_radio_err[0] = '\0';
                snprintf(app->status, sizeof(app->status), "Loading stations…");
                radio_fetch_stations(s_radio_cc, s_radio_err, (int)sizeof(s_radio_err));
                push_view(UI_VIEW_RADIO_STATIONS);
                ui_refresh_lists(app);
            }
            return 1;
        }
        if (s_view == UI_VIEW_RADIO_CUSTOM && s_rows[s_focus].kind == UI_KIND_ACTION &&
            s_rows[s_focus].id == 1) {
            s_radio_url[0] = '\0';
            push_view(UI_VIEW_RADIO_ADD);
            kb_reset();
            ui_refresh_lists(app);
            return 1;
        }
        if (s_view == UI_VIEW_RADIO_ADD) {
            char name[VIBE_NAME_MAX];
            if (!s_radio_url[0]) {
                return 1;
            }
            radio_name_from_url(s_radio_url, name, (int)sizeof(name));
            if (library_radio_custom_add(name, s_radio_url) == 0) {
                snprintf(app->status, sizeof(app->status), "Saved");
            } else {
                snprintf(app->status, sizeof(app->status), "Could not save");
            }
            pop_view();
            ui_refresh_lists(app);
            return 1;
        }
        if (s_view == UI_VIEW_ARTISTS && s_rows[s_focus].kind == UI_KIND_ACTION &&
            s_rows[s_focus].id == 1) {
            push_view(UI_VIEW_ARTIST_FAVORITES);
            ui_refresh_lists(app);
            return 1;
        }
        if (s_view == UI_VIEW_ALBUMS && s_rows[s_focus].kind == UI_KIND_ACTION &&
            s_rows[s_focus].id == 1) {
            push_view(UI_VIEW_ALBUM_FAVORITES);
            ui_refresh_lists(app);
            return 1;
        }
        if (s_view == UI_VIEW_PLAYLISTS && s_rows[s_focus].kind == UI_KIND_ACTION &&
            s_rows[s_focus].id == 1) {
            push_view(UI_VIEW_PLAYLIST_FAVORITES);
            ui_refresh_lists(app);
            return 1;
        }
        if (s_rows[s_focus].kind == UI_KIND_RADIO) {
            s_hold_kind = UI_KIND_RADIO;
            s_hold_did = 0;
            return 1;
        }
        if (s_view == UI_VIEW_MUSIC_DIRS) {
            int id = (int)s_rows[s_focus].id;
            if (id == 1) {
                open_dir_browser(app->cfg.music_dir_count > 0 ? app->cfg.music_dirs[0] : NULL);
                ui_refresh_lists(app);
            } else if (id >= 100) {
                int idx = id - 100;
                if (idx >= 0 && idx < app->cfg.music_dir_count) {
                    open_dir_browser(app->cfg.music_dirs[idx]);
                    ui_refresh_lists(app);
                }
            }
            return 1;
        }
        if (s_view == UI_VIEW_DIR_BROWSER) {
            int id = (int)s_rows[s_focus].id;
            if (id == 1) {
                if (config_add_music_dir(&app->cfg, s_browse) < 0) {
                    snprintf(app->status, sizeof(app->status), "At most %d folders", VIBE_MAX_MUSIC_DIRS);
                } else {
                    apply_music_dirs(app);
                    pop_view();
                    ui_refresh_lists(app);
                }
            } else if (id == 2) {
                browse_parent();
                s_focus = 0;
                s_scroll = 0;
                ui_refresh_lists(app);
            } else if (id >= 10) {
                int idx = id - 10;
                if (idx >= 0 && idx < s_ndirents && browse_enter(s_dirents[idx]) == 0) {
                    s_focus = 0;
                    s_scroll = 0;
                    ui_refresh_lists(app);
                }
            }
            return 1;
        }
        if (s_rows[s_focus].kind == UI_KIND_ALBUM) {
            s_hold_kind = UI_KIND_ALBUM;
            s_hold_did = 0;
            return 1;
        }
        if (s_rows[s_focus].kind == UI_KIND_ARTIST) {
            s_hold_kind = UI_KIND_ARTIST;
            s_hold_did = 0;
            return 1;
        }
        if (s_rows[s_focus].kind == UI_KIND_FOLDER) {
            if (s_folder_sp < FOLDER_STACK) {
                s_folder_stack[s_folder_sp] = s_ctx_folder;
                s_folder_focus[s_folder_sp] = s_focus;
                s_folder_scroll[s_folder_sp] = s_scroll;
                s_folder_sp++;
            }
            s_ctx_folder = s_rows[s_focus].id;
            snprintf(s_folder_title, sizeof(s_folder_title), "%s", s_rows[s_focus].line);
            s_focus = 0;
            s_scroll = 0;
            ui_refresh_lists(app);
            return 1;
        }
        if (s_rows[s_focus].kind == UI_KIND_PLAYLIST && s_rows[s_focus].id > 0) {
            s_hold_kind = UI_KIND_PLAYLIST;
            s_hold_did = 0;
            return 1;
        }
        if (s_rows[s_focus].kind == UI_KIND_TRACK) {
            play_focused_track(app, s_rows[s_focus].id);
            return 1;
        }
        if (s_view == UI_VIEW_PRESETS && s_rows[s_focus].kind == UI_KIND_PRESET) {
            viz_play_index((int)s_rows[s_focus].id, 1);
            pop_view();
            s_np_opt = NP_OPT_PRESET;
            return 1;
        }
        if (s_view == UI_VIEW_QUEUE && s_rows[s_focus].kind == UI_KIND_ACTION &&
            s_rows[s_focus].id == 1) {
            push_view(UI_VIEW_QUEUE_OPTIONS);
            ui_refresh_lists(app);
            return 1;
        }
        if (s_view == UI_VIEW_QUEUE_OPTIONS) {
            int id = (int)s_rows[s_focus].id;
            if (id == 1) {
                library_queue_clear();
                app_halt(app);
                snprintf(app->status, sizeof(app->status), "Queue cleared");
                pop_view();
                ui_refresh_lists(app);
            } else if (id == 2) {
                s_pl_name[0] = '\0';
                push_view(UI_VIEW_QUEUE_SAVE);
                kb_reset();
                ui_refresh_lists(app);
            }
            return 1;
        }
        if (s_view == UI_VIEW_QUEUE_SAVE) {
            if (save_queue_playlist(app) == 0) {
                pop_view();
                pop_view();
                ui_refresh_lists(app);
            }
            return 1;
        }
        if (s_view == UI_VIEW_BINDING) {
            if (s_bind_capture) {
                return 1;
            }
            if (s_focus >= 0 && s_focus < s_nrows && s_rows[s_focus].id == 1000) {
                input_reset_bindings();
                memcpy(app->cfg.bindings, input_binds(), sizeof(app->cfg.bindings));
                config_save(&app->cfg);
                ui_refresh_lists(app);
                snprintf(app->status, sizeof(app->status), "Layout reset");
                return 1;
            }
            if (s_focus >= 0 && s_focus < s_nrows &&
                s_rows[s_focus].id >= 0 && s_rows[s_focus].id < VIBE_BIND_COUNT) {
                s_bind_target = (int)s_rows[s_focus].id;
                s_bind_capture = 1;
                ui_refresh_lists(app);
                snprintf(app->status, sizeof(app->status), "Press a button…");
                return 1;
            }
            return 1;
        }
        if (s_view == UI_VIEW_SETTINGS) {
            switch ((int)s_rows[s_focus].id) {
            case 1:
                decode_request_scan();
                snprintf(app->status, sizeof(app->status), "Scanning…");
                break;
            case 2: {
                char names[16][128];
                int n = 0;
                audio_list_devices(names, &n, 16);
                if (n > 0) {
                    s_settings_audio_i = (s_settings_audio_i + 1) % n;
                    snprintf(app->cfg.audio_device, sizeof(app->cfg.audio_device), "%s", names[s_settings_audio_i]);
                    audio_reopen(app->cfg.audio_device);
                }
                break;
            }
            case 4:
                viz_set_shuffle(!viz_shuffle());
                app->cfg.viz_shuffle = viz_shuffle();
                break;
            case 5:
                viz_set_lock(!viz_locked());
                break;
            case 9:
                viz_set_pool((VibeVizPool)(((int)viz_pool() + 1) % 3));
                library_session_set_int("viz_pool", (int)viz_pool());
                break;
            case 6:
                app->cfg.start_library = !app->cfg.start_library;
                break;
            case 8:
                push_view(UI_VIEW_MUSIC_DIRS);
                ui_refresh_lists(app);
                return 1;
            case 10:
                push_view(UI_VIEW_BINDING);
                ui_refresh_lists(app);
                return 1;
            default:
                break;
            }
            config_save(&app->cfg);
            ui_refresh_lists(app);
            return 1;
        }
        return 1;
    case VIBE_CMD_QUEUE_ADD:
        if (s_view == UI_VIEW_NOW_PLAYING) {
            viz_rate_current(0);
            s_rate_flash_good = 0;
            s_rate_flash_ms = SDL_GetTicks();
            viz_next_preset(0);
        }
        return 1;
    case VIBE_CMD_QUEUE_ADD_HOLD:
        if (kb_on() && s_kb_active) {
            return 1;
        }
        if (s_view == UI_VIEW_NOW_PLAYING) {
            if (app->now_valid && !app->playing_radio && app->now.id > 0) {
                queue_remove_track_id(app, app->now.id);
            }
            return 1;
        }
        if (s_view == UI_VIEW_QUEUE) {
            int qi = queue_focus_index();
            if (qi >= 0) {
                queue_remove_index(app, qi);
            }
            return 1;
        }
        if (s_focus >= 0 && s_focus < s_nrows && s_rows[s_focus].kind == UI_KIND_TRACK) {
            queue_remove_track_id(app, s_rows[s_focus].id);
        }
        return 1;
    case VIBE_CMD_QUEUE_APPEND:
        if (kb_on() && s_kb_active) {
            return 1;
        }
        if (s_view == UI_VIEW_NOW_PLAYING) {
            if (app->now_valid && !app->playing_radio && app->now.id > 0) {
                if (library_queue_add(app->now.id) == 0) {
                    snprintf(app->status, sizeof(app->status), "Added to queue");
                } else {
                    snprintf(app->status, sizeof(app->status),
                             library_queue_len() >= VIBE_QUEUE_MAX ? "Queue is full" : "Nothing to add");
                }
            }
            return 1;
        }
        queue_append_focus(app);
        return 1;
    case VIBE_CMD_SEARCH_BACKSPACE:
        if (kb_on() && s_kb_active) {
            kb_backspace();
            if (s_view == UI_VIEW_SEARCH) {
                ui_refresh_lists(app);
            }
            return 1;
        }
        return 1;
    case VIBE_CMD_SEARCH_CLEAR:
        if (kb_on() && s_kb_active) {
            kb_clear();
            if (s_view == UI_VIEW_SEARCH) {
                ui_refresh_lists(app);
            }
        }
        return 1;
    case VIBE_CMD_SEARCH_OR_REMOVE:
        if (s_view == UI_VIEW_NOW_PLAYING) {
            viz_rate_current(1);
            s_rate_flash_good = 1;
            s_rate_flash_ms = SDL_GetTicks();
            viz_next_preset(0);
            return 1;
        }
        if (s_view == UI_VIEW_MUSIC_DIRS && s_focus >= 0 && s_focus < s_nrows) {
            int idx = (int)s_rows[s_focus].id - 100;
            if (idx >= 0 && idx < app->cfg.music_dir_count) {
                config_remove_music_dir(&app->cfg, idx);
                apply_music_dirs(app);
                ui_refresh_lists(app);
            }
            return 1;
        }
        if (s_view == UI_VIEW_DIR_BROWSER) {
            return 1;
        }
        if (s_view == UI_VIEW_RADIO_CUSTOM && s_rows[s_focus].kind == UI_KIND_RADIO) {
            library_radio_custom_remove(s_rows[s_focus].id);
            ui_refresh_lists(app);
            return 1;
        }
        if (s_view == UI_VIEW_RADIO_FAVORITES && s_rows[s_focus].kind == UI_KIND_RADIO) {
            library_radio_fav_remove(s_rows[s_focus].id);
            ui_refresh_lists(app);
            return 1;
        }
        if (s_view == UI_VIEW_ARTIST_FAVORITES && s_rows[s_focus].kind == UI_KIND_ARTIST) {
            library_artist_fav_remove(s_rows[s_focus].id);
            ui_refresh_lists(app);
            return 1;
        }
        if (s_view == UI_VIEW_ALBUM_FAVORITES && s_rows[s_focus].kind == UI_KIND_ALBUM) {
            library_album_fav_remove(s_rows[s_focus].id);
            ui_refresh_lists(app);
            return 1;
        }
        if (s_view == UI_VIEW_PLAYLIST_FAVORITES && s_rows[s_focus].kind == UI_KIND_PLAYLIST) {
            library_playlist_fav_remove(s_rows[s_focus].id);
            ui_refresh_lists(app);
            return 1;
        }
        if (kb_on() && s_kb_active) {
            kb_backspace();
            if (s_view == UI_VIEW_SEARCH) {
                ui_refresh_lists(app);
            }
            return 1;
        }
        return 1;
    default:
        return 0;
    }
}

void ui_text_input(const char *text)
{
    if (!text || !kb_on()) {
        return;
    }
    s_kb_active = 1;
    s_kb_zone = KB_ZONE_FIELD;
    kb_insert(text);
}

void ui_draw(struct App *app)
{
    int dw = 0, dh = 0;
    /* ProjectM already filled the default framebuffer. Do not clear it. */
    plat_drawable_size(&dw, &dh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, dw > 0 ? dw : s_win_w, dh > 0 ? dh : s_win_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(s_vao);
    glUseProgram(s_prog);

    s_nv = 0;
    if (s_overlay) {
        if (s_view == UI_VIEW_NOW_PLAYING) {
            draw_now_playing(app);
        } else {
            draw_list(app);
        }
    }
    if (s_quit_prompt) {
        draw_quit_prompt();
    }
    if (s_rate_flash_ms) {
        uint32_t now = SDL_GetTicks();
        uint32_t dt = now - s_rate_flash_ms;
        if (dt < 900) {
            float a = 1.f - (float)dt / 900.f;
            float x = (float)s_win_w - 200.f * s_scale;
            float y = (float)s_win_h - 80.f * s_scale;
            if (s_rate_flash_good) {
                draw_text(x, y, "\xE2\x9C\x94", 4.2f, 0.22f, 0.92f, 0.38f, a);
            } else {
                draw_text(x, y, "\xE2\x9C\x96", 4.2f, 0.92f, 0.28f, 0.28f, a);
            }
            flush_font();
        } else {
            s_rate_flash_ms = 0;
        }
    }
}
