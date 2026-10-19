/*
 * tts.h - synteza mowy Piper (VITS, licencja MIT) w C dla TCC64 / Windows.
 * espeak-ng.dll (tekst -> fonemy IPA) i onnxruntime.dll (model glosu .onnx) ladowane
 * dynamicznie z katalogu tts\bin - bez bibliotek importu i bez C++.
 *
 *     tts *t = tts_load("tts\\voices\\pl_PL-gosia-medium.onnx", "tts\\bin", 0);
 *     int n; int16_t *pcm = tts_say(t, "Dzien dobry!", 16000, &n);   // PCM mono 16 kHz
 */
#ifndef TTS_H
#define TTS_H

#include <stdint.h>

typedef struct tts tts;

/* voice: plik .onnx (obok musi byc .onnx.json); bin_dir: katalog z DLL i espeak-ng-data;
 * threads: watki CPU (0 = domyslnie); gpu: 1 = sprobuj DirectML (GPU), 0 = CPU */
tts        *tts_load(const char *voice, const char *bin_dir, int threads, int gpu);
void        tts_free(tts *t);
const char *tts_info(const tts *t);

/* tekst UTF-8 -> PCM mono s16 o czestotliwosci rate (malloc; zwolnij free). NULL = blad */
int16_t    *tts_say(tts *t, const char *text, int rate, int *n);

#endif /* TTS_H */
