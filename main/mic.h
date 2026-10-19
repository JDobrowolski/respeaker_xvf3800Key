/*
 * mic.h - jedyny odbiorca mikrofonu I2S: task czyta audio_read_mic i zapisuje do historii
 * (pierscien 1,5 s). Kazdy uzytkownik (slowo kluczowe, strumien sieciowy) czyta z wlasnej
 * pozycji - bezwzglednego numeru probki - wiec "audio sprzed chwili" to po prostu pozycja
 * cofnieta o N probek.
 */
#ifndef MIC_H
#define MIC_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MIC_HIST_SAMPLES (16000 * 3 / 2)

void     mic_start(void);
uint32_t mic_wpos(void);                                /* ile probek zapisano od startu */
/* kopiuje do n probek od pozycji pos; zwraca ile (0 = brak nowych), -1 = pos juz nadpisane */
int      mic_read(uint32_t pos, int16_t *dst, int n);
void     mic_set_listener(TaskHandle_t t);              /* powiadamiany po kazdej porcji */

#endif /* MIC_H */
