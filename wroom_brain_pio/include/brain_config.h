#ifndef BRAIN_CONFIG_H
#define BRAIN_CONFIG_H

#include "brain_secrets.generated.h"

#ifndef WIFI_SSID
#error "Missing WIFI_SSID define. Set it in .env."
#endif

#ifndef WIFI_PASS
#error "Missing WIFI_PASS define. Set it in .env."
#endif

#ifndef TELEGRAM_BOT_TOKEN
#error "Missing TELEGRAM_BOT_TOKEN define. Set it in .env."
#endif

#ifndef TELEGRAM_ALLOWED_CHAT_ID
#error "Missing TELEGRAM_ALLOWED_CHAT_ID define. Set it in .env."
#endif

#ifndef TELEGRAM_POLL_MS
#define TELEGRAM_POLL_MS 3000
#endif

#ifndef AUTONOMOUS_STATUS_ENABLED
#define AUTONOMOUS_STATUS_ENABLED 0
#endif

#ifndef AUTONOMOUS_STATUS_MS
#define AUTONOMOUS_STATUS_MS 30000
#endif

#ifndef LLM_PROVIDER
#define LLM_PROVIDER "none"
#endif

#ifndef LLM_API_KEY
#define LLM_API_KEY ""
#endif

#ifndef LLM_MODEL
#define LLM_MODEL ""
#endif

#ifndef LLM_OPENAI_BASE_URL
#define LLM_OPENAI_BASE_URL "https://api.openai.com"
#endif

#ifndef LLM_ANTHROPIC_BASE_URL
#define LLM_ANTHROPIC_BASE_URL "https://api.anthropic.com"
#endif

#ifndef LLM_GEMINI_BASE_URL
#define LLM_GEMINI_BASE_URL "https://generativelanguage.googleapis.com"
#endif

#ifndef LLM_GLM_BASE_URL
#define LLM_GLM_BASE_URL "https://api.z.ai/api/coding/paas/v4"
#endif

#ifndef LLM_TIMEOUT_MS
#define LLM_TIMEOUT_MS 60000
#endif

// Image generation provider (separate from chat LLM)
#ifndef IMAGE_PROVIDER
#define IMAGE_PROVIDER "none"
#endif

#ifndef IMAGE_API_KEY
#define IMAGE_API_KEY ""
#endif

// Email sending provider (resend or sendgrid)
#ifndef EMAIL_PROVIDER
#define EMAIL_PROVIDER "resend"
#endif

// Common from email for all providers
#ifndef EMAIL_FROM
#define EMAIL_FROM "onboarding@resend.dev"
#endif

// Resend API
#ifndef RESEND_API_KEY
#define RESEND_API_KEY ""
#endif

// SendGrid API
#ifndef SENDGRID_API_KEY
#define SENDGRID_API_KEY ""
#endif

#ifndef MEMORY_MAX_CHARS
#define MEMORY_MAX_CHARS 5000
#endif

#ifndef BLUE_LED_PIN
#define BLUE_LED_PIN 2
#endif

#ifndef BLUE_LED_ACTIVE_HIGH
#define BLUE_LED_ACTIVE_HIGH 1
#endif

#ifndef BLUE_LED_FLASH_MS
#define BLUE_LED_FLASH_MS 180
#endif

#ifndef ACTION_CONFIRM_TIMEOUT_MS
#define ACTION_CONFIRM_TIMEOUT_MS 30000
#endif

#ifndef SOUL_MAX_CHARS
#define SOUL_MAX_CHARS 1400
#endif

#ifndef HEARTBEAT_MAX_CHARS
#define HEARTBEAT_MAX_CHARS 1400
#endif

#ifndef HEARTBEAT_ENABLED
#define HEARTBEAT_ENABLED 1
#endif

#ifndef HEARTBEAT_INTERVAL_MS
#define HEARTBEAT_INTERVAL_MS 600000
#endif

#ifndef TIMEZONE_TZ
#define TIMEZONE_TZ "UTC0"
#endif

#ifndef NTP_SERVER_1
#define NTP_SERVER_1 "pool.ntp.org"
#endif

#ifndef NTP_SERVER_2
#define NTP_SERVER_2 "time.nist.gov"
#endif

#ifndef REMINDER_MSG_MAX_CHARS
#define REMINDER_MSG_MAX_CHARS 220
#endif

#ifndef REMINDER_GRACE_MINUTES
#define REMINDER_GRACE_MINUTES 10
#endif

#ifndef TASKS_MAX_CHARS
#define TASKS_MAX_CHARS 8000
#endif

#ifndef WEB_SEARCH_API_KEY
#define WEB_SEARCH_API_KEY ""
#endif

#ifndef WEB_SEARCH_PROVIDER
#define WEB_SEARCH_PROVIDER "auto"
#endif

#ifndef WEB_SEARCH_BASE_URL
#define WEB_SEARCH_BASE_URL "https://api.search.brave.com"
#endif

#ifndef TAVILY_API_KEY
#define TAVILY_API_KEY ""
#endif

#ifndef TAVILY_BASE_URL
#define TAVILY_BASE_URL "https://api.tavily.com"
#endif

#ifndef WEB_SEARCH_TIMEOUT_MS
#define WEB_SEARCH_TIMEOUT_MS 12000
#endif

#ifndef WEB_SEARCH_RESULTS_MAX
#define WEB_SEARCH_RESULTS_MAX 3
#endif

#ifndef WEB_JOB_ENDPOINT_URL
#define WEB_JOB_ENDPOINT_URL ""
#endif

#ifndef WEB_JOB_API_KEY
#define WEB_JOB_API_KEY ""
#endif

#ifndef WEB_JOB_TIMEOUT_MS
#define WEB_JOB_TIMEOUT_MS 20000
#endif

// GitHub repo for auto-updates (format: username/repo)
#ifndef GITHUB_REPO
#define GITHUB_REPO ""
#endif

// ============================================================================
// COMPILE-TIME FEATURE FLAGS
// Set to 0 to disable features and reduce flash usage
// ============================================================================

// Email functionality (draft, send, email_code)
#ifndef ENABLE_EMAIL
#define ENABLE_EMAIL 1
#endif

// Image generation via LLM providers
#ifndef ENABLE_IMAGE_GEN
#define ENABLE_IMAGE_GEN 1
#endif

// Media understanding (PDF, image analysis)
#ifndef ENABLE_MEDIA_UNDERSTANDING
#define ENABLE_MEDIA_UNDERSTANDING 1
#endif

// Task management system
#ifndef ENABLE_TASKS
#define ENABLE_TASKS 1
#endif

// Web job backend integration
#ifndef ENABLE_WEB_JOBS
#define ENABLE_WEB_JOBS 0
#endif

// Planning command
#ifndef ENABLE_PLAN
#define ENABLE_PLAN 0
#endif

// GPIO/Hardware control (relay, LED flash, sensor read)
#ifndef ENABLE_GPIO
#define ENABLE_GPIO 0
#endif

#endif
