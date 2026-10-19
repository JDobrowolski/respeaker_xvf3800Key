/*
 * asr_main.c - rozpoznawanie mowy z ReSpeakera XVF3800 (ESP32-S3) modelem Whisper.
 * Wersja Linux (hostCPULinux): POSIX - sockety BSD, pthreads, dlopen (tts.c).
 * Kopia asr/asr_main.c (Windows) - logika ta sama, inne API systemu.
 *
 *   vsasr <ip[:port]> [opcje]     - spi, az urzadzenie uslyszy slowo kluczowe; wtedy
 *                                   rozpoznaje polecenie i wypisuje tekst na stdout
 *   vsasr <ip> -cont              - staly strumien + VAD na hoscie (bez slowa kluczowego)
 *   vsasr -f plik.wav [opcje]     - transkrypcja pliku WAV (16-bit PCM, dowolna czestotliwosc)
 *   vsasr <ip> -say "tekst"       - powiedz tekst glosnikiem ReSpeakera (Piper TTS)
 *   vsasr -saywav "tekst" a.wav   - synteza do pliku
 *
 * Odpowiedz glosowa (gdy zbudowano z -DASR_TTS i jest tts/bin_linux + glos): po rozpoznaniu
 * polecenia host uklada odpowiedz (godzina, data, powitanie... albo -reply program),
 * syntezuje ja Piperem (asr/tts.c) i wysyla do glosnika ramkami AF_PLAY.
 *
 * Strumien z urzadzenia (protokol host/audio_proto.h) to 16 kHz mono s16le - wejscie Whispera.
 * Tryb slowa kluczowego: urzadzenie samo wykrywa slowo i wysyla AF_WAKE, polecenie (AF_MIC)
 * i AF_END - host przez caly czas czeka zablokowany (zero CPU). Tryb -cont: wypowiedzi wycina
 * VAD energetyczny z adaptacyjnym poziomem szumu.
 */
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>

#include "../host/audio_proto.h"
#include "../asr/whisper.h"
#include "../asr/backend.h"
#ifdef ASR_TTS
#include "../asr/tts.h"
#endif

typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket    close
#define MAX_PATH       PATH_MAX

static void msleep(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

/* katalog programu (jak GetModuleFileName bez nazwy pliku) */
static void exe_dir(char *out, size_t n)
{
    ssize_t k = readlink("/proc/self/exe", out, n - 1);
    if (k <= 0) { strcpy(out, "."); return; }
    out[k] = 0;
    char *sl = strrchr(out, '/');
    if (sl) *sl = 0;
}

#define DEFAULT_PORT 7778
#ifdef ASR_GPU
  #define PROG          "vsasr_gpu"
  #define DEFAULT_MODEL "ggml-small.bin"
  #define DEFAULT_AC    0                 /* GPU: zawsze pelny kontekst - najlepsza jakosc */
#else
  #define PROG          "vsasr_cpu"
  #define DEFAULT_MODEL "ggml-large-v3-turbo-q8_0.bin"
  #define DEFAULT_AC    -1                /* CPU: kontekst dopasowany do dlugosci wypowiedzi */
#endif

#define FRAME      320                    /* 20 ms @16 kHz - ramka VAD */
#define PRE_FRAMES 15                     /* 300 ms audio sprzed wykrycia mowy */
#define RING       (16000 * 120)          /* bufor odbioru: 2 min */

static volatile int g_run = 1;
static void on_sigint(int s) { (void)s; g_run = 0; }

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------------------------------------------------------------- opcje */
typedef struct {
    const char *dev, *file, *model;
    int   lang, threads, ac, sil_ms, doa, verbose, cont, ttsgpu;
    const char *tts_voice, *reply, *say, *saywav;
    float vad_db, nsp_max;
} opts_t;

/* ---------------------------------------------------------------- WAV (plik) */
static int16_t *wav_read(const char *path, int *rate, int *channels, uint32_t *nsamp)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "nie moge otworzyc %s\n", path); return NULL; }
    char tag[4]; uint32_t riff, clen; uint16_t pcm = 0, ch = 0, bps = 0, align; uint32_t sr = 0, br;
    int16_t *data = NULL; uint32_t dbytes = 0; int have_fmt = 0;
    if (fread(tag, 1, 4, f) != 4 || memcmp(tag, "RIFF", 4)) goto bad;
    fread(&riff, 4, 1, f); fread(tag, 1, 4, f);
    if (memcmp(tag, "WAVE", 4)) goto bad;
    for (;;) {
        if (fread(tag, 1, 4, f) != 4 || fread(&clen, 4, 1, f) != 1) break;
        if (!memcmp(tag, "fmt ", 4)) {
            fread(&pcm, 2, 1, f); fread(&ch, 2, 1, f); fread(&sr, 4, 1, f);
            fread(&br, 4, 1, f); fread(&align, 2, 1, f); fread(&bps, 2, 1, f);
            if (clen > 16) fseek(f, (long)(clen - 16), SEEK_CUR);
            have_fmt = 1;
        } else if (!memcmp(tag, "data", 4)) {
            data = (int16_t *)malloc(clen ? clen : 2);
            dbytes = (uint32_t)fread(data, 1, clen, f);
            break;
        } else fseek(f, (long)clen + (clen & 1), SEEK_CUR);
    }
    fclose(f);
    if (!have_fmt || !data || pcm != 1 || bps != 16) {
        fprintf(stderr, "%s: obslugiwany tylko WAV PCM 16-bit\n", path);
        free(data); return NULL;
    }
    *rate = (int)sr; *channels = ch; *nsamp = dbytes / 2;
    return data;
