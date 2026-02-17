#include "agent_loop.h"

#include <Arduino.h>

#include "scheduler.h"
#include "chat_history.h"
#include "memory_store.h"
#include "llm_client.h"
#include "model_config.h"
#include "persona_store.h"
#include "event_log.h"
#include "status_led.h"
#include "task_store.h"
#include "tool_registry.h"
#include "transport_telegram.h"

// Store last LLM response for emailing code
static String s_last_llm_response = "";

String agent_loop_get_last_response() {
  return s_last_llm_response;
}

void agent_loop_set_last_response(const String &response) {
  s_last_llm_response = response;
}

static bool is_internal_dispatch_message(const String &msg) {
  String lc = msg;
  lc.trim();
  lc.toLowerCase();
  return lc == "heartbeat_run" || lc == "reminder_run";
}

static bool should_try_route(const String &msg) {
  String lc = msg;
  lc.trim();
  lc.toLowerCase();
  if (lc.length() == 0 || lc.startsWith("/")) {
    return false;
  }

  const char *prefixes[] = {
      "set ",       "show ",      "list ",      "add ",       "delete ",    "clear ",
      "turn ",      "switch ",    "enable ",    "disable ",   "remind ",    "schedule ",
      "search ",    "look up ",   "find ",      "google ",    "status",     "health",
      "logs",       "time",       "timezone",   "task ",      "memory",     "remember ",
      "forget",     "flash ",     "blink ",     "led ",       "sensor ",    "relay ",
      "safe mode",  "email ",     "plan ",      "confirm",    "cancel",      "create ",
      "build ",     "make ",      "generate ",
  };

  for (size_t i = 0; i < (sizeof(prefixes) / sizeof(prefixes[0])); i++) {
    String p = String(prefixes[i]);
    if (lc == p || lc.startsWith(p)) {
      return true;
    }
  }

  if (lc.indexOf("every day") >= 0 || lc.indexOf("everyday") >= 0 || lc.indexOf("daily") >= 0 ||
      lc.indexOf("at ") >= 0 || lc.indexOf("reminder") >= 0 || lc.indexOf("web search") >= 0) {
    return true;
  }

  return false;
}

static void record_chat_turn(const String &incoming, const String &outgoing) {
  if (is_internal_dispatch_message(incoming)) {
    return;
  }
  String err;
  chat_history_append('U', incoming, err);
  chat_history_append('A', outgoing, err);
}

static void send_and_record(const String &incoming, const String &outgoing) {
  event_log_append("OUT: " + outgoing);
  transport_telegram_send(outgoing);
  record_chat_turn(incoming, outgoing);
  if (outgoing.startsWith("ERR:")) {
    status_led_notify_error();
  }
}

static void on_incoming_message(const String &msg) {
  if (msg.length() == 0) {
    return;
  }

  class BusyScope {
   public:
    BusyScope() { status_led_set_busy(true); }
    ~BusyScope() { status_led_set_busy(false); }
  } busy_scope;

  Serial.print("[agent] incoming: ");
  Serial.println(msg);
  event_log_append("IN: " + msg);

  String response;
  if (!tool_registry_execute(msg, response)) {
    String trimmed = msg;
    trimmed.trim();

    if (trimmed.startsWith("/")) {
      send_and_record(msg, "Denied or unknown command");
      return;
    }

    if (should_try_route(trimmed)) {
      String routed_command;
      String route_err;
      if (llm_route_tool_command(trimmed, routed_command, route_err)) {
        routed_command.trim();
        if (routed_command.length() > 0) {
          String routed_response;
          if (tool_registry_execute(routed_command, routed_response)) {
            if (routed_response.length() > 1400) {
              routed_response = routed_response.substring(0, 1400) + "...";
            }
            event_log_append("ROUTE: " + routed_command);
            send_and_record(msg, routed_response);
            return;
          }
        }
      }
    }

    String err;
    if (!llm_generate_reply(trimmed, response, err)) {
      send_and_record(msg, "ERR: " + err);
      return;
    }

    // Store the full response for potential email_code command
    s_last_llm_response = response;

    if (response.length() > 1400) {
      response = response.substring(0, 1400) + "...";
    }
    send_and_record(msg, response);
    return;
  }

  send_and_record(msg, response);
}

void agent_loop_init() {
  event_log_init();
  chat_history_init();
  memory_init();
  model_config_init();
  persona_init();
  task_store_init();
  tool_registry_init();
  status_led_init();
  scheduler_init();
  transport_telegram_init();
  Serial.println("[agent] init complete");

  // Check for firmware updates after 30 seconds (gives WiFi time to stabilize)
  // Only check if GITHUB_REPO is configured
  #ifdef GITHUB_REPO
    delay(30000);
    Serial.println("[agent] checking for firmware updates...");
    tool_registry_check_updates_async();
  #endif
}

void agent_loop_tick() {
  status_led_tick();
  transport_telegram_poll(on_incoming_message);
  scheduler_tick(on_incoming_message);
}
