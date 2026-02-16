#include "transport_telegram.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include "brain_config.h"

static unsigned long s_last_poll_ms = 0;
static long long s_last_update_id = 0;
static const size_t kMaxMediaDownloadBytes = 120000;

static String s_last_chat_id = TELEGRAM_ALLOWED_CHAT_ID;
static String s_last_photo_file_id = "";
static String s_last_photo_mime = "image/jpeg";
static String s_last_document_file_id = "";
static String s_last_document_name = "";
static String s_last_document_mime = "";

static String url_encode(const String &src) {
  static const char *hex = "0123456789ABCDEF";
  String out;
  out.reserve(src.length() * 3);
  for (size_t i = 0; i < src.length(); i++) {
    const unsigned char c = (unsigned char)src[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else if (c == ' ') {
      out += "%20";
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

static String https_get(const String &url, int *status_code) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  if (!https.begin(client, url)) {
    if (status_code) {
      *status_code = -1;
    }
    return String();
  }

  const int code = https.GET();
  if (status_code) {
    *status_code = code;
  }

  String body;
  if (code > 0) {
    body = https.getString();
  }

  https.end();
  return body;
}

static int https_post_raw(const String &url, const String &content_type, const String &body,
                          String *response_out) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  if (!https.begin(client, url)) {
    return -1;
  }

  https.setConnectTimeout(12000);
  https.setTimeout(20000);
  if (content_type.length() > 0) {
    https.addHeader("Content-Type", content_type);
  }

  const int code = https.POST((uint8_t *)body.c_str(), body.length());
  if (response_out != nullptr) {
    if (code > 0) {
      *response_out = https.getString();
    } else {
      *response_out = "";
    }
  }
  https.end();
  return code;
}

static bool extract_int64_after_key(const String &body, const char *key, long long *value_out) {
  const int k = body.indexOf(key);
  if (k < 0) {
    return false;
  }

  int i = k + (int)strlen(key);
  while (i < (int)body.length() && (body[i] == ' ' || body[i] == ':')) {
    i++;
  }

  bool neg = false;
  if (i < (int)body.length() && body[i] == '-') {
    neg = true;
    i++;
  }

  long long v = 0;
  bool any = false;
  while (i < (int)body.length() && body[i] >= '0' && body[i] <= '9') {
    any = true;
    v = v * 10 + (body[i] - '0');
    i++;
  }

  if (!any) {
    return false;
  }

  *value_out = neg ? -v : v;
  return true;
}

static bool extract_number_string_after_key(const String &body, const char *key, String &value_out) {
  const int k = body.indexOf(key);
  if (k < 0) {
    return false;
  }

  int i = k + (int)strlen(key);
  while (i < (int)body.length() && (body[i] == ' ' || body[i] == ':')) {
    i++;
  }
  if (i >= (int)body.length()) {
    return false;
  }

  const int start = i;
  if (body[i] == '-') {
    i++;
  }

  bool any = false;
  while (i < (int)body.length() && body[i] >= '0' && body[i] <= '9') {
    any = true;
    i++;
  }

  if (!any) {
    return false;
  }

  value_out = body.substring(start, i);
  return true;
}

static bool extract_escaped_string_after_key(const String &body, const char *key, String &value_out) {
  const int key_pos = body.indexOf(key);
  if (key_pos < 0) {
    return false;
  }

  const int start = key_pos + (int)strlen(key);
  String text;
  text.reserve(128);

  bool esc = false;
  for (int i = start; i < (int)body.length(); i++) {
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
        case '/':
          text += '/';
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
      value_out = text;
      return true;
    }

    text += c;
  }

  return false;
}

static bool extract_string_after_anchor(const String &body, const char *anchor,
                                        const char *key, String &value_out) {
  const int anchor_pos = body.indexOf(anchor);
  if (anchor_pos < 0) {
    return false;
  }
  return extract_escaped_string_after_key(body.substring(anchor_pos), key, value_out);
}

static bool extract_last_photo_file_id(const String &body, String &file_id_out) {
  const int photo_pos = body.indexOf("\"photo\":[");
  if (photo_pos < 0) {
    return false;
  }

  const int close_pos = body.indexOf(']', photo_pos);
  if (close_pos < 0 || close_pos <= photo_pos) {
    return false;
  }

  String segment = body.substring(photo_pos, close_pos + 1);
  const String key = "\"file_id\":\"";
  int search = 0;
  int found = -1;
  while (true) {
    const int pos = segment.indexOf(key, search);
    if (pos < 0) {
      break;
    }
    found = pos;
    search = pos + (int)key.length();
  }

  if (found < 0) {
    return false;
  }

  return extract_escaped_string_after_key(segment.substring(found), key.c_str(), file_id_out);
}

static bool extract_document_meta(const String &body, String &file_id_out,
                                  String &name_out, String &mime_out) {
  if (!extract_string_after_anchor(body, "\"document\"", "\"file_id\":\"", file_id_out)) {
    return false;
  }
  (void)extract_string_after_anchor(body, "\"document\"", "\"file_name\":\"", name_out);
  (void)extract_string_after_anchor(body, "\"document\"", "\"mime_type\":\"", mime_out);
  return true;
}

static bool extract_file_path_from_getfile(const String &body, String &path_out) {
  return extract_escaped_string_after_key(body, "\"file_path\":\"", path_out);
}

static bool extract_text_field(const String &body, String &text_out) {
  if (extract_escaped_string_after_key(body, "\"text\":\"", text_out)) {
    return true;
  }
  if (extract_escaped_string_after_key(body, "\"caption\":\"", text_out)) {
    return true;
  }
  return false;
}

static bool extract_chat_id(const String &body, String &chat_id_out) {
  if (!extract_number_string_after_key(body, "\"chat\":{\"id\"", chat_id_out)) {
    if (!extract_number_string_after_key(body, "\"chat\": {\"id\"", chat_id_out)) {
      if (!extract_number_string_after_key(body, "\"chat\":{ \"id\"", chat_id_out)) {
        return false;
      }
    }
  }
  return true;
}

static bool is_wifi_ready() {
  return WiFi.status() == WL_CONNECTED;
}

static void ensure_wifi() {
  if (is_wifi_ready()) {
    return;
  }

  Serial.print("[tg] WiFi connect: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 15000UL) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[tg] WiFi connected, IP=");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[tg] WiFi connect timeout");
  }
}

