/*
 * vsbridge.cpp - mostek ReSpeaker XVF3800 (ESP32-S3, protokol z audio_proto.h) <-> urzadzenia
 * audio Windows. Dzieki wirtualnym sterownikom audio ReSpeaker staje sie zwyklym mikrofonem
 * i glosnikiem dla dowolnego programu (Teams, Discord, OBS, przegladarka...).
 *
 *   mikrofon ReSpeakera --AF_MIC--> vsbridge --RtAudio--> wyjscie, np. "CABLE Input"
 *                                       (programy nagrywaja z "CABLE Output" = mikrofon)
 *   programy graja na "Virtual Audio Driver" --WASAPI loopback--> vsbridge --AF_PLAY--> glosnik
 *
 * Opcjonalnie zapis do WAV: -recmic (to, co przychodzi z mikrofonu) i -recspk (to, co idzie
 * do glosnika); w trakcie pracy wlaczane/wylaczane tez z konsoli (m / s).
 * Opis, sterowniki i budowanie: README.md obok.
 */
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #define CLOSESOCK closesocket
  typedef int socklen_t;
#else
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <errno.h>
  typedef int SOCKET;
  #define INVALID_SOCKET (-1)
  #define CLOSESOCK close
#endif

#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "RtAudio.h"
#include "audio_proto.h"

#define DEFAULT_PORT 7778

using clk = std::chrono::steady_clock;

static std::atomic<bool> g_run{true};
static void on_sigint(int) { g_run = false; }

static long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(clk::now().time_since_epoch()).count();
}
static void msleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

/* ---- ustawienia z linii polecen ---- */
struct Opts {
    std::string host;
    std::string out = "CABLE Input";             /* wyjscie, na ktore idzie mikrofon ReSpeakera */
    std::string in;                              /* zwykle wejscie -> glosnik ReSpeakera */
    std::string loop = "Virtual Audio Driver";   /* loopback wyjscia -> glosnik ReSpeakera */
    std::string recmic, recspk;
    int mic_ms = 120;                            /* docelowe opoznienie bufora mikrofonu */
    int spk_max_ms = 300;                        /* maks. zaleglosc wysylania do glosnika */
    int gate = 4;                                /* prog ciszy (|probka|), 0 = wysylaj zawsze */
    int volume = -1, gain = -1000;
    bool doa = false, verbose = false;
};
static Opts g_o;

/* ---- bufor kolowy SPSC (jeden producent, jeden konsument) ---- */
class Ring {
public:
    explicit Ring(size_t n) : b_(n) {}
    size_t level() const { return w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire); }
    size_t push(const int16_t *s, size_t n)       /* zwraca liczbe odrzuconych probek */
    {
        size_t w = w_.load(std::memory_order_relaxed);
        size_t room = b_.size() - (w - r_.load(std::memory_order_acquire));
        size_t k = n < room ? n : room;
        for (size_t i = 0; i < k; i++) b_[(w + i) % b_.size()] = s[i];
        w_.store(w + k, std::memory_order_release);
        return n - k;
    }
    size_t pop(int16_t *d, size_t n)
    {
        size_t r = r_.load(std::memory_order_relaxed);
        size_t have = w_.load(std::memory_order_acquire) - r;
        size_t k = n < have ? n : have;
        for (size_t i = 0; i < k; i++) d[i] = b_[(r + i) % b_.size()];
        r_.store(r + k, std::memory_order_release);
        return k;
    }
    void skip(size_t n)                           /* tylko konsument */
    {
        size_t r = r_.load(std::memory_order_relaxed);
        size_t have = w_.load(std::memory_order_acquire) - r;
        r_.store(r + (n < have ? n : have), std::memory_order_release);
    }
private:
    std::vector<int16_t> b_;
    std::atomic<size_t> r_{0}, w_{0};
};

/* ---- zapis WAV (16-bit PCM); naglowek odswiezany co ~1 s, wiec plik jest poprawny
 *      nawet po zabiciu programu ---- */
