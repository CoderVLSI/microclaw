#include "llm_client.h"

#include "agent_loop.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "brain_config.h"
#include "chat_history.h"
#include "memory_store.h"
#include "file_memory.h"
#include "model_config.h"
#include "persona_store.h"
#include "usage_stats.h"
#include "skill_registry.h"
#include "scheduler.h"
#include "cron_store.h"
#include <time.h>

namespace {

static const char *kPlanSystemPrompt =
    "You are a coding planner. Return a concise implementation plan only. "
    "Use numbered steps. Include risks and quick validation checks.";
static const char *kChatSystemPrompt =
    "You are Timi, a clever dinosaur assistant running on an ESP32 microcontroller, "
    "communicating via Telegram. Be helpful, warm, and concise.\n\n"
    "YOUR CAPABILITIES (use these proactively when relevant):\n"
    "🧠 Memory: remember <note>, memory_read, memory_clear, user_read\n"
    "📋 Tasks: task_add, task_list, task_done, task_clear\n"
    "⏰ Scheduling: cron_add <expr>|<cmd>, cron_list, reminder_set_daily <HH:MM> <msg>\n"
    "🔍 Web Search: search <query> (Tavily/Brave)\n"
    "🌤 Weather: weather <location>\n"
    "🎨 Image Gen: generate_image <prompt>\n"
    "📸 Media: Analyze photos/documents sent to you (auto-triggered)\n"
    "🌐 Web Gen: web_files_make <topic> - Create full websites (HTML/CSS/JS)\n"
    "📧 Email: send_email <to> <subject> <body>, email_draft\n"
    "🧩 Skills: use_skill <name> [context], skill_list, skill_add <name> <desc>: <instructions>\n"
    "   - You can CREATE new skills on the fly when you identify a repeatable pattern\n"
    "   - Skills are stored on SPIFFS and persist across reboots\n"
    "👤 Personality: soul_show, soul_set <personality>\n"
    "⚙️ System: status, health, specs, usage, model_list, model_use\n"
    "🔄 Updates: update (check for firmware updates from GitHub)\n\n"
    "BEHAVIOR:\n"
    "- Greet warmly based on time of day\n"
    "- Reference your memory to personalize conversations\n"
    "- Suggest relevant tools proactively (e.g. if user mentions weather, offer to check)\n"
    "- If a task seems complex or multi-step, use the ReAct agent (it triggers automatically)\n"
    "- If you notice a repeatable workflow, offer to save it as a skill\n"
    "- If you generate HTML/website code, deploy it: host_file <filename> <content>";
static const char *kHeartbeatSystemPrompt =
    "You are running an autonomous heartbeat check for an ESP32 Telegram agent. "
    "Read the heartbeat instructions and return a short operational update in 3 bullets: "
    "health, risk, next action.";
static const char *kRouteSystemPrompt =
    "Route user text to one tool command if obvious.\n"
    "Tools:\n"
    "- search <query>: Web search (Brave/Tavily)\n"
    "- weather <location>: Get weather\n"
    "- time: Get current time\n"
    "- generate_image <prompt>: Create image\n"
    "- web_files_make <topic>: Create website\n"
    "Return exactly one line only: TOOL: <command> or NONE. No markdown.";

struct HttpResult {
  int status_code;
  String body;
  String error;
};

} // namespace

// Build a compact time-awareness context for the LLM
String build_time_context() {
  struct tm timeinfo;
  if (!scheduler_get_local_time(timeinfo)) {
    return "";  // NTP not synced yet
  }

  // Day of week
  const char* days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  const char* day_name = days[timeinfo.tm_wday];

  // Time of day period
  const char* period;
  int hour = timeinfo.tm_hour;
  if (hour >= 5 && hour < 12) {
    period = "morning";
  } else if (hour >= 12 && hour < 17) {
    period = "afternoon";
  } else if (hour >= 17 && hour < 21) {
    period = "evening";
  } else {
    period = "night";
  }

  // Format: "It is Wednesday afternoon, 14:32 (Feb 19, 2026)"
  char buf[80];
  strftime(buf, sizeof(buf), "%H:%M", &timeinfo);
  String time_str = String(buf);
  strftime(buf, sizeof(buf), "%b %d, %Y", &timeinfo);
  String date_str = String(buf);

  return "It is " + String(day_name) + " " + String(period) + ", " +
         time_str + " (" + date_str + ")";
}

String build_schedule_context() {
  String out = "";

  CronJob jobs[CRON_MAX_JOBS];
  const int cron_count = cron_store_get_all(jobs, CRON_MAX_JOBS);
  if (cron_count <= 0) {
    out += "Cron jobs: none\n";
  } else {
    out += "Cron jobs (" + String(cron_count) + "):\n";
    for (int i = 0; i < cron_count; i++) {
      out += "- " + cron_job_to_string(jobs[i]) + "\n";
    }
  }

  String hhmm;
  String msg;
  String err;
  if (persona_get_daily_reminder(hhmm, msg, err)) {
    hhmm.trim();
    msg.trim();
    if (hhmm.length() > 0 && msg.length() > 0) {
      const String webjob_prefix = "__WEBJOB__:";
      if (msg.startsWith(webjob_prefix)) {
        String task = msg.substring(webjob_prefix.length());
        task.trim();
        out += "Daily schedule: " + hhmm + " (webjob) " + task;
      } else {
        out += "Daily schedule: " + hhmm + " (reminder) " + msg;
      }
    } else {
      out += "Daily schedule: none";
    }
  } else {
    out += "Daily schedule: unknown";
  }

  return out;
}

namespace {

String to_lower(String value) {
  value.toLowerCase();
  return value;
}

bool contains_ci(const String &text, const char *needle_lower) {
  String hay = text;
  hay.toLowerCase();
  return hay.indexOf(needle_lower) >= 0;
}

String join_url(const String &base, const String &path) {
  if (base.endsWith("/") && path.startsWith("/")) {
    return base.substring(0, base.length() - 1) + path;
  }
  if (!base.endsWith("/") && !path.startsWith("/")) {
    return base + "/" + path;
  }
  return base + path;
}

String trim_with_ellipsis(const String &value, size_t max_chars) {
  if (value.length() <= max_chars) {
    return value;
  }
  if (max_chars < 16) {
    return value.substring(0, max_chars);
  }
  return value.substring(0, max_chars) + "\n...(truncated)";
}

String keep_tail_with_marker(const String &value, size_t max_chars) {
  if (value.length() <= max_chars) {
    return value;
  }
  if (max_chars < 16) {
    return value.substring(value.length() - max_chars);
  }
  return String("...(truncated)\n") + value.substring(value.length() - max_chars);
}

String json_escape(const String &src) {
  String out;
  out.reserve(src.length() + 32);
  for (size_t i = 0; i < src.length(); i++) {
    const char c = src[i];
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if ((unsigned char)c < 0x20) {
          out += ' ';
        } else {
          out += c;
        }
        break;
    }
  }
  return out;
}

