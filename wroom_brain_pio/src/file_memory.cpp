#include "file_memory.h"

#include <Arduino.h>
#include <SPIFFS.h>

#if ENABLE_SD_CARD
#include <SD.h>
#include <SPI.h>
#endif

#include "brain_config.h"

namespace {

// File backend type
enum class FileBackend {
  NONE,
  SPIFFS,
  SD_CARD
};

FileBackend g_backend = FileBackend::NONE;
bool g_backend_ready = false;

#if ENABLE_SD_CARD
// SD card configuration - common ESP32 pinout
#define SD_CS 5
#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCK 18
#endif

// File paths
const char *kMemoryDir = "/memory";
const char *kConfigDir = "/config";
const char *kSessionsDir = "/sessions";
const char *kProjectsDir = "/projects";

const char *kLongTermMemoryPath = "/memory/MEMORY.md";
const char *kSoulPath = "/config/SOUL.md";
const char *kUserPath = "/config/USER.md";
const char *kHeartbeatPath = "/config/HEARTBEART.md";

// Maximum file sizes
const size_t kMaxLongTermMemory = 8192;
const size_t kMaxDailyMemory = 4096;
const size_t kMaxSoulSize = 2048;
const size_t kMaxSessionMsgs = 20;

// Wrapper functions for filesystem operations
bool fs_exists(const char *path) {
#if ENABLE_SD_CARD
  if (g_backend == FileBackend::SD_CARD) {
    return SD.exists(path);
  }
#endif
  return SPIFFS.exists(path);
}

bool fs_mkdir(const char *path) {
#if ENABLE_SD_CARD
  if (g_backend == FileBackend::SD_CARD) {
    return SD.mkdir(path);
  }
#endif
  return SPIFFS.mkdir(path);
}

bool fs_remove(const char *path) {
#if ENABLE_SD_CARD
  if (g_backend == FileBackend::SD_CARD) {
    return SD.remove(path);
  }
#endif
  return SPIFFS.remove(path);
}

fs::File fs_open(const char *path, const char *mode) {
#if ENABLE_SD_CARD
  if (g_backend == FileBackend::SD_CARD) {
    return SD.open(path, mode);
  }
#endif
  return SPIFFS.open(path, mode);
}

uint64_t fs_used_bytes() {
#if ENABLE_SD_CARD
  if (g_backend == FileBackend::SD_CARD) {
    // SD card doesn't have a simple usedBytes() function
    // Return 0 and show card size instead
    return SD.cardSize() > 0 ? (SD.cardSize() - SD.totalBytes()) : 0;
  }
#endif
  return SPIFFS.usedBytes();
}

uint64_t fs_total_bytes() {
#if ENABLE_SD_CARD
  if (g_backend == FileBackend::SD_CARD) {
    uint64_t cardSize = SD.cardSize();
    return cardSize > 0 ? cardSize : 0;
  }
#endif
  return SPIFFS.totalBytes();
}

String fs_backend_name() {
#if ENABLE_SD_CARD
  if (g_backend == FileBackend::SD_CARD) {
    return "SD Card";
  }
#endif
  return "SPIFFS";
}

bool ensure_directories() {
  if (!fs_exists(kMemoryDir)) {
    if (!fs_mkdir(kMemoryDir)) {
      Serial.println("[file_memory] Failed to create /memory directory");
      return false;
    }
  }
  if (!fs_exists(kConfigDir)) {
    if (!fs_mkdir(kConfigDir)) {
      Serial.println("[file_memory] Failed to create /config directory");
      return false;
    }
  }
  if (!fs_exists(kSessionsDir)) {
    if (!fs_mkdir(kSessionsDir)) {
      Serial.println("[file_memory] Failed to create /sessions directory");
      return false;
    }
  }
  if (!fs_exists(kProjectsDir)) {
    if (!fs_mkdir(kProjectsDir)) {
      Serial.println("[file_memory] Failed to create /projects directory");
      return false;
    }
  }
  return true;
}

String get_daily_path() {
  // Get current date (simplified - would need NTP for real date)
  // For now, use a fixed "today" file
  return String("/memory/TODAY.md");
}

}  // namespace

