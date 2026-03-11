#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include "lvgl_v8_port.h"
#include <ESP_Knob.h>
#include <Button.h>
#include <images/connected.h>
#include <images/disconnected.h>
#include <images/powerup.h>
#include <images/nosd.h>

#include "player_rpc.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

#define GPIO_NUM_KNOB_PIN_A 6
#define GPIO_NUM_KNOB_PIN_B 7
#define BUTTON_PIN 9

ESP_Knob *knob;


typedef enum {
  UI_NONE,
  UI_LOADING,
  UI_PLAYER,
  UI_BT,
  UI_NO_SD
} ui_screen_t;

static ui_screen_t current_screen = UI_NONE;

static lv_obj_t *screen_loading;
static lv_obj_t *screen_no_sd;
static lv_obj_t *screen_player;
static lv_obj_t *screen_bt;
/* Loading animation */
static lv_obj_t *spinner;

static lv_obj_t *no_sd_image;

/* Player */
static lv_obj_t *img_album;
static lv_obj_t *bt_status;
static lv_obj_t *label_status;

/* BT */
static lv_obj_t *bt_list;
static lv_obj_t *bt_spinner;
static lv_obj_t *removeAll;
static int bt_selected_item = -1;
static TimerHandle_t encTimer = 0;
typedef enum {
  NO_gesture = 0,
  L_gesture,
  R_gesture,
  TAP_gesture,
  TAPx2_gesture,
  LONG_gesture
} Gestures_e;
QueueHandle_t gesturesQueue;

static Gestures_e encLastEvent = NO_gesture;
static void sendEvent(Gestures_e g) {
  xQueueSendToBack(gesturesQueue, &g, 0);
}
void sendTimerEvent(TimerHandle_t t) {
  sendEvent(encLastEvent);
}
void onKnobLeftEventCallback(int count, void *usr_data) {
  encLastEvent = L_gesture;
  xTimerStart(encTimer, 0);
  xTimerReset(encTimer, 0);
}

void onKnobRightEventCallback(int count, void *usr_data) {
  encLastEvent = R_gesture;
  xTimerStart(encTimer, 0);
  xTimerReset(encTimer, 0);
}

static void SingleClickCb(void *button_handle, void *usr_data) {
  sendEvent(TAP_gesture);
}
static void DoubleClickCb(void *button_handle, void *usr_data) {
  sendEvent(TAPx2_gesture);
}
static void LongPressStartCb(void *button_handle, void *usr_data) {
  sendEvent(LONG_gesture);
}

lv_obj_t *create_new_screen(void) {
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
  return scr;
}

static void load_screen(lv_obj_t *scr) {

  lv_scr_load_anim(scr,
                   LV_SCR_LOAD_ANIM_FADE_ON,
                   300,
                   0,
                   false);

  lv_obj_set_style_bg_color(lv_scr_act(),
                            lv_color_hex(0x000000),
                            LV_PART_MAIN);

  lv_obj_set_style_bg_opa(lv_scr_act(),
                          LV_OPA_COVER,
                          LV_PART_MAIN);
}


static void create_loading_screen() {
  screen_loading = create_new_screen();
  spinner = lv_img_create(screen_loading);
  lv_obj_set_size(spinner, UI_w, UI_h);
  lv_obj_center(spinner);

  lv_obj_t *label = lv_label_create(screen_loading);
  lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);

  static lv_style_t style;
  lv_style_init(&style);
  lv_style_set_text_font(&style, &lv_font_montserrat_28);  // Set desired size
  lv_obj_add_style(label, &style, 0);

  lv_label_set_text(label, "   https://t.me/sketched_ninja_rabbit          ");
  lv_obj_set_size(label, UI_w * 3 / 4, 32);
  lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -50);
}

void ui_show_loading() {
  current_screen = UI_LOADING;
  load_screen(screen_loading);
  lv_img_set_src(spinner, &powerup);
}


static void create_nosd_screen() {
  screen_no_sd = create_new_screen();
  no_sd_image = lv_img_create(screen_no_sd);
  lv_obj_set_size(no_sd_image, UI_w, UI_h);
  lv_obj_center(no_sd_image);
}

void ui_show_no_sd() {
  current_screen = UI_NO_SD;
  load_screen(screen_no_sd);
  lv_img_set_src(no_sd_image, &nosd);
}


static void btn_to_bt_cb(lv_event_t *e) {
  load_screen(screen_bt);
}

