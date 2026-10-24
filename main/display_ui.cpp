#include "display_ui.h"
#include "bsp_board.h"
#include "touch_input.h"
#include "ui_settings.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
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
static volatile bool s_camera_preview_mode = false;

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

void display_ui_set_brightness(uint8_t percent) {
  if (percent > 100) percent = 100;
  if (percent < 5) percent = 5; // 최소 시인성 유지
  uint32_t duty = ((uint32_t)percent * 255) / 100;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

esp_err_t display_ui_init(void) {
  ESP_LOGI(TAG, "Initializing GC9A01 LCD & LVGL...");

  // 1. 백라이트 PWM(LEDC) 초기화 (5kHz, 8비트 해상도)
  ledc_timer_config_t timer_conf = {};
  timer_conf.speed_mode = LEDC_LOW_SPEED_MODE;
  timer_conf.duty_resolution = LEDC_TIMER_8_BIT;
  timer_conf.timer_num = LEDC_TIMER_0;
  timer_conf.freq_hz = 5000;
  timer_conf.clk_cfg = LEDC_AUTO_CLK;
  ledc_timer_config(&timer_conf);

  ledc_channel_config_t ch_conf = {};
  ch_conf.gpio_num = LCD_BL;
  ch_conf.speed_mode = LEDC_LOW_SPEED_MODE;
  ch_conf.channel = LEDC_CHANNEL_0;
  ch_conf.intr_type = LEDC_INTR_DISABLE;
  ch_conf.timer_sel = LEDC_TIMER_0;
  ch_conf.duty = 255; // 기본 100% 밝기
  ch_conf.hpoint = 0;
  ledc_channel_config(&ch_conf);

  // 2. SPI 버스 초기화 (240x240 전체 프레임 단일 DMA 전송 지원)
  spi_bus_config_t buscfg = {};
  buscfg.mosi_io_num = LCD_MOSI;
  buscfg.sclk_io_num = LCD_CLK;
  buscfg.max_transfer_sz = 240 * 240 * sizeof(uint16_t);
  esp_err_t ret = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "SPI bus initialize failed: %s", esp_err_to_name(ret));
    return ret;
  }

  // 3. LCD 패널 IO 생성 (40MHz 초고속 SPI 클럭: 240x240 전송 23ms 완료)
  esp_lcd_panel_io_handle_t io_handle;
  esp_lcd_panel_io_spi_config_t io_config = {};
  io_config.cs_gpio_num = LCD_CS;
  io_config.dc_gpio_num = LCD_DC;
  io_config.spi_mode = 0;
  io_config.pclk_hz = 40 * 1000 * 1000;
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
  port_cfg.task_max_sleep_ms = 10; // 지연 없는 초고속 실시간 화면 갱신 (기본 500ms -> 10ms 단축)
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

  // 5-1. LVGL 터치 입력 드라이버 등록 및 설정 모듈 초기화
  touch_input_register_lvgl_indev();
  ui_settings_init();

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

  // 상단 히든 설정 메뉴 터치 위치 안내용 빨간 점 (지름 14px 원형)
  lv_obj_t *s_settings_hint_dot = lv_obj_create(s_screen);
  lv_obj_set_size(s_settings_hint_dot, 14, 14);
  lv_obj_set_style_radius(s_settings_hint_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(s_settings_hint_dot, lv_palette_main(LV_PALETTE_RED), 0);
  lv_obj_set_style_bg_opa(s_settings_hint_dot, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_settings_hint_dot, 0, 0);
  lv_obj_set_style_pad_all(s_settings_hint_dot, 0, 0);
  lv_obj_clear_flag(s_settings_hint_dot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(s_settings_hint_dot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(s_settings_hint_dot, LV_ALIGN_TOP_MID, 0, 12);

  // 카메라 실시간 프리뷰용 64바이트 정렬 PSRAM 버퍼 준비 (SPI DMA 직결 최적화)
  s_crop_buf = (uint16_t *)heap_caps_aligned_alloc(64, 240 * 240 * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
  // 표정 전환 요청이 오면 프리뷰 모드를 해제하여 캐릭터 표정이 항상 나타나도록 보장
  s_camera_preview_mode = false;
  s_is_animating = false; // 진행 중이던 눈 깜빡임 즉각 취소 및 강제 표정 전환

  if (lvgl_port_lock(1000)) {
    if (s_center_label)
      lv_obj_add_flag(s_center_label, LV_OBJ_FLAG_HIDDEN);

    // 카메라 프리뷰 이미지가 화면 최상위를 가리지 않도록 강제 숨김 처리
    if (s_cam_img)
      lv_obj_add_flag(s_cam_img, LV_OBJ_FLAG_HIDDEN);

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

    if (lv_scr_act()) {
      lv_obj_invalidate(lv_scr_act());
    }
    lvgl_port_unlock();
  }
}

void display_ui_set_status(const char *status_text) {
  if (status_text == NULL || s_camera_preview_mode)
    return;
  if (lvgl_port_lock(500)) {
    if (s_status_label) {
      lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(s_status_label, status_text);
      lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -26);
    }
    lvgl_port_unlock();
  }
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
  if (s_camera_preview_mode) return;
  s_camera_preview_mode = true;
  ESP_LOGI(TAG, "Entering camera live preview mode (LVGL widget switch)");
  display_ui_pause_auto_blink(true);

  if (lvgl_port_lock(1000)) {
    if (s_left_eye) lv_obj_add_flag(s_left_eye, LV_OBJ_FLAG_HIDDEN);
    if (s_right_eye) lv_obj_add_flag(s_right_eye, LV_OBJ_FLAG_HIDDEN);
    if (s_status_label) lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    if (s_center_label) lv_obj_add_flag(s_center_label, LV_OBJ_FLAG_HIDDEN);
    if (s_cam_img) {
      lv_obj_clear_flag(s_cam_img, LV_OBJ_FLAG_HIDDEN);
      lv_obj_invalidate(s_cam_img);
    }
    // 사용자의 요청에 따라 카메라 영상을 거울처럼 좌우 반전하여 표시
    if (s_panel_handle) {
      esp_lcd_panel_mirror(s_panel_handle, true, false);
    }
    lvgl_port_unlock();
  }
}

void display_ui_stop_camera_preview(void) {
  if (!s_camera_preview_mode) return;
  s_camera_preview_mode = false;
  ESP_LOGI(TAG, "Exiting camera live preview mode (restoring LVGL UI)");

  if (lvgl_port_lock(1000)) {
    if (s_panel_handle) {
      // 캐릭터 눈 UI를 위한 좌우 미러링 복원
      esp_lcd_panel_mirror(s_panel_handle, true, false);
    }
    if (s_cam_img) lv_obj_add_flag(s_cam_img, LV_OBJ_FLAG_HIDDEN);
    if (s_left_eye) lv_obj_clear_flag(s_left_eye, LV_OBJ_FLAG_HIDDEN);
    if (s_right_eye) lv_obj_clear_flag(s_right_eye, LV_OBJ_FLAG_HIDDEN);
    if (s_status_label) lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    if (lv_scr_act()) lv_obj_invalidate(lv_scr_act());
    lvgl_port_unlock();
  }

  display_ui_pause_auto_blink(false);
}

uint16_t *display_ui_get_preview_buffer(void) {
  return s_crop_buf;
}

void display_ui_refresh_camera_preview(void) {
  if (!s_camera_preview_mode) return;
  if (lvgl_port_lock(50)) {
    if (s_cam_img && !lv_obj_has_flag(s_cam_img, LV_OBJ_FLAG_HIDDEN)) {
      lv_obj_invalidate(s_cam_img);
    }
    lvgl_port_unlock();
  }
}

void display_ui_draw_preview_frame(const uint16_t *rgb565_buf) {
  // s_crop_buf는 이미 s_cam_img_dsc에 바인딩되어 있으므로 화면 무효화(invalidate) 트리거
  display_ui_refresh_camera_preview();
}