class WavRec {
public:
    explicit WavRec(const char *what) : what_(what) {}
    ~WavRec() { stop(); }
    bool start(const std::string &path, int rate)
    {
        std::lock_guard<std::mutex> l(m_);
        close_locked();
        f_ = std::fopen(path.c_str(), "wb");
        if (!f_) { std::fprintf(stderr, "nie moge zapisac %s\n", path.c_str()); return false; }
        rate_ = rate; bytes_ = 0; since_hdr_ = 0; path_ = path;
        header();
        std::fprintf(stderr, "[nagrywanie %s] -> %s\n", what_, path.c_str());
        return true;
    }
    void stop()
    {
        std::lock_guard<std::mutex> l(m_);
        close_locked();
    }
    bool active()
    {
        std::lock_guard<std::mutex> l(m_);
        return f_ != nullptr;
    }
    void write(const int16_t *s, size_t n)
    {
        std::lock_guard<std::mutex> l(m_);
        if (!f_ || !n) return;
        std::fwrite(s, 2, n, f_);
        bytes_ += (uint32_t)(n * 2);
        since_hdr_ += n;
        if (since_hdr_ >= (size_t)rate_) { since_hdr_ = 0; header(); std::fseek(f_, 0, SEEK_END); }
    }
private:
    static void put32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
    static void put16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
    void header()
    {
        unsigned char h[44];
        std::memcpy(h, "RIFF", 4); put32(h + 4, 36 + bytes_); std::memcpy(h + 8, "WAVEfmt ", 8);
        put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, 1);            /* PCM, mono */
        put32(h + 24, (uint32_t)rate_); put32(h + 28, (uint32_t)rate_ * 2);
        put16(h + 32, 2); put16(h + 34, 16);
        std::memcpy(h + 36, "data", 4); put32(h + 40, bytes_);
        std::fseek(f_, 0, SEEK_SET);
        std::fwrite(h, 1, sizeof h, f_);
        std::fflush(f_);
    }
    void close_locked()
    {
        if (!f_) return;
        header();
        std::fclose(f_);
        f_ = nullptr;
        std::fprintf(stderr, "[nagrywanie %s] zapisano %s (%.1f s)\n", what_, path_.c_str(),
                     bytes_ / (2.0 * rate_));
    }
    const char *what_;
    std::mutex m_;
    FILE *f_ = nullptr;
    int rate_ = 16000;
    uint32_t bytes_ = 0;
    size_t since_hdr_ = 0;
    std::string path_;
};

static WavRec g_recmic("mikrofon"), g_recspk("glosnik");

static std::string auto_name(const char *prefix)
{
    char b[64];
    std::time_t t = std::time(nullptr);
    std::strftime(b, sizeof b, "_%Y%m%d_%H%M%S.wav", std::localtime(&t));
    return prefix + std::string(b);
}

/* ---- stan wspolny ---- */
static std::atomic<int> g_rate{16000};
static Ring g_mic(48000 * 2);          /* z sieci -> wyjscie audio (2 s zapasu pojemnosci) */
static Ring g_spk(48000 * 2);          /* wejscie / loopback -> siec */
static std::atomic<bool> g_mic_on{false}, g_spk_on{false};

/* statystyki (-v) */
static std::atomic<unsigned> st_mic_under{0}, st_mic_drop{0}, st_spk_drop{0}, st_spk_frames{0};

/* ---- siec ---- */
static std::mutex g_tx;                 /* wysylanie z kilku watkow */
static std::atomic<SOCKET> g_sock{INVALID_SOCKET};
static std::atomic<bool> g_reconly{false};

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
static int send_frame(unsigned char type, const void *data, int len)
{
    std::lock_guard<std::mutex> l(g_tx);
    SOCKET s = g_sock;
    if (s == INVALID_SOCKET) return -1;
    unsigned char hdr[3] = { type, (unsigned char)(len & 0xFF), (unsigned char)((len >> 8) & 0xFF) };
    if (send_all(s, hdr, 3) < 0) return -1;
    if (len > 0 && send_all(s, data, len) < 0) return -1;
    return 0;
}
static int send_u8(unsigned char type, int v) { unsigned char b = (unsigned char)v; return send_frame(type, &b, 1); }

/* 0 = ok, 1 = timeout, -1 = blad / koniec */
static int recv_all(SOCKET s, void *buf, int len)
{
    char *p = (char *)buf;
    while (len > 0) {
        int n = recv(s, p, len, 0);
        if (n == 0) return -1;
        if (n < 0) {
#ifdef _WIN32
            int e = WSAGetLastError();
            if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) return 1;
#else
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
#endif
            return -1;
        }
        p += n; len -= n;
    }
    return 0;
}

