#ifndef FILE_MEMORY_H
#define FILE_MEMORY_H

#include <Arduino.h>

// Initialize SPIFFS and create default files
void file_memory_init();

// Long-term memory (MEMORY.md)
bool file_memory_read_long_term(String &content_out, String &error_out);
bool file_memory_append_long_term(const String &text, String &error_out);

// Soul/Personality (SOUL.md)
bool file_memory_read_soul(String &soul_out, String &error_out);
bool file_memory_write_soul(const String &soul, String &error_out);

// User profile (USER.md)
bool file_memory_read_user(String &user_out, String &error_out);
bool file_memory_append_user(const String &text, String &error_out);

// Daily notes
bool file_memory_append_daily(const String &note, String &error_out);
bool file_memory_read_recent(String &content_out, int days, String &error_out);

// Session management
bool file_memory_session_append(const String &chat_id, const String &role,
                                const String &content, String &error_out);
bool file_memory_session_get(const String &chat_id, String &history_out,
                             String &error_out);
bool file_memory_session_clear(const String &chat_id, String &error_out);

// Memory info
bool file_memory_get_info(String &info_out, String &error_out);

// File listing and reading
bool file_memory_list_files(String &list_out, String &error_out);
bool file_memory_read_file(const String &filename, String &content_out, String &error_out);
bool file_memory_write_file(const String &filename, const String &content, String &error_out);

#endif
