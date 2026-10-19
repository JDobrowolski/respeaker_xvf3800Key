/*
 * whisper.c - wnioskowanie modelu Whisper (encoder-decoder transformer) w czystym C.
 * Format pliku i stale tokenow jak w whisper.cpp (ggml legacy):
 *   magic 'ggml', 11 x int32 hparams, filtry mel, slownik, tensory (f32/f16/q8_0).
 * Macierze Q8_0 (np. ggml-large-v3-turbo-q8_0.bin) zostaja skwantyzowane, gdy backend
 * umie na nich liczyc (CPU); inaczej (GPU) sa rozpakowywane do float32.
 * Dekodowanie zachlanne (greedy) bez znacznikow czasu - wystarcza do komend/dyktowania.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "whisper.h"
#include "backend.h"

#define N_FFT   400
#define HOP     160
#define N_BINS  (N_FFT / 2 + 1)
#define PI_D    3.14159265358979323846

static const char *k_langs[] = {
    "en","zh","de","es","ru","ko","fr","ja","pt","tr","pl","ca","nl","ar","sv","it","id","hi",
    "fi","vi","he","uk","el","ms","cs","ro","da","hu","ta","no","th","ur","hr","bg","lt","la",
    "mi","ml","cy","sk","te","fa","lv","bn","sr","az","sl","kn","et","mk","br","eu","is","hy",
    "ne","mn","bs","kk","sq","sw","gl","mr","pa","si","km","sn","yo","so","af","oc","ka","be",
    "tg","sd","gu","am","yi","lo","uz","fo","ht","ps","tk","nn","mt","sa","lb","my","bo","tl",
    "mg","as","tt","haw","ln","ha","ba","jw","su","yue"
};
#define N_LANGS_ALL ((int)(sizeof k_langs / sizeof k_langs[0]))

typedef struct { dptr d; int q8; } mat_t;          /* macierz wag: float32 albo Q8_0 */

typedef struct {
    dptr attn_ln_g, attn_ln_b, q_b, v_b, o_b;
    dptr x_ln_g, x_ln_b, xq_b, xv_b, xo_b;                          /* tylko decoder */
    dptr mlp_ln_g, mlp_ln_b, m0_b, m2_b;
    mat_t q_w, k_w, v_w, o_w, xq_w, xk_w, xv_w, xo_w, m0_w, m2_w;
} layer_t;

typedef struct { char *name; dptr d; int n, q8; } tensor_t;

struct whisper {
    int n_vocab, n_audio_ctx, n_state, n_head, n_audio_layer, n_text_ctx, n_text_layer, n_mels;
    float *filters;                 /* n_mels x N_BINS */
    char **tok; int *toklen; int n_tok;

    tensor_t *t; int nt, capt;

    dptr conv1_w, conv1_b, conv2_w, conv2_b, enc_pos, enc_ln_g, enc_ln_b;
    dptr dec_pos, dec_ln_g, dec_ln_b;
    mat_t tok_emb;
    layer_t *enc, *dec;

    /* bufory robocze */
    dptr mel, c1, c2, x, h, q, k, v, att, mlp;
    dptr *kc, *vc, *xk, *xv;
    dptr dx, dh, dq, datt, dmlp, dlogits;
    float *logits;
    int   n_ctx;                    /* kontekst audio ostatniego encode */

    /* tokeny specjalne */
    int eot, sot, translate, transcribe, solm, prev, nosp, notok, beg, n_langs;
};

int wh_multilingual(const whisper *w) { return w->n_vocab >= 51865; }

int wh_lang_id(const char *code)
{
    if (!code || !strcmp(code, "auto")) return -1;
    for (int i = 0; i < N_LANGS_ALL; i++) if (!strcmp(code, k_langs[i])) return i;
    return -2;
}
const char *wh_lang_code(int id) { return (id >= 0 && id < N_LANGS_ALL) ? k_langs[id] : "?"; }

