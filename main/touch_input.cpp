#include "touch_input.h"
#include "bsp_board.h"
#include "audio_io.h"

#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "TOUCH_INPUT";

#define I2C_TOUCH_PORT       I2C_NUM_0
#define CHSC6X_I2C_ADDR      0x2e
#define CST816S_I2C_ADDR     0x15
#define FT6236_I2C_ADDR      0x38

typedef enum {
    TOUCH_CHIP_NONE = 0,
    TOUCH_CHIP_CHSC6X,
    TOUCH_CHIP_CST816S,
    TOUCH_CHIP_FT6236
} touch_chip_type_t;

static touch_chip_type_t s_chip_type = TOUCH_CHIP_NONE;
static uint8_t s_detected_addr = 0;
static bool s_i2c_initialized = false;

static volatile uint32_t s_last_touch_isr_time = 0;
static volatile bool s_touch_event_flag = false;

static int16_t s_last_x = 120;
static int16_t s_last_y = 120;
static bool s_is_pressed = false;

// 상단 10회 연속 터치 감지 변수
static int s_top_tap_count = 0;
static uint32_t s_last_top_tap_time = 0;
static touch_settings_trigger_cb_t s_settings_cb = NULL;

static void IRAM_ATTR touch_isr_handler(void *arg) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    // 디바운스 200ms
    if (now - s_last_touch_isr_time > 200) {
        s_touch_event_flag = true;
        s_last_touch_isr_time = now;
    }
}

static esp_err_t i2c_master_init(void) {
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = (gpio_num_t)TOUCH_SDA;
    conf.scl_io_num = (gpio_num_t)TOUCH_SCL;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = 100000; // 100kHz standard mode for better signal compatibility

    esp_err_t ret = i2c_param_config(I2C_TOUCH_PORT, &conf);
    if (ret != ESP_OK) return ret;

    ret = i2c_driver_install(I2C_TOUCH_PORT, conf.mode, 0, 0, 0);
    return ret;
}

static bool probe_i2c_chip(uint8_t addr) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_TOUCH_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return (ret == ESP_OK);
}

static void i2c_scan_bus(void) {
    ESP_LOGI(TAG, "Scanning I2C bus (SDA:%d, SCL:%d)...", TOUCH_SDA, TOUCH_SCL);
    int devices_found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (probe_i2c_chip(addr)) {
            ESP_LOGI(TAG, " -> Found I2C device at address: 0x%02X", addr);
            devices_found++;
        }
    }
    if (devices_found == 0) {
        ESP_LOGW(TAG, "No I2C devices responded on the bus!");
    }
}

esp_err_t touch_input_init(void) {
    ESP_LOGI(TAG, "Initializing Touch Input (SDA:%d, SCL:%d, INT:%d)...",
             TOUCH_SDA, TOUCH_SCL, TOUCH_INT);

    // 1. GPIO INT 핀 인터럽트 설정
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << TOUCH_INT);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add((gpio_num_t)TOUCH_INT, touch_isr_handler, NULL);

    // 2. I2C 마스터 초기화 및 터치 칩 프로브
    esp_err_t ret = i2c_master_init();
    if (ret == ESP_OK) {
        s_i2c_initialized = true;
        vTaskDelay(pdMS_TO_TICKS(50));

        i2c_scan_bus();

        if (probe_i2c_chip(CHSC6X_I2C_ADDR)) {
            s_chip_type = TOUCH_CHIP_CHSC6X;
            s_detected_addr = CHSC6X_I2C_ADDR;
            ESP_LOGI(TAG, "Found CHSC6X Touch Controller at 0x%02X!", s_detected_addr);
        } else if (probe_i2c_chip(CST816S_I2C_ADDR)) {
            s_chip_type = TOUCH_CHIP_CST816S;
            s_detected_addr = CST816S_I2C_ADDR;
            ESP_LOGI(TAG, "Found CST816S Touch Controller at 0x%02X!", s_detected_addr);
        } else if (probe_i2c_chip(FT6236_I2C_ADDR)) {
            s_chip_type = TOUCH_CHIP_FT6236;
            s_detected_addr = FT6236_I2C_ADDR;
            ESP_LOGI(TAG, "Found FT6236 Touch Controller at 0x%02X!", s_detected_addr);
        } else {
            ESP_LOGW(TAG, "No known I2C Touch Controller detected. Using GPIO INT single-touch fallback.");
            s_chip_type = TOUCH_CHIP_NONE;
        }
    } else {
        ESP_LOGE(TAG, "I2C master init failed: %s", esp_err_to_name(ret));
    }

    return ESP_OK;
}

