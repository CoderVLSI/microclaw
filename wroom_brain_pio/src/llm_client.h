#ifndef LLM_CLIENT_H
#define LLM_CLIENT_H

#include <Arduino.h>

// Generate text with a custom system prompt (for ReAct agent, etc.)
// Returns true on success, false on error
bool llm_generate_with_custom_prompt(const String &system_prompt, const String &task,
                                     bool include_memory, String &reply_out, String &error_out);

bool llm_generate_plan(const String &task, String &plan_out, String &error_out);
bool llm_generate_reply(const String &message, String &reply_out, String &error_out);
bool llm_generate_heartbeat(const String &heartbeat_doc, String &reply_out, String &error_out);
bool llm_route_tool_command(const String &message, String &command_out, String &error_out);
bool llm_generate_image(const String &prompt, String &base64_out, String &error_out);
bool llm_understand_media(const String &instruction, const String &mime_type,
                          const String &base64_data, String &reply_out, String &error_out);
bool llm_parse_email_request(const String &message, String &to_out, String &subject_out,
                             String &body_out, String &error_out);
bool llm_parse_update_request(const String &message, String &url_out, bool &should_update_out,
                              bool &check_github_out, String &error_out);

#endif
