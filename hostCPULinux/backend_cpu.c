/*
 * backend_cpu.c (Linux) - operacje Whispera na CPU + pula watkow pthreads.
 *   gcc -O3 -mavx2 -mfma : kernele AVX2 + FMA (cel: Intel Core i3-N305, 8 rdzeni Gracemont)
 *   bez -mavx2           : te same algorytmy w czystym C (bez SIMD)
 * Kopia hostCPU/backend_cpu.c (Windows) - kernele identyczne, inna pula watkow i CPUID.
 * Wagi Q8_0 (ggml) liczone na liczbach calkowitych jak w whisper.cpp: wejscie kwantyzowane
 * do Q8_0 w locie, iloczyn int8 x int8 w blokach po 32, skala float na blok.
 * AVX-VNNI (vpdpbusd, np. Gracemont w i3-N305) wybierane przy starcie przez CPUID;
 * zmienna srodowiska ASR_NOVNNI=1 wymusza sciezke AVX2 (porownania).
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../asr/backend.h"
#ifdef __AVX2__
#include <immintrin.h>
#include <cpuid.h>
#endif

#define MAXTHR 64
typedef void (*job_t)(void *ctx, int i0, int i1);

/* ---------------------------------------------------------------- pula watkow
 * Zlecenie = numer generacji s_gen. Watek najpierw kreci sie (krotkie operacje dekodera
 * nastepuja po sobie co kilka us), potem zasypia na zmiennej warunkowej. Kazdy watek
 * potwierdza kazda generacje w s_ack[i], wiec parametry zlecenia nie zmieniaja sie pod nim.
 * Praca przydzielana dynamicznie kawalkami (s_next) - wywlaszczony watek nie hamuje reszty. */
#define SPIN_WORKER 20000
#define SPIN_MAIN   2000

static int       s_nthr = 1;
static pthread_t s_th[MAXTHR];
static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cv = PTHREAD_COND_INITIALIZER;
static int       s_gen, s_ack[MAXTHR], s_sleepers, s_next;      /* dostep przez __atomic */
static int       s_quit;
static job_t     s_fn;
static void     *s_ctx;
static int       s_n, s_chunk;
static char      s_name[64];
#ifdef __AVX2__
static int       s_vnni;                  /* 1 = kernel Q8_0 na AVX-VNNI */
#endif

static void cpu_pause(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause");
#endif
}

static void run_share(void)
{
    for (;;) {
        int a = __atomic_fetch_add(&s_next, s_chunk, __ATOMIC_RELAXED);
        if (a >= s_n) break;
        int b = a + s_chunk < s_n ? a + s_chunk : s_n;
        s_fn(s_ctx, a, b);
    }
}

static void *worker(void *arg)
{
    int i = (int)(size_t)arg;
    int seen = 0;
    for (;;) {
        int spins = 0;
        while (__atomic_load_n(&s_gen, __ATOMIC_ACQUIRE) == seen) {
            if (++spins < SPIN_WORKER) { cpu_pause(); continue; }
            /* zasypiamy; licznik spiacych + gen (seq_cst) - pobudka nie zginie */
            pthread_mutex_lock(&s_mx);
            __atomic_add_fetch(&s_sleepers, 1, __ATOMIC_SEQ_CST);
            while (__atomic_load_n(&s_gen, __ATOMIC_SEQ_CST) == seen) pthread_cond_wait(&s_cv, &s_mx);
            __atomic_sub_fetch(&s_sleepers, 1, __ATOMIC_SEQ_CST);
            pthread_mutex_unlock(&s_mx);
            spins = 0;
        }
        seen = __atomic_load_n(&s_gen, __ATOMIC_ACQUIRE);
        if (s_quit) break;
        run_share();
        __atomic_store_n(&s_ack[i], seen, __ATOMIC_RELEASE);
    }
    return NULL;
}

static void wake_all(int g)
{
    __atomic_store_n(&s_gen, g, __ATOMIC_SEQ_CST);  /* parametry zapisane wczesniej */
    if (__atomic_load_n(&s_sleepers, __ATOMIC_SEQ_CST) > 0) {
        pthread_mutex_lock(&s_mx);
        pthread_cond_broadcast(&s_cv);
        pthread_mutex_unlock(&s_mx);
    }
}

/* wykonaj fn na [0,n) rownolegle; min_per - ponizej tylu elementow na watek liczymy sami */
static void pool_run(job_t fn, void *ctx, int n, int min_per)
{
    if (n <= 0) return;
    if (s_nthr <= 1 || n < 2 * min_per) { fn(ctx, 0, n); return; }
    int act = n / min_per < s_nthr ? n / min_per : s_nthr;
    s_fn = fn; s_ctx = ctx; s_n = n;
    s_chunk = n / (act * 4);
    if (s_chunk < min_per) s_chunk = min_per;
    __atomic_store_n(&s_next, 0, __ATOMIC_RELAXED);
    int g = s_gen + 1;
    wake_all(g);
    run_share();
    for (int i = 1; i < s_nthr; i++) {
        int spins = 0;
        while (__atomic_load_n(&s_ack[i], __ATOMIC_ACQUIRE) != g) {
            if (++spins < SPIN_MAIN) cpu_pause();
            else { sched_yield(); spins = 0; }
        }
    }
}

#ifdef __AVX2__
static int cpu_has_avx2(void)
{
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return 0;
    int fma = (c >> 12) & 1, osx = (c >> 27) & 1, avx = (c >> 28) & 1;
    if (!fma || !osx || !avx) return 0;
    unsigned xlo, xhi;
    __asm__ __volatile__("xgetbv" : "=a"(xlo), "=d"(xhi) : "c"(0));
    if ((xlo & 6) != 6) return 0;                  /* system zapisuje rejestry ymm */
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return 0;
    return (b >> 5) & 1;
}

static int cpu_has_avx_vnni(void)
{
    unsigned a, b, c, d;
    if (!__get_cpuid_count(7, 1, &a, &b, &c, &d)) return 0;
    return (a >> 4) & 1;                           /* CPUID.(7,1):EAX[4] = AVX-VNNI */
}
#endif

