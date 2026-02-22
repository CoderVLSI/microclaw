#include "persona_store.h"

#include <Arduino.h>
#include <Preferences.h>

#include "brain_config.h"

namespace {

Preferences g_prefs;
bool g_ready = false;
const char *kNamespace = "brainpersona";
const char *kSoulKey = "soul";
const char *kHeartbeatKey = "heartbeat";
const char *kReminderTimeKey = "rtime";
const char *kReminderMsgKey = "rmsg";
const char *kTimezoneKey = "tz";
const char *kSafeModeKey = "safe";
const char *kEmailToKey = "emailto";
const char *kEmailSubjectKey = "emailsub";
const char *kEmailBodyKey = "emailbody";
const char *kOnboardingDoneKey = "onb_done";
const char *kOnboardingStepKey = "onb_step";
const char *kOnboardingProviderKey = "onb_prov";
const char *kOnboardingUserNameKey = "onb_user";
const char *kOnboardingBotNameKey = "onb_bot";
const char *kOnboardingPurposeKey = "onb_purp";

bool ensure_ready(String &error_out) {
  if (g_ready) {
    return true;
  }
  if (!g_prefs.begin(kNamespace, false)) {
    error_out = "NVS begin failed";
    return false;
  }
  g_ready = true;
  return true;
}

String sanitize_and_limit(const String &input, size_t max_chars) {
  String cleaned = input;
  cleaned.trim();
  if (cleaned.length() <= max_chars) {
    return cleaned;
  }
  return cleaned.substring(0, max_chars);
}

bool set_key_limited(const char *key, const String &value, size_t max_chars, String &error_out) {
  if (!ensure_ready(error_out)) {
    return false;
  }
  const String cleaned = sanitize_and_limit(value, max_chars);
  size_t written = g_prefs.putString(key, cleaned);
  if (written == 0 && cleaned.length() > 0) {
    error_out = "failed to write key";
    return false;
  }
  return true;
}

bool get_key(const char *key, String &value_out, String &error_out) {
  if (!ensure_ready(error_out)) {
    return false;
  }
  value_out = g_prefs.getString(key, "");
  return true;
}

bool clear_key(const char *key, String &error_out) {
  if (!ensure_ready(error_out)) {
    return false;
  }
  g_prefs.remove(key);
  return true;
}

}  // namespace

void persona_init() {
  String err;
  if (ensure_ready(err)) {
    Serial.println("[persona] NVS persona ready");
    // No default soul - SOUL.md in SPIFFS is now the source of truth
  } else {
    Serial.println("[persona] init failed");
  }
}

bool persona_get_soul(String &soul_out, String &error_out) {
  return get_key(kSoulKey, soul_out, error_out);
}

bool persona_set_soul(const String &soul, String &error_out) {
  return set_key_limited(kSoulKey, soul, SOUL_MAX_CHARS, error_out);
}

bool persona_clear_soul(String &error_out) {
  return clear_key(kSoulKey, error_out);
}

bool persona_get_heartbeat(String &heartbeat_out, String &error_out) {
  return get_key(kHeartbeatKey, heartbeat_out, error_out);
}

bool persona_set_heartbeat(const String &heartbeat, String &error_out) {
  return set_key_limited(kHeartbeatKey, heartbeat, HEARTBEAT_MAX_CHARS, error_out);
}

bool persona_clear_heartbeat(String &error_out) {
  return clear_key(kHeartbeatKey, error_out);
}

bool persona_set_daily_reminder(const String &hhmm, const String &message, String &error_out) {
  if (!ensure_ready(error_out)) {
    return false;
  }
  String time_clean = hhmm;
  time_clean.trim();
  String msg_clean = sanitize_and_limit(message, REMINDER_MSG_MAX_CHARS);

  size_t w1 = g_prefs.putString(kReminderTimeKey, time_clean);
  if (w1 == 0 && time_clean.length() > 0) {
    error_out = "failed to write reminder time";
    return false;
  }
  size_t w2 = g_prefs.putString(kReminderMsgKey, msg_clean);
  if (w2 == 0 && msg_clean.length() > 0) {
    error_out = "failed to write reminder message";
    return false;
  }
  return true;
}

bool persona_get_daily_reminder(String &hhmm_out, String &message_out, String &error_out) {
  if (!ensure_ready(error_out)) {
    return false;
  }
  hhmm_out = g_prefs.getString(kReminderTimeKey, "");
  message_out = g_prefs.getString(kReminderMsgKey, "");
  return true;
}

bool persona_clear_daily_reminder(String &error_out) {
  if (!ensure_ready(error_out)) {
    return false;
  }
  g_prefs.remove(kReminderTimeKey);
  g_prefs.remove(kReminderMsgKey);
  return true;
}

bool persona_set_timezone(const String &tz, String &error_out) {
  if (!ensure_ready(error_out)) {
    return false;
  }
  String cleaned = sanitize_and_limit(tz, 64);
  size_t written = g_prefs.putString(kTimezoneKey, cleaned);
  if (written == 0 && cleaned.length() > 0) {
    error_out = "failed to write timezone";
    return false;
  }
  return true;
}

