/*
 * nn_train.c - propagacja wsteczna dla warstw nn.h + AdamW + pula watkow Win32.
 * Kazdy watek liczy gradienty swojej czesci paczki do wlasnego bufora; potem suma.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "nn_train.h"

#define MAXTHR 64

typedef struct {
    float *acts;                  /* wyjscia wszystkich warstw (jedna probka) */
    float *dA, *dB;               /* gradienty aktywacji (ping-pong) */
    float *grad;                  /* suma gradientow parametrow */
    float  probs[64];
    double loss;
    int    correct;
} tws_t;

struct nn_trainer {
    nn_model *m;
    int       nthr;
    int      *aoff;               /* przesuniecie wyjscia warstwy l w acts */
    int       asize;
    tws_t     tw[MAXTHR];
    float    *am, *av;            /* momenty Adama */
    unsigned char *is_w;          /* 1 = waga (decay), 0 = bias */
    long      step;
    /* pula */
    HANDLE    th[MAXTHR], go[MAXTHR], done[MAXTHR];
    volatile int quit;
    nn_job    fn;
    void     *ctx;
    int       n;
    /* biezacy krok */
    const float *X;
    const int   *Y;
    float       *P;
};

int nn_threads(const nn_trainer *t) { return t->nthr; }

/* ---------------------------------------------------------------- pula watkow */
static void rng_of(const nn_trainer *t, int i, int *a, int *b)
{
    int per = t->n / t->nthr, rem = t->n % t->nthr;
    *a = i * per + (i < rem ? i : rem);
    *b = *a + per + (i < rem ? 1 : 0);
}

typedef struct { nn_trainer *t; int i; } warg_t;
static warg_t s_args[MAXTHR];

static DWORD WINAPI worker(LPVOID p)
{
    warg_t *w = (warg_t *)p;
    nn_trainer *t = w->t;
    for (;;) {
        WaitForSingleObject(t->go[w->i], INFINITE);
        if (t->quit) break;
        int a, b;
        rng_of(t, w->i, &a, &b);
        if (a < b) t->fn(t->ctx, a, b, w->i);
        SetEvent(t->done[w->i]);
    }
    return 0;
}

void nn_parallel(nn_trainer *t, nn_job fn, void *ctx, int n)
{
    if (t->nthr <= 1 || n < 2) { fn(ctx, 0, n, 0); return; }
    t->fn = fn; t->ctx = ctx; t->n = n;
    for (int i = 1; i < t->nthr; i++) SetEvent(t->go[i]);
    int a, b;
    rng_of(t, 0, &a, &b);
    if (a < b) fn(ctx, a, b, 0);
    WaitForMultipleObjects(t->nthr - 1, t->done + 1, TRUE, INFINITE);
}

