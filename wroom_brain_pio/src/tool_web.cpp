#include "tool_web.h"
#include "brain_config.h"
#include "model_config.h"
#include "llm_client.h" // for json_escape & http helpers if available? No, helpers are private/static in llm_client.cpp. Use standard HTTPClient.

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

// Helper for HTTP generic post
static String http_post(const String &url, const String &body, const String &header_name = "", const String &header_val = "", int *code_out = nullptr) {
  if (WiFi.status() != WL_CONNECTED) return "";
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  
  if (!http.begin(client, url)) return "";
  http.addHeader("Content-Type", "application/json");
  
  if (header_name.length() > 0) {
    http.addHeader(header_name, header_val);
  }
  
  int code = http.POST(body);
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

static bool search_tavily(const String &key, const String &query, String &out) {
  String url = "https://api.tavily.com/search";
  
  // JSON body for Tavily
  JsonDocument doc;
  doc["api_key"] = key;
  doc["query"] = query;
  doc["search_depth"] = "basic";
  doc["include_answer"] = true;
  doc["max_results"] = 3;
  
  String body;
  serializeJson(doc, body);
  
  int code = 0;
  String resp = http_post(url, body, "", "", &code);
  
  if (code < 200 || code >= 300) {
    out = "Tavily Error: HTTP " + String(code);
    return false;
  }
  
  JsonDocument resDoc;
  DeserializationError err = deserializeJson(resDoc, resp);
  if (err) {
    out = "Tavily JSON Error";
    return false;
  }
  
  String answer = resDoc["answer"].as<String>();
  if (answer != "null" && answer.length() > 5) {
    out = "Tavily Answer: " + answer + "\n\nSources:\n";
  } else {
    out = "Results:\n";
  }
  
  JsonArray results = resDoc["results"];
  for (JsonObject r : results) {
    const char* title = r["title"];
    const char* url = r["url"];
    const char* content = r["content"];
    
    out += "- [" + String(title) + "](" + String(url) + "): " + String(content).substring(0, 150) + "...\n";
  }
  
  return true;
}

static bool search_brave(const String &key, const String &query, String &out) {
  // Brave Search API GET
  // Need to URL encode query. A simple replacement for spaces is usually enough for basic queries, but better to be safe.
  String encQuery = query;
  encQuery.replace(" ", "%20");
  encQuery.replace("\"", "%22");
  encQuery.replace("&", "%26");
  encQuery.replace("?", "%3F");
  
  String url = "https://api.search.brave.com/res/v1/web/search?q=" + encQuery + "&count=3";
  
  int code = 0;
  String resp = http_get(url, "X-Subscription-Token", key, &code);
  
  if (code < 200 || code >= 300) {
    out = "Brave Search Error: HTTP " + String(code);
    return false;
  }
  
  JsonDocument resDoc;
  DeserializationError err = deserializeJson(resDoc, resp);
  if (err) {
    out = "Brave JSON Error";
    return false;
  }
  
  // Parse 'web' -> 'results'
  JsonObject web = resDoc["web"];
  JsonArray results = web["results"];
  if (results.isNull()) {
    out = "No results found.";
    return true;
  }
  
  out = "Brave Results:\n";
  for (JsonObject r : results) {
    const char* title = r["title"];
    const char* url = r["url"];
    const char* desc = r["description"];
    
    out += "- [" + String(title ? title : "No Title") + "](" + String(url ? url : "") + ")\n  " + String(desc ? desc : "").substring(0, 200) + "\n";
  }
  
  return true;
}

bool tool_web_search(const String &query, String &output_out) {
  // Determine provider: Tavily > Brave (if keys exist)
  // Check Config 
  // User can set via /config set brave_key ... or tavily_key ...
  
  String tavily_key = model_config_get_api_key("tavily");
  if (tavily_key.length() == 0) tavily_key = String(TAVILY_API_KEY);
  
  String brave_key = model_config_get_api_key("brave");
  if (brave_key.length() == 0) brave_key = String(WEB_SEARCH_API_KEY);
  
  // Logic: Prefer Tavily if available (User said "stupid brave free tier gone")
  if (tavily_key.length() > 5) {
    return search_tavily(tavily_key, query, output_out);
  }
  
  if (brave_key.length() > 5) {
    return search_brave(brave_key, query, output_out);
  }
  
  output_out = "No Search API Key found! Please set 'brave' or 'tavily' key using /config set <provider>_key <value>";
  return false;
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

