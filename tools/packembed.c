/* Host tool: stub ELF + payload + trailer -> single-file stomp. */
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAGIC "VIBEPK01"

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t m = (uint32_t)-(int)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & m);
        }
    }
    return ~crc;
}

static int wr_u8(FILE *f, uint8_t v, uint32_t *crc)
{
    if (fwrite(&v, 1, 1, f) != 1) {
        return -1;
    }
    *crc = crc32_update(*crc, &v, 1);
    return 0;
}

static int wr_u16(FILE *f, uint16_t v, uint32_t *crc)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    if (fwrite(b, 1, 2, f) != 2) {
        return -1;
    }
    *crc = crc32_update(*crc, b, 2);
    return 0;
}

static int wr_u64(FILE *f, uint64_t v, uint32_t *crc)
{
    uint8_t b[8];
    for (int i = 0; i < 8; i++) {
        b[i] = (uint8_t)(v >> (8 * i));
    }
    if (fwrite(b, 1, 8, f) != 8) {
        return -1;
    }
    *crc = crc32_update(*crc, b, 8);
    return 0;
}

static int copy_file_payload(FILE *out, const char *src, const char *dst, uint8_t type,
                             const char *link_target, uint32_t *crc, uint32_t *nfiles)
{
    uint16_t nlen;
    uint64_t size = 0;
    FILE *in = NULL;
    uint8_t buf[64 * 1024];
    size_t nr;

    nlen = (uint16_t)strlen(dst);
    if (type == 0) {
        in = fopen(src, "rb");
        if (!in) {
            fprintf(stderr, "packembed: cannot read %s: %s\n", src, strerror(errno));
            return -1;
        }
        if (fseek(in, 0, SEEK_END) != 0) {
            fclose(in);
            return -1;
        }
        long sz = ftell(in);
        if (sz < 0) {
            fclose(in);
            return -1;
        }
        size = (uint64_t)sz;
        rewind(in);
    } else {
        size = (uint64_t)strlen(link_target);
    }

    if (wr_u8(out, type, crc) != 0 || wr_u16(out, nlen, crc) != 0 || wr_u64(out, size, crc) != 0) {
        if (in) {
            fclose(in);
        }
        return -1;
    }
    if (fwrite(dst, 1, nlen, out) != nlen) {
        if (in) {
            fclose(in);
        }
        return -1;
    }
    *crc = crc32_update(*crc, (const uint8_t *)dst, nlen);

    if (type == 1) {
        if (fwrite(link_target, 1, (size_t)size, out) != (size_t)size) {
            return -1;
        }
        *crc = crc32_update(*crc, (const uint8_t *)link_target, (size_t)size);
    } else {
        while ((nr = fread(buf, 1, sizeof(buf), in)) > 0) {
            if (fwrite(buf, 1, nr, out) != nr) {
                fclose(in);
                return -1;
            }
            *crc = crc32_update(*crc, buf, nr);
        }
        fclose(in);
    }
    (*nfiles)++;
    return 0;
}

