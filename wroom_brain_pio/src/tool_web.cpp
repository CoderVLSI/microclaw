#include "tool_web.h"
#include "brain_config.h"
#include "llm_client.h"
#include "web_search.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

// Helper for HTTP generic get
static String http_get(const String &url, const String &header_name = "", const String &header_val = "", int *code_out = nullptr) {
  if (WiFi.status() != WL_CONNECTED) return "";
  
  WiFiClientSecure client;
  client.setInsecure(); // Simplify certs
  HTTPClient http;
  
  if (!http.begin(client, url)) return "";
  
  if (header_name.length() > 0) {
    http.addHeader(header_name, header_val);
  }
  
  int code = http.GET();
  if (code_out) *code_out = code;
  
  String payload = "";
  if (code > 0) {
    payload = http.getString();
  }
  http.end();
  return payload;
}

// ======================================================================================
// SEARCH IMPLEMENTATION
// ======================================================================================

static String trim_for_search_output(String value, size_t max_chars) {
  value.replace('\n', ' ');
  value.replace('\r', ' ');
  value.trim();
  if (value.length() <= max_chars) {
    return value;
  }
  if (max_chars < 4) {
    return value.substring(0, max_chars);
  }
  return value.substring(0, max_chars - 3) + "...";
}

static String build_sources_block(const SearchResult *results, int count, int max_items) {
  String out = "Sources:\n";
  const int limit = (count < max_items) ? count : max_items;
  for (int i = 0; i < limit; i++) {
    out += String(i + 1) + ". " + trim_for_search_output(results[i].title, 110) + "\n";
    out += "   " + trim_for_search_output(results[i].url, 220) + "\n";
  }
  return out;
}

static String build_result_pack_for_llm(const SearchResult *results, int count, int max_items) {
  String out;
  const int limit = (count < max_items) ? count : max_items;
  for (int i = 0; i < limit; i++) {
    out += "[" + String(i + 1) + "] " + trim_for_search_output(results[i].title, 140) + "\n";
    out += "URL: " + trim_for_search_output(results[i].url, 220) + "\n";
    String snippet = trim_for_search_output(results[i].snippet, 320);
    if (snippet.length() > 0) {
      out += "Snippet: " + snippet + "\n";
    }
    out += "\n";
  }
  return out;
}

static bool summarize_web_results_with_llm(const String &query, const String &provider,
                                           const SearchResult *results, int count,
                                           String &summary_out) {
  const String system_prompt =
      "You summarize web search results for a personal AI assistant. "
      "Use only the provided snippets. Do not invent facts. "
      "If evidence is weak or conflicting, say that clearly.";

  String task = "User query: " + query + "\n";
  task += "Search provider used: " + provider + "\n\n";
  task += "Search snippets:\n";
  task += build_result_pack_for_llm(results, count, 5);
  task += "Respond in this format:\n";
  task += "1) Direct answer (2-4 sentences)\n";
  task += "2) Key takeaways (max 5 bullets)\n";
  task += "3) What is uncertain (1-3 bullets)\n";
  task += "4) Cite evidence as [1], [2], etc.\n";

  String llm_err;
  if (!llm_generate_with_custom_prompt(system_prompt, task, false, summary_out, llm_err)) {
    return false;
  }
  summary_out.trim();
  if (summary_out.length() == 0) {
    return false;
  }
  if (summary_out.length() > 3200) {
    summary_out = summary_out.substring(0, 3200);
    summary_out += "\n...(truncated)";
  }
  return true;
}

static String build_non_llm_summary(const String &query, const String &provider,
                                    const SearchResult *results, int count) {
  String out = "Search summary for \"" + query + "\" (" + provider + ")\n";
  out += "Top findings:\n";
  const int limit = (count < 3) ? count : 3;
  for (int i = 0; i < limit; i++) {
    out += "- " + trim_for_search_output(results[i].title, 120) + ": ";
    String snippet = trim_for_search_output(results[i].snippet, 200);
    if (snippet.length() == 0) {
      snippet = "No snippet text provided.";
    }
    out += snippet + "\n";
  }
  out += "\n" + build_sources_block(results, count, 5);
  return out;
}

