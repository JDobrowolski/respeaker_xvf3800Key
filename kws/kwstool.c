/*
 * kwstool.c - narzedzie slowa kluczowego dla ReSpeakera XVF3800 (TCC64, Windows).
 *
 *   kwstool record <ip> <katalog> [-prefix punapu] [-n 50]   nagrywa wypowiedzi z urzadzenia (VAD)
 *   kwstool train <dataset> -o model.kws [-keyword punapu]   uczy siec (dataset\keyword, negative, noise)
 *   kwstool test <model.kws> <plik.wav> [-thr 0.8] [-v]      wykrywanie w pliku - ten sam kod co ESP32
 *   kwstool upload <ip> <model.kws>                          wgrywa model do urzadzenia (bez flashowania)
 *   kwstool info <model.kws>
 *
 * Cechy (kws_feat.c), siec (nn.c) i detektor (kws_detect.c) to te same pliki co w firmware.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>

#include "../components/kws/kws_feat.h"
#include "../components/kws/nn.h"
#include "../components/kws/kws_detect.h"
#include "../host/audio_proto.h"
#include "nn_train.h"

#define W        KWS_WINDOW_SAMPLES
#define INSZ     (KWS_FRAMES * KWS_NMEL)
#define PI_F     3.14159265f

static volatile int g_run = 1;
static void on_sigint(int s) { (void)s; g_run = 0; }

static double now_s(void)
{
    static double inv = 0;
    LARGE_INTEGER c;
    if (inv == 0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); inv = 1.0 / (double)f.QuadPart; }
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * inv;
}

/* ================================================================ WAV */
static float *wav_load(const char *path, int *n_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char tag[4]; uint32_t riff, clen, sr = 0, br; uint16_t pcm = 0, ch = 0, align, bps = 0;
    int16_t *d = NULL; uint32_t db = 0; int fmt = 0;
    if (fread(tag, 1, 4, f) != 4 || memcmp(tag, "RIFF", 4)) { fclose(f); return NULL; }
    fread(&riff, 4, 1, f); fread(tag, 1, 4, f);
    for (;;) {
        if (fread(tag, 1, 4, f) != 4 || fread(&clen, 4, 1, f) != 1) break;
        if (!memcmp(tag, "fmt ", 4)) {
            fread(&pcm, 2, 1, f); fread(&ch, 2, 1, f); fread(&sr, 4, 1, f);
            fread(&br, 4, 1, f); fread(&align, 2, 1, f); fread(&bps, 2, 1, f);
            if (clen > 16) fseek(f, (long)(clen - 16), SEEK_CUR);
            fmt = 1;
        } else if (!memcmp(tag, "data", 4)) {
            d = (int16_t *)malloc(clen ? clen : 2);
            db = (uint32_t)fread(d, 1, clen, f);
            break;
        } else fseek(f, (long)clen + (clen & 1), SEEK_CUR);
    }
    fclose(f);
    if (!fmt || !d || pcm != 1 || bps != 16 || ch < 1) { free(d); fprintf(stderr, "%s: tylko WAV PCM 16-bit\n", path); return NULL; }
    int frames = (int)(db / 2 / ch);
    float *x = (float *)malloc(sizeof(float) * (frames ? frames : 1));
    for (int i = 0; i < frames; i++) {
        int s = 0;
        for (int c = 0; c < ch; c++) s += d[i * ch + c];
        x[i] = (float)s / (ch * 32768.0f);
    }
    free(d);
    if ((int)sr != KWS_SR) {                          /* przeprobkowanie do 16 kHz */
        int n2 = (int)((long long)frames * KWS_SR / sr);
        float *y = (float *)malloc(sizeof(float) * (n2 ? n2 : 1));
        double r = (double)sr / KWS_SR;
        for (int i = 0; i < n2; i++) {
            double a = i * r; int ia = (int)a;
            if (r > 1.0) {
                int ib = (int)((i + 1) * r); if (ib > frames) ib = frames; if (ib <= ia) ib = ia + 1;
                double s = 0; for (int k = ia; k < ib; k++) s += x[k];
                y[i] = (float)(s / (ib - ia));
            } else {
                float s0 = x[ia], s1 = ia + 1 < frames ? x[ia + 1] : s0;
                y[i] = s0 + (s1 - s0) * (float)(a - ia);
            }
        }
        free(x); x = y; frames = n2;
    }
    *n_out = frames;
    return x;
}

static int wav_save(const char *path, const int16_t *x, int n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t db = (uint32_t)n * 2, riff = 36 + db, fl = 16, sr = KWS_SR, br = KWS_SR * 2;
    uint16_t pcm = 1, ch = 1, al = 2, bps = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fl, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&al, 2, 1, f); fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&db, 4, 1, f); fwrite(x, 2, n, f);
    fclose(f);
    return 0;
}

