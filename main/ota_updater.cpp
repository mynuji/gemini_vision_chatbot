#include "ota_updater.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "OTA_UPDATER";

static volatile bool s_ota_running = false;
static volatile bool s_ota_cancel_req = false;
static ota_progress_cb_t s_progress_cb = NULL;
static char s_target_download_url[256] = {0};

const char* ota_updater_get_current_version(void) {
    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc != NULL && desc->version[0] != '\0') {
        return desc->version;
    }
    return "1.0.0";
}

bool ota_updater_is_running(void) {
    return s_ota_running;
}

void ota_updater_cancel(void) {
    if (s_ota_running) {
        s_ota_cancel_req = true;
    }
}

typedef struct {
    char *buffer;
    size_t size;
    size_t max_size;
} http_buffer_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    http_buffer_t *resp = (http_buffer_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && resp != NULL) {
        if (resp->size + evt->data_len < resp->max_size) {
            memcpy(resp->buffer + resp->size, evt->data, evt->data_len);
            resp->size += evt->data_len;
            resp->buffer[resp->size] = '\0';
        }
    }
    return ESP_OK;
}

static void trim_whitespace(char *str) {
    if (!str) return;
    char *end;
    // 앞쪽 공백 제거
    char *start = str;
    while (isspace((unsigned char)*start)) start++;
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
    // 뒤쪽 공백 제거
    end = str + strlen(str) - 1;
    while (end >= str && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
}

esp_err_t ota_updater_check_latest_version(const char *project_name, ota_release_info_t *out_info) {
    if (!out_info) return ESP_ERR_INVALID_ARG;
    memset(out_info, 0, sizeof(ota_release_info_t));

    const char *project = (project_name && project_name[0] != '\0') ? project_name : OTA_DEFAULT_PROJECT_NAME;

    // Public GitHub Raw URL: https://raw.githubusercontent.com/mynuji/esp32_firmware_releases/main/<project>/version.txt
    char version_url[256];
    snprintf(version_url, sizeof(version_url),
             "https://raw.githubusercontent.com/%s/%s/%s/%s/version.txt",
             OTA_DEFAULT_REPO_OWNER, OTA_DEFAULT_REPO_NAME, OTA_DEFAULT_BRANCH, project);

    // 바이너리 다운로드 URL 사전 구성
    snprintf(out_info->download_url, sizeof(out_info->download_url),
             "https://raw.githubusercontent.com/%s/%s/%s/%s/%s.bin",
             OTA_DEFAULT_REPO_OWNER, OTA_DEFAULT_REPO_NAME, OTA_DEFAULT_BRANCH, project, project);

    const char *curr_ver = ota_updater_get_current_version();
    strncpy(out_info->current_version, curr_ver, sizeof(out_info->current_version) - 1);

    ESP_LOGI(TAG, "Checking version from: %s", version_url);

    char resp_buf[128] = {0};
    http_buffer_t resp = {
        .buffer = resp_buf,
        .size = 0,
        .max_size = sizeof(resp_buf) - 1
    };

    esp_http_client_config_t config = {};
    config.url = version_url;
    config.event_handler = http_event_handler;
    config.user_data = &resp;
    config.timeout_ms = 8000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.max_redirection_count = 5;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        snprintf(out_info->error_msg, sizeof(out_info->error_msg), "HTTP INIT FAILED");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "User-Agent", "ESP32-S3-OTA");

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status_code != 200) {
        ESP_LOGE(TAG, "Version fetch failed: err=%s, status=%d", esp_err_to_name(err), status_code);
        if (status_code == 404) {
            snprintf(out_info->error_msg, sizeof(out_info->error_msg), "version.txt Not Found (404)");
        } else {
            snprintf(out_info->error_msg, sizeof(out_info->error_msg), "HTTP Error %d", status_code);
        }
        return ESP_FAIL;
    }

    trim_whitespace(resp_buf);
    if (resp_buf[0] == '\0') {
        snprintf(out_info->error_msg, sizeof(out_info->error_msg), "Empty version.txt");
        return ESP_ERR_INVALID_RESPONSE;
    }

    strncpy(out_info->latest_version, resp_buf, sizeof(out_info->latest_version) - 1);
    out_info->has_update = (strcmp(out_info->current_version, out_info->latest_version) != 0);

    ESP_LOGI(TAG, "Version Check Success: Current='%s', Latest='%s', HasUpdate=%d",
             out_info->current_version, out_info->latest_version, out_info->has_update);

    return ESP_OK;
}

