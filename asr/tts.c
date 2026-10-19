/*
 * tts.c - Piper w C: espeak-ng (fonemy) + onnxruntime C API (model VITS).
 * Kroki jak w piper/piper-phonemize:
 *   1. espeak_TextToPhonemesWithTerminator: fonemy IPA zdanie po zdaniu, z interpunkcja,
 *   2. fonemy -> id wg phoneme_id_map z .onnx.json: ^ _ f1 _ f2 _ ... $,
 *   3. model: input [1,N] int64, input_lengths [1], scales [noise, length, noise_w] -> audio,
 *   4. 0,2 s ciszy miedzy zdaniami, normalizacja glosnosci, przeprobkowanie do 16 kHz.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "onnxruntime_c_api.h"
#include "tts.h"

/* ---------------------------------------------------------------- espeak-ng (API C) */
#define AUDIO_OUTPUT_SYNCHRONOUS 2
#define espeakCHARS_UTF8         1
#define espeakPHONEMES_IPA       0x02
#define CLAUSE_INTONATION_COMMA       0x00001000
#define CLAUSE_INTONATION_QUESTION    0x00002000
#define CLAUSE_INTONATION_EXCLAMATION 0x00003000
#define CLAUSE_TYPE_CLAUSE            0x00040000
#define CLAUSE_TYPE_SENTENCE          0x00080000
#define CLAUSE_PERIOD      (40 | CLAUSE_TYPE_SENTENCE)
#define CLAUSE_COMMA       (20 | CLAUSE_INTONATION_COMMA | CLAUSE_TYPE_CLAUSE)
#define CLAUSE_QUESTION    (40 | CLAUSE_INTONATION_QUESTION | CLAUSE_TYPE_SENTENCE)
#define CLAUSE_EXCLAMATION (45 | CLAUSE_INTONATION_EXCLAMATION | CLAUSE_TYPE_SENTENCE)
#define CLAUSE_COLON       (30 | CLAUSE_TYPE_CLAUSE)
#define CLAUSE_SEMICOLON   (30 | CLAUSE_INTONATION_COMMA | CLAUSE_TYPE_CLAUSE)

typedef int (*PFN_espeak_Initialize)(int output, int buflength, const char *path, int options);
typedef int (*PFN_espeak_SetVoiceByName)(const char *name);
typedef const char *(*PFN_espeak_TextToPhonemesWithTerminator)(const void **textptr, int textmode,
                                                               int phonememode, int *terminator);

typedef struct { int cp, id; } phid_t;

struct tts {
    HMODULE es_dll, ort_dll;
    PFN_espeak_TextToPhonemesWithTerminator es_phon;
    const OrtApi *ort;
    OrtEnv *env;
    OrtSession *sess;
    OrtMemoryInfo *mem;
    phid_t *map; int nmap;
    int pad, bos, eos;
    int rate, speakers;
    float noise, length, noise_w;
    char voice[32];
    char info[256];
};

/* ---------------------------------------------------------------- JSON (tylko potrzebne pola) */
static char *read_file(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc(n + 1);
    fread(b, 1, n, f);
    fclose(f);
    b[n] = 0;
    if (len) *len = n;
    return b;
}

static const char *skip_ws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++; return p; }

/* wartosc liczbowa pola "key" (pierwsze wystapienie) */
static double json_num(const char *js, const char *key, double def)
{
    char k[64];
    sprintf(k, "\"%s\"", key);
    const char *p = strstr(js, k);
    if (!p) return def;
    p = skip_ws(p + strlen(k));
    if (*p != ':') return def;
    return strtod(skip_ws(p + 1), NULL);
}