static void to_i16(const float *x, int16_t *y, int n)
{
    for (int i = 0; i < n; i++) {
        float v = x[i] * 32767.0f;
        y[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

/* ================================================================ losowosc */
typedef struct { uint64_t s; } rng_t;
static void rseed(rng_t *r, uint64_t a, uint64_t b)
{
    uint64_t z = a * 0x9E3779B97F4A7C15ull + b * 0xBF58476D1CE4E5B9ull + 0x94D049BB133111EBull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    r->s = (z ^ (z >> 31)) | 1;
}
static uint32_t ru(rng_t *r) { r->s ^= r->s << 13; r->s ^= r->s >> 7; r->s ^= r->s << 17; return (uint32_t)(r->s >> 11); }
static float rf(rng_t *r) { return (ru(r) & 0xFFFFFF) / 16777216.0f; }
static float rr(rng_t *r, float a, float b) { return a + (b - a) * rf(r); }
static int   ri(rng_t *r, int n) { return n > 0 ? (int)(ru(r) % (uint32_t)n) : 0; }
static float rg(rng_t *r) { float u = rf(r) + 1e-7f; return (float)(sqrt(-2.0 * log(u)) * cos(6.2831853 * rf(r))); }

static float rms(const float *x, int n)
{
    double s = 0;
    for (int i = 0; i < n; i++) s += (double)x[i] * x[i];
    return n ? (float)sqrt(s / n) : 0;
}

/* ================================================================ dane */
typedef struct { float *x; int n, ks, ke; char name[64]; } pos_t;   /* ks..ke - samo slowo */
typedef struct { float *x; int n, tr_end; } neg_t;                  /* [0,tr_end) trening, reszta walidacja */

typedef struct {
    pos_t *pos; int npos;
    int   *ptr, nptr, *pva, npva;            /* indeksy trening / walidacja */
    neg_t *neg; int nneg;
    int   *nsh_tr, nnsh_tr, *nsh_va, nnsh_va; /* krotkie (< 3 s) */
    int   *nlo, nnlo;                        /* dlugie */
    double lo_tr_total, lo_va_total;
    float **noise; int *noise_n; int nnoise;
} ds_t;

static int list_wavs(const char *dir, char ***out, int recurse)
{
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    int n = 0, cap = 0;
    char **v = *out;
    for (int pass = 0; pass < 2; pass++) {
        sprintf(pat, "%s\\%s", dir, pass == 0 ? "*.wav" : "*");
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (pass == 0 && !isdir) {
                if (n == cap) { cap = cap ? cap * 2 : 64; v = (char **)realloc(v, sizeof(char *) * cap); }
                v[n] = (char *)malloc(strlen(dir) + strlen(fd.cFileName) + 2);
                sprintf(v[n++], "%s\\%s", dir, fd.cFileName);
            } else if (pass == 1 && isdir && recurse && fd.cFileName[0] != '.') {
                char sub[MAX_PATH];
                sprintf(sub, "%s\\%s", dir, fd.cFileName);
                char **sv = NULL;
                int sn = list_wavs(sub, &sv, 1);
                for (int i = 0; i < sn; i++) {
                    if (n == cap) { cap = cap ? cap * 2 : 64; v = (char **)realloc(v, sizeof(char *) * cap); }
                    v[n++] = sv[i];
                }
                free(sv);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    /* stala kolejnosc (podzial trening/walidacja powtarzalny) */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcmp(v[j - 1], v[j]) > 0; j--) { char *t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    *out = v;
    return n;
}

/* zakres slowa: ramki 20 ms z energia > max - 25 dB */
static int speech_extent(const float *x, int n, int *s, int *e)
{
    int nf = n / KWS_HOP;
    if (nf < 3) return -1;
    float *en = (float *)malloc(sizeof(float) * nf), mx = -1e9f;
    for (int f = 0; f < nf; f++) {
        double a = 0;
        for (int i = 0; i < KWS_HOP; i++) { double v = x[f * KWS_HOP + i] * 32768.0; a += v * v; }
        en[f] = (float)(10 * log10(a / KWS_HOP + 1));
        if (en[f] > mx) mx = en[f];
    }
    float th = mx - 25 > 30 ? mx - 25 : 30;
    int a = -1, b = -1;
    for (int f = 0; f < nf; f++) if (en[f] > th) { if (a < 0) a = f; b = f; }
    free(en);
    if (a < 0) return -1;
    *s = a * KWS_HOP; *e = (b + 1) * KWS_HOP;
    return 0;
}

static int load_dataset(ds_t *ds, const char *root)
{
    memset(ds, 0, sizeof *ds);
    char dir[MAX_PATH];
    char **files = NULL;
    int n;

    sprintf(dir, "%s\\keyword", root);
    files = NULL; n = list_wavs(dir, &files, 1);
    ds->pos = (pos_t *)calloc(n ? n : 1, sizeof(pos_t));
    for (int i = 0; i < n; i++) {
        pos_t *p = &ds->pos[ds->npos];
        p->x = wav_load(files[i], &p->n);
        if (!p->x || speech_extent(p->x, p->n, &p->ks, &p->ke)) { fprintf(stderr, "  pomijam %s (brak mowy)\n", files[i]); free(p->x); continue; }
        if (p->ke - p->ks > (int)(0.95f * W)) { fprintf(stderr, "  pomijam %s (slowo dluzsze niz 1 s)\n", files[i]); free(p->x); continue; }
        const char *b = strrchr(files[i], '\\');
        strncpy(p->name, b ? b + 1 : files[i], sizeof p->name - 1);
        ds->npos++;
    }
    ds->ptr = (int *)malloc(sizeof(int) * (ds->npos + 1)); ds->pva = (int *)malloc(sizeof(int) * (ds->npos + 1));
    for (int i = 0; i < ds->npos; i++) { if (i % 7 == 3) ds->pva[ds->npva++] = i; else ds->ptr[ds->nptr++] = i; }

    sprintf(dir, "%s\\negative", root);
    files = NULL; n = list_wavs(dir, &files, 1);
    ds->neg = (neg_t *)calloc(n ? n : 1, sizeof(neg_t));
    ds->nsh_tr = (int *)malloc(sizeof(int) * (n + 1)); ds->nsh_va = (int *)malloc(sizeof(int) * (n + 1));
    ds->nlo = (int *)malloc(sizeof(int) * (n + 1));
    int nshort = 0;
    for (int i = 0; i < n; i++) {
        neg_t *g = &ds->neg[ds->nneg];
        g->x = wav_load(files[i], &g->n);
        if (!g->x || g->n < KWS_SR / 4) { free(g->x); continue; }
        int idx = ds->nneg++;
        if (g->n < 3 * KWS_SR) {
            g->tr_end = g->n;
            if (nshort++ % 7 == 3) ds->nsh_va[ds->nnsh_va++] = idx; else ds->nsh_tr[ds->nnsh_tr++] = idx;
        } else {
            g->tr_end = (int)(g->n * 0.8);
            ds->nlo[ds->nnlo++] = idx;
            ds->lo_tr_total += g->tr_end; ds->lo_va_total += g->n - g->tr_end;
        }
    }

    sprintf(dir, "%s\\noise", root);
    files = NULL; n = list_wavs(dir, &files, 1);
    ds->noise = (float **)calloc(n ? n : 1, sizeof(float *)); ds->noise_n = (int *)calloc(n ? n : 1, sizeof(int));
    for (int i = 0; i < n; i++) {
        int nn;
        float *x = wav_load(files[i], &nn);
        if (x && nn > W) { ds->noise[ds->nnoise] = x; ds->noise_n[ds->nnoise++] = nn; } else free(x);
    }
    fprintf(stderr, "dane: slowo %d plikow (trening %d, walidacja %d), negatywy %d krotkich + %d dlugich "
            "(%.1f min), szum %d plikow\n", ds->npos, ds->nptr, ds->npva, ds->nnsh_tr + ds->nnsh_va, ds->nnlo,
            (ds->lo_tr_total + ds->lo_va_total) / KWS_SR / 60.0, ds->nnoise);
    return ds->npos > 0 && ds->nneg > 0 ? 0 : -1;
}

/* ================================================================ augmentacja */
static void synth_noise(rng_t *r, float *y, int n)
{
    int kind = ri(r, 3);
    float b0 = 0, b1 = 0, b2 = 0, br = 0;
    for (int i = 0; i < n; i++) {
        float w = rg(r);
        if (kind == 0) y[i] = w;                                         /* bialy */
        else if (kind == 1) {                                            /* rozowy (Kellet) */
            b0 = 0.99765f * b0 + w * 0.0990460f; b1 = 0.96300f * b1 + w * 0.2965164f;
            b2 = 0.57000f * b2 + w * 1.0526913f; y[i] = b0 + b1 + b2 + w * 0.1848f;
        } else { br = 0.995f * br + w * 0.1f; y[i] = br; }               /* brazowy */
    }
}

/* fragment dlugiego negatywu (val: z czesci walidacyjnej) */
static int long_seg(const ds_t *ds, rng_t *r, int val, float *y, int n)
{
    if (!ds->nnlo) return -1;
    double tot = val ? ds->lo_va_total : ds->lo_tr_total, pick = rf(r) * tot;
    for (int k = 0; k < ds->nnlo; k++) {
        const neg_t *g = &ds->neg[ds->nlo[k]];
        int a = val ? g->tr_end : 0, b = val ? g->n : g->tr_end;
        if (pick > b - a && k < ds->nnlo - 1) { pick -= b - a; continue; }
        if (b - a <= n) { memset(y, 0, sizeof(float) * n); memcpy(y, g->x + a, sizeof(float) * (b - a)); return 0; }
        memcpy(y, g->x + a + ri(r, b - a - n), sizeof(float) * n);
        return 0;
    }
    return -1;
}

/* szum tla o zadanym rms */
static void add_noise(const ds_t *ds, rng_t *r, int val, float *w, float level, int babble)
{
    float *nz = (float *)malloc(sizeof(float) * W);
    float t = rf(r);
    if (ds->nnoise && t < 0.35f) {
        int k = ri(r, ds->nnoise);
        memcpy(nz, ds->noise[k] + ri(r, ds->noise_n[k] - W), sizeof(float) * W);
    } else if (babble && t < 0.6f && long_seg(ds, r, val, nz, W) == 0) {
    } else synth_noise(r, nz, W);
    float nr = rms(nz, W);
    if (nr > 1e-7f) { float g = level / nr; for (int i = 0; i < W; i++) w[i] += nz[i] * g; }
    free(nz);
}

static float db2a(float db) { return (float)pow(10.0, db / 20.0); }

/* slowo kluczowe w oknie; cut > 0 = tylko poczatkowa czesc slowa (przyklad negatywny) */
static void pos_window(const ds_t *ds, rng_t *r, int val, float *w, float cut)
{
    const pos_t *p = &ds->pos[val ? ds->pva[ri(r, ds->npva)] : ds->ptr[ri(r, ds->nptr)]];
    float sp = val ? 1.0f : rr(r, 0.9f, 1.1f);                     /* tempo (i wysokosc) */
    int n = (int)(p->n / sp);
    float *c = (float *)malloc(sizeof(float) * (n + 1));
    for (int i = 0; i < n; i++) {
        float a = i * sp; int ia = (int)a;
        float s0 = p->x[ia], s1 = ia + 1 < p->n ? p->x[ia + 1] : s0;
        c[i] = s0 + (s1 - s0) * (a - ia);
    }
    int ks = (int)(p->ks / sp), ke = (int)(p->ke / sp);
    int end = ke;
    if (cut > 0) end = ks + (int)((ke - ks) * cut);
    int L = end - ks;
    int lo = L > (int)(0.55f * W) ? L : (int)(0.55f * W), hi = (int)(0.97f * W);
    if (lo > hi) lo = hi;
    int endpos = val ? (lo + hi) / 2 : lo + ri(r, hi - lo + 1);
    int off = endpos - end;
    memset(w, 0, sizeof(float) * W);
    for (int j = 0; j < W; j++) {
        int s = j - off;
        if (s >= 0 && s < n && (cut <= 0 || s < end)) w[j] = c[s];
    }
    /* poziom: rms slowa -> losowo -34..-12 dBFS */
    int a = ks + off, b = end + off;
    if (a < 0) a = 0;
    if (b > W) b = W;
    float sr = rms(w + a, b - a);
    if (sr > 1e-6f) {
        float g = db2a(val ? -22.0f : rr(r, -34.0f, -12.0f)) / sr;
        for (int j = 0; j < W; j++) w[j] *= g;
    }
    free(c);
}

/* przyklad negatywny: mowa/inne slowa, szum/cisza albo poczatek slowa kluczowego */
static void neg_window(const ds_t *ds, rng_t *r, int val, float *w)
{
    float t = rf(r);
    if (t < 0.62f) {
        int use_short = (val ? ds->nnsh_va : ds->nnsh_tr) > 0 && (!ds->nnlo || rf(r) < 0.5f);
        if (use_short) {
            const neg_t *g = &ds->neg[val ? ds->nsh_va[ri(r, ds->nnsh_va)] : ds->nsh_tr[ri(r, ds->nnsh_tr)]];
            memset(w, 0, sizeof(float) * W);
            if (g->n <= W) memcpy(w + ri(r, W - g->n + 1), g->x, sizeof(float) * g->n);
            else memcpy(w, g->x + ri(r, g->n - W), sizeof(float) * W);
        } else if (long_seg(ds, r, val, w, W)) {
            synth_noise(r, w, W);
        }
        float s = rms(w, W);
        if (s > 1e-6f) { float g = db2a(rr(r, -38.0f, -12.0f)) / s; for (int i = 0; i < W; i++) w[i] *= g; }
        if (rf(r) < 0.6f) add_noise(ds, r, val, w, rms(w, W) * db2a(-rr(r, 8.0f, 30.0f)) + 1e-5f, 0);
    } else if (t < 0.82f || (val ? ds->npva : ds->nptr) == 0) {
        memset(w, 0, sizeof(float) * W);
        add_noise(ds, r, val, w, db2a(rr(r, -70.0f, -25.0f)), 1);
    } else {
        pos_window(ds, r, val, w, rr(r, 0.25f, 0.6f));
        add_noise(ds, r, val, w, db2a(rr(r, -60.0f, -35.0f)), 0);
    }
}

static void make_example(const ds_t *ds, rng_t *r, int label, int val, float *feat)
{
    float *w = (float *)malloc(sizeof(float) * W);
    int16_t *q = (int16_t *)malloc(sizeof(int16_t) * W);
    if (label) {
        pos_window(ds, r, val, w, 0);
        if (rf(r) < (val ? 0.5f : 0.85f)) {
            float ref = rms(w, W) * 3.0f;                             /* ~rms samego slowa */
            add_noise(ds, r, val, w, ref * db2a(-(val ? 15.0f : rr(r, 3.0f, 30.0f))), rf(r) < 0.3f);
        }
    } else {
        neg_window(ds, r, val, w);
    }
    to_i16(w, q, W);
    kws_feat_window(q, feat);
    free(w); free(q);
}

/* ================================================================ trening */
typedef struct { const ds_t *ds; int val; uint64_t seed; const int *lab; float *X; int base; } gen_t;

static void job_gen(void *c, int i0, int i1, int th)
{
    gen_t *g = (gen_t *)c;
    (void)th;
    for (int i = i0; i < i1; i++) {
        rng_t r;
        rseed(&r, g->seed, (uint64_t)(g->base + i));
        make_example(g->ds, &r, g->lab[i], g->val, g->X + (size_t)i * INSZ);
    }
}

/* DS-CNN: splot wejsciowy + bloki depthwise/pointwise.
 *   small  ~0,5 mln MAC,  6,5 tys. parametrow
 *   medium ~1,2 mln MAC, ~24 tys. (domyslny - ESP32-S3 ok. 20 ms co 60 ms)
 *   large  ~3,1 mln MAC, ~34 tys. (za ciezki na ESP32 przy kroku 60 ms) */
enum { ARCH_SMALL, ARCH_MEDIUM, ARCH_LARGE };
static void build_model(nn_model *m, int arch)
{
    nn_init(m, KWS_FRAMES, KWS_NMEL, 1);          /* 49 x 40 x 1 */
    if (arch == ARCH_LARGE) {
        nn_conv(m, 32, 3, 3, 2, 2, 1);            /* 25 x 20 */
        nn_dwconv(m, 3, 3, 2, 2, 1);              /* 13 x 10 */
        nn_conv(m, 64, 1, 1, 1, 1, 1);
        nn_dwconv(m, 3, 3, 1, 1, 1); nn_conv(m, 96, 1, 1, 1, 1, 1);
        nn_dwconv(m, 3, 3, 1, 1, 1); nn_conv(m, 96, 1, 1, 1, 1, 1);
        nn_dwconv(m, 3, 3, 2, 2, 1);              /*  7 x  5 */
        nn_conv(m, 128, 1, 1, 1, 1, 1);
    } else if (arch == ARCH_MEDIUM) {
        nn_conv(m, 16, 3, 3, 2, 2, 1);            /* 25 x 20 */
        nn_dwconv(m, 3, 3, 2, 2, 1);              /* 13 x 10 */
        nn_conv(m, 48, 1, 1, 1, 1, 1);
        nn_dwconv(m, 3, 3, 1, 1, 1); nn_conv(m, 64, 1, 1, 1, 1, 1);
        nn_dwconv(m, 3, 3, 2, 2, 1);              /*  7 x  5 */
        nn_conv(m, 96, 1, 1, 1, 1, 1);
        nn_dwconv(m, 3, 3, 1, 1, 1); nn_conv(m, 96, 1, 1, 1, 1, 1);
    } else {
        nn_conv(m, 16, 3, 3, 2, 2, 1);            /* 25 x 20 */
        nn_dwconv(m, 3, 3, 2, 2, 1);              /* 13 x 10 */
        nn_conv(m, 32, 1, 1, 1, 1, 1);
        nn_dwconv(m, 3, 3, 1, 1, 1); nn_conv(m, 48, 1, 1, 1, 1, 1);
        nn_dwconv(m, 3, 3, 2, 2, 1);              /*  7 x  5 */
        nn_conv(m, 64, 1, 1, 1, 1, 1);
    }
    nn_gap(m);
    nn_dense(m, 2, 0);                            /* [inne, slowo] */
    nn_alloc(m);
}

/* przebieg detektora po strumieniu (rownolegle w kawalkach) -> wynik po kazdym kroku */
typedef struct { const nn_model *m; kws_meta meta; const int16_t *x; int nhops; float *score; int chunk; } scan_t;

static void job_scan(void *c, int i0, int i1, int th)
{
    scan_t *s = (scan_t *)c;
    (void)th;
    kws_det *d = (kws_det *)malloc(sizeof(kws_det));
    kws_meta mt = s->meta;
    mt.threshold = 0.999f;
    kws_det_init(d, s->m, &mt);
    d->thr = 2.0f;                                /* nigdy nie wyzwala - czysty przebieg wyniku */
    for (int k = i0; k < i1; k++) {
        int h0 = k * s->chunk, h1 = h0 + s->chunk;
        if (h1 > s->nhops) h1 = s->nhops;
        int w0 = h0 - 60 < 0 ? 0 : h0 - 60;       /* 1,2 s rozgrzewki */
        kws_det_reset(d);
        float last = 0;
        for (int h = w0; h < h1; h++) {
            int ran;
            kws_det_hop(d, s->x + (size_t)h * KWS_HOP, &ran);
            if (ran) last = d->score;
            if (h >= h0) s->score[h] = last;
        }
    }
    kws_det_free(d);
    free(d);
}

static float *scan_stream(nn_trainer *t, const nn_model *m, const kws_meta *meta, const int16_t *x, int n, int *nh)
{
    scan_t s;
    s.m = m; s.meta = *meta; s.x = x; s.nhops = n / KWS_HOP; s.chunk = 1500;
    s.score = (float *)calloc(s.nhops + 1, sizeof(float));
    nn_parallel(t, job_scan, &s, (s.nhops + s.chunk - 1) / s.chunk);
    *nh = s.nhops;
    return s.score;
}

static int count_fa(const float *sc, int nh, float thr, int refr_hops)
{
    int fa = 0, next = 0;
    for (int h = 0; h < nh; h++) if (h >= next && sc[h] >= thr) { fa++; next = h + refr_hops; }
    return fa;
}

static int cmd_train(int argc, char **argv)
{
    const char *root = NULL, *out = "model.kws", *kw = "punapu";
    int epochs = 60, aug = 32, threads = 0, batch = 64, arch = ARCH_MEDIUM;
    float lr0 = 3e-3f, target_fa = 0.5f;
    unsigned seed = 1234;
    for (int i = 0; i < argc; i++) {
        int more = i + 1 < argc;
        if (!strcmp(argv[i], "-o") && more) out = argv[++i];
        else if (!strcmp(argv[i], "-keyword") && more) kw = argv[++i];
        else if (!strcmp(argv[i], "-epochs") && more) epochs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-aug") && more) aug = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && more) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-lr") && more) lr0 = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "-fa") && more) target_fa = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "-seed") && more) seed = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-arch") && more) {
            i++;
            arch = !strcmp(argv[i], "small") ? ARCH_SMALL : !strcmp(argv[i], "large") ? ARCH_LARGE : ARCH_MEDIUM;
        }
        else if (argv[i][0] != '-' && !root) root = argv[i];
        else { fprintf(stderr, "nieznana opcja %s\n", argv[i]); return 2; }
    }
    if (!root) { fprintf(stderr, "kwstool train <dataset> [-o model.kws] [-keyword slowo] [-epochs 40]\n"); return 2; }
    kws_feat_init();
    ds_t ds;
    if (load_dataset(&ds, root)) { fprintf(stderr, "potrzebne pliki w %s\\keyword i %s\\negative\n", root, root); return 1; }
    if (ds.npva == 0) { ds.pva[ds.npva++] = ds.ptr[--ds.nptr]; }

    nn_model m;
    build_model(&m, arch);
    nn_he_init(&m, seed);
    nn_trainer *t = nn_trainer_new(&m, threads);
    fprintf(stderr, "siec: %d warstw, %d parametrow, %.2f mln MAC na wnioskowanie, %d watkow\n",
            m.nl, m.nparams, nn_macs(&m) / 1e6, nn_threads(t));

    /* stala walidacja */
    int nvp = ds.npva * 6, nvn = nvp * 3 < 600 ? 600 : nvp * 3, nv = nvp + nvn;
    int *vlab = (int *)malloc(sizeof(int) * nv);
    for (int i = 0; i < nv; i++) vlab[i] = i < nvp;
    float *VX = (float *)malloc(sizeof(float) * (size_t)nv * INSZ), *VP = (float *)malloc(sizeof(float) * nv * 2);
    gen_t g = { &ds, 1, 777, vlab, VX, 0 };
    nn_parallel(t, job_gen, &g, nv);

    int npe = ds.nptr * aug < 800 ? 800 : ds.nptr * aug;    /* pozytywy na epoke */
    int ne = npe * 4;                                         /* 1 : 3 */
    int *lab = (int *)malloc(sizeof(int) * batch);
    float *X = (float *)malloc(sizeof(float) * (size_t)batch * INSZ);
    float *best = (float *)malloc(sizeof(float) * m.nparams);
    float best_score = -1;
    fprintf(stderr, "epoka: %d przykladow (%d ze slowem), paczka %d\n", ne, npe, batch);

    for (int ep = 0; ep < epochs && g_run; ep++) {
        double t0 = now_s(), loss = 0;
        int ok = 0, seen = 0;
        float lr = 1e-4f + (lr0 - 1e-4f) * 0.5f * (1.0f + (float)cos(PI_F * ep / epochs));
        for (int b0 = 0; b0 < ne && g_run; b0 += batch) {
            int n = ne - b0 < batch ? ne - b0 : batch;
            for (int i = 0; i < n; i++) lab[i] = ((b0 + i) % 4) == 0;
            gen_t gg = { &ds, 0, (uint64_t)seed * 1000003u + ep, lab, X, b0 };
            nn_parallel(t, job_gen, &gg, n);
            int c;
            loss += nn_train_step(t, X, lab, n, lr, 1e-4f, &c) * n;
            ok += c; seen += n;
        }
        nn_predict(t, VX, nv, VP);
        int tp = 0, fp = 0;
        for (int i = 0; i < nv; i++) {
            int pr = VP[i * 2 + 1] >= 0.5f;
            if (vlab[i] && pr) tp++;
            if (!vlab[i] && pr) fp++;
        }
        float rec = 100.0f * tp / nvp, fpr = 100.0f * fp / nvn;
        float score = rec - 4.0f * fpr;                       /* falszywe alarmy wazniejsze */
        int is_best = score > best_score;
        if (is_best) { best_score = score; memcpy(best, m.p, sizeof(float) * m.nparams); }
        fprintf(stderr, "epoka %2d/%d  lr %.4f  strata %.3f  traf %.1f%%  | walidacja: wykryte %.1f%%  falszywe %.2f%%  (%.1f s)%s\n",
                ep + 1, epochs, lr, loss / seen, 100.0 * ok / seen, rec, fpr, now_s() - t0, is_best ? "  *" : "");
    }
    memcpy(m.p, best, sizeof(float) * m.nparams);

    /* ---- dobor progu na strumieniach: falszywe alarmy na godzine i skutecznosc */
    kws_meta meta;
    memset(&meta, 0, sizeof meta);
    strncpy(meta.keyword, kw, sizeof meta.keyword - 1);
    meta.threshold = 0.8f; meta.smooth = 3; meta.stride = 3; meta.refractory_ms = 1500;

    /* zwykla mowa / tlo: czesci walidacyjne dlugich negatywow + minuta szumu -> alarmy na godzine */
    int cap = (int)ds.lo_va_total + KWS_SR * 60, nn = 0;
    float *ns = (float *)calloc(cap, sizeof(float));
    for (int k = 0; k < ds.nnlo; k++) {
        const neg_t *gn = &ds.neg[ds.nlo[k]];
        memcpy(ns + nn, gn->x + gn->tr_end, sizeof(float) * (gn->n - gn->tr_end));
        nn += gn->n - gn->tr_end;
    }
    rng_t r;
    rseed(&r, 99, 1);
    { float *z = (float *)malloc(sizeof(float) * KWS_SR * 60); synth_noise(&r, z, KWS_SR * 60);
      float s = rms(z, KWS_SR * 60); for (int i = 0; i < KWS_SR * 60; i++) ns[nn + i] = z[i] / s * 0.003f;
      nn += KWS_SR * 60; free(z); }
    int16_t *nq = (int16_t *)malloc(sizeof(int16_t) * nn);
    to_i16(ns, nq, nn);
    int nh;
    float *nsc = scan_stream(t, &m, &meta, nq, nn, &nh);
    double hours = nn / (double)KWS_SR / 3600.0;

    /* pozytywy: kazdy plik walidacyjny w 3 wariantach (czysto, szum 15 dB, szum 5 dB) */
    int npv = ds.npva * 3, plen = KWS_SR * 3 + W;
    float *pmax = (float *)calloc(npv, sizeof(float));
    for (int i = 0; i < npv; i++) {
        const pos_t *p = &ds.pos[ds.pva[i / 3]];
        float *s = (float *)calloc(plen, sizeof(float));
        int a = KWS_SR, len = p->n < plen - 2 * KWS_SR ? p->n : plen - 2 * KWS_SR;
        float sr = rms(p->x + p->ks, p->ke - p->ks), gk = sr > 1e-6f ? db2a(-22) / sr : 1;
        for (int j = 0; j < len; j++) s[a + j] = p->x[j] * gk;
        if (i % 3) {
            float *z = (float *)malloc(sizeof(float) * plen);
            synth_noise(&r, z, plen);
            float zr = rms(z, plen), lv = db2a(-22) * db2a(i % 3 == 1 ? -15.0f : -5.0f) / (zr + 1e-9f);
            for (int j = 0; j < plen; j++) s[j] += z[j] * lv;
            free(z);
        }
        int16_t *q = (int16_t *)malloc(sizeof(int16_t) * plen);
        to_i16(s, q, plen);
        int ph;
        float *sc = scan_stream(t, &m, &meta, q, plen, &ph);
        for (int h = 0; h < ph; h++) if (sc[h] > pmax[i]) pmax[i] = sc[h];
        free(sc); free(q); free(s);
    }

    /* krotkie negatywy walidacyjne (podobne slowa, inne frazy) - kazdy osobno, jak pozytywy */
    int nsv = ds.nnsh_va;
    float *smax = (float *)calloc(nsv + 1, sizeof(float));
    for (int i = 0; i < nsv; i++) {
        const neg_t *gn = &ds.neg[ds.nsh_va[i]];
        int len = gn->n + 2 * KWS_SR;
        float *s = (float *)calloc(len, sizeof(float));
        float sr = rms(gn->x, gn->n), gk = sr > 1e-6f ? db2a(-24) / sr : 1;
        for (int j = 0; j < gn->n; j++) s[KWS_SR + j] = gn->x[j] * gk;
        int16_t *q = (int16_t *)malloc(sizeof(int16_t) * len);
        to_i16(s, q, len);
        int ph;
        float *sc = scan_stream(t, &m, &meta, q, len, &ph);
        for (int h = 0; h < ph; h++) if (sc[h] > smax[i]) smax[i] = sc[h];
        free(sc); free(q); free(s);
    }

    /* prog: najnizszy >= 0.5 z alarmami na zwyklej mowie <= -fa/h i <= 10% podobnych slow */
    int refr_h = meta.refractory_ms / 20;
    float chosen = -1;
    fprintf(stderr, "\nprog | wykryte (czysto / 15 dB / 5 dB) | alarmy/h na mowie (%.0f min) | falszywe na %d krotkich negatywach\n",
            hours * 60, nsv);
    for (int ti = 30; ti <= 99; ti++) {
        float thr = ti / 100.0f;
        int hit[3] = { 0, 0, 0 }, cnt[3] = { 0, 0, 0 }, sfa = 0;
        for (int i = 0; i < npv; i++) { cnt[i % 3]++; if (pmax[i] >= thr) hit[i % 3]++; }
        for (int i = 0; i < nsv; i++) if (smax[i] >= thr) sfa++;
        int fa = count_fa(nsc, nh, thr, refr_h);
        float sfr = nsv ? 100.0f * sfa / nsv : 0;
        if (chosen < 0 && thr >= 0.5f && fa / hours <= target_fa + 1e-6 && sfr <= 10.0f) chosen = thr;
        if (ti % 5 == 0 || thr == chosen)
            fprintf(stderr, "%.2f |   %5.1f%%  %5.1f%%  %5.1f%%           | %6.1f                 | %5.1f%%%s\n", thr,
                    100.0f * hit[0] / cnt[0], 100.0f * hit[1] / cnt[1], 100.0f * hit[2] / cnt[2],
                    fa / hours, sfr, thr == chosen ? "   <- wybrany" : "");
    }
    if (chosen < 0) chosen = 0.95f;
    meta.threshold = chosen;

    size_t sz = kws_model_write(NULL, 0, &m, &meta);
    void *buf = malloc(sz);
    kws_model_write(buf, sz, &m, &meta);
    FILE *f = fopen(out, "wb");
    if (!f || fwrite(buf, 1, sz, f) != sz) { fprintf(stderr, "nie moge zapisac %s\n", out); return 1; }
    fclose(f);
    fprintf(stderr, "\nzapisano %s: slowo \"%s\", prog %.2f, %u bajtow\n", out, meta.keyword, meta.threshold, (unsigned)sz);
    nn_trainer_free(t);
    return 0;
}