static bool is_json_ws(const char c) {
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

bool extract_json_string_field(const String &body, const char *field_name, String &out) {
  const String key = String("\"") + field_name + "\"";
  int search_from = 0;

  while (true) {
    const int key_pos = body.indexOf(key, search_from);
    if (key_pos < 0) {
      return false;
    }

    int i = key_pos + (int)key.length();
    while (i < (int)body.length() && is_json_ws(body[i])) {
      i++;
    }
    if (i >= (int)body.length() || body[i] != ':') {
      search_from = key_pos + 1;
      continue;
    }

    i++;
    while (i < (int)body.length() && is_json_ws(body[i])) {
      i++;
    }
    if (i >= (int)body.length() || body[i] != '"') {
      search_from = key_pos + 1;
      continue;
    }
    i++;

    String text;
    text.reserve(256);
    bool esc = false;
    for (; i < (int)body.length(); i++) {
      const char c = body[i];

      if (esc) {
        switch (c) {
          case 'n':
            text += '\n';
            break;
          case 'r':
            text += '\r';
            break;
          case 't':
            text += '\t';
            break;
          case '\\':
            text += '\\';
            break;
          case '"':
            text += '"';
            break;
          default:
            text += c;
            break;
        }
        esc = false;
        continue;
      }

      if (c == '\\') {
        esc = true;
        continue;
      }

      if (c == '"') {
        out = text;
        return true;
      }

      text += c;
    }

    return false;
  }
}

HttpResult http_post_json(const String &url, const String &body,
                          const String &h1_name = "", const String &h1_value = "",
                          const String &h2_name = "", const String &h2_value = "",
                          const String &h3_name = "", const String &h3_value = "") {
  HttpResult result{};
  result.status_code = -1;

  if (WiFi.status() != WL_CONNECTED) {
    result.error = "WiFi not connected";
    return result;
  }

  const int kMaxAttempts = 2;
  for (int attempt = 0; attempt < kMaxAttempts; attempt++) {
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient https;
    if (!https.begin(client, url)) {
      result.error = "HTTP begin failed";
      if (attempt + 1 < kMaxAttempts) {
        delay(220);
        continue;
      }
      return result;
    }

    https.setConnectTimeout(12000);
    https.setTimeout(LLM_TIMEOUT_MS);
    https.addHeader("Content-Type", "application/json");

    if (h1_name.length()) {
      https.addHeader(h1_name, h1_value);
    }
    if (h2_name.length()) {
      https.addHeader(h2_name, h2_value);
    }
    if (h3_name.length()) {
      https.addHeader(h3_name, h3_value);
    }

    result.status_code = https.POST((uint8_t *)body.c_str(), body.length());
    if (result.status_code > 0) {
      result.body = https.getString();
      result.error = "";
      https.end();
      return result;
    }

    result.error = https.errorToString(result.status_code);
    https.end();

    if (attempt + 1 < kMaxAttempts) {
      delay(260 + (attempt * 120));
    }
  }

  return result;
}

bool parse_response_text(const String &body, String &text) {
  // OpenAI responses API compatibility fallback
  if (extract_json_string_field(body, "output_text", text)) {
    return true;
  }

  // OpenAI/GLM chat-completions style
  if (extract_json_string_field(body, "content", text)) {
    return true;
  }

  // Anthropic and Gemini both include "text" fields
  if (extract_json_string_field(body, "text", text)) {
    return true;
  }

  return false;
}

bool extract_json_string_field_after_anchor(const String &body, const char *anchor,
                                            const char *field_name, String &out) {
  const int anchor_pos = body.indexOf(anchor);
  if (anchor_pos < 0) {
    return false;
  }
  return extract_json_string_field(body.substring(anchor_pos), field_name, out);
}

String summarize_http_error(const String &label, const HttpResult &res) {
  if (res.status_code <= 0) {
    if (res.error.length() > 0) {
      return label + " network error: " + res.error;
    }
    return label + " request failed";
  }

  String msg;
  if (extract_json_string_field(res.body, "message", msg) && msg.length() > 0) {
    msg.replace('\n', ' ');
    msg.replace('\r', ' ');
    if (msg.length() > 160) {
      msg = msg.substring(0, 160);
    }
    String suffix = "";
    if (res.status_code == 429 || contains_ci(msg, "quota")) {
      suffix = " (quota/rate limit)";
    } else if (contains_ci(msg, "billed users") || contains_ci(msg, "billing")) {
      suffix = " (billing required)";
    } else if (res.status_code == 404 || contains_ci(msg, "not found")) {
      suffix = " (model unavailable)";
    }
    return label + " HTTP " + String(res.status_code) + ": " + msg + suffix;
  }

  if (res.status_code == 429) {
    return label + " HTTP 429 (quota/rate limit)";
  }
  if (res.status_code == 404) {
    return label + " HTTP 404 (model unavailable)";
  }
  return label + " HTTP " + String(res.status_code);
}

bool call_openai_like(const String &base_url, const String &api_key, const String &model,
                      const String &system_prompt, const String &task,
                      String &response_out, String &error_out) {
  const String url = join_url(base_url, "/v1/chat/completions");
  const String body = String("{\"model\":\"") + json_escape(model) +
                      "\",\"messages\":[{\"role\":\"system\",\"content\":\"" +
                      json_escape(system_prompt) + "\"},{\"role\":\"user\",\"content\":\"" +
                      json_escape(task) + "\"}],\"temperature\":0.2}";

  const HttpResult res =
      http_post_json(url, body, "Authorization", "Bearer " + api_key);
  if (res.status_code < 200 || res.status_code >= 300) {
    error_out = summarize_http_error("LLM", res);
    return false;
  }

  if (!parse_response_text(res.body, response_out)) {
    error_out = "Could not parse provider response";
    return false;
  }

  return true;
}

bool call_anthropic(const String &base_url, const String &api_key, const String &model,
                    const String &system_prompt, const String &task,
                    String &response_out, String &error_out) {
  const String url = join_url(base_url, "/v1/messages");
  const String body = String("{\"model\":\"") + json_escape(model) +
                      "\",\"max_tokens\":512,\"system\":\"" + json_escape(system_prompt) +
                      "\",\"messages\":[{\"role\":\"user\",\"content\":\"" +
                      json_escape(task) + "\"}]}";

  const HttpResult res = http_post_json(url, body, "x-api-key", api_key,
                                        "anthropic-version", "2023-06-01");
  if (res.status_code < 200 || res.status_code >= 300) {
    error_out = summarize_http_error("LLM", res);
    return false;
  }

  if (!parse_response_text(res.body, response_out)) {
    error_out = "Could not parse provider response";
    return false;
  }

  return true;
}

bool call_gemini(const String &base_url, const String &api_key, const String &model,
                 const String &system_prompt, const String &task,
                 String &response_out, String &error_out) {
  const String path = String("/v1beta/models/") + model + ":generateContent?key=" + api_key;
  const String url = join_url(base_url, path);
  const String prompt = system_prompt + String("\\n\\nUser message:\\n") + task;
  const String body = String("{\"contents\":[{\"parts\":[{\"text\":\"") +
                      json_escape(prompt) + "\"}]}]}";

  const HttpResult res = http_post_json(url, body);
  if (res.status_code < 200 || res.status_code >= 300) {
    error_out = summarize_http_error("LLM", res);
    return false;
  }

  if (!parse_response_text(res.body, response_out)) {
    error_out = "Could not parse provider response";
    return false;
  }

  return true;
}

bool call_glm_zai(const String &endpoint_url, const String &api_key, const String &model,
                  const String &system_prompt, const String &task,
                  String &response_out, String &error_out) {
  String url = endpoint_url;
  if (!to_lower(url).endsWith("/chat/completions")) {
    url = join_url(url, "/chat/completions");
  }

  const String body = String("{\"model\":\"") + json_escape(model) +
                      "\",\"messages\":[{\"role\":\"system\",\"content\":\"" +
                      json_escape(system_prompt) + "\"},{\"role\":\"user\",\"content\":\"" +
                      json_escape(task) + "\"}],\"temperature\":0.2,\"stream\":false}";

  const HttpResult res =
      http_post_json(url, body, "Authorization", "Bearer " + api_key);
  if (res.status_code < 200 || res.status_code >= 300) {
    error_out = summarize_http_error("LLM", res);
    return false;
  }

  if (!parse_response_text(res.body, response_out)) {
    error_out = "Could not parse provider response";
    return false;
  }

  return true;
}

bool call_ollama(const String &base_url, const String &model,
                 const String &system_prompt, const String &task,
                 String &response_out, String &error_out) {
  // Ollama uses /api/chat or /api/generate endpoint
  String url = base_url;
  if (!url.endsWith("/api/chat") && !url.endsWith("/api/generate")) {
    // Prefer /api/chat for OpenAI-compatible format
    url = join_url(base_url, "/api/chat");
  }

  // Ollama /api/chat uses OpenAI-compatible format
  const String body = String("{\"model\":\"") + json_escape(model) +
                      "\",\"messages\":[{\"role\":\"system\",\"content\":\"" +
                      json_escape(system_prompt) + "\"},{\"role\":\"user\",\"content\":\"" +
                      json_escape(task) + "\"}],\"stream\":false}";

  // Ollama doesn't use API key, pass empty string
  const HttpResult res = http_post_json(url, body);
  if (res.status_code < 200 || res.status_code >= 300) {
    error_out = summarize_http_error("Ollama", res);
    return false;
  }

  // Ollama /api/chat returns OpenAI-compatible format
  if (!parse_response_text(res.body, response_out)) {
    error_out = "Could not parse Ollama response";
    return false;
  }

  return true;
}

// Check if an error indicates quota/rate limit (should trigger fallback)
static bool is_quota_error(const String &error) {
  String lc = error;
  lc.toLowerCase();
  return (lc.indexOf("http 429") >= 0) ||
         (lc.indexOf("quota") >= 0) ||
         (lc.indexOf("rate limit") >= 0) ||
         (lc.indexOf("billing") >= 0) ||
         (lc.indexOf("limit exceeded") >= 0);
}

// Try to call a specific provider by name
static bool try_provider(const String &provider, const String &model,
                        const String &system_prompt, const String &task,
                        String &response_out, String &error_out) {
  String prov = to_lower(provider);

  if (prov == "openai") {
    String mod = model.length() > 0 ? model : String("gpt-4.1-mini");
    String baseUrl = String(LLM_OPENAI_BASE_URL);
    return call_openai_like(baseUrl, "", mod, system_prompt, task, response_out, error_out);
  }

  if (prov == "anthropic") {
    String mod = model.length() > 0 ? model : String("claude-3-5-sonnet-latest");
    String baseUrl = String(LLM_ANTHROPIC_BASE_URL);
    return call_anthropic(baseUrl, "", mod, system_prompt, task, response_out, error_out);
  }

  if (prov == "gemini") {
    String mod = model.length() > 0 ? model : String("gemini-2.0-flash");
    String baseUrl = String(LLM_GEMINI_BASE_URL);
    return call_gemini(baseUrl, "", mod, system_prompt, task, response_out, error_out);
  }

  if (prov == "glm") {
    String mod = model.length() > 0 ? model : String("glm-4.7");
    String baseUrl = String(LLM_GLM_BASE_URL);
    return call_glm_zai(baseUrl, "", mod, system_prompt, task, response_out, error_out);
  }

  if (prov == "openrouter" || prov == "openrouter.ai") {
    String mod = model.length() > 0 ? model : String("qwen/qwen-2.5-coder-32b-instruct:free");
    String baseUrl = "https://openrouter.ai/api";
    return call_openai_like(baseUrl, "", mod, system_prompt, task, response_out, error_out);
  }

  if (prov == "ollama") {
    String mod = model.length() > 0 ? model : String("llama3");
    String baseUrl = "http://ollama.local:11434/api/generate";
    return call_ollama(baseUrl, mod, system_prompt, task, response_out, error_out);
  }

  error_out = "Unsupported provider: " + provider;
  return false;
}

bool llm_generate_with_prompt(const String &system_prompt, const String &task, bool include_memory,
                              String &response_out, String &error_out) {
  // Enrich task with memory if requested
  String enriched_task = task;
  if (include_memory) {
    String notes;
    String mem_err;
    if (memory_get_notes(notes, mem_err)) {
      notes.trim();
      if (notes.length() > 0) {
        if (notes.length() > 400) {
          notes = notes.substring(notes.length() - 400);
        }
        enriched_task = String("Persistent memory:\n") + notes + "\n\nTask:\n" + task;
      }
    }
  }

  if (enriched_task.length() == 0) {
    error_out = "Missing task text";
    return false;
  }

  // Get primary provider config
  String primary_provider;
  String primary_model;
  ModelConfigInfo config;

  if (model_config_get_active_config(config)) {
    primary_provider = config.provider;
    primary_model = config.model;
  } else {
    primary_provider = to_lower(String(LLM_PROVIDER));
    primary_model = String(LLM_MODEL);
  }

  if (primary_provider == "none" || primary_provider.length() == 0) {
    error_out = "LLM disabled. Use: /model set <provider> <api_key>";
    return false;
  }

  // Check if primary provider has API key
  String primary_key = model_config_get_api_key(primary_provider);
  if (primary_key.length() == 0) {
    error_out = "No API key configured for " + primary_provider + ". Use: /model set " + primary_provider + " <your_api_key>";
    return false;
  }

  // Try primary provider first
  String provider = primary_provider;
  String model = primary_model;
  String api_key = primary_key;
  bool using_fallback = false;
  String fallback_reason = "";

  // Try provider (primary or fallback)
  while (true) {
    bool result = false;
    String prov = to_lower(provider);

    // Call the appropriate provider
    if (prov == "openai") {
      String mod = model.length() > 0 ? model : String("gpt-4.1-mini");
      String baseUrl = config.baseUrl.length() > 0 ? config.baseUrl : String(LLM_OPENAI_BASE_URL);
      result = call_openai_like(baseUrl, api_key, mod, system_prompt, enriched_task, response_out, error_out);
    } else if (prov == "anthropic") {
      String mod = model.length() > 0 ? model : String("claude-3-5-sonnet-latest");
      String baseUrl = config.baseUrl.length() > 0 ? config.baseUrl : String(LLM_ANTHROPIC_BASE_URL);
      result = call_anthropic(baseUrl, api_key, mod, system_prompt, enriched_task, response_out, error_out);
    } else if (prov == "gemini") {
      String mod = model.length() > 0 ? model : String("gemini-2.0-flash");
      String baseUrl = config.baseUrl.length() > 0 ? config.baseUrl : String(LLM_GEMINI_BASE_URL);
      result = call_gemini(baseUrl, api_key, mod, system_prompt, enriched_task, response_out, error_out);
    } else if (prov == "glm") {
      String mod = model.length() > 0 ? model : String("glm-4.7");
      String baseUrl = config.baseUrl.length() > 0 ? config.baseUrl : String(LLM_GLM_BASE_URL);
      result = call_glm_zai(baseUrl, api_key, mod, system_prompt, enriched_task, response_out, error_out);
    } else if (prov == "openrouter" || prov == "openrouter.ai") {
      // OpenRouter uses OpenAI-compatible API
      String mod = model.length() > 0 ? model : String("qwen/qwen-2.5-coder-32b-instruct:free");
      String baseUrl = config.baseUrl.length() > 0 ? config.baseUrl : String("https://openrouter.ai/api");
      result = call_openai_like(baseUrl, api_key, mod, system_prompt, enriched_task, response_out, error_out);
    } else if (prov == "ollama") {
      String mod = model.length() > 0 ? model : String("llama3");
      String baseUrl = config.baseUrl.length() > 0 ? config.baseUrl : String("http://ollama.local:11434/api/generate");
      result = call_ollama(baseUrl, mod, system_prompt, enriched_task, response_out, error_out);
    } else {
      error_out = "Unsupported provider: " + provider;
      if (using_fallback) {
        error_out += " (fallback from " + primary_provider + ")";
      }
      return false;
    }

    // If successful, add fallback notice if applicable
    if (result) {
      if (using_fallback) {
        response_out = "⚠️ Using " + provider + " (" + fallback_reason + ")\n\n" + response_out;
      }
      return true;
    }

    // Check if error is quota/rate limit (should trigger fallback)
    if (!is_quota_error(error_out)) {
      // Not a quota error, just fail
      if (using_fallback) {
        error_out += " (fallback from " + primary_provider + ")";
      }
      return false;
    }

    // Quota error - mark provider as failed and try fallback
    model_config_mark_provider_failed(provider, 429);

    // Find fallback provider
    String fallback = model_config_get_fallback_provider(provider);
    if (fallback.length() == 0) {
      // No fallback available
      error_out += " (all providers failed or rate limited)";
      return false;
    }

    // Switch to fallback
    using_fallback = true;
    fallback_reason = primary_provider + " rate limited";
    provider = fallback;
    model = model_config_get_model(fallback);
    api_key = model_config_get_api_key(fallback);
    config.baseUrl = "";  // Reset for new provider

    Serial.printf("[llm] Switching to fallback provider: %s\n", provider.c_str());

    // Loop to try the fallback
  }
}

String first_line_clean(const String &value) {
  String out = value;
  out.trim();
  int nl = out.indexOf('\n');
  if (nl >= 0) {
    out = out.substring(0, nl);
  }
  out.replace("`", "");
  out.trim();
  return out;
}

String extract_routed_command(const String &raw) {
  String line = first_line_clean(raw);
  String lc = line;
  lc.toLowerCase();

  if (lc == "none") {
    return "";
  }

  if (lc.startsWith("tool:")) {
    String cmd = line.substring(5);
    cmd.trim();
    if (cmd.startsWith("/")) {
      cmd.remove(0, 1);
      cmd.trim();
    }
    if (cmd.length() > 180) {
      cmd = cmd.substring(0, 180);
      cmd.trim();
    }
    return cmd;
  }

  if (line.startsWith("/")) {
    line.remove(0, 1);
    line.trim();
  }

  if (line.length() > 180) {
    line = line.substring(0, 180);
    line.trim();
  }
  return line;
}

}  // namespace