/* napis JSON -> pierwszy znak Unicode (fonem to jeden znak); *pp za cudzyslow konczacy */
static int json_str_cp(const char **pp, char *out, int outsz)
{
    const char *p = *pp;
    if (*p != '"') return -1;
    p++;
    int o = 0, cp0 = -1;
    while (*p && *p != '"') {
        unsigned cp;
        if (*p == '\\') {
            p++;
            if (*p == 'u') {
                cp = (unsigned)strtoul((char[5]){ p[1], p[2], p[3], p[4], 0 }, NULL, 16);
                p += 5;
                if (cp >= 0xD800 && cp < 0xDC00 && p[0] == '\\' && p[1] == 'u') {
                    unsigned lo = (unsigned)strtoul((char[5]){ p[2], p[3], p[4], p[5], 0 }, NULL, 16);
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            } else {
                cp = *p == 'n' ? '\n' : *p == 't' ? '\t' : (unsigned char)*p;
                p++;
            }
        } else {                                      /* UTF-8 wprost */
            unsigned char c = (unsigned char)*p;
            int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
            cp = extra ? (c & (0x3F >> extra)) : c;
            p++;
            for (int i = 0; i < extra && *p; i++) cp = (cp << 6) | ((unsigned char)*p++ & 0x3F);
        }
        if (cp0 < 0) cp0 = (int)cp;
        if (out && o < outsz - 1 && cp < 128) out[o++] = (char)cp;
    }
    if (out) out[o] = 0;
    if (*p == '"') p++;
    *pp = p;
    return cp0;
}

static int load_config(tts *t, const char *path)
{
    char *js = read_file(path, NULL);
    if (!js) { fprintf(stderr, "tts: brak %s\n", path); return -1; }
    t->rate = (int)json_num(js, "sample_rate", 22050);
    t->noise = (float)json_num(js, "noise_scale", 0.667);
    t->length = (float)json_num(js, "length_scale", 1.0);
    t->noise_w = (float)json_num(js, "noise_w", 0.8);
    t->speakers = (int)json_num(js, "num_speakers", 1);
    strcpy(t->voice, "pl");
    const char *v = strstr(js, "\"espeak\"");
    if (v && (v = strstr(v, "\"voice\""))) {
        v = skip_ws(v + 7);
        if (*v == ':') { v = skip_ws(v + 1); json_str_cp(&v, t->voice, sizeof t->voice); }
    }
    const char *p = strstr(js, "\"phoneme_id_map\"");
    if (!p || !(p = strchr(p, '{'))) { free(js); fprintf(stderr, "tts: brak phoneme_id_map\n"); return -1; }
    p++;
    int cap = 256;
    t->map = (phid_t *)malloc(sizeof(phid_t) * cap);
    t->pad = t->bos = t->eos = -1;
    for (;;) {
        p = skip_ws(p);
        if (*p == '}' || !*p) break;
        if (*p == ',') { p++; continue; }
        int cp = json_str_cp(&p, NULL, 0);
        p = skip_ws(p);
        if (cp < 0 || *p != ':') break;
        p = skip_ws(p + 1);
        if (*p != '[') break;
        int id = (int)strtol(p + 1, NULL, 10);
        p = strchr(p, ']');
        if (!p) break;
        p++;
        if (t->nmap == cap) { cap *= 2; t->map = (phid_t *)realloc(t->map, sizeof(phid_t) * cap); }
        t->map[t->nmap].cp = cp; t->map[t->nmap].id = id; t->nmap++;
        if (cp == '_') t->pad = id;
        if (cp == '^') t->bos = id;
        if (cp == '$') t->eos = id;
    }
    free(js);
    if (t->nmap < 10 || t->pad < 0 || t->bos < 0 || t->eos < 0) { fprintf(stderr, "tts: zla mapa fonemow\n"); return -1; }
    return 0;
}

static int phoneme_id(const tts *t, int cp)
{
    for (int i = 0; i < t->nmap; i++) if (t->map[i].cp == cp) return t->map[i].id;
    return -1;
}

/* ---------------------------------------------------------------- ladowanie */
static int ort_ok(const tts *t, OrtStatus *st, const char *what)
{
    if (!st) return 1;
    fprintf(stderr, "tts: onnxruntime %s: %s\n", what, t->ort->GetErrorMessage(st));
    t->ort->ReleaseStatus(st);
    return 0;
}

tts *tts_load(const char *voice, const char *bin_dir, int threads, int gpu)
{
    tts *t = (tts *)calloc(1, sizeof *t);
    char path[MAX_PATH * 2];

    sprintf(path, "%s.json", voice);
    if (load_config(t, path)) { free(t); return NULL; }

    /* DLL z katalogu bin; ich zaleznosci (np. onnxruntime_providers_shared.dll) tez stamtad.
     * SetDllDirectoryA pobierane dynamicznie - nie ma jej w imporcie kernel32 TCC28. */
    typedef BOOL (WINAPI *PFN_SetDllDirectoryA)(LPCSTR);
    PFN_SetDllDirectoryA set_dir = (PFN_SetDllDirectoryA)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetDllDirectoryA");
    if (set_dir) set_dir(bin_dir);
    sprintf(path, "%s\\espeak-ng.dll", bin_dir);
    t->es_dll = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    sprintf(path, "%s\\onnxruntime.dll", bin_dir);
    t->ort_dll = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!t->es_dll || !t->ort_dll) {
        fprintf(stderr, "tts: brak %s w %s (uruchom tts\\pobierz_tts.bat)\n", t->es_dll ? "onnxruntime.dll" : "espeak-ng.dll", bin_dir);
        free(t); return NULL;
    }
    PFN_espeak_Initialize es_init = (PFN_espeak_Initialize)GetProcAddress(t->es_dll, "espeak_Initialize");
    PFN_espeak_SetVoiceByName es_voice = (PFN_espeak_SetVoiceByName)GetProcAddress(t->es_dll, "espeak_SetVoiceByName");
    t->es_phon = (PFN_espeak_TextToPhonemesWithTerminator)GetProcAddress(t->es_dll, "espeak_TextToPhonemesWithTerminator");
    if (!es_init || !es_voice || !t->es_phon) { fprintf(stderr, "tts: espeak-ng.dll bez potrzebnych funkcji (uzyj wersji z Piper)\n"); free(t); return NULL; }
    sprintf(path, "%s\\espeak-ng-data", bin_dir);
    if (es_init(AUDIO_OUTPUT_SYNCHRONOUS, 0, path, 0) < 0) { fprintf(stderr, "tts: espeak_Initialize nie powiodl sie (%s)\n", path); free(t); return NULL; }
    if (es_voice(t->voice) != 0) { fprintf(stderr, "tts: espeak nie zna glosu \"%s\"\n", t->voice); free(t); return NULL; }

    const OrtApiBase *(*get_base)(void) = (const OrtApiBase *(*)(void))GetProcAddress(t->ort_dll, "OrtGetApiBase");
    if (!get_base || !(t->ort = get_base()->GetApi(ORT_API_VERSION))) {
        fprintf(stderr, "tts: onnxruntime.dll za stara (potrzebne API %d)\n", ORT_API_VERSION); free(t); return NULL;
    }
    const OrtApi *o = t->ort;
    OrtSessionOptions *so = NULL;
    if (!ort_ok(t, o->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "tts", &t->env), "CreateEnv") ||
        !ort_ok(t, o->CreateSessionOptions(&so), "SessionOptions")) { free(t); return NULL; }
    if (threads > 0) o->SetIntraOpNumThreads(so, threads);
    o->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL);
    const char *dev = "CPU";
    if (gpu) {
        /* DirectML (GPU przez DirectX 12) - dziala tylko z onnxruntime w wersji DirectML */
        const char *keys[1] = { "device_id" }, *vals[1] = { "0" };
        o->DisableMemPattern(so);
        o->SetSessionExecutionMode(so, ORT_SEQUENTIAL);
        OrtStatus *st = o->SessionOptionsAppendExecutionProvider(so, "DML", keys, vals, 1);
        if (st) { o->ReleaseStatus(st); fprintf(stderr, "tts: brak DirectML w onnxruntime.dll - synteza na CPU\n"); }
        else dev = "GPU (DirectML)";
    }
    wchar_t wpath[MAX_PATH * 2];
    MultiByteToWideChar(CP_UTF8, 0, voice, -1, wpath, MAX_PATH * 2);
    OrtStatus *st = o->CreateSession(t->env, wpath, so, &t->sess);
    o->ReleaseSessionOptions(so);
    if (!ort_ok(t, st, "CreateSession")) { free(t); return NULL; }
    o->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &t->mem);

    const OrtApiBase *b = get_base();
    const char *vn = strrchr(voice, '\\');
    snprintf(t->info, sizeof t->info, "Piper %s, %d Hz, espeak \"%s\", %d fonemow, onnxruntime %s, %s",
             vn ? vn + 1 : voice, t->rate, t->voice, t->nmap, b->GetVersionString(), dev);
    return t;
}

