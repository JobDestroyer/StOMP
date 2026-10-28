/* SPDX-License-Identifier: LGPL-2.1-only
 * Copyright (C) 2026 JobDestroyer
 *
 * CD+G lyric graphics. Packets are 24 bytes at 300/s (CD subcode rate).
 * Display is 300x216 (6x12 tiles), 16-color CLUT, integer-scaled with NEAREST.
 * Instruction 28's index is keyed. If most of the frame is that index, keep
 * punch-through and stroke opaque tiles; otherwise draw keyed pixels as black.
 * Sidecar may be gzip (.cdg.gz); inflated in cdg_open before packet parse.
 */

#include "cdg.h"

#include "platform_sdl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define CDG_W 300
#define CDG_H 216
#define CDG_PKT 24
#define CDG_PPS 300.0
#define CDG_MASK 0x3F
#define CDG_CMD 0x09
#define CDG_MEM_PRESET 1
#define CDG_BORDER_PRESET 2
#define CDG_TILE 6
#define CDG_SCROLL_PRESET 20
#define CDG_SCROLL_COPY 24
#define CDG_TRANSPARENT 28
#define CDG_CLUT_LOW 30
#define CDG_CLUT_HIGH 31
#define CDG_TILE_XOR 38
#define CDG_HALO_MIN_TRANS ((CDG_W * CDG_H * 2) / 3)

static unsigned char *s_data;
static int s_npkt;
static int s_applied;
static unsigned char s_pix[CDG_W * CDG_H];
static unsigned char s_rgba[CDG_W * CDG_H * 4];
static unsigned char s_ink[CDG_W * CDG_H];
static unsigned s_clut[16];
static int s_hoff, s_voff, s_trans;
static int s_dirty, s_on;

static unsigned s_tex, s_prog, s_vao, s_vbo;
static int s_u_tex;
static int s_gl_ok;

static unsigned pack_rgb(int r, int g, int b)
{
    return 0xFF000000u | ((unsigned)r) | ((unsigned)g << 8) | ((unsigned)b << 16);
}

static void fill_all(int color)
{
    memset(s_pix, color & 0x0F, sizeof(s_pix));
    s_dirty = 1;
}

static void fill_border(int color)
{
    int x, y;
    color &= 0x0F;
    for (y = 0; y < CDG_H; y++) {
        for (x = 0; x < CDG_W; x++) {
            if (x < 6 || x >= 294 || y < 12 || y >= 204) {
                s_pix[y * CDG_W + x] = (unsigned char)color;
            }
        }
    }
    s_dirty = 1;
}

static void tile(const unsigned char *d, int xor_mode)
{
    int color0 = d[0] & 0x0F;
    int color1 = d[1] & 0x0F;
    int row = d[2] & 0x1F;
    int col = d[3] & 0x3F;
    int y, x;
    if (row > 17 || col > 49) {
        return;
    }
    for (y = 0; y < 12; y++) {
        int bits = d[4 + y] & 0x3F;
        int py = row * 12 + y;
        for (x = 0; x < 6; x++) {
            int px = col * 6 + x;
            int on = bits & (0x20 >> x);
            int c = on ? color1 : color0;
            int i = py * CDG_W + px;
            if (xor_mode) {
                s_pix[i] = (unsigned char)((s_pix[i] ^ (unsigned char)c) & 0x0F);
            } else {
                s_pix[i] = (unsigned char)c;
            }
        }
    }
    s_dirty = 1;
}

static void load_clut(const unsigned char *d, int high)
{
    int i, base = high ? 8 : 0;
    for (i = 0; i < 8; i++) {
        int b0 = d[i * 2] & 0x3F;
        int b1 = d[i * 2 + 1] & 0x3F;
        int r = (b0 >> 2) & 0x0F;
        int g = ((b0 & 0x03) << 2) | ((b1 >> 4) & 0x03);
        int b = b1 & 0x0F;
        s_clut[base + i] = pack_rgb(r * 17, g * 17, b * 17);
    }
    s_dirty = 1;
}