/* ================================================================ test / info */
static int load_model_file(const char *path, nn_model *m, kws_meta *meta)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "nie moge otworzyc %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *b = malloc(n);
    fread(b, 1, n, f);
    fclose(f);
    int rc = kws_model_parse(b, (size_t)n, m, meta, 1);
    free(b);
    if (rc) fprintf(stderr, "%s: zly model (%d)\n", path, rc);
    return rc;
}

static int cmd_info(int argc, char **argv)
{
    if (argc < 1) return 2;
    nn_model m; kws_meta meta;
    if (load_model_file(argv[0], &m, &meta)) return 1;
    printf("slowo: %s\nprog: %.2f, wygladzanie %d, krok %d ramek, martwy czas %d ms\n",
           meta.keyword, meta.threshold, meta.smooth, meta.stride, meta.refractory_ms);
    printf("siec: %d warstw, %d parametrow, %.2f mln MAC, aktywacje max %d floatow\n",
           m.nl, m.nparams, nn_macs(&m) / 1e6, m.max_act);
    return 0;
}

static int cmd_test(int argc, char **argv)
{
    const char *mp = NULL, *wp = NULL;
    float thr = -1;
    int verbose = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-thr") && i + 1 < argc) thr = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!mp) mp = argv[i];
        else if (!wp) wp = argv[i];
    }
    if (!mp || !wp) { fprintf(stderr, "kwstool test <model.kws> <plik.wav> [-thr x] [-v]\n"); return 2; }
    nn_model m; kws_meta meta;
    if (load_model_file(mp, &m, &meta)) return 1;
    int n;
    float *x = wav_load(wp, &n);
    if (!x) { fprintf(stderr, "nie moge wczytac %s\n", wp); return 1; }
    int16_t *q = (int16_t *)malloc(sizeof(int16_t) * n);
    to_i16(x, q, n);
    kws_det *d = (kws_det *)malloc(sizeof(kws_det));
    kws_det_init(d, &m, &meta);
    if (thr > 0) d->thr = thr;
    fprintf(stderr, "slowo \"%s\", prog %.2f, plik %.1f s\n", meta.keyword, d->thr, n / (double)KWS_SR);
    int hits = 0;
    double t0 = now_s();
    for (int h = 0; (h + 1) * KWS_HOP <= n; h++) {
        int ran;
        if (kws_det_hop(d, q + h * KWS_HOP, &ran)) {
            hits++;
            printf("%8.2f s  wykryto \"%s\"  wynik %.2f\n", (h + 1) * KWS_HOP / (double)KWS_SR, meta.keyword, d->score);
        }
        if (verbose && ran && (h % 5) == 0) {
            char bar[41];
            int k = (int)(d->score * 40);
            for (int i = 0; i < 40; i++) bar[i] = i < k ? '#' : (i == (int)(d->thr * 40) ? '|' : '.');
            bar[40] = 0;
            fprintf(stderr, "%8.2f s  %s %.2f\n", (h + 1) * KWS_HOP / (double)KWS_SR, bar, d->score);
        }
    }
    double dt = now_s() - t0;
    fprintf(stderr, "wykryc: %d, czas %.2f s (%.0fx szybciej niz czas rzeczywisty)\n", hits, dt, n / (double)KWS_SR / dt);
    return 0;
}

