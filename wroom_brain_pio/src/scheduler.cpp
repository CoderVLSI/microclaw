#include "scheduler.h"

#include <Arduino.h>
#include <WiFi.h>
#include <stdlib.h>
#include <time.h>

#include "brain_config.h"
#include "event_log.h"
#include "persona_store.h"
#include "cron_store.h"

// Forward declaration for missed job callback
static incoming_cb_t s_dispatch_cb = nullptr;

static unsigned long s_next_status_ms = 0;
static unsigned long s_next_heartbeat_ms = 0;
static unsigned long s_next_proactive_ms = 0;
static bool s_time_configured = false;
static String s_last_tz = "";
static long s_last_tz_offset_seconds = 0;

// Cron job tracking
static unsigned long s_next_cron_check_ms = 0;
static int s_last_cron_minute = -1;
static bool s_checked_missed_jobs = false;  // Track if we've checked for missed jobs
static unsigned long s_next_reminder_check_ms = 0;
static int s_last_reminder_minute = -1;

static String to_lower_copy(String value) {
  value.toLowerCase();
  return value;
}

static String normalize_tz_for_esp(const String &tz_raw) {
  String tz = tz_raw;
  tz.trim();
  if (tz.length() == 0) {
    return "UTC0";
  }

  String lc = to_lower_copy(tz);
  if (lc == "asia/kolkata" || lc == "asia/calcutta" || lc == "india" || lc == "ist") {
    return "IST-5:30";
  }
  if (lc == "utc" || lc == "etc/utc" || lc == "gmt") {
    return "UTC0";
  }

  // Common US zones (POSIX TZ format with DST rules)
  if (lc == "america/new_york") {
    return "EST5EDT,M3.2.0/2,M11.1.0/2";
  }
  if (lc == "america/chicago") {
    return "CST6CDT,M3.2.0/2,M11.1.0/2";
  }
  if (lc == "america/denver") {
    return "MST7MDT,M3.2.0/2,M11.1.0/2";
  }
  if (lc == "america/los_angeles") {
    return "PST8PDT,M3.2.0/2,M11.1.0/2";
  }

  return tz;
}

static bool parse_offset_hhmm(const String &value, long &seconds_out) {
  String s = value;
  s.trim();
  if (s.length() == 0) {
    return false;
  }

  int sign = 1;
  if (s[0] == '+') {
    s = s.substring(1);
  } else if (s[0] == '-') {
    sign = -1;
    s = s.substring(1);
  }

  int hh = 0;
  int mm = 0;
  if (s.indexOf(':') >= 0) {
    if (sscanf(s.c_str(), "%d:%d", &hh, &mm) < 1) {
      return false;
    }
  } else {
    if (sscanf(s.c_str(), "%d", &hh) != 1) {
      return false;
    }
  }

  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
    return false;
  }

  seconds_out = sign * (hh * 3600L + mm * 60L);
  return true;
}

static long resolve_tz_offset_seconds(const String &tz_raw) {
  String tz = to_lower_copy(tz_raw);
  tz.trim();
  if (tz.length() == 0) {
    return 0;
  }

  if (tz == "asia/kolkata" || tz == "asia/calcutta" || tz == "india" || tz == "ist" ||
      tz == "ist-5:30") {
    return 19800;
  }

  if (tz == "utc" || tz == "etc/utc" || tz == "gmt" || tz == "utc0") {
    return 0;
  }

  if (tz.startsWith("utc") || tz.startsWith("gmt")) {
    String tail = tz.substring(3);
    long off = 0;
    if (parse_offset_hhmm(tail, off)) {
      return off;
    }
  }

  // POSIX-style explicit fallback: "XXX-5:30" means UTC+5:30.
  int plus_pos = tz.indexOf('+');
  int minus_pos = tz.indexOf('-');
  int pos = -1;
  if (plus_pos > 0) pos = plus_pos;
  if (minus_pos > 0 && (pos < 0 || minus_pos < pos)) pos = minus_pos;
  if (pos > 0) {
    String tail = tz.substring(pos);
    long off = 0;
    if (parse_offset_hhmm(tail, off)) {
      const char sign = tz[pos];
      if (sign == '-') {
        // POSIX TZ has reversed sign.
        off = -off;
      }
      return off;
    }
  }

  return 0;
}