int be_init(int nthreads)
{
#ifdef __AVX2__
    if (!cpu_has_avx2()) {
        fprintf(stderr, "procesor nie zglasza AVX2/FMA. Maszyna wirtualna Proxmox: ustaw typ CPU\n"
                        "\"host\" (Hardware -> Processors -> Type). Inaczej zbuduj bez SIMD: make SIMD=0\n");
        return 1;
    }
    {
        const char *nv = getenv("ASR_NOVNNI");
        s_vnni = cpu_has_avx_vnni() && !(nv && *nv && *nv != '0');
    }
#endif
    if (nthreads <= 0) nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (nthreads > MAXTHR) nthreads = MAXTHR;
    if (nthreads < 1) nthreads = 1;
    s_nthr = nthreads;
    for (int i = 1; i < s_nthr; i++)
        if (pthread_create(&s_th[i], NULL, worker, (void *)(size_t)i) != 0) { s_nthr = i; break; }
#ifdef __AVX2__
    sprintf(s_name, "CPU AVX2%s, %d watkow", s_vnni ? " + AVX-VNNI" : "", s_nthr);
#else
    sprintf(s_name, "CPU, %d watkow", s_nthr);
#endif
    return 0;
}

const char *be_name(void) { return s_name; }

void be_shutdown(void)
{
    s_quit = 1;
    wake_all(s_gen + 1);
    for (int i = 1; i < s_nthr; i++) pthread_join(s_th[i], NULL);
    s_nthr = 1;
}

/* ---------------------------------------------------------------- pamiec */
/* bufory wyrownane do 64 B (linia cache); przed blokiem zapisany oryginalny wskaznik */
static void *amalloc(size_t bytes)
{
    char *p = (char *)malloc(bytes + 64 + sizeof(void *));
    if (!p) return NULL;
    char *a = (char *)(((size_t)p + sizeof(void *) + 63) & ~(size_t)63);
    ((void **)a)[-1] = p;
    return a;
}
static void afree(void *a) { if (a) free(((void **)a)[-1]); }

/* bufor roboczy rosnacy w miare potrzeb (operacje wywolywane po kolei) */
static void *scratch(void **buf, size_t *cap, size_t bytes)
{
    if (bytes > *cap) {
        afree(*buf);
        *buf = amalloc(bytes);
        *cap = *buf ? bytes : 0;
        if (!*buf) { fprintf(stderr, "brak pamieci (%u MB)\n", (unsigned)(bytes >> 20)); exit(1); }
    }
    return *buf;
}

#define F(d) ((float *)(size_t)(d))

dptr be_alloc(size_t n) { return (dptr)(size_t)amalloc(n * sizeof(float)); }
void be_free(dptr d) { afree(F(d)); }
void be_upload(dptr d, const float *h, size_t n) { memcpy(F(d), h, n * sizeof(float)); }
void be_download(float *h, dptr d, size_t n) { memcpy(h, F(d), n * sizeof(float)); }

/* ---------------------------------------------------------------- funkcje wektorowe */
#ifdef __AVX2__
static __inline float hsum8(__m256 v)
{
    __m128 a = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
    a = _mm_add_ps(a, _mm_movehl_ps(a, a));
    a = _mm_add_ss(a, _mm_movehdup_ps(a));
    return _mm_cvtss_f32(a);
}

/* sumy 4 wektorow -> [sum a, sum b, sum c, sum d] */
static __inline __m128 hsum4x8(__m256 a, __m256 b, __m256 c, __m256 d)
{
    __m256 ab = _mm256_hadd_ps(a, b), cd = _mm256_hadd_ps(c, d);
    __m256 s = _mm256_hadd_ps(ab, cd);
    return _mm_add_ps(_mm256_castps256_ps128(s), _mm256_extractf128_ps(s, 1));
}

/* e^x, wielomian jak w Cephes (blad ~1e-7 wzgl.); x < -88 daje 0 */
static __inline __m256 exp8(__m256 x)
{
    x = _mm256_min_ps(x, _mm256_set1_ps(88.3762626647949f));
    x = _mm256_max_ps(x, _mm256_set1_ps(-88.3762626647949f));
    __m256 fx = _mm256_fmadd_ps(x, _mm256_set1_ps(1.44269504088896341f), _mm256_set1_ps(0.5f));
    fx = _mm256_floor_ps(fx);
    x = _mm256_fnmadd_ps(fx, _mm256_set1_ps(0.693359375f), x);
    x = _mm256_fnmadd_ps(fx, _mm256_set1_ps(-2.12194440e-4f), x);
    __m256 y = _mm256_set1_ps(1.9875691500E-4f);
    y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(1.3981999507E-3f));
    y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(8.3334519073E-3f));
    y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(4.1665795894E-2f));
    y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(1.6666665459E-1f));
    y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(5.0000001201E-1f));
    y = _mm256_fmadd_ps(y, _mm256_mul_ps(x, x), _mm256_add_ps(x, _mm256_set1_ps(1.0f)));
    __m256i e = _mm256_slli_epi32(_mm256_add_epi32(_mm256_cvttps_epi32(fx), _mm256_set1_epi32(127)), 23);
    return _mm256_mul_ps(y, _mm256_castsi256_ps(e));
}

/* GELU (tanh): 0.5 v (1 + tanh(u)) = v / (1 + e^(-2u)),  u = 0.79788456 (v + 0.044715 v^3) */
static __inline __m256 gelu8(__m256 v)
{
    __m256 v2 = _mm256_mul_ps(v, v);
    __m256 u = _mm256_mul_ps(v, _mm256_fmadd_ps(v2, _mm256_set1_ps(0.044715f * 0.7978845608f),
                                                _mm256_set1_ps(0.7978845608f)));
    __m256 e = exp8(_mm256_mul_ps(u, _mm256_set1_ps(-2.0f)));
    return _mm256_div_ps(v, _mm256_add_ps(e, _mm256_set1_ps(1.0f)));
}
#endif

static float gelu1(float v)
{
    return 0.5f * v * (1.0f + (float)tanh(0.7978845608 * (v + 0.044715 * v * v * v)));
}

static void gelu_row(float *x, int n)
{
    int i = 0;
#ifdef __AVX2__
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(x + i, gelu8(_mm256_loadu_ps(x + i)));
#endif
    for (; i < n; i++) x[i] = gelu1(x[i]);
}

/* ---------------------------------------------------------------- linear float32
 * y[M,N] = x[M,K] * W[N,K]^T + b. Watek dostaje grupy po 4 wiersze W (= 4 wyjscia).
 * Kernel 2 wiersze x  x  4 wiersze W liczy iloczyny skalarne wprost (8 sum czesciowych),
 * x czytane blokami po MB wierszy, zeby siedzialy w L2. */
typedef struct { float *y; const float *x, *W, *b; int M, K, N; } lin_t;

static int rows_block(size_t rowbytes)
{
    int mb = (int)(196608 / (rowbytes ? rowbytes : 1));
    if (mb < 4) mb = 4;
    if (mb > 256) mb = 256;
    return mb & ~1;
}

