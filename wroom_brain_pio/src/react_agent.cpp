#include "react_agent.h"

#include "brain_config.h"
#include "llm_client.h"
#include "tool_registry.h"
#include "file_memory.h"
#include "event_log.h"

namespace {

// ============================================================================
// TOOL SCHEMA - Available tools for ReAct agent
// ============================================================================

// clang-format off
static const ReactTool s_react_tools[] = {
    // Memory & Knowledge
    {"remember", "Save information to long-term memory (MEMORY.md)", "<text to remember>", "remember: User likes pineapple pizza"},
    {"memory_read", "Read all stored memories from MEMORY.md", "none", "memory_read"},
    {"memory_clear", "Clear all stored memories from MEMORY.md", "none", "memory_clear"},
    {"file_memory", "Show SPIFFS file system info", "none", "file_memory"},
    {"user_read", "Read user profile (USER.md)", "none", "user_read"},
    {"soul_show", "Show current personality/soul (SOUL.md)", "none", "soul_show"},
    {"soul_set", "Set new personality/soul (SOUL.md)", "<soul description>", "soul_set: You are a helpful robot assistant"},
    {"soul_clear", "Clear the soul/personality", "none", "soul_clear"},

    // Task Management
#if ENABLE_TASKS
    {"task_add", "Add a new task to the list", "<task description>", "task_add: Buy groceries tomorrow"},
    {"task_list", "Show all pending tasks", "none", "task_list"},
    {"task_done", "Mark a task as completed", "<task_id>", "task_done: 3"},
    {"task_clear", "Clear all completed tasks", "none", "task_clear"},
#endif

    // Reminders & Scheduling
    {"reminder_set_daily", "Set a daily reminder at specific time", "<HH:MM> <message>", "reminder_set_daily: 09:00 Take morning medicine"},
    {"reminder_show", "Show all active reminders", "none", "reminder_show"},
    {"reminder_clear", "Clear all reminders", "none", "reminder_clear"},
    {"reminder_run", "Manually trigger a reminder check", "none", "reminder_run"},

    // Time & Timezone
    {"time_show", "Show current time and timezone", "none", "time_show"},
    {"timezone_set", "Set user timezone", "<timezone>", "timezone_set: IST"},
    {"timezone_show", "Show current timezone", "none", "timezone_show"},
    {"timezone_clear", "Clear the timezone setting", "none", "timezone_clear"},

    // Email
#if ENABLE_EMAIL
    {"email_draft", "Draft an email (stores for sending)", "<to>|<subject>|<body>", "email_draft: user@example.com|Meeting tomorrow|Can we meet at 2pm?"},
    {"email_show", "Show current email draft", "none", "email_show"},
    {"email_clear", "Clear email draft", "none", "email_clear"},
    {"send_email", "Send an email directly", "<to> <subject> <message>", "send_email: user@example.com Meeting tomorrow Can we meet at 2pm?"},
#endif

    // Image Generation
#if ENABLE_IMAGE_GEN
    {"generate_image", "Generate an image using AI", "<prompt description>", "generate_image: A cute dinosaur robot"},
#endif

    // System Info
    {"status", "Show system status and uptime", "none", "status"},
    {"health", "Show detailed health check", "none", "health"},
    {"specs", "Show hardware/software specifications", "none", "specs"},
    {"usage", "Show token and API usage statistics", "none", "usage"},
    {"usage_reset", "Reset usage statistics", "none", "usage_reset"},
    {"security", "Show security settings and safe mode", "none", "security"},

    // System Controls
    {"logs", "Show recent system logs", "none", "logs"},
    {"logs_clear", "Clear all system logs", "none", "logs_clear"},
    {"safe_mode", "Toggle safe mode on/off", "none", "safe_mode"},
    {"safe_mode_on", "Enable safe mode (confirm required)", "none", "safe_mode_on"},
    {"safe_mode_off", "Disable safe mode", "none", "safe_mode_off"},

    // Firmware Updates
    {"update", "Check for firmware updates from GitHub", "none", "update"},
    {"update", "Update firmware from specific URL", "<url>", "update: https://raw.githubusercontent.com/user/repo/main/firmware.bin"},

    // Model Configuration
    {"model_list", "List available LLM models", "none", "model_list"},
    {"model_status", "Show current model and fallback status", "none", "model_status"},
    {"model_use", "Switch to a different LLM model", "<model_name>", "model_use: gpt-4o-mini"},
    {"model_set", "Configure model with provider and base URL", "<provider>|<model>|<base_url>|<api_key>", "model_set: openai|gpt-4o-mini|https://api.openai.com|sk-xxx"},
    {"model_failed", "Show failed providers", "none", "model_failed"},
    {"model_reset_failed", "Reset failed provider status", "none", "model_reset_failed"},

    // Heartbeat System
    {"heartbeat_show", "Show heartbeat configuration", "none", "heartbeat_show"},
    {"heartbeat_set", "Set heartbeat instructions", "<instructions>", "heartbeat_set: Check health and report any issues"},
    {"heartbeat_clear", "Clear heartbeat configuration", "none", "heartbeat_clear"},

    // Planning (if enabled)
#if ENABLE_PLAN
    {"plan", "Create a plan for a coding task", "<task description>", "plan: Add a new feature for reminders"},
#endif

    // Pending Actions
    {"cancel", "Cancel any pending confirmation", "none", "cancel"},
    {"confirm", "Confirm a pending action", "none", "confirm"},
    {"yes", "Confirm a pending action", "none", "yes"},
};
// clang-format on

static const size_t s_num_tools = sizeof(s_react_tools) / sizeof(s_react_tools[0]);

// ============================================================================
// REACT SYSTEM PROMPTS
// ============================================================================

// Build the ReAct system prompt dynamically (needs iteration count)
String build_react_system_prompt() {
  return "🦖 You are Timi, a clever dinosaur assistant on an ESP32. Think step-by-step!\n\n"
         "Format for each step:\n"
         "🤔 THINK: <what you're analyzing>\n"
         "⚡ DO: <tool_name> <parameters>\n"
         "When done, give final answer:\n"
         "✅ ANSWER: <response to user>\n\n"
         "Guidelines:\n"
         "- Always THINK first, then DO one action\n"
         "- Read tool results, THINK again, continue\n"
         "- Use ANSWER when task is complete\n"
         "- Be brief and helpful\n"
         "- Max " + String(REACT_MAX_ITERATIONS) + " thinking cycles\n\n"
         "Your tools:";
}

// ============================================================================
// REACT STATE
// ============================================================================

struct ReactStep {
  String thought;
  String action;
  String tool_result;
  bool is_final_answer;  // true if this step contains ANSWER
};

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

// Build the tools section of the system prompt
String build_tools_prompt() {
  String tools_text;
  tools_text.reserve(2000);

  for (size_t i = 0; i < s_num_tools; i++) {
    const ReactTool &tool = s_react_tools[i];
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "\n%s: %s\n  Usage: %s\n  Example: %s",
             tool.name, tool.description, tool.parameters, tool.example);
    tools_text += buffer;
  }

