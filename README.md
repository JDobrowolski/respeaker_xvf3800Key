# respeaker_xvf3800Key — słowo kluczowe, rozpoznawanie mowy i odpowiedź głosowa

Uwaga Do xvf3800 został wgrany firmware z Home Asistanata ktory ustawia ten DSP jako I2S master wiec ESp32s3 w tym przypadku ma ustawiony I2S na Slave!

Rozwinięcie `respeaker_xvf3800` (ESP32-S3 + ReSpeaker XVF3800 4-Mic Array, firmware XMOS po I2S).
Urządzenie **samo nasłuchuje słowa kluczowego** (np. „punapu") siecią neuronową liczoną na
ESP32-S3 i **nic nie wysyła**, dopóki go nie usłyszy. Host w tym czasie **śpi** (zablokowany
odczyt z gniazda, zero CPU). Po słowie urządzenie piszczy, wysyła polecenie, host rozpoznaje je
Whisperem, wypisuje tekst i **odpowiada głosem** (Piper TTS) w głośniku ReSpeakera.

```
            ESP32-S3 (firmware)                                  PC (TCC64)
  XVF3800 ─I2S─> mic.c ─> historia 1,5 s ─┬─> wake.c: log-mel + sieć (kws) co 60 ms
                                          │     └─ wykryto ──> piknięcie, stan POLECENIE
                                          └─> audio_net.c ── AF_WAKE ──────────> vsasr: budzi się
                                                ── AF_MIC (polecenie, z 300 ms zapasu) ─>   Whisper
                                                ── AF_END (cisza/limit) ─────────>   tekst -> stdout
  głośnik <─I2S─ audio.c (bufor 1 s) <────────── AF_PLAY (odpowiedź) <────────   Piper TTS
```

## Co jest w projekcie

| Katalog | Zawartość |
|---|---|
| `main/` | firmware ESP-IDF 5.4: `mic.c` (historia mikrofonu), `wake.c` (słowo kluczowe, polecenie, VAD, wgrywanie modelu), `audio_net.c` (protokół v2), reszta jak w `respeaker_xvf3800` |
| `components/kws/` | **wspólny kod C** firmware i PC: `kws_feat.c` (cechy log-mel), `nn.c` (sieć: budowa, wnioskowanie, zapis), `kws_detect.c` (plik `.kws`, detektor strumieniowy) |
| `kws/` | `kwstool` (TCC64): nagrywanie próbek, **trening sieci**, test, wgrywanie modelu; `nn_train.c` (wsteczna propagacja, AdamW, wątki); skrypty danych; `models/punapu.kws` |
| `asr/` | wspólne dla hostów: Whisper (`whisper.c`), **Piper TTS** (`tts.c`), program `asr_main.c` |
| `hostCPU/`, `hostGPU/` | `vsasr_cpu.exe` / `vsasr_gpu.exe` (Whisper na CPU / CUDA) — oba ze słowem kluczowym i TTS |
| `tts/` | `pobierz_tts.bat` (espeak-ng, onnxruntime, polskie głosy), `specstrings.h` (podkładka dla TCC) |
| `host/` | `vsmic` (nagrywanie/odtwarzanie WAV) — bez zmian, działa z nowym firmware |

## Szybki start

```
1. modele i składniki (raz):   models\pobierz_model.bat   tts\pobierz_tts.bat
2. hosty:                       hostCPU\build.bat   hostGPU\build.bat   kws\build.bat
3. firmware:                    idf.py build  &&  idf.py -p COMx flash monitor
                                (flash wgrywa też kws\models\punapu.kws do partycji "kws")
4. uruchom:                     hostGPU\vsasr_gpu.exe <ip> -doa
                                -> "spię - czekam na słowo kluczowe"
                                powiedz: "punapu ... która jest godzina?"
                                -> [135 st.] Która jest godzina?     (głośnik: "Jest godzina ...")
```

## Słowo kluczowe — sieć neuronowa i własne API (zamiast TensorFlow Lite Micro)

TFLM wymagałby treningu w Pythonie/TensorFlow, konwersji do `.tflite` i biblioteki C++ na ESP32.
Tu jest **własna mała biblioteka sieci w C** — trening w TCC64 na PC, a **ten sam plik C liczy
cechy i sieć na PC i na ESP32**, więc to, co testujesz na PC (`kwstool test`), jest dokładnie
tym, co działa w urządzeniu.

API (`components/kws/nn.h` — wnioskowanie, `kws/nn_train.h` — trening):

```c
nn_model m;
nn_init(&m, 49, 40, 1);                 // wejście: 49 ramek x 40 pasm mel
nn_conv(&m, 16, 3, 3, 2, 2, 1);         // splot 3x3, 16 kanałów, krok 2, ReLU
nn_dwconv(&m, 3, 3, 2, 2, 1);           // depthwise
nn_conv(&m, 48, 1, 1, 1, 1, 1);         // pointwise
...
nn_gap(&m); nn_dense(&m, 2, 0); nn_alloc(&m);

nn_trainer *t = nn_trainer_new(&m, 0);   // wszystkie rdzenie
nn_he_init(&m, 1234);
float loss = nn_train_step(t, X, y, n, lr, 1e-4f, &ok);   // AdamW, softmax + entropia krzyżowa
const float *logits = nn_forward(&m, &ws, x);              // wnioskowanie (też na ESP32)
```

Model domyślny (`kwstool train -arch medium`): DS-CNN, 22 tys. parametrów, **1,24 mln MAC**
na wnioskowanie, liczony co 60 ms; firmware co 500 wnioskowań loguje zmierzony czas.
Wagi czytane są **wprost z partycji flash** (`esp_partition_mmap`) — zero RAM na parametry.

### Dane do treningu (`kws\dataset\`)

| Katalog | Co | Ile |
|---|---|---|
| `keyword\` | słowo kluczowe, jedno na plik (< 1 s); najlepiej przez sam ReSpeaker: `kwstool record <ip> dataset\keyword` (mówisz co ~1 s, program sam tnie) | ≥ 50, dobrze 150–300; różne osoby, odległości, kierunki, tempo |
| `negative\` | wszystko inne: zwykła mowa (długie pliki OK), **podobne słowa** („puma", „papu", „na pół"…) — `kwstool record ... -prefix inne` | ≥ 30 min mowy + podobne słowa |
| `noise\` (opcja) | tło bez mowy (wiatrak, kuchnia, TV): `vsmic <ip> record dataset\noise\x.wav 120` | kilka minut |

Pomocnicze: `pobierz_negatywy.bat` (polska mowa z LibriVox, domena publiczna),
`tts_synth.ps1` (próbki syntezatorem Windows — tylko na start).

Trening: `kwstool train dataset -o models\moje.kws -keyword punapu` (60 epok ≈ 4 min na 20
wątkach). Augmentacja: tempo ±10 %, poziom, szum syntetyczny / z plików / mowa w tle, a także
„ucięte słowo" jako przykład negatywny. Na końcu program **sam dobiera próg** (zero alarmów na
wydzielonej mowie, ≤ 10 % na podobnych słowach) i drukuje tabelę: skuteczność vs. alarmy.
Nowy model bez flashowania: `kwstool upload <ip> models\moje.kws`; próg zmienisz też w sesji
wektorowej (suwak) albo `AF_THR`.

**Zmierzone** (model startowy `kws/models/punapu.kws`, trenowany na głosach syntezatora +
40 min polskiej mowy; test na **niewidzianym** 5-minutowym rozdziale audiobooka z wstawionymi
10 nowymi „punapu" i 10 podobnymi słowami): **wykryte 10/10, fałszywe na podobnych słowach
0/10, fałszywe na zwykłej mowie 0**. Model startowy zna tylko 2 głosy syntezatora — do
codziennego użycia **nagraj prawdziwe głosy** (`kwstool record`) i przetrenuj.

## Odpowiedź głosowa — Piper TTS w C (TCC64)

`asr/tts.c` robi to samo co Piper, ale w C: `espeak-ng.dll` (tekst → fonemy IPA) i
`onnxruntime.dll` (model VITS) ładowane dynamicznie z `tts\bin`. Polskie głosy: **gosia**
(domyślny), darkman, mc_speech. Synteza na CPU ~35× szybciej niż czas rzeczywisty
(10 s mowy w 0,26 s). Sprawdzone pętlą: mowa z Pipera → Whisper → ten sam tekst.

Odpowiedzi wbudowane: godzina („Jest godzina dwudziesta trzynaście"), data, powitanie,
podziękowanie, a dla innych zdań „Usłyszałem: …". Własna logika: `-reply mojprogram.exe`
(dostaje ścieżkę pliku z rozpoznanym tekstem, odpowiedź wypisuje na stdout — np. skrypt z LLM).

```
vsasr_gpu <ip> -say "Dzień dobry"            powiedz tekst głośnikiem
vsasr_cpu -saywav "Dzień dobry" a.wav        synteza do pliku
vsasr_gpu <ip> -tts tts\voices\pl_PL-darkman-medium.onnx    inny głos
vsasr_gpu <ip> -reply off                    bez odpowiedzi głosowej
vsasr_gpu <ip> -ttsgpu                       TTS przez DirectML (tts\pobierz_tts.bat dml)
```

GPU dla TTS: brak cuDNN wyklucza onnxruntime-CUDA, więc `-ttsgpu` używa **DirectML**
(DirectX 12). Działa, ale dla małego modelu Piper jest **wolniejszy** niż CPU (0,44 s wobec
0,26 s) — domyślnie TTS liczy się na CPU w obu hostach; Whisper w `hostGPU` dalej na CUDA.

## Protokół v2 (`main/audio_proto.h`)

Powitanie: bajt 7 = flagi (`AUDIO_FL_KWS`, `AUDIO_FL_MODEL`). Nowe ramki:

| Ramka | Kierunek | Treść |
|---|---|---|
| `W` AF_WAKE | urządzenie → host | wynik 0..255, kierunek DOA, słowo |
| `M` AF_MIC | urządzenie → host | polecenie (od 300 ms przed końcem słowa) |
| `E` AF_END | urządzenie → host | koniec: cisza / limit / brak mowy / host |
| `L` AF_LISTEN | host → urządzenie | 1 = słuchaj teraz (bez słowa), 0 = przerwij |
| `T` AF_THR | host → urządzenie | próg 0..255 (0 = z modelu), zapis w NVS |
| `X` AF_MODEL | host → urządzenie | wgrywanie `.kws`: begin / dane / end + CRC → `S` „OK …" |

`AF_REC 1` nadal włącza stały strumień (`vsmic record`, `kwstool record`, `vsasr -cont`).

## Firmware — co się zmieniło

- `mic.c`: jeden czytelnik I2S → historia 1,5 s z bezwzględnym licznikiem próbek; detektor i
  sieć czytają z własnych pozycji (zapas „sprzed wykrycia" = cofnięta pozycja).
- `wake.c` (rdzeń 1): detektor, piknięcie, polecenie kończone VAD-em na urządzeniu (cisza
  800 ms / brak mowy 4 s / limit 10 s — `menuconfig → Słowo kluczowe`).
- Partycja `kws` (256 KB, `partitions.csv`); `idf.py flash` wgrywa do niej `kws/models/punapu.kws`.
- **PSRAM** (XIAO ESP32-S3 = ESP32-S3R8, 8 MB octal): historia mikrofonu i bufor głośnika w
  PSRAM, bufory sieci w szybkim RAM. Bez PSRAM firmware też startuje (log pokazuje wolny RAM).
- CPU 240 MHz, optymalizacja `-O2`.
- Sesja wektorowa: stan słowa (nasłuch / wynik / ostatnie zdarzenie), suwak progu,
  przycisk „Mów teraz".

## Piny, sieć, XVF3800

Bez zmian względem `respeaker_xvf3800`: I2S BCLK/WS/DIN/DOUT = 8/7/43/44 (ESP32 = slave,
48 kHz/32-bit/stereo → strumień 16 kHz mono), I2C 5/6 @ 0x2C, konsola przez USB Serial/JTAG,
provisioning Wi-Fi i sesja wektorowa jak w `vsled_esp32c6`. Numery `RESID`/`CMD` XVF3800
(`main/xvf3800.h`) — do sprawdzenia dla Twojego obrazu XMOS (DOA).

## Budowanie

Patrz `budowanie.txt`.