static float dotf(const float *a, const float *b, int K)
{
    int k = 0;
    float s = 0;
#ifdef __AVX2__
    __m256 s0 = _mm256_setzero_ps(), s1 = _mm256_setzero_ps();
    for (; k + 16 <= K; k += 16) {
        s0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + k), _mm256_loadu_ps(b + k), s0);
        s1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + k + 8), _mm256_loadu_ps(b + k + 8), s1);
    }
    for (; k + 8 <= K; k += 8) s0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + k), _mm256_loadu_ps(b + k), s0);
    s = hsum8(_mm256_add_ps(s0, s1));
#endif
    for (; k < K; k++) s += a[k] * b[k];
    return s;
}

/* r[i*4 + j] = x_i . w_j,  i < nx (1|2), j < 4 */
static void kern_f32(const float *x0, const float *x1, int nx, const float *w0, int K, float *r)
{
    const float *w1 = w0 + K, *w2 = w1 + K, *w3 = w2 + K;
    int k = 0;
#ifdef __AVX2__
    __m256 a0 = _mm256_setzero_ps(), a1 = a0, a2 = a0, a3 = a0;
    __m256 c0 = a0, c1 = a0, c2 = a0, c3 = a0;
    if (nx == 2) {
        for (; k + 8 <= K; k += 8) {
            __m256 u = _mm256_loadu_ps(x0 + k), v = _mm256_loadu_ps(x1 + k), w;
            w = _mm256_loadu_ps(w0 + k); a0 = _mm256_fmadd_ps(u, w, a0); c0 = _mm256_fmadd_ps(v, w, c0);
            w = _mm256_loadu_ps(w1 + k); a1 = _mm256_fmadd_ps(u, w, a1); c1 = _mm256_fmadd_ps(v, w, c1);
            w = _mm256_loadu_ps(w2 + k); a2 = _mm256_fmadd_ps(u, w, a2); c2 = _mm256_fmadd_ps(v, w, c2);
            w = _mm256_loadu_ps(w3 + k); a3 = _mm256_fmadd_ps(u, w, a3); c3 = _mm256_fmadd_ps(v, w, c3);
        }
    } else {
        for (; k + 16 <= K; k += 16) {             /* 8 niezaleznych sum - ukryte opoznienie FMA */
            __m256 u = _mm256_loadu_ps(x0 + k), v = _mm256_loadu_ps(x0 + k + 8);
            a0 = _mm256_fmadd_ps(u, _mm256_loadu_ps(w0 + k), a0); c0 = _mm256_fmadd_ps(v, _mm256_loadu_ps(w0 + k + 8), c0);
            a1 = _mm256_fmadd_ps(u, _mm256_loadu_ps(w1 + k), a1); c1 = _mm256_fmadd_ps(v, _mm256_loadu_ps(w1 + k + 8), c1);
            a2 = _mm256_fmadd_ps(u, _mm256_loadu_ps(w2 + k), a2); c2 = _mm256_fmadd_ps(v, _mm256_loadu_ps(w2 + k + 8), c2);
            a3 = _mm256_fmadd_ps(u, _mm256_loadu_ps(w3 + k), a3); c3 = _mm256_fmadd_ps(v, _mm256_loadu_ps(w3 + k + 8), c3);
        }
        a0 = _mm256_add_ps(a0, c0); a1 = _mm256_add_ps(a1, c1); a2 = _mm256_add_ps(a2, c2); a3 = _mm256_add_ps(a3, c3);
        c0 = c1 = c2 = c3 = _mm256_setzero_ps();
        for (; k + 8 <= K; k += 8) {
            __m256 u = _mm256_loadu_ps(x0 + k);
            a0 = _mm256_fmadd_ps(u, _mm256_loadu_ps(w0 + k), a0);
            a1 = _mm256_fmadd_ps(u, _mm256_loadu_ps(w1 + k), a1);
            a2 = _mm256_fmadd_ps(u, _mm256_loadu_ps(w2 + k), a2);
            a3 = _mm256_fmadd_ps(u, _mm256_loadu_ps(w3 + k), a3);
        }
    }
    _mm_storeu_ps(r, hsum4x8(a0, a1, a2, a3));
    _mm_storeu_ps(r + 4, hsum4x8(c0, c1, c2, c3));
#else
    for (int i = 0; i < 8; i++) r[i] = 0;
#endif
    for (int i = 0; i < nx; i++) {
        const float *xr = i ? x1 : x0;
        float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        for (int kk = k; kk < K; kk++) {
            float xv = xr[kk];
            s0 += xv * w0[kk]; s1 += xv * w1[kk]; s2 += xv * w2[kk]; s3 += xv * w3[kk];
        }
        r[i * 4] += s0; r[i * 4 + 1] += s1; r[i * 4 + 2] += s2; r[i * 4 + 3] += s3;
    }
}

static void lin_job(void *c, int g0, int g1)
{
    lin_t *a = (lin_t *)c;
    int M = a->M, K = a->K, N = a->N;
    int n0 = g0 * 4, n1 = g1 * 4 < N ? g1 * 4 : N;
    int MB = rows_block((size_t)K * 4);
    for (int m0 = 0; m0 < M; m0 += MB) {
        int m1 = m0 + MB < M ? m0 + MB : M;
        for (int n = n0; n < n1; n += 4) {
            const float *w0 = a->W + (size_t)n * K;
            if (n + 4 <= n1) {
                float bb[4];
                for (int j = 0; j < 4; j++) bb[j] = a->b ? a->b[n + j] : 0;
                for (int m = m0; m < m1; m += 2) {
                    int nx = m + 1 < m1 ? 2 : 1;
                    float r[8];
                    kern_f32(a->x + (size_t)m * K, a->x + (size_t)(m + nx - 1) * K, nx, w0, K, r);
                    for (int i = 0; i < nx; i++) {
                        float *yr = a->y + (size_t)(m + i) * N + n;
                        yr[0] = r[i * 4] + bb[0]; yr[1] = r[i * 4 + 1] + bb[1];
                        yr[2] = r[i * 4 + 2] + bb[2]; yr[3] = r[i * 4 + 3] + bb[3];
                    }
                }
            } else {
                for (int j = n; j < n1; j++)
                    for (int m = m0; m < m1; m++)
                        a->y[(size_t)m * N + j] = dotf(a->x + (size_t)m * K, a->W + (size_t)j * K, K) + (a->b ? a->b[j] : 0);
            }
        }
    }
}

