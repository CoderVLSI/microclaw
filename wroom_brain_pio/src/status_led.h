#ifndef STATUS_LED_H
#define STATUS_LED_H

void status_led_init();
void status_led_tick();
void status_led_set_busy(bool busy);
void status_led_notify_error();

#endif
