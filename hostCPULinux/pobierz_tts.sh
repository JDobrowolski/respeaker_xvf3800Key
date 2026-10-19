#!/bin/sh
# Pobiera skladniki syntezy mowy (Piper, licencja MIT) dla Linux x86-64:
#   ../tts/bin_linux/   libespeak-ng.so.1 + espeak-ng-data (fonemy, z wydania Piper)
#                       libonnxruntime.so (Microsoft, MIT) - uruchamia model glosu
#   ../tts/include/     naglowki C onnxruntime (potrzebne tylko do kompilacji)
#   ../tts/voices/      polskie glosy Piper: gosia (domyslny), darkman, mc_speech
# Wymaga: curl, tar.  Uzycie: ./pobierz_tts.sh
set -e
cd "$(dirname "$0")/../tts"
ORTV=1.20.1
mkdir -p dl bin_linux include voices

if [ ! -f bin_linux/libespeak-ng.so.1 ]; then
    echo "Pobieram Piper (espeak-ng) ..."
    curl -sL --fail -o dl/piper_linux.tar.gz \
        https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_linux_x86_64.tar.gz
    mkdir -p dl/piper_linux
    tar -xzf dl/piper_linux.tar.gz -C dl/piper_linux
    cp -L dl/piper_linux/piper/libespeak-ng.so.1 bin_linux/
    rm -rf bin_linux/espeak-ng-data
    cp -r dl/piper_linux/piper/espeak-ng-data bin_linux/
fi

if [ ! -f bin_linux/libonnxruntime.so ]; then
    echo "Pobieram onnxruntime $ORTV ..."
    curl -sL --fail -o dl/ort_linux.tgz \
        https://github.com/microsoft/onnxruntime/releases/download/v$ORTV/onnxruntime-linux-x64-$ORTV.tgz
    tar -xzf dl/ort_linux.tgz -C dl
    cp -L dl/onnxruntime-linux-x64-$ORTV/lib/libonnxruntime.so.$ORTV bin_linux/libonnxruntime.so
    [ -f include/onnxruntime_c_api.h ] || cp dl/onnxruntime-linux-x64-$ORTV/include/*.h include/
fi

for v in gosia darkman mc_speech; do
    f=voices/pl_PL-$v-medium.onnx
    [ -f "$f" ] && continue
    echo "Pobieram glos pl_PL-$v-medium ..."
    curl -sL --fail -o "$f" https://huggingface.co/rhasspy/piper-voices/resolve/main/pl/pl_PL/$v/medium/pl_PL-$v-medium.onnx
    curl -sL --fail -o "$f.json" https://huggingface.co/rhasspy/piper-voices/resolve/main/pl/pl_PL/$v/medium/pl_PL-$v-medium.onnx.json
done
echo "Gotowe. Biblioteki w $(pwd)/bin_linux, glosy w $(pwd)/voices"
