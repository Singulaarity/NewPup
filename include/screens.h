#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _objects_t {
    lv_obj_t *main;
    lv_obj_t *train;
    lv_obj_t *schedule_1;
    lv_obj_t *schedule_2;
    lv_obj_t *schedule_3;
    lv_obj_t *settings;
    lv_obj_t *splash;
    lv_obj_t *bottom_manual_tab;
    lv_obj_t *manual;
    lv_obj_t *bottom_train_tab;
    lv_obj_t *training;
    lv_obj_t *bottom_schedule_tab;
    lv_obj_t *manual_2;
    lv_obj_t *bottom_settings_tab;
    lv_obj_t *settings_2;
    lv_obj_t *obj0;
    lv_obj_t *obj1;
    lv_obj_t *current_time;
    lv_obj_t *treats_dispensed;
    lv_obj_t *manual_treat_button;
    lv_obj_t *obj2;
    lv_obj_t *obj3;
    lv_obj_t *obj4;
    lv_obj_t *start_training_button;
    lv_obj_t *obj5;
    lv_obj_t *stop_training_button;
    lv_obj_t *obj6;
    lv_obj_t *obj7;
    lv_obj_t *obj8;
    lv_obj_t *schedule_treats_to_dispense;
    lv_obj_t *schedule_treats_to_dispense_button;
    lv_obj_t *obj9;
    lv_obj_t *obj10;
    lv_obj_t *obj11;
    lv_obj_t *schedule_hours_to_dispense;
    lv_obj_t *schedule_hours_to_dispense_button;
    lv_obj_t *obj12;
    lv_obj_t *obj13;
    lv_obj_t *obj14;
    lv_obj_t *schedule_start_button;
    lv_obj_t *obj15;
    lv_obj_t *schedule_pause_button;
    lv_obj_t *schedule_stop_button;
    lv_obj_t *obj16;
    lv_obj_t *schedule_treats_dispensed;
    lv_obj_t *schedule_time_left;
    lv_obj_t *obj17;
    lv_obj_t *schedule_treats_per_hour;
    lv_obj_t *manual_mode;
    lv_obj_t *training_3;
    lv_obj_t *manual_7;
    lv_obj_t *settings_3;
    lv_obj_t *obj18;
    lv_obj_t *obj19;
} objects_t;

extern objects_t objects;

enum ScreensEnum {
    SCREEN_ID_MAIN = 1,
    SCREEN_ID_TRAIN = 2,
    SCREEN_ID_SCHEDULE_1 = 3,
    SCREEN_ID_SCHEDULE_2 = 4,
    SCREEN_ID_SCHEDULE_3 = 5,
    SCREEN_ID_SETTINGS = 6,
    SCREEN_ID_SPLASH = 7,
};

void create_screen_main();
void tick_screen_main();

void create_screen_train();
void tick_screen_train();

void create_screen_schedule_1();
void tick_screen_schedule_1();

void create_screen_schedule_2();
void tick_screen_schedule_2();

void create_screen_schedule_3();
void tick_screen_schedule_3();

void create_screen_settings();
void tick_screen_settings();

void create_screen_splash();
void tick_screen_splash();

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/