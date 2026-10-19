/*
 * kws_feat.h - cechy log-mel do rozpoznawania slowa kluczowego.
 * TEN SAM kod liczy cechy na ESP32-S3 (firmware) i na PC (kwstool - trening/test),
 * wiec siec widzi dokladnie te same liczby w obu miejscach.
 *
 * 16 kHz, okno 30 ms (480 probek, Hann), krok 20 ms (320), FFT 512, 40 pasm mel (60..7600 Hz),
 * ln(energia + 1e-6), skalowane. Okno sieci: 49 ramek = 0,99 s.
 */
#ifndef KWS_FEAT_H
#define KWS_FEAT_H

#include <stdint.h>

#define KWS_SR      16000
#define KWS_WIN     480
#define KWS_HOP     320
#define KWS_NFFT    512
#define KWS_NMEL    40
#define KWS_FRAMES  49
#define KWS_FMIN    60.0f
#define KWS_FMAX    7600.0f
#define KWS_WINDOW_SAMPLES (KWS_WIN + (KWS_FRAMES - 1) * KWS_HOP)   /* 15840 */

void  kws_feat_init(void);

/* Cechy jednej ramki: x = KWS_WIN probek int16, out = KWS_NMEL liczb.
 * Zwraca energie ramki w dB (skala int16, jak VAD hosta: cisza ~20-30, mowa 50-70). */
float kws_feat_frame(const int16_t *x, float *out);

/* Cechy calego okna: x = KWS_WINDOW_SAMPLES probek, out = KWS_FRAMES x KWS_NMEL (czas-major). */
void  kws_feat_window(const int16_t *x, float *out);

#endif /* KWS_FEAT_H */