bad:
    fclose(f);
    fprintf(stderr, "zly plik WAV: %s\n", path);
    return NULL;
}

/* przeprobkowanie mono float: w dol srednia z okna, w gore interpolacja liniowa */
static float *resample(const float *in, int nin, int src, int dst, int *nout)
{
    int n = (int)((long long)nin * dst / src);
    float *out = (float *)malloc(sizeof(float) * (n ? n : 1));
    double r = (double)src / dst;
    for (int i = 0; i < n; i++) {
        double a = i * r;
        int ia = (int)a;
        if (r > 1.0) {
            int ib = (int)((i + 1) * r);
            if (ib > nin) ib = nin;
            if (ib <= ia) ib = ia + 1;
            double s = 0;
            for (int k = ia; k < ib; k++) s += in[k];
            out[i] = (float)(s / (ib - ia));
        } else {
            double f = a - ia;
            float s0 = in[ia], s1 = ia + 1 < nin ? in[ia + 1] : s0;
            out[i] = (float)(s0 + (s1 - s0) * f);
        }
    }
    *nout = n;
    return out;
}

/* ---------------------------------------------------------------- rozpoznanie */
static whisper *g_w;
static int      g_wake_mode;
static SOCKET   g_sock = INVALID_SOCKET;
static int send_frame(SOCKET s, unsigned char type, const void *data, int len);

#ifdef ASR_TTS
static tts *g_tts;

/* PCM 16 kHz -> glosnik urzadzenia: ramki AF_PLAY z ~300 ms wyprzedzenia (bufor urzadzenia 1 s) */
static void play_pcm(const int16_t *pcm, int n)
{
    if (g_sock == INVALID_SOCKET) return;
    const int CH = 512;
    long long t0 = now_ms();
    for (int off = 0; off < n && g_run; off += CH) {
        while (g_run && (long long)off * 1000 / 16000 > now_ms() - t0 + 300) msleep(5);
        int k = n - off < CH ? n - off : CH;
        if (send_frame(g_sock, AF_PLAY, pcm + off, k * 2) < 0) return;
    }
    long long left = (long long)n * 1000 / 16000 + 250 - (now_ms() - t0);   /* az dogra */
    if (left > 0) msleep((long)left);
}

static void speak(const opts_t *o, const char *text)
{
    if (!g_tts || !text || !*text) return;
    long long t0 = now_ms();
    int n = 0;
    int16_t *pcm = tts_say(g_tts, text, 16000, &n);
    if (!pcm) { fprintf(stderr, "tts: nie udalo sie zsyntezowac\n"); return; }
    if (o->verbose) fprintf(stderr, "[odpowiedz %.1f s, synteza %.2f s] %s\n", n / 16000.0, (now_ms() - t0) / 1000.0, text);
    else fprintf(stderr, "> %s\n", text);
    play_pcm(pcm, n);
    free(pcm);
}

/* male litery ASCII (polskie znaki zostaja - dopasowujemy rdzenie slow) */
static void lower_ascii(const char *s, char *d, int n)
{
    int i = 0;
    for (; s[i] && i < n - 1; i++) d[i] = (s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i];
    d[i] = 0;
}

/* godzina slowami: "dwudziesta dwanascie", "osma zero piec", "dwunasta" - syntezator
 * czyta "20:12" niejednoznacznie */