static int add_tree(FILE *out, const char *src_dir, const char *dst_prefix,
                    uint32_t *crc, uint32_t *nfiles)
{
    DIR *d = opendir(src_dir);
    struct dirent *de;
    if (!d) {
        fprintf(stderr, "packembed: dir %s: %s\n", src_dir, strerror(errno));
        return -1;
    }
    while ((de = readdir(d)) != NULL) {
        char src[2048], dst[2048];
        struct stat st;
        if (de->d_name[0] == '.') {
            continue;
        }
        snprintf(src, sizeof(src), "%s/%s", src_dir, de->d_name);
        snprintf(dst, sizeof(dst), "%s/%s", dst_prefix, de->d_name);
        if (stat(src, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (add_tree(out, src, dst, crc, nfiles) != 0) {
                closedir(d);
                return -1;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (copy_file_payload(out, src, dst, 0, NULL, crc, nfiles) != 0) {
                closedir(d);
                return -1;
            }
        }
    }
    closedir(d);
    return 0;
}

static int copy_stub(FILE *out, const char *stub_path)
{
    FILE *in = fopen(stub_path, "rb");
    uint8_t buf[64 * 1024];
    size_t nr;
    if (!in) {
        fprintf(stderr, "packembed: stub %s: %s\n", stub_path, strerror(errno));
        return -1;
    }
    while ((nr = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, nr, out) != nr) {
            fclose(in);
            return -1;
        }
    }
    fclose(in);
    return 0;
}

int main(int argc, char **argv)
{
    const char *out_path = NULL;
    const char *stub = NULL;
    FILE *out;
    uint32_t crc = 0;
    uint32_t nfiles = 0;
    long payload_start;
    uint64_t payload_size;
    int i;

    if (argc < 4) {
        fprintf(stderr,
                "usage: packembed -o packed stub --file src:dst [--dir src:dst] [--link name:target]\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        }
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            i++;
            continue;
        }
        if (strcmp(argv[i], "--file") == 0 || strcmp(argv[i], "--link") == 0 ||
            strcmp(argv[i], "--dir") == 0) {
            i++;
            continue;
        }
        if (argv[i][0] != '-') {
            stub = argv[i];
            break;
        }
    }
    if (!out_path || !stub) {
        fprintf(stderr, "packembed: need -o outfile and stub path\n");
        return 1;
    }

    out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "packembed: %s: %s\n", out_path, strerror(errno));
        return 1;
    }
    if (copy_stub(out, stub) != 0) {
        fclose(out);
        return 1;
    }
    payload_start = ftell(out);
    if (payload_start < 0) {
        fclose(out);
        return 1;
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            char *spec = argv[++i];
            char *colon = strrchr(spec, ':');
            char src[1024], dst[1024];
            if (!colon || colon == spec) {
                fprintf(stderr, "packembed: --file wants src:dst\n");
                fclose(out);
                return 1;
            }
            snprintf(src, sizeof(src), "%.*s", (int)(colon - spec), spec);
            snprintf(dst, sizeof(dst), "%s", colon + 1);
            if (copy_file_payload(out, src, dst, 0, NULL, &crc, &nfiles) != 0) {
                fclose(out);
                return 1;
            }
        } else if (strcmp(argv[i], "--link") == 0 && i + 1 < argc) {
            char *spec = argv[++i];
            char *colon = strrchr(spec, ':');
            char name[1024], target[1024];
            if (!colon) {
                fprintf(stderr, "packembed: --link wants name:target\n");
                fclose(out);
                return 1;
            }
            snprintf(name, sizeof(name), "%.*s", (int)(colon - spec), spec);
            snprintf(target, sizeof(target), "%s", colon + 1);
            if (copy_file_payload(out, NULL, name, 1, target, &crc, &nfiles) != 0) {
                fclose(out);
                return 1;
            }
        } else if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            char *spec = argv[++i];
            char *colon = strrchr(spec, ':');
            char src[1024], dst[1024];
            if (!colon || colon == spec) {
                fprintf(stderr, "packembed: --dir wants src:dst\n");
                fclose(out);
                return 1;
            }
            snprintf(src, sizeof(src), "%.*s", (int)(colon - spec), spec);
            snprintf(dst, sizeof(dst), "%s", colon + 1);
            if (add_tree(out, src, dst, &crc, &nfiles) != 0) {
                fclose(out);
                return 1;
            }
        }
    }

    {
        long end = ftell(out);
        uint8_t tr[24];
        if (end < 0) {
            fclose(out);
            return 1;
        }
        payload_size = (uint64_t)(end - payload_start);
        memset(tr, 0, sizeof(tr));
        for (int b = 0; b < 8; b++) {
            tr[b] = (uint8_t)(payload_size >> (8 * b));
        }
        tr[8] = (uint8_t)nfiles;
        tr[9] = (uint8_t)(nfiles >> 8);
        tr[10] = (uint8_t)(nfiles >> 16);
        tr[11] = (uint8_t)(nfiles >> 24);
        tr[12] = (uint8_t)crc;
        tr[13] = (uint8_t)(crc >> 8);
        tr[14] = (uint8_t)(crc >> 16);
        tr[15] = (uint8_t)(crc >> 24);
        memcpy(tr + 16, MAGIC, 8);
        if (fwrite(tr, 1, 24, out) != 24) {
            fclose(out);
            return 1;
        }
    }
    if (fclose(out) != 0) {
        return 1;
    }
    fprintf(stderr, "packembed: %s (%u files, payload %llu bytes, crc %08x)\n",
            out_path, nfiles, (unsigned long long)payload_size, crc);
    return 0;
}