// ============================================================================
// PUBLIC API FUNCTIONS
// ============================================================================

// Generate LLM response with custom system prompt (for ReAct, etc.)
bool llm_generate_with_custom_prompt(const String &system_prompt, const String &task,
                                     bool include_memory, String &reply_out, String &error_out) {
  // Enrich task with memory if requested
  String enriched_task = task;
  if (include_memory) {
    String notes;
    String mem_err;
    if (memory_get_notes(notes, mem_err)) {
      notes.trim();
      if (notes.length() > 0) {
        if (notes.length() > 400) {
          notes = notes.substring(notes.length() - 400);
        }
        enriched_task = String("Persistent memory:\n") + notes + "\n\nTask:\n" + task;
      }
    }
  }

  if (enriched_task.length() == 0) {
    error_out = "Missing task text";
    return false;
  }

  // Get primary provider config
  String primary_provider;
  String primary_model;
  ModelConfigInfo config;

  if (model_config_get_active_config(config)) {
    primary_provider = config.provider;
    primary_model = config.model;
  } else {
    primary_provider = String(LLM_PROVIDER);
    primary_provider.toLowerCase();
    primary_model = String(LLM_MODEL);
  }

  if (primary_provider == "none" || primary_provider.length() == 0) {
    error_out = "LLM disabled. Use: /model set <provider> <api_key>";
    return false;
  }

  String primary_key = model_config_get_api_key(primary_provider);
  if (primary_key.length() == 0) {
    error_out = "No API key for " + primary_provider;
    return false;
  }

  // Call appropriate provider
  String prov = primary_provider;
  prov.toLowerCase();
  bool result = false;

  if (prov == "openai") {
    String mod = primary_model.length() > 0 ? primary_model : String("gpt-4.1-mini");
    String baseUrl = config.baseUrl.length() > 0 ? config.baseUrl : String(LLM_OPENAI_BASE_URL);
    result = call_openai_like(baseUrl, primary_key, mod, system_prompt, enriched_task, reply_out, error_out);
  } else if (prov == "anthropic") {
    String mod = primary_model.length() > 0 ? primary_model : String("claude-3-5-sonnet-latest");
    String baseUrl = config.baseUrl.length() > 0 ? config.baseUrl : String(LLM_ANTHROPIC_BASE_URL);
    result = call_anthropic(baseUrl, primary_key, mod, system_prompt, enriched_task, reply_out, error_out);
  } else if (prov == "gemini") {
    String mod = primary_model.length() > 0 ? primary_model : String("gemini-2.0-flash");
    String baseUrl = config.baseUrl.length() > 0 ? config.baseUrl : String(LLM_GEMINI_BASE_URL);
    result = call_gemini(baseUrl, primary_key, mod, system_prompt, enriched_task, reply_out, error_out);
  } else if (prov == "glm") {
    String mod = primary_model.length() > 0 ? primary_model : String("glm-4.7");
    String baseUrl = config.baseUrl.length() > 0 ? config.baseUrl : String(LLM_GLM_BASE_URL);
    result = call_glm_zai(baseUrl, primary_key, mod, system_prompt, enriched_task, reply_out, error_out);
  } else if (prov == "openrouter" || prov == "openrouter.ai") {
    String mod = primary_model.length() > 0 ? primary_model : String("qwen/qwen-2.5-coder-32b-instruct:free");
    String baseUrl = config.baseUrl.length() > 0 ? config.baseUrl : String("https://openrouter.ai/api");
    result = call_openai_like(baseUrl, primary_key, mod, system_prompt, enriched_task, reply_out, error_out);
  } else if (prov == "ollama") {
    String mod = primary_model.length() > 0 ? primary_model : String("llama3");
    String baseUrl = config.baseUrl.length() > 0 ? config.baseUrl : String("http://ollama.local:11434/api/generate");
    result = call_ollama(baseUrl, mod, system_prompt, enriched_task, reply_out, error_out);
  } else {
    error_out = "Unsupported provider: " + primary_provider;
    return false;
  }

  return result;
}