static void time_words(int h, int m, char *out, int outsz)
{
    static const char *godz[] = { "zero", "pierwsza", "druga", "trzecia", "czwarta", "piąta", "szósta",
        "siódma", "ósma", "dziewiąta", "dziesiąta", "jedenasta", "dwunasta", "trzynasta", "czternasta",
        "piętnasta", "szesnasta", "siedemnasta", "osiemnasta", "dziewiętnasta", "dwudziesta",
        "dwudziesta pierwsza", "dwudziesta druga", "dwudziesta trzecia" };
    static const char *jedn[] = { "zero", "jeden", "dwa", "trzy", "cztery", "pięć", "sześć", "siedem",
        "osiem", "dziewięć", "dziesięć", "jedenaście", "dwanaście", "trzynaście", "czternaście",
        "piętnaście", "szesnaście", "siedemnaście", "osiemnaście", "dziewiętnaście" };
    static const char *dzies[] = { "", "", "dwadzieścia", "trzydzieści", "czterdzieści", "pięćdziesiąt" };
    char mm[64];
    if (m == 0) mm[0] = 0;
    else if (m < 10) snprintf(mm, sizeof mm, " zero %s", jedn[m]);
    else if (m < 20) snprintf(mm, sizeof mm, " %s", jedn[m]);
    else if (m % 10 == 0) snprintf(mm, sizeof mm, " %s", dzies[m / 10]);
    else snprintf(mm, sizeof mm, " %s %s", dzies[m / 10], jedn[m % 10]);
    snprintf(out, outsz, "%s%s", godz[h % 24], mm);
}

/* odpowiedz: program uzytkownika (-reply prog: dostaje plik z tekstem, odpowiedz na stdout)
 * albo wbudowane proste odpowiedzi */
static void make_reply(const opts_t *o, const char *text, char *out, int outsz)
{
    out[0] = 0;
    if (o->reply && !strcmp(o->reply, "off")) return;
    if (o->reply && strcmp(o->reply, "echo")) {
        char tmp[MAX_PATH], cmd[2 * MAX_PATH + 64];
        const char *dir = getenv("TMPDIR");
        snprintf(tmp, sizeof tmp, "%s/vsaXXXXXX", dir && *dir ? dir : "/tmp");
        int fd = mkstemp(tmp);
        if (fd >= 0) {
            FILE *f = fdopen(fd, "wb");
            if (f) { fputs(text, f); fclose(f); } else close(fd);
        }
        snprintf(cmd, sizeof cmd, "\"%s\" \"%s\"", o->reply, tmp);  /* tekst w pliku - nie w linii polecen */
        FILE *p = popen(cmd, "r");
        int k = 0;
        if (p) { k = (int)fread(out, 1, outsz - 1, p); pclose(p); }
        out[k > 0 ? k : 0] = 0;
        unlink(tmp);
        while (k > 0 && (out[k - 1] == '\n' || out[k - 1] == '\r')) out[--k] = 0;
        return;
    }
    char t[1024];
    lower_ascii(text, t, sizeof t);
    time_t now = time(NULL);
    struct tm st;
    localtime_r(&now, &st);
    static const char *dni[] = { "niedziela", "poniedziałek", "wtorek", "środa", "czwartek", "piątek", "sobota" };
    static const char *mies[] = { "stycznia", "lutego", "marca", "kwietnia", "maja", "czerwca", "lipca",
                                  "sierpnia", "września", "października", "listopada", "grudnia" };
    if (!o->reply && strstr(t, "godzin")) {
        char tw[96];
        time_words(st.tm_hour, st.tm_min, tw, sizeof tw);
        snprintf(out, outsz, "Jest godzina %s.", tw);
    }
    else if (!o->reply && (strstr(t, "dzień jest") || strstr(t, "dzisiaj") || strstr(t, "data") || strstr(t, "dziś")))
        snprintf(out, outsz, "Dzisiaj jest %s, %d %s %d roku.", dni[st.tm_wday], st.tm_mday, mies[st.tm_mon], st.tm_year + 1900);
    else if (!o->reply && (strstr(t, "dobry") || strstr(t, "cześć") || strstr(t, "witaj")))
        snprintf(out, outsz, "Dzień dobry! W czym mogę pomóc?");
    else if (!o->reply && strstr(t, "dzięk"))
        snprintf(out, outsz, "Proszę bardzo.");
    else
        snprintf(out, outsz, "Usłyszałem: %s", text);
}
#endif

static void recognize(const opts_t *o, const float *pcm, int n, int doa_deg)
{
    static char text[8192];
    int ac = o->ac;
    if (ac < 0) {                                  /* dopasowany: dlugosc + ~1.3 s zapasu */
        ac = n / 320 + 64;
        if (ac < 256) ac = 256;
        if (ac > 1500) ac = 0;
    }
    float nsp = 0, alp = 0;
    int lang = -1;
    long long t0 = now_ms();
    int nt = wh_transcribe(g_w, pcm, n, o->lang, ac, text, sizeof text, &nsp, &lang, &alp);
    long long dt = now_ms() - t0;
    /* regula Whispera: cisza tylko gdy model mowi "brak mowy" I sam tekst jest niepewny */
    int skip = nt <= 0 || (nsp > o->nsp_max && alp < -1.0f);
    if (o->verbose)
        fprintf(stderr, "[audio %.1f s, rozpoznanie %.2f s, kontekst %d, jezyk %s, brak mowy %.2f, pewnosc %.2f%s]\n",
                n / 16000.0, dt / 1000.0, ac ? ac : 1500, wh_lang_code(lang), nsp, alp,
                skip ? ", pominiete" : "");
    if (skip) {
        if (o->verbose && nt > 0) fprintf(stderr, "  (odrzucone: %s)\n", text);
        return;
    }
    if (o->doa && doa_deg >= 0) printf("[%3d st.] ", doa_deg);
    printf("%s\n", text);
    fflush(stdout);
#ifdef ASR_TTS
    if (g_wake_mode && g_tts) {                     /* odpowiedz glosowa tylko na polecenia */
        static char rep_text[2048];
        make_reply(o, text, rep_text, sizeof rep_text);
        speak(o, rep_text);
    }
#endif
}