static SOCKET connect_dev(const std::string &hostport, int *rate, int *flags)
{
    std::string host = hostport;
    int port = DEFAULT_PORT;
    size_t c = host.rfind(':');
    if (c != std::string::npos) { port = std::atoi(host.c_str() + c + 1); host.resize(c); }

    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        std::fprintf(stderr, "nie moge rozwiazac %s:%d\n", host.c_str(), port);
        return INVALID_SOCKET;
    }
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return INVALID_SOCKET; }
    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        CLOSESOCK(s); freeaddrinfo(res); return INVALID_SOCKET;
    }
    freeaddrinfo(res);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
#ifdef _WIN32
    DWORD tv = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
#else
    timeval tv{3, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
    unsigned char h[AUDIO_HELLO_LEN];
    if (recv_all(s, h, sizeof h) != 0 || std::memcmp(h, AUDIO_MAGIC, 4) != 0) {
        std::fprintf(stderr, "zle powitanie urzadzenia (zajete przez inny program?)\n");
        CLOSESOCK(s); return INVALID_SOCKET;
    }
    *rate = (int)(h[8] | (h[9] << 8) | (h[10] << 16) | ((unsigned)h[11] << 24));
    *flags = h[4] >= 2 ? h[7] : 0;
    if (h[5] != 1 || h[6] != 16)
        std::fprintf(stderr, "uwaga: urzadzenie podaje %d kan. / %d bit - oczekiwano mono 16 bit\n", h[5], h[6]);
    return s;
}

/* Watek sieci: laczy (i laczy ponownie), wlacza staly strumien, odbiera AF_MIC. */
static void net_thread(std::atomic<bool> *hello_ok)
{
    std::vector<unsigned char> pl(AUDIO_MAX_PAYLOAD);
    bool warned = false;
    while (g_run) {
        int rate = 0, flags = 0;
        SOCKET s = connect_dev(g_o.host, &rate, &flags);
        if (s == INVALID_SOCKET) {
            if (!warned) std::fprintf(stderr, "brak polaczenia z %s - ponawiam co 2 s...\n", g_o.host.c_str());
            warned = true;
            for (int i = 0; i < 20 && g_run; i++) msleep(100);
            continue;
        }
        warned = false;
        if (*hello_ok && rate != g_rate)
            std::fprintf(stderr, "uwaga: urzadzenie zmienilo czestotliwosc %d -> %d Hz (uruchom ponownie)\n", g_rate.load(), rate);
        g_rate = rate;
        g_reconly = (flags & AUDIO_FL_RECONLY) != 0;
        g_sock = s;
        std::fprintf(stderr, "polaczono z %s: %d Hz mono%s\n", g_o.host.c_str(), rate,
                     g_reconly ? "" : " (stary firmware: polecenia po slowie kluczowym wmieszaja sie w strumien)");
        *hello_ok = true;

        if (g_o.volume >= 0) send_u8(AF_VOLUME, g_o.volume);
        if (g_o.gain > -1000) send_u8(AF_GAIN, g_o.gain);
        send_u8(AF_REC, g_reconly ? AF_REC_ONLY : 1);

        int last_deg = -1;
        while (g_run) {
            unsigned char hdr[3];
            int rc = recv_all(s, hdr, 3);
            if (rc != 0) {                        /* w trybie stalego strumienia cisza = zerwane */
                std::fprintf(stderr, rc > 0 ? "brak danych od urzadzenia - lacze ponownie\n"
                                            : "polaczenie zerwane - lacze ponownie\n");
                break;
            }
            int len = hdr[1] | (hdr[2] << 8);
            if (len > AUDIO_MAX_PAYLOAD) { std::fprintf(stderr, "zla ramka - lacze ponownie\n"); break; }
            if (len > 0 && recv_all(s, pl.data(), len) != 0) break;
            switch (hdr[0]) {
            case AF_MIC: {
                const int16_t *smp = (const int16_t *)pl.data();
                size_t n = (size_t)len / 2;
                g_recmic.write(smp, n);
                if (g_mic_on) st_mic_drop += (unsigned)g_mic.push(smp, n);
                break;
            }
            case AF_DOA:
                if (g_o.doa && len >= 3) {
                    int deg = pl[0] | (pl[1] << 8);
                    if (deg != last_deg) {
                        std::fprintf(stderr, "kierunek: %3d st.  energia %3d%%\n", deg, pl[2] * 100 / 255);
                        last_deg = deg;
                    }
                }
                break;
            case AF_WAKE:
                if (len >= 3) {
                    std::string kw((const char *)pl.data() + 3, (size_t)(len - 3));
                    std::fprintf(stderr, "[slowo kluczowe] %s (%d%%)\n", kw.c_str(), pl[0] * 100 / 255);
                }
                if (!g_reconly) send_u8(AF_LISTEN, 0);   /* stary firmware: utnij polecenie */
                break;
            case AF_STATUS:
                std::fprintf(stderr, "[urzadzenie] %.*s\n", len, (const char *)pl.data());
                break;
            default:
                break;                            /* AF_END, AF_PING... */
            }
        }
        if (!g_run) send_u8(AF_REC, 0);           /* koniec programu - wylacz strumien */
        {
            std::lock_guard<std::mutex> l(g_tx);
            g_sock = INVALID_SOCKET;
        }
        CLOSESOCK(s);
    }
}