bool llm_generate_plan(const String &task, String &plan_out, String &error_out) {
  return llm_generate_with_custom_prompt(String(kPlanSystemPrompt), task, true, plan_out, error_out);
}

bool llm_generate_reply(const String &message, String &reply_out, String &error_out) {
  const size_t kLongUserMessageChars = 1400;
  const size_t kMaxSkillChars = 700;
  const size_t kMaxSoulChars = 420;
  const size_t kMaxMemoryChars = 700;
  const size_t kMaxScheduleChars = 900;
  const size_t kMaxHistoryChars = 1200;
  const size_t kMaxLastFileChars = 1800;
  const size_t kMaxTaskChars = 5200;

  const bool long_user_message = message.length() > kLongUserMessageChars;
  String system_prompt = String(kChatSystemPrompt);

  system_prompt += "\n\nPROJECT FILE WORKFLOW (PREFER THIS FOR LONG CODING TASKS):\n"
                   "- Persist code in SPIFFS under /projects/<project_name>/...\n"
                   "- Read existing files before editing: files_list, files_get <path>\n"
                   "- Use MinOS for file operations: minos mkdir, minos nano, minos append, minos cat\n"
                   "- When user asks to modify previous code, prefer loading from SPIFFS file path instead of relying only on chat memory.\n"
                   "- Keep edits incremental and return updated file output.";

  // Inject current time awareness
  String time_ctx = build_time_context();
  if (time_ctx.length() > 0) {
    system_prompt += "\n\nCURRENT TIME: " + time_ctx +
                     "\nUse this to greet appropriately (good morning/afternoon/evening) "
                     "and be aware of timing context in conversations.";
  }

  String stored_tz;
  String tz_err;
  if (!persona_get_timezone(stored_tz, tz_err) || stored_tz.length() == 0) {
    system_prompt += "\n\nCRITICAL: User timezone is NOT SET! If they ask to schedule a cron job, reminder, or ask for the time, "
                     "STOP and explicitly ask them 'What City/Country are you in?' FIRST. Then use the timezone_set tool.";
  }

  // Inject real schedule state so LLM doesn't hallucinate reminder/cron status.
  String schedule_ctx = build_schedule_context();
  schedule_ctx = trim_with_ellipsis(schedule_ctx, kMaxScheduleChars);
  system_prompt += "\n\nACTIVE SCHEDULE STATE (source of truth from cron.md + reminder store):\n" +
                   schedule_ctx +
                   "\nWhen user asks about reminders/cron, rely on this state before suggesting changes.";

  // Inject available skills so the agent knows what it can do
  String skill_descs = skill_get_descriptions_for_react();
  if (skill_descs.length() > 0 && !long_user_message) {
    skill_descs = trim_with_ellipsis(skill_descs, kMaxSkillChars);
    system_prompt += "\n\nAVAILABLE SKILLS:\n" + skill_descs +
                     "\nYou can activate any with: use_skill <name> [context]\n"
                      "You can also create new skills with: skill_add <name> <description>: <instructions>";
  }

  // MinOS Shell Awareness (Experimental)
  system_prompt += "\n\nEXPERIMENTAL: You have an internal minimal OS (MinOS) running! "
                   "You can interact with it using: minos <command>\n"
                   "Commands: ls, cat, cd, pwd, mkdir, touch, rm, nano <file> <text> (overwrite), "
                   "append <file> <text> (add to end), ps, free, df, uptime, reboot.\n"
                   "Use this for low-level system management or browsing the internal flash memory.";

  // Include SOUL from file_memory if available
  String soul_text;
  String soul_err;
  if (file_memory_read_soul(soul_text, soul_err)) {
    soul_text.trim();
    if (soul_text.length() > 0) {
      soul_text = trim_with_ellipsis(soul_text, kMaxSoulChars);
      system_prompt += "\n\nSOUL:\n" + soul_text;
    }
  }

  // Include MEMORY.md if available (for recall)
  String memory_text;
  String memory_err;
  if (file_memory_read_long_term(memory_text, memory_err)) {
    memory_text.trim();
    if (memory_text.length() > 0) {
      memory_text = keep_tail_with_marker(memory_text, kMaxMemoryChars);
      system_prompt += "\n\nMEMORY (what you know about the user):\n" + memory_text;
    }
  }

  String task = trim_with_ellipsis(message, kMaxTaskChars);
  String msg_lc = message;
  msg_lc.toLowerCase();

  // Always include recent chat history for better context and follow-ups
  // History is stored in NVS and persists across reboots
  String history;
  String history_err;
  if (!long_user_message && chat_history_get(history, history_err)) {
    history.trim();
    if (history.length() > 0) {
      history = keep_tail_with_marker(history, kMaxHistoryChars);
      task = "Recent conversation (last 15-30 turns):\n" + history + "\n\nCurrent user message:\n" + message;
    }
  }
  // Include last generated file for iteration (short-term memory fallback).
  // Primary preference is project files in SPIFFS (/projects/...).
  // MOVED: Append to system prompt to avoid "User sent this" hallucination
  String last_file_content = agent_loop_get_last_file_content();
  if (!long_user_message && last_file_content.length() > 0) {
    String last_file_name = agent_loop_get_last_file_name();
    if (last_file_name.length() == 0) last_file_name = "generated_code.txt";
    
    last_file_content = trim_with_ellipsis(last_file_content, kMaxLastFileChars);
    
    // Explicitly label as SYSTEM MEMORY
    system_prompt += "\n\n=== SYSTEM MEMORY (Code you previously generated) ===\n"
                     "FILENAME: " + last_file_name + "\n"
                     "CONTENT:\n```\n" + last_file_content + "\n```\n"
                     "You can edit this code if requested. Provide full updated code.\n"
                     "==========================================================\n";
  }

  if (task.length() > kMaxTaskChars) {
    task = trim_with_ellipsis(task, kMaxTaskChars);
  }

  bool result = llm_generate_with_custom_prompt(system_prompt, task, false, reply_out, error_out);

  // Auto-save important info to MEMORY.md
  if (result) {
    // Check if user is sharing something important about themselves
    String msg_lc_check = message;
    msg_lc_check.toLowerCase();

    // Patterns that indicate important info to remember
    const bool is_personal_info =
      msg_lc_check.startsWith("my ") ||
      msg_lc_check.startsWith("i am ") ||
      msg_lc_check.startsWith("i'm ") ||
      msg_lc_check.indexOf(" i like ") >= 0 ||
      msg_lc_check.indexOf(" i love ") >= 0 ||
      msg_lc_check.indexOf(" my favorite ") >= 0 ||
      msg_lc_check.indexOf(" remember that ") >= 0 ||
      msg_lc_check.startsWith("don't forget ") ||
      (msg_lc_check.startsWith("my name is ") || msg_lc_check.startsWith("call me "));

    // Also check if user explicitly asks to remember
    const bool explicit_remember =
      msg_lc_check.startsWith("remember ") ||
      msg_lc_check.indexOf(" please remember") >= 0 ||
      msg_lc_check.indexOf(" don't forget") >= 0;

    if (is_personal_info || explicit_remember) {
      // Auto-save to MEMORY.md
      String save_err;
      String memory_entry = "- " + message;
      if (file_memory_append_long_term(memory_entry, save_err)) {
        Serial.printf("[auto_memory] Saved to MEMORY.md: %s\n", message.c_str());
      }
    }
  }

  // Track usage
  String provider;
  String model;
  ModelConfigInfo config;
  if (model_config_get_active_config(config)) {
    provider = config.provider;
    model = config.model;
  } else {
    provider = String(LLM_PROVIDER);
    model = String(LLM_MODEL);
  }
  usage_record_call("chat", result ? 200 : 500, provider.c_str(), model.c_str());

  return result;
}

