/************************************************************
  ESP32 WATER LEVEL MONITOR
  -----------------------------------------------------------
  FEATURES
  - Reads water level using an ultrasonic sensor
  - Sends distance (cm) to Blynk
  - Sends water level (%) to Blynk
  - Sends Telegram alerts for LOW, CRITICAL, and HIGH levels
  - Responds to Telegram commands:
      /start
      /status
      /distance
      /level

  IMPORTANT
  - Use 2.4 GHz Wi-Fi only
  - Replace the credentials below before uploading
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
const char* WIFI_SSID     = "Vodafone_190785";
const char* WIFI_PASSWORD = "11111111";

// Telegram bot settings
const char* BOT_TOKEN    = "8614046226:AAEsAmvQRnFleZU1N4yT-a7eduwLvms8pVc";
const char* BOT_USERNAME = "ESP32_Water_data_bot";  // without @

// Telegram group chat ID
String ALLOWED_CHAT_ID = "-5126807063";

// Ultrasonic sensor pins
constexpr uint8_t TRIG_PIN = 4;
constexpr uint8_t ECHO_PIN = 16;

// Tank calibration
// MAX_DEPTH_CM = sensor-to-bottom distance when tank is empty
// MIN_DISTANCE_CM = sensor-to-water distance when tank is full
constexpr float MAX_DEPTH_CM    = 17.0;
constexpr float MIN_DISTANCE_CM = 2.5;

// Alert thresholds
constexpr int CRITICAL_LEVEL_THRESHOLD = 10;
constexpr int LOW_LEVEL_THRESHOLD      = 25;
constexpr int HIGH_LEVEL_THRESHOLD     = 85;

// Task timing
constexpr unsigned long SENSOR_INTERVAL_MS   = 2000;
constexpr unsigned long BLYNK_INTERVAL_MS    = 5000;
constexpr unsigned long ALERT_INTERVAL_MS    = 000;
constexpr unsigned long TELEGRAM_INTERVAL_MS = 3000;

// Reconnection timing
constexpr unsigned long WIFI_RETRY_INTERVAL_MS  = 30000;
constexpr unsigned long BLYNK_RETRY_INTERVAL_MS = 10000;

// ==========================================================
// GLOBAL OBJECTS
// ==========================================================

BlynkTimer timer;
WiFiClientSecure secureClient;
UniversalTelegramBot bot(BOT_TOKEN, secureClient);

// ==========================================================
// SHARED STATE
// ==========================================================

float currentDistance = -1.0;
int currentPercent    = -1;

bool criticalAlertSent = false;
bool lowAlertSent      = false;
bool highAlertSent     = false;

unsigned long lastWiFiRetry  = 0;
unsigned long lastBlynkRetry = 0;

// ==========================================================
// HELPER FUNCTIONS
// ==========================================================

bool hasValidReading() {
  return currentDistance >= 0 && currentPercent >= 0;
}

void printWiFiStatus() {
  static wl_status_t lastStatus = WL_NO_SHIELD;

  wl_status_t status = WiFi.status();

  if (status != lastStatus) {
    lastStatus = status;

    Serial.print("Wi-Fi status changed: ");
    Serial.println(status);

    if (status == WL_CONNECTED) {
      Serial.print("Wi-Fi connected. IP: ");
      Serial.println(WiFi.localIP());
    }
  }
}

void startWiFiOnce() {
  Serial.println();
  Serial.print("Starting Wi-Fi connection to: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiRetry = millis();
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  unsigned long now = millis();

  if (now - lastWiFiRetry >= WIFI_RETRY_INTERVAL_MS) {
    lastWiFiRetry = now;

    Serial.println();
    Serial.println("Wi-Fi not connected. Retrying...");

    /*
      Important:
      We use WiFi.reconnect() here instead of repeatedly calling WiFi.begin().
      This prevents:
      wifi:sta is connecting, cannot set config
    */
    WiFi.reconnect();
  }
}

void connectBlynk() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (Blynk.connected()) {
    return;
  }

  unsigned long now = millis();

  if (now - lastBlynkRetry < BLYNK_RETRY_INTERVAL_MS) {
    return;
  }

  lastBlynkRetry = now;

  Serial.println("Connecting to Blynk...");
  Blynk.connect(1000);

  if (Blynk.connected()) {
    Serial.println("Blynk connected.");
  } else {
    Serial.println("Blynk connection failed.");
  }
}

float readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);

  if (duration == 0) {
    return -1.0;
  }

  return duration * 0.0343f / 2.0f;
}