static int run_file(const opts_t *o)
{
    int rate, ch; uint32_t ns;
    int16_t *d = wav_read(o->file, &rate, &ch, &ns);
    if (!d) return 1;
    int frames = (int)(ns / (ch > 0 ? ch : 1));
    float *mono = (float *)malloc(sizeof(float) * (frames ? frames : 1));
    for (int i = 0; i < frames; i++) {
        int s = 0;
        for (int c = 0; c < ch; c++) s += d[i * ch + c];
        mono[i] = (float)s / (ch * 32768.0f);
    }
    free(d);
    if (rate != WH_SAMPLE_RATE) {
        int n2;
        float *r = resample(mono, frames, rate, WH_SAMPLE_RATE, &n2);
        free(mono); mono = r; frames = n2;
        fprintf(stderr, "przeprobkowano %d Hz -> 16000 Hz\n", rate);
    }
    fprintf(stderr, "plik %s: %.1f s\n", o->file, frames / 16000.0);
    for (int off = 0; off < frames && g_run; off += WH_MAX_SAMPLES) {
        int n = frames - off;
        if (n > WH_MAX_SAMPLES) n = WH_MAX_SAMPLES;
        recognize(o, mono + off, n, -1);
    }
    free(mono);
    return 0;
}

/* ---------------------------------------------------------------- urzadzenie */
static pthread_mutex_t  g_cs = PTHREAD_MUTEX_INITIALIZER;
static int16_t         *g_ring;
static int              g_rhead, g_rcount;      /* zapis na (head), ile probek czeka */
static volatile int     g_doa = -1, g_alive;
static int              g_dev_rate = 16000, g_flags;
static pthread_mutex_t  g_send_cs = PTHREAD_MUTEX_INITIALIZER;  /* ramki z kilku watkow nie moga sie przeplatac */

/* tryb slowa kluczowego: polecenia skladane przez watek odbioru */
typedef struct { float *x; int n, cap, doa, reason; float score; char kw[40]; } cmd_t;
#define QMAX 8
static cmd_t           *g_q[QMAX], *g_cur;
static int              g_qn;

/* zdarzenie (auto-reset) z watku odbioru do glownego */
static pthread_mutex_t  g_evm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_evc = PTHREAD_COND_INITIALIZER;
static int              g_evf;

static void ev_set(void)
{
    pthread_mutex_lock(&g_evm);
    g_evf = 1;
    pthread_cond_signal(&g_evc);
    pthread_mutex_unlock(&g_evm);
}

static void ev_wait(int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&g_evm);
    while (!g_evf && pthread_cond_timedwait(&g_evc, &g_evm, &ts) == 0) {}
    g_evf = 0;
    pthread_mutex_unlock(&g_evm);
}

static int recv_all(SOCKET s, void *buf, int len)
{
    char *p = (char *)buf;
    while (len > 0) {
        int n = recv(s, p, len, 0);
        if (n <= 0) return -1;
        p += n; len -= n;
    }
    return 0;
}

static int send_frame(SOCKET s, unsigned char type, const void *data, int len)
{
    unsigned char hdr[3] = { type, (unsigned char)(len & 0xFF), (unsigned char)((len >> 8) & 0xFF) };
    int rc = 0;
    pthread_mutex_lock(&g_send_cs);
    if (send(s, (const char *)hdr, 3, MSG_NOSIGNAL) != 3) rc = -1;
    else if (len > 0 && send(s, (const char *)data, len, MSG_NOSIGNAL) != len) rc = -1;
    pthread_mutex_unlock(&g_send_cs);
    return rc;
}

