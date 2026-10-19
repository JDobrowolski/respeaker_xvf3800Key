/*
 * vsproto.h - protokol "sesji wektorowej" (VS): aplikacja wysyla polecenia
 * (utworz obiekt, przesun, animuj...), podglad rysuje drzewo obiektow i odsyla
 * zdarzenia (mysz, klawiatura, dotyk, kontrolki). Pelna specyfikacja: PROTOKOL.md.
 *
 * Wspolny dla obu stron i celowo minimalny (czyste C99, bez malloc, bez systemu),
 * zeby aplikacja mogla dzialac na mikrokontrolerze (ESP32 itp.).
 *
 * Ramka (little-endian, dowolny strumien bajtow: potok, TCP, UART):
 *     u16 rozmiar   - liczba bajtow PO tym polu (typ + dane), 1..65535
 *     u8  typ       - VS_* (aplikacja -> podglad) albo VS_EV_* (podglad -> aplikacja)
 *     ... dane
 * Nieznane typy sa pomijane, nadmiarowe bajty na koncu danych ignorowane -
 * nowsze wersje moga dopisywac pola na koncu bez psucia starszych.
 *
 * Liczby (stalopozycyjnie, zeby bez FPU bylo tanio):
 *     fx  = s16, 1/8 piksela    (zakres +-4095.875 px)  - wspolrzedne, rozmiary
 *     sc  = s16, 1/256          (zakres +-127.99)       - skala
 *     an  = s16, 1/32 stopnia   (zakres +-1023.97 st.)  - obrot
 *     col = u32 0xAARRGGBB
 *     str = UTF-8 do konca danych ramki (bez zera)
 */
#ifndef VSPROTO_H
#define VSPROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VS_MAGIC    0x53455356u     /* bajty "VSES" */
#define VS_VERSION  1
#define VS_FX_ONE   8               /* 1 px = 8 jednostek fx */
#define VS_SC_ONE   256
#define VS_AN_ONE   32              /* 1 stopien = 32 jednostki an */
#define VS_MAX_FRAME 65535

/* ---- aplikacja -> podglad ---- */
enum {
    VS_HELLO        = 0x01,  /* u32 magic, u8 wersja, u16 szer, u16 wys, col tlo, str tytul */
    VS_COMMIT       = 0x02,  /* koniec paczki zmian - podglad rysuje dopiero po nim */
    VS_RESET        = 0x03,  /* usun wszystkie obiekty */
    VS_EVENT_MASK   = 0x04,  /* u16 maska VS_EM_* */
    VS_PING         = 0x05,  /* u32 znacznik -> podglad odsyla VS_EV_PONG */

    VS_CREATE       = 0x10,  /* u16 id, u16 rodzic, u8 typ, fx x, fx y, geometria typu */
    VS_DELETE       = 0x11,  /* u16 id (razem z dziecmi) */
    VS_SET_POS      = 0x12,  /* u16 id, fx x, fx y */
    VS_MOVE         = 0x13,  /* u16 id, fx dx, fx dy */
    VS_SET_SCALE    = 0x14,  /* u16 id, sc sx, sc sy */
    VS_SET_ROTATION = 0x15,  /* u16 id, an kat */
    VS_SET_FILL     = 0x16,  /* u16 id, col */
    VS_SET_STROKE   = 0x17,  /* u16 id, col, fx szerokosc */
    VS_SET_VISIBLE  = 0x18,  /* u16 id, u8 widoczny */
    VS_SET_TEXT     = 0x19,  /* u16 id, str */
    VS_SET_VALUE    = 0x1A,  /* u16 id, s32 wartosc */
    VS_SET_GEOM     = 0x1B,  /* u16 id, geometria typu (jak w CREATE) */
    VS_RAISE        = 0x1C,  /* u16 id - na wierzch wsrod rodzenstwa */
    VS_SET_FLAGS    = 0x1D,  /* u16 id, u8 flagi VS_OF_* */
    VS_ANIMATE      = 0x1E,  /* u16 id, u8 wlasciwosc, u8 krzywa, u16 ms, wartosci docelowe */
    VS_FOCUS        = 0x1F,  /* u16 id pola tekstowego (0 = zabierz fokus) */

    /* pliki (uklady ramek jak VS_EV_FILE_* ponizej) */
    VS_FILE_OFFER   = 0x20,  /* u16 xid, u32 rozmiar, u8 flagi VS_FF_*, str nazwa */
    VS_FILE_ACCEPT  = 0x21,  /* u16 xid, u16 kawalek (maks. bajtow DATA), u8 okno (kawalkow w drodze) */
    VS_FILE_DATA    = 0x22,  /* u16 xid, u32 pozycja, dane */
    VS_FILE_ACK     = 0x23,  /* u16 xid, u32 odebrano (bajtow od poczatku) */
    VS_FILE_END     = 0x24,  /* u16 xid, u32 crc32 calego pliku, u32 dlugosc (od wersji ze strumieniami) */
    VS_FILE_DONE    = 0x25,  /* u16 xid, u8 status VS_FS_* (odbiorca po END) */
    VS_FILE_CANCEL  = 0x26,  /* u16 xid, u8 status - odrzucenie / przerwanie (obie strony) */
    VS_FILE_PICK    = 0x27,  /* u16 xid (0x8000..0xBFFF), u8 flagi VS_FF_*, str wzorce ("*.txt;*.csv", "" = wszystkie) */