static void linear_f32(float *y, const float *x, const float *W, const float *b, int M, int K, int N)
{
    lin_t a;
    a.y = y; a.x = x; a.W = W; a.b = b; a.M = M; a.K = K; a.N = N;
    pool_run(lin_job, &a, (N + 3) / 4, 4);
}

void be_linear(dptr y, dptr x, dptr W, dptr b, int M, int K, int N)
{
    linear_f32(F(y), F(x), F(W), b ? F(b) : NULL, M, K, N);
}

/* ---------------------------------------------------------------- linear Q8_0
 * Uklad wiersza (K wag): K bajtow int8, potem K/32 skal float. Tak samo kwantyzowane
 * wejscie x. Iloczyn bloku: sum |x| * (w * sign(x)) przez maddubs (bez przepelnienia:
 * |q| <= 127), sumy int32 -> float * (dx * dw). */
#define Q8_ROW(K) ((size_t)(K) + (size_t)(K) / 8)

static void q8_quant_row(const float *x, signed char *q, float *d, int K)
{
    for (int b = 0; b < K / 32; b++, x += 32, q += 32) {
#ifdef __AVX2__
        __m256 v0 = _mm256_loadu_ps(x), v1 = _mm256_loadu_ps(x + 8);
        __m256 v2 = _mm256_loadu_ps(x + 16), v3 = _mm256_loadu_ps(x + 24);
        __m256 sm = _mm256_set1_ps(-0.0f);
        __m256 mx = _mm256_max_ps(_mm256_max_ps(_mm256_andnot_ps(sm, v0), _mm256_andnot_ps(sm, v1)),
                                  _mm256_max_ps(_mm256_andnot_ps(sm, v2), _mm256_andnot_ps(sm, v3)));
        __m128 m4 = _mm_max_ps(_mm256_castps256_ps128(mx), _mm256_extractf128_ps(mx, 1));
        m4 = _mm_max_ps(m4, _mm_movehl_ps(m4, m4));
        m4 = _mm_max_ss(m4, _mm_movehdup_ps(m4));
        float amax = _mm_cvtss_f32(m4);
        d[b] = amax / 127.0f;
        __m256 id = _mm256_set1_ps(amax > 0 ? 127.0f / amax : 0.0f);
        __m256i i0 = _mm256_cvtps_epi32(_mm256_mul_ps(v0, id)), i1 = _mm256_cvtps_epi32(_mm256_mul_ps(v1, id));
        __m256i i2 = _mm256_cvtps_epi32(_mm256_mul_ps(v2, id)), i3 = _mm256_cvtps_epi32(_mm256_mul_ps(v3, id));
        i0 = _mm256_packs_epi32(i0, i1);
        i2 = _mm256_packs_epi32(i2, i3);
        i0 = _mm256_packs_epi16(i0, i2);
        i0 = _mm256_permutevar8x32_epi32(i0, _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7));
        _mm256_storeu_si256((__m256i *)q, i0);
#else
        float amax = 0;
        for (int j = 0; j < 32; j++) { float a = fabsf(x[j]); if (a > amax) amax = a; }
        d[b] = amax / 127.0f;
        float id = amax > 0 ? 127.0f / amax : 0.0f;
        for (int j = 0; j < 32; j++) {
            float v = x[j] * id;
            q[j] = (signed char)(v >= 0 ? (int)(v + 0.5f) : -(int)(-v + 0.5f));
        }
#endif
    }
}

typedef struct { const float *x; char *q; int K; } qx_t;

static void quant_job(void *c, int m0, int m1)
{
    qx_t *a = (qx_t *)c;
    size_t rs = Q8_ROW(a->K);
    for (int m = m0; m < m1; m++) {
        signed char *q = (signed char *)a->q + (size_t)m * rs;
        q8_quant_row(a->x + (size_t)m * a->K, q, (float *)(q + a->K), a->K);
    }
}

dptr be_upload_q8(const void *blocks, int nrows, int K)
{
    if (K % 32) return 0;
    size_t rs = Q8_ROW(K);
    char *buf = (char *)amalloc(rs * nrows);
    if (!buf) return 0;
    const unsigned char *bl = (const unsigned char *)blocks;
    for (int r = 0; r < nrows; r++) {
        signed char *q = (signed char *)buf + (size_t)r * rs;
        float *d = (float *)(q + K);
        for (int b = 0; b < K / 32; b++, bl += 34) {
            /* fp16 -> float */
            unsigned h = bl[0] | bl[1] << 8;
            unsigned s = (h >> 15) << 31, e = (h >> 10) & 0x1f, m = h & 0x3ff, f;
            if (e == 0) {
                if (m == 0) f = s;
                else { e = 1; while (!(m & 0x400)) { m <<= 1; e--; } m &= 0x3ff; f = s | ((e + 112) << 23) | (m << 13); }
            } else if (e == 31) f = s | 0x7f800000u | (m << 13);
            else f = s | ((e + 112) << 23) | (m << 13);
            memcpy(&d[b], &f, 4);
            memcpy(q + b * 32, bl + 2, 32);
        }
    }
    return (dptr)(size_t)buf;
}

#define FORCE_INLINE inline __attribute__((always_inline))

#ifdef __AVX2__
/* iloczyn blokow int8 (|x|, w*sign(x)) -> 8 x int32 jako float.
 * AVX2: maddubs (int16) + madd (int32);  AVX-VNNI: jedno vpdpbusd prosto do int32 */
static FORCE_INLINE __m256 dot_blk(__m256i ax, __m256i sx, const signed char *w, int vnni)
{
    __m256i sw = _mm256_sign_epi8(_mm256_loadu_si256((const __m256i *)w), sx);
    if (vnni) {
        /* vpdpbusd przez asm: intrinsic wymagalby target("avxvnni") na calym lancuchu funkcji;
         * {vex} = kodowanie AVX-VNNI (nie AVX512-VNNI). AT&T: zrodlo_s8, zrodlo_u8, akumulator */
        __m256i acc = _mm256_setzero_si256();
        __asm__("%{vex%} vpdpbusd %2, %1, %0" : "+x"(acc) : "x"(ax), "x"(sw));
        return _mm256_cvtepi32_ps(acc);
    }
    __m256i p = _mm256_maddubs_epi16(ax, sw);
    return _mm256_cvtepi32_ps(_mm256_madd_epi16(p, _mm256_set1_epi16(1)));
}
#endif

/* r[i*4 + j] = x_i . w_j  (Q8_0), i < nx (1|2), j < 4; wiersze W co rs bajtow.
 * vnni jest stala w obu wersjach ponizej - kompilator usuwa rozgalezienie. */
