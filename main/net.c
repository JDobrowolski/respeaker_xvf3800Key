/*
 * net.c - patrz net.h. Zmiany trybu, ponowne proby, przycisk BOOT i rozgloszenia
 * robi jedno zadanie "net" (petla co 50 ms); obsluga zdarzen WiFi tylko ustawia flagi.
 */
#include <string.h>
#include <stdio.h>
#include "net.h"
#include "vsproto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

static const char *TAG = "net";

#define RECONNECT_MS   2000
#define BEACON_MS      2000

static char s_name[24], s_ap_ssid[33];
static char s_sta_ssid[33], s_sta_pass[65];
static esp_netif_t *s_sta_if;

static volatile net_mode_t s_mode;
static volatile bool       s_ip;
static volatile int        s_fails;
static volatile bool       s_want_fallback;
static volatile int64_t    s_reconnect_at;     /* us, 0 = brak */
static volatile int64_t    s_restart_at;
static volatile uint32_t   s_ver = 1;
static char                s_ip_str[16];
static int64_t             s_fallback_since;
static void (*s_short_cb)(void);

static int64_t now_us(void) { return esp_timer_get_time(); }

/* ------------------------------------------------------------ NVS */
static void load_cfg(void)
{
    nvs_handle_t h;
    s_sta_ssid[0] = s_sta_pass[0] = 0;
    if (nvs_open("vsnet", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof s_sta_ssid;
    if (nvs_get_str(h, "ssid", s_sta_ssid, &n) != ESP_OK) s_sta_ssid[0] = 0;
    n = sizeof s_sta_pass;
    if (nvs_get_str(h, "pass", s_sta_pass, &n) != ESP_OK) s_sta_pass[0] = 0;
    nvs_close(h);
}

int net_save_sta(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open("vsnet", NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e;
    if (!ssid[0]) {
        nvs_erase_key(h, "ssid");
        nvs_erase_key(h, "pass");
        e = ESP_OK;
    } else {
        e = nvs_set_str(h, "ssid", ssid);
        if (e == ESP_OK) e = nvs_set_str(h, "pass", pass);
    }
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "zapamietana siec: %s", ssid[0] ? ssid : "(brak - tryb AP)");
    return e == ESP_OK ? 0 : -1;
}

bool net_is_current(const char *ssid, const char *pass)
{
    return !strcmp(ssid, s_sta_ssid) && !strcmp(pass, s_sta_pass);
}

void net_restart_later(int ms) { s_restart_at = now_us() + (int64_t)ms * 1000; }
void net_set_button_cb(void (*cb)(void)) { s_short_cb = cb; }
uint32_t net_version(void) { return s_ver; }

/* ------------------------------------------------------------ zdarzenia */
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_mode == NET_STA) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip = false;
        s_ver++;
        if (s_mode != NET_STA) return;                    /* zatrzymanie przy zmianie trybu */
        wifi_event_sta_disconnected_t *e = data;
        s_fails++;
        ESP_LOGW(TAG, "brak polaczenia z \"%s\" (powod %d, proba %d/%d)", s_sta_ssid, e ? e->reason : -1, s_fails,
                 CONFIG_RSX_STA_MAX_FAILS);
        if (s_fails >= CONFIG_RSX_STA_MAX_FAILS) s_want_fallback = true;
        else s_reconnect_at = now_us() + RECONNECT_MS * 1000LL;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip_str, sizeof s_ip_str, IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "polaczono z \"%s\", adres %s", s_sta_ssid, s_ip_str);
        s_fails = 0;
        s_ip = true;
        s_ver++;
    } else if (base == WIFI_EVENT && (id == WIFI_EVENT_AP_STACONNECTED || id == WIFI_EVENT_AP_STADISCONNECTED)) {
        s_ver++;
    }
}

/* ------------------------------------------------------------ tryby */
static void start_ap(void)
{
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.ap.ssid, s_ap_ssid, sizeof wc.ap.ssid);
    wc.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    strlcpy((char *)wc.ap.password, CONFIG_RSX_AP_PASS, sizeof wc.ap.password);
    wc.ap.max_connection = 4;
    wc.ap.channel = CONFIG_RSX_AP_CHANNEL;
    wc.ap.authmode = strlen(CONFIG_RSX_AP_PASS) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wc);
    esp_wifi_start();
    ESP_LOGI(TAG, "punkt dostepowy \"%s\" (%s), adres 192.168.4.1", s_ap_ssid,
             wc.ap.authmode == WIFI_AUTH_OPEN ? "otwarty" : "WPA2");
}

static void start_sta(void)
{
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, s_sta_ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, s_sta_pass, sizeof wc.sta.password);
    wc.sta.threshold.authmode = s_sta_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();                                     /* STA_START -> esp_wifi_connect() */
    esp_wifi_set_ps(WIFI_PS_NONE);                        /* nizsze opoznienia sesji */
    ESP_LOGI(TAG, "lacze z \"%s\"...", s_sta_ssid);
}

