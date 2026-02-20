#include "display_manager.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

// Pin Definitions
#define TFT_CS     5
#define TFT_RST    4
#define TFT_DC     17
// MOSI=23, SCLK=18 are default VSPI pins on ESP32
#define TFT_BACKLIGHT 32 // Connect LED pin here

static Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

// Face animation state
static FaceExpression s_current_face = FACE_IDLE;
static bool s_wifi_connected = false;
static String s_last_time_str = "";
static String s_status_msg = "";
static unsigned long s_msg_expire_ms = 0;
static int s_blink_state = 0; // 0=open, 1=closing, 2=closed, 3=opening
static unsigned long s_next_blink_ms = 0;

// Helper to draw two eyes
static void draw_eyes_rect(int height, uint16_t color) {
  int center_y = 64;
  int eye_w = 24;
  int max_h = 50;
  int gap = 20;
  
  int x1 = (128 - (eye_w * 2) - gap) / 2;
  int x2 = x1 + eye_w + gap;
  
  // Clear area around eyes
  tft.fillRect(x1 - 2, center_y - (max_h/2) - 2, eye_w + 4, max_h + 4, ST77XX_BLACK);
  tft.fillRect(x2 - 2, center_y - (max_h/2) - 2, eye_w + 4, max_h + 4, ST77XX_BLACK);

  if (height > 0) {
    int h = height;
    if (h > max_h) h = max_h;
    int y_off = (max_h - h) / 2;
    // Draw eyes
    tft.fillRoundRect(x1, center_y - (max_h/2) + y_off, eye_w, h, 8, color);
    tft.fillRoundRect(x2, center_y - (max_h/2) + y_off, eye_w, h, 8, color);
  }
}


void display_manager_init() {
  pinMode(TFT_BACKLIGHT, OUTPUT);
  digitalWrite(TFT_BACKLIGHT, HIGH); // Turn on backlight

  // Init ST7735 1.44" (Green Tab usually)
  tft.initR(INITR_144GREENTAB); 
  tft.setRotation(2); // Adjust if upside down
  tft.fillScreen(ST77XX_BLACK);
  
  // Draw Boot Logo
  tft.setTextColor(ST77XX_GREEN);
  tft.setTextSize(2);
  tft.setCursor(30, 50);
  tft.print("MICRO");
  tft.setCursor(35, 70);
  tft.print("CLAW");
  delay(1000);
  tft.fillScreen(ST77XX_BLACK);

  s_next_blink_ms = millis() + 3000;
  display_manager_set_face(FACE_IDLE);
}

void display_manager_update() {
  unsigned long now = millis();

  // 1. Handle Message Timeout
  if (s_status_msg.length() > 0 && now > s_msg_expire_ms) {
    s_status_msg = "";
    // Clear message area (bottom 30px)
    tft.fillRect(0, 98, 128, 30, ST77XX_BLACK);
  }

  // 2. Handle Blink Animation (Only in IDLE mode)
  if (s_current_face == FACE_IDLE) {
    if (s_blink_state == 0 && now > s_next_blink_ms) {
      s_blink_state = 1; // Start closing
    } else if (s_blink_state == 1) {
      draw_eyes_rect(10, ST77XX_CYAN); // Half closed
      s_blink_state = 2;
      s_next_blink_ms = now + 40;
    } else if (s_blink_state == 2 && now > s_next_blink_ms) {
      draw_eyes_rect(2, ST77XX_CYAN); // Closed (thin line)
      s_blink_state = 3;
      s_next_blink_ms = now + 80;
    } else if (s_blink_state == 3 && now > s_next_blink_ms) {
      draw_eyes_rect(45, ST77XX_CYAN); // Open
      s_blink_state = 0;
      s_next_blink_ms = now + random(2000, 6000);
    }
  } 
  else if (s_current_face == FACE_THINKING) {
    // Thinking animation
    if ((now / 300) % 2 == 0) {
      draw_eyes_rect(35, ST77XX_ORANGE);
    } else {
      draw_eyes_rect(45, ST77XX_YELLOW);
    }
  }

  // 3. Update Message Overlay
  if (s_status_msg.length() > 0) {
     tft.setTextSize(1);
     tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
     // Center text approximation
     int len = s_status_msg.length();
     int x = (128 - (len * 6)) / 2;
     if (x < 0) x = 0;
     tft.setCursor(x, 110);
     tft.print(s_status_msg);
  }
}

void display_manager_set_face(FaceExpression expr) {
  if (s_current_face == expr) return;
  s_current_face = expr;
  
  // Clear eyes area for clean switch
  tft.fillRect(0, 20, 128, 80, ST77XX_BLACK);
  
  if (expr == FACE_IDLE) {
    draw_eyes_rect(45, ST77XX_CYAN);
    s_blink_state = 0;
    s_next_blink_ms = millis() + 2000;
  } else if (expr == FACE_HAPPY) {
    // Squinty happy eyes
    draw_eyes_rect(20, ST77XX_GREEN);
  } else if (expr == FACE_THINKING) {
    draw_eyes_rect(45, ST77XX_ORANGE);
  } else if (expr == FACE_ERROR) {
    draw_eyes_rect(45, ST77XX_RED);
  } else if (expr == FACE_SLEEP) {
    draw_eyes_rect(4, ST77XX_BLUE); // Just lines
  }
}

void display_manager_show_message(const String &msg, int duration_ms) {
    s_status_msg = msg;
    if (s_status_msg.length() > 20) {
        s_status_msg = s_status_msg.substring(0, 17) + "...";
    }
    s_msg_expire_ms = millis() + duration_ms;
    
    // Clear bottom area
    tft.fillRect(0, 98, 128, 30, ST77XX_BLACK);
    // Draw immediately
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK); 
    int len = s_status_msg.length();
    int x = (128 - (len * 6)) / 2;
    if (x < 0) x = 0;
    tft.setCursor(x, 110);
    tft.print(s_status_msg);
}

void display_manager_set_wifi_status(bool connected) {
    s_wifi_connected = connected;
    // Draw small dot at top right
    tft.fillCircle(120, 8, 3, connected ? ST77XX_GREEN : ST77XX_RED);
}

void display_manager_set_time(const String &time_str) {
    if (s_last_time_str == time_str) return;
    s_last_time_str = time_str;
    
    // Draw time at top center
    tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    tft.setTextSize(1);
    // Clear time area
    tft.fillRect(30, 0, 68, 16, ST77XX_BLACK);
    tft.setCursor(45, 2);
    tft.print(time_str);
}