void transport_telegram_init() {
  ensure_wifi();
  Serial.println("[tg] transport initialized");
}

void transport_telegram_send(const String &msg) {
  if (!is_wifi_ready()) {
    ensure_wifi();
    if (!is_wifi_ready()) {
      return;
    }
  }

  const String text = url_encode(msg);
  const String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN +
                     "/sendMessage?chat_id=" + s_last_chat_id +
                     "&text=" + text;

  int code = 0;
  (void)https_get(url, &code);
  Serial.print("[tg] send code=");
  Serial.println(code);
}

bool transport_telegram_send_document(const String &filename, const String &content,
                                      const String &mime_type, const String &caption) {
  if (!is_wifi_ready()) {
    ensure_wifi();
    if (!is_wifi_ready()) {
      return false;
    }
  }

  String safe_name = filename;
  safe_name.trim();
  if (safe_name.length() == 0) {
    safe_name = "file.txt";
  }

  String safe_mime = mime_type;
  safe_mime.trim();
  if (safe_mime.length() == 0) {
    safe_mime = "text/plain";
  }

  String boundary = "----esp32boundary" + String((unsigned long)millis());
  String payload;
  payload.reserve(content.length() + caption.length() + 512);

  payload += "--" + boundary + "\r\n";
  payload += "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n";
  payload += s_last_chat_id + "\r\n";

  if (caption.length() > 0) {
    payload += "--" + boundary + "\r\n";
    payload += "Content-Disposition: form-data; name=\"caption\"\r\n\r\n";
    payload += caption + "\r\n";
  }

  payload += "--" + boundary + "\r\n";
  payload += "Content-Disposition: form-data; name=\"document\"; filename=\"" + safe_name + "\"\r\n";
  payload += "Content-Type: " + safe_mime + "\r\n\r\n";
  payload += content;
  payload += "\r\n--" + boundary + "--\r\n";

  const String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN + "/sendDocument";
  String response;
  const int code = https_post_raw(url, "multipart/form-data; boundary=" + boundary, payload, &response);

  Serial.print("[tg] sendDocument code=");
  Serial.println(code);
  return code >= 200 && code < 300;
}

