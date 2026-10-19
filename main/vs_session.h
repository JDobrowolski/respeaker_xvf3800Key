/*
 * vs_session.h - serwer TCP sesji wektorowej: ESP32 jest aplikacja (vsapp),
 * Tab5 / vsview podgladem. Jedna sesja naraz; nowe polaczenie zastepuje stare
 * (Tab5 po zmianie sieci nie zamyka starego gniazda - inaczej wisialoby ono
 * do wygasniecia keepalive).
 */
#ifndef VS_SESSION_H
#define VS_SESSION_H

#include <stdint.h>
#include <stdbool.h>

typedef struct vs_conn vs_conn_t;

/* Wysylka gotowych ramek. 0 = OK, -1 = polaczenie zerwane. */
int  vs_conn_send(vs_conn_t *c, const uint8_t *buf, int len);
void vs_session_start(void);

/* Aplikacja sesji (vs_app.c): jedna instancja na polaczenie. */
void *vs_app_open(vs_conn_t *c);
void  vs_app_frame(void *app, int type, const uint8_t *data, int len);
void  vs_app_tick(void *app);             /* co ok. 0,25 s */
void  vs_app_close(void *app);

#endif