static SOCKET connect_dev(const char *hostport)
{
    char host[128];
    int port = DEFAULT_PORT;
    strncpy(host, hostport, sizeof host - 1);
    host[sizeof host - 1] = 0;
    char *colon = strrchr(host, ':');
    if (colon) { *colon = 0; port = atoi(colon + 1); }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    sprintf(ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) {
        fprintf(stderr, "nie moge rozwiazac %s\n", host);
        return INVALID_SOCKET;
    }
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET || connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        fprintf(stderr, "polaczenie z %s:%d nie powiodlo sie\n", host, port);
        if (s != INVALID_SOCKET) closesocket(s);
        freeaddrinfo(res);
        return INVALID_SOCKET;
    }
    freeaddrinfo(res);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
    unsigned char h[AUDIO_HELLO_LEN];
    if (recv_all(s, h, sizeof h) != 0 || memcmp(h, AUDIO_MAGIC, 4) != 0) {
        fprintf(stderr, "zle powitanie urzadzenia\n");
        closesocket(s);
        return INVALID_SOCKET;
    }
    g_dev_rate = (int)(h[8] | (h[9] << 8) | (h[10] << 16) | ((unsigned)h[11] << 24));
    g_flags = h[4] >= 2 ? h[7] : 0;
    fprintf(stderr, "urzadzenie %s: %d Hz, %d kan., %d-bit%s\n", hostport, g_dev_rate, h[5], h[6],
            (g_flags & AUDIO_FL_KWS) ? ((g_flags & AUDIO_FL_MODEL) ? ", slowo kluczowe" : ", slowo kluczowe BEZ MODELU") : "");
    if (h[5] != 1 || h[6] != 16) fprintf(stderr, "uwaga: oczekiwano mono 16-bit\n");
    return s;
}

static void *rx_thread(void *arg)
{
    SOCKET s = (SOCKET)(size_t)arg;
    unsigned char *pl = (unsigned char *)malloc(AUDIO_MAX_PAYLOAD);
    while (g_run) {
        unsigned char hdr[3];
        if (recv_all(s, hdr, 3) != 0) break;
        int len = hdr[1] | (hdr[2] << 8);
        if (len > AUDIO_MAX_PAYLOAD || (len > 0 && recv_all(s, pl, len) != 0)) break;
        if (hdr[0] == AF_MIC && g_wake_mode) {
            if (!g_cur) continue;                         /* audio poza poleceniem - pomijamy */
            int n = len / 2;
            const int16_t *smp = (const int16_t *)pl;
            if (g_cur->n + n > g_cur->cap) {
                g_cur->cap = (g_cur->n + n) * 2;
                g_cur->x = (float *)realloc(g_cur->x, sizeof(float) * g_cur->cap);
            }
            for (int i = 0; i < n; i++) g_cur->x[g_cur->n++] = smp[i] / 32768.0f;
        } else if (hdr[0] == AF_WAKE && len >= 3) {
            if (g_cur) { free(g_cur->x); free(g_cur); }
            g_cur = (cmd_t *)calloc(1, sizeof(cmd_t));
            g_cur->score = pl[0] / 255.0f;
            g_cur->doa = pl[1] | (pl[2] << 8);
            if (g_cur->doa == 0xFFFF) g_cur->doa = -1;
            int kl = len - 3 < 39 ? len - 3 : 39;
            memcpy(g_cur->kw, pl + 3, kl);
            if (g_cur->doa >= 0) fprintf(stderr, "[slowo \"%s\" %.2f, kierunek %d st.] slucham polecenia...\n", g_cur->kw, g_cur->score, g_cur->doa);
            else fprintf(stderr, "[slowo \"%s\" %.2f] slucham polecenia...\n", g_cur->kw, g_cur->score);
        } else if (hdr[0] == AF_END && len >= 1) {
            if (!g_cur) continue;
            g_cur->reason = pl[0];
            pthread_mutex_lock(&g_cs);
            if (g_qn < QMAX) g_q[g_qn++] = g_cur; else { free(g_cur->x); free(g_cur); }
            pthread_mutex_unlock(&g_cs);
            g_cur = NULL;
            ev_set();
        } else if (hdr[0] == AF_STATUS) {
            fprintf(stderr, "urzadzenie: %.*s\n", len, (char *)pl);
        } else if (hdr[0] == AF_MIC) {
            int n = len / 2;
            const int16_t *smp = (const int16_t *)pl;
            pthread_mutex_lock(&g_cs);
            for (int i = 0; i < n; i++) {
                g_ring[g_rhead] = smp[i];
                g_rhead = (g_rhead + 1) % RING;
                if (g_rcount < RING) g_rcount++;          /* przepelnienie: tracimy najstarsze */
            }
            pthread_mutex_unlock(&g_cs);
        } else if (hdr[0] == AF_DOA && len >= 3) {
            g_doa = pl[0] | (pl[1] << 8);
        }
    }
    free(pl);
    g_alive = 0;
    ev_set();
    return NULL;
}

static cmd_t *cmd_pop(void)
{
    cmd_t *c = NULL;
    pthread_mutex_lock(&g_cs);
    if (g_qn) { c = g_q[0]; memmove(g_q, g_q + 1, sizeof(cmd_t *) * --g_qn); }
    pthread_mutex_unlock(&g_cs);
    return c;
}

