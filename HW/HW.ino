#include <WiFi.h>
#include <FirebaseESP32.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "time.h"

// WiFi / Firebase
#define WIFI_SSID "minhphuong"
#define WIFI_PASSWORD "01010000"
#define FIREBASE_HOST "https://project2-cd9c9-default-rtdb.firebaseio.com/"
#define FIREBASE_AUTH "tsnWspwpI5U5nuM1H5FfVbFJgS3ASCravKsg53y9"

FirebaseData firebaseData;
FirebaseAuth auth;
FirebaseConfig config;
String path = "/Var";

// Serial mutex
SemaphoreHandle_t serialMutex = nullptr;
static inline void SerialLock()   { if (serialMutex) xSemaphoreTake(serialMutex, portMAX_DELAY); }
static inline void SerialUnlock() { if (serialMutex) xSemaphoreGive(serialMutex); }

// Pins
#define RELAY_PIN     27
#define BUTTON_PIN_1  32   // Pump toggle (MANUAL only)
#define BUTTON_PIN_2  33   // Mode cycle
#define LED_PIN       16

#define SOIL_PIN      34

#define TRIG_PIN      5
#define ECHO_PIN      18
#define WARN_LED_PIN  17

// Pump state
volatile bool relayState = false;

// Button 1 debounce
bool lastButtonState = HIGH;
bool buttonState = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 50;

// Button 2 debounce
bool lastModeBtnState = HIGH;
bool modeBtnState = HIGH;
unsigned long lastModeDebounceTime = 0;
unsigned long modeDebounceDelay = 50;

// Soil
const int samples = 20;
const int sampleDelayMs = 10;
int rawDry = 2650;
int rawWet = 1150;
volatile int soilPercent = 0;

// Volume / low water
#define SOUND_SPEED 0.034
#define CUP_HEIGHT  14.0
#define MAX_H       11.0

volatile bool  lowWater = false;     // volume < 200
volatile float waterVolumeMl = 0.0;

// OLED
Adafruit_SH1106G display(128, 64, &Wire, -1);

// Firebase vars
volatile int fbMode = 0;          // 0=MANUAL, 1=AUTO, 2=SCHEDULE
volatile int fbThreshold = 40;    // AUTO only
volatile int fbPumpSeconds = 10;  // AUTO + SCHEDULE
volatile int fbManualSwitch = 0;  // MANUAL ON/OFF

String fbSchDate = "";            // YYYY-MM-DD
String fbSchTime = "";            // HH:MM

// Timed pump
bool pumpTimedRunning = false;
uint32_t pumpStopAtMs = 0;

// AUTO cooldown
const uint32_t AUTO_COOLDOWN_MS = 30000;
uint32_t autoNextAllowedMs = 0;

// Manual apply tracking
int lastManualSwitchApplied = -1;

// Schedule anti-repeat
String lastScheduleKey = "";

// 2-way sync flags
volatile bool manualSwitchDirty = false;
volatile int  manualSwitchPending = 0;

volatile bool modeDirty = false;
volatile int  modePending = 0;

// Tasks
void waterTask(void *pvParameters);
void warnLedTask(void *pvParameters);
void soilTask(void *pvParameters);
void displayTask(void *pvParameters);
void firebaseTask(void *pvParameters);
void controlTask(void *pvParameters);
void serialTask(void *pvParameters);

// Helpers
int readSoilRawAvg() {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(SOIL_PIN);
    delay(sampleDelayMs);
  }
  return (int)(sum / samples);
}

static inline const char* modeText3(int m) {
  if (m == 0) return "MAN";
  if (m == 1) return "AUT";
  if (m == 2) return "SCH";
  return "N/A";
}

static inline const char* modeTextFull(int m) {
  if (m == 0) return "MANUAL";
  if (m == 1) return "AUTO";
  if (m == 2) return "SCHEDULE";
  return "N/A";
}

void applyRelay(bool on) {
  relayState = on;
  digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
  digitalWrite(LED_PIN,  relayState ? HIGH : LOW);
}

void startTimedPump(int sec) {
  if (sec < 1) sec = 1;
  pumpTimedRunning = true;
  pumpStopAtMs = millis() + (uint32_t)sec * 1000UL;
  applyRelay(true);
}

void stopTimedPump() {
  pumpTimedRunning = false;
  applyRelay(false);
}

void setupTimeNTP() {
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov"); // GMT+7
}

String todayDate() {
  struct tm t;
  if (!getLocalTime(&t, 2000)) return "";
  char buf[16];
  strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
  return String(buf);
}

