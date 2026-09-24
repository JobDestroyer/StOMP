/*
 * Single-file launcher. The packed stomp is: this stub + payload + trailer.
 * Payload is extracted once to $XDG_CACHE_HOME/vibe/r/<crc>/ then exec'd.
 * libprojectM stays a separate .so inside the payload (LGPL).
 */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAGIC "VIBEPK01"
#define TRAILER 24
#define STAMP ".ok"

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

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t rd_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

static int mkdir_p(const char *path)
{
    char tmp[1024];
    size_t n = strlen(path);
    if (n >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, path, n + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int mkdir_parent(const char *path)
{
    char tmp[1024];
    char *slash;
    snprintf(tmp, sizeof(tmp), "%s", path);
    slash = strrchr(tmp, '/');
    if (!slash) {
        return 0;
    }
    *slash = '\0';
    if (!tmp[0]) {
        return 0;
    }
    return mkdir_p(tmp);
}

static int rm_tree(const char *path)
{
    struct stat st;
    DIR *d;
    struct dirent *de;
    if (lstat(path, &st) != 0) {
        return errno == ENOENT ? 0 : -1;
    }
    if (S_ISDIR(st.st_mode)) {
        d = opendir(path);
        if (!d) {
            return -1;
        }
        while ((de = readdir(d)) != NULL) {
            char child[2048];
            if (de->d_name[0] == '.' &&
                (de->d_name[1] == '\0' ||
                 (de->d_name[1] == '.' && de->d_name[2] == '\0'))) {
                continue;
            }
            snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
            if (rm_tree(child) != 0) {
                closedir(d);
                return -1;
            }
        }
        closedir(d);
        return rmdir(path);
    }
    return unlink(path);
}

static int bad_rel_path(const char *path)
{
    const char *p;
    if (!path || !path[0] || path[0] == '/') {
        return 1;
    }
    p = path;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        if (n == 0 || (n == 1 && p[0] == '.') ||
            (n == 2 && p[0] == '.' && p[1] == '.')) {
            return 1;
        }
        if (!slash) {
            break;
        }
        p = slash + 1;
    }
    return 0;
}

static void cache_root(char *out, size_t n, uint32_t crc)
{
    const char *env = getenv("VIBE_RUNTIME_DIR");
    const char *xdg;
    if (env && env[0]) {
        snprintf(out, n, "%s", env);
        return;
    }
    xdg = getenv("XDG_CACHE_HOME");
    if (xdg && xdg[0]) {
        snprintf(out, n, "%s/vibe/r/%08x", xdg, crc);
        return;
    }
    env = getenv("HOME");
    snprintf(out, n, "%s/.cache/vibe/r/%08x", (env && env[0]) ? env : ".", crc);
}

static int extract(FILE *in, uint64_t payload_off, uint32_t nfiles, const char *dir,
                   uint32_t *out_crc)
{
    uint8_t hdr[1 + 2 + 8];
    char path[1024];
    char full[2048];
    char target[1024];
    uint8_t buf[64 * 1024];
    uint32_t crc = 0;

    if (fseek(in, (long)payload_off, SEEK_SET) != 0) {
        return -1;
    }
    for (uint32_t i = 0; i < nfiles; i++) {
        uint8_t type;
        uint16_t nlen;
        uint64_t size, left;
        if (fread(hdr, 1, sizeof(hdr), in) != sizeof(hdr)) {
            return -1;
        }
        crc = crc32_update(crc, hdr, sizeof(hdr));
        type = hdr[0];
        nlen = rd_u16(hdr + 1);
        size = rd_u64(hdr + 3);
        if (nlen == 0 || nlen >= sizeof(path)) {
            return -1;
        }
        if (fread(path, 1, nlen, in) != nlen) {
            return -1;
        }
        path[nlen] = '\0';
        crc = crc32_update(crc, (const uint8_t *)path, nlen);
        if (bad_rel_path(path)) {
            fprintf(stderr, "StOMP: bad packed path %s\n", path);
            return -1;
        }
        snprintf(full, sizeof(full), "%s/%s", dir, path);
        if (mkdir_parent(full) != 0) {
            fprintf(stderr, "StOMP: mkdir for %s: %s\n", full, strerror(errno));
            return -1;
        }
        if (type == 1) {
            if (size >= sizeof(target)) {
                return -1;
            }
            if (fread(target, 1, (size_t)size, in) != (size_t)size) {
                return -1;
            }
            target[size] = '\0';
            crc = crc32_update(crc, (const uint8_t *)target, (size_t)size);
            if (bad_rel_path(target)) {
                fprintf(stderr, "StOMP: bad packed symlink %s -> %s\n", path, target);
                return -1;
            }
            unlink(full);
            if (symlink(target, full) != 0) {
                fprintf(stderr, "StOMP: symlink %s: %s\n", full, strerror(errno));
                return -1;
            }
            continue;
        }
        if (type != 0) {
            return -1;
        }
        {
            FILE *out = fopen(full, "wb");
            if (!out) {
                fprintf(stderr, "StOMP: write %s: %s\n", full, strerror(errno));
                return -1;
            }
            left = size;
            while (left > 0) {
                size_t chunk = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
                if (fread(buf, 1, chunk, in) != chunk) {
                    fclose(out);
                    return -1;
                }
                crc = crc32_update(crc, buf, chunk);
                if (fwrite(buf, 1, chunk, out) != chunk) {
                    fclose(out);
                    return -1;
                }
                left -= chunk;
            }
            if (fclose(out) != 0) {
                return -1;
            }
        }
        if (strncmp(path, "lib", 3) == 0 || strcmp(path, "stomp") == 0) {
            chmod(full, 0755);
        } else {
            chmod(full, 0644);
        }
    }
    if (out_crc) {
        *out_crc = crc;
    }
    return 0;
}

static int write_stamp(const char *dir, uint32_t crc)
{
    char path[2048];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", dir, STAMP);
    f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    if (fprintf(f, "%08x\n", crc) < 0) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

static int ready(const char *dir, uint32_t crc)
{
    char inner[2048], stamp[2048], got[16];
    struct stat st;
    FILE *f;
    snprintf(inner, sizeof(inner), "%s/stomp", dir);
    snprintf(stamp, sizeof(stamp), "%s/%s", dir, STAMP);
    if (stat(inner, &st) != 0 || !S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR)) {
        return 0;
    }
    f = fopen(stamp, "rb");
    if (!f) {
        return 0;
    }
    if (!fgets(got, (int)sizeof(got), f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    {
        char want[16];
        snprintf(want, sizeof(want), "%08x\n", crc);
        if (strcmp(got, want) != 0) {
            snprintf(want, sizeof(want), "%08x", crc);
            if (strncmp(got, want, 8) != 0) {
                return 0;
            }
        }
    }
    return 1;
}

static int is_runtime_name(const char *name)
{
    size_t i, n;
    if (!name || !name[0]) {
        return 0;
    }
    n = strlen(name);
    if (n >= 5 && strcmp(name + n - 5, ".part") == 0) {
        n -= 5;
    }
    if (n != 8) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static void gc_old_runtimes(const char *keep_dir)
{
    char parent[1024], keep[64];
    char *slash;
    DIR *d;
    struct dirent *de;
    snprintf(parent, sizeof(parent), "%s", keep_dir);
    slash = strrchr(parent, '/');
    if (!slash || slash == parent) {
        return;
    }
    snprintf(keep, sizeof(keep), "%s", slash + 1);
    *slash = '\0';
    if (strlen(parent) < 7 || strcmp(parent + strlen(parent) - 6, "vibe/r") != 0) {
        if (!(strlen(parent) >= 2 && strcmp(parent + strlen(parent) - 1, "r") == 0 &&
              strstr(parent, "vibe"))) {
            return;
        }
    }
    d = opendir(parent);
    if (!d) {
        return;
    }
    while ((de = readdir(d)) != NULL) {
        char child[2048];
        if (de->d_name[0] == '.') {
            continue;
        }
        if (strcmp(de->d_name, keep) == 0) {
            continue;
        }
        if (!is_runtime_name(de->d_name)) {
            continue;
        }
        snprintf(child, sizeof(child), "%s/%s", parent, de->d_name);
        fprintf(stderr, "StOMP: removing old runtime %s\n", child);
        rm_tree(child);
    }
    closedir(d);
}

int main(int argc, char **argv)
{
    char self[1024];
    char dir[1024];
    char tmp[1100];
    char inner[2048];
    uint8_t tr[TRAILER];
    FILE *in;
    long flen;
    uint64_t payload_size, payload_off;
    uint32_t nfiles, crc, got_crc = 0;
    ssize_t n;
    char **nargv;
    int i;

    n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n < 0) {
        snprintf(self, sizeof(self), "%s", argv[0] ? argv[0] : "stomp");
    } else {
        self[n] = '\0';
    }

    in = fopen(self, "rb");
    if (!in) {
        fprintf(stderr, "StOMP: open %s: %s\n", self, strerror(errno));
        return 1;
    }
    if (fseek(in, 0, SEEK_END) != 0) {
        fclose(in);
        return 1;
    }
    flen = ftell(in);
    if (flen < TRAILER) {
        fprintf(stderr, "StOMP: not a packed binary\n");
        fclose(in);
        return 1;
    }
    if (fseek(in, flen - TRAILER, SEEK_SET) != 0 || fread(tr, 1, TRAILER, in) != TRAILER) {
        fclose(in);
        return 1;
    }
    if (memcmp(tr + 16, MAGIC, 8) != 0) {
        fprintf(stderr, "StOMP: missing payload (rebuild with packembed)\n");
        fclose(in);
        return 1;
    }
    payload_size = rd_u64(tr);
    nfiles = rd_u32(tr + 8);
    crc = rd_u32(tr + 12);
    if (payload_size == 0 || nfiles == 0 ||
        (uint64_t)flen < (uint64_t)TRAILER + payload_size) {
        fprintf(stderr, "StOMP: corrupt payload trailer\n");
        fclose(in);
        return 1;
    }
    payload_off = (uint64_t)flen - (uint64_t)TRAILER - payload_size;
    cache_root(dir, sizeof(dir), crc);

    if (!ready(dir, crc)) {
        fprintf(stderr, "StOMP: unpacking runtime to %s\n", dir);
        snprintf(tmp, sizeof(tmp), "%s.part", dir);
        rm_tree(tmp);
        rm_tree(dir);
        if (mkdir_p(tmp) != 0) {
            fprintf(stderr, "StOMP: mkdir %s: %s\n", tmp, strerror(errno));
            fclose(in);
            return 1;
        }
        if (extract(in, payload_off, nfiles, tmp, &got_crc) != 0 || got_crc != crc) {
            fprintf(stderr, "StOMP: extract failed%s\n",
                    got_crc != crc && got_crc != 0 ? " (crc mismatch)" : "");
            rm_tree(tmp);
            fclose(in);
            return 1;
        }
        if (write_stamp(tmp, crc) != 0 || rename(tmp, dir) != 0) {
            fprintf(stderr, "StOMP: finalize runtime: %s\n", strerror(errno));
            rm_tree(tmp);
            fclose(in);
            return 1;
        }
    }
    fclose(in);
    gc_old_runtimes(dir);

    snprintf(inner, sizeof(inner), "%s/stomp", dir);
    nargv = (char **)malloc((size_t)(argc + 1) * sizeof(char *));
    if (!nargv) {
        return 1;
    }
    nargv[0] = inner;
    for (i = 1; i < argc; i++) {
        nargv[i] = argv[i];
    }
    nargv[argc] = NULL;
    execv(inner, nargv);
    fprintf(stderr, "StOMP: exec %s: %s\n", inner, strerror(errno));
    free(nargv);
    return 1;
}
