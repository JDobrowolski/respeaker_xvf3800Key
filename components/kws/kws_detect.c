/*
 * kws_detect.c - plik .kws i detektor: okno 49 ramek log-mel przesuwane co 20 ms,
 * siec co `stride` ramek, srednia z `smooth` ostatnich wynikow >= prog -> wykrycie,
 * potem czas martwy `refractory_ms`.
 */
#include <string.h>
#include <stdlib.h>
#include "kws_detect.h"

#define KWS_MAGIC   0x3153574Bu   /* "KWS1" */
#define KWS_VERSION 1
#define HDR_SIZE    (4 * 3 + 32 + 4 * 4 + 4 * 6 + 4 * 8)

uint32_t kws_crc32(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static void put32(uint8_t **q, uint32_t v) { memcpy(*q, &v, 4); *q += 4; }
static uint32_t get32(const uint8_t **q) { uint32_t v; memcpy(&v, *q, 4); *q += 4; return v; }

size_t kws_model_write(void *buf, size_t cap, const nn_model *m, const kws_meta *meta)
{
    size_t nnsz = nn_serialize(m, NULL, 0);
    size_t need = HDR_SIZE + nnsz + 4;
    if (!buf) return need;
    if (cap < need) return 0;
    uint8_t *q = (uint8_t *)buf;
    put32(&q, KWS_MAGIC); put32(&q, KWS_VERSION); put32(&q, (uint32_t)need);
    memset(q, 0, 32);
    strncpy((char *)q, meta->keyword, 31);
    q += 32;
    memcpy(q, &meta->threshold, 4); q += 4;
    put32(&q, (uint32_t)meta->smooth); put32(&q, (uint32_t)meta->stride); put32(&q, (uint32_t)meta->refractory_ms);
    put32(&q, KWS_SR); put32(&q, KWS_WIN); put32(&q, KWS_HOP);
    put32(&q, KWS_NFFT); put32(&q, KWS_NMEL); put32(&q, KWS_FRAMES);
    memset(q, 0, 32); q += 32;
    nn_serialize(m, q, nnsz);
    q += nnsz;
    put32(&q, kws_crc32(buf, need - 4));
    return need;
}

int kws_model_parse(const void *buf, size_t len, nn_model *m, kws_meta *meta, int copy)
{
    const uint8_t *q = (const uint8_t *)buf;
    if (len < HDR_SIZE + 24 || get32(&q) != KWS_MAGIC) return -1;
    if (get32(&q) != KWS_VERSION) return -2;
    uint32_t total = get32(&q);
    if (total > len || total < HDR_SIZE + 24) return -3;
    uint32_t crc;
    memcpy(&crc, (const uint8_t *)buf + total - 4, 4);
    if (crc != kws_crc32(buf, total - 4)) return -4;
    memcpy(meta->keyword, q, 32); meta->keyword[31] = 0; q += 32;
    memcpy(&meta->threshold, q, 4); q += 4;
    meta->smooth = (int)get32(&q); meta->stride = (int)get32(&q); meta->refractory_ms = (int)get32(&q);
    uint32_t f[6];
    for (int i = 0; i < 6; i++) f[i] = get32(&q);
    if (f[0] != KWS_SR || f[1] != KWS_WIN || f[2] != KWS_HOP || f[3] != KWS_NFFT ||
        f[4] != KWS_NMEL || f[5] != KWS_FRAMES) return -5;                 /* inne cechy */
    q += 32;
    if (meta->smooth < 1) meta->smooth = 1;
    if (meta->smooth > 16) meta->smooth = 16;
    if (meta->stride < 1) meta->stride = 1;
    if (meta->threshold <= 0 || meta->threshold >= 1) meta->threshold = 0.8f;
    size_t used = 0;
    if (nn_deserialize(m, q, total - 4 - (size_t)(q - (const uint8_t *)buf), &used, copy)) return -6;
    if (m->ih != KWS_FRAMES || m->iw != KWS_NMEL || m->ic != 1 || m->n_out != 2) { nn_free(m); return -7; }
    return 0;
}

/* ---------------------------------------------------------------- detektor */
int kws_det_init(kws_det *d, const nn_model *m, const kws_meta *meta)
{
    memset(d, 0, sizeof *d);
    kws_feat_init();
    d->m = m;
    d->meta = *meta;
    d->thr = meta->threshold;
    if (nn_ws_alloc(&d->ws, m)) { nn_ws_free(&d->ws); return -1; }
    return 0;
}

void kws_det_free(kws_det *d) { nn_ws_free(&d->ws); }

void kws_det_reset(kws_det *d)
{
    memset(d->tail, 0, sizeof d->tail);
    d->nframes = 0; d->since = 0; d->refr = 0; d->nhist = 0;
    d->prob = d->score = 0;
}

int kws_det_hop(kws_det *d, const int16_t *hop, int *ran)
{
    if (ran) *ran = 0;
    memmove(d->tail, d->tail + KWS_HOP, sizeof(int16_t) * (KWS_WIN - KWS_HOP));
    memcpy(d->tail + (KWS_WIN - KWS_HOP), hop, sizeof(int16_t) * KWS_HOP);

    memmove(d->feat, d->feat + KWS_NMEL, sizeof(float) * (KWS_FRAMES - 1) * KWS_NMEL);
    d->energy_db = kws_feat_frame(d->tail, d->feat + (KWS_FRAMES - 1) * KWS_NMEL);
    if (d->nframes < KWS_FRAMES) d->nframes++;

    int refr_frames = d->meta.refractory_ms / 20;
    if (d->refr > 0) d->refr--;
    if (d->nframes < KWS_FRAMES || ++d->since < d->meta.stride) return 0;
    d->since = 0;

    const float *lg = nn_forward(d->m, &d->ws, d->feat);
    float l[2] = { lg[0], lg[1] };
    nn_softmax(l, 2);
    d->prob = l[1];
    if (ran) *ran = 1;

    if (d->nhist == d->meta.smooth) { memmove(d->hist, d->hist + 1, sizeof(float) * (d->nhist - 1)); d->nhist--; }
    d->hist[d->nhist++] = d->prob;
    float s = 0;
    for (int i = 0; i < d->nhist; i++) s += d->hist[i];
    d->score = s / d->meta.smooth;              /* do zapelnienia historii - zanizone (ostroznie) */

    if (d->refr > 0 || d->score < d->thr) return 0;
    d->refr = refr_frames;
    d->nhist = 0;
    return 1;
}