static FORCE_INLINE void kern_q8_impl(const signed char *x0, const signed char *x1, int nx,
                                      const signed char *w0, size_t rs, int K, float *r, int vnni)
{
    const float *dx0 = (const float *)(x0 + K), *dx1 = (const float *)(x1 + K);
    const signed char *w1 = w0 + rs, *w2 = w1 + rs, *w3 = w2 + rs;
    const float *d0 = (const float *)(w0 + K), *d1 = (const float *)(w1 + K);
    const float *d2 = (const float *)(w2 + K), *d3 = (const float *)(w3 + K);
    int nb = K / 32;
#ifdef __AVX2__
    __m256 a0 = _mm256_setzero_ps(), a1 = a0, a2 = a0, a3 = a0;
    __m256 c0 = a0, c1 = a0, c2 = a0, c3 = a0;
    if (nx == 2) {
        for (int b = 0; b < nb; b++) {
            int o = b * 32;
            __m256i u = _mm256_loadu_si256((const __m256i *)(x0 + o));
            __m256i v = _mm256_loadu_si256((const __m256i *)(x1 + o));
            __m256i au = _mm256_sign_epi8(u, u), av = _mm256_sign_epi8(v, v);
            float su = dx0[b], sv = dx1[b];
            a0 = _mm256_fmadd_ps(dot_blk(au, u, w0 + o, vnni), _mm256_set1_ps(su * d0[b]), a0);
            c0 = _mm256_fmadd_ps(dot_blk(av, v, w0 + o, vnni), _mm256_set1_ps(sv * d0[b]), c0);
            a1 = _mm256_fmadd_ps(dot_blk(au, u, w1 + o, vnni), _mm256_set1_ps(su * d1[b]), a1);
            c1 = _mm256_fmadd_ps(dot_blk(av, v, w1 + o, vnni), _mm256_set1_ps(sv * d1[b]), c1);
            a2 = _mm256_fmadd_ps(dot_blk(au, u, w2 + o, vnni), _mm256_set1_ps(su * d2[b]), a2);
            c2 = _mm256_fmadd_ps(dot_blk(av, v, w2 + o, vnni), _mm256_set1_ps(sv * d2[b]), c2);
            a3 = _mm256_fmadd_ps(dot_blk(au, u, w3 + o, vnni), _mm256_set1_ps(su * d3[b]), a3);
            c3 = _mm256_fmadd_ps(dot_blk(av, v, w3 + o, vnni), _mm256_set1_ps(sv * d3[b]), c3);
        }
    } else {
        int b = 0;
        for (; b + 2 <= nb; b += 2) {              /* dwa bloki naraz - wiecej niezaleznych sum */
            int o = b * 32;
            __m256i u = _mm256_loadu_si256((const __m256i *)(x0 + o));
            __m256i v = _mm256_loadu_si256((const __m256i *)(x0 + o + 32));
            __m256i au = _mm256_sign_epi8(u, u), av = _mm256_sign_epi8(v, v);
            float su = dx0[b], sv = dx0[b + 1];
            a0 = _mm256_fmadd_ps(dot_blk(au, u, w0 + o, vnni), _mm256_set1_ps(su * d0[b]), a0);
            c0 = _mm256_fmadd_ps(dot_blk(av, v, w0 + o + 32, vnni), _mm256_set1_ps(sv * d0[b + 1]), c0);
            a1 = _mm256_fmadd_ps(dot_blk(au, u, w1 + o, vnni), _mm256_set1_ps(su * d1[b]), a1);
            c1 = _mm256_fmadd_ps(dot_blk(av, v, w1 + o + 32, vnni), _mm256_set1_ps(sv * d1[b + 1]), c1);
            a2 = _mm256_fmadd_ps(dot_blk(au, u, w2 + o, vnni), _mm256_set1_ps(su * d2[b]), a2);
            c2 = _mm256_fmadd_ps(dot_blk(av, v, w2 + o + 32, vnni), _mm256_set1_ps(sv * d2[b + 1]), c2);
            a3 = _mm256_fmadd_ps(dot_blk(au, u, w3 + o, vnni), _mm256_set1_ps(su * d3[b]), a3);
            c3 = _mm256_fmadd_ps(dot_blk(av, v, w3 + o + 32, vnni), _mm256_set1_ps(sv * d3[b + 1]), c3);
        }
        for (; b < nb; b++) {
            int o = b * 32;
            __m256i u = _mm256_loadu_si256((const __m256i *)(x0 + o));
            __m256i au = _mm256_sign_epi8(u, u);
            float su = dx0[b];
            a0 = _mm256_fmadd_ps(dot_blk(au, u, w0 + o, vnni), _mm256_set1_ps(su * d0[b]), a0);
            a1 = _mm256_fmadd_ps(dot_blk(au, u, w1 + o, vnni), _mm256_set1_ps(su * d1[b]), a1);
            a2 = _mm256_fmadd_ps(dot_blk(au, u, w2 + o, vnni), _mm256_set1_ps(su * d2[b]), a2);
            a3 = _mm256_fmadd_ps(dot_blk(au, u, w3 + o, vnni), _mm256_set1_ps(su * d3[b]), a3);
        }
        a0 = _mm256_add_ps(a0, c0); a1 = _mm256_add_ps(a1, c1); a2 = _mm256_add_ps(a2, c2); a3 = _mm256_add_ps(a3, c3);
        c0 = c1 = c2 = c3 = _mm256_setzero_ps();
    }
    _mm_storeu_ps(r, hsum4x8(a0, a1, a2, a3));
    _mm_storeu_ps(r + 4, hsum4x8(c0, c1, c2, c3));
#else
    const signed char *ws[4]; const float *ds[4];
    (void)vnni;
    ws[0] = w0; ws[1] = w1; ws[2] = w2; ws[3] = w3;
    ds[0] = d0; ds[1] = d1; ds[2] = d2; ds[3] = d3;
    for (int i = 0; i < 8; i++) r[i] = 0;
    for (int i = 0; i < nx; i++) {
        const signed char *xq = i ? x1 : x0;
        const float *dx = i ? dx1 : dx0;
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int b = 0; b < nb; b++) {
                int acc = 0;
                const signed char *p = xq + b * 32, *q = ws[j] + b * 32;
                for (int t = 0; t < 32; t++) acc += p[t] * q[t];
                s += (float)acc * dx[b] * ds[j][b];
            }
            r[i * 4 + j] = s;
        }
    }
#endif
}

