#include "display_ui.h"
#include "bsp_board.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

static const char *TAG = "DISPLAY_UI";

extern "C" {
LV_IMG_DECLARE(eye_idle_100);
LV_IMG_DECLARE(eye_half_blink_100);
LV_IMG_DECLARE(eye_blink_100);
LV_IMG_DECLARE(eye_listening_100);
LV_IMG_DECLARE(eye_speaking_100);
LV_IMG_DECLARE(eye_sleepy_100);
}

static esp_lcd_panel_handle_t s_panel_handle = NULL;
static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_left_eye = NULL;
static lv_obj_t *s_right_eye = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_center_label = NULL;
static lv_obj_t *s_cam_img = NULL;
static lv_img_dsc_t s_cam_img_dsc = {};
static uint16_t *s_crop_buf = NULL;

static ui_emotion_t s_current_left_emotion = (ui_emotion_t)-1;
static ui_emotion_t s_current_right_emotion = (ui_emotion_t)-1;

static volatile bool s_is_animating = false;
static volatile bool s_auto_blink_paused = false;

static const void *get_image_for_emotion(ui_emotion_t emotion) {
  switch (emotion) {
  case UI_EMOTION_HALF_BLINK:
    return &eye_half_blink_100;
  case UI_EMOTION_BLINK:
    return &eye_blink_100;
  case UI_EMOTION_LISTENING:
    return &eye_listening_100;
  case UI_EMOTION_SPEAKING:
    return &eye_speaking_100;
  case UI_EMOTION_SLEEPY:
    return &eye_sleepy_100;
  case UI_EMOTION_IDLE:
  default:
    return &eye_idle_100;
  }
}

esp_err_t display_ui_init(void) {
  ESP_LOGI(TAG, "Initializing GC9A01 LCD & LVGL...");

  // 1. 백라이트 켜기
  gpio_set_direction((gpio_num_t)LCD_BL, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)LCD_BL, 1);

  // 2. SPI 버스 초기화
  spi_bus_config_t buscfg = {};
  buscfg.mosi_io_num = LCD_MOSI;
  buscfg.sclk_io_num = LCD_CLK;
  buscfg.max_transfer_sz = 240 * 40 * 2;
  esp_err_t ret = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "SPI bus initialize failed: %s", esp_err_to_name(ret));
    return ret;
  }

  // 3. LCD 패널 IO 생성
  esp_lcd_panel_io_handle_t io_handle;
  esp_lcd_panel_io_spi_config_t io_config = {};
  io_config.cs_gpio_num = LCD_CS;
  io_config.dc_gpio_num = LCD_DC;
  io_config.spi_mode = 0;
  io_config.pclk_hz = 20 * 1000 * 1000;
  io_config.trans_queue_depth = 10;
  io_config.lcd_cmd_bits = 8;
  io_config.lcd_param_bits = 8;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                           &io_config, &io_handle));

  // 4. GC9A01 드라이버 연결
  esp_lcd_panel_dev_config_t panel_config = {};
  panel_config.reset_gpio_num = LCD_RST;
  panel_config.rgb_endian = LCD_RGB_ENDIAN_BGR;
  panel_config.bits_per_pixel = 16;
  ESP_ERROR_CHECK(
      esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &s_panel_handle));

  esp_lcd_panel_reset(s_panel_handle);
  esp_lcd_panel_init(s_panel_handle);
  esp_lcd_panel_invert_color(s_panel_handle, true);
  esp_lcd_panel_disp_on_off(s_panel_handle, true);
  // 5. LVGL 포트 초기화
  lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
  lvgl_port_init(&port_cfg);

  lvgl_port_display_cfg_t disp_cfg = {};
  disp_cfg.io_handle = io_handle;
  disp_cfg.panel_handle = s_panel_handle;
  disp_cfg.buffer_size = 240 * 40;
  disp_cfg.double_buffer = true;
  disp_cfg.hres = 240;
  disp_cfg.vres = 240;
  disp_cfg.flags.buff_dma = true;
  disp_cfg.rotation.swap_xy = false;
  disp_cfg.rotation.mirror_x = true;  // GC9A01 좌우 거울 반전 보정 (정방향 출력)
  disp_cfg.rotation.mirror_y = false; // 상하 정방향
  lvgl_port_add_disp(&disp_cfg);

  // LVGL 디스플레이 등록 후 패널 미러링을 명시적으로 적용
  esp_lcd_panel_mirror(s_panel_handle, true, false);

  // 6. 기본 화면 구성
  lvgl_port_lock(-1);
  s_screen = lv_scr_act();
  lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);

  // 중앙 메시지 라벨 (초기 부팅 점검용)
  s_center_label = lv_label_create(s_screen);
  lv_obj_set_style_text_color(s_center_label, lv_color_white(), 0);
  lv_label_set_text(s_center_label, "SYSTEM STARTING...");
  lv_obj_align(s_center_label, LV_ALIGN_CENTER, 0, 0);

  // 양쪽 눈 이미지 생성 (초기에는 숨김 상태)
  s_left_eye = lv_img_create(s_screen);
  lv_obj_align(s_left_eye, LV_ALIGN_CENTER, -55, 0);
  lv_obj_add_flag(s_left_eye, LV_OBJ_FLAG_HIDDEN);

  s_right_eye = lv_img_create(s_screen);
  lv_obj_align(s_right_eye, LV_ALIGN_CENTER, 55, 0);
  lv_obj_add_flag(s_right_eye, LV_OBJ_FLAG_HIDDEN);

  // 하단 상태 라벨
  s_status_label = lv_label_create(s_screen);
  lv_obj_set_style_text_color(s_status_label, lv_color_white(), 0);
  lv_label_set_text(s_status_label, "");
  lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -5);
  lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);

  // 카메라 실시간 프리뷰용 이미지 오브젝트 및 PSRAM 버퍼 준비
  s_crop_buf = (uint16_t *)heap_caps_malloc(240 * 240 * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (s_crop_buf) {
    memset(s_crop_buf, 0, 240 * 240 * sizeof(uint16_t));
    s_cam_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_cam_img_dsc.header.always_zero = 0;
    s_cam_img_dsc.header.reserved = 0;
    s_cam_img_dsc.header.w = 240;
    s_cam_img_dsc.header.h = 240;
    s_cam_img_dsc.data_size = 240 * 240 * sizeof(lv_color_t);
    s_cam_img_dsc.data = (const uint8_t *)s_crop_buf;

    s_cam_img = lv_img_create(s_screen);
    lv_obj_align(s_cam_img, LV_ALIGN_CENTER, 0, 0);
    lv_img_set_src(s_cam_img, &s_cam_img_dsc);
    lv_obj_add_flag(s_cam_img, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "Camera preview LVGL image object ready");
  } else {
    ESP_LOGE(TAG, "Failed to allocate 115KB PSRAM for camera preview buffer");
  }

  lvgl_port_unlock();

  ESP_LOGI(TAG, "Display UI initialization complete.");
  return ESP_OK;
}

