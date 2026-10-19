/*
 * nn.c - budowa modelu, wnioskowanie i serializacja (wspolne dla PC i ESP32).
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "nn.h"

void nn_init(nn_model *m, int ih, int iw, int ic)
{
    memset(m, 0, sizeof *m);
    m->ih = ih; m->iw = iw; m->ic = ic;
    m->max_act = ih * iw * ic;
    m->n_out = m->max_act;
}

/* wymiary wejscia nastepnej warstwy */
static nn_layer *add(nn_model *m, int type)
{
    if (m->nl >= NN_MAX_LAYERS) { m->err = 1; return NULL; }
    nn_layer *L = &m->L[m->nl++];
    memset(L, 0, sizeof *L);
    L->type = type;
    if (m->nl == 1) { L->ih = m->ih; L->iw = m->iw; L->ic = m->ic; }
    else { nn_layer *P = &m->L[m->nl - 2]; L->ih = P->oh; L->iw = P->ow; L->ic = P->oc; }
    return L;
}

static void done(nn_model *m, nn_layer *L)
{
    L->woff = m->nparams;
    L->boff = m->nparams + L->nw;
    m->nparams += L->nw + L->nb;
    int act = L->oh * L->ow * L->oc;
    if (act > m->max_act) m->max_act = act;
    m->n_out = act;
}

static void conv_geom(nn_layer *L, int kh, int kw, int sh, int sw)
{
    L->kh = kh; L->kw = kw; L->sh = sh; L->sw = sw;
    L->ph = (kh - 1) / 2; L->pw = (kw - 1) / 2;
    L->oh = (L->ih + 2 * L->ph - kh) / sh + 1;
    L->ow = (L->iw + 2 * L->pw - kw) / sw + 1;
}

int nn_conv(nn_model *m, int oc, int kh, int kw, int sh, int sw, int relu)
{
    nn_layer *L = add(m, NN_CONV);
    if (!L) return -1;
    conv_geom(L, kh, kw, sh, sw);
    L->oc = oc; L->relu = relu;
    L->nw = oc * kh * kw * L->ic; L->nb = oc;
    done(m, L);
    return 0;
}

int nn_dwconv(nn_model *m, int kh, int kw, int sh, int sw, int relu)
{
    nn_layer *L = add(m, NN_DWCONV);
    if (!L) return -1;
    conv_geom(L, kh, kw, sh, sw);
    L->oc = L->ic; L->relu = relu;
    L->nw = L->ic * kh * kw; L->nb = L->ic;
    done(m, L);
    return 0;
}

int nn_gap(nn_model *m)
{
    nn_layer *L = add(m, NN_GAP);
    if (!L) return -1;
    L->oh = 1; L->ow = 1; L->oc = L->ic;
    done(m, L);
    return 0;
}

int nn_dense(nn_model *m, int n, int relu)
{
    nn_layer *L = add(m, NN_DENSE);
    if (!L) return -1;
    int in = L->ih * L->iw * L->ic;
    L->oh = 1; L->ow = 1; L->oc = n; L->relu = relu;
    L->nw = n * in; L->nb = n;
    done(m, L);
    return 0;
}

int nn_alloc(nn_model *m)
{
    if (m->err || m->nl == 0) return -1;
    nn_free(m);
    m->p = (float *)calloc((size_t)m->nparams, sizeof(float));
    m->own = m->p != NULL;
    return m->p ? 0 : -1;
}

void nn_free(nn_model *m) { if (m->own) free(m->p); m->p = NULL; m->own = 0; }

int nn_in_size(const nn_model *m) { return m->ih * m->iw * m->ic; }

long nn_macs(const nn_model *m)
{
    long s = 0;
    for (int i = 0; i < m->nl; i++) {
        const nn_layer *L = &m->L[i];
        long o = (long)L->oh * L->ow * L->oc;
        if (L->type == NN_CONV) s += o * L->kh * L->kw * L->ic;
        else if (L->type == NN_DWCONV) s += o * L->kh * L->kw;
        else if (L->type == NN_DENSE) s += (long)L->nw;
        else s += (long)L->ih * L->iw * L->ic;
    }
    return s;
}

#ifdef ESP_PLATFORM
/* ESP32: bufory robocze najlepiej w szybkim wewnetrznym RAM, w razie braku - PSRAM */
#include "esp_heap_caps.h"
#define FAST_MALLOC(n) heap_caps_malloc_prefer((n), 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT)
#else
#define FAST_MALLOC(n) malloc(n)
#endif

