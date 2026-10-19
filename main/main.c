/*
 * main.c - ReSpeaker XVF3800 na ESP32-S3: strumien audio po Wi-Fi + sesja wektorowa.
 *
 *  1. NVS
 *  2. XVF3800 (I2C): glosnosc, wzmocnienie mikrofonu, kierunek zrodla (DOA)
 *  3. Audio I2S: przetworzony mikrofon z XVF3800 -> PCM s16le; PCM -> glosnik
 *  4. Wi-Fi (net.c): punkt dostepowy vsapp_<nazwa> albo zapamietana siec + rozgloszenia UDP
 *  5. sesja wektorowa :7777 (vs_app.c) - stan Wi-Fi i ustawienia audio
 *  6. strumien audio :7778 (audio_net.c) - mikrofon do hosta, host do glosnika
 */
#include "nvs_flash.h"
#include "esp_log.h"
#include "net.h"
#include "vs_session.h"
#include "xvf3800.h"
#include "audio.h"
#include "audio_net.h"
#include "mic.h"
#include "wake.h"
#include "esp_heap_caps.h"

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);

    xvf_init();
    audio_init();
    audio_set_volume(audio_get_volume());      /* wyslij domyslna glosnosc do XVF3800 */
    mic_start();                               /* ciagly odczyt mikrofonu do historii */
    wake_start();                              /* slowo kluczowe (model z partycji kws) */
    ESP_LOGI("main", "wolna pamiec: %u B (najwiekszy blok %u B)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    net_start();
    vs_session_start();
    audio_net_start();
}