bool touch_input_read_coords(int16_t *out_x, int16_t *out_y, bool *out_pressed) {
    bool pressed = false;
    int16_t x = s_last_x;
    int16_t y = s_last_y;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    // 하드웨어 INT 핀 상태 확인 (Low면 터치 중)
    bool pin_active = (gpio_get_level((gpio_num_t)TOUCH_INT) == 0);

    if (s_chip_type == TOUCH_CHIP_CST816S) {
        uint8_t data[6] = {0};
        uint8_t reg = 0x01;
        esp_err_t ret = i2c_master_write_read_device(I2C_TOUCH_PORT, s_detected_addr,
                                                     &reg, 1, data, 5, pdMS_TO_TICKS(20));
        if (ret == ESP_OK) {
            uint8_t finger_num = data[0];
            if (finger_num > 0) {
                x = ((data[1] & 0x0F) << 8) | data[2];
                y = ((data[3] & 0x0F) << 8) | data[4];
                pressed = true;
            }
        }
    } else if (s_chip_type == TOUCH_CHIP_CHSC6X) {
        uint8_t data[5] = {0};
        esp_err_t ret = i2c_master_read_from_device(I2C_TOUCH_PORT, s_detected_addr,
                                                   data, 5, pdMS_TO_TICKS(20));
        if (ret == ESP_OK) {
            // CHSC6X: data[0] & 0x01 (touch down)
            if ((data[0] & 0x01) || pin_active) {
                x = ((uint16_t)(data[2] & 0x0F) << 8) | data[1];
                y = ((uint16_t)(data[2] & 0xF0) << 4) | data[3];
                pressed = true;
            }
        }
    } else if (s_chip_type == TOUCH_CHIP_FT6236) {
        uint8_t data[5] = {0};
        uint8_t reg = 0x02; // TD_STATUS
        esp_err_t ret = i2c_master_write_read_device(I2C_TOUCH_PORT, s_detected_addr,
                                                     &reg, 1, data, 5, pdMS_TO_TICKS(20));
        if (ret == ESP_OK) {
            uint8_t touches = data[0] & 0x0F;
            if (touches > 0) {
                x = ((data[1] & 0x0F) << 8) | data[2];
                y = ((data[3] & 0x0F) << 8) | data[4];
                pressed = true;
            }
        }
    } else {
        // I2C 칩이 없는 경우: GPIO INT 핀으로 터치 판정
        pressed = pin_active || (now - s_last_touch_isr_time < 250);
        // 기본 중앙 좌표
        x = 120;
        y = 120;
    }

    // 좌표 경계 클램프 (240x240)
    if (x < 0) x = 0;
    if (x > 239) x = 239;
    if (y < 0) y = 0;
    if (y > 239) y = 239;

    // 상단 10회 연속 터치 감지 (터치가 새로 눌렸을 때判定: false -> true)
    if (pressed && !s_is_pressed) {
        // 상단 영역 판정: Y <= 70 이거나, I2C 칩이 없는 경우 전체 터치를 10회 탭으로 인정
        bool is_top_area = (s_chip_type != TOUCH_CHIP_NONE) ? (y <= 70) : true;

        if (is_top_area) {
            if (now - s_last_top_tap_time < 1500) {
                s_top_tap_count++;
            } else {
                s_top_tap_count = 1; // 1.5초 이상 지났으면 리셋 후 1부터 시작
            }
            s_last_top_tap_time = now;

            ESP_LOGI(TAG, "Top area tapped! Count: %d / 10 (at x=%d, y=%d)", s_top_tap_count, x, y);

            // 탭 할 때마다 가벼운 피드백 클릭 톤
            audio_io_play_tone(1200.0f + s_top_tap_count * 80.0f, 30);

            if (s_top_tap_count >= 10) {
                s_top_tap_count = 0;
                ESP_LOGW(TAG, ">>> 10 TOP TAPS DETECTED! TRIGGERING SETTINGS MENU <<<");
                // 성공 진입 비프음 (경쾌한 3화음)
                audio_io_play_tone(1500.0f, 80);
                if (s_settings_cb) {
                    s_settings_cb();
                }
            }
        } else {
            // 다른 영역 누르면 카운트 리셋
            s_top_tap_count = 0;
        }
    }

    s_is_pressed = pressed;
    s_last_x = x;
    s_last_y = y;

    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
    if (out_pressed) *out_pressed = pressed;

    return pressed;
}

static void lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    int16_t x = 0, y = 0;
    bool pressed = false;
    touch_input_read_coords(&x, &y, &pressed);

    data->point.x = x;
    data->point.y = y;
    data->state = pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}

esp_err_t touch_input_register_lvgl_indev(void) {
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_read_cb;
    lv_indev_drv_register(&indev_drv);
    ESP_LOGI(TAG, "LVGL pointer input driver registered successfully!");
    return ESP_OK;
}

bool touch_input_has_event(void) {
    if (s_touch_event_flag) {
        s_touch_event_flag = false;
        return true;
    }
    return false;
}

bool touch_input_is_active(void) {
    return s_is_pressed || (gpio_get_level((gpio_num_t)TOUCH_INT) == 0);
}

void touch_input_set_settings_trigger_cb(touch_settings_trigger_cb_t cb) {
    s_settings_cb = cb;
}

int touch_input_get_top_tap_count(void) {
    return s_top_tap_count;
}