String nowTimeHM() {
  struct tm t;
  if (!getLocalTime(&t, 2000)) return "";
  char buf[8];
  strftime(buf, sizeof(buf), "%H:%M", &t);
  return String(buf);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  serialMutex = xSemaphoreCreateMutex();

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN_1, INPUT_PULLUP);
  pinMode(BUTTON_PIN_2, INPUT_PULLUP);

  applyRelay(false);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(WARN_LED_PIN, OUTPUT);
  digitalWrite(TRIG_PIN, LOW);
  digitalWrite(WARN_LED_PIN, LOW);

  analogReadResolution(12);
  analogSetPinAttenuation(SOIL_PIN, ADC_11db);

  Wire.begin(21, 22);
  if (!display.begin(0x3C, true)) {
    while (1) delay(10);
  }

  // WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    SerialLock(); Serial.print("."); SerialUnlock();
  }
  SerialLock(); Serial.println("\nWiFi connected."); SerialUnlock();

  setupTimeNTP();

  // Firebase
  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Tasks
  xTaskCreate(waterTask,    "Water",    3000, NULL, 1, NULL);
  xTaskCreate(warnLedTask,  "WarnLED",  1000, NULL, 1, NULL);
  xTaskCreate(soilTask,     "Soil",     2500, NULL, 1, NULL);
  xTaskCreate(displayTask,  "OLED",     2500, NULL, 1, NULL);
  xTaskCreate(firebaseTask, "Firebase", 8192, NULL, 1, NULL);
  xTaskCreate(controlTask,  "Control",  4096, NULL, 1, NULL);
  xTaskCreate(serialTask,   "Serial",   3072, NULL, 1, NULL);
}

void loop() {
  // Mode button: cycle 0->1->2->0 on RELEASE
  bool modeReading = digitalRead(BUTTON_PIN_2);
  if (modeReading != lastModeBtnState) lastModeDebounceTime = millis();

  if (millis() - lastModeDebounceTime > modeDebounceDelay) {
    if (modeReading != modeBtnState) {
      modeBtnState = modeReading;
      if (modeBtnState == HIGH) {
        int newMode = (fbMode + 1) % 3;
        fbMode = newMode;
        modePending = newMode;
        modeDirty = true;

        SerialLock();
        Serial.println("----");
        Serial.print("Mode pending = ");
        Serial.println(modeTextFull(newMode));
        SerialUnlock();
      }
    }
  }
  lastModeBtnState = modeReading;

  // Pump button: MANUAL only + not lowWater, toggle on RELEASE
  if (fbMode == 0 && !lowWater) {
    bool reading = digitalRead(BUTTON_PIN_1);

    if (reading != lastButtonState) lastDebounceTime = millis();

    if (millis() - lastDebounceTime > debounceDelay) {
      if (reading != buttonState) {
        buttonState = reading;
        if (buttonState == HIGH) {
          bool newState = !relayState;
          applyRelay(newState);

          fbManualSwitch = newState ? 1 : 0;
          manualSwitchPending = fbManualSwitch;
          manualSwitchDirty = true;

          SerialLock();
          Serial.println("----");
          Serial.print("ManualSwitch pending = ");
          Serial.println(manualSwitchPending);
          SerialUnlock();
        }
      }
    }
    lastButtonState = reading;
  } else {
    // prevent ghost trigger when returning to MANUAL
    bool r = digitalRead(BUTTON_PIN_1);
    lastButtonState = r;
    buttonState = r;
  }
}

