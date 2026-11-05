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
#include <time.h>
#include <sys/time.h>

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

// 시계 화면 표시용 위젯
static lv_obj_t *s_clock_container = NULL;
static lv_obj_t *s_clock_date_lbl = NULL;
static lv_obj_t *s_clock_time_lbl = NULL;
static lv_obj_t *s_clock_ampm_lbl = NULL;
static lv_timer_t *s_clock_timer = NULL;
static volatile bool s_is_clock_mode = false;
static volatile bool s_clock_temp_hidden = false;

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
  ESP_LOGI(TAG, "Setting LCD backlight brightness: %u%% (duty=%lu/255)", (unsigned int)percent, (unsigned long)duty);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

esp_err_t display_ui_init(void) {
  ESP_LOGI(TAG, "Initializing GC9A01 LCD & LVGL...");

  // 1. 백라이트 PWM(LEDC) 초기화 (1kHz, 8비트 해상도)
  // UART0 콘솔 해제 후 출력 모드를 명시적으로 활성화해야 OutputEn=1로 동작함
  gpio_reset_pin((gpio_num_t)LCD_BL);
  gpio_set_direction((gpio_num_t)LCD_BL, GPIO_MODE_OUTPUT);
  gpio_set_pull_mode((gpio_num_t)LCD_BL, GPIO_FLOATING);

  ledc_timer_config_t timer_conf = {};
  timer_conf.speed_mode = LEDC_LOW_SPEED_MODE;
  timer_conf.duty_resolution = LEDC_TIMER_8_BIT;
  timer_conf.timer_num = LEDC_TIMER_0;
  timer_conf.freq_hz = 1000; // 1kHz로 설정하여 100k 풀업 회로에서도 완벽한 PWM 디밍 지원
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
  lv_obj_add_flag(s_center_label, LV_OBJ_FLAG_HIDDEN);

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

  // 6-2. 대기/부팅 시계 화면 전용 컨테이너 생성 (240x240 원형 화면 전체)
  s_clock_container = lv_obj_create(s_screen);
  lv_obj_set_size(s_clock_container, 240, 240);
  lv_obj_align(s_clock_container, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(s_clock_container, lv_color_black(), 0);
  lv_obj_set_style_border_width(s_clock_container, 0, 0);
  lv_obj_set_style_pad_all(s_clock_container, 0, 0);
  lv_obj_clear_flag(s_clock_container, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_clock_container, LV_OBJ_FLAG_HIDDEN);

  // 상단 일자 및 요일 라벨 (조금 작은 16px 폰트, 상단 안전영역 Y=42)
  s_clock_date_lbl = lv_label_create(s_clock_container);
  lv_obj_set_style_text_font(s_clock_date_lbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(s_clock_date_lbl, lv_color_hex(0x00D1FF), 0);
  lv_label_set_text(s_clock_date_lbl, "2026. 11. 05  THU");
  lv_obj_align(s_clock_date_lbl, LV_ALIGN_TOP_MID, 0, 42);

  // 중앙 시간 영역 라벨 (화면의 약 80% 폭을 차지하는 48px 대형 폰트)
  s_clock_time_lbl = lv_label_create(s_clock_container);
  lv_obj_set_style_text_font(s_clock_time_lbl, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(s_clock_time_lbl, lv_color_white(), 0);
  lv_label_set_text(s_clock_time_lbl, "12:00");
  lv_obj_align(s_clock_time_lbl, LV_ALIGN_CENTER, 14, 10);

  // AM / PM 라벨 (시간 앞에 위치, AM은 윗첨자 형태, PM은 아랫첨자 형태)
  s_clock_ampm_lbl = lv_label_create(s_clock_container);
  lv_obj_set_style_text_font(s_clock_ampm_lbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(s_clock_ampm_lbl, lv_color_hex(0x00D1FF), 0);
  lv_label_set_text(s_clock_ampm_lbl, "AM");
  lv_obj_align_to(s_clock_ampm_lbl, s_clock_time_lbl, LV_ALIGN_OUT_LEFT_TOP, -6, 4);

  // 1초 주기로 시계 텍스트 자동 갱신
  s_clock_timer = lv_timer_create([](lv_timer_t *t){
    display_ui_update_clock();
  }, 1000, NULL);

  lvgl_port_unlock();

  ESP_LOGI(TAG, "Display UI initialization complete.");
  return ESP_OK;
}

void display_ui_show_message(const char *msg) {
  if (msg == NULL)
    return;
  lvgl_port_lock(-1);
  if (s_clock_container)
    lv_obj_add_flag(s_clock_container, LV_OBJ_FLAG_HIDDEN);
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
  // 표정 전환 요청이 오면 프리뷰 해제 및 깜빡임 즉각 취소
  s_camera_preview_mode = false;
  s_is_animating = false;

  // 시계 모드가 활성화되어 있을 때 IDLE 상태로의 전이는 시계를 끄지 않고 유지
  if (s_is_clock_mode) {
    if (left == UI_EMOTION_IDLE && right == UI_EMOTION_IDLE) {
      if (lvgl_port_lock(1000)) {
        if (s_left_eye) {
          lv_img_set_src(s_left_eye, get_image_for_emotion(left));
          s_current_left_emotion = left;
        }
        if (s_right_eye) {
          lv_img_set_src(s_right_eye, get_image_for_emotion(right));
          s_current_right_emotion = right;
        }
        // 간헐적 눈 엿보기 중이 아닐 때는 시계 컨테이너가 화면 최상단에 머물도록 보장
        if (!s_clock_temp_hidden && s_clock_container) {
          lv_obj_clear_flag(s_clock_container, LV_OBJ_FLAG_HIDDEN);
          lv_obj_move_foreground(s_clock_container);
        }
        lvgl_port_unlock();
      }
      return;
    } else {
      // LISTENING, SPEAKING 등 실제 사용자 대화 상호작용 표정이 오면 시계 모드 즉각 해제
      s_is_clock_mode = false;
      s_clock_temp_hidden = false;
    }
  }

  if (lvgl_port_lock(1000)) {
    if (s_clock_container)
      lv_obj_add_flag(s_clock_container, LV_OBJ_FLAG_HIDDEN);

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
  // 시계 모드가 활성화되어 있고 눈 엿보기 상태가 아닐 때는 하단 상태 텍스트로 시계를 가리지 않음
  if (s_is_clock_mode && !s_clock_temp_hidden) {
    return;
  }
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

    if (!s_is_animating && !s_auto_blink_paused && !s_is_clock_mode &&
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

void display_ui_update_clock(void) {
  if (lvgl_port_lock(500)) {
    if (s_clock_container && !lv_obj_has_flag(s_clock_container, LV_OBJ_FLAG_HIDDEN)) {
      time_t now = time(NULL);
      struct tm timeinfo;
      localtime_r(&now, &timeinfo);

      // 1. 상단 날짜 및 요일
      static const char *s_days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
      int wday = timeinfo.tm_wday;
      if (wday < 0 || wday > 6) wday = 0;

      char date_buf[48];
      if (timeinfo.tm_year > 100) { // 2000년 이후 유효 시간 동기화 완료 상태
        snprintf(date_buf, sizeof(date_buf), "%04d. %02d. %02d  %s",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, s_days[wday]);
      } else {
        snprintf(date_buf, sizeof(date_buf), "SYNCING TIME...");
      }
      lv_label_set_text(s_clock_date_lbl, date_buf);
      lv_obj_align(s_clock_date_lbl, LV_ALIGN_TOP_MID, 0, 42);

      // 2. 12시간제 시간 (1~12 : 00~59)
      int hour12 = timeinfo.tm_hour % 12;
      if (hour12 == 0) hour12 = 12;
      bool is_pm = (timeinfo.tm_hour >= 12);

      char time_buf[16];
      snprintf(time_buf, sizeof(time_buf), "%d:%02d", hour12, timeinfo.tm_min);
      lv_label_set_text(s_clock_time_lbl, time_buf);
      lv_obj_update_layout(s_clock_time_lbl);

      // 3. AM(윗첨자) / PM(아랫첨자) 설정
      lv_label_set_text(s_clock_ampm_lbl, is_pm ? "PM" : "AM");
      lv_obj_update_layout(s_clock_ampm_lbl);
      if (is_pm) {
        // PM: 시간 앞 아랫첨자 위치
        lv_obj_align_to(s_clock_ampm_lbl, s_clock_time_lbl, LV_ALIGN_OUT_LEFT_BOTTOM, -6, -4);
      } else {
        // AM: 시간 앞 윗첨자 위치
        lv_obj_align_to(s_clock_ampm_lbl, s_clock_time_lbl, LV_ALIGN_OUT_LEFT_TOP, -6, 4);
      }
    }
    lvgl_port_unlock();
  }
}

void display_ui_show_clock_mode(bool enable) {
  s_is_clock_mode = enable;
  s_clock_temp_hidden = false;

  if (lvgl_port_lock(500)) {
    if (enable) {
      if (s_left_eye) lv_obj_add_flag(s_left_eye, LV_OBJ_FLAG_HIDDEN);
      if (s_right_eye) lv_obj_add_flag(s_right_eye, LV_OBJ_FLAG_HIDDEN);
      if (s_status_label) lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
      if (s_center_label) lv_obj_add_flag(s_center_label, LV_OBJ_FLAG_HIDDEN);
      if (s_cam_img) lv_obj_add_flag(s_cam_img, LV_OBJ_FLAG_HIDDEN);

      if (s_clock_container) {
        lv_obj_clear_flag(s_clock_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_clock_container);
        display_ui_update_clock();
      }
    } else {
      if (s_clock_container) {
        lv_obj_add_flag(s_clock_container, LV_OBJ_FLAG_HIDDEN);
      }
      if (s_left_eye) lv_obj_clear_flag(s_left_eye, LV_OBJ_FLAG_HIDDEN);
      if (s_right_eye) lv_obj_clear_flag(s_right_eye, LV_OBJ_FLAG_HIDDEN);
    }
    if (lv_scr_act()) lv_obj_invalidate(lv_scr_act());
    lvgl_port_unlock();
  }
}

bool display_ui_is_clock_mode(void) {
  return s_is_clock_mode;
}

void display_ui_temp_hide_clock(bool hide) {
  if (!s_is_clock_mode) return;
  s_clock_temp_hidden = hide;
  if (lvgl_port_lock(500)) {
    if (hide) {
      // 대기 중 간헐적으로 눈 표정 표시
      if (s_clock_container) lv_obj_add_flag(s_clock_container, LV_OBJ_FLAG_HIDDEN);
      if (s_left_eye) lv_obj_clear_flag(s_left_eye, LV_OBJ_FLAG_HIDDEN);
      if (s_right_eye) lv_obj_clear_flag(s_right_eye, LV_OBJ_FLAG_HIDDEN);
    } else {
      // 다시 시계 화면으로 복귀
      if (s_left_eye) lv_obj_add_flag(s_left_eye, LV_OBJ_FLAG_HIDDEN);
      if (s_right_eye) lv_obj_add_flag(s_right_eye, LV_OBJ_FLAG_HIDDEN);
      if (s_clock_container) {
        lv_obj_clear_flag(s_clock_container, LV_OBJ_FLAG_HIDDEN);
        display_ui_update_clock();
      }
    }
    if (lv_scr_act()) lv_obj_invalidate(lv_scr_act());
    lvgl_port_unlock();
  }
}

