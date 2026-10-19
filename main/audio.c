/*
 * audio.c - I2S full-duplex z XVF3800 + konwersja miedzy formatem magistrali
 * (48 kHz / 32-bit / stereo - firmware XVF3800 pod Home Assistant) a formatem
 * strumienia sieciowego / AI (16 kHz / mono / 16-bit, s16le).
 *
 *  - RX (mikrofon): I2S 48k/stereo/32b -> wybor kanalu -> decymacja x DECIM
 *                   (48000/16000 = 3) -> mono s16le 16 kHz;
 *  - TX (glosnik):  mono s16le 16 kHz od hosta -> interpolacja x DECIM ->
 *                   duplikacja na kanaly -> I2S 48k/stereo/32b.
 * Glosnosc i wzmocnienie dzialaja programowo (zawsze) oraz przez XVF3800 (best-effort).
 */
#include <string.h>
#include <math.h>
#include "audio.h"
#include "xvf3800.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "audio";

/* ---- format magistrali I2S (to, co zegaruje XVF3800) ---- */
#if CONFIG_RSX_I2S_BITS32
  #define SLOT_BITS I2S_DATA_BIT_WIDTH_32BIT
  #define SLOT32    1
#else
  #define SLOT_BITS I2S_DATA_BIT_WIDTH_16BIT
  #define SLOT32    0
#endif
#if CONFIG_RSX_I2S_CHANNELS >= 2
  #define SLOT_MODE I2S_SLOT_MODE_STEREO
  #define WIRE_CH   2
#else
  #define SLOT_MODE I2S_SLOT_MODE_MONO
  #define WIRE_CH   1
#endif
#if CONFIG_RSX_I2S_SLAVE
  #define I2S_ROLE  I2S_ROLE_SLAVE
#else
  #define I2S_ROLE  I2S_ROLE_MASTER
#endif

/* ---- redukcja do strumienia (mono, nizsza czestotliwosc) ---- */
#define WIRE_RATE CONFIG_RSX_I2S_RATE
#if CONFIG_RSX_STREAM_RATE > 0 && (WIRE_RATE / CONFIG_RSX_STREAM_RATE) >= 1
  #define DECIM (WIRE_RATE / CONFIG_RSX_STREAM_RATE)
#else
  #define DECIM 1
#endif
#define STREAM_RATE (WIRE_RATE / DECIM)     /* faktyczna czestotliwosc strumienia */

#define OUT_BLK 64                          /* probek strumienia na iteracje (mikrofon) */
#define SPK_BLK 64                          /* probek strumienia na iteracje (glosnik) */

/* Bufor glosnika (jitter buffer) miedzy siecia a I2S - jak raw_stream 8 KB w projekcie
 * Seeed ESP32S3_reSpeaker_agora. Siec wrzuca PCM, osobny task odtwarza go rowno. */
#define SPK_BUF_MS    1000                  /* pojemnosc bufora */
#define SPK_PREBUF_MS 200                   /* tyle zbieramy przed startem / po niedoborze */
#define SPK_TAIL_MS   100                   /* brak nowych danych tak dlugo = koncowka, graj reszte */
#define SPK_BYTES(ms) ((STREAM_RATE * 2 / 1000) * (ms))

static i2s_chan_handle_t s_tx, s_rx;
static volatile int      s_volume = 60;     /* 0..100 */
static volatile int      s_mic_gain = 0;    /* dB */
static volatile float    s_vol_f = 0.60f;
static volatile float    s_gain_f = 1.0f;
static volatile uint32_t s_ver = 1;
static int16_t           s_spk_prev = 0;    /* ostatnia probka glosnika (do interpolacji) */
static StreamBufferHandle_t s_spk_sb;       /* PCM mono s16 @ STREAM_RATE czekajacy na I2S */
static volatile TickType_t  s_spk_last_push; /* kiedy siec ostatnio dorzucila dane */

int audio_sample_rate(void) { return STREAM_RATE; }
int audio_channels(void)    { return 1; }   /* strumien zawsze mono (wejscie AI) */
uint32_t audio_settings_version(void) { return s_ver; }

static int16_t clamp16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* jedna ramka I2S (WIRE_CH slotow) -> jedna probka mono w skali 16-bit */
#if SLOT32
static inline int32_t frame_to_mono(const int32_t *f)
{
  #if WIRE_CH >= 2
    #if CONFIG_RSX_MIC_MIX
        return ((f[0] >> 16) + (f[1] >> 16)) / 2;
    #elif CONFIG_RSX_MIC_RIGHT
        return f[1] >> 16;
    #else
        return f[0] >> 16;
    #endif
  #else
    return f[0] >> 16;
  #endif
}
#else
static inline int32_t frame_to_mono(const int16_t *f)
{
  #if WIRE_CH >= 2
    #if CONFIG_RSX_MIC_MIX
        return ((int32_t)f[0] + f[1]) / 2;
    #elif CONFIG_RSX_MIC_RIGHT
        return f[1];
    #else
        return f[0];
    #endif
  #else
    return f[0];
  #endif
}
#endif

