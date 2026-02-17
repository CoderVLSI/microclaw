#ifndef REACT_AGENT_H
#define REACT_AGENT_H

#include <Arduino.h>

// Maximum iterations for ReAct loop (prevents infinite loops)
#ifndef REACT_MAX_ITERATIONS
#define REACT_MAX_ITERATIONS 5
#endif

// Maximum tokens for tool responses
#ifndef REACT_TOOL_RESPONSE_MAX_CHARS
#define REACT_TOOL_RESPONSE_MAX_CHARS 600
#endif

// Tool definition for ReAct
struct ReactTool {
  const char *name;           // Tool identifier (e.g., "task_add")
  const char *description;    // What the tool does
  const char *parameters;     // Parameter format (e.g., "<task_text>")
  const char *example;        // Example usage
};

// Initialize ReAct agent with tool registry
void react_agent_init();

// Run ReAct loop for a user query
// Returns true if successful, false on error
// response_out contains the final answer or error message
bool react_agent_run(const String &user_query, String &response_out, String &error_out);

// Check if a query should use ReAct (complex reasoning needed)
bool react_agent_should_use(const String &query);

#endif
