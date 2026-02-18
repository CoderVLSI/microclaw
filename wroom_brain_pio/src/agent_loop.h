#ifndef AGENT_LOOP_H
#define AGENT_LOOP_H

#include <Arduino.h>

void agent_loop_init();
void agent_loop_tick();

// Get/set the last LLM response (for emailing code)
String agent_loop_get_last_response();
void agent_loop_set_last_response(const String &response);

// Process a message from any source (Web/Telegram) and return the reply
String agent_loop_process_message(const String &msg);

// Queue a message for async processing (Main Loop)
void agent_loop_queue_message(const String &msg);

#endif