bool tool_web_search(const String &query, String &output_out) {
  SearchResult results[10];
  int count = 0;
  String provider;
  String error;

  if (!web_search(query, results, &count, provider, error)) {
    output_out = "ERR: " + error;
    return false;
  }

  if (count <= 0) {
    output_out = "No relevant web results found for: " + query;
    return true;
  }

  String llm_summary;
  if (summarize_web_results_with_llm(query, provider, results, count, llm_summary)) {
    output_out = llm_summary + "\n\n" + build_sources_block(results, count, 5);
    return true;
  }

  output_out = build_non_llm_summary(query, provider, results, count);
  return true;
}


// ======================================================================================
// WEATHER IMPLEMENTATION
// ======================================================================================

bool tool_web_weather(const String &location, String &output_out) {
  // 1. Geocode
  String locEnc = location;
  locEnc.replace(" ", "%20");
  String geoUrl = "https://geocoding-api.open-meteo.com/v1/search?name=" + locEnc + "&count=1&language=en&format=json";
  
  int code = 0;
  String geoResp = http_get(geoUrl, "", "", &code);
  
  if (code != 200) {
    output_out = "Weather Geocode Error: HTTP " + String(code);
    return false;
  }
  
  JsonDocument geoDoc;
  if (deserializeJson(geoDoc, geoResp)) {
    output_out = "Weather Geocode JSON Error";
    return false;
  }
  
  JsonArray results = geoDoc["results"];
  if (results.isNull() || results.size() == 0) {
    output_out = "Location not found: " + location;
    return false;
  }
  
  double lat = results[0]["latitude"];
  double lon = results[0]["longitude"];
  const char* name = results[0]["name"];
  const char* country = results[0]["country"];
  
  // 2. Weather
  String weatherUrl = "https://api.open-meteo.com/v1/forecast?latitude=" + String(lat, 4) + "&longitude=" + String(lon, 4) + 
                      "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto";

  String weatherResp = http_get(weatherUrl, "", "", &code);
  if (code != 200) {
    output_out = "Weather API Error: " + String(code);
    return false;
  }
  
  JsonDocument wDoc;
  if (deserializeJson(wDoc, weatherResp)) {
    output_out = "Weather JSON Error";
    return false;
  }
  
  JsonObject current = wDoc["current"];
  float temp = current["temperature_2m"];
  int hum = current["relative_humidity_2m"];
  int wcode = current["weather_code"];
  float wind = current["wind_speed_10m"];
  
  String wdesc = "Unknown";
  // Simple WMO code mapping
  if (wcode == 0) wdesc = "Clear sky";
  else if (wcode <= 3) wdesc = "Partly cloudy";
  else if (wcode <= 48) wdesc = "Fog";
  else if (wcode <= 55) wdesc = "Drizzle";
  else if (wcode <= 67) wdesc = "Rain";
  else if (wcode <= 77) wdesc = "Snow";
  else if (wcode <= 82) wdesc = "Rain showers";
  else if (wcode <= 99) wdesc = "Thunderstorm";
  
  output_out = "Weather for " + String(name) + ", " + String(country) + ":\n" +
               "Condition: " + wdesc + "\n" +
               "Temp: " + String(temp) + "C\n" +
               "Humidity: " + String(hum) + "%\n" +
               "Wind: " + String(wind) + " km/h";
               
  return true;
}

// ======================================================================================
// TIME IMPLEMENTATION
// ======================================================================================

bool tool_web_time(String &output_out) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    output_out = "Time not synced (NTP failure)";
    return false;
  }
  
  char buffer[64];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S %Z", &timeinfo);
  output_out = String(buffer);
  return true;
}

