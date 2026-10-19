/*
 * vsmic.c - konsolowy klient strumienia audio ReSpeaker XVF3800 (ESP32-S3).
 *
 * Buduje sie zwyklym C (gcc, tcc) na Windows i Linux - patrz Makefile / naglowek nizej.
 *
 *   vsmic <host[:port]> record <plik.wav> [sekundy]   - nagrywa mikrofon do WAV (16-bit PCM)
 *   vsmic <host[:port]> play   <plik.wav>              - wysyla WAV do glosnika
 *   vsmic <host[:port]> volume <0..100>                - ustawia glosnosc
 *   vsmic <host[:port]> gain   <dB>                    - ustawia wzmocnienie mikrofonu
 *   vsmic discover                                     - nasluchuje rozgloszen urzadzen (UDP 7779)
 *
 * Nagrany WAV to 16-bit PCM w formacie urzadzenia (domyslnie 16 kHz mono) - gotowy
 * do podania modelowi AI rozpoznawania mowy/dzwieku (np. 16 kHz mono s16le).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>

#include "audio_proto.h"

#define VS_DISCOVERY_PORT 7779
#define DEFAULT_PORT      7778

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #define CLOSESOCK closesocket
  #define SOCKERR   WSAGetLastError()
  typedef int ssize_t_compat;
  static void msleep(int ms) { Sleep(ms); }
  /* QueryPerformanceCounter zamiast GetTickCount64 - ten drugi nie jest
   * eksportowany w importach kernel32 w TCC28 (blad linkera). QPC dziala
   * i jest monotoniczny; tak samo robi dzialajacy projekt ZdalnaSesjaWektor. */
  static long long now_ms(void)
  {
      static double inv = 0.0;
      LARGE_INTEGER c;
      if (inv == 0.0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); inv = 1000.0 / (double)f.QuadPart; }
      QueryPerformanceCounter(&c);
      return (long long)((double)c.QuadPart * inv);
  }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <errno.h>
  typedef int SOCKET;
  #define INVALID_SOCKET (-1)
  #define CLOSESOCK close
  #define SOCKERR   errno
  static void msleep(int ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
  static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
#endif

static volatile sig_atomic_t g_run = 1;
static void on_sigint(int s) { (void)s; g_run = 0; }

static void net_up(void)
{
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
}

/* ---- wejscie/wyjscie gniazda ---- */
static int send_all(SOCKET s, const void *buf, int len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        int n = send(s, p, len, 0);
        if (n <= 0) return -1;
        p += n; len -= n;
    }
    return 0;
}
static int recv_all(SOCKET s, void *buf, int len)
{
    char *p = (char *)buf;
    while (len > 0) {
        int n = recv(s, p, len, 0);
        if (n == 0) return -1;
        if (n < 0) {
#ifdef _WIN32
            if (SOCKERR == WSAEWOULDBLOCK || SOCKERR == WSAETIMEDOUT) return 1;  /* timeout */
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
            if (errno == EINTR) continue;
#endif
            return -1;
        }
        p += n; len -= n;
    }
    return 0;
}
static int send_frame(SOCKET s, unsigned char type, const void *data, int len)
{
    unsigned char hdr[3] = { type, (unsigned char)(len & 0xFF), (unsigned char)((len >> 8) & 0xFF) };
    if (send_all(s, hdr, 3) < 0) return -1;
    if (len > 0 && send_all(s, data, len) < 0) return -1;
    return 0;
}

/* ---- polaczenie + powitanie ---- */
typedef struct { int rate, channels, bits; } fmt_t;

