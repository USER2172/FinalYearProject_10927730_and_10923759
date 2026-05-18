/************************************************************
  ESP32 WATER LEVEL MONITOR
  -----------------------------------------------------------
  FEATURES
  - Reads water level using an ultrasonic sensor
  - Sends distance (cm) to Blynk
  - Sends water level (%) to Blynk
  - Sends Telegram alerts for LOW and HIGH water levels
  - Responds to Telegram commands:
      /start
      /status
      /distance
      /level

  DESIGN GOAL
  - Keep sensor logic, Blynk logic, and Telegram logic separate
  - Reduce repeated code
  - Make the sketch easier to maintain

  IMPORTANT
  - Replace Wi-Fi, Blynk token, and Telegram bot token
  - Keep secrets out of shared code
************************************************************/

#define BLYNK_TEMPLATE_ID   "TMPL2FnrNtSp5"
#define BLYNK_TEMPLATE_NAME "Water Level"
#define BLYNK_AUTH_TOKEN    "H4qMxm8rUuPm7zyKodK1hgWHBHkElhI5"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <BlynkSimpleEsp32.h>
#include <UniversalTelegramBot.h>

// ==========================================================
// USER SETTINGS
// ==========================================================

// Wi-Fi credentials
const char* WIFI_SSID     = "EURONOLI";
const char* WIFI_PASSWORD = "T92S7EBNAV";

// Telegram bot settings
const char* BOT_TOKEN = "8614046226:AAEsAmvQRnFleZU1N4yT-a7eduwLvms8pVc";
String ALLOWED_CHAT_ID = "-5126807063";   // e.g. "-100xxxxxxxxxx" for groups

// Ultrasonic sensor pins
constexpr uint8_t TRIG_PIN = 4;
constexpr uint8_t ECHO_PIN = 16;

// Tank calibration
// MAX_DEPTH_CM = sensor-to-bottom distance when tank is empty
// MIN_DISTANCE_CM = sensor-to-water distance when tank is full
constexpr float MAX_DEPTH_CM    = 17.0;
constexpr float MIN_DISTANCE_CM = 2.5;

// Alert thresholds
constexpr int LOW_LEVEL_THRESHOLD  = 20;   // %
constexpr int HIGH_LEVEL_THRESHOLD = 80;   // %

// Task timing
constexpr unsigned long SENSOR_INTERVAL_MS   = 2000;
constexpr unsigned long BLYNK_INTERVAL_MS    = 2500;
constexpr unsigned long ALERT_INTERVAL_MS    = 2000;
constexpr unsigned long TELEGRAM_INTERVAL_MS = 1000;

// ==========================================================
// GLOBAL OBJECTS
// ==========================================================

BlynkTimer timer;
WiFiClientSecure secureClient;
UniversalTelegramBot bot(BOT_TOKEN, secureClient);

// ==========================================================
// SHARED STATE
// ==========================================================

// Latest sensor values
float currentDistance = -1.0;
int currentPercent    = -1;

// Alert memory so the same alert is not spammed repeatedly
bool lowAlertSent  = false;
bool highAlertSent = false;

// ==========================================================
// HELPER FUNCTIONS
// ==========================================================

/**
 * Returns true if the latest sensor values are usable.
 */
bool hasValidReading() {
  return currentDistance >= 0 && currentPercent >= 0;
}

/**
 * Connects to Wi-Fi if not already connected.
 * Uses a timeout so setup/loop does not get stuck forever.
 */
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.print("Connecting to Wi-Fi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi connection failed");
  }
}

/**
 * Tries to connect Blynk without blocking for too long.
 */
void connectBlynk() {
  if (WiFi.status() != WL_CONNECTED || Blynk.connected()) return;

  Serial.println("Connecting to Blynk...");
  Blynk.connect(1000);
}

/**
 * Reads distance from the ultrasonic sensor in cm.
 * Returns:
 *   > 0   = valid reading
 *   -1.0  = timeout / failed reading
 */
float readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return -1.0;

  return duration * 0.0343f / 2.0f;
}

/**
 * Converts measured distance to water percentage.
 * 0%   = empty tank
 * 100% = full tank
 */
int calculateWaterPercent(float distance) {
  if (distance < 0) return -1;

  distance = constrain(distance, MIN_DISTANCE_CM, MAX_DEPTH_CM);

  float percent = ((MAX_DEPTH_CM - distance) / (MAX_DEPTH_CM - MIN_DISTANCE_CM)) * 100.0f;
  return constrain((int)percent, 0, 100);
}

/**
 * Builds a general status message for Telegram.
 */
String buildStatusMessage() {
  if (!hasValidReading()) {
    return "Sensor error: unable to read water level.";
  }

  return "Water Level Status\n"
         "Distance: " + String(currentDistance, 1) + " cm\n" +
         "Level: " + String(currentPercent) + "%";
}

/**
 * Sends a Telegram message only if Wi-Fi is available.
 */