bool llm_generate_heartbeat(const String &heartbeat_doc, String &reply_out, String &error_out) {
  String task = heartbeat_doc;
  task.trim();
  if (task.length() == 0) {
    error_out = "Heartbeat is empty";
    return false;
  }

  if (task.length() > 1200) {
    task = task.substring(0, 1200);
  }

  task = "Heartbeat instructions:\n" + task + "\n\nGenerate current heartbeat update.";
  return llm_generate_with_custom_prompt(String(kHeartbeatSystemPrompt), task, false, reply_out, error_out);
}

bool llm_extract_user_facts(const String &user_message, const String &existing_profile,
                            String &facts_out, String &error_out) {
  static const char *kExtractPrompt =
      "Extract ONLY new personal facts from the user's message. "
      "Facts include: name, location, age, job, interests, preferences, schedule, family, pets. "
      "Ignore questions, commands, or temporary context. "
      "If the user's existing profile already contains the fact, skip it. "
      "Return ONLY the new facts as bullet points (- fact). "
      "If no new facts found, return exactly: NONE";

  String task = "User message: " + user_message;
  if (existing_profile.length() > 0) {
    String profile = existing_profile;
    if (profile.length() > 600) {
      profile = profile.substring(profile.length() - 600);
    }
    task += "\n\nExisting profile:\n" + profile;
  }

  String raw_out;
  if (!llm_generate_with_custom_prompt(String(kExtractPrompt), task, false, raw_out, error_out)) {
    return false;
  }

  raw_out.trim();
  if (raw_out.length() == 0 || raw_out == "NONE" || raw_out.indexOf("NONE") >= 0) {
    facts_out = "";
    return true;
  }

  facts_out = raw_out;
  return true;
}

bool llm_generate_proactive(const String &context, String &reply_out, String &error_out) {
  static const char *kProactivePrompt =
      "You are Timi, a proactive dinosaur assistant on ESP32. "
      "Based on the context below, decide if you should send a proactive message to the user. "
      "Good reasons to speak: weather alert, task reminder, time-based greeting, interesting follow-up. "
      "If you have something useful to say, write a short friendly message (1-3 sentences). "
      "If there's nothing useful, respond with exactly: SILENT";

  String raw_out;
  if (!llm_generate_with_custom_prompt(String(kProactivePrompt), context, false, raw_out, error_out)) {
    return false;
  }

  raw_out.trim();
  if (raw_out == "SILENT" || raw_out.indexOf("SILENT") >= 0) {
    reply_out = "";
    return true;
  }

  reply_out = raw_out;
  return true;
}

bool llm_route_tool_command(const String &message, String &command_out, String &error_out) {
  command_out = "";

  // Direct detection for web generation (bypass LLM for reliability)
  String lc = message;
  lc.toLowerCase();

  String task = "User message:\n" + message + "\n\nReturn one line only.";
  String raw;
  if (!llm_generate_with_custom_prompt(String(kRouteSystemPrompt), task, false, raw, error_out)) {
    return false;
  }

  String routed = extract_routed_command(raw);
  if (routed.length() == 0) {
    return true;
  }

  String lc_route = routed;
  lc_route.toLowerCase();
  if (lc_route == "none") {
    return true;
  }

  command_out = routed;
  return true;
}

#if ENABLE_IMAGE_GEN

