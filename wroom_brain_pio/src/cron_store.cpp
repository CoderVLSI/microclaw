#include "cron_store.h"

#include <FS.h>
#include "SPIFFS.h"
#include <time.h>

#define CRON_FILENAME "/cron.md"
#define LAST_CHECK_FILE "/cron_lastcheck.txt"

// Cache of loaded jobs
static CronJob s_cached_jobs[CRON_MAX_JOBS];
static int s_cached_count = 0;
static bool s_initialized = false;

// Load all cron jobs from cron.md
static void cron_store_load() {
  s_cached_count = 0;

  if (!SPIFFS.exists(CRON_FILENAME)) {
    // Create default cron.md with header
    File f = SPIFFS.open(CRON_FILENAME, "w");
    if (f) {
      f.println("# Cron Jobs");
      f.println("# Format: minute hour day month weekday | command");
      f.println("# Example: 0 9 * * * | Good morning message");
      f.println("# Wildcards: * means any value");
      f.println("# minute: 0-59, hour: 0-23, day: 1-31, month: 1-12, weekday: 0-6 (0=Sunday)");
      f.println("");
      f.close();
    }
    return;
  }

  File f = SPIFFS.open(CRON_FILENAME, "r");
  if (!f) {
    Serial.println("[cron_store] Failed to open cron.md");
    return;
  }

  Serial.println("[cron_store] Loading cron jobs from cron.md");

  while (f.available() && s_cached_count < CRON_MAX_JOBS) {
    String line = f.readStringUntil('\n');
    line.trim();

    // Remove carriage return if present
    if (line.length() > 0 && line[line.length() - 1] == '\r') {
      line.remove(line.length() - 1);
    }

    if (line.length() == 0) {
      continue;
    }

    // Skip comments
    if (line[0] == '#') {
      continue;
    }

    CronJob job;
    String error;
    if (cron_parse_line(line, job, error)) {
      s_cached_jobs[s_cached_count++] = job;
      Serial.printf("[cron_store] Loaded: %s\n", cron_job_to_string(job).c_str());
    } else {
      Serial.printf("[cron_store] Skipping invalid line: %s (error: %s)\n", line.c_str(), error.c_str());
    }
  }

  f.close();
  Serial.printf("[cron_store] Loaded %d cron job(s)\n", s_cached_count);
}

void cron_store_init() {
  if (s_initialized) {
    return;
  }

  if (!SPIFFS.begin(true)) {
    Serial.println("[cron_store] SPIFFS mount failed");
    return;
  }

  cron_store_load();
  s_initialized = true;
}

bool cron_store_add(const String &cron_line, String &error_out) {
  // Validate the line first
  CronJob job;
  if (!cron_parse_line(cron_line, job, error_out)) {
    return false;
  }

  if (s_cached_count >= CRON_MAX_JOBS) {
    error_out = "Maximum cron jobs reached (" + String(CRON_MAX_JOBS) + ")";
    return false;
  }

  // Add to cache
  s_cached_jobs[s_cached_count++] = job;

  // Append to file
  File f = SPIFFS.open(CRON_FILENAME, "a");
  if (!f) {
    error_out = "Failed to open cron.md for writing";
    return false;
  }

  f.println(cron_line);
  f.close();

  Serial.printf("[cron_store] Added cron job: %s\n", cron_job_to_string(job).c_str());
  return true;
}

int cron_store_get_all(CronJob *jobs, int max_jobs) {
  int count = (s_cached_count < max_jobs) ? s_cached_count : max_jobs;
  for (int i = 0; i < count; i++) {
    jobs[i] = s_cached_jobs[i];
  }
  return count;
}

bool cron_store_clear(String &error_out) {
  s_cached_count = 0;

  // Rewrite file with header only
  File f = SPIFFS.open(CRON_FILENAME, "w");
  if (!f) {
    error_out = "Failed to open cron.md for writing";
    return false;
  }

  f.println("# Cron Jobs");
  f.println("# Format: minute hour day month weekday | command");
  f.println("# Example: 0 9 * * * | Good morning message");
  f.println("");
  f.close();

  Serial.println("[cron_store] Cleared all cron jobs");
  return true;
}

