#include "settings_manager.h"
#include "app_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "SETTINGS_MGR";
static const char *NVS_NAMESPACE = "settings";
static const char *NVS_KEY = "app_cfg";

static app_settings_t s_settings;
static bool s_initialized = false;

static void set_defaults(app_settings_t *cfg) {
    memset(cfg, 0, sizeof(app_settings_t));
    cfg->magic = SETTINGS_MAGIC;
    cfg->volume = (uint8_t)(DEFAULT_VOLUME_SCALE * 100.0f); // 기본 70%
    if (cfg->volume < 10) cfg->volume = 10;
    if (cfg->volume > 100) cfg->volume = 100;

    strncpy(cfg->wifi_ssid, CONFIG_WIFI_SSID, sizeof(cfg->wifi_ssid) - 1);
    strncpy(cfg->wifi_pass, CONFIG_WIFI_PASS, sizeof(cfg->wifi_pass) - 1);
    
    // CONFIG_WS_SERVER_URI (예: "ws://192.168.0.2:8765")에서 IP와 Port 파싱
    const char *default_uri = CONFIG_WS_SERVER_URI;
    const char *p = strstr(default_uri, "://");
    if (p) p += 3;
    else p = default_uri;

    char host_buf[64] = {0};
    uint16_t port = 8765;
    const char *colon = strchr(p, ':');
    if (colon) {
        size_t host_len = colon - p;
        if (host_len >= sizeof(host_buf)) host_len = sizeof(host_buf) - 1;
        strncpy(host_buf, p, host_len);
        port = (uint16_t)atoi(colon + 1);
    } else {
        strncpy(host_buf, p, sizeof(host_buf) - 1);
    }

    strncpy(cfg->server_ip, host_buf, sizeof(cfg->server_ip) - 1);
    cfg->server_port = (port > 0) ? port : 8765;
    cfg->brightness = 100; // 100%
    cfg->mic_gain = 35;    // 3.5x
}

esp_err_t settings_manager_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        set_defaults(&s_settings);
        s_initialized = true;
        return err;
    }

    nvs_handle_t nvs_handle;
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    bool need_save_default = false;

    if (err == ESP_OK) {
        size_t required_size = sizeof(app_settings_t);
        err = nvs_get_blob(nvs_handle, NVS_KEY, &s_settings, &required_size);
        nvs_close(nvs_handle);

        if (err != ESP_OK || s_settings.magic != SETTINGS_MAGIC) {
            ESP_LOGW(TAG, "Valid settings not found in NVS (err=%s, magic=0x%08lX). Setting defaults.",
                     esp_err_to_name(err), (unsigned long)s_settings.magic);
            set_defaults(&s_settings);
            need_save_default = true;
        } else {
            ESP_LOGI(TAG, "Loaded settings from NVS successfully: SSID='%s', Server='%s:%u', Vol=%u%%, Bright=%u%%",
                     s_settings.wifi_ssid, s_settings.server_ip, (unsigned int)s_settings.server_port,
                     s_settings.volume, s_settings.brightness);
        }
    } else {
        ESP_LOGI(TAG, "NVS namespace '%s' not found. Initializing with defaults.", NVS_NAMESPACE);
        set_defaults(&s_settings);
        need_save_default = true;
    }

    s_initialized = true;

    if (need_save_default) {
        settings_manager_save(&s_settings);
    }

    return ESP_OK;
}

const app_settings_t* settings_manager_get(void) {
    if (!s_initialized) {
        settings_manager_init();
    }
    return &s_settings;
}

esp_err_t settings_manager_save(const app_settings_t *new_settings) {
    if (!new_settings) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed for writing: %s", esp_err_to_name(err));
        return err;
    }

    memcpy(&s_settings, new_settings, sizeof(app_settings_t));
    s_settings.magic = SETTINGS_MAGIC;

    err = nvs_set_blob(nvs_handle, NVS_KEY, &s_settings, sizeof(app_settings_t));
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
        ESP_LOGI(TAG, "Saved settings to NVS successfully! (SSID='%s', Server='%s:%u', Vol=%u%%)",
                 s_settings.wifi_ssid, s_settings.server_ip, (unsigned int)s_settings.server_port, s_settings.volume);
    } else {
        ESP_LOGE(TAG, "nvs_set_blob failed: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
    return err;
}

esp_err_t settings_manager_reset_to_default(void) {
    app_settings_t def;
    set_defaults(&def);
    return settings_manager_save(&def);
}

esp_err_t settings_manager_set_volume(uint8_t volume) {
    if (volume < 10) volume = 10;
    if (volume > 100) volume = 100;
    s_settings.volume = volume;
    return settings_manager_save(&s_settings);
}

esp_err_t settings_manager_set_brightness(uint8_t brightness) {
    if (brightness < 10) brightness = 10;
    if (brightness > 100) brightness = 100;
    s_settings.brightness = brightness;
    return settings_manager_save(&s_settings);
}