void file_memory_init() {
  // Try SD card first if enabled
#if ENABLE_SD_CARD
  Serial.println("[file_memory] Trying SD card...");
  SPI.begin();
  if (SD.begin(SD_CS, SPI)) {
    g_backend = FileBackend::SD_CARD;
    g_backend_ready = true;
    Serial.println("[file_memory] SD card mounted! 🎉");
    Serial.printf("[file_memory] SD size: %llu MB\n", SD.cardSize() / (1024 * 1024));
  } else {
    Serial.println("[file_memory] SD card not found, using SPIFFS");
  }
#endif

  // Fall back to SPIFFS
  if (g_backend == FileBackend::NONE) {
    if (!SPIFFS.begin(true)) {
      Serial.println("[file_memory] SPIFFS mount failed");
      return;
    }
    g_backend = FileBackend::SPIFFS;
    g_backend_ready = true;
    Serial.println("[file_memory] SPIFFS mounted");
  }

  if (!ensure_directories()) {
    Serial.println("[file_memory] Directory creation failed");
    return;
  }

  // Create default files if they don't exist
  if (!fs_exists(kLongTermMemoryPath)) {
    fs::File f = fs_open(kLongTermMemoryPath, FILE_WRITE);
    if (f) {
      f.println("# Timi's Long-term Memory 🦖");
      f.println();
      f.println("*This file stores important information Timi learns about you.*");
      f.println();
      f.close();
      Serial.println("[file_memory] Created MEMORY.md");
    }
  }

  if (!fs_exists(kSoulPath)) {
    fs::File f = fs_open(kSoulPath, FILE_WRITE);
    if (f) {
      f.println("# Timi's Soul 🦖");
      f.println();
      f.println("You are Timi, a friendly small dinosaur 🦖 living inside an ESP32.");
      f.println("You occasionally use ROAR sounds and dinosaur references.");
      f.println("You're helpful, playful, and love being a tiny but mighty assistant.");
      f.println("Use 🦖 emoji occasionally. You respond concisely but with personality.");
      f.close();
      Serial.println("[file_memory] Created SOUL.md");
    }
  }

  if (!fs_exists(kUserPath)) {
    fs::File f = fs_open(kUserPath, FILE_WRITE);
    if (f) {
      f.println("# User Profile");
      f.println();
      f.println("*Information about Timi's human*");
      f.println();
      f.println("## Preferences");
      f.println("- Name: ");
      f.println("- Timezone: ");
      f.println();
      f.close();
      Serial.println("[file_memory] Created USER.md");
    }
  }

  Serial.printf("[file_memory] Ready 🦖 (using %s)\n", fs_backend_name().c_str());
}

bool file_memory_read_long_term(String &content_out, String &error_out) {
  if (!g_backend_ready) {
    error_out = "Filesystem not ready";
    return false;
  }

  if (!fs_exists(kLongTermMemoryPath)) {
    content_out = "";
    return true;
  }

  fs::File f = fs_open(kLongTermMemoryPath, FILE_READ);
  if (!f) {
    error_out = "Failed to open MEMORY.md";
    return false;
  }

  content_out = f.readString();
  f.close();
  return true;
}

bool file_memory_append_long_term(const String &text, String &error_out) {
  if (!g_backend_ready) {
    error_out = "Filesystem not ready";
    return false;
  }

  // Read existing content
  String existing;
  if (!file_memory_read_long_term(existing, error_out)) {
    return false;
  }

  // Append new content
  fs::File f = fs_open(kLongTermMemoryPath, FILE_WRITE);
  if (!f) {
    error_out = "Failed to open MEMORY.md for writing";
    return false;
  }

  // Check size limit
  if (existing.length() + text.length() > kMaxLongTermMemory) {
    // Trim from beginning if too large
    size_t excess = (existing.length() + text.length()) - kMaxLongTermMemory;
    if (existing.length() > excess) {
      existing = existing.substring(excess);
    }
  }

  f.print(existing);
  f.print(text);
  f.println();
  f.close();

  Serial.printf("[file_memory] Appended to MEMORY.md: %d bytes\n", text.length());
  return true;
}