static bool parse_hhmm(const String &value, int &hour_out, int &minute_out) {
  int hh = -1;
  int mm = -1;
  if (sscanf(value.c_str(), "%d:%d", &hh, &mm) != 2) {
    return false;
  }
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
    return false;
  }
  hour_out = hh;
  minute_out = mm;
  return true;
}

static void check_daily_reminder(incoming_cb_t dispatch_cb, const struct tm &tm_now) {
  const int current_minute = tm_now.tm_hour * 60 + tm_now.tm_min;
  if (current_minute == s_last_reminder_minute) {
    return;
  }
  s_last_reminder_minute = current_minute;

  String hhmm;
  String message;
  String err;
  if (!persona_get_daily_reminder(hhmm, message, err)) {
    return;
  }

  hhmm.trim();
  message.trim();
  if (hhmm.length() == 0 || message.length() == 0) {
    return;
  }

  int target_hour = -1;
  int target_minute = -1;
  if (!parse_hhmm(hhmm, target_hour, target_minute)) {
    return;
  }

  if (tm_now.tm_hour == target_hour && tm_now.tm_min == target_minute) {
    event_log_append("SCHED: reminder_run " + hhmm);
    dispatch_cb(String("reminder_run"));
    Serial.printf("[scheduler] Daily reminder triggered at %s\n", hhmm.c_str());
  }
}

static long runtime_tz_offset_seconds() {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    return 0;
  }

  struct tm local_tm{};
  struct tm utc_tm{};
  localtime_r(&now, &local_tm);
  gmtime_r(&now, &utc_tm);

  const time_t local_epoch = mktime(&local_tm);
  const time_t utc_epoch = mktime(&utc_tm);
  return (long)difftime(local_epoch, utc_epoch);
}

static void ensure_time_configured() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  String tz = String(TIMEZONE_TZ);
  String stored_tz;
  String err;
  if (persona_get_timezone(stored_tz, err)) {
    stored_tz.trim();
    if (stored_tz.length() > 0) {
      tz = stored_tz;
    }
  }
  tz = normalize_tz_for_esp(tz);
  if (s_time_configured && tz == s_last_tz) {
    return;
  }

  setenv("TZ", tz.c_str(), 1);
  tzset();
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);

  s_time_configured = true;
  s_last_tz = tz;
  long offset = runtime_tz_offset_seconds();
  if (offset == 0) {
    offset = resolve_tz_offset_seconds(tz);
  }
  s_last_tz_offset_seconds = offset;
  Serial.println("[scheduler] time sync configured: " + tz + " offset=" + String((long)s_last_tz_offset_seconds));
}

bool scheduler_get_local_time(struct tm &tm_out) {
  ensure_time_configured();
  time_t now = time(nullptr);
  if (now < 1700000000) {
    return false;
  }
  localtime_r(&now, &tm_out);
  s_last_tz_offset_seconds = runtime_tz_offset_seconds();
  return true;
}

// Check for missed cron jobs (runs once after time sync)
static void check_missed_cron_jobs(incoming_cb_t dispatch_cb) {
  if (s_checked_missed_jobs) {
    return;  // Already checked
  }

  time_t now = time(nullptr);
  if (now < 1700000000) {
    return;  // Time not synced yet
  }

  MissedJob missed[10];
  int missed_count = cron_store_check_missed_jobs(now, missed, 10);

  if (missed_count > 0) {
    for (int i = 0; i < missed_count; i++) {
      String cmd = missed[i].command;
      cmd.trim();

      char time_buf[32];
      snprintf(time_buf, sizeof(time_buf), "%02d:%02d",
               missed[i].missed_hour, missed[i].missed_minute);

      String msg = "🔄 Missed job from " + String(time_buf) + ": " + cmd;
      event_log_append("SCHED: " + msg);
      dispatch_cb(cmd);

      Serial.printf("[scheduler] Triggering missed job: %s\n", msg.c_str());
    }
  }

  // Update last check time to now
  cron_store_update_last_check(now);
  s_checked_missed_jobs = true;

  Serial.printf("[scheduler] Missed job check complete, found %d missed job(s)\n", missed_count);
}

