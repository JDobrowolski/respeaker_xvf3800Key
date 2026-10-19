/*
 * vsproto.c - kodowanie/dekodowanie ramek sesji wektorowej (patrz vsproto.h).
 * Bez malloc i bez zaleznosci od systemu.
 */
#include <string.h>
#include "vsproto.h"

/* ======================================================================
 *  Kodowanie
 * ====================================================================== */
static int vs_clamp16(float v, float mul)
{
    float s = v * mul;
    s += s >= 0.0f ? 0.5f : -0.5f;
    if (s > 32767.0f) return 32767;
    if (s < -32768.0f) return -32768;
    return (int)s;
}

void vs_w_init(VsWriter *w, uint8_t *buf, int cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->start = -1;
    w->overflow = 0;
}

int vs_w_begin(VsWriter *w, int type)
{
    w->overflow = 0;
    if (w->len + 3 > w->cap) { w->overflow = 1; w->start = -1; return 0; }
    w->start = w->len;
    w->len += 2;                          /* rozmiar - uzupelniany w vs_w_end */
    w->buf[w->len++] = (uint8_t)type;
    return 1;
}

static void vs_w_raw(VsWriter *w, const void *p, int n)
{
    if (w->start < 0 || w->overflow) return;
    if (w->len + n > w->cap || w->len + n - w->start - 2 > VS_MAX_FRAME) { w->overflow = 1; return; }
    memcpy(w->buf + w->len, p, (size_t)n);
    w->len += n;
}

void vs_w_u8(VsWriter *w, unsigned v)
{
    uint8_t b = (uint8_t)v;
    vs_w_raw(w, &b, 1);
}

void vs_w_u16(VsWriter *w, unsigned v)
{
    uint8_t b[2];
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    vs_w_raw(w, b, 2);
}

void vs_w_u32(VsWriter *w, uint32_t v)
{
    uint8_t b[4];
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    b[2] = (uint8_t)((v >> 16) & 0xFF);
    b[3] = (uint8_t)((v >> 24) & 0xFF);
    vs_w_raw(w, b, 4);
}

void vs_w_s32(VsWriter *w, int32_t v) { vs_w_u32(w, (uint32_t)v); }

void vs_w_fx(VsWriter *w, float v)
{
    int q = vs_clamp16(v, (float)VS_FX_ONE);
    vs_w_u16(w, (unsigned)(uint16_t)(int16_t)q);
}

void vs_w_sc(VsWriter *w, float v)
{
    int q = vs_clamp16(v, (float)VS_SC_ONE);
    vs_w_u16(w, (unsigned)(uint16_t)(int16_t)q);
}

void vs_w_an(VsWriter *w, float deg)
{
    int q = vs_clamp16(deg, (float)VS_AN_ONE);
    vs_w_u16(w, (unsigned)(uint16_t)(int16_t)q);
}

void vs_w_bytes(VsWriter *w, const void *data, int len)
{
    if (data && len > 0) vs_w_raw(w, data, len);
}

/* CRC-32 z tablica 16 wpisow (polbajty): 64 B stalych zamiast 1 KB - dosc szybko i na ESP32 */
uint32_t vs_crc32(uint32_t crc, const void *data, int len)
{
    static const uint32_t t[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
    };
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len-- > 0) {
        crc ^= *p++;
        crc = (crc >> 4) ^ t[crc & 15u];
        crc = (crc >> 4) ^ t[crc & 15u];
    }
    return ~crc;
}

void vs_w_str(VsWriter *w, const char *s)
{
    int n;
    if (!s) return;
    n = (int)strlen(s);
    vs_w_raw(w, s, n);
}

int vs_w_end(VsWriter *w)
{
    int size;
    if (w->start < 0) return 0;
    if (w->overflow) {                    /* niekompletna ramka - wycofujemy ja w calosci */
        w->len = w->start;
        w->start = -1;
        return 0;
    }
    size = w->len - w->start - 2;
    w->buf[w->start] = (uint8_t)(size & 0xFF);
    w->buf[w->start + 1] = (uint8_t)((size >> 8) & 0xFF);
    w->start = -1;
    return 1;
}

/* ======================================================================
 *  Dekodowanie
 * ====================================================================== */
void vs_r_init(VsReader *r, const uint8_t *p, int len)
{
    r->p = p;
    r->len = len;
    r->pos = 0;
    r->err = 0;
}

int vs_r_left(const VsReader *r) { return r->len - r->pos; }

unsigned vs_r_u8(VsReader *r)
{
    if (r->pos + 1 > r->len) { r->err = 1; return 0; }
    return r->p[r->pos++];
}

unsigned vs_r_u16(VsReader *r)
{
    unsigned v;
    if (r->pos + 2 > r->len) { r->err = 1; r->pos = r->len; return 0; }
    v = (unsigned)r->p[r->pos] | ((unsigned)r->p[r->pos + 1] << 8);
    r->pos += 2;
    return v;
}

int vs_r_s16(VsReader *r)
{
    unsigned v = vs_r_u16(r);
    return (int)(int16_t)(uint16_t)v;
}

uint32_t vs_r_u32(VsReader *r)
{
    uint32_t v;
    if (r->pos + 4 > r->len) { r->err = 1; r->pos = r->len; return 0; }
    v = (uint32_t)r->p[r->pos] | ((uint32_t)r->p[r->pos + 1] << 8) |
        ((uint32_t)r->p[r->pos + 2] << 16) | ((uint32_t)r->p[r->pos + 3] << 24);
    r->pos += 4;
    return v;
}

int32_t vs_r_s32(VsReader *r) { return (int32_t)vs_r_u32(r); }

float vs_r_fx(VsReader *r)
{
    int v = vs_r_s16(r);
    return (float)v / (float)VS_FX_ONE;
}

float vs_r_sc(VsReader *r)
{
    int v = vs_r_s16(r);
    return (float)v / (float)VS_SC_ONE;
}

float vs_r_an(VsReader *r)
{
    int v = vs_r_s16(r);
    return (float)v / (float)VS_AN_ONE;
}

void vs_r_str(VsReader *r, char *dst, int cap)
{
    int n = r->len - r->pos;
    if (cap <= 0) return;
    if (n < 0) n = 0;
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, r->p + r->pos, (size_t)n);
    dst[n] = '\0';
    r->pos = r->len;
}

int vs_frame(const uint8_t *buf, int len, int *type, const uint8_t **data, int *data_len)
{
    int size;
    if (len < 3) return 0;
    size = (int)buf[0] | ((int)buf[1] << 8);
    if (size < 1) {                       /* pusta ramka - bledna, pomijamy 2 bajty */
        *type = 0; *data = buf + 2; *data_len = 0;
        return 2;
    }
    if (len < 2 + size) return 0;
    *type = buf[2];
    *data = buf + 3;
    *data_len = size - 1;
    return 2 + size;
}