void display_ui_show_message(const char *msg) {
  if (msg == NULL)
    return;
  lvgl_port_lock(-1);
  if (s_left_eye)
    lv_obj_add_flag(s_left_eye, LV_OBJ_FLAG_HIDDEN);
  if (s_right_eye)
    lv_obj_add_flag(s_right_eye, LV_OBJ_FLAG_HIDDEN);
  if (s_status_label)
    lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);

  if (s_center_label) {
    lv_obj_clear_flag(s_center_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_center_label, msg);
    lv_obj_align(s_center_label, LV_ALIGN_CENTER, 0, 0);
  }
  lvgl_port_unlock();
}

void display_ui_set_emotion(ui_emotion_t left, ui_emotion_t right,
                            const char *status_text) {
  s_is_animating = false; // 진행 중이던 눈 깜빡임 즉각 취소 및 강제 표정 전환

  lvgl_port_lock(-1);
  if (s_center_label)
    lv_obj_add_flag(s_center_label, LV_OBJ_FLAG_HIDDEN);

  if (s_left_eye) {
    lv_obj_clear_flag(s_left_eye, LV_OBJ_FLAG_HIDDEN);
    lv_img_set_src(s_left_eye, get_image_for_emotion(left));
    s_current_left_emotion = left;
  }

  if (s_right_eye) {
    lv_obj_clear_flag(s_right_eye, LV_OBJ_FLAG_HIDDEN);
    lv_img_set_src(s_right_eye, get_image_for_emotion(right));
    s_current_right_emotion = right;
  }

  if (s_status_label && status_text != NULL) {
    lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_status_label, status_text);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -26);
  }
  lvgl_port_unlock();
}

void display_ui_set_status(const char *status_text) {
  if (status_text == NULL)
    return;
  lvgl_port_lock(-1);
  if (s_status_label) {
    lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_status_label, status_text);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -26);
  }
  lvgl_port_unlock();
}

void display_ui_pause_auto_blink(bool pause) { s_auto_blink_paused = pause; }

