// kernels.cu - kernele CUDA dla Whispera (backend_cuda.c laduje je jako PTX przez nvcuda.dll).
// Kompilacja do PTX (robi to build.bat, jesli jest nvcc):
//   nvcc -ptx -arch=compute_61 kernels.cu -o kernels.ptx
// PTX dla compute_61 sterownik dokompiluje (JIT) na kazda nowsza karte (Pascal..Ada/Blackwell).

extern "C" {

__device__ __forceinline__ float gelu_f(float v)
{
    return 0.5f * v * (1.0f + tanhf(0.7978845608f * (v + 0.044715f * v * v * v)));
}

// suma / maksimum w bloku (blockDim.x <= 1024, wielokrotnosc 32)
__device__ float block_sum(float v)
{
    __shared__ float sh[32];
    for (int o = 16; o > 0; o >>= 1) v += __shfl_down_sync(0xffffffff, v, o);
    int lane = threadIdx.x & 31, wid = threadIdx.x >> 5;
    __syncthreads();
    if (lane == 0) sh[wid] = v;
    __syncthreads();
    int nw = (blockDim.x + 31) >> 5;
    v = (threadIdx.x < nw) ? sh[threadIdx.x] : 0.0f;
    if (wid == 0) for (int o = 16; o > 0; o >>= 1) v += __shfl_down_sync(0xffffffff, v, o);
    if (threadIdx.x == 0) sh[0] = v;
    __syncthreads();
    return sh[0];
}

__device__ float block_max(float v)
{
    __shared__ float sh[32];
    for (int o = 16; o > 0; o >>= 1) v = fmaxf(v, __shfl_down_sync(0xffffffff, v, o));
    int lane = threadIdx.x & 31, wid = threadIdx.x >> 5;
    __syncthreads();
    if (lane == 0) sh[wid] = v;
    __syncthreads();
    int nw = (blockDim.x + 31) >> 5;
    v = (threadIdx.x < nw) ? sh[threadIdx.x] : -1e30f;
    if (wid == 0) for (int o = 16; o > 0; o >>= 1) v = fmaxf(v, __shfl_down_sync(0xffffffff, v, o));
    if (threadIdx.x == 0) sh[0] = v;
    __syncthreads();
    return sh[0];
}

// Y[M,N] = X[M,K] * W[N,K]^T + b.  Kafel 64x64, krok K = 16, 256 watkow, kazdy liczy 4x4.
#define BM 64
#define BN 64
#define BK 16
__global__ void linear_tiled(float *Y, const float *X, const float *W, const float *b,
                             int M, int K, int N, int has_b)
{
    __shared__ float Xs[BK][BM + 4];
    __shared__ float Ws[BK][BN + 4];
    int tid = threadIdx.x, tx = tid & 15, ty = tid >> 4;
    int m0 = blockIdx.y * BM, n0 = blockIdx.x * BN;
    float acc[4][4];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) acc[i][j] = 0.0f;

    for (int k0 = 0; k0 < K; k0 += BK) {
        for (int i = tid; i < BM * BK; i += 256) {
            int r = i / BK, c = i % BK, gm = m0 + r, gk = k0 + c;
            Xs[c][r] = (gm < M && gk < K) ? X[(size_t)gm * K + gk] : 0.0f;
        }
        for (int i = tid; i < BN * BK; i += 256) {
            int r = i / BK, c = i % BK, gn = n0 + r, gk = k0 + c;
            Ws[c][r] = (gn < N && gk < K) ? W[(size_t)gn * K + gk] : 0.0f;
        }
        __syncthreads();
#pragma unroll
        for (int k = 0; k < BK; k++) {
            float a[4], w[4];
            for (int i = 0; i < 4; i++) { a[i] = Xs[k][ty * 4 + i]; w[i] = Ws[k][tx * 4 + i]; }
            for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) acc[i][j] += a[i] * w[j];
        }
        __syncthreads();
    }
    for (int i = 0; i < 4; i++) {
        int m = m0 + ty * 4 + i;
        if (m >= M) continue;
        for (int j = 0; j < 4; j++) {
            int n = n0 + tx * 4 + j;
            if (n < N) Y[(size_t)m * N + n] = acc[i][j] + (has_b ? b[n] : 0.0f);
        }
    }
}

