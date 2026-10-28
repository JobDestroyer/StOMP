/* SPDX-License-Identifier: LGPL-2.1-only
 * Copyright (C) 2026 JobDestroyer
 *
 * Read-only ZIP (store + deflate). Enough for MP3+G karaoke zips.
 */

#ifndef STOMP_ZIPREAD_H
#define STOMP_ZIPREAD_H

#include <stddef.h>
#include <stdint.h>

#define ZIPREAD_NAME_MAX 512
#define ZIPREAD_MAX_ENTS 256

typedef struct {
    char name[ZIPREAD_NAME_MAX];
    uint16_t method;
    uint32_t local_off;
    uint32_t comp_size;
    uint32_t uncomp_size;
} ZipEnt;

int zip_list(const char *path, ZipEnt **ents, int *n);
void zip_list_free(ZipEnt *ents);
int zip_extract(const char *path, const ZipEnt *ent, unsigned char **data, size_t *len);
int zip_extract_file(const char *path, const ZipEnt *ent, const char *dest);

#endif
