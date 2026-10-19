/*
 * vs_app.c - aplikacja sesji wektorowej: stan Wi-Fi + ustawienia audio XVF3800.
 *  - suwak glosnosci glosnika (0..100) i wzmocnienia mikrofonu (-12..+36 dB),
 *  - tarcza kierunku zrodla dzwieku (DOA) z XVF3800,
 *  - stan strumienia audio (port, czy host podlaczony),
 *  - slowo kluczowe: stan nasluchu, wynik, prog (suwak), "Mow teraz",
 *  - stan sieci + "Zapomnij siec" / "Polacz ponownie" i obsluga VS_EV_NET_CONFIG
 *    (Tab5 przekazuje siec, urzadzenie zapisuje ja w NVS i restartuje).
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "vs_session.h"
#include "vsproto.h"
#include "net.h"
#include "audio.h"
#include "audio_net.h"
#include "wake.h"
#include "esp_log.h"

static const char *TAG = "vs_app";

#define W_DEF     800
#define H_DEF     760
#define CONTENT_H 720

enum {
    ID_TITLE = 2, ID_NET, ID_STATUS, ID_FORGET, ID_RETRY,
    ID_VOL_LABEL, ID_VOL_SL, ID_VOL_VAL,
    ID_GAIN_LABEL, ID_GAIN_SL, ID_GAIN_VAL,
    ID_STREAM,
    ID_DIAL, ID_DOA_PTR, ID_DOA_TXT,
    ID_THR_LABEL, ID_THR_SL, ID_THR_VAL, ID_KWS_TXT, ID_LISTEN
};

#define COL_BG    0xFF1C2127u
#define COL_TEXT  0xFFE6EAEEu
#define COL_MUTED 0xFF97A3AFu
#define COL_OK    0xFF8FD19Eu
#define COL_ERR   0xFFFF8A80u
#define COL_ACCENT 0xFF6FB3FFu
#define COL_DIAL  0xFF2A2F36u

#define DIAL_R 70.0f

typedef struct {
    vs_conn_t *conn;
    int        W, H;
    bool       built;
    uint32_t   net_ver, audio_ver;
    uint32_t   ticks, forget_tick;
    bool       forget_armed;
    bool       restarting;
    int        net_refresh;
    int        last_deg, last_en;
    bool       last_doa_ok;
    int        last_clients;
    uint32_t   wake_ver;
    int        last_score;
    VsWriter   w;
    uint8_t    ui[3072];
} app_t;

/* ------------------------------------------------------------ ramki UI */
static void ui_flush(app_t *a)
{
    if (a->w.len) vs_conn_send(a->conn, a->ui, a->w.len);
    vs_w_init(&a->w, a->ui, (int)sizeof a->ui);
}
static void ui_begin(app_t *a, int type)
{
    if (a->w.cap - a->w.len < 300) ui_flush(a);
    vs_w_begin(&a->w, type);
}
static void ui_end(app_t *a) { vs_w_end(&a->w); }
static void ui_commit(app_t *a) { ui_begin(a, VS_COMMIT); ui_end(a); ui_flush(a); }

