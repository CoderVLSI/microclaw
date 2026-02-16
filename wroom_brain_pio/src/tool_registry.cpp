#include "tool_registry.h"

#include <Arduino.h>
#include <WiFi.h>

#include "brain_config.h"
#include "chat_history.h"
#include "event_log.h"
#include "llm_client.h"
#include "memory_store.h"
#include "model_config.h"
#include "persona_store.h"
#include "scheduler.h"
#include "task_store.h"
#include "transport_telegram.h"
#include "web_job_client.h"

namespace {

enum PendingActionType {
  PENDING_NONE = 0,
  PENDING_RELAY_SET = 1,
  PENDING_LED_FLASH = 2,
};

struct PendingAction {
  bool active;
  unsigned long id;
  PendingActionType type;
  int pin;
  int state;
  int led_count;
  unsigned long expires_ms;
};

PendingAction s_pending{false, 0, PENDING_NONE, -1, -1, 0, 0};
unsigned long s_next_pending_id = 1;
const unsigned long kPendingReminderTzMs = 180000UL;
const unsigned long kPendingReminderDetailsMs = 180000UL;

struct PendingReminderTzDraft {
  bool active;
  String hhmm;
  String message;
  unsigned long expires_ms;
};

PendingReminderTzDraft s_pending_reminder_tz{false, "", "", 0};

struct PendingReminderDetailsDraft {
  bool active;
  unsigned long expires_ms;
};

PendingReminderDetailsDraft s_pending_reminder_details{false, 0};
const char *kWebJobPrefix = "webjob:";

bool is_expired(unsigned long deadline_ms) {
  return (long)(millis() - deadline_ms) >= 0;
}

void clear_pending() {
  s_pending.active = false;
  s_pending.id = 0;
  s_pending.type = PENDING_NONE;
  s_pending.pin = -1;
  s_pending.state = -1;
  s_pending.led_count = 0;
  s_pending.expires_ms = 0;
}

void clear_pending_reminder_tz() {
  s_pending_reminder_tz.active = false;
  s_pending_reminder_tz.hhmm = "";
  s_pending_reminder_tz.message = "";
  s_pending_reminder_tz.expires_ms = 0;
}

void clear_pending_reminder_details() {
  s_pending_reminder_details.active = false;
  s_pending_reminder_details.expires_ms = 0;
}

bool has_user_timezone() {
  String tz;
  String err;
  if (!persona_get_timezone(tz, err)) {
    return false;
  }
  tz.trim();
  return tz.length() > 0;
}

bool has_daily_words(const String &text_lc) {
  return (text_lc.indexOf("every day") >= 0) || (text_lc.indexOf("everyday") >= 0) ||
         (text_lc.indexOf("daily") >= 0) || (text_lc.indexOf("each day") >= 0);
}

bool is_webjob_message(const String &msg) {
  String lc = msg;
  lc.trim();
  lc.toLowerCase();
  return lc.startsWith(kWebJobPrefix);
}

String webjob_task_from_message(const String &msg) {
  if (!is_webjob_message(msg)) {
    return "";
  }
  String task = msg.substring(String(kWebJobPrefix).length());
  task.trim();
  return task;
}

String reminder_message_for_user(const String &msg) {
  if (is_webjob_message(msg)) {
    String task = webjob_task_from_message(msg);
    if (task.length() == 0) {
      return "(empty web job task)";
    }
    return task;
  }
  return msg;
}

String encode_webjob_message(const String &task) {
  String v = task;
  v.trim();
  return String(kWebJobPrefix) + v;
}

bool looks_like_webjob_task(const String &text_lc) {
  return (text_lc.indexOf("update") >= 0) || (text_lc.indexOf("updates") >= 0) ||
         (text_lc.indexOf("news") >= 0) || (text_lc.indexOf("search") >= 0) ||
         (text_lc.indexOf("latest") >= 0) || (text_lc.indexOf("headline") >= 0) ||
         (text_lc.indexOf("web") >= 0) || (text_lc.indexOf("research") >= 0);
}

bool is_safe_mode_enabled() {
  bool enabled = false;
  String err;
  if (!persona_get_safe_mode(enabled, err)) {
    return false;
  }
  return enabled;
}

bool relay_set_now(int pin, int state, String &out) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, state == 1 ? HIGH : LOW);
  out = "OK: relay pin " + String(pin) + " -> " + String(state);
  return true;
}

void build_help_text(String &out) {
  out =
      "Commands:\n"
      "status\n"
      "help\n"
      "health\n"
      "specs\n"
      "relay_set <pin> <0|1> (requires confirm)\n"
      "flash_led [1-20] (requires confirm)\n"
      "reminder_set_daily <HH:MM> <message>\n"
      "reminder_show | reminder_clear\n"
      "webjob_set_daily <HH:MM> <task>\n"
      "webjob_show | webjob_run | webjob_clear\n"
      "web_files_make [topic]\n"
      "timezone_show | timezone_set <Zone> | timezone_clear\n"
      "task_add <text> | task_list | task_done <id> | task_clear\n"
      "email_draft <to>|<subject>|<body> | email_show | email_clear\n"
      "safe_mode | safe_mode_on | safe_mode_off\n"
      "logs | logs_clear\n"
      "time_show\n"
      "soul_show | soul_set <text> | soul_clear\n"
      "heartbeat_show | heartbeat_set <text> | heartbeat_clear\n"
      "generate_image <prompt>\n"
      "confirm [id]\n"
      "cancel\n"
      "sensor_read <pin>\n"
      "plan <task>\n"
      "remember <note>\n"
      "memory\n"
      "forget\n"
      "model list | model status | model use <provider>\n"
      "model set <provider> <api_key> | model clear <provider>";
}

String wifi_health_line() {
  if (WiFi.status() == WL_CONNECTED) {
    return "connected ip=" + WiFi.localIP().toString() + " rssi=" + String(WiFi.RSSI());
  }
  return "disconnected";
}

}  // namespace

void tool_registry_init() {
  Serial.println(
      "[tools] allowlist: status, relay_set <pin> <0|1>, sensor_read <pin>, "
      "flash_led [count], help, health, specs, confirm, cancel, plan <task>, "
      "reminder_set_daily/reminder_show/reminder_clear, timezone_show/timezone_set/timezone_clear, "
      "webjob_set_daily/webjob_show/webjob_run/webjob_clear, "
      "web_files_make, "
      "task_add/task_list/task_done/task_clear, "
      "email_draft/email_show/email_clear, safe_mode, logs, time_show, "
      "soul_show/soul_set/soul_clear, heartbeat_show/heartbeat_set/heartbeat_clear, "
      "remember <note>, memory, forget, generate_image <prompt>, "
      "model list/model status/model use/model set/model clear");
}

static bool parse_two_ints(const String &s, const char *fmt, int *a, int *b) {
  return sscanf(s.c_str(), fmt, a, b) == 2;
}

static bool parse_one_int(const String &s, const char *fmt, int *a) {
  return sscanf(s.c_str(), fmt, a) == 1;
}

static String compact_spaces(const String &value) {
  String out;
  out.reserve(value.length());
  bool last_space = false;
  for (size_t i = 0; i < value.length(); i++) {
    const char c = value[i];
    const bool is_space = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
    if (is_space) {
      if (!last_space) {
        out += ' ';
      }
      last_space = true;
    } else {
      out += c;
      last_space = false;
    }
  }
  out.trim();
  return out;
}