/* ---------------------------------------------------------------- wczytanie */
static float h2f(uint16_t h)
{
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 0x1f, m = h & 0x3ff, f;
    if (e == 0) {
        if (m == 0) f = s;
        else {                                         /* liczba zdenormalizowana */
            e = 1;
            while (!(m & 0x400)) { m <<= 1; e--; }
            m &= 0x3ff;
            f = s | ((e + 112) << 23) | (m << 13);
        }
    } else if (e == 31) f = s | 0x7f800000u | (m << 13);
    else f = s | ((e + 112) << 23) | (m << 13);
    union { uint32_t u; float f; } u;
    u.u = f;
    return u.f;
}

static int rd(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }

static tensor_t *find(whisper *w, const char *name, int n)
{
    for (int i = 0; i < w->nt; i++)
        if (!strcmp(w->t[i].name, name)) {
            if (n > 0 && w->t[i].n != n) {
                fprintf(stderr, "tensor %s: %d elementow, oczekiwano %d\n", name, w->t[i].n, n);
                return NULL;
            }
            return &w->t[i];
        }
    fprintf(stderr, "brak tensora %s\n", name);
    return NULL;
}

/* wektor / tensor float32 */
static dptr need(whisper *w, const char *name, int n)
{
    tensor_t *t = find(w, name, n);
    if (t && t->q8) { fprintf(stderr, "tensor %s: Q8_0 tam, gdzie potrzebny float32\n", name); return 0; }
    return t ? t->d : 0;
}

/* macierz wag (float32 albo Q8_0) */
static mat_t needm(whisper *w, const char *name, int n)
{
    mat_t m = { 0, 0 };
    tensor_t *t = find(w, name, n);
    if (t) { m.d = t->d; m.q8 = t->q8; }
    return m;
}

static void lin(dptr y, dptr x, mat_t W, dptr b, int M, int K, int N)
{
    if (W.q8) be_linear_q8(y, x, W.d, b, M, K, N);
    else      be_linear(y, x, W.d, b, M, K, N);
}

static int bind_layer(whisper *w, layer_t *L, const char *pre, int dec)
{
    char nm[96];
    int S = w->n_state;
#define T(field, suffix, n) do { sprintf(nm, "%s.%s", pre, suffix); L->field = need(w, nm, n); if (!L->field) return -1; } while (0)
#define M(field, suffix, n) do { sprintf(nm, "%s.%s", pre, suffix); L->field = needm(w, nm, n); if (!L->field.d) return -1; } while (0)
    T(attn_ln_g, "attn_ln.weight", S);       T(attn_ln_b, "attn_ln.bias", S);
    M(q_w, "attn.query.weight", S * S);      T(q_b, "attn.query.bias", S);
    M(k_w, "attn.key.weight", S * S);
    M(v_w, "attn.value.weight", S * S);      T(v_b, "attn.value.bias", S);
    M(o_w, "attn.out.weight", S * S);        T(o_b, "attn.out.bias", S);
    if (dec) {
        T(x_ln_g, "cross_attn_ln.weight", S); T(x_ln_b, "cross_attn_ln.bias", S);
        M(xq_w, "cross_attn.query.weight", S * S); T(xq_b, "cross_attn.query.bias", S);
        M(xk_w, "cross_attn.key.weight", S * S);
        M(xv_w, "cross_attn.value.weight", S * S); T(xv_b, "cross_attn.value.bias", S);
        M(xo_w, "cross_attn.out.weight", S * S); T(xo_b, "cross_attn.out.bias", S);
    }
    T(mlp_ln_g, "mlp_ln.weight", S);         T(mlp_ln_b, "mlp_ln.bias", S);
    M(m0_w, "mlp.0.weight", 4 * S * S);      T(m0_b, "mlp.0.bias", 4 * S);
    M(m2_w, "mlp.2.weight", 4 * S * S);      T(m2_b, "mlp.2.bias", S);
#undef M
#undef T
    return 0;
}

static dptr walloc(size_t n, int *ok)
{
    dptr d = be_alloc(n);
    if (!d) *ok = 0;
    return d;
}