static SOCKET connect_dev(const char *hostport, fmt_t *fmt)
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
    char portstr[16];
    sprintf(portstr, "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        fprintf(stderr, "nie moge rozwiazac %s:%d\n", host, port);
        return INVALID_SOCKET;
    }
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return INVALID_SOCKET; }
    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        fprintf(stderr, "polaczenie z %s:%d nie powiodlo sie\n", host, port);
        CLOSESOCK(s); freeaddrinfo(res); return INVALID_SOCKET;
    }
    freeaddrinfo(res);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);

    unsigned char h[AUDIO_HELLO_LEN];
    if (recv_all(s, h, sizeof h) != 0 || memcmp(h, AUDIO_MAGIC, 4) != 0) {
        fprintf(stderr, "zle powitanie urzadzenia\n");
        CLOSESOCK(s); return INVALID_SOCKET;
    }
    fmt->channels = h[5];
    fmt->bits = h[6];
    fmt->rate = (int)(h[8] | (h[9] << 8) | (h[10] << 16) | ((unsigned)h[11] << 24));
    fprintf(stderr, "urzadzenie: %d Hz, %d kan., %d-bit\n", fmt->rate, fmt->channels, fmt->bits);
    return s;
}

/* ---- WAV ---- */
static void wav_write_header(FILE *f, int rate, int channels, int bits, uint32_t data_bytes)
{
    uint32_t byte_rate = (uint32_t)rate * channels * (bits / 8);
    uint16_t block_align = (uint16_t)(channels * (bits / 8));
    uint32_t riff = 36 + data_bytes;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fmtlen = 16; uint16_t pcm = 1, ch = (uint16_t)channels, bps = (uint16_t)bits;
    uint32_t sr = (uint32_t)rate;
    fwrite(&fmtlen, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&sr, 4, 1, f); fwrite(&byte_rate, 4, 1, f); fwrite(&block_align, 2, 1, f); fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
}

static int16_t *wav_read(const char *path, int *rate, int *channels, uint32_t *nsamp)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "nie moge otworzyc %s\n", path); return NULL; }
    char tag[4]; uint32_t riff, fmtlen; uint16_t pcm, ch, bps, align; uint32_t sr, br;
    if (fread(tag, 1, 4, f) != 4 || memcmp(tag, "RIFF", 4)) goto bad;
    fread(&riff, 4, 1, f); fread(tag, 1, 4, f);
    if (memcmp(tag, "WAVE", 4)) goto bad;
    int have_fmt = 0; int16_t *data = NULL; uint32_t dbytes = 0;
    for (;;) {
        if (fread(tag, 1, 4, f) != 4) break;
        uint32_t clen; if (fread(&clen, 4, 1, f) != 1) break;
        if (!memcmp(tag, "fmt ", 4)) {
            fmtlen = clen;
            fread(&pcm, 2, 1, f); fread(&ch, 2, 1, f); fread(&sr, 4, 1, f);
            fread(&br, 4, 1, f); fread(&align, 2, 1, f); fread(&bps, 2, 1, f);
            if (fmtlen > 16) fseek(f, (long)(fmtlen - 16), SEEK_CUR);
            if (pcm != 1 || bps != 16) { fprintf(stderr, "obslugiwany tylko PCM 16-bit\n"); fclose(f); return NULL; }
            have_fmt = 1;
        } else if (!memcmp(tag, "data", 4)) {
            dbytes = clen;
            data = (int16_t *)malloc(dbytes ? dbytes : 2);
            if (!data) { fclose(f); return NULL; }
            dbytes = (uint32_t)fread(data, 1, dbytes, f);
            break;
        } else {
            fseek(f, (long)clen + (clen & 1), SEEK_CUR);
        }
    }
    fclose(f);
    if (!have_fmt || !data) goto bad2;
    *rate = (int)sr; *channels = ch; *nsamp = dbytes / 2;
    return data;
bad:
    fclose(f);
bad2:
    fprintf(stderr, "zly plik WAV: %s\n", path);
    return NULL;
}

/* ---- komendy ---- */
static int cmd_record(SOCKET s, fmt_t *fmt, const char *path, double seconds)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "nie moge zapisac %s\n", path); return 1; }
    wav_write_header(f, fmt->rate, fmt->channels, 16, 0);

    /* timeout odbioru 1 s, zeby reagowac na Ctrl-C / limit czasu */
#ifdef _WIN32
    DWORD tv = 1000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof tv);
