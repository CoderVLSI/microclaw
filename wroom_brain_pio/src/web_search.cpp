#include "web_search.h"

#include "brain_config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static const int kMaxResults = 10;
static const int kTimeoutMs = WEB_SEARCH_TIMEOUT_MS;

// HTTP POST helper
static String http_post(const String &url, const String &json_body, int *status_code,
                        const String &header_name = "", const String &header_value = "") {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  if (!https.begin(client, url)) {
    if (status_code) *status_code = -1;
    return String();
  }

  https.setConnectTimeout(10000);
  https.setTimeout(kTimeoutMs);
  https.addHeader("Content-Type", "application/json");
  if (header_name.length() > 0) {
    https.addHeader(header_name, header_value);
  }

  const int code = https.POST((uint8_t *)json_body.c_str(), json_body.length());
  if (status_code) *status_code = code;

  String body;
  if (code > 0) {
    body = https.getString();
  }

  https.end();
  return body;
}

// JSON escape for API calls
static String json_escape(const String &src) {
  String out;
  out.reserve(src.length() * 1.2);
  for (size_t i = 0; i < src.length(); i++) {
    const char c = src[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:   out += c; break;
    }
  }
  return out;
}

static String resolve_serper_key() {
  String key = String(SERPER_API_KEY);
  key.trim();
  return key;
}

static String resolve_tavily_key() {
  String key = String(TAVILY_API_KEY);
  key.trim();
  if (key.length() == 0) {
    key = String(WEB_SEARCH_API_KEY);
    key.trim();
  }
  return key;
}

static String resolve_tavily_base_url() {
  String base = String(TAVILY_BASE_URL);
  base.trim();
  if (base.length() == 0) {
    base = "https://api.tavily.com";
  }
  return base;
}

// ============ SERPER (Google Search API) ============

static bool search_serper(const String &query, const String &api_key,
                          SearchResult *results, int *count,
                          String &error_out) {
  if (api_key.length() == 0) {
    error_out = "Serper API key not set";
    return false;
  }

  // Build Serper API request
  String url = "https://google.serper.dev/search";

  String json = "{";
  json += "\"q\":\"" + json_escape(query) + "\",";
  json += "\"num\":" + String(kMaxResults);
  json += "}";

  Serial.println("[search] Serper request: " + url);

  int code = 0;
  String response = http_post(url, json, &code, "X-API-KEY", api_key);

  if (code != 200) {
    error_out = "Serper HTTP " + String(code);
    return false;
  }

  // Parse JSON response
  // Serper format: {"answerBox":{...},"knowledgeGraph":{...},"organic":[{"title":...,"link":...,"snippet":...}]}
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, response);
  if (err != DeserializationError::Ok) {
    error_out = "Serper JSON parse failed";
    return false;
  }

  JsonArray organic = doc["organic"];
  if (organic.isNull()) {
    // No results
    *count = 0;
    return true;
  }

  int idx = 0;
  for (JsonObject result : organic) {
    if (idx >= kMaxResults) break;

    results[idx].title = result["title"] | "";
    results[idx].url = result["link"] | "";
    results[idx].snippet = result["snippet"] | "";

    if (results[idx].title.length() > 0) {
      idx++;
    }
  }

  *count = idx;
  return true;
}

// ============ TAVILY ============