int nn_ws_alloc(nn_ws *ws, const nn_model *m)
{
    /* rozmiary buforow ping-pong osobno: warstwy parzyste pisza do a, nieparzyste do b */
    int na = 1, nb = 1;
    for (int i = 0; i < m->nl; i++) {
        int s = m->L[i].oh * m->L[i].ow * m->L[i].oc;
        if (i & 1) { if (s > nb) nb = s; } else if (s > na) na = s;
    }
    ws->a = (float *)FAST_MALLOC(sizeof(float) * na);
    ws->b = (float *)FAST_MALLOC(sizeof(float) * nb);
    return (ws->a && ws->b) ? 0 : -1;
}

void nn_ws_free(nn_ws *ws) { free(ws->a); free(ws->b); ws->a = ws->b = NULL; }

/* ---------------------------------------------------------------- warstwy */
void nn_layer_forward(const nn_layer *L, const float *p, const float *in, float *out)
{
    const float *w = p + L->woff, *b = p + L->boff;
    int relu = L->relu;
    if (L->type == NN_CONV) {
        int ic = L->ic, iw = L->iw, ih = L->ih;
        for (int oy = 0; oy < L->oh; oy++)
            for (int ox = 0; ox < L->ow; ox++) {
                float *o = out + (oy * L->ow + ox) * L->oc;
                for (int co = 0; co < L->oc; co++) {
                    float s = b[co];
                    for (int ky = 0; ky < L->kh; ky++) {
                        int iy = oy * L->sh + ky - L->ph;
                        if (iy < 0 || iy >= ih) continue;
                        for (int kx = 0; kx < L->kw; kx++) {
                            int ix = ox * L->sw + kx - L->pw;
                            if (ix < 0 || ix >= iw) continue;
                            const float *ip = in + (iy * iw + ix) * ic;
                            const float *wp = w + ((co * L->kh + ky) * L->kw + kx) * ic;
                            for (int ci = 0; ci < ic; ci++) s += ip[ci] * wp[ci];
                        }
                    }
                    o[co] = (relu && s < 0) ? 0 : s;
                }
            }
    } else if (L->type == NN_DWCONV) {
        int c = L->ic, iw = L->iw, ih = L->ih;
        for (int oy = 0; oy < L->oh; oy++)
            for (int ox = 0; ox < L->ow; ox++) {
                float *o = out + (oy * L->ow + ox) * c;
                for (int ch = 0; ch < c; ch++) o[ch] = b[ch];
                for (int ky = 0; ky < L->kh; ky++) {
                    int iy = oy * L->sh + ky - L->ph;
                    if (iy < 0 || iy >= ih) continue;
                    for (int kx = 0; kx < L->kw; kx++) {
                        int ix = ox * L->sw + kx - L->pw;
                        if (ix < 0 || ix >= iw) continue;
                        const float *ip = in + (iy * iw + ix) * c;
                        const float *wp = w + ky * L->kw + kx;
                        int kk = L->kh * L->kw;
                        for (int ch = 0; ch < c; ch++) o[ch] += ip[ch] * wp[ch * kk];
                    }
                }
                if (relu) for (int ch = 0; ch < c; ch++) if (o[ch] < 0) o[ch] = 0;
            }
    } else if (L->type == NN_GAP) {
        int c = L->ic, n = L->ih * L->iw;
        for (int ch = 0; ch < c; ch++) out[ch] = 0;
        for (int i = 0; i < n; i++) for (int ch = 0; ch < c; ch++) out[ch] += in[i * c + ch];
        for (int ch = 0; ch < c; ch++) out[ch] /= (float)n;
    } else if (L->type == NN_DENSE) {
        int n = L->ih * L->iw * L->ic;
        for (int o = 0; o < L->oc; o++) {
            const float *wr = w + o * n;
            float s = b[o];
            for (int i = 0; i < n; i++) s += wr[i] * in[i];
            out[o] = (relu && s < 0) ? 0 : s;
        }
    }
}

const float *nn_forward(const nn_model *m, nn_ws *ws, const float *in)
{
    const float *cur = in;
    float *bufs[2] = { ws->a, ws->b };
    for (int i = 0; i < m->nl; i++) {
        float *out = bufs[i & 1];
        nn_layer_forward(&m->L[i], m->p, cur, out);
        cur = out;
    }
    return cur;
}