static void scroll(const unsigned char *d, int copy)
{
    unsigned char tmp[CDG_W * CDG_H];
    int color = d[0] & 0x0F;
    int hScroll = d[1] & 0x3F;
    int vScroll = d[2] & 0x3F;
    int hcmd = (hScroll >> 4) & 0x03;
    int vcmd = (vScroll >> 4) & 0x03;
    int dx = 0, dy = 0, x, y;
    s_hoff = hScroll & 0x07;
    s_voff = vScroll & 0x0F;
    if (s_hoff > 5) {
        s_hoff = 0;
    }
    if (s_voff > 11) {
        s_voff = 0;
    }
    if (hcmd == 1) {
        dx = 6;
    } else if (hcmd == 2) {
        dx = -6;
    }
    if (vcmd == 1) {
        dy = 12;
    } else if (vcmd == 2) {
        dy = -12;
    }
    if (dx == 0 && dy == 0) {
        s_dirty = 1;
        return;
    }
    memcpy(tmp, s_pix, sizeof(tmp));
    for (y = 0; y < CDG_H; y++) {
        for (x = 0; x < CDG_W; x++) {
            int sx = x - dx;
            int sy = y - dy;
            if (copy) {
                sx %= CDG_W;
                if (sx < 0) {
                    sx += CDG_W;
                }
                sy %= CDG_H;
                if (sy < 0) {
                    sy += CDG_H;
                }
                s_pix[y * CDG_W + x] = tmp[sy * CDG_W + sx];
            } else if (sx >= 0 && sx < CDG_W && sy >= 0 && sy < CDG_H) {
                s_pix[y * CDG_W + x] = tmp[sy * CDG_W + sx];
            } else {
                s_pix[y * CDG_W + x] = (unsigned char)color;
            }
        }
    }
    s_dirty = 1;
}

static void apply_pkt(const unsigned char *p)
{
    int inst;
    if ((p[0] & CDG_MASK) != CDG_CMD) {
        return;
    }
    inst = p[1] & CDG_MASK;
    switch (inst) {
    case CDG_MEM_PRESET:
        if ((p[4 + 1] & 0x0F) == 0) {
            fill_all(p[4] & 0x0F);
        }
        break;
    case CDG_BORDER_PRESET:
        fill_border(p[4] & 0x0F);
        break;
    case CDG_TILE:
        tile(p + 4, 0);
        break;
    case CDG_TILE_XOR:
        tile(p + 4, 1);
        break;
    case CDG_SCROLL_PRESET:
        scroll(p + 4, 0);
        break;
    case CDG_SCROLL_COPY:
        scroll(p + 4, 1);
        break;
    case CDG_TRANSPARENT:
        s_trans = p[4] & 0x0F;
        s_dirty = 1;
        break;
    case CDG_CLUT_LOW:
        load_clut(p + 4, 0);
        break;
    case CDG_CLUT_HIGH:
        load_clut(p + 4, 1);
        break;
    default:
        break;
    }
}

static void reset_canvas(void)
{
    int i;
    memset(s_pix, 0, sizeof(s_pix));
    s_hoff = s_voff = 0;
    s_trans = -1;
    s_applied = 0;
    for (i = 0; i < 16; i++) {
        s_clut[i] = pack_rgb(0, 0, 0);
    }
    s_dirty = 1;
}

static unsigned compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256];
        glGetShaderInfoLog(s, (GLsizei)sizeof(log), NULL, log);
        fprintf(stderr, "StOMP: CDG shader: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static void drop_gl(void)
{
    if (s_tex) {
        glDeleteTextures(1, &s_tex);
        s_tex = 0;
    }
    if (s_vbo) {
        glDeleteBuffers(1, &s_vbo);
        s_vbo = 0;
    }
    if (s_vao) {
        glDeleteVertexArrays(1, &s_vao);
        s_vao = 0;
    }
    if (s_prog) {
        glDeleteProgram(s_prog);
        s_prog = 0;
    }
    s_gl_ok = 0;
}

static int ensure_gl(void)
{
    static const char *vs =
        "#version 330 core\n"
        "layout(location=0) in vec2 a_pos;\n"
        "layout(location=1) in vec2 a_uv;\n"
        "out vec2 v_uv;\n"
        "void main(){\n"
        "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
        "  v_uv = a_uv;\n"
        "}\n";
    static const char *fs =
        "#version 330 core\n"
        "in vec2 v_uv;\n"
        "out vec4 frag;\n"
        "uniform sampler2D u_tex;\n"
        "void main(){ frag = texture(u_tex, v_uv); }\n";
    GLuint vsh, fsh;
    GLint ok = 0;
    if (s_gl_ok) {
        return 0;
    }
    vsh = compile_shader(GL_VERTEX_SHADER, vs);
    fsh = compile_shader(GL_FRAGMENT_SHADER, fs);
    if (!vsh || !fsh) {
        if (vsh) {
            glDeleteShader(vsh);
        }
        if (fsh) {
            glDeleteShader(fsh);
        }
        return -1;
    }
    s_prog = glCreateProgram();
    glAttachShader(s_prog, vsh);
    glAttachShader(s_prog, fsh);
    glBindAttribLocation(s_prog, 0, "a_pos");
    glBindAttribLocation(s_prog, 1, "a_uv");
    glLinkProgram(s_prog);
    glDeleteShader(vsh);
    glDeleteShader(fsh);
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(s_prog);
        s_prog = 0;
        return -1;
    }
    s_u_tex = glGetUniformLocation(s_prog, "u_tex");
    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(6 * 4 * sizeof(float)), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
    glBindVertexArray(0);
    glGenTextures(1, &s_tex);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, CDG_W, CDG_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, s_rgba);
    s_gl_ok = 1;
    s_dirty = 1;
    return 0;
}

