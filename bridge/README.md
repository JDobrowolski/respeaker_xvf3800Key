# bridge — ReSpeaker jako zwykły mikrofon i głośnik w Windows (`vsbridge`)

`vsbridge.exe` łączy się z ReSpeakerem obecnym protokołem (`main/audio_proto.h`, TCP 7778)
i przez **wirtualne sterowniki audio** wystawia go Windowsowi jako mikrofon i głośnik.
Dowolny program (Teams, Discord, OBS, przeglądarka, Audacity…) nagrywa z ReSpeakera
i gra na nim, nic nie wiedząc o sieci. Audio przez [RtAudio 6.0.1](https://github.com/thestk/rtaudio)
(WASAPI, 2 pliki, pobierane przy budowaniu).

```
 ReSpeaker (ESP32-S3)              vsbridge.exe                      Windows
 mikrofon ──AF_MIC 16k mono──> bufor ~120 ms ──RtAudio──> "CABLE Input"  ──> programy nagrywają
                                                                            z "CABLE Output"
 głośnik  <──AF_PLAY 16k mono── bramka ciszy <──WASAPI loopback── "Virtual Audio Driver"
                                                                    <── programy grają tutaj
                    -recmic plik.wav      -recspk plik.wav   (opcjonalny zapis obu kierunków)
```

Format strumienia to **16 kHz, mono, 16 bit w obie strony** (tak działa firmware — wyjście do
głośnika też jest mono). Stereo z Windows jest uśredniane do mono, a mono z mikrofonu kopiowane
na oba kanały wyjścia. Zmianę częstotliwości (np. 48 kHz ↔ 16 kHz) robi WASAPI/RtAudio.

## Sterowniki — który do czego

| Kierunek | Sterownik | Uwagi |
|---|---|---|
| PC → głośnik ReSpeakera | [Virtual-Audio-Driver](https://github.com/VirtualDrivers/Virtual-Audio-Driver) — wirtualny **głośnik** | `vsbridge` czyta go przez **WASAPI loopback** (domyślnie `-loop "Virtual Audio Driver"`). Działa ze sterownikiem bez zmian. |
| mikrofon ReSpeakera → PC | kabel wirtualny z pętlą zwrotną, np. **[VB-CABLE](https://vb-audio.com/Cable/)** (darmowy) | `vsbridge` gra na „CABLE Input”, programy nagrywają z „CABLE Output” (domyślnie `-out "CABLE Input"`). |

**Dlaczego nie wirtualny mikrofon z Virtual-Audio-Driver?** W obecnym kodzie tego sterownika
(sprawdzone na commicie `bb34fba`, marzec 2026) mikrofon „Virtual Mic Driver” zawsze daje
**ciszę** — `CMiniportWaveRTStream::WriteBytes` w `Source/Main/minwavertstream.cpp` zeruje bufor
przechwytywania, a sterownik nie ma żadnego interfejsu (IOCTL, pamięci współdzielonej, pętli
z głośnika), którym program mógłby do niego wpisać dźwięk. Dlatego na kierunek mikrofonu
potrzebny jest kabel z pętlą (VB-CABLE: co gra na „CABLE Input”, to słychać na „CABLE Output”).
Gdy ten sterownik dostanie pętlę głośnik→mikrofon, wystarczy podać jego głośnik w `-out`, a jego
mikrofon wybrać w programach — `vsbridge` nie wymaga zmian.

`vsbridge` nie jest przywiązany do konkretnego sterownika: `-out`, `-loop`, `-in` przyjmują
fragment nazwy dowolnego urządzenia z `vsbridge -list` (np. inny kabel wirtualny, Voicemeeter,
albo zwykłe słuchawki — do testu).

### Instalacja Virtual-Audio-Driver (głośnik)

1. Pobierz gotową paczkę z Releases projektu (jeśli jest — w chwili pisania sekcja była pusta)
   albo zbuduj sam: Visual Studio 2022 + WDK, `build.bat` w repozytorium sterownika
   (wynik: `VirtualAudioDriver.inf`, `.sys`, `.cat`).
2. Sterownik jest podpisany testowo — w konsoli administratora: `bcdedit /set testsigning on`
   i restart (przy włączonym Secure Boot trzeba go najpierw wyłączyć w BIOS/UEFI).
3. Menedżer urządzeń → **Akcja → Dodaj starszy sprzęt** → „Zainstaluj sprzęt wybrany ręcznie z
   listy” → **Kontrolery dźwięku, wideo i gier** → **Z dysku…** → `VirtualAudioDriver.inf`.
4. W Ustawieniach dźwięku pojawi się głośnik „Virtual Audio Driver” (i nieużywany tu mikrofon
   „Virtual Mic Driver”). Dokładną nazwę sprawdzisz przez `vsbridge -list`.

### Instalacja VB-CABLE (mikrofon)

Rozpakuj paczkę z vb-audio.com, uruchom `VBCABLE_Setup_x64.exe` jako administrator, restart.
Pojawią się „CABLE Input” (wyjście) i „CABLE Output” (wejście). Nie wymaga trybu testowego.

### Ustawienia Windows

- **Ustawienia → Dźwięk**: domyślne **wyjście = Virtual Audio Driver** (cały dźwięk systemu idzie
  na ReSpeakera), domyślne **wejście = CABLE Output** (ReSpeaker jest mikrofonem).
  Zamiast zmieniać domyślne: wybierz te urządzenia tylko w konkretnym programie albo w
  **Ustawienia → Dźwięk → Mikser głośności** (urządzenie per aplikacja).
- Nie ustawiaj „CABLE Input” jako domyślnego wyjścia — dźwięk systemu trafiłby do mikrofonu.
- Właściwości „CABLE Output”: wyłącz „Słuchaj tego urządzenia”, żeby nie było sprzężenia.

## Użycie

```
vsbridge -list                                   lista urządzeń (nazwy do -out / -loop / -in)
vsbridge 192.168.1.50                            oba kierunki, domyślne urządzenia
vsbridge 192.168.1.50 -v -doa                    + statystyki co 5 s i kierunek dźwięku
vsbridge 192.168.1.50 -out none                  tylko głośnik (PC -> ReSpeaker)
vsbridge 192.168.1.50 -loop none                 tylko mikrofon (ReSpeaker -> PC)
vsbridge 192.168.1.50 -in "Mikrofon (USB"        głośnik ReSpeakera z innego wejścia (np. interkom)
vsbridge 192.168.1.50 -recmic mic.wav -recspk spk.wav   most + zapis obu kierunków
vsbridge 192.168.1.50 -out none -loop none -recmic mic.wav   sam rejestrator mikrofonu
```

| Opcja | Znaczenie |
|---|---|
| `-out <nazwa>` | wyjście, na które idzie mikrofon ReSpeakera (domyślnie `CABLE Input`; `none` = wyłącz) |
| `-loop <nazwa>` | loopback wyjścia → głośnik ReSpeakera (domyślnie `Virtual Audio Driver`; `none` = wyłącz) |
| `-in <nazwa>` | zamiast `-loop`: zwykłe wejście → głośnik ReSpeakera |
| `-recmic <plik.wav>` | **zapis tego, co przychodzi z mikrofonu** (dokładnie strumień z urządzenia) |
| `-recspk <plik.wav>` | **zapis tego, co idzie do głośnika** (strumień 16 kHz mono przed wysłaniem, z ciszą — oś czasu zgodna z rzeczywistością) |
| `-latency <ms>` | bufor mikrofonu (domyślnie 120; mniej = mniejsze opóźnienie, więcej przerw przy słabym Wi-Fi) |
| `-gate <n>` | próg ciszy dla głośnika (domyślnie 4; `0` = wysyłaj zawsze) |
| `-volume <0..100>`, `-gain <dB>` | głośność głośnika / wzmocnienie mikrofonu ustawiane po połączeniu |
| `-doa`, `-v` | kierunek dźwięku z XVF3800 / statystyki bufora co 5 s |

Nazwy porównywane są jako fragment, bez wielkości liter. Pliki WAV: 16 kHz, mono, PCM 16-bit;
nagłówek odświeżany co sekundę, więc plik jest poprawny nawet po zamknięciu okna.

**Konsola w trakcie pracy** (wpisz i Enter):

| Polecenie | Działanie |
|---|---|
| `m` | nagrywanie mikrofonu wł./wył. (bez nazwy: `mic_RRRRMMDD_GGMMSS.wav`) |
| `m plik.wav` | zacznij nagrywać mikrofon do `plik.wav` (bieżący plik zostaje zamknięty) |
| `s` / `s plik.wav` | to samo dla głośnika (`spk_….wav`) |
| `v 40` / `g 6` | głośność / wzmocnienie mikrofonu |
| `q` (albo Ctrl-C) | koniec — pliki są domykane, strumień w urządzeniu wyłączany |

Przy zerwaniu połączenia (Wi-Fi, restart ESP32) `vsbridge` łączy się ponownie co 2 s;
urządzenia audio i nagrywanie działają dalej (czas bez połączenia jest w pliku pominięty, a nie
wypełniony ciszą).

## Jak to działa (szczegóły)

- **Mikrofon**: `vsbridge` wysyła `AF_REC 2` (`AF_REC_ONLY`) i dostaje stały strumień `AF_MIC`.
  Bufor kołowy trzyma ~`-latency` ms; przy niedoborze wyjście gra ciszę i znów zbiera bufor,
  przy nadmiarze (zegar ESP32 szybszy niż karta PC) odrzuca zaległość — opóźnienie się nie kumuluje.
- **Głośnik**: loopback WASAPI daje dokładnie to, co programy grają na wirtualny głośnik.
  Paczki po 20 ms idą jako `AF_PLAY`. Gdy przez >300 ms jest cisza, nic nie jest wysyłane —
  bufor urządzenia (1 s, start po 200 ms) się opróżnia, a różnica zegarów PC/ESP32 kasuje się
  przy każdej przerwie. Opóźnienie głośnika ≈ 250–350 ms. Przy wielogodzinnej muzyce bez przerw
  opóźnienie może dojść do ~1 s albo pojawić się krótka przerwa na ponowne buforowanie.
- **Słowo kluczowe** dalej działa (urządzenie piszczy, `vsbridge` wypisuje `[slowo kluczowe]`),
  ale polecenie nie jest wysyłane jako osobny strumień — jest już w strumieniu mikrofonu.

### Zmiana w firmware (`AF_REC_ONLY`)

Dotąd przy `AF_REC 1` wykryte słowo kluczowe powodowało wysyłanie *drugiego* strumienia
`AF_MIC` (polecenie z historii), przeplatanego ze stałym strumieniem — w mostku dałoby to
zdublowane, poszatkowane audio. Firmware z tego commita:

- ogłasza w powitaniu flagę `AUDIO_FL_RECONLY` (0x04),
- przyjmuje `AF_REC 2` = stały strumień **bez** dokładania polecenia (nadal `AF_WAKE`/`AF_END`).

Stare narzędzia (`vsmic`, `kwstool`, `vsasr -cont`) wysyłają `AF_REC 1` — działają jak dotąd.
Ze starym firmware `vsbridge` też działa, ale po słowie kluczowym od razu przerywa polecenie
(`AF_LISTEN 0`), więc może się wmieszać ułamek sekundy powtórzonego dźwięku. Zalecane:
`idf.py build flash`.

### Ograniczenia

- Urządzenie obsługuje **jedno połączenie naraz** — gdy działa `vsbridge`, nie uruchomisz
  równolegle `vsasr` / `vsmic` / `kwstool` (i odwrotnie: „zle powitanie” / brak połączenia).
- Tylko mono 16 kHz (tyle daje firmware); muzyka przez ReSpeakera brzmi „telefonicznie”.
- `vsbridge` używa trybu współdzielonego WASAPI — jeśli inny program zajmie urządzenie
  wirtualne w trybie wyłącznym (exclusive), `vsbridge` go nie otworzy.

## Budowanie

Wymagany Windows 10/11 x64 i jeden z kompilatorów:

```
cd bridge
build.bat          MSVC (Visual Studio 2019/2022 z „Programowanie aplikacji klasycznych w C++”;
                   cl szukany sam przez vswhere) -> vsbridge.exe
build.bat gcc      MinGW-w64 g++ (np. MSYS2: pacman -S mingw-w64-x86_64-gcc) -> vsbridge.exe
```

`build.bat` za pierwszym razem pobiera `RtAudio.h` i `RtAudio.cpp` (6.0.1) do `bridge\rtaudio\`
(curl wbudowany w Windows). Exe jest linkowany statycznie (`/MT`, `-static`) — wystarczy go
skopiować, bez dodatkowych DLL.

Alternatywnie CMake (pobiera RtAudio przez FetchContent):

```
cmake -S bridge -B bridge\build -A x64
cmake --build bridge\build --config Release        -> bridge\build\Release\vsbridge.exe
```

Na Linuksie ten sam CMake buduje wersję z ALSA (gdy są nagłówki `libasound2-dev`) albo bez
audio — przydaje się do testu protokołu i nagrywania:
`vsbridge <ip> -out none -loop none -recmic test.wav`.

Firmware z obsługą `AF_REC_ONLY`: jak w `budowanie.txt` (`idf.py build`, `idf.py -p COMx flash`).