static void create(app_t *a, uint16_t id, int type, float x, float y)
{
    ui_begin(a, VS_CREATE);
    vs_w_u16(&a->w, id);
    vs_w_u16(&a->w, 0);
    vs_w_u8(&a->w, (unsigned)type);
    vs_w_fx(&a->w, x);
    vs_w_fx(&a->w, y);
}
static void set_fill(app_t *a, uint16_t id, uint32_t col)
{
    ui_begin(a, VS_SET_FILL); vs_w_u16(&a->w, id); vs_w_u32(&a->w, col); ui_end(a);
}
static void set_stroke(app_t *a, uint16_t id, uint32_t col, float width)
{
    ui_begin(a, VS_SET_STROKE); vs_w_u16(&a->w, id); vs_w_u32(&a->w, col); vs_w_fx(&a->w, width); ui_end(a);
}
static void set_text(app_t *a, uint16_t id, const char *s)
{
    ui_begin(a, VS_SET_TEXT); vs_w_u16(&a->w, id); vs_w_str(&a->w, s); ui_end(a);
}
static void set_visible(app_t *a, uint16_t id, bool v)
{
    ui_begin(a, VS_SET_VISIBLE); vs_w_u16(&a->w, id); vs_w_u8(&a->w, v ? 1 : 0); ui_end(a);
}
static void set_value(app_t *a, uint16_t id, int32_t v)
{
    ui_begin(a, VS_SET_VALUE); vs_w_u16(&a->w, id); vs_w_s32(&a->w, v); ui_end(a);
}
static void set_rotation(app_t *a, uint16_t id, float deg)
{
    ui_begin(a, VS_SET_ROTATION); vs_w_u16(&a->w, id); vs_w_an(&a->w, deg); ui_end(a);
}
static void mk_text(app_t *a, uint16_t id, float x, float y, float size, unsigned align, uint32_t col, const char *s)
{
    create(a, id, VS_T_TEXT, x, y);
    vs_w_fx(&a->w, size); vs_w_u8(&a->w, align); vs_w_str(&a->w, s);
    ui_end(a);
    set_fill(a, id, col);
}
static void mk_button(app_t *a, uint16_t id, float x, float y, float w, float h, unsigned style, const char *label)
{
    create(a, id, VS_T_BUTTON, x, y);
    vs_w_fx(&a->w, w); vs_w_fx(&a->w, h); vs_w_u8(&a->w, style); vs_w_str(&a->w, label);
    ui_end(a);
}
static void set_button(app_t *a, uint16_t id, float w, float h, unsigned style, const char *label)
{
    ui_begin(a, VS_SET_GEOM);
    vs_w_u16(&a->w, id);
    vs_w_fx(&a->w, w); vs_w_fx(&a->w, h); vs_w_u8(&a->w, style); vs_w_str(&a->w, label);
    ui_end(a);
}
static void mk_slider(app_t *a, uint16_t id, float x, float y, float w, float h, int min, int max, int val)
{
    create(a, id, VS_T_SLIDER, x, y);
    vs_w_fx(&a->w, w); vs_w_fx(&a->w, h);
    vs_w_s32(&a->w, min); vs_w_s32(&a->w, max); vs_w_s32(&a->w, 1); vs_w_s32(&a->w, val);
    ui_end(a);
}

