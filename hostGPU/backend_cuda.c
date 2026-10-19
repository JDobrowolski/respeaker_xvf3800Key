/*
 * backend_cuda.c - operacje Whispera na GPU NVIDIA przez CUDA Driver API.
 * Jak w zestawznakow4/Infer.c: nvcuda.dll ladowane dynamicznie (LoadLibrary), kernele
 * z kernels.ptx (nvcc -ptx). Kompiluje sie TCC64 - bez naglowkow CUDA i bez cudart.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../asr/backend.h"

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
typedef void *CUmodule;
typedef void *CUfunction;
typedef unsigned long long CUdeviceptr;
typedef void *CUstream;

typedef CUresult (*PFN_cuInit)(unsigned int);
typedef CUresult (*PFN_cuDeviceGet)(CUdevice *, int);
typedef CUresult (*PFN_cuDeviceGetName)(char *, int, CUdevice);
typedef CUresult (*PFN_cuCtxCreate)(CUcontext *, unsigned int, CUdevice);
typedef CUresult (*PFN_cuCtxDestroy)(CUcontext);
typedef CUresult (*PFN_cuModuleLoad)(CUmodule *, const char *);
typedef CUresult (*PFN_cuModuleGetFunction)(CUfunction *, CUmodule, const char *);
typedef CUresult (*PFN_cuMemAlloc)(CUdeviceptr *, size_t);
typedef CUresult (*PFN_cuMemFree)(CUdeviceptr);
typedef CUresult (*PFN_cuMemcpyHtoD)(CUdeviceptr, const void *, size_t);
typedef CUresult (*PFN_cuMemcpyDtoH)(void *, CUdeviceptr, size_t);
typedef CUresult (*PFN_cuLaunchKernel)(CUfunction, unsigned int, unsigned int, unsigned int,
                                       unsigned int, unsigned int, unsigned int,
                                       unsigned int, CUstream, void **, void **);
typedef CUresult (*PFN_cuGetErrorString)(CUresult, const char **);

static HMODULE                 s_dll;
static PFN_cuMemAlloc          pMemAlloc;
static PFN_cuMemFree           pMemFree;
static PFN_cuMemcpyHtoD        pHtoD;
static PFN_cuMemcpyDtoH        pDtoH;
static PFN_cuLaunchKernel      pLaunch;
static PFN_cuGetErrorString    pErr;
static PFN_cuCtxDestroy        pCtxDestroy;
static CUcontext               s_ctx;
static CUmodule                s_mod;
static CUfunction k_linear, k_gemv, k_ln, k_gelu, k_add, k_conv, k_tpos, k_att, k_embed;
static char                    s_name[160];

static void cu_check(CUresult r, const char *what)
{
    if (r == 0) return;
    const char *s = NULL;
    if (pErr) pErr(r, &s);
    fprintf(stderr, "CUDA blad %d przy %s: %s\n", r, what, s ? s : "?");
    exit(1);
}

static void *sym(const char *a, const char *b)
{
    void *p = (void *)GetProcAddress(s_dll, a);
    if (!p && b) p = (void *)GetProcAddress(s_dll, b);
    if (!p) { fprintf(stderr, "brak funkcji %s w nvcuda.dll\n", a); exit(1); }
    return p;
}

int be_init(int nthreads)
{
    (void)nthreads;
    s_dll = LoadLibraryA("nvcuda.dll");
    if (!s_dll) { fprintf(stderr, "brak nvcuda.dll - zainstaluj sterownik NVIDIA\n"); return 1; }
    PFN_cuInit pInit = (PFN_cuInit)sym("cuInit", NULL);
    PFN_cuDeviceGet pDev = (PFN_cuDeviceGet)sym("cuDeviceGet", NULL);
    PFN_cuDeviceGetName pName = (PFN_cuDeviceGetName)sym("cuDeviceGetName", NULL);
    PFN_cuCtxCreate pCtx = (PFN_cuCtxCreate)sym("cuCtxCreate_v2", "cuCtxCreate");
    PFN_cuModuleLoad pLoad = (PFN_cuModuleLoad)sym("cuModuleLoad", NULL);
    PFN_cuModuleGetFunction pFn = (PFN_cuModuleGetFunction)sym("cuModuleGetFunction", NULL);
    pCtxDestroy = (PFN_cuCtxDestroy)sym("cuCtxDestroy_v2", "cuCtxDestroy");
    pMemAlloc = (PFN_cuMemAlloc)sym("cuMemAlloc_v2", "cuMemAlloc");
    pMemFree = (PFN_cuMemFree)sym("cuMemFree_v2", "cuMemFree");
    pHtoD = (PFN_cuMemcpyHtoD)sym("cuMemcpyHtoD_v2", "cuMemcpyHtoD");
    pDtoH = (PFN_cuMemcpyDtoH)sym("cuMemcpyDtoH_v2", "cuMemcpyDtoH");
    pLaunch = (PFN_cuLaunchKernel)sym("cuLaunchKernel", NULL);
    pErr = (PFN_cuGetErrorString)GetProcAddress(s_dll, "cuGetErrorString");

    CUdevice dev;
    cu_check(pInit(0), "cuInit");
    cu_check(pDev(&dev, 0), "cuDeviceGet");
    char nm[128] = "";
    pName(nm, sizeof nm, dev);
    cu_check(pCtx(&s_ctx, 0, dev), "cuCtxCreate");

    /* kernels.ptx obok exe, a gdy go tam nie ma - w biezacym katalogu */
    char ptx[MAX_PATH];
    GetModuleFileNameA(NULL, ptx, sizeof ptx);
    char *sl = strrchr(ptx, '\\');
    if (sl) strcpy(sl + 1, "kernels.ptx"); else strcpy(ptx, "kernels.ptx");
    if (pLoad(&s_mod, ptx) != 0) cu_check(pLoad(&s_mod, "kernels.ptx"), "cuModuleLoad kernels.ptx");

    cu_check(pFn(&k_linear, s_mod, "linear_tiled"), "linear_tiled");
    cu_check(pFn(&k_gemv, s_mod, "gemv"), "gemv");
    cu_check(pFn(&k_ln, s_mod, "layernorm"), "layernorm");
    cu_check(pFn(&k_gelu, s_mod, "gelu_inplace"), "gelu_inplace");
    cu_check(pFn(&k_add, s_mod, "add_inplace"), "add_inplace");
    cu_check(pFn(&k_conv, s_mod, "conv1d_gelu"), "conv1d_gelu");
    cu_check(pFn(&k_tpos, s_mod, "transpose_addpos"), "transpose_addpos");
    cu_check(pFn(&k_att, s_mod, "attention"), "attention");
    cu_check(pFn(&k_embed, s_mod, "embed"), "embed");
    sprintf(s_name, "GPU %s (CUDA Driver API)", nm);
    return 0;
}

