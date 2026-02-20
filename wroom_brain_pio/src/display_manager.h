#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <Arduino.h>

enum FaceExpression {
    FACE_IDLE,      // Neutral eyes
    FACE_THINKING,  // Scanning / Processing
    FACE_HAPPY,     // Happy eyes (success)
    FACE_SLEEP,     // Closed eyes (low power / night)
    FACE_ERROR,     // X eyes (error)
    FACE_LISTENING  // Wide eyes (awaiting input)
};

// Initialize display and draw boot screen
void display_manager_init();

// Main loop update (animations, message timeout)
void display_manager_update();

// Set the current facial expression
void display_manager_set_face(FaceExpression expr);

// Show a temporary text message (overlay)
void display_manager_show_message(const String &msg, int duration_ms = 3000);

// Update status bar indicators
void display_manager_set_wifi_status(bool connected);
void display_manager_set_time(const String &time_str);

#endif