/* Watek nadawania do glosnika: pobiera probki z wejscia/loopbacku, zapisuje je (-recspk)
 * i wysyla AF_PLAY. Cisza (ponizej -gate przez > 300 ms) nie jest wysylana - wtedy bufor
 * urzadzenia sie oproznia, a roznica zegarow PC/ESP32 kasuje sie przy kazdej przerwie. */
static void spk_thread()
{
    const int CHUNK = 320;                   /* 20 ms @ 16 kHz */
    int16_t buf[CHUNK];
    long long silent_ms = 1000;
    while (g_run) {
        int rate = g_rate;
        size_t max_backlog = (size_t)rate * g_o.spk_max_ms / 1000;
        size_t lvl = g_spk.level();
        if (lvl > max_backlog + CHUNK) {     /* siec nie nadazala - wyrzuc najstarsze */
            g_spk.skip(lvl - max_backlog);
            st_spk_drop += (unsigned)(lvl - max_backlog);
        }
        if (g_spk.level() < (size_t)CHUNK) { msleep(5); continue; }
        size_t n = g_spk.pop(buf, CHUNK);
        g_recspk.write(buf, n);

        int peak = 0;
        for (size_t i = 0; i < n; i++) { int a = buf[i] < 0 ? -buf[i] : buf[i]; if (a > peak) peak = a; }
        if (g_o.gate > 0 && peak <= g_o.gate) silent_ms += (long long)n * 1000 / rate;
        else silent_ms = 0;
        if (silent_ms > 300) continue;       /* cisza - nic nie wysylamy */
        if (send_frame(AF_PLAY, buf, (int)n * 2) == 0) st_spk_frames++;
    }
}

/* ---- audio (RtAudio) ---- */
struct OutState { bool playing = false; int ch = 1; };
static OutState g_outst;
struct InState { int ch = 1; };
static InState g_inst;

/* wyjscie: mikrofon ReSpeakera -> urzadzenie Windows (np. CABLE Input) */
static int out_cb(void *outbuf, void *, unsigned int nframes, double, RtAudioStreamStatus, void *)
{
    int16_t *o = (int16_t *)outbuf;
    OutState &st = g_outst;
    size_t target = (size_t)g_rate * g_o.mic_ms / 1000;
    size_t lvl = g_mic.level();
    if (!st.playing && lvl >= target + nframes) st.playing = true;
    if (st.playing && lvl > 3 * target + nframes) {          /* za duzo zaleglosci - dogon */
        g_mic.skip(lvl - target);
        st_mic_drop += (unsigned)(lvl - target);
    }
    size_t got = 0;
    if (st.playing) {
        int16_t tmp[1024];
        while (got < nframes) {
            size_t want = nframes - got < 1024 ? nframes - got : 1024;
            size_t k = g_mic.pop(tmp, want);
            for (size_t i = 0; i < k; i++)
                for (int c = 0; c < st.ch; c++) o[(got + i) * st.ch + c] = tmp[i];
            got += k;
            if (k < want) break;
        }
        if (got < nframes) { st.playing = false; st_mic_under++; }   /* niedobor - znow zbieramy */
    }
    std::memset(o + got * st.ch, 0, (nframes - got) * st.ch * sizeof(int16_t));
    return 0;
}