    VS_SET_FONT     = 0x28,  /* u16 id (0 = domyslny dla wszystkich kontrolek), u8 rozmiar px (0 = domyslny)
                                - font bitmapowy kontrolki, patrz nizej */

    /* pamiec podgladu (karta SD) i nagrywanie - patrz "Pamiec podgladu" nizej; tylko gdy
     * EV_HELLO ma VS_CAP_STORAGE / VS_CAP_CAPTURE (starszy podglad pomija te ramki) */
    VS_FS_LIST      = 0x29,  /* u16 req, u16 maks. wpisow (0 = domyslnie), u32 od ktorego, str sciezka
                                -> VS_EV_FS_LIST (jedna lub kilka ramek) */
    VS_FS_GET       = 0x2A,  /* u16 xid (0x8000..0xBFFF), str sciezka -> VS_EV_FILE_OFFER (dalej jak
                                po VS_FILE_PICK) albo VS_EV_FILE_CANCEL ze statusem */
    VS_FS_PUT       = 0x2B,  /* u16 xid (1..0x7FFF), u32 rozmiar, u8 flagi VS_FF_*, u8 tryb VS_FM_*,
                                str sciezka -> VS_EV_FILE_ACCEPT (dalej jak po VS_FILE_OFFER) albo CANCEL */
    VS_FS_OP        = 0x2C,  /* u16 req, u8 operacja VS_FO_*, u8 n, n bajtow sciezki, str druga sciezka
                                (RENAME: nowa) -> VS_EV_FS_RESULT */
    VS_CAPTURE      = 0x2D,  /* u16 req, u8 akcja VS_CPA_*, u8 zrodlo VS_CS_*, u32 probki/s, u8 kanaly,
                                u8 bity, str sciezka pliku -> VS_EV_CAPTURE */

    /* kanal audio (vsaudio.h): podglad laczy sie z aplikacja osobnym polaczeniem TCP */
    VS_AUDIO_CONNECT = 0x2E, /* u32 token, u16 port, str host ("" = ten sam co sesja; port 0 = rozlacz)
                                -> VS_EV_AUDIO; tylko gdy EV_HELLO ma VS_CAP_AUDIO */

    /* dzwiek prosto z / na pamiec podgladu (bez sieci w trakcie) - gdy opis VS_EV_DEVICE
     * ma "sdaudio=1" (bity mozliwosci sie skonczyly) */
    VS_PLAYER       = 0x2F,  /* u16 req, u8 akcja VS_PLA_*, str sciezka WAV na pamieci podgladu -> VS_EV_PLAYER */
    VS_DEV_LEVEL    = 0x30,  /* u8 rodzaj VS_LV_*, u8 wartosc - glosnosc (%) / wzmocnienie mikrofonow (dB)
                                dla odtwarzania i nagrywania na pamieci podgladu */

    /* siec Wi-Fi urzadzenia z vsapp (np. ESP32 jako punkt dostepowy vsapp_*) */
    VS_NET_RESULT   = 0x31,  /* u8 status VS_NR_*, str komunikat - odpowiedz na VS_EV_NET_CONFIG */

    /* obrazy i plotno rastrowe - gdy opis VS_EV_DEVICE ma "img=jpeg,..." / "canvas=1" */
    VS_IMG_BEGIN    = 0x32,  /* u16 obraz (1..VS_MAX_IMAGES), u8 format VS_IMF_*, u32 rozmiar danych,
                                u16 szer, u16 wys (tylko VS_IMF_RGB565) - poczatek wysylania obrazu */
    VS_IMG_DATA     = 0x33,  /* u16 obraz, u32 pozycja, dane (kawalki w dowolnej kolejnosci) */
    VS_IMG_END      = 0x34,  /* u16 obraz -> podglad dekoduje i odsyla VS_EV_IMAGE */
    VS_IMG_FREE     = 0x35,  /* u16 obraz (0 = wszystkie) - zwolnij pamiec obrazu */
    VS_DRAW         = 0x36,  /* u16 id plotna (VS_T_CANVAS), polecenia VS_D_* do konca ramki -
                                rysowanie bezposrednio w pikselach plotna (zostaje w nim) */

    /* kamera podgladu - gdy opis VS_EV_DEVICE ma "camera=SZERxWYS" */
    VS_CAMERA       = 0x37   /* u16 req, u8 akcja VS_CMA_*, u8 skala (1 = pelna, 2 = polowa), u8 jakosc JPEG
                                1..100, u8 maks. klatek/s (0 = bez limitu), u16 kawalek (maks. bajtow danych
                                w jednym VS_EV_CAMERA_DATA), u32 klatka (tylko VS_CMA_ACK) -> VS_EV_CAMERA */
};

