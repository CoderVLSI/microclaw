#ifndef USAGE_STATS_H
#define USAGE_STATS_H

#include <Arduino.h>

// Initialize stats from NVS (or start fresh)
void usage_init();

// Record an LLM API call
void usage_record_call(const char *call_type, int http_status, const char *provider, const char *model);

// Record an error (429, 500, etc.)
void usage_record_error(int http_status);

// Get formatted usage report
void usage_get_report(String &out);

// Reset all stats
void usage_reset();

#endif