#else
    struct timeval tv = { 1, 0 }; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
    unsigned char on = 1; send_frame(s, AF_REC, &on, 1);

    uint32_t data_bytes = 0;
    unsigned char *pl = (unsigned char *)malloc(AUDIO_MAX_PAYLOAD);
    long long t0 = now_ms();
    long long limit_ms = (long long)(seconds * 1000.0);
    int last_deg = -1;
    fprintf(stderr, "nagrywam do %s (Ctrl-C konczy)...\n", path);
    while (g_run) {
        unsigned char hdr[3];
        int rc = recv_all(s, hdr, 3);
        if (rc < 0) break;
        if (rc == 1) {                       /* timeout - sprawdz warunki */
            if (seconds > 0 && now_ms() - t0 >= limit_ms) break;
            continue;
        }
        int len = hdr[1] | (hdr[2] << 8);
        if (len > AUDIO_MAX_PAYLOAD) break;
        if (len > 0 && recv_all(s, pl, len) != 0) break;
        if (hdr[0] == AF_MIC) {
            fwrite(pl, 1, len, f);
            data_bytes += len;
        } else if (hdr[0] == AF_DOA && len >= 3) {
            int deg = pl[0] | (pl[1] << 8);
            if (deg != last_deg) { fprintf(stderr, "\rkierunek: %3d stopni  energia %3d%%   ", deg, pl[2] * 100 / 255); fflush(stderr); last_deg = deg; }
        }
        if (seconds > 0 && now_ms() - t0 >= limit_ms) break;
    }
    unsigned char off = 0; send_frame(s, AF_REC, &off, 1);
    wav_write_header(f, fmt->rate, fmt->channels, 16, data_bytes);
    fclose(f);
    free(pl);
    fprintf(stderr, "\nzapisano %u bajtow (%.1f s)\n", data_bytes,
            (double)data_bytes / (fmt->rate * fmt->channels * 2.0));
    return 0;
}

/* Przepróbkowanie mono do dst Hz: w dol - srednia z okna (lagodzi aliasing),
 * w gore - interpolacja liniowa. Zwraca nowy bufor (malloc), *n = liczba probek. */
static int16_t *resample(const int16_t *in, uint32_t nin, int src, int dst, uint32_t *n)
{
    uint32_t nout = (uint32_t)((unsigned long long)nin * dst / src);
    int16_t *out = (int16_t *)malloc((nout ? nout : 1) * 2);
    if (!out) return NULL;
    double r = (double)src / dst;
    for (uint32_t i = 0; i < nout; i++) {
        double a = i * r;
        uint32_t ia = (uint32_t)a;
        if (r > 1.0) {
            uint32_t ib = (uint32_t)((i + 1) * r);
            if (ib > nin) ib = nin;
            if (ib <= ia) ib = ia + 1;
            long sum = 0;
            for (uint32_t k = ia; k < ib; k++) sum += in[k];
            out[i] = (int16_t)(sum / (long)(ib - ia));
        } else {
            double f = a - ia;
            int s0 = in[ia], s1 = ia + 1 < nin ? in[ia + 1] : s0;
            out[i] = (int16_t)(s0 + (s1 - s0) * f);
        }
    }
    *n = nout;
    return out;
}