void sendTelegramMessage(const String& text) {
  if (WiFi.status() == WL_CONNECTED) {
    bot.sendMessage(ALLOWED_CHAT_ID, text, "");
  }
}

/**
 * Resets alert flags when water level returns to normal range.
 */
void resetAlertsIfNormal() {
  if (currentPercent > LOW_LEVEL_THRESHOLD && currentPercent < HIGH_LEVEL_THRESHOLD) {
    lowAlertSent = false;
    highAlertSent = false;
  }
}

// ==========================================================
// TASK FUNCTIONS
// ==========================================================

/**
 * Task 1: Read the sensor and update shared values.
 * This task does not send anything anywhere.
 */
void taskReadSensor() {
  currentDistance = readDistanceCm();
  currentPercent  = calculateWaterPercent(currentDistance);

  if (!hasValidReading()) {
    Serial.println("Sensor timeout/error");
    return;
  }

  Serial.print("Distance: ");
  Serial.print(currentDistance, 2);
  Serial.print(" cm | Level: ");
  Serial.print(currentPercent);
  Serial.println("%");
}

/**
 * Task 2: Push latest values to Blynk.
 * This is separate from the sensor-reading task.
 */
void taskSendBlynk() {
  if (!hasValidReading() || !Blynk.connected()) return;

  Blynk.virtualWrite(V0, currentDistance);
  Blynk.virtualWrite(V1, currentPercent);
}

/**
 * Task 3: Check whether a LOW or HIGH alert should be sent.
 * Prevents alert spam using flag memory.
 */
void taskCheckAlerts() {
  if (!hasValidReading()) return;

  if (currentPercent <= LOW_LEVEL_THRESHOLD && !lowAlertSent) {
    sendTelegramMessage(
      "Warning: Water level is LOW.\n"
      "Level: " + String(currentPercent) + "%\n"
      "Distance: " + String(currentDistance, 1) + " cm"
    );
    lowAlertSent = true;
    highAlertSent = false;
    return;
  }

  if (currentPercent >= HIGH_LEVEL_THRESHOLD && !highAlertSent) {
    sendTelegramMessage(
      "Alert: Water level is HIGH.\n"
      "Level: " + String(currentPercent) + "%\n"
      "Distance: " + String(currentDistance, 1) + " cm"
    );
    highAlertSent = true;
    lowAlertSent = false;
    return;
  }

  resetAlertsIfNormal();
}

/**
 * Handles incoming Telegram commands from approved chat only.
 */
void handleTelegramMessages(int messageCount) {
  for (int i = 0; i < messageCount; i++) {
    String chatID = bot.messages[i].chat_id;
    String text   = bot.messages[i].text;
    text.trim();

    if (chatID != ALLOWED_CHAT_ID) continue;

    if (text == "/start") {
      bot.sendMessage(
        chatID,
        "ESP32 Water Level Bot\n\n"
        "Available commands:\n"
        "/start - show commands\n"
        "/status - full sensor status\n"
        "/distance - distance in cm\n"
        "/level - water level percentage",
        ""
      );
    }
    else if (text == "/status") {
      bot.sendMessage(chatID, buildStatusMessage(), "");
    }
    else if (text == "/distance") {
      bot.sendMessage(
        chatID,
        hasValidReading()
          ? "Distance: " + String(currentDistance, 1) + " cm"
          : "Sensor error.",
        ""
      );
    }
    else if (text == "/level") {
      bot.sendMessage(
        chatID,
        hasValidReading()
          ? "Water level: " + String(currentPercent) + "%"
          : "Sensor error.",
        ""
      );
    }
    else {
      bot.sendMessage(chatID, "Unknown command. Use /start", "");
    }
  }
}

/**
 * Task 4: Poll Telegram for new messages.
 */
void taskCheckTelegram() {
  if (WiFi.status() != WL_CONNECTED) return;

  int count = bot.getUpdates(bot.last_message_received + 1);
  while (count > 0) {
    handleTelegramMessages(count);
    count = bot.getUpdates(bot.last_message_received + 1);
  }
}

// ==========================================================
// SETUP
// ==========================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\nESP32 Water Level Monitor Starting...");

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  connectWiFi();

  // Telegram HTTPS client
  secureClient.setInsecure();

  // Blynk setup
  Blynk.config(BLYNK_AUTH_TOKEN);
  connectBlynk();

  // Schedule independent tasks
  timer.setInterval(SENSOR_INTERVAL_MS,   taskReadSensor);
  timer.setInterval(BLYNK_INTERVAL_MS,    taskSendBlynk);
  timer.setInterval(ALERT_INTERVAL_MS,    taskCheckAlerts);
  timer.setInterval(TELEGRAM_INTERVAL_MS, taskCheckTelegram);

  Serial.println("Setup complete.");
}

// ==========================================================
// MAIN LOOP
// ==========================================================

void loop() {
  connectWiFi();
  connectBlynk();

  Blynk.run();   // Keeps Blynk alive
  timer.run();   // Runs scheduled tasks
}