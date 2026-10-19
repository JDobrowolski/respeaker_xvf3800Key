/*
 * kws_feat.c - log-mel: okno Hanna, FFT 512 (radix-2), filtry trojkatne mel (HTK), logarytm.
 * Tablice liczone raz w kws_feat_init (~3 KB): okno, wspolczynniki FFT, przypisanie binow do pasm.
 */
#include <math.h>
#include <string.h>
#include "kws_feat.h"

#define NBINS (KWS_NFFT / 2 + 1)
#define PI_D  3.14159265358979323846

static float   s_win[KWS_WIN];
static float   s_cos[KWS_NFFT / 2], s_sin[KWS_NFFT / 2];
static int16_t s_rev[KWS_NFFT];
static int8_t  s_band[NBINS];      /* odcinek mel binu: bin rosnie w pasmie s_band, opada w s_band-1 */
static float   s_rise[NBINS];      /* waga zbocza rosnacego (opadajace = 1 - s_rise) */
static int     s_ready;

static double hz2mel(double f) { return 2595.0 * log10(1.0 + f / 700.0); }

void kws_feat_init(void)
{
    if (s_ready) return;
    for (int i = 0; i < KWS_WIN; i++) s_win[i] = (float)(0.5 - 0.5 * cos(2.0 * PI_D * i / KWS_WIN));
    for (int i = 0; i < KWS_NFFT / 2; i++) {
        s_cos[i] = (float)cos(2.0 * PI_D * i / KWS_NFFT);
        s_sin[i] = (float)sin(2.0 * PI_D * i / KWS_NFFT);
    }
    int bits = 0;
    while ((1 << bits) < KWS_NFFT) bits++;
    for (int i = 0; i < KWS_NFFT; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        s_rev[i] = (int16_t)r;
    }
    /* punkty mel: KWS_NMEL + 2 rownomiernie od FMIN do FMAX */
    double m0 = hz2mel(KWS_FMIN), m1 = hz2mel(KWS_FMAX);
    double step = (m1 - m0) / (KWS_NMEL + 1);
    for (int k = 0; k < NBINS; k++) {
        double mv = hz2mel((double)k * KWS_SR / KWS_NFFT);
        double pos = (mv - m0) / step;              /* ktory odcinek miedzy punktami mel */
        if (pos < 0 || pos >= KWS_NMEL + 1) { s_band[k] = -1; s_rise[k] = 0; continue; }
        int i = (int)pos;
        s_band[k] = (int8_t)i;
        s_rise[k] = (float)(pos - i);
    }
    s_ready = 1;
}

static void fft512(float *re, float *im)
{
    for (int i = 0; i < KWS_NFFT; i++) {
        int j = s_rev[i];
        if (j > i) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= KWS_NFFT; len <<= 1) {
        int half = len >> 1, step = KWS_NFFT / len;
        for (int i = 0; i < KWS_NFFT; i += len) {
            for (int k = 0; k < half; k++) {
                float wr = s_cos[k * step], wi = -s_sin[k * step];
                int a = i + k, b = a + half;
                float xr = re[b] * wr - im[b] * wi;
                float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
            }
        }
    }
}

float kws_feat_frame(const int16_t *x, float *out)
{
    float re[KWS_NFFT], im[KWS_NFFT], mel[KWS_NMEL];
    double e2 = 0;
    for (int i = 0; i < KWS_WIN; i++) {
        e2 += (double)x[i] * x[i];
        re[i] = (x[i] * (1.0f / 32768.0f)) * s_win[i];
        im[i] = 0;
    }
    for (int i = KWS_WIN; i < KWS_NFFT; i++) { re[i] = 0; im[i] = 0; }
    fft512(re, im);
    for (int m = 0; m < KWS_NMEL; m++) mel[m] = 0;
    for (int k = 0; k < NBINS; k++) {
        int i = s_band[k];
        if (i < 0) continue;
        float p = re[k] * re[k] + im[k] * im[k];
        if (i < KWS_NMEL) mel[i] += p * s_rise[k];               /* zbocze rosnace pasma i */
        if (i >= 1) mel[i - 1] += p * (1.0f - s_rise[k]);        /* zbocze opadajace pasma i-1 */
    }
    for (int m = 0; m < KWS_NMEL; m++) out[m] = ((float)log(mel[m] + 1e-6f) + 5.0f) * 0.2f;
    return (float)(10.0 * log10(e2 / KWS_WIN + 1.0));
}

void kws_feat_window(const int16_t *x, float *out)
{
    for (int f = 0; f < KWS_FRAMES; f++) kws_feat_frame(x + f * KWS_HOP, out + f * KWS_NMEL);
}
