/*
 * xvf3800.c - sterowanie XVF3800 przez I2C (nowy sterownik i2c_master z ESP-IDF 5.x).
 * Patrz xvf3800.h - numery RESID/CMD sa zalezne od firmware XMOS i moga wymagac korekty.
 */
#include <string.h>
#include "xvf3800.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "xvf3800";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_present;

#define XVF_TIMEOUT_MS 100

esp_err_t xvf_init(void)
{
    i2c_master_bus_config_t bcfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = -1,                                  /* dowolny wolny kontroler */
        .scl_io_num = CONFIG_RSX_I2C_SCL_GPIO,
        .sda_io_num = CONFIG_RSX_I2C_SDA_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t e = i2c_new_master_bus(&bcfg, &s_bus);
    if (e != ESP_OK) { ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(e)); return e; }

    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_RSX_XVF_ADDR,
        .scl_speed_hz = CONFIG_RSX_I2C_FREQ_HZ,
    };
    e = i2c_master_bus_add_device(s_bus, &dcfg, &s_dev);
    if (e != ESP_OK) { ESP_LOGE(TAG, "add_device: %s", esp_err_to_name(e)); return e; }

    s_present = (i2c_master_probe(s_bus, CONFIG_RSX_XVF_ADDR, XVF_TIMEOUT_MS) == ESP_OK);
    ESP_LOGI(TAG, "XVF3800 pod adresem 0x%02X: %s", CONFIG_RSX_XVF_ADDR,
             s_present ? "wykryty" : "BRAK ODPOWIEDZI (sterowanie przez I2C nieaktywne)");
    return ESP_OK;
}

bool xvf_present(void) { return s_present; }

esp_err_t xvf_write(uint8_t resid, uint8_t cmd, const uint8_t *data, uint8_t len)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[3 + 64];
    if (len > sizeof buf - 3) return ESP_ERR_INVALID_ARG;
    buf[0] = resid;
    buf[1] = cmd;
    buf[2] = len;
    if (len && data) memcpy(buf + 3, data, len);
    return i2c_master_transmit(s_dev, buf, 3 + len, XVF_TIMEOUT_MS);
}

esp_err_t xvf_read(uint8_t resid, uint8_t cmd, uint8_t *data, uint8_t len)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint8_t hdr[3] = { resid, (uint8_t)(cmd | 0x80), len };
    uint8_t rx[1 + 64];
    if (len > sizeof rx - 1) return ESP_ERR_INVALID_ARG;
    /* zapis naglowka + powtorzony START + odczyt (len+1) bajtow: status + dane */
    esp_err_t e = i2c_master_transmit_receive(s_dev, hdr, sizeof hdr, rx, 1 + len, XVF_TIMEOUT_MS);
    if (e != ESP_OK) return e;
    if (rx[0] != 0) {
        ESP_LOGD(TAG, "odczyt resid %u cmd %u: status %u", resid, cmd, rx[0]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (len && data) memcpy(data, rx + 1, len);
    return ESP_OK;
}

esp_err_t xvf_set_volume(int pct)
{
    if (!s_present) return ESP_ERR_INVALID_STATE;
    if (pct < 0) pct = 0; else if (pct > 100) pct = 100;
    uint8_t v = (uint8_t)pct;
    return xvf_write(XVF_RESID_AUDIO_MGR, XVF_CMD_SET_VOLUME, &v, 1);
}

esp_err_t xvf_set_mic_gain(int db)
{
    if (!s_present) return ESP_ERR_INVALID_STATE;
    int8_t v = (int8_t)db;
    return xvf_write(XVF_RESID_PP, XVF_CMD_SET_MIC_GAIN, (uint8_t *)&v, 1);
}

esp_err_t xvf_get_doa(int *deg, int *energy)
{
    if (!s_present) return ESP_ERR_INVALID_STATE;
    uint8_t d[3] = { 0 };
    esp_err_t e = xvf_read(XVF_RESID_DOA, XVF_CMD_GET_DOA, d, sizeof d);
    if (e != ESP_OK) return e;
    int a = d[0] | (d[1] << 8);
    if (deg) *deg = ((a % 360) + 360) % 360;
    if (energy) *energy = d[2];
    return ESP_OK;
}