void transport_telegram_poll(incoming_cb_t cb) {
  if (cb == nullptr) {
    return;
  }

  if ((millis() - s_last_poll_ms) < TELEGRAM_POLL_MS) {
    return;
  }
  s_last_poll_ms = millis();

  if (!is_wifi_ready()) {
    ensure_wifi();
    return;
  }

  const String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN +
                     "/getUpdates?timeout=0&limit=1&offset=" + String(s_last_update_id + 1);

  int code = 0;
  const String body = https_get(url, &code);
  if (code != 200 || body.length() == 0) {
    return;
  }

  long long update_id = 0;
  if (!extract_int64_after_key(body, "\"update_id\"", &update_id)) {
    return;
  }
  if (update_id <= s_last_update_id) {
    return;
  }

  String chat_id;
  if (!extract_chat_id(body, chat_id)) {
    return;
  }

  if (chat_id != String(TELEGRAM_ALLOWED_CHAT_ID)) {
    Serial.println("[tg] rejected message from non-allowlisted chat");
    s_last_update_id = update_id;
    return;
  }

  String photo_file_id;
  if (extract_last_photo_file_id(body, photo_file_id) && photo_file_id.length() > 0) {
    s_last_photo_file_id = photo_file_id;
    s_last_photo_mime = "image/jpeg";
    Serial.println("[tg] cached last photo file id");
  }

  String doc_file_id;
  String doc_name;
  String doc_mime;
  if (extract_document_meta(body, doc_file_id, doc_name, doc_mime) && doc_file_id.length() > 0) {
    s_last_document_file_id = doc_file_id;
    s_last_document_name = doc_name;
    s_last_document_mime = doc_mime;
    Serial.println("[tg] cached last document file id");
  }

  String text;
  const bool has_text = extract_text_field(body, text);

  s_last_chat_id = chat_id;
  s_last_update_id = update_id;
  if (!has_text) {
    return;
  }
  cb(text);
}

namespace {

static int base64_char_value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static size_t base64_decode(const String &input, uint8_t *output, size_t output_size) {
  size_t input_len = input.length();
  size_t output_index = 0;

  for (size_t i = 0; i < input_len && output_index < output_size; i += 4) {
    int values[4];
    int valid_count = 0;

    for (int j = 0; j < 4 && (i + j) < input_len; j++) {
      char c = input[i + j];
      if (c == '=') {
        values[j] = 0;
        valid_count++;
      } else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
        continue;
      } else {
        int val = base64_char_value(c);
        if (val < 0) continue;
        values[j] = val;
        valid_count++;
      }
    }

    if (valid_count >= 2) {
      if (output_index < output_size) {
        output[output_index++] = (values[0] << 2) | ((values[1] & 0x30) >> 4);
      }
      if (valid_count >= 3 && output_index < output_size) {
        output[output_index++] = ((values[1] & 0x0F) << 4) | ((values[2] & 0x3C) >> 2);
      }
      if (valid_count >= 4 && output_index < output_size) {
        output[output_index++] = ((values[2] & 0x03) << 6) | values[3];
      }
    }
  }

  return output_index;
}

