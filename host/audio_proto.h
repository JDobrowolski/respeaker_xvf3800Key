/*
 * audio_proto.h - prosty, ramkowany protokol strumienia audio po TCP.
 * Wspolny dla firmware ESP32-S3 i konsolowego narzedzia hosta (ten sam plik
 * jest kopiowany do host/). Czyste C, little-endian, bez zaleznosci.
 *
 * Po nawiazaniu polaczenia URZADZENIE wysyla naglowek powitania:
 *     char magic[4] = "VAUD"
 *     u8   version          (= AUDIO_PROTO_VERSION)
 *     u8   channels         (kanaly strumienia mikrofonu i odtwarzania: 1 lub 2)
 *     u8   bits             (zawsze 16 - PCM s16le)
 *     u8   flags            (AUDIO_FL_*; wersja 1: 0)
 *     u32  sample_rate      (Hz, np. 16000)
 * (razem 12 bajtow)
 *
 * Tryb slowa kluczowego (wersja 2): urzadzenie samo slucha i NIC nie wysyla, dopoki nie
 * rozpozna slowa. Wtedy: AF_WAKE, strumien AF_MIC z poleceniem (od chwili tuz przed koncem
 * slowa), a po ciszy / limicie czasu - AF_END. Host w tym czasie spi (blokujacy odczyt).
 * AF_REC = 1 nadal wlacza staly strumien (nagrywanie probek, stare narzedzia).
 *
 * Dalej - ramki w OBIE strony, kazda:
 *     u8   typ   (AF_*)
 *     u16  len   (dlugosc danych, little-endian; 0..AUDIO_MAX_PAYLOAD)
 *     u8   data[len]
 */
#ifndef AUDIO_PROTO_H
#define AUDIO_PROTO_H

#include <stdint.h>

#define AUDIO_MAGIC          "VAUD"
#define AUDIO_PROTO_VERSION  2
#define AUDIO_HELLO_LEN      12
#define AUDIO_MAX_PAYLOAD    4096

/* flagi powitania (bajt 7) */
#define AUDIO_FL_KWS    0x01   /* urzadzenie obsluguje slowo kluczowe */
#define AUDIO_FL_MODEL  0x02   /* model slowa kluczowego wczytany - nasluchuje */
#define AUDIO_FL_RECONLY 0x04  /* obsluguje AF_REC = AF_REC_ONLY (strumien bez polecen) */

/* urzadzenie -> klient */
#define AF_MIC     'M'   /* dane PCM z mikrofonu (s16le, interleaved wg channels) */
#define AF_DOA     'D'   /* kierunek zrodla: u16 kat 0..359 (0xFFFF = nieznany), u8 energia 0..255 */
#define AF_STATUS  'S'   /* tekst informacyjny (UTF-8, bez zera); odpowiedz na AF_MODEL: "OK ..." / "BLAD ..." */
#define AF_WAKE    'W'   /* wykryto slowo: u8 wynik 0..255, u16 kat DOA (0xFFFF = brak), slowo (UTF-8) */
#define AF_END     'E'   /* koniec polecenia: u8 powod (AF_END_*) */

#define AF_END_SILENCE  0  /* cisza po wypowiedzi */
#define AF_END_MAXTIME  1  /* limit dlugosci polecenia */
#define AF_END_NOSPEECH 2  /* po slowie nikt nic nie powiedzial */
#define AF_END_HOST     3  /* host przerwal (AF_LISTEN 0) */

/* klient -> urzadzenie */
#define AF_PLAY    'P'   /* dane PCM do glosnika (s16le, mono, sample_rate urzadzenia) */
#define AF_VOLUME  'V'   /* u8 glosnosc glosnika 0..100 */
#define AF_GAIN    'G'   /* s8 wzmocnienie mikrofonu w dB (zakres -12..+36) */
#define AF_REC     'R'   /* u8: 1 = wlacz staly strumien mikrofonu, 0 = wylacz, 2 = AF_REC_ONLY */
#define AF_LISTEN  'L'   /* u8: 1 = sluchaj polecenia teraz (jak po slowie), 0 = zakoncz polecenie */
#define AF_THR     'T'   /* u8 prog slowa kluczowego 1..254 (/255); 0 = domyslny z modelu */
#define AF_MODEL   'X'   /* wgrywanie modelu: u8 op + dane (AF_MODEL_*) */

#define AF_REC_ONLY 2      /* staly strumien BEZ dokladania polecen po slowie kluczowym: urzadzenie
                            * nadal pika i wysyla AF_WAKE / AF_END, ale nie AF_MIC z poleceniem
                            * (inaczej dwa strumienie AF_MIC przeplataja sie - vsbridge) */

#define AF_MODEL_BEGIN 0   /* + u32 rozmiar pliku .kws */
#define AF_MODEL_DATA  1   /* + kolejny kawalek pliku */
#define AF_MODEL_END   2   /* + u32 crc32 calego pliku -> odpowiedz AF_STATUS */

/* obie strony */
#define AF_PING    'p'   /* u8 (dowolny) - utrzymanie polaczenia / pomiar */

#endif /* AUDIO_PROTO_H */