/* ================================================================ siec (urzadzenie) */
static int recv_all(SOCKET s, void *buf, int len)
{
    char *p = (char *)buf;
    while (len > 0) { int k = recv(s, p, len, 0); if (k <= 0) return -1; p += k; len -= k; }
    return 0;
}

static int send_frame(SOCKET s, unsigned char type, const void *data, int len)
{
    unsigned char hdr[3] = { type, (unsigned char)(len & 0xFF), (unsigned char)(len >> 8) };
    if (send(s, (const char *)hdr, 3, 0) != 3) return -1;
    if (len > 0 && send(s, (const char *)data, len, 0) != len) return -1;
    return 0;
}

static SOCKET dev_connect(const char *hostport, unsigned char *flags)
{
    char host[128];
    int port = 7778;
    strncpy(host, hostport, sizeof host - 1); host[sizeof host - 1] = 0;
    char *c = strrchr(host, ':');
    if (c) { *c = 0; port = atoi(c + 1); }
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    char ps[16]; sprintf(ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) || !res) { fprintf(stderr, "nie moge rozwiazac %s\n", host); return INVALID_SOCKET; }
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET || connect(s, res->ai_addr, (int)res->ai_addrlen)) {
        fprintf(stderr, "polaczenie z %s nie powiodlo sie\n", hostport);
        freeaddrinfo(res); return INVALID_SOCKET;
    }
    freeaddrinfo(res);
    unsigned char h[AUDIO_HELLO_LEN];
    if (recv_all(s, h, sizeof h) || memcmp(h, AUDIO_MAGIC, 4)) { fprintf(stderr, "zle powitanie\n"); closesocket(s); return INVALID_SOCKET; }
    if (flags) *flags = h[7];
    return s;
}