static void auto_blink_task(void *pvParameters) {
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(1000 + (esp_random() % 2000)));

    if (!s_is_animating && !s_auto_blink_paused &&
        s_current_left_emotion == UI_EMOTION_IDLE &&
        s_current_right_emotion == UI_EMOTION_IDLE) {

      s_is_animating = true;

      // 15% 확률로 윙크
      bool do_wink = (esp_random() % 100) < 15;
      bool wink_left = (esp_random() % 2) == 0;

      // 1단계: 반쯤 감기
      lvgl_port_lock(-1);
      lv_img_set_src(s_left_eye,
                     do_wink ? (wink_left ? &eye_blink_100 : &eye_idle_100)
                             : &eye_half_blink_100);
      lv_img_set_src(s_right_eye,
                     do_wink ? (wink_left ? &eye_idle_100 : &eye_blink_100)
                             : &eye_half_blink_100);
      lvgl_port_unlock();
      vTaskDelay(pdMS_TO_TICKS(40));

      if (s_auto_blink_paused || s_current_left_emotion != UI_EMOTION_IDLE) {
        s_is_animating = false;
        continue;
      }

      // 2단계: 완전히 감기
      lvgl_port_lock(-1);
      lv_img_set_src(s_left_eye,
                     do_wink ? (wink_left ? &eye_blink_100 : &eye_idle_100)
                             : &eye_blink_100);
      lv_img_set_src(s_right_eye,
                     do_wink ? (wink_left ? &eye_idle_100 : &eye_blink_100)
                             : &eye_blink_100);
      lvgl_port_unlock();
      vTaskDelay(pdMS_TO_TICKS(140));

      if (s_auto_blink_paused || s_current_left_emotion != UI_EMOTION_IDLE) {
        s_is_animating = false;
        continue;
      }

      // 3단계: 반쯤 뜨기
      lvgl_port_lock(-1);
      lv_img_set_src(s_left_eye,
                     do_wink ? (wink_left ? &eye_blink_100 : &eye_idle_100)
                             : &eye_half_blink_100);
      lv_img_set_src(s_right_eye,
                     do_wink ? (wink_left ? &eye_idle_100 : &eye_blink_100)
                             : &eye_half_blink_100);
      lvgl_port_unlock();
      vTaskDelay(pdMS_TO_TICKS(40));

      if (s_auto_blink_paused || s_current_left_emotion != UI_EMOTION_IDLE) {
        s_is_animating = false;
        continue;
      }

      // 4단계: 원래대로 복귀 (단, IDLE 상태일 때만 눈을 IDLE로 복귀)
      if (!s_auto_blink_paused && s_current_left_emotion == UI_EMOTION_IDLE) {
        lvgl_port_lock(-1);
        lv_img_set_src(s_left_eye, &eye_idle_100);
        lv_img_set_src(s_right_eye, &eye_idle_100);
        lvgl_port_unlock();

        s_current_left_emotion = UI_EMOTION_IDLE;
        s_current_right_emotion = UI_EMOTION_IDLE;
      }
      s_is_animating = false;
    }
  }
}

void display_ui_start_auto_blink(void) {
  xTaskCreatePinnedToCoreWithCaps(auto_blink_task, "auto_blink", 3072, NULL, 2, NULL,
                                  0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

esp_lcd_panel_handle_t display_ui_get_panel_handle(void) {
  return s_panel_handle;
}

void display_ui_start_camera_preview(void) {
  ESP_LOGI(TAG, "Entering camera live preview mode (LVGL widget switch)");
  display_ui_pause_auto_blink(true);

  lvgl_port_lock(-1);
  if (s_left_eye) lv_obj_add_flag(s_left_eye, LV_OBJ_FLAG_HIDDEN);
  if (s_right_eye) lv_obj_add_flag(s_right_eye, LV_OBJ_FLAG_HIDDEN);
  if (s_status_label) lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
  if (s_center_label) lv_obj_add_flag(s_center_label, LV_OBJ_FLAG_HIDDEN);
  if (s_cam_img) {
    lv_obj_clear_flag(s_cam_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_cam_img);
  }
  lvgl_port_unlock();

  // 전방 카메라 정방향(사물/글자) 출력을 위해 좌우 미러링 일시 해제
  if (s_panel_handle) {
    esp_lcd_panel_mirror(s_panel_handle, false, false);
  }
}

void display_ui_stop_camera_preview(void) {
  ESP_LOGI(TAG, "Exiting camera live preview mode (LVGL widget restore)");
  if (s_panel_handle) {
    // 캐릭터 눈 UI를 위한 좌우 미러링 복원
    esp_lcd_panel_mirror(s_panel_handle, true, false);
  }

  lvgl_port_lock(-1);
  if (s_cam_img) lv_obj_add_flag(s_cam_img, LV_OBJ_FLAG_HIDDEN);
  if (s_left_eye) lv_obj_clear_flag(s_left_eye, LV_OBJ_FLAG_HIDDEN);
  if (s_right_eye) lv_obj_clear_flag(s_right_eye, LV_OBJ_FLAG_HIDDEN);
  if (s_status_label) lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
  if (lv_scr_act()) lv_obj_invalidate(lv_scr_act());
  lvgl_port_unlock();

  display_ui_pause_auto_blink(false);
}

uint16_t *display_ui_get_preview_buffer(void) {
  return s_crop_buf;
}

void display_ui_refresh_camera_preview(void) {
  lvgl_port_lock(-1);
  if (s_cam_img) {
    lv_obj_invalidate(s_cam_img);
  }
  lvgl_port_unlock();
}

void display_ui_draw_preview_frame(const uint16_t *rgb565_buf) {
  // s_crop_buf는 이미 s_cam_img_dsc에 바인딩되어 있으므로 화면 무효화 트리거
  display_ui_refresh_camera_preview();
}
