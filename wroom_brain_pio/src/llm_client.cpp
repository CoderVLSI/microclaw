#include "llm_client.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "brain_config.h"
#include "chat_history.h"
#include "memory_store.h"
#include "persona_store.h"

namespace {

static const char *kPlanSystemPrompt =
    "You are a coding planner. Return a concise implementation plan only. "
    "Use numbered steps. Include risks and quick validation checks.";
static const char *kChatSystemPrompt =
    "You are a concise, practical assistant running on an ESP32 bot over Telegram. "
    "Be helpful, clear, and brief by default.";
static const char *kHeartbeatSystemPrompt =
    "You are running an autonomous heartbeat check for an ESP32 Telegram agent. "
    "Read the heartbeat instructions and return a short operational update in 3 bullets: "
    "health, risk, next action.";
static const char *kRouteSystemPrompt =
    "Route user text to one tool command if obvious. "
    "Return exactly one line only: TOOL: <command> or NONE. "
    "No explanation, no markdown.";

struct HttpResult {
  int status_code;
  String body;
  String error;
};

String to_lower(String value) {
  value.toLowerCase();
  return value;
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

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  if (!https.begin(client, url)) {
    result.error = "HTTP begin failed";
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
  } else {
    result.error = https.errorToString(result.status_code);
  }

  https.end();
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
    error_out = "LLM HTTP " + String(res.status_code);
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
    error_out = "LLM HTTP " + String(res.status_code);
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
    error_out = "LLM HTTP " + String(res.status_code);
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
    error_out = "LLM HTTP " + String(res.status_code);
    return false;
  }

  if (!parse_response_text(res.body, response_out)) {
    error_out = "Could not parse provider response";
    return false;
  }

  return true;
}

