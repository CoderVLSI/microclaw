#ifndef TOOL_REGISTRY_H
#define TOOL_REGISTRY_H

#include <Arduino.h>

void tool_registry_init();
bool tool_registry_execute(const String &input, String &out);

// Auto-update check on boot (async, sends notification if update available)
void tool_registry_check_updates_async();

// Trigger the pending firmware update (returns true if update started)
bool tool_registry_trigger_update(String &out);

#endif
