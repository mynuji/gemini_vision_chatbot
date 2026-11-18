#include "license_manager.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/md.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "LICENSE_MGR";
static const char *NVS_NAMESPACE = "security";
static const char *NVS_KEY = "license";

static char s_mac_str[20] = {0};
static char s_stored_license[68] = {0};
static char s_expected_license_primary[68] = {0};
static char s_expected_license_secondary[68] = {0};
static bool s_initialized = false;
static bool s_is_valid = false;

// SHA-256 서명 해시 계산 (sos_gateway_license와 100% 동일한 알고리즘)
static void compute_sha256_hex(const char *input, char *out_hex64) {
    uint8_t sha_result[32] = {0};
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, (const unsigned char *)input, strlen(input));
    mbedtls_md_finish(&ctx, sha_result);
    mbedtls_md_free(&ctx);

    for (int i = 0; i < 32; i++) {
        sprintf(out_hex64 + (i * 2), "%02x", sha_result[i]);
    }
    out_hex64[64] = '\0';
}

static void generate_license_for_key(const char *mac, const char *secret, char *out_license) {
    char combined[128] = {0};
    snprintf(combined, sizeof(combined), "%s%s", mac, secret);
    compute_sha256_hex(combined, out_license);
}

esp_err_t license_manager_init(void) {
    if (s_initialized) return ESP_OK;

    // 1. 기기 고유 Wi-Fi STA MAC 주소 읽기 (대문자 콜론 형식: "XX:XX:XX:XX:XX:XX")
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read Wi-Fi STA MAC: %s", esp_err_to_name(err));
        return err;
    }

    snprintf(s_mac_str, sizeof(s_mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Device Hardware MAC: %s", s_mac_str);

    // 2. 기대되는 정품 라이선스 키 계산 (Primary & Secondary 마스터 키)
    generate_license_for_key(s_mac_str, LICENSE_MASTER_KEY_PRIMARY, s_expected_license_primary);
    generate_license_for_key(s_mac_str, LICENSE_MASTER_KEY_SECONDARY, s_expected_license_secondary);

    // 3. NVS "security" 네임스페이스에서 저장된 라이선스 로드
    nvs_handle_t nvs_handle;
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        size_t len = sizeof(s_stored_license);
        err = nvs_get_str(nvs_handle, NVS_KEY, s_stored_license, &len);
        nvs_close(nvs_handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Loaded License from NVS: %s", s_stored_license);
        } else {
            ESP_LOGW(TAG, "No license found in NVS 'security' namespace");
            s_stored_license[0] = '\0';
        }
    } else {
        ESP_LOGW(TAG, "Failed to open NVS namespace 'security': %s", esp_err_to_name(err));
        s_stored_license[0] = '\0';
    }

    // 4. 유효성 검증
    s_is_valid = false;
    if (s_stored_license[0] != '\0') {
        if (strcmp(s_stored_license, s_expected_license_primary) == 0 ||
            strcmp(s_stored_license, s_expected_license_secondary) == 0) {
            s_is_valid = true;
            ESP_LOGI(TAG, "✅ Device License VALIDATED! (Authorized Device)");
        } else {
            ESP_LOGE(TAG, "🚨 License MISMATCH! Unauthorized Device. Stored: %s", s_stored_license);
        }
    } else {
        ESP_LOGW(TAG, "⚠️ Device NOT provisioned (License empty)");
    }

    s_initialized = true;
    return ESP_OK;
}

const char* license_manager_get_mac(void) {
    if (!s_initialized) license_manager_init();
    return s_mac_str;
}

const char* license_manager_get_stored_license(void) {
    if (!s_initialized) license_manager_init();
    return s_stored_license;
}

const char* license_manager_get_expected_license(void) {
    if (!s_initialized) license_manager_init();
    return s_expected_license_primary;
}

bool license_manager_validate(void) {
    if (!s_initialized) license_manager_init();
    return s_is_valid;
}

esp_err_t license_manager_set_license(const char *license_key) {
    if (!license_key || strlen(license_key) < 32) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing license: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs_handle, NVS_KEY, license_key);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
        strncpy(s_stored_license, license_key, sizeof(s_stored_license) - 1);
        s_is_valid = (strcmp(s_stored_license, s_expected_license_primary) == 0 ||
                      strcmp(s_stored_license, s_expected_license_secondary) == 0);
        ESP_LOGI(TAG, "License saved to NVS. Valid: %d", s_is_valid);
    }
    nvs_close(nvs_handle);
    return err;
}

esp_err_t license_manager_auto_provision(void) {
    if (!s_initialized) license_manager_init();
    ESP_LOGI(TAG, "Auto-provisioning device with Primary License Key...");
    return license_manager_set_license(s_expected_license_primary);
}
