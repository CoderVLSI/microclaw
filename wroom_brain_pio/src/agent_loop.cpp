#include "agent_loop.h"

#include <Arduino.h>
#include <vector>
#include <freertos/semphr.h>

#include "brain_config.h"
#include "scheduler.h"
#include "chat_history.h"
#include "memory_store.h"
#include "file_memory.h"
#include "llm_client.h"
#include "model_config.h"
#include "persona_store.h"
#include "event_log.h"
#include "status_led.h"
#include "task_store.h"
#include "tool_registry.h"
#include "transport_telegram.h"
#include "usage_stats.h"
#include "web_server.h"
#include "react_agent.h"

// Store last LLM response for emailing code
static String s_last_llm_response = "";

// Web Message Queue
static std::vector<String> s_web_message_queue;
static SemaphoreHandle_t s_web_queue_mutex = NULL;

String agent_loop_get_last_response() {
  return s_last_llm_response;
}

void agent_loop_set_last_response(const String &response) {
  s_last_llm_response = response;
}

static bool is_internal_dispatch_message(const String &msg) {
  String lc = msg;
  lc.trim();
  lc.toLowerCase();
  return lc == "heartbeat_run" || lc == "reminder_run";
}

static bool should_try_route(const String &msg) {
  String lc = msg;
  lc.trim();
  lc.toLowerCase();
  if (lc.length() == 0 || lc.startsWith("/")) {
    return false;
  }

  const char *prefixes[] = {
      "set ",       "show ",      "list ",      "add ",       "delete ",    "clear ",
      "turn ",      "switch ",    "enable ",    "disable ",   "remind ",    "schedule ",
      "search ",    "look up ",   "find ",      "google ",    "status",     "health",
      "logs",       "time",       "timezone",   "task ",      "memory",     "remember ",
      "forget",     "flash ",     "blink ",     "led ",       "sensor ",    "relay ",
      "safe mode",  "email ",     "plan ",      "confirm",    "cancel",      "create ",
      "build ",     "make ",      "generate ",  "update",     "upgrade",    "firmware",
      "website",    "html",       "web ",       "saas",       "landing",    "portfolio",
      "host ",      "serve ",     "deploy ",
  };

  for (size_t i = 0; i < (sizeof(prefixes) / sizeof(prefixes[0])); i++) {
    String p = String(prefixes[i]);
    if (lc == p || lc.startsWith(p)) {
      return true;
    }
  }

  if (lc.indexOf("every day") >= 0 || lc.indexOf("everyday") >= 0 || lc.indexOf("daily") >= 0 ||
      lc.indexOf("at ") >= 0 || lc.indexOf("reminder") >= 0 || lc.indexOf("web search") >= 0 ||
      lc.indexOf("update") >= 0 || lc.indexOf("upgrade") >= 0 || lc.indexOf("firmware") >= 0 ||
      lc.indexOf("new version") >= 0 || lc.indexOf("latest") >= 0) {
    return true;
  }

  return false;
}

static void record_user_msg(const String &incoming) {
  if (is_internal_dispatch_message(incoming)) {
    return;
  }
  String err;
  chat_history_append('U', incoming, err);
}

static void record_bot_msg(const String &outgoing) {
  String err;
  chat_history_append('A', outgoing, err);
}

// Detect language from code content if not specified
static String detect_language_from_content(const String &code) {
  String lc = code;
  lc.toLowerCase();

  // HTML patterns
  if (lc.indexOf("<!doctype html") >= 0 || lc.indexOf("<html") >= 0 || lc.indexOf("<div") >= 0 || lc.indexOf("<body") >= 0) {
    // Check if it also has script/style tags (might be full HTML)
    if (lc.indexOf("<style") >= 0 || lc.indexOf("<script") >= 0) {
      return "html_full";
    }
    return "html";
  }

  // CSS patterns
  if (lc.indexOf("{") >= 0 && lc.indexOf("}") >= 0 && lc.indexOf(":") >= 0 &&
      (lc.indexOf("margin") >= 0 || lc.indexOf("padding") >= 0 ||
       lc.indexOf("background") >= 0 || lc.indexOf("display:") >= 0 ||
       lc.indexOf("color:") >= 0 || lc.indexOf("font-") >= 0 ||
       lc.indexOf("border") >= 0 || lc.indexOf("flex") >= 0 ||
       lc.indexOf("@media") >= 0 || lc.indexOf("@keyframes") >= 0)) {
    return "css";
  }

  // JavaScript patterns
  if (lc.indexOf("function ") >= 0 || lc.indexOf("const ") >= 0 || lc.indexOf("let ") >= 0 || lc.indexOf("=>") >= 0 || lc.indexOf("console.log") >= 0 || lc.indexOf("document.") >= 0) {
    return "js";
  }

  // Python patterns
  if (lc.indexOf("def ") >= 0 || lc.indexOf("import ") >= 0 || lc.indexOf("print(") >= 0 || lc.indexOf("self.") >= 0) {
    return "py";
  }

  // C/C++ patterns
  if (lc.indexOf("#include") >= 0 || (lc.indexOf("int main") >= 0 && lc.indexOf("{") >= 0)) {
    if (lc.indexOf("class ") >= 0 || lc.indexOf("public:") >= 0 || lc.indexOf("namespace") >= 0) {
      return "cpp";
    }
    return "c";
  }

  return "";
}