static size_t base64_decoded_size(const String &input) {
  size_t len = input.length();
  size_t padding = 0;
  if (len > 0 && input[len - 1] == '=') padding++;
  if (len > 1 && input[len - 2] == '=') padding++;
  return (len * 3) / 4 - padding;
}

static String base64_encode(const uint8_t *data, size_t len) {
  static const char *alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  String out;
  out.reserve(((len + 2) / 3) * 4);

  for (size_t i = 0; i < len; i += 3) {
    const uint32_t b0 = data[i];
    const uint32_t b1 = (i + 1 < len) ? data[i + 1] : 0;
    const uint32_t b2 = (i + 2 < len) ? data[i + 2] : 0;
    const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;

    out += alphabet[(triple >> 18) & 0x3F];
    out += alphabet[(triple >> 12) & 0x3F];
    out += (i + 1 < len) ? alphabet[(triple >> 6) & 0x3F] : '=';
    out += (i + 2 < len) ? alphabet[triple & 0x3F] : '=';
  }

  return out;
}

static bool fetch_file_path_by_id(const String &file_id, String &path_out, String &error_out) {
  const String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN +
                     "/getFile?file_id=" + url_encode(file_id);
  int code = 0;
  const String body = https_get(url, &code);
  if (code != 200) {
    error_out = "getFile HTTP " + String(code);
    return false;
  }
  if (!extract_file_path_from_getfile(body, path_out) || path_out.length() == 0) {
    error_out = "Could not parse Telegram file path";
    return false;
  }
  return true;
}

static bool fetch_binary_file(const String &url, uint8_t **data_out, size_t *len_out,
                              String &error_out) {
  *data_out = nullptr;
  *len_out = 0;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  if (!https.begin(client, url)) {
    error_out = "HTTP begin failed";
    return false;
  }

  https.setConnectTimeout(12000);
  https.setTimeout(30000);
  const int code = https.GET();
  if (code < 200 || code >= 300) {
    https.end();
    error_out = "download HTTP " + String(code);
    return false;
  }

  const int total = https.getSize();
  if (total <= 0) {
    https.end();
    error_out = "Unknown file size";
    return false;
  }
  if ((size_t)total > kMaxMediaDownloadBytes) {
    https.end();
    error_out = "File too large for ESP32 (" + String(total) + " bytes)";
    return false;
  }

  uint8_t *buffer = (uint8_t *)malloc((size_t)total);
  if (!buffer) {
    https.end();
    error_out = "Out of memory downloading file";
    return false;
  }

  WiFiClient *stream = https.getStreamPtr();
  size_t offset = 0;
  unsigned long start_ms = millis();
  while (https.connected() && offset < (size_t)total) {
    size_t avail = stream->available();
    if (avail == 0) {
      if ((millis() - start_ms) > 15000UL) {
        break;
      }
      delay(2);
      continue;
    }
    const size_t to_read = ((size_t)total - offset < avail) ? ((size_t)total - offset) : avail;
    const int read_count = stream->readBytes(buffer + offset, to_read);
    if (read_count <= 0) {
      break;
    }
    offset += (size_t)read_count;
  }

  https.end();

  if (offset != (size_t)total) {
    free(buffer);
    error_out = "Incomplete file download";
    return false;
  }

  *data_out = buffer;
  *len_out = offset;
  return true;
}

static bool telegram_file_to_base64(const String &file_id, String &base64_out, String &error_out) {
  String file_path;
  if (!fetch_file_path_by_id(file_id, file_path, error_out)) {
    return false;
  }

  const String file_url = String("https://api.telegram.org/file/bot") + TELEGRAM_BOT_TOKEN +
                          "/" + file_path;

  uint8_t *data = nullptr;
  size_t len = 0;
  if (!fetch_binary_file(file_url, &data, &len, error_out)) {
    return false;
  }

  base64_out = base64_encode(data, len);
  free(data);

  if (base64_out.length() == 0) {
    error_out = "Failed to base64 encode file";
    return false;
  }
  return true;
}

}  // namespace