/* polecenie po slowie kluczowym -> Whisper */
static void handle_cmd(const opts_t *o, cmd_t *c)
{
    static const char *why[] = { "cisza", "limit czasu", "brak mowy", "przerwane" };
    if (o->verbose) fprintf(stderr, "[polecenie %.1f s, koniec: %s]\n", c->n / 16000.0, why[c->reason & 3]);
    if (c->reason == AF_END_NOSPEECH || c->n < 16000 / 4) {
        fprintf(stderr, "(nic nie powiedziano)\n");
        return;
    }
    if (g_dev_rate != WH_SAMPLE_RATE) {
        int n2;
        float *r = resample(c->x, c->n, g_dev_rate, WH_SAMPLE_RATE, &n2);
        recognize(o, r, n2 > WH_MAX_SAMPLES ? WH_MAX_SAMPLES : n2, c->doa);
        free(r);
    } else {
        recognize(o, c->x, c->n > WH_MAX_SAMPLES ? WH_MAX_SAMPLES : c->n, c->doa);
    }
}

static int ring_pop(int16_t *dst, int max)
{
    pthread_mutex_lock(&g_cs);
    int n = g_rcount < max ? g_rcount : max;
    int tail = (g_rhead - g_rcount + RING) % RING;
    for (int i = 0; i < n; i++) dst[i] = g_ring[(tail + i) % RING];
    g_rcount -= n;
    pthread_mutex_unlock(&g_cs);
    return n;
}

static int run_device(const opts_t *o)
{
    g_ring = (int16_t *)malloc(sizeof(int16_t) * RING);
    float *utt = (float *)malloc(sizeof(float) * WH_MAX_SAMPLES);
    int16_t frame[FRAME], pre[PRE_FRAMES * FRAME];
    int fill = 0, npre = 0;

    /* VAD */
    float nf = 0;                                  /* poziom szumu [dB] */
    int inited = 0, speaking = 0, run = 0, voiced = 0, sil = 0, un = 0, utt_doa = -1;
    long long last_info = 0;

    while (g_run) {
        if (g_sock == INVALID_SOCKET) {
            g_sock = connect_dev(o->dev);
            if (g_sock == INVALID_SOCKET) { msleep(2000); continue; }
            g_wake_mode = !o->cont && (g_flags & AUDIO_FL_KWS);
            if (!g_wake_mode) {
                unsigned char on = 1;
                send_frame(g_sock, AF_REC, &on, 1);
            }
            g_alive = 1;
            pthread_t th;
            if (pthread_create(&th, NULL, rx_thread, (void *)(size_t)g_sock) == 0) pthread_detach(th);
            else g_alive = 0;
            if (g_wake_mode && !(g_flags & AUDIO_FL_MODEL))
                fprintf(stderr, "UWAGA: urzadzenie nie ma modelu slowa kluczowego - wgraj: kwstool upload %s model.kws\n", o->dev);
            fprintf(stderr, g_wake_mode ? "spie - czekam na slowo kluczowe (Ctrl-C konczy)\n"
                                        : "slucham... (Ctrl-C konczy)\n");
        }
        if (!g_alive) {
            fprintf(stderr, "polaczenie zerwane - ponawiam\n");
            closesocket(g_sock);
            g_sock = INVALID_SOCKET;
            msleep(1000);
            continue;
        }

        if (g_wake_mode) {                         /* uspiony do zdarzenia z watku odbioru */
            ev_wait(1000);
            cmd_t *c;
            while ((c = cmd_pop()) != NULL) {
                handle_cmd(o, c);
                free(c->x); free(c);
                if (g_alive) fprintf(stderr, "spie - czekam na slowo kluczowe\n");
            }
            continue;
        }

        int got = ring_pop(frame + fill, FRAME - fill);
        fill += got;
        if (fill < FRAME) { msleep(5); continue; }
        fill = 0;

        double e2 = 0;
        for (int i = 0; i < FRAME; i++) e2 += (double)frame[i] * frame[i];
        float e = (float)(10.0 * log10(e2 / FRAME + 1.0));
        if (!inited) { nf = e; inited = 1; }
        int sp = e > nf + o->vad_db && e > 30.0f;
        if (!sp) nf = e < nf ? 0.7f * nf + 0.3f * e : 0.98f * nf + 0.02f * e;

        if (o->verbose > 1 && now_ms() - last_info > 1000) {
            last_info = now_ms();
            fprintf(stderr, "  poziom %.0f dB, szum %.0f dB%s\n", e, nf, speaking ? ", mowa" : "");
        }

        if (!speaking) {
            if (npre == PRE_FRAMES) { memmove(pre, pre + FRAME, sizeof(int16_t) * FRAME * (PRE_FRAMES - 1)); npre--; }
            memcpy(pre + npre * FRAME, frame, sizeof frame);
            npre++;
            run = sp ? run + 1 : 0;
            if (run >= 3) {                        /* 60 ms mowy - start wypowiedzi */
                speaking = 1; voiced = run; sil = 0; un = 0; utt_doa = g_doa;
                for (int i = 0; i < npre * FRAME; i++) utt[un++] = pre[i] / 32768.0f;
                npre = 0;
            }
            continue;
        }
        for (int i = 0; i < FRAME && un < WH_MAX_SAMPLES; i++) utt[un++] = frame[i] / 32768.0f;
        if (sp) { sil = 0; voiced++; if (g_doa >= 0) utt_doa = g_doa; }
        else sil += 20;
        if (sil >= o->sil_ms || un >= WH_MAX_SAMPLES - FRAME) {
            int keep = un - (sil > 200 ? (sil - 200) * 16 : 0);    /* cisze z konca obcinamy do 200 ms */
            speaking = 0; run = 0;
            if (voiced * 20 < 250) continue;                         /* za krotkie - trzask */
            if (g_dev_rate != WH_SAMPLE_RATE) {
                int n2;
                float *r = resample(utt, keep, g_dev_rate, WH_SAMPLE_RATE, &n2);
                recognize(o, r, n2, utt_doa);
                free(r);
            } else {
                recognize(o, utt, keep, utt_doa);
            }
        }
    }
    if (g_sock != INVALID_SOCKET) {
        unsigned char off = 0;
        send_frame(g_sock, AF_REC, &off, 1);
        closesocket(g_sock);
    }
    free(utt);
    return 0;
}

