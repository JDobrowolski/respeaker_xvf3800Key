/*
 * xvf3800.h - sterowanie ukladem XMOS XVF3800 (ReSpeaker 4-Mic) przez I2C.
 *
 * XVF3800 uzywa protokolu kontrolnego lib_device_control. Transport I2C:
 *   ZAPIS:  [resid, cmd,        len, data0..data(len-1)]
 *   ODCZYT: [resid, cmd|0x80,   len]  -> powtorzony START -> czytamy (len+1) bajtow,
 *           bajt[0] = status (0 = OK), bajt[1..] = dane.
 *
 * UWAGA: konkretne numery RESID/CMD zaleza od BUDOWY firmware XVF3800 (plik
 * mapy sterowania .json / device_control_shared.h z Twojego obrazu XMOS).
 * Ponizsze wartosci to miejsca do uzupelnienia - sprawdz je dla swojego firmware.
 * Glosnosc i wzmocnienie dzialaja takze programowo (w audio.c), wiec strumien
 * jest poprawny nawet przy niezgodnej mapie; DOA wymaga poprawnego RESID/CMD.
 */
#ifndef XVF3800_H
#define XVF3800_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* --- mapa sterowania (DO SPRAWDZENIA dla Twojego firmware XVF3800) --------- */
#ifndef XVF_RESID_AUDIO_MGR
#define XVF_RESID_AUDIO_MGR   20   /* zarzadca audio (glosnik / wyjscie) */
#endif
#ifndef XVF_CMD_SET_VOLUME
#define XVF_CMD_SET_VOLUME    10   /* u8: glosnosc 0..100 */
#endif
#ifndef XVF_RESID_PP
#define XVF_RESID_PP          30   /* post-processing (wzmocnienie mikrofonu) */
#endif
#ifndef XVF_CMD_SET_MIC_GAIN
#define XVF_CMD_SET_MIC_GAIN  11   /* s8: wzmocnienie [dB] */
#endif
#ifndef XVF_RESID_DOA
#define XVF_RESID_DOA         40   /* kierunek zrodla */
#endif
#ifndef XVF_CMD_GET_DOA
#define XVF_CMD_GET_DOA       1    /* -> u16 kat[stopnie], u8 energia */
#endif

esp_err_t xvf_init(void);                 /* magistrala I2C + wykrycie ukladu */
bool      xvf_present(void);              /* czy uklad odpowiedzial przy init */

/* Niskopoziomowy transport (zwraca ESP_OK / blad). */
esp_err_t xvf_write(uint8_t resid, uint8_t cmd, const uint8_t *data, uint8_t len);
esp_err_t xvf_read(uint8_t resid, uint8_t cmd, uint8_t *data, uint8_t len);

/* Wygodne operacje (best-effort; brak ukladu -> ESP_ERR_INVALID_STATE). */
esp_err_t xvf_set_volume(int pct);        /* 0..100 */
esp_err_t xvf_set_mic_gain(int db);       /* dB */
/* DOA: zwraca ESP_OK i wypelnia deg (0..359) + energy (0..255); blad = brak danych. */
esp_err_t xvf_get_doa(int *deg, int *energy);

#endif /* XVF3800_H */
