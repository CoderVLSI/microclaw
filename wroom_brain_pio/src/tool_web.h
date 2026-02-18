#ifndef TOOL_WEB_H
#define TOOL_WEB_H

#include <Arduino.h>

// Web Search (Brave or Tavily)
// Returns summary of search results.
bool tool_web_search(const String &query, String &output_out);

// Weather (OpenMeteo)
// Returns current weather for location.
bool tool_web_weather(const String &location, String &output_out);

// Time (NTP)
// Returns current local time.
bool tool_web_time(String &output_out);

#endif