static int cmd_upload(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "kwstool upload <ip[:port]> <model.kws>\n"); return 2; }
    nn_model m; kws_meta meta;
    if (load_model_file(argv[1], &m, &meta)) return 1;
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = (unsigned char *)malloc(n);
    fread(b, 1, n, f); fclose(f);
    unsigned char fl = 0;
    SOCKET s = dev_connect(argv[0], &fl);
    if (s == INVALID_SOCKET) return 1;
    if (!(fl & AUDIO_FL_KWS)) { fprintf(stderr, "urzadzenie nie obsluguje slowa kluczowego (stary firmware?)\n"); return 1; }
    unsigned char hdr[5];
    hdr[0] = AF_MODEL_BEGIN; memcpy(hdr + 1, &n, 4);
    send_frame(s, AF_MODEL, hdr, 5);
    unsigned char chunk[1 + 4000];
    for (long off = 0; off < n; off += 4000) {
        int k = n - off < 4000 ? (int)(n - off) : 4000;
        chunk[0] = AF_MODEL_DATA; memcpy(chunk + 1, b + off, k);
        if (send_frame(s, AF_MODEL, chunk, k + 1)) { fprintf(stderr, "polaczenie zerwane\n"); return 1; }
    }
    uint32_t crc = kws_crc32(b, n);
    hdr[0] = AF_MODEL_END; memcpy(hdr + 1, &crc, 4);
    send_frame(s, AF_MODEL, hdr, 5);
    fprintf(stderr, "wyslano %ld bajtow (slowo \"%s\"), czekam na potwierdzenie...\n", n, meta.keyword);
    DWORD tv = 20000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof tv);
    for (;;) {
        unsigned char h3[3], pl[AUDIO_MAX_PAYLOAD + 1];
        if (recv_all(s, h3, 3)) { fprintf(stderr, "brak odpowiedzi urzadzenia\n"); return 1; }
        int len = h3[1] | (h3[2] << 8);
        if (len > AUDIO_MAX_PAYLOAD || (len && recv_all(s, pl, len))) return 1;
        if (h3[0] == AF_STATUS) {
            pl[len] = 0;
            printf("urzadzenie: %s\n", (char *)pl);
            closesocket(s);
            return strncmp((char *)pl, "OK", 2) ? 1 : 0;
        }
    }
}