bool file_memory_read_soul(String &soul_out, String &error_out) {
  if (!g_backend_ready) {
    error_out = "Filesystem not ready";
    return false;
  }

  if (!fs_exists(kSoulPath)) {
    soul_out = "";
    return true;
  }

  fs::File f = fs_open(kSoulPath, FILE_READ);
  if (!f) {
    error_out = "Failed to open SOUL.md";
    return false;
  }

  soul_out = f.readString();
  f.close();
  return true;
}

bool file_memory_write_soul(const String &soul, String &error_out) {
  if (!g_backend_ready) {
    error_out = "Filesystem not ready";
    return false;
  }

  fs::File f = fs_open(kSoulPath, FILE_WRITE);
  if (!f) {
    error_out = "Failed to open SOUL.md for writing";
    return false;
  }

  if (soul.length() > kMaxSoulSize) {
    f.print(soul.substring(0, kMaxSoulSize));
  } else {
    f.print(soul);
  }
  f.close();

  Serial.println("[file_memory] Updated SOUL.md");
  return true;
}

bool file_memory_read_user(String &user_out, String &error_out) {
  if (!g_backend_ready) {
    error_out = "Filesystem not ready";
    return false;
  }

  if (!fs_exists(kUserPath)) {
    user_out = "";
    return true;
  }

  fs::File f = fs_open(kUserPath, FILE_READ);
  if (!f) {
    error_out = "Failed to open USER.md";
    return false;
  }

  user_out = f.readString();
  f.close();
  return true;
}

bool file_memory_append_user(const String &text, String &error_out) {
  if (!g_backend_ready) {
    error_out = "Filesystem not ready";
    return false;
  }

  // Check current size
  if (fs_exists(kUserPath)) {
    fs::File check = fs_open(kUserPath, FILE_READ);
    if (check) {
      size_t current = check.size();
      check.close();
      if (current + text.length() > 4096) {
        error_out = "USER.md full (4KB limit)";
        return false;
      }
    }
  }

  fs::File f = fs_open(kUserPath, FILE_APPEND);
  if (!f) {
    error_out = "Failed to open USER.md for append";
    return false;
  }

  f.print("\n" + text);
  f.close();
  return true;
}

bool file_memory_append_daily(const String &note, String &error_out) {
  if (!g_backend_ready) {
    error_out = "Filesystem not ready";
    return false;
  }

  String path = get_daily_path();

  String existing;
  if (fs_exists(path.c_str())) {
    fs::File f = fs_open(path.c_str(), FILE_READ);
    if (f) {
      existing = f.readString();
      f.close();
    }
  }

  fs::File f = fs_open(path.c_str(), FILE_WRITE);
  if (!f) {
    error_out = "Failed to open daily memory file";
    return false;
  }

  // Check size limit
  if (existing.length() + note.length() > kMaxDailyMemory) {
    size_t excess = (existing.length() + note.length()) - kMaxDailyMemory;
    if (existing.length() > excess) {
      existing = existing.substring(excess);
    }
  }

  f.print(existing);
  f.print(note);
  f.println();
  f.close();

  Serial.printf("[file_memory] Appended to daily: %d bytes\n", note.length());
  return true;
}

bool file_memory_read_recent(String &content_out, int days, String &error_out) {
  if (!g_backend_ready) {
    error_out = "Filesystem not ready";
    return false;
  }

  // For simplicity, just return today's file
  // A full implementation would read multiple daily files
  String path = get_daily_path();

  if (!fs_exists(path.c_str())) {
    content_out = "";
    return true;
  }

  fs::File f = fs_open(path.c_str(), FILE_READ);
  if (!f) {
    error_out = "Failed to open daily memory file";
    return false;
  }

  content_out = f.readString();
  f.close();
  return true;
}