// y[N] = W[N,K] * x[K] + b - jeden warp na wyjscie (decoder, M = 1)
__global__ void gemv(float *y, const float *x, const float *W, const float *b, int K, int N, int has_b)
{
    int warp = (blockIdx.x * blockDim.x + threadIdx.x) >> 5, lane = threadIdx.x & 31;
    if (warp >= N) return;
    const float *w = W + (size_t)warp * K;
    float s = 0.0f;
    if ((K & 3) == 0) {
        const float4 *w4 = (const float4 *)w, *x4 = (const float4 *)x;
        for (int k = lane; k < (K >> 2); k += 32) {
            float4 a = w4[k], c = x4[k];
            s += a.x * c.x + a.y * c.y + a.z * c.z + a.w * c.w;
        }
    } else {
        for (int k = lane; k < K; k += 32) s += w[k] * x[k];
    }
    for (int o = 16; o > 0; o >>= 1) s += __shfl_down_sync(0xffffffff, s, o);
    if (lane == 0) y[warp] = s + (has_b ? b[warp] : 0.0f);
}

// LayerNorm - jeden blok na wiersz
__global__ void layernorm(float *y, const float *x, const float *g, const float *b, int N)
{
    const float *xr = x + (size_t)blockIdx.x * N;
    float *yr = y + (size_t)blockIdx.x * N;
    float s = 0.0f;
    for (int i = threadIdx.x; i < N; i += blockDim.x) s += xr[i];
    float mean = block_sum(s) / N;
    float v = 0.0f;
    for (int i = threadIdx.x; i < N; i += blockDim.x) { float d = xr[i] - mean; v += d * d; }
    float inv = rsqrtf(block_sum(v) / N + 1e-5f);
    for (int i = threadIdx.x; i < N; i += blockDim.x) yr[i] = (xr[i] - mean) * inv * g[i] + b[i];
}

__global__ void gelu_inplace(float *x, int n)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) x[i] = gelu_f(x[i]);
}

__global__ void add_inplace(float *y, const float *a, int n)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) y[i] += a[i];
}

// conv1d k=3 pad=1 + bias + GELU; x[Cin][L] -> y[Cout][Lout]
__global__ void conv1d_gelu(float *y, const float *x, const float *W, const float *b,
                            int Cin, int L, int Lout, int stride, int Cout)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= Cout * Lout) return;
    int o = idx / Lout, t = idx % Lout, base = t * stride - 1;
    const float *w = W + (size_t)o * Cin * 3;
    float s = b[o];
    for (int ci = 0; ci < Cin; ci++) {
        const float *xr = x + (size_t)ci * L;
        const float *wc = w + ci * 3;
        if (base >= 0)    s += wc[0] * xr[base];
        s += wc[1] * xr[base + 1];
        if (base + 2 < L) s += wc[2] * xr[base + 2];
    }
    y[idx] = gelu_f(s);
}

// y[t][c] = x[c][t] + pos[t][c]
__global__ void transpose_addpos(float *y, const float *x, const float *pos, int C, int T)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= C * T) return;
    int t = idx / C, c = idx % C;
    y[idx] = x[(size_t)c * T + t] + pos[idx];
}

// uwaga: blok na (zapytanie i, glowa h); wyniki punktowe w pamieci wspoldzielonej (nk floatow)
__global__ void attention(float *out, const float *q, const float *k, const float *v,
                          int nk, int nh, int dh, float scale)
{
    extern __shared__ float sc[];
    __shared__ float qs[128];
    int i = blockIdx.x, h = blockIdx.y, ld = nh * dh;
    const float *qi = q + (size_t)i * ld + h * dh;
    for (int d = threadIdx.x; d < dh; d += blockDim.x) qs[d] = qi[d];
    __syncthreads();

    float mx = -1e30f;
    for (int j = threadIdx.x; j < nk; j += blockDim.x) {
        const float *kj = k + (size_t)j * ld + h * dh;
        float s = 0.0f;
        for (int d = 0; d < dh; d++) s += qs[d] * kj[d];
        s *= scale;
        sc[j] = s;
        mx = fmaxf(mx, s);
    }
    mx = block_max(mx);
    float sum = 0.0f;
    for (int j = threadIdx.x; j < nk; j += blockDim.x) { float e = __expf(sc[j] - mx); sc[j] = e; sum += e; }
    sum = block_sum(sum);
    float inv = 1.0f / sum;
    for (int d = threadIdx.x; d < dh; d += blockDim.x) {
        float o = 0.0f;
        const float *vd = v + h * dh + d;
        for (int j = 0; j < nk; j++) o += sc[j] * vd[(size_t)j * ld];
        out[(size_t)i * ld + h * dh + d] = o * inv;
    }
}

__global__ void embed(float *y, const float *tok, const float *pos, int token, int p, int N)
{
    int c = blockIdx.x * blockDim.x + threadIdx.x;
    if (c < N) y[c] = tok[(size_t)token * N + c] + pos[(size_t)p * N + c];
}

} // extern "C"