static void ota_download_task(void *pvParameter) {
    s_ota_running = true;
    s_ota_cancel_req = false;

    if (s_progress_cb) {
        s_progress_cb(OTA_STATE_CONNECTING, 0, "CONNECTING TO GITHUB...");
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA partition found!");
        if (s_progress_cb) s_progress_cb(OTA_STATE_FAILED, 0, "NO OTA PARTITION");
        s_ota_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Target partition: subtype=%d, offset=0x%lx, size=0x%lx",
             update_partition->subtype, update_partition->address, update_partition->size);
    ESP_LOGI(TAG, "Downloading raw binary from: %s", s_target_download_url);

    esp_http_client_config_t config = {};
    config.url = s_target_download_url;
    config.timeout_ms = 20000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 4096;
    config.buffer_size_tx = 1024;
    config.max_redirection_count = 5; // Public 저장소이므로 자동 리다이렉트 완벽 지원!

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        if (s_progress_cb) s_progress_cb(OTA_STATE_FAILED, 0, "CLIENT INIT FAILED");
        s_ota_running = false;
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_set_header(client, "User-Agent", "ESP32-S3-OTA");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        if (s_progress_cb) s_progress_cb(OTA_STATE_FAILED, 0, "CONNECTION FAILED");
        s_ota_running = false;
        vTaskDelete(NULL);
        return;
    }

    int total_size = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Download started: status=%d, content_length=%d", status_code, total_size);

    if (status_code != 200 && status_code != 206) {
        ESP_LOGE(TAG, "Download rejected with status: %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        char err_str[32];
        snprintf(err_str, sizeof(err_str), "HTTP ERROR %d", status_code);
        if (s_progress_cb) s_progress_cb(OTA_STATE_FAILED, 0, err_str);
        s_ota_running = false;
        vTaskDelete(NULL);
        return;
    }

    if (s_progress_cb) {
        s_progress_cb(OTA_STATE_DOWNLOADING, 0, "DOWNLOADING FIRMWARE...");
    }

    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(update_partition, (total_size > 0) ? total_size : OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (s_progress_cb) s_progress_cb(OTA_STATE_FAILED, 0, "OTA BEGIN FAILED");
        s_ota_running = false;
        vTaskDelete(NULL);
        return;
    }

    const size_t chunk_size = 4096;
    char *chunk = (char *)malloc(chunk_size);
    if (!chunk) {
        ESP_LOGE(TAG, "Out of memory for chunk buffer");
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (s_progress_cb) s_progress_cb(OTA_STATE_FAILED, 0, "OUT OF MEMORY");
        s_ota_running = false;
        vTaskDelete(NULL);
        return;
    }

    int downloaded_bytes = 0;
    int last_reported_pct = -1;

    while (1) {
        if (s_ota_cancel_req) {
            ESP_LOGW(TAG, "OTA canceled by user");
            esp_ota_abort(ota_handle);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            free(chunk);
            if (s_progress_cb) s_progress_cb(OTA_STATE_FAILED, 0, "OTA CANCELED");
            s_ota_running = false;
            vTaskDelete(NULL);
            return;
        }

        int read_len = esp_http_client_read(client, chunk, chunk_size);
        if (read_len < 0) {
            ESP_LOGE(TAG, "esp_http_client_read error: %d", read_len);
            esp_ota_abort(ota_handle);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            free(chunk);
            if (s_progress_cb) s_progress_cb(OTA_STATE_FAILED, 0, "READ ERROR");
            s_ota_running = false;
            vTaskDelete(NULL);
            return;
        }

        if (read_len == 0) {
            // 다운로드 완료
            break;
        }

        err = esp_ota_write(ota_handle, chunk, read_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write error: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            free(chunk);
            if (s_progress_cb) s_progress_cb(OTA_STATE_FAILED, 0, "FLASH WRITE FAILED");
            s_ota_running = false;
            vTaskDelete(NULL);
            return;
        }

        downloaded_bytes += read_len;

        if (total_size > 0) {
            int pct = (downloaded_bytes * 100) / total_size;
            if (pct != last_reported_pct) {
                last_reported_pct = pct;
                char msg[48];
                snprintf(msg, sizeof(msg), "%d%% (%d / %d KB)", pct, downloaded_bytes / 1024, total_size / 1024);
                if (s_progress_cb) s_progress_cb(OTA_STATE_DOWNLOADING, pct, msg);
            }
        }
    }

    free(chunk);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Downloaded %d bytes. Finalizing OTA...", downloaded_bytes);
    if (s_progress_cb) {
        s_progress_cb(OTA_STATE_FINISHING, 100, "VERIFYING & FINALIZING...");
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        if (s_progress_cb) s_progress_cb(OTA_STATE_FAILED, 0, "VERIFY FAILED");
        s_ota_running = false;
        vTaskDelete(NULL);
        return;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        if (s_progress_cb) s_progress_cb(OTA_STATE_FAILED, 0, "BOOT SET FAILED");
        s_ota_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "OTA SUCCESS! Restarting in 2 seconds...");
    if (s_progress_cb) {
        s_progress_cb(OTA_STATE_SUCCESS, 100, "SUCCESS! REBOOTING...");
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

esp_err_t ota_updater_start(const char *download_url, ota_progress_cb_t cb) {
    if (s_ota_running) return ESP_ERR_INVALID_STATE;

    if (download_url && download_url[0] != '\0') {
        strncpy(s_target_download_url, download_url, sizeof(s_target_download_url) - 1);
    } else {
        snprintf(s_target_download_url, sizeof(s_target_download_url),
                 "https://raw.githubusercontent.com/%s/%s/%s/%s/%s.bin",
                 OTA_DEFAULT_REPO_OWNER, OTA_DEFAULT_REPO_NAME, OTA_DEFAULT_BRANCH,
                 OTA_DEFAULT_PROJECT_NAME, OTA_DEFAULT_PROJECT_NAME);
    }

    s_progress_cb = cb;

    BaseType_t res = xTaskCreatePinnedToCore(ota_download_task, "ota_task", 8192, NULL, 5, NULL, 0);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        return ESP_FAIL;
    }

    return ESP_OK;
}