nn_trainer *nn_trainer_new(nn_model *m, int threads)
{
    if (threads <= 0) { SYSTEM_INFO si; GetSystemInfo(&si); threads = (int)si.dwNumberOfProcessors; }
    if (threads > MAXTHR) threads = MAXTHR;
    nn_trainer *t = (nn_trainer *)calloc(1, sizeof *t);
    t->m = m;
    t->nthr = threads;
    t->aoff = (int *)calloc(m->nl, sizeof(int));
    for (int l = 0; l < m->nl; l++) {
        t->aoff[l] = t->asize;
        t->asize += m->L[l].oh * m->L[l].ow * m->L[l].oc;
    }
    for (int i = 0; i < threads; i++) {
        tws_t *w = &t->tw[i];
        w->acts = (float *)malloc(sizeof(float) * t->asize);
        w->dA = (float *)malloc(sizeof(float) * m->max_act);
        w->dB = (float *)malloc(sizeof(float) * m->max_act);
        w->grad = (float *)calloc(m->nparams, sizeof(float));
    }
    t->am = (float *)calloc(m->nparams, sizeof(float));
    t->av = (float *)calloc(m->nparams, sizeof(float));
    t->is_w = (unsigned char *)calloc(m->nparams, 1);
    for (int l = 0; l < m->nl; l++) memset(t->is_w + m->L[l].woff, 1, m->L[l].nw);
    for (int i = 1; i < threads; i++) {
        t->go[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
        t->done[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
        s_args[i].t = t; s_args[i].i = i;
        t->th[i] = CreateThread(NULL, 0, worker, &s_args[i], 0, NULL);
    }
    return t;
}

void nn_trainer_free(nn_trainer *t)
{
    if (!t) return;
    t->quit = 1;
    for (int i = 1; i < t->nthr; i++) SetEvent(t->go[i]);
    if (t->nthr > 1) WaitForMultipleObjects(t->nthr - 1, t->th + 1, TRUE, 2000);
    for (int i = 0; i < t->nthr; i++) {
        free(t->tw[i].acts); free(t->tw[i].dA); free(t->tw[i].dB); free(t->tw[i].grad);
    }
    free(t->aoff); free(t->am); free(t->av); free(t->is_w);
    free(t);
}

/* ---------------------------------------------------------------- inicjalizacja He */
static unsigned s_seed;
static float frand(void) { s_seed = s_seed * 1664525u + 1013904223u; return ((s_seed >> 8) + 0.5f) / 16777216.0f; }
static float grand(void) { return (float)(sqrt(-2.0 * log(frand())) * cos(6.283185307 * frand())); }

void nn_he_init(nn_model *m, unsigned seed)
{
    s_seed = seed ? seed : 1;
    for (int l = 0; l < m->nl; l++) {
        nn_layer *L = &m->L[l];
        int fan = 1;
        if (L->type == NN_CONV) fan = L->kh * L->kw * L->ic;
        else if (L->type == NN_DWCONV) fan = L->kh * L->kw;
        else if (L->type == NN_DENSE) fan = L->ih * L->iw * L->ic;
        float sd = (float)sqrt((L->relu ? 2.0 : 1.0) / fan);
        for (int i = 0; i < L->nw; i++) m->p[L->woff + i] = grand() * sd;
        for (int i = 0; i < L->nb; i++) m->p[L->boff + i] = 0;
    }
}

/* ---------------------------------------------------------------- wstecz */
static void layer_backward(const nn_layer *L, const float *p, const float *in, const float *dout,
                           float *din, float *g)
{
    const float *w = p + L->woff;
    float *gw = g + L->woff, *gb = g + L->boff;
    int insz = L->ih * L->iw * L->ic;
    if (din) memset(din, 0, sizeof(float) * insz);

    if (L->type == NN_CONV) {
        int ic = L->ic, iw = L->iw, ih = L->ih;
        for (int oy = 0; oy < L->oh; oy++)
            for (int ox = 0; ox < L->ow; ox++) {
                const float *go = dout + (oy * L->ow + ox) * L->oc;
                for (int co = 0; co < L->oc; co++) {
                    float gv = go[co];
                    if (gv == 0) continue;
                    gb[co] += gv;
                    for (int ky = 0; ky < L->kh; ky++) {
                        int iy = oy * L->sh + ky - L->ph;
                        if (iy < 0 || iy >= ih) continue;
                        for (int kx = 0; kx < L->kw; kx++) {
                            int ix = ox * L->sw + kx - L->pw;
                            if (ix < 0 || ix >= iw) continue;
                            int io = (iy * iw + ix) * ic, wo = ((co * L->kh + ky) * L->kw + kx) * ic;
                            const float *ip = in + io;
                            float *gwp = gw + wo;
                            for (int ci = 0; ci < ic; ci++) gwp[ci] += gv * ip[ci];
                            if (din) {
                                const float *wp = w + wo;
                                float *dp = din + io;
                                for (int ci = 0; ci < ic; ci++) dp[ci] += gv * wp[ci];
                            }
                        }
                    }
                }
            }
    } else if (L->type == NN_DWCONV) {
        int c = L->ic, iw = L->iw, ih = L->ih, kk = L->kh * L->kw;
        for (int oy = 0; oy < L->oh; oy++)
            for (int ox = 0; ox < L->ow; ox++) {
                const float *go = dout + (oy * L->ow + ox) * c;
                for (int ch = 0; ch < c; ch++) gb[ch] += go[ch];
                for (int ky = 0; ky < L->kh; ky++) {
                    int iy = oy * L->sh + ky - L->ph;
                    if (iy < 0 || iy >= ih) continue;
                    for (int kx = 0; kx < L->kw; kx++) {
                        int ix = ox * L->sw + kx - L->pw;
                        if (ix < 0 || ix >= iw) continue;
                        int io = (iy * iw + ix) * c, k = ky * L->kw + kx;
                        for (int ch = 0; ch < c; ch++) {
                            gw[ch * kk + k] += go[ch] * in[io + ch];
                            if (din) din[io + ch] += go[ch] * w[ch * kk + k];
                        }
                    }
                }
            }
    } else if (L->type == NN_GAP) {
        int c = L->ic, n = L->ih * L->iw;
        if (din) for (int i = 0; i < n; i++) for (int ch = 0; ch < c; ch++) din[i * c + ch] = dout[ch] / n;
    } else if (L->type == NN_DENSE) {
        int n = insz;
        for (int o = 0; o < L->oc; o++) {
            float gv = dout[o];
            if (gv == 0) continue;
            gb[o] += gv;
            const float *wr = w + o * n;
            float *gwr = gw + o * n;
            for (int i = 0; i < n; i++) gwr[i] += gv * in[i];
            if (din) for (int i = 0; i < n; i++) din[i] += gv * wr[i];
        }
    }
}

/* jedna probka: w przod z zapamietaniem aktywacji, strata, wstecz (gdy y >= 0) */
static void sample(nn_trainer *t, tws_t *w, const float *x, int y, float *probs_out)
{
    nn_model *m = t->m;
    const float *cur = x;
    for (int l = 0; l < m->nl; l++) {
        float *out = w->acts + t->aoff[l];
        nn_layer_forward(&m->L[l], m->p, cur, out);
        cur = out;
    }
    int no = m->n_out;
    for (int i = 0; i < no; i++) w->probs[i] = cur[i];
    nn_softmax(w->probs, no);
    if (probs_out) memcpy(probs_out, w->probs, sizeof(float) * no);
    if (y < 0) return;

    int am = 0;
    for (int i = 1; i < no; i++) if (w->probs[i] > w->probs[am]) am = i;
    w->correct += am == y;
    w->loss += -log(w->probs[y] + 1e-9);

    float *d = w->dA, *dn = w->dB;
    for (int i = 0; i < no; i++) d[i] = w->probs[i] - (i == y ? 1.0f : 0.0f);
    for (int l = m->nl - 1; l >= 0; l--) {
        const nn_layer *L = &m->L[l];
        const float *out = w->acts + t->aoff[l];
        const float *in = l ? w->acts + t->aoff[l - 1] : x;
        if (L->relu) {
            int os = L->oh * L->ow * L->oc;
            for (int i = 0; i < os; i++) if (out[i] <= 0) d[i] = 0;
        }
        layer_backward(L, m->p, in, d, l ? dn : NULL, w->grad);
        float *tmp = d; d = dn; dn = tmp;
    }
}

static void job_train(void *c, int i0, int i1, int th)
{
    nn_trainer *t = (nn_trainer *)c;
    tws_t *w = &t->tw[th];
    int insz = nn_in_size(t->m);
    for (int i = i0; i < i1; i++) sample(t, w, t->X + (size_t)i * insz, t->Y[i], NULL);
}

static void job_pred(void *c, int i0, int i1, int th)
{
    nn_trainer *t = (nn_trainer *)c;
    tws_t *w = &t->tw[th];
    int insz = nn_in_size(t->m);
    for (int i = i0; i < i1; i++) sample(t, w, t->X + (size_t)i * insz, -1, t->P + (size_t)i * t->m->n_out);
}

float nn_train_step(nn_trainer *t, const float *X, const int *y, int n, float lr, float wd, int *correct)
{
    nn_model *m = t->m;
    for (int i = 0; i < t->nthr; i++) {
        memset(t->tw[i].grad, 0, sizeof(float) * m->nparams);
        t->tw[i].loss = 0; t->tw[i].correct = 0;
    }
    t->X = X; t->Y = y;
    nn_parallel(t, job_train, t, n);

    double loss = 0;
    int ok = 0;
    float *g = t->tw[0].grad;
    for (int i = 0; i < t->nthr; i++) {
        loss += t->tw[i].loss; ok += t->tw[i].correct;
        if (i) for (int k = 0; k < m->nparams; k++) g[k] += t->tw[i].grad[k];
    }
    /* srednia + przyciecie normy gradientu do 5 */
    double norm = 0;
    for (int k = 0; k < m->nparams; k++) { g[k] /= n; norm += (double)g[k] * g[k]; }
    norm = sqrt(norm);
    float clip = norm > 5.0 ? (float)(5.0 / norm) : 1.0f;

    const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
    t->step++;
    float c1 = 1.0f - (float)pow(b1, (double)t->step), c2 = 1.0f - (float)pow(b2, (double)t->step);
    for (int k = 0; k < m->nparams; k++) {
        float gk = g[k] * clip;
        t->am[k] = b1 * t->am[k] + (1 - b1) * gk;
        t->av[k] = b2 * t->av[k] + (1 - b2) * gk * gk;
        float upd = (t->am[k] / c1) / ((float)sqrt(t->av[k] / c2) + eps);
        if (t->is_w[k]) upd += wd * m->p[k];
        m->p[k] -= lr * upd;
    }
    if (correct) *correct = ok;
    return (float)(loss / n);
}

void nn_predict(nn_trainer *t, const float *X, int n, float *probs)
{
    t->X = X; t->P = probs;
    nn_parallel(t, job_pred, t, n);
}