// Extract code blocks from LLM response and send as files
// Returns number of code files sent
static int extract_and_send_code_blocks(const String &response) {
  int files_sent = 0;
  int block_start = -1;
  String code_lang;
  String code_content;

  Serial.println("[agent] Checking for code blocks...");

  for (int i = 0; i < (int)response.length() - 2; i++) {
    // Check for code block start ```
    if (response[i] == '`' && response[i+1] == '`' && response[i+2] == '`') {
      if (block_start < 0) {
        // Found opening ```
        block_start = i + 3;
        // Extract language (e.g., ```cpp, ```python)
        int lang_end = block_start;
        while (lang_end < (int)response.length() && response[lang_end] != '\n' && response[lang_end] != ' ') {
          lang_end++;
        }
        code_lang = response.substring(block_start, lang_end);
        code_lang.toLowerCase();
        code_lang.trim();

        // Handle empty language tag
        if (code_lang.length() == 0 || code_lang == "\r" || code_lang == "\n") {
          code_lang = "";
        }

        Serial.printf("[agent] Found code block, language: '%s'\n", code_lang.c_str());

        // Find actual code start
        while (lang_end < (int)response.length() && (response[lang_end] == ' ' || response[lang_end] == '\r')) {
          lang_end++;
        }
        if (lang_end < (int)response.length() && response[lang_end] == '\n') {
          lang_end++;
        }
        block_start = lang_end;
      } else {
        // Found closing ```
        code_content = response.substring(block_start, i);
        code_content.trim();

        // Skip empty code blocks
        if (code_content.length() < 10) {
          block_start = -1;
          continue;
        }

        // If no language specified, detect from content
        if (code_lang.length() == 0) {
          code_lang = detect_language_from_content(code_content);
          Serial.printf("[agent] Auto-detected language: '%s'\n", code_lang.c_str());
        }

        // Map language to file extension
        String ext = "txt";
        String mime = "text/plain";

        if (code_lang == "cpp" || code_lang == "c++" || code_lang == "cxx") {
          ext = "cpp";
          mime = "text/x-c++src";
        } else if (code_lang == "c") {
          ext = "c";
          mime = "text/x-csrc";
        } else if (code_lang == "py" || code_lang == "python") {
          ext = "py";
          mime = "text/x-python";
        } else if (code_lang == "js" || code_lang == "javascript") {
          ext = "js";
          mime = "application/javascript";
        } else if (code_lang == "html") {
          ext = "html";
          mime = "text/html";
        } else if (code_lang == "css") {
          ext = "css";
          mime = "text/css";
        } else if (code_lang == "json") {
          ext = "json";
          mime = "application/json";
        } else if (code_lang == "md" || code_lang == "markdown") {
          ext = "md";
          mime = "text/markdown";
        } else if (code_lang == "ino" || code_lang == "arduino") {
          ext = "ino";
          mime = "text/x-c++src";
        } else if (code_lang == "h" || code_lang == "hpp") {
          ext = code_lang;
          mime = "text/x-csrc";
        } else if (code_lang == "sh" || code_lang == "bash" || code_lang == "shell") {
          ext = "sh";
          mime = "text/x-sh";
        } else if (code_lang == "ts" || code_lang == "typescript") {
          ext = "ts";
          mime = "text/typescript";
        } else if (code_lang == "tsx" || code_lang == "jsx") {
          ext = code_lang;
          mime = "text/javascript";
        } else if (code_lang == "sql") {
          ext = "sql";
          mime = "text/sql";
        } else if (code_lang == "java") {
          ext = "java";
          mime = "text/java";
        } else if (code_lang == "rust" || code_lang == "rs") {
          ext = "rs";
          mime = "text/rust";
        } else if (code_lang == "go" || code_lang == "golang") {
          ext = "go";
          mime = "text/go";
        } else if (code_lang == "xml" || code_lang == "yaml" || code_lang == "yml") {
          ext = code_lang;
          mime = "text/plain";
        } else if (code_lang == "html_full") {
          ext = "html";
          mime = "text/html";
        }

        // Generate filename with timestamp
        String filename = "code_" + String((unsigned long)millis()) + "_" + String(files_sent) + "." + ext;

        Serial.printf("[agent] Sending code file: %s (%d bytes)\n", filename.c_str(), code_content.length());

        // Send as document
        if (transport_telegram_send_document(filename, code_content, mime, "Here's the code file:")) {
          files_sent++;
          Serial.printf("[agent] Code file sent successfully!\n");
        } else {
          Serial.printf("[agent] Failed to send code file\n");
        }

        // Reset for next code block
        block_start = -1;
        code_content = "";
      }
    }
  }

  Serial.printf("[agent] Total code files sent: %d\n", files_sent);
  return files_sent;
}