/* Fonty bitmapowe podgladu. Podglad moze miec kilka rozmiarow jednego kroju
 * (Tab5: Courier New 16 i 22 px); lista jest w opisie VS_EV_DEVICE: "bmfonts=16,22".
 *   - VS_T_TEXT: rozmiar z CREATE wybiera font - najblizszy dostepny rozmiar
 *     bitmapowy (podglad z fontem wektorowym rysuje wektorowo, gdy rozmiar
 *     odbiega od bitmapowych albo tekst jest skalowany/obracany).
 *   - kontrolki (przycisk, checkbox, pasek postepu, pole tekstowe): VS_SET_FONT.
 *     id 0 ustawia domyslny font kontrolek calej sesji, id obiektu - tylko jego
 *     (0 = wraca do domyslnego sesji). Brak dokladnego rozmiaru -> najblizszy.
 *     Podglad bez VS_SET_FONT pomija polecenie - kontrolki zostaja w swoim foncie. */

/* ---- podglad -> aplikacja ---- */
enum {
    VS_EV_HELLO   = 0x81,  /* u32 magic, u8 wersja, u16 szer, u16 wys, u8 mozliwosci VS_CAP_*, str nazwa */
    VS_EV_RESIZE  = 0x82,  /* u16 szer, u16 wys */
    VS_EV_POINTER = 0x83,  /* u8 akcja, u8 wskaznik, u8 przyciski, u8 mod, fx x, fx y, u16 cel, fx lx, fx ly */
    VS_EV_WHEEL   = 0x84,  /* s16 dx, s16 dy (1/120 zabka), fx x, fx y, u16 cel, u8 mod */
    VS_EV_KEY     = 0x85,  /* u8 wcisniety, u8 mod, u16 klawisz VSK_* */
    VS_EV_CHAR    = 0x86,  /* u32 kod znaku Unicode */
    VS_EV_CONTROL = 0x87,  /* u16 id, u8 akcja VS_CA_*, s32 wartosc */
    VS_EV_PONG    = 0x88,  /* u32 znacznik z VS_PING */
    VS_EV_MOVED   = 0x89,  /* u16 id, fx x, fx y, u8 koniec - obiekt przeciagniety lokalnie */
    VS_EV_CLOSE   = 0x8A,  /* okno podgladu zamykane */
    VS_EV_TEXT    = 0x8B,  /* u16 id, u8 akcja VS_TA_*, str tekst - pole tekstowe (VS_T_EDIT) */
    VS_EV_DEVICE  = 0x8C,  /* u8 klasa VS_DC_*, u16 ppi (0 = nieznane), str opis "klucz=wartosc;..."
                              - wysylane PRZED VS_EV_HELLO, patrz nizej */

    /* pliki - te same uklady co VS_FILE_* */
    VS_EV_FILE_OFFER  = 0x90,
    VS_EV_FILE_ACCEPT = 0x91,
    VS_EV_FILE_DATA   = 0x92,
    VS_EV_FILE_ACK    = 0x93,
    VS_EV_FILE_END    = 0x94,
    VS_EV_FILE_DONE   = 0x95,
    VS_EV_FILE_CANCEL = 0x96,

    /* pamiec podgladu i nagrywanie */
    VS_EV_FS_LIST     = 0x97,  /* u16 req, u8 status, u8 flagi VS_FL_*, wpisy do konca ramki:
                                  u8 rodzaj VS_FK_*, u32 rozmiar, u32 czas (s od 1970), u8 n, n bajtow nazwy */
    VS_EV_FS_RESULT   = 0x98,  /* u16 req, u8 operacja, u8 status, u8 rodzaj VS_FK_*, u32 a, u32 b
                                  (STAT: rozmiar, czas; INFO: KiB calosc, KiB wolne) */
    VS_EV_CAPTURE     = 0x99,  /* u16 req (0 = z inicjatywy podgladu), u8 status, u8 stan VS_CPS_*,
                                  u8 zrodlo, u32 bajty zapisane, u32 ms nagrania, u32 zgubione probki,
                                  str sciezka pliku */
    VS_EV_AUDIO       = 0x9A,  /* u32 token, u8 stan VS_AU_* - polaczenie audio podgladu */
    VS_EV_PLAYER      = 0x9B,  /* u16 req (0 = z inicjatywy podgladu), u8 status, u8 stan VS_CPS_*,
                                  u32 pozycja ms, u32 dlugosc ms, u32 niedobory, str sciezka */
    VS_EV_NET_CONFIG  = 0x9C,  /* u8 flagi (0), u8 n, n bajtow SSID, str haslo -> VS_NET_RESULT;
                                  urzadzenie zapamietuje siec i laczy sie z nia (pusty SSID = zapomnij, AP) */
    VS_EV_IMAGE       = 0x9D,  /* u16 obraz, u8 status VS_IS_*, u16 szer, u16 wys - po VS_IMG_END */
    VS_EV_CAMERA      = 0x9E,  /* u16 req, u8 status VS_CMS_*, u8 stan (1 = strumien trwa), u32 klatka,
                                  u32 rozmiar JPEG (0 = tylko stan/blad), u16 szer, u16 wys, u32 ms od startu */
    VS_EV_CAMERA_DATA = 0x9F   /* u16 req, u32 klatka, u32 pozycja, dane JPEG (kawalki po kolei) */
};
#define VS_PLA_START  1             /* VS_PLAYER: akcje */
#define VS_PLA_STOP   2
#define VS_PLA_STATUS 3
#define VS_LV_VOLUME   1            /* VS_DEV_LEVEL: rodzaj */
#define VS_LV_MIC_GAIN 2
#define VS_AU_CONNECTING 1          /* stan polaczenia audio (VS_EV_AUDIO) */
#define VS_AU_CONNECTED  2
#define VS_AU_FAILED     3          /* nie udalo sie polaczyc (podglad probuje dalej co kilka s) */
#define VS_AU_CLOSED     4

