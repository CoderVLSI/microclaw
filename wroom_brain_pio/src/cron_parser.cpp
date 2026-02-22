#include "cron_parser.h"

static int parse_cron_field(const String &field, int min_val, int max_val, String &error) {
  String f = field;
  f.trim();

  // Wildcard
  if (f == "*" || f == "?") {
    return -1;
  }

  if (f.length() == 0) {
    error = "Empty field";
    return -2;
  }

  // Strict numeric validation (reject values like "14:05" or "60abc").
  for (int i = 0; i < (int)f.length(); i++) {
    if (f[i] < '0' || f[i] > '9') {
      error = "Invalid numeric value: " + f;
      return -2;
    }
  }

  // Parse as number
  int val = atoi(f.c_str());
  if (val < min_val || val > max_val) {
    error = "Value " + String(val) + " out of range [" + String(min_val) + "-" + String(max_val) + "]";
    return -2;
  }

  return val;
}

static String get_cron_field_name(int index) {
  switch (index) {
    case 0: return "minute";
    case 1: return "hour";
    case 2: return "day";
    case 3: return "month";
    case 4: return "weekday";
    default: return "unknown";
  }
}

bool cron_parse_line(const String &line, CronJob &job, String &error_out) {
  String l = line;
  l.trim();

  // Skip empty lines and comments
  if (l.length() == 0 || l[0] == '#') {
    return false;
  }

  // Split by pipe (|)
  int pipe_pos = l.indexOf('|');
  if (pipe_pos < 0) {
    error_out = "Missing '|' separator in cron line";
    return false;
  }

  String cron_part = l.substring(0, pipe_pos);
  String command = l.substring(pipe_pos + 1);
  cron_part.trim();
  command.trim();

  if (command.length() == 0) {
    error_out = "Empty command after '|'";
    return false;
  }

  job.command = command;

  // Split cron part by spaces (5 fields expected)
  // Format: minute hour day month weekday
  int space1 = cron_part.indexOf(' ');
  if (space1 < 0) {
    error_out = "Invalid cron format (need 5 fields: min hour day month weekday)";
    return false;
  }

  int space2 = cron_part.indexOf(' ', space1 + 1);
  if (space2 < 0) {
    error_out = "Invalid cron format (need 5 fields)";
    return false;
  }

  int space3 = cron_part.indexOf(' ', space2 + 1);
  if (space3 < 0) {
    error_out = "Invalid cron format (need 5 fields)";
    return false;
  }

  int space4 = cron_part.indexOf(' ', space3 + 1);
  if (space4 < 0) {
    error_out = "Invalid cron format (need 5 fields)";
    return false;
  }

  String min_str = cron_part.substring(0, space1);
  String hour_str = cron_part.substring(space1 + 1, space2);
  String day_str = cron_part.substring(space2 + 1, space3);
  String month_str = cron_part.substring(space3 + 1, space4);
  String wday_str = cron_part.substring(space4 + 1);

  // Parse minute (0-59)
  job.minute = parse_cron_field(min_str, 0, 59, error_out);
  if (job.minute == -2) {
    error_out = "minute: " + error_out;
    return false;
  }

  // Parse hour (0-23)
  job.hour = parse_cron_field(hour_str, 0, 23, error_out);
  if (job.hour == -2) {
    error_out = "hour: " + error_out;
    return false;
  }

  // Parse day (1-31)
  job.day = parse_cron_field(day_str, 1, 31, error_out);
  if (job.day == -2) {
    error_out = "day: " + error_out;
    return false;
  }

  // Parse month (1-12)
  job.month = parse_cron_field(month_str, 1, 12, error_out);
  if (job.month == -2) {
    error_out = "month: " + error_out;
    return false;
  }

  // Parse weekday (0-6, 0=Sunday)
  job.weekday = parse_cron_field(wday_str, 0, 6, error_out);
  if (job.weekday == -2) {
    error_out = "weekday: " + error_out;
    return false;
  }

  return true;
}

bool cron_should_trigger(const CronJob &job, int hour, int minute, int day, int month, int weekday) {
  // Check minute
  if (job.minute != -1 && job.minute != minute) {
    return false;
  }

  // Check hour
  if (job.hour != -1 && job.hour != hour) {
    return false;
  }

  // Check day
  if (job.day != -1 && job.day != day) {
    return false;
  }

  // Check month
  if (job.month != -1 && job.month != month) {
    return false;
  }

  // Check weekday (0=Sunday, 1=Monday, ..., 6=Saturday)
  if (job.weekday != -1 && job.weekday != weekday) {
    return false;
  }

  return true;
}

String cron_job_to_string(const CronJob &job) {
  String result = "";

  // Minute
  result += (job.minute == -1) ? "*" : String(job.minute);
  result += " ";

  // Hour
  result += (job.hour == -1) ? "*" : String(job.hour);
  result += " ";

  // Day
  result += (job.day == -1) ? "*" : String(job.day);
  result += " ";

  // Month
  result += (job.month == -1) ? "*" : String(job.month);
  result += " ";

  // Weekday
  result += (job.weekday == -1) ? "*" : String(job.weekday);

  result += " | " + job.command;

  return result;
}