/* wejscie / loopback: dzwiek z Windows -> glosnik ReSpeakera */
static int in_cb(void *, void *inbuf, unsigned int nframes, double, RtAudioStreamStatus, void *)
{
    const int16_t *in = (const int16_t *)inbuf;
    if (!in) return 0;
    int16_t tmp[4096];
    int ch = g_inst.ch;
    for (unsigned int off = 0; off < nframes; off += 4096) {
        unsigned int k = nframes - off < 4096 ? nframes - off : 4096;
        for (unsigned int i = 0; i < k; i++) {
            int sum = 0;
            for (int c = 0; c < ch; c++) sum += in[(off + i) * ch + c];
            tmp[i] = (int16_t)(sum / ch);
        }
        st_spk_drop += (unsigned)g_spk.push(tmp, k);
    }
    return 0;
}

static bool icontains(const std::string &hay, const std::string &needle)
{
    if (needle.empty()) return false;
    std::string a = hay, b = needle;
    for (auto &c : a) c = (char)std::tolower((unsigned char)c);
    for (auto &c : b) c = (char)std::tolower((unsigned char)c);
    return a.find(b) != std::string::npos;
}

/* want_out: szukamy urzadzenia z wyjsciami; inaczej z wejsciami. 0 = nie znaleziono */
static unsigned int find_dev(RtAudio &a, const std::string &name, bool want_out)
{
    for (unsigned int id : a.getDeviceIds()) {
        RtAudio::DeviceInfo d = a.getDeviceInfo(id);
        if ((want_out ? d.outputChannels : d.inputChannels) > 0 && icontains(d.name, name)) return id;
    }
    return 0;
}

static void list_devices(RtAudio &a)
{
    std::printf("API: %s\n", RtAudio::getApiDisplayName(a.getCurrentApi()).c_str());
    for (unsigned int id : a.getDeviceIds()) {
        RtAudio::DeviceInfo d = a.getDeviceInfo(id);
        std::printf("  [%3u] %-60s  wyj %u  wej %u  %u Hz%s%s\n", id, d.name.c_str(), d.outputChannels,
                    d.inputChannels, d.preferredSampleRate,
                    d.isDefaultOutput ? "  (domyslne wyj.)" : "", d.isDefaultInput ? "  (domyslne wej.)" : "");
    }
    std::printf("Wyjscia (-out) i loopback (-loop) to urzadzenia z 'wyj' > 0, wejscia (-in) z 'wej' > 0.\n");
}

static bool is_none(const std::string &s) { return s.empty() || s == "none" || s == "-"; }

/* ---- konsola ---- */
static void toggle_rec(WavRec &r, const char *prefix, const std::string &arg)
{
    if (r.active() && arg.empty()) { r.stop(); return; }
    r.start(arg.empty() ? auto_name(prefix) : arg, g_rate);
}