whisper *wh_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "nie moge otworzyc modelu %s\n", path); return NULL; }
    whisper *w = (whisper *)calloc(1, sizeof *w);
    uint32_t magic;
    int32_t hp[11];
    if (!rd(f, &magic, 4) || magic != 0x67676d6c || !rd(f, hp, sizeof hp)) {
        fprintf(stderr, "%s: to nie jest model ggml whisper\n", path);
        fclose(f); free(w); return NULL;
    }
    w->n_vocab = hp[0]; w->n_audio_ctx = hp[1]; w->n_state = hp[2]; w->n_head = hp[3];
    w->n_audio_layer = hp[4]; w->n_text_ctx = hp[5]; w->n_text_layer = hp[8]; w->n_mels = hp[9];
    if (hp[6] != hp[2] || hp[7] != hp[3]) { fprintf(stderr, "nieobslugiwany model (rozne wymiary enc/dec)\n"); fclose(f); free(w); return NULL; }

    int32_t fm, fn;
    if (!rd(f, &fm, 4) || !rd(f, &fn, 4) || fm != w->n_mels || fn != N_BINS) {
        fprintf(stderr, "zle filtry mel w pliku\n"); fclose(f); free(w); return NULL;
    }
    w->filters = (float *)malloc(sizeof(float) * fm * fn);
    rd(f, w->filters, sizeof(float) * fm * fn);

    int32_t nv;
    rd(f, &nv, 4);
    w->n_tok = nv;
    w->tok = (char **)calloc(nv, sizeof(char *));
    w->toklen = (int *)calloc(nv, sizeof(int));
    for (int i = 0; i < nv; i++) {
        uint32_t len;
        if (!rd(f, &len, 4)) break;
        w->tok[i] = (char *)malloc(len + 1);
        rd(f, w->tok[i], len);
        w->tok[i][len] = 0;
        w->toklen[i] = (int)len;
    }

    /* tensory */
    size_t total = 0, nq8 = 0;
    uint16_t *tmp16 = NULL; size_t tmpcap = 0;          /* surowe dane: f16 albo bloki Q8_0 */
    float *tmpf = NULL; size_t tmpfcap = 0;
    for (;;) {
        int32_t nd, nl, tt;
        if (!rd(f, &nd, 4)) break;
        if (!rd(f, &nl, 4) || !rd(f, &tt, 4) || nd < 1 || nd > 4 || nl <= 0 || nl > 200) {
            fprintf(stderr, "uszkodzony naglowek tensora\n"); break;
        }
        int32_t ne[4] = { 1, 1, 1, 1 };
        int n = 1;
        for (int i = 0; i < nd; i++) { rd(f, &ne[i], 4); n *= ne[i]; }
        char *name = (char *)malloc(nl + 1);
        rd(f, name, nl); name[nl] = 0;
        if (tt != 0 && tt != 1 && !(tt == 8 && ne[0] % 32 == 0)) {
            fprintf(stderr, "tensor %s: typ %d nieobslugiwany (dzialaja f32, f16, q8_0)\n", name, tt);
            fclose(f); return NULL;
        }
        dptr d = 0;
        int q8 = 0;
        if (tt == 8) {                                   /* Q8_0: n/32 blokow po 34 bajty */
            size_t bytes = (size_t)n / 32 * 34;
            if (bytes > tmpcap * 2) { tmpcap = (bytes + 1) / 2; tmp16 = (uint16_t *)realloc(tmp16, tmpcap * 2); }
            rd(f, tmp16, bytes);
            d = be_upload_q8(tmp16, n / ne[0], ne[0]);
            if (d) { q8 = 1; nq8++; }
            else {                                       /* backend bez Q8_0 - rozpakuj */
                if ((size_t)n > tmpfcap) { tmpfcap = n; tmpf = (float *)realloc(tmpf, tmpfcap * 4); }
                const unsigned char *bl = (const unsigned char *)tmp16;
                for (int i = 0; i < n; i += 32, bl += 34) {
                    float dd = h2f((uint16_t)(bl[0] | bl[1] << 8));
                    for (int j = 0; j < 32; j++) tmpf[i + j] = dd * (signed char)bl[2 + j];
                }
            }
        } else {
            if ((size_t)n > tmpfcap) { tmpfcap = n; tmpf = (float *)realloc(tmpf, tmpfcap * 4); }
            if (tt == 1) {
                if ((size_t)n > tmpcap) { tmpcap = n; tmp16 = (uint16_t *)realloc(tmp16, tmpcap * 2); }
                rd(f, tmp16, (size_t)n * 2);
                for (int i = 0; i < n; i++) tmpf[i] = h2f(tmp16[i]);
            } else {
                rd(f, tmpf, (size_t)n * 4);
            }
        }
        if (!q8) {
            d = be_alloc(n);
            if (d) be_upload(d, tmpf, n);
        }
        if (!d) { fprintf(stderr, "brak pamieci na tensor %s\n", name); fclose(f); return NULL; }
        if (w->nt == w->capt) { w->capt = w->capt ? w->capt * 2 : 256; w->t = (tensor_t *)realloc(w->t, w->capt * sizeof(tensor_t)); }
        w->t[w->nt].name = name; w->t[w->nt].d = d; w->t[w->nt].n = n; w->t[w->nt].q8 = q8; w->nt++;
        total += (size_t)n;
    }
    fclose(f);
    free(tmp16); free(tmpf);

    int S = w->n_state;
    w->conv1_w = need(w, "encoder.conv1.weight", S * w->n_mels * 3);
    w->conv1_b = need(w, "encoder.conv1.bias", S);
    w->conv2_w = need(w, "encoder.conv2.weight", S * S * 3);
    w->conv2_b = need(w, "encoder.conv2.bias", S);
    w->enc_pos = need(w, "encoder.positional_embedding", w->n_audio_ctx * S);
    w->enc_ln_g = need(w, "encoder.ln_post.weight", S);
    w->enc_ln_b = need(w, "encoder.ln_post.bias", S);
    w->dec_pos = need(w, "decoder.positional_embedding", w->n_text_ctx * S);
    w->tok_emb = needm(w, "decoder.token_embedding.weight", w->n_vocab * S);
    w->dec_ln_g = need(w, "decoder.ln.weight", S);
    w->dec_ln_b = need(w, "decoder.ln.bias", S);
    if (!w->conv1_w || !w->conv1_b || !w->conv2_w || !w->conv2_b || !w->enc_pos || !w->enc_ln_g ||
        !w->enc_ln_b || !w->dec_pos || !w->tok_emb.d || !w->dec_ln_g || !w->dec_ln_b) return NULL;

    w->enc = (layer_t *)calloc(w->n_audio_layer, sizeof(layer_t));
    w->dec = (layer_t *)calloc(w->n_text_layer, sizeof(layer_t));
    char pre[48];
    for (int l = 0; l < w->n_audio_layer; l++) { sprintf(pre, "encoder.blocks.%d", l); if (bind_layer(w, &w->enc[l], pre, 0)) return NULL; }
    for (int l = 0; l < w->n_text_layer; l++)  { sprintf(pre, "decoder.blocks.%d", l); if (bind_layer(w, &w->dec[l], pre, 1)) return NULL; }

    /* tokeny specjalne (jak whisper.cpp) */
    w->eot = 50256; w->sot = 50257; w->translate = 50357; w->transcribe = 50358;
    w->solm = 50359; w->prev = 50360; w->nosp = 50361; w->notok = 50362; w->beg = 50363;
    w->n_langs = 0;
    if (wh_multilingual(w)) {
        w->n_langs = w->n_vocab - 51765 - 1;
        int dt = w->n_langs - 98;
        w->eot++; w->sot++;
        w->translate += dt; w->transcribe += dt; w->solm += dt; w->prev += dt;
        w->nosp += dt; w->notok += dt; w->beg += dt;
    }

    /* bufory robocze */
    int ok = 1, A = w->n_audio_ctx, NM = w->n_mels;
    w->mel = walloc((size_t)NM * 2 * A, &ok);
    w->c1  = walloc((size_t)S * 2 * A, &ok);
    w->c2  = walloc((size_t)S * A, &ok);
    w->x   = walloc((size_t)A * S, &ok);
    w->h   = walloc((size_t)A * S, &ok);
    w->q   = walloc((size_t)A * S, &ok);
    w->k   = walloc((size_t)A * S, &ok);
    w->v   = walloc((size_t)A * S, &ok);
    w->att = walloc((size_t)A * S, &ok);
    w->mlp = walloc((size_t)A * 4 * S, &ok);
    w->kc = (dptr *)calloc(w->n_text_layer, sizeof(dptr));
    w->vc = (dptr *)calloc(w->n_text_layer, sizeof(dptr));
    w->xk = (dptr *)calloc(w->n_text_layer, sizeof(dptr));
    w->xv = (dptr *)calloc(w->n_text_layer, sizeof(dptr));
    for (int l = 0; l < w->n_text_layer; l++) {
        w->kc[l] = walloc((size_t)w->n_text_ctx * S, &ok);
        w->vc[l] = walloc((size_t)w->n_text_ctx * S, &ok);
        w->xk[l] = walloc((size_t)A * S, &ok);
        w->xv[l] = walloc((size_t)A * S, &ok);
    }
    w->dx = walloc(S, &ok); w->dh = walloc(S, &ok); w->dq = walloc(S, &ok);
    w->datt = walloc(S, &ok); w->dmlp = walloc(4 * S, &ok);
    w->dlogits = walloc(w->n_vocab, &ok);
    w->logits = (float *)malloc(sizeof(float) * w->n_vocab);
    if (!ok) { fprintf(stderr, "brak pamieci na bufory robocze\n"); return NULL; }

    fprintf(stderr, "model: %s, %s, warstwy %d+%d, wymiar %d, glowy %d, parametry %.1f M%s\n",
            path, wh_multilingual(w) ? "wielojezyczny" : "tylko angielski",
            w->n_audio_layer, w->n_text_layer, S, w->n_head, total / 1e6,
            nq8 ? ", wagi Q8_0" : "");
    return w;
}

