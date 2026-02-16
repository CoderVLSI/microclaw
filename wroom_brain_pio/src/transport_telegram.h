#ifndef TRANSPORT_TELEGRAM_H
#define TRANSPORT_TELEGRAM_H

#include <Arduino.h>

typedef void (*incoming_cb_t)(const String &msg);

void transport_telegram_init();
void transport_telegram_poll(incoming_cb_t cb);
void transport_telegram_send(const String &msg);
bool transport_telegram_send_document(const String &filename, const String &content,
                                      const String &mime_type, const String &caption);
bool transport_telegram_send_document_base64(const String &filename, const String &base64_content,
                                             const String &mime_type, const String &caption);
bool transport_telegram_get_last_photo_base64(String &mime_out, String &base64_out, String &error_out);
bool transport_telegram_get_last_document_base64(String &filename_out, String &mime_out,
                                                 String &base64_out, String &error_out);
bool transport_telegram_send_photo_base64(const String &base64_data, const String &caption);

#endif