typedef void (*kern_q8_t)(const signed char *, const signed char *, int, const signed char *, size_t, int, float *);
static void kern_q8_base(const signed char *x0, const signed char *x1, int nx, const signed char *w0,
                         size_t rs, int K, float *r)
{
    kern_q8_impl(x0, x1, nx, w0, rs, K, r, 0);
}
#ifdef __AVX2__
static void kern_q8_vnni(const signed char *x0, const signed char *x1, int nx, const signed char *w0,
                         size_t rs, int K, float *r)
{
    kern_q8_impl(x0, x1, nx, w0, rs, K, r, 1);
}
#endif

typedef struct { float *y; const char *xq, *W; const float *b; int M, K, N; } linq_t;

static void linq_job(void *c, int g0, int g1)
{
    linq_t *a = (linq_t *)c;
    int M = a->M, K = a->K, N = a->N;
    size_t rs = Q8_ROW(K);
    int n0 = g0 * 4, n1 = g1 * 4 < N ? g1 * 4 : N;
    int MB = rows_block(rs);
    kern_q8_t kern_q8 = kern_q8_base;
#ifdef __AVX2__
    if (s_vnni) kern_q8 = kern_q8_vnni;
#endif
    for (int m0 = 0; m0 < M; m0 += MB) {
        int m1 = m0 + MB < M ? m0 + MB : M;
        for (int n = n0; n < n1; n += 4) {
            int nn = n1 - n < 4 ? n1 - n : 4;
            const signed char *w0 = (const signed char *)a->W + (size_t)n * rs;
            float bb[4];
            for (int j = 0; j < nn; j++) bb[j] = a->b ? a->b[n + j] : 0;
            for (int m = m0; m < m1; m += 2) {
                int nx = m + 1 < m1 ? 2 : 1;
                const signed char *x0 = (const signed char *)a->xq + (size_t)m * rs;
                float r[8];
                if (nn == 4) kern_q8(x0, x0 + (nx - 1) * rs, nx, w0, rs, K, r);
                else {
                    /* ostatnia niepelna grupa: kernel z tym samym wierszem W 4 razy (rs = 0) */
                    for (int i = 0; i < nx; i++)
                        for (int j = 0; j < nn; j++) {
                            float rr[8];
                            const signed char *xi = x0 + i * rs;
                            kern_q8(xi, xi, 1, w0 + (size_t)j * rs, 0, K, rr);
                            r[i * 4 + j] = rr[0];
                        }
                }
                for (int i = 0; i < nx; i++) {
                    float *yr = a->y + (size_t)(m + i) * N + n;
                    for (int j = 0; j < nn; j++) yr[j] = r[i * 4 + j] + bb[j];
                }
            }
        }
    }
}

static void *s_qx; static size_t s_qxcap;

void be_linear_q8(dptr y, dptr x, dptr W, dptr b, int M, int K, int N)
{
    size_t rs = Q8_ROW(K);
    qx_t q;
    q.x = F(x); q.K = K;
    q.q = (char *)scratch(&s_qx, &s_qxcap, rs * M);
    pool_run(quant_job, &q, M, 16);
    linq_t a;
    a.y = F(y); a.xq = q.q; a.W = (const char *)(size_t)W; a.b = b ? F(b) : NULL;
    a.M = M; a.K = K; a.N = N;
    pool_run(linq_job, &a, (N + 3) / 4, 4);
}

void be_embed_q8(dptr y, dptr tok, dptr pos, int token, int p, int N)
{
    float *Y = F(y);
    const signed char *q = (const signed char *)(size_t)tok + (size_t)token * Q8_ROW(N);
    const float *d = (const float *)(q + N), *P = F(pos) + (size_t)p * N;
    for (int i = 0; i < N; i++) Y[i] = d[i / 32] * q[i] + P[i];
}

/* ---------------------------------------------------------------- layernorm */
typedef struct { float *y; const float *x, *g, *b; int N; } ln_t;

static void ln_job(void *c, int m0, int m1)
{
    ln_t *a = (ln_t *)c;
    int N = a->N;
    for (int m = m0; m < m1; m++) {
        const float *xr = a->x + (size_t)m * N;
        float *yr = a->y + (size_t)m * N;
        int i = 0;
        double mean = 0, var = 0;
#ifdef __AVX2__
        __m256 s = _mm256_setzero_ps();
        for (; i + 8 <= N; i += 8) s = _mm256_add_ps(s, _mm256_loadu_ps(xr + i));
        mean = hsum8(s);
#endif
        for (; i < N; i++) mean += xr[i];
        mean /= N;
        float mu = (float)mean;
        i = 0;
#ifdef __AVX2__
        __m256 vm = _mm256_set1_ps(mu), sv = _mm256_setzero_ps();
        for (; i + 8 <= N; i += 8) { __m256 d = _mm256_sub_ps(_mm256_loadu_ps(xr + i), vm); sv = _mm256_fmadd_ps(d, d, sv); }
        var = hsum8(sv);
#endif
        for (; i < N; i++) { double d = xr[i] - mean; var += d * d; }
        var /= N;
        float inv = (float)(1.0 / sqrt(var + 1e-5));
        i = 0;
#ifdef __AVX2__
        __m256 vi = _mm256_set1_ps(inv);
        for (; i + 8 <= N; i += 8) {
            __m256 v = _mm256_mul_ps(_mm256_sub_ps(_mm256_loadu_ps(xr + i), vm), vi);
            _mm256_storeu_ps(yr + i, _mm256_fmadd_ps(v, _mm256_loadu_ps(a->g + i), _mm256_loadu_ps(a->b + i)));
        }
#endif
        for (; i < N; i++) yr[i] = (xr[i] - mu) * inv * a->g[i] + a->b[i];
    }
}

void be_layernorm(dptr y, dptr x, dptr g, dptr b, int M, int N)
{
    ln_t a;
    a.y = F(y); a.x = F(x); a.g = F(g); a.b = F(b); a.N = N;
    pool_run(ln_job, &a, M, 8);
}

/* ---------------------------------------------------------------- gelu, add */
typedef struct { float *y; const float *a; } ew_t;

static void gelu_job(void *c, int i0, int i1)
{
    gelu_row(((ew_t *)c)->y + i0, i1 - i0);
}
void be_gelu(dptr x, size_t n)
{
    ew_t a; a.y = F(x); a.a = NULL;
    pool_run(gelu_job, &a, (int)n, 4096);
}

