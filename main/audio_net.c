/*
 * audio_net.c - serwer TCP strumienia audio. Patrz audio_proto.h.
 * Petla select: odbiera ramki od hosta (PCM do glosnika, glosnosc, gain, rec, sluchaj, prog,
 * model) i wysyla:
 *   - po slowie kluczowym (wake.c): AF_WAKE, AF_MIC z poleceniem (z historii mikrofonu,
 *     lacznie z krotkim prebuforem), AF_END;
 *   - przy AF_REC = 1: staly strumien AF_MIC (nagrywanie probek, stare narzedzia);
 *   - okresowo AF_DOA, gdy cos wysylamy.
 */
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "audio_net.h"
#include "audio_proto.h"
#include "audio.h"
#include "mic.h"
#include "wake.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

static const char *TAG = "audio_net";

#define MIC_CHUNK     640             /* probek na ramke AF_MIC: 40 ms @16k */
#define DOA_PERIOD_MS 200

static volatile int s_clients;

int audio_net_clients(void) { return s_clients; }

static int send_all(int s, const void *buf, int len)
{
    const uint8_t *p = buf;
    while (len > 0) {
        int n = (int)send(s, p, (size_t)len, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) return -1;
        p += n; len -= n;
    }
    return 0;
}

static int send_frame(int s, uint8_t type, const void *data, int len)
{
    uint8_t hdr[3] = { type, (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF) };
    if (send_all(s, hdr, 3) < 0) return -1;
    if (len > 0 && send_all(s, data, len) < 0) return -1;
    return 0;
}

static int send_text(int s, const char *t) { return send_frame(s, AF_STATUS, t, (int)strlen(t)); }

static int send_hello(int s)
{
    uint32_t sr = (uint32_t)audio_sample_rate();
    uint8_t h[AUDIO_HELLO_LEN];
    memcpy(h, AUDIO_MAGIC, 4);
    h[4] = AUDIO_PROTO_VERSION;
    h[5] = (uint8_t)audio_channels();
    h[6] = AUDIO_BITS;
    h[7] = AUDIO_FL_KWS | AUDIO_FL_RECONLY | (wake_model_ok() ? AUDIO_FL_MODEL : 0);
    h[8] = (uint8_t)(sr & 0xFF);
    h[9] = (uint8_t)((sr >> 8) & 0xFF);
    h[10] = (uint8_t)((sr >> 16) & 0xFF);
    h[11] = (uint8_t)((sr >> 24) & 0xFF);
    return send_all(s, h, sizeof h);
}

typedef struct {
    bool     rec;            /* staly strumien */
    bool     rec_only;       /* AF_REC_ONLY: polecen po slowie nie dokladamy do strumienia */
    uint32_t rec_pos;
    bool     cmd;            /* wysylamy polecenie */
    uint32_t cmd_pos, seq;
} cli_t;

/* obsluga jednej kompletnej ramki od hosta; -1 = zrywamy polaczenie */
static int on_frame(int s, cli_t *c, uint8_t type, const uint8_t *d, int len)
{
    char msg[160];
    switch (type) {
    case AF_PLAY:
        audio_write_spk(d, len);
        break;
    case AF_VOLUME:
        if (len >= 1) audio_set_volume(d[0]);
        break;
    case AF_GAIN:
        if (len >= 1) audio_set_mic_gain((int8_t)d[0]);
        break;
    case AF_REC:
        if (len >= 1) {
            c->rec = d[0] != 0;
            c->rec_only = d[0] == AF_REC_ONLY;
            c->rec_pos = mic_wpos();
            ESP_LOGI(TAG, "staly strumien mikrofonu %s%s", c->rec ? "wlaczony" : "wylaczony",
                     c->rec_only ? " (bez polecen)" : "");
        }
        break;
    case AF_LISTEN:
        if (len >= 1) { if (d[0]) wake_listen_now(); else wake_cancel(); }
        break;
    case AF_THR:
        if (len >= 1) wake_set_threshold(d[0] / 255.0f);
        break;
    case AF_MODEL:
        if (len < 1) break;
        if (d[0] == AF_MODEL_BEGIN && len >= 5) {
            uint32_t sz; memcpy(&sz, d + 1, 4);
            if (wake_model_begin(sz, msg, sizeof msg)) return send_text(s, msg);
        } else if (d[0] == AF_MODEL_DATA) {
            if (wake_model_data(d + 1, len - 1, msg, sizeof msg)) return send_text(s, msg);
        } else if (d[0] == AF_MODEL_END && len >= 5) {
            uint32_t crc; memcpy(&crc, d + 1, 4);
            wake_model_end(crc, msg, sizeof msg);
            return send_text(s, msg);
        }
        break;
    default:
        break;                                    /* nieznane typy (np. AF_PING) pomijamy */
    }
    return 0;
}

/* wysyla PCM z historii od *pos do limit (max kilka ramek naraz); -1 = blad gniazda */
static int pump(int s, uint32_t *pos, uint32_t limit)
{
    static int16_t buf[MIC_CHUNK];
    for (int k = 0; k < 8 && (int32_t)(limit - *pos) > 0; k++) {
        int want = (int)(limit - *pos);
        if (want > MIC_CHUNK) want = MIC_CHUNK;
        int n = mic_read(*pos, buf, want);
        if (n < 0) { *pos = mic_wpos(); ESP_LOGW(TAG, "siec nie nadaza - przeskok w historii"); return 0; }
        if (n == 0) return 0;
        if (send_frame(s, AF_MIC, buf, n * 2) < 0) return -1;
        *pos += n;
    }
    return 0;
}