  return tools_text;
}

// Parse ReAct response to extract THINK, DO, or ANSWER
bool parse_react_response(const String &response, ReactStep &step, String &error) {
  step.is_final_answer = false;
  step.thought = "";
  step.action = "";
  step.tool_result = "";

  String resp = response;
  resp.trim();

  // Check for ANSWER first (final response)
  int answer_pos = resp.indexOf("ANSWER:");
  if (answer_pos >= 0) {
    step.is_final_answer = true;
    step.thought = resp.substring(answer_pos + 7);  // After "ANSWER:"
    step.thought.trim();
    return true;
  }

  // Also check for ✅ ANSWER format
  int answer_emoji = resp.indexOf("✅");
  if (answer_emoji >= 0) {
    int after_emoji = answer_emoji + 3;  // Skip emoji
    int ans_start = resp.indexOf("ANSWER:", after_emoji);
    if (ans_start >= 0) {
      step.is_final_answer = true;
      step.thought = resp.substring(ans_start + 7);
      step.thought.trim();
      return true;
    }
  }

  // Check for THINK (also support THOUGHT for compatibility)
  int think_pos = resp.indexOf("THINK:");
  if (think_pos < 0) {
    think_pos = resp.indexOf("THOUGHT:");
  }
  if (think_pos >= 0) {
    int think_end = resp.indexOf("\n", think_pos);
    if (think_end < 0) {
      think_end = resp.length();
    }
    // Skip past the colon
    int colon_pos = resp.indexOf(":", think_pos);
    step.thought = resp.substring(colon_pos + 1, think_end);
    step.thought.trim();
  }

  // Check for DO (also support ACTION for compatibility)
  int do_pos = resp.indexOf("⚡");
  if (do_pos >= 0) {
    // Find next word after emoji
    int do_start = resp.indexOf("DO:", do_pos);
    if (do_start < 0) {
      do_start = resp.indexOf("ACTION:", do_pos);
    }
    if (do_start >= 0) {
      int do_end = resp.indexOf("\n", do_start);
      if (do_end < 0) {
        do_end = resp.length();
      }
      int colon_pos = resp.indexOf(":", do_start);
      step.action = resp.substring(colon_pos + 1, do_end);
      step.action.trim();
      return step.action.length() > 0;
    }
  }

  // Fallback: check for ACTION: directly
  int action_pos = resp.indexOf("ACTION:");
  if (action_pos >= 0) {
    int action_end = resp.indexOf("\n", action_pos);
    if (action_end < 0) {
      action_end = resp.length();
    }
    step.action = resp.substring(action_pos + 7, action_end);
    step.action.trim();
    return step.action.length() > 0;
  }

  error = "Invalid ReAct format: missing DO/ACTION or ANSWER";
  return false;
}