/* Siec urzadzenia z vsapp (VS_EV_NET_CONFIG -> VS_NET_RESULT). Urzadzenie bez sieci
 * jest punktem dostepowym "vsapp_<nazwa>" (serwer na bramie, port 7777); podglad
 * przekazuje mu SSID i haslo swojej sieci. Po VS_NR_OK urzadzenie zamyka sesje i
 * przechodzi do tej sieci; tam oglasza sie rozgloszeniem UDP na porcie VS_DISCOVERY_PORT:
 *     "VSAPP1 name=<nazwa>;port=<tcp>" (ASCII, co ~2 s). Aplikacja bez obslugi pomija
 * ramke - podglad konczy wtedy czekanie po kilku sekundach. */
#define VS_NR_OK        0          /* zapisano - urzadzenie zaraz laczy sie z ta siecia */
#define VS_NR_REJECTED  1          /* aplikacja nie pozwala zmieniac sieci */
#define VS_NR_INVALID   2          /* SSID / haslo za dlugie albo za krotkie */
#define VS_NR_IO_ERROR  3          /* nie udalo sie zapisac */
#define VS_DISCOVERY_PORT 7779
#define VS_DISCOVERY_MAGIC "VSAPP1 "

/* Pamiec podgladu (np. karta SD w Tab5) - aplikacja przeglada ja i przenosi pliki:
 *   FS_LIST(sciezka)   -> EV_FS_LIST: wpisy w ramkach do ~480 B (zmieszcza sie w buforze 512 B);
 *                         ostatnia ma VS_FL_LAST, a VS_FL_MORE = sa dalsze wpisy (kolejne FS_LIST
 *                         z "od ktorego" = liczba juz odebranych). Blad: jedna ramka ze statusem.
 *   FS_GET(xid, sciezka) -> podglad wysyla plik: OFFER(xid, rozmiar, nazwa) albo CANCEL(xid, status)
 *   FS_PUT(xid, sciezka) -> podglad zapisuje plik: ACCEPT albo CANCEL; dalej DATA/END/DONE jak zwykle
 *                         (rozmiar VS_SIZE_UNKNOWN = strumien, np. log dopisywany na karte)
 *   FS_OP(op, sciezka)   -> EV_FS_RESULT
 * Sciezki: UTF-8, '/' miedzy katalogami, wzgledem korzenia pamieci ("/" albo "" = korzen).
 * Podglad odrzuca ".." i znaki zastrzezone (VS_FS_BAD_PATH) - aplikacja nie wyjdzie poza pamiec.
 * Nagrywanie (CAPTURE): podglad sam zbiera dane ze zrodla i zapisuje je na karte; START/STOP/STATUS
 * odpowiada EV_CAPTURE, a w trakcie nagrywania podglad wysyla je sam (req 0) co ~1 s i przy zmianie
 * stanu (np. koniec miejsca). Nagrywanie trwa dalej po zerwaniu polaczenia. */
#define VS_FL_LAST       0x01       /* EV_FS_LIST: ostatnia ramka odpowiedzi */
#define VS_FL_MORE       0x02       /* EV_FS_LIST: katalog ma dalsze wpisy (limit odpowiedzi) */
#define VS_FK_FILE       1          /* rodzaj wpisu */
#define VS_FK_DIR        2
#define VS_FM_CREATE     0          /* FS_PUT: nowy plik albo nadpisanie */
#define VS_FM_APPEND     1          /* FS_PUT: dopisywanie na koncu (plik tworzony, gdy go nie ma) */
#define VS_FM_NEW        2          /* FS_PUT: tylko nowy plik - istniejacy = VS_FS_EXISTS */
#define VS_FO_STAT       1          /* FS_OP: rodzaj, rozmiar, czas */
#define VS_FO_MKDIR      2
#define VS_FO_REMOVE     3          /* plik albo pusty katalog */
#define VS_FO_RENAME     4          /* sciezka -> druga sciezka */
#define VS_FO_INFO       5          /* pojemnosc: a = KiB calosc, b = KiB wolne */
#define VS_CPA_START     1          /* CAPTURE: akcje */
#define VS_CPA_STOP      2
#define VS_CPA_STATUS    3
#define VS_CS_TEST_I2S   1          /* zrodlo: symulowany grabber I2S (sygnal testowy w czasie rzeczywistym) -> WAV */
#define VS_CS_MIC        2          /* zrodlo: mikrofony podgladu -> WAV; pole "kanaly" = MASKA kanalow sprzetu
                                       (Tab5: bit0 MIC-L, bit1 AEC, bit2 MIC-R, bit3 MIC-HP; 0 = MIC-L+MIC-R) */