static void compose_rgba(void)
{
    int x, y, trans_n = 0;
    static const int ox[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
    static const int oy[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };
    for (y = 0; y < CDG_H; y++) {
        int sy = y + s_voff;
        if (sy >= CDG_H) {
            sy -= CDG_H;
        }
        for (x = 0; x < CDG_W; x++) {
            int sx = x + s_hoff;
            int idx, pi, ink;
            unsigned c;
            unsigned char *o;
            if (sx >= CDG_W) {
                sx -= CDG_W;
            }
            idx = s_pix[sy * CDG_W + sx] & 0x0F;
            c = s_clut[idx];
            pi = (y * CDG_W + x) * 4;
            o = s_rgba + pi;
            o[0] = (unsigned char)(c & 0xFF);
            o[1] = (unsigned char)((c >> 8) & 0xFF);
            o[2] = (unsigned char)((c >> 16) & 0xFF);
            ink = !(s_trans >= 0 && idx == s_trans);
            if (!ink) {
                trans_n++;
            }
            o[3] = ink ? 255 : 0;
            s_ink[y * CDG_W + x] = (unsigned char)ink;
        }
    }
    if (s_trans < 0 || trans_n < CDG_HALO_MIN_TRANS) {
        if (s_trans >= 0 && trans_n > 0) {
            for (y = 0; y < CDG_H; y++) {
                for (x = 0; x < CDG_W; x++) {
                    unsigned char *o;
                    if (s_ink[y * CDG_W + x]) {
                        continue;
                    }
                    o = s_rgba + (y * CDG_W + x) * 4;
                    o[0] = 0;
                    o[1] = 0;
                    o[2] = 0;
                    o[3] = 255;
                }
            }
        }
        return;
    }
    for (y = 0; y < CDG_H; y++) {
        for (x = 0; x < CDG_W; x++) {
            int k, dark = 0, light = 0;
            unsigned char *o;
            if (s_ink[y * CDG_W + x]) {
                continue;
            }
            for (k = 0; k < 8; k++) {
                int nx = x + ox[k];
                int ny = y + oy[k];
                unsigned char *n;
                int luma;
                if (nx < 0 || ny < 0 || nx >= CDG_W || ny >= CDG_H) {
                    continue;
                }
                if (!s_ink[ny * CDG_W + nx]) {
                    continue;
                }
                n = s_rgba + (ny * CDG_W + nx) * 4;
                luma = (int)n[0] + (int)n[1] + (int)n[2];
                if (luma < 384) {
                    dark = 1;
                } else {
                    light = 1;
                }
            }
            if (!dark && !light) {
                continue;
            }
            o = s_rgba + (y * CDG_W + x) * 4;
            if (dark) {
                o[0] = 236;
                o[1] = 236;
                o[2] = 236;
            } else {
                o[0] = 18;
                o[1] = 18;
                o[2] = 18;
            }
            o[3] = 255;
        }
    }
}

#define CDG_MAX_BYTES (16 * 1024 * 1024)

static int cdg_gunzip(const unsigned char *in, size_t in_n,
                      unsigned char **out, size_t *out_n)
{
    z_stream zs;
    unsigned char *buf;
    size_t cap = 256 * 1024;
    int zr;

    if (in_n >= 8) {
        size_t isize = (size_t)in[in_n - 4] |
                       ((size_t)in[in_n - 3] << 8) |
                       ((size_t)in[in_n - 2] << 16) |
                       ((size_t)in[in_n - 1] << 24);
        if (isize >= CDG_PKT && isize <= (size_t)CDG_MAX_BYTES) {
            cap = isize;
        }
    }
    buf = (unsigned char *)malloc(cap);
    if (!buf) {
        return -1;
    }
    memset(&zs, 0, sizeof(zs));
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)in_n;
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) {
        free(buf);
        return -1;
    }
    for (;;) {
        size_t used = (size_t)zs.total_out;
        zs.next_out = (Bytef *)(buf + used);
        zs.avail_out = (uInt)(cap - used);
        zr = inflate(&zs, Z_NO_FLUSH);
        if (zr == Z_STREAM_END) {
            break;
        }
        if (zr != Z_OK || zs.avail_out != 0) {
            inflateEnd(&zs);
            free(buf);
            return -1;
        }
        if (cap >= (size_t)CDG_MAX_BYTES) {
            inflateEnd(&zs);
            free(buf);
            return -1;
        }
        cap *= 2;
        if (cap > (size_t)CDG_MAX_BYTES) {
            cap = (size_t)CDG_MAX_BYTES;
        }
        {
            unsigned char *nbuf = (unsigned char *)realloc(buf, cap);
            if (!nbuf) {
                inflateEnd(&zs);
                free(buf);
                return -1;
            }
            buf = nbuf;
        }
    }
    inflateEnd(&zs);
    *out = buf;
    *out_n = (size_t)zs.total_out;
    return 0;
}