int calculateWaterPercent(float distance) {
  if (distance < 0) {
    return -1;
  }

  distance = constrain(distance, MIN_DISTANCE_CM, MAX_DEPTH_CM);

  float percent = ((MAX_DEPTH_CM - distance) / (MAX_DEPTH_CM - MIN_DISTANCE_CM)) * 100.0f;

  return constrain((int)percent, 0, 100);
}

String buildStatusMessage() {
  if (!hasValidReading()) {
    return "Sensor error: unable to read water level.";
  }

  return "Water Level Status\n\n"
         "Distance: " + String(currentDistance, 1) + " cm\n" +
         "Level: " + String(currentPercent) + "%";
}

String getCommandListMessage() {
  return "ESP32 Water Level Bot\n\n"
         "Approved commands:\n"
         "/start - show this command list\n"
         "/status - show full water level status\n"
         "/distance - show sensor distance in cm\n"
         "/level - show water level percentage";
}

void sendTelegramMessage(const String& text) {
  if (WiFi.status() == WL_CONNECTED) {
    bot.sendMessage(ALLOWED_CHAT_ID, text, "");
  }
}

void resetAlertsIfNormal() {
  if (currentPercent > LOW_LEVEL_THRESHOLD && currentPercent < HIGH_LEVEL_THRESHOLD) {
    criticalAlertSent = false;
    lowAlertSent = false;
    highAlertSent = false;
  }
}

// ==========================================================
// TASK FUNCTIONS
// ==========================================================

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

void taskSendBlynk() {
  if (!hasValidReading()) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (!Blynk.connected()) {
    return;
  }

  Blynk.virtualWrite(V0, currentDistance);
  Blynk.virtualWrite(V1, currentPercent);
}

void taskCheckAlerts() {
  if (!hasValidReading()) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (currentPercent <= CRITICAL_LEVEL_THRESHOLD && !criticalAlertSent) {
    sendTelegramMessage(
      "URGENT: Water level is CRITICAL.\n\n"
      "Level: " + String(currentPercent) + "%\n" +
      "Distance: " + String(currentDistance, 1) + " cm\n\n" +
      "Immediate action required: The tank is almost empty. Team, contact the water suppliers now and limit water usage where possible."
    );

    criticalAlertSent = true;
    lowAlertSent = true;
    highAlertSent = false;
    return;
  }

  if (currentPercent <= LOW_LEVEL_THRESHOLD && !lowAlertSent) {
    sendTelegramMessage(
      "Warning: Water level is LOW.\n\n"
      "Level: " + String(currentPercent) + "%\n" +
      "Distance: " + String(currentDistance, 1) + " cm\n\n" +
      "Action required: Team, please contact the water suppliers for refill."
    );

    lowAlertSent = true;
    highAlertSent = false;
    return;
  }

  if (currentPercent >= HIGH_LEVEL_THRESHOLD && !highAlertSent) {
    sendTelegramMessage(
      "Alert: Water level is HIGH.\n\n"
      "Level: " + String(currentPercent) + "%\n" +
      "Distance: " + String(currentDistance, 1) + " cm\n\n" +
      "Action required: Please monitor the tank to prevent overflow."
    );

    highAlertSent = true;
    lowAlertSent = false;
    criticalAlertSent = false;
    return;
  }

  resetAlertsIfNormal();
}

void handleTelegramMessages(int messageCount) {
  for (int i = 0; i < messageCount; i++) {
    String chatID = bot.messages[i].chat_id;
    String senderName = bot.messages[i].from_name;
    String text = bot.messages[i].text;

    text.trim();

    if (chatID != ALLOWED_CHAT_ID) {
      continue;
    }

    Serial.print("[Telegram] ");
    Serial.print(senderName);
    Serial.print(": ");
    Serial.println(text);

    bool mentionsBot = text.indexOf("@" + String(BOT_USERNAME)) >= 0;

    text.replace("@" + String(BOT_USERNAME), "");
    text.trim();

    if (text == "/start") {
      bot.sendMessage(chatID, getCommandListMessage(), "");
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
      if (mentionsBot) {
        bot.sendMessage(chatID, getCommandListMessage(), "");
      }
    }
  }
}

void taskCheckTelegram() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

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

  Serial.println();
  Serial.println("ESP32 Water Level Monitor Starting...");

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  secureClient.setInsecure();

  Blynk.config(BLYNK_AUTH_TOKEN);

  startWiFiOnce();

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
  maintainWiFi();
  printWiFiStatus();

  if (WiFi.status() == WL_CONNECTED) {
    connectBlynk();

    if (Blynk.connected()) {
      Blynk.run();
    }
  }

  timer.run();
}