/*
 * wake.c - task slowa kluczowego (rdzen 1): co 20 ms porcja z historii mikrofonu -> kws_det_hop.
 * Wagi modelu czytane wprost z partycji flash (esp_partition_mmap) - zero RAM na parametry.
 */
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "wake.h"
#include "mic.h"
#include "audio.h"
#include "audio_net.h"
#include "audio_proto.h"
#include "kws_detect.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static const char *TAG = "wake";

#define HOP KWS_HOP
#define MS2S(ms) ((uint32_t)(ms) * 16u)

static SemaphoreHandle_t s_lock;               /* model / detektor */
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;   /* stan polecenia */

static const esp_partition_t *s_part;
static esp_partition_mmap_handle_t s_mh;
static const void *s_map;
static nn_model    s_model;
static kws_meta    s_meta;
static kws_det    *s_det;
static bool        s_ok;
static float       s_thr_user;                 /* 0 = prog z modelu */
static volatile float s_score;

static volatile wake_state_t s_state;
static wake_cmd_t  s_cmd;
static volatile bool s_req_listen, s_req_cancel;
static volatile uint32_t s_ver = 1;
static char        s_event[96] = "start";

static uint32_t    s_up_size, s_up_off;
static bool        s_uploading;

/* ---------------------------------------------------------------- dostep */
bool wake_model_ok(void) { return s_ok; }
const char *wake_keyword(void) { return s_ok ? s_meta.keyword : ""; }
float wake_default_threshold(void) { return s_ok ? s_meta.threshold : 0; }
float wake_threshold(void) { return s_thr_user > 0 ? s_thr_user : wake_default_threshold(); }
float wake_score(void) { return s_score; }
wake_state_t wake_state(void) { return s_state; }
uint32_t wake_version(void) { return s_ver; }
const char *wake_last_event(void) { return s_event; }

void wake_cmd(wake_cmd_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_cmd;
    portEXIT_CRITICAL(&s_mux);
}

void wake_cmd_done(void)
{
    portENTER_CRITICAL(&s_mux);
    s_state = WAKE_LISTEN;
    portEXIT_CRITICAL(&s_mux);
    s_ver++;
}

void wake_listen_now(void) { s_req_listen = true; }
void wake_cancel(void) { s_req_cancel = true; }

static void apply_threshold(void)
{
    if (s_det) s_det->thr = wake_threshold();
}

void wake_set_threshold(float t)
{
    if (t < 0) t = 0;
    if (t >= 1) t = 0.99f;
    s_thr_user = t;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    apply_threshold();
    xSemaphoreGive(s_lock);
    nvs_handle_t h;
    if (nvs_open("kws", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "thr", (uint8_t)(t * 255 + 0.5f));
        nvs_commit(h);
        nvs_close(h);
    }
    s_ver++;
}

/* ---------------------------------------------------------------- model */
static void model_unload(void)                  /* pod s_lock */
{
    s_ok = false;
    if (s_det) { kws_det_free(s_det); free(s_det); s_det = NULL; }
    nn_free(&s_model);
    if (s_map) { esp_partition_munmap(s_mh); s_map = NULL; }
}

static int model_load(char *msg, int msgsz)     /* pod s_lock */
{
    if (!s_part) { snprintf(msg, msgsz, "BLAD brak partycji kws"); return -1; }
    if (esp_partition_mmap(s_part, 0, s_part->size, ESP_PARTITION_MMAP_DATA, &s_map, &s_mh) != ESP_OK) {
        s_map = NULL;
        snprintf(msg, msgsz, "BLAD mmap partycji kws");
        return -1;
    }
    int rc = kws_model_parse(s_map, s_part->size, &s_model, &s_meta, 0);
    if (rc) {
        snprintf(msg, msgsz, "BLAD brak poprawnego modelu w partycji (%d)", rc);
        esp_partition_munmap(s_mh); s_map = NULL;
        return -1;
    }
    s_det = (kws_det *)malloc(sizeof(kws_det));
    if (!s_det || kws_det_init(s_det, &s_model, &s_meta)) {
        free(s_det); s_det = NULL;
        model_unload();
        snprintf(msg, msgsz, "BLAD brak pamieci na detektor");
        return -1;
    }
    apply_threshold();
    s_ok = true;
    snprintf(msg, msgsz, "OK model \"%s\", %d parametrow, %.2f mln MAC, prog %.2f",
             s_meta.keyword, s_model.nparams, nn_macs(&s_model) / 1e6, wake_threshold());
    return 0;
}

