/*
 * audio.h - sciezka audio I2S miedzy ESP32-S3 a XVF3800.
 * Magistrala I2S: 48 kHz / 32-bit / stereo (XVF3800 master pod Home Assistant).
 * Strumien sieciowy/AI: 16 kHz / mono / 16-bit (s16le) - redukcja w audio.c.
 *  - RX: przetworzony strumien z mikrofonow (beamforming + AEC) z XVF3800,
 *        decymowany do mono s16le @ audio_sample_rate();
 *  - TX: PCM s16le mono od hosta -> interpolacja -> glosnik XVF3800.
 * Glosnosc i wzmocnienie dzialaja programowo (zawsze) oraz przez XVF3800 (best-effort).
 * Kierunek zrodla (DOA) odczytywany z XVF3800.
 */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* format strumienia sieciowego */
int audio_sample_rate(void);     /* Hz */
int audio_channels(void);        /* 1 lub 2 */
#define AUDIO_BITS 16

void audio_init(void);

/* Mikrofon: blokujacy odczyt do timeout_ms. Zwraca liczbe bajtow s16le (wielokrotnosc ramki). */
int  audio_read_mic(void *dst, int maxbytes, int timeout_ms);

/* Glosnik: PCM s16le mono @ audio_sample_rate() do bufora (1 s, start po 200 ms prebuforu).
 * Gdy bufor pelny - czeka do 1 s (kontrola przeplywu dla nadawcy). Zwraca przyjete bajty. */
int  audio_write_spk(const void *src, int bytes);

/* Ustawienia (0..100 glosnosc; dB wzmocnienie). Trzymane w pamieci + best-effort do XVF. */
void audio_set_volume(int pct);
int  audio_get_volume(void);
void audio_set_mic_gain(int db);
int  audio_get_mic_gain(void);

/* DOA: true + wypelnia deg (0..359) i energy (0..255); false = brak danych. */
bool audio_get_doa(int *deg, int *energy);

/* wersja ustawien - rosnie przy kazdej zmianie (dla odswiezania UI sesji) */
uint32_t audio_settings_version(void);

#endif /* AUDIO_H */