bool cron_store_get_content(String &content_out, String &error_out) {
  if (!SPIFFS.exists(CRON_FILENAME)) {
    error_out = "cron.md does not exist";
    return false;
  }

  File f = SPIFFS.open(CRON_FILENAME, "r");
  if (!f) {
    error_out = "Failed to open cron.md";
    return false;
  }

  content_out = f.readString();
  f.close();
  return true;
}

int cron_store_count() {
  return s_cached_count;
}

// ============================================================================
// MISSED JOB TRACKING IMPLEMENTATION
// ============================================================================

time_t cron_store_get_last_check() {
  if (!SPIFFS.exists(LAST_CHECK_FILE)) {
    return 0;  // Never checked
  }

  File f = SPIFFS.open(LAST_CHECK_FILE, "r");
  if (!f) {
    return 0;
  }

  String timestamp_str = f.readStringUntil('\n');
  f.close();

  return (time_t)timestamp_str.toInt();
}

void cron_store_update_last_check(time_t timestamp) {
  File f = SPIFFS.open(LAST_CHECK_FILE, "w");
  if (f) {
    f.println((unsigned long)timestamp);
    f.close();
  }
}

// Helper: Check if a time is within a cron job's schedule
static bool matches_schedule(const CronJob &job, struct tm *tm_time) {
  return cron_should_trigger(job, tm_time->tm_hour, tm_time->tm_min,
                            tm_time->tm_mday, tm_time->tm_mon + 1, tm_time->tm_wday);
}

int cron_store_check_missed_jobs(time_t now, MissedJob *missed_jobs, int max_jobs) {
  time_t last_check = cron_store_get_last_check();

  // If never checked or last_check is invalid, no missed jobs to report
  if (last_check == 0 || last_check >= now) {
    return 0;
  }

  // Bound recovery window to avoid expensive scans after very long offline periods.
  const time_t MAX_LOOKBACK = 48 * 3600;
  if ((now - last_check) > MAX_LOOKBACK) {
    last_check = now - MAX_LOOKBACK;
  }

  int missed_count = 0;
  const time_t CHECK_INTERVAL = 60;  // Check every minute

  // Start from the next full minute boundary.
  time_t t = (last_check / 60) * 60;
  if (t <= last_check) {
    t += CHECK_INTERVAL;
  }

  // Iterate through time from last_check to now
  for (; t <= now && missed_count < max_jobs; t += CHECK_INTERVAL) {
    struct tm tm_check_storage{};
    struct tm *tm_check = localtime_r(&t, &tm_check_storage);
    if (!tm_check) {
      continue;
    }

    // For each cron job, check if it should have triggered
    for (int i = 0; i < s_cached_count && missed_count < max_jobs; i++) {
      const CronJob &job = s_cached_jobs[i];

      // Skip wildcard-only jobs (would trigger too often)
      if (job.minute == -1) {
        continue;  // Skip jobs with minute wildcard
      }

      // Check if this job should have triggered at this time
      if (matches_schedule(job, tm_check)) {
        // Found a missed job
        missed_jobs[missed_count].command = job.command;
        missed_jobs[missed_count].missed_hour = tm_check->tm_hour;
        missed_jobs[missed_count].missed_minute = tm_check->tm_min;
        missed_jobs[missed_count].missed_day = tm_check->tm_mday;
        missed_jobs[missed_count].missed_month = tm_check->tm_mon + 1;
        missed_jobs[missed_count].missed_weekday = tm_check->tm_wday;
        missed_count++;
      }
    }
  }

  if (missed_count > 0) {
    Serial.printf("[cron_store] Found %d missed job(s)\n", missed_count);
  }

  return missed_count;
}