void wh_free(whisper *w)
{
    if (!w) return;
    for (int i = 0; i < w->nt; i++) { be_free(w->t[i].d); free(w->t[i].name); }
    free(w->t);
    for (int i = 0; i < w->n_tok; i++) free(w->tok[i]);
    free(w->tok); free(w->toklen); free(w->filters); free(w->enc); free(w->dec); free(w->logits);
    free(w->kc); free(w->vc); free(w->xk); free(w->xv);
    free(w);
}

/* ---------------------------------------------------------------- spektrogram */
static float s_cos[N_FFT], s_sin[N_FFT], s_hann[N_FFT];
static int   s_tables;

static void init_tables(void)
{
    if (s_tables) return;
    for (int i = 0; i < N_FFT; i++) {
        s_cos[i] = (float)cos(2.0 * PI_D * i / N_FFT);
        s_sin[i] = (float)sin(2.0 * PI_D * i / N_FFT);
        s_hann[i] = (float)(0.5 * (1.0 - cos(2.0 * PI_D * i / N_FFT)));   /* okno okresowe */
    }
    s_tables = 1;
}

/* FFT rzeczywistego sygnalu jak w whisper.cpp: podzial parzyste/nieparzyste, DFT dla
 * nieparzystych dlugosci (400 = 16 * 25). out = N liczb zespolonych (re,im). */
