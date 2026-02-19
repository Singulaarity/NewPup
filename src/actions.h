#ifndef ACTIONS_H
#define ACTIONS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void actions_init(void);
void init_audio(void);
void setPCF8574Pin(uint8_t pin, bool state);
void action_manual_dispense_treat(lv_event_t * e);
void action_train_dispense_treat(lv_event_t * e);
void action_train_dispense_stop(lv_event_t * e);
void action_schedule_add_treat_num(lv_event_t * e);
void action_schedule_add_hours(lv_event_t * e);
void action_scheduletreatdispensestop(lv_event_t * e);
void action_scheduletreatdispensepause(lv_event_t * e);
void action_scheduletreatdispensestart(lv_event_t * e);

#ifdef __cplusplus
}
#endif

#endif // ACTIONS_H