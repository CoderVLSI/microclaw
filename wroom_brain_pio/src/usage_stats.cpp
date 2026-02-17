#include "usage_stats.h"

#include <Arduino.h>
#include <Preferences.h>
#include "event_log.h"

namespace {

// NVS namespace for usage stats
static const char *kNvsNamespace = "usage";

// Stats stored in NVS
struct UsageStats {
  uint32_t total_calls;
  uint32_t successful_calls;
  uint32_t failed_calls;
  uint32_t rate_limited;      // HTTP 429
  uint32_t last_call_time;    // Unix timestamp
  char last_provider[32];
  char last_model[64];
  char last_call_type[32];    // chat, image, route, etc.
  uint32_t chat_calls;
  uint32_t image_calls;
  uint32_t route_calls;
  uint32_t media_calls;
  uint32_t other_calls;
};

static UsageStats s_stats = {};
static bool s_loaded = false;

void load_stats() {
  if (s_loaded) {
    return;
  }

  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, true)) {
    // No saved stats, initialize to zero
    memset(&s_stats, 0, sizeof(s_stats));
    s_loaded = true;
    return;
  }

  s_stats.total_calls = prefs.getUInt("total", 0);
  s_stats.successful_calls = prefs.getUInt("success", 0);
  s_stats.failed_calls = prefs.getUInt("failed", 0);
  s_stats.rate_limited = prefs.getUInt("rate_limited", 0);
  s_stats.last_call_time = prefs.getUInt("last_time", 0);

  prefs.getString("last_provider", s_stats.last_provider, sizeof(s_stats.last_provider));
  prefs.getString("last_model", s_stats.last_model, sizeof(s_stats.last_model));
  prefs.getString("last_type", s_stats.last_call_type, sizeof(s_stats.last_call_type));

  s_stats.chat_calls = prefs.getUInt("chat", 0);
  s_stats.image_calls = prefs.getUInt("image", 0);
  s_stats.route_calls = prefs.getUInt("route", 0);
  s_stats.media_calls = prefs.getUInt("media", 0);
  s_stats.other_calls = prefs.getUInt("other", 0);

  prefs.end();
  s_loaded = true;
}

void save_stats() {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, false)) {
    event_log_append("USAGE: failed to save stats");
    return;
  }

  prefs.putUInt("total", s_stats.total_calls);
  prefs.putUInt("success", s_stats.successful_calls);
  prefs.putUInt("failed", s_stats.failed_calls);
  prefs.putUInt("rate_limited", s_stats.rate_limited);
  prefs.putUInt("last_time", s_stats.last_call_time);

  prefs.putString("last_provider", s_stats.last_provider);
  prefs.putString("last_model", s_stats.last_model);
  prefs.putString("last_type", s_stats.last_call_type);

  prefs.putUInt("chat", s_stats.chat_calls);
  prefs.putUInt("image", s_stats.image_calls);
  prefs.putUInt("route", s_stats.route_calls);
  prefs.putUInt("media", s_stats.media_calls);
  prefs.putUInt("other", s_stats.other_calls);

  prefs.end();
}

}  // namespace

void usage_init() {
  load_stats();
}

void usage_record_call(const char *call_type, int http_status, const char *provider, const char *model) {
  load_stats();

  s_stats.total_calls++;

  // Track call type
  if (strcmp(call_type, "chat") == 0) {
    s_stats.chat_calls++;
  } else if (strcmp(call_type, "image") == 0) {
    s_stats.image_calls++;
  } else if (strcmp(call_type, "route") == 0) {
    s_stats.route_calls++;
  } else if (strcmp(call_type, "media") == 0) {
    s_stats.media_calls++;
  } else {
    s_stats.other_calls++;
  }

  // Track success/failure
  if (http_status >= 200 && http_status < 300) {
    s_stats.successful_calls++;
  } else {
    s_stats.failed_calls++;
    if (http_status == 429) {
      s_stats.rate_limited++;
    }
  }

  // Update last call info
  s_stats.last_call_time = millis() / 1000;  // Approximate uptime-based timestamp
  if (provider != nullptr && provider[0] != '\0') {
    strncpy(s_stats.last_provider, provider, sizeof(s_stats.last_provider) - 1);
    s_stats.last_provider[sizeof(s_stats.last_provider) - 1] = '\0';
  }
  if (model != nullptr && model[0] != '\0') {
    strncpy(s_stats.last_model, model, sizeof(s_stats.last_model) - 1);
    s_stats.last_model[sizeof(s_stats.last_model) - 1] = '\0';
  }
  if (call_type != nullptr && call_type[0] != '\0') {
    strncpy(s_stats.last_call_type, call_type, sizeof(s_stats.last_call_type) - 1);
    s_stats.last_call_type[sizeof(s_stats.last_call_type) - 1] = '\0';
  }

  save_stats();
}

void usage_record_error(int http_status) {
  load_stats();
  if (http_status == 429) {
    s_stats.rate_limited++;
  }
  save_stats();
}

void usage_get_report(String &out) {
  load_stats();

  out = "📊 Usage Statistics\n\n";

  // Call summary
  out += "Calls:\n";
  out += "  Total: " + String(s_stats.total_calls) + "\n";
  out += "  Success: " + String(s_stats.successful_calls) + "\n";
  out += "  Failed: " + String(s_stats.failed_calls) + "\n";

  if (s_stats.rate_limited > 0) {
    out += "  ⚠️ Rate limited (429): " + String(s_stats.rate_limited) + "\n";
  }

  // Call breakdown
  out += "\nBy type:\n";
  if (s_stats.chat_calls > 0) {
    out += "  Chat: " + String(s_stats.chat_calls) + "\n";
  }
  if (s_stats.image_calls > 0) {
    out += "  Image: " + String(s_stats.image_calls) + "\n";
  }
  if (s_stats.route_calls > 0) {
    out += "  Route: " + String(s_stats.route_calls) + "\n";
  }
  if (s_stats.media_calls > 0) {
    out += "  Media: " + String(s_stats.media_calls) + "\n";
  }
  if (s_stats.other_calls > 0) {
    out += "  Other: " + String(s_stats.other_calls) + "\n";
  }

  // Last call info
  if (s_stats.last_provider[0] != '\0') {
    out += "\nLast call:\n";
    out += "  Type: " + String(s_stats.last_call_type) + "\n";
    out += "  Provider: " + String(s_stats.last_provider) + "\n";
    if (s_stats.last_model[0] != '\0') {
      out += "  Model: " + String(s_stats.last_model) + "\n";
    }
  }

  // Success rate
  if (s_stats.total_calls > 0) {
    float success_rate = (s_stats.successful_calls * 100.0f) / s_stats.total_calls;
    out += "\nSuccess rate: " + String((int)success_rate) + "%\n";
  }
}

void usage_reset() {
  memset(&s_stats, 0, sizeof(s_stats));
  save_stats();
  event_log_append("USAGE: stats reset");
}
