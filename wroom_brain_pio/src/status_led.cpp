#include "status_led.h"

#include <Arduino.h>

#include "brain_config.h"

namespace {

static const unsigned long kStartupBlinkMs = 90;
static const unsigned long kIdlePulseMs = 60;
static const unsigned long kIdlePeriodMs = 3500;
static const unsigned long kErrorBlinkMs = 120;
static const int kErrorPulseCount = 3;

bool s_busy = false;
bool s_led_on = false;
bool s_error_pending = false;
int s_error_pulses_remaining = 0;
unsigned long s_last_transition_ms = 0;
unsigned long s_next_idle_pulse_ms = 0;

void led_write(bool on) {
  pinMode(BLUE_LED_PIN, OUTPUT);
#if BLUE_LED_ACTIVE_HIGH
  digitalWrite(BLUE_LED_PIN, on ? HIGH : LOW);
#else
  digitalWrite(BLUE_LED_PIN, on ? LOW : HIGH);
#endif
  s_led_on = on;
}

void start_error_pattern() {
  s_error_pending = false;
  s_error_pulses_remaining = kErrorPulseCount;
  led_write(false);
  s_last_transition_ms = millis();
}

}  // namespace

void status_led_init() {
  pinMode(BLUE_LED_PIN, OUTPUT);
  led_write(false);

  // Quick startup signature.
  for (int i = 0; i < 2; i++) {
    led_write(true);
    delay(kStartupBlinkMs);
    led_write(false);
    delay(kStartupBlinkMs);
  }

  s_last_transition_ms = millis();
  s_next_idle_pulse_ms = s_last_transition_ms + 800;
}

void status_led_tick() {
  const unsigned long now = millis();

  if (s_busy) {
    if (!s_led_on) {
      led_write(true);
    }
    return;
  }

  if (s_error_pulses_remaining > 0) {
    if (!s_led_on) {
      if ((long)(now - s_last_transition_ms) >= (long)kErrorBlinkMs) {
        led_write(true);
        s_last_transition_ms = now;
      }
    } else if ((long)(now - s_last_transition_ms) >= (long)kErrorBlinkMs) {
      led_write(false);
      s_last_transition_ms = now;
      s_error_pulses_remaining--;
      if (s_error_pulses_remaining == 0) {
        s_next_idle_pulse_ms = now + 1000;
      }
    }
    return;
  }

  if (s_led_on) {
    if ((long)(now - s_last_transition_ms) >= (long)kIdlePulseMs) {
      led_write(false);
    }
    return;
  }

  if ((long)(now - s_next_idle_pulse_ms) >= 0) {
    led_write(true);
    s_last_transition_ms = now;
    s_next_idle_pulse_ms = now + kIdlePeriodMs;
  }
}

void status_led_set_busy(bool busy) {
  if (s_busy == busy) {
    return;
  }

  s_busy = busy;
  if (busy) {
    s_error_pending = false;
    s_error_pulses_remaining = 0;
    led_write(true);
    return;
  }

  led_write(false);
  s_last_transition_ms = millis();
  s_next_idle_pulse_ms = s_last_transition_ms + 600;
  if (s_error_pending) {
    start_error_pattern();
  }
}

void status_led_notify_error() {
  if (s_busy) {
    s_error_pending = true;
    return;
  }
  start_error_pattern();
}
