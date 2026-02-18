#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <Arduino.h>

// Initialize web server on port 80
void web_server_init();

// Get the URL where the server is running
String web_server_get_url();

// Serve a file with content (saves to SPIFFS and makes available via web)
bool web_server_publish_file(const String &filename, const String &content, const String &mime_type);

#endif
