/*
 * kws_detect.h - plik modelu slowa kluczowego (.kws) i detektor strumieniowy.
 * Wspolne dla firmware (wake.c) i kwstool (test/trening) - identyczna decyzja na PC i ESP32.
 *
 * Plik .kws (little-endian):
 *   "KWS1", u32 wersja, u32 dlugosc calego pliku,
 *   char slowo[32], f32 prog, u32 wygladzanie, u32 krok, u32 martwy_ms,
 *   u32 sr, win, hop, nfft, nmel, frames  (kontrola zgodnosci cech),
 *   u32 zarezerwowane[8],
 *   siec (nn_serialize), u32 crc32 (wszystko przed nim)
 */
#ifndef KWS_DETECT_H
#define KWS_DETECT_H

#include <stdint.h>
#include <stddef.h>
#include "nn.h"
#include "kws_feat.h"

typedef struct {
    char  keyword[32];
    float threshold;              /* 0..1 - prog wygladzonego prawdopodobienstwa */
    int   smooth;                 /* ile ostatnich wynikow usredniac */
    int   stride;                 /* co ile ramek (20 ms) liczyc siec */
    int   refractory_ms;          /* czas martwy po wykryciu */
} kws_meta;

uint32_t kws_crc32(const void *buf, size_t len);

/* 0 = OK, <0 = blad (zly plik / niezgodne cechy / CRC).
 * copy = 0: wagi czytane wprost z buf (np. zmapowana partycja flash) */
int    kws_model_parse(const void *buf, size_t len, nn_model *m, kws_meta *meta, int copy);
/* zapis; buf == NULL -> potrzebny rozmiar */
size_t kws_model_write(void *buf, size_t cap, const nn_model *m, const kws_meta *meta);

typedef struct {
    const nn_model *m;
    nn_ws    ws;
    kws_meta meta;
    float    thr;                         /* biezacy prog (domyslnie meta.threshold) */
    int16_t  tail[KWS_WIN];               /* ostatnie KWS_WIN probek */
    float    feat[KWS_FRAMES * KWS_NMEL]; /* okno cech, najstarsza ramka pierwsza */
    int      nframes, since, refr;
    float    hist[16];
    int      nhist;
    float    prob;                        /* ostatni wynik sieci */
    float    score;                       /* ostatni wynik wygladzony */
    float    energy_db;                   /* energia ostatniej ramki */
} kws_det;

int  kws_det_init(kws_det *d, const nn_model *m, const kws_meta *meta);
void kws_det_free(kws_det *d);
void kws_det_reset(kws_det *d);
/* Dokladnie KWS_HOP nowych probek (20 ms). Zwraca 1, gdy wlasnie wykryto slowo kluczowe;
 * *ran = 1, gdy w tym kroku liczono siec (d->score aktualny). */
int  kws_det_hop(kws_det *d, const int16_t *hop, int *ran);

#endif /* KWS_DETECT_H */
