#include "touch_input.h"
#include "ui_settings.h"
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

static touch_chip_type_t s_chip_type = TOUCH_CHIP_CHSC6X;
static uint8_t s_detected_addr = CHSC6X_I2C_ADDR;
static bool s_i2c_initialized = false;

static volatile uint32_t s_last_touch_isr_time = 0;
static volatile bool s_chat_touch_event_flag = false;

static int16_t s_last_x = 120;
static int16_t s_last_y = 120;
static bool s_is_pressed = false;

// 상단 10회 연속 터치 감지 변수
static int s_top_tap_count = 0;
static uint32_t s_last_top_tap_time = 0;
static touch_settings_trigger_cb_t s_settings_cb = NULL;

static void IRAM_ATTR touch_isr_handler(void *arg) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now - s_last_touch_isr_time > 100) {
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
            if (addr == CHSC6X_I2C_ADDR && s_chip_type == TOUCH_CHIP_NONE) {
                s_chip_type = TOUCH_CHIP_CHSC6X;
                s_detected_addr = addr;
                ESP_LOGI(TAG, "Identified CHSC6X Touch Controller at 0x%02X!", addr);
            } else if (addr == CST816S_I2C_ADDR && s_chip_type == TOUCH_CHIP_NONE) {
                s_chip_type = TOUCH_CHIP_CST816S;
                s_detected_addr = addr;
                ESP_LOGI(TAG, "Identified CST816S Touch Controller at 0x%02X!", addr);
            } else if (addr == FT6236_I2C_ADDR && s_chip_type == TOUCH_CHIP_NONE) {
                s_chip_type = TOUCH_CHIP_FT6236;
                s_detected_addr = addr;
                ESP_LOGI(TAG, "Identified FT6236 Touch Controller at 0x%02X!", addr);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
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

        if (s_chip_type != TOUCH_CHIP_NONE) {
            ESP_LOGI(TAG, "Touch Controller configured successfully! Type=%d, Addr=0x%02X",
                     (int)s_chip_type, s_detected_addr);
        } else {
            ESP_LOGW(TAG, "No known I2C Touch Controller identified. Using GPIO INT single-touch fallback.");
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
            // Seeed 공식 CHSC6X 프로토콜: data[0] == 0x01 (Touch Down / Active), 0x00 (Released)
            if (data[0] == 0x01 || data[0] == 0x02 || data[0] == 0x03) {
                x = (int16_t)data[2]; // 정방향 (오른쪽으로 밀면 증가)
                y = (int16_t)data[4];
                pressed = true;
            } else {
                pressed = false; // 손을 떼었을 때 확실하게 릴리즈 처리 (직후 터치 좌표 점프 방지)
            }
        } else {
            pressed = pin_active;
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

    // 상단 10회 연속 터치 감지 (터치가 새로 눌렸을 때, 설정 화면이 닫혀있을 때만 동작)
    if (pressed && !s_is_pressed && !ui_settings_is_active()) {
        bool is_rapid_tapping = (now - s_last_top_tap_time < 3000) && (s_top_tap_count > 0);
        bool is_top_area = (y <= 100) || is_rapid_tapping;

        if (is_top_area) {
            if (now - s_last_top_tap_time < 3000) {
                s_top_tap_count++;
            } else {
                s_top_tap_count = 1;
            }
            s_last_top_tap_time = now;

            ESP_LOGI(TAG, "Top area tapped! Count: %d / 10 (at x=%d, y=%d)", s_top_tap_count, x, y);
            audio_io_play_tone(2400.0f, 15);

            if (s_top_tap_count >= 10) {
                s_top_tap_count = 0;
                ESP_LOGW(TAG, ">>> 10 TOP TAPS DETECTED! TRIGGERING SETTINGS MENU <<<");
                audio_io_play_tone(1500.0f, 100);
                if (s_settings_cb) {
                    s_settings_cb();
                }
            }
        } else {
            s_top_tap_count = 0;
            s_chat_touch_event_flag = true;
            ESP_LOGI(TAG, "Chat touch area tapped (x=%d, y=%d) -> Chat event flagged", x, y);
        }
    }

    static int16_t s_anchor_x = 0;
    static int16_t s_anchor_y = 0;
    static bool s_vertical_scroll_active = false;
    static bool s_horizontal_drag_active = false;

    if (pressed && !s_is_pressed) {
        // 새 터치 시작: 앵커 좌표 기록 및 스크롤/드래그 플래그 초기화
        s_anchor_x = x;
        s_anchor_y = y;
        s_vertical_scroll_active = false;
        s_horizontal_drag_active = false;
        // 터치 시작 시에는 최초 탭 좌표로 고정
        x = s_anchor_x;
        y = s_anchor_y;
    } else if (pressed && s_is_pressed) {
        int32_t dx = (int32_t)x - s_anchor_x;
        int32_t dy = (int32_t)y - s_anchor_y;
        int32_t abs_dx = (dx < 0) ? -dx : dx;
        int32_t abs_dy = (dy < 0) ? -dy : dy;

        if (s_vertical_scroll_active) {
            // 수직 스크롤 진행 중: x좌표는 앵커로 고정하여 좌우 흔들림 방지
            x = s_anchor_x;
        } else if (s_horizontal_drag_active) {
            // 슬라이더 좌우 드래그 진행 중: y좌표는 앵커로 고정하여 상하 흔들림 방지
            y = s_anchor_y;
        } else {
            // 방향 감지 단계
            if (abs_dy >= 20 && abs_dy > (abs_dx * 3 / 2)) {
                // 수직 스크롤 확정
                s_vertical_scroll_active = true;
                x = s_anchor_x;
            } else if (abs_dx >= 12 && abs_dx > (abs_dy * 3 / 2)) {
                // 슬라이더 좌우 드래그 확정
                s_horizontal_drag_active = true;
                y = s_anchor_y;
            } else {
                // 탭/클릭 데드존: 미세 흔들림은 100% 시작 앵커로 고정하여 버튼 오인식 원천 차단!
                x = s_anchor_x;
                y = s_anchor_y;
            }
        }
    } else if (!pressed) {
        s_vertical_scroll_active = false;
        s_horizontal_drag_active = false;
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
    indev_drv.scroll_limit = 16;
    indev_drv.scroll_throw = 0;        // 터치 해제 시 즉시 스크롤 정지하여 깔끔하게 정착
    indev_drv.long_press_time = 1500;   // 1500ms로 대폭 확장하여 일반적인 터치가 롱프레스로 오인식되어 클릭 취소되는 현상 원천 차단!
    lv_indev_drv_register(&indev_drv);
    ESP_LOGI(TAG, "LVGL pointer input driver registered (horizontal drag & vertical scroll enabled)!");
    return ESP_OK;
}

bool touch_input_has_event(void) {
    if (s_chat_touch_event_flag) {
        s_chat_touch_event_flag = false;
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