static int cmd_record(int argc, char **argv)
{
    const char *dev = NULL, *dir = NULL, *prefix = "punapu";
    int limit = 0;
    float vad_db = 9.0f;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-prefix") && i + 1 < argc) prefix = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-vad") && i + 1 < argc) vad_db = (float)atof(argv[++i]);
        else if (!dev) dev = argv[i];
        else if (!dir) dir = argv[i];
    }
    if (!dev || !dir) { fprintf(stderr, "kwstool record <ip> <katalog> [-prefix nazwa] [-n ile]\n"); return 2; }
    CreateDirectoryA(dir, NULL);
    int idx = 0;
    char path[MAX_PATH];
    for (;; idx++) { sprintf(path, "%s\\%s_%04d.wav", dir, prefix, idx); FILE *f = fopen(path, "rb"); if (!f) break; fclose(f); }

    SOCKET s = dev_connect(dev, NULL);
    if (s == INVALID_SOCKET) return 1;
    unsigned char on = 1;
    send_frame(s, AF_REC, &on, 1);
    fprintf(stderr, "mow \"%s\" z przerwami ~1 s (Ctrl-C konczy); zapis do %s od numeru %d\n", prefix, dir, idx);

    enum { PRE = 15, MAXU = KWS_SR * 4 };
    int16_t fr[KWS_HOP], pre[PRE * KWS_HOP], *u = (int16_t *)malloc(sizeof(int16_t) * MAXU);
    int fill = 0, npre = 0, speaking = 0, run = 0, sil = 0, un = 0, saved = 0, init = 0;
    float nf = 0;
    unsigned char pl[AUDIO_MAX_PAYLOAD];
    while (g_run && (!limit || saved < limit)) {
        unsigned char h3[3];
        if (recv_all(s, h3, 3)) break;
        int len = h3[1] | (h3[2] << 8);
        if (len > AUDIO_MAX_PAYLOAD || (len && recv_all(s, pl, len))) break;
        if (h3[0] != AF_MIC) continue;
        const int16_t *smp = (const int16_t *)pl;
        for (int i = 0; i < len / 2; i++) {
            fr[fill++] = smp[i];
            if (fill < KWS_HOP) continue;
            fill = 0;
            double e2 = 0;
            for (int k = 0; k < KWS_HOP; k++) e2 += (double)fr[k] * fr[k];
            float e = (float)(10 * log10(e2 / KWS_HOP + 1));
            if (!init) { nf = e; init = 1; }
            int sp = e > nf + vad_db && e > 30;
            if (!sp) nf = e < nf ? 0.7f * nf + 0.3f * e : 0.98f * nf + 0.02f * e;
            if (!speaking) {
                if (npre == PRE) { memmove(pre, pre + KWS_HOP, sizeof(int16_t) * KWS_HOP * (PRE - 1)); npre--; }
                memcpy(pre + npre * KWS_HOP, fr, sizeof fr); npre++;
                run = sp ? run + 1 : 0;
                if (run >= 2) { speaking = 1; sil = 0; un = 0; memcpy(u, pre, sizeof(int16_t) * npre * KWS_HOP); un = npre * KWS_HOP; npre = 0; }
                continue;
            }
            if (un + KWS_HOP <= MAXU) { memcpy(u + un, fr, sizeof fr); un += KWS_HOP; }
            sil = sp ? 0 : sil + 20;
            if (sil >= 400 || un + KWS_HOP > MAXU) {
                speaking = 0; run = 0;
                int keep = un - (sil > 300 ? (sil - 300) * 16 : 0);
                if (keep < KWS_SR / 4 || keep > (int)(KWS_SR * 1.6)) {
                    fprintf(stderr, "  pominieto (%.2f s - za %s)\n", keep / 16000.0, keep < KWS_SR / 4 ? "krotkie" : "dlugie");
                    continue;
                }
                sprintf(path, "%s\\%s_%04d.wav", dir, prefix, idx++);
                wav_save(path, u, keep);
                saved++;
                fprintf(stderr, "  zapisano %s (%.2f s)\n", path, keep / 16000.0);
            }
        }
    }
    unsigned char off = 0;
    send_frame(s, AF_REC, &off, 1);
    closesocket(s);
    fprintf(stderr, "nagrano %d plikow\n", saved);
    return 0;
}