const char *tts_info(const tts *t) { return t->info; }

void tts_free(tts *t)
{
    if (!t) return;
    if (t->sess) t->ort->ReleaseSession(t->sess);
    if (t->mem) t->ort->ReleaseMemoryInfo(t->mem);
    if (t->env) t->ort->ReleaseEnv(t->env);
    free(t->map);
    free(t);
}

/* ---------------------------------------------------------------- synteza */
typedef struct { float *x; int n, cap; } fbuf;
static void fpush(fbuf *b, const float *x, int n)
{
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 4096; b->x = (float *)realloc(b->x, sizeof(float) * b->cap); }
    if (x) memcpy(b->x + b->n, x, sizeof(float) * n); else memset(b->x + b->n, 0, sizeof(float) * n);
    b->n += n;
}

typedef struct { int64_t *v; int n, cap; } ibuf;
static void ipush(ibuf *b, int64_t v)
{
    if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 256; b->v = (int64_t *)realloc(b->v, sizeof(int64_t) * b->cap); }
    b->v[b->n++] = v;
}

/* UTF-8 -> id fonemow z wtracanym padem */
static void add_phonemes(const tts *t, ibuf *ids, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p, cp;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        cp = extra ? (c & (0x3F >> extra)) : c;
        p++;
        for (int i = 0; i < extra && *p; i++) cp = (cp << 6) | (*p++ & 0x3F);
        int id = phoneme_id(t, (int)cp);
        if (id < 0) continue;                             /* nieznany znak - pomijamy */
        ipush(ids, id);
        ipush(ids, t->pad);
    }
}