const char *be_name(void) { return s_name; }

void be_shutdown(void)
{
    if (s_ctx) pCtxDestroy(s_ctx);
    if (s_dll) FreeLibrary(s_dll);
}

dptr be_alloc(size_t n)
{
    CUdeviceptr d = 0;
    if (pMemAlloc(&d, n * sizeof(float)) != 0) return 0;
    return (dptr)d;
}
void be_free(dptr d) { if (d) pMemFree((CUdeviceptr)d); }
void be_upload(dptr d, const float *h, size_t n) { cu_check(pHtoD((CUdeviceptr)d, h, n * sizeof(float)), "HtoD"); }
void be_download(float *h, dptr d, size_t n) { cu_check(pDtoH(h, (CUdeviceptr)d, n * sizeof(float)), "DtoH"); }

static void launch(CUfunction f, unsigned gx, unsigned gy, unsigned bx, unsigned shmem, void **args, const char *nm)
{
    cu_check(pLaunch(f, gx, gy, 1, bx, 1, 1, shmem, (CUstream)0, args, NULL), nm);
}

static unsigned blocks(size_t n, unsigned b) { return (unsigned)((n + b - 1) / b); }

void be_linear(dptr y, dptr x, dptr W, dptr b, int M, int K, int N)
{
    int has_b = b != 0;
    if (M == 1) {
        void *a[] = { &y, &x, &W, &b, &K, &N, &has_b };
        launch(k_gemv, blocks((size_t)N * 32, 256), 1, 256, 0, a, "gemv");
    } else {
        void *a[] = { &y, &x, &W, &b, &M, &K, &N, &has_b };
        launch(k_linear, blocks(N, 64), blocks(M, 64), 256, 0, a, "linear_tiled");
    }
}

void be_layernorm(dptr y, dptr x, dptr g, dptr b, int M, int N)
{
    void *a[] = { &y, &x, &g, &b, &N };
    launch(k_ln, M, 1, 256, 0, a, "layernorm");
}

void be_gelu(dptr x, size_t n)
{
    int nn = (int)n;
    void *a[] = { &x, &nn };
    launch(k_gelu, blocks(n, 256), 1, 256, 0, a, "gelu");
}

void be_add(dptr y, dptr s, size_t n)
{
    int nn = (int)n;
    void *a[] = { &y, &s, &nn };
    launch(k_add, blocks(n, 256), 1, 256, 0, a, "add");
}

void be_conv1d_gelu(dptr y, dptr x, dptr W, dptr b, int Cin, int Cout, int L, int stride)
{
    int Lout = (L + 2 - 3) / stride + 1;
    void *a[] = { &y, &x, &W, &b, &Cin, &L, &Lout, &stride, &Cout };
    launch(k_conv, blocks((size_t)Cout * Lout, 256), 1, 256, 0, a, "conv1d_gelu");
}

void be_transpose_addpos(dptr y, dptr x, dptr pos, int C, int T)
{
    void *a[] = { &y, &x, &pos, &C, &T };
    launch(k_tpos, blocks((size_t)C * T, 256), 1, 256, 0, a, "transpose_addpos");
}

void be_attention(dptr out, dptr q, dptr k, dptr v, int nq, int nk, int nhead, int dh)
{
    float scale = (float)(1.0 / sqrt((double)dh));
    void *a[] = { &out, &q, &k, &v, &nk, &nhead, &dh, &scale };
    launch(k_att, nq, nhead, 128, (unsigned)(nk * sizeof(float)), a, "attention");
}

void be_embed(dptr y, dptr tok, dptr pos, int token, int p, int N)
{
    void *a[] = { &y, &tok, &pos, &token, &p, &N };
    launch(k_embed, blocks(N, 256), 1, 256, 0, a, "embed");
}

/* Q8_0 liczy tylko CPU - na GPU whisper.c rozpakowuje wagi do float32 */
dptr be_upload_q8(const void *blocks, int nrows, int K) { (void)blocks; (void)nrows; (void)K; return 0; }
void be_linear_q8(dptr y, dptr x, dptr W, dptr b, int M, int K, int N)
{ (void)y; (void)x; (void)W; (void)b; (void)M; (void)K; (void)N; }
void be_embed_q8(dptr y, dptr tok, dptr pos, int token, int p, int N)
{ (void)y; (void)tok; (void)pos; (void)token; (void)p; (void)N; }