bool llm_generate_image(const String &prompt, String &base64_out, String &error_out) {
  String provider = to_lower(String(IMAGE_PROVIDER));
  String api_key = String(IMAGE_API_KEY);

  // Backward-compatible fallback: if IMAGE_* are not configured, reuse active LLM provider/key.
  if (provider == "none" || provider.length() == 0) {
    ModelConfigInfo config;
    if (model_config_get_active_config(config)) {
      String llm_provider = config.provider;
      if (llm_provider == "gemini" || llm_provider == "openai") {
        provider = llm_provider;
        api_key = config.apiKey;
      }
    } else {
      const String llm_provider = to_lower(String(LLM_PROVIDER));
      if (llm_provider == "gemini" || llm_provider == "openai") {
        provider = llm_provider;
      }
      if (api_key.length() == 0) {
        api_key = String(LLM_API_KEY);
      }
    }
  }
  if (api_key.length() == 0) {
    api_key = String(LLM_API_KEY);
  }

  if (provider != "gemini" && provider != "openai") {
    error_out = "Image generation requires IMAGE_PROVIDER=gemini/openai (or LLM_PROVIDER fallback)";
    return false;
  }

  if (api_key.length() == 0) {
    error_out = "Missing IMAGE_API_KEY (or LLM_API_KEY fallback)";
    return false;
  }

  if (prompt.length() == 0) {
    error_out = "Missing prompt";
    return false;
  }

  if (provider == "gemini") {
    // Get base URL from config or fallback
    String gemini_base;
    ModelConfigInfo cfg;
    if (model_config_get_active_config(cfg) && cfg.provider == "gemini") {
      gemini_base = cfg.baseUrl;
    } else {
      gemini_base = String(LLM_GEMINI_BASE_URL);
    }

    String last_err = "";

    // Native Gemini image generation models (docs + backward compatibility).
    const char *native_models[] = {
        "gemini-2.5-flash-image",
        "gemini-3-pro-image-preview",
        "gemini-2.0-flash-exp-image-generation",
    };

    for (size_t i = 0; i < (sizeof(native_models) / sizeof(native_models[0])); i++) {
      const String model = String(native_models[i]);
      const String gen_url =
          join_url(gemini_base, String("/v1beta/models/") + model + ":generateContent");
      const String gen_body =
          String("{\"contents\":[{\"parts\":[{\"text\":\"") + json_escape(prompt) +
          "\"}]}],\"generationConfig\":{\"responseModalities\":[\"TEXT\",\"IMAGE\"]}}";

      const HttpResult gen_res =
          http_post_json(gen_url, gen_body, "x-goog-api-key", api_key);
      if (gen_res.status_code >= 200 && gen_res.status_code < 300) {
        if (extract_json_string_field_after_anchor(gen_res.body, "\"inlineData\"", "data",
                                                   base64_out) ||
            extract_json_string_field_after_anchor(gen_res.body, "\"inline_data\"", "data",
                                                   base64_out) ||
            extract_json_string_field(gen_res.body, "data", base64_out)) {
          return true;
        }
        last_err = "Could not parse Gemini image response";
        continue;
      }

      last_err = summarize_http_error("Gemini image", gen_res);
      if (gen_res.status_code == 401) {
        error_out = last_err;
        return false;
      }
    }

    // Optional Imagen endpoint (requires billed access in many projects).
    const String imagen_url =
        join_url(gemini_base, "/v1beta/models/imagen-4.0-generate-001:predict");
    const String imagen_body = String("{\"instances\":[{\"prompt\":\"") + json_escape(prompt) +
                               "\"}],\"parameters\":{\"sampleCount\":1}}";

    const HttpResult imagen_res =
        http_post_json(imagen_url, imagen_body, "x-goog-api-key", api_key);
    if (imagen_res.status_code >= 200 && imagen_res.status_code < 300) {
      if (extract_json_string_field(imagen_res.body, "bytesBase64Encoded", base64_out)) {
        return true;
      }
      error_out = "Could not parse Imagen response";
      return false;
    }

    const String imagen_err = summarize_http_error("Imagen", imagen_res);
    if (last_err.length() == 0) {
      error_out = imagen_err;
      return false;
    }
    if (contains_ci(last_err, "quota") || contains_ci(last_err, "billing")) {
      error_out = last_err;
      return false;
    }
    error_out = imagen_err;
    return false;
  }

  if (provider == "openai") {
    // Get base URL from config or fallback
    String openai_base;
    ModelConfigInfo cfg;
    if (model_config_get_active_config(cfg) && cfg.provider == "openai") {
      openai_base = cfg.baseUrl;
    } else {
      openai_base = String(LLM_OPENAI_BASE_URL);
    }

    const String url = join_url(openai_base, "/v1/images/generations");
    const String body = String("{\"model\":\"dall-e-3\",\"prompt\":\"") + json_escape(prompt) +
                        "\",\"n\":1,\"size\":\"1024x1024\",\"response_format\":\"b64_json\"}";

    const HttpResult res = http_post_json(url, body, "Authorization", "Bearer " + api_key);
    if (res.status_code < 200 || res.status_code >= 300) {
      error_out = "DALL-E HTTP " + String(res.status_code);
      usage_record_call("image", res.status_code, "openai", "dall-e-3");
      return false;
    }

    if (!extract_json_string_field(res.body, "b64_json", base64_out)) {
      error_out = "Could not parse DALL-E response";
      usage_record_call("image", 500, "openai", "dall-e-3");
      return false;
    }

    usage_record_call("image", 200, "openai", "dall-e-3");
    return true;
  }

  error_out = "Image generation requires IMAGE_PROVIDER=gemini/openai (or LLM_PROVIDER fallback)";
  return false;
}

#endif  // ENABLE_IMAGE_GEN

#if ENABLE_MEDIA_UNDERSTANDING

bool llm_understand_media(const String &instruction, const String &mime_type,
                          const String &base64_data, String &reply_out, String &error_out) {
  reply_out = "";

  // Try to get config from NVS first, fallback to .env
  String provider;
  String api_key;
  String model;
  String base_url;

  ModelConfigInfo config;
  if (model_config_get_active_config(config)) {
    provider = config.provider;
    api_key = config.apiKey;
    model = config.model;
    base_url = config.baseUrl;
  } else {
    provider = to_lower(String(LLM_PROVIDER));
    api_key = String(LLM_API_KEY);
    model = String(LLM_MODEL);
  }

  // Allow IMAGE_PROVIDER gemini override when no provider set
  if ((provider.length() == 0 || provider == "none") &&
      to_lower(String(IMAGE_PROVIDER)) == "gemini") {
    provider = "gemini";
    if (api_key.length() == 0) {
      api_key = String(IMAGE_API_KEY);
    }
  }
  if (api_key.length() == 0) {
    api_key = String(IMAGE_API_KEY);
  }

  if (api_key.length() == 0) {
    error_out = "No API key configured. Use: /model set <provider> <key>";
    return false;
  }

  String prompt = instruction;
  prompt.trim();
  if (prompt.length() == 0) {
    prompt = "Analyze this image and return a concise summary.";
  }

  String media_mime = mime_type;
  media_mime.trim();
  if (media_mime.length() == 0) {
    media_mime = "image/jpeg";
  }

  if (base64_data.length() == 0) {
    error_out = "Missing media data";
    return false;
  }
  if (base64_data.length() > 260000) {
    error_out = "Media payload too large for ESP32";
    return false;
  }

  // ── Gemini path ──
  if (provider == "gemini") {
    String gemini_base = base_url.length() > 0 ? base_url : String(LLM_GEMINI_BASE_URL);
    model.trim();
    String model_lc = model;
    model_lc.toLowerCase();
    if (model.length() == 0 || contains_ci(model_lc, "image-generation") ||
        model_lc.endsWith("-image")) {
      model = "gemini-2.0-flash";
    }

    const String url = join_url(gemini_base,
                                String("/v1beta/models/") + model + ":generateContent");
    const String body =
        String("{\"contents\":[{\"parts\":[{\"text\":\"") + json_escape(prompt) +
        "\"},{\"inlineData\":{\"mimeType\":\"" + json_escape(media_mime) +
        "\",\"data\":\"" + base64_data +
        "\"}}]}],\"generationConfig\":{\"temperature\":0.2}}";

    const HttpResult res = http_post_json(url, body, "x-goog-api-key", api_key);
    if (res.status_code < 200 || res.status_code >= 300) {
      error_out = summarize_http_error("Gemini media", res);
      usage_record_call("media", res.status_code, "gemini", model.c_str());
      return false;
    }

    if (!parse_response_text(res.body, reply_out)) {
      error_out = "Could not parse Gemini media response";
      usage_record_call("media", 500, "gemini", model.c_str());
      return false;
    }

    reply_out.trim();
    if (reply_out.length() == 0) {
      error_out = "Empty Gemini media response";
      usage_record_call("media", 500, "gemini", model.c_str());
      return false;
    }

    usage_record_call("media", 200, "gemini", model.c_str());
    return true;
  }

  // ── OpenRouter / OpenAI / Anthropic path (OpenAI-compatible vision API) ──
  if (provider == "openrouter" || provider == "openrouter.ai" ||
      provider == "openai" || provider == "anthropic") {

    String vision_base;
    String vision_model = model;

    if (provider == "openrouter" || provider == "openrouter.ai") {
      vision_base = base_url.length() > 0 ? base_url : String("https://openrouter.ai/api");
      if (vision_model.length() == 0) {
        vision_model = "qwen/qwen-2.5-coder-32b-instruct:free";
      }
    } else if (provider == "openai") {
      vision_base = base_url.length() > 0 ? base_url : String(LLM_OPENAI_BASE_URL);
      if (vision_model.length() == 0) {
        vision_model = "gpt-4o-mini";
      }
    } else {
      // Anthropic via OpenRouter-compatible endpoint
      vision_base = base_url.length() > 0 ? base_url : String(LLM_ANTHROPIC_BASE_URL);
      if (vision_model.length() == 0) {
        vision_model = "claude-3-haiku-20240307";
      }
    }

    const String url = join_url(vision_base, "/v1/chat/completions");

    // Build data URI: data:<mime>;base64,<data>
    const String data_uri = "data:" + media_mime + ";base64," + base64_data;

    // Retry loop: Try requested model, then fallback if it fails
    for (int attempt = 0; attempt < 2; attempt++) {
      // OpenAI vision format with image_url
      const String body =
          String("{\"model\":\"") + json_escape(vision_model) +
          "\",\"messages\":[{\"role\":\"user\",\"content\":[" +
          "{\"type\":\"text\",\"text\":\"" + json_escape(prompt) + "\"}," +
          "{\"type\":\"image_url\",\"image_url\":{\"url\":\"" + data_uri + "\"}}" +
          "]}],\"temperature\":0.2,\"max_tokens\":1024}";

      const HttpResult res =
          http_post_json(url, body, "Authorization", "Bearer " + api_key);

      if (res.status_code >= 200 && res.status_code < 300) {
        if (parse_response_text(res.body, reply_out)) {
          reply_out.trim();
          if (reply_out.length() > 0) {
            usage_record_call("media", 200, provider.c_str(), vision_model.c_str());
            return true;
          }
        }
      }

      // Handle failure
      usage_record_call("media", res.status_code, provider.c_str(), vision_model.c_str());
      
      // Only retry if on OpenRouter (can switch to other models)
      if (attempt == 0 && (provider == "openrouter" || provider == "openrouter.ai")) {
        Serial.printf("[llm] Vision model %s failed (HTTP %d), trying fallback...\n", vision_model.c_str(), res.status_code);
        
        // Priority 1: Fallback to Gemini provider if configured (user preference)
        String gemini_key = model_config_get_api_key("gemini");
        if (gemini_key.length() > 0) {
            String gemini_model = model_config_get_model("gemini");
            if (gemini_model.length() == 0) gemini_model = "gemini-2.5-flash"; // Default to 2.5-flash (GA June 2025)

            Serial.printf("[llm] Switching to Gemini provider: %s\n", gemini_model.c_str());
            
            // Execute Gemini logic inline
            String gemini_base = String(LLM_GEMINI_BASE_URL);
            String g_url = join_url(gemini_base, String("/v1beta/models/") + gemini_model + ":generateContent");
            String g_body = String("{\"contents\":[{\"parts\":[{\"text\":\"") + json_escape(prompt) +
                            "\"},{\"inlineData\":{\"mimeType\":\"" + json_escape(media_mime) +
                            "\",\"data\":\"" + base64_data +
                            "\"}}]}],\"generationConfig\":{\"temperature\":0.2}}";

            HttpResult g_res = http_post_json(g_url, g_body, "x-goog-api-key", gemini_key);
            if (g_res.status_code >= 200 && g_res.status_code < 300) {
               if (parse_response_text(g_res.body, reply_out)) {
                   reply_out.trim();
                   if (reply_out.length() > 0) {
                       usage_record_call("media", 200, "gemini", gemini_model.c_str());
                       return true;
                   }
               }
            }
            // If Gemini provider failed, continue to other fallback? 
            // Or stop? Let's stop if explicit provider fallback failed to avoid confusion.
            error_out = summarize_http_error("Gemini fallback", g_res);
            return false;
        }

        // Priority 2: Fallback to OpenRouter free Gemini
        vision_model = "google/gemini-2.0-flash-lite-preview-02-05:free";
        continue;
      }
      
      // Final failure
      error_out = summarize_http_error("Vision", res);
      return false;
    }
    
    return false;
  }

  error_out = "Vision not supported for provider: " + provider + ". Use openrouter, openai, or gemini.";
  return false;
}