/* ---------------------------------------------------------------- main */
static void usage(void)
{
    fprintf(stderr,
        "%s <ip[:port]> [opcje]   - czeka na slowo kluczowe, rozpoznaje polecenie, tekst na stdout\n"
        "%s -f plik.wav [opcje]   - transkrypcja pliku WAV\n"
        "opcje:\n"
        "  -m plik.bin   model ggml Whisper (domyslnie <katalog programu>/../models/%s)\n"
        "  -l kod        jezyk: pl, en, de, ... albo auto (domyslnie pl)\n"
        "  -t N          watki CPU (domyslnie wszystkie)\n"
        "  -ac N         kontekst audio 1..1500 (0 = pelny 30 s, -1 = dopasowany; domyslnie %d)\n"
        "  -vad dB       prog mowy ponad szum (domyslnie 9)\n"
        "  -sil ms       cisza konczaca wypowiedz (domyslnie 800)\n"
        "  -nsp p        odrzuc, gdy prawdop. 'brak mowy' > p i tekst niepewny (domyslnie 0.6)\n"
        "  -doa          dopisz kierunek zrodla przed tekstem\n"
        "  -cont         bez slowa kluczowego: staly strumien + VAD na hoscie\n"
        "  -tts glos.onnx  glos Piper (domyslnie ../tts/voices/pl_PL-gosia-medium.onnx); -tts off\n"
        "  -reply r      odpowiedz: wbudowana (domyslnie), echo, off albo program (dostaje plik z tekstem)\n"
        "  -say \"tekst\"  powiedz tekst glosnikiem urzadzenia i zakoncz\n"
        "  -saywav \"tekst\" plik.wav   synteza do pliku (bez urzadzenia)\n"
        "  -v / -vv      czasy i decyzje na stderr / poziomy VAD\n",
        PROG, PROG, DEFAULT_MODEL, DEFAULT_AC);
}

static int file_exists(const char *p) { FILE *f = fopen(p, "rb"); if (f) fclose(f); return f != NULL; }