void scheduler_init() {
  if (!AUTONOMOUS_STATUS_ENABLED) {
    Serial.println("[scheduler] autonomous status disabled");
  } else {
    s_next_status_ms = millis() + AUTONOMOUS_STATUS_MS;
    Serial.println("[scheduler] autonomous status enabled");
  }

  if (!HEARTBEAT_ENABLED) {
    Serial.println("[scheduler] heartbeat disabled");
  } else {
    s_next_heartbeat_ms = millis() + HEARTBEAT_INTERVAL_MS;
    Serial.println("[scheduler] heartbeat enabled");
  }

  if (!PROACTIVE_ENABLED) {
    Serial.println("[scheduler] proactive agent disabled");
  } else {
    s_next_proactive_ms = millis() + PROACTIVE_INTERVAL_MS;
    Serial.println("[scheduler] proactive agent enabled (every " + String(PROACTIVE_INTERVAL_MS / 60000) + "m)");
  }

  s_next_cron_check_ms = millis() + 5000;
  s_next_reminder_check_ms = millis() + 5000;
  Serial.println("[scheduler] cron jobs enabled");
}

void scheduler_tick(incoming_cb_t dispatch_cb) {
  if (dispatch_cb == nullptr) {
    return;
  }

  const unsigned long now = millis();

  if (AUTONOMOUS_STATUS_ENABLED && (long)(now - s_next_status_ms) >= 0) {
    event_log_append("SCHED: status");
    dispatch_cb(String("status"));
    s_next_status_ms = now + AUTONOMOUS_STATUS_MS;
  }

  if (HEARTBEAT_ENABLED && (long)(now - s_next_heartbeat_ms) >= 0) {
    String heartbeat;
    String err;
    if (persona_get_heartbeat(heartbeat, err)) {
      heartbeat.trim();
      if (heartbeat.length() > 0) {
        event_log_append("SCHED: heartbeat_run");
        dispatch_cb(String("heartbeat_run"));
      }
    }
    s_next_heartbeat_ms = now + HEARTBEAT_INTERVAL_MS;
  }

  // Proactive agent check
  if (PROACTIVE_ENABLED && (long)(now - s_next_proactive_ms) >= 0) {
    event_log_append("SCHED: proactive_check");
    dispatch_cb(String("proactive_check"));
    s_next_proactive_ms = now + PROACTIVE_INTERVAL_MS;
  }

  // Cron job checking
  if ((long)(now - s_next_cron_check_ms) >= 0) {
    s_next_cron_check_ms = now + 15000;

    struct tm tm_now{};
    if (!scheduler_get_local_time(tm_now)) {
      // No time sync yet, skip cron check
      return;
    }

    // Check for missed jobs on first successful time sync
    check_missed_cron_jobs(dispatch_cb);

    const int current_minute = tm_now.tm_hour * 60 + tm_now.tm_min;

    // Only check once per minute to avoid duplicate triggers
    if (current_minute != s_last_cron_minute) {
      s_last_cron_minute = current_minute;

      CronJob jobs[CRON_MAX_JOBS];
      int job_count = cron_store_get_all(jobs, CRON_MAX_JOBS);

      for (int i = 0; i < job_count; i++) {
        const CronJob &job = jobs[i];

        if (cron_should_trigger(job, tm_now.tm_hour, tm_now.tm_min,
                               tm_now.tm_mday, tm_now.tm_mon + 1, tm_now.tm_wday)) {
          String cmd = job.command;
          cmd.trim();

          event_log_append("SCHED: cron triggered: " + cmd);
          dispatch_cb(cmd);
          Serial.printf("[scheduler] Cron job triggered: %s\n", cmd.c_str());
        }
      }

      // Update last check time after successful check
      time_t current_time = time(nullptr);
      if (current_time >= 1700000000) {
        cron_store_update_last_check(current_time);
      }
    }
  }

  // Daily reminder check
  if ((long)(now - s_next_reminder_check_ms) >= 0) {
    s_next_reminder_check_ms = now + 15000;

    struct tm tm_now{};
    if (!scheduler_get_local_time(tm_now)) {
      return;
    }

    check_daily_reminder(dispatch_cb, tm_now);
  }
}

void scheduler_time_debug(String &out) {
  ensure_time_configured();
  time_t now = time(nullptr);

  out = "Time:\n";
  out += "tz_active=" + s_last_tz + "\n";
  out += "tz_offset_sec=" + String((long)s_last_tz_offset_seconds) + "\n";
  out += "epoch=" + String((long)now) + "\n";
  out += "synced=" + String(now >= 1700000000 ? "yes" : "no");

  if (now >= 1700000000) {
    struct tm tm_now{};
    localtime_r(&now, &tm_now);
    s_last_tz_offset_seconds = runtime_tz_offset_seconds();
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    out += "\nlocal=" + String(buf);
  }
}