#endif  // ENABLE_MEDIA_UNDERSTANDING

namespace {

static String extract_json_value(const String &json, const String &key) {
  String search_key = "\"" + key + "\":\"";
  int key_start = json.indexOf(search_key);
  if (key_start < 0) {
    return "";
  }

  int value_start = key_start + search_key.length();
  if (value_start >= json.length()) {
    return "";
  }

  int value_end = value_start;
  while (value_end < json.length()) {
    char c = json[value_end];
    if (c == '\\' && value_end + 1 < json.length()) {
      value_end += 2;  // Skip escaped character
    } else if (c == '"') {
      break;
    } else {
      value_end++;
    }
  }

  if (value_end >= json.length() || json[value_end] != '"') {
    return "";
  }

  return json.substring(value_start, value_end);
}

}  // namespace

bool llm_parse_email_request(const String &message, String &to_out, String &subject_out,
                             String &body_out, String &error_out) {
  if (message.length() == 0) {
    error_out = "Empty message";
    return false;
  }

  const String provider = to_lower(String(LLM_PROVIDER));
  const String api_key = String(LLM_API_KEY);

  if (api_key.length() == 0) {
    error_out = "Missing LLM_API_KEY";
    return false;
  }

  String url;
  String headers;
  String body;

  // Build request based on provider
  if (provider == "glm" || provider == "zhipu" || provider == "openai") {
    // GLM and OpenAI-compatible format
    url = join_url(String(LLM_GLM_BASE_URL), "/chat/completions");

    const String system_prompt =
        "Extract email details from the user's message. "
        "Return ONLY in this exact JSON format (no markdown, no extra text):\n"
        "{\"to\":\"email@example.com\",\"subject\":\"Email Subject\",\"body\":\"Email "
        "body text\"}\n\n"
        "Rules:\n"
        "- If any field is missing or unclear, use empty string \"\"\n"
        "- to: must be a valid email address\n"
        "- subject: short and clear\n"
        "- body: the main message content\n"
        "- Return ONLY valid JSON, nothing else";

    const String json_body =
        String("{\"model\":\"") + String(LLM_MODEL) +
        "\",\"messages\":[{\"role\":\"system\",\"content\":\"" + json_escape(system_prompt) +
        "\"},{\"role\":\"user\",\"content\":\"" + json_escape(message) + "\"}]}";

    const HttpResult res = http_post_json(url, json_body, "Authorization", "Bearer " + api_key);

    if (res.status_code < 200 || res.status_code >= 300) {
      error_out = "LLM HTTP " + String(res.status_code);
      return false;
    }

    String response;
    if (!parse_response_text(res.body, response)) {
      error_out = "Could not parse LLM response";
      return false;
    }

    // Parse JSON response
    String json_str = response;
    to_out = extract_json_value(json_str, "to");
    subject_out = extract_json_value(json_str, "subject");
    body_out = extract_json_value(json_str, "body");

    if (to_out.length() == 0) {
      error_out = "Could not extract email address from response";
      return false;
    }

    return true;
  }

  if (provider == "gemini" || provider == "anthropic") {
    // Gemini and Anthropic format (media understanding endpoint)
    if (provider == "gemini") {
      url = String("https://generativelanguage.googleapis.com/v1beta/models/") +
             String(LLM_MODEL) + ":generateContent?key=" + api_key;
    } else {
      url = join_url(String(LLM_ANTHROPIC_BASE_URL), "/v1/messages");
    }

    const String prompt =
        "Extract email details from: \"" + message + "\"\n"
        "Return ONLY this JSON format: {\"to\":\"email\",\"subject\":\"subject\",\"body\":\"body\"}\n"
        "Use empty string \"\" for missing fields.";

    if (provider == "gemini") {
      const String json_body =
          String("{\"contents\":[{\"parts\":[{\"text\":\"" + json_escape(prompt) + "\"}]}]}");
      const HttpResult res = http_post_json(url, json_body);

      if (res.status_code < 200 || res.status_code >= 300) {
        error_out = "Gemini HTTP " + String(res.status_code);
        return false;
      }

      String response;
      if (!parse_response_text(res.body, response)) {
        error_out = "Could not parse Gemini response";
        return false;
      }

      String json_str = response;
      to_out = extract_json_value(json_str, "to");
      subject_out = extract_json_value(json_str, "subject");
      body_out = extract_json_value(json_str, "body");

      if (to_out.length() == 0) {
        error_out = "Could not extract email from response";
        return false;
      }

      return true;
    }

    // Anthropic
    const String json_body =
        String("{\"model\":\"claude-3-haiku-20240307\",\"max_tokens\":1024,") +
        "\"messages\":[{\"role\":\"user\",\"content\":\"" + json_escape(prompt) + "\"}]}";

    const HttpResult res = http_post_json(url, json_body, "x-api-key",
                                           "sk-ant-" + api_key);

    if (res.status_code < 200 || res.status_code >= 300) {
      error_out = "Anthropic HTTP " + String(res.status_code);
      return false;
    }

    String response;
    if (!parse_response_text(res.body, response)) {
      error_out = "Could not parse Anthropic response";
      return false;
    }

    String json_str = response;
    to_out = extract_json_value(json_str, "to");
    subject_out = extract_json_value(json_str, "subject");
    body_out = extract_json_value(json_str, "body");

    if (to_out.length() == 0) {
      error_out = "Could not extract email from response";
      return false;
    }

    return true;
  }

  error_out = "Email parsing not supported for provider: " + provider;
  return false;
}