static bool search_tavily(const String &query, const String &api_key,
                          SearchResult *results, int *count,
                          String &error_out) {
  if (api_key.length() == 0) {
    error_out = "Tavily API key not set";
    return false;
  }

  // Build Tavily API request
  String url = resolve_tavily_base_url();
  if (!url.endsWith("/")) url += "/";
  url += "search";

  String json = "{";
  json += "\"api_key\":\"" + json_escape(api_key) + "\",";
  json += "\"query\":\"" + json_escape(query) + "\",";
  json += "\"max_results\":" + String(kMaxResults);
  json += "}";

  Serial.println("[search] Tavily request: " + url);

  int code = 0;
  String response = http_post(url, json, &code);

  if (code != 200) {
    error_out = "Tavily HTTP " + String(code);
    return false;
  }

  // Parse JSON response
  // Tavily format: {"results":[{"title":...,"url":...,"content":...}]}
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, response);
  if (err != DeserializationError::Ok) {
    error_out = "Tavily JSON parse failed";
    return false;
  }

  JsonArray arr = doc["results"];
  if (arr.isNull()) {
    *count = 0;
    return true;
  }

  int idx = 0;
  for (JsonObject result : arr) {
    if (idx >= kMaxResults) break;

    results[idx].title = result["title"] | "";
    results[idx].url = result["url"] | "";
    results[idx].snippet = result["content"] | "";

    if (results[idx].title.length() > 0) {
      idx++;
    }
  }

  *count = idx;
  return true;
}

// ============ MAIN SEARCH FUNCTION ============

bool web_search(const String &query, SearchResult *results_out, int *results_count,
               String &provider_used, String &error_out) {
  if (WiFi.status() != WL_CONNECTED) {
    error_out = "WiFi not connected";
    return false;
  }

  String provider = String(WEB_SEARCH_PROVIDER);
  provider.toLowerCase();
  if (provider.length() == 0) {
    provider = "auto";
  }

  const String serper_key = resolve_serper_key();
  const String tavily_key = resolve_tavily_key();

  const bool provider_allows_serper = (provider == "serper" || provider == "auto");
  const bool provider_allows_tavily = (provider == "tavily" || provider == "auto");

  if (!provider_allows_serper && !provider_allows_tavily) {
    error_out = "Unsupported WEB_SEARCH_PROVIDER: " + provider + " (use auto, serper, or tavily)";
    return false;
  }

  // Default order: Serper -> Tavily.
  if (provider_allows_serper && serper_key.length() > 0) {
    Serial.println("[search] Trying Serper...");
    if (search_serper(query, serper_key, results_out, results_count, error_out)) {
      provider_used = "Serper";
      return true;
    }
    Serial.println("[search] Serper failed: " + error_out);
  }

  if (provider_allows_tavily && tavily_key.length() > 0) {
    Serial.println("[search] Trying Tavily...");
    if (search_tavily(query, tavily_key, results_out, results_count, error_out)) {
      provider_used = "Tavily";
      return true;
    }
  }

  if (provider_allows_serper && serper_key.length() == 0 &&
      provider_allows_tavily && tavily_key.length() == 0) {
    error_out = "No search key found. Set SERPER_API_KEY or TAVILY_API_KEY/WEB_SEARCH_API_KEY.";
  } else if (provider_allows_serper && serper_key.length() == 0 && provider == "serper") {
    error_out = "Serper API key not set";
  } else if (provider_allows_tavily && tavily_key.length() == 0 && provider == "tavily") {
    error_out = "Tavily API key not set";
  }
  return false;
}

bool web_search_simple(const String &query, String &formatted_output, String &error_out) {
  SearchResult results[kMaxResults];
  int count = 0;
  String provider;

  if (!web_search(query, results, &count, provider, error_out)) {
    return false;
  }

  formatted_output = "🔍 " + provider + " results for \"" + query + "\":\n\n";

  for (int i = 0; i < count; i++) {
    formatted_output += String(i + 1) + ". " + results[i].title + "\n";
    formatted_output += "   " + results[i].url + "\n";
    if (results[i].snippet.length() > 0) {
      // Truncate snippet if too long
      String snippet = results[i].snippet;
      if (snippet.length() > 150) {
        snippet = snippet.substring(0, 147) + "...";
      }
      formatted_output += "   \"" + snippet + "\"\n";
    }
    formatted_output += "\n";
  }

  if (count == 0) {
    formatted_output += "No results found.";
  }

  return true;
}
