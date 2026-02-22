#include "web_search.h"

#include "brain_config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static const int kMaxResults = 10;
static const int kTimeoutMs = WEB_SEARCH_TIMEOUT_MS;

// HTTP GET helper
static String http_get(const String &url, int *status_code) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  if (!https.begin(client, url)) {
    if (status_code) *status_code = -1;
    return String();
  }

  https.setConnectTimeout(10000);
  https.setTimeout(kTimeoutMs);

  const int code = https.GET();
  if (status_code) *status_code = code;

  String body;
  if (code > 0) {
    body = https.getString();
  }

  https.end();
  return body;
}

// HTTP POST helper (for Serper)
static String http_post(const String &url, const String &json_body, int *status_code) {
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
  if (strlen(WEB_SEARCH_API_KEY) > 0) {
    https.addHeader("X-API-KEY", WEB_SEARCH_API_KEY);
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

// ============ SERPER (Google Search API) ============

static bool search_serper(const String &query, SearchResult *results, int *count,
                          String &error_out) {
  if (strlen(WEB_SEARCH_API_KEY) == 0) {
    error_out = "Serper API key not set";
    return false;
  }

  // Build Serper API request
  String url = String(WEB_SEARCH_BASE_URL);
  if (!url.endsWith("/")) url += "/";
  url += "search";

  String json = "{";
  json += "\"q\":\"" + json_escape(query) + "\",";
  json += "\"num\":" + String(kMaxResults);
  json += "}";

  Serial.println("[search] Serper request: " + url);

  int code = 0;
  String response = http_post(url, json, &code);

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

static bool search_tavily(const String &query, SearchResult *results, int *count,
                          String &error_out) {
  if (strlen(WEB_SEARCH_API_KEY) == 0) {
    error_out = "Tavily API key not set";
    return false;
  }

  // Build Tavily API request
  String url = String(WEB_SEARCH_BASE_URL);
  if (!url.endsWith("/")) url += "/";
  url += "search";

  String json = "{";
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

  // Try Serper first (if set or auto)
  if (provider == "serper" || provider == "auto") {
    if (strlen(WEB_SEARCH_API_KEY) > 0) {
      String url = String(WEB_SEARCH_BASE_URL);
      url.toLowerCase();
      if (url.indexOf("serper") >= 0 || provider == "serper") {
        Serial.println("[search] Trying Serper...");
        if (search_serper(query, results_out, results_count, error_out)) {
          provider_used = "Serper";
          return true;
        }
        Serial.println("[search] Serper failed: " + error_out);
      }
    }
  }

  // Fallback to Tavily
  Serial.println("[search] Trying Tavily...");
  if (search_tavily(query, results_out, results_count, error_out)) {
    provider_used = "Tavily";
    return true;
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
