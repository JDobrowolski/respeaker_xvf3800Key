/*
 * wake.h - slowo kluczowe na ESP32-S3: detektor (components/kws) na strumieniu mikrofonu,
 * stan "polecenia" po wykryciu i jego koniec (VAD na urzadzeniu), model w partycji flash "kws".
 *
 *   LISTEN  - nasluchuje, nic nie wysyla
 *   CMD     - po slowie: audio_net wysyla AF_WAKE + AF_MIC od cmd_start do cmd_end, potem AF_END
 */
#ifndef WAKE_H
#define WAKE_H

#include <stdint.h>
#include <stdbool.h>

typedef enum { WAKE_LISTEN = 0, WAKE_CMD = 1 } wake_state_t;

typedef struct {
    uint32_t seq;            /* numer polecenia (rosnie przy kazdym wykryciu) */
    uint32_t start, end;     /* pozycje w historii mikrofonu; end = 0 - jeszcze trwa */
    int      reason;         /* AF_END_* gdy end != 0 */
    uint8_t  score;          /* 0..255 */
    int      doa;            /* -1 = brak */
} wake_cmd_t;

void        wake_start(void);
bool        wake_model_ok(void);
const char *wake_keyword(void);
float       wake_threshold(void);              /* biezacy prog */
float       wake_default_threshold(void);      /* prog z modelu */
void        wake_set_threshold(float t);       /* 0 = z modelu; zapisywany w NVS */
float       wake_score(void);                  /* ostatni wygladzony wynik (do UI) */
wake_state_t wake_state(void);
uint32_t    wake_version(void);                /* rosnie przy zmianie stanu (UI) */
const char *wake_last_event(void);             /* opis ostatniego zdarzenia (UI) */

void        wake_cmd(wake_cmd_t *out);         /* migawka biezacego/ostatniego polecenia */
void        wake_cmd_done(void);               /* audio_net wyslal AF_END -> nasluch */
void        wake_listen_now(void);             /* jak po slowie (AF_LISTEN 1 / przycisk) */
void        wake_cancel(void);                 /* zakoncz polecenie (AF_LISTEN 0) */

/* wgrywanie modelu (z audio_net); zwracaja 0 = OK, msg = opis dla AF_STATUS */
int         wake_model_begin(uint32_t size, char *msg, int msgsz);
int         wake_model_data(const uint8_t *d, int n, char *msg, int msgsz);
int         wake_model_end(uint32_t crc, char *msg, int msgsz);

#endif /* WAKE_H */