static void create_player_screen() {
  screen_player = create_new_screen();

  img_album = lv_img_create(screen_player);
  lv_obj_set_size(img_album, UI_w, UI_h);
  lv_obj_center(img_album);

  bt_status = lv_img_create(screen_player);
  lv_obj_set_size(bt_status, 32, 32);
  lv_obj_align(bt_status, LV_ALIGN_TOP_MID, 0, 10);


  //label_status = lv_label_create(screen_player);
  //lv_label_set_text(label_status, "Stopped Stopped Stopped Stopped");
  //lv_label_set_long_mode(label_status, LV_LABEL_LONG_SCROLL_CIRCULAR);
  //lv_obj_set_style_text_color(label_status, lv_color_white(), LV_PART_MAIN);
  //lv_obj_set_size(label_status, UI_w * 3 / 4, 20);
  //lv_obj_align(label_status, LV_ALIGN_TOP_MID, 0, 50);
}

void ui_show_player() {
  current_screen = UI_PLAYER;
  load_screen(screen_player);
}

/* ============================================================
   Bluetooth Screen
   ============================================================ */

static void create_bt_screen(void) {
  screen_bt = create_new_screen();

  bt_list = lv_list_create(screen_bt);
  lv_obj_align(bt_list, LV_ALIGN_TOP_MID, 0, 30);
  lv_obj_set_size(bt_list, UI_w, UI_h - 60);
  lv_obj_center(bt_list);

  removeAll = lv_btn_create(bt_list);
  lv_obj_add_flag(removeAll, LV_OBJ_FLAG_CHECKABLE);
  lv_obj_set_width(removeAll, lv_pct(100));
  lv_obj_t *label = lv_label_create(removeAll);
  lv_label_set_text(label, "Remove paired devices");
  lv_obj_center(label);

  bt_spinner = lv_spinner_create(screen_bt, 1000, 60);
  lv_obj_set_size(bt_spinner, 80, 80);
  lv_obj_center(bt_spinner);
  lv_obj_add_flag(bt_spinner, LV_OBJ_FLAG_HIDDEN);
}

void ui_add_bt_item(ScanItem_t item) {
  lv_obj_t *btn = lv_list_add_btn(bt_list, NULL, item.name);
  ScanItem_t *data = (ScanItem_t *)lv_mem_alloc(sizeof(ScanItem_t));
  if (data) {
    memcpy(data, &item, sizeof(ScanItem_t));
    btn->user_data = data;
  }
  lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
  //lv_obj_add_event_cb(btn,
  //                    bt_item_event_cb,
  //                    LV_EVENT_CLICKED,
  //                     data);

  //lv_obj_set_style_text_color(btn,
  //                            lv_color_hex(0xFFFFFF),
  //                            LV_PART_MAIN);

  // lv_obj_set_style_bg_color(btn,
  //                           lv_color_hex(0x000000),
  //                          LV_PART_MAIN);
  ui_bt_scan_stop_anim();
}

void ui_bt_scan_start_anim(void) {
  lv_obj_clear_flag(bt_spinner, LV_OBJ_FLAG_HIDDEN);
}

void ui_bt_scan_stop_anim(void) {
  lv_obj_add_flag(bt_spinner, LV_OBJ_FLAG_HIDDEN);
}

void ui_update_bt_list_selection() {
  int child = lv_obj_get_child_cnt(bt_list);
  for (int i = 0; i < child; i++) {
    lv_obj_t *child = lv_obj_get_child(bt_list, i);
    if (i == bt_selected_item) {
      lv_obj_add_state(child, LV_STATE_CHECKED);
    } else {
      lv_obj_clear_state(child, LV_STATE_CHECKED);
    }
  }
}

void ui_bt_list_clear(void) {
  uint32_t count = lv_obj_get_child_cnt(bt_list);
  for (uint32_t i = 0; i < count; i++) {
    lv_obj_t *child = lv_obj_get_child(bt_list, i);
    if (child->user_data) lv_mem_free(child->user_data);
  }
  lv_obj_clean(bt_list);
}

void ui_show_bt_list(void) {
  bt_selected_item = -1;
  current_screen = UI_BT;
  load_screen(screen_bt);
  ui_update_bt_list_selection();
}


void ui_init() {
  create_loading_screen();
  create_player_screen();
  create_bt_screen();
  create_nosd_screen();
  ui_show_loading();
}