#define VS_CPS_IDLE      0          /* stan nagrywania */
#define VS_CPS_RUNNING   1

/* Przesylanie plikow (w obie strony, przeplatane z ruchem UI):
 *   nadawca: OFFER            odbiorca: ACCEPT(kawalek, okno) albo CANCEL(REJECTED)
 *   nadawca: DATA, DATA...    odbiorca: ACK(odebrano) po kazdym kawalku
 *   nadawca: END(crc32)       odbiorca: DONE(status)
 * Nadawca ma w drodze najwyzej okno*kawalek bajtow ponad ostatni ACK, wiec maly
 * odbiorca (ESP32) sam ustala tempo i rozmiar bufora. Odbiorca moze potwierdzac
 * dopiero ZUZYTE bajty (np. wykonane linie G-code) - wtedy okno*kawalek to caly
 * bufor, jakiego potrzebuje. CANCEL - w kazdej chwili.
 * Rozmiar VS_SIZE_UNKNOWN = strumien: dane wysylane, gdy sie pojawiaja (log z czujnikow),
 * koniec - END z CRC i dlugoscia.
 * Numery transferow (xid) - jedna przestrzen, bez kolizji:
 *   0x0001..0x7FFF  aplikacja -> podglad (numeruje aplikacja)
 *   0x8000..0xBFFF  podglad -> aplikacja na prosbe aplikacji (VS_FILE_PICK, numeruje aplikacja)
 *   0xC000..0xFFFF  podglad -> aplikacja z inicjatywy podgladu (przeciagniety plik; numeruje podglad) */
#define VS_SIZE_UNKNOWN  0xFFFFFFFFu  /* OFFER: rozmiar nieznany - strumien (log, pomiary); koniec: END */
#define VS_XID_PICK      0x8000
#define VS_XID_VIEWER    0xC000
#define VS_FF_TEXT       0x01       /* plik tekstowy (UTF-8) - tylko informacja */
#define VS_FS_OK           0        /* statusy DONE / CANCEL */
#define VS_FS_CANCELLED    1        /* przerwany przez uzytkownika / nadawce */
#define VS_FS_REJECTED     2        /* odbiorca nie chce pliku */
#define VS_FS_IO_ERROR     3        /* blad odczytu / zapisu */
#define VS_FS_CRC_ERROR    4        /* suma kontrolna albo dlugosc sie nie zgadza */
#define VS_FS_PROTOCOL     5        /* zla pozycja / za duzy kawalek / nieznany xid */
#define VS_FS_BUSY         6        /* za duzo transferow naraz */
#define VS_FS_TOO_BIG      7        /* plik za duzy dla odbiorcy */
#define VS_FS_DISCONNECTED 8        /* tylko lokalnie: polaczenie zerwane */
#define VS_FS_NOT_FOUND    9        /* pamiec podgladu: nie ma takiego pliku / katalogu */
#define VS_FS_EXISTS       10       /* juz istnieje */
#define VS_FS_NO_MEDIA     11       /* brak karty / pamiec niedostepna */
#define VS_FS_BAD_PATH     12       /* zla sciezka (".." albo znaki zastrzezone) */
#define VS_FS_FULL         13       /* brak miejsca */
#define VS_FS_NOT_EMPTY    14       /* katalog nie jest pusty */

/* typy obiektow i ich geometria w CREATE / SET_GEOM */
enum {
    VS_T_GROUP    = 1,   /* (brak) */
    VS_T_RECT     = 2,   /* fx x0, fx y0, fx w, fx h, fx promien */
    VS_T_ELLIPSE  = 3,   /* fx cx, fx cy, fx rx, fx ry */
    VS_T_PATH     = 4,   /* polecenia sciezki VS_P_* do konca danych */
    VS_T_TEXT     = 5,   /* fx rozmiar, u8 wyrownanie VS_AL_*, str */
    VS_T_BUTTON   = 6,   /* fx w, fx h, u8 styl (bit0 wyrozniony, bity 4-7 ikona), str etykieta */
    VS_T_CHECKBOX = 7,   /* fx w, fx h, u8 zaznaczony, str etykieta */
    VS_T_SLIDER   = 8,   /* fx w, fx h, s32 min, s32 max, s32 krok, s32 wartosc */
    VS_T_PROGRESS = 9,   /* fx w, fx h, s32 max (<= 0: nieokreslony), s32 wartosc, str etykieta */
    VS_T_EDIT     = 10,  /* fx w, fx h, u8 flagi VS_ED_*, u16 limit bajtow (0 = bez), u8 n, n bajtow
                            podpowiedzi, str tekst - pole tekstowe edytowane lokalnie w podgladzie */
    VS_T_IMAGE    = 11,  /* u16 obraz (VS_IMG_*), fx w, fx h (0 = naturalny rozmiar) - lewy gorny rog
                            w (x, y) obiektu; skala/obrot jak inne obiekty, krycie = alfa wypelnienia */
    VS_T_CANVAS   = 12   /* u16 szer px, u16 wys px, col tlo - plotno rastrowe: VS_DRAW i rysowanie
                            palcem (VS_D_PEN); zawartosc znika z obiektem */
};