bool transport_telegram_get_last_photo_base64(String &mime_out, String &base64_out,
                                              String &error_out) {
  if (!is_wifi_ready()) {
    ensure_wifi();
    if (!is_wifi_ready()) {
      error_out = "WiFi not connected";
      return false;
    }
  }

  if (s_last_photo_file_id.length() == 0) {
    error_out = "No recent photo found. Send a photo first.";
    return false;
  }

  if (!telegram_file_to_base64(s_last_photo_file_id, base64_out, error_out)) {
    return false;
  }

  mime_out = s_last_photo_mime;
  if (mime_out.length() == 0) {
    mime_out = "image/jpeg";
  }
  return true;
}

bool transport_telegram_get_last_document_base64(String &filename_out, String &mime_out,
                                                 String &base64_out, String &error_out) {
  if (!is_wifi_ready()) {
    ensure_wifi();
    if (!is_wifi_ready()) {
      error_out = "WiFi not connected";
      return false;
    }
  }

  if (s_last_document_file_id.length() == 0) {
    error_out = "No recent document found. Send a document first.";
    return false;
  }

  if (!telegram_file_to_base64(s_last_document_file_id, base64_out, error_out)) {
    return false;
  }

  filename_out = s_last_document_name;
  mime_out = s_last_document_mime;
  if (mime_out.length() == 0) {
    mime_out = "application/octet-stream";
  }
  return true;
}

bool transport_telegram_send_photo_base64(const String &base64_data, const String &caption) {
  if (!is_wifi_ready()) {
    ensure_wifi();
    if (!is_wifi_ready()) {
      return false;
    }
  }

  const size_t decoded_len = base64_decoded_size(base64_data);
  if (decoded_len > 200000) {
    Serial.println("[tg] Image too large, skipping");
    return false;
  }

  uint8_t *binary_data = (uint8_t *)malloc(decoded_len);
  if (!binary_data) {
    Serial.println("[tg] Failed to allocate memory for photo");
    return false;
  }

  const size_t actual_len = base64_decode(base64_data, binary_data, decoded_len);

  String boundary = "----esp32photoboundary" + String((unsigned long)millis());

  String headers;
  headers.reserve(512);
  headers += "--" + boundary + "\r\n";
  headers += "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n";
  headers += s_last_chat_id + "\r\n";

  if (caption.length() > 0) {
    headers += "--" + boundary + "\r\n";
    headers += "Content-Disposition: form-data; name=\"caption\"\r\n\r\n";
    headers += caption + "\r\n";
  }

  headers += "--" + boundary + "\r\n";
  headers += "Content-Disposition: form-data; name=\"photo\"; filename=\"generated.png\"\r\n";
  headers += "Content-Type: image/png\r\n\r\n";

  String footer = "\r\n--" + boundary + "--\r\n";

  const size_t total_size = headers.length() + actual_len + footer.length();

  uint8_t *payload = (uint8_t *)malloc(total_size);
  if (!payload) {
    free(binary_data);
    Serial.println("[tg] Failed to allocate memory for photo payload");
    return false;
  }

  size_t offset = 0;
  memcpy(payload + offset, headers.c_str(), headers.length());
  offset += headers.length();
  memcpy(payload + offset, binary_data, actual_len);
  offset += actual_len;
  memcpy(payload + offset, footer.c_str(), footer.length());

  free(binary_data);

  const String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN + "/sendPhoto";
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  if (!https.begin(client, url)) {
    free(payload);
    return false;
  }

  https.setConnectTimeout(12000);
  https.setTimeout(30000);
  https.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

  const int code = https.POST(payload, total_size);
  free(payload);

  Serial.print("[tg] sendPhoto code=");
  Serial.println(code);

  https.end();
  return code >= 200 && code < 300;
}