void setup() {
  Serial.begin(921600);
  Serial.println("Initializing board");
  rpc_init();
  Board *board = new Board();
  board->init();
#if LVGL_PORT_AVOID_TEARING_MODE
  auto lcd = board->getLCD();
  // When avoid tearing function is enabled, the frame buffer number should be set in the board driver
  lcd->configFrameBufferNumber(LVGL_PORT_DISP_BUFFER_NUM);
#if ESP_PANEL_DRIVERS_BUS_ENABLE_RGB && CONFIG_IDF_TARGET_ESP32S3
  auto lcd_bus = lcd->getBus();
  if (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
    static_cast<BusRGB *>(lcd_bus)->configRGB_BounceBufferSize(lcd->getFrameWidth() * 10);
  }
#endif
#endif
  assert(board->begin());

  Serial.println("Initializing LVGL");
  lvgl_port_init(board->getLCD(), board->getTouch());

  Serial.println("Initialize Knob device");
  knob = new ESP_Knob(GPIO_NUM_KNOB_PIN_A, GPIO_NUM_KNOB_PIN_B);
  knob->begin();
  knob->attachLeftEventCallback(onKnobLeftEventCallback);
  knob->attachRightEventCallback(onKnobRightEventCallback);

  Serial.println("Initialize Button device");
  Button *btn = new Button((gpio_num_t)BUTTON_PIN, false);
  btn->attachSingleClickEventCb(&SingleClickCb, NULL);
  btn->attachDoubleClickEventCb(&DoubleClickCb, NULL);
  btn->attachLongPressStartEventCb(&LongPressStartCb, NULL);

  gesturesQueue = xQueueCreate(30, sizeof(Gestures_e));
  Serial.println("Creating UI");
  lvgl_port_lock(-1);
  lv_disp_set_rotation(lv_disp_get_default(), LV_DISP_ROT_180);
  ui_init();
  lvgl_port_unlock();

  encTimer = xTimerCreate("enc", 100, false, 0, sendTimerEvent);
}


void ui_change_image(const uint16_t *new_img) {
  static lv_img_dsc_t my_img_dsc = { 0 };
  my_img_dsc.header.always_zero = 0;
  my_img_dsc.header.w = UI_w;
  my_img_dsc.header.h = UI_h;
  my_img_dsc.data_size = UI_w * UI_h * 2;
  my_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR; /*Set the color format*/
  my_img_dsc.data = (const uint8_t *)new_img;
  lv_img_set_src(img_album, &my_img_dsc);
}

void loop() {
  if (current_screen == UI_LOADING && rpc_intercom_is_active()) {  // communication ready
    ui_show_player();
  } else if (current_screen != UI_LOADING && !rpc_intercom_is_active()) {  // communication error
    ui_show_loading();
  } else if (current_screen != UI_LOADING && rpc_intercom_is_active()) {  //update ui
    Gestures_e g = NO_gesture;
    while (xQueueReceive(gesturesQueue, &g, 0) == pdPASS) {
    }

    if (g != NO_gesture) {
      switch (current_screen) {
        case UI_NONE:
        case UI_LOADING:
        case UI_NO_SD:
          break;

        case UI_PLAYER:
          switch (g) {
            case L_gesture: rpc_set_new_folder(-1); break;
            case R_gesture: rpc_set_new_folder(1); break;
            case TAP_gesture: rpc_set_new_track(1); break;
            case TAPx2_gesture: rpc_set_new_track(-1); break;
            case LONG_gesture:
              ui_show_bt_list();
              rpc_start_bt_scan();
              break;
          }
          break;

        case UI_BT:
          switch (g) {
            case L_gesture:
              if (bt_selected_item > -1) {
                bt_selected_item--;
              }
              ui_update_bt_list_selection();
              break;
            case R_gesture:
              if (bt_selected_item < (int)lv_obj_get_child_cnt(lv_obj_get_parent(removeAll)) - 1) {
                bt_selected_item++;
              }
              ui_update_bt_list_selection();
              break;
            case TAP_gesture:
              {
                lv_obj_t *child = lv_obj_get_child(bt_list, bt_selected_item);
                if (removeAll == child) {
                  rpc_delete_bonding();
                } else {
                  rpc_save_bt_item((uint8_t *)((ScanItem_t *)child->user_data)->mac);
                }
              }
              break;
            case TAPx2_gesture:
              ui_bt_scan_start_anim();
              rpc_get_bt_scan_list();
              break;
            case LONG_gesture:
              rpc_set_bt_end_scan();
              ui_bt_list_clear();
              ui_show_player();
              break;
          }
          break;
      }
    }
  }

  rpc_task(0);
  delay(1);
}