// Send message directly (no streaming overhead)
static void send_streaming(const String &outgoing) {
  if (outgoing.length() == 0) {
    return;
  }
  transport_telegram_send(outgoing);
}

// Check if text looks like code (even without ``` blocks)
static bool looks_like_code(const String &text) {
  String lc = text;
  lc.toLowerCase();

  // Code-like patterns
  const char *code_patterns[] = {
    "function ", "def ", "class ", "import ", "#include",
    "public void", "private int", "const ", "let ", "var ",
    "return ", "if (", "for (", "while (", "print(", "console.log",
    "{", "}", "//", "/*", "*/", "#!"
  };

  // Check for multiple code patterns
  int pattern_count = 0;
  for (size_t i = 0; i < sizeof(code_patterns) / sizeof(code_patterns[0]); i++) {
    if (lc.indexOf(code_patterns[i]) >= 0) {
      pattern_count++;
      if (pattern_count >= 2) return true;
    }
  }

  // Check for code-like line patterns (indentation)
  int line_count = 0;
  int indented_lines = 0;
  for (int i = 0; i < (int)lc.length(); i++) {
    if (lc[i] == '\n') {
      line_count++;
      // Check if next non-space character is indented (4+ spaces or tab)
      int j = i + 1;
      int spaces = 0;
      while (j < (int)lc.length() && (lc[j] == ' ' || lc[j] == '\t')) {
        if (lc[j] == ' ') spaces++;
        j++;
      }
      if (spaces >= 4) indented_lines++;
    }
  }

  // Many indented lines = likely code
  if (indented_lines >= 3 && line_count > 5) {
    return true;
  }

  return false;
}

static void send_reply_via_telegram(const String &outgoing) {
  event_log_append("OUT: " + outgoing);

  // Check if response contains code blocks
  int code_count = 0;
  for (int i = 0; i < (int)outgoing.length() - 2; i++) {
    if (outgoing[i] == '`' && outgoing[i+1] == '`' && outgoing[i+2] == '`') {
      code_count++;
    }
  }

  // If we have code blocks (even pairs of ```), send them as files
  if (code_count >= 2) {
    int sent = extract_and_send_code_blocks(outgoing);
    if (sent > 0) {
      String summary = "🦖 I've sent " + String(sent) + " code file(s)! Check above.";
      send_streaming(summary);
    } else {
      send_streaming(outgoing);
    }
  } else if (looks_like_code(outgoing) && outgoing.length() > 100) {
    String detected = detect_language_from_content(outgoing);
    String ext = "txt";
    if (detected == "html" || detected == "html_full") ext = "html";
    else if (detected == "css") ext = "css";
    else if (detected == "js") ext = "js";
    else if (detected == "py") ext = "py";
    else if (detected == "c") ext = "c";
    else if (detected == "cpp") ext = "cpp";
    String filename = "code_" + String((unsigned long)millis()) + "." + ext;
    transport_telegram_send_document(filename, outgoing, "text/plain", "Here's your code:");
    send_streaming("🦖 I've sent the code as a file!");
  } else {
    // No code blocks, send with streaming
    send_streaming(outgoing);
  }

  if (outgoing.startsWith("ERR:")) {
    status_led_notify_error();
  }
}