bool llm_generate_with_prompt(const String &system_prompt, const String &task, bool include_memory,
                              String &response_out, String &error_out) {
  const String provider = to_lower(String(LLM_PROVIDER));
  const String api_key = String(LLM_API_KEY);
  String model = String(LLM_MODEL);

  if (provider == "none" || provider.length() == 0) {
    error_out = "LLM disabled (set LLM_PROVIDER in .env)";
    return false;
  }

  if (api_key.length() == 0) {
    error_out = "Missing LLM_API_KEY";
    return false;
  }

  if (task.length() == 0) {
    error_out = "Missing task text";
    return false;
  }

  String notes;
  String mem_err;
  String enriched_task = task;
  if (include_memory && memory_get_notes(notes, mem_err)) {
    notes.trim();
    if (notes.length() > 0) {
      if (notes.length() > 400) {
        notes = notes.substring(notes.length() - 400);
      }
      enriched_task = String("Persistent memory:\n") + notes + "\n\nTask:\n" + task;
    }
  }

  if (provider == "openai") {
    if (model.length() == 0) {
      model = "gpt-4.1-mini";
    }
    return call_openai_like(String(LLM_OPENAI_BASE_URL), api_key, model, system_prompt,
                            enriched_task, response_out, error_out);
  }

  if (provider == "anthropic") {
    if (model.length() == 0) {
      model = "claude-3-5-sonnet-latest";
    }
    return call_anthropic(String(LLM_ANTHROPIC_BASE_URL), api_key, model, system_prompt,
                          enriched_task, response_out, error_out);
  }

  if (provider == "gemini") {
    if (model.length() == 0) {
      model = "gemini-2.0-flash";
    }
    return call_gemini(String(LLM_GEMINI_BASE_URL), api_key, model, system_prompt, enriched_task,
                       response_out, error_out);
  }

  if (provider == "glm") {
    if (model.length() == 0) {
      model = "glm-4.7";
    }
    return call_glm_zai(String(LLM_GLM_BASE_URL), api_key, model, system_prompt, enriched_task,
                        response_out, error_out);
  }

  error_out = "Unsupported LLM_PROVIDER. Use: openai, anthropic, gemini, glm, none";
  return false;
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

bool llm_generate_plan(const String &task, String &plan_out, String &error_out) {
  return llm_generate_with_prompt(String(kPlanSystemPrompt), task, true, plan_out, error_out);
}

bool llm_generate_reply(const String &message, String &reply_out, String &error_out) {
  String system_prompt = String(kChatSystemPrompt);
  String soul_text;
  String soul_err;
  if (persona_get_soul(soul_text, soul_err)) {
    soul_text.trim();
    if (soul_text.length() > 0) {
      if (soul_text.length() > 600) {
        soul_text = soul_text.substring(0, 600);
      }
      system_prompt += "\n\nSOUL profile:\n" + soul_text;
    }
  }

  String task = message;
  String msg_lc = message;
  msg_lc.toLowerCase();
  const bool likely_followup = (msg_lc.indexOf("it") >= 0) || (msg_lc.indexOf("that") >= 0) ||
                               (msg_lc.indexOf("this") >= 0) || (msg_lc.indexOf("again") >= 0) ||
                               (msg_lc.indexOf("continue") >= 0) || (msg_lc.indexOf("same") >= 0) ||
                               (msg_lc.indexOf("previous") >= 0) || (msg_lc.indexOf("above") >= 0) ||
                               (msg_lc.indexOf("last") >= 0) || (msg_lc.startsWith("and ")) ||
                               (msg_lc.startsWith("also "));
  String history;
  String history_err;
  if (likely_followup && chat_history_get(history, history_err)) {
    history.trim();
    if (history.length() > 0) {
      task = "Recent conversation:\n" + history + "\n\nCurrent user message:\n" + message;
    }
  }

  return llm_generate_with_prompt(system_prompt, task, true, reply_out, error_out);
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
  return llm_generate_with_prompt(String(kHeartbeatSystemPrompt), task, false, reply_out, error_out);
}

bool llm_route_tool_command(const String &message, String &command_out, String &error_out) {
  command_out = "";

  String task = "User message:\n" + message + "\n\nReturn one line only.";
  String raw;
  if (!llm_generate_with_prompt(String(kRouteSystemPrompt), task, false, raw, error_out)) {
    return false;
  }

  String routed = extract_routed_command(raw);
  if (routed.length() == 0) {
    return true;
  }

  String lc = routed;
  lc.toLowerCase();
  if (lc == "none") {
    return true;
  }

  command_out = routed;
  return true;
}

bool llm_generate_image(const String &prompt, String &base64_out, String &error_out) {
  const String provider = to_lower(String(IMAGE_PROVIDER));
  const String api_key = String(IMAGE_API_KEY);

  if (provider != "gemini" && provider != "openai") {
    error_out = "Image generation requires IMAGE_PROVIDER=gemini or openai in .env";
    return false;
  }

  if (api_key.length() == 0) {
    error_out = "Missing IMAGE_API_KEY in .env";
    return false;
  }

  if (prompt.length() == 0) {
    error_out = "Missing prompt";
    return false;
  }

  if (provider == "gemini") {
    const String url = String("https://generativelanguage.googleapis.com/v1beta/models/imagen-4.0-generate-001:predict?key=") + api_key;
    const String body = String("{\"instances\":[{\"prompt\":\"") + json_escape(prompt) +
                        "\"}],\"parameters\":{\"sampleCount\":1}}";

    const HttpResult res = http_post_json(url, body);
    if (res.status_code < 200 || res.status_code >= 300) {
      error_out = "Imagen HTTP " + String(res.status_code);
      return false;
    }

    if (!extract_json_string_field(res.body, "bytesBase64Encoded", base64_out)) {
      error_out = "Could not parse Imagen response";
      return false;
    }

    return true;
  }

  if (provider == "openai") {
    const String url = join_url(String(LLM_OPENAI_BASE_URL), "/v1/images/generations");
    const String body = String("{\"model\":\"dall-e-3\",\"prompt\":\"") + json_escape(prompt) +
                        "\",\"n\":1,\"size\":\"1024x1024\",\"response_format\":\"b64_json\"}";

    const HttpResult res = http_post_json(url, body, "Authorization", "Bearer " + api_key);
    if (res.status_code < 200 || res.status_code >= 300) {
      error_out = "DALL-E HTTP " + String(res.status_code);
      return false;
    }

    if (!extract_json_string_field(res.body, "b64_json", base64_out)) {
      error_out = "Could not parse DALL-E response";
      return false;
    }

    return true;
  }

  error_out = "Image generation requires IMAGE_PROVIDER=gemini or openai in .env";
  return false;
}