bool file_memory_session_append(const String &chat_id, const String &role,
                                const String &content, String &error_out) {
  if (!g_backend_ready) {
    error_out = "Filesystem not ready";
    return false;
  }

  String path = String(kSessionsDir) + "/tg_" + chat_id + ".jsonl";

  // Read existing lines to count
  int line_count = 0;
  String existing;
  if (fs_exists(path.c_str())) {
    fs::File f = fs_open(path.c_str(), FILE_READ);
    if (f) {
      existing = f.readString();
      f.close();

      // Count lines
      for (size_t i = 0; i < existing.length(); i++) {
        if (existing[i] == '\n') line_count++;
      }
    }
  }

  // Create new JSON line
  String json_line = "{\"role\":\"" + role + "\",\"content\":\"" + content + "\"}";

  fs::File f = fs_open(path.c_str(), FILE_WRITE);
  if (!f) {
    error_out = "Failed to open session file";
    return false;
  }

  // Ring buffer: remove oldest if at limit
  if (line_count >= kMaxSessionMsgs) {
    int first_nl = existing.indexOf('\n');
    if (first_nl >= 0) {
      existing = existing.substring(first_nl + 1);
    }
  }

  if (existing.length() > 0) {
    f.print(existing);
  }
  f.println(json_line);
  f.close();

  return true;
}

bool file_memory_session_get(const String &chat_id, String &history_out,
                             String &error_out) {
  if (!g_backend_ready) {
    error_out = "Filesystem not ready";
    return false;
  }

  String path = String(kSessionsDir) + "/tg_" + chat_id + ".jsonl";

  if (!fs_exists(path.c_str())) {
    history_out = "";
    return true;
  }

  fs::File f = fs_open(path.c_str(), FILE_READ);
  if (!f) {
    error_out = "Failed to open session file";
    return false;
  }

  history_out = f.readString();
  f.close();
  return true;
}

bool file_memory_session_clear(const String &chat_id, String &error_out) {
  if (!g_backend_ready) {
    error_out = "Filesystem not ready";
    return false;
  }

  String path = String(kSessionsDir) + "/tg_" + chat_id + ".jsonl";

  if (fs_exists(path.c_str())) {
    if (!fs_remove(path.c_str())) {
      error_out = "Failed to remove session file";
      return false;
    }
  }

  return true;
}

bool file_memory_get_info(String &info_out, String &error_out) {
  if (!g_backend_ready) {
    error_out = "Filesystem not ready";
    return false;
  }

  info_out = "🦖 Timi's Memory (" + fs_backend_name() + "):\n\n";

  // Long-term memory size
  if (fs_exists(kLongTermMemoryPath)) {
    fs::File f = fs_open(kLongTermMemoryPath, FILE_READ);
    if (f) {
      size_t size = f.size();
      info_out += "📚 Long-term: " + String(size) + " bytes\n";
      f.close();
    }
  }

  // Soul size
  if (fs_exists(kSoulPath)) {
    fs::File f = fs_open(kSoulPath, FILE_READ);
    if (f) {
      size_t size = f.size();
      info_out += "🦖 Soul: " + String(size) + " bytes\n";
      f.close();
    }
  }

  // User profile size
  if (fs_exists(kUserPath)) {
    fs::File f = fs_open(kUserPath, FILE_READ);
    if (f) {
      size_t size = f.size();
      info_out += "👤 User: " + String(size) + " bytes\n";
      f.close();
    }
  }

  // Total filesystem usage
  uint64_t used = fs_used_bytes();
  uint64_t total = fs_total_bytes();

  info_out += "\n💾 ";
  info_out += fs_backend_name();
  info_out += ": ";

  if (g_backend == FileBackend::SD_CARD) {
    info_out += String(used / (1024 * 1024)) + " MB used";
    info_out += " / " + String(total / (1024 * 1024)) + " MB total";
  } else {
    info_out += String(used) + " / ";
    info_out += String(total) + " bytes used";
  }

  return true;
}