static void spk_task(void *arg);

void audio_init(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE);
    cc.dma_desc_num = 6;
    cc.dma_frame_num = 480;                 /* 6 x 10 ms @48k - wiecej zapasu niz 6 x 5 ms */
    /* Przy braku danych wysylaj CISZE. Domyslnie (false) DMA powtarza w kolko stare
     * bufory - slychac to jako poglos/echo i trzaski przy kazdym niedoborze. */
    cc.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&cc, &s_tx, &s_rx));

    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(WIRE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(SLOT_BITS, SLOT_MODE),
        .gpio_cfg = {
            .mclk = CONFIG_RSX_I2S_MCLK_GPIO < 0 ? I2S_GPIO_UNUSED : CONFIG_RSX_I2S_MCLK_GPIO,
            .bclk = CONFIG_RSX_I2S_BCLK_GPIO,
            .ws   = CONFIG_RSX_I2S_WS_GPIO,
            .dout = CONFIG_RSX_I2S_DOUT_GPIO < 0 ? I2S_GPIO_UNUSED : CONFIG_RSX_I2S_DOUT_GPIO,
            .din  = CONFIG_RSX_I2S_DIN_GPIO,
            .invert_flags = { 0 },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));

    ESP_LOGI(TAG, "I2S %s magistrala %d Hz / %d-bit / %d kan.  ->  strumien %d Hz mono (decym. x%d)",
             I2S_ROLE == I2S_ROLE_MASTER ? "master" : "slave", WIRE_RATE,
             SLOT32 ? 32 : 16, WIRE_CH, STREAM_RATE, DECIM);
    ESP_LOGI(TAG, "piny: BCLK=%d WS=%d DIN=%d DOUT=%d MCLK=%d",
             CONFIG_RSX_I2S_BCLK_GPIO, CONFIG_RSX_I2S_WS_GPIO, CONFIG_RSX_I2S_DIN_GPIO,
             CONFIG_RSX_I2S_DOUT_GPIO, CONFIG_RSX_I2S_MCLK_GPIO);

#if CONFIG_RSX_I2S_DOUT_GPIO >= 0
    /* bufor glosnika w PSRAM, jesli jest (zwalnia wewnetrzny RAM dla sieci slowa kluczowego) */
    static StaticStreamBuffer_t sb;
    size_t sbn = SPK_BYTES(SPK_BUF_MS);
    uint8_t *mem = heap_caps_malloc_prefer(sbn + 1, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    configASSERT(mem);
    s_spk_sb = xStreamBufferCreateStatic(sbn, 2, mem, &sb);
    configASSERT(s_spk_sb);
    xTaskCreate(spk_task, "spk", 4096, NULL, 6, NULL);
    ESP_LOGI(TAG, "glosnik: bufor %d ms, prebufor %d ms", SPK_BUF_MS, SPK_PREBUF_MS);
#endif
}

/* Mikrofon: zwraca mono s16le @ STREAM_RATE. Czyta DECIM ramek I2S na probke wyjsciowa
 * i usrednia je (prosty filtr decymujacy, lagodzi aliasing). */
int audio_read_mic(void *dst, int maxbytes, int timeout_ms)
{
    int16_t *out = (int16_t *)dst;
    int outcount = maxbytes / 2;              /* probek strumienia do oddania */
    int done = 0;
    float g = s_gain_f;
    TickType_t to = pdMS_TO_TICKS(timeout_ms);

    while (done < outcount) {
        int want = outcount - done;
        if (want > OUT_BLK) want = OUT_BLK;
        int frames = want * DECIM;             /* ramek I2S do odczytania */
        size_t br = 0;
#if SLOT32
        int32_t buf[OUT_BLK * DECIM * WIRE_CH];
        if (i2s_channel_read(s_rx, buf, (size_t)frames * WIRE_CH * 4, &br, to) != ESP_OK) break;
        int gotframes = (int)(br / 4) / WIRE_CH;
#else
        int16_t buf[OUT_BLK * DECIM * WIRE_CH];
        if (i2s_channel_read(s_rx, buf, (size_t)frames * WIRE_CH * 2, &br, to) != ESP_OK) break;
        int gotframes = (int)(br / 2) / WIRE_CH;
#endif
        int outs = gotframes / DECIM;
        for (int o = 0; o < outs; o++) {
            int32_t acc = 0;
            for (int j = 0; j < DECIM; j++)
                acc += frame_to_mono(&buf[(o * DECIM + j) * WIRE_CH]);
            acc /= DECIM;
            out[done + o] = clamp16((int32_t)(acc * g));
        }
        done += outs;
        if (gotframes < frames) break;         /* timeout / czesciowy odczyt */
    }
    return done * 2;
}