static void fft(const float *in, int N, float *out)
{
    if (N == 1) { out[0] = in[0]; out[1] = 0; return; }
    int step = N_FFT / N;                          /* N dzieli 400 */
    if (N & 1) {
        for (int k = 0; k < N; k++) {
            float re = 0, im = 0;
            for (int n = 0; n < N; n++) {
                int idx = (k * n * step) % N_FFT;
                re += in[n] * s_cos[idx];
                im -= in[n] * s_sin[idx];
            }
            out[2 * k] = re; out[2 * k + 1] = im;
        }
        return;
    }
    float even[N_FFT / 2], odd[N_FFT / 2], fe[N_FFT], fo[N_FFT];
    int h = N / 2;
    for (int i = 0; i < h; i++) { even[i] = in[2 * i]; odd[i] = in[2 * i + 1]; }
    fft(even, h, fe);
    fft(odd, h, fo);
    for (int k = 0; k < h; k++) {
        int idx = k * step;
        float re = s_cos[idx], im = -s_sin[idx];
        float ro = fo[2 * k], io = fo[2 * k + 1];
        float tr = re * ro - im * io, ti = re * io + im * ro;
        out[2 * k]           = fe[2 * k] + tr;
        out[2 * k + 1]       = fe[2 * k + 1] + ti;
        out[2 * (k + h)]     = fe[2 * k] - tr;
        out[2 * (k + h) + 1] = fe[2 * k + 1] - ti;
    }
}