static void add_job(void *c, int i0, int i1)
{
    ew_t *e = (ew_t *)c;
    int i = i0;
#ifdef __AVX2__
    for (; i + 8 <= i1; i += 8)
        _mm256_storeu_ps(e->y + i, _mm256_add_ps(_mm256_loadu_ps(e->y + i), _mm256_loadu_ps(e->a + i)));
#endif
    for (; i < i1; i++) e->y[i] += e->a[i];
}
void be_add(dptr y, dptr a, size_t n)
{
    ew_t e; e.y = F(y); e.a = F(a);
    pool_run(add_job, &e, (int)n, 16384);
}

/* ---------------------------------------------------------------- conv1d
 * im2col: col[t][ci*3 + k] = x[ci][t*stride - 1 + k]  ->  tmp[t][o] = col * W^T + b
 * (ten sam linear float32), potem GELU i transpozycja do y[o][t]. */
typedef struct { float *col, *tmp, *y; const float *x; int Cin, Cout, L, Lout, stride; } conv_t;

static void im2col_job(void *c, int t0, int t1)
{
    conv_t *a = (conv_t *)c;
    int K3 = 3 * a->Cin, L = a->L, st = a->stride;
    for (int ci = 0; ci < a->Cin; ci++) {
        const float *xr = a->x + (size_t)ci * L;
        for (int t = t0; t < t1; t++) {
            float *cr = a->col + (size_t)t * K3 + ci * 3;
            int s = t * st - 1;                          /* pad = 1 */
            cr[0] = s >= 0 ? xr[s] : 0.0f;
            cr[1] = xr[s + 1];
            cr[2] = s + 2 < L ? xr[s + 2] : 0.0f;
        }
    }
}

static void conv_out_job(void *c, int o0, int o1)
{
    conv_t *a = (conv_t *)c;
    int Lout = a->Lout, Cout = a->Cout;
    for (int o = o0; o < o1; o++) {
        float *yr = a->y + (size_t)o * Lout;
        for (int t = 0; t < Lout; t++) yr[t] = a->tmp[(size_t)t * Cout + o];
        gelu_row(yr, Lout);
    }
}

static void *s_col, *s_tmp; static size_t s_colcap, s_tmpcap;

void be_conv1d_gelu(dptr y, dptr x, dptr W, dptr b, int Cin, int Cout, int L, int stride)
{
    conv_t a;
    a.y = F(y); a.x = F(x);
    a.Cin = Cin; a.Cout = Cout; a.L = L; a.stride = stride; a.Lout = (L + 2 - 3) / stride + 1;
    a.col = (float *)scratch(&s_col, &s_colcap, (size_t)a.Lout * 3 * Cin * sizeof(float));
    a.tmp = (float *)scratch(&s_tmp, &s_tmpcap, (size_t)a.Lout * Cout * sizeof(float));
    pool_run(im2col_job, &a, a.Lout, 16);
    linear_f32(a.tmp, a.col, F(W), F(b), a.Lout, 3 * Cin, Cout);
    pool_run(conv_out_job, &a, Cout, 4);
}

void be_transpose_addpos(dptr y, dptr x, dptr pos, int C, int T)
{
    float *Y = F(y), *X = F(x), *P = F(pos);
    for (int t = 0; t < T; t++)
        for (int c = 0; c < C; c++)
            Y[(size_t)t * C + c] = X[(size_t)c * T + t] + P[(size_t)t * C + c];
}

/* ---------------------------------------------------------------- attention
 * Element pracy = (glowa h, para zapytan). Wyniki dla 2 zapytan x 4 kluczy naraz,
 * softmax z wektorowym exp, potem suma wazona V dla obu zapytan. */
typedef struct { float *out; const float *q, *k, *v; int nq, nk, nh, dh; } att_t;