bool persona_get_timezone(String &tz_out, String &error_out) {
  if (!ensure_ready(error_out)) {
    return false;
  }
  tz_out = g_prefs.getString(kTimezoneKey, "");
  return true;
}

bool persona_clear_timezone(String &error_out) {
  return clear_key(kTimezoneKey, error_out);
}

bool persona_set_safe_mode(bool enabled, String &error_out) {
  if (!ensure_ready(error_out)) {
    return false;
  }
  g_prefs.putUChar(kSafeModeKey, enabled ? 1 : 0);
  return true;
}

bool persona_get_safe_mode(bool &enabled_out, String &error_out) {
  if (!ensure_ready(error_out)) {
    return false;
  }
  enabled_out = g_prefs.getUChar(kSafeModeKey, 0) == 1;
  return true;
}

bool persona_set_email_draft(const String &to, const String &subject, const String &body,
                             String &error_out) {
  if (!ensure_ready(error_out)) {
    return false;
  }

  String to_clean = sanitize_and_limit(to, 120);
  String subject_clean = sanitize_and_limit(subject, 180);
  String body_clean = sanitize_and_limit(body, 800);

  size_t w1 = g_prefs.putString(kEmailToKey, to_clean);
  size_t w2 = g_prefs.putString(kEmailSubjectKey, subject_clean);
  size_t w3 = g_prefs.putString(kEmailBodyKey, body_clean);
  if ((w1 == 0 && to_clean.length() > 0) || (w2 == 0 && subject_clean.length() > 0) ||
      (w3 == 0 && body_clean.length() > 0)) {
    error_out = "failed to write email draft";
    return false;
  }
  return true;
}

bool persona_get_email_draft(String &to_out, String &subject_out, String &body_out,
                             String &error_out) {
  if (!ensure_ready(error_out)) {
    return false;
  }
  to_out = g_prefs.getString(kEmailToKey, "");
  subject_out = g_prefs.getString(kEmailSubjectKey, "");
  body_out = g_prefs.getString(kEmailBodyKey, "");
  return true;
}

bool persona_clear_email_draft(String &error_out) {
  if (!ensure_ready(error_out)) {
    return false;
  }
  g_prefs.remove(kEmailToKey);
  g_prefs.remove(kEmailSubjectKey);
  g_prefs.remove(kEmailBodyKey);
  return true;
}

bool persona_set_onboarding_done(bool done, String &error_out) {
  if (!ensure_ready(error_out)) {
    return false;
  }
  g_prefs.putUChar(kOnboardingDoneKey, done ? 1 : 0);
  return true;
}

bool persona_get_onboarding_done(bool &done_out, String &error_out) {
  if (!ensure_ready(error_out)) {
    return false;
  }
  done_out = g_prefs.getUChar(kOnboardingDoneKey, 0) == 1;
  return true;
}

bool persona_set_onboarding_step(const String &step, String &error_out) {
  return set_key_limited(kOnboardingStepKey, step, 24, error_out);
}

bool persona_get_onboarding_step(String &step_out, String &error_out) {
  return get_key(kOnboardingStepKey, step_out, error_out);
}

bool persona_set_onboarding_provider(const String &provider, String &error_out) {
  return set_key_limited(kOnboardingProviderKey, provider, 24, error_out);
}

bool persona_get_onboarding_provider(String &provider_out, String &error_out) {
  return get_key(kOnboardingProviderKey, provider_out, error_out);
}

bool persona_set_onboarding_user_name(const String &name, String &error_out) {
  return set_key_limited(kOnboardingUserNameKey, name, 48, error_out);
}

bool persona_get_onboarding_user_name(String &name_out, String &error_out) {
  return get_key(kOnboardingUserNameKey, name_out, error_out);
}

bool persona_set_onboarding_bot_name(const String &name, String &error_out) {
  return set_key_limited(kOnboardingBotNameKey, name, 48, error_out);
}

bool persona_get_onboarding_bot_name(String &name_out, String &error_out) {
  return get_key(kOnboardingBotNameKey, name_out, error_out);
}

bool persona_set_onboarding_purpose(const String &purpose, String &error_out) {
  return set_key_limited(kOnboardingPurposeKey, purpose, 180, error_out);
}

bool persona_get_onboarding_purpose(String &purpose_out, String &error_out) {
  return get_key(kOnboardingPurposeKey, purpose_out, error_out);
}

bool persona_clear_onboarding_state(String &error_out) {
  if (!ensure_ready(error_out)) {
    return false;
  }
  g_prefs.remove(kOnboardingStepKey);
  g_prefs.remove(kOnboardingProviderKey);
  g_prefs.remove(kOnboardingUserNameKey);
  g_prefs.remove(kOnboardingBotNameKey);
  g_prefs.remove(kOnboardingPurposeKey);
  return true;
}
