/* SPDX-License-Identifier: LGPL-2.1-only
 * Copyright (C) 2026 JobDestroyer
 */

#include "zipread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define ZIP_EOCD 0x06054b50u
#define ZIP_CDIR 0x02014b50u
#define ZIP_LOC  0x04034b50u
#define ZIP_MAX_BYTES (64 * 1024 * 1024)

static uint16_t u16le(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t u32le(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_fully(FILE *f, void *buf, size_t n)
{
    return fread(buf, 1, n, f) == n ? 0 : -1;
}

static int zip_name_skip(const char *name)
{
    const char *base = strrchr(name, '/');
    const char *alt = strrchr(name, '\\');
    if (alt && (!base || alt > base)) {
        base = alt;
    }
    base = base ? base + 1 : name;
    if (!base[0] || base[0] == '.') {
        return 1;
    }
    if (strstr(name, "__MACOSX")) {
        return 1;
    }
    return 0;
}

int zip_list(const char *path, ZipEnt **ents, int *n)
{
    FILE *f;
    long sz, i, start;
    unsigned char tail[64 * 1024 + 22];
    size_t tail_n;
    uint32_t cd_off = 0, cd_size = 0;
    uint16_t nent = 0;
    unsigned char *cd = NULL;
    ZipEnt *out = NULL;
    int found = 0, count = 0, k;

    if (ents) {
        *ents = NULL;
    }
    if (n) {
        *n = 0;
    }
    if (!path || !ents || !n) {
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
    if (sz < 22) {
        fclose(f);
        return -1;
    }
    tail_n = (size_t)sz < sizeof(tail) ? (size_t)sz : sizeof(tail);
    if (fseek(f, sz - (long)tail_n, SEEK_SET) != 0 ||
        read_fully(f, tail, tail_n) != 0) {
        fclose(f);
        return -1;
    }
    for (i = (long)tail_n - 22; i >= 0; i--) {
        if (u32le(tail + i) == ZIP_EOCD) {
            uint16_t comment = u16le(tail + i + 20);
            if (i + 22 + (long)comment <= (long)tail_n ||
                (size_t)sz == tail_n) {
                nent = u16le(tail + i + 10);
                cd_size = u32le(tail + i + 12);
                cd_off = u32le(tail + i + 16);
                found = 1;
                break;
            }
        }
    }
    if (!found || nent == 0 || cd_size == 0 || cd_size > 8 * 1024 * 1024) {
        fclose(f);
        return -1;
    }
    cd = (unsigned char *)malloc(cd_size);
    if (!cd) {
        fclose(f);
        return -1;
    }
    if (fseek(f, (long)cd_off, SEEK_SET) != 0 || read_fully(f, cd, cd_size) != 0) {
        free(cd);
        fclose(f);
        return -1;
    }
    fclose(f);
    out = (ZipEnt *)calloc(ZIPREAD_MAX_ENTS, sizeof(ZipEnt));
    if (!out) {
        free(cd);
        return -1;
    }
    start = 0;
    for (k = 0; start + 46 <= (long)cd_size && count < ZIPREAD_MAX_ENTS; k++) {
        uint16_t method, flags, nlen, xlen, clen;
        uint32_t csz, usz, loff;
        size_t ncopy;
        if (u32le(cd + start) != ZIP_CDIR) {
            break;
        }
        flags = u16le(cd + start + 8);
        method = u16le(cd + start + 10);
        csz = u32le(cd + start + 20);
        usz = u32le(cd + start + 24);
        nlen = u16le(cd + start + 28);
        xlen = u16le(cd + start + 30);
        clen = u16le(cd + start + 32);
        loff = u32le(cd + start + 42);
        if (start + 46 + nlen > (long)cd_size) {
            break;
        }
        if (!(flags & 1u) && nlen > 0 && usz > 0 && usz <= ZIP_MAX_BYTES &&
            (method == 0 || method == 8)) {
            ZipEnt *e = &out[count];
            ncopy = nlen < ZIPREAD_NAME_MAX - 1 ? nlen : ZIPREAD_NAME_MAX - 1;
            memcpy(e->name, cd + start + 46, ncopy);
            e->name[ncopy] = '\0';
            if (e->name[ncopy - 1] != '/' && e->name[ncopy - 1] != '\\' &&
                !zip_name_skip(e->name)) {
                e->method = method;
                e->local_off = loff;
                e->comp_size = csz;
                e->uncomp_size = usz;
                count++;
            }
        }
        start += 46 + nlen + xlen + clen;
    }
    free(cd);
    if (count <= 0) {
        free(out);
        return -1;
    }
    *ents = out;
    *n = count;
    return 0;
}

void zip_list_free(ZipEnt *ents)
{
    free(ents);
}

int zip_extract(const char *path, const ZipEnt *ent, unsigned char **data, size_t *len)
{
    FILE *f;
    unsigned char loc[30];
    uint16_t nlen, xlen;
    unsigned char *comp = NULL;
    unsigned char *out = NULL;

    if (data) {
        *data = NULL;
    }
    if (len) {
        *len = 0;
    }
    if (!path || !ent || !data || !len || ent->uncomp_size == 0 ||
        ent->uncomp_size > ZIP_MAX_BYTES) {
        return -1;
    }
    f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    if (fseek(f, (long)ent->local_off, SEEK_SET) != 0 ||
        read_fully(f, loc, 30) != 0 || u32le(loc) != ZIP_LOC) {
        fclose(f);
        return -1;
    }
    nlen = u16le(loc + 26);
    xlen = u16le(loc + 28);
    if (fseek(f, (long)ent->local_off + 30 + nlen + xlen, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    if (ent->method == 0) {
        if (ent->comp_size != ent->uncomp_size) {
            fclose(f);
            return -1;
        }
        out = (unsigned char *)malloc(ent->uncomp_size);
        if (!out) {
            fclose(f);
            return -1;
        }
        if (read_fully(f, out, ent->uncomp_size) != 0) {
            free(out);
            fclose(f);
            return -1;
        }
        fclose(f);
        *data = out;
        *len = ent->uncomp_size;
        return 0;
    }
    if (ent->comp_size == 0 || ent->comp_size > ZIP_MAX_BYTES) {
        fclose(f);
        return -1;
    }
    comp = (unsigned char *)malloc(ent->comp_size);
    out = (unsigned char *)malloc(ent->uncomp_size);
    if (!comp || !out) {
        free(comp);
        free(out);
        fclose(f);
        return -1;
    }
    if (read_fully(f, comp, ent->comp_size) != 0) {
        free(comp);
        free(out);
        fclose(f);
        return -1;
    }
    fclose(f);
    {
        z_stream zs;
        int zr;
        memset(&zs, 0, sizeof(zs));
        zs.next_in = comp;
        zs.avail_in = ent->comp_size;
        zs.next_out = out;
        zs.avail_out = ent->uncomp_size;
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
            free(comp);
            free(out);
            return -1;
        }
        zr = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        free(comp);
        if (zr != Z_STREAM_END || zs.total_out != ent->uncomp_size) {
            free(out);
            return -1;
        }
    }
    *data = out;
    *len = ent->uncomp_size;
    return 0;
}

int zip_extract_file(const char *path, const ZipEnt *ent, const char *dest)
{
    unsigned char *data = NULL;
    size_t len = 0;
    FILE *o;
    int wr;

    if (zip_extract(path, ent, &data, &len) != 0) {
        return -1;
    }
    {
        char tmp[4096 + 8];
        size_t dn = strlen(dest);
        if (dn == 0 || dn + 6 >= sizeof(tmp)) {
            free(data);
            return -1;
        }
        memcpy(tmp, dest, dn);
        memcpy(tmp + dn, ".part", 6);
        o = fopen(tmp, "wb");
        if (!o) {
            free(data);
            return -1;
        }
        wr = fwrite(data, 1, len, o) == len ? 0 : -1;
        if (wr == 0 && fflush(o) != 0) {
            wr = -1;
        }
        fclose(o);
        free(data);
        if (wr != 0) {
            remove(tmp);
            return -1;
        }
        if (rename(tmp, dest) != 0) {
            remove(tmp);
            return -1;
        }
        return 0;
    }
}