String agent_loop_process_message(const String &msg) {
  if (msg.length() == 0) {
    return "";
  }

  status_led_set_busy(true);

  Serial.print("[agent] processing: ");
  Serial.println(msg);
  event_log_append("IN: " + msg);

  String response;
  bool handled = false;

  // 1. Direct Tool Execution
  if (tool_registry_execute(msg, response)) {
    handled = true;
  } 
  else {
    String trimmed = msg;
    trimmed.trim();

    // 2. Deny unknown slash commands
    if (trimmed.startsWith("/")) {
      response = "Denied or unknown command";
      handled = true;
    }
    
    // 3. Router (if not handled)
    if (!handled && should_try_route(trimmed)) {
      String routed_command;
      String route_err;
      if (llm_route_tool_command(trimmed, routed_command, route_err)) {
        routed_command.trim();
        if (routed_command.length() > 0) {
          String routed_response;
          if (tool_registry_execute(routed_command, routed_response)) {
            if (routed_response.length() > 1400) {
              routed_response = routed_response.substring(0, 1400) + "...";
            }
            event_log_append("ROUTE: " + routed_command);
            response = routed_response;
            handled = true;
          }
        }
      }
    }

    // 4. ReAct Agent (if not handled)
    if (!handled && react_agent_should_use(trimmed)) {
      String react_response, react_error;
      event_log_append("ReAct: Starting agent loop");
      if (react_agent_run(trimmed, react_response, react_error)) {
        s_last_llm_response = react_response;
        if (react_response.length() > 1400) {
          react_response = react_response.substring(0, 1400) + "...";
        }
        response = react_response;
        handled = true;
      } else {
        Serial.println("[ReAct] Failed: " + react_error);
      }
    }

    // 5. Direct LLM Chat (if not handled)
    if (!handled) {
      String err;
      if (llm_generate_reply(trimmed, response, err)) {
        s_last_llm_response = response;
        if (response.length() > 1400) {
          response = response.substring(0, 1400) + "...";
        }
        handled = true;
      } else {
        response = "ERR: " + err;
        handled = true;
      }
    }
  }

  status_led_set_busy(false);

  status_led_set_busy(false);

  // Record history (Bot only, User recorded at ingress)
  record_bot_msg(response);
  
  return response;
}

static void on_incoming_message(const String &msg) {
  record_user_msg(msg);
  String reply = agent_loop_process_message(msg);
  if (reply.length() > 0) {
    // Send to Telegram (do NOT record again)
    send_reply_via_telegram(reply);
  }
}

void agent_loop_queue_message(const String &msg) {
  if (msg.length() == 0) return;
  
  // Record User Msg immediately so UI sees it
  record_user_msg(msg);

  if (!s_web_queue_mutex) return;

  if (xSemaphoreTake(s_web_queue_mutex, pdMS_TO_TICKS(100))) {
    s_web_message_queue.push_back(msg);
    xSemaphoreGive(s_web_queue_mutex);
  } else {
    Serial.println("[agent] failed to queue message (mutex timeout)");
  }
}

void agent_loop_init() {
  s_web_queue_mutex = xSemaphoreCreateMutex();
  event_log_init();
  chat_history_init();
  memory_init();
  file_memory_init();  // Initialize SPIFFS-based file memory
  model_config_init();
  persona_init();
#if ENABLE_TASKS
  task_store_init();
#endif
  tool_registry_init();
  react_agent_init();  // Initialize ReAct agent with tool registry
  usage_init();
  status_led_init();
  scheduler_init();
  transport_telegram_init();
  web_server_init();
  Serial.println("[agent] init complete");

  // Check for firmware updates after 30 seconds (gives WiFi time to stabilize)
  // Only check if GITHUB_REPO is configured
  #ifdef GITHUB_REPO
    delay(30000);
    Serial.println("[agent] checking for firmware updates...");
    tool_registry_check_updates_async();
  #endif
}

void agent_loop_tick() {
  status_led_tick();
  transport_telegram_poll(on_incoming_message);
  scheduler_tick(on_incoming_message);

  // Process Web Queue
  String pending_msg = "";
  if (s_web_queue_mutex && xSemaphoreTake(s_web_queue_mutex, 0)) {
    if (!s_web_message_queue.empty()) {
      pending_msg = s_web_message_queue.front();
      s_web_message_queue.erase(s_web_message_queue.begin());
    }
    xSemaphoreGive(s_web_queue_mutex);
  }

  if (pending_msg.length() > 0) {
    // Process message (blocking is fine in main loop)
    agent_loop_process_message(pending_msg);
  }
}
