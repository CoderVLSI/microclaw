#include "llm_client.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "brain_config.h"
#include "chat_history.h"
#include "memory_store.h"
#include "model_config.h"
#include "persona_store.h"
#include "usage_stats.h"

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
  // Try to get config from NVS first, fallback to .env defaults
  ModelConfigInfo config;
  bool has_config = model_config_get_active_config(config);

  String provider;
  String api_key;
  String model;

  if (has_config) {
    provider = config.provider;
    api_key = config.apiKey;
    model = config.model;
  } else {
    // Fallback to compile-time .env defaults
    provider = to_lower(String(LLM_PROVIDER));
    api_key = String(LLM_API_KEY);
    model = String(LLM_MODEL);
  }

  if (provider == "none" || provider.length() == 0) {
    error_out = "LLM disabled. Use: /model set <provider> <api_key>";
    return false;
  }

  if (api_key.length() == 0) {
    error_out = "No API key configured for " + provider + ". Use: /model set " + provider + " <your_api_key>";
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
    String baseUrl = (has_config && config.baseUrl.length() > 0) ? config.baseUrl : String(LLM_OPENAI_BASE_URL);
    return call_openai_like(baseUrl, api_key, model, system_prompt,
                            enriched_task, response_out, error_out);
  }

  if (provider == "anthropic") {
    if (model.length() == 0) {
      model = "claude-3-5-sonnet-latest";
    }
    String baseUrl = (has_config && config.baseUrl.length() > 0) ? config.baseUrl : String(LLM_ANTHROPIC_BASE_URL);
    return call_anthropic(baseUrl, api_key, model, system_prompt,
                          enriched_task, response_out, error_out);
  }

  if (provider == "gemini") {
    if (model.length() == 0) {
      model = "gemini-2.0-flash";
    }
    String baseUrl = (has_config && config.baseUrl.length() > 0) ? config.baseUrl : String(LLM_GEMINI_BASE_URL);
    return call_gemini(baseUrl, api_key, model, system_prompt, enriched_task,
                       response_out, error_out);
  }

  if (provider == "glm") {
    if (model.length() == 0) {
      model = "glm-4.7";
    }
    String baseUrl = (has_config && config.baseUrl.length() > 0) ? config.baseUrl : String(LLM_GLM_BASE_URL);
    return call_glm_zai(baseUrl, api_key, model, system_prompt, enriched_task,
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

  bool result = llm_generate_with_prompt(system_prompt, task, true, reply_out, error_out);

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

bool llm_understand_media(const String &instruction, const String &mime_type,
                          const String &base64_data, String &reply_out, String &error_out) {
  reply_out = "";

  // Try to get config from NVS first, fallback to .env
  String provider;
  String api_key;

  ModelConfigInfo config;
  if (model_config_get_active_config(config)) {
    provider = config.provider;
    api_key = config.apiKey;
  } else {
    provider = to_lower(String(LLM_PROVIDER));
    api_key = String(LLM_API_KEY);
  }

  // Allow IMAGE_PROVIDER gemini override
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

  if (provider != "gemini") {
    error_out = "Media understanding currently requires Gemini. Use: /model use gemini";
    return false;
  }
  if (api_key.length() == 0) {
    error_out = "No API key for Gemini. Use: /model set gemini <your_api_key>";
    return false;
  }

  String prompt = instruction;
  prompt.trim();
  if (prompt.length() == 0) {
    prompt = "Analyze this file and return a concise summary.";
  }

  String media_mime = mime_type;
  media_mime.trim();
  if (media_mime.length() == 0) {
    media_mime = "application/octet-stream";
  }

  if (base64_data.length() == 0) {
    error_out = "Missing media data";
    return false;
  }
  if (base64_data.length() > 260000) {
    error_out = "Media payload too large for ESP32";
    return false;
  }

  // Get model and baseUrl from config
  String model;
  String gemini_base;
  ModelConfigInfo cfg;
  if (model_config_get_active_config(cfg) && cfg.provider == "gemini") {
    model = cfg.model;
    gemini_base = cfg.baseUrl;
  } else {
    model = String(LLM_MODEL);
    gemini_base = String(LLM_GEMINI_BASE_URL);
  }

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

