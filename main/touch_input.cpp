#include "touch_input.h"
#include "bsp_board.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "TOUCH_INPUT";
static volatile uint32_t s_last_touch_isr_time = 0;
static volatile bool s_touch_event_flag = false;

static void IRAM_ATTR touch_isr_handler(void *arg) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    // 하드웨어 채터링 및 손가락 뗄 때 발생하는 잔여 바운스 방지 (300ms)
    if (now - s_last_touch_isr_time > 300) {
        s_touch_event_flag = true;
        s_last_touch_isr_time = now;
    }
}

esp_err_t touch_input_init(void) {
    ESP_LOGI(TAG, "Initializing Touch Input on GPIO %d...", TOUCH_INT);

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_NEGEDGE; // Falling edge
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << TOUCH_INT);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    gpio_install_isr_service(0);
    ret = gpio_isr_handler_add((gpio_num_t)TOUCH_INT, touch_isr_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Touch input initialized.");
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
    return (gpio_get_level((gpio_num_t)TOUCH_INT) == 0);
}

uint32_t touch_input_get_last_isr_time(void) {
    return s_last_touch_isr_time;
}