bool llm_parse_update_request(const String &message, String &url_out, bool &should_update_out,
                              bool &check_github_out, String &error_out) {
  if (message.length() == 0) {
    error_out = "Empty message";
    return false;
  }

  const String provider = to_lower(String(LLM_PROVIDER));
  const String api_key = String(LLM_API_KEY);

  if (api_key.length() == 0) {
    error_out = "Missing LLM_API_KEY";
    return false;
  }

  String url;
  String headers;
  String body;

  // Build request based on provider
  if (provider == "glm" || provider == "zhipu" || provider == "openai") {
    // GLM and OpenAI-compatible format
    url = join_url(String(LLM_GLM_BASE_URL), "/chat/completions");

    const String system_prompt =
        "Parse the user's message about firmware update. "
        "Return ONLY in this exact JSON format (no markdown, no extra text):\n"
        "{\"url\":\"https://...\",\"should_update\":true,\"check_github\":false}\n\n"
        "Rules:\n"
        "- url: the firmware URL if provided, otherwise empty string \"\"\n"
        "- should_update: true if user wants to update/check for updates, false otherwise\n"
        "- check_github: true if user says 'latest', 'newest', or wants GitHub release, false otherwise\n"
        "- If user just asks about update status, set should_update=true but url=\"\" and check_github=false\n"
        "- If user wants latest release from GitHub, set check_github=true and url=\"\"\n"
        "- Return ONLY valid JSON, nothing else";

    const String json_body =
        String("{\"model\":\"") + String(LLM_MODEL) +
        "\",\"messages\":[{\"role\":\"system\",\"content\":\"" + json_escape(system_prompt) +
        "\"},{\"role\":\"user\",\"content\":\"" + json_escape(message) + "\"}]}";

    const HttpResult res = http_post_json(url, json_body, "Authorization", "Bearer " + api_key);

    if (res.status_code < 200 || res.status_code >= 300) {
      error_out = "LLM HTTP " + String(res.status_code);
      return false;
    }

    String response;
    if (!parse_response_text(res.body, response)) {
      error_out = "Could not parse LLM response";
      return false;
    }

    // Parse JSON response
    String json_str = response;
    url_out = extract_json_value(json_str, "url");

    String should_update_str = extract_json_value(json_str, "should_update");
    should_update_out = (should_update_str == "true" || should_update_str == "1");

    String check_github_str = extract_json_value(json_str, "check_github");
    check_github_out = (check_github_str == "true" || check_github_str == "1");

    return true;
  }

  if (provider == "gemini" || provider == "anthropic") {
    // Gemini and Anthropic format
    if (provider == "gemini") {
      url = String("https://generativelanguage.googleapis.com/v1beta/models/") +
             String(LLM_MODEL) + ":generateContent?key=" + api_key;
    } else {
      url = join_url(String(LLM_ANTHROPIC_BASE_URL), "/v1/messages");
    }

    const String prompt =
        "Parse this update request: \"" + message + "\"\n"
        "Return ONLY this JSON format: {\"url\":\"https://...\",\"should_update\":true,\"check_github\":false}\n"
        "url: firmware URL or empty, should_update: true/false, check_github: true if user wants latest from GitHub";

    if (provider == "gemini") {
      const String json_body =
          String("{\"contents\":[{\"parts\":[{\"text\":\"" + json_escape(prompt) + "\"}]}]}");
      const HttpResult res = http_post_json(url, json_body);

      if (res.status_code < 200 || res.status_code >= 300) {
        error_out = "Gemini HTTP " + String(res.status_code);
        return false;
      }

      String response;
      if (!parse_response_text(res.body, response)) {
        error_out = "Could not parse Gemini response";
        return false;
      }

      String json_str = response;
      url_out = extract_json_value(json_str, "url");

      String should_update_str = extract_json_value(json_str, "should_update");
      should_update_out = (should_update_str == "true" || should_update_str == "1");

      String check_github_str = extract_json_value(json_str, "check_github");
      check_github_out = (check_github_str == "true" || check_github_str == "1");

      return true;
    }

    // Anthropic
    const String json_body =
        String("{\"model\":\"claude-3-haiku-20240307\",\"max_tokens\":1024,") +
        "\"messages\":[{\"role\":\"user\",\"content\":\"" + json_escape(prompt) + "\"}]}";

    const HttpResult res = http_post_json(url, json_body, "x-api-key",
                                           "sk-ant-" + api_key);

    if (res.status_code < 200 || res.status_code >= 300) {
      error_out = "Anthropic HTTP " + String(res.status_code);
      return false;
    }

    String response;
    if (!parse_response_text(res.body, response)) {
      error_out = "Could not parse Anthropic response";
      return false;
    }

    String json_str = response;
    url_out = extract_json_value(json_str, "url");

    String should_update_str = extract_json_value(json_str, "should_update");
    should_update_out = (should_update_str == "true" || should_update_str == "1");

    String check_github_str = extract_json_value(json_str, "check_github");
    check_github_out = (check_github_str == "true" || check_github_str == "1");

    return true;
  }

  error_out = "Update parsing not supported for provider: " + provider;
  return false;
}

bool llm_fetch_provider_models(const String &provider, String &models_out, String &error_out) {
  String prov_lc = to_lower(provider);

  if (prov_lc != "openrouter" && prov_lc != "openrouter.ai") {
    error_out = "Model listing only supported for OpenRouter. Use: model list openrouter";
    return false;
  }

  // Get OpenRouter API key
  String api_key = model_config_get_api_key("openrouter");
  if (api_key.length() == 0) {
    error_out = "No OpenRouter API key configured. Use: model set openrouter <your_api_key>";
    return false;
  }

  // Fetch models from OpenRouter
  if (WiFi.status() != WL_CONNECTED) {
    error_out = "WiFi not connected";
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  const String url = "https://openrouter.ai/api/v1/models";
  if (!https.begin(client, url)) {
    error_out = "HTTP begin failed";
    return false;
  }

  https.setConnectTimeout(12000);
  https.setTimeout(15000);
  https.addHeader("Authorization", "Bearer " + api_key);

  int status_code = https.GET();
  String body = "";
  if (status_code > 0) {
    body = https.getString();
  } else {
    error_out = "HTTP request failed: " + https.errorToString(status_code);
    https.end();
    return false;
  }
  https.end();

  if (status_code < 200 || status_code >= 300) {
    error_out = "OpenRouter HTTP " + String(status_code);
    return false;
  }

  // Parse models from JSON response
  // Format: {"data":[{"id":"model-name","name":"Display Name",...},...]}
  models_out = "📋 OpenRouter Available Models:\n\n";

  int data_start = body.indexOf("\"data\":");
  if (data_start < 0) {
    error_out = "Could not parse OpenRouter response";
    return false;
  }

  // Find each model object
  int search_pos = data_start + 6;  // Skip "data":
  int count = 0;
  const int max_models = 30;  // Limit to prevent overflow

  while (count < max_models) {
    int id_start = body.indexOf("\"id\":", search_pos);
    if (id_start < 0) break;

    int id_val_start = body.indexOf("\"", id_start + 5);
    if (id_val_start < 0) break;
    id_val_start++;  // Skip opening quote

    int id_val_end = body.indexOf("\"", id_val_start);
    if (id_val_end < 0) break;

    String model_id = body.substring(id_val_start, id_val_end);

    // Try to find the name field too
    int name_start = body.indexOf("\"name\":", id_val_end);
    String model_name = model_id;
    if (name_start > 0 && name_start < id_val_end + 200) {  // Within reasonable distance
      int name_val_start = body.indexOf("\"", name_start + 7);
      if (name_val_start > 0) {
        name_val_start++;
        int name_val_end = body.indexOf("\"", name_val_start);
        if (name_val_end > 0) {
          model_name = body.substring(name_val_start, name_val_end);
        }
      }
    }

    models_out += String("• ") + model_id;
    if (model_name != model_id) {
      models_out += " (" + model_name + ")";
    }
    models_out += "\n";

    search_pos = id_val_end + 1;
    count++;

    // Check if we've reached the end of the array
    if (body[search_pos] == ']') break;
  }

  if (count == 0) {
    error_out = "No models found";
    return false;
  }

  models_out += "\nShowing " + String(count) + " models.";
  return true;
}

