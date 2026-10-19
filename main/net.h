/*
 * net.h - WiFi urzadzenia vsapp:
 *  - brak zapamietanej sieci: punkt dostepowy "vsapp_<nazwa>" (192.168.4.1), Tab5 widzi go jako ikone;
 *  - zapamietana siec (z Tab5: VS_EV_NET_CONFIG): stacja + rozgloszenie UDP VS_DISCOVERY_PORT,
 *    po ktorym Tab5 znajduje urzadzenie w tej sieci;
 *  - kilka nieudanych prob z rzedu: TYMCZASOWO punkt dostepowy (nic nie jest kasowane,
 *    po restarcie - i co CONFIG_VSLED_FALLBACK_RETRY_MIN minut - znow proba sieci);
 *  - przytrzymanie BOOT (CONFIG_VSLED_RESET_HOLD_MS): kasuje siec, restart jako punkt dostepowy.
 *    Krotkie nacisniecie: wlacz/wylacz diode.
 */
#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NET_AP = 0,             /* brak zapamietanej sieci */
    NET_STA,                /* laczy sie / polaczony z zapamietana siecia */
    NET_AP_FALLBACK         /* zapamietana siec nie dziala - tymczasowo AP */
} net_mode_t;

typedef struct {
    net_mode_t mode;
    bool       has_ip;      /* STA: adres z DHCP */
    char       name[24];    /* "led-A1B2" */
    char       ap_ssid[33]; /* "vsapp_led-A1B2" */
    char       sta_ssid[33];/* zapamietana siec ("" = brak) */
    char       ip[16];
    int        rssi;        /* STA polaczony */
    int        clients;     /* AP: liczba podlaczonych stacji */
    int        fails;       /* STA: nieudane proby z rzedu */
} net_info_t;

void     net_start(void);
void     net_get_info(net_info_t *out);
uint32_t net_version(void);                 /* zmienia sie przy zmianie stanu sieci */
bool     net_is_current(const char *ssid, const char *pass);   /* to samo co zapamietane? */
/* zapamietaj siec (ssid "" = zapomnij); 0 = OK. Nie restartuje. */
int      net_save_sta(const char *ssid, const char *pass);
void     net_restart_later(int ms);
void     net_set_button_cb(void (*short_press)(void));

#endif
