# vsmic - klient strumienia audio. Linux/macOS: make.  Windows (mingw): make.
CC     ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

ifeq ($(OS),Windows_NT)
  LDLIBS = -lws2_32
  EXE    = .exe
else
  LDLIBS =
  EXE    =
endif

all: vsmic$(EXE)

vsmic$(EXE): vsmic.c audio_proto.h
	$(CC) $(CFLAGS) -o $@ vsmic.c $(LDLIBS)

clean:
	rm -f vsmic vsmic.exe
