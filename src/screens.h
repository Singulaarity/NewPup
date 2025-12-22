#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ScreensEnum {
    SCREEN_ID_MAIN = 1,
    SCREEN_ID_MANUAL = 2,
    SCREEN_ID_TRAIN = 3,
    SCREEN_ID_SCHEDULE_1 = 4,
    SCREEN_ID_SCHEDULE_2 = 5,
    SCREEN_ID_SCHEDULE_3 = 6,
    SCREEN_ID_SETTINGS = 7,
};

void create_screen_main();
void tick_screen_main();

void create_screen_manual();
void tick_screen_manual();

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

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();


#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/