// Water (volume + lowWater)
void waterTask(void *pvParameters) {
  float volumeMax = (1.0 / 3.0) * PI * MAX_H *
                    (sq(3.0) + 3.0 * 5.5 + sq(5.5));

  while (1) {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    long duration = pulseIn(ECHO_PIN, HIGH, 30000);
    if (duration == 0) {
      vTaskDelay(300 / portTICK_PERIOD_MS);
      continue;
    }

    float distanceCm = duration * SOUND_SPEED / 2.0;

    float h = CUP_HEIGHT - distanceCm;
    if (h < 0) h = 0;
    if (h > MAX_H) h = MAX_H;

    float radius = 3.0 + (5.5 - 3.0) * (h / MAX_H);

    float volume = (1.0 / 3.0) * PI * h *
                   (sq(3.0) + 3.0 * radius + sq(radius));
    volume = volume * (800.0 / volumeMax);

    waterVolumeMl = volume;
    lowWater = (volume < 200);

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// Warn LED
void warnLedTask(void *pvParameters) {
  while (1) {
    if (lowWater) {
      digitalWrite(WARN_LED_PIN, HIGH);
      vTaskDelay(500 / portTICK_PERIOD_MS);
      digitalWrite(WARN_LED_PIN, LOW);
      vTaskDelay(500 / portTICK_PERIOD_MS);
    } else {
      digitalWrite(WARN_LED_PIN, LOW);
      vTaskDelay(200 / portTICK_PERIOD_MS);
    }
  }
}

// Soil (%)
void soilTask(void *pvParameters) {
  while (1) {
    int raw = readSoilRawAvg();
    int moisturePercent = map(raw, rawDry, rawWet, 0, 100);
    moisturePercent = constrain(moisturePercent, 0, 100);
    soilPercent = moisturePercent;

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// OLED
void displayTask(void *pvParameters) {
  while (1) {
    int sp   = soilPercent;
    int th   = fbThreshold;
    int mode = fbMode;
    int tsec = fbPumpSeconds;      // NEW: show pump time
    float vol = waterVolumeMl;

    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);

    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("Soil:");
    display.setTextSize(2);
    display.setCursor(42, 0);
    display.print(sp);
    display.print("%");

    display.setTextSize(1);
    display.setCursor(0, 24);
    display.print("Water:");
    display.setTextSize(2);
    display.setCursor(54, 24);
    display.print((int)vol);
    display.print("ml");

    // Last line fixed + NEW T:
    display.setTextSize(1);
    display.setCursor(0, 56);
    display.print("Th:");
    display.print(th);
    display.print("% M:");
    display.print(modeText3(mode));
    display.print(" T:");
    display.print(tsec);
    display.print("s");

    display.display();
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

// Firebase
void firebaseTask(void *pvParameters) {
  uint32_t lastWrite = 0;
  uint32_t lastRead  = 0;

  while (1) {
    if (Firebase.ready()) {

      if (manualSwitchDirty) {
        int v = manualSwitchPending;
        if (Firebase.setInt(firebaseData, path + "/ManualSwitch", v)) {
          manualSwitchDirty = false;
          fbManualSwitch = v;
        }
      }

      if (modeDirty) {
        int m = modePending;
        if (Firebase.setInt(firebaseData, path + "/Mode", m)) {
          modeDirty = false;
        }
      }

      if (millis() - lastWrite >= 5000) {
        lastWrite = millis();
        Firebase.setInt(firebaseData, path + "/Soid", (int)soilPercent);
        Firebase.setFloat(firebaseData, path + "/Volume", (float)waterVolumeMl);
      }

      if (millis() - lastRead >= 2000) {
        lastRead = millis();

        if (!modeDirty) {
          if (Firebase.getInt(firebaseData, path + "/Mode")) fbMode = firebaseData.intData();
        }

        if (Firebase.getInt(firebaseData, path + "/Threshold")) fbThreshold = firebaseData.intData();
        if (Firebase.getInt(firebaseData, path + "/PumpSeconds")) fbPumpSeconds = firebaseData.intData();

        if (!manualSwitchDirty) {
          if (Firebase.getInt(firebaseData, path + "/ManualSwitch")) fbManualSwitch = firebaseData.intData();
        }

        if (Firebase.getString(firebaseData, path + "/Schedule/Date")) fbSchDate = firebaseData.stringData();
        if (Firebase.getString(firebaseData, path + "/Schedule/Time")) fbSchTime = firebaseData.stringData();
      }
    }

    vTaskDelay(200 / portTICK_PERIOD_MS);
  }
}

// Control
void controlTask(void *pvParameters) {
  while (1) {
    int mode = fbMode;

    if (lowWater) {
      if (pumpTimedRunning) stopTimedPump();
      if (relayState) applyRelay(false);

      if (fbManualSwitch != 0) {
        fbManualSwitch = 0;
        manualSwitchPending = 0;
        manualSwitchDirty = true;
      }

      vTaskDelay(200 / portTICK_PERIOD_MS);
      continue;
    }

    if (pumpTimedRunning && millis() >= pumpStopAtMs) {
      stopTimedPump();
      if (mode == 1) autoNextAllowedMs = millis() + AUTO_COOLDOWN_MS;
    }

    if (mode == 0) {
      if (fbManualSwitch != lastManualSwitchApplied) {
        lastManualSwitchApplied = fbManualSwitch;
        applyRelay(fbManualSwitch == 1);
      }

      if (pumpTimedRunning) stopTimedPump();
      autoNextAllowedMs = 0;
    }
    else if (mode == 1) {
      lastManualSwitchApplied = -1;

      int soil = soilPercent;

      if (soil >= fbThreshold) {
        if (!pumpTimedRunning && relayState) applyRelay(false);
        autoNextAllowedMs = 0;
      } else {
        if (!pumpTimedRunning && (autoNextAllowedMs == 0 || millis() >= autoNextAllowedMs)) {
          startTimedPump(fbPumpSeconds);
        }
      }
    }
    else if (mode == 2) {
      lastManualSwitchApplied = -1;
      autoNextAllowedMs = 0;

      if (!pumpTimedRunning && fbSchDate.length() == 10 && fbSchTime.length() == 5) {
        String d = todayDate();
        String t = nowTimeHM();

        if (d == fbSchDate && t == fbSchTime) {
          String key = d + " " + t;
          if (key != lastScheduleKey) {
            lastScheduleKey = key;
            startTimedPump(fbPumpSeconds);
          }
        }
      }

      if (!pumpTimedRunning && relayState) applyRelay(false);
    }

    vTaskDelay(200 / portTICK_PERIOD_MS);
  }
}

// Serial (every 5s)
void serialTask(void *pvParameters) {
  while (1) {
    int soil = soilPercent;
    int vol  = (int)waterVolumeMl;
    int th   = fbThreshold;
    int mode = fbMode;

    SerialLock();
    Serial.println("----");
    Serial.print("Soid: ");
    Serial.println(soil);
    Serial.print("Volume: ");
    Serial.println(vol);
    Serial.print("Threshold: ");
    Serial.println(th);
    Serial.print("Mode: ");
    Serial.println(modeTextFull(mode));
    SerialUnlock();

    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
}