#if CONFIG_RSX_I2S_DOUT_GPIO >= 0
/* Mono s16 @ STREAM_RATE -> interpolacja liniowa x DECIM -> kanaly I2S @ WIRE_RATE.
 * Blokuje na i2s_channel_write, wiec to I2S wyznacza tempo odtwarzania. */
static void spk_out(const int16_t *in, int incount)
{
    int done = 0;
    float v = s_vol_f;
    int prev = s_spk_prev;

    while (done < incount) {
        int want = incount - done;
        if (want > SPK_BLK) want = SPK_BLK;
        size_t bw = 0;
#if SLOT32
        int32_t buf[SPK_BLK * DECIM * WIRE_CH];
#else
        int16_t buf[SPK_BLK * DECIM * WIRE_CH];
#endif
        int bi = 0;
        for (int i = 0; i < want; i++) {
            int cur = in[done + i];
            for (int k = 0; k < DECIM; k++) {
                int interp = prev + (cur - prev) * k / DECIM;   /* liniowo prev->cur */
                int16_t s = clamp16((int32_t)(interp * v));
#if SLOT32
                int32_t w = ((int32_t)s) << 16;
                buf[bi++] = w;
  #if WIRE_CH >= 2
                buf[bi++] = w;
  #endif
#else
                buf[bi++] = s;
  #if WIRE_CH >= 2
                buf[bi++] = s;
  #endif
#endif
            }
            prev = cur;
        }
#if SLOT32
        if (i2s_channel_write(s_tx, buf, (size_t)bi * 4, &bw, pdMS_TO_TICKS(200)) != ESP_OK) break;
#else
        if (i2s_channel_write(s_tx, buf, (size_t)bi * 2, &bw, pdMS_TO_TICKS(200)) != ESP_OK) break;
#endif
        done += want;
    }
    s_spk_prev = (int16_t)prev;
}

/* Odtwarzanie z bufora. Start dopiero po zebraniu SPK_PREBUF_MS (albo gdy siec przestala
 * dosylac - koncowka krotkiego nagrania). Po niedoborze wraca do zbierania; I2S w tym czasie
 * gra cisze (auto_clear), zamiast powtarzac stare bufory. */
static void spk_task(void *arg)
{
    (void)arg;
    int16_t blk[SPK_BLK];
    bool playing = false;
    for (;;) {
        if (!playing) {
            size_t avail = xStreamBufferBytesAvailable(s_spk_sb);
            bool tail = avail > 0 &&
                        (xTaskGetTickCount() - s_spk_last_push) > pdMS_TO_TICKS(SPK_TAIL_MS);
            if (avail < SPK_BYTES(SPK_PREBUF_MS) && !tail) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            playing = true;
        }
        size_t n = xStreamBufferReceive(s_spk_sb, blk, sizeof blk, pdMS_TO_TICKS(20));
        if (n < 2) {                                  /* niedobor / koniec - czekamy na nowe dane */
            playing = false;
            s_spk_prev = 0;                           /* nastepny start lagodnie od zera */
            continue;
        }
        spk_out(blk, (int)(n / 2));
    }
}
#endif

/* Glosnik: dokleja mono s16le @ STREAM_RATE do bufora. Gdy bufor pelny - czeka (do 1 s),
 * co przez TCP spowalnia nadawce (naturalna kontrola przeplywu). */
int audio_write_spk(const void *src, int bytes)
{
#if CONFIG_RSX_I2S_DOUT_GPIO < 0
    (void)src; return bytes;                          /* brak wyjscia */
#else
    bytes &= ~1;                                      /* tylko pelne probki */
    size_t sent = xStreamBufferSend(s_spk_sb, src, (size_t)bytes, pdMS_TO_TICKS(1000));
    s_spk_last_push = xTaskGetTickCount();
    return (int)sent;
#endif
}

void audio_set_volume(int pct)
{
    if (pct < 0) pct = 0; else if (pct > 100) pct = 100;
    s_volume = pct;
    s_vol_f = (float)pct / 100.0f;
    s_ver++;
    xvf_set_volume(pct);                            /* best-effort */
}
int audio_get_volume(void) { return s_volume; }

void audio_set_mic_gain(int db)
{
    if (db < -12) db = -12; else if (db > 36) db = 36;
    s_mic_gain = db;
    s_gain_f = powf(10.0f, (float)db / 20.0f);
    s_ver++;
    xvf_set_mic_gain(db);                            /* best-effort */
}
int audio_get_mic_gain(void) { return s_mic_gain; }

bool audio_get_doa(int *deg, int *energy)
{
#if CONFIG_RSX_XVF_DOA
    return xvf_get_doa(deg, energy) == ESP_OK;
#else
    (void)deg; (void)energy; return false;
#endif
}