/* Obrazy: aplikacja wysyla dane kawalkami (BEGIN, DATA..., END), podglad dekoduje je
 * do swojej pamieci i odpowiada VS_EV_IMAGE. Obraz pokazuje obiekt VS_T_IMAGE (w dowolnym
 * miejscu, wiele razy) albo VS_D_IMAGE wkleja go w plotno. Ten sam numer = zastap obraz
 * (obiekty, ktore go pokazuja, odswiezaja sie same). JPEG: baseline (bez progresywnego),
 * skala szarosci albo YCbCr. RGB565: piksele little-endian, wiersz po wierszu. */
#define VS_MAX_IMAGES     255
#define VS_IMF_JPEG       1
#define VS_IMF_RGB565     2
#define VS_IS_OK          0
#define VS_IS_BAD         1        /* uszkodzone dane / nie da sie zdekodowac */
#define VS_IS_UNSUPPORTED 2        /* np. JPEG progresywny, CMYK */
#define VS_IS_NO_MEMORY   3
#define VS_IS_TOO_BIG     4        /* rozmiar danych albo obrazu ponad limit podgladu */
#define VS_IS_PROTOCOL    5        /* DATA/END bez BEGIN, zla pozycja */

/* Plotno (VS_T_CANVAS) - polecenia w ramce VS_DRAW, wspolrzedne w pikselach plotna
 * (0,0 = lewy gorny rog). Kilka polecen w jednej ramce. */
enum {
    VS_D_CLEAR  = 'c',   /* col - cale plotno */
    VS_D_RECT   = 'r',   /* col, fx x, fx y, fx w, fx h - wypelniony prostokat */
    VS_D_LINE   = 'l',   /* col, fx grubosc, fx x0, fx y0, fx x1, fx y1 (zaokraglone konce) */
    VS_D_CIRCLE = 'o',   /* col, fx cx, fx cy, fx r, fx grubosc (0 = wypelnione kolo) */
    VS_D_FILL   = 'f',   /* col, u16 n, n bajtow polecen sciezki VS_P_* - wypelnienie */
    VS_D_STROKE = 's',   /* col, fx grubosc, u16 n, n bajtow polecen VS_P_* - obrys */
    VS_D_IMAGE  = 'i',   /* u16 obraz, fx x, fx y, fx w, fx h (0 = naturalny) - wklej obraz */
    VS_D_PIXELS = 'p',   /* u16 x, u16 y, u16 w, u16 h, w*h*2 bajtow RGB565 LE - surowe piksele */
    VS_D_TEXT   = 't',   /* col, fx x, fx linia bazowa, u8 px, u8 n, n bajtow UTF-8 */
    VS_D_PEN    = 'n'    /* u8 wlaczone, col, fx grubosc - rysowanie palcem / mysza na plotnie;
                            kreske rysuje podglad od razu, aplikacja dostaje zwykle
                            VS_EV_POINTER (cel = plotno, lx/ly = piksele plotna) */
};

/* Kamera: VS_CMA_SNAP - jedno zdjecie JPEG; VS_CMA_START - strumien klatek JPEG (MJPEG) do
 * VS_CMA_STOP. Kazda klatka: VS_EV_CAMERA (naglowek z rozmiarem), potem VS_EV_CAMERA_DATA az do
 * rozmiaru. W strumieniu aplikacja potwierdza odebrane klatki (VS_CMA_ACK); podglad ma w drodze
 * najwyzej VS_CAM_WINDOW niepotwierdzonych - gdy siec nie nadaza, klatki sa pomijane w podgladzie
 * (nie rosnie opoznienie ani pamiec). */
#define VS_CMA_SNAP   1
#define VS_CMA_START  2
#define VS_CMA_STOP   3
#define VS_CMA_ACK    4
#define VS_CMS_OK         0
#define VS_CMS_NO_CAMERA  1         /* brak kamery / nie ruszyla */
#define VS_CMS_NO_MEMORY  2
#define VS_CMS_ERROR      3         /* blad przechwytywania albo kodowania */
#define VS_CAM_WINDOW     2