#ifdef __AVX2__
static void att_pair(att_t *a, int h, int i0, int i1, float *s0, float *s1)
{
    int ld = a->nh * a->dh, dh = a->dh, nk = a->nk;
    float scale = (float)(1.0 / sqrt((double)dh));
    const float *q0 = a->q + (size_t)i0 * ld + h * dh, *q1 = a->q + (size_t)i1 * ld + h * dh;
    const float *kb = a->k + h * dh, *vb = a->v + h * dh;
    int j = 0;
    for (; j + 4 <= nk; j += 4) {
        const float *k0 = kb + (size_t)j * ld, *k1 = k0 + ld, *k2 = k1 + ld, *k3 = k2 + ld;
        __m256 a0 = _mm256_setzero_ps(), a1 = a0, a2 = a0, a3 = a0, c0 = a0, c1 = a0, c2 = a0, c3 = a0;
        for (int d = 0; d < dh; d += 8) {
            __m256 u = _mm256_loadu_ps(q0 + d), v = _mm256_loadu_ps(q1 + d), w;
            w = _mm256_loadu_ps(k0 + d); a0 = _mm256_fmadd_ps(u, w, a0); c0 = _mm256_fmadd_ps(v, w, c0);
            w = _mm256_loadu_ps(k1 + d); a1 = _mm256_fmadd_ps(u, w, a1); c1 = _mm256_fmadd_ps(v, w, c1);
            w = _mm256_loadu_ps(k2 + d); a2 = _mm256_fmadd_ps(u, w, a2); c2 = _mm256_fmadd_ps(v, w, c2);
            w = _mm256_loadu_ps(k3 + d); a3 = _mm256_fmadd_ps(u, w, a3); c3 = _mm256_fmadd_ps(v, w, c3);
        }
        __m128 sc = _mm_set1_ps(scale);
        _mm_storeu_ps(s0 + j, _mm_mul_ps(hsum4x8(a0, a1, a2, a3), sc));
        _mm_storeu_ps(s1 + j, _mm_mul_ps(hsum4x8(c0, c1, c2, c3), sc));
    }
    for (; j < nk; j++) {
        s0[j] = dotf(q0, kb + (size_t)j * ld, dh) * scale;
        s1[j] = dotf(q1, kb + (size_t)j * ld, dh) * scale;
    }

    /* softmax obu wierszy */
    float *ss[2];
    float inv[2];
    ss[0] = s0; ss[1] = s1;
    for (int r = 0; r < 2; r++) {
        float *s = ss[r], mx = -1e30f;
        __m256 vmx = _mm256_set1_ps(-1e30f);
        for (j = 0; j + 8 <= nk; j += 8) vmx = _mm256_max_ps(vmx, _mm256_loadu_ps(s + j));
        __m128 m4 = _mm_max_ps(_mm256_castps256_ps128(vmx), _mm256_extractf128_ps(vmx, 1));
        m4 = _mm_max_ps(m4, _mm_movehl_ps(m4, m4));
        m4 = _mm_max_ss(m4, _mm_movehdup_ps(m4));
        mx = _mm_cvtss_f32(m4);
        for (; j < nk; j++) if (s[j] > mx) mx = s[j];
        __m256 vm = _mm256_set1_ps(mx), vs = _mm256_setzero_ps();
        for (j = 0; j + 8 <= nk; j += 8) {
            __m256 e = exp8(_mm256_sub_ps(_mm256_loadu_ps(s + j), vm));
            _mm256_storeu_ps(s + j, e);
            vs = _mm256_add_ps(vs, e);
        }
        float sum = hsum8(vs);
        for (; j < nk; j++) { s[j] = (float)exp(s[j] - mx); sum += s[j]; }
        inv[r] = 1.0f / sum;
    }

    /* out = P . V, kolumny po 32 (4 wektory) dla obu zapytan */
    float *o0 = a->out + (size_t)i0 * ld + h * dh, *o1 = a->out + (size_t)i1 * ld + h * dh;
    __m256 iv0 = _mm256_set1_ps(inv[0]), iv1 = _mm256_set1_ps(inv[1]);
    int d = 0;
    for (; d + 32 <= dh; d += 32) {
        __m256 a0 = _mm256_setzero_ps(), a1 = a0, a2 = a0, a3 = a0, c0 = a0, c1 = a0, c2 = a0, c3 = a0;
        for (j = 0; j < nk; j++) {
            const float *vj = vb + (size_t)j * ld + d;
            __m256 p = _mm256_broadcast_ss(s0 + j), r = _mm256_broadcast_ss(s1 + j), w;
            w = _mm256_loadu_ps(vj);      a0 = _mm256_fmadd_ps(p, w, a0); c0 = _mm256_fmadd_ps(r, w, c0);
            w = _mm256_loadu_ps(vj + 8);  a1 = _mm256_fmadd_ps(p, w, a1); c1 = _mm256_fmadd_ps(r, w, c1);
            w = _mm256_loadu_ps(vj + 16); a2 = _mm256_fmadd_ps(p, w, a2); c2 = _mm256_fmadd_ps(r, w, c2);
            w = _mm256_loadu_ps(vj + 24); a3 = _mm256_fmadd_ps(p, w, a3); c3 = _mm256_fmadd_ps(r, w, c3);
        }
        _mm256_storeu_ps(o0 + d, _mm256_mul_ps(a0, iv0));      _mm256_storeu_ps(o1 + d, _mm256_mul_ps(c0, iv1));
        _mm256_storeu_ps(o0 + d + 8, _mm256_mul_ps(a1, iv0));  _mm256_storeu_ps(o1 + d + 8, _mm256_mul_ps(c1, iv1));
        _mm256_storeu_ps(o0 + d + 16, _mm256_mul_ps(a2, iv0)); _mm256_storeu_ps(o1 + d + 16, _mm256_mul_ps(c2, iv1));
        _mm256_storeu_ps(o0 + d + 24, _mm256_mul_ps(a3, iv0)); _mm256_storeu_ps(o1 + d + 24, _mm256_mul_ps(c3, iv1));
    }
    for (; d < dh; d += 8) {
        __m256 a0 = _mm256_setzero_ps(), c0 = a0;
        for (j = 0; j < nk; j++) {
            __m256 w = _mm256_loadu_ps(vb + (size_t)j * ld + d);
            a0 = _mm256_fmadd_ps(_mm256_broadcast_ss(s0 + j), w, a0);
            c0 = _mm256_fmadd_ps(_mm256_broadcast_ss(s1 + j), w, c0);
        }
        _mm256_storeu_ps(o0 + d, _mm256_mul_ps(a0, iv0));
        _mm256_storeu_ps(o1 + d, _mm256_mul_ps(c0, iv1));
    }
}
#endif

static void att_one(att_t *a, int h, int i, float *sc)
{
    int ld = a->nh * a->dh, dh = a->dh, nk = a->nk;
    float scale = (float)(1.0 / sqrt((double)dh));
    const float *qi = a->q + (size_t)i * ld + h * dh;
    float mx = -1e30f;
    for (int j = 0; j < nk; j++) {
        const float *kj = a->k + (size_t)j * ld + h * dh;
        float s = 0;
        for (int d = 0; d < dh; d++) s += qi[d] * kj[d];
        s *= scale;
        sc[j] = s;
        if (s > mx) mx = s;
    }
    float sum = 0;
    for (int j = 0; j < nk; j++) { sc[j] = (float)exp(sc[j] - mx); sum += sc[j]; }
    float inv = 1.0f / sum;
    float *o = a->out + (size_t)i * ld + h * dh;
    for (int d = 0; d < dh; d++) o[d] = 0;
    for (int j = 0; j < nk; j++) {
        const float *vj = a->v + (size_t)j * ld + h * dh;
        float p = sc[j] * inv;
        for (int d = 0; d < dh; d++) o[d] += p * vj[d];
    }
}

static void att_job(void *c, int it0, int it1)
{
    att_t *a = (att_t *)c;
    int np = (a->nq + 1) / 2;
    float *sc = (float *)malloc(sizeof(float) * 2 * a->nk);
    for (int it = it0; it < it1; it++) {
        int h = it / np, i0 = (it % np) * 2;            /* kolejne elementy - ta sama glowa */
        int i1 = i0 + 1 < a->nq ? i0 + 1 : i0;
#ifdef __AVX2__
        if (a->dh % 8 == 0) {
            if (i1 != i0) att_pair(a, h, i0, i1, sc, sc + a->nk);
            else {
                /* pojedyncze zapytanie: liczone podwojnie, wynik ten sam */
                att_pair(a, h, i0, i0, sc, sc + a->nk);
            }
            continue;
        }
#endif
        att_one(a, h, i0, sc);
        if (i1 != i0) att_one(a, h, i1, sc);
    }
    free(sc);
}

void be_attention(dptr out, dptr q, dptr k, dptr v, int nq, int nk, int nhead, int dh)
{
    att_t a;
    a.out = F(out); a.q = F(q); a.k = F(k); a.v = F(v);
    a.nq = nq; a.nk = nk; a.nh = nhead; a.dh = dh;
    pool_run(att_job, &a, nhead * ((nq + 1) / 2), 1);
}

void be_embed(dptr y, dptr tok, dptr pos, int token, int p, int N)
{
    float *Y = F(y);
    const float *T = F(tok) + (size_t)token * N, *P = F(pos) + (size_t)p * N;
    for (int i = 0; i < N; i++) Y[i] = T[i] + P[i];
}