void nn_softmax(float *x, int n)
{
    float mx = x[0], s = 0;
    for (int i = 1; i < n; i++) if (x[i] > mx) mx = x[i];
    for (int i = 0; i < n; i++) { x[i] = (float)exp(x[i] - mx); s += x[i]; }
    for (int i = 0; i < n; i++) x[i] /= s;
}

/* ---------------------------------------------------------------- serializacja
 * "NN01" u32 nl, i32 ih iw ic, nl x (i32 type relu oc kh kw sh sw 0), u32 nparams, float p[] */
#define NN_MAGIC 0x31304E4Eu   /* "NN01" */

static void put32(uint8_t **q, uint32_t v) { memcpy(*q, &v, 4); *q += 4; }
static uint32_t get32(const uint8_t **q) { uint32_t v; memcpy(&v, *q, 4); *q += 4; return v; }

size_t nn_serialize(const nn_model *m, void *buf, size_t cap)
{
    size_t need = 4 + 4 + 12 + (size_t)m->nl * 32 + 4 + (size_t)m->nparams * 4;
    if (!buf) return need;
    if (cap < need) return 0;
    uint8_t *q = (uint8_t *)buf;
    put32(&q, NN_MAGIC);
    put32(&q, (uint32_t)m->nl);
    put32(&q, (uint32_t)m->ih); put32(&q, (uint32_t)m->iw); put32(&q, (uint32_t)m->ic);
    for (int i = 0; i < m->nl; i++) {
        const nn_layer *L = &m->L[i];
        put32(&q, (uint32_t)L->type); put32(&q, (uint32_t)L->relu); put32(&q, (uint32_t)L->oc);
        put32(&q, (uint32_t)L->kh); put32(&q, (uint32_t)L->kw);
        put32(&q, (uint32_t)L->sh); put32(&q, (uint32_t)L->sw); put32(&q, 0);
    }
    put32(&q, (uint32_t)m->nparams);
    memcpy(q, m->p, (size_t)m->nparams * 4);
    return need;
}

int nn_deserialize(nn_model *m, const void *buf, size_t len, size_t *used, int copy)
{
    const uint8_t *q = (const uint8_t *)buf, *end = q + len;
    if (len < 20 || get32(&q) != NN_MAGIC) return -1;
    int nl = (int)get32(&q);
    int ih = (int)get32(&q), iw = (int)get32(&q), ic = (int)get32(&q);
    if (nl <= 0 || nl > NN_MAX_LAYERS || ih <= 0 || iw <= 0 || ic <= 0 || ih * iw * ic > 1 << 20) return -2;
    if ((size_t)(end - q) < (size_t)nl * 32 + 4) return -3;
    nn_init(m, ih, iw, ic);
    for (int i = 0; i < nl; i++) {
        int t = (int)get32(&q), relu = (int)get32(&q), oc = (int)get32(&q);
        int kh = (int)get32(&q), kw = (int)get32(&q), sh = (int)get32(&q), sw = (int)get32(&q);
        get32(&q);
        if ((t == NN_CONV || t == NN_DWCONV) &&
            (kh < 1 || kw < 1 || sh < 1 || sw < 1 || kh > 15 || kw > 15)) return -4;
        if (oc < 0 || oc > 4096) return -4;
        int r = -1;
        if (t == NN_CONV) r = nn_conv(m, oc, kh, kw, sh, sw, relu);
        else if (t == NN_DWCONV) r = nn_dwconv(m, kh, kw, sh, sw, relu);
        else if (t == NN_GAP) r = nn_gap(m);
        else if (t == NN_DENSE) r = nn_dense(m, oc, relu);
        if (r) return -5;
    }
    int np = (int)get32(&q);
    if (np != m->nparams || (size_t)(end - q) < (size_t)np * 4) return -6;
    if (copy) {
        if (nn_alloc(m)) return -7;
        memcpy(m->p, q, (size_t)np * 4);
    } else {
        if ((size_t)q & 3) return -8;                    /* wagi musza byc wyrownane do 4 */
        m->p = (float *)(void *)q;
        m->own = 0;
    }
    q += (size_t)np * 4;
    if (used) *used = (size_t)(q - (const uint8_t *)buf);
    return 0;
}
