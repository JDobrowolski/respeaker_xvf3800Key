/*
 * audio_net.h - serwer TCP strumienia audio (protokol audio_proto.h).
 * Jedno polaczenie naraz: wysyla PCM mikrofonu do hosta i przyjmuje PCM do glosnika,
 * a takze proste polecenia (glosnosc, wzmocnienie, start/stop nagrywania).
 */
#ifndef AUDIO_NET_H
#define AUDIO_NET_H

void audio_net_start(void);
int  audio_net_clients(void);     /* 0 lub 1 - do podgladu stanu w sesji wektorowej */

#endif /* AUDIO_NET_H */