/* jedno zdanie (id fonemow) -> audio dopisane do out */
static int infer(tts *t, ibuf *ids, fbuf *out)
{
    const OrtApi *o = t->ort;
    int64_t shp[2] = { 1, ids->n }, one = 1, len = ids->n, sid = 0;
    float scales[3] = { t->noise, t->length, t->noise_w };
    OrtValue *in[4] = { 0 }, *res = NULL;
    const char *names[4] = { "input", "input_lengths", "scales", "sid" };
    const char *onames[1] = { "output" };
    int nin = t->speakers > 1 ? 4 : 3, ok = 0;
    if (ort_ok(t, o->CreateTensorWithDataAsOrtValue(t->mem, ids->v, sizeof(int64_t) * ids->n, shp, 2,
                                                   ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in[0]), "input") &&
        ort_ok(t, o->CreateTensorWithDataAsOrtValue(t->mem, &len, sizeof len, &one, 1,
                                                   ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in[1]), "lengths") &&
        ort_ok(t, o->CreateTensorWithDataAsOrtValue(t->mem, scales, sizeof scales, (int64_t[]){ 3 }, 1,
                                                   ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[2]), "scales") &&
        (nin == 3 || ort_ok(t, o->CreateTensorWithDataAsOrtValue(t->mem, &sid, sizeof sid, &one, 1,
                                                   ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in[3]), "sid")) &&
        ort_ok(t, o->Run(t->sess, NULL, names, (const OrtValue *const *)in, nin, onames, 1, &res), "Run")) {
        float *a = NULL;
        OrtTensorTypeAndShapeInfo *info = NULL;
        size_t cnt = 0;
        o->GetTensorMutableData(res, (void **)&a);
        o->GetTensorTypeAndShape(res, &info);
        o->GetTensorShapeElementCount(info, &cnt);
        o->ReleaseTensorTypeAndShapeInfo(info);
        fpush(out, a, (int)cnt);
        ok = 1;
    }
    for (int i = 0; i < 4; i++) if (in[i]) o->ReleaseValue(in[i]);
    if (res) o->ReleaseValue(res);
    return ok;
}

int16_t *tts_say(tts *t, const char *text, int rate, int *n)
{
    fbuf au = { 0 };
    ibuf ids = { 0 };
    const void *tp = text;
    int sentences = 0;
    ipush(&ids, t->bos); ipush(&ids, t->pad);
    while (tp) {
        int term = 0;
        const char *ph = t->es_phon(&tp, espeakCHARS_UTF8, espeakPHONEMES_IPA, &term);
        if (ph) add_phonemes(t, &ids, ph);
        int punct = term & 0x000FFFFF;
        const char *pm = punct == CLAUSE_PERIOD ? "." : punct == CLAUSE_QUESTION ? "?" :
                         punct == CLAUSE_EXCLAMATION ? "!" : punct == CLAUSE_COMMA ? ", " :
                         punct == CLAUSE_COLON ? ": " : punct == CLAUSE_SEMICOLON ? "; " : NULL;
        if (pm) add_phonemes(t, &ids, pm);
        if ((term & CLAUSE_TYPE_SENTENCE) == CLAUSE_TYPE_SENTENCE || !tp) {
            if (ids.n > 2) {
                ipush(&ids, t->eos);
                if (sentences++) fpush(&au, NULL, t->rate / 5);          /* 0,2 s miedzy zdaniami */
                if (!infer(t, &ids, &au)) { free(ids.v); free(au.x); return NULL; }
            }
            ids.n = 0;
            ipush(&ids, t->bos); ipush(&ids, t->pad);
        }
    }
    free(ids.v);
    if (!au.n) { free(au.x); return NULL; }

    /* normalizacja: szczyt -3 dBFS */
    float mx = 0.01f;
    for (int i = 0; i < au.n; i++) { float a = (float)fabs(au.x[i]); if (a > mx) mx = a; }
    float g = 0.7f / mx;
    /* przeprobkowanie (w dol - srednia z okna, w gore - liniowo) */
    int m = (int)((long long)au.n * rate / t->rate);
    int16_t *pcm = (int16_t *)malloc(sizeof(int16_t) * (m ? m : 1));
    double r = (double)t->rate / rate;
    for (int i = 0; i < m; i++) {
        double a = i * r, v;
        int ia = (int)a;
        if (r > 1.0) {
            int ib = (int)((i + 1) * r);
            if (ib > au.n) ib = au.n;
            if (ib <= ia) ib = ia + 1;
            double s = 0;
            for (int k = ia; k < ib; k++) s += au.x[k];
            v = s / (ib - ia);
        } else {
            float s0 = au.x[ia], s1 = ia + 1 < au.n ? au.x[ia + 1] : s0;
            v = s0 + (s1 - s0) * (a - ia);
        }
        v *= g * 32767.0;
        pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    free(au.x);
    *n = m;
    return pcm;
}
