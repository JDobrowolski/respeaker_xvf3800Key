/*
 * mic.c - ciagly odczyt mikrofonu do pierscienia historii (jeden zapisujacy, wielu czytajacych).
 */
#include <string.h>
#include <stdlib.h>
#include "mic.h"
#include "audio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "mic";

static int16_t           *s_hist;
static volatile uint32_t  s_wpos;
static TaskHandle_t       s_listener;

uint32_t mic_wpos(void) { return __atomic_load_n(&s_wpos, __ATOMIC_ACQUIRE); }
void mic_set_listener(TaskHandle_t t) { s_listener = t; }

int mic_read(uint32_t pos, int16_t *dst, int n)
{
    uint32_t w = mic_wpos();
    if (w - pos > MIC_HIST_SAMPLES) return -1;          /* za stare (lub pos z przyszlosci) */
    uint32_t avail = w - pos;
    if ((uint32_t)n > avail) n = (int)avail;
    for (int i = 0; i < n;) {
        uint32_t idx = (pos + i) % MIC_HIST_SAMPLES;
        int k = n - i;
        if (idx + k > MIC_HIST_SAMPLES) k = MIC_HIST_SAMPLES - idx;
        memcpy(dst + i, s_hist + idx, sizeof(int16_t) * k);
        i += k;
    }
    /* dane mogly zostac nadpisane w trakcie kopiowania */
    if (mic_wpos() - pos > MIC_HIST_SAMPLES) return -1;
    return n;
}

static void mic_task(void *arg)
{
    (void)arg;
    int16_t buf[320];                                   /* 20 ms */
    for (;;) {
        int b = audio_read_mic(buf, sizeof buf, 100);
        int n = b / 2;
        if (n <= 0) continue;
        uint32_t w = s_wpos;
        for (int i = 0; i < n; i++) s_hist[(w + i) % MIC_HIST_SAMPLES] = buf[i];
        __atomic_store_n(&s_wpos, w + n, __ATOMIC_RELEASE);
        if (s_listener) xTaskNotifyGive(s_listener);
    }
}

void mic_start(void)
{
    /* PSRAM jesli jest (48 KB nie musi byc w szybkim RAM - zapis co 20 ms) */
    s_hist = (int16_t *)heap_caps_calloc_prefer(MIC_HIST_SAMPLES, sizeof(int16_t), 2,
                                                MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    configASSERT(s_hist);
    xTaskCreatePinnedToCore(mic_task, "mic", 4096, NULL, 7, NULL, 1);
    ESP_LOGI(TAG, "historia mikrofonu %d ms", MIC_HIST_SAMPLES / 16);
}
