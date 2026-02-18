#include "web_server.h"

#include <Arduino.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

#include "transport_telegram.h"
#include "brain_config.h"
#include "model_config.h"
#include "agent_loop.h"
#include "chat_history.h"

namespace {

AsyncWebServer *g_server = nullptr;
bool g_initialized = false;

// ======================================================================================
// HELPERS
// ======================================================================================

void send_json(AsyncWebServerRequest *request, const JsonDocument &doc) {
  String response;
  serializeJson(doc, response);
  request->send(200, "application/json", response);
}

void send_error(AsyncWebServerRequest *request, int code, const String &msg) {
  JsonDocument doc;
  doc["error"] = msg;
  send_json(request, doc);
}

// ======================================================================================
// API HANDLERS
// ======================================================================================

// GET /api/status
void handle_api_status(AsyncWebServerRequest *request) {
  JsonDocument doc;
  doc["uptime"] = millis() / 1000;
  doc["rssi"] = WiFi.RSSI();
  doc["heap_free"] = ESP.getFreeHeap();
  doc["ip"] = WiFi.localIP().toString();
  doc["ssid"] = WiFi.SSID();
  
  doc["model_provider"] = model_config_get_active_provider();
  doc["model_name"] = model_config_get_model(model_config_get_active_provider());
  
  send_json(request, doc);
}

// GET /api/config
void handle_api_config_get(AsyncWebServerRequest *request) {
  JsonDocument doc;
  
  // Return masked keys for display
  String providers[] = {"openai", "anthropic", "gemini", "tavily", "brave"};
  JsonObject keys = doc["api_keys"].to<JsonObject>();
  
  for (const String &p : providers) {
    String key = model_config_get_api_key(p);
    if (key.length() > 4) {
      keys[p] = "..." + key.substring(key.length() - 4);
    } else if (key.length() > 0) {
      keys[p] = "***";
    } else {
      keys[p] = "";
    }
  }
  
  JsonObject models = doc["models"].to<JsonObject>();
  for (const String &p : providers) {
      models[p] = model_config_get_model(p);
  }
  
  send_json(request, doc);
}

// POST /api/config
// Body: { "provider": "openai", "key": "sk-...", "model": "gpt-4" }
void handle_api_config_set(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, (const char*)data, len);
  
  if (error) {
    send_error(request, 400, "Invalid JSON");
    return;
  }
  
  const char* provider = doc["provider"];
  const char* key = doc["key"];
  const char* model = doc["model"];
  
  if (!provider) {
    send_error(request, 400, "Missing provider");
    return;
  }
  
  String provStr = String(provider);
  String err;
  
  if (key && strlen(key) > 0) {
    if (!model_config_set_api_key(provStr, String(key), err)) {
      send_error(request, 500, "Failed to set key: " + err);
      return;
    }
  }
  
  if (model && strlen(model) > 0) {
    if (!model_config_set_model(provStr, String(model), err)) {
      send_error(request, 500, "Failed to set model: " + err);
      return;
    }
  }
  
  send_json(request, doc);
}

// GET /api/chat
void handle_api_chat_history(AsyncWebServerRequest *request) {
  String history, err;
  if (chat_history_get(history, err)) {
    // Wrap in JSON
    JsonDocument doc;
    doc["history"] = history;
    send_json(request, doc);
  } else {
    send_error(request, 500, "Failed to read history");
  }
}

// POST /api/chat
// Body: { "message": "Hello" }
void handle_api_chat_send(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, (const char*)data, len);
  
  if (error) {
    send_error(request, 400, "Invalid JSON");
    return;
  }
  
  const char* msg = doc["message"];
  if (!msg || strlen(msg) == 0) {
    send_error(request, 400, "Empty message");
    return;
  }
  
  // Send to agent loop (async queue)
  // Non-blocking to avoid WDT reset on AsyncWebServer thread
  agent_loop_queue_message(String(msg));
  
  JsonDocument resDoc;
  resDoc["status"] = "queued";
  send_json(request, resDoc);
}


// Handle static file requests (Fallback)
void handle_static_file(AsyncWebServerRequest *request) {
  String path = request->url();

  if (path == "/") {
    path = "/index.html";
  }

  if (SPIFFS.exists(path)) {
    String mime_type = "text/plain";
    if (path.endsWith(".html")) mime_type = "text/html";
    else if (path.endsWith(".css")) mime_type = "text/css";
    else if (path.endsWith(".js")) mime_type = "application/javascript";
    else if (path.endsWith(".json")) mime_type = "application/json";
    
    request->send(SPIFFS, path, mime_type);
  } else {
    // 404
    request->send(404, "text/plain", "File not found");
  }
}

}  // namespace

void web_server_init() {
  if (g_initialized) {
    return;
  }

  if (!SPIFFS.begin(true)) {
    Serial.println("[web] SPIFFS mount failed");
    return;
  }
  Serial.println("[web] SPIFFS mounted");

  g_server = new AsyncWebServer(80);

  // API
  g_server->on("/api/status", HTTP_GET, handle_api_status);
  g_server->on("/api/config", HTTP_GET, handle_api_config_get);
  
  // POST handlers need body parser
  g_server->on("/api/config", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, handle_api_config_set);
  g_server->on("/api/chat", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, handle_api_chat_send);
  g_server->on("/api/chat", HTTP_GET, handle_api_chat_history);

  // Static
  g_server->onNotFound(handle_static_file);

  g_server->begin();
  g_initialized = true;

  String ip = WiFi.localIP().toString();
  Serial.println("[web] Server started at http://" + ip + "/");
}

String web_server_get_url() {
  if (!g_initialized) return "";
  return "http://" + WiFi.localIP().toString() + "/";
}

bool web_server_publish_file(const String &filename, const String &content, const String &mime_type) {
  if (!g_initialized) return false;
  
  String path = "/" + filename;
  if (!path.startsWith("/")) path = "/" + path;
  
  File file = SPIFFS.open(path, "w");
  if (!file) return false;
  
  size_t written = file.print(content);
  file.close();
  return written > 0;
}