/* ================================================================ main */
int main(int argc, char **argv)
{
    signal(SIGINT, on_sigint);
    SetConsoleOutputCP(65001);
    if (argc < 2) {
        fprintf(stderr,
            "kwstool - slowo kluczowe dla ReSpeakera XVF3800\n"
            "  kwstool record <ip> <katalog> [-prefix punapu] [-n 50]  nagraj probki z urzadzenia\n"
            "  kwstool train <dataset> [-o model.kws] [-keyword punapu] [-epochs 40] [-t watki]\n"
            "  kwstool test <model.kws> <plik.wav> [-thr 0.8] [-v]     wykrywanie w pliku\n"
            "  kwstool upload <ip> <model.kws>                         wgraj model do urzadzenia\n"
            "  kwstool info <model.kws>\n");
        return 2;
    }
    const char *c = argv[1];
    if (!strcmp(c, "train")) return cmd_train(argc - 2, argv + 2);
    if (!strcmp(c, "test")) return cmd_test(argc - 2, argv + 2);
    if (!strcmp(c, "info")) return cmd_info(argc - 2, argv + 2);
    if (!strcmp(c, "upload")) return cmd_upload(argc - 2, argv + 2);
    if (!strcmp(c, "record")) return cmd_record(argc - 2, argv + 2);
    fprintf(stderr, "nieznane polecenie %s\n", c);
    return 2;
}