/* pole tekstowe (VS_T_EDIT): flagi */
#define VS_ED_MULTILINE    0x01     /* wiele linii + paski przewijania; bez tego: jedna linia */
#define VS_ED_READONLY     0x02
#define VS_ED_LINE_NUMBERS 0x04     /* numery linii (z VS_ED_MULTILINE) */
#define VS_ED_LIVE         0x08     /* VS_TA_CHANGE po kazdej zmianie (inaczej tylko ENTER/BLUR) */
/* pole tekstowe: akcje w VS_EV_TEXT (zawsze z pelnym tekstem) */
#define VS_TA_CHANGE 1              /* tekst zmieniony (tylko z VS_ED_LIVE) */
#define VS_TA_ENTER  2              /* Enter w polu jednoliniowym */
#define VS_TA_BLUR   3              /* pole stracilo fokus */
#define VS_TA_FOCUS  4              /* pole dostalo fokus (tablet: pokaz klawiature) */

/* polecenia sciezki (typ VS_T_PATH), wspolrzedne lokalne fx */
enum {
    VS_P_MOVE  = 'M',   /* x y */
    VS_P_LINE  = 'L',   /* x y */
    VS_P_QUAD  = 'Q',   /* cx cy x y */
    VS_P_CUBIC = 'C',   /* c1x c1y c2x c2y x y */
    VS_P_ARC   = 'A',   /* przez_x przez_y x y  (luk przez 3 punkty) */
    VS_P_CLOSE = 'Z'
};

/* wyrownanie tekstu: poziomo | pionowo; punkt (0,0) obiektu to kotwica */
#define VS_AL_LEFT     0x00
#define VS_AL_CENTER   0x01
#define VS_AL_RIGHT    0x02
#define VS_AL_TOP      0x00
#define VS_AL_MIDDLE   0x04
#define VS_AL_BASELINE 0x08
#define VS_AL_BOTTOM   0x0C

/* styl przycisku */
#define VS_BTN_PRIMARY 0x01
#define VS_BTN_ICON(n) ((n) << 4)   /* 1 play, 2 pauza, 3 reset, 4 wyczysc, 5 tekst, 6 ok */

/* flagi obiektu (SET_FLAGS) */
#define VS_OF_INTERACTIVE 0x01      /* trafiany przez wskaznik: cel w zdarzeniach */
#define VS_OF_DRAGGABLE   0x02      /* przeciagany LOKALNIE w podgladzie, raport VS_EV_MOVED */

/* ANIMATE: wlasciwosc i wartosci docelowe */
enum {
    VS_AP_POS      = 1,   /* fx x, fx y */
    VS_AP_SCALE    = 2,   /* sc sx, sc sy */
    VS_AP_ROTATION = 3,   /* an kat */
    VS_AP_VALUE    = 4,   /* s32 */
    VS_AP_FILL     = 5    /* col */
};
/* ANIMATE: krzywa (bity 0-3) + flagi */
#define VS_EASE_LINEAR    0
#define VS_EASE_OUT       1
#define VS_EASE_IN_OUT    2
#define VS_EASE_LOOP      0x80      /* po dojsciu do celu zaczyna od poczatku */
#define VS_EASE_ALTERNATE 0x40      /* z LOOP: tam i z powrotem */

/* maska zdarzen (VS_EVENT_MASK); HELLO, RESIZE, CONTROL, MOVED, PONG, CLOSE - zawsze */
#define VS_EM_BUTTONS 0x0001        /* wcisniecie / puszczenie wskaznika */
#define VS_EM_DRAG    0x0002        /* ruch przy wcisnietym przycisku / dotyku */
#define VS_EM_HOVER   0x0004        /* ruch bez wcisniecia (duzo ruchu w sieci) */
#define VS_EM_WHEEL   0x0008
#define VS_EM_KEYS    0x0010
#define VS_EM_CHARS   0x0020
#define VS_EM_DEFAULT (VS_EM_BUTTONS | VS_EM_DRAG | VS_EM_WHEEL | VS_EM_KEYS | VS_EM_CHARS)

/* zdarzenia wskaznika */
#define VS_PA_DOWN   1
#define VS_PA_MOVE   2
#define VS_PA_UP     3
#define VS_PA_CANCEL 4
#define VS_PTR_MOUSE 0              /* numer wskaznika: 0 = mysz, 1..N = kolejne palce */

/* akcje kontrolek */
#define VS_CA_CLICK  1              /* przycisk */
#define VS_CA_TOGGLE 2              /* checkbox, wartosc 0/1 */
#define VS_CA_CHANGE 3              /* suwak w trakcie przeciagania */
#define VS_CA_COMMIT 4              /* suwak puszczony (wartosc koncowa) */