static void console_thread()
{
    std::string line;
    while (g_run && std::getline(std::cin, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        char cmd = line[0];
        std::string arg = line.size() > 2 ? line.substr(2) : "";
        switch (cmd) {
        case 'm': toggle_rec(g_recmic, "mic", arg); break;
        case 's': toggle_rec(g_recspk, "spk", arg); break;
        case 'v': if (!arg.empty()) send_u8(AF_VOLUME, std::atoi(arg.c_str())); break;
        case 'g': if (!arg.empty()) send_u8(AF_GAIN, std::atoi(arg.c_str())); break;
        case 'q': g_run = false; break;
        default:
            std::fprintf(stderr,
                "  m [plik.wav]   nagrywanie mikrofonu wl./wyl.\n"
                "  s [plik.wav]   nagrywanie glosnika wl./wyl.\n"
                "  v <0..100>     glosnosc glosnika\n"
                "  g <dB>         wzmocnienie mikrofonu (-12..36)\n"
                "  q              koniec\n");
        }
    }
}

static void usage()
{
    std::fprintf(stderr,
        "vsbridge <ip[:port]> [opcje]       mostek ReSpeaker <-> urzadzenia audio Windows\n"
        "vsbridge -list                     lista urzadzen audio\n"
        "\n"
        "  -out <nazwa>     wyjscie dla mikrofonu ReSpeakera (domyslnie \"CABLE Input\"; none = wylacz)\n"
        "  -loop <nazwa>    loopback wyjscia -> glosnik ReSpeakera (domyslnie \"Virtual Audio Driver\")\n"
        "  -in <nazwa>      zamiast -loop: zwykle wejscie -> glosnik ReSpeakera\n"
        "                   (none = bez glosnika; nazwa = fragment nazwy z -list)\n"
        "  -recmic <plik>   zapis WAV tego, co przychodzi z mikrofonu\n"
        "  -recspk <plik>   zapis WAV tego, co idzie do glosnika\n"
        "  -latency <ms>    bufor mikrofonu (domyslnie 120)\n"
        "  -gate <0..>      prog ciszy dla glosnika (domyslnie 4; 0 = wysylaj zawsze)\n"
        "  -volume <0..100> glosnosc glosnika   -gain <dB> wzmocnienie mikrofonu\n"
        "  -doa             wypisuj kierunek dzwieku   -v  statystyki co 5 s\n"
        "W trakcie pracy: m / s = nagrywanie mikrofonu / glosnika wl.-wyl., q = koniec, h = pomoc.\n");
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    std::signal(SIGINT, on_sigint);

#ifdef _WIN32
    RtAudio audio(RtAudio::WINDOWS_WASAPI);
#else
    RtAudio audio;
#endif
    if (argc >= 2 && !std::strcmp(argv[1], "-list")) { list_devices(audio); return 0; }
    if (argc < 2 || argv[1][0] == '-') { usage(); return 2; }

    g_o.host = argv[1];
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](void) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "brak wartosci dla %s\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "-out") g_o.out = next();
        else if (a == "-loop") { g_o.loop = next(); g_o.in.clear(); }
        else if (a == "-in") { g_o.in = next(); g_o.loop.clear(); }
        else if (a == "-recmic") g_o.recmic = next();
        else if (a == "-recspk") g_o.recspk = next();
        else if (a == "-latency") g_o.mic_ms = std::atoi(next().c_str());
        else if (a == "-gate") g_o.gate = std::atoi(next().c_str());
        else if (a == "-volume") g_o.volume = std::atoi(next().c_str());
        else if (a == "-gain") g_o.gain = std::atoi(next().c_str());
        else if (a == "-doa") g_o.doa = true;
        else if (a == "-v") g_o.verbose = true;
        else { usage(); return 2; }
    }
    if (g_o.mic_ms < 20) g_o.mic_ms = 20;

    /* najpierw polaczenie - czestotliwosc strumieni bierzemy z powitania urzadzenia */
    std::atomic<bool> hello_ok{false};
    std::thread net(net_thread, &hello_ok);
    while (g_run && !hello_ok) msleep(50);
    if (!g_run) { net.join(); return 1; }
    bool ok = true;
    unsigned int rate = (unsigned int)g_rate.load();

    if (!g_o.recmic.empty()) g_recmic.start(g_o.recmic, (int)rate);
    if (!g_o.recspk.empty()) g_recspk.start(g_o.recspk, (int)rate);

    /* dwa osobne strumienie RtAudio: kazdy ze swoim zegarem urzadzenia */
#ifdef _WIN32
    RtAudio aout(RtAudio::WINDOWS_WASAPI), ain(RtAudio::WINDOWS_WASAPI);
#else
    RtAudio aout, ain;
