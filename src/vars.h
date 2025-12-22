#ifndef EEZ_LVGL_UI_VARS_H
#define EEZ_LVGL_UI_VARS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// enum declarations

// Flow global variables
enum FlowGlobalVariables {
    FLOW_GLOBAL_VARIABLE_STORED_TREATS_NUMBER = 0,
    FLOW_GLOBAL_VARIABLE_STORED_TREATS_HOURS = 1,
    FLOW_GLOBAL_VARIABLE_THE_TIME = 2
};

// Native global variables
#define DISPENSER_PIN 26  // GPIO connected to the treat dispenser
#define SD_CS 5

#ifndef SCHEDULE_SELECTION_VARS_H
#define SCHEDULE_SELECTION_VARS_H
extern int selected_treats_number;
extern int selected_hours_to_dispense;

// Schedule state variables
extern int schedule_treats_dispensed;
extern int schedule_remaining_minutes;
extern bool schedule_is_running;
extern bool schedule_is_paused;
extern void* schedule_timer; // Store the schedule timer reference
#endif

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_VARS_H*/