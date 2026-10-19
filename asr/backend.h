/*
 * backend.h - operacje numeryczne potrzebne Whisperowi. Dwie implementacje:
 *   hostCPU/backend_cpu.c   - czyste C + pula watkow (TCC64)
 *   hostGPU/backend_cuda.c  - CUDA Driver API (nvcuda.dll) + kernels.ptx
 *
 * Bufor to "dptr": na CPU adres float*, na GPU CUdeviceptr. Oba sa adresami bajtowymi,
 * wiec przesuniecie o n liczb to  d + n*sizeof(float)  (makro BOFF).
 * Wszystkie macierze sa wierszowe (row-major), float32.
 */
#ifndef BACKEND_H
#define BACKEND_H

#include <stddef.h>

typedef unsigned long long dptr;
#define BOFF(d, n) ((d) + (dptr)(n) * 4u)          /* d przesuniete o n liczb float */

int   be_init(int nthreads);                       /* 0 = OK */
const char *be_name(void);                         /* opis: "CPU 20 watkow" / "GPU RTX..." */
void  be_shutdown(void);

dptr  be_alloc(size_t nfloat);                     /* 0 = brak pamieci */
void  be_free(dptr d);
void  be_upload(dptr d, const float *h, size_t n);
void  be_download(float *h, dptr d, size_t n);     /* czeka na zakonczenie obliczen */

/* y[M,N] = x[M,K] * W[N,K]^T + b[N]   (b == 0 - bez biasu); W jak nn.Linear (out,in) */
void  be_linear(dptr y, dptr x, dptr W, dptr b, int M, int K, int N);

/* LayerNorm wierszy: y[M,N] = (x - mean) / sqrt(var + 1e-5) * g + b */
void  be_layernorm(dptr y, dptr x, dptr g, dptr b, int M, int N);

void  be_gelu(dptr x, size_t n);                   /* w miejscu */
void  be_add(dptr y, dptr a, size_t n);            /* y += a */

/* conv1d k=3, pad=1, stride 1|2, + bias + GELU:
 * x[Cin][L] -> y[Cout][Lout], Lout = (L + 2 - 3) / stride + 1, W[Cout][Cin][3] */
void  be_conv1d_gelu(dptr y, dptr x, dptr W, dptr b, int Cin, int Cout, int L, int stride);

/* y[t][c] = x[c][t] + pos[t][c]   (x: C x T, y/pos: T x C) */
void  be_transpose_addpos(dptr y, dptr x, dptr pos, int C, int T);

/* Wielowatkowa uwaga (bez maski): dla kazdej glowy h i zapytania i
 *   out[i, h*dh..] = softmax(q_i,h . k_j,h / sqrt(dh)) . v_j,h
 * q: nq x ld, k/v: nk x ld, out: nq x ld; ld = nhead*dh */
void  be_attention(dptr out, dptr q, dptr k, dptr v, int nq, int nk, int nhead, int dh);

/* y[c] = tok[token][c] + pos[p][c] */
void  be_embed(dptr y, dptr tok, dptr pos, int token, int p, int N);

/* Wagi kwantyzowane Q8_0 (ggml: bloki po 32 - fp16 skala + 32 x int8), K % 32 == 0.
 * be_upload_q8 tworzy bufor z nrows wierszy po K wag w ukladzie wlasnym backendu;
 * 0 = backend nie liczy na Q8_0 (whisper.c rozpakowuje wtedy wagi do float32). */
dptr  be_upload_q8(const void *blocks, int nrows, int K);
/* jak be_linear / be_embed, ale W / tok to bufor z be_upload_q8 */
void  be_linear_q8(dptr y, dptr x, dptr W, dptr b, int M, int K, int N);
void  be_embed_q8(dptr y, dptr tok, dptr pos, int token, int p, int N);

#endif /* BACKEND_H */