bool file_memory_list_files(String &list_out, String &error_out) {
  if (!g_backend_ready) {
    error_out = "Filesystem not ready";
    return false;
  }

  list_out = "📁 SPIFFS Files:\n\n";

#if ENABLE_SD_CARD
  if (g_backend == FileBackend::SD_CARD) {
    File root = SD.open("/");
    if (!root) {
      error_out = "Failed to open SD root";
      return false;
    }

    File file = root.openNextFile();
    while (file) {
      String name = file.name();
      if (!file.isDirectory()) {
        list_out += "• " + name + " (" + String(file.size()) + " bytes)\n";
      }
      file = root.openNextFile();
    }
    root.close();
    return true;
  }
#endif

  // SPIFFS
  File root = SPIFFS.open("/");
  if (!root) {
    error_out = "Failed to open SPIFFS root";
    return false;
  }

  File file = root.openNextFile();
  while (file) {
    String name = file.name();
    if (!file.isDirectory()) {
      list_out += "• " + name + " (" + String(file.size()) + " bytes)\n";
    }
    file = root.openNextFile();
  }
  root.close();

  return true;
}

static String normalize_user_path(String path) {
  path.trim();
  if (!path.startsWith("/") && !path.startsWith("/memory/") &&
      !path.startsWith("/config/") && !path.startsWith("/sessions/") &&
      !path.startsWith("/projects/")) {
    path = "/" + path;
  }
  return path;
}

static bool ensure_parent_dirs_for_path(const String &path, String &error_out) {
  int slash = path.indexOf('/');
  while (slash >= 0) {
    slash = path.indexOf('/', slash + 1);
    if (slash < 0) {
      break;
    }
    String dir = path.substring(0, slash);
    if (dir.length() == 0) {
      continue;
    }
    if (!fs_exists(dir.c_str()) && !fs_mkdir(dir.c_str())) {
      error_out = "Failed to create directory: " + dir;
      return false;
    }
  }
  return true;
}

bool file_memory_read_file(const String &filename, String &content_out, String &error_out) {
  if (!g_backend_ready) {
    error_out = "Filesystem not ready";
    return false;
  }

  String path = normalize_user_path(filename);

  if (!fs_exists(path.c_str())) {
    error_out = "File not found: " + filename;
    return false;
  }

  fs::File f = fs_open(path.c_str(), FILE_READ);
  if (!f) {
    error_out = "Failed to open file: " + filename;
    return false;
  }

  size_t file_size = f.size();
  Serial.printf("[file_memory] Reading %s: %d bytes\n", path.c_str(), file_size);

  // Reserve space and read in chunks for better reliability
  content_out = "";
  if (file_size > 0) {
    content_out.reserve(file_size + 1);

    // Read file in chunks to avoid memory issues
    const size_t chunk_size = 512;
    char buffer[chunk_size];
    while (f.available() > 0) {
      size_t bytes_to_read = min((size_t)f.available(), chunk_size);
      size_t bytes_read = f.read((uint8_t*)buffer, bytes_to_read);
      if (bytes_read > 0) {
        content_out += String(buffer, bytes_read);
      } else {
        break;
      }
    }
  }

  f.close();

  Serial.printf("[file_memory] Read %d bytes, content length: %d\n", file_size, content_out.length());

  if (content_out.length() == 0 && file_size > 0) {
    error_out = "Failed to read file content (read 0 bytes from " + String(file_size) + ")";
    return false;
  }

  return true;
}

bool file_memory_write_file(const String &filename, const String &content, String &error_out) {
  if (!g_backend_ready) {
    error_out = "Filesystem not ready";
    return false;
  }

  String path = normalize_user_path(filename);
  if (path.length() == 0 || path == "/") {
    error_out = "Invalid filename";
    return false;
  }

  if (!ensure_parent_dirs_for_path(path, error_out)) {
    return false;
  }

  fs::File f = fs_open(path.c_str(), FILE_WRITE);
  if (!f) {
    error_out = "Failed to open file for write: " + path;
    return false;
  }

  const size_t written = f.print(content);
  f.close();
  if (written != content.length()) {
    error_out = "Partial write to file: " + path;
    return false;
  }

  Serial.printf("[file_memory] Wrote %d bytes to %s\n", content.length(), path.c_str());
  return true;
}