/* log-mel jak whisper: odbicie 200 probek na poczatku, zera na koncu, |FFT|^2, filtry,
 * log10, obciecie do (max - 8), (x + 4) / 4. mel: n_mels x nframes */
static void compute_mel(const whisper *w, const float *pcm, int n, int nframes, float *mel)
{
    init_tables();
    int NM = w->n_mels;
    float frame[N_FFT], spec[2 * N_FFT], power[N_BINS];
    float mmax = -1e30f;
    int last = (n + N_FFT / 2) / HOP + 1;          /* dalsze ramki to same zera */
    for (int i = 0; i < nframes; i++) {
        if (i > last) {
            for (int m = 0; m < NM; m++) mel[m * nframes + i] = -10.0f;    /* log10(1e-10) */
            continue;
        }
        for (int j = 0; j < N_FFT; j++) {
            int s = i * HOP + j - N_FFT / 2;       /* indeks w pcm (z odbiciem na poczatku) */
            float v;
            if (s < 0) v = (-s < n) ? pcm[-s] : 0.0f;
            else v = s < n ? pcm[s] : 0.0f;
            frame[j] = v * s_hann[j];
        }
        fft(frame, N_FFT, spec);
        for (int k = 0; k < N_BINS; k++) power[k] = spec[2 * k] * spec[2 * k] + spec[2 * k + 1] * spec[2 * k + 1];
        for (int m = 0; m < NM; m++) {
            const float *fl = w->filters + m * N_BINS;
            double sum = 0;
            for (int k = 0; k < N_BINS; k++) sum += fl[k] * power[k];
            float v = (float)log10(sum > 1e-10 ? sum : 1e-10);
            mel[m * nframes + i] = v;
            if (v > mmax) mmax = v;
        }
    }
    for (int i = 0; i < NM * nframes; i++) {
        float v = mel[i];
        if (v < mmax - 8.0f) v = mmax - 8.0f;
        mel[i] = (v + 4.0f) / 4.0f;
    }
}