// Execute an ACTION by calling the tool registry
bool execute_tool_action(const String &action, String &result, String &error) {
  String action_trimmed = action;
  action_trimmed.trim();

  // Extract tool name and parameters
  int space_pos = action_trimmed.indexOf(' ');
  String tool_name;
  String params;

  if (space_pos > 0) {
    tool_name = action_trimmed.substring(0, space_pos);
    params = action_trimmed.substring(space_pos + 1);
  } else {
    tool_name = action_trimmed;
  }

  tool_name.trim();
  params.trim();

  // Build command string for tool registry
  String command = tool_name;
  if (params.length() > 0) {
    command += " " + params;
  }

  event_log_append("[ReAct] Executing: " + command);

  // Execute via tool registry
  if (!tool_registry_execute(command, result)) {
    error = "Tool not found or failed: " + tool_name;
    return false;
  }

  // Truncate long responses
  if (result.length() > REACT_TOOL_RESPONSE_MAX_CHARS) {
    result = result.substring(0, REACT_TOOL_RESPONSE_MAX_CHARS) + "...(truncated)";
  }

  return true;
}

// Build context for LLM (previous steps)
String build_react_context(const String &user_query, const ReactStep *steps,
                           int step_count, const String &tools_prompt) {
  String context;
  context.reserve(4000);

  context += build_react_system_prompt();
  context += tools_prompt;
  context += "\n\n=== Conversation ===\n";
  context += "👤 User: " + user_query + "\n\n";

  // Add previous steps
  for (int i = 0; i < step_count; i++) {
    context += "🤔 THINK: " + steps[i].thought + "\n";
    if (steps[i].is_final_answer) {
      context += "✅ ANSWER: " + steps[i].thought + "\n";
    } else {
      context += "⚡ DO: " + steps[i].action + "\n";
      context += "📊 Result: " + steps[i].tool_result + "\n\n";
    }
  }

  context += "\nYour next response:";

  return context;
}

}  // namespace

// ============================================================================
// PUBLIC API
// ============================================================================

void react_agent_init() {
  Serial.println("[ReAct] Agent initialized with " + String(s_num_tools) + " tools");
}

bool react_agent_should_use(const String &query) {
  // Use ReAct for complex queries that suggest multi-step reasoning
  String lc = query;
  lc.toLowerCase();

  // Keywords that suggest multi-step reasoning
  const char *complex_keywords[] = {
      "how do i", "help me", "what should", "can you", "i need to",
      "remember to", "set up", "configure", "schedule", "remind me to",
      "figure out", "find out", "check if", "make sure", "todo", "task",
      "plan", "organize", "track"
  };

  for (size_t i = 0; i < sizeof(complex_keywords) / sizeof(complex_keywords[0]); i++) {
    if (lc.indexOf(complex_keywords[i]) >= 0) {
      return true;
    }
  }

  return false;
}

bool react_agent_run(const String &user_query, String &response_out,
                     String &error_out) {
  ReactStep steps[REACT_MAX_ITERATIONS];
  int step_count = 0;
  String tools_prompt = build_tools_prompt();

  Serial.println("[ReAct] Starting for: " + user_query);

  for (int iter = 0; iter < REACT_MAX_ITERATIONS; iter++) {
    // Build context with all previous steps
    String context = build_react_context(user_query, steps, step_count, tools_prompt);

    // Call LLM
    String llm_response, llm_error;
    if (!llm_generate_with_custom_prompt(context, "", true, llm_response, llm_error)) {
      error_out = "LLM call failed: " + llm_error;
      return false;
    }

    Serial.printf("[ReAct] Iteration %d response: %s\n", iter + 1,
                  llm_response.substring(0, 100).c_str());

    // Parse response
    ReactStep step;
    String parse_error;
    if (!parse_react_response(llm_response, step, parse_error)) {
      // If parsing fails, treat LLM response as final answer
      response_out = llm_response;
      Serial.println("[ReAct] Parse failed, using LLM response as answer");
      return true;
    }

    // Check if this is the final answer
    if (step.is_final_answer) {
      response_out = step.thought;
      Serial.println("[ReAct] Final answer received");
      return true;
    }

    // Execute the action
    String tool_result, tool_error;
    if (!execute_tool_action(step.action, tool_result, tool_error)) {
      // Tool execution failed - feed error back to LLM
      step.tool_result = "ERROR: " + tool_error;
      Serial.println("[ReAct] Tool error: " + tool_error);
    } else {
      step.tool_result = tool_result;
      Serial.printf("[ReAct] Tool result: %s\n", tool_result.substring(0, 80).c_str());
    }

    steps[step_count++] = step;
  }

  // Max iterations reached - ask LLM for final summary
  String summary_context = build_react_system_prompt() + build_tools_prompt() +
      "\n\n=== Conversation ===\n👤 User: " + user_query + "\n\n";

  for (int i = 0; i < step_count; i++) {
    summary_context += "🤔 THINK: " + steps[i].thought + "\n";
    summary_context += "⚡ DO: " + steps[i].action + "\n";
    summary_context += "📊 Result: " + steps[i].tool_result + "\n\n";
  }

  summary_context += "\nMax thinking cycles reached. Give your final ✅ ANSWER:";

  String final_response, final_error;
  if (llm_generate_with_custom_prompt(summary_context, "", true, final_response, final_error)) {
    response_out = final_response;
  } else {
    response_out = "I need more iterations to complete this task. Try being more specific.";
  }

  return true;
}
