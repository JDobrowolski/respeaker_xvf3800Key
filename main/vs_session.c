/*
 * vs_session.c - serwer TCP sesji wektorowej (select: gniazdo nasluchu + biezace polaczenie),
 * skladanie ramek vsproto i przekazywanie ich do vs_app.c.
 */
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "vs_session.h"
#include "vsproto.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "vs_session";

#define VS_RBUF   4096          /* UI wysyla male ramki; wieksze sa pomijane */
#define TICK_MS   250

struct vs_conn {
    int      sock;
    bool     closed;
    int      rlen;
    long     skip;
    SemaphoreHandle_t tx_lock;
    uint8_t  rbuf[VS_RBUF];
};

int vs_conn_send(vs_conn_t *c, const uint8_t *buf, int len)
{
    xSemaphoreTake(c->tx_lock, portMAX_DELAY);
    while (len > 0 && !c->closed) {
        int n = (int)send(c->sock, buf, (size_t)len, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) { c->closed = true; break; }
        buf += n; len -= n;
    }
    int rc = c->closed ? -1 : 0;
    xSemaphoreGive(c->tx_lock);
    return rc;
}

static void process_frames(vs_conn_t *c, void *app)
{
    int off = 0;
    for (;;) {
        if (c->skip) {
            long n = c->rlen - off < c->skip ? c->rlen - off : c->skip;
            off += (int)n; c->skip -= n;
            if (c->skip) break;
        }
        if (c->rlen - off < 2) break;
        int size = c->rbuf[off] | (c->rbuf[off + 1] << 8);
        if (2 + size > VS_RBUF) {
            ESP_LOGW(TAG, "ramka %d B za duza - pomijam", size);
            c->skip = 2 + size;
            continue;
        }
        int type, dlen;
        const uint8_t *d;
        int used = vs_frame(c->rbuf + off, c->rlen - off, &type, &d, &dlen);
        if (!used) break;
        off += used;
        if (used >= 3) vs_app_frame(app, type, d, dlen);
        if (c->closed) break;
    }
    if (off) {
        memmove(c->rbuf, c->rbuf + off, (size_t)(c->rlen - off));
        c->rlen -= off;
    }
}

static void conn_end(vs_conn_t **pc, void **papp)
{
    vs_conn_t *c = *pc;
    if (!c) return;
    c->closed = true;
    if (*papp) vs_app_close(*papp);
    closesocket(c->sock);
    vSemaphoreDelete(c->tx_lock);
    free(c);
    *pc = NULL;
    *papp = NULL;
    ESP_LOGI(TAG, "sesja zakonczona");
}

static void serve(uint16_t port)
{
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 2) < 0) {
        ESP_LOGE(TAG, "bind/listen portu %u: %d", port, errno);
        closesocket(ls);
        return;
    }
    ESP_LOGI(TAG, "nasluchuje na porcie TCP %u", port);

    vs_conn_t *c = NULL;
    void *app = NULL;
    int64_t last_tick = esp_timer_get_time();
    for (;;) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(ls, &rf);
        int mx = ls;
        if (c) { FD_SET(c->sock, &rf); if (c->sock > mx) mx = c->sock; }
        struct timeval tv = { 0, 50 * 1000 };
        int n = select(mx + 1, &rf, NULL, NULL, &tv);

        if (n > 0 && FD_ISSET(ls, &rf)) {
            struct sockaddr_in ca;
            socklen_t cl = sizeof ca;
            int s = accept(ls, (struct sockaddr *)&ca, &cl);
            if (s >= 0) {
                if (c) {
                    ESP_LOGI(TAG, "nowe polaczenie zastepuje poprzednie");
                    conn_end(&c, &app);
                }
                ESP_LOGI(TAG, "polaczenie z %s:%u", inet_ntoa(ca.sin_addr), ntohs(ca.sin_port));
                setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
                c = calloc(1, sizeof *c);
                if (!c) { closesocket(s); continue; }
                c->sock = s;
                c->tx_lock = xSemaphoreCreateMutex();
                app = vs_app_open(c);
                if (!app) { conn_end(&c, &app); continue; }
                FD_CLR(s, &rf);                          /* nie bylo go w tym select() */
            }
        }
        if (c && n > 0 && FD_ISSET(c->sock, &rf)) {
            int r = (int)recv(c->sock, c->rbuf + c->rlen, (size_t)(VS_RBUF - c->rlen), 0);
            if (r > 0) { c->rlen += r; process_frames(c, app); }
            else if (r == 0 || (errno != EAGAIN && errno != EINTR)) c->closed = true;
        }
        if (c && c->closed) conn_end(&c, &app);

        int64_t t = esp_timer_get_time();
        if (t - last_tick >= TICK_MS * 1000LL) {
            last_tick = t;
            if (app) vs_app_tick(app);
            if (c && c->closed) conn_end(&c, &app);
        }
    }
}

static void session_task(void *arg)
{
    (void)arg;
    serve(CONFIG_RSX_VS_PORT);
    vTaskDelete(NULL);
}

void vs_session_start(void)
{
    xTaskCreate(session_task, "vs_session", 8192, NULL, 5, NULL);
}