static void handle_client(int cs)
{
    s_clients = 1;
    setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, &(int){ 1 }, sizeof(int));
    if (send_hello(cs) < 0) { s_clients = 0; closesocket(cs); return; }

    cli_t c = { 0 };
    wake_cmd_t wc;
    wake_cmd(&wc);
    c.seq = wc.seq;                               /* polecenia sprzed polaczenia pomijamy */
    static uint8_t rbuf[3 + AUDIO_MAX_PAYLOAD];
    int rlen = 0;
    int64_t next_doa = 0;

    for (;;) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(cs, &rf);
        bool busy = c.rec || c.cmd;
        struct timeval tv = { 0, (busy ? 5 : 20) * 1000 };
        int n = select(cs + 1, &rf, NULL, NULL, &tv);
        if (n < 0 && errno != EINTR) break;

        if (n > 0 && FD_ISSET(cs, &rf)) {
            int r = (int)recv(cs, rbuf + rlen, (size_t)(sizeof rbuf - rlen), 0);
            if (r == 0) break;
            if (r < 0) { if (errno == EAGAIN || errno == EINTR) r = 0; else break; }
            rlen += r;
            int off = 0, err = 0;
            while (rlen - off >= 3) {
                int plen = rbuf[off + 1] | (rbuf[off + 2] << 8);
                if (plen > AUDIO_MAX_PAYLOAD) { off = rlen; break; }
                if (rlen - off < 3 + plen) break;
                if (on_frame(cs, &c, rbuf[off], rbuf + off + 3, plen) < 0) { err = 1; break; }
                off += 3 + plen;
            }
            if (err) break;
            if (off > 0) { memmove(rbuf, rbuf + off, (size_t)(rlen - off)); rlen -= off; }
        }

        /* nowe polecenie po slowie kluczowym */
        wake_cmd(&wc);
        if (!c.cmd && wc.seq != c.seq && wake_state() == WAKE_CMD) {
            c.seq = wc.seq;
            c.cmd = true;
            c.cmd_pos = wc.start;
            const char *kw = wake_keyword();
            uint8_t w[3 + 32];
            uint16_t doa = wc.doa < 0 ? 0xFFFF : (uint16_t)wc.doa;
            int kl = (int)strlen(kw);
            if (kl > 32) kl = 32;
            w[0] = wc.score; w[1] = (uint8_t)(doa & 0xFF); w[2] = (uint8_t)(doa >> 8);
            memcpy(w + 3, kw, kl);
            if (send_frame(cs, AF_WAKE, w, 3 + kl) < 0) break;
            ESP_LOGI(TAG, "AF_WAKE -> host, polecenie od %u", (unsigned)wc.start);
        }
        if (c.cmd) {
            uint32_t limit = wc.end ? wc.end : mic_wpos();
            if (c.rec && c.rec_only) c.cmd_pos = limit;   /* polecenie juz jest w stalym strumieniu */
            else if (pump(cs, &c.cmd_pos, limit) < 0) break;
            if (wc.end && (int32_t)(wc.end - c.cmd_pos) <= 0) {
                uint8_t why = (uint8_t)wc.reason;
                if (send_frame(cs, AF_END, &why, 1) < 0) break;
                c.cmd = false;
                wake_cmd_done();
                ESP_LOGI(TAG, "AF_END (%d)", wc.reason);
            }
        }
        if (c.rec && pump(cs, &c.rec_pos, mic_wpos()) < 0) break;

        if (c.rec || c.cmd) {
            int64_t t = esp_timer_get_time() / 1000;
            if (t >= next_doa) {
                next_doa = t + DOA_PERIOD_MS;
                int deg, en;
                if (audio_get_doa(&deg, &en)) {
                    uint8_t dd[3] = { (uint8_t)(deg & 0xFF), (uint8_t)((deg >> 8) & 0xFF), (uint8_t)en };
                    if (send_frame(cs, AF_DOA, dd, 3) < 0) break;
                }
            }
        }
    }
    if (c.cmd) wake_cmd_done();
    s_clients = 0;
    closesocket(cs);
    ESP_LOGI(TAG, "polaczenie audio zakonczone");
}

static void serve(uint16_t port)
{
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &(int){ 1 }, sizeof(int));
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 1) < 0) {
        ESP_LOGE(TAG, "bind/listen portu %u: %d", port, errno);
        closesocket(ls);
        return;
    }
    ESP_LOGI(TAG, "strumien audio nasluchuje na porcie TCP %u", port);
    for (;;) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof ca;
        int cs = accept(ls, (struct sockaddr *)&ca, &cl);
        if (cs < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        ESP_LOGI(TAG, "polaczenie audio z %s:%u", inet_ntoa(ca.sin_addr), ntohs(ca.sin_port));
        handle_client(cs);
    }
}

static void audio_net_task(void *arg)
{
    (void)arg;
    serve(CONFIG_RSX_AUDIO_PORT);
    vTaskDelete(NULL);
}

void audio_net_start(void)
{
    xTaskCreate(audio_net_task, "audio_net", 8192, NULL, 5, NULL);
}
