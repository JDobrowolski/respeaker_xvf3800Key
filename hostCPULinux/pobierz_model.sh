#!/bin/sh
# Pobiera modele Whisper (OpenAI, licencja MIT) w formacie ggml z repozytorium whisper.cpp
# do ../models/.
#   ./pobierz_model.sh                 - large-v3-turbo-q8_0 (domyslny, 834 MB)
#   ./pobierz_model.sh base            - wybrany: tiny 75 MB | base 142 MB | small 466 MB
#                                        | medium 1.5 GB | large-v3-turbo-q8_0 834 MB
#   ./pobierz_model.sh test            - probka jfk.wav do sprawdzenia (angielski)
set -e
cd "$(dirname "$0")/../models"
m=${1:-large-v3-turbo-q8_0}
if [ "$m" = test ]; then
    curl -L --fail -o jfk.wav https://raw.githubusercontent.com/ggml-org/whisper.cpp/master/samples/jfk.wav
    exit 0
fi
if [ -f "ggml-$m.bin" ]; then echo "ggml-$m.bin juz jest"; exit 0; fi
echo "Pobieram ggml-$m.bin ..."
curl -L --fail -o "ggml-$m.bin.part" "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-$m.bin"
mv "ggml-$m.bin.part" "ggml-$m.bin"
