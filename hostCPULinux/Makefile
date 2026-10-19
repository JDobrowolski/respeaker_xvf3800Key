# vsasr_cpu - rozpoznawanie mowy Whisper na CPU, Linux (Ubuntu), cel: Intel Core i3-N305
#   make            AVX2 + FMA (AVX-VNNI wybierane przy starcie, gdy procesor ma)
#   make SIMD=0     czyste C bez SIMD (kazdy x86-64, kilka razy wolniej)
#   make clean
# Synteza mowy dolacza sie sama, gdy jest ../tts/include (./pobierz_tts.sh).

CC     ?= gcc
SIMD   ?= 1
CFLAGS ?= -O3
CFLAGS += -std=gnu11 -Wall -Wno-unused-result -Wno-format-truncation -pthread
LDLIBS  = -lm -lpthread -ldl

ifeq ($(SIMD),1)
CFLAGS += -mavx2 -mfma
endif

SRC = asr_main.c backend_cpu.c ../asr/whisper.c
HDR = ../asr/whisper.h ../asr/backend.h ../asr/tts.h ../host/audio_proto.h

ifneq ($(wildcard ../tts/include/onnxruntime_c_api.h),)
CFLAGS += -DASR_TTS -I../tts/include
SRC    += tts.c
else
$(info [tts] brak ../tts/include - buduje bez syntezy mowy (uruchom ./pobierz_tts.sh))
endif

vsasr_cpu: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f vsasr_cpu

.PHONY: clean