int wake_model_begin(uint32_t size, char *msg, int msgsz)
{
    if (!s_part) { snprintf(msg, msgsz, "BLAD brak partycji kws"); return -1; }
    if (size < 64 || size > s_part->size) { snprintf(msg, msgsz, "BLAD rozmiar %u (partycja %u)", (unsigned)size, (unsigned)s_part->size); return -1; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    model_unload();
    uint32_t er = (size + 4095) & ~4095u;
    esp_err_t e = esp_partition_erase_range(s_part, 0, er);
    xSemaphoreGive(s_lock);
    if (e != ESP_OK) { snprintf(msg, msgsz, "BLAD kasowania %s", esp_err_to_name(e)); return -1; }
    s_up_size = size; s_up_off = 0; s_uploading = true;
    snprintf(s_event, sizeof s_event, "wgrywanie modelu...");
    s_ver++;
    return 0;
}

int wake_model_data(const uint8_t *d, int n, char *msg, int msgsz)
{
    if (!s_uploading || s_up_off + (uint32_t)n > s_up_size) { snprintf(msg, msgsz, "BLAD nieoczekiwane dane modelu"); s_uploading = false; return -1; }
    esp_err_t e = esp_partition_write(s_part, s_up_off, d, n);
    if (e != ESP_OK) { snprintf(msg, msgsz, "BLAD zapisu %s", esp_err_to_name(e)); s_uploading = false; return -1; }
    s_up_off += n;
    return 0;
}

int wake_model_end(uint32_t crc, char *msg, int msgsz)
{
    if (!s_uploading || s_up_off != s_up_size) { snprintf(msg, msgsz, "BLAD niekompletny model (%u/%u)", (unsigned)s_up_off, (unsigned)s_up_size); s_uploading = false; return -1; }
    s_uploading = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int rc = model_load(msg, msgsz);
    if (rc == 0 && kws_crc32(s_map, s_up_size) != crc) { model_unload(); snprintf(msg, msgsz, "BLAD CRC po zapisie"); rc = -1; }
    xSemaphoreGive(s_lock);
    snprintf(s_event, sizeof s_event, rc ? "wgrywanie nieudane" : "nowy model wgrany");
    ESP_LOGI(TAG, "%s", msg);
    s_ver++;
    return rc;
}

/* ---------------------------------------------------------------- polecenie */
static void beep(void)
{
#if CONFIG_RSX_KWS_BEEP
    static int16_t t[1600];                          /* 100 ms, 1 kHz, lagodne zbocza */
    for (int i = 0; i < 1600; i++) {
        float env = i < 160 ? i / 160.0f : (i > 1440 ? (1600 - i) / 160.0f : 1.0f);
        t[i] = (int16_t)(6000.0f * env * sinf(2.0f * 3.14159265f * 1000.0f * i / 16000.0f));
    }
    audio_write_spk(t, sizeof t);
#endif
}

static void start_cmd(uint32_t pos, uint8_t score, uint32_t preroll)
{
    int deg = -1, en;
    if (!audio_get_doa(&deg, &en)) deg = -1;
    if (audio_net_clients() == 0) {
        snprintf(s_event, sizeof s_event, "wykryto (%.2f) - brak hosta, nie wysylam", score / 255.0f);
        ESP_LOGI(TAG, "%s", s_event);
        s_ver++;
        beep();
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s_cmd.seq++;
    s_cmd.start = pos > preroll ? pos - preroll : 0;
    s_cmd.end = 0;
    s_cmd.reason = 0;
    s_cmd.score = score;
    s_cmd.doa = deg;
    s_state = WAKE_CMD;
    portEXIT_CRITICAL(&s_mux);
    snprintf(s_event, sizeof s_event, "wykryto (%.2f) - nagrywam polecenie", score / 255.0f);
    ESP_LOGI(TAG, "%s, kierunek %d", s_event, deg);
    s_ver++;
    beep();
}

static void end_cmd(uint32_t pos, int reason)
{
    portENTER_CRITICAL(&s_mux);
    if (s_state == WAKE_CMD && s_cmd.end == 0) { s_cmd.end = pos; s_cmd.reason = reason; }
    portEXIT_CRITICAL(&s_mux);
    static const char *why[] = { "cisza", "limit czasu", "brak mowy", "host" };
    snprintf(s_event, sizeof s_event, "koniec polecenia (%s)", why[reason & 3]);
    s_ver++;
}

static float frame_db(const int16_t *x)
{
    double e = 0;
    for (int i = 0; i < HOP; i++) e += (double)x[i] * x[i];
    return (float)(10.0 * log10(e / HOP + 1.0));
}

static void kws_task(void *arg)
{
    (void)arg;
    int16_t hop[HOP];
    uint32_t pos = mic_wpos();
    float nf = 0;
    bool nf_init = false, heard = false;
    int sil = 0, nspeech = 0;
    int64_t t_inf = 0;
    int n_inf = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        for (;;) {
            uint32_t w = mic_wpos();
            if (w - pos < HOP) break;
            if (w - pos > MIC_HIST_SAMPLES - HOP) pos = w - HOP;   /* nie nadazamy - przeskok */
            if (mic_read(pos, hop, HOP) != HOP) { pos = mic_wpos(); break; }
            pos += HOP;

            int det = 0, ran = 0;
            float e;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_det) {
                int64_t t0 = esp_timer_get_time();
                det = kws_det_hop(s_det, hop, &ran);
                if (ran) { t_inf += esp_timer_get_time() - t0; n_inf++; s_score = s_det->score; }
                e = s_det->energy_db;
            } else {
                e = frame_db(hop);
            }
            float sc = s_det ? s_det->score : 0;
            xSemaphoreGive(s_lock);
            if (n_inf == 500) {
                ESP_LOGI(TAG, "siec: srednio %.1f ms na wnioskowanie", t_inf / 500.0 / 1000.0);
                t_inf = 0; n_inf = 0;
            }

            /* poziom szumu i mowa (jak VAD hosta) */
            if (!nf_init) { nf = e; nf_init = true; }
            bool sp = e > nf + 9.0f && e > 30.0f;
            if (!sp) nf = e < nf ? 0.7f * nf + 0.3f * e : 0.98f * nf + 0.02f * e;

            if (s_state == WAKE_LISTEN) {
                if (s_req_listen) {
                    s_req_listen = false;
                    heard = false; sil = 0; nspeech = 0;
                    start_cmd(pos, 255, 0);
                } else if (det) {
                    heard = false; sil = 0; nspeech = 0;
                    start_cmd(pos, (uint8_t)(sc * 255), MS2S(CONFIG_RSX_KWS_PREROLL_MS));
                }
                s_req_cancel = false;
                continue;
            }
            wake_cmd_t c;
            wake_cmd(&c);
            if (c.end) {                                 /* czekamy, az audio_net dosle reszte */
                if (audio_net_clients() == 0) wake_cmd_done();
                continue;
            }
            if (sp) { if (++nspeech >= 3) heard = true; sil = 0; }
            else { nspeech = 0; sil += 20; }
            uint32_t ms = (pos - c.start) / 16;
            if (s_req_cancel) { s_req_cancel = false; end_cmd(pos, AF_END_HOST); }
            else if (heard && sil >= CONFIG_RSX_KWS_END_SIL_MS) end_cmd(pos - MS2S(sil > 200 ? sil - 200 : 0), AF_END_SILENCE);
            else if (!heard && ms >= CONFIG_RSX_KWS_NOSPEECH_MS) end_cmd(pos, AF_END_NOSPEECH);
            else if (ms >= CONFIG_RSX_KWS_MAX_MS) end_cmd(pos, AF_END_MAXTIME);
        }
    }
}

void wake_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    nvs_handle_t h;
    uint8_t thr = 0;
    if (nvs_open("kws", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "thr", &thr); nvs_close(h); }
    s_thr_user = thr / 255.0f;

    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "kws");
    char msg[128];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    model_load(msg, sizeof msg);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s", msg);
    snprintf(s_event, sizeof s_event, "%s", s_ok ? "nasluchuje" : "brak modelu - kwstool upload");

    TaskHandle_t t;
    xTaskCreatePinnedToCore(kws_task, "kws", 8192, NULL, 5, &t, 1);
    mic_set_listener(t);
}