static void switch_to(net_mode_t m)
{
    s_mode = m;                                           /* najpierw: zdarzenia z esp_wifi_stop() pomijane */
    esp_wifi_stop();
    s_ip = false;
    s_fails = 0;
    s_want_fallback = false;
    s_reconnect_at = 0;
    if (m == NET_STA) start_sta(); else start_ap();
    s_ver++;
}

/* ------------------------------------------------------------ zadanie */
static void send_beacon(int *sock)
{
    if (*sock < 0) {
        *sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (*sock < 0) return;
        int one = 1;
        setsockopt(*sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char msg[128];
    int n = snprintf(msg, sizeof msg, VS_DISCOVERY_MAGIC "name=%s;port=%d;id=%02x%02x%02x%02x%02x%02x", s_name,
                     CONFIG_RSX_VS_PORT, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(VS_DISCOVERY_PORT);
    a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(*sock, msg, (size_t)n, 0, (struct sockaddr *)&a, sizeof a);
}

static int ap_clients(void)
{
    wifi_sta_list_t l;
    return esp_wifi_ap_get_sta_list(&l) == ESP_OK ? l.num : 0;
}

static void net_task(void *arg)
{
    (void)arg;
    int64_t pressed_at = 0;
    bool    fired = false;
    int64_t next_beacon = 0;
    int     bsock = -1;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        int64_t t = now_us();

        /* przycisk BOOT: krotko = dioda, dlugo = zapomnij siec */
        bool down = gpio_get_level(CONFIG_RSX_BOOT_GPIO) == 0;
        if (down && !pressed_at) { pressed_at = t; fired = false; }
        if (down && !fired && t - pressed_at >= CONFIG_RSX_RESET_HOLD_MS * 1000LL) {
            fired = true;
            ESP_LOGW(TAG, "BOOT przytrzymany - kasuje siec, wracam do punktu dostepowego");
            net_save_sta("", "");
            esp_restart();
        }
        if (!down && pressed_at) {
            if (!fired && t - pressed_at >= 30000 && t - pressed_at < 800000 && s_short_cb) s_short_cb();
            pressed_at = 0;
        }

        if (s_restart_at && t >= s_restart_at) {
            ESP_LOGI(TAG, "restart");
            esp_restart();
        }

        if (s_mode == NET_STA && s_want_fallback) {
            ESP_LOGW(TAG, "\"%s\" nie odpowiada - tymczasowo punkt dostepowy (siec zostaje zapamietana)", s_sta_ssid);
            switch_to(NET_AP_FALLBACK);
            s_fallback_since = t;
        }
        if (s_mode == NET_STA && s_reconnect_at && t >= s_reconnect_at) {
            s_reconnect_at = 0;
            esp_wifi_connect();
        }
        if (s_mode == NET_AP_FALLBACK && CONFIG_RSX_FALLBACK_RETRY_MIN > 0 &&
            t - s_fallback_since >= CONFIG_RSX_FALLBACK_RETRY_MIN * 60LL * 1000000LL) {
            s_fallback_since = t;
            if (ap_clients() == 0) {                         /* nie zrywaj komus sesji */
                ESP_LOGI(TAG, "ponowna proba zapamietanej sieci");
                switch_to(NET_STA);
            }
        }

        if (s_mode == NET_STA && s_ip && t >= next_beacon) {
            next_beacon = t + BEACON_MS * 1000LL;
            send_beacon(&bsock);
        }
    }
}

/* ------------------------------------------------------------ API */
void net_start(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
#if CONFIG_RSX_NAME_MAC_SUFFIX
    snprintf(s_name, sizeof s_name, "%.15s-%02X%02X", CONFIG_RSX_NAME, mac[4], mac[5]);
#else
    snprintf(s_name, sizeof s_name, "%.23s", CONFIG_RSX_NAME);
#endif
    snprintf(s_ap_ssid, sizeof s_ap_ssid, "%s%s", "vsapp_", s_name);
    load_cfg();

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_RSX_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_if = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta_if, s_name);
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));   /* konfiguracje trzymamy sami (vsnet) */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    s_mode = s_sta_ssid[0] ? NET_STA : NET_AP;
    if (s_mode == NET_STA) start_sta(); else start_ap();
    xTaskCreate(net_task, "net", 4096, NULL, 4, NULL);
}

void net_get_info(net_info_t *o)
{
    memset(o, 0, sizeof *o);
    o->mode = s_mode;
    o->has_ip = s_ip;
    o->fails = s_fails;
    strlcpy(o->name, s_name, sizeof o->name);
    strlcpy(o->ap_ssid, s_ap_ssid, sizeof o->ap_ssid);
    strlcpy(o->sta_ssid, s_sta_ssid, sizeof o->sta_ssid);
    if (s_mode == NET_STA) {
        if (s_ip) {
            strlcpy(o->ip, s_ip_str, sizeof o->ip);
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) o->rssi = ap.rssi;
        }
    } else {
        strlcpy(o->ip, "192.168.4.1", sizeof o->ip);
        o->clients = ap_clients();
    }
}
