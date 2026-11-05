#include "time_sync.h"
#include "settings_manager.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "TIME_SYNC";

// 유명 NTP 서버 목록 (기본: time.google.com - Anycast 초고속 응답)
static const ntp_server_info_t s_ntp_servers[] = {
    {"time.google.com (Google)","time.google.com"},
    {"kr.pool.ntp.org (Korea)", "kr.pool.ntp.org"},
    {"time.cloudflare.com",     "time.cloudflare.com"},
    {"pool.ntp.org (Global)",   "pool.ntp.org"},
    {"time.windows.com (MS)",   "time.windows.com"},
    {"time.apple.com (Apple)",  "time.apple.com"}
};
static const uint8_t s_ntp_server_count = sizeof(s_ntp_servers) / sizeof(s_ntp_servers[0]);

// 타임존 목록: 사용자 요구사항에 따라 최상단에 서울 배치, 나머지는 GMT 시간 기준 순서대로 나열
static const timezone_info_t s_timezones[] = {
    {"Seoul (GMT+9)",             "KST-9"},
    {"(GMT-11) Samoa / Midway",    "SST11"},
    {"(GMT-10) Honolulu",         "HST10"},
    {"(GMT-9) Anchorage",         "AKST9AKDT,M3.2.0,M11.1.0"},
    {"(GMT-8) Los Angeles",       "PST8PDT,M3.2.0,M11.1.0"},
    {"(GMT-7) Denver",            "MST7MDT,M3.2.0,M11.1.0"},
    {"(GMT-6) Chicago",           "CST6CDT,M3.2.0,M11.1.0"},
    {"(GMT-5) New York",          "EST5EDT,M3.2.0,M11.1.0"},
    {"(GMT-4) Santiago",          "CLT4"},
    {"(GMT-3) Sao Paulo",         "BRT3"},
    {"(GMT-2) Mid-Atlantic",      "FNT2"},
    {"(GMT-1) Azores",            "AZOT1"},
    {"(GMT+0) London / UTC",      "GMT0BST,M3.5.0/1,M10.5.0"},
    {"(GMT+1) Paris / Berlin",    "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"(GMT+2) Cairo",             "EET-2"},
    {"(GMT+3) Moscow / Riyadh",   "MSK-3"},
    {"(GMT+4) Dubai",             "GST-4"},
    {"(GMT+5) Karachi",           "PKT-5"},
    {"(GMT+5:30) New Delhi",      "IST-5:30"},
    {"(GMT+6) Dhaka",             "BST-6"},
    {"(GMT+7) Bangkok / Jakarta", "ICT-7"},
    {"(GMT+8) Beijing / Singapore","CST-8"},
    {"(GMT+9) Tokyo",             "JST-9"},
    {"(GMT+9:30) Adelaide",       "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"(GMT+10) Sydney",           "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"(GMT+11) Noumea",           "SBT-11"},
    {"(GMT+12) Auckland",         "NZST-12NZDT,M9.5.0,M4.1.0/3"}
};
static const uint8_t s_timezone_count = sizeof(s_timezones) / sizeof(s_timezones[0]);

static bool s_time_synced = false;
static time_sync_cb_t s_sync_cb = NULL;

void time_sync_set_callback(time_sync_cb_t cb) {
    s_sync_cb = cb;
}

static void time_sync_notification_cb(struct timeval *tv) {
    s_time_synced = true;
    time_t now = tv ? tv->tv_sec : time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S (%Z)", &timeinfo);
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "  NTP Time Synced Successfully!");
    ESP_LOGI(TAG, "  Current Local Time: %s", buf);
    ESP_LOGI(TAG, "==================================================");

    if (s_sync_cb) {
        s_sync_cb();
    }
}

uint8_t time_sync_get_ntp_server_count(void) {
    return s_ntp_server_count;
}