static bool is_valid_timezone_string(const String &tz) {
  String v = tz;
  v.trim();
  if (v.length() == 0 || v.length() > 63) {
    return false;
  }
  for (size_t i = 0; i < v.length(); i++) {
    const char c = v[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '/' || c == '_' ||
                    c == '-' || c == '+' || c == ':';
    if (!ok) {
      return false;
    }
  }
  return true;
}

static bool extract_timezone_from_text(const String &input, String &tz_out) {
  String raw = input;
  raw.trim();
  if (raw.length() == 0) {
    return false;
  }

  String lc = raw;
  lc.toLowerCase();

  if (lc.indexOf("india") >= 0 || lc == "ist" || lc == "in") {
    tz_out = "Asia/Kolkata";
    return true;
  }
  if (lc.indexOf("kolkata") >= 0) {
    tz_out = "Asia/Kolkata";
    return true;
  }

  const char *prefixes[] = {
      "timezone_set ",
      "timezone is ",
      "my timezone is ",
      "timezone ",
      "tz is ",
      "tz ",
  };
  for (size_t i = 0; i < (sizeof(prefixes) / sizeof(prefixes[0])); i++) {
    const String p = String(prefixes[i]);
    if (lc.startsWith(p)) {
      String cand = raw.substring(p.length());
      cand.trim();
      if (is_valid_timezone_string(cand)) {
        tz_out = cand;
        return true;
      }
      return false;
    }
  }

  if (is_valid_timezone_string(raw)) {
    tz_out = raw;
    return true;
  }

  return false;
}

static bool parse_time_from_natural(const String &text_lc, int &hour24_out, int &minute_out) {
  for (int i = 0; i < (int)text_lc.length(); i++) {
    if (i > 0 && text_lc[i - 1] >= '0' && text_lc[i - 1] <= '9') {
      continue;
    }
    if (text_lc[i] < '0' || text_lc[i] > '9') {
      continue;
    }

    int j = i;
    int hour = 0;
    int digits = 0;
    while (j < (int)text_lc.length() && text_lc[j] >= '0' && text_lc[j] <= '9' && digits < 2) {
      hour = hour * 10 + (text_lc[j] - '0');
      j++;
      digits++;
    }
    if (digits == 0) {
      continue;
    }

    int minute = 0;
    bool has_minute = false;
    if (j < (int)text_lc.length() && text_lc[j] == ':') {
      j++;
      int md = 0;
      while (j < (int)text_lc.length() && text_lc[j] >= '0' && text_lc[j] <= '9' && md < 2) {
        minute = minute * 10 + (text_lc[j] - '0');
        j++;
        md++;
      }
      if (md == 0) {
        continue;
      }
      has_minute = true;
    }

    while (j < (int)text_lc.length() && text_lc[j] == ' ') {
      j++;
    }

    bool has_ampm = false;
    bool is_pm = false;
    if (j + 1 < (int)text_lc.length()) {
      if (text_lc[j] == 'a' && text_lc[j + 1] == 'm') {
        has_ampm = true;
      } else if (text_lc[j] == 'p' && text_lc[j + 1] == 'm') {
        has_ampm = true;
        is_pm = true;
      }
    }

    // Accept plain HH:MM or H am/pm patterns.
    if (!has_minute && !has_ampm) {
      continue;
    }

    if (has_ampm) {
      if (hour >= 13 && hour <= 23 && minute >= 0 && minute <= 59) {
        // User provided 24h time with am/pm suffix; keep 24h value.
        hour24_out = hour;
        minute_out = minute;
        return true;
      }
      if (hour < 1 || hour > 12 || minute < 0 || minute > 59) {
        continue;
      }
      int h24 = hour % 12;
      if (is_pm) {
        h24 += 12;
      }
      hour24_out = h24;
      minute_out = minute;
      return true;
    }

    if (hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59) {
      hour24_out = hour;
      minute_out = minute;
      return true;
    }
  }

  // Contextual fallback: "morning 6", "evening 7"
  const char *parts[] = {"morning", "afternoon", "evening", "night"};
  for (size_t p = 0; p < (sizeof(parts) / sizeof(parts[0])); p++) {
    const String tag = String(parts[p]);
    const int pos = text_lc.indexOf(tag);
    if (pos < 0) {
      continue;
    }
    int i = pos + tag.length();
    while (i < (int)text_lc.length() && text_lc[i] == ' ') {
      i++;
    }
    int hour = -1;
    if (sscanf(text_lc.substring(i).c_str(), "%d", &hour) != 1) {
      continue;
    }
    if (hour < 1 || hour > 12) {
      continue;
    }
    int h24 = hour % 12;
    if (tag == "afternoon" || tag == "evening" || tag == "night") {
      h24 += 12;
    }
    hour24_out = h24;
    minute_out = 0;
    return true;
  }

  return false;
}

static String strip_daily_words(String text_lc) {
  text_lc.replace("every day", "");
  text_lc.replace("everyday", "");
  text_lc.replace("daily", "");
  text_lc.replace("each day", "");
  text_lc = compact_spaces(text_lc);
  return text_lc;
}

static bool parse_natural_daily_reminder(const String &input, String &hhmm_out, String &message_out,
                                         bool assume_daily) {
  String text = input;
  text.trim();
  if (text.length() == 0) {
    return false;
  }

  String lc = text;
  lc.toLowerCase();
  lc = compact_spaces(lc);

  const bool has_daily = has_daily_words(lc);
  const bool looks_like_reminder =
      (lc.indexOf("remind") >= 0) || (lc.indexOf("send") >= 0) || (lc.indexOf("wake up") >= 0);
  const bool schedule_hint =
      (lc.indexOf("schedule") >= 0) || (lc.indexOf("reschedule") >= 0) ||
      (lc.indexOf("instead") >= 0);

  if (!looks_like_reminder && !schedule_hint) {
    return false;
  }

  int hh = -1;
  int mm = -1;
  if (!parse_time_from_natural(lc, hh, mm)) {
    return false;
  }

  const bool has_time_and_send = (lc.indexOf("send") >= 0);
  if (!assume_daily && !has_daily && !schedule_hint && !has_time_and_send) {
    return false;
  }

  char hhmm_buf[6];
  snprintf(hhmm_buf, sizeof(hhmm_buf), "%02d:%02d", hh, mm);
  hhmm_out = String(hhmm_buf);

  String msg_lc;
  int p = lc.lastIndexOf("send ");
  if (p >= 0) {
    msg_lc = lc.substring(p + 5);
  } else {
    p = lc.lastIndexOf("remind me to ");
    if (p >= 0) {
      msg_lc = lc.substring(p + 13);
    } else {
      p = lc.lastIndexOf("remind me ");
      if (p >= 0) {
        msg_lc = lc.substring(p + 10);
      }
    }
  }

  msg_lc = strip_daily_words(msg_lc);
  msg_lc.replace(" at ", " ");
  msg_lc.replace(" am", "");
  msg_lc.replace(" pm", "");
  msg_lc.replace("morning", "");
  msg_lc.replace("evening", "");
  msg_lc.replace("afternoon", "");
  msg_lc.replace("night", "");
  msg_lc = compact_spaces(msg_lc);

  // Remove leading time tokens if still present.
  if (msg_lc.length() >= 2 && msg_lc[0] >= '0' && msg_lc[0] <= '9') {
    int cut = msg_lc.indexOf(' ');
    if (cut > 0) {
      msg_lc = msg_lc.substring(cut + 1);
      msg_lc = compact_spaces(msg_lc);
    }
  }

  if (msg_lc.length() == 0) {
    msg_lc = "pls wake up";
  }

  message_out = msg_lc;
  return true;
}

static bool parse_natural_reminder_time_change(const String &input, String &hhmm_out) {
  String text = input;
  text.trim();
  if (text.length() == 0) {
    return false;
  }

  String lc = compact_spaces(text);
  lc.toLowerCase();

  const bool has_change_word =
      (lc.indexOf("change") >= 0) || (lc.indexOf("reschedule") >= 0) ||
      (lc.indexOf("move") >= 0) || (lc.indexOf("shift") >= 0) ||
      (lc.indexOf("instead") >= 0) || (lc.indexOf("update") >= 0);
  if (!has_change_word) {
    return false;
  }

  // Require a reference like "change it to ...", "reschedule reminder ...", etc.
  const bool mentions_target =
      (lc.indexOf("reminder") >= 0) || (lc.indexOf(" it ") >= 0) ||
      lc.startsWith("it ") || lc.startsWith("no change it") ||
      (lc.indexOf(" time ") >= 0);
  if (!mentions_target) {
    return false;
  }

  int hh = -1;
  int mm = -1;
  if (!parse_time_from_natural(lc, hh, mm)) {
    return false;
  }

  char hhmm_buf[6];
  snprintf(hhmm_buf, sizeof(hhmm_buf), "%02d:%02d", hh, mm);
  hhmm_out = String(hhmm_buf);
  return true;
}

static String effective_timezone_for_jobs() {
  String tz = String(TIMEZONE_TZ);
  String stored_tz;
  String err;
  if (persona_get_timezone(stored_tz, err)) {
    stored_tz.trim();
    if (stored_tz.length() > 0) {
      tz = stored_tz;
    }
  }
  return tz;
}

static bool run_webjob_now_task(const String &task_input, String &out) {
  String task = compact_spaces(task_input);
  task.trim();
  if (task.length() == 0) {
    out = "Tell me what to search.\nExample: search for cricket matches today";
    return true;
  }

  String job_out;
  String err;
  if (!web_job_run(task, effective_timezone_for_jobs(), job_out, err)) {
    if (err == "WEB_JOB_ENDPOINT_URL not set" || err == "Missing WEB_SEARCH_API_KEY for Tavily") {
      out = "Web search needs setup: add WEB_SEARCH_API_KEY (Tavily) or WEB_JOB_ENDPOINT_URL.";
    } else if (err == "No quick result.") {
      out = "No good quick result found. Try a clearer query, or add Tavily key for better web search.";
    } else {
      out = "ERR: " + err;
    }
    return true;
  }

  out = job_out;
  return true;
}

static bool extract_web_query_from_text(const String &input, String &query_out) {
  String text = input;
  text.trim();
  if (text.length() == 0) {
    return false;
  }
  String lc = compact_spaces(text);
  lc.toLowerCase();

  const char *prefixes[] = {
      "search for ",
      "search ",
      "web search ",
      "look up ",
      "find ",
      "google ",
  };

  for (size_t i = 0; i < (sizeof(prefixes) / sizeof(prefixes[0])); i++) {
    String p = String(prefixes[i]);
    if (lc.startsWith(p)) {
      String q = lc.substring(p.length());
      q.trim();
      if (q.length() == 0 || q == "web" || q == "the web") {
        return false;
      }
      query_out = q;
      return true;
    }
  }

  if ((lc.startsWith("what are") || lc.startsWith("what is") || lc.startsWith("show me") ||
       lc.startsWith("give me")) &&
      looks_like_webjob_task(lc) && !has_daily_words(lc)) {
    query_out = lc;
    return true;
  }

  if (lc.indexOf("search for ") >= 0) {
    int p = lc.indexOf("search for ");
    String q = lc.substring(p + 11);
    q.trim();
    if (q.length() > 0) {
      query_out = q;
      return true;
    }
  }

  return false;
}

static bool parse_natural_daily_webjob(const String &input, String &hhmm_out, String &task_out) {
  String lc = compact_spaces(input);
  lc.toLowerCase();
  if (!has_daily_words(lc)) {
    return false;
  }
  if (!looks_like_webjob_task(lc)) {
    return false;
  }

  int hh = -1;
  int mm = -1;
  if (!parse_time_from_natural(lc, hh, mm)) {
    return false;
  }

  char hhmm_buf[6];
  snprintf(hhmm_buf, sizeof(hhmm_buf), "%02d:%02d", hh, mm);
  hhmm_out = String(hhmm_buf);

  String task = lc;
  task = strip_daily_words(task);
  task.replace("send me ", "");
  task.replace("send ", "");
  task.replace("give me ", "");
  task.replace("show me ", "");
  task.replace("please ", "");
  task.replace("pls ", "");
  task.replace(" at ", " ");
  task.replace(" am", "");
  task.replace(" pm", "");
  task.replace("morning", "");
  task.replace("afternoon", "");
  task.replace("evening", "");
  task.replace("night", "");
  task = compact_spaces(task);

  for (int i = 0; i < 3; i++) {
    if (task.length() == 0) {
      break;
    }
    if (task[0] < '0' || task[0] > '9') {
      break;
    }
    int cut = task.indexOf(' ');
    if (cut <= 0) {
      break;
    }
    task = task.substring(cut + 1);
    task = compact_spaces(task);
  }

  if (task.length() == 0) {
    task = "ai updates of the day";
  }
  task_out = task;
  return true;
}

static String sanitize_web_topic(const String &input) {
  String v = compact_spaces(input);
  if (v.length() == 0) {
    return "mini demo";
  }
  String out;
  out.reserve(v.length());
  for (size_t i = 0; i < v.length(); i++) {
    char c = v[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';
    if (ok) {
      out += c;
    }
  }
  out = compact_spaces(out);
  if (out.length() == 0) {
    out = "mini demo";
  }
  if (out.length() > 40) {
    out = out.substring(0, 40);
  }
  return out;
}

static bool extract_web_files_topic_from_text(const String &input, String &topic_out) {
  String text = input;
  text.trim();
  if (text.length() == 0) {
    return false;
  }

  String lc = compact_spaces(text);
  lc.toLowerCase();

  const bool asks_build = (lc.indexOf("make") >= 0) || (lc.indexOf("create") >= 0) ||
                          (lc.indexOf("build") >= 0) || (lc.indexOf("generate") >= 0) ||
                          (lc.indexOf("gen ") >= 0);
  const bool has_html = (lc.indexOf("html") >= 0) || (lc.indexOf("htm l") >= 0) ||
                        (lc.indexOf("webpage") >= 0) || (lc.indexOf("web page") >= 0);
  const bool has_css = (lc.indexOf("css") >= 0) || (lc.indexOf("style") >= 0);
  const bool has_js = (lc.indexOf("js") >= 0) || (lc.indexOf("javascript") >= 0);
  const bool has_site_words = (lc.indexOf("website") >= 0) || (lc.indexOf("web site") >= 0) ||
                              (lc.indexOf("landing page") >= 0) || (lc.indexOf("saas") >= 0);
  const bool has_style_words =
      (lc.indexOf("stunning") >= 0) || (lc.indexOf("modern") >= 0) ||
      (lc.indexOf("premium") >= 0) || (lc.indexOf("beautiful") >= 0) ||
      (lc.indexOf("polish") >= 0) || (lc.indexOf("revamp") >= 0) ||
      (lc.indexOf("better") >= 0) || (lc.indexOf("improve") >= 0) ||
      (lc.indexOf("redesign") >= 0) || (lc.indexOf("attractive") >= 0);
  const bool asks_file_delivery =
      (lc.indexOf(" file") >= 0) || (lc.indexOf(" files") >= 0) || (lc.indexOf(" send") >= 0);
  const bool wants_web_files =
      (has_html && (has_css || has_js || has_site_words || asks_file_delivery || has_style_words)) ||
      (has_site_words && (asks_file_delivery || has_style_words)) ||
      (has_style_words && (has_site_words || has_html));
  if (!(asks_build && wants_web_files)) {
    return false;
  }

  int p = lc.indexOf(" for ");
  String topic = "";
  if (p >= 0) {
    topic = lc.substring(p + 5);
  }
  if (topic.length() == 0) {
    if (lc.indexOf("saas") >= 0) {
      topic = "saas website";
    } else if (has_site_words && has_style_words) {
      topic = "stunning website";
    } else if (has_site_words) {
      topic = "website";
    }
  }
  topic.replace(" and send", "");
  topic.replace(" send", "");
  topic.replace(" as file", "");
  topic.replace(" as files", "");
  topic.replace(" file", "");
  topic.replace(" files", "");
  topic.replace(" more stunning", "");
  topic.replace(" stunning", "");
  topic.replace(" more modern", "");
  topic.replace(" modern", "");
  topic.replace(" improve", "");
  topic.replace(" improved", "");
  topic.replace(" redesign", "");
  topic.replace(" website", "");
  topic = compact_spaces(topic);
  topic = sanitize_web_topic(topic);
  topic_out = topic;
  return true;
}

static void build_small_web_files(const String &topic, String &html_out, String &css_out,
                                  String &js_out) {
  String t = topic;
  t.trim();
  if (t.length() == 0) {
    t = "saas website";
  }

  html_out =
      "<!doctype html>\n"
      "<html lang=\"en\">\n"
      "<head>\n"
      "  <meta charset=\"utf-8\" />\n"
      "  <meta name=\"viewport\" content=\"width=device-width,initial-scale=1\" />\n"
      "  <title>" + t + " | AI SaaS</title>\n"
      "  <link rel=\"stylesheet\" href=\"styles.css\" />\n"
      "</head>\n"
      "<body>\n"
      "  <div class=\"bg-orb orb-a\"></div>\n"
      "  <div class=\"bg-orb orb-b\"></div>\n"
      "  <header class=\"nav\">\n"
      "    <div class=\"brand\">clawflow</div>\n"
      "    <a class=\"nav-cta\" href=\"#pricing\">Start Free</a>\n"
      "  </header>\n"
      "  <main class=\"hero reveal\">\n"
      "    <p class=\"eyebrow\">Launch faster with automation</p>\n"
      "    <h1>" + t + " that ships outcomes, not busywork.</h1>\n"
      "    <p class=\"sub\">Automate repetitive ops, visualize growth, and keep teams aligned with a practical AI workflow stack.</p>\n"
      "    <div class=\"actions\">\n"
      "      <button id=\"demoBtn\" class=\"btn btn-primary\">Book Demo</button>\n"
      "      <button id=\"tourBtn\" class=\"btn btn-ghost\">See Product Tour</button>\n"
      "    </div>\n"
      "    <p id=\"out\" class=\"out\"></p>\n"
      "  </main>\n"
      "  <section class=\"features\">\n"
      "    <article class=\"card reveal\"><h3>Automations</h3><p>Build no-code flows for onboarding, support, and reporting.</p></article>\n"
      "    <article class=\"card reveal\"><h3>Live Insights</h3><p>Track pipeline health, churn risk, and key metrics in one place.</p></article>\n"
      "    <article class=\"card reveal\"><h3>Team Velocity</h3><p>Turn requests into prioritized tasks with transparent ownership.</p></article>\n"
      "  </section>\n"
      "  <section class=\"pricing reveal\" id=\"pricing\">\n"
      "    <h2>Simple pricing</h2>\n"
      "    <p>$29/mo starter, $99/mo growth, enterprise with custom SLAs.</p>\n"
      "  </section>\n"
      "  <script src=\"script.js\"></script>\n"
      "</body>\n"
      "</html>\n";

  css_out =
      "@import url('https://fonts.googleapis.com/css2?family=Space+Grotesk:wght@400;500;700&display=swap');\n"
      ":root {\n"
      "  --bg-1: #081521;\n"
      "  --bg-2: #10293a;\n"
      "  --ink: #e9f2ff;\n"
      "  --muted: #9bb4c9;\n"
      "  --line: rgba(255,255,255,.14);\n"
      "  --accent: #44f2b8;\n"
      "  --accent-2: #ffb347;\n"
      "}\n"
      "* { box-sizing: border-box; }\n"
      "html, body { margin: 0; }\n"
      "body {\n"
      "  min-height: 100vh;\n"
      "  font-family: 'Space Grotesk', 'Segoe UI', sans-serif;\n"
      "  color: var(--ink);\n"
      "  background: radial-gradient(circle at 12% 20%, #11405f, transparent 34%),\n"
      "              radial-gradient(circle at 90% 14%, #4b2b14, transparent 30%),\n"
      "              linear-gradient(160deg, var(--bg-1), var(--bg-2));\n"
      "  padding: 20px clamp(16px, 4vw, 40px) 40px;\n"
      "}\n"
      ".bg-orb { position: fixed; width: 260px; height: 260px; border-radius: 50%; filter: blur(42px); opacity: .28; z-index: -1; animation: drift 12s ease-in-out infinite; }\n"
      ".orb-a { background: var(--accent); top: 8%; left: -40px; }\n"
      ".orb-b { background: var(--accent-2); bottom: 4%; right: -40px; animation-delay: -4s; }\n"
      ".nav { display: flex; justify-content: space-between; align-items: center; margin-bottom: 28px; }\n"
      ".brand { font-weight: 700; letter-spacing: .08em; text-transform: uppercase; }\n"
      ".nav-cta { color: #032b1f; text-decoration: none; background: var(--accent); padding: 10px 14px; border-radius: 10px; font-weight: 700; }\n"
      ".hero { max-width: 860px; }\n"
      ".eyebrow { color: var(--accent); text-transform: uppercase; letter-spacing: .09em; font-size: .78rem; }\n"
      "h1 { margin: 8px 0 12px; font-size: clamp(1.9rem, 5.5vw, 3.7rem); line-height: 1.05; max-width: 18ch; }\n"
      ".sub { color: var(--muted); max-width: 56ch; font-size: 1.03rem; }\n"
      ".actions { margin-top: 18px; display: flex; gap: 12px; flex-wrap: wrap; }\n"
      ".btn { border: 0; border-radius: 12px; padding: 11px 16px; font-weight: 700; cursor: pointer; transition: transform .2s ease, box-shadow .2s ease; }\n"
      ".btn:hover { transform: translateY(-2px); box-shadow: 0 14px 28px rgba(0,0,0,.24); }\n"
      ".btn-primary { background: linear-gradient(135deg, var(--accent), #8ffff0); color: #023026; }\n"
      ".btn-ghost { background: rgba(255,255,255,.06); color: var(--ink); border: 1px solid var(--line); }\n"
      ".out { min-height: 20px; margin-top: 12px; color: #b8fff0; }\n"
      ".features { margin-top: 34px; display: grid; gap: 14px; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); }\n"
      ".card { background: rgba(255,255,255,.05); border: 1px solid var(--line); border-radius: 16px; padding: 16px; backdrop-filter: blur(6px); }\n"
      ".card h3 { margin: 0 0 6px; }\n"
      ".card p { margin: 0; color: var(--muted); }\n"
      ".pricing { margin-top: 26px; padding: 18px; border: 1px solid var(--line); border-radius: 16px; background: rgba(0,0,0,.18); }\n"
      ".pricing h2 { margin: 0 0 8px; }\n"
      ".pricing p { margin: 0; color: #d1e0ed; }\n"
      ".reveal { opacity: 0; transform: translateY(12px); }\n"
      ".reveal.is-on { opacity: 1; transform: translateY(0); transition: opacity .55s ease, transform .55s ease; }\n"
      "@keyframes drift { 0%, 100% { transform: translateY(0); } 50% { transform: translateY(-12px); } }\n"
      "@media (max-width: 640px) { .nav { margin-bottom: 18px; } .actions { gap: 10px; } }\n";

  js_out =
      "const btn = document.getElementById('demoBtn');\n"
      "const tourBtn = document.getElementById('tourBtn');\n"
      "const out = document.getElementById('out');\n"
      "const reveal = document.querySelectorAll('.reveal');\n"
      "reveal.forEach((el, i) => {\n"
      "  setTimeout(() => el.classList.add('is-on'), 120 + i * 120);\n"
      "});\n"
      "let demoCount = 0;\n"
      "btn.addEventListener('click', () => {\n"
      "  demoCount += 1;\n"
      "  out.textContent = 'Demo request queued (' + demoCount + ')';\n"
      "});\n"
      "tourBtn.addEventListener('click', () => {\n"
      "  out.textContent = 'Product tour sent to your inbox.';\n"
      "});\n";
}

static bool send_small_web_files(const String &topic, String &out) {
  String html;
  String css;
  String js;
  build_small_web_files(topic, html, css, js);

  bool ok_html = transport_telegram_send_document("index.html", html, "text/html",
                                                  "Generated HTML");
  delay(120);
  bool ok_css = transport_telegram_send_document("styles.css", css, "text/css",
                                                 "Generated CSS");
  delay(120);
  bool ok_js = transport_telegram_send_document("script.js", js, "application/javascript",
                                                "Generated JS");

  if (!ok_html && !ok_css && !ok_js) {
    out = "ERR: failed to send files";
    return true;
  }

  event_log_append("WEBFILES sent topic=" + topic);
  out = "Sent small web files for \"" + topic + "\".\nFiles: index.html, styles.css, script.js";
  return true;
}

static bool is_valid_hhmm(const String &value) {
  if (value.length() != 5 || value[2] != ':') {
    return false;
  }
  const char h1 = value[0];
  const char h2 = value[1];
  const char m1 = value[3];
  const char m2 = value[4];
  if (h1 < '0' || h1 > '9' || h2 < '0' || h2 > '9' || m1 < '0' || m1 > '9' || m2 < '0' || m2 > '9') {
    return false;
  }
  const int hh = (h1 - '0') * 10 + (h2 - '0');
  const int mm = (m1 - '0') * 10 + (m2 - '0');
  return hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59;
}

static void blue_led_write(bool on) {
  pinMode(BLUE_LED_PIN, OUTPUT);
#if BLUE_LED_ACTIVE_HIGH
  digitalWrite(BLUE_LED_PIN, on ? HIGH : LOW);
#else
  digitalWrite(BLUE_LED_PIN, on ? LOW : HIGH);
#endif
}

static void flash_led_now(int count, String &out) {
  for (int i = 0; i < count; i++) {
    blue_led_write(true);
    delay(BLUE_LED_FLASH_MS);
    blue_led_write(false);
    delay(BLUE_LED_FLASH_MS);
  }
  blue_led_write(false);
  out = "OK: flashed blue LED " + String(count) + "x on pin " + String(BLUE_LED_PIN);
}

static int parse_led_flash_count(const String &cmd_lc) {
  const char *patterns[] = {
      "flash_led",
      "blink_led",
      "flash led",
      "blink led",
      "flash blue led",
      "blink blue led",
  };
  const int default_count = 3;

  for (size_t i = 0; i < (sizeof(patterns) / sizeof(patterns[0])); i++) {
    const String prefix = String(patterns[i]);
    if (cmd_lc == prefix) {
      return default_count;
    }
    const String with_space = prefix + " ";
    if (cmd_lc.startsWith(with_space)) {
      const String tail = cmd_lc.substring(with_space.length());
      int count = -1;
      if (parse_one_int(tail, "%d", &count)) {
        return count;
      }
      return -1;
    }
  }

  const bool has_led = cmd_lc.indexOf("led") >= 0;
  const bool has_blue = cmd_lc.indexOf("blue") >= 0;
  const bool has_flash_word = (cmd_lc.indexOf("flash") >= 0) || (cmd_lc.indexOf("blink") >= 0);
  if (has_led && has_blue && has_flash_word) {
    int count = -1;
    if (parse_one_int(cmd_lc, "%*[^0-9]%d", &count)) {
      return count;
    }
    return default_count;
  }

  return 0;
}

static bool text_has_any(const String &text_lc, const char *const terms[], size_t term_count) {
  for (size_t i = 0; i < term_count; i++) {
    if (text_lc.indexOf(terms[i]) >= 0) {
      return true;
    }
  }
  return false;
}

static bool is_pdf_summary_request(const String &cmd_lc) {
  const char *doc_terms[] = {
      "pdf",
      "document",
      "doc file",
      "report",
  };
  const char *summary_terms[] = {
      "summar",
      "tldr",
      "tl;dr",
      "key points",
      "highlights",
      "gist",
      "explain this",
      "review this",
  };
  return text_has_any(cmd_lc, doc_terms, sizeof(doc_terms) / sizeof(doc_terms[0])) &&
         text_has_any(cmd_lc, summary_terms, sizeof(summary_terms) / sizeof(summary_terms[0]));
}

static bool is_image_understanding_request(const String &cmd_lc) {
  const char *image_terms[] = {
      "image",
      "photo",
      "picture",
      "screenshot",
      "diagram",
  };
  const char *understand_terms[] = {
      "describe",
      "what is",
      "what's in",
      "analy",
      "explain",
      "understand",
      "ocr",
      "extract text",
      "read text",
      "summar",
  };
  return text_has_any(cmd_lc, image_terms, sizeof(image_terms) / sizeof(image_terms[0])) &&
         text_has_any(cmd_lc, understand_terms,
                      sizeof(understand_terms) / sizeof(understand_terms[0]));
}

static bool extract_natural_image_prompt(const String &cmd, String &prompt_out) {
  String raw = cmd;
  raw.trim();
  String lc = raw;
  lc.toLowerCase();

  const char *prefixes[] = {
      "generate_image ",
      "generate image ",
      "generate an image of ",
      "generate a photo of ",
      "create an image of ",
      "create image of ",
      "make an image of ",
      "make a poster of ",
      "make a logo of ",
      "draw ",
  };

  for (size_t i = 0; i < (sizeof(prefixes) / sizeof(prefixes[0])); i++) {
    String p = String(prefixes[i]);
    if (lc.startsWith(p)) {
      String prompt = raw.substring(p.length());
      prompt.trim();
      if (prompt.length() > 0) {
        prompt_out = prompt;
        return true;
      }
      return false;
    }
  }

  const bool has_image_noun = (lc.indexOf("image") >= 0) || (lc.indexOf("photo") >= 0) ||
                              (lc.indexOf("poster") >= 0) || (lc.indexOf("logo") >= 0);
  const bool has_generation_verb = lc.startsWith("generate ") || lc.startsWith("create ") ||
                                   lc.startsWith("make ") || lc.startsWith("draw ");

  if (has_image_noun && has_generation_verb) {
    prompt_out = raw;
    return true;
  }
  return false;
}

static String build_media_instruction(const String &user_message, bool is_pdf_mode) {
  String msg_lc = user_message;
  msg_lc.toLowerCase();
  if (is_pdf_mode) {
    return String("Read this PDF and answer the user request clearly.\n"
                  "Format with:\n"
                  "TL;DR:\n"
                  "Key Points:\n"
                  "Action Items:\n"
                  "Risks / Open Questions:\n"
                  "If the document text is unreadable, say so clearly.\n"
                  "User request: ") +
           user_message;
  }

  if (msg_lc.indexOf("ocr") >= 0 || msg_lc.indexOf("extract text") >= 0 ||
      msg_lc.indexOf("read text") >= 0) {
    return String("Perform OCR on this image. Return:\n"
                  "1) Exact extracted text\n"
                  "2) Cleaned summary in 3 bullets\n"
                  "3) Any uncertain words/regions.\n"
                  "User request: ") +
           user_message;
  }

  return String("Analyze this image and answer the user request.\n"
                "Return concise output with:\n"
                "Scene summary\n"
                "Visible text (if any)\n"
                "Actionable takeaways.\n"
                "User request: ") +
         user_message;
}

static String normalize_command(const String &input) {
  String cmd = input;
  cmd.trim();

  if (!cmd.startsWith("/")) {
    return cmd;
  }

  cmd.remove(0, 1);  // strip leading slash for Telegram-style commands

  int space_pos = cmd.indexOf(' ');
  if (space_pos < 0) {
    int at_pos = cmd.indexOf('@');
    if (at_pos > 0) {
      cmd = cmd.substring(0, at_pos);
    }
    return cmd;
  }

  String first = cmd.substring(0, space_pos);
  int at_pos = first.indexOf('@');
  if (at_pos > 0) {
    first = first.substring(0, at_pos);
    cmd = first + cmd.substring(space_pos);
  }

  return cmd;
}

bool tool_registry_execute(const String &input, String &out) {
  String cmd = normalize_command(input);
  cmd.trim();
  String cmd_lc = cmd;
  cmd_lc.toLowerCase();

  if (s_pending.active && is_expired(s_pending.expires_ms)) {
    clear_pending();
  }
  if (s_pending_reminder_tz.active && is_expired(s_pending_reminder_tz.expires_ms)) {
    clear_pending_reminder_tz();
  }
  if (s_pending_reminder_details.active &&
      is_expired(s_pending_reminder_details.expires_ms)) {
    clear_pending_reminder_details();
  }

  if (s_pending_reminder_tz.active) {
    String guessed_tz;
    if (extract_timezone_from_text(cmd, guessed_tz)) {
      String err;
      if (!persona_set_timezone(guessed_tz, err)) {
        out = "ERR: " + err;
        return true;
      }
      if (!persona_set_daily_reminder(s_pending_reminder_tz.hhmm, s_pending_reminder_tz.message, err)) {
        out = "ERR: " + err;
        return true;
      }
      if (is_webjob_message(s_pending_reminder_tz.message)) {
        event_log_append("WEBJOB set daily " + s_pending_reminder_tz.hhmm);
      } else {
        event_log_append("REMINDER set daily " + s_pending_reminder_tz.hhmm);
      }
      String msg_for_user = reminder_message_for_user(s_pending_reminder_tz.message);
      out = "OK: timezone set to " + guessed_tz +
            "\nOK: daily reminder set at " + s_pending_reminder_tz.hhmm +
            "\nMessage: " + msg_for_user;
      clear_pending_reminder_tz();
      return true;
    }
  }

  if (cmd_lc == "help") {
    build_help_text(out);
    return true;
  }

  if (cmd_lc == "status") {
    out = "OK: alive";
    return true;
  }

  if (cmd_lc == "health") {
    String notes;
    String mem_err;
    size_t note_chars = 0;
    if (memory_get_notes(notes, mem_err)) {
      note_chars = notes.length();
    }
    String soul;
    String heartbeat;
    String persona_err;
    size_t soul_chars = 0;
    size_t heartbeat_chars = 0;
    if (persona_get_soul(soul, persona_err)) {
      soul_chars = soul.length();
    }
    if (persona_get_heartbeat(heartbeat, persona_err)) {
      heartbeat_chars = heartbeat.length();
    }

    String pending = "none";
    if (s_pending.active) {
      unsigned long remain_ms = 0;
      if (!is_expired(s_pending.expires_ms)) {
        remain_ms = s_pending.expires_ms - millis();
      }
      if (s_pending.type == PENDING_RELAY_SET) {
        pending = "relay_set id=" + String(s_pending.id) + " pin=" + String(s_pending.pin) +
                  " state=" + String(s_pending.state) + " ttl_ms=" + String(remain_ms);
      } else if (s_pending.type == PENDING_LED_FLASH) {
        pending = "flash_led id=" + String(s_pending.id) + " count=" + String(s_pending.led_count) +
                  " ttl_ms=" + String(remain_ms);
      } else {
        pending = "unknown id=" + String(s_pending.id) + " ttl_ms=" + String(remain_ms);
      }
    }

    out = "OK: health\n"
          "uptime_s=" + String(millis() / 1000UL) + "\n"
          "heap=" + String(ESP.getFreeHeap()) + "\n"
          "wifi=" + wifi_health_line() + "\n"
          "memory_chars=" + String(note_chars) + "\n"
          "soul_chars=" + String(soul_chars) + "\n"
          "heartbeat_chars=" + String(heartbeat_chars) + "\n"
          "pending=" + pending + "\n"
          "safe_mode=" + String(is_safe_mode_enabled() ? "on" : "off");

    String tz;
    if (persona_get_timezone(tz, persona_err)) {
      tz.trim();
      if (tz.length() == 0) {
        tz = String(TIMEZONE_TZ) + " (default)";
      }
      out += "\ntimezone=" + tz;
    }

    String rem_hhmm;
    String rem_msg;
    if (persona_get_daily_reminder(rem_hhmm, rem_msg, persona_err)) {
      rem_hhmm.trim();
      rem_msg.trim();
      if (rem_hhmm.length() > 0 && rem_msg.length() > 0) {
        if (is_webjob_message(rem_msg)) {
          String task = webjob_task_from_message(rem_msg);
          out += "\nwebjob_daily=" + rem_hhmm + " task_chars=" + String(task.length());
        } else {
          out += "\nreminder_daily=" + rem_hhmm + " msg_chars=" + String(rem_msg.length());
        }
      } else {
        out += "\nreminder_daily=none";
      }
    }
    return true;
  }

  if (cmd_lc == "specs") {
    // Chip and flash info
    out = "=== ESP32 Specs ===\n\n";
    out += "Chip: " + String(ESP.getChipModel()) + "\n";
    out += "Cores: " + String(ESP.getChipCores()) + "\n";
    out += "CPU Frequency: " + String(ESP.getCpuFreqMHz()) + " MHz\n";
    out += "Flash Size: " + String(ESP.getFlashChipSize() / 1024) + " KB\n";
    out += "Sketch Size: " + String(ESP.getSketchSize() / 1024) + " KB\n";
    out += "Free Sketch Space: " + String(ESP.getFreeSketchSpace() / 1024) + " KB\n\n";

    // RAM info
    out += "=== RAM ===\n";
    out += "Free Heap: " + String(ESP.getFreeHeap()) + " bytes\n";
    out += "Largest Free Block: " + String(ESP.getMaxAllocHeap()) + " bytes\n";
    out += "Total Heap: " + String(ESP.getHeapSize()) + " bytes\n\n";

    // PSRAM info (if available)
    if (psramFound()) {
      out += "=== PSRAM ===\n";
      out += "PSRAM Total: " + String(ESP.getPsramSize()) + " bytes\n";
      out += "PSRAM Free: " + String(ESP.getFreePsram()) + " bytes\n\n";
    } else {
      out += "=== PSRAM: Not Available ===\n\n";
    }

    // NVS Storage breakdown
    out += "=== NVS Storage (61KB partition) ===\n";
    out += "Used / Limit:\n\n";

    // Memory
    String mem;
    String mem_err;
    if (memory_get_notes(mem, mem_err)) {
      size_t used = mem.length();
      size_t limit = MEMORY_MAX_CHARS;
      int percent = (used * 100) / limit;
      out += "memory: " + String(used) + " / " + String(limit) + " chars (" + String(percent) + "%)\n";
    } else {
      out += "memory: Error\n";
    }

    // Chat history
    String chat;
    String chat_err;
    if (chat_history_get(chat, chat_err)) {
      size_t used = chat.length();
      size_t lines = 0;
      for (size_t i = 0; i < chat.length(); i++) {
        if (chat[i] == '\n') lines++;
      }
      out += "chat_history: " + String(lines) + " lines, " + String(used) + " chars\n";
    } else {
      out += "chat_history: " + String(chat_err) + "\n";
    }

    // Persona (soul + heartbeat)
    String soul, heartbeat, persona_err;
    size_t persona_used = 0;
    if (persona_get_soul(soul, persona_err)) {
      persona_used += soul.length();
    }
    if (persona_get_heartbeat(heartbeat, persona_err)) {
      persona_used += heartbeat.length();
    }
    out += "persona: " + String(persona_used) + " chars used\n";

    // Tasks
    String tasks, tasks_err;
    if (task_list(tasks, tasks_err)) {
      size_t used = tasks.length();
      size_t limit = TASKS_MAX_CHARS;
      int percent = (used * 100) / limit;
      out += "tasks: " + String(used) + " / " + String(limit) + " chars (" + String(percent) + "%)\n";
    } else {
      out += "tasks: " + tasks_err + "\n";
    }

    // Model config
    String active_provider = model_config_get_active_provider();
    out += "\n=== LLM Config ===\n";
    out += "Active Provider: " + (active_provider.length() > 0 ? active_provider : "(none)") + "\n";
    out += "Configured: " + model_config_get_configured_list() + "\n";

    // WiFi
    out += "\n=== WiFi ===\n";
    out += wifi_health_line() + "\n";
    out += "RSSI: " + String(WiFi.RSSI()) + " dBm\n";

    return true;
  }

  if (cmd_lc == "logs") {
    event_log_dump(out, 1400);
    return true;
  }

  if (cmd_lc == "logs_clear") {
    event_log_clear();
    out = "OK: logs cleared";
    return true;
  }

  if (cmd_lc == "time_show" || cmd_lc == "clock" || cmd_lc == "time") {
    scheduler_time_debug(out);
    return true;
  }

  if (cmd_lc == "safe_mode") {
    out = String("Safe mode: ") + (is_safe_mode_enabled() ? "ON" : "OFF");
    return true;
  }

  if (cmd_lc == "safe_mode_on") {
    String err;
    if (!persona_set_safe_mode(true, err)) {
      out = "ERR: " + err;
      return true;
    }
    clear_pending();
    out = "OK: safe mode ON (risky actions blocked)";
    return true;
  }

  if (cmd_lc == "safe_mode_off") {
    String err;
    if (!persona_set_safe_mode(false, err)) {
      out = "ERR: " + err;
      return true;
    }
    out = "OK: safe mode OFF";
    return true;
  }

  if (cmd_lc == "task_list") {
    String err;
    if (!task_list(out, err)) {
      out = "ERR: " + err;
    }
    return true;
  }

  if (cmd_lc == "task_clear") {
    String err;
    if (!task_clear(err)) {
      out = "ERR: " + err;
      return true;
    }
    out = "OK: tasks cleared";
    return true;
  }

  if (cmd_lc == "task_add" || cmd_lc.startsWith("task_add ")) {
    String text = cmd.length() > 8 ? cmd.substring(8) : "";
    text.trim();
    if (text.length() == 0) {
      out = "ERR: usage task_add <text>";
      return true;
    }
    int task_id = 0;
    String err;
    if (!task_add(text, task_id, err)) {
      out = "ERR: " + err;
      return true;
    }
    out = "OK: task #" + String(task_id) + " added";
    return true;
  }

  if (cmd_lc == "task_done" || cmd_lc.startsWith("task_done ")) {
    String tail = cmd.length() > 9 ? cmd.substring(9) : "";
    tail.trim();
    int id = -1;
    if (!parse_one_int(tail, "%d", &id) || id <= 0) {
      out = "ERR: usage task_done <id>";
      return true;
    }
    String err;
    if (!task_done(id, err)) {
      out = "ERR: " + err;
      return true;
    }
    out = "OK: task #" + String(id) + " done";
    return true;
  }

  if (cmd_lc == "email_show") {
    String to;
    String subject;
    String body;
    String err;
    if (!persona_get_email_draft(to, subject, body, err)) {
      out = "ERR: " + err;
      return true;
    }
    to.trim();
    subject.trim();
    body.trim();
    if (to.length() == 0 && subject.length() == 0 && body.length() == 0) {
      out = "Email draft is empty";
      return true;
    }
    out = "Email draft:\nTo: " + to + "\nSubject: " + subject + "\nBody:\n" + body;
    if (out.length() > 1400) {
      out = out.substring(0, 1400) + "...";
    }
    return true;
  }

  if (cmd_lc == "email_clear") {
    String err;
    if (!persona_clear_email_draft(err)) {
      out = "ERR: " + err;
      return true;
    }
    out = "OK: email draft cleared";
    return true;
  }

  if (cmd_lc == "email_draft" || cmd_lc.startsWith("email_draft ")) {
    String tail = cmd.length() > 11 ? cmd.substring(11) : "";
    tail.trim();
    int p1 = tail.indexOf('|');
    int p2 = p1 >= 0 ? tail.indexOf('|', p1 + 1) : -1;
    if (p1 <= 0 || p2 <= p1) {
      out = "ERR: usage email_draft <to>|<subject>|<body>";
      return true;
    }

    String to = tail.substring(0, p1);
    String subject = tail.substring(p1 + 1, p2);
    String body = tail.substring(p2 + 1);
    to.trim();
    subject.trim();
    body.trim();

    if (to.length() == 0 || subject.length() == 0 || body.length() == 0) {
      out = "ERR: usage email_draft <to>|<subject>|<body>";
      return true;
    }

    String err;
    if (!persona_set_email_draft(to, subject, body, err)) {
      out = "ERR: " + err;
      return true;
    }
    out = "OK: email draft saved (draft only, not sent)";
    return true;
  }

  if (cmd_lc == "timezone_show") {
    String tz;
    String err;
    if (!persona_get_timezone(tz, err)) {
      out = "ERR: " + err;
      return true;
    }
    tz.trim();
    if (tz.length() == 0) {
      out = "Timezone not set. Using default: " + String(TIMEZONE_TZ) +
            "\nSet with: timezone_set <Area/City>";
      return true;
    }
    out = "Timezone: " + tz;
    return true;
  }

  if (cmd_lc == "timezone_clear") {
    String err;
    if (!persona_clear_timezone(err)) {
      out = "ERR: " + err;
      return true;
    }
    out = "OK: timezone cleared. Using default " + String(TIMEZONE_TZ);
    return true;
  }

  if (cmd_lc == "timezone_set" || cmd_lc.startsWith("timezone_set ")) {
    String tz = cmd.length() > 12 ? cmd.substring(12) : "";
    tz.trim();
    if (!is_valid_timezone_string(tz)) {
      out = "ERR: usage timezone_set <Area/City or UTC offset>\n"
            "Example: timezone_set Asia/Kolkata";
      return true;
    }

    String err;
    if (!persona_set_timezone(tz, err)) {
      out = "ERR: " + err;
      return true;
    }

    if (s_pending_reminder_tz.active) {
      if (!persona_set_daily_reminder(s_pending_reminder_tz.hhmm, s_pending_reminder_tz.message, err)) {
        out = "ERR: " + err;
        return true;
      }
      if (is_webjob_message(s_pending_reminder_tz.message)) {
        event_log_append("WEBJOB set daily " + s_pending_reminder_tz.hhmm);
      } else {
        event_log_append("REMINDER set daily " + s_pending_reminder_tz.hhmm);
      }
      String msg_for_user = reminder_message_for_user(s_pending_reminder_tz.message);
      out = "OK: timezone set to " + tz + "\nOK: daily reminder set at " + s_pending_reminder_tz.hhmm +
            "\nMessage: " + msg_for_user;
      clear_pending_reminder_tz();
      return true;
    }

    out = "OK: timezone set to " + tz;
    return true;
  }

  if (cmd_lc == "reminder_show" || cmd_lc == "remainder_show" || cmd_lc == "reminder_shiw") {
    String hhmm;
    String msg;
    String err;
    if (!persona_get_daily_reminder(hhmm, msg, err)) {
      out = "ERR: " + err;
      return true;
    }
    hhmm.trim();
    msg.trim();
    if (hhmm.length() == 0 || msg.length() == 0) {
      out = "Daily reminder is empty";
      return true;
    }
    if (is_webjob_message(msg)) {
      out = "Daily web job " + hhmm + ":\nTask: " + webjob_task_from_message(msg);
      return true;
    }
    out = "Daily reminder " + hhmm + ":\n" + msg;
    return true;
  }

  if (cmd_lc == "webjob_show") {
    String hhmm;
    String msg;
    String err;
    if (!persona_get_daily_reminder(hhmm, msg, err)) {
      out = "ERR: " + err;
      return true;
    }
    hhmm.trim();
    msg.trim();
    if (hhmm.length() == 0 || msg.length() == 0 || !is_webjob_message(msg)) {
      out = "Daily web job is empty";
      return true;
    }
    out = "Daily web job " + hhmm + ":\nTask: " + webjob_task_from_message(msg);
    return true;
  }

  if (cmd_lc == "reminder_clear") {
    String err;
    if (!persona_clear_daily_reminder(err)) {
      out = "ERR: " + err;
      return true;
    }
    out = "OK: daily reminder cleared";
    return true;
  }

  if (cmd_lc == "webjob_clear") {
    String hhmm;
    String msg;
    String err;
    if (!persona_get_daily_reminder(hhmm, msg, err)) {
      out = "ERR: " + err;
      return true;
    }
    hhmm.trim();
    msg.trim();
    if (hhmm.length() == 0 || msg.length() == 0 || !is_webjob_message(msg)) {
      out = "Daily web job is empty";
      return true;
    }
    if (!persona_clear_daily_reminder(err)) {
      out = "ERR: " + err;
      return true;
    }
    out = "OK: daily web job cleared";
    return true;
  }

  if (cmd_lc == "reminder_run") {
    String hhmm;
    String msg;
    String err;
    if (!persona_get_daily_reminder(hhmm, msg, err)) {
      out = "ERR: " + err;
      return true;
    }
    hhmm.trim();
    msg.trim();
    if (hhmm.length() == 0 || msg.length() == 0) {
      out = "ERR: daily reminder is empty";
      return true;
    }
    if (is_webjob_message(msg)) {
      String task = webjob_task_from_message(msg);
      if (task.length() == 0) {
        out = "ERR: empty web job task";
        return true;
      }
      String job_out;
      if (!web_job_run(task, effective_timezone_for_jobs(), job_out, err)) {
        out = "ERR: " + err;
        return true;
      }
      out = "Web job (" + hhmm + "): " + task + "\n" + job_out;
      return true;
    }
    out = "Reminder (" + hhmm + "): " + msg;
    return true;
  }

  if (cmd_lc == "webjob_run") {
    String hhmm;
    String msg;
    String err;
    if (!persona_get_daily_reminder(hhmm, msg, err)) {
      out = "ERR: " + err;
      return true;
    }
    hhmm.trim();
    msg.trim();
    if (hhmm.length() == 0 || msg.length() == 0 || !is_webjob_message(msg)) {
      out = "ERR: daily web job is empty";
      return true;
    }
    String task = webjob_task_from_message(msg);
    if (task.length() == 0) {
      out = "ERR: empty web job task";
      return true;
    }
    String job_out;
    if (!web_job_run(task, effective_timezone_for_jobs(), job_out, err)) {
      out = "ERR: " + err;
      return true;
    }
    out = "Web job now:\n" + job_out;
    return true;
  }

  if (cmd_lc == "web_files_make" || cmd_lc.startsWith("web_files_make ")) {
    String topic = cmd.length() > 14 ? cmd.substring(14) : "";
    topic = sanitize_web_topic(topic);
    return send_small_web_files(topic, out);
  }

  String web_files_topic;
  if (extract_web_files_topic_from_text(cmd, web_files_topic)) {
    return send_small_web_files(web_files_topic, out);
  }

  String web_query;
  if (extract_web_query_from_text(cmd, web_query)) {
    return run_webjob_now_task(web_query, out);
  }
  if (cmd_lc.indexOf("search web") >= 0 || cmd_lc.indexOf("web search") >= 0) {
    out = "Yes. Tell me what to search.\nExample: search for cricket matches today";
    return true;
  }

  if (cmd_lc == "webjob_set_daily" || cmd_lc.startsWith("webjob_set_daily ")) {
    String tail = cmd.length() > 16 ? cmd.substring(16) : "";
    tail.trim();
    int sp = tail.indexOf(' ');
    if (sp <= 0) {
      out = "ERR: usage webjob_set_daily <HH:MM> <task>";
      return true;
    }

    String hhmm = tail.substring(0, sp);
    String task = tail.substring(sp + 1);
    hhmm.trim();
    task.trim();

    if (!is_valid_hhmm(hhmm) || task.length() == 0) {
      out = "ERR: usage webjob_set_daily <HH:MM> <task>";
      return true;
    }

    String encoded_msg = encode_webjob_message(task);
    String err;
    if (!has_user_timezone()) {
      s_pending_reminder_tz.active = true;
      s_pending_reminder_tz.hhmm = hhmm;
      s_pending_reminder_tz.message = encoded_msg;
      s_pending_reminder_tz.expires_ms = millis() + kPendingReminderTzMs;
      clear_pending_reminder_details();
      out = "Before I set that web job, tell me your timezone.\n"
            "Reply: timezone_set Asia/Kolkata";
      return true;
    }

    if (!persona_set_daily_reminder(hhmm, encoded_msg, err)) {
      out = "ERR: " + err;
      return true;
    }
    event_log_append("WEBJOB set daily " + hhmm);
    out = "OK: daily web job set at " + hhmm + "\nTask: " + task;
    return true;
  }

  if (cmd_lc == "reminder_set_daily" || cmd_lc.startsWith("reminder_set_daily ")) {
    String tail = cmd.length() > 18 ? cmd.substring(18) : "";
    tail.trim();
    int sp = tail.indexOf(' ');
    if (sp <= 0) {
      out = "ERR: usage reminder_set_daily <HH:MM> <message>";
      return true;
    }

    String hhmm = tail.substring(0, sp);
    String message = tail.substring(sp + 1);
    hhmm.trim();
    message.trim();

    if (!is_valid_hhmm(hhmm) || message.length() == 0) {
      out = "ERR: usage reminder_set_daily <HH:MM> <message>";
      return true;
    }

    String err;
    if (!has_user_timezone()) {
      s_pending_reminder_tz.active = true;
      s_pending_reminder_tz.hhmm = hhmm;
      s_pending_reminder_tz.message = message;
      s_pending_reminder_tz.expires_ms = millis() + kPendingReminderTzMs;
      clear_pending_reminder_details();
      out = "Before I set that reminder, tell me your timezone.\n"
            "Reply: timezone_set Asia/Kolkata";
      return true;
    }

    if (!persona_set_daily_reminder(hhmm, message, err)) {
      out = "ERR: " + err;
      return true;
    }
    if (is_webjob_message(message)) {
      event_log_append("WEBJOB set daily " + hhmm);
    } else {
      event_log_append("REMINDER set daily " + hhmm);
    }
    out = "OK: daily reminder set at " + hhmm + "\nMessage: " + reminder_message_for_user(message);
    return true;
  }

  String changed_hhmm;
  if (parse_natural_reminder_time_change(cmd, changed_hhmm)) {
    String old_hhmm;
    String old_message;
    String err;
    if (!persona_get_daily_reminder(old_hhmm, old_message, err)) {
      out = "ERR: " + err;
      return true;
    }
    old_hhmm.trim();
    old_message.trim();
    if (old_hhmm.length() == 0 || old_message.length() == 0) {
      out = "ERR: daily reminder is empty";
      return true;
    }

    if (!persona_set_daily_reminder(changed_hhmm, old_message, err)) {
      out = "ERR: " + err;
      return true;
    }

    if (is_webjob_message(old_message)) {
      event_log_append("WEBJOB set daily " + changed_hhmm);
      out = "OK: daily web job changed to " + changed_hhmm +
            "\nTask: " + webjob_task_from_message(old_message);
    } else {
      event_log_append("REMINDER set daily " + changed_hhmm);
      out = "OK: daily reminder changed to " + changed_hhmm +
            "\nMessage: " + old_message;
    }
    return true;
  }

  String webjob_hhmm;
  String webjob_task;
  if (parse_natural_daily_webjob(cmd, webjob_hhmm, webjob_task)) {
    String stored_message = encode_webjob_message(webjob_task);
    String err;
    if (!has_user_timezone()) {
      s_pending_reminder_tz.active = true;
      s_pending_reminder_tz.hhmm = webjob_hhmm;
      s_pending_reminder_tz.message = stored_message;
      s_pending_reminder_tz.expires_ms = millis() + kPendingReminderTzMs;
      clear_pending_reminder_details();
      out = "Got it. I need your timezone first.\n"
            "Reply with your timezone, for example: timezone_set Asia/Kolkata";
      return true;
    }
    if (!persona_set_daily_reminder(webjob_hhmm, stored_message, err)) {
      out = "ERR: " + err;
      return true;
    }
    event_log_append("WEBJOB set daily " + webjob_hhmm);
    out = "OK: daily web job set at " + webjob_hhmm + "\nTask: " + webjob_task;
    return true;
  }

  if (has_daily_words(cmd_lc) && !cmd_lc.startsWith("reminder_") &&
      !cmd_lc.startsWith("timezone_")) {
    s_pending_reminder_details.active = true;
    s_pending_reminder_details.expires_ms = millis() + kPendingReminderDetailsMs;
    out = "Got it, daily.\nNow send time + message, for example:\n6 am send pls wake up";
    return true;
  }

  String natural_hhmm;
  String natural_message;
  const bool assume_daily = s_pending_reminder_details.active;
  if (parse_natural_daily_reminder(cmd, natural_hhmm, natural_message, assume_daily)) {
    String err;
    String natural_lc = natural_message;
    natural_lc.toLowerCase();
    const bool should_be_webjob = looks_like_webjob_task(natural_lc);
    const String stored_message =
        should_be_webjob ? encode_webjob_message(natural_message) : natural_message;
    clear_pending_reminder_details();
    if (!has_user_timezone()) {
      s_pending_reminder_tz.active = true;
      s_pending_reminder_tz.hhmm = natural_hhmm;
      s_pending_reminder_tz.message = stored_message;
      s_pending_reminder_tz.expires_ms = millis() + kPendingReminderTzMs;
      clear_pending_reminder_details();
      out = "Got it. I need your timezone first.\n"
            "Reply with your timezone, for example: timezone_set Asia/Kolkata";
      return true;
    }
    if (!persona_set_daily_reminder(natural_hhmm, stored_message, err)) {
      out = "ERR: " + err;
      return true;
    }
    if (should_be_webjob) {
      event_log_append("WEBJOB set daily " + natural_hhmm);
      out = "OK: daily web job set at " + natural_hhmm + "\nTask: " + natural_message;
    } else {
      event_log_append("REMINDER set daily " + natural_hhmm);
      out = "OK: daily reminder set at " + natural_hhmm + "\nMessage: " + natural_message;
    }
    return true;
  }

  if (s_pending_reminder_details.active) {
    out = "I still need both time and message.\nExample: 6 am send pls wake up";
    return true;
  }

  if (cmd_lc == "soul_show" || cmd_lc == "soul") {
    String soul;
    String err;
    if (!persona_get_soul(soul, err)) {
      out = "ERR: " + err;
      return true;
    }
    soul.trim();
    if (soul.length() == 0) {
      out = "Soul is empty";
      return true;
    }
    if (soul.length() > 1400) {
      soul = soul.substring(0, 1400);
    }
    out = "SOUL:\n" + soul;
    return true;
  }

  if (cmd_lc == "soul_clear") {
    String err;
    if (!persona_clear_soul(err)) {
      out = "ERR: " + err;
      return true;
    }
    out = "OK: soul cleared";
    return true;
  }

  if (cmd_lc == "soul_set" || cmd_lc.startsWith("soul_set ")) {
    String text = cmd.length() > 8 ? cmd.substring(8) : "";
    text.trim();
    if (text.length() == 0) {
      out = "ERR: usage soul_set <text>";
      return true;
    }
    String err;
    if (!persona_set_soul(text, err)) {
      out = "ERR: " + err;
      return true;
    }
    out = "OK: soul updated";
    return true;
  }

  if (cmd_lc == "heartbeat_show") {
    String hb;
    String err;
    if (!persona_get_heartbeat(hb, err)) {
      out = "ERR: " + err;
      return true;
    }
    hb.trim();
    if (hb.length() == 0) {
      out = "Heartbeat is empty";
      return true;
    }
    if (hb.length() > 1400) {
      hb = hb.substring(0, 1400);
    }
    out = "HEARTBEAT:\n" + hb;
    return true;
  }

  if (cmd_lc == "heartbeat_clear") {
    String err;
    if (!persona_clear_heartbeat(err)) {
      out = "ERR: " + err;
      return true;
    }
    out = "OK: heartbeat cleared";
    return true;
  }

  if (cmd_lc == "heartbeat_set" || cmd_lc.startsWith("heartbeat_set ")) {
    String text = cmd.length() > 13 ? cmd.substring(13) : "";
    text.trim();
    if (text.length() == 0) {
      out = "ERR: usage heartbeat_set <text>";
      return true;
    }
    String err;
    if (!persona_set_heartbeat(text, err)) {
      out = "ERR: " + err;
      return true;
    }
    out = "OK: heartbeat updated";
    return true;
  }

  if (cmd_lc == "heartbeat_run") {
    String hb;
    String hb_err;
    if (!persona_get_heartbeat(hb, hb_err)) {
      out = "ERR: " + hb_err;
      return true;
    }
    hb.trim();
    if (hb.length() == 0) {
      out = "ERR: heartbeat is empty";
      return true;
    }

    String reply;
    String llm_err;
    if (!llm_generate_heartbeat(hb, reply, llm_err)) {
      out = "ERR: " + llm_err;
      return true;
    }
    if (reply.length() > 1400) {
      reply = reply.substring(0, 1400) + "...";
    }
    out = "Heartbeat:\n" + reply;
    return true;
  }

  if (cmd_lc == "cancel") {
    if (!s_pending.active) {
      if (s_pending_reminder_tz.active || s_pending_reminder_details.active) {
        clear_pending_reminder_tz();
        clear_pending_reminder_details();
        out = "OK: pending reminder flow canceled";
        return true;
      }
      out = "OK: no pending action";
      return true;
    }
    clear_pending();
    clear_pending_reminder_tz();
    clear_pending_reminder_details();
    out = "OK: pending action canceled";
    return true;
  }

  if (cmd_lc == "confirm" || cmd_lc.startsWith("confirm ")) {
    if (!s_pending.active) {
      out = "ERR: no pending action";
      return true;
    }
    if (is_expired(s_pending.expires_ms)) {
      clear_pending();
      out = "ERR: pending action expired";
      return true;
    }

    if (cmd_lc.startsWith("confirm ")) {
      const String tail = cmd_lc.substring(8);
      int user_id = -1;
      if (!parse_one_int(tail, "%d", &user_id)) {
        out = "ERR: usage confirm [id]";
        return true;
      }
      if ((unsigned long)user_id != s_pending.id) {
        out = "ERR: confirm id mismatch";
        return true;
      }
    }

    const int pin = s_pending.pin;
    const int state = s_pending.state;
    const int led_count = s_pending.led_count;
    const PendingActionType type = s_pending.type;
    const unsigned long id = s_pending.id;
    if (is_safe_mode_enabled() &&
        (type == PENDING_RELAY_SET || type == PENDING_LED_FLASH)) {
      clear_pending();
      out = "ERR: safe mode ON. Disable with safe_mode_off first";
      return true;
    }
    clear_pending();
    if (type == PENDING_RELAY_SET) {
      relay_set_now(pin, state, out);
    } else if (type == PENDING_LED_FLASH) {
      flash_led_now(led_count, out);
    } else {
      out = "ERR: unknown pending action";
      return true;
    }
    out += " (confirmed id=" + String(id) + ")";
    return true;
  }

  const int led_flash_count = parse_led_flash_count(cmd_lc);
  if (led_flash_count != 0) {
    if (is_safe_mode_enabled()) {
      out = "ERR: safe mode ON. flash_led blocked";
      return true;
    }
    if (led_flash_count < 1 || led_flash_count > 20) {
      out = "ERR: usage flash_led [1-20]";
      return true;
    }

    if (s_pending.active) {
      out = "ERR: pending action exists (id=" + String(s_pending.id) + "). confirm/cancel first";
      return true;
    }

    s_pending.active = true;
    s_pending.id = s_next_pending_id++;
    s_pending.type = PENDING_LED_FLASH;
    s_pending.led_count = led_flash_count;
    s_pending.expires_ms = millis() + ACTION_CONFIRM_TIMEOUT_MS;

    out = "CONFIRM flash_led " + String(led_flash_count) +
          "\nRun: confirm " + String(s_pending.id) +
          "\nOr: cancel";
    return true;
  }

  if (cmd_lc.startsWith("relay_set ")) {
    if (is_safe_mode_enabled()) {
      out = "ERR: safe mode ON. relay_set blocked";
      return true;
    }
    int pin = -1;
    int state = -1;
    if (parse_two_ints(cmd_lc, "relay_set %d %d", &pin, &state)) {
      if (pin >= 0 && pin <= 39 && (state == 0 || state == 1)) {
        if (s_pending.active) {
          out = "ERR: pending action exists (id=" + String(s_pending.id) + "). confirm/cancel first";
          return true;
        }

        s_pending.active = true;
        s_pending.id = s_next_pending_id++;
        s_pending.type = PENDING_RELAY_SET;
        s_pending.pin = pin;
        s_pending.state = state;
        s_pending.expires_ms = millis() + ACTION_CONFIRM_TIMEOUT_MS;
        out = "CONFIRM relay_set pin " + String(pin) + " -> " + String(state) +
              "\nRun: confirm " + String(s_pending.id) +
              "\nOr: cancel";
        return true;
      }
    }
    out = "ERR: usage relay_set <pin> <0|1>";
    return true;
  }

  if (cmd_lc.startsWith("sensor_read ")) {
    int pin = -1;
    if (parse_one_int(cmd_lc, "sensor_read %d", &pin)) {
      if (pin >= 0 && pin <= 39) {
        pinMode(pin, INPUT);
        int v = digitalRead(pin);
        out = "OK: sensor pin " + String(pin) + " = " + String(v);
        return true;
      }
    }
    out = "ERR: usage sensor_read <pin>";
    return true;
  }

  if (cmd_lc == "plan" || cmd_lc.startsWith("plan ")) {
    String task = cmd.length() > 4 ? cmd.substring(4) : "";
    task.trim();
    if (task.length() == 0) {
      out = "ERR: usage plan <what to build>";
      return true;
    }

    String plan_text;
    String plan_error;
    if (!llm_generate_plan(task, plan_text, plan_error)) {
      out = "ERR: " + plan_error;
      return true;
    }

    if (plan_text.length() > 1400) {
      plan_text = plan_text.substring(0, 1400) + "...";
    }
    out = plan_text;
    return true;
  }

  if (cmd_lc == "memory") {
    String notes;
    String err;
    if (!memory_get_notes(notes, err)) {
      out = "ERR: " + err;
      return true;
    }
    notes.trim();
    if (notes.length() == 0) {
      out = "Memory is empty";
      return true;
    }
    if (notes.length() > 1400) {
      notes = notes.substring(notes.length() - 1400);
    }
    out = "Memory:\n" + notes;
    return true;
  }

  if (cmd_lc == "forget" || cmd_lc == "memory_clear") {
    String err;
    if (!memory_clear_notes(err)) {
      out = "ERR: " + err;
      return true;
    }
    out = "OK: memory cleared";
    return true;
  }

  if (cmd_lc == "remember" || cmd_lc.startsWith("remember ")) {
    String note = cmd.length() > 8 ? cmd.substring(8) : "";
    note.trim();
    if (note.length() == 0) {
      out = "ERR: usage remember <note>";
      return true;
    }
    String err;
    if (!memory_append_note(note, err)) {
      out = "ERR: " + err;
      return true;
    }
    out = "OK: remembered";
    return true;
  }

  if (cmd_lc == "generate_image" || cmd_lc.startsWith("generate_image ")) {
    String prompt = "";
    if (cmd_lc.startsWith("generate_image ")) {
      prompt = cmd.substring(14);
      prompt.trim();
    }
    if (prompt.length() == 0) {
      out = "ERR: usage generate_image <prompt>";
      return true;
    }

    String base64_image;
    String llm_error;
    if (!llm_generate_image(prompt, base64_image, llm_error)) {
      out = "ERR: " + llm_error;
      return true;
    }

    if (!transport_telegram_send_photo_base64(base64_image, "")) {
      out = "ERR: failed to send photo";
      return true;
    }

    out = "Image generated and sent";
    return true;
  }

  if (cmd_lc.indexOf("summarize") >= 0 || cmd_lc.indexOf("analyse") >= 0 ||
      cmd_lc.indexOf("analyze") >= 0 || cmd_lc.indexOf("describe") >= 0 ||
      cmd_lc.indexOf("explain") >= 0 || cmd_lc.indexOf("read this") >= 0 ||
      cmd_lc.startsWith("what is in") || cmd_lc.startsWith("what's in")) {
    
    // Check for document first (PDFs etc)
    String doc_name, doc_mime, doc_b64, doc_err;
    if (transport_telegram_get_last_document_base64(doc_name, doc_mime, doc_b64, doc_err)) {
      String reply, llm_err;
      out = "Analyzing document: " + doc_name + "...";
      if (llm_understand_media(cmd, doc_mime, doc_b64, reply, llm_err)) {
        out = "Document Analysis (" + doc_name + "):\n" + reply;
        return true;
      }
      out = "ERR: " + llm_err;
      return true;
    }

    // Check for photo second
    String photo_mime, photo_b64, photo_err;
    if (transport_telegram_get_last_photo_base64(photo_mime, photo_b64, photo_err)) {
      String reply, llm_err;
      out = "Analyzing photo...";
      if (llm_understand_media(cmd, photo_mime, photo_b64, reply, llm_err)) {
        out = "Photo Analysis:\n" + reply;
        return true;
      }
      out = "ERR: " + llm_err;
      return true;
    }
    
    // Fall through if no media found (might be normal text chat)
  }

  // Model management commands
  if (cmd_lc == "model list" || cmd_lc == "model_list") {
    String configured = model_config_get_configured_list();
    out = "Configured providers:\n" + configured +
          "\n\nUse: model use <provider> to switch";
    return true;
  }

  if (cmd_lc == "model status" || cmd_lc == "model_status") {
    out = model_config_get_status_summary();
    return true;
  }

  if (cmd_lc.startsWith("model use ") || cmd_lc.startsWith("model_use ")) {
    String provider = cmd.length() > 9 ? cmd.substring(9) : "";
    provider.trim();
    if (provider.length() == 0) {
      out = "ERR: usage model use <provider>\nProviders: openai, anthropic, gemini, glm";
      return true;
    }
    if (!model_config_is_provider_configured(provider)) {
      out = "ERR: provider '" + provider + "' not configured.\n"
            "Use: model set " + provider + " <your_api_key>";
      return true;
    }
    String err;
    if (!model_config_set_active_provider(provider, err)) {
      out = "ERR: " + err;
      return true;
    }
    String model = model_config_get_model(provider);
    out = "OK: switched to " + provider + " (" + model + ")";
    return true;
  }

  if (cmd_lc.startsWith("model set ") || cmd_lc.startsWith("model_set ")) {
    String tail = cmd.length() > 9 ? cmd.substring(9) : "";
    tail.trim();
    if (tail.length() == 0) {
      out = "ERR: usage model set <provider> <api_key>\nProviders: openai, anthropic, gemini, glm";
      return true;
    }
    // Find first space to separate provider and key
    int first_space = tail.indexOf(' ');
    if (first_space < 0) {
      out = "ERR: usage model set <provider> <api_key>";
      return true;
    }
    String provider = tail.substring(0, first_space);
    String api_key = tail.substring(first_space + 1);
    provider.trim();
    api_key.trim();
    if (api_key.length() == 0) {
      out = "ERR: API key cannot be empty";
      return true;
    }
    String err;
    if (!model_config_set_api_key(provider, api_key, err)) {
      out = "ERR: " + err;
      return true;
    }
    out = "OK: API key saved for " + provider +
          "\nUse: model use " + provider + " to activate";
    return true;
  }

  if (cmd_lc.startsWith("model clear ") || cmd_lc.startsWith("model_clear ")) {
    String provider = cmd.length() > 11 ? cmd.substring(11) : "";
    provider.trim();
    if (provider.length() == 0) {
      out = "ERR: usage model clear <provider>";
      return true;
    }
    String err;
    if (!model_config_clear_provider(provider, err)) {
      out = "ERR: " + err;
      return true;
    }
    out = "OK: configuration cleared for " + provider;
    return true;
  }

  return false;
}
