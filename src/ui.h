#ifndef EEZ_LVGL_UI_GUI_H
#define EEZ_LVGL_UI_GUI_H

#include <lvgl.h>
#include "eez-flow.h"

#if !defined(EEZ_FOR_LVGL)
#include "screens.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Add the structure definition here
typedef struct _objects_t {
    // Screen objects
    lv_obj_t* main;
    lv_obj_t* manual;
    lv_obj_t* train;
    lv_obj_t* settings;
    lv_obj_t* schedule_1;
    lv_obj_t* schedule_2;
    lv_obj_t* schedule_3;
    lv_obj_t* splashed;

    // Manual screen objects
    lv_obj_t* bottom_manual_tab;
    lv_obj_t* manual_manual_button_label;
    lv_obj_t* manual_train_button;
    lv_obj_t* manual_train_label;
    lv_obj_t* manual_schedule_button;
    lv_obj_t* manual_schedule_label;
    lv_obj_t* manual_settings_button;
    lv_obj_t* manual_settings_label;
    lv_obj_t* manual_treat_button;
    lv_obj_t* manual_treat_label;

    // Train screen objects
    lv_obj_t* train_manual_button;
    lv_obj_t* train_manual_label;
    lv_obj_t* train_train_button;
    lv_obj_t* train_train_label;
    lv_obj_t* train_schedule_button;
    lv_obj_t* train_schedule_label;
    lv_obj_t* train_settings_button;
    lv_obj_t* train_settings_label;
    lv_obj_t* train_start_button;
    lv_obj_t* train_start_label;
    lv_obj_t* train_stop_button;
    lv_obj_t* train_stop_label;

    // Schedule screen objects
    lv_obj_t* bottom_schedule_tab;
    lv_obj_t* schedule_1_manual_button;
    lv_obj_t* schedule_1_manual_label;
    lv_obj_t* schedule_1_train_button;
    lv_obj_t* schedule_1_train_label;
    lv_obj_t* schedule_1_schedule_label;
    lv_obj_t* schedule_1_settings_button;
    lv_obj_t* schedule_1_next_button;
    lv_obj_t* schedule_1_treatsnumber;
    
    // Schedule 2 objects
    lv_obj_t* schedule_2_manual_button;
    lv_obj_t* schedule_2_manual_label;
    lv_obj_t* schedule_2_train_button;
    lv_obj_t* schedule_2_train_label;
    lv_obj_t* schedule_2_schedule_button;
    lv_obj_t* schedule_2_settings_button;
    lv_obj_t* schedule_2_hours_to_dispense;

    lv_obj_t* schedule_2_hours_to_dispense_button;
    lv_obj_t* schedule_2_hours_to_dispense_label;

    // Schedule 3 objects
    lv_obj_t* schedule_3_manual_button;
    lv_obj_t* bottom_train_tab;
    lv_obj_t* schedule_3_schedule_button;
    lv_obj_t* schedule_3_settings_button;
    lv_obj_t* schedule_3_startbutton;
    lv_obj_t* schedule_3_startlabel;
    lv_obj_t* schedule_3_pausebutton;
    lv_obj_t* schedule_3_stopbutton;
    lv_obj_t* schedule_3_stopbutton_label;
    lv_obj_t* schedule_time_left;

    // Settings objects
    lv_obj_t* settings_manual_button;
    lv_obj_t* settings_manual_label;
    lv_obj_t* settings_train_button;
    lv_obj_t* settings_train_label;
    lv_obj_t* settings_schedule_button;
    lv_obj_t* settings_schedule_label;
    lv_obj_t* settings_settings_button;
    lv_obj_t* settings_settings_label;
    lv_obj_t* settings_timer;

    // Other UI elements
    lv_obj_t* obj0;
    lv_obj_t* obj1;
    lv_obj_t* obj2;
    lv_obj_t* obj3;
    lv_obj_t* obj4;
    lv_obj_t* obj5;
    lv_obj_t* obj6;
    lv_obj_t* obj7;
    lv_obj_t* current_time_2;
    lv_obj_t* current_time_4;
    lv_obj_t* settings_6;
    lv_obj_t* settings_7;
    lv_obj_t* settings_8;
    lv_obj_t* manual_12;
    lv_obj_t* manual_17;
    lv_obj_t* training_6;
    lv_obj_t* schedule_3_schedulelabel;
    lv_obj_t* treats_dispensed;
    lv_obj_t* treats_per_hour;
} objects_t;

extern objects_t objects;
extern const uint8_t assets[8860];

void ui_init();
void ui_tick();

#if !defined(EEZ_FOR_LVGL)
void loadScreen(enum ScreensEnum screenId);
#endif

#ifdef __cplusplus
}
#endif

#endif // EEZ_LVGL_UI_GUI_H