/* modyfikatory i mozliwosci podgladu */
#define VS_MOD_SHIFT 0x01
#define VS_MOD_CTRL  0x02
#define VS_MOD_ALT   0x04
#define VS_CAP_MOUSE    0x01
#define VS_CAP_KEYBOARD 0x02
#define VS_CAP_TOUCH    0x04
#define VS_CAP_VTEXT    0x08        /* tekst wektorowy (skalowanie/obrot tekstu) */
#define VS_CAP_FILES    0x10        /* przesylanie plikow (VS_FILE_*) */
#define VS_CAP_STORAGE  0x20        /* pamiec podgladu: VS_FS_LIST/GET/PUT/OP */
#define VS_CAP_CAPTURE  0x40        /* nagrywanie na pamiec podgladu: VS_CAPTURE */
#define VS_CAP_AUDIO    0x80        /* glosnik / mikrofony przez kanal audio: VS_AUDIO_CONNECT (vsaudio.h) */

/* Rodzaj urzadzenia podgladu (VS_EV_DEVICE). Podglad wysyla DEVICE tuz przed
 * HELLO, w tej samej paczce - aplikacja, ktora dostaje HELLO, zna juz urzadzenie
 * i moze od razu zbudowac pasujace UI. Starsze podglady DEVICE nie wysylaja
 * (aplikacja zostaje przy VS_DC_UNKNOWN), starsze aplikacje ja pomijaja.
 * Opis: pary "klucz=wartosc" rozdzielone ';' (male litery, bez spacji), np.
 *     "platform=esp32p4;board=m5stack-tab5;os=esp-idf"
 * Klucze:
 *     platform - procesor / system: esp32p4, esp32s3, win32, linux, macos, web...
 *     board    - konkretne urzadzenie / plytka: m5stack-tab5, ...
 *     os       - system / SDK: esp-idf, windows, ...
 *     fw       - wersja oprogramowania podgladu
 * Nieznane klucze pomija sie; nowe mozna dopisywac bez zmiany wersji. */
#define VS_DC_UNKNOWN  0
#define VS_DC_DESKTOP  1            /* komputer: mysz + klawiatura, duzy ekran */
#define VS_DC_TABLET   2            /* dotyk, ekran 7"+ */
#define VS_DC_PHONE    3            /* dotyk, maly ekran */
#define VS_DC_PANEL    4            /* wbudowany panel / wyswietlacz urzadzenia */

/* kody klawiszy: znaki drukowalne = ASCII (litery wielkie), reszta ponizej */
enum {
    VSK_BACKSPACE = 0x08, VSK_TAB = 0x09, VSK_ENTER = 0x0D, VSK_ESCAPE = 0x1B, VSK_SPACE = 0x20,
    VSK_LEFT = 0x100, VSK_RIGHT, VSK_UP, VSK_DOWN, VSK_HOME, VSK_END, VSK_PGUP, VSK_PGDN,
    VSK_INSERT, VSK_DELETE,
    VSK_F1 = 0x110   /* F1..F12 = VSK_F1 + 0..11 */
};

/* ---- kodowanie ---- */
typedef struct {
    uint8_t *buf;
    int      cap, len;
    int      start;          /* poczatek biezacej ramki */
    int      overflow;       /* ramka nie zmiescila sie w buforze - odrzucona */
} VsWriter;

void vs_w_init(VsWriter *w, uint8_t *buf, int cap);
int  vs_w_begin(VsWriter *w, int type);                 /* 0 = brak miejsca */
void vs_w_u8(VsWriter *w, unsigned v);
void vs_w_u16(VsWriter *w, unsigned v);
void vs_w_u32(VsWriter *w, uint32_t v);
void vs_w_s32(VsWriter *w, int32_t v);
void vs_w_fx(VsWriter *w, float v);
void vs_w_sc(VsWriter *w, float v);
void vs_w_an(VsWriter *w, float deg);
void vs_w_str(VsWriter *w, const char *s);
void vs_w_bytes(VsWriter *w, const void *data, int len);
int  vs_w_end(VsWriter *w);                             /* 1 = ramka zapisana */

/* CRC-32 (IEEE, jak w ZIP/PNG): crc = vs_crc32(0, dane, n); kolejne kawalki: vs_crc32(crc, ...) */
uint32_t vs_crc32(uint32_t crc, const void *data, int len);

/* ---- dekodowanie ---- */
typedef struct {
    const uint8_t *p;
    int len, pos, err;
} VsReader;

void     vs_r_init(VsReader *r, const uint8_t *p, int len);
unsigned vs_r_u8(VsReader *r);
unsigned vs_r_u16(VsReader *r);
int      vs_r_s16(VsReader *r);
uint32_t vs_r_u32(VsReader *r);
int32_t  vs_r_s32(VsReader *r);
float    vs_r_fx(VsReader *r);
float    vs_r_sc(VsReader *r);
float    vs_r_an(VsReader *r);
int      vs_r_left(const VsReader *r);
/* reszta danych jako napis: kopiuje do dst (zawsze zakonczony zerem) */
void     vs_r_str(VsReader *r, char *dst, int cap);

/* Wydziela kolejna pelna ramke z bufora: zwraca liczbe zuzytych bajtow
 * (0 = ramka jeszcze niekompletna). */
int vs_frame(const uint8_t *buf, int len, int *type, const uint8_t **data, int *data_len);

#ifdef __cplusplus
}
#endif

#endif /* VSPROTO_H */