#endif
    if (!is_none(g_o.out)) {
        unsigned int id = find_dev(aout, g_o.out, true);
        if (!id) { std::fprintf(stderr, "nie znalazlem wyjscia \"%s\" (vsbridge -list)\n", g_o.out.c_str()); g_run = false; ok = false; }
        else {
            RtAudio::DeviceInfo d = aout.getDeviceInfo(id);
            RtAudio::StreamParameters p;
            p.deviceId = id;
            p.nChannels = d.outputChannels >= 2 ? 2 : 1;     /* mono kopiowane na L i P */
            g_outst.ch = (int)p.nChannels;
            unsigned int frames = rate / 100;                 /* 10 ms */
            RtAudio::StreamOptions so;
            so.flags = RTAUDIO_MINIMIZE_LATENCY;
            if (aout.openStream(&p, nullptr, RTAUDIO_SINT16, rate, &frames, &out_cb, nullptr, &so) ||
                aout.startStream()) {
                std::fprintf(stderr, "nie moge otworzyc wyjscia %s\n", d.name.c_str());
                g_run = false; ok = false;
            } else {
                g_mic_on = true;
                std::fprintf(stderr, "mikrofon ReSpeakera -> %s (%u kan., bufor %d ms)\n", d.name.c_str(), p.nChannels, g_o.mic_ms);
            }
        }
    }
    bool loop = !is_none(g_o.loop);
    const std::string &src = loop ? g_o.loop : g_o.in;
    if (g_run && !is_none(src)) {
        /* loopback w RtAudio/WASAPI: urzadzenie WYJSCIOWE otwarte jako wejscie */
        unsigned int id = find_dev(ain, src, loop);
        if (!id) { std::fprintf(stderr, "nie znalazlem %s \"%s\" (vsbridge -list)\n", loop ? "wyjscia do loopbacku" : "wejscia", src.c_str()); g_run = false; ok = false; }
        else {
            RtAudio::DeviceInfo d = ain.getDeviceInfo(id);
            unsigned int devch = loop ? d.outputChannels : d.inputChannels;
            RtAudio::StreamParameters p;
            p.deviceId = id;
            p.nChannels = devch >= 2 ? 2 : 1;                /* stereo -> mono (srednia) */
            g_inst.ch = (int)p.nChannels;
            unsigned int frames = rate / 100;
            if (ain.openStream(nullptr, &p, RTAUDIO_SINT16, rate, &frames, &in_cb) || ain.startStream()) {
                std::fprintf(stderr, "nie moge otworzyc %s%s\n", loop ? "loopbacku " : "wejscia ", d.name.c_str());
                g_run = false; ok = false;
            } else {
                g_spk_on = true;
                std::fprintf(stderr, "%s%s -> glosnik ReSpeakera\n", loop ? "loopback " : "", d.name.c_str());
            }
        }
    }
    if (!g_o.recspk.empty() && !g_spk_on) std::fprintf(stderr, "uwaga: -recspk bez zrodla glosnika (-loop/-in) - plik bedzie pusty\n");

    std::thread spk;
    if (g_run && g_spk_on) spk = std::thread(spk_thread);
    std::thread(console_thread).detach();     /* getline blokuje - nie czekamy na niego */
    if (g_run) std::fprintf(stderr, "dziala (h + Enter = pomoc, Ctrl-C / q = koniec)\n");

    long long next_stat = now_ms() + 5000;
    while (g_run) {
        msleep(100);
        if (g_o.verbose && now_ms() >= next_stat) {
            next_stat += 5000;
            std::fprintf(stderr, "[stat] mic bufor %zu ms, niedobory %u, odrzucone %u | glosnik ramek %u, odrzucone %u%s%s\n",
                         g_mic.level() * 1000 / rate, st_mic_under.load(), st_mic_drop.load(),
                         st_spk_frames.load(), st_spk_drop.load(),
                         g_recmic.active() ? " | REC mic" : "", g_recspk.active() ? " | REC spk" : "");
        }
    }

    if (aout.isStreamOpen()) { if (aout.isStreamRunning()) aout.stopStream(); aout.closeStream(); }
    if (ain.isStreamOpen()) { if (ain.isStreamRunning()) ain.stopStream(); ain.closeStream(); }
    if (spk.joinable()) spk.join();
    {
        std::lock_guard<std::mutex> l(g_tx);
        SOCKET s = g_sock;
        if (s != INVALID_SOCKET) shutdown(s, 2);   /* odblokowuje recv w watku sieci */
    }
    net.join();
    g_recmic.stop();
    g_recspk.stop();
    std::fprintf(stderr, "koniec\n");
    std::fflush(stderr);
    std::_Exit(ok ? 0 : 1);                   /* watek konsoli wisi w getline */
}
