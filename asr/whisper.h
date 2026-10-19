/*
 * whisper.h - rozpoznawanie mowy modelem OpenAI Whisper (licencja MIT) w czystym C.
 * Wagi w formacie ggml z whisper.cpp (ggml-tiny.bin / ggml-base.bin / ggml-small.bin ...,
 * wersje f16/f32 oraz q8_0, np. ggml-large-v3-turbo-q8_0.bin). Obliczenia przez backend.h.
 *
 * Wejscie: PCM 16 kHz mono float [-1..1], do 30 s - dokladnie format strumienia ReSpeakera.
 */
#ifndef WHISPER_H
#define WHISPER_H

typedef struct whisper whisper;

whisper    *wh_load(const char *path);         /* NULL = blad (opis na stderr) */
void        wh_free(whisper *w);

int         wh_lang_id(const char *code);      /* "pl" -> 10, "auto"/NULL -> -1, nieznany -> -2 */
const char *wh_lang_code(int id);
int         wh_multilingual(const whisper *w);

#define WH_SAMPLE_RATE 16000
#define WH_MAX_SAMPLES (WH_SAMPLE_RATE * 30)

/* Transkrypcja jednego fragmentu (<= 30 s).
 *   lang      - id jezyka albo -1 = wykryj
 *   audio_ctx - 0 = pelny kontekst 30 s (najlepsza jakosc), >0 = skrocony (szybciej, CPU)
 *   out       - tekst UTF-8
 *   no_speech - prawdopodobienstwo "to nie mowa" (do odrzucania halucynacji na ciszy)
 *   lang_out  - uzyty/wykryty jezyk (moze byc NULL)
 *   avg_logprob - srednie log-prawdopodobienstwo tokenow (pewnosc; moze byc NULL).
 *               Whisper odrzuca fragment, gdy no_speech > 0.6 I avg_logprob < -1.
 * Zwraca liczbe tokenow tekstu (>=0) albo -1 przy bledzie. */
int wh_transcribe(whisper *w, const float *pcm, int n, int lang, int audio_ctx,
                  char *out, int outsz, float *no_speech, int *lang_out, float *avg_logprob);

#endif /* WHISPER_H */