/* ---------------------------------------------------------------- encoder */
static void encode(whisper *w, const float *pcm, int n, int n_ctx)
{
    int S = w->n_state, H = w->n_head, NM = w->n_mels, L = 2 * n_ctx;
    float *mel = (float *)malloc(sizeof(float) * NM * L);
    compute_mel(w, pcm, n, L, mel);
    be_upload(w->mel, mel, (size_t)NM * L);
    free(mel);

    be_conv1d_gelu(w->c1, w->mel, w->conv1_w, w->conv1_b, NM, S, L, 1);
    be_conv1d_gelu(w->c2, w->c1, w->conv2_w, w->conv2_b, S, S, L, 2);
    be_transpose_addpos(w->x, w->c2, w->enc_pos, S, n_ctx);

    for (int l = 0; l < w->n_audio_layer; l++) {
        layer_t *Ly = &w->enc[l];
        be_layernorm(w->h, w->x, Ly->attn_ln_g, Ly->attn_ln_b, n_ctx, S);
        lin(w->q, w->h, Ly->q_w, Ly->q_b, n_ctx, S, S);
        lin(w->k, w->h, Ly->k_w, 0, n_ctx, S, S);
        lin(w->v, w->h, Ly->v_w, Ly->v_b, n_ctx, S, S);
        be_attention(w->att, w->q, w->k, w->v, n_ctx, n_ctx, H, S / H);
        lin(w->h, w->att, Ly->o_w, Ly->o_b, n_ctx, S, S);
        be_add(w->x, w->h, (size_t)n_ctx * S);

        be_layernorm(w->h, w->x, Ly->mlp_ln_g, Ly->mlp_ln_b, n_ctx, S);
        lin(w->mlp, w->h, Ly->m0_w, Ly->m0_b, n_ctx, S, 4 * S);
        be_gelu(w->mlp, (size_t)n_ctx * 4 * S);
        lin(w->h, w->mlp, Ly->m2_w, Ly->m2_b, n_ctx, 4 * S, S);
        be_add(w->x, w->h, (size_t)n_ctx * S);
    }
    be_layernorm(w->h, w->x, w->enc_ln_g, w->enc_ln_b, n_ctx, S);   /* wyjscie encodera w h */

    /* klucze/wartosci uwagi krzyzowej - raz na fragment */
    for (int l = 0; l < w->n_text_layer; l++) {
        layer_t *Ly = &w->dec[l];
        lin(w->xk[l], w->h, Ly->xk_w, 0, n_ctx, S, S);
        lin(w->xv[l], w->h, Ly->xv_w, Ly->xv_b, n_ctx, S, S);
    }
    w->n_ctx = n_ctx;
}

/* ---------------------------------------------------------------- decoder */
/* jeden token na pozycji p; z pamiecia K/V samo-uwagi. want_logits -> w->logits */
static void decode_step(whisper *w, int token, int p, int want_logits)
{
    int S = w->n_state, H = w->n_head;
    if (w->tok_emb.q8) be_embed_q8(w->dx, w->tok_emb.d, w->dec_pos, token, p, S);
    else               be_embed(w->dx, w->tok_emb.d, w->dec_pos, token, p, S);
    for (int l = 0; l < w->n_text_layer; l++) {
        layer_t *Ly = &w->dec[l];
        be_layernorm(w->dh, w->dx, Ly->attn_ln_g, Ly->attn_ln_b, 1, S);
        lin(w->dq, w->dh, Ly->q_w, Ly->q_b, 1, S, S);
        lin(BOFF(w->kc[l], (size_t)p * S), w->dh, Ly->k_w, 0, 1, S, S);
        lin(BOFF(w->vc[l], (size_t)p * S), w->dh, Ly->v_w, Ly->v_b, 1, S, S);
        be_attention(w->datt, w->dq, w->kc[l], w->vc[l], 1, p + 1, H, S / H);
        lin(w->dh, w->datt, Ly->o_w, Ly->o_b, 1, S, S);
        be_add(w->dx, w->dh, S);

        be_layernorm(w->dh, w->dx, Ly->x_ln_g, Ly->x_ln_b, 1, S);
        lin(w->dq, w->dh, Ly->xq_w, Ly->xq_b, 1, S, S);
        be_attention(w->datt, w->dq, w->xk[l], w->xv[l], 1, w->n_ctx, H, S / H);
        lin(w->dh, w->datt, Ly->xo_w, Ly->xo_b, 1, S, S);
        be_add(w->dx, w->dh, S);

        be_layernorm(w->dh, w->dx, Ly->mlp_ln_g, Ly->mlp_ln_b, 1, S);
        lin(w->dmlp, w->dh, Ly->m0_w, Ly->m0_b, 1, S, 4 * S);
        be_gelu(w->dmlp, 4 * S);
        lin(w->dh, w->dmlp, Ly->m2_w, Ly->m2_b, 1, 4 * S, S);
        be_add(w->dx, w->dh, S);
    }
    if (want_logits) {
        be_layernorm(w->dh, w->dx, w->dec_ln_g, w->dec_ln_b, 1, S);
        lin(w->dlogits, w->dh, w->tok_emb, 0, 1, S, w->n_vocab);
        be_download(w->logits, w->dlogits, w->n_vocab);
    }
}