int main(int argc, char **argv)
{
    opts_t o;
    memset(&o, 0, sizeof o);
    o.lang = 10;                                  /* pl */
    o.ac = DEFAULT_AC; o.sil_ms = 800; o.vad_db = 9.0f; o.nsp_max = 0.6f;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(a, "-f") && more) o.file = argv[++i];
        else if (!strcmp(a, "-m") && more) o.model = argv[++i];
        else if (!strcmp(a, "-l") && more) {
            o.lang = wh_lang_id(argv[++i]);
            if (o.lang == -2) { fprintf(stderr, "nieznany jezyk %s\n", argv[i]); return 2; }
        }
        else if (!strcmp(a, "-t") && more) o.threads = atoi(argv[++i]);
        else if (!strcmp(a, "-ac") && more) o.ac = atoi(argv[++i]);
        else if (!strcmp(a, "-vad") && more) o.vad_db = (float)atof(argv[++i]);
        else if (!strcmp(a, "-sil") && more) o.sil_ms = atoi(argv[++i]);
        else if (!strcmp(a, "-nsp") && more) o.nsp_max = (float)atof(argv[++i]);
        else if (!strcmp(a, "-doa")) o.doa = 1;
        else if (!strcmp(a, "-cont")) o.cont = 1;
        else if (!strcmp(a, "-tts") && more) o.tts_voice = argv[++i];
        else if (!strcmp(a, "-reply") && more) o.reply = argv[++i];
        else if (!strcmp(a, "-say") && more) o.say = argv[++i];
        else if (!strcmp(a, "-saywav") && i + 2 < argc) { o.say = argv[++i]; o.saywav = argv[++i]; }
        else if (!strcmp(a, "-v")) o.verbose = 1;
        else if (!strcmp(a, "-vv")) o.verbose = 2;
        else if (a[0] != '-' && !o.dev) o.dev = a;
        else { usage(); return 2; }
    }
    if (!o.file && !o.dev && !o.saywav) { usage(); return 2; }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGPIPE, SIG_IGN);                     /* zerwane polaczenie: blad send(), nie smierc */

    /* model: -m, albo <katalog programu>/../models/, albo models/ */
    static char mpath[MAX_PATH + 128];
    if (o.model) strncpy(mpath, o.model, sizeof mpath - 1);
    else {
        char exe[MAX_PATH];
        exe_dir(exe, sizeof exe);
        snprintf(mpath, sizeof mpath, "%s/../models/%s", exe, DEFAULT_MODEL);
        if (!file_exists(mpath)) snprintf(mpath, sizeof mpath, "models/%s", DEFAULT_MODEL);
    }

#ifdef ASR_TTS
    if (!o.tts_voice || strcmp(o.tts_voice, "off")) {
        char exe[MAX_PATH], bin[MAX_PATH + 64], voice[MAX_PATH + 64];
        exe_dir(exe, sizeof exe);
        /* biblioteki Linux (libonnxruntime.so, libespeak-ng.so) obok windowsowych DLL w tts/bin */
        snprintf(bin, sizeof bin, "%s/../tts/bin_linux", exe);
        if (o.tts_voice) strncpy(voice, o.tts_voice, sizeof voice - 1), voice[sizeof voice - 1] = 0;
        else snprintf(voice, sizeof voice, "%s/../tts/voices/pl_PL-gosia-medium.onnx", exe);
        if (file_exists(voice)) {
            g_tts = tts_load(voice, bin, o.threads > 0 ? o.threads : 4, 0);
            if (g_tts) fprintf(stderr, "tts: %s\n", tts_info(g_tts));
        } else if (o.tts_voice || o.say) {
            fprintf(stderr, "tts: brak glosu %s (hostCPULinux/pobierz_tts.sh)\n", voice);
        }
    }
    if (o.say) {
        const char *say8 = o.say;                 /* argv w Linuksie jest juz w UTF-8 */
        if (!g_tts) return 1;
        if (o.saywav) {
            int n;
            int16_t *pcm = tts_say(g_tts, say8, 16000, &n);
            if (!pcm) return 1;
            FILE *f = fopen(o.saywav, "wb");
            if (!f) return 1;
            uint32_t db = (uint32_t)n * 2, riff = 36 + db, fl = 16, sr = 16000, br = 32000;
            uint16_t pcmf = 1, ch = 1, al = 2, bps = 16;
            fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fl, 4, 1, f);
            fwrite(&pcmf, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
            fwrite(&al, 2, 1, f); fwrite(&bps, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&db, 4, 1, f);
            fwrite(pcm, 2, n, f); fclose(f);
            fprintf(stderr, "zapisano %s (%.1f s)\n", o.saywav, n / 16000.0);
            return 0;
        }
        g_sock = connect_dev(o.dev);
        if (g_sock == INVALID_SOCKET) return 1;
        speak(&o, say8);
        closesocket(g_sock);
        return 0;
    }
#else
    if (o.say || o.saywav) { fprintf(stderr, "zbudowano bez TTS (brak tts/include - uruchom hostCPULinux/pobierz_tts.sh i przebuduj)\n"); return 1; }
#endif

    if (be_init(o.threads) != 0) return 1;
    fprintf(stderr, "backend: %s\n", be_name());
    long long t0 = now_ms();
    g_w = wh_load(mpath);
    if (!g_w) { fprintf(stderr, "nie udalo sie wczytac modelu (pobierz: hostCPULinux/pobierz_model.sh)\n"); return 1; }
    fprintf(stderr, "model wczytany w %.1f s, jezyk: %s\n", (now_ms() - t0) / 1000.0,
            o.lang < 0 ? "auto" : wh_lang_code(o.lang));

    int rc;
    if (o.file) rc = run_file(&o);
    else rc = run_device(&o);
    wh_free(g_w);
    be_shutdown();
    return rc;
}