static int cmd_play(SOCKET s, fmt_t *fmt, const char *path)
{
    int rate, ch; uint32_t nsamp;
    int16_t *data = wav_read(path, &rate, &ch, &nsamp);
    if (!data) return 1;

    /* do mono */
    uint32_t frames = nsamp / (ch > 0 ? ch : 1);
    int16_t *mono = data;
    if (ch == 2) {
        mono = (int16_t *)malloc(frames * 2);
        for (uint32_t i = 0; i < frames; i++) mono[i] = (int16_t)(((int)data[2 * i] + data[2 * i + 1]) / 2);
    }
    /* do czestotliwosci urzadzenia (inaczej zla wysokosc i tempo dzwieku) */
    if (rate > 0 && rate != fmt->rate) {
        fprintf(stderr, "przeprobkowuje %d Hz -> %d Hz\n", rate, fmt->rate);
        uint32_t nn;
        int16_t *rs = resample(mono, frames, rate, fmt->rate, &nn);
        if (rs) { if (mono != data) free(mono); mono = rs; frames = nn; }
    }

    /* Tempo wg zegara bezwzglednego z wyprzedzeniem LEAD_MS: urzadzenie ma bufor 1 s i
     * startuje po 200 ms, wiec trzymamy ~300 ms zapasu. Nie ma kumulowania bledu Sleep()
     * (ziarnistosc ~15 ms w Windows), ktore wczesniej powodowalo niedobory i trzaski. */
    const int CHUNK = 512;                   /* probek na ramke (32 ms @16k) */
    const long long LEAD_MS = 300;
    long long t0 = now_ms();
    fprintf(stderr, "odtwarzam %s (%u probek, %.1f s)...\n", path, frames, (double)frames / fmt->rate);
    for (uint32_t off = 0; off < frames && g_run; off += CHUNK) {
        while (g_run && (long long)off * 1000 / fmt->rate > now_ms() - t0 + LEAD_MS) msleep(5);
        int n = (int)(frames - off); if (n > CHUNK) n = CHUNK;
        if (send_frame(s, AF_PLAY, mono + off, n * 2) < 0) { fprintf(stderr, "polaczenie zerwane\n"); break; }
    }
    /* poczekaj, az urzadzenie dogra bufor (zamkniecie nie przerywa odtwarzania, ale
     * dzieki temu komenda konczy sie razem z dzwiekiem) */
    long long left = (long long)frames * 1000 / fmt->rate + 200 - (now_ms() - t0);
    while (g_run && left > 0) { msleep(left > 50 ? 50 : (int)left); left -= 50; }
    if (mono != data) free(mono);
    free(data);
    return 0;
}

static int cmd_level(SOCKET s, unsigned char type, int val)
{
    unsigned char b = (unsigned char)val;
    int rc = send_frame(s, type, &b, 1);
    msleep(150);
    return rc < 0 ? 1 : 0;
}

static int cmd_discover(void)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(VS_DISCOVERY_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) { fprintf(stderr, "bind UDP %d nie powiodl sie\n", VS_DISCOVERY_PORT); return 1; }
    fprintf(stderr, "nasluchuje rozgloszen urzadzen na UDP %d (Ctrl-C konczy)...\n", VS_DISCOVERY_PORT);
    while (g_run) {
        char buf[256];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        buf[n] = 0;
        printf("%s od %s\n", buf, inet_ntoa(from.sin_addr));
        fflush(stdout);
    }
    CLOSESOCK(s);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "vsmic <host[:port]> record <plik.wav> [sekundy]\n"
        "vsmic <host[:port]> play   <plik.wav>\n"
        "vsmic <host[:port]> volume <0..100>\n"
        "vsmic <host[:port]> gain   <dB>\n"
        "vsmic discover\n"
        "(domyslny port %d)\n", DEFAULT_PORT);
}

int main(int argc, char **argv)
{
    net_up();
    signal(SIGINT, on_sigint);

    if (argc >= 2 && !strcmp(argv[1], "discover")) return cmd_discover();
    if (argc < 3) { usage(); return 2; }

    const char *host = argv[1];
    const char *cmd = argv[2];
    fmt_t fmt = { 16000, 1, 16 };
    SOCKET s = connect_dev(host, &fmt);
    if (s == INVALID_SOCKET) return 1;

    int rc;
    if (!strcmp(cmd, "record") && argc >= 4) {
        rc = cmd_record(s, &fmt, argv[3], argc >= 5 ? atof(argv[4]) : 0);
    } else if (!strcmp(cmd, "play") && argc >= 4) {
        rc = cmd_play(s, &fmt, argv[3]);
    } else if (!strcmp(cmd, "volume") && argc >= 4) {
        rc = cmd_level(s, AF_VOLUME, atoi(argv[3]));
    } else if (!strcmp(cmd, "gain") && argc >= 4) {
        rc = cmd_level(s, AF_GAIN, atoi(argv[3]));
    } else {
        usage();
        rc = 2;
    }
    CLOSESOCK(s);
    return rc;
}