int cdg_open(const char *path)
{
    FILE *f;
    long sz;
    unsigned char *raw = NULL;
    unsigned char *pkt = NULL;
    size_t pkt_n = 0;

    cdg_close();
    if (!path || !path[0]) {
        return -1;
    }
    f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    sz = ftell(f);
    if (sz < 18 || sz > CDG_MAX_BYTES) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    raw = (unsigned char *)malloc((size_t)sz);
    if (!raw) {
        fclose(f);
        return -1;
    }
    if (fread(raw, 1, (size_t)sz, f) != (size_t)sz) {
        free(raw);
        fclose(f);
        return -1;
    }
    fclose(f);
    if (sz >= 2 && raw[0] == 0x1f && raw[1] == 0x8b) {
        if (cdg_gunzip(raw, (size_t)sz, &pkt, &pkt_n) != 0) {
            free(raw);
            return -1;
        }
        free(raw);
    } else {
        pkt = raw;
        pkt_n = (size_t)sz;
    }
    if (pkt_n < CDG_PKT || pkt_n > (size_t)CDG_MAX_BYTES) {
        free(pkt);
        return -1;
    }
    s_data = pkt;
    s_npkt = (int)(pkt_n / CDG_PKT);
    reset_canvas();
    s_on = 1;
    fprintf(stderr, "StOMP: CDG lyrics %s (%d packets)\n", path, s_npkt);
    return 0;
}

void cdg_close(void)
{
    free(s_data);
    s_data = NULL;
    s_npkt = 0;
    s_on = 0;
    reset_canvas();
}

void cdg_lost_gl(void)
{
    drop_gl();
}

void cdg_shutdown(void)
{
    cdg_close();
    drop_gl();
}

int cdg_active(void)
{
    return s_on && s_data && s_npkt > 0;
}

void cdg_update(double seconds)
{
    int want, i;
    if (!cdg_active()) {
        return;
    }
    if (seconds < 0) {
        seconds = 0;
    }
    want = (int)(seconds * CDG_PPS + 0.0001);
    if (want < 0) {
        want = 0;
    }
    if (want > s_npkt) {
        want = s_npkt;
    }
    if (want < s_applied) {
        reset_canvas();
    }
    for (i = s_applied; i < want; i++) {
        apply_pkt(s_data + (size_t)i * CDG_PKT);
    }
    s_applied = want;
}

void cdg_present(int win_w, int win_h)
{
    float verts[24];
    float x0, x1, y0, y1;
    int scale, dw, dh, px, py;
    if (!cdg_active() || win_w < 1 || win_h < 1) {
        return;
    }
    if (ensure_gl() != 0) {
        return;
    }
    if (s_dirty) {
        compose_rgba();
        glBindTexture(GL_TEXTURE_2D, s_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, CDG_W, CDG_H, GL_RGBA, GL_UNSIGNED_BYTE, s_rgba);
        s_dirty = 0;
    }
    scale = win_w / CDG_W;
    if (win_h / CDG_H < scale) {
        scale = win_h / CDG_H;
    }
    if (scale < 1) {
        scale = 1;
    }
    dw = CDG_W * scale;
    dh = CDG_H * scale;
    px = (win_w - dw) / 2;
    py = (win_h - dh) / 2;
    x0 = 2.f * (float)px / (float)win_w - 1.f;
    x1 = 2.f * (float)(px + dw) / (float)win_w - 1.f;
    y0 = 2.f * (float)py / (float)win_h - 1.f;
    y1 = 2.f * (float)(py + dh) / (float)win_h - 1.f;
    verts[0] = x0;
    verts[1] = y1;
    verts[2] = 0.f;
    verts[3] = 0.f;
    verts[4] = x1;
    verts[5] = y1;
    verts[6] = 1.f;
    verts[7] = 0.f;
    verts[8] = x0;
    verts[9] = y0;
    verts[10] = 0.f;
    verts[11] = 1.f;
    verts[12] = x1;
    verts[13] = y1;
    verts[14] = 1.f;
    verts[15] = 0.f;
    verts[16] = x1;
    verts[17] = y0;
    verts[18] = 1.f;
    verts[19] = 1.f;
    verts[20] = x0;
    verts[21] = y0;
    verts[22] = 0.f;
    verts[23] = 1.f;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, win_w, win_h);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(s_prog);
    glUniform1i(s_u_tex, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glEnable(GL_BLEND);
}
