#ifndef VIBE_RADIO_H
#define VIBE_RADIO_H

#include "config.h"

typedef struct {
    char name[VIBE_NAME_MAX];
    char sub[VIBE_NAME_MAX];
    char url[VIBE_PATH_MAX];
} RadioItem;

typedef struct {
    char name[VIBE_NAME_MAX];
    char code[8];
    int stationcount;
} RadioCountry;

void radio_init(void);

int radio_fetch_soma(char *err, int errn);
int radio_soma_count(void);
int radio_soma_at(int i, RadioItem *out);

int radio_fetch_countries(char *err, int errn);
int radio_country_count(void);
int radio_country_at(int i, RadioCountry *out);

int radio_fetch_stations(const char *country_code, char *err, int errn);
int radio_station_count(void);
int radio_station_at(int i, RadioItem *out);

int radio_resolve_play_url(const char *in, char *out, int n);
void radio_name_from_url(const char *url, char *out, int n);

#endif