static int argmax_range(const float *l, int a, int b)
{
    int best = a;
    for (int i = a + 1; i <= b; i++) if (l[i] > l[best]) best = i;
    return best;
}

static float softmax_prob(const float *l, int n, int idx)
{
    float mx = l[0];
    for (int i = 1; i < n; i++) if (l[i] > mx) mx = l[i];
    double sum = 0;
    for (int i = 0; i < n; i++) sum += exp(l[i] - mx);
    return (float)(exp(l[idx] - mx) / sum);
}

/* petla powtorzen (typowa usterka Whispera) - zwraca nowa dlugosc albo -1 */
static int cut_repeats(const int *t, int n)
{
    for (int L = 1; L <= 16; L++) {
        int need = L == 1 ? 8 : 4;
        if (n < need * L) continue;
        int rep = 1;
        while (rep < need && !memcmp(t + n - L, t + n - (rep + 1) * L, L * sizeof(int))) rep++;
        if (rep >= need) return n - (need - 1) * L;
    }
    return -1;
}

int wh_transcribe(whisper *w, const float *pcm, int n, int lang, int audio_ctx,
                  char *out, int outsz, float *no_speech, int *lang_out, float *avg_logprob)
{
    if (n > WH_MAX_SAMPLES) n = WH_MAX_SAMPLES;
    int n_ctx = w->n_audio_ctx;
    if (audio_ctx > 0 && audio_ctx < n_ctx) n_ctx = audio_ctx;
    encode(w, pcm, n, n_ctx);

    int p = 0;
    float nsp = 0;
    decode_step(w, w->sot, p++, 1);
    nsp = softmax_prob(w->logits, w->n_vocab, w->nosp);
    if (wh_multilingual(w)) {
        if (lang < 0 || lang >= w->n_langs)
            lang = argmax_range(w->logits, w->sot + 1, w->sot + w->n_langs) - (w->sot + 1);
        decode_step(w, w->sot + 1 + lang, p++, 0);
        decode_step(w, w->transcribe, p++, 0);
    } else {
        lang = 0;
    }
    decode_step(w, w->notok, p++, 1);

    int maxtok = w->n_text_ctx / 2;
    int *toks = (int *)malloc(sizeof(int) * maxtok);
    int nt = 0;
    double lp_sum = 0;
    int lp_n = 0;
    while (nt < maxtok && p < w->n_text_ctx) {
        float *lg = w->logits;
        if (nt == 0) { lg[w->eot] = -1e30f; lg[220] = -1e30f; }   /* bez pustego poczatku */
        int t = argmax_range(lg, 0, w->eot);                        /* tylko tekst + koniec */
        /* log-prawdopodobienstwo wybranego tokenu (wsrod dozwolonych: tekst + koniec) */
        double se = 0;
        for (int i = 0; i <= w->eot; i++) se += exp(lg[i] - lg[t]);
        lp_sum += -log(se);
        lp_n++;
        if (t == w->eot) break;
        toks[nt++] = t;
        int cut = cut_repeats(toks, nt);
        if (cut >= 0) { nt = cut; break; }
        decode_step(w, t, p++, 1);
    }

    int o = 0;
    for (int i = 0; i < nt && o < outsz - 1; i++) {
        int t = toks[i];
        if (t < 0 || t >= w->n_tok) continue;
        int len = w->toklen[t];
        if (o + len >= outsz) len = outsz - 1 - o;
        memcpy(out + o, w->tok[t], len);
        o += len;
    }
    out[o] = 0;
    /* bez wiodacej spacji */
    if (out[0] == ' ') memmove(out, out + 1, strlen(out));
    free(toks);
    if (no_speech) *no_speech = nsp;
    if (lang_out) *lang_out = lang;
    if (avg_logprob) *avg_logprob = lp_n ? (float)(lp_sum / lp_n) : 0;
    return nt;
}