static void status(app_t *a, uint32_t col, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void status(app_t *a, uint32_t col, const char *fmt, ...)
{
    char s[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    set_text(a, ID_STATUS, s);
    set_fill(a, ID_STATUS, col);
}

/* ------------------------------------------------------------ widok audio */
static void ui_audio(app_t *a, int skip)
{
    a->audio_ver = audio_settings_version();
    char s[64];
    int vol = audio_get_volume(), gain = audio_get_mic_gain();
    if (skip != ID_VOL_SL) set_value(a, ID_VOL_SL, vol);
    snprintf(s, sizeof s, "%d %%", vol);
    set_text(a, ID_VOL_VAL, s);
    if (skip != ID_GAIN_SL) set_value(a, ID_GAIN_SL, gain);
    snprintf(s, sizeof s, "%+d dB", gain);
    set_text(a, ID_GAIN_VAL, s);
}

static void ui_doa(app_t *a)
{
    int deg = 0, en = 0;
    a->last_doa_ok = audio_get_doa(&deg, &en);
    char s[64];
    if (a->last_doa_ok) {
        a->last_deg = deg; a->last_en = en;
        set_rotation(a, ID_DOA_PTR, (float)deg);
        set_visible(a, ID_DOA_PTR, true);
        snprintf(s, sizeof s, "Kierunek %d°  ·  energia %d %%", deg, en * 100 / 255);
    } else {
        set_visible(a, ID_DOA_PTR, false);
        snprintf(s, sizeof s, "Kierunek: — (brak danych z XVF3800)");
    }
    set_text(a, ID_DOA_TXT, s);
}

static void ui_stream(app_t *a)
{
    a->last_clients = audio_net_clients();
    char s[160];
    snprintf(s, sizeof s, "Strumień audio: port %d · %d Hz · %s · %s", CONFIG_RSX_AUDIO_PORT,
             audio_sample_rate(), audio_channels() == 1 ? "mono" : "stereo",
             a->last_clients ? "host podłączony" : "czeka na hosta");
    set_text(a, ID_STREAM, s);
    set_fill(a, ID_STREAM, a->last_clients ? COL_OK : COL_MUTED);
}

static void ui_kws(app_t *a, int skip)
{
    a->wake_ver = wake_version();
    a->last_score = (int)(wake_score() * 100 + 0.5f);
    char s[200];
    int thr = (int)(wake_threshold() * 100 + 0.5f);
    if (skip != ID_THR_SL) set_value(a, ID_THR_SL, thr);
    snprintf(s, sizeof s, "%d %%", thr);
    set_text(a, ID_THR_VAL, s);
    uint32_t col = COL_MUTED;
    if (!wake_model_ok()) {
        snprintf(s, sizeof s, "Słowo kluczowe: brak modelu (kwstool upload <ip> model.kws) · %s", wake_last_event());
        col = COL_ERR;
    } else if (wake_state() == WAKE_CMD) {
        snprintf(s, sizeof s, "„%s” · nagrywam polecenie dla hosta…", wake_keyword());
        col = COL_OK;
    } else {
        snprintf(s, sizeof s, "„%s” · nasłuchuję · wynik %d %% · %s", wake_keyword(), a->last_score, wake_last_event());
    }
    set_text(a, ID_KWS_TXT, s);
    set_fill(a, ID_KWS_TXT, col);
}

static void ui_net(app_t *a)
{
    net_info_t ni;
    net_get_info(&ni);
    a->net_ver = net_version();
    char s[256];
    switch (ni.mode) {
    case NET_AP:
        snprintf(s, sizeof s, "Punkt dostępowy „%s” · %s · klientów: %d. Sieć ustawisz z Tab5: "
                 "Ustawienia Wi-Fi → „Apply to vsapps”.", ni.ap_ssid, ni.ip, ni.clients);
        break;
    case NET_STA:
        if (ni.has_ip) snprintf(s, sizeof s, "Sieć „%s” · %s · sygnał %d dBm", ni.sta_ssid, ni.ip, ni.rssi);
        else snprintf(s, sizeof s, "Łączę z „%s”… (nieudane próby: %d)", ni.sta_ssid, ni.fails);
        break;
    default:
        snprintf(s, sizeof s, "Nie mogę połączyć z „%s” - tymczasowo punkt dostępowy „%s”. "
                 "Po restarcie spróbuję ponownie.", ni.sta_ssid, ni.ap_ssid);
        break;
    }
    set_text(a, ID_NET, s);
    set_fill(a, ID_NET, ni.mode == NET_AP_FALLBACK ? COL_ERR : COL_MUTED);
    set_visible(a, ID_FORGET, ni.sta_ssid[0] != 0);
    set_visible(a, ID_RETRY, ni.mode == NET_AP_FALLBACK);
}

static void ui_build(app_t *a)
{
    int W = a->W, H = a->H;
    float PW = W - 32 > 720 ? 720 : (float)(W - 32);
    float X = (W - PW) / 2;
    float Y = H > CONTENT_H ? (float)(H - CONTENT_H) / 2 : 8;

    ui_begin(a, VS_RESET); ui_end(a);
    ui_begin(a, VS_SET_FONT); vs_w_u16(&a->w, 0); vs_w_u8(&a->w, 22); ui_end(a);

    net_info_t ni;
    net_get_info(&ni);
    char title[64];
    snprintf(title, sizeof title, "ReSpeaker XVF3800 · %s", ni.name);
    mk_text(a, ID_TITLE, X, Y, 22, VS_AL_LEFT | VS_AL_TOP, COL_TEXT, title);
    mk_text(a, ID_NET, X, Y + 34, 16, VS_AL_LEFT | VS_AL_TOP, COL_MUTED, "");

    /* suwaki audio */
    float sy = Y + 76, lw = 150, vw = 90;
    mk_text(a, ID_VOL_LABEL, X, sy + 10, 16, VS_AL_LEFT | VS_AL_TOP, COL_TEXT, "Głośność");
    mk_slider(a, ID_VOL_SL, X + lw, sy, PW - lw - vw, 40, 0, 100, audio_get_volume());
    mk_text(a, ID_VOL_VAL, X + PW, sy + 10, 16, VS_AL_RIGHT | VS_AL_TOP, COL_TEXT, "");
    float gy = sy + 58;
    mk_text(a, ID_GAIN_LABEL, X, gy + 10, 16, VS_AL_LEFT | VS_AL_TOP, COL_TEXT, "Wzm. mikrofonu");
    mk_slider(a, ID_GAIN_SL, X + lw, gy, PW - lw - vw, 40, -12, 36, audio_get_mic_gain());
    mk_text(a, ID_GAIN_VAL, X + PW, gy + 10, 16, VS_AL_RIGHT | VS_AL_TOP, COL_TEXT, "");

    /* slowo kluczowe */
    float ky = gy + 58;
    mk_text(a, ID_THR_LABEL, X, ky + 10, 16, VS_AL_LEFT | VS_AL_TOP, COL_TEXT, "Próg słowa");
    mk_slider(a, ID_THR_SL, X + lw, ky, PW - lw - vw, 40, 30, 99, (int)(wake_threshold() * 100 + 0.5f));
    mk_text(a, ID_THR_VAL, X + PW, ky + 10, 16, VS_AL_RIGHT | VS_AL_TOP, COL_TEXT, "");
    mk_text(a, ID_KWS_TXT, X, ky + 52, 16, VS_AL_LEFT | VS_AL_TOP, COL_MUTED, "");
    mk_button(a, ID_LISTEN, X, ky + 80, 220, 46, VS_BTN_PRIMARY, "Mów teraz");

    /* tarcza kierunku (DOA) */
    float cx = X + DIAL_R + 10, cy = ky + 140 + DIAL_R;
    create(a, ID_DIAL, VS_T_ELLIPSE, cx, cy);
    vs_w_fx(&a->w, 0); vs_w_fx(&a->w, 0); vs_w_fx(&a->w, DIAL_R); vs_w_fx(&a->w, DIAL_R);
    ui_end(a);
    set_fill(a, ID_DIAL, COL_DIAL);
    set_stroke(a, ID_DIAL, 0xFF55606Cu, 2);
    create(a, ID_DOA_PTR, VS_T_PATH, cx, cy);
    vs_w_u8(&a->w, VS_P_MOVE); vs_w_fx(&a->w, 0); vs_w_fx(&a->w, 0);
    vs_w_u8(&a->w, VS_P_LINE); vs_w_fx(&a->w, 0); vs_w_fx(&a->w, -(DIAL_R - 8));
    ui_end(a);
    set_stroke(a, ID_DOA_PTR, COL_ACCENT, 5);
    mk_text(a, ID_DOA_TXT, cx + DIAL_R + 20, cy - 8, 16, VS_AL_LEFT | VS_AL_TOP, COL_TEXT, "");
    mk_text(a, ID_STREAM, X, cy + DIAL_R + 10, 16, VS_AL_LEFT | VS_AL_TOP, COL_MUTED, "");

    /* siec */
    float ny = cy + DIAL_R + 44;
    mk_button(a, ID_FORGET, X, ny, 340, 50, 0, "Zapomnij sieć Wi-Fi");
    mk_button(a, ID_RETRY, X + 356, ny, 260, 50, VS_BTN_PRIMARY, "Połącz ponownie");
    mk_text(a, ID_STATUS, X, ny + 64, 16, VS_AL_LEFT | VS_AL_TOP, COL_MUTED,
            "Przycisk BOOT przytrzymany: powrót do punktu dostępowego.");

    a->forget_armed = false;
    ui_audio(a, -1);
    ui_doa(a);
    ui_stream(a);
    ui_kws(a, -1);
    ui_net(a);
    a->built = true;
}

/* ------------------------------------------------------------ siec (VS_EV_NET_CONFIG) */
static void net_result(app_t *a, int st, const char *msg)
{
    ui_begin(a, VS_NET_RESULT); vs_w_u8(&a->w, (unsigned)st); vs_w_str(&a->w, msg); ui_end(a);
}

static void on_net_config(app_t *a, const uint8_t *data, int len)
{
    VsReader r;
    vs_r_init(&r, data, len);
    (void)vs_r_u8(&r);
    unsigned n = vs_r_u8(&r);
    char ssid[33] = "", pass[65] = "";
    if (r.err || n > 32 || vs_r_left(&r) < (int)n) { net_result(a, VS_NR_INVALID, "SSID za długi"); return; }
    memcpy(ssid, data + r.pos, n);
    ssid[n] = 0;
    r.pos += (int)n;
    if (vs_r_left(&r) > 64) { net_result(a, VS_NR_INVALID, "hasło za długie"); return; }
    vs_r_str(&r, pass, sizeof pass);
    if (pass[0] && strlen(pass) < 8) { net_result(a, VS_NR_INVALID, "hasło krótsze niż 8 znaków"); return; }

    net_info_t ni;
    net_get_info(&ni);
    ESP_LOGI(TAG, "VS_EV_NET_CONFIG: \"%s\"", ssid[0] ? ssid : "(zapomnij)");
    if (net_is_current(ssid, pass) && (ni.mode == NET_STA ? ni.has_ip : !ssid[0])) {
        net_result(a, VS_NR_OK, "bez zmian - już w tej sieci");
        return;
    }
    if (net_save_sta(ssid, pass) != 0) { net_result(a, VS_NR_IO_ERROR, "zapis NVS nie powiódł się"); return; }
    char msg[80];
    if (ssid[0]) snprintf(msg, sizeof msg, "zapisano - łączę z „%s”", ssid);
    else snprintf(msg, sizeof msg, "siec zapomniana - punkt dostępowy");
    net_result(a, VS_NR_OK, msg);
    status(a, COL_OK, "Nowa sieć: %s. Restart…", ssid[0] ? ssid : "(brak)");
    a->restarting = true;
    net_restart_later(1500);
}

/* ------------------------------------------------------------ sesja */
void *vs_app_open(vs_conn_t *c)
{
    app_t *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->conn = c;
    a->W = W_DEF; a->H = H_DEF;
    vs_w_init(&a->w, a->ui, (int)sizeof a->ui);

    net_info_t ni;
    net_get_info(&ni);
    char title[48];
    snprintf(title, sizeof title, "xvf3800 · %s", ni.name);
    ui_begin(a, VS_HELLO);
    vs_w_u32(&a->w, VS_MAGIC);
    vs_w_u8(&a->w, VS_VERSION);
    vs_w_u16(&a->w, (unsigned)a->W);
    vs_w_u16(&a->w, (unsigned)a->H);
    vs_w_u32(&a->w, COL_BG);
    vs_w_str(&a->w, title);
    ui_end(a);
    ui_begin(a, VS_EVENT_MASK); vs_w_u16(&a->w, VS_EM_BUTTONS); ui_end(a);
    ui_flush(a);
    return a;
}

void vs_app_close(void *p) { free(p); }

void vs_app_tick(void *p)
{
    app_t *a = p;
    a->ticks++;
    if (!a->built) {
        if (a->ticks >= 4) { ui_build(a); ui_commit(a); }
        return;
    }
    bool dirty = false;
    if (a->audio_ver != audio_settings_version()) { ui_audio(a, -1); dirty = true; }
    if ((a->ticks % 3) == 0) { ui_doa(a); dirty = true; }            /* DOA co ~0,75 s */
    if (a->last_clients != audio_net_clients()) { ui_stream(a); dirty = true; }
    if (a->wake_ver != wake_version() || abs((int)(wake_score() * 100 + 0.5f) - a->last_score) >= 5) { ui_kws(a, -1); dirty = true; }
    if (++a->net_refresh >= 20 || a->net_ver != net_version()) {
        a->net_refresh = 0;
        ui_net(a);
        dirty = true;
    }
    if (a->forget_armed && a->ticks - a->forget_tick > 20) {
        a->forget_armed = false;
        set_button(a, ID_FORGET, 340, 50, 0, "Zapomnij sieć Wi-Fi");
        dirty = true;
    }
    if (dirty) ui_commit(a);
}

void vs_app_frame(void *p, int type, const uint8_t *data, int len)
{
    app_t *a = p;
    VsReader r;
    vs_r_init(&r, data, len);

    if (type == VS_EV_NET_CONFIG) {
        on_net_config(a, data, len);
        if (a->built) ui_commit(a); else ui_flush(a);
        return;
    }
    if (type == VS_EV_HELLO || type == VS_EV_RESIZE) {
        if (type == VS_EV_HELLO) {
            uint32_t magic = vs_r_u32(&r);
            (void)vs_r_u8(&r);
            if (magic != VS_MAGIC) return;
        }
        int w = (int)vs_r_u16(&r), h = (int)vs_r_u16(&r);
        if (r.err) return;
        if (w >= 320 && h >= 400) { a->W = w; a->H = h; }
        ui_build(a);
        ui_commit(a);
        return;
    }
    if (!a->built || a->restarting) return;

    if (type != VS_EV_CONTROL) return;
    uint16_t id = (uint16_t)vs_r_u16(&r);
    unsigned act = vs_r_u8(&r);
    int32_t val = vs_r_s32(&r);
    if (r.err) return;

    if (id == ID_VOL_SL) {
        if (act == VS_CA_CHANGE || act == VS_CA_COMMIT) { audio_set_volume((int)val); ui_audio(a, act == VS_CA_COMMIT ? -1 : ID_VOL_SL); }
        else return;
    } else if (id == ID_GAIN_SL) {
        if (act == VS_CA_CHANGE || act == VS_CA_COMMIT) { audio_set_mic_gain((int)val); ui_audio(a, act == VS_CA_COMMIT ? -1 : ID_GAIN_SL); }
        else return;
    } else if (id == ID_THR_SL) {
        if (act == VS_CA_CHANGE || act == VS_CA_COMMIT) { wake_set_threshold((float)val / 100.0f); ui_kws(a, act == VS_CA_COMMIT ? -1 : ID_THR_SL); }
        else return;
    } else if (act != VS_CA_CLICK) {
        return;
    } else if (id == ID_FORGET) {
        if (!a->forget_armed) {
            a->forget_armed = true;
            a->forget_tick = a->ticks;
            set_button(a, ID_FORGET, 340, 50, VS_BTN_PRIMARY, "Na pewno? Kliknij znowu");
            status(a, COL_ERR, "Urządzenie zapomni sieć i wróci do trybu punktu dostępowego.");
        } else {
            a->forget_armed = false;
            if (net_save_sta("", "") == 0) {
                status(a, COL_OK, "Sieć zapomniana - restart jako punkt dostępowy…");
                a->restarting = true;
                net_restart_later(1000);
            } else {
                status(a, COL_ERR, "Zapis NVS nie powiódł się");
            }
        }
    } else if (id == ID_LISTEN) {
        wake_listen_now();
        status(a, audio_net_clients() ? COL_OK : COL_ERR, audio_net_clients()
               ? "Słucham polecenia - mów…" : "Brak hosta (vsasr) - polecenie nie zostanie wysłane");
    } else if (id == ID_RETRY) {
        status(a, COL_TEXT, "Restart - ponowna próba połączenia z siecią…");
        a->restarting = true;
        net_restart_later(800);
    } else {
        return;
    }
    ui_commit(a);
}