const ntp_server_info_t* time_sync_get_ntp_server(uint8_t idx) {
    if (idx >= s_ntp_server_count) idx = 0;
    return &s_ntp_servers[idx];
}

const char* time_sync_get_ntp_server_name(uint8_t idx) {
    if (idx >= s_ntp_server_count) idx = 0;
    return s_ntp_servers[idx].name;
}

uint8_t time_sync_get_timezone_count(void) {
    return s_timezone_count;
}

const timezone_info_t* time_sync_get_timezone(uint8_t idx) {
    if (idx >= s_timezone_count) idx = 0;
    return &s_timezones[idx];
}

const char* time_sync_get_timezone_name(uint8_t idx) {
    if (idx >= s_timezone_count) idx = 0;
    return s_timezones[idx].name;
}

esp_err_t time_sync_init(void) {
    const app_settings_t *cfg = settings_manager_get();
    uint8_t tz_idx = cfg ? cfg->timezone_idx : 0;
    if (tz_idx >= s_timezone_count) tz_idx = 0;

    // 타임존 환경 변수 등록
    setenv("TZ", s_timezones[tz_idx].tz_posix, 1);
    tzset();

    ESP_LOGI(TAG, "Initialized Timezone: '%s' (%s)",
             s_timezones[tz_idx].name, s_timezones[tz_idx].tz_posix);
    return ESP_OK;
}

esp_err_t time_sync_start(void) {
    const app_settings_t *cfg = settings_manager_get();
    uint8_t s_idx = cfg ? cfg->ntp_server_idx : 0;
    uint8_t tz_idx = cfg ? cfg->timezone_idx : 0;
    if (s_idx >= s_ntp_server_count) s_idx = 0;
    if (tz_idx >= s_timezone_count) tz_idx = 0;

    // 타임존 갱신
    setenv("TZ", s_timezones[tz_idx].tz_posix, 1);
    tzset();

    ESP_LOGI(TAG, "Starting SNTP sync with server '%s' (%s), TZ='%s'",
             s_ntp_servers[s_idx].server, s_ntp_servers[s_idx].name, s_timezones[tz_idx].name);

    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED); // 첫 패킷 수신 즉시 로컬 클럭 동기화!
    esp_sntp_setservername(0, s_ntp_servers[s_idx].server);
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
    esp_sntp_setservername(1, "time.google.com");
#endif
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 2
    esp_sntp_setservername(2, "time.cloudflare.com");
#endif
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();

    return ESP_OK;
}

bool time_sync_is_synced(void) {
    if (s_time_synced) return true;
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        s_time_synced = true;
        return true;
    }
    return false;
}

void time_sync_apply_settings(uint8_t server_idx, uint8_t tz_idx) {
    if (server_idx >= s_ntp_server_count) server_idx = 0;
    if (tz_idx >= s_timezone_count) tz_idx = 0;

    setenv("TZ", s_timezones[tz_idx].tz_posix, 1);
    tzset();

    ESP_LOGI(TAG, "Applying new settings: Server='%s', Timezone='%s'",
             s_ntp_servers[server_idx].server, s_timezones[tz_idx].name);

    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_setservername(0, s_ntp_servers[server_idx].server);
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
    esp_sntp_setservername(1, "time.google.com");
#endif
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 2
    esp_sntp_setservername(2, "time.cloudflare.com");
#endif
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
}

void time_sync_get_time_str(char *buf, size_t max_len) {
    if (!buf || max_len == 0) return;
    if (!time_sync_is_synced()) {
        snprintf(buf, max_len, "--:--:--");
        return;
    }
    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(buf, max_len, "%H:%M:%S", &timeinfo);
}

void time_sync_get_date_time_str(char *buf, size_t max_len) {
    if (!buf || max_len == 0) return;
    if (!time_sync_is_synced()) {
        snprintf(buf, max_len, "Syncing...");
        return;
    }
    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(buf, max_len, "%Y-%m-%d %H:%M:%S", &timeinfo);
}
